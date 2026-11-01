// SPDX-License-Identifier: GPL-2.0-only
/* Link the actual kernel Zstd library and include the actual ZRAM backend. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "drivers/block/zram/backend_zstd.c"

static unsigned int alloc_count, fail_at, live_allocs;
static size_t live_bytes;
union allocation { max_align_t alignment; size_t size; };

void *zram_test_alloc(size_t size)
{
	union allocation *p;

	alloc_count++;
	if (fail_at && alloc_count == fail_at)
		return NULL;
	p = calloc(1, sizeof(*p) + size);
	assert(p);
	p->size = size;
	live_allocs++;
	live_bytes += size;
	return p + 1;
}

void zram_test_free(const void *ptr)
{
	union allocation *p;

	if (!ptr)
		return;
	p = (union allocation *)ptr - 1;
	live_bytes -= p->size;
	live_allocs--;
	free(p);
}

int main(void)
{
	struct zcomp_zstd_ctx *ctx = NULL;
	unsigned char src[PAGE_SIZE], compressed[PAGE_SIZE * 2], dst[PAGE_SIZE];
	unsigned int len, i, pattern;
	uint32_t state = 1234;
	int level;
	size_t bytes;

	assert(zcomp_zstd_create(&ctx, 0) == -EINVAL && !ctx);
	assert(zcomp_zstd_create(&ctx, ZSTD_maxCLevel() + 1) == -EINVAL && !ctx);
	for (fail_at = 1; fail_at <= 3; fail_at++) {
		alloc_count = 0;
		assert(zcomp_zstd_create(&ctx, 6) == -ENOMEM);
		assert(!ctx && !live_allocs && !live_bytes);
	}
	fail_at = 0;
	for (level = 1; level <= ZSTD_maxCLevel(); level++) {
		assert(!zcomp_zstd_create(&ctx, level));
		bytes = live_bytes;
		assert(ctx->params.cParams.windowLog <= PAGE_SHIFT);
		for (pattern = 0; pattern < 3; pattern++) {
			for (i = 0; i < PAGE_SIZE; i++) {
				state ^= state << 13;
				state ^= state >> 17;
				state ^= state << 5;
				src[i] = pattern == 0 ? 0 : pattern == 1 ? i % 31 : state;
			}
			len = sizeof(compressed);
			assert(!zcomp_zstd_compress(ctx, src, compressed, &len));
			assert(len && len <= sizeof(compressed));
			memset(dst, 0xa5, sizeof(dst));
			assert(!zcomp_zstd_decompress(ctx, compressed, len, dst));
			assert(!memcmp(src, dst, sizeof(src)));
			assert(zcomp_zstd_decompress(ctx, compressed, len - 1, dst) == -EINVAL);
			compressed[0] ^= 0xff;
			assert(zcomp_zstd_decompress(ctx, compressed, len, dst) == -EINVAL);
			compressed[0] ^= 0xff;
			assert(!zcomp_zstd_decompress(ctx, compressed, len, dst));
			assert(!memcmp(src, dst, sizeof(src)));
			len = 1;
			assert(zcomp_zstd_compress(ctx, src, compressed, &len) == -EINVAL);
			/* A valid frame that expands to less than a page must fail. */
			len = ZSTD_compressCCtx(ctx->cctx, compressed, sizeof(compressed),
						src, 123, ctx->params);
			assert(!ZSTD_isError(len));
			assert(zcomp_zstd_decompress(ctx, compressed, len, dst) == -EINVAL);
		}
		assert(live_bytes == bytes); /* No allocations in compress/decompress. */
		printf("level=%d per-stream-context-bytes=%zu windowLog=%u PASS\n",
		       level, bytes, ctx->params.cParams.windowLog);
		zcomp_zstd_destroy(ctx);
		ctx = NULL;
		assert(!live_allocs && !live_bytes);
	}
	puts("ZRAM actual kernel Zstd round-trip and allocation failure tests: PASS");
	return 0;
}
