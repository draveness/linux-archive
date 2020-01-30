/*
 * pata_cmd64x.c 	- CMD64x PATA for new ATA layer
 *			  (C) 2005 Red Hat Inc
 *			  Alan Cox <alan@redhat.com>
 *
 * Based upon
 * linux/drivers/ide/pci/cmd64x.c		Version 1.30	Sept 10, 2002
 *
 * cmd64x.c: Enable interrupts at initialization time on Ultra/PCI machines.
 *           Note, this driver is not used at all on other systems because
 *           there the "BIOS" has done all of the following already.
 *           Due to massive hardware bugs, UltraDMA is only supported
 *           on the 646U2 and not on the 646U.
 *
 * Copyright (C) 1998		Eddie C. Dost  (ecd@skynet.be)
 * Copyright (C) 1998		David S. Miller (davem@redhat.com)
 *
 * Copyright (C) 1999-2002	Andre Hedrick <andre@linux-ide.org>
 *
 * TODO
 *	Testing work
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <scsi/scsi_host.h>
#include <linux/libata.h>

#define DRV_NAME "pata_cmd64x"
#define DRV_VERSION "0.2.4"

/*
 * CMD64x specific registers definition.
 */

enum {
	CFR 		= 0x50,
		CFR_INTR_CH0  = 0x02,
	CNTRL 		= 0x51,
		CNTRL_DIS_RA0 = 0x40,
		CNTRL_DIS_RA1 = 0x80,
		CNTRL_ENA_2ND = 0x08,
	CMDTIM 		= 0x52,
	ARTTIM0 	= 0x53,
	DRWTIM0 	= 0x54,
	ARTTIM1 	= 0x55,
	DRWTIM1 	= 0x56,
	ARTTIM23 	= 0x57,
		ARTTIM23_DIS_RA2  = 0x04,
		ARTTIM23_DIS_RA3  = 0x08,
		ARTTIM23_INTR_CH1 = 0x10,
	ARTTIM2 	= 0x57,
	ARTTIM3 	= 0x57,
	DRWTIM23	= 0x58,
	DRWTIM2 	= 0x58,
	BRST 		= 0x59,
	DRWTIM3 	= 0x5b,
	BMIDECR0	= 0x70,
	MRDMODE		= 0x71,
		MRDMODE_INTR_CH0 = 0x04,
		MRDMODE_INTR_CH1 = 0x08,
		MRDMODE_BLK_CH0  = 0x10,
		MRDMODE_BLK_CH1	 = 0x20,
	BMIDESR0	= 0x72,
	UDIDETCR0	= 0x73,
	DTPR0		= 0x74,
	BMIDECR1	= 0x78,
	BMIDECSR	= 0x79,
	BMIDESR1	= 0x7A,
	UDIDETCR1	= 0x7B,
	DTPR1		= 0x7C
};

static int cmd648_cable_detect(struct ata_port *ap)
{
	struct pci_dev *pdev = to_pci_dev(ap->host->dev);
	u8 r;

	/* Check cable detect bits */
	pci_read_config_byte(pdev, BMIDECSR, &r);
	if (r & (1 << ap->port_no))
		return ATA_CBL_PATA80;
	return ATA_CBL_PATA40;
}

/**
 *	cmd64x_set_piomode	-	set initial PIO mode data
 *	@ap: ATA interface
 *	@adev: ATA device
 *
 *	Called to do the PIO mode setup.
 */

static void cmd64x_set_piomode(struct ata_port *ap, struct ata_device *adev)
{
	struct pci_dev *pdev = to_pci_dev(ap->host->dev);
	struct ata_timing t;
	const unsigned long T = 1000000 / 33;
	const u8 setup_data[] = { 0x40, 0x40, 0x40, 0x80, 0x00 };

	u8 reg;

	/* Port layout is not logical so use a table */
	const u8 arttim_port[2][2] = {
		{ ARTTIM0, ARTTIM1 },
		{ ARTTIM23, ARTTIM23 }
	};
	const u8 drwtim_port[2][2] = {
		{ DRWTIM0, DRWTIM1 },
		{ DRWTIM2, DRWTIM3 }
	};

	int arttim = arttim_port[ap->port_no][adev->devno];
	int drwtim = drwtim_port[ap->port_no][adev->devno];


	if (ata_timing_compute(adev, adev->pio_mode, &t, T, 0) < 0) {
		printk(KERN_ERR DRV_NAME ": mode computation failed.\n");
		return;
	}
	if (ap->port_no) {
		/* Slave has shared address setup */
		struct ata_device *pair = ata_dev_pair(adev);

		if (pair) {
			struct ata_timing tp;
			ata_timing_compute(pair, pair->pio_mode, &tp, T, 0);
			ata_timing_merge(&t, &tp, &t, ATA_TIMING_SETUP);
		}
	}

	printk(KERN_DEBUG DRV_NAME ": active %d recovery %d setup %d.\n",
		t.active, t.recover, t.setup);
	if (t.recover > 16) {
		t.active += t.recover - 16;
		t.recover = 16;
	}
	if (t.active > 16)
		t.active = 16;

	/* Now convert the clocks into values we can actually stuff into
	   the chip */

	if (t.recover > 1)
		t.recover--;
	else
		t.recover = 15;

	if (t.setup > 4)
		t.setup = 0xC0;
	else
		t.setup = setup_data[t.setup];

	t.active &= 0x0F;	/* 0 = 16 */

	/* Load setup timing */
	pci_read_config_byte(pdev, arttim, &reg);
	reg &= 0x3F;
	reg |= t.setup;
	pci_write_config_byte(pdev, arttim, reg);

	/* Load active/recovery */
	pci_write_config_byte(pdev, drwtim, (t.active << 4) | t.recover);
}

/**
 *	cmd64x_set_dmamode	-	set initial DMA mode data
 *	@ap: ATA interface
 *	@adev: ATA device
 *
 *	Called to do the DMA mode setup.
 */

static void cmd64x_set_dmamode(struct ata_port *ap, struct ata_device *adev)
{
	static const u8 udma_data[] = {
		0x30, 0x20, 0x10, 0x20, 0x10, 0x00
	};
	static const u8 mwdma_data[] = {
		0x30, 0x20, 0x10
	};

	struct pci_dev *pdev = to_pci_dev(ap->host->dev);
	u8 regU, regD;

	int pciU = UDIDETCR0 + 8 * ap->port_no;
	int pciD = BMIDESR0 + 8 * ap->port_no;
	int shift = 2 * adev->devno;

	pci_read_config_byte(pdev, pciD, &regD);
	pci_read_config_byte(pdev, pciU, &regU);

	/* DMA bits off */
	regD &= ~(0x20 << adev->devno);
	/* DMA control bits */
	regU &= ~(0x30 << shift);
	/* DMA timing bits */
	regU &= ~(0x05 << adev->devno);

	if (adev->dma_mode >= XFER_UDMA_0) {
		/* Merge thge timing value */
		regU |= udma_data[adev->dma_mode - XFER_UDMA_0] << shift;
		/* Merge the control bits */
		regU |= 1 << adev->devno; /* UDMA on */
		if (adev->dma_mode > 2)	/* 15nS timing */
			regU |= 4 << adev->devno;
	} else
		regD |= mwdma_data[adev->dma_mode - XFER_MW_DMA_0] << shift;

	regD |= 0x20 << adev->devno;

	pci_write_config_byte(pdev, pciU, regU);
	pci_write_config_byte(pdev, pciD, regD);
}

/**
 *	cmd648_dma_stop	-	DMA stop callback
 *	@qc: Command in progress
 *
 *	DMA has completed.
 */

static void cmd648_bmdma_stop(struct ata_queued_cmd *qc)
{
	struct ata_port *ap = qc->ap;
	struct pci_dev *pdev = to_pci_dev(ap->host->dev);
	u8 dma_intr;
	int dma_mask = ap->port_no ? ARTTIM23_INTR_CH1 : CFR_INTR_CH0;
	int dma_reg = ap->port_no ? ARTTIM2 : CFR;

	ata_bmdma_stop(qc);

	pci_read_config_byte(pdev, dma_reg, &dma_intr);
	pci_write_config_byte(pdev, dma_reg, dma_intr | dma_mask);
}

/**
 *	cmd646r1_dma_stop	-	DMA stop callback
 *	@qc: Command in progress
 *
 *	Stub for now while investigating the r1 quirk in the old driver.
 */

static void cmd646r1_bmdma_stop(struct ata_queued_cmd *qc)
{
	ata_bmdma_stop(qc);
}

static struct scsi_host_template cmd64x_sht = {
	.module			= THIS_MODULE,
	.name			= DRV_NAME,
	.ioctl			= ata_scsi_ioctl,
	.queuecommand		= ata_scsi_queuecmd,
	.can_queue		= ATA_DEF_QUEUE,
	.this_id		= ATA_SHT_THIS_ID,
	.sg_tablesize		= LIBATA_MAX_PRD,
	.cmd_per_lun		= ATA_SHT_CMD_PER_LUN,
	.emulated		= ATA_SHT_EMULATED,
	.use_clustering		= ATA_SHT_USE_CLUSTERING,
	.proc_name		= DRV_NAME,
	.dma_boundary		= ATA_DMA_BOUNDARY,
	.slave_configure	= ata_scsi_slave_config,
	.slave_destroy		= ata_scsi_slave_destroy,
	.bios_param		= ata_std_bios_param,
};

static struct ata_port_operations cmd64x_port_ops = {
	.port_disable	= ata_port_disable,
	.set_piomode	= cmd64x_set_piomode,
	.set_dmamode	= cmd64x_set_dmamode,
	.mode_filter	= ata_pci_default_filter,
	.tf_load	= ata_tf_load,
	.tf_read	= ata_tf_read,
	.check_status 	= ata_check_status,
	.exec_command	= ata_exec_command,
	.dev_select 	= ata_std_dev_select,

	.freeze		= ata_bmdma_freeze,
	.thaw		= ata_bmdma_thaw,
	.error_handler	= ata_bmdma_error_handler,
	.post_internal_cmd = ata_bmdma_post_internal_cmd,
	.cable_detect	= ata_cable_40wire,

	.bmdma_setup 	= ata_bmdma_setup,
	.bmdma_start 	= ata_bmdma_start,
	.bmdma_stop	= ata_bmdma_stop,
	.bmdma_status 	= ata_bmdma_status,

	.qc_prep 	= ata_qc_prep,
	.qc_issue	= ata_qc_issue_prot,

	.data_xfer	= ata_data_xfer,

	.irq_handler	= ata_interrupt,
	.irq_clear	= ata_bmdma_irq_clear,
	.irq_on		= ata_irq_on,
	.irq_ack	= ata_irq_ack,

	.port_start	= ata_port_start,
};

static struct ata_port_operations cmd646r1_port_ops = {
	.port_disable	= ata_port_disable,
	.set_piomode	= cmd64x_set_piomode,
	.set_dmamode	= cmd64x_set_dmamode,
	.mode_filter	= ata_pci_default_filter,
	.tf_load	= ata_tf_load,
	.tf_read	= ata_tf_read,
	.check_status 	= ata_check_status,
	.exec_command	= ata_exec_command,
	.dev_select 	= ata_std_dev_select,

	.freeze		= ata_bmdma_freeze,
	.thaw		= ata_bmdma_thaw,
	.error_handler	= ata_bmdma_error_handler,
	.post_internal_cmd = ata_bmdma_post_internal_cmd,
	.cable_detect	= ata_cable_40wire,

	.bmdma_setup 	= ata_bmdma_setup,
	.bmdma_start 	= ata_bmdma_start,
	.bmdma_stop	= cmd646r1_bmdma_stop,
	.bmdma_status 	= ata_bmdma_status,

	.qc_prep 	= ata_qc_prep,
	.qc_issue	= ata_qc_issue_prot,

	.data_xfer	= ata_data_xfer,

	.irq_handler	= ata_interrupt,
	.irq_clear	= ata_bmdma_irq_clear,
	.irq_on		= ata_irq_on,
	.irq_ack	= ata_irq_ack,

	.port_start	= ata_port_start,
};

static struct ata_port_operations cmd648_port_ops = {
	.port_disable	= ata_port_disable,
	.set_piomode	= cmd64x_set_piomode,
	.set_dmamode	= cmd64x_set_dmamode,
	.mode_filter	= ata_pci_default_filter,
	.tf_load	= ata_tf_load,
	.tf_read	= ata_tf_read,
	.check_status 	= ata_check_status,
	.exec_command	= ata_exec_command,
	.dev_select 	= ata_std_dev_select,

	.freeze		= ata_bmdma_freeze,
	.thaw		= ata_bmdma_thaw,
	.error_handler	= ata_bmdma_error_handler,
	.post_internal_cmd = ata_bmdma_post_internal_cmd,
	.cable_detect	= cmd648_cable_detect,

	.bmdma_setup 	= ata_bmdma_setup,
	.bmdma_start 	= ata_bmdma_start,
	.bmdma_stop	= cmd648_bmdma_stop,
	.bmdma_status 	= ata_bmdma_status,

	.qc_prep 	= ata_qc_prep,
	.qc_issue	= ata_qc_issue_prot,

	.data_xfer	= ata_data_xfer,

	.irq_handler	= ata_interrupt,
	.irq_clear	= ata_bmdma_irq_clear,
	.irq_on		= ata_irq_on,
	.irq_ack	= ata_irq_ack,

	.port_start	= ata_port_start,
};

static int cmd64x_init_one(struct pci_dev *pdev, const struct pci_device_id *id)
{
	u32 class_rev;

	static const struct ata_port_info cmd_info[6] = {
		{	/* CMD 643 - no UDMA */
			.sht = &cmd64x_sht,
			.flags = ATA_FLAG_SLAVE_POSS,
			.pio_mask = 0x1f,
			.mwdma_mask = 0x07,
			.port_ops = &cmd64x_port_ops
		},
		{	/* CMD 646 with broken UDMA */
			.sht = &cmd64x_sht,
			.flags = ATA_FLAG_SLAVE_POSS,
			.pio_mask = 0x1f,
			.mwdma_mask = 0x07,
			.port_ops = &cmd64x_port_ops
		},
		{	/* CMD 646 with working UDMA */
			.sht = &cmd64x_sht,
			.flags = ATA_FLAG_SLAVE_POSS,
			.pio_mask = 0x1f,
			.mwdma_mask = 0x07,
			.udma_mask = ATA_UDMA2,
			.port_ops = &cmd64x_port_ops
		},
		{	/* CMD 646 rev 1  */
			.sht = &cmd64x_sht,
			.flags = ATA_FLAG_SLAVE_POSS,
			.pio_mask = 0x1f,
			.mwdma_mask = 0x07,
			.port_ops = &cmd646r1_port_ops
		},
		{	/* CMD 648 */
			.sht = &cmd64x_sht,
			.flags = ATA_FLAG_SLAVE_POSS,
			.pio_mask = 0x1f,
			.mwdma_mask = 0x07,
			.udma_mask = ATA_UDMA4,
			.port_ops = &cmd648_port_ops
		},
		{	/* CMD 649 */
			.sht = &cmd64x_sht,
			.flags = ATA_FLAG_SLAVE_POSS,
			.pio_mask = 0x1f,
			.mwdma_mask = 0x07,
			.udma_mask = ATA_UDMA5,
			.port_ops = &cmd648_port_ops
		}
	};
	const struct ata_port_info *ppi[] = { &cmd_info[id->driver_data], NULL };
	u8 mrdmode;

	pci_read_config_dword(pdev, PCI_CLASS_REVISION, &class_rev);
	class_rev &= 0xFF;

	if (id->driver_data == 0)	/* 643 */
		ata_pci_clear_simplex(pdev);

	if (pdev->device == PCI_DEVICE_ID_CMD_646) {
		/* Does UDMA work ? */
		if (class_rev > 4)
			ppi[0] = &cmd_info[2];
		/* Early rev with other problems ? */
		else if (class_rev == 1)
			ppi[0] = &cmd_info[3];
	}

	pci_write_config_byte(pdev, PCI_LATENCY_TIMER, 64);
	pci_read_config_byte(pdev, MRDMODE, &mrdmode);
	mrdmode &= ~ 0x30;	/* IRQ set up */
	mrdmode |= 0x02;	/* Memory read line enable */
	pci_write_config_byte(pdev, MRDMODE, mrdmode);

	/* Force PIO 0 here.. */

	/* PPC specific fixup copied from old driver */
#ifdef CONFIG_PPC
	pci_write_config_byte(pdev, UDIDETCR0, 0xF0);
#endif

	return ata_pci_init_one(pdev, ppi);
}

#ifdef CONFIG_PM
static int cmd64x_reinit_one(struct pci_dev *pdev)
{
	u8 mrdmode;
	pci_write_config_byte(pdev, PCI_LATENCY_TIMER, 64);
	pci_read_config_byte(pdev, MRDMODE, &mrdmode);
	mrdmode &= ~ 0x30;	/* IRQ set up */
	mrdmode |= 0x02;	/* Memory read line enable */
	pci_write_config_byte(pdev, MRDMODE, mrdmode);
#ifdef CONFIG_PPC
	pci_write_config_byte(pdev, UDIDETCR0, 0xF0);
#endif
	return ata_pci_device_resume(pdev);
}
#endif

static const struct pci_device_id cmd64x[] = {
	{ PCI_VDEVICE(CMD, PCI_DEVICE_ID_CMD_643), 0 },
	{ PCI_VDEVICE(CMD, PCI_DEVICE_ID_CMD_646), 1 },
	{ PCI_VDEVICE(CMD, PCI_DEVICE_ID_CMD_648), 4 },
	{ PCI_VDEVICE(CMD, PCI_DEVICE_ID_CMD_649), 5 },

	{ },
};

static struct pci_driver cmd64x_pci_driver = {
	.name 		= DRV_NAME,
	.id_table	= cmd64x,
	.probe 		= cmd64x_init_one,
	.remove		= ata_pci_remove_one,
#ifdef CONFIG_PM
	.suspend	= ata_pci_device_suspend,
	.resume		= cmd64x_reinit_one,
#endif
};

static int __init cmd64x_init(void)
{
	return pci_register_driver(&cmd64x_pci_driver);
}

static void __exit cmd64x_exit(void)
{
	pci_unregister_driver(&cmd64x_pci_driver);
}

MODULE_AUTHOR("Alan Cox");
MODULE_DESCRIPTION("low-level driver for CMD64x series PATA controllers");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, cmd64x);
MODULE_VERSION(DRV_VERSION);

module_init(cmd64x_init);
module_exit(cmd64x_exit);
