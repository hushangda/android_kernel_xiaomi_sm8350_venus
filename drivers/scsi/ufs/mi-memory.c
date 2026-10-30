// SPDX-License-Identifier: GPL-2.0-only
/* Xiaomi memory interfaces reconstructed from the stock Venus mi_memory.ko. */
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/memblock.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/soc/qcom/smem.h>
#include <asm/unaligned.h>
#include <scsi/scsi_device.h>
#include "ufshcd.h"
#include "mi-memory.h"

struct mi_memory {
	struct ufs_hba *hba;
	struct class *class;
	struct device *device;
	struct proc_dir_entry *proc;
	struct mutex hr_lock;
	int major;
};

/* Cache while memblock metadata is still available, including NOMAP memory. */
static unsigned int mi_ddr_size;

static int __init mi_memory_cache_ddr_size(void)
{
	unsigned int gb = (memblock_phys_mem_size() +
			   memblock_reserved_size()) >> 30;

	if (gb < 2)
		mi_ddr_size = 0;
	else if (gb <= 4)
		mi_ddr_size = gb;
	else if (gb <= 12)
		mi_ddr_size = (gb + 1) & ~1;
	else if (gb == 17 || gb == 18)
		mi_ddr_size = 18;
	else
		mi_ddr_size = 16;
	return 0;
}
core_initcall(mi_memory_cache_ddr_size);

static int mi_ddr_id(void)
{
	size_t size;
	const u8 *info = qcom_smem_get(QCOM_SMEM_HOST_ANY, 135, &size);

	if (IS_ERR(info))
		return PTR_ERR(info);
	if (size < 0x45)
		return -EINVAL;
	return info[0x44];
}

static const char *mi_ddr_vendor(unsigned int id)
{
	switch (id) {
	case 1: return "SAMSUNG";
	case 3: return "ELPIDA";
	case 5: return "NANYA";
	case 6: return "HYNIX";
	case 14: return "INTEL";
	case 255: return "MICRON";
	default: return "UNKNOWN";
	}
}

static ssize_t ddr_id_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	int id = mi_ddr_id();

	return id < 0 ? id : scnprintf(buf, PAGE_SIZE, "0x%02x\n", id);
}
static DEVICE_ATTR_RO(ddr_id);

static ssize_t ddr_size_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", mi_ddr_size);
}
static DEVICE_ATTR_RO(ddr_size);

static ssize_t ddr_vendor_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	int id = mi_ddr_id();

	return id < 0 ? id : scnprintf(buf, PAGE_SIZE, "%s\n", mi_ddr_vendor(id));
}
static DEVICE_ATTR_RO(ddr_vendor);

static int mi_read_desc(struct ufs_hba *hba, enum desc_idn id, u8 *buf, int size)
{
	int ret = pm_runtime_get_sync(hba->dev);

	if (ret < 0) {
		pm_runtime_put_noidle(hba->dev);
		return ret;
	}
	ret = ufshcd_read_desc_param(hba, id, 0, 0, buf, size);
	pm_runtime_put_sync(hba->dev);
	if (!ret && !buf[0])
		return -ENODATA;
	return ret;
}

struct mi_desc_field {
	const char *name;
	u8 offset;
	u8 width;
};

static const struct mi_desc_field mi_device_fields[] = {
	{ "bLength", 0x00, 1 },
	{ "bDescriptorType", 0x01, 1 },
	{ "bDevice", 0x02, 1 },
	{ "bDeviceClass", 0x03, 1 },
	{ "bDeviceSubClass", 0x04, 1 },
	{ "bProtocol", 0x05, 1 },
	{ "bNumberLU", 0x06, 1 },
	{ "bNumberWLU", 0x07, 1 },
	{ "bBootEnable", 0x08, 1 },
	{ "bDescrAccessEn", 0x09, 1 },
	{ "bInitPowerMode", 0x0a, 1 },
	{ "bHighPriorityLUN", 0x0b, 1 },
	{ "bSecureRemovalType", 0x0c, 1 },
	{ "bSecurityLU", 0x0d, 1 },
	{ "Reserved", 0x0e, 1 },
	{ "bInitActiveICCLevel", 0x0f, 1 },
	{ "wSpecVersion", 0x10, 2 },
	{ "wManufactureDate", 0x12, 2 },
	{ "iManufactureName", 0x14, 1 },
	{ "iProductName", 0x15, 1 },
	{ "iSerialNumber", 0x16, 1 },
	{ "iOemID", 0x17, 1 },
	{ "wManufactureID", 0x18, 2 },
	{ "bUD0BaseOffset", 0x1a, 1 },
	{ "bUDConfigPLength", 0x1b, 1 },
	{ "bDeviceRTTCap", 0x1c, 1 },
	{ "wPeriodicRTCUpdate", 0x1d, 2 },
	{ "bUFSFeaturesSupport", 0x1f, 1 },
	{ "bFFUTimeout", 0x20, 1 },
	{ "bQueueDepth", 0x21, 1 },
	{ "wDeviceVersion", 0x22, 2 },
	{ "bNumSecureWpArea", 0x24, 1 },
	{ "dPSAMaxDataSize", 0x25, 4 },
	{ "bPSAStateTimeout", 0x29, 1 },
	{ "iProductRevisionLevel", 0x2a, 1 },
};

static const struct mi_desc_field mi_health_fields[] = {
	{ "bLength", 0, 1 },
	{ "bDescriptorType", 1, 1 },
	{ "bPreEOLInfo", 2, 1 },
	{ "bDeviceLifeTimeEstA", 3, 1 },
	{ "bDeviceLifeTimeEstB", 4, 1 },
};

static ssize_t mi_dump_desc(struct device *dev, char *buf, enum desc_idn id,
			    const struct mi_desc_field *fields, unsigned int count)
{
	struct mi_memory *memory = dev_get_drvdata(dev);
	u8 desc[QUERY_DESC_MAX_SIZE] = {};
	unsigned int i, value;
	ssize_t len = 0;
	int ret = mi_read_desc(memory->hba, id, desc, sizeof(desc));

	if (ret)
		return ret;
	for (i = 0; i < count; i++) {
		const struct mi_desc_field *field = &fields[i];
		const u8 *p = desc + field->offset;

		if (field->offset + field->width > desc[0])
			break;
		if (field->width == 4)
			value = get_unaligned_be32(p);
		else if (field->width == 2)
			value = get_unaligned_be16(p);
		else
			value = *p;
		len += scnprintf(buf + len, PAGE_SIZE - len,
			"Device Descriptor[Byte offset 0x%x]: %s = 0x%x\n",
			field->offset, field->name, value);
	}
	return len;
}

static ssize_t dump_device_desc_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return mi_dump_desc(dev, buf, QUERY_DESC_IDN_DEVICE, mi_device_fields,
			    ARRAY_SIZE(mi_device_fields));
}
static DEVICE_ATTR_RO(dump_device_desc);

static ssize_t dump_health_desc_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return mi_dump_desc(dev, buf, QUERY_DESC_IDN_HEALTH, mi_health_fields,
			    ARRAY_SIZE(mi_health_fields));
}
static DEVICE_ATTR_RO(dump_health_desc);

static ssize_t dump_string_desc_serial_show(struct device *dev,
					   struct device_attribute *attr, char *buf)
{
	struct mi_memory *memory = dev_get_drvdata(dev);
	struct ufs_hba *hba = memory->hba;
	u8 desc[QUERY_DESC_MAX_SIZE] = {}, *serial = NULL;
	ssize_t len = 0;
	int i, ret;

	ret = pm_runtime_get_sync(hba->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(hba->dev);
		return ret;
	}
	ret = ufshcd_read_desc_param(hba, QUERY_DESC_IDN_DEVICE, 0, 0,
				     desc, sizeof(desc));
	if (!ret && desc[0] <= DEVICE_DESC_PARAM_SN)
		ret = -EINVAL;
	if (!ret)
		ret = ufshcd_read_string_desc(hba, desc[DEVICE_DESC_PARAM_SN],
					     &serial, false);
	pm_runtime_put_sync(hba->dev);
	if (ret < 0)
		return ret;
	if (!serial)
		return -ENODATA;
	len = scnprintf(buf, PAGE_SIZE, "serial:");
	for (i = QUERY_DESC_HDR_SIZE; i + 1 < serial[0]; i += 2)
		len += scnprintf(buf + len, PAGE_SIZE - len, "%02x%02x",
				 serial[i], serial[i + 1]);
	kfree(serial);
	return len + scnprintf(buf + len, PAGE_SIZE - len, "\n");
}
static DEVICE_ATTR_RO(dump_string_desc_serial);

void mi_memory_error_init(struct ufs_hba *hba)
{
	spin_lock_init(&hba->mi_errors.lock);
}

void mi_memory_record_error(struct ufs_hba *hba, const char *reason)
{
	struct mi_memory_errors *errors = &hba->mi_errors;
	unsigned long flags;

	spin_lock_irqsave(&errors->lock, flags);
	errors->count++;
	memmove(errors->reason[1], errors->reason[0],
		 sizeof(errors->reason) - sizeof(errors->reason[0]));
	strscpy(errors->reason[0], reason, sizeof(errors->reason[0]));
	spin_unlock_irqrestore(&errors->lock, flags);
}

static ssize_t err_state_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct mi_memory *memory = dev_get_drvdata(dev);
	struct mi_memory_errors *errors = &memory->hba->mi_errors;
	unsigned long flags;
	u64 count;

	spin_lock_irqsave(&errors->lock, flags);
	count = errors->count;
	spin_unlock_irqrestore(&errors->lock, flags);
	return scnprintf(buf, PAGE_SIZE, "%llu\n", count);
}
static DEVICE_ATTR_RO(err_state);

static ssize_t err_reason_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct mi_memory *memory = dev_get_drvdata(dev);
	struct mi_memory_errors *errors = &memory->hba->mi_errors;
	unsigned long flags;
	char reasons[10][32];
	ssize_t len = 0;
	int i;

	spin_lock_irqsave(&errors->lock, flags);
	memcpy(reasons, errors->reason, sizeof(reasons));
	spin_unlock_irqrestore(&errors->lock, flags);
	for (i = 0; i < ARRAY_SIZE(reasons); i++)
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s", reasons[i]);
	return len;
}
static DEVICE_ATTR_RO(err_reason);

static ssize_t show_hba_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct mi_memory *memory = dev_get_drvdata(dev);
	struct ufs_hba *hba = memory->hba;
	struct ufs_stats *stats = &hba->ufs_stats;
	ssize_t len = 0;
	int i;
	static const char * const dl_names[] = {
		"dl_nac_received_err_cnt", "dl_tcx_replay_timer_expired_err_cnt",
		"dl_afcx_request_timer_expired_err_cnt",
		"dl_fcx_protection_timer_expired_err_cnt", "dl_crc_err_cnt",
		"dll_rx_buffer_overflow_err_cnt", "dl_max_frame_length_exceeded_err_cnt",
		"dl_wrong_sequence_number_err_cnt", "dl_afc_frame_syntax_err_cnt",
		"dl_nac_frame_syntax_err_cnt", "dl_eof_syntax_err_cnt",
		"dl_frame_syntax_err_cnt", "dl_bad_ctrl_symbol_type_err_cnt",
		"dl_pa_init_err_cnt", "dl_pa_error_ind_received",
	};

#define MI_HBA_FIELD(field, fmt) \
	(len += scnprintf(buf + len, PAGE_SIZE - len, \
			  "hba->" #field " = " fmt "\n", READ_ONCE(hba->field)))
	MI_HBA_FIELD(outstanding_tasks, "0x%lx");
	MI_HBA_FIELD(outstanding_reqs, "0x%lx");
	MI_HBA_FIELD(capabilities, "0x%x");
	MI_HBA_FIELD(nutrs, "%d");
	MI_HBA_FIELD(nutmrs, "%d");
	MI_HBA_FIELD(ufs_version, "0x%x");
	MI_HBA_FIELD(irq, "0x%x");
	MI_HBA_FIELD(auto_bkops_enabled, "%d");
	MI_HBA_FIELD(ufshcd_state, "0x%x");
	MI_HBA_FIELD(clk_gating.state, "0x%x");
	MI_HBA_FIELD(eh_flags, "0x%x");
	MI_HBA_FIELD(intr_mask, "0x%x");
	MI_HBA_FIELD(ee_ctrl_mask, "0x%x");
	MI_HBA_FIELD(errors, "0x%x");
	MI_HBA_FIELD(uic_error, "0x%x");
	MI_HBA_FIELD(saved_err, "0x%x");
	MI_HBA_FIELD(saved_uic_err, "0x%x");
#undef MI_HBA_FIELD
#define MI_STAT(name, value) \
	(len += scnprintf(buf + len, PAGE_SIZE - len, \
			  name " = %u\n", READ_ONCE(value)))
	MI_STAT("power_mode_change_cnt", stats->power_mode_change_cnt);
	MI_STAT("hibern8_exit_cnt", stats->hibern8_exit_cnt);
	MI_STAT("pa_err_cnt_total", stats->pa_err_cnt_total);
	MI_STAT("pa_lane_0_err_cnt", stats->pa_err_cnt[UFS_EC_PA_LANE_0]);
	MI_STAT("pa_lane_1_err_cnt", stats->pa_err_cnt[UFS_EC_PA_LANE_1]);
	MI_STAT("pa_line_reset_err_cnt", stats->pa_err_cnt[UFS_EC_PA_LINE_RESET]);
	MI_STAT("dl_err_cnt_total", stats->dl_err_cnt_total);
	for (i = 0; i < ARRAY_SIZE(dl_names); i++)
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s = %u\n",
				 dl_names[i], READ_ONCE(stats->dl_err_cnt[i]));
	MI_STAT("dme_err_cnt", stats->dme_err_cnt);
#undef MI_STAT
	return len;
}
static DEVICE_ATTR_RO(show_hba);

/* The stock CDBs use vendor-specific 16-byte commands and 512-byte reports. */
static int mi_hr_execute(struct scsi_device *sdev, u64 lo, u64 hi,
			 int direction, void *buf, unsigned int size)
{
	u8 cdb[16];
	int ret;

	put_unaligned_le64(lo, cdb);
	put_unaligned_le64(hi, cdb + 8);
	ret = scsi_execute(sdev, cdb, direction, buf, size, NULL, NULL,
			   30 * HZ, 3, 0, 0, NULL);
	return ret > 0 ? -EIO : ret;
}

static int mi_hr_samsung(struct scsi_device *sdev, u8 *buf)
{
	int ret, exit_ret;

	ret = mi_hr_execute(sdev, 0xc01000040c0ULL, 0x1c00000000000000ULL,
			    DMA_FROM_DEVICE, buf, 0x1c);
	if (ret)
		return ret;
	ret = mi_hr_execute(sdev, 0x6867ae23385c00c0ULL, 0x72,
			    DMA_NONE, NULL, 0);
	if (ret) {
		ret = mi_hr_execute(sdev, 0x72686703c0ULL, 0, DMA_NONE, NULL, 0);
		if (!ret)
			ret = mi_hr_execute(sdev, 0x6867ae23385c00c0ULL, 0x72,
					    DMA_NONE, NULL, 0);
	}
	if (ret)
		return ret;
	ret = mi_hr_execute(sdev, 0xa01000040c0ULL, 0x4c00000000000000ULL,
			    DMA_FROM_DEVICE, buf + 0x80, 0x4c);
	/* Always leave vendor mode, including after a failed report read. */
	exit_ret = mi_hr_execute(sdev, 0x1c0, 0, DMA_NONE, NULL, 0);
	return ret ? ret : exit_ret;
}

static int mi_hr_read(struct ufs_hba *hba, struct scsi_device *sdev, u8 *buf)
{
	u8 *micron_request;
	int ret;

	if (!strncmp(sdev->vendor, "WDC", 3))
		return mi_hr_execute(sdev, 0x200699c7d01013cULL, 0,
				     DMA_FROM_DEVICE, buf, 512);
	if (!strncmp(sdev->vendor, "TOSHIBA", 7) ||
	    !strncmp(sdev->vendor, "KIOXIA", 6))
		return mi_hr_execute(sdev, 0x2c06912, 0, DMA_FROM_DEVICE, buf, 512);
	if (!strncmp(sdev->vendor, "SAMSUNG", 7))
		return mi_hr_samsung(sdev, buf);
	if (!strncmp(sdev->vendor, "MICRON", 6)) {
		micron_request = kzalloc(44, GFP_KERNEL);
		if (!micron_request)
			return -ENOMEM;
		put_unaligned_le64(0x1100040feULL, micron_request);
		ret = mi_hr_execute(sdev, 0xe13b, 0x2c, DMA_TO_DEVICE,
				    micron_request, 44);
		kfree(micron_request);
		if (ret)
			return ret;
		return mi_hr_execute(sdev, 0x20000000000c13cULL, 0,
				     DMA_FROM_DEVICE, buf, 512);
	}
	if (!strncmp(sdev->vendor, "SKhynix", 7)) {
		ret = mi_read_desc(hba, QUERY_DESC_IDN_HEALTH, buf, 0x25);
		if (ret)
			return ret;
		return mi_hr_execute(sdev, 0x5803d0, 0x2c000000,
				     DMA_FROM_DEVICE, buf + 0x80, 0x4c);
	}
	return -EOPNOTSUPP;
}

static ssize_t hr_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mi_memory *memory = dev_get_drvdata(dev);
	struct scsi_device *sdev;
	u8 *report;
	ssize_t len;
	int ret, i;

	report = kzalloc(512, GFP_KERNEL);
	if (!report)
		return -ENOMEM;
	/* Serialise vendor-mode sequences, not just individual SCSI commands. */
	mutex_lock(&memory->hr_lock);
	sdev = scsi_device_lookup(memory->hba->host, 0, 0, 0);
	if (!sdev) {
		len = -ENODEV;
		goto out_unlock;
	}
	ret = scsi_autopm_get_device(sdev);
	if (!ret) {
		ret = mi_hr_read(memory->hba, sdev, report);
		scsi_autopm_put_device(sdev);
	}
	if (ret == -EOPNOTSUPP) {
		len = scnprintf(buf, PAGE_SIZE, "NOT SUPPORTED %s\n", sdev->vendor);
	} else if (ret) {
		len = scnprintf(buf, PAGE_SIZE, "Fail to get hr, err is: %d\n", ret);
	} else {
		for (i = 0; i < 512; i++)
			hex_byte_pack(buf + 2 * i, report[i]);
		buf[1024] = '\n';
		len = 1025;
	}
	scsi_device_put(sdev);
out_unlock:
	mutex_unlock(&memory->hr_lock);
	kfree(report);
	return len;
}
static DEVICE_ATTR_RO(hr);

static struct attribute *mi_ufs_attrs[] = {
	&dev_attr_dump_health_desc.attr,
	&dev_attr_dump_string_desc_serial.attr,
	&dev_attr_dump_device_desc.attr,
	&dev_attr_show_hba.attr,
	&dev_attr_hr.attr,
	&dev_attr_err_state.attr,
	&dev_attr_err_reason.attr,
	NULL,
};

static struct attribute *mi_ddr_attrs[] = {
	&dev_attr_ddr_size.attr, &dev_attr_ddr_id.attr, &dev_attr_ddr_vendor.attr,
	NULL,
};

static const struct attribute_group mi_ufs_group = {
	.name = "ufshcd0", .attrs = mi_ufs_attrs,
};
static const struct attribute_group mi_ddr_group = {
	.name = "ddr", .attrs = mi_ddr_attrs,
};
static const struct attribute_group *mi_memory_groups[] = {
	&mi_ufs_group, &mi_ddr_group, NULL,
};

static unsigned int mi_ufs_capacity(u64 blocks)
{
	u64 gb = blocks >> 21;

	if (gb <= 8)
		return 0;
	if (gb <= 16)
		return 8;
	if (gb <= 32)
		return 32;
	if (gb <= 64)
		return 64;
	if (gb <= 128)
		return 128;
	if (gb <= 256)
		return 256;
	if (gb <= 512 || gb > 1024)
		return 512;
	return 1024;
}

static int mi_mv_show(struct seq_file *seq, void *v)
{
	struct mi_memory *memory = seq->private;
	struct ufs_hba *hba = memory->hba;
	u8 device[QUERY_DESC_MAX_SIZE] = {}, geometry[QUERY_DESC_MAX_SIZE] = {};
	u8 *product = NULL, *revision = NULL;
	int ret, id = mi_ddr_id();

	if (id < 0)
		return id;
	ret = pm_runtime_get_sync(hba->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(hba->dev);
		return ret;
	}
	ret = ufshcd_read_desc_param(hba, QUERY_DESC_IDN_DEVICE, 0, 0,
				     device, sizeof(device));
	if (!ret)
		ret = ufshcd_read_desc_param(hba, QUERY_DESC_IDN_GEOMETRY, 0, 0,
					     geometry, sizeof(geometry));
	if (!ret && (device[0] <= 0x2a || geometry[0] < 12))
		ret = -EINVAL;
	if (!ret) {
		ret = ufshcd_read_string_desc(hba, device[0x15], &product, true);
		if (ret >= 0)
			ret = ufshcd_read_string_desc(hba, device[0x2a], &revision, true);
		if (ret >= 0)
			ret = product && revision ? 0 : -ENODATA;
	}
	pm_runtime_put_sync(hba->dev);
	if (!ret) {
		seq_printf(seq, "D: 0x%02x %u\n", id, mi_ddr_size);
		seq_printf(seq, "U: 0x%04x %u %.14s %.4s\n",
			   get_unaligned_be16(device + 0x18),
			   mi_ufs_capacity(get_unaligned_be64(geometry + 4)),
			   product, revision);
	}
	kfree(product);
	kfree(revision);
	return ret;
}

static int mi_mv_open(struct inode *inode, struct file *file)
{
	return single_open(file, mi_mv_show, PDE_DATA(inode));
}

static const struct file_operations mi_mv_ops = {
	.owner = THIS_MODULE,
	.open = mi_mv_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations mi_memory_ops = {
	.owner = THIS_MODULE,
	.llseek = no_llseek,
};

int mi_memory_register(struct ufs_hba *hba)
{
	struct mi_memory *memory;
	int ret;

	memory = kzalloc(sizeof(*memory), GFP_KERNEL);
	if (!memory)
		return -ENOMEM;
	memory->hba = hba;
	mutex_init(&memory->hr_lock);
	memory->class = class_create(THIS_MODULE, "mi_memory");
	if (IS_ERR(memory->class)) {
		ret = PTR_ERR(memory->class);
		goto free_memory;
	}
	memory->major = register_chrdev(0, "mi_memory_module", &mi_memory_ops);
	if (memory->major < 0) {
		ret = memory->major;
		goto destroy_class;
	}
	memory->device = device_create(memory->class, NULL,
				       MKDEV(memory->major, 1), memory,
				       "mi_memory_device");
	if (IS_ERR(memory->device)) {
		ret = PTR_ERR(memory->device);
		goto unregister_chrdev;
	}
	ret = sysfs_create_groups(&memory->device->kobj, mi_memory_groups);
	if (ret)
		goto destroy_device;
	memory->proc = proc_create_data("mv", 0444, NULL, &mi_mv_ops, memory);
	if (!memory->proc) {
		ret = -ENOMEM;
		goto remove_groups;
	}
	hba->mi_memory = memory;
	return 0;
remove_groups:
	sysfs_remove_groups(&memory->device->kobj, mi_memory_groups);
destroy_device:
	device_destroy(memory->class, MKDEV(memory->major, 1));
unregister_chrdev:
	unregister_chrdev(memory->major, "mi_memory_module");
destroy_class:
	class_destroy(memory->class);
free_memory:
	kfree(memory);
	return ret;
}

void mi_memory_unregister(struct ufs_hba *hba)
{
	struct mi_memory *memory = hba->mi_memory;

	if (!memory)
		return;
	proc_remove(memory->proc);
	sysfs_remove_groups(&memory->device->kobj, mi_memory_groups);
	device_destroy(memory->class, MKDEV(memory->major, 1));
	unregister_chrdev(memory->major, "mi_memory_module");
	class_destroy(memory->class);
	hba->mi_memory = NULL;
	kfree(memory);
}
