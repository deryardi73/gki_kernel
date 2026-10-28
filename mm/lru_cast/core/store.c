// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- where the state words live.
 *
 * One 32-bit word per page frame, in an array indexed by PFN: the state
 * word of a folio, lru_cast_states[folio_pfn()], with no lookup and no RCU. Memory
 * comes and goes (ACPI DIMMs, virtio-mem), so the array cannot be sized at
 * boot to what is installed: it is a virtual range reserved for every PFN
 * the machine could have (max_possible_pfn), backed one memory section at a
 * time (PAGES_PER_SECTION words), as the kernel backs its own struct page
 * array. lru_cast_sec_on[] says which sections are backed and is the only
 * authority for "may this word be touched": a section is backed before its
 * memory goes online and let go after it is offline, so nothing is in use
 * while it changes. A PFN past the limit, or in a section that could not be
 * backed, has no word: it reads as zero -- a folio never read -- and its
 * writes go nowhere.
 *
 * Nothing is set up until CAST first runs: booted with lru_cast=0 the
 * array costs nothing, and switched on later it is set up then. Once set
 * up it stays, through CAST going off and on again.
 */
#include <linux/memory.h>
#include <linux/memblock.h>
#include <linux/memory_hotplug.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>
#include "core/store.h"
#include "compat/vmap.h"
#include "version.h"

#define LRU_CAST_SEC_BYTES		(PAGES_PER_SECTION * sizeof(u32))

u32 *lru_cast_states __read_mostly;
unsigned long lru_cast_pfn_limit __read_mostly;
u8 *lru_cast_sec_on __read_mostly;
static unsigned long lru_cast_nr_secs;
static DEFINE_MUTEX(lru_cast_store_lock);

/* back section @sec with zeroed pages */
static int lru_cast_sec_attach(unsigned long sec)
{
	unsigned long addr = (unsigned long)(lru_cast_states + sec * PAGES_PER_SECTION);
	unsigned long n = LRU_CAST_SEC_BYTES >> PAGE_SHIFT, i;
	struct page **pages;
	int err = -ENOMEM;

	if (sec >= lru_cast_nr_secs)
		return -ERANGE;
	if (lru_cast_sec_on[sec])
		return 0;
	pages = kvmalloc_array(n, sizeof(*pages), GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN);
	if (!pages)
		return -ENOMEM;
	for (i = 0; i < n; i++) {
		pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO | __GFP_NORETRY | __GFP_NOWARN);
		if (!pages[i])
			goto free;
	}
	err = lru_cast_vmap_pages(addr, addr + LRU_CAST_SEC_BYTES, pages);
	if (err)
		goto free;
	flush_cache_vmap(addr, addr + LRU_CAST_SEC_BYTES);
	kvfree(pages);
	smp_store_release(&lru_cast_sec_on[sec], 1);
	return 0;
free:
	while (i--)
		__free_page(pages[i]);
	kvfree(pages);
	return err;
}

/* let section @sec go: its memory is offline, no word of it is in use */
static void lru_cast_sec_detach(unsigned long sec)
{
	unsigned long addr = (unsigned long)(lru_cast_states + sec * PAGES_PER_SECTION);
	unsigned long n = LRU_CAST_SEC_BYTES >> PAGE_SHIFT, i;
	struct page **pages;

	if (sec >= lru_cast_nr_secs || !lru_cast_sec_on[sec])
		return;
	pages = kvmalloc_array(n, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return;		/* cannot name the pages: keep them, still backed */
	WRITE_ONCE(lru_cast_sec_on[sec], 0);
	for (i = 0; i < n; i++)
		pages[i] = vmalloc_to_page((void *)addr + i * PAGE_SIZE);
	vunmap_range(addr, addr + LRU_CAST_SEC_BYTES);
	for (i = 0; i < n; i++)
		__free_page(pages[i]);
	kvfree(pages);
}

static int lru_cast_mem_cb(struct notifier_block *nb, unsigned long action, void *arg)
{
	struct memory_notify *mn = arg;
	unsigned long sec = pfn_to_section_nr(mn->start_pfn);
	unsigned long end = pfn_to_section_nr(mn->start_pfn + mn->nr_pages - 1);

	mutex_lock(&lru_cast_store_lock);
	if (lru_cast_states) {
		for (; sec <= end; sec++) {
			if (action == MEM_GOING_ONLINE) {
				if (lru_cast_sec_attach(sec))	/* it cannot refuse: no words */
					pr_warn_ratelimited(LRU_CAST_PROGNAME ": no state words for section %lu, its pages unranked\n",
							    sec);
			} else if (action == MEM_OFFLINE || action == MEM_CANCEL_ONLINE)
				lru_cast_sec_detach(sec);
		}
	}
	mutex_unlock(&lru_cast_store_lock);
	return NOTIFY_OK;
}

static struct notifier_block lru_cast_mem_nb = {
	.notifier_call = lru_cast_mem_cb,
	.priority = DEFAULT_CALLBACK_PRI,
};

/*
 * The words for all the memory there is, before CAST runs: the array set
 * up the first time, then every present section without its words given
 * them -- those a short allocation left out before. Short now, CAST does
 * not come on: a page with no word is never ranked, and running with some
 * would be a failure no one sees. A section brought online later that
 * finds no memory for its words goes without them (it cannot refuse) and
 * says so.
 */
int lru_cast_store_init(void)
{
	struct vm_struct *area;
	unsigned long sec, limit;
	int err = 0;

	get_online_mems();
	mutex_lock(&lru_cast_store_lock);
	if (!lru_cast_states) {
		/*
		 * max_possible_pfn is only set by x86 and memory hotplug; on arm64
		 * it stays 0 (get_vm_area(0) fails: CAST would stay off), so
		 * the end of present memory counts too.
		 */
		limit = ALIGN(max_t(unsigned long, (unsigned long)max_possible_pfn, max_pfn),
			      PAGES_PER_SECTION);
		lru_cast_nr_secs = limit / PAGES_PER_SECTION;
		lru_cast_sec_on = kvzalloc(lru_cast_nr_secs,
					   GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN);
		area = lru_cast_sec_on ? get_vm_area(limit * sizeof(u32), VM_MAP) : NULL;
		if (!area) {
			kvfree(lru_cast_sec_on);
			lru_cast_sec_on = NULL;
			err = -ENOMEM;
			goto out;
		}
		lru_cast_states = area->addr;
		smp_store_release(&lru_cast_pfn_limit, limit);
		pr_info(LRU_CAST_PROGNAME ": state words for %lu PFNs (%lu KiB reserved)\n",
			limit, limit * sizeof(u32) >> 10);
	}
	for (sec = 0; sec < lru_cast_nr_secs; sec++) {
		if (present_section_nr(sec)) {
			err = lru_cast_sec_attach(sec);
			if (err)
				break;
		}
		cond_resched();
	}
out:
	mutex_unlock(&lru_cast_store_lock);
	put_online_mems();
	return err;
}

/*
 * Hotplug is followed from boot, whether or not the array is set up yet:
 * the callback does nothing until it is. Registered here, apart from
 * lru_cast_store_init(): there, under lru_cast_store_lock, it would take the
 * notifier chain's lock inside ours, the callback ours inside the chain's.
 */
static int __init lru_cast_store_hotplug_init(void)
{
	return register_memory_notifier(&lru_cast_mem_nb);
}
subsys_initcall(lru_cast_store_hotplug_init);

/*
 * Every state word to zero, before CAST is switched back on: while it was
 * off nothing reset a freed page's word, and a page in use since then would
 * carry reads from before. CAST is off while this runs, so nothing else
 * writes the words.
 */
void lru_cast_clear_states(void)
{
	unsigned long sec;

	mutex_lock(&lru_cast_store_lock);
	for (sec = 0; lru_cast_states && sec < lru_cast_nr_secs; sec++) {
		if (lru_cast_sec_on[sec])
			memset(lru_cast_states + sec * PAGES_PER_SECTION, 0, LRU_CAST_SEC_BYTES);
		cond_resched();
	}
	mutex_unlock(&lru_cast_store_lock);
}
