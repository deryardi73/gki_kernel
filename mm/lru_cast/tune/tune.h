/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CAST -- the controllers: what the main cache's operating point
 * (policy/params.c) should be, measured on a sample of the pages.
 *
 *   tune.c       runs them: each read of a sampled page, the decisions
 *   sample.c     which pages are sampled
 *   sim.c        the simulated caches of a type's sample (shape.h: which)
 *   world.c      the curve's points, memory held at each split
 *   readahead.c  file's misses readahead covers, at each point
 *   evidence.c   what the caches showed
 *   decay.c      the decay, from the grid of decays
 *   wshare.c     W's share, from the pair of shares
 *   share.c      anon's share of memory, from the curves
 *   cost.c       what a refault costs, per type
 *   protect.c    every read of a sampled mapped page, by a protection fault
 *
 * Built on core/ and on policy/'s parameters, which it sets. Inside, each
 * file has its own header; sim.c's caches and readahead.c's blocks are
 * reached only through theirs.
 */
#ifndef _MM_LRU_CAST_TUNE_H
#define _MM_LRU_CAST_TUNE_H

#include <linux/hash.h>
#include "core/core.h"

#define LRU_CAST_TUNE_BLOCK	32	/* pages sampled together: readahead's window */

extern u32 lru_cast_tune_threshold;	/* sample.c: sampled, a block's hash >> 32 below it */
extern unsigned int lru_cast_tune_on;	/* tune.c: the controllers set the numbers */

/* the controllers set the decay, W's share and anon's share; off, they are set by hand */
static inline bool lru_cast_tune_auto(void)
{
	return READ_ONCE(lru_cast_tune_on);
}

/* (mapping, @index) mixed to 64 bits (splitmix64's finaliser): 0 for no mapping */
static inline u64 lru_cast_tune_mix(struct address_space *mapping, pgoff_t index)
{
	u64 x = (unsigned long)mapping;

	if (!x)
		return 0;
	x ^= index * GOLDEN_RATIO_64;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
	return (x ^ (x >> 31)) | 1;
}

/* the identity of @folio's block (sample.c) */
static inline u64 lru_cast_tune_block(struct folio *folio)
{
	return lru_cast_tune_mix(READ_ONCE(folio->mapping), folio->index / LRU_CAST_TUNE_BLOCK);
}

static inline bool lru_cast_tune_in(u64 block)
{
	return block && (u32)(block >> 32) < READ_ONCE(lru_cast_tune_threshold);
}

static inline bool lru_cast_tune_sampled(struct folio *folio)
{
	return lru_cast_tune_in(lru_cast_tune_block(folio));
}

/* tune.c */
int lru_cast_tune_init(void);			/* each time CAST comes on */
void lru_cast_tune_event(struct folio *folio, u64 block);
void lru_cast_tune_set_auto(bool on);

/*
 * A read of @folio, for the controllers if it is in their sample. Every
 * read the main cache records is asked this, and all but one in some
 * fifty are not: asked inline, they cost no call.
 */
static inline void lru_cast_tune_read(struct folio *folio)
{
	u64 block;

	if (!lru_cast_tune_auto())
		return;
	block = lru_cast_tune_block(folio);
	if (lru_cast_tune_in(block))
		lru_cast_tune_event(folio, block);
}
int lru_cast_tune_show(char *buf);
int lru_cast_tune_estimate_show(char *buf);

/* protect.c */
bool lru_cast_protect_active(void);
void lru_cast_protect_queue(struct folio *folio);
void lru_cast_protect_installed(long nr, unsigned long size);
int lru_cast_protect_show(char *buf, int len);

#endif /* _MM_LRU_CAST_TUNE_H */
