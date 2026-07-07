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
static DEFINE_MUTEX(evx_afs_lock);

static int evx_afs_config_show(struct seq_file *m, void *v)
{
        mutex_lock(&evx_afs_lock);
        if (evx_afs_len)
                seq_write(m, evx_afs_buf, evx_afs_len);
        else
                seq_puts(m, "0\n");
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

static const struct proc_ops evx_afs_config_fops = {
        .proc_open      = evx_afs_config_open,
        .proc_read      = seq_read,
        .proc_write     = evx_afs_config_write,
        .proc_lseek     = seq_lseek,
        .proc_release   = single_release,
};

static int __init evx_cos_afs_compat_init(void)
{
        evx_afs_dir = proc_mkdir("oplus_afs_config", NULL);
        if (!evx_afs_dir)
                return 0;

        proc_create("afs_config", 0666, evx_afs_dir, &evx_afs_config_fops);

        pr_info("evonix_cos_afs: AFS proc compat ready\n");
        return 0;
}

module_init(evx_cos_afs_compat_init);
