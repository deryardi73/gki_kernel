/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_LRU_CAST_WALK_SIMD_H
#define _MM_LRU_CAST_WALK_SIMD_H

/*
 * CAST -- the young bits of one page table, at once.
 *
 *	lru_cast_young_pte_mask(table, bitmap);
 *
 * sets bit i of @bitmap (LRU_CAST_PTE_LONGS longs, cleared by the caller) for
 * each PTE of the table at @table with its accessed bit set, whatever else
 * it holds: the walker looks again at those (present, not PROT_NONE) and
 * at those only. On x86 a SIMD kernel does it inside kernel_fpu_begin/end
 * -- the walker is a kthread, which has no FPU state to save -- picked at
 * boot from AVX-512F, AVX2 and SSE2; lru_cast_simd_enable() (sysfs
 * walker_simd) turns it off for comparison. Elsewhere it is pte_young() in
 * a loop. The switch and the name are in walk.h.
 */
#include <linux/types.h>
#include <linux/bits.h>
#include <linux/pgtable.h>

#define LRU_CAST_PTE_LONGS	BITS_TO_LONGS(PTRS_PER_PTE)

void lru_cast_young_pte_mask(const void *table, unsigned long *bitmap);

#endif /* _MM_LRU_CAST_WALK_SIMD_H */
