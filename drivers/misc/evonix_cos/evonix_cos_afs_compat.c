// SPDX-License-Identifier: GPL-2.0
/*
 * EVONIX ColorOS/OPlus AFS config compatibility.
 * Provides /proc/oplus_afs_config/afs_config so OPlus framework can open it.
 * Safe compat: read-only stable defaults, write accepts data without changing
 * real kernel scheduling/thermal policy.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#define EVX_AFS_MAX_BUF 512

static struct proc_dir_entry *evx_afs_dir;
static char evx_afs_buf[EVX_AFS_MAX_BUF];
static size_t evx_afs_len;
static unsigned long evx_afs_writes;
static unsigned long evx_afs_ioctls;
static unsigned int evx_afs_last_cmd;
static unsigned long evx_afs_last_arg;
static int evx_afs_enable = 1;
static DEFINE_MUTEX(evx_afs_lock);

/*
 * Minimal binary android.os.afsConfig.AfsConfig protobuf:
 *   sceneTypeMax = 2
 *   sceneConfig { sceneType = SCENE_SYSTEM_UI(0) }
 *   sceneConfig { sceneType = SCENE_LAUNCHER(1) }
 *
 * Wire guess is from afsConfig.so embedded proto descriptors:
 * AfsConfig: sceneTypeMax, sceneConfig
 * SceneConfig: sceneType
 */
static const unsigned char evx_afs_default_proto[] = {
        0x08, 0x02,
        0x12, 0x02, 0x08, 0x00,
        0x12, 0x02, 0x08, 0x01,
};

static int evx_afs_config_show(struct seq_file *m, void *v)
{
        mutex_lock(&evx_afs_lock);
        if (evx_afs_len)
                seq_write(m, evx_afs_buf, evx_afs_len);
        else
                seq_write(m, evx_afs_default_proto,
                          sizeof(evx_afs_default_proto));
        mutex_unlock(&evx_afs_lock);
        return 0;
}

static int evx_afs_config_open(struct inode *inode, struct file *file)
{
        return single_open(file, evx_afs_config_show, NULL);
}

static ssize_t evx_afs_config_write(struct file *file,
                                    const char __user *buf,
                                    size_t count, loff_t *ppos)
{
        size_t n = min_t(size_t, count, EVX_AFS_MAX_BUF - 1);

        mutex_lock(&evx_afs_lock);
        memset(evx_afs_buf, 0, sizeof(evx_afs_buf));
        if (n && copy_from_user(evx_afs_buf, buf, n)) {
                mutex_unlock(&evx_afs_lock);
                return -EFAULT;
        }
        evx_afs_len = n;
        evx_afs_writes++;
        mutex_unlock(&evx_afs_lock);

        return count;
}


static long evx_afs_config_ioctl(struct file *file,
                                 unsigned int cmd, unsigned long arg)
{
        /*
         * OPlus afsConfig.so uses ioctl on /proc/oplus_afs_config/afs_config
         * for scene lookup, observed command 0x7102.
         * Accept the request so the native side does not fail with ENOTTY.
         */
        mutex_lock(&evx_afs_lock);
        evx_afs_ioctls++;
        evx_afs_last_cmd = cmd;
        evx_afs_last_arg = arg;
        mutex_unlock(&evx_afs_lock);

        return 0;
}

static const struct proc_ops evx_afs_config_fops = {
        .proc_open      = evx_afs_config_open,
        .proc_read      = seq_read,
        .proc_write     = evx_afs_config_write,
        .proc_ioctl     = evx_afs_config_ioctl,
#ifdef CONFIG_COMPAT
        .proc_compat_ioctl = evx_afs_config_ioctl,
#endif
        .proc_lseek     = seq_lseek,
        .proc_release   = single_release,
};


static int evx_afs_enable_show(struct seq_file *m, void *v)
{
        seq_printf(m, "%d\n", evx_afs_enable);
        return 0;
}

static int evx_afs_enable_open(struct inode *inode, struct file *file)
{
        return single_open(file, evx_afs_enable_show, NULL);
}

static ssize_t evx_afs_enable_write(struct file *file,
                                    const char __user *buf,
                                    size_t count, loff_t *ppos)
{
        char tmp[16] = {0};
        long val;

        if (count) {
                size_t n = min_t(size_t, count, sizeof(tmp) - 1);

                if (copy_from_user(tmp, buf, n))
                        return -EFAULT;

                if (!kstrtol(tmp, 0, &val))
                        evx_afs_enable = !!val;
        }

        return count;
}

static const struct proc_ops evx_afs_enable_fops = {
        .proc_open      = evx_afs_enable_open,
        .proc_read      = seq_read,
        .proc_write     = evx_afs_enable_write,
        .proc_lseek     = seq_lseek,
        .proc_release   = single_release,
};


static int evx_afs_debug_show(struct seq_file *m, void *v)
{
        mutex_lock(&evx_afs_lock);
        seq_printf(m,
                   "enable=%d writes=%lu ioctls=%lu last_cmd=0x%x last_arg=0x%lx len=%zu\n",
                   evx_afs_enable, evx_afs_writes, evx_afs_ioctls,
                   evx_afs_last_cmd, evx_afs_last_arg, evx_afs_len);
        mutex_unlock(&evx_afs_lock);
        return 0;
}

static int evx_afs_debug_open(struct inode *inode, struct file *file)
{
        return single_open(file, evx_afs_debug_show, NULL);
}

static const struct proc_ops evx_afs_debug_fops = {
        .proc_open      = evx_afs_debug_open,
        .proc_read      = seq_read,
        .proc_lseek     = seq_lseek,
        .proc_release   = single_release,
};

static int __init evx_cos_afs_compat_init(void)
{
        evx_afs_dir = proc_mkdir("oplus_afs_config", NULL);
        if (!evx_afs_dir)
                return 0;

        proc_create("afs_config", 0666, evx_afs_dir, &evx_afs_config_fops);
        proc_create("afs_enable", 0666, evx_afs_dir, &evx_afs_enable_fops);
        proc_create("afs_debug", 0444, evx_afs_dir, &evx_afs_debug_fops);

        pr_info("evonix_cos_afs: AFS binary proc compat ready\n");
        return 0;
}

module_init(evx_cos_afs_compat_init);
