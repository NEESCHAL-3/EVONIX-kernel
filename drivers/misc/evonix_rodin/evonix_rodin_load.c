// SPDX-License-Identifier: GPL-2.0
/*
 * Evonix Rodin adaptive CPU demand detector
 *
 * Measures real CPU busy-time deltas through the exported kernel cpustat API.
 * Sampling is slow while light and becomes faster only while activity exists.
 */

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>

#include "evonix_rodin_internal.h"

#define EVX_LOAD_NAME			"evonix_rodin_load"

#define EVX_LOAD_LIGHT_SAMPLE_MS		2000U
#define EVX_LOAD_MODERATE_SAMPLE_MS	750U
#define EVX_LOAD_HEAVY_SAMPLE_MS	500U
#define EVX_LOAD_WAKE_SAMPLE_MS		250U

#define EVX_LOAD_HEAVY_UP_SAMPLES	2U
#define EVX_LOAD_DOWN_SAMPLES		4U

struct evx_cpu_snapshot {
	u64 total;
	u64 idle;
	bool valid;
};

static struct evx_cpu_snapshot evx_prev_cpu[NR_CPUS];

static ATOMIC_NOTIFIER_HEAD(evx_load_notifier);
static atomic_t evx_current_load =
	ATOMIC_INIT(EVX_RODIN_LOAD_LIGHT);

static atomic64_t evx_load_samples = ATOMIC64_INIT(0);
static atomic64_t evx_load_transitions = ATOMIC64_INIT(0);

static struct proc_dir_entry *evx_load_proc;

static unsigned int evx_total_busy_pct;
static unsigned int evx_little_busy_pct;
static unsigned int evx_medium_busy_pct;
static unsigned int evx_prime_busy_pct;

static unsigned int evx_promote_samples;
static unsigned int evx_demote_samples;
static enum evx_rodin_load_tier evx_desired_load;

static void evx_load_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(evx_load_work, evx_load_workfn);

static const char *
evx_load_tier_name(enum evx_rodin_load_tier tier)
{
	switch (tier) {
	case EVX_RODIN_LOAD_LIGHT:
		return "light";
	case EVX_RODIN_LOAD_MODERATE:
		return "moderate";
	case EVX_RODIN_LOAD_HEAVY:
		return "heavy";
	default:
		return "unknown";
	}
}

enum evx_rodin_load_tier evx_rodin_get_load_tier(void)
{
	int tier = atomic_read(&evx_current_load);

	if (tier < EVX_RODIN_LOAD_LIGHT ||
	    tier >= EVX_RODIN_LOAD_TIER_MAX)
		return EVX_RODIN_LOAD_LIGHT;

	return tier;
}
EXPORT_SYMBOL_GPL(evx_rodin_get_load_tier);

int evx_rodin_register_load_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_register(
		&evx_load_notifier, nb);
}
EXPORT_SYMBOL_GPL(evx_rodin_register_load_notifier);

int evx_rodin_unregister_load_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_unregister(
		&evx_load_notifier, nb);
}
EXPORT_SYMBOL_GPL(evx_rodin_unregister_load_notifier);

static void evx_read_cpustat(int cpu, u64 *total, u64 *idle)
{
	struct kernel_cpustat stat;

	kcpustat_cpu_fetch(&stat, cpu);

	*idle =
		stat.cpustat[CPUTIME_IDLE] +
		stat.cpustat[CPUTIME_IOWAIT];

	/*
	 * Guest time is not added because it is already accounted within
	 * user and nice time.
	 */
	*total =
		stat.cpustat[CPUTIME_USER] +
		stat.cpustat[CPUTIME_NICE] +
		stat.cpustat[CPUTIME_SYSTEM] +
		stat.cpustat[CPUTIME_IDLE] +
		stat.cpustat[CPUTIME_IOWAIT] +
		stat.cpustat[CPUTIME_IRQ] +
		stat.cpustat[CPUTIME_SOFTIRQ] +
		stat.cpustat[CPUTIME_STEAL];
}

static unsigned int evx_busy_percentage(u64 busy, u64 total)
{
	u64 percentage;

	if (!total)
		return 0;

	percentage = div64_u64(busy * 100ULL, total);

	return min_t(u64, percentage, 100ULL);
}

static int evx_cluster_index(int cpu)
{
	if (cpu <= 3)
		return 0;

	if (cpu <= 6)
		return 1;

	return 2;
}

static bool evx_collect_load_sample(void)
{
	u64 cluster_busy[3] = { 0, 0, 0 };
	u64 cluster_total[3] = { 0, 0, 0 };
	u64 all_busy = 0;
	u64 all_total = 0;
	bool have_delta = false;
	int cpu;

	for_each_possible_cpu(cpu) {
		struct evx_cpu_snapshot *previous;
		u64 current_total;
		u64 current_idle;
		u64 delta_total;
		u64 delta_idle;
		u64 delta_busy;
		int cluster;

		previous = &evx_prev_cpu[cpu];

		if (!cpu_online(cpu)) {
			previous->valid = false;
			continue;
		}

		evx_read_cpustat(cpu, &current_total, &current_idle);

		if (!previous->valid ||
		    current_total < previous->total ||
		    current_idle < previous->idle) {
			previous->total = current_total;
			previous->idle = current_idle;
			previous->valid = true;
			continue;
		}

		delta_total = current_total - previous->total;
		delta_idle = current_idle - previous->idle;

		previous->total = current_total;
		previous->idle = current_idle;

		if (!delta_total)
			continue;

		if (delta_idle > delta_total)
			delta_idle = delta_total;

		delta_busy = delta_total - delta_idle;
		cluster = evx_cluster_index(cpu);

		cluster_busy[cluster] += delta_busy;
		cluster_total[cluster] += delta_total;

		all_busy += delta_busy;
		all_total += delta_total;
		have_delta = true;
	}

	if (!have_delta)
		return false;

	WRITE_ONCE(
		evx_total_busy_pct,
		evx_busy_percentage(all_busy, all_total));

	WRITE_ONCE(
		evx_little_busy_pct,
		evx_busy_percentage(
			cluster_busy[0], cluster_total[0]));

	WRITE_ONCE(
		evx_medium_busy_pct,
		evx_busy_percentage(
			cluster_busy[1], cluster_total[1]));

	WRITE_ONCE(
		evx_prime_busy_pct,
		evx_busy_percentage(
			cluster_busy[2], cluster_total[2]));

	atomic64_inc(&evx_load_samples);
	return true;
}

static enum evx_rodin_load_tier evx_classify_load(void)
{
	unsigned int total = READ_ONCE(evx_total_busy_pct);
	unsigned int little = READ_ONCE(evx_little_busy_pct);
	unsigned int medium = READ_ONCE(evx_medium_busy_pct);
	unsigned int prime = READ_ONCE(evx_prime_busy_pct);

	/*
	 * Heavy requires broad sustained pressure, strong medium-cluster
	 * pressure, or simultaneous prime and medium demand. A single short
	 * UI thread therefore does not unnecessarily unlock every cluster.
	 */
	if (total >= 55 ||
	    medium >= 65 ||
	    (prime >= 75 && medium >= 30))
		return EVX_RODIN_LOAD_HEAVY;

	if (total >= 18 ||
	    little >= 30 ||
	    medium >= 30 ||
	    prime >= 35)
		return EVX_RODIN_LOAD_MODERATE;

	return EVX_RODIN_LOAD_LIGHT;
}

static void
evx_publish_load_tier(enum evx_rodin_load_tier tier)
{
	enum evx_rodin_load_tier previous;

	previous = evx_rodin_get_load_tier();

	if (previous == tier)
		return;

	atomic_set(&evx_current_load, tier);
	atomic64_inc(&evx_load_transitions);

	pr_info(EVX_LOAD_NAME
		": tier %s -> %s total=%u little=%u medium=%u prime=%u\n",
		evx_load_tier_name(previous),
		evx_load_tier_name(tier),
		READ_ONCE(evx_total_busy_pct),
		READ_ONCE(evx_little_busy_pct),
		READ_ONCE(evx_medium_busy_pct),
		READ_ONCE(evx_prime_busy_pct));

	atomic_notifier_call_chain(
		&evx_load_notifier, tier, NULL);
}

static unsigned int
evx_load_sample_delay(enum evx_rodin_load_tier tier)
{
	switch (tier) {
	case EVX_RODIN_LOAD_HEAVY:
		return EVX_LOAD_HEAVY_SAMPLE_MS;
	case EVX_RODIN_LOAD_MODERATE:
		return EVX_LOAD_MODERATE_SAMPLE_MS;
	case EVX_RODIN_LOAD_LIGHT:
	default:
		return EVX_LOAD_LIGHT_SAMPLE_MS;
	}
}

static void evx_load_workfn(struct work_struct *work)
{
	enum evx_rodin_load_tier current_tier;
	enum evx_rodin_load_tier desired;
	enum evx_rodin_load_tier activity;
	unsigned int delay_ms;
	unsigned int required_samples;

	if (!evx_collect_load_sample()) {
		delay_ms = EVX_LOAD_HEAVY_SAMPLE_MS;
		goto out_schedule;
	}

	current_tier = evx_rodin_get_load_tier();
	desired = evx_classify_load();
	WRITE_ONCE(evx_desired_load, desired);

	if (desired > current_tier) {
		evx_demote_samples = 0;
		evx_promote_samples++;

		required_samples =
			desired == EVX_RODIN_LOAD_HEAVY ?
			EVX_LOAD_HEAVY_UP_SAMPLES : 1U;

		if (evx_promote_samples >= required_samples) {
			evx_promote_samples = 0;
			evx_publish_load_tier(desired);
			current_tier = desired;
		}
	} else if (desired < current_tier) {
		evx_promote_samples = 0;
		evx_demote_samples++;

		if (evx_demote_samples >= EVX_LOAD_DOWN_SAMPLES) {
			evx_demote_samples = 0;
			evx_publish_load_tier(desired);
			current_tier = desired;
		}
	} else {
		evx_promote_samples = 0;
		evx_demote_samples = 0;
	}

	/*
	 * Sample quickly while a possible promotion is developing. Once light
	 * activity is stable, return to the low-overhead two-second interval.
	 */
	activity = max_t(enum evx_rodin_load_tier,
			 current_tier, desired);

	delay_ms = evx_load_sample_delay(activity);

out_schedule:
	schedule_delayed_work(
		&evx_load_work,
		msecs_to_jiffies(delay_ms));
}

static int evx_load_state_changed(struct notifier_block *nb,
				  unsigned long state,
				  void *unused)
{
	if (state != EVX_RODIN_IDLE)
		mod_delayed_work(
			system_wq,
			&evx_load_work,
			msecs_to_jiffies(
				EVX_LOAD_WAKE_SAMPLE_MS));

	return NOTIFY_OK;
}

static struct notifier_block evx_load_state_notifier = {
	.notifier_call = evx_load_state_changed,
};

static int evx_load_stats_show(struct seq_file *m, void *v)
{
	enum evx_rodin_load_tier current_tier;

	current_tier = evx_rodin_get_load_tier();

	seq_printf(m, "tier=%s\n",
		   evx_load_tier_name(current_tier));
	seq_printf(m, "tier_id=%d\n", current_tier);

	seq_printf(m, "desired_tier=%s\n",
		   evx_load_tier_name(
			   READ_ONCE(evx_desired_load)));

	seq_printf(m, "total_busy_pct=%u\n",
		   READ_ONCE(evx_total_busy_pct));
	seq_printf(m, "little_busy_pct=%u\n",
		   READ_ONCE(evx_little_busy_pct));
	seq_printf(m, "medium_busy_pct=%u\n",
		   READ_ONCE(evx_medium_busy_pct));
	seq_printf(m, "prime_busy_pct=%u\n",
		   READ_ONCE(evx_prime_busy_pct));

	seq_printf(m, "promote_samples=%u\n",
		   READ_ONCE(evx_promote_samples));
	seq_printf(m, "demote_samples=%u\n",
		   READ_ONCE(evx_demote_samples));

	seq_printf(m, "samples=%lld\n",
		   (long long)atomic64_read(
			   &evx_load_samples));
	seq_printf(m, "transitions=%lld\n",
		   (long long)atomic64_read(
			   &evx_load_transitions));

	return 0;
}

static int evx_load_stats_open(struct inode *inode,
			       struct file *file)
{
	return single_open(file, evx_load_stats_show, NULL);
}

static const struct proc_ops evx_load_stats_fops = {
	.proc_open	= evx_load_stats_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int __init evx_load_init(void)
{
	struct proc_dir_entry *parent;
	int ret;

	evx_desired_load = EVX_RODIN_LOAD_LIGHT;

	ret = evx_rodin_register_state_notifier(
		&evx_load_state_notifier);
	if (ret)
		return ret;

	parent = evx_rodin_get_proc_dir();

	if (parent)
		evx_load_proc =
			proc_create(
				"load_stats",
				0444,
				parent,
				&evx_load_stats_fops);

	schedule_delayed_work(
		&evx_load_work,
		msecs_to_jiffies(
			EVX_LOAD_HEAVY_SAMPLE_MS));

	pr_info(EVX_LOAD_NAME
		": adaptive demand detector initialized\n");

	return 0;
}

static void __exit evx_load_exit(void)
{
	evx_rodin_unregister_state_notifier(
		&evx_load_state_notifier);

	cancel_delayed_work_sync(&evx_load_work);

	if (evx_load_proc)
		proc_remove(evx_load_proc);

	evx_load_proc = NULL;

	pr_info(EVX_LOAD_NAME ": removed\n");
}

late_initcall(evx_load_init);
module_exit(evx_load_exit);

MODULE_DESCRIPTION("Evonix Rodin adaptive CPU demand detector");
MODULE_AUTHOR("NEESCHAL");
MODULE_LICENSE("GPL");
