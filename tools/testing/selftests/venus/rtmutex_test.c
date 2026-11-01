// SPDX-License-Identifier: GPL-2.0-only
/*
 * Deterministic harness for functions extracted from kernel/locking/rtmutex.c.
 * Models queue and locking contracts; does not emulate the scheduler, rbtree,
 * memory ordering, or real concurrent PI futex operation.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
	fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #x); \
	exit(1); \
} } while (0)
#define unlikely(x) (x)

struct rt_mutex;
struct rt_mutex_waiter;
struct test_lock { bool held; unsigned int acquisitions; };
struct task_struct {
	struct test_lock pi_lock;
	struct rt_mutex_waiter *pi_blocked_on, *pi_top;
	int prio, references;
	struct { unsigned long deadline; } dl;
};
struct rt_mutex_waiter {
	struct task_struct *task;
	struct rt_mutex *lock;
	int prio;
	unsigned long deadline;
	bool queued;
};
struct rt_mutex {
	struct test_lock wait_lock;
	struct task_struct *owner;
	struct rt_mutex_waiter *queue[4];
	unsigned int count;
};
enum rtmutex_chainwalk { RT_MUTEX_MIN_CHAINWALK, RT_MUTEX_FULL_CHAINWALK };

static struct task_struct *current;
static unsigned int dequeues, pi_enqueues, pi_dequeues, adjustments, chain_calls;
static unsigned int remove_calls;
static bool release_owner_in_chain;
static int chain_result;
static struct task_struct *expected_chain_task;

static void raw_spin_lock(struct test_lock *lock)
{
	CHECK(!lock->held);
	lock->held = true;
	lock->acquisitions++;
}

static void raw_spin_unlock(struct test_lock *lock)
{
	CHECK(lock->held);
	lock->held = false;
}

#define raw_spin_lock_irq raw_spin_lock
#define raw_spin_unlock_irq raw_spin_unlock
#define lockdep_assert_held(lock) CHECK((lock)->held)

static struct task_struct *rt_mutex_owner(struct rt_mutex *lock)
{
	CHECK(lock->wait_lock.held);
	return lock->owner;
}

static struct rt_mutex_waiter *rt_mutex_top_waiter(struct rt_mutex *lock)
{
	CHECK(lock->wait_lock.held);
	return lock->count ? lock->queue[0] : NULL;
}

static bool rt_mutex_has_waiters(struct rt_mutex *lock)
{
	CHECK(lock->wait_lock.held);
	return lock->count != 0;
}

static void rt_mutex_enqueue(struct rt_mutex *lock, struct rt_mutex_waiter *waiter)
{
	unsigned int i;

	CHECK(lock->wait_lock.held && waiter->task->pi_lock.held);
	CHECK(!waiter->queued && lock->count < 4);
	for (i = lock->count; i && lock->queue[i - 1]->prio > waiter->prio; i--)
		lock->queue[i] = lock->queue[i - 1];
	lock->queue[i] = waiter;
	lock->count++;
	waiter->queued = true;
}

static void rt_mutex_dequeue(struct rt_mutex *lock, struct rt_mutex_waiter *waiter)
{
	unsigned int i;

	CHECK(lock->wait_lock.held);
	CHECK(waiter->task && waiter->task->pi_lock.held && waiter->queued);
	for (i = 0; i < lock->count && lock->queue[i] != waiter; i++)
		;
	CHECK(i < lock->count);
	for (; i + 1 < lock->count; i++)
		lock->queue[i] = lock->queue[i + 1];
	lock->queue[--lock->count] = NULL;
	waiter->queued = false;
	dequeues++;
}

static void rt_mutex_dequeue_pi(struct task_struct *task,
				struct rt_mutex_waiter *waiter)
{
	CHECK(task->pi_lock.held);
	/* The real helper tolerates a waiter not yet in the PI tree. */
	if (task->pi_top == waiter)
		task->pi_top = NULL;
	pi_dequeues++;
}

static void rt_mutex_enqueue_pi(struct task_struct *task,
				struct rt_mutex_waiter *waiter)
{
	CHECK(task->pi_lock.held && waiter->queued && !task->pi_top);
	task->pi_top = waiter;
	pi_enqueues++;
}

static void rt_mutex_adjust_prio(struct task_struct *task)
{
	CHECK(task->pi_lock.held);
	adjustments++;
}

static struct rt_mutex *task_blocked_on_lock(struct task_struct *task)
{
	CHECK(task->pi_lock.held);
	return task->pi_blocked_on ? task->pi_blocked_on->lock : NULL;
}

static bool rt_mutex_cond_detect_deadlock(struct rt_mutex_waiter *waiter,
					 enum rtmutex_chainwalk walk)
{
	(void)waiter;
	return walk == RT_MUTEX_FULL_CHAINWALK;
}

static void get_task_struct(struct task_struct *task)
{
	task->references++;
}

static int rt_mutex_adjust_prio_chain(struct task_struct *owner,
		enum rtmutex_chainwalk walk, struct rt_mutex *lock,
		struct rt_mutex *next_lock, struct rt_mutex_waiter *waiter,
		struct task_struct *task)
{
	CHECK(!lock->wait_lock.held && !owner->pi_lock.held && next_lock);
	CHECK(owner->references == 1 && task == expected_chain_task);
	CHECK((waiter && walk == RT_MUTEX_FULL_CHAINWALK) ||
	      (!waiter && walk == RT_MUTEX_MIN_CHAINWALK));
	owner->references--;
	chain_calls++;
	if (release_owner_in_chain)
		lock->owner = NULL;
	return chain_result;
}

static int try_to_take_rt_mutex(struct rt_mutex *lock, struct task_struct *task,
				 struct rt_mutex_waiter *waiter)
{
	CHECK(lock->wait_lock.held && !waiter);
	if (lock->owner || lock->count)
		return 0;
	lock->owner = task;
	return 1;
}

static void debug_rt_mutex_print_deadlock(struct rt_mutex_waiter *waiter)
{
	(void)waiter;
}

/* Definitions are appended from the production source by the runner. */
static void remove_waiter(struct rt_mutex *, struct rt_mutex_waiter *);
int rt_mutex_start_proxy_lock(struct rt_mutex *, struct rt_mutex_waiter *,
			      struct task_struct *);

/* Observe entry without modifying the extracted production function bodies. */
void __attribute__((no_instrument_function))
__cyg_profile_func_enter(void *function, void *caller)
{
	(void)caller;
	if (function == (void *)remove_waiter)
		remove_calls++;
}

void __attribute__((no_instrument_function))
__cyg_profile_func_exit(void *function, void *caller)
{
	(void)function;
	(void)caller;
}

struct fixture {
	struct task_struct caller, proxy, owner, other;
	struct rt_mutex lock, next_lock;
	struct rt_mutex_waiter waiter, lower, caller_blocked, owner_blocked;
};

static void init(struct fixture *f)
{
	memset(f, 0, sizeof(*f));
	current = &f->caller;
	f->caller.pi_blocked_on = &f->caller_blocked;
	f->caller_blocked.task = &f->caller;
	f->proxy.prio = 10;
	f->other.prio = 20;
	f->owner_blocked.lock = &f->next_lock;
	expected_chain_task = &f->proxy;
}

static void enqueue(struct fixture *f, bool with_lower)
{
	f->waiter.task = &f->proxy;
	f->waiter.lock = &f->lock;
	f->waiter.prio = f->proxy.prio;
	f->waiter.queued = true;
	f->proxy.pi_blocked_on = &f->waiter;
	f->lock.queue[0] = &f->waiter;
	f->lock.count = 1;
	if (with_lower) {
		f->lower.task = &f->other;
		f->lower.lock = &f->lock;
		f->lower.prio = f->other.prio;
		f->lower.queued = true;
		f->other.pi_blocked_on = &f->lower;
		f->lock.queue[1] = &f->lower;
		f->lock.count++;
	}
}

static void remove_proxy(struct fixture *f, bool top, bool with_owner, bool chain)
{
	enqueue(f, true);
	if (with_owner) {
		f->lock.owner = &f->owner;
		f->owner.pi_top = &f->waiter;
	}
	if (chain)
		f->owner.pi_blocked_on = &f->owner_blocked;
	if (!top) {
		f->lower.prio = 5;
		f->lock.queue[0] = &f->lower;
		f->lock.queue[1] = &f->waiter;
		f->owner.pi_top = &f->lower;
	}
	raw_spin_lock_irq(&f->lock.wait_lock);
	remove_waiter(&f->lock, &f->waiter);
	CHECK(f->lock.wait_lock.held);
	raw_spin_unlock_irq(&f->lock.wait_lock);
	CHECK(!f->proxy.pi_blocked_on && !f->waiter.queued && dequeues == 1);
	CHECK(f->lock.count == 1 && f->lock.queue[0] == &f->lower);
	CHECK(f->caller.pi_blocked_on == &f->caller_blocked);
	CHECK(f->caller.pi_lock.acquisitions == 0);
	CHECK(f->proxy.pi_lock.acquisitions == 1);
	CHECK(adjustments == (unsigned int)(with_owner && top));
	CHECK(chain_calls == (unsigned int)(chain && top));
	CHECK(f->owner.references == 0);
	if (with_owner)
		CHECK(f->owner.pi_top == &f->lower);
}

static void remove_unenqueued(struct fixture *f)
{
	f->lock.owner = &f->owner;
	raw_spin_lock_irq(&f->lock.wait_lock);
	remove_waiter(&f->lock, &f->waiter);
	CHECK(f->lock.wait_lock.held);
	raw_spin_unlock_irq(&f->lock.wait_lock);
	CHECK(!dequeues && !pi_dequeues && !pi_enqueues && !chain_calls);
	CHECK(!f->caller.pi_lock.acquisitions && !f->owner.pi_lock.acquisitions);
	CHECK(f->caller.pi_blocked_on == &f->caller_blocked);
}

static void start_proxy(struct fixture *f, const char *scenario)
{
	int ret;

	if (strcmp(scenario, "start-acquired"))
		f->lock.owner = &f->owner;
	if (!strcmp(scenario, "self-deadlock"))
		f->lock.owner = &f->proxy;
	if (!strcmp(scenario, "chain-rollback") ||
	    !strcmp(scenario, "owner-released")) {
		f->owner.pi_blocked_on = &f->owner_blocked;
		chain_result = -EDEADLK;
		release_owner_in_chain = !strcmp(scenario, "owner-released");
	}
	ret = rt_mutex_start_proxy_lock(&f->lock, &f->waiter, &f->proxy);
	CHECK(!f->lock.wait_lock.held && !f->proxy.pi_lock.held);
	CHECK(!f->owner.pi_lock.held && !f->caller.pi_lock.held);
	CHECK(f->caller.pi_blocked_on == &f->caller_blocked);
	CHECK(!f->caller.pi_lock.acquisitions);
	if (!strcmp(scenario, "start-acquired")) {
		CHECK(!remove_calls);
		CHECK(ret == 1 && f->lock.owner == &f->proxy);
		CHECK(!f->waiter.task && !f->lock.count && !dequeues);
		CHECK(!f->proxy.pi_blocked_on && !chain_calls);
	} else if (!strcmp(scenario, "self-deadlock")) {
		CHECK(remove_calls == 1);
		CHECK(ret == -EDEADLK && f->lock.owner == &f->proxy);
		CHECK(!f->waiter.task && !f->lock.count && !dequeues);
		CHECK(!f->proxy.pi_blocked_on && !chain_calls);
	} else if (!strcmp(scenario, "chain-rollback")) {
		CHECK(remove_calls == 1);
		CHECK(ret == -EDEADLK && !f->lock.count && dequeues == 1);
		CHECK(!f->proxy.pi_blocked_on && !f->owner.pi_top);
		CHECK(chain_calls == 2 && adjustments == 2);
		CHECK(!f->owner.references && f->lock.owner == &f->owner);
	} else {
		CHECK(!remove_calls);
		CHECK(ret == 0 && f->lock.count == 1 && !dequeues);
		CHECK(f->proxy.pi_blocked_on == &f->waiter && f->waiter.queued);
		CHECK(f->waiter.task == &f->proxy && f->waiter.lock == &f->lock);
		CHECK(f->owner.pi_top == &f->waiter);
		if (!strcmp(scenario, "owner-released"))
			CHECK(!f->lock.owner && chain_calls == 1 && !f->owner.references);
		else
			CHECK(f->lock.owner == &f->owner && !chain_calls);
	}
}

int main(int argc, char **argv)
{
	struct fixture f;

	CHECK(argc == 2);
	init(&f);
	if (!strcmp(argv[1], "remove-proxy"))
		remove_proxy(&f, true, false, false);
	else if (!strcmp(argv[1], "remove-top"))
		remove_proxy(&f, true, true, false);
	else if (!strcmp(argv[1], "remove-nontop"))
		remove_proxy(&f, false, true, true);
	else if (!strcmp(argv[1], "remove-chain"))
		remove_proxy(&f, true, true, true);
	else if (!strcmp(argv[1], "remove-unenqueued"))
		remove_unenqueued(&f);
	else if (!strcmp(argv[1], "start-acquired") ||
		 !strcmp(argv[1], "start-waiting") ||
		 !strcmp(argv[1], "self-deadlock") ||
		 !strcmp(argv[1], "chain-rollback") ||
		 !strcmp(argv[1], "owner-released"))
		start_proxy(&f, argv[1]);
	else
		CHECK(!"unknown scenario");
	printf("PASS rtmutex %s\n", argv[1]);
	return 0;
}
