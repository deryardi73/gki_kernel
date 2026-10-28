/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CAST -- the state words' array, inside core/ only: the rest reads
 * and writes a folio's word through core.h.
 */
#ifndef _MM_LRU_CAST_CORE_STORE_H
#define _MM_LRU_CAST_CORE_STORE_H

#include "core/core.h"

extern u32 *lru_cast_states;
extern unsigned long lru_cast_pfn_limit;
extern u8 *lru_cast_sec_on;

/* the state word of @pfn, or NULL: none (see store.c) */
static inline u32 *lru_cast_word(unsigned long pfn)
{
	if (unlikely(pfn >= READ_ONCE(lru_cast_pfn_limit)))
		return NULL;
	if (unlikely(!smp_load_acquire(&lru_cast_sec_on[pfn_to_section_nr(pfn)])))
		return NULL;
	return lru_cast_states + pfn;
}

#endif /* _MM_LRU_CAST_CORE_STORE_H */
