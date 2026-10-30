// SPDX-License-Identifier: GPL-2.0-only
/* Xiaomi Venus SLA v3 traffic-statistics ABI, recovered from stock sla.ko. */
#include <linux/bitmap.h>
#include <linux/etherdevice.h>
#include <linux/init.h>
#include <linux/ip.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netfilter_ipv4.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/tcp.h>
#include <linux/timekeeping.h>
#include <linux/udp.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <net/ip.h>
#include <net/net_namespace.h>
#include <net/sock.h>

#define SLA_BUFFER_SIZE 0x5000
#define SLA_MAGIC 0x686a7379
#define SLA_RECORD_COUNT 1279

struct sla_record {
	u64 time_ms;
	u32 size;
	u16 local_port;
	u8 direction;
	u8 request;
};

struct sla_buffer {
	u32 magic;
	u32 count;
	u32 next;
	u32 reserved;
	struct sla_record records[SLA_RECORD_COUNT];
};

struct sla_iface {
	struct list_head list;
	struct kref ref;
	struct proc_dir_entry *mmap_entry;
	struct proc_dir_entry *stats_entry;
	struct sla_buffer *buffer;
	u64 rx_bytes;
	char name[IFNAMSIZ];
};

static LIST_HEAD(sla_ifaces);
static DEFINE_SPINLOCK(sla_lock);
static DEFINE_MUTEX(sla_config_lock);
static DECLARE_BITMAP(sla_requests, 65536);
static struct proc_dir_entry *sla_dir;
static bool sla_enabled;
static bool sla_rate_on;
static u16 sla_ports[127] = { 80, 443 };
static u16 sla_max_size = 4096;
static u16 sla_limit_time = 5;
static bool sla_voip_enabled;
static u32 sla_voip_uid;
static __be32 sla_voip_address;
static __be16 sla_voip_port;

static void sla_iface_free(struct kref *ref)
{
	struct sla_iface *iface = container_of(ref, struct sla_iface, ref);

	vfree(iface->buffer);
	kfree(iface);
}

static int sla_stats_open(struct inode *inode, struct file *file)
{
	struct sla_iface *iface = PDE_DATA(inode);

	kref_get(&iface->ref);
	file->private_data = iface;
	return 0;
}

static int sla_stats_release(struct inode *inode, struct file *file)
{
	struct sla_iface *iface = file->private_data;

	kref_put(&iface->ref, sla_iface_free);
	return 0;
}

static ssize_t sla_stats_read(struct file *file, char __user *buf,
			      size_t count, loff_t *pos)
{
	struct sla_iface *iface = file->private_data;

	return simple_read_from_buffer(buf, count, pos, iface->buffer,
				       SLA_BUFFER_SIZE);
}

static void sla_vma_open(struct vm_area_struct *vma)
{
	struct sla_iface *iface = vma->vm_private_data;

	__module_get(THIS_MODULE);
	kref_get(&iface->ref);
}

static void sla_vma_close(struct vm_area_struct *vma)
{
	struct sla_iface *iface = vma->vm_private_data;

	kref_put(&iface->ref, sla_iface_free);
	module_put(THIS_MODULE);
}

static const struct vm_operations_struct sla_vm_ops = {
	.open = sla_vma_open,
	.close = sla_vma_close,
};

static int sla_stats_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct sla_iface *iface = file->private_data;
	int ret;

	if (vma->vm_pgoff || vma->vm_end - vma->vm_start > SLA_BUFFER_SIZE)
		return -EINVAL;
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
	ret = remap_vmalloc_range(vma, iface->buffer, 0);
	if (ret) {
		module_put(THIS_MODULE);
		return ret;
	}
	/* proc_remove() calls ->release even while this VMA is still mapped. */
	vma->vm_private_data = iface;
	vma->vm_ops = &sla_vm_ops;
	kref_get(&iface->ref);
	return 0;
}

static const struct file_operations sla_mmap_fops = {
	.owner = THIS_MODULE,
	.open = sla_stats_open,
	.release = sla_stats_release,
	.read = sla_stats_read,
	.mmap = sla_stats_mmap,
	.llseek = default_llseek,
};

static ssize_t sla_total_read(struct file *file, char __user *buf,
			      size_t count, loff_t *pos)
{
	struct sla_iface *iface = file->private_data;
	unsigned long flags;
	char text[32];
	u64 bytes;
	int length;

	spin_lock_irqsave(&sla_lock, flags);
	bytes = iface->rx_bytes;
	spin_unlock_irqrestore(&sla_lock, flags);
	length = scnprintf(text, sizeof(text), "%llu\n", bytes);
	return simple_read_from_buffer(buf, count, pos, text, length);
}

static ssize_t sla_total_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *pos)
{
	struct sla_iface *iface = file->private_data;
	unsigned long flags;
	u64 value;
	int ret;

	ret = kstrtoull_from_user(buf, count, 0, &value);
	if (ret)
		return ret;
	spin_lock_irqsave(&sla_lock, flags);
	iface->rx_bytes = value;
	spin_unlock_irqrestore(&sla_lock, flags);
	return count;
}

static const struct file_operations sla_total_fops = {
	.owner = THIS_MODULE,
	.open = sla_stats_open,
	.release = sla_stats_release,
	.read = sla_total_read,
	.write = sla_total_write,
	.llseek = default_llseek,
};

/* Called with sla_config_lock held. Existing mappings retain their buffers. */
static void sla_disable(void)
{
	struct sla_iface *iface, *next;
	unsigned long flags;
	LIST_HEAD(removed);

	spin_lock_irqsave(&sla_lock, flags);
	sla_enabled = false;
	sla_rate_on = false;
	bitmap_zero(sla_requests, 65536);
	list_splice_init(&sla_ifaces, &removed);
	spin_unlock_irqrestore(&sla_lock, flags);
	list_for_each_entry_safe(iface, next, &removed, list) {
		proc_remove(iface->mmap_entry);
		proc_remove(iface->stats_entry);
		list_del(&iface->list);
		kref_put(&iface->ref, sla_iface_free);
	}
}

static int sla_add_iface(const char *name)
{
	struct sla_iface *iface;
	unsigned long flags;
	char entry[IFNAMSIZ + 6];

	if (!dev_valid_name(name))
		return -EINVAL;
	list_for_each_entry(iface, &sla_ifaces, list)
		if (!strcmp(iface->name, name))
			return 0;
	iface = kzalloc(sizeof(*iface), GFP_KERNEL);
	if (!iface)
		return -ENOMEM;
	kref_init(&iface->ref);
	strscpy(iface->name, name, sizeof(iface->name));
	iface->buffer = vmalloc_user(SLA_BUFFER_SIZE);
	if (!iface->buffer) {
		kfree(iface);
		return -ENOMEM;
	}
	iface->buffer->magic = SLA_MAGIC;
	iface->buffer->count = SLA_RECORD_COUNT;
	snprintf(entry, sizeof(entry), "%s_mmap", name);
	iface->mmap_entry = proc_create_data(entry, 0644, sla_dir,
					     &sla_mmap_fops, iface);
	if (!iface->mmap_entry)
		goto free_iface;
	snprintf(entry, sizeof(entry), "%s_stats", name);
	iface->stats_entry = proc_create_data(entry, 0644, sla_dir,
					      &sla_total_fops, iface);
	if (!iface->stats_entry) {
		proc_remove(iface->mmap_entry);
		goto free_iface;
	}
	spin_lock_irqsave(&sla_lock, flags);
	list_add_tail(&iface->list, &sla_ifaces);
	spin_unlock_irqrestore(&sla_lock, flags);
	return 0;

free_iface:
	kref_put(&iface->ref, sla_iface_free);
	return -ENOMEM;
}

static int sla_config_show(struct seq_file *seq, void *unused)
{
	int i;

	mutex_lock(&sla_config_lock);
	seq_printf(seq, "enable=%d\nrate_on=%d\nports=", sla_enabled,
		   sla_rate_on);
	for (i = 0; i < ARRAY_SIZE(sla_ports) && sla_ports[i]; i++)
		seq_printf(seq, "%s%u", i ? "," : "", sla_ports[i]);
	seq_printf(seq, "\nmax_size=%u\nlimit_time=%u\nvoip_enable=%d\nvoip_uid=%u\n",
		   sla_max_size, sla_limit_time, sla_voip_enabled, sla_voip_uid);
	mutex_unlock(&sla_config_lock);
	return 0;
}

static int sla_config_open(struct inode *inode, struct file *file)
{
	return single_open(file, sla_config_show, NULL);
}

static ssize_t sla_config_write(struct file *file, const char __user *buf,
				size_t count, loff_t *pos)
{
	struct sla_iface *iface;
	unsigned long flags;
	char *text, *command, *value, *item;
	u16 ports[ARRAY_SIZE(sla_ports)] = { 0 };
	u32 number;
	int ret = 0, i = 0;

	if (!count || count > 780)
		return -EINVAL;
	text = memdup_user_nul(buf, count);
	if (IS_ERR(text))
		return PTR_ERR(text);
	command = strim(text);
	value = strchr(command, '=');
	if (value)
		*value++ = '\0';
	mutex_lock(&sla_config_lock);
	if (!strcmp(command, "iface") && value) {
		ret = sla_add_iface(value);
	} else if (!strcmp(command, "ports") && value) {
		while ((item = strsep(&value, ",")) && i < ARRAY_SIZE(ports)) {
			ret = kstrtou16(item, 10, &ports[i]);
			if (ret || !ports[i]) {
				ret = -EINVAL;
				break;
			}
			i++;
		}
		if (!ret)
			memcpy(sla_ports, ports, sizeof(ports));
	} else if (!strcmp(command, "hash_clear")) {
		spin_lock_irqsave(&sla_lock, flags);
		bitmap_zero(sla_requests, 65536);
		spin_unlock_irqrestore(&sla_lock, flags);
	} else if (!strcmp(command, "hash_print")) {
		pr_debug("sla: %u outstanding local ports\n",
			 bitmap_weight(sla_requests, 65536));
	} else if (!value || kstrtou32(value, 10, &number)) {
		ret = -EINVAL;
	} else if (!strcmp(command, "enable") && number <= 1) {
		if (!number) {
			sla_disable();
		} else if (!sla_enabled) {
			spin_lock_irqsave(&sla_lock, flags);
			bitmap_zero(sla_requests, 65536);
			sla_rate_on = true;
			sla_enabled = true;
			spin_unlock_irqrestore(&sla_lock, flags);
		}
	} else if (!strcmp(command, "rate_on") && number <= 1) {
		WRITE_ONCE(sla_rate_on, number);
	} else if (!strcmp(command, "reset_traffic") && number == 1) {
		spin_lock_irqsave(&sla_lock, flags);
		list_for_each_entry(iface, &sla_ifaces, list)
			iface->rx_bytes = 0;
		spin_unlock_irqrestore(&sla_lock, flags);
	} else if (!strcmp(command, "max_size") &&
		   number >= 4096 && number <= U16_MAX) {
		WRITE_ONCE(sla_max_size, number);
	} else if (!strcmp(command, "limit_time") &&
		   number >= 5 && number <= U16_MAX) {
		WRITE_ONCE(sla_limit_time, number);
	} else if (!strcmp(command, "voip_enable") && number <= 1) {
		spin_lock_irqsave(&sla_lock, flags);
		sla_voip_address = 0;
		sla_voip_port = 0;
		sla_voip_enabled = number;
		spin_unlock_irqrestore(&sla_lock, flags);
	} else if (!strcmp(command, "voip_uid")) {
		WRITE_ONCE(sla_voip_uid, number);
	} else {
		ret = -EINVAL;
	}
	mutex_unlock(&sla_config_lock);
	kfree(text);
	return ret ? ret : count;
}

static const struct file_operations sla_config_fops = {
	.owner = THIS_MODULE,
	.open = sla_config_open,
	.read = seq_read,
	.write = sla_config_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static ssize_t sla_voip_read(struct file *file, char __user *buf,
			     size_t count, loff_t *pos)
{
	unsigned long flags;
	char text[32];
	int length;

	spin_lock_irqsave(&sla_lock, flags);
	length = scnprintf(text, sizeof(text), "%u,%d",
			   (__force u32)sla_voip_address,
			   (__force u16)sla_voip_port);
	sla_voip_address = 0;
	sla_voip_port = 0;
	spin_unlock_irqrestore(&sla_lock, flags);
	return simple_read_from_buffer(buf, count, pos, text, length);
}

static const struct file_operations sla_voip_fops = {
	.owner = THIS_MODULE,
	.read = sla_voip_read,
	.llseek = no_llseek,
};

static unsigned int sla_packet(void *priv, struct sk_buff *skb,
			       const struct nf_hook_state *state)
{
	struct iphdr ip_buffer, *ip;
	struct tcphdr tcp_buffer, *tcp;
	struct udphdr udp_buffer, *udp;
	struct sla_iface *iface;
	struct sla_record record, *previous;
	struct net_device *dev;
	struct sock *sk;
	unsigned long flags;
	unsigned int ip_length, tcp_length, port, index;
	bool outgoing = state->hook == NF_INET_POST_ROUTING;
	int network_offset;

	if (!READ_ONCE(sla_enabled))
		return NF_ACCEPT;
	network_offset = skb_network_offset(skb);
	ip = skb_header_pointer(skb, network_offset, sizeof(ip_buffer), &ip_buffer);
	if (!ip || ip->version != 4 || ip->ihl < 5 || ip_is_fragment(ip))
		return NF_ACCEPT;
	ip_length = ip->ihl * 4;
	if (outgoing && READ_ONCE(sla_voip_enabled) &&
	    READ_ONCE(sla_voip_uid) && ip->protocol == IPPROTO_UDP) {
		sk = skb_to_full_sk(skb);
		udp = skb_header_pointer(skb, network_offset + ip_length,
					 sizeof(udp_buffer), &udp_buffer);
		if (sk && udp && ntohs(udp->dest) != 53 &&
		    __kuid_val(sock_i_uid(sk)) == READ_ONCE(sla_voip_uid)) {
			spin_lock_irqsave(&sla_lock, flags);
			if (!sla_voip_address) {
				sla_voip_address = ip->daddr;
				sla_voip_port = udp->dest;
			}
			spin_unlock_irqrestore(&sla_lock, flags);
		}
		return NF_ACCEPT;
	}
	if (!READ_ONCE(sla_rate_on) || ip->protocol != IPPROTO_TCP)
		return NF_ACCEPT;
	tcp = skb_header_pointer(skb, network_offset + ip_length,
				 sizeof(tcp_buffer), &tcp_buffer);
	if (!tcp || tcp->doff < 5)
		return NF_ACCEPT;
	tcp_length = tcp->doff * 4;
	port = ntohs(outgoing ? tcp->source : tcp->dest);
	spin_lock_irqsave(&sla_lock, flags);
	if (ntohs(ip->tot_len) <= ip_length + tcp_length) {
		if (tcp->fin || tcp->rst)
			clear_bit(port, sla_requests);
		goto unlock;
	}
	dev = outgoing ? state->out : state->in;
	if (!dev)
		goto unlock;
	list_for_each_entry(iface, &sla_ifaces, list) {
		if (strcmp(iface->name, dev->name))
			continue;
		/* The buffer is writable by userspace: never trust its cursor. */
		index = READ_ONCE(iface->buffer->next);
		if (index >= SLA_RECORD_COUNT)
			index = 0;
		record.time_ms = div_u64(ktime_get_real_ns(), NSEC_PER_MSEC);
		record.size = skb->len + ETH_HLEN;
		record.local_port = port;
		record.direction = outgoing ? 1 : 2;
		if (outgoing) {
			set_bit(port, sla_requests);
			record.request = 1;
		} else {
			iface->rx_bytes += skb->len;
			record.request = test_and_clear_bit(port, sla_requests);
			previous = &iface->buffer->records[index ? index - 1 :
							 SLA_RECORD_COUNT - 1];
			if (!record.request && !previous->request &&
			    previous->local_port == port &&
			    record.time_ms >= previous->time_ms &&
			    record.time_ms - previous->time_ms <= sla_limit_time &&
			    previous->size <= sla_max_size &&
			    record.size <= sla_max_size - previous->size) {
				previous->size += record.size;
				break;
			}
		}
		iface->buffer->records[index] = record;
		/* Publish each complete record before advancing the mmap cursor. */
		smp_wmb();
		WRITE_ONCE(iface->buffer->next, (index + 1) % SLA_RECORD_COUNT);
		break;
	}
unlock:
	spin_unlock_irqrestore(&sla_lock, flags);
	return NF_ACCEPT;
}

static const struct nf_hook_ops sla_hooks[] = {
	{ .hook = sla_packet, .pf = NFPROTO_IPV4,
	  .hooknum = NF_INET_PRE_ROUTING, .priority = -198 },
	{ .hook = sla_packet, .pf = NFPROTO_IPV4,
	  .hooknum = NF_INET_POST_ROUTING, .priority = INT_MAX },
};

static int __init mi_sla_init(void)
{
	int ret;

	BUILD_BUG_ON(sizeof(struct sla_record) != 16);
	BUILD_BUG_ON(sizeof(struct sla_buffer) != SLA_BUFFER_SIZE);
	sla_dir = proc_mkdir("sla", NULL);
	if (!sla_dir)
		return -ENOMEM;
	if (!proc_create("config", 0644, sla_dir, &sla_config_fops) ||
	    !proc_create("sla_voip_state", 0644, sla_dir, &sla_voip_fops)) {
		ret = -ENOMEM;
		goto remove_proc;
	}
	ret = nf_register_net_hooks(&init_net, sla_hooks, ARRAY_SIZE(sla_hooks));
	if (!ret)
		return 0;
remove_proc:
	proc_remove(sla_dir);
	return ret;
}
module_init(mi_sla_init);

static void __exit mi_sla_exit(void)
{
	nf_unregister_net_hooks(&init_net, sla_hooks, ARRAY_SIZE(sla_hooks));
	mutex_lock(&sla_config_lock);
	sla_disable();
	mutex_unlock(&sla_config_lock);
	proc_remove(sla_dir);
}
module_exit(mi_sla_exit);

MODULE_DESCRIPTION("Xiaomi SLA v3 traffic statistics");
MODULE_LICENSE("GPL v2");
