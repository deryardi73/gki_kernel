// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- the walker: reads of mapped pages, from the young bits.
 *
 * A read of a mapped page leaves only the PTE's young bit. Reclaim looks at
 * it when it reaches the page, which for a page in use comes late: by then
 * the page has had its key from reads long past. So once a tick of the
 * clock (C/4 pages taken in; at most ten times a second) this walks
 * the page tables of every process: a young PTE is cleared and counts one
 * read of its folio -- one per folio per walk, however many times it was
 * touched in between, which is what a young bit can tell. Once a tick, as
 * a state word tells reads apart by tick: walked more often, the walker
 * would add no time, only reads counted by how often it walked.
 *
 * A young bit is not free to take: cleared, it costs the task that next
 * touches the page a few hundred ns while the CPU sets it again. So none is
 * taken that would add nothing -- of a folio already read this tick
 * (lru_cast_young_take()) -- and what cannot have been read since the last
 * walk is not looked at: an mm no CPU has run since (lru_cast_used, set at a
 * switch to it and at a tick in it), and the PTEs under a PMD nothing under
 * which was reached. The CPU says so where it keeps an accessed bit in the
 * PMD (arch_has_hw_nonleaf_pmd_young()), cleared here, under the PMD's lock
 * and before its PTEs are read, when the walk covers the whole PMD. Where it
 * does not, and in an mm with a secondary MMU (KVM), whose accesses do not
 * walk these tables, the walker reads only the PMDs reclaim's look-around
 * found young (bloom.c, lookaround.c), and asks the secondary MMU of each
 * PTE (lru_cast_ptep_clear_young()). Otherwise a page table is read for its
 * young bits at once (simd.h), and only the PTEs found young are looked at
 * one by one.
 *
 * The first young bit after a folio entered the cache may be the fault
 * that brought it, counted already: as at reclaim, it only clears the mark
 * (lru_cast_access_seen()). While sampled pages are protected their reads
 * reach the controllers as faults, so the walker's are the main cache's
 * only (lru_cast_access_young()); a PTE protected for that is left to its
 * fault.
 *
 * Executable file text is not walked: whether it stays is decided at
 * reclaim by whether it is in use -- a young pte then -- not by its key
 * (mm/vmscan.c), and a bit cleared here would be that sign gone.
 *
 * A process whose mmap lock is taken is skipped this time. PMD-mapped THPs
 * are read at the PMD, not split; hugetlb and PFN maps are not walked.
 */
#include <linux/pagewalk.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/stat.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/pgtable.h>
#include <linux/mmu_notifier.h>
#include "walk/walk.h"
#include "walk/simd.h"
#include "walk/young.h"

static unsigned int lru_cast_walk_on = 1;
static long lru_cast_walk_net, lru_cast_walk_next;		/* the clock, net, as told */
static unsigned long lru_cast_walk_last;
static atomic_long_t lru_cast_walk_walks, lru_cast_walk_young, lru_cast_walk_kept;
static atomic_long_t lru_cast_walk_mms, lru_cast_walk_mms_idle;
static atomic_long_t lru_cast_walk_tables, lru_cast_walk_tables_idle;
static atomic64_t lru_cast_walk_ns;		/* time spent walking */

static void lru_cast_walk_workfn(struct work_struct *work);
static DECLARE_WORK(lru_cast_walk_work, lru_cast_walk_workfn);

/*
 * The young bit of @folio's PTE @pte, if it is the walker's to take: one
 * read per folio per walk. @pmd for a PMD-mapped folio, under its lock.
 */
static void lru_cast_walk_take(struct vm_area_struct *vma, unsigned long addr, pte_t *pte,
		    pmd_t *pmd, struct folio *folio, struct folio **last)
{
	if (folio == *last || !folio_test_lru(folio))
		return;
	if (!lru_cast_young_take(folio)) {
		atomic_long_inc(&lru_cast_walk_kept);
		return;
	}
	if (pte ? !lru_cast_ptep_clear_young(vma, addr, pte) :
		  !lru_cast_pmdp_clear_young(vma, addr, pmd))
		return;
	*last = folio;
	if (lru_cast_young_record(folio))
		atomic_long_inc(&lru_cast_walk_young);
}

/* the hardware's own record of a PMD's use, where the CPU keeps it and nothing bypasses it */
static bool lru_cast_walk_pmd_young_usable(struct mm_struct *mm)
{
	return arch_has_hw_nonleaf_pmd_young() && !mm_has_notifiers(mm);
}

static int lru_cast_walk_pmd(pmd_t *pmd, unsigned long addr, unsigned long next,
		  struct mm_walk *walk)
{
	struct vm_area_struct *vma = walk->vma;
	struct folio **last = walk->private;
	spinlock_t *ptl;
	pte_t *start, *pte;

	bool notifiers = mm_has_notifiers(walk->mm);

	walk->action = ACTION_CONTINUE;
	ptl = pmd_trans_huge_lock(pmd, vma);
	if (ptl) {
		pmd_t val = pmdp_get(pmd);

		if (pmd_present(val) && !pmd_protnone(val) && (pmd_young(val) || notifiers))
			lru_cast_walk_take(vma, addr, NULL, pmd, pmd_folio(val), last);
		spin_unlock(ptl);
		return 0;
	}

	/*
	 * Nothing under it reached since its accessed bit was cleared: the
	 * hardware's word, or failing it the bloom's. The bit is cleared
	 * only when the walk covers the whole PMD -- a PMD may span more
	 * than one VMA, and the next would find it clear -- under its lock,
	 * and before the PTEs are read, so that a use racing the walk sets
	 * it again; a lock not got leaves it set, a table read once more.
	 */
	if (lru_cast_walk_pmd_young_usable(walk->mm)) {
		pmd_t val = pmdp_get_lockless(pmd);

		if (!pmd_present(val) || !pmd_young(val)) {
			atomic_long_inc(&lru_cast_walk_tables_idle);
			return 0;
		}
		if (next - addr == PMD_SIZE) {
			ptl = pmd_lockptr(walk->mm, pmd);
			if (spin_trylock(ptl)) {
				if (pmd_present(*pmd) && !pmd_leaf(*pmd))
					pmdp_test_and_clear_young(vma, addr, pmd);
				spin_unlock(ptl);
			}
		}
	} else if (!lru_cast_bloom_test(walk->mm, addr)) {
		atomic_long_inc(&lru_cast_walk_tables_idle);
		return 0;
	}

	start = pte_offset_map_lock(walk->mm, pmd, addr, &ptl);
	if (!start)
		return 0;
	{
		unsigned long young[LRU_CAST_PTE_LONGS] = { 0 };
		unsigned int base = pte_index(addr), i = base;
		unsigned int end = base + ((next - addr) >> PAGE_SHIFT);
		unsigned int l3 = READ_ONCE(lru_cast_pf_l3), l1 = READ_ONCE(lru_cast_pf_l1);
		unsigned int p3 = base, p1 = base, n;
		pte_t *table = start - base;

		atomic_long_inc(&lru_cast_walk_tables);
		if (notifiers) {	/* the secondary MMU's uses set no bit here: ask of each */
			bitmap_set(young, base, end - base);
			l3 = 0;
		} else {
			lru_cast_young_pte_mask(table, young);
		}
		/*
		 * The young frames read next pulled in: cursors l3 and l1 young
		 * PTEs ahead of the one read, each pulling its frame toward L3
		 * and L1 as it passes it.
		 */
		for (n = 0; n < l3 && (p3 = find_next_bit(young, end, p3)) < end; n++, p3++)
			lru_cast_young_ahead(table + p3, false);
		for (n = 0; n < l1 && (p1 = find_next_bit(young, end, p1)) < end; n++, p1++)
			lru_cast_young_ahead(table + p1, true);
		for_each_set_bit_from(i, young, end) {
			unsigned long a = addr + ((unsigned long)(i - base) << PAGE_SHIFT);
			pte_t val;
			struct folio *folio;

			if (l3 && (p3 = find_next_bit(young, end, p3)) < end)
				lru_cast_young_ahead(table + p3++, false);
			if (l3 && (p1 = find_next_bit(young, end, p1)) < end)
				lru_cast_young_ahead(table + p1++, true);

			pte = table + i;
			val = ptep_get(pte);
			if (!pte_present(val) || pte_protnone(val) || (!pte_young(val) && !notifiers))
				continue;
			folio = vm_normal_folio(vma, a, val);
			if (folio)
				lru_cast_walk_take(vma, a, pte, NULL, folio, last);
		}
	}
	pte_unmap_unlock(start, ptl);
	cond_resched();
	return 0;
}

static int lru_cast_walk_test(unsigned long start, unsigned long end, struct mm_walk *walk)
{
	struct vm_area_struct *vma = walk->vma;

	/* only what the LRU keeps: no hugetlb, no special mappings */
	if (is_vm_hugetlb_page(vma) || (vma->vm_flags & (VM_PFNMAP | VM_MIXEDMAP | VM_IO)))
		return 1;
	/* program text: kept while in use, which its young bits tell reclaim */
	if ((vma->vm_flags & VM_EXEC) && vma->vm_file)
		return 1;
	return 0;
}

static const struct mm_walk_ops lru_cast_walk_ops = {
	.pmd_entry = lru_cast_walk_pmd,
	.test_walk = lru_cast_walk_test,
	.walk_lock = PGWALK_RDLOCK,
};

static void lru_cast_walk_mm(struct mm_struct *mm)
{
	struct folio *last = NULL;

	atomic_long_inc(&lru_cast_walk_mms);
	/* no CPU has run it since the last walk: no young bit can be new */
	if (!xchg(&mm->lru_cast_used, 0)) {
		atomic_long_inc(&lru_cast_walk_mms_idle);
		return;
	}
	if (!mmap_read_trylock(mm)) {
		WRITE_ONCE(mm->lru_cast_used, 1);	/* not walked: the next walk does */
		return;
	}
	walk_page_range(mm, 0, TASK_SIZE, &lru_cast_walk_ops, &last);
	mmap_read_unlock(mm);
}

static void lru_cast_walk_workfn(struct work_struct *work)
{
	struct task_struct *p;
	struct mm_struct **mms;
	int n = 0, max = nr_processes() + 64, i;
	u64 t0 = ktime_get_ns();

	mms = kvmalloc_array(max, sizeof(*mms), GFP_KERNEL);
	if (!mms)
		return;
	rcu_read_lock();
	for_each_process(p) {
		struct mm_struct *mm;

		if (n >= max)
			break;
		if (p->flags & PF_KTHREAD)
			continue;
		mm = get_task_mm(p);
		if (mm)
			mms[n++] = mm;
	}
	rcu_read_unlock();

	for (i = 0; i < n; i++) {
		if (lru_cast_enabled())
			lru_cast_walk_mm(mms[i]);
		mmput(mms[i]);
		cond_resched();
	}
	kvfree(mms);
	lru_cast_bloom_swap();
	atomic_long_inc(&lru_cast_walk_walks);
	atomic64_add(ktime_get_ns() - t0, &lru_cast_walk_ns);
}

/* the clock moved by @nr pages taken in, of @size: a tick on, walk */
void lru_cast_walk_installed(long nr, unsigned long size)
{
	if (!READ_ONCE(lru_cast_walk_on))
		return;
	data_race(lru_cast_walk_net += nr);
	if (data_race(lru_cast_walk_net) < data_race(lru_cast_walk_next))
		return;
	data_race(lru_cast_walk_next = lru_cast_walk_net + (long)max(size / 4, 1024UL));
	if (time_before(jiffies, data_race(lru_cast_walk_last) + HZ / 10))
		return;
	data_race(lru_cast_walk_last = jiffies);
	lru_cast_queue_work(&lru_cast_walk_work);
}

bool lru_cast_walk_enabled(void)
{
	return READ_ONCE(lru_cast_walk_on);
}

void lru_cast_walk_enable(bool on)
{
	WRITE_ONCE(lru_cast_walk_on, on);
}

int lru_cast_walk_show(char *buf, int len)
{
	return len + sysfs_emit_at(buf, len,
				   "walker walks %ld us %llu young %ld kept %ld mms %ld idle %ld tables %ld idle %ld scan %s\n",
				   atomic_long_read(&lru_cast_walk_walks),
				   atomic64_read(&lru_cast_walk_ns) / 1000,
				   atomic_long_read(&lru_cast_walk_young),
				   atomic_long_read(&lru_cast_walk_kept),
				   atomic_long_read(&lru_cast_walk_mms),
				   atomic_long_read(&lru_cast_walk_mms_idle),
				   atomic_long_read(&lru_cast_walk_tables),
				   atomic_long_read(&lru_cast_walk_tables_idle),
				   lru_cast_simd_name());
}
