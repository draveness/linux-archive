/*
** mux.c:
**	serial driver for the Mux console found in some PA-RISC servers.
**
**	(c) Copyright 2002 Ryan Bradetich
**	(c) Copyright 2002 Hewlett-Packard Company
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This Driver currently only supports the console (port 0) on the MUX.
** Additional work will be needed on this driver to enable the full
** functionality of the MUX.
**
*/

#include <linux/config.h>
#include <linux/module.h>
#include <linux/tty.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/serial.h>
#include <linux/console.h>
#include <linux/slab.h>
#include <linux/delay.h> /* for udelay */
#include <linux/device.h>
#include <asm/io.h>
#include <asm/parisc-device.h>

#ifdef CONFIG_MAGIC_SYSRQ
#include <linux/sysrq.h>
#define SUPPORT_SYSRQ
#endif

#include <linux/serial_core.h>

#define MUX_OFFSET 0x800
#define MUX_LINE_OFFSET 0x80

#define MUX_FIFO_SIZE 255
#define MUX_POLL_DELAY (30 * HZ / 1000)

#define IO_DATA_REG_OFFSET 0x3c
#define IO_DCOUNT_REG_OFFSET 0x40

#define MUX_EOFIFO(status) ((status & 0xF000) == 0xF000)
#define MUX_STATUS(status) ((status & 0xF000) == 0x8000)
#define MUX_BREAK(status) ((status & 0xF000) == 0x2000)

#define MUX_NR 256
static unsigned int port_cnt = 0;
static struct uart_port mux_ports[MUX_NR];

static struct uart_driver mux_driver = {
	.owner = THIS_MODULE,
	.driver_name = "ttyB",
	.dev_name = "ttyB",
	.major = MUX_MAJOR,
	.minor = 0,
	.nr = MUX_NR,
};

static struct timer_list mux_timer;

#define UART_PUT_CHAR(p, c) __raw_writel((c), (unsigned long)(p)->membase + IO_DATA_REG_OFFSET)
#define UART_GET_FIFO_CNT(p) __raw_readl((unsigned long)(p)->membase + IO_DCOUNT_REG_OFFSET)
#define GET_MUX_PORTS(iodc_data) ((((iodc_data)[4] & 0xf0) >> 4) * 8) + 8

/**
 * mux_tx_empty - Check if the transmitter fifo is empty.
 * @port: Ptr to the uart_port.
 *
 * This function test if the transmitter fifo for the port
 * described by 'port' is empty.  If it is empty, this function
 * should return TIOCSER_TEMT, otherwise return 0.
 */
static unsigned int mux_tx_empty(struct uart_port *port)
{
	unsigned int cnt = __raw_readl((unsigned long)port->membase 
				+ IO_DCOUNT_REG_OFFSET);

	return cnt ? 0 : TIOCSER_TEMT;
} 

/**
 * mux_set_mctrl - Set the current state of the modem control inputs.
 * @ports: Ptr to the uart_port.
 * @mctrl: Modem control bits.
 *
 * The Serial MUX does not support CTS, DCD or DSR so this function
 * is ignored.
 */
static void mux_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

/**
 * mux_get_mctrl - Returns the current state of modem control inputs.
 * @port: Ptr to the uart_port.
 *
 * The Serial MUX does not support CTS, DCD or DSR so these lines are
 * treated as permanently active.
 */
static unsigned int mux_get_mctrl(struct uart_port *port)
{ 
	return TIOCM_CAR | TIOCM_DSR | TIOCM_CTS;
}

/**
 * mux_stop_tx - Stop transmitting characters.
 * @port: Ptr to the uart_port.
 * @tty_stop: tty layer issue this command?
 *
 * The Serial MUX does not support this function.
 */
static void mux_stop_tx(struct uart_port *port, unsigned int tty_stop)
{
}

/**
 * mux_start_tx - Start transmitting characters.
 * @port: Ptr to the uart_port.
 * @tty_start: tty layer issue this command?
 *
 * The Serial Mux does not support this function.
 */
static void mux_start_tx(struct uart_port *port, unsigned int tty_start)
{
}

/**
 * mux_stop_rx - Stop receiving characters.
 * @port: Ptr to the uart_port.
 *
 * The Serial Mux does not support this function.
 */
static void mux_stop_rx(struct uart_port *port)
{
}

/**
 * mux_enable_ms - Enable modum status interrupts.
 * @port: Ptr to the uart_port.
 *
 * The Serial Mux does not support this function.
 */
static void mux_enable_ms(struct uart_port *port)
{
}

/**
 * mux_break_ctl - Control the transmitssion of a break signal.
 * @port: Ptr to the uart_port.
 * @break_state: Raise/Lower the break signal.
 *
 * The Serial Mux does not support this function.
 */
static void mux_break_ctl(struct uart_port *port, int break_state)
{
}

/**
 * mux_write - Write chars to the mux fifo.
 * @port: Ptr to the uart_port.
 *
 * This function writes all the data from the uart buffer to
 * the mux fifo.
 */
static void mux_write(struct uart_port *port)
{
	int count;
	struct circ_buf *xmit = &port->info->xmit;

	if(port->x_char) {
		UART_PUT_CHAR(port, port->x_char);
		port->icount.tx++;
		port->x_char = 0;
		return;
	}

	if(uart_circ_empty(xmit) || uart_tx_stopped(port)) {
		mux_stop_tx(port, 0);
		return;
	}

	count = (port->fifosize) - UART_GET_FIFO_CNT(port);
	do {
		UART_PUT_CHAR(port, xmit->buf[xmit->tail]);
		xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
		port->icount.tx++;
		if(uart_circ_empty(xmit))
			break;

	} while(--count > 0);

	while(UART_GET_FIFO_CNT(port)) 
		udelay(1);

	if(uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(port);

	if (uart_circ_empty(xmit))
		mux_stop_tx(port, 0);
}

/**
 * mux_read - Read chars from the mux fifo.
 * @port: Ptr to the uart_port.
 *
 * This reads all available data from the mux's fifo and pushes
 * the data to the tty layer.
 */
static void mux_read(struct uart_port *port)
{
	int data;
	struct tty_struct *tty = port->info->tty;
	__u32 start_count = port->icount.rx;

	while(1) {
		data = __raw_readl((unsigned long)port->membase
						+ IO_DATA_REG_OFFSET);

		if (MUX_STATUS(data))
			continue;

		if (MUX_EOFIFO(data))
			break;

		if (tty->flip.count >= TTY_FLIPBUF_SIZE)
			continue;

		*tty->flip.char_buf_ptr = data & 0xffu;
		*tty->flip.flag_buf_ptr = TTY_NORMAL;
		port->icount.rx++;

		if (MUX_BREAK(data)) {
			port->icount.brk++;
			if(uart_handle_break(port))
				continue;
		}

		if (uart_handle_sysrq_char(port, data & 0xffu, NULL))
			continue;

		tty->flip.flag_buf_ptr++;
		tty->flip.char_buf_ptr++;
		tty->flip.count++;
	}
	
	if (start_count != port->icount.rx) {
		tty_flip_buffer_push(tty);
	}
}

/**
 * mux_startup - Initialize the port.
 * @port: Ptr to the uart_port.
 *
 * Grab any resources needed for this port and start the
 * mux timer.
 */
static int mux_startup(struct uart_port *port)
{
	mod_timer(&mux_timer, jiffies + MUX_POLL_DELAY);
	return 0;
}

/**
 * mux_shutdown - Disable the port.
 * @port: Ptr to the uart_port.
 *
 * Release any resources needed for the port.
 */
static void mux_shutdown(struct uart_port *port)
{
}

/**
 * mux_set_termios - Chane port parameters.
 * @port: Ptr to the uart_port.
 * @termios: new termios settings.
 * @old: old termios settings.
 *
 * The Serial Mux does not support this function.
 */
static void
mux_set_termios(struct uart_port *port, struct termios *termios,
	        struct termios *old)
{
}

/**
 * mux_type - Describe the port.
 * @port: Ptr to the uart_port.
 *
 * Return a pointer to a string constant describing the
 * specified port.
 */
static const char *mux_type(struct uart_port *port)
{
	return "Mux";
}

/**
 * mux_release_port - Release memory and IO regions.
 * @port: Ptr to the uart_port.
 * 
 * Release any memory and IO region resources currently in use by
 * the port.
 */
static void mux_release_port(struct uart_port *port)
{
}

/**
 * mux_request_port - Request memory and IO regions.
 * @port: Ptr to the uart_port.
 *
 * Request any memory and IO region resources required by the port.
 * If any fail, no resources should be registered when this function
 * returns, and it should return -EBUSY on failure.
 */
static int mux_request_port(struct uart_port *port)
{
	return 0;
}

/**
 * mux_config_port - Perform port autoconfiguration.
 * @port: Ptr to the uart_port.
 * @type: Bitmask of required configurations.
 *
 * Perform any autoconfiguration steps for the port.  This functino is
 * called if the UPF_BOOT_AUTOCONF flag is specified for the port.
 * [Note: This is required for now because of a bug in the Serial core.
 *  rmk has already submitted a patch to linus, should be available for
 *  2.5.47.]
 */
static void mux_config_port(struct uart_port *port, int type)
{
	port->type = PORT_MUX;
}

/**
 * mux_verify_port - Verify the port information.
 * @port: Ptr to the uart_port.
 * @ser: Ptr to the serial information.
 *
 * Verify the new serial port information contained within serinfo is
 * suitable for this port type.
 */
static int mux_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	if(port->membase == NULL)
		return -EINVAL;

	return 0;
}

/**
 * mux_drv_poll - Mux poll function.
 * @unused: Unused variable
 *
 * This function periodically polls the Serial MUX to check for new data.
 */
static void mux_poll(unsigned long unused)
{  
	int i;

	for(i = 0; i < port_cnt; ++i) {
		if(!mux_ports[i].info)
			continue;

		mux_read(&mux_ports[i]);
		mux_write(&mux_ports[i]);
	}

	mod_timer(&mux_timer, jiffies + MUX_POLL_DELAY);
}


#ifdef CONFIG_SERIAL_MUX_CONSOLE
static void mux_console_write(struct console *co, const char *s, unsigned count)
{
        while(count--)
                pdc_iodc_putc(*s++);
}

static int mux_console_setup(struct console *co, char *options)
{
        return 0;
}

struct tty_driver *mux_console_device(struct console *co, int *index)
{
        *index = co->index;
	return mux_driver.tty_driver;
}

static struct console mux_console = {
	.name =		"ttyB",
	.write =	mux_console_write,
	.device =	mux_console_device,
	.setup =	mux_console_setup,
	.flags =	CON_BOOT|CON_PRINTBUFFER|CON_ENABLED,
	.index =	0,
};

#define MUX_CONSOLE	&mux_console
#else
#define MUX_CONSOLE	NULL
#endif

static struct uart_ops mux_pops = {
	.tx_empty =		mux_tx_empty,
	.set_mctrl =		mux_set_mctrl,
	.get_mctrl =		mux_get_mctrl,
	.stop_tx =		mux_stop_tx,
	.start_tx =		mux_start_tx,
	.stop_rx =		mux_stop_rx,
	.enable_ms =		mux_enable_ms,
	.break_ctl =		mux_break_ctl,
	.startup =		mux_startup,
	.shutdown =		mux_shutdown,
	.set_termios =		mux_set_termios,
	.type =			mux_type,
	.release_port =		mux_release_port,
	.request_port =		mux_request_port,
	.config_port =		mux_config_port,
	.verify_port =		mux_verify_port,
};

/**
 * mux_probe - Determine if the Serial Mux should claim this device.
 * @dev: The parisc device.
 *
 * Deterimine if the Serial Mux should claim this chip (return 0)
 * or not (return 1).
 */
static int __init mux_probe(struct parisc_device *dev)
{
	int i, status, ports;
	u8 iodc_data[32];
	unsigned long bytecnt;
	struct uart_port *port;

	status = pdc_iodc_read(&bytecnt, dev->hpa, 0, iodc_data, 32);
	if(status != PDC_OK) {
		printk(KERN_ERR "Serial mux: Unable to read IODC.\n");
		return 1;
	}

	ports = GET_MUX_PORTS(iodc_data);
 	printk(KERN_INFO "Serial mux driver (%d ports) Revision: 0.3\n", ports);

	if(!port_cnt) {
		mux_driver.cons = MUX_CONSOLE;

		status = uart_register_driver(&mux_driver);
		if(status) {
			printk(KERN_ERR "Serial mux: Unable to register driver.\n");
			return 1;
		}

		init_timer(&mux_timer);
		mux_timer.function = mux_poll;
	}

	for(i = 0; i < ports; ++i, ++port_cnt) {
		port = &mux_ports[port_cnt];
		port->iobase	= 0;
		port->mapbase	= dev->hpa + MUX_OFFSET + (i * MUX_LINE_OFFSET);
		port->membase	= ioremap(port->mapbase, MUX_LINE_OFFSET);
		port->iotype	= SERIAL_IO_MEM;
		port->type	= PORT_MUX;
		port->irq	= SERIAL_IRQ_NONE;
		port->uartclk	= 0;
		port->fifosize	= MUX_FIFO_SIZE;
		port->ops	= &mux_pops;
		port->flags	= UPF_BOOT_AUTOCONF;
		port->line	= port_cnt;
		status = uart_add_one_port(&mux_driver, port);
		BUG_ON(status);
	}

#ifdef CONFIG_SERIAL_MUX_CONSOLE
        register_console(&mux_console);
#endif
	return 0;
}

static struct parisc_device_id mux_tbl[] = {
	{ HPHW_A_DIRECT, HVERSION_REV_ANY_ID, HVERSION_ANY_ID, 0x0000D },
	{ 0, }
};

MODULE_DEVICE_TABLE(parisc, mux_tbl);

static struct parisc_driver serial_mux_driver = {
	.name =		"Serial MUX",
	.id_table =	mux_tbl,
	.probe =	mux_probe,
};

/**
 * mux_init - Serial MUX initalization procedure.
 *
 * Register the Serial MUX driver.
 */
static int __init mux_init(void)
{
	return register_parisc_driver(&serial_mux_driver);
}

/**
 * mux_exit - Serial MUX cleanup procedure.
 *
 * Unregister the Serial MUX driver from the tty layer.
 */
static void __exit mux_exit(void)
{
	int i;

	for (i = 0; i < port_cnt; i++) {
		uart_remove_one_port(&mux_driver, &mux_ports[i]);
	}

	uart_unregister_driver(&mux_driver);
}

module_init(mux_init);
module_exit(mux_exit);

MODULE_AUTHOR("Ryan Bradetich");
MODULE_DESCRIPTION("Serial MUX driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS_CHARDEV_MAJOR(MUX_MAJOR);
