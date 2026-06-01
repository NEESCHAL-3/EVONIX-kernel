// SPDX-License-Identifier: GPL-2.0
/*
 * OPlus frame boost compatibility for ColorOS.
 *
 * ColorOS libSchedAssistExtImpl.so expects:
 *   /proc/oplus_frame_boost/ctrl
 *   /proc/oplus_frame_boost/sys_ctrl
 *   /proc/sys/fbg/frame_boost_enabled
 *
 * This layer accepts ColorOS frame boost open/write paths and stores
 * runtime state so framework/native SchedAssist can initialize cleanly.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/sysctl.h>
#include <linux/string.h>

#define OPLUS_FB_MAX_RAW 256

#define OPLUS_FB_IOC_SET_FPS          0xc030de01
#define OPLUS_FB_IOC_SET_SF_MSG_TRANS 0xc030de08
#define OPLUS_FB_IOC_CFG_APP_PARAM    0xc044de0a
#define OPLUS_FB_IOC_SET_SF_VAL       0xc044de0b

#define OPLUS_FB_IOCTL_MAX_RAW        0x44

static DEFINE_MUTEX(fb_lock);

static struct proc_dir_entry *fb_dir;
static struct proc_dir_entry *fb_ctrl_entry;
static struct proc_dir_entry *fb_sys_ctrl_entry;
static struct ctl_table_header *fbg_sysctl_hdr;

static int frame_boost_enabled;
static unsigned long ctrl_write_count;
static unsigned long sys_ctrl_write_count;
static unsigned long sys_ctrl_ioctl_count;
static unsigned int last_ioctl_cmd;
static unsigned int last_ioctl_size;
static unsigned char last_ioctl_raw[OPLUS_FB_IOCTL_MAX_RAW];
static char last_ctrl_raw[OPLUS_FB_MAX_RAW];
static char last_sys_ctrl_raw[OPLUS_FB_MAX_RAW];

static int fbg_table_frame_boost_enabled;

static struct ctl_table fbg_sysctl_table[] = {
	{
		.procname = "frame_boost_enabled",
		.data = &fbg_table_frame_boost_enabled,
		.maxlen = sizeof(int),
		.mode = 0666,
		.proc_handler = proc_dointvec,
	},
	{}
};

static void oplus_fb_store_raw(char *dst, const char __user *ubuf, size_t count)
{
	size_t len = min_t(size_t, count, OPLUS_FB_MAX_RAW - 1);

	memset(dst, 0, OPLUS_FB_MAX_RAW);

	if (copy_from_user(dst, ubuf, len))
		return;

	dst[len] = '\0';
	strim(dst);
}

static ssize_t oplus_fb_ctrl_write(struct file *file,
				   const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	mutex_lock(&fb_lock);

	oplus_fb_store_raw(last_ctrl_raw, ubuf, count);
	ctrl_write_count++;

	mutex_unlock(&fb_lock);

	pr_debug("oplus_frame_boost_compat: ctrl raw=%s\n", last_ctrl_raw);

	return count;
}

static ssize_t oplus_fb_sys_ctrl_write(struct file *file,
				       const char __user *ubuf,
				       size_t count, loff_t *ppos)
{
	int val;

	mutex_lock(&fb_lock);

	oplus_fb_store_raw(last_sys_ctrl_raw, ubuf, count);
	sys_ctrl_write_count++;

	if (!kstrtoint(last_sys_ctrl_raw, 0, &val)) {
		frame_boost_enabled = !!val;
		fbg_table_frame_boost_enabled = frame_boost_enabled;
	}

	mutex_unlock(&fb_lock);

	pr_debug("oplus_frame_boost_compat: sys_ctrl raw=%s enabled=%d\n",
		 last_sys_ctrl_raw, frame_boost_enabled);

	return count;
}

static int oplus_fb_ctrl_show(struct seq_file *m, void *v)
{
	mutex_lock(&fb_lock);

	seq_printf(m, "frame_boost_enabled=%d\n", frame_boost_enabled);
	seq_printf(m, "ctrl_write_count=%lu\n", ctrl_write_count);
	seq_printf(m, "last_ctrl_raw=%s\n", last_ctrl_raw);

	mutex_unlock(&fb_lock);

	return 0;
}

static long oplus_fb_sys_ctrl_ioctl(struct file *file, unsigned int cmd,
				    unsigned long arg)
{
	size_t size = 0;

	switch (cmd) {
	case OPLUS_FB_IOC_SET_FPS:
	case OPLUS_FB_IOC_SET_SF_MSG_TRANS:
		size = 0x30;
		break;
	case OPLUS_FB_IOC_CFG_APP_PARAM:
	case OPLUS_FB_IOC_SET_SF_VAL:
		size = 0x44;
		break;
	default:
		return -ENOTTY;
	}

	mutex_lock(&fb_lock);

	sys_ctrl_ioctl_count++;
	last_ioctl_cmd = cmd;
	last_ioctl_size = size;
	memset(last_ioctl_raw, 0, sizeof(last_ioctl_raw));

	if (arg && copy_from_user(last_ioctl_raw, (void __user *)arg, size)) {
		mutex_unlock(&fb_lock);
		return -EFAULT;
	}

	/*
	 * Real enough for compat stage:
	 * ColorOS sends frame boost state through ioctl, not write().
	 * Store/accept all known frame boost payloads so SurfaceFlinger
	 * and SchedAssist stop treating the kernel as unsupported.
	 */
	if (cmd == OPLUS_FB_IOC_SET_SF_MSG_TRANS ||
	    cmd == OPLUS_FB_IOC_SET_FPS ||
	    cmd == OPLUS_FB_IOC_CFG_APP_PARAM ||
	    cmd == OPLUS_FB_IOC_SET_SF_VAL) {
		frame_boost_enabled = 1;
		fbg_table_frame_boost_enabled = 1;
	}

	mutex_unlock(&fb_lock);

	pr_debug("oplus_frame_boost_compat: ioctl cmd=0x%x size=%zu\n",
		 cmd, size);

	return 0;
}

static int oplus_fb_sys_ctrl_show(struct seq_file *m, void *v)
{
	mutex_lock(&fb_lock);

	seq_printf(m, "frame_boost_enabled=%d\n", frame_boost_enabled);
	seq_printf(m, "sys_ctrl_write_count=%lu\n", sys_ctrl_write_count);
	seq_printf(m, "sys_ctrl_ioctl_count=%lu\n", sys_ctrl_ioctl_count);
	seq_printf(m, "last_ioctl_cmd=0x%x\n", last_ioctl_cmd);
	seq_printf(m, "last_ioctl_size=%u\n", last_ioctl_size);
	seq_printf(m, "last_sys_ctrl_raw=%s\n", last_sys_ctrl_raw);

	mutex_unlock(&fb_lock);

	return 0;
}

static int oplus_fb_ctrl_open(struct inode *inode, struct file *file)
{
	return single_open(file, oplus_fb_ctrl_show, NULL);
}

static int oplus_fb_sys_ctrl_open(struct inode *inode, struct file *file)
{
	return single_open(file, oplus_fb_sys_ctrl_show, NULL);
}

static const struct proc_ops fb_ctrl_ops = {
	.proc_open = oplus_fb_ctrl_open,
	.proc_read = seq_read,
	.proc_write = oplus_fb_ctrl_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops fb_sys_ctrl_ops = {
	.proc_open = oplus_fb_sys_ctrl_open,
	.proc_read = seq_read,
	.proc_write = oplus_fb_sys_ctrl_write,
	.proc_ioctl = oplus_fb_sys_ctrl_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = oplus_fb_sys_ctrl_ioctl,
#endif
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init oplus_frame_boost_compat_init(void)
{
	fb_dir = proc_mkdir("oplus_frame_boost", NULL);
	if (!fb_dir)
		return -ENOMEM;

	fb_ctrl_entry = proc_create("ctrl", 0666, fb_dir, &fb_ctrl_ops);
	if (!fb_ctrl_entry)
		goto err;

	fb_sys_ctrl_entry = proc_create("sys_ctrl", 0666, fb_dir, &fb_sys_ctrl_ops);
	if (!fb_sys_ctrl_entry)
		goto err;

	fbg_sysctl_hdr = register_sysctl("fbg", fbg_sysctl_table);
	if (!fbg_sysctl_hdr)
		goto err;

	pr_info("oplus_frame_boost_compat: active\n");
	return 0;

err:
	if (fb_ctrl_entry)
		proc_remove(fb_ctrl_entry);
	if (fb_sys_ctrl_entry)
		proc_remove(fb_sys_ctrl_entry);
	if (fb_dir)
		proc_remove(fb_dir);
	return -ENOMEM;
}

static void __exit oplus_frame_boost_compat_exit(void)
{
	if (fbg_sysctl_hdr)
		unregister_sysctl_table(fbg_sysctl_hdr);
	if (fb_ctrl_entry)
		proc_remove(fb_ctrl_entry);
	if (fb_sys_ctrl_entry)
		proc_remove(fb_sys_ctrl_entry);
	if (fb_dir)
		proc_remove(fb_dir);

	pr_info("oplus_frame_boost_compat: removed\n");
}

module_init(oplus_frame_boost_compat_init);
module_exit(oplus_frame_boost_compat_exit);

MODULE_DESCRIPTION("OPlus frame boost compatibility for ColorOS");
MODULE_AUTHOR("NEESCHAL");
MODULE_LICENSE("GPL");
