#include <linux/types.h>
#include <linux/mm.h>
#include <linux/blk.h>
#include <linux/sched.h>
#include <linux/version.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/spinlock.h>

#include <asm/setup.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/amigaints.h>
#include <asm/amigahw.h>
#include <asm/irq.h>

#include "scsi.h"
#include "hosts.h"
#include "wd33c93.h"
#include "a3000.h"

#include<linux/stat.h>

#define DMA(ptr) ((a3000_scsiregs *)((ptr)->base))
#define HDATA(ptr) ((struct WD33C93_hostdata *)((ptr)->hostdata))

static struct Scsi_Host *a3000_host = NULL;

static void a3000_intr (int irq, void *dummy, struct pt_regs *fp)
{
	unsigned long flags;
	unsigned int status = DMA(a3000_host)->ISTR;

	if (!(status & ISTR_INT_P))
		return;
	if (status & ISTR_INTS)
	{
		spin_lock_irqsave(&io_request_lock, flags);
		wd33c93_intr (a3000_host);
		spin_unlock_irqrestore(&io_request_lock, flags);
	} else
		printk("Non-serviced A3000 SCSI-interrupt? ISTR = %02x\n",
		       status);
}

static int dma_setup (Scsi_Cmnd *cmd, int dir_in)
{
    unsigned short cntr = CNTR_PDMD | CNTR_INTEN;
    unsigned long addr = virt_to_bus(cmd->SCp.ptr);

    /*
     * if the physical address has the wrong alignment, or if
     * physical address is bad, or if it is a write and at the
     * end of a physical memory chunk, then allocate a bounce
     * buffer
     */
    if (addr & A3000_XFER_MASK ||
	(!dir_in && mm_end_of_chunk (addr, cmd->SCp.this_residual)))
    {
	HDATA(a3000_host)->dma_bounce_len = (cmd->SCp.this_residual + 511)
	    & ~0x1ff;
	HDATA(a3000_host)->dma_bounce_buffer =
	    scsi_malloc (HDATA(a3000_host)->dma_bounce_len);
	
	/* can't allocate memory; use PIO */
	if (!HDATA(a3000_host)->dma_bounce_buffer) {
	    HDATA(a3000_host)->dma_bounce_len = 0;
	    return 1;
	}

	if (!dir_in) {
	    /* copy to bounce buffer for a write */
	    if (cmd->use_sg) {
		memcpy (HDATA(a3000_host)->dma_bounce_buffer,
			cmd->SCp.ptr, cmd->SCp.this_residual);
	    } else
		memcpy (HDATA(a3000_host)->dma_bounce_buffer,
			cmd->request_buffer, cmd->request_bufflen);
	}

	addr = virt_to_bus(HDATA(a3000_host)->dma_bounce_buffer);
    }

    /* setup dma direction */
    if (!dir_in)
	cntr |= CNTR_DDIR;

    /* remember direction */
    HDATA(a3000_host)->dma_dir = dir_in;

    DMA(a3000_host)->CNTR = cntr;

    /* setup DMA *physical* address */
    DMA(a3000_host)->ACR = addr;

    if (dir_in)
  	/* invalidate any cache */
	cache_clear (addr, cmd->SCp.this_residual);
    else
	/* push any dirty cache */
	cache_push (addr, cmd->SCp.this_residual);

    /* start DMA */
    mb();			/* make sure setup is completed */
    DMA(a3000_host)->ST_DMA = 1;
    mb();			/* make sure DMA has started before next IO */

    /* return success */
    return 0;
}

static void dma_stop (struct Scsi_Host *instance, Scsi_Cmnd *SCpnt,
		      int status)
{
    /* disable SCSI interrupts */
    unsigned short cntr = CNTR_PDMD;

    if (!HDATA(instance)->dma_dir)
	cntr |= CNTR_DDIR;

    DMA(instance)->CNTR = cntr;
    mb();			/* make sure CNTR is updated before next IO */

    /* flush if we were reading */
    if (HDATA(instance)->dma_dir) {
	DMA(instance)->FLUSH = 1;
	mb();			/* don't allow prefetch */
	while (!(DMA(instance)->ISTR & ISTR_FE_FLG))
	    barrier();
	mb();			/* no IO until FLUSH is done */
    }

    /* clear a possible interrupt */
    /* I think that this CINT is only necessary if you are
     * using the terminal count features.   HM 7 Mar 1994
     */
    DMA(instance)->CINT = 1;

    /* stop DMA */
    DMA(instance)->SP_DMA = 1;
    mb();			/* make sure DMA is stopped before next IO */

    /* restore the CONTROL bits (minus the direction flag) */
    DMA(instance)->CNTR = CNTR_PDMD | CNTR_INTEN;
    mb();			/* make sure CNTR is updated before next IO */

    /* copy from a bounce buffer, if necessary */
    if (status && HDATA(instance)->dma_bounce_buffer) {
	if (SCpnt && SCpnt->use_sg) {
	    if (HDATA(instance)->dma_dir && SCpnt)
		memcpy (SCpnt->SCp.ptr,
			HDATA(instance)->dma_bounce_buffer,
			SCpnt->SCp.this_residual);
	    scsi_free (HDATA(instance)->dma_bounce_buffer,
		       HDATA(instance)->dma_bounce_len);
	    HDATA(instance)->dma_bounce_buffer = NULL;
	    HDATA(instance)->dma_bounce_len = 0;
	} else {
	    if (HDATA(instance)->dma_dir && SCpnt)
		memcpy (SCpnt->request_buffer,
			HDATA(instance)->dma_bounce_buffer,
			SCpnt->request_bufflen);

	    scsi_free (HDATA(instance)->dma_bounce_buffer,
		       HDATA(instance)->dma_bounce_len);
	    HDATA(instance)->dma_bounce_buffer = NULL;
	    HDATA(instance)->dma_bounce_len = 0;
	}
    }
}

int __init a3000_detect(Scsi_Host_Template *tpnt)
{
    static unsigned char called = 0;

    if (called)
	return 0;

    if  (!MACH_IS_AMIGA || !AMIGAHW_PRESENT(A3000_SCSI))
	return 0;
    if (!request_mem_region(0xDD0000, 256, "wd33c93"))
	return -EBUSY;

    tpnt->proc_name = "A3000";
    tpnt->proc_info = &wd33c93_proc_info;

    a3000_host = scsi_register (tpnt, sizeof(struct WD33C93_hostdata));
    if (a3000_host == NULL) {
	release_mem_region(0xDD0000, 256);
    	return 0;
    }
    a3000_host->base = ZTWO_VADDR(0xDD0000);
    a3000_host->irq = IRQ_AMIGA_PORTS;
    DMA(a3000_host)->DAWR = DAWR_A3000;
    wd33c93_init(a3000_host, (wd33c93_regs *)&(DMA(a3000_host)->SASR),
		 dma_setup, dma_stop, WD33C93_FS_12_15);
    request_irq(IRQ_AMIGA_PORTS, a3000_intr, SA_SHIRQ, "A3000 SCSI",
		a3000_intr);
    DMA(a3000_host)->CNTR = CNTR_PDMD | CNTR_INTEN;
    called = 1;

    return 1;
}

#define HOSTS_C

static Scsi_Host_Template driver_template = _A3000_SCSI;

#include "scsi_module.c"

int a3000_release(struct Scsi_Host *instance)
{
#ifdef MODULE
    wd33c93_release();
    DMA(instance)->CNTR = 0;
    release_mem_region(0xDD0000, 256);
    free_irq(IRQ_AMIGA_PORTS, a3000_intr);
#endif
    return 1;
}
