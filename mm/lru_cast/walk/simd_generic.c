// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- the young-bit scan where there is no SIMD kernel: the
 * arch's pte_young(), one PTE at a time.
 */
#include <linux/bitmap.h>
#include <linux/mm.h>
#include "walk/walk.h"
#include "walk/simd.h"

void lru_cast_young_pte_mask(const void *table, unsigned long *bitmap)
{
	const pte_t *pte = table;
	int i;

	for (i = 0; i < PTRS_PER_PTE; i++)
		if (pte_young(ptep_get((pte_t *)&pte[i])))
			__set_bit(i, bitmap);
}

bool lru_cast_simd_enabled(void)
{
	return false;
}

int lru_cast_simd_enable(bool on)
{
	return on ? -EINVAL : 0;
}

const char *lru_cast_simd_name(void)
{
	return "scalar";
}
