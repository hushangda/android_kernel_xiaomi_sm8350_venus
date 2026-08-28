// SPDX-License-Identifier: GPL-2.0
/*
 * Functional ColorOS ABI translation for Qualcomm WALT kernels.
 *
 * ColorOS performance userspace expects OPlus scheduling nodes which are not
 * present in the Venus vendor kernel.  This driver keeps the userspace ABI,
 * but translates requests to the native per-task WALT boost mechanism.  It
 * deliberately has no permanent boost and no background polling while idle.
 */

#include <linux/compat.h>
#include <linux/cpu.h>
#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/kernel_stat.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/oplus_coloros_compat.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sysctl.h>
#include <linux/uaccess.h>
#include <linux/user_namespace.h>
#include <linux/workqueue.h>

#define OPLUS_SYSTEM_UID		1000
#define OPLUS_FIRST_APP_UID		10000
#define OPLUS_LAST_APP_UID		19999
#define OPLUS_PER_USER_RANGE		100000

#define SA_TYPE_LIGHT			BIT(0)
#define SA_TYPE_HEAVY			BIT(1)
#define SA_TYPE_ANIMATOR		BIT(2)
#define SA_TYPE_LISTPICK		BIT(3)
#define SA_OPT_SET			BIT(7)
#define SA_OPT_RESET			BIT(8)
#define SA_OPT_SET_PRIORITY		BIT(9)
#define SA_TYPE_SWIFT			BIT(14)
#define SA_UX_MASK			(SA_TYPE_LIGHT | SA_TYPE_HEAVY | \
					 SA_TYPE_ANIMATOR | SA_TYPE_LISTPICK | \
					 SA_TYPE_SWIFT)
#define SA_UX_PRIORITY_MASK		0xff000000U

#define OPLUS_IM_SURFACEFLINGER		1
#define OPLUS_IM_RENDERENGINE		3
#define OPLUS_IM_AUDIO			6
#define OPLUS_IM_RENDER_THREAD		18
#define OPLUS_IM_CLEAR_BASE		64

#define OPLUS_UX_MAGIC			0x27
#define OPLUS_BINDER_UX_MAGIC		0x69
#define OPLUS_FRAME_MAGIC		0xde
#define OPLUS_ASYNC_FRAME_MAGIC		0xfb
#define OPLUS_FRAME_START		200
#define OPLUS_FRAME_END			211
#define OPLUS_FRAME_TIMEOUT		212

#define OPLUS_STAGE_RING_ENTRIES	16
#define OPLUS_STAGE_LEN			64
#define OPLUS_BOOT_RING_ENTRIES		192
#define OPLUS_BOOT_EVENT_LEN		128

/* Exact Venus ColorOS ioctl payloads: 0x30-byte control, 0x44-byte stune. */
struct oplus_ofb_ctrl_data {
	pid_t pid;
	pid_t tid;
	pid_t hwtid1;
	pid_t hwtid2;
	int stage;
	s64 vsync_ns;
	int capacity_need;
	int related;
	int group_id;
};

struct oplus_ofb_stune_data {
	int data[17];
};

static unsigned long sched_boost_page;
static int fbg_enabled = 1;
static int fbg_boost_ms = 120;
static int fbg_boost_level = TASK_BOOST_ON_MAX;
static int audio_status;
static int ux_query_pid;
static int im_query_pid;

static struct proc_dir_entry *binder_dir;
static struct proc_dir_entry *scheduler_dir;
static struct proc_dir_entry *sched_assist_dir;
static struct proc_dir_entry *audio_dir;
static struct proc_dir_entry *frame_dir;
static struct proc_dir_entry *async_frame_dir;
static struct proc_dir_entry *jank_dir;
static struct proc_dir_entry *cpu_jank_dir;
static struct ctl_table_header *fbg_sysctl_header;

static DEFINE_SPINLOCK(stage_lock);
static char stage_ring[OPLUS_STAGE_RING_ENTRIES][OPLUS_STAGE_LEN];
static unsigned int stage_head;
static unsigned int stage_count;

static DEFINE_SPINLOCK(boot_lock);
static char boot_ring[OPLUS_BOOT_RING_ENTRIES][OPLUS_BOOT_EVENT_LEN];
static unsigned int boot_head;
static unsigned int boot_count;

static DEFINE_MUTEX(shutdown_lock);
static int shutdown_phase;
static unsigned int shutdown_timeout_ms = 30000;
static struct delayed_work shutdown_timeout_work;

static DEFINE_MUTEX(clm_lock);
static struct delayed_work clm_work;
static bool clm_enabled;
static int clm_mux_switch;
static u64 clm_prev_total[NR_CPUS];
static u64 clm_prev_idle[NR_CPUS];
static unsigned int clm_load[NR_CPUS];

static bool oplus_privileged(void)
{
	uid_t uid = from_kuid(&init_user_ns, current_euid());

	return uid == 0 || uid == OPLUS_SYSTEM_UID;
}

static bool oplus_app_self(struct task_struct *task)
{
	uid_t uid = from_kuid(&init_user_ns, current_euid());

	uid %= OPLUS_PER_USER_RANGE;
	return task->tgid == current->tgid &&
		(uid == OPLUS_SYSTEM_UID ||
		 (uid >= OPLUS_FIRST_APP_UID && uid <= OPLUS_LAST_APP_UID));
}

static struct task_struct *oplus_get_task(pid_t pid)
{
	struct task_struct *task;

	if (pid <= 0 || pid > PID_MAX_DEFAULT)
		return NULL;

	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	return task;
}

static int oplus_ux_level(u32 state)
{
	if (state & (SA_TYPE_HEAVY | SA_TYPE_ANIMATOR |
		     SA_TYPE_LISTPICK | SA_TYPE_SWIFT))
		return TASK_BOOST_ON_MAX;
	if (state & SA_TYPE_LIGHT)
		return TASK_BOOST_ON_MID;
	return TASK_BOOST_NONE;
}

int oplus_coloros_task_boost(struct task_struct *task, int native_boost)
{
	int boost = native_boost;
	u32 ux_state;
	u64 im_flag;
	u64 expires;
	u32 *slots;
	int i;

	if (unlikely(!READ_ONCE(fbg_enabled)))
		return native_boost;

	ux_state = READ_ONCE(task->wts.oplus_ux_state);
	boost = max(boost, oplus_ux_level(ux_state));
	if (READ_ONCE(task->wts.oplus_binder_ux))
		boost = max(boost, TASK_BOOST_ON_MID);

	im_flag = READ_ONCE(task->wts.oplus_im_flag);
	if (im_flag & (BIT_ULL(OPLUS_IM_RENDERENGINE) |
		       BIT_ULL(OPLUS_IM_RENDER_THREAD)))
		boost = max(boost, TASK_BOOST_ON_MAX);
	else if (im_flag & (BIT_ULL(OPLUS_IM_SURFACEFLINGER) |
			    BIT_ULL(OPLUS_IM_AUDIO)))
		boost = max(boost, TASK_BOOST_ON_MID);

	expires = READ_ONCE(task->wts.oplus_frame_boost_expires);
	if (expires && sched_clock() <= expires)
		boost = max_t(int, boost,
			READ_ONCE(task->wts.oplus_frame_boost_level));
	else if (expires) {
		WRITE_ONCE(task->wts.oplus_frame_boost_expires, 0);
		WRITE_ONCE(task->wts.oplus_frame_boost_level, 0);
	}

	/* Three userspace-owned {active, tid} slots in one shared page. */
	if (likely(sched_boost_page)) {
		slots = (u32 *)sched_boost_page;
		for (i = 0; i < 3; i++) {
			if (READ_ONCE(slots[i * 2]) &&
			    READ_ONCE(slots[i * 2 + 1]) == task->pid) {
				boost = max(boost, TASK_BOOST_ON_MAX);
				break;
			}
		}
	}

	return boost;
}
EXPORT_SYMBOL_GPL(oplus_coloros_task_boost);

static int oplus_update_ux(struct task_struct *task, u32 value)
{
	u32 original = READ_ONCE(task->wts.oplus_ux_state);
	u32 state = original;

	if ((value & SA_OPT_SET) && (value & SA_OPT_RESET))
		return -EINVAL;

	if ((value & (SA_OPT_RESET | SA_OPT_SET_PRIORITY)) ==
	    (SA_OPT_RESET | SA_OPT_SET_PRIORITY)) {
		state = value & (SA_UX_PRIORITY_MASK | SA_UX_MASK);
	} else if (value & SA_OPT_RESET) {
		state = (value & SA_UX_MASK) | (original & SA_UX_PRIORITY_MASK);
	} else if ((value & (SA_OPT_SET | SA_OPT_SET_PRIORITY)) ==
		   (SA_OPT_SET | SA_OPT_SET_PRIORITY)) {
		state = value & (SA_UX_PRIORITY_MASK | SA_UX_MASK);
		if (value & SA_UX_MASK)
			state |= original & SA_UX_MASK;
	} else if (value & SA_OPT_SET) {
		state = original & SA_UX_PRIORITY_MASK;
		if (value & SA_UX_MASK)
			state |= (original | value) & SA_UX_MASK;
	} else if (value & SA_OPT_SET_PRIORITY) {
		if (!(original & SA_UX_MASK))
			return original;
		state = (original & SA_UX_MASK) | (value & SA_UX_PRIORITY_MASK);
	} else if (!(value & SA_UX_MASK)) {
		state = original & SA_UX_PRIORITY_MASK;
	} else {
		state = original & ~(value & SA_UX_MASK);
	}

	/* Swift is owned by the audio path and cannot be cleared remotely. */
	if (original & SA_TYPE_SWIFT)
		state |= SA_TYPE_SWIFT;
	WRITE_ONCE(task->wts.oplus_ux_state, state);
	return state;
}

static int oplus_set_im_flag(struct task_struct *task, int flag)
{
	u64 state = READ_ONCE(task->wts.oplus_im_flag);

	if (flag >= 0 && flag < OPLUS_IM_CLEAR_BASE)
		state |= BIT_ULL(flag);
	else if (flag >= OPLUS_IM_CLEAR_BASE && flag < OPLUS_IM_CLEAR_BASE * 2)
		state &= ~BIT_ULL(flag - OPLUS_IM_CLEAR_BASE);
	else
		return -ERANGE;
	WRITE_ONCE(task->wts.oplus_im_flag, state);
	return 0;
}

static int oplus_validate_task_access(struct task_struct *task, pid_t tgid,
				      bool app_node)
{
	if (!task || task->tgid != tgid)
		return -EINVAL;
	if (app_node)
		return oplus_app_self(task) ? 0 : -EPERM;
	return oplus_privileged() ? 0 : -EPERM;
}

static long oplus_ux_ioctl_common(unsigned int cmd, unsigned long arg,
				  bool app_node)
{
	struct task_struct *task;
	int values[3];
	int ret;

	if (_IOC_TYPE(cmd) != OPLUS_UX_MAGIC)
		return -ENOTTY;

	if (_IOC_NR(cmd) == 0) {
		if (copy_from_user(values, (void __user *)arg, sizeof(int) * 2))
			return -EFAULT;
		task = oplus_get_task(values[1]);
		ret = oplus_validate_task_access(task, values[0], app_node);
		if (!ret)
			ret = READ_ONCE(task->wts.oplus_ux_state);
	} else if (_IOC_NR(cmd) == 1) {
		if (copy_from_user(values, (void __user *)arg, sizeof(values)))
			return -EFAULT;
		task = oplus_get_task(values[1]);
		ret = oplus_validate_task_access(task, values[0], app_node);
		if (!ret)
			ret = oplus_update_ux(task, values[2]);
	} else {
		return -ENOTTY;
	}

	if (task)
		put_task_struct(task);
	return ret;
}

static long oplus_ux_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	return oplus_ux_ioctl_common(cmd, arg, false);
}

static long oplus_ux_app_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	return oplus_ux_ioctl_common(cmd, arg, true);
}

#ifdef CONFIG_COMPAT
static long oplus_ux_compat_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	return oplus_ux_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}

static long oplus_ux_app_compat_ioctl(struct file *file, unsigned int cmd,
				      unsigned long arg)
{
	return oplus_ux_app_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static ssize_t oplus_ux_write_common(const char __user *buf, size_t count,
				     bool app_node)
{
	struct task_struct *task;
	char input[64];
	char op;
	int pid, value;
	int ret;

	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buf, count))
		return -EFAULT;
	input[count] = '\0';

	if (sscanf(input, " %c %d %i", &op, &pid, &value) == 3 && op == 'p') {
		task = oplus_get_task(pid);
		ret = oplus_validate_task_access(task, task ? task->tgid : -1,
					 app_node);
		if (!ret)
			ret = oplus_update_ux(task, value);
		if (task)
			put_task_struct(task);
		return ret < 0 ? ret : count;
	}
	if (sscanf(input, " %c %d", &op, &pid) == 2 && op == 'r') {
		WRITE_ONCE(ux_query_pid, pid);
		return count;
	}
	return -EINVAL;
}

static ssize_t oplus_ux_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	return oplus_ux_write_common(buf, count, false);
}

static ssize_t oplus_ux_app_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	return oplus_ux_write_common(buf, count, true);
}

static ssize_t oplus_ux_read(struct file *file, char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct task_struct *task = oplus_get_task(READ_ONCE(ux_query_pid));
	char output[160];
	int len;

	if (!task)
		len = scnprintf(output, sizeof(output), "Task not found\n");
	else {
		len = scnprintf(output, sizeof(output),
			"comm=%s pid=%d tgid=%d ux_state=0x%08x im_flag=0x%016llx\n",
			task->comm, task->pid, task->tgid,
			READ_ONCE(task->wts.oplus_ux_state),
			READ_ONCE(task->wts.oplus_im_flag));
		put_task_struct(task);
	}
	return simple_read_from_buffer(buf, count, ppos, output, len);
}

static const struct file_operations oplus_ux_fops = {
	.owner = THIS_MODULE,
	.read = oplus_ux_read,
	.write = oplus_ux_write,
	.unlocked_ioctl = oplus_ux_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = oplus_ux_compat_ioctl,
#endif
	.llseek = default_llseek,
};

static const struct file_operations oplus_ux_app_fops = {
	.owner = THIS_MODULE,
	.read = oplus_ux_read,
	.write = oplus_ux_app_write,
	.unlocked_ioctl = oplus_ux_app_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = oplus_ux_app_compat_ioctl,
#endif
	.llseek = default_llseek,
};

static ssize_t oplus_im_write_common(const char __user *buf, size_t count,
				     bool app_node)
{
	struct task_struct *task;
	char input[64];
	char op;
	int pid, flag;
	int ret;

	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buf, count))
		return -EFAULT;
	input[count] = '\0';

	if (sscanf(input, " %c %d %d", &op, &pid, &flag) == 3 && op == 'p') {
		task = oplus_get_task(pid);
		if (!task)
			return -ESRCH;
		ret = app_node ? (oplus_app_self(task) ? 0 : -EPERM) :
			(oplus_privileged() ? 0 : -EPERM);
		if (!ret)
			ret = oplus_set_im_flag(task, flag);
		put_task_struct(task);
		return ret ? ret : count;
	}
	if (sscanf(input, " %c %d", &op, &pid) == 2 && op == 'r') {
		WRITE_ONCE(im_query_pid, pid);
		return count;
	}
	return -EINVAL;
}

static ssize_t oplus_im_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	return oplus_im_write_common(buf, count, false);
}

static ssize_t oplus_im_app_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	return oplus_im_write_common(buf, count, true);
}

static ssize_t oplus_im_read(struct file *file, char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct task_struct *task = oplus_get_task(READ_ONCE(im_query_pid));
	char output[128];
	int len;

	if (!task)
		len = scnprintf(output, sizeof(output), "Can not find task\n");
	else {
		len = scnprintf(output, sizeof(output),
			"comm=%s pid=%d tgid=%d im_flag=0x%016llx\n",
			task->comm, task->pid, task->tgid,
			READ_ONCE(task->wts.oplus_im_flag));
		put_task_struct(task);
	}
	return simple_read_from_buffer(buf, count, ppos, output, len);
}

static const struct file_operations oplus_im_fops = {
	.owner = THIS_MODULE,
	.read = oplus_im_read,
	.write = oplus_im_write,
	.llseek = default_llseek,
};

static const struct file_operations oplus_im_app_fops = {
	.owner = THIS_MODULE,
	.read = oplus_im_read,
	.write = oplus_im_app_write,
	.llseek = default_llseek,
};

static long oplus_binder_ux_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	struct task_struct *task;
	int values[2];

	if (_IOC_TYPE(cmd) != OPLUS_BINDER_UX_MAGIC || _IOC_NR(cmd) != 0)
		return -ENOTTY;
	if (copy_from_user(values, (void __user *)arg, sizeof(values)))
		return -EFAULT;
	task = oplus_get_task(values[0]);
	if (!task)
		return -ESRCH;
	if (!oplus_privileged() && !oplus_app_self(task)) {
		put_task_struct(task);
		return -EPERM;
	}
	WRITE_ONCE(task->wts.oplus_binder_ux, !!values[1]);
	put_task_struct(task);
	return 0;
}

#ifdef CONFIG_COMPAT
static long oplus_binder_ux_compat_ioctl(struct file *file, unsigned int cmd,
					 unsigned long arg)
{
	return oplus_binder_ux_ioctl(file, cmd,
				     (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations oplus_binder_ux_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = oplus_binder_ux_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = oplus_binder_ux_compat_ioctl,
#endif
};

static int oplus_mark_frame_task(pid_t pid, int level,
				 unsigned int duration_ms, bool self_only)
{
	struct task_struct *task = oplus_get_task(pid);

	if (!task)
		return pid ? -ESRCH : 0;
	if (self_only && !oplus_app_self(task)) {
		put_task_struct(task);
		return -EPERM;
	}
	if (!level) {
		WRITE_ONCE(task->wts.oplus_frame_boost_level, 0);
		WRITE_ONCE(task->wts.oplus_frame_boost_expires, 0);
	} else {
		WRITE_ONCE(task->wts.oplus_frame_boost_level, level);
		WRITE_ONCE(task->wts.oplus_frame_boost_expires,
			   sched_clock() + (u64)duration_ms * NSEC_PER_MSEC);
	}
	put_task_struct(task);
	return 0;
}

static int oplus_frame_tasks(pid_t *pids, bool enable, bool self_only)
{
	int level = enable ? clamp_val(READ_ONCE(fbg_boost_level),
				       TASK_BOOST_ON_MID,
				       TASK_BOOST_STRICT_MAX) : 0;
	unsigned int duration = enable ? clamp_val(READ_ONCE(fbg_boost_ms),
						    16, 1000) : 0;
	int i, ret = 0;

	for (i = 0; i < 4; i++) {
		int task_ret = oplus_mark_frame_task(pids[i], level, duration,
						     self_only);

		if (task_ret && !ret)
			ret = task_ret;
	}
	return ret;
}

static long oplus_frame_ioctl_common(unsigned int cmd, unsigned long arg,
				      bool async, bool system_only)
{
	struct oplus_ofb_ctrl_data ctrl = { };
	struct oplus_ofb_stune_data stune;
	pid_t async_pids[4] = { };
	int async_data[12] = { };
	unsigned int nr = _IOC_NR(cmd);
	bool self_only = !oplus_privileged();

	if (_IOC_TYPE(cmd) != (async ? OPLUS_ASYNC_FRAME_MAGIC :
					       OPLUS_FRAME_MAGIC))
		return -ENOTTY;
	if (system_only && !oplus_privileged())
		return -EPERM;

	if (async) {
		if (nr < 1 || nr > 9)
			return -ENOTTY;
		if (_IOC_SIZE(cmd) != sizeof(async_data))
			return -EINVAL;
		if (copy_from_user(async_data, (void __user *)arg,
				   sizeof(async_data)))
			return -EFAULT;
		memcpy(async_pids, async_data, sizeof(async_pids));
		return oplus_frame_tasks(async_pids, true, self_only);
	}

	if (nr >= 1 && nr <= 9) {
		if (_IOC_SIZE(cmd) != sizeof(ctrl))
			return -EINVAL;
		if (copy_from_user(&ctrl, (void __user *)arg, sizeof(ctrl)))
			return -EFAULT;
	} else if (nr == 10 || nr == 11) {
		if (_IOC_SIZE(cmd) != sizeof(stune))
			return -EINVAL;
		if (copy_from_user(&stune, (void __user *)arg, sizeof(stune)))
			return -EFAULT;
		/* Native WALT already supplies the requested cluster tuning. */
		return 0;
	}

	switch (nr) {
	case 1: /* SET_FPS: accepted; WALT already follows frame demand. */
		return 0;
	case 2: /* BOOST_HIT */
		return oplus_frame_tasks(&ctrl.pid,
			ctrl.stage != OPLUS_FRAME_END, self_only);
	case 3: /* END_FRAME */
		return oplus_frame_tasks(&ctrl.pid, false, self_only);
	case 4:
	case 5:
	case 6:
	case 7:
	case 8:
	case 9:
		return 0;
	default:
		return -ENOTTY;
	}
}

static long oplus_frame_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	return oplus_frame_ioctl_common(cmd, arg, false, false);
}

static long oplus_frame_sys_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	return oplus_frame_ioctl_common(cmd, arg, false, true);
}

static long oplus_async_frame_ioctl(struct file *file, unsigned int cmd,
				    unsigned long arg)
{
	return oplus_frame_ioctl_common(cmd, arg, true, false);
}

#ifdef CONFIG_COMPAT
static long oplus_frame_compat_ioctl(struct file *file, unsigned int cmd,
				     unsigned long arg)
{
	return oplus_frame_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}

static long oplus_frame_sys_compat_ioctl(struct file *file, unsigned int cmd,
					 unsigned long arg)
{
	return oplus_frame_sys_ioctl(file, cmd,
				     (unsigned long)compat_ptr(arg));
}

static long oplus_async_frame_compat_ioctl(struct file *file,
					   unsigned int cmd,
					   unsigned long arg)
{
	return oplus_async_frame_ioctl(file, cmd,
				       (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations oplus_frame_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = oplus_frame_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = oplus_frame_compat_ioctl,
#endif
};

static const struct file_operations oplus_frame_sys_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = oplus_frame_sys_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = oplus_frame_sys_compat_ioctl,
#endif
};

static const struct file_operations oplus_async_frame_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = oplus_async_frame_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = oplus_async_frame_compat_ioctl,
#endif
};

static int sched_boost_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;

	if (size != PAGE_SIZE || vma->vm_pgoff)
		return -EINVAL;
	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
	return remap_pfn_range(vma, vma->vm_start,
			       virt_to_pfn((void *)sched_boost_page), PAGE_SIZE,
			       vma->vm_page_prot);
}

static const struct file_operations sched_boost_fops = {
	.owner = THIS_MODULE,
	.mmap = sched_boost_mmap,
};

static struct miscdevice sched_boost_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "sched_boost",
	.fops = &sched_boost_fops,
	.mode = 0666,
};

static ssize_t audio_status_read(struct file *file, char __user *buf,
				 size_t count, loff_t *ppos)
{
	char output[24];
	int len = scnprintf(output, sizeof(output), "%d\n",
			    READ_ONCE(audio_status));

	return simple_read_from_buffer(buf, count, ppos, output, len);
}

static ssize_t audio_status_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	char input[24];
	int value;

	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buf, count))
		return -EFAULT;
	input[count] = '\0';
	if (kstrtoint(strstrip(input), 0, &value))
		return -EINVAL;
	WRITE_ONCE(audio_status, value);
	return count;
}

static const struct file_operations audio_status_fops = {
	.owner = THIS_MODULE,
	.read = audio_status_read,
	.write = audio_status_write,
	.llseek = default_llseek,
};

static ssize_t ring_read(char __user *buf, size_t count, loff_t *ppos,
			 char ring[][OPLUS_STAGE_LEN], unsigned int entries,
			 unsigned int head, unsigned int used, spinlock_t *lock)
{
	char *output;
	unsigned long flags;
	unsigned int start, i;
	ssize_t len = 0;

	output = kzalloc(entries * (OPLUS_STAGE_LEN + 1), GFP_KERNEL);
	if (!output)
		return -ENOMEM;
	spin_lock_irqsave(lock, flags);
	start = (head + entries - used) % entries;
	for (i = 0; i < used; i++)
		len += scnprintf(output + len, entries * (OPLUS_STAGE_LEN + 1) - len,
				 "%s%s", ring[(start + i) % entries],
				 i + 1 == used ? "" : ",");
	spin_unlock_irqrestore(lock, flags);
	len = simple_read_from_buffer(buf, count, ppos, output, len);
	kfree(output);
	return len;
}

static ssize_t stage_report_read(struct file *file, char __user *buf,
				 size_t count, loff_t *ppos)
{
	return ring_read(buf, count, ppos, stage_ring, OPLUS_STAGE_RING_ENTRIES,
			 READ_ONCE(stage_head), READ_ONCE(stage_count), &stage_lock);
}

static ssize_t stage_report_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	char input[OPLUS_STAGE_LEN];
	unsigned long flags;
	size_t len = min_t(size_t, count, sizeof(input) - 1);

	if (!len)
		return -EINVAL;
	if (copy_from_user(input, buf, len))
		return -EFAULT;
	input[len] = '\0';
	strreplace(input, '\n', '\0');
	spin_lock_irqsave(&stage_lock, flags);
	strscpy(stage_ring[stage_head], input, OPLUS_STAGE_LEN);
	stage_head = (stage_head + 1) % OPLUS_STAGE_RING_ENTRIES;
	stage_count = min(stage_count + 1, (unsigned int)OPLUS_STAGE_RING_ENTRIES);
	spin_unlock_irqrestore(&stage_lock, flags);
	return count;
}

static const struct file_operations stage_report_fops = {
	.owner = THIS_MODULE,
	.read = stage_report_read,
	.write = stage_report_write,
	.llseek = default_llseek,
};

static ssize_t phoenix_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	char *output;
	unsigned long flags;
	unsigned int start, i;
	ssize_t len = 0;

	output = kzalloc(OPLUS_BOOT_RING_ENTRIES * OPLUS_BOOT_EVENT_LEN,
			 GFP_KERNEL);
	if (!output)
		return -ENOMEM;
	spin_lock_irqsave(&boot_lock, flags);
	start = (boot_head + OPLUS_BOOT_RING_ENTRIES - boot_count) %
		OPLUS_BOOT_RING_ENTRIES;
	for (i = 0; i < boot_count; i++)
		len += scnprintf(output + len,
			OPLUS_BOOT_RING_ENTRIES * OPLUS_BOOT_EVENT_LEN - len,
			"%s\n", boot_ring[(start + i) % OPLUS_BOOT_RING_ENTRIES]);
	spin_unlock_irqrestore(&boot_lock, flags);
	len = simple_read_from_buffer(buf, count, ppos, output, len);
	kfree(output);
	return len;
}

static ssize_t phoenix_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	char input[OPLUS_BOOT_EVENT_LEN];
	unsigned long flags;
	size_t len = min_t(size_t, count, sizeof(input) - 1);

	if (!len)
		return -EINVAL;
	if (copy_from_user(input, buf, len))
		return -EFAULT;
	input[len] = '\0';
	strreplace(input, '\n', '\0');
	spin_lock_irqsave(&boot_lock, flags);
	strscpy(boot_ring[boot_head], input, OPLUS_BOOT_EVENT_LEN);
	boot_head = (boot_head + 1) % OPLUS_BOOT_RING_ENTRIES;
	boot_count = min(boot_count + 1, (unsigned int)OPLUS_BOOT_RING_ENTRIES);
	spin_unlock_irqrestore(&boot_lock, flags);
	return count;
}

static const struct file_operations phoenix_fops = {
	.owner = THIS_MODULE,
	.read = phoenix_read,
	.write = phoenix_write,
	.llseek = default_llseek,
};

static void shutdown_timeout_fn(struct work_struct *work)
{
	int phase = READ_ONCE(shutdown_phase);

	if (!phase)
		return;
	pr_err("ColorOS shutdown phase %d timed out after %u ms\n", phase,
	       READ_ONCE(shutdown_timeout_ms));
	show_state_filter(TASK_UNINTERRUPTIBLE);
}

static ssize_t shutdown_detect_read(struct file *file, char __user *buf,
				    size_t count, loff_t *ppos)
{
	char output[64];
	int len = scnprintf(output, sizeof(output), "phase=%d timeout=%u\n",
			    READ_ONCE(shutdown_phase),
			    READ_ONCE(shutdown_timeout_ms));

	return simple_read_from_buffer(buf, count, ppos, output, len);
}

static ssize_t shutdown_detect_write(struct file *file, const char __user *buf,
				     size_t count, loff_t *ppos)
{
	char input[64];
	unsigned int timeout;
	int phase;
	int parsed;

	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buf, count))
		return -EFAULT;
	input[count] = '\0';
	parsed = sscanf(input, "%d %u", &phase, &timeout);
	if (parsed < 1 || phase < 0)
		return -EINVAL;

	mutex_lock(&shutdown_lock);
	cancel_delayed_work_sync(&shutdown_timeout_work);
	shutdown_phase = phase;
	if (parsed == 2)
		shutdown_timeout_ms = clamp_val(timeout, 1000, 300000);
	if (phase)
		schedule_delayed_work(&shutdown_timeout_work,
				      msecs_to_jiffies(shutdown_timeout_ms));
	mutex_unlock(&shutdown_lock);
	return count;
}

static const struct file_operations shutdown_detect_fops = {
	.owner = THIS_MODULE,
	.read = shutdown_detect_read,
	.write = shutdown_detect_write,
	.llseek = default_llseek,
};

static void clm_sample_work(struct work_struct *work)
{
	unsigned int delay_ms = (READ_ONCE(clm_mux_switch) & BIT(12)) ? 100 : 1000;
	int cpu, index;

	mutex_lock(&clm_lock);
	if (!clm_enabled) {
		mutex_unlock(&clm_lock);
		return;
	}
	for_each_online_cpu(cpu) {
		u64 total = 0;
		u64 idle;
		u64 delta_total, delta_idle;

		for (index = 0; index < NR_STATS; index++)
			total += kcpustat_cpu(cpu).cpustat[index];
		idle = kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE] +
			kcpustat_cpu(cpu).cpustat[CPUTIME_IOWAIT];
		delta_total = total - clm_prev_total[cpu];
		delta_idle = idle - clm_prev_idle[cpu];
		if (clm_prev_total[cpu] && delta_total)
			clm_load[cpu] = div64_u64(100ULL *
				(delta_total - min(delta_idle, delta_total)), delta_total);
		clm_prev_total[cpu] = total;
		clm_prev_idle[cpu] = idle;
	}
	mutex_unlock(&clm_lock);
	schedule_delayed_work(&clm_work, msecs_to_jiffies(delay_ms));
}

static ssize_t clm_enable_read(struct file *file, char __user *buf,
			       size_t count, loff_t *ppos)
{
	char output[192];
	int cpu;
	int len = scnprintf(output, sizeof(output), "%u", READ_ONCE(clm_enabled));

	for_each_online_cpu(cpu)
		len += scnprintf(output + len, sizeof(output) - len, " %d:%u",
				 cpu, READ_ONCE(clm_load[cpu]));
	len += scnprintf(output + len, sizeof(output) - len, "\n");
	return simple_read_from_buffer(buf, count, ppos, output, len);
}

static ssize_t clm_enable_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	char input[16];
	bool enabled;

	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buf, count))
		return -EFAULT;
	input[count] = '\0';
	if (kstrtobool(strstrip(input), &enabled))
		return -EINVAL;

	mutex_lock(&clm_lock);
	clm_enabled = enabled;
	if (enabled) {
		memset(clm_prev_total, 0, sizeof(clm_prev_total));
		memset(clm_prev_idle, 0, sizeof(clm_prev_idle));
	}
	mutex_unlock(&clm_lock);
	if (enabled)
		mod_delayed_work(system_wq, &clm_work, 0);
	else
		cancel_delayed_work_sync(&clm_work);
	return count;
}

static const struct file_operations clm_enable_fops = {
	.owner = THIS_MODULE,
	.read = clm_enable_read,
	.write = clm_enable_write,
	.llseek = default_llseek,
};

static ssize_t clm_mux_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	char output[24];
	int len = scnprintf(output, sizeof(output), "%d\n",
			    READ_ONCE(clm_mux_switch));

	return simple_read_from_buffer(buf, count, ppos, output, len);
}

static ssize_t clm_mux_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	char input[24];
	int value;

	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buf, count))
		return -EFAULT;
	input[count] = '\0';
	if (kstrtoint(strstrip(input), 0, &value))
		return -EINVAL;
	WRITE_ONCE(clm_mux_switch, value);
	return count;
}

static const struct file_operations clm_mux_fops = {
	.owner = THIS_MODULE,
	.read = clm_mux_read,
	.write = clm_mux_write,
	.llseek = default_llseek,
};

static struct ctl_table fbg_sysctls[] = {
	{
		.procname = "enabled",
		.data = &fbg_enabled,
		.maxlen = sizeof(fbg_enabled),
		.mode = 0644,
		.proc_handler = proc_dointvec_minmax,
		.extra1 = SYSCTL_ZERO,
		.extra2 = SYSCTL_ONE,
	},
	{
		.procname = "boost_ms",
		.data = &fbg_boost_ms,
		.maxlen = sizeof(fbg_boost_ms),
		.mode = 0644,
		.proc_handler = proc_dointvec,
	},
	{
		.procname = "boost_level",
		.data = &fbg_boost_level,
		.maxlen = sizeof(fbg_boost_level),
		.mode = 0644,
		.proc_handler = proc_dointvec,
	},
	{ }
};

static int __init oplus_coloros_compat_init(void)
{
	int ret;

	BUILD_BUG_ON(sizeof(struct oplus_ofb_ctrl_data) != 48);
	BUILD_BUG_ON(sizeof(struct oplus_ofb_stune_data) != 68);

	INIT_DELAYED_WORK(&shutdown_timeout_work, shutdown_timeout_fn);
	INIT_DELAYED_WORK(&clm_work, clm_sample_work);
	sched_boost_page = get_zeroed_page(GFP_KERNEL);
	if (!sched_boost_page)
		return -ENOMEM;
	ret = misc_register(&sched_boost_misc);
	if (ret)
		goto free_page;

	binder_dir = proc_mkdir("oplus_binder", NULL);
	scheduler_dir = proc_mkdir("oplus_scheduler", NULL);
	sched_assist_dir = scheduler_dir ? proc_mkdir("sched_assist", scheduler_dir) : NULL;
	audio_dir = sched_assist_dir ? proc_mkdir("audio", sched_assist_dir) : NULL;
	frame_dir = proc_mkdir("oplus_frame_boost", NULL);
	async_frame_dir = proc_mkdir("oplus_frame_boost_async", NULL);
	jank_dir = proc_mkdir("jank_info", NULL);
	cpu_jank_dir = jank_dir ? proc_mkdir("cpu_jank_info", jank_dir) : NULL;
	if (!binder_dir || !sched_assist_dir || !audio_dir || !frame_dir ||
	    !async_frame_dir || !cpu_jank_dir) {
		ret = -ENOMEM;
		goto remove_nodes;
	}

	if (!proc_create("ux_flag", 0666, binder_dir, &oplus_binder_ux_fops) ||
	    !proc_create("ux_task", 0666, sched_assist_dir, &oplus_ux_fops) ||
	    !proc_create("ux_task_app", 0666, sched_assist_dir,
			 &oplus_ux_app_fops) ||
	    !proc_create("im_flag", 0666, sched_assist_dir, &oplus_im_fops) ||
	    !proc_create("im_flag_app", 0666, sched_assist_dir,
			 &oplus_im_app_fops) ||
	    !proc_create("status", 0666, audio_dir, &audio_status_fops) ||
	    !proc_create("ctrl", 0666, frame_dir, &oplus_frame_fops) ||
	    !proc_create("sys_ctrl", 0666, frame_dir, &oplus_frame_sys_fops) ||
	    !proc_create("async_perf_ctrl", 0666, async_frame_dir,
			 &oplus_async_frame_fops) ||
	    !proc_create("clm_enable", 0660, cpu_jank_dir, &clm_enable_fops) ||
	    !proc_create("clm_mux_switch", 0660, cpu_jank_dir, &clm_mux_fops) ||
	    !proc_create("theiaPwkReport", 0660, NULL, &stage_report_fops) ||
	    !proc_create("phoenix", 0660, NULL, &phoenix_fops) ||
	    !proc_create("bootprof", 0660, NULL, &phoenix_fops) ||
	    !proc_create("shutdown_detect", 0660, NULL,
			 &shutdown_detect_fops)) {
		ret = -ENOMEM;
		goto remove_nodes;
	}

	fbg_sysctl_header = register_sysctl("fbg", fbg_sysctls);
	if (!fbg_sysctl_header) {
		ret = -ENOMEM;
		goto remove_nodes;
	}

	pr_info("ColorOS compatibility ABI backed by WALT is ready\n");
	return 0;

remove_nodes:
	remove_proc_subtree("oplus_binder", NULL);
	remove_proc_subtree("oplus_scheduler", NULL);
	remove_proc_subtree("oplus_frame_boost", NULL);
	remove_proc_subtree("oplus_frame_boost_async", NULL);
	remove_proc_subtree("jank_info", NULL);
	remove_proc_entry("theiaPwkReport", NULL);
	remove_proc_entry("phoenix", NULL);
	remove_proc_entry("bootprof", NULL);
	remove_proc_entry("shutdown_detect", NULL);
	misc_deregister(&sched_boost_misc);
free_page:
	free_page(sched_boost_page);
	sched_boost_page = 0;
	return ret;
}
late_initcall(oplus_coloros_compat_init);
