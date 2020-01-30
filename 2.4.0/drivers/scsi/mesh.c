/*
 * SCSI low-level driver for the MESH (Macintosh Enhanced SCSI Hardware)
 * bus adaptor found on Power Macintosh computers.
 * We assume the MESH is connected to a DBDMA (descriptor-based DMA)
 * controller.
 *
 * Paul Mackerras, August 1996.
 * Copyright (C) 1996 Paul Mackerras.
 */
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/malloc.h>
#include <linux/blk.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/tqueue.h>
#include <linux/interrupt.h>
#include <linux/reboot.h>
#include <linux/spinlock.h>
#include <asm/dbdma.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/prom.h>
#include <asm/system.h>
#include <asm/irq.h>
#include <asm/hydra.h>
#include <asm/processor.h>
#include <asm/feature.h>

#include "scsi.h"
#include "hosts.h"
#include "mesh.h"

/*
 * To do:
 * - handle aborts correctly
 * - retry arbitration if lost (unless higher levels do this for us)
 */

#define MESH_NEW_STYLE_EH

#if 1
#undef KERN_DEBUG
#define KERN_DEBUG KERN_WARNING
#endif

#if CONFIG_SCSI_MESH_SYNC_RATE == 0
int mesh_sync_period = 100;
int mesh_sync_offset = 0;
#else
int mesh_sync_period = 1000 / CONFIG_SCSI_MESH_SYNC_RATE;	/* ns */
int mesh_sync_offset = 15;
#endif

int mesh_sync_targets = 0xff;	/* targets to set synchronous (bitmap) */
int mesh_resel_targets = 0xff;	/* targets that we let disconnect (bitmap) */
int mesh_debug_targets = 0;	/* print debug for these targets */
unsigned char use_active_neg = 0;  /* bit mask for SEQ_ACTIVE_NEG if used */

#define ALLOW_SYNC(tgt)		((mesh_sync_targets >> (tgt)) & 1)
#define ALLOW_RESEL(tgt)	((mesh_resel_targets >> (tgt)) & 1)
#define ALLOW_DEBUG(tgt)	((mesh_debug_targets >> (tgt)) & 1)
#define DEBUG_TARGET(cmd)	((cmd) && ALLOW_DEBUG((cmd)->target))

#undef MESH_DBG
#define N_DBG_LOG	50
#define N_DBG_SLOG	20
#define NUM_DBG_EVENTS	13
#undef	DBG_USE_TB		/* bombs on 601 */

struct dbglog {
	char	*fmt;
	u32	tb;
	u8	phase;
	u8	bs0;
	u8	bs1;
	u8	tgt;
	int	d;
};

enum mesh_phase {
	idle,
	arbitrating,
	selecting,
	commanding,
	dataing,
	statusing,
	busfreeing,
	disconnecting,
	reselecting
};

enum msg_phase {
	msg_none,
	msg_out,
	msg_out_xxx,
	msg_out_last,
	msg_in,
	msg_in_bad,
};

enum sdtr_phase {
	do_sdtr,
	sdtr_sent,
	sdtr_done
};

struct mesh_target {
	enum sdtr_phase sdtr_state;
	int	sync_params;
	int	data_goes_out;		/* guess as to data direction */
	Scsi_Cmnd *current_req;
	u32	saved_ptr;
	int	want_abort;
#ifdef MESH_DBG
	int	log_ix;
	int	n_log;
	struct dbglog log[N_DBG_LOG];
#endif
};

struct mesh_state {
	volatile struct	mesh_regs *mesh;
	int	meshintr;
	volatile struct	dbdma_regs *dma;
	int	dmaintr;
	struct	Scsi_Host *host;
	struct	mesh_state *next;
	Scsi_Cmnd *request_q;
	Scsi_Cmnd *request_qtail;
	enum mesh_phase phase;		/* what we're currently trying to do */
	enum msg_phase msgphase;
	int	conn_tgt;		/* target we're connected to */
	Scsi_Cmnd *current_req;		/* req we're currently working on */
	int	data_ptr;
	int	dma_started;
	int	dma_count;
	int	stat;
	int	aborting;
	int	expect_reply;
	int	n_msgin;
	u8	msgin[16];
	int	n_msgout;
	int	last_n_msgout;
	u8	msgout[16];
	struct dbdma_cmd *dma_cmds;	/* space for dbdma commands, aligned */
	int	clk_freq;
	struct mesh_target tgts[8];
	void	*dma_cmd_space;
	struct device_node *ofnode;
#ifndef MESH_NEW_STYLE_EH
	Scsi_Cmnd *completed_q;
	Scsi_Cmnd *completed_qtail;
	struct tq_struct tqueue;
#endif
#ifdef MESH_DBG
	int	log_ix;
	int	n_log;
	struct dbglog log[N_DBG_SLOG];
#endif
};

#ifdef MESH_DBG

static void dlog(struct mesh_state *ms, char *fmt, int a);
static void dumplog(struct mesh_state *ms, int tgt);
static void dumpslog(struct mesh_state *ms);

#else
static inline void dlog(struct mesh_state *ms, char *fmt, int a)
{}
static inline void dumplog(struct mesh_state *ms, int tgt)
{}
static inline void dumpslog(struct mesh_state *ms)
{}

#endif /* MESH_DBG */
#define MKWORD(a, b, c, d)	(((a) << 24) + ((b) << 16) + ((c) << 8) + (d))

static struct mesh_state *all_meshes;

static void mesh_init(struct mesh_state *);
static int mesh_notify_reboot(struct notifier_block *, unsigned long, void *);
static void mesh_dump_regs(struct mesh_state *);
static void mesh_start(struct mesh_state *);
static void mesh_start_cmd(struct mesh_state *, Scsi_Cmnd *);
#ifndef MESH_NEW_STYLE_EH
static void finish_cmds(void *);
#endif
static void add_sdtr_msg(struct mesh_state *);
static void set_sdtr(struct mesh_state *, int, int);
static void start_phase(struct mesh_state *);
static void get_msgin(struct mesh_state *);
static int msgin_length(struct mesh_state *);
static void cmd_complete(struct mesh_state *);
static void phase_mismatch(struct mesh_state *);
static void reselected(struct mesh_state *);
static void handle_reset(struct mesh_state *);
static void handle_error(struct mesh_state *);
static void handle_exception(struct mesh_state *);
static void mesh_interrupt(int, void *, struct pt_regs *);
static void do_mesh_interrupt(int, void *, struct pt_regs *);
static void handle_msgin(struct mesh_state *);
static void mesh_done(struct mesh_state *, int);
static void mesh_completed(struct mesh_state *, Scsi_Cmnd *);
static void set_dma_cmds(struct mesh_state *, Scsi_Cmnd *);
static void halt_dma(struct mesh_state *);
static int data_goes_out(Scsi_Cmnd *);
static void do_abort(struct mesh_state *ms);

static struct notifier_block mesh_notifier = {
	mesh_notify_reboot,
	NULL,
	0
};

int
mesh_detect(Scsi_Host_Template *tp)
{
	struct device_node *mesh;
	int nmeshes, tgt, *cfp, minper;
	struct mesh_state *ms, **prev_statep;
	struct Scsi_Host *mesh_host;
	void *dma_cmd_space;

	if (_machine == _MACH_Pmac) {
	    use_active_neg = (find_devices("mac-io") ? 0 : SEQ_ACTIVE_NEG);
	} else {
	    /* CHRP mac-io */
	    use_active_neg = SEQ_ACTIVE_NEG;
	}

	nmeshes = 0;
	prev_statep = &all_meshes;
	/*
	 * On powermacs, the MESH node has device_type "mesh".
	 * On chrp machines, its device_type is "scsi" with
	 * "chrp,mesh0" as its `compatible' property.
	 */
	mesh = find_devices("mesh");
	if (mesh == 0)
		mesh = find_compatible_devices("scsi", "chrp,mesh0");
	for (; mesh != 0; mesh = mesh->next) {
		if (mesh->n_addrs != 2 || mesh->n_intrs != 2) {
			printk(KERN_ERR "mesh: expected 2 addrs and 2 intrs"
			       " (got %d,%d)", mesh->n_addrs, mesh->n_intrs);
			continue;
		}
		mesh_host = scsi_register(tp, sizeof(struct mesh_state));
		if (mesh_host == 0) {
			printk(KERN_ERR "mesh: couldn't register host");
			continue;
		}
		mesh_host->unique_id = nmeshes;
#if !defined(MODULE)
		note_scsi_host(mesh, mesh_host);
#endif

		ms = (struct mesh_state *) mesh_host->hostdata;
		if (ms == 0)
			panic("no mesh state");
		memset(ms, 0, sizeof(*ms));
		ms->host = mesh_host;
		ms->ofnode = mesh;
		ms->mesh = (volatile struct mesh_regs *)
			ioremap(mesh->addrs[0].address, 0x1000);
		ms->dma = (volatile struct dbdma_regs *)
			ioremap(mesh->addrs[1].address, 0x1000);
		ms->meshintr = mesh->intrs[0].line;
		ms->dmaintr = mesh->intrs[1].line;

		/* Space for dma command list: +1 for stop command,
		   +1 to allow for aligning. */
		dma_cmd_space = kmalloc((mesh_host->sg_tablesize + 2) *
					sizeof(struct dbdma_cmd), GFP_KERNEL);
		if (dma_cmd_space == 0)
			panic("mesh: couldn't allocate dma command space");
		ms->dma_cmds = (struct dbdma_cmd *) DBDMA_ALIGN(dma_cmd_space);
		memset(ms->dma_cmds, 0, (mesh_host->sg_tablesize + 1)
		       * sizeof(struct dbdma_cmd));
		ms->dma_cmd_space = dma_cmd_space;

		ms->current_req = 0;
		for (tgt = 0; tgt < 8; ++tgt) {
			ms->tgts[tgt].sdtr_state = do_sdtr;
			ms->tgts[tgt].sync_params = ASYNC_PARAMS;
			ms->tgts[tgt].current_req = 0;
		}
#ifndef MESH_NEW_STYLE_EH
		ms->tqueue.routine = finish_cmds;
		ms->tqueue.data = ms;
#endif
		*prev_statep = ms;
		prev_statep = &ms->next;

		if ((cfp = (int *) get_property(mesh, "clock-frequency",
						NULL))) {
			ms->clk_freq = *cfp;
		} else {
			printk(KERN_INFO "mesh: assuming 50MHz clock frequency\n");
			ms->clk_freq = 50000000;
		}
		/* The maximum sync rate is clock / 5; increase
		   mesh_sync_period if necessary. */
		minper = 1000000000 / (ms->clk_freq / 5);	/* ns */
		if (mesh_sync_period < minper)
			mesh_sync_period = minper;

		feature_set(mesh, FEATURE_MESH_enable);
		mdelay(200);

		mesh_init(ms);

		if (request_irq(ms->meshintr, do_mesh_interrupt, 0, "MESH", ms)) {
			printk(KERN_ERR "MESH: can't get irq %d\n", ms->meshintr);
		}

		++nmeshes;
	}

	if ((_machine == _MACH_Pmac) && (nmeshes > 0))
		register_reboot_notifier(&mesh_notifier);

	return nmeshes;
}

int
mesh_release(struct Scsi_Host *host)
{
	struct mesh_state *ms = (struct mesh_state *) host->hostdata;

	if (ms == 0)
		return 0;
	if (ms->mesh)
		iounmap((void *) ms->mesh);
	if (ms->dma)
		iounmap((void *) ms->dma);
	kfree(ms->dma_cmd_space);
	free_irq(ms->meshintr, ms);
	feature_clear(ms->ofnode, FEATURE_MESH_enable);
	return 0;
}

int
mesh_queue(Scsi_Cmnd *cmd, void (*done)(Scsi_Cmnd *))
{
	unsigned long flags;
	struct mesh_state *ms;

	cmd->scsi_done = done;
	cmd->host_scribble = NULL;

	ms = (struct mesh_state *) cmd->host->hostdata;

	save_flags(flags);
	cli();
	if (ms->request_q == NULL)
		ms->request_q = cmd;
	else
		ms->request_qtail->host_scribble = (void *) cmd;
	ms->request_qtail = cmd;

	if (ms->phase == idle)
		mesh_start(ms);

	restore_flags(flags);
	return 0;
}

int
mesh_abort(Scsi_Cmnd *cmd)
{
	struct mesh_state *ms = (struct mesh_state *) cmd->host->hostdata;

	printk(KERN_DEBUG "mesh_abort(%p)\n", cmd);
	mesh_dump_regs(ms);
	dumplog(ms, cmd->target);
	dumpslog(ms);
	return SCSI_ABORT_SNOOZE;
}

static void
mesh_dump_regs(struct mesh_state *ms)
{
	volatile struct mesh_regs *mr = ms->mesh;
	volatile struct dbdma_regs *md = ms->dma;
	int t;
	struct mesh_target *tp;

	printk(KERN_DEBUG "mesh: state at %p, regs at %p, dma at %p\n",
	       ms, mr, md);
	printk(KERN_DEBUG "    ct=%4x seq=%2x bs=%4x fc=%2x "
	       "exc=%2x err=%2x im=%2x int=%2x sp=%2x\n",
	       (mr->count_hi << 8) + mr->count_lo, mr->sequence,
	       (mr->bus_status1 << 8) + mr->bus_status0, mr->fifo_count,
	       mr->exception, mr->error, mr->intr_mask, mr->interrupt,
	       mr->sync_params);
	while(in_8(&mr->fifo_count))
		printk(KERN_DEBUG " fifo data=%.2x\n",in_8(&mr->fifo));
	printk(KERN_DEBUG "    dma stat=%x cmdptr=%x\n",
	       in_le32(&md->status), in_le32(&md->cmdptr));
	printk(KERN_DEBUG "    phase=%d msgphase=%d conn_tgt=%d data_ptr=%d\n",
	       ms->phase, ms->msgphase, ms->conn_tgt, ms->data_ptr);
	printk(KERN_DEBUG "    dma_st=%d dma_ct=%d n_msgout=%d\n",
	       ms->dma_started, ms->dma_count, ms->n_msgout);
	for (t = 0; t < 8; ++t) {
		tp = &ms->tgts[t];
		if (tp->current_req == NULL)
			continue;
		printk(KERN_DEBUG "    target %d: req=%p goes_out=%d saved_ptr=%d\n",
		       t, tp->current_req, tp->data_goes_out, tp->saved_ptr);
	}
}

int
mesh_reset(Scsi_Cmnd *cmd, unsigned how)
{
	struct mesh_state *ms = (struct mesh_state *) cmd->host->hostdata;
	volatile struct mesh_regs *mr = ms->mesh;
	volatile struct dbdma_regs *md = ms->dma;
	unsigned long flags;
	int ret;

	printk(KERN_DEBUG "mesh_reset %x\n", how);
	ret = SCSI_RESET_BUS_RESET;
	save_flags(flags);
	cli();
	out_le32(&md->control, (RUN|PAUSE|FLUSH|WAKE) << 16);	/* stop dma */
	out_8(&mr->exception, 0xff);	/* clear all exception bits */
	out_8(&mr->error, 0xff);	/* clear all error bits */
	if (how & SCSI_RESET_SUGGEST_HOST_RESET) {
		out_8(&mr->sequence, SEQ_RESETMESH);
		ret |= SCSI_RESET_HOST_RESET;
		udelay(1);
		out_8(&mr->intr_mask, INT_ERROR | INT_EXCEPTION | INT_CMDDONE);
		out_8(&mr->source_id, ms->host->this_id);
		out_8(&mr->sel_timeout, 25);	/* 250ms */
		out_8(&mr->sync_params, ASYNC_PARAMS);
	}
	out_8(&mr->bus_status1, BS1_RST);	/* assert RST */
	udelay(30);			/* leave it on for >= 25us */
	out_8(&mr->bus_status1, 0);	/* negate RST */
#ifdef DO_ASYNC_RESET
	if (how & SCSI_RESET_ASYNCHRONOUS) {
		restore_flags(flags);
		ret |= SCSI_RESET_PENDING;
	} else
#endif
	{
		handle_reset(ms);
		restore_flags(flags);
#ifndef MESH_NEW_STYLE_EH
		finish_cmds(ms);
#endif
		ret |= SCSI_RESET_SUCCESS;
	}
	return ret;
}

/*
 * If we leave drives set for synchronous transfers (especially
 * CDROMs), and reboot to MacOS, it gets confused, poor thing.
 * So, on reboot we reset the SCSI bus.
 */
static int
mesh_notify_reboot(struct notifier_block *this, unsigned long code, void *x)
{
	struct mesh_state *ms;
	volatile struct mesh_regs *mr;

	if (code == SYS_DOWN) {
		printk(KERN_INFO "resetting MESH scsi bus(es)\n");
		for (ms = all_meshes; ms != 0; ms = ms->next) {
			mr = ms->mesh;
			out_8(&mr->intr_mask, 0);
			out_8(&mr->interrupt,
			      INT_ERROR | INT_EXCEPTION | INT_CMDDONE);
			out_8(&mr->bus_status1, BS1_RST);
			udelay(30);
			out_8(&mr->bus_status1, 0);
		}
	}
	return NOTIFY_DONE;
}

int
mesh_command(Scsi_Cmnd *cmd)
{
	printk(KERN_WARNING "whoops... mesh_command called\n");
	return -1;
}

static void
mesh_init(struct mesh_state *ms)
{
	volatile struct mesh_regs *mr = ms->mesh;
	volatile struct dbdma_regs *md = ms->dma;

	udelay(100);

	out_le32(&md->control, (RUN|PAUSE|FLUSH|WAKE) << 16);	/* stop dma */
	out_8(&mr->exception, 0xff);	/* clear all exception bits */
	out_8(&mr->error, 0xff);	/* clear all error bits */
	out_8(&mr->sequence, SEQ_RESETMESH);
	udelay(10);
	out_8(&mr->intr_mask, INT_ERROR | INT_EXCEPTION | INT_CMDDONE);
	out_8(&mr->source_id, ms->host->this_id);
	out_8(&mr->sel_timeout, 25);	/* 250ms */
	out_8(&mr->sync_params, ASYNC_PARAMS);

	out_8(&mr->bus_status1, BS1_RST);	/* assert RST */
	udelay(30);			/* leave it on for >= 25us */
	out_8(&mr->bus_status1, 0);	/* negate RST */

	out_8(&mr->sequence, SEQ_FLUSHFIFO);
	udelay(1);
	out_8(&mr->sync_params, ASYNC_PARAMS);
	out_8(&mr->sequence, SEQ_ENBRESEL);
	out_8(&mr->interrupt, 0xff);	/* clear all interrupt bits */
}

/*
 * Start the next command for a MESH.
 * Should be called with interrupts disabled.
 */
static void
mesh_start(struct mesh_state *ms)
{
	Scsi_Cmnd *cmd, *prev, *next;

	if (ms->phase != idle || ms->current_req != NULL) {
		printk(KERN_ERR "inappropriate mesh_start (phase=%d, ms=%p)",
		       ms->phase, ms);
		return;
	}

	while (ms->phase == idle) {
		prev = NULL;
		for (cmd = ms->request_q; ; cmd = (Scsi_Cmnd *) cmd->host_scribble) {
			if (cmd == NULL)
				return;
			if (ms->tgts[cmd->target].current_req == NULL)
				break;
			prev = cmd;
		}
		next = (Scsi_Cmnd *) cmd->host_scribble;
		if (prev == NULL)
			ms->request_q = next;
		else
			prev->host_scribble = (void *) next;
		if (next == NULL)
			ms->request_qtail = prev;

		mesh_start_cmd(ms, cmd);
	}
}

static void
mesh_start_cmd(struct mesh_state *ms, Scsi_Cmnd *cmd)
{
	volatile struct mesh_regs *mr = ms->mesh;
	int t;

	ms->current_req = cmd;
	ms->tgts[cmd->target].data_goes_out = data_goes_out(cmd);
	ms->tgts[cmd->target].current_req = cmd;

#if 1
	if (DEBUG_TARGET(cmd)) {
		int i;
		printk(KERN_DEBUG "mesh_start: %p ser=%lu tgt=%d cmd=",
		       cmd, cmd->serial_number, cmd->target);
		for (i = 0; i < cmd->cmd_len; ++i)
			printk(" %x", cmd->cmnd[i]);
		printk(" use_sg=%d buffer=%p bufflen=%u\n",
		       cmd->use_sg, cmd->request_buffer, cmd->request_bufflen);
	}
#endif

	ms->phase = arbitrating;
	ms->msgphase = msg_none;
	ms->data_ptr = 0;
	ms->dma_started = 0;
	ms->n_msgout = 0;
	ms->last_n_msgout = 0;
	ms->expect_reply = 0;
	ms->conn_tgt = cmd->target;
	ms->tgts[cmd->target].saved_ptr = 0;
	ms->stat = DID_OK;
	ms->aborting = 0;
#ifdef MESH_DBG
	ms->tgts[cmd->target].n_log = 0;
	dlog(ms, "start cmd=%x", (int) cmd);
#endif

	/* Off we go */
	dlog(ms, "about to arb, intr/exc/err/fc=%.8x",
	     MKWORD(mr->interrupt, mr->exception, mr->error, mr->fifo_count));
	out_8(&mr->interrupt, INT_CMDDONE);
	out_8(&mr->sequence, SEQ_ENBRESEL);
	udelay(1);

	if (mr->bus_status1 & (BS1_BSY | BS1_SEL)) {
		/*
		 * Some other device has the bus or is arbitrating for it -
		 * probably a target which is about to reselect us.
		 */
		dlog(ms, "busy b4 arb, intr/exc/err/fc=%.8x",
		     MKWORD(mr->interrupt, mr->exception,
			    mr->error, mr->fifo_count));
		for (t = 100; t > 0; --t) {
			if ((mr->bus_status1 & (BS1_BSY | BS1_SEL)) == 0)
				break;
			if (in_8(&mr->interrupt) != 0) {
				dlog(ms, "intr b4 arb, intr/exc/err/fc=%.8x",
				     MKWORD(mr->interrupt, mr->exception,
					    mr->error, mr->fifo_count));
				mesh_interrupt(0, (void *)ms, 0);
				if (ms->phase != arbitrating)
					return;
			}
			udelay(1);
		}
		if (mr->bus_status1 & (BS1_BSY | BS1_SEL)) {
			/* XXX should try again in a little while */
			ms->stat = DID_BUS_BUSY;
			ms->phase = idle;
			mesh_done(ms, 0);
			return;
		}
	}

	/*
	 * Apparently the mesh has a bug where it will assert both its
	 * own bit and the target's bit on the bus during arbitration.
	 */
	out_8(&mr->dest_id, mr->source_id);

	/*
	 * There appears to be a race with reselection sometimes,
	 * where a target reselects us just as we issue the
	 * arbitrate command.  It seems that then the arbitrate
	 * command just hangs waiting for the bus to be free
	 * without giving us a reselection exception.
	 * The only way I have found to get it to respond correctly
	 * is this: disable reselection before issuing the arbitrate
	 * command, then after issuing it, if it looks like a target
	 * is trying to reselect us, reset the mesh and then enable
	 * reselection.
	 */
	out_8(&mr->sequence, SEQ_DISRESEL);
	if (in_8(&mr->interrupt) != 0) {
		dlog(ms, "intr after disresel, intr/exc/err/fc=%.8x",
		     MKWORD(mr->interrupt, mr->exception,
			    mr->error, mr->fifo_count));
		mesh_interrupt(0, (void *)ms, 0);
		if (ms->phase != arbitrating)
			return;
		dlog(ms, "after intr after disresel, intr/exc/err/fc=%.8x",
		     MKWORD(mr->interrupt, mr->exception,
			    mr->error, mr->fifo_count));
	}

	out_8(&mr->sequence, SEQ_ARBITRATE);

	for (t = 230; t > 0; --t) {
		if (in_8(&mr->interrupt) != 0)
			break;
		udelay(1);
	}
	dlog(ms, "after arb, intr/exc/err/fc=%.8x",
	     MKWORD(mr->interrupt, mr->exception, mr->error, mr->fifo_count));
	if (mr->interrupt == 0 && (mr->bus_status1 & BS1_SEL)
	    && (mr->bus_status0 & BS0_IO)) {
		/* looks like a reselection - try resetting the mesh */
		dlog(ms, "resel? after arb, intr/exc/err/fc=%.8x",
		     MKWORD(mr->interrupt, mr->exception, mr->error, mr->fifo_count));
		out_8(&mr->sequence, SEQ_RESETMESH);
		udelay(10);
		out_8(&mr->interrupt, INT_ERROR | INT_EXCEPTION | INT_CMDDONE);
		out_8(&mr->intr_mask, INT_ERROR | INT_EXCEPTION | INT_CMDDONE);
		out_8(&mr->sequence, SEQ_ENBRESEL);
		for (t = 10; t > 0 && mr->interrupt == 0; --t)
			udelay(1);
		dlog(ms, "tried reset after arb, intr/exc/err/fc=%.8x",
		     MKWORD(mr->interrupt, mr->exception, mr->error, mr->fifo_count));
#ifndef MESH_MULTIPLE_HOSTS
		if (mr->interrupt == 0 && (mr->bus_status1 & BS1_SEL)
		    && (mr->bus_status0 & BS0_IO)) {
			printk(KERN_ERR "mesh: controller not responding"
			       " to reselection!\n");
			/*
			 * If this is a target reselecting us, and the
			 * mesh isn't responding, the higher levels of
			 * the scsi code will eventually time out and
			 * reset the bus.
			 */
		}
#endif
	}
}

#ifndef MESH_NEW_STYLE_EH
static void
finish_cmds(void *data)
{
	struct mesh_state *ms = data;
	Scsi_Cmnd *cmd;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&io_request_lock, flags);
		cmd = ms->completed_q;
		if (cmd == NULL) {
			spin_unlock_irqrestore(&io_request_lock, flags);
			break;
		}
		ms->completed_q = (Scsi_Cmnd *) cmd->host_scribble;
		(*cmd->scsi_done)(cmd);
		spin_unlock_irqrestore(&io_request_lock, flags);
	}
}
#endif /* MESH_NEW_STYLE_EH */

static inline void
add_sdtr_msg(struct mesh_state *ms)
{
	int i = ms->n_msgout;

	ms->msgout[i] = EXTENDED_MESSAGE;
	ms->msgout[i+1] = 3;
	ms->msgout[i+2] = EXTENDED_SDTR;
	ms->msgout[i+3] = mesh_sync_period/4;
	ms->msgout[i+4] = (ALLOW_SYNC(ms->conn_tgt)? mesh_sync_offset: 0);
	ms->n_msgout = i + 5;
}

static void
set_sdtr(struct mesh_state *ms, int period, int offset)
{
	struct mesh_target *tp = &ms->tgts[ms->conn_tgt];
	volatile struct mesh_regs *mr = ms->mesh;
	int v, tr;

	tp->sdtr_state = sdtr_done;
	if (offset == 0) {
		/* asynchronous */
		if (SYNC_OFF(tp->sync_params))
			printk(KERN_INFO "mesh: target %d now asynchronous\n",
			       ms->conn_tgt);
		tp->sync_params = ASYNC_PARAMS;
		out_8(&mr->sync_params, ASYNC_PARAMS);
		return;
	}
	/*
	 * We need to compute ceil(clk_freq * period / 500e6) - 2
	 * without incurring overflow.
	 */
	v = (ms->clk_freq / 5000) * period;
	if (v <= 250000) {
		/* special case: sync_period == 5 * clk_period */
		v = 0;
		/* units of tr are 100kB/s */
		tr = (ms->clk_freq + 250000) / 500000;
	} else {
		/* sync_period == (v + 2) * 2 * clk_period */
		v = (v + 99999) / 100000 - 2;
		if (v > 15)
			v = 15;	/* oops */
		tr = ((ms->clk_freq / (v + 2)) + 199999) / 200000;
	}
	if (offset > 15)
		offset = 15;	/* can't happen */
	tp->sync_params = SYNC_PARAMS(offset, v);
	out_8(&mr->sync_params, tp->sync_params);
	printk(KERN_INFO "mesh: target %d synchronous at %d.%d MB/s\n",
	       ms->conn_tgt, tr/10, tr%10);
}

static void
start_phase(struct mesh_state *ms)
{
	int i, seq, nb;
	volatile struct mesh_regs *mr = ms->mesh;
	volatile struct dbdma_regs *md = ms->dma;
	Scsi_Cmnd *cmd = ms->current_req;
	struct mesh_target *tp = &ms->tgts[ms->conn_tgt];

	dlog(ms, "start_phase nmo/exc/fc/seq = %.8x",
	     MKWORD(ms->n_msgout, mr->exception, mr->fifo_count, mr->sequence));
	out_8(&mr->interrupt, INT_ERROR | INT_EXCEPTION | INT_CMDDONE);
	seq = use_active_neg + (ms->n_msgout? SEQ_ATN: 0);
	switch (ms->msgphase) {
	case msg_none:
		break;

	case msg_in:
		out_8(&mr->count_hi, 0);
		out_8(&mr->count_lo, 1);
		out_8(&mr->sequence, SEQ_MSGIN + seq);
		ms->n_msgin = 0;
		return;

	case msg_out:
		/*
		 * To make sure ATN drops before we assert ACK for
		 * the last byte of the message, we have to do the
		 * last byte specially.
		 */
		if (ms->n_msgout <= 0) {
			printk(KERN_ERR "mesh: msg_out but n_msgout=%d\n",
			       ms->n_msgout);
			mesh_dump_regs(ms);
			ms->msgphase = msg_none;
			break;
		}
		if (ALLOW_DEBUG(ms->conn_tgt)) {
			printk(KERN_DEBUG "mesh: sending %d msg bytes:",
			       ms->n_msgout);
			for (i = 0; i < ms->n_msgout; ++i)
				printk(" %x", ms->msgout[i]);
			printk("\n");
		}
		dlog(ms, "msgout msg=%.8x", MKWORD(ms->n_msgout, ms->msgout[0],
						ms->msgout[1], ms->msgout[2]));
		out_8(&mr->count_hi, 0);
		out_8(&mr->sequence, SEQ_FLUSHFIFO);
		udelay(1);
		/*
		 * If ATN is not already asserted, we assert it, then
		 * issue a SEQ_MSGOUT to get the mesh to drop ACK.
		 */
		if ((mr->bus_status0 & BS0_ATN) == 0) {
			dlog(ms, "bus0 was %.2x explictly asserting ATN", mr->bus_status0);
			out_8(&mr->bus_status0, BS0_ATN); /* explicit ATN */
			udelay(1);
			out_8(&mr->count_lo, 1);
			out_8(&mr->sequence, SEQ_MSGOUT + seq);
			out_8(&mr->bus_status0, 0); /* release explicit ATN */
			dlog(ms,"hace: after explicit ATN bus0=%.2x",mr->bus_status0);
		}
		if (ms->n_msgout == 1) {
			/*
			 * We can't issue the SEQ_MSGOUT without ATN
			 * until the target has asserted REQ.  The logic
			 * in cmd_complete handles both situations:
			 * REQ already asserted or not.
			 */
			cmd_complete(ms);
		} else {
			out_8(&mr->count_lo, ms->n_msgout - 1);
			out_8(&mr->sequence, SEQ_MSGOUT + seq);
			for (i = 0; i < ms->n_msgout - 1; ++i)
				out_8(&mr->fifo, ms->msgout[i]);
		}
		return;

	default:
		printk(KERN_ERR "mesh bug: start_phase msgphase=%d\n",
		       ms->msgphase);
	}

	switch (ms->phase) {
	case selecting:
		out_8(&mr->dest_id, ms->conn_tgt);
		out_8(&mr->sequence, SEQ_SELECT + SEQ_ATN);
		break;
	case commanding:
		out_8(&mr->sync_params, tp->sync_params);
		out_8(&mr->count_hi, 0);
		if (cmd) {
			out_8(&mr->count_lo, cmd->cmd_len);
			out_8(&mr->sequence, SEQ_COMMAND + seq);
			for (i = 0; i < cmd->cmd_len; ++i)
				out_8(&mr->fifo, cmd->cmnd[i]);
		} else {
			out_8(&mr->count_lo, 6);
			out_8(&mr->sequence, SEQ_COMMAND + seq);
			for (i = 0; i < 6; ++i)
				out_8(&mr->fifo, 0);
		}
		break;
	case dataing:
		/* transfer data, if any */
		if (!ms->dma_started) {
			set_dma_cmds(ms, cmd);
			out_le32(&md->cmdptr, virt_to_phys(ms->dma_cmds));
			out_le32(&md->control, (RUN << 16) | RUN);
			ms->dma_started = 1;
		}
		nb = ms->dma_count;
		if (nb > 0xfff0)
			nb = 0xfff0;
		ms->dma_count -= nb;
		ms->data_ptr += nb;
		out_8(&mr->count_lo, nb);
		out_8(&mr->count_hi, nb >> 8);
		out_8(&mr->sequence, (tp->data_goes_out?
				SEQ_DATAOUT: SEQ_DATAIN) + SEQ_DMA_MODE + seq);
		break;
	case statusing:
		out_8(&mr->count_hi, 0);
		out_8(&mr->count_lo, 1);
		out_8(&mr->sequence, SEQ_STATUS + seq);
		break;
	case busfreeing:
	case disconnecting:
		out_8(&mr->sequence, SEQ_ENBRESEL);
		udelay(1);
		dlog(ms, "enbresel intr/exc/err/fc=%.8x",
		     MKWORD(mr->interrupt, mr->exception, mr->error,
			    mr->fifo_count));
		out_8(&mr->sequence, SEQ_BUSFREE);
		break;
	default:
		printk(KERN_ERR "mesh: start_phase called with phase=%d\n",
		       ms->phase);
		dumpslog(ms);
	}

}

static inline void
get_msgin(struct mesh_state *ms)
{
	volatile struct mesh_regs *mr = ms->mesh;
	int i, n;

	n = mr->fifo_count;
	if (n != 0) {
		i = ms->n_msgin;
		ms->n_msgin = i + n;
		for (; n > 0; --n)
			ms->msgin[i++] = in_8(&mr->fifo);
	}
}

static inline int
msgin_length(struct mesh_state *ms)
{
	int b, n;

	n = 1;
	if (ms->n_msgin > 0) {
		b = ms->msgin[0];
		if (b == 1) {
			/* extended message */
			n = ms->n_msgin < 2? 2: ms->msgin[1] + 2;
		} else if (0x20 <= b && b <= 0x2f) {
			/* 2-byte message */
			n = 2;
		}
	}
	return n;
}

static void
cmd_complete(struct mesh_state *ms)
{
	volatile struct mesh_regs *mr = ms->mesh;
	Scsi_Cmnd *cmd = ms->current_req;
	struct mesh_target *tp = &ms->tgts[ms->conn_tgt];
	int seq, n, t;

	dlog(ms, "cmd_complete fc=%x", mr->fifo_count);
	seq = use_active_neg + (ms->n_msgout? SEQ_ATN: 0);
	switch (ms->msgphase) {
	case msg_out_xxx:
		/* huh?  we expected a phase mismatch */
		ms->n_msgin = 0;
		ms->msgphase = msg_in;
		/* fall through */

	case msg_in:
		/* should have some message bytes in fifo */
		get_msgin(ms);
		n = msgin_length(ms);
		if (ms->n_msgin < n) {
			out_8(&mr->count_lo, n - ms->n_msgin);
			out_8(&mr->sequence, SEQ_MSGIN + seq);
		} else {
			ms->msgphase = msg_none;
			handle_msgin(ms);
			start_phase(ms);
		}
		break;

	case msg_in_bad:
		out_8(&mr->sequence, SEQ_FLUSHFIFO);
		udelay(1);
		out_8(&mr->count_lo, 1);
		out_8(&mr->sequence, SEQ_MSGIN + SEQ_ATN + use_active_neg);
		break;

	case msg_out:
		/*
		 * To get the right timing on ATN wrt ACK, we have
		 * to get the MESH to drop ACK, wait until REQ gets
		 * asserted, then drop ATN.  To do this we first
		 * issue a SEQ_MSGOUT with ATN and wait for REQ,
		 * then change the command to a SEQ_MSGOUT w/o ATN.
		 * If we don't see REQ in a reasonable time, we
		 * change the command to SEQ_MSGIN with ATN,
		 * wait for the phase mismatch interrupt, then
		 * issue the SEQ_MSGOUT without ATN.
		 */
		out_8(&mr->count_lo, 1);
		out_8(&mr->sequence, SEQ_MSGOUT + use_active_neg + SEQ_ATN);
		t = 30;		/* wait up to 30us */
		while ((mr->bus_status0 & BS0_REQ) == 0 && --t >= 0)
			udelay(1);
		dlog(ms, "last_mbyte err/exc/fc/cl=%.8x",
		     MKWORD(mr->error, mr->exception,
			    mr->fifo_count, mr->count_lo));
		if (in_8(&mr->interrupt) & (INT_ERROR | INT_EXCEPTION)) {
			/* whoops, target didn't do what we expected */
			ms->last_n_msgout = ms->n_msgout;
			ms->n_msgout = 0;
			if (in_8(&mr->interrupt) & INT_ERROR) {
				printk(KERN_ERR "mesh: error %x in msg_out\n",
				       in_8(&mr->error));
				handle_error(ms);
				return;
			}
			if (in_8(&mr->exception) != EXC_PHASEMM)
				printk(KERN_ERR "mesh: exc %x in msg_out\n",
				       in_8(&mr->exception));
			else
				printk(KERN_DEBUG "mesh: bs0=%x in msg_out\n",
				       in_8(&mr->bus_status0));
			handle_exception(ms);
			return;
		}
		if (mr->bus_status0 & BS0_REQ) {
			out_8(&mr->sequence, SEQ_MSGOUT + use_active_neg);
			udelay(1);
			out_8(&mr->fifo, ms->msgout[ms->n_msgout-1]);
			ms->msgphase = msg_out_last;
		} else {
			out_8(&mr->sequence, SEQ_MSGIN + use_active_neg + SEQ_ATN);
			ms->msgphase = msg_out_xxx;
		}
		break;

	case msg_out_last:
		ms->last_n_msgout = ms->n_msgout;
		ms->n_msgout = 0;
		ms->msgphase = ms->expect_reply? msg_in: msg_none;
		start_phase(ms);
		break;

	case msg_none:
		switch (ms->phase) {
		case idle:
			printk(KERN_ERR "mesh: interrupt in idle phase?\n");
			dumpslog(ms);
			return;
		case selecting:
			dlog(ms, "Selecting phase at command completion",0);
			ms->msgout[0] = IDENTIFY(ALLOW_RESEL(ms->conn_tgt),
						 (cmd? cmd->lun: 0));
			ms->n_msgout = 1;
			ms->expect_reply = 0;
			if (ms->aborting) {
				ms->msgout[0] = ABORT;
				ms->n_msgout++;
			} else if (tp->sdtr_state == do_sdtr) {
				/* add SDTR message */
				add_sdtr_msg(ms);
				ms->expect_reply = 1;
				tp->sdtr_state = sdtr_sent;
			}
			ms->msgphase = msg_out;
			/*
			 * We need to wait for REQ before dropping ATN.
			 * We wait for at most 30us, then fall back to
			 * a scheme where we issue a SEQ_COMMAND with ATN,
			 * which will give us a phase mismatch interrupt
			 * when REQ does come, and then we send the message.
			 */
			t = 230;		/* wait up to 230us */
			while ((mr->bus_status0 & BS0_REQ) == 0) {
				if (--t < 0) {
					dlog(ms, "impatient for req", ms->n_msgout);
					ms->msgphase = msg_none;
					break;
				}
				udelay(1);
			}
			break;
		case dataing:
			if (ms->dma_count != 0) {
				start_phase(ms);
				return;
			}
			/*
			 * We can get a phase mismatch here if the target
			 * changes to the status phase, even though we have
			 * had a command complete interrupt.  Then, if we
			 * issue the SEQ_STATUS command, we'll get a sequence
			 * error interrupt.  Which isn't so bad except that
			 * occasionally the mesh actually executes the
			 * SEQ_STATUS *as well as* giving us the sequence
			 * error and phase mismatch exception.
			 */
			out_8(&mr->sequence, 0);
			out_8(&mr->interrupt,
			      INT_ERROR | INT_EXCEPTION | INT_CMDDONE);
			halt_dma(ms);
			break;
		case statusing:
			if (cmd) {
				cmd->SCp.Status = mr->fifo;
				if (DEBUG_TARGET(cmd))
					printk(KERN_DEBUG "mesh: status is %x\n",
					       cmd->SCp.Status);
			}
			ms->msgphase = msg_in;
			break;
		case busfreeing:
			mesh_done(ms, 1);
			return;
		case disconnecting:
			ms->current_req = 0;
			ms->phase = idle;
			mesh_start(ms);
			return;
		default:
			break;
		}
		++ms->phase;
		start_phase(ms);
		break;
	}
}

static void phase_mismatch(struct mesh_state *ms)
{
	volatile struct mesh_regs *mr = ms->mesh;
	int phase;

	dlog(ms, "phasemm ch/cl/seq/fc=%.8x",
	     MKWORD(mr->count_hi, mr->count_lo, mr->sequence, mr->fifo_count));
	phase = mr->bus_status0 & BS0_PHASE;
	if (ms->msgphase == msg_out_xxx && phase == BP_MSGOUT) {
		/* output the last byte of the message, without ATN */
		out_8(&mr->count_lo, 1);
		out_8(&mr->sequence, SEQ_MSGOUT + use_active_neg);
		udelay(1);
		out_8(&mr->fifo, ms->msgout[ms->n_msgout-1]);
		ms->msgphase = msg_out_last;
		return;
	}

	if (ms->msgphase == msg_in) {
		get_msgin(ms);
		if (ms->n_msgin)
			handle_msgin(ms);
	}

	if (ms->dma_started)
		halt_dma(ms);
	if (mr->fifo_count) {
		out_8(&mr->sequence, SEQ_FLUSHFIFO);
		udelay(1);
	}

	ms->msgphase = msg_none;
	switch (phase) {
	case BP_DATAIN:
		ms->tgts[ms->conn_tgt].data_goes_out = 0;
		ms->phase = dataing;
		break;
	case BP_DATAOUT:
		ms->tgts[ms->conn_tgt].data_goes_out = 1;
		ms->phase = dataing;
		break;
	case BP_COMMAND:
		ms->phase = commanding;
		break;
	case BP_STATUS:
		ms->phase = statusing;
		break;
	case BP_MSGIN:
		ms->msgphase = msg_in;
		ms->n_msgin = 0;
		break;
	case BP_MSGOUT:
		ms->msgphase = msg_out;
		if (ms->n_msgout == 0) {
			if (ms->aborting) {
				do_abort(ms);
			} else {
				if (ms->last_n_msgout == 0) {
					printk(KERN_DEBUG
					       "mesh: no msg to repeat\n");
					ms->msgout[0] = NOP;
					ms->last_n_msgout = 1;
				}
				ms->n_msgout = ms->last_n_msgout;
			}
		}
		break;
	default:
		printk(KERN_DEBUG "mesh: unknown scsi phase %x\n", phase);
		ms->stat = DID_ERROR;
		mesh_done(ms, 1);
		return;
	}

	start_phase(ms);
}

static void
reselected(struct mesh_state *ms)
{
	volatile struct mesh_regs *mr = ms->mesh;
	Scsi_Cmnd *cmd;
	struct mesh_target *tp;
	int b, t, prev;

	switch (ms->phase) {
	case idle:
		break;
	case arbitrating:
		if ((cmd = ms->current_req) != NULL) {
			/* put the command back on the queue */
			cmd->host_scribble = (void *) ms->request_q;
			if (ms->request_q == NULL)
				ms->request_qtail = cmd;
			ms->request_q = cmd;
			tp = &ms->tgts[cmd->target];
			tp->current_req = NULL;
		}
		break;
	case busfreeing:
		ms->phase = reselecting;
		mesh_done(ms, 0);
		break;
	case disconnecting:
		break;
	default:
		printk(KERN_ERR "mesh: reselected in phase %d/%d tgt %d\n",
		       ms->msgphase, ms->phase, ms->conn_tgt);
		dumplog(ms, ms->conn_tgt);
		dumpslog(ms);
	}

	ms->current_req = NULL;
	ms->phase = dataing;
	ms->msgphase = msg_in;
	ms->dma_started = 0;
	ms->n_msgout = 0;
	ms->last_n_msgout = 0;
	prev = ms->conn_tgt;

	/*
	 * We seem to get abortive reselections sometimes.
	 */
	while ((mr->bus_status1 & BS1_BSY) == 0) {
		static int mesh_aborted_resels;
		mesh_aborted_resels++;
		out_8(&mr->interrupt, INT_ERROR | INT_EXCEPTION | INT_CMDDONE);
		udelay(1);
		out_8(&mr->sequence, SEQ_ENBRESEL);
		udelay(5);
		dlog(ms, "extra resel err/exc/fc = %.6x",
		     MKWORD(0, mr->error, mr->exception, mr->fifo_count));
	}
	out_8(&mr->interrupt, INT_ERROR | INT_EXCEPTION | INT_CMDDONE);
	udelay(1);
	out_8(&mr->sequence, SEQ_ENBRESEL);
	udelay(1);
	out_8(&mr->sync_params, ASYNC_PARAMS);

	/*
	 * Find out who reselected us.
	 */
	if (mr->fifo_count == 0) {
		printk(KERN_ERR "mesh: reselection but nothing in fifo?\n");
		ms->conn_tgt = ms->host->this_id;
		goto bogus;
	}
	/* get the last byte in the fifo */
	do {
		b = in_8(&mr->fifo);
		dlog(ms, "reseldata %x", b);
	} while (in_8(&mr->fifo_count));
	for (t = 0; t < 8; ++t)
		if ((b & (1 << t)) != 0 && t != ms->host->this_id)
			break;
	if (b != (1 << t) + (1 << ms->host->this_id)) {
		printk(KERN_ERR "mesh: bad reselection data %x\n", b);
		ms->conn_tgt = ms->host->this_id;
		goto bogus;
	}


	/*
	 * Set up to continue with that target's transfer.
	 */
	ms->conn_tgt = t;
	tp = &ms->tgts[t];
	out_8(&mr->sync_params, tp->sync_params);
	if (ALLOW_DEBUG(t)) {
		printk(KERN_DEBUG "mesh: reselected by target %d\n", t);
		printk(KERN_DEBUG "mesh: saved_ptr=%x goes_out=%d cmd=%p\n",
		       tp->saved_ptr, tp->data_goes_out, tp->current_req);
	}
	ms->current_req = tp->current_req;
	if (tp->current_req == NULL) {
		printk(KERN_ERR "mesh: reselected by tgt %d but no cmd!\n", t);
		goto bogus;
	}
	ms->data_ptr = tp->saved_ptr;
	dlog(ms, "resel prev tgt=%d", prev);
	dlog(ms, "resel err/exc=%.4x", MKWORD(0, 0, mr->error, mr->exception));
	start_phase(ms);
	return;

bogus:
	dumplog(ms, ms->conn_tgt);
	dumpslog(ms);
	ms->data_ptr = 0;
	ms->aborting = 1;
	start_phase(ms);
}

static void do_abort(struct mesh_state *ms)
{
	ms->msgout[0] = ABORT;
	ms->n_msgout = 1;
	ms->aborting = 1;
	ms->stat = DID_ABORT;
	dlog(ms, "abort", 0);
}

static void
handle_reset(struct mesh_state *ms)
{
	int tgt;
	struct mesh_target *tp;
	Scsi_Cmnd *cmd;
	volatile struct mesh_regs *mr = ms->mesh;

	for (tgt = 0; tgt < 8; ++tgt) {
		tp = &ms->tgts[tgt];
		if ((cmd = tp->current_req) != NULL) {
			cmd->result = DID_RESET << 16;
			tp->current_req = NULL;
			mesh_completed(ms, cmd);
		}
		ms->tgts[tgt].sdtr_state = do_sdtr;
		ms->tgts[tgt].sync_params = ASYNC_PARAMS;
	}
	ms->current_req = NULL;
	while ((cmd = ms->request_q) != NULL) {
		ms->request_q = (Scsi_Cmnd *) cmd->host_scribble;
		cmd->result = DID_RESET << 16;
		mesh_completed(ms, cmd);
	}
	ms->phase = idle;
	ms->msgphase = msg_none;
	out_8(&mr->interrupt, INT_ERROR | INT_EXCEPTION | INT_CMDDONE);
	out_8(&mr->sequence, SEQ_FLUSHFIFO);
	udelay(1);
	out_8(&mr->sync_params, ASYNC_PARAMS);
	out_8(&mr->sequence, SEQ_ENBRESEL);
}

static void
do_mesh_interrupt(int irq, void *dev_id, struct pt_regs *ptregs)
{
	unsigned long flags;

	spin_lock_irqsave(&io_request_lock, flags);
	mesh_interrupt(irq, dev_id, ptregs);
	spin_unlock_irqrestore(&io_request_lock, flags);
}

static void handle_error(struct mesh_state *ms)
{
	int err, exc, count;
	volatile struct mesh_regs *mr = ms->mesh;

	err = in_8(&mr->error);
	exc = in_8(&mr->exception);
	out_8(&mr->interrupt, INT_ERROR | INT_EXCEPTION | INT_CMDDONE);
	dlog(ms, "error err/exc/fc/cl=%.8x",
	     MKWORD(err, exc, mr->fifo_count, mr->count_lo));
	if (err & ERR_SCSIRESET) {
		/* SCSI bus was reset */
		printk(KERN_INFO "mesh: SCSI bus reset detected: "
		       "waiting for end...");
		while ((mr->bus_status1 & BS1_RST) != 0)
			udelay(1);
		printk("done\n");
		handle_reset(ms);
		/* request_q is empty, no point in mesh_start() */
		return;
	}
	if (err & ERR_UNEXPDISC) {
		/* Unexpected disconnect */
		if (exc & EXC_RESELECTED) {
			reselected(ms);
			return;
		}
		if (!ms->aborting) {
			printk(KERN_WARNING "mesh: target %d aborted\n",
			       ms->conn_tgt);
			dumplog(ms, ms->conn_tgt);
			dumpslog(ms);
		}
		out_8(&mr->interrupt, INT_CMDDONE);
		ms->stat = DID_ABORT;
		mesh_done(ms, 1);
		return;
	}
	if (err & ERR_PARITY) {
		if (ms->msgphase == msg_in) {
			printk(KERN_ERR "mesh: msg parity error, target %d\n",
			       ms->conn_tgt);
			ms->msgout[0] = MSG_PARITY_ERROR;
			ms->n_msgout = 1;
			ms->msgphase = msg_in_bad;
			cmd_complete(ms);
			return;
		}
		if (ms->stat == DID_OK) {
			printk(KERN_ERR "mesh: parity error, target %d\n",
			       ms->conn_tgt);
			ms->stat = DID_PARITY;
		}
		count = (mr->count_hi << 8) + mr->count_lo;
		if (count == 0) {
			cmd_complete(ms);
		} else {
			/* reissue the data transfer command */
			out_8(&mr->sequence, mr->sequence);
		}
		return;
	}
	if (err & ERR_SEQERR) {
		if (exc & EXC_RESELECTED) {
			/* This can happen if we issue a command to
			   get the bus just after the target reselects us. */
			static int mesh_resel_seqerr;
			mesh_resel_seqerr++;
			reselected(ms);
			return;
		}
		if (exc == EXC_PHASEMM) {
			static int mesh_phasemm_seqerr;
			mesh_phasemm_seqerr++;
			phase_mismatch(ms);
			return;
		}
		printk(KERN_ERR "mesh: sequence error (err=%x exc=%x)\n",
		       err, exc);
	} else {
		printk(KERN_ERR "mesh: unknown error %x (exc=%x)\n", err, exc);
	}
	mesh_dump_regs(ms);
	dumplog(ms, ms->conn_tgt);
	if (ms->phase > selecting && (mr->bus_status1 & BS1_BSY)) {
		/* try to do what the target wants */
		do_abort(ms);
		phase_mismatch(ms);
		return;
	}
	ms->stat = DID_ERROR;
	mesh_done(ms, 1);
}

static void handle_exception(struct mesh_state *ms)
{
	int exc;
	volatile struct mesh_regs *mr = ms->mesh;

	exc = in_8(&mr->exception);
	out_8(&mr->interrupt, INT_EXCEPTION | INT_CMDDONE);
	if (exc & EXC_RESELECTED) {
		static int mesh_resel_exc;
		mesh_resel_exc++;
		reselected(ms);
	} else if (exc == EXC_ARBLOST) {
		printk(KERN_DEBUG "mesh: lost arbitration\n");
		ms->stat = DID_BUS_BUSY;
		mesh_done(ms, 1);
	} else if (exc == EXC_SELTO) {
		/* selection timed out */
		ms->stat = DID_BAD_TARGET;
		mesh_done(ms, 1);
	} else if (exc == EXC_PHASEMM) {
		/* target wants to do something different:
		   find out what it wants and do it. */
		phase_mismatch(ms);
	} else {
		printk(KERN_ERR "mesh: can't cope with exception %x\n", exc);
		mesh_dump_regs(ms);
		dumplog(ms, ms->conn_tgt);
		do_abort(ms);
		phase_mismatch(ms);
	}
}

static void
mesh_interrupt(int irq, void *dev_id, struct pt_regs *ptregs)
{
	struct mesh_state *ms = (struct mesh_state *) dev_id;
	volatile struct mesh_regs *mr = ms->mesh;
	int intr;

#if 0
	if (ALLOW_DEBUG(ms->conn_tgt))
		printk(KERN_DEBUG "mesh_intr, bs0=%x int=%x exc=%x err=%x "
		       "phase=%d msgphase=%d\n", mr->bus_status0,
		       mr->interrupt, mr->exception, mr->error,
		       ms->phase, ms->msgphase);
#endif
	while ((intr = in_8(&mr->interrupt)) != 0) {
		dlog(ms, "interrupt intr/err/exc/seq=%.8x", 
		     MKWORD(intr, mr->error, mr->exception, mr->sequence));
		if (intr & INT_ERROR) {
			handle_error(ms);
		} else if (intr & INT_EXCEPTION) {
			handle_exception(ms);
		} else if (intr & INT_CMDDONE) {
			out_8(&mr->interrupt, INT_CMDDONE);
			cmd_complete(ms);
		}
	}
}

static void
handle_msgin(struct mesh_state *ms)
{
	int i, code;
	Scsi_Cmnd *cmd = ms->current_req;
	struct mesh_target *tp = &ms->tgts[ms->conn_tgt];

	if (ms->n_msgin == 0)
		return;
	code = ms->msgin[0];
	if (ALLOW_DEBUG(ms->conn_tgt)) {
		printk(KERN_DEBUG "got %d message bytes:", ms->n_msgin);
		for (i = 0; i < ms->n_msgin; ++i)
			printk(" %x", ms->msgin[i]);
		printk("\n");
	}
	dlog(ms, "msgin msg=%.8x",
	     MKWORD(ms->n_msgin, code, ms->msgin[1], ms->msgin[2]));

	ms->expect_reply = 0;
	ms->n_msgout = 0;
	if (ms->n_msgin < msgin_length(ms))
		goto reject;
	if (cmd)
		cmd->SCp.Message = code;
	switch (code) {
	case COMMAND_COMPLETE:
		break;
	case EXTENDED_MESSAGE:
		switch (ms->msgin[2]) {
		case EXTENDED_MODIFY_DATA_POINTER:
			ms->data_ptr += (ms->msgin[3] << 24) + ms->msgin[6]
				+ (ms->msgin[4] << 16) + (ms->msgin[5] << 8);
			break;
		case EXTENDED_SDTR:
			if (tp->sdtr_state != sdtr_sent) {
				/* reply with an SDTR */
				add_sdtr_msg(ms);
				/* limit period to at least his value,
				   offset to no more than his */
				if (ms->msgout[3] < ms->msgin[3])
					ms->msgout[3] = ms->msgin[3];
				if (ms->msgout[4] > ms->msgin[4])
					ms->msgout[4] = ms->msgin[4];
				set_sdtr(ms, ms->msgout[3], ms->msgout[4]);
				ms->msgphase = msg_out;
			} else {
				set_sdtr(ms, ms->msgin[3], ms->msgin[4]);
			}
			break;
		default:
			goto reject;
		}
		break;
	case SAVE_POINTERS:
		tp->saved_ptr = ms->data_ptr;
		break;
	case RESTORE_POINTERS:
		ms->data_ptr = tp->saved_ptr;
		break;
	case DISCONNECT:
		ms->phase = disconnecting;
		break;
	case ABORT:
		break;
	case MESSAGE_REJECT:
		if (tp->sdtr_state == sdtr_sent)
			set_sdtr(ms, 0, 0);
		break;
	case NOP:
		break;
	default:
		if (IDENTIFY_BASE <= code && code <= IDENTIFY_BASE + 7) {
			if (cmd == NULL) {
				do_abort(ms);
				ms->msgphase = msg_out;
			} else if (code != cmd->lun + IDENTIFY_BASE) {
				printk(KERN_WARNING "mesh: lun mismatch "
				       "(%d != %d) on reselection from "
				       "target %d\n", i, cmd->lun,
				       ms->conn_tgt);
			}
			break;
		}
		goto reject;
	}
	return;

 reject:
	printk(KERN_WARNING "mesh: rejecting message from target %d:",
	       ms->conn_tgt);
	for (i = 0; i < ms->n_msgin; ++i)
		printk(" %x", ms->msgin[i]);
	printk("\n");
	ms->msgout[0] = MESSAGE_REJECT;
	ms->n_msgout = 1;
	ms->msgphase = msg_out;
}

static void
mesh_done(struct mesh_state *ms, int start_next)
{
	Scsi_Cmnd *cmd;
	struct mesh_target *tp = &ms->tgts[ms->conn_tgt];

	cmd = ms->current_req;
	ms->current_req = 0;
	tp->current_req = 0;
	if (cmd) {
		cmd->result = (ms->stat << 16) + cmd->SCp.Status;
		if (ms->stat == DID_OK)
			cmd->result += (cmd->SCp.Message << 8);
		if (DEBUG_TARGET(cmd)) {
			printk(KERN_DEBUG "mesh_done: result = %x, data_ptr=%d, buflen=%d\n",
			       cmd->result, ms->data_ptr, cmd->request_bufflen);
			if ((cmd->cmnd[0] == 0 || cmd->cmnd[0] == 0x12 || cmd->cmnd[0] == 3)
			    && cmd->request_buffer != 0) {
				unsigned char *b = cmd->request_buffer;
				printk(KERN_DEBUG "buffer = %x %x %x %x %x %x %x %x\n",
				       b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
			}
		}
		cmd->SCp.this_residual -= ms->data_ptr;
		mesh_completed(ms, cmd);
	}
	if (start_next) {
		out_8(&ms->mesh->sequence, SEQ_ENBRESEL);
		udelay(1);
		ms->phase = idle;
		mesh_start(ms);
	}
}

static void
mesh_completed(struct mesh_state *ms, Scsi_Cmnd *cmd)
{
#ifdef MESH_NEW_STYLE_EH
	(*cmd->scsi_done)(cmd);
#else
	if (ms->completed_q == NULL)
		ms->completed_q = cmd;
	else
		ms->completed_qtail->host_scribble = (void *) cmd;
	ms->completed_qtail = cmd;
	cmd->host_scribble = NULL;
	queue_task(&ms->tqueue, &tq_immediate);
	mark_bh(IMMEDIATE_BH);
#endif /* MESH_NEW_STYLE_EH */
}

/*
 * Set up DMA commands for transferring data.
 */
static void
set_dma_cmds(struct mesh_state *ms, Scsi_Cmnd *cmd)
{
	int i, dma_cmd, total, off, dtot;
	struct scatterlist *scl;
	struct dbdma_cmd *dcmds;

	dma_cmd = ms->tgts[ms->conn_tgt].data_goes_out?
		OUTPUT_MORE: INPUT_MORE;
	dcmds = ms->dma_cmds;
	dtot = 0;
	if (cmd) {
		cmd->SCp.this_residual = cmd->request_bufflen;
		if (cmd->use_sg > 0) {
			total = 0;
			scl = (struct scatterlist *) cmd->buffer;
			off = ms->data_ptr;
			for (i = 0; i < cmd->use_sg; ++i, ++scl) {
				total += scl->length;
				if (off >= scl->length) {
					off -= scl->length;
					continue;
				}
				if (scl->length > 0xffff)
					panic("mesh: scatterlist element >= 64k");
				st_le16(&dcmds->req_count, scl->length - off);
				st_le16(&dcmds->command, dma_cmd);
				st_le32(&dcmds->phy_addr,
					virt_to_phys(scl->address) + off);
				dcmds->xfer_status = 0;
				++dcmds;
				dtot += scl->length - off;
				off = 0;
			}
		} else if (ms->data_ptr < cmd->request_bufflen) {
			dtot = cmd->request_bufflen - ms->data_ptr;
			if (dtot > 0xffff)
				panic("mesh: transfer size >= 64k");
			st_le16(&dcmds->req_count, dtot);
			st_le32(&dcmds->phy_addr,
				virt_to_phys(cmd->request_buffer) + ms->data_ptr);
			dcmds->xfer_status = 0;
			++dcmds;
		}
	}
	if (dtot == 0) {
		/* Either the target has overrun our buffer,
		   or the caller didn't provide a buffer. */
		static char mesh_extra_buf[64];

		dtot = sizeof(mesh_extra_buf);
		st_le16(&dcmds->req_count, dtot);
		st_le32(&dcmds->phy_addr, virt_to_phys(mesh_extra_buf));
		dcmds->xfer_status = 0;
		++dcmds;
	}
	dma_cmd += OUTPUT_LAST - OUTPUT_MORE;
	st_le16(&dcmds[-1].command, dma_cmd);
	memset(dcmds, 0, sizeof(*dcmds));
	st_le16(&dcmds->command, DBDMA_STOP);
	ms->dma_count = dtot;
}

static void
halt_dma(struct mesh_state *ms)
{
	volatile struct dbdma_regs *md = ms->dma;
	volatile struct mesh_regs *mr = ms->mesh;
	Scsi_Cmnd *cmd = ms->current_req;
	int t, nb;

	if (!ms->tgts[ms->conn_tgt].data_goes_out) {
		/* wait a little while until the fifo drains */
		t = 50;
		while (t > 0 && mr->fifo_count != 0
		       && (in_le32(&md->status) & ACTIVE) != 0) {
			--t;
			udelay(1);
		}
	}
	out_le32(&md->control, RUN << 16);	/* turn off RUN bit */
	nb = (mr->count_hi << 8) + mr->count_lo;
	dlog(ms, "halt_dma fc/count=%.6x",
	     MKWORD(0, mr->fifo_count, 0, nb));
	if (ms->tgts[ms->conn_tgt].data_goes_out)
		nb += mr->fifo_count;
	/* nb is the number of bytes not yet transferred
	   to/from the target. */
	ms->data_ptr -= nb;
	dlog(ms, "data_ptr %x", ms->data_ptr);
	if (ms->data_ptr < 0) {
		printk(KERN_ERR "mesh: halt_dma: data_ptr=%d (nb=%d, ms=%p)\n",
		       ms->data_ptr, nb, ms);
		ms->data_ptr = 0;
#ifdef MESH_DBG
		dumplog(ms, ms->conn_tgt);
		dumpslog(ms);
#endif /* MESH_DBG */
	} else if (cmd && cmd->request_bufflen != 0 &&
		   ms->data_ptr > cmd->request_bufflen) {
		printk(KERN_DEBUG "mesh: target %d overrun, "
		       "data_ptr=%x total=%x goes_out=%d\n",
		       ms->conn_tgt, ms->data_ptr, cmd->request_bufflen,
		       ms->tgts[ms->conn_tgt].data_goes_out);
	}
	ms->dma_started = 0;
}

/*
 * Work out whether we expect data to go out from the host adaptor or into it.
 * (If this information is available from somewhere else in the scsi
 * code, somebody please let me know :-)
 */
static int
data_goes_out(Scsi_Cmnd *cmd)
{
	switch (cmd->cmnd[0]) {
	case CHANGE_DEFINITION: 
	case COMPARE:	  
	case COPY:
	case COPY_VERIFY:	    
	case FORMAT_UNIT:	 
	case LOG_SELECT:
	case MEDIUM_SCAN:	  
	case MODE_SELECT:
	case MODE_SELECT_10:
	case REASSIGN_BLOCKS: 
	case RESERVE:
	case SEARCH_EQUAL:	  
	case SEARCH_EQUAL_12: 
	case SEARCH_HIGH:	 
	case SEARCH_HIGH_12:  
	case SEARCH_LOW:
	case SEARCH_LOW_12:
	case SEND_DIAGNOSTIC: 
	case SEND_VOLUME_TAG:	     
	case SET_WINDOW: 
	case UPDATE_BLOCK:	
	case WRITE_BUFFER:
 	case WRITE_6:	
	case WRITE_10:	
	case WRITE_12:	  
	case WRITE_LONG:	
	case WRITE_LONG_2:      /* alternate code for WRITE_LONG */
	case WRITE_SAME:	
	case WRITE_VERIFY:
	case WRITE_VERIFY_12:
		return 1;
	default:
		return 0;
	}
}

#ifdef MESH_DBG
static inline u32 readtb(void)
{
	u32 tb;

#ifdef DBG_USE_TB
	/* Beware: if you enable this, it will crash on 601s. */
	asm ("mftb %0" : "=r" (tb) : );
#else
	tb = 0;
#endif
	return tb;
}

static void dlog(struct mesh_state *ms, char *fmt, int a)
{
	struct mesh_target *tp = &ms->tgts[ms->conn_tgt];
	struct dbglog *tlp, *slp;

	tlp = &tp->log[tp->log_ix];
	slp = &ms->log[ms->log_ix];
	tlp->fmt = fmt;
	tlp->tb = readtb();
	tlp->phase = (ms->msgphase << 4) + ms->phase;
	tlp->bs0 = ms->mesh->bus_status0;
	tlp->bs1 = ms->mesh->bus_status1;
	tlp->tgt = ms->conn_tgt;
	tlp->d = a;
	*slp = *tlp;
	if (++tp->log_ix >= N_DBG_LOG)
		tp->log_ix = 0;
	if (tp->n_log < N_DBG_LOG)
		++tp->n_log;
	if (++ms->log_ix >= N_DBG_SLOG)
		ms->log_ix = 0;
	if (ms->n_log < N_DBG_SLOG)
		++ms->n_log;
}

static void dumplog(struct mesh_state *ms, int t)
{
	struct mesh_target *tp = &ms->tgts[t];
	struct dbglog *lp;
	int i;

	if (tp->n_log == 0)
		return;
	i = tp->log_ix - tp->n_log;
	if (i < 0)
		i += N_DBG_LOG;
	tp->n_log = 0;
	do {
		lp = &tp->log[i];
		printk(KERN_DEBUG "mesh log %d: bs=%.2x%.2x ph=%.2x ",
		       t, lp->bs1, lp->bs0, lp->phase);
#ifdef DBG_USE_TB
		printk("tb=%10u ", lp->tb);
#endif
		printk(lp->fmt, lp->d);
		printk("\n");
		if (++i >= N_DBG_LOG)
			i = 0;
	} while (i != tp->log_ix);
}

static void dumpslog(struct mesh_state *ms)
{
	struct dbglog *lp;
	int i;

	if (ms->n_log == 0)
		return;
	i = ms->log_ix - ms->n_log;
	if (i < 0)
		i += N_DBG_SLOG;
	ms->n_log = 0;
	do {
		lp = &ms->log[i];
		printk(KERN_DEBUG "mesh log: bs=%.2x%.2x ph=%.2x t%d ",
		       lp->bs1, lp->bs0, lp->phase, lp->tgt);
#ifdef DBG_USE_TB
		printk("tb=%10u ", lp->tb);
#endif
		printk(lp->fmt, lp->d);
		printk("\n");
		if (++i >= N_DBG_SLOG)
			i = 0;
	} while (i != ms->log_ix);
}
#endif /* MESH_DBG */

static Scsi_Host_Template driver_template = SCSI_MESH;

#include "scsi_module.c"
