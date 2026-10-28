/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CAST -- the core: what a folio's reads were, and when.
 *
 *   store.c  a 32-bit state word per page frame
 *   state.c  the word of a folio: its reads, recorded lock-free
 *   clock.c  the clock the reads are stamped with: pages taken in
 *   key.c    the key a word ranks by, at a decay
 *
 * The core knows nothing of what uses it. It tells of a read and of
 * pages taken in through the two hooks below, which the composition root
 * (hooks.c) implements; everything else in mm/lru_cast/ is built on this
 * header and depends on nothing beneath it.
 */
#ifndef _MM_LRU_CAST_CORE_H
#define _MM_LRU_CAST_CORE_H

#include <linux/lru_cast.h>
#include "compat/kernel.h"
#include "core/prefetch.h"

/* store.c */
int lru_cast_store_init(void);
void lru_cast_clear_states(void);

/*
 * state.c: a folio's state word. Bits 0-7, the tick of its last read;
 * 8-10 and 11-13, the gaps back to the two reads before, a code each
 * (lru_cast_gap_code()); 14-23, its owner tag; 24-31, its read count. What
 * lru_cast_read() and the rest give is the word without the tag: the history.
 */
#define LRU_CAST_W_GAP		8
#define LRU_CAST_W_OWNER		14
#define LRU_CAST_OWNER_BITS	10
#define LRU_CAST_OWNER_MASK	(((1U << LRU_CAST_OWNER_BITS) - 1) << LRU_CAST_W_OWNER)

/*
 * A gap of @d ticks between two reads, in 3 bits: max(fls(d) - 1, 0), the
 * gaps [0, 1], [2, 3], [4, 7] ... [128, 255]. The two older reads are known
 * to within a factor of two of their gaps, the newest exactly.
 */
static inline u32 lru_cast_gap_code(u8 d)
{
	int c = fls(d);

	return c > 1 ? c - 1 : 0;
}

/* @old with a read at tick @t pushed in, its count @n; the tag kept */
static inline u32 lru_cast_push(u32 old, u8 t, u32 n)
{
	u32 g1 = old >> 24 ? lru_cast_gap_code(t - (u8)old) : 0, g2 = (old >> LRU_CAST_W_GAP) & 7;

	return (old & LRU_CAST_OWNER_MASK) | n << 24 | g2 << (LRU_CAST_W_GAP + 3) |
	       g1 << LRU_CAST_W_GAP | t;
}

u32 lru_cast_read(struct folio *folio);
u32 lru_cast_read_pfn(unsigned long pfn);
u32 lru_cast_set_owner(struct folio *folio, u32 owner);
unsigned long lru_cast_find_owner(unsigned long pfn, unsigned long end, u32 owner, u32 *w);
unsigned long lru_cast_find_owners(unsigned long pfn, unsigned long end,
				   const unsigned long *lids, u32 *owner);
void lru_cast_write(struct folio *folio, u32 w);

/* clock.c */
void lru_cast_installed(struct folio *folio, unsigned long nr);
void lru_cast_released(int nid, unsigned long nr);
unsigned long lru_cast_ticks_now(void);		/* every node's ticks, summed */
int lru_cast_clock_show(char *buf);

/* the node's tick; both types run on it (@type is for the callers' sake) */
static inline unsigned long lru_cast_tick(struct pglist_data *pgdat, int type)
{
	return READ_ONCE(pgdat->lru_cast_clock.tick);
}

/* key.c */
/*
 * The weakest decay, H = 128 C: never none. With none a key is the read
 * count alone, with nothing of when: folios read 255 times or more tie,
 * and a set read often once stays however long it is not. At le 1 a
 * folio's reads still count for 8 half-lives of recency at most (log2 255),
 * and among those read that often the last read decides.
 */
#define LRU_CAST_LE_MIN	1
#define LRU_CAST_LE_MAX	14			/* the steepest decay */
long lru_cast_key(u32 w, unsigned long tick, int le);
long lru_cast_key_res(u32 w, unsigned long tick, int le, int res);
long lru_cast_window_lo(unsigned long tick, int le);

/* implemented above the core (hooks.c) */
enum lru_cast_read {
	LRU_CAST_READ,		/* seen as it happened: a fault, a read(), a mark */
	LRU_CAST_READ_YOUNG,	/* known only by a young bit, found later */
};
void lru_cast_hook_read(struct folio *folio, enum lru_cast_read how);
void lru_cast_hook_installed(long nr, unsigned long size);

#endif /* _MM_LRU_CAST_CORE_H */
