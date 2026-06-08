// SPDX-License-Identifier: GPL-2.0
/*
 * EVONIX core sysfs API
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

#define EVONIX_API_VERSION	"1"
#define EVONIX_RELEASE		"v3.0"
#define EVONIX_MAINTAINER	"Neeschal"
#define EVONIX_DEVICE		"POCO X7 Pro / rodin"
#define EVONIX_BASE		"6.6 LTS"

static struct kobject *evonix_kobj;

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
			  "charging=0\n"
			  "watt_mode=0\n"
			  "bypass=0\n"
			  "performance=0\n"
			  "touch=0\n");
}

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

	pr_info("EVONIX: core sysfs API v%s initialized\n", EVONIX_API_VERSION);
	return 0;
}

late_initcall(evonix_core_init);
