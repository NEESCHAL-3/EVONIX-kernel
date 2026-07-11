// SPDX-License-Identifier: GPL-2.0
/*
 * Evonix Rodin CPU QoS Controller
 *
 * Applies short kernel-owned minimum-frequency requests according to the
 * shared Rodin state machine. Existing maximum constraints, including thermal
 * cooling limits, always remain authoritative.
 */

#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/pm_qos.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>

#include "evonix_rodin_internal.h"

#define EVX_QOS_NAME			"evonix_rodin_qos"
#define EVX_QOS_DOMAINS			3
#define EVX_QOS_RETRY_MS		500U
#define EVX_QOS_MAX_RETRIES		40U

struct evx_qos_domain {
	unsigned int cpu;
	const char *name;
	struct cpufreq_policy *policy;
	struct freq_qos_request min_request;
	s32 requested_khz;
	s32 applied_khz;
	bool active;
};

static struct evx_qos_domain evx_domains[EVX_QOS_DOMAINS] = {
	{
		.cpu = 0,
		.name = "policy0",
	},
	{
		.cpu = 4,
		.name = "policy4",
	},
	{
		.cpu = 7,
		.name = "policy7",
	},
};

/*
 * Frequency values are in kHz.
 *
 * CPU7 is intentionally untouched during normal interaction. Rodin's
 * policy4 domain should carry ordinary UI work more efficiently.
 */
static const s32
evx_state_min_khz[EVX_RODIN_STATE_MAX][EVX_QOS_DOMAINS] = {
	[EVX_RODIN_IDLE] = {
		0, 0, 0,
	},
	[EVX_RODIN_INTERACTIVE] = {
		700000, 1200000, 0,
	},
	[EVX_RODIN_SUSTAINED] = {
		600000, 1400000, 0,
	},
	[EVX_RODIN_FRAME_PRESSURE] = {
		800000, 1600000, 1200000,
	},
};

static DEFINE_MUTEX(evx_qos_lock);

static struct proc_dir_entry *evx_qos_proc;
static unsigned int evx_qos_retry_count;
static bool evx_qos_ready;

static void evx_qos_apply_workfn(struct work_struct *work);
static void evx_qos_retry_workfn(struct work_struct *work);

static DECLARE_WORK(evx_qos_apply_work, evx_qos_apply_workfn);
static DECLARE_DELAYED_WORK(evx_qos_retry_work, evx_qos_retry_workfn);

static s32 evx_qos_limit_request(struct evx_qos_domain *domain,
                                 s32 requested_khz)
{
        s32 effective_max;

        if (!domain->policy || requested_khz <= 0)
                return FREQ_QOS_MIN_DEFAULT_VALUE;

        requested_khz =
                clamp_t(s32, requested_khz,
                        domain->policy->cpuinfo.min_freq,
                        domain->policy->cpuinfo.max_freq);

        /*
         * Existing maximum constraints always remain authoritative.
         * Thermal, firmware and ROM maximum limits are never bypassed.
         */
        effective_max =
                freq_qos_read_value(&domain->policy->constraints,
                                    FREQ_QOS_MAX);

        if (effective_max > 0 && requested_khz > effective_max)
                requested_khz = effective_max;

        return requested_khz;
}

static void evx_qos_apply_workfn(struct work_struct *work)
{
	enum evx_rodin_state state;
	int i;

	mutex_lock(&evx_qos_lock);

	if (!evx_qos_ready)
		goto out_unlock;

	state = evx_rodin_get_state();

	for (i = 0; i < EVX_QOS_DOMAINS; i++) {
		struct evx_qos_domain *domain = &evx_domains[i];
		s32 requested;
		s32 target;
		int ret;

		requested = evx_state_min_khz[state][i];
		target = evx_qos_limit_request(domain, requested);

		domain->requested_khz = requested;

		if (target == domain->applied_khz)
			continue;

		ret = freq_qos_update_request(&domain->min_request, target);
		if (ret < 0) {
			pr_warn_ratelimited(
				EVX_QOS_NAME
				": %s update failed target=%d ret=%d\n",
				domain->name, target, ret);
			continue;
		}

		domain->applied_khz = target;
	}

out_unlock:
	mutex_unlock(&evx_qos_lock);
}

static int evx_qos_state_changed(struct notifier_block *nb,
				 unsigned long state,
				 void *unused)
{
	/*
	 * The state notifier is atomic. Queue all QoS work into process context.
	 */
	schedule_work(&evx_qos_apply_work);
	return NOTIFY_OK;
}

void evx_rodin_qos_refresh(void)
{
	schedule_work(&evx_qos_apply_work);
}

static struct notifier_block evx_qos_state_notifier = {
	.notifier_call = evx_qos_state_changed,
};

static bool evx_qos_try_initialize_domains(void)
{
	bool all_ready = true;
	int i;

	for (i = 0; i < EVX_QOS_DOMAINS; i++) {
		struct evx_qos_domain *domain = &evx_domains[i];
		int ret;

		if (domain->active)
			continue;

		domain->policy = cpufreq_cpu_get(domain->cpu);
		if (!domain->policy) {
			all_ready = false;
			continue;
		}

		ret = freq_qos_add_request(
			&domain->policy->constraints,
			&domain->min_request,
			FREQ_QOS_MIN,
			FREQ_QOS_MIN_DEFAULT_VALUE);

		if (ret < 0) {
			pr_warn(EVX_QOS_NAME
				": failed adding %s request: %d\n",
				domain->name, ret);

			cpufreq_cpu_put(domain->policy);
			domain->policy = NULL;
			all_ready = false;
			continue;
		}

		domain->requested_khz = 0;
		domain->applied_khz = FREQ_QOS_MIN_DEFAULT_VALUE;
		domain->active = true;

		pr_info(EVX_QOS_NAME
			": attached %s cpu=%u hw=%u-%u kHz\n",
			domain->name,
			domain->cpu,
			domain->policy->cpuinfo.min_freq,
			domain->policy->cpuinfo.max_freq);
	}

	return all_ready;
}

static void evx_qos_retry_workfn(struct work_struct *work)
{
	bool ready;

	mutex_lock(&evx_qos_lock);
	ready = evx_qos_try_initialize_domains();

	if (ready)
		evx_qos_ready = true;

	mutex_unlock(&evx_qos_lock);

	if (ready) {
		schedule_work(&evx_qos_apply_work);
		pr_info(EVX_QOS_NAME ": all CPU domains ready\n");
		return;
	}

	evx_qos_retry_count++;

	if (evx_qos_retry_count < EVX_QOS_MAX_RETRIES) {
		schedule_delayed_work(&evx_qos_retry_work,
				      msecs_to_jiffies(EVX_QOS_RETRY_MS));
	} else {
		pr_warn(EVX_QOS_NAME
			": CPU policies not ready after %u retries\n",
			evx_qos_retry_count);
	}
}

static int evx_qos_stats_show(struct seq_file *m, void *v)
{
	enum evx_rodin_state state;
	unsigned int thermal_pressure;
	int i;

	mutex_lock(&evx_qos_lock);

	state = evx_rodin_get_state();
	thermal_pressure = evx_rodin_get_thermal_pressure_pct();

	seq_printf(m, "ready=%d\n", evx_qos_ready ? 1 : 0);
	seq_printf(m, "state=%d\n", state);
	seq_printf(m, "thermal_pressure_pct=%u\n", thermal_pressure);

	for (i = 0; i < EVX_QOS_DOMAINS; i++) {
		struct evx_qos_domain *domain = &evx_domains[i];

		seq_printf(m, "%s_active=%d\n",
			   domain->name, domain->active ? 1 : 0);
		seq_printf(m, "%s_requested_khz=%d\n",
			   domain->name, domain->requested_khz);
		seq_printf(m, "%s_applied_khz=%d\n",
			   domain->name, domain->applied_khz);

		if (domain->policy) {
			seq_printf(m, "%s_effective_min_khz=%d\n",
				   domain->name,
				   freq_qos_read_value(
					   &domain->policy->constraints,
					   FREQ_QOS_MIN));
			seq_printf(m, "%s_effective_max_khz=%d\n",
				   domain->name,
				   freq_qos_read_value(
					   &domain->policy->constraints,
					   FREQ_QOS_MAX));
		}
	}

	mutex_unlock(&evx_qos_lock);
	return 0;
}

static int evx_qos_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, evx_qos_stats_show, NULL);
}

static const struct proc_ops evx_qos_stats_fops = {
	.proc_open	= evx_qos_stats_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int __init evx_qos_init(void)
{
	struct proc_dir_entry *parent;
	int ret;

	ret = evx_rodin_register_state_notifier(
		&evx_qos_state_notifier);
	if (ret)
		return ret;

	parent = evx_rodin_get_proc_dir();
	if (parent)
		evx_qos_proc =
			proc_create("qos_stats", 0444, parent,
				    &evx_qos_stats_fops);

	schedule_delayed_work(&evx_qos_retry_work, 0);

	pr_info(EVX_QOS_NAME
		": thermal-aware CPU QoS controller initialized\n");

	return 0;
}

static void __exit evx_qos_exit(void)
{
	int i;

	evx_rodin_unregister_state_notifier(
		&evx_qos_state_notifier);

	cancel_delayed_work_sync(&evx_qos_retry_work);
	cancel_work_sync(&evx_qos_apply_work);

	if (evx_qos_proc)
		proc_remove(evx_qos_proc);

	mutex_lock(&evx_qos_lock);

	for (i = 0; i < EVX_QOS_DOMAINS; i++) {
		struct evx_qos_domain *domain = &evx_domains[i];

		if (domain->active) {
			freq_qos_remove_request(&domain->min_request);
			domain->active = false;
		}

		if (domain->policy) {
			cpufreq_cpu_put(domain->policy);
			domain->policy = NULL;
		}
	}

	evx_qos_ready = false;

	mutex_unlock(&evx_qos_lock);

	pr_info(EVX_QOS_NAME ": removed\n");
}

late_initcall(evx_qos_init);
module_exit(evx_qos_exit);

MODULE_DESCRIPTION("Evonix Rodin thermal-aware CPU frequency QoS");
MODULE_AUTHOR("NEESCHAL");
MODULE_LICENSE("GPL");
