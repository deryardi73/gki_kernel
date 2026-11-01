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
 * does not know who. At each tick a part of the node's state words is
 * looked over, and those read last long ago are marked stale (core.h).
 */
#include <linux/ktime.h>
#include <linux/memory_hotplug.h>
#include <linux/percpu.h>
#include <linux/swap.h>
#include <linux/sysfs.h>
#include "core/core.h"

#define LRU_CAST_INSTALL_BATCH	64

/* every node's ticks, summed: the controllers' clock */
static atomic_long_t lru_cast_ticks;

/* words marked stale, and the time it took */
static atomic_long_t lru_cast_stale_marked;
static atomic64_t lru_cast_stale_ns;

static void lru_cast_stale_workfn(struct work_struct *work);
static DECLARE_WORK(lru_cast_stale_work, lru_cast_stale_workfn);

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
		int type;

		for (type = 0; type < ANON_AND_FILE; type++) {
			data_race(c->shadowed[type] -= c->shadowed[type] / 4);
			data_race(c->returned[type] -= c->returned[type] / 4);
		}
		WRITE_ONCE(c->tick, c->tick + 1);
		atomic_long_inc(&lru_cast_ticks);
		lru_cast_queue_work(&lru_cast_stale_work);
	}
}

/*
 * Each node's words, a part at each tick, marked stale or deep where their
 * last read is old (core.h): 1/LRU_CAST_STALE_PASS of the node's PFNs for
 * each tick it has gone on since, all of them at most, so that a word is
 * read once in LRU_CAST_STALE_PASS ticks, a tick more for the part left at
 * the end. Marked from LRU_CAST_STALE_AGE ticks on, it is marked well
 * before its read is 256 ticks old; deep, before its unit wraps.
 */
static void lru_cast_stale_workfn(struct work_struct *work)
{
	u64 t0 = ktime_get_ns();
	int nid;

	BUILD_BUG_ON(LRU_CAST_STALE_UNIT <= LRU_CAST_STALE_PASS + 1);	/* a word seen in each unit */
	if (!lru_cast_enabled())
		return;
	get_online_mems();
	for_each_node_state(nid, N_MEMORY) {
		struct pglist_data *pgdat = NODE_DATA(nid);
		struct lru_cast_clock *c = &pgdat->lru_cast_clock;
		unsigned long tick = READ_ONCE(c->tick);
		unsigned long start = pgdat->node_start_pfn, end = pgdat_end_pfn(pgdat);
		unsigned long part = DIV_ROUND_UP(end - start, LRU_CAST_STALE_PASS);
		unsigned long n = min(tick - c->stale_tick, (unsigned long)LRU_CAST_STALE_PASS);

		c->stale_tick = tick;
		if (c->stale_pfn < start || c->stale_pfn >= end)
			c->stale_pfn = start;
		while (n--) {
			unsigned long stop = min(end, c->stale_pfn + part);

			atomic_long_add(lru_cast_mark_stale(c->stale_pfn, stop, tick),
					&lru_cast_stale_marked);
			c->stale_pfn = stop < end ? stop : start;
		}
	}
	put_online_mems();
	atomic64_add(ktime_get_ns() - t0, &lru_cast_stale_ns);
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

/* @nr pages of @type evicted from @pgdat with a shadow */
void lru_cast_shadowed(struct pglist_data *pgdat, int type, unsigned long nr)
{
	data_race(pgdat->lru_cast_clock.shadowed[type] += nr);
}

/* @nr pages of @type faulted back into @pgdat within the ghosts' window */
void lru_cast_returned(struct pglist_data *pgdat, int type, unsigned long nr)
{
	data_race(pgdat->lru_cast_clock.returned[type] += nr);
}

/*
 * Too many of a type's pages isolated for a direct reclaimer to take
 * more (lru_cast.h): of the pages in flight, those that will come back,
 * the share r of evictions faulted back within the ghosts' window, would
 * be more than a batch; an eighth of one for a caller that may do IO and
 * FS, as upstream gives GFP_NOIO and GFP_NOFS callers eight times the
 * room. Isolated * r > batch, without the division.
 */
bool lru_cast_too_many_isolated(struct pglist_data *pgdat, int type, unsigned long isolated,
				bool io_fs)
{
	struct lru_cast_clock *c = &pgdat->lru_cast_clock;
	unsigned long out = data_race(c->shadowed[type]);
	unsigned long back = min(data_race(c->returned[type]), out);
	unsigned long batch = io_fs ? SWAP_CLUSTER_MAX / 8 : SWAP_CLUSTER_MAX;

	return isolated * back > batch * out;
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

		struct lru_cast_clock *c = &pgdat->lru_cast_clock;

		len += sysfs_emit_at(buf, len, "node%d installed %ld evicted %lu tick %lu\n", nid,
				     atomic_long_read(&c->installed), data_race(c->evict),
				     READ_ONCE(c->tick));
		len += sysfs_emit_at(buf, len, "node%d returned anon %lu/%lu file %lu/%lu\n", nid,
				     data_race(c->returned[0]), data_race(c->shadowed[0]),
				     data_race(c->returned[1]), data_race(c->shadowed[1]));
	}
	return len + sysfs_emit_at(buf, len, "stale marked %ld us %llu\n",
				   atomic_long_read(&lru_cast_stale_marked),
				   atomic64_read(&lru_cast_stale_ns) / 1000);
}
