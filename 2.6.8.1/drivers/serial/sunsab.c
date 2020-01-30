/* sunsab.c: ASYNC Driver for the SIEMENS SAB82532 DUSCC.
 *
 * Copyright (C) 1997  Eddie C. Dost  (ecd@skynet.be)
 * Copyright (C) 2002  David S. Miller (davem@redhat.com)
 *
 * Rewrote buffer handling to use CIRC(Circular Buffer) macros.
 *   Maxim Krasnyanskiy <maxk@qualcomm.com>
 *
 * Fixed to use tty_get_baud_rate, and to allow for arbitrary baud
 * rates to be programmed into the UART.  Also eliminated a lot of
 * duplicated code in the console setup.
 *   Theodore Ts'o <tytso@mit.edu>, 2001-Oct-12
 *
 * Ported to new 2.5.x UART layer.
 *   David S. Miller <davem@redhat.com>
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/circ_buf.h>
#include <linux/serial.h>
#include <linux/sysrq.h>
#include <linux/console.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/init.h>

#include <asm/io.h>
#include <asm/irq.h>
#include <asm/oplib.h>
#include <asm/ebus.h>

#if defined(CONFIG_SERIAL_SUNZILOG_CONSOLE) && defined(CONFIG_MAGIC_SYSRQ)
#define SUPPORT_SYSRQ
#endif

#include <linux/serial_core.h>

#include "suncore.h"
#include "sunsab.h"

struct uart_sunsab_port {
	struct uart_port		port;		/* Generic UART port	*/
	union sab82532_async_regs	*regs;		/* Chip registers	*/
	unsigned long			irqflags;	/* IRQ state flags	*/
	int				dsr;		/* Current DSR state	*/
	unsigned int			cec_timeout;	/* Chip poll timeout... */
	unsigned int			tec_timeout;	/* likewise		*/
	unsigned char			interrupt_mask0;/* ISR0 masking		*/
	unsigned char			interrupt_mask1;/* ISR1 masking		*/
	unsigned char			pvr_dtr_bit;	/* Which PVR bit is DTR */
	unsigned char			pvr_dsr_bit;	/* Which PVR bit is DSR */
	int				type;		/* SAB82532 version	*/
};

/*
 * This assumes you have a 29.4912 MHz clock for your UART.
 */
#define SAB_BASE_BAUD ( 29491200 / 16 )

static char *sab82532_version[16] = {
	"V1.0", "V2.0", "V3.2", "V(0x03)",
	"V(0x04)", "V(0x05)", "V(0x06)", "V(0x07)",
	"V(0x08)", "V(0x09)", "V(0x0a)", "V(0x0b)",
	"V(0x0c)", "V(0x0d)", "V(0x0e)", "V(0x0f)"
};

#define SAB82532_MAX_TEC_TIMEOUT 200000	/* 1 character time (at 50 baud) */
#define SAB82532_MAX_CEC_TIMEOUT  50000	/* 2.5 TX CLKs (at 50 baud) */

#define SAB82532_RECV_FIFO_SIZE	32      /* Standard async fifo sizes */
#define SAB82532_XMIT_FIFO_SIZE	32

static __inline__ void sunsab_tec_wait(struct uart_sunsab_port *up)
{
	int timeout = up->tec_timeout;

	while ((readb(&up->regs->r.star) & SAB82532_STAR_TEC) && --timeout)
		udelay(1);
}

static __inline__ void sunsab_cec_wait(struct uart_sunsab_port *up)
{
	int timeout = up->cec_timeout;

	while ((readb(&up->regs->r.star) & SAB82532_STAR_CEC) && --timeout)
		udelay(1);
}

static struct tty_struct *
receive_chars(struct uart_sunsab_port *up,
	      union sab82532_irq_status *stat,
	      struct pt_regs *regs)
{
	struct tty_struct *tty = NULL;
	unsigned char buf[32];
	int saw_console_brk = 0;
	int free_fifo = 0;
	int count = 0;
	int i;

	if (up->port.info != NULL)		/* Unopened serial console */
		tty = up->port.info->tty;

	/* Read number of BYTES (Character + Status) available. */
	if (stat->sreg.isr0 & SAB82532_ISR0_RPF) {
		count = SAB82532_RECV_FIFO_SIZE;
		free_fifo++;
	}

	if (stat->sreg.isr0 & SAB82532_ISR0_TCD) {
		count = readb(&up->regs->r.rbcl) & (SAB82532_RECV_FIFO_SIZE - 1);
		free_fifo++;
	}

	/* Issue a FIFO read command in case we where idle. */
	if (stat->sreg.isr0 & SAB82532_ISR0_TIME) {
		sunsab_cec_wait(up);
		writeb(SAB82532_CMDR_RFRD, &up->regs->w.cmdr);
		return tty;
	}

	if (stat->sreg.isr0 & SAB82532_ISR0_RFO)
		free_fifo++;

	/* Read the FIFO. */
	for (i = 0; i < count; i++)
		buf[i] = readb(&up->regs->r.rfifo[i]);

	/* Issue Receive Message Complete command. */
	if (free_fifo) {
		sunsab_cec_wait(up);
		writeb(SAB82532_CMDR_RMC, &up->regs->w.cmdr);
	}

	for (i = 0; i < count; i++) {
		unsigned char ch = buf[i];

		if (tty == NULL) {
			uart_handle_sysrq_char(&up->port, ch, regs);
			continue;
		}

		if (unlikely(tty->flip.count >= TTY_FLIPBUF_SIZE)) {
			tty->flip.work.func((void *)tty);
			if (tty->flip.count >= TTY_FLIPBUF_SIZE)
				return tty; // if TTY_DONT_FLIP is set
		}

		*tty->flip.char_buf_ptr = ch;
		*tty->flip.flag_buf_ptr = TTY_NORMAL;
		up->port.icount.rx++;

		if (unlikely(stat->sreg.isr0 & (SAB82532_ISR0_PERR |
						SAB82532_ISR0_FERR |
						SAB82532_ISR0_RFO)) ||
		    unlikely(stat->sreg.isr1 & SAB82532_ISR1_BRK)) {
			/*
			 * For statistics only
			 */
			if (stat->sreg.isr1 & SAB82532_ISR1_BRK) {
				stat->sreg.isr0 &= ~(SAB82532_ISR0_PERR |
						     SAB82532_ISR0_FERR);
				up->port.icount.brk++;
				if (up->port.line == up->port.cons->index)
					saw_console_brk = 1;
				/*
				 * We do the SysRQ and SAK checking
				 * here because otherwise the break
				 * may get masked by ignore_status_mask
				 * or read_status_mask.
				 */
				if (uart_handle_break(&up->port))
					continue;
			} else if (stat->sreg.isr0 & SAB82532_ISR0_PERR)
				up->port.icount.parity++;
			else if (stat->sreg.isr0 & SAB82532_ISR0_FERR)
				up->port.icount.frame++;
			if (stat->sreg.isr0 & SAB82532_ISR0_RFO)
				up->port.icount.overrun++;

			/*
			 * Mask off conditions which should be ingored.
			 */
			stat->sreg.isr0 &= (up->port.read_status_mask & 0xff);
			stat->sreg.isr1 &= ((up->port.read_status_mask >> 8) & 0xff);

			if (stat->sreg.isr1 & SAB82532_ISR1_BRK) {
				*tty->flip.flag_buf_ptr = TTY_BREAK;
			} else if (stat->sreg.isr0 & SAB82532_ISR0_PERR)
				*tty->flip.flag_buf_ptr = TTY_PARITY;
			else if (stat->sreg.isr0 & SAB82532_ISR0_FERR)
				*tty->flip.flag_buf_ptr = TTY_FRAME;
		}

		if (uart_handle_sysrq_char(&up->port, ch, regs))
			continue;

		if ((stat->sreg.isr0 & (up->port.ignore_status_mask & 0xff)) == 0 &&
		    (stat->sreg.isr1 & ((up->port.ignore_status_mask >> 8) & 0xff)) == 0){
			tty->flip.flag_buf_ptr++;
			tty->flip.char_buf_ptr++;
			tty->flip.count++;
		}
		if ((stat->sreg.isr0 & SAB82532_ISR0_RFO) &&
		    tty->flip.count < TTY_FLIPBUF_SIZE) {
			/*
			 * Overrun is special, since it's reported
			 * immediately, and doesn't affect the current
			 * character.
			 */
			*tty->flip.flag_buf_ptr = TTY_OVERRUN;
			tty->flip.flag_buf_ptr++;
			tty->flip.char_buf_ptr++;
			tty->flip.count++;
		}
	}

	if (saw_console_brk)
		sun_do_break();

	return tty;
}

static void sunsab_stop_tx(struct uart_port *, unsigned int);

static void transmit_chars(struct uart_sunsab_port *up,
			   union sab82532_irq_status *stat)
{
	struct circ_buf *xmit = &up->port.info->xmit;
	int i;

	if (stat->sreg.isr1 & SAB82532_ISR1_ALLS) {
		up->interrupt_mask1 |= SAB82532_IMR1_ALLS;
		writeb(up->interrupt_mask1, &up->regs->w.imr1);
		set_bit(SAB82532_ALLS, &up->irqflags);
	}

#if 0 /* bde@nwlink.com says this check causes problems */
	if (!(stat->sreg.isr1 & SAB82532_ISR1_XPR))
		return;
#endif

	if (!(readb(&up->regs->r.star) & SAB82532_STAR_XFW))
		return;

	set_bit(SAB82532_XPR, &up->irqflags);

	if (uart_circ_empty(xmit) || uart_tx_stopped(&up->port)) {
		up->interrupt_mask1 |= SAB82532_IMR1_XPR;
		writeb(up->interrupt_mask1, &up->regs->w.imr1);
		uart_write_wakeup(&up->port);
		return;
	}

	up->interrupt_mask1 &= ~(SAB82532_IMR1_ALLS|SAB82532_IMR1_XPR);
	writeb(up->interrupt_mask1, &up->regs->w.imr1);
	clear_bit(SAB82532_ALLS, &up->irqflags);

	/* Stuff 32 bytes into Transmit FIFO. */
	clear_bit(SAB82532_XPR, &up->irqflags);
	for (i = 0; i < up->port.fifosize; i++) {
		writeb(xmit->buf[xmit->tail],
		       &up->regs->w.xfifo[i]);
		xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
		up->port.icount.tx++;
		if (uart_circ_empty(xmit))
			break;
	}

	/* Issue a Transmit Frame command. */
	sunsab_cec_wait(up);
	writeb(SAB82532_CMDR_XF, &up->regs->w.cmdr);

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(&up->port);

	if (uart_circ_empty(xmit))
		sunsab_stop_tx(&up->port, 0);
}

static void check_status(struct uart_sunsab_port *up,
			 union sab82532_irq_status *stat)
{
	if (stat->sreg.isr0 & SAB82532_ISR0_CDSC)
		uart_handle_dcd_change(&up->port,
				       !(readb(&up->regs->r.vstr) & SAB82532_VSTR_CD));

	if (stat->sreg.isr1 & SAB82532_ISR1_CSC)
		uart_handle_cts_change(&up->port,
				       (readb(&up->regs->r.star) & SAB82532_STAR_CTS));

	if ((readb(&up->regs->r.pvr) & up->pvr_dsr_bit) ^ up->dsr) {
		up->dsr = (readb(&up->regs->r.pvr) & up->pvr_dsr_bit) ? 0 : 1;
		up->port.icount.dsr++;
	}

	wake_up_interruptible(&up->port.info->delta_msr_wait);
}

static irqreturn_t sunsab_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	struct uart_sunsab_port *up = dev_id;
	struct tty_struct *tty;
	union sab82532_irq_status status;
	unsigned long flags;

	spin_lock_irqsave(&up->port.lock, flags);

	status.stat = 0;
	if (readb(&up->regs->r.gis) & SAB82532_GIS_ISA0)
		status.sreg.isr0 = readb(&up->regs->r.isr0);
	if (readb(&up->regs->r.gis) & SAB82532_GIS_ISA1)
		status.sreg.isr1 = readb(&up->regs->r.isr1);

	tty = NULL;
	if (status.stat) {
		if (status.sreg.isr0 & (SAB82532_ISR0_TCD | SAB82532_ISR0_TIME |
					SAB82532_ISR0_RFO | SAB82532_ISR0_RPF))
			tty = receive_chars(up, &status, regs);
		if ((status.sreg.isr0 & SAB82532_ISR0_CDSC) ||
		    (status.sreg.isr1 & SAB82532_ISR1_CSC))
			check_status(up, &status);
		if (status.sreg.isr1 & (SAB82532_ISR1_ALLS | SAB82532_ISR1_XPR))
			transmit_chars(up, &status);
	}

	spin_unlock(&up->port.lock);

	if (tty)
		tty_flip_buffer_push(tty);

	up++;

	spin_lock(&up->port.lock);

	status.stat = 0;
	if (readb(&up->regs->r.gis) & SAB82532_GIS_ISB0)
		status.sreg.isr0 = readb(&up->regs->r.isr0);
	if (readb(&up->regs->r.gis) & SAB82532_GIS_ISB1)
		status.sreg.isr1 = readb(&up->regs->r.isr1);

	tty = NULL;
	if (status.stat) {
		if (status.sreg.isr0 & (SAB82532_ISR0_TCD | SAB82532_ISR0_TIME |
					SAB82532_ISR0_RFO | SAB82532_ISR0_RPF))
			tty = receive_chars(up, &status, regs);
		if ((status.sreg.isr0 & SAB82532_ISR0_CDSC) ||
		    (status.sreg.isr1 & (SAB82532_ISR1_BRK | SAB82532_ISR1_CSC)))
			check_status(up, &status);
		if (status.sreg.isr1 & (SAB82532_ISR1_ALLS | SAB82532_ISR1_XPR))
			transmit_chars(up, &status);
	}

	spin_unlock_irqrestore(&up->port.lock, flags);

	if (tty)
		tty_flip_buffer_push(tty);

	return IRQ_HANDLED;
}

/* port->lock is not held.  */
static unsigned int sunsab_tx_empty(struct uart_port *port)
{
	struct uart_sunsab_port *up = (struct uart_sunsab_port *) port;
	int ret;

	/* Do not need a lock for a state test like this.  */
	if (test_bit(SAB82532_ALLS, &up->irqflags))
		ret = TIOCSER_TEMT;
	else
		ret = 0;

	return ret;
}

/* port->lock held by caller.  */
static void sunsab_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	struct uart_sunsab_port *up = (struct uart_sunsab_port *) port;

	if (mctrl & TIOCM_RTS) {
		writeb(readb(&up->regs->rw.mode) & ~SAB82532_MODE_FRTS,
		       &up->regs->rw.mode);
		writeb(readb(&up->regs->rw.mode) | SAB82532_MODE_RTS,
		       &up->regs->rw.mode);
	} else {
		writeb(readb(&up->regs->rw.mode) | SAB82532_MODE_FRTS,
		       &up->regs->rw.mode);
		writeb(readb(&up->regs->rw.mode) | SAB82532_MODE_RTS,
		       &up->regs->rw.mode);
	}
	if (mctrl & TIOCM_DTR) {
		writeb(readb(&up->regs->rw.pvr) & ~(up->pvr_dtr_bit), &up->regs->rw.pvr);
	} else {
		writeb(readb(&up->regs->rw.pvr) | up->pvr_dtr_bit, &up->regs->rw.pvr);
	}
}

/* port->lock is not held.  */
static unsigned int sunsab_get_mctrl(struct uart_port *port)
{
	struct uart_sunsab_port *up = (struct uart_sunsab_port *) port;
	unsigned long flags;
	unsigned char val;
	unsigned int result;

	result = 0;

	spin_lock_irqsave(&up->port.lock, flags);

	val = readb(&up->regs->r.pvr);
	result |= (val & up->pvr_dsr_bit) ? 0 : TIOCM_DSR;

	val = readb(&up->regs->r.vstr);
	result |= (val & SAB82532_VSTR_CD) ? 0 : TIOCM_CAR;

	val = readb(&up->regs->r.star);
	result |= (val & SAB82532_STAR_CTS) ? TIOCM_CTS : 0;

	spin_unlock_irqrestore(&up->port.lock, flags);

	return result;
}

/* port->lock held by caller.  */
static void sunsab_stop_tx(struct uart_port *port, unsigned int tty_stop)
{
	struct uart_sunsab_port *up = (struct uart_sunsab_port *) port;

	up->interrupt_mask1 |= SAB82532_IMR1_XPR;
	writeb(up->interrupt_mask1, &up->regs->w.imr1);
}

/* port->lock held by caller.  */
static void sunsab_start_tx(struct uart_port *port, unsigned int tty_start)
{
	struct uart_sunsab_port *up = (struct uart_sunsab_port *) port;
	struct circ_buf *xmit = &up->port.info->xmit;
	int i;

	up->interrupt_mask1 &= ~(SAB82532_IMR1_ALLS|SAB82532_IMR1_XPR);
	writeb(up->interrupt_mask1, &up->regs->w.imr1);
	
	if (!test_bit(SAB82532_XPR, &up->irqflags))
		return;

	clear_bit(SAB82532_ALLS, &up->irqflags);
	clear_bit(SAB82532_XPR, &up->irqflags);

	for (i = 0; i < up->port.fifosize; i++) {
		writeb(xmit->buf[xmit->tail],
		       &up->regs->w.xfifo[i]);
		xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
		up->port.icount.tx++;
		if (uart_circ_empty(xmit))
			break;
	}

	/* Issue a Transmit Frame command.  */
	sunsab_cec_wait(up);
	writeb(SAB82532_CMDR_XF, &up->regs->w.cmdr);
}

/* port->lock is not held.  */
static void sunsab_send_xchar(struct uart_port *port, char ch)
{
	struct uart_sunsab_port *up = (struct uart_sunsab_port *) port;
	unsigned long flags;

	spin_lock_irqsave(&up->port.lock, flags);

	sunsab_tec_wait(up);
	writeb(ch, &up->regs->w.tic);

	spin_unlock_irqrestore(&up->port.lock, flags);
}

/* port->lock held by caller.  */
static void sunsab_stop_rx(struct uart_port *port)
{
	struct uart_sunsab_port *up = (struct uart_sunsab_port *) port;

	up->interrupt_mask0 |= SAB82532_ISR0_TCD;
	writeb(up->interrupt_mask1, &up->regs->w.imr0);
}

/* port->lock held by caller.  */
static void sunsab_enable_ms(struct uart_port *port)
{
	/* For now we always receive these interrupts.  */
}

/* port->lock is not held.  */
static void sunsab_break_ctl(struct uart_port *port, int break_state)
{
	struct uart_sunsab_port *up = (struct uart_sunsab_port *) port;
	unsigned long flags;
	unsigned char val;

	spin_lock_irqsave(&up->port.lock, flags);

	val = readb(&up->regs->rw.dafo);
	if (break_state)
		val |= SAB82532_DAFO_XBRK;
	else
		val &= ~SAB82532_DAFO_XBRK;
	writeb(val, &up->regs->rw.dafo);

	spin_unlock_irqrestore(&up->port.lock, flags);
}

/* port->lock is not held.  */
static int sunsab_startup(struct uart_port *port)
{
	struct uart_sunsab_port *up = (struct uart_sunsab_port *) port;
	unsigned long flags;
	unsigned char tmp;

	spin_lock_irqsave(&up->port.lock, flags);

	/*
	 * Wait for any commands or immediate characters
	 */
	sunsab_cec_wait(up);
	sunsab_tec_wait(up);

	/*
	 * Clear the FIFO buffers.
	 */
	writeb(SAB82532_CMDR_RRES, &up->regs->w.cmdr);
	sunsab_cec_wait(up);
	writeb(SAB82532_CMDR_XRES, &up->regs->w.cmdr);

	/*
	 * Clear the interrupt registers.
	 */
	(void) readb(&up->regs->r.isr0);
	(void) readb(&up->regs->r.isr1);

	/*
	 * Now, initialize the UART 
	 */
	writeb(0, &up->regs->w.ccr0);				/* power-down */
	writeb(SAB82532_CCR0_MCE | SAB82532_CCR0_SC_NRZ |
	       SAB82532_CCR0_SM_ASYNC, &up->regs->w.ccr0);
	writeb(SAB82532_CCR1_ODS | SAB82532_CCR1_BCR | 7, &up->regs->w.ccr1);
	writeb(SAB82532_CCR2_BDF | SAB82532_CCR2_SSEL |
	       SAB82532_CCR2_TOE, &up->regs->w.ccr2);
	writeb(0, &up->regs->w.ccr3);
	writeb(SAB82532_CCR4_MCK4 | SAB82532_CCR4_EBRG, &up->regs->w.ccr4);
	writeb(SAB82532_MODE_RTS | SAB82532_MODE_FCTS |
	       SAB82532_MODE_RAC, &up->regs->w.mode);
	writeb(SAB82532_RFC_DPS|SAB82532_RFC_RFTH_32, &up->regs->w.rfc);
	
	tmp = readb(&up->regs->rw.ccr0);
	tmp |= SAB82532_CCR0_PU;	/* power-up */
	writeb(tmp, &up->regs->rw.ccr0);

	/*
	 * Finally, enable interrupts
	 */
	up->interrupt_mask0 = (SAB82532_IMR0_PERR | SAB82532_IMR0_FERR |
			       SAB82532_IMR0_PLLA);
	writeb(up->interrupt_mask0, &up->regs->w.imr0);
	up->interrupt_mask1 = (SAB82532_IMR1_BRKT | SAB82532_IMR1_ALLS |
			       SAB82532_IMR1_XOFF | SAB82532_IMR1_TIN |
			       SAB82532_IMR1_CSC | SAB82532_IMR1_XON |
			       SAB82532_IMR1_XPR);
	writeb(up->interrupt_mask1, &up->regs->w.imr1);
	set_bit(SAB82532_ALLS, &up->irqflags);
	set_bit(SAB82532_XPR, &up->irqflags);

	spin_unlock_irqrestore(&up->port.lock, flags);

	return 0;
}

/* port->lock is not held.  */
static void sunsab_shutdown(struct uart_port *port)
{
	struct uart_sunsab_port *up = (struct uart_sunsab_port *) port;
	unsigned long flags;
	unsigned char tmp;

	spin_lock_irqsave(&up->port.lock, flags);

	/* Disable Interrupts */
	up->interrupt_mask0 = 0xff;
	writeb(up->interrupt_mask0, &up->regs->w.imr0);
	up->interrupt_mask1 = 0xff;
	writeb(up->interrupt_mask1, &up->regs->w.imr1);

	/* Disable break condition */
	tmp = readb(&up->regs->rw.dafo);
	tmp &= ~SAB82532_DAFO_XBRK;
	writeb(tmp, &up->regs->rw.dafo);

	/* Disable Receiver */	
	tmp = readb(&up->regs->rw.mode);
	tmp &= ~SAB82532_MODE_RAC;
	writeb(tmp, &up->regs->rw.mode);

	/*
	 * XXX FIXME
	 *
	 * If the chip is powered down here the system hangs/crashes during
	 * reboot or shutdown.  This needs to be investigated further,
	 * similar behaviour occurs in 2.4 when the driver is configured
	 * as a module only.  One hint may be that data is sometimes
	 * transmitted at 9600 baud during shutdown (regardless of the
	 * speed the chip was configured for when the port was open).
	 */
#if 0
	/* Power Down */	
	tmp = readb(&up->regs->rw.ccr0);
	tmp &= ~SAB82532_CCR0_PU;
	writeb(tmp, &up->regs->rw.ccr0);
#endif

	spin_unlock_irqrestore(&up->port.lock, flags);
}

/*
 * This is used to figure out the divisor speeds.
 *
 * The formula is:    Baud = SAB_BASE_BAUD / ((N + 1) * (1 << M)),
 *
 * with               0 <= N < 64 and 0 <= M < 16
 */

static void calc_ebrg(int baud, int *n_ret, int *m_ret)
{
	int	n, m;

	if (baud == 0) {
		*n_ret = 0;
		*m_ret = 0;
		return;
	}
     
	/*
	 * We scale numbers by 10 so that we get better accuracy
	 * without having to use floating point.  Here we increment m
	 * until n is within the valid range.
	 */
	n = (SAB_BASE_BAUD * 10) / baud;
	m = 0;
	while (n >= 640) {
		n = n / 2;
		m++;
	}
	n = (n+5) / 10;
	/*
	 * We try very hard to avoid speeds with M == 0 since they may
	 * not work correctly for XTAL frequences above 10 MHz.
	 */
	if ((m == 0) && ((n & 1) == 0)) {
		n = n / 2;
		m++;
	}
	*n_ret = n - 1;
	*m_ret = m;
}

/* Internal routine, port->lock is held and local interrupts are disabled.  */
static void sunsab_convert_to_sab(struct uart_sunsab_port *up, unsigned int cflag,
				  unsigned int iflag, int baud)
{
	unsigned int ebrg;
	unsigned char dafo;
	int bits, n, m;

	/* Byte size and parity */
	switch (cflag & CSIZE) {
	      case CS5: dafo = SAB82532_DAFO_CHL5; bits = 7; break;
	      case CS6: dafo = SAB82532_DAFO_CHL6; bits = 8; break;
	      case CS7: dafo = SAB82532_DAFO_CHL7; bits = 9; break;
	      case CS8: dafo = SAB82532_DAFO_CHL8; bits = 10; break;
	      /* Never happens, but GCC is too dumb to figure it out */
	      default:  dafo = SAB82532_DAFO_CHL5; bits = 7; break;
	}

	if (cflag & CSTOPB) {
		dafo |= SAB82532_DAFO_STOP;
		bits++;
	}

	if (cflag & PARENB) {
		dafo |= SAB82532_DAFO_PARE;
		bits++;
	}

	if (cflag & PARODD) {
		dafo |= SAB82532_DAFO_PAR_ODD;
	} else {
		dafo |= SAB82532_DAFO_PAR_EVEN;
	}

	calc_ebrg(baud, &n, &m);

	ebrg = n | (m << 6);

	up->tec_timeout = (10 * 1000000) / baud;
	up->cec_timeout = up->tec_timeout >> 2;

	/* CTS flow control flags */
	/* We encode read_status_mask and ignore_status_mask like so:
	 *
	 * ---------------------
	 * | ... | ISR1 | ISR0 |
	 * ---------------------
	 *  ..    15   8 7    0
	 */

	up->port.read_status_mask = (SAB82532_ISR0_TCD | SAB82532_ISR0_TIME |
				     SAB82532_ISR0_RFO | SAB82532_ISR0_RPF |
				     SAB82532_ISR0_CDSC);
	up->port.read_status_mask |= (SAB82532_ISR1_CSC |
				      SAB82532_ISR1_ALLS |
				      SAB82532_ISR1_XPR) << 8;
	if (iflag & INPCK)
		up->port.read_status_mask |= (SAB82532_ISR0_PERR |
					      SAB82532_ISR0_FERR);
	if (iflag & (BRKINT | PARMRK))
		up->port.read_status_mask |= (SAB82532_ISR1_BRK << 8);

	/*
	 * Characteres to ignore
	 */
	up->port.ignore_status_mask = 0;
	if (iflag & IGNPAR)
		up->port.ignore_status_mask |= (SAB82532_ISR0_PERR |
						SAB82532_ISR0_FERR);
	if (iflag & IGNBRK) {
		up->port.ignore_status_mask |= (SAB82532_ISR1_BRK << 8);
		/*
		 * If we're ignoring parity and break indicators,
		 * ignore overruns too (for real raw support).
		 */
		if (iflag & IGNPAR)
			up->port.ignore_status_mask |= SAB82532_ISR0_RFO;
	}

	/*
	 * ignore all characters if CREAD is not set
	 */
	if ((cflag & CREAD) == 0)
		up->port.ignore_status_mask |= (SAB82532_ISR0_RPF |
						SAB82532_ISR0_TCD);

	/* Now bang the new settings into the chip.  */
	sunsab_cec_wait(up);
	sunsab_tec_wait(up);
	writeb(dafo, &up->regs->w.dafo);
	writeb(ebrg & 0xff, &up->regs->w.bgr);
	writeb((readb(&up->regs->rw.ccr2) & ~0xc0) | ((ebrg >> 2) & 0xc0),
	       &up->regs->rw.ccr2);

	if (cflag & CRTSCTS) {
		writeb(readb(&up->regs->rw.mode) & ~SAB82532_MODE_RTS,
		       &up->regs->rw.mode);
		writeb(readb(&up->regs->rw.mode) | SAB82532_MODE_FRTS,
		       &up->regs->rw.mode);
		writeb(readb(&up->regs->rw.mode) & ~SAB82532_MODE_FCTS,
		       &up->regs->rw.mode);
		up->interrupt_mask1 &= ~SAB82532_IMR1_CSC;
		writeb(up->interrupt_mask1, &up->regs->w.imr1);
	} else {
		writeb(readb(&up->regs->rw.mode) | SAB82532_MODE_RTS,
		       &up->regs->rw.mode);
		writeb(readb(&up->regs->rw.mode) & ~SAB82532_MODE_FRTS,
		       &up->regs->rw.mode);
		writeb(readb(&up->regs->rw.mode) | SAB82532_MODE_FCTS,
		       &up->regs->rw.mode);
		up->interrupt_mask1 |= SAB82532_IMR1_CSC;
		writeb(up->interrupt_mask1, &up->regs->w.imr1);
	}
	writeb(readb(&up->regs->rw.mode) | SAB82532_MODE_RAC, &up->regs->rw.mode);

}

/* port->lock is not held.  */
static void sunsab_set_termios(struct uart_port *port, struct termios *termios,
			       struct termios *old)
{
	struct uart_sunsab_port *up = (struct uart_sunsab_port *) port;
	unsigned long flags;
	int baud = uart_get_baud_rate(port, termios, old, 0, 4000000);

	spin_lock_irqsave(&up->port.lock, flags);
	sunsab_convert_to_sab(up, termios->c_cflag, termios->c_iflag, baud);
	spin_unlock_irqrestore(&up->port.lock, flags);
}

static const char *sunsab_type(struct uart_port *port)
{
	struct uart_sunsab_port *up = (void *)port;
	static char buf[36];
	
	sprintf(buf, "SAB82532 %s", sab82532_version[up->type]);
	return buf;
}

static void sunsab_release_port(struct uart_port *port)
{
}

static int sunsab_request_port(struct uart_port *port)
{
	return 0;
}

static void sunsab_config_port(struct uart_port *port, int flags)
{
}

static int sunsab_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	return -EINVAL;
}

static struct uart_ops sunsab_pops = {
	.tx_empty	= sunsab_tx_empty,
	.set_mctrl	= sunsab_set_mctrl,
	.get_mctrl	= sunsab_get_mctrl,
	.stop_tx	= sunsab_stop_tx,
	.start_tx	= sunsab_start_tx,
	.send_xchar	= sunsab_send_xchar,
	.stop_rx	= sunsab_stop_rx,
	.enable_ms	= sunsab_enable_ms,
	.break_ctl	= sunsab_break_ctl,
	.startup	= sunsab_startup,
	.shutdown	= sunsab_shutdown,
	.set_termios	= sunsab_set_termios,
	.type		= sunsab_type,
	.release_port	= sunsab_release_port,
	.request_port	= sunsab_request_port,
	.config_port	= sunsab_config_port,
	.verify_port	= sunsab_verify_port,
};

static struct uart_driver sunsab_reg = {
	.owner			= THIS_MODULE,
	.driver_name		= "serial",
	.devfs_name		= "tts/",
	.dev_name		= "ttyS",
	.major			= TTY_MAJOR,
};

static struct uart_sunsab_port *sunsab_ports;
static int num_channels;

#ifdef CONFIG_SERIAL_SUNSAB_CONSOLE

static __inline__ void sunsab_console_putchar(struct uart_sunsab_port *up, char c)
{
	unsigned long flags;

	spin_lock_irqsave(&up->port.lock, flags);

	sunsab_tec_wait(up);
	writeb(c, &up->regs->w.tic);

	spin_unlock_irqrestore(&up->port.lock, flags);
}

static void sunsab_console_write(struct console *con, const char *s, unsigned n)
{
	struct uart_sunsab_port *up = &sunsab_ports[con->index];
	int i;

	for (i = 0; i < n; i++) {
		if (*s == '\n')
			sunsab_console_putchar(up, '\r');
		sunsab_console_putchar(up, *s++);
	}
	sunsab_tec_wait(up);
}

static int sunsab_console_setup(struct console *con, char *options)
{
	struct uart_sunsab_port *up = &sunsab_ports[con->index];
	unsigned long flags;
	int baud;

	printk("Console: ttyS%d (SAB82532)\n",
	       (sunsab_reg.minor - 64) + con->index);

	sunserial_console_termios(con);

	/* Firmware console speed is limited to 150-->38400 baud so
	 * this hackish cflag thing is OK.
	 */
	switch (con->cflag & CBAUD) {
	case B150: baud = 150; break;
	case B300: baud = 300; break;
	case B600: baud = 600; break;
	case B1200: baud = 1200; break;
	case B2400: baud = 2400; break;
	case B4800: baud = 4800; break;
	default: case B9600: baud = 9600; break;
	case B19200: baud = 19200; break;
	case B38400: baud = 38400; break;
	};

	/*
	 * Temporary fix.
	 */
	spin_lock_init(&up->port.lock);

	/*
	 * Initialize the hardware
	 */
	sunsab_startup(&up->port);

	spin_lock_irqsave(&up->port.lock, flags);

	/*
	 * Finally, enable interrupts
	 */
	up->interrupt_mask0 = SAB82532_IMR0_PERR | SAB82532_IMR0_FERR |
				SAB82532_IMR0_PLLA | SAB82532_IMR0_CDSC;
	writeb(up->interrupt_mask0, &up->regs->w.imr0);
	up->interrupt_mask1 = SAB82532_IMR1_BRKT | SAB82532_IMR1_ALLS |
				SAB82532_IMR1_XOFF | SAB82532_IMR1_TIN |
				SAB82532_IMR1_CSC | SAB82532_IMR1_XON |
				SAB82532_IMR1_XPR;
	writeb(up->interrupt_mask1, &up->regs->w.imr1);

	sunsab_convert_to_sab(up, con->cflag, 0, baud);

	spin_unlock_irqrestore(&up->port.lock, flags);
	
	return 0;
}

static struct console sunsab_console = {
	.name	=	"ttyS",
	.write	=	sunsab_console_write,
	.device	=	uart_console_device,
	.setup	=	sunsab_console_setup,
	.flags	=	CON_PRINTBUFFER,
	.index	=	-1,
	.data	=	&sunsab_reg,
};
#define SUNSAB_CONSOLE	(&sunsab_console)

static void __init sunsab_console_init(void)
{
	int i;

	if (con_is_present())
		return;

	for (i = 0; i < num_channels; i++) {
		int this_minor = sunsab_reg.minor + i;

		if ((this_minor - 64) == (serial_console - 1))
			break;
	}
	if (i == num_channels)
		return;

	sunsab_console.index = i;
	register_console(&sunsab_console);
}
#else
#define SUNSAB_CONSOLE		(NULL)
#define sunsab_console_init()	do { } while (0)
#endif

static void __init for_each_sab_edev(void (*callback)(struct linux_ebus_device *, void *), void *arg)
{
	struct linux_ebus *ebus;
	struct linux_ebus_device *edev = NULL;

	for_each_ebus(ebus) {
		for_each_ebusdev(edev, ebus) {
			if (!strcmp(edev->prom_name, "se")) {
				callback(edev, arg);
				continue;
			} else if (!strcmp(edev->prom_name, "serial")) {
				char compat[32];
				int clen;

				/* On RIO this can be an SE, check it.  We could
				 * just check ebus->is_rio, but this is more portable.
				 */
				clen = prom_getproperty(edev->prom_node, "compatible",
							compat, sizeof(compat));
				if (clen > 0) {
					if (strncmp(compat, "sab82532", 8) == 0) {
						callback(edev, arg);
						continue;
					}
				}
			}
		}
	}
}

static void __init sab_count_callback(struct linux_ebus_device *edev, void *arg)
{
	int *count_p = arg;

	(*count_p)++;
}

static void __init sab_attach_callback(struct linux_ebus_device *edev, void *arg)
{
	int *instance_p = arg;
	struct uart_sunsab_port *up;
	unsigned long regs, offset;
	int i;

	/* Note: ports are located in reverse order */
	regs = edev->resource[0].start;
	offset = sizeof(union sab82532_async_regs);
	for (i = 0; i < 2; i++) {
		up = &sunsab_ports[(*instance_p * 2) + 1 - i];

		memset(up, 0, sizeof(*up));
		up->regs = ioremap(regs + offset, sizeof(union sab82532_async_regs));
		up->port.irq = edev->irqs[0];
		up->port.fifosize = SAB82532_XMIT_FIFO_SIZE;
		up->port.mapbase = (unsigned long)up->regs;
		up->port.iotype = SERIAL_IO_MEM;

		writeb(SAB82532_IPC_IC_ACT_LOW, &up->regs->w.ipc);

		offset -= sizeof(union sab82532_async_regs);
	}
	
	(*instance_p)++;
}

static int __init probe_for_sabs(void)
{
	int this_sab = 0;

	/* Find device instances.  */
	for_each_sab_edev(&sab_count_callback, &this_sab);
	if (!this_sab)
		return -ENODEV;

	/* Allocate tables.  */
	sunsab_ports = kmalloc(sizeof(struct uart_sunsab_port) * this_sab * 2,
			       GFP_KERNEL);
	if (!sunsab_ports)
		return -ENOMEM;

	num_channels = this_sab * 2;

	this_sab = 0;
	for_each_sab_edev(&sab_attach_callback, &this_sab);
	return 0;
}

static void __init sunsab_init_hw(void)
{
	int i;

	for (i = 0; i < num_channels; i++) {
		struct uart_sunsab_port *up = &sunsab_ports[i];

		up->port.line = i;
		up->port.ops = &sunsab_pops;
		up->port.type = PORT_SUNSAB;
		up->port.uartclk = SAB_BASE_BAUD;

		up->type = readb(&up->regs->r.vstr) & 0x0f;
		writeb(~((1 << 1) | (1 << 2) | (1 << 4)), &up->regs->w.pcr);
		writeb(0xff, &up->regs->w.pim);
		if (up->port.line == 0) {
			up->pvr_dsr_bit = (1 << 0);
			up->pvr_dtr_bit = (1 << 1);
		} else {
			up->pvr_dsr_bit = (1 << 3);
			up->pvr_dtr_bit = (1 << 2);
		}
		writeb((1 << 1) | (1 << 2) | (1 << 4), &up->regs->w.pvr);
		writeb(readb(&up->regs->rw.mode) | SAB82532_MODE_FRTS,
		       &up->regs->rw.mode);
		writeb(readb(&up->regs->rw.mode) | SAB82532_MODE_RTS,
		       &up->regs->rw.mode);

		up->tec_timeout = SAB82532_MAX_TEC_TIMEOUT;
		up->cec_timeout = SAB82532_MAX_CEC_TIMEOUT;

		if (!(up->port.line & 0x01)) {
			if (request_irq(up->port.irq, sunsab_interrupt,
			                SA_SHIRQ, "serial(sab82532)", up)) {
				printk("sunsab%d: can't get IRQ %x\n",
				       i, up->port.irq);
				continue;
			}
		}
	}
}

static int __init sunsab_init(void)
{
	int ret = probe_for_sabs();
	int i;

	if (ret < 0)
		return ret;

	sunsab_init_hw();

	sunsab_reg.minor = sunserial_current_minor;
	sunsab_reg.nr = num_channels;
	sunsab_reg.cons = SUNSAB_CONSOLE;

	ret = uart_register_driver(&sunsab_reg);
	if (ret < 0) {
		int i;

		for (i = 0; i < num_channels; i++) {
			struct uart_sunsab_port *up = &sunsab_ports[i];

			if (!(up->port.line & 0x01))
				free_irq(up->port.irq, up);
			iounmap(up->regs);
		}
		kfree(sunsab_ports);
		sunsab_ports = NULL;

		return ret;
	}

	sunserial_current_minor += num_channels;
	
	for (i = 0; i < num_channels; i++) {
		struct uart_sunsab_port *up = &sunsab_ports[i];

		uart_add_one_port(&sunsab_reg, &up->port);
	}

	sunsab_console_init();

	return 0;
}

static void __exit sunsab_exit(void)
{
	int i;

	for (i = 0; i < num_channels; i++) {
		struct uart_sunsab_port *up = &sunsab_ports[i];

		uart_remove_one_port(&sunsab_reg, &up->port);

		if (!(up->port.line & 0x01))
			free_irq(up->port.irq, up);
		iounmap(up->regs);
	}

	sunserial_current_minor -= num_channels;
	uart_unregister_driver(&sunsab_reg);

	kfree(sunsab_ports);
	sunsab_ports = NULL;
}

module_init(sunsab_init);
module_exit(sunsab_exit);

MODULE_AUTHOR("Eddie C. Dost and David S. Miller");
MODULE_DESCRIPTION("Sun SAB82532 serial port driver");
MODULE_LICENSE("GPL");
