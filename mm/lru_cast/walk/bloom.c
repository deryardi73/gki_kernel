// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- the bloom of PMDs reclaim found young (Marie's).
 *
 * Where the walker cannot ask the hardware whether anything under a PMD
 * was used -- no accessed bit kept in a non-leaf PMD, or an mm with a
 * secondary MMU (KVM), whose accesses do not walk these page tables --
 * it asks this instead: the PMDs in which reclaim's look-around
 * (lookaround.c) found a young folio since the walk before. It is a list
 * of PMDs reclaim has already reached, so it is the fallback, not the
 * gate: a region gets in only once reclaim has visited it.
 *
 * Two filters, keyed by (mm, PMD), 1 << 15 bits and two hashes each: the
 * look-around sets bits in one while the walker reads the other, and at
 * the end of each walk they trade places and the one to fill is cleared.
 * Lock-free; a bit lost to a swap racing a set costs one PMD a walk.
 */
#include <linux/bitmap.h>
#include <linux/hash.h>
#include "walk/young.h"

#define LRU_CAST_BLOOM_SHIFT	15
#define LRU_CAST_BLOOM_BITS		(1U << LRU_CAST_BLOOM_SHIFT)

static DECLARE_BITMAP(lru_cast_bloom_filter[2], LRU_CAST_BLOOM_BITS);
static int lru_cast_bloom_read;	/* the walker's filter; the other is filled */

static void lru_cast_bloom_keys(struct mm_struct *mm, unsigned long addr, unsigned int *key)
{
	u64 h = hash_64((addr >> PMD_SHIFT) ^ ((u64)(unsigned long)mm >> 6) * GOLDEN_RATIO_64,
			2 * LRU_CAST_BLOOM_SHIFT);

	key[0] = h & (LRU_CAST_BLOOM_BITS - 1);
	key[1] = (h >> LRU_CAST_BLOOM_SHIFT) & (LRU_CAST_BLOOM_BITS - 1);
}

void lru_cast_bloom_set(struct mm_struct *mm, unsigned long addr)
{
	unsigned long *f = lru_cast_bloom_filter[!READ_ONCE(lru_cast_bloom_read)];
	unsigned int key[2];

	lru_cast_bloom_keys(mm, addr, key);
	if (!test_bit(key[0], f))
		set_bit(key[0], f);
	if (!test_bit(key[1], f))
		set_bit(key[1], f);
}

bool lru_cast_bloom_test(struct mm_struct *mm, unsigned long addr)
{
	unsigned long *f = lru_cast_bloom_filter[READ_ONCE(lru_cast_bloom_read)];
	unsigned int key[2];

	lru_cast_bloom_keys(mm, addr, key);
	return test_bit(key[0], f) && test_bit(key[1], f);
}

/* a walk done: it reads next what reclaim found since; the old one fills afresh */
void lru_cast_bloom_swap(void)
{
	int r = !READ_ONCE(lru_cast_bloom_read);

	WRITE_ONCE(lru_cast_bloom_read, r);
	bitmap_zero(lru_cast_bloom_filter[!r], LRU_CAST_BLOOM_BITS);
}
