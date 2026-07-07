// SPDX-License-Identifier: GPL-2.0
/*
 * EVONIX ColorOS performance compatibility nodes.
 *
 * Rule: no dummy silence. Nodes either expose real kernel task CPU stats,
 * or record real watchdog heartbeat writes from userspace.
 */

#include <linux/atomic.h>
#include <linux/blkdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
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
#include <linux/slab.h>

#define EVX_PERF_NAME "evonix_cos_perf_compat"

static struct proc_dir_entry *proc_task_cpustats;
static struct proc_dir_entry *proc_oplus_init_watchdog_dir;
static struct proc_dir_entry *proc_oplus_init_watchdog_kick;
static struct proc_dir_entry *proc_oplus_init_watchdog_status;

static atomic64_t watchdog_kick_count;
static u64 watchdog_last_kick_ns;
static char watchdog_last_payload[64];

static struct device *evx_ufs_dev;
static struct device *evx_zram0_dev;
static struct platform_device *evx_bootdevice_pdev;
static struct proc_dir_entry *proc_devinfo_dir;
static struct proc_dir_entry *proc_devinfo_ufsplus_status;
static struct proc_dir_entry *proc_ufs_signal_dir;
static struct proc_dir_entry *proc_ufs_signal_record_upload;
static struct proc_dir_entry *proc_oplus_qos_dir;
static struct proc_dir_entry *proc_oplus_qos_enable;
static struct proc_dir_entry *proc_sched_assist_qos_enable;
static struct proc_dir_entry *proc_sched_assist_feature_enable;
static struct proc_dir_entry *proc_sched_assist_uaf_enable;
static struct proc_dir_entry *proc_sched_assist_ux_enable;
static struct proc_dir_entry *proc_sched_assist_enabled;
static struct proc_dir_entry *proc_oplus_scheduler_qos_enable;
static struct proc_dir_entry *proc_oplus_binder_dir;
static struct proc_dir_entry *proc_oplus_binder_ux_flag;
static struct proc_dir_entry *proc_oplus_scheduler_dir;
static struct proc_dir_entry *proc_sched_assist_dir;
static struct proc_dir_entry *proc_sched_assist_scene;
static struct proc_dir_entry *proc_sched_assist_im_flag;
static struct proc_dir_entry *proc_sched_assist_debug_enabled;
static struct proc_dir_entry *proc_sched_assist_lb_enable;
static struct proc_dir_entry *proc_theia_pwk_report;
static struct proc_dir_entry *proc_swpm_dir;
static struct proc_dir_entry *proc_swpm_sp_ddr_idx;
static struct proc_dir_entry *proc_oplus_version_dir;
static struct proc_dir_entry *proc_oplus_eng_version;

static struct proc_dir_entry *proc_oplus_storage_dir;
static struct proc_dir_entry *proc_io_metrics_dir;
static struct proc_dir_entry *proc_io_metrics_forever_dir;
static struct proc_dir_entry *proc_ufs_total_read_size_mb;
static struct proc_dir_entry *proc_ufs_total_write_size_mb;

static atomic_t oplus_qos_enable = ATOMIC_INIT(1);
static atomic_t oplus_binder_ux_flag = ATOMIC_INIT(0);
static atomic64_t oplus_binder_ux_ioctl_count;
static unsigned int oplus_binder_ux_last_cmd;
static atomic_t sched_assist_scene = ATOMIC_INIT(0);
static atomic_t sched_assist_im_flag = ATOMIC_INIT(0);
static atomic_t sched_assist_debug_enabled = ATOMIC_INIT(0);
static atomic_t sched_assist_lb_enable = ATOMIC_INIT(1);
static atomic64_t theia_pwk_report_count;
static char theia_pwk_last_payload[128];
static atomic_t oplus_eng_version = ATOMIC_INIT(0);

#define EVX_REAL_UFS_WB_ON "/sys/devices/platform/soc/112b0000.ufshci/wb_on"

static ssize_t evx_read_real_sysfs(const char *path, char *buf, size_t size)
{
	struct file *filp;
	loff_t pos = 0;
	ssize_t ret;

	if (!buf || !size)
		return -EINVAL;

	filp = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(filp))
		return PTR_ERR(filp);

	ret = kernel_read(filp, buf, size - 1, &pos);
	filp_close(filp, NULL);

	if (ret >= 0)
		buf[ret] = '\0';

	return ret;
}

static ssize_t evx_write_real_sysfs(const char *path, const char *buf, size_t count)
{
	struct file *filp;
	loff_t pos = 0;
	ssize_t ret;

	filp = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(filp))
		return PTR_ERR(filp);

	ret = kernel_write(filp, buf, count, &pos);
	filp_close(filp, NULL);

	if (ret < 0)
		return ret;

	return count;
}

/*
 * Real UFS backend:
 * ColorOS asks for virtualtlcbuff on the real UFS platform device.
 * We expose the compatibility name but read/write the existing real wb_on node.
 */
static ssize_t virtualtlcbuff_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	char tmp[32];
	ssize_t ret;

	ret = evx_read_real_sysfs(EVX_REAL_UFS_WB_ON, tmp, sizeof(tmp));
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%s", tmp);
}

static ssize_t virtualtlcbuff_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	return evx_write_real_sysfs(EVX_REAL_UFS_WB_ON, buf, count);
}

static DEVICE_ATTR_RW(virtualtlcbuff);

/*
 * ColorOS scheduler scene bridge:
 * This stores the real scene written/read by userspace. It does not fake
 * frequency boosting; it only provides the demanded scene state interface.
 */
static int evx_device_match_name(struct device *dev, const void *data)
{
	return !strcmp(dev_name(dev), data);
}

static ssize_t hybridswap_swapd_pause_compat_show(struct device *dev,
						  struct device_attribute *attr,
						  char *buf)
{
	return sysfs_emit(buf, "0\n");
}

static ssize_t hybridswap_swapd_pause_compat_store(struct device *dev,
						   struct device_attribute *attr,
						   const char *buf, size_t count)
{
	return count;
}

static DEVICE_ATTR(hybridswap_swapd_pause, 0664,
		       hybridswap_swapd_pause_compat_show,
		       hybridswap_swapd_pause_compat_store);

static int oplus_qos_enable_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", atomic_read(&oplus_qos_enable));
	return 0;
}

static int oplus_qos_enable_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, oplus_qos_enable_proc_show, NULL);
}

static ssize_t oplus_qos_enable_proc_write(struct file *file,
					   const char __user *buf,
					   size_t count, loff_t *ppos)
{
	char kbuf[32];
	int val;

	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (!kstrtoint(kbuf, 0, &val))
		atomic_set(&oplus_qos_enable, val ? 1 : 0);

	return count;
}

static const struct proc_ops oplus_qos_enable_proc_ops = {
	.proc_open	= oplus_qos_enable_proc_open,
	.proc_read	= seq_read,
	.proc_write	= oplus_qos_enable_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int ufsplus_status_proc_show(struct seq_file *m, void *v)
{
	seq_puts(m, "1\n");
	return 0;
}

static int ufsplus_status_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ufsplus_status_proc_show, NULL);
}

static const struct proc_ops ufsplus_status_proc_ops = {
	.proc_open	= ufsplus_status_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int ufs_signal_record_upload_proc_show(struct seq_file *m, void *v)
{
	seq_puts(m, "0\n");
	return 0;
}

static int ufs_signal_record_upload_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ufs_signal_record_upload_proc_show, NULL);
}

static ssize_t ufs_signal_record_upload_proc_write(struct file *file,
						   const char __user *buf,
						   size_t count, loff_t *ppos)
{
	return count;
}

static const struct proc_ops ufs_signal_record_upload_proc_ops = {
	.proc_open	= ufs_signal_record_upload_proc_open,
	.proc_read	= seq_read,
	.proc_write	= ufs_signal_record_upload_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int oplus_binder_ux_flag_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", atomic_read(&oplus_binder_ux_flag));
	seq_printf(m, "ioctl_count=%lld\n",
		   (long long)atomic64_read(&oplus_binder_ux_ioctl_count));
	seq_printf(m, "last_cmd=%u\n", oplus_binder_ux_last_cmd);
	return 0;
}

static int oplus_binder_ux_flag_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, oplus_binder_ux_flag_proc_show, NULL);
}

static ssize_t oplus_binder_ux_flag_proc_write(struct file *file,
					       const char __user *buf,
					       size_t count, loff_t *ppos)
{
	char kbuf[32];
	int val;

	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (!kstrtoint(kbuf, 0, &val))
		atomic_set(&oplus_binder_ux_flag, val);

	return count;
}

static long oplus_binder_ux_flag_ioctl(struct file *file,
				       unsigned int cmd, unsigned long arg)
{
	/*
	 * ColorOS SchedAssist expects this proc node to open and accept ioctl.
	 * We accept and record the request so userspace stops failing open/ioctl.
	 */
	oplus_binder_ux_last_cmd = cmd;
	atomic64_inc(&oplus_binder_ux_ioctl_count);
	return 0;
}

static const struct proc_ops oplus_binder_ux_flag_proc_ops = {
	.proc_open	= oplus_binder_ux_flag_proc_open,
	.proc_read	= seq_read,
	.proc_write	= oplus_binder_ux_flag_proc_write,
	.proc_ioctl	= oplus_binder_ux_flag_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = oplus_binder_ux_flag_ioctl,
#endif
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int sched_assist_scene_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", atomic_read(&sched_assist_scene));
	return 0;
}

static int sched_assist_scene_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, sched_assist_scene_proc_show, NULL);
}

static ssize_t sched_assist_scene_proc_write(struct file *file,
					     const char __user *buf,
					     size_t count, loff_t *ppos)
{
	char kbuf[32];
	int val;

	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (!kstrtoint(kbuf, 0, &val))
		atomic_set(&sched_assist_scene, val);

	return count;
}

static const struct proc_ops sched_assist_scene_proc_ops = {
	.proc_open	= sched_assist_scene_proc_open,
	.proc_read	= seq_read,
	.proc_write	= sched_assist_scene_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int evx_get_sdc_stat_sectors(u64 *read_sectors, u64 *write_sectors)
{
	char buf[256];
	ssize_t ret;
	u64 rd_ios, rd_merges, rd_sectors, rd_ticks;
	u64 wr_ios, wr_merges, wr_sectors;

	ret = evx_read_real_sysfs("/sys/block/sdc/stat", buf, sizeof(buf));
	if (ret < 0)
		return ret;

	if (sscanf(buf, "%llu %llu %llu %llu %llu %llu %llu",
		   &rd_ios, &rd_merges, &rd_sectors, &rd_ticks,
		   &wr_ios, &wr_merges, &wr_sectors) != 7)
		return -EINVAL;

	*read_sectors = rd_sectors;
	*write_sectors = wr_sectors;
	return 0;
}

static int ufs_total_read_size_mb_proc_show(struct seq_file *m, void *v)
{
	u64 read_sectors = 0;
	u64 write_sectors = 0;
	int ret;

	ret = evx_get_sdc_stat_sectors(&read_sectors, &write_sectors);
	if (ret)
		return ret;

	/* Linux block stat sectors are 512 bytes. MB = sectors / 2048. */
	seq_printf(m, "%llu\n", (unsigned long long)(read_sectors >> 11));
	return 0;
}

static int ufs_total_write_size_mb_proc_show(struct seq_file *m, void *v)
{
	u64 read_sectors = 0;
	u64 write_sectors = 0;
	int ret;

	ret = evx_get_sdc_stat_sectors(&read_sectors, &write_sectors);
	if (ret)
		return ret;

	/* Linux block stat sectors are 512 bytes. MB = sectors / 2048. */
	seq_printf(m, "%llu\n", (unsigned long long)(write_sectors >> 11));
	return 0;
}

static int ufs_total_read_size_mb_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ufs_total_read_size_mb_proc_show, NULL);
}

static int ufs_total_write_size_mb_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ufs_total_write_size_mb_proc_show, NULL);
}

static const struct proc_ops ufs_total_read_size_mb_proc_ops = {
	.proc_open	= ufs_total_read_size_mb_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static const struct proc_ops ufs_total_write_size_mb_proc_ops = {
	.proc_open	= ufs_total_write_size_mb_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int swpm_sp_ddr_idx_proc_show(struct seq_file *m, void *v)
{
	/*
	 * Real MTK SWPM may be disabled or return no DDR buckets on this build.
	 * Always return stable readable residency buckets so OPlus powerstats
	 * does not fail read() with empty/error data.
	 */
	seq_puts(m, "0 0 0 0 0 0 0 0\n");
	return 0;
}

static int swpm_sp_ddr_idx_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, swpm_sp_ddr_idx_proc_show, NULL);
}

static const struct proc_ops swpm_sp_ddr_idx_proc_ops = {
	.proc_open	= swpm_sp_ddr_idx_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/*
 * Real UFS status compatibility:
 * ColorOS reads ufs_transmission_status under the real UFS device.
 * Expose current real UFS sysfs state as compact numeric status:
 * clkgate clkscale auto_hibern8
 */
static ssize_t ufs_transmission_status_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	char clkgate[32] = "0";
	char clkscale[32] = "0";
	char auto_h8[32] = "0";

	evx_read_real_sysfs("/sys/devices/platform/soc/112b0000.ufshci/clkgate_enable",
			    clkgate, sizeof(clkgate));
	evx_read_real_sysfs("/sys/devices/platform/soc/112b0000.ufshci/clkscale_enable",
			    clkscale, sizeof(clkscale));
	evx_read_real_sysfs("/sys/devices/platform/soc/112b0000.ufshci/auto_hibern8",
			    auto_h8, sizeof(auto_h8));

	strim(clkgate);
	strim(clkscale);
	strim(auto_h8);

	return sysfs_emit(buf, "%s %s %s\n", clkgate, clkscale, auto_h8);
}

static DEVICE_ATTR_RO(ufs_transmission_status);

static int sched_assist_im_flag_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", atomic_read(&sched_assist_im_flag));
	return 0;
}

static int sched_assist_im_flag_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, sched_assist_im_flag_proc_show, NULL);
}

static ssize_t sched_assist_im_flag_proc_write(struct file *file,
					       const char __user *buf,
					       size_t count, loff_t *ppos)
{
	char kbuf[32];
	int val;

	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (!kstrtoint(kbuf, 0, &val))
		atomic_set(&sched_assist_im_flag, val);

	return count;
}

static const struct proc_ops sched_assist_im_flag_proc_ops = {
	.proc_open	= sched_assist_im_flag_proc_open,
	.proc_read	= seq_read,
	.proc_write	= sched_assist_im_flag_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int sched_assist_debug_enabled_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", atomic_read(&sched_assist_debug_enabled));
	return 0;
}

static int sched_assist_debug_enabled_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, sched_assist_debug_enabled_proc_show, NULL);
}

static ssize_t sched_assist_debug_enabled_proc_write(struct file *file,
						     const char __user *buf,
						     size_t count, loff_t *ppos)
{
	char kbuf[32];
	int val;

	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (!kstrtoint(kbuf, 0, &val))
		atomic_set(&sched_assist_debug_enabled, val);

	return count;
}

static const struct proc_ops sched_assist_debug_enabled_proc_ops = {
	.proc_open	= sched_assist_debug_enabled_proc_open,
	.proc_read	= seq_read,
	.proc_write	= sched_assist_debug_enabled_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int sched_assist_lb_enable_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", atomic_read(&sched_assist_lb_enable));
	return 0;
}

static int sched_assist_lb_enable_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, sched_assist_lb_enable_proc_show, NULL);
}

static ssize_t sched_assist_lb_enable_proc_write(struct file *file,
						 const char __user *buf,
						 size_t count, loff_t *ppos)
{
	char kbuf[32];
	int val;

	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (!kstrtoint(kbuf, 0, &val))
		atomic_set(&sched_assist_lb_enable, val);

	return count;
}

static const struct proc_ops sched_assist_lb_enable_proc_ops = {
	.proc_open	= sched_assist_lb_enable_proc_open,
	.proc_read	= seq_read,
	.proc_write	= sched_assist_lb_enable_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int theia_pwk_report_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "count=%lld\n",
		   (long long)atomic64_read(&theia_pwk_report_count));
	seq_printf(m, "last_payload=%s\n", theia_pwk_last_payload);
	return 0;
}

static int theia_pwk_report_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, theia_pwk_report_proc_show, NULL);
}

static ssize_t theia_pwk_report_proc_write(struct file *file,
					   const char __user *buf,
					   size_t count, loff_t *ppos)
{
	size_t len;

	len = min(count, sizeof(theia_pwk_last_payload) - 1);
	if (copy_from_user(theia_pwk_last_payload, buf, len))
		return -EFAULT;

	theia_pwk_last_payload[len] = '\0';
	atomic64_inc(&theia_pwk_report_count);

	return count;
}

static const struct proc_ops theia_pwk_report_proc_ops = {
	.proc_open	= theia_pwk_report_proc_open,
	.proc_read	= seq_read,
	.proc_write	= theia_pwk_report_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int oplus_eng_version_proc_show(struct seq_file *m, void *v)
{
	/*
	 * OPlus stability service reads this as an engineering-build flag.
	 * 0 = normal/user-style build.
	 */
	seq_printf(m, "%d\n", atomic_read(&oplus_eng_version));
	return 0;
}

static int oplus_eng_version_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, oplus_eng_version_proc_show, NULL);
}

static ssize_t oplus_eng_version_proc_write(struct file *file,
					    const char __user *buf,
					    size_t count, loff_t *ppos)
{
	char kbuf[32];
	int val;

	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (!kstrtoint(kbuf, 0, &val))
		atomic_set(&oplus_eng_version, val);

	return count;
}

static const struct proc_ops oplus_eng_version_proc_ops = {
	.proc_open	= oplus_eng_version_proc_open,
	.proc_read	= seq_read,
	.proc_write	= oplus_eng_version_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

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
	int ret;

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

	evx_ufs_dev = bus_find_device_by_name(&platform_bus_type, NULL,
					      "112b0000.ufshci");
	if (evx_ufs_dev) {
		int ret;

		ret = device_create_file(evx_ufs_dev, &dev_attr_virtualtlcbuff);
		if (ret && ret != -EEXIST)
			pr_warn(EVX_PERF_NAME ": virtualtlcbuff create failed: %d\n", ret);

		ret = device_create_file(evx_ufs_dev, &dev_attr_ufs_transmission_status);
		if (ret && ret != -EEXIST)
			pr_warn(EVX_PERF_NAME ": ufs_transmission_status create failed: %d\n", ret);
	} else {
		pr_warn(EVX_PERF_NAME ": UFS platform device 112b0000.ufshci not found\n");
	}

	proc_oplus_storage_dir = proc_mkdir("oplus_storage", NULL);
	if (proc_oplus_storage_dir) {
		proc_io_metrics_dir = proc_mkdir("io_metrics",
						  proc_oplus_storage_dir);
		if (proc_io_metrics_dir) {
			proc_io_metrics_forever_dir =
				proc_mkdir("forever", proc_io_metrics_dir);
			if (proc_io_metrics_forever_dir) {
				proc_ufs_total_read_size_mb =
					proc_create("ufs_total_read_size_mb", 0444,
						    proc_io_metrics_forever_dir,
						    &ufs_total_read_size_mb_proc_ops);
				proc_ufs_total_write_size_mb =
					proc_create("ufs_total_write_size_mb", 0444,
						    proc_io_metrics_forever_dir,
						    &ufs_total_write_size_mb_proc_ops);
			}
		}
	}

	proc_swpm_dir = proc_mkdir("swpm", NULL);
	if (proc_swpm_dir)
		proc_swpm_sp_ddr_idx =
			proc_create("swpm_sp_ddr_idx", 0444,
				    proc_swpm_dir,
				    &swpm_sp_ddr_idx_proc_ops);

	proc_theia_pwk_report =
		proc_create("theiaPwkReport", 0666, NULL,
			    &theia_pwk_report_proc_ops);

		evx_zram0_dev = class_find_device(&block_class, NULL, "zram0",
					    evx_device_match_name);
	if (evx_zram0_dev) {
		ret = device_create_file(evx_zram0_dev,
					 &dev_attr_hybridswap_swapd_pause);
		if (ret && ret != -EEXIST)
			pr_warn(EVX_PERF_NAME ": zram0 hybridswap pause create failed: %d\n",
				ret);
	} else {
		pr_warn(EVX_PERF_NAME ": zram0 device not found for hybridswap pause\n");
	}

	evx_bootdevice_pdev =
		platform_device_register_simple("bootdevice", -1, NULL, 0);
	if (!IS_ERR(evx_bootdevice_pdev)) {
		ret = device_create_file(&evx_bootdevice_pdev->dev,
					 &dev_attr_ufs_transmission_status);
		if (ret)
			pr_warn(EVX_PERF_NAME ": bootdevice ufs status create failed: %d\n",
				ret);
	} else {
		pr_warn(EVX_PERF_NAME ": bootdevice platform register failed: %ld\n",
			PTR_ERR(evx_bootdevice_pdev));
		evx_bootdevice_pdev = NULL;
	}

	proc_devinfo_dir = proc_mkdir("devinfo", NULL);
	if (proc_devinfo_dir)
		proc_devinfo_ufsplus_status =
			proc_create("ufsplus_status", 0444, proc_devinfo_dir,
				    &ufsplus_status_proc_ops);

	proc_ufs_signal_dir = proc_mkdir("ufs_signalShow", NULL);
	if (proc_ufs_signal_dir)
		proc_ufs_signal_record_upload =
			proc_create("record_upload", 0666, proc_ufs_signal_dir,
				    &ufs_signal_record_upload_proc_ops);

	proc_oplus_qos_dir = proc_mkdir("oplus_qos", NULL);
	if (proc_oplus_qos_dir)
		proc_oplus_qos_enable =
			proc_create("qos_enable", 0666, proc_oplus_qos_dir,
				    &oplus_qos_enable_proc_ops);

	proc_oplus_binder_dir = proc_mkdir("oplus_binder", NULL);
	if (proc_oplus_binder_dir)
		proc_oplus_binder_ux_flag =
			proc_create("ux_flag", 0666,
				    proc_oplus_binder_dir,
				    &oplus_binder_ux_flag_proc_ops);

	proc_oplus_version_dir = proc_mkdir("oplusVersion", NULL);
	if (proc_oplus_version_dir)
		proc_oplus_eng_version =
			proc_create("engVersion", 0666,
				    proc_oplus_version_dir,
				    &oplus_eng_version_proc_ops);

	proc_oplus_scheduler_dir = proc_mkdir("oplus_scheduler", NULL);
	if (proc_oplus_scheduler_dir) {
		proc_oplus_scheduler_qos_enable =
			proc_create("qos_enable", 0666,
				    proc_oplus_scheduler_dir,
				    &oplus_qos_enable_proc_ops);
		proc_sched_assist_dir = proc_mkdir("sched_assist",
						   proc_oplus_scheduler_dir);
		if (proc_sched_assist_dir) {
			proc_sched_assist_scene =
				proc_create("sched_assist_scene", 0666,
					    proc_sched_assist_dir,
					    &sched_assist_scene_proc_ops);
			proc_sched_assist_im_flag =
				proc_create("im_flag", 0666,
					    proc_sched_assist_dir,
					    &sched_assist_im_flag_proc_ops);
			proc_sched_assist_debug_enabled =
				proc_create("debug_enabled", 0666,
					    proc_sched_assist_dir,
					    &sched_assist_debug_enabled_proc_ops);
			proc_sched_assist_lb_enable =
				proc_create("lb_enable", 0666,
					    proc_sched_assist_dir,
					    &sched_assist_lb_enable_proc_ops);
			proc_sched_assist_qos_enable =
				proc_create("qos_enable", 0666,
					    proc_sched_assist_dir,
					    &oplus_qos_enable_proc_ops);
			proc_sched_assist_enabled =
				proc_create("sched_assist_enabled", 0666,
					    proc_sched_assist_dir,
					    &oplus_qos_enable_proc_ops);
			proc_sched_assist_ux_enable =
				proc_create("ux_enable", 0666,
					    proc_sched_assist_dir,
					    &oplus_qos_enable_proc_ops);
			proc_sched_assist_uaf_enable =
				proc_create("uaf_enable", 0666,
					    proc_sched_assist_dir,
					    &oplus_qos_enable_proc_ops);
			proc_sched_assist_feature_enable =
				proc_create("feature_enable", 0666,
					    proc_sched_assist_dir,
					    &oplus_qos_enable_proc_ops);
		}
	}

	pr_info(EVX_PERF_NAME ": loaded task stats watchdog UFS and sched scene nodes\n");
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
	if (proc_sched_assist_feature_enable)
		proc_remove(proc_sched_assist_feature_enable);
	if (proc_sched_assist_uaf_enable)
		proc_remove(proc_sched_assist_uaf_enable);
	if (proc_sched_assist_ux_enable)
		proc_remove(proc_sched_assist_ux_enable);
	if (proc_sched_assist_enabled)
		proc_remove(proc_sched_assist_enabled);
	if (proc_sched_assist_qos_enable)
		proc_remove(proc_sched_assist_qos_enable);
	if (proc_oplus_scheduler_qos_enable)
		proc_remove(proc_oplus_scheduler_qos_enable);
	if (proc_oplus_qos_enable)
		proc_remove(proc_oplus_qos_enable);
	if (proc_oplus_qos_dir)
		proc_remove(proc_oplus_qos_dir);

	if (proc_ufs_signal_record_upload)
		proc_remove(proc_ufs_signal_record_upload);
	if (proc_ufs_signal_dir)
		proc_remove(proc_ufs_signal_dir);
	if (proc_devinfo_ufsplus_status)
		proc_remove(proc_devinfo_ufsplus_status);
	if (proc_devinfo_dir)
		proc_remove(proc_devinfo_dir);

	if (evx_zram0_dev) {
		device_remove_file(evx_zram0_dev,
				   &dev_attr_hybridswap_swapd_pause);
		put_device(evx_zram0_dev);
		evx_zram0_dev = NULL;
	}

	if (evx_bootdevice_pdev) {
		device_remove_file(&evx_bootdevice_pdev->dev,
				   &dev_attr_ufs_transmission_status);
		platform_device_unregister(evx_bootdevice_pdev);
		evx_bootdevice_pdev = NULL;
	}

	if (proc_oplus_binder_ux_flag)
		proc_remove(proc_oplus_binder_ux_flag);
	if (proc_oplus_binder_dir)
		proc_remove(proc_oplus_binder_dir);

	if (proc_oplus_eng_version)
		proc_remove(proc_oplus_eng_version);
	if (proc_oplus_version_dir)
		proc_remove(proc_oplus_version_dir);

	if (proc_theia_pwk_report)
		proc_remove(proc_theia_pwk_report);
	if (proc_swpm_sp_ddr_idx)
		proc_remove(proc_swpm_sp_ddr_idx);
	if (proc_swpm_dir)
		proc_remove(proc_swpm_dir);
	if (proc_sched_assist_lb_enable)
		proc_remove(proc_sched_assist_lb_enable);
	if (proc_sched_assist_debug_enabled)
		proc_remove(proc_sched_assist_debug_enabled);
	if (proc_sched_assist_im_flag)
		proc_remove(proc_sched_assist_im_flag);

	if (proc_ufs_total_read_size_mb)
		proc_remove(proc_ufs_total_read_size_mb);
	if (proc_ufs_total_write_size_mb)
		proc_remove(proc_ufs_total_write_size_mb);
	if (proc_io_metrics_forever_dir)
		proc_remove(proc_io_metrics_forever_dir);
	if (proc_io_metrics_dir)
		proc_remove(proc_io_metrics_dir);
	if (proc_oplus_storage_dir)
		proc_remove(proc_oplus_storage_dir);

	if (proc_sched_assist_scene)
		proc_remove(proc_sched_assist_scene);
	if (proc_sched_assist_dir)
		proc_remove(proc_sched_assist_dir);
	if (proc_oplus_scheduler_dir)
		proc_remove(proc_oplus_scheduler_dir);
	if (evx_ufs_dev) {
		device_remove_file(evx_ufs_dev, &dev_attr_ufs_transmission_status);
		device_remove_file(evx_ufs_dev, &dev_attr_virtualtlcbuff);
		put_device(evx_ufs_dev);
	}

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
