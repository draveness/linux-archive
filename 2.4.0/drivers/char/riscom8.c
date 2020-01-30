/*
 *      linux/drivers/char/riscom.c  -- RISCom/8 multiport serial driver.
 *
 *      Copyright (C) 1994-1996  Dmitry Gorodchanin (pgmdsg@ibi.com)
 *
 *      This code is loosely based on the Linux serial driver, written by
 *      Linus Torvalds, Theodore T'so and others. The RISCom/8 card 
 *      programming info was obtained from various drivers for other OSes 
 *	(FreeBSD, ISC, etc), but no source code from those drivers were 
 *	directly included in this driver.
 *
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *	Revision 1.0 
 */

#include <linux/module.h>

#include <asm/io.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/tty.h>
#include <linux/mm.h>
#include <linux/serial.h>
#include <linux/fcntl.h>
#include <linux/major.h>
#include <linux/init.h>

#include <asm/uaccess.h>

#include "riscom8.h"
#include "riscom8_reg.h"

/* Am I paranoid or not ? ;-) */
#define RISCOM_PARANOIA_CHECK

/* 
 * Crazy InteliCom/8 boards sometimes has swapped CTS & DSR signals.
 * You can slightly speed up things by #undefing the following option,
 * if you are REALLY sure that your board is correct one. 
 */

#define RISCOM_BRAIN_DAMAGED_CTS

/* 
 * The following defines are mostly for testing purposes. But if you need
 * some nice reporting in your syslog, you can define them also.
 */
#undef RC_REPORT_FIFO
#undef RC_REPORT_OVERRUN


#define RISCOM_LEGAL_FLAGS \
	(ASYNC_HUP_NOTIFY   | ASYNC_SAK          | ASYNC_SPLIT_TERMIOS   | \
	 ASYNC_SPD_HI       | ASYNC_SPEED_VHI    | ASYNC_SESSION_LOCKOUT | \
	 ASYNC_PGRP_LOCKOUT | ASYNC_CALLOUT_NOHUP)

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

#define RS_EVENT_WRITE_WAKEUP	0

static DECLARE_TASK_QUEUE(tq_riscom);

#define RISCOM_TYPE_NORMAL	1
#define RISCOM_TYPE_CALLOUT	2

static struct riscom_board * IRQ_to_board[16];
static struct tty_driver riscom_driver, riscom_callout_driver;
static int    riscom_refcount;
static struct tty_struct * riscom_table[RC_NBOARD * RC_NPORT];
static struct termios * riscom_termios[RC_NBOARD * RC_NPORT];
static struct termios * riscom_termios_locked[RC_NBOARD * RC_NPORT];
static unsigned char * tmp_buf;
static DECLARE_MUTEX(tmp_buf_sem);

static unsigned long baud_table[] =  {
	0, 50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400, 4800,
	9600, 19200, 38400, 57600, 76800, 0, 
};

static struct riscom_board rc_board[RC_NBOARD] =  {
	{ 0, RC_IOBASE1, 0, },
	{ 0, RC_IOBASE2, 0, },
	{ 0, RC_IOBASE3, 0, },
	{ 0, RC_IOBASE4, 0, },
};

static struct riscom_port rc_port[RC_NBOARD * RC_NPORT];
		
/* RISCom/8 I/O ports addresses (without address translation) */
static unsigned short rc_ioport[] =  {
#if 1	
	0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x09, 0x0a, 0x0b, 0x0c,
#else	
	0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x09, 0x0a, 0x0b, 0x0c, 0x10,
	0x11, 0x12, 0x18, 0x28, 0x31, 0x32, 0x39, 0x3a, 0x40, 0x41, 0x61, 0x62,
	0x63, 0x64, 0x6b, 0x70, 0x71, 0x78, 0x7a, 0x7b, 0x7f, 0x100, 0x101
#endif	
};
#define RC_NIOPORT	(sizeof(rc_ioport) / sizeof(rc_ioport[0]))


static inline int rc_paranoia_check(struct riscom_port const * port,
				    kdev_t device, const char *routine)
{
#ifdef RISCOM_PARANOIA_CHECK
	static const char *badmagic =
		"rc: Warning: bad riscom port magic number for device %s in %s\n";
	static const char *badinfo =
		"rc: Warning: null riscom port for device %s in %s\n";

	if (!port) {
		printk(badinfo, kdevname(device), routine);
		return 1;
	}
	if (port->magic != RISCOM8_MAGIC) {
		printk(badmagic, kdevname(device), routine);
		return 1;
	}
#endif
	return 0;
}

/*
 * 
 *  Service functions for RISCom/8 driver.
 * 
 */

/* Get board number from pointer */
static inline int board_No (struct riscom_board const * bp)
{
	return bp - rc_board;
}

/* Get port number from pointer */
static inline int port_No (struct riscom_port const * port)
{
	return RC_PORT(port - rc_port); 
}

/* Get pointer to board from pointer to port */
static inline struct riscom_board * port_Board(struct riscom_port const * port)
{
	return &rc_board[RC_BOARD(port - rc_port)];
}

/* Input Byte from CL CD180 register */
static inline unsigned char rc_in(struct riscom_board const * bp, unsigned short reg)
{
	return inb(bp->base + RC_TO_ISA(reg));
}

/* Output Byte to CL CD180 register */
static inline void rc_out(struct riscom_board const * bp, unsigned short reg,
			  unsigned char val)
{
	outb(val, bp->base + RC_TO_ISA(reg));
}

/* Wait for Channel Command Register ready */
static inline void rc_wait_CCR(struct riscom_board const * bp)
{
	unsigned long delay;

	/* FIXME: need something more descriptive then 100000 :) */
	for (delay = 100000; delay; delay--) 
		if (!rc_in(bp, CD180_CCR))
			return;
	
	printk("rc%d: Timeout waiting for CCR.\n", board_No(bp));
}

/*
 *  RISCom/8 probe functions.
 */

static inline int rc_check_io_range(struct riscom_board * const bp)
{
	int i;
	
	for (i = 0; i < RC_NIOPORT; i++)  
		if (check_region(RC_TO_ISA(rc_ioport[i]) + bp->base, 1))  {
			printk("rc%d: Skipping probe at 0x%03x. I/O address in use.\n",
			       board_No(bp), bp->base);
			return 1;
		}
	return 0;
}

static inline void rc_request_io_range(struct riscom_board * const bp)
{
	int i;
	
	for (i = 0; i < RC_NIOPORT; i++) 
		request_region(RC_TO_ISA(rc_ioport[i]) + bp->base, 1, "RISCom/8" );
}

static inline void rc_release_io_range(struct riscom_board * const bp)
{
	int i;
	
	for (i = 0; i < RC_NIOPORT; i++)  
		release_region(RC_TO_ISA(rc_ioport[i]) + bp->base, 1);
}

	
/* Must be called with enabled interrupts */
static inline void rc_long_delay(unsigned long delay)
{
	unsigned long i;
	
	for (i = jiffies + delay; time_after(i,jiffies); ) ;
}

/* Reset and setup CD180 chip */
static void __init rc_init_CD180(struct riscom_board const * bp)
{
	unsigned long flags;
	
	save_flags(flags); cli();
	rc_out(bp, RC_CTOUT, 0);     	           /* Clear timeout             */
	rc_wait_CCR(bp);			   /* Wait for CCR ready        */
	rc_out(bp, CD180_CCR, CCR_HARDRESET);      /* Reset CD180 chip          */
	sti();
	rc_long_delay(HZ/20);                      /* Delay 0.05 sec            */
	cli();
	rc_out(bp, CD180_GIVR, RC_ID);             /* Set ID for this chip      */
	rc_out(bp, CD180_GICR, 0);                 /* Clear all bits            */
	rc_out(bp, CD180_PILR1, RC_ACK_MINT);      /* Prio for modem intr       */
	rc_out(bp, CD180_PILR2, RC_ACK_TINT);      /* Prio for transmitter intr */
	rc_out(bp, CD180_PILR3, RC_ACK_RINT);      /* Prio for receiver intr    */
	
	/* Setting up prescaler. We need 4 ticks per 1 ms */
	rc_out(bp, CD180_PPRH, (RC_OSCFREQ/(1000000/RISCOM_TPS)) >> 8);
	rc_out(bp, CD180_PPRL, (RC_OSCFREQ/(1000000/RISCOM_TPS)) & 0xff);
	
	restore_flags(flags);
}

/* Main probing routine, also sets irq. */
static int __init rc_probe(struct riscom_board *bp)
{
	unsigned char val1, val2;
	int irqs = 0;
	int retries;
	
	bp->irq = 0;

	if (rc_check_io_range(bp))
		return 1;
	
	/* Are the I/O ports here ? */
	rc_out(bp, CD180_PPRL, 0x5a);
	outb(0xff, 0x80);
	val1 = rc_in(bp, CD180_PPRL);
	rc_out(bp, CD180_PPRL, 0xa5);
	outb(0x00, 0x80);
	val2 = rc_in(bp, CD180_PPRL);
	
	if ((val1 != 0x5a) || (val2 != 0xa5))  {
		printk("rc%d: RISCom/8 Board at 0x%03x not found.\n",
		       board_No(bp), bp->base);
		return 1;
	}
	
	/* It's time to find IRQ for this board */
	for (retries = 0; retries < 5 && irqs <= 0; retries++)  {
		irqs = probe_irq_on();
		rc_init_CD180(bp);	       		/* Reset CD180 chip       */
		rc_out(bp, CD180_CAR, 2);               /* Select port 2          */
		rc_wait_CCR(bp);
		rc_out(bp, CD180_CCR, CCR_TXEN);        /* Enable transmitter     */
		rc_out(bp, CD180_IER, IER_TXRDY);       /* Enable tx empty intr   */
		rc_long_delay(HZ/20);	       		
		irqs = probe_irq_off(irqs);
		val1 = rc_in(bp, RC_BSR);		/* Get Board Status reg   */
		val2 = rc_in(bp, RC_ACK_TINT);          /* ACK interrupt          */
		rc_init_CD180(bp);	       		/* Reset CD180 again      */
	
		if ((val1 & RC_BSR_TINT) || (val2 != (RC_ID | GIVR_IT_TX)))  {
			printk("rc%d: RISCom/8 Board at 0x%03x not found.\n",
			       board_No(bp), bp->base);
			return 1;
		}
	}
	
	if (irqs <= 0)  {
		printk("rc%d: Can't find IRQ for RISCom/8 board at 0x%03x.\n",
		       board_No(bp), bp->base);
		return 1;
	}
	rc_request_io_range(bp);
	bp->irq = irqs;
	bp->flags |= RC_BOARD_PRESENT;
	
	printk("rc%d: RISCom/8 Rev. %c board detected at 0x%03x, IRQ %d.\n",
	       board_No(bp),
	       (rc_in(bp, CD180_GFRCR) & 0x0f) + 'A',   /* Board revision */
	       bp->base, bp->irq);
	
	return 0;
}

/* 
 * 
 *  Interrupt processing routines.
 * 
 */

static inline void rc_mark_event(struct riscom_port * port, int event)
{
	/* 
         * I'm not quite happy with current scheme all serial
	 * drivers use their own BH routine.
         * It seems this easily can be done with one BH routine
	 * serving for all serial drivers.
	 * For now I must introduce another one - RISCOM8_BH.
	 * Still hope this will be changed in near future.
         */
	set_bit(event, &port->event);
	queue_task(&port->tqueue, &tq_riscom);
	mark_bh(RISCOM8_BH);
}

static inline struct riscom_port * rc_get_port(struct riscom_board const * bp,
					       unsigned char const * what)
{
	unsigned char channel;
	struct riscom_port * port;
	
	channel = rc_in(bp, CD180_GICR) >> GICR_CHAN_OFF;
	if (channel < CD180_NCH)  {
		port = &rc_port[board_No(bp) * RC_NPORT + channel];
		if (port->flags & ASYNC_INITIALIZED)  {
			return port;
		}
	}
	printk("rc%d: %s interrupt from invalid port %d\n", 
	       board_No(bp), what, channel);
	return NULL;
}

static inline void rc_receive_exc(struct riscom_board const * bp)
{
	struct riscom_port *port;
	struct tty_struct *tty;
	unsigned char status;
	unsigned char ch;
	
	if (!(port = rc_get_port(bp, "Receive")))
		return;

	tty = port->tty;
	if (tty->flip.count >= TTY_FLIPBUF_SIZE)  {
		printk("rc%d: port %d: Working around flip buffer overflow.\n",
		       board_No(bp), port_No(port));
		return;
	}
	
#ifdef RC_REPORT_OVERRUN	
	status = rc_in(bp, CD180_RCSR);
	if (status & RCSR_OE)  {
		port->overrun++;
#if 0		
		printk("rc%d: port %d: Overrun. Total %ld overruns.\n", 
		       board_No(bp), port_No(port), port->overrun);
#endif		
	}
	status &= port->mark_mask;
#else	
	status = rc_in(bp, CD180_RCSR) & port->mark_mask;
#endif	
	ch = rc_in(bp, CD180_RDR);
	if (!status)  {
		return;
	}
	if (status & RCSR_TOUT)  {
		printk("rc%d: port %d: Receiver timeout. Hardware problems ?\n", 
		       board_No(bp), port_No(port));
		return;
		
	} else if (status & RCSR_BREAK)  {
		printk("rc%d: port %d: Handling break...\n",
		       board_No(bp), port_No(port));
		*tty->flip.flag_buf_ptr++ = TTY_BREAK;
		if (port->flags & ASYNC_SAK)
			do_SAK(tty);
		
	} else if (status & RCSR_PE) 
		*tty->flip.flag_buf_ptr++ = TTY_PARITY;
	
	else if (status & RCSR_FE) 
		*tty->flip.flag_buf_ptr++ = TTY_FRAME;
	
        else if (status & RCSR_OE)
		*tty->flip.flag_buf_ptr++ = TTY_OVERRUN;
	
	else
		*tty->flip.flag_buf_ptr++ = 0;
	
	*tty->flip.char_buf_ptr++ = ch;
	tty->flip.count++;
	queue_task(&tty->flip.tqueue, &tq_timer);
}

static inline void rc_receive(struct riscom_board const * bp)
{
	struct riscom_port *port;
	struct tty_struct *tty;
	unsigned char count;
	
	if (!(port = rc_get_port(bp, "Receive")))
		return;
	
	tty = port->tty;
	
	count = rc_in(bp, CD180_RDCR);
	
#ifdef RC_REPORT_FIFO
	port->hits[count > 8 ? 9 : count]++;
#endif	
	
	while (count--)  {
		if (tty->flip.count >= TTY_FLIPBUF_SIZE)  {
			printk("rc%d: port %d: Working around flip buffer overflow.\n",
			       board_No(bp), port_No(port));
			break;
		}
		*tty->flip.char_buf_ptr++ = rc_in(bp, CD180_RDR);
		*tty->flip.flag_buf_ptr++ = 0;
		tty->flip.count++;
	}
	queue_task(&tty->flip.tqueue, &tq_timer);
}

static inline void rc_transmit(struct riscom_board const * bp)
{
	struct riscom_port *port;
	struct tty_struct *tty;
	unsigned char count;
	
	
	if (!(port = rc_get_port(bp, "Transmit")))
		return;
	
	tty = port->tty;
	
	if (port->IER & IER_TXEMPTY)  {
		/* FIFO drained */
		rc_out(bp, CD180_CAR, port_No(port));
		port->IER &= ~IER_TXEMPTY;
		rc_out(bp, CD180_IER, port->IER);
		return;
	}
	
	if ((port->xmit_cnt <= 0 && !port->break_length)
	    || tty->stopped || tty->hw_stopped)  {
		rc_out(bp, CD180_CAR, port_No(port));
		port->IER &= ~IER_TXRDY;
		rc_out(bp, CD180_IER, port->IER);
		return;
	}
	
	if (port->break_length)  {
		if (port->break_length > 0)  {
			if (port->COR2 & COR2_ETC)  {
				rc_out(bp, CD180_TDR, CD180_C_ESC);
				rc_out(bp, CD180_TDR, CD180_C_SBRK);
				port->COR2 &= ~COR2_ETC;
			}
			count = MIN(port->break_length, 0xff);
			rc_out(bp, CD180_TDR, CD180_C_ESC);
			rc_out(bp, CD180_TDR, CD180_C_DELAY);
			rc_out(bp, CD180_TDR, count);
			if (!(port->break_length -= count))
				port->break_length--;
		} else  {
			rc_out(bp, CD180_TDR, CD180_C_ESC);
			rc_out(bp, CD180_TDR, CD180_C_EBRK);
			rc_out(bp, CD180_COR2, port->COR2);
			rc_wait_CCR(bp);
			rc_out(bp, CD180_CCR, CCR_CORCHG2);
			port->break_length = 0;
		}
		return;
	}
	
	count = CD180_NFIFO;
	do {
		rc_out(bp, CD180_TDR, port->xmit_buf[port->xmit_tail++]);
		port->xmit_tail = port->xmit_tail & (SERIAL_XMIT_SIZE-1);
		if (--port->xmit_cnt <= 0)
			break;
	} while (--count > 0);
	
	if (port->xmit_cnt <= 0)  {
		rc_out(bp, CD180_CAR, port_No(port));
		port->IER &= ~IER_TXRDY;
		rc_out(bp, CD180_IER, port->IER);
	}
	if (port->xmit_cnt <= port->wakeup_chars)
		rc_mark_event(port, RS_EVENT_WRITE_WAKEUP);
}

static inline void rc_check_modem(struct riscom_board const * bp)
{
	struct riscom_port *port;
	struct tty_struct *tty;
	unsigned char mcr;
	
	if (!(port = rc_get_port(bp, "Modem")))
		return;
	
	tty = port->tty;
	
	mcr = rc_in(bp, CD180_MCR);
	if (mcr & MCR_CDCHG)  {
		if (rc_in(bp, CD180_MSVR) & MSVR_CD) 
			wake_up_interruptible(&port->open_wait);
		else if (!((port->flags & ASYNC_CALLOUT_ACTIVE) &&
			   (port->flags & ASYNC_CALLOUT_NOHUP))) {
			MOD_INC_USE_COUNT;
			if (schedule_task(&port->tqueue_hangup) == 0)
				MOD_DEC_USE_COUNT;
		}
	}
	
#ifdef RISCOM_BRAIN_DAMAGED_CTS
	if (mcr & MCR_CTSCHG)  {
		if (rc_in(bp, CD180_MSVR) & MSVR_CTS)  {
			tty->hw_stopped = 0;
			port->IER |= IER_TXRDY;
			if (port->xmit_cnt <= port->wakeup_chars)
				rc_mark_event(port, RS_EVENT_WRITE_WAKEUP);
		} else  {
			tty->hw_stopped = 1;
			port->IER &= ~IER_TXRDY;
		}
		rc_out(bp, CD180_IER, port->IER);
	}
	if (mcr & MCR_DSRCHG)  {
		if (rc_in(bp, CD180_MSVR) & MSVR_DSR)  {
			tty->hw_stopped = 0;
			port->IER |= IER_TXRDY;
			if (port->xmit_cnt <= port->wakeup_chars)
				rc_mark_event(port, RS_EVENT_WRITE_WAKEUP);
		} else  {
			tty->hw_stopped = 1;
			port->IER &= ~IER_TXRDY;
		}
		rc_out(bp, CD180_IER, port->IER);
	}
#endif /* RISCOM_BRAIN_DAMAGED_CTS */
	
	/* Clear change bits */
	rc_out(bp, CD180_MCR, 0);
}

/* The main interrupt processing routine */
static void rc_interrupt(int irq, void * dev_id, struct pt_regs * regs)
{
	unsigned char status;
	unsigned char ack;
	struct riscom_board *bp;
	unsigned long loop = 0;
	
	bp = IRQ_to_board[irq];
	
	if (!bp || !(bp->flags & RC_BOARD_ACTIVE))  {
		return;
	}
	
	while ((++loop < 16) && ((status = ~(rc_in(bp, RC_BSR))) &
				 (RC_BSR_TOUT | RC_BSR_TINT |
				  RC_BSR_MINT | RC_BSR_RINT))) {
	
		if (status & RC_BSR_TOUT) 
			printk("rc%d: Got timeout. Hardware error ?\n", board_No(bp));
		
		else if (status & RC_BSR_RINT) {
			ack = rc_in(bp, RC_ACK_RINT);
		
			if (ack == (RC_ID | GIVR_IT_RCV))
				rc_receive(bp);
			else if (ack == (RC_ID | GIVR_IT_REXC))
				rc_receive_exc(bp);
			else
				printk("rc%d: Bad receive ack 0x%02x.\n",
				       board_No(bp), ack);
		
		} else if (status & RC_BSR_TINT) {
			ack = rc_in(bp, RC_ACK_TINT);
		
			if (ack == (RC_ID | GIVR_IT_TX))
				rc_transmit(bp);
			else
				printk("rc%d: Bad transmit ack 0x%02x.\n",
				       board_No(bp), ack);
		
		} else /* if (status & RC_BSR_MINT) */ {
			ack = rc_in(bp, RC_ACK_MINT);
		
			if (ack == (RC_ID | GIVR_IT_MODEM)) 
				rc_check_modem(bp);
			else
				printk("rc%d: Bad modem ack 0x%02x.\n",
				       board_No(bp), ack);
		
		} 

		rc_out(bp, CD180_EOIR, 0);   /* Mark end of interrupt */
		rc_out(bp, RC_CTOUT, 0);     /* Clear timeout flag    */
	}
}

/*
 *  Routines for open & close processing.
 */

/* Called with disabled interrupts */
static inline int rc_setup_board(struct riscom_board * bp)
{
	int error;

	if (bp->flags & RC_BOARD_ACTIVE) 
		return 0;
	
	error = request_irq(bp->irq, rc_interrupt, SA_INTERRUPT, "RISCom/8", NULL);
	if (error) 
		return error;
	
	rc_out(bp, RC_CTOUT, 0);       		/* Just in case         */
	bp->DTR = ~0;
	rc_out(bp, RC_DTR, bp->DTR);	        /* Drop DTR on all ports */
	
	IRQ_to_board[bp->irq] = bp;
	bp->flags |= RC_BOARD_ACTIVE;
	
	MOD_INC_USE_COUNT;
	return 0;
}

/* Called with disabled interrupts */
static inline void rc_shutdown_board(struct riscom_board *bp)
{
	if (!(bp->flags & RC_BOARD_ACTIVE))
		return;
	
	bp->flags &= ~RC_BOARD_ACTIVE;
	
	free_irq(bp->irq, NULL);
	IRQ_to_board[bp->irq] = NULL;
	
	bp->DTR = ~0;
	rc_out(bp, RC_DTR, bp->DTR);	       /* Drop DTR on all ports */
	
	MOD_DEC_USE_COUNT;
}

/*
 * Setting up port characteristics. 
 * Must be called with disabled interrupts
 */
static void rc_change_speed(struct riscom_board *bp, struct riscom_port *port)
{
	struct tty_struct *tty;
	unsigned long baud;
	long tmp;
	unsigned char cor1 = 0, cor3 = 0;
	unsigned char mcor1 = 0, mcor2 = 0;
	
	if (!(tty = port->tty) || !tty->termios)
		return;

	port->IER  = 0;
	port->COR2 = 0;
	port->MSVR = MSVR_RTS;
	
	baud = C_BAUD(tty);
	
	if (baud & CBAUDEX) {
		baud &= ~CBAUDEX;
		if (baud < 1 || baud > 2) 
			port->tty->termios->c_cflag &= ~CBAUDEX;
		else
			baud += 15;
	}
	if (baud == 15)  {
		if ((port->flags & ASYNC_SPD_MASK) == ASYNC_SPD_HI)
			baud ++;
		if ((port->flags & ASYNC_SPD_MASK) == ASYNC_SPD_VHI)
			baud += 2;
	}
	
	/* Select port on the board */
	rc_out(bp, CD180_CAR, port_No(port));
	
	if (!baud_table[baud])  {
		/* Drop DTR & exit */
		bp->DTR |= (1u << port_No(port));
		rc_out(bp, RC_DTR, bp->DTR);
		return;
	} else  {
		/* Set DTR on */
		bp->DTR &= ~(1u << port_No(port));
		rc_out(bp, RC_DTR, bp->DTR);
	}
	
	/*
	 * Now we must calculate some speed depended things 
	 */
	
	/* Set baud rate for port */
	tmp = (((RC_OSCFREQ + baud_table[baud]/2) / baud_table[baud] +
		CD180_TPC/2) / CD180_TPC);

	rc_out(bp, CD180_RBPRH, (tmp >> 8) & 0xff); 
	rc_out(bp, CD180_TBPRH, (tmp >> 8) & 0xff); 
	rc_out(bp, CD180_RBPRL, tmp & 0xff); 
	rc_out(bp, CD180_TBPRL, tmp & 0xff);
	
	baud = (baud_table[baud] + 5) / 10;   /* Estimated CPS */
	
	/* Two timer ticks seems enough to wakeup something like SLIP driver */
	tmp = ((baud + HZ/2) / HZ) * 2 - CD180_NFIFO;		
	port->wakeup_chars = (tmp < 0) ? 0 : ((tmp >= SERIAL_XMIT_SIZE) ?
					      SERIAL_XMIT_SIZE - 1 : tmp);
	
	/* Receiver timeout will be transmission time for 1.5 chars */
	tmp = (RISCOM_TPS + RISCOM_TPS/2 + baud/2) / baud;
	tmp = (tmp > 0xff) ? 0xff : tmp;
	rc_out(bp, CD180_RTPR, tmp);
	
	switch (C_CSIZE(tty))  {
	 case CS5:
		cor1 |= COR1_5BITS;
		break;
	 case CS6:
		cor1 |= COR1_6BITS;
		break;
	 case CS7:
		cor1 |= COR1_7BITS;
		break;
	 case CS8:
		cor1 |= COR1_8BITS;
		break;
	}
	
	if (C_CSTOPB(tty)) 
		cor1 |= COR1_2SB;
	
	cor1 |= COR1_IGNORE;
	if (C_PARENB(tty))  {
		cor1 |= COR1_NORMPAR;
		if (C_PARODD(tty)) 
			cor1 |= COR1_ODDP;
		if (I_INPCK(tty)) 
			cor1 &= ~COR1_IGNORE;
	}
	/* Set marking of some errors */
	port->mark_mask = RCSR_OE | RCSR_TOUT;
	if (I_INPCK(tty)) 
		port->mark_mask |= RCSR_FE | RCSR_PE;
	if (I_BRKINT(tty) || I_PARMRK(tty)) 
		port->mark_mask |= RCSR_BREAK;
	if (I_IGNPAR(tty)) 
		port->mark_mask &= ~(RCSR_FE | RCSR_PE);
	if (I_IGNBRK(tty))  {
		port->mark_mask &= ~RCSR_BREAK;
		if (I_IGNPAR(tty)) 
			/* Real raw mode. Ignore all */
			port->mark_mask &= ~RCSR_OE;
	}
	/* Enable Hardware Flow Control */
	if (C_CRTSCTS(tty))  {
#ifdef RISCOM_BRAIN_DAMAGED_CTS
		port->IER |= IER_DSR | IER_CTS;
		mcor1 |= MCOR1_DSRZD | MCOR1_CTSZD;
		mcor2 |= MCOR2_DSROD | MCOR2_CTSOD;
		tty->hw_stopped = !(rc_in(bp, CD180_MSVR) & (MSVR_CTS|MSVR_DSR));
#else
		port->COR2 |= COR2_CTSAE;
#endif
	}
	/* Enable Software Flow Control. FIXME: I'm not sure about this */
	/* Some people reported that it works, but I still doubt */
	if (I_IXON(tty))  {
		port->COR2 |= COR2_TXIBE;
		cor3 |= (COR3_FCT | COR3_SCDE);
		if (I_IXANY(tty))
			port->COR2 |= COR2_IXM;
		rc_out(bp, CD180_SCHR1, START_CHAR(tty));
		rc_out(bp, CD180_SCHR2, STOP_CHAR(tty));
		rc_out(bp, CD180_SCHR3, START_CHAR(tty));
		rc_out(bp, CD180_SCHR4, STOP_CHAR(tty));
	}
	if (!C_CLOCAL(tty))  {
		/* Enable CD check */
		port->IER |= IER_CD;
		mcor1 |= MCOR1_CDZD;
		mcor2 |= MCOR2_CDOD;
	}
	
	if (C_CREAD(tty)) 
		/* Enable receiver */
		port->IER |= IER_RXD;
	
	/* Set input FIFO size (1-8 bytes) */
	cor3 |= RISCOM_RXFIFO; 
	/* Setting up CD180 channel registers */
	rc_out(bp, CD180_COR1, cor1);
	rc_out(bp, CD180_COR2, port->COR2);
	rc_out(bp, CD180_COR3, cor3);
	/* Make CD180 know about registers change */
	rc_wait_CCR(bp);
	rc_out(bp, CD180_CCR, CCR_CORCHG1 | CCR_CORCHG2 | CCR_CORCHG3);
	/* Setting up modem option registers */
	rc_out(bp, CD180_MCOR1, mcor1);
	rc_out(bp, CD180_MCOR2, mcor2);
	/* Enable CD180 transmitter & receiver */
	rc_wait_CCR(bp);
	rc_out(bp, CD180_CCR, CCR_TXEN | CCR_RXEN);
	/* Enable interrupts */
	rc_out(bp, CD180_IER, port->IER);
	/* And finally set RTS on */
	rc_out(bp, CD180_MSVR, port->MSVR);
}

/* Must be called with interrupts enabled */
static int rc_setup_port(struct riscom_board *bp, struct riscom_port *port)
{
	unsigned long flags;
	
	if (port->flags & ASYNC_INITIALIZED)
		return 0;
	
	if (!port->xmit_buf) {
		/* We may sleep in get_free_page() */
		unsigned long tmp;
		
		if (!(tmp = get_free_page(GFP_KERNEL)))
			return -ENOMEM;
		    
		if (port->xmit_buf) {
			free_page(tmp);
			return -ERESTARTSYS;
		}
		port->xmit_buf = (unsigned char *) tmp;
	}
		
	save_flags(flags); cli();
		
	if (port->tty) 
		clear_bit(TTY_IO_ERROR, &port->tty->flags);
		
	if (port->count == 1) 
		bp->count++;
		
	port->xmit_cnt = port->xmit_head = port->xmit_tail = 0;
	rc_change_speed(bp, port);
	port->flags |= ASYNC_INITIALIZED;
		
	restore_flags(flags);
	return 0;
}

/* Must be called with interrupts disabled */
static void rc_shutdown_port(struct riscom_board *bp, struct riscom_port *port)
{
	struct tty_struct *tty;
	
	if (!(port->flags & ASYNC_INITIALIZED)) 
		return;
	
#ifdef RC_REPORT_OVERRUN
	printk("rc%d: port %d: Total %ld overruns were detected.\n",
	       board_No(bp), port_No(port), port->overrun);
#endif	
#ifdef RC_REPORT_FIFO
	{
		int i;
		
		printk("rc%d: port %d: FIFO hits [ ",
		       board_No(bp), port_No(port));
		for (i = 0; i < 10; i++)  {
			printk("%ld ", port->hits[i]);
		}
		printk("].\n");
	}
#endif	
	if (port->xmit_buf)  {
		free_page((unsigned long) port->xmit_buf);
		port->xmit_buf = NULL;
	}

	if (!(tty = port->tty) || C_HUPCL(tty))  {
		/* Drop DTR */
		bp->DTR |= (1u << port_No(port));
		rc_out(bp, RC_DTR, bp->DTR);
	}
	
        /* Select port */
	rc_out(bp, CD180_CAR, port_No(port));
	/* Reset port */
	rc_wait_CCR(bp);
	rc_out(bp, CD180_CCR, CCR_SOFTRESET);
	/* Disable all interrupts from this port */
	port->IER = 0;
	rc_out(bp, CD180_IER, port->IER);
	
	if (tty)  
		set_bit(TTY_IO_ERROR, &tty->flags);
	port->flags &= ~ASYNC_INITIALIZED;
	
	if (--bp->count < 0)  {
		printk("rc%d: rc_shutdown_port: bad board count: %d\n",
		       board_No(bp), bp->count);
		bp->count = 0;
	}
	
	/*
	 * If this is the last opened port on the board
	 * shutdown whole board
	 */
	if (!bp->count) 
		rc_shutdown_board(bp);
}

	
static int block_til_ready(struct tty_struct *tty, struct file * filp,
			   struct riscom_port *port)
{
	DECLARE_WAITQUEUE(wait, current);
	struct riscom_board *bp = port_Board(port);
	int    retval;
	int    do_clocal = 0;
	int    CD;

	/*
	 * If the device is in the middle of being closed, then block
	 * until it's done, and then try again.
	 */
	if (tty_hung_up_p(filp) || port->flags & ASYNC_CLOSING) {
		interruptible_sleep_on(&port->close_wait);
		if (port->flags & ASYNC_HUP_NOTIFY)
			return -EAGAIN;
		else
			return -ERESTARTSYS;
	}

	/*
	 * If this is a callout device, then just make sure the normal
	 * device isn't being used.
	 */
	if (tty->driver.subtype == RISCOM_TYPE_CALLOUT) {
		if (port->flags & ASYNC_NORMAL_ACTIVE)
			return -EBUSY;
		if ((port->flags & ASYNC_CALLOUT_ACTIVE) &&
		    (port->flags & ASYNC_SESSION_LOCKOUT) &&
		    (port->session != current->session))
		    return -EBUSY;
		if ((port->flags & ASYNC_CALLOUT_ACTIVE) &&
		    (port->flags & ASYNC_PGRP_LOCKOUT) &&
		    (port->pgrp != current->pgrp))
		    return -EBUSY;
		port->flags |= ASYNC_CALLOUT_ACTIVE;
		return 0;
	}
	
	/*
	 * If non-blocking mode is set, or the port is not enabled,
	 * then make the check up front and then exit.
	 */
	if ((filp->f_flags & O_NONBLOCK) ||
	    (tty->flags & (1 << TTY_IO_ERROR))) {
		if (port->flags & ASYNC_CALLOUT_ACTIVE)
			return -EBUSY;
		port->flags |= ASYNC_NORMAL_ACTIVE;
		return 0;
	}

	if (port->flags & ASYNC_CALLOUT_ACTIVE) {
		if (port->normal_termios.c_cflag & CLOCAL) 
			do_clocal = 1;
	} else {
		if (C_CLOCAL(tty))  
			do_clocal = 1;
	}
	
	/*
	 * Block waiting for the carrier detect and the line to become
	 * free (i.e., not in use by the callout).  While we are in
	 * this loop, info->count is dropped by one, so that
	 * rs_close() knows when to free things.  We restore it upon
	 * exit, either normal or abnormal.
	 */
	retval = 0;
	add_wait_queue(&port->open_wait, &wait);
	cli();
	if (!tty_hung_up_p(filp))
		port->count--;
	sti();
	port->blocked_open++;
	while (1) {
		cli();
		rc_out(bp, CD180_CAR, port_No(port));
		CD = rc_in(bp, CD180_MSVR) & MSVR_CD;
		if (!(port->flags & ASYNC_CALLOUT_ACTIVE))  {
			rc_out(bp, CD180_MSVR, MSVR_RTS);
			bp->DTR &= ~(1u << port_No(port));
			rc_out(bp, RC_DTR, bp->DTR);
		}
		sti();
		set_current_state(TASK_INTERRUPTIBLE);
		if (tty_hung_up_p(filp) ||
		    !(port->flags & ASYNC_INITIALIZED)) {
			if (port->flags & ASYNC_HUP_NOTIFY)
				retval = -EAGAIN;
			else
				retval = -ERESTARTSYS;	
			break;
		}
		if (!(port->flags & ASYNC_CALLOUT_ACTIVE) &&
		    !(port->flags & ASYNC_CLOSING) &&
		    (do_clocal || CD))
			break;
		if (signal_pending(current)) {
			retval = -ERESTARTSYS;
			break;
		}
		schedule();
	}
	current->state = TASK_RUNNING;
	remove_wait_queue(&port->open_wait, &wait);
	if (!tty_hung_up_p(filp))
		port->count++;
	port->blocked_open--;
	if (retval)
		return retval;
	
	port->flags |= ASYNC_NORMAL_ACTIVE;
	return 0;
}	

static int rc_open(struct tty_struct * tty, struct file * filp)
{
	int board;
	int error;
	struct riscom_port * port;
	struct riscom_board * bp;
	unsigned long flags;
	
	board = RC_BOARD(MINOR(tty->device));
	if (board > RC_NBOARD || !(rc_board[board].flags & RC_BOARD_PRESENT))
		return -ENODEV;
	
	bp = &rc_board[board];
	port = rc_port + board * RC_NPORT + RC_PORT(MINOR(tty->device));
	if (rc_paranoia_check(port, tty->device, "rc_open"))
		return -ENODEV;
	
	if ((error = rc_setup_board(bp))) 
		return error;
		
	port->count++;
	tty->driver_data = port;
	port->tty = tty;
	
	if ((error = rc_setup_port(bp, port))) 
		return error;
	
	if ((error = block_til_ready(tty, filp, port)))
		return error;
	
	if ((port->count == 1) && (port->flags & ASYNC_SPLIT_TERMIOS)) {
		if (tty->driver.subtype == RISCOM_TYPE_NORMAL)
			*tty->termios = port->normal_termios;
		else
			*tty->termios = port->callout_termios;
		save_flags(flags); cli();
		rc_change_speed(bp, port);
		restore_flags(flags);
	}

	port->session = current->session;
	port->pgrp = current->pgrp;
	
	return 0;
}

static void rc_close(struct tty_struct * tty, struct file * filp)
{
	struct riscom_port *port = (struct riscom_port *) tty->driver_data;
	struct riscom_board *bp;
	unsigned long flags;
	unsigned long timeout;
	
	if (!port || rc_paranoia_check(port, tty->device, "close"))
		return;
	
	save_flags(flags); cli();
	if (tty_hung_up_p(filp))  {
		restore_flags(flags);
		return;
	}
	
	bp = port_Board(port);
	if ((tty->count == 1) && (port->count != 1))  {
		printk("rc%d: rc_close: bad port count;"
		       " tty->count is 1, port count is %d\n",
		       board_No(bp), port->count);
		port->count = 1;
	}
	if (--port->count < 0)  {
		printk("rc%d: rc_close: bad port count for tty%d: %d\n",
		       board_No(bp), port_No(port), port->count);
		port->count = 0;
	}
	if (port->count)  {
		restore_flags(flags);
		return;
	}
	port->flags |= ASYNC_CLOSING;
	/*
	 * Save the termios structure, since this port may have
	 * separate termios for callout and dialin.
	 */
	if (port->flags & ASYNC_NORMAL_ACTIVE)
		port->normal_termios = *tty->termios;
	if (port->flags & ASYNC_CALLOUT_ACTIVE)
		port->callout_termios = *tty->termios;
	/*
	 * Now we wait for the transmit buffer to clear; and we notify 
	 * the line discipline to only process XON/XOFF characters.
	 */
	tty->closing = 1;
	if (port->closing_wait != ASYNC_CLOSING_WAIT_NONE)
		tty_wait_until_sent(tty, port->closing_wait);
	/*
	 * At this point we stop accepting input.  To do this, we
	 * disable the receive line status interrupts, and tell the
	 * interrupt driver to stop checking the data ready bit in the
	 * line status register.
	 */
	port->IER &= ~IER_RXD;
	if (port->flags & ASYNC_INITIALIZED) {
		port->IER &= ~IER_TXRDY;
		port->IER |= IER_TXEMPTY;
		rc_out(bp, CD180_CAR, port_No(port));
		rc_out(bp, CD180_IER, port->IER);
		/*
		 * Before we drop DTR, make sure the UART transmitter
		 * has completely drained; this is especially
		 * important if there is a transmit FIFO!
		 */
		timeout = jiffies+HZ;
		while(port->IER & IER_TXEMPTY)  {
			current->state = TASK_INTERRUPTIBLE;
 			schedule_timeout(port->timeout);
			if (time_after(jiffies, timeout))
				break;
		}
	}
	rc_shutdown_port(bp, port);
	if (tty->driver.flush_buffer)
		tty->driver.flush_buffer(tty);
	if (tty->ldisc.flush_buffer)
		tty->ldisc.flush_buffer(tty);
	tty->closing = 0;
	port->event = 0;
	port->tty = 0;
	if (port->blocked_open) {
		if (port->close_delay) {
			current->state = TASK_INTERRUPTIBLE;
			schedule_timeout(port->close_delay);
		}
		wake_up_interruptible(&port->open_wait);
	}
	port->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CALLOUT_ACTIVE|
			 ASYNC_CLOSING);
	wake_up_interruptible(&port->close_wait);
	restore_flags(flags);
}

static int rc_write(struct tty_struct * tty, int from_user, 
		    const unsigned char *buf, int count)
{
	struct riscom_port *port = (struct riscom_port *)tty->driver_data;
	struct riscom_board *bp;
	int c, total = 0;
	unsigned long flags;
				
	if (rc_paranoia_check(port, tty->device, "rc_write"))
		return 0;
	
	bp = port_Board(port);

	if (!tty || !port->xmit_buf || !tmp_buf)
		return 0;

	if (from_user)
		down(&tmp_buf_sem);

	save_flags(flags);
	while (1) {
		cli();		
		c = MIN(count, MIN(SERIAL_XMIT_SIZE - port->xmit_cnt - 1,
				   SERIAL_XMIT_SIZE - port->xmit_head));
		if (c <= 0)
			break;

		if (from_user) {
			copy_from_user(tmp_buf, buf, c);
			c = MIN(c, MIN(SERIAL_XMIT_SIZE - port->xmit_cnt - 1,
				       SERIAL_XMIT_SIZE - port->xmit_head));
			memcpy(port->xmit_buf + port->xmit_head, tmp_buf, c);
		} else
			memcpy(port->xmit_buf + port->xmit_head, buf, c);
		port->xmit_head = (port->xmit_head + c) & (SERIAL_XMIT_SIZE-1);
		port->xmit_cnt += c;
		restore_flags(flags);
		buf += c;
		count -= c;
		total += c;
	}
	if (from_user)
		up(&tmp_buf_sem);
	if (port->xmit_cnt && !tty->stopped && !tty->hw_stopped &&
	    !(port->IER & IER_TXRDY)) {
		port->IER |= IER_TXRDY;
		rc_out(bp, CD180_CAR, port_No(port));
		rc_out(bp, CD180_IER, port->IER);
	}
	restore_flags(flags);
	return total;
}

static void rc_put_char(struct tty_struct * tty, unsigned char ch)
{
	struct riscom_port *port = (struct riscom_port *)tty->driver_data;
	unsigned long flags;

	if (rc_paranoia_check(port, tty->device, "rc_put_char"))
		return;

	if (!tty || !port->xmit_buf)
		return;

	save_flags(flags); cli();
	
	if (port->xmit_cnt >= SERIAL_XMIT_SIZE - 1) {
		restore_flags(flags);
		return;
	}

	port->xmit_buf[port->xmit_head++] = ch;
	port->xmit_head &= SERIAL_XMIT_SIZE - 1;
	port->xmit_cnt++;
	restore_flags(flags);
}

static void rc_flush_chars(struct tty_struct * tty)
{
	struct riscom_port *port = (struct riscom_port *)tty->driver_data;
	unsigned long flags;
				
	if (rc_paranoia_check(port, tty->device, "rc_flush_chars"))
		return;
	
	if (port->xmit_cnt <= 0 || tty->stopped || tty->hw_stopped ||
	    !port->xmit_buf)
		return;

	save_flags(flags); cli();
	port->IER |= IER_TXRDY;
	rc_out(port_Board(port), CD180_CAR, port_No(port));
	rc_out(port_Board(port), CD180_IER, port->IER);
	restore_flags(flags);
}

static int rc_write_room(struct tty_struct * tty)
{
	struct riscom_port *port = (struct riscom_port *)tty->driver_data;
	int	ret;
				
	if (rc_paranoia_check(port, tty->device, "rc_write_room"))
		return 0;

	ret = SERIAL_XMIT_SIZE - port->xmit_cnt - 1;
	if (ret < 0)
		ret = 0;
	return ret;
}

static int rc_chars_in_buffer(struct tty_struct *tty)
{
	struct riscom_port *port = (struct riscom_port *)tty->driver_data;
				
	if (rc_paranoia_check(port, tty->device, "rc_chars_in_buffer"))
		return 0;
	
	return port->xmit_cnt;
}

static void rc_flush_buffer(struct tty_struct *tty)
{
	struct riscom_port *port = (struct riscom_port *)tty->driver_data;
	unsigned long flags;
				
	if (rc_paranoia_check(port, tty->device, "rc_flush_buffer"))
		return;

	save_flags(flags); cli();
	port->xmit_cnt = port->xmit_head = port->xmit_tail = 0;
	restore_flags(flags);
	
	wake_up_interruptible(&tty->write_wait);
	if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
	    tty->ldisc.write_wakeup)
		(tty->ldisc.write_wakeup)(tty);
}

static int rc_get_modem_info(struct riscom_port * port, unsigned int *value)
{
	struct riscom_board * bp;
	unsigned char status;
	unsigned int result;
	unsigned long flags;

	bp = port_Board(port);
	save_flags(flags); cli();
	rc_out(bp, CD180_CAR, port_No(port));
	status = rc_in(bp, CD180_MSVR);
	result = rc_in(bp, RC_RI) & (1u << port_No(port)) ? 0 : TIOCM_RNG;
	restore_flags(flags);
	result |= ((status & MSVR_RTS) ? TIOCM_RTS : 0)
		| ((status & MSVR_DTR) ? TIOCM_DTR : 0)
		| ((status & MSVR_CD)  ? TIOCM_CAR : 0)
		| ((status & MSVR_DSR) ? TIOCM_DSR : 0)
		| ((status & MSVR_CTS) ? TIOCM_CTS : 0);
	put_user(result, value);
	return 0;
}

static int rc_set_modem_info(struct riscom_port * port, unsigned int cmd,
			     unsigned int *value)
{
	int error;
	unsigned int arg;
	unsigned long flags;
	struct riscom_board *bp = port_Board(port);

	error = get_user(arg, value);
	if (error) 
		return error;
	switch (cmd) {
	 case TIOCMBIS: 
		if (arg & TIOCM_RTS) 
			port->MSVR |= MSVR_RTS;
		if (arg & TIOCM_DTR)
			bp->DTR &= ~(1u << port_No(port));
		break;
	case TIOCMBIC:
		if (arg & TIOCM_RTS)
			port->MSVR &= ~MSVR_RTS;
		if (arg & TIOCM_DTR)
			bp->DTR |= (1u << port_No(port));
		break;
	case TIOCMSET:
		port->MSVR = (arg & TIOCM_RTS) ? (port->MSVR | MSVR_RTS) : 
					         (port->MSVR & ~MSVR_RTS);
		bp->DTR = arg & TIOCM_DTR ? (bp->DTR &= ~(1u << port_No(port))) :
					    (bp->DTR |=  (1u << port_No(port)));
		break;
	 default:
		return -EINVAL;
	}
	save_flags(flags); cli();
	rc_out(bp, CD180_CAR, port_No(port));
	rc_out(bp, CD180_MSVR, port->MSVR);
	rc_out(bp, RC_DTR, bp->DTR);
	restore_flags(flags);
	return 0;
}

static inline void rc_send_break(struct riscom_port * port, unsigned long length)
{
	struct riscom_board *bp = port_Board(port);
	unsigned long flags;
	
	save_flags(flags); cli();
	port->break_length = RISCOM_TPS / HZ * length;
	port->COR2 |= COR2_ETC;
	port->IER  |= IER_TXRDY;
	rc_out(bp, CD180_CAR, port_No(port));
	rc_out(bp, CD180_COR2, port->COR2);
	rc_out(bp, CD180_IER, port->IER);
	rc_wait_CCR(bp);
	rc_out(bp, CD180_CCR, CCR_CORCHG2);
	rc_wait_CCR(bp);
	restore_flags(flags);
}

static inline int rc_set_serial_info(struct riscom_port * port,
				     struct serial_struct * newinfo)
{
	struct serial_struct tmp;
	struct riscom_board *bp = port_Board(port);
	int change_speed;
	unsigned long flags;
	int error;
	
	error = verify_area(VERIFY_READ, (void *) newinfo, sizeof(tmp));
	if (error)
		return error;
	copy_from_user(&tmp, newinfo, sizeof(tmp));
	
#if 0	
	if ((tmp.irq != bp->irq) ||
	    (tmp.port != bp->base) ||
	    (tmp.type != PORT_CIRRUS) ||
	    (tmp.baud_base != (RC_OSCFREQ + CD180_TPC/2) / CD180_TPC) ||
	    (tmp.custom_divisor != 0) ||
	    (tmp.xmit_fifo_size != CD180_NFIFO) ||
	    (tmp.flags & ~RISCOM_LEGAL_FLAGS))
		return -EINVAL;
#endif	
	
	change_speed = ((port->flags & ASYNC_SPD_MASK) !=
			(tmp.flags & ASYNC_SPD_MASK));
	
	if (!capable(CAP_SYS_ADMIN)) {
		if ((tmp.close_delay != port->close_delay) ||
		    (tmp.closing_wait != port->closing_wait) ||
		    ((tmp.flags & ~ASYNC_USR_MASK) !=
		     (port->flags & ~ASYNC_USR_MASK)))  
			return -EPERM;
		port->flags = ((port->flags & ~ASYNC_USR_MASK) |
			       (tmp.flags & ASYNC_USR_MASK));
	} else  {
		port->flags = ((port->flags & ~ASYNC_FLAGS) |
			       (tmp.flags & ASYNC_FLAGS));
		port->close_delay = tmp.close_delay;
		port->closing_wait = tmp.closing_wait;
	}
	if (change_speed)  {
		save_flags(flags); cli();
		rc_change_speed(bp, port);
		restore_flags(flags);
	}
	return 0;
}

static inline int rc_get_serial_info(struct riscom_port * port,
				     struct serial_struct * retinfo)
{
	struct serial_struct tmp;
	struct riscom_board *bp = port_Board(port);
	int error;
	
	error = verify_area(VERIFY_WRITE, (void *) retinfo, sizeof(tmp));
	if (error)
		return error;
	
	memset(&tmp, 0, sizeof(tmp));
	tmp.type = PORT_CIRRUS;
	tmp.line = port - rc_port;
	tmp.port = bp->base;
	tmp.irq  = bp->irq;
	tmp.flags = port->flags;
	tmp.baud_base = (RC_OSCFREQ + CD180_TPC/2) / CD180_TPC;
	tmp.close_delay = port->close_delay * HZ/100;
	tmp.closing_wait = port->closing_wait * HZ/100;
	tmp.xmit_fifo_size = CD180_NFIFO;
	copy_to_user(retinfo, &tmp, sizeof(tmp));
	return 0;
}

static int rc_ioctl(struct tty_struct * tty, struct file * filp, 
		    unsigned int cmd, unsigned long arg)
		    
{
	struct riscom_port *port = (struct riscom_port *)tty->driver_data;
	int error;
	int retval;
				
	if (rc_paranoia_check(port, tty->device, "rc_ioctl"))
		return -ENODEV;
	
	switch (cmd) {
	 case TCSBRK:	/* SVID version: non-zero arg --> no break */
		retval = tty_check_change(tty);
		if (retval)
			return retval;
		tty_wait_until_sent(tty, 0);
		if (!arg)
			rc_send_break(port, HZ/4);	/* 1/4 second */
		return 0;
	 case TCSBRKP:	/* support for POSIX tcsendbreak() */
		retval = tty_check_change(tty);
		if (retval)
			return retval;
		tty_wait_until_sent(tty, 0);
		rc_send_break(port, arg ? arg*(HZ/10) : HZ/4);
		return 0;
	 case TIOCGSOFTCAR:
		return put_user(C_CLOCAL(tty) ? 1 : 0, (unsigned int *) arg);
	 case TIOCSSOFTCAR:
		retval = get_user(arg,(unsigned int *) arg);
		if (retval)
			return retval;
		tty->termios->c_cflag =
			((tty->termios->c_cflag & ~CLOCAL) |
			(arg ? CLOCAL : 0));
		return 0;
	 case TIOCMGET:
		error = verify_area(VERIFY_WRITE, (void *) arg,
				    sizeof(unsigned int));
		if (error)
			return error;
		return rc_get_modem_info(port, (unsigned int *) arg);
	 case TIOCMBIS:
	 case TIOCMBIC:
	 case TIOCMSET:
		return rc_set_modem_info(port, cmd, (unsigned int *) arg);
	 case TIOCGSERIAL:	
		return rc_get_serial_info(port, (struct serial_struct *) arg);
	 case TIOCSSERIAL:	
		return rc_set_serial_info(port, (struct serial_struct *) arg);
	 default:
		return -ENOIOCTLCMD;
	}
	return 0;
}

static void rc_throttle(struct tty_struct * tty)
{
	struct riscom_port *port = (struct riscom_port *)tty->driver_data;
	struct riscom_board *bp;
	unsigned long flags;
				
	if (rc_paranoia_check(port, tty->device, "rc_throttle"))
		return;
	
	bp = port_Board(port);
	
	save_flags(flags); cli();
	port->MSVR &= ~MSVR_RTS;
	rc_out(bp, CD180_CAR, port_No(port));
	if (I_IXOFF(tty))  {
		rc_wait_CCR(bp);
		rc_out(bp, CD180_CCR, CCR_SSCH2);
		rc_wait_CCR(bp);
	}
	rc_out(bp, CD180_MSVR, port->MSVR);
	restore_flags(flags);
}

static void rc_unthrottle(struct tty_struct * tty)
{
	struct riscom_port *port = (struct riscom_port *)tty->driver_data;
	struct riscom_board *bp;
	unsigned long flags;
				
	if (rc_paranoia_check(port, tty->device, "rc_unthrottle"))
		return;
	
	bp = port_Board(port);
	
	save_flags(flags); cli();
	port->MSVR |= MSVR_RTS;
	rc_out(bp, CD180_CAR, port_No(port));
	if (I_IXOFF(tty))  {
		rc_wait_CCR(bp);
		rc_out(bp, CD180_CCR, CCR_SSCH1);
		rc_wait_CCR(bp);
	}
	rc_out(bp, CD180_MSVR, port->MSVR);
	restore_flags(flags);
}

static void rc_stop(struct tty_struct * tty)
{
	struct riscom_port *port = (struct riscom_port *)tty->driver_data;
	struct riscom_board *bp;
	unsigned long flags;
				
	if (rc_paranoia_check(port, tty->device, "rc_stop"))
		return;
	
	bp = port_Board(port);
	
	save_flags(flags); cli();
	port->IER &= ~IER_TXRDY;
	rc_out(bp, CD180_CAR, port_No(port));
	rc_out(bp, CD180_IER, port->IER);
	restore_flags(flags);
}

static void rc_start(struct tty_struct * tty)
{
	struct riscom_port *port = (struct riscom_port *)tty->driver_data;
	struct riscom_board *bp;
	unsigned long flags;
				
	if (rc_paranoia_check(port, tty->device, "rc_start"))
		return;
	
	bp = port_Board(port);
	
	save_flags(flags); cli();
	if (port->xmit_cnt && port->xmit_buf && !(port->IER & IER_TXRDY))  {
		port->IER |= IER_TXRDY;
		rc_out(bp, CD180_CAR, port_No(port));
		rc_out(bp, CD180_IER, port->IER);
	}
	restore_flags(flags);
}

/*
 * This routine is called from the scheduler tqueue when the interrupt
 * routine has signalled that a hangup has occurred.  The path of
 * hangup processing is:
 *
 * 	serial interrupt routine -> (scheduler tqueue) ->
 * 	do_rc_hangup() -> tty->hangup() -> rc_hangup()
 * 
 */
static void do_rc_hangup(void *private_)
{
	struct riscom_port	*port = (struct riscom_port *) private_;
	struct tty_struct	*tty;
	
	tty = port->tty;
	if (tty)
		tty_hangup(tty);	/* FIXME: module removal race still here */
	MOD_DEC_USE_COUNT;
}

static void rc_hangup(struct tty_struct * tty)
{
	struct riscom_port *port = (struct riscom_port *)tty->driver_data;
	struct riscom_board *bp;
				
	if (rc_paranoia_check(port, tty->device, "rc_hangup"))
		return;
	
	bp = port_Board(port);
	
	rc_shutdown_port(bp, port);
	port->event = 0;
	port->count = 0;
	port->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CALLOUT_ACTIVE);
	port->tty = 0;
	wake_up_interruptible(&port->open_wait);
}

static void rc_set_termios(struct tty_struct * tty, struct termios * old_termios)
{
	struct riscom_port *port = (struct riscom_port *)tty->driver_data;
	unsigned long flags;
				
	if (rc_paranoia_check(port, tty->device, "rc_set_termios"))
		return;
	
	if (tty->termios->c_cflag == old_termios->c_cflag &&
	    tty->termios->c_iflag == old_termios->c_iflag)
		return;

	save_flags(flags); cli();
	rc_change_speed(port_Board(port), port);
	restore_flags(flags);

	if ((old_termios->c_cflag & CRTSCTS) &&
	    !(tty->termios->c_cflag & CRTSCTS)) {
		tty->hw_stopped = 0;
		rc_start(tty);
	}
}

static void do_riscom_bh(void)
{
	 run_task_queue(&tq_riscom);
}

static void do_softint(void *private_)
{
	struct riscom_port	*port = (struct riscom_port *) private_;
	struct tty_struct	*tty;
	
	if(!(tty = port->tty)) 
		return;

	if (test_and_clear_bit(RS_EVENT_WRITE_WAKEUP, &port->event)) {
		if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
		    tty->ldisc.write_wakeup)
			(tty->ldisc.write_wakeup)(tty);
		wake_up_interruptible(&tty->write_wait);
	}
}

static inline int rc_init_drivers(void)
{
	int error;
	int i;

	
	if (!(tmp_buf = (unsigned char *) get_free_page(GFP_KERNEL))) {
		printk("rc: Couldn't get free page.\n");
		return 1;
	}
	init_bh(RISCOM8_BH, do_riscom_bh);
	memset(IRQ_to_board, 0, sizeof(IRQ_to_board));
	memset(&riscom_driver, 0, sizeof(riscom_driver));
	riscom_driver.magic = TTY_DRIVER_MAGIC;
	riscom_driver.name = "ttyL";
	riscom_driver.major = RISCOM8_NORMAL_MAJOR;
	riscom_driver.num = RC_NBOARD * RC_NPORT;
	riscom_driver.type = TTY_DRIVER_TYPE_SERIAL;
	riscom_driver.subtype = RISCOM_TYPE_NORMAL;
	riscom_driver.init_termios = tty_std_termios;
	riscom_driver.init_termios.c_cflag =
		B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	riscom_driver.flags = TTY_DRIVER_REAL_RAW;
	riscom_driver.refcount = &riscom_refcount;
	riscom_driver.table = riscom_table;
	riscom_driver.termios = riscom_termios;
	riscom_driver.termios_locked = riscom_termios_locked;

	riscom_driver.open  = rc_open;
	riscom_driver.close = rc_close;
	riscom_driver.write = rc_write;
	riscom_driver.put_char = rc_put_char;
	riscom_driver.flush_chars = rc_flush_chars;
	riscom_driver.write_room = rc_write_room;
	riscom_driver.chars_in_buffer = rc_chars_in_buffer;
	riscom_driver.flush_buffer = rc_flush_buffer;
	riscom_driver.ioctl = rc_ioctl;
	riscom_driver.throttle = rc_throttle;
	riscom_driver.unthrottle = rc_unthrottle;
	riscom_driver.set_termios = rc_set_termios;
	riscom_driver.stop = rc_stop;
	riscom_driver.start = rc_start;
	riscom_driver.hangup = rc_hangup;

	riscom_callout_driver = riscom_driver;
	riscom_callout_driver.name = "cul";
	riscom_callout_driver.major = RISCOM8_CALLOUT_MAJOR;
	riscom_callout_driver.subtype = RISCOM_TYPE_CALLOUT;
	
	if ((error = tty_register_driver(&riscom_driver)))  {
		free_page((unsigned long)tmp_buf);
		printk("rc: Couldn't register RISCom/8 driver, error = %d\n",
		       error);
		return 1;
	}
	if ((error = tty_register_driver(&riscom_callout_driver)))  {
		free_page((unsigned long)tmp_buf);
		tty_unregister_driver(&riscom_driver);
		printk("rc: Couldn't register RISCom/8 callout driver, error = %d\n",
		       error);
		return 1;
	}
	
	memset(rc_port, 0, sizeof(rc_port));
	for (i = 0; i < RC_NPORT * RC_NBOARD; i++)  {
		rc_port[i].callout_termios = riscom_callout_driver.init_termios;
		rc_port[i].normal_termios  = riscom_driver.init_termios;
		rc_port[i].magic = RISCOM8_MAGIC;
		rc_port[i].tqueue.routine = do_softint;
		rc_port[i].tqueue.data = &rc_port[i];
		rc_port[i].tqueue_hangup.routine = do_rc_hangup;
		rc_port[i].tqueue_hangup.data = &rc_port[i];
		rc_port[i].close_delay = 50 * HZ/100;
		rc_port[i].closing_wait = 3000 * HZ/100;
		init_waitqueue_head(&rc_port[i].open_wait);
		init_waitqueue_head(&rc_port[i].close_wait);
	}
	
	return 0;
}

static void rc_release_drivers(void)
{
	unsigned long flags;

	save_flags(flags);
	cli();
	remove_bh(RISCOM8_BH);
	free_page((unsigned long)tmp_buf);
	tty_unregister_driver(&riscom_driver);
	tty_unregister_driver(&riscom_callout_driver);
	restore_flags(flags);
}

#ifndef MODULE
/*
 * Called at boot time.
 * 
 * You can specify IO base for up to RC_NBOARD cards,
 * using line "riscom8=0xiobase1,0xiobase2,.." at LILO prompt.
 * Note that there will be no probing at default
 * addresses in this case.
 *
 */ 
static int __init riscom8_setup(char *str)
{
	int ints[RC_NBOARD];
	int i;

	str = get_options(str, ARRAY_SIZE(ints), ints);

	for (i = 0; i < RC_NBOARD; i++) {
		if (i < ints[0])
			rc_board[i].base = ints[i+1];
		else 
			rc_board[i].base = 0;
	}
	return 1;
}

__setup("riscom8=", riscom8_setup);
#endif

/* 
 * This routine must be called by kernel at boot time 
 */
static int __init riscom8_init(void)
{
	int i;
	int found = 0;

	printk("rc: SDL RISCom/8 card driver v1.0, (c) D.Gorodchanin 1994-1996.\n");

	if (rc_init_drivers()) 
		return -EIO;

	for (i = 0; i < RC_NBOARD; i++) 
		if (rc_board[i].base && !rc_probe(&rc_board[i]))  
			found++;
	
	if (!found)  {
		rc_release_drivers();
		printk("rc: No RISCom/8 boards detected.\n");
		return -EIO;
	}
	return 0;
}

#ifdef MODULE
static int iobase;
static int iobase1;
static int iobase2;
static int iobase3;
MODULE_PARM(iobase, "i");
MODULE_PARM(iobase1, "i");
MODULE_PARM(iobase2, "i");
MODULE_PARM(iobase3, "i");
#endif /* MODULE */

/*
 * You can setup up to 4 boards (current value of RC_NBOARD)
 * by specifying "iobase=0xXXX iobase1=0xXXX ..." as insmod parameter.
 *
 */
static int __init riscom8_init_module (void)
{
#ifdef MODULE
	int i;

	if (iobase || iobase1 || iobase2 || iobase3) {
		for(i = 0; i < RC_NBOARD; i++)
			rc_board[0].base = 0;
	}

	if (iobase)
		rc_board[0].base = iobase;
	if (iobase1)
		rc_board[1].base = iobase1;
	if (iobase2)
		rc_board[2].base = iobase2;
	if (iobase3)
		rc_board[3].base = iobase3;
#endif /* MODULE */

	return riscom8_init();
}
	
static void __exit riscom8_exit_module (void)
{
	int i;
	
	rc_release_drivers();
	for (i = 0; i < RC_NBOARD; i++)  
		if (rc_board[i].flags & RC_BOARD_PRESENT) 
			rc_release_io_range(&rc_board[i]);
	
}

module_init(riscom8_init_module);
module_exit(riscom8_exit_module);

