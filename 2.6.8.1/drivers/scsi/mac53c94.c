/*
 * SCSI low-level driver for the 53c94 SCSI bus adaptor found
 * on Power Macintosh computers, controlling the external SCSI chain.
 * We assume the 53c94 is connected to a DBDMA (descriptor-based DMA)
 * controller.
 *
 * Paul Mackerras, August 1996.
 * Copyright (C) 1996 Paul Mackerras.
 */
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <asm/dbdma.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/prom.h>
#include <asm/system.h>
#include <asm/pci-bridge.h>
#include <asm/macio.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>

#include "mac53c94.h"

enum fsc_phase {
	idle,
	selecting,
	dataing,
	completing,
	busfreeing,
};

struct fsc_state {
	struct	mac53c94_regs *regs;
	int	intr;
	struct	dbdma_regs *dma;
	int	dmaintr;
	int	clk_freq;
	struct	Scsi_Host *host;
	struct scsi_cmnd *request_q;
	struct scsi_cmnd *request_qtail;
	struct scsi_cmnd *current_req;		/* req we're currently working on */
	enum fsc_phase phase;		/* what we're currently trying to do */
	struct dbdma_cmd *dma_cmds;	/* space for dbdma commands, aligned */
	void	*dma_cmd_space;
	struct	pci_dev *pdev;
	dma_addr_t dma_addr;
	struct macio_dev *mdev;
};

static void mac53c94_init(struct fsc_state *);
static void mac53c94_start(struct fsc_state *);
static void mac53c94_interrupt(int, void *, struct pt_regs *);
static irqreturn_t do_mac53c94_interrupt(int, void *, struct pt_regs *);
static void cmd_done(struct fsc_state *, int result);
static void set_dma_cmds(struct fsc_state *, struct scsi_cmnd *);


static int mac53c94_queue(struct scsi_cmnd *cmd, void (*done)(struct scsi_cmnd *))
{
	struct fsc_state *state;

#if 0
	if (cmd->sc_data_direction == DMA_TO_DEVICE) {
		int i;
		printk(KERN_DEBUG "mac53c94_queue %p: command is", cmd);
		for (i = 0; i < cmd->cmd_len; ++i)
			printk(" %.2x", cmd->cmnd[i]);
		printk("\n" KERN_DEBUG "use_sg=%d request_bufflen=%d request_buffer=%p\n",
		       cmd->use_sg, cmd->request_bufflen, cmd->request_buffer);
	}
#endif

	cmd->scsi_done = done;
	cmd->host_scribble = NULL;

	state = (struct fsc_state *) cmd->device->host->hostdata;

	if (state->request_q == NULL)
		state->request_q = cmd;
	else
		state->request_qtail->host_scribble = (void *) cmd;
	state->request_qtail = cmd;

	if (state->phase == idle)
		mac53c94_start(state);

	return 0;
}

static int mac53c94_abort(struct scsi_cmnd *cmd)
{
	return FAILED;
}

static int mac53c94_host_reset(struct scsi_cmnd *cmd)
{
	struct fsc_state *state = (struct fsc_state *) cmd->device->host->hostdata;
	struct mac53c94_regs *regs = state->regs;
	struct dbdma_regs *dma = state->dma;

	st_le32(&dma->control, (RUN|PAUSE|FLUSH|WAKE) << 16);
	writeb(CMD_SCSI_RESET, &regs->command);	/* assert RST */
	udelay(100);			/* leave it on for a while (>= 25us) */
	writeb(CMD_RESET, &regs->command);
	udelay(20);
	mac53c94_init(state);
	writeb(CMD_NOP, &regs->command);
	return SUCCESS;
}

static void mac53c94_init(struct fsc_state *state)
{
	struct mac53c94_regs *regs = state->regs;
	struct dbdma_regs *dma = state->dma;
	int x;

	writeb(state->host->this_id | CF1_PAR_ENABLE, &regs->config1);
	writeb(TIMO_VAL(250), &regs->sel_timeout);	/* 250ms */
	writeb(CLKF_VAL(state->clk_freq), &regs->clk_factor);
	writeb(CF2_FEATURE_EN, &regs->config2);
	writeb(0, &regs->config3);
	writeb(0, &regs->sync_period);
	writeb(0, &regs->sync_offset);
	x = readb(&regs->interrupt);
	writel((RUN|PAUSE|FLUSH|WAKE) << 16, &dma->control);
}

/*
 * Start the next command for a 53C94.
 * Should be called with interrupts disabled.
 */
static void mac53c94_start(struct fsc_state *state)
{
	struct scsi_cmnd *cmd;
	struct mac53c94_regs *regs = state->regs;
	int i;

	if (state->phase != idle || state->current_req != NULL)
		panic("inappropriate mac53c94_start (state=%p)", state);
	if (state->request_q == NULL)
		return;
	state->current_req = cmd = state->request_q;
	state->request_q = (struct scsi_cmnd *) cmd->host_scribble;

	/* Off we go */
	writeb(0, &regs->count_lo);
	writeb(0, &regs->count_mid);
	writeb(0, &regs->count_hi);
	writeb(CMD_NOP + CMD_DMA_MODE, &regs->command);
	udelay(1);
	writeb(CMD_FLUSH, &regs->command);
	udelay(1);
	writeb(cmd->device->id, &regs->dest_id);
	writeb(0, &regs->sync_period);
	writeb(0, &regs->sync_offset);

	/* load the command into the FIFO */
	for (i = 0; i < cmd->cmd_len; ++i)
		writeb(cmd->cmnd[i], &regs->fifo);

	/* do select without ATN XXX */
	writeb(CMD_SELECT, &regs->command);
	state->phase = selecting;

	if (cmd->use_sg > 0 || cmd->request_bufflen != 0)
		set_dma_cmds(state, cmd);
}

static irqreturn_t do_mac53c94_interrupt(int irq, void *dev_id, struct pt_regs *ptregs)
{
	unsigned long flags;
	struct Scsi_Host *dev = ((struct fsc_state *) dev_id)->current_req->device->host;
	
	spin_lock_irqsave(dev->host_lock, flags);
	mac53c94_interrupt(irq, dev_id, ptregs);
	spin_unlock_irqrestore(dev->host_lock, flags);
	return IRQ_HANDLED;
}

static void mac53c94_interrupt(int irq, void *dev_id, struct pt_regs *ptregs)
{
	struct fsc_state *state = (struct fsc_state *) dev_id;
	struct mac53c94_regs *regs = state->regs;
	struct dbdma_regs *dma = state->dma;
	struct scsi_cmnd *cmd = state->current_req;
	int nb, stat, seq, intr;
	static int mac53c94_errors;

	/*
	 * Apparently, reading the interrupt register unlatches
	 * the status and sequence step registers.
	 */
	seq = readb(&regs->seqstep);
	stat = readb(&regs->status);
	intr = readb(&regs->interrupt);

#if 0
	printk(KERN_DEBUG "mac53c94_intr, intr=%x stat=%x seq=%x phase=%d\n",
	       intr, stat, seq, state->phase);
#endif

	if (intr & INTR_RESET) {
		/* SCSI bus was reset */
		printk(KERN_INFO "external SCSI bus reset detected\n");
		writeb(CMD_NOP, &regs->command);
		writel(RUN << 16, &dma->control);	/* stop dma */
		cmd_done(state, DID_RESET << 16);
		return;
	}
	if (intr & INTR_ILL_CMD) {
		printk(KERN_ERR "53c94: invalid cmd, intr=%x stat=%x seq=%x phase=%d\n",
		       intr, stat, seq, state->phase);
		cmd_done(state, DID_ERROR << 16);
		return;
	}
	if (stat & STAT_ERROR) {
#if 0
		/* XXX these seem to be harmless? */
		printk("53c94: bad error, intr=%x stat=%x seq=%x phase=%d\n",
		       intr, stat, seq, state->phase);
#endif
		++mac53c94_errors;
		writeb(CMD_NOP + CMD_DMA_MODE, &regs->command);
	}
	if (cmd == 0) {
		printk(KERN_DEBUG "53c94: interrupt with no command active?\n");
		return;
	}
	if (stat & STAT_PARITY) {
		printk(KERN_ERR "mac53c94: parity error\n");
		cmd_done(state, DID_PARITY << 16);
		return;
	}
	switch (state->phase) {
	case selecting:
		if (intr & INTR_DISCONNECT) {
			/* selection timed out */
			cmd_done(state, DID_BAD_TARGET << 16);
			return;
		}
		if (intr != INTR_BUS_SERV + INTR_DONE) {
			printk(KERN_DEBUG "got intr %x during selection\n", intr);
			cmd_done(state, DID_ERROR << 16);
			return;
		}
		if ((seq & SS_MASK) != SS_DONE) {
			printk(KERN_DEBUG "seq step %x after command\n", seq);
			cmd_done(state, DID_ERROR << 16);
			return;
		}
		writeb(CMD_NOP, &regs->command);
		/* set DMA controller going if any data to transfer */
		if ((stat & (STAT_MSG|STAT_CD)) == 0
		    && (cmd->use_sg > 0 || cmd->request_bufflen != 0)) {
			nb = cmd->SCp.this_residual;
			if (nb > 0xfff0)
				nb = 0xfff0;
			cmd->SCp.this_residual -= nb;
			writeb(nb, &regs->count_lo);
			writeb(nb >> 8, &regs->count_mid);
			writeb(CMD_DMA_MODE + CMD_NOP, &regs->command);
			writel(virt_to_phys(state->dma_cmds), &dma->cmdptr);
			writel((RUN << 16) | RUN, &dma->control);
			writeb(CMD_DMA_MODE + CMD_XFER_DATA, &regs->command);
			state->phase = dataing;
			break;
		} else if ((stat & STAT_PHASE) == STAT_CD + STAT_IO) {
			/* up to status phase already */
			writeb(CMD_I_COMPLETE, &regs->command);
			state->phase = completing;
		} else {
			printk(KERN_DEBUG "in unexpected phase %x after cmd\n",
			       stat & STAT_PHASE);
			cmd_done(state, DID_ERROR << 16);
			return;
		}
		break;

	case dataing:
		if (intr != INTR_BUS_SERV) {
			printk(KERN_DEBUG "got intr %x before status\n", intr);
			cmd_done(state, DID_ERROR << 16);
			return;
		}
		if (cmd->SCp.this_residual != 0
		    && (stat & (STAT_MSG|STAT_CD)) == 0) {
			/* Set up the count regs to transfer more */
			nb = cmd->SCp.this_residual;
			if (nb > 0xfff0)
				nb = 0xfff0;
			cmd->SCp.this_residual -= nb;
			writeb(nb, &regs->count_lo);
			writeb(nb >> 8, &regs->count_mid);
			writeb(CMD_DMA_MODE + CMD_NOP, &regs->command);
			writeb(CMD_DMA_MODE + CMD_XFER_DATA, &regs->command);
			break;
		}
		if ((stat & STAT_PHASE) != STAT_CD + STAT_IO) {
			printk(KERN_DEBUG "intr %x before data xfer complete\n", intr);
		}
		writel(RUN << 16, &dma->control);	/* stop dma */
		if (cmd->use_sg != 0) {
			pci_unmap_sg(state->pdev,
				(struct scatterlist *)cmd->request_buffer,
				cmd->use_sg, cmd->sc_data_direction);
		} else {
			pci_unmap_single(state->pdev, state->dma_addr,
				cmd->request_bufflen, cmd->sc_data_direction);
		}
		/* should check dma status */
		writeb(CMD_I_COMPLETE, &regs->command);
		state->phase = completing;
		break;
	case completing:
		if (intr != INTR_DONE) {
			printk(KERN_DEBUG "got intr %x on completion\n", intr);
			cmd_done(state, DID_ERROR << 16);
			return;
		}
		cmd->SCp.Status = readb(&regs->fifo);
		cmd->SCp.Message = readb(&regs->fifo);
		cmd->result = CMD_ACCEPT_MSG;
		writeb(CMD_ACCEPT_MSG, &regs->command);
		state->phase = busfreeing;
		break;
	case busfreeing:
		if (intr != INTR_DISCONNECT) {
			printk(KERN_DEBUG "got intr %x when expected disconnect\n", intr);
		}
		cmd_done(state, (DID_OK << 16) + (cmd->SCp.Message << 8)
			 + cmd->SCp.Status);
		break;
	default:
		printk(KERN_DEBUG "don't know about phase %d\n", state->phase);
	}
}

static void cmd_done(struct fsc_state *state, int result)
{
	struct scsi_cmnd *cmd;

	cmd = state->current_req;
	if (cmd != 0) {
		cmd->result = result;
		(*cmd->scsi_done)(cmd);
		state->current_req = NULL;
	}
	state->phase = idle;
	mac53c94_start(state);
}

/*
 * Set up DMA commands for transferring data.
 */
static void set_dma_cmds(struct fsc_state *state, struct scsi_cmnd *cmd)
{
	int i, dma_cmd, total;
	struct scatterlist *scl;
	struct dbdma_cmd *dcmds;
	dma_addr_t dma_addr;
	u32 dma_len;

	dma_cmd = cmd->sc_data_direction == DMA_TO_DEVICE ?
			OUTPUT_MORE : INPUT_MORE;
	dcmds = state->dma_cmds;
	if (cmd->use_sg > 0) {
		int nseg;

		total = 0;
		scl = (struct scatterlist *) cmd->buffer;
		nseg = pci_map_sg(state->pdev, scl, cmd->use_sg,
				cmd->sc_data_direction);
		for (i = 0; i < nseg; ++i) {
			dma_addr = sg_dma_address(scl);
			dma_len = sg_dma_len(scl);
			if (dma_len > 0xffff)
				panic("mac53c94: scatterlist element >= 64k");
			total += dma_len;
			st_le16(&dcmds->req_count, dma_len);
			st_le16(&dcmds->command, dma_cmd);
			st_le32(&dcmds->phy_addr, dma_addr);
			dcmds->xfer_status = 0;
			++scl;
			++dcmds;
		}
	} else {
		total = cmd->request_bufflen;
		if (total > 0xffff)
			panic("mac53c94: transfer size >= 64k");
		dma_addr = pci_map_single(state->pdev, cmd->request_buffer,
					  total, cmd->sc_data_direction);
		state->dma_addr = dma_addr;
		st_le16(&dcmds->req_count, total);
		st_le32(&dcmds->phy_addr, dma_addr);
		dcmds->xfer_status = 0;
		++dcmds;
	}
	dma_cmd += OUTPUT_LAST - OUTPUT_MORE;
	st_le16(&dcmds[-1].command, dma_cmd);
	st_le16(&dcmds->command, DBDMA_STOP);
	cmd->SCp.this_residual = total;
}

static struct scsi_host_template mac53c94_template = {
	.proc_name	= "53c94",
	.name		= "53C94",
	.queuecommand	= mac53c94_queue,
	.eh_abort_handler = mac53c94_abort,
	.eh_host_reset_handler = mac53c94_host_reset,
	.can_queue	= 1,
	.this_id	= 7,
	.sg_tablesize	= SG_ALL,
	.cmd_per_lun	= 1,
	.use_clustering	= DISABLE_CLUSTERING,
};

static int mac53c94_probe(struct macio_dev *mdev, const struct of_match *match)
{
	struct device_node *node = macio_get_of_node(mdev);
	struct pci_dev *pdev = macio_get_pci_dev(mdev);
	struct fsc_state *state;
	struct Scsi_Host *host;
	void *dma_cmd_space;
	unsigned char *clkprop;
	int proplen;

	if (macio_resource_count(mdev) != 2 || macio_irq_count(mdev) != 2) {
		printk(KERN_ERR "mac53c94: expected 2 addrs and intrs (got %d/%d)\n",
		       node->n_addrs, node->n_intrs);
		return -ENODEV;
	}

	if (macio_request_resources(mdev, "mac53c94") != 0) {
       		printk(KERN_ERR "mac53c94: unable to request memory resources");
		return -EBUSY;
	}

       	host = scsi_host_alloc(&mac53c94_template, sizeof(struct fsc_state));
	if (host == NULL) {
		printk(KERN_ERR "mac53c94: couldn't register host");
		goto out_release;
	}

	state = (struct fsc_state *) host->hostdata;
	macio_set_drvdata(mdev, state);
	state->host = host;
	state->pdev = pdev;
	state->mdev = mdev;

	state->regs = (struct mac53c94_regs *)
		ioremap(macio_resource_start(mdev, 0), 0x1000);
	state->intr = macio_irq(mdev, 0);
	state->dma = (struct dbdma_regs *)
		ioremap(macio_resource_start(mdev, 1), 0x1000);
	state->dmaintr = macio_irq(mdev, 1);
	if (state->regs == NULL || state->dma == NULL) {
		printk(KERN_ERR "mac53c94: ioremap failed for %s\n",
		       node->full_name);
		goto out_free;
	}

       	clkprop = get_property(node, "clock-frequency", &proplen);
       	if (clkprop == NULL || proplen != sizeof(int)) {
       		printk(KERN_ERR "%s: can't get clock frequency, "
       		       "assuming 25MHz\n", node->full_name);
       		state->clk_freq = 25000000;
       	} else
       		state->clk_freq = *(int *)clkprop;

       	/* Space for dma command list: +1 for stop command,
       	 * +1 to allow for aligning.
	 * XXX FIXME: Use DMA consistent routines
	 */
       	dma_cmd_space = kmalloc((host->sg_tablesize + 2) *
       				sizeof(struct dbdma_cmd), GFP_KERNEL);
       	if (dma_cmd_space == 0) {
       		printk(KERN_ERR "mac53c94: couldn't allocate dma "
       		       "command space for %s\n", node->full_name);
       		goto out_free;
       	}
	state->dma_cmds = (struct dbdma_cmd *)DBDMA_ALIGN(dma_cmd_space);
	memset(state->dma_cmds, 0, (host->sg_tablesize + 1)
	       * sizeof(struct dbdma_cmd));
	state->dma_cmd_space = dma_cmd_space;

	mac53c94_init(state);

	if (request_irq(state->intr, do_mac53c94_interrupt, 0, "53C94", state)) {
		printk(KERN_ERR "mac53C94: can't get irq %d for %s\n",
		       state->intr, node->full_name);
		goto out_free_dma;
	}

	/* XXX FIXME: handle failure */
	scsi_add_host(host, &mdev->ofdev.dev);
	scsi_scan_host(host);

	return 0;

 out_free_dma:
	kfree(state->dma_cmd_space);
 out_free:
	if (state->dma != NULL)
		iounmap(state->dma);
	if (state->regs != NULL)
		iounmap(state->regs);
	scsi_host_put(host);
 out_release:
	macio_release_resources(mdev);

	return  -ENODEV;
}

static int mac53c94_remove(struct macio_dev *mdev)
{
	struct fsc_state *fp = (struct fsc_state *)macio_get_drvdata(mdev);
	struct Scsi_Host *host = fp->host;

	scsi_remove_host(host);

	free_irq(fp->intr, fp);

	if (fp->regs)
		iounmap((void *) fp->regs);
	if (fp->dma)
		iounmap((void *) fp->dma);
	kfree(fp->dma_cmd_space);

	scsi_host_put(host);

	macio_release_resources(mdev);

	return 0;
}


static struct of_match mac53c94_match[] = 
{
	{
	.name 		= "53c94",
	.type		= OF_ANY_MATCH,
	.compatible	= OF_ANY_MATCH
	},
	{},
};

static struct macio_driver mac53c94_driver = 
{
	.name 		= "mac53c94",
	.match_table	= mac53c94_match,
	.probe		= mac53c94_probe,
	.remove		= mac53c94_remove,
};


static int __init init_mac53c94(void)
{
	return macio_register_driver(&mac53c94_driver);
}

static void __exit exit_mac53c94(void)
{
	return macio_unregister_driver(&mac53c94_driver);
}

module_init(init_mac53c94);
module_exit(exit_mac53c94);

MODULE_DESCRIPTION("PowerMac 53c94 SCSI driver");
MODULE_AUTHOR("Paul Mackerras <paulus@samba.org>");
MODULE_LICENSE("GPL");
