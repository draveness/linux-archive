/*
 * PCI Express PCI Hot Plug Driver
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
 * Send feedback to <greg@kroah.com>,<dely.l.sy@intel.com>
 *
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/pci.h>
#include <asm/system.h>
#include "../pci.h"
#include "pciehp.h"

#ifdef DEBUG
#define DBG_K_TRACE_ENTRY      ((unsigned int)0x00000001)	/* On function entry */
#define DBG_K_TRACE_EXIT       ((unsigned int)0x00000002)	/* On function exit */
#define DBG_K_INFO             ((unsigned int)0x00000004)	/* Info messages */
#define DBG_K_ERROR            ((unsigned int)0x00000008)	/* Error messages */
#define DBG_K_TRACE            (DBG_K_TRACE_ENTRY|DBG_K_TRACE_EXIT)
#define DBG_K_STANDARD         (DBG_K_INFO|DBG_K_ERROR|DBG_K_TRACE)
/* Redefine this flagword to set debug level */
#define DEBUG_LEVEL            DBG_K_STANDARD

#define DEFINE_DBG_BUFFER     char __dbg_str_buf[256];

#define DBG_PRINT( dbg_flags, args... )              \
	do {                                             \
	  if ( DEBUG_LEVEL & ( dbg_flags ) )             \
	  {                                              \
	    int len;                                     \
	    len = sprintf( __dbg_str_buf, "%s:%d: %s: ", \
		  __FILE__, __LINE__, __FUNCTION__ );    \
	    sprintf( __dbg_str_buf + len, args );        \
	    printk( KERN_NOTICE "%s\n", __dbg_str_buf ); \
	  }                                              \
	} while (0)

#define DBG_ENTER_ROUTINE	DBG_PRINT (DBG_K_TRACE_ENTRY, "%s", "[Entry]");
#define DBG_LEAVE_ROUTINE	DBG_PRINT (DBG_K_TRACE_EXIT, "%s", "[Exit]");
#else
#define DEFINE_DBG_BUFFER
#define DBG_ENTER_ROUTINE
#define DBG_LEAVE_ROUTINE
#endif				/* DEBUG */

struct ctrl_reg {
	u8 cap_id;
	u8 nxt_ptr;
	u16 cap_reg;
	u32 dev_cap;
	u16 dev_ctrl;
	u16 dev_status;
	u32 lnk_cap;
	u16 lnk_ctrl;
	u16 lnk_status;
	u32 slot_cap;
	u16 slot_ctrl;
	u16 slot_status;
	u16 root_ctrl;
	u16 rsvp;
	u32 root_status;
} __attribute__ ((packed));

/* offsets to the controller registers based on the above structure layout */
enum ctrl_offsets {
	PCIECAPID	=	offsetof(struct ctrl_reg, cap_id),
	NXTCAPPTR	=	offsetof(struct ctrl_reg, nxt_ptr),
	CAPREG		=	offsetof(struct ctrl_reg, cap_reg),
	DEVCAP		=	offsetof(struct ctrl_reg, dev_cap),
	DEVCTRL		=	offsetof(struct ctrl_reg, dev_ctrl),
	DEVSTATUS	=	offsetof(struct ctrl_reg, dev_status),
	LNKCAP		=	offsetof(struct ctrl_reg, lnk_cap),
	LNKCTRL		=	offsetof(struct ctrl_reg, lnk_ctrl),
	LNKSTATUS	=	offsetof(struct ctrl_reg, lnk_status),
	SLOTCAP		=	offsetof(struct ctrl_reg, slot_cap),
	SLOTCTRL	=	offsetof(struct ctrl_reg, slot_ctrl),
	SLOTSTATUS	=	offsetof(struct ctrl_reg, slot_status),
	ROOTCTRL	=	offsetof(struct ctrl_reg, root_ctrl),
	ROOTSTATUS	=	offsetof(struct ctrl_reg, root_status),
};
static int pcie_cap_base = 0;		/* Base of the PCI Express capability item structure */ 

#define PCIE_CAP_ID	( pcie_cap_base + PCIECAPID )
#define NXT_CAP_PTR	( pcie_cap_base + NXTCAPPTR )
#define CAP_REG		( pcie_cap_base + CAPREG )
#define DEV_CAP		( pcie_cap_base + DEVCAP )
#define DEV_CTRL	( pcie_cap_base + DEVCTRL )
#define DEV_STATUS	( pcie_cap_base + DEVSTATUS )
#define LNK_CAP		( pcie_cap_base + LNKCAP )
#define LNK_CTRL	( pcie_cap_base + LNKCTRL )
#define LNK_STATUS	( pcie_cap_base + LNKSTATUS )
#define SLOT_CAP	( pcie_cap_base + SLOTCAP )
#define SLOT_CTRL	( pcie_cap_base + SLOTCTRL )
#define SLOT_STATUS	( pcie_cap_base + SLOTSTATUS )
#define ROOT_CTRL	( pcie_cap_base + ROOTCTRL )
#define ROOT_STATUS	( pcie_cap_base + ROOTSTATUS )

#define hp_register_read_word(pdev, reg , value)		\
	pci_read_config_word(pdev, reg, &value)

#define hp_register_read_dword(pdev, reg , value)		\
	pci_read_config_dword(pdev, reg, &value)
 
#define hp_register_write_word(pdev, reg , value)		\
	pci_write_config_word(pdev, reg, value)

#define hp_register_dwrite_word(pdev, reg , value)		\
	pci_write_config_dword(pdev, reg, value)

/* Field definitions in PCI Express Capabilities Register */
#define CAP_VER			0x000F
#define DEV_PORT_TYPE		0x00F0
#define SLOT_IMPL		0x0100
#define MSG_NUM			0x3E00

/* Device or Port Type */
#define NAT_ENDPT		0x00
#define LEG_ENDPT		0x01
#define ROOT_PORT		0x04
#define UP_STREAM		0x05
#define	DN_STREAM		0x06
#define PCIE_PCI_BRDG		0x07
#define PCI_PCIE_BRDG		0x10

/* Field definitions in Device Capabilities Register */
#define DATTN_BUTTN_PRSN	0x1000
#define DATTN_LED_PRSN		0x2000
#define DPWR_LED_PRSN		0x4000

/* Field definitions in Link Capabilities Register */
#define MAX_LNK_SPEED		0x000F
#define MAX_LNK_WIDTH		0x03F0

/* Link Width Encoding */
#define LNK_X1		0x01
#define LNK_X2		0x02
#define LNK_X4		0x04	
#define LNK_X8		0x08
#define LNK_X12		0x0C
#define LNK_X16		0x10	
#define LNK_X32		0x20

/*Field definitions of Link Status Register */
#define LNK_SPEED	0x000F
#define NEG_LINK_WD	0x03F0
#define LNK_TRN_ERR	0x0400
#define	LNK_TRN		0x0800
#define SLOT_CLK_CONF	0x1000

/* Field definitions in Slot Capabilities Register */
#define ATTN_BUTTN_PRSN	0x00000001
#define	PWR_CTRL_PRSN	0x00000002
#define MRL_SENS_PRSN	0x00000004
#define ATTN_LED_PRSN	0x00000008
#define PWR_LED_PRSN	0x00000010
#define HP_SUPR_RM	0x00000020
#define HP_CAP		0x00000040
#define SLOT_PWR_VALUE	0x000003F8
#define SLOT_PWR_LIMIT	0x00000C00
#define PSN		0xFFF80000	/* PSN: Physical Slot Number */

/* Field definitions in Slot Control Register */
#define ATTN_BUTTN_ENABLE		0x0001
#define PWR_FAULT_DETECT_ENABLE		0x0002
#define MRL_DETECT_ENABLE		0x0004
#define PRSN_DETECT_ENABLE		0x0008
#define CMD_CMPL_INTR_ENABLE		0x0010
#define HP_INTR_ENABLE			0x0020
#define ATTN_LED_CTRL			0x00C0
#define PWR_LED_CTRL			0x0300
#define PWR_CTRL			0x0400

/* Attention indicator and Power indicator states */
#define LED_ON		0x01
#define LED_BLINK	0x10
#define LED_OFF		0x11

/* Power Control Command */
#define POWER_ON	0
#define POWER_OFF	0x0400

/* Field definitions in Slot Status Register */
#define ATTN_BUTTN_PRESSED	0x0001
#define PWR_FAULT_DETECTED	0x0002
#define MRL_SENS_CHANGED	0x0004
#define PRSN_DETECT_CHANGED	0x0008
#define CMD_COMPLETED		0x0010
#define MRL_STATE		0x0020
#define PRSN_STATE		0x0040

struct php_ctlr_state_s {
	struct php_ctlr_state_s *pnext;
	struct pci_dev *pci_dev;
	unsigned int irq;
	unsigned long flags;				/* spinlock's */
	u32 slot_device_offset;
	u32 num_slots;
    	struct timer_list	int_poll_timer;		/* Added for poll event */
	php_intr_callback_t 	attention_button_callback;
	php_intr_callback_t 	switch_change_callback;
	php_intr_callback_t 	presence_change_callback;
	php_intr_callback_t 	power_fault_callback;
	void 			*callback_instance_id;
	struct ctrl_reg 	*creg;				/* Ptr to controller register space */
};


static spinlock_t hpc_event_lock;

DEFINE_DBG_BUFFER		/* Debug string buffer for entire HPC defined here */
static struct php_ctlr_state_s *php_ctlr_list_head;	/* HPC state linked list */
static int ctlr_seq_num;	/* Controller sequence # */
static spinlock_t list_lock;

static irqreturn_t pcie_isr(int IRQ, void *dev_id, struct pt_regs *regs);

static void start_int_poll_timer(struct php_ctlr_state_s *php_ctlr, int seconds);

/* This is the interrupt polling timeout function. */
static void int_poll_timeout(unsigned long lphp_ctlr)
{
	struct php_ctlr_state_s *php_ctlr = (struct php_ctlr_state_s *)lphp_ctlr;

	DBG_ENTER_ROUTINE

	if ( !php_ctlr ) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return;
	}

	/* Poll for interrupt events.  regs == NULL => polling */
	pcie_isr( 0, (void *)php_ctlr, NULL );

	init_timer(&php_ctlr->int_poll_timer);

	if (!pciehp_poll_time)
		pciehp_poll_time = 2; /* reset timer to poll in 2 secs if user doesn't specify at module installation*/

	start_int_poll_timer(php_ctlr, pciehp_poll_time);  
	
	return;
}

/* This function starts the interrupt polling timer. */
static void start_int_poll_timer(struct php_ctlr_state_s *php_ctlr, int seconds)
{
	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return;
	}

	if ( ( seconds <= 0 ) || ( seconds > 60 ) )
        	seconds = 2;            /* Clamp to sane value */

	php_ctlr->int_poll_timer.function = &int_poll_timeout;
	php_ctlr->int_poll_timer.data = (unsigned long)php_ctlr;    /* Instance data */
	php_ctlr->int_poll_timer.expires = jiffies + seconds * HZ;
	add_timer(&php_ctlr->int_poll_timer);

	return;
}

static int pcie_write_cmd(struct slot *slot, u16 cmd)
{
	struct php_ctlr_state_s *php_ctlr = slot->ctrl->hpc_ctlr_handle;
	int retval = 0;
	u16 slot_status;

	DBG_ENTER_ROUTINE 
	
	dbg("%s : Enter\n", __FUNCTION__);
	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return -1;
	}

	retval = hp_register_read_word(php_ctlr->pci_dev, SLOT_STATUS, slot_status);
	if (retval) {
			err("%s : hp_register_read_word SLOT_STATUS failed\n", __FUNCTION__);
			return retval;
		}
	dbg("%s : hp_register_read_word SLOT_STATUS %x\n", __FUNCTION__, slot_status);
	
	if ((slot_status & CMD_COMPLETED) == CMD_COMPLETED ) { 
		/* After 1 sec and CMD_COMPLETED still not set, just proceed forward to issue 
		   the next command according to spec.  Just print out the error message */
		dbg("%s : CMD_COMPLETED not clear after 1 sec.\n", __FUNCTION__);
	}

	dbg("%s: Before hp_register_write_word SLOT_CTRL %x\n", __FUNCTION__, cmd);
	retval = hp_register_write_word(php_ctlr->pci_dev, SLOT_CTRL, cmd | CMD_CMPL_INTR_ENABLE);
	if (retval) {
		err("%s : hp_register_write_word SLOT_CTRL failed\n", __FUNCTION__);
		return retval;
	}
	dbg("%s : hp_register_write_word SLOT_CTRL %x\n", __FUNCTION__, cmd | CMD_CMPL_INTR_ENABLE);
	dbg("%s : Exit\n", __FUNCTION__);

	DBG_LEAVE_ROUTINE 
	return retval;
}

static int hpc_check_lnk_status(struct controller *ctrl)
{
	struct php_ctlr_state_s *php_ctlr = ctrl->hpc_ctlr_handle;
	u16 lnk_status;
	int retval = 0;

	DBG_ENTER_ROUTINE 

	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return -1;
	}
	
	retval = hp_register_read_word(php_ctlr->pci_dev, LNK_STATUS, lnk_status);

	if (retval) {
		err("%s : hp_register_read_word LNK_STATUS failed\n", __FUNCTION__);
		return retval;
	}

	dbg("%s: lnk_status = %x\n", __FUNCTION__, lnk_status);
	if ( (lnk_status & LNK_TRN) || (lnk_status & LNK_TRN_ERR) || 
		!(lnk_status & NEG_LINK_WD)) {
		err("%s : Link Training Error occurs \n", __FUNCTION__);
		retval = -1;
		return retval;
	}

	DBG_LEAVE_ROUTINE 
	return retval;
}


static int hpc_get_attention_status(struct slot *slot, u8 *status)
{
	struct php_ctlr_state_s *php_ctlr = slot->ctrl->hpc_ctlr_handle;
	u16 slot_ctrl;
	u8 atten_led_state;
	int retval = 0;
	
	DBG_ENTER_ROUTINE 

	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return -1;
	}

	retval = hp_register_read_word(php_ctlr->pci_dev, SLOT_CTRL, slot_ctrl);

	if (retval) {
		err("%s : hp_register_read_word SLOT_CTRL failed\n", __FUNCTION__);
		return retval;
	}

	dbg("%s: SLOT_CTRL %x, value read %x\n", __FUNCTION__,SLOT_CTRL, slot_ctrl);

	atten_led_state = (slot_ctrl & ATTN_LED_CTRL) >> 6;

	switch (atten_led_state) {
	case 0:
		*status = 0xFF;	/* Reserved */
		break;
	case 1:
		*status = 1;	/* On */
		break;
	case 2:
		*status = 2;	/* Blink */
		break;
	case 3:
		*status = 0;	/* Off */
		break;
	default:
		*status = 0xFF;
		break;
	}

	DBG_LEAVE_ROUTINE 
	return 0;
}

static int hpc_get_power_status(struct slot * slot, u8 *status)
{
	struct php_ctlr_state_s *php_ctlr = slot->ctrl->hpc_ctlr_handle;
	u16 slot_ctrl;
	u8 pwr_state;
	int	retval = 0;
	
	DBG_ENTER_ROUTINE 

	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return -1;
	}

	retval = hp_register_read_word(php_ctlr->pci_dev, SLOT_CTRL, slot_ctrl);

	if (retval) {
		err("%s : hp_register_read_word SLOT_CTRL failed\n", __FUNCTION__);
		return retval;
	}
	dbg("%s: SLOT_CTRL %x value read %x\n", __FUNCTION__, SLOT_CTRL, slot_ctrl);

	pwr_state = (slot_ctrl & PWR_CTRL) >> 10;

	switch (pwr_state) {
	case 0:
		*status = 1;
		break;
	case 1:
		*status = 0;	
		break;
	default:
		*status = 0xFF;
		break;
	}

	DBG_LEAVE_ROUTINE 
	return retval;
}


static int hpc_get_latch_status(struct slot *slot, u8 *status)
{
	struct php_ctlr_state_s *php_ctlr = slot->ctrl->hpc_ctlr_handle;
	u16 slot_status;
	int retval = 0;

	DBG_ENTER_ROUTINE 

	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return -1;
	}

	retval = hp_register_read_word(php_ctlr->pci_dev, SLOT_STATUS, slot_status);

	if (retval) {
		err("%s : hp_register_read_word SLOT_STATUS failed\n", __FUNCTION__);
		return retval;
	}

	*status = (((slot_status & MRL_STATE) >> 5) == 0) ? 0 : 1;  

	DBG_LEAVE_ROUTINE 
	return 0;
}

static int hpc_get_adapter_status(struct slot *slot, u8 *status)
{
	struct php_ctlr_state_s *php_ctlr = slot->ctrl->hpc_ctlr_handle;
	u16 slot_status;
	u8 card_state;
	int retval = 0;

	DBG_ENTER_ROUTINE 

	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return -1;
	}

	retval = hp_register_read_word(php_ctlr->pci_dev, SLOT_STATUS, slot_status);

	if (retval) {
		err("%s : hp_register_read_word SLOT_STATUS failed\n", __FUNCTION__);
		return retval;
	}
	card_state = (u8)((slot_status & PRSN_STATE) >> 6);
	*status = (card_state == 1) ? 1 : 0;

	DBG_LEAVE_ROUTINE 
	return 0;
}

static int hpc_query_power_fault(struct slot * slot)
{
	struct php_ctlr_state_s *php_ctlr = slot->ctrl->hpc_ctlr_handle;
	u16 slot_status;
	u8 pwr_fault;
	int retval = 0;
	u8 status;

	DBG_ENTER_ROUTINE 

	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return -1;
	}

	retval = hp_register_read_word(php_ctlr->pci_dev, SLOT_STATUS, slot_status);

	if (retval) {
		err("%s : hp_register_read_word SLOT_STATUS failed\n", __FUNCTION__);
		return retval;
	}
	pwr_fault = (u8)((slot_status & PWR_FAULT_DETECTED) >> 1);
	status = (pwr_fault != 1) ? 1 : 0;
	
	DBG_LEAVE_ROUTINE
	/* Note: Logic 0 => fault */
	return status;
}

static int hpc_set_attention_status(struct slot *slot, u8 value)
{
	struct php_ctlr_state_s *php_ctlr = slot->ctrl->hpc_ctlr_handle;
	u16 slot_cmd = 0;
	u16 slot_ctrl;
	int rc = 0;

	dbg("%s: \n", __FUNCTION__);
	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return -1;
	}

	if (slot->hp_slot >= php_ctlr->num_slots) {
		err("%s: Invalid HPC slot number!\n", __FUNCTION__);
		return -1;
	}
	rc = hp_register_read_word(php_ctlr->pci_dev, SLOT_CTRL, slot_ctrl);

	if (rc) {
		err("%s : hp_register_read_word SLOT_CTRL failed\n", __FUNCTION__);
		return rc;
	}
	dbg("%s : hp_register_read_word SLOT_CTRL %x\n", __FUNCTION__, slot_ctrl);

	switch (value) {
		case 0 :	/* turn off */
			slot_cmd = (slot_ctrl & ~ATTN_LED_CTRL) | 0x00C0;
			break;
		case 1:		/* turn on */
			slot_cmd = (slot_ctrl & ~ATTN_LED_CTRL) | 0x0040;
			break;
		case 2:		/* turn blink */
			slot_cmd = (slot_ctrl & ~ATTN_LED_CTRL) | 0x0080;
			break;
		default:
			return -1;
	}
	if (!pciehp_poll_mode)
		slot_cmd = slot_cmd | HP_INTR_ENABLE; 

	pcie_write_cmd(slot, slot_cmd);
	dbg("%s: SLOT_CTRL %x write cmd %x\n", __FUNCTION__, SLOT_CTRL, slot_cmd);
	
	return rc;
}


static void hpc_set_green_led_on(struct slot *slot)
{
	struct php_ctlr_state_s *php_ctlr = slot->ctrl->hpc_ctlr_handle;
	u16 slot_cmd;
	u16 slot_ctrl;
	int rc = 0;
       	
	dbg("%s: \n", __FUNCTION__);	
	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return ;
	}

	if (slot->hp_slot >= php_ctlr->num_slots) {
		err("%s: Invalid HPC slot number!\n", __FUNCTION__);
		return ;
	}

	rc = hp_register_read_word(php_ctlr->pci_dev, SLOT_CTRL, slot_ctrl);

	if (rc) {
		err("%s : hp_register_read_word SLOT_CTRL failed\n", __FUNCTION__);
		return;
	}
	dbg("%s : hp_register_read_word SLOT_CTRL %x\n", __FUNCTION__, slot_ctrl);
	slot_cmd = (slot_ctrl & ~PWR_LED_CTRL) | 0x0100;
	if (!pciehp_poll_mode)
		slot_cmd = slot_cmd | HP_INTR_ENABLE; 

	pcie_write_cmd(slot, slot_cmd);

	dbg("%s: SLOT_CTRL %x write cmd %x\n",__FUNCTION__, SLOT_CTRL, slot_cmd);
	return;
}

static void hpc_set_green_led_off(struct slot *slot)
{
	struct php_ctlr_state_s *php_ctlr = slot->ctrl->hpc_ctlr_handle;
	u16 slot_cmd;
	u16 slot_ctrl;
	int rc = 0;

	dbg("%s: \n", __FUNCTION__);	
	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return ;
	}

	if (slot->hp_slot >= php_ctlr->num_slots) {
		err("%s: Invalid HPC slot number!\n", __FUNCTION__);
		return ;
	}

	rc = hp_register_read_word(php_ctlr->pci_dev, SLOT_CTRL, slot_ctrl);

	if (rc) {
		err("%s : hp_register_read_word SLOT_CTRL failed\n", __FUNCTION__);
		return;
	}
	dbg("%s : hp_register_read_word SLOT_CTRL %x\n", __FUNCTION__, slot_ctrl);

	slot_cmd = (slot_ctrl & ~PWR_LED_CTRL) | 0x0300;

	if (!pciehp_poll_mode)
		slot_cmd = slot_cmd | HP_INTR_ENABLE; 
	pcie_write_cmd(slot, slot_cmd);
	dbg("%s: SLOT_CTRL %x write cmd %x\n", __FUNCTION__, SLOT_CTRL, slot_cmd);

	return;
}

static void hpc_set_green_led_blink(struct slot *slot)
{
	struct php_ctlr_state_s *php_ctlr = slot->ctrl->hpc_ctlr_handle;
	u16 slot_cmd;
	u16 slot_ctrl;
	int rc = 0; 
	
	dbg("%s: \n", __FUNCTION__);	
	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return ;
	}

	if (slot->hp_slot >= php_ctlr->num_slots) {
		err("%s: Invalid HPC slot number!\n", __FUNCTION__);
		return ;
	}

	rc = hp_register_read_word(php_ctlr->pci_dev, SLOT_CTRL, slot_ctrl);

	if (rc) {
		err("%s : hp_register_read_word SLOT_CTRL failed\n", __FUNCTION__);
		return;
	}
	dbg("%s : hp_register_read_word SLOT_CTRL %x\n", __FUNCTION__, slot_ctrl);

	slot_cmd = (slot_ctrl & ~PWR_LED_CTRL) | 0x0200;

	if (!pciehp_poll_mode)
		slot_cmd = slot_cmd | HP_INTR_ENABLE; 
	pcie_write_cmd(slot, slot_cmd);

	dbg("%s: SLOT_CTRL %x write cmd %x\n",__FUNCTION__, SLOT_CTRL, slot_cmd);
	return;
}

int pcie_get_ctlr_slot_config(struct controller *ctrl,
	int *num_ctlr_slots,	/* number of slots in this HPC; only 1 in PCIE  */	
	int *first_device_num,	/* PCI dev num of the first slot in this PCIE	*/
	int *physical_slot_num,	/* phy slot num of the first slot in this PCIE	*/
	int *updown,		/* physical_slot_num increament: 1 or -1	*/
	int *flags)
{
	struct php_ctlr_state_s *php_ctlr = ctrl->hpc_ctlr_handle;
	u32 slot_cap;
	int rc = 0;
	
	DBG_ENTER_ROUTINE 

	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return -1;
	}

	*first_device_num = 0;
	*num_ctlr_slots = 1; 

	rc = hp_register_read_dword(php_ctlr->pci_dev, SLOT_CAP, slot_cap);

	if (rc) {
		err("%s : hp_register_read_dword SLOT_CAP failed\n", __FUNCTION__);
		return -1;
	}
	
	*physical_slot_num = slot_cap >> 19;

	*updown = -1;

	DBG_LEAVE_ROUTINE 
	return 0;
}

static void hpc_release_ctlr(struct controller *ctrl)
{
	struct php_ctlr_state_s *php_ctlr = ctrl->hpc_ctlr_handle;
	struct php_ctlr_state_s *p, *p_prev;

	DBG_ENTER_ROUTINE 

	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return ;
	}

	if (pciehp_poll_mode) {
	    del_timer(&php_ctlr->int_poll_timer);
	} else {	
		if (php_ctlr->irq) {
			free_irq(php_ctlr->irq, ctrl);
			php_ctlr->irq = 0;
		}
	}
	if (php_ctlr->pci_dev) 
		php_ctlr->pci_dev = NULL;

	spin_lock(&list_lock);
	p = php_ctlr_list_head;
	p_prev = NULL;
	while (p) {
		if (p == php_ctlr) {
			if (p_prev)
				p_prev->pnext = p->pnext;
			else
				php_ctlr_list_head = p->pnext;
			break;
		} else {
			p_prev = p;
			p = p->pnext;
		}
	}
	spin_unlock(&list_lock);

	kfree(php_ctlr);

	DBG_LEAVE_ROUTINE
			  
}

static int hpc_power_on_slot(struct slot * slot)
{
	struct php_ctlr_state_s *php_ctlr = slot->ctrl->hpc_ctlr_handle;
	u16 slot_cmd;
	u16 slot_ctrl;

	int retval = 0;

	DBG_ENTER_ROUTINE 
	dbg("%s: \n", __FUNCTION__);	

	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return -1;
	}

	dbg("%s: slot->hp_slot %x\n", __FUNCTION__, slot->hp_slot);
	if (slot->hp_slot >= php_ctlr->num_slots) {
		err("%s: Invalid HPC slot number!\n", __FUNCTION__);
		return -1;
	}

	retval = hp_register_read_word(php_ctlr->pci_dev, SLOT_CTRL, slot_ctrl);

	if (retval) {
		err("%s : hp_register_read_word SLOT_CTRL failed\n", __FUNCTION__);
		return retval;
	}
	dbg("%s: SLOT_CTRL %x, value read %xn", __FUNCTION__, SLOT_CTRL, 
		slot_ctrl);

	slot_cmd = (slot_ctrl & ~PWR_CTRL) | POWER_ON;

	if (!pciehp_poll_mode)
		slot_cmd = slot_cmd | HP_INTR_ENABLE; 

	retval = pcie_write_cmd(slot, slot_cmd);

	if (retval) {
		err("%s: Write %x command failed!\n", __FUNCTION__, slot_cmd);
		return -1;
	}
	dbg("%s: SLOT_CTRL %x write cmd %x\n",__FUNCTION__, SLOT_CTRL, slot_cmd);

	DBG_LEAVE_ROUTINE

	return retval;
}

static int hpc_power_off_slot(struct slot * slot)
{
	struct php_ctlr_state_s *php_ctlr = slot->ctrl->hpc_ctlr_handle;
	u16 slot_cmd;
	u16 slot_ctrl;

	int retval = 0;

	DBG_ENTER_ROUTINE 
	dbg("%s: \n", __FUNCTION__);	

	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return -1;
	}

	dbg("%s: slot->hp_slot %x\n", __FUNCTION__, slot->hp_slot);
	slot->hp_slot = 0;
	if (slot->hp_slot >= php_ctlr->num_slots) {
		err("%s: Invalid HPC slot number!\n", __FUNCTION__);
		return -1;
	}
	retval = hp_register_read_word(php_ctlr->pci_dev, SLOT_CTRL, slot_ctrl);

	if (retval) {
		err("%s : hp_register_read_word SLOT_CTRL failed\n", __FUNCTION__);
		return retval;
	}
	dbg("%s: SLOT_CTRL %x, value read %x\n", __FUNCTION__, SLOT_CTRL, 
		slot_ctrl);

	slot_cmd = (slot_ctrl & ~PWR_CTRL) | POWER_OFF;

	if (!pciehp_poll_mode)
		slot_cmd = slot_cmd | HP_INTR_ENABLE; 

	retval = pcie_write_cmd(slot, slot_cmd);

	if (retval) {
		err("%s: Write command failed!\n", __FUNCTION__);
		return -1;
	}
	dbg("%s: SLOT_CTRL %x write cmd %x\n",__FUNCTION__, SLOT_CTRL, slot_cmd);

	DBG_LEAVE_ROUTINE

	return retval;
}

static irqreturn_t pcie_isr(int IRQ, void *dev_id, struct pt_regs *regs)
{
	struct controller *ctrl = NULL;
	struct php_ctlr_state_s *php_ctlr;
	u8 schedule_flag = 0;
	u16 slot_status, intr_detect, intr_loc;
	u16 temp_word;
	int hp_slot = 0;	/* only 1 slot per PCI Express port */
	int rc = 0;

	if (!dev_id)
		return IRQ_NONE;

	if (!pciehp_poll_mode) { 
		ctrl = dev_id;
		php_ctlr = ctrl->hpc_ctlr_handle;
	} else {
		php_ctlr = dev_id;
		ctrl = (struct controller *)php_ctlr->callback_instance_id;
	}

	if (!ctrl) {
		dbg("%s: dev_id %p ctlr == NULL\n", __FUNCTION__, (void*) dev_id);
		return IRQ_NONE;
	}
	
	if (!php_ctlr) {
		dbg("%s: php_ctlr == NULL\n", __FUNCTION__);
		return IRQ_NONE;
	}

	rc = hp_register_read_word(php_ctlr->pci_dev, SLOT_STATUS, slot_status);
	if (rc) {
		err("%s : hp_register_read_word SLOT_STATUS failed\n", __FUNCTION__);
		return IRQ_NONE;
	}

	intr_detect = ( ATTN_BUTTN_PRESSED | PWR_FAULT_DETECTED | MRL_SENS_CHANGED |
					PRSN_DETECT_CHANGED | CMD_COMPLETED );

	intr_loc = slot_status & intr_detect;

	/* Check to see if it was our interrupt */
	if ( !intr_loc )
		return IRQ_NONE;

	dbg("%s: intr_loc %x\n", __FUNCTION__, intr_loc);
	/* Mask Hot-plug Interrupt Enable */
	if (!pciehp_poll_mode) {
		rc = hp_register_read_word(php_ctlr->pci_dev, SLOT_CTRL, temp_word);
		if (rc) {
			err("%s : hp_register_read_word SLOT_CTRL failed\n", __FUNCTION__);
			return IRQ_NONE;
		}

		dbg("%s: Set Mask Hot-plug Interrupt Enable\n", __FUNCTION__);
		dbg("%s: hp_register_read_word SLOT_CTRL with value %x\n", __FUNCTION__, temp_word);
		temp_word = (temp_word & ~HP_INTR_ENABLE & ~CMD_CMPL_INTR_ENABLE) | 0x00;

		rc = hp_register_write_word(php_ctlr->pci_dev, SLOT_CTRL, temp_word);
		if (rc) {
			err("%s : hp_register_write_word SLOT_CTRL failed\n", __FUNCTION__);
			return IRQ_NONE;
		}
		dbg("%s: hp_register_write_word SLOT_CTRL with value %x\n", __FUNCTION__, temp_word);
		
		rc = hp_register_read_word(php_ctlr->pci_dev, SLOT_STATUS, slot_status);
		if (rc) {
			err("%s : hp_register_read_word SLOT_STATUS failed\n", __FUNCTION__);
			return IRQ_NONE;
		}
		dbg("%s: hp_register_read_word SLOT_STATUS with value %x\n", __FUNCTION__, slot_status); 
		
		/* Clear command complete interrupt caused by this write */
		temp_word = 0x1f;
		rc = hp_register_write_word(php_ctlr->pci_dev, SLOT_STATUS, temp_word);
		if (rc) {
			err("%s : hp_register_write_word SLOT_STATUS failed\n", __FUNCTION__);
			return IRQ_NONE;
		}
		dbg("%s: hp_register_write_word SLOT_STATUS with value %x\n", __FUNCTION__, temp_word);
	}
	
	if (intr_loc & CMD_COMPLETED) {
		/* 
		 * Command Complete Interrupt Pending 
		 */
		dbg("%s: In Command Complete Interrupt Pending\n", __FUNCTION__);
		wake_up_interruptible(&ctrl->queue);
	}

	if ((php_ctlr->switch_change_callback) && (intr_loc & MRL_SENS_CHANGED))
		schedule_flag += php_ctlr->switch_change_callback(
			hp_slot, php_ctlr->callback_instance_id);
	if ((php_ctlr->attention_button_callback) && (intr_loc & ATTN_BUTTN_PRESSED))
		schedule_flag += php_ctlr->attention_button_callback(
			hp_slot, php_ctlr->callback_instance_id);
	if ((php_ctlr->presence_change_callback) && (intr_loc & PRSN_DETECT_CHANGED))
		schedule_flag += php_ctlr->presence_change_callback(
			hp_slot , php_ctlr->callback_instance_id);
	if ((php_ctlr->power_fault_callback) && (intr_loc & PWR_FAULT_DETECTED))
		schedule_flag += php_ctlr->power_fault_callback(
			hp_slot, php_ctlr->callback_instance_id);

	/* Clear all events after serving them */
	temp_word = 0x1F;
	rc = hp_register_write_word(php_ctlr->pci_dev, SLOT_STATUS, temp_word);
	if (rc) {
		err("%s : hp_register_write_word SLOT_STATUS failed\n", __FUNCTION__);
		return IRQ_NONE;
	}
	/* Unmask Hot-plug Interrupt Enable */
	if (!pciehp_poll_mode) {
		rc = hp_register_read_word(php_ctlr->pci_dev, SLOT_CTRL, temp_word);
		if (rc) {
			err("%s : hp_register_read_word SLOT_CTRL failed\n", __FUNCTION__);
			return IRQ_NONE;
		}

		dbg("%s: Unmask Hot-plug Interrupt Enable\n", __FUNCTION__);
		dbg("%s: hp_register_read_word SLOT_CTRL with value %x\n", __FUNCTION__, temp_word);
		temp_word = (temp_word & ~HP_INTR_ENABLE) | HP_INTR_ENABLE;

		rc = hp_register_write_word(php_ctlr->pci_dev, SLOT_CTRL, temp_word);
		if (rc) {
			err("%s : hp_register_write_word SLOT_CTRL failed\n", __FUNCTION__);
			return IRQ_NONE;
		}
		dbg("%s: hp_register_write_word SLOT_CTRL with value %x\n", __FUNCTION__, temp_word); 	
	
		rc = hp_register_read_word(php_ctlr->pci_dev, SLOT_STATUS, slot_status);
		if (rc) {
			err("%s : hp_register_read_word SLOT_STATUS failed\n", __FUNCTION__);
			return IRQ_NONE;
		}
		dbg("%s: hp_register_read_word SLOT_STATUS with value %x\n", __FUNCTION__, slot_status); 
		
		/* Clear command complete interrupt caused by this write */
		temp_word = 0x1F;
		rc = hp_register_write_word(php_ctlr->pci_dev, SLOT_STATUS, temp_word);
		if (rc) {
			err("%s : hp_register_write_word SLOT_STATUS failed\n", __FUNCTION__);
			return IRQ_NONE;
		}
		dbg("%s: hp_register_write_word SLOT_STATUS with value %x\n", __FUNCTION__, temp_word); 
	}
	
	return IRQ_HANDLED;
}

static int hpc_get_max_lnk_speed (struct slot *slot, enum pcie_link_speed *value)
{
	struct php_ctlr_state_s *php_ctlr = slot->ctrl->hpc_ctlr_handle;
	enum pcie_link_speed lnk_speed;
	u32	lnk_cap;
	int retval = 0;

	DBG_ENTER_ROUTINE 

	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return -1;
	}

	if (slot->hp_slot >= php_ctlr->num_slots) {
		err("%s: Invalid HPC slot number!\n", __FUNCTION__);
		return -1;
	}

	retval = hp_register_read_dword(php_ctlr->pci_dev, LNK_CAP, lnk_cap);

	if (retval) {
		err("%s : hp_register_read_dword  LNK_CAP failed\n", __FUNCTION__);
		return retval;
	}

	switch (lnk_cap & 0x000F) {
	case 1:
		lnk_speed = PCIE_2PT5GB;
		break;
	default:
		lnk_speed = PCIE_LNK_SPEED_UNKNOWN;
		break;
	}

	*value = lnk_speed;
	dbg("Max link speed = %d\n", lnk_speed);
	DBG_LEAVE_ROUTINE 
	return retval;
}

static int hpc_get_max_lnk_width (struct slot *slot, enum pcie_link_width *value)
{
	struct php_ctlr_state_s *php_ctlr = slot->ctrl->hpc_ctlr_handle;
	enum pcie_link_width lnk_wdth;
	u32	lnk_cap;
	int retval = 0;

	DBG_ENTER_ROUTINE 

	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return -1;
	}

	if (slot->hp_slot >= php_ctlr->num_slots) {
		err("%s: Invalid HPC slot number!\n", __FUNCTION__);
		return -1;
	}

	retval = hp_register_read_dword(php_ctlr->pci_dev, LNK_CAP, lnk_cap);

	if (retval) {
		err("%s : hp_register_read_dword  LNK_CAP failed\n", __FUNCTION__);
		return retval;
	}

	switch ((lnk_cap & 0x03F0) >> 4){
	case 0:
		lnk_wdth = PCIE_LNK_WIDTH_RESRV;
		break;
	case 1:
		lnk_wdth = PCIE_LNK_X1;
		break;
	case 2:
		lnk_wdth = PCIE_LNK_X2;
		break;
	case 4:
		lnk_wdth = PCIE_LNK_X4;
		break;
	case 8:
		lnk_wdth = PCIE_LNK_X8;
		break;
	case 12:
		lnk_wdth = PCIE_LNK_X12;
		break;
	case 16:
		lnk_wdth = PCIE_LNK_X16;
		break;
	case 32:
		lnk_wdth = PCIE_LNK_X32;
		break;
	default:
		lnk_wdth = PCIE_LNK_WIDTH_UNKNOWN;
		break;
	}

	*value = lnk_wdth;
	dbg("Max link width = %d\n", lnk_wdth);
	DBG_LEAVE_ROUTINE 
	return retval;
}

static int hpc_get_cur_lnk_speed (struct slot *slot, enum pcie_link_speed *value)
{
	struct php_ctlr_state_s *php_ctlr = slot->ctrl->hpc_ctlr_handle;
	enum pcie_link_speed lnk_speed = PCI_SPEED_UNKNOWN;
	int retval = 0;
	u16 lnk_status;

	DBG_ENTER_ROUTINE 

	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return -1;
	}

	if (slot->hp_slot >= php_ctlr->num_slots) {
		err("%s: Invalid HPC slot number!\n", __FUNCTION__);
		return -1;
	}

	retval = hp_register_read_word(php_ctlr->pci_dev, LNK_STATUS, lnk_status);

	if (retval) {
		err("%s : hp_register_read_word LNK_STATUS failed\n", __FUNCTION__);
		return retval;
	}

	switch (lnk_status & 0x0F) {
	case 1:
		lnk_speed = PCIE_2PT5GB;
		break;
	default:
		lnk_speed = PCIE_LNK_SPEED_UNKNOWN;
		break;
	}

	*value = lnk_speed;
	dbg("Current link speed = %d\n", lnk_speed);
	DBG_LEAVE_ROUTINE 
	return retval;
}

static int hpc_get_cur_lnk_width (struct slot *slot, enum pcie_link_width *value)
{
	struct php_ctlr_state_s *php_ctlr = slot->ctrl->hpc_ctlr_handle;
	enum pcie_link_width lnk_wdth = PCIE_LNK_WIDTH_UNKNOWN;
	int retval = 0;
	u16 lnk_status;

	DBG_ENTER_ROUTINE 

	if (!php_ctlr) {
		err("%s: Invalid HPC controller handle!\n", __FUNCTION__);
		return -1;
	}

	if (slot->hp_slot >= php_ctlr->num_slots) {
		err("%s: Invalid HPC slot number!\n", __FUNCTION__);
		return -1;
	}

	retval = hp_register_read_word(php_ctlr->pci_dev, LNK_STATUS, lnk_status);

	if (retval) {
		err("%s : hp_register_read_word LNK_STATUS failed\n", __FUNCTION__);
		return retval;
	}
	
	switch ((lnk_status & 0x03F0) >> 4){
	case 0:
		lnk_wdth = PCIE_LNK_WIDTH_RESRV;
		break;
	case 1:
		lnk_wdth = PCIE_LNK_X1;
		break;
	case 2:
		lnk_wdth = PCIE_LNK_X2;
		break;
	case 4:
		lnk_wdth = PCIE_LNK_X4;
		break;
	case 8:
		lnk_wdth = PCIE_LNK_X8;
		break;
	case 12:
		lnk_wdth = PCIE_LNK_X12;
		break;
	case 16:
		lnk_wdth = PCIE_LNK_X16;
		break;
	case 32:
		lnk_wdth = PCIE_LNK_X32;
		break;
	default:
		lnk_wdth = PCIE_LNK_WIDTH_UNKNOWN;
		break;
	}

	*value = lnk_wdth;
	dbg("Current link width = %d\n", lnk_wdth);
	DBG_LEAVE_ROUTINE 
	return retval;
}

static struct hpc_ops pciehp_hpc_ops = {
	.power_on_slot			= hpc_power_on_slot,
	.power_off_slot			= hpc_power_off_slot,
	.set_attention_status		= hpc_set_attention_status,
	.get_power_status		= hpc_get_power_status,
	.get_attention_status		= hpc_get_attention_status,
	.get_latch_status		= hpc_get_latch_status,
	.get_adapter_status		= hpc_get_adapter_status,

	.get_max_bus_speed		= hpc_get_max_lnk_speed,
	.get_cur_bus_speed		= hpc_get_cur_lnk_speed,
	.get_max_lnk_width		= hpc_get_max_lnk_width,
	.get_cur_lnk_width		= hpc_get_cur_lnk_width,
	
	.query_power_fault		= hpc_query_power_fault,
	.green_led_on			= hpc_set_green_led_on,
	.green_led_off			= hpc_set_green_led_off,
	.green_led_blink		= hpc_set_green_led_blink,
	
	.release_ctlr			= hpc_release_ctlr,
	.check_lnk_status		= hpc_check_lnk_status,
};

int pcie_init(struct controller * ctrl,
	struct pci_dev *pdev,
	php_intr_callback_t attention_button_callback,
	php_intr_callback_t switch_change_callback,
	php_intr_callback_t presence_change_callback,
	php_intr_callback_t power_fault_callback)
{
	struct php_ctlr_state_s *php_ctlr, *p;
	void *instance_id = ctrl;
	int rc;
	static int first = 1;
	u16 temp_word;
	u16 cap_reg;
	u16 intr_enable;
	u32 slot_cap;
	int cap_base, saved_cap_base;
	u16 slot_status, slot_ctrl;

	DBG_ENTER_ROUTINE
	
	spin_lock_init(&list_lock);	
	php_ctlr = (struct php_ctlr_state_s *) kmalloc(sizeof(struct php_ctlr_state_s), GFP_KERNEL);

	if (!php_ctlr) {	/* allocate controller state data */
		err("%s: HPC controller memory allocation error!\n", __FUNCTION__);
		goto abort;
	}

	memset(php_ctlr, 0, sizeof(struct php_ctlr_state_s));

	php_ctlr->pci_dev = pdev;	/* save pci_dev in context */

	dbg("%s: pdev->vendor %x pdev->device %x\n", __FUNCTION__,
		pdev->vendor, pdev->device);

	saved_cap_base = pcie_cap_base;

	if ((cap_base = pci_find_capability(pdev, PCI_CAP_ID_EXP)) == 0) {
		dbg("%s: Can't find PCI_CAP_ID_EXP (0x10)\n", __FUNCTION__);
		goto abort_free_ctlr;
	}

	pcie_cap_base = cap_base;

	dbg("%s: pcie_cap_base %x\n", __FUNCTION__, pcie_cap_base);

	rc = hp_register_read_word(pdev, CAP_REG, cap_reg);
	if (rc) {
		err("%s : hp_register_read_word CAP_REG failed\n", __FUNCTION__);
		goto abort_free_ctlr;
	}
	dbg("%s: CAP_REG offset %x cap_reg %x\n", __FUNCTION__, CAP_REG, cap_reg);

	if (((cap_reg & SLOT_IMPL) == 0) || ((cap_reg & DEV_PORT_TYPE) != 0x0040)){
		dbg("%s : This is not a root port or the port is not connected to a slot\n", __FUNCTION__);
		goto abort_free_ctlr;
	}

	rc = hp_register_read_dword(php_ctlr->pci_dev, SLOT_CAP, slot_cap);
	if (rc) {
		err("%s : hp_register_read_word CAP_REG failed\n", __FUNCTION__);
		goto abort_free_ctlr;
	}
	dbg("%s: SLOT_CAP offset %x slot_cap %x\n", __FUNCTION__, SLOT_CAP, slot_cap);

	if (!(slot_cap & HP_CAP)) {
		dbg("%s : This slot is not hot-plug capable\n", __FUNCTION__);
		goto abort_free_ctlr;
	}
	/* For debugging purpose */
	rc = hp_register_read_word(php_ctlr->pci_dev, SLOT_STATUS, slot_status);
	if (rc) {
		err("%s : hp_register_read_word SLOT_STATUS failed\n", __FUNCTION__);
		goto abort_free_ctlr;
	}
	dbg("%s: SLOT_STATUS offset %x slot_status %x\n", __FUNCTION__, SLOT_STATUS, slot_status);

	rc = hp_register_read_word(php_ctlr->pci_dev, SLOT_CTRL, slot_ctrl);
	if (rc) {
		err("%s : hp_register_read_word SLOT_CTRL failed\n", __FUNCTION__);
		goto abort_free_ctlr;
	}
	dbg("%s: SLOT_CTRL offset %x slot_ctrl %x\n", __FUNCTION__, SLOT_CTRL, slot_ctrl);

	if (first) {
		spin_lock_init(&hpc_event_lock);
		first = 0;
	}

	dbg("pdev = %p: b:d:f:irq=0x%x:%x:%x:%x\n", pdev, pdev->bus->number, 
		PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn), pdev->irq);
	for ( rc = 0; rc < DEVICE_COUNT_RESOURCE; rc++)
		if (pci_resource_len(pdev, rc) > 0)
			dbg("pci resource[%d] start=0x%lx(len=0x%lx)\n", rc,
				pci_resource_start(pdev, rc), pci_resource_len(pdev, rc));

	info("HPC vendor_id %x device_id %x ss_vid %x ss_did %x\n", pdev->vendor, pdev->device, 
		pdev->subsystem_vendor, pdev->subsystem_device);

	init_MUTEX(&ctrl->crit_sect);
	/* setup wait queue */
	init_waitqueue_head(&ctrl->queue);

	/* find the IRQ */
	php_ctlr->irq = pdev->irq;
	dbg("HPC interrupt = %d\n", php_ctlr->irq);

	/* Save interrupt callback info */
	php_ctlr->attention_button_callback = attention_button_callback;
	php_ctlr->switch_change_callback = switch_change_callback;
	php_ctlr->presence_change_callback = presence_change_callback;
	php_ctlr->power_fault_callback = power_fault_callback;
	php_ctlr->callback_instance_id = instance_id;

	/* return PCI Controller Info */
	php_ctlr->slot_device_offset = 0;
	php_ctlr->num_slots = 1;

	/* Mask Hot-plug Interrupt Enable */
	rc = hp_register_read_word(pdev, SLOT_CTRL, temp_word);
	if (rc) {
		err("%s : hp_register_read_word SLOT_CTRL failed\n", __FUNCTION__);
		goto abort_free_ctlr;
	}

	dbg("%s: SLOT_CTRL %x value read %x\n", __FUNCTION__, SLOT_CTRL, temp_word);
	temp_word = (temp_word & ~HP_INTR_ENABLE & ~CMD_CMPL_INTR_ENABLE) | 0x00;

	rc = hp_register_write_word(pdev, SLOT_CTRL, temp_word);
	if (rc) {
		err("%s : hp_register_write_word SLOT_CTRL failed\n", __FUNCTION__);
		goto abort_free_ctlr;
	}
	dbg("%s : Mask HPIE hp_register_write_word SLOT_CTRL %x\n", __FUNCTION__, temp_word);

	rc = hp_register_read_word(php_ctlr->pci_dev, SLOT_STATUS, slot_status);
	if (rc) {
		err("%s : hp_register_read_word SLOT_STATUS failed\n", __FUNCTION__);
		goto abort_free_ctlr;
	}
	dbg("%s: Mask HPIE SLOT_STATUS offset %x reads slot_status %x\n", __FUNCTION__, SLOT_STATUS, slot_status);

	temp_word = 0x1F; /* Clear all events */
	rc = hp_register_write_word(php_ctlr->pci_dev, SLOT_STATUS, temp_word);
	if (rc) {
		err("%s : hp_register_write_word SLOT_STATUS failed\n", __FUNCTION__);
		goto abort_free_ctlr;
	}
	dbg("%s: SLOT_STATUS offset %x writes slot_status %x\n", __FUNCTION__, SLOT_STATUS, temp_word);

	if (pciehp_poll_mode)  {/* Install interrupt polling code */
		/* Install and start the interrupt polling timer */
		init_timer(&php_ctlr->int_poll_timer);
		start_int_poll_timer( php_ctlr, 10 );   /* start with 10 second delay */
	} else {
		/* Installs the interrupt handler */
		dbg("%s: pciehp_msi_quirk = %x\n", __FUNCTION__, pciehp_msi_quirk);
		if (!pciehp_msi_quirk) {
			rc = pci_enable_msi(pdev);
			if (rc) {
				info("Can't get msi for the hotplug controller\n");
				info("Use INTx for the hotplug controller\n");
				dbg("%s: rc = %x\n", __FUNCTION__, rc);
			} else 
				php_ctlr->irq = pdev->irq;
		}
		rc = request_irq(php_ctlr->irq, pcie_isr, SA_SHIRQ, MY_NAME, (void *) ctrl);
		dbg("%s: request_irq %d for hpc%d (returns %d)\n", __FUNCTION__, php_ctlr->irq, ctlr_seq_num, rc);
		if (rc) {
			err("Can't get irq %d for the hotplug controller\n", php_ctlr->irq);
			goto abort_free_ctlr;
		}
	}

	rc = hp_register_read_word(pdev, SLOT_CTRL, temp_word);
	if (rc) {
		err("%s : hp_register_read_word SLOT_CTRL failed\n", __FUNCTION__);
		goto abort_free_ctlr;
	}
	dbg("%s: SLOT_CTRL %x value read %x\n", __FUNCTION__, SLOT_CTRL, temp_word);

	intr_enable = ATTN_BUTTN_ENABLE | PWR_FAULT_DETECT_ENABLE | MRL_DETECT_ENABLE |
					PRSN_DETECT_ENABLE;

	temp_word = (temp_word & ~intr_enable) | intr_enable; 

	if (pciehp_poll_mode) {
		temp_word = (temp_word & ~HP_INTR_ENABLE) | 0x0;
	} else {
		temp_word = (temp_word & ~HP_INTR_ENABLE) | HP_INTR_ENABLE;
	}
	dbg("%s: temp_word %x\n", __FUNCTION__, temp_word);

	/* Unmask Hot-plug Interrupt Enable for the interrupt notification mechanism case */
	rc = hp_register_write_word(pdev, SLOT_CTRL, temp_word);
	if (rc) {
		err("%s : hp_register_write_word SLOT_CTRL failed\n", __FUNCTION__);
		goto abort_free_ctlr;
	}
	dbg("%s : Unmask HPIE hp_register_write_word SLOT_CTRL with %x\n", __FUNCTION__, temp_word);
	rc = hp_register_read_word(php_ctlr->pci_dev, SLOT_STATUS, slot_status);
	if (rc) {
		err("%s : hp_register_read_word SLOT_STATUS failed\n", __FUNCTION__);
		goto abort_free_ctlr;
	}
	dbg("%s: Unmask HPIE SLOT_STATUS offset %x reads slot_status %x\n", __FUNCTION__, 
		SLOT_STATUS, slot_status);
	
	temp_word =  0x1F; /* Clear all events */
	rc = hp_register_write_word(php_ctlr->pci_dev, SLOT_STATUS, temp_word);
	if (rc) {
		err("%s : hp_register_write_word SLOT_STATUS failed\n", __FUNCTION__);
		goto abort_free_ctlr;
	}
	dbg("%s: SLOT_STATUS offset %x writes slot_status %x\n", __FUNCTION__, SLOT_STATUS, temp_word);
	
	/*  Add this HPC instance into the HPC list */
	spin_lock(&list_lock);
	if (php_ctlr_list_head == 0) {
		php_ctlr_list_head = php_ctlr;
		p = php_ctlr_list_head;
		p->pnext = NULL;
	} else {
		p = php_ctlr_list_head;

		while (p->pnext)
			p = p->pnext;

		p->pnext = php_ctlr;
	}
	spin_unlock(&list_lock);

	ctlr_seq_num++;
	ctrl->hpc_ctlr_handle = php_ctlr;
	ctrl->hpc_ops = &pciehp_hpc_ops;

	DBG_LEAVE_ROUTINE
	return 0;

	/* We end up here for the many possible ways to fail this API.  */
abort_free_ctlr:
	pcie_cap_base = saved_cap_base;
	kfree(php_ctlr);
abort:
	DBG_LEAVE_ROUTINE
	return -1;
}
