// SPDX-License-Identifier: GPL-2.0
/*
 * Evonix Rodin Optimizer Core
 *
 * Shared event-driven state machine for scheduler, touch, CPU QoS,
 * thermal, block I/O, memory and network optimization.
 */

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/topology.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "evonix_rodin_internal.h"

#define EVX_RODIN_NAME		"evonix_rodin_core"
#define EVX_MIN_HOLD_MS		16U
#define EVX_MAX_HOLD_MS		10000U

static DEFINE_SPINLOCK(evx_state_lock);
static ATOMIC_NOTIFIER_HEAD(evx_state_notifier);

static atomic_t evx_current_state = ATOMIC_INIT(EVX_RODIN_IDLE);
static atomic64_t evx_request_count = ATOMIC64_INIT(0);
static atomic64_t evx_transition_count = ATOMIC64_INIT(0);

static unsigned long evx_state_deadline[EVX_RODIN_STATE_MAX];

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

static enum evx_rodin_state
evx_pick_highest_state_locked(unsigned long now)
{
	int state;

	for (state = EVX_RODIN_STATE_MAX - 1;
	     state > EVX_RODIN_IDLE;
	     state--) {
		if (time_before(now, evx_state_deadline[state]))
			return state;
	}

	return EVX_RODIN_IDLE;
}

static unsigned long
evx_state_remaining_locked(enum evx_rodin_state state,
			   unsigned long now)
{
	if (state <= EVX_RODIN_IDLE ||
	    state >= EVX_RODIN_STATE_MAX)
		return 0;

	if (!time_before(now, evx_state_deadline[state]))
		return 0;

	return evx_state_deadline[state] - now;
}

static void
evx_publish_state_change(enum evx_rodin_state new_state)
{
	atomic64_inc(&evx_transition_count);

	/*
	 * Callbacks registered here must remain atomic-safe and queue their
	 * heavier policy work onto a workqueue.
	 */
	atomic_notifier_call_chain(&evx_state_notifier,
				   new_state, NULL);
}

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

	if (state < EVX_RODIN_IDLE ||
	    state >= EVX_RODIN_STATE_MAX)
		return EVX_RODIN_IDLE;

	return state;
}
EXPORT_SYMBOL_GPL(evx_rodin_get_state);

int evx_rodin_register_state_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_register(&evx_state_notifier, nb);
}
EXPORT_SYMBOL_GPL(evx_rodin_register_state_notifier);

int evx_rodin_unregister_state_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_unregister(&evx_state_notifier, nb);
}
EXPORT_SYMBOL_GPL(evx_rodin_unregister_state_notifier);

struct proc_dir_entry *evx_rodin_get_proc_dir(void)
{
	return READ_ONCE(evx_proc_dir);
}
EXPORT_SYMBOL_GPL(evx_rodin_get_proc_dir);

void evx_rodin_request_state(enum evx_rodin_state state,
			     unsigned int hold_ms)
{
	enum evx_rodin_state old_state;
	enum evx_rodin_state new_state;
	unsigned long flags;
	unsigned long now;
	unsigned long expires;
	unsigned long delay = 0;
	bool accepted = false;
	bool changed = false;

	if (state <= EVX_RODIN_IDLE ||
	    state >= EVX_RODIN_STATE_MAX)
		return;

	hold_ms = clamp_t(unsigned int, hold_ms,
			  EVX_MIN_HOLD_MS,
			  EVX_MAX_HOLD_MS);

	now = jiffies;
	expires = now + msecs_to_jiffies(hold_ms);

	spin_lock_irqsave(&evx_state_lock, flags);

	if (time_after(expires, evx_state_deadline[state])) {
		evx_state_deadline[state] = expires;
		accepted = true;
	}

	old_state = evx_rodin_get_state();
	new_state = evx_pick_highest_state_locked(now);

	if (new_state != old_state) {
		atomic_set(&evx_current_state, new_state);
		changed = true;
	}

	delay = evx_state_remaining_locked(new_state, now);

	spin_unlock_irqrestore(&evx_state_lock, flags);

	if (!accepted)
		return;

	atomic64_inc(&evx_request_count);

	if (changed)
		evx_publish_state_change(new_state);

	if (delay)
		mod_delayed_work(system_wq,
				 &evx_state_decay_work,
				 max_t(unsigned long, 1, delay));
}

static void evx_state_decay_workfn(struct work_struct *work)
{
	enum evx_rodin_state old_state;
	enum evx_rodin_state new_state;
	unsigned long flags;
	unsigned long now;
	unsigned long delay = 0;
	bool changed = false;

	now = jiffies;

	spin_lock_irqsave(&evx_state_lock, flags);

	old_state = evx_rodin_get_state();
	new_state = evx_pick_highest_state_locked(now);

	if (new_state != old_state) {
		atomic_set(&evx_current_state, new_state);
		changed = true;
	}

	delay = evx_state_remaining_locked(new_state, now);

	spin_unlock_irqrestore(&evx_state_lock, flags);

	if (changed)
		evx_publish_state_change(new_state);

	if (delay)
		mod_delayed_work(system_wq,
				 &evx_state_decay_work,
				 max_t(unsigned long, 1, delay));
}

static int evx_status_show(struct seq_file *m, void *v)
{
	enum evx_rodin_state active_state;
	unsigned long flags;
	unsigned long now;
	unsigned long remaining;
	int state;

	now = jiffies;
	active_state = evx_rodin_get_state();

	spin_lock_irqsave(&evx_state_lock, flags);

	remaining =
		evx_state_remaining_locked(active_state, now);

	seq_printf(m, "state=%s\n",
		   evx_state_names[active_state]);
	seq_printf(m, "state_id=%d\n", active_state);
	seq_printf(m, "hold_remaining_ms=%u\n",
		   jiffies_to_msecs(remaining));

	for (state = EVX_RODIN_INTERACTIVE;
	     state < EVX_RODIN_STATE_MAX;
	     state++) {
		unsigned long state_remaining;

		state_remaining =
			evx_state_remaining_locked(state, now);

		seq_printf(m, "%s_remaining_ms=%u\n",
			   evx_state_names[state],
			   jiffies_to_msecs(state_remaining));
	}

	spin_unlock_irqrestore(&evx_state_lock, flags);

	seq_printf(m, "thermal_pressure_pct=%u\n",
		   evx_rodin_get_thermal_pressure_pct());
	seq_printf(m, "requests=%lld\n",
		   (long long)atomic64_read(&evx_request_count));
	seq_printf(m, "transitions=%lld\n",
		   (long long)atomic64_read(&evx_transition_count));

	return 0;
}

static int evx_status_open(struct inode *inode,
			   struct file *file)
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

	evx_proc_status =
		proc_create("status", 0444,
			    evx_proc_dir,
			    &evx_status_fops);

	if (!evx_proc_status) {
		proc_remove(evx_proc_dir);
		evx_proc_dir = NULL;
		return -ENOMEM;
	}

	pr_info(EVX_RODIN_NAME
		": multi-deadline optimizer core initialized\n");

	return 0;
}

static void __exit evx_rodin_core_exit(void)
{
	cancel_delayed_work_sync(&evx_state_decay_work);

	if (evx_proc_status)
		proc_remove(evx_proc_status);

	if (evx_proc_dir)
		proc_remove(evx_proc_dir);

	evx_proc_status = NULL;
	evx_proc_dir = NULL;

	pr_info(EVX_RODIN_NAME ": optimizer core removed\n");
}

module_init(evx_rodin_core_init);
module_exit(evx_rodin_core_exit);

MODULE_DESCRIPTION("Evonix Rodin kernel optimizer state machine");
MODULE_AUTHOR("NEESCHAL");
MODULE_LICENSE("GPL");
