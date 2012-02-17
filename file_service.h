/*
 * This header is included by both the server and the client. It should define
 * the interface between the two, and not contain anything private to either
 * module
 */

#ifndef FILE_SERVICE_H_
#define FILE_SERVICE_H_

#include <semaphore.h>

/* Name of the shared memory file that clients should `mmap()` to register
 * their pid with the server */
#define shm_registrar_name "/fs_registrar"

/* structs used in the registrar shared memory */
struct fs_registration {
	// mutexes? cond vars? semaphores?
	// valid bits?
	int client_pid;
};

/* number of slots in the ringbuffer */
#define FS_REGISTRAR_SLOT_COUNT 10
struct fs_registrar {
	sem_t empty;
	sem_t full;
	sem_t mtx;
	int client_index;
	struct fs_registration registrar[FS_REGISTRAR_SLOT_COUNT];
};

/* Prefix of the name of the shared memory file that clients should `mmap()` to
 * communicate via ring buffer. The full name will be
 * "shm_ring_buffer_prefix.pid", where `pid` is the PID of the client.
 */
#define shm_ring_buffer_prefix "/fs_ringbuffer"

/* Functions to handle shared memory */
void *shm_create(char *fname, size_t size);
void *shm_map(char *fname, size_t size);
void *shm_destroy(char *fname, void *ptr, size_t size);

#endif /* end of include guard: FILE_SERVICE_H_ */
