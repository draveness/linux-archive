/*
 *  sata_promise.c - Promise SATA
 *
 *  Maintained by:  Jeff Garzik <jgarzik@pobox.com>
 *  		    Please ALWAYS copy linux-ide@vger.kernel.org
 *		    on emails.
 *
 *  Copyright 2003-2004 Red Hat, Inc.
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
#include <linux/sched.h>
#include "scsi.h"
#include <scsi/scsi_host.h>
#include <linux/libata.h>
#include <asm/io.h>
#include "sata_promise.h"

#define DRV_NAME	"sata_promise"
#define DRV_VERSION	"1.00"


enum {
	PDC_PKT_SUBMIT		= 0x40, /* Command packet pointer addr */
	PDC_INT_SEQMASK		= 0x40,	/* Mask of asserted SEQ INTs */
	PDC_TBG_MODE		= 0x41,	/* TBG mode */
	PDC_FLASH_CTL		= 0x44, /* Flash control register */
	PDC_PCI_CTL		= 0x48, /* PCI control and status register */
	PDC_GLOBAL_CTL		= 0x48, /* Global control/status (per port) */
	PDC_CTLSTAT		= 0x60,	/* IDE control and status (per port) */
	PDC_SATA_PLUG_CSR	= 0x6C, /* SATA Plug control/status reg */
	PDC_SLEW_CTL		= 0x470, /* slew rate control reg */

	PDC_ERR_MASK		= (1<<19) | (1<<20) | (1<<21) | (1<<22) |
				  (1<<8) | (1<<9) | (1<<10),

	board_2037x		= 0,	/* FastTrak S150 TX2plus */
	board_20319		= 1,	/* FastTrak S150 TX4 */

	PDC_HAS_PATA		= (1 << 1), /* PDC20375 has PATA */

	PDC_RESET		= (1 << 11), /* HDMA reset */
};


struct pdc_port_priv {
	u8			*pkt;
	dma_addr_t		pkt_dma;
};

static u32 pdc_sata_scr_read (struct ata_port *ap, unsigned int sc_reg);
static void pdc_sata_scr_write (struct ata_port *ap, unsigned int sc_reg, u32 val);
static int pdc_sata_init_one (struct pci_dev *pdev, const struct pci_device_id *ent);
static void pdc_dma_start(struct ata_queued_cmd *qc);
static irqreturn_t pdc_interrupt (int irq, void *dev_instance, struct pt_regs *regs);
static void pdc_eng_timeout(struct ata_port *ap);
static int pdc_port_start(struct ata_port *ap);
static void pdc_port_stop(struct ata_port *ap);
static void pdc_phy_reset(struct ata_port *ap);
static void pdc_qc_prep(struct ata_queued_cmd *qc);
static void pdc_tf_load_mmio(struct ata_port *ap, struct ata_taskfile *tf);
static void pdc_exec_command_mmio(struct ata_port *ap, struct ata_taskfile *tf);
static inline void pdc_dma_complete (struct ata_port *ap,
                                     struct ata_queued_cmd *qc, int have_err);
static void pdc_irq_clear(struct ata_port *ap);
static int pdc_qc_issue_prot(struct ata_queued_cmd *qc);

static Scsi_Host_Template pdc_sata_sht = {
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

static struct ata_port_operations pdc_sata_ops = {
	.port_disable		= ata_port_disable,
	.tf_load		= pdc_tf_load_mmio,
	.tf_read		= ata_tf_read_mmio,
	.check_status		= ata_check_status_mmio,
	.exec_command		= pdc_exec_command_mmio,
	.phy_reset		= pdc_phy_reset,
	.qc_prep		= pdc_qc_prep,
	.qc_issue		= pdc_qc_issue_prot,
	.eng_timeout		= pdc_eng_timeout,
	.irq_handler		= pdc_interrupt,
	.irq_clear		= pdc_irq_clear,
	.scr_read		= pdc_sata_scr_read,
	.scr_write		= pdc_sata_scr_write,
	.port_start		= pdc_port_start,
	.port_stop		= pdc_port_stop,
};

static struct ata_port_info pdc_port_info[] = {
	/* board_2037x */
	{
		.sht		= &pdc_sata_sht,
		.host_flags	= ATA_FLAG_SATA | ATA_FLAG_NO_LEGACY |
				  ATA_FLAG_SRST | ATA_FLAG_MMIO,
		.pio_mask	= 0x03, /* pio3-4 */
		.udma_mask	= 0x7f, /* udma0-6 ; FIXME */
		.port_ops	= &pdc_sata_ops,
	},

	/* board_20319 */
	{
		.sht		= &pdc_sata_sht,
		.host_flags	= ATA_FLAG_SATA | ATA_FLAG_NO_LEGACY |
				  ATA_FLAG_SRST | ATA_FLAG_MMIO,
		.pio_mask	= 0x03, /* pio3-4 */
		.udma_mask	= 0x7f, /* udma0-6 ; FIXME */
		.port_ops	= &pdc_sata_ops,
	},
};

static struct pci_device_id pdc_sata_pci_tbl[] = {
	{ PCI_VENDOR_ID_PROMISE, 0x3371, PCI_ANY_ID, PCI_ANY_ID, 0, 0,
	  board_2037x },
	{ PCI_VENDOR_ID_PROMISE, 0x3373, PCI_ANY_ID, PCI_ANY_ID, 0, 0,
	  board_2037x },
	{ PCI_VENDOR_ID_PROMISE, 0x3375, PCI_ANY_ID, PCI_ANY_ID, 0, 0,
	  board_2037x },
	{ PCI_VENDOR_ID_PROMISE, 0x3376, PCI_ANY_ID, PCI_ANY_ID, 0, 0,
	  board_2037x },
	{ PCI_VENDOR_ID_PROMISE, 0x3318, PCI_ANY_ID, PCI_ANY_ID, 0, 0,
	  board_20319 },
	{ PCI_VENDOR_ID_PROMISE, 0x3319, PCI_ANY_ID, PCI_ANY_ID, 0, 0,
	  board_20319 },
	{ }	/* terminate list */
};


static struct pci_driver pdc_sata_pci_driver = {
	.name			= DRV_NAME,
	.id_table		= pdc_sata_pci_tbl,
	.probe			= pdc_sata_init_one,
	.remove			= ata_pci_remove_one,
};


static int pdc_port_start(struct ata_port *ap)
{
	struct pci_dev *pdev = ap->host_set->pdev;
	struct pdc_port_priv *pp;
	int rc;

	rc = ata_port_start(ap);
	if (rc)
		return rc;

	pp = kmalloc(sizeof(*pp), GFP_KERNEL);
	if (!pp) {
		rc = -ENOMEM;
		goto err_out;
	}
	memset(pp, 0, sizeof(*pp));

	pp->pkt = pci_alloc_consistent(pdev, 128, &pp->pkt_dma);
	if (!pp->pkt) {
		rc = -ENOMEM;
		goto err_out_kfree;
	}

	ap->private_data = pp;

	return 0;

err_out_kfree:
	kfree(pp);
err_out:
	ata_port_stop(ap);
	return rc;
}


static void pdc_port_stop(struct ata_port *ap)
{
	struct pci_dev *pdev = ap->host_set->pdev;
	struct pdc_port_priv *pp = ap->private_data;

	ap->private_data = NULL;
	pci_free_consistent(pdev, 128, pp->pkt, pp->pkt_dma);
	kfree(pp);
	ata_port_stop(ap);
}


static void pdc_reset_port(struct ata_port *ap)
{
	void *mmio = (void *) ap->ioaddr.cmd_addr + PDC_CTLSTAT;
	unsigned int i;
	u32 tmp;

	for (i = 11; i > 0; i--) {
		tmp = readl(mmio);
		if (tmp & PDC_RESET)
			break;

		udelay(100);

		tmp |= PDC_RESET;
		writel(tmp, mmio);
	}

	tmp &= ~PDC_RESET;
	writel(tmp, mmio);
	readl(mmio);	/* flush */
}

static void pdc_phy_reset(struct ata_port *ap)
{
	pdc_reset_port(ap);
	sata_phy_reset(ap);
}

static u32 pdc_sata_scr_read (struct ata_port *ap, unsigned int sc_reg)
{
	if (sc_reg > SCR_CONTROL)
		return 0xffffffffU;
	return readl((void *) ap->ioaddr.scr_addr + (sc_reg * 4));
}


static void pdc_sata_scr_write (struct ata_port *ap, unsigned int sc_reg,
			       u32 val)
{
	if (sc_reg > SCR_CONTROL)
		return;
	writel(val, (void *) ap->ioaddr.scr_addr + (sc_reg * 4));
}

static void pdc_qc_prep(struct ata_queued_cmd *qc)
{
	struct pdc_port_priv *pp = qc->ap->private_data;
	unsigned int i;

	VPRINTK("ENTER\n");

	ata_qc_prep(qc);

	i = pdc_pkt_header(&qc->tf, qc->ap->prd_dma,  qc->dev->devno, pp->pkt);

	if (qc->tf.flags & ATA_TFLAG_LBA48)
		i = pdc_prep_lba48(&qc->tf, pp->pkt, i);
	else
		i = pdc_prep_lba28(&qc->tf, pp->pkt, i);

	pdc_pkt_footer(&qc->tf, pp->pkt, i);
}

static inline void pdc_dma_complete (struct ata_port *ap,
                                     struct ata_queued_cmd *qc,
				     int have_err)
{
	u8 err_bit = have_err ? ATA_ERR : 0;

	/* get drive status; clear intr; complete txn */
	ata_qc_complete(qc, ata_wait_idle(ap) | err_bit);
}

static void pdc_eng_timeout(struct ata_port *ap)
{
	u8 drv_stat;
	struct ata_queued_cmd *qc;

	DPRINTK("ENTER\n");

	qc = ata_qc_from_tag(ap, ap->active_tag);
	if (!qc) {
		printk(KERN_ERR "ata%u: BUG: timeout without command\n",
		       ap->id);
		goto out;
	}

	/* hack alert!  We cannot use the supplied completion
	 * function from inside the ->eh_strategy_handler() thread.
	 * libata is the only user of ->eh_strategy_handler() in
	 * any kernel, so the default scsi_done() assumes it is
	 * not being called from the SCSI EH.
	 */
	qc->scsidone = scsi_finish_command;

	switch (qc->tf.protocol) {
	case ATA_PROT_DMA:
		printk(KERN_ERR "ata%u: DMA timeout\n", ap->id);
		ata_qc_complete(qc, ata_wait_idle(ap) | ATA_ERR);
		break;

	case ATA_PROT_NODATA:
		drv_stat = ata_busy_wait(ap, ATA_BUSY | ATA_DRQ, 1000);

		printk(KERN_ERR "ata%u: command 0x%x timeout, stat 0x%x\n",
		       ap->id, qc->tf.command, drv_stat);

		ata_qc_complete(qc, drv_stat);
		break;

	default:
		drv_stat = ata_busy_wait(ap, ATA_BUSY | ATA_DRQ, 1000);

		printk(KERN_ERR "ata%u: unknown timeout, cmd 0x%x stat 0x%x\n",
		       ap->id, qc->tf.command, drv_stat);

		ata_qc_complete(qc, drv_stat);
		break;
	}

out:
	DPRINTK("EXIT\n");
}

static inline unsigned int pdc_host_intr( struct ata_port *ap,
                                          struct ata_queued_cmd *qc)
{
	u8 status;
	unsigned int handled = 0, have_err = 0;
	u32 tmp;
	void *mmio = (void *) ap->ioaddr.cmd_addr + PDC_GLOBAL_CTL;

	tmp = readl(mmio);
	if (tmp & PDC_ERR_MASK) {
		have_err = 1;
		pdc_reset_port(ap);
	}

	switch (qc->tf.protocol) {
	case ATA_PROT_DMA:
		pdc_dma_complete(ap, qc, have_err);
		handled = 1;
		break;

	case ATA_PROT_NODATA:   /* command completion, but no data xfer */
		status = ata_busy_wait(ap, ATA_BUSY | ATA_DRQ, 1000);
		DPRINTK("BUS_NODATA (drv_stat 0x%X)\n", status);
		if (have_err)
			status |= ATA_ERR;
		ata_qc_complete(qc, status);
		handled = 1;
		break;

        default:
                ap->stats.idle_irq++;
                break;
        }

        return handled;
}

static void pdc_irq_clear(struct ata_port *ap)
{
	struct ata_host_set *host_set = ap->host_set;
	void *mmio = host_set->mmio_base;

	readl(mmio + PDC_INT_SEQMASK);
}

static irqreturn_t pdc_interrupt (int irq, void *dev_instance, struct pt_regs *regs)
{
	struct ata_host_set *host_set = dev_instance;
	struct ata_port *ap;
	u32 mask = 0;
	unsigned int i, tmp;
	unsigned int handled = 0;
	void *mmio_base;

	VPRINTK("ENTER\n");

	if (!host_set || !host_set->mmio_base) {
		VPRINTK("QUICK EXIT\n");
		return IRQ_NONE;
	}

	mmio_base = host_set->mmio_base;

	/* reading should also clear interrupts */
	mask = readl(mmio_base + PDC_INT_SEQMASK);

	if (mask == 0xffffffff) {
		VPRINTK("QUICK EXIT 2\n");
		return IRQ_NONE;
	}
	mask &= 0xffff;		/* only 16 tags possible */
	if (!mask) {
		VPRINTK("QUICK EXIT 3\n");
		return IRQ_NONE;
	}

        spin_lock(&host_set->lock);

        for (i = 0; i < host_set->n_ports; i++) {
		VPRINTK("port %u\n", i);
		ap = host_set->ports[i];
		tmp = mask & (1 << (i + 1));
		if (tmp && ap && (!(ap->flags & ATA_FLAG_PORT_DISABLED))) {
			struct ata_queued_cmd *qc;

			qc = ata_qc_from_tag(ap, ap->active_tag);
			if (qc && (!(qc->tf.ctl & ATA_NIEN)))
				handled += pdc_host_intr(ap, qc);
		}
	}

        spin_unlock(&host_set->lock);

	VPRINTK("EXIT\n");

	return IRQ_RETVAL(handled);
}

static inline void pdc_dma_start(struct ata_queued_cmd *qc)
{
	struct ata_port *ap = qc->ap;
	struct pdc_port_priv *pp = ap->private_data;
	unsigned int port_no = ap->port_no;
	u8 seq = (u8) (port_no + 1);

	VPRINTK("ENTER, ap %p\n", ap);

	writel(0x00000001, ap->host_set->mmio_base + (seq * 4));
	readl(ap->host_set->mmio_base + (seq * 4));	/* flush */

	pp->pkt[2] = seq;
	wmb();			/* flush PRD, pkt writes */
	writel(pp->pkt_dma, (void *) ap->ioaddr.cmd_addr + PDC_PKT_SUBMIT);
	readl((void *) ap->ioaddr.cmd_addr + PDC_PKT_SUBMIT); /* flush */
}

static int pdc_qc_issue_prot(struct ata_queued_cmd *qc)
{
	switch (qc->tf.protocol) {
	case ATA_PROT_DMA:
		pdc_dma_start(qc);
		return 0;

	case ATA_PROT_ATAPI_DMA:
		BUG();
		break;

	default:
		break;
	}

	return ata_qc_issue_prot(qc);
}

static void pdc_tf_load_mmio(struct ata_port *ap, struct ata_taskfile *tf)
{
	WARN_ON (tf->protocol == ATA_PROT_DMA);
	ata_tf_load_mmio(ap, tf);
}


static void pdc_exec_command_mmio(struct ata_port *ap, struct ata_taskfile *tf)
{
	WARN_ON (tf->protocol == ATA_PROT_DMA);
	ata_exec_command_mmio(ap, tf);
}


static void pdc_sata_setup_port(struct ata_ioports *port, unsigned long base)
{
	port->cmd_addr		= base;
	port->data_addr		= base;
	port->feature_addr	=
	port->error_addr	= base + 0x4;
	port->nsect_addr	= base + 0x8;
	port->lbal_addr		= base + 0xc;
	port->lbam_addr		= base + 0x10;
	port->lbah_addr		= base + 0x14;
	port->device_addr	= base + 0x18;
	port->command_addr	=
	port->status_addr	= base + 0x1c;
	port->altstatus_addr	=
	port->ctl_addr		= base + 0x38;
}


static void pdc_host_init(unsigned int chip_id, struct ata_probe_ent *pe)
{
	void *mmio = pe->mmio_base;
	u32 tmp;

	/*
	 * Except for the hotplug stuff, this is voodoo from the
	 * Promise driver.  Label this entire section
	 * "TODO: figure out why we do this"
	 */

	/* change FIFO_SHD to 8 dwords, enable BMR_BURST */
	tmp = readl(mmio + PDC_FLASH_CTL);
	tmp |= 0x12000;	/* bit 16 (fifo 8 dw) and 13 (bmr burst?) */
	writel(tmp, mmio + PDC_FLASH_CTL);

	/* clear plug/unplug flags for all ports */
	tmp = readl(mmio + PDC_SATA_PLUG_CSR);
	writel(tmp | 0xff, mmio + PDC_SATA_PLUG_CSR);

	/* mask plug/unplug ints */
	tmp = readl(mmio + PDC_SATA_PLUG_CSR);
	writel(tmp | 0xff0000, mmio + PDC_SATA_PLUG_CSR);

	/* reduce TBG clock to 133 Mhz. */
	tmp = readl(mmio + PDC_TBG_MODE);
	tmp &= ~0x30000; /* clear bit 17, 16*/
	tmp |= 0x10000;  /* set bit 17:16 = 0:1 */
	writel(tmp, mmio + PDC_TBG_MODE);

	readl(mmio + PDC_TBG_MODE);	/* flush */
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(msecs_to_jiffies(10) + 1);

	/* adjust slew rate control register. */
	tmp = readl(mmio + PDC_SLEW_CTL);
	tmp &= 0xFFFFF03F; /* clear bit 11 ~ 6 */
	tmp  |= 0x00000900; /* set bit 11-9 = 100b , bit 8-6 = 100 */
	writel(tmp, mmio + PDC_SLEW_CTL);
}

static int pdc_sata_init_one (struct pci_dev *pdev, const struct pci_device_id *ent)
{
	static int printed_version;
	struct ata_probe_ent *probe_ent = NULL;
	unsigned long base;
	void *mmio_base;
	unsigned int board_idx = (unsigned int) ent->driver_data;
	int rc;

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
	probe_ent->pdev = pdev;
	INIT_LIST_HEAD(&probe_ent->node);

	mmio_base = ioremap(pci_resource_start(pdev, 3),
		            pci_resource_len(pdev, 3));
	if (mmio_base == NULL) {
		rc = -ENOMEM;
		goto err_out_free_ent;
	}
	base = (unsigned long) mmio_base;

	probe_ent->sht		= pdc_port_info[board_idx].sht;
	probe_ent->host_flags	= pdc_port_info[board_idx].host_flags;
	probe_ent->pio_mask	= pdc_port_info[board_idx].pio_mask;
	probe_ent->udma_mask	= pdc_port_info[board_idx].udma_mask;
	probe_ent->port_ops	= pdc_port_info[board_idx].port_ops;

       	probe_ent->irq = pdev->irq;
       	probe_ent->irq_flags = SA_SHIRQ;
	probe_ent->mmio_base = mmio_base;

	pdc_sata_setup_port(&probe_ent->port[0], base + 0x200);
	pdc_sata_setup_port(&probe_ent->port[1], base + 0x280);

	probe_ent->port[0].scr_addr = base + 0x400;
	probe_ent->port[1].scr_addr = base + 0x500;

	/* notice 4-port boards */
	switch (board_idx) {
	case board_20319:
       		probe_ent->n_ports = 4;

		pdc_sata_setup_port(&probe_ent->port[2], base + 0x300);
		pdc_sata_setup_port(&probe_ent->port[3], base + 0x380);

		probe_ent->port[2].scr_addr = base + 0x600;
		probe_ent->port[3].scr_addr = base + 0x700;
		break;
	case board_2037x:
       		probe_ent->n_ports = 2;
		break;
	default:
		BUG();
		break;
	}

	pci_set_master(pdev);

	/* initialize adapter */
	pdc_host_init(board_idx, probe_ent);

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


static int __init pdc_sata_init(void)
{
	return pci_module_init(&pdc_sata_pci_driver);
}


static void __exit pdc_sata_exit(void)
{
	pci_unregister_driver(&pdc_sata_pci_driver);
}


MODULE_AUTHOR("Jeff Garzik");
MODULE_DESCRIPTION("Promise SATA TX2/TX4 low-level driver");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, pdc_sata_pci_tbl);

module_init(pdc_sata_init);
module_exit(pdc_sata_exit);
