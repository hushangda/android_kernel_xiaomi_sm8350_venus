// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2019, The Linux Foundation. All rights reserved.
 */

#include <linux/types.h>
#include <linux/msm_dsps.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/of_device.h>
#include <asm/arch_timer.h>
#include <linux/uaccess.h>

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>

#include <soc/qcom/subsystem_restart.h>

#define IMAGE_LOAD_CMD 1
#define IMAGE_UNLOAD_CMD 0
#define SSR_RESET_CMD 1
#define CLASS_NAME	"ssc"
#define DRV_NAME	"sensors"
#define DRV_VERSION	"2.00"
#ifdef CONFIG_COMPAT
#define DSPS_IOCTL_READ_SLOW_TIMER32	_IOR(DSPS_IOCTL_MAGIC, 3, compat_uint_t)
#endif

#define QTICK_DIV_FACTOR	0x249F

struct sns_ssc_control_s {
	struct class *dev_class;
	dev_t dev_num;
	struct device *dev;
	struct cdev *cdev;
};
static struct sns_ssc_control_s sns_ctl;

static ssize_t slpi_boot_store(struct kobject *kobj,
	struct kobj_attribute *attr,
	const char *buf, size_t count);

static ssize_t slpi_ssr_store(struct kobject *kobj,
	struct kobj_attribute *attr,
	const char *buf, size_t count);

struct slpi_loader_private {
	void *pil_h;
	bool boot_requested;
	struct kobject *boot_slpi_obj;
	struct attribute_group *attr_group;
};

static struct kobj_attribute slpi_boot_attribute =
	__ATTR(boot, 0220, NULL, slpi_boot_store);

static struct kobj_attribute slpi_ssr_attribute =
	__ATTR(ssr, 0220, NULL, slpi_ssr_store);

static struct attribute *attrs[] = {
	&slpi_boot_attribute.attr,
	&slpi_ssr_attribute.attr,
	NULL,
};

static struct platform_device *slpi_private;
static struct work_struct slpi_ldr_work;
/* Serializes handle ownership and boot/SSR requests, including a running load. */
static DEFINE_MUTEX(slpi_loader_lock);
static bool slpi_claimed;

static void slpi_load_fw(struct work_struct *work)
{
	struct platform_device *pdev;
	struct slpi_loader_private *priv;
	const char *firmware_name;
	void *pil_h;
	int ret;

	mutex_lock(&slpi_loader_lock);
	pdev = slpi_private;
	if (!pdev)
		goto out;
	priv = platform_get_drvdata(pdev);
	if (!priv->boot_requested || priv->pil_h)
		goto out;

	ret = of_property_read_string(pdev->dev.of_node,
		"qcom,firmware-name", &firmware_name);
	if (ret < 0) {
		dev_err(&pdev->dev, "can't get fw name\n");
		goto out;
	}

	pil_h = subsystem_get_with_fwname("slpi", firmware_name);
	if (IS_ERR_OR_NULL(pil_h)) {
		dev_err(&pdev->dev, "SLPI image loading failed: %ld\n",
			pil_h ? PTR_ERR(pil_h) : -ENODEV);
		goto out;
	}
	priv->pil_h = pil_h;
	dev_dbg(&pdev->dev, "SLPI image is loaded\n");
out:
	mutex_unlock(&slpi_loader_lock);
}

/* Caller holds slpi_loader_lock. */
static void slpi_loader_unload(struct platform_device *pdev)
{
	struct slpi_loader_private *priv = platform_get_drvdata(pdev);

	priv->boot_requested = false;
	if (!IS_ERR_OR_NULL(priv->pil_h))
		subsystem_put(priv->pil_h);
	priv->pil_h = NULL;
}

static ssize_t slpi_ssr_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct platform_device *pdev;
	struct slpi_loader_private *priv;
	int ssr_cmd, ret = -ENODEV;

	if (kstrtoint(buf, 10, &ssr_cmd) < 0 || ssr_cmd != SSR_RESET_CMD)
		return -EINVAL;

	mutex_lock(&slpi_loader_lock);
	pdev = slpi_private;
	if (!pdev)
		goto out;
	priv = platform_get_drvdata(pdev);
	if (IS_ERR_OR_NULL(priv->pil_h))
		goto out;

	/* Keep the owned handle valid until restart has acquired its references. */
	ret = subsystem_restart_dev(priv->pil_h);
out:
	mutex_unlock(&slpi_loader_lock);
	return ret ? ret : count;
}

static ssize_t slpi_boot_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct platform_device *pdev;
	struct slpi_loader_private *priv;
	int boot, ret = 0;

	if (kstrtoint(buf, 10, &boot) < 0 ||
	    (boot != IMAGE_LOAD_CMD && boot != IMAGE_UNLOAD_CMD))
		return -EINVAL;

	mutex_lock(&slpi_loader_lock);
	pdev = slpi_private;
	if (!pdev) {
		ret = -ENODEV;
		goto out;
	}
	priv = platform_get_drvdata(pdev);
	if (boot == IMAGE_LOAD_CMD) {
		priv->boot_requested = true;
		if (!priv->pil_h)
			schedule_work(&slpi_ldr_work);
	} else {
		/* A pending worker observes false; a running load finishes first. */
		slpi_loader_unload(pdev);
	}
out:
	mutex_unlock(&slpi_loader_lock);
	return ret ? ret : count;
}

static int slpi_loader_init_sysfs(struct platform_device *pdev)
{
	int ret = -EINVAL;
	struct slpi_loader_private *priv = NULL;
	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		return ret;
	}

	platform_set_drvdata(pdev, priv);

	priv->pil_h = NULL;
	priv->boot_slpi_obj = NULL;
	priv->attr_group = devm_kzalloc(&pdev->dev,
				sizeof(*(priv->attr_group)),
				GFP_KERNEL);
	if (!priv->attr_group) {
		dev_err(&pdev->dev, "%s: malloc attr_group failed\n",
						__func__);
		ret = -ENOMEM;
		goto error_return;
	}

	priv->attr_group->attrs = attrs;

	priv->boot_slpi_obj = kobject_create_and_add("boot_slpi", kernel_kobj);
	if (!priv->boot_slpi_obj) {
		dev_err(&pdev->dev, "%s: sysfs create and add failed\n",
						__func__);
		ret = -ENOMEM;
		goto error_return;
	}

	INIT_WORK(&slpi_ldr_work, slpi_load_fw);
	mutex_lock(&slpi_loader_lock);
	slpi_private = pdev;
	mutex_unlock(&slpi_loader_lock);

	ret = sysfs_create_group(priv->boot_slpi_obj, priv->attr_group);
	if (ret) {
		dev_err(&pdev->dev, "%s: sysfs create group failed %d\n",
							__func__, ret);
		/* A partially published group may already have accepted boot=1. */
		mutex_lock(&slpi_loader_lock);
		slpi_private = NULL;
		slpi_loader_unload(pdev);
		mutex_unlock(&slpi_loader_lock);
		cancel_work_sync(&slpi_ldr_work);
		goto error_return;
	}

	return 0;

error_return:
	mutex_lock(&slpi_loader_lock);
	slpi_private = NULL;
	mutex_unlock(&slpi_loader_lock);

	if (priv->boot_slpi_obj) {
		kobject_put(priv->boot_slpi_obj);
		priv->boot_slpi_obj = NULL;
	}

	return ret;
}

static int slpi_loader_remove(struct platform_device *pdev)
{
	struct slpi_loader_private *priv = platform_get_drvdata(pdev);

	/* Drain sysfs callers before withdrawing the device and queued work. */
	sysfs_remove_group(priv->boot_slpi_obj, priv->attr_group);
	mutex_lock(&slpi_loader_lock);
	slpi_private = NULL;
	slpi_loader_unload(pdev);
	mutex_unlock(&slpi_loader_lock);
	cancel_work_sync(&slpi_ldr_work);
	kobject_put(priv->boot_slpi_obj);
	priv->boot_slpi_obj = NULL;
	return 0;
}

/*
 * Read virtual QTimer clock ticks and scale down to 32KHz clock as used
 * in DSPS
 */
static u32 sns_read_qtimer(void)
{
	u64 val;

	val = __arch_counter_get_cntvct();
	/*
	 * To convert ticks from 19.2 Mhz clock to 32768 Hz clock:
	 * x = (value * 32768) / 19200000
	 * This is same as first left shift the value by 4 bits, i.e. multiply
	 * by 16, and then divide by 0x249F. The latter is preferable since
	 * QTimer tick (value) is 56-bit, so (value * 32768) could overflow,
	 * while (value * 16) will never do
	 */
	val <<= 4;
	do_div(val, QTICK_DIV_FACTOR);

	return (u32)val;
}

static int sensors_ssc_open(struct inode *ip, struct file *fp)
{
	return 0;
}

static int sensors_ssc_release(struct inode *inode, struct file *file)
{
	return 0;
}

static long sensors_ssc_ioctl(struct file *file,
			unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	u32 val = 0;

	switch (cmd) {
	case DSPS_IOCTL_READ_SLOW_TIMER:
#ifdef CONFIG_COMPAT
	case DSPS_IOCTL_READ_SLOW_TIMER32:
#endif
		val = sns_read_qtimer();
		ret = put_user(val, (u32 __user *) arg);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

const struct file_operations sensors_ssc_fops = {
	.owner = THIS_MODULE,
	.open = sensors_ssc_open,
	.release = sensors_ssc_release,
#ifdef CONFIG_COMPAT
	.compat_ioctl = sensors_ssc_ioctl,
#endif
	.unlocked_ioctl = sensors_ssc_ioctl
};

static int sensors_ssc_probe(struct platform_device *pdev)
{
	int ret;

	mutex_lock(&slpi_loader_lock);
	if (slpi_claimed) {
		mutex_unlock(&slpi_loader_lock);
		return -EBUSY;
	}
	slpi_claimed = true;
	mutex_unlock(&slpi_loader_lock);

	sns_ctl.dev_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(sns_ctl.dev_class)) {
		ret = PTR_ERR(sns_ctl.dev_class);
		pr_err("%s: class_create fail.\n", __func__);
		goto res_err;
	}

	ret = alloc_chrdev_region(&sns_ctl.dev_num, 0, 1, DRV_NAME);
	if (ret) {
		pr_err("%s: alloc_chrdev_region fail.\n", __func__);
		goto alloc_chrdev_region_err;
	}

	sns_ctl.dev = device_create(sns_ctl.dev_class, NULL,
				     sns_ctl.dev_num,
				     &sns_ctl, DRV_NAME);
	if (IS_ERR(sns_ctl.dev)) {
		ret = PTR_ERR(sns_ctl.dev);
		pr_err("%s: device_create fail.\n", __func__);
		goto device_create_err;
	}

	sns_ctl.cdev = cdev_alloc();
	if (sns_ctl.cdev == NULL) {
		ret = -ENOMEM;
		pr_err("%s: cdev_alloc fail.\n", __func__);
		goto cdev_alloc_err;
	}
	sns_ctl.cdev->ops = &sensors_ssc_fops;
	sns_ctl.cdev->owner = THIS_MODULE;

	ret = cdev_add(sns_ctl.cdev, sns_ctl.dev_num, 1);
	if (ret) {
		pr_err("%s: cdev_add fail.\n", __func__);
		goto cdev_add_err;
	}

	ret = slpi_loader_init_sysfs(pdev);
	if (ret)
		goto sysfs_err;

	return 0;

sysfs_err:
	cdev_del(sns_ctl.cdev);
	goto cdev_alloc_err;
cdev_add_err:
	kobject_put(&sns_ctl.cdev->kobj);
cdev_alloc_err:
	device_destroy(sns_ctl.dev_class, sns_ctl.dev_num);
device_create_err:
	unregister_chrdev_region(sns_ctl.dev_num, 1);
alloc_chrdev_region_err:
	class_destroy(sns_ctl.dev_class);
res_err:
	mutex_lock(&slpi_loader_lock);
	slpi_claimed = false;
	mutex_unlock(&slpi_loader_lock);
	return ret;
}

static int sensors_ssc_remove(struct platform_device *pdev)
{
	slpi_loader_remove(pdev);
	cdev_del(sns_ctl.cdev);
	sns_ctl.cdev = NULL;
	device_destroy(sns_ctl.dev_class, sns_ctl.dev_num);
	unregister_chrdev_region(sns_ctl.dev_num, 1);
	class_destroy(sns_ctl.dev_class);
	mutex_lock(&slpi_loader_lock);
	slpi_claimed = false;
	mutex_unlock(&slpi_loader_lock);

	return 0;
}

static const struct of_device_id msm_ssc_sensors_dt_match[] = {
	{.compatible = "qcom,msm-ssc-sensors"},
	{},
};
MODULE_DEVICE_TABLE(of, msm_ssc_sensors_dt_match);

static struct platform_driver sensors_ssc_driver = {
	.driver = {
		.name = "sensors-ssc",
		.of_match_table = msm_ssc_sensors_dt_match,
	},
	.probe = sensors_ssc_probe,
	.remove = sensors_ssc_remove,
};

static int __init sensors_ssc_init(void)
{
	int rc;

	pr_debug("%s driver version %s.\n", DRV_NAME, DRV_VERSION);
	rc = platform_driver_register(&sensors_ssc_driver);
	if (rc) {
		pr_err("%s: Failed to register sensors ssc driver\n",
			__func__);
		return rc;
	}

	return 0;
}

static void __exit sensors_ssc_exit(void)
{
	platform_driver_unregister(&sensors_ssc_driver);
}

module_init(sensors_ssc_init);
module_exit(sensors_ssc_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Sensors SSC driver");
