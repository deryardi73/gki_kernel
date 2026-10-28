// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- what a refault costs, per type: how long a task waited,
 * on average, for a page reclaim had evicted to come back in. Only
 * refaults count: the misses a split of memory changes are the pages it
 * evicted and wanted again -- a page read once costs the same under every
 * split and decides nothing. Only refaults a task waited for count, too: one read ahead in
 * time is a hit, and how many are depends on what reclaim evicted -- the
 * more of a type it takes, the more of its refaults come back in runs and
 * are read ahead, the cheaper the type would look and the more it would
 * take: a loop that feeds itself. The wait for a page not yet in is the
 * device's and the decompressor's, whatever the split. The waits are taken
 * where tasks wait, each a sample: anon's major faults, a swap-in always a
 * refault; and file's faults and read()s that wait for a page not yet in
 * (not there, or still being read), when it came back with its history
 * (policy/ghost.c).
 *
 * The estimate is the samples' sum over their number with one sample of
 * prior, the ratio swappiness gives:
 *
 *	cost = (prior + sum of waits) / (1 + waits)
 *
 * at every moment, nothing switching as samples come: with none it is the
 * prior, with many what is measured. Sum and number forget together by the
 * number of waits: once there are as many as the pages on the LRU -- a
 * cache's worth, the controllers' unit of time -- both are halved. A busy
 * system averages over its last turnover or two, an idle one forgets
 * nothing. The prior's scale is the two types' samples taken together
 * (1 ns with none), its ratio anon : file = (MAX_SWAPPINESS - s) : s.
 *
 * A wait is timed as the device's part of it, not as the time from asking
 * to having: while others wait too, a page comes in behind the ones asked
 * before it, and the time it spent queued behind them is theirs. So, as
 * ADIOS's latency model does, the waits in flight together on one queue are
 * a burst: each is the time from the end before it to its own, and the
 * first to end, with no end before it, is timed from the burst's start,
 * when the first of it began. The times add up to the burst's, from its
 * start to its last end, however its waits interleave. The clock is read
 * not at each: at a burst's start, at its last end, and every
 * LRU_CAST_WAIT_BATCH ends between, the ends since the last reading sharing
 * the time alike. A wait is in flight from where the task is known to wait
 * for a page not in -- a swap-in, or a file page not yet read -- to where
 * the call that waited returns. The queue is what does the work: a device,
 * one per type, for what is read from a disk; the CPU the task is on, for
 * what the task works off itself -- a swap-in from zram decompressed by the
 * task, which other CPUs decompressing at the same time do not make any
 * quicker. A task that sleeps in such a wait and wakes on another CPU ends
 * it on the CPU it began on; every wait counts to the bursts, those for a
 * page back from an eviction are also the samples. What the device has
 * queued that no task waits for -- readahead, writeback -- CAST cannot see.
 *
 * Waits and their number are counted per CPU, where they happen, and
 * gathered at each evaluation.
 */
#include <linux/backing-dev.h>
#include <linux/swap.h>
#include "tune/tune.h"
#include "tune/cost.h"
#include "policy/policy.h"

static DEFINE_PER_CPU(u64, lru_cast_tune_wait[ANON_AND_FILE]);	/* ns */
static DEFINE_PER_CPU(u64, lru_cast_tune_refaults[ANON_AND_FILE]);
/* anon's swap-outs: ns, pages */
static DEFINE_PER_CPU(u64, lru_cast_tune_evict_ns);
static DEFINE_PER_CPU(u64, lru_cast_tune_evicts);
#define LRU_CAST_WAIT_BATCH	16	/* ends timed at one reading of the clock, about */

/*
 * A queue waits form on: the waits in flight; those ended and not yet
 * timed, packed in four fields of 16 bits -- all of them, anon's refaults,
 * file's, and anon's pages written out (LRU_CAST_WAIT_EVICT); when the
 * clock was last read. A swap-out to a device queues there as a read does.
 */
struct lru_cast_wait_q {
	atomic_t inflight;
	atomic64_t ended, mark;
};

#define LRU_CAST_WAIT_BITS	16
#define LRU_CAST_WAIT_MASK	((1ULL << LRU_CAST_WAIT_BITS) - 1)
#define LRU_CAST_WAIT_EVICT	3	/* the field of anon's pages written out */
#define LRU_CAST_WAIT_ONE(f)	(1ULL << (LRU_CAST_WAIT_BITS * (f)))

/* what a queue's refaults cost, and anon's swap-outs: ns and number */
struct lru_cast_wait_stat {
	atomic64_t ns[ANON_AND_FILE], n[ANON_AND_FILE];
	atomic64_t evict_ns, evict_n;
};

/*
 * A device, by the id of its backing_dev_info: its queue, shared by
 * whatever is read from it, swap and file alike. The last is all the
 * devices the table has no room for, and those of no id.
 */
#define LRU_CAST_WAIT_DEVS	16

struct lru_cast_wait_dev {
	u64 id;
	struct lru_cast_wait_q q;
	struct lru_cast_wait_stat stat;
};

static struct lru_cast_wait_dev lru_cast_wait_devs[LRU_CAST_WAIT_DEVS + 1];
/* the CPUs, each its own queue; what their waits cost, together */
static DEFINE_PER_CPU(struct lru_cast_wait_q, lru_cast_wait_cpu_q);
static struct lru_cast_wait_stat lru_cast_wait_cpu_stat;

static DEFINE_SPINLOCK(lru_cast_tune_cost_lock);
/* forgetting */
static u64 lru_cast_tune_sum[ANON_AND_FILE], lru_cast_tune_n[ANON_AND_FILE];
/* gathered so far */
static u64 lru_cast_tune_last_wait[ANON_AND_FILE], lru_cast_tune_last_n[ANON_AND_FILE];
/* anon's swap-outs, forgetting with the refaults; gathered so far */
static u64 lru_cast_tune_esum, lru_cast_tune_en, lru_cast_tune_last_esum, lru_cast_tune_last_en;

/*
 * The clock read at @now: the waits ended on @q since it was last read share
 * the time between, each alike; the refaults' part is their wait. Two
 * readings at once may pair a time with the other's ends; both add up all
 * the same.
 */
static void lru_cast_wait_time(struct lru_cast_wait_q *q, struct lru_cast_wait_stat *stat,
			       u64 now)
{
	u64 mark = atomic64_xchg(&q->mark, now);
	u64 ended = atomic64_xchg(&q->ended, 0);
	u64 n = ended & LRU_CAST_WAIT_MASK;
	u64 w = (ended >> (LRU_CAST_WAIT_BITS * LRU_CAST_WAIT_EVICT)) & LRU_CAST_WAIT_MASK;
	/* now behind mark: read before another's reading, swapped in after it */
	u64 dt = (s64)(now - mark) > 0 ? now - mark : 0;
	int type;

	for (type = 0; type < ANON_AND_FILE; type++) {
		u64 r = (ended >> (LRU_CAST_WAIT_BITS * (type + 1))) & LRU_CAST_WAIT_MASK;
		u64 part;

		if (!r)
			continue;
		part = div64_u64(dt * r, n);
		this_cpu_add(lru_cast_tune_wait[type], part);
		this_cpu_add(lru_cast_tune_refaults[type], r);
		atomic64_add(part, &stat->ns[type]);
		atomic64_add(r, &stat->n[type]);
	}
	if (w) {
		u64 part = div64_u64(dt * w, n);

		this_cpu_add(lru_cast_tune_evict_ns, part);
		this_cpu_add(lru_cast_tune_evicts, w);
		atomic64_add(part, &stat->evict_ns);
		atomic64_add(w, &stat->evict_n);
	}
}

/* the table's place of device @id, taken if it has none yet */
static int lru_cast_wait_dev_slot(u64 id)
{
	int i;

	if (!id)
		return LRU_CAST_WAIT_DEVS;
	for (i = 0; i < LRU_CAST_WAIT_DEVS; i++) {
		u64 cur = READ_ONCE(lru_cast_wait_devs[i].id);

		if (cur == id)
			return i;
		if (!cur) {
			cur = cmpxchg64(&lru_cast_wait_devs[i].id, 0, id);
			if (!cur || cur == id)
				return i;
		}
	}
	return LRU_CAST_WAIT_DEVS;
}

/* the queue the task's wait is on, and where its costs go */
static struct lru_cast_wait_q *lru_cast_wait_q(u8 wait, struct lru_cast_wait_stat **stat)
{
	u16 where = current->lru_cast_wait_where;

	if (wait & LRU_CAST_WAIT_CPU) {
		*stat = &lru_cast_wait_cpu_stat;
		return per_cpu_ptr(&lru_cast_wait_cpu_q, where);
	}
	*stat = &lru_cast_wait_devs[where].stat;
	return &lru_cast_wait_devs[where].q;
}

/* one more in flight on @q: the first of a burst reads the clock at its start */
static void lru_cast_q_join(struct lru_cast_wait_q *q, struct lru_cast_wait_stat *stat)
{
	if (atomic_inc_return(&q->inflight) == 1)
		lru_cast_wait_time(q, stat, ktime_get_ns());
}

/* one fewer, @inc added to its ends: the last of a burst, or the end that makes a batch */
static void lru_cast_q_leave(struct lru_cast_wait_q *q, struct lru_cast_wait_stat *stat,
			     u64 inc)
{
	u64 ended = inc ? atomic64_add_return(inc, &q->ended) : 0;
	u64 now = ended & LRU_CAST_WAIT_MASK, was = (ended - inc) & LRU_CAST_WAIT_MASK;

	/* the ends after the batch's, before the reading takes the count, go with it */
	if (atomic_dec_if_positive(&q->inflight) <= 0 ||
	    (was < LRU_CAST_WAIT_BATCH && now >= LRU_CAST_WAIT_BATCH))
		lru_cast_wait_time(q, stat, ktime_get_ns());
}

static void lru_cast_wait_join(void)
{
	struct lru_cast_wait_stat *stat;
	struct lru_cast_wait_q *q = lru_cast_wait_q(current->lru_cast_wait, &stat);

	lru_cast_q_join(q, stat);
}

static void lru_cast_wait_leave(u8 wait, u64 inc)
{
	struct lru_cast_wait_stat *stat;
	struct lru_cast_wait_q *q = lru_cast_wait_q(wait, &stat);

	lru_cast_q_leave(q, stat, inc);
}

void __lru_cast_wait_begin(int type, bool refault, u64 dev)
{
	/* one begun and never ended -- CAST switched off before it ended */
	if (unlikely(current->lru_cast_wait))
		__lru_cast_wait_end(false);
	current->lru_cast_wait = (type + 1) | (refault ? LRU_CAST_WAIT_REFAULT : 0);
	if (dev == LRU_CAST_DEV_CPU) {
		/* the CPU it began on, which the task may leave if it sleeps */
		current->lru_cast_wait |= LRU_CAST_WAIT_CPU;
		current->lru_cast_wait_where = raw_smp_processor_id();
	} else {
		current->lru_cast_wait_where = lru_cast_wait_dev_slot(dev);
	}
	lru_cast_wait_join();
}

void __lru_cast_wait_begin_file(bool refault, struct address_space *mapping)
{
	__lru_cast_wait_begin(LRU_CAST_FILE, refault, inode_to_bdi(mapping->host)->id);
}

/* the swap-in begun on the CPU is read from device @dev: it queues there */
void __lru_cast_wait_queued(u64 dev)
{
	lru_cast_wait_leave(current->lru_cast_wait, 0);
	current->lru_cast_wait &= ~LRU_CAST_WAIT_CPU;
	current->lru_cast_wait_where = lru_cast_wait_dev_slot(dev);
	lru_cast_wait_join();
}

/* the task's wait ended: with the page in (@done), or given up */
void __lru_cast_wait_end(bool done)
{
	u8 wait = current->lru_cast_wait;
	int type = (wait & LRU_CAST_WAIT_TYPE) - 1;
	u64 inc = 0;

	current->lru_cast_wait_swap = 0;
	if (!wait)
		return;
	current->lru_cast_wait = 0;
	if (done)
		inc = 1 | (wait & LRU_CAST_WAIT_REFAULT ? LRU_CAST_WAIT_ONE(type + 1) : 0);
	lru_cast_wait_leave(wait, inc);
}

/*
 * Swap-outs: what writing out a page of anon costs. Done by the CPU -- a
 * zero-filled page, zswap's compression, a synchronous device's (zram's)
 * -- it is the time taken there, from @t0; written to a device, the
 * write queues on it with the reads, from submission to completion.
 */
void __lru_cast_evict_cpu(u64 t0, unsigned long nr)
{
	u64 ns = ktime_get_ns() - t0;

	this_cpu_add(lru_cast_tune_evict_ns, ns);
	this_cpu_add(lru_cast_tune_evicts, nr);
	atomic64_add(ns, &lru_cast_wait_cpu_stat.evict_ns);
	atomic64_add(nr, &lru_cast_wait_cpu_stat.evict_n);
}

void __lru_cast_evict_io_begin(u64 dev)
{
	struct lru_cast_wait_dev *d = &lru_cast_wait_devs[lru_cast_wait_dev_slot(dev)];

	lru_cast_q_join(&d->q, &d->stat);
}

void __lru_cast_evict_io_end(u64 dev, unsigned long nr)
{
	struct lru_cast_wait_dev *d = &lru_cast_wait_devs[lru_cast_wait_dev_slot(dev)];

	lru_cast_q_leave(&d->q, &d->stat, nr | nr * LRU_CAST_WAIT_ONE(LRU_CAST_WAIT_EVICT));
}

/* the waits in flight on every queue */
static int lru_cast_wait_inflight(void)
{
	int i, cpu, n = 0;

	for (i = 0; i <= LRU_CAST_WAIT_DEVS; i++)
		n += atomic_read(&lru_cast_wait_devs[i].q.inflight);
	for_each_possible_cpu(cpu)
		n += atomic_read(&per_cpu_ptr(&lru_cast_wait_cpu_q, cpu)->inflight);
	return n;
}

static int lru_cast_wait_stat_show(char *buf, int len, const char *what,
				   struct lru_cast_wait_stat *stat)
{
	u64 n[ANON_AND_FILE], ns[ANON_AND_FILE], en = atomic64_read(&stat->evict_n);
	int type;

	for (type = 0; type < ANON_AND_FILE; type++) {
		n[type] = atomic64_read(&stat->n[type]);
		ns[type] = n[type] ? div64_u64(atomic64_read(&stat->ns[type]), n[type]) : 0;
	}

	if (!n[0] && !n[1] && !en)
		return 0;
	return sysfs_emit_at(buf, len,
			     "%s: anon %llu ns (%llu) file %llu ns (%llu) swap-out %llu ns (%llu)\n",
			     what, ns[0], n[0], ns[1], n[1],
			     en ? div64_u64(atomic64_read(&stat->evict_ns), en) : 0, en);
}

/*
 * A page being read in is back from an eviction if it came back with its
 * history (policy/ghost.c): one new to the cache has read once, its entry.
 */
bool lru_cast_refaulted(struct folio *folio)
{
	return lru_cast_enabled() && (lru_cast_read(folio) >> 24) >= 2;
}


/* the CPUs' counts into the estimate, forgetting by the cache's worth */
void lru_cast_tune_cost_gather(void)
{
	u64 window = max(lru_cast_tune_type_pages(0) + lru_cast_tune_type_pages(1), 1024UL);
	unsigned long flags;
	int type, cpu;

	if (!spin_trylock_irqsave(&lru_cast_tune_cost_lock, flags))
		return;
	for (type = 0; type < ANON_AND_FILE; type++) {
		u64 w = 0, n = 0;

		for_each_possible_cpu(cpu) {
			w += per_cpu(lru_cast_tune_wait[type], cpu);
			n += per_cpu(lru_cast_tune_refaults[type], cpu);
		}
		lru_cast_tune_sum[type] += w - lru_cast_tune_last_wait[type];
		lru_cast_tune_n[type] += n - lru_cast_tune_last_n[type];
		lru_cast_tune_last_wait[type] = w;
		lru_cast_tune_last_n[type] = n;
		while (lru_cast_tune_n[type] >= window) {
			lru_cast_tune_sum[type] -= lru_cast_tune_sum[type] >> 1;
			lru_cast_tune_n[type] -= lru_cast_tune_n[type] >> 1;
		}
	}
	{
		u64 e = 0, en = 0;

		for_each_possible_cpu(cpu) {
			e += per_cpu(lru_cast_tune_evict_ns, cpu);
			en += per_cpu(lru_cast_tune_evicts, cpu);
		}
		lru_cast_tune_esum += e - lru_cast_tune_last_esum;
		lru_cast_tune_en += en - lru_cast_tune_last_en;
		lru_cast_tune_last_esum = e;
		lru_cast_tune_last_en = en;
		while (lru_cast_tune_en >= window) {
			lru_cast_tune_esum -= lru_cast_tune_esum >> 1;
			lru_cast_tune_en -= lru_cast_tune_en >> 1;
		}
	}
	spin_unlock_irqrestore(&lru_cast_tune_cost_lock, flags);
}

/* no samples: the estimate back at its prior, the CPUs' counts so far left out */
void lru_cast_tune_cost_reset(void)
{
	unsigned long flags;
	int type, cpu;

	spin_lock_irqsave(&lru_cast_tune_cost_lock, flags);
	for (type = 0; type < ANON_AND_FILE; type++) {
		u64 w = 0, n = 0;

		for_each_possible_cpu(cpu) {
			w += per_cpu(lru_cast_tune_wait[type], cpu);
			n += per_cpu(lru_cast_tune_refaults[type], cpu);
		}
		WRITE_ONCE(lru_cast_tune_sum[type], 0);
		WRITE_ONCE(lru_cast_tune_n[type], 0);
		lru_cast_tune_last_wait[type] = w;
		lru_cast_tune_last_n[type] = n;
	}
	lru_cast_tune_last_esum = lru_cast_tune_last_en = 0;
	for_each_possible_cpu(cpu) {
		lru_cast_tune_last_esum += per_cpu(lru_cast_tune_evict_ns, cpu);
		lru_cast_tune_last_en += per_cpu(lru_cast_tune_evicts, cpu);
	}
	WRITE_ONCE(lru_cast_tune_esum, 0);
	WRITE_ONCE(lru_cast_tune_en, 0);
	spin_unlock_irqrestore(&lru_cast_tune_cost_lock, flags);
}

/* the estimated cost of each type's refault, in ns */
static void lru_cast_tune_costs(u64 *ca, u64 *cf)
{
	int sw = clamp(READ_ONCE(vm_swappiness), 1, MAX_SWAPPINESS - 1);
	u64 sa = READ_ONCE(lru_cast_tune_sum[0]), na = READ_ONCE(lru_cast_tune_n[0]);
	u64 sf = READ_ONCE(lru_cast_tune_sum[1]), nf = READ_ONCE(lru_cast_tune_n[1]);
	u64 m = na + nf ? div64_u64(sa + sf, na + nf) : 1;
	u64 se = READ_ONCE(lru_cast_tune_esum), ne = READ_ONCE(lru_cast_tune_en);

	/* (prior + sum) / (1 + n) */
	*ca = div64_u64(div_u64(m * (MAX_SWAPPINESS - sw), sw) + sa, 1 + na);
	*cf = div64_u64(m + sf, 1 + nf);
	/*
	 * A page of anon a split loses is also written out: in a steady
	 * state a miss is an eviction. File's is dropped (its writeback is
	 * owed whether it stays or not).
	 */
	if (ne)
		*ca += div64_u64(se, ne);
	*ca = max(*ca, 1ULL);
	*cf = max(*cf, 1ULL);
}

/*
 * The estimated costs, no further apart than swappiness can set them:
 * 1 : (MAX_SWAPPINESS - 1) either way. The bound is on how extreme the
 * measure may be, and the measure weighs the two types alike.
 */
static void lru_cast_tune_costs_bounded(u64 *ca, u64 *cf)
{
	lru_cast_tune_costs(ca, cf);
	*ca = clamp(*ca, max(div_u64(*cf, MAX_SWAPPINESS - 1), 1ULL),
		    *cf * (MAX_SWAPPINESS - 1));
}

/* the weights of anon's and file's misses: their costs, in 16 bits */
void lru_cast_tune_weights(u64 *wa, u64 *wf)
{
	u64 ca, cf;
	int shift;

	lru_cast_tune_costs_bounded(&ca, &cf);
	shift = max(fls64(max(ca, cf)) - 16, 0);
	*wa = max(ca >> shift, 1ULL);
	*wf = max(cf >> shift, 1ULL);
}

/* the swappiness the estimate amounts to: 200 * file / (anon + file) */
static unsigned int lru_cast_estimated_swappiness(void)
{
	u64 ca, cf;

	lru_cast_tune_costs_bounded(&ca, &cf);
	return div64_u64(MAX_SWAPPINESS * cf, ca + cf);
}

/* anon's share, the swappiness the estimated costs amount to, and the costs */
int lru_cast_tune_estimate_show(char *buf)
{
	int len = sysfs_emit(buf, "share %u/1000\nswappiness %u\n",
			     lru_cast_param_share(), lru_cast_estimated_swappiness());
	u64 ca, cf;
	int i;

	lru_cast_tune_costs(&ca, &cf);
	len += sysfs_emit_at(buf, len,
			     "cost anon %llu ns (%llu waits) file %llu ns (%llu waits)\n",
			     ca, READ_ONCE(lru_cast_tune_n[0]),
			     cf, READ_ONCE(lru_cast_tune_n[1]));
	len += sysfs_emit_at(buf, len, "swap-out %llu ns (%llu pages)\n",
			     READ_ONCE(lru_cast_tune_en) ?
			     div64_u64(READ_ONCE(lru_cast_tune_esum), READ_ONCE(lru_cast_tune_en)) : 0,
			     READ_ONCE(lru_cast_tune_en));
	len += sysfs_emit_at(buf, len, "in flight %d\n", lru_cast_wait_inflight());
	/* what the refaults cost on each queue: the CPUs, and each device */
	len += lru_cast_wait_stat_show(buf, len, "cpu", &lru_cast_wait_cpu_stat);
	for (i = 0; i <= LRU_CAST_WAIT_DEVS; i++) {
		u64 id = READ_ONCE(lru_cast_wait_devs[i].id);
		char what[32];

		if (i < LRU_CAST_WAIT_DEVS && !id)
			continue;
		if (i < LRU_CAST_WAIT_DEVS)
			snprintf(what, sizeof(what), "bdi %llu", id);
		else
			strscpy(what, "other", sizeof(what));
		len += lru_cast_wait_stat_show(buf, len, what, &lru_cast_wait_devs[i].stat);
	}
	return len;
}
