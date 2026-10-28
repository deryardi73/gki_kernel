// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- what the core's events reach. The core (core/) tells of a
 * read and of pages taken in and knows no more; who hears of them is decided
 * here, and only here.
 */
#include "core/core.h"
#include "tune/tune.h"
#include "walk/walk.h"

/*
 * A read: the controllers' sample sees it too. One known only by a young
 * bit is not passed on while sampled pages are protected: their reads
 * reach the controllers as faults, one each (tune/protect.c).
 */
void lru_cast_hook_read(struct folio *folio, enum lru_cast_read how)
{
	if (how == LRU_CAST_READ_YOUNG && lru_cast_protect_active())
		return;
	lru_cast_tune_read(folio);
}

/* those paced by the clock, by pages taken in: re-protection, and the walker */
void lru_cast_hook_installed(long nr, unsigned long size)
{
	lru_cast_protect_installed(nr, size);
	lru_cast_walk_installed(nr, size);
}
