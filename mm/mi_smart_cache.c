// SPDX-License-Identifier: GPL-2.0-only
/*
 * Venus SmartCache: fd-level policy ABI from X-Ring's GPL-2.0 mctrl/smart_cache
 * (430688007431c0c69a3c5782c1766b3aee66c8cd), implemented with native 5.4
 * charging, LRU isolation and memcg accounting. No inode/page KABI bits used.
 */
#include <linux/cgroup.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/hugetlb.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/magic.h>
#include <linux/memcontrol.h>
#include <linux/mi_memcg.h>
#include <linux/mi_smart_cache.h>
#include <linux/mutex.h>
#include <linux/pagemap.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/sysfs.h>

#define SC_LEVELS 3
#define SC_MAX_TAGS 4096
#define SC_BATCH 32
#define SC_MAX_COMMANDS 64

struct sc_tag {
	struct hlist_node node;
	struct rcu_head rcu;
	struct inode *inode;
	u32 level;
};

static DEFINE_HASHTABLE(sc_tags, 10);
/* Leaf lock: tags, slots and policy. No page/CSS allocation or waits inside. */
static DEFINE_SPINLOCK(sc_lock);
/* Serialize sysfs/cgroup policy transactions; never taken by reclaim/offline. */
static DEFINE_MUTEX(sc_update_lock);
static struct mem_cgroup *sc_groups[SC_LEVELS];
static unsigned int sc_nr_tags;
static unsigned int sc_ratio;
static bool sc_enabled;
static atomic64_t sc_moved = ATOMIC64_INIT(0);
static atomic64_t sc_failed = ATOMIC64_INIT(0);

static struct sc_tag *sc_find_locked(struct inode *inode)
{
	struct sc_tag *tag;

	hash_for_each_possible(sc_tags, tag, node, (unsigned long)inode)
		if (tag->inode == inode)
			return tag;
	return NULL;
}

static unsigned int sc_inode_level(struct inode *inode)
{
	struct sc_tag *tag;
	unsigned int level = 0;

	rcu_read_lock();
	hash_for_each_possible_rcu(sc_tags, tag, node, (unsigned long)inode) {
		if (tag->inode == inode) {
			level = READ_ONCE(tag->level);
			break;
		}
	}
	rcu_read_unlock();
	return level;
}

void mi_smart_cache_inode_free(struct inode *inode)
{
	struct sc_tag *tag;
	unsigned long flags;

	spin_lock_irqsave(&sc_lock, flags);
	tag = sc_find_locked(inode);
	if (tag) {
		hash_del_rcu(&tag->node);
		sc_nr_tags--;
	}
	spin_unlock_irqrestore(&sc_lock, flags);
	if (tag)
		kfree_rcu(tag, rcu);
}

void mi_smart_cache_offline(struct mem_cgroup *memcg)
{
	unsigned long flags;
	u32 level;

	spin_lock_irqsave(&sc_lock, flags);
	level = mi_memcg_data(memcg)->sc_level;
	if (level && sc_groups[level - 1] == memcg)
		sc_groups[level - 1] = NULL;
	WRITE_ONCE(mi_memcg_data(memcg)->sc_level, 0);
	WRITE_ONCE(mi_memcg_data(memcg)->sc_dying, true);
	spin_unlock_irqrestore(&sc_lock, flags);
}

static struct mem_cgroup *sc_get_group(unsigned int level)
{
	struct mem_cgroup *memcg = NULL;
	unsigned long flags;

	if (!level || level > SC_LEVELS)
		return NULL;
	spin_lock_irqsave(&sc_lock, flags);
	memcg = sc_groups[level - 1];
	if (memcg && !css_tryget_online(&memcg->css))
		memcg = NULL;
	spin_unlock_irqrestore(&sc_lock, flags);
	return memcg;
}

struct mem_cgroup *mi_smart_cache_charge_target(struct page *page)
{
	struct address_space *mapping;

	if (!READ_ONCE(sc_enabled) || !PageLocked(page) || PageAnon(page) ||
	    PageSwapBacked(page) || PageHuge(page))
		return NULL;
	mapping = page_mapping(page);
	if (!mapping || !mapping->host || !S_ISREG(mapping->host->i_mode))
		return NULL;
	return sc_get_group(sc_inode_level(mapping->host));
}

/* Level 1 gets first claim on the global budget, followed by levels 2 and 3. */
static unsigned long sc_budget(unsigned long total, unsigned int ratio,
			       unsigned long *usage, unsigned long *max,
			       unsigned int level)
{
	unsigned long remaining = (u64)total * ratio / 100;
	unsigned long limit;
	unsigned int i;

	for (i = 0; i < level; i++) {
		limit = min(max[i], remaining);
		if (i == level - 1)
			return limit;
		remaining -= min(usage[i], limit);
	}
	return 0;
}

bool mi_smart_cache_protect(struct page *page, int priority)
{
	struct mem_cgroup *memcg;
	struct address_space *mapping;
	struct pglist_data *pgdat = page_pgdat(page);
	unsigned long usage[SC_LEVELS] = { 0 }, max[SC_LEVELS] = { 0 };
	unsigned long flags, budget;
	unsigned int level, i;
	bool protect = false;

	/* Ordinary cache remains reclaimable at urgent priorities (1 and 0). */
	if (!READ_ONCE(sc_enabled) || priority <= 1 || PageAnon(page) ||
	    PageSwapBacked(page))
		return false;
	mapping = page_mapping(page);
	if (!mapping || !mapping->host)
		return false;
	level = sc_inode_level(mapping->host);
	if (!level)
		return false;
	/* Page lock + LRU isolation, supplied by shrink_page_list, pins charge. */
	memcg = page->mem_cgroup;
	spin_lock_irqsave(&sc_lock, flags);
	if (!READ_ONCE(sc_enabled) || sc_groups[level - 1] != memcg)
		goto out;
	for (i = 0; i < SC_LEVELS; i++) {
		struct lruvec *lruvec;

		if (!sc_groups[i])
			continue;
		lruvec = mem_cgroup_lruvec(sc_groups[i], pgdat);
		usage[i] = lruvec_page_state(lruvec, NR_ACTIVE_FILE) +
			lruvec_page_state(lruvec, NR_INACTIVE_FILE);
		max[i] = mi_memcg_data(sc_groups[i])->sc_max_pages;
	}
	budget = sc_budget(node_page_state(pgdat, NR_ACTIVE_FILE) +
			   node_page_state(pgdat, NR_INACTIVE_FILE),
			   sc_ratio, usage, max, level);
	/* Over-budget groups are reclaimable until their real charge fits. */
	protect = budget && usage[level - 1] <= budget;
out:
	spin_unlock_irqrestore(&sc_lock, flags);
	return protect;
}

static int sc_tag_inode(struct inode *inode, unsigned int level)
{
	struct sc_tag *tag, *new = NULL;
	unsigned long flags;
	int ret = 0;

	if (level) {
		new = kzalloc(sizeof(*new), GFP_KERNEL);
		if (!new)
			return -ENOMEM;
		new->inode = inode;
		new->level = level;
	}
	spin_lock_irqsave(&sc_lock, flags);
	tag = sc_find_locked(inode);
	if (!level) {
		if (tag) {
			hash_del_rcu(&tag->node);
			sc_nr_tags--;
		}
	} else if (tag) {
		WRITE_ONCE(tag->level, level);
	} else if (sc_nr_tags == SC_MAX_TAGS) {
		ret = -ENOSPC;
	} else {
		hash_add_rcu(sc_tags, &new->node, (unsigned long)inode);
		sc_nr_tags++;
		new = NULL;
	}
	spin_unlock_irqrestore(&sc_lock, flags);
	if (!level && tag)
		kfree_rcu(tag, rcu);
	kfree(new);
	return ret;
}

static bool sc_page_in_cache_group(struct page *page)
{
	struct mem_cgroup *memcg;
	bool result = false;

	rcu_read_lock();
	memcg = page_memcg_rcu(page);
	if (memcg)
		result = READ_ONCE(mi_memcg_data(memcg)->sc_level) != 0;
	rcu_read_unlock();
	return result;
}

static int sc_migrate_mapping(struct address_space *mapping,
			      struct mem_cgroup *to, bool untag)
{
	struct mem_cgroup *source = NULL;
	struct page *pages[SC_BATCH];
	pgoff_t index = 0;
	unsigned long deadline = jiffies + msecs_to_jiffies(5000);
	unsigned int nr, i;
	int ret = 0;

	lru_add_drain_all();
	do {
		nr = find_get_pages_range(mapping, &index, (pgoff_t)-1,
					  SC_BATCH, pages);
		for (i = 0; i < nr; i++) {
			struct page *page = pages[i];
			int err;

			if (fatal_signal_pending(current)) {
				ret = -EINTR;
				break;
			}
			if (time_after_eq(jiffies, deadline)) {
				ret = -ETIMEDOUT;
				break;
			}
			/* The scan starts at zero; each THP is moved at its head. */
			if (PageTail(page) || page_mapping(page) != mapping ||
			    (untag && !sc_page_in_cache_group(page)))
				goto put;
			err = mi_smart_cache_move_page(page, to, &source);
			if (err < 0) {
				atomic64_inc(&sc_failed);
				if (!ret)
					ret = err;
			} else if (err > 0) {
				atomic64_add(err, &sc_moved);
			}
put:
			put_page(pages[i]);
		}
		if (i < nr) {
			while (i < nr)
				put_page(pages[i++]);
			break;
		}
		cond_resched();
	} while (nr && index && index != (pgoff_t)-1);
	mi_smart_cache_move_end(&source);
	return ret;
}

struct sc_command {
	int fd;
	unsigned int level;
	struct file *file;
};

static int sc_parse_command(char *token, struct sc_command *command)
{
	char *dash = strchr(token, '-');
	char *p;
	unsigned int fd, level;

	if (!dash || dash == token || strchr(dash + 1, '-') ||
	    token[0] == '+' || dash[1] == '+')
		return -EINVAL;
	*dash++ = '\0';
	for (p = token; *p; p++)
		if (*p < '0' || *p > '9')
			return -EINVAL;
	for (p = dash; *p; p++)
		if (*p < '0' || *p > '9')
			return -EINVAL;
	if (kstrtouint(token, 10, &fd) || fd > INT_MAX ||
	    kstrtouint(dash, 10, &level) || level > SC_LEVELS)
		return -EINVAL;
	command->fd = fd;
	command->level = level;
	return 0;
}

static struct file *sc_get_file(int fd)
{
	struct file *file = fget(fd);
	unsigned int depth;

	if (!file)
		return ERR_PTR(-EBADF);
	for (depth = 0; file_inode(file)->i_sb->s_magic == OVERLAYFS_SUPER_MAGIC;
	     depth++) {
		struct file *real;

		if (depth == 4) {
			fput(file);
			return ERR_PTR(-ELOOP);
		}
		real = ovl_smart_cache_real_file(file);
		fput(file);
		if (IS_ERR(real))
			return real;
		file = real;
	}
	if (!S_ISREG(file_inode(file)->i_mode) ||
	    shmem_mapping(file->f_mapping) || is_file_hugepages(file) ||
	    IS_DAX(file_inode(file))) {
		fput(file);
		return ERR_PTR(-EOPNOTSUPP);
	}
	return file;
}

static ssize_t file_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	struct sc_command *commands;
	char *input, *cursor, *token;
	unsigned int n = 0, i;
	int ret = 0;

	if (!count || count >= PAGE_SIZE || memchr(buf, '\0', count))
		return -EINVAL;
	input = kstrndup(buf, count, GFP_KERNEL);
	commands = kcalloc(SC_MAX_COMMANDS, sizeof(*commands), GFP_KERNEL);
	if (!input || !commands) {
		ret = -ENOMEM;
		goto free;
	}
	cursor = strim(input);
	/* Parse and pin every descriptor before changing any tag or charge. */
	while ((token = strsep(&cursor, ","))) {
		if (n == SC_MAX_COMMANDS || !*token) {
			ret = -EINVAL;
			goto put;
		}
		ret = sc_parse_command(token, &commands[n]);
		if (ret)
			goto put;
		commands[n].file = sc_get_file(commands[n].fd);
		if (IS_ERR(commands[n].file)) {
			ret = PTR_ERR(commands[n].file);
			goto put;
		}
		n++;
	}
	mutex_lock(&sc_update_lock);
	for (i = 0; i < n; i++) {
		struct mem_cgroup *to;
		struct address_space *mapping = commands[i].file->f_mapping;
		unsigned int level = commands[i].level;

		to = level ? sc_get_group(level) : root_mem_cgroup;
		if (!to) {
			ret = -ENODEV;
			break;
		}
		ret = sc_tag_inode(mapping->host, level);
		if (!ret && (!level || READ_ONCE(sc_enabled)))
			ret = sc_migrate_mapping(mapping, to, !level);
		if (!ret && level && READ_ONCE(mi_memcg_data(to)->sc_dying))
			ret = -ENODEV;
		if (level)
			css_put(&to->css);
		if (ret)
			break;
	}
	mutex_unlock(&sc_update_lock);
put:
	for (i = 0; i < n; i++)
		fput(commands[i].file);
free:
	kfree(commands);
	kfree(input);
	return ret ? ret : (ssize_t)count;
}

static ssize_t enable_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", READ_ONCE(sc_enabled));
}

static ssize_t enable_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 10, &val) || val > 1)
		return -EINVAL;
	mutex_lock(&sc_update_lock);
	WRITE_ONCE(sc_enabled, val);
	mutex_unlock(&sc_update_lock);
	return count;
}

static ssize_t ratio_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", READ_ONCE(sc_ratio));
}

static ssize_t ratio_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	unsigned int val;
	unsigned long flags;

	if (kstrtouint(buf, 10, &val) || val > 100)
		return -EINVAL;
	spin_lock_irqsave(&sc_lock, flags);
	sc_ratio = val;
	spin_unlock_irqrestore(&sc_lock, flags);
	return count;
}

static ssize_t stat_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	return scnprintf(buf, PAGE_SIZE,
		"tags %u\nmigration_pages %lld\nmigration_failures %lld\n",
		READ_ONCE(sc_nr_tags), atomic64_read(&sc_moved),
		atomic64_read(&sc_failed));
}

static struct kobj_attribute file_attr = __ATTR_WO(file);
static struct kobj_attribute enable_attr = __ATTR_RW(enable);
static struct kobj_attribute ratio_attr = __ATTR_RW(ratio);
static struct kobj_attribute stat_attr = __ATTR_RO(stat);
static struct attribute *sc_attrs[] = {
	&file_attr.attr, &enable_attr.attr, &ratio_attr.attr, &stat_attr.attr, NULL
};
static const struct attribute_group sc_attr_group = { .attrs = sc_attrs };

static u64 sc_level_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	return READ_ONCE(mi_memcg_data(mem_cgroup_from_css(css))->sc_level);
}

static int sc_level_write(struct cgroup_subsys_state *css, struct cftype *cft,
			  u64 val)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);
	struct mi_memcg *data = mi_memcg_data(memcg);
	unsigned long flags;
	unsigned int i;
	int ret = 0;

	if (val > SC_LEVELS || (val && mem_cgroup_is_root(memcg)))
		return -EINVAL;
	mutex_lock(&sc_update_lock);
	spin_lock_irqsave(&sc_lock, flags);
	if (!READ_ONCE(data->online) || data->sc_dying) {
		ret = -ENODEV;
		goto out;
	}
	if (val) {
		if (sc_groups[val - 1] && sc_groups[val - 1] != memcg) {
			ret = -EBUSY;
			goto out;
		}
		/* Avoid hierarchical double counting between protection groups. */
		for (i = 0; i < SC_LEVELS; i++) {
			if (!sc_groups[i] || sc_groups[i] == memcg)
				continue;
			if (mem_cgroup_is_descendant(memcg, sc_groups[i]) ||
			    mem_cgroup_is_descendant(sc_groups[i], memcg)) {
				ret = -EINVAL;
				goto out;
			}
		}
	}
	if (data->sc_level)
		sc_groups[data->sc_level - 1] = NULL;
	WRITE_ONCE(data->sc_level, val);
	if (val)
		sc_groups[val - 1] = memcg;
out:
	spin_unlock_irqrestore(&sc_lock, flags);
	mutex_unlock(&sc_update_lock);
	return ret;
}

static u64 sc_max_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	return (u64)READ_ONCE(mi_memcg_data(mem_cgroup_from_css(css))->sc_max_pages)
		<< PAGE_SHIFT;
}

static int sc_max_write(struct cgroup_subsys_state *css, struct cftype *cft,
			u64 val)
{
	unsigned long flags;

	if (mem_cgroup_is_root(mem_cgroup_from_css(css)) ||
	    (val & (PAGE_SIZE - 1)) || (val >> PAGE_SHIFT) > ULONG_MAX)
		return -EINVAL;
	spin_lock_irqsave(&sc_lock, flags);
	WRITE_ONCE(mi_memcg_data(mem_cgroup_from_css(css))->sc_max_pages,
		   val >> PAGE_SHIFT);
	spin_unlock_irqrestore(&sc_lock, flags);
	return 0;
}

static struct cftype sc_files[] = {
	{ .name = "mctrl.level",
	  .read_u64 = sc_level_read, .write_u64 = sc_level_write },
	{ .name = "mctrl.max",
	  .read_u64 = sc_max_read, .write_u64 = sc_max_write },
	{ }
};

static int __init mi_smart_cache_init(void)
{
	struct cftype *dfl_files;
	struct kobject *kobj;
	int ret;

	if (mem_cgroup_disabled())
		return 0;
	dfl_files = kmemdup(sc_files, sizeof(sc_files), GFP_KERNEL);
	if (!dfl_files)
		return -ENOMEM;
	kobj = kobject_create_and_add("smart_cache", kernel_kobj);
	if (!kobj) {
		ret = -ENOMEM;
		goto free;
	}
	ret = cgroup_add_legacy_cftypes(&memory_cgrp_subsys, sc_files);
	if (ret)
		goto put;
	ret = cgroup_add_dfl_cftypes(&memory_cgrp_subsys, dfl_files);
	if (ret)
		goto legacy;
	ret = sysfs_create_group(kobj, &sc_attr_group);
	if (!ret)
		return 0;
	cgroup_rm_cftypes(dfl_files);
legacy:
	cgroup_rm_cftypes(sc_files);
put:
	kobject_put(kobj);
free:
	kfree(dfl_files);
	return ret;
}
late_initcall(mi_smart_cache_init);
