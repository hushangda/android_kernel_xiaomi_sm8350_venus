// SPDX-License-Identifier: GPL-2.0-only
/* Bounded on-device DMA-heap checks. Only allocates its own buffers/fds.
 * CPU mapping/sync checks do not prove coherency with GPU/camera devices.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../../../../include/uapi/linux/dma-heap.h"

#define CHECK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL line %d: %s errno=%d (%s)\n", \
		__LINE__, #x, errno, strerror(errno)); exit(1); \
} } while (0)

static unsigned int page_size;

static int fd_count(void)
{
	DIR *dir = opendir("/proc/self/fd");
	struct dirent *entry;
	int count = 0;

	CHECK(dir);
	while ((entry = readdir(dir)))
		if (entry->d_name[0] != '.')
			count++;
	closedir(dir);
	return count;
}

static void sync_buffer(int fd, unsigned long long flags)
{
	struct dma_buf_sync data = { .flags = flags };
	int ret;

	do {
		ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &data);
	} while (ret < 0 && (errno == EINTR || errno == EAGAIN));
	CHECK(ret == 0);
}

static void check_allocation(int heap, size_t len, unsigned int mode)
{
	struct dma_heap_allocation_data data = {
		.len = len, .fd_flags = mode,
	};
	size_t mapped = (len + page_size - 1) & ~(size_t)(page_size - 1);
	unsigned char *ptr;
	int prot = (mode & O_ACCMODE) == O_RDWR ? PROT_READ | PROT_WRITE : PROT_READ;

	CHECK(ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data) == 0);
	CHECK((fcntl(data.fd, F_GETFL) & O_ACCMODE) == (int)(mode & O_ACCMODE));
	CHECK(!!(fcntl(data.fd, F_GETFD) & FD_CLOEXEC) == !!(mode & O_CLOEXEC));
	if ((mode & O_ACCMODE) == O_WRONLY) {
		CHECK(close(data.fd) == 0);
		return;
	}
	ptr = mmap(NULL, mapped, prot, MAP_SHARED, data.fd, 0);
	CHECK(ptr != MAP_FAILED);
	sync_buffer(data.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
	for (size_t i = 0; i < mapped; i++)
		CHECK(ptr[i] == 0);
	sync_buffer(data.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
	if ((mode & O_ACCMODE) == O_RDWR) {
		sync_buffer(data.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
		memset(ptr, 0xa5, mapped);
		sync_buffer(data.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
		sync_buffer(data.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
		for (size_t i = 0; i < mapped; i++)
			CHECK(ptr[i] == 0xa5);
		sync_buffer(data.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
	} else {
		void *write_map = mmap(NULL, mapped, PROT_READ | PROT_WRITE,
				       MAP_SHARED, data.fd, 0);
		CHECK(write_map == MAP_FAILED && errno == EACCES);
	}
	CHECK(munmap(ptr, mapped) == 0);
	CHECK(close(data.fd) == 0);
}

static void check_error(int heap, struct dma_heap_allocation_data data)
{
	errno = 0;
	CHECK(ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data) == -1 && errno == EINVAL);
}

static void check_heap(const char *name)
{
	char path[128];
	int heap, before;
	struct dma_heap_allocation_data data = {
		.len = 4096, .fd_flags = O_RDWR | O_CLOEXEC,
	};

	CHECK(snprintf(path, sizeof(path), "/dev/dma_heap/%s", name) < (int)sizeof(path));
	heap = open(path, O_RDONLY | O_CLOEXEC);
	CHECK(heap >= 0);
	before = fd_count();
	for (int i = 0; i < 16; i++) {
		check_allocation(heap, page_size + 1, O_RDWR | O_CLOEXEC);
		check_allocation(heap, 65536, O_RDWR);
	}
	check_allocation(heap, 1, O_RDONLY | O_CLOEXEC);
	check_allocation(heap, 1, O_WRONLY);
	data.len = 0;
	check_error(heap, data);
	data.len = UINT64_MAX;
	check_error(heap, data);
	data.len = 4096;
	data.heap_flags = 1;
	check_error(heap, data);
	data.heap_flags = 0;
	data.fd_flags |= O_CREAT;
	check_error(heap, data);
	data.fd_flags = O_RDWR | O_CLOEXEC;
	data.fd = 1;
	check_error(heap, data);
	data.fd = 0;
	/* Readable argument but non-writable result: allocation must be undone. */
	void *arg = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(arg != MAP_FAILED);
	memcpy(arg, &data, sizeof(data));
	CHECK(mprotect(arg, page_size, PROT_READ) == 0);
	for (int i = 0; i < 32; i++) {
		errno = 0;
		CHECK(ioctl(heap, DMA_HEAP_IOCTL_ALLOC, arg) == -1 && errno == EFAULT);
	}
	CHECK(munmap(arg, page_size) == 0);
	CHECK(fd_count() == before);
	CHECK(close(heap) == 0);
	printf("PASS %s: 34 allocations, zeroing, mmap, sync, access modes, invalid parameters, 32 copyout failures without fd leaks\n", name);
}

int main(void)
{
	page_size = sysconf(_SC_PAGESIZE);
	CHECK(page_size && !(page_size & (page_size - 1)));
	printf("abi_bits=%zu uid=%u page_size=%u\n", sizeof(void *) * 8, getuid(), page_size);
	check_heap("system");
	check_heap("system-uncached");
	return 0;
}
