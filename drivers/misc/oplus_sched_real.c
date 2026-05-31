// SPDX-License-Identifier: GPL-2.0
/*
 * OPlus scheduler compatibility layer for ColorOS bringup.
 *
 * Real v2 behavior:
 * - Creates OPlus scheduler proc/sysctl nodes expected by ColorOS.
 * - Handles OPlus payloads such as: "p <pid> <flag>".
 * - Maps important task / UX / im_flag hints to real Linux scheduler nice boost.
 * - Keeps high-frequency logs quiet to avoid performance regression.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sysctl.h>
#include <linux/ctype.h>
#include <linux/string.h>

#define OPLUS_BUF_SZ		256
#define OPLUS_BOOST_LIGHT	(-5)
#define OPLUS_BOOST_STRONG	(-10)

static DEFINE_MUTEX(oplus_sched_lock);

static struct proc_dir_entry *root_dir;
static struct proc_dir_entry *assist_dir;
static struct proc_dir_entry *audio_dir;

static int sched_assist_scene;
static int sched_impt_tgid;
static int audio_enable;
static int disable_setting;
static int im_flag;
static int im_flag_app;
static int sched_prop;
static int tpd_id;
static int tpd_cmds;

static struct ctl_table_header *oplus_sysctl_header;

enum oplus_node_type {
	NODE_DISABLE_SETTING,
	NODE_IM_FLAG,
	NODE_IM_FLAG_APP,
	NODE_SCHED_ASSIST_SCENE,
	NODE_SCHED_IMPT_TASK,
	NODE_SCHED_PROP,
	NODE_TPD_CMDS,
	NODE_TPD_ID,
	NODE_UX_TASK,
	NODE_AUDIO_ENABLE,
};

struct oplus_node {
	const char *name;
	enum oplus_node_type type;
	int *value;
	struct proc_dir_entry *entry;
};

static int oplus_boost_pid(pid_t pid, int nice)
{
	struct task_struct *task;
	struct pid *kpid;

	if (pid <= 0)
		return -EINVAL;

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

	pr_debug("oplus_sched_real: boosted pid=%d comm=%s nice=%d\n",
		 pid, task->comm, nice);

	put_task_struct(task);
	put_pid(kpid);

	return 0;
}

static int oplus_nice_from_flag(long flag)
{
	if (flag >= 64)
		return OPLUS_BOOST_STRONG;

	if (flag > 0)
		return OPLUS_BOOST_LIGHT;

	return OPLUS_BOOST_LIGHT;
}

static const char *oplus_skip_to_number(const char *p)
{
	while (*p && !isdigit(*p) && *p != '-')
		p++;
	return p;
}

static int oplus_read_long(const char **pp, long *out)
{
	const char *p = *pp;
	long val = 0;
	int neg = 0;
	int found = 0;

	p = oplus_skip_to_number(p);

	if (!*p)
		return -EINVAL;

	if (*p == '-') {
		neg = 1;
		p++;
	}

	while (isdigit(*p)) {
		found = 1;
		val = val * 10 + (*p - '0');
		p++;
	}

	if (!found)
		return -EINVAL;

	if (neg)
		val = -val;

	*out = val;
	*pp = p;

	return 0;
}

/*
 * Handles common OPlus payloads:
 *   "p 2732 74"
 *   "p 2732 10"
 *   "2732"
 *   "2732 74"
 *
 * We boost only the PID/TID, not every number in the string.
 */
static void oplus_handle_task_payload(const char *buf)
{
	const char *p = buf;
	long pid = 0;
	long flag = 1;
	int nice;

	/* Skip leading command char like 'p' if present. */
	if (*p && !isdigit(*p) && *p != '-')
		p++;

	if (oplus_read_long(&p, &pid))
		return;

	/* Optional second number is usually flag/class. */
	oplus_read_long(&p, &flag);

	nice = oplus_nice_from_flag(flag);

	if (pid > 0) {
		oplus_boost_pid((pid_t)pid, nice);
		pr_debug("oplus_sched_real: task payload pid=%ld flag=%ld nice=%d raw=%s",
			 pid, flag, nice, buf);
	}
}

static int oplus_show(struct seq_file *m, void *v)
{
	struct oplus_node *node = m->private;

	mutex_lock(&oplus_sched_lock);
	seq_printf(m, "%d\n", node->value ? *node->value : 0);
	mutex_unlock(&oplus_sched_lock);

	return 0;
}

static int oplus_open(struct inode *inode, struct file *file)
{
	return single_open(file, oplus_show, pde_data(inode));
}

static ssize_t oplus_write(struct file *file, const char __user *ubuf,
			   size_t count, loff_t *ppos)
{
	struct oplus_node *node = pde_data(file_inode(file));
	char buf[OPLUS_BUF_SZ];
	const char *p;
	size_t len;
	long first = 0;
	int has_first = 0;

	if (!node)
		return -EINVAL;

	len = min_t(size_t, count, OPLUS_BUF_SZ - 1);
	memset(buf, 0, sizeof(buf));

	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;

	buf[len] = '\0';

	p = buf;
	if (!oplus_read_long(&p, &first))
		has_first = 1;

	mutex_lock(&oplus_sched_lock);

	switch (node->type) {
	case NODE_SCHED_ASSIST_SCENE:
		if (has_first)
			sched_assist_scene = first;
		pr_debug("oplus_sched_real: scene=%d raw=%s",
			 sched_assist_scene, buf);
		break;

	case NODE_SCHED_IMPT_TASK:
	case NODE_UX_TASK:
		if (has_first)
			sched_impt_tgid = first;
		mutex_unlock(&oplus_sched_lock);
		oplus_handle_task_payload(buf);
		return count;

	case NODE_IM_FLAG:
		if (has_first)
			im_flag = first;
		mutex_unlock(&oplus_sched_lock);
		oplus_handle_task_payload(buf);
		return count;

	case NODE_IM_FLAG_APP:
		if (has_first)
			im_flag_app = first;
		mutex_unlock(&oplus_sched_lock);
		oplus_handle_task_payload(buf);
		return count;

	case NODE_AUDIO_ENABLE:
		if (has_first)
			audio_enable = first ? 1 : 0;
		pr_debug("oplus_sched_real: audio_enable=%d raw=%s",
			 audio_enable, buf);
		break;

	case NODE_DISABLE_SETTING:
		if (has_first)
			disable_setting = first;
		pr_debug("oplus_sched_real: disable_setting=%d raw=%s",
			 disable_setting, buf);
		break;

	case NODE_SCHED_PROP:
		if (has_first)
			sched_prop = first;
		pr_debug("oplus_sched_real: sched_prop=%d raw=%s",
			 sched_prop, buf);
		break;

	case NODE_TPD_ID:
		if (has_first)
			tpd_id = first;
		pr_debug("oplus_sched_real: tpd_id=%d raw=%s",
			 tpd_id, buf);
		break;

	case NODE_TPD_CMDS:
		if (has_first)
			tpd_cmds = first;
		pr_debug("oplus_sched_real: tpd_cmds=%d raw=%s",
			 tpd_cmds, buf);
		break;
	}

	mutex_unlock(&oplus_sched_lock);

	return count;
}

static const struct proc_ops oplus_ops = {
	.proc_open = oplus_open,
	.proc_read = seq_read,
	.proc_write = oplus_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct oplus_node nodes[] = {
	{ "disable_setting", NODE_DISABLE_SETTING, &disable_setting, NULL },
	{ "im_flag", NODE_IM_FLAG, &im_flag, NULL },
	{ "im_flag_app", NODE_IM_FLAG_APP, &im_flag_app, NULL },
	{ "sched_assist_scene", NODE_SCHED_ASSIST_SCENE, &sched_assist_scene, NULL },
	{ "sched_impt_task", NODE_SCHED_IMPT_TASK, &sched_impt_tgid, NULL },
	{ "sched_prop", NODE_SCHED_PROP, &sched_prop, NULL },
	{ "tpd_cmds", NODE_TPD_CMDS, &tpd_cmds, NULL },
	{ "tpd_id", NODE_TPD_ID, &tpd_id, NULL },
	{ "ux_task", NODE_UX_TASK, &sched_impt_tgid, NULL },
	{ "ux_task_app", NODE_UX_TASK, &sched_impt_tgid, NULL },
	{ "enable", NODE_AUDIO_ENABLE, &audio_enable, NULL },
	{ "status", NODE_AUDIO_ENABLE, &audio_enable, NULL },
};

static int sched_impt_tgid_handler(struct ctl_table *table, int write,
				   void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);

	if (!ret && write && sched_impt_tgid > 0)
		oplus_boost_pid(sched_impt_tgid, OPLUS_BOOST_STRONG);

	return ret;
}

static struct ctl_table oplus_sysctls[] = {
	{
		.procname = "sched_assist_scene",
		.data = &sched_assist_scene,
		.maxlen = sizeof(int),
		.mode = 0666,
		.proc_handler = proc_dointvec,
	},
	{
		.procname = "sched_impt_tgid",
		.data = &sched_impt_tgid,
		.maxlen = sizeof(int),
		.mode = 0666,
		.proc_handler = sched_impt_tgid_handler,
	},
	{ }
};

static int __init oplus_sched_real_init(void)
{
	int i;

	root_dir = proc_mkdir("oplus_scheduler", NULL);
	if (!root_dir)
		return -ENOMEM;

	assist_dir = proc_mkdir("sched_assist", root_dir);
	if (!assist_dir)
		goto err_root;

	audio_dir = proc_mkdir("audio", assist_dir);
	if (!audio_dir)
		goto err_assist;

	for (i = 0; i < ARRAY_SIZE(nodes); i++) {
		struct proc_dir_entry *parent = assist_dir;

		if (nodes[i].type == NODE_AUDIO_ENABLE)
			parent = audio_dir;

		nodes[i].entry = proc_create_data(nodes[i].name, 0666, parent,
						  &oplus_ops, &nodes[i]);
		if (!nodes[i].entry)
			goto err_nodes;
	}

	oplus_sysctl_header = register_sysctl("kernel", oplus_sysctls);
	if (!oplus_sysctl_header)
		pr_warn("oplus_sched_real: failed to register sysctl nodes\n");

	pr_info("oplus_sched_real: real OPlus scheduler compatibility active v2\n");
	return 0;

err_nodes:
	for (i = 0; i < ARRAY_SIZE(nodes); i++) {
		if (nodes[i].entry)
			proc_remove(nodes[i].entry);
	}
	proc_remove(audio_dir);
err_assist:
	proc_remove(assist_dir);
err_root:
	proc_remove(root_dir);
	return -ENOMEM;
}

static void __exit oplus_sched_real_exit(void)
{
	int i;

	if (oplus_sysctl_header)
		unregister_sysctl_table(oplus_sysctl_header);

	for (i = 0; i < ARRAY_SIZE(nodes); i++) {
		if (nodes[i].entry)
			proc_remove(nodes[i].entry);
	}

	if (audio_dir)
		proc_remove(audio_dir);
	if (assist_dir)
		proc_remove(assist_dir);
	if (root_dir)
		proc_remove(root_dir);

	pr_info("oplus_sched_real: removed\n");
}

module_init(oplus_sched_real_init);
module_exit(oplus_sched_real_exit);

MODULE_DESCRIPTION("Functional OPlus scheduler compatibility for ColorOS");
MODULE_AUTHOR("NEESCHAL");
MODULE_LICENSE("GPL");
