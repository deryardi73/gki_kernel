// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- the clock.
 *
 * Time is how much has pressed into a node's memory: the clock counts the
 * pages the node takes in -- a folio new to the cache, or one brought back
 * with its history (policy/ghost.c) -- less those that leave it of
 * themselves, freed by their owners; it ticks every C/4 of them, C what
 * the node's LRUs can hold, their pages and the free. A state word stamps
 * its reads with ticks (core/state.c), and keys decay by them (core/key.c).
 *
 * What comes in either fills free memory or displaces what was there, and
 * either way competes with it; what comes into the room a page left of
 * itself displaces nobody, and a process freeing and taking memory again
 * and again makes no time for the rest. So with memory full the clock is
 * the eviction clock it was -- an evicted page, freed later, is counted
 * back in when it is evicted. Unlike that, it runs while memory fills: a
 * set in use from before and the pages streaming in after are read in
 * different ticks by the time reclaim first has to choose between them.
 * And it stands still while nothing comes in, so that nothing grows old,
 * nor its reads many, while memory is quiet.
 *
 * Pages taken in and freed are counted per CPU and added to the node's
 * count a batch at a time. Whoever paces itself by the clock (the walker,
 * re-protection) is told of each batch by lru_cast_hook_installed(); the clock
 * does not know who.
 */
#include <linux/percpu.h>
#include <linux/sysfs.h>
#include "core/core.h"

#define LRU_CAST_INSTALL_BATCH	64

/* every node's ticks, summed: the controllers' clock */
static atomic_long_t lru_cast_ticks;

/* a CPU's pages taken in less those freed, not yet added to their node's count */
struct lru_cast_pending {
	int nid;
	long nr;
};
static DEFINE_PER_CPU(struct lru_cast_pending, lru_cast_pending) = { NUMA_NO_NODE, 0 };

/* what the node's LRUs can hold: their pages, and the free they may take */
static unsigned long lru_cast_capacity(struct pglist_data *pgdat)
{
	return node_page_state(pgdat, NR_INACTIVE_ANON) + node_page_state(pgdat, NR_ACTIVE_ANON) +
	       node_page_state(pgdat, NR_INACTIVE_FILE) + node_page_state(pgdat, NR_ACTIVE_FILE) +
	       sum_zone_node_page_state(pgdat->node_id, NR_FREE_PAGES);
}

/* @nr pages into node @nid, net: the count, and the tick when it is due */
static void lru_cast_clock_add(int nid, long nr)
{
	struct pglist_data *pgdat = NODE_DATA(nid);
	struct lru_cast_clock *c = &pgdat->lru_cast_clock;
	unsigned long size = lru_cast_capacity(pgdat);
	long installed = atomic_long_add_return(nr, &c->installed);
	unsigned long next = READ_ONCE(c->next);

	lru_cast_hook_installed(nr, size);
	if (installed >= (long)next &&
	    try_cmpxchg(&c->next, &next, installed + max(size / 4, 1UL))) {
		WRITE_ONCE(c->tick, c->tick + 1);
		atomic_long_inc(&lru_cast_ticks);
	}
}

/* @nr pages into node @nid, net, on this CPU's account */
static void lru_cast_clock_count(int nid, long nr)
{
	struct lru_cast_pending *p = get_cpu_ptr(&lru_cast_pending);

	if (p->nid != nid) {
		if (p->nr && p->nid != NUMA_NO_NODE)
			lru_cast_clock_add(p->nid, p->nr);
		p->nid = nid;
		p->nr = 0;
	}
	p->nr += nr;
	if (abs(p->nr) >= LRU_CAST_INSTALL_BATCH) {
		lru_cast_clock_add(nid, p->nr);
		p->nr = 0;
	}
	put_cpu_ptr(&lru_cast_pending);
}

/* @nr pages of @folio's node taken in */
void lru_cast_installed(struct folio *folio, unsigned long nr)
{
	lru_cast_clock_count(folio_nid(folio), nr);
}

/* @nr pages the node had taken in freed */
void lru_cast_released(int nid, unsigned long nr)
{
	lru_cast_clock_count(nid, -(long)nr);
}

/*
 * @nr pages of @type evicted from @pgdat: they will be freed, and counted
 * out then; counted back in now, an eviction is no page leaving of itself
 */
void lru_cast_evicted(struct pglist_data *pgdat, int type, unsigned long nr)
{
	/* racy across the node's lruvecs: an approximate count is all it is for */
	data_race(pgdat->lru_cast_clock.evict += nr);
	lru_cast_clock_count(pgdat->node_id, nr);
}

unsigned long lru_cast_ticks_now(void)
{
	return atomic_long_read(&lru_cast_ticks);
}

int lru_cast_clock_show(char *buf)
{
	int nid, len = 0;

	for_each_node_state(nid, N_MEMORY) {
		struct pglist_data *pgdat = NODE_DATA(nid);

		len += sysfs_emit_at(buf, len, "node%d installed %ld evicted %lu tick %lu\n", nid,
				     atomic_long_read(&pgdat->lru_cast_clock.installed),
				     data_race(pgdat->lru_cast_clock.evict),
				     READ_ONCE(pgdat->lru_cast_clock.tick));
	}
	return len;
}
