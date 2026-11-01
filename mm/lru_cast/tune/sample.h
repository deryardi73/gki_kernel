/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CAST -- which pages the controllers sample (sample.c), and how
 * file's are read ahead in the curve's caches (readahead.c).
 */
#ifndef _MM_LRU_CAST_TUNE_SAMPLE_H
#define _MM_LRU_CAST_TUNE_SAMPLE_H

#include "tune/shape.h"

/* sample.c */
void lru_cast_tune_sample_init(unsigned long total);
u64 lru_cast_tune_sample_page(struct folio *folio);
/* of @pages, how many are sampled */
unsigned long lru_cast_tune_sample_pages(unsigned long pages);

/* readahead.c: under the controllers' lock */
struct lru_cast_tune_ra;
struct lru_cast_tune_ra *lru_cast_tune_ra_alloc(void);
void lru_cast_tune_ra_free(struct lru_cast_tune_ra *ra);
void lru_cast_tune_ra_init(struct lru_cast_tune_ra *ra);
u16 lru_cast_tune_ra_waits(struct lru_cast_tune_ra *ra, u64 block, pgoff_t index,
			   unsigned int nr, u16 miss);

#endif /* _MM_LRU_CAST_TUNE_SAMPLE_H */
