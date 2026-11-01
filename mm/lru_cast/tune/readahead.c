// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- readahead, as each point of the curve would have it.
 *
 * What a split costs is the time tasks wait, and a file miss read ahead
 * in time waits nothing. How many are depends on the split -- the more of a
 * file is gone, the longer its misses run and the more of them readahead
 * covers -- so each point of the curve reads ahead for itself, as the
 * kernel would in a cache of its size: a read that follows the one before
 * it in the block (a run) opens a window at its point if it missed, and
 * keeps it open; a miss in an open window waits nothing and counts as
 * none. A read out of a run closes it. Anon is not read ahead (zram swaps
 * in one page at a time).
 */
#include <linux/hash.h>
#include <linux/vmalloc.h>
#include "tune/sample.h"

#define LRU_CAST_TUNE_BLKS		4096		/* file's blocks whose runs are followed */

/* a sampled block of file: where its run is, and the curve's open windows */
struct lru_cast_tune_blk {
	u64 id;
	pgoff_t next;			/* the index a read continuing the run is at */
	u16 win;			/* the curve's points (0..10 tenths) with a window open */
};

/* direct-mapped: a clash forgets */
struct lru_cast_tune_ra {
	struct lru_cast_tune_blk blk[LRU_CAST_TUNE_BLKS];
};

struct lru_cast_tune_ra *lru_cast_tune_ra_alloc(void)
{
	return __vmalloc(sizeof(struct lru_cast_tune_ra), LRU_CAST_TUNE_GFP);
}

void lru_cast_tune_ra_free(struct lru_cast_tune_ra *ra)
{
	vfree(ra);
}

void lru_cast_tune_ra_init(struct lru_cast_tune_ra *ra)
{
	memset(ra, 0, sizeof(*ra));
}

/*
 * Which of the curve's points (bit p: p tenths; 0 misses all) wait for the
 * read of the @nr pages at @index of @block, given those that miss it -- a
 * miss in an open window does not -- and the windows after it. The run
 * before a block is out of sight: a read at its start begins one, as a
 * read of a file's start does.
 */
u16 lru_cast_tune_ra_waits(struct lru_cast_tune_ra *ra, u64 block, pgoff_t index,
			   unsigned int nr, u16 miss)
{
	struct lru_cast_tune_blk *b = &ra->blk[hash_64(block, ilog2(LRU_CAST_TUNE_BLKS))];
	bool run = b->id == block && b->next == index;
	u16 wait = run ? miss & ~b->win : miss;

	b->id = block;
	b->win = run ? b->win | miss : index % LRU_CAST_TUNE_BLOCK ? 0 : miss;
	b->next = index + nr;
	return wait;
}
