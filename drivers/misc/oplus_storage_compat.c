// SPDX-License-Identifier: GPL-2.0
/*
 * OPlus storage/devinfo compatibility layer for ColorOS bringup.
 *
 * Real mapped behavior:
 * - Exposes OPlus paths expected by ColorOS PerformanceService.
 * - Reads UFS identity dynamically from standard sysfs.
 * - Maps ColorOS virtualtlcbuff read to real UFS write-buffer sysfs values.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#define UFSHCI_DEV_NAME "112b0000.ufshci"

static struct proc_dir_entry *devinfo_dir;
static struct device *ufshci_dev;

static int read_file_trimmed(const char *path, char *buf, size_t size)
{
	struct file *file;
	loff_t pos = 0;
	ssize_t ret;

	if (!buf || size < 2)
		return -EINVAL;

	memset(buf, 0, size);

	file = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);

	ret = kernel_read(file, buf, size - 1, &pos);
	filp_close(file, NULL);

	if (ret < 0)
		return ret;

	buf[ret] = '\0';
	strim(buf);

	return 0;
}

static bool file_readable(const char *path)
{
	char tmp[8];

	return read_file_trimmed(path, tmp, sizeof(tmp)) == 0;
}

static int read_first_available(char *buf, size_t size, const char * const paths[])
{
	int i;

	for (i = 0; paths[i]; i++) {
		if (!read_file_trimmed(paths[i], buf, size))
			return 0;
	}

	return -ENOENT;
}

static int devinfo_ufs_show(struct seq_file *m, void *v)
{
	char vendor[64] = "unknown";
	char model[96] = "unknown";
	char rev[32] = "unknown";

	read_file_trimmed("/sys/block/sda/device/vendor", vendor, sizeof(vendor));
	read_file_trimmed("/sys/block/sda/device/model", model, sizeof(model));
	read_file_trimmed("/sys/block/sda/device/rev", rev, sizeof(rev));

	seq_printf(m, "%s %s %s\n", vendor, model, rev);
	return 0;
}

static int devinfo_emmc_show(struct seq_file *m, void *v)
{
	if (file_readable("/sys/block/mmcblk0/device/name"))
		seq_puts(m, "present\n");
	else
		seq_puts(m, "not_support\n");

	return 0;
}

static int devinfo_ufs_version_show(struct seq_file *m, void *v)
{
	char product_rev[64];

	if (!read_file_trimmed("/sys/devices/platform/soc/" UFSHCI_DEV_NAME
			       "/string_descriptors/product_revision",
			       product_rev, sizeof(product_rev)))
		seq_printf(m, "UFS %s\n", product_rev);
	else
		seq_puts(m, "UFS\n");

	return 0;
}

static int devinfo_emmc_version_show(struct seq_file *m, void *v)
{
	char name[64];

	if (!read_file_trimmed("/sys/block/mmcblk0/device/name", name, sizeof(name)))
		seq_printf(m, "%s\n", name);
	else
		seq_puts(m, "not_support\n");

	return 0;
}

static int devinfo_ufsplus_status_show(struct seq_file *m, void *v)
{
	if (file_readable("/sys/devices/platform/soc/" UFSHCI_DEV_NAME "/attributes/wb_avail_buf") ||
	    file_readable("/sys/devices/platform/soc/" UFSHCI_DEV_NAME "/wb_on") ||
	    file_readable("/sys/devices/platform/soc/" UFSHCI_DEV_NAME "/flags/wb_enable"))
		seq_puts(m, "1\n");
	else
		seq_puts(m, "0\n");

	return 0;
}

static int open_ufs(struct inode *inode, struct file *file)
{
	return single_open(file, devinfo_ufs_show, NULL);
}

static int open_emmc(struct inode *inode, struct file *file)
{
	return single_open(file, devinfo_emmc_show, NULL);
}

static int open_ufs_version(struct inode *inode, struct file *file)
{
	return single_open(file, devinfo_ufs_version_show, NULL);
}

static int open_emmc_version(struct inode *inode, struct file *file)
{
	return single_open(file, devinfo_emmc_version_show, NULL);
}

static int open_ufsplus_status(struct inode *inode, struct file *file)
{
	return single_open(file, devinfo_ufsplus_status_show, NULL);
}

static const struct proc_ops proc_ufs_ops = {
	.proc_open = open_ufs,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops proc_emmc_ops = {
	.proc_open = open_emmc,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops proc_ufs_version_ops = {
	.proc_open = open_ufs_version,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops proc_emmc_version_ops = {
	.proc_open = open_emmc_version,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops proc_ufsplus_status_ops = {
	.proc_open = open_ufsplus_status,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static ssize_t virtualtlcbuff_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	char value[64];
	const char * const paths[] = {
		"/sys/devices/platform/soc/" UFSHCI_DEV_NAME "/attributes/wb_avail_buf",
		"/sys/devices/platform/soc/" UFSHCI_DEV_NAME "/attributes/wb_cur_buf",
		"/sys/devices/platform/soc/" UFSHCI_DEV_NAME "/device_descriptor/wb_shared_alloc_units",
		"/sys/devices/platform/soc/" UFSHCI_DEV_NAME "/geometry_descriptor/max_in_buffer_size",
		NULL,
	};

	if (!read_first_available(value, sizeof(value), paths))
		return scnprintf(buf, PAGE_SIZE, "%s\n", value);

	return scnprintf(buf, PAGE_SIZE, "0\n");
}

static ssize_t virtualtlcbuff_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	/*
	 * ColorOS may write this node as a storage feature hint.
	 * We accept the write so userspace does not fail, but we do not
	 * force unknown vendor-specific UFS behavior blindly.
	 */
	return count;
}

static DEVICE_ATTR_RW(virtualtlcbuff);

static int __init oplus_storage_compat_init(void)
{
	int ret;

	devinfo_dir = proc_mkdir("devinfo", NULL);
	if (!devinfo_dir)
		return -ENOMEM;

	proc_create("ufs", 0444, devinfo_dir, &proc_ufs_ops);
	proc_create("emmc", 0444, devinfo_dir, &proc_emmc_ops);
	proc_create("ufs_version", 0444, devinfo_dir, &proc_ufs_version_ops);
	proc_create("emmc_version", 0444, devinfo_dir, &proc_emmc_version_ops);
	proc_create("ufsplus_status", 0444, devinfo_dir, &proc_ufsplus_status_ops);

	ufshci_dev = bus_find_device_by_name(&platform_bus_type, NULL,
					      UFSHCI_DEV_NAME);
	if (!ufshci_dev) {
		pr_warn("oplus_storage_compat: ufshci device not found, sysfs compat skipped\n");
		pr_info("oplus_storage_compat: /proc/devinfo active dynamic\n");
		return 0;
	}

	ret = device_create_file(ufshci_dev, &dev_attr_virtualtlcbuff);
	if (ret)
		pr_warn("oplus_storage_compat: failed to create virtualtlcbuff ret=%d\n",
			ret);
	else
		pr_info("oplus_storage_compat: virtualtlcbuff active dynamic\n");

	pr_info("oplus_storage_compat: /proc/devinfo active dynamic\n");
	return 0;
}

static void __exit oplus_storage_compat_exit(void)
{
	if (ufshci_dev) {
		device_remove_file(ufshci_dev, &dev_attr_virtualtlcbuff);
		put_device(ufshci_dev);
	}

	if (devinfo_dir)
		proc_remove(devinfo_dir);

	pr_info("oplus_storage_compat: removed\n");
}

late_initcall(oplus_storage_compat_init);
module_exit(oplus_storage_compat_exit);

MODULE_DESCRIPTION("OPlus storage/devinfo compatibility for ColorOS");
MODULE_AUTHOR("NEESCHAL");
MODULE_LICENSE("GPL");
