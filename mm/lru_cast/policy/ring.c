// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- keys, and M's lowest found by a sweep.
 *
 * Each type of an lruvec is a cache of two parts: W, the inactive list, a
 * FIFO window for newcomers; and M, the active list, in no order.
 *
 * A folio of M carries its owner tag in its state word (core/state.c):
 * its lruvec's number in the node and its type, 0 if not in M -- set as it enters M,
 * cleared as reclaim takes it, as it goes to W or becomes unevictable, and
 * as the page is freed. A folio isolated by others keeps its tag until it
 * comes back: the tag says where to look, the folio itself is asked before
 * it is taken. A stale tag costs more than a wasted candidate: its block
 * is never found empty.
 *
 * The tag has 10 bits: an lruvec is given a number in its node as its first
 * folio comes into M, one of LRU_CAST_LIDS - 2, and gives it back when it
 * is freed. Past that many in a node, the rest share the last number, and
 * their folios of M are told apart by CAST's own number of their lruvec
 * (lru_cast.tag) in a table by PFN beside the array, lru_cast_spill: a
 * sweep of a spilled lruvec looks there for each folio of the shared number
 * it meets. A folio of the shared number the table has nothing for (a
 * split's tail, a store that failed) is taken for the sweeper's; whatever
 * the table holds, the folio itself is asked before it is taken.
 *
 * Reclaim takes M's lowest key (core/key.c), counted in sixteenths of a
 * half-life. It finds it by sweeping the node's state words in PFN order
 * from a cursor that wraps, and for each of this lruvec's M taking the
 * key of the word; it keeps the LRU_CAST_CANDS lowest keys
 * as candidates (PFN, key, the word as read). A sweep stops once it holds
 * that many all at or below tau, the 1/16 quantile of the keys the last
 * full pass read (a sample of them) -- before any pass, of the keys of
 * folios of M drawn at random from the node (lru_cast_tau_probe()): the
 * cursor starts where the node does, and the folios there, the first to
 * come in, would otherwise be taken as the lowest of the few read, a
 * working set allocated at once among them; until there is tau, or after
 * LRU_CAST_SWEEP_MAX frames, it keeps the lowest it has read -- or, having
 * found none, reads on to LRU_CAST_SWEEP_EMPTY: a small memcg's folios lie
 * in a few places of the node. A candidate is
 * taken only if it is still a folio of M here and its word is as read: one
 * read since has a higher key and waits for the next pass.
 *
 * A newcomer from W's tail below the lowest candidate goes in front of the
 * candidates, first to be taken: W's tail meeting M's lowest, the lower
 * goes.
 *
 * A memcg taken offline may hand its LRU folios to its parent's lruvec by
 * splicing its lists (lru_cast_ring_reparent()), and its folios of M come
 * with its number. The number is then the node's to give back, not the
 * lruvec's: it is marked moved, and a work item (lru_cast_rehome_fn()),
 * out of the locks the splice holds, gives each folio of M carrying it the
 * tag of the lruvec it is on now, before the number is given back. Until
 * then a sweep of the parent does not find them, as if they had come in
 * late.
 *
 * Everything here runs under the lruvec's lru_lock but the work.
 */
#include <linux/hash.h>
#include <linux/sort.h>
#include <linux/xarray.h>
#include <linux/workqueue.h>
#include "policy/policy.h"

#define LRU_CAST_RES		4	/* sixteenths of a half-life */
#define LRU_CAST_SWEEP_MAX		4096	/* frames a sweep reads at most */
#define LRU_CAST_SWEEP_EMPTY	65536	/* ... or, with nothing found yet, this many */
#define LRU_CAST_TAU_SHIFT		4	/* tau: the 1/16 quantile */
#define LRU_CAST_PROBES		4096	/* frames drawn at most for tau before a pass */
#define LRU_CAST_PROBE_MIN		32	/* ... and the keys it takes */
#define LRU_CAST_REHOME_SCAN	(1UL << 18)	/* frames the work reads between rests */

/* the number the lruvecs past the node's numbers share */
#ifndef LRU_CAST_LID_SPILL
#define LRU_CAST_LID_SPILL		(LRU_CAST_LIDS - 1)
#endif

/* their folios of M by PFN: their lruvecs' tag, type (as lru_cast_spilled()) */
static DEFINE_XARRAY_FLAGS(lru_cast_spill, XA_FLAGS_LOCK_IRQ);

/* what the sweeps did, for sysfs (lru_cast/sweep): counted under the lru_lock, per CPU */
enum { SW_SWEEPS, SW_FRAMES, SW_WORDS, SW_TAKEN, SW_STALE, SW_GATE, SW_FALLBACK, SW_SPILLED, SW_PROBED, SW_NR };
static DEFINE_PER_CPU(unsigned long [SW_NR], lru_cast_sw);

static inline void sw_add(int i, unsigned long n)
{
	__this_cpu_add(lru_cast_sw[i], n);
}

long lru_cast_bucket(struct folio *folio, int type)
{
	return lru_cast_key_res(lru_cast_read(folio), lru_cast_tick(folio_pgdat(folio), type),
			     lru_cast_param_le(type), LRU_CAST_RES);
}

static struct list_head *lru_cast_m(struct lruvec *lruvec, int type)
{
	return &lruvec->lists[LRU_ACTIVE_ANON + type * LRU_FILE];
}

/*
 * @lruvec's number in its node, given on first use: the lowest free of
 * 1 .. LRU_CAST_LID_SPILL - 1, or LRU_CAST_LID_SPILL, shared, if none is. Not the
 * memcg's ID, which a memcg removed with pages left gives up, to 0.
 */
static u32 lru_cast_lid(struct lruvec *lruvec)
{
	struct lru_cast *k = &lruvec->lru_cast[0];
	unsigned long *ids = lruvec_pgdat(lruvec)->lru_cast_clock.lids;
	unsigned int id;

	BUILD_BUG_ON(LRU_CAST_LIDS > 1U << (LRU_CAST_OWNER_BITS - 1));
	if (likely(k->lid))
		return k->lid;
	do {
		id = find_next_zero_bit(ids, LRU_CAST_LID_SPILL, 1);
	} while (id < LRU_CAST_LID_SPILL && test_and_set_bit(id, ids));
	if (id == LRU_CAST_LID_SPILL)
		sw_add(SW_SPILLED, 1);
	k->lid = id;
	return id;
}

/* the owner tag of @lruvec's M of @type: its number, its type; never 0 */
static u32 lru_cast_owner(struct lruvec *lruvec, int type)
{
	return lru_cast_lid(lruvec) << 1 | type;
}

static bool lru_cast_spilled(u32 owner)
{
	return owner >> 1 == LRU_CAST_LID_SPILL;
}

/* who a folio of a spilled M belongs to: CAST's number of its lruvec, its type */
static unsigned long lru_cast_spill_owner(struct lruvec *lruvec, int type)
{
	return (unsigned long)lruvec->lru_cast[0].tag << 1 | type;
}

/* the table's entry for @pfn made @v, or none for NULL; failing, left */
static void lru_cast_spill_store(unsigned long pfn, void *v)
{
	unsigned long flags;

	xa_lock_irqsave(&lru_cast_spill, flags);
	__xa_store(&lru_cast_spill, pfn, v, GFP_ATOMIC | __GFP_NOWARN);
	xa_unlock_irqrestore(&lru_cast_spill, flags);
}

/* is the folio at @pfn, of the shared number, @lruvec's of @type, as the table tells */
static bool lru_cast_spill_ours(unsigned long pfn, struct lruvec *lruvec, int type)
{
	void *v = xa_load(&lru_cast_spill, pfn);

	return !v || xa_to_value(v) == lru_cast_spill_owner(lruvec, type);
}

/*
 * The blocks of the node, as @k has them: LRU_CAST_BLOCKS of 2^shift
 * PFNs, at least 512 (a PMD). A block's bit is set as a folio of M comes
 * into it and cleared once a sweep reads all of it and finds none; the
 * node grown or shrunk, the blocks are drawn again, all set.
 */
static unsigned int lru_cast_blocks(struct lruvec *lruvec, struct lru_cast *k)
{
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	unsigned long span = pgdat_end_pfn(pgdat) - pgdat->node_start_pfn;
	unsigned int shift = max_t(int, 9, fls64(span ? span - 1 : 0) - ilog2(LRU_CAST_BLOCKS));

	if (k->block_shift != shift) {
		bitmap_fill(k->blocks, LRU_CAST_BLOCKS);
		k->block_shift = shift;
	}
	return shift;
}

/* @folio into M of @type: its tag, and its block */
static void lru_cast_m_enter(struct lruvec *lruvec, struct folio *folio, int type)
{
	struct lru_cast *k = &lruvec->lru_cast[type];
	unsigned int shift = lru_cast_blocks(lruvec, k);
	u32 owner = lru_cast_owner(lruvec, type);

	lru_cast_set_owner(folio, owner);
	if (lru_cast_spilled(owner))
		lru_cast_spill_store(folio_pfn(folio),
				     xa_mk_value(lru_cast_spill_owner(lruvec, type)));
	else if (!xa_empty(&lru_cast_spill) && xa_load(&lru_cast_spill, folio_pfn(folio)))
		/* the page's, before it was freed */
		lru_cast_spill_store(folio_pfn(folio), NULL);
	__set_bit((folio_pfn(folio) - lruvec_pgdat(lruvec)->node_start_pfn) >> shift, k->blocks);
}

void lru_cast_ring_init(struct lruvec *lruvec)
{
	static atomic_t next;

	lruvec->lru_cast[0].tag = atomic_inc_return(&next);
	lruvec->lru_cast[0].lid = 0;
}

/* its number back to the node; no folio is left with it */
void lru_cast_ring_exit(struct lruvec *lruvec)
{
	u16 lid = lruvec->lru_cast[0].lid;

	if (lid && lid != LRU_CAST_LID_SPILL)
		clear_bit(lid, lruvec_pgdat(lruvec)->lru_cast_clock.lids);
	lruvec->lru_cast[0].lid = 0;
}

static void lru_cast_rehome_fn(struct work_struct *work);
static DECLARE_WORK(lru_cast_rehome_work, lru_cast_rehome_fn);

/*
 * @child's folios go to @parent's lists, with both lru_locks held: its
 * number goes with them, marked moved, for the work to retag them by
 */
void lru_cast_ring_reparent(struct lruvec *child, struct lruvec *parent)
{
	u16 lid = child->lru_cast[0].lid;

	if (!lid)
		return;
	child->lru_cast[0].lid = 0;
	set_bit(lid, lruvec_pgdat(child)->lru_cast_clock.lids_moved);
	lru_cast_queue_work(&lru_cast_rehome_work);
}

/*
 * The folio at @pfn, if it is of M of @type: tagged as of the lruvec it is
 * on now. Its lruvec is not known until it is held and locked.
 */
static void lru_cast_rehome(unsigned long pfn, int type)
{
	struct page *page = pfn_to_online_page(pfn);
	struct lruvec *lruvec;
	struct folio *folio;

	if (!page || PageTail(page))
		return;
	folio = page_folio(page);
	if (!folio_try_get(folio))
		return;
	if (folio_pfn(folio) == pfn) {
		lruvec = folio_lruvec_lock_irq(folio);
		if (lru_cast_on(lruvec) && folio_test_lru(folio) && folio_test_active(folio) &&
		    !folio_test_unevictable(folio) && folio_is_file_lru(folio) == type)
			lru_cast_m_enter(lruvec, folio, type);
		lru_cast_lruvec_unlock_irq(lruvec);
	}
	folio_put(folio);
}

/*
 * Every node's moved numbers: their folios of M retagged, then the numbers
 * given back. A folio isolated meanwhile keeps the number until it is put
 * back and tagged afresh; one found then by an lruvec given the number
 * again is not its folio, and the sweep's check passes it over. A spilled
 * lruvec moved (the shared number marked) has its folios found by the
 * table: each is told again whose it is.
 */
static void lru_cast_rehome_fn(struct work_struct *work)
{
	DECLARE_BITMAP(moved, LRU_CAST_LIDS);
	int nid, i;

	for_each_online_node(nid) {
		struct pglist_data *pgdat = NODE_DATA(nid);
		unsigned long *lids = pgdat->lru_cast_clock.lids;
		unsigned long pfn = pgdat->node_start_pfn, end = pgdat_end_pfn(pgdat);
		unsigned long idx;
		bool spilled;
		void *v;
		u32 owner;

		for (i = 0; i < BITS_TO_LONGS(LRU_CAST_LIDS); i++)
			moved[i] = xchg(&pgdat->lru_cast_clock.lids_moved[i], 0);
		spilled = __test_and_clear_bit(LRU_CAST_LID_SPILL, moved);
		if (!bitmap_empty(moved, LRU_CAST_LIDS)) {
			while (pfn < end) {
				unsigned long lim = min(end, pfn + LRU_CAST_REHOME_SCAN);

				pfn = lru_cast_find_owners(pfn, lim, moved, &owner);
				if (pfn < lim)
					lru_cast_rehome(pfn++, owner & 1);
				cond_resched();
			}
			for_each_set_bit(i, moved, LRU_CAST_LIDS)
				clear_bit(i, lids);
		}
		if (spilled) {
			xa_for_each_range(&lru_cast_spill, idx, v, pgdat->node_start_pfn, end - 1) {
				lru_cast_rehome(idx, xa_to_value(v) & 1);
				cond_resched();
			}
		}
	}
}

/* the key of @folio, taken now; the mark comes off first (see lru_cast_record()) */
static long lru_cast_key_now(struct folio *folio, unsigned long tick, int le)
{
	folio_clear_rekey(folio);
	smp_mb__after_atomic();
	return lru_cast_key_res(lru_cast_read(folio), tick, le, LRU_CAST_RES);
}

/*
 * A key ranked among its equals. Folios with one key -- read
 * in the same ticks, the same number of times -- are taken in the order a
 * sweep meets them, a cursor's over PFNs: a clock, a folio read again
 * since it was found skipped. Of folios read more than once that is a loop's
 * order, and a loop over more than M is served best by keeping the same
 * part of itself: those are ranked by a hash of the PFN, the same of them
 * going first each time. A folio read once keeps the clock's order. The
 * rank is the key times 2^16, plus the hash; the key is the rank >> 16.
 */
static long lru_cast_rank(long key, unsigned long pfn, u32 w)
{
	if (key == LONG_MIN)
		return LONG_MIN;
	return key * 65536 + ((w >> 24) >= 2 ? hash_long(pfn, 16) : 0);
}

static long lru_cast_unrank(long rank)
{
	return rank == LONG_MIN ? LONG_MIN : rank >> 16;
}

static void lru_cast_flush(struct lru_cast *k)
{
	k->ncand = k->pos = 0;
	k->seen = 0;
	k->tau_ok = false;
}

/* the decay changed: the keys are in other units, the candidates void */
void lru_cast_ring_prepare(struct lruvec *lruvec, int type)
{
	struct lru_cast *k = &lruvec->lru_cast[type];
	int le = lru_cast_param_le(type);

	if (k->ring_le != le) {
		lru_cast_flush(k);
		k->ring_le = le;
	}
}

/* a folio into M; the caller does the size accounting */
void lru_cast_ring_put(struct lruvec *lruvec, struct folio *folio, int type)
{
	list_add(&folio->lru, lru_cast_m(lruvec, type));
	lru_cast_m_enter(lruvec, folio, type);
}

/* a folio out of M: taken by reclaim, gone to W, or unevictable */
void lru_cast_ring_out(struct folio *folio)
{
	if (lru_cast_spilled(lru_cast_set_owner(folio, 0)))
		lru_cast_spill_store(folio_pfn(folio), NULL);
}

/*
 * A newcomer from W's tail joins M. Below the lowest candidate -- below
 * tau when there is none -- it goes in front of them; the highest
 * candidate gives way if they are full.
 */
void lru_cast_ring_admit(struct lruvec *lruvec, struct folio *folio, int type)
{
	struct lru_cast *k = &lruvec->lru_cast[type];
	long b = lru_cast_key_now(folio, lru_cast_tick(lruvec_pgdat(lruvec), type), k->ring_le);
	u32 w = lru_cast_read(folio);
	/*
	 * Read more than once, it is ranked as M's are; read once, by its key
	 * alone: of one key, the newcomer is the one read last.
	 */
	bool ranked = (w >> 24) >= 2;
	long r = lru_cast_rank(b, folio_pfn(folio), w), x = ranked ? r : b;
	struct lru_cast_cand *c;

	list_add(&folio->lru, lru_cast_m(lruvec, type));
	lru_cast_m_enter(lruvec, folio, type);
	if (k->pos < k->ncand) {
		long lo = k->cand[k->pos].key;

		if (x >= (ranked ? lo : lru_cast_unrank(lo)))
			return;
	} else if (!k->tau_ok || x > (ranked ? k->tau : lru_cast_unrank(k->tau))) {
		return;
	}
	if (k->pos == k->ncand)
		k->pos = k->ncand = 0;
	if (k->pos) {
		k->pos--;
	} else {
		if (k->ncand < LRU_CAST_CANDS)
			k->ncand++;
		memmove(&k->cand[1], &k->cand[0], (k->ncand - 1) * sizeof(k->cand[0]));
	}
	c = &k->cand[k->pos];
	c->pfn = folio_pfn(folio);
	c->key = r;
	c->word = w;
	sw_add(SW_GATE, 1);
}

static int lru_cast_cmp_cand(const void *a, const void *b)
{
	long x = ((const struct lru_cast_cand *)a)->key, y = ((const struct lru_cast_cand *)b)->key;

	return x < y ? -1 : x > y;
}

static int lru_cast_cmp_long(const void *a, const void *b)
{
	long x = *(const long *)a, y = *(const long *)b;

	return x < y ? -1 : x > y;
}

/* the candidates as a max-heap on the key: one more in, the highest out if full */
static void lru_cast_heap_put(struct lru_cast *k, unsigned long pfn, long key, u32 word)
{
	struct lru_cast_cand *h = k->cand, x = { pfn, key, word };
	unsigned int i, n = k->ncand;

	if (n < LRU_CAST_CANDS) {
		for (i = k->ncand++; i && h[(i - 1) / 2].key < key; i = (i - 1) / 2)
			h[i] = h[(i - 1) / 2];
		h[i] = x;
		return;
	}
	if (key >= h[0].key)
		return;
	for (i = 0;;) {
		unsigned int l = 2 * i + 1, m = l;

		if (l >= n)
			break;
		if (l + 1 < n && h[l + 1].key > h[l].key)
			m = l + 1;
		if (h[m].key <= key)
			break;
		h[i] = h[m];
		i = m;
	}
	h[i] = x;
}

/* is the page at @pfn the head of a folio of M of @lruvec, as far as its flags tell */
static struct page *lru_cast_m_page(struct lruvec *lruvec, int type, unsigned long pfn)
{
	struct page *page = pfn_to_online_page(pfn);
	unsigned long flags;

	if (!page || PageTail(page))
		return NULL;
	flags = lru_cast_page_flags(page);
	if (!(flags & BIT(PG_lru)) || !(flags & BIT(PG_active)) || (flags & BIT(PG_unevictable)))
		return NULL;
	if ((int)!(flags & BIT(PG_swapbacked)) != type)	/* a file folio is not swap-backed */
		return NULL;
#ifdef CONFIG_MEMCG
	if (!lru_cast_page_memcg_is(page, lruvec_memcg(lruvec)))
		return NULL;
#endif
	return page;
}

static u32 lru_cast_rnd(struct lru_cast *k)
{
	k->rnd = k->rnd * 1664525 + 1013904223;
	return k->rnd;
}

/*
 * tau with no pass read yet: the keys of folios of @lruvec's M of @type
 * at frames drawn at random from the blocks that may hold them, each frame
 * of those blocks as likely, so each folio of M as likely. The pass then
 * samples afresh.
 */
static void lru_cast_tau_probe(struct lruvec *lruvec, int type, unsigned int shift,
			    unsigned long tick, int le)
{
	struct lru_cast *k = &lruvec->lru_cast[type];
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	unsigned long start = pgdat->node_start_pfn, end = pgdat_end_pfn(pgdat);
	unsigned int nb = min_t(unsigned long, LRU_CAST_BLOCKS, ((end - start - 1) >> shift) + 1);
	u32 owner = lru_cast_owner(lruvec, type);
	unsigned int i, n = 0;

	for (i = 0; i < LRU_CAST_PROBES && n < LRU_CAST_SAMPLES; i++) {
		unsigned int b = ((u64)lru_cast_rnd(k) * nb) >> 32;
		unsigned long pfn = start + ((unsigned long)b << shift) +
				    (lru_cast_rnd(k) & ((1UL << shift) - 1));
		u32 w;

		if (!test_bit(b, k->blocks) || pfn >= end ||
		    lru_cast_find_owner(pfn, pfn + 1, owner, &w) != pfn || !(w >> 24))
			continue;
		if (lru_cast_spilled(owner) && !lru_cast_spill_ours(pfn, lruvec, type))
			continue;
		k->sample[n++] = lru_cast_rank(lru_cast_key_res(w, tick, le, LRU_CAST_RES), pfn, w);
	}
	k->seen = 0;
	if (n < LRU_CAST_PROBE_MIN)
		return;
	sort(k->sample, n, sizeof(long), lru_cast_cmp_long, NULL);
	k->tau = k->sample[(n - 1) >> LRU_CAST_TAU_SHIFT];
	k->tau_ok = true;
	sw_add(SW_PROBED, 1);
}

static void lru_cast_sweep(struct lruvec *lruvec, int type)
{
	struct lru_cast *k = &lruvec->lru_cast[type];
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	unsigned long start = pgdat->node_start_pfn, end = pgdat_end_pfn(pgdat);
	unsigned long tick = lru_cast_tick(pgdat, type), frames = 0, words = 0;
	u32 owner = lru_cast_owner(lruvec, type);
	bool spilled = lru_cast_spilled(owner);
	unsigned int shift = lru_cast_blocks(lruvec, k), nb = LRU_CAST_BLOCKS;
	long clean = -1;			/* a block read from its start, none found yet */
	int le = k->ring_le, wraps = 0;

	k->ncand = k->pos = 0;
	if (end <= start)
		return;
	if (k->cursor < start || k->cursor > end)
		k->cursor = start;
	if (!k->tau_ok)
		lru_cast_tau_probe(lruvec, type, shift, tick, le);
	for (;;) {
		unsigned long pfn, budget = k->ncand ? LRU_CAST_SWEEP_MAX : LRU_CAST_SWEEP_EMPTY;
		long key;
		u32 w;

		if (frames >= budget)
			break;
		if (k->tau_ok && k->ncand == LRU_CAST_CANDS && k->cand[0].key <= k->tau)
			break;
		if (k->cursor >= end) {			/* a pass ends: tau from its keys */
			if (++wraps > 2)		/* no block left that may hold M */
				break;
			k->cursor = start;
			if (k->seen) {
				unsigned int m = min_t(unsigned long, k->seen, LRU_CAST_SAMPLES);

				sort(k->sample, m, sizeof(long), lru_cast_cmp_long, NULL);
				k->tau = k->sample[(m - 1) >> LRU_CAST_TAU_SHIFT];
				k->tau_ok = true;
				k->seen = 0;
			}
		}
		{
			unsigned long b = (k->cursor - start) >> shift, bstart, bend, lim;

			if (!test_bit(b, k->blocks)) {	/* none of M there: the next block that may */
				b = find_next_bit(k->blocks, nb, b);
				k->cursor = b >= nb ? end : start + (b << shift);
				continue;
			}
			bstart = start + (b << shift);
			bend = min(end, bstart + (1UL << shift));
			if (k->cursor == bstart)
				clean = b;
			lim = min(bend, k->cursor + budget - frames);
			pfn = lru_cast_find_owner(k->cursor, lim, owner, &w);
			frames += pfn - k->cursor;
			k->cursor = pfn;
			if (pfn >= lim) {
				if (lim == bend && clean == b)
					__clear_bit(b, k->blocks);
				continue;
			}
			if (spilled && !lru_cast_spill_ours(pfn, lruvec, type)) {
				k->cursor++;		/* another spilled lruvec's */
				frames++;
				continue;
			}
			clean = -1;
		}
		k->cursor++;
		frames++;
		if (!(w >> 24))
			continue;
		words++;
		key = lru_cast_rank(lru_cast_key_res(w, tick, le, LRU_CAST_RES), pfn, w);
		/* a reservoir sample of the pass's keys */
		if (k->seen < LRU_CAST_SAMPLES) {
			k->sample[k->seen] = key;
		} else {
			u64 r = (u64)lru_cast_rnd(k) * (k->seen + 1) >> 32;

			if (r < LRU_CAST_SAMPLES)
				k->sample[r] = key;
		}
		k->seen++;
		lru_cast_heap_put(k, pfn, key, w);
	}
	sort(k->cand, k->ncand, sizeof(k->cand[0]), lru_cast_cmp_cand, NULL);
	sw_add(SW_SWEEPS, 1);
	sw_add(SW_FRAMES, frames);
	sw_add(SW_WORDS, words);
}

/*
 * The candidates asked next pulled in (core/prefetch.h): the one l3 past
 * @pos toward L3, the one l1 past it on to L1; with @all, every one up to
 * them, as a sweep has just found them.
 */
static void lru_cast_cand_ahead(struct lru_cast *k, unsigned int pos, bool all)
{
	unsigned int l3 = READ_ONCE(lru_cast_pf_l3), l1 = READ_ONCE(lru_cast_pf_l1), i;

	if (all) {
		for (i = pos; i < min(pos + l3, k->ncand); i++)
			lru_cast_prefetch_frame(k->cand[i].pfn, i < pos + l1);
		return;
	}
	if (l3 && pos + l3 < k->ncand)
		lru_cast_prefetch_frame(k->cand[pos + l3].pfn, false);
	if (l1 && pos + l1 < k->ncand)
		lru_cast_prefetch_frame(k->cand[pos + l1].pfn, true);
}

/* the folio of candidate @c if it is still of M here and not read since */
static struct folio *lru_cast_cand_folio(struct lruvec *lruvec, int type, struct lru_cast_cand *c)
{
	struct page *page = lru_cast_m_page(lruvec, type, c->pfn);
	struct folio *folio;

	if (!page)
		return NULL;
	folio = page_folio(page);
	if (&folio->page != page || lru_cast_read_pfn(c->pfn) != c->word)
		return NULL;
	/* PG_lru under this lru_lock, of this memcg and node: on this lruvec's M */
	return folio;
}

/*
 * M's lowest folio, and its key; taken off the candidates. When the
 * candidates run out, a sweep; when it finds none, M's oldest-added folio.
 */
struct folio *lru_cast_ring_min(struct lruvec *lruvec, int type, long *bucket)
{
	struct lru_cast *k = &lruvec->lru_cast[type];
	struct list_head *m = lru_cast_m(lruvec, type);
	struct folio *folio;
	bool swept = false;

	if (list_empty(m))
		return NULL;
	for (;;) {
		while (k->pos < k->ncand) {
			struct lru_cast_cand *c = &k->cand[k->pos++];

			lru_cast_cand_ahead(k, k->pos - 1, false);
			folio = lru_cast_cand_folio(lruvec, type, c);
			if (folio) {
				*bucket = lru_cast_unrank(c->key);
				sw_add(SW_TAKEN, 1);
				return folio;
			}
			sw_add(SW_STALE, 1);
		}
		if (swept)
			break;
		lru_cast_sweep(lruvec, type);
		lru_cast_cand_ahead(k, 0, true);
		swept = true;
	}
	sw_add(SW_FALLBACK, 1);
	folio = lru_to_folio(m);
	*bucket = lru_cast_key_now(folio, lru_cast_tick(lruvec_pgdat(lruvec), type), k->ring_le);
	return folio;
}

int lru_cast_sweep_show(char *buf)
{
	unsigned long n[SW_NR] = { 0 };
	int cpu, i;

	for_each_possible_cpu(cpu)
		for (i = 0; i < SW_NR; i++)
			n[i] += per_cpu(lru_cast_sw, cpu)[i];
	return sysfs_emit(buf, "sweeps %lu frames %lu words %lu taken %lu stale %lu gate %lu fallback %lu spilled %lu probed %lu\n",
			  n[SW_SWEEPS], n[SW_FRAMES], n[SW_WORDS], n[SW_TAKEN], n[SW_STALE],
			  n[SW_GATE], n[SW_FALLBACK], n[SW_SPILLED], n[SW_PROBED]);
}

/*
 * lruvec_add_folio(), on an lruvec CAST runs: an active folio goes into
 * M; the rest into W, the inactive list, after which W's excess over its
 * share, once more than a slack, joins M (lru_cast_w_admit()). A folio new to
 * the cache had its entry counted in folio_add_lru() (lru_cast_enter()),
 * before the lock: this runs under it, for every folio the LRU takes in.
 */
bool __lru_cast_add_folio(struct lruvec *lruvec, struct folio *folio, bool tail)
{
	if (folio_test_unevictable(folio)) {
		lru_cast_ring_out(folio);
		return false;
	}
	if (!folio_test_active(folio)) {
		enum lru_list lru = folio_lru_list(folio);
		int type = folio_is_file_lru(folio);
		struct lru_cast *k = &lruvec->lru_cast[type];
		unsigned long nr_w, nr_m, nr = folio_nr_pages(folio);

		folio_set_rekey(folio);		/* W's: never placed by a key */
		lru_cast_ring_out(folio);		/* deactivated: no longer of M */
		lru_cast_update_lru_size(lruvec, lru, folio_zonenum(folio), nr);
		if (tail)
			list_add_tail(&folio->lru, &lruvec->lists[lru]);
		else
			list_add(&folio->lru, &lruvec->lists[lru]);
		/*
		 * W kept to its share as it fills, not only once reclaim runs.
		 * Its size, summed over the zones on lines every CPU adding to
		 * the LRU writes, is looked at only once the room left the
		 * last time is used: W gains only here, so it is over its share
		 * by more than the slack no sooner. The room is a slack at
		 * most, so that a share made smaller is caught up with soon.
		 */
		k->w_room -= nr;
		if (k->w_room > 0)
			return true;
		nr_w = lru_cast_lru_size(lruvec, lru);
		nr_m = lru_cast_lru_size(lruvec, lru + LRU_ACTIVE);
		lru_cast_w_admit(lruvec, type, &nr_w, &nr_m, LRU_CAST_W_SLACK,
				 LRU_CAST_W_SLACK + nr);
		k->w_room = clamp_t(long, (nr_w + nr_m) * lru_cast_param_w(type) / 1000 +
				    LRU_CAST_W_SLACK - nr_w, 1, LRU_CAST_W_SLACK);
		return true;
	}
	lru_cast_update_lru_size(lruvec, folio_lru_list(folio), folio_zonenum(folio),
			      folio_nr_pages(folio));
	lru_cast_ring_put(lruvec, folio, folio_is_file_lru(folio));
	return true;
}

void lru_cast_ring_reset(struct lruvec *lruvec)
{
	int type;

	for (type = 0; type < ANON_AND_FILE; type++) {
		struct lru_cast *k = &lruvec->lru_cast[type];

		lru_cast_flush(k);
		k->cursor = 0;
		k->rnd = type + 1;
		k->block_shift = 0;		/* the blocks drawn on first use, all set */
		k->ring_le = -1;		/* set on first use */
		k->w_room = 0;			/* W's size looked at on first use */
		k->victim = LONG_MIN;
	}
}
