// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi Venus low-intrusion boot trace and persistent printk capture.
 * Early boot only appends to static RAM. Files and the logger thread are
 * created by the mount hook after Android mounts the real /metadata.
 */

#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/timekeeping.h>
#include <linux/utsname.h>
#include <linux/venus_boottrace.h>

#define VENUS_TRACE_MAX_RECORDS 8192
#define VENUS_TRACE_SLOT_A "/metadata/venus_boottrace-A.txt"
#define VENUS_TRACE_SLOT_B "/metadata/venus_boottrace-B.txt"
#define VENUS_KMSG_SLOT_A "/metadata/venus-kmsg-A.log"
#define VENUS_KMSG_SLOT_B "/metadata/venus-kmsg-B.log"
#define VENUS_KMSG_BUFSZ 8192
#define VENUS_KMSG_MAX_BYTES ((loff_t)CONFIG_VENUS_BOOTTRACE_KMSG_MAX_MB << 20)
#define VENUS_MIN_FREE_BYTES (4ULL << 20)

enum venus_trace_type {
	VENUS_TRACE_STAGE = 1,
	VENUS_TRACE_INITCALL_ENTER,
	VENUS_TRACE_INITCALL_EXIT,
};

struct venus_trace_record {
	u64 time_ns;
	unsigned long fn;
	s32 ret;
	u16 type;
	u16 stage;
};

static struct venus_trace_record venus_records[VENUS_TRACE_MAX_RECORDS];
static unsigned int venus_record_count;
static bool venus_clock_is_ready;
static bool venus_trace_sealed;
static int venus_metadata_state;

static void venus_record(u16 type, u16 stage, unsigned long fn, int ret)
{
	struct venus_trace_record *r;
	unsigned int idx = venus_record_count;

	if (venus_trace_sealed || idx >= VENUS_TRACE_MAX_RECORDS)
		return;

	/* Only the boot CPU, then PID 1, writes these serialized boot records. */
	r = &venus_records[idx];
	r->time_ns = venus_clock_is_ready ? ktime_get_ns() : 0;
	r->fn = fn;
	r->ret = ret;
	r->type = type;
	r->stage = stage;
	venus_record_count = idx + 1;
}

void venus_boottrace_stage(unsigned int stage)
{
	venus_record(VENUS_TRACE_STAGE, (u16)stage, 0, 0);
	if (stage == 12) {
		/* Publish the complete trace to the mount hook's acquire load. */
		smp_store_release(&venus_trace_sealed, true);
	}
}

void venus_boottrace_clock_ready(void)
{
	venus_clock_is_ready = true;
}

void venus_boottrace_initcall_start(initcall_t fn)
{
	/* do_one_initcall() is also used by runtime module loading. */
	if (current->pid == 1)
		venus_record(VENUS_TRACE_INITCALL_ENTER, 0, (unsigned long)fn, 0);
}

void venus_boottrace_initcall_finish(initcall_t fn, int ret)
{
	if (current->pid == 1)
		venus_record(VENUS_TRACE_INITCALL_EXIT, 0, (unsigned long)fn, ret);
}

static time64_t venus_slot_mtime(const char *name, bool *exists)
{
	struct path path;
	time64_t sec;

	*exists = false;
	if (kern_path(name, LOOKUP_FOLLOW, &path))
		return 0;

	sec = d_inode(path.dentry)->i_mtime.tv_sec;
	*exists = true;
	path_put(&path);
	return sec;
}

static const char *venus_choose_slot(const char *a, const char *b)
{
	bool ae, be;
	time64_t am, bm;

	am = venus_slot_mtime(a, &ae);
	bm = venus_slot_mtime(b, &be);
	if (!ae)
		return a;
	if (!be)
		return b;
	return am <= bm ? a : b;
}

static const char *venus_stage_name(unsigned int stage)
{
	switch (stage) {
	case 0: return "start_kernel";
	case 1: return "setup_arch complete";
	case 2: return "mm_init complete";
	case 3: return "sched_init complete";
	case 4: return "irq/timer/time init complete";
	case 5: return "console_init complete";
	case 6: return "entering rest_init";
	case 7: return "workqueue_init complete";
	case 8: return "pre-SMP initcalls begin";
	case 9: return "pre-SMP initcalls complete";
	case 10: return "built-in initcalls begin";
	case 11: return "built-in initcalls complete";
	case 12: return "attempting userspace init";
	default: return "unknown";
	}
}

static int venus_write_all(struct file *file, const char *buf,
			   size_t len, loff_t *pos)
{
	while (len) {
		ssize_t n;

		file_start_write(file);
		n = __kernel_write(file, buf, len, pos);
		file_end_write(file);

		if (n < 0)
			return n;
		if (!n)
			return -EIO;
		buf += n;
		len -= n;
	}
	return 0;
}

static int venus_check_space(struct file *file)
{
	struct kstatfs stat;
	int ret = vfs_statfs(&file->f_path, &stat);

	if (ret)
		return ret;
	if ((u64)stat.f_bavail * stat.f_bsize < VENUS_MIN_FREE_BYTES)
		return -ENOSPC;
	return 0;
}

static struct file *venus_open_log(const char *name)
{
	struct file *file;
	int ret;

	file = filp_open(name, O_WRONLY | O_CREAT | O_TRUNC | O_LARGEFILE |
			 O_NOFOLLOW | O_NONBLOCK, 0600);
	if (IS_ERR(file))
		return file;

	ret = S_ISREG(file_inode(file)->i_mode) ? venus_check_space(file) : -EINVAL;
	if (ret) {
		filp_close(file, NULL);
		return ERR_PTR(ret);
	}
	return file;
}

struct venus_kmsg_ctx {
	struct file *out;
	char *buf;
	loff_t out_pos;
	u64 seq;
	u32 idx;
};

static int venus_kmsg_thread(void *arg)
{
	struct venus_kmsg_ctx *ctx = arg;
	unsigned long last_sync = jiffies;
	bool dirty = false;
	int ret = 0;

	set_freezable();
	while (!kthread_freezable_should_stop(NULL)) {
		ssize_t n = venus_printk_read_record(ctx->buf, VENUS_KMSG_BUFSZ,
						   &ctx->seq, &ctx->idx);

		if (n == -EPIPE)
			n = scnprintf(ctx->buf, VENUS_KMSG_BUFSZ,
				      "--- VENUS KMSG: ring overrun; reader resynced ---\n");
		else if (n == -ENOSPC)
			n = scnprintf(ctx->buf, VENUS_KMSG_BUFSZ,
				      "--- VENUS KMSG: oversized record skipped ---\n");
		else if (n < 0) {
			ret = n;
			break;
		}

		/* Leave room for a final marker, keeping each file below its cap. */
		if (n > 0 && ctx->out_pos + n > VENUS_KMSG_MAX_BYTES - 128) {
			static const char limit[] =
				"--- VENUS KMSG: size limit reached; capture stopped ---\n";

			ret = venus_write_all(ctx->out, limit, sizeof(limit) - 1,
					      &ctx->out_pos);
			break;
		}
		if (n > 0) {
			ret = venus_write_all(ctx->out, ctx->buf, n, &ctx->out_pos);
			if (ret)
				break;
			dirty = true;
		}

		/* Check even during a continuous printk stream, not only at idle. */
		if (time_after_eq(jiffies, last_sync + HZ / 2)) {
			if (dirty) {
				ret = vfs_fsync(ctx->out, 0);
				if (ret)
					break;
				dirty = false;
			}
			last_sync = jiffies;
			ret = venus_check_space(ctx->out);
			if (ret)
				break;
		}

		if (n)
			cond_resched();
		else
			msleep(50);
	}

	vfs_fsync(ctx->out, 0);
	filp_close(ctx->out, NULL);
	kfree(ctx->buf);
	kfree(ctx);
	pr_info("venus_boottrace: kmsg capture stopped (error %d)\n", ret);
	return ret;
}

static int venus_start_kmsg_logger(void)
{
	const char *slot;
	struct venus_kmsg_ctx *ctx;
	struct task_struct *task;
	int ret, len;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->buf = kmalloc(VENUS_KMSG_BUFSZ, GFP_KERNEL);
	if (!ctx->buf) {
		ret = -ENOMEM;
		goto free_ctx;
	}
	ctx->seq = U64_MAX;
	slot = venus_choose_slot(VENUS_KMSG_SLOT_A, VENUS_KMSG_SLOT_B);
	ctx->out = venus_open_log(slot);
	if (IS_ERR(ctx->out)) {
		ret = PTR_ERR(ctx->out);
		goto free_buf;
	}

	len = scnprintf(ctx->buf, VENUS_KMSG_BUFSZ,
		"VENUS PERSISTENT PRINTK-RING CAPTURE\n"
		"kernel=%s max_bytes=%lld\n"
		"starts after /metadata mount; reads printk directly\n"
		"==================================================\n",
		init_uts_ns.name.release, VENUS_KMSG_MAX_BYTES);
	ret = venus_write_all(ctx->out, ctx->buf, len, &ctx->out_pos);
	if (ret)
		goto close_out;
	ret = vfs_fsync(ctx->out, 0);
	if (ret)
		goto close_out;

	task = kthread_run(venus_kmsg_thread, ctx, "venus_kmsg");
	if (IS_ERR(task)) {
		ret = PTR_ERR(task);
		goto close_out;
	}
	pr_info("venus_boottrace: saving printk to %s\n", slot);
	return 0;

close_out:
	filp_close(ctx->out, NULL);
free_buf:
	kfree(ctx->buf);
free_ctx:
	kfree(ctx);
	return ret;
}

void venus_boottrace_dump_metadata(void)
{
	const char *slot;
	struct file *file;
	unsigned int count, i;
	loff_t pos = 0;
	char line[256];
	int len, ret;

	/* The trace is immutable before userspace can mount /metadata. */
	if (!smp_load_acquire(&venus_trace_sealed) ||
	    cmpxchg(&venus_metadata_state, 0, 1))
		return;

	count = min_t(unsigned int, venus_record_count, VENUS_TRACE_MAX_RECORDS);
	slot = venus_choose_slot(VENUS_TRACE_SLOT_A, VENUS_TRACE_SLOT_B);
	file = venus_open_log(slot);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto failed;
	}

	len = scnprintf(line, sizeof(line),
		"VENUS LOW-INTRUSION BOOT TRACE\nkernel=%s\nrecords=%u max=%u\n"
		"format: seq time_ns TYPE detail\n"
		"========================================\n",
		init_uts_ns.name.release, count, VENUS_TRACE_MAX_RECORDS);
	ret = venus_write_all(file, line, len, &pos);
	for (i = 0; !ret && i < count; i++) {
		const struct venus_trace_record *r = &venus_records[i];

		if (r->type == VENUS_TRACE_STAGE) {
			len = scnprintf(line, sizeof(line),
				"%04u %llu STAGE S%02u %s\n", i, r->time_ns,
				r->stage, venus_stage_name(r->stage));
		} else if (r->type == VENUS_TRACE_INITCALL_ENTER) {
			len = scnprintf(line, sizeof(line),
				"%04u %llu INIT_ENTER %pS\n", i,
				r->time_ns, (void *)r->fn);
		} else {
			len = scnprintf(line, sizeof(line),
				"%04u %llu INIT_EXIT  %pS ret=%d\n", i,
				r->time_ns, (void *)r->fn, r->ret);
		}
		ret = venus_write_all(file, line, len, &pos);
	}
	if (!ret) {
		len = scnprintf(line, sizeof(line),
			"%04u %llu STAGE S13 metadata mounted; exporting trace\n",
			count, ktime_get_ns());
		ret = venus_write_all(file, line, len, &pos);
	}
	if (!ret)
		ret = vfs_fsync(file, 0);
	filp_close(file, NULL);
	if (!ret)
		ret = venus_start_kmsg_logger();
	if (!ret) {
		WRITE_ONCE(venus_metadata_state, 2);
		return;
	}
failed:
	/* Allow a later successful mount/remount to retry a failed start. */
	WRITE_ONCE(venus_metadata_state, 0);
	pr_warn("venus_boottrace: metadata capture failed: %d\n", ret);
}
