// SPDX-License-Identifier: GPL-2.0-only
/*
 * Per-memcg transfer between the real ZRAM pool and its backing block device.
 * Included by zram_drv.c to keep slot and pool internals private to the driver.
 *
 * Lock order: device registry mutex -> init_lock -> slot -> memcg statistics.
 * No slot/statistics lock spans allocation, IO, or reclaim. An in-flight read
 * owns a backing-block reference; discarding its slot cannot recycle the block.
 */
static bool zram_group_matches(struct zram *zram, u32 index,
			       struct mem_cgroup *memcg)
{
	struct mem_cgroup *owner = zram->table[index].memcg;

	return owner && mem_cgroup_is_descendant(owner, memcg);
}

static int zram_group_bio(struct zram *zram, struct page *page,
			  unsigned long block, bool write)
{
	struct bio bio;
	struct bio_vec vec;
	int ret;

#ifdef CONFIG_XIAOMI_ZRAM_EXTM
	if (write) {
		ret = zram_extm_crypt(zram, page, block, true);
		if (ret)
			return ret;
	}
#endif
	bio_init(&bio, &vec, 1);
	bio_set_dev(&bio, zram->bdev);
	bio.bi_iter.bi_sector = block * (PAGE_SIZE >> 9);
	bio.bi_opf = (write ? REQ_OP_WRITE : REQ_OP_READ) | REQ_SYNC;
	if (bio_add_page(&bio, page, PAGE_SIZE, 0) != PAGE_SIZE)
		return -EIO;
#ifdef CONFIG_XIAOMI_ZRAM_EXTM
	ret = zram_extm_prepare_bio(zram, &bio, block, GFP_NOIO);
	if (ret) {
		bio_uninit(&bio);
		return ret;
	}
#endif
	ret = submit_bio_wait(&bio);
	bio_uninit(&bio);
#ifdef CONFIG_XIAOMI_ZRAM_EXTM
	if (!ret && !write)
		ret = zram_extm_crypt(zram, page, block, false);
#endif
	return ret;
}

/* Reserve before IO, so concurrent group operations cannot overspend quota. */
static int zram_group_reserve_quota(struct zram *zram)
{
	int reserved = 0;
	u64 pages = 1UL << (PAGE_SHIFT - 12);

	spin_lock(&zram->wb_limit_lock);
	if (zram->wb_limit_enable) {
		if (zram->bd_wb_limit < pages)
			reserved = -ENOSPC;
		else {
			zram->bd_wb_limit -= pages;
			reserved = 1;
		}
	}
	spin_unlock(&zram->wb_limit_lock);
	return reserved;
}

static void zram_group_release_quota(struct zram *zram, int reserved)
{
	if (reserved <= 0)
		return;
	spin_lock(&zram->wb_limit_lock);
	zram->bd_wb_limit += min_t(u64, U64_MAX - zram->bd_wb_limit,
				   1UL << (PAGE_SHIFT - 12));
	spin_unlock(&zram->wb_limit_lock);
}

/* Apply the ratio to this backend's real bytes, including a whole WB page. */
static bool zram_group_ratio_allows(struct mem_cgroup *memcg, u32 bytes, u32 ratio)
{
	struct mi_memcg_stats stats;
	u64 total, writeback, allowance;

	if (ratio == 100)
		return true;
	mi_memcg_stats(memcg, &stats);
	if (stats.compressed_bytes < bytes || stats.writeback_bytes > U64_MAX - PAGE_SIZE)
		return false;
	writeback = stats.writeback_bytes + PAGE_SIZE;
	if (stats.compressed_bytes - bytes > U64_MAX - writeback)
		return false;
	total = stats.compressed_bytes - bytes + writeback;
	allowance = total / 100 * ratio + total % 100 * ratio / 100;
	return writeback <= allowance;
}

/* Returns the actual compressed bytes removed, zero for a raced/skipped slot. */
static int zram_group_write_slot(struct zram *zram, struct mem_cgroup *memcg,
				 u32 index, struct page *page, u64 *done, u32 ratio)
{
	struct mem_cgroup *owner;
	unsigned long block;
	u32 bytes;
	int ret, reserved;

	*done = 0;
	zram_slot_lock(zram, index);
	if (!zram_group_matches(zram, index, memcg) ||
	    !zram_allocated(zram, index) ||
	    zram_test_flag(zram, index, ZRAM_WB) ||
	    zram_test_flag(zram, index, ZRAM_SAME) ||
	    zram_test_flag(zram, index, ZRAM_UNDER_WB) ||
	    !zram_group_ratio_allows(memcg, zram_get_obj_size(zram, index), ratio)) {
		zram_slot_unlock(zram, index);
		return 0;
	}
	ret = zram_read_from_zspool(zram, page, index);
	if (ret) {
		zram_slot_unlock(zram, index);
		return ret;
	}
	zram_set_flag(zram, index, ZRAM_UNDER_WB);
	zram_set_flag(zram, index, ZRAM_IDLE);
	zram_slot_unlock(zram, index);

	reserved = zram_group_reserve_quota(zram);
	if (reserved < 0) {
		ret = reserved;
		goto cancel;
	}
	block = alloc_block_bdev(zram);
	if (!block) {
		ret = -ENOSPC;
		goto release_quota;
	}
	ret = zram_group_bio(zram, page, block, true);
	if (ret)
		goto free_block;
	atomic64_inc(&zram->stats.bd_writes);

	zram_slot_lock(zram, index);
	if (!zram_allocated(zram, index) ||
	    !zram_test_flag(zram, index, ZRAM_IDLE)) {
		zram_slot_unlock(zram, index);
		goto free_block;
	}
	bytes = zram_get_obj_size(zram, index);
	owner = zram_memcg_preserve(zram, index);
	zram_free_page(zram, index);
	zram_clear_flag(zram, index, ZRAM_UNDER_WB);
	zram_set_flag(zram, index, ZRAM_WB);
	zram_set_element(zram, index, block);
	zram_memcg_restore(zram, index, owner, bytes, PAGE_SIZE, false, true);
	atomic64_inc(&zram->stats.pages_stored);
	zram_slot_unlock(zram, index);
	*done = bytes;
	return 0;

free_block:
	free_block_bdev(zram, block);
release_quota:
	/* Physical writes consume quota even when a concurrent access won. */
	if (ret)
		zram_group_release_quota(zram, reserved);
cancel:
	zram_slot_lock(zram, index);
	zram_clear_flag(zram, index, ZRAM_UNDER_WB);
	zram_clear_flag(zram, index, ZRAM_IDLE);
	zram_slot_unlock(zram, index);
	return ret;
}

static int zram_group_read_slot(struct zram *zram, struct mem_cgroup *memcg,
				u32 index, struct page *page, u64 *done)
{
	struct zram_wb_token token;
	struct bio_vec bvec = { .bv_page = page, .bv_len = PAGE_SIZE };
	int ret;

	*done = 0;
	zram_slot_lock(zram, index);
	if (!zram_group_matches(zram, index, memcg) ||
	    !zram_test_flag(zram, index, ZRAM_WB) ||
	    zram_test_flag(zram, index, ZRAM_UNDER_WB)) {
		zram_slot_unlock(zram, index);
		return 0;
	}
	token.block = zram_get_element(zram, index);
	atomic_inc(&zram->backing_refs[token.block]);
	zram_set_flag(zram, index, ZRAM_UNDER_WB);
	zram_set_flag(zram, index, ZRAM_IDLE);
	zram_slot_unlock(zram, index);

	ret = zram_group_bio(zram, page, token.block, false);
	if (!ret) {
		atomic64_inc(&zram->stats.bd_reads);
		ret = __zram_bvec_write(zram, &bvec, index, NULL, NULL, &token);
		if (!ret)
			*done = PAGE_SIZE;
	}
	/* Drop IO pin after the token check and possible slot ownership drop. */
	free_block_bdev(zram, token.block);
	zram_slot_lock(zram, index);
	zram_clear_flag(zram, index, ZRAM_UNDER_WB);
	zram_clear_flag(zram, index, ZRAM_IDLE);
	zram_slot_unlock(zram, index);
	return ret == -EAGAIN ? 0 : ret;
}

int mi_zram_group_io(struct mem_cgroup *memcg, u64 bytes, bool write,
		     u64 *done, u32 ratio, bool (*cancelled)(void *), void *context)
{
	struct zram *zram;
	struct page *page;
	unsigned long index, nr_pages;
	u64 transferred;
	int id, ret = 0;
	bool found = false;

	*done = 0;
	if (ratio > 100)
		return -EINVAL;
	if (!bytes)
		return 0;
	page = alloc_page(GFP_NOIO | __GFP_HIGHMEM);
	if (!page)
		return -ENOMEM;
	mutex_lock(&zram_index_mutex);
	idr_for_each_entry(&zram_index_idr, zram, id) {
		down_read(&zram->init_lock);
		if (!init_done(zram) || !zram->group_enabled || !zram->backing_dev)
			goto next;
		found = true;
		nr_pages = zram->disksize >> PAGE_SHIFT;
		for (index = 0; index < nr_pages && *done < bytes; index++) {
			if (cancelled && cancelled(context)) {
				ret = -EINTR;
				break;
			}
			ret = write ? zram_group_write_slot(zram, memcg, index, page, &transferred, ratio) :
				zram_group_read_slot(zram, memcg, index, page, &transferred);
			*done += transferred;
			if (ret)
				break;
			cond_resched();
		}
next:
		up_read(&zram->init_lock);
		if (ret || *done >= bytes)
			break;
	}
	mutex_unlock(&zram_index_mutex);
	__free_page(page);
	return !found && !ret ? -ENODEV : ret;
}
EXPORT_SYMBOL_GPL(mi_zram_group_io);

/* Fault IO owns its storage block from slot lookup until bio completion. */
struct zram_group_read_work {
	struct work_struct work;
	struct zram *zram;
	struct bio *completion;
	struct page *page;
	unsigned long block;
	int result;
};

static void zram_group_read_workfn(struct work_struct *work)
{
	struct zram_group_read_work *io = container_of(work, struct zram_group_read_work, work);
	int ret;

	atomic64_inc(&io->zram->stats.bd_reads);
	ret = zram_group_bio(io->zram, io->page, io->block, false);
	free_block_bdev(io->zram, io->block);
	if (io->completion) {
		io->completion->bi_status = errno_to_blk_status(ret);
		bio_endio(io->completion);
		kfree(io);
	} else {
		/* Synchronous partial-page read: caller owns the stack work/page. */
		io->result = ret;
	}
}

static int zram_group_read_async(struct zram *zram, struct bio_vec *bvec,
				unsigned long block, struct bio *parent, bool sync)
{
	struct zram_group_read_work *io;
	struct bio *completion;

	if (sync) {
		struct zram_group_read_work work = {
			.zram = zram, .page = bvec->bv_page, .block = block,
		};

		/* Avoid nested submit_bio_wait from make_request's bio recursion. */
		INIT_WORK_ONSTACK(&work.work, zram_group_read_workfn);
		queue_work(zram->group_read_wq, &work.work);
		flush_work(&work.work);
		destroy_work_on_stack(&work.work);
		return work.result;
	}
	io = kmalloc(sizeof(*io), GFP_ATOMIC);
	if (!io)
		goto fail;
	completion = bio_alloc(GFP_ATOMIC, 1);
	if (!completion) {
		kfree(io);
		goto fail;
	}
	if (bio_add_page(completion, bvec->bv_page, bvec->bv_len, bvec->bv_offset) != bvec->bv_len) {
		bio_put(completion);
		kfree(io);
		goto fail;
	}
	completion->bi_opf = REQ_OP_READ;
	if (parent)
		bio_chain(completion, parent);
	else
		completion->bi_end_io = zram_page_end_io;
	io->zram = zram;
	io->completion = completion;
	io->page = bvec->bv_page;
	io->block = block;
	INIT_WORK(&io->work, zram_group_read_workfn);
	queue_work(zram->group_read_wq, &io->work);
	return 1;
fail:
	free_block_bdev(zram, block);
	return -ENOMEM;
}
