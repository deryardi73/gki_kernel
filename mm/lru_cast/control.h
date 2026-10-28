/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CAST -- switching it on and off (control.c), and its sysfs
 * directory (sysfs.c). The composition root: these two, with hooks.c, are
 * the only files that see every component.
 */
#ifndef _MM_LRU_CAST_CONTROL_H
#define _MM_LRU_CAST_CONTROL_H

#include <linux/types.h>

int lru_cast_set_enabled(bool on);
int lru_cast_sysfs_init(void);

#endif /* _MM_LRU_CAST_CONTROL_H */
