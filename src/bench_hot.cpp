/*
 * C++ shim exposing HOT (Height Optimized Trie, speedskater/hot, ISC) to the
 * C benchmark bench_one_st.c via a small extern "C" API.
 *
 * HOT is header-only C++14 and requires AVX2 + BMI2 (Haswell / Zen+).  We use
 * the single-threaded variant with the value type being the key string itself
 * (IdentityKeyExtractor over `const char *`), so HOT keys on the NUL-terminated
 * string via its CStringComparator — matching how bench_one_st stores DNS keys.
 */
#include <hot/singlethreaded/HOTSingleThreaded.hpp>	/* full impl, not just Interface */
#include <idx/contenthelpers/IdentityKeyExtractor.hpp>
#include <idx/contenthelpers/OptionalValue.hpp>

#include <cstddef>

using HotTrie = hot::singlethreaded::HOTSingleThreaded<
	const char *, idx::contenthelpers::IdentityKeyExtractor>;

extern "C" {

void *hot_create(void)
{
	return new HotTrie();
}

void hot_destroy(void *h)
{
	delete static_cast<HotTrie *>(h);
}

/*
 * Insert a key; the stored value is the passed pointer.  The caller
 * (bench_one_st run_hot) MUST pass a pointer to a key COPY it owns (a dense
 * cds_ft_external_arena slot), not a pointer into the shared query buffer:
 * HOT's value is its key, so storing the query buffer would let the lookup's
 * contentEquals (and FORCE_READ_LEAF) validate against already-hot memory --
 * near-free, and unfair vs every other engine, which stores the key in its own
 * (cold, dense-arena or internal) nodes.  Keeping the copy caller-side puts
 * HOT on the exact same arena/layout as the FT/qp/ART engines.
 */
int hot_insert(void *h, const char *key)
{
	return static_cast<HotTrie *>(h)->insert(key) ? 1 : 0;
}

/* Look up a key; return the stored value pointer (key string) or NULL. */
const char *hot_lookup(void *h, const char *key)
{
	idx::contenthelpers::OptionalValue<const char *> r =
		static_cast<HotTrie *>(h)->lookup(key);
	return r.mIsValid ? r.mValue : nullptr;
}

} /* extern "C" */
