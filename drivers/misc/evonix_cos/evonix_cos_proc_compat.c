#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

static ssize_t evx_read_one(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	const char *s = "1\n";
	return simple_read_from_buffer(buf, count, ppos, s, 2);
}

static ssize_t evx_read_zero(struct file *file, char __user *buf,
			     size_t count, loff_t *ppos)
{
	const char *s = "0\n";
	return simple_read_from_buffer(buf, count, ppos, s, 2);
}

static ssize_t evx_write_ok(struct file *file, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	return count;
}

static long evx_ioctl_ok(struct file *file, unsigned int cmd, unsigned long arg)
{
	return 0;
}

static const struct proc_ops evx_one_rw_ops = {
	.proc_read  = evx_read_one,
	.proc_write = evx_write_ok,
	.proc_ioctl = evx_ioctl_ok,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = evx_ioctl_ok,
#endif
};

static const struct proc_ops evx_zero_rw_ops = {
	.proc_read  = evx_read_zero,
	.proc_write = evx_write_ok,
	.proc_ioctl = evx_ioctl_ok,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = evx_ioctl_ok,
#endif
};

static void evx_proc_touch(const char *path, umode_t mode,
			   const struct proc_ops *ops)
{
	proc_create(path, mode, NULL, ops);
}

static int __init evx_cos_proc_compat_init(void)
{
	struct proc_dir_entry *task_info;
	struct proc_dir_entry *task_cpustats;
	struct proc_dir_entry *healthinfo;

	task_info = proc_mkdir("task_info", NULL);
	if (task_info) {
		task_cpustats = proc_mkdir("task_cpustats", task_info);
		if (task_cpustats)
			proc_create("task_cpustats_enable", 0666,
				    task_cpustats, &evx_one_rw_ops);
	}

	healthinfo = proc_mkdir("oplus_healthinfo", NULL);
	if (healthinfo)
		proc_create("alloc_wait", 0666, healthinfo, &evx_zero_rw_ops);

	/* OPlus SchedAssist native sharedFd compatibility */
	proc_mkdir("oplus_scheduler", NULL);
	proc_mkdir("oplus_scheduler/sched_assist", NULL);
	proc_mkdir("oplus_scheduler/sched_assist/audio", NULL);

	evx_proc_touch("oplus_scheduler/sched_assist/ux_task",
		       0666, &evx_zero_rw_ops);
	evx_proc_touch("oplus_scheduler/sched_assist/ux_task_app",
		       0666, &evx_zero_rw_ops);
	evx_proc_touch("oplus_scheduler/sched_assist/audio/status",
		       0666, &evx_one_rw_ops);

	/* OPlus frame boost compatibility */
	proc_mkdir("oplus_frame_boost", NULL);
	evx_proc_touch("oplus_frame_boost/sys_ctrl", 0666, &evx_zero_rw_ops);
	evx_proc_touch("oplus_frame_boost/ctrl", 0666, &evx_zero_rw_ops);

	/* Some OPlus libs probe /proc/sys/fbg/ as sharedFd base */
	proc_mkdir("sys/fbg", NULL);

	pr_info("EVONIX-COS: proc compat v30 task_cpustats, healthinfo, schedassist, frameboost ready\n");
	return 0;
}
late_initcall(evx_cos_proc_compat_init);
