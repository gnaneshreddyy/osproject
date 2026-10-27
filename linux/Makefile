CC      = gcc
CFLAGS  = -O2 -Wall -Wextra

all: ctxschedd workload

ctxschedd: daemon.c
	$(CC) $(CFLAGS) -o $@ $< -lm

workload: workload.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f ctxschedd workload

.PHONY: all clean
