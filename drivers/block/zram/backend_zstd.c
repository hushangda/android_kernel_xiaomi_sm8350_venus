// SPDX-License-Identifier: GPL-2.0-or-later
/* Explicit ZRAM Zstd levels, using the 5.4 kernel's workspace API. */
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/zstd.h>

#include "backend_zstd.h"

struct zcomp_zstd_ctx {
	ZSTD_CCtx *cctx;
	ZSTD_DCtx *dctx;
	void *cworkspace;
	void *dworkspace;
	ZSTD_parameters params;
};

bool zcomp_zstd_level_valid(int level)
{
	return level >= 1 && level <= ZSTD_maxCLevel();
}

void zcomp_zstd_destroy(struct zcomp_zstd_ctx *ctx)
{
	if (!ctx)
		return;
	vfree(ctx->cworkspace);
	vfree(ctx->dworkspace);
	kfree(ctx);
}

int zcomp_zstd_create(struct zcomp_zstd_ctx **ctx, int level)
{
	struct zcomp_zstd_ctx *zctx;
	size_t size;
	int ret = -ENOMEM;

	if (!zcomp_zstd_level_valid(level))
		return -EINVAL;
	zctx = kzalloc(sizeof(*zctx), GFP_KERNEL);
	if (!zctx)
		return -ENOMEM;

	/* Bound the window/workspace to the actual input, not an unknown size. */
	zctx->params = ZSTD_getParams(level, PAGE_SIZE, 0);
	size = ZSTD_CCtxWorkspaceBound(zctx->params.cParams);
	if (ZSTD_isError(size)) {
		ret = -EINVAL;
		goto error;
	}
	zctx->cworkspace = vzalloc(size);
	if (!zctx->cworkspace)
		goto error;
	zctx->cctx = ZSTD_initCCtx(zctx->cworkspace, size);
	if (!zctx->cctx) {
		ret = -EINVAL;
		goto error;
	}

	size = ZSTD_DCtxWorkspaceBound();
	zctx->dworkspace = vzalloc(size);
	if (!zctx->dworkspace)
		goto error;
	zctx->dctx = ZSTD_initDCtx(zctx->dworkspace, size);
	if (!zctx->dctx) {
		ret = -EINVAL;
		goto error;
	}
	*ctx = zctx;
	return 0;
error:
	zcomp_zstd_destroy(zctx);
	return ret;
}

int zcomp_zstd_compress(struct zcomp_zstd_ctx *ctx, const void *src,
			void *dst, unsigned int *dst_len)
{
	size_t size;

	size = ZSTD_compressCCtx(ctx->cctx, dst, *dst_len, src, PAGE_SIZE,
				ctx->params);
	if (ZSTD_isError(size) || !size || size > *dst_len)
		return -EINVAL;
	*dst_len = size;
	return 0;
}

int zcomp_zstd_decompress(struct zcomp_zstd_ctx *ctx, const void *src,
			  unsigned int src_len, void *dst)
{
	size_t size;

	size = ZSTD_decompressDCtx(ctx->dctx, dst, PAGE_SIZE, src, src_len);
	if (ZSTD_isError(size) || size != PAGE_SIZE)
		return -EINVAL;
	return 0;
}
