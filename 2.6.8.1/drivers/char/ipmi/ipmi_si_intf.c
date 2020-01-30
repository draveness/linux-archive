/*
 * ipmi_si.c
 *
 * The interface to the IPMI driver for the system interfaces (KCS, SMIC,
 * BT).
 *
 * Author: MontaVista Software, Inc.
 *         Corey Minyard <minyard@mvista.com>
 *         source@mvista.com
 *
 * Copyright 2002 MontaVista Software Inc.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or (at your
 *  option) any later version.
 *
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 *  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 *  TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *  USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * This file holds the "policy" for the interface to the SMI state
 * machine.  It does the configuration, handles timers and interrupts,
 * and drives the real SMI state machine.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <asm/system.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/ioport.h>
#include <asm/irq.h>
#ifdef CONFIG_HIGH_RES_TIMERS
#include <linux/hrtime.h>
# if defined(schedule_next_int)
/* Old high-res timer code, do translations. */
#  define get_arch_cycles(a) quick_update_jiffies_sub(a)
#  define arch_cycles_per_jiffy cycles_per_jiffies
# endif
static inline void add_usec_to_timer(struct timer_list *t, long v)
{
	t->sub_expires += nsec_to_arch_cycle(v * 1000);
	while (t->sub_expires >= arch_cycles_per_jiffy)
	{
		t->expires++;
		t->sub_expires -= arch_cycles_per_jiffy;
	}
}
#endif
#include <linux/interrupt.h>
#include <linux/rcupdate.h>
#include <linux/ipmi_smi.h>
#include <asm/io.h>
#include "ipmi_si_sm.h"
#include <linux/init.h>

#define IPMI_SI_VERSION "v32"

/* Measure times between events in the driver. */
#undef DEBUG_TIMING

/* Call every 10 ms. */
#define SI_TIMEOUT_TIME_USEC	10000
#define SI_USEC_PER_JIFFY	(1000000/HZ)
#define SI_TIMEOUT_JIFFIES	(SI_TIMEOUT_TIME_USEC/SI_USEC_PER_JIFFY)
#define SI_SHORT_TIMEOUT_USEC  250 /* .25ms when the SM request a
                                       short timeout */

enum si_intf_state {
	SI_NORMAL,
	SI_GETTING_FLAGS,
	SI_GETTING_EVENTS,
	SI_CLEARING_FLAGS,
	SI_CLEARING_FLAGS_THEN_SET_IRQ,
	SI_GETTING_MESSAGES,
	SI_ENABLE_INTERRUPTS1,
	SI_ENABLE_INTERRUPTS2
	/* FIXME - add watchdog stuff. */
};

enum si_type {
    SI_KCS, SI_SMIC, SI_BT
};

struct smi_info
{
	ipmi_smi_t             intf;
	struct si_sm_data      *si_sm;
	struct si_sm_handlers  *handlers;
	enum si_type           si_type;
	spinlock_t             si_lock;
	spinlock_t             msg_lock;
	struct list_head       xmit_msgs;
	struct list_head       hp_xmit_msgs;
	struct ipmi_smi_msg    *curr_msg;
	enum si_intf_state     si_state;

	/* Used to handle the various types of I/O that can occur with
           IPMI */
	struct si_sm_io io;
	int (*io_setup)(struct smi_info *info);
	void (*io_cleanup)(struct smi_info *info);
	int (*irq_setup)(struct smi_info *info);
	void (*irq_cleanup)(struct smi_info *info);
	unsigned int io_size;

	/* Flags from the last GET_MSG_FLAGS command, used when an ATTN
	   is set to hold the flags until we are done handling everything
	   from the flags. */
#define RECEIVE_MSG_AVAIL	0x01
#define EVENT_MSG_BUFFER_FULL	0x02
#define WDT_PRE_TIMEOUT_INT	0x08
	unsigned char       msg_flags;

	/* If set to true, this will request events the next time the
	   state machine is idle. */
	atomic_t            req_events;

	/* If true, run the state machine to completion on every send
	   call.  Generally used after a panic to make sure stuff goes
	   out. */
	int                 run_to_completion;

	/* The I/O port of an SI interface. */
	int                 port;

	/* zero if no irq; */
	int                 irq;

	/* The timer for this si. */
	struct timer_list   si_timer;

	/* The time (in jiffies) the last timeout occurred at. */
	unsigned long       last_timeout_jiffies;

	/* Used to gracefully stop the timer without race conditions. */
	volatile int        stop_operation;
	volatile int        timer_stopped;

	/* The driver will disable interrupts when it gets into a
	   situation where it cannot handle messages due to lack of
	   memory.  Once that situation clears up, it will re-enable
	   interrupts. */
	int interrupt_disabled;

	unsigned char ipmi_si_dev_rev;
	unsigned char ipmi_si_fw_rev_major;
	unsigned char ipmi_si_fw_rev_minor;
	unsigned char ipmi_version_major;
	unsigned char ipmi_version_minor;

	/* Counters and things for the proc filesystem. */
	spinlock_t count_lock;
	unsigned long short_timeouts;
	unsigned long long_timeouts;
	unsigned long timeout_restarts;
	unsigned long idles;
	unsigned long interrupts;
	unsigned long attentions;
	unsigned long flag_fetches;
	unsigned long hosed_count;
	unsigned long complete_transactions;
	unsigned long events;
	unsigned long watchdog_pretimeouts;
	unsigned long incoming_messages;
};

static void si_restart_short_timer(struct smi_info *smi_info);

static void deliver_recv_msg(struct smi_info *smi_info,
			     struct ipmi_smi_msg *msg)
{
	/* Deliver the message to the upper layer with the lock
           released. */
	spin_unlock(&(smi_info->si_lock));
	ipmi_smi_msg_received(smi_info->intf, msg);
	spin_lock(&(smi_info->si_lock));
}

static void return_hosed_msg(struct smi_info *smi_info)
{
	struct ipmi_smi_msg *msg = smi_info->curr_msg;

	/* Make it a reponse */
	msg->rsp[0] = msg->data[0] | 4;
	msg->rsp[1] = msg->data[1];
	msg->rsp[2] = 0xFF; /* Unknown error. */
	msg->rsp_size = 3;

	smi_info->curr_msg = NULL;
	deliver_recv_msg(smi_info, msg);
}

static enum si_sm_result start_next_msg(struct smi_info *smi_info)
{
	int              rv;
	struct list_head *entry = NULL;
#ifdef DEBUG_TIMING
	struct timeval t;
#endif

	/* No need to save flags, we aleady have interrupts off and we
	   already hold the SMI lock. */
	spin_lock(&(smi_info->msg_lock));

	/* Pick the high priority queue first. */
	if (! list_empty(&(smi_info->hp_xmit_msgs))) {
		entry = smi_info->hp_xmit_msgs.next;
	} else if (! list_empty(&(smi_info->xmit_msgs))) {
		entry = smi_info->xmit_msgs.next;
	}

	if (!entry) {
		smi_info->curr_msg = NULL;
		rv = SI_SM_IDLE;
	} else {
		int err;

		list_del(entry);
		smi_info->curr_msg = list_entry(entry,
						struct ipmi_smi_msg,
						link);
#ifdef DEBUG_TIMING
		do_gettimeofday(&t);
		printk("**Start2: %d.%9.9d\n", t.tv_sec, t.tv_usec);
#endif
		err = smi_info->handlers->start_transaction(
			smi_info->si_sm,
			smi_info->curr_msg->data,
			smi_info->curr_msg->data_size);
		if (err) {
			return_hosed_msg(smi_info);
		}

		rv = SI_SM_CALL_WITHOUT_DELAY;
	}
	spin_unlock(&(smi_info->msg_lock));

	return rv;
}

static void start_enable_irq(struct smi_info *smi_info)
{
	unsigned char msg[2];

	/* If we are enabling interrupts, we have to tell the
	   BMC to use them. */
	msg[0] = (IPMI_NETFN_APP_REQUEST << 2);
	msg[1] = IPMI_GET_BMC_GLOBAL_ENABLES_CMD;

	smi_info->handlers->start_transaction(smi_info->si_sm, msg, 2);
	smi_info->si_state = SI_ENABLE_INTERRUPTS1;
}

static void start_clear_flags(struct smi_info *smi_info)
{
	unsigned char msg[3];

	/* Make sure the watchdog pre-timeout flag is not set at startup. */
	msg[0] = (IPMI_NETFN_APP_REQUEST << 2);
	msg[1] = IPMI_CLEAR_MSG_FLAGS_CMD;
	msg[2] = WDT_PRE_TIMEOUT_INT;

	smi_info->handlers->start_transaction(smi_info->si_sm, msg, 3);
	smi_info->si_state = SI_CLEARING_FLAGS;
}

/* When we have a situtaion where we run out of memory and cannot
   allocate messages, we just leave them in the BMC and run the system
   polled until we can allocate some memory.  Once we have some
   memory, we will re-enable the interrupt. */
static inline void disable_si_irq(struct smi_info *smi_info)
{
	if ((smi_info->irq) && (!smi_info->interrupt_disabled)) {
		disable_irq_nosync(smi_info->irq);
		smi_info->interrupt_disabled = 1;
	}
}

static inline void enable_si_irq(struct smi_info *smi_info)
{
	if ((smi_info->irq) && (smi_info->interrupt_disabled)) {
		enable_irq(smi_info->irq);
		smi_info->interrupt_disabled = 0;
	}
}

static void handle_flags(struct smi_info *smi_info)
{
	if (smi_info->msg_flags & WDT_PRE_TIMEOUT_INT) {
		/* Watchdog pre-timeout */
		spin_lock(&smi_info->count_lock);
		smi_info->watchdog_pretimeouts++;
		spin_unlock(&smi_info->count_lock);

		start_clear_flags(smi_info);
		smi_info->msg_flags &= ~WDT_PRE_TIMEOUT_INT;
		spin_unlock(&(smi_info->si_lock));
		ipmi_smi_watchdog_pretimeout(smi_info->intf);
		spin_lock(&(smi_info->si_lock));
	} else if (smi_info->msg_flags & RECEIVE_MSG_AVAIL) {
		/* Messages available. */
		smi_info->curr_msg = ipmi_alloc_smi_msg();
		if (!smi_info->curr_msg) {
			disable_si_irq(smi_info);
			smi_info->si_state = SI_NORMAL;
			return;
		}
		enable_si_irq(smi_info);

		smi_info->curr_msg->data[0] = (IPMI_NETFN_APP_REQUEST << 2);
		smi_info->curr_msg->data[1] = IPMI_GET_MSG_CMD;
		smi_info->curr_msg->data_size = 2;

		smi_info->handlers->start_transaction(
			smi_info->si_sm,
			smi_info->curr_msg->data,
			smi_info->curr_msg->data_size);
		smi_info->si_state = SI_GETTING_MESSAGES;
	} else if (smi_info->msg_flags & EVENT_MSG_BUFFER_FULL) {
		/* Events available. */
		smi_info->curr_msg = ipmi_alloc_smi_msg();
		if (!smi_info->curr_msg) {
			disable_si_irq(smi_info);
			smi_info->si_state = SI_NORMAL;
			return;
		}
		enable_si_irq(smi_info);

		smi_info->curr_msg->data[0] = (IPMI_NETFN_APP_REQUEST << 2);
		smi_info->curr_msg->data[1] = IPMI_READ_EVENT_MSG_BUFFER_CMD;
		smi_info->curr_msg->data_size = 2;

		smi_info->handlers->start_transaction(
			smi_info->si_sm,
			smi_info->curr_msg->data,
			smi_info->curr_msg->data_size);
		smi_info->si_state = SI_GETTING_EVENTS;
	} else {
		smi_info->si_state = SI_NORMAL;
	}
}

static void handle_transaction_done(struct smi_info *smi_info)
{
	struct ipmi_smi_msg *msg;
#ifdef DEBUG_TIMING
	struct timeval t;

	do_gettimeofday(&t);
	printk("**Done: %d.%9.9d\n", t.tv_sec, t.tv_usec);
#endif
	switch (smi_info->si_state) {
	case SI_NORMAL:
		if (!smi_info->curr_msg)
			break;

		smi_info->curr_msg->rsp_size
			= smi_info->handlers->get_result(
				smi_info->si_sm,
				smi_info->curr_msg->rsp,
				IPMI_MAX_MSG_LENGTH);

		/* Do this here becase deliver_recv_msg() releases the
		   lock, and a new message can be put in during the
		   time the lock is released. */
		msg = smi_info->curr_msg;
		smi_info->curr_msg = NULL;
		deliver_recv_msg(smi_info, msg);
		break;

	case SI_GETTING_FLAGS:
	{
		unsigned char msg[4];
		unsigned int  len;

		/* We got the flags from the SMI, now handle them. */
		len = smi_info->handlers->get_result(smi_info->si_sm, msg, 4);
		if (msg[2] != 0) {
			/* Error fetching flags, just give up for
			   now. */
			smi_info->si_state = SI_NORMAL;
		} else if (len < 3) {
			/* Hmm, no flags.  That's technically illegal, but
			   don't use uninitialized data. */
			smi_info->si_state = SI_NORMAL;
		} else {
			smi_info->msg_flags = msg[3];
			handle_flags(smi_info);
		}
		break;
	}

	case SI_CLEARING_FLAGS:
	case SI_CLEARING_FLAGS_THEN_SET_IRQ:
	{
		unsigned char msg[3];

		/* We cleared the flags. */
		smi_info->handlers->get_result(smi_info->si_sm, msg, 3);
		if (msg[2] != 0) {
			/* Error clearing flags */
			printk(KERN_WARNING
			       "ipmi_si: Error clearing flags: %2.2x\n",
			       msg[2]);
		}
		if (smi_info->si_state == SI_CLEARING_FLAGS_THEN_SET_IRQ)
			start_enable_irq(smi_info);
		else
			smi_info->si_state = SI_NORMAL;
		break;
	}

	case SI_GETTING_EVENTS:
	{
		smi_info->curr_msg->rsp_size
			= smi_info->handlers->get_result(
				smi_info->si_sm,
				smi_info->curr_msg->rsp,
				IPMI_MAX_MSG_LENGTH);

		/* Do this here becase deliver_recv_msg() releases the
		   lock, and a new message can be put in during the
		   time the lock is released. */
		msg = smi_info->curr_msg;
		smi_info->curr_msg = NULL;
		if (msg->rsp[2] != 0) {
			/* Error getting event, probably done. */
			msg->done(msg);

			/* Take off the event flag. */
			smi_info->msg_flags &= ~EVENT_MSG_BUFFER_FULL;
		} else {
			spin_lock(&smi_info->count_lock);
			smi_info->events++;
			spin_unlock(&smi_info->count_lock);

			deliver_recv_msg(smi_info, msg);
		}
		handle_flags(smi_info);
		break;
	}

	case SI_GETTING_MESSAGES:
	{
		smi_info->curr_msg->rsp_size
			= smi_info->handlers->get_result(
				smi_info->si_sm,
				smi_info->curr_msg->rsp,
				IPMI_MAX_MSG_LENGTH);

		/* Do this here becase deliver_recv_msg() releases the
		   lock, and a new message can be put in during the
		   time the lock is released. */
		msg = smi_info->curr_msg;
		smi_info->curr_msg = NULL;
		if (msg->rsp[2] != 0) {
			/* Error getting event, probably done. */
			msg->done(msg);

			/* Take off the msg flag. */
			smi_info->msg_flags &= ~RECEIVE_MSG_AVAIL;
		} else {
			spin_lock(&smi_info->count_lock);
			smi_info->incoming_messages++;
			spin_unlock(&smi_info->count_lock);

			deliver_recv_msg(smi_info, msg);
		}
		handle_flags(smi_info);
		break;
	}

	case SI_ENABLE_INTERRUPTS1:
	{
		unsigned char msg[4];

		/* We got the flags from the SMI, now handle them. */
		smi_info->handlers->get_result(smi_info->si_sm, msg, 4);
		if (msg[2] != 0) {
			printk(KERN_WARNING
			       "ipmi_si: Could not enable interrupts"
			       ", failed get, using polled mode.\n");
			smi_info->si_state = SI_NORMAL;
		} else {
			msg[0] = (IPMI_NETFN_APP_REQUEST << 2);
			msg[1] = IPMI_SET_BMC_GLOBAL_ENABLES_CMD;
			msg[2] = msg[3] | 1; /* enable msg queue int */
			smi_info->handlers->start_transaction(
				smi_info->si_sm, msg, 3);
			smi_info->si_state = SI_ENABLE_INTERRUPTS2;
		}
		break;
	}

	case SI_ENABLE_INTERRUPTS2:
	{
		unsigned char msg[4];

		/* We got the flags from the SMI, now handle them. */
		smi_info->handlers->get_result(smi_info->si_sm, msg, 4);
		if (msg[2] != 0) {
			printk(KERN_WARNING
			       "ipmi_si: Could not enable interrupts"
			       ", failed set, using polled mode.\n");
		}
		smi_info->si_state = SI_NORMAL;
		break;
	}
	}
}

/* Called on timeouts and events.  Timeouts should pass the elapsed
   time, interrupts should pass in zero. */
static enum si_sm_result smi_event_handler(struct smi_info *smi_info,
					   int time)
{
	enum si_sm_result si_sm_result;

 restart:
	/* There used to be a loop here that waited a little while
	   (around 25us) before giving up.  That turned out to be
	   pointless, the minimum delays I was seeing were in the 300us
	   range, which is far too long to wait in an interrupt.  So
	   we just run until the state machine tells us something
	   happened or it needs a delay. */
	si_sm_result = smi_info->handlers->event(smi_info->si_sm, time);
	time = 0;
	while (si_sm_result == SI_SM_CALL_WITHOUT_DELAY)
	{
		si_sm_result = smi_info->handlers->event(smi_info->si_sm, 0);
	}

	if (si_sm_result == SI_SM_TRANSACTION_COMPLETE)
	{
		spin_lock(&smi_info->count_lock);
		smi_info->complete_transactions++;
		spin_unlock(&smi_info->count_lock);

		handle_transaction_done(smi_info);
		si_sm_result = smi_info->handlers->event(smi_info->si_sm, 0);
	}
	else if (si_sm_result == SI_SM_HOSED)
	{
		spin_lock(&smi_info->count_lock);
		smi_info->hosed_count++;
		spin_unlock(&smi_info->count_lock);

		if (smi_info->curr_msg != NULL) {
			/* If we were handling a user message, format
                           a response to send to the upper layer to
                           tell it about the error. */
			return_hosed_msg(smi_info);
		}
		si_sm_result = smi_info->handlers->event(smi_info->si_sm, 0);
		smi_info->si_state = SI_NORMAL;
	}

	/* We prefer handling attn over new messages. */
	if (si_sm_result == SI_SM_ATTN)
	{
		unsigned char msg[2];

		spin_lock(&smi_info->count_lock);
		smi_info->attentions++;
		spin_unlock(&smi_info->count_lock);

		/* Got a attn, send down a get message flags to see
                   what's causing it.  It would be better to handle
                   this in the upper layer, but due to the way
                   interrupts work with the SMI, that's not really
                   possible. */
		msg[0] = (IPMI_NETFN_APP_REQUEST << 2);
		msg[1] = IPMI_GET_MSG_FLAGS_CMD;

		smi_info->handlers->start_transaction(
			smi_info->si_sm, msg, 2);
		smi_info->si_state = SI_GETTING_FLAGS;
		goto restart;
	}

	/* If we are currently idle, try to start the next message. */
	if (si_sm_result == SI_SM_IDLE) {
		spin_lock(&smi_info->count_lock);
		smi_info->idles++;
		spin_unlock(&smi_info->count_lock);

		si_sm_result = start_next_msg(smi_info);
		if (si_sm_result != SI_SM_IDLE)
			goto restart;
        }

	if ((si_sm_result == SI_SM_IDLE)
	    && (atomic_read(&smi_info->req_events)))
	{
		/* We are idle and the upper layer requested that I fetch
		   events, so do so. */
		unsigned char msg[2];

		spin_lock(&smi_info->count_lock);
		smi_info->flag_fetches++;
		spin_unlock(&smi_info->count_lock);

		atomic_set(&smi_info->req_events, 0);
		msg[0] = (IPMI_NETFN_APP_REQUEST << 2);
		msg[1] = IPMI_GET_MSG_FLAGS_CMD;

		smi_info->handlers->start_transaction(
			smi_info->si_sm, msg, 2);
		smi_info->si_state = SI_GETTING_FLAGS;
		goto restart;
	}

	return si_sm_result;
}

static void sender(void                *send_info,
		   struct ipmi_smi_msg *msg,
		   int                 priority)
{
	struct smi_info   *smi_info = send_info;
	enum si_sm_result result;
	unsigned long     flags;
#ifdef DEBUG_TIMING
	struct timeval    t;
#endif

	spin_lock_irqsave(&(smi_info->msg_lock), flags);
#ifdef DEBUG_TIMING
	do_gettimeofday(&t);
	printk("**Enqueue: %d.%9.9d\n", t.tv_sec, t.tv_usec);
#endif

	if (smi_info->run_to_completion) {
		/* If we are running to completion, then throw it in
		   the list and run transactions until everything is
		   clear.  Priority doesn't matter here. */
		list_add_tail(&(msg->link), &(smi_info->xmit_msgs));

		/* We have to release the msg lock and claim the smi
		   lock in this case, because of race conditions. */
		spin_unlock_irqrestore(&(smi_info->msg_lock), flags);

		spin_lock_irqsave(&(smi_info->si_lock), flags);
		result = smi_event_handler(smi_info, 0);
		while (result != SI_SM_IDLE) {
			udelay(SI_SHORT_TIMEOUT_USEC);
			result = smi_event_handler(smi_info,
						   SI_SHORT_TIMEOUT_USEC);
		}
		spin_unlock_irqrestore(&(smi_info->si_lock), flags);
		return;
	} else {
		if (priority > 0) {
			list_add_tail(&(msg->link), &(smi_info->hp_xmit_msgs));
		} else {
			list_add_tail(&(msg->link), &(smi_info->xmit_msgs));
		}
	}
	spin_unlock_irqrestore(&(smi_info->msg_lock), flags);

	spin_lock_irqsave(&(smi_info->si_lock), flags);
	if ((smi_info->si_state == SI_NORMAL)
	    && (smi_info->curr_msg == NULL))
	{
		start_next_msg(smi_info);
		si_restart_short_timer(smi_info);
	}
	spin_unlock_irqrestore(&(smi_info->si_lock), flags);
}

static void set_run_to_completion(void *send_info, int i_run_to_completion)
{
	struct smi_info   *smi_info = send_info;
	enum si_sm_result result;
	unsigned long     flags;

	spin_lock_irqsave(&(smi_info->si_lock), flags);

	smi_info->run_to_completion = i_run_to_completion;
	if (i_run_to_completion) {
		result = smi_event_handler(smi_info, 0);
		while (result != SI_SM_IDLE) {
			udelay(SI_SHORT_TIMEOUT_USEC);
			result = smi_event_handler(smi_info,
						   SI_SHORT_TIMEOUT_USEC);
		}
	}

	spin_unlock_irqrestore(&(smi_info->si_lock), flags);
}

static void poll(void *send_info)
{
	struct smi_info *smi_info = send_info;

	smi_event_handler(smi_info, 0);
}

static void request_events(void *send_info)
{
	struct smi_info *smi_info = send_info;

	atomic_set(&smi_info->req_events, 1);
}

static int initialized = 0;

/* Must be called with interrupts off and with the si_lock held. */
static void si_restart_short_timer(struct smi_info *smi_info)
{
#if defined(CONFIG_HIGH_RES_TIMERS)
	unsigned long flags;
	unsigned long jiffies_now;

	if (del_timer(&(smi_info->si_timer))) {
		/* If we don't delete the timer, then it will go off
		   immediately, anyway.  So we only process if we
		   actually delete the timer. */

		/* We already have irqsave on, so no need for it
                   here. */
		read_lock(&xtime_lock);
		jiffies_now = jiffies;
		smi_info->si_timer.expires = jiffies_now;
		smi_info->si_timer.sub_expires = get_arch_cycles(jiffies_now);

		add_usec_to_timer(&smi_info->si_timer, SI_SHORT_TIMEOUT_USEC);

		add_timer(&(smi_info->si_timer));
		spin_lock_irqsave(&smi_info->count_lock, flags);
		smi_info->timeout_restarts++;
		spin_unlock_irqrestore(&smi_info->count_lock, flags);
	}
#endif
}

static void smi_timeout(unsigned long data)
{
	struct smi_info   *smi_info = (struct smi_info *) data;
	enum si_sm_result smi_result;
	unsigned long     flags;
	unsigned long     jiffies_now;
	unsigned long     time_diff;
#ifdef DEBUG_TIMING
	struct timeval    t;
#endif

	if (smi_info->stop_operation) {
		smi_info->timer_stopped = 1;
		return;
	}

	spin_lock_irqsave(&(smi_info->si_lock), flags);
#ifdef DEBUG_TIMING
	do_gettimeofday(&t);
	printk("**Timer: %d.%9.9d\n", t.tv_sec, t.tv_usec);
#endif
	jiffies_now = jiffies;
	time_diff = ((jiffies_now - smi_info->last_timeout_jiffies)
		     * SI_USEC_PER_JIFFY);
	smi_result = smi_event_handler(smi_info, time_diff);

	spin_unlock_irqrestore(&(smi_info->si_lock), flags);

	smi_info->last_timeout_jiffies = jiffies_now;

	if ((smi_info->irq) && (! smi_info->interrupt_disabled)) {
		/* Running with interrupts, only do long timeouts. */
		smi_info->si_timer.expires = jiffies + SI_TIMEOUT_JIFFIES;
		spin_lock_irqsave(&smi_info->count_lock, flags);
		smi_info->long_timeouts++;
		spin_unlock_irqrestore(&smi_info->count_lock, flags);
		goto do_add_timer;
	}

	/* If the state machine asks for a short delay, then shorten
           the timer timeout. */
	if (smi_result == SI_SM_CALL_WITH_DELAY) {
		spin_lock_irqsave(&smi_info->count_lock, flags);
		smi_info->short_timeouts++;
		spin_unlock_irqrestore(&smi_info->count_lock, flags);
#if defined(CONFIG_HIGH_RES_TIMERS)
		read_lock(&xtime_lock);
                smi_info->si_timer.expires = jiffies;
                smi_info->si_timer.sub_expires
                        = get_arch_cycles(smi_info->si_timer.expires);
                read_unlock(&xtime_lock);
		add_usec_to_timer(&smi_info->si_timer, SI_SHORT_TIMEOUT_USEC);
#else
		smi_info->si_timer.expires = jiffies + 1;
#endif
	} else {
		spin_lock_irqsave(&smi_info->count_lock, flags);
		smi_info->long_timeouts++;
		spin_unlock_irqrestore(&smi_info->count_lock, flags);
		smi_info->si_timer.expires = jiffies + SI_TIMEOUT_JIFFIES;
#if defined(CONFIG_HIGH_RES_TIMERS)
		smi_info->si_timer.sub_expires = 0;
#endif
	}

 do_add_timer:
	add_timer(&(smi_info->si_timer));
}

static irqreturn_t si_irq_handler(int irq, void *data, struct pt_regs *regs)
{
	struct smi_info *smi_info = data;
	unsigned long   flags;
#ifdef DEBUG_TIMING
	struct timeval  t;
#endif

	spin_lock_irqsave(&(smi_info->si_lock), flags);

	spin_lock(&smi_info->count_lock);
	smi_info->interrupts++;
	spin_unlock(&smi_info->count_lock);

	if (smi_info->stop_operation)
		goto out;

#ifdef DEBUG_TIMING
	do_gettimeofday(&t);
	printk("**Interrupt: %d.%9.9d\n", t.tv_sec, t.tv_usec);
#endif
	smi_event_handler(smi_info, 0);
 out:
	spin_unlock_irqrestore(&(smi_info->si_lock), flags);
	return IRQ_HANDLED;
}

static struct ipmi_smi_handlers handlers =
{
	.owner                  = THIS_MODULE,
	.sender			= sender,
	.request_events		= request_events,
	.set_run_to_completion  = set_run_to_completion,
	.poll			= poll,
};

/* There can be 4 IO ports passed in (with or without IRQs), 4 addresses,
   a default IO port, and 1 ACPI/SPMI address.  That sets SI_MAX_DRIVERS */

#define SI_MAX_PARMS 4
#define SI_MAX_DRIVERS ((SI_MAX_PARMS * 2) + 2)
static struct smi_info *smi_infos[SI_MAX_DRIVERS] =
{ NULL, NULL, NULL, NULL };

#define DEVICE_NAME "ipmi_si"

#define DEFAULT_KCS_IO_PORT 0xca2
#define DEFAULT_SMIC_IO_PORT 0xca9
#define DEFAULT_BT_IO_PORT   0xe4

static int           si_trydefaults = 1;
static char          *si_type[SI_MAX_PARMS] = { NULL, NULL, NULL, NULL };
#define MAX_SI_TYPE_STR 30
static char          si_type_str[MAX_SI_TYPE_STR];
static unsigned long addrs[SI_MAX_PARMS] = { 0, 0, 0, 0 };
static int num_addrs = 0;
static unsigned int  ports[SI_MAX_PARMS] = { 0, 0, 0, 0 };
static int num_ports = 0;
static int           irqs[SI_MAX_PARMS] = { 0, 0, 0, 0 };
static int num_irqs = 0;


module_param_named(trydefaults, si_trydefaults, bool, 0);
MODULE_PARM_DESC(trydefaults, "Setting this to 'false' will disable the"
		 " default scan of the KCS and SMIC interface at the standard"
		 " address");
module_param_string(type, si_type_str, MAX_SI_TYPE_STR, 0);
MODULE_PARM_DESC(type, "Defines the type of each interface, each"
		 " interface separated by commas.  The types are 'kcs',"
		 " 'smic', and 'bt'.  For example si_type=kcs,bt will set"
		 " the first interface to kcs and the second to bt");
module_param_array(addrs, long, num_addrs, 0);
MODULE_PARM_DESC(addrs, "Sets the memory address of each interface, the"
		 " addresses separated by commas.  Only use if an interface"
		 " is in memory.  Otherwise, set it to zero or leave"
		 " it blank.");
module_param_array(ports, int, num_ports, 0);
MODULE_PARM_DESC(ports, "Sets the port address of each interface, the"
		 " addresses separated by commas.  Only use if an interface"
		 " is a port.  Otherwise, set it to zero or leave"
		 " it blank.");
module_param_array(irqs, int, num_irqs, 0);
MODULE_PARM_DESC(irqs, "Sets the interrupt of each interface, the"
		 " addresses separated by commas.  Only use if an interface"
		 " has an interrupt.  Otherwise, set it to zero or leave"
		 " it blank.");

#define IPMI_MEM_ADDR_SPACE 1
#define IPMI_IO_ADDR_SPACE  2

#if defined(CONFIG_ACPI_INTERPETER) || defined(CONFIG_X86) || defined(CONFIG_PCI)
static int is_new_interface(int intf, u8 addr_space, unsigned long base_addr)
{
	int i;

	for (i = 0; i < SI_MAX_PARMS; ++i) {
		/* Don't check our address. */
		if (i == intf)
			continue;
		if (si_type[i] != NULL) {
			if ((addr_space == IPMI_MEM_ADDR_SPACE &&
			     base_addr == addrs[i]) ||
			    (addr_space == IPMI_IO_ADDR_SPACE &&
			     base_addr == ports[i]))
				return 0;
		}
		else
			break;
	}

	return 1;
}
#endif

static int std_irq_setup(struct smi_info *info)
{
	int rv;

	if (!info->irq)
		return 0;

	rv = request_irq(info->irq,
			 si_irq_handler,
			 SA_INTERRUPT,
			 DEVICE_NAME,
			 info);
	if (rv) {
		printk(KERN_WARNING
		       "ipmi_si: %s unable to claim interrupt %d,"
		       " running polled\n",
		       DEVICE_NAME, info->irq);
		info->irq = 0;
	} else {
		printk("  Using irq %d\n", info->irq);
	}

	return rv;
}

static void std_irq_cleanup(struct smi_info *info)
{
	if (!info->irq)
		return;

	free_irq(info->irq, info);
}

static unsigned char port_inb(struct si_sm_io *io, unsigned int offset)
{
	unsigned int *addr = io->info;

	return inb((*addr)+offset);
}

static void port_outb(struct si_sm_io *io, unsigned int offset,
		      unsigned char b)
{
	unsigned int *addr = io->info;

	outb(b, (*addr)+offset);
}

static int port_setup(struct smi_info *info)
{
	unsigned int *addr = info->io.info;

	if (!addr || (!*addr))
		return -ENODEV;

	if (request_region(*addr, info->io_size, DEVICE_NAME) == NULL)
		return -EIO;
	return 0;
}

static void port_cleanup(struct smi_info *info)
{
	unsigned int *addr = info->io.info;

	if (addr && (*addr))
		release_region (*addr, info->io_size);
	kfree(info);
}

static int try_init_port(int intf_num, struct smi_info **new_info)
{
	struct smi_info *info;

	if (!ports[intf_num])
		return -ENODEV;

	if (!is_new_interface(intf_num, IPMI_IO_ADDR_SPACE,
			      ports[intf_num]))
		return -ENODEV;

	info = kmalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		printk(KERN_ERR "ipmi_si: Could not allocate SI data (1)\n");
		return -ENOMEM;
	}
	memset(info, 0, sizeof(*info));

	info->io_setup = port_setup;
	info->io_cleanup = port_cleanup;
	info->io.inputb = port_inb;
	info->io.outputb = port_outb;
	info->io.info = &(ports[intf_num]);
	info->io.addr = NULL;
	info->irq = 0;
	info->irq_setup = NULL;
	*new_info = info;

	if (si_type[intf_num] == NULL)
		si_type[intf_num] = "kcs";

	printk("ipmi_si: Trying \"%s\" at I/O port 0x%x\n",
	       si_type[intf_num], ports[intf_num]);
	return 0;
}

static unsigned char mem_inb(struct si_sm_io *io, unsigned int offset)
{
	return readb((io->addr)+offset);
}

static void mem_outb(struct si_sm_io *io, unsigned int offset,
		     unsigned char b)
{
	writeb(b, (io->addr)+offset);
}

static int mem_setup(struct smi_info *info)
{
	unsigned long *addr = info->io.info;

	if (!addr || (!*addr))
		return -ENODEV;

	if (request_mem_region(*addr, info->io_size, DEVICE_NAME) == NULL)
		return -EIO;

	info->io.addr = ioremap(*addr, info->io_size);
	if (info->io.addr == NULL) {
		release_mem_region(*addr, info->io_size);
		return -EIO;
	}
	return 0;
}

static void mem_cleanup(struct smi_info *info)
{
	unsigned long *addr = info->io.info;

	if (info->io.addr) {
		iounmap(info->io.addr);
		release_mem_region(*addr, info->io_size);
	}
	kfree(info);
}

static int try_init_mem(int intf_num, struct smi_info **new_info)
{
	struct smi_info *info;

	if (!addrs[intf_num])
		return -ENODEV;

	if (!is_new_interface(intf_num, IPMI_MEM_ADDR_SPACE,
			      addrs[intf_num]))
		return -ENODEV;

	info = kmalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		printk(KERN_ERR "ipmi_si: Could not allocate SI data (2)\n");
		return -ENOMEM;
	}
	memset(info, 0, sizeof(*info));

	info->io_setup = mem_setup;
	info->io_cleanup = mem_cleanup;
	info->io.inputb = mem_inb;
	info->io.outputb = mem_outb;
	info->io.info = (void *) addrs[intf_num];
	info->io.addr = NULL;
	info->irq = 0;
	info->irq_setup = NULL;
	*new_info = info;

	if (si_type[intf_num] == NULL)
		si_type[intf_num] = "kcs";

	printk("ipmi_si: Trying \"%s\" at memory address 0x%lx\n",
	       si_type[intf_num], addrs[intf_num]);
	return 0;
}


#ifdef CONFIG_ACPI_INTERPRETER

#include <linux/acpi.h>

/* Once we get an ACPI failure, we don't try any more, because we go
   through the tables sequentially.  Once we don't find a table, there
   are no more. */
static int acpi_failure = 0;

/* For GPE-type interrupts. */
void ipmi_acpi_gpe(void *context)
{
	struct smi_info *smi_info = context;
	unsigned long   flags;
#ifdef DEBUG_TIMING
	struct timeval t;
#endif

	spin_lock_irqsave(&(smi_info->si_lock), flags);

	spin_lock(&smi_info->count_lock);
	smi_info->interrupts++;
	spin_unlock(&smi_info->count_lock);

	if (smi_info->stop_operation)
		goto out;

#ifdef DEBUG_TIMING
	do_gettimeofday(&t);
	printk("**ACPI_GPE: %d.%9.9d\n", t.tv_sec, t.tv_usec);
#endif
	smi_event_handler(smi_info, 0);
 out:
	spin_unlock_irqrestore(&(smi_info->si_lock), flags);
}

static int acpi_gpe_irq_setup(struct smi_info *info)
{
	acpi_status status;

	if (!info->irq)
		return 0;

	/* FIXME - is level triggered right? */
	status = acpi_install_gpe_handler(NULL,
					  info->irq,
					  ACPI_GPE_LEVEL_TRIGGERED,
					  ipmi_acpi_gpe,
					  info);
	if (status != AE_OK) {
		printk(KERN_WARNING
		       "ipmi_si: %s unable to claim ACPI GPE %d,"
		       " running polled\n",
		       DEVICE_NAME, info->irq);
		info->irq = 0;
		return -EINVAL;
	} else {
		printk("  Using ACPI GPE %d\n", info->irq);
		return 0;
	}

}

static void acpi_gpe_irq_cleanup(struct smi_info *info)
{
	if (!info->irq)
		return;

	acpi_remove_gpe_handler(NULL, info->irq, ipmi_acpi_gpe);
}

/*
 * Defined at
 * http://h21007.www2.hp.com/dspp/files/unprotected/devresource/Docs/TechPapers/IA64/hpspmi.pdf
 */
struct SPMITable {
	s8	Signature[4];
	u32	Length;
	u8	Revision;
	u8	Checksum;
	s8	OEMID[6];
	s8	OEMTableID[8];
	s8	OEMRevision[4];
	s8	CreatorID[4];
	s8	CreatorRevision[4];
	u8	InterfaceType;
	u8	IPMIlegacy;
	s16	SpecificationRevision;

	/*
	 * Bit 0 - SCI interrupt supported
	 * Bit 1 - I/O APIC/SAPIC
	 */
	u8	InterruptType;

	/* If bit 0 of InterruptType is set, then this is the SCI
           interrupt in the GPEx_STS register. */
	u8	GPE;

	s16	Reserved;

	/* If bit 1 of InterruptType is set, then this is the I/O
           APIC/SAPIC interrupt. */
	u32	GlobalSystemInterrupt;

	/* The actual register address. */
	struct acpi_generic_address addr;

	u8	UID[4];

	s8      spmi_id[1]; /* A '\0' terminated array starts here. */
};

static int try_init_acpi(int intf_num, struct smi_info **new_info)
{
	struct smi_info  *info;
	acpi_status      status;
	struct SPMITable *spmi;
	char             *io_type;
	u8 		 addr_space;

	if (acpi_failure)
		return -ENODEV;

	status = acpi_get_firmware_table("SPMI", intf_num+1,
					 ACPI_LOGICAL_ADDRESSING,
					 (struct acpi_table_header **) &spmi);
	if (status != AE_OK) {
		acpi_failure = 1;
		return -ENODEV;
	}

	if (spmi->IPMIlegacy != 1) {
	    printk(KERN_INFO "IPMI: Bad SPMI legacy %d\n", spmi->IPMIlegacy);
  	    return -ENODEV;
	}

	if (spmi->addr.address_space_id == ACPI_ADR_SPACE_SYSTEM_MEMORY)
		addr_space = IPMI_MEM_ADDR_SPACE;
	else
		addr_space = IPMI_IO_ADDR_SPACE;
	if (!is_new_interface(-1, addr_space, spmi->addr.address))
		return -ENODEV;

	/* Figure out the interface type. */
	switch (spmi->InterfaceType)
	{
	case 1:	/* KCS */
		si_type[intf_num] = "kcs";
		break;

	case 2:	/* SMIC */
		si_type[intf_num] = "smic";
		break;

	case 3:	/* BT */
		si_type[intf_num] = "bt";
		break;

	default:
		printk(KERN_INFO "ipmi_si: Unknown ACPI/SPMI SI type %d\n",
			spmi->InterfaceType);
		return -EIO;
	}

	info = kmalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		printk(KERN_ERR "ipmi_si: Could not allocate SI data (3)\n");
		return -ENOMEM;
	}
	memset(info, 0, sizeof(*info));

	if (spmi->InterruptType & 1) {
		/* We've got a GPE interrupt. */
		info->irq = spmi->GPE;
		info->irq_setup = acpi_gpe_irq_setup;
		info->irq_cleanup = acpi_gpe_irq_cleanup;
	} else if (spmi->InterruptType & 2) {
		/* We've got an APIC/SAPIC interrupt. */
		info->irq = spmi->GlobalSystemInterrupt;
		info->irq_setup = std_irq_setup;
		info->irq_cleanup = std_irq_cleanup;
	} else {
		/* Use the default interrupt setting. */
		info->irq = 0;
		info->irq_setup = NULL;
	}

	if (spmi->addr.address_space_id == ACPI_ADR_SPACE_SYSTEM_MEMORY) {
		io_type = "memory";
		info->io_setup = mem_setup;
		info->io_cleanup = mem_cleanup;
		addrs[intf_num] = spmi->addr.address;
		info->io.inputb = mem_inb;
		info->io.outputb = mem_outb;
		info->io.info = &(addrs[intf_num]);
	} else if (spmi->addr.address_space_id == ACPI_ADR_SPACE_SYSTEM_IO) {
		io_type = "I/O";
		info->io_setup = port_setup;
		info->io_cleanup = port_cleanup;
		ports[intf_num] = spmi->addr.address;
		info->io.inputb = port_inb;
		info->io.outputb = port_outb;
		info->io.info = &(ports[intf_num]);
	} else {
		kfree(info);
		printk("ipmi_si: Unknown ACPI I/O Address type\n");
		return -EIO;
	}

	*new_info = info;

	printk("ipmi_si: ACPI/SPMI specifies \"%s\" %s SI @ 0x%lx\n",
	       si_type[intf_num], io_type, (unsigned long) spmi->addr.address);
	return 0;
}
#endif

#ifdef CONFIG_X86

typedef struct dmi_ipmi_data
{
	u8   		type;
	u8   		addr_space;
	unsigned long	base_addr;
	u8   		irq;
}dmi_ipmi_data_t;

typedef struct dmi_header
{
	u8	type;
	u8	length;
	u16	handle;
}dmi_header_t;

static int decode_dmi(dmi_header_t *dm, dmi_ipmi_data_t *ipmi_data)
{
	u8		*data = (u8 *)dm;
	unsigned long  	base_addr;

	ipmi_data->type = data[0x04];

	memcpy(&base_addr,&data[0x08],sizeof(unsigned long));
	if (base_addr & 1) {
		/* I/O */
		base_addr &= 0xFFFE;
		ipmi_data->addr_space = IPMI_IO_ADDR_SPACE;
	}
	else {
		/* Memory */
		ipmi_data->addr_space = IPMI_MEM_ADDR_SPACE;
	}

	ipmi_data->base_addr = base_addr;
	ipmi_data->irq = data[0x11];

	if (is_new_interface(-1, ipmi_data->addr_space,ipmi_data->base_addr))
	    return 0;

	memset(ipmi_data,0,sizeof(dmi_ipmi_data_t));

	return -1;
}

static int dmi_table(u32 base, int len, int num,
	dmi_ipmi_data_t *ipmi_data)
{
	u8 		  *buf;
	struct dmi_header *dm;
	u8 		  *data;
	int 		  i=1;
	int		  status=-1;

	buf = ioremap(base, len);
	if(buf==NULL)
		return -1;

	data = buf;

	while(i<num && (data - buf) < len)
	{
		dm=(dmi_header_t *)data;

		if((data-buf+dm->length) >= len)
        		break;

		if (dm->type == 38) {
			if (decode_dmi(dm, ipmi_data) == 0) {
				status = 0;
				break;
			}
		}

	        data+=dm->length;
		while((data-buf) < len && (*data || data[1]))
			data++;
		data+=2;
		i++;
	}
	iounmap(buf);

	return status;
}

inline static int dmi_checksum(u8 *buf)
{
	u8   sum=0;
	int  a;

	for(a=0; a<15; a++)
		sum+=buf[a];
	return (sum==0);
}

static int dmi_iterator(dmi_ipmi_data_t *ipmi_data)
{
	u8   buf[15];
	u32  fp=0xF0000;

#ifdef CONFIG_SIMNOW
	return -1;
#endif

	while(fp < 0xFFFFF)
	{
		isa_memcpy_fromio(buf, fp, 15);
		if(memcmp(buf, "_DMI_", 5)==0 && dmi_checksum(buf))
		{
			u16 num=buf[13]<<8|buf[12];
			u16 len=buf[7]<<8|buf[6];
			u32 base=buf[11]<<24|buf[10]<<16|buf[9]<<8|buf[8];

			if(dmi_table(base, len, num, ipmi_data) == 0)
				return 0;
		}
		fp+=16;
	}

	return -1;
}

static int try_init_smbios(int intf_num, struct smi_info **new_info)
{
	struct smi_info   *info;
	dmi_ipmi_data_t   ipmi_data;
	char              *io_type;
	int               status;

	status = dmi_iterator(&ipmi_data);

	if (status < 0)
		return -ENODEV;

	switch(ipmi_data.type) {
		case 0x01: /* KCS */
			si_type[intf_num] = "kcs";
			break;
		case 0x02: /* SMIC */
			si_type[intf_num] = "smic";
			break;
		case 0x03: /* BT */
			si_type[intf_num] = "bt";
			break;
		default:
			printk("ipmi_si: Unknown SMBIOS SI type.\n");
			return -EIO;
	}

	info = kmalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		printk(KERN_ERR "ipmi_si: Could not allocate SI data (4)\n");
		return -ENOMEM;
	}
	memset(info, 0, sizeof(*info));

	if (ipmi_data.addr_space == 1) {
		io_type = "memory";
		info->io_setup = mem_setup;
		info->io_cleanup = mem_cleanup;
		addrs[intf_num] = ipmi_data.base_addr;
		info->io.inputb = mem_inb;
		info->io.outputb = mem_outb;
		info->io.info = &(addrs[intf_num]);
	} else if (ipmi_data.addr_space == 2) {
		io_type = "I/O";
		info->io_setup = port_setup;
		info->io_cleanup = port_cleanup;
		ports[intf_num] = ipmi_data.base_addr;
		info->io.inputb = port_inb;
		info->io.outputb = port_outb;
		info->io.info = &(ports[intf_num]);
	} else {
		kfree(info);
		printk("ipmi_si: Unknown SMBIOS I/O Address type.\n");
		return -EIO;
	}

	irqs[intf_num] = ipmi_data.irq;

	*new_info = info;

	printk("ipmi_si: Found SMBIOS-specified state machine at %s"
	       " address 0x%lx\n",
	       io_type, (unsigned long)ipmi_data.base_addr);
	return 0;
}
#endif /* CONFIG_X86 */

#ifdef CONFIG_PCI

#define PCI_ERMC_CLASSCODE  0x0C0700
#define PCI_HP_VENDOR_ID    0x103C
#define PCI_MMC_DEVICE_ID   0x121A
#define PCI_MMC_ADDR_CW     0x10

/* Avoid more than one attempt to probe pci smic. */
static int pci_smic_checked = 0;

static int find_pci_smic(int intf_num, struct smi_info **new_info)
{
	struct smi_info  *info;
	int              error;
	struct pci_dev   *pci_dev = NULL;
	u16    		 base_addr;
	int              fe_rmc = 0;

	if (pci_smic_checked)
		return -ENODEV;

	pci_smic_checked = 1;

	if ((pci_dev = pci_find_device(PCI_HP_VENDOR_ID, PCI_MMC_DEVICE_ID,
				       NULL)))
		;
	else if ((pci_dev = pci_find_class(PCI_ERMC_CLASSCODE, NULL)) &&
		 pci_dev->subsystem_vendor == PCI_HP_VENDOR_ID)
		fe_rmc = 1;
	else
		return -ENODEV;

	error = pci_read_config_word(pci_dev, PCI_MMC_ADDR_CW, &base_addr);
	if (error)
	{
		printk(KERN_ERR
		       "ipmi_si: pci_read_config_word() failed (%d).\n",
		       error);
		return -ENODEV;
	}

	/* Bit 0: 1 specifies programmed I/O, 0 specifies memory mapped I/O */
	if (!(base_addr & 0x0001))
	{
		printk(KERN_ERR
		       "ipmi_si: memory mapped I/O not supported for PCI"
		       " smic.\n");
		return -ENODEV;
	}

	base_addr &= 0xFFFE;
	if (!fe_rmc)
		/* Data register starts at base address + 1 in eRMC */
		++base_addr;

	if (!is_new_interface(-1, IPMI_IO_ADDR_SPACE, base_addr))
	    return -ENODEV;

	info = kmalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		printk(KERN_ERR "ipmi_si: Could not allocate SI data (5)\n");
		return -ENOMEM;
	}
	memset(info, 0, sizeof(*info));

	info->io_setup = port_setup;
	info->io_cleanup = port_cleanup;
	ports[intf_num] = base_addr;
	info->io.inputb = port_inb;
	info->io.outputb = port_outb;
	info->io.info = &(ports[intf_num]);

	*new_info = info;

	irqs[intf_num] = pci_dev->irq;
	si_type[intf_num] = "smic";

	printk("ipmi_si: Found PCI SMIC at I/O address 0x%lx\n",
		(long unsigned int) base_addr);

	return 0;
}
#endif /* CONFIG_PCI */

static int try_init_plug_and_play(int intf_num, struct smi_info **new_info)
{
#ifdef CONFIG_PCI
	if (find_pci_smic(intf_num, new_info)==0)
		return 0;
#endif
	/* Include other methods here. */

	return -ENODEV;
}


static int try_get_dev_id(struct smi_info *smi_info)
{
	unsigned char      msg[2];
	unsigned char      *resp;
	unsigned long      resp_len;
	enum si_sm_result smi_result;
	int               rv = 0;

	resp = kmalloc(IPMI_MAX_MSG_LENGTH, GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	/* Do a Get Device ID command, since it comes back with some
	   useful info. */
	msg[0] = IPMI_NETFN_APP_REQUEST << 2;
	msg[1] = IPMI_GET_DEVICE_ID_CMD;
	smi_info->handlers->start_transaction(smi_info->si_sm, msg, 2);

	smi_result = smi_info->handlers->event(smi_info->si_sm, 0);
	for (;;)
	{
		if (smi_result == SI_SM_CALL_WITH_DELAY) {
			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_timeout(1);
			smi_result = smi_info->handlers->event(
				smi_info->si_sm, 100);
		}
		else if (smi_result == SI_SM_CALL_WITHOUT_DELAY)
		{
			smi_result = smi_info->handlers->event(
				smi_info->si_sm, 0);
		}
		else
			break;
	}
	if (smi_result == SI_SM_HOSED) {
		/* We couldn't get the state machine to run, so whatever's at
		   the port is probably not an IPMI SMI interface. */
		rv = -ENODEV;
		goto out;
	}

	/* Otherwise, we got some data. */
	resp_len = smi_info->handlers->get_result(smi_info->si_sm,
						  resp, IPMI_MAX_MSG_LENGTH);
	if (resp_len < 6) {
		/* That's odd, it should be longer. */
		rv = -EINVAL;
		goto out;
	}

	if ((resp[1] != IPMI_GET_DEVICE_ID_CMD) || (resp[2] != 0)) {
		/* That's odd, it shouldn't be able to fail. */
		rv = -EINVAL;
		goto out;
	}

	/* Record info from the get device id, in case we need it. */
	smi_info->ipmi_si_dev_rev = resp[4] & 0xf;
	smi_info->ipmi_si_fw_rev_major = resp[5] & 0x7f;
	smi_info->ipmi_si_fw_rev_minor = resp[6];
	smi_info->ipmi_version_major = resp[7] & 0xf;
	smi_info->ipmi_version_minor = resp[7] >> 4;

 out:
	kfree(resp);
	return rv;
}

static int type_file_read_proc(char *page, char **start, off_t off,
			       int count, int *eof, void *data)
{
	char            *out = (char *) page;
	struct smi_info *smi = data;

	switch (smi->si_type) {
	    case SI_KCS:
		return sprintf(out, "kcs\n");
	    case SI_SMIC:
		return sprintf(out, "smic\n");
	    case SI_BT:
		return sprintf(out, "bt\n");
	    default:
		return 0;
	}
}

static int stat_file_read_proc(char *page, char **start, off_t off,
			       int count, int *eof, void *data)
{
	char            *out = (char *) page;
	struct smi_info *smi = data;

	out += sprintf(out, "interrupts_enabled:    %d\n",
		       smi->irq && !smi->interrupt_disabled);
	out += sprintf(out, "short_timeouts:        %ld\n",
		       smi->short_timeouts);
	out += sprintf(out, "long_timeouts:         %ld\n",
		       smi->long_timeouts);
	out += sprintf(out, "timeout_restarts:      %ld\n",
		       smi->timeout_restarts);
	out += sprintf(out, "idles:                 %ld\n",
		       smi->idles);
	out += sprintf(out, "interrupts:            %ld\n",
		       smi->interrupts);
	out += sprintf(out, "attentions:            %ld\n",
		       smi->attentions);
	out += sprintf(out, "flag_fetches:          %ld\n",
		       smi->flag_fetches);
	out += sprintf(out, "hosed_count:           %ld\n",
		       smi->hosed_count);
	out += sprintf(out, "complete_transactions: %ld\n",
		       smi->complete_transactions);
	out += sprintf(out, "events:                %ld\n",
		       smi->events);
	out += sprintf(out, "watchdog_pretimeouts:  %ld\n",
		       smi->watchdog_pretimeouts);
	out += sprintf(out, "incoming_messages:     %ld\n",
		       smi->incoming_messages);

	return (out - ((char *) page));
}

/* Returns 0 if initialized, or negative on an error. */
static int init_one_smi(int intf_num, struct smi_info **smi)
{
	int		rv;
	struct smi_info *new_smi;


	rv = try_init_mem(intf_num, &new_smi);
	if (rv)
		rv = try_init_port(intf_num, &new_smi);
#ifdef CONFIG_ACPI_INTERPRETER
	if ((rv) && (si_trydefaults)) {
		rv = try_init_acpi(intf_num, &new_smi);
	}
#endif
#ifdef CONFIG_X86
	if ((rv) && (si_trydefaults)) {
		rv = try_init_smbios(intf_num, &new_smi);
        }
#endif
	if ((rv) && (si_trydefaults)) {
		rv = try_init_plug_and_play(intf_num, &new_smi);
	}


	if (rv)
		return rv;

	/* So we know not to free it unless we have allocated one. */
	new_smi->intf = NULL;
	new_smi->si_sm = NULL;
	new_smi->handlers = NULL;

	if (!new_smi->irq_setup) {
		new_smi->irq = irqs[intf_num];
		new_smi->irq_setup = std_irq_setup;
		new_smi->irq_cleanup = std_irq_cleanup;
	}

	/* Default to KCS if no type is specified. */
	if (si_type[intf_num] == NULL) {
		if (si_trydefaults)
			si_type[intf_num] = "kcs";
		else {
			rv = -EINVAL;
			goto out_err;
		}
	}

	/* Set up the state machine to use. */
	if (strcmp(si_type[intf_num], "kcs") == 0) {
		new_smi->handlers = &kcs_smi_handlers;
		new_smi->si_type = SI_KCS;
	} else if (strcmp(si_type[intf_num], "smic") == 0) {
		new_smi->handlers = &smic_smi_handlers;
		new_smi->si_type = SI_SMIC;
	} else if (strcmp(si_type[intf_num], "bt") == 0) {
		new_smi->handlers = &bt_smi_handlers;
		new_smi->si_type = SI_BT;
	} else {
		/* No support for anything else yet. */
		rv = -EIO;
		goto out_err;
	}

	/* Allocate the state machine's data and initialize it. */
	new_smi->si_sm = kmalloc(new_smi->handlers->size(), GFP_KERNEL);
	if (!new_smi->si_sm) {
		printk(" Could not allocate state machine memory\n");
		rv = -ENOMEM;
		goto out_err;
	}
	new_smi->io_size = new_smi->handlers->init_data(new_smi->si_sm,
							&new_smi->io);

	/* Now that we know the I/O size, we can set up the I/O. */
	rv = new_smi->io_setup(new_smi);
	if (rv) {
		printk(" Could not set up I/O space\n");
		goto out_err;
	}

	spin_lock_init(&(new_smi->si_lock));
	spin_lock_init(&(new_smi->msg_lock));
	spin_lock_init(&(new_smi->count_lock));

	/* Do low-level detection first. */
	if (new_smi->handlers->detect(new_smi->si_sm)) {
		rv = -ENODEV;
		goto out_err;
	}

	/* Attempt a get device id command.  If it fails, we probably
           don't have a SMI here. */
	rv = try_get_dev_id(new_smi);
	if (rv)
		goto out_err;

	/* Try to claim any interrupts. */
	new_smi->irq_setup(new_smi);

	INIT_LIST_HEAD(&(new_smi->xmit_msgs));
	INIT_LIST_HEAD(&(new_smi->hp_xmit_msgs));
	new_smi->curr_msg = NULL;
	atomic_set(&new_smi->req_events, 0);
	new_smi->run_to_completion = 0;

	new_smi->interrupt_disabled = 0;
	new_smi->timer_stopped = 0;
	new_smi->stop_operation = 0;

	/* The ipmi_register_smi() code does some operations to
	   determine the channel information, so we must be ready to
	   handle operations before it is called.  This means we have
	   to stop the timer if we get an error after this point. */
	init_timer(&(new_smi->si_timer));
	new_smi->si_timer.data = (long) new_smi;
	new_smi->si_timer.function = smi_timeout;
	new_smi->last_timeout_jiffies = jiffies;
	new_smi->si_timer.expires = jiffies + SI_TIMEOUT_JIFFIES;
	add_timer(&(new_smi->si_timer));

	rv = ipmi_register_smi(&handlers,
			       new_smi,
			       new_smi->ipmi_version_major,
			       new_smi->ipmi_version_minor,
			       &(new_smi->intf));
	if (rv) {
		printk(KERN_ERR
		       "ipmi_si: Unable to register device: error %d\n",
		       rv);
		goto out_err_stop_timer;
	}

	rv = ipmi_smi_add_proc_entry(new_smi->intf, "type",
				     type_file_read_proc, NULL,
				     new_smi, THIS_MODULE);
	if (rv) {
		printk(KERN_ERR
		       "ipmi_si: Unable to create proc entry: %d\n",
		       rv);
		goto out_err_stop_timer;
	}

	rv = ipmi_smi_add_proc_entry(new_smi->intf, "si_stats",
				     stat_file_read_proc, NULL,
				     new_smi, THIS_MODULE);
	if (rv) {
		printk(KERN_ERR
		       "ipmi_si: Unable to create proc entry: %d\n",
		       rv);
		goto out_err_stop_timer;
	}

	start_clear_flags(new_smi);

	/* IRQ is defined to be set when non-zero. */
	if (new_smi->irq)
		new_smi->si_state = SI_CLEARING_FLAGS_THEN_SET_IRQ;

	*smi = new_smi;

	printk(" IPMI %s interface initialized\n", si_type[intf_num]);

	return 0;

 out_err_stop_timer:
	new_smi->stop_operation = 1;

	/* Wait for the timer to stop.  This avoids problems with race
	   conditions removing the timer here. */
	while (!new_smi->timer_stopped) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(1);
	}

 out_err:
	if (new_smi->intf)
		ipmi_unregister_smi(new_smi->intf);

	new_smi->irq_cleanup(new_smi);

	/* Wait until we know that we are out of any interrupt
	   handlers might have been running before we freed the
	   interrupt. */
	synchronize_kernel();

	if (new_smi->si_sm) {
		if (new_smi->handlers)
			new_smi->handlers->cleanup(new_smi->si_sm);
		kfree(new_smi->si_sm);
	}
	new_smi->io_cleanup(new_smi);

	return rv;
}

static __init int init_ipmi_si(void)
{
	int  rv = 0;
	int  pos = 0;
	int  i;
	char *str;

	if (initialized)
		return 0;
	initialized = 1;

	/* Parse out the si_type string into its components. */
	str = si_type_str;
	if (*str != '\0') {
		for (i=0; (i<SI_MAX_PARMS) && (*str != '\0'); i++) {
			si_type[i] = str;
			str = strchr(str, ',');
			if (str) {
				*str = '\0';
				str++;
			} else {
				break;
			}
		}
	}

	printk(KERN_INFO "IPMI System Interface driver version "
	       IPMI_SI_VERSION);
	if (kcs_smi_handlers.version)
		printk(", KCS version %s", kcs_smi_handlers.version);
	if (smic_smi_handlers.version)
		printk(", SMIC version %s", smic_smi_handlers.version);
	if (bt_smi_handlers.version)
   	        printk(", BT version %s", bt_smi_handlers.version);
	printk("\n");

	rv = init_one_smi(0, &(smi_infos[pos]));
	if (rv && !ports[0] && si_trydefaults) {
		/* If we are trying defaults and the initial port is
                   not set, then set it. */
		si_type[0] = "kcs";
		ports[0] = DEFAULT_KCS_IO_PORT;
		rv = init_one_smi(0, &(smi_infos[pos]));
		if (rv) {
			/* No KCS - try SMIC */
			si_type[0] = "smic";
			ports[0] = DEFAULT_SMIC_IO_PORT;
			rv = init_one_smi(0, &(smi_infos[pos]));
		}
		if (rv) {
			/* No SMIC - try BT */
			si_type[0] = "bt";
			ports[0] = DEFAULT_BT_IO_PORT;
			rv = init_one_smi(0, &(smi_infos[pos]));
		}
	}
	if (rv == 0)
		pos++;

	for (i=1; i < SI_MAX_PARMS; i++) {
		rv = init_one_smi(i, &(smi_infos[pos]));
		if (rv == 0)
			pos++;
	}

	if (smi_infos[0] == NULL) {
		printk("ipmi_si: Unable to find any System Interface(s)\n");
		return -ENODEV;
	}

	return 0;
}
module_init(init_ipmi_si);

void __exit cleanup_one_si(struct smi_info *to_clean)
{
	int           rv;
	unsigned long flags;

	if (! to_clean)
		return;

	/* Tell the timer and interrupt handlers that we are shutting
	   down. */
	spin_lock_irqsave(&(to_clean->si_lock), flags);
	spin_lock(&(to_clean->msg_lock));

	to_clean->stop_operation = 1;

	to_clean->irq_cleanup(to_clean);

	spin_unlock(&(to_clean->msg_lock));
	spin_unlock_irqrestore(&(to_clean->si_lock), flags);

	/* Wait until we know that we are out of any interrupt
	   handlers might have been running before we freed the
	   interrupt. */
	synchronize_kernel();

	/* Wait for the timer to stop.  This avoids problems with race
	   conditions removing the timer here. */
	while (!to_clean->timer_stopped) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(1);
	}

	rv = ipmi_unregister_smi(to_clean->intf);
	if (rv) {
		printk(KERN_ERR
		       "ipmi_si: Unable to unregister device: errno=%d\n",
		       rv);
	}

	to_clean->handlers->cleanup(to_clean->si_sm);

	kfree(to_clean->si_sm);

	to_clean->io_cleanup(to_clean);
}

static __exit void cleanup_ipmi_si(void)
{
	int i;

	if (!initialized)
		return;

	for (i=0; i<SI_MAX_DRIVERS; i++) {
		cleanup_one_si(smi_infos[i]);
	}
}
module_exit(cleanup_ipmi_si);

MODULE_LICENSE("GPL");
