/*
 * PCI Express Hot Plug Controller Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM Corp.
 * Copyright (C) 2003-2004 Intel Corporation
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
 * Send feedback to <greg@kroah.com>, <dely.l.sy@intel.com>
 *
 */
#ifndef _PCIEHP_H
#define _PCIEHP_H

#include <linux/types.h>
#include <linux/pci.h>
#include <asm/semaphore.h>
#include <asm/io.h>		
#include "pci_hotplug.h"

#define MY_NAME	"pciehp"

extern int pciehp_poll_mode;
extern int pciehp_poll_time;
extern int pciehp_debug;

/*#define dbg(format, arg...) do { if (pciehp_debug) printk(KERN_DEBUG "%s: " format, MY_NAME , ## arg); } while (0)*/
#define dbg(format, arg...) do { if (pciehp_debug) printk("%s: " format, MY_NAME , ## arg); } while (0)
#define err(format, arg...) printk(KERN_ERR "%s: " format, MY_NAME , ## arg)
#define info(format, arg...) printk(KERN_INFO "%s: " format, MY_NAME , ## arg)
#define warn(format, arg...) printk(KERN_WARNING "%s: " format, MY_NAME , ## arg)

struct pci_func {
	struct pci_func *next;
	u8 bus;
	u8 device;
	u8 function;
	u8 is_a_board;
	u16 status;
	u8 configured;
	u8 switch_save;
	u8 presence_save;
	u32 base_length[0x06];
	u8 base_type[0x06];
	u16 reserved2;
	u32 config_space[0x20];
	struct pci_resource *mem_head;
	struct pci_resource *p_mem_head;
	struct pci_resource *io_head;
	struct pci_resource *bus_head;
	struct pci_dev* pci_dev;
};

struct slot {
	struct slot *next;
	u8 bus;
	u8 device;
	u32 number;
	u8 is_a_board;
	u8 configured;
	u8 state;
	u8 switch_save;
	u8 presence_save;
	u32 capabilities;
	u16 reserved2;
	struct timer_list task_event;
	u8 hp_slot;
	struct controller *ctrl;
	struct hpc_ops *hpc_ops;
	struct hotplug_slot *hotplug_slot;
	struct list_head	slot_list;
};

struct pci_resource {
	struct pci_resource * next;
	u32 base;
	u32 length;
};

struct event_info {
	u32 event_type;
	u8 hp_slot;
};

struct controller {
	struct controller *next;
	struct semaphore crit_sect;	/* critical section semaphore */
	void *hpc_ctlr_handle;		/* HPC controller handle */
	int num_slots;			/* Number of slots on ctlr */
	int slot_num_inc;		/* 1 or -1 */
	struct pci_resource *mem_head;
	struct pci_resource *p_mem_head;
	struct pci_resource *io_head;
	struct pci_resource *bus_head;
	struct pci_dev *pci_dev;
	struct pci_bus *pci_bus;
	struct event_info event_queue[10];
	struct slot *slot;
	struct hpc_ops *hpc_ops;
	wait_queue_head_t queue;	/* sleep & wake process */
	u8 next_event;
	u8 seg;
	u8 bus;
	u8 device;
	u8 function;
	u8 rev;
	u8 slot_device_offset;
	u8 add_support;
	enum pci_bus_speed speed;
	u32 first_slot;		/* First physical slot number */  /* PCIE only has 1 slot */
	u8 slot_bus;		/* Bus where the slots handled by this controller sit */
	u8 push_flag;
	u16 ctlrcap;
	u16 vendor_id;
};

struct irq_mapping {
	u8 barber_pole;
	u8 valid_INT;
	u8 interrupt[4];
};

struct resource_lists {
	struct pci_resource *mem_head;
	struct pci_resource *p_mem_head;
	struct pci_resource *io_head;
	struct pci_resource *bus_head;
	struct irq_mapping *irqs;
};

#define INT_BUTTON_IGNORE		0
#define INT_PRESENCE_ON			1
#define INT_PRESENCE_OFF		2
#define INT_SWITCH_CLOSE		3
#define INT_SWITCH_OPEN			4
#define INT_POWER_FAULT			5
#define INT_POWER_FAULT_CLEAR		6
#define INT_BUTTON_PRESS		7
#define INT_BUTTON_RELEASE		8
#define INT_BUTTON_CANCEL		9

#define STATIC_STATE			0
#define BLINKINGON_STATE		1
#define BLINKINGOFF_STATE		2
#define POWERON_STATE			3
#define POWEROFF_STATE			4

#define PCI_TO_PCI_BRIDGE_CLASS		0x00060400

/* Error messages */
#define INTERLOCK_OPEN			0x00000002
#define ADD_NOT_SUPPORTED		0x00000003
#define CARD_FUNCTIONING		0x00000005
#define ADAPTER_NOT_SAME		0x00000006
#define NO_ADAPTER_PRESENT		0x00000009
#define NOT_ENOUGH_RESOURCES		0x0000000B
#define DEVICE_TYPE_NOT_SUPPORTED	0x0000000C
#define WRONG_BUS_FREQUENCY		0x0000000D
#define POWER_FAILURE			0x0000000E

#define REMOVE_NOT_SUPPORTED		0x00000003

#define DISABLE_CARD			1

/*
 * error Messages
 */
#define msg_initialization_err	"Initialization failure, error=%d\n"
#define msg_HPC_rev_error	"Unsupported revision of the PCI hot plug controller found.\n"
#define msg_HPC_non_pcie	"The PCI hot plug controller is not supported by this driver.\n"
#define msg_HPC_not_supported	"This system is not supported by this version of pciephd module. Upgrade to a newer version of pciehpd\n"
#define msg_unable_to_save	"Unable to store PCI hot plug add resource information. This system must be rebooted before adding any PCI devices.\n"
#define msg_button_on		"PCI slot #%d - powering on due to button press.\n"
#define msg_button_off		"PCI slot #%d - powering off due to button press.\n"
#define msg_button_cancel	"PCI slot #%d - action canceled due to button press.\n"
#define msg_button_ignore	"PCI slot #%d - button press ignored.  (action in progress...)\n"

/* sysfs function for the hotplug controller info */
extern void pciehp_create_ctrl_files	(struct controller *ctrl);

/* controller functions */
extern int	pciehprm_find_available_resources	(struct controller *ctrl);
extern int	pciehp_event_start_thread	(void);
extern void	pciehp_event_stop_thread	(void);
extern struct 	pci_func *pciehp_slot_create	(unsigned char busnumber);
extern struct 	pci_func *pciehp_slot_find	(unsigned char bus, unsigned char device, unsigned char index);
extern int	pciehp_enable_slot		(struct slot *slot);
extern int	pciehp_disable_slot		(struct slot *slot);

extern u8	pciehp_handle_attention_button	(u8 hp_slot, void *inst_id);
extern u8	pciehp_handle_switch_change	(u8 hp_slot, void *inst_id);
extern u8	pciehp_handle_presence_change	(u8 hp_slot, void *inst_id);
extern u8	pciehp_handle_power_fault	(u8 hp_slot, void *inst_id);
/* extern void	long_delay (int delay); */

/* resource functions */
extern int	pciehp_resource_sort_and_combine	(struct pci_resource **head);

/* pci functions */
extern int	pciehp_set_irq			(u8 bus_num, u8 dev_num, u8 int_pin, u8 irq_num);
/*extern int	pciehp_get_bus_dev		(struct controller *ctrl, u8 *bus_num, u8 *dev_num, struct slot *slot);*/
extern int	pciehp_save_config	 	(struct controller *ctrl, int busnumber, int num_ctlr_slots, int first_device_num);
extern int	pciehp_save_used_resources	(struct controller *ctrl, struct pci_func * func, int flag);
extern int	pciehp_save_slot_config		(struct controller *ctrl, struct pci_func * new_slot);
extern void	pciehp_destroy_board_resources	(struct pci_func * func);
extern int	pciehp_return_board_resources	(struct pci_func * func, struct resource_lists * resources);
extern void	pciehp_destroy_resource_list	(struct resource_lists * resources);
extern int	pciehp_configure_device		(struct controller* ctrl, struct pci_func* func);
extern int	pciehp_unconfigure_device	(struct pci_func* func);


/* Global variables */
extern struct controller *pciehp_ctrl_list;
extern struct pci_func *pciehp_slot_list[256];

/* Inline functions */

static inline struct slot *pciehp_find_slot(struct controller *ctrl, u8 device)
{
	struct slot *p_slot, *tmp_slot = NULL;

	p_slot = ctrl->slot;

	dbg("p_slot = %p\n", p_slot);

	while (p_slot && (p_slot->device != device)) {
		tmp_slot = p_slot;
		p_slot = p_slot->next;
		dbg("In while loop, p_slot = %p\n", p_slot);
	}
	if (p_slot == NULL) {
		err("ERROR: pciehp_find_slot device=0x%x\n", device);
		p_slot = tmp_slot;
	}

	return p_slot;
}

static inline int wait_for_ctrl_irq(struct controller *ctrl)
{
	int retval = 0;

	DECLARE_WAITQUEUE(wait, current);

	dbg("%s : start\n", __FUNCTION__);
	add_wait_queue(&ctrl->queue, &wait);
	set_current_state(TASK_INTERRUPTIBLE);
	if (!pciehp_poll_mode) {
		/* Sleep for up to 1 second */
		schedule_timeout(1*HZ);
	} else
		schedule_timeout(2.5*HZ);
	
	set_current_state(TASK_RUNNING);
	remove_wait_queue(&ctrl->queue, &wait);
	if (signal_pending(current))
		retval =  -EINTR;

	dbg("%s : end\n", __FUNCTION__);
	return retval;
}

/* Puts node back in the resource list pointed to by head */
static inline void return_resource(struct pci_resource **head, struct pci_resource *node)
{
	if (!node || !head)
		return;
	node->next = *head;
	*head = node;
}

#define SLOT_NAME_SIZE 10

static inline void make_slot_name(char *buffer, int buffer_size, struct slot *slot)
{
	snprintf(buffer, buffer_size, "%d", slot->number);
}

enum php_ctlr_type {
	PCI,
	ISA,
	ACPI
};

typedef u8(*php_intr_callback_t) (unsigned int change_id, void *instance_id);

int pcie_init(struct controller *ctrl, struct pci_dev *pdev,
		php_intr_callback_t attention_button_callback,
		php_intr_callback_t switch_change_callback,
		php_intr_callback_t presence_change_callback,
		php_intr_callback_t power_fault_callback);


/* This has no meaning for PCI Express, as there is only 1 slot per port */
int pcie_get_ctlr_slot_config(struct controller *ctrl,
		int *num_ctlr_slots,
		int *first_device_num,
		int *physical_slot_num,
		int *updown,
		int *flags);

struct hpc_ops {
	int	(*power_on_slot)	(struct slot *slot);
	int	(*power_off_slot)	(struct slot *slot);
	int	(*get_power_status)	(struct slot *slot, u8 *status);
	int	(*get_attention_status)	(struct slot *slot, u8 *status);
	int	(*set_attention_status)	(struct slot *slot, u8 status);
	int	(*get_latch_status)	(struct slot *slot, u8 *status);
	int	(*get_adapter_status)	(struct slot *slot, u8 *status);

	int	(*get_max_bus_speed)	(struct slot *slot, enum pci_bus_speed *speed);
	int	(*get_cur_bus_speed)	(struct slot *slot, enum pci_bus_speed *speed);

	int	(*get_max_lnk_width)	(struct slot *slot, enum pcie_link_width *value);
	int	(*get_cur_lnk_width)	(struct slot *slot, enum pcie_link_width *value);
	
	int	(*query_power_fault)	(struct slot *slot);
	void	(*green_led_on)		(struct slot *slot);
	void	(*green_led_off)	(struct slot *slot);
	void	(*green_led_blink)	(struct slot *slot);
	void	(*release_ctlr)		(struct controller *ctrl);
	int	(*check_lnk_status)	(struct controller *ctrl);
};

#endif				/* _PCIEHP_H */
