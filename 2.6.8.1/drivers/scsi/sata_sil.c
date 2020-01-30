/*
 *  sata_sil.c - Silicon Image SATA
 *
 *  Maintained by:  Jeff Garzik <jgarzik@pobox.com>
 *  		    Please ALWAYS copy linux-ide@vger.kernel.org
 *		    on emails.
 *
 *  Copyright 2003 Red Hat, Inc.
 *  Copyright 2003 Benjamin Herrenschmidt <benh@kernel.crashing.org>
 *
 *  The contents of this file are subject to the Open
 *  Software License version 1.1 that can be found at
 *  http://www.opensource.org/licenses/osl-1.1.txt and is included herein
 *  by reference.
 *
 *  Alternatively, the contents of this file may be used under the terms
 *  of the GNU General Public License version 2 (the "GPL") as distributed
 *  in the kernel source COPYING file, in which case the provisions of
 *  the GPL are applicable instead of the above.  If you wish to allow
 *  the use of your version of this file only under the terms of the
 *  GPL and not to allow others to use your version of this file under
 *  the OSL, indicate your decision by deleting the provisions above and
 *  replace them with the notice and other provisions required by the GPL.
 *  If you do not delete the provisions above, a recipient may use your
 *  version of this file under either the OSL or the GPL.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include "scsi.h"
#include <scsi/scsi_host.h>
#include <linux/libata.h>

#define DRV_NAME	"sata_sil"
#define DRV_VERSION	"0.54"

enum {
	sil_3112		= 0,
	sil_3114		= 1,

	SIL_SYSCFG		= 0x48,
	SIL_MASK_IDE0_INT	= (1 << 22),
	SIL_MASK_IDE1_INT	= (1 << 23),
	SIL_MASK_IDE2_INT	= (1 << 24),
	SIL_MASK_IDE3_INT	= (1 << 25),
	SIL_MASK_2PORT		= SIL_MASK_IDE0_INT | SIL_MASK_IDE1_INT,
	SIL_MASK_4PORT		= SIL_MASK_2PORT |
				  SIL_MASK_IDE2_INT | SIL_MASK_IDE3_INT,

	SIL_IDE2_BMDMA		= 0x200,

	SIL_INTR_STEERING	= (1 << 1),
	SIL_QUIRK_MOD15WRITE	= (1 << 0),
	SIL_QUIRK_UDMA5MAX	= (1 << 1),
};

static int sil_init_one (struct pci_dev *pdev, const struct pci_device_id *ent);
static void sil_dev_config(struct ata_port *ap, struct ata_device *dev);
static u32 sil_scr_read (struct ata_port *ap, unsigned int sc_reg);
static void sil_scr_write (struct ata_port *ap, unsigned int sc_reg, u32 val);
static void sil_post_set_mode (struct ata_port *ap);

static struct pci_device_id sil_pci_tbl[] = {
	{ 0x1095, 0x3112, PCI_ANY_ID, PCI_ANY_ID, 0, 0, sil_3112 },
	{ 0x1095, 0x0240, PCI_ANY_ID, PCI_ANY_ID, 0, 0, sil_3112 },
	{ 0x1095, 0x3512, PCI_ANY_ID, PCI_ANY_ID, 0, 0, sil_3112 },
	{ 0x1095, 0x3114, PCI_ANY_ID, PCI_ANY_ID, 0, 0, sil_3114 },
	{ }	/* terminate list */
};


/* TODO firmware versions should be added - eric */
struct sil_drivelist {
	const char * product;
	unsigned int quirk;
} sil_blacklist [] = {
	{ "ST320012AS",		SIL_QUIRK_MOD15WRITE },
	{ "ST330013AS",		SIL_QUIRK_MOD15WRITE },
	{ "ST340017AS",		SIL_QUIRK_MOD15WRITE },
	{ "ST360015AS",		SIL_QUIRK_MOD15WRITE },
	{ "ST380023AS",		SIL_QUIRK_MOD15WRITE },
	{ "ST3120023AS",	SIL_QUIRK_MOD15WRITE },
	{ "ST3160023AS",	SIL_QUIRK_MOD15WRITE },
	{ "ST340014ASL",	SIL_QUIRK_MOD15WRITE },
	{ "ST360014ASL",	SIL_QUIRK_MOD15WRITE },
	{ "ST380011ASL",	SIL_QUIRK_MOD15WRITE },
	{ "ST3120022ASL",	SIL_QUIRK_MOD15WRITE },
	{ "ST3160021ASL",	SIL_QUIRK_MOD15WRITE },
	{ "Maxtor 4D060H3",	SIL_QUIRK_UDMA5MAX },
	{ }
};

static struct pci_driver sil_pci_driver = {
	.name			= DRV_NAME,
	.id_table		= sil_pci_tbl,
	.probe			= sil_init_one,
	.remove			= ata_pci_remove_one,
};

static Scsi_Host_Template sil_sht = {
	.module			= THIS_MODULE,
	.name			= DRV_NAME,
	.queuecommand		= ata_scsi_queuecmd,
	.eh_strategy_handler	= ata_scsi_error,
	.can_queue		= ATA_DEF_QUEUE,
	.this_id		= ATA_SHT_THIS_ID,
	.sg_tablesize		= LIBATA_MAX_PRD,
	.max_sectors		= ATA_MAX_SECTORS,
	.cmd_per_lun		= ATA_SHT_CMD_PER_LUN,
	.emulated		= ATA_SHT_EMULATED,
	.use_clustering		= ATA_SHT_USE_CLUSTERING,
	.proc_name		= DRV_NAME,
	.dma_boundary		= ATA_DMA_BOUNDARY,
	.slave_configure	= ata_scsi_slave_config,
	.bios_param		= ata_std_bios_param,
};

static struct ata_port_operations sil_ops = {
	.port_disable		= ata_port_disable,
	.dev_config		= sil_dev_config,
	.tf_load		= ata_tf_load_mmio,
	.tf_read		= ata_tf_read_mmio,
	.check_status		= ata_check_status_mmio,
	.exec_command		= ata_exec_command_mmio,
	.phy_reset		= sata_phy_reset,
	.post_set_mode		= sil_post_set_mode,
	.bmdma_setup            = ata_bmdma_setup_mmio,
	.bmdma_start            = ata_bmdma_start_mmio,
	.qc_prep		= ata_qc_prep,
	.qc_issue		= ata_qc_issue_prot,
	.eng_timeout		= ata_eng_timeout,
	.irq_handler		= ata_interrupt,
	.irq_clear		= ata_bmdma_irq_clear,
	.scr_read		= sil_scr_read,
	.scr_write		= sil_scr_write,
	.port_start		= ata_port_start,
	.port_stop		= ata_port_stop,
};

static struct ata_port_info sil_port_info[] = {
	/* sil_3112 */
	{
		.sht		= &sil_sht,
		.host_flags	= ATA_FLAG_SATA | ATA_FLAG_NO_LEGACY |
				  ATA_FLAG_SRST | ATA_FLAG_MMIO,
		.pio_mask	= 0x03,			/* pio3-4 */
		.udma_mask	= 0x3f,			/* udma0-5 */
		.port_ops	= &sil_ops,
	}, /* sil_3114 */
	{
		.sht		= &sil_sht,
		.host_flags	= ATA_FLAG_SATA | ATA_FLAG_NO_LEGACY |
				  ATA_FLAG_SRST | ATA_FLAG_MMIO,
		.pio_mask	= 0x03,			/* pio3-4 */
		.udma_mask	= 0x3f,			/* udma0-5 */
		.port_ops	= &sil_ops,
	},
};

/* per-port register offsets */
/* TODO: we can probably calculate rather than use a table */
static const struct {
	unsigned long tf;	/* ATA taskfile register block */
	unsigned long ctl;	/* ATA control/altstatus register block */
	unsigned long bmdma;	/* DMA register block */
	unsigned long scr;	/* SATA control register block */
	unsigned long sien;	/* SATA Interrupt Enable register */
	unsigned long xfer_mode;/* data transfer mode register */
} sil_port[] = {
	/* port 0 ... */
	{ 0x80, 0x8A, 0x00, 0x100, 0x148, 0xb4 },
	{ 0xC0, 0xCA, 0x08, 0x180, 0x1c8, 0xf4 },
	{ 0x280, 0x28A, 0x200, 0x300, 0x348, 0x2b4 },
	{ 0x2C0, 0x2CA, 0x208, 0x380, 0x3c8, 0x2f4 },
	/* ... port 3 */
};

MODULE_AUTHOR("Jeff Garzik");
MODULE_DESCRIPTION("low-level driver for Silicon Image SATA controller");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, sil_pci_tbl);

static void sil_post_set_mode (struct ata_port *ap)
{
	struct ata_host_set *host_set = ap->host_set;
	struct ata_device *dev;
	void *addr = host_set->mmio_base + sil_port[ap->port_no].xfer_mode;
	u32 tmp, dev_mode[2];
	unsigned int i;

	for (i = 0; i < 2; i++) {
		dev = &ap->device[i];
		if (!ata_dev_present(dev))
			dev_mode[i] = 0;	/* PIO0/1/2 */
		else if (dev->flags & ATA_DFLAG_PIO)
			dev_mode[i] = 1;	/* PIO3/4 */
		else
			dev_mode[i] = 3;	/* UDMA */
		/* value 2 indicates MDMA */
	}

	tmp = readl(addr);
	tmp &= ~((1<<5) | (1<<4) | (1<<1) | (1<<0));
	tmp |= dev_mode[0];
	tmp |= (dev_mode[1] << 4);
	writel(tmp, addr);
	readl(addr);	/* flush */
}

static inline unsigned long sil_scr_addr(struct ata_port *ap, unsigned int sc_reg)
{
	unsigned long offset = ap->ioaddr.scr_addr;

	switch (sc_reg) {
	case SCR_STATUS:
		return offset + 4;
	case SCR_ERROR:
		return offset + 8;
	case SCR_CONTROL:
		return offset;
	default:
		/* do nothing */
		break;
	}

	return 0;
}

static u32 sil_scr_read (struct ata_port *ap, unsigned int sc_reg)
{
	void *mmio = (void *) sil_scr_addr(ap, sc_reg);
	if (mmio)
		return readl(mmio);
	return 0xffffffffU;
}

static void sil_scr_write (struct ata_port *ap, unsigned int sc_reg, u32 val)
{
	void *mmio = (void *) sil_scr_addr(ap, sc_reg);
	if (mmio)
		writel(val, mmio);
}

/**
 *	sil_dev_config - Apply device/host-specific errata fixups
 *	@ap: Port containing device to be examined
 *	@dev: Device to be examined
 *
 *	After the IDENTIFY [PACKET] DEVICE step is complete, and a
 *	device is known to be present, this function is called.
 *	We apply two errata fixups which are specific to Silicon Image,
 *	a Seagate and a Maxtor fixup.
 *
 *	For certain Seagate devices, we must limit the maximum sectors
 *	to under 8K.
 *
 *	For certain Maxtor devices, we must not program the drive
 *	beyond udma5.
 *
 *	Both fixups are unfairly pessimistic.  As soon as I get more
 *	information on these errata, I will create a more exhaustive
 *	list, and apply the fixups to only the specific
 *	devices/hosts/firmwares that need it.
 *
 *	20040111 - Seagate drives affected by the Mod15Write bug are blacklisted
 *	The Maxtor quirk is in the blacklist, but I'm keeping the original
 *	pessimistic fix for the following reasons...
 *	- There seems to be less info on it, only one device gleaned off the
 *	Windows	driver, maybe only one is affected.  More info would be greatly
 *	appreciated.
 *	- But then again UDMA5 is hardly anything to complain about
 */
static void sil_dev_config(struct ata_port *ap, struct ata_device *dev)
{
	unsigned int n, quirks = 0;
	unsigned char model_num[40];
	const char *s;
	unsigned int len;

	ata_dev_id_string(dev, model_num, ATA_ID_PROD_OFS,
			  sizeof(model_num));
	s = &model_num[0];
	len = strnlen(s, sizeof(model_num));

	/* ATAPI specifies that empty space is blank-filled; remove blanks */
	while ((len > 0) && (s[len - 1] == ' '))
		len--;

	for (n = 0; sil_blacklist[n].product; n++) 
		if (!memcmp(sil_blacklist[n].product, s,
			    strlen(sil_blacklist[n].product))) {
			quirks = sil_blacklist[n].quirk;
			break;
		}
	
	/* limit requests to 15 sectors */
	if (quirks & SIL_QUIRK_MOD15WRITE) {
		printk(KERN_INFO "ata%u(%u): applying Seagate errata fix\n",
		       ap->id, dev->devno);
		ap->host->max_sectors = 15;
		ap->host->hostt->max_sectors = 15;
		dev->flags |= ATA_DFLAG_LOCK_SECTORS;
		return;
	}

	/* limit to udma5 */
	if (quirks & SIL_QUIRK_UDMA5MAX) {
		printk(KERN_INFO "ata%u(%u): applying Maxtor errata fix %s\n",
		       ap->id, dev->devno, s);
		ap->udma_mask &= ATA_UDMA5;
		return;
	}
}

static int sil_init_one (struct pci_dev *pdev, const struct pci_device_id *ent)
{
	static int printed_version;
	struct ata_probe_ent *probe_ent = NULL;
	unsigned long base;
	void *mmio_base;
	int rc;
	unsigned int i;
	u32 tmp, irq_mask;

	if (!printed_version++)
		printk(KERN_DEBUG DRV_NAME " version " DRV_VERSION "\n");

	/*
	 * If this driver happens to only be useful on Apple's K2, then
	 * we should check that here as it has a normal Serverworks ID
	 */
	rc = pci_enable_device(pdev);
	if (rc)
		return rc;

	rc = pci_request_regions(pdev, DRV_NAME);
	if (rc)
		goto err_out;

	rc = pci_set_dma_mask(pdev, ATA_DMA_MASK);
	if (rc)
		goto err_out_regions;
	rc = pci_set_consistent_dma_mask(pdev, ATA_DMA_MASK);
	if (rc)
		goto err_out_regions;

	probe_ent = kmalloc(sizeof(*probe_ent), GFP_KERNEL);
	if (probe_ent == NULL) {
		rc = -ENOMEM;
		goto err_out_regions;
	}

	memset(probe_ent, 0, sizeof(*probe_ent));
	INIT_LIST_HEAD(&probe_ent->node);
	probe_ent->pdev = pdev;
	probe_ent->port_ops = sil_port_info[ent->driver_data].port_ops;
	probe_ent->sht = sil_port_info[ent->driver_data].sht;
	probe_ent->n_ports = (ent->driver_data == sil_3114) ? 4 : 2;
	probe_ent->pio_mask = sil_port_info[ent->driver_data].pio_mask;
	probe_ent->udma_mask = sil_port_info[ent->driver_data].udma_mask;
       	probe_ent->irq = pdev->irq;
       	probe_ent->irq_flags = SA_SHIRQ;
	probe_ent->host_flags = sil_port_info[ent->driver_data].host_flags;

	mmio_base = ioremap(pci_resource_start(pdev, 5),
		            pci_resource_len(pdev, 5));
	if (mmio_base == NULL) {
		rc = -ENOMEM;
		goto err_out_free_ent;
	}

	probe_ent->mmio_base = mmio_base;

	base = (unsigned long) mmio_base;

	for (i = 0; i < probe_ent->n_ports; i++) {
		probe_ent->port[i].cmd_addr = base + sil_port[i].tf;
		probe_ent->port[i].altstatus_addr =
		probe_ent->port[i].ctl_addr = base + sil_port[i].ctl;
		probe_ent->port[i].bmdma_addr = base + sil_port[i].bmdma;
		probe_ent->port[i].scr_addr = base + sil_port[i].scr;
		ata_std_ports(&probe_ent->port[i]);
	}

	if (ent->driver_data == sil_3114) {
		irq_mask = SIL_MASK_4PORT;

		/* flip the magic "make 4 ports work" bit */
		tmp = readl(mmio_base + SIL_IDE2_BMDMA);
		if ((tmp & SIL_INTR_STEERING) == 0)
			writel(tmp | SIL_INTR_STEERING,
			       mmio_base + SIL_IDE2_BMDMA);

	} else {
		irq_mask = SIL_MASK_2PORT;
	}

	/* make sure IDE0/1/2/3 interrupts are not masked */
	tmp = readl(mmio_base + SIL_SYSCFG);
	if (tmp & irq_mask) {
		tmp &= ~irq_mask;
		writel(tmp, mmio_base + SIL_SYSCFG);
		readl(mmio_base + SIL_SYSCFG);	/* flush */
	}

	/* mask all SATA phy-related interrupts */
	/* TODO: unmask bit 6 (SError N bit) for hotplug */
	for (i = 0; i < probe_ent->n_ports; i++)
		writel(0, mmio_base + sil_port[i].sien);

	pci_set_master(pdev);

	/* FIXME: check ata_device_add return value */
	ata_device_add(probe_ent);
	kfree(probe_ent);

	return 0;

err_out_free_ent:
	kfree(probe_ent);
err_out_regions:
	pci_release_regions(pdev);
err_out:
	pci_disable_device(pdev);
	return rc;
}

static int __init sil_init(void)
{
	return pci_module_init(&sil_pci_driver);
}

static void __exit sil_exit(void)
{
	pci_unregister_driver(&sil_pci_driver);
}


module_init(sil_init);
module_exit(sil_exit);
