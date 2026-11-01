// SPDX-License-Identifier: GPL-2.0-only
/*
 * Framework -> MIMD2 event FIFO. The vendor consumer watches IN_CLOSE_WRITE,
 * opens/reads/closes this attribute once per event and drains until EOF.
 * This transports policy requests; policy execution remains in the daemon.
 */
#include <linux/kobject.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>

#define MIMD_EVENT_SIZE 40
#define MIMD_QUEUE_SIZE 128
struct mimd_event {
	char text[MIMD_EVENT_SIZE];
	u8 length;
};
static struct mimd_event mimd_events[MIMD_QUEUE_SIZE];
static DEFINE_SPINLOCK(mimd_lock);
static unsigned int mimd_head, mimd_count;
static u64 mimd_accepted, mimd_consumed, mimd_full;
static bool mimd_legacy_pending;
static unsigned int mimd_legacy_value;
static u64 mimd_legacy_accepted, mimd_legacy_consumed, mimd_legacy_full;
static struct kobject *mimd_module, *mimd_kobj;
static s64 mimd_dump_value;

/* Actual daemon log records, independent of the event transport. */
#define MIMD_LOG_SIZE 128
#define MIMD_LOG_COUNT 2048
struct mimd_log_record {
	char text[MIMD_LOG_SIZE];
	struct timespec64 time;
};
static struct mimd_log_record mimd_log[MIMD_LOG_COUNT];
static DEFINE_MUTEX(mimd_log_lock);
static unsigned int mimd_log_head, mimd_log_count;
static struct proc_dir_entry *mimd_log_proc;

static ssize_t mimdlog_write(struct file *file, const char __user *userbuf,
			     size_t count, loff_t *pos)
{
	struct mimd_log_record record = { };

	if (!count || count >= MIMD_LOG_SIZE)
		return -EINVAL;
	if (copy_from_user(record.text, userbuf, count))
		return -EFAULT;
	if (memchr(record.text, '\0', count))
		return -EINVAL;
	ktime_get_real_ts64(&record.time);
	mutex_lock(&mimd_log_lock);
	mimd_log[mimd_log_head] = record;
	mimd_log_head = (mimd_log_head + 1) % MIMD_LOG_COUNT;
	if (mimd_log_count < MIMD_LOG_COUNT)
		mimd_log_count++;
	mutex_unlock(&mimd_log_lock);
	return count;
}

static int mimdlog_show(struct seq_file *seq, void *unused)
{
	unsigned int i, start;
	struct rtc_time tm;

	mutex_lock(&mimd_log_lock);
	start = (mimd_log_head + MIMD_LOG_COUNT - mimd_log_count) % MIMD_LOG_COUNT;
	for (i = 0; i < mimd_log_count; i++) {
		struct mimd_log_record *record = &mimd_log[(start + i) % MIMD_LOG_COUNT];

		rtc_time64_to_tm(record->time.tv_sec, &tm);
		seq_printf(seq, "%d-%02d-%02d %02d:%02d:%02d UTC { %s }\n",
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec, record->text);
	}
	mutex_unlock(&mimd_log_lock);
	return 0;
}

static int mimdlog_open(struct inode *inode, struct file *file)
{
	return single_open(file, mimdlog_show, NULL);
}

static const struct file_operations mimdlog_fops = {
	.owner = THIS_MODULE,
	.open = mimdlog_open,
	.read = seq_read,
	.write = mimdlog_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int mimd_parse_event(const char *buf, size_t count, struct mimd_event *event)
{
	u32 values[3] = { 0 };
	unsigned int field = 0, digits = 0;
	size_t i, length = count;

	if (!length || length >= MIMD_EVENT_SIZE - 1)
		return -EINVAL;
	if (buf[length - 1] == '\n')
		length--;
	if (!length)
		return -EINVAL;
	for (i = 0; i < length; i++) {
		unsigned char c = buf[i];

		if (c == ':') {
			if (!digits || ++field == ARRAY_SIZE(values))
				return -EINVAL;
			digits = 0;
			continue;
		}
		if (c < '0' || c > '9' || values[field] > (INT_MAX - (c - '0')) / 10U)
			return -EINVAL;
		values[field] = values[field] * 10 + c - '0';
		digits++;
	}
	if (!digits || values[0] < 1 || values[0] > 6 ||
	    field != (values[0] == 5 ? 2 : 1))
		return -EINVAL;
	/* Misc events in the framework are sleep=1 and screen-off=2. */
	if (values[0] == 1 && (values[1] < 1 || values[1] > 2))
		return -EINVAL;
	event->length = length + 1;
	memcpy(event->text, buf, length);
	event->text[length] = '\n';
	event->text[length + 1] = '\0';
	return 0;
}

static ssize_t mimdtrigger_store(struct kobject *kobj, struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	struct mimd_event event = { 0 };
	int ret;

	ret = mimd_parse_event(buf, count, &event);
	if (ret)
		return ret;
	spin_lock(&mimd_lock);
	if (mimd_count == MIMD_QUEUE_SIZE) {
		mimd_full++;
		spin_unlock(&mimd_lock);
		return -ENOSPC;
	}
	mimd_events[(mimd_head + mimd_count) % MIMD_QUEUE_SIZE] = event;
	mimd_count++;
	mimd_accepted++;
	spin_unlock(&mimd_lock);
	/* VFS sends the consumer's IN_CLOSE_WRITE on the real writer's close. */
	return count;
}

static ssize_t mimdtrigger_show(struct kobject *kobj, struct kobj_attribute *attr,
			       char *buf)
{
	unsigned int length;

	spin_lock(&mimd_lock);
	if (!mimd_count) {
		spin_unlock(&mimd_lock);
		return 0;
	}
	length = mimd_events[mimd_head].length;
	memcpy(buf, mimd_events[mimd_head].text, length);
	mimd_head = (mimd_head + 1) % MIMD_QUEUE_SIZE;
	mimd_count--;
	mimd_consumed++;
	spin_unlock(&mimd_lock);
	return length;
}

static ssize_t stats_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	u64 accepted, consumed, full;
	unsigned int pending;

	spin_lock(&mimd_lock);
	accepted = mimd_accepted;
	consumed = mimd_consumed;
	full = mimd_full;
	pending = mimd_count;
	spin_unlock(&mimd_lock);
	return scnprintf(buf, PAGE_SIZE, "accepted %llu consumed %llu pending %u full %llu\n",
			 accepted, consumed, pending, full);
}

/* The old HIDL consumer parses atoi() and does not drain to EOF. Keep its
 * transport separate, with one outstanding event and explicit backpressure.
 * Never translate a MIMD2 event into a guessed legacy policy number.
 */
static ssize_t mimdlegacy_store(struct kobject *kobj, struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	unsigned int value = 0;
	size_t i, length = count;

	if (!length || length > 3)
		return -EINVAL;
	if (buf[length - 1] == '\n')
		length--;
	if (!length || length > 2)
		return -EINVAL;
	for (i = 0; i < length; i++) {
		if (buf[i] < '0' || buf[i] > '9')
			return -EINVAL;
		value = value * 10 + buf[i] - '0';
	}
	if (value > 19)
		return -EINVAL;
	spin_lock(&mimd_lock);
	if (mimd_legacy_pending) {
		mimd_legacy_full++;
		spin_unlock(&mimd_lock);
		return -ENOSPC;
	}
	mimd_legacy_value = value;
	mimd_legacy_pending = true;
	mimd_legacy_accepted++;
	spin_unlock(&mimd_lock);
	return count;
}

static ssize_t mimdlegacy_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	unsigned int value;

	spin_lock(&mimd_lock);
	if (!mimd_legacy_pending) {
		spin_unlock(&mimd_lock);
		return 0;
	}
	value = mimd_legacy_value;
	mimd_legacy_pending = false;
	mimd_legacy_consumed++;
	spin_unlock(&mimd_lock);
	return scnprintf(buf, PAGE_SIZE, "%u\n", value);
}

static ssize_t legacy_stats_show(struct kobject *kobj, struct kobj_attribute *attr,
				 char *buf)
{
	u64 accepted, consumed, full;
	bool pending;

	spin_lock(&mimd_lock);
	accepted = mimd_legacy_accepted;
	consumed = mimd_legacy_consumed;
	full = mimd_legacy_full;
	pending = mimd_legacy_pending;
	spin_unlock(&mimd_lock);
	return scnprintf(buf, PAGE_SIZE, "accepted %llu consumed %llu pending %u full %llu\n",
			 accepted, consumed, pending, full);
}

static ssize_t dump_set_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	s64 value;

	spin_lock(&mimd_lock);
	value = mimd_dump_value;
	spin_unlock(&mimd_lock);
	return scnprintf(buf, PAGE_SIZE, "%lld\n", value);
}

static ssize_t dump_set_store(struct kobject *kobj, struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	s64 value;
	int ret = kstrtoll(buf, 10, &value);

	if (ret)
		return ret;
	spin_lock(&mimd_lock);
	mimd_dump_value = value;
	spin_unlock(&mimd_lock);
	/* The daemon's real inotify watcher consumes the dump notification. */
	return count;
}

static struct kobj_attribute mimdtrigger_attr = __ATTR(mimdtrigger, 0660,
						      mimdtrigger_show, mimdtrigger_store);
static struct kobj_attribute mimd_stats_attr = __ATTR_RO(stats);
static struct kobj_attribute mimdlegacy_attr = __ATTR(mimdlegacy, 0660,
						     mimdlegacy_show, mimdlegacy_store);
static struct kobj_attribute mimd_legacy_stats_attr = __ATTR_RO(legacy_stats);
static struct kobj_attribute mimd_dump_attr = __ATTR(dump_set, 0660,
						   dump_set_show, dump_set_store);
static struct attribute *mimd_attrs[] = {
	&mimdtrigger_attr.attr, &mimd_stats_attr.attr,
	&mimdlegacy_attr.attr, &mimd_legacy_stats_attr.attr, &mimd_dump_attr.attr, NULL,
};
static const struct attribute_group mimd_group = { .attrs = mimd_attrs };

static void mimd_exit(void)
{
	proc_remove(mimd_log_proc);
	mimd_log_proc = NULL;
	if (mimd_kobj) {
		sysfs_remove_group(mimd_kobj, &mimd_group);
		kobject_put(mimd_kobj);
		mimd_kobj = NULL;
	}
	kobject_put(mimd_module);
	mimd_module = NULL;
}

static int __init mimd_init(void)
{
	int ret;

	mimd_module = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!mimd_module)
		mimd_module = kobject_create_and_add(KBUILD_MODNAME, &module_kset->kobj);
	if (!mimd_module)
		return -ENOMEM;
	mimd_kobj = kobject_create_and_add("mimd", mimd_module);
	if (!mimd_kobj) {
		mimd_exit();
		return -ENOMEM;
	}
	ret = sysfs_create_group(mimd_kobj, &mimd_group);
	if (ret) {
		kobject_put(mimd_kobj);
		mimd_kobj = NULL;
		mimd_exit();
		return ret;
	}
	mimd_log_proc = proc_create("mimdlog", 0660, NULL, &mimdlog_fops);
	if (!mimd_log_proc) {
		mimd_exit();
		return -ENOMEM;
	}
	return 0;
}
