// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- the controllers: the decay, W's share, and anon's share
 * of memory.
 *
 * Three numbers of the main cache's operating point (policy/params.c) are
 * chosen by measuring, not by tuning: per type the half-life its keys decay
 * with, H = C * 2^(8 - le), and W's share w of its pages; and across the
 * types anon's share of memory. A sample of the pages (sample.c) runs
 * through simulated CAST caches at once (sim.c), LRU_CAST_TUNE_SIM of each type
 * (shape.h): a grid of decays (decay.c) and a pair of W's shares (wshare.c);
 * and the type's miss-ratio curve, its points worlds held at each split of
 * memory with the other type (world.c), is read from the rank of each read
 * in the type's order, file read ahead at each point (readahead.c). What
 * they show (evidence.c) moves the decay and W of each type, and the split
 * between them (share.c), its misses weighed by what a refault costs
 * (cost.c).
 *
 * This file runs them: a read of a sampled page is an access to every
 * cache of its type and to its order, and the decisions are taken every so
 * many accesses.
 *
 * Every read the main cache records of a sampled page reaches here
 * (hooks.c); a sampled page that is mapped is protected (protect.c), so
 * that every read of it faults and comes here too. The caches share the
 * pages' state words with the main cache, and its clock -- the nodes'
 * ticks, so that le means the same half-life here as there.
 *
 * A read is not taken in where it happens: it is queued on its CPU and a
 * worker takes the queues in, every few milliseconds or sooner, all under
 * one lock -- a world holds of each type what the other leaves -- that no
 * reader waits for. With the controllers off
 * (lru_cast_tune_set_auto()) nothing is sampled: the numbers are set by hand.
 */
#include <linux/sysfs.h>
#include <linux/percpu.h>
#include "tune/tune.h"
#include "tune/sim.h"
#include "tune/world.h"
#include "tune/sample.h"
#include "tune/evidence.h"
#include "tune/decay.h"
#include "tune/wshare.h"
#include "tune/share.h"
#include "tune/cost.h"
#include "policy/policy.h"

struct lru_cast_tune_type {
	int type;
	struct lru_cast_tune_sim *sim;
	struct lru_cast_tune_ra *ra;		/* file's: it is read ahead */
	struct lru_cast_tune_evidence ev;
	struct lru_cast_tune_w w;
	unsigned long tick;		/* the nodes' clock, at the last read */
	unsigned long cap;		/* each cache's size: the type's sample */
	unsigned long events, next_eval, moves;
};

static struct lru_cast_tune_type *lru_cast_tune_types[ANON_AND_FILE];
static DEFINE_MUTEX(lru_cast_tune_lock);
static unsigned int lru_cast_tune_nslots;		/* slots per type */
/* the curve's evidence: the tick it was last aged at, and the accesses it holds, in 1/256 */
static unsigned long lru_cast_tune_aged;
static long lru_cast_tune_held;

unsigned int lru_cast_tune_on __read_mostly = 1;

void lru_cast_tune_set_auto(bool on)
{
	WRITE_ONCE(lru_cast_tune_on, on);
}

/* ------------------------------------------------------------ decisions */

/* the decay grid around @centre, each point from the nearest old one */
static void lru_cast_tune_move(struct lru_cast_tune_type *t, int centre)
{
	int le[LRU_CAST_TUNE_SIM], src[LRU_CAST_TUNE_SIM];

	lru_cast_tune_decay_regrid(t->sim, centre, le, src);
	lru_cast_tune_sim_regrid(t->sim, le, src);
	lru_cast_tune_ev_forget_grid(&t->ev);
	lru_cast_tune_w_forget(&t->w);
	t->moves++;
	if (lru_cast_tune_auto())
		lru_cast_param_set_le(t->type, centre);
}

/* each cache's size: the type's sample; the worlds', all memory's */
static void lru_cast_tune_set_caps(struct lru_cast_tune_type *t)
{
	unsigned long all = lru_cast_tune_sample_pages(lru_cast_tune_type_pages(0) +
							lru_cast_tune_type_pages(1));
	int m;

	lru_cast_tune_world_set_size(clamp(all, 512UL, (unsigned long)lru_cast_tune_nslots / 2));
	for (m = 0; m < LRU_CAST_TUNE_CURVE; m++)
		lru_cast_tune_sim_set_cap(t->sim, m, t->cap);
}

static void lru_cast_tune_evaluate(struct lru_cast_tune_type *t)
{
	int best = lru_cast_tune_decay_choose(&t->ev, t->sim);

	lru_cast_tune_share_update(&lru_cast_tune_types[0]->ev, &lru_cast_tune_types[1]->ev);
	lru_cast_tune_ev_decay(&t->ev);
	if (best >= 0)
		lru_cast_tune_move(t, lru_cast_tune_sim_le(t->sim, best));

	t->cap = clamp(lru_cast_tune_sample_pages(lru_cast_tune_type_pages(t->type)), 256UL,
		       (unsigned long)lru_cast_tune_nslots / LRU_CAST_TUNE_TABLE);
	lru_cast_tune_set_caps(t);
	t->next_eval = t->events + max(t->cap / 16, 32UL);
}

/* ---------------------------------------------------------------- reads */

/*
 * A read of a sampled page, as the caches take it: queued where it
 * happened, taken in order of each CPU's queue, a queue at a time.
 */
struct lru_cast_tune_rec {
	u64 id, block;
	pgoff_t index;
	unsigned long tick;		/* the nodes' clock when it was read */
	unsigned int nr;
	int type;
};

/*
 * Each CPU's reads, until the worker takes them in (lru_cast_tune_drain()):
 * a reader takes only its own CPU's lock, and the worker each one's for as
 * long as it copies the queue out. Half full, the worker is called at
 * once; otherwise the first read waits LRU_CAST_TUNE_DRAIN_DELAY at most.
 * Full -- the worker behind -- a read is dropped: the caches see fewer
 * reads when they cost more than a CPU.
 */
#define LRU_CAST_TUNE_QUEUE		256
#define LRU_CAST_TUNE_DRAIN_DELAY	max(HZ / 100, 1)

struct lru_cast_tune_queue {
	spinlock_t lock;
	unsigned int n;
	struct lru_cast_tune_rec rec[LRU_CAST_TUNE_QUEUE];
};

static struct lru_cast_tune_queue __percpu *lru_cast_tune_queues;
static struct lru_cast_tune_rec lru_cast_tune_taken[LRU_CAST_TUNE_QUEUE];	/* the worker's */
static atomic_long_t lru_cast_tune_dropped;

static void lru_cast_tune_drain(struct work_struct *work);
static DECLARE_DELAYED_WORK(lru_cast_tune_drain_work, lru_cast_tune_drain);

/*
 * Both types' curve evidence aged together, a step a tick since the last,
 * and while it holds more than it may: one time for both (evidence.c).
 */
static void lru_cast_tune_age(unsigned long tick)
{
	long ticks = tick - lru_cast_tune_aged;
	int steps, type;

	if (ticks > 0)
		lru_cast_tune_aged = tick;
	steps = clamp(ticks, 0L, (long)BITS_PER_LONG);
	if (!steps && lru_cast_tune_held > LRU_CAST_TUNE_HELD * 256)
		steps = 1;
	while (steps--) {
		for (type = 0; type < ANON_AND_FILE; type++)
			lru_cast_tune_ev_age(&lru_cast_tune_types[type]->ev);
		lru_cast_tune_held -= lru_cast_tune_held >> LRU_CAST_TUNE_AGE_SHIFT;
	}
}

/* the read @r, taken in by the caches of its type */
static void lru_cast_tune_take(struct lru_cast_tune_type *t, const struct lru_cast_tune_rec *r)
{
	int type = t->type, m;
	unsigned int rank, read[ANON_AND_FILE];
	u16 s, miss = 1, wait;
	bool full, hit[LRU_CAST_TUNE_N];
	u32 res;

	/* the queues are taken one by one: a CPU's reads can be behind another's */
	t->tick = max(t->tick, r->tick);
	s = lru_cast_tune_sim_read(t->sim, r->id, t->tick, &res);
	if (s == LRU_CAST_TUNE_SIM_NONE)
		return;
	for (m = 0; m < LRU_CAST_TUNE_SIM; m++)
		hit[m] = res & BIT(m);
	/*
	 * The curve's points: a hit where the page ranked within what its type
	 * holds in the world. Those that miss (bit p: p tenths), and of them
	 * those that wait.
	 */
	rank = lru_cast_tune_sim_rank(t->sim, s);
	read[0] = lru_cast_tune_sim_tracked(lru_cast_tune_types[0]->sim);
	read[1] = lru_cast_tune_sim_tracked(lru_cast_tune_types[1]->sim);
	for (m = LRU_CAST_TUNE_CURVE; m < LRU_CAST_TUNE_N; m++) {
		int world = lru_cast_tune_world_of(type, m);

		hit[m] = rank && rank <= lru_cast_tune_world_size(type, world, read);
		if (!hit[m])
			miss |= BIT(m - LRU_CAST_TUNE_CURVE + 1);
	}
	wait = t->ra ? lru_cast_tune_ra_waits(t->ra, r->block, r->index, r->nr, miss) : miss;
	full = lru_cast_tune_sim_count(t->sim, LRU_CAST_TUNE_CENTER) >= t->cap;
	lru_cast_tune_age(t->tick);
	lru_cast_tune_ev_note(&t->ev, hit, wait, full);
	lru_cast_tune_held += 256;
	if (full)
		lru_cast_tune_w_note(&t->w, hit[LRU_CAST_TUNE_WLO], hit[LRU_CAST_TUNE_WHI]);

	lru_cast_tune_sim_access_all(t->sim, s, res);

	if (++t->events >= t->next_eval)
		lru_cast_tune_evaluate(t);
	if (t->events >= t->w.next_wtest)
		lru_cast_tune_w_test(&t->w, t->sim, type, t->events, t->cap);
}

/* the worker: every CPU's queue taken in, one after another */
static void lru_cast_tune_drain(struct work_struct *work)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct lru_cast_tune_queue *q = per_cpu_ptr(lru_cast_tune_queues, cpu);
		unsigned int n, i;

		if (!READ_ONCE(q->n))
			continue;
		spin_lock_irq(&q->lock);
		n = q->n;
		memcpy(lru_cast_tune_taken, q->rec, n * sizeof(q->rec[0]));
		q->n = 0;
		spin_unlock_irq(&q->lock);

		mutex_lock(&lru_cast_tune_lock);
		for (i = 0; i < n; i++) {
			const struct lru_cast_tune_rec *r = &lru_cast_tune_taken[i];
			struct lru_cast_tune_type *t = lru_cast_tune_types[r->type];

			if (t)
				lru_cast_tune_take(t, r);
		}
		mutex_unlock(&lru_cast_tune_lock);
		cond_resched();
	}
}

/* a read of @folio, whose block @block is in the sample (lru_cast_tune_read()): queued */
void lru_cast_tune_event(struct folio *folio, u64 block)
{
	int type = folio_is_file_lru(folio);
	struct lru_cast_tune_queue *q;
	struct lru_cast_tune_rec r;
	unsigned long flags;
	unsigned int n;
	bool taken;

	if (!READ_ONCE(lru_cast_tune_types[type]))
		return;

	r = (struct lru_cast_tune_rec) {
		.id = lru_cast_tune_sample_page(folio),
		.block = block,
		.index = folio->index,
		.tick = lru_cast_ticks_now(),
		.nr = folio_nr_pages(folio),
		.type = type,
	};
	q = raw_cpu_ptr(lru_cast_tune_queues);
	spin_lock_irqsave(&q->lock, flags);
	n = q->n;
	taken = n < LRU_CAST_TUNE_QUEUE;
	if (taken) {
		q->rec[n++] = r;
		q->n = n;
	}
	spin_unlock_irqrestore(&q->lock, flags);
	if (!taken)
		atomic_long_inc(&lru_cast_tune_dropped);
	else if (n == 1)
		lru_cast_queue_delayed_work(&lru_cast_tune_drain_work, LRU_CAST_TUNE_DRAIN_DELAY);
	else if (n == LRU_CAST_TUNE_QUEUE / 2)
		lru_cast_mod_delayed_work(&lru_cast_tune_drain_work, 0);

	/* mapped, its next reads are caught by a protection fault */
	if (folio_mapped(folio))
		lru_cast_protect_queue(folio);
}

/*
 * do_swap_page(): an anon page swapped in, just mapped. Its refault was a
 * read the main cache counted (policy/ghost.c), but the page had no
 * identity then -- an anon folio in the swap cache has no mapping -- and
 * the sample could not see it: the reads it would miss are exactly those
 * of the pages the main cache evicted, which would make them look cold to
 * the controllers and evicted the more. Mapped, the page has its identity,
 * and the read is passed on now.
 */
void lru_cast_swapin(struct folio *folio)
{
	if (lru_cast_enabled())
		lru_cast_tune_read(folio);
}

/* ------------------------------------------------------------------ setup */

static void lru_cast_tune_free(struct lru_cast_tune_type *t)
{
	if (!t)
		return;
	lru_cast_tune_sim_free(t->sim);
	lru_cast_tune_ra_free(t->ra);
	kvfree(t);
}

static struct lru_cast_tune_type *lru_cast_tune_alloc_type(int type)
{
	struct lru_cast_tune_type *t = kvzalloc(sizeof(*t), LRU_CAST_TUNE_GFP);

	if (!t)
		return NULL;
	t->sim = lru_cast_tune_sim_alloc(lru_cast_tune_nslots);
	t->ra = type ? lru_cast_tune_ra_alloc() : NULL;
	if (!t->sim || (type && !t->ra)) {
		lru_cast_tune_free(t);
		return NULL;
	}
	return t;
}

/* a type's sample, empty, its caches at the decay and W the knobs hold now */
static void lru_cast_tune_init_type(struct lru_cast_tune_type *t, int type)
{
	int le[LRU_CAST_TUNE_SIM], src[LRU_CAST_TUNE_SIM];

	t->type = type;
	t->tick = lru_cast_ticks_now();
	t->cap = 256;
	t->events = t->moves = 0;
	t->next_eval = 32;
	lru_cast_tune_decay_grid(lru_cast_param_le(type), le, src);
	lru_cast_tune_sim_init(t->sim, t->tick, le);
	if (t->ra)
		lru_cast_tune_ra_init(t->ra);
	lru_cast_tune_ev_reset(&t->ev);
	lru_cast_tune_w_init(&t->w, lru_cast_param_w(type), t->sim);
}

/*
 * CAST coming on: the controllers start afresh -- empty samples, the
 * worlds' clocks at zero, the share unset, the costs at their prior. What
 * they had learnt before CAST last went off is of another time, its pages
 * since freed and their state words cleared (lru_cast_clear_states()); kept,
 * it would weigh as if it were now, the more for the clocks having stood
 * still meanwhile. Decay and W start where the knobs are.
 *
 * The samples are taken out from under lru_cast_tune_lock, set up again where they
 * are, and put back: nothing reaches them meanwhile (lru_cast_tune_types[] is read under
 * the lock), the reads queued of before dropped, and no memory is wanted but
 * the first time. Short of it then, CAST does not come on
 * (lru_cast_set_enabled()), and tries again the next time it is asked to.
 */
int lru_cast_tune_init(void)
{
	struct lru_cast_tune_type *t[ANON_AND_FILE];
	int type, cpu;

	if (!lru_cast_tune_queues) {
		lru_cast_tune_queues = alloc_percpu_gfp(struct lru_cast_tune_queue, LRU_CAST_TUNE_GFP);
		if (!lru_cast_tune_queues)
			return -ENOMEM;
		for_each_possible_cpu(cpu)
			spin_lock_init(&per_cpu_ptr(lru_cast_tune_queues, cpu)->lock);
	}
	if (!lru_cast_tune_nslots) {
		lru_cast_tune_sample_init(totalram_pages());
		lru_cast_tune_nslots = min(LRU_CAST_TUNE_TARGET * LRU_CAST_TUNE_TABLE,
					   lru_cast_tune_sim_max_slots());
	}

	mutex_lock(&lru_cast_tune_lock);
	for (type = 0; type < ANON_AND_FILE; type++) {
		t[type] = lru_cast_tune_types[type];
		lru_cast_tune_types[type] = NULL;
	}
	lru_cast_tune_world_reset();
	lru_cast_tune_share_reset();
	mutex_unlock(&lru_cast_tune_lock);
	lru_cast_tune_cost_reset();

	cancel_delayed_work_sync(&lru_cast_tune_drain_work);
	for_each_possible_cpu(cpu) {
		struct lru_cast_tune_queue *q = per_cpu_ptr(lru_cast_tune_queues, cpu);

		spin_lock_irq(&q->lock);
		q->n = 0;
		spin_unlock_irq(&q->lock);
	}

	for (type = 0; type < ANON_AND_FILE; type++)
		if (!t[type])
			t[type] = lru_cast_tune_alloc_type(type);
	if (!t[0] || !t[1]) {		/* the worlds need both */
		lru_cast_tune_free(t[0]);
		lru_cast_tune_free(t[1]);
		return -ENOMEM;
	}
	for (type = 0; type < ANON_AND_FILE; type++)
		lru_cast_tune_init_type(t[type], type);

	mutex_lock(&lru_cast_tune_lock);
	for (type = 0; type < ANON_AND_FILE; type++)
		lru_cast_tune_types[type] = t[type];
	lru_cast_tune_aged = t[0]->tick;
	lru_cast_tune_held = 0;
	mutex_unlock(&lru_cast_tune_lock);
	return 0;
}

int lru_cast_tune_show(char *buf)
{
	int type, m, len = 0;

	for (type = 0; type < ANON_AND_FILE; type++) {
		unsigned int read[ANON_AND_FILE];
		struct lru_cast_tune_sim_stat st;
		struct lru_cast_tune_type *t;

		mutex_lock(&lru_cast_tune_lock);
		t = lru_cast_tune_types[type];
		if (!t) {
			mutex_unlock(&lru_cast_tune_lock);
			continue;
		}
		len += sysfs_emit_at(buf, len,
				     "%s le %d w %u/1000 share %u/1000%s cap %lu events %lu moves %lu wmoves %lu tick %lu test %lld/%lld\n",
				     type ? "file" : "anon",
				     lru_cast_tune_sim_le(t->sim, LRU_CAST_TUNE_CENTER),
				     t->w.wq * 1000 / LRU_CAST_TUNE_WUNIT,
				     type ? 1000 - lru_cast_param_share() : lru_cast_param_share(),
				     lru_cast_param_share_held() ? "" : " (unset)",
				     t->cap, t->events, t->moves,
				     t->w.wmoves, t->tick, t->w.ws, t->w.wn);
		for (m = 0; m < LRU_CAST_TUNE_SIM; m++) {
			lru_cast_tune_sim_stat(t->sim, m, &st);
			len += sysfs_emit_at(buf, len,
					     "  le %2d w %4u cap %5lu resident %5lu W %5lu miss %lld dis %lld\n",
					     st.le, st.wq * 1000 / LRU_CAST_TUNE_WUNIT,
					     st.cap, st.count, st.wcount,
					     t->ev.miss[m] >> 8, t->ev.dis[m] >> 8);
		}
		/* the curve: what the type holds in each world, of what it has read */
		for (m = 0; m < ANON_AND_FILE; m++)
			read[m] = lru_cast_tune_types[m] ?
				  lru_cast_tune_sim_tracked(lru_cast_tune_types[m]->sim) : 0;
		for (m = LRU_CAST_TUNE_CURVE; m < LRU_CAST_TUNE_N; m++) {
			int world = lru_cast_tune_world_of(type, m);
			unsigned long held = lru_cast_tune_world_size(type, world, read);

			len += sysfs_emit_at(buf, len,
					     "  le %2d w    0 cap %5lu resident %5lu W     0 miss %lld dis %lld\n",
					     lru_cast_tune_sim_le(t->sim, LRU_CAST_TUNE_CENTER),
					     held,
					     min(held, (unsigned long)read[type]),
					     t->ev.miss[m] >> 8, t->ev.dis[m] >> 8);
		}
		mutex_unlock(&lru_cast_tune_lock);
	}
	len += sysfs_emit_at(buf, len, "reads dropped %ld\n", atomic_long_read(&lru_cast_tune_dropped));
	return len;
}
