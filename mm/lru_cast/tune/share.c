// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- anon's share of memory.
 *
 * Chosen across the types from their curves: of the eleven splits
 * (0..10 tenths, world.c), the one that loses least -- anon's waits at k
 * tenths and file's at the rest, weighed by what their refaults cost
 * (cost.c). Reclaim presses both types toward the share
 * (policy/balance.c). A curve, measured over the whole range, finds a
 * split a gradient would not: the gain of a loop that does not fit comes
 * all at once, when it does.
 *
 * Nothing switches on how much has been learnt. The share is held, the
 * split memory had when reclaim first wanted it or the curves could first
 * compare, whichever came first, and moves only to a split they show
 * better at 2 sigma; reclaim presses toward it, so with no evidence the
 * split stays about as it is. The curves can compare once the sample has seen LRU_CAST_TUNE_SHARE_SEEN
 * of each type's reads: before, a split taken from memory would be the
 * few pages a booting system has, as it happened to be. The weights of the two types' misses come from
 * the estimate of what their refaults cost, which is the ratio swappiness
 * gives until samples say otherwise. Either leans from what it was told to
 * what it measures as the measure grows.
 *
 * A split that leaves a type nothing is taken only once the sample has
 * seen LRU_CAST_TUNE_SHARE_SEEN of the type's reads (those a cache of nothing waits
 * for, decaying as the rest). Few reads say only that the sample has not
 * met the type's pages read: a program's text is a few hundred pages, of
 * which a sample of LRU_CAST_TUNE_TARGET pages of all memory, in blocks, often holds
 * none, and the type's few cold pages read once would show it needing
 * nothing. Short of that, the
 * type keeps a tenth, what it reads of it.
 */
#include "tune/tune.h"
#include "tune/share.h"
#include "tune/cost.h"
#include "policy/policy.h"

#define LRU_CAST_TUNE_SHARE_SEEN	256

void lru_cast_tune_share_reset(void)
{
	lru_cast_param_free_share();
}

/* @a, @f: anon's and file's evidence */
void lru_cast_tune_share_update(const struct lru_cast_tune_evidence *a,
				const struct lru_cast_tune_evidence *f)
{
	unsigned long pa = lru_cast_tune_type_pages(0), pf = lru_cast_tune_type_pages(1);
	s64 ma[LRU_CAST_TUNE_SHARES + 1], mf[LRU_CAST_TUNE_SHARES + 1];
	s64 cost[LRU_CAST_TUNE_SHARES + 1], gain = 0;
	int k, cur, best;
	u64 wa, wf;

	lru_cast_tune_cost_gather();
	lru_cast_tune_weights(&wa, &wf);
	for (k = 0; k <= LRU_CAST_TUNE_SHARES; k++) {
		ma[k] = k ? READ_ONCE(a->miss[lru_cast_tune_cache_in(0, k)]) : READ_ONCE(a->evd);
		mf[k] = k < LRU_CAST_TUNE_SHARES ?
			READ_ONCE(f->miss[lru_cast_tune_cache_in(1, k)]) : READ_ONCE(f->evd);
		cost[k] = ma[k] * wa + mf[k] * wf;
	}
	/*
	 * The share is held, and starts as the split memory has when reclaim
	 * first wants it (policy/balance.c) or here, when the curves can first
	 * compare: reclaim pressing toward it, the split stays about as it
	 * is. Until then
	 * there is nothing to move. The curves move it only to a split they
	 * show better at 2 sigma. The
	 * spread of the difference between two splits comes from the accesses
	 * on which their points disagree, each moving it by its type's weight,
	 * as likely either way if neither is better.
	 */
	if (!lru_cast_param_share_held()) {
		if (READ_ONCE(a->evd) < LRU_CAST_TUNE_SHARE_SEEN * 256 ||
		    READ_ONCE(f->evd) < LRU_CAST_TUNE_SHARE_SEEN * 256)
			return;
		lru_cast_param_hold_share(DIV_ROUND_CLOSEST(pa * 1000, max(pa + pf, 1UL)));
	}
	/* the split as it is, not rounded: the curves compare at the nearest point */
	cur = best = DIV_ROUND_CLOSEST(lru_cast_param_share() * LRU_CAST_TUNE_SHARES, 1000);
	for (k = 0; k <= LRU_CAST_TUNE_SHARES; k++) {
		s64 g = cost[cur] - cost[k];
		const struct lru_cast_tune_evidence *left =
			!k ? a : k == LRU_CAST_TUNE_SHARES ? f : NULL;
		int i = min(k, cur), j = max(k, cur);
		u64 var = wa * wa * lru_cast_tune_ev_pdis(a, i, j) +
			  wf * wf * lru_cast_tune_ev_pdis(f, LRU_CAST_TUNE_SHARES - j,
							  LRU_CAST_TUNE_SHARES - i);

		/*
		 * misses in 1/256: g^2 > 4 * 256 * var, as g > 32 sqrt(var) --
		 * g, misses times a weight of up to 16 bits, squared would not
		 * fit in 64
		 */
		if (k != cur && left && READ_ONCE(left->evd) < LRU_CAST_TUNE_SHARE_SEEN * 256)
			continue;
		if (g > gain && g > 32 * (s64)int_sqrt64(var)) {
			gain = g;
			best = k;
		}
	}
	if (best != cur && lru_cast_tune_auto())
		lru_cast_param_set_share(best * 1000 / LRU_CAST_TUNE_SHARES);
}
