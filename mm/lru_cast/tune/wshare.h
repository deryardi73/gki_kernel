/* SPDX-License-Identifier: GPL-2.0 */
/* CAST -- W's share and its test (wshare.c) */
#ifndef _MM_LRU_CAST_TUNE_WSHARE_H
#define _MM_LRU_CAST_TUNE_WSHARE_H

#include "tune/sim.h"

struct lru_cast_tune_w {
	unsigned int wq;		/* W's share, in LRU_CAST_TUNE_WUNIT */
	s64 ws, wn;			/* the test: w - 0.1C's misses less w + 0.1C's, and N */
	unsigned long next_wtest, wmoves;
};

void lru_cast_tune_w_init(struct lru_cast_tune_w *w, unsigned int permille,
			  struct lru_cast_tune_sim *sim);
void lru_cast_tune_w_note(struct lru_cast_tune_w *w, bool hit_lo, bool hit_hi);
void lru_cast_tune_w_forget(struct lru_cast_tune_w *w);
void lru_cast_tune_w_test(struct lru_cast_tune_w *w, struct lru_cast_tune_sim *sim, int type,
			  unsigned long events, unsigned long cap);

#endif /* _MM_LRU_CAST_TUNE_WSHARE_H */
