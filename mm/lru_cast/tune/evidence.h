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

/*
 * The curve's evidence ages by 1/4 a tick, C/4 of what memory takes in:
 * at the rate the cache turns over, a time constant of about one cache's
 * worth. And by as much while it holds over LRU_CAST_TUNE_HELD accesses of
 * both types, as the share's test can take in 64 bits (share.c): a
 * weight of 16 bits squared, times the disagreements in 1/256.
 */
#define LRU_CAST_TUNE_AGE_SHIFT	2
#define LRU_CAST_TUNE_HELD	(1L << 20)

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
void lru_cast_tune_ev_age(struct lru_cast_tune_evidence *ev);
void lru_cast_tune_ev_forget_grid(struct lru_cast_tune_evidence *ev);
u64 lru_cast_tune_ev_pdis(const struct lru_cast_tune_evidence *ev, int i, int j);

#endif /* _MM_LRU_CAST_TUNE_EVIDENCE_H */
