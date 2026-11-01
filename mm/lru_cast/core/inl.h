/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CAST -- the inline forms of core's functions that policy/ calls in its
 * hot loops, for a sweep reads every word of a block. The code is core's,
 * and so are the tables and the array it reads: policy/ includes this
 * header and never the ones inside core/ (store.h).
 */
#ifndef _MM_LRU_CAST_CORE_INL_H
#define _MM_LRU_CAST_CORE_INL_H

#include "core/core.h"

/*
 * The tables of lru_cast_key_res() (core/key.c), for its inline form below:
 * a sweep calls it for every word it reads. The weights of a word's reads at
 * each decay, its gap codes read as an index: [0] the newest's, 1, [1 + g1]
 * the one before's, [9 + 8 g1 + g2] the one before that's.
 */
#define LRU_CAST_WTAB		(1 + 8 + 64)
extern u32 lru_cast_wtab[LRU_CAST_LE_MAX + 1][LRU_CAST_WTAB];
extern const u8 lru_cast_log2m[256];

/* tau / H in Q16 half-lives per tick: 2^(le - 10) */
static inline u64 lru_cast_dq(int le)
{
	return le ? 1ULL << (le + 6) : 0;
}

/* 256 log2(v / 2^16) */
static __always_inline s64 lru_cast_log2q8(u64 v)
{
	int msb = fls64(v) - 1;
	u32 m = (msb >= 8 ? v >> (msb - 8) : v << (8 - msb)) & 255;

	return ((s64)msb - 16) * 256 + lru_cast_log2m[m];
}

/* the bucket of a state word: 2^-@res half-lives of the key, or n */
static __always_inline long lru_cast_key_res_inl(u32 w, unsigned long tick, int le, int res)
{
	u32 n = w >> 24, g1 = (w >> LRU_CAST_W_GAP) & 7, g2 = (w >> (LRU_CAST_W_GAP + 3)) & 7;
	u8 age0 = n ? lru_cast_age(w, tick) : 0;
	unsigned long last = tick >= age0 ? tick - age0 : 0;
	const u32 *t;
	u64 s, wt;

	if (!le)
		return n;
	if (!n)
		return LONG_MIN;
	/*
	 * Stale (core.h), the reads all at the last, taken at the start of its
	 * unit: as fixed as any other key. Deep, older than any: the count alone.
	 */
	if (lru_cast_stale(w)) {
		s64 lift = lru_cast_log2q8((u64)n << 16);

		if (lru_cast_deep(w))
			return lift >> (8 - res);
		last = lru_cast_stale_last(w, tick);
		return ((s64)((last * lru_cast_dq(le)) >> 8) + lift) >> (8 - res);
	}
	/*
	 * The key is the same taken at any time; taken at the last read, its
	 * weight is 1 and the sum cannot underflow however long ago that was.
	 * The reads before it are the middles of the gaps their codes allow
	 * (lru_cast_gap2()), their weights looked up.
	 */
	/*
	 * Read once, the weights are the newest's alone, 1 (65536): the log of
	 * it is 0, and nothing needs looking up. Most words in a sweep are.
	 */
	if (n == 1)
		return (s64)((last * lru_cast_dq(le)) >> 8) >> (8 - res);
	t = lru_cast_wtab[min(le, LRU_CAST_LE_MAX)];
	s = wt = t[0];
	if (n >= 2) {
		wt = t[1 + g1];
		s += wt;
	}
	if (n >= 3) {
		wt = t[9 + 8 * g1 + g2];
		s += wt;
	}
	if (n > 3)
		s += (u64)(n - 3) * wt;		/* older reads count at the oldest stamp */
	return ((s64)((last * lru_cast_dq(le)) >> 8) + lru_cast_log2q8(s)) >> (8 - res);
}

#include "core/store.h"

/*
 * The first PFN in [@pfn, @end) whose owner tag is @owner, its state word
 * in *@w; @end if there is none. A section without words is passed over.
 */
static __always_inline unsigned long lru_cast_find_owner_inl(unsigned long pfn, unsigned long end,
							    u32 owner, u32 *w)
{
	owner <<= LRU_CAST_W_OWNER;
	while (pfn < end) {
		unsigned long sec_end = min(end, section_nr_to_pfn(pfn_to_section_nr(pfn) + 1));
		u32 *p = lru_cast_word(pfn);

		if (!p) {
			pfn = sec_end;
			continue;
		}
		for (; pfn < sec_end; pfn++, p++) {
			u32 v = READ_ONCE(*p);

			if ((v & LRU_CAST_OWNER_MASK) == owner) {
				*w = v & ~LRU_CAST_OWNER_MASK;
				return pfn;
			}
		}
	}
	return end;
}

#endif /* _MM_LRU_CAST_CORE_INL_H */
