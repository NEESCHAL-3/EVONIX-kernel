/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _EVONIX_RODIN_INTERNAL_H
#define _EVONIX_RODIN_INTERNAL_H

#include <linux/types.h>

enum evx_rodin_state {
	EVX_RODIN_IDLE = 0,
	EVX_RODIN_INTERACTIVE,
	EVX_RODIN_SUSTAINED,
	EVX_RODIN_FRAME_PRESSURE,
	EVX_RODIN_THERMAL_GUARD,
	EVX_RODIN_STATE_MAX,
};

void evx_rodin_request_state(enum evx_rodin_state state,
			     unsigned int hold_ms);
enum evx_rodin_state evx_rodin_get_state(void);
unsigned int evx_rodin_get_thermal_pressure_pct(void);

#endif
