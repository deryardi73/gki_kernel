// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- the main cache's operating point.
 *
 * Three numbers say how the main cache runs, and only these: per type the
 * decay le its keys rank by (policy/ring.c), W's share of its pages
 * (policy/reclaim.c), and anon's share of memory (policy/balance.c). The
 * controllers (tune/) set them from what they measure, or the
 * administrator by hand (sysfs.c); the policy reads them, on every
 * reclaim, inline (policy.h). Each is set here and nowhere else, within its
 * range: lru_cast_params is named in this file and in policy.h's readers
 * only (check-layers.sh).
 */
#include "policy/policy.h"

/*
 * The decay starts at its weakest (core/core.h). A decay too weak costs
 * misses as long as it lasts -- folios read often once and no more stay --
 * and the controllers see them pile up; one too strong costs only after a scan, a few, the
 * folios read often until it began: too few, in the sample, to move the
 * decay back down before the next.
 */
struct lru_cast_params lru_cast_params = {
	.le = { LRU_CAST_LE_MIN, LRU_CAST_LE_MIN },
	.share = 500,
};

void lru_cast_param_set_le(int type, unsigned int le)
{
	WRITE_ONCE(lru_cast_params.le[type], clamp(le, (unsigned int)LRU_CAST_LE_MIN,
						   (unsigned int)LRU_CAST_LE_MAX));
}

void lru_cast_param_set_w(int type, unsigned int permille)
{
	WRITE_ONCE(lru_cast_params.w[type], min(permille, 1000u));
}

void lru_cast_param_set_share(unsigned int permille)
{
	WRITE_ONCE(lru_cast_params.share, min(permille, 1000u));
}

/*
 * The share is held once it has been set at a split: when reclaim first
 * wants it (balance.c) or the curves can first compare (tune/share.c),
 * whichever comes first. Before that it is not used.
 */
static atomic_t lru_cast_share_held = ATOMIC_INIT(0);

bool lru_cast_param_share_held(void)
{
	return atomic_read(&lru_cast_share_held);
}

/* the share held at @permille, unless it is already; true if this held it */
bool lru_cast_param_hold_share(unsigned int permille)
{
	if (atomic_cmpxchg(&lru_cast_share_held, 0, 1))
		return false;
	lru_cast_param_set_share(permille);
	return true;
}

/* the share let go, to be held again: the controllers start over */
void lru_cast_param_free_share(void)
{
	lru_cast_param_set_share(500);
	atomic_set(&lru_cast_share_held, 0);
}
