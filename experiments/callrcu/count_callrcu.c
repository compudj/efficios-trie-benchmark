#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
struct rcu_head;
#define NF 64
static void *fn[NF]; static unsigned long cnt[NF];
void urcu_qsbr_call_rcu(struct rcu_head *h, void (*f)(struct rcu_head *))
{
	static void (*real)(struct rcu_head *, void (*)(struct rcu_head *));
	int i;
	if (!real) real = dlsym(RTLD_NEXT, "urcu_qsbr_call_rcu");
	for (i = 0; i < NF; i++) {
		void *cur = __atomic_load_n(&fn[i], __ATOMIC_RELAXED);
		if (cur == (void *) f) break;
		if (!cur) { void *z = NULL;
			if (__atomic_compare_exchange_n(&fn[i], &z, (void *) f, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) || z == (void *) f) break; }
	}
	if (i < NF) __atomic_add_fetch(&cnt[i], 1, __ATOMIC_RELAXED);
	real(h, f);
}
__attribute__((destructor)) static void report(void)
{
	int i; Dl_info di;
	for (i = 0; i < NF && fn[i]; i++)
		fprintf(stderr, "callrcu: %-32s %lu\n",
			dladdr(fn[i], &di) && di.dli_sname ? di.dli_sname : "?", cnt[i]);
}
