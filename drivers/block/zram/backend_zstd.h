/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _ZRAM_BACKEND_ZSTD_H_
#define _ZRAM_BACKEND_ZSTD_H_

struct zcomp_zstd_ctx;

#ifdef CONFIG_ZRAM_ZSTD_PARAMS
bool zcomp_zstd_level_valid(int level);
int zcomp_zstd_create(struct zcomp_zstd_ctx **ctx, int level);
void zcomp_zstd_destroy(struct zcomp_zstd_ctx *ctx);
int zcomp_zstd_compress(struct zcomp_zstd_ctx *ctx, const void *src,
			void *dst, unsigned int *dst_len);
int zcomp_zstd_decompress(struct zcomp_zstd_ctx *ctx, const void *src,
			  unsigned int src_len, void *dst);
#else
static inline bool zcomp_zstd_level_valid(int level)
{
	return false;
}
#endif

#endif
