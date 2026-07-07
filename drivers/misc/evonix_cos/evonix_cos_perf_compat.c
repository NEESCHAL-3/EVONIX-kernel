// SPDX-License-Identifier: GPL-2.0
/*
 * EVONIX ColorOS performance compatibility nodes.
 *
 * Rule: no dummy silence. Nodes either expose real kernel task CPU stats,
 * or record real watchdog heartbeat writes from userspace.
 */

#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/cputime.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>

#define EVX_PERF_NAME "evonix_cos_perf_compat"

static struct proc_dir_entry *proc_task_cpustats;
static struct proc_dir_entry *proc_oplus_init_watchdog_dir;
static struct proc_dir_entry *proc_oplus_init_watchdog_kick;
static struct proc_dir_entry *proc_oplus_init_watchdog_status;

static atomic64_t watchdog_kick_count;
static u64 watchdog_last_kick_ns;
static char watchdog_last_payload[64];

static int task_cpustats_proc_show(struct seq_file *m, void *v)
{
	struct task_struct *g;
	struct task_struct *t;

	/*
	 * Real-backed task CPU stats.
	 * Output is intentionally simple and parse-safe:
	 * pid tgid utime_ns stime_ns runtime_ns comm
	 */
	seq_puts(m, "pid tgid utime_ns stime_ns runtime_ns comm\n");

	rcu_read_lock();
	for_each_process_thread(g, t) {
		u64 utime = 0;
		u64 stime = 0;
		u64 runtime = 0;

		task_cputime_adjusted(t, &utime, &stime);
		runtime = task_sched_runtime(t);

		seq_printf(m, "%d %d %llu %llu %llu %.64s\n",
			   task_pid_nr(t),
			   task_tgid_nr(t),
			   (unsigned long long)utime,
			   (unsigned long long)stime,
			   (unsigned long long)runtime,
			   t->comm);
	}
	rcu_read_unlock();

	return 0;
}

static int task_cpustats_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, task_cpustats_proc_show, NULL);
}

static const struct proc_ops task_cpustats_proc_ops = {
	.proc_open	= task_cpustats_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static ssize_t watchdog_kick_proc_write(struct file *file,
					const char __user *buf,
					size_t count, loff_t *ppos)
{
	size_t len;

	len = min(count, sizeof(watchdog_last_payload) - 1);
	if (copy_from_user(watchdog_last_payload, buf, len))
		return -EFAULT;

	watchdog_last_payload[len] = '\0';
	watchdog_last_kick_ns = ktime_get_boottime_ns();
	atomic64_inc(&watchdog_kick_count);

	return count;
}

static int watchdog_status_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "kick_count=%lld\n",
		   (long long)atomic64_read(&watchdog_kick_count));
	seq_printf(m, "last_kick_boottime_ns=%llu\n",
		   (unsigned long long)watchdog_last_kick_ns);
	seq_printf(m, "last_payload=%s\n", watchdog_last_payload);
	return 0;
}

static int watchdog_status_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, watchdog_status_proc_show, NULL);
}

static const struct proc_ops watchdog_kick_proc_ops = {
	.proc_write	= watchdog_kick_proc_write,
};

static const struct proc_ops watchdog_status_proc_ops = {
	.proc_open	= watchdog_status_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int __init evx_cos_perf_compat_init(void)
{
	proc_task_cpustats = proc_create("task_cpustats", 0444, NULL,
					 &task_cpustats_proc_ops);
	if (!proc_task_cpustats)
		return -ENOMEM;

	proc_oplus_init_watchdog_dir = proc_mkdir("oplus_init_watchdog", NULL);
	if (!proc_oplus_init_watchdog_dir)
		goto err_task_cpustats;

	proc_oplus_init_watchdog_kick =
		proc_create("kick", 0666, proc_oplus_init_watchdog_dir,
			    &watchdog_kick_proc_ops);
	if (!proc_oplus_init_watchdog_kick)
		goto err_watchdog_dir;

	proc_oplus_init_watchdog_status =
		proc_create("status", 0444, proc_oplus_init_watchdog_dir,
			    &watchdog_status_proc_ops);
	if (!proc_oplus_init_watchdog_status)
		goto err_watchdog_kick;

	pr_info(EVX_PERF_NAME ": loaded real-backed task stats and watchdog nodes\n");
	return 0;

err_watchdog_kick:
	proc_remove(proc_oplus_init_watchdog_kick);
err_watchdog_dir:
	proc_remove(proc_oplus_init_watchdog_dir);
err_task_cpustats:
	proc_remove(proc_task_cpustats);
	return -ENOMEM;
}

static void __exit evx_cos_perf_compat_exit(void)
{
	if (proc_oplus_init_watchdog_status)
		proc_remove(proc_oplus_init_watchdog_status);
	if (proc_oplus_init_watchdog_kick)
		proc_remove(proc_oplus_init_watchdog_kick);
	if (proc_oplus_init_watchdog_dir)
		proc_remove(proc_oplus_init_watchdog_dir);
	if (proc_task_cpustats)
		proc_remove(proc_task_cpustats);
}

module_init(evx_cos_perf_compat_init);
module_exit(evx_cos_perf_compat_exit);

MODULE_DESCRIPTION("EVONIX ColorOS real-backed performance compatibility nodes");
MODULE_AUTHOR("EVONIX");
MODULE_LICENSE("GPL");
