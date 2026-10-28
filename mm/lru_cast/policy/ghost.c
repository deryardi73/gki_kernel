// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- the history of evicted folios.
 *
 * An evicted folio leaves a shadow entry in the page cache or swap cache
 * (mm/workingset.c); on a CAST node the shadow carries the folio's state
 * word, the tick of the clock (core/clock.c) it was evicted at, its node
 * and whether it had been read more than once, over the bit that marks it
 * CAST's:
 *
 *	| state word 32 | tick 16 | node NODES_SHIFT | workingset 1 | kind 1 |
 *
 * The tick wraps after 65536 ticks, 16384 turnovers of the node's memory;
 * a shadow that old comes back as recent, and only then.
 *
 * A folio faulted back within LRU_CAST_GHOST_TICKS (8C pages taken in) gets its
 * state word back, its reads since it entered the cache with it, and the
 * fault counts as one more read: the model's ghost. Later than that the
 * history is too old to rank by and the folio starts afresh.
 *
 * Legacy reclaim measures a refault against the size of the workingset to
 * tell whether the eviction was a mistake; CAST asks its own question --
 * kept, would the folio's key, its history alone, rank above the bucket
 * reclaim is taking from? -- and answers it with the same counters and flags
 * (WORKINGSET_ACTIVATE, PG_active, and for a folio read more than once
 * WORKINGSET_RESTORE, PG_workingset and the refault cost that
 * get_scan_count() weighs).
 *
 * A shadow is read as CAST's while CAST runs and as legacy's otherwise;
 * one left by the other side across a switch is told by its kind bit
 * (LRU_CAST_SHADOW_KIND) and taken as no history.
 */
#include <linux/swap.h>
#include <linux/xarray.h>
#include "policy/policy.h"

#define LRU_CAST_GHOST_TICKS	32

#define LRU_CAST_SHADOW_WS		1
#define LRU_CAST_SHADOW_TICK	16
#define LRU_CAST_SHADOW_STATE	32

static void *lru_cast_pack(u32 w, u16 tick, int nid, bool workingset)
{
	unsigned long v = w;

	BUILD_BUG_ON(LRU_CAST_SHADOW_STATE + LRU_CAST_SHADOW_TICK + NODES_SHIFT +
		     LRU_CAST_SHADOW_WS + LRU_CAST_SHADOW_KIND > BITS_PER_XA_VALUE);
	v = (v << LRU_CAST_SHADOW_TICK) | tick;
	v = (v << NODES_SHIFT) | nid;
	v = (v << LRU_CAST_SHADOW_WS) | workingset;
	v = (v << LRU_CAST_SHADOW_KIND) | 1;
	return xa_mk_value(v);
}

static void lru_cast_unpack(void *shadow, u32 *w, u16 *tick, int *nid, bool *workingset)
{
	unsigned long v = xa_to_value(shadow) >> LRU_CAST_SHADOW_KIND;

	*workingset = v & 1;
	v >>= LRU_CAST_SHADOW_WS;
	*nid = v & ((1UL << NODES_SHIFT) - 1);
	v >>= NODES_SHIFT;
	*tick = v & 0xffff;
	v >>= LRU_CAST_SHADOW_TICK;
	*w = v;
}

/* the shadow's history if it is still worth having, else false */
static bool lru_cast_ghost(void *shadow, int nid, int type, u32 *w, bool *workingset)
{
	u16 tick;
	int snid;

	*workingset = false;
	if (!lru_cast_shadow(shadow))
		return false;
	lru_cast_unpack(shadow, w, &tick, &snid, workingset);
	if (snid != nid || !node_online(snid) || !(*w >> 24))
		return false;
	return (u16)(lru_cast_tick(NODE_DATA(snid), type) - tick) <= LRU_CAST_GHOST_TICKS;
}

void *lru_cast_eviction(struct folio *folio)
{
	struct pglist_data *pgdat = folio_pgdat(folio);
	u32 w = lru_cast_read(folio);

	return lru_cast_pack(w, lru_cast_tick(pgdat, folio_is_file_lru(folio)), pgdat->node_id,
			  (w >> 24) > 1);
}

bool lru_cast_test_recent(void *shadow, bool file, bool *workingset)
{
	int nid = (xa_to_value(shadow) >> (LRU_CAST_SHADOW_KIND + LRU_CAST_SHADOW_WS)) &
		  ((1UL << NODES_SHIFT) - 1);
	u32 w;

	return lru_cast_ghost(shadow, nid, file, &w, workingset);
}

void lru_cast_refault(struct folio *folio, void *shadow)
{
	int type = folio_is_file_lru(folio);
	long nr = folio_nr_pages(folio), kept, victim;
	struct lruvec *lruvec;
	bool workingset;
	u32 w;

	/* the folio is locked: its memcg is held, its lruvec read under RCU */
	rcu_read_lock();
	lruvec = folio_lruvec(folio);
	mod_lruvec_state(lruvec, WORKINGSET_REFAULT_BASE + type, nr);
	if (!lru_cast_ghost(shadow, folio_nid(folio), type, &w, &workingset))
		goto out;

	/*
	 * The history back, and the question asked of it before the fault
	 * adds its read: kept, would the folio rank above what reclaim takes
	 * now? With this read counted it would, for anything just read ranks
	 * above M's lowest. With no bucket taken from yet there is nothing to
	 * ask it against.
	 */
	lru_cast_write(folio, w);
	lru_cast_installed(folio, nr);		/* taken in again: the clock counts it */
	kept = lru_cast_bucket(folio, type);
	victim = READ_ONCE(lruvec->lru_cast[type].victim);
	lru_cast_access(folio);
	folio_set_referenced(folio);

	if (victim == LONG_MIN || kept <= victim)
		goto out;
	folio_set_active(folio);
	mod_lruvec_state(lruvec, WORKINGSET_ACTIVATE_BASE + type, nr);
	if (workingset) {
		folio_set_workingset(folio);
		lru_cast_note_refault_cost(folio);
		mod_lruvec_state(lruvec, WORKINGSET_RESTORE_BASE + type, nr);
	}
out:
	rcu_read_unlock();
}

/*
 * CAST coming back on: the shadows it left before it went off are older
 * than their ticks say, the clocks having stood still meanwhile, and their
 * folios' state words have been cleared. Every node's clock moves past the
 * ghosts' window, and those shadows are taken as no history.
 */
void lru_cast_ghosts_forget(void)
{
	int nid;

	for_each_online_node(nid) {
		struct lru_cast_clock *c = &NODE_DATA(nid)->lru_cast_clock;

		WRITE_ONCE(c->tick, c->tick + LRU_CAST_GHOST_TICKS + 1);
	}
}
