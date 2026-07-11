// SPDX-License-Identifier: GPL-2.0
/*
 * Evonix Rodin Touch Activity Source
 *
 * Detects real touchscreen activity through the Linux input subsystem.
 * It does not poll, alter the touch driver, or force CPU frequencies.
 */

#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "evonix_rodin_internal.h"

#define EVX_INPUT_NAME			"evonix_rodin_input"
#define EVX_TOUCH_DOWN_HOLD_MS		180U
#define EVX_TOUCH_MOVE_HOLD_MS		120U
#define EVX_TOUCH_REFRESH_MS		24U

struct evx_input_handle {
	struct input_handle handle;
};

static struct proc_dir_entry *evx_input_proc;

static atomic_t evx_connected_devices = ATOMIC_INIT(0);
static atomic64_t evx_relevant_events = ATOMIC64_INIT(0);
static atomic64_t evx_touch_downs = ATOMIC64_INIT(0);
static atomic64_t evx_motion_boosts = ATOMIC64_INIT(0);

static unsigned long evx_last_motion_boost;

static bool evx_is_direct_touchscreen(struct input_dev *dev)
{
	bool has_mt_coordinates;
	bool has_single_coordinates;

	if (!test_bit(EV_ABS, dev->evbit))
		return false;

	has_mt_coordinates =
		test_bit(ABS_MT_POSITION_X, dev->absbit) &&
		test_bit(ABS_MT_POSITION_Y, dev->absbit);

	has_single_coordinates =
		test_bit(ABS_X, dev->absbit) &&
		test_bit(ABS_Y, dev->absbit);

	if (test_bit(INPUT_PROP_DIRECT, dev->propbit))
		return has_mt_coordinates || has_single_coordinates;

	return has_mt_coordinates;
}

static void evx_input_request_motion_boost(void)
{
	unsigned long now;
	unsigned long last;

	now = jiffies;
	last = READ_ONCE(evx_last_motion_boost);

	if (time_before(now,
			last + msecs_to_jiffies(
				EVX_TOUCH_REFRESH_MS)))
		return;

	WRITE_ONCE(evx_last_motion_boost, now);
	atomic64_inc(&evx_motion_boosts);

	evx_rodin_request_state(EVX_RODIN_INTERACTIVE,
				EVX_TOUCH_MOVE_HOLD_MS);
}

static void evx_input_event(struct input_handle *handle,
			    unsigned int type,
			    unsigned int code,
			    int value)
{
	if (type == EV_KEY && code == BTN_TOUCH) {
		atomic64_inc(&evx_relevant_events);

		if (value > 0) {
			atomic64_inc(&evx_touch_downs);
			evx_rodin_request_state(
				EVX_RODIN_INTERACTIVE,
				EVX_TOUCH_DOWN_HOLD_MS);
		}

		return;
	}

	if (type != EV_ABS)
		return;

	switch (code) {
	case ABS_MT_TRACKING_ID:
		atomic64_inc(&evx_relevant_events);

		if (value >= 0) {
			atomic64_inc(&evx_touch_downs);
			evx_rodin_request_state(
				EVX_RODIN_INTERACTIVE,
				EVX_TOUCH_DOWN_HOLD_MS);
		}
		break;

	case ABS_MT_POSITION_X:
	case ABS_MT_POSITION_Y:
	case ABS_X:
	case ABS_Y:
		atomic64_inc(&evx_relevant_events);
		evx_input_request_motion_boost();
		break;

	default:
		break;
	}
}

static int evx_input_connect(struct input_handler *handler,
			     struct input_dev *dev,
			     const struct input_device_id *id)
{
	struct evx_input_handle *evx_handle;
	int ret;

	if (!evx_is_direct_touchscreen(dev))
		return -ENODEV;

	evx_handle = kzalloc(sizeof(*evx_handle), GFP_KERNEL);
	if (!evx_handle)
		return -ENOMEM;

	evx_handle->handle.dev = dev;
	evx_handle->handle.handler = handler;
	evx_handle->handle.name = EVX_INPUT_NAME;

	ret = input_register_handle(&evx_handle->handle);
	if (ret)
		goto free_handle;

	ret = input_open_device(&evx_handle->handle);
	if (ret)
		goto unregister_handle;

	atomic_inc(&evx_connected_devices);

	pr_info(EVX_INPUT_NAME ": connected to %s\n",
		dev_name(&dev->dev));

	return 0;

unregister_handle:
	input_unregister_handle(&evx_handle->handle);

free_handle:
	kfree(evx_handle);
	return ret;
}

static void evx_input_disconnect(struct input_handle *handle)
{
	struct evx_input_handle *evx_handle;

	evx_handle =
		container_of(handle,
			     struct evx_input_handle,
			     handle);

	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(evx_handle);

	atomic_dec_if_positive(&evx_connected_devices);
}

static const struct input_device_id evx_input_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_ABS) },
	},
	{ },
};

MODULE_DEVICE_TABLE(input, evx_input_ids);

static struct input_handler evx_input_handler = {
	.event		= evx_input_event,
	.connect	= evx_input_connect,
	.disconnect	= evx_input_disconnect,
	.name		= EVX_INPUT_NAME,
	.id_table	= evx_input_ids,
};

static int evx_input_stats_show(struct seq_file *m, void *v)
{
	unsigned long last;
	unsigned long age_ms = 0;

	last = READ_ONCE(evx_last_motion_boost);

	if (last && time_after_eq(jiffies, last))
		age_ms = jiffies_to_msecs(jiffies - last);

	seq_printf(m, "connected_devices=%d\n",
		   atomic_read(&evx_connected_devices));
	seq_printf(m, "relevant_events=%lld\n",
		   (long long)atomic64_read(
			   &evx_relevant_events));
	seq_printf(m, "touch_downs=%lld\n",
		   (long long)atomic64_read(
			   &evx_touch_downs));
	seq_printf(m, "motion_boosts=%lld\n",
		   (long long)atomic64_read(
			   &evx_motion_boosts));
	seq_printf(m, "last_motion_age_ms=%lu\n",
		   age_ms);

	return 0;
}

static int evx_input_stats_open(struct inode *inode,
				struct file *file)
{
	return single_open(file,
			   evx_input_stats_show,
			   NULL);
}

static const struct proc_ops evx_input_stats_fops = {
	.proc_open	= evx_input_stats_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int __init evx_input_init(void)
{
	struct proc_dir_entry *parent;
	int ret;

	ret = input_register_handler(&evx_input_handler);
	if (ret)
		return ret;

	parent = evx_rodin_get_proc_dir();

	if (parent)
		evx_input_proc =
			proc_create("input_stats", 0444,
				    parent,
				    &evx_input_stats_fops);

	pr_info(EVX_INPUT_NAME
		": event-driven touch source initialized\n");

	return 0;
}

static void __exit evx_input_exit(void)
{
	if (evx_input_proc)
		proc_remove(evx_input_proc);

	input_unregister_handler(&evx_input_handler);

	evx_input_proc = NULL;

	pr_info(EVX_INPUT_NAME ": removed\n");
}

module_init(evx_input_init);
module_exit(evx_input_exit);

MODULE_DESCRIPTION("Evonix Rodin event-driven touchscreen source");
MODULE_AUTHOR("NEESCHAL");
MODULE_LICENSE("GPL");
