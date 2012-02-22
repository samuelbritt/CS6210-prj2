#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include "file_service.h"
#include "common.h"
#include "stlist.h"

/* ring ptr and shm_name pair for server worker threads*/
struct ring_name {
        struct fs_process_sring* ring;
        char* shm_name;
};

/* ring ptr and shm_name pair for registrar thread */
struct reg_ring_name {
        struct fs_registrar_sring* ring;
        char* shm_name;
};

/* arg to pass to the worker server threads */
struct worker_arg {
	struct ring_name rData;
	struct stlist_node *ll_node;
};

/* global circular linked list */
struct stlist server_list;

/* set to 1 when we must exit */
volatile sig_atomic_t done = 0;

/* Sig handler for exit signal */
static void exit_handler(int signo)
{
	done = 1;
}

static void pidfile_create(char *pidfile_name)
{
	FILE *pidfile;
	if (!(pidfile = fopen(pidfile_name, "w")))
	    fail_en("fopen");
	fprintf(pidfile, "%d\n", getpid());
	fclose(pidfile);
}

static void pidfile_destroy(char *pidfile_name)
{
	remove(pidfile_name);
}

static void install_sig_handler(int signo, void (*handler)(int))
{
	struct sigaction sa;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = handler;
	if (sigaction(signo, &sa, NULL) == -1)
		fail_en("sigaction");
}

/* Puts application into a daemon state. Keeps the working directory where
 * it is */
static void daemonize()
{
	bool change_cwd_to_root = False;
	bool close_stdout = False; // maybe set to True once debugged
	if (daemon(!change_cwd_to_root, !close_stdout))
		fail_en("daemon");
}

/* Loop around, finding the first node that has work to be done */
static struct stlist_node *find_work(struct stlist_node *p)
{
	int has_work = False;
	sem_wait(&server_list.mtx);
	do {
		p = p->next;
		pthread_mutex_lock(&p->mtx);
		has_work = p->has_work;
		pthread_mutex_unlock(&p->mtx);
	} while (!has_work);
	sem_post(&server_list.mtx);
	return p;
}

/* Fill a buffer with a sector */
static void fill_sector_data(int sector, char *buf)
{
	sprintf(buf, "Sector %d: test", sector);
}

/* Main file server thread. Continually waits for work to be put in the circular
 * linked list of worker threads. When there is work to be done, it loops around
 * the list taking each job in round-robin fasion. It performs each job and
 * signals to the worker thread that it is done.
 */
static void file_server()
{
	sem_wait(&server_list.mtx);
	struct stlist_node *p = server_list.first;
	sem_post(&server_list.mtx);
	while (!done) {
		if (sem_wait(&server_list.full) == -1) {
			int en = errno;
			if (en == EINTR) {
				continue;
			} else {
				fail_en("sem_wait");
			}
		}
		p = find_work(p);
		int sector = p->entry->req;
		fill_sector_data(sector, p->entry->rsp.data);
		pthread_mutex_lock(&p->mtx);
		p->has_work = False;
		pthread_cond_signal(&p->cond);
		pthread_mutex_unlock(&p->mtx);
	}
}

/* Handler for the worker thread servers. Note that the fs_process_sring_entry
 * is locked by a mutex through the duration of this call */
static void data_lookup_handle(union fs_process_sring_entry *entry,
			       struct stlist_node *ll_node)
{
	/* We let the file server thread we have work to do, and wait till it
	 * finishes */
	ll_node->entry = entry;
	pthread_mutex_lock(&ll_node->mtx);
	ll_node->has_work = True;
	sem_post(&server_list.full);
	while (!ll_node->has_work)
		pthread_cond_wait(&ll_node->cond, &ll_node->mtx);
	pthread_mutex_unlock(&ll_node->mtx);
}

/* Cleanup functions for ring buffers. This will be called from a cancellation
 * point, like sem_wait. Will eventually cause this thread to exit, and it can
 * be join()ed */
static void fs_registrar_ring_cleanup(void *arg)
{
        struct reg_ring_name *rname = arg;
	shm_destroy(rname->shm_name, rname->ring, sizeof(*rname->ring));
}

static void fs_process_ring_cleanup(void *arg_)
{
        struct worker_arg *arg = arg_;
        struct ring_name *rname = &arg->rData;
	shm_destroy(rname->shm_name, rname->ring, sizeof(*rname->ring));
	free(arg);
}

static void *start_worker(void* arg_)
{
        struct worker_arg *arg = arg_;
	struct ring_name *rData = &arg->rData;
        struct fs_process_sring *reg = rData->ring;
	struct stlist_node *ll_node = arg->ll_node;

	printf("worker thread\n");

	pthread_cleanup_push(&fs_process_ring_cleanup, arg);
	RB_SERVE(fs_process, reg, done, &data_lookup_handle, ll_node);
	pthread_cleanup_pop(1); // 1 => execute cleanup unconditionally

	return 0;
}

/* Handles a single request/response for client registration. Takes in the
 * client pid in entry->req, and pushes the sector limits for the served file in
 * entry->rsp. */
static void reg_handle_request(union fs_registrar_sring_entry *entry, void *nil)
{
	// process_request
	int client_pid = entry->req;
	printf("server: client request %d\n", client_pid);

	//create new ring
        char *shmWorkerName = shm_ring_buffer_name(client_pid);
	struct fs_process_sring *reg = shm_create(shmWorkerName, sizeof(*reg));
	RB_INIT(fs_process, reg, FS_PROCESS_SLOT_COUNT);

	//create new linked list node
	struct stlist_node *n = stlist_node_create();
	stlist_insert(&server_list, n);

	//spawn new worker thread)
	struct worker_arg *arg = emalloc(sizeof(*arg));
	arg->rData.ring = reg;
	arg->rData.shm_name = shmWorkerName;
	arg->ll_node = n;
	pthread_create(&n->tid, NULL, start_worker, arg);

	printf("thread created. worker shm %s\n", shmWorkerName);

	// push_response
	entry->rsp.start = -1 * client_pid;
	entry->rsp.end =  client_pid;
}

/* starts the infinite loop for the client registrar */
static void *registrar(void *arg)
{
	/* Create the internal circular linked list for worker threads */
	stlist_init(&server_list);

	/* Create the registration ring buffer for clients */
	char *shm_name = shm_registrar_name;
	struct fs_registrar_sring *reg = shm_create(shm_name,
						    sizeof(*reg));
	RB_INIT(fs_registrar, reg, FS_REGISTRAR_SLOT_COUNT);

	struct reg_ring_name rname = {
		.ring = reg,
		.shm_name = shm_name
	};
	pthread_cleanup_push(&fs_registrar_ring_cleanup, &rname);
	RB_SERVE(fs_registrar, reg, done, &reg_handle_request, NULL);
	pthread_cleanup_pop(1); // 1 => execute cleanup unconditionally
	return NULL;
}

static pthread_t start_registrar()
{
	pthread_t reg;
	pthread_create(&reg, NULL, &registrar, NULL);
	return reg;
}

static void kill_registrar(pthread_t reg)
{
	pthread_cancel(reg);
	pthread_join(reg, NULL);
}

/* Kills all the worker threads in the linked list and waits for them finish */
static void kill_worker_threads(struct stlist *list)
{
	if (stlist_is_empty(list))
		return;

	struct stlist_node *p = list->first;
	do {
		pthread_cancel(p->tid);
		p = p->next;
	} while (p != list->first);

	do {
		pthread_join(p->tid, NULL);
		p = p->next;
	} while (p != list->first);
}

int main(int argc, char *argv[])
{
	if (argc < 2)
		fail("Must provide a pidfile path");
	daemonize();

	char *pidfile_path = argv[1];
	pidfile_create(pidfile_path);

	/* block all signals */
	sigset_t sigset, oldset;
	sigfillset(&sigset);
	pthread_sigmask(SIG_SETMASK, &sigset, &oldset);

	/* start the registrar */
	pthread_t reg = start_registrar();

	/* Unblock "done" signal */
	pthread_sigmask(SIG_SETMASK, &oldset, NULL);
	install_sig_handler(SIGTERM, &exit_handler);

	/* Start the file server */
	file_server();

	/* Kill all the threads */
	kill_registrar(reg);
	kill_worker_threads(&server_list);

	pidfile_destroy(pidfile_path);
	return 0;
}
