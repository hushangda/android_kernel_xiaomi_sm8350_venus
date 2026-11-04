// SPDX-License-Identifier: GPL-2.0-only
/* Native/compat capability probe. Uses only its own mappings and descriptors.
 * It never freezes/kills another process, attaches BPF, or changes sysctls.
 * Run separately as shell/root; neither context represents app seccomp policy.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/android/binder.h>
#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/futex.h>
#include <linux/memfd.h>
#include <linux/openat2.h>
#include <linux/userfaultfd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifndef MREMAP_DONTUNMAP
#define MREMAP_DONTUNMAP 4
#endif
#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 8
#define MFD_EXEC 16
#endif
#ifndef UFFDIO_ZEROPAGE_MODE_MMAP_TRYLOCK
#define UFFDIO_ZEROPAGE_MODE_MMAP_TRYLOCK (UINT64_C(1) << 63)
#endif
#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#define PR_SET_VMA_ANON_NAME 0
#endif

static size_t page_size;

static void result(const char *name, long ret)
{
	int saved = ret < 0 ? errno : 0;
	printf("%s ret=%ld errno=%d (%s)\n", name, ret, saved,
	       saved ? strerror(saved) : "ok");
}

static void fd_result(const char *name, int fd)
{
	result(name, fd);
	if (fd >= 0)
		close(fd);
}

static void probe_dmabuf_heap(void)
{
	void *lib = dlopen("libdmabufheap.so", RTLD_NOW | RTLD_LOCAL);

	if (!lib) {
		printf("dmabuf.library_unavailable=%s\n", dlerror());
		return;
	}
	void *(*create)(void) = dlsym(lib, "CreateDmabufHeapBufferAllocator");
	void (*destroy)(void *) = dlsym(lib, "FreeDmabufHeapBufferAllocator");
	int (*alloc)(void *, bool, size_t, unsigned int) =
		dlsym(lib, "DmabufHeapAllocSystem2");
	bool (*ion_supported)(void) = dlsym(lib, "CheckIonSupport");

	if (ion_supported)
		printf("dmabuf.library_ion_support=%d\n", ion_supported());
	if (create && destroy && alloc) {
		void *allocator = create();

		if (allocator) {
			errno = 0;
			fd_result("dmabuf.library_alloc_system", alloc(allocator, true, page_size, 0));
			destroy(allocator);
		}
	} else {
		puts("dmabuf.library_wrapper_unavailable");
	}
	dlclose(lib);
}

static void probe_uffd(void)
{
	struct uffdio_api api = { .api = UFFD_API };
	int fd = syscall(__NR_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);

	result("uffd.create", fd);
	if (fd < 0)
		return;
	int ret = ioctl(fd, UFFDIO_API, &api);
	result("uffd.api", ret);
	printf("uffd.features=0x%" PRIx64 " ioctls=0x%" PRIx64 "\n",
	       (uint64_t)api.features, (uint64_t)api.ioctls);
	void *p = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ret == 0 && p != MAP_FAILED) {
		/* Same MMAP_TRYLOCK detection used by Android 17 ART. */
		struct uffdio_zeropage zero = {
			.range = { (uintptr_t)p, page_size },
			.mode = UFFDIO_ZEROPAGE_MODE_MMAP_TRYLOCK,
		};
		result("uffd.trylock_unregistered", ioctl(fd, UFFDIO_ZEROPAGE, &zero));
		printf("uffd.trylock_result=%" PRId64 "\n", (int64_t)zero.zeropage);
		struct uffdio_register reg = {
			.range = { (uintptr_t)p, page_size },
			.mode = UFFDIO_REGISTER_MODE_MISSING,
		};
		ret = ioctl(fd, UFFDIO_REGISTER, &reg);
		result("uffd.register_missing", ret);
		if (!ret) {
			void *src = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
					 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (src != MAP_FAILED) {
				memset(src, 0x5a, page_size);
				struct uffdio_copy copy = { .dst = (uintptr_t)p,
					.src = (uintptr_t)src, .len = page_size };
				ret = ioctl(fd, UFFDIO_COPY, &copy);
				result("uffd.copy", ret);
				if (!ret)
					printf("uffd.copy_data=%s\n", memcmp(p, src, page_size) ? "BAD" : "ok");
				munmap(src, page_size);
			}
			ioctl(fd, UFFDIO_UNREGISTER, &reg.range);
		}
		munmap(p, page_size);
	}
	close(fd);

	fd = syscall(__NR_memfd_create, "venus-minor-probe", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, page_size)) {
		if (fd >= 0) close(fd);
		return;
	}
	void *a = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	void *b = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (a != MAP_FAILED && b != MAP_FAILED) {
		memset(a, 0x63, page_size);
		int u = syscall(__NR_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
		api = (struct uffdio_api){ .api = UFFD_API, .features = UFFD_FEATURE_MINOR_SHMEM };
		ret = u < 0 ? -1 : ioctl(u, UFFDIO_API, &api);
		result("uffd.minor_api", ret);
		if (!ret) {
			struct uffdio_register reg = { .range = { (uintptr_t)b, page_size },
				.mode = UFFDIO_REGISTER_MODE_MINOR };
			ret = ioctl(u, UFFDIO_REGISTER, &reg);
			result("uffd.register_minor_shmem", ret);
			if (!ret) {
				struct uffdio_continue cont = { .range = reg.range };
				ret = ioctl(u, UFFDIO_CONTINUE, &cont);
				result("uffd.continue", ret);
				if (!ret) printf("uffd.minor_data=%s\n", memcmp(a, b, page_size) ? "BAD" : "ok");
				ioctl(u, UFFDIO_UNREGISTER, &reg.range);
			}
		}
		if (u >= 0) close(u);
	}
	if (a != MAP_FAILED) munmap(a, page_size);
	if (b != MAP_FAILED) munmap(b, page_size);
	close(fd);
}

static void probe_bpf(void)
{
	struct {
		struct btf_header header;
		struct { struct btf_type type; uint32_t encoding; } key, integer;
		struct { struct btf_type type; struct btf_member member; } value;
		char strings[21];
	} __attribute__((packed)) btf = {
		.header = { .magic = BTF_MAGIC, .version = BTF_VERSION,
			.hdr_len = sizeof(struct btf_header), .type_len = 56,
			.str_off = 56, .str_len = 21 },
		.key = { .type = { .name_off = 1, .info = BTF_KIND_INT << 24, .size = 4 },
			.encoding = 32 },
		.integer = { .type = { .name_off = 16, .info = BTF_KIND_INT << 24, .size = 8 },
			.encoding = 64 },
		.value = { .type = { .name_off = 5, .info = (BTF_KIND_STRUCT << 24) | 1, .size = 8 },
			.member = { .name_off = 11, .type = 2 } },
		.strings = "\0int\0value\0slot\0long",
	};
	union bpf_attr btf_attr = { .btf = (uintptr_t)&btf, .btf_size = sizeof(btf) };
	int btf_fd = syscall(__NR_bpf, BPF_BTF_LOAD, &btf_attr, sizeof(btf_attr));
	result("bpf.btf_load", btf_fd);
	const int types[] = { BPF_MAP_TYPE_ARRAY, BPF_MAP_TYPE_HASH,
		BPF_MAP_TYPE_PERCPU_ARRAY, BPF_MAP_TYPE_RINGBUF, BPF_MAP_TYPE_SK_STORAGE };
	const char *names[] = { "bpf.array", "bpf.hash", "bpf.percpu_array",
		"bpf.ringbuf", "bpf.sk_storage" };
	for (size_t i = 0; i < sizeof(types)/sizeof(types[0]); i++) {
		union bpf_attr attr = { .map_type = types[i], .key_size = 4,
			.value_size = 8, .max_entries = 4 };
		if (types[i] == BPF_MAP_TYPE_RINGBUF) {
			attr.key_size = attr.value_size = 0;
			attr.max_entries = page_size;
		} else if (types[i] == BPF_MAP_TYPE_SK_STORAGE) {
			if (btf_fd < 0) continue;
			attr.map_flags = BPF_F_NO_PREALLOC;
			attr.max_entries = 0;
			attr.btf_fd = btf_fd;
			attr.btf_key_type_id = 1;
			attr.btf_value_type_id = 3;
		}
		fd_result(names[i], syscall(__NR_bpf, BPF_MAP_CREATE, &attr, sizeof(attr)));
	}
	if (btf_fd >= 0) close(btf_fd);
	struct bpf_insn insns[] = {
		{ .code = BPF_ALU64 | BPF_MOV | BPF_K, .dst_reg = BPF_REG_0, .imm = 1 },
		{ .code = BPF_JMP | BPF_EXIT },
	};
	char license[] = "GPL";
	const int prog_types[] = { BPF_PROG_TYPE_SOCKET_FILTER,
		BPF_PROG_TYPE_SCHED_CLS, BPF_PROG_TYPE_CGROUP_SKB };
	const char *prog_names[] = { "bpf.socket_filter", "bpf.sched_cls", "bpf.cgroup_skb" };
	for (size_t i = 0; i < sizeof(prog_types)/sizeof(prog_types[0]); i++) {
		union bpf_attr attr = { .prog_type = prog_types[i], .insn_cnt = 2,
			.insns = (uintptr_t)insns, .license = (uintptr_t)license };
		fd_result(prog_names[i], syscall(__NR_bpf, BPF_PROG_LOAD, &attr, sizeof(attr)));
	}
}

static void probe_binder(void)
{
	int fd = open("/dev/binder", O_RDWR | O_CLOEXEC);
	result("binder.open", fd);
	if (fd < 0) return;
	struct binder_version version = {0};
	result("binder.version", ioctl(fd, BINDER_VERSION, &version));
	printf("binder.protocol=%d\n", version.protocol_version);
	struct binder_frozen_status_info frozen = { .pid = getpid() };
	result("binder.get_frozen_info_self", ioctl(fd, BINDER_GET_FROZEN_INFO, &frozen));
	struct binder_extended_error ee = {0};
	result("binder.extended_error", ioctl(fd, BINDER_GET_EXTENDED_ERROR, &ee));
	uint32_t enable = 1;
	result("binder.oneway_spam_per_fd", ioctl(fd, BINDER_ENABLE_ONEWAY_SPAM_DETECTION, &enable));
	close(fd);
}

int main(void)
{
	struct utsname uts;
	uname(&uts);
	page_size = sysconf(_SC_PAGESIZE);
	printf("abi_bits=%zu uid=%u kernel=%s page_size=%zu\n", sizeof(void *)*8, getuid(), uts.release, page_size);
	fd_result("memfd.exec", syscall(__NR_memfd_create, "venus-exec-probe", MFD_CLOEXEC | MFD_EXEC));
	fd_result("memfd.noexec_seal", syscall(__NR_memfd_create, "venus-noexec-probe", MFD_CLOEXEC | MFD_NOEXEC_SEAL));
	int m = syscall(__NR_memfd_create, "venus-seal-probe", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	result("memfd.create", m);
	if (m >= 0) {
		result("memfd.future_write_seal", fcntl(m, F_ADD_SEALS, F_SEAL_FUTURE_WRITE));
		close(m);
	}
	void *p = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (p != MAP_FAILED) {
		void *q = mremap(p, page_size, page_size, MREMAP_MAYMOVE | MREMAP_DONTUNMAP, NULL);
		result("mremap.shared_dontunmap", q == MAP_FAILED ? -1 : 0);
		munmap(p, page_size);
		if (q != MAP_FAILED) munmap(q, page_size);
	}
	p = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p != MAP_FAILED) {
		result("prctl.anon_name", prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, p, page_size, "venus-a17-probe"));
		result("madvise.populate_read", madvise(p, page_size, 22));
		result("madvise.populate_write", madvise(p, page_size, 23));
		int pidfd = syscall(__NR_pidfd_open, getpid(), 0);
		result("pidfd.open_self", pidfd);
		if (pidfd >= 0) {
			result("pidfd.signal_zero_self", syscall(__NR_pidfd_send_signal, pidfd, 0, NULL, 0));
			struct iovec iov = { .iov_base = p, .iov_len = page_size };
			result("process_madvise.self_cold", syscall(__NR_process_madvise, pidfd, &iov, 1, MADV_COLD, 0));
			close(pidfd);
		}
		/* Feature query on an invalid address; cannot seal any existing mapping. */
		result("mseal.invalid_range", syscall(__NR_mseal, 1, page_size, 0));
		munmap(p, page_size);
	}
	result("process_mrelease.invalid_fd", syscall(__NR_process_mrelease, -1, 0));
	result("membarrier.query", syscall(__NR_membarrier, 0, 0, 0));
	result("rseq.invalid_args", syscall(__NR_rseq, NULL, 0, 0, 0));
	struct open_how how = { .flags = O_RDONLY | O_CLOEXEC, .resolve = RESOLVE_NO_SYMLINKS };
	fd_result("openat2.dev_null", syscall(__NR_openat2, AT_FDCWD, "/dev/null", &how, sizeof(how)));
	result("close_range.empty", syscall(__NR_close_range, 100000, 100001, 0));
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep >= 0) {
		struct epoll_event ev;
		int64_t timeout[2] = {0, 0};
		result("epoll_pwait2.zero_timeout", syscall(__NR_epoll_pwait2, ep, &ev, 1, timeout, NULL, 8));
		close(ep);
	}
	uint32_t value = 0;
	struct futex_waitv wait = { .val = 1, .uaddr = (uintptr_t)&value, .flags = FUTEX_32 };
	result("futex_waitv.value_mismatch", syscall(__NR_futex_waitv, &wait, 1, 0, NULL, 1));
	probe_uffd();
	probe_binder();
	probe_bpf();
	probe_dmabuf_heap();
	return 0;
}
