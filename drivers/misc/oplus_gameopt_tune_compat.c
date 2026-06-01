// SPDX-License-Identifier: GPL-2.0
/*
 * OPlus GameOpt scheduler tuning compatibility for ColorOS.
 *
 * Provides safe writable compatibility nodes seen in GameOpt logs:
 *   /proc/sys/walt/sched_auto_penalty
 *   /sys/kernel/sched_penalty/parameters/target_pid
 *   /sys/kernel/sched_penalty/parameters/fps
 *   /proc/hmbird_sched/parctrl_high_ratio
 *   /proc/hmbird_sched/parctrl_low_ratio
 *   /proc/sys/oplus_sched_ext/scx_enable
 *   /proc/sys/oplus_sched_ext/scx_shadow_tick_enable
 *   /proc/sys/oplus_sched_ext/sched_ravg_window_frame_per_sec
 *   /proc/oplus_gameopt_tune/status
 *
 * Stage-1: accept/store values only. No fake Qualcomm KGSL paths.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/sysctl.h>
#include <linux/kobject.h>
#include <linux/string.h>

#define EVONIX_GAMEOPT_TUNE_BUILD "EVONIX-OPlusGameOptTune-v14"

static DEFINE_MUTEX(tune_lock);

static struct ctl_table_header *walt_sysctl_hdr;
static struct ctl_table_header *oplus_sched_ext_sysctl_hdr;

static struct proc_dir_entry *hmbird_dir;
static struct proc_dir_entry *hmbird_high_entry;
static struct proc_dir_entry *hmbird_low_entry;
static struct proc_dir_entry *status_dir;
static struct proc_dir_entry *status_entry;

static struct kobject *sched_penalty_kobj;
static struct kobject *sched_penalty_params_kobj;

static int sched_auto_penalty;
static int scx_enable;
static int scx_shadow_tick_enable;
static int sched_ravg_window_frame_per_sec;

static int penalty_target_pid;
static int penalty_fps;

static int parctrl_high_ratio;
static int parctrl_low_ratio;

static unsigned long parctrl_high_write_count;
static unsigned long parctrl_low_write_count;
static unsigned long penalty_target_pid_write_count;
static unsigned long penalty_fps_write_count;

static struct ctl_table walt_sysctl_table[] = {
	{
		.procname = "sched_auto_penalty",
		.data = &sched_auto_penalty,
		.maxlen = sizeof(int),
		.mode = 0666,
		.proc_handler = proc_dointvec,
	},
	{}
};

static struct ctl_table oplus_sched_ext_sysctl_table[] = {
	{
		.procname = "scx_enable",
		.data = &scx_enable,
		.maxlen = sizeof(int),
		.mode = 0666,
		.proc_handler = proc_dointvec,
	},
	{
		.procname = "scx_shadow_tick_enable",
		.data = &scx_shadow_tick_enable,
		.maxlen = sizeof(int),
		.mode = 0666,
		.proc_handler = proc_dointvec,
	},
	{
		.procname = "sched_ravg_window_frame_per_sec",
		.data = &sched_ravg_window_frame_per_sec,
		.maxlen = sizeof(int),
		.mode = 0666,
		.proc_handler = proc_dointvec,
	},
	{}
};

static ssize_t target_pid_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", penalty_target_pid);
}

static ssize_t target_pid_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	int val;

	if (!kstrtoint(buf, 0, &val)) {
		mutex_lock(&tune_lock);
		penalty_target_pid = val;
		penalty_target_pid_write_count++;
		mutex_unlock(&tune_lock);
	}

	return count;
}

static ssize_t fps_show(struct kobject *kobj,
			struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", penalty_fps);
}

static ssize_t fps_store(struct kobject *kobj,
			 struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	int val;

	if (!kstrtoint(buf, 0, &val)) {
		mutex_lock(&tune_lock);
		penalty_fps = val;
		penalty_fps_write_count++;
		mutex_unlock(&tune_lock);
	}

	return count;
}

static struct kobj_attribute target_pid_attr =
	__ATTR(target_pid, 0644, target_pid_show, target_pid_store);

static struct kobj_attribute fps_attr =
	__ATTR(fps, 0644, fps_show, fps_store);

static ssize_t hmbird_write_int(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos, bool high)
{
	char buf[32];
	size_t len = min_t(size_t, count, sizeof(buf) - 1);
	int val;

	memset(buf, 0, sizeof(buf));

	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;

	buf[len] = '\0';
	strim(buf);

	if (!kstrtoint(buf, 0, &val)) {
		mutex_lock(&tune_lock);
		if (high) {
			parctrl_high_ratio = val;
			parctrl_high_write_count++;
		} else {
			parctrl_low_ratio = val;
			parctrl_low_write_count++;
		}
		mutex_unlock(&tune_lock);
	}

	return count;
}

static int hmbird_high_show(struct seq_file *m, void *v)
{
	mutex_lock(&tune_lock);
	seq_printf(m, "%d\n", parctrl_high_ratio);
	mutex_unlock(&tune_lock);
	return 0;
}

static int hmbird_low_show(struct seq_file *m, void *v)
{
	mutex_lock(&tune_lock);
	seq_printf(m, "%d\n", parctrl_low_ratio);
	mutex_unlock(&tune_lock);
	return 0;
}

static int hmbird_high_open(struct inode *inode, struct file *file)
{
	return single_open(file, hmbird_high_show, NULL);
}

static int hmbird_low_open(struct inode *inode, struct file *file)
{
	return single_open(file, hmbird_low_show, NULL);
}

static ssize_t hmbird_high_write(struct file *file, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	return hmbird_write_int(file, ubuf, count, ppos, true);
}

static ssize_t hmbird_low_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	return hmbird_write_int(file, ubuf, count, ppos, false);
}

static const struct proc_ops hmbird_high_ops = {
	.proc_open = hmbird_high_open,
	.proc_read = seq_read,
	.proc_write = hmbird_high_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops hmbird_low_ops = {
	.proc_open = hmbird_low_open,
	.proc_read = seq_read,
	.proc_write = hmbird_low_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int tune_status_show(struct seq_file *m, void *v)
{
	mutex_lock(&tune_lock);

	seq_printf(m, "build=%s\n", EVONIX_GAMEOPT_TUNE_BUILD);
	seq_printf(m, "sched_auto_penalty=%d\n", sched_auto_penalty);
	seq_printf(m, "penalty_target_pid=%d\n", penalty_target_pid);
	seq_printf(m, "penalty_fps=%d\n", penalty_fps);
	seq_printf(m, "penalty_target_pid_write_count=%lu\n",
		   penalty_target_pid_write_count);
	seq_printf(m, "penalty_fps_write_count=%lu\n",
		   penalty_fps_write_count);
	seq_printf(m, "parctrl_high_ratio=%d\n", parctrl_high_ratio);
	seq_printf(m, "parctrl_low_ratio=%d\n", parctrl_low_ratio);
	seq_printf(m, "parctrl_high_write_count=%lu\n",
		   parctrl_high_write_count);
	seq_printf(m, "parctrl_low_write_count=%lu\n",
		   parctrl_low_write_count);
	seq_printf(m, "scx_enable=%d\n", scx_enable);
	seq_printf(m, "scx_shadow_tick_enable=%d\n",
		   scx_shadow_tick_enable);
	seq_printf(m, "sched_ravg_window_frame_per_sec=%d\n",
		   sched_ravg_window_frame_per_sec);

	mutex_unlock(&tune_lock);

	return 0;
}

static int tune_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, tune_status_show, NULL);
}

static const struct proc_ops tune_status_ops = {
	.proc_open = tune_status_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init oplus_gameopt_tune_compat_init(void)
{
	int ret;

	walt_sysctl_hdr = register_sysctl("walt", walt_sysctl_table);
	if (!walt_sysctl_hdr)
		pr_warn("oplus_gameopt_tune_compat: failed to register /proc/sys/walt\n");

	oplus_sched_ext_sysctl_hdr =
		register_sysctl("oplus_sched_ext", oplus_sched_ext_sysctl_table);
	if (!oplus_sched_ext_sysctl_hdr)
		pr_warn("oplus_gameopt_tune_compat: failed to register /proc/sys/oplus_sched_ext\n");

	hmbird_dir = proc_mkdir("hmbird_sched", NULL);
	if (hmbird_dir) {
		hmbird_high_entry = proc_create("parctrl_high_ratio", 0666,
						hmbird_dir, &hmbird_high_ops);
		hmbird_low_entry = proc_create("parctrl_low_ratio", 0666,
					       hmbird_dir, &hmbird_low_ops);
	}

	status_dir = proc_mkdir("oplus_gameopt_tune", NULL);
	if (status_dir)
		status_entry = proc_create("status", 0444, status_dir,
					   &tune_status_ops);

	sched_penalty_kobj = kobject_create_and_add("sched_penalty", kernel_kobj);
	if (sched_penalty_kobj) {
		sched_penalty_params_kobj =
			kobject_create_and_add("parameters", sched_penalty_kobj);
		if (sched_penalty_params_kobj) {
			ret = sysfs_create_file(sched_penalty_params_kobj,
						&target_pid_attr.attr);
			if (ret)
				pr_warn("oplus_gameopt_tune_compat: target_pid sysfs failed\n");

			ret = sysfs_create_file(sched_penalty_params_kobj,
						&fps_attr.attr);
			if (ret)
				pr_warn("oplus_gameopt_tune_compat: fps sysfs failed\n");
		}
	}

	pr_info("oplus_gameopt_tune_compat: active %s\n",
		EVONIX_GAMEOPT_TUNE_BUILD);

	return 0;
}

static void __exit oplus_gameopt_tune_compat_exit(void)
{
	if (sched_penalty_params_kobj) {
		sysfs_remove_file(sched_penalty_params_kobj,
				  &target_pid_attr.attr);
		sysfs_remove_file(sched_penalty_params_kobj, &fps_attr.attr);
		kobject_put(sched_penalty_params_kobj);
	}

	if (sched_penalty_kobj)
		kobject_put(sched_penalty_kobj);

	if (status_entry)
		proc_remove(status_entry);
	if (status_dir)
		proc_remove(status_dir);

	if (hmbird_high_entry)
		proc_remove(hmbird_high_entry);
	if (hmbird_low_entry)
		proc_remove(hmbird_low_entry);
	if (hmbird_dir)
		proc_remove(hmbird_dir);

	if (walt_sysctl_hdr)
		unregister_sysctl_table(walt_sysctl_hdr);
	if (oplus_sched_ext_sysctl_hdr)
		unregister_sysctl_table(oplus_sched_ext_sysctl_hdr);

	pr_info("oplus_gameopt_tune_compat: removed\n");
}

module_init(oplus_gameopt_tune_compat_init);
module_exit(oplus_gameopt_tune_compat_exit);

MODULE_DESCRIPTION("OPlus GameOpt scheduler tuning compatibility");
MODULE_AUTHOR("NEESCHAL");
MODULE_LICENSE("GPL");
