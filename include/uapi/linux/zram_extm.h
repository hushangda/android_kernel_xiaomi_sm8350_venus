/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_ZRAM_EXTM_H
#define _UAPI_LINUX_ZRAM_EXTM_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* X-Ring 2024 zgroup ABI, also used by the current system_perf_init. */
#define ZRAM_EXTM_PATH_MAX 128
struct zram_extm_target {
	__u64 physical;
	__u64 length;
};
struct zram_extm_bdev {
	__u32 dev_id;
	__u32 reserved1;
	__u32 crypto_info;
	__u32 key_id;
	char bdev_path[ZRAM_EXTM_PATH_MAX];
	__u64 bdev_size;
	__u32 reserved2;
	__u32 nr_targets;
	struct zram_extm_target targets[];
};
#define ZRAM_EXTM_SET_BDEV _IOW(0x5c, 1, struct zram_extm_bdev)
#define ZRAM_EXTM_GET_BDEV _IOWR(0x5c, 2, struct zram_extm_bdev)
#endif
