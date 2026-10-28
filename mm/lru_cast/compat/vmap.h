/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Mapping pages into a reserved range, as the state array's sections are
 * attached (store.c). mm-private API, so this is included by store.c alone.
 */
#ifndef _MM_LRU_CAST_COMPAT_VMAP_H
#define _MM_LRU_CAST_COMPAT_VMAP_H

#include <linux/vmalloc.h>
#include "compat/caps.h"
#include "../../internal.h"
#if LRU_CAST_CAP_VMAP_API_IN_VMALLOC_H
#include "../../vmalloc.h"
#endif

static inline int lru_cast_vmap_pages(unsigned long addr, unsigned long end,
				   struct page **pages)
{
#if LRU_CAST_CAP_VMAP_PAGES_GFP
	return vmap_pages_range_noflush(addr, end, PAGE_KERNEL, pages, PAGE_SHIFT,
					GFP_KERNEL);
#else
	return vmap_pages_range_noflush(addr, end, PAGE_KERNEL, pages, PAGE_SHIFT);
#endif
}

#endif /* _MM_LRU_CAST_COMPAT_VMAP_H */
