# SPDX-License-Identifier: GPL-2.0
# Vendored Linux kernel rw_semaphore (rwsem.c) + userspace shim, as a small lib.
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -march=native
# rwsem.c is kernel source; silence a few kernel-idiom warnings there only.
RWSEM_CFLAGS = $(CFLAGS) -Wno-unused-function -Wno-unused-parameter \
	       -Wno-pointer-sign -Wno-unused-but-set-variable

OBJS = rwsem.o kcompat.o krwsem.o

libkrwsem.a: $(OBJS)
	ar rcs $@ $(OBJS)

rwsem.o: rwsem.c kcompat.h
	$(CC) $(RWSEM_CFLAGS) -c -o $@ $<
kcompat.o: kcompat.c kcompat.h
	$(CC) $(CFLAGS) -c -o $@ $<
krwsem.o: krwsem.c kcompat.h krwsem.h
	$(CC) $(CFLAGS) -c -o $@ $<

test_krwsem: test_krwsem.c libkrwsem.a krwsem.h
	$(CC) $(CFLAGS) -o $@ test_krwsem.c libkrwsem.a -lpthread

check: test_krwsem
	./test_krwsem 4 8
	./test_krwsem 8 32
	./test_krwsem 1 16

clean:
	rm -f $(OBJS) libkrwsem.a test_krwsem
