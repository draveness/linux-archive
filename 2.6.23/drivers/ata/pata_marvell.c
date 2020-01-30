/*
 *	Marvell PATA driver.
 *
 *	For the moment we drive the PATA port in legacy mode. That
 *	isn't making full use of the device functionality but it is
 *	easy to get working.
 *
 *	(c) 2006 Red Hat  <alan@redhat.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <scsi/scsi_host.h>
#include <linux/libata.h>
#include <linux/ata.h>

#define DRV_NAME	"pata_marvell"
#define DRV_VERSION	"0.1.4"

/**
 *	marvell_pre_reset	-	check for 40/80 pin
 *	@ap: Port
 *	@deadline: deadline jiffies for the operation
 *
 *	Perform the PATA port setup we need.
 */

static int marvell_pre_reset(struct ata_port *ap, unsigned long deadline)
{
	struct pci_dev *pdev = to_pci_dev(ap->host->dev);
	u32 devices;
	void __iomem *barp;
	int i;

	/* Check if our port is enabled */

	barp = pci_iomap(pdev, 5, 0x10);
	if (barp == NULL)
		return -ENOMEM;
	printk("BAR5:");
	for(i = 0; i <= 0x0F; i++)
		printk("%02X:%02X ", i, ioread8(barp + i));
	printk("\n");

	devices = ioread32(barp + 0x0C);
	pci_iounmap(pdev, barp);

	if ((pdev->device == 0x6145) && (ap->port_no == 0) &&
	    (!(devices & 0x10)))	/* PATA enable ? */
		return -ENOENT;

	return ata_std_prereset(ap, deadline);
}

static int marvell_cable_detect(struct ata_port *ap)
{
	/* Cable type */
	switch(ap->port_no)
	{
	case 0:
		if (ioread8(ap->ioaddr.bmdma_addr + 1) & 1)
			return ATA_CBL_PATA40;
		return ATA_CBL_PATA80;
	case 1: /* Legacy SATA port */
		return ATA_CBL_SATA;
	}

	BUG();
	return 0;	/* Our BUG macro needs the right markup */
}

/**
 *	marvell_error_handler - Setup and error handler
 *	@ap: Port to handle
 *
 *	LOCKING:
 *	None (inherited from caller).
 */

static void marvell_error_handler(struct ata_port *ap)
{
	return ata_bmdma_drive_eh(ap, marvell_pre_reset, ata_std_softreset,
				  NULL, ata_std_postreset);
}

/* No PIO or DMA methods needed for this device */

static struct scsi_host_template marvell_sht = {
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
	/* Use standard CHS mapping rules */
	.bios_param		= ata_std_bios_param,
};

static const struct ata_port_operations marvell_ops = {
	.port_disable		= ata_port_disable,

	/* Task file is PCI ATA format, use helpers */
	.tf_load		= ata_tf_load,
	.tf_read		= ata_tf_read,
	.check_status		= ata_check_status,
	.exec_command		= ata_exec_command,
	.dev_select		= ata_std_dev_select,

	.freeze			= ata_bmdma_freeze,
	.thaw			= ata_bmdma_thaw,
	.error_handler		= marvell_error_handler,
	.post_internal_cmd	= ata_bmdma_post_internal_cmd,
	.cable_detect		= marvell_cable_detect,

	/* BMDMA handling is PCI ATA format, use helpers */
	.bmdma_setup		= ata_bmdma_setup,
	.bmdma_start		= ata_bmdma_start,
	.bmdma_stop		= ata_bmdma_stop,
	.bmdma_status		= ata_bmdma_status,
	.qc_prep		= ata_qc_prep,
	.qc_issue		= ata_qc_issue_prot,
	.data_xfer		= ata_data_xfer,

	/* Timeout handling */
	.irq_handler		= ata_interrupt,
	.irq_clear		= ata_bmdma_irq_clear,
	.irq_on			= ata_irq_on,
	.irq_ack		= ata_irq_ack,

	/* Generic PATA PCI ATA helpers */
	.port_start		= ata_port_start,
};


/**
 *	marvell_init_one - Register Marvell ATA PCI device with kernel services
 *	@pdev: PCI device to register
 *	@ent: Entry in marvell_pci_tbl matching with @pdev
 *
 *	Called from kernel PCI layer.
 *
 *	LOCKING:
 *	Inherited from PCI layer (may sleep).
 *
 *	RETURNS:
 *	Zero on success, or -ERRNO value.
 */

static int marvell_init_one (struct pci_dev *pdev, const struct pci_device_id *id)
{
	static const struct ata_port_info info = {
		.sht		= &marvell_sht,
		.flags		= ATA_FLAG_SLAVE_POSS,

		.pio_mask	= 0x1f,
		.mwdma_mask	= 0x07,
		.udma_mask 	= ATA_UDMA5,

		.port_ops	= &marvell_ops,
	};
	static const struct ata_port_info info_sata = {
		.sht		= &marvell_sht,
		/* Slave possible as its magically mapped not real */
		.flags		= ATA_FLAG_SLAVE_POSS,

		.pio_mask	= 0x1f,
		.mwdma_mask	= 0x07,
		.udma_mask 	= ATA_UDMA6,

		.port_ops	= &marvell_ops,
	};
	const struct ata_port_info *ppi[] = { &info, &info_sata };

	if (pdev->device == 0x6101)
		ppi[1] = &ata_dummy_port_info;

	return ata_pci_init_one(pdev, ppi);
}

static const struct pci_device_id marvell_pci_tbl[] = {
	{ PCI_DEVICE(0x11AB, 0x6101), },
	{ PCI_DEVICE(0x11AB, 0x6121), },
	{ PCI_DEVICE(0x11AB, 0x6123), },
	{ PCI_DEVICE(0x11AB, 0x6145), },
	{ }	/* terminate list */
};

static struct pci_driver marvell_pci_driver = {
	.name			= DRV_NAME,
	.id_table		= marvell_pci_tbl,
	.probe			= marvell_init_one,
	.remove			= ata_pci_remove_one,
#ifdef CONFIG_PM
	.suspend		= ata_pci_device_suspend,
	.resume			= ata_pci_device_resume,
#endif
};

static int __init marvell_init(void)
{
	return pci_register_driver(&marvell_pci_driver);
}

static void __exit marvell_exit(void)
{
	pci_unregister_driver(&marvell_pci_driver);
}

module_init(marvell_init);
module_exit(marvell_exit);

MODULE_AUTHOR("Alan Cox");
MODULE_DESCRIPTION("SCSI low-level driver for Marvell ATA in legacy mode");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, marvell_pci_tbl);
MODULE_VERSION(DRV_VERSION);

