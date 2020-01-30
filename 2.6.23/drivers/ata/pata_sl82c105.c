/*
 * pata_sl82c105.c 	- SL82C105 PATA for new ATA layer
 *			  (C) 2005 Red Hat Inc
 *			  Alan Cox <alan@redhat.com>
 *
 * Based in part on linux/drivers/ide/pci/sl82c105.c
 * 		SL82C105/Winbond 553 IDE driver
 *
 * and in part on the documentation and errata sheet
 *
 *
 * Note: The controller like many controllers has shared timings for
 * PIO and DMA. We thus flip to the DMA timings in dma_start and flip back
 * in the dma_stop function. Thus we actually don't need a set_dmamode
 * method as the PIO method is always called and will set the right PIO
 * timing parameters.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <scsi/scsi_host.h>
#include <linux/libata.h>

#define DRV_NAME "pata_sl82c105"
#define DRV_VERSION "0.3.2"

enum {
	/*
	 * SL82C105 PCI config register 0x40 bits.
	 */
	CTRL_IDE_IRQB	=	(1 << 30),
	CTRL_IDE_IRQA   =	(1 << 28),
	CTRL_LEGIRQ     =	(1 << 11),
	CTRL_P1F16      =	(1 << 5),
	CTRL_P1EN       =	(1 << 4),
	CTRL_P0F16      =	(1 << 1),
	CTRL_P0EN       =	(1 << 0)
};

/**
 *	sl82c105_pre_reset		-	probe begin
 *	@ap: ATA port
 *	@deadline: deadline jiffies for the operation
 *
 *	Set up cable type and use generic probe init
 */

static int sl82c105_pre_reset(struct ata_port *ap, unsigned long deadline)
{
	static const struct pci_bits sl82c105_enable_bits[] = {
		{ 0x40, 1, 0x01, 0x01 },
		{ 0x40, 1, 0x10, 0x10 }
	};
	struct pci_dev *pdev = to_pci_dev(ap->host->dev);

	if (ap->port_no && !pci_test_config_bits(pdev, &sl82c105_enable_bits[ap->port_no]))
		return -ENOENT;
	return ata_std_prereset(ap, deadline);
}


static void sl82c105_error_handler(struct ata_port *ap)
{
	ata_bmdma_drive_eh(ap, sl82c105_pre_reset, ata_std_softreset, NULL, ata_std_postreset);
}


/**
 *	sl82c105_configure_piomode	-	set chip PIO timing
 *	@ap: ATA interface
 *	@adev: ATA device
 *	@pio: PIO mode
 *
 *	Called to do the PIO mode setup. Our timing registers are shared
 *	so a configure_dmamode call will undo any work we do here and vice
 *	versa
 */

static void sl82c105_configure_piomode(struct ata_port *ap, struct ata_device *adev, int pio)
{
	struct pci_dev *pdev = to_pci_dev(ap->host->dev);
	static u16 pio_timing[5] = {
		0x50D, 0x407, 0x304, 0x242, 0x240
	};
	u16 dummy;
	int timing = 0x44 + (8 * ap->port_no) + (4 * adev->devno);

	pci_write_config_word(pdev, timing, pio_timing[pio]);
	/* Can we lose this oddity of the old driver */
	pci_read_config_word(pdev, timing, &dummy);
}

/**
 *	sl82c105_set_piomode	-	set initial PIO mode data
 *	@ap: ATA interface
 *	@adev: ATA device
 *
 *	Called to do the PIO mode setup. Our timing registers are shared
 *	but we want to set the PIO timing by default.
 */

static void sl82c105_set_piomode(struct ata_port *ap, struct ata_device *adev)
{
	sl82c105_configure_piomode(ap, adev, adev->pio_mode - XFER_PIO_0);
}

/**
 *	sl82c105_configure_dmamode	-	set DMA mode in chip
 *	@ap: ATA interface
 *	@adev: ATA device
 *
 *	Load DMA cycle times into the chip ready for a DMA transfer
 *	to occur.
 */

static void sl82c105_configure_dmamode(struct ata_port *ap, struct ata_device *adev)
{
	struct pci_dev *pdev = to_pci_dev(ap->host->dev);
	static u16 dma_timing[3] = {
		0x707, 0x201, 0x200
	};
	u16 dummy;
	int timing = 0x44 + (8 * ap->port_no) + (4 * adev->devno);
	int dma = adev->dma_mode - XFER_MW_DMA_0;

	pci_write_config_word(pdev, timing, dma_timing[dma]);
	/* Can we lose this oddity of the old driver */
	pci_read_config_word(pdev, timing, &dummy);
}

/**
 *	sl82c105_reset_engine	-	Reset the DMA engine
 *	@ap: ATA interface
 *
 *	The sl82c105 has some serious problems with the DMA engine
 *	when transfers don't run as expected or ATAPI is used. The
 *	recommended fix is to reset the engine each use using a chip
 *	test register.
 */

static void sl82c105_reset_engine(struct ata_port *ap)
{
	struct pci_dev *pdev = to_pci_dev(ap->host->dev);
	u16 val;

	pci_read_config_word(pdev, 0x7E, &val);
	pci_write_config_word(pdev, 0x7E, val | 4);
	pci_write_config_word(pdev, 0x7E, val & ~4);
}

/**
 *	sl82c105_bmdma_start		-	DMA engine begin
 *	@qc: ATA command
 *
 *	Reset the DMA engine each use as recommended by the errata
 *	document.
 *
 *	FIXME: if we switch clock at BMDMA start/end we might get better
 *	PIO performance on DMA capable devices.
 */

static void sl82c105_bmdma_start(struct ata_queued_cmd *qc)
{
	struct ata_port *ap = qc->ap;

	udelay(100);
	sl82c105_reset_engine(ap);
	udelay(100);

	/* Set the clocks for DMA */
	sl82c105_configure_dmamode(ap, qc->dev);
	/* Activate DMA */
	ata_bmdma_start(qc);
}

/**
 *	sl82c105_bmdma_end		-	DMA engine stop
 *	@qc: ATA command
 *
 *	Reset the DMA engine each use as recommended by the errata
 *	document.
 *
 *	This function is also called to turn off DMA when a timeout occurs
 *	during DMA operation. In both cases we need to reset the engine,
 *	so no actual eng_timeout handler is required.
 *
 *	We assume bmdma_stop is always called if bmdma_start as called. If
 *	not then we may need to wrap qc_issue.
 */

static void sl82c105_bmdma_stop(struct ata_queued_cmd *qc)
{
	struct ata_port *ap = qc->ap;

	ata_bmdma_stop(qc);
	sl82c105_reset_engine(ap);
	udelay(100);

	/* This will redo the initial setup of the DMA device to matching
	   PIO timings */
	sl82c105_set_piomode(ap, qc->dev);
}

static struct scsi_host_template sl82c105_sht = {
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

static struct ata_port_operations sl82c105_port_ops = {
	.port_disable	= ata_port_disable,
	.set_piomode	= sl82c105_set_piomode,
	.mode_filter	= ata_pci_default_filter,

	.tf_load	= ata_tf_load,
	.tf_read	= ata_tf_read,
	.check_status 	= ata_check_status,
	.exec_command	= ata_exec_command,
	.dev_select 	= ata_std_dev_select,

	.freeze		= ata_bmdma_freeze,
	.thaw		= ata_bmdma_thaw,
	.error_handler	= sl82c105_error_handler,
	.post_internal_cmd = ata_bmdma_post_internal_cmd,
	.cable_detect	= ata_cable_40wire,

	.bmdma_setup 	= ata_bmdma_setup,
	.bmdma_start 	= sl82c105_bmdma_start,
	.bmdma_stop	= sl82c105_bmdma_stop,
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

/**
 *	sl82c105_bridge_revision	-	find bridge version
 *	@pdev: PCI device for the ATA function
 *
 *	Locates the PCI bridge associated with the ATA function and
 *	providing it is a Winbond 553 reports the revision. If it cannot
 *	find a revision or the right device it returns -1
 */

static int sl82c105_bridge_revision(struct pci_dev *pdev)
{
	struct pci_dev *bridge;

	/*
	 * The bridge should be part of the same device, but function 0.
	 */
	bridge = pci_get_slot(pdev->bus,
			       PCI_DEVFN(PCI_SLOT(pdev->devfn), 0));
	if (!bridge)
		return -1;

	/*
	 * Make sure it is a Winbond 553 and is an ISA bridge.
	 */
	if (bridge->vendor != PCI_VENDOR_ID_WINBOND ||
	    bridge->device != PCI_DEVICE_ID_WINBOND_83C553 ||
	    bridge->class >> 8 != PCI_CLASS_BRIDGE_ISA) {
	    	pci_dev_put(bridge);
		return -1;
	}
	/*
	 * We need to find function 0's revision, not function 1
	 */
	pci_dev_put(bridge);
	return bridge->revision;
}


static int sl82c105_init_one(struct pci_dev *dev, const struct pci_device_id *id)
{
	static const struct ata_port_info info_dma = {
		.sht = &sl82c105_sht,
		.flags = ATA_FLAG_SLAVE_POSS,
		.pio_mask = 0x1f,
		.mwdma_mask = 0x07,
		.port_ops = &sl82c105_port_ops
	};
	static const struct ata_port_info info_early = {
		.sht = &sl82c105_sht,
		.flags = ATA_FLAG_SLAVE_POSS,
		.pio_mask = 0x1f,
		.port_ops = &sl82c105_port_ops
	};
	/* for now use only the first port */
	const struct ata_port_info *ppi[] = { &info_early,
					       &ata_dummy_port_info };
	u32 val;
	int rev;

	rev = sl82c105_bridge_revision(dev);

	if (rev == -1)
		dev_printk(KERN_WARNING, &dev->dev, "pata_sl82c105: Unable to find bridge, disabling DMA.\n");
	else if (rev <= 5)
		dev_printk(KERN_WARNING, &dev->dev, "pata_sl82c105: Early bridge revision, no DMA available.\n");
	else
		ppi[0] = &info_dma;

	pci_read_config_dword(dev, 0x40, &val);
	val |= CTRL_P0EN | CTRL_P0F16 | CTRL_P1F16;
	pci_write_config_dword(dev, 0x40, val);

	return ata_pci_init_one(dev, ppi);
}

static const struct pci_device_id sl82c105[] = {
	{ PCI_VDEVICE(WINBOND, PCI_DEVICE_ID_WINBOND_82C105), },

	{ },
};

static struct pci_driver sl82c105_pci_driver = {
	.name 		= DRV_NAME,
	.id_table	= sl82c105,
	.probe 		= sl82c105_init_one,
	.remove		= ata_pci_remove_one
};

static int __init sl82c105_init(void)
{
	return pci_register_driver(&sl82c105_pci_driver);
}

static void __exit sl82c105_exit(void)
{
	pci_unregister_driver(&sl82c105_pci_driver);
}

MODULE_AUTHOR("Alan Cox");
MODULE_DESCRIPTION("low-level driver for Sl82c105");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, sl82c105);
MODULE_VERSION(DRV_VERSION);

module_init(sl82c105_init);
module_exit(sl82c105_exit);
