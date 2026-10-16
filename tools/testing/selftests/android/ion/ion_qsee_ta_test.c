// SPDX-License-Identifier: GPL-2.0-only
/* Exercise the QSEE TA heap, including the 2674-page gfvenus allocation. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>

#include "ion.h"

#define TA_SIZE 10952704
#define TEST_SKIP 4
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static int alloc_buffer(int ion, unsigned int mask, size_t size,
			unsigned int flags)
{
	struct ion_allocation_data data = {
		.len = size,
		.heap_id_mask = mask,
		.flags = flags,
	};
	int ret;

	if (ioctl(ion, ION_IOC_ALLOC, &data)) {
		ret = -errno;
		fprintf(stderr, "ION allocation of %zu bytes, flags=%u: %s\n",
			size, flags, strerror(errno));
		return ret;
	}
	return data.fd;
}

static int sync_buffer(int fd, unsigned long flags)
{
	struct dma_buf_sync sync = { .flags = flags | DMA_BUF_SYNC_RW };
	int ret;

	do {
		ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
	} while (ret && (errno == EINTR || errno == EAGAIN));
	if (ret)
		perror("DMA_BUF_IOCTL_SYNC");
	return ret;
}

/* Check zeroing on reuse, mmap access and isolation from a live small buffer. */
static int check_buffer(int fd, size_t size, unsigned char pattern,
			int expect_zero)
{
	unsigned char *ptr;
	size_t i;
	int ret = 0;

	ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (ptr == MAP_FAILED) {
		ret = -errno;
		perror("mmap");
		return ret;
	}
	if (sync_buffer(fd, DMA_BUF_SYNC_START)) {
		ret = -1;
		goto unmap;
	}
	for (i = 0; i < size; i++) {
		if (ptr[i] != (expect_zero ? 0 : pattern)) {
			fprintf(stderr, "Unexpected data at byte %zu: %02x\n",
				i, ptr[i]);
			ret = -1;
			break;
		}
	}
	if (!ret && expect_zero)
		memset(ptr, pattern, size);
	if (sync_buffer(fd, DMA_BUF_SYNC_END))
		ret = -1;
unmap:
	munmap(ptr, size);
	return ret;
}

int main(void)
{
	struct ion_heap_data heaps[32] = { 0 };
	struct ion_heap_query query = {
		.cnt = 32,
		.heaps = (uintptr_t)heaps,
	};
	const size_t sizes[] = { TA_SIZE, 1, 65537 };
	unsigned int mask = 0, i, round, flags;
	int ion, small = -1, fd = -1, ret = 1;

	ion = open("/dev/ion", O_RDWR | O_CLOEXEC);
	if (ion < 0) {
		perror("SKIP: /dev/ion");
		return TEST_SKIP;
	}
	if (ioctl(ion, ION_IOC_HEAP_QUERY, &query)) {
		perror("ION_IOC_HEAP_QUERY");
		goto out;
	}
	for (i = 0; i < query.cnt && i < 32; i++) {
		if (!strncmp(heaps[i].name, "qsecom_ta", MAX_HEAP_NAME) &&
		    heaps[i].heap_id < 32) {
			mask = 1U << heaps[i].heap_id;
			break;
		}
	}
	if (!mask) {
		puts("SKIP: qsecom_ta heap is absent");
		ret = TEST_SKIP;
		goto out;
	}
	printf("qsecom_ta mask=%#x; gfvenus request=%u bytes\n", mask, TA_SIZE);
	fflush(stdout);
	for (round = 0; round < 8; round++) {
		flags = round & 1 ? ION_FLAG_CACHED : 0;
		/* Retain one page to exercise aligned allocations at an offset. */
		small = alloc_buffer(ion, mask, 4096, flags);
		if (small < 0 || check_buffer(small, 4096, 0x5a, 1))
			goto out;
		for (i = 0; i < ARRAY_SIZE(sizes); i++) {
			fd = alloc_buffer(ion, mask, sizes[i], flags);
			if (fd < 0 || check_buffer(fd, sizes[i], 0xa5, 1) ||
			    check_buffer(fd, sizes[i], 0xa5, 0) ||
			    check_buffer(small, 4096, 0x5a, 0))
				goto out;
			close(fd);
			fd = -1;
		}
		close(small);
		small = -1;
	}
	puts("PASS: 8 rounds, cached/uncached, TA size, reuse, zeroing and isolation");
	ret = 0;
out:
	if (fd >= 0)
		close(fd);
	if (small >= 0)
		close(small);
	close(ion);
	return ret;
}
