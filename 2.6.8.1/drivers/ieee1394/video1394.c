/*
 * video1394.c - video driver for OHCI 1394 boards
 * Copyright (C)1999,2000 Sebastien Rougeaux <sebastien.rougeaux@anu.edu.au>
 *                        Peter Schlaile <udbz@rz.uni-karlsruhe.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/* jds -- add private data to file to keep track of iso contexts associated
   with each open -- so release won't kill all iso transfers */

/* Damien Douxchamps: Fix failure when the number of DMA pages per frame is
   one */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/smp_lock.h>
#include <linux/delay.h>
#include <linux/devfs_fs_kernel.h>
#include <linux/bitops.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/timex.h>
#include <linux/mm.h>
#include <linux/ioctl32.h>
#include <linux/compat.h>
#include <linux/cdev.h>

#include "ieee1394.h"
#include "ieee1394_types.h"
#include "hosts.h"
#include "ieee1394_core.h"
#include "highlevel.h"
#include "video1394.h"
#include "nodemgr.h"
#include "dma.h"

#include "ohci1394.h"

#define ISO_CHANNELS 64

#ifndef virt_to_page
#define virt_to_page(x) MAP_NR(x)
#endif

#ifndef vmalloc_32
#define vmalloc_32(x) vmalloc(x)
#endif

struct it_dma_prg {
	struct dma_cmd begin;
	quadlet_t data[4];
	struct dma_cmd end;
	quadlet_t pad[4]; /* FIXME: quick hack for memory alignment */
};

struct dma_iso_ctx {
	struct ti_ohci *ohci;
	int type; /* OHCI_ISO_TRANSMIT or OHCI_ISO_RECEIVE */
	struct ohci1394_iso_tasklet iso_tasklet;
	int channel;
	int ctx;
	int last_buffer;
	int * next_buffer;  /* For ISO Transmit of video packets
			       to write the correct SYT field
			       into the next block */
	unsigned int num_desc;
	unsigned int buf_size;
	unsigned int frame_size;
	unsigned int packet_size;
	unsigned int left_size;
	unsigned int nb_cmd;

	struct dma_region dma;

	struct dma_prog_region *prg_reg;

        struct dma_cmd **ir_prg;
	struct it_dma_prg **it_prg;

	unsigned int *buffer_status;
        struct timeval *buffer_time; /* time when the buffer was received */
	unsigned int *last_used_cmd; /* For ISO Transmit with
					variable sized packets only ! */
	int ctrlClear;
	int ctrlSet;
	int cmdPtr;
	int ctxMatch;
	wait_queue_head_t waitq;
	spinlock_t lock;
	unsigned int syt_offset;
	int flags;

	struct list_head link;
};


struct file_ctx {
	struct ti_ohci *ohci;
	struct list_head context_list;
	struct dma_iso_ctx *current_ctx;
};

#ifdef CONFIG_IEEE1394_VERBOSEDEBUG
#define VIDEO1394_DEBUG
#endif

#ifdef DBGMSG
#undef DBGMSG
#endif

#ifdef VIDEO1394_DEBUG
#define DBGMSG(card, fmt, args...) \
printk(KERN_INFO "video1394_%d: " fmt "\n" , card , ## args)
#else
#define DBGMSG(card, fmt, args...)
#endif

/* print general (card independent) information */
#define PRINT_G(level, fmt, args...) \
printk(level "video1394: " fmt "\n" , ## args)

/* print card specific information */
#define PRINT(level, card, fmt, args...) \
printk(level "video1394_%d: " fmt "\n" , card , ## args)

void wakeup_dma_ir_ctx(unsigned long l);
void wakeup_dma_it_ctx(unsigned long l);

static struct hpsb_highlevel video1394_highlevel;

static int free_dma_iso_ctx(struct dma_iso_ctx *d)
{
	int i;

	DBGMSG(d->ohci->host->id, "Freeing dma_iso_ctx %d", d->ctx);

	ohci1394_stop_context(d->ohci, d->ctrlClear, NULL);
	if (d->iso_tasklet.link.next != NULL)
		ohci1394_unregister_iso_tasklet(d->ohci, &d->iso_tasklet);

	dma_region_free(&d->dma);

	if (d->prg_reg) {
		for (i = 0; i < d->num_desc; i++)
			dma_prog_region_free(&d->prg_reg[i]);
		kfree(d->prg_reg);
	}

	if (d->ir_prg)
		kfree(d->ir_prg);

	if (d->it_prg)
		kfree(d->it_prg);

	if (d->buffer_status)
		kfree(d->buffer_status);
	if (d->buffer_time)
		kfree(d->buffer_time);
	if (d->last_used_cmd)
		kfree(d->last_used_cmd);
	if (d->next_buffer)
		kfree(d->next_buffer);

	list_del(&d->link);

	kfree(d);

	return 0;
}

static struct dma_iso_ctx *
alloc_dma_iso_ctx(struct ti_ohci *ohci, int type, int num_desc,
		  int buf_size, int channel, unsigned int packet_size)
{
	struct dma_iso_ctx *d;
	int i;

	d = kmalloc(sizeof(struct dma_iso_ctx), GFP_KERNEL);
	if (d == NULL) {
		PRINT(KERN_ERR, ohci->host->id, "Failed to allocate dma_iso_ctx");
		return NULL;
	}

	memset(d, 0, sizeof *d);

	d->ohci = ohci;
	d->type = type;
	d->channel = channel;
	d->num_desc = num_desc;
	d->frame_size = buf_size;
	d->buf_size = PAGE_ALIGN(buf_size);
	d->last_buffer = -1;
	INIT_LIST_HEAD(&d->link);
	init_waitqueue_head(&d->waitq);

	/* Init the regions for easy cleanup */
	dma_region_init(&d->dma);

	if (dma_region_alloc(&d->dma, d->num_desc * d->buf_size, ohci->dev,
			     PCI_DMA_BIDIRECTIONAL)) {
		PRINT(KERN_ERR, ohci->host->id, "Failed to allocate dma buffer");
		free_dma_iso_ctx(d);
		return NULL;
	}

	if (type == OHCI_ISO_RECEIVE)
		ohci1394_init_iso_tasklet(&d->iso_tasklet, type,
					  wakeup_dma_ir_ctx,
					  (unsigned long) d);
	else
		ohci1394_init_iso_tasklet(&d->iso_tasklet, type,
					  wakeup_dma_it_ctx,
					  (unsigned long) d);

	if (ohci1394_register_iso_tasklet(ohci, &d->iso_tasklet) < 0) {
		PRINT(KERN_ERR, ohci->host->id, "no free iso %s contexts",
		      type == OHCI_ISO_RECEIVE ? "receive" : "transmit");
		free_dma_iso_ctx(d);
		return NULL;
	}
	d->ctx = d->iso_tasklet.context;

	d->prg_reg = kmalloc(d->num_desc * sizeof(struct dma_prog_region),
			GFP_KERNEL);
	if (d->prg_reg == NULL) {
		PRINT(KERN_ERR, ohci->host->id, "Failed to allocate ir prg regs");
		free_dma_iso_ctx(d);
		return NULL;
	}
	/* Makes for easier cleanup */
	for (i = 0; i < d->num_desc; i++)
		dma_prog_region_init(&d->prg_reg[i]);

	if (type == OHCI_ISO_RECEIVE) {
		d->ctrlSet = OHCI1394_IsoRcvContextControlSet+32*d->ctx;
		d->ctrlClear = OHCI1394_IsoRcvContextControlClear+32*d->ctx;
		d->cmdPtr = OHCI1394_IsoRcvCommandPtr+32*d->ctx;
		d->ctxMatch = OHCI1394_IsoRcvContextMatch+32*d->ctx;

		d->ir_prg = kmalloc(d->num_desc * sizeof(struct dma_cmd *),
				    GFP_KERNEL);

		if (d->ir_prg == NULL) {
			PRINT(KERN_ERR, ohci->host->id, "Failed to allocate dma ir prg");
			free_dma_iso_ctx(d);
			return NULL;
		}
		memset(d->ir_prg, 0, d->num_desc * sizeof(struct dma_cmd *));

		d->nb_cmd = d->buf_size / PAGE_SIZE + 1;
		d->left_size = (d->frame_size % PAGE_SIZE) ?
			d->frame_size % PAGE_SIZE : PAGE_SIZE;

		for (i = 0;i < d->num_desc; i++) {
			if (dma_prog_region_alloc(&d->prg_reg[i], d->nb_cmd *
						  sizeof(struct dma_cmd), ohci->dev)) {
				PRINT(KERN_ERR, ohci->host->id, "Failed to allocate dma ir prg");
				free_dma_iso_ctx(d);
				return NULL;
			}
			d->ir_prg[i] = (struct dma_cmd *)d->prg_reg[i].kvirt;
		}

	} else {  /* OHCI_ISO_TRANSMIT */
		d->ctrlSet = OHCI1394_IsoXmitContextControlSet+16*d->ctx;
		d->ctrlClear = OHCI1394_IsoXmitContextControlClear+16*d->ctx;
		d->cmdPtr = OHCI1394_IsoXmitCommandPtr+16*d->ctx;

		d->it_prg = kmalloc(d->num_desc * sizeof(struct it_dma_prg *),
				    GFP_KERNEL);

		if (d->it_prg == NULL) {
			PRINT(KERN_ERR, ohci->host->id,
			      "Failed to allocate dma it prg");
			free_dma_iso_ctx(d);
			return NULL;
		}
		memset(d->it_prg, 0, d->num_desc*sizeof(struct it_dma_prg *));

		d->packet_size = packet_size;

		if (PAGE_SIZE % packet_size || packet_size>4096) {
			PRINT(KERN_ERR, ohci->host->id,
			      "Packet size %d (page_size: %ld) "
			      "not yet supported\n",
			      packet_size, PAGE_SIZE);
			free_dma_iso_ctx(d);
			return NULL;
		}

		d->nb_cmd = d->frame_size / d->packet_size;
		if (d->frame_size % d->packet_size) {
			d->nb_cmd++;
			d->left_size = d->frame_size % d->packet_size;
		} else
			d->left_size = d->packet_size;

		for (i = 0; i < d->num_desc; i++) {
			if (dma_prog_region_alloc(&d->prg_reg[i], d->nb_cmd *
						sizeof(struct it_dma_prg), ohci->dev)) {
				PRINT(KERN_ERR, ohci->host->id, "Failed to allocate dma it prg");
				free_dma_iso_ctx(d);
				return NULL;
			}
			d->it_prg[i] = (struct it_dma_prg *)d->prg_reg[i].kvirt;
		}
	}

	d->buffer_status = kmalloc(d->num_desc * sizeof(unsigned int),
				   GFP_KERNEL);
	d->buffer_time = kmalloc(d->num_desc * sizeof(struct timeval),
				   GFP_KERNEL);
	d->last_used_cmd = kmalloc(d->num_desc * sizeof(unsigned int),
				   GFP_KERNEL);
	d->next_buffer = kmalloc(d->num_desc * sizeof(int),
				 GFP_KERNEL);

	if (d->buffer_status == NULL) {
		PRINT(KERN_ERR, ohci->host->id, "Failed to allocate buffer_status");
		free_dma_iso_ctx(d);
		return NULL;
	}
	if (d->buffer_time == NULL) {
		PRINT(KERN_ERR, ohci->host->id, "Failed to allocate buffer_time");
		free_dma_iso_ctx(d);
		return NULL;
	}
	if (d->last_used_cmd == NULL) {
		PRINT(KERN_ERR, ohci->host->id, "Failed to allocate last_used_cmd");
		free_dma_iso_ctx(d);
		return NULL;
	}
	if (d->next_buffer == NULL) {
		PRINT(KERN_ERR, ohci->host->id, "Failed to allocate next_buffer");
		free_dma_iso_ctx(d);
		return NULL;
	}
	memset(d->buffer_status, 0, d->num_desc * sizeof(unsigned int));
	memset(d->buffer_time, 0, d->num_desc * sizeof(struct timeval));
	memset(d->last_used_cmd, 0, d->num_desc * sizeof(unsigned int));
	memset(d->next_buffer, -1, d->num_desc * sizeof(int));

        spin_lock_init(&d->lock);

	PRINT(KERN_INFO, ohci->host->id, "Iso %s DMA: %d buffers "
	      "of size %d allocated for a frame size %d, each with %d prgs",
	      (type == OHCI_ISO_RECEIVE) ? "receive" : "transmit",
	      d->num_desc, d->buf_size, d->frame_size, d->nb_cmd);

	return d;
}

static void reset_ir_status(struct dma_iso_ctx *d, int n)
{
	int i;
	d->ir_prg[n][0].status = cpu_to_le32(4);
	d->ir_prg[n][1].status = cpu_to_le32(PAGE_SIZE-4);
	for (i = 2; i < d->nb_cmd - 1; i++)
		d->ir_prg[n][i].status = cpu_to_le32(PAGE_SIZE);
	d->ir_prg[n][i].status = cpu_to_le32(d->left_size);
}

static void initialize_dma_ir_prg(struct dma_iso_ctx *d, int n, int flags)
{
	struct dma_cmd *ir_prg = d->ir_prg[n];
	struct dma_prog_region *ir_reg = &d->prg_reg[n];
	unsigned long buf = (unsigned long)d->dma.kvirt + n * d->buf_size;
	int i;

	/* the first descriptor will read only 4 bytes */
	ir_prg[0].control = cpu_to_le32(DMA_CTL_INPUT_MORE | DMA_CTL_UPDATE |
		DMA_CTL_BRANCH | 4);

	/* set the sync flag */
	if (flags & VIDEO1394_SYNC_FRAMES)
		ir_prg[0].control |= cpu_to_le32(DMA_CTL_WAIT);

	ir_prg[0].address = cpu_to_le32(dma_region_offset_to_bus(&d->dma, buf -
				(unsigned long)d->dma.kvirt));
	ir_prg[0].branchAddress = cpu_to_le32((dma_prog_region_offset_to_bus(ir_reg,
					1 * sizeof(struct dma_cmd)) & 0xfffffff0) | 0x1);

	/* If there is *not* only one DMA page per frame (hence, d->nb_cmd==2) */
	if (d->nb_cmd > 2) {
		/* The second descriptor will read PAGE_SIZE-4 bytes */
		ir_prg[1].control = cpu_to_le32(DMA_CTL_INPUT_MORE | DMA_CTL_UPDATE |
						DMA_CTL_BRANCH | (PAGE_SIZE-4));
		ir_prg[1].address = cpu_to_le32(dma_region_offset_to_bus(&d->dma, (buf + 4) -
						(unsigned long)d->dma.kvirt));
		ir_prg[1].branchAddress = cpu_to_le32((dma_prog_region_offset_to_bus(ir_reg,
						      2 * sizeof(struct dma_cmd)) & 0xfffffff0) | 0x1);

		for (i = 2; i < d->nb_cmd - 1; i++) {
			ir_prg[i].control = cpu_to_le32(DMA_CTL_INPUT_MORE | DMA_CTL_UPDATE |
							DMA_CTL_BRANCH | PAGE_SIZE);
			ir_prg[i].address = cpu_to_le32(dma_region_offset_to_bus(&d->dma,
							(buf+(i-1)*PAGE_SIZE) -
							(unsigned long)d->dma.kvirt));

			ir_prg[i].branchAddress =
				cpu_to_le32((dma_prog_region_offset_to_bus(ir_reg,
					    (i + 1) * sizeof(struct dma_cmd)) & 0xfffffff0) | 0x1);
		}

		/* The last descriptor will generate an interrupt */
		ir_prg[i].control = cpu_to_le32(DMA_CTL_INPUT_MORE | DMA_CTL_UPDATE |
						DMA_CTL_IRQ | DMA_CTL_BRANCH | d->left_size);
		ir_prg[i].address = cpu_to_le32(dma_region_offset_to_bus(&d->dma,
						(buf+(i-1)*PAGE_SIZE) -
						(unsigned long)d->dma.kvirt));
	} else {
		/* Only one DMA page is used. Read d->left_size immediately and */
		/* generate an interrupt as this is also the last page. */
		ir_prg[1].control = cpu_to_le32(DMA_CTL_INPUT_MORE | DMA_CTL_UPDATE |
						DMA_CTL_IRQ | DMA_CTL_BRANCH | (d->left_size-4));
		ir_prg[1].address = cpu_to_le32(dma_region_offset_to_bus(&d->dma,
						(buf + 4) - (unsigned long)d->dma.kvirt));
	}
}

static void initialize_dma_ir_ctx(struct dma_iso_ctx *d, int tag, int flags)
{
	struct ti_ohci *ohci = (struct ti_ohci *)d->ohci;
	int i;

	d->flags = flags;

	ohci1394_stop_context(ohci, d->ctrlClear, NULL);

	for (i=0;i<d->num_desc;i++) {
		initialize_dma_ir_prg(d, i, flags);
		reset_ir_status(d, i);
	}

	/* reset the ctrl register */
	reg_write(ohci, d->ctrlClear, 0xf0000000);

	/* Set bufferFill */
	reg_write(ohci, d->ctrlSet, 0x80000000);

	/* Set isoch header */
	if (flags & VIDEO1394_INCLUDE_ISO_HEADERS)
		reg_write(ohci, d->ctrlSet, 0x40000000);

	/* Set the context match register to match on all tags,
	   sync for sync tag, and listen to d->channel */
	reg_write(ohci, d->ctxMatch, 0xf0000000|((tag&0xf)<<8)|d->channel);

	/* Set up isoRecvIntMask to generate interrupts */
	reg_write(ohci, OHCI1394_IsoRecvIntMaskSet, 1<<d->ctx);
}

/* find which context is listening to this channel */
static struct dma_iso_ctx *
find_ctx(struct list_head *list, int type, int channel)
{
	struct dma_iso_ctx *ctx;

	list_for_each_entry(ctx, list, link) {
		if (ctx->type == type && ctx->channel == channel)
			return ctx;
	}

	return NULL;
}

void wakeup_dma_ir_ctx(unsigned long l)
{
	struct dma_iso_ctx *d = (struct dma_iso_ctx *) l;
	int i;

	spin_lock(&d->lock);

	for (i = 0; i < d->num_desc; i++) {
		if (d->ir_prg[i][d->nb_cmd-1].status & cpu_to_le32(0xFFFF0000)) {
			reset_ir_status(d, i);
			d->buffer_status[i] = VIDEO1394_BUFFER_READY;
			do_gettimeofday(&d->buffer_time[i]);
		}
	}

	spin_unlock(&d->lock);

	if (waitqueue_active(&d->waitq))
		wake_up_interruptible(&d->waitq);
}

static inline void put_timestamp(struct ti_ohci *ohci, struct dma_iso_ctx * d,
				 int n)
{
	unsigned char* buf = d->dma.kvirt + n * d->buf_size;
	u32 cycleTimer;
	u32 timeStamp;

	if (n == -1) {
	  return;
	}

	cycleTimer = reg_read(ohci, OHCI1394_IsochronousCycleTimer);

	timeStamp = ((cycleTimer & 0x0fff) + d->syt_offset); /* 11059 = 450 us */
	timeStamp = (timeStamp % 3072 + ((timeStamp / 3072) << 12)
		+ (cycleTimer & 0xf000)) & 0xffff;

	buf[6] = timeStamp >> 8;
	buf[7] = timeStamp & 0xff;

    /* if first packet is empty packet, then put timestamp into the next full one too */
    if ( (le32_to_cpu(d->it_prg[n][0].data[1]) >>16) == 0x008) {
   	    buf += d->packet_size;
    	buf[6] = timeStamp >> 8;
	    buf[7] = timeStamp & 0xff;
	}

    /* do the next buffer frame too in case of irq latency */
	n = d->next_buffer[n];
	if (n == -1) {
	  return;
	}
	buf = d->dma.kvirt + n * d->buf_size;

	timeStamp += (d->last_used_cmd[n] << 12) & 0xffff;

	buf[6] = timeStamp >> 8;
	buf[7] = timeStamp & 0xff;

    /* if first packet is empty packet, then put timestamp into the next full one too */
    if ( (le32_to_cpu(d->it_prg[n][0].data[1]) >>16) == 0x008) {
   	    buf += d->packet_size;
    	buf[6] = timeStamp >> 8;
	    buf[7] = timeStamp & 0xff;
	}

#if 0
	printk("curr: %d, next: %d, cycleTimer: %08x timeStamp: %08x\n",
	       curr, n, cycleTimer, timeStamp);
#endif
}

void wakeup_dma_it_ctx(unsigned long l)
{
	struct dma_iso_ctx *d = (struct dma_iso_ctx *) l;
	struct ti_ohci *ohci = d->ohci;
	int i;

	spin_lock(&d->lock);

	for (i = 0; i < d->num_desc; i++) {
		if (d->it_prg[i][d->last_used_cmd[i]].end.status &
		    cpu_to_le32(0xFFFF0000)) {
			int next = d->next_buffer[i];
			put_timestamp(ohci, d, next);
			d->it_prg[i][d->last_used_cmd[i]].end.status = 0;
			d->buffer_status[i] = VIDEO1394_BUFFER_READY;
		}
	}

	spin_unlock(&d->lock);

	if (waitqueue_active(&d->waitq))
		wake_up_interruptible(&d->waitq);
}

static void initialize_dma_it_prg(struct dma_iso_ctx *d, int n, int sync_tag)
{
	struct it_dma_prg *it_prg = d->it_prg[n];
	struct dma_prog_region *it_reg = &d->prg_reg[n];
	unsigned long buf = (unsigned long)d->dma.kvirt + n * d->buf_size;
	int i;
	d->last_used_cmd[n] = d->nb_cmd - 1;
	for (i=0;i<d->nb_cmd;i++) {

		it_prg[i].begin.control = cpu_to_le32(DMA_CTL_OUTPUT_MORE |
			DMA_CTL_IMMEDIATE | 8) ;
		it_prg[i].begin.address = 0;

		it_prg[i].begin.status = 0;

		it_prg[i].data[0] = cpu_to_le32(
			(IEEE1394_SPEED_100 << 16)
			| (/* tag */ 1 << 14)
			| (d->channel << 8)
			| (TCODE_ISO_DATA << 4));
		if (i==0) it_prg[i].data[0] |= cpu_to_le32(sync_tag);
		it_prg[i].data[1] = cpu_to_le32(d->packet_size << 16);
		it_prg[i].data[2] = 0;
		it_prg[i].data[3] = 0;

		it_prg[i].end.control = cpu_to_le32(DMA_CTL_OUTPUT_LAST |
			    	    	     DMA_CTL_BRANCH);
		it_prg[i].end.address =
			cpu_to_le32(dma_region_offset_to_bus(&d->dma, (buf+i*d->packet_size) -
						(unsigned long)d->dma.kvirt));

		if (i<d->nb_cmd-1) {
			it_prg[i].end.control |= cpu_to_le32(d->packet_size);
			it_prg[i].begin.branchAddress =
				cpu_to_le32((dma_prog_region_offset_to_bus(it_reg, (i + 1) *
					sizeof(struct it_dma_prg)) & 0xfffffff0) | 0x3);
			it_prg[i].end.branchAddress =
				cpu_to_le32((dma_prog_region_offset_to_bus(it_reg, (i + 1) *
					sizeof(struct it_dma_prg)) & 0xfffffff0) | 0x3);
		} else {
			/* the last prg generates an interrupt */
			it_prg[i].end.control |= cpu_to_le32(DMA_CTL_UPDATE |
				DMA_CTL_IRQ | d->left_size);
			/* the last prg doesn't branch */
			it_prg[i].begin.branchAddress = 0;
			it_prg[i].end.branchAddress = 0;
		}
		it_prg[i].end.status = 0;
	}
}

static void initialize_dma_it_prg_var_packet_queue(
	struct dma_iso_ctx *d, int n, unsigned int * packet_sizes,
	struct ti_ohci *ohci)
{
	struct it_dma_prg *it_prg = d->it_prg[n];
	struct dma_prog_region *it_reg = &d->prg_reg[n];
	int i;

#if 0
	if (n != -1) {
		put_timestamp(ohci, d, n);
	}
#endif
	d->last_used_cmd[n] = d->nb_cmd - 1;

	for (i = 0; i < d->nb_cmd; i++) {
		unsigned int size;
		if (packet_sizes[i] > d->packet_size) {
			size = d->packet_size;
		} else {
			size = packet_sizes[i];
		}
		it_prg[i].data[1] = cpu_to_le32(size << 16);
		it_prg[i].end.control = cpu_to_le32(DMA_CTL_OUTPUT_LAST | DMA_CTL_BRANCH);

		if (i < d->nb_cmd-1 && packet_sizes[i+1] != 0) {
			it_prg[i].end.control |= cpu_to_le32(size);
			it_prg[i].begin.branchAddress =
				cpu_to_le32((dma_prog_region_offset_to_bus(it_reg, (i + 1) *
					sizeof(struct it_dma_prg)) & 0xfffffff0) | 0x3);
			it_prg[i].end.branchAddress =
				cpu_to_le32((dma_prog_region_offset_to_bus(it_reg, (i + 1) *
					sizeof(struct it_dma_prg)) & 0xfffffff0) | 0x3);
		} else {
			/* the last prg generates an interrupt */
			it_prg[i].end.control |= cpu_to_le32(DMA_CTL_UPDATE |
				DMA_CTL_IRQ | size);
			/* the last prg doesn't branch */
			it_prg[i].begin.branchAddress = 0;
			it_prg[i].end.branchAddress = 0;
			d->last_used_cmd[n] = i;
			break;
		}
	}
}

static void initialize_dma_it_ctx(struct dma_iso_ctx *d, int sync_tag,
				  unsigned int syt_offset, int flags)
{
	struct ti_ohci *ohci = (struct ti_ohci *)d->ohci;
	int i;

	d->flags = flags;
	d->syt_offset = (syt_offset == 0 ? 11000 : syt_offset);

	ohci1394_stop_context(ohci, d->ctrlClear, NULL);

	for (i=0;i<d->num_desc;i++)
		initialize_dma_it_prg(d, i, sync_tag);

	/* Set up isoRecvIntMask to generate interrupts */
	reg_write(ohci, OHCI1394_IsoXmitIntMaskSet, 1<<d->ctx);
}

static int video1394_ioctl(struct inode *inode, struct file *file,
			   unsigned int cmd, unsigned long arg)
{
	struct file_ctx *ctx = (struct file_ctx *)file->private_data;
	struct ti_ohci *ohci = ctx->ohci;
	unsigned long flags;
	void __user *argp = (void __user *)arg;

	switch(cmd)
	{
	case VIDEO1394_IOC_LISTEN_CHANNEL:
	case VIDEO1394_IOC_TALK_CHANNEL:
	{
		struct video1394_mmap v;
		u64 mask;
		struct dma_iso_ctx *d;
		int i;

		if (copy_from_user(&v, argp, sizeof(v)))
			return -EFAULT;

		/* if channel < 0, find lowest available one */
		if (v.channel < 0) {
		    mask = (u64)0x1;
		    for (i=0; i<ISO_CHANNELS; i++) {
			if (!(ohci->ISO_channel_usage & mask)) {
			    v.channel = i;
			    PRINT(KERN_INFO, ohci->host->id, "Found free channel %d", i);
			    break;
			}
			mask = mask << 1;
		    }
		}

		if (v.channel<0 || v.channel>(ISO_CHANNELS-1)) {
			PRINT(KERN_ERR, ohci->host->id,
			      "Iso channel %d out of bounds", v.channel);
			return -EFAULT;
		}
		mask = (u64)0x1<<v.channel;
		printk("mask: %08X%08X usage: %08X%08X\n",
		       (u32)(mask>>32),(u32)(mask&0xffffffff),
		       (u32)(ohci->ISO_channel_usage>>32),
		       (u32)(ohci->ISO_channel_usage&0xffffffff));
		if (ohci->ISO_channel_usage & mask) {
			PRINT(KERN_ERR, ohci->host->id,
			      "Channel %d is already taken", v.channel);
			return -EFAULT;
		}
		ohci->ISO_channel_usage |= mask;

		if (v.buf_size == 0 || v.buf_size > VIDEO1394_MAX_SIZE) {
			PRINT(KERN_ERR, ohci->host->id,
			      "Invalid %d length buffer requested",v.buf_size);
			return -EFAULT;
		}

		if (v.nb_buffers == 0 || v.nb_buffers > VIDEO1394_MAX_SIZE) {
			PRINT(KERN_ERR, ohci->host->id,
			      "Invalid %d buffers requested",v.nb_buffers);
			return -EFAULT;
		}

		if (v.nb_buffers * v.buf_size > VIDEO1394_MAX_SIZE) {
			PRINT(KERN_ERR, ohci->host->id,
			      "%d buffers of size %d bytes is too big",
			      v.nb_buffers, v.buf_size);
			return -EFAULT;
		}

		if (cmd == VIDEO1394_IOC_LISTEN_CHANNEL) {
			d = alloc_dma_iso_ctx(ohci, OHCI_ISO_RECEIVE,
					      v.nb_buffers, v.buf_size,
					      v.channel, 0);

			if (d == NULL) {
				PRINT(KERN_ERR, ohci->host->id,
				      "Couldn't allocate ir context");
				return -EFAULT;
			}
			initialize_dma_ir_ctx(d, v.sync_tag, v.flags);

			ctx->current_ctx = d;

			v.buf_size = d->buf_size;
			list_add_tail(&d->link, &ctx->context_list);

			PRINT(KERN_INFO, ohci->host->id,
			      "iso context %d listen on channel %d",
			      d->ctx, v.channel);
		}
		else {
			d = alloc_dma_iso_ctx(ohci, OHCI_ISO_TRANSMIT,
					      v.nb_buffers, v.buf_size,
					      v.channel, v.packet_size);

			if (d == NULL) {
				PRINT(KERN_ERR, ohci->host->id,
				      "Couldn't allocate it context");
				return -EFAULT;
			}
			initialize_dma_it_ctx(d, v.sync_tag,
					      v.syt_offset, v.flags);

			ctx->current_ctx = d;

			v.buf_size = d->buf_size;

			list_add_tail(&d->link, &ctx->context_list);

			PRINT(KERN_INFO, ohci->host->id,
			      "Iso context %d talk on channel %d", d->ctx,
			      v.channel);
		}

		if (copy_to_user(argp, &v, sizeof(v)))
			return -EFAULT;

		return 0;
	}
	case VIDEO1394_IOC_UNLISTEN_CHANNEL:
	case VIDEO1394_IOC_UNTALK_CHANNEL:
	{
		int channel;
		u64 mask;
		struct dma_iso_ctx *d;

		if (copy_from_user(&channel, argp, sizeof(int)))
			return -EFAULT;

		if (channel<0 || channel>(ISO_CHANNELS-1)) {
			PRINT(KERN_ERR, ohci->host->id,
			      "Iso channel %d out of bound", channel);
			return -EFAULT;
		}
		mask = (u64)0x1<<channel;
		if (!(ohci->ISO_channel_usage & mask)) {
			PRINT(KERN_ERR, ohci->host->id,
			      "Channel %d is not being used", channel);
			return -EFAULT;
		}

		/* Mark this channel as unused */
		ohci->ISO_channel_usage &= ~mask;

		if (cmd == VIDEO1394_IOC_UNLISTEN_CHANNEL)
			d = find_ctx(&ctx->context_list, OHCI_ISO_RECEIVE, channel);
		else
			d = find_ctx(&ctx->context_list, OHCI_ISO_TRANSMIT, channel);

		if (d == NULL) return -EFAULT;
		PRINT(KERN_INFO, ohci->host->id, "Iso context %d "
		      "stop talking on channel %d", d->ctx, channel);
		free_dma_iso_ctx(d);

		return 0;
	}
	case VIDEO1394_IOC_LISTEN_QUEUE_BUFFER:
	{
		struct video1394_wait v;
		struct dma_iso_ctx *d;

		if (copy_from_user(&v, argp, sizeof(v)))
			return -EFAULT;

		d = find_ctx(&ctx->context_list, OHCI_ISO_RECEIVE, v.channel);
		if (d == NULL) return -EFAULT;

		if ((v.buffer<0) || (v.buffer>d->num_desc)) {
			PRINT(KERN_ERR, ohci->host->id,
			      "Buffer %d out of range",v.buffer);
			return -EFAULT;
		}

		spin_lock_irqsave(&d->lock,flags);

		if (d->buffer_status[v.buffer]==VIDEO1394_BUFFER_QUEUED) {
			PRINT(KERN_ERR, ohci->host->id,
			      "Buffer %d is already used",v.buffer);
			spin_unlock_irqrestore(&d->lock,flags);
			return -EFAULT;
		}

		d->buffer_status[v.buffer]=VIDEO1394_BUFFER_QUEUED;

		if (d->last_buffer>=0)
			d->ir_prg[d->last_buffer][d->nb_cmd-1].branchAddress =
				cpu_to_le32((dma_prog_region_offset_to_bus(&d->prg_reg[v.buffer], 0)
					& 0xfffffff0) | 0x1);

		d->last_buffer = v.buffer;

		d->ir_prg[d->last_buffer][d->nb_cmd-1].branchAddress = 0;

		spin_unlock_irqrestore(&d->lock,flags);

		if (!(reg_read(ohci, d->ctrlSet) & 0x8000))
		{
			DBGMSG(ohci->host->id, "Starting iso DMA ctx=%d",d->ctx);

			/* Tell the controller where the first program is */
			reg_write(ohci, d->cmdPtr,
				dma_prog_region_offset_to_bus(&d->prg_reg[v.buffer], 0) | 0x1);

			/* Run IR context */
			reg_write(ohci, d->ctrlSet, 0x8000);
		}
		else {
			/* Wake up dma context if necessary */
			if (!(reg_read(ohci, d->ctrlSet) & 0x400)) {
				PRINT(KERN_INFO, ohci->host->id,
				      "Waking up iso dma ctx=%d", d->ctx);
				reg_write(ohci, d->ctrlSet, 0x1000);
			}
		}
		return 0;

	}
	case VIDEO1394_IOC_LISTEN_WAIT_BUFFER:
	case VIDEO1394_IOC_LISTEN_POLL_BUFFER:
	{
		struct video1394_wait v;
		struct dma_iso_ctx *d;
		int i;

		if (copy_from_user(&v, argp, sizeof(v)))
			return -EFAULT;

		d = find_ctx(&ctx->context_list, OHCI_ISO_RECEIVE, v.channel);
		if (d == NULL) return -EFAULT;

		if ((v.buffer<0) || (v.buffer>d->num_desc)) {
			PRINT(KERN_ERR, ohci->host->id,
			      "Buffer %d out of range",v.buffer);
			return -EFAULT;
		}

		/*
		 * I change the way it works so that it returns
		 * the last received frame.
		 */
		spin_lock_irqsave(&d->lock, flags);
		switch(d->buffer_status[v.buffer]) {
		case VIDEO1394_BUFFER_READY:
			d->buffer_status[v.buffer]=VIDEO1394_BUFFER_FREE;
			break;
		case VIDEO1394_BUFFER_QUEUED:
			if (cmd == VIDEO1394_IOC_LISTEN_POLL_BUFFER) {
			    /* for polling, return error code EINTR */
			    spin_unlock_irqrestore(&d->lock, flags);
			    return -EINTR;
			}

#if 1
			while (d->buffer_status[v.buffer]!=
			      VIDEO1394_BUFFER_READY) {
				spin_unlock_irqrestore(&d->lock, flags);
				interruptible_sleep_on(&d->waitq);
				spin_lock_irqsave(&d->lock, flags);
				if (signal_pending(current)) {
					spin_unlock_irqrestore(&d->lock,flags);
					return -EINTR;
				}
			}
#else
			if (wait_event_interruptible(d->waitq,
						     d->buffer_status[v.buffer]
						     == VIDEO1394_BUFFER_READY)
			    == -ERESTARTSYS)
				return -EINTR;
#endif
			d->buffer_status[v.buffer]=VIDEO1394_BUFFER_FREE;
			break;
		default:
			PRINT(KERN_ERR, ohci->host->id,
			      "Buffer %d is not queued",v.buffer);
			spin_unlock_irqrestore(&d->lock, flags);
			return -EFAULT;
		}

		/* set time of buffer */
		v.filltime = d->buffer_time[v.buffer];
//		printk("Buffer %d time %d\n", v.buffer, (d->buffer_time[v.buffer]).tv_usec);

		/*
		 * Look ahead to see how many more buffers have been received
		 */
		i=0;
		while (d->buffer_status[(v.buffer+1)%d->num_desc]==
		       VIDEO1394_BUFFER_READY) {
			v.buffer=(v.buffer+1)%d->num_desc;
			i++;
		}
		spin_unlock_irqrestore(&d->lock, flags);

		v.buffer=i;
		if (copy_to_user(argp, &v, sizeof(v)))
			return -EFAULT;

		return 0;
	}
	case VIDEO1394_IOC_TALK_QUEUE_BUFFER:
	{
		struct video1394_wait v;
		unsigned int *psizes = NULL;
		struct dma_iso_ctx *d;

		if (copy_from_user(&v, argp, sizeof(v)))
			return -EFAULT;

		d = find_ctx(&ctx->context_list, OHCI_ISO_TRANSMIT, v.channel);
		if (d == NULL) return -EFAULT;

		if ((v.buffer<0) || (v.buffer>d->num_desc)) {
			PRINT(KERN_ERR, ohci->host->id,
			      "Buffer %d out of range",v.buffer);
			return -EFAULT;
		}

		if (d->flags & VIDEO1394_VARIABLE_PACKET_SIZE) {
			int buf_size = d->nb_cmd * sizeof(unsigned int);
			struct video1394_queue_variable __user *p = argp;
			unsigned int __user *qv;

			if (get_user(qv, &p->packet_sizes))
				return -EFAULT;

			psizes = kmalloc(buf_size, GFP_KERNEL);
			if (!psizes)
				return -ENOMEM;

			if (copy_from_user(psizes, qv, buf_size)) {
				kfree(psizes);
				return -EFAULT;
			}
		}

		spin_lock_irqsave(&d->lock,flags);

		if (d->buffer_status[v.buffer]!=VIDEO1394_BUFFER_FREE) {
			PRINT(KERN_ERR, ohci->host->id,
			      "Buffer %d is already used",v.buffer);
			spin_unlock_irqrestore(&d->lock,flags);
			if (psizes)
				kfree(psizes);
			return -EFAULT;
		}

		if (d->flags & VIDEO1394_VARIABLE_PACKET_SIZE) {
			initialize_dma_it_prg_var_packet_queue(
				d, v.buffer, psizes,
				ohci);
		}

		d->buffer_status[v.buffer]=VIDEO1394_BUFFER_QUEUED;

		if (d->last_buffer >= 0) {
			d->it_prg[d->last_buffer]
				[ d->last_used_cmd[d->last_buffer] ].end.branchAddress =
					cpu_to_le32((dma_prog_region_offset_to_bus(&d->prg_reg[v.buffer],
						0) & 0xfffffff0) | 0x3);

			d->it_prg[d->last_buffer]
				[ d->last_used_cmd[d->last_buffer] ].begin.branchAddress =
					cpu_to_le32((dma_prog_region_offset_to_bus(&d->prg_reg[v.buffer],
						0) & 0xfffffff0) | 0x3);
			d->next_buffer[d->last_buffer] = v.buffer;
		}
		d->last_buffer = v.buffer;
		d->next_buffer[d->last_buffer] = -1;

		d->it_prg[d->last_buffer][d->last_used_cmd[d->last_buffer]].end.branchAddress = 0;

		spin_unlock_irqrestore(&d->lock,flags);

		if (!(reg_read(ohci, d->ctrlSet) & 0x8000))
		{
			DBGMSG(ohci->host->id, "Starting iso transmit DMA ctx=%d",
			       d->ctx);
			put_timestamp(ohci, d, d->last_buffer);

			/* Tell the controller where the first program is */
			reg_write(ohci, d->cmdPtr,
				dma_prog_region_offset_to_bus(&d->prg_reg[v.buffer], 0) | 0x3);

			/* Run IT context */
			reg_write(ohci, d->ctrlSet, 0x8000);
		}
		else {
			/* Wake up dma context if necessary */
			if (!(reg_read(ohci, d->ctrlSet) & 0x400)) {
				PRINT(KERN_INFO, ohci->host->id,
				      "Waking up iso transmit dma ctx=%d",
				      d->ctx);
				put_timestamp(ohci, d, d->last_buffer);
				reg_write(ohci, d->ctrlSet, 0x1000);
			}
		}

		if (psizes)
			kfree(psizes);

		return 0;

	}
	case VIDEO1394_IOC_TALK_WAIT_BUFFER:
	{
		struct video1394_wait v;
		struct dma_iso_ctx *d;

		if (copy_from_user(&v, argp, sizeof(v)))
			return -EFAULT;

		d = find_ctx(&ctx->context_list, OHCI_ISO_TRANSMIT, v.channel);
		if (d == NULL) return -EFAULT;

		if ((v.buffer<0) || (v.buffer>d->num_desc)) {
			PRINT(KERN_ERR, ohci->host->id,
			      "Buffer %d out of range",v.buffer);
			return -EFAULT;
		}

		switch(d->buffer_status[v.buffer]) {
		case VIDEO1394_BUFFER_READY:
			d->buffer_status[v.buffer]=VIDEO1394_BUFFER_FREE;
			return 0;
		case VIDEO1394_BUFFER_QUEUED:
#if 1
			while (d->buffer_status[v.buffer]!=
			      VIDEO1394_BUFFER_READY) {
				interruptible_sleep_on(&d->waitq);
				if (signal_pending(current)) return -EINTR;
			}
#else
			if (wait_event_interruptible(d->waitq,
						     d->buffer_status[v.buffer]
						     == VIDEO1394_BUFFER_READY)
			    == -ERESTARTSYS)
				return -EINTR;
#endif
			d->buffer_status[v.buffer]=VIDEO1394_BUFFER_FREE;
			return 0;
		default:
			PRINT(KERN_ERR, ohci->host->id,
			      "Buffer %d is not queued",v.buffer);
			return -EFAULT;
		}
	}
	default:
		return -EINVAL;
	}
}

/*
 *	This maps the vmalloced and reserved buffer to user space.
 *
 *  FIXME:
 *  - PAGE_READONLY should suffice!?
 *  - remap_page_range is kind of inefficient for page by page remapping.
 *    But e.g. pte_alloc() does not work in modules ... :-(
 */

int video1394_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct file_ctx *ctx = (struct file_ctx *)file->private_data;
	int res = -EINVAL;

	lock_kernel();
	if (ctx->current_ctx == NULL) {
		PRINT(KERN_ERR, ctx->ohci->host->id, "Current iso context not set");
	} else
		res = dma_region_mmap(&ctx->current_ctx->dma, file, vma);
	unlock_kernel();

	return res;
}

static int video1394_open(struct inode *inode, struct file *file)
{
	int i = ieee1394_file_to_instance(file);
	struct ti_ohci *ohci;
	struct file_ctx *ctx;

	ohci = hpsb_get_hostinfo_bykey(&video1394_highlevel, i);
        if (ohci == NULL)
                return -EIO;

	ctx = kmalloc(sizeof(struct file_ctx), GFP_KERNEL);
	if (ctx == NULL)  {
		PRINT(KERN_ERR, ohci->host->id, "Cannot malloc file_ctx");
		return -ENOMEM;
	}

	memset(ctx, 0, sizeof(struct file_ctx));
	ctx->ohci = ohci;
	INIT_LIST_HEAD(&ctx->context_list);
	ctx->current_ctx = NULL;
	file->private_data = ctx;

	return 0;
}

static int video1394_release(struct inode *inode, struct file *file)
{
	struct file_ctx *ctx = (struct file_ctx *)file->private_data;
	struct ti_ohci *ohci = ctx->ohci;
	struct list_head *lh, *next;
	u64 mask;

	lock_kernel();
	list_for_each_safe(lh, next, &ctx->context_list) {
		struct dma_iso_ctx *d;
		d = list_entry(lh, struct dma_iso_ctx, link);
		mask = (u64) 1 << d->channel;

		if (!(ohci->ISO_channel_usage & mask))
			PRINT(KERN_ERR, ohci->host->id, "On release: Channel %d "
			      "is not being used", d->channel);
		else
			ohci->ISO_channel_usage &= ~mask;
		PRINT(KERN_INFO, ohci->host->id, "On release: Iso %s context "
		      "%d stop listening on channel %d",
		      d->type == OHCI_ISO_RECEIVE ? "receive" : "transmit",
		      d->ctx, d->channel);
		free_dma_iso_ctx(d);
	}

	kfree(ctx);
	file->private_data = NULL;

	unlock_kernel();
	return 0;
}

static struct cdev video1394_cdev;
static struct file_operations video1394_fops=
{
	.owner =	THIS_MODULE,
	.ioctl =	video1394_ioctl,
	.mmap =		video1394_mmap,
	.open =		video1394_open,
	.release =	video1394_release
};

/*** HOTPLUG STUFF **********************************************************/
/*
 * Export information about protocols/devices supported by this driver.
 */
static struct ieee1394_device_id video1394_id_table[] = {
	{
		.match_flags	= IEEE1394_MATCH_SPECIFIER_ID | IEEE1394_MATCH_VERSION,
		.specifier_id	= CAMERA_UNIT_SPEC_ID_ENTRY & 0xffffff,
		.version	= CAMERA_SW_VERSION_ENTRY & 0xffffff
	},
	{ }
};

MODULE_DEVICE_TABLE(ieee1394, video1394_id_table);

static struct hpsb_protocol_driver video1394_driver = {
	.name		= "1394 Digital Camera Driver",
	.id_table	= video1394_id_table,
	.driver		= {
		.name	= VIDEO1394_DRIVER_NAME,
		.bus	= &ieee1394_bus_type,
	},
};


static void video1394_add_host (struct hpsb_host *host)
{
	struct ti_ohci *ohci;
	int minor;

	/* We only work with the OHCI-1394 driver */
	if (strcmp(host->driver->name, OHCI1394_DRIVER_NAME))
		return;

	ohci = (struct ti_ohci *)host->hostdata;

	if (!hpsb_create_hostinfo(&video1394_highlevel, host, 0)) {
		PRINT(KERN_ERR, ohci->host->id, "Cannot allocate hostinfo");
		return;
	}

	hpsb_set_hostinfo(&video1394_highlevel, host, ohci);
	hpsb_set_hostinfo_key(&video1394_highlevel, host, ohci->host->id);

	minor = IEEE1394_MINOR_BLOCK_VIDEO1394 * 16 + ohci->host->id;
	devfs_mk_cdev(MKDEV(IEEE1394_MAJOR, minor),
		       S_IFCHR | S_IRUSR | S_IWUSR,
		       "%s/%d", VIDEO1394_DRIVER_NAME, ohci->host->id);
}


static void video1394_remove_host (struct hpsb_host *host)
{
	struct ti_ohci *ohci = hpsb_get_hostinfo(&video1394_highlevel, host);

	if (ohci)
		devfs_remove("%s/%d", VIDEO1394_DRIVER_NAME, ohci->host->id);

	return;
}


static struct hpsb_highlevel video1394_highlevel = {
	.name =		VIDEO1394_DRIVER_NAME,
	.add_host =	video1394_add_host,
	.remove_host =	video1394_remove_host,
};

MODULE_AUTHOR("Sebastien Rougeaux <sebastien.rougeaux@anu.edu.au>");
MODULE_DESCRIPTION("driver for digital video on OHCI board");
MODULE_SUPPORTED_DEVICE(VIDEO1394_DRIVER_NAME);
MODULE_LICENSE("GPL");

#ifdef CONFIG_COMPAT

#define VIDEO1394_IOC32_LISTEN_QUEUE_BUFFER     \
	_IOW ('#', 0x12, struct video1394_wait32)
#define VIDEO1394_IOC32_LISTEN_WAIT_BUFFER      \
	_IOWR('#', 0x13, struct video1394_wait32)
#define VIDEO1394_IOC32_TALK_WAIT_BUFFER        \
	_IOW ('#', 0x17, struct video1394_wait32)
#define VIDEO1394_IOC32_LISTEN_POLL_BUFFER      \
	_IOWR('#', 0x18, struct video1394_wait32)

struct video1394_wait32 {
	u32 channel;
	u32 buffer;
	struct compat_timeval filltime;
};

static int video1394_wr_wait32(unsigned int fd, unsigned int cmd, unsigned long arg,
			       struct file *file)
{
        struct video1394_wait32 __user *argp = (void __user *)arg;
        struct video1394_wait32 wait32;
        struct video1394_wait wait;
        mm_segment_t old_fs;
        int ret;

	if (file->f_op->ioctl != video1394_ioctl)
		return -EFAULT;

        if (copy_from_user(&wait32, argp, sizeof(wait32)))
                return -EFAULT;

        wait.channel = wait32.channel;
        wait.buffer = wait32.buffer;
        wait.filltime.tv_sec = (time_t)wait32.filltime.tv_sec;
        wait.filltime.tv_usec = (suseconds_t)wait32.filltime.tv_usec;

        old_fs = get_fs();
        set_fs(KERNEL_DS);
        if (cmd == VIDEO1394_IOC32_LISTEN_WAIT_BUFFER)
		ret = video1394_ioctl(file->f_dentry->d_inode, file,
				      VIDEO1394_IOC_LISTEN_WAIT_BUFFER,
				      (unsigned long) &wait);
        else
		ret = video1394_ioctl(file->f_dentry->d_inode, file,
				      VIDEO1394_IOC_LISTEN_POLL_BUFFER,
				      (unsigned long) &wait);
        set_fs(old_fs);

        if (!ret) {
                wait32.channel = wait.channel;
                wait32.buffer = wait.buffer;
                wait32.filltime.tv_sec = (int)wait.filltime.tv_sec;
                wait32.filltime.tv_usec = (int)wait.filltime.tv_usec;

                if (copy_to_user(argp, &wait32, sizeof(wait32)))
                        ret = -EFAULT;
        }

        return ret;
}

static int video1394_w_wait32(unsigned int fd, unsigned int cmd, unsigned long arg,
			      struct file *file)
{
        struct video1394_wait32 wait32;
        struct video1394_wait wait;
        mm_segment_t old_fs;
        int ret;

	if (file->f_op->ioctl != video1394_ioctl)
		return -EFAULT;

        if (copy_from_user(&wait32, (void __user *)arg, sizeof(wait32)))
                return -EFAULT;

        wait.channel = wait32.channel;
        wait.buffer = wait32.buffer;
        wait.filltime.tv_sec = (time_t)wait32.filltime.tv_sec;
        wait.filltime.tv_usec = (suseconds_t)wait32.filltime.tv_usec;

        old_fs = get_fs();
        set_fs(KERNEL_DS);
        if (cmd == VIDEO1394_IOC32_LISTEN_QUEUE_BUFFER)
		ret = video1394_ioctl(file->f_dentry->d_inode, file,
				      VIDEO1394_IOC_LISTEN_QUEUE_BUFFER,
				      (unsigned long) &wait);
        else
		ret = video1394_ioctl(file->f_dentry->d_inode, file,
				      VIDEO1394_IOC_TALK_WAIT_BUFFER,
				      (unsigned long) &wait);
        set_fs(old_fs);

        return ret;
}

static int video1394_queue_buf32(unsigned int fd, unsigned int cmd, unsigned long arg,
				 struct file *file)
{
	if (file->f_op->ioctl != video1394_ioctl)
		return -EFAULT;

        return -EFAULT;

	return video1394_ioctl(file->f_dentry->d_inode, file,
				VIDEO1394_IOC_TALK_QUEUE_BUFFER, arg);
}

#endif /* CONFIG_COMPAT */

static void __exit video1394_exit_module (void)
{
#ifdef CONFIG_COMPAT
	int ret;

	ret = unregister_ioctl32_conversion(VIDEO1394_IOC_LISTEN_CHANNEL);
	ret |= unregister_ioctl32_conversion(VIDEO1394_IOC_UNLISTEN_CHANNEL);
	ret |= unregister_ioctl32_conversion(VIDEO1394_IOC_TALK_CHANNEL);
	ret |= unregister_ioctl32_conversion(VIDEO1394_IOC_UNTALK_CHANNEL);
	ret |= unregister_ioctl32_conversion(VIDEO1394_IOC32_LISTEN_QUEUE_BUFFER);
	ret |= unregister_ioctl32_conversion(VIDEO1394_IOC32_LISTEN_WAIT_BUFFER);
	ret |= unregister_ioctl32_conversion(VIDEO1394_IOC_TALK_QUEUE_BUFFER);
	ret |= unregister_ioctl32_conversion(VIDEO1394_IOC32_TALK_WAIT_BUFFER);
	ret |= unregister_ioctl32_conversion(VIDEO1394_IOC32_LISTEN_POLL_BUFFER);
	if (ret)
		PRINT_G(KERN_CRIT, "Error unregistering ioctl32 translations");
#endif

	hpsb_unregister_protocol(&video1394_driver);

	hpsb_unregister_highlevel(&video1394_highlevel);

	devfs_remove(VIDEO1394_DRIVER_NAME);
	cdev_del(&video1394_cdev);

	PRINT_G(KERN_INFO, "Removed " VIDEO1394_DRIVER_NAME " module");
}

static int __init video1394_init_module (void)
{
	int ret;

	cdev_init(&video1394_cdev, &video1394_fops);
	video1394_cdev.owner = THIS_MODULE;
	kobject_set_name(&video1394_cdev.kobj, VIDEO1394_DRIVER_NAME);
	ret = cdev_add(&video1394_cdev, IEEE1394_VIDEO1394_DEV, 16);
	if (ret) {
		PRINT_G(KERN_ERR, "video1394: unable to get minor device block");
		return ret;
        }

	devfs_mk_dir(VIDEO1394_DRIVER_NAME);

	hpsb_register_highlevel(&video1394_highlevel);

	ret = hpsb_register_protocol(&video1394_driver);
	if (ret) {
		PRINT_G(KERN_ERR, "video1394: failed to register protocol");
		hpsb_unregister_highlevel(&video1394_highlevel);
		devfs_remove(VIDEO1394_DRIVER_NAME);
		cdev_del(&video1394_cdev);
		return ret;
	}

#ifdef CONFIG_COMPAT
	{
		/* First the compatible ones */
		ret = register_ioctl32_conversion(VIDEO1394_IOC_LISTEN_CHANNEL, NULL);
		ret |= register_ioctl32_conversion(VIDEO1394_IOC_UNLISTEN_CHANNEL, NULL);
		ret |= register_ioctl32_conversion(VIDEO1394_IOC_TALK_CHANNEL, NULL);
		ret |= register_ioctl32_conversion(VIDEO1394_IOC_UNTALK_CHANNEL, NULL);

		/* These need translation */
		ret |= register_ioctl32_conversion(VIDEO1394_IOC32_LISTEN_QUEUE_BUFFER,
					    video1394_w_wait32);
		ret |= register_ioctl32_conversion(VIDEO1394_IOC32_LISTEN_WAIT_BUFFER,
					    video1394_wr_wait32);
		ret |= register_ioctl32_conversion(VIDEO1394_IOC_TALK_QUEUE_BUFFER,
					    video1394_queue_buf32);
		ret |= register_ioctl32_conversion(VIDEO1394_IOC32_TALK_WAIT_BUFFER,
					    video1394_w_wait32);
		ret |= register_ioctl32_conversion(VIDEO1394_IOC32_LISTEN_POLL_BUFFER,
					    video1394_wr_wait32);
		if (ret)
			PRINT_G(KERN_INFO, "Error registering ioctl32 translations");
	}
#endif

	PRINT_G(KERN_INFO, "Installed " VIDEO1394_DRIVER_NAME " module");
	return 0;
}


module_init(video1394_init_module);
module_exit(video1394_exit_module);
MODULE_ALIAS_CHARDEV(IEEE1394_MAJOR, IEEE1394_MINOR_BLOCK_VIDEO1394 * 16);
