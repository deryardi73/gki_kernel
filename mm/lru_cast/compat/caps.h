/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CAST's view of the host kernel, as capabilities.
 *
 * This is the only file in mm/lru_cast/ that asks which kernel version it
 * is built for. Everything else asks for a capability by name, so a host
 * difference is described once, here, in terms of what changed, and the
 * adapters in compat/kernel.h pick the matching body. That keeps the rest of
 * mm/lru_cast/ byte-identical on every kernel.
 *
 * Each capability is 0 or 1. Add one only when an adapter needs it, and say
 * in its comment what differs.
 */
#ifndef _MM_LRU_CAST_COMPAT_CAPS_H
#define _MM_LRU_CAST_COMPAT_CAPS_H

#include <linux/version.h>

/* pmd_folio() does not exist yet (added in 6.8). */
#define LRU_CAST_CAP_NO_PMD_FOLIO		(LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0))

/* vmap_pages_range_noflush() takes the gfp mask of its page-table allocations last (7.0). */
#define LRU_CAST_CAP_VMAP_PAGES_GFP		(LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0))

/* page->flags is a memdesc_flags_t, its word in .f (6.18). */
#define LRU_CAST_CAP_MEMDESC_FLAGS		(LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0))

/*
 * An LRU folio's memcg_data points to its obj_cgroup, not its mem_cgroup,
 * and a memcg taken offline hands its LRU folios to its parent's lruvec
 * by splicing its lists, lru_reparent_memcg() (7.1).
 */
#define LRU_CAST_CAP_OBJCG_LRU			(LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 0))

/*
 * system_dfl_wq is the unbound system workqueue (6.18); system_unbound_wq,
 * its old name, is warned of as deprecated from 7.2.
 */
#define LRU_CAST_CAP_SYSTEM_DFL_WQ		(LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0))

/* lazy_mmu_mode_enable()/disable() replace arch_enter/leave_lazy_mmu_mode() (7.0). */
#define LRU_CAST_CAP_LAZY_MMU_MODE_ENABLE	(LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0))

/*
 * ptep_clear_young_notify() and pmdp_clear_young_notify() are gone; their
 * like are test_and_clear_young_ptes_notify() and
 * pmdp_test_and_clear_young_notify() in mm/internal.h (7.1).
 */
#define LRU_CAST_CAP_YOUNG_NOTIFY_INTERNAL	(LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 0))

/* unlock_page_lruvec_irq() is lruvec_unlock_irq() (7.1). */
#define LRU_CAST_CAP_LRUVEC_UNLOCK		(LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 0))

/* workingset_age_nonresident() is declared in mm/internal.h, not linux/swap.h (7.3). */
#define LRU_CAST_CAP_AGE_NONRESIDENT_INTERNAL	(LINUX_VERSION_CODE >= KERNEL_VERSION(7, 3, 0))

/*
 * The anon/file scan balance reads its costs from the PGROTATE and
 * WORKINGSET_RESTORE counters: lru_note_cost_refault() is gone (7.3).
 */
#define LRU_CAST_CAP_LRU_COST_FROM_STATS	(LINUX_VERSION_CODE >= KERNEL_VERSION(7, 3, 0))

/* vm_swappiness is declared in mm/swap.h, not linux/swap.h (7.3). */
#define LRU_CAST_CAP_SWAPPINESS_IN_SWAP_H	(LINUX_VERSION_CODE >= KERNEL_VERSION(7, 3, 0))

/* vmap_pages_range_noflush() is declared in mm/vmalloc.h, not mm/internal.h (7.3). */
#define LRU_CAST_CAP_VMAP_API_IN_VMALLOC_H	(LINUX_VERSION_CODE >= KERNEL_VERSION(7, 3, 0))

#endif /* _MM_LRU_CAST_COMPAT_CAPS_H */
