/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _EVONIX_RODIN_INTERNAL_H
#define _EVONIX_RODIN_INTERNAL_H

#include <linux/notifier.h>
#include <linux/types.h>

struct proc_dir_entry;

enum evx_rodin_state {
	EVX_RODIN_IDLE = 0,
	EVX_RODIN_INTERACTIVE,
	EVX_RODIN_SUSTAINED,
	EVX_RODIN_FRAME_PRESSURE,
	EVX_RODIN_STATE_MAX,
};

enum evx_rodin_load_tier {
	EVX_RODIN_LOAD_LIGHT = 0,
	EVX_RODIN_LOAD_MODERATE,
	EVX_RODIN_LOAD_HEAVY,
	EVX_RODIN_LOAD_TIER_MAX,
};

void evx_rodin_request_state(enum evx_rodin_state state,
			     unsigned int hold_ms);

enum evx_rodin_state evx_rodin_get_state(void);
unsigned int evx_rodin_get_thermal_pressure_pct(void);

int evx_rodin_register_state_notifier(struct notifier_block *nb);
int evx_rodin_unregister_state_notifier(struct notifier_block *nb);

struct proc_dir_entry *evx_rodin_get_proc_dir(void);
void evx_rodin_qos_refresh(void);

enum evx_rodin_load_tier evx_rodin_get_load_tier(void);

int evx_rodin_register_load_notifier(struct notifier_block *nb);
int evx_rodin_unregister_load_notifier(struct notifier_block *nb);

#endif
