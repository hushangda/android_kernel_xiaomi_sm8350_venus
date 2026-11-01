// SPDX-License-Identifier: GPL-2.0
/* Xiaomi libxqos v2 ABI, backed by native task and dependency scheduling. */
#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/pid_namespace.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/security.h>
#include <linux/uaccess.h>
#include <linux/xr_qos.h>

struct xr_qos_request {
	__u32 qos;
	__s32 id;
};

#define XR_QOS_SET       _IOW('x', 7, struct xr_qos_request)
#define XR_QOS_GET_PID  _IOWR('x', 8, struct xr_qos_request)
#define XR_QOS_GET_TID  _IOWR('x', 9, struct xr_qos_request)
#define XR_QOS_ASYNC     _IOW('x', 11, struct xr_qos_request)

static bool xr_qos_admin(void)
{
	const struct cred *cred = current_cred();

	return uid_eq(cred->euid, GLOBAL_ROOT_UID) ||
		uid_eq(cred->euid, KUIDT_INIT(1000)) || capable(CAP_SYS_NICE);
}

static bool xr_qos_access(struct task_struct *p)
{
	return xr_qos_admin() || uid_eq(current_euid(), task_uid(p));
}

static long xr_qos_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct xr_qos_request request;
	struct task_struct *p, *thread;
	void __user *ptr = (void __user *)arg;
	int ret = 0;

	switch (cmd) {
	case XR_QOS_SET:
	case XR_QOS_GET_PID:
	case XR_QOS_GET_TID:
	case XR_QOS_ASYNC:
		break;
	default:
		return -ENOTTY;
	}
	if (copy_from_user(&request, ptr, sizeof(request)))
		return -EFAULT;
	if (request.id <= 0)
		return -EINVAL;
	rcu_read_lock();
	p = find_task_by_vpid(request.id);
	if (!p) {
		ret = -ESRCH;
	} else if (!xr_qos_access(p) || p->flags & PF_KTHREAD) {
		ret = -EPERM;
	} else {
		get_task_struct(p);
	}
	rcu_read_unlock();
	if (ret)
		return ret;

	switch (cmd) {
	case XR_QOS_SET:
	case XR_QOS_ASYNC:
		ret = security_task_setscheduler(p);
		if (ret)
			break;
		if (cmd == XR_QOS_SET) {
			/* The device must not grant unprivileged callers real-time policy. */
			if ((request.qos & 0xf) && ((request.qos >> 4) & 3) == 1 &&
			    !xr_qos_admin())
				ret = -EPERM;
			else
				ret = xr_qos_set(p, request.qos);
		}
		else if (request.qos > 1)
			ret = -EINVAL;
		else
			ret = xr_qos_async_arm(p, request.qos != 0);
		break;
	case XR_QOS_GET_PID:
		request.qos = 0;
		rcu_read_lock();
		for_each_thread(p, thread) {
			request.qos = max(request.qos, xr_qos_get(thread));
			if (request.qos == 10)
				break;
		}
		/* for_each_thread includes the leader, regardless of input TID. */
		rcu_read_unlock();
		if (copy_to_user(ptr, &request, sizeof(request)))
			ret = -EFAULT;
		break;
	case XR_QOS_GET_TID:
		request.qos = xr_qos_get(p);
		if (copy_to_user(ptr, &request, sizeof(request)))
			ret = -EFAULT;
		break;
	}
	put_task_struct(p);
	return ret;
}

static const struct file_operations xr_qos_fops = {
	.owner = THIS_MODULE,
	.open = nonseekable_open,
	.llseek = no_llseek,
	.unlocked_ioctl = xr_qos_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
};

static struct miscdevice xr_qos_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "xr_fbt",
	.fops = &xr_qos_fops,
	.mode = 0666,
};

static int __init xr_qos_init(void)
{
	BUILD_BUG_ON(sizeof(struct xr_qos_request) != 8);
	BUILD_BUG_ON(XR_QOS_SET != 0x40087807);
	BUILD_BUG_ON(XR_QOS_GET_PID != 0xc0087808);
	BUILD_BUG_ON(XR_QOS_GET_TID != 0xc0087809);
	BUILD_BUG_ON(XR_QOS_ASYNC != 0x4008780b);
	return misc_register(&xr_qos_device);
}
/* All seven hooks are built-in dependencies of CONFIG_XIAOMI_XR_QOS. */
device_initcall(xr_qos_init);
MODULE_LICENSE("GPL");
