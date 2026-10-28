/* SPDX-License-Identifier: GPL-2.0 */
/* CAST -- the decay controller (decay.c) */
#ifndef _MM_LRU_CAST_TUNE_DECAY_H
#define _MM_LRU_CAST_TUNE_DECAY_H

#include "tune/evidence.h"
#include "tune/sim.h"

void lru_cast_tune_decay_grid(int centre, int *le, int *src);
void lru_cast_tune_decay_regrid(struct lru_cast_tune_sim *sim, int centre, int *le, int *src);
int lru_cast_tune_decay_choose(const struct lru_cast_tune_evidence *ev,
			       struct lru_cast_tune_sim *sim);

#endif /* _MM_LRU_CAST_TUNE_DECAY_H */
