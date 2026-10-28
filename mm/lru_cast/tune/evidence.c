// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- the evidence the controllers decide on.
 *
 * The decay grid's and W's caches keep none while they fill: they differ
 * only once they evict. The curve's points are read from the order's
 * ranks, not from those caches, and keep theirs from the first read: a
 * type whose sample never fills its cache -- a few pages read all the
 * time, a program's text -- would otherwise wait at no split, and the
 * share would give its memory away for nothing. Everything decays by 1/16
 * an evaluation, a time constant of about one cache of accesses.
 */
#include "tune/evidence.h"

void lru_cast_tune_ev_reset(struct lru_cast_tune_evidence *ev)
{
	memset(ev, 0, sizeof(*ev));
}

/*
 * An access of the sample: @hit, the caches it hit in; @wait, the curve's
 * points it costs a wait at (bit p: p tenths, bit 0 a cache of nothing);
 * @full, the caches full. A decay or W cache waits on every miss; the
 * curve's on the misses readahead did not cover.
 */
void lru_cast_tune_ev_note(struct lru_cast_tune_evidence *ev, const bool *hit, u16 wait, bool full)
{
	int m;

	if (wait & 1)
		ev->evd += 256;
	for (m = full ? 0 : LRU_CAST_TUNE_CURVE; m < LRU_CAST_TUNE_N; m++) {
		bool waits = m < LRU_CAST_TUNE_CURVE ? !hit[m] :
			     wait & BIT(m - LRU_CAST_TUNE_CURVE + 1);

		if (waits)
			ev->miss[m] += 256;
		if (hit[m] != hit[LRU_CAST_TUNE_CENTER])
			ev->dis[m] += 256;
	}
	ev->pat[wait] += 256;
	__set_bit(wait, ev->pats);
}

void lru_cast_tune_ev_decay(struct lru_cast_tune_evidence *ev)
{
	unsigned int p;
	int m;

	for (m = 0; m < LRU_CAST_TUNE_N; m++) {
		ev->miss[m] -= ev->miss[m] >> 4;
		ev->dis[m] -= ev->dis[m] >> 4;
	}
	ev->evd -= ev->evd >> 4;
	for_each_set_bit(p, ev->pats, LRU_CAST_TUNE_PATS) {
		ev->pat[p] -= ev->pat[p] >> 4;
		if (!ev->pat[p])
			__clear_bit(p, ev->pats);
	}
}

/*
 * The decay grid and the W pair moved (lru_cast_tune_sim_regrid()): what they showed
 * was of caches no longer there. The curve's points keep their counts:
 * they take the new le together and are weighed only against each other
 * (with evd and pat, which stay too), and what they learnt under the old
 * one fades as everything does. Zeroed, they would sit at nothing next to
 * evd's and pat's counts, and a split would look free of the type's misses
 * until they filled again.
 */
void lru_cast_tune_ev_forget_grid(struct lru_cast_tune_evidence *ev)
{
	int m;

	for (m = 0; m < LRU_CAST_TUNE_CURVE; m++)
		ev->miss[m] = ev->dis[m] = 0;
}

/* the accesses on which the curve's points @i and @j disagree, in 1/256 */
u64 lru_cast_tune_ev_pdis(const struct lru_cast_tune_evidence *ev, int i, int j)
{
	u64 sum = 0;
	unsigned int p;

	for_each_set_bit(p, ev->pats, LRU_CAST_TUNE_PATS)
		if (!(p & BIT(i)) != !(p & BIT(j)))
			sum += ev->pat[p];
	return sum;
}
