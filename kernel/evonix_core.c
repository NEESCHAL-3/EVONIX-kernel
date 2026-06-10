// SPDX-License-Identifier: GPL-2.0
/*
 * EVONIX core sysfs API
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>

#define EVONIX_API_VERSION	"1"
#define EVONIX_RELEASE		"v3.0"
#define EVONIX_MAINTAINER	"Neeschal"
#define EVONIX_DEVICE		"POCO X7 Pro / rodin"
#define EVONIX_BASE		"6.6 LTS"

static struct kobject *evonix_kobj;
static struct kobject *evonix_charging_kobj;
static struct proc_dir_entry *evonix_ctl_proc;

static DEFINE_MUTEX(evonix_charging_lock);
static int evonix_limit_enabled;
static int evonix_limit_percent = 100;
static char evonix_watt_mode[16] = "dynamic";

static ssize_t api_version_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", EVONIX_API_VERSION);
}

static ssize_t release_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", EVONIX_RELEASE);
}

static ssize_t maintainer_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", EVONIX_MAINTAINER);
}

static ssize_t device_show(struct kobject *kobj,
			   struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", EVONIX_DEVICE);
}

static ssize_t base_show(struct kobject *kobj,
			 struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", EVONIX_BASE);
}

static ssize_t features_show(struct kobject *kobj,
			     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf,
			  "identity=1\n"
			  "charging_api=1\n"
			  "charge_limit=api_only\n"
			  "watt_mode=api_only\n"
			  "bypass=0\n"
			  "performance=0\n"
			  "touch=0\n");
}


static ssize_t limit_enabled_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	int val;

	mutex_lock(&evonix_charging_lock);
	val = evonix_limit_enabled;
	mutex_unlock(&evonix_charging_lock);

	return sysfs_emit(buf, "%d\n", val);
}

static ssize_t limit_enabled_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 0, &val))
		return -EINVAL;

	if (val != 0 && val != 1)
		return -EINVAL;

	mutex_lock(&evonix_charging_lock);
	evonix_limit_enabled = val;
	mutex_unlock(&evonix_charging_lock);

	return count;
}

static ssize_t limit_percent_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	int val;

	mutex_lock(&evonix_charging_lock);
	val = evonix_limit_percent;
	mutex_unlock(&evonix_charging_lock);

	return sysfs_emit(buf, "%d\n", val);
}

static ssize_t limit_percent_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 0, &val))
		return -EINVAL;

	if (val < 1 || val > 100)
		return -EINVAL;

	mutex_lock(&evonix_charging_lock);
	evonix_limit_percent = val;
	mutex_unlock(&evonix_charging_lock);

	return count;
}

static ssize_t watt_mode_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	char mode[16];

	mutex_lock(&evonix_charging_lock);
	strscpy(mode, evonix_watt_mode, sizeof(mode));
	mutex_unlock(&evonix_charging_lock);

	return sysfs_emit(buf, "%s\n", mode);
}

static ssize_t watt_mode_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	const char *mode = NULL;

	if (sysfs_streq(buf, "dynamic"))
		mode = "dynamic";
	else if (sysfs_streq(buf, "33"))
		mode = "33";
	else if (sysfs_streq(buf, "45"))
		mode = "45";
	else if (sysfs_streq(buf, "65"))
		mode = "65";
	else if (sysfs_streq(buf, "90"))
		mode = "90";
	else
		return -EINVAL;

	mutex_lock(&evonix_charging_lock);
	strscpy(evonix_watt_mode, mode, sizeof(evonix_watt_mode));
	mutex_unlock(&evonix_charging_lock);

	return count;
}

static ssize_t charging_status_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	int enabled;
	int percent;
	char mode[16];

	mutex_lock(&evonix_charging_lock);
	enabled = evonix_limit_enabled;
	percent = evonix_limit_percent;
	strscpy(mode, evonix_watt_mode, sizeof(mode));
	mutex_unlock(&evonix_charging_lock);

	return sysfs_emit(buf,
			  "api=1\n"
			  "enforcement=0\n"
			  "limit_enabled=%d\n"
			  "limit_percent=%d\n"
			  "watt_mode=%s\n",
			  enabled, percent, mode);
}

static struct kobj_attribute limit_enabled_attr = __ATTR_RW(limit_enabled);
static struct kobj_attribute limit_percent_attr = __ATTR_RW(limit_percent);
static struct kobj_attribute watt_mode_attr = __ATTR_RW(watt_mode);
static struct kobj_attribute charging_status_attr =
	__ATTR(status, 0444, charging_status_show, NULL);

static struct attribute *evonix_charging_attrs[] = {
	&limit_enabled_attr.attr,
	&limit_percent_attr.attr,
	&watt_mode_attr.attr,
	&charging_status_attr.attr,
	NULL,
};

static const struct attribute_group evonix_charging_attr_group = {
	.attrs = evonix_charging_attrs,
};


static struct kobj_attribute api_version_attr = __ATTR_RO(api_version);
static struct kobj_attribute release_attr = __ATTR_RO(release);
static struct kobj_attribute maintainer_attr = __ATTR_RO(maintainer);
static struct kobj_attribute device_attr = __ATTR_RO(device);
static struct kobj_attribute base_attr = __ATTR_RO(base);
static struct kobj_attribute features_attr = __ATTR_RO(features);

static struct attribute *evonix_attrs[] = {
	&api_version_attr.attr,
	&release_attr.attr,
	&maintainer_attr.attr,
	&device_attr.attr,
	&base_attr.attr,
	&features_attr.attr,
	NULL,
};

static const struct attribute_group evonix_attr_group = {
	.attrs = evonix_attrs,
};

#define EVONIX_CTL_MAX_CMD	64

static bool evonix_valid_watt_mode(const char *mode);

static int evonix_ctl_format_status(char *buf, size_t size)
{
	int enabled;
	int percent;
	char watt[sizeof(evonix_watt_mode)];

	mutex_lock(&evonix_charging_lock);
	enabled = evonix_limit_enabled;
	percent = evonix_limit_percent;
	strscpy(watt, evonix_watt_mode, sizeof(watt));
	mutex_unlock(&evonix_charging_lock);

	return scnprintf(buf, size,
		"EVONIX control v1\n"
		"api=%s\n"
		"limit_enabled=%d\n"
		"limit_percent=%d\n"
		"watt_mode=%s\n"
		"commands:\n"
		"  limit_enabled=0|1\n"
		"  limit_percent=1..100\n"
		"  watt_mode=dynamic|33w|45w|65w|90w\n"
		"  reset\n",
		EVONIX_API_VERSION, enabled, percent, watt);
}

static ssize_t evonix_ctl_apply_command(const char *input, size_t count)
{
	char cmd[EVONIX_CTL_MAX_CMD];
	char *val;
	int ret;
	int tmp;

	if (!count)
		return 0;

	if (count >= sizeof(cmd))
		return -EINVAL;

	strscpy(cmd, input, sizeof(cmd));
	strim(cmd);

	if (!strcmp(cmd, "reset")) {
		mutex_lock(&evonix_charging_lock);
		evonix_limit_enabled = 0;
		evonix_limit_percent = 100;
		strscpy(evonix_watt_mode, "dynamic", sizeof(evonix_watt_mode));
		mutex_unlock(&evonix_charging_lock);

		pr_info("EVONIX: ctl reset\n");
		return count;
	}

	val = strchr(cmd, '=');
	if (!val)
		return -EINVAL;

	*val++ = '\0';
	strim(cmd);
	strim(val);

	if (!strcmp(cmd, "limit_enabled")) {
		ret = kstrtoint(val, 10, &tmp);
		if (ret)
			return ret;

		if (tmp != 0 && tmp != 1)
			return -EINVAL;

		mutex_lock(&evonix_charging_lock);
		evonix_limit_enabled = tmp;
		mutex_unlock(&evonix_charging_lock);

		pr_info("EVONIX: ctl limit_enabled=%d\n", tmp);
		return count;
	}

	if (!strcmp(cmd, "limit_percent")) {
		ret = kstrtoint(val, 10, &tmp);
		if (ret)
			return ret;

		if (tmp < 1 || tmp > 100)
			return -EINVAL;

		mutex_lock(&evonix_charging_lock);
		evonix_limit_percent = tmp;
		mutex_unlock(&evonix_charging_lock);

		pr_info("EVONIX: ctl limit_percent=%d\n", tmp);
		return count;
	}

	if (!strcmp(cmd, "watt_mode")) {
		if (!evonix_valid_watt_mode(val))
			return -EINVAL;

		mutex_lock(&evonix_charging_lock);
		strscpy(evonix_watt_mode, val, sizeof(evonix_watt_mode));
		mutex_unlock(&evonix_charging_lock);

		pr_info("EVONIX: ctl watt_mode=%s\n", val);
		return count;
	}

	return -EINVAL;
}

static int evonix_ctl_show(struct seq_file *m, void *v)
{
	char buf[512];

	evonix_ctl_format_status(buf, sizeof(buf));
	seq_puts(m, buf);

	return 0;
}

static int evonix_ctl_open(struct inode *inode, struct file *file)
{
	return single_open(file, evonix_ctl_show, NULL);
}

static bool evonix_valid_watt_mode(const char *mode)
{
	return sysfs_streq(mode, "dynamic") ||
	       sysfs_streq(mode, "33w") ||
	       sysfs_streq(mode, "45w") ||
	       sysfs_streq(mode, "65w") ||
	       sysfs_streq(mode, "90w");
}

static ssize_t evonix_ctl_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	char cmd[EVONIX_CTL_MAX_CMD];

	if (!count)
		return 0;

	if (count >= sizeof(cmd))
		return -EINVAL;

	if (copy_from_user(cmd, ubuf, count))
		return -EFAULT;

	cmd[count] = '\0';

	return evonix_ctl_apply_command(cmd, count);
}

static const struct proc_ops evonix_ctl_proc_ops = {
	.proc_open = evonix_ctl_open,
	.proc_read = seq_read,
	.proc_write = evonix_ctl_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static ssize_t evonix_dev_read(struct file *file, char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	char buf[512];
	int len;

	len = evonix_ctl_format_status(buf, sizeof(buf));

	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static ssize_t evonix_dev_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	char cmd[EVONIX_CTL_MAX_CMD];

	if (!count)
		return 0;

	if (count >= sizeof(cmd))
		return -EINVAL;

	if (copy_from_user(cmd, ubuf, count))
		return -EFAULT;

	cmd[count] = '\0';

	return evonix_ctl_apply_command(cmd, count);
}

static const struct file_operations evonix_dev_fops = {
	.owner = THIS_MODULE,
	.read = evonix_dev_read,
	.write = evonix_dev_write,
	.llseek = no_llseek,
};

static struct miscdevice evonix_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "evonix_ctl",
	.fops = &evonix_dev_fops,
	.mode = 0666,
};

static int __init evonix_core_init(void)
{
	int ret;

	evonix_kobj = kobject_create_and_add("evonix", kernel_kobj);
	if (!evonix_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(evonix_kobj, &evonix_attr_group);
	if (ret) {
		kobject_put(evonix_kobj);
		evonix_kobj = NULL;
		return ret;
	}

	evonix_charging_kobj = kobject_create_and_add("charging", evonix_kobj);
	if (!evonix_charging_kobj) {
		sysfs_remove_group(evonix_kobj, &evonix_attr_group);
		kobject_put(evonix_kobj);
		evonix_kobj = NULL;
		return -ENOMEM;
	}

	ret = sysfs_create_group(evonix_charging_kobj, &evonix_charging_attr_group);
	if (ret) {
		kobject_put(evonix_charging_kobj);
		evonix_charging_kobj = NULL;
		sysfs_remove_group(evonix_kobj, &evonix_attr_group);
		kobject_put(evonix_kobj);
		evonix_kobj = NULL;
		return ret;
	}

	evonix_ctl_proc = proc_create("evonix_ctl", 0666, NULL,
		&evonix_ctl_proc_ops);
	if (!evonix_ctl_proc)
		pr_warn("EVONIX: failed to create /proc/evonix_ctl\n");
	else
		pr_info("EVONIX: /proc/evonix_ctl created\n");

	ret = misc_register(&evonix_miscdev);
	if (ret)
		pr_warn("EVONIX: failed to register /dev/evonix_ctl ret=%d\n", ret);
	else
		pr_info("EVONIX: /dev/evonix_ctl registered\n");

	pr_info("EVONIX: core sysfs API v%s initialized\n", EVONIX_API_VERSION);
	return 0;
}

late_initcall(evonix_core_init);
