// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * dcache_bench_rand.h -- the one PRNG every dcache harness draws from.
 *
 * xorshift64* (Vigna): the xorshift64 state step, then a multiply on output.
 * The multiply is what matters.  Plain xorshift64 is LINEAR over GF(2) and the
 * low bits of consecutive outputs are shifted copies of each other: with the
 * (13, 7, 17) triple the harnesses used, low7(x[n+1]) = x[n][0..6] ^
 * x[n][7..13].  A reader drew a name `x[n] % total` and then its directory
 * `x[n+1] % ndirs`; with `total` a power of two the name fixed 5 of the
 * directory's 7 bits, so each name was only ever looked up in 4 of 128
 * directories.  That is a 32x smaller reader working set at 128 threads, a
 * partial one at 192 (total = 3 * 2^11), almost none at 168 -- a lookup curve
 * that jumped 2-3x with the thread count for no engine reason, and vacated
 * names nobody looked at.
 *
 * xrange() maps the HIGH 32 bits onto [0, n) by multiply-shift (Lemire): the
 * product's high bits are the well-mixed ones, and no 64-bit divide sits on a
 * reader's hot loop.  Its bias is below n / 2^32, far under anything measured.
 * xtop() takes the top @bits bits, for a power-of-two die.
 */
#ifndef DCACHE_BENCH_RAND_H
#define DCACHE_BENCH_RAND_H

#include <stdint.h>

/* @s must be seeded nonzero. */
static inline uint64_t xrand(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*s = x;
	return x * 0x2545f4914f6cdd1dULL;
}

/* Uniform in [0, n), for 0 < n < 2^32. */
static inline uint32_t xrange(uint64_t *s, uint32_t n)
{
	return (uint32_t) (((xrand(s) >> 32) * (uint64_t) n) >> 32);
}

/* Uniform in [0, 2^bits), for 0 < bits <= 32. */
static inline uint32_t xtop(uint64_t *s, unsigned int bits)
{
	return (uint32_t) (xrand(s) >> (64 - bits));
}

#endif /* DCACHE_BENCH_RAND_H */
