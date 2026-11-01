// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- switching it on and off.
 *
 * CAST runs per lruvec: lruvec->lru_cast_on, read under the lru_lock by the
 * add hook and by reclaim, says whether the lruvec's active folios are M
 * (on) or legacy reclaim's active list (off); they are on the active list
 * either way. Switching on walks every lruvec and gives each active folio
 * with no reads recorded one, in batches, dropping the lock between them;
 * the flag flips when that is done (on) or first thing (off). The static
 * key lru_cast_enabled_key covers the hooks that have no lruvec at hand
 * (reads, frees, migration).
 *
 * Boot: lru_cast=0 starts with CAST off, and the state words are not set
 * up until it is first switched on. At run time:
 * /sys/kernel/mm/lru_cast/enabled. While it is off the state words go
 * stale (nothing resets them at free); they are all cleared before it comes
 * back on, and with them goes all it had learnt: the shadows it left
 * (lru_cast_ghosts_forget()) and the controllers (lru_cast_tune_init()).
 *
 * CAST and MGLRU both run on the lists legacy reclaim leaves: one is
 * switched off to them before the other is switched on from them, and
 * never are both on. Either's switch is made under lru_cast_switch_lock
 * (MGLRU's, lru_gen_change_state(), takes it before its own locks), and
 * neither comes on while the other is: -EBUSY.
 */
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/mutex.h>
#include "control.h"
#include "core/core.h"
#include "policy/policy.h"
#include "tune/tune.h"
#include "version.h"

DEFINE_STATIC_KEY_TRUE(lru_cast_enabled_key);

/* what a new lruvec starts with */
static bool lru_cast_active __read_mostly = true;
static DEFINE_MUTEX(lru_cast_switch_lock);

#define LRU_CAST_BATCH	1024

static int __init lru_cast_setup(char *str)
{
	int v;

	if (str && !kstrtoint(str, 0, &v))
		lru_cast_active = v;
	return 0;
}
early_param("lru_cast", lru_cast_setup);

void lru_cast_init_lruvec(struct lruvec *lruvec)
{
	lru_cast_ring_init(lruvec);
	lru_cast_ring_reset(lruvec);
	lruvec->lru_cast_on = lru_cast_active;
}

/* an lruvec freed (its memcg's): no folio is left on it */
void lru_cast_exit_lruvec(struct lruvec *lruvec)
{
	lru_cast_ring_exit(lruvec);
}

/* CAST runs @folio's lruvec; the folio locked or isolated, so its memcg held */
bool lru_cast_folio_on(struct folio *folio)
{
	bool on;

	rcu_read_lock();
	on = lru_cast_on(folio_lruvec(folio));
	rcu_read_unlock();
	return on;
}

/* @child's folios spliced onto @parent's lists, both lru_locks held */
void lru_cast_reparent(struct lruvec *child, struct lruvec *parent)
{
	lru_cast_ring_reparent(child, parent);
}

/*
 * The active list is M as it is; a folio with no reads recorded is given
 * one. The list is gone through off to the side, so that the lock can be
 * let go: a folio leaving meanwhile leaves that list as it would this one.
 */
static void lru_cast_fill(struct lruvec *lruvec)
{
	int type;

	spin_lock_irq(&lruvec->lru_lock);
	for (type = 0; type < ANON_AND_FILE; type++) {
		struct list_head *list = &lruvec->lists[LRU_ACTIVE_ANON + type * LRU_FILE];
		LIST_HEAD(todo);
		int n = 0;

		list_splice_init(list, &todo);
		while (!list_empty(&todo)) {
			struct folio *folio = lru_to_folio(&todo);

			list_del(&folio->lru);
			if (!(lru_cast_read(folio) >> 24))
				lru_cast_access(folio);
			lru_cast_ring_put(lruvec, folio, type);
			if (++n % LRU_CAST_BATCH == 0) {
				spin_unlock_irq(&lruvec->lru_lock);
				cond_resched();
				spin_lock_irq(&lruvec->lru_lock);
			}
		}
	}
	WRITE_ONCE(lruvec->lru_cast_on, true);
	spin_unlock_irq(&lruvec->lru_lock);
}

/* M is the active list already */
static void lru_cast_drain(struct lruvec *lruvec)
{
	spin_lock_irq(&lruvec->lru_lock);
	WRITE_ONCE(lruvec->lru_cast_on, false);
	lru_cast_ring_reset(lruvec);
	spin_unlock_irq(&lruvec->lru_lock);
}

static void lru_cast_walk(void (*fn)(struct lruvec *lruvec))
{
	struct mem_cgroup *memcg = mem_cgroup_iter(NULL, NULL, NULL);

	do {
		int nid;

		for_each_node_state(nid, N_MEMORY)
			fn(mem_cgroup_lruvec(memcg, NODE_DATA(nid)));
		cond_resched();
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)));
}

int lru_cast_set_enabled(bool on)
{
	int err = 0;

	mutex_lock(&lru_cast_switch_lock);
	if (on != READ_ONCE(lru_cast_active)) {
		if (on && lru_gen_enabled()) {
			err = -EBUSY;
			goto out;
		}
		if (on) {
			/* all it needs, or it stays off and says so */
			err = lru_cast_store_init();
			if (!err)
				err = lru_cast_tune_init();
			if (err) {
				pr_warn(LRU_CAST_PROGNAME ": no memory to run on, still off\n");
				goto out;
			}
			lru_cast_clear_states();
			lru_cast_ghosts_forget();
			WRITE_ONCE(lru_cast_active, true);
			static_branch_enable(&lru_cast_enabled_key);
			lru_add_drain_all();
			lru_cast_walk(lru_cast_fill);
		} else {
			WRITE_ONCE(lru_cast_active, false);
			lru_add_drain_all();
			lru_cast_walk(lru_cast_drain);
			static_branch_disable(&lru_cast_enabled_key);
		}
		pr_info(LRU_CAST_PROGNAME ": %s\n", on ? "enabled" : "disabled");
	}
out:
	mutex_unlock(&lru_cast_switch_lock);
	return err;
}

bool lru_cast_is_enabled(void)
{
	return READ_ONCE(lru_cast_active);
}

/* MGLRU's switch: CAST stays as it is while it is made */
void lru_cast_switch_begin(void)
{
	mutex_lock(&lru_cast_switch_lock);
}

void lru_cast_switch_end(void)
{
	mutex_unlock(&lru_cast_switch_lock);
}

static int __init lru_cast_init(void)
{
	lru_cast_key_init();
	lru_cast_prefetch_init();
	if (lru_cast_active && (lru_cast_store_init() || lru_cast_tune_init())) {
		pr_warn(LRU_CAST_PROGNAME ": no memory to run on, off\n");
		lru_cast_set_enabled(false);
	}
	if (!lru_cast_active)
		static_branch_disable(&lru_cast_enabled_key);
	pr_info(LRU_CAST_PROGNAME " v" LRU_CAST_VERSION " by " LRU_CAST_AUTHOR "\n");
	pr_info(LRU_CAST_PROGNAME ": currently %s\n", lru_cast_active ? "enabled" : "disabled");
	return lru_cast_sysfs_init();
}
subsys_initcall(lru_cast_init);
