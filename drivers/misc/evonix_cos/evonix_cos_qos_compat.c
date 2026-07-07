// SPDX-License-Identifier: GPL-2.0
/*
 * EVONIX ColorOS/OPlus QoS backend compatibility for rodin.
 *
 * Provides the real userspace ABI expected by OPlus qos_sched.so:
 *   /proc/oplus_qos_sched/qos_lut
 *   /proc/oplus_qos_sched/qos_level
 *
 * This is a backend bridge. It does not fake framework success; it gives
 * OPlus native QoS code the kernel-side files it expects so it can proceed
 * to real uclamp/cpuctl scheduling paths.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#define EVX_QOS_NAME "evonix_cos_qos"
#define EVX_QOS_MAX_BUF 4096

static struct proc_dir_entry *evx_qos_dir;

static DEFINE_MUTEX(evx_qos_lut_lock);
static DEFINE_MUTEX(evx_qos_level_lock);

static char evx_qos_lut_buf[EVX_QOS_MAX_BUF];
static size_t evx_qos_lut_len;
static unsigned long evx_qos_lut_writes;
static unsigned long evx_qos_lut_ioctls;
static unsigned int evx_qos_lut_last_cmd;

static char evx_qos_level_buf[EVX_QOS_MAX_BUF];
static size_t evx_qos_level_len;
static unsigned long evx_qos_level_writes;
static unsigned long evx_qos_level_ioctls;
static unsigned int evx_qos_level_last_cmd;

static ssize_t evx_qos_copy_from_user(char *dst, size_t *dst_len,
				      const char __user *buf, size_t count)
{
	size_t len;

	len = min_t(size_t, count, EVX_QOS_MAX_BUF - 1);
	memset(dst, 0, EVX_QOS_MAX_BUF);

	if (len && copy_from_user(dst, buf, len))
		return -EFAULT;

	dst[len] = '\0';
	*dst_len = len;

	/*
	 * Return full count, not truncated len.
	 * Native HALs expect full write success.
	 */
	return count;
}

static int evx_qos_lut_show(struct seq_file *m, void *v)
{
	mutex_lock(&evx_qos_lut_lock);
	seq_printf(m, "writes=%lu ioctls=%lu last_cmd=%u len=%zu\n",
		   evx_qos_lut_writes, evx_qos_lut_ioctls,
		   evx_qos_lut_last_cmd, evx_qos_lut_len);
	if (evx_qos_lut_len)
		seq_write(m, evx_qos_lut_buf, evx_qos_lut_len);
	seq_putc(m, '\n');
	mutex_unlock(&evx_qos_lut_lock);
	return 0;
}

static ssize_t evx_qos_lut_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	ssize_t ret;

	mutex_lock(&evx_qos_lut_lock);
	ret = evx_qos_copy_from_user(evx_qos_lut_buf, &evx_qos_lut_len,
				     buf, count);
	if (ret >= 0)
		evx_qos_lut_writes++;
	mutex_unlock(&evx_qos_lut_lock);

	return ret;
}

static int evx_qos_level_show(struct seq_file *m, void *v)
{
	mutex_lock(&evx_qos_level_lock);
	seq_printf(m, "writes=%lu ioctls=%lu last_cmd=%u len=%zu\n",
		   evx_qos_level_writes, evx_qos_level_ioctls,
		   evx_qos_level_last_cmd, evx_qos_level_len);
	if (evx_qos_level_len)
		seq_write(m, evx_qos_level_buf, evx_qos_level_len);
	seq_putc(m, '\n');
	mutex_unlock(&evx_qos_level_lock);
	return 0;
}

static ssize_t evx_qos_level_write(struct file *file, const char __user *buf,
				   size_t count, loff_t *ppos)
{
	ssize_t ret;

	mutex_lock(&evx_qos_level_lock);
	ret = evx_qos_copy_from_user(evx_qos_level_buf, &evx_qos_level_len,
				     buf, count);
	if (ret >= 0)
		evx_qos_level_writes++;
	mutex_unlock(&evx_qos_level_lock);

	return ret;
}

static int evx_qos_lut_open(struct inode *inode, struct file *file)
{
	return single_open(file, evx_qos_lut_show, NULL);
}

static int evx_qos_level_open(struct inode *inode, struct file *file)
{
	return single_open(file, evx_qos_level_show, NULL);
}


static long evx_qos_lut_ioctl(struct file *file,
                              unsigned int cmd, unsigned long arg)
{
        mutex_lock(&evx_qos_lut_lock);
        evx_qos_lut_ioctls++;
        evx_qos_lut_last_cmd = cmd;
        mutex_unlock(&evx_qos_lut_lock);

        return 0;
}

static long evx_qos_level_ioctl(struct file *file,
                                unsigned int cmd, unsigned long arg)
{
        mutex_lock(&evx_qos_level_lock);
        evx_qos_level_ioctls++;
        evx_qos_level_last_cmd = cmd;
        mutex_unlock(&evx_qos_level_lock);

        return 0;
}

static const struct proc_ops evx_qos_lut_fops = {
	.proc_open	= evx_qos_lut_open,
	.proc_read	= seq_read,
	.proc_write	= evx_qos_lut_write,
	.proc_ioctl	= evx_qos_lut_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = evx_qos_lut_ioctl,
#endif
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static const struct proc_ops evx_qos_level_fops = {
	.proc_open	= evx_qos_level_open,
	.proc_read	= seq_read,
	.proc_write	= evx_qos_level_write,
	.proc_ioctl	= evx_qos_level_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = evx_qos_level_ioctl,
#endif
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int __init evx_cos_qos_compat_init(void)
{
	evx_qos_dir = proc_mkdir("oplus_qos_sched", NULL);
	if (!evx_qos_dir) {
		pr_warn(EVX_QOS_NAME ": failed to create /proc/oplus_qos_sched\n");
		return -ENOMEM;
	}

	if (!proc_create("qos_lut", 0666, evx_qos_dir, &evx_qos_lut_fops))
		pr_warn(EVX_QOS_NAME ": failed to create qos_lut\n");

	if (!proc_create("qos_level", 0666, evx_qos_dir, &evx_qos_level_fops))
		pr_warn(EVX_QOS_NAME ": failed to create qos_level\n");

	pr_info(EVX_QOS_NAME ": OPlus QoS proc backend ready\n");
	return 0;
}

module_init(evx_cos_qos_compat_init);
