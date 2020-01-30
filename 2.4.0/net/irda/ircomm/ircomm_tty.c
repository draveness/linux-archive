/*********************************************************************
 *                
 * Filename:      ircomm_tty.c
 * Version:       1.0
 * Description:   IrCOMM serial TTY driver
 * Status:        Experimental.
 * Author:        Dag Brattli <dagb@cs.uit.no>
 * Created at:    Sun Jun  6 21:00:56 1999
 * Modified at:   Wed Feb 23 00:09:02 2000
 * Modified by:   Dag Brattli <dagb@cs.uit.no>
 * Sources:       serial.c and previous IrCOMM work by Takahide Higuchi
 * 
 *     Copyright (c) 1999-2000 Dag Brattli, All Rights Reserved.
 *     
 *     This program is free software; you can redistribute it and/or 
 *     modify it under the terms of the GNU General Public License as 
 *     published by the Free Software Foundation; either version 2 of 
 *     the License, or (at your option) any later version.
 * 
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *     GNU General Public License for more details.
 * 
 *     You should have received a copy of the GNU General Public License 
 *     along with this program; if not, write to the Free Software 
 *     Foundation, Inc., 59 Temple Place, Suite 330, Boston, 
 *     MA 02111-1307 USA
 *     
 ********************************************************************/

#include <linux/config.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/termios.h>
#include <linux/tty.h>
#include <linux/interrupt.h>

#include <asm/segment.h>
#include <asm/uaccess.h>

#include <net/irda/irda.h>
#include <net/irda/irmod.h>

#include <net/irda/ircomm_core.h>
#include <net/irda/ircomm_param.h>
#include <net/irda/ircomm_tty_attach.h>
#include <net/irda/ircomm_tty.h>

static int  ircomm_tty_open(struct tty_struct *tty, struct file *filp);
static void ircomm_tty_close(struct tty_struct * tty, struct file *filp);
static int  ircomm_tty_write(struct tty_struct * tty, int from_user,
			     const unsigned char *buf, int count);
static int  ircomm_tty_write_room(struct tty_struct *tty);
static void ircomm_tty_throttle(struct tty_struct *tty);
static void ircomm_tty_unthrottle(struct tty_struct *tty);
static int  ircomm_tty_chars_in_buffer(struct tty_struct *tty);
static void ircomm_tty_flush_buffer(struct tty_struct *tty);
static void ircomm_tty_send_xchar(struct tty_struct *tty, char ch);
static void ircomm_tty_wait_until_sent(struct tty_struct *tty, int timeout);
static void ircomm_tty_hangup(struct tty_struct *tty);
static void ircomm_tty_do_softint(void *private_);
static void ircomm_tty_shutdown(struct ircomm_tty_cb *self);

static int ircomm_tty_data_indication(void *instance, void *sap,
				      struct sk_buff *skb);
static int ircomm_tty_control_indication(void *instance, void *sap,
					 struct sk_buff *skb);
static void ircomm_tty_flow_indication(void *instance, void *sap, 
				       LOCAL_FLOW cmd);
#ifdef CONFIG_PROC_FS
static int ircomm_tty_read_proc(char *buf, char **start, off_t offset, int len,
				int *eof, void *unused);
#endif /* CONFIG_PROC_FS */
static struct tty_driver driver;
static int ircomm_tty_refcount;       /* If we manage several devices */

static struct tty_struct *ircomm_tty_table[NR_PTYS];
static struct termios *ircomm_tty_termios[NR_PTYS];
static struct termios *ircomm_tty_termios_locked[NR_PTYS];

hashbin_t *ircomm_tty = NULL;

/*
 * Function ircomm_tty_init()
 *
 *    Init IrCOMM TTY layer/driver
 *
 */
int __init ircomm_tty_init(void)
{	
	ircomm_tty = hashbin_new(HB_LOCAL); 
	if (ircomm_tty == NULL) {
		ERROR(__FUNCTION__ "(), can't allocate hashbin!\n");
		return -ENOMEM;
	}

	memset(&driver, 0, sizeof(struct tty_driver));
	driver.magic           = TTY_DRIVER_MAGIC;
	driver.driver_name     = "ircomm";
#ifdef CONFIG_DEVFS_FS
	driver.name            = "ircomm%d";
#else
	driver.name            = "ircomm";
#endif
	driver.major           = IRCOMM_TTY_MAJOR;
	driver.minor_start     = IRCOMM_TTY_MINOR;
	driver.num             = IRCOMM_TTY_PORTS;
	driver.type            = TTY_DRIVER_TYPE_SERIAL;
	driver.subtype         = SERIAL_TYPE_NORMAL;
	driver.init_termios    = tty_std_termios;
	driver.init_termios.c_cflag = B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	driver.flags           = TTY_DRIVER_REAL_RAW;
	driver.refcount        = &ircomm_tty_refcount;
	driver.table           = ircomm_tty_table;
	driver.termios         = ircomm_tty_termios;
	driver.termios_locked  = ircomm_tty_termios_locked;
	driver.open            = ircomm_tty_open;
	driver.close           = ircomm_tty_close;
	driver.write           = ircomm_tty_write;
	driver.write_room      = ircomm_tty_write_room;
	driver.chars_in_buffer = ircomm_tty_chars_in_buffer;
	driver.flush_buffer    = ircomm_tty_flush_buffer;
	driver.ioctl           = ircomm_tty_ioctl;
	driver.throttle        = ircomm_tty_throttle;
	driver.unthrottle      = ircomm_tty_unthrottle;
	driver.send_xchar      = ircomm_tty_send_xchar;
	driver.set_termios     = ircomm_tty_set_termios;
	driver.stop            = ircomm_tty_stop;
	driver.start           = ircomm_tty_start;
	driver.hangup          = ircomm_tty_hangup;
	driver.wait_until_sent = ircomm_tty_wait_until_sent;
#ifdef CONFIG_PROC_FS
	driver.read_proc       = ircomm_tty_read_proc;
#endif /* CONFIG_PROC_FS */
	if (tty_register_driver(&driver)) {
		ERROR(__FUNCTION__ "Couldn't register serial driver\n");
		return -1;
	}
	return 0;
}

#ifdef MODULE
static void __ircomm_tty_cleanup(struct ircomm_tty_cb *self)
{
	IRDA_DEBUG(0, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return;);

	ircomm_tty_shutdown(self);

	self->magic = 0;
	kfree(self);
}

/*
 * Function ircomm_tty_cleanup ()
 *
 *    Remove IrCOMM TTY layer/driver
 *
 */
void ircomm_tty_cleanup(void)
{
	int ret;

	IRDA_DEBUG(4, __FUNCTION__"()\n");	

	ret = tty_unregister_driver(&driver);
        if (ret) {
                ERROR(__FUNCTION__ "(), failed to unregister driver\n");
		return;
	}

	hashbin_delete(ircomm_tty, (FREE_FUNC) __ircomm_tty_cleanup);
}
#endif /* MODULE */

/*
 * Function ircomm_startup (self)
 *
 *    
 *
 */
static int ircomm_tty_startup(struct ircomm_tty_cb *self)
{
	notify_t notify;
	int ret;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);

	/* Already open */
	if (self->flags & ASYNC_INITIALIZED) {
		IRDA_DEBUG(2, __FUNCTION__ "(), already open so break out!\n");
		return 0;
	}

	/* Register with IrCOMM */
	irda_notify_init(&notify);
	/* These callbacks we must handle ourselves */
	notify.data_indication       = ircomm_tty_data_indication;
	notify.udata_indication      = ircomm_tty_control_indication;
 	notify.flow_indication       = ircomm_tty_flow_indication;

	/* Use the ircomm_tty interface for these ones */
 	notify.disconnect_indication = ircomm_tty_disconnect_indication;
	notify.connect_confirm       = ircomm_tty_connect_confirm;
 	notify.connect_indication    = ircomm_tty_connect_indication;
	strncpy(notify.name, "ircomm_tty", NOTIFY_MAX_NAME);
	notify.instance = self;

	if (!self->ircomm) {
		self->ircomm = ircomm_open(&notify, self->service_type, 
					   self->line);
	}
	if (!self->ircomm)
		return -ENODEV;

	self->slsap_sel = self->ircomm->slsap_sel;

	/* Connect IrCOMM link with remote device */
	ret = ircomm_tty_attach_cable(self);
	if (ret < 0) {
		ERROR(__FUNCTION__ "(), error attaching cable!\n");
		return ret;
	}

	self->flags |= ASYNC_INITIALIZED;

	return 0;
}

/*
 * Function ircomm_block_til_ready (self, filp)
 *
 *    
 *
 */
static int ircomm_tty_block_til_ready(struct ircomm_tty_cb *self, 
				      struct file *filp)
{
	DECLARE_WAITQUEUE(wait, current);
	int		retval;
	int		do_clocal = 0, extra_count = 0;
	unsigned long	flags;
	struct tty_struct *tty;
	
	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	tty = self->tty;

	if (tty->driver.subtype == SERIAL_TYPE_CALLOUT) {
		/* this is a callout device */
		/* just verify that normal device is not in use */
		if (self->flags & ASYNC_NORMAL_ACTIVE)
			return -EBUSY;
		if ((self->flags & ASYNC_CALLOUT_ACTIVE) &&
		    (self->flags & ASYNC_SESSION_LOCKOUT) &&
		    (self->session != current->session))
			return -EBUSY;
		if ((self->flags & ASYNC_CALLOUT_ACTIVE) &&
		    (self->flags & ASYNC_PGRP_LOCKOUT) &&
		    (self->pgrp != current->pgrp))
			return -EBUSY;
		self->flags |= ASYNC_CALLOUT_ACTIVE;
		return 0;
	}
	
	/*
	 * If non-blocking mode is set, or the port is not enabled,
	 * then make the check up front and then exit.
	 */	
	if (filp->f_flags & O_NONBLOCK || tty->flags & (1 << TTY_IO_ERROR)){
		/* nonblock mode is set or port is not enabled */
		/* just verify that callout device is not active */
		if (self->flags & ASYNC_CALLOUT_ACTIVE)
			return -EBUSY;
		self->flags |= ASYNC_NORMAL_ACTIVE;

		IRDA_DEBUG(1, __FUNCTION__ "(), O_NONBLOCK requested!\n");
		return 0;
	}

	if (self->flags & ASYNC_CALLOUT_ACTIVE) {
		if (self->normal_termios.c_cflag & CLOCAL) {
			IRDA_DEBUG(1, __FUNCTION__ "(), doing CLOCAL!\n");
			do_clocal = 1;
		}
	} else {
		if (tty->termios->c_cflag & CLOCAL) {
			IRDA_DEBUG(1, __FUNCTION__ "(), doing CLOCAL!\n");
			do_clocal = 1;
		}
	}
	
	/* Wait for carrier detect and the line to become
	 * free (i.e., not in use by the callout).  While we are in
	 * this loop, self->open_count is dropped by one, so that
	 * mgsl_close() knows when to free things.  We restore it upon
	 * exit, either normal or abnormal.
	 */
	 
	retval = 0;
	add_wait_queue(&self->open_wait, &wait);
	
	IRDA_DEBUG(2, "%s(%d):block_til_ready before block on %s open_count=%d\n",
	      __FILE__,__LINE__, tty->driver.name, self->open_count );

	save_flags(flags); cli();
	if (!tty_hung_up_p(filp)) {
		extra_count = 1;
		self->open_count--;
	}
	restore_flags(flags);
	self->blocked_open++;
	
	while (1) {
		if (!(self->flags & ASYNC_CALLOUT_ACTIVE) &&
 		    (tty->termios->c_cflag & CBAUD)) {
			save_flags(flags); cli();
			self->settings.dte |= IRCOMM_RTS + IRCOMM_DTR;
		 	
			ircomm_param_request(self, IRCOMM_DTE, TRUE);
			restore_flags(flags);
		}
		
		current->state = TASK_INTERRUPTIBLE;
		
		if (tty_hung_up_p(filp) || !(self->flags & ASYNC_INITIALIZED)){
			retval = (self->flags & ASYNC_HUP_NOTIFY) ?
					-EAGAIN : -ERESTARTSYS;
			break;
		}
		
		/*  
		 * Check if link is ready now. Even if CLOCAL is
		 * specified, we cannot return before the IrCOMM link is
		 * ready 
		 */
 		if (!(self->flags & ASYNC_CALLOUT_ACTIVE) &&
 		    !(self->flags & ASYNC_CLOSING) &&
 		    (do_clocal || (self->settings.dce & IRCOMM_CD)) &&
		    self->state == IRCOMM_TTY_READY)
		{
 			break;
		}
			
		if (signal_pending(current)) {
			retval = -ERESTARTSYS;
			break;
		}
		
		IRDA_DEBUG(1, "%s(%d):block_til_ready blocking on %s open_count=%d\n",
		      __FILE__,__LINE__, tty->driver.name, self->open_count );
		
		schedule();
	}
	
	__set_current_state(TASK_RUNNING);
	remove_wait_queue(&self->open_wait, &wait);
	
	if (extra_count)
		self->open_count++;
	self->blocked_open--;
	
	IRDA_DEBUG(1, "%s(%d):block_til_ready after blocking on %s open_count=%d\n",
	      __FILE__,__LINE__, tty->driver.name, self->open_count);
			 
	if (!retval)
		self->flags |= ASYNC_NORMAL_ACTIVE;
		
	return retval;	
}

/*
 * Function ircomm_tty_open (tty, filp)
 *
 *    This routine is called when a particular tty device is opened. This
 *    routine is mandatory; if this routine is not filled in, the attempted
 *    open will fail with ENODEV.
 */
static int ircomm_tty_open(struct tty_struct *tty, struct file *filp)
{
	struct ircomm_tty_cb *self;
	int line;
	int ret;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	MOD_INC_USE_COUNT;
	line = MINOR(tty->device) - tty->driver.minor_start;
	if ((line < 0) || (line >= IRCOMM_TTY_PORTS)) {
		MOD_DEC_USE_COUNT;
		return -ENODEV;
	}

	/* Check if instance already exists */
	self = hashbin_find(ircomm_tty, line, NULL);
	if (!self) {
		/* No, so make new instance */
		self = kmalloc(sizeof(struct ircomm_tty_cb), GFP_KERNEL);
		if (self == NULL) {
			ERROR(__FUNCTION__"(), kmalloc failed!\n");
			MOD_DEC_USE_COUNT;
			return -ENOMEM;
		}
		memset(self, 0, sizeof(struct ircomm_tty_cb));
		
		self->magic = IRCOMM_TTY_MAGIC;
		self->flow = FLOW_STOP;

		self->line = line;
		self->tqueue.routine = ircomm_tty_do_softint;
		self->tqueue.data = self;
		self->max_header_size = 5;
		self->max_data_size = 64-self->max_header_size;
		self->close_delay = 5*HZ/10;
		self->closing_wait = 30*HZ;

		/* Init some important stuff */
		init_timer(&self->watchdog_timer);
		init_waitqueue_head(&self->open_wait);
 		init_waitqueue_head(&self->close_wait);

		/* 
		 * Force TTY into raw mode by default which is usually what
		 * we want for IrCOMM and IrLPT. This way applications will
		 * not have to twiddle with printcap etc.  
		 */
		tty->termios->c_iflag = 0;
		tty->termios->c_oflag = 0;

		/* Insert into hash */
		hashbin_insert(ircomm_tty, (irda_queue_t *) self, line, NULL);
	}
	self->open_count++;

	tty->driver_data = self;
	self->tty = tty;

	IRDA_DEBUG(1, __FUNCTION__"(), %s%d, count = %d\n", tty->driver.name, 
		   self->line, self->open_count);

	/* Not really used by us, but lets do it anyway */
	self->tty->low_latency = (self->flags & ASYNC_LOW_LATENCY) ? 1 : 0;

	/*
	 * If the port is the middle of closing, bail out now
	 */
	if (tty_hung_up_p(filp) ||
	    (self->flags & ASYNC_CLOSING)) {
		if (self->flags & ASYNC_CLOSING)
			interruptible_sleep_on(&self->close_wait);
		/* MOD_DEC_USE_COUNT; "info->tty" will cause this? */
#ifdef SERIAL_DO_RESTART
		return ((self->flags & ASYNC_HUP_NOTIFY) ?
			-EAGAIN : -ERESTARTSYS);
#else
		return -EAGAIN;
#endif
	}

	/* Check if this is a "normal" ircomm device, or an irlpt device */
	if (line < 0x10) {
		self->service_type = IRCOMM_3_WIRE | IRCOMM_9_WIRE;
		self->settings.service_type = IRCOMM_9_WIRE; /* Default */
		IRDA_DEBUG(2, __FUNCTION__ "(), IrCOMM device\n");
	} else {
		IRDA_DEBUG(2, __FUNCTION__ "(), IrLPT device\n");
		self->service_type = IRCOMM_3_WIRE_RAW;
		self->settings.service_type = IRCOMM_3_WIRE_RAW; /* Default */
	}

	ret = ircomm_tty_startup(self);
	if (ret)
		return ret;

	ret = ircomm_tty_block_til_ready(self, filp);
	if (ret) {
		/* MOD_DEC_USE_COUNT; "info->tty" will cause this? */
		IRDA_DEBUG(2, __FUNCTION__ 
		      "(), returning after block_til_ready with %d\n",
		      ret);

		return ret;
	}

	self->session = current->session;
	self->pgrp = current->pgrp;

	return 0;
}

/*
 * Function ircomm_tty_close (tty, filp)
 *
 *    This routine is called when a particular tty device is closed.
 *
 */
static void ircomm_tty_close(struct tty_struct *tty, struct file *filp)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) tty->driver_data;
	unsigned long flags;

	IRDA_DEBUG(0, __FUNCTION__ "()\n");

	if (!tty)
		return;

	save_flags(flags); 
	cli();

	if (tty_hung_up_p(filp)) {
		MOD_DEC_USE_COUNT;
		restore_flags(flags);

		IRDA_DEBUG(0, __FUNCTION__ "(), returning 1\n");
		return;
	}

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return;);

	if ((tty->count == 1) && (self->open_count != 1)) {
		/*
		 * Uh, oh.  tty->count is 1, which means that the tty
		 * structure will be freed.  state->count should always
		 * be one in these conditions.  If it's greater than
		 * one, we've got real problems, since it means the
		 * serial port won't be shutdown.
		 */
		IRDA_DEBUG(0, __FUNCTION__ "(), bad serial port count; "
			   "tty->count is 1, state->count is %d\n", 
			   self->open_count);
		self->open_count = 1;
	}

	if (--self->open_count < 0) {
		ERROR(__FUNCTION__ 
		      "(), bad serial port count for ttys%d: %d\n",
		      self->line, self->open_count);
		self->open_count = 0;
	}
	if (self->open_count) {
		MOD_DEC_USE_COUNT;
		restore_flags(flags);

		IRDA_DEBUG(0, __FUNCTION__ "(), open count > 0\n");
		return;
	}
	self->flags |= ASYNC_CLOSING;

	/*
	 * Now we wait for the transmit buffer to clear; and we notify 
	 * the line discipline to only process XON/XOFF characters.
	 */
	tty->closing = 1;
	if (self->closing_wait != ASYNC_CLOSING_WAIT_NONE)
		tty_wait_until_sent(tty, self->closing_wait);

	ircomm_tty_shutdown(self);

	if (tty->driver.flush_buffer)
		tty->driver.flush_buffer(tty);
	if (tty->ldisc.flush_buffer)
		tty->ldisc.flush_buffer(tty);

	tty->closing = 0;
	self->tty = 0;

	if (self->blocked_open) {
		if (self->close_delay) {
			current->state = TASK_INTERRUPTIBLE;
			schedule_timeout(self->close_delay);
		}
		wake_up_interruptible(&self->open_wait);
	}

	self->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CALLOUT_ACTIVE|
			 ASYNC_CLOSING);
	wake_up_interruptible(&self->close_wait);

	MOD_DEC_USE_COUNT;
	restore_flags(flags);
}

/*
 * Function ircomm_tty_flush_buffer (tty)
 *
 *    
 *
 */
static void ircomm_tty_flush_buffer(struct tty_struct *tty)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) tty->driver_data;

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return;);

	/* 
	 * Let do_softint() do this to avoid race condition with 
	 * do_softint() ;-) 
	 */
	queue_task(&self->tqueue, &tq_immediate);
	mark_bh(IMMEDIATE_BH);
}

/*
 * Function ircomm_tty_do_softint (private_)
 *
 *    We use this routine to give the write wakeup to the user at at a
 *    safe time (as fast as possible after write have completed). This 
 *    can be compared to the Tx interrupt.
 */
static void ircomm_tty_do_softint(void *private_)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) private_;
	struct tty_struct *tty;
	unsigned long flags;
	struct sk_buff *skb, *ctrl_skb;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	if (!self || self->magic != IRCOMM_TTY_MAGIC)
		return;

	tty = self->tty;
	if (!tty)
		return;

	/* Unlink control buffer */
	save_flags(flags);
	cli();

	ctrl_skb = self->ctrl_skb;
	self->ctrl_skb = NULL;

	restore_flags(flags);

	/* Flush control buffer if any */
	if (ctrl_skb && self->flow == FLOW_START)
		ircomm_control_request(self->ircomm, ctrl_skb);

	if (tty->hw_stopped)
		return;

	/* Unlink transmit buffer */
	save_flags(flags);
	cli();
	
	skb = self->tx_skb;
	self->tx_skb = NULL;

	restore_flags(flags);	

	/* Flush transmit buffer if any */
	if (skb)
		ircomm_tty_do_event(self, IRCOMM_TTY_DATA_REQUEST, skb, NULL);
		
	/* Check if user (still) wants to be waken up */
	if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) && 
	    tty->ldisc.write_wakeup)
	{
		(tty->ldisc.write_wakeup)(tty);
	}
	wake_up_interruptible(&tty->write_wait);
}

/*
 * Function ircomm_tty_write (tty, from_user, buf, count)
 *
 *    This routine is called by the kernel to write a series of characters
 *    to the tty device. The characters may come from user space or kernel
 *    space. This routine will return the number of characters actually
 *    accepted for writing. This routine is mandatory.
 */
static int ircomm_tty_write(struct tty_struct *tty, int from_user,
			    const unsigned char *buf, int count)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) tty->driver_data;
	unsigned long flags;
	struct sk_buff *skb;
	int tailroom = 0;
	int len = 0;
	int size;

	IRDA_DEBUG(2, __FUNCTION__ "(), count=%d, hw_stopped=%d\n", count,
		   tty->hw_stopped);

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);

	save_flags(flags);
	cli();

	/* Fetch current transmit buffer */
	skb = self->tx_skb;

	/*  
	 * Send out all the data we get, possibly as multiple fragmented
	 * frames, but this will only happen if the data is larger than the
	 * max data size. The normal case however is just the opposite, and
	 * this function may be called multiple times, and will then actually
	 * defragment the data and send it out as one packet as soon as 
	 * possible, but at a safer point in time
	 */
	while (count) {
		size = count;

		/* Adjust data size to the max data size */
		if (size > self->max_data_size)
			size = self->max_data_size;
		
		/* 
		 * Do we already have a buffer ready for transmit, or do
		 * we need to allocate a new frame 
		 */
		if (skb) {			
			/* 
			 * Any room for more data at the end of the current 
			 * transmit buffer? Cannot use skb_tailroom, since
			 * dev_alloc_skb gives us a larger skb than we 
			 * requested
			 */
			if ((tailroom = (self->max_data_size-skb->len)) > 0) {
				/* Adjust data to tailroom */
				if (size > tailroom)
					size = tailroom;
			} else {
				/* 
				 * Current transmit frame is full, so break 
				 * out, so we can send it as soon as possible
				 */
				break;
			}
		} else {
			/* Prepare a full sized frame */
			skb = dev_alloc_skb(self->max_data_size+
					    self->max_header_size);
			if (!skb) {
				restore_flags(flags);
				return -ENOBUFS;
			}
			skb_reserve(skb, self->max_header_size);
			self->tx_skb = skb;
		}
		
		/* Copy data */
		if (from_user)
			copy_from_user(skb_put(skb,size), buf+len, size);
		else
			memcpy(skb_put(skb,size), buf+len, size);
		
		count -= size;
		len += size;
	}

	restore_flags(flags);

	/*     
	 * Schedule a new thread which will transmit the frame as soon
	 * as possible, but at a safe point in time. We do this so the
	 * "user" can give us data multiple times, as PPP does (because of
	 * its 256 byte tx buffer). We will then defragment and send out
	 * all this data as one single packet.  
	 */
	queue_task(&self->tqueue, &tq_immediate);
	mark_bh(IMMEDIATE_BH);
	
	return len;
}

/*
 * Function ircomm_tty_write_room (tty)
 *
 *    This routine returns the numbers of characters the tty driver will
 *    accept for queuing to be written. This number is subject to change as
 *    output buffers get emptied, or if the output flow control is acted.
 */
static int ircomm_tty_write_room(struct tty_struct *tty)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) tty->driver_data;
	unsigned long flags;
	int ret;

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);

	/* Check if we are allowed to transmit any data */
	if (tty->hw_stopped)
		ret = 0;
	else {
		save_flags(flags);
		cli();
		if (self->tx_skb)
			ret = self->max_data_size - self->tx_skb->len;
		else
			ret = self->max_data_size;
		restore_flags(flags);
	}
	IRDA_DEBUG(2, __FUNCTION__ "(), ret=%d\n", ret);

	return ret;
}

/*
 * Function ircomm_tty_wait_until_sent (tty, timeout)
 *
 *    This routine waits until the device has written out all of the
 *    characters in its transmitter FIFO.
 */
static void ircomm_tty_wait_until_sent(struct tty_struct *tty, int timeout)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) tty->driver_data;
	unsigned long orig_jiffies, poll_time;
	
	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return;);

	orig_jiffies = jiffies;

	/* Set poll time to 200 ms */
	poll_time = IRDA_MIN(timeout, MSECS_TO_JIFFIES(200));

	while (self->tx_skb && self->tx_skb->len) {
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(poll_time);
		if (signal_pending(current))
			break;
		if (timeout && time_after(jiffies, orig_jiffies + timeout))
			break;
	}
	current->state = TASK_RUNNING;
}

/*
 * Function ircomm_tty_throttle (tty)
 *
 *    This routine notifies the tty driver that input buffers for the line
 *    discipline are close to full, and it should somehow signal that no
 *    more characters should be sent to the tty.  
 */
static void ircomm_tty_throttle(struct tty_struct *tty)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) tty->driver_data;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return;);

	/* Software flow control? */
	if (I_IXOFF(tty))
		ircomm_tty_send_xchar(tty, STOP_CHAR(tty));
	
	/* Hardware flow control? */
	if (tty->termios->c_cflag & CRTSCTS) {
		self->settings.dte &= ~IRCOMM_RTS;
		self->settings.dte |= IRCOMM_DELTA_RTS;
	
		ircomm_param_request(self, IRCOMM_DTE, TRUE);
	}

        ircomm_flow_request(self->ircomm, FLOW_STOP);
}

/*
 * Function ircomm_tty_unthrottle (tty)
 *
 *    This routine notifies the tty drivers that it should signals that
 *    characters can now be sent to the tty without fear of overrunning the
 *    input buffers of the line disciplines.
 */
static void ircomm_tty_unthrottle(struct tty_struct *tty)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) tty->driver_data;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return;);

	/* Using software flow control? */
	if (I_IXOFF(tty)) {
		ircomm_tty_send_xchar(tty, START_CHAR(tty));
	}

	/* Using hardware flow control? */
	if (tty->termios->c_cflag & CRTSCTS) {
		self->settings.dte |= (IRCOMM_RTS|IRCOMM_DELTA_RTS);

		ircomm_param_request(self, IRCOMM_DTE, TRUE);
		IRDA_DEBUG(1, __FUNCTION__"(), FLOW_START\n");
	}
        ircomm_flow_request(self->ircomm, FLOW_START);
}

/*
 * Function ircomm_tty_chars_in_buffer (tty)
 *
 *    Indicates if there are any data in the buffer
 *
 */
static int ircomm_tty_chars_in_buffer(struct tty_struct *tty)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) tty->driver_data;
	unsigned long flags;
	int len = 0;

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);

	save_flags(flags);
	cli();

	if (self->tx_skb)
		len = self->tx_skb->len;

	restore_flags(flags);

	return len;
}

static void ircomm_tty_shutdown(struct ircomm_tty_cb *self)
{
	unsigned long flags;

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return;);

	IRDA_DEBUG(0, __FUNCTION__ "()\n");
	
	if (!(self->flags & ASYNC_INITIALIZED))
		return;

	save_flags(flags);
	cli();

	del_timer(&self->watchdog_timer);
	
	/* Free parameter buffer */
	if (self->ctrl_skb) {
		dev_kfree_skb(self->ctrl_skb);
		self->ctrl_skb = NULL;
	}

	/* Free transmit buffer */
	if (self->tx_skb) {
		dev_kfree_skb(self->tx_skb);
		self->tx_skb = NULL;
	}

	ircomm_tty_detach_cable(self);

	if (self->ircomm) {
		ircomm_close(self->ircomm);
		self->ircomm = NULL;
	}
	self->flags &= ~ASYNC_INITIALIZED;

	restore_flags(flags);
}

/*
 * Function ircomm_tty_hangup (tty)
 *
 *    This routine notifies the tty driver that it should hangup the tty
 *    device.
 * 
 */
static void ircomm_tty_hangup(struct tty_struct *tty)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) tty->driver_data;

	IRDA_DEBUG(0, __FUNCTION__"()\n");

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return;);

	if (!tty)
		return;

	/* ircomm_tty_flush_buffer(tty); */
	ircomm_tty_shutdown(self);

	self->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CALLOUT_ACTIVE);
	self->tty = 0;
	self->open_count = 0;
	wake_up_interruptible(&self->open_wait);
}

/*
 * Function ircomm_tty_send_xchar (tty, ch)
 *
 *    This routine is used to send a high-priority XON/XOFF character to
 *    the device.
 */
static void ircomm_tty_send_xchar(struct tty_struct *tty, char ch)
{
	IRDA_DEBUG(0, __FUNCTION__"(), not impl\n");
}

/*
 * Function ircomm_tty_start (tty)
 *
 *    This routine notifies the tty driver that it resume sending
 *    characters to the tty device.  
 */
void ircomm_tty_start(struct tty_struct *tty)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) tty->driver_data;

	ircomm_flow_request(self->ircomm, FLOW_START);
}

/*
 * Function ircomm_tty_stop (tty)
 *
 *     This routine notifies the tty driver that it should stop outputting
 *     characters to the tty device. 
 */
void ircomm_tty_stop(struct tty_struct *tty) 
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) tty->driver_data;

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return;);

	ircomm_flow_request(self->ircomm, FLOW_STOP);
}

/*
 * Function ircomm_check_modem_status (self)
 *
 *    Check for any changes in the DCE's line settings. This function should
 *    be called whenever the dce parameter settings changes, to update the
 *    flow control settings and other things
 */
void ircomm_tty_check_modem_status(struct ircomm_tty_cb *self)
{
	struct tty_struct *tty;
	int status;

	IRDA_DEBUG(0, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return;);

	tty = self->tty;

	status = self->settings.dce;

	if (status & IRCOMM_DCE_DELTA_ANY) {
		/*wake_up_interruptible(&self->delta_msr_wait);*/
	}
	if ((self->flags & ASYNC_CHECK_CD) && (status & IRCOMM_DELTA_CD)) {
		IRDA_DEBUG(2, __FUNCTION__ 
			   "(), ircomm%d CD now %s...\n", self->line,
			   (status & IRCOMM_CD) ? "on" : "off");

		if (status & IRCOMM_CD) {
			wake_up_interruptible(&self->open_wait);
		} else if (!((self->flags & ASYNC_CALLOUT_ACTIVE) &&
			   (self->flags & ASYNC_CALLOUT_NOHUP))) 
		{
			IRDA_DEBUG(2, __FUNCTION__ 
				   "(), Doing serial hangup..\n");
			if (tty)
				tty_hangup(tty);

			/* Hangup will remote the tty, so better break out */
			return;
		}
	}
	if (self->flags & ASYNC_CTS_FLOW) {
		if (tty->hw_stopped) {
			if (status & IRCOMM_CTS) {
				IRDA_DEBUG(2, __FUNCTION__ 
					   "(), CTS tx start...\n");
				tty->hw_stopped = 0;
				
				/* Wake up processes blocked on open */
				wake_up_interruptible(&self->open_wait);

				queue_task(&self->tqueue, &tq_immediate);
				mark_bh(IMMEDIATE_BH);
				return;
			}
		} else {
			if (!(status & IRCOMM_CTS)) {
				IRDA_DEBUG(2, __FUNCTION__ 
					   "(), CTS tx stop...\n");
				tty->hw_stopped = 1;
			}
		}
	}
}

/*
 * Function ircomm_tty_data_indication (instance, sap, skb)
 *
 *    Handle incomming data, and deliver it to the line discipline
 *
 */
static int ircomm_tty_data_indication(void *instance, void *sap,
				      struct sk_buff *skb)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;

	IRDA_DEBUG(2, __FUNCTION__"()\n");
	
	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);
	ASSERT(skb != NULL, return -1;);

	if (!self->tty) {
		IRDA_DEBUG(0, __FUNCTION__ "(), no tty!\n");
		dev_kfree_skb(skb);
		return 0;
	}

	/* 
	 * If we receive data when hardware is stopped then something is wrong.
	 * We try to poll the peers line settings to check if we are up todate.
	 * Devices like WinCE can do this, and since they don't send any 
	 * params, we can just as well declare the hardware for running.
	 */
	if (self->tty->hw_stopped && (self->flow == FLOW_START)) {
		IRDA_DEBUG(0, __FUNCTION__ "(), polling for line settings!\n");
		ircomm_param_request(self, IRCOMM_POLL, TRUE);

		/* We can just as well declare the hardware for running */
		ircomm_tty_send_initial_parameters(self);
		ircomm_tty_link_established(self);
	}

	/* 
	 * Just give it over to the line discipline. There is no need to
	 * involve the flip buffers, since we are not running in an interrupt 
	 * handler
	 */
	self->tty->ldisc.receive_buf(self->tty, skb->data, NULL, skb->len);
	dev_kfree_skb(skb);

	return 0;
}

/*
 * Function ircomm_tty_control_indication (instance, sap, skb)
 *
 *    Parse all incomming parameters (easy!)
 *
 */
static int ircomm_tty_control_indication(void *instance, void *sap,
					 struct sk_buff *skb)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;
	int clen;

	IRDA_DEBUG(4, __FUNCTION__"()\n");
	
	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);
	ASSERT(skb != NULL, return -1;);

	clen = skb->data[0];

	irda_param_extract_all(self, skb->data+1, IRDA_MIN(skb->len-1, clen), 
			       &ircomm_param_info);
	dev_kfree_skb(skb);

	return 0;
}

/*
 * Function ircomm_tty_flow_indication (instance, sap, cmd)
 *
 *    This function is called by IrTTP when it wants us to slow down the
 *    transmission of data. We just mark the hardware as stopped, and wait
 *    for IrTTP to notify us that things are OK again.
 */
static void ircomm_tty_flow_indication(void *instance, void *sap, 
				       LOCAL_FLOW cmd)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;
	struct tty_struct *tty;

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return;);

	tty = self->tty;

	switch (cmd) {
	case FLOW_START:
		IRDA_DEBUG(2, __FUNCTION__ "(), hw start!\n");
		tty->hw_stopped = 0;

		/* ircomm_tty_do_softint will take care of the rest */
		queue_task(&self->tqueue, &tq_immediate);
		mark_bh(IMMEDIATE_BH);
		break;
	default:  /* If we get here, something is very wrong, better stop */
	case FLOW_STOP:
		IRDA_DEBUG(2, __FUNCTION__ "(), hw stopped!\n");
		tty->hw_stopped = 1;
		break;
	}
	self->flow = cmd;
}

static int ircomm_tty_line_info(struct ircomm_tty_cb *self, char *buf)
{
        int  ret=0;

	ret += sprintf(buf+ret, "State: %s\n", ircomm_tty_state[self->state]);

	ret += sprintf(buf+ret, "Service type: ");
	if (self->service_type & IRCOMM_9_WIRE)
		ret += sprintf(buf+ret, "9_WIRE");
	else if (self->service_type & IRCOMM_3_WIRE)
		ret += sprintf(buf+ret, "3_WIRE");
	else if (self->service_type & IRCOMM_3_WIRE_RAW)
		ret += sprintf(buf+ret, "3_WIRE_RAW");
	else
		ret += sprintf(buf+ret, "No common service type!\n");
        ret += sprintf(buf+ret, "\n");

	ret += sprintf(buf+ret, "Port name: %s\n", self->settings.port_name);

	ret += sprintf(buf+ret, "DTE status: ");	
        if (self->settings.dte & IRCOMM_RTS)
                ret += sprintf(buf+ret, "RTS|");
        if (self->settings.dte & IRCOMM_DTR)
                ret += sprintf(buf+ret, "DTR|");
	if (self->settings.dte)
		ret--; /* remove the last | */
        ret += sprintf(buf+ret, "\n");

	ret += sprintf(buf+ret, "DCE status: ");
        if (self->settings.dce & IRCOMM_CTS)
                ret += sprintf(buf+ret, "CTS|");
        if (self->settings.dce & IRCOMM_DSR)
                ret += sprintf(buf+ret, "DSR|");
        if (self->settings.dce & IRCOMM_CD)
                ret += sprintf(buf+ret, "CD|");
        if (self->settings.dce & IRCOMM_RI) 
                ret += sprintf(buf+ret, "RI|");
	if (self->settings.dce)
		ret--; /* remove the last | */
        ret += sprintf(buf+ret, "\n");

	ret += sprintf(buf+ret, "Configuration: ");
	if (!self->settings.null_modem)
		ret += sprintf(buf+ret, "DTE <-> DCE\n");
	else
		ret += sprintf(buf+ret, 
			       "DTE <-> DTE (null modem emulation)\n");

	ret += sprintf(buf+ret, "Data rate: %d\n", self->settings.data_rate);

	ret += sprintf(buf+ret, "Flow control: ");
	if (self->settings.flow_control & IRCOMM_XON_XOFF_IN)
		ret += sprintf(buf+ret, "XON_XOFF_IN|");
	if (self->settings.flow_control & IRCOMM_XON_XOFF_OUT)
		ret += sprintf(buf+ret, "XON_XOFF_OUT|");
	if (self->settings.flow_control & IRCOMM_RTS_CTS_IN)
		ret += sprintf(buf+ret, "RTS_CTS_IN|");
	if (self->settings.flow_control & IRCOMM_RTS_CTS_OUT)
		ret += sprintf(buf+ret, "RTS_CTS_OUT|");
	if (self->settings.flow_control & IRCOMM_DSR_DTR_IN)
		ret += sprintf(buf+ret, "DSR_DTR_IN|");
	if (self->settings.flow_control & IRCOMM_DSR_DTR_OUT)
		ret += sprintf(buf+ret, "DSR_DTR_OUT|");
	if (self->settings.flow_control & IRCOMM_ENQ_ACK_IN)
		ret += sprintf(buf+ret, "ENQ_ACK_IN|");
	if (self->settings.flow_control & IRCOMM_ENQ_ACK_OUT)
		ret += sprintf(buf+ret, "ENQ_ACK_OUT|");
	if (self->settings.flow_control)
		ret--; /* remove the last | */
        ret += sprintf(buf+ret, "\n");

	ret += sprintf(buf+ret, "Flags: ");
	if (self->flags & ASYNC_CTS_FLOW)
		ret += sprintf(buf+ret, "ASYNC_CTS_FLOW|");
	if (self->flags & ASYNC_CHECK_CD)
		ret += sprintf(buf+ret, "ASYNC_CHECK_CD|");
	if (self->flags & ASYNC_INITIALIZED)
		ret += sprintf(buf+ret, "ASYNC_INITIALIZED|");
	if (self->flags & ASYNC_LOW_LATENCY)
		ret += sprintf(buf+ret, "ASYNC_LOW_LATENCY|");
	if (self->flags & ASYNC_CLOSING)
		ret += sprintf(buf+ret, "ASYNC_CLOSING|");
	if (self->flags & ASYNC_NORMAL_ACTIVE)
		ret += sprintf(buf+ret, "ASYNC_NORMAL_ACTIVE|");
	if (self->flags & ASYNC_CALLOUT_ACTIVE)
		ret += sprintf(buf+ret, "ASYNC_CALLOUT_ACTIVE|");
	if (self->flags)
		ret--; /* remove the last | */
	ret += sprintf(buf+ret, "\n");

	ret += sprintf(buf+ret, "Role: %s\n", self->client ? 
		       "client" : "server");
	ret += sprintf(buf+ret, "Open count: %d\n", self->open_count);
	ret += sprintf(buf+ret, "Max data size: %d\n", self->max_data_size);
	ret += sprintf(buf+ret, "Max header size: %d\n", self->max_header_size);
		
	if (self->tty)
		ret += sprintf(buf+ret, "Hardware: %s\n", 
			       self->tty->hw_stopped ? "Stopped" : "Running");

        ret += sprintf(buf+ret, "\n");
        return ret;
}


/*
 * Function ircomm_tty_read_proc (buf, start, offset, len, eof, unused)
 *
 *    
 *
 */
#ifdef CONFIG_PROC_FS
static int ircomm_tty_read_proc(char *buf, char **start, off_t offset, int len,
				int *eof, void *unused)
{
	struct ircomm_tty_cb *self;
        int count = 0, l;
        off_t begin = 0;

	self = (struct ircomm_tty_cb *) hashbin_get_first(ircomm_tty);
	while ((self != NULL) && (count < 4000)) {
		if (self->magic != IRCOMM_TTY_MAGIC)
			return 0;

                l = ircomm_tty_line_info(self, buf + count);
                count += l;
                if (count+begin > offset+len)
                        goto done;
                if (count+begin < offset) {
                        begin += count;
                        count = 0;
                }
				
		self = (struct ircomm_tty_cb *) hashbin_get_next(ircomm_tty);
        }
        *eof = 1;
done:
        if (offset >= count+begin)
                return 0;
        *start = buf + (offset-begin);
        return ((len < begin+count-offset) ? len : begin+count-offset);
}
#endif /* CONFIG_PROC_FS */

#ifdef MODULE
MODULE_AUTHOR("Dag Brattli <dagb@cs.uit.no>");
MODULE_DESCRIPTION("IrCOMM serial TTY driver");

int init_module(void) 
{
	return ircomm_tty_init();
}

void cleanup_module(void)
{
	ircomm_tty_cleanup();
}

#endif /* MODULE */




