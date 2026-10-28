// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- the curve's worlds.
 *
 * A point of the curve is a world: memory held at that split, as the real
 * cache would be -- anon's cache at k tenths and file's at the rest are one
 * cache of all memory's sample, and on a miss with it full the type over
 * its share gives: the split held, which reclaim presses toward. A type short of its share leaves the
 * rest to the other: a world is what memory would be at its split, not at
 * one where each type fills its share whatever it needs.
 *
 * What each type holds in a world follows from that rule alone. A type at
 * or under its share gives nothing, so it holds every page it has read, up
 * to its share; over it, it holds what the other leaves. And what it holds
 * is the top of its order (sim.c): the curve is read from the ranks, the
 * worlds kept as numbers, not as caches.
 */
#include "tune/world.h"

/* all memory's sample: the worlds' size */
static unsigned long lru_cast_tune_all = 512;

void lru_cast_tune_world_reset(void)
{
	WRITE_ONCE(lru_cast_tune_all, 512);
}

void lru_cast_tune_world_set_size(unsigned long all)
{
	WRITE_ONCE(lru_cast_tune_all, all);
}

/*
 * The pages @type holds in world @k -- anon's cache at k tenths, file's at
 * the rest -- the types having read @read[0] and @read[1] pages: one of
 * them read no more than its share holds all it read, and the other the
 * rest; else each holds its share.
 */
unsigned long lru_cast_tune_world_size(int type, int k, const unsigned int *read)
{
	unsigned long all = READ_ONCE(lru_cast_tune_all), share[ANON_AND_FILE], held[ANON_AND_FILE];

	share[0] = k * all / LRU_CAST_TUNE_SHARES;
	share[1] = all - share[0];
	if (read[0] <= share[0]) {
		held[0] = read[0];
		held[1] = all - held[0];
	} else if (read[1] <= share[1]) {
		held[1] = read[1];
		held[0] = all - held[1];
	} else {
		held[0] = share[0];
		held[1] = share[1];
	}
	return held[type];
}
