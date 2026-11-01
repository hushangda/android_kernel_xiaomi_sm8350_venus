/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/mi_memcg_core.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define U64MAX (~(u64)0)
static unsigned int checks;
#define CHECK(x) do { assert(x); checks++; } while (0)

static u64 seed = 0x4853378350;
static u64 random64(void)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return seed;
}

static void test_policy(void)
{
	struct vmc_policy p = {300, 70, 50, 0};
	struct vmc_snapshot s = {800, 150, 50, 1, 0};
	struct vmc_budget b;
	unsigned int i;

	CHECK(vmc_policy_budget(&p, &s, &b) == 0 && b.pages == 500);
	p.priority = 2;
	CHECK(vmc_policy_budget(&p, &s, &b) == 0 && b.pages == 125);
	p.score = 100;
	CHECK(vmc_policy_budget(&p, &s, &b) == 0 && b.gate == VMC_SCORE_PROTECTED);
	p.score = 101;
	s.faults = 500;
	CHECK(vmc_policy_budget(&p, &s, &b) == 0 && b.gate == VMC_REFAULT);
	s.previous_faults = 501;
	CHECK(vmc_policy_budget(&p, &s, &b) == -EAGAIN);
	s.previous_faults = s.faults = 0;
	p.refault_ratio = 0;
	CHECK(vmc_policy_budget(&p, &s, &b) == 0 && b.gate == VMC_REFAULT);
	p.refault_ratio = 50;
	p.comp_ratio = 101;
	CHECK(vmc_policy_budget(&p, &s, &b) == -EINVAL);
	p.comp_ratio = 70;
	p.priority = 64;
	CHECK(vmc_policy_budget(&p, &s, &b) == -EINVAL);
	p.priority = 0;
	s.anon_pages = U64MAX;
	CHECK(vmc_policy_budget(&p, &s, &b) == -ERANGE);
	s.compressed_pages = s.writeback_pages = 0;
	p.comp_ratio = 100;
	CHECK(vmc_policy_budget(&p, &s, &b) == 0 && b.pages == U64MAX);

	/* Wide arithmetic oracle checks the donor equations without u64 wrap. */
	for (i = 0; i < 100000; i++) {
		u64 total, used, expected, limit;
		enum vmc_gate gate;

		s.anon_pages = random64() / 4;
		s.compressed_pages = random64() / 4;
		s.writeback_pages = random64() / 4;
		s.previous_faults = 0;
		s.faults = random64() / 4;
		p.score = random64() % 1001;
		p.comp_ratio = random64() % 101;
		p.refault_ratio = random64() % 101;
		p.priority = random64() % 64;
		used = s.compressed_pages + s.writeback_pages;
		total = s.anon_pages + used;
		limit = ((__uint128_t)total * p.refault_ratio) / 100;
		expected = ((__uint128_t)total * p.comp_ratio) / 100;
		expected = expected > used ? (expected - used) >> p.priority : 0;
		gate = expected ? VMC_ELIGIBLE : VMC_AT_RATIO;
		if (p.score <= 100) {
			expected = 0;
			gate = VMC_SCORE_PROTECTED;
		} else if (s.faults >= limit) {
			expected = 0;
			gate = VMC_REFAULT;
		}
		CHECK(vmc_policy_budget(&p, &s, &b) == 0);
		CHECK(b.pages == expected && b.gate == gate);
	}
}

static int parse(const char *s, struct vmc_request *r)
{
	return vmc_parse_reclaim(s, strlen(s), 12, r);
}

static void test_parser(void)
{
	static const char *bad[] = {"", "1", "1 2", "1 2 3", "4 1 1",
		"-1 1 1", "1 -1 0", "1 +1 0", "1 1 1 2", "1 1 1x",
		"1 0x10 0", "1 18446744073709551616 0"};
	struct vmc_request r;
	u64 lru[4] = {100, 200, 300, 400};
	unsigned int i;
	long page = sysconf(_SC_PAGESIZE);
	char *guard = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
			   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

	CHECK(guard != MAP_FAILED);
	CHECK(mprotect(guard + page, page, PROT_NONE) == 0);
	memcpy(guard + page - 5, "1 4 4", 5);
	CHECK(vmc_parse_reclaim(guard + page - 5, 5, 12, &r) == 0);
	CHECK(munmap(guard, 2 * page) == 0);
	CHECK(parse("1 4096 4096", &r) == 0);
	CHECK(r.type == VMC_ACTIVE_ANON && r.target_pages == 1048576 &&
	      r.window_pages == 1048576);
	CHECK(vmc_cap_request(&r, lru) == 0 && r.target_pages == 300 &&
	      r.window_pages == 300);
	CHECK(parse(" 3\t0 0\n", &r) == 0 && r.window_pages == 1024);
	CHECK(vmc_cap_request(&r, lru) == 0 && r.target_pages == 700);
	CHECK(parse("0 1 0", &r) == 0 && r.window_pages == 1024);
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		CHECK(parse(bad[i], &r) < 0);
	CHECK(parse("1 72057594037927936 0", &r) == -ERANGE);
	CHECK(vmc_parse_reclaim("1 1 1\0junk", 10, 12, &r) == -EINVAL);
	CHECK(vmc_parse_reclaim("1 1 1", 5, 21, &r) == -EINVAL);
	CHECK(parse("1 0 0", &r) == 0);
	lru[0] = U64MAX;
	CHECK(vmc_cap_request(&r, lru) == -ERANGE);
	/* Fuzz bounded non-NUL byte arrays, including non-ASCII and huge digits. */
	for (i = 0; i < 100000; i++) {
		char bytes[129];
		size_t j, len = random64() % sizeof(bytes);
		for (j = 0; j < len; j++)
			bytes[j] = random64();
		(void)vmc_parse_reclaim(bytes, len, 12, &r);
	}
}

struct fake_backend {
	u64 answer, requests[16];
	unsigned int calls, cancel_after;
	int error;
	enum vmc_lru_type expected_type;
};

static int reclaim(void *ctx, enum vmc_lru_type type, u64 pages, u64 *done)
{
	struct fake_backend *f = ctx;
	CHECK(f->calls < 16 && type == f->expected_type && pages != 0);
	f->requests[f->calls++] = pages;
	*done = f->answer == U64MAX ? pages : f->answer;
	return f->error;
}

static bool cancelled(void *ctx)
{
	struct fake_backend *f = ctx;
	return f->calls >= f->cancel_after;
}

static void test_loop(void)
{
	struct vmc_request r = {VMC_ACTIVE_ANON, 2500, 1024};
	struct fake_backend f = {.answer = U64MAX, .cancel_after = 16,
		.expected_type = VMC_ACTIVE_ANON};
	struct vmc_reclaimer ops = {reclaim, cancelled, &f};
	struct vmc_result out;

	CHECK(vmc_run_reclaim(&r, &ops, 16, &out) == 0);
	CHECK(out.reclaimed_pages == 2500 && out.calls == 3 && out.reason == VMC_TARGET);
	CHECK(f.requests[0] == 1024 && f.requests[1] == 1024 && f.requests[2] == 452);
	f.calls = 0; f.answer = 0;
	CHECK(vmc_run_reclaim(&r, &ops, 16, &out) == 0 && out.reason == VMC_NO_PROGRESS);
	f.calls = 0; f.answer = 1;
	CHECK(vmc_run_reclaim(&r, &ops, 2, &out) == 0 && out.reason == VMC_CALL_LIMIT && out.calls == 2);
	f.calls = 0; f.cancel_after = 0;
	CHECK(vmc_run_reclaim(&r, &ops, 16, &out) == 0 && out.reason == VMC_CANCELLED && out.calls == 0);
	f.calls = 0; f.cancel_after = 1;
	CHECK(vmc_run_reclaim(&r, &ops, 16, &out) == 0 && out.reason == VMC_CANCELLED && out.reclaimed_pages == 1);
	f.calls = 0; f.cancel_after = 16; f.error = -EIO;
	CHECK(vmc_run_reclaim(&r, &ops, 16, &out) == -EIO && out.reason == VMC_BACKEND_ERROR);
	f.calls = 0; f.error = 0; f.answer = 4096;
	CHECK(vmc_run_reclaim(&r, &ops, 16, &out) == 0 && out.reclaimed_pages == 4096 && out.calls == 1);
	f.calls = 0; r.target_pages = U64MAX; r.window_pages = U64MAX / 2; f.answer = U64MAX - 1;
	CHECK(vmc_run_reclaim(&r, &ops, 16, &out) == -EOVERFLOW);
	f.calls = 0; r.target_pages = 0; r.window_pages = 0;
	CHECK(vmc_run_reclaim(&r, &ops, 16, &out) == 0 && out.calls == 0);
	r.target_pages = 1;
	CHECK(vmc_run_reclaim(&r, &ops, 16, &out) == -EINVAL);
}

int main(void)
{
	test_policy(); test_parser(); test_loop();
	printf("PASS: %u checks, 100000 wide-math policy cases, 100000 bounded parser fuzz cases\n", checks);
	puts("Backend is a test double: this does not claim real memcg reclaim or device validation.");
	return 0;
}
