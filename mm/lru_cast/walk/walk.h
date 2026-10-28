/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CAST -- the walker: reads of mapped pages, from their young bits.
 *
 *   walk.c        once a tick, the page tables of every process that ran
 *   lookaround.c  reclaim's references check, and the PTEs around it
 *   bloom.c       the PMDs the look-around found young, for the walker
 *   simd*         a page table's young bits at once (simd.h, inside walk/)
 *
 * Built on core/ alone: a young bit is recorded as a read of its folio.
 */
#ifndef _MM_LRU_CAST_WALK_H
#define _MM_LRU_CAST_WALK_H

#include "core/core.h"

void lru_cast_walk_installed(long nr, unsigned long size);
bool lru_cast_walk_enabled(void);
void lru_cast_walk_enable(bool on);
int lru_cast_walk_show(char *buf, int len);
int lru_cast_look_around_show(char *buf, int len);

/* the young-bit scan: SIMD where there is a kernel for the CPU */
bool lru_cast_simd_enabled(void);
int lru_cast_simd_enable(bool on);			/* -EINVAL: no SIMD kernel here */
const char *lru_cast_simd_name(void);

#endif /* _MM_LRU_CAST_WALK_H */
