// SPDX-License-Identifier: GPL-2.0
/*
 * OPlus Binder UX compatibility layer for ColorOS bringup.
 *
 * ColorOS SchedAssist expects:
 *   /proc/oplus_binder/ux_flag
 *
 * Native libSchedAssistExtImpl.so calls ioctl(fd, 0x407a6900, int[2])
 * where arg is:
 *   int data[2] = { tid, flag };
 *
 * Real v1 behavior:
 * - Creates /proc/oplus_binder/ux_flag
 * - Accepts Binder UX ioctl from ColorOS userspace
 * - Applies real Linux scheduler nice boost to the target TID
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/task.h>

#define OPLUS_BINDER_UX_IOCTL	0x407a6900

#define OPLUS_BINDER_UX_NONE	0
#define OPLUS_BINDER_UX_LIGHT	1
#define OPLUS_BINDER_UX_STRONG	2

#define OPLUS_NICE_NORMAL	0
#define OPLUS_NICE_LIGHT	(-5)
#define OPLUS_NICE_STRONG	(-10)

struct oplus_binder_ux_data {
	int tid;
	int flag;
};

static struct proc_dir_entry *oplus_binder_dir;
static struct proc_dir_entry *ux_flag_entry;

static int oplus_binder_ux_nice_from_flag(int flag)
{
	switch (flag) {
	case OPLUS_BINDER_UX_NONE:
		return OPLUS_NICE_NORMAL;
	case OPLUS_BINDER_UX_LIGHT:
		return OPLUS_NICE_LIGHT;
	case OPLUS_BINDER_UX_STRONG:
		return OPLUS_NICE_STRONG;
	default:
		return OPLUS_NICE_LIGHT;
	}
}

static int oplus_binder_ux_apply(int tid, int flag)
{
	struct task_struct *task;
	struct pid *kpid = NULL;
	int nice;
	int target_tid = tid;

	if (flag < 0 || flag > 2)
		return -EINVAL;

	nice = oplus_binder_ux_nice_from_flag(flag);

	/*
	 * OPlus userspace may pass tid = -1 for "current/calling thread".
	 * Do not reject it; apply the Binder UX hint to the ioctl caller.
	 */
	if (tid <= 0) {
		task = current;
		target_tid = task_pid_nr(task);

		set_user_nice(task, nice);

		pr_debug("oplus_binder_ux: current tid=%d comm=%s flag=%d nice=%d\n",
			 target_tid, task->comm, flag, nice);
		return 0;
	}

	rcu_read_lock();
	kpid = find_get_pid(tid);
	task = kpid ? get_pid_task(kpid, PIDTYPE_PID) : NULL;
	rcu_read_unlock();

	if (!task) {
		if (kpid)
			put_pid(kpid);
		return -ESRCH;
	}

	set_user_nice(task, nice);

	pr_debug("oplus_binder_ux: tid=%d comm=%s flag=%d nice=%d\n",
		 tid, task->comm, flag, nice);

	put_task_struct(task);
	put_pid(kpid);

	return 0;
}

static int oplus_binder_ux_show(struct seq_file *m, void *v)
{
	seq_puts(m, "0\n");
	return 0;
}

static int oplus_binder_ux_open(struct inode *inode, struct file *file)
{
	return single_open(file, oplus_binder_ux_show, NULL);
}

static ssize_t oplus_binder_ux_write(struct file *file,
				     const char __user *ubuf,
				     size_t count, loff_t *ppos)
{
	char buf[64];
	int tid = 0;
	int flag = 1;
	size_t len;

	len = min_t(size_t, count, sizeof(buf) - 1);
	memset(buf, 0, sizeof(buf));

	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;

	buf[len] = '\0';

	if (sscanf(buf, "%d %d", &tid, &flag) >= 1)
		oplus_binder_ux_apply(tid, flag);

	return count;
}

static long oplus_binder_ux_ioctl(struct file *file,
				  unsigned int cmd, unsigned long arg)
{
	struct oplus_binder_ux_data data;

	if (cmd != OPLUS_BINDER_UX_IOCTL)
		return -ENOTTY;

	if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
		return -EFAULT;

	return oplus_binder_ux_apply(data.tid, data.flag);
}

static const struct proc_ops oplus_binder_ux_ops = {
	.proc_open		= oplus_binder_ux_open,
	.proc_read		= seq_read,
	.proc_write		= oplus_binder_ux_write,
	.proc_ioctl		= oplus_binder_ux_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl	= oplus_binder_ux_ioctl,
#endif
	.proc_lseek		= seq_lseek,
	.proc_release		= single_release,
};

static int __init oplus_binder_ux_init(void)
{
	oplus_binder_dir = proc_mkdir("oplus_binder", NULL);
	if (!oplus_binder_dir)
		return -ENOMEM;

	ux_flag_entry = proc_create("ux_flag", 0666, oplus_binder_dir,
				    &oplus_binder_ux_ops);
	if (!ux_flag_entry) {
		proc_remove(oplus_binder_dir);
		return -ENOMEM;
	}

	pr_info("oplus_binder_ux: /proc/oplus_binder/ux_flag active\n");
	return 0;
}

static void __exit oplus_binder_ux_exit(void)
{
	if (ux_flag_entry)
		proc_remove(ux_flag_entry);
	if (oplus_binder_dir)
		proc_remove(oplus_binder_dir);

	pr_info("oplus_binder_ux: removed\n");
}

module_init(oplus_binder_ux_init);
module_exit(oplus_binder_ux_exit);

MODULE_DESCRIPTION("OPlus Binder UX compatibility for ColorOS");
MODULE_AUTHOR("NEESCHAL");
MODULE_LICENSE("GPL");
