/* SPDX-License-Identifier: GPL-2.0 */
/* CAST -- anon's share of memory (share.c), under the controllers' lock */
#ifndef _MM_LRU_CAST_TUNE_SHARE_H
#define _MM_LRU_CAST_TUNE_SHARE_H

#include "tune/evidence.h"

void lru_cast_tune_share_reset(void);
void lru_cast_tune_share_update(const struct lru_cast_tune_evidence *a,
				const struct lru_cast_tune_evidence *f);

#endif /* _MM_LRU_CAST_TUNE_SHARE_H */
