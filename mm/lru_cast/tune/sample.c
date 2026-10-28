// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- the controllers' sample.
 *
 * A page is in it when the hash of its block -- (mapping, index /
 * LRU_CAST_TUNE_BLOCK), which survives eviction and refault, unlike the PFN -- falls
 * under a threshold set at boot so that about LRU_CAST_TUNE_TARGET pages of the whole
 * memory are (the test, asked of every read, is inline: tune.h); each
 * simulated cache holds the sample's share of the type's pages. Whole
 * blocks, so that the caches see reads in runs, as readahead does.
 *
 * Not yet as the model has it: an anon page swapped out comes back as a
 * new identity.
 */
#include "tune/tune.h"
#include "tune/sample.h"

u32 lru_cast_tune_threshold __read_mostly;

void lru_cast_tune_sample_init(unsigned long total)
{
	WRITE_ONCE(lru_cast_tune_threshold, total > LRU_CAST_TUNE_TARGET ?
		   (u32)div64_u64((u64)LRU_CAST_TUNE_TARGET << 32, total) : U32_MAX);
}

/* the identity of the page, in the sample if its block is (lru_cast_tune_block()) */
u64 lru_cast_tune_sample_page(struct folio *folio)
{
	return lru_cast_tune_mix(READ_ONCE(folio->mapping), folio->index);
}

unsigned long lru_cast_tune_sample_pages(unsigned long pages)
{
	return mult_frac(pages, (unsigned long)READ_ONCE(lru_cast_tune_threshold), 1UL << 32);
}
