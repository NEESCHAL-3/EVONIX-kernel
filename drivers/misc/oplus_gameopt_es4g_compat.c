// SPDX-License-Identifier: GPL-2.0
/*
 * OPlus GameOpt ES4G compatibility for ColorOS.
 *
 * Provides:
 *   /proc/game_opt/es4g/es4ga_ctrl
 *
 * Used by:
 *   /odm/bin/hw/vendor.oplus.hardware.gameopt-service
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/string.h>

#define EVONIX_ES4G_BUILD "EVONIX-OPlusGameOpt-ES4G-v13"
#define ES4G_MAX_RAW 256
#define ES4G_MAX_IOCTL_RAW 128

static DEFINE_MUTEX(es4g_lock);

static struct proc_dir_entry *game_opt_dir;
static struct proc_dir_entry *es4g_dir;
static struct proc_dir_entry *es4ga_ctrl_entry;

static unsigned long es4g_open_count;
static unsigned long es4g_write_count;
static unsigned long es4g_ioctl_count;

static unsigned int last_ioctl_cmd;
static unsigned int last_ioctl_size;
static char last_write_raw[ES4G_MAX_RAW];
static unsigned char last_ioctl_raw[ES4G_MAX_IOCTL_RAW];

static void es4g_store_raw(const char __user *ubuf, size_t count)
{
	size_t len = min_t(size_t, count, ES4G_MAX_RAW - 1);

	memset(last_write_raw, 0, sizeof(last_write_raw));

	if (copy_from_user(last_write_raw, ubuf, len))
		return;

	last_write_raw[len] = '\0';
	strim(last_write_raw);
}

static int es4g_show(struct seq_file *m, void *v)
{
	mutex_lock(&es4g_lock);

	seq_printf(m, "build=%s\n", EVONIX_ES4G_BUILD);
	seq_printf(m, "open_count=%lu\n", es4g_open_count);
	seq_printf(m, "write_count=%lu\n", es4g_write_count);
	seq_printf(m, "ioctl_count=%lu\n", es4g_ioctl_count);
	seq_printf(m, "last_ioctl_cmd=0x%x\n", last_ioctl_cmd);
	seq_printf(m, "last_ioctl_size=%u\n", last_ioctl_size);
	seq_printf(m, "last_write_raw=%s\n", last_write_raw);

	mutex_unlock(&es4g_lock);

	return 0;
}

static int es4g_open(struct inode *inode, struct file *file)
{
	mutex_lock(&es4g_lock);
	es4g_open_count++;
	mutex_unlock(&es4g_lock);

	return single_open(file, es4g_show, NULL);
}

static ssize_t es4g_write(struct file *file, const char __user *ubuf,
			  size_t count, loff_t *ppos)
{
	mutex_lock(&es4g_lock);

	es4g_write_count++;
	es4g_store_raw(ubuf, count);

	mutex_unlock(&es4g_lock);

	pr_debug("oplus_gameopt_es4g_compat: write raw=%s\n", last_write_raw);

	return count;
}

static long es4g_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	/*
	 * Stage-1 compat:
	 * GameOpt uses private ES4G ioctls for isolate cpus, critical thread,
	 * select cpu list and preempt policy. We accept and record all commands
	 * so ColorOS GameOpt can continue, then we decode real cmd/payload
	 * from runtime counters for v13.1 behavior mapping.
	 */
	mutex_lock(&es4g_lock);

	es4g_ioctl_count++;
	last_ioctl_cmd = cmd;
	last_ioctl_size = _IOC_SIZE(cmd);
	memset(last_ioctl_raw, 0, sizeof(last_ioctl_raw));

	if (arg && last_ioctl_size > 0) {
		size_t copy_len = min_t(size_t, last_ioctl_size, ES4G_MAX_IOCTL_RAW);
		if (copy_from_user(last_ioctl_raw, (void __user *)arg, copy_len)) {
			mutex_unlock(&es4g_lock);
			return -EFAULT;
		}
	}

	mutex_unlock(&es4g_lock);

	pr_debug("oplus_gameopt_es4g_compat: ioctl cmd=0x%x size=%u\n",
		 cmd, _IOC_SIZE(cmd));

	return 0;
}

static const struct proc_ops es4ga_ctrl_ops = {
	.proc_open = es4g_open,
	.proc_read = seq_read,
	.proc_write = es4g_write,
	.proc_ioctl = es4g_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = es4g_ioctl,
#endif
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init oplus_gameopt_es4g_compat_init(void)
{
	game_opt_dir = proc_mkdir("game_opt", NULL);
	if (!game_opt_dir)
		return -ENOMEM;

	es4g_dir = proc_mkdir("es4g", game_opt_dir);
	if (!es4g_dir)
		goto err;

	es4ga_ctrl_entry = proc_create("es4ga_ctrl", 0666, es4g_dir,
				       &es4ga_ctrl_ops);
	if (!es4ga_ctrl_entry)
		goto err;

	pr_info("oplus_gameopt_es4g_compat: active %s\n", EVONIX_ES4G_BUILD);
	return 0;

err:
	if (es4ga_ctrl_entry)
		proc_remove(es4ga_ctrl_entry);
	if (es4g_dir)
		proc_remove(es4g_dir);
	if (game_opt_dir)
		proc_remove(game_opt_dir);

	return -ENOMEM;
}

static void __exit oplus_gameopt_es4g_compat_exit(void)
{
	if (es4ga_ctrl_entry)
		proc_remove(es4ga_ctrl_entry);
	if (es4g_dir)
		proc_remove(es4g_dir);
	if (game_opt_dir)
		proc_remove(game_opt_dir);

	pr_info("oplus_gameopt_es4g_compat: removed\n");
}

module_init(oplus_gameopt_es4g_compat_init);
module_exit(oplus_gameopt_es4g_compat_exit);

MODULE_DESCRIPTION("OPlus GameOpt ES4G compatibility");
MODULE_AUTHOR("NEESCHAL");
MODULE_LICENSE("GPL");
