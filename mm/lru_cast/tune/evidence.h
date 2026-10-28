/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CAST -- what the controllers decide on (evidence.c): per cache of
 * a type's sample its waits and its disagreements with the decay grid's
 * centre, and the patterns of the curve's waits; all in 1/256, decaying
 * together.
 */
#ifndef _MM_LRU_CAST_TUNE_EVIDENCE_H
#define _MM_LRU_CAST_TUNE_EVIDENCE_H

#include "tune/shape.h"

#define LRU_CAST_TUNE_PATS		(1 << (LRU_CAST_TUNE_SHARES + 1))

struct lru_cast_tune_evidence {
	s64 miss[LRU_CAST_TUNE_N];		/* the accesses cache m waited for */
	s64 dis[LRU_CAST_TUNE_N];		/* ... on which it and the centre disagree */
	s64 evd;		/* the waits of a cache of nothing */
	/*
	 * The accesses by which of the curve's points (bit p: p tenths) they
	 * cost a wait at. Two points disagree on an access when one bit is
	 * set and the other not: their disagreements, lru_cast_tune_ev_pdis(), are a sum
	 * over the patterns seen -- a dozen or so, the curve nesting almost
	 * always -- where counting them per pair would be 55 counts for
	 * every access.
	 */
	s32 pat[LRU_CAST_TUNE_PATS];
	DECLARE_BITMAP(pats, LRU_CAST_TUNE_PATS);
};

void lru_cast_tune_ev_reset(struct lru_cast_tune_evidence *ev);
void lru_cast_tune_ev_note(struct lru_cast_tune_evidence *ev, const bool *hit, u16 wait, bool full);
void lru_cast_tune_ev_decay(struct lru_cast_tune_evidence *ev);
void lru_cast_tune_ev_forget_grid(struct lru_cast_tune_evidence *ev);
u64 lru_cast_tune_ev_pdis(const struct lru_cast_tune_evidence *ev, int i, int j);

#endif /* _MM_LRU_CAST_TUNE_EVIDENCE_H */
