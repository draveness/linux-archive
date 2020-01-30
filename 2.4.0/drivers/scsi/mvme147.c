#include <linux/types.h>
#include <linux/mm.h>
#include <linux/blk.h>
#include <linux/sched.h>
#include <linux/version.h>

#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/mvme147hw.h>
#include <asm/irq.h>

#include "scsi.h"
#include "hosts.h"
#include "wd33c93.h"
#include "mvme147.h"

#include<linux/stat.h>

#define HDATA(ptr) ((struct WD33C93_hostdata *)((ptr)->hostdata))

static struct Scsi_Host *mvme147_host = NULL;

static void mvme147_intr (int irq, void *dummy, struct pt_regs *fp)
{
    if (irq == MVME147_IRQ_SCSI_PORT)
	wd33c93_intr (mvme147_host);
    else
	m147_pcc->dma_intr = 0x89;	/* Ack and enable ints */
}

static int dma_setup (Scsi_Cmnd *cmd, int dir_in)
{
    unsigned char flags = 0x01;
    unsigned long addr = virt_to_bus(cmd->SCp.ptr);

    /* setup dma direction */
    if (!dir_in)
	flags |= 0x04;

    /* remember direction */
    HDATA(mvme147_host)->dma_dir = dir_in;

    if (dir_in)
  	/* invalidate any cache */
	cache_clear (addr, cmd->SCp.this_residual);
    else
	/* push any dirty cache */
	cache_push (addr, cmd->SCp.this_residual);

    /* start DMA */
    m147_pcc->dma_bcr   = cmd->SCp.this_residual | (1<<24);
    m147_pcc->dma_dadr  = addr;
    m147_pcc->dma_cntrl = flags;

    /* return success */
    return 0;
}

static void dma_stop (struct Scsi_Host *instance, Scsi_Cmnd *SCpnt,
		      int status)
{
    m147_pcc->dma_cntrl = 0;
}

int mvme147_detect(Scsi_Host_Template *tpnt)
{
    static unsigned char called = 0;

    if (!MACH_IS_MVME147 || called)
	return 0;
    called++;

    tpnt->proc_name = "MVME147";
    tpnt->proc_info = &wd33c93_proc_info;

    mvme147_host = scsi_register (tpnt, sizeof(struct WD33C93_hostdata));
    mvme147_host->base = 0xfffe4000;
    mvme147_host->irq = MVME147_IRQ_SCSI_PORT;
    wd33c93_init(mvme147_host, (wd33c93_regs *)0xfffe4000,
		 dma_setup, dma_stop, WD33C93_FS_8_10);

    request_irq(MVME147_IRQ_SCSI_PORT, mvme147_intr, 0, "MVME147 SCSI PORT", mvme147_intr);
    request_irq(MVME147_IRQ_SCSI_DMA, mvme147_intr, 0, "MVME147 SCSI DMA", mvme147_intr);
#if 0	/* Disabled; causes problems booting */
    m147_pcc->scsi_interrupt = 0x10;	/* Assert SCSI bus reset */
    udelay(100);
    m147_pcc->scsi_interrupt = 0x00;	/* Negate SCSI bus reset */
    udelay(2000);
    m147_pcc->scsi_interrupt = 0x40;	/* Clear bus reset interrupt */
#endif
    m147_pcc->scsi_interrupt = 0x09;	/* Enable interrupt */

    m147_pcc->dma_cntrl = 0x00;		/* ensure DMA is stopped */
    m147_pcc->dma_intr = 0x89;		/* Ack and enable ints */

    return 1;
}

#define HOSTS_C

#include "mvme147.h"

static Scsi_Host_Template driver_template = MVME147_SCSI;

#include "scsi_module.c"

int mvme147_release(struct Scsi_Host *instance)
{
#ifdef MODULE
    /* XXX Make sure DMA is stopped! */
    wd33c93_release();
    free_irq(MVME147_IRQ_SCSI_PORT, mvme147_intr);
    free_irq(MVME147_IRQ_SCSI_DMA, mvme147_intr);
#endif
    return 1;
}
