/*
 * C++ shim exposing HOT's concurrent ROWEX trie (HOTRowex) to the bind9
 * load-names lookup-scaling benchmark via a small extern "C" API.
 *
 * load-names is C and built by the bind9 meson; this shim is compiled
 * separately by g++ (scripts/build-bind9.sh) and linked into the load-names
 * executable with -ltbb -lstdc++.  HOTRowex keys on NUL-terminated C-strings
 * (IdentityKeyExtractor over const char*); load-names feeds it the dns_qpkey
 * bytes, which are safe to NUL-terminate because every qpkey byte is
 * >= SHIFT_NOBYTE (== 2), so a qpkey never contains an embedded 0x00.
 *
 * The caller owns the key memory: it passes a pointer to a NUL-terminated copy
 * of the qpkey it keeps in its own (NUMA-interleaved) arena, so HOTRowex's
 * value is a pointer to cold, separate memory -- contentEquals validates
 * against that copy, not against the shared query buffer (the same fairness the
 * FT engines get from their external-node arena).
 */
#include <hot/rowex/HOTRowex.hpp>
#include <idx/contenthelpers/IdentityKeyExtractor.hpp>
#include <idx/contenthelpers/OptionalValue.hpp>

using HotRowex =
	hot::rowex::HOTRowex<const char *, idx::contenthelpers::IdentityKeyExtractor>;

extern "C" {

void *hotrowex_create(void)
{
	return new HotRowex();
}

void hotrowex_destroy(void *h)
{
	delete static_cast<HotRowex *>(h);
}

/* @key: NUL-terminated copy the caller owns; stored as HOTRowex's value. */
int hotrowex_insert(void *h, const char *key)
{
	return static_cast<HotRowex *>(h)->insert(key) ? 1 : 0;
}

/* Return the stored value (the caller's key copy) or NULL; validated. */
const char *hotrowex_lookup(void *h, const char *key)
{
	idx::contenthelpers::OptionalValue<const char *> r =
		static_cast<HotRowex *>(h)->lookup(key);
	return r.mIsValid ? r.mValue : nullptr;
}

} /* extern "C" */
