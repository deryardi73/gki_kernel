/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The host kernel's API, as mm/lru_cast/ uses it. Each adapter picks its
 * body by a capability from caps.h; the callers never see a version.
 */
#ifndef _MM_LRU_CAST_COMPAT_KERNEL_H
#define _MM_LRU_CAST_COMPAT_KERNEL_H

#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/memcontrol.h>
#include <linux/swap.h>
#include <linux/mmu_notifier.h>
#include <linux/sched/sysctl.h>
#include <linux/workqueue.h>
#include "compat/caps.h"
#if LRU_CAST_CAP_AGE_NONRESIDENT_INTERNAL || LRU_CAST_CAP_YOUNG_NOTIFY_INTERNAL
#include "../../internal.h"
#endif
#if LRU_CAST_CAP_SWAPPINESS_IN_SWAP_H
#include "../../swap.h"
#endif

#if LRU_CAST_CAP_NO_PMD_FOLIO
#ifndef pmd_folio
#define pmd_folio(pmd) page_folio(pmd_page(pmd))
#endif
#endif

/* the lruvec's per-list, per-zone page counts */
static inline void lru_cast_update_lru_size(struct lruvec *lruvec, enum lru_list lru,
					 int zid, long nr_pages)
{
	update_lru_size(lruvec, lru, zid, nr_pages);
}

/* the pages on one of the lruvec's lists, every zone's (vmscan.c's lruvec_lru_size()) */
static inline unsigned long lru_cast_lru_size(struct lruvec *lruvec, enum lru_list lru)
{
	unsigned long size = 0;
	int zid;

	for (zid = 0; zid < MAX_NR_ZONES; zid++) {
		struct zone *zone = &lruvec_pgdat(lruvec)->node_zones[zid];

		if (!managed_zone(zone))
			continue;
		if (!mem_cgroup_disabled())
			size += mem_cgroup_get_zone_lru_size(lruvec, lru, zid);
		else
			size += zone_page_state(zone, NR_ZONE_LRU_BASE + lru);
	}
	return size;
}

/* @page's flags, read without a lock */
static inline unsigned long lru_cast_page_flags(struct page *page)
{
#if LRU_CAST_CAP_MEMDESC_FLAGS
	return READ_ONCE(page->flags.f);
#else
	return READ_ONCE(page->flags);
#endif
}

/*
 * Is @page, the head of a folio on an LRU, charged to @memcg, as its
 * memcg_data says? Read without a lock, as the page may be freed and
 * used again meanwhile: the answer is a hint the caller confirms.
 */
static inline bool lru_cast_page_memcg_is(struct page *page, struct mem_cgroup *memcg)
{
#if LRU_CAST_CAP_OBJCG_LRU
	unsigned long data = READ_ONCE(page->memcg_data);
	struct obj_cgroup *objcg;
	bool is;

	if (!data)
		return !memcg;
	if (data & (__NR_MEMCG_DATA_FLAGS - 1))	/* a slab's, or charged kernel memory */
		return false;
	objcg = (struct obj_cgroup *)data;
	rcu_read_lock();			/* an objcg is freed after a grace period */
	is = READ_ONCE(objcg->memcg) == memcg;
	rcu_read_unlock();
	return is;
#else
	return READ_ONCE(page->memcg_data) == (unsigned long)memcg;
#endif
}

/* the lruvec folio_lruvec_lock_irq() locked, unlocked */
static inline void lru_cast_lruvec_unlock_irq(struct lruvec *lruvec)
{
#if LRU_CAST_CAP_LRUVEC_UNLOCK
	lruvec_unlock_irq(lruvec);
#else
	unlock_page_lruvec_irq(lruvec);
#endif
}

/* a refault of a folio that had been in use: the scan balance's cost, as legacy reclaim counts it */
static inline void lru_cast_note_refault_cost(struct folio *folio)
{
#if !LRU_CAST_CAP_LRU_COST_FROM_STATS
	lru_note_cost_refault(folio);
#endif
}

/* @pte's young bit taken, and a secondary MMU's of its page; was either set */
static inline bool lru_cast_ptep_clear_young(struct vm_area_struct *vma, unsigned long addr,
					     pte_t *pte)
{
#if LRU_CAST_CAP_YOUNG_NOTIFY_INTERNAL
	return test_and_clear_young_ptes_notify(vma, addr, pte, 1);
#else
	return ptep_clear_young_notify(vma, addr, pte);
#endif
}

/* ... the same of a PMD mapping a THP */
static inline bool lru_cast_pmdp_clear_young(struct vm_area_struct *vma, unsigned long addr,
					     pmd_t *pmd)
{
#if LRU_CAST_CAP_YOUNG_NOTIFY_INTERNAL
	return pmdp_test_and_clear_young_notify(vma, addr, pmd);
#else
	return pmdp_clear_young_notify(vma, addr, pmd);
#endif
}

/* PTE updates batched until left, where the architecture can */
static inline void lru_cast_lazy_mmu_enter(void)
{
#if LRU_CAST_CAP_LAZY_MMU_MODE_ENABLE
	lazy_mmu_mode_enable();
#else
	arch_enter_lazy_mmu_mode();
#endif
}

static inline void lru_cast_lazy_mmu_leave(void)
{
#if LRU_CAST_CAP_LAZY_MMU_MODE_ENABLE
	lazy_mmu_mode_disable();
#else
	arch_leave_lazy_mmu_mode();
#endif
}

/* @work queued on the unbound system workqueue */
static inline bool lru_cast_queue_work(struct work_struct *work)
{
#if LRU_CAST_CAP_SYSTEM_DFL_WQ
	return queue_work(system_dfl_wq, work);
#else
	return queue_work(system_unbound_wq, work);
#endif
}

/* the clock refault distances are read on: pages that left the inactive list */
static inline void lru_cast_age_nonresident(struct lruvec *lruvec, unsigned long nr_pages)
{
	workingset_age_nonresident(lruvec, nr_pages);
}

/*
 * Protection faults on sampled pages use the NUMA hinting PTE (PROT_NONE in
 * an accessible VMA), so they need NUMA balancing built in, and may be
 * used only while it is off: then every hinting fault is CAST's.
 */
static inline bool lru_cast_numa_hinting_free(void)
{
#ifdef CONFIG_NUMA_BALANCING
	return READ_ONCE(sysctl_numa_balancing_mode) == NUMA_BALANCING_DISABLED;
#else
	return false;
#endif
}

/* the range of PTEs of one mapping a protection change is about to touch */
static inline void lru_cast_mmu_range_init(struct mmu_notifier_range *range,
					struct mm_struct *mm,
					unsigned long start, unsigned long end)
{
	mmu_notifier_range_init(range, MMU_NOTIFY_PROTECTION_PAGE, 0, mm, start, end);
}

#endif /* _MM_LRU_CAST_COMPAT_KERNEL_H */
