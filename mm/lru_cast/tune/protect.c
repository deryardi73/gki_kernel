// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- catching every read of a sampled mapped page.
 *
 * The decay controller's simulated caches need the true reference stream
 * of the sampled pages (tune.c). A read of a page mapped in a process is
 * invisible to the kernel while the page is resident: the CPU only sets the
 * young bit. So the sampled pages' PTEs are made PROT_NONE, the way NUMA
 * balancing marks pages for its hinting faults; the next access faults, and
 * do_numa_page() hands the fault here (lru_cast_protfault()): one read of
 * the page for the simulated caches, the PTE made accessible again, and the
 * page queued to be protected again.
 *
 * Protecting again is lazy and batched: the queue is worked through once
 * the clock has moved by delta = 1/1024 of the node's pages since the last
 * time (about 20 ms at 100k pages taken in a second with C = 2M), and
 * only the pages that faulted in between -- or had a read recorded by the
 * main cache -- are in it. The TLB is flushed once a batch, per mm, not per
 * page: a flush per page was an IPI per page to the CPUs running the mm.
 * Until the flush a stale entry lets a read through without its fault --
 * one read the sample misses, nothing more, as the PTE only grew stricter. A page touched again within delta faults once:
 * the model's finding is that this loses nothing, and it keeps the faults
 * to about 0.6% of accesses.
 *
 * Only while NUMA balancing is off -- a single node, or switched off: then
 * every hinting fault is CAST's. With it on, no page is protected and the
 * simulated caches see what the main cache sees. Not protected either:
 * KSM pages, PMD-mapped THPs, hugetlb and special mappings.
 */
#include <linux/rmap.h>
#include <linux/hugetlb.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <asm/tlbflush.h>
#include "tune/tune.h"

#define LRU_CAST_PROT_QUEUE	4096

static DEFINE_SPINLOCK(lru_cast_prot_lock);
static unsigned long lru_cast_prot_queue[2][LRU_CAST_PROT_QUEUE];
static unsigned int lru_cast_prot_len, lru_cast_prot_cur;
static long lru_cast_prot_installed, lru_cast_prot_next;	/* the clock, net, as told */
/* PTEs protected, and protection faults taken */
static atomic_long_t lru_cast_prot_armed, lru_cast_prot_faults;

/* the mms a batch armed PTEs in, to be flushed once at its end */
#define LRU_CAST_PROT_MMS	8
struct lru_cast_prot_flush {
	struct mm_struct *mm[LRU_CAST_PROT_MMS];
	int n;
};

static void lru_cast_prot_flush_all(struct lru_cast_prot_flush *f)
{
	while (f->n) {
		struct mm_struct *mm = f->mm[--f->n];

		flush_tlb_mm(mm);
		mmdrop(mm);
	}
}

static void lru_cast_prot_flush_add(struct lru_cast_prot_flush *f, struct mm_struct *mm)
{
	int i;

	for (i = 0; i < f->n; i++)
		if (f->mm[i] == mm)
			return;
	if (f->n == LRU_CAST_PROT_MMS)
		lru_cast_prot_flush_all(f);
	mmgrab(mm);
	f->mm[f->n++] = mm;
}

static void lru_cast_prot_workfn(struct work_struct *work);
static DECLARE_WORK(lru_cast_prot_work, lru_cast_prot_workfn);

bool lru_cast_protect_active(void)
{
	return lru_cast_numa_hinting_free();
}

/* @folio, sampled, is to be protected (again) at the next boundary */
void lru_cast_protect_queue(struct folio *folio)
{
	unsigned long flags;

	if (!lru_cast_protect_active())
		return;
	spin_lock_irqsave(&lru_cast_prot_lock, flags);
	if (lru_cast_prot_len < LRU_CAST_PROT_QUEUE)
		lru_cast_prot_queue[lru_cast_prot_cur][lru_cast_prot_len++] = folio_pfn(folio);
	spin_unlock_irqrestore(&lru_cast_prot_lock, flags);
}

/* the clock moved by @nr pages taken in, of @size: past a boundary, protect */
void lru_cast_protect_installed(long nr, unsigned long size)
{
	data_race(lru_cast_prot_installed += nr);
	if (data_race(lru_cast_prot_installed) < data_race(lru_cast_prot_next))
		return;
	data_race(lru_cast_prot_next = lru_cast_prot_installed + (long)max(size / 1024, 32UL));
	if (READ_ONCE(lru_cast_prot_len) && lru_cast_protect_active())
		lru_cast_queue_work(&lru_cast_prot_work);
}

static bool lru_cast_prot_arm_one(struct folio *folio, struct vm_area_struct *vma,
		       unsigned long address, void *arg)
{
	DEFINE_FOLIO_VMA_WALK(pvmw, folio, vma, address, 0);
	struct mmu_notifier_range range;
	bool armed = false;

	if (!vma_is_accessible(vma) || is_vm_hugetlb_page(vma) ||
	    (vma->vm_flags & (VM_IO | VM_PFNMAP | VM_MIXEDMAP)))
		return true;

	lru_cast_mmu_range_init(&range, vma->vm_mm, address,
			     min(vma->vm_end, address + folio_size(folio)));
	mmu_notifier_invalidate_range_start(&range);
	while (page_vma_mapped_walk(&pvmw)) {
		pte_t old, new;

		if (!pvmw.pte)			/* PMD-mapped: not protected */
			continue;
		old = ptep_get(pvmw.pte);
		if (!pte_present(old) || pte_protnone(old))
			continue;
		old = ptep_modify_prot_start(vma, pvmw.address, pvmw.pte);
		new = pte_modify(old, PAGE_NONE);
		ptep_modify_prot_commit(vma, pvmw.address, pvmw.pte, old, new);
		atomic_long_inc(&lru_cast_prot_armed);
		armed = true;
	}
	mmu_notifier_invalidate_range_end(&range);
	if (armed)
		lru_cast_prot_flush_add(arg, vma->vm_mm);
	return true;
}

static void lru_cast_prot_arm_pfn(unsigned long pfn, struct lru_cast_prot_flush *f)
{
	struct rmap_walk_control rwc = {
		.rmap_one = lru_cast_prot_arm_one,
		.arg = f,
		.try_lock = true,
	};
	struct page *page = pfn_to_online_page(pfn);
	struct folio *folio;

	if (!page)
		return;
	folio = page_folio(page);
	if (!folio_try_get(folio))
		return;
	if (unlikely(page_folio(page) != folio) || !folio_test_lru(folio) ||
	    !folio_mapped(folio) || folio_test_ksm(folio) ||
	    folio_is_zone_device(folio) || !lru_cast_tune_sampled(folio))
		goto put;
	if (!folio_trylock(folio))
		goto put;
	rmap_walk(folio, &rwc);
	folio_unlock(folio);
put:
	folio_put(folio);
}

static void lru_cast_prot_workfn(struct work_struct *work)
{
	struct lru_cast_prot_flush f = { .n = 0 };
	unsigned int n, i, b;

	spin_lock_irq(&lru_cast_prot_lock);
	b = lru_cast_prot_cur;
	n = lru_cast_prot_len;
	lru_cast_prot_cur ^= 1;
	lru_cast_prot_len = 0;
	spin_unlock_irq(&lru_cast_prot_lock);

	for (i = 0; i < n; i++) {
		if (!lru_cast_enabled() || !lru_cast_protect_active())
			break;
		lru_cast_prot_arm_pfn(lru_cast_prot_queue[b][i], &f);
		if (!(i & 63))
			cond_resched();
	}
	lru_cast_prot_flush_all(&f);
}

/*
 * do_numa_page(), with the PTE lock held: a hinting fault. CAST's while
 * NUMA balancing is off -- a read of a sampled page for the simulated
 * caches, the page to be protected again -- and the caller only makes the
 * PTE accessible. True if it was CAST's.
 */
bool lru_cast_protfault(struct folio *folio)
{
	if (!lru_cast_enabled() || !lru_cast_protect_active())
		return false;
	atomic_long_inc(&lru_cast_prot_faults);
	lru_cast_tune_read(folio);
	lru_cast_protect_queue(folio);
	return true;
}

int lru_cast_protect_show(char *buf, int len)
{
	return len + sysfs_emit_at(buf, len, "protect %s armed %ld faults %ld\n",
				   lru_cast_protect_active() ? "on" : "off",
				   atomic_long_read(&lru_cast_prot_armed),
			   atomic_long_read(&lru_cast_prot_faults));
}
