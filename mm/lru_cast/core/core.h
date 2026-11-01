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
 * A word not read for long is marked stale (below).
 */
#define LRU_CAST_W_GAP		8
#define LRU_CAST_W_OWNER		14
#define LRU_CAST_OWNER_BITS	10
#define LRU_CAST_OWNER_MASK	(((1U << LRU_CAST_OWNER_BITS) - 1) << LRU_CAST_W_OWNER)
#define LRU_CAST_W_BITS		BITS_PER_TYPE(u32)	/* as lru_cast_word() holds it */

/*
 * The history packed into its LRU_CAST_W_HISTORY_BITS low bits, the tag's
 * bits taken out, as a shadow carries it (policy/ghost.c); and back.
 */
#define LRU_CAST_W_HISTORY_BITS	(LRU_CAST_W_BITS - LRU_CAST_OWNER_BITS)

static inline u32 lru_cast_history_pack(u32 w)
{
	return (w & ((1U << LRU_CAST_W_OWNER) - 1)) |
	       (w >> (LRU_CAST_W_OWNER + LRU_CAST_OWNER_BITS)) << LRU_CAST_W_OWNER;
}

static inline u32 lru_cast_history_unpack(unsigned long h)
{
	h &= (1UL << LRU_CAST_W_HISTORY_BITS) - 1;
	return (h & ((1U << LRU_CAST_W_OWNER) - 1)) |
	       (h >> LRU_CAST_W_OWNER) << (LRU_CAST_W_OWNER + LRU_CAST_OWNER_BITS);
}

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

/*
 * The age of a word's last read. A tick is 8 bits and wraps: the tick now
 * less the one stamped is the age while it is under 256 ticks, and a word
 * not read for longer would show its read as recent again. So the age has
 * more rules, kept here and nowhere else -- lru_cast_age() and
 * lru_cast_stale_last() are the only readings of a word's tick,
 * lru_cast_push() and lru_cast_mark() the only writings of it:
 *
 *	stale	a word read last LRU_CAST_STALE_AGE ticks ago or more is
 *		marked stale (lru_cast_mark(); clock.c looks over a part of
 *		the words at each tick, each in LRU_CAST_STALE_PASS ticks, so
 *		that it is marked well before 256). Its tick is then the
 *		read's, in units of LRU_CAST_STALE_UNIT ticks and modulo
 *		LRU_CAST_STALE_UNITS: the read is taken at the start of its
 *		unit, as old as it is and growing older, until it is read
 *		again; its count stays, its gaps go (the reads are taken as
 *		all at the last).
 *	deep	a stale word whose read is LRU_CAST_STALE_DEEP units old is
 *		marked deep, before its unit could wrap: its read is older
 *		than any a word tells, and only its count is kept. At the
 *		steepest decay and the weakest alike it is then past decaying
 *		(some 32 half-lives at the weakest).
 *
 * The mark is the gap codes 0 and 7 -- the read before the last within a
 * tick of it, the one before that 128 or more ticks earlier -- which
 * lru_cast_push() does not make: it codes that gap 6. A deep word's tick
 * is LRU_CAST_STALE_DEEP_TICK, which no unit is.
 */
#define LRU_CAST_STALE_AGE	192
#define LRU_CAST_STALE_PASS	32
#define LRU_CAST_STALE_SHIFT	6
#define LRU_CAST_STALE_UNIT	(1UL << LRU_CAST_STALE_SHIFT)	/* > LRU_CAST_STALE_PASS + 1 */
#define LRU_CAST_STALE_UNITS	255
#define LRU_CAST_STALE_DEEP	(LRU_CAST_STALE_UNITS - 2)
#define LRU_CAST_STALE_DEEP_TICK	0xffU
#define LRU_CAST_W_GAPS		(0x3fU << LRU_CAST_W_GAP)
#define LRU_CAST_W_STALE	(7U << (LRU_CAST_W_GAP + 3))

/* @w is marked stale, or deep */
static inline bool lru_cast_stale(u32 w)
{
	return (w >> 24) && (w & LRU_CAST_W_GAPS) == LRU_CAST_W_STALE;
}

/* @w is marked deep */
static inline bool lru_cast_deep(u32 w)
{
	return lru_cast_stale(w) && (w & 0xffU) == LRU_CAST_STALE_DEEP_TICK;
}

/* the ticks since @w's last read, the clock at @tick, 255 for any more; @w read at least once */
static inline u8 lru_cast_age(u32 w, unsigned long tick)
{
	return lru_cast_stale(w) ? 255 : (u8)(tick - (w & 0xffU));
}

/* the units since the unit stamped in a stale word that is not deep */
static inline unsigned long lru_cast_stale_units(u32 w, unsigned long tick)
{
	return ((tick >> LRU_CAST_STALE_SHIFT) % LRU_CAST_STALE_UNITS + LRU_CAST_STALE_UNITS -
		(w & 0xffU)) % LRU_CAST_STALE_UNITS;
}

/* the tick a stale word not deep takes its read at: the start of its unit */
static inline unsigned long lru_cast_stale_last(u32 w, unsigned long tick)
{
	unsigned long unit = (tick >> LRU_CAST_STALE_SHIFT) - lru_cast_stale_units(w, tick);

	return unit << LRU_CAST_STALE_SHIFT;
}

/* @w marked stale, or deep, if its last read is old enough to be; else 0 */
static inline u32 lru_cast_mark(u32 w, unsigned long tick)
{
	u8 age;

	if (!(w >> 24) || lru_cast_deep(w))
		return 0;
	if (lru_cast_stale(w))
		return lru_cast_stale_units(w, tick) >= LRU_CAST_STALE_DEEP ?
		       w | LRU_CAST_STALE_DEEP_TICK : 0;
	age = lru_cast_age(w, tick);
	if (age < LRU_CAST_STALE_AGE)
		return 0;
	return (w & ~(LRU_CAST_W_GAPS | 0xffU)) | LRU_CAST_W_STALE |
	       ((tick - age) >> LRU_CAST_STALE_SHIFT) % LRU_CAST_STALE_UNITS;
}

/*
 * @old with a read at tick @t pushed in, its count @n; the tag kept. The
 * gap back to the read before is its age; to the one before that, the gap
 * it had -- of a stale word, as far back as a gap goes.
 */
static inline u32 lru_cast_push(u32 old, u8 t, u32 n)
{
	u32 g1 = old >> 24 ? lru_cast_gap_code(lru_cast_age(old, t)) : 0;
	u32 g2 = lru_cast_stale(old) ? 7 : (old >> LRU_CAST_W_GAP) & 7;

	if (!g1 && g2 == 7)
		g2 = 6;			/* not the stale mark */
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
unsigned long lru_cast_mark_stale(unsigned long pfn, unsigned long end, unsigned long tick);

/* clock.c */
void lru_cast_installed(struct folio *folio, unsigned long nr);
void lru_cast_released(int nid, unsigned long nr);
void lru_cast_shadowed(struct pglist_data *pgdat, int type, unsigned long nr);
void lru_cast_returned(struct pglist_data *pgdat, int type, unsigned long nr);
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
void lru_cast_key_init(void);
long lru_cast_key_res(u32 w, unsigned long tick, int le, int res);

/* quarter half-lives */
static inline long lru_cast_key(u32 w, unsigned long tick, int le)
{
	return lru_cast_key_res(w, tick, le, 2);
}
long lru_cast_window_lo(unsigned long tick, int le);

/* implemented above the core (hooks.c) */
enum lru_cast_read {
	LRU_CAST_READ,		/* seen as it happened: a fault, a read(), a mark */
	LRU_CAST_READ_YOUNG,	/* known only by a young bit, found later */
};
void lru_cast_hook_read(struct folio *folio, enum lru_cast_read how);
void lru_cast_hook_installed(long nr, unsigned long size);

#endif /* _MM_LRU_CAST_CORE_H */
