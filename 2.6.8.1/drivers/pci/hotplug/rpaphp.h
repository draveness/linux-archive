/*
 * PCI Hot Plug Controller Driver for RPA-compliant PPC64 platform.
 *
 * Copyright (C) 2003 Linda Xie <lxie@us.ibm.com>
 *
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Send feedback to <lxie@us.ibm.com>,
 *
 */

#ifndef _PPC64PHP_H
#define _PPC64PHP_H

#include <linux/pci.h>
#include "pci_hotplug.h"

#define	HOTPLUG	1
#define	EMBEDDED 0

#define DR_INDICATOR 9002
#define DR_ENTITY_SENSE 9003

#define POWER_ON	100
#define POWER_OFF	0

#define LED_OFF		0
#define LED_ON		1	/* continuous on */
#define LED_ID		2	/* slow blinking */
#define LED_ACTION	3	/* fast blinking */

/* Error status from rtas_get-sensor */
#define NEED_POWER    -9000	/* slot must be power up and unisolated to get state */
#define PWR_ONLY      -9001	/* slot must be powerd up to get state, leave isolated */
#define ERR_SENSE_USE -9002	/* No DR operation will succeed, slot is unusable  */

/* Sensor values from rtas_get-sensor */
#define EMPTY           0	/* No card in slot */
#define PRESENT         1	/* Card in slot */

#define MY_NAME "rpaphp"
extern int debug;
#define dbg(format, arg...)					\
	do {							\
		if (debug)					\
			printk(KERN_DEBUG "%s: " format,	\
				MY_NAME , ## arg); 		\
	} while (0)
#define err(format, arg...) printk(KERN_ERR "%s: " format, MY_NAME , ## arg)
#define info(format, arg...) printk(KERN_INFO "%s: " format, MY_NAME , ## arg)
#define warn(format, arg...) printk(KERN_WARNING "%s: " format, MY_NAME , ## arg)

/* slot types */
#define VIO_DEV	1
#define PCI_DEV	2

/* slot states */

#define	NOT_VALID	3
#define	NOT_CONFIGURED	2
#define	CONFIGURED	1
#define	EMPTY		0

struct rpaphp_pci_func {
	struct pci_dev *pci_dev;
	struct list_head sibling;
};

/*
 * struct slot - slot information for each *physical* slot
 */
struct slot {
	struct list_head rpaphp_slot_list;
	int state;
	u32 index;
	u32 type;
	u32 power_domain;
	char *name;
	char *location;
	u8 removable;
	u8 dev_type;		/* VIO or PCI */
	struct device_node *dn;	/* slot's device_node in OFDT */
				/* dn has phb info */
	struct pci_dev *bridge;	/* slot's pci_dev in pci_devices */
	union {
		struct list_head pci_funcs; /* pci_devs in PCI slot */ 
		struct vio_dev *vio_dev; /* vio_dev in VIO slot */
	} dev;
	struct hotplug_slot *hotplug_slot;
};

extern struct hotplug_slot_ops rpaphp_hotplug_slot_ops;
extern struct list_head rpaphp_slot_head;
extern int num_slots;

static inline int is_hotplug_capable(struct device_node *dn)
{
	unsigned char *ptr = get_property(dn, "ibm,fw-pci-hot-plug-ctrl", NULL);

	return (int) (ptr != NULL);
}

/* function prototypes */

/* rpaphp_pci.c */
extern struct pci_dev *rpaphp_find_pci_dev(struct device_node *dn);
extern int rpaphp_claim_resource(struct pci_dev *dev, int resource);
extern int rpaphp_enable_pci_slot(struct slot *slot);
extern int register_pci_slot(struct slot *slot);
extern int rpaphp_unconfig_pci_adapter(struct slot *slot);
extern int rpaphp_get_pci_adapter_status(struct slot *slot, int is_init, u8 * value);
extern struct hotplug_slot *rpaphp_find_hotplug_slot(struct pci_dev *dev);

/* rpaphp_core.c */
extern int rpaphp_add_slot(struct device_node *dn);
extern int rpaphp_remove_slot(struct slot *slot);
extern char *rpaphp_get_drc_name(struct device_node *dn);

/* rpaphp_vio.c */
extern int rpaphp_get_vio_adapter_status(struct slot *slot, int is_init, u8 * value);
extern int rpaphp_unconfig_vio_adapter(struct slot *slot);
extern int register_vio_slot(struct device_node *dn);
extern int rpaphp_enable_vio_slot(struct slot *slot);

/* rpaphp_slot.c */
extern void dealloc_slot_struct(struct slot *slot);
extern struct slot *alloc_slot_struct(struct device_node *dn, int drc_index, char *drc_name, int power_domain);
extern int register_slot(struct slot *slot);
extern int deregister_slot(struct slot *slot);
extern int rpaphp_get_power_status(struct slot *slot, u8 * value);
extern int rpaphp_set_attention_status(struct slot *slot, u8 status);
	
#endif				/* _PPC64PHP_H */
