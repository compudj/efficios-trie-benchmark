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
#include <cstdint>

using HotTrie = hot::singlethreaded::HOTSingleThreaded<
	const char *, idx::contenthelpers::IdentityKeyExtractor>;

/*
 * Integer HOT (the u32/u64 datasets): map-mode, value = pointer to the caller's
 * key record (a uint64_t the caller owns), key extracted by dereferencing it.
 * Storing a pointer (not the raw uint64) keeps HOT on the same footing as the
 * other integer engines: the lookup returns a cold record pointer that
 * bench_one_st force-reads, instead of HOT's cheaper set-mode where the value
 * IS the key (no separate value load).
 */
template <typename ValueType>		/* ValueType == uint64_t* */
struct HotU64PtrExtractor {
	typedef uint64_t KeyType;
	inline KeyType operator()(ValueType const &v) const { return *v; }
};
using HotIntTrie = hot::singlethreaded::HOTSingleThreaded<
	uint64_t *, HotU64PtrExtractor>;

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

/* Integer HOT (u32/u64).  @rec is a pointer to the caller's uint64_t key. */
void *hot_u64_create(void)
{
	return new HotIntTrie();
}

void hot_u64_destroy(void *h)
{
	delete static_cast<HotIntTrie *>(h);
}

int hot_u64_insert(void *h, uint64_t *rec)
{
	return static_cast<HotIntTrie *>(h)->insert(rec) ? 1 : 0;
}

/* Return the stored record pointer (cold) or NULL. */
const void *hot_u64_lookup(void *h, uint64_t key)
{
	idx::contenthelpers::OptionalValue<uint64_t *> r =
		static_cast<HotIntTrie *>(h)->lookup(key);
	return r.mIsValid ? r.mValue : nullptr;
}

} /* extern "C" */
