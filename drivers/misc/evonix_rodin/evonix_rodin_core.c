// SPDX-License-Identifier: GPL-2.0
/*
 * Evonix Rodin Optimizer Core
 *
 * Shared event-driven state machine for scheduler, input, CPU QoS,
 * thermal, block I/O, memory and network optimization.
 *
 * This core does not alter hardware policy by itself.
 */

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/topology.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "evonix_rodin_internal.h"

#define EVX_RODIN_NAME "evonix_rodin_core"
#define EVX_MIN_HOLD_MS 20U
#define EVX_MAX_HOLD_MS 5000U

static DEFINE_SPINLOCK(evx_state_lock);
static atomic_t evx_current_state = ATOMIC_INIT(EVX_RODIN_IDLE);
static unsigned long evx_state_expires;

static struct proc_dir_entry *evx_proc_dir;
static struct proc_dir_entry *evx_proc_status;

static const char * const evx_state_names[EVX_RODIN_STATE_MAX] = {
	[EVX_RODIN_IDLE] = "IDLE",
	[EVX_RODIN_INTERACTIVE] = "INTERACTIVE",
	[EVX_RODIN_SUSTAINED] = "SUSTAINED",
	[EVX_RODIN_FRAME_PRESSURE] = "FRAME_PRESSURE",
	[EVX_RODIN_THERMAL_GUARD] = "THERMAL_GUARD",
};

static void evx_state_decay_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(evx_state_decay_work,
			    evx_state_decay_workfn);

unsigned int evx_rodin_get_thermal_pressure_pct(void)
{
	unsigned long max_pressure = 0;
	unsigned long pressure;
	int cpu;

	for_each_online_cpu(cpu) {
		pressure = arch_scale_thermal_pressure(cpu);
		if (pressure > max_pressure)
			max_pressure = pressure;
	}

	if (max_pressure >= SCHED_CAPACITY_SCALE)
		return 100;

	return (unsigned int)((max_pressure * 100UL) /
			      SCHED_CAPACITY_SCALE);
}
EXPORT_SYMBOL_GPL(evx_rodin_get_thermal_pressure_pct);

enum evx_rodin_state evx_rodin_get_state(void)
{
	int state = atomic_read(&evx_current_state);

	if (state < EVX_RODIN_IDLE || state >= EVX_RODIN_STATE_MAX)
		return EVX_RODIN_IDLE;

	return state;
}
EXPORT_SYMBOL_GPL(evx_rodin_get_state);

void evx_rodin_request_state(enum evx_rodin_state state,
			     unsigned int hold_ms)
{
	unsigned long flags;
	unsigned long expires;
	enum evx_rodin_state active_state;
	bool accepted = false;

	if (state < EVX_RODIN_IDLE || state >= EVX_RODIN_STATE_MAX)
		return;

	hold_ms = clamp_t(unsigned int, hold_ms,
			  EVX_MIN_HOLD_MS, EVX_MAX_HOLD_MS);
	expires = jiffies + msecs_to_jiffies(hold_ms);

	spin_lock_irqsave(&evx_state_lock, flags);

	active_state = evx_rodin_get_state();

	/*
	 * Higher-priority states may replace lower-priority states.
	 * Lower-priority requests cannot extend an active stronger state.
	 */
	if (time_after_eq(jiffies, evx_state_expires) ||
	    state >= active_state) {
		atomic_set(&evx_current_state, state);
		evx_state_expires = expires;
		accepted = true;
	}

	spin_unlock_irqrestore(&evx_state_lock, flags);

	if (accepted)
		mod_delayed_work(system_wq, &evx_state_decay_work,
				 msecs_to_jiffies(hold_ms));
}
EXPORT_SYMBOL_GPL(evx_rodin_request_state);

static void evx_state_decay_workfn(struct work_struct *work)
{
	unsigned long flags;
	unsigned long delay = 0;
	bool decay = false;

	spin_lock_irqsave(&evx_state_lock, flags);

	if (time_after_eq(jiffies, evx_state_expires)) {
		atomic_set(&evx_current_state, EVX_RODIN_IDLE);
		evx_state_expires = jiffies;
		decay = true;
	} else {
		delay = evx_state_expires - jiffies;
	}

	spin_unlock_irqrestore(&evx_state_lock, flags);

	if (!decay && delay)
		mod_delayed_work(system_wq, &evx_state_decay_work, delay);
}

static int evx_status_show(struct seq_file *m, void *v)
{
	enum evx_rodin_state state;
	unsigned long flags;
	unsigned long remaining = 0;

	state = evx_rodin_get_state();

	spin_lock_irqsave(&evx_state_lock, flags);
	if (time_after(evx_state_expires, jiffies))
		remaining = jiffies_to_msecs(evx_state_expires - jiffies);
	spin_unlock_irqrestore(&evx_state_lock, flags);

	seq_printf(m, "state=%s\n", evx_state_names[state]);
	seq_printf(m, "state_id=%d\n", state);
	seq_printf(m, "hold_remaining_ms=%lu\n", remaining);
	seq_printf(m, "thermal_pressure_pct=%u\n",
		   evx_rodin_get_thermal_pressure_pct());

	return 0;
}

static int evx_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, evx_status_show, NULL);
}

static const struct proc_ops evx_status_fops = {
	.proc_open	= evx_status_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int __init evx_rodin_core_init(void)
{
	evx_proc_dir = proc_mkdir("evonix_rodin", NULL);
	if (!evx_proc_dir)
		return -ENOMEM;

	evx_proc_status = proc_create("status", 0444, evx_proc_dir,
				      &evx_status_fops);
	if (!evx_proc_status) {
		proc_remove(evx_proc_dir);
		evx_proc_dir = NULL;
		return -ENOMEM;
	}

	pr_info(EVX_RODIN_NAME ": optimizer core initialized\n");
	return 0;
}

static void __exit evx_rodin_core_exit(void)
{
	cancel_delayed_work_sync(&evx_state_decay_work);

	if (evx_proc_status)
		proc_remove(evx_proc_status);

	if (evx_proc_dir)
		proc_remove(evx_proc_dir);

	pr_info(EVX_RODIN_NAME ": optimizer core removed\n");
}

module_init(evx_rodin_core_init);
module_exit(evx_rodin_core_exit);

MODULE_DESCRIPTION("Evonix Rodin kernel optimizer core state machine");
MODULE_AUTHOR("NEESCHAL");
MODULE_LICENSE("GPL");
