/*
 *  linux/drivers/s390/net/lcs.c
 *
 *  Linux for S/390 Lan Channel Station Network Driver
 *
 *  Copyright (C)  1999-2001 IBM Deutschland Entwicklung GmbH,
 *			     IBM Corporation
 *    Author(s): Original Code written by
 *			  DJ Barrow (djbarrow@de.ibm.com,barrow_dj@yahoo.com)
 *		 Rewritten by
 *			  Frank Pavlic (pavlic@de.ibm.com) and
 *		 	  Martin Schwidefsky <schwidefsky@de.ibm.com>
 *
 *    $Revision: 1.85 $	 $Date: 2004/08/04 11:05:43 $
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/if.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/trdevice.h>
#include <linux/fddidevice.h>
#include <linux/inetdevice.h>
#include <linux/in.h>
#include <linux/igmp.h>
#include <linux/delay.h>
#include <net/arp.h>
#include <net/ip.h>

#include <asm/debug.h>
#include <asm/idals.h>
#include <asm/timex.h>
#include <linux/device.h>
#include <asm/ccwgroup.h>

#include "lcs.h"
#include "cu3088.h"


#if !defined(CONFIG_NET_ETHERNET) && \
    !defined(CONFIG_TR) && !defined(CONFIG_FDDI)
#error Cannot compile lcs.c without some net devices switched on.
#endif

/**
 * initialization string for output
 */
#define VERSION_LCS_C  "$Revision: 1.85 $"

static char version[] __initdata = "LCS driver ("VERSION_LCS_C "/" VERSION_LCS_H ")";
static char debug_buffer[255];

/**
 * Some prototypes.
 */
static void lcs_tasklet(unsigned long);
static void lcs_start_kernel_thread(struct lcs_card *card);
static void lcs_get_frames_cb(struct lcs_channel *, struct lcs_buffer *);

/**
 * Debug Facility Stuff
 */
static debug_info_t *lcs_dbf_setup;
static debug_info_t *lcs_dbf_trace;

/**
 *  LCS Debug Facility functions
 */
static void
lcs_unregister_debug_facility(void)
{
	if (lcs_dbf_setup)
		debug_unregister(lcs_dbf_setup);
	if (lcs_dbf_trace)
		debug_unregister(lcs_dbf_trace);
}

static int
lcs_register_debug_facility(void)
{
	lcs_dbf_setup = debug_register("lcs_setup", 1, 1, 8);
	lcs_dbf_trace = debug_register("lcs_trace", 1, 2, 8);
	if (lcs_dbf_setup == NULL || lcs_dbf_trace == NULL) {
		PRINT_ERR("Not enough memory for debug facility.\n");
		lcs_unregister_debug_facility();
		return -ENOMEM;
	}
	debug_register_view(lcs_dbf_setup, &debug_hex_ascii_view);
	debug_set_level(lcs_dbf_setup, 2);
	debug_register_view(lcs_dbf_trace, &debug_hex_ascii_view);
	debug_set_level(lcs_dbf_trace, 2);
	return 0;
}

/**
 * Allocate io buffers.
 */
static int
lcs_alloc_channel(struct lcs_channel *channel)
{
	int cnt;

	LCS_DBF_TEXT(2, setup, "ichalloc");
	for (cnt = 0; cnt < LCS_NUM_BUFFS; cnt++) {
		/* alloc memory fo iobuffer */
		channel->iob[cnt].data = (void *)
			kmalloc(LCS_IOBUFFERSIZE, GFP_DMA | GFP_KERNEL);
		if (channel->iob[cnt].data == NULL)
			break;
		memset(channel->iob[cnt].data, 0, LCS_IOBUFFERSIZE);
		channel->iob[cnt].state = BUF_STATE_EMPTY;
	}
	if (cnt < LCS_NUM_BUFFS) {
		/* Not all io buffers could be allocated. */
		LCS_DBF_TEXT(2, setup, "echalloc");
		while (cnt-- > 0)
			kfree(channel->iob[cnt].data);
		return -ENOMEM;
	}
	return 0;
}

/**
 * Free io buffers.
 */
static void
lcs_free_channel(struct lcs_channel *channel)
{
	int cnt;

	LCS_DBF_TEXT(2, setup, "ichfree");
	for (cnt = 0; cnt < LCS_NUM_BUFFS; cnt++) {
		if (channel->iob[cnt].data != NULL)
			kfree(channel->iob[cnt].data);
		channel->iob[cnt].data = NULL;
	}
}

/*
 * Cleanup channel.
 */
static void
lcs_cleanup_channel(struct lcs_channel *channel)
{
	LCS_DBF_TEXT(3, setup, "cleanch");
	/* Kill write channel tasklets. */
	tasklet_kill(&channel->irq_tasklet);
	/* Free channel buffers. */
	lcs_free_channel(channel);
}

/**
 * LCS free memory for card and channels.
 */
static void
lcs_free_card(struct lcs_card *card)
{
	LCS_DBF_TEXT(2, setup, "remcard");
	LCS_DBF_HEX(2, setup, &card, sizeof(void*));
	kfree(card);
}

/**
 * LCS alloc memory for card and channels
 */
static struct lcs_card *
lcs_alloc_card(void)
{
	struct lcs_card *card;
	int rc;

	LCS_DBF_TEXT(2, setup, "alloclcs");

	card = kmalloc(sizeof(struct lcs_card), GFP_KERNEL | GFP_DMA);
	if (card == NULL)
		return NULL;
	memset(card, 0, sizeof(struct lcs_card));
	card->lan_type = LCS_FRAME_TYPE_AUTO;
	card->lancmd_timeout = LCS_LANCMD_TIMEOUT_DEFAULT;
	/* Allocate io buffers for the read channel. */
	rc = lcs_alloc_channel(&card->read);
	if (rc){
		LCS_DBF_TEXT(2, setup, "iccwerr");
		lcs_free_card(card);
		return NULL;
	}
	/* Allocate io buffers for the write channel. */
	rc = lcs_alloc_channel(&card->write);
	if (rc) {
		LCS_DBF_TEXT(2, setup, "iccwerr");
		lcs_cleanup_channel(&card->read);
		lcs_free_card(card);
		return NULL;
	}

#ifdef CONFIG_IP_MULTICAST
	INIT_LIST_HEAD(&card->ipm_list);
#endif
	LCS_DBF_HEX(2, setup, &card, sizeof(void*));
	return card;
}

/*
 * Setup read channel.
 */
static void
lcs_setup_read_ccws(struct lcs_card *card)
{
	int cnt;

	LCS_DBF_TEXT(2, setup, "ireadccw");
	/* Setup read ccws. */
	memset(card->read.ccws, 0, sizeof (struct ccw1) * (LCS_NUM_BUFFS + 1));
	for (cnt = 0; cnt < LCS_NUM_BUFFS; cnt++) {
		card->read.ccws[cnt].cmd_code = LCS_CCW_READ;
		card->read.ccws[cnt].count = LCS_IOBUFFERSIZE;
		card->read.ccws[cnt].flags =
			CCW_FLAG_CC | CCW_FLAG_SLI | CCW_FLAG_PCI;
		/*
		 * Note: we have allocated the buffer with GFP_DMA, so
		 * we do not need to do set_normalized_cda.
		 */
		card->read.ccws[cnt].cda =
			(__u32) __pa(card->read.iob[cnt].data);
		((struct lcs_header *)
		 card->read.iob[cnt].data)->offset = LCS_ILLEGAL_OFFSET;
		card->read.iob[cnt].callback = lcs_get_frames_cb;
		card->read.iob[cnt].state = BUF_STATE_READY;
		card->read.iob[cnt].count = LCS_IOBUFFERSIZE;
	}
	card->read.ccws[0].flags &= ~CCW_FLAG_PCI;
	card->read.ccws[LCS_NUM_BUFFS - 1].flags &= ~CCW_FLAG_PCI;
	card->read.ccws[LCS_NUM_BUFFS - 1].flags |= CCW_FLAG_SUSPEND;
	/* Last ccw is a tic (transfer in channel). */
	card->read.ccws[LCS_NUM_BUFFS].cmd_code = LCS_CCW_TRANSFER;
	card->read.ccws[LCS_NUM_BUFFS].cda =
		(__u32) __pa(card->read.ccws);
	/* Setg initial state of the read channel. */
	card->read.state = CH_STATE_INIT;

	card->read.io_idx = 0;
	card->read.buf_idx = 0;
}

static void
lcs_setup_read(struct lcs_card *card)
{
	LCS_DBF_TEXT(3, setup, "initread");

	lcs_setup_read_ccws(card);
	/* Initialize read channel tasklet. */
	card->read.irq_tasklet.data = (unsigned long) &card->read;
	card->read.irq_tasklet.func = lcs_tasklet;
	/* Initialize waitqueue. */
	init_waitqueue_head(&card->read.wait_q);
}

/*
 * Setup write channel.
 */
static void
lcs_setup_write_ccws(struct lcs_card *card)
{
	int cnt;

	LCS_DBF_TEXT(3, setup, "iwritccw");
	/* Setup write ccws. */
	memset(card->write.ccws, 0, sizeof(struct ccw1) * LCS_NUM_BUFFS + 1);
	for (cnt = 0; cnt < LCS_NUM_BUFFS; cnt++) {
		card->write.ccws[cnt].cmd_code = LCS_CCW_WRITE;
		card->write.ccws[cnt].count = 0;
		card->write.ccws[cnt].flags =
			CCW_FLAG_SUSPEND | CCW_FLAG_CC | CCW_FLAG_SLI;
		/*
		 * Note: we have allocated the buffer with GFP_DMA, so
		 * we do not need to do set_normalized_cda.
		 */
		card->write.ccws[cnt].cda =
			(__u32) __pa(card->write.iob[cnt].data);
	}
	/* Last ccw is a tic (transfer in channel). */
	card->write.ccws[LCS_NUM_BUFFS].cmd_code = LCS_CCW_TRANSFER;
	card->write.ccws[LCS_NUM_BUFFS].cda =
		(__u32) __pa(card->write.ccws);
	/* Set initial state of the write channel. */
	card->read.state = CH_STATE_INIT;

	card->write.io_idx = 0;
	card->write.buf_idx = 0;
}

static void
lcs_setup_write(struct lcs_card *card)
{
	LCS_DBF_TEXT(3, setup, "initwrit");

	lcs_setup_write_ccws(card);
	/* Initialize write channel tasklet. */
	card->write.irq_tasklet.data = (unsigned long) &card->write;
	card->write.irq_tasklet.func = lcs_tasklet;
	/* Initialize waitqueue. */
	init_waitqueue_head(&card->write.wait_q);
}



/**
 * Initialize channels,card and state machines.
 */
static void
lcs_setup_card(struct lcs_card *card)
{
	LCS_DBF_TEXT(2, setup, "initcard");
	LCS_DBF_HEX(2, setup, &card, sizeof(void*));

	lcs_setup_read(card);
	lcs_setup_write(card);
	/* Set cards initial state. */
	card->state = DEV_STATE_DOWN;
	card->tx_buffer = NULL;
	card->tx_emitted = 0;

	/* Initialize kernel thread task used for LGW commands. */
	INIT_WORK(&card->kernel_thread_starter,
		  (void *)lcs_start_kernel_thread,card);
	card->thread_mask = 0;
	spin_lock_init(&card->lock);
	spin_lock_init(&card->ipm_lock);
#ifdef CONFIG_IP_MULTICAST
	INIT_LIST_HEAD(&card->ipm_list);
#endif
	INIT_LIST_HEAD(&card->lancmd_waiters);
}

/**
 * Cleanup channels,card and state machines.
 */
static void
lcs_cleanup_card(struct lcs_card *card)
{
	struct list_head *l, *n;
	struct lcs_ipm_list *ipm_list;

	LCS_DBF_TEXT(3, setup, "cleancrd");
	LCS_DBF_HEX(2,setup,&card,sizeof(void*));
#ifdef	CONFIG_IP_MULTICAST
	/* Free multicast list. */
	list_for_each_safe(l, n, &card->ipm_list) {
		ipm_list = list_entry(l, struct lcs_ipm_list, list);
		list_del(&ipm_list->list);
		kfree(ipm_list);
	}
#endif
	if (card->dev != NULL)
		free_netdev(card->dev);
	/* Cleanup channels. */
	lcs_cleanup_channel(&card->write);
	lcs_cleanup_channel(&card->read);
}

/**
 * Start channel.
 */
static int
lcs_start_channel(struct lcs_channel *channel)
{
	unsigned long flags;
	int rc;

	LCS_DBF_TEXT_(4,trace,"ssch%s", channel->ccwdev->dev.bus_id);
	spin_lock_irqsave(get_ccwdev_lock(channel->ccwdev), flags);
	rc = ccw_device_start(channel->ccwdev,
			      channel->ccws + channel->io_idx, 0, 0,
			      DOIO_DENY_PREFETCH | DOIO_ALLOW_SUSPEND);
	if (rc == 0)
		channel->state = CH_STATE_RUNNING;
	spin_unlock_irqrestore(get_ccwdev_lock(channel->ccwdev), flags);
	if (rc) {
		LCS_DBF_TEXT_(4,trace,"essh%s", channel->ccwdev->dev.bus_id);
		PRINT_ERR("Error in starting channel, rc=%d!\n", rc);
	}
	return rc;
}

static int
lcs_clear_channel(struct lcs_channel *channel)
{
	unsigned long flags;
	int rc;

	LCS_DBF_TEXT(4,trace,"clearch");
	LCS_DBF_TEXT_(4,trace,"%s", channel->ccwdev->dev.bus_id);
	spin_lock_irqsave(get_ccwdev_lock(channel->ccwdev), flags);
	rc = ccw_device_clear(channel->ccwdev, (addr_t) channel);
	spin_unlock_irqrestore(get_ccwdev_lock(channel->ccwdev), flags);
	if (rc) {
		LCS_DBF_TEXT_(4,trace,"ecsc%s", channel->ccwdev->dev.bus_id);
		return rc;
	}
	wait_event(channel->wait_q, (channel->state == CH_STATE_CLEARED));
	channel->state = CH_STATE_STOPPED;
	return rc;
}


/**
 * Stop channel.
 */
static int
lcs_stop_channel(struct lcs_channel *channel)
{
	unsigned long flags;
	int rc;

	if (channel->state == CH_STATE_STOPPED)
		return 0;
	LCS_DBF_TEXT(4,trace,"haltsch");
	LCS_DBF_TEXT_(4,trace,"%s", channel->ccwdev->dev.bus_id);
	spin_lock_irqsave(get_ccwdev_lock(channel->ccwdev), flags);
	rc = ccw_device_halt(channel->ccwdev, (addr_t) channel);
	spin_unlock_irqrestore(get_ccwdev_lock(channel->ccwdev), flags);
	if (rc) {
		LCS_DBF_TEXT_(4,trace,"ehsc%s", channel->ccwdev->dev.bus_id);
		return rc;
	}
	/* Asynchronous halt initialted. Wait for its completion. */
	wait_event(channel->wait_q, (channel->state == CH_STATE_HALTED));
	lcs_clear_channel(channel);
	return 0;
}

/**
 * start read and write channel
 */
static int
lcs_start_channels(struct lcs_card *card)
{
	int rc;

	LCS_DBF_TEXT(2, trace, "chstart");
	/* start read channel */
	rc = lcs_start_channel(&card->read);
	if (rc)
		return rc;
	/* start write channel */
	rc = lcs_start_channel(&card->write);
	if (rc)
		lcs_stop_channel(&card->read);
	return rc;
}

/**
 * stop read and write channel
 */
static int
lcs_stop_channels(struct lcs_card *card)
{
	LCS_DBF_TEXT(2, trace, "chhalt");
	lcs_stop_channel(&card->read);
	lcs_stop_channel(&card->write);
	return 0;
}

/**
 * Get empty buffer.
 */
static struct lcs_buffer *
__lcs_get_buffer(struct lcs_channel *channel)
{
	int index;

	LCS_DBF_TEXT(5, trace, "_getbuff");
	index = channel->io_idx;
	do {
		if (channel->iob[index].state == BUF_STATE_EMPTY) {
			channel->iob[index].state = BUF_STATE_LOCKED;
			return channel->iob + index;
		}
		index = (index + 1) & (LCS_NUM_BUFFS - 1);
	} while (index != channel->io_idx);
	return NULL;
}

static struct lcs_buffer *
lcs_get_buffer(struct lcs_channel *channel)
{
	struct lcs_buffer *buffer;
	unsigned long flags;

	LCS_DBF_TEXT(5, trace, "getbuff");
	spin_lock_irqsave(get_ccwdev_lock(channel->ccwdev), flags);
	buffer = __lcs_get_buffer(channel);
	spin_unlock_irqrestore(get_ccwdev_lock(channel->ccwdev), flags);
	return buffer;
}

/**
 * Resume channel program if the channel is suspended.
 */
static int
__lcs_resume_channel(struct lcs_channel *channel)
{
	int rc;

	if (channel->state != CH_STATE_SUSPENDED)
		return 0;
	if (channel->ccws[channel->io_idx].flags & CCW_FLAG_SUSPEND)
		return 0;
	LCS_DBF_TEXT_(5, trace, "rsch%s", channel->ccwdev->dev.bus_id);
	rc = ccw_device_resume(channel->ccwdev);
	if (rc) {
		LCS_DBF_TEXT_(4, trace, "ersc%s", channel->ccwdev->dev.bus_id);
		PRINT_ERR("Error in lcs_resume_channel: rc=%d\n",rc);
	} else
		channel->state = CH_STATE_RUNNING;
	return rc;

}

/**
 * Make a buffer ready for processing.
 */
static inline void
__lcs_ready_buffer_bits(struct lcs_channel *channel, int index)
{
	int prev, next;

	LCS_DBF_TEXT(5, trace, "rdybits");
	prev = (index - 1) & (LCS_NUM_BUFFS - 1);
	next = (index + 1) & (LCS_NUM_BUFFS - 1);
	/* Check if we may clear the suspend bit of this buffer. */
	if (channel->ccws[next].flags & CCW_FLAG_SUSPEND) {
		/* Check if we have to set the PCI bit. */
		if (!(channel->ccws[prev].flags & CCW_FLAG_SUSPEND))
			/* Suspend bit of the previous buffer is not set. */
			channel->ccws[index].flags |= CCW_FLAG_PCI;
		/* Suspend bit of the next buffer is set. */
		channel->ccws[index].flags &= ~CCW_FLAG_SUSPEND;
	}
}

static int
lcs_ready_buffer(struct lcs_channel *channel, struct lcs_buffer *buffer)
{
	unsigned long flags;
	int index, rc;

	LCS_DBF_TEXT(5, trace, "rdybuff");
	if (buffer->state != BUF_STATE_LOCKED &&
	    buffer->state != BUF_STATE_PROCESSED)
		BUG();
	spin_lock_irqsave(get_ccwdev_lock(channel->ccwdev), flags);
	buffer->state = BUF_STATE_READY;
	index = buffer - channel->iob;
	/* Set length. */
	channel->ccws[index].count = buffer->count;
	/* Check relevant PCI/suspend bits. */
	__lcs_ready_buffer_bits(channel, index);
	rc = __lcs_resume_channel(channel);
	spin_unlock_irqrestore(get_ccwdev_lock(channel->ccwdev), flags);
	return rc;
}

/**
 * Mark the buffer as processed. Take care of the suspend bit
 * of the previous buffer. This function is called from
 * interrupt context, so the lock must not be taken.
 */
static int
__lcs_processed_buffer(struct lcs_channel *channel, struct lcs_buffer *buffer)
{
	int index, prev, next;

	LCS_DBF_TEXT(5, trace, "prcsbuff");
	if (buffer->state != BUF_STATE_READY)
		BUG();
	buffer->state = BUF_STATE_PROCESSED;
	index = buffer - channel->iob;
	prev = (index - 1) & (LCS_NUM_BUFFS - 1);
	next = (index + 1) & (LCS_NUM_BUFFS - 1);
	/* Set the suspend bit and clear the PCI bit of this buffer. */
	channel->ccws[index].flags |= CCW_FLAG_SUSPEND;
	channel->ccws[index].flags &= ~CCW_FLAG_PCI;
	/* Check the suspend bit of the previous buffer. */
	if (channel->iob[prev].state == BUF_STATE_READY) {
		/*
		 * Previous buffer is in state ready. It might have
		 * happened in lcs_ready_buffer that the suspend bit
		 * has not been cleared to avoid an endless loop.
		 * Do it now.
		 */
		__lcs_ready_buffer_bits(channel, prev);
	}
	/* Clear PCI bit of next buffer. */
	channel->ccws[next].flags &= ~CCW_FLAG_PCI;
	return __lcs_resume_channel(channel);
}

/**
 * Put a processed buffer back to state empty.
 */
static void
lcs_release_buffer(struct lcs_channel *channel, struct lcs_buffer *buffer)
{
	unsigned long flags;

	LCS_DBF_TEXT(5, trace, "relbuff");
	if (buffer->state != BUF_STATE_LOCKED &&
	    buffer->state != BUF_STATE_PROCESSED)
		BUG();
	spin_lock_irqsave(get_ccwdev_lock(channel->ccwdev), flags);
	buffer->state = BUF_STATE_EMPTY;
	spin_unlock_irqrestore(get_ccwdev_lock(channel->ccwdev), flags);
}

/**
 * Get buffer for a lan command.
 */
static struct lcs_buffer *
lcs_get_lancmd(struct lcs_card *card, int count)
{
	struct lcs_buffer *buffer;
	struct lcs_cmd *cmd;

	LCS_DBF_TEXT(4, trace, "getlncmd");
	/* Get buffer and wait if none is available. */
	wait_event(card->write.wait_q,
		   ((buffer = lcs_get_buffer(&card->write)) != NULL));
	count += sizeof(struct lcs_header);
	*(__u16 *)(buffer->data + count) = 0;
	buffer->count = count + sizeof(__u16);
	buffer->callback = lcs_release_buffer;
	cmd = (struct lcs_cmd *) buffer->data;
	cmd->offset = count;
	cmd->type = LCS_FRAME_TYPE_CONTROL;
	cmd->slot = 0;
	return buffer;
}

/**
 * Notifier function for lancmd replies. Called from read irq.
 */
static void
lcs_notify_lancmd_waiters(struct lcs_card *card, struct lcs_cmd *cmd)
{
	struct list_head *l, *n;
	struct lcs_reply *reply;

	LCS_DBF_TEXT(4, trace, "notiwait");
	spin_lock(&card->lock);
	list_for_each_safe(l, n, &card->lancmd_waiters) {
		reply = list_entry(l, struct lcs_reply, list);
		if (reply->sequence_no == cmd->sequence_no) {
			list_del(&reply->list);
			if (reply->callback != NULL)
				reply->callback(card, cmd);
			reply->received = 1;
			reply->rc = cmd->return_code;
			wake_up(&reply->wait_q);
			break;
		}
	}
	spin_unlock(&card->lock);
}

/**
 * Emit buffer of a lan comand.
 */
void
lcs_lancmd_timeout(unsigned long data)
{
	struct lcs_reply *reply;

	LCS_DBF_TEXT(4, trace, "timeout");
	reply = (struct lcs_reply *) data;
	list_del(&reply->list);
	reply->received = 1;
	reply->rc = -ETIME;
	wake_up(&reply->wait_q);
}

static int
lcs_send_lancmd(struct lcs_card *card, struct lcs_buffer *buffer,
		void (*reply_callback)(struct lcs_card *, struct lcs_cmd *))
{
	struct lcs_reply reply;
	struct lcs_cmd *cmd;
	struct timer_list timer;
	int rc;

	LCS_DBF_TEXT(4, trace, "sendcmd");
	cmd = (struct lcs_cmd *) buffer->data;
	cmd->sequence_no = ++card->sequence_no;
	cmd->return_code = 0;
	reply.sequence_no = cmd->sequence_no;
	reply.callback = reply_callback;
	reply.received = 0;
	reply.rc = 0;
	init_waitqueue_head(&reply.wait_q);
	spin_lock(&card->lock);
	list_add_tail(&reply.list, &card->lancmd_waiters);
	spin_unlock(&card->lock);
	buffer->callback = lcs_release_buffer;
	rc = lcs_ready_buffer(&card->write, buffer);
	if (rc)
		return rc;
	init_timer(&timer);
	timer.function = lcs_lancmd_timeout;
	timer.data = (unsigned long) &reply;
	timer.expires = jiffies + HZ*card->lancmd_timeout;
	add_timer(&timer);
	wait_event(reply.wait_q, reply.received);
	del_timer(&timer);
	LCS_DBF_TEXT_(4, trace, "rc:%d",reply.rc);
	return reply.rc ? -EIO : 0;
}

/**
 * LCS startup command
 */
static int
lcs_send_startup(struct lcs_card *card, __u8 initiator)
{
	struct lcs_buffer *buffer;
	struct lcs_cmd *cmd;

	LCS_DBF_TEXT(2, trace, "startup");
	buffer = lcs_get_lancmd(card, LCS_STD_CMD_SIZE);
	cmd = (struct lcs_cmd *) buffer->data;
	cmd->cmd_code = LCS_CMD_STARTUP;
	cmd->initiator = initiator;
	cmd->cmd.lcs_startup.buff_size = LCS_IOBUFFERSIZE;
	return lcs_send_lancmd(card, buffer, NULL);
}

/**
 * LCS shutdown command
 */
static int
lcs_send_shutdown(struct lcs_card *card)
{
	struct lcs_buffer *buffer;
	struct lcs_cmd *cmd;

	LCS_DBF_TEXT(2, trace, "shutdown");
	buffer = lcs_get_lancmd(card, LCS_STD_CMD_SIZE);
	cmd = (struct lcs_cmd *) buffer->data;
	cmd->cmd_code = LCS_CMD_SHUTDOWN;
	cmd->initiator = LCS_INITIATOR_TCPIP;
	return lcs_send_lancmd(card, buffer, NULL);
}

/**
 * LCS lanstat command
 */
static void
__lcs_lanstat_cb(struct lcs_card *card, struct lcs_cmd *cmd)
{
	LCS_DBF_TEXT(2, trace, "statcb");
	memcpy(card->mac, cmd->cmd.lcs_lanstat_cmd.mac_addr, LCS_MAC_LENGTH);
}

static int
lcs_send_lanstat(struct lcs_card *card)
{
	struct lcs_buffer *buffer;
	struct lcs_cmd *cmd;

	LCS_DBF_TEXT(2,trace, "cmdstat");
	buffer = lcs_get_lancmd(card, LCS_STD_CMD_SIZE);
	cmd = (struct lcs_cmd *) buffer->data;
	/* Setup lanstat command. */
	cmd->cmd_code = LCS_CMD_LANSTAT;
	cmd->initiator = LCS_INITIATOR_TCPIP;
	cmd->cmd.lcs_std_cmd.lan_type = card->lan_type;
	cmd->cmd.lcs_std_cmd.portno = card->portno;
	return lcs_send_lancmd(card, buffer, __lcs_lanstat_cb);
}

/**
 * send stoplan command
 */
static int
lcs_send_stoplan(struct lcs_card *card, __u8 initiator)
{
	struct lcs_buffer *buffer;
	struct lcs_cmd *cmd;

	LCS_DBF_TEXT(2, trace, "cmdstpln");
	buffer = lcs_get_lancmd(card, LCS_STD_CMD_SIZE);
	cmd = (struct lcs_cmd *) buffer->data;
	cmd->cmd_code = LCS_CMD_STOPLAN;
	cmd->initiator = initiator;
	cmd->cmd.lcs_std_cmd.lan_type = card->lan_type;
	cmd->cmd.lcs_std_cmd.portno = card->portno;
	return lcs_send_lancmd(card, buffer, NULL);
}

/**
 * send startlan command
 */
static void
__lcs_send_startlan_cb(struct lcs_card *card, struct lcs_cmd *cmd)
{
	LCS_DBF_TEXT(2, trace, "srtlancb");
	card->lan_type = cmd->cmd.lcs_std_cmd.lan_type;
	card->portno = cmd->cmd.lcs_std_cmd.portno;
}

static int
lcs_send_startlan(struct lcs_card *card, __u8 initiator)
{
	struct lcs_buffer *buffer;
	struct lcs_cmd *cmd;

	LCS_DBF_TEXT(2, trace, "cmdstaln");
	buffer = lcs_get_lancmd(card, LCS_STD_CMD_SIZE);
	cmd = (struct lcs_cmd *) buffer->data;
	cmd->cmd_code = LCS_CMD_STARTLAN;
	cmd->initiator = initiator;
	cmd->cmd.lcs_std_cmd.lan_type = card->lan_type;
	cmd->cmd.lcs_std_cmd.portno = card->portno;
	return lcs_send_lancmd(card, buffer, __lcs_send_startlan_cb);
}

#ifdef CONFIG_IP_MULTICAST
/**
 * send setipm command (Multicast)
 */
static int
lcs_send_setipm(struct lcs_card *card,struct lcs_ipm_list *ipm_list)
{
	struct lcs_buffer *buffer;
	struct lcs_cmd *cmd;

	LCS_DBF_TEXT(2, trace, "cmdsetim");
	buffer = lcs_get_lancmd(card, LCS_MULTICAST_CMD_SIZE);
	cmd = (struct lcs_cmd *) buffer->data;
	cmd->cmd_code = LCS_CMD_SETIPM;
	cmd->initiator = LCS_INITIATOR_TCPIP;
	cmd->cmd.lcs_qipassist.lan_type = card->lan_type;
	cmd->cmd.lcs_qipassist.portno = card->portno;
	cmd->cmd.lcs_qipassist.version = 4;
	cmd->cmd.lcs_qipassist.num_ip_pairs = 1;
	memcpy(cmd->cmd.lcs_qipassist.lcs_ipass_ctlmsg.ip_mac_pair,
	       &ipm_list->ipm, sizeof (struct lcs_ip_mac_pair));
	LCS_DBF_TEXT_(2, trace, "%x",ipm_list->ipm.ip_addr);
	return lcs_send_lancmd(card, buffer, NULL);
}

/**
 * send delipm command (Multicast)
 */
static int
lcs_send_delipm(struct lcs_card *card,struct lcs_ipm_list *ipm_list)
{
	struct lcs_buffer *buffer;
	struct lcs_cmd *cmd;

	LCS_DBF_TEXT(2, trace, "cmddelim");
	buffer = lcs_get_lancmd(card, LCS_MULTICAST_CMD_SIZE);
	cmd = (struct lcs_cmd *) buffer->data;
	cmd->cmd_code = LCS_CMD_DELIPM;
	cmd->initiator = LCS_INITIATOR_TCPIP;
	cmd->cmd.lcs_qipassist.lan_type = card->lan_type;
	cmd->cmd.lcs_qipassist.portno = card->portno;
	cmd->cmd.lcs_qipassist.version = 4;
	cmd->cmd.lcs_qipassist.num_ip_pairs = 1;
	memcpy(cmd->cmd.lcs_qipassist.lcs_ipass_ctlmsg.ip_mac_pair,
	       &ipm_list->ipm, sizeof (struct lcs_ip_mac_pair));
	LCS_DBF_TEXT_(2, trace, "%x",ipm_list->ipm.ip_addr);
	return lcs_send_lancmd(card, buffer, NULL);
}

/**
 * check if multicast is supported by LCS
 */
static void
__lcs_check_multicast_cb(struct lcs_card *card, struct lcs_cmd *cmd)
{
	LCS_DBF_TEXT(2, trace, "chkmccb");
	card->ip_assists_supported =
		cmd->cmd.lcs_qipassist.ip_assists_supported;
	card->ip_assists_enabled =
		cmd->cmd.lcs_qipassist.ip_assists_enabled;
}

static int
lcs_check_multicast_support(struct lcs_card *card)
{
	struct lcs_buffer *buffer;
	struct lcs_cmd *cmd;
	int rc;

	LCS_DBF_TEXT(2, trace, "cmdqipa");
	/* Send query ipassist. */
	buffer = lcs_get_lancmd(card, LCS_STD_CMD_SIZE);
	cmd = (struct lcs_cmd *) buffer->data;
	cmd->cmd_code = LCS_CMD_QIPASSIST;
	cmd->initiator = LCS_INITIATOR_TCPIP;
	cmd->cmd.lcs_qipassist.lan_type = card->lan_type;
	cmd->cmd.lcs_qipassist.portno = card->portno;
	cmd->cmd.lcs_qipassist.version = 4;
	cmd->cmd.lcs_qipassist.num_ip_pairs = 1;
	rc = lcs_send_lancmd(card, buffer, __lcs_check_multicast_cb);
	if (rc != 0) {
		PRINT_ERR("Query IPAssist failed. Assuming unsupported!\n");
		return -EOPNOTSUPP;
	}
	/* Print out supported assists: IPv6 */
	PRINT_INFO("LCS device %s %s IPv6 support\n", card->dev->name,
		   (card->ip_assists_supported & LCS_IPASS_IPV6_SUPPORT) ?
		   "with" : "without");
	/* Print out supported assist: Multicast */
	PRINT_INFO("LCS device %s %s Multicast support\n", card->dev->name,
		   (card->ip_assists_supported & LCS_IPASS_MULTICAST_SUPPORT) ?
		   "with" : "without");
	if (card->ip_assists_supported & LCS_IPASS_MULTICAST_SUPPORT)
		return 0;
	return -EOPNOTSUPP;
}

/**
 * set or del multicast address on LCS card
 */
static void
lcs_fix_multicast_list(struct lcs_card *card)
{
	struct list_head *l, *n;
	struct lcs_ipm_list *ipm;

	LCS_DBF_TEXT(4,trace, "fixipm");
	spin_lock(&card->ipm_lock);
	list_for_each_safe(l, n, &card->ipm_list) {
		ipm = list_entry(l, struct lcs_ipm_list, list);
		switch (ipm->ipm_state) {
		case LCS_IPM_STATE_SET_REQUIRED:
			if (lcs_send_setipm(card, ipm))
				PRINT_INFO("Adding multicast address failed."
					   "Table possibly full!\n");
			else
				ipm->ipm_state = LCS_IPM_STATE_ON_CARD;
			break;
		case LCS_IPM_STATE_DEL_REQUIRED:
			lcs_send_delipm(card, ipm);
			list_del(&ipm->list);
			kfree(ipm);
			break;
		case LCS_IPM_STATE_ON_CARD:
			break;
		}
	}
	if (card->state == DEV_STATE_UP)
		netif_wake_queue(card->dev);
	spin_unlock(&card->ipm_lock);
}

/**
 * get mac address for the relevant Multicast address
 */
static void
lcs_get_mac_for_ipm(__u32 ipm, char *mac, struct net_device *dev)
{
	LCS_DBF_TEXT(4,trace, "getmac");
	if (dev->type == ARPHRD_IEEE802_TR)
		ip_tr_mc_map(ipm, mac);
	else
		ip_eth_mc_map(ipm, mac);
}

/**
 * function called by net device to handle multicast address relevant things
 */
static int
lcs_register_mc_addresses(void *data)
{
	struct lcs_card *card;
	char buf[MAX_ADDR_LEN];
	struct list_head *l;
	struct ip_mc_list *im4;
	struct in_device *in4_dev;
	struct lcs_ipm_list *ipm, *tmp;

	daemonize("regipm");
	LCS_DBF_TEXT(4, trace, "regmulti");

	card = (struct lcs_card *) data;
	in4_dev = in_dev_get(card->dev);
	if (in4_dev == NULL)
		return 0;
	read_lock(&in4_dev->lock);
	spin_lock(&card->ipm_lock);
	/* Check for multicast addresses to be removed. */
	list_for_each(l, &card->ipm_list) {
		ipm = list_entry(l, struct lcs_ipm_list, list);
		for (im4 = in4_dev->mc_list; im4 != NULL; im4 = im4->next) {
			lcs_get_mac_for_ipm(im4->multiaddr, buf, card->dev);
			if (memcmp(buf, &ipm->ipm.mac_addr,
				   LCS_MAC_LENGTH) == 0 &&
			    ipm->ipm.ip_addr == im4->multiaddr)
				break;
		}
		if (im4 == NULL)
			ipm->ipm_state = LCS_IPM_STATE_DEL_REQUIRED;
	}
	/* Check for multicast addresses to be added. */
	for (im4 = in4_dev->mc_list; im4; im4 = im4->next) {
		lcs_get_mac_for_ipm(im4->multiaddr, buf, card->dev);
		ipm = NULL;
		list_for_each(l, &card->ipm_list) {
			tmp = list_entry(l, struct lcs_ipm_list, list);
			if (memcmp(buf, &tmp->ipm.mac_addr,
				   LCS_MAC_LENGTH) == 0 &&
			    tmp->ipm.ip_addr == im4->multiaddr) {
				ipm = tmp;
				break;
			}
		}
		if (ipm != NULL)
			continue;	/* Address already in list. */
		ipm = (struct lcs_ipm_list *)
			kmalloc(sizeof(struct lcs_ipm_list), GFP_ATOMIC);
		if (ipm == NULL) {
			PRINT_INFO("Not enough memory to add "
				   "new multicast entry!\n");
			break;
		}
		memset(ipm, 0, sizeof(struct lcs_ipm_list));
		memcpy(&ipm->ipm.mac_addr, buf, LCS_MAC_LENGTH);
		ipm->ipm.ip_addr = im4->multiaddr;
		ipm->ipm_state = LCS_IPM_STATE_SET_REQUIRED;
		list_add(&ipm->list, &card->ipm_list);
	}
	spin_unlock(&card->ipm_lock);
	read_unlock(&in4_dev->lock);
	in_dev_put(in4_dev);
	lcs_fix_multicast_list(card);
	return 0;
}
/**
 * function called by net device to
 * handle multicast address relevant things
 */
static void
lcs_set_multicast_list(struct net_device *dev)
{
        struct lcs_card *card;

        LCS_DBF_TEXT(4, trace, "setmulti");
        card = (struct lcs_card *) dev->priv;
        set_bit(3, &card->thread_mask);
        schedule_work(&card->kernel_thread_starter);
}

#endif /* CONFIG_IP_MULTICAST */

static long
lcs_check_irb_error(struct ccw_device *cdev, struct irb *irb)
{
	if (!IS_ERR(irb))
		return 0;

	switch (PTR_ERR(irb)) {
	case -EIO:
		PRINT_WARN("i/o-error on device %s\n", cdev->dev.bus_id);
		LCS_DBF_TEXT(2, trace, "ckirberr");
		LCS_DBF_TEXT_(2, trace, "  rc%d", -EIO);
		break;
	case -ETIMEDOUT:
		PRINT_WARN("timeout on device %s\n", cdev->dev.bus_id);
		LCS_DBF_TEXT(2, trace, "ckirberr");
		LCS_DBF_TEXT_(2, trace, "  rc%d", -ETIMEDOUT);
		break;
	default:
		PRINT_WARN("unknown error %ld on device %s\n", PTR_ERR(irb),
			   cdev->dev.bus_id);
		LCS_DBF_TEXT(2, trace, "ckirberr");
		LCS_DBF_TEXT(2, trace, "  rc???");
	}
	return PTR_ERR(irb);
}


/**
 * IRQ Handler for LCS channels
 */
static void
lcs_irq(struct ccw_device *cdev, unsigned long intparm, struct irb *irb)
{
	struct lcs_card *card;
	struct lcs_channel *channel;
	int index;

	if (lcs_check_irb_error(cdev, irb))
		return;

	card = CARD_FROM_DEV(cdev);
	if (card->read.ccwdev == cdev)
		channel = &card->read;
	else
		channel = &card->write;

	LCS_DBF_TEXT_(5, trace, "Rint%s",cdev->dev.bus_id);
	LCS_DBF_TEXT_(5, trace, "%4x%4x",irb->scsw.cstat, irb->scsw.dstat);

	/* How far in the ccw chain have we processed? */
	if ((channel->state != CH_STATE_INIT) &&
	    (irb->scsw.fctl & SCSW_FCTL_START_FUNC)) {
		index = (struct ccw1 *) __va((addr_t) irb->scsw.cpa) 
			- channel->ccws;
		if ((irb->scsw.actl & SCSW_ACTL_SUSPENDED) ||
		    (irb->scsw.cstat | SCHN_STAT_PCI))
			/* Bloody io subsystem tells us lies about cpa... */
			index = (index - 1) & (LCS_NUM_BUFFS - 1);
		while (channel->io_idx != index) {
			__lcs_processed_buffer(channel,
					       channel->iob + channel->io_idx);
			channel->io_idx =
				(channel->io_idx + 1) & (LCS_NUM_BUFFS - 1);
		}
	}

	if ((irb->scsw.dstat & DEV_STAT_DEV_END) ||
	    (irb->scsw.dstat & DEV_STAT_CHN_END) ||
	    (irb->scsw.dstat & DEV_STAT_UNIT_CHECK))
		/* Mark channel as stopped. */
		channel->state = CH_STATE_STOPPED;
	else if (irb->scsw.actl & SCSW_ACTL_SUSPENDED)
		/* CCW execution stopped on a suspend bit. */
		channel->state = CH_STATE_SUSPENDED;

	if (irb->scsw.fctl & SCSW_FCTL_HALT_FUNC) {
		if (irb->scsw.cc != 0) {
			ccw_device_halt(channel->ccwdev, (addr_t) channel);
			return;
		}
		/* The channel has been stopped by halt_IO. */
		channel->state = CH_STATE_HALTED;
	}

	if (irb->scsw.fctl & SCSW_FCTL_CLEAR_FUNC) {
		channel->state = CH_STATE_CLEARED;
	}
	/* Do the rest in the tasklet. */
	tasklet_schedule(&channel->irq_tasklet);
}

/**
 * Tasklet for IRQ handler
 */
static void
lcs_tasklet(unsigned long data)
{
	unsigned long flags;
	struct lcs_channel *channel;
	struct lcs_buffer *iob;
	int buf_idx;
	int rc;

	channel = (struct lcs_channel *) data;
	LCS_DBF_TEXT_(5, trace, "tlet%s",channel->ccwdev->dev.bus_id);

	/* Check for processed buffers. */
	iob = channel->iob;
	buf_idx = channel->buf_idx;
	while (iob[buf_idx].state == BUF_STATE_PROCESSED) {
		/* Do the callback thing. */
		if (iob[buf_idx].callback != NULL)
			iob[buf_idx].callback(channel, iob + buf_idx);
		buf_idx = (buf_idx + 1) & (LCS_NUM_BUFFS - 1);
	}
	channel->buf_idx = buf_idx;

	if (channel->state == CH_STATE_STOPPED)
		// FIXME: what if rc != 0 ??
		rc = lcs_start_channel(channel);
	spin_lock_irqsave(get_ccwdev_lock(channel->ccwdev), flags);
	if (channel->state == CH_STATE_SUSPENDED &&
	    channel->iob[channel->io_idx].state == BUF_STATE_READY) {
		// FIXME: what if rc != 0 ??
		rc = __lcs_resume_channel(channel);
	}
	spin_unlock_irqrestore(get_ccwdev_lock(channel->ccwdev), flags);

	/* Something happened on the channel. Wake up waiters. */
	wake_up(&channel->wait_q);
}

/**
 * Finish current tx buffer and make it ready for transmit.
 */
static void
__lcs_emit_txbuffer(struct lcs_card *card)
{
	LCS_DBF_TEXT(5, trace, "emittx");
	*(__u16 *)(card->tx_buffer->data + card->tx_buffer->count) = 0;
	card->tx_buffer->count += 2;
	lcs_ready_buffer(&card->write, card->tx_buffer);
	card->tx_buffer = NULL;
	card->tx_emitted++;
}

/**
 * Callback for finished tx buffers.
 */
static void
lcs_txbuffer_cb(struct lcs_channel *channel, struct lcs_buffer *buffer)
{
	struct lcs_card *card;

	LCS_DBF_TEXT(5, trace, "txbuffcb");
	/* Put buffer back to pool. */
	lcs_release_buffer(channel, buffer);
	card = (struct lcs_card *)
		((char *) channel - offsetof(struct lcs_card, write));
	spin_lock(&card->lock);
	card->tx_emitted--;
	if (card->tx_emitted <= 0 && card->tx_buffer != NULL)
		/*
		 * Last running tx buffer has finished. Submit partially
		 * filled current buffer.
		 */
		__lcs_emit_txbuffer(card);
	spin_unlock(&card->lock);
}

/**
 * Packet transmit function called by network stack
 */
static int
__lcs_start_xmit(struct lcs_card *card, struct sk_buff *skb,
		 struct net_device *dev)
{
	struct lcs_header *header;

	LCS_DBF_TEXT(5, trace, "hardxmit");
	if (skb == NULL) {
		card->stats.tx_dropped++;
		card->stats.tx_errors++;
		return -EIO;
	}
	if (card->state != DEV_STATE_UP) {
		dev_kfree_skb(skb);
		card->stats.tx_dropped++;
		card->stats.tx_errors++;
		card->stats.tx_carrier_errors++;
		return 0;
	}
	if (netif_queue_stopped(dev) ) {
		card->stats.tx_dropped++;
		return -EBUSY;
	}
	if (card->tx_buffer != NULL &&
	    card->tx_buffer->count + sizeof(struct lcs_header) +
	    skb->len + sizeof(u16) > LCS_IOBUFFERSIZE)
		/* skb too big for current tx buffer. */
		__lcs_emit_txbuffer(card);
	if (card->tx_buffer == NULL) {
		/* Get new tx buffer */
		card->tx_buffer = lcs_get_buffer(&card->write);
		if (card->tx_buffer == NULL) {
			card->stats.tx_dropped++;
			return -EBUSY;
		}
		card->tx_buffer->callback = lcs_txbuffer_cb;
		card->tx_buffer->count = 0;
	}
	header = (struct lcs_header *)
		(card->tx_buffer->data + card->tx_buffer->count);
	card->tx_buffer->count += skb->len + sizeof(struct lcs_header);
	header->offset = card->tx_buffer->count;
	header->type = card->lan_type;
	header->slot = card->portno;
	memcpy(header + 1, skb->data, skb->len);
	card->stats.tx_bytes += skb->len;
	card->stats.tx_packets++;
	dev_kfree_skb(skb);
	if (card->tx_emitted <= 0)
		/* If this is the first tx buffer emit it immediately. */
		__lcs_emit_txbuffer(card);
	return 0;
}

static int
lcs_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct lcs_card *card;
	int rc;

	LCS_DBF_TEXT(5, trace, "pktxmit");
	card = (struct lcs_card *) dev->priv;
	spin_lock(&card->lock);
	rc = __lcs_start_xmit(card, skb, dev);
	spin_unlock(&card->lock);
	return rc;
}

/**
 * send startlan and lanstat command to make LCS device ready
 */
static int
lcs_startlan_auto(struct lcs_card *card)
{
	int rc;

	LCS_DBF_TEXT(2, trace, "strtauto");
#ifdef CONFIG_NET_ETHERNET
	card->lan_type = LCS_FRAME_TYPE_ENET;
	rc = lcs_send_startlan(card, LCS_INITIATOR_TCPIP);
	if (rc == 0)
		return 0;

#endif
#ifdef CONFIG_TR
	card->lan_type = LCS_FRAME_TYPE_TR;
	rc = lcs_send_startlan(card, LCS_INITIATOR_TCPIP);
	if (rc == 0)
		return 0;
#endif
#ifdef CONFIG_FDDI
	card->lan_type = LCS_FRAME_TYPE_FDDI;
	rc = lcs_send_startlan(card, LCS_INITIATOR_TCPIP);
	if (rc == 0)
		return 0;
#endif
	return -EIO;
}

static int
lcs_startlan(struct lcs_card *card)
{
	int rc, i;

	LCS_DBF_TEXT(2, trace, "startlan");
	rc = 0;
	if (card->portno != LCS_INVALID_PORT_NO) {
		if (card->lan_type == LCS_FRAME_TYPE_AUTO)
			rc = lcs_startlan_auto(card);
		else
			rc = lcs_send_startlan(card, LCS_INITIATOR_TCPIP);
	} else {
                for (i = 0; i <= 16; i++) {
                        card->portno = i;
                        if (card->lan_type != LCS_FRAME_TYPE_AUTO)
                                rc = lcs_send_startlan(card,
                                                       LCS_INITIATOR_TCPIP);
                        else
                                /* autodetecting lan type */
                                rc = lcs_startlan_auto(card);
                        if (rc == 0)
                                break;
                }
        }
	if (rc == 0)
		return lcs_send_lanstat(card);
	return rc;
}

/**
 * LCS detect function
 * setup channels and make them I/O ready
 */
static int
lcs_detect(struct lcs_card *card)
{
	int rc = 0;

	LCS_DBF_TEXT(2, setup, "lcsdetct");
	/* start/reset card */
	if (card->dev)
		netif_stop_queue(card->dev);
	card->write.state = CH_STATE_INIT;
	card->read.state = CH_STATE_INIT;
	rc = lcs_stop_channels(card);
	if (rc == 0) {
		rc = lcs_start_channels(card);
		if (rc == 0) {
			rc = lcs_send_startup(card, LCS_INITIATOR_TCPIP);
			if (rc == 0)
				rc = lcs_startlan(card);
		}
	}
	if (rc == 0) {
		card->state = DEV_STATE_UP;
	} else {
		card->state = DEV_STATE_DOWN;
		card->write.state = CH_STATE_INIT;
		card->read.state =  CH_STATE_INIT;
	}
	return rc;
}

/**
 * reset card
 */
static int
lcs_resetcard(struct lcs_card *card)
{
	int retries;

	LCS_DBF_TEXT(2, trace, "rescard");
	for (retries = 0; retries < 10; retries++) {
		if (lcs_detect(card) == 0) {
			netif_wake_queue(card->dev);
			card->state = DEV_STATE_UP;
			PRINT_INFO("LCS device %s successfully restarted!\n",
				   card->dev->name);
			return 0;
		}
		msleep(3000);
	}
	PRINT_ERR("Error in Reseting LCS card!\n");
	return -EIO;
}

/**
 * LCS Stop card
 */
static int
lcs_stopcard(struct lcs_card *card)
{
	int rc;

	LCS_DBF_TEXT(3, setup, "stopcard");

	if (card->read.state != CH_STATE_STOPPED &&
	    card->write.state != CH_STATE_STOPPED &&
	    card->state == DEV_STATE_UP) {
		rc = lcs_send_stoplan(card,LCS_INITIATOR_TCPIP);
		rc = lcs_send_shutdown(card);
	}
	rc = lcs_stop_channels(card);
	card->state = DEV_STATE_DOWN;

	return rc;
}

/**
 * LGW initiated commands
 */
static int
lcs_lgw_startlan_thread(void *data)
{
	struct lcs_card *card;

	card = (struct lcs_card *) data;
	daemonize("lgwstpln");
	LCS_DBF_TEXT(4, trace, "lgwstpln");
	if (card->dev)
		netif_stop_queue(card->dev);
	if (lcs_startlan(card) == 0) {
		netif_wake_queue(card->dev);
		card->state = DEV_STATE_UP;
		PRINT_INFO("LCS Startlan for device %s succeeded!\n",
			   card->dev->name);

	} else
		PRINT_ERR("LCS Startlan for device %s failed!\n",
			  card->dev->name);
	return 0;
}

/**
 * Send startup command initiated by Lan Gateway
 */
static int
lcs_lgw_startup_thread(void *data)
{
	int rc;

	struct lcs_card *card;

	card = (struct lcs_card *) data;
	daemonize("lgwstpln");
	LCS_DBF_TEXT(4, trace, "lgwstpln");
	if (card->dev)
		netif_stop_queue(card->dev);
	rc = lcs_send_startup(card, LCS_INITIATOR_LGW);
	if (rc != 0) {
		PRINT_ERR("Startup for LCS device %s initiated " \
			  "by LGW failed!\nReseting card ...\n",
			  card->dev->name);
		/* do a card reset */
		rc = lcs_resetcard(card);
		if (rc == 0)
			goto Done;
	}
	rc = lcs_startlan(card);
	if (rc == 0) {
		netif_wake_queue(card->dev);
		card->state = DEV_STATE_UP;
	}
Done:
	if (rc == 0)
		PRINT_INFO("LCS Startup for device %s succeeded!\n",
			   card->dev->name);
	else
		PRINT_ERR("LCS Startup for device %s failed!\n",
			  card->dev->name);
	return 0;
}


/**
 * send stoplan command initiated by Lan Gateway
 */
static int
lcs_lgw_stoplan_thread(void *data)
{
	struct lcs_card *card;
	int rc;

	card = (struct lcs_card *) data;
	daemonize("lgwstop");
	LCS_DBF_TEXT(4, trace, "lgwstop");
	if (card->dev)
		netif_stop_queue(card->dev);
	if (lcs_send_stoplan(card, LCS_INITIATOR_LGW) == 0)
		PRINT_INFO("Stoplan for %s initiated by LGW succeeded!\n",
			   card->dev->name);
	else
		PRINT_ERR("Stoplan %s initiated by LGW failed!\n",
			  card->dev->name);
	/*Try to reset the card, stop it on failure */
        rc = lcs_resetcard(card);
        if (rc != 0)
                rc = lcs_stopcard(card);
        return rc;
}

/**
 * Kernel Thread helper functions for LGW initiated commands
 */
static void
lcs_start_kernel_thread(struct lcs_card *card)
{
	LCS_DBF_TEXT(5, trace, "krnthrd");
	if (test_and_clear_bit(0, &card->thread_mask))
		kernel_thread(lcs_lgw_startup_thread, (void *) card, SIGCHLD);
	if (test_and_clear_bit(1, &card->thread_mask))
		kernel_thread(lcs_lgw_startlan_thread, (void *) card, SIGCHLD);
	if (test_and_clear_bit(2, &card->thread_mask))
		kernel_thread(lcs_lgw_stoplan_thread, (void *) card, SIGCHLD);
#ifdef CONFIG_IP_MULTICAST
	if (test_and_clear_bit(3, &card->thread_mask))
		kernel_thread(lcs_register_mc_addresses, (void *) card, SIGCHLD);
#endif
}

/**
 * Process control frames.
 */
static void
lcs_get_control(struct lcs_card *card, struct lcs_cmd *cmd)
{
	LCS_DBF_TEXT(5, trace, "getctrl");
	if (cmd->initiator == LCS_INITIATOR_LGW) {
		switch(cmd->cmd_code) {
		case LCS_CMD_STARTUP:
			set_bit(0, &card->thread_mask);
			schedule_work(&card->kernel_thread_starter);
			break;
		case LCS_CMD_STARTLAN:
			set_bit(1, &card->thread_mask);
			schedule_work(&card->kernel_thread_starter);
			break;
		case LCS_CMD_STOPLAN:
			set_bit(2, &card->thread_mask);
			schedule_work(&card->kernel_thread_starter);
			break;
		default:
			PRINT_INFO("UNRECOGNIZED LGW COMMAND\n");
			break;
		}
	} else
		lcs_notify_lancmd_waiters(card, cmd);
}

/**
 * Unpack network packet.
 */
static void
lcs_get_skb(struct lcs_card *card, char *skb_data, unsigned int skb_len)
{
	struct sk_buff *skb;

	LCS_DBF_TEXT(5, trace, "getskb");
	if (card->dev == NULL ||
	    card->state != DEV_STATE_UP)
		/* The card isn't up. Ignore the packet. */
		return;

	skb = dev_alloc_skb(skb_len);
	if (skb == NULL) {
		PRINT_ERR("LCS: alloc_skb failed for device=%s\n",
			  card->dev->name);
		card->stats.rx_dropped++;
		return;
	}
	skb->dev = card->dev;
	memcpy(skb_put(skb, skb_len), skb_data, skb_len);
	skb->protocol =	card->lan_type_trans(skb, card->dev);
	card->stats.rx_bytes += skb_len;
	card->stats.rx_packets++;
	netif_rx(skb);
}

/**
 * LCS main routine to get packets and lancmd replies from the buffers
 */
static void
lcs_get_frames_cb(struct lcs_channel *channel, struct lcs_buffer *buffer)
{
	struct lcs_card *card;
	struct lcs_header *lcs_hdr;
	__u16 offset;

	LCS_DBF_TEXT(5, trace, "lcsgtpkt");
	lcs_hdr = (struct lcs_header *) buffer->data;
	if (lcs_hdr->offset == LCS_ILLEGAL_OFFSET) {
		LCS_DBF_TEXT(4, trace, "-eiogpkt");
		return;
	}
	card = (struct lcs_card *)
		((char *) channel - offsetof(struct lcs_card, read));
	offset = 0;
	while (lcs_hdr->offset != 0) {
		if (lcs_hdr->offset <= 0 ||
		    lcs_hdr->offset > LCS_IOBUFFERSIZE ||
		    lcs_hdr->offset < offset) {
			/* Offset invalid. */
			card->stats.rx_length_errors++;
			card->stats.rx_errors++;
			return;
		}
		/* What kind of frame is it? */
		if (lcs_hdr->type == LCS_FRAME_TYPE_CONTROL)
			/* Control frame. */
			lcs_get_control(card, (struct lcs_cmd *) lcs_hdr);
		else if (lcs_hdr->type == LCS_FRAME_TYPE_ENET ||
			 lcs_hdr->type == LCS_FRAME_TYPE_TR ||
			 lcs_hdr->type == LCS_FRAME_TYPE_FDDI)
			/* Normal network packet. */
			lcs_get_skb(card, (char *)(lcs_hdr + 1),
				    lcs_hdr->offset - offset -
				    sizeof(struct lcs_header));
		else
			/* Unknown frame type. */
			; // FIXME: error message ?
		/* Proceed to next frame. */
		offset = lcs_hdr->offset;
		lcs_hdr->offset = LCS_ILLEGAL_OFFSET;
		lcs_hdr = (struct lcs_header *) (buffer->data + offset);
	}
	/* The buffer is now empty. Make it ready again. */
	lcs_ready_buffer(&card->read, buffer);
}

/**
 * get network statistics for ifconfig and other user programs
 */
static struct net_device_stats *
lcs_getstats(struct net_device *dev)
{
	struct lcs_card *card;

	LCS_DBF_TEXT(4, trace, "netstats");
	card = (struct lcs_card *) dev->priv;
	return &card->stats;
}

/**
 * stop lcs device
 * This function will be called by user doing ifconfig xxx down
 */
static int
lcs_stop_device(struct net_device *dev)
{
	struct lcs_card *card;
	int rc;

	LCS_DBF_TEXT(2, trace, "stopdev");
	card   = (struct lcs_card *) dev->priv;
	netif_stop_queue(dev);
	dev->flags &= ~IFF_UP;
	rc = lcs_stopcard(card);
	if (rc)
		PRINT_ERR("Try it again!\n ");
	return rc;
}

/**
 * start lcs device and make it runnable
 * This function will be called by user doing ifconfig xxx up
 */
static int
lcs_open_device(struct net_device *dev)
{
	struct lcs_card *card;
	int rc;

	LCS_DBF_TEXT(2, trace, "opendev");
	card = (struct lcs_card *) dev->priv;
	/* initialize statistics */
	rc = lcs_detect(card);
	if (rc) {
		PRINT_ERR("LCS:Error in opening device!\n");

	} else {
		dev->flags |= IFF_UP;
		netif_wake_queue(dev);
		card->state = DEV_STATE_UP;
	}
	return rc;
}

/**
 * show function for portno called by cat or similar things
 */
static ssize_t
lcs_portno_show (struct device *dev, char *buf)
{
        struct lcs_card *card;

	card = (struct lcs_card *)dev->driver_data;

        if (!card)
                return 0;

        return sprintf(buf, "%d\n", card->portno);
}

/**
 * store the value which is piped to file portno
 */
static ssize_t
lcs_portno_store (struct device *dev, const char *buf, size_t count)
{
        struct lcs_card *card;
        int value;

	card = (struct lcs_card *)dev->driver_data;

        if (!card)
                return 0;

        sscanf(buf, "%u", &value);
        /* TODO: sanity checks */
        card->portno = value;

        return count;

}

static DEVICE_ATTR(portno, 0644, lcs_portno_show, lcs_portno_store);

static ssize_t
lcs_type_show(struct device *dev, char *buf)
{
	struct ccwgroup_device *cgdev;

	cgdev = to_ccwgroupdev(dev);
	if (!cgdev)
		return -ENODEV;

	return sprintf(buf, "%s\n", cu3088_type[cgdev->cdev[0]->id.driver_info]);
}

static DEVICE_ATTR(type, 0444, lcs_type_show, NULL);

static ssize_t
lcs_timeout_show(struct device *dev, char *buf)
{
	struct lcs_card *card;

	card = (struct lcs_card *)dev->driver_data;

	return card ? sprintf(buf, "%u\n", card->lancmd_timeout) : 0;
}

static ssize_t
lcs_timeout_store (struct device *dev, const char *buf, size_t count)
{
        struct lcs_card *card;
        int value;

	card = (struct lcs_card *)dev->driver_data;

        if (!card)
                return 0;

        sscanf(buf, "%u", &value);
        /* TODO: sanity checks */
        card->lancmd_timeout = value;

        return count;

}

DEVICE_ATTR(lancmd_timeout, 0644, lcs_timeout_show, lcs_timeout_store);

static struct attribute * lcs_attrs[] = {
	&dev_attr_portno.attr,
	&dev_attr_type.attr,
	&dev_attr_lancmd_timeout.attr,
	NULL,
};

static struct attribute_group lcs_attr_group = {
	.attrs = lcs_attrs,
};

/**
 * lcs_probe_device is called on establishing a new ccwgroup_device.
 */
static int
lcs_probe_device(struct ccwgroup_device *ccwgdev)
{
	struct lcs_card *card;
	int ret;

	if (!get_device(&ccwgdev->dev))
		return -ENODEV;

	LCS_DBF_TEXT(2, setup, "add_dev");
        card = lcs_alloc_card();
        if (!card) {
                PRINT_ERR("Allocation of lcs card failed\n");
		put_device(&ccwgdev->dev);
                return -ENOMEM;
        }
	ret = sysfs_create_group(&ccwgdev->dev.kobj, &lcs_attr_group);
	if (ret) {
                PRINT_ERR("Creating attributes failed");
		lcs_free_card(card);
		put_device(&ccwgdev->dev);
		return ret;
        }
	ccwgdev->dev.driver_data = card;
	ccwgdev->cdev[0]->handler = lcs_irq;
	ccwgdev->cdev[1]->handler = lcs_irq;
        return 0;
}

static int
lcs_register_netdev(struct ccwgroup_device *ccwgdev)
{
	struct lcs_card *card;

	LCS_DBF_TEXT(2, setup, "regnetdv");
	card = (struct lcs_card *)ccwgdev->dev.driver_data;
	if (card->dev->reg_state != NETREG_UNINITIALIZED)
		return 0;
	SET_NETDEV_DEV(card->dev, &ccwgdev->dev);
	return register_netdev(card->dev);
}

/**
 * lcs_new_device will be called by setting the group device online.
 */

static int
lcs_new_device(struct ccwgroup_device *ccwgdev)
{
	struct  lcs_card *card;
	struct net_device *dev=NULL;
	enum lcs_dev_states recover_state;
	int rc;

	card = (struct lcs_card *)ccwgdev->dev.driver_data;
	if (!card)
		return -ENODEV;

	LCS_DBF_TEXT(2, setup, "newdev");
	LCS_DBF_HEX(3, setup, &card, sizeof(void*));
	card->read.ccwdev  = ccwgdev->cdev[0];
	card->write.ccwdev = ccwgdev->cdev[1];

	recover_state = card->state;
	ccw_device_set_online(card->read.ccwdev);
	ccw_device_set_online(card->write.ccwdev);

	LCS_DBF_TEXT(3, setup, "lcsnewdv");

	lcs_setup_card(card);
	rc = lcs_detect(card);
	if (rc) {
		LCS_DBF_TEXT(2, setup, "dtctfail");
		PRINT_WARN("Detection of LCS card failed with return code "
			   "%d (0x%x)\n", rc, rc);
		lcs_stopcard(card);
		goto out;
	}
	if (card->dev) {
		LCS_DBF_TEXT(2, setup, "samedev");
		LCS_DBF_HEX(3, setup, &card, sizeof(void*));
		goto netdev_out;
	}
	switch (card->lan_type) {
#ifdef CONFIG_NET_ETHERNET
	case LCS_FRAME_TYPE_ENET:
		card->lan_type_trans = eth_type_trans;
		dev = alloc_etherdev(0);
		break;
#endif
#ifdef CONFIG_TR
	case LCS_FRAME_TYPE_TR:
		card->lan_type_trans = tr_type_trans;
		dev = alloc_trdev(0);
		break;
#endif
#ifdef CONFIG_FDDI
	case LCS_FRAME_TYPE_FDDI:
		card->lan_type_trans = fddi_type_trans;
		dev = alloc_fddidev(0);
		break;
#endif
	default:
		LCS_DBF_TEXT(3, setup, "errinit");
		PRINT_ERR("LCS: Initialization failed\n");
		PRINT_ERR("LCS: No device found!\n");
		goto out;
	}
	if (!dev)
		goto out;
	card->dev = dev;
netdev_out:
	card->dev->priv = card;
	card->dev->open = lcs_open_device;
	card->dev->stop = lcs_stop_device;
	card->dev->hard_start_xmit = lcs_start_xmit;
	card->dev->get_stats = lcs_getstats;
	SET_MODULE_OWNER(dev);
	if (lcs_register_netdev(ccwgdev) != 0)
		goto out;
	memcpy(card->dev->dev_addr, card->mac, LCS_MAC_LENGTH);
#ifdef CONFIG_IP_MULTICAST
	if (!lcs_check_multicast_support(card))
		card->dev->set_multicast_list = lcs_set_multicast_list;
#endif
	netif_stop_queue(card->dev);
	if (recover_state == DEV_STATE_RECOVER) {
		card->dev->flags |= IFF_UP;
		netif_wake_queue(card->dev);
		card->state = DEV_STATE_UP;
	} else
		lcs_stopcard(card);

	return 0;
out:

	ccw_device_set_offline(card->read.ccwdev);
	ccw_device_set_offline(card->write.ccwdev);
	return -ENODEV;
}

/**
 * lcs_shutdown_device, called when setting the group device offline.
 */
static int
lcs_shutdown_device(struct ccwgroup_device *ccwgdev)
{
	struct lcs_card *card;
	enum lcs_dev_states recover_state;
	int ret;

	LCS_DBF_TEXT(3, setup, "shtdndev");
	card = (struct lcs_card *)ccwgdev->dev.driver_data;
	if (!card)
		return -ENODEV;

	LCS_DBF_HEX(3, setup, &card, sizeof(void*));
	recover_state = card->state;

	ret = lcs_stop_device(card->dev);
	ret = ccw_device_set_offline(card->read.ccwdev);
	ret = ccw_device_set_offline(card->write.ccwdev);
	if (recover_state == DEV_STATE_UP) {
		card->state = DEV_STATE_RECOVER;
	}
	if (ret)
		return ret;
	return 0;
}

/**
 * lcs_remove_device, free buffers and card
 */
static void
lcs_remove_device(struct ccwgroup_device *ccwgdev)
{
	struct lcs_card *card;

	card = (struct lcs_card *)ccwgdev->dev.driver_data;
	if (!card)
		return;

	PRINT_INFO("Removing lcs group device ....\n");
	LCS_DBF_TEXT(3, setup, "remdev");
	LCS_DBF_HEX(3, setup, &card, sizeof(void*));
	if (ccwgdev->state == CCWGROUP_ONLINE) {
		lcs_shutdown_device(ccwgdev);
	}
	if (card->dev)
		unregister_netdev(card->dev);
	sysfs_remove_group(&ccwgdev->dev.kobj, &lcs_attr_group);
	lcs_cleanup_card(card);
	lcs_free_card(card);
	put_device(&ccwgdev->dev);
}

/**
 * LCS ccwgroup driver registration
 */
static struct ccwgroup_driver lcs_group_driver = {
	.owner       = THIS_MODULE,
	.name        = "lcs",
	.max_slaves  = 2,
	.driver_id   = 0xD3C3E2,
	.probe       = lcs_probe_device,
	.remove      = lcs_remove_device,
	.set_online  = lcs_new_device,
	.set_offline = lcs_shutdown_device,
};

/**
 *  LCS Module/Kernel initialization function
 */
static int
__init lcs_init_module(void)
{
	int rc;

	PRINT_INFO("Loading %s\n",version);
	rc = lcs_register_debug_facility();
	LCS_DBF_TEXT(0, setup, "lcsinit");
	if (rc) {
		PRINT_ERR("Initialization failed\n");
		return rc;
	}

	rc = register_cu3088_discipline(&lcs_group_driver);
	if (rc) {
		PRINT_ERR("Initialization failed\n");
		return rc;
	}

	return 0;
}


/**
 *  LCS module cleanup function
 */
static void
__exit lcs_cleanup_module(void)
{
	PRINT_INFO("Terminating lcs module.\n");
	LCS_DBF_TEXT(0, trace, "cleanup");
	unregister_cu3088_discipline(&lcs_group_driver);
	lcs_unregister_debug_facility();
}

module_init(lcs_init_module);
module_exit(lcs_cleanup_module);

MODULE_AUTHOR("Frank Pavlic <pavlic@de.ibm.com>");
MODULE_LICENSE("GPL");

