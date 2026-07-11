// SPDX-License-Identifier: GPL-2.0
/*
 * Evonix Rodin balanced thermal controller
 *
 * Balanced mode owns conservative CPU maximum-frequency requests and uses
 * battery temperature as a soft thermal signal. High Performance releases
 * only Evonix-owned maximum constraints. Hardware and vendor protections
 * always remain authoritative.
 */

#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/pm_qos.h>
#include <linux/power_supply.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "evonix_rodin_internal.h"

#define EVX_THERMAL_NAME		"evonix_rodin_thermal"
#define EVX_THERMAL_DOMAINS		3
#define EVX_THERMAL_REFRESH_MS		30000U
#define EVX_THERMAL_RETRY_MS		500U
#define EVX_THERMAL_MAX_RETRIES		40U
#define EVX_THERMAL_EVENT_MIN_MS	2000U
#define EVX_THERMAL_RELEASE_DWELL_MS	20000U

/* POWER_SUPPLY_PROP_TEMP is reported in tenths of one degree Celsius. */
#define EVX_TEMP_WARM_ENTER		380
#define EVX_TEMP_WARM_EXIT		370
#define EVX_TEMP_HOT_ENTER		400
#define EVX_TEMP_HOT_EXIT		390

enum evx_thermal_level {
	EVX_THERMAL_NORMAL = 0,
	EVX_THERMAL_WARM,
	EVX_THERMAL_HOT,
	EVX_THERMAL_LEVEL_MAX,
};

struct evx_thermal_domain {
	unsigned int cpu;
	const char *name;
	struct cpufreq_policy *policy;
	struct freq_qos_request max_request;
	s32 requested_khz;
	s32 applied_khz;
	bool active;
};

static struct evx_thermal_domain
evx_thermal_domains[EVX_THERMAL_DOMAINS] = {
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
 * Balanced maximum-frequency envelopes in kHz.
 *
 * The workload tier is measured from actual CPU busy-time deltas.
 * Interactive workload receives at least moderate headroom immediately,
 * while sustained heavy demand can release the Evonix cap below 38C.
 */
static const s32
evx_balanced_max_khz
[EVX_THERMAL_LEVEL_MAX]
[EVX_RODIN_LOAD_TIER_MAX]
[EVX_THERMAL_DOMAINS] = {
	[EVX_THERMAL_NORMAL] = {
		[EVX_RODIN_LOAD_LIGHT] = {
			1200000, 1600000, 1600000,
		},
		[EVX_RODIN_LOAD_MODERATE] = {
			1800000, 2500000, 2600000,
		},
		[EVX_RODIN_LOAD_HEAVY] = {
			FREQ_QOS_MAX_DEFAULT_VALUE,
			FREQ_QOS_MAX_DEFAULT_VALUE,
			FREQ_QOS_MAX_DEFAULT_VALUE,
		},
	},
	[EVX_THERMAL_WARM] = {
		[EVX_RODIN_LOAD_LIGHT] = {
			1200000, 1600000, 1400000,
		},
		[EVX_RODIN_LOAD_MODERATE] = {
			1700000, 2400000, 2500000,
		},
		[EVX_RODIN_LOAD_HEAVY] = {
			1900000, 2700000, 2800000,
		},
	},
	[EVX_THERMAL_HOT] = {
		[EVX_RODIN_LOAD_LIGHT] = {
			1100000, 1500000, 1200000,
		},
		[EVX_RODIN_LOAD_MODERATE] = {
			1500000, 2100000, 2200000,
		},
		[EVX_RODIN_LOAD_HEAVY] = {
			1700000, 2400000, 2500000,
		},
	},
};

static DEFINE_MUTEX(evx_thermal_lock);

static struct proc_dir_entry *evx_mode_proc;
static struct proc_dir_entry *evx_thermal_proc;

static enum evx_thermal_level evx_thermal_level;
static unsigned long evx_level_changed_jiffies;
static unsigned int evx_policy_retry_count;
static int evx_battery_temp_deci_c;
static int evx_last_temp_error;
static bool evx_high_performance;
static bool evx_domains_ready;
static bool evx_psy_notifier_registered;
static unsigned long evx_last_temp_sample_jiffies;

static void evx_thermal_workfn(struct work_struct *work);
static void evx_thermal_apply_workfn(struct work_struct *work);

static DECLARE_DELAYED_WORK(evx_thermal_work, evx_thermal_workfn);
static DECLARE_WORK(evx_thermal_apply_work,
		    evx_thermal_apply_workfn);

static const char *evx_thermal_level_name(enum evx_thermal_level level)
{
	switch (level) {
	case EVX_THERMAL_NORMAL:
		return "normal";
	case EVX_THERMAL_WARM:
		return "warm";
	case EVX_THERMAL_HOT:
		return "hot";
	default:
		return "unknown";
	}
}

static int evx_read_battery_temp(int *temp)
{
	struct power_supply *battery;
	union power_supply_propval value;
	int ret;

	battery = power_supply_get_by_name("battery");
	if (!battery)
		return -ENODEV;

	ret = power_supply_get_property(
		battery, POWER_SUPPLY_PROP_TEMP, &value);

	power_supply_put(battery);

	if (ret)
		return ret;

	/*
	 * Reject impossible or clearly invalid readings rather than making
	 * performance decisions from a broken thermal source.
	 */
	if (value.intval <= 0 || value.intval >= 900)
		return -ERANGE;

	*temp = value.intval;
	return 0;
}

static bool evx_release_dwell_complete(void)
{
	return time_after_eq(
		jiffies,
		evx_level_changed_jiffies +
		msecs_to_jiffies(EVX_THERMAL_RELEASE_DWELL_MS));
}

static void evx_update_thermal_level_locked(int temp)
{
	enum evx_thermal_level next = evx_thermal_level;

	switch (evx_thermal_level) {
	case EVX_THERMAL_NORMAL:
		if (temp >= EVX_TEMP_HOT_ENTER)
			next = EVX_THERMAL_HOT;
		else if (temp >= EVX_TEMP_WARM_ENTER)
			next = EVX_THERMAL_WARM;
		break;

	case EVX_THERMAL_WARM:
		if (temp >= EVX_TEMP_HOT_ENTER) {
			next = EVX_THERMAL_HOT;
		} else if (temp <= EVX_TEMP_WARM_EXIT &&
			   evx_release_dwell_complete()) {
			next = EVX_THERMAL_NORMAL;
		}
		break;

	case EVX_THERMAL_HOT:
		if (temp <= EVX_TEMP_HOT_EXIT &&
		    evx_release_dwell_complete()) {
			if (temp <= EVX_TEMP_WARM_EXIT)
				next = EVX_THERMAL_NORMAL;
			else
				next = EVX_THERMAL_WARM;
		}
		break;

	default:
		next = EVX_THERMAL_NORMAL;
		break;
	}

	if (next != evx_thermal_level) {
		pr_info(EVX_THERMAL_NAME
			": level %s -> %s temp=%d.%dC\n",
			evx_thermal_level_name(evx_thermal_level),
			evx_thermal_level_name(next),
			temp / 10, temp % 10);

		evx_thermal_level = next;
		evx_level_changed_jiffies = jiffies;
	}
}

static bool evx_try_initialize_domains_locked(void)
{
	bool all_ready = true;
	int i;

	for (i = 0; i < EVX_THERMAL_DOMAINS; i++) {
		struct evx_thermal_domain *domain =
			&evx_thermal_domains[i];
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
			&domain->max_request,
			FREQ_QOS_MAX,
			FREQ_QOS_MAX_DEFAULT_VALUE);

		if (ret < 0) {
			pr_warn(EVX_THERMAL_NAME
				": failed adding %s max request: %d\n",
				domain->name, ret);

			cpufreq_cpu_put(domain->policy);
			domain->policy = NULL;
			all_ready = false;
			continue;
		}

		domain->requested_khz =
			FREQ_QOS_MAX_DEFAULT_VALUE;
		domain->applied_khz =
			FREQ_QOS_MAX_DEFAULT_VALUE;
		domain->active = true;

		pr_info(EVX_THERMAL_NAME
			": attached %s cpu=%u hw=%u-%u kHz\n",
			domain->name,
			domain->cpu,
			domain->policy->cpuinfo.min_freq,
			domain->policy->cpuinfo.max_freq);
	}

	return all_ready;
}

static enum evx_rodin_load_tier
evx_effective_load_tier(void)
{
	enum evx_rodin_load_tier tier;
	enum evx_rodin_state state;

	tier = evx_rodin_get_load_tier();
	state = evx_rodin_get_state();

	/*
	 * Touch/UI activity receives moderate headroom immediately instead of
	 * waiting for the slower sustained-load detector.
	 */
	if (state == EVX_RODIN_INTERACTIVE &&
	    tier < EVX_RODIN_LOAD_MODERATE)
		tier = EVX_RODIN_LOAD_MODERATE;

	/*
	 * Sustained and frame-pressure states represent latency-sensitive
	 * heavy work and therefore receive the heavy envelope.
	 */
	if (state == EVX_RODIN_SUSTAINED ||
	    state == EVX_RODIN_FRAME_PRESSURE)
		tier = EVX_RODIN_LOAD_HEAVY;

	if (tier < EVX_RODIN_LOAD_LIGHT ||
	    tier >= EVX_RODIN_LOAD_TIER_MAX)
		tier = EVX_RODIN_LOAD_LIGHT;

	return tier;
}

static bool evx_apply_maximums_locked(void)
{
	enum evx_rodin_load_tier load_tier;
	bool changed = false;
	int i;

	if (!evx_domains_ready)
		return false;

	load_tier = evx_effective_load_tier();

	for (i = 0; i < EVX_THERMAL_DOMAINS; i++) {
		struct evx_thermal_domain *domain =
			&evx_thermal_domains[i];
		s32 effective_min;
		s32 target;
		int ret;

		if (evx_high_performance) {
			target = FREQ_QOS_MAX_DEFAULT_VALUE;
		} else {
			target =
				evx_balanced_max_khz
				[evx_thermal_level]
				[load_tier]
				[i];

			if (target != FREQ_QOS_MAX_DEFAULT_VALUE) {
				target = clamp_t(
					s32,
					target,
					domain->policy->cpuinfo.min_freq,
					domain->policy->cpuinfo.max_freq);

				/*
				 * Never place an Evonix soft maximum below an
				 * active UI or frame minimum-frequency request.
				 */
				effective_min = freq_qos_read_value(
					&domain->policy->constraints,
					FREQ_QOS_MIN);

				if (effective_min > target)
					target = effective_min;
			}
		}

		domain->requested_khz = target;

		if (target == domain->applied_khz)
			continue;

		ret = freq_qos_update_request(
			&domain->max_request, target);

		if (ret < 0) {
			pr_warn_ratelimited(
				EVX_THERMAL_NAME
				": %s max update failed target=%d ret=%d\n",
				domain->name, target, ret);
			continue;
		}

		domain->applied_khz = target;
		changed = true;
	}

	return changed;
}

static void evx_thermal_apply_workfn(struct work_struct *work)
{
	bool qos_refresh;

	mutex_lock(&evx_thermal_lock);
	qos_refresh = evx_apply_maximums_locked();
	mutex_unlock(&evx_thermal_lock);

	if (qos_refresh)
		evx_rodin_qos_refresh();
}

static void evx_thermal_workfn(struct work_struct *work)
{
	unsigned long next_delay;
	bool qos_refresh = false;
	int temp;
	int ret;

	mutex_lock(&evx_thermal_lock);

	if (!evx_domains_ready) {
		evx_domains_ready =
			evx_try_initialize_domains_locked();

		if (!evx_domains_ready) {
			evx_policy_retry_count++;

			if (evx_policy_retry_count <
			    EVX_THERMAL_MAX_RETRIES)
				next_delay =
					msecs_to_jiffies(
						EVX_THERMAL_RETRY_MS);
			else
				next_delay =
					msecs_to_jiffies(
						EVX_THERMAL_REFRESH_MS);

			goto out_schedule;
		}

		pr_info(EVX_THERMAL_NAME
			": all CPU domains ready\n");
	}

	ret = evx_read_battery_temp(&temp);
	evx_last_temp_error = ret;
	WRITE_ONCE(evx_last_temp_sample_jiffies, jiffies);

	if (!ret) {
		evx_battery_temp_deci_c = temp;
		evx_update_thermal_level_locked(temp);
	}

	qos_refresh = evx_apply_maximums_locked();
	next_delay = msecs_to_jiffies(EVX_THERMAL_REFRESH_MS);

out_schedule:
	mutex_unlock(&evx_thermal_lock);

	if (qos_refresh)
		evx_rodin_qos_refresh();

	schedule_delayed_work(&evx_thermal_work, next_delay);
}

static unsigned long evx_thermal_event_delay(void)
{
	unsigned long last;
	unsigned long next;

	last = READ_ONCE(evx_last_temp_sample_jiffies);
	if (!last)
		return 0;

	next = last +
		msecs_to_jiffies(EVX_THERMAL_EVENT_MIN_MS);

	if (time_before(jiffies, next))
		return next - jiffies;

	return 0;
}

static int evx_power_supply_changed(struct notifier_block *nb,
				    unsigned long event,
				    void *data)
{
	struct power_supply *psy = data;

	if (event != PSY_EVENT_PROP_CHANGED ||
	    !psy || !psy->desc || !psy->desc->name)
		return NOTIFY_OK;

	if (strcmp(psy->desc->name, "battery"))
		return NOTIFY_OK;

	mod_delayed_work(system_wq, &evx_thermal_work,
			 evx_thermal_event_delay());
	return NOTIFY_OK;
}

static struct notifier_block evx_power_supply_notifier = {
	.notifier_call = evx_power_supply_changed,
};

static void evx_unregister_power_supply_notifier(void)
{
	if (!evx_psy_notifier_registered)
		return;

	power_supply_unreg_notifier(
		&evx_power_supply_notifier);
	evx_psy_notifier_registered = false;
}


static int evx_load_tier_changed(struct notifier_block *nb,
				 unsigned long tier,
				 void *unused)
{
	/*
	 * The load notifier is atomic. Apply frequency QoS changes later from
	 * process context instead of touching constraints inside the callback.
	 */
	schedule_work(&evx_thermal_apply_work);
	return NOTIFY_OK;
}

static struct notifier_block evx_load_notifier = {
	.notifier_call = evx_load_tier_changed,
};


static int evx_workload_state_changed(struct notifier_block *nb,
				      unsigned long state,
				      void *unused)
{
	/*
	 * Re-evaluate maximums whenever short UI floors change. This lets
	 * active frames retain their minimum frequency while thermal caps
	 * tighten again immediately after the workload decays.
	 */
	schedule_work(&evx_thermal_apply_work);
	return NOTIFY_OK;
}

static struct notifier_block evx_workload_notifier = {
	.notifier_call = evx_workload_state_changed,
};

static int evx_mode_show(struct seq_file *m, void *v)
{
	mutex_lock(&evx_thermal_lock);

	seq_printf(m, "%s\n",
		   evx_high_performance ?
		   "high_performance" : "balanced");

	mutex_unlock(&evx_thermal_lock);
	return 0;
}

static int evx_mode_open(struct inode *inode, struct file *file)
{
	return single_open(file, evx_mode_show, NULL);
}

static ssize_t evx_mode_write(struct file *file,
			      const char __user *buffer,
			      size_t count,
			      loff_t *ppos)
{
	char command[32];
	size_t length;
	bool high_performance;

	if (!count)
		return 0;

	length = min_t(size_t, count, sizeof(command) - 1);

	if (copy_from_user(command, buffer, length))
		return -EFAULT;

	command[length] = '\0';
	strim(command);

	if (!strcmp(command, "1") ||
	    !strcmp(command, "high_performance") ||
	    !strcmp(command, "performance")) {
		high_performance = true;
	} else if (!strcmp(command, "0") ||
		   !strcmp(command, "balanced")) {
		high_performance = false;
	} else {
		return -EINVAL;
	}

	mutex_lock(&evx_thermal_lock);

	if (evx_high_performance != high_performance) {
		evx_high_performance = high_performance;

		pr_info(EVX_THERMAL_NAME ": mode=%s\n",
			evx_high_performance ?
			"high_performance" : "balanced");
	}

	mutex_unlock(&evx_thermal_lock);

	schedule_work(&evx_thermal_apply_work);
	return count;
}

static const struct proc_ops evx_mode_fops = {
	.proc_open	= evx_mode_open,
	.proc_read	= seq_read,
	.proc_write	= evx_mode_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int evx_thermal_stats_show(struct seq_file *m, void *v)
{
	int i;

	mutex_lock(&evx_thermal_lock);

	seq_printf(m, "mode=%s\n",
		   evx_high_performance ?
		   "high_performance" : "balanced");
	seq_printf(m, "battery_temp_deci_c=%d\n",
		   evx_battery_temp_deci_c);
	seq_printf(m, "battery_temp_error=%d\n",
		   evx_last_temp_error);
	seq_printf(m, "thermal_level=%s\n",
		   evx_thermal_level_name(evx_thermal_level));
	seq_printf(m, "domains_ready=%d\n",
		   evx_domains_ready ? 1 : 0);
	seq_printf(m, "detected_load_tier=%d\n",
		   evx_rodin_get_load_tier());
	seq_printf(m, "effective_load_tier=%d\n",
		   evx_effective_load_tier());

	for (i = 0; i < EVX_THERMAL_DOMAINS; i++) {
		struct evx_thermal_domain *domain =
			&evx_thermal_domains[i];

		seq_printf(m, "%s_active=%d\n",
			   domain->name,
			   domain->active ? 1 : 0);
		seq_printf(m, "%s_requested_max_khz=%d\n",
			   domain->name,
			   domain->requested_khz);
		seq_printf(m, "%s_applied_max_khz=%d\n",
			   domain->name,
			   domain->applied_khz);

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

	mutex_unlock(&evx_thermal_lock);
	return 0;
}

static int evx_thermal_stats_open(struct inode *inode,
				  struct file *file)
{
	return single_open(file, evx_thermal_stats_show, NULL);
}

static const struct proc_ops evx_thermal_stats_fops = {
	.proc_open	= evx_thermal_stats_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int __init evx_thermal_init(void)
{
	struct proc_dir_entry *parent;
	int ret;

	evx_level_changed_jiffies = jiffies;

	ret = power_supply_reg_notifier(
		&evx_power_supply_notifier);
	if (ret) {
		pr_warn(EVX_THERMAL_NAME
			": battery notifier unavailable: %d\n", ret);
	} else {
		evx_psy_notifier_registered = true;
	}

	ret = evx_rodin_register_load_notifier(
		&evx_load_notifier);
	if (ret) {
		evx_unregister_power_supply_notifier();
		return ret;
	}

	ret = evx_rodin_register_state_notifier(
		&evx_workload_notifier);
	if (ret) {
		evx_rodin_unregister_load_notifier(
			&evx_load_notifier);
		evx_unregister_power_supply_notifier();
		return ret;
	}

	parent = evx_rodin_get_proc_dir();

	if (parent) {
		evx_mode_proc =
			proc_create("mode", 0644, parent,
				    &evx_mode_fops);

		evx_thermal_proc =
			proc_create("thermal_stats", 0444, parent,
				    &evx_thermal_stats_fops);
	}

	schedule_delayed_work(&evx_thermal_work, 0);

	pr_info(EVX_THERMAL_NAME
		": balanced controller initialized target=38.0C\n");

	return 0;
}

static void __exit evx_thermal_exit(void)
{
	int i;

	evx_rodin_unregister_state_notifier(
		&evx_workload_notifier);
	evx_rodin_unregister_load_notifier(
		&evx_load_notifier);
	evx_unregister_power_supply_notifier();

	cancel_delayed_work_sync(&evx_thermal_work);
	cancel_work_sync(&evx_thermal_apply_work);

	if (evx_mode_proc)
		proc_remove(evx_mode_proc);

	if (evx_thermal_proc)
		proc_remove(evx_thermal_proc);

	mutex_lock(&evx_thermal_lock);

	for (i = 0; i < EVX_THERMAL_DOMAINS; i++) {
		struct evx_thermal_domain *domain =
			&evx_thermal_domains[i];

		if (domain->active) {
			freq_qos_remove_request(
				&domain->max_request);
			domain->active = false;
		}

		if (domain->policy) {
			cpufreq_cpu_put(domain->policy);
			domain->policy = NULL;
		}
	}

	evx_domains_ready = false;

	mutex_unlock(&evx_thermal_lock);

	pr_info(EVX_THERMAL_NAME ": removed\n");
}

late_initcall(evx_thermal_init);
module_exit(evx_thermal_exit);

MODULE_DESCRIPTION("Evonix Rodin balanced thermal controller");
MODULE_AUTHOR("NEESCHAL");
MODULE_LICENSE("GPL");
