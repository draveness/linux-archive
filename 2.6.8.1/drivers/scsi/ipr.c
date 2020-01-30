/*
 * ipr.c -- driver for IBM Power Linux RAID adapters
 *
 * Written By: Brian King, IBM Corporation
 *
 * Copyright (C) 2003, 2004 IBM Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/*
 * Notes:
 *
 * This driver is used to control the following SCSI adapters:
 *
 * IBM iSeries: 5702, 5703, 2780, 5709, 570A, 570B
 *
 * IBM pSeries: PCI-X Dual Channel Ultra 320 SCSI RAID Adapter
 *              PCI-X Dual Channel Ultra 320 SCSI Adapter
 *              PCI-X Dual Channel Ultra 320 SCSI RAID Enablement Card
 *              Embedded SCSI adapter on p615 and p655 systems
 *
 * Supported Hardware Features:
 *	- Ultra 320 SCSI controller
 *	- PCI-X host interface
 *	- Embedded PowerPC RISC Processor and Hardware XOR DMA Engine
 *	- Non-Volatile Write Cache
 *	- Supports attachment of non-RAID disks, tape, and optical devices
 *	- RAID Levels 0, 5, 10
 *	- Hot spare
 *	- Background Parity Checking
 *	- Background Data Scrubbing
 *	- Ability to increase the capacity of an existing RAID 5 disk array
 *		by adding disks
 *
 * Driver Features:
 *	- Tagged command queuing
 *	- Adapter microcode download
 *	- PCI hot plug
 *	- SCSI device hot plug
 *
 */

#include <linux/config.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/blkdev.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/processor.h>
#include <scsi/scsi.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_tcq.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_request.h>
#include "ipr.h"

/*
 *   Global Data
 */
static struct list_head ipr_ioa_head = LIST_HEAD_INIT(ipr_ioa_head);
static unsigned int ipr_log_level = IPR_DEFAULT_LOG_LEVEL;
static unsigned int ipr_max_speed = 1;
static int ipr_testmode = 0;
static spinlock_t ipr_driver_lock = SPIN_LOCK_UNLOCKED;

/* This table describes the differences between DMA controller chips */
static const struct ipr_chip_cfg_t ipr_chip_cfg[] = {
	{ /* Gemstone */
		.mailbox = 0x0042C,
		.cache_line_size = 0x20,
		{
			.set_interrupt_mask_reg = 0x0022C,
			.clr_interrupt_mask_reg = 0x00230,
			.sense_interrupt_mask_reg = 0x0022C,
			.clr_interrupt_reg = 0x00228,
			.sense_interrupt_reg = 0x00224,
			.ioarrin_reg = 0x00404,
			.sense_uproc_interrupt_reg = 0x00214,
			.set_uproc_interrupt_reg = 0x00214,
			.clr_uproc_interrupt_reg = 0x00218
		}
	},
	{ /* Snipe */
		.mailbox = 0x0052C,
		.cache_line_size = 0x20,
		{
			.set_interrupt_mask_reg = 0x00288,
			.clr_interrupt_mask_reg = 0x0028C,
			.sense_interrupt_mask_reg = 0x00288,
			.clr_interrupt_reg = 0x00284,
			.sense_interrupt_reg = 0x00280,
			.ioarrin_reg = 0x00504,
			.sense_uproc_interrupt_reg = 0x00290,
			.set_uproc_interrupt_reg = 0x00290,
			.clr_uproc_interrupt_reg = 0x00294
		}
	},
};

static int ipr_max_bus_speeds [] = {
	IPR_80MBs_SCSI_RATE, IPR_U160_SCSI_RATE, IPR_U320_SCSI_RATE
};

MODULE_AUTHOR("Brian King <brking@us.ibm.com>");
MODULE_DESCRIPTION("IBM Power RAID SCSI Adapter Driver");
module_param_named(max_speed, ipr_max_speed, uint, 0);
MODULE_PARM_DESC(max_speed, "Maximum bus speed (0-2). Default: 1=U160. Speeds: 0=80 MB/s, 1=U160, 2=U320");
module_param_named(log_level, ipr_log_level, uint, 0);
MODULE_PARM_DESC(log_level, "Set to 0 - 4 for increasing verbosity of device driver");
module_param_named(testmode, ipr_testmode, int, 0);
MODULE_PARM_DESC(testmode, "DANGEROUS!!! Allows unsupported configurations");
MODULE_LICENSE("GPL");
MODULE_VERSION(IPR_DRIVER_VERSION);

static const char *ipr_gpdd_dev_end_states[] = {
	"Command complete",
	"Terminated by host",
	"Terminated by device reset",
	"Terminated by bus reset",
	"Unknown",
	"Command not started"
};

static const char *ipr_gpdd_dev_bus_phases[] = {
	"Bus free",
	"Arbitration",
	"Selection",
	"Message out",
	"Command",
	"Message in",
	"Data out",
	"Data in",
	"Status",
	"Reselection",
	"Unknown"
};

/*  A constant array of IOASCs/URCs/Error Messages */
static const
struct ipr_error_table_t ipr_error_table[] = {
	{0x00000000, 1, 1,
	"8155: An unknown error was received"},
	{0x00330000, 0, 0,
	"Soft underlength error"},
	{0x005A0000, 0, 0,
	"Command to be cancelled not found"},
	{0x00808000, 0, 0,
	"Qualified success"},
	{0x01080000, 1, 1,
	"FFFE: Soft device bus error recovered by the IOA"},
	{0x01170600, 0, 1,
	"FFF9: Device sector reassign successful"},
	{0x01170900, 0, 1,
	"FFF7: Media error recovered by device rewrite procedures"},
	{0x01180200, 0, 1,
	"7001: IOA sector reassignment successful"},
	{0x01180500, 0, 1,
	"FFF9: Soft media error. Sector reassignment recommended"},
	{0x01180600, 0, 1,
	"FFF7: Media error recovered by IOA rewrite procedures"},
	{0x01418000, 0, 1,
	"FF3D: Soft PCI bus error recovered by the IOA"},
	{0x01440000, 1, 1,
	"FFF6: Device hardware error recovered by the IOA"},
	{0x01448100, 0, 1,
	"FFF6: Device hardware error recovered by the device"},
	{0x01448200, 1, 1,
	"FF3D: Soft IOA error recovered by the IOA"},
	{0x01448300, 0, 1,
	"FFFA: Undefined device response recovered by the IOA"},
	{0x014A0000, 1, 1,
	"FFF6: Device bus error, message or command phase"},
	{0x015D0000, 0, 1,
	"FFF6: Failure prediction threshold exceeded"},
	{0x015D9200, 0, 1,
	"8009: Impending cache battery pack failure"},
	{0x02040400, 0, 0,
	"34FF: Disk device format in progress"},
	{0x023F0000, 0, 0,
	"Synchronization required"},
	{0x024E0000, 0, 0,
	"No ready, IOA shutdown"},
	{0x02670100, 0, 1,
	"3020: Storage subsystem configuration error"},
	{0x03110B00, 0, 0,
	"FFF5: Medium error, data unreadable, recommend reassign"},
	{0x03110C00, 0, 0,
	"7000: Medium error, data unreadable, do not reassign"},
	{0x03310000, 0, 1,
	"FFF3: Disk media format bad"},
	{0x04050000, 0, 1,
	"3002: Addressed device failed to respond to selection"},
	{0x04080000, 1, 1,
	"3100: Device bus error"},
	{0x04080100, 0, 1,
	"3109: IOA timed out a device command"},
	{0x04088000, 0, 0,
	"3120: SCSI bus is not operational"},
	{0x04118000, 0, 1,
	"9000: IOA reserved area data check"},
	{0x04118100, 0, 1,
	"9001: IOA reserved area invalid data pattern"},
	{0x04118200, 0, 1,
	"9002: IOA reserved area LRC error"},
	{0x04320000, 0, 1,
	"102E: Out of alternate sectors for disk storage"},
	{0x04330000, 1, 1,
	"FFF4: Data transfer underlength error"},
	{0x04338000, 1, 1,
	"FFF4: Data transfer overlength error"},
	{0x043E0100, 0, 1,
	"3400: Logical unit failure"},
	{0x04408500, 0, 1,
	"FFF4: Device microcode is corrupt"},
	{0x04418000, 1, 1,
	"8150: PCI bus error"},
	{0x04430000, 1, 0,
	"Unsupported device bus message received"},
	{0x04440000, 1, 1,
	"FFF4: Disk device problem"},
	{0x04448200, 1, 1,
	"8150: Permanent IOA failure"},
	{0x04448300, 0, 1,
	"3010: Disk device returned wrong response to IOA"},
	{0x04448400, 0, 1,
	"8151: IOA microcode error"},
	{0x04448500, 0, 0,
	"Device bus status error"},
	{0x04448600, 0, 1,
	"8157: IOA error requiring IOA reset to recover"},
	{0x04490000, 0, 0,
	"Message reject received from the device"},
	{0x04449200, 0, 1,
	"8008: A permanent cache battery pack failure occurred"},
	{0x0444A000, 0, 1,
	"9090: Disk unit has been modified after the last known status"},
	{0x0444A200, 0, 1,
	"9081: IOA detected device error"},
	{0x0444A300, 0, 1,
	"9082: IOA detected device error"},
	{0x044A0000, 1, 1,
	"3110: Device bus error, message or command phase"},
	{0x04670400, 0, 1,
	"9091: Incorrect hardware configuration change has been detected"},
	{0x046E0000, 0, 1,
	"FFF4: Command to logical unit failed"},
	{0x05240000, 1, 0,
	"Illegal request, invalid request type or request packet"},
	{0x05250000, 0, 0,
	"Illegal request, invalid resource handle"},
	{0x05260000, 0, 0,
	"Illegal request, invalid field in parameter list"},
	{0x05260100, 0, 0,
	"Illegal request, parameter not supported"},
	{0x05260200, 0, 0,
	"Illegal request, parameter value invalid"},
	{0x052C0000, 0, 0,
	"Illegal request, command sequence error"},
	{0x06040500, 0, 1,
	"9031: Array protection temporarily suspended, protection resuming"},
	{0x06040600, 0, 1,
	"9040: Array protection temporarily suspended, protection resuming"},
	{0x06290000, 0, 1,
	"FFFB: SCSI bus was reset"},
	{0x06290500, 0, 0,
	"FFFE: SCSI bus transition to single ended"},
	{0x06290600, 0, 0,
	"FFFE: SCSI bus transition to LVD"},
	{0x06298000, 0, 1,
	"FFFB: SCSI bus was reset by another initiator"},
	{0x063F0300, 0, 1,
	"3029: A device replacement has occurred"},
	{0x064C8000, 0, 1,
	"9051: IOA cache data exists for a missing or failed device"},
	{0x06670100, 0, 1,
	"9025: Disk unit is not supported at its physical location"},
	{0x06670600, 0, 1,
	"3020: IOA detected a SCSI bus configuration error"},
	{0x06678000, 0, 1,
	"3150: SCSI bus configuration error"},
	{0x06690200, 0, 1,
	"9041: Array protection temporarily suspended"},
	{0x066B0200, 0, 1,
	"9030: Array no longer protected due to missing or failed disk unit"},
	{0x07270000, 0, 0,
	"Failure due to other device"},
	{0x07278000, 0, 1,
	"9008: IOA does not support functions expected by devices"},
	{0x07278100, 0, 1,
	"9010: Cache data associated with attached devices cannot be found"},
	{0x07278200, 0, 1,
	"9011: Cache data belongs to devices other than those attached"},
	{0x07278400, 0, 1,
	"9020: Array missing 2 or more devices with only 1 device present"},
	{0x07278500, 0, 1,
	"9021: Array missing 2 or more devices with 2 or more devices present"},
	{0x07278600, 0, 1,
	"9022: Exposed array is missing a required device"},
	{0x07278700, 0, 1,
	"9023: Array member(s) not at required physical locations"},
	{0x07278800, 0, 1,
	"9024: Array not functional due to present hardware configuration"},
	{0x07278900, 0, 1,
	"9026: Array not functional due to present hardware configuration"},
	{0x07278A00, 0, 1,
	"9027: Array is missing a device and parity is out of sync"},
	{0x07278B00, 0, 1,
	"9028: Maximum number of arrays already exist"},
	{0x07278C00, 0, 1,
	"9050: Required cache data cannot be located for a disk unit"},
	{0x07278D00, 0, 1,
	"9052: Cache data exists for a device that has been modified"},
	{0x07278F00, 0, 1,
	"9054: IOA resources not available due to previous problems"},
	{0x07279100, 0, 1,
	"9092: Disk unit requires initialization before use"},
	{0x07279200, 0, 1,
	"9029: Incorrect hardware configuration change has been detected"},
	{0x07279600, 0, 1,
	"9060: One or more disk pairs are missing from an array"},
	{0x07279700, 0, 1,
	"9061: One or more disks are missing from an array"},
	{0x07279800, 0, 1,
	"9062: One or more disks are missing from an array"},
	{0x07279900, 0, 1,
	"9063: Maximum number of functional arrays has been exceeded"},
	{0x0B260000, 0, 0,
	"Aborted command, invalid descriptor"},
	{0x0B5A0000, 0, 0,
	"Command terminated by host"}
};

static const struct ipr_ses_table_entry ipr_ses_table[] = {
	{ "2104-DL1        ", "XXXXXXXXXXXXXXXX", 80 },
	{ "2104-TL1        ", "XXXXXXXXXXXXXXXX", 80 },
	{ "HSBP07M P U2SCSI", "XXXXXXXXXXXXXXXX", 80 }, /* Hidive 7 slot */
	{ "HSBP05M P U2SCSI", "XXXXXXXXXXXXXXXX", 80 }, /* Hidive 5 slot */
	{ "HSBP05M S U2SCSI", "XXXXXXXXXXXXXXXX", 80 }, /* Bowtie */
	{ "HSBP06E ASU2SCSI", "XXXXXXXXXXXXXXXX", 80 }, /* MartinFenning */
	{ "2104-DU3        ", "XXXXXXXXXXXXXXXX", 160 },
	{ "2104-TU3        ", "XXXXXXXXXXXXXXXX", 160 },
	{ "HSBP04C RSU2SCSI", "XXXXXXX*XXXXXXXX", 160 },
	{ "HSBP06E RSU2SCSI", "XXXXXXX*XXXXXXXX", 160 },
	{ "St  V1S2        ", "XXXXXXXXXXXXXXXX", 160 },
	{ "HSBPD4M  PU3SCSI", "XXXXXXX*XXXXXXXX", 160 },
	{ "VSBPD1H   U3SCSI", "XXXXXXX*XXXXXXXX", 160 }
};

/*
 *  Function Prototypes
 */
static int ipr_reset_alert(struct ipr_cmnd *);
static void ipr_process_ccn(struct ipr_cmnd *);
static void ipr_process_error(struct ipr_cmnd *);
static void ipr_reset_ioa_job(struct ipr_cmnd *);
static void ipr_initiate_ioa_reset(struct ipr_ioa_cfg *,
				   enum ipr_shutdown_type);

#ifdef CONFIG_SCSI_IPR_TRACE
/**
 * ipr_trc_hook - Add a trace entry to the driver trace
 * @ipr_cmd:	ipr command struct
 * @type:		trace type
 * @add_data:	additional data
 *
 * Return value:
 * 	none
 **/
static void ipr_trc_hook(struct ipr_cmnd *ipr_cmd,
			 u8 type, u32 add_data)
{
	struct ipr_trace_entry *trace_entry;
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;

	trace_entry = &ioa_cfg->trace[ioa_cfg->trace_index++];
	trace_entry->time = jiffies;
	trace_entry->op_code = ipr_cmd->ioarcb.cmd_pkt.cdb[0];
	trace_entry->type = type;
	trace_entry->cmd_index = ipr_cmd->cmd_index;
	trace_entry->res_handle = ipr_cmd->ioarcb.res_handle;
	trace_entry->u.add_data = add_data;
}
#else
#define ipr_trc_hook(ipr_cmd, type, add_data) do { } while(0)
#endif

/**
 * ipr_reinit_ipr_cmnd - Re-initialize an IPR Cmnd block for reuse
 * @ipr_cmd:	ipr command struct
 *
 * Return value:
 * 	none
 **/
static void ipr_reinit_ipr_cmnd(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioarcb *ioarcb = &ipr_cmd->ioarcb;
	struct ipr_ioasa *ioasa = &ipr_cmd->ioasa;

	memset(&ioarcb->cmd_pkt, 0, sizeof(struct ipr_cmd_pkt));
	ioarcb->write_data_transfer_length = 0;
	ioarcb->read_data_transfer_length = 0;
	ioarcb->write_ioadl_len = 0;
	ioarcb->read_ioadl_len = 0;
	ioasa->ioasc = 0;
	ioasa->residual_data_len = 0;

	ipr_cmd->scsi_cmd = NULL;
	ipr_cmd->sense_buffer[0] = 0;
	ipr_cmd->dma_use_sg = 0;
}

/**
 * ipr_init_ipr_cmnd - Initialize an IPR Cmnd block
 * @ipr_cmd:	ipr command struct
 *
 * Return value:
 * 	none
 **/
static void ipr_init_ipr_cmnd(struct ipr_cmnd *ipr_cmd)
{
	ipr_reinit_ipr_cmnd(ipr_cmd);
	ipr_cmd->u.scratch = 0;
	ipr_cmd->sibling = NULL;
	init_timer(&ipr_cmd->timer);
}

/**
 * ipr_get_free_ipr_cmnd - Get a free IPR Cmnd block
 * @ioa_cfg:	ioa config struct
 *
 * Return value:
 * 	pointer to ipr command struct
 **/
static
struct ipr_cmnd *ipr_get_free_ipr_cmnd(struct ipr_ioa_cfg *ioa_cfg)
{
	struct ipr_cmnd *ipr_cmd;

	ipr_cmd = list_entry(ioa_cfg->free_q.next, struct ipr_cmnd, queue);
	list_del(&ipr_cmd->queue);
	ipr_init_ipr_cmnd(ipr_cmd);

	return ipr_cmd;
}

/**
 * ipr_unmap_sglist - Unmap scatterlist if mapped
 * @ioa_cfg:	ioa config struct
 * @ipr_cmd:	ipr command struct
 *
 * Return value:
 * 	nothing
 **/
static void ipr_unmap_sglist(struct ipr_ioa_cfg *ioa_cfg,
			     struct ipr_cmnd *ipr_cmd)
{
	struct scsi_cmnd *scsi_cmd = ipr_cmd->scsi_cmd;

	if (ipr_cmd->dma_use_sg) {
		if (scsi_cmd->use_sg > 0) {
			pci_unmap_sg(ioa_cfg->pdev, scsi_cmd->request_buffer,
				     scsi_cmd->use_sg,
				     scsi_cmd->sc_data_direction);
		} else {
			pci_unmap_single(ioa_cfg->pdev, ipr_cmd->dma_handle,
					 scsi_cmd->request_bufflen,
					 scsi_cmd->sc_data_direction);
		}
	}
}

/**
 * ipr_mask_and_clear_interrupts - Mask all and clear specified interrupts
 * @ioa_cfg:	ioa config struct
 * @clr_ints:     interrupts to clear
 *
 * This function masks all interrupts on the adapter, then clears the
 * interrupts specified in the mask
 *
 * Return value:
 * 	none
 **/
static void ipr_mask_and_clear_interrupts(struct ipr_ioa_cfg *ioa_cfg,
					  u32 clr_ints)
{
	volatile u32 int_reg;

	/* Stop new interrupts */
	ioa_cfg->allow_interrupts = 0;

	/* Set interrupt mask to stop all new interrupts */
	writel(~0, ioa_cfg->regs.set_interrupt_mask_reg);

	/* Clear any pending interrupts */
	writel(clr_ints, ioa_cfg->regs.clr_interrupt_reg);
	int_reg = readl(ioa_cfg->regs.sense_interrupt_reg);
}

/**
 * ipr_save_pcix_cmd_reg - Save PCI-X command register
 * @ioa_cfg:	ioa config struct
 *
 * Return value:
 * 	0 on success / -EIO on failure
 **/
static int ipr_save_pcix_cmd_reg(struct ipr_ioa_cfg *ioa_cfg)
{
	int pcix_cmd_reg = pci_find_capability(ioa_cfg->pdev, PCI_CAP_ID_PCIX);

	if (pcix_cmd_reg == 0) {
		dev_err(&ioa_cfg->pdev->dev, "Failed to save PCI-X command register\n");
		return -EIO;
	}

	if (pci_read_config_word(ioa_cfg->pdev, pcix_cmd_reg,
				 &ioa_cfg->saved_pcix_cmd_reg) != PCIBIOS_SUCCESSFUL) {
		dev_err(&ioa_cfg->pdev->dev, "Failed to save PCI-X command register\n");
		return -EIO;
	}

	ioa_cfg->saved_pcix_cmd_reg |= PCI_X_CMD_DPERR_E | PCI_X_CMD_ERO;
	return 0;
}

/**
 * ipr_set_pcix_cmd_reg - Setup PCI-X command register
 * @ioa_cfg:	ioa config struct
 *
 * Return value:
 * 	0 on success / -EIO on failure
 **/
static int ipr_set_pcix_cmd_reg(struct ipr_ioa_cfg *ioa_cfg)
{
	int pcix_cmd_reg = pci_find_capability(ioa_cfg->pdev, PCI_CAP_ID_PCIX);

	if (pcix_cmd_reg) {
		if (pci_write_config_word(ioa_cfg->pdev, pcix_cmd_reg,
					  ioa_cfg->saved_pcix_cmd_reg) != PCIBIOS_SUCCESSFUL) {
			dev_err(&ioa_cfg->pdev->dev, "Failed to setup PCI-X command register\n");
			return -EIO;
		}
	} else {
		dev_err(&ioa_cfg->pdev->dev,
			"Failed to setup PCI-X command register\n");
		return -EIO;
	}

	return 0;
}

/**
 * ipr_scsi_eh_done - mid-layer done function for aborted ops
 * @ipr_cmd:	ipr command struct
 *
 * This function is invoked by the interrupt handler for
 * ops generated by the SCSI mid-layer which are being aborted.
 *
 * Return value:
 * 	none
 **/
static void ipr_scsi_eh_done(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	struct scsi_cmnd *scsi_cmd = ipr_cmd->scsi_cmd;

	scsi_cmd->result |= (DID_ERROR << 16);

	ipr_unmap_sglist(ioa_cfg, ipr_cmd);
	scsi_cmd->scsi_done(scsi_cmd);
	list_add_tail(&ipr_cmd->queue, &ioa_cfg->free_q);
}

/**
 * ipr_fail_all_ops - Fails all outstanding ops.
 * @ioa_cfg:	ioa config struct
 *
 * This function fails all outstanding ops.
 *
 * Return value:
 * 	none
 **/
static void ipr_fail_all_ops(struct ipr_ioa_cfg *ioa_cfg)
{
	struct ipr_cmnd *ipr_cmd, *temp;

	ENTER;
	list_for_each_entry_safe(ipr_cmd, temp, &ioa_cfg->pending_q, queue) {
		list_del(&ipr_cmd->queue);

		ipr_cmd->ioasa.ioasc = cpu_to_be32(IPR_IOASC_IOA_WAS_RESET);
		ipr_cmd->ioasa.ilid = cpu_to_be32(IPR_DRIVER_ILID);

		if (ipr_cmd->scsi_cmd)
			ipr_cmd->done = ipr_scsi_eh_done;

		ipr_trc_hook(ipr_cmd, IPR_TRACE_FINISH, IPR_IOASC_IOA_WAS_RESET);
		del_timer(&ipr_cmd->timer);
		ipr_cmd->done(ipr_cmd);
	}

	LEAVE;
}

/**
 * ipr_do_req -  Send driver initiated requests.
 * @ipr_cmd:		ipr command struct
 * @done:			done function
 * @timeout_func:	timeout function
 * @timeout:		timeout value
 *
 * This function sends the specified command to the adapter with the
 * timeout given. The done function is invoked on command completion.
 *
 * Return value:
 * 	none
 **/
static void ipr_do_req(struct ipr_cmnd *ipr_cmd,
		       void (*done) (struct ipr_cmnd *),
		       void (*timeout_func) (struct ipr_cmnd *), u32 timeout)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;

	list_add_tail(&ipr_cmd->queue, &ioa_cfg->pending_q);

	ipr_cmd->done = done;

	ipr_cmd->timer.data = (unsigned long) ipr_cmd;
	ipr_cmd->timer.expires = jiffies + timeout;
	ipr_cmd->timer.function = (void (*)(unsigned long))timeout_func;

	add_timer(&ipr_cmd->timer);

	ipr_trc_hook(ipr_cmd, IPR_TRACE_START, 0);

	mb();
	writel(be32_to_cpu(ipr_cmd->ioarcb.ioarcb_host_pci_addr),
	       ioa_cfg->regs.ioarrin_reg);
}

/**
 * ipr_internal_cmd_done - Op done function for an internally generated op.
 * @ipr_cmd:	ipr command struct
 *
 * This function is the op done function for an internally generated,
 * blocking op. It simply wakes the sleeping thread.
 *
 * Return value:
 * 	none
 **/
static void ipr_internal_cmd_done(struct ipr_cmnd *ipr_cmd)
{
	if (ipr_cmd->sibling)
		ipr_cmd->sibling = NULL;
	else
		complete(&ipr_cmd->completion);
}

/**
 * ipr_send_blocking_cmd - Send command and sleep on its completion.
 * @ipr_cmd:	ipr command struct
 * @timeout_func:	function to invoke if command times out
 * @timeout:	timeout
 *
 * Return value:
 * 	none
 **/
static void ipr_send_blocking_cmd(struct ipr_cmnd *ipr_cmd,
				  void (*timeout_func) (struct ipr_cmnd *ipr_cmd),
				  u32 timeout)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;

	init_completion(&ipr_cmd->completion);
	ipr_do_req(ipr_cmd, ipr_internal_cmd_done, timeout_func, timeout);

	spin_unlock_irq(ioa_cfg->host->host_lock);
	wait_for_completion(&ipr_cmd->completion);
	spin_lock_irq(ioa_cfg->host->host_lock);
}

/**
 * ipr_send_hcam - Send an HCAM to the adapter.
 * @ioa_cfg:	ioa config struct
 * @type:		HCAM type
 * @hostrcb:	hostrcb struct
 *
 * This function will send a Host Controlled Async command to the adapter.
 * If HCAMs are currently not allowed to be issued to the adapter, it will
 * place the hostrcb on the free queue.
 *
 * Return value:
 * 	none
 **/
static void ipr_send_hcam(struct ipr_ioa_cfg *ioa_cfg, u8 type,
			  struct ipr_hostrcb *hostrcb)
{
	struct ipr_cmnd *ipr_cmd;
	struct ipr_ioarcb *ioarcb;

	if (ioa_cfg->allow_cmds) {
		ipr_cmd = ipr_get_free_ipr_cmnd(ioa_cfg);
		list_add_tail(&ipr_cmd->queue, &ioa_cfg->pending_q);
		list_add_tail(&hostrcb->queue, &ioa_cfg->hostrcb_pending_q);

		ipr_cmd->u.hostrcb = hostrcb;
		ioarcb = &ipr_cmd->ioarcb;

		ioarcb->res_handle = cpu_to_be32(IPR_IOA_RES_HANDLE);
		ioarcb->cmd_pkt.request_type = IPR_RQTYPE_HCAM;
		ioarcb->cmd_pkt.cdb[0] = IPR_HOST_CONTROLLED_ASYNC;
		ioarcb->cmd_pkt.cdb[1] = type;
		ioarcb->cmd_pkt.cdb[7] = (sizeof(hostrcb->hcam) >> 8) & 0xff;
		ioarcb->cmd_pkt.cdb[8] = sizeof(hostrcb->hcam) & 0xff;

		ioarcb->read_data_transfer_length = cpu_to_be32(sizeof(hostrcb->hcam));
		ioarcb->read_ioadl_len = cpu_to_be32(sizeof(struct ipr_ioadl_desc));
		ipr_cmd->ioadl[0].flags_and_data_len =
			cpu_to_be32(IPR_IOADL_FLAGS_READ_LAST | sizeof(hostrcb->hcam));
		ipr_cmd->ioadl[0].address = cpu_to_be32(hostrcb->hostrcb_dma);

		if (type == IPR_HCAM_CDB_OP_CODE_CONFIG_CHANGE)
			ipr_cmd->done = ipr_process_ccn;
		else
			ipr_cmd->done = ipr_process_error;

		ipr_trc_hook(ipr_cmd, IPR_TRACE_START, IPR_IOA_RES_ADDR);

		mb();
		writel(be32_to_cpu(ipr_cmd->ioarcb.ioarcb_host_pci_addr),
		       ioa_cfg->regs.ioarrin_reg);
	} else {
		list_add_tail(&hostrcb->queue, &ioa_cfg->hostrcb_free_q);
	}
}

/**
 * ipr_init_res_entry - Initialize a resource entry struct.
 * @res:	resource entry struct
 *
 * Return value:
 * 	none
 **/
static void ipr_init_res_entry(struct ipr_resource_entry *res)
{
	res->needs_sync_complete = 1;
	res->in_erp = 0;
	res->add_to_ml = 0;
	res->del_from_ml = 0;
	res->resetting_device = 0;
	res->tcq_active = 0;
	res->qdepth = IPR_MAX_CMD_PER_LUN;
	res->sdev = NULL;
}

/**
 * ipr_handle_config_change - Handle a config change from the adapter
 * @ioa_cfg:	ioa config struct
 * @hostrcb:	hostrcb
 *
 * Return value:
 * 	none
 **/
static void ipr_handle_config_change(struct ipr_ioa_cfg *ioa_cfg,
			      struct ipr_hostrcb *hostrcb)
{
	struct ipr_resource_entry *res = NULL;
	struct ipr_config_table_entry *cfgte;
	u32 is_ndn = 1;

	cfgte = &hostrcb->hcam.u.ccn.cfgte;

	list_for_each_entry(res, &ioa_cfg->used_res_q, queue) {
		if (!memcmp(&res->cfgte.res_addr, &cfgte->res_addr,
			    sizeof(cfgte->res_addr))) {
			is_ndn = 0;
			break;
		}
	}

	if (is_ndn) {
		if (list_empty(&ioa_cfg->free_res_q)) {
			ipr_send_hcam(ioa_cfg,
				      IPR_HCAM_CDB_OP_CODE_CONFIG_CHANGE,
				      hostrcb);
			return;
		}

		res = list_entry(ioa_cfg->free_res_q.next,
				 struct ipr_resource_entry, queue);

		list_del(&res->queue);
		ipr_init_res_entry(res);
		list_add_tail(&res->queue, &ioa_cfg->used_res_q);
	}

	memcpy(&res->cfgte, cfgte, sizeof(struct ipr_config_table_entry));

	if (hostrcb->hcam.notify_type == IPR_HOST_RCB_NOTIF_TYPE_REM_ENTRY) {
		if (res->sdev) {
			res->sdev->hostdata = NULL;
			res->del_from_ml = 1;
			if (ioa_cfg->allow_ml_add_del)
				schedule_work(&ioa_cfg->work_q);
		} else
			list_move_tail(&res->queue, &ioa_cfg->free_res_q);
	} else if (!res->sdev) {
		res->add_to_ml = 1;
		if (ioa_cfg->allow_ml_add_del)
			schedule_work(&ioa_cfg->work_q);
	}

	ipr_send_hcam(ioa_cfg, IPR_HCAM_CDB_OP_CODE_CONFIG_CHANGE, hostrcb);
}

/**
 * ipr_process_ccn - Op done function for a CCN.
 * @ipr_cmd:	ipr command struct
 *
 * This function is the op done function for a configuration
 * change notification host controlled async from the adapter.
 *
 * Return value:
 * 	none
 **/
static void ipr_process_ccn(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	struct ipr_hostrcb *hostrcb = ipr_cmd->u.hostrcb;
	u32 ioasc = be32_to_cpu(ipr_cmd->ioasa.ioasc);

	list_del(&hostrcb->queue);
	list_add_tail(&ipr_cmd->queue, &ioa_cfg->free_q);

	if (ioasc) {
		if (ioasc != IPR_IOASC_IOA_WAS_RESET)
			dev_err(&ioa_cfg->pdev->dev,
				"Host RCB failed with IOASC: 0x%08X\n", ioasc);

		ipr_send_hcam(ioa_cfg, IPR_HCAM_CDB_OP_CODE_CONFIG_CHANGE, hostrcb);
	} else {
		ipr_handle_config_change(ioa_cfg, hostrcb);
	}
}

/**
 * ipr_log_vpd - Log the passed VPD to the error log.
 * @vpids:			vendor/product id struct
 * @serial_num:		serial number string
 *
 * Return value:
 * 	none
 **/
static void ipr_log_vpd(struct ipr_std_inq_vpids *vpids, u8 *serial_num)
{
	char buffer[max_t(int, sizeof(struct ipr_std_inq_vpids),
			  IPR_SERIAL_NUM_LEN) + 1];

	memcpy(buffer, vpids, sizeof(struct ipr_std_inq_vpids));
	buffer[sizeof(struct ipr_std_inq_vpids)] = '\0';
	ipr_err("Vendor/Product ID: %s\n", buffer);

	memcpy(buffer, serial_num, IPR_SERIAL_NUM_LEN);
	buffer[IPR_SERIAL_NUM_LEN] = '\0';
	ipr_err("    Serial Number: %s\n", buffer);
}

/**
 * ipr_log_cache_error - Log a cache error.
 * @ioa_cfg:	ioa config struct
 * @hostrcb:	hostrcb struct
 *
 * Return value:
 * 	none
 **/
static void ipr_log_cache_error(struct ipr_ioa_cfg *ioa_cfg,
				struct ipr_hostrcb *hostrcb)
{
	struct ipr_hostrcb_type_02_error *error =
		&hostrcb->hcam.u.error.u.type_02_error;

	ipr_err("-----Current Configuration-----\n");
	ipr_err("Cache Directory Card Information:\n");
	ipr_log_vpd(&error->ioa_vpids, error->ioa_sn);
	ipr_err("Adapter Card Information:\n");
	ipr_log_vpd(&error->cfc_vpids, error->cfc_sn);

	ipr_err("-----Expected Configuration-----\n");
	ipr_err("Cache Directory Card Information:\n");
	ipr_log_vpd(&error->ioa_last_attached_to_cfc_vpids,
		    error->ioa_last_attached_to_cfc_sn);
	ipr_err("Adapter Card Information:\n");
	ipr_log_vpd(&error->cfc_last_attached_to_ioa_vpids,
		    error->cfc_last_attached_to_ioa_sn);

	ipr_err("Additional IOA Data: %08X %08X %08X\n",
		     be32_to_cpu(error->ioa_data[0]),
		     be32_to_cpu(error->ioa_data[1]),
		     be32_to_cpu(error->ioa_data[2]));
}

/**
 * ipr_log_config_error - Log a configuration error.
 * @ioa_cfg:	ioa config struct
 * @hostrcb:	hostrcb struct
 *
 * Return value:
 * 	none
 **/
static void ipr_log_config_error(struct ipr_ioa_cfg *ioa_cfg,
				 struct ipr_hostrcb *hostrcb)
{
	int errors_logged, i;
	struct ipr_hostrcb_device_data_entry *dev_entry;
	struct ipr_hostrcb_type_03_error *error;

	error = &hostrcb->hcam.u.error.u.type_03_error;
	errors_logged = be32_to_cpu(error->errors_logged);

	ipr_err("Device Errors Detected/Logged: %d/%d\n",
		be32_to_cpu(error->errors_detected), errors_logged);

	dev_entry = error->dev_entry;

	for (i = 0; i < errors_logged; i++, dev_entry++) {
		ipr_err_separator;

		if (dev_entry->dev_res_addr.bus >= IPR_MAX_NUM_BUSES) {
			ipr_err("Device %d: missing\n", i + 1);
		} else {
			ipr_err("Device %d: %d:%d:%d:%d\n", i + 1,
				ioa_cfg->host->host_no, dev_entry->dev_res_addr.bus,
				dev_entry->dev_res_addr.target, dev_entry->dev_res_addr.lun);
		}
		ipr_log_vpd(&dev_entry->dev_vpids, dev_entry->dev_sn);

		ipr_err("-----New Device Information-----\n");
		ipr_log_vpd(&dev_entry->new_dev_vpids, dev_entry->new_dev_sn);

		ipr_err("Cache Directory Card Information:\n");
		ipr_log_vpd(&dev_entry->ioa_last_with_dev_vpids,
			    dev_entry->ioa_last_with_dev_sn);

		ipr_err("Adapter Card Information:\n");
		ipr_log_vpd(&dev_entry->cfc_last_with_dev_vpids,
			    dev_entry->cfc_last_with_dev_sn);

		ipr_err("Additional IOA Data: %08X %08X %08X %08X %08X\n",
			be32_to_cpu(dev_entry->ioa_data[0]),
			be32_to_cpu(dev_entry->ioa_data[1]),
			be32_to_cpu(dev_entry->ioa_data[2]),
			be32_to_cpu(dev_entry->ioa_data[3]),
			be32_to_cpu(dev_entry->ioa_data[4]));
	}
}

/**
 * ipr_log_array_error - Log an array configuration error.
 * @ioa_cfg:	ioa config struct
 * @hostrcb:	hostrcb struct
 *
 * Return value:
 * 	none
 **/
static void ipr_log_array_error(struct ipr_ioa_cfg *ioa_cfg,
				struct ipr_hostrcb *hostrcb)
{
	int i;
	struct ipr_hostrcb_type_04_error *error;
	struct ipr_hostrcb_array_data_entry *array_entry;
	u8 zero_sn[IPR_SERIAL_NUM_LEN];

	memset(zero_sn, '0', IPR_SERIAL_NUM_LEN);

	error = &hostrcb->hcam.u.error.u.type_04_error;

	ipr_err_separator;

	ipr_err("RAID %s Array Configuration: %d:%d:%d:%d\n",
		error->protection_level,
		ioa_cfg->host->host_no,
		error->last_func_vset_res_addr.bus,
		error->last_func_vset_res_addr.target,
		error->last_func_vset_res_addr.lun);

	ipr_err_separator;

	array_entry = error->array_member;

	for (i = 0; i < 18; i++) {
		if (!memcmp(array_entry->serial_num, zero_sn, IPR_SERIAL_NUM_LEN))
			continue;

		if (error->exposed_mode_adn == i) {
			ipr_err("Exposed Array Member %d:\n", i);
		} else {
			ipr_err("Array Member %d:\n", i);
		}

		ipr_log_vpd(&array_entry->vpids, array_entry->serial_num);

		if (array_entry->dev_res_addr.bus >= IPR_MAX_NUM_BUSES) {
			ipr_err("Current Location: unknown\n");
		} else {
			ipr_err("Current Location: %d:%d:%d:%d\n",
				ioa_cfg->host->host_no,
				array_entry->dev_res_addr.bus,
				array_entry->dev_res_addr.target,
				array_entry->dev_res_addr.lun);
		}

		if (array_entry->dev_res_addr.bus >= IPR_MAX_NUM_BUSES) {
			ipr_err("Expected Location: unknown\n");
		} else {
			ipr_err("Expected Location: %d:%d:%d:%d\n",
				ioa_cfg->host->host_no,
				array_entry->expected_dev_res_addr.bus,
				array_entry->expected_dev_res_addr.target,
				array_entry->expected_dev_res_addr.lun);
		}

		ipr_err_separator;

		if (i == 9)
			array_entry = error->array_member2;
		else
			array_entry++;
	}
}

/**
 * ipr_log_generic_error - Log an adapter error.
 * @ioa_cfg:	ioa config struct
 * @hostrcb:	hostrcb struct
 *
 * Return value:
 * 	none
 **/
static void ipr_log_generic_error(struct ipr_ioa_cfg *ioa_cfg,
				  struct ipr_hostrcb *hostrcb)
{
	int i;
	int ioa_data_len = be32_to_cpu(hostrcb->hcam.length);

	if (ioa_data_len == 0)
		return;

	ipr_err("IOA Error Data:\n");
	ipr_err("Offset    0 1 2 3  4 5 6 7  8 9 A B  C D E F\n");

	for (i = 0; i < ioa_data_len / 4; i += 4) {
		ipr_err("%08X: %08X %08X %08X %08X\n", i*4,
			be32_to_cpu(hostrcb->hcam.u.raw.data[i]),
			be32_to_cpu(hostrcb->hcam.u.raw.data[i+1]),
			be32_to_cpu(hostrcb->hcam.u.raw.data[i+2]),
			be32_to_cpu(hostrcb->hcam.u.raw.data[i+3]));
	}
}

/**
 * ipr_get_error - Find the specfied IOASC in the ipr_error_table.
 * @ioasc:	IOASC
 *
 * This function will return the index of into the ipr_error_table
 * for the specified IOASC. If the IOASC is not in the table,
 * 0 will be returned, which points to the entry used for unknown errors.
 *
 * Return value:
 * 	index into the ipr_error_table
 **/
static u32 ipr_get_error(u32 ioasc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ipr_error_table); i++)
		if (ipr_error_table[i].ioasc == ioasc)
			return i;

	return 0;
}

/**
 * ipr_handle_log_data - Log an adapter error.
 * @ioa_cfg:	ioa config struct
 * @hostrcb:	hostrcb struct
 *
 * This function logs an adapter error to the system.
 *
 * Return value:
 * 	none
 **/
static void ipr_handle_log_data(struct ipr_ioa_cfg *ioa_cfg,
				struct ipr_hostrcb *hostrcb)
{
	u32 ioasc;
	int error_index;

	if (hostrcb->hcam.notify_type != IPR_HOST_RCB_NOTIF_TYPE_ERROR_LOG_ENTRY)
		return;

	if (hostrcb->hcam.notifications_lost == IPR_HOST_RCB_NOTIFICATIONS_LOST)
		dev_err(&ioa_cfg->pdev->dev, "Error notifications lost\n");

	ioasc = be32_to_cpu(hostrcb->hcam.u.error.failing_dev_ioasc);

	if (ioasc == IPR_IOASC_BUS_WAS_RESET ||
	    ioasc == IPR_IOASC_BUS_WAS_RESET_BY_OTHER) {
		/* Tell the midlayer we had a bus reset so it will handle the UA properly */
		scsi_report_bus_reset(ioa_cfg->host,
				      hostrcb->hcam.u.error.failing_dev_res_addr.bus);
	}

	error_index = ipr_get_error(ioasc);

	if (!ipr_error_table[error_index].log_hcam)
		return;

	if (ipr_is_device(&hostrcb->hcam.u.error.failing_dev_res_addr)) {
		ipr_res_err(ioa_cfg, hostrcb->hcam.u.error.failing_dev_res_addr,
			    "%s\n", ipr_error_table[error_index].error);
	} else {
		dev_err(&ioa_cfg->pdev->dev, "%s\n",
			ipr_error_table[error_index].error);
	}

	/* Set indication we have logged an error */
	ioa_cfg->errors_logged++;

	if (ioa_cfg->log_level < IPR_DEFAULT_LOG_LEVEL)
		return;

	switch (hostrcb->hcam.overlay_id) {
	case IPR_HOST_RCB_OVERLAY_ID_1:
		ipr_log_generic_error(ioa_cfg, hostrcb);
		break;
	case IPR_HOST_RCB_OVERLAY_ID_2:
		ipr_log_cache_error(ioa_cfg, hostrcb);
		break;
	case IPR_HOST_RCB_OVERLAY_ID_3:
		ipr_log_config_error(ioa_cfg, hostrcb);
		break;
	case IPR_HOST_RCB_OVERLAY_ID_4:
	case IPR_HOST_RCB_OVERLAY_ID_6:
		ipr_log_array_error(ioa_cfg, hostrcb);
		break;
	case IPR_HOST_RCB_OVERLAY_ID_DEFAULT:
		ipr_log_generic_error(ioa_cfg, hostrcb);
		break;
	default:
		dev_err(&ioa_cfg->pdev->dev,
			"Unknown error received. Overlay ID: %d\n",
			hostrcb->hcam.overlay_id);
		break;
	}
}

/**
 * ipr_process_error - Op done function for an adapter error log.
 * @ipr_cmd:	ipr command struct
 *
 * This function is the op done function for an error log host
 * controlled async from the adapter. It will log the error and
 * send the HCAM back to the adapter.
 *
 * Return value:
 * 	none
 **/
static void ipr_process_error(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	struct ipr_hostrcb *hostrcb = ipr_cmd->u.hostrcb;
	u32 ioasc = be32_to_cpu(ipr_cmd->ioasa.ioasc);

	list_del(&hostrcb->queue);
	list_add_tail(&ipr_cmd->queue, &ioa_cfg->free_q);

	if (!ioasc) {
		ipr_handle_log_data(ioa_cfg, hostrcb);
	} else if (ioasc != IPR_IOASC_IOA_WAS_RESET) {
		dev_err(&ioa_cfg->pdev->dev,
			"Host RCB failed with IOASC: 0x%08X\n", ioasc);
	}

	ipr_send_hcam(ioa_cfg, IPR_HCAM_CDB_OP_CODE_LOG_DATA, hostrcb);
}

/**
 * ipr_timeout -  An internally generated op has timed out.
 * @ipr_cmd:	ipr command struct
 *
 * This function blocks host requests and initiates an
 * adapter reset.
 *
 * Return value:
 * 	none
 **/
static void ipr_timeout(struct ipr_cmnd *ipr_cmd)
{
	unsigned long lock_flags = 0;
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;

	ENTER;
	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);

	ioa_cfg->errors_logged++;
	dev_err(&ioa_cfg->pdev->dev,
		"Adapter being reset due to command timeout.\n");

	if (WAIT_FOR_DUMP == ioa_cfg->sdt_state)
		ioa_cfg->sdt_state = GET_DUMP;

	if (!ioa_cfg->in_reset_reload || ioa_cfg->reset_cmd == ipr_cmd)
		ipr_initiate_ioa_reset(ioa_cfg, IPR_SHUTDOWN_NONE);

	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
	LEAVE;
}

/**
 * ipr_reset_reload - Reset/Reload the IOA
 * @ioa_cfg:		ioa config struct
 * @shutdown_type:	shutdown type
 *
 * This function resets the adapter and re-initializes it.
 * This function assumes that all new host commands have been stopped.
 * Return value:
 * 	SUCCESS / FAILED
 **/
static int ipr_reset_reload(struct ipr_ioa_cfg *ioa_cfg,
			    enum ipr_shutdown_type shutdown_type)
{
	if (!ioa_cfg->in_reset_reload)
		ipr_initiate_ioa_reset(ioa_cfg, shutdown_type);

	spin_unlock_irq(ioa_cfg->host->host_lock);
	wait_event(ioa_cfg->reset_wait_q, !ioa_cfg->in_reset_reload);
	spin_lock_irq(ioa_cfg->host->host_lock);

	/* If we got hit with a host reset while we were already resetting
	 the adapter for some reason, and the reset failed. */
	if (ioa_cfg->ioa_is_dead) {
		ipr_trace;
		return FAILED;
	}

	return SUCCESS;
}

/**
 * ipr_find_ses_entry - Find matching SES in SES table
 * @res:	resource entry struct of SES
 *
 * Return value:
 * 	pointer to SES table entry / NULL on failure
 **/
static const struct ipr_ses_table_entry *
ipr_find_ses_entry(struct ipr_resource_entry *res)
{
	int i, j, matches;
	const struct ipr_ses_table_entry *ste = ipr_ses_table;

	for (i = 0; i < ARRAY_SIZE(ipr_ses_table); i++, ste++) {
		for (j = 0, matches = 0; j < IPR_PROD_ID_LEN; j++) {
			if (ste->compare_product_id_byte[j] == 'X') {
				if (res->cfgte.std_inq_data.vpids.product_id[j] == ste->product_id[j])
					matches++;
				else
					break;
			} else
				matches++;
		}

		if (matches == IPR_PROD_ID_LEN)
			return ste;
	}

	return NULL;
}

/**
 * ipr_get_max_scsi_speed - Determine max SCSI speed for a given bus
 * @ioa_cfg:	ioa config struct
 * @bus:		SCSI bus
 * @bus_width:	bus width
 *
 * Return value:
 *	SCSI bus speed in units of 100KHz, 1600 is 160 MHz
 *	For a 2-byte wide SCSI bus, the maximum transfer speed is
 *	twice the maximum transfer rate (e.g. for a wide enabled bus,
 *	max 160MHz = max 320MB/sec).
 **/
static u32 ipr_get_max_scsi_speed(struct ipr_ioa_cfg *ioa_cfg, u8 bus, u8 bus_width)
{
	struct ipr_resource_entry *res;
	const struct ipr_ses_table_entry *ste;
	u32 max_xfer_rate = IPR_MAX_SCSI_RATE(bus_width);

	/* Loop through each config table entry in the config table buffer */
	list_for_each_entry(res, &ioa_cfg->used_res_q, queue) {
		if (!(IPR_IS_SES_DEVICE(res->cfgte.std_inq_data)))
			continue;

		if (bus != res->cfgte.res_addr.bus)
			continue;

		if (!(ste = ipr_find_ses_entry(res)))
			continue;

		max_xfer_rate = (ste->max_bus_speed_limit * 10) / (bus_width / 8);
	}

	return max_xfer_rate;
}

/**
 * ipr_wait_iodbg_ack - Wait for an IODEBUG ACK from the IOA
 * @ioa_cfg:		ioa config struct
 * @max_delay:		max delay in micro-seconds to wait
 *
 * Waits for an IODEBUG ACK from the IOA, doing busy looping.
 *
 * Return value:
 * 	0 on success / other on failure
 **/
static int ipr_wait_iodbg_ack(struct ipr_ioa_cfg *ioa_cfg, int max_delay)
{
	volatile u32 pcii_reg;
	int delay = 1;

	/* Read interrupt reg until IOA signals IO Debug Acknowledge */
	while (delay < max_delay) {
		pcii_reg = readl(ioa_cfg->regs.sense_interrupt_reg);

		if (pcii_reg & IPR_PCII_IO_DEBUG_ACKNOWLEDGE)
			return 0;

		/* udelay cannot be used if delay is more than a few milliseconds */
		if ((delay / 1000) > MAX_UDELAY_MS)
			mdelay(delay / 1000);
		else
			udelay(delay);

		delay += delay;
	}
	return -EIO;
}

/**
 * ipr_get_ldump_data_section - Dump IOA memory
 * @ioa_cfg:			ioa config struct
 * @start_addr:			adapter address to dump
 * @dest:				destination kernel buffer
 * @length_in_words:	length to dump in 4 byte words
 *
 * Return value:
 * 	0 on success / -EIO on failure
 **/
static int ipr_get_ldump_data_section(struct ipr_ioa_cfg *ioa_cfg,
				      u32 start_addr,
				      u32 *dest, u32 length_in_words)
{
	volatile u32 temp_pcii_reg;
	int i, delay = 0;

	/* Write IOA interrupt reg starting LDUMP state  */
	writel((IPR_UPROCI_RESET_ALERT | IPR_UPROCI_IO_DEBUG_ALERT),
	       ioa_cfg->regs.set_uproc_interrupt_reg);

	/* Wait for IO debug acknowledge */
	if (ipr_wait_iodbg_ack(ioa_cfg,
			       IPR_LDUMP_MAX_LONG_ACK_DELAY_IN_USEC)) {
		dev_err(&ioa_cfg->pdev->dev,
			"IOA dump long data transfer timeout\n");
		return -EIO;
	}

	/* Signal LDUMP interlocked - clear IO debug ack */
	writel(IPR_PCII_IO_DEBUG_ACKNOWLEDGE,
	       ioa_cfg->regs.clr_interrupt_reg);

	/* Write Mailbox with starting address */
	writel(start_addr, ioa_cfg->ioa_mailbox);

	/* Signal address valid - clear IOA Reset alert */
	writel(IPR_UPROCI_RESET_ALERT,
	       ioa_cfg->regs.clr_uproc_interrupt_reg);

	for (i = 0; i < length_in_words; i++) {
		/* Wait for IO debug acknowledge */
		if (ipr_wait_iodbg_ack(ioa_cfg,
				       IPR_LDUMP_MAX_SHORT_ACK_DELAY_IN_USEC)) {
			dev_err(&ioa_cfg->pdev->dev,
				"IOA dump short data transfer timeout\n");
			return -EIO;
		}

		/* Read data from mailbox and increment destination pointer */
		*dest = cpu_to_be32(readl(ioa_cfg->ioa_mailbox));
		dest++;

		/* For all but the last word of data, signal data received */
		if (i < (length_in_words - 1)) {
			/* Signal dump data received - Clear IO debug Ack */
			writel(IPR_PCII_IO_DEBUG_ACKNOWLEDGE,
			       ioa_cfg->regs.clr_interrupt_reg);
		}
	}

	/* Signal end of block transfer. Set reset alert then clear IO debug ack */
	writel(IPR_UPROCI_RESET_ALERT,
	       ioa_cfg->regs.set_uproc_interrupt_reg);

	writel(IPR_UPROCI_IO_DEBUG_ALERT,
	       ioa_cfg->regs.clr_uproc_interrupt_reg);

	/* Signal dump data received - Clear IO debug Ack */
	writel(IPR_PCII_IO_DEBUG_ACKNOWLEDGE,
	       ioa_cfg->regs.clr_interrupt_reg);

	/* Wait for IOA to signal LDUMP exit - IOA reset alert will be cleared */
	while (delay < IPR_LDUMP_MAX_SHORT_ACK_DELAY_IN_USEC) {
		temp_pcii_reg =
		    readl(ioa_cfg->regs.sense_uproc_interrupt_reg);

		if (!(temp_pcii_reg & IPR_UPROCI_RESET_ALERT))
			return 0;

		udelay(10);
		delay += 10;
	}

	return 0;
}

#ifdef CONFIG_SCSI_IPR_DUMP
/**
 * ipr_sdt_copy - Copy Smart Dump Table to kernel buffer
 * @ioa_cfg:		ioa config struct
 * @pci_address:	adapter address
 * @length:			length of data to copy
 *
 * Copy data from PCI adapter to kernel buffer.
 * Note: length MUST be a 4 byte multiple
 * Return value:
 * 	0 on success / other on failure
 **/
static int ipr_sdt_copy(struct ipr_ioa_cfg *ioa_cfg,
			unsigned long pci_address, u32 length)
{
	int bytes_copied = 0;
	int cur_len, rc, rem_len, rem_page_len;
	u32 *page;
	unsigned long lock_flags = 0;
	struct ipr_ioa_dump *ioa_dump = &ioa_cfg->dump->ioa_dump;

	while (bytes_copied < length &&
	       (ioa_dump->hdr.len + bytes_copied) < IPR_MAX_IOA_DUMP_SIZE) {
		if (ioa_dump->page_offset >= PAGE_SIZE ||
		    ioa_dump->page_offset == 0) {
			page = (u32 *)__get_free_page(GFP_ATOMIC);

			if (!page) {
				ipr_trace;
				return bytes_copied;
			}

			ioa_dump->page_offset = 0;
			ioa_dump->ioa_data[ioa_dump->next_page_index] = page;
			ioa_dump->next_page_index++;
		} else
			page = ioa_dump->ioa_data[ioa_dump->next_page_index - 1];

		rem_len = length - bytes_copied;
		rem_page_len = PAGE_SIZE - ioa_dump->page_offset;
		cur_len = min(rem_len, rem_page_len);

		spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
		if (ioa_cfg->sdt_state == ABORT_DUMP) {
			rc = -EIO;
		} else {
			rc = ipr_get_ldump_data_section(ioa_cfg,
							pci_address + bytes_copied,
							&page[ioa_dump->page_offset / 4],
							(cur_len / sizeof(u32)));
		}
		spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);

		if (!rc) {
			ioa_dump->page_offset += cur_len;
			bytes_copied += cur_len;
		} else {
			ipr_trace;
			break;
		}
		schedule();
	}

	return bytes_copied;
}

/**
 * ipr_init_dump_entry_hdr - Initialize a dump entry header.
 * @hdr:	dump entry header struct
 *
 * Return value:
 * 	nothing
 **/
static void ipr_init_dump_entry_hdr(struct ipr_dump_entry_header *hdr)
{
	hdr->eye_catcher = IPR_DUMP_EYE_CATCHER;
	hdr->num_elems = 1;
	hdr->offset = sizeof(*hdr);
	hdr->status = IPR_DUMP_STATUS_SUCCESS;
}

/**
 * ipr_dump_ioa_type_data - Fill in the adapter type in the dump.
 * @ioa_cfg:	ioa config struct
 * @driver_dump:	driver dump struct
 *
 * Return value:
 * 	nothing
 **/
static void ipr_dump_ioa_type_data(struct ipr_ioa_cfg *ioa_cfg,
				   struct ipr_driver_dump *driver_dump)
{
	struct ipr_inquiry_page3 *ucode_vpd = &ioa_cfg->vpd_cbs->page3_data;

	ipr_init_dump_entry_hdr(&driver_dump->ioa_type_entry.hdr);
	driver_dump->ioa_type_entry.hdr.len =
		sizeof(struct ipr_dump_ioa_type_entry) -
		sizeof(struct ipr_dump_entry_header);
	driver_dump->ioa_type_entry.hdr.data_type = IPR_DUMP_DATA_TYPE_BINARY;
	driver_dump->ioa_type_entry.hdr.id = IPR_DUMP_DRIVER_TYPE_ID;
	driver_dump->ioa_type_entry.type = ioa_cfg->type;
	driver_dump->ioa_type_entry.fw_version = (ucode_vpd->major_release << 24) |
		(ucode_vpd->card_type << 16) | (ucode_vpd->minor_release[0] << 8) |
		ucode_vpd->minor_release[1];
	driver_dump->hdr.num_entries++;
}

/**
 * ipr_dump_version_data - Fill in the driver version in the dump.
 * @ioa_cfg:	ioa config struct
 * @driver_dump:	driver dump struct
 *
 * Return value:
 * 	nothing
 **/
static void ipr_dump_version_data(struct ipr_ioa_cfg *ioa_cfg,
				  struct ipr_driver_dump *driver_dump)
{
	ipr_init_dump_entry_hdr(&driver_dump->version_entry.hdr);
	driver_dump->version_entry.hdr.len =
		sizeof(struct ipr_dump_version_entry) -
		sizeof(struct ipr_dump_entry_header);
	driver_dump->version_entry.hdr.data_type = IPR_DUMP_DATA_TYPE_ASCII;
	driver_dump->version_entry.hdr.id = IPR_DUMP_DRIVER_VERSION_ID;
	strcpy(driver_dump->version_entry.version, IPR_DRIVER_VERSION);
	driver_dump->hdr.num_entries++;
}

/**
 * ipr_dump_trace_data - Fill in the IOA trace in the dump.
 * @ioa_cfg:	ioa config struct
 * @driver_dump:	driver dump struct
 *
 * Return value:
 * 	nothing
 **/
static void ipr_dump_trace_data(struct ipr_ioa_cfg *ioa_cfg,
				   struct ipr_driver_dump *driver_dump)
{
	ipr_init_dump_entry_hdr(&driver_dump->trace_entry.hdr);
	driver_dump->trace_entry.hdr.len =
		sizeof(struct ipr_dump_trace_entry) -
		sizeof(struct ipr_dump_entry_header);
	driver_dump->trace_entry.hdr.data_type = IPR_DUMP_DATA_TYPE_BINARY;
	driver_dump->trace_entry.hdr.id = IPR_DUMP_TRACE_ID;
	memcpy(driver_dump->trace_entry.trace, ioa_cfg->trace, IPR_TRACE_SIZE);
	driver_dump->hdr.num_entries++;
}

/**
 * ipr_dump_location_data - Fill in the IOA location in the dump.
 * @ioa_cfg:	ioa config struct
 * @driver_dump:	driver dump struct
 *
 * Return value:
 * 	nothing
 **/
static void ipr_dump_location_data(struct ipr_ioa_cfg *ioa_cfg,
				   struct ipr_driver_dump *driver_dump)
{
	ipr_init_dump_entry_hdr(&driver_dump->location_entry.hdr);
	driver_dump->location_entry.hdr.len =
		sizeof(struct ipr_dump_location_entry) -
		sizeof(struct ipr_dump_entry_header);
	driver_dump->location_entry.hdr.data_type = IPR_DUMP_DATA_TYPE_ASCII;
	driver_dump->location_entry.hdr.id = IPR_DUMP_LOCATION_ID;
	strcpy(driver_dump->location_entry.location, ioa_cfg->pdev->dev.bus_id);
	driver_dump->hdr.num_entries++;
}

/**
 * ipr_get_ioa_dump - Perform a dump of the driver and adapter.
 * @ioa_cfg:	ioa config struct
 * @dump:		dump struct
 *
 * Return value:
 * 	nothing
 **/
static void ipr_get_ioa_dump(struct ipr_ioa_cfg *ioa_cfg, struct ipr_dump *dump)
{
	unsigned long start_addr, sdt_word;
	unsigned long lock_flags = 0;
	struct ipr_driver_dump *driver_dump = &dump->driver_dump;
	struct ipr_ioa_dump *ioa_dump = &dump->ioa_dump;
	u32 num_entries, start_off, end_off;
	u32 bytes_to_copy, bytes_copied, rc;
	struct ipr_sdt *sdt;
	int i;

	ENTER;

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);

	if (ioa_cfg->sdt_state != GET_DUMP) {
		spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
		return;
	}

	start_addr = readl(ioa_cfg->ioa_mailbox);

	if (!ipr_sdt_is_fmt2(start_addr)) {
		dev_err(&ioa_cfg->pdev->dev,
			"Invalid dump table format: %lx\n", start_addr);
		spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
		return;
	}

	dev_err(&ioa_cfg->pdev->dev, "Dump of IOA initiated\n");

	driver_dump->hdr.eye_catcher = IPR_DUMP_EYE_CATCHER;

	/* Initialize the overall dump header */
	driver_dump->hdr.len = sizeof(struct ipr_driver_dump);
	driver_dump->hdr.num_entries = 1;
	driver_dump->hdr.first_entry_offset = sizeof(struct ipr_dump_header);
	driver_dump->hdr.status = IPR_DUMP_STATUS_SUCCESS;
	driver_dump->hdr.os = IPR_DUMP_OS_LINUX;
	driver_dump->hdr.driver_name = IPR_DUMP_DRIVER_NAME;

	ipr_dump_version_data(ioa_cfg, driver_dump);
	ipr_dump_location_data(ioa_cfg, driver_dump);
	ipr_dump_ioa_type_data(ioa_cfg, driver_dump);
	ipr_dump_trace_data(ioa_cfg, driver_dump);

	/* Update dump_header */
	driver_dump->hdr.len += sizeof(struct ipr_dump_entry_header);

	/* IOA Dump entry */
	ipr_init_dump_entry_hdr(&ioa_dump->hdr);
	ioa_dump->format = IPR_SDT_FMT2;
	ioa_dump->hdr.len = 0;
	ioa_dump->hdr.data_type = IPR_DUMP_DATA_TYPE_BINARY;
	ioa_dump->hdr.id = IPR_DUMP_IOA_DUMP_ID;

	/* First entries in sdt are actually a list of dump addresses and
	 lengths to gather the real dump data.  sdt represents the pointer
	 to the ioa generated dump table.  Dump data will be extracted based
	 on entries in this table */
	sdt = &ioa_dump->sdt;

	rc = ipr_get_ldump_data_section(ioa_cfg, start_addr, (u32 *)sdt,
					sizeof(struct ipr_sdt) / sizeof(u32));

	/* Smart Dump table is ready to use and the first entry is valid */
	if (rc || (be32_to_cpu(sdt->hdr.state) != IPR_FMT2_SDT_READY_TO_USE)) {
		dev_err(&ioa_cfg->pdev->dev,
			"Dump of IOA failed. Dump table not valid: %d, %X.\n",
			rc, be32_to_cpu(sdt->hdr.state));
		driver_dump->hdr.status = IPR_DUMP_STATUS_FAILED;
		ioa_cfg->sdt_state = DUMP_OBTAINED;
		spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
		return;
	}

	num_entries = be32_to_cpu(sdt->hdr.num_entries_used);

	if (num_entries > IPR_NUM_SDT_ENTRIES)
		num_entries = IPR_NUM_SDT_ENTRIES;

	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);

	for (i = 0; i < num_entries; i++) {
		if (ioa_dump->hdr.len > IPR_MAX_IOA_DUMP_SIZE) {
			driver_dump->hdr.status = IPR_DUMP_STATUS_QUAL_SUCCESS;
			break;
		}

		if (sdt->entry[i].flags & IPR_SDT_VALID_ENTRY) {
			sdt_word = be32_to_cpu(sdt->entry[i].bar_str_offset);
			start_off = sdt_word & IPR_FMT2_MBX_ADDR_MASK;
			end_off = be32_to_cpu(sdt->entry[i].end_offset);

			if (ipr_sdt_is_fmt2(sdt_word) && sdt_word) {
				bytes_to_copy = end_off - start_off;
				if (bytes_to_copy > IPR_MAX_IOA_DUMP_SIZE) {
					sdt->entry[i].flags &= ~IPR_SDT_VALID_ENTRY;
					continue;
				}

				/* Copy data from adapter to driver buffers */
				bytes_copied = ipr_sdt_copy(ioa_cfg, sdt_word,
							    bytes_to_copy);

				ioa_dump->hdr.len += bytes_copied;

				if (bytes_copied != bytes_to_copy) {
					driver_dump->hdr.status = IPR_DUMP_STATUS_QUAL_SUCCESS;
					break;
				}
			}
		}
	}

	dev_err(&ioa_cfg->pdev->dev, "Dump of IOA completed.\n");

	/* Update dump_header */
	driver_dump->hdr.len += ioa_dump->hdr.len;
	wmb();
	ioa_cfg->sdt_state = DUMP_OBTAINED;
	LEAVE;
}

#else
#define ipr_get_ioa_dump(ioa_cfg, dump) do { } while(0)
#endif

/**
 * ipr_worker_thread - Worker thread
 * @data:		ioa config struct
 *
 * Called at task level from a work thread. This function takes care
 * of adding and removing device from the mid-layer as configuration
 * changes are detected by the adapter.
 *
 * Return value:
 * 	nothing
 **/
static void ipr_worker_thread(void *data)
{
	unsigned long lock_flags;
	struct ipr_resource_entry *res;
	struct scsi_device *sdev;
	struct ipr_dump *dump;
	struct ipr_ioa_cfg *ioa_cfg = data;
	u8 bus, target, lun;
	int did_work;

	ENTER;
	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);

	if (ioa_cfg->sdt_state == GET_DUMP) {
		dump = ioa_cfg->dump;
		if (!dump || !kobject_get(&dump->kobj)) {
			spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
			return;
		}
		spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
		ipr_get_ioa_dump(ioa_cfg, dump);
		kobject_put(&dump->kobj);

		spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
		if (ioa_cfg->sdt_state == DUMP_OBTAINED)
			ipr_initiate_ioa_reset(ioa_cfg, IPR_SHUTDOWN_NONE);
		spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
		return;
	}

restart:
	do {
		did_work = 0;
		if (!ioa_cfg->allow_cmds || !ioa_cfg->allow_ml_add_del) {
			spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
			return;
		}

		list_for_each_entry(res, &ioa_cfg->used_res_q, queue) {
			if (res->del_from_ml && res->sdev) {
				did_work = 1;
				sdev = res->sdev;
				if (!scsi_device_get(sdev)) {
					res->sdev = NULL;
					list_move_tail(&res->queue, &ioa_cfg->free_res_q);
					spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
					scsi_remove_device(sdev);
					scsi_device_put(sdev);
					spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
				}
				break;
			}
		}
	} while(did_work);

	list_for_each_entry(res, &ioa_cfg->used_res_q, queue) {
		if (res->add_to_ml) {
			bus = res->cfgte.res_addr.bus;
			target = res->cfgte.res_addr.target;
			lun = res->cfgte.res_addr.lun;
			spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
			scsi_add_device(ioa_cfg->host, bus, target, lun);
			spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
			goto restart;
		}
	}

	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
	LEAVE;
}

#ifdef CONFIG_SCSI_IPR_TRACE
/**
 * ipr_read_trace - Dump the adapter trace
 * @kobj:		kobject struct
 * @buf:		buffer
 * @off:		offset
 * @count:		buffer size
 *
 * Return value:
 *	number of bytes printed to buffer
 **/
static ssize_t ipr_read_trace(struct kobject *kobj, char *buf,
			      loff_t off, size_t count)
{
	struct class_device *cdev = container_of(kobj,struct class_device,kobj);
	struct Scsi_Host *shost = class_to_shost(cdev);
	struct ipr_ioa_cfg *ioa_cfg = (struct ipr_ioa_cfg *)shost->hostdata;
	unsigned long lock_flags = 0;
	int size = IPR_TRACE_SIZE;
	char *src = (char *)ioa_cfg->trace;

	if (off > size)
		return 0;
	if (off + count > size) {
		size -= off;
		count = size;
	}

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
	memcpy(buf, &src[off], count);
	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
	return count;
}

static struct bin_attribute ipr_trace_attr = {
	.attr =	{
		.name = "trace",
		.mode = S_IRUGO,
	},
	.size = 0,
	.read = ipr_read_trace,
};
#endif

/**
 * ipr_show_fw_version - Show the firmware version
 * @class_dev:	class device struct
 * @buf:		buffer
 *
 * Return value:
 *	number of bytes printed to buffer
 **/
static ssize_t ipr_show_fw_version(struct class_device *class_dev, char *buf)
{
	struct Scsi_Host *shost = class_to_shost(class_dev);
	struct ipr_ioa_cfg *ioa_cfg = (struct ipr_ioa_cfg *)shost->hostdata;
	struct ipr_inquiry_page3 *ucode_vpd = &ioa_cfg->vpd_cbs->page3_data;
	unsigned long lock_flags = 0;
	int len;

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
	len = snprintf(buf, PAGE_SIZE, "%02X%02X%02X%02X\n",
		       ucode_vpd->major_release, ucode_vpd->card_type,
		       ucode_vpd->minor_release[0],
		       ucode_vpd->minor_release[1]);
	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
	return len;
}

static struct class_device_attribute ipr_fw_version_attr = {
	.attr = {
		.name =		"fw_version",
		.mode =		S_IRUGO,
	},
	.show = ipr_show_fw_version,
};

/**
 * ipr_show_log_level - Show the adapter's error logging level
 * @class_dev:	class device struct
 * @buf:		buffer
 *
 * Return value:
 * 	number of bytes printed to buffer
 **/
static ssize_t ipr_show_log_level(struct class_device *class_dev, char *buf)
{
	struct Scsi_Host *shost = class_to_shost(class_dev);
	struct ipr_ioa_cfg *ioa_cfg = (struct ipr_ioa_cfg *)shost->hostdata;
	unsigned long lock_flags = 0;
	int len;

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
	len = snprintf(buf, PAGE_SIZE, "%d\n", ioa_cfg->log_level);
	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
	return len;
}

/**
 * ipr_store_log_level - Change the adapter's error logging level
 * @class_dev:	class device struct
 * @buf:		buffer
 *
 * Return value:
 * 	number of bytes printed to buffer
 **/
static ssize_t ipr_store_log_level(struct class_device *class_dev,
				   const char *buf, size_t count)
{
	struct Scsi_Host *shost = class_to_shost(class_dev);
	struct ipr_ioa_cfg *ioa_cfg = (struct ipr_ioa_cfg *)shost->hostdata;
	unsigned long lock_flags = 0;

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
	ioa_cfg->log_level = simple_strtoul(buf, NULL, 10);
	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
	return strlen(buf);
}

static struct class_device_attribute ipr_log_level_attr = {
	.attr = {
		.name =		"log_level",
		.mode =		S_IRUGO | S_IWUSR,
	},
	.show = ipr_show_log_level,
	.store = ipr_store_log_level
};

/**
 * ipr_store_diagnostics - IOA Diagnostics interface
 * @class_dev:	class_device struct
 * @buf:		buffer
 * @count:		buffer size
 *
 * This function will reset the adapter and wait a reasonable
 * amount of time for any errors that the adapter might log.
 *
 * Return value:
 * 	count on success / other on failure
 **/
static ssize_t ipr_store_diagnostics(struct class_device *class_dev,
				     const char *buf, size_t count)
{
	struct Scsi_Host *shost = class_to_shost(class_dev);
	struct ipr_ioa_cfg *ioa_cfg = (struct ipr_ioa_cfg *)shost->hostdata;
	unsigned long lock_flags = 0;
	int rc = count;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	wait_event(ioa_cfg->reset_wait_q, !ioa_cfg->in_reset_reload);
	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
	ioa_cfg->errors_logged = 0;
	ipr_initiate_ioa_reset(ioa_cfg, IPR_SHUTDOWN_NORMAL);

	if (ioa_cfg->in_reset_reload) {
		spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
		wait_event(ioa_cfg->reset_wait_q, !ioa_cfg->in_reset_reload);

		/* Wait for a second for any errors to be logged */
		schedule_timeout(HZ);
	} else {
		spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
		return -EIO;
	}

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
	if (ioa_cfg->in_reset_reload || ioa_cfg->errors_logged)
		rc = -EIO;
	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);

	return rc;
}

static struct class_device_attribute ipr_diagnostics_attr = {
	.attr = {
		.name =		"run_diagnostics",
		.mode =		S_IWUSR,
	},
	.store = ipr_store_diagnostics
};

/**
 * ipr_store_reset_adapter - Reset the adapter
 * @class_dev:	class_device struct
 * @buf:		buffer
 * @count:		buffer size
 *
 * This function will reset the adapter.
 *
 * Return value:
 * 	count on success / other on failure
 **/
static ssize_t ipr_store_reset_adapter(struct class_device *class_dev,
				       const char *buf, size_t count)
{
	struct Scsi_Host *shost = class_to_shost(class_dev);
	struct ipr_ioa_cfg *ioa_cfg = (struct ipr_ioa_cfg *)shost->hostdata;
	unsigned long lock_flags;
	int result = count;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
	if (!ioa_cfg->in_reset_reload)
		ipr_initiate_ioa_reset(ioa_cfg, IPR_SHUTDOWN_NORMAL);
	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
	wait_event(ioa_cfg->reset_wait_q, !ioa_cfg->in_reset_reload);

	return result;
}

static struct class_device_attribute ipr_ioa_reset_attr = {
	.attr = {
		.name =		"reset_host",
		.mode =		S_IWUSR,
	},
	.store = ipr_store_reset_adapter
};

/**
 * ipr_alloc_ucode_buffer - Allocates a microcode download buffer
 * @buf_len:		buffer length
 *
 * Allocates a DMA'able buffer in chunks and assembles a scatter/gather
 * list to use for microcode download
 *
 * Return value:
 * 	pointer to sglist / NULL on failure
 **/
static struct ipr_sglist *ipr_alloc_ucode_buffer(int buf_len)
{
	int sg_size, order, bsize_elem, num_elem, i, j;
	struct ipr_sglist *sglist;
	struct scatterlist *scatterlist;
	struct page *page;

	/* Get the minimum size per scatter/gather element */
	sg_size = buf_len / (IPR_MAX_SGLIST - 1);

	/* Get the actual size per element */
	order = get_order(sg_size);

	/* Determine the actual number of bytes per element */
	bsize_elem = PAGE_SIZE * (1 << order);

	/* Determine the actual number of sg entries needed */
	if (buf_len % bsize_elem)
		num_elem = (buf_len / bsize_elem) + 1;
	else
		num_elem = buf_len / bsize_elem;

	/* Allocate a scatter/gather list for the DMA */
	sglist = kmalloc(sizeof(struct ipr_sglist) +
			 (sizeof(struct scatterlist) * (num_elem - 1)),
			 GFP_KERNEL);

	if (sglist == NULL) {
		ipr_trace;
		return NULL;
	}

	memset(sglist, 0, sizeof(struct ipr_sglist) +
	       (sizeof(struct scatterlist) * (num_elem - 1)));

	scatterlist = sglist->scatterlist;

	sglist->order = order;
	sglist->num_sg = num_elem;

	/* Allocate a bunch of sg elements */
	for (i = 0; i < num_elem; i++) {
		page = alloc_pages(GFP_KERNEL, order);
		if (!page) {
			ipr_trace;

			/* Free up what we already allocated */
			for (j = i - 1; j >= 0; j--)
				__free_pages(scatterlist[j].page, order);
			kfree(sglist);
			return NULL;
		}

		scatterlist[i].page = page;
	}

	return sglist;
}

/**
 * ipr_free_ucode_buffer - Frees a microcode download buffer
 * @p_dnld:		scatter/gather list pointer
 *
 * Free a DMA'able ucode download buffer previously allocated with
 * ipr_alloc_ucode_buffer
 *
 * Return value:
 * 	nothing
 **/
static void ipr_free_ucode_buffer(struct ipr_sglist *sglist)
{
	int i;

	for (i = 0; i < sglist->num_sg; i++)
		__free_pages(sglist->scatterlist[i].page, sglist->order);

	kfree(sglist);
}

/**
 * ipr_copy_ucode_buffer - Copy user buffer to kernel buffer
 * @sglist:		scatter/gather list pointer
 * @buffer:		buffer pointer
 * @len:		buffer length
 *
 * Copy a microcode image from a user buffer into a buffer allocated by
 * ipr_alloc_ucode_buffer
 *
 * Return value:
 * 	0 on success / other on failure
 **/
static int ipr_copy_ucode_buffer(struct ipr_sglist *sglist,
				 u8 *buffer, u32 len)
{
	int bsize_elem, i, result = 0;
	struct scatterlist *scatterlist;
	void *kaddr;

	/* Determine the actual number of bytes per element */
	bsize_elem = PAGE_SIZE * (1 << sglist->order);

	scatterlist = sglist->scatterlist;

	for (i = 0; i < (len / bsize_elem); i++, buffer += bsize_elem) {
		kaddr = kmap(scatterlist[i].page);
		memcpy(kaddr, buffer, bsize_elem);
		kunmap(scatterlist[i].page);

		scatterlist[i].length = bsize_elem;

		if (result != 0) {
			ipr_trace;
			return result;
		}
	}

	if (len % bsize_elem) {
		kaddr = kmap(scatterlist[i].page);
		memcpy(kaddr, buffer, len % bsize_elem);
		kunmap(scatterlist[i].page);

		scatterlist[i].length = len % bsize_elem;
	}

	sglist->buffer_len = len;
	return result;
}

/**
 * ipr_map_ucode_buffer - Map a microcode download buffer
 * @ipr_cmd:	ipr command struct
 * @sglist:		scatter/gather list
 * @len:		total length of download buffer
 *
 * Maps a microcode download scatter/gather list for DMA and
 * builds the IOADL.
 *
 * Return value:
 * 	0 on success / -EIO on failure
 **/
static int ipr_map_ucode_buffer(struct ipr_cmnd *ipr_cmd,
				struct ipr_sglist *sglist, int len)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	struct ipr_ioarcb *ioarcb = &ipr_cmd->ioarcb;
	struct ipr_ioadl_desc *ioadl = ipr_cmd->ioadl;
	struct scatterlist *scatterlist = sglist->scatterlist;
	int i;

	ipr_cmd->dma_use_sg = pci_map_sg(ioa_cfg->pdev, scatterlist,
					 sglist->num_sg, DMA_TO_DEVICE);

	ioarcb->cmd_pkt.flags_hi |= IPR_FLAGS_HI_WRITE_NOT_READ;
	ioarcb->write_data_transfer_length = cpu_to_be32(len);
	ioarcb->write_ioadl_len =
		cpu_to_be32(sizeof(struct ipr_ioadl_desc) * ipr_cmd->dma_use_sg);

	for (i = 0; i < ipr_cmd->dma_use_sg; i++) {
		ioadl[i].flags_and_data_len =
			cpu_to_be32(IPR_IOADL_FLAGS_WRITE | sg_dma_len(&scatterlist[i]));
		ioadl[i].address =
			cpu_to_be32(sg_dma_address(&scatterlist[i]));
	}

	if (likely(ipr_cmd->dma_use_sg)) {
		ioadl[i-1].flags_and_data_len |=
			cpu_to_be32(IPR_IOADL_FLAGS_LAST);
	}
	else {
		dev_err(&ioa_cfg->pdev->dev, "pci_map_sg failed!\n");
		return -EIO;
	}

	return 0;
}

/**
 * ipr_store_update_fw - Update the firmware on the adapter
 * @class_dev:	class_device struct
 * @buf:		buffer
 * @count:		buffer size
 *
 * This function will update the firmware on the adapter.
 *
 * Return value:
 * 	count on success / other on failure
 **/
static ssize_t ipr_store_update_fw(struct class_device *class_dev,
				       const char *buf, size_t count)
{
	struct Scsi_Host *shost = class_to_shost(class_dev);
	struct ipr_ioa_cfg *ioa_cfg = (struct ipr_ioa_cfg *)shost->hostdata;
	struct ipr_ucode_image_header *image_hdr;
	const struct firmware *fw_entry;
	struct ipr_sglist *sglist;
	unsigned long lock_flags;
	char fname[100];
	char *src;
	int len, result, dnld_size;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	len = snprintf(fname, 99, "%s", buf);
	fname[len-1] = '\0';

	if(request_firmware(&fw_entry, fname, &ioa_cfg->pdev->dev)) {
		dev_err(&ioa_cfg->pdev->dev, "Firmware file %s not found\n", fname);
		return -EIO;
	}

	image_hdr = (struct ipr_ucode_image_header *)fw_entry->data;

	if (be32_to_cpu(image_hdr->header_length) > fw_entry->size ||
	    (ioa_cfg->vpd_cbs->page3_data.card_type &&
	     ioa_cfg->vpd_cbs->page3_data.card_type != image_hdr->card_type)) {
		dev_err(&ioa_cfg->pdev->dev, "Invalid microcode buffer\n");
		release_firmware(fw_entry);
		return -EINVAL;
	}

	src = (u8 *)image_hdr + be32_to_cpu(image_hdr->header_length);
	dnld_size = fw_entry->size - be32_to_cpu(image_hdr->header_length);
	sglist = ipr_alloc_ucode_buffer(dnld_size);

	if (!sglist) {
		dev_err(&ioa_cfg->pdev->dev, "Microcode buffer allocation failed\n");
		release_firmware(fw_entry);
		return -ENOMEM;
	}

	result = ipr_copy_ucode_buffer(sglist, src, dnld_size);

	if (result) {
		dev_err(&ioa_cfg->pdev->dev,
			"Microcode buffer copy to DMA buffer failed\n");
		ipr_free_ucode_buffer(sglist);
		release_firmware(fw_entry);
		return result;
	}

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);

	if (ioa_cfg->ucode_sglist) {
		spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
		dev_err(&ioa_cfg->pdev->dev,
			"Microcode download already in progress\n");
		ipr_free_ucode_buffer(sglist);
		release_firmware(fw_entry);
		return -EIO;
	}

	ioa_cfg->ucode_sglist = sglist;
	ipr_initiate_ioa_reset(ioa_cfg, IPR_SHUTDOWN_NORMAL);
	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
	wait_event(ioa_cfg->reset_wait_q, !ioa_cfg->in_reset_reload);

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
	ioa_cfg->ucode_sglist = NULL;
	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);

	ipr_free_ucode_buffer(sglist);
	release_firmware(fw_entry);

	return count;
}

static struct class_device_attribute ipr_update_fw_attr = {
	.attr = {
		.name =		"update_fw",
		.mode =		S_IWUSR,
	},
	.store = ipr_store_update_fw
};

static struct class_device_attribute *ipr_ioa_attrs[] = {
	&ipr_fw_version_attr,
	&ipr_log_level_attr,
	&ipr_diagnostics_attr,
	&ipr_ioa_reset_attr,
	&ipr_update_fw_attr,
	NULL,
};

#ifdef CONFIG_SCSI_IPR_DUMP
/**
 * ipr_read_dump - Dump the adapter
 * @kobj:		kobject struct
 * @buf:		buffer
 * @off:		offset
 * @count:		buffer size
 *
 * Return value:
 *	number of bytes printed to buffer
 **/
static ssize_t ipr_read_dump(struct kobject *kobj, char *buf,
			      loff_t off, size_t count)
{
	struct class_device *cdev = container_of(kobj,struct class_device,kobj);
	struct Scsi_Host *shost = class_to_shost(cdev);
	struct ipr_ioa_cfg *ioa_cfg = (struct ipr_ioa_cfg *)shost->hostdata;
	struct ipr_dump *dump;
	unsigned long lock_flags = 0;
	char *src;
	int len;
	size_t rc = count;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
	dump = ioa_cfg->dump;

	if (ioa_cfg->sdt_state != DUMP_OBTAINED || !dump || !kobject_get(&dump->kobj)) {
		spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
		return 0;
	}

	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);

	if (off > dump->driver_dump.hdr.len) {
		kobject_put(&dump->kobj);
		return 0;
	}

	if (off + count > dump->driver_dump.hdr.len) {
		count = dump->driver_dump.hdr.len - off;
		rc = count;
	}

	if (count && off < sizeof(dump->driver_dump)) {
		if (off + count > sizeof(dump->driver_dump))
			len = sizeof(dump->driver_dump) - off;
		else
			len = count;
		src = (u8 *)&dump->driver_dump + off;
		memcpy(buf, src, len);
		buf += len;
		off += len;
		count -= len;
	}

	off -= sizeof(dump->driver_dump);

	if (count && off < offsetof(struct ipr_ioa_dump, ioa_data)) {
		if (off + count > offsetof(struct ipr_ioa_dump, ioa_data))
			len = offsetof(struct ipr_ioa_dump, ioa_data) - off;
		else
			len = count;
		src = (u8 *)&dump->ioa_dump + off;
		memcpy(buf, src, len);
		buf += len;
		off += len;
		count -= len;
	}

	off -= offsetof(struct ipr_ioa_dump, ioa_data);

	while (count) {
		if ((off & PAGE_MASK) != ((off + count) & PAGE_MASK))
			len = PAGE_ALIGN(off) - off;
		else
			len = count;
		src = (u8 *)dump->ioa_dump.ioa_data[(off & PAGE_MASK) >> PAGE_SHIFT];
		src += off & ~PAGE_MASK;
		memcpy(buf, src, len);
		buf += len;
		off += len;
		count -= len;
	}

	kobject_put(&dump->kobj);
	return rc;
}

/**
 * ipr_release_dump - Free adapter dump memory
 * @kobj:	kobject struct
 *
 * Return value:
 *	nothing
 **/
static void ipr_release_dump(struct kobject *kobj)
{
	struct ipr_dump *dump = container_of(kobj,struct ipr_dump,kobj);
	struct ipr_ioa_cfg *ioa_cfg = dump->ioa_cfg;
	unsigned long lock_flags = 0;
	int i;

	ENTER;
	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
	ioa_cfg->dump = NULL;
	ioa_cfg->sdt_state = INACTIVE;
	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);

	for (i = 0; i < dump->ioa_dump.next_page_index; i++)
		free_page((unsigned long) dump->ioa_dump.ioa_data[i]);

	kfree(dump);
	LEAVE;
}

static struct kobj_type ipr_dump_kobj_type = {
	.release = ipr_release_dump,
};

/**
 * ipr_alloc_dump - Prepare for adapter dump
 * @ioa_cfg:	ioa config struct
 *
 * Return value:
 *	0 on success / other on failure
 **/
static int ipr_alloc_dump(struct ipr_ioa_cfg *ioa_cfg)
{
	struct ipr_dump *dump;
	unsigned long lock_flags = 0;

	ENTER;
	dump = kmalloc(sizeof(struct ipr_dump), GFP_KERNEL);

	if (!dump) {
		ipr_err("Dump memory allocation failed\n");
		return -ENOMEM;
	}

	memset(dump, 0, sizeof(struct ipr_dump));
	kobject_init(&dump->kobj);
	dump->kobj.ktype = &ipr_dump_kobj_type;
	dump->ioa_cfg = ioa_cfg;

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);

	if (INACTIVE != ioa_cfg->sdt_state) {
		spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
		kfree(dump);
		return 0;
	}

	ioa_cfg->dump = dump;
	ioa_cfg->sdt_state = WAIT_FOR_DUMP;
	if (ioa_cfg->ioa_is_dead && !ioa_cfg->dump_taken) {
		ioa_cfg->dump_taken = 1;
		schedule_work(&ioa_cfg->work_q);
	}
	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);

	LEAVE;
	return 0;
}

/**
 * ipr_free_dump - Free adapter dump memory
 * @ioa_cfg:	ioa config struct
 *
 * Return value:
 *	0 on success / other on failure
 **/
static int ipr_free_dump(struct ipr_ioa_cfg *ioa_cfg)
{
	struct ipr_dump *dump;
	unsigned long lock_flags = 0;

	ENTER;

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
	dump = ioa_cfg->dump;
	if (!dump) {
		spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
		return 0;
	}

	ioa_cfg->dump = NULL;
	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);

	kobject_put(&dump->kobj);

	LEAVE;
	return 0;
}

/**
 * ipr_write_dump - Setup dump state of adapter
 * @kobj:		kobject struct
 * @buf:		buffer
 * @off:		offset
 * @count:		buffer size
 *
 * Return value:
 *	number of bytes printed to buffer
 **/
static ssize_t ipr_write_dump(struct kobject *kobj, char *buf,
			      loff_t off, size_t count)
{
	struct class_device *cdev = container_of(kobj,struct class_device,kobj);
	struct Scsi_Host *shost = class_to_shost(cdev);
	struct ipr_ioa_cfg *ioa_cfg = (struct ipr_ioa_cfg *)shost->hostdata;
	int rc;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	if (buf[0] == '1')
		rc = ipr_alloc_dump(ioa_cfg);
	else if (buf[0] == '0')
		rc = ipr_free_dump(ioa_cfg);
	else
		return -EINVAL;

	if (rc)
		return rc;
	else
		return count;
}

static struct bin_attribute ipr_dump_attr = {
	.attr =	{
		.name = "dump",
		.mode = S_IRUSR | S_IWUSR,
	},
	.size = 0,
	.read = ipr_read_dump,
	.write = ipr_write_dump
};
#else
static int ipr_free_dump(struct ipr_ioa_cfg *ioa_cfg) { return 0; };
#endif

/**
 * ipr_store_queue_depth - Change the device's queue depth
 * @dev:	device struct
 * @buf:	buffer
 *
 * Return value:
 * 	number of bytes printed to buffer
 **/
static ssize_t ipr_store_queue_depth(struct device *dev,
				    const char *buf, size_t count)
{
	struct scsi_device *sdev = to_scsi_device(dev);
	struct ipr_ioa_cfg *ioa_cfg = (struct ipr_ioa_cfg *)sdev->host->hostdata;
	struct ipr_resource_entry *res;
	int qdepth = simple_strtoul(buf, NULL, 10);
	int tagged = 0;
	unsigned long lock_flags = 0;
	ssize_t len = -ENXIO;

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
	res = (struct ipr_resource_entry *)sdev->hostdata;
	if (res) {
		res->qdepth = qdepth;

		if (ipr_is_gscsi(res) && res->tcq_active)
			tagged = MSG_ORDERED_TAG;

		len = strlen(buf);
	}

	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
	scsi_adjust_queue_depth(sdev, tagged, qdepth);
	return len;
}

static struct device_attribute ipr_queue_depth_attr = {
	.attr = {
		.name = 	"queue_depth",
		.mode =		S_IRUSR | S_IWUSR,
	},
	.store = ipr_store_queue_depth
};

/**
 * ipr_show_tcq_enable - Show if the device is enabled for tcqing
 * @dev:	device struct
 * @buf:	buffer
 *
 * Return value:
 * 	number of bytes printed to buffer
 **/
static ssize_t ipr_show_tcq_enable(struct device *dev, char *buf)
{
	struct scsi_device *sdev = to_scsi_device(dev);
	struct ipr_ioa_cfg *ioa_cfg = (struct ipr_ioa_cfg *)sdev->host->hostdata;
	struct ipr_resource_entry *res;
	unsigned long lock_flags = 0;
	ssize_t len = -ENXIO;

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
	res = (struct ipr_resource_entry *)sdev->hostdata;
	if (res)
		len = snprintf(buf, PAGE_SIZE, "%d\n", res->tcq_active);
	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
	return len;
}

/**
 * ipr_store_tcq_enable - Change the device's TCQing state
 * @dev:	device struct
 * @buf:	buffer
 *
 * Return value:
 * 	number of bytes printed to buffer
 **/
static ssize_t ipr_store_tcq_enable(struct device *dev,
				    const char *buf, size_t count)
{
	struct scsi_device *sdev = to_scsi_device(dev);
	struct ipr_ioa_cfg *ioa_cfg = (struct ipr_ioa_cfg *)sdev->host->hostdata;
	struct ipr_resource_entry *res;
	unsigned long lock_flags = 0;
	int tcq_active = simple_strtoul(buf, NULL, 10);
	int qdepth = IPR_MAX_CMD_PER_LUN;
	int tagged = 0;
	ssize_t len = -ENXIO;

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);

	res = (struct ipr_resource_entry *)sdev->hostdata;

	if (res) {
		res->tcq_active = 0;
		qdepth = res->qdepth;

		if (ipr_is_gscsi(res) && sdev->tagged_supported) {
			if (tcq_active) {
				tagged = MSG_ORDERED_TAG;
				res->tcq_active = 1;
			}

			len = strlen(buf);
		} else if (tcq_active) {
			len = -EINVAL;
		}
	}

	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
	scsi_adjust_queue_depth(sdev, tagged, qdepth);
	return len;
}

static struct device_attribute ipr_tcqing_attr = {
	.attr = {
		.name = 	"tcq_enable",
		.mode =		S_IRUSR | S_IWUSR,
	},
	.store = ipr_store_tcq_enable,
	.show = ipr_show_tcq_enable
};

/**
 * ipr_show_adapter_handle - Show the adapter's resource handle for this device
 * @dev:	device struct
 * @buf:	buffer
 *
 * Return value:
 * 	number of bytes printed to buffer
 **/
static ssize_t ipr_show_adapter_handle(struct device *dev, char *buf)
{
	struct scsi_device *sdev = to_scsi_device(dev);
	struct ipr_ioa_cfg *ioa_cfg = (struct ipr_ioa_cfg *)sdev->host->hostdata;
	struct ipr_resource_entry *res;
	unsigned long lock_flags = 0;
	ssize_t len = -ENXIO;

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
	res = (struct ipr_resource_entry *)sdev->hostdata;
	if (res)
		len = snprintf(buf, PAGE_SIZE, "%08X\n", res->cfgte.res_handle);
	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
	return len;
}

static struct device_attribute ipr_adapter_handle_attr = {
	.attr = {
		.name = 	"adapter_handle",
		.mode =		S_IRUSR,
	},
	.show = ipr_show_adapter_handle
};

static struct device_attribute *ipr_dev_attrs[] = {
	&ipr_queue_depth_attr,
	&ipr_tcqing_attr,
	&ipr_adapter_handle_attr,
	NULL,
};

/**
 * ipr_biosparam - Return the HSC mapping
 * @sdev:			scsi device struct
 * @block_device:	block device pointer
 * @capacity:		capacity of the device
 * @parm:			Array containing returned HSC values.
 *
 * This function generates the HSC parms that fdisk uses.
 * We want to make sure we return something that places partitions
 * on 4k boundaries for best performance with the IOA.
 *
 * Return value:
 * 	0 on success
 **/
static int ipr_biosparam(struct scsi_device *sdev,
			 struct block_device *block_device,
			 sector_t capacity, int *parm)
{
	int heads, sectors, cylinders;

	heads = 128;
	sectors = 32;

	cylinders = capacity;
	sector_div(cylinders, (128 * 32));

	/* return result */
	parm[0] = heads;
	parm[1] = sectors;
	parm[2] = cylinders;

	return 0;
}

/**
 * ipr_slave_destroy - Unconfigure a SCSI device
 * @sdev:	scsi device struct
 *
 * Return value:
 * 	nothing
 **/
static void ipr_slave_destroy(struct scsi_device *sdev)
{
	struct ipr_resource_entry *res;
	struct ipr_ioa_cfg *ioa_cfg;
	unsigned long lock_flags = 0;

	ioa_cfg = (struct ipr_ioa_cfg *) sdev->host->hostdata;

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
	res = (struct ipr_resource_entry *) sdev->hostdata;
	if (res) {
		sdev->hostdata = NULL;
		res->sdev = NULL;
	}
	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
}

/**
 * ipr_slave_configure - Configure a SCSI device
 * @sdev:	scsi device struct
 *
 * This function configures the specified scsi device.
 *
 * Return value:
 * 	0 on success
 **/
static int ipr_slave_configure(struct scsi_device *sdev)
{
	struct ipr_ioa_cfg *ioa_cfg = (struct ipr_ioa_cfg *) sdev->host->hostdata;
	struct ipr_resource_entry *res;
	unsigned long lock_flags = 0;

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
	res = sdev->hostdata;
	if (res) {
		if (ipr_is_af_dasd_device(res))
			sdev->type = TYPE_RAID;
		if (ipr_is_af_dasd_device(res) || ipr_is_ioa_resource(res))
			sdev->scsi_level = 4;
		if (ipr_is_vset_device(res))
			sdev->timeout = IPR_VSET_RW_TIMEOUT;

		sdev->allow_restart = 1;
		scsi_adjust_queue_depth(sdev, 0, res->qdepth);
	}
	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
	return 0;
}

/**
 * ipr_slave_alloc - Prepare for commands to a device.
 * @sdev:	scsi device struct
 *
 * This function saves a pointer to the resource entry
 * in the scsi device struct if the device exists. We
 * can then use this pointer in ipr_queuecommand when
 * handling new commands.
 *
 * Return value:
 * 	0 on success
 **/
static int ipr_slave_alloc(struct scsi_device *sdev)
{
	struct ipr_ioa_cfg *ioa_cfg = (struct ipr_ioa_cfg *) sdev->host->hostdata;
	struct ipr_resource_entry *res;
	unsigned long lock_flags;

	sdev->hostdata = NULL;

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);

	list_for_each_entry(res, &ioa_cfg->used_res_q, queue) {
		if ((res->cfgte.res_addr.bus == sdev->channel) &&
		    (res->cfgte.res_addr.target == sdev->id) &&
		    (res->cfgte.res_addr.lun == sdev->lun)) {
			res->sdev = sdev;
			res->add_to_ml = 0;
			res->in_erp = 0;
			sdev->hostdata = res;
			res->needs_sync_complete = 1;
			break;
		}
	}

	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);

	return 0;
}

/**
 * ipr_eh_host_reset - Reset the host adapter
 * @scsi_cmd:	scsi command struct
 *
 * Return value:
 * 	SUCCESS / FAILED
 **/
static int ipr_eh_host_reset(struct scsi_cmnd * scsi_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg;
	int rc;

	ENTER;
	ioa_cfg = (struct ipr_ioa_cfg *) scsi_cmd->device->host->hostdata;

	dev_err(&ioa_cfg->pdev->dev,
		"Adapter being reset as a result of error recovery.\n");

	if (WAIT_FOR_DUMP == ioa_cfg->sdt_state)
		ioa_cfg->sdt_state = GET_DUMP;

	rc = ipr_reset_reload(ioa_cfg, IPR_SHUTDOWN_ABBREV);

	LEAVE;
	return rc;
}

/**
 * ipr_eh_dev_reset - Reset the device
 * @scsi_cmd:	scsi command struct
 *
 * This function issues a device reset to the affected device.
 * A LUN reset will be sent to the device first. If that does
 * not work, a target reset will be sent.
 *
 * Return value:
 *	SUCCESS / FAILED
 **/
static int ipr_eh_dev_reset(struct scsi_cmnd * scsi_cmd)
{
	struct ipr_cmnd *ipr_cmd;
	struct ipr_ioa_cfg *ioa_cfg;
	struct ipr_resource_entry *res;
	struct ipr_cmd_pkt *cmd_pkt;
	u32 ioasc;

	ENTER;
	ioa_cfg = (struct ipr_ioa_cfg *) scsi_cmd->device->host->hostdata;
	res = scsi_cmd->device->hostdata;

	if (!res || (!ipr_is_gscsi(res) && !ipr_is_vset_device(res)))
		return FAILED;

	/*
	 * If we are currently going through reset/reload, return failed. This will force the
	 * mid-layer to call ipr_eh_host_reset, which will then go to sleep and wait for the
	 * reset to complete
	 */
	if (ioa_cfg->in_reset_reload)
		return FAILED;
	if (ioa_cfg->ioa_is_dead)
		return FAILED;

	list_for_each_entry(ipr_cmd, &ioa_cfg->pending_q, queue) {
		if (ipr_cmd->ioarcb.res_handle == res->cfgte.res_handle) {
			if (ipr_cmd->scsi_cmd)
				ipr_cmd->done = ipr_scsi_eh_done;
		}
	}

	res->resetting_device = 1;

	ipr_cmd = ipr_get_free_ipr_cmnd(ioa_cfg);

	ipr_cmd->ioarcb.res_handle = res->cfgte.res_handle;
	cmd_pkt = &ipr_cmd->ioarcb.cmd_pkt;
	cmd_pkt->request_type = IPR_RQTYPE_IOACMD;
	cmd_pkt->cdb[0] = IPR_RESET_DEVICE;

	ipr_sdev_err(scsi_cmd->device, "Resetting device\n");
	ipr_send_blocking_cmd(ipr_cmd, ipr_timeout, IPR_DEVICE_RESET_TIMEOUT);

	ioasc = be32_to_cpu(ipr_cmd->ioasa.ioasc);

	res->resetting_device = 0;

	list_add_tail(&ipr_cmd->queue, &ioa_cfg->free_q);

	LEAVE;
	return (IPR_IOASC_SENSE_KEY(ioasc) ? FAILED : SUCCESS);
}

/**
 * ipr_bus_reset_done - Op done function for bus reset.
 * @ipr_cmd:	ipr command struct
 *
 * This function is the op done function for a bus reset
 *
 * Return value:
 * 	none
 **/
static void ipr_bus_reset_done(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	struct ipr_resource_entry *res;

	ENTER;
	list_for_each_entry(res, &ioa_cfg->used_res_q, queue) {
		if (!memcmp(&res->cfgte.res_handle, &ipr_cmd->ioarcb.res_handle,
			    sizeof(res->cfgte.res_handle))) {
			scsi_report_bus_reset(ioa_cfg->host, res->cfgte.res_addr.bus);
			break;
		}
	}

	/*
	 * If abort has not completed, indicate the reset has, else call the
	 * abort's done function to wake the sleeping eh thread
	 */
	if (ipr_cmd->sibling->sibling)
		ipr_cmd->sibling->sibling = NULL;
	else
		ipr_cmd->sibling->done(ipr_cmd->sibling);

	list_add_tail(&ipr_cmd->queue, &ioa_cfg->free_q);
	LEAVE;
}

/**
 * ipr_abort_timeout - An abort task has timed out
 * @ipr_cmd:	ipr command struct
 *
 * This function handles when an abort task times out. If this
 * happens we issue a bus reset since we have resources tied
 * up that must be freed before returning to the midlayer.
 *
 * Return value:
 *	none
 **/
static void ipr_abort_timeout(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_cmnd *reset_cmd;
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	struct ipr_cmd_pkt *cmd_pkt;
	unsigned long lock_flags = 0;

	ENTER;
	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
	if (ipr_cmd->completion.done || ioa_cfg->in_reset_reload) {
		spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
		return;
	}

	ipr_sdev_err(ipr_cmd->u.sdev, "Abort timed out. Resetting bus\n");
	reset_cmd = ipr_get_free_ipr_cmnd(ioa_cfg);
	ipr_cmd->sibling = reset_cmd;
	reset_cmd->sibling = ipr_cmd;
	reset_cmd->ioarcb.res_handle = ipr_cmd->ioarcb.res_handle;
	cmd_pkt = &reset_cmd->ioarcb.cmd_pkt;
	cmd_pkt->request_type = IPR_RQTYPE_IOACMD;
	cmd_pkt->cdb[0] = IPR_RESET_DEVICE;
	cmd_pkt->cdb[2] = IPR_RESET_TYPE_SELECT | IPR_BUS_RESET;

	ipr_do_req(reset_cmd, ipr_bus_reset_done, ipr_timeout, IPR_DEVICE_RESET_TIMEOUT);
	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
	LEAVE;
}

/**
 * ipr_cancel_op - Cancel specified op
 * @scsi_cmd:	scsi command struct
 *
 * This function cancels specified op.
 *
 * Return value:
 *	SUCCESS / FAILED
 **/
static int ipr_cancel_op(struct scsi_cmnd * scsi_cmd)
{
	struct ipr_cmnd *ipr_cmd;
	struct ipr_ioa_cfg *ioa_cfg;
	struct ipr_resource_entry *res;
	struct ipr_cmd_pkt *cmd_pkt;
	u32 ioasc, ioarcb_addr;
	int op_found = 0;

	ENTER;
	ioa_cfg = (struct ipr_ioa_cfg *)scsi_cmd->device->host->hostdata;
	res = scsi_cmd->device->hostdata;

	if (!res || (!ipr_is_gscsi(res) && !ipr_is_vset_device(res)))
		return FAILED;

	list_for_each_entry(ipr_cmd, &ioa_cfg->pending_q, queue) {
		if (ipr_cmd->scsi_cmd == scsi_cmd) {
			ipr_cmd->done = ipr_scsi_eh_done;
			op_found = 1;
			break;
		}
	}

	if (!op_found)
		return SUCCESS;

	ioarcb_addr = be32_to_cpu(ipr_cmd->ioarcb.ioarcb_host_pci_addr);

	ipr_cmd = ipr_get_free_ipr_cmnd(ioa_cfg);
	ipr_cmd->ioarcb.res_handle = res->cfgte.res_handle;
	cmd_pkt = &ipr_cmd->ioarcb.cmd_pkt;
	cmd_pkt->request_type = IPR_RQTYPE_IOACMD;
	cmd_pkt->cdb[0] = IPR_ABORT_TASK;
	cmd_pkt->cdb[2] = (ioarcb_addr >> 24) & 0xff;
	cmd_pkt->cdb[3] = (ioarcb_addr >> 16) & 0xff;
	cmd_pkt->cdb[4] = (ioarcb_addr >> 8) & 0xff;
	cmd_pkt->cdb[5] = ioarcb_addr & 0xff;
	ipr_cmd->u.sdev = scsi_cmd->device;

	ipr_sdev_err(scsi_cmd->device, "Aborting command: %02X\n", scsi_cmd->cmnd[0]);
	ipr_send_blocking_cmd(ipr_cmd, ipr_abort_timeout, IPR_ABORT_TASK_TIMEOUT);
	ioasc = be32_to_cpu(ipr_cmd->ioasa.ioasc);

	/*
	 * If the abort task timed out and we sent a bus reset, we will get
	 * one the following responses to the abort
	 */
	if (ioasc == IPR_IOASC_BUS_WAS_RESET || ioasc == IPR_IOASC_SYNC_REQUIRED) {
		ioasc = 0;
		ipr_trace;
	}

	list_add_tail(&ipr_cmd->queue, &ioa_cfg->free_q);
	res->needs_sync_complete = 1;

	LEAVE;
	return (IPR_IOASC_SENSE_KEY(ioasc) ? FAILED : SUCCESS);
}

/**
 * ipr_eh_abort - Abort a single op
 * @scsi_cmd:	scsi command struct
 *
 * Return value:
 * 	SUCCESS / FAILED
 **/
static int ipr_eh_abort(struct scsi_cmnd * scsi_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg;

	ENTER;
	ioa_cfg = (struct ipr_ioa_cfg *) scsi_cmd->device->host->hostdata;

	/* If we are currently going through reset/reload, return failed. This will force the
	   mid-layer to call ipr_eh_host_reset, which will then go to sleep and wait for the
	   reset to complete */
	if (ioa_cfg->in_reset_reload)
		return FAILED;
	if (ioa_cfg->ioa_is_dead)
		return FAILED;
	if (!scsi_cmd->device->hostdata)
		return FAILED;

	LEAVE;
	return ipr_cancel_op(scsi_cmd);
}

/**
 * ipr_handle_other_interrupt - Handle "other" interrupts
 * @ioa_cfg:	ioa config struct
 * @int_reg:	interrupt register
 *
 * Return value:
 * 	IRQ_NONE / IRQ_HANDLED
 **/
static irqreturn_t ipr_handle_other_interrupt(struct ipr_ioa_cfg *ioa_cfg,
					      volatile u32 int_reg)
{
	irqreturn_t rc = IRQ_HANDLED;

	if (int_reg & IPR_PCII_IOA_TRANS_TO_OPER) {
		/* Mask the interrupt */
		writel(IPR_PCII_IOA_TRANS_TO_OPER, ioa_cfg->regs.set_interrupt_mask_reg);

		/* Clear the interrupt */
		writel(IPR_PCII_IOA_TRANS_TO_OPER, ioa_cfg->regs.clr_interrupt_reg);
		int_reg = readl(ioa_cfg->regs.sense_interrupt_reg);

		list_del(&ioa_cfg->reset_cmd->queue);
		del_timer(&ioa_cfg->reset_cmd->timer);
		ipr_reset_ioa_job(ioa_cfg->reset_cmd);
	} else {
		if (int_reg & IPR_PCII_IOA_UNIT_CHECKED)
			ioa_cfg->ioa_unit_checked = 1;
		else
			dev_err(&ioa_cfg->pdev->dev,
				"Permanent IOA failure. 0x%08X\n", int_reg);

		if (WAIT_FOR_DUMP == ioa_cfg->sdt_state)
			ioa_cfg->sdt_state = GET_DUMP;

		ipr_mask_and_clear_interrupts(ioa_cfg, ~0);
		ipr_initiate_ioa_reset(ioa_cfg, IPR_SHUTDOWN_NONE);
	}

	return rc;
}

/**
 * ipr_isr - Interrupt service routine
 * @irq:	irq number
 * @devp:	pointer to ioa config struct
 * @regs:	pt_regs struct
 *
 * Return value:
 * 	IRQ_NONE / IRQ_HANDLED
 **/
static irqreturn_t ipr_isr(int irq, void *devp, struct pt_regs *regs)
{
	struct ipr_ioa_cfg *ioa_cfg = (struct ipr_ioa_cfg *)devp;
	unsigned long lock_flags = 0;
	volatile u32 int_reg, int_mask_reg;
	u32 ioasc;
	u16 cmd_index;
	struct ipr_cmnd *ipr_cmd;
	irqreturn_t rc = IRQ_NONE;

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);

	/* If interrupts are disabled, ignore the interrupt */
	if (!ioa_cfg->allow_interrupts) {
		spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
		return IRQ_NONE;
	}

	int_mask_reg = readl(ioa_cfg->regs.sense_interrupt_mask_reg);
	int_reg = readl(ioa_cfg->regs.sense_interrupt_reg) & ~int_mask_reg;

	/* If an interrupt on the adapter did not occur, ignore it */
	if (unlikely((int_reg & IPR_PCII_OPER_INTERRUPTS) == 0)) {
		spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
		return IRQ_NONE;
	}

	while (1) {
		ipr_cmd = NULL;

		while ((be32_to_cpu(*ioa_cfg->hrrq_curr) & IPR_HRRQ_TOGGLE_BIT) ==
		       ioa_cfg->toggle_bit) {

			cmd_index = (be32_to_cpu(*ioa_cfg->hrrq_curr) &
				     IPR_HRRQ_REQ_RESP_HANDLE_MASK) >> IPR_HRRQ_REQ_RESP_HANDLE_SHIFT;

			if (unlikely(cmd_index >= IPR_NUM_CMD_BLKS)) {
				ioa_cfg->errors_logged++;
				dev_err(&ioa_cfg->pdev->dev, "Invalid response handle from IOA\n");

				if (WAIT_FOR_DUMP == ioa_cfg->sdt_state)
					ioa_cfg->sdt_state = GET_DUMP;

				ipr_initiate_ioa_reset(ioa_cfg, IPR_SHUTDOWN_NONE);
				spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
				return IRQ_HANDLED;
			}

			ipr_cmd = ioa_cfg->ipr_cmnd_list[cmd_index];

			ioasc = be32_to_cpu(ipr_cmd->ioasa.ioasc);

			ipr_trc_hook(ipr_cmd, IPR_TRACE_FINISH, ioasc);

			list_del(&ipr_cmd->queue);
			del_timer(&ipr_cmd->timer);
			ipr_cmd->done(ipr_cmd);

			rc = IRQ_HANDLED;

			if (ioa_cfg->hrrq_curr < ioa_cfg->hrrq_end) {
				ioa_cfg->hrrq_curr++;
			} else {
				ioa_cfg->hrrq_curr = ioa_cfg->hrrq_start;
				ioa_cfg->toggle_bit ^= 1u;
			}
		}

		if (ipr_cmd != NULL) {
			/* Clear the PCI interrupt */
			writel(IPR_PCII_HRRQ_UPDATED, ioa_cfg->regs.clr_interrupt_reg);
			int_reg = readl(ioa_cfg->regs.sense_interrupt_reg) & ~int_mask_reg;
		} else
			break;
	}

	if (unlikely(rc == IRQ_NONE))
		rc = ipr_handle_other_interrupt(ioa_cfg, int_reg);

	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
	return rc;
}

/**
 * ipr_build_ioadl - Build a scatter/gather list and map the buffer
 * @ioa_cfg:	ioa config struct
 * @ipr_cmd:	ipr command struct
 *
 * Return value:
 * 	0 on success / -1 on failure
 **/
static int ipr_build_ioadl(struct ipr_ioa_cfg *ioa_cfg,
			   struct ipr_cmnd *ipr_cmd)
{
	int i;
	struct scatterlist *sglist;
	u32 length;
	u32 ioadl_flags = 0;
	struct scsi_cmnd *scsi_cmd = ipr_cmd->scsi_cmd;
	struct ipr_ioarcb *ioarcb = &ipr_cmd->ioarcb;
	struct ipr_ioadl_desc *ioadl = ipr_cmd->ioadl;

	length = scsi_cmd->request_bufflen;

	if (length == 0)
		return 0;

	if (scsi_cmd->use_sg) {
		ipr_cmd->dma_use_sg = pci_map_sg(ioa_cfg->pdev,
						 scsi_cmd->request_buffer,
						 scsi_cmd->use_sg,
						 scsi_cmd->sc_data_direction);

		if (scsi_cmd->sc_data_direction == DMA_TO_DEVICE) {
			ioadl_flags = IPR_IOADL_FLAGS_WRITE;
			ioarcb->cmd_pkt.flags_hi |= IPR_FLAGS_HI_WRITE_NOT_READ;
			ioarcb->write_data_transfer_length = cpu_to_be32(length);
			ioarcb->write_ioadl_len =
				cpu_to_be32(sizeof(struct ipr_ioadl_desc) * ipr_cmd->dma_use_sg);
		} else if (scsi_cmd->sc_data_direction == DMA_FROM_DEVICE) {
			ioadl_flags = IPR_IOADL_FLAGS_READ;
			ioarcb->read_data_transfer_length = cpu_to_be32(length);
			ioarcb->read_ioadl_len =
				cpu_to_be32(sizeof(struct ipr_ioadl_desc) * ipr_cmd->dma_use_sg);
		}

		sglist = scsi_cmd->request_buffer;

		for (i = 0; i < ipr_cmd->dma_use_sg; i++) {
			ioadl[i].flags_and_data_len =
				cpu_to_be32(ioadl_flags | sg_dma_len(&sglist[i]));
			ioadl[i].address =
				cpu_to_be32(sg_dma_address(&sglist[i]));
		}

		if (likely(ipr_cmd->dma_use_sg)) {
			ioadl[i-1].flags_and_data_len |=
				cpu_to_be32(IPR_IOADL_FLAGS_LAST);
			return 0;
		} else
			dev_err(&ioa_cfg->pdev->dev, "pci_map_sg failed!\n");
	} else {
		if (scsi_cmd->sc_data_direction == DMA_TO_DEVICE) {
			ioadl_flags = IPR_IOADL_FLAGS_WRITE;
			ioarcb->cmd_pkt.flags_hi |= IPR_FLAGS_HI_WRITE_NOT_READ;
			ioarcb->write_data_transfer_length = cpu_to_be32(length);
			ioarcb->write_ioadl_len = cpu_to_be32(sizeof(struct ipr_ioadl_desc));
		} else if (scsi_cmd->sc_data_direction == DMA_FROM_DEVICE) {
			ioadl_flags = IPR_IOADL_FLAGS_READ;
			ioarcb->read_data_transfer_length = cpu_to_be32(length);
			ioarcb->read_ioadl_len = cpu_to_be32(sizeof(struct ipr_ioadl_desc));
		}

		ipr_cmd->dma_handle = pci_map_single(ioa_cfg->pdev,
						     scsi_cmd->request_buffer, length,
						     scsi_cmd->sc_data_direction);

		if (likely(!pci_dma_mapping_error(ipr_cmd->dma_handle))) {
			ipr_cmd->dma_use_sg = 1;
			ioadl[0].flags_and_data_len =
				cpu_to_be32(ioadl_flags | length | IPR_IOADL_FLAGS_LAST);
			ioadl[0].address = cpu_to_be32(ipr_cmd->dma_handle);
			return 0;
		} else
			dev_err(&ioa_cfg->pdev->dev, "pci_map_single failed!\n");
	}

	return -1;
}

/**
 * ipr_get_task_attributes - Translate SPI Q-Tag to task attributes
 * @scsi_cmd:	scsi command struct
 *
 * Return value:
 * 	task attributes
 **/
static u8 ipr_get_task_attributes(struct scsi_cmnd *scsi_cmd)
{
	u8 tag[2];
	u8 rc = IPR_FLAGS_LO_UNTAGGED_TASK;

	if (scsi_populate_tag_msg(scsi_cmd, tag)) {
		switch (tag[0]) {
		case MSG_SIMPLE_TAG:
			rc = IPR_FLAGS_LO_SIMPLE_TASK;
			break;
		case MSG_HEAD_TAG:
			rc = IPR_FLAGS_LO_HEAD_OF_Q_TASK;
			break;
		case MSG_ORDERED_TAG:
			rc = IPR_FLAGS_LO_ORDERED_TASK;
			break;
		};
	}

	return rc;
}

/**
 * ipr_erp_done - Process completion of ERP for a device
 * @ipr_cmd:		ipr command struct
 *
 * This function copies the sense buffer into the scsi_cmd
 * struct and pushes the scsi_done function.
 *
 * Return value:
 * 	nothing
 **/
static void ipr_erp_done(struct ipr_cmnd *ipr_cmd)
{
	struct scsi_cmnd *scsi_cmd = ipr_cmd->scsi_cmd;
	struct ipr_resource_entry *res = scsi_cmd->device->hostdata;
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	u32 ioasc = be32_to_cpu(ipr_cmd->ioasa.ioasc);

	if (IPR_IOASC_SENSE_KEY(ioasc) > 0) {
		scsi_cmd->result |= (DID_ERROR << 16);
		ipr_sdev_err(scsi_cmd->device,
			     "Request Sense failed with IOASC: 0x%08X\n", ioasc);
	} else {
		memcpy(scsi_cmd->sense_buffer, ipr_cmd->sense_buffer,
		       SCSI_SENSE_BUFFERSIZE);
	}

	if (res) {
		res->needs_sync_complete = 1;
		res->in_erp = 0;
	}
	ipr_unmap_sglist(ioa_cfg, ipr_cmd);
	list_add_tail(&ipr_cmd->queue, &ioa_cfg->free_q);
	scsi_cmd->scsi_done(scsi_cmd);
}

/**
 * ipr_reinit_ipr_cmnd_for_erp - Re-initialize a cmnd block to be used for ERP
 * @ipr_cmd:	ipr command struct
 *
 * Return value:
 * 	none
 **/
static void ipr_reinit_ipr_cmnd_for_erp(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioarcb *ioarcb;
	struct ipr_ioasa *ioasa;

	ioarcb = &ipr_cmd->ioarcb;
	ioasa = &ipr_cmd->ioasa;

	memset(&ioarcb->cmd_pkt, 0, sizeof(struct ipr_cmd_pkt));
	ioarcb->write_data_transfer_length = 0;
	ioarcb->read_data_transfer_length = 0;
	ioarcb->write_ioadl_len = 0;
	ioarcb->read_ioadl_len = 0;
	ioasa->ioasc = 0;
	ioasa->residual_data_len = 0;
}

/**
 * ipr_erp_request_sense - Send request sense to a device
 * @ipr_cmd:	ipr command struct
 *
 * This function sends a request sense to a device as a result
 * of a check condition.
 *
 * Return value:
 * 	nothing
 **/
static void ipr_erp_request_sense(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_cmd_pkt *cmd_pkt = &ipr_cmd->ioarcb.cmd_pkt;
	u32 ioasc = be32_to_cpu(ipr_cmd->ioasa.ioasc);

	if (IPR_IOASC_SENSE_KEY(ioasc) > 0) {
		ipr_erp_done(ipr_cmd);
		return;
	}

	ipr_reinit_ipr_cmnd_for_erp(ipr_cmd);

	cmd_pkt->request_type = IPR_RQTYPE_SCSICDB;
	cmd_pkt->cdb[0] = REQUEST_SENSE;
	cmd_pkt->cdb[4] = SCSI_SENSE_BUFFERSIZE;
	cmd_pkt->flags_hi |= IPR_FLAGS_HI_SYNC_OVERRIDE;
	cmd_pkt->flags_hi |= IPR_FLAGS_HI_NO_ULEN_CHK;
	cmd_pkt->timeout = cpu_to_be16(IPR_REQUEST_SENSE_TIMEOUT / HZ);

	ipr_cmd->ioadl[0].flags_and_data_len =
		cpu_to_be32(IPR_IOADL_FLAGS_READ_LAST | SCSI_SENSE_BUFFERSIZE);
	ipr_cmd->ioadl[0].address =
		cpu_to_be32(ipr_cmd->sense_buffer_dma);

	ipr_cmd->ioarcb.read_ioadl_len =
		cpu_to_be32(sizeof(struct ipr_ioadl_desc));
	ipr_cmd->ioarcb.read_data_transfer_length =
		cpu_to_be32(SCSI_SENSE_BUFFERSIZE);

	ipr_do_req(ipr_cmd, ipr_erp_done, ipr_timeout,
		   IPR_REQUEST_SENSE_TIMEOUT * 2);
}

/**
 * ipr_erp_cancel_all - Send cancel all to a device
 * @ipr_cmd:	ipr command struct
 *
 * This function sends a cancel all to a device to clear the
 * queue. If we are running TCQ on the device, QERR is set to 1,
 * which means all outstanding ops have been dropped on the floor.
 * Cancel all will return them to us.
 *
 * Return value:
 * 	nothing
 **/
static void ipr_erp_cancel_all(struct ipr_cmnd *ipr_cmd)
{
	struct scsi_cmnd *scsi_cmd = ipr_cmd->scsi_cmd;
	struct ipr_resource_entry *res = scsi_cmd->device->hostdata;
	struct ipr_cmd_pkt *cmd_pkt;

	res->in_erp = 1;

	ipr_reinit_ipr_cmnd_for_erp(ipr_cmd);

	if (!res->tcq_active) {
		ipr_erp_request_sense(ipr_cmd);
		return;
	}

	cmd_pkt = &ipr_cmd->ioarcb.cmd_pkt;
	cmd_pkt->request_type = IPR_RQTYPE_IOACMD;
	cmd_pkt->cdb[0] = IPR_CANCEL_ALL_REQUESTS;

	ipr_do_req(ipr_cmd, ipr_erp_request_sense, ipr_timeout,
		   IPR_CANCEL_ALL_TIMEOUT);
}

/**
 * ipr_dump_ioasa - Dump contents of IOASA
 * @ioa_cfg:	ioa config struct
 * @ipr_cmd:	ipr command struct
 *
 * This function is invoked by the interrupt handler when ops
 * fail. It will log the IOASA if appropriate. Only called
 * for GPDD ops.
 *
 * Return value:
 * 	none
 **/
static void ipr_dump_ioasa(struct ipr_ioa_cfg *ioa_cfg,
			   struct ipr_cmnd *ipr_cmd)
{
	int i;
	u16 data_len;
	u32 ioasc;
	struct ipr_ioasa *ioasa = &ipr_cmd->ioasa;
	u32 *ioasa_data = (u32 *)ioasa;
	int error_index;

	ioasc = be32_to_cpu(ioasa->ioasc) & IPR_IOASC_IOASC_MASK;

	if (0 == ioasc)
		return;

	if (ioa_cfg->log_level < IPR_DEFAULT_LOG_LEVEL)
		return;

	error_index = ipr_get_error(ioasc);

	if (ioa_cfg->log_level < IPR_MAX_LOG_LEVEL) {
		/* Don't log an error if the IOA already logged one */
		if (ioasa->ilid != 0)
			return;

		if (ipr_error_table[error_index].log_ioasa == 0)
			return;
	}

	ipr_sdev_err(ipr_cmd->scsi_cmd->device, "%s\n",
		     ipr_error_table[error_index].error);

	if ((ioasa->u.gpdd.end_state <= ARRAY_SIZE(ipr_gpdd_dev_end_states)) &&
	    (ioasa->u.gpdd.bus_phase <=  ARRAY_SIZE(ipr_gpdd_dev_bus_phases))) {
		ipr_sdev_err(ipr_cmd->scsi_cmd->device,
			     "Device End state: %s Phase: %s\n",
			     ipr_gpdd_dev_end_states[ioasa->u.gpdd.end_state],
			     ipr_gpdd_dev_bus_phases[ioasa->u.gpdd.bus_phase]);
	}

	if (sizeof(struct ipr_ioasa) < be16_to_cpu(ioasa->ret_stat_len))
		data_len = sizeof(struct ipr_ioasa);
	else
		data_len = be16_to_cpu(ioasa->ret_stat_len);

	ipr_err("IOASA Dump:\n");

	for (i = 0; i < data_len / 4; i += 4) {
		ipr_err("%08X: %08X %08X %08X %08X\n", i*4,
			be32_to_cpu(ioasa_data[i]),
			be32_to_cpu(ioasa_data[i+1]),
			be32_to_cpu(ioasa_data[i+2]),
			be32_to_cpu(ioasa_data[i+3]));
	}
}

/**
 * ipr_gen_sense - Generate SCSI sense data from an IOASA
 * @ioasa:		IOASA
 * @sense_buf:	sense data buffer
 *
 * Return value:
 * 	none
 **/
static void ipr_gen_sense(struct ipr_cmnd *ipr_cmd)
{
	u32 failing_lba;
	u8 *sense_buf = ipr_cmd->scsi_cmd->sense_buffer;
	struct ipr_resource_entry *res = ipr_cmd->scsi_cmd->device->hostdata;
	struct ipr_ioasa *ioasa = &ipr_cmd->ioasa;
	u32 ioasc = be32_to_cpu(ioasa->ioasc);

	memset(sense_buf, 0, SCSI_SENSE_BUFFERSIZE);

	if (ioasc >= IPR_FIRST_DRIVER_IOASC)
		return;

	ipr_cmd->scsi_cmd->result = SAM_STAT_CHECK_CONDITION;

	if (ipr_is_vset_device(res) &&
	    ioasc == IPR_IOASC_MED_DO_NOT_REALLOC &&
	    ioasa->u.vset.failing_lba_hi != 0) {
		sense_buf[0] = 0x72;
		sense_buf[1] = IPR_IOASC_SENSE_KEY(ioasc);
		sense_buf[2] = IPR_IOASC_SENSE_CODE(ioasc);
		sense_buf[3] = IPR_IOASC_SENSE_QUAL(ioasc);

		sense_buf[7] = 12;
		sense_buf[8] = 0;
		sense_buf[9] = 0x0A;
		sense_buf[10] = 0x80;

		failing_lba = be32_to_cpu(ioasa->u.vset.failing_lba_hi);

		sense_buf[12] = (failing_lba & 0xff000000) >> 24;
		sense_buf[13] = (failing_lba & 0x00ff0000) >> 16;
		sense_buf[14] = (failing_lba & 0x0000ff00) >> 8;
		sense_buf[15] = failing_lba & 0x000000ff;

		failing_lba = be32_to_cpu(ioasa->u.vset.failing_lba_lo);

		sense_buf[16] = (failing_lba & 0xff000000) >> 24;
		sense_buf[17] = (failing_lba & 0x00ff0000) >> 16;
		sense_buf[18] = (failing_lba & 0x0000ff00) >> 8;
		sense_buf[19] = failing_lba & 0x000000ff;
	} else {
		sense_buf[0] = 0x70;
		sense_buf[2] = IPR_IOASC_SENSE_KEY(ioasc);
		sense_buf[12] = IPR_IOASC_SENSE_CODE(ioasc);
		sense_buf[13] = IPR_IOASC_SENSE_QUAL(ioasc);

		/* Illegal request */
		if ((IPR_IOASC_SENSE_KEY(ioasc) == 0x05) &&
		    (be32_to_cpu(ioasa->ioasc_specific) & IPR_FIELD_POINTER_VALID)) {
			sense_buf[7] = 10;	/* additional length */

			/* IOARCB was in error */
			if (IPR_IOASC_SENSE_CODE(ioasc) == 0x24)
				sense_buf[15] = 0xC0;
			else	/* Parameter data was invalid */
				sense_buf[15] = 0x80;

			sense_buf[16] =
			    ((IPR_FIELD_POINTER_MASK &
			      be32_to_cpu(ioasa->ioasc_specific)) >> 8) & 0xff;
			sense_buf[17] =
			    (IPR_FIELD_POINTER_MASK &
			     be32_to_cpu(ioasa->ioasc_specific)) & 0xff;
		} else {
			if (ioasc == IPR_IOASC_MED_DO_NOT_REALLOC) {
				if (ipr_is_vset_device(res))
					failing_lba = be32_to_cpu(ioasa->u.vset.failing_lba_lo);
				else
					failing_lba = be32_to_cpu(ioasa->u.dasd.failing_lba);

				sense_buf[0] |= 0x80;	/* Or in the Valid bit */
				sense_buf[3] = (failing_lba & 0xff000000) >> 24;
				sense_buf[4] = (failing_lba & 0x00ff0000) >> 16;
				sense_buf[5] = (failing_lba & 0x0000ff00) >> 8;
				sense_buf[6] = failing_lba & 0x000000ff;
			}

			sense_buf[7] = 6;	/* additional length */
		}
	}
}

/**
 * ipr_erp_start - Process an error response for a SCSI op
 * @ioa_cfg:	ioa config struct
 * @ipr_cmd:	ipr command struct
 *
 * This function determines whether or not to initiate ERP
 * on the affected device.
 *
 * Return value:
 * 	nothing
 **/
static void ipr_erp_start(struct ipr_ioa_cfg *ioa_cfg,
			      struct ipr_cmnd *ipr_cmd)
{
	struct scsi_cmnd *scsi_cmd = ipr_cmd->scsi_cmd;
	struct ipr_resource_entry *res = scsi_cmd->device->hostdata;
	u32 ioasc = be32_to_cpu(ipr_cmd->ioasa.ioasc);

	if (!res) {
		ipr_scsi_eh_done(ipr_cmd);
		return;
	}

	if (ipr_is_gscsi(res))
		ipr_dump_ioasa(ioa_cfg, ipr_cmd);
	else
		ipr_gen_sense(ipr_cmd);

	switch (ioasc & IPR_IOASC_IOASC_MASK) {
	case IPR_IOASC_ABORTED_CMD_TERM_BY_HOST:
		scsi_cmd->result |= (DID_ERROR << 16);
		break;
	case IPR_IOASC_IR_RESOURCE_HANDLE:
		scsi_cmd->result |= (DID_NO_CONNECT << 16);
		break;
	case IPR_IOASC_HW_SEL_TIMEOUT:
		scsi_cmd->result |= (DID_NO_CONNECT << 16);
		res->needs_sync_complete = 1;
		break;
	case IPR_IOASC_SYNC_REQUIRED:
		if (!res->in_erp)
			res->needs_sync_complete = 1;
		scsi_cmd->result |= (DID_IMM_RETRY << 16);
		break;
	case IPR_IOASC_MED_DO_NOT_REALLOC: /* prevent retries */
		scsi_cmd->result |= (DID_PASSTHROUGH << 16);
		break;
	case IPR_IOASC_BUS_WAS_RESET:
	case IPR_IOASC_BUS_WAS_RESET_BY_OTHER:
		/*
		 * Report the bus reset and ask for a retry. The device
		 * will give CC/UA the next command.
		 */
		if (!res->resetting_device)
			scsi_report_bus_reset(ioa_cfg->host, scsi_cmd->device->channel);
		scsi_cmd->result |= (DID_ERROR << 16);
		res->needs_sync_complete = 1;
		break;
	case IPR_IOASC_HW_DEV_BUS_STATUS:
		scsi_cmd->result |= IPR_IOASC_SENSE_STATUS(ioasc);
		if (IPR_IOASC_SENSE_STATUS(ioasc) == SAM_STAT_CHECK_CONDITION) {
			ipr_erp_cancel_all(ipr_cmd);
			return;
		}
		res->needs_sync_complete = 1;
		break;
	case IPR_IOASC_NR_INIT_CMD_REQUIRED:
		break;
	default:
		scsi_cmd->result |= (DID_ERROR << 16);
		if (!ipr_is_vset_device(res))
			res->needs_sync_complete = 1;
		break;
	}

	ipr_unmap_sglist(ioa_cfg, ipr_cmd);
	list_add_tail(&ipr_cmd->queue, &ioa_cfg->free_q);
	scsi_cmd->scsi_done(scsi_cmd);
}

/**
 * ipr_scsi_done - mid-layer done function
 * @ipr_cmd:	ipr command struct
 *
 * This function is invoked by the interrupt handler for
 * ops generated by the SCSI mid-layer
 *
 * Return value:
 * 	none
 **/
static void ipr_scsi_done(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	struct scsi_cmnd *scsi_cmd = ipr_cmd->scsi_cmd;
	u32 ioasc = be32_to_cpu(ipr_cmd->ioasa.ioasc);

	scsi_cmd->resid = be32_to_cpu(ipr_cmd->ioasa.residual_data_len);

	if (likely(IPR_IOASC_SENSE_KEY(ioasc) == 0)) {
		ipr_unmap_sglist(ioa_cfg, ipr_cmd);
		list_add_tail(&ipr_cmd->queue, &ioa_cfg->free_q);
		scsi_cmd->scsi_done(scsi_cmd);
	} else
		ipr_erp_start(ioa_cfg, ipr_cmd);
}

/**
 * ipr_save_ioafp_mode_select - Save adapters mode select data
 * @ioa_cfg:	ioa config struct
 * @scsi_cmd:	scsi command struct
 *
 * This function saves mode select data for the adapter to
 * use following an adapter reset.
 *
 * Return value:
 *	0 on success / SCSI_MLQUEUE_HOST_BUSY on failure
 **/
static int ipr_save_ioafp_mode_select(struct ipr_ioa_cfg *ioa_cfg,
				       struct scsi_cmnd *scsi_cmd)
{
	if (!ioa_cfg->saved_mode_pages) {
		ioa_cfg->saved_mode_pages  = kmalloc(sizeof(struct ipr_mode_pages),
						     GFP_ATOMIC);
		if (!ioa_cfg->saved_mode_pages) {
			dev_err(&ioa_cfg->pdev->dev,
				"IOA mode select buffer allocation failed\n");
			return SCSI_MLQUEUE_HOST_BUSY;
		}
	}

	memcpy(ioa_cfg->saved_mode_pages, scsi_cmd->buffer, scsi_cmd->cmnd[4]);
	ioa_cfg->saved_mode_page_len = scsi_cmd->cmnd[4];
	return 0;
}

/**
 * ipr_queuecommand - Queue a mid-layer request
 * @scsi_cmd:	scsi command struct
 * @done:		done function
 *
 * This function queues a request generated by the mid-layer.
 *
 * Return value:
 *	0 on success
 *	SCSI_MLQUEUE_DEVICE_BUSY if device is busy
 *	SCSI_MLQUEUE_HOST_BUSY if host is busy
 **/
static int ipr_queuecommand(struct scsi_cmnd *scsi_cmd,
			    void (*done) (struct scsi_cmnd *))
{
	struct ipr_ioa_cfg *ioa_cfg;
	struct ipr_resource_entry *res;
	struct ipr_ioarcb *ioarcb;
	struct ipr_cmnd *ipr_cmd;
	int rc = 0;

	scsi_cmd->scsi_done = done;
	ioa_cfg = (struct ipr_ioa_cfg *)scsi_cmd->device->host->hostdata;
	res = scsi_cmd->device->hostdata;
	scsi_cmd->result = (DID_OK << 16);

	/*
	 * We are currently blocking all devices due to a host reset
	 * We have told the host to stop giving us new requests, but
	 * ERP ops don't count. FIXME
	 */
	if (unlikely(!ioa_cfg->allow_cmds))
		return SCSI_MLQUEUE_HOST_BUSY;

	/*
	 * FIXME - Create scsi_set_host_offline interface
	 *  and the ioa_is_dead check can be removed
	 */
	if (unlikely(ioa_cfg->ioa_is_dead || !res)) {
		memset(scsi_cmd->sense_buffer, 0, SCSI_SENSE_BUFFERSIZE);
		scsi_cmd->result = (DID_NO_CONNECT << 16);
		scsi_cmd->scsi_done(scsi_cmd);
		return 0;
	}

	ipr_cmd = ipr_get_free_ipr_cmnd(ioa_cfg);
	ioarcb = &ipr_cmd->ioarcb;
	list_add_tail(&ipr_cmd->queue, &ioa_cfg->pending_q);

	memcpy(ioarcb->cmd_pkt.cdb, scsi_cmd->cmnd, scsi_cmd->cmd_len);
	ipr_cmd->scsi_cmd = scsi_cmd;
	ioarcb->res_handle = res->cfgte.res_handle;
	ipr_cmd->done = ipr_scsi_done;
	ipr_trc_hook(ipr_cmd, IPR_TRACE_START, IPR_GET_PHYS_LOC(res->cfgte.res_addr));

	if (ipr_is_gscsi(res) || ipr_is_vset_device(res)) {
		if (scsi_cmd->underflow == 0)
			ioarcb->cmd_pkt.flags_hi |= IPR_FLAGS_HI_NO_ULEN_CHK;

		if (res->needs_sync_complete) {
			ioarcb->cmd_pkt.flags_hi |= IPR_FLAGS_HI_SYNC_COMPLETE;
			res->needs_sync_complete = 0;
		}

		ioarcb->cmd_pkt.flags_hi |= IPR_FLAGS_HI_NO_LINK_DESC;
		ioarcb->cmd_pkt.flags_lo |= IPR_FLAGS_LO_DELAY_AFTER_RST;
		ioarcb->cmd_pkt.flags_lo |= IPR_FLAGS_LO_ALIGNED_BFR;
		ioarcb->cmd_pkt.flags_lo |= ipr_get_task_attributes(scsi_cmd);
	}

	if (!ipr_is_gscsi(res) && scsi_cmd->cmnd[0] >= 0xC0)
		ioarcb->cmd_pkt.request_type = IPR_RQTYPE_IOACMD;

	if (ipr_is_ioa_resource(res) && scsi_cmd->cmnd[0] == MODE_SELECT)
		rc = ipr_save_ioafp_mode_select(ioa_cfg, scsi_cmd);

	if (likely(rc == 0))
		rc = ipr_build_ioadl(ioa_cfg, ipr_cmd);

	if (likely(rc == 0)) {
		mb();
		writel(be32_to_cpu(ipr_cmd->ioarcb.ioarcb_host_pci_addr),
		       ioa_cfg->regs.ioarrin_reg);
	} else {
		 list_move_tail(&ipr_cmd->queue, &ioa_cfg->free_q);
		 return SCSI_MLQUEUE_HOST_BUSY;
	}

	return 0;
}

/**
 * ipr_info - Get information about the card/driver
 * @scsi_host:	scsi host struct
 *
 * Return value:
 * 	pointer to buffer with description string
 **/
static const char * ipr_ioa_info(struct Scsi_Host *host)
{
	static char buffer[512];
	struct ipr_ioa_cfg *ioa_cfg;
	unsigned long lock_flags = 0;

	ioa_cfg = (struct ipr_ioa_cfg *) host->hostdata;

	spin_lock_irqsave(host->host_lock, lock_flags);
	sprintf(buffer, "IBM %X Storage Adapter", ioa_cfg->type);
	spin_unlock_irqrestore(host->host_lock, lock_flags);

	return buffer;
}

static struct scsi_host_template driver_template = {
	.module = THIS_MODULE,
	.name = "IPR",
	.info = ipr_ioa_info,
	.queuecommand = ipr_queuecommand,
	.eh_abort_handler = ipr_eh_abort,
	.eh_device_reset_handler = ipr_eh_dev_reset,
	.eh_host_reset_handler = ipr_eh_host_reset,
	.slave_alloc = ipr_slave_alloc,
	.slave_configure = ipr_slave_configure,
	.slave_destroy = ipr_slave_destroy,
	.bios_param = ipr_biosparam,
	.can_queue = IPR_MAX_COMMANDS,
	.this_id = -1,
	.sg_tablesize = IPR_MAX_SGLIST,
	.max_sectors = IPR_MAX_SECTORS,
	.cmd_per_lun = IPR_MAX_CMD_PER_LUN,
	.use_clustering = ENABLE_CLUSTERING,
	.shost_attrs = ipr_ioa_attrs,
	.sdev_attrs = ipr_dev_attrs,
	.proc_name = IPR_NAME
};

#ifdef CONFIG_PPC_PSERIES
static const u16 ipr_blocked_processors[] = {
	PV_NORTHSTAR,
	PV_PULSAR,
	PV_POWER4,
	PV_ICESTAR,
	PV_SSTAR,
	PV_POWER4p,
	PV_630,
	PV_630p
};

/**
 * ipr_invalid_adapter - Determine if this adapter is supported on this hardware
 * @ioa_cfg:	ioa cfg struct
 *
 * Adapters that use Gemstone revision < 3.1 do not work reliably on
 * certain pSeries hardware. This function determines if the given
 * adapter is in one of these confgurations or not.
 *
 * Return value:
 * 	1 if adapter is not supported / 0 if adapter is supported
 **/
static int ipr_invalid_adapter(struct ipr_ioa_cfg *ioa_cfg)
{
	u8 rev_id;
	int i;

	if (ioa_cfg->type == 0x5702) {
		if (pci_read_config_byte(ioa_cfg->pdev, PCI_REVISION_ID,
					 &rev_id) == PCIBIOS_SUCCESSFUL) {
			if (rev_id < 4) {
				for (i = 0; i < ARRAY_SIZE(ipr_blocked_processors); i++){
					if (__is_processor(ipr_blocked_processors[i]))
						return 1;
				}
			}
		}
	}
	return 0;
}
#else
#define ipr_invalid_adapter(ioa_cfg) 0
#endif

/**
 * ipr_ioa_bringdown_done - IOA bring down completion.
 * @ipr_cmd:	ipr command struct
 *
 * This function processes the completion of an adapter bring down.
 * It wakes any reset sleepers.
 *
 * Return value:
 * 	IPR_RC_JOB_RETURN
 **/
static int ipr_ioa_bringdown_done(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;

	ENTER;
	ioa_cfg->in_reset_reload = 0;
	ioa_cfg->reset_retries = 0;
	list_add_tail(&ipr_cmd->queue, &ioa_cfg->free_q);
	wake_up_all(&ioa_cfg->reset_wait_q);

	spin_unlock_irq(ioa_cfg->host->host_lock);
	scsi_unblock_requests(ioa_cfg->host);
	spin_lock_irq(ioa_cfg->host->host_lock);
	LEAVE;

	return IPR_RC_JOB_RETURN;
}

/**
 * ipr_ioa_reset_done - IOA reset completion.
 * @ipr_cmd:	ipr command struct
 *
 * This function processes the completion of an adapter reset.
 * It schedules any necessary mid-layer add/removes and
 * wakes any reset sleepers.
 *
 * Return value:
 * 	IPR_RC_JOB_RETURN
 **/
static int ipr_ioa_reset_done(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	struct ipr_resource_entry *res;
	struct ipr_hostrcb *hostrcb, *temp;
	int i = 0;

	ENTER;
	ioa_cfg->in_reset_reload = 0;
	ioa_cfg->allow_cmds = 1;
	ioa_cfg->reset_cmd = NULL;

	list_for_each_entry(res, &ioa_cfg->used_res_q, queue) {
		if (ioa_cfg->allow_ml_add_del && (res->add_to_ml || res->del_from_ml)) {
			ipr_trace;
			schedule_work(&ioa_cfg->work_q);
			break;
		}
	}

	list_for_each_entry_safe(hostrcb, temp, &ioa_cfg->hostrcb_free_q, queue) {
		list_del(&hostrcb->queue);
		if (i++ < IPR_NUM_LOG_HCAMS)
			ipr_send_hcam(ioa_cfg, IPR_HCAM_CDB_OP_CODE_LOG_DATA, hostrcb);
		else
			ipr_send_hcam(ioa_cfg, IPR_HCAM_CDB_OP_CODE_CONFIG_CHANGE, hostrcb);
	}

	dev_info(&ioa_cfg->pdev->dev, "IOA initialized.\n");

	ioa_cfg->reset_retries = 0;
	list_add_tail(&ipr_cmd->queue, &ioa_cfg->free_q);
	wake_up_all(&ioa_cfg->reset_wait_q);

	spin_unlock_irq(ioa_cfg->host->host_lock);
	scsi_unblock_requests(ioa_cfg->host);
	spin_lock_irq(ioa_cfg->host->host_lock);

	if (!ioa_cfg->allow_cmds)
		scsi_block_requests(ioa_cfg->host);

	LEAVE;
	return IPR_RC_JOB_RETURN;
}

/**
 * ipr_set_sup_dev_dflt - Initialize a Set Supported Device buffer
 * @supported_dev:	supported device struct
 * @vpids:			vendor product id struct
 *
 * Return value:
 * 	none
 **/
static void ipr_set_sup_dev_dflt(struct ipr_supported_device *supported_dev,
				 struct ipr_std_inq_vpids *vpids)
{
	memset(supported_dev, 0, sizeof(struct ipr_supported_device));
	memcpy(&supported_dev->vpids, vpids, sizeof(struct ipr_std_inq_vpids));
	supported_dev->num_records = 1;
	supported_dev->data_length =
		cpu_to_be16(sizeof(struct ipr_supported_device));
	supported_dev->reserved = 0;
}

/**
 * ipr_set_supported_devs - Send Set Supported Devices for a device
 * @ipr_cmd:	ipr command struct
 *
 * This function send a Set Supported Devices to the adapter
 *
 * Return value:
 * 	IPR_RC_JOB_CONTINUE / IPR_RC_JOB_RETURN
 **/
static int ipr_set_supported_devs(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	struct ipr_supported_device *supp_dev = &ioa_cfg->vpd_cbs->supp_dev;
	struct ipr_ioadl_desc *ioadl = ipr_cmd->ioadl;
	struct ipr_ioarcb *ioarcb = &ipr_cmd->ioarcb;
	struct ipr_resource_entry *res = ipr_cmd->u.res;

	ipr_cmd->job_step = ipr_ioa_reset_done;

	list_for_each_entry_continue(res, &ioa_cfg->used_res_q, queue) {
		if (!ipr_is_af_dasd_device(res))
			continue;

		ipr_cmd->u.res = res;
		ipr_set_sup_dev_dflt(supp_dev, &res->cfgte.std_inq_data.vpids);

		ioarcb->res_handle = cpu_to_be32(IPR_IOA_RES_HANDLE);
		ioarcb->cmd_pkt.flags_hi |= IPR_FLAGS_HI_WRITE_NOT_READ;
		ioarcb->cmd_pkt.request_type = IPR_RQTYPE_IOACMD;

		ioarcb->cmd_pkt.cdb[0] = IPR_SET_SUPPORTED_DEVICES;
		ioarcb->cmd_pkt.cdb[7] = (sizeof(struct ipr_supported_device) >> 8) & 0xff;
		ioarcb->cmd_pkt.cdb[8] = sizeof(struct ipr_supported_device) & 0xff;

		ioadl->flags_and_data_len = cpu_to_be32(IPR_IOADL_FLAGS_WRITE_LAST |
							sizeof(struct ipr_supported_device));
		ioadl->address = cpu_to_be32(ioa_cfg->vpd_cbs_dma +
					     offsetof(struct ipr_misc_cbs, supp_dev));
		ioarcb->write_ioadl_len = cpu_to_be32(sizeof(struct ipr_ioadl_desc));
		ioarcb->write_data_transfer_length =
			cpu_to_be32(sizeof(struct ipr_supported_device));

		ipr_do_req(ipr_cmd, ipr_reset_ioa_job, ipr_timeout,
			   IPR_SET_SUP_DEVICE_TIMEOUT);

		ipr_cmd->job_step = ipr_set_supported_devs;
		return IPR_RC_JOB_RETURN;
	}

	return IPR_RC_JOB_CONTINUE;
}

/**
 * ipr_get_mode_page - Locate specified mode page
 * @mode_pages:	mode page buffer
 * @page_code:	page code to find
 * @len:		minimum required length for mode page
 *
 * Return value:
 * 	pointer to mode page / NULL on failure
 **/
static void *ipr_get_mode_page(struct ipr_mode_pages *mode_pages,
			       u32 page_code, u32 len)
{
	struct ipr_mode_page_hdr *mode_hdr;
	u32 page_length;
	u32 length;

	if (!mode_pages || (mode_pages->hdr.length == 0))
		return NULL;

	length = (mode_pages->hdr.length + 1) - 4 - mode_pages->hdr.block_desc_len;
	mode_hdr = (struct ipr_mode_page_hdr *)
		(mode_pages->data + mode_pages->hdr.block_desc_len);

	while (length) {
		if (IPR_GET_MODE_PAGE_CODE(mode_hdr) == page_code) {
			if (mode_hdr->page_length >= (len - sizeof(struct ipr_mode_page_hdr)))
				return mode_hdr;
			break;
		} else {
			page_length = (sizeof(struct ipr_mode_page_hdr) +
				       mode_hdr->page_length);
			length -= page_length;
			mode_hdr = (struct ipr_mode_page_hdr *)
				((unsigned long)mode_hdr + page_length);
		}
	}
	return NULL;
}

/**
 * ipr_check_term_power - Check for term power errors
 * @ioa_cfg:	ioa config struct
 * @mode_pages:	IOAFP mode pages buffer
 *
 * Check the IOAFP's mode page 28 for term power errors
 *
 * Return value:
 * 	nothing
 **/
static void ipr_check_term_power(struct ipr_ioa_cfg *ioa_cfg,
				 struct ipr_mode_pages *mode_pages)
{
	int i;
	int entry_length;
	struct ipr_dev_bus_entry *bus;
	struct ipr_mode_page28 *mode_page;

	mode_page = ipr_get_mode_page(mode_pages, 0x28,
				      sizeof(struct ipr_mode_page28));

	entry_length = mode_page->entry_length;

	bus = mode_page->bus;

	for (i = 0; i < mode_page->num_entries; i++) {
		if (bus->flags & IPR_SCSI_ATTR_NO_TERM_PWR) {
			dev_err(&ioa_cfg->pdev->dev,
				"Term power is absent on scsi bus %d\n",
				bus->res_addr.bus);
		}

		bus = (struct ipr_dev_bus_entry *)((char *)bus + entry_length);
	}
}

/**
 * ipr_scsi_bus_speed_limit - Limit the SCSI speed based on SES table
 * @ioa_cfg:	ioa config struct
 *
 * Looks through the config table checking for SES devices. If
 * the SES device is in the SES table indicating a maximum SCSI
 * bus speed, the speed is limited for the bus.
 *
 * Return value:
 * 	none
 **/
static void ipr_scsi_bus_speed_limit(struct ipr_ioa_cfg *ioa_cfg)
{
	u32 max_xfer_rate;
	int i;

	for (i = 0; i < IPR_MAX_NUM_BUSES; i++) {
		max_xfer_rate = ipr_get_max_scsi_speed(ioa_cfg, i,
						       ioa_cfg->bus_attr[i].bus_width);

		if (max_xfer_rate < ioa_cfg->bus_attr[i].max_xfer_rate)
			ioa_cfg->bus_attr[i].max_xfer_rate = max_xfer_rate;
	}
}

/**
 * ipr_modify_ioafp_mode_page_28 - Modify IOAFP Mode Page 28
 * @ioa_cfg:	ioa config struct
 * @mode_pages:	mode page 28 buffer
 *
 * Updates mode page 28 based on driver configuration
 *
 * Return value:
 * 	none
 **/
static void ipr_modify_ioafp_mode_page_28(struct ipr_ioa_cfg *ioa_cfg,
					  	struct ipr_mode_pages *mode_pages)
{
	int i, entry_length;
	struct ipr_dev_bus_entry *bus;
	struct ipr_bus_attributes *bus_attr;
	struct ipr_mode_page28 *mode_page;

	mode_page = ipr_get_mode_page(mode_pages, 0x28,
				      sizeof(struct ipr_mode_page28));

	entry_length = mode_page->entry_length;

	/* Loop for each device bus entry */
	for (i = 0, bus = mode_page->bus;
	     i < mode_page->num_entries;
	     i++, bus = (struct ipr_dev_bus_entry *)((u8 *)bus + entry_length)) {
		if (bus->res_addr.bus > IPR_MAX_NUM_BUSES) {
			dev_err(&ioa_cfg->pdev->dev,
				"Invalid resource address reported: 0x%08X\n",
				IPR_GET_PHYS_LOC(bus->res_addr));
			continue;
		}

		bus_attr = &ioa_cfg->bus_attr[i];
		bus->extended_reset_delay = IPR_EXTENDED_RESET_DELAY;
		bus->bus_width = bus_attr->bus_width;
		bus->max_xfer_rate = cpu_to_be32(bus_attr->max_xfer_rate);
		bus->flags &= ~IPR_SCSI_ATTR_QAS_MASK;
		if (bus_attr->qas_enabled)
			bus->flags |= IPR_SCSI_ATTR_ENABLE_QAS;
		else
			bus->flags |= IPR_SCSI_ATTR_DISABLE_QAS;
	}
}

/**
 * ipr_build_mode_select - Build a mode select command
 * @ipr_cmd:	ipr command struct
 * @res_handle:	resource handle to send command to
 * @parm:		Byte 2 of Mode Sense command
 * @dma_addr:	DMA buffer address
 * @xfer_len:	data transfer length
 *
 * Return value:
 * 	none
 **/
static void ipr_build_mode_select(struct ipr_cmnd *ipr_cmd,
				  u32 res_handle, u8 parm, u32 dma_addr,
				  u8 xfer_len)
{
	struct ipr_ioadl_desc *ioadl = ipr_cmd->ioadl;
	struct ipr_ioarcb *ioarcb = &ipr_cmd->ioarcb;

	ioarcb->res_handle = res_handle;
	ioarcb->cmd_pkt.request_type = IPR_RQTYPE_SCSICDB;
	ioarcb->cmd_pkt.flags_hi |= IPR_FLAGS_HI_WRITE_NOT_READ;
	ioarcb->cmd_pkt.cdb[0] = MODE_SELECT;
	ioarcb->cmd_pkt.cdb[1] = parm;
	ioarcb->cmd_pkt.cdb[4] = xfer_len;

	ioadl->flags_and_data_len =
		cpu_to_be32(IPR_IOADL_FLAGS_WRITE_LAST | xfer_len);
	ioadl->address = cpu_to_be32(dma_addr);
	ioarcb->write_ioadl_len = cpu_to_be32(sizeof(struct ipr_ioadl_desc));
	ioarcb->write_data_transfer_length = cpu_to_be32(xfer_len);
}

/**
 * ipr_ioafp_mode_select_page28 - Issue Mode Select Page 28 to IOA
 * @ipr_cmd:	ipr command struct
 *
 * This function sets up the SCSI bus attributes and sends
 * a Mode Select for Page 28 to activate them.
 *
 * Return value:
 * 	IPR_RC_JOB_RETURN
 **/
static int ipr_ioafp_mode_select_page28(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	struct ipr_mode_pages *mode_pages = &ioa_cfg->vpd_cbs->mode_pages;
	int length;

	ENTER;
	if (ioa_cfg->saved_mode_pages) {
		memcpy(mode_pages, ioa_cfg->saved_mode_pages,
		       ioa_cfg->saved_mode_page_len);
		length = ioa_cfg->saved_mode_page_len;
	} else {
		ipr_scsi_bus_speed_limit(ioa_cfg);
		ipr_check_term_power(ioa_cfg, mode_pages);
		ipr_modify_ioafp_mode_page_28(ioa_cfg, mode_pages);
		length = mode_pages->hdr.length + 1;
		mode_pages->hdr.length = 0;
	}

	ipr_build_mode_select(ipr_cmd, cpu_to_be32(IPR_IOA_RES_HANDLE), 0x11,
			      ioa_cfg->vpd_cbs_dma + offsetof(struct ipr_misc_cbs, mode_pages),
			      length);

	ipr_cmd->job_step = ipr_set_supported_devs;
	ipr_cmd->u.res = list_entry(ioa_cfg->used_res_q.next,
				    struct ipr_resource_entry, queue);

	ipr_do_req(ipr_cmd, ipr_reset_ioa_job, ipr_timeout, IPR_INTERNAL_TIMEOUT);

	LEAVE;
	return IPR_RC_JOB_RETURN;
}

/**
 * ipr_build_mode_sense - Builds a mode sense command
 * @ipr_cmd:	ipr command struct
 * @res:		resource entry struct
 * @parm:		Byte 2 of mode sense command
 * @dma_addr:	DMA address of mode sense buffer
 * @xfer_len:	Size of DMA buffer
 *
 * Return value:
 * 	none
 **/
static void ipr_build_mode_sense(struct ipr_cmnd *ipr_cmd,
				 u32 res_handle,
				 u8 parm, u32 dma_addr, u8 xfer_len)
{
	struct ipr_ioadl_desc *ioadl = ipr_cmd->ioadl;
	struct ipr_ioarcb *ioarcb = &ipr_cmd->ioarcb;

	ioarcb->res_handle = res_handle;
	ioarcb->cmd_pkt.cdb[0] = MODE_SENSE;
	ioarcb->cmd_pkt.cdb[2] = parm;
	ioarcb->cmd_pkt.cdb[4] = xfer_len;
	ioarcb->cmd_pkt.request_type = IPR_RQTYPE_SCSICDB;

	ioadl->flags_and_data_len =
		cpu_to_be32(IPR_IOADL_FLAGS_READ_LAST | xfer_len);
	ioadl->address = cpu_to_be32(dma_addr);
	ioarcb->read_ioadl_len = cpu_to_be32(sizeof(struct ipr_ioadl_desc));
	ioarcb->read_data_transfer_length = cpu_to_be32(xfer_len);
}

/**
 * ipr_ioafp_mode_sense_page28 - Issue Mode Sense Page 28 to IOA
 * @ipr_cmd:	ipr command struct
 *
 * This function send a Page 28 mode sense to the IOA to
 * retrieve SCSI bus attributes.
 *
 * Return value:
 * 	IPR_RC_JOB_RETURN
 **/
static int ipr_ioafp_mode_sense_page28(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;

	ENTER;
	ipr_build_mode_sense(ipr_cmd, cpu_to_be32(IPR_IOA_RES_HANDLE),
			     0x28, ioa_cfg->vpd_cbs_dma +
			     offsetof(struct ipr_misc_cbs, mode_pages),
			     sizeof(struct ipr_mode_pages));

	ipr_cmd->job_step = ipr_ioafp_mode_select_page28;

	ipr_do_req(ipr_cmd, ipr_reset_ioa_job, ipr_timeout, IPR_INTERNAL_TIMEOUT);

	LEAVE;
	return IPR_RC_JOB_RETURN;
}

/**
 * ipr_init_res_table - Initialize the resource table
 * @ipr_cmd:	ipr command struct
 *
 * This function looks through the existing resource table, comparing
 * it with the config table. This function will take care of old/new
 * devices and schedule adding/removing them from the mid-layer
 * as appropriate.
 *
 * Return value:
 * 	IPR_RC_JOB_CONTINUE
 **/
static int ipr_init_res_table(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	struct ipr_resource_entry *res, *temp;
	struct ipr_config_table_entry *cfgte;
	int found, i;
	LIST_HEAD(old_res);

	ENTER;
	if (ioa_cfg->cfg_table->hdr.flags & IPR_UCODE_DOWNLOAD_REQ)
		dev_err(&ioa_cfg->pdev->dev, "Microcode download required\n");

	list_for_each_entry_safe(res, temp, &ioa_cfg->used_res_q, queue)
		list_move_tail(&res->queue, &old_res);

	for (i = 0; i < ioa_cfg->cfg_table->hdr.num_entries; i++) {
		cfgte = &ioa_cfg->cfg_table->dev[i];
		found = 0;

		list_for_each_entry_safe(res, temp, &old_res, queue) {
			if (!memcmp(&res->cfgte.res_addr,
				    &cfgte->res_addr, sizeof(cfgte->res_addr))) {
				list_move_tail(&res->queue, &ioa_cfg->used_res_q);
				found = 1;
				break;
			}
		}

		if (!found) {
			if (list_empty(&ioa_cfg->free_res_q)) {
				dev_err(&ioa_cfg->pdev->dev, "Too many devices attached\n");
				break;
			}

			found = 1;
			res = list_entry(ioa_cfg->free_res_q.next,
					 struct ipr_resource_entry, queue);
			list_move_tail(&res->queue, &ioa_cfg->used_res_q);
			ipr_init_res_entry(res);
			res->add_to_ml = 1;
		}

		if (found)
			memcpy(&res->cfgte, cfgte, sizeof(struct ipr_config_table_entry));
	}

	list_for_each_entry_safe(res, temp, &old_res, queue) {
		if (res->sdev) {
			res->del_from_ml = 1;
			list_move_tail(&res->queue, &ioa_cfg->used_res_q);
		} else {
			list_move_tail(&res->queue, &ioa_cfg->free_res_q);
		}
	}

	ipr_cmd->job_step = ipr_ioafp_mode_sense_page28;

	LEAVE;
	return IPR_RC_JOB_CONTINUE;
}

/**
 * ipr_ioafp_query_ioa_cfg - Send a Query IOA Config to the adapter.
 * @ipr_cmd:	ipr command struct
 *
 * This function sends a Query IOA Configuration command
 * to the adapter to retrieve the IOA configuration table.
 *
 * Return value:
 * 	IPR_RC_JOB_RETURN
 **/
static int ipr_ioafp_query_ioa_cfg(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	struct ipr_ioarcb *ioarcb = &ipr_cmd->ioarcb;
	struct ipr_ioadl_desc *ioadl = ipr_cmd->ioadl;
	struct ipr_inquiry_page3 *ucode_vpd = &ioa_cfg->vpd_cbs->page3_data;

	ENTER;
	dev_info(&ioa_cfg->pdev->dev, "Adapter firmware version: %02X%02X%02X%02X\n",
		 ucode_vpd->major_release, ucode_vpd->card_type,
		 ucode_vpd->minor_release[0], ucode_vpd->minor_release[1]);
	ioarcb->cmd_pkt.request_type = IPR_RQTYPE_IOACMD;
	ioarcb->res_handle = cpu_to_be32(IPR_IOA_RES_HANDLE);

	ioarcb->cmd_pkt.cdb[0] = IPR_QUERY_IOA_CONFIG;
	ioarcb->cmd_pkt.cdb[7] = (sizeof(struct ipr_config_table) >> 8) & 0xff;
	ioarcb->cmd_pkt.cdb[8] = sizeof(struct ipr_config_table) & 0xff;

	ioarcb->read_ioadl_len = cpu_to_be32(sizeof(struct ipr_ioadl_desc));
	ioarcb->read_data_transfer_length =
		cpu_to_be32(sizeof(struct ipr_config_table));

	ioadl->address = cpu_to_be32(ioa_cfg->cfg_table_dma);
	ioadl->flags_and_data_len =
		cpu_to_be32(IPR_IOADL_FLAGS_READ_LAST | sizeof(struct ipr_config_table));

	ipr_cmd->job_step = ipr_init_res_table;

	ipr_do_req(ipr_cmd, ipr_reset_ioa_job, ipr_timeout, IPR_INTERNAL_TIMEOUT);

	LEAVE;
	return IPR_RC_JOB_RETURN;
}

/**
 * ipr_ioafp_inquiry - Send an Inquiry to the adapter.
 * @ipr_cmd:	ipr command struct
 *
 * This utility function sends an inquiry to the adapter.
 *
 * Return value:
 * 	none
 **/
static void ipr_ioafp_inquiry(struct ipr_cmnd *ipr_cmd, u8 flags, u8 page,
			      u32 dma_addr, u8 xfer_len)
{
	struct ipr_ioarcb *ioarcb = &ipr_cmd->ioarcb;
	struct ipr_ioadl_desc *ioadl = ipr_cmd->ioadl;

	ENTER;
	ioarcb->cmd_pkt.request_type = IPR_RQTYPE_SCSICDB;
	ioarcb->res_handle = cpu_to_be32(IPR_IOA_RES_HANDLE);

	ioarcb->cmd_pkt.cdb[0] = INQUIRY;
	ioarcb->cmd_pkt.cdb[1] = flags;
	ioarcb->cmd_pkt.cdb[2] = page;
	ioarcb->cmd_pkt.cdb[4] = xfer_len;

	ioarcb->read_ioadl_len = cpu_to_be32(sizeof(struct ipr_ioadl_desc));
	ioarcb->read_data_transfer_length = cpu_to_be32(xfer_len);

	ioadl->address = cpu_to_be32(dma_addr);
	ioadl->flags_and_data_len =
		cpu_to_be32(IPR_IOADL_FLAGS_READ_LAST | xfer_len);

	ipr_do_req(ipr_cmd, ipr_reset_ioa_job, ipr_timeout, IPR_INTERNAL_TIMEOUT);
	LEAVE;
}

/**
 * ipr_ioafp_page3_inquiry - Send a Page 3 Inquiry to the adapter.
 * @ipr_cmd:	ipr command struct
 *
 * This function sends a Page 3 inquiry to the adapter
 * to retrieve software VPD information.
 *
 * Return value:
 * 	IPR_RC_JOB_CONTINUE / IPR_RC_JOB_RETURN
 **/
static int ipr_ioafp_page3_inquiry(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	char type[5];

	ENTER;

	/* Grab the type out of the VPD and store it away */
	memcpy(type, ioa_cfg->vpd_cbs->ioa_vpd.std_inq_data.vpids.product_id, 4);
	type[4] = '\0';
	ioa_cfg->type = simple_strtoul((char *)type, NULL, 16);

	ipr_cmd->job_step = ipr_ioafp_query_ioa_cfg;

	ipr_ioafp_inquiry(ipr_cmd, 1, 3,
			  ioa_cfg->vpd_cbs_dma + offsetof(struct ipr_misc_cbs, page3_data),
			  sizeof(struct ipr_inquiry_page3));

	LEAVE;
	return IPR_RC_JOB_RETURN;
}

/**
 * ipr_ioafp_std_inquiry - Send a Standard Inquiry to the adapter.
 * @ipr_cmd:	ipr command struct
 *
 * This function sends a standard inquiry to the adapter.
 *
 * Return value:
 * 	IPR_RC_JOB_RETURN
 **/
static int ipr_ioafp_std_inquiry(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;

	ENTER;
	ipr_cmd->job_step = ipr_ioafp_page3_inquiry;

	ipr_ioafp_inquiry(ipr_cmd, 0, 0,
			  ioa_cfg->vpd_cbs_dma + offsetof(struct ipr_misc_cbs, ioa_vpd),
			  sizeof(struct ipr_ioa_vpd));

	LEAVE;
	return IPR_RC_JOB_RETURN;
}

/**
 * ipr_ioafp_indentify_hrrq - Send Identify Host RRQ.
 * @ipr_cmd:	ipr command struct
 *
 * This function send an Identify Host Request Response Queue
 * command to establish the HRRQ with the adapter.
 *
 * Return value:
 * 	IPR_RC_JOB_RETURN
 **/
static int ipr_ioafp_indentify_hrrq(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	struct ipr_ioarcb *ioarcb = &ipr_cmd->ioarcb;

	ENTER;
	dev_info(&ioa_cfg->pdev->dev, "Starting IOA initialization sequence.\n");

	ioarcb->cmd_pkt.cdb[0] = IPR_ID_HOST_RR_Q;
	ioarcb->res_handle = cpu_to_be32(IPR_IOA_RES_HANDLE);

	ioarcb->cmd_pkt.request_type = IPR_RQTYPE_IOACMD;
	ioarcb->cmd_pkt.cdb[2] =
		((u32) ioa_cfg->host_rrq_dma >> 24) & 0xff;
	ioarcb->cmd_pkt.cdb[3] =
		((u32) ioa_cfg->host_rrq_dma >> 16) & 0xff;
	ioarcb->cmd_pkt.cdb[4] =
		((u32) ioa_cfg->host_rrq_dma >> 8) & 0xff;
	ioarcb->cmd_pkt.cdb[5] =
		((u32) ioa_cfg->host_rrq_dma) & 0xff;
	ioarcb->cmd_pkt.cdb[7] =
		((sizeof(u32) * IPR_NUM_CMD_BLKS) >> 8) & 0xff;
	ioarcb->cmd_pkt.cdb[8] =
		(sizeof(u32) * IPR_NUM_CMD_BLKS) & 0xff;

	ipr_cmd->job_step = ipr_ioafp_std_inquiry;

	ipr_do_req(ipr_cmd, ipr_reset_ioa_job, ipr_timeout, IPR_INTERNAL_TIMEOUT);

	LEAVE;
	return IPR_RC_JOB_RETURN;
}

/**
 * ipr_reset_timer_done - Adapter reset timer function
 * @ipr_cmd:	ipr command struct
 *
 * Description: This function is used in adapter reset processing
 * for timing events. If the reset_cmd pointer in the IOA
 * config struct is not this adapter's we are doing nested
 * resets and fail_all_ops will take care of freeing the
 * command block.
 *
 * Return value:
 * 	none
 **/
static void ipr_reset_timer_done(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	unsigned long lock_flags = 0;

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);

	if (ioa_cfg->reset_cmd == ipr_cmd) {
		list_del(&ipr_cmd->queue);
		ipr_cmd->done(ipr_cmd);
	}

	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
}

/**
 * ipr_reset_start_timer - Start a timer for adapter reset job
 * @ipr_cmd:	ipr command struct
 * @timeout:	timeout value
 *
 * Description: This function is used in adapter reset processing
 * for timing events. If the reset_cmd pointer in the IOA
 * config struct is not this adapter's we are doing nested
 * resets and fail_all_ops will take care of freeing the
 * command block.
 *
 * Return value:
 * 	none
 **/
static void ipr_reset_start_timer(struct ipr_cmnd *ipr_cmd,
				  unsigned long timeout)
{
	list_add_tail(&ipr_cmd->queue, &ipr_cmd->ioa_cfg->pending_q);
	ipr_cmd->done = ipr_reset_ioa_job;

	ipr_cmd->timer.data = (unsigned long) ipr_cmd;
	ipr_cmd->timer.expires = jiffies + timeout;
	ipr_cmd->timer.function = (void (*)(unsigned long))ipr_reset_timer_done;
	add_timer(&ipr_cmd->timer);
}

/**
 * ipr_init_ioa_mem - Initialize ioa_cfg control block
 * @ioa_cfg:	ioa cfg struct
 *
 * Return value:
 * 	nothing
 **/
static void ipr_init_ioa_mem(struct ipr_ioa_cfg *ioa_cfg)
{
	memset(ioa_cfg->host_rrq, 0, sizeof(u32) * IPR_NUM_CMD_BLKS);

	/* Initialize Host RRQ pointers */
	ioa_cfg->hrrq_start = ioa_cfg->host_rrq;
	ioa_cfg->hrrq_end = &ioa_cfg->host_rrq[IPR_NUM_CMD_BLKS - 1];
	ioa_cfg->hrrq_curr = ioa_cfg->hrrq_start;
	ioa_cfg->toggle_bit = 1;

	/* Zero out config table */
	memset(ioa_cfg->cfg_table, 0, sizeof(struct ipr_config_table));
}

/**
 * ipr_reset_enable_ioa - Enable the IOA following a reset.
 * @ipr_cmd:	ipr command struct
 *
 * This function reinitializes some control blocks and
 * enables destructive diagnostics on the adapter.
 *
 * Return value:
 * 	IPR_RC_JOB_RETURN
 **/
static int ipr_reset_enable_ioa(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	volatile u32 int_reg;

	ENTER;
	ipr_cmd->job_step = ipr_ioafp_indentify_hrrq;
	ipr_init_ioa_mem(ioa_cfg);

	ioa_cfg->allow_interrupts = 1;
	int_reg = readl(ioa_cfg->regs.sense_interrupt_reg);

	if (int_reg & IPR_PCII_IOA_TRANS_TO_OPER) {
		writel((IPR_PCII_ERROR_INTERRUPTS | IPR_PCII_HRRQ_UPDATED),
		       ioa_cfg->regs.clr_interrupt_mask_reg);
		int_reg = readl(ioa_cfg->regs.sense_interrupt_mask_reg);
		return IPR_RC_JOB_CONTINUE;
	}

	/* Enable destructive diagnostics on IOA */
	writel(IPR_DOORBELL, ioa_cfg->regs.set_uproc_interrupt_reg);

	writel(IPR_PCII_OPER_INTERRUPTS, ioa_cfg->regs.clr_interrupt_mask_reg);
	int_reg = readl(ioa_cfg->regs.sense_interrupt_mask_reg);

	dev_info(&ioa_cfg->pdev->dev, "Initializing IOA.\n");

	ipr_cmd->timer.data = (unsigned long) ipr_cmd;
	ipr_cmd->timer.expires = jiffies + IPR_OPERATIONAL_TIMEOUT;
	ipr_cmd->timer.function = (void (*)(unsigned long))ipr_timeout;
	ipr_cmd->done = ipr_reset_ioa_job;
	add_timer(&ipr_cmd->timer);
	list_add_tail(&ipr_cmd->queue, &ioa_cfg->pending_q);

	LEAVE;
	return IPR_RC_JOB_RETURN;
}

/**
 * ipr_reset_wait_for_dump - Wait for a dump to timeout.
 * @ipr_cmd:	ipr command struct
 *
 * This function is invoked when an adapter dump has run out
 * of processing time.
 *
 * Return value:
 * 	IPR_RC_JOB_CONTINUE
 **/
static int ipr_reset_wait_for_dump(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;

	if (ioa_cfg->sdt_state == GET_DUMP)
		ioa_cfg->sdt_state = ABORT_DUMP;

	ipr_cmd->job_step = ipr_reset_alert;

	return IPR_RC_JOB_CONTINUE;
}

/**
 * ipr_unit_check_no_data - Log a unit check/no data error log
 * @ioa_cfg:		ioa config struct
 *
 * Logs an error indicating the adapter unit checked, but for some
 * reason, we were unable to fetch the unit check buffer.
 *
 * Return value:
 * 	nothing
 **/
static void ipr_unit_check_no_data(struct ipr_ioa_cfg *ioa_cfg)
{
	ioa_cfg->errors_logged++;
	dev_err(&ioa_cfg->pdev->dev, "IOA unit check with no data\n");
}

/**
 * ipr_get_unit_check_buffer - Get the unit check buffer from the IOA
 * @ioa_cfg:		ioa config struct
 *
 * Fetches the unit check buffer from the adapter by clocking the data
 * through the mailbox register.
 *
 * Return value:
 * 	nothing
 **/
static void ipr_get_unit_check_buffer(struct ipr_ioa_cfg *ioa_cfg)
{
	unsigned long mailbox;
	struct ipr_hostrcb *hostrcb;
	struct ipr_uc_sdt sdt;
	int rc, length;

	mailbox = readl(ioa_cfg->ioa_mailbox);

	if (!ipr_sdt_is_fmt2(mailbox)) {
		ipr_unit_check_no_data(ioa_cfg);
		return;
	}

	memset(&sdt, 0, sizeof(struct ipr_uc_sdt));
	rc = ipr_get_ldump_data_section(ioa_cfg, mailbox, (u32 *) &sdt,
					(sizeof(struct ipr_uc_sdt)) / sizeof(u32));

	if (rc || (be32_to_cpu(sdt.hdr.state) != IPR_FMT2_SDT_READY_TO_USE) ||
	    !(sdt.entry[0].flags & IPR_SDT_VALID_ENTRY)) {
		ipr_unit_check_no_data(ioa_cfg);
		return;
	}

	/* Find length of the first sdt entry (UC buffer) */
	length = (be32_to_cpu(sdt.entry[0].end_offset) -
		  be32_to_cpu(sdt.entry[0].bar_str_offset)) & IPR_FMT2_MBX_ADDR_MASK;

	hostrcb = list_entry(ioa_cfg->hostrcb_free_q.next,
			     struct ipr_hostrcb, queue);
	list_del(&hostrcb->queue);
	memset(&hostrcb->hcam, 0, sizeof(hostrcb->hcam));

	rc = ipr_get_ldump_data_section(ioa_cfg,
					be32_to_cpu(sdt.entry[0].bar_str_offset),
					(u32 *)&hostrcb->hcam,
					min(length, (int)sizeof(hostrcb->hcam)) / sizeof(u32));

	if (!rc)
		ipr_handle_log_data(ioa_cfg, hostrcb);
	else
		ipr_unit_check_no_data(ioa_cfg);

	list_add_tail(&hostrcb->queue, &ioa_cfg->hostrcb_free_q);
}

/**
 * ipr_reset_restore_cfg_space - Restore PCI config space.
 * @ipr_cmd:	ipr command struct
 *
 * Description: This function restores the saved PCI config space of
 * the adapter, fails all outstanding ops back to the callers, and
 * fetches the dump/unit check if applicable to this reset.
 *
 * Return value:
 * 	IPR_RC_JOB_CONTINUE / IPR_RC_JOB_RETURN
 **/
static int ipr_reset_restore_cfg_space(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	int rc;

	ENTER;
	rc = pci_restore_state(ioa_cfg->pdev, ioa_cfg->pci_cfg_buf);

	if (rc != PCIBIOS_SUCCESSFUL) {
		ipr_cmd->ioasa.ioasc = cpu_to_be32(IPR_IOASC_PCI_ACCESS_ERROR);
		return IPR_RC_JOB_CONTINUE;
	}

	if (ipr_set_pcix_cmd_reg(ioa_cfg)) {
		ipr_cmd->ioasa.ioasc = cpu_to_be32(IPR_IOASC_PCI_ACCESS_ERROR);
		return IPR_RC_JOB_CONTINUE;
	}

	ipr_fail_all_ops(ioa_cfg);

	if (ioa_cfg->ioa_unit_checked) {
		ioa_cfg->ioa_unit_checked = 0;
		ipr_get_unit_check_buffer(ioa_cfg);
		ipr_cmd->job_step = ipr_reset_alert;
		ipr_reset_start_timer(ipr_cmd, 0);
		return IPR_RC_JOB_RETURN;
	}

	if (ioa_cfg->in_ioa_bringdown) {
		ipr_cmd->job_step = ipr_ioa_bringdown_done;
	} else {
		ipr_cmd->job_step = ipr_reset_enable_ioa;

		if (GET_DUMP == ioa_cfg->sdt_state) {
			ipr_reset_start_timer(ipr_cmd, IPR_DUMP_TIMEOUT);
			ipr_cmd->job_step = ipr_reset_wait_for_dump;
			schedule_work(&ioa_cfg->work_q);
			return IPR_RC_JOB_RETURN;
		}
	}

	ENTER;
	return IPR_RC_JOB_CONTINUE;
}

/**
 * ipr_reset_start_bist - Run BIST on the adapter.
 * @ipr_cmd:	ipr command struct
 *
 * Description: This function runs BIST on the adapter, then delays 2 seconds.
 *
 * Return value:
 * 	IPR_RC_JOB_CONTINUE / IPR_RC_JOB_RETURN
 **/
static int ipr_reset_start_bist(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	int rc;

	ENTER;
	rc = pci_write_config_byte(ioa_cfg->pdev, PCI_BIST, PCI_BIST_START);

	if (rc != PCIBIOS_SUCCESSFUL) {
		ipr_cmd->ioasa.ioasc = cpu_to_be32(IPR_IOASC_PCI_ACCESS_ERROR);
		rc = IPR_RC_JOB_CONTINUE;
	} else {
		ipr_cmd->job_step = ipr_reset_restore_cfg_space;
		ipr_reset_start_timer(ipr_cmd, IPR_WAIT_FOR_BIST_TIMEOUT);
		rc = IPR_RC_JOB_RETURN;
	}

	LEAVE;
	return rc;
}

/**
 * ipr_reset_allowed - Query whether or not IOA can be reset
 * @ioa_cfg:	ioa config struct
 *
 * Return value:
 * 	0 if reset not allowed / non-zero if reset is allowed
 **/
static int ipr_reset_allowed(struct ipr_ioa_cfg *ioa_cfg)
{
	volatile u32 temp_reg;

	temp_reg = readl(ioa_cfg->regs.sense_interrupt_reg);
	return ((temp_reg & IPR_PCII_CRITICAL_OPERATION) == 0);
}

/**
 * ipr_reset_wait_to_start_bist - Wait for permission to reset IOA.
 * @ipr_cmd:	ipr command struct
 *
 * Description: This function waits for adapter permission to run BIST,
 * then runs BIST. If the adapter does not give permission after a
 * reasonable time, we will reset the adapter anyway. The impact of
 * resetting the adapter without warning the adapter is the risk of
 * losing the persistent error log on the adapter. If the adapter is
 * reset while it is writing to the flash on the adapter, the flash
 * segment will have bad ECC and be zeroed.
 *
 * Return value:
 * 	IPR_RC_JOB_CONTINUE / IPR_RC_JOB_RETURN
 **/
static int ipr_reset_wait_to_start_bist(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	int rc = IPR_RC_JOB_RETURN;

	if (!ipr_reset_allowed(ioa_cfg) && ipr_cmd->u.time_left) {
		ipr_cmd->u.time_left -= IPR_CHECK_FOR_RESET_TIMEOUT;
		ipr_reset_start_timer(ipr_cmd, IPR_CHECK_FOR_RESET_TIMEOUT);
	} else {
		ipr_cmd->job_step = ipr_reset_start_bist;
		rc = IPR_RC_JOB_CONTINUE;
	}

	return rc;
}

/**
 * ipr_reset_alert_part2 - Alert the adapter of a pending reset
 * @ipr_cmd:	ipr command struct
 *
 * Description: This function alerts the adapter that it will be reset.
 * If memory space is not currently enabled, proceed directly
 * to running BIST on the adapter. The timer must always be started
 * so we guarantee we do not run BIST from ipr_isr.
 *
 * Return value:
 * 	IPR_RC_JOB_RETURN
 **/
static int ipr_reset_alert(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	u16 cmd_reg;
	int rc;

	ENTER;
	rc = pci_read_config_word(ioa_cfg->pdev, PCI_COMMAND, &cmd_reg);

	if ((rc == PCIBIOS_SUCCESSFUL) && (cmd_reg & PCI_COMMAND_MEMORY)) {
		ipr_mask_and_clear_interrupts(ioa_cfg, ~0);
		writel(IPR_UPROCI_RESET_ALERT, ioa_cfg->regs.set_uproc_interrupt_reg);
		ipr_cmd->job_step = ipr_reset_wait_to_start_bist;
	} else {
		ipr_cmd->job_step = ipr_reset_start_bist;
	}

	ipr_cmd->u.time_left = IPR_WAIT_FOR_RESET_TIMEOUT;
	ipr_reset_start_timer(ipr_cmd, IPR_CHECK_FOR_RESET_TIMEOUT);

	LEAVE;
	return IPR_RC_JOB_RETURN;
}

/**
 * ipr_reset_ucode_download_done - Microcode download completion
 * @ipr_cmd:	ipr command struct
 *
 * Description: This function unmaps the microcode download buffer.
 *
 * Return value:
 * 	IPR_RC_JOB_CONTINUE
 **/
static int ipr_reset_ucode_download_done(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	struct ipr_sglist *sglist = ioa_cfg->ucode_sglist;

	pci_unmap_sg(ioa_cfg->pdev, sglist->scatterlist,
		     sglist->num_sg, DMA_TO_DEVICE);

	ipr_cmd->job_step = ipr_reset_alert;
	return IPR_RC_JOB_CONTINUE;
}

/**
 * ipr_reset_ucode_download - Download microcode to the adapter
 * @ipr_cmd:	ipr command struct
 *
 * Description: This function checks to see if it there is microcode
 * to download to the adapter. If there is, a download is performed.
 *
 * Return value:
 * 	IPR_RC_JOB_CONTINUE / IPR_RC_JOB_RETURN
 **/
static int ipr_reset_ucode_download(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	struct ipr_sglist *sglist = ioa_cfg->ucode_sglist;

	ENTER;
	ipr_cmd->job_step = ipr_reset_alert;

	if (!sglist)
		return IPR_RC_JOB_CONTINUE;

	ipr_cmd->ioarcb.res_handle = cpu_to_be32(IPR_IOA_RES_HANDLE);
	ipr_cmd->ioarcb.cmd_pkt.request_type = IPR_RQTYPE_SCSICDB;
	ipr_cmd->ioarcb.cmd_pkt.cdb[0] = WRITE_BUFFER;
	ipr_cmd->ioarcb.cmd_pkt.cdb[1] = IPR_WR_BUF_DOWNLOAD_AND_SAVE;
	ipr_cmd->ioarcb.cmd_pkt.cdb[6] = (sglist->buffer_len & 0xff0000) >> 16;
	ipr_cmd->ioarcb.cmd_pkt.cdb[7] = (sglist->buffer_len & 0x00ff00) >> 8;
	ipr_cmd->ioarcb.cmd_pkt.cdb[8] = sglist->buffer_len & 0x0000ff;

	if (ipr_map_ucode_buffer(ipr_cmd, sglist, sglist->buffer_len)) {
		dev_err(&ioa_cfg->pdev->dev,
			"Failed to map microcode download buffer\n");
		return IPR_RC_JOB_CONTINUE;
	}

	ipr_cmd->job_step = ipr_reset_ucode_download_done;

	ipr_do_req(ipr_cmd, ipr_reset_ioa_job, ipr_timeout,
		   IPR_WRITE_BUFFER_TIMEOUT);

	LEAVE;
	return IPR_RC_JOB_RETURN;
}

/**
 * ipr_reset_shutdown_ioa - Shutdown the adapter
 * @ipr_cmd:	ipr command struct
 *
 * Description: This function issues an adapter shutdown of the
 * specified type to the specified adapter as part of the
 * adapter reset job.
 *
 * Return value:
 * 	IPR_RC_JOB_CONTINUE / IPR_RC_JOB_RETURN
 **/
static int ipr_reset_shutdown_ioa(struct ipr_cmnd *ipr_cmd)
{
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;
	enum ipr_shutdown_type shutdown_type = ipr_cmd->u.shutdown_type;
	unsigned long timeout;
	int rc = IPR_RC_JOB_CONTINUE;

	ENTER;
	if (shutdown_type != IPR_SHUTDOWN_NONE && !ioa_cfg->ioa_is_dead) {
		ipr_cmd->ioarcb.res_handle = cpu_to_be32(IPR_IOA_RES_HANDLE);
		ipr_cmd->ioarcb.cmd_pkt.request_type = IPR_RQTYPE_IOACMD;
		ipr_cmd->ioarcb.cmd_pkt.cdb[0] = IPR_IOA_SHUTDOWN;
		ipr_cmd->ioarcb.cmd_pkt.cdb[1] = shutdown_type;

		if (shutdown_type == IPR_SHUTDOWN_ABBREV)
			timeout = IPR_ABBREV_SHUTDOWN_TIMEOUT;
		else if (shutdown_type == IPR_SHUTDOWN_PREPARE_FOR_NORMAL)
			timeout = IPR_INTERNAL_TIMEOUT;
		else
			timeout = IPR_SHUTDOWN_TIMEOUT;

		ipr_do_req(ipr_cmd, ipr_reset_ioa_job, ipr_timeout, timeout);

		rc = IPR_RC_JOB_RETURN;
		ipr_cmd->job_step = ipr_reset_ucode_download;
	} else
		ipr_cmd->job_step = ipr_reset_alert;

	LEAVE;
	return rc;
}

/**
 * ipr_reset_ioa_job - Adapter reset job
 * @ipr_cmd:	ipr command struct
 *
 * Description: This function is the job router for the adapter reset job.
 *
 * Return value:
 * 	none
 **/
static void ipr_reset_ioa_job(struct ipr_cmnd *ipr_cmd)
{
	u32 rc, ioasc;
	unsigned long scratch = ipr_cmd->u.scratch;
	struct ipr_ioa_cfg *ioa_cfg = ipr_cmd->ioa_cfg;

	do {
		ioasc = be32_to_cpu(ipr_cmd->ioasa.ioasc);

		if (ioa_cfg->reset_cmd != ipr_cmd) {
			/*
			 * We are doing nested adapter resets and this is
			 * not the current reset job.
			 */
			list_add_tail(&ipr_cmd->queue, &ioa_cfg->free_q);
			return;
		}

		if (IPR_IOASC_SENSE_KEY(ioasc)) {
			dev_err(&ioa_cfg->pdev->dev,
				"0x%02X failed with IOASC: 0x%08X\n",
				ipr_cmd->ioarcb.cmd_pkt.cdb[0], ioasc);

			ipr_initiate_ioa_reset(ioa_cfg, IPR_SHUTDOWN_NONE);
			list_add_tail(&ipr_cmd->queue, &ioa_cfg->free_q);
			return;
		}

		ipr_reinit_ipr_cmnd(ipr_cmd);
		ipr_cmd->u.scratch = scratch;
		rc = ipr_cmd->job_step(ipr_cmd);
	} while(rc == IPR_RC_JOB_CONTINUE);
}

/**
 * _ipr_initiate_ioa_reset - Initiate an adapter reset
 * @ioa_cfg:		ioa config struct
 * @job_step:		first job step of reset job
 * @shutdown_type:	shutdown type
 *
 * Description: This function will initiate the reset of the given adapter
 * starting at the selected job step.
 * If the caller needs to wait on the completion of the reset,
 * the caller must sleep on the reset_wait_q.
 *
 * Return value:
 * 	none
 **/
static void _ipr_initiate_ioa_reset(struct ipr_ioa_cfg *ioa_cfg,
				    int (*job_step) (struct ipr_cmnd *),
				    enum ipr_shutdown_type shutdown_type)
{
	struct ipr_cmnd *ipr_cmd;

	ioa_cfg->in_reset_reload = 1;
	ioa_cfg->allow_cmds = 0;
	scsi_block_requests(ioa_cfg->host);

	ipr_cmd = ipr_get_free_ipr_cmnd(ioa_cfg);
	ioa_cfg->reset_cmd = ipr_cmd;
	ipr_cmd->job_step = job_step;
	ipr_cmd->u.shutdown_type = shutdown_type;

	ipr_reset_ioa_job(ipr_cmd);
}

/**
 * ipr_initiate_ioa_reset - Initiate an adapter reset
 * @ioa_cfg:		ioa config struct
 * @shutdown_type:	shutdown type
 *
 * Description: This function will initiate the reset of the given adapter.
 * If the caller needs to wait on the completion of the reset,
 * the caller must sleep on the reset_wait_q.
 *
 * Return value:
 * 	none
 **/
static void ipr_initiate_ioa_reset(struct ipr_ioa_cfg *ioa_cfg,
				   enum ipr_shutdown_type shutdown_type)
{
	if (ioa_cfg->ioa_is_dead)
		return;

	if (ioa_cfg->in_reset_reload && ioa_cfg->sdt_state == GET_DUMP)
		ioa_cfg->sdt_state = ABORT_DUMP;

	if (ioa_cfg->reset_retries++ > IPR_NUM_RESET_RELOAD_RETRIES) {
		dev_err(&ioa_cfg->pdev->dev,
			"IOA taken offline - error recovery failed\n");

		ioa_cfg->reset_retries = 0;
		ioa_cfg->ioa_is_dead = 1;

		if (ioa_cfg->in_ioa_bringdown) {
			ioa_cfg->reset_cmd = NULL;
			ioa_cfg->in_reset_reload = 0;
			ipr_fail_all_ops(ioa_cfg);
			wake_up_all(&ioa_cfg->reset_wait_q);

			spin_unlock_irq(ioa_cfg->host->host_lock);
			scsi_unblock_requests(ioa_cfg->host);
			spin_lock_irq(ioa_cfg->host->host_lock);
			return;
		} else {
			ioa_cfg->in_ioa_bringdown = 1;
			shutdown_type = IPR_SHUTDOWN_NONE;
		}
	}

	_ipr_initiate_ioa_reset(ioa_cfg, ipr_reset_shutdown_ioa,
				shutdown_type);
}

/**
 * ipr_probe_ioa_part2 - Initializes IOAs found in ipr_probe_ioa(..)
 * @ioa_cfg:	ioa cfg struct
 *
 * Description: This is the second phase of adapter intialization
 * This function takes care of initilizing the adapter to the point
 * where it can accept new commands.

 * Return value:
 * 	0 on sucess / -EIO on failure
 **/
static int __devinit ipr_probe_ioa_part2(struct ipr_ioa_cfg *ioa_cfg)
{
	int rc = 0;
	unsigned long host_lock_flags = 0;

	ENTER;
	spin_lock_irqsave(ioa_cfg->host->host_lock, host_lock_flags);
	dev_dbg(&ioa_cfg->pdev->dev, "ioa_cfg adx: 0x%p\n", ioa_cfg);
	_ipr_initiate_ioa_reset(ioa_cfg, ipr_reset_enable_ioa, IPR_SHUTDOWN_NONE);

	spin_unlock_irqrestore(ioa_cfg->host->host_lock, host_lock_flags);
	wait_event(ioa_cfg->reset_wait_q, !ioa_cfg->in_reset_reload);
	spin_lock_irqsave(ioa_cfg->host->host_lock, host_lock_flags);

	if (ioa_cfg->ioa_is_dead) {
		rc = -EIO;
	} else if (ipr_invalid_adapter(ioa_cfg)) {
		if (!ipr_testmode)
			rc = -EIO;

		dev_err(&ioa_cfg->pdev->dev,
			"Adapter not supported in this hardware configuration.\n");
	}

	spin_unlock_irqrestore(ioa_cfg->host->host_lock, host_lock_flags);

	LEAVE;
	return rc;
}

/**
 * ipr_free_cmd_blks - Frees command blocks allocated for an adapter
 * @ioa_cfg:	ioa config struct
 *
 * Return value:
 * 	none
 **/
static void ipr_free_cmd_blks(struct ipr_ioa_cfg *ioa_cfg)
{
	int i;

	for (i = 0; i < IPR_NUM_CMD_BLKS; i++) {
		if (ioa_cfg->ipr_cmnd_list[i])
			pci_pool_free(ioa_cfg->ipr_cmd_pool,
				      ioa_cfg->ipr_cmnd_list[i],
				      ioa_cfg->ipr_cmnd_list_dma[i]);

		ioa_cfg->ipr_cmnd_list[i] = NULL;
	}

	if (ioa_cfg->ipr_cmd_pool)
		pci_pool_destroy (ioa_cfg->ipr_cmd_pool);

	ioa_cfg->ipr_cmd_pool = NULL;
}

/**
 * ipr_free_mem - Frees memory allocated for an adapter
 * @ioa_cfg:	ioa cfg struct
 *
 * Return value:
 * 	nothing
 **/
static void ipr_free_mem(struct ipr_ioa_cfg *ioa_cfg)
{
	int i;

	kfree(ioa_cfg->res_entries);
	pci_free_consistent(ioa_cfg->pdev, sizeof(struct ipr_misc_cbs),
			    ioa_cfg->vpd_cbs, ioa_cfg->vpd_cbs_dma);
	ipr_free_cmd_blks(ioa_cfg);
	pci_free_consistent(ioa_cfg->pdev, sizeof(u32) * IPR_NUM_CMD_BLKS,
			    ioa_cfg->host_rrq, ioa_cfg->host_rrq_dma);
	pci_free_consistent(ioa_cfg->pdev, sizeof(struct ipr_config_table),
			    ioa_cfg->cfg_table,
			    ioa_cfg->cfg_table_dma);

	for (i = 0; i < IPR_NUM_HCAMS; i++) {
		pci_free_consistent(ioa_cfg->pdev,
				    sizeof(struct ipr_hostrcb),
				    ioa_cfg->hostrcb[i],
				    ioa_cfg->hostrcb_dma[i]);
	}

	ipr_free_dump(ioa_cfg);
	kfree(ioa_cfg->saved_mode_pages);
	kfree(ioa_cfg->trace);
}

/**
 * ipr_free_all_resources - Free all allocated resources for an adapter.
 * @ipr_cmd:	ipr command struct
 *
 * This function frees all allocated resources for the
 * specified adapter.
 *
 * Return value:
 * 	none
 **/
static void ipr_free_all_resources(struct ipr_ioa_cfg *ioa_cfg)
{
	ENTER;
	free_irq(ioa_cfg->pdev->irq, ioa_cfg);
	iounmap((void *) ioa_cfg->hdw_dma_regs);
	release_mem_region(ioa_cfg->hdw_dma_regs_pci,
			   pci_resource_len(ioa_cfg->pdev, 0));
	ipr_free_mem(ioa_cfg);
	scsi_host_put(ioa_cfg->host);
	LEAVE;
}

/**
 * ipr_alloc_cmd_blks - Allocate command blocks for an adapter
 * @ioa_cfg:	ioa config struct
 *
 * Return value:
 * 	0 on success / -ENOMEM on allocation failure
 **/
static int __devinit ipr_alloc_cmd_blks(struct ipr_ioa_cfg *ioa_cfg)
{
	struct ipr_cmnd *ipr_cmd;
	struct ipr_ioarcb *ioarcb;
	u32 dma_addr;
	int i;

	ioa_cfg->ipr_cmd_pool = pci_pool_create (IPR_NAME, ioa_cfg->pdev,
						 sizeof(struct ipr_cmnd), 8, 0);

	if (!ioa_cfg->ipr_cmd_pool)
		return -ENOMEM;

	for (i = 0; i < IPR_NUM_CMD_BLKS; i++) {
		ipr_cmd = pci_pool_alloc (ioa_cfg->ipr_cmd_pool, SLAB_KERNEL, &dma_addr);

		if (!ipr_cmd) {
			ipr_free_cmd_blks(ioa_cfg);
			return -ENOMEM;
		}

		memset(ipr_cmd, 0, sizeof(*ipr_cmd));
		ioa_cfg->ipr_cmnd_list[i] = ipr_cmd;
		ioa_cfg->ipr_cmnd_list_dma[i] = dma_addr;

		ioarcb = &ipr_cmd->ioarcb;
		ioarcb->ioarcb_host_pci_addr = cpu_to_be32(dma_addr);
		ioarcb->host_response_handle = cpu_to_be32(i << 2);
		ioarcb->write_ioadl_addr =
			cpu_to_be32(dma_addr + offsetof(struct ipr_cmnd, ioadl));
		ioarcb->read_ioadl_addr = ioarcb->write_ioadl_addr;
		ioarcb->ioasa_host_pci_addr =
			cpu_to_be32(dma_addr + offsetof(struct ipr_cmnd, ioasa));
		ioarcb->ioasa_len = cpu_to_be16(sizeof(struct ipr_ioasa));
		ipr_cmd->cmd_index = i;
		ipr_cmd->ioa_cfg = ioa_cfg;
		ipr_cmd->sense_buffer_dma = dma_addr +
			offsetof(struct ipr_cmnd, sense_buffer);

		list_add_tail(&ipr_cmd->queue, &ioa_cfg->free_q);
	}

	return 0;
}

/**
 * ipr_alloc_mem - Allocate memory for an adapter
 * @ioa_cfg:	ioa config struct
 *
 * Return value:
 * 	0 on success / non-zero for error
 **/
static int __devinit ipr_alloc_mem(struct ipr_ioa_cfg *ioa_cfg)
{
	int i;

	ENTER;
	ioa_cfg->res_entries = kmalloc(sizeof(struct ipr_resource_entry) *
				       IPR_MAX_PHYSICAL_DEVS, GFP_KERNEL);

	if (!ioa_cfg->res_entries)
		goto cleanup;

	memset(ioa_cfg->res_entries, 0,
	       sizeof(struct ipr_resource_entry) * IPR_MAX_PHYSICAL_DEVS);

	for (i = 0; i < IPR_MAX_PHYSICAL_DEVS; i++)
		list_add_tail(&ioa_cfg->res_entries[i].queue, &ioa_cfg->free_res_q);

	ioa_cfg->vpd_cbs = pci_alloc_consistent(ioa_cfg->pdev,
						sizeof(struct ipr_misc_cbs),
						&ioa_cfg->vpd_cbs_dma);

	if (!ioa_cfg->vpd_cbs)
		goto cleanup;

	if (ipr_alloc_cmd_blks(ioa_cfg))
		goto cleanup;

	ioa_cfg->host_rrq = pci_alloc_consistent(ioa_cfg->pdev,
						 sizeof(u32) * IPR_NUM_CMD_BLKS,
						 &ioa_cfg->host_rrq_dma);

	if (!ioa_cfg->host_rrq)
		goto cleanup;

	ioa_cfg->cfg_table = pci_alloc_consistent(ioa_cfg->pdev,
						  sizeof(struct ipr_config_table),
						  &ioa_cfg->cfg_table_dma);

	if (!ioa_cfg->cfg_table)
		goto cleanup;

	for (i = 0; i < IPR_NUM_HCAMS; i++) {
		ioa_cfg->hostrcb[i] = pci_alloc_consistent(ioa_cfg->pdev,
							   sizeof(struct ipr_hostrcb),
							   &ioa_cfg->hostrcb_dma[i]);

		if (!ioa_cfg->hostrcb[i])
			goto cleanup;

		memset(ioa_cfg->hostrcb[i], 0, sizeof(struct ipr_hostrcb));
		ioa_cfg->hostrcb[i]->hostrcb_dma =
			ioa_cfg->hostrcb_dma[i] + offsetof(struct ipr_hostrcb, hcam);
		list_add_tail(&ioa_cfg->hostrcb[i]->queue, &ioa_cfg->hostrcb_free_q);
	}

	ioa_cfg->trace = kmalloc(sizeof(struct ipr_trace_entry) *
				 IPR_NUM_TRACE_ENTRIES, GFP_KERNEL);

	if (!ioa_cfg->trace)
		goto cleanup;

	memset(ioa_cfg->trace, 0,
	       sizeof(struct ipr_trace_entry) * IPR_NUM_TRACE_ENTRIES);

	LEAVE;
	return 0;

cleanup:
	ipr_free_mem(ioa_cfg);

	LEAVE;
	return -ENOMEM;
}

/**
 * ipr_initialize_bus_attr - Initialize SCSI bus attributes to default values
 * @ioa_cfg:	ioa config struct
 *
 * Return value:
 * 	none
 **/
static void __devinit ipr_initialize_bus_attr(struct ipr_ioa_cfg *ioa_cfg)
{
	int i;

	for (i = 0; i < IPR_MAX_NUM_BUSES; i++) {
		ioa_cfg->bus_attr[i].bus = i;
		ioa_cfg->bus_attr[i].qas_enabled = 0;
		ioa_cfg->bus_attr[i].bus_width = IPR_DEFAULT_BUS_WIDTH;
		if (ipr_max_speed < ARRAY_SIZE(ipr_max_bus_speeds))
			ioa_cfg->bus_attr[i].max_xfer_rate = ipr_max_bus_speeds[ipr_max_speed];
		else
			ioa_cfg->bus_attr[i].max_xfer_rate = IPR_U160_SCSI_RATE;
	}
}

/**
 * ipr_init_ioa_cfg - Initialize IOA config struct
 * @ioa_cfg:	ioa config struct
 * @host:		scsi host struct
 * @pdev:		PCI dev struct
 *
 * Return value:
 * 	none
 **/
static void __devinit ipr_init_ioa_cfg(struct ipr_ioa_cfg *ioa_cfg,
				       struct Scsi_Host *host, struct pci_dev *pdev)
{
	ioa_cfg->host = host;
	ioa_cfg->pdev = pdev;
	ioa_cfg->log_level = ipr_log_level;
	sprintf(ioa_cfg->eye_catcher, IPR_EYECATCHER);
	sprintf(ioa_cfg->trace_start, IPR_TRACE_START_LABEL);
	sprintf(ioa_cfg->ipr_free_label, IPR_FREEQ_LABEL);
	sprintf(ioa_cfg->ipr_pending_label, IPR_PENDQ_LABEL);
	sprintf(ioa_cfg->cfg_table_start, IPR_CFG_TBL_START);
	sprintf(ioa_cfg->resource_table_label, IPR_RES_TABLE_LABEL);
	sprintf(ioa_cfg->ipr_hcam_label, IPR_HCAM_LABEL);
	sprintf(ioa_cfg->ipr_cmd_label, IPR_CMD_LABEL);

	INIT_LIST_HEAD(&ioa_cfg->free_q);
	INIT_LIST_HEAD(&ioa_cfg->pending_q);
	INIT_LIST_HEAD(&ioa_cfg->hostrcb_free_q);
	INIT_LIST_HEAD(&ioa_cfg->hostrcb_pending_q);
	INIT_LIST_HEAD(&ioa_cfg->free_res_q);
	INIT_LIST_HEAD(&ioa_cfg->used_res_q);
	INIT_WORK(&ioa_cfg->work_q, ipr_worker_thread, ioa_cfg);
	init_waitqueue_head(&ioa_cfg->reset_wait_q);
	ioa_cfg->sdt_state = INACTIVE;

	ipr_initialize_bus_attr(ioa_cfg);

	host->max_id = IPR_MAX_NUM_TARGETS_PER_BUS;
	host->max_lun = IPR_MAX_NUM_LUNS_PER_TARGET;
	host->max_channel = IPR_MAX_BUS_TO_SCAN;
	host->unique_id = host->host_no;
	host->max_cmd_len = IPR_MAX_CDB_LEN;
	pci_set_drvdata(pdev, ioa_cfg);

	memcpy(&ioa_cfg->regs, &ioa_cfg->chip_cfg->regs, sizeof(ioa_cfg->regs));

	ioa_cfg->regs.set_interrupt_mask_reg += ioa_cfg->hdw_dma_regs;
	ioa_cfg->regs.clr_interrupt_mask_reg += ioa_cfg->hdw_dma_regs;
	ioa_cfg->regs.sense_interrupt_mask_reg += ioa_cfg->hdw_dma_regs;
	ioa_cfg->regs.clr_interrupt_reg += ioa_cfg->hdw_dma_regs;
	ioa_cfg->regs.sense_interrupt_reg += ioa_cfg->hdw_dma_regs;
	ioa_cfg->regs.ioarrin_reg += ioa_cfg->hdw_dma_regs;
	ioa_cfg->regs.sense_uproc_interrupt_reg += ioa_cfg->hdw_dma_regs;
	ioa_cfg->regs.set_uproc_interrupt_reg += ioa_cfg->hdw_dma_regs;
	ioa_cfg->regs.clr_uproc_interrupt_reg += ioa_cfg->hdw_dma_regs;
}

/**
 * ipr_probe_ioa - Allocates memory and does first stage of initialization
 * @pdev:		PCI device struct
 * @dev_id:		PCI device id struct
 *
 * Return value:
 * 	0 on success / non-zero on failure
 **/
static int __devinit ipr_probe_ioa(struct pci_dev *pdev,
				   const struct pci_device_id *dev_id)
{
	struct ipr_ioa_cfg *ioa_cfg;
	struct Scsi_Host *host;
	unsigned long ipr_regs, ipr_regs_pci;
	u32 rc = PCIBIOS_SUCCESSFUL;

	ENTER;

	if ((rc = pci_enable_device(pdev))) {
		dev_err(&pdev->dev, "Cannot enable adapter\n");
		return rc;
	}

	dev_info(&pdev->dev, "Found IOA with IRQ: %d\n", pdev->irq);

	host = scsi_host_alloc(&driver_template, sizeof(*ioa_cfg));

	if (!host) {
		dev_err(&pdev->dev, "call to scsi_host_alloc failed!\n");
		return -ENOMEM;
	}

	ioa_cfg = (struct ipr_ioa_cfg *)host->hostdata;
	memset(ioa_cfg, 0, sizeof(struct ipr_ioa_cfg));

	ioa_cfg->chip_cfg = (const struct ipr_chip_cfg_t *)dev_id->driver_data;

	ipr_regs_pci = pci_resource_start(pdev, 0);

	if (!request_mem_region(ipr_regs_pci,
				pci_resource_len(pdev, 0), IPR_NAME)) {
		dev_err(&pdev->dev,
			"Couldn't register memory range of registers\n");
		scsi_host_put(host);
		return -ENOMEM;
	}

	ipr_regs = (unsigned long)ioremap(ipr_regs_pci,
					  pci_resource_len(pdev, 0));

	if (!ipr_regs) {
		dev_err(&pdev->dev,
			"Couldn't map memory range of registers\n");
		release_mem_region(ipr_regs_pci, pci_resource_len(pdev, 0));
		scsi_host_put(host);
		return -ENOMEM;
	}

	ioa_cfg->hdw_dma_regs = ipr_regs;
	ioa_cfg->hdw_dma_regs_pci = ipr_regs_pci;
	ioa_cfg->ioa_mailbox = ioa_cfg->chip_cfg->mailbox + ipr_regs;

	ipr_init_ioa_cfg(ioa_cfg, host, pdev);

	pci_set_master(pdev);
	rc = pci_set_dma_mask(pdev, 0xffffffff);

	if (rc != PCIBIOS_SUCCESSFUL) {
		dev_err(&pdev->dev, "Failed to set PCI DMA mask\n");
		rc = -EIO;
		goto cleanup_nomem;
	}

	rc = pci_write_config_byte(pdev, PCI_CACHE_LINE_SIZE,
				   ioa_cfg->chip_cfg->cache_line_size);

	if (rc != PCIBIOS_SUCCESSFUL) {
		dev_err(&pdev->dev, "Write of cache line size failed\n");
		rc = -EIO;
		goto cleanup_nomem;
	}

	/* Save away PCI config space for use following IOA reset */
	rc = pci_save_state(pdev, ioa_cfg->pci_cfg_buf);

	if (rc != PCIBIOS_SUCCESSFUL) {
		dev_err(&pdev->dev, "Failed to save PCI config space\n");
		rc = -EIO;
		goto cleanup_nomem;
	}

	if ((rc = ipr_save_pcix_cmd_reg(ioa_cfg)))
		goto cleanup_nomem;

	if ((rc = ipr_set_pcix_cmd_reg(ioa_cfg)))
		goto cleanup_nomem;

	if ((rc = ipr_alloc_mem(ioa_cfg)))
		goto cleanup;

	ipr_mask_and_clear_interrupts(ioa_cfg, ~IPR_PCII_IOA_TRANS_TO_OPER);
	rc = request_irq(pdev->irq, ipr_isr, SA_SHIRQ, IPR_NAME, ioa_cfg);

	if (rc) {
		dev_err(&pdev->dev, "Couldn't register IRQ %d! rc=%d\n",
			pdev->irq, rc);
		goto cleanup_nolog;
	}

	spin_lock(&ipr_driver_lock);
	list_add_tail(&ioa_cfg->queue, &ipr_ioa_head);
	spin_unlock(&ipr_driver_lock);

	LEAVE;
	return 0;

cleanup:
	dev_err(&pdev->dev, "Couldn't allocate enough memory for device driver!\n");
cleanup_nolog:
	ipr_free_mem(ioa_cfg);
cleanup_nomem:
	iounmap((void *) ipr_regs);
	release_mem_region(ipr_regs_pci, pci_resource_len(pdev, 0));
	scsi_host_put(host);

	return rc;
}

/**
 * ipr_scan_vsets - Scans for VSET devices
 * @ioa_cfg:	ioa config struct
 *
 * Description: Since the VSET resources do not follow SAM in that we can have
 * sparse LUNs with no LUN 0, we have to scan for these ourselves.
 *
 * Return value:
 * 	none
 **/
static void ipr_scan_vsets(struct ipr_ioa_cfg *ioa_cfg)
{
	int target, lun;

	for (target = 0; target < IPR_MAX_NUM_TARGETS_PER_BUS; target++)
		for (lun = 0; lun < IPR_MAX_NUM_VSET_LUNS_PER_TARGET; lun++ )
			scsi_add_device(ioa_cfg->host, IPR_VSET_BUS, target, lun);
}

/**
 * ipr_initiate_ioa_bringdown - Bring down an adapter
 * @ioa_cfg:		ioa config struct
 * @shutdown_type:	shutdown type
 *
 * Description: This function will initiate bringing down the adapter.
 * This consists of issuing an IOA shutdown to the adapter
 * to flush the cache, and running BIST.
 * If the caller needs to wait on the completion of the reset,
 * the caller must sleep on the reset_wait_q.
 *
 * Return value:
 * 	none
 **/
static void ipr_initiate_ioa_bringdown(struct ipr_ioa_cfg *ioa_cfg,
				       enum ipr_shutdown_type shutdown_type)
{
	ENTER;
	if (ioa_cfg->sdt_state == WAIT_FOR_DUMP)
		ioa_cfg->sdt_state = ABORT_DUMP;
	ioa_cfg->reset_retries = 0;
	ioa_cfg->in_ioa_bringdown = 1;
	ipr_initiate_ioa_reset(ioa_cfg, shutdown_type);
	LEAVE;
}

/**
 * __ipr_remove - Remove a single adapter
 * @pdev:	pci device struct
 *
 * Adapter hot plug remove entry point.
 *
 * Return value:
 * 	none
 **/
static void __ipr_remove(struct pci_dev *pdev)
{
	unsigned long host_lock_flags = 0;
	struct ipr_ioa_cfg *ioa_cfg = pci_get_drvdata(pdev);
	ENTER;

	spin_lock_irqsave(ioa_cfg->host->host_lock, host_lock_flags);
	ipr_initiate_ioa_bringdown(ioa_cfg, IPR_SHUTDOWN_NORMAL);

	spin_unlock_irqrestore(ioa_cfg->host->host_lock, host_lock_flags);
	wait_event(ioa_cfg->reset_wait_q, !ioa_cfg->in_reset_reload);
	spin_lock_irqsave(ioa_cfg->host->host_lock, host_lock_flags);

	spin_lock(&ipr_driver_lock);
	list_del(&ioa_cfg->queue);
	spin_unlock(&ipr_driver_lock);

	if (ioa_cfg->sdt_state == ABORT_DUMP)
		ioa_cfg->sdt_state = WAIT_FOR_DUMP;
	spin_unlock_irqrestore(ioa_cfg->host->host_lock, host_lock_flags);

	ipr_free_all_resources(ioa_cfg);

	LEAVE;
}

/**
 * ipr_remove - IOA hot plug remove entry point
 * @pdev:	pci device struct
 *
 * Adapter hot plug remove entry point.
 *
 * Return value:
 * 	none
 **/
static void ipr_remove(struct pci_dev *pdev)
{
	struct ipr_ioa_cfg *ioa_cfg = pci_get_drvdata(pdev);

	ENTER;

	ioa_cfg->allow_cmds = 0;
	flush_scheduled_work();
	ipr_remove_trace_file(&ioa_cfg->host->shost_classdev.kobj,
			      &ipr_trace_attr);
	ipr_remove_dump_file(&ioa_cfg->host->shost_classdev.kobj,
			     &ipr_dump_attr);
	scsi_remove_host(ioa_cfg->host);

	__ipr_remove(pdev);

	LEAVE;
}

/**
 * ipr_probe - Adapter hot plug add entry point
 *
 * Return value:
 * 	0 on success / non-zero on failure
 **/
static int __devinit ipr_probe(struct pci_dev *pdev,
			       const struct pci_device_id *dev_id)
{
	struct ipr_ioa_cfg *ioa_cfg;
	int rc;

	rc = ipr_probe_ioa(pdev, dev_id);

	if (rc)
		return rc;

	ioa_cfg = pci_get_drvdata(pdev);
	rc = ipr_probe_ioa_part2(ioa_cfg);

	if (rc) {
		__ipr_remove(pdev);
		return rc;
	}

	rc = scsi_add_host(ioa_cfg->host, &pdev->dev);

	if (rc) {
		__ipr_remove(pdev);
		return rc;
	}

	rc = ipr_create_trace_file(&ioa_cfg->host->shost_classdev.kobj,
				   &ipr_trace_attr);

	if (rc) {
		scsi_remove_host(ioa_cfg->host);
		__ipr_remove(pdev);
		return rc;
	}

	rc = ipr_create_dump_file(&ioa_cfg->host->shost_classdev.kobj,
				   &ipr_dump_attr);

	if (rc) {
		ipr_remove_trace_file(&ioa_cfg->host->shost_classdev.kobj,
				      &ipr_trace_attr);
		scsi_remove_host(ioa_cfg->host);
		__ipr_remove(pdev);
		return rc;
	}

	scsi_scan_host(ioa_cfg->host);
	ipr_scan_vsets(ioa_cfg);
	scsi_add_device(ioa_cfg->host, IPR_IOA_BUS, IPR_IOA_TARGET, IPR_IOA_LUN);
	ioa_cfg->allow_ml_add_del = 1;
	schedule_work(&ioa_cfg->work_q);
	return 0;
}

/**
 * ipr_shutdown - Shutdown handler.
 * @dev:	device struct
 *
 * This function is invoked upon system shutdown/reboot. It will issue
 * an adapter shutdown to the adapter to flush the write cache.
 *
 * Return value:
 * 	none
 **/
static void ipr_shutdown(struct device *dev)
{
	struct ipr_ioa_cfg *ioa_cfg = pci_get_drvdata(to_pci_dev(dev));
	unsigned long lock_flags = 0;

	spin_lock_irqsave(ioa_cfg->host->host_lock, lock_flags);
	ipr_initiate_ioa_bringdown(ioa_cfg, IPR_SHUTDOWN_NORMAL);
	spin_unlock_irqrestore(ioa_cfg->host->host_lock, lock_flags);
	wait_event(ioa_cfg->reset_wait_q, !ioa_cfg->in_reset_reload);
}

static struct pci_device_id ipr_pci_table[] __devinitdata = {
	{ PCI_VENDOR_ID_MYLEX, PCI_DEVICE_ID_IBM_GEMSTONE,
		PCI_VENDOR_ID_IBM, IPR_SUBS_DEV_ID_5702,
		0, 0, (kernel_ulong_t)&ipr_chip_cfg[0] },
	{ PCI_VENDOR_ID_MYLEX, PCI_DEVICE_ID_IBM_GEMSTONE,
		PCI_VENDOR_ID_IBM, IPR_SUBS_DEV_ID_572E,
		0, 0, (kernel_ulong_t)&ipr_chip_cfg[0] },
	{ PCI_VENDOR_ID_MYLEX, PCI_DEVICE_ID_IBM_GEMSTONE,
		PCI_VENDOR_ID_IBM, IPR_SUBS_DEV_ID_5703,
	      0, 0, (kernel_ulong_t)&ipr_chip_cfg[0] },
	{ PCI_VENDOR_ID_MYLEX, PCI_DEVICE_ID_IBM_GEMSTONE,
		PCI_VENDOR_ID_IBM, IPR_SUBS_DEV_ID_573D,
	      0, 0, (kernel_ulong_t)&ipr_chip_cfg[0] },
	{ PCI_VENDOR_ID_IBM, PCI_DEVICE_ID_IBM_SNIPE,
		PCI_VENDOR_ID_IBM, IPR_SUBS_DEV_ID_2780,
		0, 0, (kernel_ulong_t)&ipr_chip_cfg[1] },
	{ }
};
MODULE_DEVICE_TABLE(pci, ipr_pci_table);

static struct pci_driver ipr_driver = {
	.name = IPR_NAME,
	.id_table = ipr_pci_table,
	.probe = ipr_probe,
	.remove = ipr_remove,
	.driver = {
		.shutdown = ipr_shutdown,
	},
};

/**
 * ipr_init - Module entry point
 *
 * Return value:
 * 	0 on success / non-zero on failure
 **/
static int __init ipr_init(void)
{
	ipr_info("IBM Power RAID SCSI Device Driver version: %s %s\n",
		 IPR_DRIVER_VERSION, IPR_DRIVER_DATE);

	pci_register_driver(&ipr_driver);

	return 0;
}

/**
 * ipr_exit - Module unload
 *
 * Module unload entry point.
 *
 * Return value:
 * 	none
 **/
static void __exit ipr_exit(void)
{
	pci_unregister_driver(&ipr_driver);
}

module_init(ipr_init);
module_exit(ipr_exit);
