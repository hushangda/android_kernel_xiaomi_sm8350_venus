// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi xlog record transport, reconstructed from the Venus stock module.
 * The userspace ABI is /dev/xlog with 512-byte records and a 1 MiB FIFO.
 */
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/xlog.h>

#define XLOG_BUFFER_SIZE SZ_1M
#define XLOG_RECORD_SIZE 512

static struct {
	struct cdev cdev;
	dev_t devt;
	struct class *class;
	char *buffer;
	size_t read_pos;
	size_t write_pos;
	size_t used;
} xlog;
static DEFINE_MUTEX(xlog_lock);
static DECLARE_WAIT_QUEUE_HEAD(xlog_readq);

static bool xlog_valid_count(size_t count)
{
	return count <= XLOG_BUFFER_SIZE && !(count % XLOG_RECORD_SIZE);
}

static ssize_t xlog_write_record(const char *buffer, size_t count)
{
	size_t first;
	ssize_t ret = count;

	if (!xlog_valid_count(count))
		return -EBADMSG;
	if (!count)
		return 0;

	mutex_lock(&xlog_lock);
	if (!xlog.buffer) {
		ret = -ENODEV;
		goto out;
	}
	if (count > XLOG_BUFFER_SIZE - xlog.used) {
		ret = -EIO;
		goto out;
	}
	first = min(count, XLOG_BUFFER_SIZE - xlog.write_pos);
	memcpy(xlog.buffer + xlog.write_pos, buffer, first);
	memcpy(xlog.buffer, buffer + first, count - first);
	xlog.write_pos = (xlog.write_pos + count) % XLOG_BUFFER_SIZE;
	xlog.used += count;
out:
	mutex_unlock(&xlog_lock);
	if (ret > 0)
		wake_up_interruptible(&xlog_readq);
	return ret;
}

ssize_t xlogchar_kwrite(const char *buffer, size_t count)
{
	if (!buffer && count)
		return -EINVAL;
	return xlog_write_record(buffer, count);
}
EXPORT_SYMBOL(xlogchar_kwrite);

static ssize_t xlogchar_write(struct file *file, const char __user *buffer,
			     size_t count, loff_t *offset)
{
	char *record;
	ssize_t ret;

	if (!xlog_valid_count(count))
		return -EBADMSG;
	if (!count)
		return 0;
	record = kvmalloc(count, GFP_KERNEL);
	if (!record)
		return -ENOMEM;
	if (copy_from_user(record, buffer, count))
		ret = -EFAULT;
	else
		ret = xlog_write_record(record, count);
	kvfree(record);
	return ret;
}

static ssize_t xlogchar_read(struct file *file, char __user *buffer,
			    size_t count, loff_t *offset)
{
	size_t first;
	ssize_t ret;

	if (!xlog_valid_count(count))
		return -EBADMSG;
	if (!count)
		return 0;

	for (;;) {
		ret = mutex_lock_interruptible(&xlog_lock);
		if (ret)
			return ret;
		if (xlog.used >= count)
			break;
		mutex_unlock(&xlog_lock);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(xlog_readq,
					      READ_ONCE(xlog.used) >= count);
		if (ret)
			return ret;
	}

	first = min(count, XLOG_BUFFER_SIZE - xlog.read_pos);
	if (copy_to_user(buffer, xlog.buffer + xlog.read_pos, first) ||
	    copy_to_user(buffer + first, xlog.buffer, count - first)) {
		ret = -EFAULT;
	} else {
		xlog.read_pos = (xlog.read_pos + count) % XLOG_BUFFER_SIZE;
		xlog.used -= count;
		ret = count;
	}
	mutex_unlock(&xlog_lock);
	if (ret > 0)
		wake_up_interruptible(&xlog_readq);
	return ret;
}

static __poll_t xlogchar_poll(struct file *file, poll_table *wait)
{
	__poll_t mask = 0;
	size_t used;

	poll_wait(file, &xlog_readq, wait);
	used = READ_ONCE(xlog.used);
	if (used >= XLOG_RECORD_SIZE)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (XLOG_BUFFER_SIZE - used >= XLOG_RECORD_SIZE)
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}

static const struct file_operations xlogchar_fops = {
	.owner = THIS_MODULE,
	.open = nonseekable_open,
	.read = xlogchar_read,
	.write = xlogchar_write,
	.poll = xlogchar_poll,
	.llseek = no_llseek,
};

static int __init xlogchar_init(void)
{
	struct device *device;
	int ret;

	xlog.buffer = kvzalloc(XLOG_BUFFER_SIZE, GFP_KERNEL);
	if (!xlog.buffer)
		return -ENOMEM;
	ret = alloc_chrdev_region(&xlog.devt, 0, 1, "xlog");
	if (ret)
		goto free_buffer;
	cdev_init(&xlog.cdev, &xlogchar_fops);
	xlog.cdev.owner = THIS_MODULE;
	ret = cdev_add(&xlog.cdev, xlog.devt, 1);
	if (ret)
		goto unregister_region;
	xlog.class = class_create(THIS_MODULE, "xlog");
	if (IS_ERR(xlog.class)) {
		ret = PTR_ERR(xlog.class);
		goto delete_cdev;
	}
	device = device_create(xlog.class, NULL, xlog.devt, NULL, "xlog");
	if (IS_ERR(device)) {
		ret = PTR_ERR(device);
		goto destroy_class;
	}
	return 0;

destroy_class:
	class_destroy(xlog.class);
delete_cdev:
	cdev_del(&xlog.cdev);
unregister_region:
	unregister_chrdev_region(xlog.devt, 1);
free_buffer:
	kvfree(xlog.buffer);
	xlog.buffer = NULL;
	return ret;
}
module_init(xlogchar_init);

static void __exit xlogchar_exit(void)
{
	device_destroy(xlog.class, xlog.devt);
	class_destroy(xlog.class);
	cdev_del(&xlog.cdev);
	unregister_chrdev_region(xlog.devt, 1);
	kvfree(xlog.buffer);
}
module_exit(xlogchar_exit);

MODULE_DESCRIPTION("Xiaomi xlog record transport");
MODULE_LICENSE("GPL v2");
