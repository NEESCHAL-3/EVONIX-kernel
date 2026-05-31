// SPDX-License-Identifier: GPL-2.0
/*
 * OPlus QoS scheduler compatibility layer for ColorOS.
 *
 * ColorOS qos_sched.so expects:
 *   /proc/oplus_qos_sched/qos_lut
 *   /proc/oplus_qos_sched/qos_level
 *
 * Real compat behavior:
 * - accepts QoS LUT update ioctl and provides mmap buffer
 * - accepts TID/PID/TID-array QoS level ioctls
 * - maps QoS levels to real Linux scheduler nice boost safely
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/types.h>

#define OPLUS_QOS_IOCTL_UPDATE_LUT  0x40107001
#define OPLUS_QOS_IOCTL_SET_TID     0x41087101
#define OPLUS_QOS_IOCTL_SET_PID     0x41087102
#define OPLUS_QOS_IOCTL_SET_TID_ARR 0x41087103

#define OPLUS_QOS_MAX_TYPES         8
#define OPLUS_QOS_MAX_LUT_BYTES     (128 * 1024)
#define OPLUS_QOS_MAX_TIDS          64

struct oplus_qos_lut_user {
	__s32 bytes;
	__s32 type;
	__u64 user_ptr;
};

struct oplus_qos_one_user {
	__s32 level_type;
	__s32 id;
};

struct oplus_qos_tid_array_user {
	__s32 level;
	__s32 count;
	__s32 tids[OPLUS_QOS_MAX_TIDS];
};

struct oplus_qos_lut_slot {
	void *area;
	size_t size;
	int type;
};

static DEFINE_MUTEX(qos_lock);

static struct proc_dir_entry *qos_dir;
static struct proc_dir_entry *qos_lut_entry;
static struct proc_dir_entry *qos_level_entry;

static struct oplus_qos_lut_slot qos_luts[OPLUS_QOS_MAX_TYPES];
static int qos_last_lut_type;
static unsigned long qos_tid_apply_count;
static unsigned long qos_pid_apply_count;
static unsigned long qos_tid_array_apply_count;
static unsigned long qos_lut_update_count;

static int oplus_qos_level_to_nice_fallback(int level)
{
	if (level <= 0)
		return 0;
	if (level == 1)
		return -2;
	if (level == 2)
		return -4;
	if (level == 3)
		return -6;

	return -8;
}

static int oplus_qos_level_to_nice_from_lut(int type, int level)
{
	struct oplus_qos_lut_slot *slot;
	size_t off;
	int prio;

	if (type < 1 || type > OPLUS_QOS_MAX_TYPES)
		return oplus_qos_level_to_nice_fallback(level);

	if (level < 0)
		return 0;

	slot = &qos_luts[type - 1];

	if (!slot->area || !slot->size)
		return oplus_qos_level_to_nice_fallback(level);

	/*
	 * qos_sched.so treats each LUT item as 32 bytes.
	 * It logs fields at:
	 *   +0x10 = prio
	 *   +0x14 = uclamp_min
	 *   +0x18 = uclamp_max
	 *
	 * For now, only use prio if it is already a safe Linux nice value.
	 */
	off = ((size_t)level * 32) + 0x10;
	if (off + sizeof(prio) > slot->size)
		return oplus_qos_level_to_nice_fallback(level);

	memcpy(&prio, slot->area + off, sizeof(prio));

	if (prio >= -20 && prio <= 19)
		return prio;

	return oplus_qos_level_to_nice_fallback(level);
}

static int oplus_qos_apply_to_tid(int level, int tid)
{
	struct task_struct *task;
	struct pid *kpid;
	int nice;

	if (tid <= 0)
		return -ESRCH;

	nice = oplus_qos_level_to_nice_from_lut(1, level);

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

	put_task_struct(task);
	put_pid(kpid);

	return 0;
}

static int oplus_qos_apply_to_pid(int level_type, int pid)
{
	struct task_struct *task;
	struct pid *kpid;
	int level = level_type & 0x00ffffff;
	int nice;

	if (pid <= 0)
		return -ESRCH;

	nice = oplus_qos_level_to_nice_from_lut(2, level);

	rcu_read_lock();
	kpid = find_get_pid(pid);
	task = kpid ? get_pid_task(kpid, PIDTYPE_PID) : NULL;
	rcu_read_unlock();

	if (!task) {
		if (kpid)
			put_pid(kpid);
		return -ESRCH;
	}

	set_user_nice(task, nice);

	put_task_struct(task);
	put_pid(kpid);

	return 0;
}

static long oplus_qos_lut_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct oplus_qos_lut_user req;
	struct oplus_qos_lut_slot *slot;
	void *new_area;
	size_t size;

	if (cmd != OPLUS_QOS_IOCTL_UPDATE_LUT)
		return -ENOTTY;

	if (!arg)
		return -EINVAL;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (req.type < 1 || req.type > OPLUS_QOS_MAX_TYPES)
		return -EINVAL;

	if (req.bytes <= 0 || req.bytes > OPLUS_QOS_MAX_LUT_BYTES)
		return -EINVAL;

	size = PAGE_ALIGN(req.bytes);

	new_area = vmalloc_user(size);
	if (!new_area)
		return -ENOMEM;

	mutex_lock(&qos_lock);

	slot = &qos_luts[req.type - 1];

	if (slot->area)
		vfree(slot->area);

	slot->area = new_area;
	slot->size = size;
	slot->type = req.type;

	qos_last_lut_type = req.type;
	qos_lut_update_count++;

	mutex_unlock(&qos_lock);

	pr_info("oplus_qos_compat: lut update type=%d bytes=%d mapped=%zu\n",
		req.type, req.bytes, size);

	return 0;
}

static int oplus_qos_lut_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct oplus_qos_lut_slot *slot;
	size_t size = vma->vm_end - vma->vm_start;
	int ret;

	mutex_lock(&qos_lock);

	if (qos_last_lut_type < 1 || qos_last_lut_type > OPLUS_QOS_MAX_TYPES) {
		mutex_unlock(&qos_lock);
		return -ENODEV;
	}

	slot = &qos_luts[qos_last_lut_type - 1];

	if (!slot->area || !slot->size || size > slot->size) {
		mutex_unlock(&qos_lock);
		return -EINVAL;
	}

	ret = remap_vmalloc_range(vma, slot->area, 0);

	mutex_unlock(&qos_lock);

	return ret;
}

static long oplus_qos_level_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	struct oplus_qos_one_user one;
	struct oplus_qos_tid_array_user arr;
	int i;
	int ret = 0;

	if (!arg)
		return -EINVAL;

	switch (cmd) {
	case OPLUS_QOS_IOCTL_SET_TID:
		if (copy_from_user(&one, (void __user *)arg, sizeof(one)))
			return -EFAULT;

		ret = oplus_qos_apply_to_tid(one.level_type, one.id);
		if (!ret)
			qos_tid_apply_count++;
		return ret;

	case OPLUS_QOS_IOCTL_SET_PID:
		if (copy_from_user(&one, (void __user *)arg, sizeof(one)))
			return -EFAULT;

		ret = oplus_qos_apply_to_pid(one.level_type, one.id);
		if (!ret)
			qos_pid_apply_count++;
		return ret;

	case OPLUS_QOS_IOCTL_SET_TID_ARR:
		if (copy_from_user(&arr, (void __user *)arg, sizeof(arr)))
			return -EFAULT;

		if (arr.count < 1 || arr.count > OPLUS_QOS_MAX_TIDS)
			return -EINVAL;

		for (i = 0; i < arr.count; i++) {
			int r = oplus_qos_apply_to_tid(arr.level, arr.tids[i]);

			if (r && !ret)
				ret = r;
		}

		if (!ret)
			qos_tid_array_apply_count++;
		return ret;

	default:
		return -ENOTTY;
	}
}

static ssize_t oplus_qos_write_accept(struct file *file,
				      const char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	return count;
}

static int oplus_qos_lut_show(struct seq_file *m, void *v)
{
	int i;

	mutex_lock(&qos_lock);

	seq_printf(m, "last_lut_type=%d\n", qos_last_lut_type);
	seq_printf(m, "lut_update_count=%lu\n", qos_lut_update_count);

	for (i = 0; i < OPLUS_QOS_MAX_TYPES; i++) {
		if (qos_luts[i].area)
			seq_printf(m, "type=%d size=%zu\n",
				   i + 1, qos_luts[i].size);
	}

	mutex_unlock(&qos_lock);

	return 0;
}

static int oplus_qos_level_show(struct seq_file *m, void *v)
{
	seq_printf(m, "tid_apply_count=%lu\n", qos_tid_apply_count);
	seq_printf(m, "pid_apply_count=%lu\n", qos_pid_apply_count);
	seq_printf(m, "tid_array_apply_count=%lu\n", qos_tid_array_apply_count);
	return 0;
}

static int oplus_qos_lut_open(struct inode *inode, struct file *file)
{
	return single_open(file, oplus_qos_lut_show, NULL);
}

static int oplus_qos_level_open(struct inode *inode, struct file *file)
{
	return single_open(file, oplus_qos_level_show, NULL);
}

static const struct proc_ops qos_lut_ops = {
	.proc_open = oplus_qos_lut_open,
	.proc_read = seq_read,
	.proc_write = oplus_qos_write_accept,
	.proc_ioctl = oplus_qos_lut_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = oplus_qos_lut_ioctl,
#endif
	.proc_mmap = oplus_qos_lut_mmap,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops qos_level_ops = {
	.proc_open = oplus_qos_level_open,
	.proc_read = seq_read,
	.proc_write = oplus_qos_write_accept,
	.proc_ioctl = oplus_qos_level_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = oplus_qos_level_ioctl,
#endif
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init oplus_qos_compat_init(void)
{
	qos_dir = proc_mkdir("oplus_qos_sched", NULL);
	if (!qos_dir)
		return -ENOMEM;

	qos_lut_entry = proc_create("qos_lut", 0666, qos_dir, &qos_lut_ops);
	if (!qos_lut_entry)
		goto err;

	qos_level_entry = proc_create("qos_level", 0666, qos_dir, &qos_level_ops);
	if (!qos_level_entry)
		goto err;

	pr_info("oplus_qos_compat: /proc/oplus_qos_sched active\n");
	return 0;

err:
	if (qos_lut_entry)
		proc_remove(qos_lut_entry);
	if (qos_level_entry)
		proc_remove(qos_level_entry);
	if (qos_dir)
		proc_remove(qos_dir);

	return -ENOMEM;
}

static void __exit oplus_qos_compat_exit(void)
{
	int i;

	if (qos_lut_entry)
		proc_remove(qos_lut_entry);
	if (qos_level_entry)
		proc_remove(qos_level_entry);
	if (qos_dir)
		proc_remove(qos_dir);

	mutex_lock(&qos_lock);

	for (i = 0; i < OPLUS_QOS_MAX_TYPES; i++) {
		if (qos_luts[i].area) {
			vfree(qos_luts[i].area);
			qos_luts[i].area = NULL;
			qos_luts[i].size = 0;
		}
	}

	mutex_unlock(&qos_lock);

	pr_info("oplus_qos_compat: removed\n");
}

module_init(oplus_qos_compat_init);
module_exit(oplus_qos_compat_exit);

MODULE_DESCRIPTION("OPlus QoS scheduler compatibility for ColorOS");
MODULE_AUTHOR("NEESCHAL");
MODULE_LICENSE("GPL");
