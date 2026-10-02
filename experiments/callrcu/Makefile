# experiments/callrcu/Makefile -- call_rcu() caller-side cost microbench.
#
# Builds one binary per liburcu build (the enqueue path is the library's, so
# the binary is the same source linked differently):
#
#   make                                   # against urcu-txn-build/
#   make URCU_TXN_BUILD=../../urcu-txn-build-cbrseq BIN=bench_call_rcu_cbrseq
#
# A build configured --enable-call-rcu-rseq links its own librseq; the -L/rpath
# are taken from that build's src/Makefile, as experiments/dcache/Makefile does.

URCU_TXN_BUILD ?= $(abspath ../../urcu-txn-build)
BIN            ?= bench_call_rcu

comma := ,
CC       ?= gcc
CFLAGS   := -O2 -g -pthread -Wall -Wextra -march=native
CPPFLAGS := -I$(URCU_TXN_BUILD)/include
RSEQ_L   := $(shell grep -shoE '\-L[^ ]*librseq[^ ]*' $(URCU_TXN_BUILD)/src/Makefile | head -1)
LDFLAGS  := -L$(URCU_TXN_BUILD)/src/.libs -Wl,-rpath,$(URCU_TXN_BUILD)/src/.libs \
	    $(RSEQ_L) $(patsubst -L%,-Wl$(comma)-rpath$(comma)%,$(RSEQ_L))
LDLIBS   := -lurcu-qsbr -lurcu-common -lpthread

.PHONY: all clean

all: $(BIN)

$(BIN): bench_call_rcu.c $(URCU_TXN_BUILD)/src/.libs/liburcu-qsbr.so
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
	rm -f bench_call_rcu bench_call_rcu_*
