#ifndef __RFKILL_H
#define __RFKILL_H

/*
 * Copyright (C) 2006 Ivo van Doorn
 * Copyright (C) 2007 Dmitry Torokhov
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/device.h>

/**
 * enum rfkill_type - type of rfkill switch.
 * RFKILL_TYPE_WLAN: switch is no a Wireless network devices.
 * RFKILL_TYPE_BlUETOOTH: switch is on a bluetooth device.
 * RFKILL_TYPE_IRDA: switch is on an infrared devices.
 */
enum rfkill_type {
	RFKILL_TYPE_WLAN = 0,
	RFKILL_TYPE_BLUETOOTH = 1,
	RFKILL_TYPE_IRDA = 2,
	RFKILL_TYPE_MAX = 3,
};

enum rfkill_state {
	RFKILL_STATE_OFF	= 0,
	RFKILL_STATE_ON		= 1,
};

/**
 * struct rfkill - rfkill control structure.
 * @name: Name of the switch.
 * @type: Radio type which the button controls, the value stored
 *	here should be a value from enum rfkill_type.
 * @state: State of the switch (on/off).
 * @user_claim: Set when the switch is controlled exlusively by userspace.
 * @mutex: Guards switch state transitions
 * @data: Pointer to the RF button drivers private data which will be
 *	passed along when toggling radio state.
 * @toggle_radio(): Mandatory handler to control state of the radio.
 * @dev: Device structure integrating the switch into device tree.
 * @node: Used to place switch into list of all switches known to the
 *	the system.
 *
 * This structure represents a RF switch located on a network device.
 */
struct rfkill {
	const char *name;
	enum rfkill_type type;

	enum rfkill_state state;
	bool user_claim;

	struct mutex mutex;

	void *data;
	int (*toggle_radio)(void *data, enum rfkill_state state);

	struct device dev;
	struct list_head node;
};
#define to_rfkill(d)	container_of(d, struct rfkill, dev)

struct rfkill *rfkill_allocate(struct device *parent, enum rfkill_type type);
void rfkill_free(struct rfkill *rfkill);
int rfkill_register(struct rfkill *rfkill);
void rfkill_unregister(struct rfkill *rfkill);

void rfkill_switch_all(enum rfkill_type type, enum rfkill_state state);

#endif /* RFKILL_H */
