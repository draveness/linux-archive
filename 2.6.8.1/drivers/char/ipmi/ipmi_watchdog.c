/*
 * ipmi_watchdog.c
 *
 * A watchdog timer based upon the IPMI interface.
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

#include <linux/config.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/ipmi.h>
#include <linux/ipmi_smi.h>
#include <linux/watchdog.h>
#include <linux/miscdevice.h>
#include <linux/init.h>
#include <linux/rwsem.h>
#include <linux/errno.h>
#include <asm/uaccess.h>
#include <linux/notifier.h>
#include <linux/nmi.h>
#include <linux/reboot.h>
#include <linux/wait.h>
#include <linux/poll.h>
#ifdef CONFIG_X86_LOCAL_APIC
#include <asm/apic.h>
#endif

#define IPMI_WATCHDOG_VERSION "v32"

/*
 * The IPMI command/response information for the watchdog timer.
 */

/* values for byte 1 of the set command, byte 2 of the get response. */
#define WDOG_DONT_LOG		(1 << 7)
#define WDOG_DONT_STOP_ON_SET	(1 << 6)
#define WDOG_SET_TIMER_USE(byte, use) \
	byte = ((byte) & 0xf8) | ((use) & 0x7)
#define WDOG_GET_TIMER_USE(byte) ((byte) & 0x7)
#define WDOG_TIMER_USE_BIOS_FRB2	1
#define WDOG_TIMER_USE_BIOS_POST	2
#define WDOG_TIMER_USE_OS_LOAD		3
#define WDOG_TIMER_USE_SMS_OS		4
#define WDOG_TIMER_USE_OEM		5

/* values for byte 2 of the set command, byte 3 of the get response. */
#define WDOG_SET_PRETIMEOUT_ACT(byte, use) \
	byte = ((byte) & 0x8f) | (((use) & 0x7) << 4)
#define WDOG_GET_PRETIMEOUT_ACT(byte) (((byte) >> 4) & 0x7)
#define WDOG_PRETIMEOUT_NONE		0
#define WDOG_PRETIMEOUT_SMI		1
#define WDOG_PRETIMEOUT_NMI		2
#define WDOG_PRETIMEOUT_MSG_INT		3

/* Operations that can be performed on a pretimout. */
#define WDOG_PREOP_NONE		0
#define WDOG_PREOP_PANIC	1
#define WDOG_PREOP_GIVE_DATA	2 /* Cause data to be available to
                                     read.  Doesn't work in NMI
                                     mode. */

/* Actions to perform on a full timeout. */
#define WDOG_SET_TIMEOUT_ACT(byte, use) \
	byte = ((byte) & 0xf8) | ((use) & 0x7)
#define WDOG_GET_TIMEOUT_ACT(byte) ((byte) & 0x7)
#define WDOG_TIMEOUT_NONE		0
#define WDOG_TIMEOUT_RESET		1
#define WDOG_TIMEOUT_POWER_DOWN		2
#define WDOG_TIMEOUT_POWER_CYCLE	3

/* Byte 3 of the get command, byte 4 of the get response is the
   pre-timeout in seconds. */

/* Bits for setting byte 4 of the set command, byte 5 of the get response. */
#define WDOG_EXPIRE_CLEAR_BIOS_FRB2	(1 << 1)
#define WDOG_EXPIRE_CLEAR_BIOS_POST	(1 << 2)
#define WDOG_EXPIRE_CLEAR_OS_LOAD	(1 << 3)
#define WDOG_EXPIRE_CLEAR_SMS_OS	(1 << 4)
#define WDOG_EXPIRE_CLEAR_OEM		(1 << 5)

/* Setting/getting the watchdog timer value.  This is for bytes 5 and
   6 (the timeout time) of the set command, and bytes 6 and 7 (the
   timeout time) and 8 and 9 (the current countdown value) of the
   response.  The timeout value is given in seconds (in the command it
   is 100ms intervals). */
#define WDOG_SET_TIMEOUT(byte1, byte2, val) \
	(byte1) = (((val) * 10) & 0xff), (byte2) = (((val) * 10) >> 8)
#define WDOG_GET_TIMEOUT(byte1, byte2) \
	(((byte1) | ((byte2) << 8)) / 10)

#define IPMI_WDOG_RESET_TIMER		0x22
#define IPMI_WDOG_SET_TIMER		0x24
#define IPMI_WDOG_GET_TIMER		0x25

/* These are here until the real ones get into the watchdog.h interface. */
#ifndef WDIOC_GETTIMEOUT
#define	WDIOC_GETTIMEOUT        _IOW(WATCHDOG_IOCTL_BASE, 20, int)
#endif
#ifndef WDIOC_SET_PRETIMEOUT
#define	WDIOC_SET_PRETIMEOUT     _IOW(WATCHDOG_IOCTL_BASE, 21, int)
#endif
#ifndef WDIOC_GET_PRETIMEOUT
#define	WDIOC_GET_PRETIMEOUT     _IOW(WATCHDOG_IOCTL_BASE, 22, int)
#endif

#ifdef CONFIG_WATCHDOG_NOWAYOUT
static int nowayout = 1;
#else
static int nowayout;
#endif

static ipmi_user_t watchdog_user = NULL;

/* Default the timeout to 10 seconds. */
static int timeout = 10;

/* The pre-timeout is disabled by default. */
static int pretimeout = 0;

/* Default action is to reset the board on a timeout. */
static unsigned char action_val = WDOG_TIMEOUT_RESET;

static char action[16] = "reset";

static unsigned char preaction_val = WDOG_PRETIMEOUT_NONE;

static char preaction[16] = "pre_none";

static unsigned char preop_val = WDOG_PREOP_NONE;

static char preop[16] = "preop_none";
static spinlock_t ipmi_read_lock = SPIN_LOCK_UNLOCKED;
static char data_to_read = 0;
static DECLARE_WAIT_QUEUE_HEAD(read_q);
static struct fasync_struct *fasync_q = NULL;
static char pretimeout_since_last_heartbeat = 0;

/* If true, the driver will start running as soon as it is configured
   and ready. */
static int start_now = 0;

module_param(timeout, int, 0);
MODULE_PARM_DESC(timeout, "Timeout value in seconds.");
module_param(pretimeout, int, 0);
MODULE_PARM_DESC(pretimeout, "Pretimeout value in seconds.");
module_param_string(action, action, sizeof(action), 0);
MODULE_PARM_DESC(action, "Timeout action. One of: "
		 "reset, none, power_cycle, power_off.");
module_param_string(preaction, preaction, sizeof(preaction), 0);
MODULE_PARM_DESC(preaction, "Pretimeout action.  One of: "
		 "pre_none, pre_smi, pre_nmi, pre_int.");
module_param_string(preop, preop, sizeof(preop), 0);
MODULE_PARM_DESC(preop, "Pretimeout driver operation.  One of: "
		 "preop_none, preop_panic, preop_give_data.");
module_param(start_now, int, 0);
MODULE_PARM_DESC(start_now, "Set to 1 to start the watchdog as"
		 "soon as the driver is loaded.");
module_param(nowayout, int, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default=CONFIG_WATCHDOG_NOWAYOUT)");

/* Default state of the timer. */
static unsigned char ipmi_watchdog_state = WDOG_TIMEOUT_NONE;

/* If shutting down via IPMI, we ignore the heartbeat. */
static int ipmi_ignore_heartbeat = 0;

/* Is someone using the watchdog?  Only one user is allowed. */
static int ipmi_wdog_open = 0;

/* If set to 1, the heartbeat command will set the state to reset and
   start the timer.  The timer doesn't normally run when the driver is
   first opened until the heartbeat is set the first time, this
   variable is used to accomplish this. */
static int ipmi_start_timer_on_heartbeat = 0;

/* IPMI version of the BMC. */
static unsigned char ipmi_version_major;
static unsigned char ipmi_version_minor;


static int ipmi_heartbeat(void);
static void panic_halt_ipmi_heartbeat(void);


/* We use a semaphore to make sure that only one thing can send a set
   timeout at one time, because we only have one copy of the data.
   The semaphore is claimed when the set_timeout is sent and freed
   when both messages are free. */
static atomic_t set_timeout_tofree = ATOMIC_INIT(0);
static DECLARE_MUTEX(set_timeout_lock);
static void set_timeout_free_smi(struct ipmi_smi_msg *msg)
{
    if (atomic_dec_and_test(&set_timeout_tofree))
	    up(&set_timeout_lock);
}
static void set_timeout_free_recv(struct ipmi_recv_msg *msg)
{
    if (atomic_dec_and_test(&set_timeout_tofree))
	    up(&set_timeout_lock);
}
static struct ipmi_smi_msg set_timeout_smi_msg =
{
	.done = set_timeout_free_smi
};
static struct ipmi_recv_msg set_timeout_recv_msg =
{
	.done = set_timeout_free_recv
};
 
static int i_ipmi_set_timeout(struct ipmi_smi_msg  *smi_msg,
			      struct ipmi_recv_msg *recv_msg,
			      int                  *send_heartbeat_now)
{
	struct kernel_ipmi_msg            msg;
	unsigned char                     data[6];
	int                               rv;
	struct ipmi_system_interface_addr addr;
	int                               hbnow = 0;


	data[0] = 0;
	WDOG_SET_TIMER_USE(data[0], WDOG_TIMER_USE_SMS_OS);

	if ((ipmi_version_major > 1)
	    || ((ipmi_version_major == 1) && (ipmi_version_minor >= 5)))
	{
		/* This is an IPMI 1.5-only feature. */
		data[0] |= WDOG_DONT_STOP_ON_SET;
	} else if (ipmi_watchdog_state != WDOG_TIMEOUT_NONE) {
		/* In ipmi 1.0, setting the timer stops the watchdog, we
		   need to start it back up again. */
		hbnow = 1;
	}

	data[1] = 0;
	WDOG_SET_TIMEOUT_ACT(data[1], ipmi_watchdog_state);
	if (pretimeout > 0) {
	    WDOG_SET_PRETIMEOUT_ACT(data[1], preaction_val);
	    data[2] = pretimeout;
	} else {
	    WDOG_SET_PRETIMEOUT_ACT(data[1], WDOG_PRETIMEOUT_NONE);
	    data[2] = 0; /* No pretimeout. */
	}
	data[3] = 0;
	WDOG_SET_TIMEOUT(data[4], data[5], timeout);

	addr.addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
	addr.channel = IPMI_BMC_CHANNEL;
	addr.lun = 0;

	msg.netfn = 0x06;
	msg.cmd = IPMI_WDOG_SET_TIMER;
	msg.data = data;
	msg.data_len = sizeof(data);
	rv = ipmi_request_supply_msgs(watchdog_user,
				      (struct ipmi_addr *) &addr,
				      0,
				      &msg,
				      NULL,
				      smi_msg,
				      recv_msg,
				      1);
	if (rv) {
		printk(KERN_WARNING "IPMI Watchdog, set timeout error: %d\n",
		       rv);
	}

	if (send_heartbeat_now)
	    *send_heartbeat_now = hbnow;

	return rv;
}

/* Parameters to ipmi_set_timeout */
#define IPMI_SET_TIMEOUT_NO_HB			0
#define IPMI_SET_TIMEOUT_HB_IF_NECESSARY	1
#define IPMI_SET_TIMEOUT_FORCE_HB		2

static int ipmi_set_timeout(int do_heartbeat)
{
	int send_heartbeat_now;
	int rv;


	/* We can only send one of these at a time. */
	down(&set_timeout_lock);

	atomic_set(&set_timeout_tofree, 2);

	rv = i_ipmi_set_timeout(&set_timeout_smi_msg,
				&set_timeout_recv_msg,
				&send_heartbeat_now);
	if (rv) {
		up(&set_timeout_lock);
	} else {
		if ((do_heartbeat == IPMI_SET_TIMEOUT_FORCE_HB)
		    || ((send_heartbeat_now)
			&& (do_heartbeat == IPMI_SET_TIMEOUT_HB_IF_NECESSARY)))
		{
			rv = ipmi_heartbeat();
		}
	}

	return rv;
}

static void dummy_smi_free(struct ipmi_smi_msg *msg)
{
}
static void dummy_recv_free(struct ipmi_recv_msg *msg)
{
}
static struct ipmi_smi_msg panic_halt_smi_msg =
{
	.done = dummy_smi_free
};
static struct ipmi_recv_msg panic_halt_recv_msg =
{
	.done = dummy_recv_free
};

/* Special call, doesn't claim any locks.  This is only to be called
   at panic or halt time, in run-to-completion mode, when the caller
   is the only CPU and the only thing that will be going is these IPMI
   calls. */
static void panic_halt_ipmi_set_timeout(void)
{
	int send_heartbeat_now;
	int rv;

	rv = i_ipmi_set_timeout(&panic_halt_smi_msg,
				&panic_halt_recv_msg,
				&send_heartbeat_now);
	if (!rv) {
		if (send_heartbeat_now)
			panic_halt_ipmi_heartbeat();
	}
}

/* Do a delayed shutdown, with the delay in milliseconds.  If power_off is
   false, do a reset.  If power_off is true, do a power down.  This is
   primarily for the IMB code's shutdown. */
void ipmi_delayed_shutdown(long delay, int power_off)
{
	ipmi_ignore_heartbeat = 1;
	if (power_off) 
		ipmi_watchdog_state = WDOG_TIMEOUT_POWER_DOWN;
	else
		ipmi_watchdog_state = WDOG_TIMEOUT_RESET;
	timeout = delay;
	ipmi_set_timeout(IPMI_SET_TIMEOUT_HB_IF_NECESSARY);
}

/* We use a semaphore to make sure that only one thing can send a
   heartbeat at one time, because we only have one copy of the data.
   The semaphore is claimed when the set_timeout is sent and freed
   when both messages are free. */
static atomic_t heartbeat_tofree = ATOMIC_INIT(0);
static DECLARE_MUTEX(heartbeat_lock);
static DECLARE_MUTEX_LOCKED(heartbeat_wait_lock);
static void heartbeat_free_smi(struct ipmi_smi_msg *msg)
{
    if (atomic_dec_and_test(&heartbeat_tofree))
	    up(&heartbeat_wait_lock);
}
static void heartbeat_free_recv(struct ipmi_recv_msg *msg)
{
    if (atomic_dec_and_test(&heartbeat_tofree))
	    up(&heartbeat_wait_lock);
}
static struct ipmi_smi_msg heartbeat_smi_msg =
{
	.done = heartbeat_free_smi
};
static struct ipmi_recv_msg heartbeat_recv_msg =
{
	.done = heartbeat_free_recv
};
 
static struct ipmi_smi_msg panic_halt_heartbeat_smi_msg =
{
	.done = dummy_smi_free
};
static struct ipmi_recv_msg panic_halt_heartbeat_recv_msg =
{
	.done = dummy_recv_free
};
 
static int ipmi_heartbeat(void)
{
	struct kernel_ipmi_msg            msg;
	int                               rv;
	struct ipmi_system_interface_addr addr;

	if (ipmi_ignore_heartbeat) {
		return 0;
	}

	if (ipmi_start_timer_on_heartbeat) {
		ipmi_start_timer_on_heartbeat = 0;
		ipmi_watchdog_state = action_val;
		return ipmi_set_timeout(IPMI_SET_TIMEOUT_FORCE_HB);
	} else if (pretimeout_since_last_heartbeat) {
		/* A pretimeout occurred, make sure we set the timeout.
		   We don't want to set the action, though, we want to
		   leave that alone (thus it can't be combined with the
		   above operation. */
		pretimeout_since_last_heartbeat = 0;
		return ipmi_set_timeout(IPMI_SET_TIMEOUT_HB_IF_NECESSARY);
	}

	down(&heartbeat_lock);

	atomic_set(&heartbeat_tofree, 2);

	/* Don't reset the timer if we have the timer turned off, that
           re-enables the watchdog. */
	if (ipmi_watchdog_state == WDOG_TIMEOUT_NONE) {
		up(&heartbeat_lock);
		return 0;
	}

	addr.addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
	addr.channel = IPMI_BMC_CHANNEL;
	addr.lun = 0;

	msg.netfn = 0x06;
	msg.cmd = IPMI_WDOG_RESET_TIMER;
	msg.data = NULL;
	msg.data_len = 0;
	rv = ipmi_request_supply_msgs(watchdog_user,
				      (struct ipmi_addr *) &addr,
				      0,
				      &msg,
				      NULL,
				      &heartbeat_smi_msg,
				      &heartbeat_recv_msg,
				      1);
	if (rv) {
		up(&heartbeat_lock);
		printk(KERN_WARNING "IPMI Watchdog, heartbeat failure: %d\n",
		       rv);
		return rv;
	}

	/* Wait for the heartbeat to be sent. */
	down(&heartbeat_wait_lock);

	if (heartbeat_recv_msg.msg.data[0] != 0) {
	    /* Got an error in the heartbeat response.  It was already
	       reported in ipmi_wdog_msg_handler, but we should return
	       an error here. */
	    rv = -EINVAL;
	}

	up(&heartbeat_lock);

	return rv;
}

static void panic_halt_ipmi_heartbeat(void)
{
	struct kernel_ipmi_msg             msg;
	struct ipmi_system_interface_addr addr;


	/* Don't reset the timer if we have the timer turned off, that
           re-enables the watchdog. */
	if (ipmi_watchdog_state == WDOG_TIMEOUT_NONE)
		return;

	addr.addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
	addr.channel = IPMI_BMC_CHANNEL;
	addr.lun = 0;

	msg.netfn = 0x06;
	msg.cmd = IPMI_WDOG_RESET_TIMER;
	msg.data = NULL;
	msg.data_len = 0;
	ipmi_request_supply_msgs(watchdog_user,
				 (struct ipmi_addr *) &addr,
				 0,
				 &msg,
				 NULL,
				 &panic_halt_heartbeat_smi_msg,
				 &panic_halt_heartbeat_recv_msg,
				 1);
}

static struct watchdog_info ident=
{
	0, /* WDIOF_SETTIMEOUT, */
	1,
	"IPMI"
};

static int ipmi_ioctl(struct inode *inode, struct file *file,
		      unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int i;
	int val;

	switch(cmd) {
	case WDIOC_GETSUPPORT:
		i = copy_to_user(argp, &ident, sizeof(ident));
		return i ? -EFAULT : 0;

	case WDIOC_SETTIMEOUT:
		i = copy_from_user(&val, argp, sizeof(int));
		if (i)
			return -EFAULT;
		timeout = val;
		return ipmi_set_timeout(IPMI_SET_TIMEOUT_HB_IF_NECESSARY);

	case WDIOC_GETTIMEOUT:
		i = copy_to_user(argp, &timeout, sizeof(timeout));
		if (i)
			return -EFAULT;
		return 0;

	case WDIOC_SET_PRETIMEOUT:
		i = copy_from_user(&val, argp, sizeof(int));
		if (i)
			return -EFAULT;
		pretimeout = val;
		return ipmi_set_timeout(IPMI_SET_TIMEOUT_HB_IF_NECESSARY);

	case WDIOC_GET_PRETIMEOUT:
		i = copy_to_user(argp, &pretimeout, sizeof(pretimeout));
		if (i)
			return -EFAULT;
		return 0;

	case WDIOC_KEEPALIVE:
		return ipmi_heartbeat();

	case WDIOC_SETOPTIONS:
		i = copy_from_user(&val, argp, sizeof(int));
		if (i)
			return -EFAULT;
		if (val & WDIOS_DISABLECARD)
		{
			ipmi_watchdog_state = WDOG_TIMEOUT_NONE;
			ipmi_set_timeout(IPMI_SET_TIMEOUT_NO_HB);
			ipmi_start_timer_on_heartbeat = 0;
		}

		if (val & WDIOS_ENABLECARD)
		{
			ipmi_watchdog_state = action_val;
			ipmi_set_timeout(IPMI_SET_TIMEOUT_FORCE_HB);
		}
		return 0;

	case WDIOC_GETSTATUS:
		val = 0;
		i = copy_to_user(argp, &val, sizeof(val));
		if (i)
			return -EFAULT;
		return 0;

	default:
		return -ENOIOCTLCMD;
	}
}

static ssize_t ipmi_write(struct file *file,
			  const char  __user *buf,
			  size_t      len,
			  loff_t      *ppos)
{
	int rv;

	if (len) {
		rv = ipmi_heartbeat();
		if (rv)
			return rv;
		return 1;
	}
	return 0;
}

static ssize_t ipmi_read(struct file *file,
			 char        __user *buf,
			 size_t      count,
			 loff_t      *ppos)
{
	int          rv = 0;
	wait_queue_t wait;

	if (count <= 0)
		return 0;

	/* Reading returns if the pretimeout has gone off, and it only does
	   it once per pretimeout. */
	spin_lock(&ipmi_read_lock);
	if (!data_to_read) {
		if (file->f_flags & O_NONBLOCK) {
			rv = -EAGAIN;
			goto out;
		}
		
		init_waitqueue_entry(&wait, current);
		add_wait_queue(&read_q, &wait);
		while (!data_to_read) {
			set_current_state(TASK_INTERRUPTIBLE);
			spin_unlock(&ipmi_read_lock);
			schedule();
			spin_lock(&ipmi_read_lock);
		}
		remove_wait_queue(&read_q, &wait);
	    
		if (signal_pending(current)) {
			rv = -ERESTARTSYS;
			goto out;
		}
	}
	data_to_read = 0;

 out:
	spin_unlock(&ipmi_read_lock);

	if (rv == 0) {
		if (copy_to_user(buf, &data_to_read, 1))
			rv = -EFAULT;
		else
			rv = 1;
	}

	return rv;
}

static int ipmi_open(struct inode *ino, struct file *filep)
{
        switch (iminor(ino))
        {
                case WATCHDOG_MINOR:
                    if (ipmi_wdog_open)
                        return -EBUSY;

                    ipmi_wdog_open = 1;

		    /* Don't start the timer now, let it start on the
		       first heartbeat. */
		    ipmi_start_timer_on_heartbeat = 1;
                    return nonseekable_open(ino, filep);

                default:
                    return (-ENODEV);
        }
}

static unsigned int ipmi_poll(struct file *file, poll_table *wait)
{
	unsigned int mask = 0;
	
	poll_wait(file, &read_q, wait);

	spin_lock(&ipmi_read_lock);
	if (data_to_read)
		mask |= (POLLIN | POLLRDNORM);
	spin_unlock(&ipmi_read_lock);

	return mask;
}

static int ipmi_fasync(int fd, struct file *file, int on)
{
	int result;

	result = fasync_helper(fd, file, on, &fasync_q);

	return (result);
}

static int ipmi_close(struct inode *ino, struct file *filep)
{
	if (iminor(ino)==WATCHDOG_MINOR)
	{
		if (!nowayout) {
			ipmi_watchdog_state = WDOG_TIMEOUT_NONE;
			ipmi_set_timeout(IPMI_SET_TIMEOUT_NO_HB);
		}
	        ipmi_wdog_open = 0;
	}

	ipmi_fasync (-1, filep, 0);

	return 0;
}

static struct file_operations ipmi_wdog_fops = {
	.owner   = THIS_MODULE,
	.read    = ipmi_read,
	.poll    = ipmi_poll,
	.write   = ipmi_write,
	.ioctl   = ipmi_ioctl,
	.open    = ipmi_open,
	.release = ipmi_close,
	.fasync  = ipmi_fasync,
};

static struct miscdevice ipmi_wdog_miscdev = {
	WATCHDOG_MINOR,
	"watchdog",
	&ipmi_wdog_fops
};

static DECLARE_RWSEM(register_sem);

static void ipmi_wdog_msg_handler(struct ipmi_recv_msg *msg,
				  void                 *handler_data)
{
	if (msg->msg.data[0] != 0) {
		printk(KERN_ERR "IPMI Watchdog response: Error %x on cmd %x\n",
		       msg->msg.data[0],
		       msg->msg.cmd);
	}
	
	ipmi_free_recv_msg(msg);
}

static void ipmi_wdog_pretimeout_handler(void *handler_data)
{
	if (preaction_val != WDOG_PRETIMEOUT_NONE) {
		if (preop_val == WDOG_PREOP_PANIC)
			panic("Watchdog pre-timeout");
		else if (preop_val == WDOG_PREOP_GIVE_DATA) {
			spin_lock(&ipmi_read_lock);
			data_to_read = 1;
			wake_up_interruptible(&read_q);
			kill_fasync(&fasync_q, SIGIO, POLL_IN);

			spin_unlock(&ipmi_read_lock);
		}
	}

	/* On some machines, the heartbeat will give
	   an error and not work unless we re-enable
	   the timer.   So do so. */
	pretimeout_since_last_heartbeat = 1;
}

static struct ipmi_user_hndl ipmi_hndlrs =
{
	.ipmi_recv_hndl           = ipmi_wdog_msg_handler,
	.ipmi_watchdog_pretimeout = ipmi_wdog_pretimeout_handler
};

static void ipmi_register_watchdog(int ipmi_intf)
{
	int rv = -EBUSY;

	down_write(&register_sem);
	if (watchdog_user)
		goto out;

	rv = ipmi_create_user(ipmi_intf, &ipmi_hndlrs, NULL, &watchdog_user);
	if (rv < 0) {
		printk("IPMI watchdog: Unable to register with ipmi\n");
		goto out;
	}

	ipmi_get_version(watchdog_user,
			 &ipmi_version_major,
			 &ipmi_version_minor);

	rv = misc_register(&ipmi_wdog_miscdev);
	if (rv < 0) {
		ipmi_destroy_user(watchdog_user);
		watchdog_user = NULL;
		printk("IPMI watchdog: Unable to register misc device\n");
	}

 out:
	up_write(&register_sem);

	if ((start_now) && (rv == 0)) {
		/* Run from startup, so start the timer now. */
		start_now = 0; /* Disable this function after first startup. */
		ipmi_watchdog_state = action_val;
		ipmi_set_timeout(IPMI_SET_TIMEOUT_FORCE_HB);
		printk("Starting IPMI Watchdog now!\n");
	}
}

#ifdef HAVE_NMI_HANDLER
static int
ipmi_nmi(void *dev_id, struct pt_regs *regs, int cpu, int handled)
{
	/* If no one else handled the NMI, we assume it was the IPMI
           watchdog. */
	if ((!handled) && (preop_val == WDOG_PREOP_PANIC))
		panic("IPMI watchdog pre-timeout");

	/* On some machines, the heartbeat will give
	   an error and not work unless we re-enable
	   the timer.   So do so. */
	pretimeout_since_last_heartbeat = 1;

	return NOTIFY_DONE;
}

static struct nmi_handler ipmi_nmi_handler =
{
	.link     = LIST_HEAD_INIT(ipmi_nmi_handler.link),
	.dev_name = "ipmi_watchdog",
	.dev_id   = NULL,
	.handler  = ipmi_nmi,
	.priority = 0, /* Call us last. */
};
#endif

static int wdog_reboot_handler(struct notifier_block *this,
			       unsigned long         code,
			       void                  *unused)
{
	static int reboot_event_handled = 0;

	if ((watchdog_user) && (!reboot_event_handled)) {
		/* Make sure we only do this once. */
		reboot_event_handled = 1;

		if (code == SYS_DOWN || code == SYS_HALT) {
			/* Disable the WDT if we are shutting down. */
			ipmi_watchdog_state = WDOG_TIMEOUT_NONE;
			panic_halt_ipmi_set_timeout();
		} else {
			/* Set a long timer to let the reboot happens, but
			   reboot if it hangs. */
			timeout = 120;
			pretimeout = 0;
			ipmi_watchdog_state = WDOG_TIMEOUT_RESET;
			panic_halt_ipmi_set_timeout();
		}
	}
	return NOTIFY_OK;
}

static struct notifier_block wdog_reboot_notifier = {
	wdog_reboot_handler,
	NULL,
	0
};

extern int panic_timeout; /* Why isn't this defined anywhere? */

static int wdog_panic_handler(struct notifier_block *this,
			      unsigned long         event,
			      void                  *unused)
{
	static int panic_event_handled = 0;

	/* On a panic, if we have a panic timeout, make sure that the thing
	   reboots, even if it hangs during that panic. */
	if (watchdog_user && !panic_event_handled) {
		/* Make sure the panic doesn't hang, and make sure we
		   do this only once. */
		panic_event_handled = 1;
	    
		timeout = 255;
		pretimeout = 0;
		ipmi_watchdog_state = WDOG_TIMEOUT_RESET;
		panic_halt_ipmi_set_timeout();
	}

	return NOTIFY_OK;
}

static struct notifier_block wdog_panic_notifier = {
	wdog_panic_handler,
	NULL,
	150   /* priority: INT_MAX >= x >= 0 */
};


static void ipmi_new_smi(int if_num)
{
	ipmi_register_watchdog(if_num);
}

static void ipmi_smi_gone(int if_num)
{
	/* This can never be called, because once the watchdog is
	   registered, the interface can't go away until the watchdog
	   is unregistered. */
}

static struct ipmi_smi_watcher smi_watcher =
{
	.owner    = THIS_MODULE,
	.new_smi  = ipmi_new_smi,
	.smi_gone = ipmi_smi_gone
};

static int __init ipmi_wdog_init(void)
{
	int rv;

	printk(KERN_INFO "IPMI watchdog driver version "
	       IPMI_WATCHDOG_VERSION "\n");

	if (strcmp(action, "reset") == 0) {
		action_val = WDOG_TIMEOUT_RESET;
	} else if (strcmp(action, "none") == 0) {
		action_val = WDOG_TIMEOUT_NONE;
	} else if (strcmp(action, "power_cycle") == 0) {
		action_val = WDOG_TIMEOUT_POWER_CYCLE;
	} else if (strcmp(action, "power_off") == 0) {
		action_val = WDOG_TIMEOUT_POWER_DOWN;
	} else {
		action_val = WDOG_TIMEOUT_RESET;
		printk("ipmi_watchdog: Unknown action '%s', defaulting to"
		       " reset\n", action);
	}

	if (strcmp(preaction, "pre_none") == 0) {
		preaction_val = WDOG_PRETIMEOUT_NONE;
	} else if (strcmp(preaction, "pre_smi") == 0) {
		preaction_val = WDOG_PRETIMEOUT_SMI;
#ifdef HAVE_NMI_HANDLER
	} else if (strcmp(preaction, "pre_nmi") == 0) {
		preaction_val = WDOG_PRETIMEOUT_NMI;
#endif
	} else if (strcmp(preaction, "pre_int") == 0) {
		preaction_val = WDOG_PRETIMEOUT_MSG_INT;
	} else {
		preaction_val = WDOG_PRETIMEOUT_NONE;
		printk("ipmi_watchdog: Unknown preaction '%s', defaulting to"
		       " none\n", preaction);
	}

	if (strcmp(preop, "preop_none") == 0) {
		preop_val = WDOG_PREOP_NONE;
	} else if (strcmp(preop, "preop_panic") == 0) {
		preop_val = WDOG_PREOP_PANIC;
	} else if (strcmp(preop, "preop_give_data") == 0) {
		preop_val = WDOG_PREOP_GIVE_DATA;
	} else {
		preop_val = WDOG_PREOP_NONE;
		printk("ipmi_watchdog: Unknown preop '%s', defaulting to"
		       " none\n", preop);
	}

#ifdef HAVE_NMI_HANDLER
	if (preaction_val == WDOG_PRETIMEOUT_NMI) {
		if (preop_val == WDOG_PREOP_GIVE_DATA) {
			printk(KERN_WARNING
			       "ipmi_watchdog: Pretimeout op is to give data"
			       " but NMI pretimeout is enabled, setting"
			       " pretimeout op to none\n");
			preop_val = WDOG_PREOP_NONE;
		}
#ifdef CONFIG_X86_LOCAL_APIC
		if (nmi_watchdog == NMI_IO_APIC) {
			printk(KERN_WARNING
			       "ipmi_watchdog: nmi_watchdog is set to IO APIC"
			       " mode (value is %d), that is incompatible"
			       " with using NMI in the IPMI watchdog."
			       " Disabling IPMI nmi pretimeout.\n",
			       nmi_watchdog);
			preaction_val = WDOG_PRETIMEOUT_NONE;
		} else {
#endif
		rv = request_nmi(&ipmi_nmi_handler);
		if (rv) {
			printk(KERN_WARNING
			       "ipmi_watchdog: Can't register nmi handler\n");
			return rv;
		}
#ifdef CONFIG_X86_LOCAL_APIC
		}
#endif
	}
#endif

	rv = ipmi_smi_watcher_register(&smi_watcher);
	if (rv) {
#ifdef HAVE_NMI_HANDLER
		if (preaction_val == WDOG_PRETIMEOUT_NMI)
			release_nmi(&ipmi_nmi_handler);
#endif
		printk(KERN_WARNING
		       "ipmi_watchdog: can't register smi watcher\n");
		return rv;
	}

	register_reboot_notifier(&wdog_reboot_notifier);
	notifier_chain_register(&panic_notifier_list, &wdog_panic_notifier);

	return 0;
}

static __exit void ipmi_unregister_watchdog(void)
{
	int rv;

	down_write(&register_sem);

#ifdef HAVE_NMI_HANDLER
	if (preaction_val == WDOG_PRETIMEOUT_NMI)
		release_nmi(&ipmi_nmi_handler);
#endif

	notifier_chain_unregister(&panic_notifier_list, &wdog_panic_notifier);
	unregister_reboot_notifier(&wdog_reboot_notifier);

	if (! watchdog_user)
		goto out;

	/* Make sure no one can call us any more. */
	misc_deregister(&ipmi_wdog_miscdev);

	/*  Disable the timer. */
	ipmi_watchdog_state = WDOG_TIMEOUT_NONE;
	ipmi_set_timeout(IPMI_SET_TIMEOUT_NO_HB);

	/* Wait to make sure the message makes it out.  The lower layer has
	   pointers to our buffers, we want to make sure they are done before
	   we release our memory. */
	while (atomic_read(&set_timeout_tofree)) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(1);
	}

	/* Disconnect from IPMI. */
	rv = ipmi_destroy_user(watchdog_user);
	if (rv) {
		printk(KERN_WARNING
		       "IPMI Watchdog, error unlinking from IPMI: %d\n",
		       rv);
	}
	watchdog_user = NULL;

 out:
	up_write(&register_sem);
}

static void __exit ipmi_wdog_exit(void)
{
	ipmi_smi_watcher_unregister(&smi_watcher);
	ipmi_unregister_watchdog();
}
module_exit(ipmi_wdog_exit);

EXPORT_SYMBOL(ipmi_delayed_shutdown);

module_init(ipmi_wdog_init);
MODULE_LICENSE("GPL");
