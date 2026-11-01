/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_MI_MEMCG_CORE_H
#define _LINUX_MI_MEMCG_CORE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint64_t u64;
#endif

/* Internal porting interface only. No cgroup or sysfs ABI is registered. */
struct vmc_policy {
	unsigned int score, comp_ratio, refault_ratio, priority;
};

struct vmc_snapshot {
	u64 anon_pages, compressed_pages, writeback_pages;
	u64 faults, previous_faults;
};

enum vmc_gate {
	VMC_ELIGIBLE, VMC_SCORE_PROTECTED, VMC_REFAULT, VMC_AT_RATIO,
};

struct vmc_budget {
	u64 pages;
	enum vmc_gate gate;
};

enum vmc_lru_type {
	VMC_INACTIVE_ANON, VMC_ACTIVE_ANON,
	VMC_INACTIVE_FILE, VMC_ACTIVE_FILE,
};

struct vmc_request {
	enum vmc_lru_type type;
	u64 target_pages, window_pages;
};

enum vmc_stop_reason {
	VMC_TARGET, VMC_NO_PROGRESS, VMC_CANCELLED, VMC_CALL_LIMIT,
	VMC_BACKEND_ERROR,
};

struct vmc_result {
	u64 reclaimed_pages;
	unsigned int calls;
	enum vmc_stop_reason reason;
};

/*
 * A backend must hold a live css reference, implement the requested anon/file
 * scan class and proactive semantics, and honor cancellation between calls.
 * The active/inactive distinction selects the availability budget; it does
 * not promise the VM never ages an active page. Its result
 * may exceed the requested pages (the VM reclaims in batches). Passing the
 * type as a bool to the 5.4 reclaim entry point does NOT satisfy this contract.
 */
struct vmc_reclaimer {
	int (*reclaim)(void *ctx, enum vmc_lru_type type, u64 pages, u64 *done);
	bool (*cancelled)(void *ctx);
	void *ctx;
};

int vmc_policy_budget(const struct vmc_policy *policy,
		      const struct vmc_snapshot *snapshot, struct vmc_budget *out);
int vmc_parse_values(const char *buf, size_t len, u64 *values,
		     unsigned int capacity, unsigned int *count);
int vmc_parse_reclaim(const char *buf, size_t len, unsigned int page_shift,
		      struct vmc_request *out);
int vmc_cap_request(struct vmc_request *request, const u64 lru_pages[4]);
int vmc_run_reclaim(const struct vmc_request *request,
		    const struct vmc_reclaimer *backend, unsigned int max_calls,
		    struct vmc_result *out);
#endif
