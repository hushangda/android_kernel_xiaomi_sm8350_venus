// SPDX-License-Identifier: GPL-2.0-only
/*
 * Native Xiaomi/X-Ring zram-control extent and encryption ABI.
 * Protocol: X-Ring 2024 zgroup_wb.c (GPL-2.0-only), not private OEM layouts.
 * The driver uses its existing page-sized writeback format behind this ABI.
 */
#include <crypto/skcipher.h>
#include <linux/blk-crypto.h>
#include <linux/bio-crypt-ctx.h>
#include <linux/compat.h>
#include <linux/key.h>
#include <linux/key-type.h>
#include <linux/miscdevice.h>
#include <linux/random.h>
#include <linux/sort.h>
#include <linux/zram_extm.h>
#include <uapi/linux/fscrypt.h>

#define ZRAM_EXTM_MAX_TARGETS 16384U
#define ZRAM_EXTM_SEG_SIZE (32UL << 10)
struct zram_extm {
	struct zram_extm_target *targets;
	u64 *ends;
	struct crypto_skcipher *cipher;
	struct blk_crypto_key *key;
	bool key_started;
	struct zram_extm_bdev info;
};

static int zram_extm_compare(const void *a, const void *b)
{
	const struct zram_extm_target *left = a, *right = b;

	return (left->physical > right->physical) - (left->physical < right->physical);
}

static int zram_extm_validate(struct zram_extm *extm, u64 capacity)
{
	struct zram_extm_bdev *info = &extm->info;
	struct zram_extm_target *sorted;
	u64 total = 0, previous = 0;
	unsigned int i;
	int ret = -EINVAL;

	if (!info->nr_targets || info->nr_targets > ZRAM_EXTM_MAX_TARGETS ||
	    !info->bdev_size || info->bdev_size % ZRAM_EXTM_SEG_SIZE)
		return -EINVAL;
	sorted = kvmalloc_array(info->nr_targets, sizeof(*sorted), GFP_KERNEL);
	if (!sorted)
		return -ENOMEM;
	for (i = 0; i < info->nr_targets; i++) {
		struct zram_extm_target *target = &extm->targets[i];

		if (!target->length || target->physical % ZRAM_EXTM_SEG_SIZE ||
		    target->length % ZRAM_EXTM_SEG_SIZE || target->physical > capacity ||
		    target->length > capacity - target->physical ||
		    target->length > U64_MAX - total)
			goto out;
		total += target->length;
		extm->ends[i] = total;
		sorted[i] = *target;
	}
	if (total != info->bdev_size)
		goto out;
	sort(sorted, info->nr_targets, sizeof(*sorted), zram_extm_compare, NULL);
	for (i = 0; i < info->nr_targets; i++) {
		if (sorted[i].physical < previous)
			goto out;
		previous = sorted[i].physical + sorted[i].length;
	}
	ret = 0;
out:
	kvfree(sorted);
	return ret;
}

static sector_t zram_extm_sector(struct zram *zram, unsigned long block)
{
	struct zram_extm *extm = zram->extm;
	u64 offset = (u64)block << PAGE_SHIFT, start;
	unsigned int low = 0, high;

	if (!extm)
		return (sector_t)block * (PAGE_SIZE >> 9);
	high = extm->info.nr_targets;
	while (low < high) {
		unsigned int mid = low + (high - low) / 2;

		if (offset < extm->ends[mid])
			high = mid;
		else
			low = mid + 1;
	}
	if (WARN_ON_ONCE(low == extm->info.nr_targets))
		return (sector_t)-1;
	start = low ? extm->ends[low - 1] : 0;
	return (extm->targets[low].physical + offset - start) >> 9;
}

static void zram_extm_free(struct zram_extm *extm, struct block_device *bdev)
{
	if (!extm)
		return;
	if (extm->key) {
		if (extm->key_started)
			blk_crypto_evict_key(bdev_get_queue(bdev), extm->key);
		kzfree(extm->key);
	}
	if (extm->cipher)
		crypto_free_skcipher(extm->cipher);
	kvfree(extm->targets);
	kvfree(extm->ends);
	kfree(extm);
}

static void zram_extm_reset(struct zram *zram)
{
	zram_extm_free(zram->extm, zram->bdev);
	zram->extm = NULL;
}

static int zram_extm_key(struct zram_extm *extm, struct block_device *bdev)
{
	const struct fscrypt_provisioning_key_payload *payload;
	key_ref_t ref;
	struct key *key;
	u8 raw[BLK_CRYPTO_MAX_WRAPPED_KEY_SIZE] = { 0 };
	u32 type = (extm->info.crypto_info >> 16) & 0xff;
	u32 mode = extm->info.crypto_info >> 24;
	size_t size;
	int ret = -EKEYREJECTED;

	/* ABI constants proven by current system_perf_init: standard=1, wrapped=2. */
	if ((type != 1 && type != 2) || mode != BLK_ENCRYPTION_MODE_AES_256_XTS ||
	    !extm->info.key_id)
		return -EINVAL;
	ref = lookup_user_key(extm->info.key_id, 0, KEY_NEED_SEARCH);
	if (IS_ERR(ref))
		return PTR_ERR(ref);
	key = key_ref_to_ptr(ref);
	down_read(&key->sem);
	if (key_validate(key) || strcmp(key->type->name, "fscrypt-provisioning") ||
	    key->datalen < sizeof(*payload) ||
	    key->datalen > sizeof(*payload) + sizeof(raw))
		goto unlock;
	payload = key->payload.data[0];
	if (!payload || payload->type != FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER || payload->__reserved)
		goto unlock;
	size = key->datalen - sizeof(*payload);
	memcpy(raw, payload->raw, size);
	ret = 0;
unlock:
	up_read(&key->sem);
	key_ref_put(ref);
	if (ret)
		goto wipe;
	extm->key = kzalloc(sizeof(*extm->key), GFP_KERNEL);
	if (!extm->key) {
		ret = -ENOMEM;
		goto wipe;
	}
	ret = blk_crypto_init_key(extm->key, raw, size, type == 2,
				 BLK_ENCRYPTION_MODE_AES_256_XTS, 8, PAGE_SIZE);
	if (ret)
		goto wipe;
	ret = blk_crypto_start_using_mode(BLK_ENCRYPTION_MODE_AES_256_XTS, 8,
					 PAGE_SIZE, type == 2, bdev_get_queue(bdev));
	if (!ret)
		extm->key_started = true;
wipe:
	memzero_explicit(raw, sizeof(raw));
	return ret;
}

static int zram_extm_crypto_init(struct zram_extm *extm, struct block_device *bdev)
{
	u32 info = extm->info.crypto_info;
	u8 raw[64];
	int ret;

	if (info & ~0xffff0003U || (!(info & 1) && (info & 2)))
		return -EINVAL;
	if (!(info & 1))
		return info || extm->info.key_id ? -EINVAL : 0;
	if (info & 2)
		return zram_extm_key(extm, bdev);
	if (info != 1 || extm->info.key_id)
		return -EINVAL;
	extm->cipher = crypto_alloc_skcipher("xts(aes)", 0, 0);
	if (IS_ERR(extm->cipher)) {
		ret = PTR_ERR(extm->cipher);
		extm->cipher = NULL;
		return ret;
	}
	get_random_bytes(raw, sizeof(raw));
	ret = crypto_skcipher_setkey(extm->cipher, raw, sizeof(raw));
	memzero_explicit(raw, sizeof(raw));
	return ret;
}

static int zram_extm_prepare_bio(struct zram *zram, struct bio *bio,
				unsigned long block, gfp_t gfp)
{
	struct bio_crypt_ctx *ctx;

	bio->bi_iter.bi_sector = zram_extm_sector(zram, block);
	if (bio->bi_iter.bi_sector == (sector_t)-1)
		return -EIO;
	if (!zram->extm || !zram->extm->key)
		return 0;
	ctx = bio_crypt_alloc_ctx(gfp);
	if (!ctx)
		return -ENOMEM;
	ctx->bc_key = zram->extm->key;
	memset(ctx->bc_dun, 0, sizeof(ctx->bc_dun));
	ctx->bc_dun[0] = bio->bi_iter.bi_sector >> (PAGE_SHIFT - 9);
	ctx->bc_ksm = NULL;
	ctx->bc_keyslot = -1;
	bio->bi_crypt_context = ctx;
	return 0;
}

static int zram_extm_crypt(struct zram *zram, struct page *page,
			   unsigned long block, bool write)
{
	struct skcipher_request *request;
	struct scatterlist sg;
	u8 iv[16] = { 0 };
	__le64 number;
	DECLARE_CRYPTO_WAIT(wait);
	int ret;

	if (!zram->extm || !zram->extm->cipher)
		return 0;
	number = cpu_to_le64(zram_extm_sector(zram, block) >> (PAGE_SHIFT - 9));
	memcpy(iv, &number, sizeof(number));
	request = skcipher_request_alloc(zram->extm->cipher, GFP_NOIO);
	if (!request)
		return -ENOMEM;
	sg_init_table(&sg, 1);
	sg_set_page(&sg, page, PAGE_SIZE, 0);
	skcipher_request_set_callback(request, CRYPTO_TFM_REQ_MAY_BACKLOG |
		CRYPTO_TFM_REQ_MAY_SLEEP, crypto_req_done, &wait);
	skcipher_request_set_crypt(request, &sg, &sg, PAGE_SIZE, iv);
	ret = crypto_wait_req(write ? crypto_skcipher_encrypt(request) :
				     crypto_skcipher_decrypt(request), &wait);
	skcipher_request_free(request);
	return ret;
}

static int zram_extm_set(struct zram *zram, const struct zram_extm_bdev *info,
			 struct zram_extm_target __user *targets)
{
	struct zram_extm *extm;
	struct file *file = NULL;
	struct block_device *bdev = NULL;
	unsigned long *bitmap = NULL, nr_pages;
	atomic_t *refs = NULL;
	int ret = -EINVAL;

	if (info->reserved1 || info->reserved2 || !memchr(info->bdev_path, 0, sizeof(info->bdev_path)) ||
	    !info->nr_targets || info->nr_targets > ZRAM_EXTM_MAX_TARGETS)
		return -EINVAL;
	extm = kzalloc(sizeof(*extm), GFP_KERNEL);
	if (!extm)
		return -ENOMEM;
	extm->info = *info;
	extm->targets = kvmalloc_array(info->nr_targets, sizeof(*extm->targets), GFP_KERNEL);
	extm->ends = kvmalloc_array(info->nr_targets, sizeof(*extm->ends), GFP_KERNEL);
	if (!extm->targets || !extm->ends) {
		ret = -ENOMEM;
		goto out;
	}
	if (copy_from_user(extm->targets, targets, info->nr_targets * sizeof(*targets))) {
		ret = -EFAULT;
		goto out;
	}
	file = filp_open(info->bdev_path, O_RDWR | O_LARGEFILE, 0);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		file = NULL;
		goto out;
	}
	if (!S_ISBLK(file_inode(file)->i_mode)) {
		ret = -ENOTBLK;
		goto out;
	}
	/* Extents belong to an allocated userspace swap file on a mounted bdev. */
	bdev = blkdev_get_by_dev(file_inode(file)->i_rdev, FMODE_READ | FMODE_WRITE, NULL);
	if (IS_ERR(bdev)) {
		ret = PTR_ERR(bdev);
		bdev = NULL;
		goto out;
	}
	ret = zram_extm_validate(extm, i_size_read(bdev->bd_inode));
	if (ret)
		goto out;
	if (bdev->bd_disk == zram->disk || info->bdev_size >> PAGE_SHIFT > ULONG_MAX) {
		ret = -EINVAL;
		goto out;
	}
	ret = zram_extm_crypto_init(extm, bdev);
	if (ret)
		goto out;
	nr_pages = info->bdev_size >> PAGE_SHIFT;
	bitmap = kvcalloc(BITS_TO_LONGS(nr_pages), sizeof(*bitmap), GFP_KERNEL);
	refs = kvcalloc(nr_pages, sizeof(*refs), GFP_KERNEL);
	if (!bitmap || !refs) {
		ret = -ENOMEM;
		goto out;
	}
	down_write(&zram->init_lock);
	if (init_done(zram) || !zram->group_enabled) {
		ret = init_done(zram) ? -EBUSY : -EPERM;
		up_write(&zram->init_lock);
		goto out;
	}
	reset_bdev(zram);
	zram->extm = extm;
	zram->bdev = bdev;
	zram->backing_dev = file;
	zram->bitmap = bitmap;
	zram->backing_refs = refs;
	zram->nr_pages = nr_pages;
	zram->disk->queue->backing_dev_info->capabilities &= ~BDI_CAP_SYNCHRONOUS_IO;
	up_write(&zram->init_lock);
	return 0;
out:
	zram_extm_free(extm, bdev);
	kvfree(bitmap);
	kvfree(refs);
	if (bdev)
		blkdev_put(bdev, FMODE_READ | FMODE_WRITE);
	if (file)
		filp_close(file, NULL);
	return ret;
}

static long zram_extm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct zram_extm_bdev __user *user = (void __user *)arg;
	struct zram_extm_bdev info;
	struct zram *zram;
	unsigned int count;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (cmd != ZRAM_EXTM_SET_BDEV && cmd != ZRAM_EXTM_GET_BDEV)
		return -ENOTTY;
	if (copy_from_user(&info, user, sizeof(info)))
		return -EFAULT;
	mutex_lock(&zram_index_mutex);
	zram = idr_find(&zram_index_idr, info.dev_id);
	if (!zram) {
		ret = -ENODEV;
		goto out;
	}
	if (cmd == ZRAM_EXTM_SET_BDEV) {
		ret = zram_extm_set(zram, &info, user->targets);
		goto out;
	}
	down_read(&zram->init_lock);
	if (!zram->extm || info.nr_targets > zram->extm->info.nr_targets) {
		ret = -EINVAL;
		goto unlock;
	}
	count = info.nr_targets;
	/* GET returns the total target count, device identity and active crypto
	 * flags. The native ABI leaves the caller's other header fields intact;
	 * in particular it does not return SET's key type/mode or stored key ID.
	 */
	info.nr_targets = zram->extm->info.nr_targets;
	info.crypto_info = zram->extm->info.crypto_info & 3;
	memset(info.bdev_path, 0, sizeof(info.bdev_path));
	scnprintf(info.bdev_path, sizeof(info.bdev_path), "dev=%u:%u",
		  MAJOR(zram->bdev->bd_dev), MINOR(zram->bdev->bd_dev));
	ret = copy_to_user(user->targets, zram->extm->targets, count * sizeof(*user->targets)) ||
	      copy_to_user(user, &info, sizeof(info)) ? -EFAULT : 0;
unlock:
	up_read(&zram->init_lock);
out:
	mutex_unlock(&zram_index_mutex);
	return ret;
}

static const struct file_operations zram_extm_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = zram_extm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
};
static struct miscdevice zram_extm_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "zram-control",
	.fops = &zram_extm_fops,
	.mode = 0600,
};
