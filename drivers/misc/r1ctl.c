// SPDX-License-Identifier: GPL-2.0-only
/*
 * Rabbit R1: reboot / reboot-to-FASTBOOT from sysfs or /dev/r1ctl.
 *
 * FASTBOOT on this LK is preloader ASCII "FASTBOOT", not a Linux boot
 * mode. We reset TOPRGU; the host catcher writes FASTBOOT on 0e8d:2000.
 * No SIM-slot button required.
 */
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kobject.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>

#define TOPRGU_PA	0x10007000UL
#define WDT_MODE	0x00
#define WDT_MODE_EN	BIT(0)
#define WDT_MODE_KEY	0x22000000
#define WDT_RST		0x08
#define WDT_RST_RELOAD	0x1971
#define WDT_SWRST	0x14
#define WDT_SWRST_KEY	0x1209

static void r1ctl_toprgu_reset(void)
{
	void __iomem *w = ioremap(TOPRGU_PA, 0x1000);

	if (!w) {
		pr_err("r1ctl: toprgu ioremap failed\n");
		return;
	}
	writel(WDT_MODE_EN | WDT_MODE_KEY, w + WDT_MODE);
	writel(WDT_RST_RELOAD, w + WDT_RST);
	wmb();
	while (1) {
		writel(WDT_SWRST_KEY, w + WDT_SWRST);
		mdelay(5);
	}
}

static void r1ctl_do_reboot(const char *why)
{
	pr_info("r1ctl: %s — kernel_restart then TOPRGU\n", why);
	kernel_restart(NULL);
	r1ctl_toprgu_reset();
}

static ssize_t cmd_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	char tmp[32];
	size_t n = min(count, sizeof(tmp) - 1);

	memcpy(tmp, buf, n);
	tmp[n] = 0;
	strim(tmp);
	if (!strcmp(tmp, "reboot") || !strcmp(tmp, "restart")) {
		r1ctl_do_reboot("reboot");
		return count;
	}
	if (!strcmp(tmp, "fastboot") || !strcmp(tmp, "bootloader")) {
		r1ctl_do_reboot("fastboot");
		return count;
	}
	return -EINVAL;
}

static struct kobj_attribute cmd_attr = __ATTR_WO(cmd);
static struct kobject *r1_kobj;

static ssize_t r1ctl_write(struct file *f, const char __user *ubuf,
			   size_t len, loff_t *ppos)
{
	char tmp[32];
	size_t n = min(len, sizeof(tmp) - 1);

	if (copy_from_user(tmp, ubuf, n))
		return -EFAULT;
	tmp[n] = 0;
	strim(tmp);
	if (!strcmp(tmp, "reboot") || !strcmp(tmp, "restart")) {
		r1ctl_do_reboot("reboot");
		return len;
	}
	if (!strcmp(tmp, "fastboot") || !strcmp(tmp, "bootloader")) {
		r1ctl_do_reboot("fastboot");
		return len;
	}
	return -EINVAL;
}

static const struct file_operations r1ctl_fops = {
	.owner	= THIS_MODULE,
	.write	= r1ctl_write,
	.llseek	= noop_llseek,
};

static struct miscdevice r1ctl_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "r1ctl",
	.fops	= &r1ctl_fops,
	.mode	= 0222,
};

static int __init r1ctl_init(void)
{
	int ret;

	r1_kobj = kobject_create_and_add("r1", kernel_kobj);
	if (!r1_kobj)
		return -ENOMEM;
	ret = sysfs_create_file(r1_kobj, &cmd_attr.attr);
	if (ret)
		goto err_kobj;
	ret = misc_register(&r1ctl_dev);
	if (ret)
		goto err_sys;
	pr_info("r1ctl: /sys/kernel/r1/cmd and /dev/r1ctl (reboot|fastboot)\n");
	return 0;
err_sys:
	sysfs_remove_file(r1_kobj, &cmd_attr.attr);
err_kobj:
	kobject_put(r1_kobj);
	return ret;
}

static void __exit r1ctl_exit(void)
{
	misc_deregister(&r1ctl_dev);
	sysfs_remove_file(r1_kobj, &cmd_attr.attr);
	kobject_put(r1_kobj);
}

module_init(r1ctl_init);
module_exit(r1ctl_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 reboot / FASTBOOT trigger");
