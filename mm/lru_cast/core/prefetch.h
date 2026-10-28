/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CAST -- software prefetch in two stages, as Marie's.
 *
 * Where the frames to be read next are known before they are read -- a
 * page table's young PTEs, the sweep's candidates -- the loop pulls the
 * struct page and the state word of the frame lru_cast_pf_l3 ahead from
 * memory toward L3, and of the frame lru_cast_pf_l1 ahead from L3 into
 * L1: the long haul started as early as the next frames are known, while
 * L1 holds only the few about to be read.
 *
 *	L3 leg: prefetcht2 on x86 (into L2/L3, not L1), tracked by the L2's
 *		miss buffers (MSHRs): it may be as far ahead as those can
 *		hold misses in flight
 *	L1 leg: prefetcht0 (into every level), tracked by L1's fill buffers:
 *		a short way, as far as the L3-to-L1 latency needs
 *
 * The kernel's prefetch() is PREFETCHNTA on x86, which bypasses L3 and
 * is gone from L1 soon: wrong for the long leg. __builtin_prefetch() with
 * locality 1 and 3 gives the two hints where the architecture has them.
 *
 * How far is set at boot by the CPU (lru_cast_prefetch_init()), and may
 * be set by hand (sysfs prefetch; 0 0 turns it off). A prefetch never
 * faults: a frame with no struct page or no word costs a line at most.
 */
#ifndef _MM_LRU_CAST_CORE_PREFETCH_H
#define _MM_LRU_CAST_CORE_PREFETCH_H

#include <linux/mm.h>

#define LRU_CAST_PF_MAX		64	/* the most either leg may be set to */

extern unsigned int lru_cast_pf_l3, lru_cast_pf_l1;
/* store.c's, for the address of a frame's word alone */
extern u32 *lru_cast_states;

void lru_cast_prefetch_init(void);
int lru_cast_prefetch_set(unsigned int l3, unsigned int l1);

/* the frame @pfn's struct page and state word, pulled toward L1 (@l1) or L3 */
static __always_inline void lru_cast_prefetch_frame(unsigned long pfn, bool l1)
{
	u32 *states = READ_ONCE(lru_cast_states);

	if (l1) {
#ifdef CONFIG_SPARSEMEM_VMEMMAP		/* pfn_to_page() is arithmetic, for any pfn */
		__builtin_prefetch(pfn_to_page(pfn), 0, 3);
#endif
		if (states)
			__builtin_prefetch(states + pfn, 0, 3);
	} else {
#ifdef CONFIG_SPARSEMEM_VMEMMAP
		__builtin_prefetch(pfn_to_page(pfn), 0, 1);
#endif
		if (states)
			__builtin_prefetch(states + pfn, 0, 1);
	}
}

#endif /* _MM_LRU_CAST_CORE_PREFETCH_H */
