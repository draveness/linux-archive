/* sun_esp.c: ESP front-end for Sparc SBUS systems.
 *
 * Copyright (C) 2007 David S. Miller (davem@davemloft.net)
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/init.h>

#include <asm/irq.h>
#include <asm/io.h>
#include <asm/dma.h>

#include <asm/sbus.h>

#include <scsi/scsi_host.h>

#include "esp_scsi.h"

#define DRV_MODULE_NAME		"sun_esp"
#define PFX DRV_MODULE_NAME	": "
#define DRV_VERSION		"1.000"
#define DRV_MODULE_RELDATE	"April 19, 2007"

#define dma_read32(REG) \
	sbus_readl(esp->dma_regs + (REG))
#define dma_write32(VAL, REG) \
	sbus_writel((VAL), esp->dma_regs + (REG))

static int __devinit esp_sbus_find_dma(struct esp *esp, struct sbus_dev *dma_sdev)
{
	struct sbus_dev *sdev = esp->dev;
	struct sbus_dma *dma;

	if (dma_sdev != NULL) {
		for_each_dvma(dma) {
			if (dma->sdev == dma_sdev)
				break;
		}
	} else {
		for_each_dvma(dma) {
			if (dma->sdev == NULL)
				break;

			/* If bus + slot are the same and it has the
			 * correct OBP name, it's ours.
			 */
			if (sdev->bus == dma->sdev->bus &&
			    sdev->slot == dma->sdev->slot &&
			    (!strcmp(dma->sdev->prom_name, "dma") ||
			     !strcmp(dma->sdev->prom_name, "espdma")))
				break;
		}
	}

	if (dma == NULL) {
		printk(KERN_ERR PFX "[%s] Cannot find dma.\n",
		       sdev->ofdev.node->full_name);
		return -ENODEV;
	}
	esp->dma = dma;
	esp->dma_regs = dma->regs;

	return 0;

}

static int __devinit esp_sbus_map_regs(struct esp *esp, int hme)
{
	struct sbus_dev *sdev = esp->dev;
	struct resource *res;

	/* On HME, two reg sets exist, first is DVMA,
	 * second is ESP registers.
	 */
	if (hme)
		res = &sdev->resource[1];
	else
		res = &sdev->resource[0];

	esp->regs = sbus_ioremap(res, 0, SBUS_ESP_REG_SIZE, "ESP");
	if (!esp->regs)
		return -ENOMEM;

	return 0;
}

static int __devinit esp_sbus_map_command_block(struct esp *esp)
{
	struct sbus_dev *sdev = esp->dev;

	esp->command_block = sbus_alloc_consistent(sdev, 16,
						   &esp->command_block_dma);
	if (!esp->command_block)
		return -ENOMEM;
	return 0;
}

static int __devinit esp_sbus_register_irq(struct esp *esp)
{
	struct Scsi_Host *host = esp->host;
	struct sbus_dev *sdev = esp->dev;

	host->irq = sdev->irqs[0];
	return request_irq(host->irq, scsi_esp_intr, IRQF_SHARED, "ESP", esp);
}

static void __devinit esp_get_scsi_id(struct esp *esp)
{
	struct sbus_dev *sdev = esp->dev;
	struct device_node *dp = sdev->ofdev.node;

	esp->scsi_id = of_getintprop_default(dp, "initiator-id", 0xff);
	if (esp->scsi_id != 0xff)
		goto done;

	esp->scsi_id = of_getintprop_default(dp, "scsi-initiator-id", 0xff);
	if (esp->scsi_id != 0xff)
		goto done;

	if (!sdev->bus) {
		/* SUN4 */
		esp->scsi_id = 7;
		goto done;
	}

	esp->scsi_id = of_getintprop_default(sdev->bus->ofdev.node,
					     "scsi-initiator-id", 7);

done:
	esp->host->this_id = esp->scsi_id;
	esp->scsi_id_mask = (1 << esp->scsi_id);
}

static void __devinit esp_get_differential(struct esp *esp)
{
	struct sbus_dev *sdev = esp->dev;
	struct device_node *dp = sdev->ofdev.node;

	if (of_find_property(dp, "differential", NULL))
		esp->flags |= ESP_FLAG_DIFFERENTIAL;
	else
		esp->flags &= ~ESP_FLAG_DIFFERENTIAL;
}

static void __devinit esp_get_clock_params(struct esp *esp)
{
	struct sbus_dev *sdev = esp->dev;
	struct device_node *dp = sdev->ofdev.node;
	struct device_node *bus_dp;
	int fmhz;

	bus_dp = NULL;
	if (sdev != NULL && sdev->bus != NULL)
		bus_dp = sdev->bus->ofdev.node;

	fmhz = of_getintprop_default(dp, "clock-frequency", 0);
	if (fmhz == 0)
		fmhz = (!bus_dp) ? 0 :
			of_getintprop_default(bus_dp, "clock-frequency", 0);

	esp->cfreq = fmhz;
}

static void __devinit esp_get_bursts(struct esp *esp, struct sbus_dev *dma)
{
	struct sbus_dev *sdev = esp->dev;
	struct device_node *dp = sdev->ofdev.node;
	u8 bursts;

	bursts = of_getintprop_default(dp, "burst-sizes", 0xff);
	if (dma) {
		struct device_node *dma_dp = dma->ofdev.node;
		u8 val = of_getintprop_default(dma_dp, "burst-sizes", 0xff);
		if (val != 0xff)
			bursts &= val;
	}

	if (sdev->bus) {
		u8 val = of_getintprop_default(sdev->bus->ofdev.node,
					       "burst-sizes", 0xff);
		if (val != 0xff)
			bursts &= val;
	}

	if (bursts == 0xff ||
	    (bursts & DMA_BURST16) == 0 ||
	    (bursts & DMA_BURST32) == 0)
		bursts = (DMA_BURST32 - 1);

	esp->bursts = bursts;
}

static void __devinit esp_sbus_get_props(struct esp *esp, struct sbus_dev *espdma)
{
	esp_get_scsi_id(esp);
	esp_get_differential(esp);
	esp_get_clock_params(esp);
	esp_get_bursts(esp, espdma);
}

static void sbus_esp_write8(struct esp *esp, u8 val, unsigned long reg)
{
	sbus_writeb(val, esp->regs + (reg * 4UL));
}

static u8 sbus_esp_read8(struct esp *esp, unsigned long reg)
{
	return sbus_readb(esp->regs + (reg * 4UL));
}

static dma_addr_t sbus_esp_map_single(struct esp *esp, void *buf,
				      size_t sz, int dir)
{
	return sbus_map_single(esp->dev, buf, sz, dir);
}

static int sbus_esp_map_sg(struct esp *esp, struct scatterlist *sg,
				  int num_sg, int dir)
{
	return sbus_map_sg(esp->dev, sg, num_sg, dir);
}

static void sbus_esp_unmap_single(struct esp *esp, dma_addr_t addr,
				  size_t sz, int dir)
{
	sbus_unmap_single(esp->dev, addr, sz, dir);
}

static void sbus_esp_unmap_sg(struct esp *esp, struct scatterlist *sg,
			      int num_sg, int dir)
{
	sbus_unmap_sg(esp->dev, sg, num_sg, dir);
}

static int sbus_esp_irq_pending(struct esp *esp)
{
	if (dma_read32(DMA_CSR) & (DMA_HNDL_INTR | DMA_HNDL_ERROR))
		return 1;
	return 0;
}

static void sbus_esp_reset_dma(struct esp *esp)
{
	int can_do_burst16, can_do_burst32, can_do_burst64;
	int can_do_sbus64, lim;
	u32 val;

	can_do_burst16 = (esp->bursts & DMA_BURST16) != 0;
	can_do_burst32 = (esp->bursts & DMA_BURST32) != 0;
	can_do_burst64 = 0;
	can_do_sbus64 = 0;
	if (sbus_can_dma_64bit(esp->dev))
		can_do_sbus64 = 1;
	if (sbus_can_burst64(esp->sdev))
		can_do_burst64 = (esp->bursts & DMA_BURST64) != 0;

	/* Put the DVMA into a known state. */
	if (esp->dma->revision != dvmahme) {
		val = dma_read32(DMA_CSR);
		dma_write32(val | DMA_RST_SCSI, DMA_CSR);
		dma_write32(val & ~DMA_RST_SCSI, DMA_CSR);
	}
	switch (esp->dma->revision) {
	case dvmahme:
		dma_write32(DMA_RESET_FAS366, DMA_CSR);
		dma_write32(DMA_RST_SCSI, DMA_CSR);

		esp->prev_hme_dmacsr = (DMA_PARITY_OFF | DMA_2CLKS |
					DMA_SCSI_DISAB | DMA_INT_ENAB);

		esp->prev_hme_dmacsr &= ~(DMA_ENABLE | DMA_ST_WRITE |
					  DMA_BRST_SZ);

		if (can_do_burst64)
			esp->prev_hme_dmacsr |= DMA_BRST64;
		else if (can_do_burst32)
			esp->prev_hme_dmacsr |= DMA_BRST32;

		if (can_do_sbus64) {
			esp->prev_hme_dmacsr |= DMA_SCSI_SBUS64;
			sbus_set_sbus64(esp->dev, esp->bursts);
		}

		lim = 1000;
		while (dma_read32(DMA_CSR) & DMA_PEND_READ) {
			if (--lim == 0) {
				printk(KERN_ALERT PFX "esp%d: DMA_PEND_READ "
				       "will not clear!\n",
				       esp->host->unique_id);
				break;
			}
			udelay(1);
		}

		dma_write32(0, DMA_CSR);
		dma_write32(esp->prev_hme_dmacsr, DMA_CSR);

		dma_write32(0, DMA_ADDR);
		break;

	case dvmarev2:
		if (esp->rev != ESP100) {
			val = dma_read32(DMA_CSR);
			dma_write32(val | DMA_3CLKS, DMA_CSR);
		}
		break;

	case dvmarev3:
		val = dma_read32(DMA_CSR);
		val &= ~DMA_3CLKS;
		val |= DMA_2CLKS;
		if (can_do_burst32) {
			val &= ~DMA_BRST_SZ;
			val |= DMA_BRST32;
		}
		dma_write32(val, DMA_CSR);
		break;

	case dvmaesc1:
		val = dma_read32(DMA_CSR);
		val |= DMA_ADD_ENABLE;
		val &= ~DMA_BCNT_ENAB;
		if (!can_do_burst32 && can_do_burst16) {
			val |= DMA_ESC_BURST;
		} else {
			val &= ~(DMA_ESC_BURST);
		}
		dma_write32(val, DMA_CSR);
		break;

	default:
		break;
	}

	/* Enable interrupts.  */
	val = dma_read32(DMA_CSR);
	dma_write32(val | DMA_INT_ENAB, DMA_CSR);
}

static void sbus_esp_dma_drain(struct esp *esp)
{
	u32 csr;
	int lim;

	if (esp->dma->revision == dvmahme)
		return;

	csr = dma_read32(DMA_CSR);
	if (!(csr & DMA_FIFO_ISDRAIN))
		return;

	if (esp->dma->revision != dvmarev3 && esp->dma->revision != dvmaesc1)
		dma_write32(csr | DMA_FIFO_STDRAIN, DMA_CSR);

	lim = 1000;
	while (dma_read32(DMA_CSR) & DMA_FIFO_ISDRAIN) {
		if (--lim == 0) {
			printk(KERN_ALERT PFX "esp%d: DMA will not drain!\n",
			       esp->host->unique_id);
			break;
		}
		udelay(1);
	}
}

static void sbus_esp_dma_invalidate(struct esp *esp)
{
	if (esp->dma->revision == dvmahme) {
		dma_write32(DMA_RST_SCSI, DMA_CSR);

		esp->prev_hme_dmacsr = ((esp->prev_hme_dmacsr |
					 (DMA_PARITY_OFF | DMA_2CLKS |
					  DMA_SCSI_DISAB | DMA_INT_ENAB)) &
					~(DMA_ST_WRITE | DMA_ENABLE));

		dma_write32(0, DMA_CSR);
		dma_write32(esp->prev_hme_dmacsr, DMA_CSR);

		/* This is necessary to avoid having the SCSI channel
		 * engine lock up on us.
		 */
		dma_write32(0, DMA_ADDR);
	} else {
		u32 val;
		int lim;

		lim = 1000;
		while ((val = dma_read32(DMA_CSR)) & DMA_PEND_READ) {
			if (--lim == 0) {
				printk(KERN_ALERT PFX "esp%d: DMA will not "
				       "invalidate!\n", esp->host->unique_id);
				break;
			}
			udelay(1);
		}

		val &= ~(DMA_ENABLE | DMA_ST_WRITE | DMA_BCNT_ENAB);
		val |= DMA_FIFO_INV;
		dma_write32(val, DMA_CSR);
		val &= ~DMA_FIFO_INV;
		dma_write32(val, DMA_CSR);
	}
}

static void sbus_esp_send_dma_cmd(struct esp *esp, u32 addr, u32 esp_count,
				  u32 dma_count, int write, u8 cmd)
{
	u32 csr;

	BUG_ON(!(cmd & ESP_CMD_DMA));

	sbus_esp_write8(esp, (esp_count >> 0) & 0xff, ESP_TCLOW);
	sbus_esp_write8(esp, (esp_count >> 8) & 0xff, ESP_TCMED);
	if (esp->rev == FASHME) {
		sbus_esp_write8(esp, (esp_count >> 16) & 0xff, FAS_RLO);
		sbus_esp_write8(esp, 0, FAS_RHI);

		scsi_esp_cmd(esp, cmd);

		csr = esp->prev_hme_dmacsr;
		csr |= DMA_SCSI_DISAB | DMA_ENABLE;
		if (write)
			csr |= DMA_ST_WRITE;
		else
			csr &= ~DMA_ST_WRITE;
		esp->prev_hme_dmacsr = csr;

		dma_write32(dma_count, DMA_COUNT);
		dma_write32(addr, DMA_ADDR);
		dma_write32(csr, DMA_CSR);
	} else {
		csr = dma_read32(DMA_CSR);
		csr |= DMA_ENABLE;
		if (write)
			csr |= DMA_ST_WRITE;
		else
			csr &= ~DMA_ST_WRITE;
		dma_write32(csr, DMA_CSR);
		if (esp->dma->revision == dvmaesc1) {
			u32 end = PAGE_ALIGN(addr + dma_count + 16U);
			dma_write32(end - addr, DMA_COUNT);
		}
		dma_write32(addr, DMA_ADDR);

		scsi_esp_cmd(esp, cmd);
	}

}

static int sbus_esp_dma_error(struct esp *esp)
{
	u32 csr = dma_read32(DMA_CSR);

	if (csr & DMA_HNDL_ERROR)
		return 1;

	return 0;
}

static const struct esp_driver_ops sbus_esp_ops = {
	.esp_write8	=	sbus_esp_write8,
	.esp_read8	=	sbus_esp_read8,
	.map_single	=	sbus_esp_map_single,
	.map_sg		=	sbus_esp_map_sg,
	.unmap_single	=	sbus_esp_unmap_single,
	.unmap_sg	=	sbus_esp_unmap_sg,
	.irq_pending	=	sbus_esp_irq_pending,
	.reset_dma	=	sbus_esp_reset_dma,
	.dma_drain	=	sbus_esp_dma_drain,
	.dma_invalidate	=	sbus_esp_dma_invalidate,
	.send_dma_cmd	=	sbus_esp_send_dma_cmd,
	.dma_error	=	sbus_esp_dma_error,
};

static int __devinit esp_sbus_probe_one(struct device *dev,
					struct sbus_dev *esp_dev,
					struct sbus_dev *espdma,
					struct sbus_bus *sbus,
					int hme)
{
	struct scsi_host_template *tpnt = &scsi_esp_template;
	struct Scsi_Host *host;
	struct esp *esp;
	int err;

	host = scsi_host_alloc(tpnt, sizeof(struct esp));

	err = -ENOMEM;
	if (!host)
		goto fail;

	host->max_id = (hme ? 16 : 8);
	esp = shost_priv(host);

	esp->host = host;
	esp->dev = esp_dev;
	esp->ops = &sbus_esp_ops;

	if (hme)
		esp->flags |= ESP_FLAG_WIDE_CAPABLE;

	err = esp_sbus_find_dma(esp, espdma);
	if (err < 0)
		goto fail_unlink;

	err = esp_sbus_map_regs(esp, hme);
	if (err < 0)
		goto fail_unlink;

	err = esp_sbus_map_command_block(esp);
	if (err < 0)
		goto fail_unmap_regs;

	err = esp_sbus_register_irq(esp);
	if (err < 0)
		goto fail_unmap_command_block;

	esp_sbus_get_props(esp, espdma);

	/* Before we try to touch the ESP chip, ESC1 dma can
	 * come up with the reset bit set, so make sure that
	 * is clear first.
	 */
	if (esp->dma->revision == dvmaesc1) {
		u32 val = dma_read32(DMA_CSR);

		dma_write32(val & ~DMA_RST_SCSI, DMA_CSR);
	}

	dev_set_drvdata(&esp_dev->ofdev.dev, esp);

	err = scsi_esp_register(esp, dev);
	if (err)
		goto fail_free_irq;

	return 0;

fail_free_irq:
	free_irq(host->irq, esp);
fail_unmap_command_block:
	sbus_free_consistent(esp->dev, 16,
			     esp->command_block,
			     esp->command_block_dma);
fail_unmap_regs:
	sbus_iounmap(esp->regs, SBUS_ESP_REG_SIZE);
fail_unlink:
	scsi_host_put(host);
fail:
	return err;
}

static int __devinit esp_sbus_probe(struct of_device *dev, const struct of_device_id *match)
{
	struct sbus_dev *sdev = to_sbus_device(&dev->dev);
	struct device_node *dp = dev->node;
	struct sbus_dev *dma_sdev = NULL;
	int hme = 0;

	if (dp->parent &&
	    (!strcmp(dp->parent->name, "espdma") ||
	     !strcmp(dp->parent->name, "dma")))
		dma_sdev = sdev->parent;
	else if (!strcmp(dp->name, "SUNW,fas")) {
		dma_sdev = sdev;
		hme = 1;
	}

	return esp_sbus_probe_one(&dev->dev, sdev, dma_sdev,
				  sdev->bus, hme);
}

static int __devexit esp_sbus_remove(struct of_device *dev)
{
	struct esp *esp = dev_get_drvdata(&dev->dev);
	unsigned int irq = esp->host->irq;
	u32 val;

	scsi_esp_unregister(esp);

	/* Disable interrupts.  */
	val = dma_read32(DMA_CSR);
	dma_write32(val & ~DMA_INT_ENAB, DMA_CSR);

	free_irq(irq, esp);
	sbus_free_consistent(esp->dev, 16,
			     esp->command_block,
			     esp->command_block_dma);
	sbus_iounmap(esp->regs, SBUS_ESP_REG_SIZE);

	scsi_host_put(esp->host);

	return 0;
}

static struct of_device_id esp_match[] = {
	{
		.name = "SUNW,esp",
	},
	{
		.name = "SUNW,fas",
	},
	{
		.name = "esp",
	},
	{},
};
MODULE_DEVICE_TABLE(of, esp_match);

static struct of_platform_driver esp_sbus_driver = {
	.name		= "esp",
	.match_table	= esp_match,
	.probe		= esp_sbus_probe,
	.remove		= __devexit_p(esp_sbus_remove),
};

static int __init sunesp_init(void)
{
	return of_register_driver(&esp_sbus_driver, &sbus_bus_type);
}

static void __exit sunesp_exit(void)
{
	of_unregister_driver(&esp_sbus_driver);
}

MODULE_DESCRIPTION("Sun ESP SCSI driver");
MODULE_AUTHOR("David S. Miller (davem@davemloft.net)");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);

module_init(sunesp_init);
module_exit(sunesp_exit);
