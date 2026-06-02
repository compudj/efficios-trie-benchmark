/*
 * C shim exposing the Cuckoo Trie (cuckoo-trie/cuckoo-trie-code, Unlicense /
 * public domain) to bench_one_st.c.  Kept in its own TU so Cuckoo's headers
 * (config.h / compiler.h / ...) don't collide with bench_one_st's FT/urcu
 * includes.
 *
 * Cuckoo references (does not copy) the ct_kv objects we insert, so they are
 * allocated here and intentionally never freed — fine for a one-shot
 * single-process benchmark (RSS still accounts for them).
 *
 * NOTE: third_party/cuckoo-trie/util.c carries a local change so it falls back
 * to a regular mmap + MADV_HUGEPAGE when reserved 2 MiB hugepages are absent
 * (see that file); upstream Cuckoo requires reserved hugepages.
 */
#include "cuckoo_trie.h"	/* pulls key_object.h: ct_kv + kv_* helpers, S_OK */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *cuckoo_create(unsigned long num_keys)
{
	/* 2.5 cells/key, matching the upstream benchmark's TRIE_CELLS_AUTO. */
	return ct_alloc(num_keys * 5 / 2);
}

/* Insert a key (zero-length value).  Returns 0 on success (or already-present),
 * else the Cuckoo status code. */
int cuckoo_insert(void *t, const void *key, unsigned int len)
{
	ct_kv *kv = (ct_kv *)malloc(kv_required_size(len, 0));
	int ret;

	if (!kv)
		return -1;
	kv_init(kv, len, 0);
	memcpy(kv_key_bytes(kv), key, len);
	ret = ct_insert((cuckoo_trie *)t, kv);
	if (ret == S_OK || ret == S_ALREADYIN)
		return 0;	/* on S_OK the trie keeps kv; ALREADYIN leaks it */
	free(kv);
	return ret;
}

/* Look up a key; return the stored ct_kv* (touchable) or NULL on miss. */
const void *cuckoo_lookup(void *t, const void *key, unsigned int len)
{
	return ct_lookup((cuckoo_trie *)t, len, (uint8_t *)key);
}
