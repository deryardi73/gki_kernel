// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- the decay: the half-life a type's keys decay with,
 * H = C * 2^(8 - le), chosen by measuring.
 *
 * The grid's caches run at le at the centre and +-1, 2, 4, 8 from it. Per
 * point, its misses and the accesses on which it and the centre disagree
 * (one hits, the other misses). A point whose misses are below the
 * centre's by more than twice the square root of the disagreements is
 * better at 2 sigma -- a sign test: on a disagreement each is as likely to
 * be the one that missed if neither is better. Of those, the centre moves
 * to the one that gains most; the grid moves with it, a new point starting
 * from the nearest old one, and the main cache takes the new le.
 */
#include "tune/decay.h"

static const int lru_cast_tune_off[LRU_CAST_TUNE_DECAY] = { -8, -4, -2, -1, 0, 1, 2, 4, 8 };

/* every cache's le about @centre, each its own: the grid as it starts */
void lru_cast_tune_decay_grid(int centre, int *le, int *src)
{
	int m;

	for (m = 0; m < LRU_CAST_TUNE_DECAY; m++)
		le[m] = clamp(centre + lru_cast_tune_off[m], LRU_CAST_LE_MIN, LRU_CAST_LE_MAX);
	for (m = LRU_CAST_TUNE_WLO; m < LRU_CAST_TUNE_SIM; m++)
		le[m] = centre;
	for (m = 0; m < LRU_CAST_TUNE_SIM; m++)
		src[m] = m;
}

/* the grid moved to @centre, each point from the nearest old one */
void lru_cast_tune_decay_regrid(struct lru_cast_tune_sim *sim, int centre, int *le, int *src)
{
	int m, i;

	for (m = 0; m < LRU_CAST_TUNE_DECAY; m++) {
		int best = 0, d, bd = INT_MAX;

		le[m] = clamp(centre + lru_cast_tune_off[m], LRU_CAST_LE_MIN, LRU_CAST_LE_MAX);
		for (i = 0; i < LRU_CAST_TUNE_DECAY; i++) {
			d = abs(lru_cast_tune_sim_le(sim, i) - le[m]);
			if (d < bd) {
				bd = d;
				best = i;
			}
		}
		src[m] = best;
	}
	/* the w pair at the new decay; the order follows the centre */
	for (m = LRU_CAST_TUNE_WLO; m < LRU_CAST_TUNE_SIM; m++) {
		le[m] = centre;
		src[m] = m;
	}
}

/* the point better than the centre at 2 sigma that gains most, or -1 */
int lru_cast_tune_decay_choose(const struct lru_cast_tune_evidence *ev,
			       struct lru_cast_tune_sim *sim)
{
	int c = lru_cast_tune_sim_le(sim, LRU_CAST_TUNE_CENTER), m, best = -1;
	s64 best_gain = 0;

	for (m = 0; m < LRU_CAST_TUNE_DECAY; m++) {
		s64 gain = ev->miss[LRU_CAST_TUNE_CENTER] - ev->miss[m];

		/* gain > 2 sqrt(dis), all in 1/256: gain^2 > 1024 dis */
		if (m != LRU_CAST_TUNE_CENTER && lru_cast_tune_sim_le(sim, m) != c &&
		    gain > best_gain &&
		    (u64)gain * gain > 1024 * (u64)ev->dis[m]) {
			best_gain = gain;
			best = m;
		}
	}
	return best;
}
