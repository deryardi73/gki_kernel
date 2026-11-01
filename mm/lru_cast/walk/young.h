/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CAST -- what the walker and the look-around share, inside walk/:
 * whether a young bit is theirs to take, how a taken one is recorded, and
 * the bloom of PMDs reclaim found young (bloom.c).
 */
#ifndef _MM_LRU_CAST_WALK_YOUNG_H
#define _MM_LRU_CAST_WALK_YOUNG_H

#include "core/core.h"

/*
 * May @folio's young bit be taken -- cleared and recorded? Not when its
 * last read is stamped with the current tick: a state word tells reads
 * apart by tick, so recording another now adds nothing, and the bit is
 * left for reclaim's references check, which needs it. Clearing it would
 * cost the task its next access to the page, where the CPU sets the bit
 * again (some hundred ns a page), for nothing.
 */
static inline bool lru_cast_young_take(struct folio *folio)
{
	u32 w = lru_cast_read(folio);

	return !(w >> 24) ||
	       lru_cast_age(w, lru_cast_tick(folio_pgdat(folio), folio_is_file_lru(folio)));
}

/*
 * A young bit taken from @folio. The first after the folio entered the
 * cache may be the fault that brought it, counted already: it only clears
 * the mark (lru_cast_access_seen()). False if nothing was recorded.
 */
static inline bool lru_cast_young_record(struct folio *folio)
{
	if (folio_test_clear_referenced(folio))
		return false;
	lru_cast_access_young(folio);
	return true;
}

/*
 * The frame of the PTE @pte, if it is young, pulled toward L1 (@l1) or L3
 * (core/prefetch.h): it is one the loop will record a read of soon.
 */
static inline void lru_cast_young_ahead(pte_t *pte, bool l1)
{
	pte_t val = ptep_get(pte);

	if (pte_present(val) && pte_young(val))
		lru_cast_prefetch_frame(pte_pfn(val), l1);
}

/* bloom.c */
void lru_cast_bloom_set(struct mm_struct *mm, unsigned long addr);
bool lru_cast_bloom_test(struct mm_struct *mm, unsigned long addr);
void lru_cast_bloom_swap(void);

#endif /* _MM_LRU_CAST_WALK_YOUNG_H */
