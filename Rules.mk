
CC 	= gcc
CFLAGS	= -pedantic -Wall -std=gnu99 -pthread -O2 $(DEBUG)
INCLUDES =
LDFLAGS = -pthread
LDLIBS	= -lrt -lm
DBFLAGS = -g -O0 -DDEBUG

# Generic rules:
LINK	= $(LINK.c) -o $@ $^ $(LDLIBS)
COMP	= $(COMPILE.c) $(INCLUDES) -MMD -MP $<

