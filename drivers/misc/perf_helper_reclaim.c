// SPDX-License-Identifier: GPL-2.0-only
/*
 * MIMD2 global reclaim protocol. Stock pandora perf_helper.ko decodes
 * value / 10000000 as SCAN_ANON=2 or SCAN_FILE=3 and the remainder as pages.
 * This port keeps scan selection local to the reclaim request and counts only
 * real reclaimed pages. A bounded queue preserves distinct accepted requests.
 */
#include <linux/jiffies.h>
#include <linux/memcontrol.h>
#include <linux/workqueue.h>

#define MIMD_RECLAIM_QUEUE 16U
#define MIMD_RECLAIM_BASE 10000000UL
#define MIMD_RECLAIM_CHUNK (4UL << (20 - PAGE_SHIFT))
struct mimd_reclaim_request {
	unsigned long pages;
	unsigned int type;
};
static DEFINE_SPINLOCK(mimd_reclaim_lock);
static struct mimd_reclaim_request mimd_reclaim_queue[MIMD_RECLAIM_QUEUE];
static unsigned int mimd_reclaim_head, mimd_reclaim_count;
static bool mimd_reclaim_stopping;
static struct workqueue_struct *mimd_reclaim_wq;

static int mimd_reclaim_decode(unsigned long value, struct mimd_reclaim_request *request)
{
	unsigned long type = value / MIMD_RECLAIM_BASE;

	if (type != 0 && type != 2 && type != 3)
		return -EINVAL;
	request->type = type;
	request->pages = value % MIMD_RECLAIM_BASE;
	return 0;
}

static unsigned long mimd_reclaim_run(const struct mimd_reclaim_request *request)
{
	unsigned long reclaimed = 0, remaining, nr;
	unsigned long deadline = jiffies + msecs_to_jiffies(5000);
	unsigned int calls = 0, idle = 0;
	bool anon = request->type != 3;

	while (reclaimed < request->pages && calls++ < 4096 &&
	       !READ_ONCE(mimd_reclaim_stopping) && time_before(jiffies, deadline)) {
		remaining = min(request->pages - reclaimed, MIMD_RECLAIM_CHUNK);
		nr = xiaomi_reclaim_memcg(root_mem_cgroup, remaining,
			anon ? XIAOMI_RECLAIM_ANON : XIAOMI_RECLAIM_FILE);
		reclaimed += nr;
		idle = nr ? 0 : idle + 1;
		/* Unspecified class tries both real LRUs before declaring no progress. */
		if (idle >= (request->type ? 1 : 2))
			break;
		if (!request->type)
			anon = !anon;
		cond_resched();
	}
	return reclaimed;
}

static void mimd_reclaim_worker(struct work_struct *work)
{
	struct mimd_reclaim_request request;

	for (;;) {
		spin_lock(&mimd_reclaim_lock);
		if (!mimd_reclaim_count || mimd_reclaim_stopping) {
			spin_unlock(&mimd_reclaim_lock);
			break;
		}
		request = mimd_reclaim_queue[mimd_reclaim_head];
		mimd_reclaim_head = (mimd_reclaim_head + 1) % MIMD_RECLAIM_QUEUE;
		mimd_reclaim_count--;
		spin_unlock(&mimd_reclaim_lock);
		global_reclaim_record(mimd_reclaim_run(&request));
		cond_resched();
	}
}
static DECLARE_WORK(mimd_reclaim_work, mimd_reclaim_worker);

static int mimd_reclaim_submit(unsigned long value)
{
	struct mimd_reclaim_request request;
	int ret = mimd_reclaim_decode(value, &request);

	if (ret || !request.pages)
		return ret;
	spin_lock(&mimd_reclaim_lock);
	if (mimd_reclaim_stopping || !mimd_reclaim_wq) {
		ret = -ESHUTDOWN;
	} else if (mimd_reclaim_count == MIMD_RECLAIM_QUEUE) {
		ret = -ENOSPC;
	} else {
		mimd_reclaim_queue[(mimd_reclaim_head + mimd_reclaim_count) % MIMD_RECLAIM_QUEUE] = request;
		mimd_reclaim_count++;
		queue_work(mimd_reclaim_wq, &mimd_reclaim_work);
	}
	spin_unlock(&mimd_reclaim_lock);
	return ret;
}

static int __init mimd_reclaim_init(void)
{
	mimd_reclaim_wq = alloc_workqueue("mimd_reclaim",
				WQ_UNBOUND | WQ_FREEZABLE | WQ_MEM_RECLAIM, 1);
	return mimd_reclaim_wq ? 0 : -ENOMEM;
}

static void mimd_reclaim_exit(void)
{
	WRITE_ONCE(mimd_reclaim_stopping, true);
	destroy_workqueue(mimd_reclaim_wq);
}
