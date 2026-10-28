/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CAST -- the curve's worlds (world.c): memory held at each split.
 * Under the controllers' lock.
 */
#ifndef _MM_LRU_CAST_TUNE_WORLD_H
#define _MM_LRU_CAST_TUNE_WORLD_H

#include "tune/shape.h"

void lru_cast_tune_world_reset(void);
void lru_cast_tune_world_set_size(unsigned long all);
unsigned long lru_cast_tune_world_size(int type, int k, const unsigned int *read);

#endif /* _MM_LRU_CAST_TUNE_WORLD_H */
