/*
 *   serial.c
 *   Copyright (c) by Jaroslav Kysela <perex@suse.cz>,
 *                    Isaku Yamahata <yamahata@private.email.ne.jp>,
 *		      George Hansper <ghansper@apana.org.au>,
 *		      Hannu Savolainen
 *
 *   This code is based on the code from ALSA 0.5.9, but heavily rewritten.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 * Sat Mar 31 17:27:57 PST 2001 tim.mann@compaq.com 
 *      Added support for the Midiator MS-124T and for the MS-124W in
 *      Single Addressed (S/A) or Multiple Burst (M/B) mode, with
 *      power derived either parasitically from the serial port or
 *      from a separate power supply.
 *
 *      More documentation can be found in serial-u16550.txt.
 */

#include <sound/driver.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/moduleparam.h>
#include <sound/core.h>
#include <sound/rawmidi.h>
#include <sound/initval.h>

#include <linux/serial_reg.h>

#include <asm/io.h>

MODULE_DESCRIPTION("MIDI serial u16550");
MODULE_LICENSE("GPL");
MODULE_CLASSES("{sound}");
MODULE_DEVICES("{{ALSA, MIDI serial u16550}}");

#define SNDRV_SERIAL_SOUNDCANVAS 0 /* Roland Soundcanvas; F5 NN selects part */
#define SNDRV_SERIAL_MS124T 1      /* Midiator MS-124T */
#define SNDRV_SERIAL_MS124W_SA 2   /* Midiator MS-124W in S/A mode */
#define SNDRV_SERIAL_MS124W_MB 3   /* Midiator MS-124W in M/B mode */
#define SNDRV_SERIAL_GENERIC 4     /* Generic Interface */
#define SNDRV_SERIAL_MAX_ADAPTOR SNDRV_SERIAL_GENERIC
static char *adaptor_names[] = {
	"Soundcanvas",
        "MS-124T",
	"MS-124W S/A",
	"MS-124W M/B",
	"Generic"
};

#define SNDRV_SERIAL_NORMALBUFF 0 /* Normal blocking buffer operation */
#define SNDRV_SERIAL_DROPBUFF   1 /* Non-blocking discard operation */

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;	/* Index 0-MAX */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;	/* ID for this card */
static int enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE; /* Enable this card */
static long port[SNDRV_CARDS] = SNDRV_DEFAULT_PORT; /* 0x3f8,0x2f8,0x3e8,0x2e8 */
static int irq[SNDRV_CARDS] = SNDRV_DEFAULT_IRQ; 	/* 3,4,5,7,9,10,11,14,15 */
static int speed[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = 38400}; /* 9600,19200,38400,57600,115200 */
static int base[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = 115200}; /* baud base */
static int outs[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = 1};	 /* 1 to 16 */
static int ins[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = 1};	/* 1 to 16 */
static int adaptor[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = SNDRV_SERIAL_SOUNDCANVAS};
static int droponfull[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS -1)] = SNDRV_SERIAL_NORMALBUFF };
static int boot_devs;

module_param_array(index, int, boot_devs, 0444);
MODULE_PARM_DESC(index, "Index value for Serial MIDI.");
MODULE_PARM_SYNTAX(index, SNDRV_INDEX_DESC);
module_param_array(id, charp, boot_devs, 0444);
MODULE_PARM_DESC(id, "ID string for Serial MIDI.");
MODULE_PARM_SYNTAX(id, SNDRV_ID_DESC);
module_param_array(enable, bool, boot_devs, 0444);
MODULE_PARM_DESC(enable, "Enable UART16550A chip.");
MODULE_PARM_SYNTAX(enable, SNDRV_ENABLE_DESC);
module_param_array(port, long, boot_devs, 0444);
MODULE_PARM_DESC(port, "Port # for UART16550A chip.");
MODULE_PARM_SYNTAX(port, SNDRV_PORT12_DESC);
module_param_array(irq, int, boot_devs, 0444);
MODULE_PARM_DESC(irq, "IRQ # for UART16550A chip.");
MODULE_PARM_SYNTAX(irq, SNDRV_IRQ_DESC);
module_param_array(speed, int, boot_devs, 0444);
MODULE_PARM_DESC(speed, "Speed in bauds.");
MODULE_PARM_SYNTAX(speed, SNDRV_ENABLED ",allows:{9600,19200,38400,57600,115200},dialog:list");
module_param_array(base, int, boot_devs, 0444);
MODULE_PARM_DESC(base, "Base for divisor in bauds.");
MODULE_PARM_SYNTAX(base, SNDRV_ENABLED ",allows:{57600,115200,230400,460800},dialog:list");
module_param_array(outs, int, boot_devs, 0444);
MODULE_PARM_DESC(outs, "Number of MIDI outputs.");
module_param_array(ins, int, boot_devs, 0444);
MODULE_PARM_DESC(ins, "Number of MIDI inputs.");
module_param_array(droponfull, bool, boot_devs, 0444);
MODULE_PARM_DESC(droponfull, "Flag to enable drop-on-full buffer mode");
MODULE_PARM_SYNTAX(droponfull, SNDRV_ENABLED "," SNDRV_BOOLEAN_FALSE_DESC);

MODULE_PARM_SYNTAX(outs, SNDRV_ENABLED ",allows:{{1,16}},dialog:list");
MODULE_PARM_SYNTAX(ins, SNDRV_ENABLED ",allows:{{1,16}},dialog:list");
module_param_array(adaptor, int, boot_devs, 0444);
MODULE_PARM_DESC(adaptor, "Type of adaptor.");
MODULE_PARM_SYNTAX(adaptor, SNDRV_ENABLED ",allows:{{0=Soundcanvas,1=MS-124T,2=MS-124W S/A,3=MS-124W M/B,4=Generic}},dialog:list");

/*#define SNDRV_SERIAL_MS124W_MB_NOCOMBO 1*/  /* Address outs as 0-3 instead of bitmap */

#define SNDRV_SERIAL_MAX_OUTS	16		/* max 64, min 16 */
#define SNDRV_SERIAL_MAX_INS	16		/* max 64, min 16 */

#define TX_BUFF_SIZE		(1<<15)		/* Must be 2^n */
#define TX_BUFF_MASK		(TX_BUFF_SIZE - 1)

#define SERIAL_MODE_NOT_OPENED 		(0)
#define SERIAL_MODE_INPUT_OPEN		(1 << 0)
#define SERIAL_MODE_OUTPUT_OPEN		(1 << 1)
#define SERIAL_MODE_INPUT_TRIGGERED	(1 << 2)
#define SERIAL_MODE_OUTPUT_TRIGGERED	(1 << 3)

typedef struct _snd_uart16550 {
	snd_card_t *card;
	snd_rawmidi_t *rmidi;
	snd_rawmidi_substream_t *midi_output[SNDRV_SERIAL_MAX_OUTS];
	snd_rawmidi_substream_t *midi_input[SNDRV_SERIAL_MAX_INS];

	int filemode;		//open status of file

	spinlock_t open_lock;

	int irq;

	unsigned long base;
	struct resource *res_base;

	unsigned int speed;
	unsigned int speed_base;
	unsigned char divisor;

	unsigned char old_divisor_lsb;
	unsigned char old_divisor_msb;
	unsigned char old_line_ctrl_reg;

	// parameter for using of write loop
	short int fifo_limit;	//used in uart16550
        short int fifo_count;	//used in uart16550

	// type of adaptor
	int adaptor;

	// inputs
	int prev_in;
	unsigned char rstatus;

	// outputs
	int prev_out;
	unsigned char prev_status[SNDRV_SERIAL_MAX_OUTS];

	// write buffer and its writing/reading position
	unsigned char tx_buff[TX_BUFF_SIZE];
	int buff_in_count;
        int buff_in;
        int buff_out;
        int drop_on_full;

	// wait timer
	unsigned int timer_running:1;
	struct timer_list buffer_timer;

} snd_uart16550_t;

static snd_card_t *snd_serial_cards[SNDRV_CARDS] = SNDRV_DEFAULT_PTR;

inline static void snd_uart16550_add_timer(snd_uart16550_t *uart)
{
	if (! uart->timer_running) {
		/* timer 38600bps * 10bit * 16byte */
		uart->buffer_timer.expires = jiffies + (HZ+255)/256;
		uart->timer_running = 1;
		add_timer(&uart->buffer_timer);
	}
}

inline static void snd_uart16550_del_timer(snd_uart16550_t *uart)
{
	if (uart->timer_running) {
		del_timer(&uart->buffer_timer);
		uart->timer_running = 0;
	}
}

/* This macro is only used in snd_uart16550_io_loop */
inline static void snd_uart16550_buffer_output(snd_uart16550_t *uart)
{
	unsigned short buff_out = uart->buff_out;
	if( uart->buff_in_count > 0 ) {
		outb(uart->tx_buff[buff_out], uart->base + UART_TX);
		uart->fifo_count++;
		buff_out++;
		buff_out &= TX_BUFF_MASK;
		uart->buff_out = buff_out;
		uart->buff_in_count--;
	}
}

/* This loop should be called with interrupts disabled
 * We don't want to interrupt this, 
 * as we're already handling an interrupt 
 */
static void snd_uart16550_io_loop(snd_uart16550_t * uart)
{
	unsigned char c, status;
	int substream;

	/* recall previous stream */
	substream = uart->prev_in;

	/* Read Loop */
	while ((status = inb(uart->base + UART_LSR)) & UART_LSR_DR) {
		/* while receive data ready */
		c = inb(uart->base + UART_RX);

		/* keep track of last status byte */
		if (c & 0x80) {
			uart->rstatus = c;
		}

		/* handle stream switch */
		if (uart->adaptor == SNDRV_SERIAL_GENERIC) {
			if (uart->rstatus == 0xf5) {
				if (c <= SNDRV_SERIAL_MAX_INS && c > 0)
					substream = c - 1;
				if (c != 0xf5)
					uart->rstatus = 0; /* prevent future bytes from being interpreted as streams */
			}
			else if ((uart->filemode & SERIAL_MODE_INPUT_OPEN) && (uart->midi_input[substream] != NULL)) {
				snd_rawmidi_receive(uart->midi_input[substream], &c, 1);
		}
		} else if ((uart->filemode & SERIAL_MODE_INPUT_OPEN) && (uart->midi_input[substream] != NULL)) {
			snd_rawmidi_receive(uart->midi_input[substream], &c, 1);
		}

		if (status & UART_LSR_OE)
			snd_printk("%s: Overrun on device at 0x%lx\n",
			       uart->rmidi->name, uart->base);
	}

	/* remember the last stream */
	uart->prev_in = substream;

	/* no need of check SERIAL_MODE_OUTPUT_OPEN because if not,
	   buffer is never filled. */
	/* Check write status */
	if (status & UART_LSR_THRE) {
		uart->fifo_count = 0;
	}
	if (uart->adaptor == SNDRV_SERIAL_MS124W_SA
	   || uart->adaptor == SNDRV_SERIAL_GENERIC) {
		/* Can't use FIFO, must send only when CTS is true */
		status = inb(uart->base + UART_MSR);
		while( (uart->fifo_count == 0) && (status & UART_MSR_CTS) &&
		      (uart->buff_in_count > 0) ) {
		       snd_uart16550_buffer_output(uart);
		       status = inb( uart->base + UART_MSR );
		}
	} else {
		/* Write loop */
		while (uart->fifo_count < uart->fifo_limit	/* Can we write ? */
		       && uart->buff_in_count > 0)	/* Do we want to? */
			snd_uart16550_buffer_output(uart);
	}
	if (uart->irq < 0 && uart->buff_in_count > 0)
		snd_uart16550_add_timer(uart);
}

/* NOTES ON SERVICING INTERUPTS
 * ---------------------------
 * After receiving a interrupt, it is important to indicate to the UART that
 * this has been done. 
 * For a Rx interrupt, this is done by reading the received byte.
 * For a Tx interrupt this is done by either:
 * a) Writing a byte
 * b) Reading the IIR
 * It is particularly important to read the IIR if a Tx interrupt is received
 * when there is no data in tx_buff[], as in this case there no other
 * indication that the interrupt has been serviced, and it remains outstanding
 * indefinitely. This has the curious side effect that and no further interrupts
 * will be generated from this device AT ALL!!.
 * It is also desirable to clear outstanding interrupts when the device is
 * opened/closed.
 *
 *
 * Note that some devices need OUT2 to be set before they will generate
 * interrupts at all. (Possibly tied to an internal pull-up on CTS?)
 */
static irqreturn_t snd_uart16550_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	snd_uart16550_t *uart;

	uart = (snd_uart16550_t *) dev_id;
	spin_lock(&uart->open_lock);
	if (uart->filemode == SERIAL_MODE_NOT_OPENED) {
		spin_unlock(&uart->open_lock);
		return IRQ_NONE;
	}
	inb(uart->base + UART_IIR);		/* indicate to the UART that the interrupt has been serviced */
	snd_uart16550_io_loop(uart);
	spin_unlock(&uart->open_lock);
	return IRQ_HANDLED;
}

/* When the polling mode, this function calls snd_uart16550_io_loop. */
static void snd_uart16550_buffer_timer(unsigned long data)
{
	snd_uart16550_t *uart;

	uart = (snd_uart16550_t *)data;
	spin_lock(&uart->open_lock);
	snd_uart16550_del_timer(uart);
	snd_uart16550_io_loop(uart);
	spin_unlock(&uart->open_lock);
}

/*
 *  this method probes, if an uart sits on given port
 *  return 0 if found
 *  return negative error if not found
 */
static int __init snd_uart16550_detect(snd_uart16550_t *uart)
{
	unsigned long io_base = uart->base;
	int ok;
	unsigned char c;

	/* Do some vague tests for the presence of the uart */
	if (io_base == 0 || io_base == SNDRV_AUTO_PORT) {
		return -ENODEV;	/* Not configured */
	}

	uart->res_base = request_region(io_base, 8, "Serial MIDI");
	if (uart->res_base == NULL) {
		snd_printk(KERN_ERR "u16550: can't grab port 0x%lx\n", io_base);
		return -EBUSY;
	}

	ok = 1;			/* uart detected unless one of the following tests should fail */
	/* 8 data-bits, 1 stop-bit, parity off, DLAB = 0 */
	outb(UART_LCR_WLEN8, io_base + UART_LCR); /* Line Control Register */
	c = inb(io_base + UART_IER);
	/* The top four bits of the IER should always == 0 */
	if ((c & 0xf0) != 0)
		ok = 0;		/* failed */

	outb(0xaa, io_base + UART_SCR);
	/* Write arbitrary data into the scratch reg */
	c = inb(io_base + UART_SCR);
	/* If it comes back, it's OK */
	if (c != 0xaa)
		ok = 0;		/* failed */

	outb(0x55, io_base + UART_SCR);
	/* Write arbitrary data into the scratch reg */
	c = inb(io_base + UART_SCR);
	/* If it comes back, it's OK */
	if (c != 0x55)
		ok = 0;		/* failed */

	return ok;
}

static void snd_uart16550_do_open(snd_uart16550_t * uart)
{
	char byte;

	/* Initialize basic variables */
	uart->buff_in_count = 0;
	uart->buff_in = 0;
	uart->buff_out = 0;
	uart->fifo_limit = 1;
	uart->fifo_count = 0;
	uart->timer_running = 0;

	outb(UART_FCR_ENABLE_FIFO	/* Enable FIFO's (if available) */
	     | UART_FCR_CLEAR_RCVR	/* Clear receiver FIFO */
	     | UART_FCR_CLEAR_XMIT	/* Clear transmitter FIFO */
	     | UART_FCR_TRIGGER_4	/* Set FIFO trigger at 4-bytes */
	/* NOTE: interrupt generated after T=(time)4-bytes
	 * if less than UART_FCR_TRIGGER bytes received
	 */
	     ,uart->base + UART_FCR);	/* FIFO Control Register */

	if ((inb(uart->base + UART_IIR) & 0xf0) == 0xc0)
		uart->fifo_limit = 16;
	if (uart->divisor != 0) {
		uart->old_line_ctrl_reg = inb(uart->base + UART_LCR);
		outb(UART_LCR_DLAB	/* Divisor latch access bit */
		     ,uart->base + UART_LCR);	/* Line Control Register */
		uart->old_divisor_lsb = inb(uart->base + UART_DLL);
		uart->old_divisor_msb = inb(uart->base + UART_DLM);

		outb(uart->divisor
		     ,uart->base + UART_DLL);	/* Divisor Latch Low */
		outb(0
		     ,uart->base + UART_DLM);	/* Divisor Latch High */
		/* DLAB is reset to 0 in next outb() */
	}
	/* Set serial parameters (parity off, etc) */
	outb(UART_LCR_WLEN8	/* 8 data-bits */
	     | 0		/* 1 stop-bit */
	     | 0		/* parity off */
	     | 0		/* DLAB = 0 */
	     ,uart->base + UART_LCR);	/* Line Control Register */

	switch (uart->adaptor) {
	default:
		outb(UART_MCR_RTS	/* Set Request-To-Send line active */
		     | UART_MCR_DTR	/* Set Data-Terminal-Ready line active */
		     | UART_MCR_OUT2	/* Set OUT2 - not always required, but when
					 * it is, it is ESSENTIAL for enabling interrupts
				 */
		     ,uart->base + UART_MCR);	/* Modem Control Register */
		break;
	case SNDRV_SERIAL_MS124W_SA:
	case SNDRV_SERIAL_MS124W_MB:
		/* MS-124W can draw power from RTS and DTR if they
		   are in opposite states. */ 
		outb(UART_MCR_RTS | (0&UART_MCR_DTR) | UART_MCR_OUT2,
		     uart->base + UART_MCR);
		break;
	case SNDRV_SERIAL_MS124T:
		/* MS-124T can draw power from RTS and/or DTR (preferably
		   both) if they are both asserted. */
		outb(UART_MCR_RTS | UART_MCR_DTR | UART_MCR_OUT2,
		     uart->base + UART_MCR);
		break;
	}

	if (uart->irq < 0) {
		byte = (0 & UART_IER_RDI)	/* Disable Receiver data interrupt */
		    |(0 & UART_IER_THRI)	/* Disable Transmitter holding register empty interrupt */
		    ;
	} else if (uart->adaptor == SNDRV_SERIAL_MS124W_SA) {
		byte = UART_IER_RDI	/* Enable Receiver data interrupt */
		    | UART_IER_MSI	/* Enable Modem status interrupt */
		    ;
	} else if (uart->adaptor == SNDRV_SERIAL_GENERIC) {
		byte = UART_IER_RDI	/* Enable Receiver data interrupt */
		    | UART_IER_MSI	/* Enable Modem status interrupt */
		    | UART_IER_THRI	/* Enable Transmitter holding register empty interrupt */
		    ;
	} else {
		byte = UART_IER_RDI	/* Enable Receiver data interrupt */
		    | UART_IER_THRI	/* Enable Transmitter holding register empty interrupt */
		    ;
	}
	outb(byte, uart->base + UART_IER);	/* Interupt enable Register */

	inb(uart->base + UART_LSR);	/* Clear any pre-existing overrun indication */
	inb(uart->base + UART_IIR);	/* Clear any pre-existing transmit interrupt */
	inb(uart->base + UART_RX);	/* Clear any pre-existing receive interrupt */
}

static void snd_uart16550_do_close(snd_uart16550_t * uart)
{
	if (uart->irq < 0)
		snd_uart16550_del_timer(uart);

	/* NOTE: may need to disable interrupts before de-registering out handler.
	 * For now, the consequences are harmless.
	 */

	outb((0 & UART_IER_RDI)		/* Disable Receiver data interrupt */
	     |(0 & UART_IER_THRI)	/* Disable Transmitter holding register empty interrupt */
	     ,uart->base + UART_IER);	/* Interupt enable Register */

	switch (uart->adaptor) {
	default:
		outb((0 & UART_MCR_RTS)		/* Deactivate Request-To-Send line  */
		     |(0 & UART_MCR_DTR)	/* Deactivate Data-Terminal-Ready line */
		     |(0 & UART_MCR_OUT2)	/* Deactivate OUT2 */
		     ,uart->base + UART_MCR);	/* Modem Control Register */
	  break;
	case SNDRV_SERIAL_MS124W_SA:
	case SNDRV_SERIAL_MS124W_MB:
		/* MS-124W can draw power from RTS and DTR if they
		   are in opposite states; leave it powered. */ 
		outb(UART_MCR_RTS | (0&UART_MCR_DTR) | (0&UART_MCR_OUT2),
		     uart->base + UART_MCR);
		break;
	case SNDRV_SERIAL_MS124T:
		/* MS-124T can draw power from RTS and/or DTR (preferably
		   both) if they are both asserted; leave it powered. */
		outb(UART_MCR_RTS | UART_MCR_DTR | (0&UART_MCR_OUT2),
		     uart->base + UART_MCR);
		break;
	}

	inb(uart->base + UART_IIR);	/* Clear any outstanding interrupts */

	/* Restore old divisor */
	if (uart->divisor != 0) {
		outb(UART_LCR_DLAB		/* Divisor latch access bit */
		     ,uart->base + UART_LCR);	/* Line Control Register */
		outb(uart->old_divisor_lsb
		     ,uart->base + UART_DLL);	/* Divisor Latch Low */
		outb(uart->old_divisor_msb
		     ,uart->base + UART_DLM);	/* Divisor Latch High */
		/* Restore old LCR (data bits, stop bits, parity, DLAB) */
		outb(uart->old_line_ctrl_reg
		     ,uart->base + UART_LCR);	/* Line Control Register */
	}
}

static int snd_uart16550_input_open(snd_rawmidi_substream_t * substream)
{
	unsigned long flags;
	snd_uart16550_t *uart = snd_magic_cast(snd_uart16550_t, substream->rmidi->private_data, return -ENXIO);

	spin_lock_irqsave(&uart->open_lock, flags);
	if (uart->filemode == SERIAL_MODE_NOT_OPENED)
		snd_uart16550_do_open(uart);
	uart->filemode |= SERIAL_MODE_INPUT_OPEN;
	uart->midi_input[substream->number] = substream;
	spin_unlock_irqrestore(&uart->open_lock, flags);
	return 0;
}

static int snd_uart16550_input_close(snd_rawmidi_substream_t * substream)
{
	unsigned long flags;
	snd_uart16550_t *uart = snd_magic_cast(snd_uart16550_t, substream->rmidi->private_data, return -ENXIO);

	spin_lock_irqsave(&uart->open_lock, flags);
	uart->filemode &= ~SERIAL_MODE_INPUT_OPEN;
	uart->midi_input[substream->number] = NULL;
	if (uart->filemode == SERIAL_MODE_NOT_OPENED)
		snd_uart16550_do_close(uart);
	spin_unlock_irqrestore(&uart->open_lock, flags);
	return 0;
}

static void snd_uart16550_input_trigger(snd_rawmidi_substream_t * substream, int up)
{
	unsigned long flags;
	snd_uart16550_t *uart = snd_magic_cast(snd_uart16550_t, substream->rmidi->private_data, return);

	spin_lock_irqsave(&uart->open_lock, flags);
	if (up) {
		uart->filemode |= SERIAL_MODE_INPUT_TRIGGERED;
	} else {
		uart->filemode &= ~SERIAL_MODE_INPUT_TRIGGERED;
	}
	spin_unlock_irqrestore(&uart->open_lock, flags);
}

static int snd_uart16550_output_open(snd_rawmidi_substream_t * substream)
{
	unsigned long flags;
	snd_uart16550_t *uart = snd_magic_cast(snd_uart16550_t, substream->rmidi->private_data, return -ENXIO);

	spin_lock_irqsave(&uart->open_lock, flags);
	if (uart->filemode == SERIAL_MODE_NOT_OPENED)
		snd_uart16550_do_open(uart);
	uart->filemode |= SERIAL_MODE_OUTPUT_OPEN;
	uart->midi_output[substream->number] = substream;
	spin_unlock_irqrestore(&uart->open_lock, flags);
	return 0;
};

static int snd_uart16550_output_close(snd_rawmidi_substream_t * substream)
{
	unsigned long flags;
	snd_uart16550_t *uart = snd_magic_cast(snd_uart16550_t, substream->rmidi->private_data, return -ENXIO);

	spin_lock_irqsave(&uart->open_lock, flags);
	uart->filemode &= ~SERIAL_MODE_OUTPUT_OPEN;
	uart->midi_output[substream->number] = NULL;
	if (uart->filemode == SERIAL_MODE_NOT_OPENED)
		snd_uart16550_do_close(uart);
	spin_unlock_irqrestore(&uart->open_lock, flags);
	return 0;
};

inline static int snd_uart16550_buffer_can_write( snd_uart16550_t *uart, int Num )
{
	if( uart->buff_in_count + Num < TX_BUFF_SIZE )
		return 1;
	else
		return 0;
}

inline static int snd_uart16550_write_buffer(snd_uart16550_t *uart, unsigned char byte)
{
	unsigned short buff_in = uart->buff_in;
	if( uart->buff_in_count < TX_BUFF_SIZE ) {
		uart->tx_buff[buff_in] = byte;
		buff_in++;
		buff_in &= TX_BUFF_MASK;
		uart->buff_in = buff_in;
		uart->buff_in_count++;
		if (uart->irq < 0) /* polling mode */
			snd_uart16550_add_timer(uart);
		return 1;
	} else
		return 0;
}

static int snd_uart16550_output_byte(snd_uart16550_t *uart, snd_rawmidi_substream_t * substream, unsigned char midi_byte)
{
	if (uart->buff_in_count == 0                            /* Buffer empty? */
	    && ((uart->adaptor != SNDRV_SERIAL_MS124W_SA &&
	    uart->adaptor != SNDRV_SERIAL_GENERIC) ||
		(uart->fifo_count == 0                               /* FIFO empty? */
		 && (inb(uart->base + UART_MSR) & UART_MSR_CTS)))) { /* CTS? */

	        /* Tx Buffer Empty - try to write immediately */
		if ((inb(uart->base + UART_LSR) & UART_LSR_THRE) != 0) {
		        /* Transmitter holding register (and Tx FIFO) empty */
		        uart->fifo_count = 1;
			outb(midi_byte, uart->base + UART_TX);
		} else {
		        if (uart->fifo_count < uart->fifo_limit) {
			        uart->fifo_count++;
				outb(midi_byte, uart->base + UART_TX);
			} else {
			        /* Cannot write (buffer empty) - put char in buffer */
				snd_uart16550_write_buffer(uart, midi_byte);
			}
		}
	} else {
		if( !snd_uart16550_write_buffer(uart, midi_byte) ) {
			snd_printk("%s: Buffer overrun on device at 0x%lx\n",
				   uart->rmidi->name, uart->base);
			return 0;
		}
	}

	return 1;
}

static void snd_uart16550_output_write(snd_rawmidi_substream_t * substream)
{
	unsigned long flags;
	unsigned char midi_byte, addr_byte;
	snd_uart16550_t *uart = snd_magic_cast(snd_uart16550_t, substream->rmidi->private_data, return);
	char first;
	static unsigned long lasttime=0;
	
	/* Interupts are disabled during the updating of the tx_buff,
	 * since it is 'bad' to have two processes updating the same
	 * variables (ie buff_in & buff_out)
	 */

	spin_lock_irqsave(&uart->open_lock, flags);

	if (uart->irq < 0)	//polling
		snd_uart16550_io_loop(uart);

	if (uart->adaptor == SNDRV_SERIAL_MS124W_MB) {
		while (1) {
			/* buffer full? */
			/* in this mode we need two bytes of space */
			if (uart->buff_in_count > TX_BUFF_SIZE - 2)
				break;
			if (snd_rawmidi_transmit(substream, &midi_byte, 1) != 1)
				break;
#ifdef SNDRV_SERIAL_MS124W_MB_NOCOMBO
			/* select exactly one of the four ports */
			addr_byte = (1 << (substream->number + 4)) | 0x08;
#else
			/* select any combination of the four ports */
			addr_byte = (substream->number << 4) | 0x08;
			/* ...except none */
			if (addr_byte == 0x08) addr_byte = 0xf8;
#endif
			snd_uart16550_output_byte(uart, substream, addr_byte);
			/* send midi byte */
			snd_uart16550_output_byte(uart, substream, midi_byte);
		}
	} else {
		first = 0;
		while( 1 == snd_rawmidi_transmit_peek(substream, &midi_byte, 1) ) {
			/* Also send F5 after 3 seconds with no data to handle device disconnect */
			if (first == 0 && (uart->adaptor == SNDRV_SERIAL_SOUNDCANVAS ||
				uart->adaptor == SNDRV_SERIAL_GENERIC) &&
			   (uart->prev_out != substream->number || jiffies-lasttime > 3*HZ)) {

				if( snd_uart16550_buffer_can_write( uart, 3 ) ) {
					/* Roland Soundcanvas part selection */
					/* If this substream of the data is different previous
					   substream in this uart, send the change part event */
					uart->prev_out = substream->number;
					/* change part */
					snd_uart16550_output_byte(uart, substream, 0xf5);
					/* data */
					snd_uart16550_output_byte(uart, substream, uart->prev_out + 1);
					/* If midi_byte is a data byte, send the previous status byte */
					if ((midi_byte < 0x80) && (uart->adaptor == SNDRV_SERIAL_SOUNDCANVAS))
						snd_uart16550_output_byte(uart, substream, uart->prev_status[uart->prev_out]);
				} else if( !uart->drop_on_full )
					break;

			}

			/* send midi byte */
			if( !snd_uart16550_output_byte(uart, substream, midi_byte) && !uart->drop_on_full )
				break;

			if (midi_byte >= 0x80 && midi_byte < 0xf0)
				uart->prev_status[uart->prev_out] = midi_byte;
			first = 1;

			snd_rawmidi_transmit_ack( substream, 1 );
		}
		lasttime = jiffies;
	}
	spin_unlock_irqrestore(&uart->open_lock, flags);
}

static void snd_uart16550_output_trigger(snd_rawmidi_substream_t * substream, int up)
{
	unsigned long flags;
	snd_uart16550_t *uart = snd_magic_cast(snd_uart16550_t, substream->rmidi->private_data, return);

	spin_lock_irqsave(&uart->open_lock, flags);
	if (up) {
		uart->filemode |= SERIAL_MODE_OUTPUT_TRIGGERED;
	} else {
		uart->filemode &= ~SERIAL_MODE_OUTPUT_TRIGGERED;
	}
	spin_unlock_irqrestore(&uart->open_lock, flags);
	if (up)
		snd_uart16550_output_write(substream);
}

static snd_rawmidi_ops_t snd_uart16550_output =
{
	.open =		snd_uart16550_output_open,
	.close =	snd_uart16550_output_close,
	.trigger =	snd_uart16550_output_trigger,
};

static snd_rawmidi_ops_t snd_uart16550_input =
{
	.open =		snd_uart16550_input_open,
	.close =	snd_uart16550_input_close,
	.trigger =	snd_uart16550_input_trigger,
};

static int snd_uart16550_free(snd_uart16550_t *uart)
{
	if (uart->irq >= 0)
		free_irq(uart->irq, (void *)uart);
	if (uart->res_base) {
		release_resource(uart->res_base);
		kfree_nocheck(uart->res_base);
	}
	snd_magic_kfree(uart);
	return 0;
};

static int snd_uart16550_dev_free(snd_device_t *device)
{
	snd_uart16550_t *uart = snd_magic_cast(snd_uart16550_t, device->device_data, return -ENXIO);
	return snd_uart16550_free(uart);
}

static int __init snd_uart16550_create(snd_card_t * card,
				       unsigned long iobase,
				       int irq,
				       unsigned int speed,
				       unsigned int base,
				       int adaptor,
				       int droponfull,
				       snd_uart16550_t **ruart)
{
	static snd_device_ops_t ops = {
		.dev_free =	snd_uart16550_dev_free,
	};
	snd_uart16550_t *uart;
	int err;


	if ((uart = snd_magic_kcalloc(snd_uart16550_t, 0, GFP_KERNEL)) == NULL)
		return -ENOMEM;
	uart->adaptor = adaptor;
	uart->card = card;
	spin_lock_init(&uart->open_lock);
	uart->irq = -1;
	uart->base = iobase;
	uart->drop_on_full = droponfull;

	if ((err = snd_uart16550_detect(uart)) <= 0) {
		printk(KERN_ERR "no UART detected at 0x%lx\n", iobase);
		return err;
	}

	if (irq >= 0 && irq != SNDRV_AUTO_IRQ) {
		if (request_irq(irq, snd_uart16550_interrupt,
				SA_INTERRUPT, "Serial MIDI", (void *) uart)) {
			snd_printk("irq %d busy. Using Polling.\n", irq);
		} else {
			uart->irq = irq;
		}
	}
	uart->divisor = base / speed;
	uart->speed = base / (unsigned int)uart->divisor;
	uart->speed_base = base;
	uart->prev_out = -1;
	uart->prev_in = 0;
	uart->rstatus = 0;
	memset(uart->prev_status, 0x80, sizeof(unsigned char) * SNDRV_SERIAL_MAX_OUTS);
	init_timer(&uart->buffer_timer);
	uart->buffer_timer.function = snd_uart16550_buffer_timer;
	uart->buffer_timer.data = (unsigned long)uart;
	uart->timer_running = 0;

	/* Register device */
	if ((err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, uart, &ops)) < 0) {
		snd_uart16550_free(uart);
		return err;
	}

	switch (uart->adaptor) {
	case SNDRV_SERIAL_MS124W_SA:
	case SNDRV_SERIAL_MS124W_MB:
		/* MS-124W can draw power from RTS and DTR if they
		   are in opposite states. */ 
		outb(UART_MCR_RTS | (0&UART_MCR_DTR), uart->base + UART_MCR);
		break;
	case SNDRV_SERIAL_MS124T:
		/* MS-124T can draw power from RTS and/or DTR (preferably
		   both) if they are asserted. */
		outb(UART_MCR_RTS | UART_MCR_DTR, uart->base + UART_MCR);
		break;
	default:
		break;
	}

	if (ruart)
		*ruart = uart;

	return 0;
}

static int __init snd_uart16550_rmidi(snd_uart16550_t *uart, int device, int outs, int ins, snd_rawmidi_t **rmidi)
{
	snd_rawmidi_t *rrawmidi;
	int err;

	if ((err = snd_rawmidi_new(uart->card, "UART Serial MIDI", device, outs, ins, &rrawmidi)) < 0)
		return err;
	snd_rawmidi_set_ops(rrawmidi, SNDRV_RAWMIDI_STREAM_INPUT, &snd_uart16550_input);
	snd_rawmidi_set_ops(rrawmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &snd_uart16550_output);
	sprintf(rrawmidi->name, "uart16550 MIDI #%d", device);
	rrawmidi->info_flags = SNDRV_RAWMIDI_INFO_OUTPUT |
			       SNDRV_RAWMIDI_INFO_INPUT |
			       SNDRV_RAWMIDI_INFO_DUPLEX;
	rrawmidi->private_data = uart;
	if (rmidi)
		*rmidi = rrawmidi;
	return 0;
}

static int __init snd_serial_probe(int dev)
{
	snd_card_t *card;
	snd_uart16550_t *uart;
	int err;

	if (!enable[dev])
		return -ENOENT;

	switch (adaptor[dev]) {
	case SNDRV_SERIAL_SOUNDCANVAS:
		ins[dev] = 1;
		break;
	case SNDRV_SERIAL_MS124T:
	case SNDRV_SERIAL_MS124W_SA:
		outs[dev] = 1;
		ins[dev] = 1;
		break;
	case SNDRV_SERIAL_MS124W_MB:
		outs[dev] = 16;
		ins[dev] = 1;
		break;
	case SNDRV_SERIAL_GENERIC:
		break;
	default:
		snd_printk("Adaptor type is out of range 0-%d (%d)\n",
			   SNDRV_SERIAL_MAX_ADAPTOR, adaptor[dev]);
		return -ENODEV;
	}

	if (outs[dev] < 1 || outs[dev] > SNDRV_SERIAL_MAX_OUTS) {
		snd_printk("Count of outputs is out of range 1-%d (%d)\n",
			   SNDRV_SERIAL_MAX_OUTS, outs[dev]);
		return -ENODEV;
	}

	if (ins[dev] < 1 || ins[dev] > SNDRV_SERIAL_MAX_INS) {
		snd_printk("Count of inputs is out of range 1-%d (%d)\n",
			   SNDRV_SERIAL_MAX_INS, ins[dev]);
		return -ENODEV;
	}

	card  = snd_card_new(index[dev], id[dev], THIS_MODULE, 0);
	if (card == NULL)
		return -ENOMEM;

	strcpy(card->driver, "Serial");
	strcpy(card->shortname, "Serial midi (uart16550A)");

	if ((err = snd_uart16550_create(card,
					port[dev],
					irq[dev],
					speed[dev],
					base[dev],
					adaptor[dev],
					droponfull[dev],
					&uart)) < 0) {
		snd_card_free(card);
		return err;
	}

	if ((err = snd_uart16550_rmidi(uart, 0, outs[dev], ins[dev], &uart->rmidi)) < 0) {
		snd_card_free(card);
		return err;
	}

	sprintf(card->longname, "%s at 0x%lx, irq %d speed %d div %d outs %d ins %d adaptor %s droponfull %d",
		card->shortname,
		uart->base,
		uart->irq,
		uart->speed,
		(int)uart->divisor,
		outs[dev],
		ins[dev],
		adaptor_names[uart->adaptor],
		uart->drop_on_full);

	if ((err = snd_card_register(card)) < 0) {
		snd_card_free(card);
		return err;
	}
	snd_serial_cards[dev] = card;
	return 0;
}

static int __init alsa_card_serial_init(void)
{
	int dev = 0;
	int cards = 0;

	for (dev = 0; dev < SNDRV_CARDS; dev++) {
		if (snd_serial_probe(dev) == 0)
			cards++;
	}

	if (cards == 0) {
#ifdef MODULE
		printk(KERN_ERR "serial midi soundcard not found or device busy\n");
#endif
		return -ENODEV;
	}
	return 0;
}

static void __exit alsa_card_serial_exit(void)
{
	int dev;

	for (dev = 0; dev < SNDRV_CARDS; dev++) {
		if (snd_serial_cards[dev] != NULL)
			snd_card_free(snd_serial_cards[dev]);
	}
}

module_init(alsa_card_serial_init)
module_exit(alsa_card_serial_exit)
