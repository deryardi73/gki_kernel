// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- W's share of a type's pages, chosen by measuring.
 *
 * A sequential test of the gradient. S sums the misses of w - 0.1C less
 * those of w + 0.1C, N the accesses they disagree on, since w last moved.
 * Every 1/64 of a cache of accesses, t = S / sqrt(N + 1): past +-2, w takes
 * a step of 0.1C / 64 towards the better side, and the evidence is dropped:
 * each step is taken on evidence gathered since the last. Kept, a sum that
 * crossed once would carry w on a step every test whatever followed --
 * and with nothing to choose between the two, a sum looked at again and
 * again crosses 2 sooner or later: w would wander 0.1C at a time where
 * now it only jitters. It starts where the main cache's is.
 */
#include "tune/tune.h"
#include "tune/wshare.h"
#include "policy/policy.h"

#define LRU_CAST_TUNE_WSTEP	(LRU_CAST_TUNE_WUNIT / 10 / 64)	/* 0.1 C / 64 */
#define LRU_CAST_TUNE_WRANGE	(LRU_CAST_TUNE_WUNIT / 10)		/* 0.1 C */

/* the w of each cache: the main cache's for the decay grid, -/+ 0.1C for the pair */
static void lru_cast_tune_w_apply(struct lru_cast_tune_w *w, struct lru_cast_tune_sim *sim)
{
	int m;

	for (m = 0; m < LRU_CAST_TUNE_DECAY; m++)
		lru_cast_tune_sim_set_wq(sim, m, w->wq);
	lru_cast_tune_sim_set_wq(sim, LRU_CAST_TUNE_WLO,
				 w->wq > LRU_CAST_TUNE_WRANGE ? w->wq - LRU_CAST_TUNE_WRANGE : 0);
	lru_cast_tune_sim_set_wq(sim, LRU_CAST_TUNE_WHI, min(w->wq + LRU_CAST_TUNE_WRANGE,
							      (unsigned int)LRU_CAST_TUNE_WUNIT));
}

void lru_cast_tune_w_init(struct lru_cast_tune_w *w, unsigned int permille,
			  struct lru_cast_tune_sim *sim)
{
	memset(w, 0, sizeof(*w));
	w->wq = permille * LRU_CAST_TUNE_WUNIT / 1000;
	w->next_wtest = 8;
	lru_cast_tune_w_apply(w, sim);
}

/* an access of a full sample: the pair disagreeing is a step of the test */
void lru_cast_tune_w_note(struct lru_cast_tune_w *w, bool hit_lo, bool hit_hi)
{
	if (hit_lo != hit_hi) {
		w->ws += hit_hi ? 1 : -1;	/* w - 0.1C missed: +1 */
		w->wn++;
	}
}

/* the pair's caches were set up again: what they showed is gone */
void lru_cast_tune_w_forget(struct lru_cast_tune_w *w)
{
	w->ws = w->wn = 0;
}

/* the sequential test, every 1/64 of a cache of accesses */
void lru_cast_tune_w_test(struct lru_cast_tune_w *w, struct lru_cast_tune_sim *sim, int type,
			  unsigned long events, unsigned long cap)
{
	s64 s = w->ws;
	unsigned int wq = w->wq;

	w->next_wtest = events + max(cap / 64, 8UL);
	/* t > 2: s^2 > 4 (n + 1), with the sign of s */
	if ((u64)(s * s) <= 4 * (u64)(w->wn + 1))
		return;
	if (s > 0)			/* w - 0.1C misses more: grow */
		wq = min(wq + LRU_CAST_TUNE_WSTEP, (unsigned int)LRU_CAST_TUNE_WUNIT);
	else
		wq = wq > LRU_CAST_TUNE_WSTEP ? wq - LRU_CAST_TUNE_WSTEP : 0;
	if (wq == w->wq)
		return;
	w->wq = wq;
	w->wmoves++;
	w->ws = w->wn = 0;		/* each step on evidence of its own */
	lru_cast_tune_w_apply(w, sim);
	if (lru_cast_tune_auto())
		lru_cast_param_set_w(type, wq * 1000 / LRU_CAST_TUNE_WUNIT);
}
