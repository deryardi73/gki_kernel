// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- simulated CAST caches of a sample, and its order.
 *
 * Each cache is the main cache in small (policy/): W, a FIFO window for
 * newcomers, and M, a ring of LRU_CAST_TUNE_BUCKETS buckets of the key (core/key.c).
 * A miss enters W's head; while W is over its share, its tail meets the
 * lowest bucket of M -- the lower goes, a tie against the page of that
 * bucket read longest ago (policy/ring.c) -- or, the cache not yet full,
 * joins M. A hit in M moves the page up; a hit in W only updates its state.
 *
 * Among pages of one key the main cache takes those read once first, in
 * its clock's order, and those read more than once by a hash, the same of
 * them first each time (policy/ring.c). So does each cache here: a key's
 * bucket is LRU_CAST_TUNE_SUB of them, the first for pages read once, read
 * longest ago first, and the rest for pages read more than once by a hash
 * of the page, so that a loop over more than the cache keeps the same part
 * of itself here as it does there -- ranked by when they were read, it
 * would keep none, and the decay and W's share would be chosen for a
 * cache that is not the one they set. A newcomer read once meets M's
 * lowest by its key alone, as there.
 *
 * The order is every cache size at once. Without W, a CAST cache of any
 * size holds the top of one order of the pages -- by key, at the decay
 * grid's centre, and among equal keys the one read last first: a key
 * changes only when its page is read, which only lifts it, and a newcomer
 * joins only above the lowest, which then goes. So a page read is a hit in
 * a cache of n pages if its rank before the read is n or better, and one
 * rank says it for every point of the curve (world.c). That needs the
 * clock to be one for all of them -- the nodes', as the other caches have.
 *
 * The caches and the order share the sample's slots: a slot is a sampled
 * page read, linked into each cache it is resident in, and into the
 * order. Past the slots are the sentinels of each cache's buckets and W,
 * each linked in that cache only. Lists are circular through the
 * sentinels, so a slot leaves one without knowing which. With the table
 * full, a new page takes the slot of the lowest page in the order resident
 * in no cache: the one every size would have let go first.
 */
#include <linux/hash.h>
#include <linux/rbtree_augmented.h>
#include <linux/vmalloc.h>
#include "tune/sim.h"

#define LRU_CAST_TUNE_SUB	4				/* buckets per key */
#define LRU_CAST_TUNE_BUCKETS	(256 * LRU_CAST_TUNE_SUB)
#define LRU_CAST_TUNE_HEADS	(LRU_CAST_TUNE_BUCKETS + 1)	/* per cache: the buckets, then W */

struct lru_cast_tune_slot {
	u64 id;				/* 0: free */
	u32 w;				/* state word, in the clock's ticks */
	u16 hnext;			/* hash chain, or the free list */
	u16 res;			/* the caches it is resident in */
	u16 inw;			/* ... and in W of */
	u16 lnk[LRU_CAST_TUNE_SIM][2];		/* prev, next in its list of each cache */
	u16 size;			/* the order: pages in its subtree */
	struct rb_node rb;		/* ... its node, empty if not in it */
	long key;			/* ... its key when last read */
	u64 seq;			/* ... and when */
};

struct lru_cast_tune_cache {
	int le;
	unsigned int wq;		/* W's share, in LRU_CAST_TUNE_WUNIT */
	unsigned long cap;		/* pages it holds */
	long lo;			/* absolute number of the lowest bucket */
	DECLARE_BITMAP(nonempty, LRU_CAST_TUNE_BUCKETS);
	unsigned long count, wcount;	/* pages resident, of them in W */
};

struct lru_cast_tune_sim {
	struct lru_cast_tune_slot *slot;		/* slots, then the sentinels */
	u16 *hash;
	unsigned int nslots, hbits;
	u16 free;
	unsigned long tick;		/* the clock */
	struct lru_cast_tune_cache c[LRU_CAST_TUNE_SIM];
	struct rb_root order;		/* lowest first */
	int ole;			/* the order's decay */
	unsigned int tracked;		/* slots in it */
	u64 seq;			/* reads */
};

static inline u16 lru_cast_tune_head(struct lru_cast_tune_sim *sim, int m, unsigned int idx)
{
	return sim->nslots + m * LRU_CAST_TUNE_HEADS + idx;
}

static inline u16 lru_cast_tune_whead(struct lru_cast_tune_sim *sim, int m)
{
	return lru_cast_tune_head(sim, m, LRU_CAST_TUNE_BUCKETS);
}

/* the key of @w at decay @le in buckets: its key's, then which of them (see above) */
static long lru_cast_tune_skey(struct lru_cast_tune_sim *sim, u16 s, int le)
{
	u32 w = sim->slot[s].w;
	long key = lru_cast_key(w, sim->tick, le);

	if (key == LONG_MIN)
		return LONG_MIN;
	return key * LRU_CAST_TUNE_SUB +
	       ((w >> 24) >= 2 ? 1 + hash_64(sim->slot[s].id, 16) % (LRU_CAST_TUNE_SUB - 1) : 0);
}

/* the bucket of slot @s in cache @m */
static inline long lru_cast_tune_mkey(struct lru_cast_tune_sim *sim, int m, u16 s)
{
	return lru_cast_tune_skey(sim, s, sim->c[m].le);
}

/* the lowest bucket the window of cache @m holds now */
static inline long lru_cast_tune_window_lo(struct lru_cast_tune_sim *sim, int m)
{
	return lru_cast_window_lo(sim->tick, sim->c[m].le) * LRU_CAST_TUNE_SUB;
}

/* ------------------------------------------------------------- lists */

static void lru_cast_tune_unlink(struct lru_cast_tune_sim *sim, u16 s, int m)
{
	u16 p = sim->slot[s].lnk[m][0], n = sim->slot[s].lnk[m][1];

	sim->slot[p].lnk[m][1] = n;
	sim->slot[n].lnk[m][0] = p;
}

static void lru_cast_tune_add_tail(struct lru_cast_tune_sim *sim, u16 s, u16 head, int m)
{
	u16 p = sim->slot[head].lnk[m][0];

	sim->slot[s].lnk[m][0] = p;
	sim->slot[s].lnk[m][1] = head;
	sim->slot[p].lnk[m][1] = s;
	sim->slot[head].lnk[m][0] = s;
}

static void lru_cast_tune_add_head(struct lru_cast_tune_sim *sim, u16 s, u16 head, int m)
{
	u16 n = sim->slot[head].lnk[m][1];

	sim->slot[s].lnk[m][0] = head;
	sim->slot[s].lnk[m][1] = n;
	sim->slot[n].lnk[m][0] = s;
	sim->slot[head].lnk[m][1] = s;
}

static bool lru_cast_tune_empty(struct lru_cast_tune_sim *sim, u16 head, int m)
{
	return sim->slot[head].lnk[m][1] == head;
}

/* move all of @from to the head of @to, keeping its order */
static void lru_cast_tune_splice_head(struct lru_cast_tune_sim *sim, u16 from, u16 to, int m)
{
	u16 first, last, nxt;

	if (lru_cast_tune_empty(sim, from, m))
		return;
	first = sim->slot[from].lnk[m][1];
	last = sim->slot[from].lnk[m][0];
	nxt = sim->slot[to].lnk[m][1];
	sim->slot[to].lnk[m][1] = first;
	sim->slot[first].lnk[m][0] = to;
	sim->slot[last].lnk[m][1] = nxt;
	sim->slot[nxt].lnk[m][0] = last;
	sim->slot[from].lnk[m][0] = sim->slot[from].lnk[m][1] = from;
}

/* -------------------------------------------------------------- order */

static inline u16 lru_cast_tune_osize(struct rb_node *rb)
{
	return rb ? rb_entry(rb, struct lru_cast_tune_slot, rb)->size : 0;
}

static inline bool lru_cast_tune_ocompute(struct lru_cast_tune_slot *sl, bool exit)
{
	u16 size = 1 + lru_cast_tune_osize(sl->rb.rb_left) + lru_cast_tune_osize(sl->rb.rb_right);

	if (exit && sl->size == size)
		return true;
	sl->size = size;
	return false;
}

RB_DECLARE_CALLBACKS(static, lru_cast_tune_ocb, struct lru_cast_tune_slot, rb, size,
		     lru_cast_tune_ocompute);

/* @a ranks below @b: a lower key, or an equal one read before */
static inline bool lru_cast_tune_obelow(const struct lru_cast_tune_slot *a,
					const struct lru_cast_tune_slot *b)
{
	return a->key < b->key || (a->key == b->key && a->seq < b->seq);
}

/* slot @s into the order, by its key now and the read it had at @seq */
static void lru_cast_tune_oinsert(struct lru_cast_tune_sim *sim, u16 s, u64 seq)
{
	struct lru_cast_tune_slot *sl = &sim->slot[s];
	struct rb_node **link = &sim->order.rb_node, *parent = NULL;

	sl->key = lru_cast_tune_skey(sim, s, sim->ole);
	sl->seq = seq;
	while (*link) {
		struct lru_cast_tune_slot *p = rb_entry(*link, struct lru_cast_tune_slot, rb);

		parent = *link;
		p->size++;
		link = lru_cast_tune_obelow(sl, p) ? &parent->rb_left : &parent->rb_right;
	}
	sl->size = 1;
	rb_link_node(&sl->rb, parent, link);
	rb_insert_augmented(&sl->rb, &sim->order, &lru_cast_tune_ocb);
	sim->tracked++;
}

static void lru_cast_tune_oerase(struct lru_cast_tune_sim *sim, u16 s)
{
	rb_erase_augmented(&sim->slot[s].rb, &sim->order, &lru_cast_tune_ocb);
	RB_CLEAR_NODE(&sim->slot[s].rb);
	sim->tracked--;
}

/* the order rebuilt at decay @le: the keys are in other units */
static void lru_cast_tune_oregrid(struct lru_cast_tune_sim *sim, int le)
{
	unsigned int s;

	sim->ole = le;
	sim->order = RB_ROOT;
	sim->tracked = 0;
	for (s = 0; s < sim->nslots; s++)
		if (sim->slot[s].id)
			lru_cast_tune_oinsert(sim, s, sim->slot[s].seq);
}

/* -------------------------------------------------------------- rings */

static void lru_cast_tune_place(struct lru_cast_tune_sim *sim, int m, u16 s, long b)
{
	struct lru_cast_tune_cache *c = &sim->c[m];
	unsigned int idx;

	if (b < c->lo)
		b = c->lo;
	if (b > c->lo + LRU_CAST_TUNE_BUCKETS - 1)
		b = c->lo + LRU_CAST_TUNE_BUCKETS - 1;
	idx = b & (LRU_CAST_TUNE_BUCKETS - 1);
	lru_cast_tune_add_tail(sim, s, lru_cast_tune_head(sim, m, idx), m);
	__set_bit(idx, c->nonempty);
}

/* the window follows the clock: buckets fallen below join the lowest, oldest first */
static void lru_cast_tune_advance(struct lru_cast_tune_sim *sim, int m)
{
	struct lru_cast_tune_cache *c = &sim->c[m];
	long lo = lru_cast_tune_window_lo(sim, m), b, end;
	u16 to;

	if (lo <= c->lo)
		return;
	end = min(lo, c->lo + LRU_CAST_TUNE_BUCKETS - 1);
	to = lru_cast_tune_head(sim, m, lo & (LRU_CAST_TUNE_BUCKETS - 1));
	for (b = end; b >= c->lo; b--) {
		unsigned int idx = b & (LRU_CAST_TUNE_BUCKETS - 1);
		u16 h = lru_cast_tune_head(sim, m, idx);

		if (h == to)
			continue;
		lru_cast_tune_splice_head(sim, h, to, m);
		__clear_bit(idx, c->nonempty);
	}
	c->lo = lo;
	if (!lru_cast_tune_empty(sim, to, m))
		__set_bit(lo & (LRU_CAST_TUNE_BUCKETS - 1), c->nonempty);
}

/* the lowest nonempty bucket of cache @m: its number, and its sentinel */
static long lru_cast_tune_lowest(struct lru_cast_tune_sim *sim, int m, u16 *head)
{
	struct lru_cast_tune_cache *c = &sim->c[m];
	unsigned int start = c->lo & (LRU_CAST_TUNE_BUCKETS - 1), idx;

	for (;;) {
		idx = find_next_bit(c->nonempty, LRU_CAST_TUNE_BUCKETS, start);
		if (idx >= LRU_CAST_TUNE_BUCKETS)
			idx = find_first_bit(c->nonempty, LRU_CAST_TUNE_BUCKETS);
		if (idx >= LRU_CAST_TUNE_BUCKETS)
			return LONG_MAX;
		*head = lru_cast_tune_head(sim, m, idx);
		if (!lru_cast_tune_empty(sim, *head, m))
			return c->lo + ((idx - start) & (LRU_CAST_TUNE_BUCKETS - 1));
		__clear_bit(idx, c->nonempty);
	}
}

/* @s leaves cache @m, from W or M */
static void lru_cast_tune_evict(struct lru_cast_tune_sim *sim, int m, u16 s)
{
	lru_cast_tune_unlink(sim, s, m);
	if (sim->slot[s].inw & BIT(m)) {
		sim->slot[s].inw &= ~BIT(m);
		sim->c[m].wcount--;
	}
	sim->slot[s].res &= ~BIT(m);
	sim->c[m].count--;
}

/* W's tail into M (no longer counted in W) */
static void lru_cast_tune_promote(struct lru_cast_tune_sim *sim, int m, u16 s, long b)
{
	lru_cast_tune_unlink(sim, s, m);
	sim->slot[s].inw &= ~BIT(m);
	sim->c[m].wcount--;
	lru_cast_tune_place(sim, m, s, b);
}

/* W over its share meets M; then the cache within its size */
static void lru_cast_tune_balance(struct lru_cast_tune_sim *sim, int m)
{
	struct lru_cast_tune_cache *c = &sim->c[m];
	unsigned long wt = c->cap * c->wq / LRU_CAST_TUNE_WUNIT;
	long low, bt;
	u16 h, s;

	while (c->wcount > wt) {
		s = sim->slot[lru_cast_tune_whead(sim, m)].lnk[m][0];	/* W's tail */
		bt = lru_cast_tune_mkey(sim, m, s);
		if (c->count <= c->cap) {
			lru_cast_tune_promote(sim, m, s, bt);	/* not full: it joins M */
			continue;
		}
		low = lru_cast_tune_lowest(sim, m, &h);
		/* read once, by its key alone: of one key, the newcomer was read last */
		if (low == LONG_MAX ||
		    ((sim->slot[s].w >> 24) >= 2 ? bt < low :
		     bt / LRU_CAST_TUNE_SUB < low / LRU_CAST_TUNE_SUB)) {
			lru_cast_tune_evict(sim, m, s);
			continue;
		}
		lru_cast_tune_evict(sim, m, sim->slot[h].lnk[m][1]);
		lru_cast_tune_promote(sim, m, s, bt);
	}
	while (c->count > c->cap) {
		low = lru_cast_tune_lowest(sim, m, &h);
		if (low == LONG_MAX)
			s = sim->slot[lru_cast_tune_whead(sim, m)].lnk[m][0];
		else
			s = sim->slot[h].lnk[m][1];
		lru_cast_tune_evict(sim, m, s);
	}
}

/* an access to @s in cache @m: in, or up; not yet balanced */
static void lru_cast_tune_access(struct lru_cast_tune_sim *sim, int m, u16 s, bool hit)
{
	struct lru_cast_tune_cache *c = &sim->c[m];

	lru_cast_tune_advance(sim, m);
	if (hit) {
		if (sim->slot[s].inw & BIT(m))
			return;			/* in W: its state only */
		lru_cast_tune_unlink(sim, s, m);
		lru_cast_tune_place(sim, m, s, lru_cast_tune_mkey(sim, m, s));
		return;
	}
	lru_cast_tune_add_head(sim, s, lru_cast_tune_whead(sim, m), m);
	sim->slot[s].res |= BIT(m);
	sim->slot[s].inw |= BIT(m);
	c->count++;
	c->wcount++;
}

/*
 * A read of @s, resident in the caches of @res, in every cache and in the
 * order: in, or up. Each cache is made to fit on a hit too: a size or a
 * share set lower (lru_cast_tune_sim_set_cap(), _set_wq()) or a regrid
 * leaves it over until it is, and a cache over its size that only hits
 * would never be -- holding more than it may, it misses less than the
 * others, and the decay it runs at wins.
 */
void lru_cast_tune_sim_access_all(struct lru_cast_tune_sim *sim, u16 s, u32 res)
{
	int m;

	for (m = 0; m < LRU_CAST_TUNE_SIM; m++) {
		lru_cast_tune_access(sim, m, s, res & BIT(m));
		lru_cast_tune_balance(sim, m);
	}
	if (!RB_EMPTY_NODE(&sim->slot[s].rb))
		lru_cast_tune_oerase(sim, s);
	lru_cast_tune_oinsert(sim, s, ++sim->seq);
}

/* @s's rank in the order, 1 the top, before its read reaches it; 0: not in it */
unsigned int lru_cast_tune_sim_rank(struct lru_cast_tune_sim *sim, u16 s)
{
	struct rb_node *rb = &sim->slot[s].rb, *p;
	unsigned int below;

	if (RB_EMPTY_NODE(rb))
		return 0;
	below = lru_cast_tune_osize(rb->rb_left);
	for (p = rb_parent(rb); p; rb = p, p = rb_parent(rb))
		if (p->rb_right == rb)
			below += lru_cast_tune_osize(p->rb_left) + 1;
	return sim->tracked - below;
}

/* the pages in the order: those read, while the table had room */
unsigned int lru_cast_tune_sim_tracked(struct lru_cast_tune_sim *sim)
{
	return sim->tracked;
}

/* --------------------------------------------------------------- slots */

static u16 lru_cast_tune_lookup(struct lru_cast_tune_sim *sim, u64 id)
{
	u16 s = sim->hash[hash_64(id, sim->hbits)];

	while (s != LRU_CAST_TUNE_SIM_NONE && sim->slot[s].id != id)
		s = sim->slot[s].hnext;
	return s;
}

static void lru_cast_tune_unhash(struct lru_cast_tune_sim *sim, u16 s)
{
	u16 *pp = &sim->hash[hash_64(sim->slot[s].id, sim->hbits)];

	while (*pp != s)
		pp = &sim->slot[*pp].hnext;
	*pp = sim->slot[s].hnext;
}

/* how far up the order to look for a slot in no cache */
#define LRU_CAST_TUNE_REUSE_SCAN	64

/* a slot for @id: a free one, or that of the lowest page in the order resident in no cache */
static u16 lru_cast_tune_alloc(struct lru_cast_tune_sim *sim, u64 id)
{
	u16 s = sim->free, *hp;

	if (s != LRU_CAST_TUNE_SIM_NONE) {
		sim->free = sim->slot[s].hnext;
	} else {
		struct rb_node *rb = rb_first(&sim->order);
		int n;

		for (n = 0; rb && n < LRU_CAST_TUNE_REUSE_SCAN; n++, rb = rb_next(rb))
			if (!rb_entry(rb, struct lru_cast_tune_slot, rb)->res)
				break;
		if (!rb || n == LRU_CAST_TUNE_REUSE_SCAN)
			return LRU_CAST_TUNE_SIM_NONE;
		s = rb_entry(rb, struct lru_cast_tune_slot, rb) - sim->slot;
		lru_cast_tune_oerase(sim, s);
		lru_cast_tune_unhash(sim, s);
	}
	sim->slot[s].id = id;
	sim->slot[s].w = 0;
	sim->slot[s].res = 0;
	sim->slot[s].inw = 0;
	RB_CLEAR_NODE(&sim->slot[s].rb);
	hp = &sim->hash[hash_64(id, sim->hbits)];
	sim->slot[s].hnext = *hp;
	*hp = s;
	return s;
}

/*
 * A read of page @id at @tick, the nodes' clock: its slot, given one if it
 * has none (LRU_CAST_TUNE_SIM_NONE: no room), and the read in its state word. @res:
 * the caches it is resident in, before the read reaches them, and its rank
 * in the order (lru_cast_tune_sim_rank()) is as it was, until lru_cast_tune_sim_access_all().
 */
u16 lru_cast_tune_sim_read(struct lru_cast_tune_sim *sim, u64 id, unsigned long tick, u32 *res)
{
	u16 s = lru_cast_tune_lookup(sim, id);
	u32 old, n;

	sim->tick = tick;
	if (s == LRU_CAST_TUNE_SIM_NONE)
		s = lru_cast_tune_alloc(sim, id);
	if (s == LRU_CAST_TUNE_SIM_NONE)
		return s;
	old = sim->slot[s].w;
	n = old >> 24;
	sim->slot[s].w = lru_cast_push(old, sim->tick, min(n + 1, 255u));
	*res = sim->slot[s].res;
	return s;
}

/* ------------------------------------------------------------- caches */

int lru_cast_tune_sim_le(struct lru_cast_tune_sim *sim, int m)
{
	return sim->c[m].le;
}

unsigned long lru_cast_tune_sim_count(struct lru_cast_tune_sim *sim, int m)
{
	return sim->c[m].count;
}

void lru_cast_tune_sim_set_cap(struct lru_cast_tune_sim *sim, int m, unsigned long cap)
{
	sim->c[m].cap = cap;
}

void lru_cast_tune_sim_set_wq(struct lru_cast_tune_sim *sim, int m, unsigned int wq)
{
	sim->c[m].wq = wq;
}

void lru_cast_tune_sim_stat(struct lru_cast_tune_sim *sim, int m, struct lru_cast_tune_sim_stat *st)
{
	struct lru_cast_tune_cache *c = &sim->c[m];

	st->le = c->le;
	st->wq = c->wq;
	st->cap = c->cap;
	st->count = c->count;
	st->wcount = c->wcount;
}

/* the caches again: cache m takes le[m] and the pages of cache src[m]; the order, the centre's */
void lru_cast_tune_sim_regrid(struct lru_cast_tune_sim *sim, const int *le, const int *src)
{
	unsigned int s, m, idx;

	for (s = 0; s < sim->nslots; s++) {
		struct lru_cast_tune_slot *sl = &sim->slot[s];
		u16 res = 0, inw = 0;

		if (!sl->id)
			continue;
		for (m = 0; m < LRU_CAST_TUNE_SIM; m++) {
			if (sl->res & BIT(src[m]))
				res |= BIT(m);
			if (sl->inw & BIT(src[m]))
				inw |= BIT(m);
		}
		sl->res = res;
		sl->inw = inw;
	}
	for (m = 0; m < LRU_CAST_TUNE_SIM; m++) {
		struct lru_cast_tune_cache *c = &sim->c[m];

		c->le = le[m];
		c->lo = lru_cast_tune_window_lo(sim, m);
		c->count = c->wcount = 0;
		bitmap_zero(c->nonempty, LRU_CAST_TUNE_BUCKETS);
		for (idx = 0; idx < LRU_CAST_TUNE_HEADS; idx++) {
			u16 h = lru_cast_tune_head(sim, m, idx);

			sim->slot[h].lnk[m][0] = sim->slot[h].lnk[m][1] = h;
		}
	}
	for (s = 0; s < sim->nslots; s++) {
		struct lru_cast_tune_slot *sl = &sim->slot[s];

		if (!sl->id || !sl->res)
			continue;
		for (m = 0; m < LRU_CAST_TUNE_SIM; m++) {
			if (!(sl->res & BIT(m)))
				continue;
			sim->c[m].count++;
			if (sl->inw & BIT(m)) {
				lru_cast_tune_add_tail(sim, s, lru_cast_tune_whead(sim, m), m);
				sim->c[m].wcount++;
			} else {
				lru_cast_tune_place(sim, m, s, lru_cast_tune_mkey(sim, m, s));
			}
		}
	}
	if (le[LRU_CAST_TUNE_CENTER] != sim->ole)
		lru_cast_tune_oregrid(sim, le[LRU_CAST_TUNE_CENTER]);
}

/* ------------------------------------------------------------- set up */

/* the most slots a sim can have: they, the sentinels and LRU_CAST_TUNE_SIM_NONE fit in a u16 */
unsigned int lru_cast_tune_sim_max_slots(void)
{
	return LRU_CAST_TUNE_SIM_NONE - LRU_CAST_TUNE_SIM * LRU_CAST_TUNE_HEADS;
}

struct lru_cast_tune_sim *lru_cast_tune_sim_alloc(unsigned int nslots)
{
	unsigned int total = nslots + LRU_CAST_TUNE_SIM * LRU_CAST_TUNE_HEADS;
	struct lru_cast_tune_sim *sim;

	BUILD_BUG_ON(LRU_CAST_TUNE_SIM > 16);		/* res and inw are bitmasks */
	if (WARN_ON_ONCE(nslots > lru_cast_tune_sim_max_slots()))
		return NULL;
	sim = kvmalloc(sizeof(*sim), LRU_CAST_TUNE_GFP);
	if (!sim)
		return NULL;
	sim->nslots = nslots;
	sim->hbits = ilog2(roundup_pow_of_two(nslots));
	sim->slot = __vmalloc(array_size(total, sizeof(*sim->slot)), LRU_CAST_TUNE_GFP);
	sim->hash = __vmalloc(array_size(1U << sim->hbits, sizeof(*sim->hash)), LRU_CAST_TUNE_GFP);
	if (!sim->slot || !sim->hash) {
		lru_cast_tune_sim_free(sim);
		return NULL;
	}
	return sim;
}

void lru_cast_tune_sim_free(struct lru_cast_tune_sim *sim)
{
	if (!sim)
		return;
	vfree(sim->slot);
	vfree(sim->hash);
	kvfree(sim);
}

/* empty, every cache 256 pages at le[m], W's shares 0 */
void lru_cast_tune_sim_init(struct lru_cast_tune_sim *sim, unsigned long tick, const int *le)
{
	unsigned int total = sim->nslots + LRU_CAST_TUNE_SIM * LRU_CAST_TUNE_HEADS, s;
	int src[LRU_CAST_TUNE_SIM], m;

	memset(sim->slot, 0, array_size(total, sizeof(*sim->slot)));
	memset(sim->hash, 0xff, (1U << sim->hbits) * sizeof(*sim->hash));
	memset(sim->c, 0, sizeof(sim->c));
	sim->free = LRU_CAST_TUNE_SIM_NONE;
	for (s = sim->nslots; s-- > 0;) {
		sim->slot[s].hnext = sim->free;
		sim->free = s;
	}
	sim->tick = tick;
	sim->order = RB_ROOT;
	sim->ole = -1;
	sim->tracked = 0;
	sim->seq = 0;
	for (m = 0; m < LRU_CAST_TUNE_SIM; m++) {
		sim->c[m].cap = 256;
		src[m] = m;
	}
	lru_cast_tune_sim_regrid(sim, le, src);
}
