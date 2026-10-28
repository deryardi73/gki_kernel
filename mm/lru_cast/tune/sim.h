/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CAST -- LRU_CAST_TUNE_SIM simulated CAST caches of one type's sample, and
 * its order (sim.c).
 *
 * A sampled page has a slot, found by its identity (lru_cast_tune_sim_read(), which
 * also says which caches it is resident in), and one state word, by the
 * nodes' clock, given with each read. Each cache has its decay le, W's
 * share wq (in LRU_CAST_TUNE_WUNIT) and its size cap. The order ranks the pages read
 * by key at the grid's centre: a cache of that decay and no W, of any
 * size n, holds those ranked n or better (lru_cast_tune_sim_rank()).
 *
 * A read reaches every cache and the order in one call,
 * lru_cast_tune_sim_access_all(): a page is taken in or moved up, and each cache is
 * made to fit -- W over its share meets M's lowest, then the cache gives
 * until it is within cap.
 *
 * All under the caller's lock; the sim takes none.
 */
#ifndef _MM_LRU_CAST_TUNE_SIM_H
#define _MM_LRU_CAST_TUNE_SIM_H

#include "tune/shape.h"

#define LRU_CAST_TUNE_SIM_NONE	0xffff		/* no slot */

struct lru_cast_tune_sim;

struct lru_cast_tune_sim_stat {
	int le;
	unsigned int wq;
	unsigned long cap, count, wcount;	/* size; pages resident, of them in W */
};

unsigned int lru_cast_tune_sim_max_slots(void);
struct lru_cast_tune_sim *lru_cast_tune_sim_alloc(unsigned int nslots);
void lru_cast_tune_sim_free(struct lru_cast_tune_sim *sim);
void lru_cast_tune_sim_init(struct lru_cast_tune_sim *sim, unsigned long tick, const int *le);

u16 lru_cast_tune_sim_read(struct lru_cast_tune_sim *sim, u64 id, unsigned long tick, u32 *res);
void lru_cast_tune_sim_access_all(struct lru_cast_tune_sim *sim, u16 s, u32 res);
unsigned int lru_cast_tune_sim_rank(struct lru_cast_tune_sim *sim, u16 s);
unsigned int lru_cast_tune_sim_tracked(struct lru_cast_tune_sim *sim);

int lru_cast_tune_sim_le(struct lru_cast_tune_sim *sim, int m);
unsigned long lru_cast_tune_sim_count(struct lru_cast_tune_sim *sim, int m);
void lru_cast_tune_sim_set_cap(struct lru_cast_tune_sim *sim, int m, unsigned long cap);
void lru_cast_tune_sim_set_wq(struct lru_cast_tune_sim *sim, int m, unsigned int wq);
void lru_cast_tune_sim_regrid(struct lru_cast_tune_sim *sim, const int *le, const int *src);
void lru_cast_tune_sim_stat(struct lru_cast_tune_sim *sim, int m,
			    struct lru_cast_tune_sim_stat *st);

#endif /* _MM_LRU_CAST_TUNE_SIM_H */
