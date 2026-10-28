// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- the choice of victims.
 *
 * W holds the newcomers not yet weighed against M. While W is over its
 * share, its tail joins M (lru_cast_w_admit()), in front of reclaim's
 * candidates when its key is below theirs, so it is the first to go; and
 * what reclaim takes is M's lowest as a sweep finds it (policy/ring.c).
 * That is the model's gate -- W's tail meets M's lowest, the lower goes.
 * W is kept to its share as folios come in (policy/ring.c), not only once
 * reclaim runs: left to grow until then, W held all that came in since
 * boot, in the order it came, and reclaim admitted its oldest first -- the
 * folios read most since, a database warmed before a scan -- to take M's
 * lowest from among only those, while the scan waited in W unweighed.
 *
 * A folio leaving W for M has left what legacy reclaim calls the inactive
 * list, as an activated one has there, and ages the nonresident clock the
 * same way: refault distances are measured on it, and with it running on
 * evictions alone a refault would look twice as recent as it is.
 *
 * As folios come in, W may go over its share by LRU_CAST_W_SLACK pages before
 * its excess is admitted, all of it at once: the fixed part of an
 * admission, under the lock every fault adding to the LRU takes, is paid
 * once a slack's worth and not for every folio, and W stays within a few
 * dozen pages of where the model keeps it. Reclaim admits without slack,
 * LRU_CAST_ADMIT times the pages it asks for at most, so the lock is not held
 * over the whole of a large W; what is left waits for the next call, still
 * the oldest of W. A folio taken from
 * M loses PG_active, so what reclaim hands back without a verdict joins
 * W's head.
 *
 * Runs under the lruvec's lru_lock; the caller (mm/vmscan.c) passes in
 * what it knows of the scan and does the size accounting from the counts
 * this hands back.
 */
#include "policy/policy.h"

/*
 * W's excess joins M by key, its tail first, at most @budget pages, once
 * W is over its share by more than @slack; the lists' sizes, *@nr_w and
 * *@nr_m, follow, once per zone. Run where a folio enters W
 * (__lru_cast_add_folio()), so that W holds no more than its share, as in
 * the model, but for the slack; and again by reclaim, should the share
 * have shrunk. Under the lru_lock.
 */
unsigned long lru_cast_w_admit(struct lruvec *lruvec, int type, unsigned long *nr_w,
			    unsigned long *nr_m, unsigned long slack, unsigned long budget)
{
	enum lru_list lru_w = type ? LRU_INACTIVE_FILE : LRU_INACTIVE_ANON;
	enum lru_list lru_m = lru_w + LRU_ACTIVE;
	struct list_head *w = &lruvec->lists[lru_w];
	unsigned long target = (*nr_w + *nr_m) * lru_cast_param_w(type) / 1000;
	unsigned long moved[MAX_NR_ZONES] = { 0 }, admitted = 0, nr_pages;
	int zid;

	if (*nr_w <= target + slack)
		return 0;
	lru_cast_ring_prepare(lruvec, type);
	while (budget && *nr_w > target && !list_empty(w)) {
		struct folio *folio = lru_to_folio(w);

		/* still being read in: it has not arrived, and waits in W until it has */
		if (folio_test_locked(folio))
			break;

		nr_pages = folio_nr_pages(folio);
		list_del(&folio->lru);
		folio_set_active(folio);
		moved[folio_zonenum(folio)] += nr_pages;
		lru_cast_ring_admit(lruvec, folio, type);
		*nr_w -= min(*nr_w, nr_pages);
		*nr_m += nr_pages;
		budget -= min(budget, nr_pages);
		admitted += nr_pages;
	}
	for (zid = 0; admitted && zid < MAX_NR_ZONES; zid++) {
		if (!moved[zid])
			continue;
		lru_cast_update_lru_size(lruvec, lru_w, zid, -moved[zid]);
		lru_cast_update_lru_size(lruvec, lru_m, zid, moved[zid]);
	}
	if (admitted)
		lru_cast_age_nonresident(lruvec, admitted);
	return admitted;
}

unsigned long lru_cast_isolate(struct lruvec *lruvec, struct lru_cast_scan *s,
				struct list_head *dst)
{
	enum lru_list lru_w = s->type ? LRU_INACTIVE_FILE : LRU_INACTIVE_ANON;
	struct list_head *w = &lruvec->lists[lru_w];
	unsigned long nr_w = s->nr_w, nr_m = s->nr_m, scan = 0, nr_pages;
	LIST_HEAD(aside_w);
	LIST_HEAD(aside_m);

	lru_cast_w_admit(lruvec, s->type, &nr_w, &nr_m, 0, s->nr_to_scan * LRU_CAST_ADMIT);
	lru_cast_ring_prepare(lruvec, s->type);

	while (scan < s->nr_to_scan) {
		struct folio *folio;
		bool from_m = true;
		long bm = 0;

		folio = nr_m ? lru_cast_ring_min(lruvec, s->type, &bm) : NULL;
		if (folio) {
			WRITE_ONCE(lruvec->lru_cast[s->type].victim, bm);
		} else if (!list_empty(w)) {
			folio = lru_to_folio(w);
			from_m = false;
		} else {
			break;
		}

		nr_pages = folio_nr_pages(folio);
		s->nr_scanned += nr_pages;
		if (from_m)
			nr_m -= min(nr_m, nr_pages);
		else
			nr_w -= min(nr_w, nr_pages);

		/* not eligible: set aside, back where it was at the end */
		if (folio_zonenum(folio) > s->reclaim_idx) {
			s->skipped[folio_zonenum(folio)] += nr_pages;
			goto aside;
		}
		scan += nr_pages;
		if (!folio_test_lru(folio) || (!s->may_unmap && folio_mapped(folio)))
			goto aside;
		if (unlikely(!folio_try_get(folio)))
			goto aside;
		if (!folio_test_clear_lru(folio)) {
			folio_put(folio);
			goto aside;
		}

		list_move(&folio->lru, dst);
		if (from_m) {
			s->taken_m[folio_zonenum(folio)] += nr_pages;
			folio_clear_active(folio);
			lru_cast_ring_out(folio);
		} else {
			s->taken_w[folio_zonenum(folio)] += nr_pages;
			folio_set_rekey(folio);	/* not placed by a key: reclaim asks it */
		}
		s->nr_taken += nr_pages;
		continue;
aside:
		list_move(&folio->lru, from_m ? &aside_m : &aside_w);
	}

	list_splice(&aside_w, w);
	while (!list_empty(&aside_m)) {
		struct folio *folio = lru_to_folio(&aside_m);

		list_del(&folio->lru);
		lru_cast_ring_put(lruvec, folio, s->type);
	}
	if (s->nr_taken)
		lru_cast_evicted(lruvec_pgdat(lruvec), s->type, s->nr_taken);
	return s->nr_taken;
}

/*
 * Reclaim found @folio young and credited the read. It stays only if that
 * lifted it above the key reclaim was taking at; still at or below,
 * it is as good a victim as before -- the same decision as picking M's
 * lowest again after the credit. Without this, a cache whose every folio is
 * in use would keep them all and make no progress.
 */
bool lru_cast_still_victim(struct folio *folio)
{
	int type = folio_is_file_lru(folio);
	bool still;

	/* not read since its key was taken at or below that one */
	if (!folio_test_rekey(folio))
		return true;
	/* the folio is isolated: its memcg is held, its lruvec read under RCU */
	rcu_read_lock();
	still = lru_cast_bucket(folio, type) <=
		READ_ONCE(folio_lruvec(folio)->lru_cast[type].victim);
	rcu_read_unlock();
	return still;
}
