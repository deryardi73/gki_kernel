// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- the look-around (Marie's): reclaim's references check, a
 * little wider.
 *
 * folio_referenced_one() asks whether a mapped folio was used, with the
 * page table's lock held for its PTE. CAST answers through here: the
 * folio's own young bit is cleared and returned, as the plain check would;
 * and when it was young, the PMD goes into the walker's bloom (bloom.c), and
 * the PTEs around it -- at most LRU_CAST_LOOK_AROUND of them, inside the PMD
 * and the VMA -- are read under the same lock: what would be one rmap walk
 * per neighbouring folio comes as one pass. A young neighbour is taken by
 * the walker's rule (lru_cast_young_take()) and recorded as its read.
 *
 * A secondary MMU's accesses are asked of it (lru_cast_ptep_clear_young()).
 * The neighbours are left alone when the lock is contended -- someone is
 * waiting for it -- in special mappings, and in program text, whose young
 * bits reclaim keeps for itself (mm/vmscan.c).
 */
#include <linux/mmu_notifier.h>
#include <linux/rmap.h>
#include <linux/sysfs.h>
#include "walk/walk.h"
#include "walk/young.h"

#define LRU_CAST_LOOK_AROUND	BITS_PER_LONG	/* PTEs read per call */

/* calls whose folio was young, and the neighbours' young bits taken */
static atomic_long_t kl_young, kl_taken;

int lru_cast_look_around_show(char *buf, int len)
{
	return len + sysfs_emit_at(buf, len, "lookaround young %ld taken %ld\n",
				   atomic_long_read(&kl_young), atomic_long_read(&kl_taken));
}

bool lru_cast_look_around(struct page_vma_mapped_walk *pvmw)
{
	struct vm_area_struct *vma = pvmw->vma;
	struct folio *target = pfn_folio(pvmw->pfn), *last = target;
	unsigned long addr = pvmw->address, start, end, a;
	unsigned int l3 = READ_ONCE(lru_cast_pf_l3), l1 = READ_ONCE(lru_cast_pf_l1), i, n;
	pte_t *pte;

	lockdep_assert_held(pvmw->ptl);

	if (!lru_cast_ptep_clear_young(vma, addr, pvmw->pte))
		return false;

	lru_cast_bloom_set(vma->vm_mm, addr);
	atomic_long_inc(&kl_young);

	if (spin_is_contended(pvmw->ptl) || (vma->vm_flags & VM_SPECIAL) ||
	    ((vma->vm_flags & VM_EXEC) && vma->vm_file))
		return true;

	/* centred on @addr, inside the PMD and the VMA */
	start = max(addr & PMD_MASK, vma->vm_start);
	end = min(addr | ~PMD_MASK, vma->vm_end - 1) + 1;
	if (end - start > LRU_CAST_LOOK_AROUND * PAGE_SIZE) {
		start = max(start, addr - min(addr - start, LRU_CAST_LOOK_AROUND * PAGE_SIZE / 2));
		start = min(start, end - LRU_CAST_LOOK_AROUND * PAGE_SIZE);
		end = start + LRU_CAST_LOOK_AROUND * PAGE_SIZE;
	}

	lru_cast_lazy_mmu_enter();
	pte = pvmw->pte - ((addr - start) >> PAGE_SHIFT);
	n = (end - start) >> PAGE_SHIFT;
	if (mm_has_notifiers(vma->vm_mm))	/* no young bits to tell which */
		l3 = 0;
	/* the young frames read next pulled in: l3 of them toward L3, the first l1 on to L1 */
	for (i = 0; i < min(l3, n); i++)
		lru_cast_young_ahead(pte + i, i < l1);
	for (a = start, i = 0; a != end; a += PAGE_SIZE, pte++, i++) {
		pte_t val = ptep_get(pte);
		struct folio *folio;

		if (l3 && i + l3 < n)
			lru_cast_young_ahead(pte + l3, false);
		if (l3 && i + l1 < n)
			lru_cast_young_ahead(pte + l1, true);

		if (a == addr || !pte_present(val) || pte_protnone(val))
			continue;
		if (!pte_young(val) && !mm_has_notifiers(vma->vm_mm))
			continue;
		folio = vm_normal_folio(vma, a, val);
		/* one read a folio: its other PTEs, and the target's, are passed */
		if (!folio || folio == last || folio == target || !folio_test_lru(folio))
			continue;
		if (!lru_cast_young_take(folio))
			continue;
		if (lru_cast_ptep_clear_young(vma, a, pte)) {
			last = folio;
			lru_cast_young_record(folio);
			atomic_long_inc(&kl_taken);
		}
	}
	lru_cast_lazy_mmu_leave();
	return true;
}
