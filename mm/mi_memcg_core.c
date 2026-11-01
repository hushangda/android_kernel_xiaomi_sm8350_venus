// SPDX-License-Identifier: GPL-2.0-only
/*
 * Overflow-checked Xiaomi memcg policy and reclaim request core, 2026.
 * Policy equations and reclaim request semantics adapted from xswapd.c,
 * Copyright(c) 2024 X-Ring technologies Inc. (GPL-2.0).
 * Pinned source and differences: Documentation/admin-guide/mm/xiaomi-memcg.rst.
 * Registration and VM integration are kept in mi_memcg_policy.c.
 */
#include <linux/mi_memcg_core.h>
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

#define VMC_U64_MAX (~(u64)0)

static bool vmc_add(u64 a, u64 b, u64 *sum)
{
	if (a > VMC_U64_MAX - b)
		return false;
	*sum = a + b;
	return true;
}

/* Exact floor(total * ratio / 100), without overflowing the multiplication. */
static u64 vmc_percent(u64 total, unsigned int ratio)
{
	return (total / 100) * ratio + ((total % 100) * ratio) / 100;
}

int vmc_policy_budget(const struct vmc_policy *p,
		      const struct vmc_snapshot *s, struct vmc_budget *out)
{
	u64 total, swapped, allowance, faults;
	struct vmc_budget result = { .pages = 0, .gate = VMC_SCORE_PROTECTED };

	if (!p || !s || !out || p->score > 1000 || p->comp_ratio > 100 ||
	    p->refault_ratio > 100 || p->priority >= 64)
		return -EINVAL;
	if (!vmc_add(s->compressed_pages, s->writeback_pages, &swapped) ||
	    !vmc_add(s->anon_pages, swapped, &total))
		return -ERANGE;
	if (p->score <= 100)
		goto done;
	/* A reset or inconsistent counter snapshot is not a wrapped huge delta. */
	if (s->faults < s->previous_faults)
		return -EAGAIN;
	faults = s->faults - s->previous_faults;
	if (faults >= vmc_percent(total, p->refault_ratio)) {
		result.gate = VMC_REFAULT;
		goto done;
	}
	allowance = vmc_percent(total, p->comp_ratio);
	if (allowance > swapped)
		result.pages = (allowance - swapped) >> p->priority;
	result.gate = result.pages ? VMC_ELIGIBLE : VMC_AT_RATIO;
done:
	*out = result;
	return 0;
}

static bool vmc_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
	       c == '\v' || c == '\f';
}

int vmc_parse_values(const char *buf, size_t len, u64 *values,
                     unsigned int capacity, unsigned int *count)
{
	size_t pos = 0;
	unsigned int field = 0;

	if (!buf || !len || len > 256 || !values || !capacity || !count)
		return -EINVAL;
	while (pos < len) {
		u64 value = 0;
		bool any = false;

		while (pos < len && vmc_space(buf[pos]))
			pos++;
		if (pos == len)
			break;
		if (field == capacity)
			return -EINVAL;
		while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') {
			unsigned int digit = buf[pos++] - '0';

			if (value > (VMC_U64_MAX - digit) / 10)
				return -ERANGE;
			value = value * 10 + digit;
			any = true;
		}
		if (!any || (pos < len && !vmc_space(buf[pos])))
			return -EINVAL;
		values[field++] = value;
	}
	if (!field)
		return -EINVAL;
	*count = field;
	return 0;
}

int vmc_parse_reclaim(const char *buf, size_t len, unsigned int page_shift,
		      struct vmc_request *out)
{
	u64 fields[3], window;
	unsigned int shift, count;
	struct vmc_request result;
	int ret;

	if (!out || page_shift > 20 || len > 128)
		return -EINVAL;
	ret = vmc_parse_values(buf, len, fields, 3, &count);
	if (ret)
		return ret;
	if (count != 3 || fields[0] > VMC_ACTIVE_FILE || fields[1] < fields[2])
		return -EINVAL;
	window = fields[2] ? fields[2] : 4;
	shift = 20 - page_shift;
	if (fields[1] > (VMC_U64_MAX >> shift) ||
	    window > (VMC_U64_MAX >> shift))
		return -ERANGE;
	result.type = fields[0];
	result.target_pages = fields[1] << shift;
	result.window_pages = window << shift;
	*out = result;
	return 0;
}

int vmc_cap_request(struct vmc_request *r, const u64 lru_pages[4])
{
	u64 available;

	if (!r || !lru_pages || (unsigned int)r->type > VMC_ACTIVE_FILE)
		return -EINVAL;
	available = lru_pages[r->type];
	if ((r->type == VMC_ACTIVE_ANON || r->type == VMC_ACTIVE_FILE) &&
	    !vmc_add(available, lru_pages[r->type - 1], &available))
		return -ERANGE;
	if (!r->target_pages || r->target_pages > available)
		r->target_pages = available;
	if (r->window_pages > available)
		r->window_pages = available;
	return 0;
}

int vmc_run_reclaim(const struct vmc_request *r,
		    const struct vmc_reclaimer *backend, unsigned int max_calls,
		    struct vmc_result *out)
{
	struct vmc_result result = { .reason = VMC_TARGET };
	int ret = 0;

	if (!r || !backend || !backend->reclaim || !out || !max_calls ||
	    (unsigned int)r->type > VMC_ACTIVE_FILE ||
	    (r->target_pages && !r->window_pages))
		return -EINVAL;
	while (result.reclaimed_pages < r->target_pages) {
		u64 remaining = r->target_pages - result.reclaimed_pages;
		u64 window = r->window_pages < remaining ? r->window_pages : remaining;
		u64 done = 0;

		if (backend->cancelled && backend->cancelled(backend->ctx)) {
			result.reason = VMC_CANCELLED;
			break;
		}
		if (result.calls == max_calls) {
			result.reason = VMC_CALL_LIMIT;
			break;
		}
		result.calls++;
		ret = backend->reclaim(backend->ctx, r->type, window, &done);
		if (ret) {
			result.reason = VMC_BACKEND_ERROR;
			break;
		}
		if (!vmc_add(result.reclaimed_pages, done, &result.reclaimed_pages)) {
			ret = -EOVERFLOW;
			result.reason = VMC_BACKEND_ERROR;
			break;
		}
		if (!done) {
			result.reason = VMC_NO_PROGRESS;
			break;
		}
	}
	*out = result;
	return ret;
}
