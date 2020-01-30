/*
 *  drivers/s390/char/con3215.c
 *    3215 line mode terminal driver.
 *
 *  S390 version
 *    Copyright (C) 1999,2000 IBM Deutschland Entwicklung GmbH, IBM Corporation
 *    Author(s): Martin Schwidefsky (schwidefsky@de.ibm.com),
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/tty.h>
#include <linux/vt_kern.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/interrupt.h>

#include <linux/malloc.h>
#include <linux/bootmem.h>
#include <asm/io.h>
#include <asm/ebcdic.h>
#include <asm/uaccess.h>

#include "../../../arch/s390/kernel/cpcmd.h"
#include <asm/irq.h>

#define NR_3215		    1
#define NR_3215_REQ	    (4*NR_3215)
#define RAW3215_BUFFER_SIZE 65536     /* output buffer size */
#define RAW3215_INBUF_SIZE  256	      /* input buffer size */
#define RAW3215_MIN_SPACE   128	      /* minimum free space for wakeup */
#define RAW3215_MIN_WRITE   1024      /* min. length for immediate output */
#define RAW3215_MAX_CCWLEN  3968      /* max. bytes to write with one ccw */
#define RAW3215_NR_CCWS	    ((RAW3215_BUFFER_SIZE/RAW3215_MAX_CCWLEN)+2)
#define RAW3215_TIMEOUT	    HZ/10     /* time for delayed output */

#define RAW3215_FIXED	    1	      /* 3215 console device is not be freed */
#define RAW3215_ACTIVE	    2	      /* set if the device is in use */
#define RAW3215_WORKING	    4	      /* set if a request is being worked on */
#define RAW3215_THROTTLED   8	      /* set if reading is disabled */
#define RAW3215_STOPPED	    16	      /* set if writing is disabled */
#define RAW3215_CLOSING	    32	      /* set while in close process */
#define RAW3215_TIMER_RUNS  64	      /* set if the output delay timer is on */
#define RAW3215_FLUSHING    128	      /* set to flush buffer (no delay) */
#define RAW3215_BH_PENDING  256       /* indication for bh scheduling */

struct _raw3215_info;		      /* forward declaration ... */

int raw3215_condevice = -1;           /* preset console device */

/*
 * Request types for a 3215 device
 */
typedef enum {
	RAW3215_FREE, RAW3215_READ, RAW3215_WRITE
} raw3215_type;

/*
 * Request structure for a 3215 device
 */
typedef struct _raw3215_req {
	raw3215_type type;	      /* type of the request */
	int start, end;		      /* start/end index into output buffer */
	int residual;		      /* residual count for read request */
	ccw1_t ccws[RAW3215_NR_CCWS]; /* space for the channel program */
	struct _raw3215_info *info;   /* pointer to main structure */
	struct _raw3215_req *next;    /* pointer to next request */
} raw3215_req  __attribute__ ((aligned(8)));

typedef struct _raw3215_info {
	int flags;		      /* state flags */
	int irq;		      /* interrupt number to do_IO */
	char *buffer;		      /* pointer to output buffer */
	char *inbuf;		      /* pointer to input buffer */
	int head;		      /* first free byte in output buffer */
	int count;		      /* number of bytes in output buffer */
	devstat_t devstat;	      /* device status structure for do_IO */
	struct tty_struct *tty;	      /* pointer to tty structure if present */
	struct tq_struct tqueue;      /* task queue to bottom half */
	raw3215_req *queued_read;     /* pointer to queued read requests */
	raw3215_req *queued_write;    /* pointer to queued write requests */
	wait_queue_head_t empty_wait; /* wait queue for flushing */
	struct timer_list timer;      /* timer for delayed output */
	char *message;                /* pending message from raw3215_irq */
	int msg_dstat;                /* dstat for pending message */
	int msg_cstat;                /* cstat for pending message */
} raw3215_info;

static raw3215_info *raw3215[NR_3215];	/* array of 3215 devices structures */
static raw3215_req *raw3215_freelist;	/* list of free request structures */
static spinlock_t raw3215_freelist_lock;/* spinlock to protect free list */

static struct tty_driver tty3215_driver;
static struct tty_struct *tty3215_table[NR_3215];
static struct termios *tty3215_termios[NR_3215];
static struct termios *tty3215_termios_locked[NR_3215];
static int tty3215_refcount;

#ifndef MIN
#define MIN(a,b)	((a) < (b) ? (a) : (b))
#endif

static int __init con3215_setup(char *str)
{
        int vdev;

        vdev = simple_strtoul(str,&str,16);
        if (vdev >= 0 && vdev < 65536)
                raw3215_condevice = vdev;
        return 1;
}

__setup("condev=", con3215_setup);

/*
 * Get a request structure from the free list
 */
extern inline raw3215_req *raw3215_alloc_req(void) {
	raw3215_req *req;
	unsigned long flags;

	spin_lock_irqsave(&raw3215_freelist_lock, flags);
	req = raw3215_freelist;
	raw3215_freelist = req->next;
	spin_unlock_irqrestore(&raw3215_freelist_lock, flags);
	return req;
}

/*
 * Put a request structure back to the free list
 */
extern inline void raw3215_free_req(raw3215_req *req) {
	unsigned long flags;

        if (req->type == RAW3215_FREE)
                return;         /* don't free a free request */
        req->type = RAW3215_FREE;
	spin_lock_irqsave(&raw3215_freelist_lock, flags);
	req->next = raw3215_freelist;
	raw3215_freelist = req;
	spin_unlock_irqrestore(&raw3215_freelist_lock, flags);
}

/*
 * Get a write request structure. That is either a new or the last
 * queued write request. The request structure is set up in 
 * raw3215_mk_write_ccw.
 */
static raw3215_req *raw3215_mk_write_req(raw3215_info *raw)
{
	raw3215_req *req;

	/* check if there is a queued write request */
	req = raw->queued_write;
	if (req == NULL) {
		/* no queued write request, use new req structure */
		req = raw3215_alloc_req();
		req->type = RAW3215_WRITE;
		req->info = raw;
		req->start = raw->head;
	} else
		raw->queued_write = NULL;
	return req;
}

/*
 * Get a read request structure. If there is a queued read request
 * it is used, but that shouldn't happen because a 3215 terminal
 * won't accept a new read before the old one is completed.
 */
static raw3215_req *raw3215_mk_read_req(raw3215_info *raw)
{
	raw3215_req *req;

	/* there can only be ONE read request at a time */
	req = raw->queued_read;
	if (req == NULL) {
		/* no queued read request, use new req structure */
		req = raw3215_alloc_req();
		req->type = RAW3215_READ;
		req->info = raw;
	} else
		raw->queued_read = NULL;
	return req;
}

/*
 * Set up a write request with the information from the main structure.
 * A ccw chain is created that writes everything in the output buffer
 * to the 3215 device.
 */
static int raw3215_mk_write_ccw(raw3215_info *raw, raw3215_req *req)
{
	ccw1_t *ccw;
	int len, count, ix;

	ccw = req->ccws;
	req->end = (raw->head - 1) & (RAW3215_BUFFER_SIZE - 1);
	len = ((req->end - req->start) & (RAW3215_BUFFER_SIZE - 1)) + 1;
	ix = req->start;
	while (len > 0) {
		if (ccw > req->ccws)
			ccw[-1].flags |= 0x40; /* use command chaining */
		ccw->cmd_code = 0x01; /* write, auto carrier return */
		ccw->flags = 0x20;    /* ignore incorrect length ind.  */
		ccw->cda =
			(void *) virt_to_phys(raw->buffer + ix);
		count = (len > RAW3215_MAX_CCWLEN) ? 
			RAW3215_MAX_CCWLEN : len;
		if (ix + count > RAW3215_BUFFER_SIZE)
			count = RAW3215_BUFFER_SIZE-ix;
		ccw->count = count;
		len -= count;
		ix = (ix + count) & (RAW3215_BUFFER_SIZE - 1);
		ccw++;
	}
	return len;
}

/*
 * Set up a read request that reads up to 160 byte from the 3215 device.
 */
static void raw3215_mk_read_ccw(raw3215_info *raw, raw3215_req *req)
{
	ccw1_t *ccw;

	ccw = req->ccws;
	ccw->cmd_code = 0x0A; /* read inquiry */
	ccw->flags = 0x20;    /* ignore incorrect length */
	ccw->count = 160;
	ccw->cda = (void *) virt_to_phys(raw->inbuf);
}

/*
 * Start a read or a write request
 */
static void raw3215_start_io(raw3215_info *raw)
{
	raw3215_req *req;
	int res;

	req = raw->queued_read;
	if (req != NULL &&
	    !(raw->flags & (RAW3215_WORKING | RAW3215_THROTTLED))) {
		/* dequeue request */
		raw->queued_read = NULL;
		res = do_IO(raw->irq, req->ccws, (__u32) req, 0, 0);
		if (res != 0) {
			/* do_IO failed, put request back to queue */
			raw->queued_read = req;
		} else {
			raw->flags |= RAW3215_WORKING;
		} 
	}
	req = raw->queued_write;
	if (req != NULL &&
	    !(raw->flags & (RAW3215_WORKING | RAW3215_STOPPED))) {
		/* dequeue request */
		raw->queued_write = NULL;
		res = do_IO(raw->irq, req->ccws, (__u32) req, 0, 0);
		if (res != 0) {
			/* do_IO failed, put request back to queue */
			raw->queued_write = req;
		} else {
			raw->flags |= RAW3215_WORKING;
		}
	}
}

/*
 * Function to start a delayed output after RAW3215_TIMEOUT seconds
 */
static void raw3215_timeout(unsigned long __data)
{
	raw3215_info *raw = (raw3215_info *) __data;
	unsigned long flags;

	s390irq_spin_lock_irqsave(raw->irq, flags);
	if (raw->flags & RAW3215_TIMER_RUNS) {
		del_timer(&raw->timer);
		raw->flags &= ~RAW3215_TIMER_RUNS;
		raw3215_start_io(raw);
	}
	s390irq_spin_unlock_irqrestore(raw->irq, flags);
}

/*
 * Function to conditionally start an IO. A read is started immediatly,
 * a write is only started immediatly if the flush flag is on or the
 * amount of data is bigger than RAW3215_MIN_WRITE. If a write is not
 * done immediatly a timer is started with a delay of RAW3215_TIMEOUT.
 */
extern inline void raw3215_try_io(raw3215_info *raw)
{
	if (!(raw->flags & RAW3215_ACTIVE))
		return;
	if (raw->queued_read != NULL)
		raw3215_start_io(raw);
	else if (raw->queued_write != NULL) {
		if (raw->count >= RAW3215_MIN_WRITE ||
		    (raw->flags & RAW3215_FLUSHING)) {
			/* execute write requests bigger than minimum size */
			raw3215_start_io(raw);
			if (raw->flags & RAW3215_TIMER_RUNS) {
				del_timer(&raw->timer);
				raw->flags &= ~RAW3215_TIMER_RUNS;
			}
		} else if (!(raw->flags & RAW3215_TIMER_RUNS)) {
			/* delay small writes */
			init_timer(&raw->timer);
			raw->timer.expires = RAW3215_TIMEOUT + jiffies;
			raw->timer.data = (unsigned long) raw;
			raw->timer.function = raw3215_timeout;
			add_timer(&raw->timer);
			raw->flags |= RAW3215_TIMER_RUNS;
		}
	}
}

/*
 * The bottom half handler routine for 3215 devices. It tries to start
 * the next IO and wakes up processes waiting on the tty.
 */
static void raw3215_softint(void *data)
{
	raw3215_info *raw;
	struct tty_struct *tty;
	unsigned long flags;

	raw = (raw3215_info *) data;
	s390irq_spin_lock_irqsave(raw->irq, flags);
	raw3215_try_io((raw3215_info *) data);
        raw->flags &= ~RAW3215_BH_PENDING;
	s390irq_spin_unlock_irqrestore(raw->irq, flags);
	/* Check for pending message from raw3215_irq */
	if (raw->message != NULL) {
		printk(raw->message, raw->irq, raw->msg_dstat, raw->msg_cstat);
		raw->message = NULL;
	}
	tty = raw->tty;
	if (tty != NULL &&
	    RAW3215_BUFFER_SIZE - raw->count >= RAW3215_MIN_SPACE) {
		if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
		    tty->ldisc.write_wakeup)
			(tty->ldisc.write_wakeup)(tty);
		wake_up_interruptible(&tty->write_wait);
	}
}

/*
 * Function to safely add raw3215_softint to tq_immediate.
 * The s390irq spinlock must be held.
 */
static inline void raw3215_sched_bh(raw3215_info *raw)
{
        if (raw->flags & RAW3215_BH_PENDING)
                return;       /* already pending */
        raw->flags |= RAW3215_BH_PENDING;
        raw->tqueue.routine = raw3215_softint;
        raw->tqueue.data = raw;
        queue_task(&raw->tqueue, &tq_immediate);
        mark_bh(IMMEDIATE_BH);
}

/*
 * Find the raw3215_info structure associated with irq
 */
static inline raw3215_info *raw3215_find_info(int irq) {
	raw3215_info *raw;
	int i;

	for (i = 0; i < NR_3215; i++) {
		raw = raw3215[i];
		if (raw != NULL && raw->irq == irq &&
		    (raw->flags & RAW3215_ACTIVE))
			break;
	}
	return (i >= NR_3215) ? NULL : raw;
}

/*
 * Interrupt routine, called from Ingo's I/O layer
 */
static void raw3215_irq(int irq, void *int_parm, struct pt_regs *regs)
{
	raw3215_info *raw;
	raw3215_req *req;
	struct tty_struct *tty;
	devstat_t *stat;
        int cstat, dstat;
	int count, slen;

	stat = (devstat_t *) int_parm;
	req = (raw3215_req *) stat->intparm;
	cstat = stat->cstat;
	dstat = stat->dstat;
	if (cstat != 0) {
		raw = raw3215_find_info(irq);
		if (raw != NULL) {
			raw->message = KERN_WARNING
				"Got nonzero channel status in raw3215_irq "
				"(dev %i, dev sts 0x%2x, sch sts 0x%2x)";
			raw->msg_dstat = dstat;
			raw->msg_cstat = cstat;
                        raw3215_sched_bh(raw);
		}
	}
        if (dstat & 0x01) { /* we got a unit exception */
		dstat &= ~0x01;  /* we can ignore it */
        }
	switch (dstat) {
	case 0x80:
		if (cstat != 0)
			break;
		/* Attention interrupt, someone hit the enter key */
		if ((raw = raw3215_find_info(irq)) == NULL)
			return;              /* That shouldn't happen ... */
		/* Setup a read request */
		req = raw3215_mk_read_req(raw);
		raw3215_mk_read_ccw(raw, req);
		raw->queued_read = req;
                if (MACHINE_IS_P390)
                        memset(raw->inbuf, 0, RAW3215_INBUF_SIZE);
                raw3215_sched_bh(raw);
		break;
	case 0x08:
	case 0x0C:
		/* Channel end interrupt. */
		raw = req->info;
		if (req->type == RAW3215_READ) {
			/* store residual count, then wait for device end */
			req->residual = stat->rescnt;
		}
		if (dstat == 0x08)
			break;
	case 0x04:
		/* Device end interrupt. */
		raw = req->info;
		if (req->type == RAW3215_READ && raw->tty != NULL) {
			tty = raw->tty;
                        count = 160 - req->residual;
                        if (MACHINE_IS_P390) {
                                slen = strnlen(raw->inbuf, RAW3215_INBUF_SIZE);
                                if (count > slen)
                                        count = slen;
                        } else
			if (count >= TTY_FLIPBUF_SIZE - tty->flip.count)
				count = TTY_FLIPBUF_SIZE - tty->flip.count - 1;
			EBCASC(raw->inbuf, count);
			if (count == 2 && (
                            /* hat is 0xb0 in codepage 037 (US etc.) and thus */
                            /* converted to 0x5e in ascii ('^') */
                            strncmp(raw->inbuf, "^c", 2) == 0 ||
                            /* hat is 0xb0 in several other codepages (German,*/
                            /* UK, ...) and thus converted to ascii octal 252 */
                            strncmp(raw->inbuf, "\252c", 2) == 0) ) {
				/* emulate a control C = break */
				tty->flip.count++;
				*tty->flip.flag_buf_ptr++ = TTY_NORMAL;
				*tty->flip.char_buf_ptr++ = INTR_CHAR(tty);
				tty_flip_buffer_push(raw->tty);
			} else if (count == 2 && (
                                   strncmp(raw->inbuf, "^d", 2) == 0 ||
                                    strncmp(raw->inbuf, "\252d", 2) == 0) ) {
				/* emulate a control D = end of file */
				tty->flip.count++;
				*tty->flip.flag_buf_ptr++ = TTY_NORMAL;
				*tty->flip.char_buf_ptr++ = EOF_CHAR(tty);
				tty_flip_buffer_push(raw->tty);
			} else if (count == 2 && (
                                   strncmp(raw->inbuf, "^z", 2) == 0 ||
                                    strncmp(raw->inbuf, "\252z", 2) == 0) ) {
				/* emulate a control Z = suspend */
				tty->flip.count++;
				*tty->flip.flag_buf_ptr++ = TTY_NORMAL;
				*tty->flip.char_buf_ptr++ = SUSP_CHAR(tty);
				tty_flip_buffer_push(raw->tty);
			} else {
				memcpy(tty->flip.char_buf_ptr,
				       raw->inbuf, count);
				if (count < 2 ||
                                    (strncmp(raw->inbuf+count-2, "^n", 2) ||
                                    strncmp(raw->inbuf+count-2, "\252n", 2)) ) {					/* don't add the auto \n */
					tty->flip.char_buf_ptr[count] = '\n';
					memset(tty->flip.flag_buf_ptr,
					       TTY_NORMAL, count + 1);
					count++;
				} else
					count-=2;
				tty->flip.char_buf_ptr += count;
				tty->flip.flag_buf_ptr += count;
				tty->flip.count += count;
				tty_flip_buffer_push(raw->tty);
			}
		} else if (req->type == RAW3215_WRITE) {
			raw->count -= ((req->end - req->start) &
				       (RAW3215_BUFFER_SIZE - 1)) + 1;
		}
		raw->flags &= ~RAW3215_WORKING;
		raw3215_free_req(req);
		/* check for empty wait */
		if (waitqueue_active(&raw->empty_wait) &&
		    raw->queued_write == NULL &&
		    raw->queued_read == NULL) {
			wake_up_interruptible(&raw->empty_wait);
		}
                raw3215_sched_bh(raw);
		break;
	default:
		/* Strange interrupt, I'll do my best to clean up */
                if ((raw = raw3215_find_info(irq)) == NULL)
                        return;              /* That shouldn't happen ... */
                if (raw == NULL) break;
		if (req != NULL && req->type != RAW3215_FREE) {
		        if (req->type == RAW3215_WRITE) 
			        raw->count -= ((req->end - req->start) &
				              (RAW3215_BUFFER_SIZE-1))+1;
                        raw->flags &= ~RAW3215_WORKING;
                        raw3215_free_req(req);
		}
		raw->message = KERN_WARNING
			"Spurious interrupt in in raw3215_irq "
			"(dev %i, dev sts 0x%2x, sch sts 0x%2x)";
		raw->msg_dstat = dstat;
		raw->msg_cstat = cstat;
                raw3215_sched_bh(raw);
	}
	return;
}

/*
 * String write routine for 3215 devices
 */
static int
raw3215_write(raw3215_info *raw, const char *str,
	      int from_user, unsigned int length)
{
	raw3215_req *req;
	unsigned long flags;
	int ret, c;
	int count;
	
	ret = 0;
	while (length > 0) {
		s390irq_spin_lock_irqsave(raw->irq, flags);
		count = (length > RAW3215_BUFFER_SIZE) ?
					     RAW3215_BUFFER_SIZE : length;
		length -= count;

		while (RAW3215_BUFFER_SIZE - raw->count < count) {
			/* there might be a request pending */
			raw3215_try_io(raw);
			if (wait_cons_dev(raw->irq) != 0) {
				/* that shouldn't happen */
				raw->count = 0;
			} 
		}

		req = raw3215_mk_write_req(raw);
		/* copy string to output buffer and convert it to EBCDIC */
		if (from_user) {
			while (1) {
				c = MIN(count,
					MIN(RAW3215_BUFFER_SIZE - raw->count,
					    RAW3215_BUFFER_SIZE - raw->head));
				if (c <= 0)
					break;
				c -= copy_from_user(raw->buffer + raw->head,
						    str, c);
				if (c == 0) {
					if (!ret)
						ret = -EFAULT;
					break;
				}
				ASCEBC(raw->buffer + raw->head, c);
				raw->head = (raw->head + c) &
					    (RAW3215_BUFFER_SIZE - 1);
				raw->count += c;
				str += c;
				count -= c;
				ret += c;
			}
		} else {
			while (1) {
				c = MIN(count,
					MIN(RAW3215_BUFFER_SIZE - raw->count,
					    RAW3215_BUFFER_SIZE - raw->head));
				if (c <= 0)
					break;
				memcpy(raw->buffer + raw->head, str, c);
				ASCEBC(raw->buffer + raw->head, c);
				raw->head = (raw->head + c) &
					    (RAW3215_BUFFER_SIZE - 1);
				raw->count += c;
				str += c;
				count -= c;
				ret += c;
			}
		}
		raw3215_mk_write_ccw(raw, req);
		raw->queued_write = req;
		/* start or queue request */
		raw3215_try_io(raw);
		s390irq_spin_unlock_irqrestore(raw->irq, flags);

	}

	return ret;
}

/*
 * Put character routine for 3215 devices
 */
static void raw3215_putchar(raw3215_info *raw, unsigned char ch)
{
	raw3215_req *req;
	unsigned long flags;

	s390irq_spin_lock_irqsave(raw->irq, flags);
	while (RAW3215_BUFFER_SIZE - raw->count < 1) {
		/* there might be a request pending */
		raw3215_try_io(raw);
		if (wait_cons_dev(raw->irq) != 0) {
			/* that shouldn't happen */
			raw->count = 0;
		}
	}

	req = raw3215_mk_write_req(raw);
	raw->buffer[raw->head] = (char) _ascebc[(int) ch];
	raw->head = (raw->head + 1) & (RAW3215_BUFFER_SIZE - 1);
	raw->count++;
	raw3215_mk_write_ccw(raw, req);
	raw->queued_write = req;
	/* start or queue request */
	raw3215_try_io(raw);
	s390irq_spin_unlock_irqrestore(raw->irq, flags);
}

/*
 * Flush routine, it simply sets the flush flag and tries to start 
 * pending IO.
 */
static void raw3215_flush_buffer(raw3215_info *raw)
{
	unsigned long flags;

	s390irq_spin_lock_irqsave(raw->irq, flags);
	if (raw->count > 0) {
		raw->flags |= RAW3215_FLUSHING;
		raw3215_try_io(raw);
		raw->flags &= ~RAW3215_FLUSHING;
	}
	s390irq_spin_unlock_irqrestore(raw->irq, flags);
}

/*
 * Fire up a 3215 device.
 */
static int raw3215_startup(raw3215_info *raw)
{
	unsigned long flags;

	if (raw->flags & RAW3215_ACTIVE)
		return 0;
	if (request_irq(raw->irq, raw3215_irq, SA_INTERRUPT,
			"3215 terminal driver", &raw->devstat) != 0)
		return -1;
	raw->flags |= RAW3215_ACTIVE;
	s390irq_spin_lock_irqsave(raw->irq, flags);
	raw3215_try_io(raw);
	s390irq_spin_unlock_irqrestore(raw->irq, flags);

	return 0;	
}

/*
 * Shutdown a 3215 device.
 */
static void raw3215_shutdown(raw3215_info *raw)
{
	DECLARE_WAITQUEUE(wait, current);
	unsigned long flags;

	if (!(raw->flags & RAW3215_ACTIVE) || (raw->flags & RAW3215_FIXED))
		return;
	/* Wait for outstanding requests, then free irq */
	s390irq_spin_lock_irqsave(raw->irq, flags);
	if ((raw->flags & RAW3215_WORKING) ||
	    raw->queued_write != NULL ||
	    raw->queued_read != NULL) {
		raw->flags |= RAW3215_CLOSING;
		add_wait_queue(&raw->empty_wait, &wait);
		current->state = TASK_INTERRUPTIBLE;
                s390irq_spin_unlock_irqrestore(raw->irq, flags);
		schedule();
		s390irq_spin_lock_irqsave(raw->irq, flags);
		remove_wait_queue(&raw->empty_wait, &wait);
                current->state = TASK_RUNNING;
		raw->flags &= ~(RAW3215_ACTIVE | RAW3215_CLOSING);
	}
	free_irq(raw->irq, NULL);
	s390irq_spin_unlock_irqrestore(raw->irq, flags);
}

static int
raw3215_find_dev(int number)
{
	dev_info_t dinfo;
	int irq;
	int count;

	irq = get_irq_first();
	count = 0;
        while (count <= number && irq != -ENODEV) {
                if (get_dev_info(irq, &dinfo) == -ENODEV)
                        break;
                if (dinfo.devno == raw3215_condevice ||
                    dinfo.sid_data.cu_type == 0x3215) {
                        count++;
                    if (count > number)
                        return irq;
                }
                irq = get_irq_next(irq);
        }
        return -1;            /* console not found */
}

#ifdef CONFIG_3215_CONSOLE

/*
 * Try to request the console IRQ. Called from init/main.c
 */
int con3215_activate(void)
{
	raw3215_info *raw;

        if (!MACHINE_IS_VM && !MACHINE_IS_P390)
                return 0;
	raw = raw3215[0];  /* 3215 console is the first one */
	if (raw->irq == -1) /* now console device found in con3215_init */
		return -1;
	return raw3215_startup(raw);
}

/*
 * Write a string to the 3215 console
 */
static void
con3215_write(struct console *co, const char *str, unsigned int count)
{
	raw3215_info *raw;

	if (count <= 0)
		return;
	raw = raw3215[0];  /* console 3215 is the first one */
	raw3215_write(raw, str, 0, count);
}

kdev_t con3215_device(struct console *c)
{
	return MKDEV(TTY_MAJOR, c->index);
}

/*
 * panic() calls console_unblank before the system enters a
 * disabled, endless loop.
 */
void con3215_unblank(void)
{
	raw3215_info *raw;
	unsigned long flags;

	raw = raw3215[0];  /* console 3215 is the first one */
	s390irq_spin_lock_irqsave(raw->irq, flags);
	while (raw->count > 0) {
		/* there might be a request pending */
		raw->flags |= RAW3215_FLUSHING;
		raw3215_try_io(raw);
		if (wait_cons_dev(raw->irq) != 0) {
			/* that shouldn't happen */
			raw->count = 0;
		}
		raw->flags &= ~RAW3215_FLUSHING;
	}
	s390irq_spin_unlock_irqrestore(raw->irq, flags);
}

static int __init con3215_consetup(struct console *co, char *options)
{
	return 0;
}

/*
 *  The console structure for the 3215 console
 */
static struct console con3215 = {
	name:		"tty3215",
	write:		con3215_write,
	device:		con3215_device,
	unblank:	con3215_unblank,
	setup:		con3215_consetup,
	flags:		CON_PRINTBUFFER,
};

#endif

/*
 * tty3215_open
 *
 * This routine is called whenever a 3215 tty is opened.
 */
static int tty3215_open(struct tty_struct *tty, struct file * filp)
{
	raw3215_info *raw;
	int retval, line;

	line = MINOR(tty->device) - tty->driver.minor_start;
	if ((line < 0) || (line >= NR_3215))
		return -ENODEV;

	raw = raw3215[line];
	if (raw == NULL) {
		raw = kmalloc(sizeof(raw3215_info) +
			      RAW3215_INBUF_SIZE, GFP_KERNEL);
		if (raw == NULL)
			return -ENOMEM;
		raw->irq = raw3215_find_dev(line);
		if (raw->irq == -1) {
			kfree(raw);
			return -ENODEV;
		}
		raw->inbuf = (char *) raw + sizeof(raw3215_info);
		memset(raw, 0, sizeof(raw3215_info));
		raw->buffer = (char *) kmalloc(RAW3215_BUFFER_SIZE, GFP_KERNEL);
		if (raw->buffer == NULL) {
			kfree(raw);
			return -ENOMEM;
		}
		raw->tqueue.routine = raw3215_softint;
		raw->tqueue.data = raw;
                init_waitqueue_head(&raw->empty_wait);
		raw3215[line] = raw;
	}

	tty->driver_data = raw;
	raw->tty = tty;

	tty->low_latency = 0;  /* don't use bottom half for pushing chars */
	/*
	 * Start up 3215 device
	 */
	retval = raw3215_startup(raw);
	if (retval)
		return retval;

	return 0;
}

/*
 * tty3215_close()
 *
 * This routine is called when the 3215 tty is closed. We wait
 * for the remaining request to be completed. Then we clean up.
 */
static void tty3215_close(struct tty_struct *tty, struct file * filp)
{
	raw3215_info *raw;

	raw = (raw3215_info *) tty->driver_data;
	if (raw == NULL || tty->count > 1)
		return;
	tty->closing = 1;
	/* Shutdown the terminal */
	raw3215_shutdown(raw);
	tty->closing = 0;
	raw->tty = NULL;
}

/*
 * Returns the amount of free space in the output buffer.
 */
static int tty3215_write_room(struct tty_struct *tty)
{
	raw3215_info *raw;
				
	raw = (raw3215_info *) tty->driver_data;
	return RAW3215_BUFFER_SIZE - raw->count;
}

/*
 * String write routine for 3215 ttys
 */
static int tty3215_write(struct tty_struct * tty, int from_user,
		    const unsigned char *buf, int count)
{
	raw3215_info *raw;
	int ret;
				
	if (!tty)
		return 0;
	raw = (raw3215_info *) tty->driver_data;
	ret = raw3215_write(raw, buf, from_user, count);
	return ret;
}

/*
 * Put character routine for 3215 ttys
 */
static void tty3215_put_char(struct tty_struct *tty, unsigned char ch)
{
	raw3215_info *raw;

	if (!tty)
		return;
	raw = (raw3215_info *) tty->driver_data;
	raw3215_putchar(raw, ch);
}

static void tty3215_flush_chars(struct tty_struct *tty)
{
}

/*
 * Returns the number of characters in the output buffer
 */
static int tty3215_chars_in_buffer(struct tty_struct *tty)
{
	raw3215_info *raw;

	raw = (raw3215_info *) tty->driver_data;
	return raw->count;
}

static void tty3215_flush_buffer(struct tty_struct *tty)
{
	raw3215_info *raw;

	raw = (raw3215_info *) tty->driver_data;
	raw3215_flush_buffer(raw);
	wake_up_interruptible(&tty->write_wait);
	if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
	    tty->ldisc.write_wakeup)
		(tty->ldisc.write_wakeup)(tty);
}

/*
 * Currently we don't have any io controls for 3215 ttys
 */
static int tty3215_ioctl(struct tty_struct *tty, struct file * file,
		    unsigned int cmd, unsigned long arg)
{
	if (tty->flags & (1 << TTY_IO_ERROR))
		return -EIO;

	switch (cmd) {
	default:
		return -ENOIOCTLCMD;
	}
	return 0;
}

/*
 * Disable reading from a 3215 tty
 */
static void tty3215_throttle(struct tty_struct * tty)
{
	raw3215_info *raw;

	raw = (raw3215_info *) tty->driver_data;
	raw->flags |= RAW3215_THROTTLED;
}

/*
 * Enable reading from a 3215 tty
 */
static void tty3215_unthrottle(struct tty_struct * tty)
{
	raw3215_info *raw;
	unsigned long flags;

	raw = (raw3215_info *) tty->driver_data;
	if (raw->flags & RAW3215_THROTTLED) {
		s390irq_spin_lock_irqsave(raw->irq, flags);
		raw->flags &= ~RAW3215_THROTTLED;
		raw3215_try_io(raw);
		s390irq_spin_unlock_irqrestore(raw->irq, flags);
	}
}

/*
 * Disable writing to a 3215 tty
 */
static void tty3215_stop(struct tty_struct *tty)
{
	raw3215_info *raw;

	raw = (raw3215_info *) tty->driver_data;
	raw->flags |= RAW3215_STOPPED;
}

/*
 * Enable writing to a 3215 tty
 */
static void tty3215_start(struct tty_struct *tty)
{
	raw3215_info *raw;
	unsigned long flags;

	raw = (raw3215_info *) tty->driver_data;
	if (raw->flags & RAW3215_STOPPED) {
		s390irq_spin_lock_irqsave(raw->irq, flags);
		raw->flags &= ~RAW3215_STOPPED;
		raw3215_try_io(raw);
		s390irq_spin_unlock_irqrestore(raw->irq, flags);
	}
}

/*
 * 3215 console driver boottime initialization code.
 * Register console. We can't request the IRQ here, because
 * it's too early (kmalloc isn't working yet). We'll have to
 * buffer all the console requests until we can request the
 * irq. For this purpose we use some pages of fixed memory.
 */
void __init con3215_init(void)
{
	raw3215_info *raw;
	raw3215_req *req;
	int i;

	if (!MACHINE_IS_VM && !MACHINE_IS_P390)
                return;
        if (MACHINE_IS_VM) {
	        cpcmd("TERM CONMODE 3215", NULL, 0);
	        cpcmd("TERM AUTOCR OFF", NULL, 0);
        }

	/* allocate 3215 request structures */
	raw3215_freelist = NULL;
	spin_lock_init(&raw3215_freelist_lock);
	for (i = 0; i < NR_3215_REQ; i++) {
		req = (raw3215_req *) alloc_bootmem(sizeof(raw3215_req));
		req->next = raw3215_freelist;
		raw3215_freelist = req;
	}

#ifdef CONFIG_3215_CONSOLE
	raw3215[0] = raw = (raw3215_info *) 
		alloc_bootmem(sizeof(raw3215_info));
	memset(raw, 0, sizeof(raw3215_info));
	raw->buffer = (char *) alloc_bootmem(RAW3215_BUFFER_SIZE);
	raw->inbuf = (char *) alloc_bootmem(RAW3215_INBUF_SIZE);
	/* Find the first console */
	raw->irq = raw3215_find_dev(0);
	raw->flags |= RAW3215_FIXED;
	raw->tqueue.routine = raw3215_softint;
	raw->tqueue.data = raw;
        init_waitqueue_head(&raw->empty_wait);

	if (raw->irq != -1) {
		register_console(&con3215);
		s390irq_spin_lock(raw->irq);
		set_cons_dev(raw->irq);
		s390irq_spin_unlock(raw->irq);
	} else {
		free_bootmem((unsigned long) raw->inbuf, RAW3215_INBUF_SIZE);
		free_bootmem((unsigned long) raw->buffer, RAW3215_BUFFER_SIZE);
		free_bootmem((unsigned long) raw, sizeof(raw3215_info));
		raw3215[0] = NULL;
		printk("Couldn't find a 3215 console device\n");
	}
#endif

	/*
	 * Initialize the tty_driver structure
	 * Entries in tty3215_driver that are NOT initialized:
	 * proc_entry, set_termios, flush_buffer, set_ldisc, write_proc
	 */

	memset(&tty3215_driver, 0, sizeof(struct tty_driver));
	tty3215_driver.magic = TTY_DRIVER_MAGIC;
	tty3215_driver.driver_name = "tty3215";
	tty3215_driver.name = "ttyS";
	tty3215_driver.name_base = 0;
	tty3215_driver.major = TTY_MAJOR;
	tty3215_driver.minor_start = 64;
	tty3215_driver.num = NR_3215;
	tty3215_driver.type = TTY_DRIVER_TYPE_SYSTEM;
	tty3215_driver.subtype = SYSTEM_TYPE_TTY;
	tty3215_driver.init_termios = tty_std_termios;
	tty3215_driver.init_termios.c_iflag = IGNBRK | IGNPAR;
	tty3215_driver.init_termios.c_oflag = ONLCR;
	tty3215_driver.init_termios.c_lflag = ISIG;
	tty3215_driver.flags = TTY_DRIVER_REAL_RAW;
	tty3215_driver.refcount = &tty3215_refcount;
	tty3215_driver.table = tty3215_table;
	tty3215_driver.termios = tty3215_termios;
	tty3215_driver.termios_locked = tty3215_termios_locked;

	tty3215_driver.open = tty3215_open;
	tty3215_driver.close = tty3215_close;
	tty3215_driver.write = tty3215_write;
	tty3215_driver.put_char = tty3215_put_char;
	tty3215_driver.flush_chars = tty3215_flush_chars;
	tty3215_driver.write_room = tty3215_write_room;
	tty3215_driver.chars_in_buffer = tty3215_chars_in_buffer;
	tty3215_driver.flush_buffer = tty3215_flush_buffer;
	tty3215_driver.ioctl = tty3215_ioctl;
	tty3215_driver.throttle = tty3215_throttle;
	tty3215_driver.unthrottle = tty3215_unthrottle;
	tty3215_driver.send_xchar = NULL;
	tty3215_driver.set_termios = NULL;
	tty3215_driver.stop = tty3215_stop;
	tty3215_driver.start = tty3215_start;
	tty3215_driver.hangup = NULL;
	tty3215_driver.break_ctl = NULL;
	tty3215_driver.wait_until_sent = NULL;
	tty3215_driver.read_proc = NULL;

	if (tty_register_driver(&tty3215_driver))
		panic("Couldn't register tty3215 driver\n");

}
