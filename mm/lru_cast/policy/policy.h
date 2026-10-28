/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CAST -- the policy: the main cache, run inside reclaim.
 *
 *   params.c   its operating point: per type the decay and W's share, and
 *              anon's share of memory -- set by tune/ or by hand
 *   ring.c     M, the active folios, and its lowest found by a sweep
 *   reclaim.c  the victims: W's excess admitted to M, then M's lowest
 *   ghost.c    the history an evicted folio leaves in its shadow
 *   balance.c  which type reclaim takes from, and the clean-file floor
 *
 * Built on core/ alone. mm/vmscan.c reaches it through <linux/lru_cast.h>.
 */
#ifndef _MM_LRU_CAST_POLICY_H
#define _MM_LRU_CAST_POLICY_H

#include "core/core.h"

/* params.c: set there only; read here, inline */
struct lru_cast_params {
	unsigned int le[ANON_AND_FILE];	/* half-life C * 2^(8 - le) pages taken in; 0: no decay */
	unsigned int w[ANON_AND_FILE];	/* W's share of the type, thousandths; 0: the gate */
	unsigned int share;		/* anon's share of memory, thousandths */
};
extern struct lru_cast_params lru_cast_params;

void lru_cast_param_set_le(int type, unsigned int le);
void lru_cast_param_set_w(int type, unsigned int permille);
void lru_cast_param_set_share(unsigned int permille);
/* anon's share held at a split once, by whichever wants it first; the curves move it from there */
bool lru_cast_param_share_held(void);
bool lru_cast_param_hold_share(unsigned int permille);
void lru_cast_param_free_share(void);

static inline unsigned int lru_cast_param_le(int type)
{
	return READ_ONCE(lru_cast_params.le[type]);
}

static inline unsigned int lru_cast_param_w(int type)
{
	return READ_ONCE(lru_cast_params.w[type]);
}

static inline unsigned int lru_cast_param_share(void)
{
	return READ_ONCE(lru_cast_params.share);
}

/* reclaim.c: under the lruvec's lru_lock */
#define LRU_CAST_ADMIT	2	/* W's tail admitted per page reclaim asks for */
#define LRU_CAST_W_SLACK	32	/* pages W may hold over its share as folios come in */
unsigned long lru_cast_w_admit(struct lruvec *lruvec, int type, unsigned long *nr_w,
			    unsigned long *nr_m, unsigned long slack, unsigned long budget);

/* ring.c: under the lruvec's lru_lock; admit and min after prepare, in the same hold */
long lru_cast_bucket(struct folio *folio, int type);
void lru_cast_ring_prepare(struct lruvec *lruvec, int type);
void lru_cast_ring_put(struct lruvec *lruvec, struct folio *folio, int type);
void lru_cast_ring_out(struct folio *folio);
void lru_cast_ring_admit(struct lruvec *lruvec, struct folio *folio, int type);
struct folio *lru_cast_ring_min(struct lruvec *lruvec, int type, long *bucket);
void lru_cast_ring_reset(struct lruvec *lruvec);
void lru_cast_ring_init(struct lruvec *lruvec);
void lru_cast_ring_exit(struct lruvec *lruvec);
void lru_cast_ring_reparent(struct lruvec *child, struct lruvec *parent);
int lru_cast_sweep_show(char *buf);

/* ghost.c */
void lru_cast_ghosts_forget(void);

/* balance.c */
unsigned int lru_cast_floor_ratio(void);
int lru_cast_set_floor_ratio(unsigned int percent);
int lru_cast_floor_held_show(char *buf);
int lru_cast_balance_show(char *buf);

#endif /* _MM_LRU_CAST_POLICY_H */
