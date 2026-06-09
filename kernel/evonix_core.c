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
#include <linux/workqueue.h>
#include <linux/umh.h>
#include <linux/jiffies.h>

#define EVONIX_API_VERSION	"1"
#define EVONIX_RELEASE		"v3.0"
#define EVONIX_MAINTAINER	"Neeschal"
#define EVONIX_DEVICE		"POCO X7 Pro / rodin"
#define EVONIX_BASE		"6.6 LTS"

static struct kobject *evonix_kobj;
static struct kobject *evonix_charging_kobj;

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

static void evonix_boot_helper_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(evonix_boot_helper_dwork, evonix_boot_helper_work);

static ssize_t boot_helper_trigger_store(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 const char *buf, size_t count)
{
	if (sysfs_streq(buf, "1")) {
		pr_info("EVONIX: manual boot helper trigger requested\n");
		schedule_delayed_work(&evonix_boot_helper_dwork, 0);
	}

	return count;
}

static struct kobj_attribute boot_helper_trigger_attr =
	__ATTR_WO(boot_helper_trigger);

static struct attribute *evonix_attrs[] = {
	&api_version_attr.attr,
	&release_attr.attr,
	&maintainer_attr.attr,
	&device_attr.attr,
	&base_attr.attr,
	&features_attr.attr,
	&boot_helper_trigger_attr.attr,
	NULL,
};

static const struct attribute_group evonix_attr_group = {
	.attrs = evonix_attrs,
};

#define EVONIX_BOOT_HELPER_DELAY_SEC	30
#define EVONIX_BOOT_HELPER_SCRIPT		"/data/evonix/bin/evonix_boot_probe.sh"

static void evonix_boot_helper_work(struct work_struct *work)
{
	static char *argv[] = {
		"/system/bin/sh",
		EVONIX_BOOT_HELPER_SCRIPT,
		NULL,
	};
	static char *envp[] = {
		"HOME=/",
		"PATH=/sbin:/system/bin:/system/xbin:/vendor/bin:/product/bin:/system_ext/bin:/odm/bin",
		NULL,
	};
	int ret;

	ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
	pr_info("EVONIX: boot helper probe finished ret=%d script=%s\n",
		ret, EVONIX_BOOT_HELPER_SCRIPT);
}

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

	pr_info("EVONIX: core sysfs API v%s initialized\n", EVONIX_API_VERSION);

	schedule_delayed_work(&evonix_boot_helper_dwork,
		EVONIX_BOOT_HELPER_DELAY_SEC * HZ);

	return 0;
}

late_initcall(evonix_core_init);
