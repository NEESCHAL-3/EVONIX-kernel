// SPDX-License-Identifier: GPL-2.0
/*
 * OPlus AFS compatibility layer for ColorOS bringup.
 *
 * ColorOS afsConfig.so expects:
 *   /proc/oplus_afs_config/afs_config
 *   /proc/oplus_afs_config/afs_enable
 *
 * afsConfig.so parses real protobuf config, then calls:
 *   ioctl(fd, 0x40107101, struct afs_config_user *)
 *
 * This driver accepts the real userspace-converted table and stores it
 * in kernel memory. No dummy hardcoded AFS scene table.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/types.h>

#define OPLUS_AFS_IOCTL_UPDATE_CONFIG 0x40107101
#define OPLUS_AFS_IOCTL_GET_SCENE     0x800c7102
#define OPLUS_AFS_MAX_SCENES          512

struct oplus_afs_user_config {
	int version;
	int scene_type_max;
	__u64 scene_table_ptr;
};

struct oplus_afs_scene_entry {
	__s32 scene_type;
	__u8 animation_type;
	__u8 enhance_level;
	__u8 brk_type;
	__u8 enable;
} __packed;

struct oplus_afs_get_scene_user {
	__s32 scene_type;
	struct oplus_afs_scene_entry scene;
} __packed;

static DEFINE_MUTEX(afs_lock);

static struct proc_dir_entry *afs_dir;
static struct proc_dir_entry *afs_config_entry;
static struct proc_dir_entry *afs_enable_entry;

static int afs_enable = 1;
static int afs_version;
static int afs_scene_type_max;
static int afs_scene_count;
static struct oplus_afs_scene_entry *afs_scene_table;

static int oplus_afs_update_from_user(unsigned long arg)
{
	struct oplus_afs_user_config cfg;
	struct oplus_afs_scene_entry *new_table;
	size_t entries;
	size_t bytes;

	if (!arg)
		return -EINVAL;

	if (copy_from_user(&cfg, (void __user *)arg, sizeof(cfg)))
		return -EFAULT;

	if (cfg.scene_type_max < 0 || cfg.scene_type_max > OPLUS_AFS_MAX_SCENES)
		return -EINVAL;

	/*
	 * afsConfig.so allocates enough for scene_type_max indexes.
	 * We copy scene_type_max + 1 entries because scene types are used
	 * as direct indexes and 0 can be valid.
	 */
	entries = (size_t)cfg.scene_type_max + 1;
	bytes = entries * sizeof(*new_table);

	if (!cfg.scene_table_ptr || !bytes)
		return -EINVAL;

	new_table = kzalloc(bytes, GFP_KERNEL);
	if (!new_table)
		return -ENOMEM;

	if (copy_from_user(new_table,
			   (void __user *)(uintptr_t)cfg.scene_table_ptr,
			   bytes)) {
		kfree(new_table);
		return -EFAULT;
	}

	mutex_lock(&afs_lock);

	kfree(afs_scene_table);
	afs_scene_table = new_table;
	afs_version = cfg.version;
	afs_scene_type_max = cfg.scene_type_max;
	afs_scene_count = entries;

	mutex_unlock(&afs_lock);

	pr_info("oplus_afs_compat: config updated version=%d scene_type_max=%d entries=%zu\n",
		cfg.version, cfg.scene_type_max, entries);

	return 0;
}

static int oplus_afs_get_scene_to_user(unsigned long arg)
{
	struct oplus_afs_get_scene_user req;
	struct oplus_afs_scene_entry out = { 0 };
	int i;
	int found = 0;

	if (!arg)
		return -EINVAL;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req.scene_type)))
		return -EFAULT;

	mutex_lock(&afs_lock);

	if (!afs_scene_table || afs_scene_count <= 0) {
		mutex_unlock(&afs_lock);
		return -ENOENT;
	}

	/* Fast path: scene type usually maps directly to table index. */
	if (req.scene_type >= 0 && req.scene_type < afs_scene_count) {
		out = afs_scene_table[req.scene_type];
		if (out.scene_type == req.scene_type || out.enable)
			found = 1;
	}

	/* Fallback: search actual scene_type field. */
	if (!found) {
		for (i = 0; i < afs_scene_count; i++) {
			if (afs_scene_table[i].scene_type == req.scene_type) {
				out = afs_scene_table[i];
				found = 1;
				break;
			}
		}
	}

	mutex_unlock(&afs_lock);

	if (!found)
		return -ENOENT;

	/*
	 * afsConfig.so passes 12 bytes:
	 *   int scene_type;
	 *   8-byte scene config payload;
	 *
	 * It reads returned payload from arg + 4.
	 */
	if (copy_to_user((void __user *)(arg + sizeof(__s32)),
			 &out, sizeof(out)))
		return -EFAULT;

	pr_debug("oplus_afs_compat: get scene=%d anim=%u enhance=%u brk=%u enable=%u\n",
		 req.scene_type, out.animation_type, out.enhance_level,
		 out.brk_type, out.enable);

	return 0;
}

static long oplus_afs_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	switch (cmd) {
	case OPLUS_AFS_IOCTL_UPDATE_CONFIG:
		return oplus_afs_update_from_user(arg);
	case OPLUS_AFS_IOCTL_GET_SCENE:
		return oplus_afs_get_scene_to_user(arg);
	default:
		return -ENOTTY;
	}
}

static ssize_t oplus_afs_config_write(struct file *file,
				      const char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	/*
	 * Keep write accepted for userspace compatibility.
	 * Real config update is through ioctl after protobuf parsing.
	 */
	return count;
}

static int oplus_afs_config_show(struct seq_file *m, void *v)
{
	int i;
	int shown = 0;

	mutex_lock(&afs_lock);

	seq_printf(m, "enable=%d\n", afs_enable);
	seq_printf(m, "version=%d\n", afs_version);
	seq_printf(m, "scene_type_max=%d\n", afs_scene_type_max);
	seq_printf(m, "scene_count=%d\n", afs_scene_count);

	if (afs_scene_table) {
		for (i = 0; i < afs_scene_count && shown < 32; i++) {
			struct oplus_afs_scene_entry *e = &afs_scene_table[i];

			if (e->scene_type < 0 && !e->animation_type &&
			    !e->enhance_level && !e->brk_type && !e->enable)
				continue;

			seq_printf(m,
				   "scene[%d]: type=%d anim=%u enhance=%u brk=%u enable=%u\n",
				   i, e->scene_type, e->animation_type,
				   e->enhance_level, e->brk_type, e->enable);
			shown++;
		}
	}

	mutex_unlock(&afs_lock);

	return 0;
}

static int oplus_afs_config_open(struct inode *inode, struct file *file)
{
	return single_open(file, oplus_afs_config_show, NULL);
}

static const struct proc_ops afs_config_ops = {
	.proc_open = oplus_afs_config_open,
	.proc_read = seq_read,
	.proc_write = oplus_afs_config_write,
	.proc_ioctl = oplus_afs_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = oplus_afs_ioctl,
#endif
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static ssize_t oplus_afs_enable_write(struct file *file,
				      const char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	char buf[32];
	size_t len;
	int val;

	len = min_t(size_t, count, sizeof(buf) - 1);
	memset(buf, 0, sizeof(buf));

	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;

	buf[len] = '\0';

	if (!strncasecmp(buf, "true", 4) || !strncasecmp(buf, "enable", 6))
		val = 1;
	else if (!strncasecmp(buf, "false", 5) || !strncasecmp(buf, "disable", 7))
		val = 0;
	else if (kstrtoint(buf, 0, &val))
		return -EINVAL;

	mutex_lock(&afs_lock);
	afs_enable = !!val;
	mutex_unlock(&afs_lock);

	pr_info("oplus_afs_compat: afs_enable=%d\n", afs_enable);

	return count;
}

static int oplus_afs_enable_show(struct seq_file *m, void *v)
{
	mutex_lock(&afs_lock);
	seq_printf(m, "%s\n", afs_enable ? "true" : "false");
	mutex_unlock(&afs_lock);

	return 0;
}

static int oplus_afs_enable_open(struct inode *inode, struct file *file)
{
	return single_open(file, oplus_afs_enable_show, NULL);
}

static const struct proc_ops afs_enable_ops = {
	.proc_open = oplus_afs_enable_open,
	.proc_read = seq_read,
	.proc_write = oplus_afs_enable_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init oplus_afs_compat_init(void)
{
	afs_dir = proc_mkdir("oplus_afs_config", NULL);
	if (!afs_dir)
		return -ENOMEM;

	afs_config_entry = proc_create("afs_config", 0666, afs_dir,
				       &afs_config_ops);
	if (!afs_config_entry)
		goto err;

	afs_enable_entry = proc_create("afs_enable", 0666, afs_dir,
				       &afs_enable_ops);
	if (!afs_enable_entry)
		goto err;

	pr_info("oplus_afs_compat: /proc/oplus_afs_config active\n");
	return 0;

err:
	if (afs_config_entry)
		proc_remove(afs_config_entry);
	if (afs_enable_entry)
		proc_remove(afs_enable_entry);
	if (afs_dir)
		proc_remove(afs_dir);
	return -ENOMEM;
}

static void __exit oplus_afs_compat_exit(void)
{
	if (afs_config_entry)
		proc_remove(afs_config_entry);
	if (afs_enable_entry)
		proc_remove(afs_enable_entry);
	if (afs_dir)
		proc_remove(afs_dir);

	mutex_lock(&afs_lock);
	kfree(afs_scene_table);
	afs_scene_table = NULL;
	afs_scene_count = 0;
	mutex_unlock(&afs_lock);

	pr_info("oplus_afs_compat: removed\n");
}

module_init(oplus_afs_compat_init);
module_exit(oplus_afs_compat_exit);

MODULE_DESCRIPTION("OPlus AFS compatibility for ColorOS");
MODULE_AUTHOR("NEESCHAL");
MODULE_LICENSE("GPL");
