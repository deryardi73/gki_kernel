/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CAST -- the shape of the controllers, shared inside tune/.
 *
 * Each type's sample is measured at LRU_CAST_TUNE_N points at once, the first LRU_CAST_TUNE_SIM
 * of them simulated caches (sim.c), the rest read from its order:
 *
 *	0..8	the decay grid: le at the centre and +-1, 2, 4, 8 from it
 *		(decay.c), W as the main cache's
 *	9, 10	W's share -/+ 0.1 C, le at the centre (wshare.c)
 *	11..20	the curve: the type at 1..10 tenths of all memory's sample,
 *		each a world with the other type's point at the rest
 *		(world.c, share.c), le at the centre and no W
 */
#ifndef _MM_LRU_CAST_TUNE_SHAPE_H
#define _MM_LRU_CAST_TUNE_SHAPE_H

#include "tune/tune.h"

#define LRU_CAST_TUNE_N		21
#define LRU_CAST_TUNE_DECAY	9		/* the decay grid: 0..8 */
#define LRU_CAST_TUNE_CENTER	4
#define LRU_CAST_TUNE_WLO		9
#define LRU_CAST_TUNE_WHI		10
#define LRU_CAST_TUNE_CURVE	11		/* the type at 1..10 tenths of all memory: 11..20 */
#define LRU_CAST_TUNE_SIM		LRU_CAST_TUNE_CURVE	/* the simulated caches: 0..10 */
#define LRU_CAST_TUNE_SHARES	10

#define LRU_CAST_TUNE_TARGET	15000		/* sampled pages in the whole memory */
#define LRU_CAST_TUNE_TABLE	4		/* slots per page of a simulated cache */

#define LRU_CAST_TUNE_WUNIT	64000		/* W's share in 1/64000 of the cache */

/* the controllers are optional: switching CAST on must not invoke the OOM killer */
#define LRU_CAST_TUNE_GFP		(GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN)

/* the world of @type's curve point @m: anon's at k tenths is world k, file's world 10 - k */
static inline int lru_cast_tune_world_of(int type, int m)
{
	int p = m - LRU_CAST_TUNE_CURVE + 1;

	return type ? LRU_CAST_TUNE_SHARES - p : p;
}

/* the curve point of @type in world @k, or -1: none (anon at 0, file at 10) */
static inline int lru_cast_tune_cache_in(int type, int k)
{
	if (!type)
		return k ? LRU_CAST_TUNE_CURVE + k - 1 : -1;
	return k < LRU_CAST_TUNE_SHARES ? LRU_CAST_TUNE_CURVE + LRU_CAST_TUNE_SHARES - k - 1 : -1;
}

/* the pages of @type on the LRUs, every node's */
static inline unsigned long lru_cast_tune_type_pages(int type)
{
	return global_node_page_state(NR_LRU_BASE + LRU_INACTIVE_ANON + type * LRU_FILE) +
	       global_node_page_state(NR_LRU_BASE + LRU_ACTIVE_ANON + type * LRU_FILE);
}

#endif /* _MM_LRU_CAST_TUNE_SHAPE_H */
