/*
 *                  QLOGIC LINUX SOFTWARE
 *
 * QLogic ISP2x00 device driver for Linux 2.6.x
 * Copyright (C) 2003-2004 QLogic Corporation
 * (www.qlogic.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 */
#include "qla_def.h"

#include <linux/moduleparam.h>
#include <linux/vmalloc.h>
#include <linux/smp_lock.h>

#include <scsi/scsi_tcq.h>
#include <scsi/scsicam.h>
#include <scsi/scsi_transport.h>
#include <scsi/scsi_transport_fc.h>

/*
 * Driver version
 */
char qla2x00_version_str[40];

/*
 * SRB allocation cache
 */
char srb_cachep_name[16];
kmem_cache_t *srb_cachep;

/*
 * Stats for all adpaters.
 */
struct _qla2x00stats qla2x00_stats;

/*
 * Ioctl related information.
 */
int num_hosts;
int apiHBAInstance;

/*
 * Module parameter information and variables
 */
int ql2xmaxqdepth;
module_param(ql2xmaxqdepth, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(ql2xmaxqdepth,
		"Maximum queue depth to report for target devices.");

int ql2xlogintimeout = 20;
module_param(ql2xlogintimeout, int, S_IRUGO|S_IRUSR);
MODULE_PARM_DESC(ql2xlogintimeout,
		"Login timeout value in seconds.");

int qlport_down_retry;
module_param(qlport_down_retry, int, S_IRUGO|S_IRUSR);
MODULE_PARM_DESC(qlport_down_retry,
		"Maximum number of command retries to a port that returns"
		"a PORT-DOWN status.");

int ql2xretrycount = 20;
module_param(ql2xretrycount, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(ql2xretrycount,
		"Maximum number of mid-layer retries allowed for a command.  "
		"Default value is 20, ");

int displayConfig;
module_param(displayConfig, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(displayConfig,
		"If 1 then display the configuration used in /etc/modprobe.conf.");

int ql2xplogiabsentdevice;
module_param(ql2xplogiabsentdevice, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(ql2xplogiabsentdevice,
		"Option to enable PLOGI to devices that are not present after "
		"a Fabric scan.  This is needed for several broken switches."
		"Default is 0 - no PLOGI. 1 - perfom PLOGI.");

int ql2xenablezio = 0;
module_param(ql2xenablezio, int, S_IRUGO|S_IRUSR);
MODULE_PARM_DESC(ql2xenablezio,
		"Option to enable ZIO:If 1 then enable it otherwise" 
		" use the default set in the NVRAM."
		" Default is 0 : disabled");

int ql2xintrdelaytimer = 10;
module_param(ql2xintrdelaytimer, int, S_IRUGO|S_IRUSR);
MODULE_PARM_DESC(ql2xintrdelaytimer,
		"ZIO: Waiting time for Firmware before it generates an "
		"interrupt to the host to notify completion of request.");

int ConfigRequired;
module_param(ConfigRequired, int, S_IRUGO|S_IRUSR);
MODULE_PARM_DESC(ConfigRequired,
		"If 1, then only configured devices passed in through the"
		"ql2xopts parameter will be presented to the OS");

int Bind = BIND_BY_PORT_NAME;
module_param(Bind, int, S_IRUGO|S_IRUSR);
MODULE_PARM_DESC(Bind,
		"Target persistent binding method: "
		"0 by Portname (default); 1 by PortID; 2 by Nodename. ");

int ql2xsuspendcount = SUSPEND_COUNT;
module_param(ql2xsuspendcount, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(ql2xsuspendcount,
		"Number of 6-second suspend iterations to perform while a "
		"target returns a <NOT READY> status.  Default is 10 "
		"iterations.");

int ql2xdoinitscan = 1;
module_param(ql2xdoinitscan, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(ql2xdoinitscan,
		"Signal mid-layer to perform scan after driver load: 0 -- no "
		"signal sent to mid-layer.");

int ql2xloginretrycount = 0;
module_param(ql2xloginretrycount, int, S_IRUGO|S_IRUSR);
MODULE_PARM_DESC(ql2xloginretrycount,
		"Specify an alternate value for the NVRAM login retry count.");

/*
 * Proc structures and functions
 */
struct info_str {
	char	*buffer;
	int	length;
	off_t	offset;
	int	pos;
};

static void copy_mem_info(struct info_str *, char *, int);
static int copy_info(struct info_str *, char *, ...);

static void qla2x00_free_device(scsi_qla_host_t *);

static void qla2x00_config_dma_addressing(scsi_qla_host_t *ha);

/*
 * SCSI host template entry points 
 */
static int qla2xxx_slave_configure(struct scsi_device * device);
static int qla2xxx_eh_abort(struct scsi_cmnd *);
static int qla2xxx_eh_device_reset(struct scsi_cmnd *);
static int qla2xxx_eh_bus_reset(struct scsi_cmnd *);
static int qla2xxx_eh_host_reset(struct scsi_cmnd *);
static int qla2x00_loop_reset(scsi_qla_host_t *ha);
static int qla2x00_device_reset(scsi_qla_host_t *, fc_port_t *);

static int qla2x00_proc_info(struct Scsi_Host *, char *, char **,
    off_t, int, int);

static struct scsi_host_template qla2x00_driver_template = {
	.module			= THIS_MODULE,
	.name			= "qla2xxx",
	.proc_name		= "qla2xxx",
	.proc_info		= qla2x00_proc_info,
	.queuecommand		= qla2x00_queuecommand,

	.eh_abort_handler	= qla2xxx_eh_abort,
	.eh_device_reset_handler = qla2xxx_eh_device_reset,
	.eh_bus_reset_handler	= qla2xxx_eh_bus_reset,
	.eh_host_reset_handler	= qla2xxx_eh_host_reset,

	.slave_configure	= qla2xxx_slave_configure,

	.this_id		= -1,
	.cmd_per_lun		= 3,
	.use_clustering		= ENABLE_CLUSTERING,
	.sg_tablesize		= SG_ALL,

	/*
	 * The RISC allows for each command to transfer (2^32-1) bytes of data,
	 * which equates to 0x800000 sectors.
	 */
	.max_sectors		= 0xFFFF,
};

static struct scsi_transport_template *qla2xxx_transport_template = NULL;

static void qla2x00_display_fc_names(scsi_qla_host_t *);

/* TODO Convert to inlines
 *
 * Timer routines
 */
#define	WATCH_INTERVAL		1       /* number of seconds */

static void qla2x00_timer(scsi_qla_host_t *);

static __inline__ void qla2x00_start_timer(scsi_qla_host_t *,
    void *, unsigned long);
static __inline__ void qla2x00_restart_timer(scsi_qla_host_t *, unsigned long);
static __inline__ void qla2x00_stop_timer(scsi_qla_host_t *);

static inline void
qla2x00_start_timer(scsi_qla_host_t *ha, void *func, unsigned long interval)
{
	init_timer(&ha->timer);
	ha->timer.expires = jiffies + interval * HZ;
	ha->timer.data = (unsigned long)ha;
	ha->timer.function = (void (*)(unsigned long))func;
	add_timer(&ha->timer);
	ha->timer_active = 1;
}

static inline void
qla2x00_restart_timer(scsi_qla_host_t *ha, unsigned long interval)
{
	mod_timer(&ha->timer, jiffies + interval * HZ);
}

static __inline__ void
qla2x00_stop_timer(scsi_qla_host_t *ha)
{
	del_timer_sync(&ha->timer);
	ha->timer_active = 0;
}

void qla2x00_cmd_timeout(srb_t *);

static __inline__ void qla2x00_callback(scsi_qla_host_t *, struct scsi_cmnd *);
static __inline__ void sp_put(struct scsi_qla_host * ha, srb_t *sp);
static __inline__ void sp_get(struct scsi_qla_host * ha, srb_t *sp);
static __inline__ void
qla2x00_delete_from_done_queue(scsi_qla_host_t *, srb_t *); 

/**************************************************************************
* sp_put
*
* Description:
*   Decrement reference count and call the callback if we're the last
*   owner of the specified sp. Will get the host_lock before calling
*   the callback.
*
* Input:
*   ha - pointer to the scsi_qla_host_t where the callback is to occur.
*   sp - pointer to srb_t structure to use.
*
* Returns:
*
**************************************************************************/
static inline void
sp_put(struct scsi_qla_host * ha, srb_t *sp)
{
        if (atomic_read(&sp->ref_count) == 0) {
		qla_printk(KERN_INFO, ha,
			"%s(): **** SP->ref_count not zero\n",
			__func__);
                DEBUG2(BUG();)

                return;
	}

        if (!atomic_dec_and_test(&sp->ref_count)) {
                return;
        }

        qla2x00_callback(ha, sp->cmd);
}

/**************************************************************************
* sp_get
*
* Description:
*   Increment reference count of the specified sp.
*
* Input:
*   sp - pointer to srb_t structure to use.
*
* Returns:
*
**************************************************************************/
static inline void
sp_get(struct scsi_qla_host * ha, srb_t *sp)
{
        atomic_inc(&sp->ref_count);

        if (atomic_read(&sp->ref_count) > 2) {
		qla_printk(KERN_INFO, ha,
			"%s(): **** SP->ref_count greater than two\n",
			__func__);
                DEBUG2(BUG();)

		return;
	}
}

/*
* qla2x00_callback
*      Returns the completed SCSI command to LINUX.
*
* Input:
*	ha -- Host adapter structure
*	cmd -- SCSI mid-level command structure.
* Returns:
*      None
* Note:From failover point of view we always get the sp
*      from vis_ha pool in queuecommand.So when we put it 
*      back to the pool it has to be the vis_ha.	 
*      So rely on struct scsi_cmnd to get the vis_ha and not on sp. 		 	
*/
static inline void
qla2x00_callback(scsi_qla_host_t *ha, struct scsi_cmnd *cmd)
{
	srb_t *sp = (srb_t *) CMD_SP(cmd);
	scsi_qla_host_t *vis_ha;
	os_lun_t *lq;
	int got_sense;
	unsigned long	cpu_flags = 0;

	cmd->host_scribble = (unsigned char *) NULL;
	vis_ha = (scsi_qla_host_t *) cmd->device->host->hostdata;

	if (sp == NULL) {
		qla_printk(KERN_INFO, ha,
			"%s(): **** CMD derives a NULL SP\n",
			__func__);
                DEBUG2(BUG();)
		return;
	}

	/*
	 * If command status is not DID_BUS_BUSY then go ahead and freed sp.
	 */
	/*
	 * Cancel command timeout
	 */
	qla2x00_delete_timer_from_cmd(sp);

	/*
	 * Put SP back in the free queue
	 */
	sp->cmd   = NULL;
	CMD_SP(cmd) = NULL;
	lq = sp->lun_queue;
	got_sense = (sp->flags & SRB_GOT_SENSE)? 1: 0;
	add_to_free_queue(vis_ha, sp);

	if (host_byte(cmd->result) == DID_OK) {
		/* device ok */
		ha->total_bytes += cmd->bufflen;
		if (!got_sense) {
			/* If lun was suspended then clear retry count */
			spin_lock_irqsave(&lq->q_lock, cpu_flags);
			if (!test_bit(LUN_EXEC_DELAYED, &lq->q_flag))
				lq->q_state = LUN_STATE_READY;
			spin_unlock_irqrestore(&lq->q_lock, cpu_flags);
		}
	} else if (host_byte(cmd->result) == DID_ERROR) {
		/* device error */
		ha->total_dev_errs++;
	}

	/* Call the mid-level driver interrupt handler */
	(*(cmd)->scsi_done)(cmd);
}

static inline void 
qla2x00_delete_from_done_queue(scsi_qla_host_t *dest_ha, srb_t *sp) 
{
	/* remove command from done list */
	list_del_init(&sp->list);
	dest_ha->done_q_cnt--;
	sp->state = SRB_NO_QUEUE_STATE;

	if (sp->flags & SRB_DMA_VALID) {
		sp->flags &= ~SRB_DMA_VALID;

		/* Release memory used for this I/O */
		if (sp->cmd->use_sg) {
			pci_unmap_sg(dest_ha->pdev, sp->cmd->request_buffer,
			    sp->cmd->use_sg, sp->cmd->sc_data_direction);
		} else if (sp->cmd->request_bufflen) {
			pci_unmap_page(dest_ha->pdev, sp->dma_handle,
			    sp->cmd->request_bufflen,
			    sp->cmd->sc_data_direction);
		}
	}
}

static int qla2x00_do_dpc(void *data);

static void qla2x00_rst_aen(scsi_qla_host_t *);

static uint8_t qla2x00_mem_alloc(scsi_qla_host_t *);
static void qla2x00_mem_free(scsi_qla_host_t *ha);
int qla2x00_allocate_sp_pool( scsi_qla_host_t *ha);
void qla2x00_free_sp_pool(scsi_qla_host_t *ha);

static ssize_t qla2x00_sysfs_read_fw_dump(struct kobject *, char *, loff_t,
    size_t);
static ssize_t qla2x00_sysfs_write_fw_dump(struct kobject *, char *, loff_t,
    size_t);
static struct bin_attribute sysfs_fw_dump_attr = {
	.attr = {
		.name = "fw_dump",
		.mode = S_IRUSR | S_IWUSR,
		.owner = THIS_MODULE,
	},
	.size = 0,
	.read = qla2x00_sysfs_read_fw_dump,
	.write = qla2x00_sysfs_write_fw_dump,
};
static ssize_t qla2x00_sysfs_read_nvram(struct kobject *, char *, loff_t,
    size_t);
static ssize_t qla2x00_sysfs_write_nvram(struct kobject *, char *, loff_t,
    size_t);
static struct bin_attribute sysfs_nvram_attr = {
	.attr = {
		.name = "nvram",
		.mode = S_IRUSR | S_IWUSR,
		.owner = THIS_MODULE,
	},
	.size = sizeof(nvram_t),
	.read = qla2x00_sysfs_read_nvram,
	.write = qla2x00_sysfs_write_nvram,
};


int
qla2x00_set_info(char *buffer, int length, struct Scsi_Host *shost)
{
	return (-ENOSYS);  /* Currently this is a no-op */
}

/* -------------------------------------------------------------------------- */


/* SysFS attributes. */
static ssize_t qla2x00_sysfs_read_fw_dump(struct kobject *kobj, char *buf,
    loff_t off, size_t count)
{
	struct scsi_qla_host *ha = to_qla_host(dev_to_shost(container_of(kobj,
	    struct device, kobj)));

	if (ha->fw_dump_reading == 0)
		return 0;
	if (off > ha->fw_dump_buffer_len)
		return 0;
	if (off + count > ha->fw_dump_buffer_len)
		count = ha->fw_dump_buffer_len - off;

	memcpy(buf, &ha->fw_dump_buffer[off], count);

	return (count);
}

static ssize_t qla2x00_sysfs_write_fw_dump(struct kobject *kobj, char *buf,
    loff_t off, size_t count)
{
	struct scsi_qla_host *ha = to_qla_host(dev_to_shost(container_of(kobj,
	    struct device, kobj)));
	int reading;
	uint32_t dump_size;

	if (off != 0)
		return (0);

	reading = simple_strtol(buf, NULL, 10);
	switch (reading) {
	case 0:
		if (ha->fw_dump_reading == 1) {
			qla_printk(KERN_INFO, ha,
			    "Firmware dump cleared on (%ld).\n",
			    ha->host_no);

			vfree(ha->fw_dump_buffer);
			free_pages((unsigned long)ha->fw_dump,
			    ha->fw_dump_order);

			ha->fw_dump_reading = 0;
			ha->fw_dump_buffer = NULL;
			ha->fw_dump = NULL;
		}
		break;
	case 1:
		if (ha->fw_dump != NULL && !ha->fw_dump_reading) {
			ha->fw_dump_reading = 1;

			dump_size = FW_DUMP_SIZE_1M;
			if (ha->fw_memory_size < 0x20000) 
				dump_size = FW_DUMP_SIZE_128K;
			else if (ha->fw_memory_size < 0x80000) 
				dump_size = FW_DUMP_SIZE_512K;
			ha->fw_dump_buffer = (char *)vmalloc(dump_size);
			if (ha->fw_dump_buffer == NULL) {
				qla_printk(KERN_WARNING, ha,
				    "Unable to allocate memory for firmware "
				    "dump buffer (%d).\n", dump_size);

				ha->fw_dump_reading = 0;
				return (count);
			}
			qla_printk(KERN_INFO, ha,
			    "Firmware dump ready for read on (%ld).\n",
			    ha->host_no);
			memset(ha->fw_dump_buffer, 0, dump_size);
			if (IS_QLA2100(ha) || IS_QLA2200(ha))
 				qla2100_ascii_fw_dump(ha);
 			else
 				qla2300_ascii_fw_dump(ha);
			ha->fw_dump_buffer_len = strlen(ha->fw_dump_buffer);
		}
		break;
	}
	return (count);
}

static ssize_t qla2x00_sysfs_read_nvram(struct kobject *kobj, char *buf,
    loff_t off, size_t count)
{
	struct scsi_qla_host *ha = to_qla_host(dev_to_shost(container_of(kobj,
	    struct device, kobj)));
	uint16_t	*witer;
	unsigned long	flags;
	uint16_t	cnt;

	if (!capable(CAP_SYS_ADMIN) || off != 0 || count != sizeof(nvram_t))
		return 0;

	/* Read NVRAM. */
	spin_lock_irqsave(&ha->hardware_lock, flags);
	qla2x00_lock_nvram_access(ha);
 	witer = (uint16_t *)buf;
 	for (cnt = 0; cnt < count / 2; cnt++) {
		*witer = cpu_to_le16(qla2x00_get_nvram_word(ha,
		    cnt+ha->nvram_base));
		witer++;
 	}
	qla2x00_unlock_nvram_access(ha);
	spin_unlock_irqrestore(&ha->hardware_lock, flags);

	return (count);
}

static ssize_t qla2x00_sysfs_write_nvram(struct kobject *kobj, char *buf,
    loff_t off, size_t count)
{
	struct scsi_qla_host *ha = to_qla_host(dev_to_shost(container_of(kobj,
	    struct device, kobj)));
	uint8_t		*iter;
	uint16_t	*witer;
	unsigned long	flags;
	uint16_t	cnt;
	uint8_t		chksum;

	if (!capable(CAP_SYS_ADMIN) || off != 0 || count != sizeof(nvram_t))
		return 0;

	/* Checksum NVRAM. */
	iter = (uint8_t *)buf;
	chksum = 0;
	for (cnt = 0; cnt < count - 1; cnt++)
		chksum += *iter++;
	chksum = ~chksum + 1;
	*iter = chksum;

	/* Write NVRAM. */
	spin_lock_irqsave(&ha->hardware_lock, flags);
	qla2x00_lock_nvram_access(ha);
 	witer = (uint16_t *)buf;
	for (cnt = 0; cnt < count / 2; cnt++) {
		qla2x00_write_nvram_word(ha, cnt+ha->nvram_base,
		    cpu_to_le16(*witer));
		witer++;
	}
	qla2x00_unlock_nvram_access(ha);
	spin_unlock_irqrestore(&ha->hardware_lock, flags);

	return (count);
}

/* -------------------------------------------------------------------------- */
char *
qla2x00_get_pci_info_str(struct scsi_qla_host *ha, char *str)
{
	static char *pci_bus_modes[] = {
		"33", "66", "100", "133",
	};
	uint16_t pci_bus;

	strcpy(str, "PCI");
	pci_bus = (ha->pci_attr & (BIT_9 | BIT_10)) >> 9;
	if (pci_bus) {
		strcat(str, "-X (");
		strcat(str, pci_bus_modes[pci_bus]);
	} else {
		pci_bus = (ha->pci_attr & BIT_8) >> 8;
		strcat(str, " (");
		strcat(str, pci_bus_modes[pci_bus]);
	}
	strcat(str, " MHz)");

	return (str);
}

char *
qla2x00_get_fw_version_str(struct scsi_qla_host *ha, char *str)
{
	char un_str[10];
	
	sprintf(str, "%d.%02d.%02d ", ha->fw_major_version,
	    ha->fw_minor_version,
	    ha->fw_subminor_version);

	if (ha->fw_attributes & BIT_9) {
		strcat(str, "FLX");
		return (str);
	}

	switch (ha->fw_attributes & 0xFF) {
	case 0x7:
		strcat(str, "EF");
		break;
	case 0x17:
		strcat(str, "TP");
		break;
	case 0x37:
		strcat(str, "IP");
		break;
	case 0x77:
		strcat(str, "VI");
		break;
	default:
		sprintf(un_str, "(%x)", ha->fw_attributes);
		strcat(str, un_str);
		break;
	}
	if (ha->fw_attributes & 0x100)
		strcat(str, "X");

	return (str);
}

/**************************************************************************
* qla2x00_queuecommand
*
* Description:
*     Queue a command to the controller.
*
* Input:
*     cmd - pointer to Scsi cmd structure
*     fn - pointer to Scsi done function
*
* Returns:
*   0 - Always
*
* Note:
* The mid-level driver tries to ensures that queuecommand never gets invoked
* concurrently with itself or the interrupt handler (although the
* interrupt handler may call this routine as part of request-completion
* handling).
**************************************************************************/
int
qla2x00_queuecommand(struct scsi_cmnd *cmd, void (*fn)(struct scsi_cmnd *))
{
	fc_port_t	*fcport;
	os_lun_t	*lq;
	os_tgt_t	*tq;
	scsi_qla_host_t	*ha, *ha2;
	srb_t		*sp;
	struct Scsi_Host *host;
	unsigned int	b, t, l;
	unsigned long	handle;
	int		was_empty;


	host = cmd->device->host;
	ha = (scsi_qla_host_t *) host->hostdata;
	was_empty = 1;

	cmd->scsi_done = fn;

	spin_unlock_irq(ha->host->host_lock);

	/*
	 * Allocate a command packet from the "sp" pool.  If we cant get back
	 * one then let scsi layer come back later.
	 */
	if ((sp = qla2x00_get_new_sp(ha)) == NULL) {
		qla_printk(KERN_WARNING, ha,
		    "Couldn't allocate memory for sp - retried.\n");

		spin_lock_irq(ha->host->host_lock);

		return (1);
	}

	sp->cmd = cmd;
	CMD_SP(cmd) = (void *)sp;

	sp->flags = 0;
	if (CMD_RESID_LEN(cmd) & SRB_IOCTL) {
		/* Need to set sp->flags */
		sp->flags |= SRB_IOCTL;
		CMD_RESID_LEN(cmd) = 0; /* Clear it since no more use. */
	}

	sp->fo_retry_cnt = 0;
	sp->err_id = 0;

	/* Generate LU queue on bus, target, LUN */
	b = cmd->device->channel;
	t = cmd->device->id;
	l = cmd->device->lun;

	/*
	 * Start Command Timer. Typically it will be 2 seconds less than what
	 * is requested by the Host such that we can return the IO before
	 * aborts are called.
	 */
	if ((cmd->timeout_per_command / HZ) > QLA_CMD_TIMER_DELTA)
		qla2x00_add_timer_to_cmd(sp,
		    (cmd->timeout_per_command / HZ) - QLA_CMD_TIMER_DELTA);
	else
		qla2x00_add_timer_to_cmd(sp, cmd->timeout_per_command / HZ);

	if (l >= ha->max_luns) {
		cmd->result = DID_NO_CONNECT << 16;
		sp->err_id = SRB_ERR_PORT;

		spin_lock_irq(ha->host->host_lock);

		sp_put(ha, sp);

		return (0);
	}

	if ((tq = (os_tgt_t *) TGT_Q(ha, t)) != NULL &&
	    (lq = (os_lun_t *) LUN_Q(ha, t, l)) != NULL) {
		fcport = lq->fclun->fcport;
		ha2 = fcport->ha;
	} else {
		lq = NULL;
		fcport = NULL;
		ha2 = ha;
	}

	/* Set an invalid handle until we issue the command to ISP */
	/* then we will set the real handle value.                 */
	handle = INVALID_HANDLE;
	cmd->host_scribble = (unsigned char *)handle;

	/* Bookkeeping information */
	sp->r_start = jiffies;       /* time the request was recieved */
	sp->u_start = 0;

	/* Setup device queue pointers. */
	sp->tgt_queue = tq;
	sp->lun_queue = lq;

	/*
	 * NOTE : q is NULL
	 *
	 * 1. When device is added from persistent binding but has not been
	 *    discovered yet.The state of loopid == PORT_AVAIL.
	 * 2. When device is never found on the bus.(loopid == UNUSED)
	 *
	 * IF Device Queue is not created, or device is not in a valid state
	 * and link down error reporting is enabled, reject IO.
	 */
	if (fcport == NULL) {
		DEBUG3(printk("scsi(%ld:%2d:%2d): port unavailable\n",
		    ha->host_no,t,l));

		cmd->result = DID_NO_CONNECT << 16;
		sp->err_id = SRB_ERR_PORT;

		spin_lock_irq(ha->host->host_lock);

		sp_put(ha, sp);

		return (0);
	}

	/* Only modify the allowed count if the target is a *non* tape device */
	if ((fcport->flags & FCF_TAPE_PRESENT) == 0) {
		sp->flags &= ~SRB_TAPE;
		if (cmd->allowed < ql2xretrycount) {
			cmd->allowed = ql2xretrycount;
		}
	} else
		sp->flags |= SRB_TAPE;


	DEBUG5(printk("scsi(%ld:%2d:%2d): (queuecmd) queue sp = %p, "
	    "flags=0x%x fo retry=%d, pid=%ld\n",
	    ha->host_no, t, l, sp, sp->flags, sp->fo_retry_cnt,
	    cmd->serial_number));
	DEBUG5(qla2x00_print_scsi_cmd(cmd));

	sp->fclun = lq->fclun;
	sp->ha = ha2;

	if (cmd->sc_data_direction == DMA_BIDIRECTIONAL &&
	    cmd->request_bufflen != 0) {

		DEBUG2(printk(KERN_WARNING
		    "scsi(%ld): Incorrect data direction - transfer "
		    "length=%d, direction=%d, pid=%ld, opcode=%x\n",
		    ha->host_no, cmd->request_bufflen, cmd->sc_data_direction,
		    cmd->serial_number, cmd->cmnd[0]));
	}

	/* Final pre-check :
	 *
	 *	Either PORT_DOWN_TIMER OR LINK_DOWN_TIMER Expired.
	 */
	if (atomic_read(&fcport->state) == FCS_DEVICE_DEAD ||
	    atomic_read(&ha2->loop_state) == LOOP_DEAD) {
		/*
		 * Add the command to the done-queue for later failover
		 * processing.
		 */
		cmd->result = DID_NO_CONNECT << 16;
		if (atomic_read(&ha2->loop_state) == LOOP_DOWN) 
			sp->err_id = SRB_ERR_LOOP;
		else
			sp->err_id = SRB_ERR_PORT;

		add_to_done_queue(ha, sp);
		qla2x00_done(ha);

		spin_lock_irq(ha->host->host_lock);
		return (0);
	}
	if (tq && test_bit(TQF_SUSPENDED, &tq->flags) &&
	    (sp->flags & SRB_TAPE) == 0) {
		/* If target suspended put incoming I/O in retry_q. */
		qla2x00_extend_timeout(sp->cmd, 10);
		add_to_scsi_retry_queue(ha, sp);
	} else
		was_empty = add_to_pending_queue(ha, sp);

	if ((IS_QLA2100(ha) || IS_QLA2200(ha)) && ha->flags.online) {
		unsigned long flags;
		device_reg_t *reg;
		reg = ha->iobase;
		
		if (ha->response_ring_ptr->signature != RESPONSE_PROCESSED) {
			spin_lock_irqsave(&ha->hardware_lock, flags);	
			qla2x00_process_response_queue(ha);
			spin_unlock_irqrestore(&ha->hardware_lock, flags);
		}
	}

	/* We submit to the hardware if:
	 *
	 *	1) we're on the cpu the irq's arrive on or
	 *	2) there are very few io's outstanding.
	 *
	 * In all other cases we'll let an irq pick up our IO and submit it
	 * to the controller to improve affinity.
	 */
	if (smp_processor_id() == ha->last_irq_cpu || was_empty)
		qla2x00_next(ha);

	spin_lock_irq(ha->host->host_lock);

	return (0);
}

/*
 * qla2x00_eh_wait_on_command
 *    Waits for the command to be returned by the Firmware for some
 *    max time.
 *
 * Input:
 *    ha = actual ha whose done queue will contain the command
 *	      returned by firmware.
 *    cmd = Scsi Command to wait on.
 *    flag = Abort/Reset(Bus or Device Reset)
 *
 * Return:
 *    Not Found : 0
 *    Found : 1
 */
static int
qla2x00_eh_wait_on_command(scsi_qla_host_t *ha, struct scsi_cmnd *cmd)
{
#define ABORT_POLLING_PERIOD	HZ
#define ABORT_WAIT_TIME		((10 * HZ) / (ABORT_POLLING_PERIOD))

	int		found = 0;
	int		done = 0;
	srb_t		*rp = NULL;
	struct list_head *list, *temp;
	u_long		max_wait_time = ABORT_WAIT_TIME;

	do {
		/* Check on done queue */
		spin_lock(&ha->list_lock);
		list_for_each_safe(list, temp, &ha->done_queue) {
			rp = list_entry(list, srb_t, list);

			/*
			 * Found command. Just exit and wait for the cmd sent
			 * to OS.
			*/
			if (cmd == rp->cmd) {
				found++;
				DEBUG3(printk("%s: found in done queue.\n",
				    __func__);)
				break;
			}
		}
		spin_unlock(&ha->list_lock);

		/* Complete the cmd right away. */
		if (found) { 
			qla2x00_delete_from_done_queue(ha, rp);
			sp_put(ha, rp);
			done++;
			break;
		}

		spin_unlock_irq(ha->host->host_lock);

		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(ABORT_POLLING_PERIOD);

		spin_lock_irq(ha->host->host_lock);

	} while ((max_wait_time--));

	if (done)
		DEBUG2(printk(KERN_INFO "%s: found cmd=%p.\n", __func__, cmd));

	return (done);
}

/*
 * qla2x00_wait_for_hba_online
 *    Wait till the HBA is online after going through 
 *    <= MAX_RETRIES_OF_ISP_ABORT  or
 *    finally HBA is disabled ie marked offline
 *
 * Input:
 *     ha - pointer to host adapter structure
 * 
 * Note:    
 *    Does context switching-Release SPIN_LOCK
 *    (if any) before calling this routine.
 *
 * Return:
 *    Success (Adapter is online) : 0
 *    Failed  (Adapter is offline/disabled) : 1
 */
int 
qla2x00_wait_for_hba_online(scsi_qla_host_t *ha)
{
	int 	 return_status;
	unsigned long wait_online;

	wait_online = jiffies + (MAX_LOOP_TIMEOUT * HZ); 
	while (((test_bit(ISP_ABORT_NEEDED, &ha->dpc_flags)) ||
	    test_bit(ABORT_ISP_ACTIVE, &ha->dpc_flags) ||
	    test_bit(ISP_ABORT_RETRY, &ha->dpc_flags) ||
	    ha->dpc_active) && time_before(jiffies, wait_online)) {

		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ);
	}
	if (ha->flags.online) 
		return_status = QLA_SUCCESS; 
	else
		return_status = QLA_FUNCTION_FAILED;

	DEBUG2(printk("%s return_status=%d\n",__func__,return_status));

	return (return_status);
}

/*
 * qla2x00_wait_for_loop_ready
 *    Wait for MAX_LOOP_TIMEOUT(5 min) value for loop
 *    to be in LOOP_READY state.	 
 * Input:
 *     ha - pointer to host adapter structure
 * 
 * Note:    
 *    Does context switching-Release SPIN_LOCK
 *    (if any) before calling this routine.
 *    
 *
 * Return:
 *    Success (LOOP_READY) : 0
 *    Failed  (LOOP_NOT_READY) : 1
 */
static inline int 
qla2x00_wait_for_loop_ready(scsi_qla_host_t *ha)
{
	int 	 return_status = QLA_SUCCESS;
	unsigned long loop_timeout ;

	/* wait for 5 min at the max for loop to be ready */
	loop_timeout = jiffies + (MAX_LOOP_TIMEOUT * HZ); 

	while ((!atomic_read(&ha->loop_down_timer) &&
	    atomic_read(&ha->loop_state) == LOOP_DOWN) ||
	    test_bit(CFG_ACTIVE, &ha->cfg_flags) ||
	    atomic_read(&ha->loop_state) != LOOP_READY) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ);
		if (time_after_eq(jiffies, loop_timeout)) {
			return_status = QLA_FUNCTION_FAILED;
			break;
		}
	}
	return (return_status);	
}

/**************************************************************************
* qla2xxx_eh_abort
*
* Description:
*    The abort function will abort the specified command.
*
* Input:
*    cmd = Linux SCSI command packet to be aborted.
*
* Returns:
*    Either SUCCESS or FAILED.
*
* Note:
**************************************************************************/
int
qla2xxx_eh_abort(struct scsi_cmnd *cmd)
{
	int		i;
	int		return_status = FAILED;
	os_lun_t	*q;
	scsi_qla_host_t *ha;
	scsi_qla_host_t *vis_ha;
	srb_t		*sp;
	srb_t		*rp;
	struct list_head *list, *temp;
	struct Scsi_Host *host;
	uint8_t		found = 0;
	unsigned int	b, t, l;

	/* Get the SCSI request ptr */
	sp = (srb_t *) CMD_SP(cmd);

	/*
	 * If sp is NULL, command is already returned.
	 * sp is NULLED just before we call back scsi_done
	 *
	 */
	if ((sp == NULL)) {
		/* no action - we don't have command */
		qla_printk(KERN_INFO, to_qla_host(cmd->device->host),
		    "qla2xxx_eh_abort: cmd already done sp=%p\n", sp);
		DEBUG(printk("qla2xxx_eh_abort: cmd already done sp=%p\n", sp);)
		return SUCCESS;
	}
	if (sp) {
		DEBUG(printk("qla2xxx_eh_abort: refcount %i \n",
		    atomic_read(&sp->ref_count));)
	}

	vis_ha = (scsi_qla_host_t *) cmd->device->host->hostdata;
	ha = (scsi_qla_host_t *)cmd->device->host->hostdata;

	host = ha->host;

	/* Generate LU queue on bus, target, LUN */
	b = cmd->device->channel;
	t = cmd->device->id;
	l = cmd->device->lun;
	q = GET_LU_Q(vis_ha, t, l);

	qla_printk(KERN_INFO, ha, 
	    "%s scsi(%ld:%d:%d:%d): cmd_timeout_in_sec=0x%x.\n", __func__,
	    ha->host_no, (int)b, (int)t, (int)l,
	    cmd->timeout_per_command / HZ);

	/*
	 * if no LUN queue then something is very wrong!!!
	 */
	if (q == NULL) {
		qla_printk(KERN_WARNING, ha,
			"qla2x00: (%x:%x:%x) No LUN queue.\n", b, t, l);

		/* no action - we don't have command */
		return FAILED;
	}

	DEBUG2(printk("scsi(%ld): ABORTing cmd=%p sp=%p jiffies = 0x%lx, "
	    "timeout=%x, dpc_flags=%lx, vis_ha->dpc_flags=%lx q->flag=%lx\n",
	    ha->host_no, cmd, sp, jiffies, cmd->timeout_per_command / HZ,
	    ha->dpc_flags, vis_ha->dpc_flags, q->q_flag));
	DEBUG2(qla2x00_print_scsi_cmd(cmd));

	spin_unlock_irq(ha->host->host_lock);
	if (qla2x00_wait_for_hba_online(ha) != QLA_SUCCESS) {
		DEBUG2(printk("%s failed:board disabled\n", __func__);)
		spin_lock_irq(ha->host->host_lock);
		return FAILED;
	}
	spin_lock_irq(ha->host->host_lock);

	/* Search done queue */
	spin_lock(&ha->list_lock);
	list_for_each_safe(list, temp, &ha->done_queue) {
		rp = list_entry(list, srb_t, list);

		if (cmd != rp->cmd)
			continue;

		/*
		 * Found command.Remove it from done list.
		 * And proceed to post completion to scsi mid layer.
		 */
		return_status = SUCCESS;
		found++;
		qla2x00_delete_from_done_queue(ha, sp);

		break;
	} /* list_for_each_safe() */
	spin_unlock(&ha->list_lock);

	/*
	 * Return immediately if the aborted command was already in the done
	 * queue
	 */
	if (found) {
		qla_printk(KERN_INFO, ha,
		    "qla2xxx_eh_abort: Returning completed command=%p sp=%p\n",
		    cmd, sp);
		sp_put(ha, sp);
		return (return_status);
	}
	

	/*
	 * See if this command is in the retry queue
	 */
	DEBUG3(printk("qla2xxx_eh_abort: searching sp %p in retry "
		    "queue.\n", sp);)

	spin_lock(&ha->list_lock);
	list_for_each_safe(list, temp, &ha->retry_queue) {
		rp = list_entry(list, srb_t, list);

		if (cmd != rp->cmd)
			continue;


		DEBUG2(printk("qla2xxx_eh_abort: found "
		    "in retry queue. SP=%p\n", sp);)

		__del_from_retry_queue(ha, rp);
		cmd->result = DID_ABORT << 16;
		__add_to_done_queue(ha, rp);

		return_status = SUCCESS;
		found++;

		break;

	} 
	spin_unlock(&ha->list_lock);


	/*
	 * Our SP pointer points at the command we want to remove from the
	 * pending queue providing we haven't already sent it to the adapter.
	 */
	if (!found) {
		DEBUG3(printk("qla2xxx_eh_abort: searching sp %p "
		    "in pending queue.\n", sp);)

		spin_lock(&vis_ha->list_lock);
		list_for_each_safe(list, temp, &vis_ha->pending_queue) {
			rp = list_entry(list, srb_t, list);

			if (rp->cmd != cmd)
				continue;

			/* Remove srb from LUN queue. */
			rp->flags |=  SRB_ABORTED;

			DEBUG2(printk("qla2xxx_eh_abort: Cmd in pending queue."
			    " serial_number %ld.\n",
			    sp->cmd->serial_number);)

			__del_from_pending_queue(vis_ha, rp);
			cmd->result = DID_ABORT << 16;

			__add_to_done_queue(vis_ha, rp);

			return_status = SUCCESS;

			found++;
			break;
		} /* list_for_each_safe() */
		spin_unlock(&vis_ha->list_lock);
	} /*End of if !found */

	if (!found) {  /* find the command in our active list */
		DEBUG3(printk("qla2xxx_eh_abort: searching sp %p "
		    "in outstanding queue.\n", sp);)

		spin_lock(&ha->hardware_lock);
		for (i = 1; i < MAX_OUTSTANDING_COMMANDS; i++) {
			sp = ha->outstanding_cmds[i];

			if (sp == NULL)
				continue;

			if (sp->cmd != cmd)
				continue;

			DEBUG2(printk("qla2xxx_eh_abort(%ld): aborting sp %p "
			    "from RISC. pid=%ld sp->state=%x q->q_flag=%lx\n",
			    ha->host_no, sp, sp->cmd->serial_number,
			    sp->state, q->q_flag);)
			DEBUG(qla2x00_print_scsi_cmd(cmd);)

			/* Get a reference to the sp and drop the lock.*/
			sp_get(ha, sp);

			spin_unlock(&ha->hardware_lock);
			spin_unlock_irq(ha->host->host_lock);

			if (qla2x00_abort_command(ha, sp)) {
				DEBUG2(printk("qla2xxx_eh_abort: abort_command "
				    "mbx failed.\n");)
				return_status = FAILED;
			} else {
				DEBUG3(printk("qla2xxx_eh_abort: abort_command "
				    " mbx success.\n");)
				return_status = SUCCESS;
			}

			sp_put(ha,sp);

			spin_lock_irq(ha->host->host_lock);
			spin_lock(&ha->hardware_lock);

			/*
			 * Regardless of mailbox command status, go check on
			 * done queue just in case the sp is already done.
			 */
			break;

		}/*End of for loop */
		spin_unlock(&ha->hardware_lock);

	} /*End of if !found */

	/* Waiting for our command in done_queue to be returned to OS.*/
	if (qla2x00_eh_wait_on_command(ha, cmd) != 0) {
		DEBUG2(printk("qla2xxx_eh_abort: cmd returned back to OS.\n");)
		return_status = SUCCESS;
	}

	if (return_status == FAILED) {
		qla_printk(KERN_INFO, ha, 
			"qla2xxx_eh_abort Exiting: status=Failed\n");
		return FAILED;
	}

	DEBUG2(printk("qla2xxx_eh_abort: Exiting. return_status=0x%x.\n",
	    return_status));

	return return_status;
}

/**************************************************************************
* qla2x00_eh_wait_for_pending_target_commands
*
* Description:
*    Waits for all the commands to come back from the specified target.
*
* Input:
*    ha - pointer to scsi_qla_host structure.
*    t  - target 	
* Returns:
*    Either SUCCESS or FAILED.
*
* Note:
**************************************************************************/
int
qla2x00_eh_wait_for_pending_target_commands(scsi_qla_host_t *ha, unsigned int t)
{
	int	cnt;
	int	status;
	srb_t		*sp;
	struct scsi_cmnd *cmd;

	status = 0;

	/*
	 * Waiting for all commands for the designated target in the active
	 * array
	 */
	for (cnt = 1; cnt < MAX_OUTSTANDING_COMMANDS; cnt++) {
		spin_lock(&ha->hardware_lock);
		sp = ha->outstanding_cmds[cnt];
		if (sp) {
			cmd = sp->cmd;
			spin_unlock(&ha->hardware_lock);
			if (cmd->device->id == t) {
				if (!qla2x00_eh_wait_on_command(ha, cmd)) {
					status = 1;
					break;
				}
			}
		}
		else {
			spin_unlock(&ha->hardware_lock);
		}
	}
	return (status);
}


/**************************************************************************
* qla2xxx_eh_device_reset
*
* Description:
*    The device reset function will reset the target and abort any
*    executing commands.
*
*    NOTE: The use of SP is undefined within this context.  Do *NOT*
*          attempt to use this value, even if you determine it is 
*          non-null.
*
* Input:
*    cmd = Linux SCSI command packet of the command that cause the
*          bus device reset.
*
* Returns:
*    SUCCESS/FAILURE (defined as macro in scsi.h).
*
**************************************************************************/
int
qla2xxx_eh_device_reset(struct scsi_cmnd *cmd)
{
	int		return_status;
	unsigned int	b, t, l;
	scsi_qla_host_t	*ha;
	os_tgt_t	*tq;
	os_lun_t	*lq;
	fc_port_t	*fcport_to_reset;
	srb_t		*rp;
	struct list_head *list, *temp;

	return_status = FAILED;
	if (cmd == NULL) {
		printk(KERN_INFO
		    "%s(): **** SCSI mid-layer passing in NULL cmd\n",
		    __func__);

		return (return_status);
	}

	b = cmd->device->channel;
	t = cmd->device->id;
	l = cmd->device->lun;
	ha = (scsi_qla_host_t *)cmd->device->host->hostdata;

	tq = TGT_Q(ha, t);
	if (tq == NULL) {
		qla_printk(KERN_INFO, ha,
		    "%s(): **** CMD derives a NULL TGT_Q\n", __func__);

		return (return_status);
	}
	lq = (os_lun_t *)LUN_Q(ha, t, l);
	if (lq == NULL) {
		printk(KERN_INFO
		    "%s(): **** CMD derives a NULL LUN_Q\n", __func__);

		return (return_status);
	}
	fcport_to_reset = lq->fclun->fcport;

	/* If we are coming in from the back-door, stall I/O until complete. */
	if (!cmd->device->host->eh_active)
		set_bit(TQF_SUSPENDED, &tq->flags);

	qla_printk(KERN_INFO, ha,
	    "scsi(%ld:%d:%d:%d): DEVICE RESET ISSUED.\n", ha->host_no, b, t, l);

	DEBUG2(printk(KERN_INFO
	    "scsi(%ld): DEVICE_RESET cmd=%p jiffies = 0x%lx, timeout=%x, "
	    "dpc_flags=%lx, status=%x allowed=%d cmd.state=%x\n",
	    ha->host_no, cmd, jiffies, cmd->timeout_per_command / HZ,
	    ha->dpc_flags, cmd->result, cmd->allowed, cmd->state));

 	/* Clear commands from the retry queue. */
 	spin_lock(&ha->list_lock);
 	list_for_each_safe(list, temp, &ha->retry_queue) {
 		rp = list_entry(list, srb_t, list);
 
 		if (t != rp->cmd->device->id) 
 			continue;
 
 		DEBUG2(printk(KERN_INFO
		    "qla2xxx_eh_reset: found in retry queue. SP=%p\n", rp));
 
 		__del_from_retry_queue(ha, rp);
 		rp->cmd->result = DID_RESET << 16;
 		__add_to_done_queue(ha, rp);
 	}
 	spin_unlock(&ha->list_lock);

	spin_unlock_irq(ha->host->host_lock);

	if (qla2x00_wait_for_hba_online(ha) != QLA_SUCCESS) {
		DEBUG2(printk(KERN_INFO
		    "%s failed:board disabled\n",__func__));

		spin_lock_irq(ha->host->host_lock);
		goto eh_dev_reset_done;
	}

	if (qla2x00_wait_for_loop_ready(ha) == QLA_SUCCESS) {
		if (qla2x00_device_reset(ha, fcport_to_reset) == 0) {
			return_status = SUCCESS;
		}

#if defined(LOGOUT_AFTER_DEVICE_RESET)
		if (return_status == SUCCESS) {
			if (fcport_to_reset->flags & FC_FABRIC_DEVICE) {
				qla2x00_fabric_logout(ha,
				    fcport_to_reset->loop_id);
				qla2x00_mark_device_lost(ha, fcport_to_reset);
			}
		}
#endif
	} else {
		DEBUG2(printk(KERN_INFO
		    "%s failed: loop not ready\n",__func__);)
	}

	spin_lock_irq(ha->host->host_lock);

	if (return_status == FAILED) {
		DEBUG3(printk("%s(%ld): device reset failed\n",
		    __func__, ha->host_no));
		qla_printk(KERN_INFO, ha, "%s: device reset failed\n",
		    __func__);

		goto eh_dev_reset_done;
	}

	/*
	 * If we are coming down the EH path, wait for all commands to
	 * complete for the device.
	 */
	if (cmd->device->host->eh_active) {
		if (qla2x00_eh_wait_for_pending_target_commands(ha, t))
			return_status = FAILED;

		if (return_status == FAILED) {
			DEBUG3(printk("%s(%ld): failed while waiting for "
			    "commands\n", __func__, ha->host_no));
			qla_printk(KERN_INFO, ha,
			    "%s: failed while waiting for commands\n",
			    __func__); 

			goto eh_dev_reset_done;
		}
	}

	qla_printk(KERN_INFO, ha,
	    "scsi(%ld:%d:%d:%d): DEVICE RESET SUCCEEDED.\n",
	    ha->host_no, b, t, l);

eh_dev_reset_done:

	if (!cmd->device->host->eh_active)
		clear_bit(TQF_SUSPENDED, &tq->flags);

	return (return_status);
}

/**************************************************************************
* qla2x00_eh_wait_for_pending_commands
*
* Description:
*    Waits for all the commands to come back from the specified host.
*
* Input:
*    ha - pointer to scsi_qla_host structure.
*
* Returns:
*    1 : SUCCESS
*    0 : FAILED
*
* Note:
**************************************************************************/
int
qla2x00_eh_wait_for_pending_commands(scsi_qla_host_t *ha)
{
	int	cnt;
	int	status;
	srb_t		*sp;
	struct scsi_cmnd *cmd;

	status = 1;

	/*
	 * Waiting for all commands for the designated target in the active
	 * array
	 */
	for (cnt = 1; cnt < MAX_OUTSTANDING_COMMANDS; cnt++) {
		spin_lock(&ha->hardware_lock);
		sp = ha->outstanding_cmds[cnt];
		if (sp) {
			cmd = sp->cmd;
			spin_unlock(&ha->hardware_lock);
			status = qla2x00_eh_wait_on_command(ha, cmd);
			if (status == 0)
				break;
		}
		else {
			spin_unlock(&ha->hardware_lock);
		}
	}
	return (status);
}


/**************************************************************************
* qla2xxx_eh_bus_reset
*
* Description:
*    The bus reset function will reset the bus and abort any executing
*    commands.
*
* Input:
*    cmd = Linux SCSI command packet of the command that cause the
*          bus reset.
*
* Returns:
*    SUCCESS/FAILURE (defined as macro in scsi.h).
*
**************************************************************************/
int
qla2xxx_eh_bus_reset(struct scsi_cmnd *cmd)
{
	scsi_qla_host_t *ha;
	srb_t *sp;
	int rval = FAILED;

	ha = (scsi_qla_host_t *) cmd->device->host->hostdata;
	sp = (srb_t *) CMD_SP(cmd);

	qla_printk(KERN_INFO, ha,
	    "scsi(%ld:%d:%d:%d): LOOP RESET ISSUED.\n", ha->host_no,
	    cmd->device->channel, cmd->device->id, cmd->device->lun);

	spin_unlock_irq(ha->host->host_lock);

	if (qla2x00_wait_for_hba_online(ha) != QLA_SUCCESS) {
		DEBUG2(printk("%s failed:board disabled\n",__func__));
		spin_lock_irq(ha->host->host_lock);
		return FAILED;
	}

	if (qla2x00_wait_for_loop_ready(ha) == QLA_SUCCESS) {
		if (qla2x00_loop_reset(ha) == QLA_SUCCESS) 
			rval = SUCCESS;
	}

	spin_lock_irq(ha->host->host_lock);
	if (rval == FAILED)
		goto out;

	/*
	 * Blocking Call. It goes to sleep waiting for cmd to get to done q
	 *
	 * XXX(hch): really?  We're under host_lock here..
	 */
	/* Waiting for our command in done_queue to be returned to OS.*/
	if (!qla2x00_eh_wait_for_pending_commands(ha))
		rval = FAILED;

 out:
	qla_printk(KERN_INFO, ha, "%s: reset %s\n", __func__,
			(rval == FAILED) ? "failed" : "succeded");

	return rval;
}

/**************************************************************************
* qla2xxx_eh_host_reset
*
* Description:
*    The reset function will reset the Adapter.
*
* Input:
*      cmd = Linux SCSI command packet of the command that cause the
*            adapter reset.
*
* Returns:
*      Either SUCCESS or FAILED.
*
* Note:
**************************************************************************/
int
qla2xxx_eh_host_reset(struct scsi_cmnd *cmd)
{
	scsi_qla_host_t *ha = (scsi_qla_host_t *)cmd->device->host->hostdata;
	int		rval = SUCCESS;

	/* Display which one we're actually resetting for debug. */
	DEBUG(printk("qla2xxx_eh_host_reset:Resetting scsi(%ld).\n",
	    ha->host_no));

	/*
	 *  Now issue reset.
	 */
	qla_printk(KERN_INFO, ha,
	    "scsi(%ld:%d:%d:%d): ADAPTER RESET issued.\n", ha->host_no,
	    cmd->device->channel, cmd->device->id, cmd->device->lun);

	spin_unlock_irq(ha->host->host_lock);

	if (qla2x00_wait_for_hba_online(ha) != QLA_SUCCESS)
		goto board_disabled;

	/*
	 * Fixme-may be dpc thread is active and processing
	 * loop_resync,so wait a while for it to 
	 * be completed and then issue big hammer.Otherwise
	 * it may cause I/O failure as big hammer marks the
	 * devices as lost kicking of the port_down_timer
	 * while dpc is stuck for the mailbox to complete.
	 */
	/* Blocking call-Does context switching if loop is Not Ready */
	qla2x00_wait_for_loop_ready(ha);
	set_bit(ABORT_ISP_ACTIVE, &ha->dpc_flags);
	if (qla2x00_abort_isp(ha)) {
		clear_bit(ABORT_ISP_ACTIVE, &ha->dpc_flags);
		/* failed. schedule dpc to try */
		set_bit(ISP_ABORT_NEEDED, &ha->dpc_flags);

		if (qla2x00_wait_for_hba_online(ha) != QLA_SUCCESS)
			goto board_disabled;
	} 

	clear_bit(ABORT_ISP_ACTIVE, &ha->dpc_flags);

	spin_lock_irq(ha->host->host_lock);
	if (rval == FAILED)
		goto out;

	/* Waiting for our command in done_queue to be returned to OS.*/
	if (qla2x00_eh_wait_for_pending_commands(ha))
		rval = FAILED;

 out:
	qla_printk(KERN_INFO, ha, "%s: reset %s\n", __func__,
			(rval == FAILED) ? "failed" : "succeded");

	return rval;

 board_disabled:
	spin_lock_irq(ha->host->host_lock);

	qla_printk(KERN_INFO, ha, "%s: failed:board disabled\n", __func__);
	return FAILED;
}


/*
* qla2x00_loop_reset
*      Issue loop reset.
*
* Input:
*      ha = adapter block pointer.
*
* Returns:
*      0 = success
*/
static int
qla2x00_loop_reset(scsi_qla_host_t *ha)
{
	int status = QLA_SUCCESS;
	uint16_t t;
	os_tgt_t        *tq;

	if (ha->flags.enable_lip_reset) {
		status = qla2x00_lip_reset(ha);
	}

	if (status == QLA_SUCCESS && ha->flags.enable_target_reset) {
		for (t = 0; t < MAX_FIBRE_DEVICES; t++) {
			if ((tq = TGT_Q(ha, t)) == NULL)
				continue;

			if (tq->fcport == NULL)
				continue;

			status = qla2x00_target_reset(ha, 0, t);
			if (status != QLA_SUCCESS) {
				break;
			}
		}
	}

	if (status == QLA_SUCCESS &&
		((!ha->flags.enable_target_reset && 
		  !ha->flags.enable_lip_reset) ||
		ha->flags.enable_lip_full_login)) {

		status = qla2x00_full_login_lip(ha);
	}

	/* Issue marker command only when we are going to start the I/O */
	ha->marker_needed = 1;

	if (status) {
		/* Empty */
		DEBUG2_3(printk("%s(%ld): **** FAILED ****\n",
				__func__,
				ha->host_no);)
	} else {
		/* Empty */
		DEBUG3(printk("%s(%ld): exiting normally.\n",
				__func__,
				ha->host_no);)
	}

	return(status);
}

/*
 * qla2x00_device_reset
 *	Issue bus device reset message to the target.
 *
 * Input:
 *	ha = adapter block pointer.
 *	t = SCSI ID.
 *	TARGET_QUEUE_LOCK must be released.
 *	ADAPTER_STATE_LOCK must be released.
 *
 * Context:
 *	Kernel context.
 */
static int
qla2x00_device_reset(scsi_qla_host_t *ha, fc_port_t *reset_fcport)
{
	/* Abort Target command will clear Reservation */
	return qla2x00_abort_target(reset_fcport);
}

/**************************************************************************
* qla2xxx_slave_configure
*
* Description:
**************************************************************************/
int
qla2xxx_slave_configure(struct scsi_device *sdev)
{
	scsi_qla_host_t *ha = to_qla_host(sdev->host);
	int queue_depth;

	if (IS_QLA2100(ha) || IS_QLA2200(ha))
		queue_depth = 16;
	else
		queue_depth = 32;

	if (sdev->tagged_supported) {
		if (ql2xmaxqdepth != 0 && ql2xmaxqdepth <= 0xffffU)
			queue_depth = ql2xmaxqdepth;

		ql2xmaxqdepth = queue_depth;

		scsi_adjust_queue_depth(sdev, MSG_ORDERED_TAG, queue_depth);

		qla_printk(KERN_INFO, ha,
		    "scsi(%d:%d:%d:%d): Enabled tagged queuing, queue "
		    "depth %d.\n",
		    sdev->host->host_no, sdev->channel, sdev->id, sdev->lun,
		    sdev->queue_depth);
	} else {
		 scsi_adjust_queue_depth(sdev, 0 /* TCQ off */,
		     sdev->host->hostt->cmd_per_lun /* 3 */);
	}

	return (0);
}

/**
 * qla2x00_config_dma_addressing() - Configure OS DMA addressing method.
 * @ha: HA context
 *
 * At exit, the @ha's flags.enable_64bit_addressing set to indicated
 * supported addressing method.
 */
static void
qla2x00_config_dma_addressing(scsi_qla_host_t *ha)
{
	/* Assume 32bit DMA address */
	ha->flags.enable_64bit_addressing = 0;
	ha->calc_request_entries = qla2x00_calc_iocbs_32;
	ha->build_scsi_iocbs = qla2x00_build_scsi_iocbs_32;

	/*
	 * Given the two variants pci_set_dma_mask(), allow the compiler to
	 * assist in setting the proper dma mask.
	 */
	if (sizeof(dma_addr_t) > 4) {
		/* Update our PCI device dma_mask for full 64 bits */
		if (pci_set_dma_mask(ha->pdev, 0xffffffffffffffffULL) == 0) {
			ha->flags.enable_64bit_addressing = 1;
			ha->calc_request_entries = qla2x00_calc_iocbs_64;
			ha->build_scsi_iocbs = qla2x00_build_scsi_iocbs_64;

			if (pci_set_consistent_dma_mask(ha->pdev,
			    0xffffffffffffffffULL)) {
				qla_printk(KERN_DEBUG, ha, 
				    "Failed to set 64 bit PCI consistent mask; "
				    "using 32 bit.\n");
				pci_set_consistent_dma_mask(ha->pdev,
				    0xffffffffULL);
			}
		} else {
			qla_printk(KERN_DEBUG, ha,
			    "Failed to set 64 bit PCI DMA mask, falling back "
			    "to 32 bit MASK.\n");
			pci_set_dma_mask(ha->pdev, 0xffffffff);
		}
	} else {
		pci_set_dma_mask(ha->pdev, 0xffffffff);
	}
}

static int
qla2x00_iospace_config(scsi_qla_host_t *ha)
{
	unsigned long	pio, pio_len, pio_flags;
	unsigned long	mmio, mmio_len, mmio_flags;

	/* We only need PIO for Flash operations on ISP2312 v2 chips. */
	pio = pci_resource_start(ha->pdev, 0);
	pio_len = pci_resource_len(ha->pdev, 0);
	pio_flags = pci_resource_flags(ha->pdev, 0);
	if (pio_flags & IORESOURCE_IO) {
		if (pio_len < MIN_IOBASE_LEN) {
			qla_printk(KERN_WARNING, ha,
			    "Invalid PCI I/O region size (%s)...\n",
			    ha->pdev->slot_name);
			pio = 0;
		}
	} else {
		qla_printk(KERN_WARNING, ha,
		    "region #0 not a PIO resource (%s)...\n",
		    ha->pdev->slot_name);
		pio = 0;
	}

	/* Use MMIO operations for all accesses. */
	mmio = pci_resource_start(ha->pdev, 1);
	mmio_len = pci_resource_len(ha->pdev, 1);
	mmio_flags = pci_resource_flags(ha->pdev, 1);

	if (!(mmio_flags & IORESOURCE_MEM)) {
		qla_printk(KERN_ERR, ha,
		    "region #0 not an MMIO resource (%s), aborting\n",
		    ha->pdev->slot_name);
		goto iospace_error_exit;
	}
	if (mmio_len < MIN_IOBASE_LEN) {
		qla_printk(KERN_ERR, ha,
		    "Invalid PCI mem region size (%s), aborting\n",
		    ha->pdev->slot_name);
		goto iospace_error_exit;
	}

	if (pci_request_regions(ha->pdev, ha->brd_info->drv_name)) {
		qla_printk(KERN_WARNING, ha,
		    "Failed to reserve PIO/MMIO regions (%s)\n", 
		    ha->pdev->slot_name);

		goto iospace_error_exit;
	}

	ha->pio_address = pio;
	ha->pio_length = pio_len;
	ha->mmio_address = ioremap(mmio, MIN_IOBASE_LEN);
	if (!ha->mmio_address) {
		qla_printk(KERN_ERR, ha,
		    "cannot remap MMIO (%s), aborting\n", ha->pdev->slot_name);

		goto iospace_error_exit;
	}
	ha->mmio_length = mmio_len;
	ha->iobase = (device_reg_t *) ha->mmio_address;

	return (0);

iospace_error_exit:
	return (-ENOMEM);
}

/*
 * PCI driver interface
 */
int qla2x00_probe_one(struct pci_dev *pdev, struct qla_board_info *brd_info)
{
	int	ret;
	device_reg_t *reg;
	struct Scsi_Host *host;
	scsi_qla_host_t *ha;
	unsigned long	flags = 0;
	unsigned long	wait_switch = 0;
	char pci_info[20];
	char fw_str[30];

	if (pci_enable_device(pdev))
		return -1;

	host = scsi_host_alloc(&qla2x00_driver_template,
	    sizeof(scsi_qla_host_t));
	if (host == NULL) {
		printk(KERN_WARNING
		    "qla2xxx: Couldn't allocate host from scsi layer!\n");
		return -1;
	}

	/* Clear our data area */
	ha = (scsi_qla_host_t *)host->hostdata;
	memset(ha, 0, sizeof(scsi_qla_host_t));

	ha->pdev = pdev;
	ha->host = host;
	ha->host_no = host->host_no;
	ha->brd_info = brd_info;
	sprintf(ha->host_str, "%s_%ld", ha->brd_info->drv_name, ha->host_no);

	/* Configure PCI I/O space */
	ret = qla2x00_iospace_config(ha);
	if (ret != 0) {
		goto probe_failed;
	}

	/* Sanitize the information from PCI BIOS. */
	host->irq = pdev->irq;

	qla_printk(KERN_INFO, ha,
	    "Found an %s, irq %d, iobase 0x%p\n", ha->brd_info->isp_name,
	    host->irq, ha->iobase);

	spin_lock_init(&ha->hardware_lock);

	/* 4.23 Initialize /proc/scsi/qla2x00 counters */
	ha->actthreads = 0;
	ha->qthreads   = 0;
	ha->total_isr_cnt = 0;
	ha->total_isp_aborts = 0;
	ha->total_lip_cnt = 0;
	ha->total_dev_errs = 0;
	ha->total_ios = 0;
	ha->total_bytes = 0;

	ha->prev_topology = 0;
	ha->ports = MAX_BUSES;

	if (IS_QLA2100(ha)) {
		ha->max_targets = MAX_TARGETS_2100;
		ha->mbx_count = MAILBOX_REGISTER_COUNT_2100;
		ha->request_q_length = REQUEST_ENTRY_CNT_2100;
		ha->response_q_length = RESPONSE_ENTRY_CNT_2100;
		ha->last_loop_id = SNS_LAST_LOOP_ID_2100;
		host->sg_tablesize = 32;
	} else if (IS_QLA2200(ha)) {
		ha->max_targets = MAX_TARGETS_2200;
		ha->mbx_count = MAILBOX_REGISTER_COUNT;
		ha->request_q_length = REQUEST_ENTRY_CNT_2200;
		ha->response_q_length = RESPONSE_ENTRY_CNT_2100;
		ha->last_loop_id = SNS_LAST_LOOP_ID_2100;
	} else /*if (IS_QLA2300(ha))*/ {
		ha->max_targets = MAX_TARGETS_2200;
		ha->mbx_count = MAILBOX_REGISTER_COUNT;
		ha->request_q_length = REQUEST_ENTRY_CNT_2200;
		ha->response_q_length = RESPONSE_ENTRY_CNT_2300;
		ha->last_loop_id = SNS_LAST_LOOP_ID_2300;
	}
	host->can_queue = ha->request_q_length + 128;

	/* load the F/W, read paramaters, and init the H/W */
	ha->instance = num_hosts;

	init_MUTEX(&ha->mbx_cmd_sem);
	init_MUTEX_LOCKED(&ha->mbx_intr_sem);

	INIT_LIST_HEAD(&ha->list);
	INIT_LIST_HEAD(&ha->fcports);
	INIT_LIST_HEAD(&ha->rscn_fcports);
	INIT_LIST_HEAD(&ha->done_queue);
	INIT_LIST_HEAD(&ha->retry_queue);
	INIT_LIST_HEAD(&ha->scsi_retry_queue);
	INIT_LIST_HEAD(&ha->pending_queue);

	/*
	 * These locks are used to prevent more than one CPU
	 * from modifying the queue at the same time. The
	 * higher level "host_lock" will reduce most
	 * contention for these locks.
	 */
	spin_lock_init(&ha->mbx_reg_lock);
	spin_lock_init(&ha->list_lock);

	ha->dpc_pid = -1;
	init_completion(&ha->dpc_inited);
	init_completion(&ha->dpc_exited);

	qla2x00_config_dma_addressing(ha);
	if (qla2x00_mem_alloc(ha)) {
		qla_printk(KERN_WARNING, ha,
		    "[ERROR] Failed to allocate memory for adapter\n");

		goto probe_failed;
	}

	if (qla2x00_initialize_adapter(ha) &&
	    !(ha->device_flags & DFLG_NO_CABLE)) {

		qla_printk(KERN_WARNING, ha,
		    "Failed to initialize adapter\n");

		DEBUG2(printk("scsi(%ld): Failed to initialize adapter - "
		    "Adapter flags %x.\n",
		    ha->host_no, ha->device_flags));

		goto probe_failed;
	}

	/*
	 * Startup the kernel thread for this host adapter
	 */
	ha->dpc_should_die = 0;
	ha->dpc_pid = kernel_thread(qla2x00_do_dpc, ha, 0);
	if (ha->dpc_pid < 0) {
		qla_printk(KERN_WARNING, ha,
		    "Unable to start DPC thread!\n");

		goto probe_failed;
	}
	wait_for_completion(&ha->dpc_inited);

	host->this_id = 255;
	host->cmd_per_lun = 3;
	host->max_cmd_len = MAX_CMDSZ;
	host->max_channel = ha->ports - 1;
	host->max_lun = ha->max_luns;
	BUG_ON(qla2xxx_transport_template == NULL);
	host->transportt = qla2xxx_transport_template;
	host->unique_id = ha->instance;
	host->max_id = ha->max_targets;

	if (request_irq(host->irq, qla2x00_intr_handler, SA_INTERRUPT|SA_SHIRQ,
	    ha->brd_info->drv_name, ha)) {
		qla_printk(KERN_WARNING, ha,
		    "Failed to reserve interrupt %d already in use.\n",
		    host->irq);
		goto probe_failed;
	}

	/* Initialized the timer */
	qla2x00_start_timer(ha, qla2x00_timer, WATCH_INTERVAL);

	DEBUG2(printk("DEBUG: detect hba %ld at address = %p\n",
	    ha->host_no, ha));

	reg = ha->iobase;

	/* Disable ISP interrupts. */
	qla2x00_disable_intrs(ha);

	/* Ensure mailbox registers are free. */
	spin_lock_irqsave(&ha->hardware_lock, flags);
	WRT_REG_WORD(&reg->semaphore, 0);
	WRT_REG_WORD(&reg->hccr, HCCR_CLR_RISC_INT);
	WRT_REG_WORD(&reg->hccr, HCCR_CLR_HOST_INT);

	/* Enable proper parity */
	if (!IS_QLA2100(ha) && !IS_QLA2200(ha)) {
		if (IS_QLA2300(ha))
			/* SRAM parity */
			WRT_REG_WORD(&reg->hccr, (HCCR_ENABLE_PARITY + 0x1));
		else
			/* SRAM, Instruction RAM and GP RAM parity */
			WRT_REG_WORD(&reg->hccr, (HCCR_ENABLE_PARITY + 0x7));
	}
	spin_unlock_irqrestore(&ha->hardware_lock, flags);

	/* Enable chip interrupts. */
	qla2x00_enable_intrs(ha);

	/* v2.19.5b6 */
	/*
	 * Wait around max loop_reset_delay secs for the devices to come
	 * on-line. We don't want Linux scanning before we are ready.
	 *
	 */
	for (wait_switch = jiffies + (ha->loop_reset_delay * HZ);
	    time_before(jiffies,wait_switch) &&
	     !(ha->device_flags & (DFLG_NO_CABLE | DFLG_FABRIC_DEVICES))
	     && (ha->device_flags & SWITCH_FOUND) ;) {

		qla2x00_check_fabric_devices(ha);

		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ/100);
	}

	pci_set_drvdata(pdev, ha);
	ha->flags.init_done = 1;
	num_hosts++;

	/* List the target we have found */
	if (displayConfig) {
		qla2x00_display_fc_names(ha);
	}

	if (scsi_add_host(host, &pdev->dev))
		goto probe_failed;

	sysfs_create_bin_file(&host->shost_gendev.kobj, &sysfs_fw_dump_attr);
	sysfs_create_bin_file(&host->shost_gendev.kobj, &sysfs_nvram_attr);

	qla_printk(KERN_INFO, ha, "\n"
	    " QLogic Fibre Channel HBA Driver: %s\n"
	    "  QLogic %s - %s\n"
	    "  %s: %s @ %s hdma%c, host#=%ld, fw=%s\n", qla2x00_version_str,
	    ha->model_number, ha->model_desc ? ha->model_desc: "",
	    ha->brd_info->isp_name, qla2x00_get_pci_info_str(ha, pci_info),
	    pci_name(ha->pdev), ha->flags.enable_64bit_addressing ? '+': '-',
	    ha->host_no, qla2x00_get_fw_version_str(ha, fw_str));

	if (ql2xdoinitscan)
		scsi_scan_host(host);

	return 0;

probe_failed:
	qla2x00_free_device(ha);

	scsi_host_put(host);

	return -1;
}
EXPORT_SYMBOL_GPL(qla2x00_probe_one);

void qla2x00_remove_one(struct pci_dev *pdev)
{
	scsi_qla_host_t *ha;

	ha = pci_get_drvdata(pdev);

	sysfs_remove_bin_file(&ha->host->shost_gendev.kobj,
	    &sysfs_fw_dump_attr);
	sysfs_remove_bin_file(&ha->host->shost_gendev.kobj, &sysfs_nvram_attr);

	scsi_remove_host(ha->host);

	qla2x00_free_device(ha);

	scsi_host_put(ha->host);

	pci_set_drvdata(pdev, NULL);
}
EXPORT_SYMBOL_GPL(qla2x00_remove_one);

static void
qla2x00_free_device(scsi_qla_host_t *ha)
{
	int ret;

	/* Abort any outstanding IO descriptors. */
	if (!IS_QLA2100(ha) && !IS_QLA2200(ha))
		qla2x00_cancel_io_descriptors(ha);

	/* turn-off interrupts on the card */
	if (ha->interrupts_on)
		qla2x00_disable_intrs(ha);

	/* Disable timer */
	if (ha->timer_active)
		qla2x00_stop_timer(ha);

	/* Kill the kernel thread for this host */
	if (ha->dpc_pid >= 0) {
		ha->dpc_should_die = 1;
		wmb();
		ret = kill_proc(ha->dpc_pid, SIGHUP, 1);
		if (ret) {
			qla_printk(KERN_ERR, ha,
			    "Unable to signal DPC thread -- (%d)\n", ret);

			/* TODO: SOMETHING MORE??? */
		} else {
			wait_for_completion(&ha->dpc_exited);
		}
	}

	qla2x00_mem_free(ha);


	ha->flags.online = 0;

	/* Detach interrupts */
	if (ha->pdev->irq)
		free_irq(ha->pdev->irq, ha);

	/* release io space registers  */
	pci_release_regions(ha->pdev);

#if MEMORY_MAPPED_IO
	if (ha->mmio_address)
		iounmap(ha->mmio_address);
#endif
}


/*
 * The following support functions are adopted to handle
 * the re-entrant qla2x00_proc_info correctly.
 */
static void
copy_mem_info(struct info_str *info, char *data, int len)
{
	if (info->pos + len > info->offset + info->length)
		len = info->offset + info->length - info->pos;

	if (info->pos + len < info->offset) {
		info->pos += len;
		return;
	}
 
	if (info->pos < info->offset) {
		off_t partial;
 
		partial = info->offset - info->pos;
		data += partial;
		info->pos += partial;
		len  -= partial;
	}
 
	if (len > 0) {
		memcpy(info->buffer, data, len);
		info->pos += len;
		info->buffer += len;
	}
}

static int
copy_info(struct info_str *info, char *fmt, ...)
{
	va_list args;
	char buf[256];
	int len;
 
	va_start(args, fmt);
	len = vsprintf(buf, fmt, args);
	va_end(args);
 
	copy_mem_info(info, buf, len);

	return (len);
}

/*************************************************************************
* qla2x00_proc_info
*
* Description:
*   Return information to handle /proc support for the driver.
*
* inout : decides the direction of the dataflow and the meaning of the
*         variables
* buffer: If inout==0 data is being written to it else read from it
*         (ptr to a page buffer)
* *start: If inout==0 start of the valid data in the buffer
* offset: If inout==0 starting offset from the beginning of all
*         possible data to return.
* length: If inout==0 max number of bytes to be written into the buffer
*         else number of bytes in "buffer"
* Returns:
*         < 0:  error. errno value.
*         >= 0: sizeof data returned.
*************************************************************************/
int
qla2x00_proc_info(struct Scsi_Host *shost, char *buffer,
    char **start, off_t offset, int length, int inout)
{
	struct info_str	info;
	int             retval = -EINVAL;
	os_lun_t	*up;
	os_tgt_t	*tq;
	unsigned int	t, l;
	uint32_t        tmp_sn;
	uint32_t	*flags;
	uint8_t		*loop_state;
	scsi_qla_host_t *ha;
	char fw_info[30];
 
	DEBUG3(printk(KERN_INFO
	    "Entering proc_info buff_in=%p, offset=0x%lx, length=0x%x\n",
	    buffer, offset, length);)

	ha = (scsi_qla_host_t *) shost->hostdata;

	if (inout) {
		/* Has data been written to the file? */
		DEBUG3(printk(
		    "%s: has data been written to the file. \n",
		    __func__);)

		return (qla2x00_set_info(buffer, length, shost));
	}

	if (start) {
		*start = buffer;
	}

	info.buffer = buffer;
	info.length = length;
	info.offset = offset;
	info.pos    = 0;

	/* start building the print buffer */
	copy_info(&info,
	    "QLogic PCI to Fibre Channel Host Adapter for %s:\n"
	    "        Firmware version %s, ",
	    ha->model_number, qla2x00_get_fw_version_str(ha, fw_info));

	copy_info(&info, "Driver version %s\n", qla2x00_version_str);

	copy_info(&info, "Entry address = %p\n", qla2x00_set_info);

	tmp_sn = ((ha->serial0 & 0x1f) << 16) | (ha->serial2 << 8) | 
	    ha->serial1;
	copy_info(&info, "ISP: %s, Serial# %c%05d\n",
	    ha->brd_info->isp_name, ('A' + tmp_sn/100000), (tmp_sn%100000));

	copy_info(&info,
	    "Request Queue = 0x%llx, Response Queue = 0x%llx\n",
		(unsigned long long)ha->request_dma,
		(unsigned long long)ha->response_dma);

	copy_info(&info,
	    "Request Queue count = %d, Response Queue count = %d\n",
	    ha->request_q_length, ha->response_q_length);

	copy_info(&info,
	    "Total number of active commands = %ld\n",
	    ha->actthreads);

	copy_info(&info,
	    "Total number of interrupts = %ld\n",
	    (long)ha->total_isr_cnt);

	copy_info(&info,
	    "    Device queue depth = 0x%x\n",
	    (ql2xmaxqdepth == 0) ? 16 : ql2xmaxqdepth);

	copy_info(&info,
	    "Number of free request entries = %d\n", ha->req_q_cnt);

	copy_info(&info,
	    "Number of mailbox timeouts = %ld\n", ha->total_mbx_timeout);

	copy_info(&info,
	    "Number of ISP aborts = %ld\n", ha->total_isp_aborts);

	copy_info(&info,
	    "Number of loop resyncs = %ld\n", ha->total_loop_resync);

	copy_info(&info,
	    "Number of retries for empty slots = %ld\n",
	    qla2x00_stats.outarray_full);

	copy_info(&info,
	    "Number of reqs in pending_q= %ld, retry_q= %d, "
	    "done_q= %ld, scsi_retry_q= %d\n",
	    ha->qthreads, ha->retry_q_cnt,
	    ha->done_q_cnt, ha->scsi_retry_q_cnt);


	flags = (uint32_t *) &ha->flags;

	if (atomic_read(&ha->loop_state) == LOOP_DOWN) {
		loop_state = "DOWN";
	} else if (atomic_read(&ha->loop_state) == LOOP_UP) {
		loop_state = "UP";
	} else if (atomic_read(&ha->loop_state) == LOOP_READY) {
		loop_state = "READY";
	} else if (atomic_read(&ha->loop_state) == LOOP_TIMEOUT) {
		loop_state = "TIMEOUT";
	} else if (atomic_read(&ha->loop_state) == LOOP_UPDATE) {
		loop_state = "UPDATE";
	} else {
		loop_state = "UNKNOWN";
	}

	copy_info(&info, 
	    "Host adapter:loop state = <%s>, flags = 0x%lx\n",
	    loop_state , *flags);

	copy_info(&info, "Dpc flags = 0x%lx\n", ha->dpc_flags);

	copy_info(&info, "MBX flags = 0x%x\n", ha->mbx_flags);

	copy_info(&info, "Link down Timeout = %3.3d\n",
	    ha->link_down_timeout);

	copy_info(&info, "Port down retry = %3.3d\n",
	    ha->port_down_retry_count);

	copy_info(&info, "Login retry count = %3.3d\n",
	    ha->login_retry_count);

	copy_info(&info,
	    "Commands retried with dropped frame(s) = %d\n",
	    ha->dropped_frame_error_cnt);

	copy_info(&info,
	    "Product ID = %04x %04x %04x %04x\n", ha->product_id[0],
	    ha->product_id[1], ha->product_id[2], ha->product_id[3]);

	copy_info(&info, "\n");

	/* 2.25 node/port display to proc */
	/* Display the node name for adapter */
	copy_info(&info, "\nSCSI Device Information:\n");
	copy_info(&info,
	    "scsi-qla%d-adapter-node="
	    "%02x%02x%02x%02x%02x%02x%02x%02x;\n",
	    (int)ha->instance,
	    ha->init_cb->node_name[0],
	    ha->init_cb->node_name[1],
	    ha->init_cb->node_name[2],
	    ha->init_cb->node_name[3],
	    ha->init_cb->node_name[4],
	    ha->init_cb->node_name[5],
	    ha->init_cb->node_name[6],
	    ha->init_cb->node_name[7]);

	/* display the port name for adapter */
	copy_info(&info,
	    "scsi-qla%d-adapter-port="
	    "%02x%02x%02x%02x%02x%02x%02x%02x;\n",
	    (int)ha->instance,
	    ha->init_cb->port_name[0],
	    ha->init_cb->port_name[1],
	    ha->init_cb->port_name[2],
	    ha->init_cb->port_name[3],
	    ha->init_cb->port_name[4],
	    ha->init_cb->port_name[5],
	    ha->init_cb->port_name[6],
	    ha->init_cb->port_name[7]);

	/* Print out device port names */
	for (t = 0; t < MAX_FIBRE_DEVICES; t++) {
		if ((tq = TGT_Q(ha, t)) == NULL)
			continue;

		copy_info(&info,
		    "scsi-qla%d-target-%d="
		    "%02x%02x%02x%02x%02x%02x%02x%02x;\n",
		    (int)ha->instance, t,
		    tq->port_name[0], tq->port_name[1],
		    tq->port_name[2], tq->port_name[3],
		    tq->port_name[4], tq->port_name[5],
		    tq->port_name[6], tq->port_name[7]);
	}

	copy_info(&info, "\nSCSI LUN Information:\n");
	copy_info(&info,
	    "(Id:Lun)  * - indicates lun is not registered with the OS.\n");

	/* scan for all equipment stats */
	for (t = 0; t < MAX_FIBRE_DEVICES; t++) {
		/* scan all luns */
		for (l = 0; l < ha->max_luns; l++) {
			up = (os_lun_t *) GET_LU_Q(ha, t, l);

			if (up == NULL) {
				continue;
			}
			if (up->fclun == NULL) {
				continue;
			}

			copy_info(&info,
			    "(%2d:%2d): Total reqs %ld,",
			    t,l,up->io_cnt);

			copy_info(&info,
			    " Pending reqs %ld,",
			    up->out_cnt);

			if (up->io_cnt < 4) {
				copy_info(&info,
				    " flags 0x%x*,",
				    (int)up->q_flag);
			} else {
				copy_info(&info,
				    " flags 0x%x,",
				    (int)up->q_flag);
			}

			copy_info(&info, 
			    " %ld:%d:%02x %02x",
			    up->fclun->fcport->ha->instance,
			    up->fclun->fcport->cur_path,
			    up->fclun->fcport->loop_id,
			    up->fclun->device_type);

			copy_info(&info, "\n");

			if (info.pos >= info.offset + info.length) {
				/* No need to continue */
				goto profile_stop;
			}
		}

		if (info.pos >= info.offset + info.length) {
			/* No need to continue */
			break;
		}
	}

profile_stop:

	retval = info.pos > info.offset ? info.pos - info.offset : 0;

	DEBUG3(printk(KERN_INFO 
	    "Exiting proc_info: info.pos=%d, offset=0x%lx, "
	    "length=0x%x\n", info.pos, offset, length);)

	return (retval);
}

/*
* qla2x00_display_fc_names
*      This routine will the node names of the different devices found
*      after port inquiry.
*
* Input:
*      cmd = SCSI command structure
*
* Returns:
*      None.
*/
static void
qla2x00_display_fc_names(scsi_qla_host_t *ha) 
{
	uint16_t	tgt;
	os_tgt_t	*tq;

	/* Display the node name for adapter */
	qla_printk(KERN_INFO, ha,
	    "scsi-qla%d-adapter-node=%02x%02x%02x%02x%02x%02x%02x%02x\\;\n",
	    (int)ha->instance,
	    ha->init_cb->node_name[0],
	    ha->init_cb->node_name[1],
	    ha->init_cb->node_name[2],
	    ha->init_cb->node_name[3],
	    ha->init_cb->node_name[4],
	    ha->init_cb->node_name[5],
	    ha->init_cb->node_name[6],
	    ha->init_cb->node_name[7]);

	/* display the port name for adapter */
	qla_printk(KERN_INFO, ha,
	    "scsi-qla%d-adapter-port=%02x%02x%02x%02x%02x%02x%02x%02x\\;\n",
	    (int)ha->instance,
	    ha->init_cb->port_name[0],
	    ha->init_cb->port_name[1],
	    ha->init_cb->port_name[2],
	    ha->init_cb->port_name[3],
	    ha->init_cb->port_name[4],
	    ha->init_cb->port_name[5],
	    ha->init_cb->port_name[6],
	    ha->init_cb->port_name[7]);

	/* Print out device port names */
	for (tgt = 0; tgt < MAX_TARGETS; tgt++) {
		if ((tq = ha->otgt[tgt]) == NULL)
			continue;

		if (tq->fcport == NULL)
			continue;

		switch (ha->binding_type) {
			case BIND_BY_PORT_NAME:
				qla_printk(KERN_INFO, ha,
				    "scsi-qla%d-tgt-%d-di-0-port="
				    "%02x%02x%02x%02x%02x%02x%02x%02x\\;\n",
				    (int)ha->instance, 
				    tgt,
				    tq->port_name[0], 
				    tq->port_name[1],
				    tq->port_name[2], 
				    tq->port_name[3],
				    tq->port_name[4], 
				    tq->port_name[5],
				    tq->port_name[6], 
				    tq->port_name[7]);

				break;

			case BIND_BY_PORT_ID:
				qla_printk(KERN_INFO, ha,
				    "scsi-qla%d-tgt-%d-di-0-pid="
				    "%02x%02x%02x\\;\n",
				    (int)ha->instance,
				    tgt,
				    tq->d_id.b.domain,
				    tq->d_id.b.area,
				    tq->d_id.b.al_pa);
				break;
		}

#if VSA
		qla_printk(KERN_INFO, ha,
		    "scsi-qla%d-target-%d-vsa=01;\n", (int)ha->instance, tgt);
#endif
	}
}

/*
 *  qla2x00_suspend_lun
 *	Suspend lun and start port down timer
 *
 * Input:
 *	ha = visable adapter block pointer.
 *  lq = lun queue
 *  cp = Scsi command pointer 
 *  time = time in seconds
 *  count = number of times to let time expire
 *  delay_lun = non-zero, if lun should be delayed rather than suspended
 *
 * Return:
 *     QLA_SUCCESS  -- suspended lun 
 *     QLA_FUNCTION_FAILED  -- Didn't suspend lun
 *
 * Context:
 *	Interrupt context.
 */
int
__qla2x00_suspend_lun(scsi_qla_host_t *ha,
		os_lun_t *lq, int time, int count, int delay_lun)
{
	int	rval;
	srb_t *sp;
	struct list_head *list, *temp;
	unsigned long flags;

	rval = QLA_SUCCESS;

	/* if the lun_q is already suspended then don't do it again */
	if (lq->q_state == LUN_STATE_READY ||lq->q_state == LUN_STATE_RUN) {

		spin_lock_irqsave(&lq->q_lock, flags);
		if (lq->q_state == LUN_STATE_READY) {
			lq->q_max = count;
			lq->q_count = 0;
		}
		/* Set the suspend time usually 6 secs */
		atomic_set(&lq->q_timer, time);

		/* now suspend the lun */
		lq->q_state = LUN_STATE_WAIT;

		if (delay_lun) {
			set_bit(LUN_EXEC_DELAYED, &lq->q_flag);
			DEBUG(printk(KERN_INFO
			    "scsi(%ld): Delay lun execution for %d secs, "
			    "count=%d, max count=%d, state=%d\n",
			    ha->host_no,
			    time,
			    lq->q_count, lq->q_max, lq->q_state));
		} else {
			DEBUG(printk(KERN_INFO
			    "scsi(%ld): Suspend lun for %d secs, count=%d, "
			    "max count=%d, state=%d\n",
			    ha->host_no,
			    time,
			    lq->q_count, lq->q_max, lq->q_state));
		}
		spin_unlock_irqrestore(&lq->q_lock, flags);

		/*
		 * Remove all pending commands from request queue and  put them
		 * in the scsi_retry queue.
		 */
		spin_lock_irqsave(&ha->list_lock, flags);
		list_for_each_safe(list, temp, &ha->pending_queue) {
			sp = list_entry(list, srb_t, list);
			if (sp->lun_queue != lq)
				continue;

			__del_from_pending_queue(ha, sp);

			if (sp->cmd->allowed < count)
				sp->cmd->allowed = count;
			__add_to_scsi_retry_queue(ha, sp);

		} /* list_for_each_safe */
		spin_unlock_irqrestore(&ha->list_lock, flags);
		rval = QLA_SUCCESS;
	} else {
		rval = QLA_FUNCTION_FAILED;
	}

	return (rval);
}

/*
 * qla2x00_mark_device_lost Updates fcport state when device goes offline.
 *
 * Input: ha = adapter block pointer.  fcport = port structure pointer.
 *
 * Return: None.
 *
 * Context:
 */
void qla2x00_mark_device_lost(scsi_qla_host_t *ha, fc_port_t *fcport,
    int do_login)
{
	/* 
	 * We may need to retry the login, so don't change the state of the
	 * port but do the retries.
	 */
	if (atomic_read(&fcport->state) != FCS_DEVICE_DEAD)
		atomic_set(&fcport->state, FCS_DEVICE_LOST);

	if (!do_login)
		return;

	if (fcport->login_retry == 0) {
		fcport->login_retry = ha->login_retry_count;
		set_bit(RELOGIN_NEEDED, &ha->dpc_flags);

		DEBUG(printk("scsi(%ld): Port login retry: "
		    "%02x%02x%02x%02x%02x%02x%02x%02x, "
		    "id = 0x%04x retry cnt=%d\n",
		    ha->host_no,
		    fcport->port_name[0],
		    fcport->port_name[1],
		    fcport->port_name[2],
		    fcport->port_name[3],
		    fcport->port_name[4],
		    fcport->port_name[5],
		    fcport->port_name[6],
		    fcport->port_name[7],
		    fcport->loop_id,
		    fcport->login_retry));
	}
}

/*
 * qla2x00_mark_all_devices_lost
 *	Updates fcport state when device goes offline.
 *
 * Input:
 *	ha = adapter block pointer.
 *	fcport = port structure pointer.
 *
 * Return:
 *	None.
 *
 * Context:
 */
void
qla2x00_mark_all_devices_lost(scsi_qla_host_t *ha) 
{
	fc_port_t *fcport;

	list_for_each_entry(fcport, &ha->fcports, list) {
		if (fcport->port_type != FCT_TARGET)
			continue;

		/*
		 * No point in marking the device as lost, if the device is
		 * already DEAD.
		 */
		if (atomic_read(&fcport->state) == FCS_DEVICE_DEAD)
			continue;

		atomic_set(&fcport->state, FCS_DEVICE_LOST);
	}
}

/*
* qla2x00_mem_alloc
*      Allocates adapter memory.
*
* Returns:
*      0  = success.
*      1  = failure.
*/
static uint8_t
qla2x00_mem_alloc(scsi_qla_host_t *ha)
{
	uint8_t   status = 1;
	int	retry= 10;

	do {
		/*
		 * This will loop only once if everything goes well, else some
		 * number of retries will be performed to get around a kernel
		 * bug where available mem is not allocated until after a
		 * little delay and a retry.
		 */
		ha->request_ring = pci_alloc_consistent(ha->pdev,
		    ((ha->request_q_length + 1) * (sizeof(request_t))),
		    &ha->request_dma);
		if (ha->request_ring == NULL) {
			qla_printk(KERN_WARNING, ha,
			    "Memory Allocation failed - request_ring\n");

			qla2x00_mem_free(ha);
			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_timeout(HZ/10);

			continue;
		}

		ha->response_ring = pci_alloc_consistent(ha->pdev,
		    ((ha->response_q_length + 1) * (sizeof(response_t))),
		    &ha->response_dma);
		if (ha->response_ring == NULL) {
			qla_printk(KERN_WARNING, ha,
			    "Memory Allocation failed - response_ring\n");

			qla2x00_mem_free(ha);
			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_timeout(HZ/10);

			continue;
		}

		/* get consistent memory allocated for init control block */
		ha->init_cb = pci_alloc_consistent(ha->pdev,
		    sizeof(init_cb_t), &ha->init_cb_dma);
		if (ha->init_cb == NULL) {
			qla_printk(KERN_WARNING, ha,
			    "Memory Allocation failed - init_cb\n");

			qla2x00_mem_free(ha);
			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_timeout(HZ/10);

			continue;
		}
		memset(ha->init_cb, 0, sizeof(init_cb_t));

		/* Allocate ioctl related memory. */
		if (qla2x00_alloc_ioctl_mem(ha)) {
			qla_printk(KERN_WARNING, ha,
			    "Memory Allocation failed - ioctl_mem\n");

			qla2x00_mem_free(ha);
			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_timeout(HZ/10);

			continue;
		}

		if (qla2x00_allocate_sp_pool(ha)) {
			qla_printk(KERN_WARNING, ha,
			    "Memory Allocation failed - "
			    "qla2x00_allocate_sp_pool()\n");

			qla2x00_mem_free(ha);
			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_timeout(HZ/10);

			continue;
		}

		/* Allocate memory for SNS commands */
		if (IS_QLA2100(ha) || IS_QLA2200(ha)) {
			/* Get consistent memory allocated for SNS commands */
			ha->sns_cmd = pci_alloc_consistent(ha->pdev,
			    sizeof(struct sns_cmd_pkt), &ha->sns_cmd_dma);
			if (ha->sns_cmd == NULL) {
				/* error */
				qla_printk(KERN_WARNING, ha,
				    "Memory Allocation failed - sns_cmd\n");

				qla2x00_mem_free(ha);
				set_current_state(TASK_UNINTERRUPTIBLE);
				schedule_timeout(HZ/10);

				continue;
			}
			memset(ha->sns_cmd, 0, sizeof(struct sns_cmd_pkt));
		} else {
			/* Get consistent memory allocated for MS IOCB */
			ha->ms_iocb = pci_alloc_consistent(ha->pdev,
			    sizeof(ms_iocb_entry_t), &ha->ms_iocb_dma);
			if (ha->ms_iocb == NULL) {
				/* error */
				qla_printk(KERN_WARNING, ha,
				    "Memory Allocation failed - ms_iocb\n");

				qla2x00_mem_free(ha);
				set_current_state(TASK_UNINTERRUPTIBLE);
				schedule_timeout(HZ/10);

				continue;
			}
			memset(ha->ms_iocb, 0, sizeof(ms_iocb_entry_t));

			/*
			 * Get consistent memory allocated for CT SNS
			 * commands
			 */
			ha->ct_sns = pci_alloc_consistent(ha->pdev,
			    sizeof(struct ct_sns_pkt), &ha->ct_sns_dma);
			if (ha->ct_sns == NULL) {
				/* error */
				qla_printk(KERN_WARNING, ha,
				    "Memory Allocation failed - ct_sns\n");

				qla2x00_mem_free(ha);
				set_current_state(TASK_UNINTERRUPTIBLE);
				schedule_timeout(HZ/10);

				continue;
			}
			memset(ha->ct_sns, 0, sizeof(struct ct_sns_pkt));
		}

		/* Get consistent memory allocated for Get Port Database cmd */
		ha->iodesc_pd = pci_alloc_consistent(ha->pdev,
		    PORT_DATABASE_SIZE, &ha->iodesc_pd_dma);
		if (ha->iodesc_pd == NULL) {
			/* error */
			qla_printk(KERN_WARNING, ha,
			    "Memory Allocation failed - iodesc_pd\n");

			qla2x00_mem_free(ha);
			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_timeout(HZ/10);

			continue;
		}
		memset(ha->iodesc_pd, 0, PORT_DATABASE_SIZE);

		/* Done all allocations without any error. */
		status = 0;

	} while (retry-- && status != 0);

	if (status) {
		printk(KERN_WARNING
			"%s(): **** FAILED ****\n", __func__);
	}

	return(status);
}

/*
* qla2x00_mem_free
*      Frees all adapter allocated memory.
*
* Input:
*      ha = adapter block pointer.
*/
static void
qla2x00_mem_free(scsi_qla_host_t *ha)
{
	uint32_t	t;
	struct list_head	*fcpl, *fcptemp;
	fc_port_t	*fcport;
	struct list_head	*fcll, *fcltemp;
	fc_lun_t	*fclun;
	unsigned long	wtime;/* max wait time if mbx cmd is busy. */

	if (ha == NULL) {
		/* error */
		DEBUG2(printk("%s(): ERROR invalid ha pointer.\n", __func__));
		return;
	}

	/* Free the target queues */
	for (t = 0; t < MAX_TARGETS; t++) {
		qla2x00_tgt_free(ha, t);
	}

	/* Make sure all other threads are stopped. */
	wtime = 60 * HZ;
	while (ha->dpc_wait && wtime) {
		set_current_state(TASK_INTERRUPTIBLE);
		wtime = schedule_timeout(wtime);
	}

	/* free ioctl memory */
	qla2x00_free_ioctl_mem(ha);

	/* free sp pool */
	qla2x00_free_sp_pool(ha);

	if (ha->iodesc_pd) {
		pci_free_consistent(ha->pdev, PORT_DATABASE_SIZE,
		    ha->iodesc_pd, ha->iodesc_pd_dma);
	}

	if (ha->sns_cmd) {
		pci_free_consistent(ha->pdev,
		    sizeof(struct sns_cmd_pkt), ha->sns_cmd, ha->sns_cmd_dma);
	}

	if (ha->ct_sns) {
		pci_free_consistent(ha->pdev,
		    sizeof(struct ct_sns_pkt), ha->ct_sns, ha->ct_sns_dma);
	}
	if (ha->ms_iocb) {
		pci_free_consistent(ha->pdev,
		    sizeof(ms_iocb_entry_t), ha->ms_iocb, ha->ms_iocb_dma);
	}

	if (ha->init_cb) {
		pci_free_consistent(ha->pdev,
		    sizeof(init_cb_t), ha->init_cb, ha->init_cb_dma);
	}

	if (ha->request_ring) {
		pci_free_consistent(ha->pdev,
		    ((ha->request_q_length + 1) * (sizeof(request_t))),
		    ha->request_ring, ha->request_dma);
	}

	if (ha->response_ring) {
		pci_free_consistent(ha->pdev,
		    ((ha->response_q_length + 1) * (sizeof(response_t))),
		    ha->response_ring, ha->response_dma);
	}

	ha->iodesc_pd = NULL;
	ha->iodesc_pd_dma = 0;
	ha->ct_sns = NULL;
	ha->ms_iocb = NULL;

	ha->init_cb = NULL;
	ha->request_ring = NULL;
	ha->request_dma = 0;
	ha->response_ring = NULL;
	ha->response_dma = 0;

	list_for_each_safe(fcpl, fcptemp, &ha->fcports) {
		fcport = list_entry(fcpl, fc_port_t, list);

		/* fc luns */
		list_for_each_safe(fcll, fcltemp, &fcport->fcluns) {
			fclun = list_entry(fcll, fc_lun_t, list);

			list_del_init(&fclun->list);
			kfree(fclun);
		}

		/* fc ports */
		list_del_init(&fcport->list);
		kfree(fcport);
	}
	INIT_LIST_HEAD(&ha->fcports);

	if (ha->fw_dump) {
		free_pages((unsigned long)ha->fw_dump, ha->fw_dump_order);
		ha->fw_dump = NULL;
	}

	if (ha->fw_dump_buffer) {
		vfree(ha->fw_dump_buffer);
		ha->fw_dump_reading = 0;
		ha->fw_dump_buffer = NULL;
	}
}

/*
 * qla2x00_allocate_sp_pool
 * 	 This routine is called during initialization to allocate
 *  	 memory for local srb_t.
 *
 * Input:
 *	 ha   = adapter block pointer.
 *
 * Context:
 *      Kernel context.
 * 
 * Note: Sets the ref_count for non Null sp to one.
 */
int
qla2x00_allocate_sp_pool(scsi_qla_host_t *ha) 
{
	int      rval;

	rval = QLA_SUCCESS;
	ha->srb_mempool = mempool_create(SRB_MIN_REQ, mempool_alloc_slab,
	    mempool_free_slab, srb_cachep);
	if (ha->srb_mempool == NULL) {
		qla_printk(KERN_INFO, ha, "Unable to allocate SRB mempool.\n");
		rval = QLA_FUNCTION_FAILED;
	}
	return (rval);
}

/*
 *  This routine frees all adapter allocated memory.
 *  
 */
void
qla2x00_free_sp_pool( scsi_qla_host_t *ha) 
{
	if (ha->srb_mempool) {
		mempool_destroy(ha->srb_mempool);
		ha->srb_mempool = NULL;
	}
}

/**************************************************************************
* qla2x00_do_dpc
*   This kernel thread is a task that is schedule by the interrupt handler
*   to perform the background processing for interrupts.
*
* Notes:
* This task always run in the context of a kernel thread.  It
* is kick-off by the driver's detect code and starts up
* up one per adapter. It immediately goes to sleep and waits for
* some fibre event.  When either the interrupt handler or
* the timer routine detects a event it will one of the task
* bits then wake us up.
**************************************************************************/
static int
qla2x00_do_dpc(void *data)
{
	DECLARE_MUTEX_LOCKED(sem);
	scsi_qla_host_t *ha;
	fc_port_t	*fcport;
	os_lun_t        *q;
	srb_t           *sp;
	uint8_t		status;
	unsigned long	flags = 0;
	struct list_head *list, *templist;
	int	dead_cnt, online_cnt;
	int	retry_cmds = 0;
	uint16_t	next_loopid;
	int t;
	os_tgt_t *tq;

	ha = (scsi_qla_host_t *)data;

	lock_kernel();

	daemonize("%s_dpc", ha->host_str);
	allow_signal(SIGHUP);

	ha->dpc_wait = &sem;

	set_user_nice(current, -20);

	unlock_kernel();

	complete(&ha->dpc_inited);

	while (1) {
		DEBUG3(printk("qla2x00: DPC handler sleeping\n"));

		if (down_interruptible(&sem))
			break;

		if (ha->dpc_should_die)
			break;

		if (!list_empty(&ha->done_queue))
			qla2x00_done(ha);

		DEBUG3(printk("qla2x00: DPC handler waking up\n"));

		/* Initialization not yet finished. Don't do anything yet. */
		if (!ha->flags.init_done || ha->dpc_active)
			continue;

		DEBUG3(printk("scsi(%ld): DPC handler\n", ha->host_no));

		ha->dpc_active = 1;

		/* Process commands in retry queue */
		if (test_and_clear_bit(PORT_RESTART_NEEDED, &ha->dpc_flags)) {
			DEBUG(printk("scsi(%ld): DPC checking retry_q. "
			    "total=%d\n",
			    ha->host_no, ha->retry_q_cnt));

			spin_lock_irqsave(&ha->list_lock, flags);
			dead_cnt = online_cnt = 0;
			list_for_each_safe(list, templist, &ha->retry_queue) {
				sp = list_entry(list, srb_t, list);
				q = sp->lun_queue;
				DEBUG3(printk("scsi(%ld): pid=%ld sp=%p, "
				    "spflags=0x%x, q_flag= 0x%lx\n",
				    ha->host_no, sp->cmd->serial_number, sp,
				    sp->flags, q->q_flag));

				if (q == NULL)
					continue;
				fcport = q->fclun->fcport;

				if (atomic_read(&fcport->state) ==
				    FCS_DEVICE_DEAD ||
				    atomic_read(&fcport->ha->loop_state) == LOOP_DEAD) {

					__del_from_retry_queue(ha, sp);
					sp->cmd->result = DID_NO_CONNECT << 16;
					if (atomic_read(&fcport->ha->loop_state) ==
					    LOOP_DOWN) 
						sp->err_id = SRB_ERR_LOOP;
					else
						sp->err_id = SRB_ERR_PORT;
					sp->cmd->host_scribble =
					    (unsigned char *) NULL;
					__add_to_done_queue(ha, sp);
					dead_cnt++;
				} else if (atomic_read(&fcport->state) !=
				    FCS_DEVICE_LOST) {

					__del_from_retry_queue(ha, sp);
					sp->cmd->result = DID_BUS_BUSY << 16;
					sp->cmd->host_scribble =
					    (unsigned char *) NULL;
					__add_to_done_queue(ha, sp);
					online_cnt++;
				}
			} /* list_for_each_safe() */
			spin_unlock_irqrestore(&ha->list_lock, flags);

			DEBUG(printk("scsi(%ld): done processing retry queue "
			    "- dead=%d, online=%d\n ",
			    ha->host_no, dead_cnt, online_cnt));
		}

		/* Process commands in scsi retry queue */
		if (test_and_clear_bit(SCSI_RESTART_NEEDED, &ha->dpc_flags)) {
			/*
			 * Any requests we want to delay for some period is put
			 * in the scsi retry queue with a delay added. The
			 * timer will schedule a "scsi_restart_needed" every 
			 * second as long as there are requests in the scsi
			 * queue. 
			 */
			DEBUG(printk("scsi(%ld): DPC checking scsi "
			    "retry_q.total=%d\n",
			    ha->host_no, ha->scsi_retry_q_cnt));

			online_cnt = 0;
			spin_lock_irqsave(&ha->list_lock, flags);
			list_for_each_safe(list, templist,
			    &ha->scsi_retry_queue) {

				sp = list_entry(list, srb_t, list);
				q = sp->lun_queue;
				tq = sp->tgt_queue;

				DEBUG3(printk("scsi(%ld): scsi_retry_q: "
				    "pid=%ld sp=%p, spflags=0x%x, "
				    "q_flag= 0x%lx,q_state=%d\n",
				    ha->host_no, sp->cmd->serial_number,
				    sp, sp->flags, q->q_flag, q->q_state));

				/* Was this lun suspended */
				if (q->q_state != LUN_STATE_WAIT) {
					online_cnt++;
					__del_from_scsi_retry_queue(ha, sp);

					if (test_bit(TQF_RETRY_CMDS,
					    &tq->flags)) {
						qla2x00_extend_timeout(sp->cmd,
						    (sp->cmd->timeout_per_command / HZ) - QLA_CMD_TIMER_DELTA);
						__add_to_pending_queue(ha, sp);
						retry_cmds++;
					} else
						__add_to_retry_queue(ha, sp);
				}

				/* Was this command suspended for N secs */
				if (sp->delay != 0) {
					sp->delay--;
					if (sp->delay == 0) {
						online_cnt++;
						__del_from_scsi_retry_queue(
						    ha, sp);
						__add_to_retry_queue(ha,sp);
					}
				}
			}
			spin_unlock_irqrestore(&ha->list_lock, flags);

			/* Clear all Target Unsuspended bits */
			for (t = 0; t < ha->max_targets; t++) {
				if ((tq = ha->otgt[t]) == NULL)
					continue;

				if (test_bit(TQF_RETRY_CMDS, &tq->flags))
					clear_bit(TQF_RETRY_CMDS, &tq->flags);
			}
			if (retry_cmds)
				qla2x00_next(ha);

			DEBUG(if (online_cnt > 0))
			DEBUG(printk("scsi(%ld): dpc() found scsi reqs to "
			    "restart= %d\n",
			    ha->host_no, online_cnt));
		}

		if (ha->flags.mbox_busy) {
			if (!list_empty(&ha->done_queue))
				qla2x00_done(ha);

			ha->dpc_active = 0;
			continue;
		}

		if (test_and_clear_bit(ISP_ABORT_NEEDED, &ha->dpc_flags)) {

			DEBUG(printk("scsi(%ld): dpc: sched "
			    "qla2x00_abort_isp ha = %p\n",
			    ha->host_no, ha));
			if (!(test_and_set_bit(ABORT_ISP_ACTIVE,
			    &ha->dpc_flags))) {

				if (qla2x00_abort_isp(ha)) {
					/* failed. retry later */
					set_bit(ISP_ABORT_NEEDED,
					    &ha->dpc_flags);
				}
				clear_bit(ABORT_ISP_ACTIVE, &ha->dpc_flags);
			}
			DEBUG(printk("scsi(%ld): dpc: qla2x00_abort_isp end\n",
			    ha->host_no));
		}

		if (test_and_clear_bit(RESET_MARKER_NEEDED, &ha->dpc_flags) &&
		    (!(test_and_set_bit(RESET_ACTIVE, &ha->dpc_flags)))) {

			DEBUG(printk("scsi(%ld): qla2x00_reset_marker()\n",
			    ha->host_no));

			qla2x00_rst_aen(ha);
			clear_bit(RESET_ACTIVE, &ha->dpc_flags);
		}

		/* Retry each device up to login retry count */
		if ((test_and_clear_bit(RELOGIN_NEEDED, &ha->dpc_flags)) &&
		    !test_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags) &&
		    atomic_read(&ha->loop_state) != LOOP_DOWN) {

			DEBUG(printk("scsi(%ld): qla2x00_port_login()\n",
			    ha->host_no));

			next_loopid = 0;
			list_for_each_entry(fcport, &ha->fcports, list) {
				if (fcport->port_type != FCT_TARGET)
					continue;

				/*
				 * If the port is not ONLINE then try to login
				 * to it if we haven't run out of retries.
				 */
				if (atomic_read(&fcport->state) != FCS_ONLINE &&
				    fcport->login_retry) {

					fcport->login_retry--;
					if (fcport->flags & FCF_FABRIC_DEVICE) {
						if (fcport->flags &
						    FCF_TAPE_PRESENT)
							qla2x00_fabric_logout(
							    ha,
							    fcport->loop_id);
						status = qla2x00_fabric_login(
						    ha, fcport, &next_loopid);
					} else
						status =
						    qla2x00_local_device_login(
							ha, fcport->loop_id);

					if (status == QLA_SUCCESS) {
						fcport->old_loop_id = fcport->loop_id;

						DEBUG(printk("scsi(%ld): port login OK: logged in ID 0x%x\n",
						    ha->host_no, fcport->loop_id));
						
						fcport->port_login_retry_count =
						    ha->port_down_retry_count * PORT_RETRY_TIME;
						atomic_set(&fcport->state, FCS_ONLINE);
						atomic_set(&fcport->port_down_timer,
						    ha->port_down_retry_count * PORT_RETRY_TIME);

						fcport->login_retry = 0;
					} else if (status == 1) {
						set_bit(RELOGIN_NEEDED, &ha->dpc_flags);
						/* retry the login again */
						DEBUG(printk("scsi(%ld): Retrying %d login again loop_id 0x%x\n",
						    ha->host_no,
						    fcport->login_retry, fcport->loop_id));
					} else {
						fcport->login_retry = 0;
					}
				}
				if (test_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags))
					break;
			}
			DEBUG(printk("scsi(%ld): qla2x00_port_login - end\n",
			    ha->host_no));
		}

		if ((test_bit(LOGIN_RETRY_NEEDED, &ha->dpc_flags)) &&
		    atomic_read(&ha->loop_state) != LOOP_DOWN) {

			clear_bit(LOGIN_RETRY_NEEDED, &ha->dpc_flags);
			DEBUG(printk("scsi(%ld): qla2x00_login_retry()\n",
			    ha->host_no));
				
			set_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags);

			DEBUG(printk("scsi(%ld): qla2x00_login_retry - end\n",
			    ha->host_no));
		}

		if (test_and_clear_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags)) {

			DEBUG(printk("scsi(%ld): qla2x00_loop_resync()\n",
			    ha->host_no));

			if (!(test_and_set_bit(LOOP_RESYNC_ACTIVE,
			    &ha->dpc_flags))) {

				qla2x00_loop_resync(ha);

				clear_bit(LOOP_RESYNC_ACTIVE, &ha->dpc_flags);
			}

			DEBUG(printk("scsi(%ld): qla2x00_loop_resync - end\n",
			    ha->host_no));
		}


		if (test_bit(RESTART_QUEUES_NEEDED, &ha->dpc_flags)) {
			DEBUG(printk("scsi(%ld): qla2x00_restart_queues()\n",
			    ha->host_no));

			qla2x00_restart_queues(ha, 0);

			DEBUG(printk("scsi(%ld): qla2x00_restart_queues - end\n",
			    ha->host_no));
		}

		if (test_bit(ABORT_QUEUES_NEEDED, &ha->dpc_flags)) {

			DEBUG(printk("scsi(%ld): qla2x00_abort_queues()\n",
			    ha->host_no));
				
			qla2x00_abort_queues(ha, 0);

			DEBUG(printk("scsi(%ld): qla2x00_abort_queues - end\n",
			    ha->host_no));
		}

		if (test_and_clear_bit(FCPORT_RESCAN_NEEDED, &ha->dpc_flags)) {

			DEBUG(printk("scsi(%ld): Rescan flagged fcports...\n",
			    ha->host_no));

			qla2x00_rescan_fcports(ha);

			DEBUG(printk("scsi(%ld): Rescan flagged fcports..."
			    "end.\n",
			    ha->host_no));
		}


		if (!ha->interrupts_on)
			qla2x00_enable_intrs(ha);

		if (!list_empty(&ha->done_queue))
			qla2x00_done(ha);

		ha->dpc_active = 0;
	} /* End of while(1) */

	DEBUG(printk("scsi(%ld): DPC handler exiting\n", ha->host_no));

	/*
	 * Make sure that nobody tries to wake us up again.
	 */
	ha->dpc_wait = NULL;
	ha->dpc_active = 0;

	complete_and_exit(&ha->dpc_exited, 0);
}

/*
 *  qla2x00_abort_queues
 *	Abort all commands on queues on device
 *
 * Input:
 *	ha = adapter block pointer.
 *
 * Context:
 *	Interrupt context.
 */
void
qla2x00_abort_queues(scsi_qla_host_t *ha, uint8_t doneqflg) 
{

	srb_t       *sp;
	struct list_head *list, *temp;
	unsigned long flags;

	clear_bit(ABORT_QUEUES_NEEDED, &ha->dpc_flags);

	/* Return all commands device queues. */
	spin_lock_irqsave(&ha->list_lock,flags);
	list_for_each_safe(list, temp, &ha->pending_queue) {
		sp = list_entry(list, srb_t, list);

		if (sp->flags & SRB_ABORTED)
			continue;

		/* Remove srb from LUN queue. */
		__del_from_pending_queue(ha, sp);

		/* Set ending status. */
		sp->cmd->result = DID_BUS_BUSY << 16;

		__add_to_done_queue(ha, sp);
	}
	spin_unlock_irqrestore(&ha->list_lock, flags);
}

/*
*  qla2x00_rst_aen
*      Processes asynchronous reset.
*
* Input:
*      ha  = adapter block pointer.
*/
static void
qla2x00_rst_aen(scsi_qla_host_t *ha) 
{
	if (ha->flags.online && !ha->flags.reset_active &&
	    !atomic_read(&ha->loop_down_timer) &&
	    !(test_bit(ABORT_ISP_ACTIVE, &ha->dpc_flags))) {
		do {
			clear_bit(RESET_MARKER_NEEDED, &ha->dpc_flags);

			/*
			 * Issue marker command only when we are going to start
			 * the I/O.
			 */
			ha->marker_needed = 1;
		} while (!atomic_read(&ha->loop_down_timer) &&
		    (test_bit(RESET_MARKER_NEEDED, &ha->dpc_flags)));
	}
}


/*
 * This routine will allocate SP from the free queue
 * input:
 *        scsi_qla_host_t *
 * output:
 *        srb_t * or NULL
 */
srb_t *
qla2x00_get_new_sp(scsi_qla_host_t *ha)
{
	srb_t *sp;

	sp = mempool_alloc(ha->srb_mempool, GFP_KERNEL);
	if (sp)
		atomic_set(&sp->ref_count, 1);
	return (sp);
}

/**************************************************************************
*   qla2x00_timer
*
* Description:
*   One second timer
*
* Context: Interrupt
***************************************************************************/
static void
qla2x00_timer(scsi_qla_host_t *ha)
{
	int		t,l;
	unsigned long	cpu_flags = 0;
	fc_port_t	*fcport;
	os_lun_t *lq;
	os_tgt_t *tq;
	int		start_dpc = 0;
	int		index;
	srb_t		*sp;

	/*
	 * We try and restart any request in the retry queue every second.
	 */
	if (!list_empty(&ha->retry_queue)) {
		set_bit(PORT_RESTART_NEEDED, &ha->dpc_flags);
		start_dpc++;
	}

	/*
	 * We try and restart any request in the scsi_retry queue every second.
	 */
	if (!list_empty(&ha->scsi_retry_queue)) {
		set_bit(SCSI_RESTART_NEEDED, &ha->dpc_flags);
		start_dpc++;
	}

	/*
	 * Ports - Port down timer.
	 *
	 * Whenever, a port is in the LOST state we start decrementing its port
	 * down timer every second until it reaches zero. Once  it reaches zero
	 * the port it marked DEAD. 
	 */
	t = 0;
	list_for_each_entry(fcport, &ha->fcports, list) {
		if (fcport->port_type != FCT_TARGET)
			continue;

		if (atomic_read(&fcport->state) == FCS_DEVICE_LOST) {

			if (atomic_read(&fcport->port_down_timer) == 0)
				continue;

			if (atomic_dec_and_test(&fcport->port_down_timer) != 0) 
				atomic_set(&fcport->state, FCS_DEVICE_DEAD);
			
			DEBUG(printk("scsi(%ld): fcport-%d - port retry count: "
			    "%d remainning\n",
			    ha->host_no,
			    t, atomic_read(&fcport->port_down_timer)));
		}
		t++;
	} /* End of for fcport  */

	/*
	 * LUNS - lun suspend timer.
	 *
	 * Whenever, a lun is suspended the timer starts decrementing its
	 * suspend timer every second until it reaches zero. Once  it reaches
	 * zero the lun retry count is decremented. 
	 */

	/*
	 * FIXME(dg) - Need to convert this linear search of luns into a search
	 * of a list of suspended luns.
	 */
	for (t = 0; t < ha->max_targets; t++) {
		if ((tq = ha->otgt[t]) == NULL)
			continue;

		for (l = 0; l < ha->max_luns; l++) {
			if ((lq = (os_lun_t *) tq->olun[l]) == NULL)
				continue;

			spin_lock_irqsave(&lq->q_lock, cpu_flags);
			if (lq->q_state == LUN_STATE_WAIT &&
				atomic_read(&lq->q_timer) != 0) {

				if (atomic_dec_and_test(&lq->q_timer) != 0) {
					/*
					 * A delay should immediately
					 * transition to a READY state
					 */
					if (test_and_clear_bit(LUN_EXEC_DELAYED,
					    &lq->q_flag)) {
						lq->q_state = LUN_STATE_READY;
					}
					else {
						lq->q_count++;
						if (lq->q_count == lq->q_max)
							lq->q_state =
							    LUN_STATE_TIMEOUT;
						else
							lq->q_state =
							    LUN_STATE_RUN;
					}
				}
				DEBUG3(printk("scsi(%ld): lun%d - timer %d, "
				    "count=%d, max=%d, state=%d\n",
				    ha->host_no,
				    l,
				    atomic_read(&lq->q_timer),
				    lq->q_count, lq->q_max, lq->q_state));
			}
			spin_unlock_irqrestore(&lq->q_lock, cpu_flags);
		} /* End of for luns  */
	} /* End of for targets  */

	/* Loop down handler. */
	if (atomic_read(&ha->loop_down_timer) > 0 &&
	    !(test_bit(ABORT_ISP_ACTIVE, &ha->dpc_flags)) && ha->flags.online) {

		if (atomic_read(&ha->loop_down_timer) ==
		    ha->loop_down_abort_time) {

			DEBUG(printk("scsi(%ld): Loop Down - aborting the "
			    "queues before time expire\n",
			    ha->host_no));

			if (!IS_QLA2100(ha) && ha->link_down_timeout)
				atomic_set(&ha->loop_state, LOOP_DEAD); 

			/* Schedule an ISP abort to return any tape commands. */
			spin_lock_irqsave(&ha->hardware_lock, cpu_flags);
			for (index = 1; index < MAX_OUTSTANDING_COMMANDS;
			    index++) {
				sp = ha->outstanding_cmds[index];
				if (!sp)
					continue;
				if (!(sp->fclun->fcport->flags &
				    FCF_TAPE_PRESENT))
					continue;

				set_bit(ISP_ABORT_NEEDED, &ha->dpc_flags);
				break;
			}
			spin_unlock_irqrestore(&ha->hardware_lock, cpu_flags);

			set_bit(ABORT_QUEUES_NEEDED, &ha->dpc_flags);
			start_dpc++;
		}

		/* if the loop has been down for 4 minutes, reinit adapter */
		if (atomic_dec_and_test(&ha->loop_down_timer) != 0) {
			DEBUG(printk("scsi(%ld): Loop down exceed 4 mins - "
			    "restarting queues.\n",
			    ha->host_no));

			set_bit(RESTART_QUEUES_NEEDED, &ha->dpc_flags);
			start_dpc++;

			if (!(ha->device_flags & DFLG_NO_CABLE)) {
				DEBUG(printk("scsi(%ld): Loop down - "
				    "aborting ISP.\n",
				    ha->host_no));
				qla_printk(KERN_WARNING, ha,
				    "Loop down - aborting ISP.\n");

				set_bit(ISP_ABORT_NEEDED, &ha->dpc_flags);
			}
		}
		DEBUG3(printk("scsi(%ld): Loop Down - seconds remainning %d\n",
		    ha->host_no,
		    atomic_read(&ha->loop_down_timer)));
	}

	/*
	 * Done Q Handler -- dgFIXME This handler will kick off doneq if we
	 * haven't process it in 2 seconds.
	 */
	if (!list_empty(&ha->done_queue))
		qla2x00_done(ha);


	/* Schedule the DPC routine if needed */
	if ((test_bit(ISP_ABORT_NEEDED, &ha->dpc_flags) ||
	    test_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags) ||
	    start_dpc ||
	    test_bit(LOGIN_RETRY_NEEDED, &ha->dpc_flags) ||
	    test_bit(RELOGIN_NEEDED, &ha->dpc_flags)) &&
	    ha->dpc_wait && !ha->dpc_active) {

		up(ha->dpc_wait);
	}

	qla2x00_restart_timer(ha, WATCH_INTERVAL);
}

/*
 * qla2x00_extend_timeout
 *      This routine will extend the timeout to the specified value.
 *
 * Input:
 *      cmd = SCSI command structure
 *
 * Returns:
 *      None.
 */
void 
qla2x00_extend_timeout(struct scsi_cmnd *cmd, int timeout) 
{
	srb_t *sp = (srb_t *) CMD_SP(cmd);
	u_long our_jiffies = (timeout * HZ) + jiffies;

    	sp->ext_history= 0; 
	sp->e_start = jiffies;
	if (cmd->eh_timeout.function) {
		mod_timer(&cmd->eh_timeout,our_jiffies);
		sp->ext_history |= 1;
	}
	if (sp->timer.function != NULL) {
		/* 
		 * Our internal timer should timeout before the midlayer has a
		 * chance begin the abort process
		 */
		mod_timer(&sp->timer,our_jiffies - (QLA_CMD_TIMER_DELTA * HZ));

    	 	sp->ext_history |= 2;
	}
}

/**************************************************************************
*   qla2x00_cmd_timeout
*
* Description:
*       Handles the command if it times out in any state.
*
* Input:
*     sp - pointer to validate
*
* Returns:
* None.
* Note:Need to add the support for if( sp->state == SRB_FAILOVER_STATE).
**************************************************************************/
void
qla2x00_cmd_timeout(srb_t *sp)
{
	int t, l;
	int processed;
	scsi_qla_host_t *vis_ha, *dest_ha;
	struct scsi_cmnd *cmd;
	unsigned long flags, cpu_flags;
	fc_port_t *fcport;

	cmd = sp->cmd;
	vis_ha = (scsi_qla_host_t *)cmd->device->host->hostdata;

	DEBUG3(printk("cmd_timeout: Entering sp->state = %x\n", sp->state));

	t = cmd->device->id;
	l = cmd->device->lun;
	fcport = sp->fclun->fcport;
	dest_ha = sp->ha;

	/*
	 * If IO is found either in retry Queue 
	 *    OR in Lun Queue
	 * Return this IO back to host
	 */
	spin_lock_irqsave(&vis_ha->list_lock, flags);
	processed = 0;
	if (sp->state == SRB_PENDING_STATE) {
		__del_from_pending_queue(vis_ha, sp);
		DEBUG2(printk("scsi(%ld): Found in Pending queue pid %ld, "
		    "State = %x., fcport state=%d jiffies=%lx\n",
		    vis_ha->host_no, cmd->serial_number, sp->state,
		    atomic_read(&fcport->state), jiffies));

		/*
		 * If FC_DEVICE is marked as dead return the cmd with
		 * DID_NO_CONNECT status.  Otherwise set the host_byte to
		 * DID_BUS_BUSY to let the OS  retry this cmd.
		 */
		if (atomic_read(&fcport->state) == FCS_DEVICE_DEAD ||
		    atomic_read(&fcport->ha->loop_state) == LOOP_DEAD) {
			cmd->result = DID_NO_CONNECT << 16;
			if (atomic_read(&fcport->ha->loop_state) == LOOP_DOWN) 
				sp->err_id = SRB_ERR_LOOP;
			else
				sp->err_id = SRB_ERR_PORT;
		} else {
			cmd->result = DID_BUS_BUSY << 16;
		}
		__add_to_done_queue(vis_ha, sp);
		processed++;
	} 
	spin_unlock_irqrestore(&vis_ha->list_lock, flags);

	if (processed) {
		qla2x00_done(vis_ha);
		return;
	}

	spin_lock_irqsave(&dest_ha->list_lock, flags);
	if ((sp->state == SRB_RETRY_STATE) ||
	    (sp->state == SRB_SCSI_RETRY_STATE)) {

		DEBUG2(printk("scsi(%ld): Found in (Scsi) Retry queue or "
		    "failover Q pid %ld, State = %x., fcport state=%d "
		    "jiffies=%lx retried=%d\n",
		    dest_ha->host_no, cmd->serial_number, sp->state,
		    atomic_read(&fcport->state), jiffies, cmd->retries));

		if ((sp->state == SRB_RETRY_STATE)) {
			__del_from_retry_queue(dest_ha, sp);
		} else if ((sp->state == SRB_SCSI_RETRY_STATE)) {
			__del_from_scsi_retry_queue(dest_ha, sp);
		} 

		/*
		 * If FC_DEVICE is marked as dead return the cmd with
		 * DID_NO_CONNECT status.  Otherwise set the host_byte to
		 * DID_BUS_BUSY to let the OS  retry this cmd.
		 */
		if ((atomic_read(&fcport->state) == FCS_DEVICE_DEAD) ||
		    atomic_read(&dest_ha->loop_state) == LOOP_DEAD) {
			qla2x00_extend_timeout(cmd, EXTEND_CMD_TIMEOUT);
			cmd->result = DID_NO_CONNECT << 16;
			if (atomic_read(&dest_ha->loop_state) == LOOP_DOWN) 
				sp->err_id = SRB_ERR_LOOP;
			else
				sp->err_id = SRB_ERR_PORT;
		} else {
			cmd->result = DID_BUS_BUSY << 16;
		}

		__add_to_done_queue(dest_ha, sp);
		processed++;
	} 
	spin_unlock_irqrestore(&dest_ha->list_lock, flags);

	if (processed) {
		qla2x00_done(dest_ha);

		 return;
	}

	spin_lock_irqsave(&dest_ha->list_lock, cpu_flags);
	if (sp->state == SRB_DONE_STATE) {
		/* IO in done_q  -- leave it */
		DEBUG(printk("scsi(%ld): Found in Done queue pid %ld sp=%p.\n",
		    dest_ha->host_no, cmd->serial_number, sp));
	} else if (sp->state == SRB_SUSPENDED_STATE) {
		DEBUG(printk("scsi(%ld): Found SP %p in suspended state  "
		    "- pid %ld:\n",
		    dest_ha->host_no, sp, cmd->serial_number));
		DEBUG(qla2x00_dump_buffer((uint8_t *)sp, sizeof(srb_t));)
	} else if (sp->state == SRB_ACTIVE_STATE) {
		/*
		 * IO is with ISP find the command in our active list.
		 */
		spin_unlock_irqrestore(&dest_ha->list_lock, cpu_flags);
		spin_lock_irqsave(&dest_ha->hardware_lock, flags);
		if (sp == dest_ha->outstanding_cmds[
		    (unsigned long)sp->cmd->host_scribble]) {

			DEBUG(printk("cmd_timeout: Found in ISP \n"));

			if (sp->flags & SRB_TAPE) {
				/*
				 * We cannot allow the midlayer error handler
				 * to wakeup and begin the abort process.
				 * Extend the timer so that the firmware can
				 * properly return the IOCB.
				 */
				DEBUG(printk("cmd_timeout: Extending timeout "
				    "of FCP2 tape command!\n"));
				qla2x00_extend_timeout(sp->cmd,
				    EXTEND_CMD_TIMEOUT);
			}
			sp->state = SRB_ACTIVE_TIMEOUT_STATE;
			spin_unlock_irqrestore(&dest_ha->hardware_lock, flags);
		} else {
			spin_unlock_irqrestore(&dest_ha->hardware_lock, flags);
			printk(KERN_INFO 
				"qla_cmd_timeout: State indicates it is with "
				"ISP, But not in active array\n");
		}
		spin_lock_irqsave(&dest_ha->list_lock, cpu_flags);
	} else if (sp->state == SRB_ACTIVE_TIMEOUT_STATE) {
		DEBUG(printk("qla2100%ld: Found in Active timeout state"
				"pid %ld, State = %x., \n",
				dest_ha->host_no,
				sp->cmd->serial_number, sp->state);)
	} else {
		/* EMPTY */
		DEBUG2(printk("cmd_timeout%ld: LOST command state = "
				"0x%x, sp=%p\n",
				vis_ha->host_no, sp->state,sp);)

		qla_printk(KERN_INFO, vis_ha,
			"cmd_timeout: LOST command state = 0x%x\n", sp->state);
	}
	spin_unlock_irqrestore(&dest_ha->list_lock, cpu_flags);

	DEBUG3(printk("cmd_timeout: Leaving\n");)
}

/**************************************************************************
* qla2x00_done
*      Process completed commands.
*
* Input:
*      old_ha           = adapter block pointer.
*
**************************************************************************/
void
qla2x00_done(scsi_qla_host_t *old_ha)
{
	os_lun_t	*lq;
	struct scsi_cmnd *cmd;
	unsigned long	flags = 0;
	scsi_qla_host_t	*ha;
	scsi_qla_host_t	*vis_ha;
	int	send_marker_once = 0;
	srb_t           *sp, *sptemp;
	LIST_HEAD(local_sp_list);

	/*
	 * Get into local queue such that we do not wind up calling done queue
	 * tasklet for the same IOs from DPC or any other place.
	 */
	spin_lock_irqsave(&old_ha->list_lock, flags);
	list_splice_init(&old_ha->done_queue, &local_sp_list);
	spin_unlock_irqrestore(&old_ha->list_lock, flags);

	/*
	 * All done commands are in the local queue, now do the call back.
	 */
	list_for_each_entry_safe(sp, sptemp, &local_sp_list, list) {
		old_ha->done_q_cnt--;
        	sp->state = SRB_NO_QUEUE_STATE;

		/* remove command from local list */
		list_del_init(&sp->list);

		cmd = sp->cmd;
		if (cmd == NULL)
		 	continue;

		vis_ha = (scsi_qla_host_t *)cmd->device->host->hostdata;
		lq = sp->lun_queue;
		ha = sp->ha;

		if (sp->flags & SRB_DMA_VALID) {
			sp->flags &= ~SRB_DMA_VALID;

			/* Release memory used for this I/O */
			if (cmd->use_sg) {
				pci_unmap_sg(ha->pdev, cmd->request_buffer,
				    cmd->use_sg, cmd->sc_data_direction);
			} else if (cmd->request_bufflen) {
				pci_unmap_page(ha->pdev, sp->dma_handle,
				    cmd->request_bufflen,
				    cmd->sc_data_direction);
			}
		}


		switch (host_byte(cmd->result)) {
			case DID_OK:
			case DID_ERROR:
				break;

			case DID_RESET:
				/*
				 * Set marker needed, so we don't have to
				 * send multiple markers
				 */
				if (!send_marker_once) {
					ha->marker_needed = 1;
					send_marker_once++;
				}

				/*
				 * WORKAROUND
				 *
				 * A backdoor device-reset requires different
				 * error handling.  This code differentiates
				 * between normal error handling and the
				 * backdoor method.
				 *
				 */
				if (ha->host->eh_active != EH_ACTIVE)
					cmd->result = DID_BUS_BUSY << 16;
				break;


			case DID_ABORT:
				sp->flags &= ~SRB_ABORT_PENDING;
				sp->flags |= SRB_ABORTED;

				if (sp->flags & SRB_TIMEOUT)
					cmd->result = DID_TIME_OUT << 16;

				break;

			default:
				DEBUG2(printk("scsi(%ld:%d:%d) %s: did_error "
				    "= %d, comp-scsi= 0x%x-0x%x.\n",
				    vis_ha->host_no,
				    cmd->device->id, cmd->device->lun,
				    __func__,
				    host_byte(cmd->result),
				    CMD_COMPL_STATUS(cmd),
				    CMD_SCSI_STATUS(cmd)));
				break;
		}

		/*
		 * Call the mid-level driver interrupt handler -- via sp_put()
		 */
		sp_put(ha, sp);
	} /* end of while */
}

/*
 * qla2x00_process_response_queue_in_zio_mode
 *	Process response queue completion as fast as possible
 *	to achieve Zero Interrupt Opertions-ZIO
 *
 * Input:
 *	ha = adapter block pointer.
 *
 * Context:
 *	Kernel context.
 */
static inline void
qla2x00_process_response_queue_in_zio_mode(scsi_qla_host_t *ha)
{
	unsigned long flags;

	/* Check for unprocessed commands in response queue. */
	if (!ha->flags.process_response_queue)
		return;
	if (!ha->flags.online)
		return;
	if (ha->response_ring_ptr->signature == RESPONSE_PROCESSED)
		return;
	
	spin_lock_irqsave(&ha->hardware_lock,flags);
	qla2x00_process_response_queue(ha);
	spin_unlock_irqrestore(&ha->hardware_lock, flags);
}

/*
 * qla2x00_next
 *	Retrieve and process next job in the LUN queue.
 *
 * Input:
 *	tq = SCSI target queue pointer.
 *	lq = SCSI LUN queue pointer.
 *	TGT_LOCK must be already obtained.
 *
 * Output:
 *	Releases TGT_LOCK upon exit.
 *
 * Context:
 *	Kernel/Interrupt context.
 * 
 * Note: This routine will always try to start I/O from visible HBA.
 */
void
qla2x00_next(scsi_qla_host_t *vis_ha) 
{
	int		rval;
	unsigned long   flags;
	scsi_qla_host_t *dest_ha;
	fc_port_t	*fcport;
	srb_t           *sp, *sptemp;
	LIST_HEAD(local_sp_list);

	dest_ha = NULL;

	spin_lock_irqsave(&vis_ha->list_lock, flags);
	list_splice_init(&vis_ha->pending_queue, &local_sp_list);
	vis_ha->qthreads = 0;
	spin_unlock_irqrestore(&vis_ha->list_lock, flags);

	list_for_each_entry_safe(sp, sptemp, &local_sp_list, list) {
		list_del_init(&sp->list);
        	sp->state = SRB_NO_QUEUE_STATE;

		fcport = sp->fclun->fcport;
		dest_ha = fcport->ha;

		/* If device is dead then send request back to OS */
		if (atomic_read(&fcport->state) == FCS_DEVICE_DEAD) {
			sp->cmd->result = DID_NO_CONNECT << 16;
			if (atomic_read(&dest_ha->loop_state) == LOOP_DOWN) 
				sp->err_id = SRB_ERR_LOOP;
			else
				sp->err_id = SRB_ERR_PORT;

			DEBUG3(printk("scsi(%ld): loop/port is down - pid=%ld, "
			    "sp=%p err_id=%d loopid=0x%x queued to dest HBA "
			    "scsi%ld.\n", dest_ha->host_no,
			    sp->cmd->serial_number, sp, sp->err_id,
			    fcport->loop_id, dest_ha->host_no));
			/* 
			 * Initiate a failover - done routine will initiate.
			 */
			add_to_done_queue(vis_ha, sp);

			continue;
		}

		/*
		 * SCSI Kluge: Whenever, we need to wait for an event such as
		 * loop down (i.e. loop_down_timer ) or port down (i.e.  LUN
		 * request qeueue is suspended) then we will recycle new
		 * commands back to the SCSI layer.  We do this because this is
		 * normally a temporary condition and we don't want the
		 * mid-level scsi.c driver to get upset and start aborting
		 * commands.  The timeout value is extracted from the command
		 * minus 1-second and put on a retry queue (watchdog). Once the
		 * command timeout it is returned to the mid-level with a BUSY
		 * status, so the mid-level will retry it. This process
		 * continues until the LOOP DOWN time expires or the condition
		 * goes away.
		 */
		if (!(sp->flags & (SRB_IOCTL | SRB_TAPE)) &&
		    (atomic_read(&fcport->state) != FCS_ONLINE ||
			test_bit(ABORT_ISP_ACTIVE, &dest_ha->dpc_flags) ||
			atomic_read(&dest_ha->loop_state) != LOOP_READY)) {

			DEBUG3(printk("scsi(%ld): port=(0x%x) retry_q(%d) "
			    "loop state = %d, loop counter = 0x%x dpc flags "
			    "= 0x%lx\n",
			    dest_ha->host_no,
			    fcport->loop_id,
			    atomic_read(&fcport->state),
			    atomic_read(&dest_ha->loop_state),
			    atomic_read(&dest_ha->loop_down_timer),
			    dest_ha->dpc_flags));

			qla2x00_extend_timeout(sp->cmd, EXTEND_CMD_TIMEOUT);
			add_to_retry_queue(vis_ha, sp);

			continue;
		} 

		/*
		 * If this request's lun is suspended then put the request on
		 * the  scsi_retry queue. 
		 */
	 	if (!(sp->flags & (SRB_IOCTL | SRB_TAPE)) &&
		    sp->lun_queue->q_state == LUN_STATE_WAIT) {
			DEBUG3(printk("scsi(%ld): lun wait state - pid=%ld, "
			    "opcode=%d, allowed=%d, retries=%d\n",
			    dest_ha->host_no,
			    sp->cmd->serial_number,
			    sp->cmd->cmnd[0],
			    sp->cmd->allowed,
			    sp->cmd->retries));
				
			add_to_scsi_retry_queue(vis_ha, sp);

			continue;
		}

		sp->lun_queue->io_cnt++;

		rval = qla2x00_start_scsi(sp);
		if (rval != QLA_SUCCESS) {
			/* Place request back on top of device queue */
			/* add to the top of queue */
			add_to_pending_queue_head(vis_ha, sp);

			sp->lun_queue->io_cnt--;
		}
	}

	if (!IS_QLA2100(vis_ha) && !IS_QLA2200(vis_ha)) {
		/* Process response_queue if ZIO support is enabled*/ 
		qla2x00_process_response_queue_in_zio_mode(vis_ha);

	}
}


/**************************************************************************
*   qla2x00_check_tgt_status
*
* Description:
*     Checks to see if the target or loop is down.
*
* Input:
*     cmd - pointer to Scsi cmd structure
*
* Returns:
*   1 - if target is present
*   0 - if target is not present
*
**************************************************************************/
int
qla2x00_check_tgt_status(scsi_qla_host_t *ha, struct scsi_cmnd *cmd)
{
	os_lun_t        *lq;
	unsigned int	b, t, l;
	fc_port_t	*fcport;

	/* Generate LU queue on bus, target, LUN */
	b = cmd->device->channel;
	t = cmd->device->id;
	l = cmd->device->lun;

	if ((lq = GET_LU_Q(ha,t,l)) == NULL) {
		return (QLA_FUNCTION_FAILED);
	}

	fcport = lq->fclun->fcport;

	if (TGT_Q(ha, t) == NULL ||
	    l >= ha->max_luns ||
	    atomic_read(&fcport->state) == FCS_DEVICE_DEAD ||
	    atomic_read(&ha->loop_state) == LOOP_DEAD ||
	    (!atomic_read(&ha->loop_down_timer) &&
		atomic_read(&ha->loop_state) == LOOP_DOWN) ||
	    test_bit(ABORT_ISP_ACTIVE, &ha->dpc_flags) ||
	    test_bit(ISP_ABORT_NEEDED, &ha->dpc_flags) ||
	    atomic_read(&ha->loop_state) != LOOP_READY) {

		DEBUG(printk(KERN_INFO
		    "scsi(%ld:%2d:%2d:%2d): %s connection is down\n",
		    ha->host_no,
		    b, t, l,
		    __func__));

		cmd->result = DID_NO_CONNECT << 16;
		return (QLA_FUNCTION_FAILED);
	}
	return (QLA_SUCCESS);
}

/**************************************************************************
*   qla2x00_check_port_status
*
* Description:
*     Checks to see if the port or loop is down.
*
* Input:
*     fcport - pointer to fc_port_t structure.
*
* Returns:
*   1 - if port is present
*   0 - if port is not present
*
**************************************************************************/
int
qla2x00_check_port_status(scsi_qla_host_t *ha, fc_port_t *fcport)
{
	if (fcport == NULL) {
		return (QLA_FUNCTION_FAILED);
	}

	if (atomic_read(&fcport->state) == FCS_DEVICE_DEAD ||
	    atomic_read(&ha->loop_state) == LOOP_DEAD) {
		return (QLA_FUNCTION_FAILED);
	}

	if ((atomic_read(&fcport->state) != FCS_ONLINE) || 
	    (!atomic_read(&ha->loop_down_timer) &&
		atomic_read(&ha->loop_state) == LOOP_DOWN) ||
	    (test_bit(ABORT_ISP_ACTIVE, &ha->dpc_flags)) ||
	    test_bit(CFG_ACTIVE, &ha->cfg_flags) ||
	    test_bit(ISP_ABORT_NEEDED, &ha->dpc_flags) ||
	    atomic_read(&ha->loop_state) != LOOP_READY) {

		DEBUG(printk(KERN_INFO
		    "scsi(%ld): Connection is down. fcport=%p.\n",
		    ha->host_no, fcport));

		return (QLA_BUSY);
	}

	return (QLA_SUCCESS);
}

/* XXX(hch): crude hack to emulate a down_timeout() */
int
qla2x00_down_timeout(struct semaphore *sema, unsigned long timeout)
{
	const unsigned int step = HZ/10;

	do {
		if (!down_trylock(sema))
			return 0;
		set_current_state(TASK_INTERRUPTIBLE);
		if (schedule_timeout(step))
			break;
	} while ((timeout -= step) > 0);

	return -ETIMEDOUT;
}

static void
qla2xxx_get_port_id(struct scsi_device *sdev)
{
	scsi_qla_host_t *ha = to_qla_host(sdev->host);
	struct fc_port *fc;

	list_for_each_entry(fc, &ha->fcports, list) {
		if (fc->os_target_id == sdev->id) {
			fc_port_id(sdev) = fc->d_id.b.domain << 16 |
				fc->d_id.b.area << 8 | 
				fc->d_id.b.al_pa;
			return;
		}
	}
	fc_port_id(sdev) = -1;
}

static void
qla2xxx_get_port_name(struct scsi_device *sdev)
{
	scsi_qla_host_t *ha = to_qla_host(sdev->host);
	struct fc_port *fc;

	list_for_each_entry(fc, &ha->fcports, list) {
		if (fc->os_target_id == sdev->id) {
			fc_port_name(sdev) =
				__be64_to_cpu(*(uint64_t *)fc->port_name);
			return;
		}
	}
	fc_port_name(sdev) = -1;
}

static void
qla2xxx_get_node_name(struct scsi_device *sdev)
{
	scsi_qla_host_t *ha = to_qla_host(sdev->host);
	struct fc_port *fc;

	list_for_each_entry(fc, &ha->fcports, list) {
		if (fc->os_target_id == sdev->id) {
			fc_node_name(sdev) =
				__be64_to_cpu(*(uint64_t *)fc->node_name);
			return;
		}
	}
	fc_node_name(sdev) = -1;
}

static struct fc_function_template qla2xxx_transport_functions = {
	.get_port_id = qla2xxx_get_port_id,
	.show_port_id = 1,
	.get_port_name = qla2xxx_get_port_name,
	.show_port_name = 1,
	.get_node_name = qla2xxx_get_node_name,
	.show_node_name = 1,
};

/**
 * qla2x00_module_init - Module initialization.
 **/
static int __init
qla2x00_module_init(void)
{
	/* Allocate cache for SRBs. */
	sprintf(srb_cachep_name, "qla2xxx_srbs");
	srb_cachep = kmem_cache_create(srb_cachep_name, sizeof(srb_t), 0,
	    SLAB_HWCACHE_ALIGN, NULL, NULL);
	if (srb_cachep == NULL) {
		printk(KERN_ERR
		    "qla2xxx: Unable to allocate SRB cache...Failing load!\n");
		return -ENOMEM;
	}

	/* Derive version string. */
	strcpy(qla2x00_version_str, QLA2XXX_VERSION);
#if DEBUG_QLA2100
	strcat(qla2x00_version_str, "-debug");
#endif

	qla2xxx_transport_template = fc_attach_transport(&qla2xxx_transport_functions);
	if (!qla2xxx_transport_template)
		return -ENODEV;

	printk(KERN_INFO
	    "QLogic Fibre Channel HBA Driver (%p)\n", qla2x00_set_info);

	return 0;
}

/**
 * qla2x00_module_exit - Module cleanup.
 **/
static void __exit
qla2x00_module_exit(void)
{
	/* Free SRBs cache. */
	if (srb_cachep != NULL) {
		if (kmem_cache_destroy(srb_cachep) != 0) {
			printk(KERN_ERR
			    "qla2xxx: Unable to free SRB cache...Memory pools "
			    "still active?\n");
		}
		srb_cachep = NULL;
	}

	fc_release_transport(qla2xxx_transport_template);
}

module_init(qla2x00_module_init);
module_exit(qla2x00_module_exit);

MODULE_AUTHOR("QLogic Corporation");
MODULE_DESCRIPTION("QLogic Fibre Channel HBA Driver");
MODULE_LICENSE("GPL");
