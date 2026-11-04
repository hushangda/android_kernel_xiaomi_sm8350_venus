// SPDX-License-Identifier: GPL-2.0-only
/*
 * Expose the existing Qualcomm system allocator through the DMA-heap UAPI.
 * Reuse its page pools, DMA mapping, cache sync and release operations so
 * legacy ION and DMA-heap users share the same platform memory semantics.
 */

#include <linux/dma-heap.h>
#include <linux/ion.h>
#include <linux/msm_ion.h>

#include "msm_ion_priv.h"

static struct dma_buf *msm_system_heap_allocate(struct dma_heap *heap,
					       unsigned long len,
					       unsigned long fd_flags,
					       unsigned long heap_flags)
{
	unsigned int ion_flags = (unsigned long)dma_heap_get_drvdata(heap);

	if (heap_flags)
		return ERR_PTR(-EINVAL);

	return ion_alloc_fd_flags(len, ION_SYSTEM_HEAP_ID, ion_flags, fd_flags);
}

static const struct dma_heap_ops msm_system_heap_ops = {
	.allocate = msm_system_heap_allocate,
};

int msm_ion_register_dma_heaps(struct ion_heap *heap)
{
	struct dma_heap_export_info info = {
		.name = "system",
		.ops = &msm_system_heap_ops,
		.priv = (void *)ION_FLAG_CACHED,
	};
	struct dma_heap *dma_heap;

	/* Never expose secure/carveout heaps through the system heap names. */
	if (heap->type != ION_HEAP_TYPE_MSM_SYSTEM ||
	    heap->id != __ffs(ION_SYSTEM_HEAP_ID))
		return 0;

	/* Both heaps and their allocator are built in and live until shutdown. */
	dma_heap = dma_heap_add(&info);
	if (IS_ERR(dma_heap))
		return PTR_ERR(dma_heap);

	info.name = "system-uncached";
	info.priv = NULL;
	dma_heap = dma_heap_add(&info);
	return PTR_ERR_OR_ZERO(dma_heap);
}
