/*
 *  sata_sis.c - Silicon Integrated Systems SATA
 *
 *  Maintained by:  Uwe Koziolek
 *  		    Please ALWAYS copy linux-ide@vger.kernel.org
 *		    on emails.
 *
 *  Copyright 2004 Uwe Koziolek
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

#include <linux/config.h>
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

#define DRV_NAME	"sata_sis"
#define DRV_VERSION	"0.10"

enum {
	sis_180			= 0,
	SIS_SCR_PCI_BAR		= 5,

	/* PCI configuration registers */
	SIS_GENCTL		= 0x54, /* IDE General Control register */
	SIS_SCR_BASE		= 0xc0, /* sata0 phy SCR registers */
	SIS_SATA1_OFS		= 0x10, /* offset from sata0->sata1 phy regs */

	/* random bits */
	SIS_FLAG_CFGSCR		= (1 << 30), /* host flag: SCRs via PCI cfg */

	GENCTL_IOMAPPED_SCR	= (1 << 26), /* if set, SCRs are in IO space */
};

static int sis_init_one (struct pci_dev *pdev, const struct pci_device_id *ent);
static u32 sis_scr_read (struct ata_port *ap, unsigned int sc_reg);
static void sis_scr_write (struct ata_port *ap, unsigned int sc_reg, u32 val);

static struct pci_device_id sis_pci_tbl[] = {
	{ PCI_VENDOR_ID_SI, 0x180, PCI_ANY_ID, PCI_ANY_ID, 0, 0, sis_180 },
	{ PCI_VENDOR_ID_SI, 0x181, PCI_ANY_ID, PCI_ANY_ID, 0, 0, sis_180 },
	{ }	/* terminate list */
};


static struct pci_driver sis_pci_driver = {
	.name			= DRV_NAME,
	.id_table		= sis_pci_tbl,
	.probe			= sis_init_one,
	.remove			= ata_pci_remove_one,
};

static Scsi_Host_Template sis_sht = {
	.module			= THIS_MODULE,
	.name			= DRV_NAME,
	.queuecommand		= ata_scsi_queuecmd,
	.eh_strategy_handler	= ata_scsi_error,
	.can_queue		= ATA_DEF_QUEUE,
	.this_id		= ATA_SHT_THIS_ID,
	.sg_tablesize		= ATA_MAX_PRD,
	.max_sectors		= ATA_MAX_SECTORS,
	.cmd_per_lun		= ATA_SHT_CMD_PER_LUN,
	.emulated		= ATA_SHT_EMULATED,
	.use_clustering		= ATA_SHT_USE_CLUSTERING,
	.proc_name		= DRV_NAME,
	.dma_boundary		= ATA_DMA_BOUNDARY,
	.slave_configure	= ata_scsi_slave_config,
	.bios_param		= ata_std_bios_param,
};

static struct ata_port_operations sis_ops = {
	.port_disable		= ata_port_disable,
	.tf_load		= ata_tf_load_pio,
	.tf_read		= ata_tf_read_pio,
	.check_status		= ata_check_status_pio,
	.exec_command		= ata_exec_command_pio,
	.phy_reset		= sata_phy_reset,
	.bmdma_setup            = ata_bmdma_setup_pio,
	.bmdma_start            = ata_bmdma_start_pio,
	.qc_prep		= ata_qc_prep,
	.qc_issue		= ata_qc_issue_prot,
	.eng_timeout		= ata_eng_timeout,
	.irq_handler		= ata_interrupt,
	.irq_clear		= ata_bmdma_irq_clear,
	.scr_read		= sis_scr_read,
	.scr_write		= sis_scr_write,
	.port_start		= ata_port_start,
	.port_stop		= ata_port_stop,
};


MODULE_AUTHOR("Uwe Koziolek");
MODULE_DESCRIPTION("low-level driver for Silicon Integratad Systems SATA controller");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, sis_pci_tbl);

static unsigned int get_scr_cfg_addr(unsigned int port_no, unsigned int sc_reg)
{
	unsigned int addr = SIS_SCR_BASE + (4 * sc_reg);

	if (port_no)
		addr += SIS_SATA1_OFS;
	return addr;
}

static u32 sis_scr_cfg_read (struct ata_port *ap, unsigned int sc_reg)
{
	unsigned int cfg_addr = get_scr_cfg_addr(ap->port_no, sc_reg);
	u32 val;

	if (sc_reg == SCR_ERROR) /* doesn't exist in PCI cfg space */
		return 0xffffffff;
	pci_read_config_dword(ap->host_set->pdev, cfg_addr, &val);
	return val;
}

static void sis_scr_cfg_write (struct ata_port *ap, unsigned int scr, u32 val)
{
	unsigned int cfg_addr = get_scr_cfg_addr(ap->port_no, scr);

	if (scr == SCR_ERROR) /* doesn't exist in PCI cfg space */
		return;
	pci_write_config_dword(ap->host_set->pdev, cfg_addr, val);
}

static u32 sis_scr_read (struct ata_port *ap, unsigned int sc_reg)
{
	if (sc_reg > SCR_CONTROL)
		return 0xffffffffU;

	if (ap->flags & SIS_FLAG_CFGSCR)
		return sis_scr_cfg_read(ap, sc_reg);
	return inl(ap->ioaddr.scr_addr + (sc_reg * 4));
}

static void sis_scr_write (struct ata_port *ap, unsigned int sc_reg, u32 val)
{
	if (sc_reg > SCR_CONTROL)
		return;

	if (ap->flags & SIS_FLAG_CFGSCR)
		sis_scr_cfg_write(ap, sc_reg, val);
	else
		outl(val, ap->ioaddr.scr_addr + (sc_reg * 4));
}

/* move to PCI layer, integrate w/ MSI stuff */
static void pci_enable_intx(struct pci_dev *pdev)
{
	u16 pci_command;

	pci_read_config_word(pdev, PCI_COMMAND, &pci_command);
	if (pci_command & PCI_COMMAND_INTX_DISABLE) {
		pci_command &= ~PCI_COMMAND_INTX_DISABLE;
		pci_write_config_word(pdev, PCI_COMMAND, pci_command);
	}
}

static int sis_init_one (struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct ata_probe_ent *probe_ent = NULL;
	int rc;
	u32 genctl;

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
	if (!probe_ent) {
		rc = -ENOMEM;
		goto err_out_regions;
	}

	memset(probe_ent, 0, sizeof(*probe_ent));
	probe_ent->pdev = pdev;
	INIT_LIST_HEAD(&probe_ent->node);

	probe_ent->sht = &sis_sht;
	probe_ent->host_flags = ATA_FLAG_SATA | ATA_FLAG_SATA_RESET |
				ATA_FLAG_NO_LEGACY;

	/* check and see if the SCRs are in IO space or PCI cfg space */
	pci_read_config_dword(pdev, SIS_GENCTL, &genctl);
	if ((genctl & GENCTL_IOMAPPED_SCR) == 0)
		probe_ent->host_flags |= SIS_FLAG_CFGSCR;
	
	/* if hardware thinks SCRs are in IO space, but there are
	 * no IO resources assigned, change to PCI cfg space.
	 */
	if ((!(probe_ent->host_flags & SIS_FLAG_CFGSCR)) &&
	    ((pci_resource_start(pdev, SIS_SCR_PCI_BAR) == 0) ||
	     (pci_resource_len(pdev, SIS_SCR_PCI_BAR) < 128))) {
		genctl &= ~GENCTL_IOMAPPED_SCR;
		pci_write_config_dword(pdev, SIS_GENCTL, genctl);
		probe_ent->host_flags |= SIS_FLAG_CFGSCR;
	}

	probe_ent->pio_mask = 0x03;
	probe_ent->udma_mask = 0x7f;
	probe_ent->port_ops = &sis_ops;

	probe_ent->port[0].cmd_addr = pci_resource_start(pdev, 0);
	ata_std_ports(&probe_ent->port[0]);
	probe_ent->port[0].ctl_addr =
		pci_resource_start(pdev, 1) | ATA_PCI_CTL_OFS;
	probe_ent->port[0].bmdma_addr = pci_resource_start(pdev, 4);
	if (!(probe_ent->host_flags & SIS_FLAG_CFGSCR))
		probe_ent->port[0].scr_addr =
			pci_resource_start(pdev, SIS_SCR_PCI_BAR);

	probe_ent->port[1].cmd_addr = pci_resource_start(pdev, 2);
	ata_std_ports(&probe_ent->port[1]);
	probe_ent->port[1].ctl_addr =
		pci_resource_start(pdev, 3) | ATA_PCI_CTL_OFS;
	probe_ent->port[1].bmdma_addr = pci_resource_start(pdev, 4) + 8;
	if (!(probe_ent->host_flags & SIS_FLAG_CFGSCR))
		probe_ent->port[1].scr_addr =
			pci_resource_start(pdev, SIS_SCR_PCI_BAR) + 64;

	probe_ent->n_ports = 2;
	probe_ent->irq = pdev->irq;
	probe_ent->irq_flags = SA_SHIRQ;

	pci_set_master(pdev);
	pci_enable_intx(pdev);

	/* FIXME: check ata_device_add return value */
	ata_device_add(probe_ent);
	kfree(probe_ent);

	return 0;

err_out_regions:
	pci_release_regions(pdev);

err_out:
	pci_disable_device(pdev);
	return rc;

}

static int __init sis_init(void)
{
	return pci_module_init(&sis_pci_driver);
}

static void __exit sis_exit(void)
{
	pci_unregister_driver(&sis_pci_driver);
}


module_init(sis_init);
module_exit(sis_exit);
