/* SPDX-License-Identifier: GPL-2.0 */
/* CAST -- what a refault costs, per type (cost.c) */
#ifndef _MM_LRU_CAST_TUNE_COST_H
#define _MM_LRU_CAST_TUNE_COST_H

#include "tune/shape.h"

void lru_cast_tune_cost_gather(void);
void lru_cast_tune_cost_reset(void);
void lru_cast_tune_weights(u64 *wa, u64 *wf);

#endif /* _MM_LRU_CAST_TUNE_COST_H */
