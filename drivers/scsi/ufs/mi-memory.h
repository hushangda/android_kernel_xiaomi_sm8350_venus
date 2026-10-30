/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _UFS_MI_MEMORY_H
#define _UFS_MI_MEMORY_H

#include <linux/spinlock.h>
#include <linux/types.h>

struct ufs_hba;
struct mi_memory;

struct mi_memory_errors {
	spinlock_t lock;
	u64 count;
	char reason[10][32];
};

#ifdef CONFIG_MI_MEMORY_SYSFS
void mi_memory_error_init(struct ufs_hba *hba);
void mi_memory_record_error(struct ufs_hba *hba, const char *reason);
int mi_memory_register(struct ufs_hba *hba);
void mi_memory_unregister(struct ufs_hba *hba);
#else
static inline void mi_memory_error_init(struct ufs_hba *hba) {}
static inline void mi_memory_record_error(struct ufs_hba *hba,
					 const char *reason) {}
static inline int mi_memory_register(struct ufs_hba *hba) { return 0; }
static inline void mi_memory_unregister(struct ufs_hba *hba) {}
#endif
#endif
