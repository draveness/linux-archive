/*
 *  linux/drivers/char/serial_core.h
 *
 *  Copyright (C) 2000 Deep Blue Solutions Ltd.
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * The type definitions.  These are from Ted Ts'o's serial.h
 */
#define PORT_UNKNOWN	0
#define PORT_8250	1
#define PORT_16450	2
#define PORT_16550	3
#define PORT_16550A	4
#define PORT_CIRRUS	5
#define PORT_16650	6
#define PORT_16650V2	7
#define PORT_16750	8
#define PORT_STARTECH	9
#define PORT_16C950	10
#define PORT_16654	11
#define PORT_16850	12
#define PORT_RSA	13
#define PORT_NS16550A	14
#define PORT_XSCALE	15
#define PORT_MAX_8250	15	/* max port ID */

/*
 * ARM specific type numbers.  These are not currently guaranteed
 * to be implemented, and will change in the future.  These are
 * separate so any additions to the old serial.c that occur before
 * we are merged can be easily merged here.
 */
#define PORT_PXA	31
#define PORT_AMBA	32
#define PORT_CLPS711X	33
#define PORT_SA1100	34
#define PORT_UART00	35
#define PORT_21285	37

/* Sparc type numbers.  */
#define PORT_SUNZILOG	38
#define PORT_SUNSAB	39

/* NEC v850.  */
#define PORT_V850E_UART	40

/* DZ */
#define PORT_DZ		47

/* Parisc type numbers. */
#define PORT_MUX	48

/* Macintosh Zilog type numbers */
#define PORT_MAC_ZILOG	50	/* m68k : not yet implemented */
#define PORT_PMAC_ZILOG	51

/* SH-SCI */
#define PORT_SCI	52
#define PORT_SCIF	53
#define PORT_IRDA	54

/* Samsung S3C2410 SoC and derivatives thereof */
#define PORT_S3C2410    55

/* SGI IP22 aka Indy / Challenge S / Indigo 2 */
#define PORT_IP22ZILOG	56

/* Sharp LH7a40x -- an ARM9 SoC series */
#define PORT_LH7A40X	57

/* PPC CPM type number */
#define PORT_CPM        58

/* MPC52xx type numbers */
#define PORT_MPC52xx	59

#ifdef __KERNEL__

#include <linux/config.h>
#include <linux/interrupt.h>
#include <linux/circ_buf.h>
#include <linux/spinlock.h>
#include <linux/sched.h>

struct uart_port;
struct uart_info;
struct serial_struct;
struct device;

/*
 * This structure describes all the operations that can be
 * done on the physical hardware.
 */
struct uart_ops {
	unsigned int	(*tx_empty)(struct uart_port *);
	void		(*set_mctrl)(struct uart_port *, unsigned int mctrl);
	unsigned int	(*get_mctrl)(struct uart_port *);
	void		(*stop_tx)(struct uart_port *, unsigned int tty_stop);
	void		(*start_tx)(struct uart_port *, unsigned int tty_start);
	void		(*send_xchar)(struct uart_port *, char ch);
	void		(*stop_rx)(struct uart_port *);
	void		(*enable_ms)(struct uart_port *);
	void		(*break_ctl)(struct uart_port *, int ctl);
	int		(*startup)(struct uart_port *);
	void		(*shutdown)(struct uart_port *);
	void		(*set_termios)(struct uart_port *, struct termios *new,
				       struct termios *old);
	void		(*pm)(struct uart_port *, unsigned int state,
			      unsigned int oldstate);
	int		(*set_wake)(struct uart_port *, unsigned int state);

	/*
	 * Return a string describing the type of the port
	 */
	const char *(*type)(struct uart_port *);

	/*
	 * Release IO and memory resources used by the port.
	 * This includes iounmap if necessary.
	 */
	void		(*release_port)(struct uart_port *);

	/*
	 * Request IO and memory resources used by the port.
	 * This includes iomapping the port if necessary.
	 */
	int		(*request_port)(struct uart_port *);
	void		(*config_port)(struct uart_port *, int);
	int		(*verify_port)(struct uart_port *, struct serial_struct *);
	int		(*ioctl)(struct uart_port *, unsigned int, unsigned long);
};

#define UART_CONFIG_TYPE	(1 << 0)
#define UART_CONFIG_IRQ		(1 << 1)

struct uart_icount {
	__u32	cts;
	__u32	dsr;
	__u32	rng;
	__u32	dcd;
	__u32	rx;
	__u32	tx;
	__u32	frame;
	__u32	overrun;
	__u32	parity;
	__u32	brk;
	__u32	buf_overrun;
};

struct uart_port {
	spinlock_t		lock;			/* port lock */
	unsigned int		iobase;			/* in/out[bwl] */
	char			*membase;		/* read/write[bwl] */
	unsigned int		irq;			/* irq number */
	unsigned int		uartclk;		/* base uart clock */
	unsigned char		fifosize;		/* tx fifo size */
	unsigned char		x_char;			/* xon/xoff char */
	unsigned char		regshift;		/* reg offset shift */
	unsigned char		iotype;			/* io access style */

#define UPIO_PORT		(0)
#define UPIO_HUB6		(1)
#define UPIO_MEM		(2)

	unsigned int		read_status_mask;	/* driver specific */
	unsigned int		ignore_status_mask;	/* driver specific */
	struct uart_info	*info;			/* pointer to parent info */
	struct uart_icount	icount;			/* statistics */

	struct console		*cons;			/* struct console, if any */
#ifdef CONFIG_SERIAL_CORE_CONSOLE
	unsigned long		sysrq;			/* sysrq timeout */
#endif

	unsigned int		flags;

#define UPF_FOURPORT		(1 << 1)
#define UPF_SAK			(1 << 2)
#define UPF_SPD_MASK		(0x1030)
#define UPF_SPD_HI		(0x0010)
#define UPF_SPD_VHI		(0x0020)
#define UPF_SPD_CUST		(0x0030)
#define UPF_SPD_SHI		(0x1000)
#define UPF_SPD_WARP		(0x1010)
#define UPF_SKIP_TEST		(1 << 6)
#define UPF_AUTO_IRQ		(1 << 7)
#define UPF_HARDPPS_CD		(1 << 11)
#define UPF_LOW_LATENCY		(1 << 13)
#define UPF_BUGGY_UART		(1 << 14)
#define UPF_AUTOPROBE		(1 << 15)
#define UPF_MAGIC_MULTIPLIER	(1 << 16)
#define UPF_BOOT_ONLYMCA	(1 << 22)
#define UPF_CONS_FLOW		(1 << 23)
#define UPF_SHARE_IRQ		(1 << 24)
#define UPF_BOOT_AUTOCONF	(1 << 28)
#define UPF_IOREMAP		(1 << 31)

#define UPF_CHANGE_MASK		(0x17fff)
#define UPF_USR_MASK		(UPF_SPD_MASK|UPF_LOW_LATENCY)

	unsigned int		mctrl;			/* current modem ctrl settings */
	unsigned int		timeout;		/* character-based timeout */
	unsigned int		type;			/* port type */
	struct uart_ops		*ops;
	unsigned int		custom_divisor;
	unsigned int		line;			/* port index */
	unsigned long		mapbase;		/* for ioremap */
	struct device		*dev;			/* parent device */
	unsigned char		hub6;			/* this should be in the 8250 driver */
	unsigned char		unused[3];
};

/*
 * This is the state information which is persistent across opens.
 * The low level driver must not to touch any elements contained
 * within.
 */
struct uart_state {
	unsigned int		close_delay;
	unsigned int		closing_wait;

#define USF_CLOSING_WAIT_INF	(0)
#define USF_CLOSING_WAIT_NONE	(65535)

	int			count;
	int			pm_state;
	struct uart_info	*info;
	struct uart_port	*port;

	struct semaphore	sem;
};

#define UART_XMIT_SIZE 1024
/*
 * This is the state information which is only valid when the port
 * is open; it may be freed by the core driver once the device has
 * been closed.  Either the low level driver or the core can modify
 * stuff here.
 */
struct uart_info {
	struct tty_struct	*tty;
	struct circ_buf		xmit;
	unsigned int		flags;

/*
 * These are the flags that specific to info->flags, and reflect our
 * internal state.  They can not be accessed via port->flags.  Low
 * level drivers must not change these, but may query them instead.
 */
#define UIF_CHECK_CD		(1 << 25)
#define UIF_CTS_FLOW		(1 << 26)
#define UIF_NORMAL_ACTIVE	(1 << 29)
#define UIF_INITIALIZED		(1 << 31)

	unsigned char		*tmpbuf;
	struct semaphore	tmpbuf_sem;

	int			blocked_open;

	struct tasklet_struct	tlet;

	wait_queue_head_t	open_wait;
	wait_queue_head_t	delta_msr_wait;
};

/* number of characters left in xmit buffer before we ask for more */
#define WAKEUP_CHARS		256

struct module;
struct tty_driver;

struct uart_driver {
	struct module		*owner;
	const char		*driver_name;
	const char		*dev_name;
	const char		*devfs_name;
	int			 major;
	int			 minor;
	int			 nr;
	struct console		*cons;

	/*
	 * these are private; the low level driver should not
	 * touch these; they should be initialised to NULL
	 */
	struct uart_state	*state;
	struct tty_driver	*tty_driver;
};

void uart_write_wakeup(struct uart_port *port);

/*
 * Baud rate helpers.
 */
void uart_update_timeout(struct uart_port *port, unsigned int cflag,
			 unsigned int baud);
unsigned int uart_get_baud_rate(struct uart_port *port, struct termios *termios,
				struct termios *old, unsigned int min,
				unsigned int max);
unsigned int uart_get_divisor(struct uart_port *port, unsigned int baud);

/*
 * Console helpers.
 */
struct uart_port *uart_get_console(struct uart_port *ports, int nr,
				   struct console *c);
void uart_parse_options(char *options, int *baud, int *parity, int *bits,
			int *flow);
int uart_set_options(struct uart_port *port, struct console *co, int baud,
		     int parity, int bits, int flow);
struct tty_driver *uart_console_device(struct console *co, int *index);

/*
 * Port/driver registration/removal
 */
int uart_register_driver(struct uart_driver *uart);
void uart_unregister_driver(struct uart_driver *uart);
void uart_unregister_port(struct uart_driver *reg, int line);
int uart_register_port(struct uart_driver *reg, struct uart_port *port);
int uart_add_one_port(struct uart_driver *reg, struct uart_port *port);
int uart_remove_one_port(struct uart_driver *reg, struct uart_port *port);

/*
 * Power Management
 */
int uart_suspend_port(struct uart_driver *reg, struct uart_port *port);
int uart_resume_port(struct uart_driver *reg, struct uart_port *port);

#define uart_circ_empty(circ)		((circ)->head == (circ)->tail)
#define uart_circ_clear(circ)		((circ)->head = (circ)->tail = 0)

#define uart_circ_chars_pending(circ)	\
	(CIRC_CNT((circ)->head, (circ)->tail, UART_XMIT_SIZE))

#define uart_circ_chars_free(circ)	\
	(CIRC_SPACE((circ)->head, (circ)->tail, UART_XMIT_SIZE))

#define uart_tx_stopped(port)		\
	((port)->info->tty->stopped || (port)->info->tty->hw_stopped)

/*
 * The following are helper functions for the low level drivers.
 */
#ifdef SUPPORT_SYSRQ
static inline int
uart_handle_sysrq_char(struct uart_port *port, unsigned int ch,
		       struct pt_regs *regs)
{
	if (port->sysrq) {
		if (ch && time_before(jiffies, port->sysrq)) {
			handle_sysrq(ch, regs, NULL);
			port->sysrq = 0;
			return 1;
		}
		port->sysrq = 0;
	}
	return 0;
}
#else
#define uart_handle_sysrq_char(port,ch,regs)	(0)
#endif

/*
 * We do the SysRQ and SAK checking like this...
 */
static inline int uart_handle_break(struct uart_port *port)
{
	struct uart_info *info = port->info;
#ifdef SUPPORT_SYSRQ
	if (port->cons && port->cons->index == port->line) {
		if (!port->sysrq) {
			port->sysrq = jiffies + HZ*5;
			return 1;
		}
		port->sysrq = 0;
	}
#endif
	if (info->flags & UPF_SAK)
		do_SAK(info->tty);
	return 0;
}

/**
 *	uart_handle_dcd_change - handle a change of carrier detect state
 *	@port: uart_port structure for the open port
 *	@status: new carrier detect status, nonzero if active
 */
static inline void
uart_handle_dcd_change(struct uart_port *port, unsigned int status)
{
	struct uart_info *info = port->info;

	port->icount.dcd++;

#ifdef CONFIG_HARD_PPS
	if ((port->flags & UPF_HARDPPS_CD) && status)
		hardpps();
#endif

	if (info->flags & UIF_CHECK_CD) {
		if (status)
			wake_up_interruptible(&info->open_wait);
		else if (info->tty)
			tty_hangup(info->tty);
	}
}

/**
 *	uart_handle_cts_change - handle a change of clear-to-send state
 *	@port: uart_port structure for the open port
 *	@status: new clear to send status, nonzero if active
 */
static inline void
uart_handle_cts_change(struct uart_port *port, unsigned int status)
{
	struct uart_info *info = port->info;
	struct tty_struct *tty = info->tty;

	port->icount.cts++;

	if (info->flags & UIF_CTS_FLOW) {
		if (tty->hw_stopped) {
			if (status) {
				tty->hw_stopped = 0;
				port->ops->start_tx(port, 0);
				uart_write_wakeup(port);
			}
		} else {
			if (!status) {
				tty->hw_stopped = 1;
				port->ops->stop_tx(port, 0);
			}
		}
	}
}

/*
 *	UART_ENABLE_MS - determine if port should enable modem status irqs
 */
#define UART_ENABLE_MS(port,cflag)	((port)->flags & UPF_HARDPPS_CD || \
					 (cflag) & CRTSCTS || \
					 !((cflag) & CLOCAL))

#endif
