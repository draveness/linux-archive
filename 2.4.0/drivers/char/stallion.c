/*****************************************************************************/

/*
 *	stallion.c  -- stallion multiport serial driver.
 *
 *	Copyright (C) 1996-1999  Stallion Technologies (support@stallion.oz.au).
 *	Copyright (C) 1994-1996  Greg Ungerer.
 *
 *	This code is loosely based on the Linux serial driver, written by
 *	Linus Torvalds, Theodore T'so and others.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*****************************************************************************/

#include <linux/config.h>
#include <linux/module.h>
#include <linux/version.h> /* for linux/stallion.h */
#include <linux/malloc.h>
#include <linux/interrupt.h>
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <linux/cd1400.h>
#include <linux/sc26198.h>
#include <linux/comstats.h>
#include <linux/stallion.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/smp_lock.h>
#include <linux/devfs_fs_kernel.h>

#include <asm/io.h>
#include <asm/uaccess.h>

#ifdef CONFIG_PCI
#include <linux/pci.h>
#endif

/*****************************************************************************/

/*
 *	Define different board types. Use the standard Stallion "assigned"
 *	board numbers. Boards supported in this driver are abbreviated as
 *	EIO = EasyIO and ECH = EasyConnection 8/32.
 */
#define	BRD_EASYIO	20
#define	BRD_ECH		21
#define	BRD_ECHMC	22
#define	BRD_ECHPCI	26
#define	BRD_ECH64PCI	27
#define	BRD_EASYIOPCI	28

/*
 *	Define a configuration structure to hold the board configuration.
 *	Need to set this up in the code (for now) with the boards that are
 *	to be configured into the system. This is what needs to be modified
 *	when adding/removing/modifying boards. Each line entry in the
 *	stl_brdconf[] array is a board. Each line contains io/irq/memory
 *	ranges for that board (as well as what type of board it is).
 *	Some examples:
 *		{ BRD_EASYIO, 0x2a0, 0, 0, 10, 0 },
 *	This line would configure an EasyIO board (4 or 8, no difference),
 *	at io address 2a0 and irq 10.
 *	Another example:
 *		{ BRD_ECH, 0x2a8, 0x280, 0, 12, 0 },
 *	This line will configure an EasyConnection 8/32 board at primary io
 *	address 2a8, secondary io address 280 and irq 12.
 *	Enter as many lines into this array as you want (only the first 4
 *	will actually be used!). Any combination of EasyIO and EasyConnection
 *	boards can be specified. EasyConnection 8/32 boards can share their
 *	secondary io addresses between each other.
 *
 *	NOTE: there is no need to put any entries in this table for PCI
 *	boards. They will be found automatically by the driver - provided
 *	PCI BIOS32 support is compiled into the kernel.
 */

typedef struct {
	int		brdtype;
	int		ioaddr1;
	int		ioaddr2;
	unsigned long	memaddr;
	int		irq;
	int		irqtype;
} stlconf_t;

static stlconf_t	stl_brdconf[] = {
	/*{ BRD_EASYIO, 0x2a0, 0, 0, 10, 0 },*/
};

static int	stl_nrbrds = sizeof(stl_brdconf) / sizeof(stlconf_t);

/*****************************************************************************/

/*
 *	Define some important driver characteristics. Device major numbers
 *	allocated as per Linux Device Registry.
 */
#ifndef	STL_SIOMEMMAJOR
#define	STL_SIOMEMMAJOR		28
#endif
#ifndef	STL_SERIALMAJOR
#define	STL_SERIALMAJOR		24
#endif
#ifndef	STL_CALLOUTMAJOR
#define	STL_CALLOUTMAJOR	25
#endif

#define	STL_DRVTYPSERIAL	1
#define	STL_DRVTYPCALLOUT	2

/*
 *	Set the TX buffer size. Bigger is better, but we don't want
 *	to chew too much memory with buffers!
 */
#define	STL_TXBUFLOW		512
#define	STL_TXBUFSIZE		4096

/*****************************************************************************/

/*
 *	Define our local driver identity first. Set up stuff to deal with
 *	all the local structures required by a serial tty driver.
 */
static char	*stl_drvtitle = "Stallion Multiport Serial Driver";
static char	*stl_drvname = "stallion";
static char	*stl_drvversion = "5.6.0";
static char	*stl_serialname = "ttyE";
static char	*stl_calloutname = "cue";

static struct tty_driver	stl_serial;
static struct tty_driver	stl_callout;
static struct tty_struct	*stl_ttys[STL_MAXDEVS];
static struct termios		*stl_termios[STL_MAXDEVS];
static struct termios		*stl_termioslocked[STL_MAXDEVS];
static int			stl_refcount;

/*
 *	We will need to allocate a temporary write buffer for chars that
 *	come direct from user space. The problem is that a copy from user
 *	space might cause a page fault (typically on a system that is
 *	swapping!). All ports will share one buffer - since if the system
 *	is already swapping a shared buffer won't make things any worse.
 */
static char			*stl_tmpwritebuf;
static DECLARE_MUTEX(stl_tmpwritesem);

/*
 *	Define a local default termios struct. All ports will be created
 *	with this termios initially. Basically all it defines is a raw port
 *	at 9600, 8 data bits, 1 stop bit.
 */
static struct termios		stl_deftermios = {
	0,
	0,
	(B9600 | CS8 | CREAD | HUPCL | CLOCAL),
	0,
	0,
	INIT_C_CC
};

/*
 *	Define global stats structures. Not used often, and can be
 *	re-used for each stats call.
 */
static comstats_t	stl_comstats;
static combrd_t		stl_brdstats;
static stlbrd_t		stl_dummybrd;
static stlport_t	stl_dummyport;

/*
 *	Define global place to put buffer overflow characters.
 */
static char		stl_unwanted[SC26198_RXFIFOSIZE];

/*
 *	Keep track of what interrupts we have requested for us.
 *	We don't need to request an interrupt twice if it is being
 *	shared with another Stallion board.
 */
static int	stl_gotintrs[STL_MAXBRDS];
static int	stl_numintrs;

/*****************************************************************************/

static stlbrd_t		*stl_brds[STL_MAXBRDS];

/*
 *	Per board state flags. Used with the state field of the board struct.
 *	Not really much here!
 */
#define	BRD_FOUND	0x1

/*
 *	Define the port structure istate flags. These set of flags are
 *	modified at interrupt time - so setting and reseting them needs
 *	to be atomic. Use the bit clear/setting routines for this.
 */
#define	ASYI_TXBUSY	1
#define	ASYI_TXLOW	2
#define	ASYI_DCDCHANGE	3
#define	ASYI_TXFLOWED	4

/*
 *	Define an array of board names as printable strings. Handy for
 *	referencing boards when printing trace and stuff.
 */
static char	*stl_brdnames[] = {
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	"EasyIO",
	"EC8/32-AT",
	"EC8/32-MC",
	(char *) NULL,
	(char *) NULL,
	(char *) NULL,
	"EC8/32-PCI",
	"EC8/64-PCI",
	"EasyIO-PCI",
};

/*****************************************************************************/

#ifdef MODULE
/*
 *	Define some string labels for arguments passed from the module
 *	load line. These allow for easy board definitions, and easy
 *	modification of the io, memory and irq resoucres.
 */

static char	*board0[4];
static char	*board1[4];
static char	*board2[4];
static char	*board3[4];

static char	**stl_brdsp[] = {
	(char **) &board0,
	(char **) &board1,
	(char **) &board2,
	(char **) &board3
};

/*
 *	Define a set of common board names, and types. This is used to
 *	parse any module arguments.
 */

typedef struct stlbrdtype {
	char	*name;
	int	type;
} stlbrdtype_t;

static stlbrdtype_t	stl_brdstr[] = {
	{ "easyio", BRD_EASYIO },
	{ "eio", BRD_EASYIO },
	{ "20", BRD_EASYIO },
	{ "ec8/32", BRD_ECH },
	{ "ec8/32-at", BRD_ECH },
	{ "ec8/32-isa", BRD_ECH },
	{ "ech", BRD_ECH },
	{ "echat", BRD_ECH },
	{ "21", BRD_ECH },
	{ "ec8/32-mc", BRD_ECHMC },
	{ "ec8/32-mca", BRD_ECHMC },
	{ "echmc", BRD_ECHMC },
	{ "echmca", BRD_ECHMC },
	{ "22", BRD_ECHMC },
	{ "ec8/32-pc", BRD_ECHPCI },
	{ "ec8/32-pci", BRD_ECHPCI },
	{ "26", BRD_ECHPCI },
	{ "ec8/64-pc", BRD_ECH64PCI },
	{ "ec8/64-pci", BRD_ECH64PCI },
	{ "ech-pci", BRD_ECH64PCI },
	{ "echpci", BRD_ECH64PCI },
	{ "echpc", BRD_ECH64PCI },
	{ "27", BRD_ECH64PCI },
	{ "easyio-pc", BRD_EASYIOPCI },
	{ "easyio-pci", BRD_EASYIOPCI },
	{ "eio-pci", BRD_EASYIOPCI },
	{ "eiopci", BRD_EASYIOPCI },
	{ "28", BRD_EASYIOPCI },
};

/*
 *	Define the module agruments.
 */
MODULE_AUTHOR("Greg Ungerer");
MODULE_DESCRIPTION("Stallion Multiport Serial Driver");

MODULE_PARM(board0, "1-4s");
MODULE_PARM_DESC(board0, "Board 0 config -> name[,ioaddr[,ioaddr2][,irq]]");
MODULE_PARM(board1, "1-4s");
MODULE_PARM_DESC(board1, "Board 1 config -> name[,ioaddr[,ioaddr2][,irq]]");
MODULE_PARM(board2, "1-4s");
MODULE_PARM_DESC(board2, "Board 2 config -> name[,ioaddr[,ioaddr2][,irq]]");
MODULE_PARM(board3, "1-4s");
MODULE_PARM_DESC(board3, "Board 3 config -> name[,ioaddr[,ioaddr2][,irq]]");

#endif

/*****************************************************************************/

/*
 *	Hardware ID bits for the EasyIO and ECH boards. These defines apply
 *	to the directly accessible io ports of these boards (not the uarts -
 *	they are in cd1400.h and sc26198.h).
 */
#define	EIO_8PORTRS	0x04
#define	EIO_4PORTRS	0x05
#define	EIO_8PORTDI	0x00
#define	EIO_8PORTM	0x06
#define	EIO_MK3		0x03
#define	EIO_IDBITMASK	0x07

#define	EIO_BRDMASK	0xf0
#define	ID_BRD4		0x10
#define	ID_BRD8		0x20
#define	ID_BRD16	0x30

#define	EIO_INTRPEND	0x08
#define	EIO_INTEDGE	0x00
#define	EIO_INTLEVEL	0x08
#define	EIO_0WS		0x10

#define	ECH_ID		0xa0
#define	ECH_IDBITMASK	0xe0
#define	ECH_BRDENABLE	0x08
#define	ECH_BRDDISABLE	0x00
#define	ECH_INTENABLE	0x01
#define	ECH_INTDISABLE	0x00
#define	ECH_INTLEVEL	0x02
#define	ECH_INTEDGE	0x00
#define	ECH_INTRPEND	0x01
#define	ECH_BRDRESET	0x01

#define	ECHMC_INTENABLE	0x01
#define	ECHMC_BRDRESET	0x02

#define	ECH_PNLSTATUS	2
#define	ECH_PNL16PORT	0x20
#define	ECH_PNLIDMASK	0x07
#define	ECH_PNLXPID	0x40
#define	ECH_PNLINTRPEND	0x80

#define	ECH_ADDR2MASK	0x1e0

/*
 *	Define the vector mapping bits for the programmable interrupt board
 *	hardware. These bits encode the interrupt for the board to use - it
 *	is software selectable (except the EIO-8M).
 */
static unsigned char	stl_vecmap[] = {
	0xff, 0xff, 0xff, 0x04, 0x06, 0x05, 0xff, 0x07,
	0xff, 0xff, 0x00, 0x02, 0x01, 0xff, 0xff, 0x03
};

/*
 *	Set up enable and disable macros for the ECH boards. They require
 *	the secondary io address space to be activated and deactivated.
 *	This way all ECH boards can share their secondary io region.
 *	If this is an ECH-PCI board then also need to set the page pointer
 *	to point to the correct page.
 */
#define	BRDENABLE(brdnr,pagenr)						\
	if (stl_brds[(brdnr)]->brdtype == BRD_ECH)			\
		outb((stl_brds[(brdnr)]->ioctrlval | ECH_BRDENABLE),	\
			stl_brds[(brdnr)]->ioctrl);			\
	else if (stl_brds[(brdnr)]->brdtype == BRD_ECHPCI)		\
		outb((pagenr), stl_brds[(brdnr)]->ioctrl);

#define	BRDDISABLE(brdnr)						\
	if (stl_brds[(brdnr)]->brdtype == BRD_ECH)			\
		outb((stl_brds[(brdnr)]->ioctrlval | ECH_BRDDISABLE),	\
			stl_brds[(brdnr)]->ioctrl);

#define	STL_CD1400MAXBAUD	230400
#define	STL_SC26198MAXBAUD	460800

#define	STL_BAUDBASE		115200
#define	STL_CLOSEDELAY		(5 * HZ / 10)

/*****************************************************************************/

#ifdef CONFIG_PCI

/*
 *	Define the Stallion PCI vendor and device IDs.
 */
#ifndef	PCI_VENDOR_ID_STALLION
#define	PCI_VENDOR_ID_STALLION		0x124d
#endif
#ifndef PCI_DEVICE_ID_ECHPCI832
#define	PCI_DEVICE_ID_ECHPCI832		0x0000
#endif
#ifndef PCI_DEVICE_ID_ECHPCI864
#define	PCI_DEVICE_ID_ECHPCI864		0x0002
#endif
#ifndef PCI_DEVICE_ID_EIOPCI
#define	PCI_DEVICE_ID_EIOPCI		0x0003
#endif

/*
 *	Define structure to hold all Stallion PCI boards.
 */
typedef struct stlpcibrd {
	unsigned short		vendid;
	unsigned short		devid;
	int			brdtype;
} stlpcibrd_t;

static stlpcibrd_t	stl_pcibrds[] = {
	{ PCI_VENDOR_ID_STALLION, PCI_DEVICE_ID_ECHPCI864, BRD_ECH64PCI },
	{ PCI_VENDOR_ID_STALLION, PCI_DEVICE_ID_EIOPCI, BRD_EASYIOPCI },
	{ PCI_VENDOR_ID_STALLION, PCI_DEVICE_ID_ECHPCI832, BRD_ECHPCI },
	{ PCI_VENDOR_ID_NS, PCI_DEVICE_ID_NS_87410, BRD_ECHPCI },
};

static int	stl_nrpcibrds = sizeof(stl_pcibrds) / sizeof(stlpcibrd_t);

#endif

/*****************************************************************************/

/*
 *	Define macros to extract a brd/port number from a minor number.
 */
#define	MINOR2BRD(min)		(((min) & 0xc0) >> 6)
#define	MINOR2PORT(min)		((min) & 0x3f)

/*
 *	Define a baud rate table that converts termios baud rate selector
 *	into the actual baud rate value. All baud rate calculations are
 *	based on the actual baud rate required.
 */
static unsigned int	stl_baudrates[] = {
	0, 50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400, 4800,
	9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600
};

/*
 *	Define some handy local macros...
 */
#undef	MIN
#define	MIN(a,b)	(((a) <= (b)) ? (a) : (b))

#undef	TOLOWER
#define	TOLOWER(x)	((((x) >= 'A') && ((x) <= 'Z')) ? ((x) + 0x20) : (x))

/*****************************************************************************/

/*
 *	Declare all those functions in this driver!
 */

#ifdef MODULE
int		init_module(void);
void		cleanup_module(void);
static void	stl_argbrds(void);
static int	stl_parsebrd(stlconf_t *confp, char **argp);

static unsigned long stl_atol(char *str);
#endif

int		stl_init(void);
static int	stl_open(struct tty_struct *tty, struct file *filp);
static void	stl_close(struct tty_struct *tty, struct file *filp);
static int	stl_write(struct tty_struct *tty, int from_user, const unsigned char *buf, int count);
static void	stl_putchar(struct tty_struct *tty, unsigned char ch);
static void	stl_flushchars(struct tty_struct *tty);
static int	stl_writeroom(struct tty_struct *tty);
static int	stl_charsinbuffer(struct tty_struct *tty);
static int	stl_ioctl(struct tty_struct *tty, struct file *file, unsigned int cmd, unsigned long arg);
static void	stl_settermios(struct tty_struct *tty, struct termios *old);
static void	stl_throttle(struct tty_struct *tty);
static void	stl_unthrottle(struct tty_struct *tty);
static void	stl_stop(struct tty_struct *tty);
static void	stl_start(struct tty_struct *tty);
static void	stl_flushbuffer(struct tty_struct *tty);
static void	stl_breakctl(struct tty_struct *tty, int state);
static void	stl_waituntilsent(struct tty_struct *tty, int timeout);
static void	stl_sendxchar(struct tty_struct *tty, char ch);
static void	stl_hangup(struct tty_struct *tty);
static int	stl_memioctl(struct inode *ip, struct file *fp, unsigned int cmd, unsigned long arg);
static int	stl_portinfo(stlport_t *portp, int portnr, char *pos);
static int	stl_readproc(char *page, char **start, off_t off, int count, int *eof, void *data);

static int	stl_brdinit(stlbrd_t *brdp);
static int	stl_initports(stlbrd_t *brdp, stlpanel_t *panelp);
static int	stl_mapirq(int irq, char *name);
static void	stl_getserial(stlport_t *portp, struct serial_struct *sp);
static int	stl_setserial(stlport_t *portp, struct serial_struct *sp);
static int	stl_getbrdstats(combrd_t *bp);
static int	stl_getportstats(stlport_t *portp, comstats_t *cp);
static int	stl_clrportstats(stlport_t *portp, comstats_t *cp);
static int	stl_getportstruct(unsigned long arg);
static int	stl_getbrdstruct(unsigned long arg);
static int	stl_waitcarrier(stlport_t *portp, struct file *filp);
static void	stl_delay(int len);
static void	stl_intr(int irq, void *dev_id, struct pt_regs *regs);
static void	stl_eiointr(stlbrd_t *brdp);
static void	stl_echatintr(stlbrd_t *brdp);
static void	stl_echmcaintr(stlbrd_t *brdp);
static void	stl_echpciintr(stlbrd_t *brdp);
static void	stl_echpci64intr(stlbrd_t *brdp);
static void	stl_offintr(void *private);
static void	*stl_memalloc(int len);
static stlbrd_t *stl_allocbrd(void);
static stlport_t *stl_getport(int brdnr, int panelnr, int portnr);

static inline int	stl_initbrds(void);
static inline int	stl_initeio(stlbrd_t *brdp);
static inline int	stl_initech(stlbrd_t *brdp);
static inline int	stl_getbrdnr(void);

#ifdef	CONFIG_PCI
static inline int	stl_findpcibrds(void);
static inline int	stl_initpcibrd(int brdtype, struct pci_dev *devp);
#endif

/*
 *	CD1400 uart specific handling functions.
 */
static void	stl_cd1400setreg(stlport_t *portp, int regnr, int value);
static int	stl_cd1400getreg(stlport_t *portp, int regnr);
static int	stl_cd1400updatereg(stlport_t *portp, int regnr, int value);
static int	stl_cd1400panelinit(stlbrd_t *brdp, stlpanel_t *panelp);
static void	stl_cd1400portinit(stlbrd_t *brdp, stlpanel_t *panelp, stlport_t *portp);
static void	stl_cd1400setport(stlport_t *portp, struct termios *tiosp);
static int	stl_cd1400getsignals(stlport_t *portp);
static void	stl_cd1400setsignals(stlport_t *portp, int dtr, int rts);
static void	stl_cd1400ccrwait(stlport_t *portp);
static void	stl_cd1400enablerxtx(stlport_t *portp, int rx, int tx);
static void	stl_cd1400startrxtx(stlport_t *portp, int rx, int tx);
static void	stl_cd1400disableintrs(stlport_t *portp);
static void	stl_cd1400sendbreak(stlport_t *portp, int len);
static void	stl_cd1400flowctrl(stlport_t *portp, int state);
static void	stl_cd1400sendflow(stlport_t *portp, int state);
static void	stl_cd1400flush(stlport_t *portp);
static int	stl_cd1400datastate(stlport_t *portp);
static void	stl_cd1400eiointr(stlpanel_t *panelp, unsigned int iobase);
static void	stl_cd1400echintr(stlpanel_t *panelp, unsigned int iobase);
static void	stl_cd1400txisr(stlpanel_t *panelp, int ioaddr);
static void	stl_cd1400rxisr(stlpanel_t *panelp, int ioaddr);
static void	stl_cd1400mdmisr(stlpanel_t *panelp, int ioaddr);

static inline int	stl_cd1400breakisr(stlport_t *portp, int ioaddr);

/*
 *	SC26198 uart specific handling functions.
 */
static void	stl_sc26198setreg(stlport_t *portp, int regnr, int value);
static int	stl_sc26198getreg(stlport_t *portp, int regnr);
static int	stl_sc26198updatereg(stlport_t *portp, int regnr, int value);
static int	stl_sc26198getglobreg(stlport_t *portp, int regnr);
static int	stl_sc26198panelinit(stlbrd_t *brdp, stlpanel_t *panelp);
static void	stl_sc26198portinit(stlbrd_t *brdp, stlpanel_t *panelp, stlport_t *portp);
static void	stl_sc26198setport(stlport_t *portp, struct termios *tiosp);
static int	stl_sc26198getsignals(stlport_t *portp);
static void	stl_sc26198setsignals(stlport_t *portp, int dtr, int rts);
static void	stl_sc26198enablerxtx(stlport_t *portp, int rx, int tx);
static void	stl_sc26198startrxtx(stlport_t *portp, int rx, int tx);
static void	stl_sc26198disableintrs(stlport_t *portp);
static void	stl_sc26198sendbreak(stlport_t *portp, int len);
static void	stl_sc26198flowctrl(stlport_t *portp, int state);
static void	stl_sc26198sendflow(stlport_t *portp, int state);
static void	stl_sc26198flush(stlport_t *portp);
static int	stl_sc26198datastate(stlport_t *portp);
static void	stl_sc26198wait(stlport_t *portp);
static void	stl_sc26198txunflow(stlport_t *portp, struct tty_struct *tty);
static void	stl_sc26198intr(stlpanel_t *panelp, unsigned int iobase);
static void	stl_sc26198txisr(stlport_t *port);
static void	stl_sc26198rxisr(stlport_t *port, unsigned int iack);
static void	stl_sc26198rxbadch(stlport_t *portp, unsigned char status, char ch);
static void	stl_sc26198rxbadchars(stlport_t *portp);
static void	stl_sc26198otherisr(stlport_t *port, unsigned int iack);

/*****************************************************************************/

/*
 *	Generic UART support structure.
 */
typedef struct uart {
	int	(*panelinit)(stlbrd_t *brdp, stlpanel_t *panelp);
	void	(*portinit)(stlbrd_t *brdp, stlpanel_t *panelp, stlport_t *portp);
	void	(*setport)(stlport_t *portp, struct termios *tiosp);
	int	(*getsignals)(stlport_t *portp);
	void	(*setsignals)(stlport_t *portp, int dtr, int rts);
	void	(*enablerxtx)(stlport_t *portp, int rx, int tx);
	void	(*startrxtx)(stlport_t *portp, int rx, int tx);
	void	(*disableintrs)(stlport_t *portp);
	void	(*sendbreak)(stlport_t *portp, int len);
	void	(*flowctrl)(stlport_t *portp, int state);
	void	(*sendflow)(stlport_t *portp, int state);
	void	(*flush)(stlport_t *portp);
	int	(*datastate)(stlport_t *portp);
	void	(*intr)(stlpanel_t *panelp, unsigned int iobase);
} uart_t;

/*
 *	Define some macros to make calling these functions nice and clean.
 */
#define	stl_panelinit		(* ((uart_t *) panelp->uartp)->panelinit)
#define	stl_portinit		(* ((uart_t *) portp->uartp)->portinit)
#define	stl_setport		(* ((uart_t *) portp->uartp)->setport)
#define	stl_getsignals		(* ((uart_t *) portp->uartp)->getsignals)
#define	stl_setsignals		(* ((uart_t *) portp->uartp)->setsignals)
#define	stl_enablerxtx		(* ((uart_t *) portp->uartp)->enablerxtx)
#define	stl_startrxtx		(* ((uart_t *) portp->uartp)->startrxtx)
#define	stl_disableintrs	(* ((uart_t *) portp->uartp)->disableintrs)
#define	stl_sendbreak		(* ((uart_t *) portp->uartp)->sendbreak)
#define	stl_flowctrl		(* ((uart_t *) portp->uartp)->flowctrl)
#define	stl_sendflow		(* ((uart_t *) portp->uartp)->sendflow)
#define	stl_flush		(* ((uart_t *) portp->uartp)->flush)
#define	stl_datastate		(* ((uart_t *) portp->uartp)->datastate)

/*****************************************************************************/

/*
 *	CD1400 UART specific data initialization.
 */
static uart_t stl_cd1400uart = {
	stl_cd1400panelinit,
	stl_cd1400portinit,
	stl_cd1400setport,
	stl_cd1400getsignals,
	stl_cd1400setsignals,
	stl_cd1400enablerxtx,
	stl_cd1400startrxtx,
	stl_cd1400disableintrs,
	stl_cd1400sendbreak,
	stl_cd1400flowctrl,
	stl_cd1400sendflow,
	stl_cd1400flush,
	stl_cd1400datastate,
	stl_cd1400eiointr
};

/*
 *	Define the offsets within the register bank of a cd1400 based panel.
 *	These io address offsets are common to the EasyIO board as well.
 */
#define	EREG_ADDR	0
#define	EREG_DATA	4
#define	EREG_RXACK	5
#define	EREG_TXACK	6
#define	EREG_MDACK	7

#define	EREG_BANKSIZE	8

#define	CD1400_CLK	25000000
#define	CD1400_CLK8M	20000000

/*
 *	Define the cd1400 baud rate clocks. These are used when calculating
 *	what clock and divisor to use for the required baud rate. Also
 *	define the maximum baud rate allowed, and the default base baud.
 */
static int	stl_cd1400clkdivs[] = {
	CD1400_CLK0, CD1400_CLK1, CD1400_CLK2, CD1400_CLK3, CD1400_CLK4
};

/*****************************************************************************/

/*
 *	SC26198 UART specific data initization.
 */
static uart_t stl_sc26198uart = {
	stl_sc26198panelinit,
	stl_sc26198portinit,
	stl_sc26198setport,
	stl_sc26198getsignals,
	stl_sc26198setsignals,
	stl_sc26198enablerxtx,
	stl_sc26198startrxtx,
	stl_sc26198disableintrs,
	stl_sc26198sendbreak,
	stl_sc26198flowctrl,
	stl_sc26198sendflow,
	stl_sc26198flush,
	stl_sc26198datastate,
	stl_sc26198intr
};

/*
 *	Define the offsets within the register bank of a sc26198 based panel.
 */
#define	XP_DATA		0
#define	XP_ADDR		1
#define	XP_MODID	2
#define	XP_STATUS	2
#define	XP_IACK		3

#define	XP_BANKSIZE	4

/*
 *	Define the sc26198 baud rate table. Offsets within the table
 *	represent the actual baud rate selector of sc26198 registers.
 */
static unsigned int	sc26198_baudtable[] = {
	50, 75, 150, 200, 300, 450, 600, 900, 1200, 1800, 2400, 3600,
	4800, 7200, 9600, 14400, 19200, 28800, 38400, 57600, 115200,
	230400, 460800, 921600
};

#define	SC26198_NRBAUDS		(sizeof(sc26198_baudtable) / sizeof(unsigned int))

/*****************************************************************************/

/*
 *	Define the driver info for a user level control device. Used mainly
 *	to get at port stats - only not using the port device itself.
 */
static struct file_operations	stl_fsiomem = {
	owner:		THIS_MODULE,
	ioctl:		stl_memioctl,
};

/*****************************************************************************/

static devfs_handle_t devfs_handle;

#ifdef MODULE

/*
 *	Loadable module initialization stuff.
 */

int init_module()
{
	unsigned long	flags;

#if DEBUG
	printk("init_module()\n");
#endif

	save_flags(flags);
	cli();
	stl_init();
	restore_flags(flags);

	return(0);
}

/*****************************************************************************/

void cleanup_module()
{
	stlbrd_t	*brdp;
	stlpanel_t	*panelp;
	stlport_t	*portp;
	unsigned long	flags;
	int		i, j, k;

#if DEBUG
	printk("cleanup_module()\n");
#endif

	printk(KERN_INFO "Unloading %s: version %s\n", stl_drvtitle,
		stl_drvversion);

	save_flags(flags);
	cli();

/*
 *	Free up all allocated resources used by the ports. This includes
 *	memory and interrupts. As part of this process we will also do
 *	a hangup on every open port - to try to flush out any processes
 *	hanging onto ports.
 */
	i = tty_unregister_driver(&stl_serial);
	j = tty_unregister_driver(&stl_callout);
	if (i || j) {
		printk("STALLION: failed to un-register tty driver, "
			"errno=%d,%d\n", -i, -j);
		restore_flags(flags);
		return;
	}
	devfs_unregister (devfs_handle);
	if ((i = devfs_unregister_chrdev(STL_SIOMEMMAJOR, "staliomem")))
		printk("STALLION: failed to un-register serial memory device, "
			"errno=%d\n", -i);

	if (stl_tmpwritebuf != (char *) NULL)
		kfree(stl_tmpwritebuf);

	for (i = 0; (i < stl_nrbrds); i++) {
		if ((brdp = stl_brds[i]) == (stlbrd_t *) NULL)
			continue;
		for (j = 0; (j < STL_MAXPANELS); j++) {
			panelp = brdp->panels[j];
			if (panelp == (stlpanel_t *) NULL)
				continue;
			for (k = 0; (k < STL_PORTSPERPANEL); k++) {
				portp = panelp->ports[k];
				if (portp == (stlport_t *) NULL)
					continue;
				if (portp->tty != (struct tty_struct *) NULL)
					stl_hangup(portp->tty);
				if (portp->tx.buf != (char *) NULL)
					kfree(portp->tx.buf);
				kfree(portp);
			}
			kfree(panelp);
		}

		release_region(brdp->ioaddr1, brdp->iosize1);
		if (brdp->iosize2 > 0)
			release_region(brdp->ioaddr2, brdp->iosize2);

		kfree(brdp);
		stl_brds[i] = (stlbrd_t *) NULL;
	}

	for (i = 0; (i < stl_numintrs); i++)
		free_irq(stl_gotintrs[i], NULL);

	restore_flags(flags);
}

/*****************************************************************************/

/*
 *	Check for any arguments passed in on the module load command line.
 */

static void stl_argbrds()
{
	stlconf_t	conf;
	stlbrd_t	*brdp;
	int		nrargs, i;

#if DEBUG
	printk("stl_argbrds()\n");
#endif

	nrargs = sizeof(stl_brdsp) / sizeof(char **);

	for (i = stl_nrbrds; (i < nrargs); i++) {
		memset(&conf, 0, sizeof(conf));
		if (stl_parsebrd(&conf, stl_brdsp[i]) == 0)
			continue;
		if ((brdp = stl_allocbrd()) == (stlbrd_t *) NULL)
			continue;
		stl_nrbrds = i + 1;
		brdp->brdnr = i;
		brdp->brdtype = conf.brdtype;
		brdp->ioaddr1 = conf.ioaddr1;
		brdp->ioaddr2 = conf.ioaddr2;
		brdp->irq = conf.irq;
		brdp->irqtype = conf.irqtype;
		stl_brdinit(brdp);
	}
}

/*****************************************************************************/

/*
 *	Convert an ascii string number into an unsigned long.
 */

static unsigned long stl_atol(char *str)
{
	unsigned long	val;
	int		base, c;
	char		*sp;

	val = 0;
	sp = str;
	if ((*sp == '0') && (*(sp+1) == 'x')) {
		base = 16;
		sp += 2;
	} else if (*sp == '0') {
		base = 8;
		sp++;
	} else {
		base = 10;
	}

	for (; (*sp != 0); sp++) {
		c = (*sp > '9') ? (TOLOWER(*sp) - 'a' + 10) : (*sp - '0');
		if ((c < 0) || (c >= base)) {
			printk("STALLION: invalid argument %s\n", str);
			val = 0;
			break;
		}
		val = (val * base) + c;
	}
	return(val);
}

/*****************************************************************************/

/*
 *	Parse the supplied argument string, into the board conf struct.
 */

static int stl_parsebrd(stlconf_t *confp, char **argp)
{
	char	*sp;
	int	nrbrdnames, i;

#if DEBUG
	printk("stl_parsebrd(confp=%x,argp=%x)\n", (int) confp, (int) argp);
#endif

	if ((argp[0] == (char *) NULL) || (*argp[0] == 0))
		return(0);

	for (sp = argp[0], i = 0; ((*sp != 0) && (i < 25)); sp++, i++)
		*sp = TOLOWER(*sp);

	nrbrdnames = sizeof(stl_brdstr) / sizeof(stlbrdtype_t);
	for (i = 0; (i < nrbrdnames); i++) {
		if (strcmp(stl_brdstr[i].name, argp[0]) == 0)
			break;
	}
	if (i >= nrbrdnames) {
		printk("STALLION: unknown board name, %s?\n", argp[0]);
		return(0);
	}

	confp->brdtype = stl_brdstr[i].type;

	i = 1;
	if ((argp[i] != (char *) NULL) && (*argp[i] != 0))
		confp->ioaddr1 = stl_atol(argp[i]);
	i++;
	if (confp->brdtype == BRD_ECH) {
		if ((argp[i] != (char *) NULL) && (*argp[i] != 0))
			confp->ioaddr2 = stl_atol(argp[i]);
		i++;
	}
	if ((argp[i] != (char *) NULL) && (*argp[i] != 0))
		confp->irq = stl_atol(argp[i]);
	return(1);
}

#endif

/*****************************************************************************/

/*
 *	Local driver kernel memory allocation routine.
 */

static void *stl_memalloc(int len)
{
	return((void *) kmalloc(len, GFP_KERNEL));
}

/*****************************************************************************/

/*
 *	Allocate a new board structure. Fill out the basic info in it.
 */

static stlbrd_t *stl_allocbrd()
{
	stlbrd_t	*brdp;

	brdp = (stlbrd_t *) stl_memalloc(sizeof(stlbrd_t));
	if (brdp == (stlbrd_t *) NULL) {
		printk("STALLION: failed to allocate memory (size=%d)\n",
			sizeof(stlbrd_t));
		return((stlbrd_t *) NULL);
	}

	memset(brdp, 0, sizeof(stlbrd_t));
	brdp->magic = STL_BOARDMAGIC;
	return(brdp);
}

/*****************************************************************************/

static int stl_open(struct tty_struct *tty, struct file *filp)
{
	stlport_t	*portp;
	stlbrd_t	*brdp;
	unsigned int	minordev;
	int		brdnr, panelnr, portnr, rc;

#if DEBUG
	printk("stl_open(tty=%x,filp=%x): device=%x\n", (int) tty,
		(int) filp, tty->device);
#endif

	minordev = MINOR(tty->device);
	brdnr = MINOR2BRD(minordev);
	if (brdnr >= stl_nrbrds)
		return(-ENODEV);
	brdp = stl_brds[brdnr];
	if (brdp == (stlbrd_t *) NULL)
		return(-ENODEV);
	minordev = MINOR2PORT(minordev);
	for (portnr = -1, panelnr = 0; (panelnr < STL_MAXPANELS); panelnr++) {
		if (brdp->panels[panelnr] == (stlpanel_t *) NULL)
			break;
		if (minordev < brdp->panels[panelnr]->nrports) {
			portnr = minordev;
			break;
		}
		minordev -= brdp->panels[panelnr]->nrports;
	}
	if (portnr < 0)
		return(-ENODEV);

	portp = brdp->panels[panelnr]->ports[portnr];
	if (portp == (stlport_t *) NULL)
		return(-ENODEV);

	MOD_INC_USE_COUNT;

/*
 *	On the first open of the device setup the port hardware, and
 *	initialize the per port data structure.
 */
	portp->tty = tty;
	tty->driver_data = portp;
	portp->refcount++;

	if ((portp->flags & ASYNC_INITIALIZED) == 0) {
		if (portp->tx.buf == (char *) NULL) {
			portp->tx.buf = (char *) stl_memalloc(STL_TXBUFSIZE);
			if (portp->tx.buf == (char *) NULL)
				return(-ENOMEM);
			portp->tx.head = portp->tx.buf;
			portp->tx.tail = portp->tx.buf;
		}
		stl_setport(portp, tty->termios);
		portp->sigs = stl_getsignals(portp);
		stl_setsignals(portp, 1, 1);
		stl_enablerxtx(portp, 1, 1);
		stl_startrxtx(portp, 1, 0);
		clear_bit(TTY_IO_ERROR, &tty->flags);
		portp->flags |= ASYNC_INITIALIZED;
	}

/*
 *	Check if this port is in the middle of closing. If so then wait
 *	until it is closed then return error status, based on flag settings.
 *	The sleep here does not need interrupt protection since the wakeup
 *	for it is done with the same context.
 */
	if (portp->flags & ASYNC_CLOSING) {
		interruptible_sleep_on(&portp->close_wait);
		if (portp->flags & ASYNC_HUP_NOTIFY)
			return(-EAGAIN);
		return(-ERESTARTSYS);
	}

/*
 *	Based on type of open being done check if it can overlap with any
 *	previous opens still in effect. If we are a normal serial device
 *	then also we might have to wait for carrier.
 */
	if (tty->driver.subtype == STL_DRVTYPCALLOUT) {
		if (portp->flags & ASYNC_NORMAL_ACTIVE)
			return(-EBUSY);
		if (portp->flags & ASYNC_CALLOUT_ACTIVE) {
			if ((portp->flags & ASYNC_SESSION_LOCKOUT) &&
			    (portp->session != current->session))
				return(-EBUSY);
			if ((portp->flags & ASYNC_PGRP_LOCKOUT) &&
			    (portp->pgrp != current->pgrp))
				return(-EBUSY);
		}
		portp->flags |= ASYNC_CALLOUT_ACTIVE;
	} else {
		if (filp->f_flags & O_NONBLOCK) {
			if (portp->flags & ASYNC_CALLOUT_ACTIVE)
				return(-EBUSY);
		} else {
			if ((rc = stl_waitcarrier(portp, filp)) != 0)
				return(rc);
		}
		portp->flags |= ASYNC_NORMAL_ACTIVE;
	}

	if ((portp->refcount == 1) && (portp->flags & ASYNC_SPLIT_TERMIOS)) {
		if (tty->driver.subtype == STL_DRVTYPSERIAL)
			*tty->termios = portp->normaltermios;
		else
			*tty->termios = portp->callouttermios;
		stl_setport(portp, tty->termios);
	}

	portp->session = current->session;
	portp->pgrp = current->pgrp;
	return(0);
}

/*****************************************************************************/

/*
 *	Possibly need to wait for carrier (DCD signal) to come high. Say
 *	maybe because if we are clocal then we don't need to wait...
 */

static int stl_waitcarrier(stlport_t *portp, struct file *filp)
{
	unsigned long	flags;
	int		rc, doclocal;

#if DEBUG
	printk("stl_waitcarrier(portp=%x,filp=%x)\n", (int) portp, (int) filp);
#endif

	rc = 0;
	doclocal = 0;

	if (portp->flags & ASYNC_CALLOUT_ACTIVE) {
		if (portp->normaltermios.c_cflag & CLOCAL)
			doclocal++;
	} else {
		if (portp->tty->termios->c_cflag & CLOCAL)
			doclocal++;
	}

	save_flags(flags);
	cli();
	portp->openwaitcnt++;
	if (! tty_hung_up_p(filp))
		portp->refcount--;

	for (;;) {
		if ((portp->flags & ASYNC_CALLOUT_ACTIVE) == 0)
			stl_setsignals(portp, 1, 1);
		if (tty_hung_up_p(filp) ||
		    ((portp->flags & ASYNC_INITIALIZED) == 0)) {
			if (portp->flags & ASYNC_HUP_NOTIFY)
				rc = -EBUSY;
			else
				rc = -ERESTARTSYS;
			break;
		}
		if (((portp->flags & ASYNC_CALLOUT_ACTIVE) == 0) &&
		    ((portp->flags & ASYNC_CLOSING) == 0) &&
		    (doclocal || (portp->sigs & TIOCM_CD))) {
			break;
		}
		if (signal_pending(current)) {
			rc = -ERESTARTSYS;
			break;
		}
		interruptible_sleep_on(&portp->open_wait);
	}

	if (! tty_hung_up_p(filp))
		portp->refcount++;
	portp->openwaitcnt--;
	restore_flags(flags);

	return(rc);
}

/*****************************************************************************/

static void stl_close(struct tty_struct *tty, struct file *filp)
{
	stlport_t	*portp;
	unsigned long	flags;

#if DEBUG
	printk("stl_close(tty=%x,filp=%x)\n", (int) tty, (int) filp);
#endif

	portp = tty->driver_data;
	if (portp == (stlport_t *) NULL)
		return;

	save_flags(flags);
	cli();
	if (tty_hung_up_p(filp)) {
		MOD_DEC_USE_COUNT;
		restore_flags(flags);
		return;
	}
	if ((tty->count == 1) && (portp->refcount != 1))
		portp->refcount = 1;
	if (portp->refcount-- > 1) {
		MOD_DEC_USE_COUNT;
		restore_flags(flags);
		return;
	}

	portp->refcount = 0;
	portp->flags |= ASYNC_CLOSING;

	if (portp->flags & ASYNC_NORMAL_ACTIVE)
		portp->normaltermios = *tty->termios;
	if (portp->flags & ASYNC_CALLOUT_ACTIVE)
		portp->callouttermios = *tty->termios;

/*
 *	May want to wait for any data to drain before closing. The BUSY
 *	flag keeps track of whether we are still sending or not - it is
 *	very accurate for the cd1400, not quite so for the sc26198.
 *	(The sc26198 has no "end-of-data" interrupt only empty FIFO)
 */
	tty->closing = 1;
	if (portp->closing_wait != ASYNC_CLOSING_WAIT_NONE)
		tty_wait_until_sent(tty, portp->closing_wait);
	stl_waituntilsent(tty, (HZ / 2));

	portp->flags &= ~ASYNC_INITIALIZED;
	stl_disableintrs(portp);
	if (tty->termios->c_cflag & HUPCL)
		stl_setsignals(portp, 0, 0);
	stl_enablerxtx(portp, 0, 0);
	stl_flushbuffer(tty);
	portp->istate = 0;
	if (portp->tx.buf != (char *) NULL) {
		kfree(portp->tx.buf);
		portp->tx.buf = (char *) NULL;
		portp->tx.head = (char *) NULL;
		portp->tx.tail = (char *) NULL;
	}
	set_bit(TTY_IO_ERROR, &tty->flags);
	if (tty->ldisc.flush_buffer)
		(tty->ldisc.flush_buffer)(tty);

	tty->closing = 0;
	portp->tty = (struct tty_struct *) NULL;

	if (portp->openwaitcnt) {
		if (portp->close_delay)
			stl_delay(portp->close_delay);
		wake_up_interruptible(&portp->open_wait);
	}

	portp->flags &= ~(ASYNC_CALLOUT_ACTIVE | ASYNC_NORMAL_ACTIVE |
		ASYNC_CLOSING);
	wake_up_interruptible(&portp->close_wait);
	MOD_DEC_USE_COUNT;
	restore_flags(flags);
}

/*****************************************************************************/

/*
 *	Wait for a specified delay period, this is not a busy-loop. It will
 *	give up the processor while waiting. Unfortunately this has some
 *	rather intimate knowledge of the process management stuff.
 */

static void stl_delay(int len)
{
#if DEBUG
	printk("stl_delay(len=%d)\n", len);
#endif
	if (len > 0) {
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(len);
		current->state = TASK_RUNNING;
	}
}

/*****************************************************************************/

/*
 *	Write routine. Take data and stuff it in to the TX ring queue.
 *	If transmit interrupts are not running then start them.
 */

static int stl_write(struct tty_struct *tty, int from_user, const unsigned char *buf, int count)
{
	stlport_t	*portp;
	unsigned int	len, stlen;
	unsigned char	*chbuf;
	char		*head, *tail;

#if DEBUG
	printk("stl_write(tty=%x,from_user=%d,buf=%x,count=%d)\n",
		(int) tty, from_user, (int) buf, count);
#endif

	if ((tty == (struct tty_struct *) NULL) ||
	    (stl_tmpwritebuf == (char *) NULL))
		return(0);
	portp = tty->driver_data;
	if (portp == (stlport_t *) NULL)
		return(0);
	if (portp->tx.buf == (char *) NULL)
		return(0);

/*
 *	If copying direct from user space we must cater for page faults,
 *	causing us to "sleep" here for a while. To handle this copy in all
 *	the data we need now, into a local buffer. Then when we got it all
 *	copy it into the TX buffer.
 */
	chbuf = (unsigned char *) buf;
	if (from_user) {
		head = portp->tx.head;
		tail = portp->tx.tail;
		len = (head >= tail) ? (STL_TXBUFSIZE - (head - tail) - 1) :
			(tail - head - 1);
		count = MIN(len, count);
		
		down(&stl_tmpwritesem);
		copy_from_user(stl_tmpwritebuf, chbuf, count);
		chbuf = &stl_tmpwritebuf[0];
	}

	head = portp->tx.head;
	tail = portp->tx.tail;
	if (head >= tail) {
		len = STL_TXBUFSIZE - (head - tail) - 1;
		stlen = STL_TXBUFSIZE - (head - portp->tx.buf);
	} else {
		len = tail - head - 1;
		stlen = len;
	}

	len = MIN(len, count);
	count = 0;
	while (len > 0) {
		stlen = MIN(len, stlen);
		memcpy(head, chbuf, stlen);
		len -= stlen;
		chbuf += stlen;
		count += stlen;
		head += stlen;
		if (head >= (portp->tx.buf + STL_TXBUFSIZE)) {
			head = portp->tx.buf;
			stlen = tail - head;
		}
	}
	portp->tx.head = head;

	clear_bit(ASYI_TXLOW, &portp->istate);
	stl_startrxtx(portp, -1, 1);

	if (from_user)
		up(&stl_tmpwritesem);

	return(count);
}

/*****************************************************************************/

static void stl_putchar(struct tty_struct *tty, unsigned char ch)
{
	stlport_t	*portp;
	unsigned int	len;
	char		*head, *tail;

#if DEBUG
	printk("stl_putchar(tty=%x,ch=%x)\n", (int) tty, (int) ch);
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	portp = tty->driver_data;
	if (portp == (stlport_t *) NULL)
		return;
	if (portp->tx.buf == (char *) NULL)
		return;

	head = portp->tx.head;
	tail = portp->tx.tail;

	len = (head >= tail) ? (STL_TXBUFSIZE - (head - tail)) : (tail - head);
	len--;

	if (len > 0) {
		*head++ = ch;
		if (head >= (portp->tx.buf + STL_TXBUFSIZE))
			head = portp->tx.buf;
	}	
	portp->tx.head = head;
}

/*****************************************************************************/

/*
 *	If there are any characters in the buffer then make sure that TX
 *	interrupts are on and get'em out. Normally used after the putchar
 *	routine has been called.
 */

static void stl_flushchars(struct tty_struct *tty)
{
	stlport_t	*portp;

#if DEBUG
	printk("stl_flushchars(tty=%x)\n", (int) tty);
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	portp = tty->driver_data;
	if (portp == (stlport_t *) NULL)
		return;
	if (portp->tx.buf == (char *) NULL)
		return;

#if 0
	if (tty->stopped || tty->hw_stopped ||
	    (portp->tx.head == portp->tx.tail))
		return;
#endif
	stl_startrxtx(portp, -1, 1);
}

/*****************************************************************************/

static int stl_writeroom(struct tty_struct *tty)
{
	stlport_t	*portp;
	char		*head, *tail;

#if DEBUG
	printk("stl_writeroom(tty=%x)\n", (int) tty);
#endif

	if (tty == (struct tty_struct *) NULL)
		return(0);
	portp = tty->driver_data;
	if (portp == (stlport_t *) NULL)
		return(0);
	if (portp->tx.buf == (char *) NULL)
		return(0);

	head = portp->tx.head;
	tail = portp->tx.tail;
	return((head >= tail) ? (STL_TXBUFSIZE - (head - tail) - 1) : (tail - head - 1));
}

/*****************************************************************************/

/*
 *	Return number of chars in the TX buffer. Normally we would just
 *	calculate the number of chars in the buffer and return that, but if
 *	the buffer is empty and TX interrupts are still on then we return
 *	that the buffer still has 1 char in it. This way whoever called us
 *	will not think that ALL chars have drained - since the UART still
 *	must have some chars in it (we are busy after all).
 */

static int stl_charsinbuffer(struct tty_struct *tty)
{
	stlport_t	*portp;
	unsigned int	size;
	char		*head, *tail;

#if DEBUG
	printk("stl_charsinbuffer(tty=%x)\n", (int) tty);
#endif

	if (tty == (struct tty_struct *) NULL)
		return(0);
	portp = tty->driver_data;
	if (portp == (stlport_t *) NULL)
		return(0);
	if (portp->tx.buf == (char *) NULL)
		return(0);

	head = portp->tx.head;
	tail = portp->tx.tail;
	size = (head >= tail) ? (head - tail) : (STL_TXBUFSIZE - (tail - head));
	if ((size == 0) && test_bit(ASYI_TXBUSY, &portp->istate))
		size = 1;
	return(size);
}

/*****************************************************************************/

/*
 *	Generate the serial struct info.
 */

static void stl_getserial(stlport_t *portp, struct serial_struct *sp)
{
	struct serial_struct	sio;
	stlbrd_t		*brdp;

#if DEBUG
	printk("stl_getserial(portp=%x,sp=%x)\n", (int) portp, (int) sp);
#endif

	memset(&sio, 0, sizeof(struct serial_struct));
	sio.line = portp->portnr;
	sio.port = portp->ioaddr;
	sio.flags = portp->flags;
	sio.baud_base = portp->baud_base;
	sio.close_delay = portp->close_delay;
	sio.closing_wait = portp->closing_wait;
	sio.custom_divisor = portp->custom_divisor;
	sio.hub6 = 0;
	if (portp->uartp == &stl_cd1400uart) {
		sio.type = PORT_CIRRUS;
		sio.xmit_fifo_size = CD1400_TXFIFOSIZE;
	} else {
		sio.type = PORT_UNKNOWN;
		sio.xmit_fifo_size = SC26198_TXFIFOSIZE;
	}

	brdp = stl_brds[portp->brdnr];
	if (brdp != (stlbrd_t *) NULL)
		sio.irq = brdp->irq;

	copy_to_user(sp, &sio, sizeof(struct serial_struct));
}

/*****************************************************************************/

/*
 *	Set port according to the serial struct info.
 *	At this point we do not do any auto-configure stuff, so we will
 *	just quietly ignore any requests to change irq, etc.
 */

static int stl_setserial(stlport_t *portp, struct serial_struct *sp)
{
	struct serial_struct	sio;

#if DEBUG
	printk("stl_setserial(portp=%x,sp=%x)\n", (int) portp, (int) sp);
#endif

	copy_from_user(&sio, sp, sizeof(struct serial_struct));
	if (!capable(CAP_SYS_ADMIN)) {
		if ((sio.baud_base != portp->baud_base) ||
		    (sio.close_delay != portp->close_delay) ||
		    ((sio.flags & ~ASYNC_USR_MASK) !=
		    (portp->flags & ~ASYNC_USR_MASK)))
			return(-EPERM);
	} 

	portp->flags = (portp->flags & ~ASYNC_USR_MASK) |
		(sio.flags & ASYNC_USR_MASK);
	portp->baud_base = sio.baud_base;
	portp->close_delay = sio.close_delay;
	portp->closing_wait = sio.closing_wait;
	portp->custom_divisor = sio.custom_divisor;
	stl_setport(portp, portp->tty->termios);
	return(0);
}

/*****************************************************************************/

static int stl_ioctl(struct tty_struct *tty, struct file *file, unsigned int cmd, unsigned long arg)
{
	stlport_t	*portp;
	unsigned int	ival;
	int		rc;

#if DEBUG
	printk("stl_ioctl(tty=%x,file=%x,cmd=%x,arg=%x)\n",
		(int) tty, (int) file, cmd, (int) arg);
#endif

	if (tty == (struct tty_struct *) NULL)
		return(-ENODEV);
	portp = tty->driver_data;
	if (portp == (stlport_t *) NULL)
		return(-ENODEV);

	if ((cmd != TIOCGSERIAL) && (cmd != TIOCSSERIAL) &&
 	    (cmd != COM_GETPORTSTATS) && (cmd != COM_CLRPORTSTATS)) {
		if (tty->flags & (1 << TTY_IO_ERROR))
			return(-EIO);
	}

	rc = 0;

	switch (cmd) {
	case TIOCGSOFTCAR:
		rc = put_user(((tty->termios->c_cflag & CLOCAL) ? 1 : 0),
			(unsigned int *) arg);
		break;
	case TIOCSSOFTCAR:
		if ((rc = verify_area(VERIFY_READ, (void *) arg,
		    sizeof(int))) == 0) {
			get_user(ival, (unsigned int *) arg);
			tty->termios->c_cflag =
				(tty->termios->c_cflag & ~CLOCAL) |
				(ival ? CLOCAL : 0);
		}
		break;
	case TIOCMGET:
		if ((rc = verify_area(VERIFY_WRITE, (void *) arg,
		    sizeof(unsigned int))) == 0) {
			ival = stl_getsignals(portp);
			put_user(ival, (unsigned int *) arg);
		}
		break;
	case TIOCMBIS:
		if ((rc = verify_area(VERIFY_READ, (void *) arg,
		    sizeof(unsigned int))) == 0) {
			get_user(ival, (unsigned int *) arg);
			stl_setsignals(portp, ((ival & TIOCM_DTR) ? 1 : -1),
				((ival & TIOCM_RTS) ? 1 : -1));
		}
		break;
	case TIOCMBIC:
		if ((rc = verify_area(VERIFY_READ, (void *) arg,
		    sizeof(unsigned int))) == 0) {
			get_user(ival, (unsigned int *) arg);
			stl_setsignals(portp, ((ival & TIOCM_DTR) ? 0 : -1),
				((ival & TIOCM_RTS) ? 0 : -1));
		}
		break;
	case TIOCMSET:
		if ((rc = verify_area(VERIFY_READ, (void *) arg,
		    sizeof(unsigned int))) == 0) {
			get_user(ival, (unsigned int *) arg);
			stl_setsignals(portp, ((ival & TIOCM_DTR) ? 1 : 0),
				((ival & TIOCM_RTS) ? 1 : 0));
		}
		break;
	case TIOCGSERIAL:
		if ((rc = verify_area(VERIFY_WRITE, (void *) arg,
		    sizeof(struct serial_struct))) == 0)
			stl_getserial(portp, (struct serial_struct *) arg);
		break;
	case TIOCSSERIAL:
		if ((rc = verify_area(VERIFY_READ, (void *) arg,
		    sizeof(struct serial_struct))) == 0)
			rc = stl_setserial(portp, (struct serial_struct *) arg);
		break;
	case COM_GETPORTSTATS:
		if ((rc = verify_area(VERIFY_WRITE, (void *) arg,
		    sizeof(comstats_t))) == 0)
			rc = stl_getportstats(portp, (comstats_t *) arg);
		break;
	case COM_CLRPORTSTATS:
		if ((rc = verify_area(VERIFY_WRITE, (void *) arg,
		    sizeof(comstats_t))) == 0)
			rc = stl_clrportstats(portp, (comstats_t *) arg);
		break;
	case TIOCSERCONFIG:
	case TIOCSERGWILD:
	case TIOCSERSWILD:
	case TIOCSERGETLSR:
	case TIOCSERGSTRUCT:
	case TIOCSERGETMULTI:
	case TIOCSERSETMULTI:
	default:
		rc = -ENOIOCTLCMD;
		break;
	}

	return(rc);
}

/*****************************************************************************/

static void stl_settermios(struct tty_struct *tty, struct termios *old)
{
	stlport_t	*portp;
	struct termios	*tiosp;

#if DEBUG
	printk("stl_settermios(tty=%x,old=%x)\n", (int) tty, (int) old);
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	portp = tty->driver_data;
	if (portp == (stlport_t *) NULL)
		return;

	tiosp = tty->termios;
	if ((tiosp->c_cflag == old->c_cflag) &&
	    (tiosp->c_iflag == old->c_iflag))
		return;

	stl_setport(portp, tiosp);
	stl_setsignals(portp, ((tiosp->c_cflag & (CBAUD & ~CBAUDEX)) ? 1 : 0),
		-1);
	if ((old->c_cflag & CRTSCTS) && ((tiosp->c_cflag & CRTSCTS) == 0)) {
		tty->hw_stopped = 0;
		stl_start(tty);
	}
	if (((old->c_cflag & CLOCAL) == 0) && (tiosp->c_cflag & CLOCAL))
		wake_up_interruptible(&portp->open_wait);
}

/*****************************************************************************/

/*
 *	Attempt to flow control who ever is sending us data. Based on termios
 *	settings use software or/and hardware flow control.
 */

static void stl_throttle(struct tty_struct *tty)
{
	stlport_t	*portp;

#if DEBUG
	printk("stl_throttle(tty=%x)\n", (int) tty);
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	portp = tty->driver_data;
	if (portp == (stlport_t *) NULL)
		return;
	stl_flowctrl(portp, 0);
}

/*****************************************************************************/

/*
 *	Unflow control the device sending us data...
 */

static void stl_unthrottle(struct tty_struct *tty)
{
	stlport_t	*portp;

#if DEBUG
	printk("stl_unthrottle(tty=%x)\n", (int) tty);
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	portp = tty->driver_data;
	if (portp == (stlport_t *) NULL)
		return;
	stl_flowctrl(portp, 1);
}

/*****************************************************************************/

/*
 *	Stop the transmitter. Basically to do this we will just turn TX
 *	interrupts off.
 */

static void stl_stop(struct tty_struct *tty)
{
	stlport_t	*portp;

#if DEBUG
	printk("stl_stop(tty=%x)\n", (int) tty);
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	portp = tty->driver_data;
	if (portp == (stlport_t *) NULL)
		return;
	stl_startrxtx(portp, -1, 0);
}

/*****************************************************************************/

/*
 *	Start the transmitter again. Just turn TX interrupts back on.
 */

static void stl_start(struct tty_struct *tty)
{
	stlport_t	*portp;

#if DEBUG
	printk("stl_start(tty=%x)\n", (int) tty);
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	portp = tty->driver_data;
	if (portp == (stlport_t *) NULL)
		return;
	stl_startrxtx(portp, -1, 1);
}

/*****************************************************************************/

/*
 *	Hangup this port. This is pretty much like closing the port, only
 *	a little more brutal. No waiting for data to drain. Shutdown the
 *	port and maybe drop signals.
 */

static void stl_hangup(struct tty_struct *tty)
{
	stlport_t	*portp;

#if DEBUG
	printk("stl_hangup(tty=%x)\n", (int) tty);
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	portp = tty->driver_data;
	if (portp == (stlport_t *) NULL)
		return;

	portp->flags &= ~ASYNC_INITIALIZED;
	stl_disableintrs(portp);
	if (tty->termios->c_cflag & HUPCL)
		stl_setsignals(portp, 0, 0);
	stl_enablerxtx(portp, 0, 0);
	stl_flushbuffer(tty);
	portp->istate = 0;
	set_bit(TTY_IO_ERROR, &tty->flags);
	if (portp->tx.buf != (char *) NULL) {
		kfree(portp->tx.buf);
		portp->tx.buf = (char *) NULL;
		portp->tx.head = (char *) NULL;
		portp->tx.tail = (char *) NULL;
	}
	portp->tty = (struct tty_struct *) NULL;
	portp->flags &= ~(ASYNC_NORMAL_ACTIVE | ASYNC_CALLOUT_ACTIVE);
	portp->refcount = 0;
	wake_up_interruptible(&portp->open_wait);
}

/*****************************************************************************/

static void stl_flushbuffer(struct tty_struct *tty)
{
	stlport_t	*portp;

#if DEBUG
	printk("stl_flushbuffer(tty=%x)\n", (int) tty);
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	portp = tty->driver_data;
	if (portp == (stlport_t *) NULL)
		return;

	stl_flush(portp);
	wake_up_interruptible(&tty->write_wait);
	if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
	    tty->ldisc.write_wakeup)
		(tty->ldisc.write_wakeup)(tty);
}

/*****************************************************************************/

static void stl_breakctl(struct tty_struct *tty, int state)
{
	stlport_t	*portp;

#if DEBUG
	printk("stl_breakctl(tty=%x,state=%d)\n", (int) tty, state);
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	portp = tty->driver_data;
	if (portp == (stlport_t *) NULL)
		return;

	stl_sendbreak(portp, ((state == -1) ? 1 : 2));
}

/*****************************************************************************/

static void stl_waituntilsent(struct tty_struct *tty, int timeout)
{
	stlport_t	*portp;
	unsigned long	tend;

#if DEBUG
	printk("stl_waituntilsent(tty=%x,timeout=%d)\n", (int) tty, timeout);
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	portp = tty->driver_data;
	if (portp == (stlport_t *) NULL)
		return;

	if (timeout == 0)
		timeout = HZ;
	tend = jiffies + timeout;

	while (stl_datastate(portp)) {
		if (signal_pending(current))
			break;
		stl_delay(2);
		if (time_after_eq(jiffies, tend))
			break;
	}
}

/*****************************************************************************/

static void stl_sendxchar(struct tty_struct *tty, char ch)
{
	stlport_t	*portp;

#if DEBUG
	printk("stl_sendxchar(tty=%x,ch=%x)\n", (int) tty, ch);
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	portp = tty->driver_data;
	if (portp == (stlport_t *) NULL)
		return;

	if (ch == STOP_CHAR(tty))
		stl_sendflow(portp, 0);
	else if (ch == START_CHAR(tty))
		stl_sendflow(portp, 1);
	else
		stl_putchar(tty, ch);
}

/*****************************************************************************/

#define	MAXLINE		80

/*
 *	Format info for a specified port. The line is deliberately limited
 *	to 80 characters. (If it is too long it will be truncated, if too
 *	short then padded with spaces).
 */

static int stl_portinfo(stlport_t *portp, int portnr, char *pos)
{
	char	*sp;
	int	sigs, cnt;

	sp = pos;
	sp += sprintf(sp, "%d: uart:%s tx:%d rx:%d",
		portnr, (portp->hwid == 1) ? "SC26198" : "CD1400",
		(int) portp->stats.txtotal, (int) portp->stats.rxtotal);

	if (portp->stats.rxframing)
		sp += sprintf(sp, " fe:%d", (int) portp->stats.rxframing);
	if (portp->stats.rxparity)
		sp += sprintf(sp, " pe:%d", (int) portp->stats.rxparity);
	if (portp->stats.rxbreaks)
		sp += sprintf(sp, " brk:%d", (int) portp->stats.rxbreaks);
	if (portp->stats.rxoverrun)
		sp += sprintf(sp, " oe:%d", (int) portp->stats.rxoverrun);

	sigs = stl_getsignals(portp);
	cnt = sprintf(sp, "%s%s%s%s%s ",
		(sigs & TIOCM_RTS) ? "|RTS" : "",
		(sigs & TIOCM_CTS) ? "|CTS" : "",
		(sigs & TIOCM_DTR) ? "|DTR" : "",
		(sigs & TIOCM_CD) ? "|DCD" : "",
		(sigs & TIOCM_DSR) ? "|DSR" : "");
	*sp = ' ';
	sp += cnt;

	for (cnt = (sp - pos); (cnt < (MAXLINE - 1)); cnt++)
		*sp++ = ' ';
	if (cnt >= MAXLINE)
		pos[(MAXLINE - 2)] = '+';
	pos[(MAXLINE - 1)] = '\n';

	return(MAXLINE);
}

/*****************************************************************************/

/*
 *	Port info, read from the /proc file system.
 */

static int stl_readproc(char *page, char **start, off_t off, int count, int *eof, void *data)
{
	stlbrd_t	*brdp;
	stlpanel_t	*panelp;
	stlport_t	*portp;
	int		brdnr, panelnr, portnr, totalport;
	int		curoff, maxoff;
	char		*pos;

#if DEBUG
	printk("stl_readproc(page=%x,start=%x,off=%x,count=%d,eof=%x,"
		"data=%x\n", (int) page, (int) start, (int) off, count,
		(int) eof, (int) data);
#endif

	pos = page;
	totalport = 0;
	curoff = 0;

	if (off == 0) {
		pos += sprintf(pos, "%s: version %s", stl_drvtitle,
			stl_drvversion);
		while (pos < (page + MAXLINE - 1))
			*pos++ = ' ';
		*pos++ = '\n';
	}
	curoff =  MAXLINE;

/*
 *	We scan through for each board, panel and port. The offset is
 *	calculated on the fly, and irrelevant ports are skipped.
 */
	for (brdnr = 0; (brdnr < stl_nrbrds); brdnr++) {
		brdp = stl_brds[brdnr];
		if (brdp == (stlbrd_t *) NULL)
			continue;
		if (brdp->state == 0)
			continue;

		maxoff = curoff + (brdp->nrports * MAXLINE);
		if (off >= maxoff) {
			curoff = maxoff;
			continue;
		}

		totalport = brdnr * STL_MAXPORTS;
		for (panelnr = 0; (panelnr < brdp->nrpanels); panelnr++) {
			panelp = brdp->panels[panelnr];
			if (panelp == (stlpanel_t *) NULL)
				continue;

			maxoff = curoff + (panelp->nrports * MAXLINE);
			if (off >= maxoff) {
				curoff = maxoff;
				totalport += panelp->nrports;
				continue;
			}

			for (portnr = 0; (portnr < panelp->nrports); portnr++,
			    totalport++) {
				portp = panelp->ports[portnr];
				if (portp == (stlport_t *) NULL)
					continue;
				if (off >= (curoff += MAXLINE))
					continue;
				if ((pos - page + MAXLINE) > count)
					goto stl_readdone;
				pos += stl_portinfo(portp, totalport, pos);
			}
		}
	}

	*eof = 1;

stl_readdone:
	*start = page;
	return(pos - page);
}

/*****************************************************************************/

/*
 *	All board interrupts are vectored through here first. This code then
 *	calls off to the approrpriate board interrupt handlers.
 */

static void stl_intr(int irq, void *dev_id, struct pt_regs *regs)
{
	stlbrd_t	*brdp;
	int		i;

#if DEBUG
	printk("stl_intr(irq=%d,regs=%x)\n", irq, (int) regs);
#endif

	for (i = 0; (i < stl_nrbrds); i++) {
		if ((brdp = stl_brds[i]) == (stlbrd_t *) NULL)
			continue;
		if (brdp->state == 0)
			continue;
		(* brdp->isr)(brdp);
	}
}

/*****************************************************************************/

/*
 *	Interrupt service routine for EasyIO board types.
 */

static void stl_eiointr(stlbrd_t *brdp)
{
	stlpanel_t	*panelp;
	unsigned int	iobase;

	panelp = brdp->panels[0];
	iobase = panelp->iobase;
	while (inb(brdp->iostatus) & EIO_INTRPEND)
		(* panelp->isr)(panelp, iobase);
}

/*****************************************************************************/

/*
 *	Interrupt service routine for ECH-AT board types.
 */

static void stl_echatintr(stlbrd_t *brdp)
{
	stlpanel_t	*panelp;
	unsigned int	ioaddr;
	int		bnknr;

	outb((brdp->ioctrlval | ECH_BRDENABLE), brdp->ioctrl);

	while (inb(brdp->iostatus) & ECH_INTRPEND) {
		for (bnknr = 0; (bnknr < brdp->nrbnks); bnknr++) {
			ioaddr = brdp->bnkstataddr[bnknr];
			if (inb(ioaddr) & ECH_PNLINTRPEND) {
				panelp = brdp->bnk2panel[bnknr];
				(* panelp->isr)(panelp, (ioaddr & 0xfffc));
			}
		}
	}

	outb((brdp->ioctrlval | ECH_BRDDISABLE), brdp->ioctrl);
}

/*****************************************************************************/

/*
 *	Interrupt service routine for ECH-MCA board types.
 */

static void stl_echmcaintr(stlbrd_t *brdp)
{
	stlpanel_t	*panelp;
	unsigned int	ioaddr;
	int		bnknr;

	while (inb(brdp->iostatus) & ECH_INTRPEND) {
		for (bnknr = 0; (bnknr < brdp->nrbnks); bnknr++) {
			ioaddr = brdp->bnkstataddr[bnknr];
			if (inb(ioaddr) & ECH_PNLINTRPEND) {
				panelp = brdp->bnk2panel[bnknr];
				(* panelp->isr)(panelp, (ioaddr & 0xfffc));
			}
		}
	}
}

/*****************************************************************************/

/*
 *	Interrupt service routine for ECH-PCI board types.
 */

static void stl_echpciintr(stlbrd_t *brdp)
{
	stlpanel_t	*panelp;
	unsigned int	ioaddr;
	int		bnknr, recheck;

	while (1) {
		recheck = 0;
		for (bnknr = 0; (bnknr < brdp->nrbnks); bnknr++) {
			outb(brdp->bnkpageaddr[bnknr], brdp->ioctrl);
			ioaddr = brdp->bnkstataddr[bnknr];
			if (inb(ioaddr) & ECH_PNLINTRPEND) {
				panelp = brdp->bnk2panel[bnknr];
				(* panelp->isr)(panelp, (ioaddr & 0xfffc));
				recheck++;
			}
		}
		if (! recheck)
			break;
	}
}

/*****************************************************************************/

/*
 *	Interrupt service routine for ECH-8/64-PCI board types.
 */

static void stl_echpci64intr(stlbrd_t *brdp)
{
	stlpanel_t	*panelp;
	unsigned int	ioaddr;
	int		bnknr;

	while (inb(brdp->ioctrl) & 0x1) {
		for (bnknr = 0; (bnknr < brdp->nrbnks); bnknr++) {
			ioaddr = brdp->bnkstataddr[bnknr];
			if (inb(ioaddr) & ECH_PNLINTRPEND) {
				panelp = brdp->bnk2panel[bnknr];
				(* panelp->isr)(panelp, (ioaddr & 0xfffc));
			}
		}
	}
}

/*****************************************************************************/

/*
 *	Service an off-level request for some channel.
 */
static void stl_offintr(void *private)
{
	stlport_t		*portp;
	struct tty_struct	*tty;
	unsigned int		oldsigs;

	portp = private;

#if DEBUG
	printk("stl_offintr(portp=%x)\n", (int) portp);
#endif

	if (portp == (stlport_t *) NULL)
		goto out;

	tty = portp->tty;
	if (tty == (struct tty_struct *) NULL)
		goto out;

	lock_kernel();
	if (test_bit(ASYI_TXLOW, &portp->istate)) {
		if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
		    tty->ldisc.write_wakeup)
			(tty->ldisc.write_wakeup)(tty);
		wake_up_interruptible(&tty->write_wait);
	}
	if (test_bit(ASYI_DCDCHANGE, &portp->istate)) {
		clear_bit(ASYI_DCDCHANGE, &portp->istate);
		oldsigs = portp->sigs;
		portp->sigs = stl_getsignals(portp);
		if ((portp->sigs & TIOCM_CD) && ((oldsigs & TIOCM_CD) == 0))
			wake_up_interruptible(&portp->open_wait);
		if ((oldsigs & TIOCM_CD) && ((portp->sigs & TIOCM_CD) == 0)) {
			if (portp->flags & ASYNC_CHECK_CD) {
				if (! ((portp->flags & ASYNC_CALLOUT_ACTIVE) &&
				    (portp->flags & ASYNC_CALLOUT_NOHUP))) {
					tty_hangup(tty);	/* FIXME: module removal race here - AKPM */
				}
			}
		}
	}
	unlock_kernel();
out:
	MOD_DEC_USE_COUNT;
}

/*****************************************************************************/

/*
 *	Map in interrupt vector to this driver. Check that we don't
 *	already have this vector mapped, we might be sharing this
 *	interrupt across multiple boards.
 */

static int __init stl_mapirq(int irq, char *name)
{
	int	rc, i;

#if DEBUG
	printk("stl_mapirq(irq=%d,name=%s)\n", irq, name);
#endif

	rc = 0;
	for (i = 0; (i < stl_numintrs); i++) {
		if (stl_gotintrs[i] == irq)
			break;
	}
	if (i >= stl_numintrs) {
		if (request_irq(irq, stl_intr, SA_SHIRQ, name, NULL) != 0) {
			printk("STALLION: failed to register interrupt "
				"routine for %s irq=%d\n", name, irq);
			rc = -ENODEV;
		} else {
			stl_gotintrs[stl_numintrs++] = irq;
		}
	}
	return(rc);
}

/*****************************************************************************/

/*
 *	Initialize all the ports on a panel.
 */

static int __init stl_initports(stlbrd_t *brdp, stlpanel_t *panelp)
{
	stlport_t	*portp;
	int		chipmask, i;

#if DEBUG
	printk("stl_initports(brdp=%x,panelp=%x)\n", (int) brdp, (int) panelp);
#endif

	chipmask = stl_panelinit(brdp, panelp);

/*
 *	All UART's are initialized (if found!). Now go through and setup
 *	each ports data structures.
 */
	for (i = 0; (i < panelp->nrports); i++) {
		portp = (stlport_t *) stl_memalloc(sizeof(stlport_t));
		if (portp == (stlport_t *) NULL) {
			printk("STALLION: failed to allocate memory "
				"(size=%d)\n", sizeof(stlport_t));
			break;
		}
		memset(portp, 0, sizeof(stlport_t));

		portp->magic = STL_PORTMAGIC;
		portp->portnr = i;
		portp->brdnr = panelp->brdnr;
		portp->panelnr = panelp->panelnr;
		portp->uartp = panelp->uartp;
		portp->clk = brdp->clk;
		portp->baud_base = STL_BAUDBASE;
		portp->close_delay = STL_CLOSEDELAY;
		portp->closing_wait = 30 * HZ;
		portp->normaltermios = stl_deftermios;
		portp->callouttermios = stl_deftermios;
		portp->tqueue.routine = stl_offintr;
		portp->tqueue.data = portp;
		init_waitqueue_head(&portp->open_wait);
		init_waitqueue_head(&portp->close_wait);
		portp->stats.brd = portp->brdnr;
		portp->stats.panel = portp->panelnr;
		portp->stats.port = portp->portnr;
		panelp->ports[i] = portp;
		stl_portinit(brdp, panelp, portp);
	}

	return(0);
}

/*****************************************************************************/

/*
 *	Try to find and initialize an EasyIO board.
 */

static inline int stl_initeio(stlbrd_t *brdp)
{
	stlpanel_t	*panelp;
	unsigned int	status;
	char		*name;
	int		rc;

#if DEBUG
	printk("stl_initeio(brdp=%x)\n", (int) brdp);
#endif

	brdp->ioctrl = brdp->ioaddr1 + 1;
	brdp->iostatus = brdp->ioaddr1 + 2;

	status = inb(brdp->iostatus);
	if ((status & EIO_IDBITMASK) == EIO_MK3)
		brdp->ioctrl++;

/*
 *	Handle board specific stuff now. The real difference is PCI
 *	or not PCI.
 */
	if (brdp->brdtype == BRD_EASYIOPCI) {
		brdp->iosize1 = 0x80;
		brdp->iosize2 = 0x80;
		name = "serial(EIO-PCI)";
		outb(0x41, (brdp->ioaddr2 + 0x4c));
	} else {
		brdp->iosize1 = 8;
		name = "serial(EIO)";
		if ((brdp->irq < 0) || (brdp->irq > 15) ||
		    (stl_vecmap[brdp->irq] == (unsigned char) 0xff)) {
			printk("STALLION: invalid irq=%d for brd=%d\n",
				brdp->irq, brdp->brdnr);
			return(-EINVAL);
		}
		outb((stl_vecmap[brdp->irq] | EIO_0WS |
			((brdp->irqtype) ? EIO_INTLEVEL : EIO_INTEDGE)),
			brdp->ioctrl);
	}

	if (check_region(brdp->ioaddr1, brdp->iosize1)) {
		printk("STALLION: Warning, board %d I/O address %x conflicts "
			"with another device\n", brdp->brdnr, brdp->ioaddr1);
	}
	if (brdp->iosize2 > 0) {
		if (check_region(brdp->ioaddr2, brdp->iosize2)) {
			printk("STALLION: Warning, board %d I/O address %x "
				"conflicts with another device\n",
				brdp->brdnr, brdp->ioaddr2);
		}
	}
 
/*
 *	Everything looks OK, so let's go ahead and probe for the hardware.
 */
	brdp->clk = CD1400_CLK;
	brdp->isr = stl_eiointr;

	switch (status & EIO_IDBITMASK) {
	case EIO_8PORTM:
		brdp->clk = CD1400_CLK8M;
		/* fall thru */
	case EIO_8PORTRS:
	case EIO_8PORTDI:
		brdp->nrports = 8;
		break;
	case EIO_4PORTRS:
		brdp->nrports = 4;
		break;
	case EIO_MK3:
		switch (status & EIO_BRDMASK) {
		case ID_BRD4:
			brdp->nrports = 4;
			break;
		case ID_BRD8:
			brdp->nrports = 8;
			break;
		case ID_BRD16:
			brdp->nrports = 16;
			break;
		default:
			return(-ENODEV);
		}
		break;
	default:
		return(-ENODEV);
	}

/*
 *	We have verfied that the board is actually present, so now we
 *	can complete the setup.
 */
	request_region(brdp->ioaddr1, brdp->iosize1, name);
	if (brdp->iosize2 > 0)
		request_region(brdp->ioaddr2, brdp->iosize2, name);

	panelp = (stlpanel_t *) stl_memalloc(sizeof(stlpanel_t));
	if (panelp == (stlpanel_t *) NULL) {
		printk("STALLION: failed to allocate memory (size=%d)\n",
			sizeof(stlpanel_t));
		return(-ENOMEM);
	}
	memset(panelp, 0, sizeof(stlpanel_t));

	panelp->magic = STL_PANELMAGIC;
	panelp->brdnr = brdp->brdnr;
	panelp->panelnr = 0;
	panelp->nrports = brdp->nrports;
	panelp->iobase = brdp->ioaddr1;
	panelp->hwid = status;
	if ((status & EIO_IDBITMASK) == EIO_MK3) {
		panelp->uartp = (void *) &stl_sc26198uart;
		panelp->isr = stl_sc26198intr;
	} else {
		panelp->uartp = (void *) &stl_cd1400uart;
		panelp->isr = stl_cd1400eiointr;
	}

	brdp->panels[0] = panelp;
	brdp->nrpanels = 1;
	brdp->state |= BRD_FOUND;
	brdp->hwid = status;
	rc = stl_mapirq(brdp->irq, name);
	return(rc);
}

/*****************************************************************************/

/*
 *	Try to find an ECH board and initialize it. This code is capable of
 *	dealing with all types of ECH board.
 */

static int inline stl_initech(stlbrd_t *brdp)
{
	stlpanel_t	*panelp;
	unsigned int	status, nxtid, ioaddr, conflict;
	int		panelnr, banknr, i;
	char		*name;

#if DEBUG
	printk("stl_initech(brdp=%x)\n", (int) brdp);
#endif

	status = 0;
	conflict = 0;

/*
 *	Set up the initial board register contents for boards. This varies a
 *	bit between the different board types. So we need to handle each
 *	separately. Also do a check that the supplied IRQ is good.
 */
	switch (brdp->brdtype) {

	case BRD_ECH:
		brdp->isr = stl_echatintr;
		brdp->ioctrl = brdp->ioaddr1 + 1;
		brdp->iostatus = brdp->ioaddr1 + 1;
		status = inb(brdp->iostatus);
		if ((status & ECH_IDBITMASK) != ECH_ID)
			return(-ENODEV);
		if ((brdp->irq < 0) || (brdp->irq > 15) ||
		    (stl_vecmap[brdp->irq] == (unsigned char) 0xff)) {
			printk("STALLION: invalid irq=%d for brd=%d\n",
				brdp->irq, brdp->brdnr);
			return(-EINVAL);
		}
		status = ((brdp->ioaddr2 & ECH_ADDR2MASK) >> 1);
		status |= (stl_vecmap[brdp->irq] << 1);
		outb((status | ECH_BRDRESET), brdp->ioaddr1);
		brdp->ioctrlval = ECH_INTENABLE |
			((brdp->irqtype) ? ECH_INTLEVEL : ECH_INTEDGE);
		for (i = 0; (i < 10); i++)
			outb((brdp->ioctrlval | ECH_BRDENABLE), brdp->ioctrl);
		brdp->iosize1 = 2;
		brdp->iosize2 = 32;
		name = "serial(EC8/32)";
		outb(status, brdp->ioaddr1);
		break;

	case BRD_ECHMC:
		brdp->isr = stl_echmcaintr;
		brdp->ioctrl = brdp->ioaddr1 + 0x20;
		brdp->iostatus = brdp->ioctrl;
		status = inb(brdp->iostatus);
		if ((status & ECH_IDBITMASK) != ECH_ID)
			return(-ENODEV);
		if ((brdp->irq < 0) || (brdp->irq > 15) ||
		    (stl_vecmap[brdp->irq] == (unsigned char) 0xff)) {
			printk("STALLION: invalid irq=%d for brd=%d\n",
				brdp->irq, brdp->brdnr);
			return(-EINVAL);
		}
		outb(ECHMC_BRDRESET, brdp->ioctrl);
		outb(ECHMC_INTENABLE, brdp->ioctrl);
		brdp->iosize1 = 64;
		name = "serial(EC8/32-MC)";
		break;

	case BRD_ECHPCI:
		brdp->isr = stl_echpciintr;
		brdp->ioctrl = brdp->ioaddr1 + 2;
		brdp->iosize1 = 4;
		brdp->iosize2 = 8;
		name = "serial(EC8/32-PCI)";
		break;

	case BRD_ECH64PCI:
		brdp->isr = stl_echpci64intr;
		brdp->ioctrl = brdp->ioaddr2 + 0x40;
		outb(0x43, (brdp->ioaddr1 + 0x4c));
		brdp->iosize1 = 0x80;
		brdp->iosize2 = 0x80;
		name = "serial(EC8/64-PCI)";
		break;

	default:
		printk("STALLION: unknown board type=%d\n", brdp->brdtype);
		return(-EINVAL);
		break;
	}

/*
 *	Check boards for possible IO address conflicts. We won't actually
 *	do anything about it here, just issue a warning...
 */
	conflict = check_region(brdp->ioaddr1, brdp->iosize1) ?
		brdp->ioaddr1 : 0;
	if ((conflict == 0) && (brdp->iosize2 > 0))
		conflict = check_region(brdp->ioaddr2, brdp->iosize2) ?
			brdp->ioaddr2 : 0;
	if (conflict) {
		printk("STALLION: Warning, board %d I/O address %x conflicts "
			"with another device\n", brdp->brdnr, conflict);
	}

	request_region(brdp->ioaddr1, brdp->iosize1, name);
	if (brdp->iosize2 > 0)
		request_region(brdp->ioaddr2, brdp->iosize2, name);

/*
 *	Scan through the secondary io address space looking for panels.
 *	As we find'em allocate and initialize panel structures for each.
 */
	brdp->clk = CD1400_CLK;
	brdp->hwid = status;

	ioaddr = brdp->ioaddr2;
	banknr = 0;
	panelnr = 0;
	nxtid = 0;

	for (i = 0; (i < STL_MAXPANELS); i++) {
		if (brdp->brdtype == BRD_ECHPCI) {
			outb(nxtid, brdp->ioctrl);
			ioaddr = brdp->ioaddr2;
		}
		status = inb(ioaddr + ECH_PNLSTATUS);
		if ((status & ECH_PNLIDMASK) != nxtid)
			break;
		panelp = (stlpanel_t *) stl_memalloc(sizeof(stlpanel_t));
		if (panelp == (stlpanel_t *) NULL) {
			printk("STALLION: failed to allocate memory "
				"(size=%d)\n", sizeof(stlpanel_t));
			break;
		}
		memset(panelp, 0, sizeof(stlpanel_t));
		panelp->magic = STL_PANELMAGIC;
		panelp->brdnr = brdp->brdnr;
		panelp->panelnr = panelnr;
		panelp->iobase = ioaddr;
		panelp->pagenr = nxtid;
		panelp->hwid = status;
		brdp->bnk2panel[banknr] = panelp;
		brdp->bnkpageaddr[banknr] = nxtid;
		brdp->bnkstataddr[banknr++] = ioaddr + ECH_PNLSTATUS;

		if (status & ECH_PNLXPID) {
			panelp->uartp = (void *) &stl_sc26198uart;
			panelp->isr = stl_sc26198intr;
			if (status & ECH_PNL16PORT) {
				panelp->nrports = 16;
				brdp->bnk2panel[banknr] = panelp;
				brdp->bnkpageaddr[banknr] = nxtid;
				brdp->bnkstataddr[banknr++] = ioaddr + 4 +
					ECH_PNLSTATUS;
			} else {
				panelp->nrports = 8;
			}
		} else {
			panelp->uartp = (void *) &stl_cd1400uart;
			panelp->isr = stl_cd1400echintr;
			if (status & ECH_PNL16PORT) {
				panelp->nrports = 16;
				panelp->ackmask = 0x80;
				if (brdp->brdtype != BRD_ECHPCI)
					ioaddr += EREG_BANKSIZE;
				brdp->bnk2panel[banknr] = panelp;
				brdp->bnkpageaddr[banknr] = ++nxtid;
				brdp->bnkstataddr[banknr++] = ioaddr +
					ECH_PNLSTATUS;
			} else {
				panelp->nrports = 8;
				panelp->ackmask = 0xc0;
			}
		}

		nxtid++;
		ioaddr += EREG_BANKSIZE;
		brdp->nrports += panelp->nrports;
		brdp->panels[panelnr++] = panelp;
		if ((brdp->brdtype != BRD_ECHPCI) &&
		    (ioaddr >= (brdp->ioaddr2 + brdp->iosize2)))
			break;
	}

	brdp->nrpanels = panelnr;
	brdp->nrbnks = banknr;
	if (brdp->brdtype == BRD_ECH)
		outb((brdp->ioctrlval | ECH_BRDDISABLE), brdp->ioctrl);

	brdp->state |= BRD_FOUND;
	i = stl_mapirq(brdp->irq, name);
	return(i);
}

/*****************************************************************************/

/*
 *	Initialize and configure the specified board.
 *	Scan through all the boards in the configuration and see what we
 *	can find. Handle EIO and the ECH boards a little differently here
 *	since the initial search and setup is very different.
 */

static int __init stl_brdinit(stlbrd_t *brdp)
{
	int	i;

#if DEBUG
	printk("stl_brdinit(brdp=%x)\n", (int) brdp);
#endif

	switch (brdp->brdtype) {
	case BRD_EASYIO:
	case BRD_EASYIOPCI:
		stl_initeio(brdp);
		break;
	case BRD_ECH:
	case BRD_ECHMC:
	case BRD_ECHPCI:
	case BRD_ECH64PCI:
		stl_initech(brdp);
		break;
	default:
		printk("STALLION: board=%d is unknown board type=%d\n",
			brdp->brdnr, brdp->brdtype);
		return(ENODEV);
	}

	stl_brds[brdp->brdnr] = brdp;
	if ((brdp->state & BRD_FOUND) == 0) {
		printk("STALLION: %s board not found, board=%d io=%x irq=%d\n",
			stl_brdnames[brdp->brdtype], brdp->brdnr,
			brdp->ioaddr1, brdp->irq);
		return(ENODEV);
	}

	for (i = 0; (i < STL_MAXPANELS); i++)
		if (brdp->panels[i] != (stlpanel_t *) NULL)
			stl_initports(brdp, brdp->panels[i]);

	printk("STALLION: %s found, board=%d io=%x irq=%d "
		"nrpanels=%d nrports=%d\n", stl_brdnames[brdp->brdtype],
		brdp->brdnr, brdp->ioaddr1, brdp->irq, brdp->nrpanels,
		brdp->nrports);
	return(0);
}

/*****************************************************************************/

/*
 *	Find the next available board number that is free.
 */

static inline int stl_getbrdnr()
{
	int	i;

	for (i = 0; (i < STL_MAXBRDS); i++) {
		if (stl_brds[i] == (stlbrd_t *) NULL) {
			if (i >= stl_nrbrds)
				stl_nrbrds = i + 1;
			return(i);
		}
	}
	return(-1);
}

/*****************************************************************************/

#ifdef	CONFIG_PCI

/*
 *	We have a Stallion board. Allocate a board structure and
 *	initialize it. Read its IO and IRQ resources from PCI
 *	configuration space.
 */

static inline int stl_initpcibrd(int brdtype, struct pci_dev *devp)
{
	stlbrd_t	*brdp;

#if DEBUG
	printk("stl_initpcibrd(brdtype=%d,busnr=%x,devnr=%x)\n", brdtype,
		devp->bus->number, devp->devfn);
#endif

	if (pci_enable_device(devp))
		return(-EIO);
	if ((brdp = stl_allocbrd()) == (stlbrd_t *) NULL)
		return(-ENOMEM);
	if ((brdp->brdnr = stl_getbrdnr()) < 0) {
		printk("STALLION: too many boards found, "
			"maximum supported %d\n", STL_MAXBRDS);
		return(0);
	}
	brdp->brdtype = brdtype;

/*
 *	Different Stallion boards use the BAR registers in different ways,
 *	so set up io addresses based on board type.
 */
#if DEBUG
	printk("%s(%d): BAR[]=%x,%x,%x,%x IRQ=%x\n", __FILE__, __LINE__,
		pci_resource_start(devp, 0), pci_resource_start(devp, 1),
		pci_resource_start(devp, 2), pci_resource_start(devp, 3), devp->irq);
#endif

/*
 *	We have all resources from the board, so let's setup the actual
 *	board structure now.
 */
	switch (brdtype) {
	case BRD_ECHPCI:
		brdp->ioaddr2 = pci_resource_start(devp, 0);
		brdp->ioaddr1 = pci_resource_start(devp, 1);
		break;
	case BRD_ECH64PCI:
		brdp->ioaddr2 = pci_resource_start(devp, 2);
		brdp->ioaddr1 = pci_resource_start(devp, 1);
		break;
	case BRD_EASYIOPCI:
		brdp->ioaddr1 = pci_resource_start(devp, 2);
		brdp->ioaddr2 = pci_resource_start(devp, 1);
		break;
	default:
		printk("STALLION: unknown PCI board type=%d\n", brdtype);
		break;
	}

	brdp->irq = devp->irq;
	stl_brdinit(brdp);

	return(0);
}

/*****************************************************************************/

/*
 *	Find all Stallion PCI boards that might be installed. Initialize each
 *	one as it is found.
 */


static inline int stl_findpcibrds()
{
	struct pci_dev	*dev = NULL;
	int		i, rc;

#if DEBUG
	printk("stl_findpcibrds()\n");
#endif

	if (! pci_present())
		return(0);

	for (i = 0; (i < stl_nrpcibrds); i++)
		while ((dev = pci_find_device(stl_pcibrds[i].vendid,
		    stl_pcibrds[i].devid, dev))) {

/*
 *			Found a device on the PCI bus that has our vendor and
 *			device ID. Need to check now that it is really us.
 */
			if ((dev->class >> 8) == PCI_CLASS_STORAGE_IDE)
				continue;

			rc = stl_initpcibrd(stl_pcibrds[i].brdtype, dev);
			if (rc)
				return(rc);
		}

	return(0);
}

#endif

/*****************************************************************************/

/*
 *	Scan through all the boards in the configuration and see what we
 *	can find. Handle EIO and the ECH boards a little differently here
 *	since the initial search and setup is too different.
 */

static inline int stl_initbrds()
{
	stlbrd_t	*brdp;
	stlconf_t	*confp;
	int		i;

#if DEBUG
	printk("stl_initbrds()\n");
#endif

	if (stl_nrbrds > STL_MAXBRDS) {
		printk("STALLION: too many boards in configuration table, "
			"truncating to %d\n", STL_MAXBRDS);
		stl_nrbrds = STL_MAXBRDS;
	}

/*
 *	Firstly scan the list of static boards configured. Allocate
 *	resources and initialize the boards as found.
 */
	for (i = 0; (i < stl_nrbrds); i++) {
		confp = &stl_brdconf[i];
#ifdef MODULE
		stl_parsebrd(confp, stl_brdsp[i]);
#endif
		if ((brdp = stl_allocbrd()) == (stlbrd_t *) NULL)
			return(-ENOMEM);
		brdp->brdnr = i;
		brdp->brdtype = confp->brdtype;
		brdp->ioaddr1 = confp->ioaddr1;
		brdp->ioaddr2 = confp->ioaddr2;
		brdp->irq = confp->irq;
		brdp->irqtype = confp->irqtype;
		stl_brdinit(brdp);
	}

/*
 *	Find any dynamically supported boards. That is via module load
 *	line options or auto-detected on the PCI bus.
 */
#ifdef MODULE
	stl_argbrds();
#endif
#ifdef CONFIG_PCI
	stl_findpcibrds();
#endif

	return(0);
}

/*****************************************************************************/

/*
 *	Return the board stats structure to user app.
 */

static int stl_getbrdstats(combrd_t *bp)
{
	stlbrd_t	*brdp;
	stlpanel_t	*panelp;
	int		i;

	copy_from_user(&stl_brdstats, bp, sizeof(combrd_t));
	if (stl_brdstats.brd >= STL_MAXBRDS)
		return(-ENODEV);
	brdp = stl_brds[stl_brdstats.brd];
	if (brdp == (stlbrd_t *) NULL)
		return(-ENODEV);

	memset(&stl_brdstats, 0, sizeof(combrd_t));
	stl_brdstats.brd = brdp->brdnr;
	stl_brdstats.type = brdp->brdtype;
	stl_brdstats.hwid = brdp->hwid;
	stl_brdstats.state = brdp->state;
	stl_brdstats.ioaddr = brdp->ioaddr1;
	stl_brdstats.ioaddr2 = brdp->ioaddr2;
	stl_brdstats.irq = brdp->irq;
	stl_brdstats.nrpanels = brdp->nrpanels;
	stl_brdstats.nrports = brdp->nrports;
	for (i = 0; (i < brdp->nrpanels); i++) {
		panelp = brdp->panels[i];
		stl_brdstats.panels[i].panel = i;
		stl_brdstats.panels[i].hwid = panelp->hwid;
		stl_brdstats.panels[i].nrports = panelp->nrports;
	}

	copy_to_user(bp, &stl_brdstats, sizeof(combrd_t));
	return(0);
}

/*****************************************************************************/

/*
 *	Resolve the referenced port number into a port struct pointer.
 */

static stlport_t *stl_getport(int brdnr, int panelnr, int portnr)
{
	stlbrd_t	*brdp;
	stlpanel_t	*panelp;

	if ((brdnr < 0) || (brdnr >= STL_MAXBRDS))
		return((stlport_t *) NULL);
	brdp = stl_brds[brdnr];
	if (brdp == (stlbrd_t *) NULL)
		return((stlport_t *) NULL);
	if ((panelnr < 0) || (panelnr >= brdp->nrpanels))
		return((stlport_t *) NULL);
	panelp = brdp->panels[panelnr];
	if (panelp == (stlpanel_t *) NULL)
		return((stlport_t *) NULL);
	if ((portnr < 0) || (portnr >= panelp->nrports))
		return((stlport_t *) NULL);
	return(panelp->ports[portnr]);
}

/*****************************************************************************/

/*
 *	Return the port stats structure to user app. A NULL port struct
 *	pointer passed in means that we need to find out from the app
 *	what port to get stats for (used through board control device).
 */

static int stl_getportstats(stlport_t *portp, comstats_t *cp)
{
	unsigned char	*head, *tail;
	unsigned long	flags;

	if (portp == (stlport_t *) NULL) {
		copy_from_user(&stl_comstats, cp, sizeof(comstats_t));
		portp = stl_getport(stl_comstats.brd, stl_comstats.panel,
			stl_comstats.port);
		if (portp == (stlport_t *) NULL)
			return(-ENODEV);
	}

	portp->stats.state = portp->istate;
	portp->stats.flags = portp->flags;
	portp->stats.hwid = portp->hwid;

	portp->stats.ttystate = 0;
	portp->stats.cflags = 0;
	portp->stats.iflags = 0;
	portp->stats.oflags = 0;
	portp->stats.lflags = 0;
	portp->stats.rxbuffered = 0;

	save_flags(flags);
	cli();
	if (portp->tty != (struct tty_struct *) NULL) {
		if (portp->tty->driver_data == portp) {
			portp->stats.ttystate = portp->tty->flags;
			portp->stats.rxbuffered = portp->tty->flip.count;
			if (portp->tty->termios != (struct termios *) NULL) {
				portp->stats.cflags = portp->tty->termios->c_cflag;
				portp->stats.iflags = portp->tty->termios->c_iflag;
				portp->stats.oflags = portp->tty->termios->c_oflag;
				portp->stats.lflags = portp->tty->termios->c_lflag;
			}
		}
	}
	restore_flags(flags);

	head = portp->tx.head;
	tail = portp->tx.tail;
	portp->stats.txbuffered = ((head >= tail) ? (head - tail) :
		(STL_TXBUFSIZE - (tail - head)));

	portp->stats.signals = (unsigned long) stl_getsignals(portp);

	copy_to_user(cp, &portp->stats, sizeof(comstats_t));
	return(0);
}

/*****************************************************************************/

/*
 *	Clear the port stats structure. We also return it zeroed out...
 */

static int stl_clrportstats(stlport_t *portp, comstats_t *cp)
{
	if (portp == (stlport_t *) NULL) {
		copy_from_user(&stl_comstats, cp, sizeof(comstats_t));
		portp = stl_getport(stl_comstats.brd, stl_comstats.panel,
			stl_comstats.port);
		if (portp == (stlport_t *) NULL)
			return(-ENODEV);
	}

	memset(&portp->stats, 0, sizeof(comstats_t));
	portp->stats.brd = portp->brdnr;
	portp->stats.panel = portp->panelnr;
	portp->stats.port = portp->portnr;
	copy_to_user(cp, &portp->stats, sizeof(comstats_t));
	return(0);
}

/*****************************************************************************/

/*
 *	Return the entire driver ports structure to a user app.
 */

static int stl_getportstruct(unsigned long arg)
{
	stlport_t	*portp;

	copy_from_user(&stl_dummyport, (void *) arg, sizeof(stlport_t));
	portp = stl_getport(stl_dummyport.brdnr, stl_dummyport.panelnr,
		 stl_dummyport.portnr);
	if (portp == (stlport_t *) NULL)
		return(-ENODEV);
	copy_to_user((void *) arg, portp, sizeof(stlport_t));
	return(0);
}

/*****************************************************************************/

/*
 *	Return the entire driver board structure to a user app.
 */

static int stl_getbrdstruct(unsigned long arg)
{
	stlbrd_t	*brdp;

	copy_from_user(&stl_dummybrd, (void *) arg, sizeof(stlbrd_t));
	if ((stl_dummybrd.brdnr < 0) || (stl_dummybrd.brdnr >= STL_MAXBRDS))
		return(-ENODEV);
	brdp = stl_brds[stl_dummybrd.brdnr];
	if (brdp == (stlbrd_t *) NULL)
		return(-ENODEV);
	copy_to_user((void *) arg, brdp, sizeof(stlbrd_t));
	return(0);
}

/*****************************************************************************/

/*
 *	The "staliomem" device is also required to do some special operations
 *	on the board and/or ports. In this driver it is mostly used for stats
 *	collection.
 */

static int stl_memioctl(struct inode *ip, struct file *fp, unsigned int cmd, unsigned long arg)
{
	int	brdnr, rc;

#if DEBUG
	printk("stl_memioctl(ip=%x,fp=%x,cmd=%x,arg=%x)\n", (int) ip,
		(int) fp, cmd, (int) arg);
#endif

	brdnr = MINOR(ip->i_rdev);
	if (brdnr >= STL_MAXBRDS)
		return(-ENODEV);
	rc = 0;

	switch (cmd) {
	case COM_GETPORTSTATS:
		if ((rc = verify_area(VERIFY_WRITE, (void *) arg,
		    sizeof(comstats_t))) == 0)
			rc = stl_getportstats((stlport_t *) NULL,
				(comstats_t *) arg);
		break;
	case COM_CLRPORTSTATS:
		if ((rc = verify_area(VERIFY_WRITE, (void *) arg,
		    sizeof(comstats_t))) == 0)
			rc = stl_clrportstats((stlport_t *) NULL,
				(comstats_t *) arg);
		break;
	case COM_GETBRDSTATS:
		if ((rc = verify_area(VERIFY_WRITE, (void *) arg,
		    sizeof(combrd_t))) == 0)
			rc = stl_getbrdstats((combrd_t *) arg);
		break;
	case COM_READPORT:
		if ((rc = verify_area(VERIFY_WRITE, (void *) arg,
		    sizeof(stlport_t))) == 0)
			rc = stl_getportstruct(arg);
		break;
	case COM_READBOARD:
		if ((rc = verify_area(VERIFY_WRITE, (void *) arg,
		    sizeof(stlbrd_t))) == 0)
			rc = stl_getbrdstruct(arg);
		break;
	default:
		rc = -ENOIOCTLCMD;
		break;
	}

	return(rc);
}

/*****************************************************************************/

int __init stl_init(void)
{
	printk(KERN_INFO "%s: version %s\n", stl_drvtitle, stl_drvversion);

	stl_initbrds();

/*
 *	Allocate a temporary write buffer.
 */
	stl_tmpwritebuf = (char *) stl_memalloc(STL_TXBUFSIZE);
	if (stl_tmpwritebuf == (char *) NULL)
		printk("STALLION: failed to allocate memory (size=%d)\n",
			STL_TXBUFSIZE);

/*
 *	Set up a character driver for per board stuff. This is mainly used
 *	to do stats ioctls on the ports.
 */
	if (devfs_register_chrdev(STL_SIOMEMMAJOR, "staliomem", &stl_fsiomem))
		printk("STALLION: failed to register serial board device\n");
	devfs_handle = devfs_mk_dir (NULL, "staliomem", NULL);
	devfs_register_series (devfs_handle, "%u", 4, DEVFS_FL_DEFAULT,
			       STL_SIOMEMMAJOR, 0,
			       S_IFCHR | S_IRUSR | S_IWUSR,
			       &stl_fsiomem, NULL);

/*
 *	Set up the tty driver structure and register us as a driver.
 *	Also setup the callout tty device.
 */
	memset(&stl_serial, 0, sizeof(struct tty_driver));
	stl_serial.magic = TTY_DRIVER_MAGIC;
	stl_serial.driver_name = stl_drvname;
	stl_serial.name = stl_serialname;
	stl_serial.major = STL_SERIALMAJOR;
	stl_serial.minor_start = 0;
	stl_serial.num = STL_MAXBRDS * STL_MAXPORTS;
	stl_serial.type = TTY_DRIVER_TYPE_SERIAL;
	stl_serial.subtype = STL_DRVTYPSERIAL;
	stl_serial.init_termios = stl_deftermios;
	stl_serial.flags = TTY_DRIVER_REAL_RAW;
	stl_serial.refcount = &stl_refcount;
	stl_serial.table = stl_ttys;
	stl_serial.termios = stl_termios;
	stl_serial.termios_locked = stl_termioslocked;
	
	stl_serial.open = stl_open;
	stl_serial.close = stl_close;
	stl_serial.write = stl_write;
	stl_serial.put_char = stl_putchar;
	stl_serial.flush_chars = stl_flushchars;
	stl_serial.write_room = stl_writeroom;
	stl_serial.chars_in_buffer = stl_charsinbuffer;
	stl_serial.ioctl = stl_ioctl;
	stl_serial.set_termios = stl_settermios;
	stl_serial.throttle = stl_throttle;
	stl_serial.unthrottle = stl_unthrottle;
	stl_serial.stop = stl_stop;
	stl_serial.start = stl_start;
	stl_serial.hangup = stl_hangup;
	stl_serial.flush_buffer = stl_flushbuffer;
	stl_serial.break_ctl = stl_breakctl;
	stl_serial.wait_until_sent = stl_waituntilsent;
	stl_serial.send_xchar = stl_sendxchar;
	stl_serial.read_proc = stl_readproc;

	stl_callout = stl_serial;
	stl_callout.name = stl_calloutname;
	stl_callout.major = STL_CALLOUTMAJOR;
	stl_callout.subtype = STL_DRVTYPCALLOUT;
	stl_callout.read_proc = 0;

	if (tty_register_driver(&stl_serial))
		printk("STALLION: failed to register serial driver\n");
	if (tty_register_driver(&stl_callout))
		printk("STALLION: failed to register callout driver\n");

	return(0);
}

/*****************************************************************************/
/*                       CD1400 HARDWARE FUNCTIONS                           */
/*****************************************************************************/

/*
 *	These functions get/set/update the registers of the cd1400 UARTs.
 *	Access to the cd1400 registers is via an address/data io port pair.
 *	(Maybe should make this inline...)
 */

static int stl_cd1400getreg(stlport_t *portp, int regnr)
{
	outb((regnr + portp->uartaddr), portp->ioaddr);
	return(inb(portp->ioaddr + EREG_DATA));
}

static void stl_cd1400setreg(stlport_t *portp, int regnr, int value)
{
	outb((regnr + portp->uartaddr), portp->ioaddr);
	outb(value, portp->ioaddr + EREG_DATA);
}

static int stl_cd1400updatereg(stlport_t *portp, int regnr, int value)
{
	outb((regnr + portp->uartaddr), portp->ioaddr);
	if (inb(portp->ioaddr + EREG_DATA) != value) {
		outb(value, portp->ioaddr + EREG_DATA);
		return(1);
	}
	return(0);
}

/*****************************************************************************/

/*
 *	Inbitialize the UARTs in a panel. We don't care what sort of board
 *	these ports are on - since the port io registers are almost
 *	identical when dealing with ports.
 */

static int stl_cd1400panelinit(stlbrd_t *brdp, stlpanel_t *panelp)
{
	unsigned int	gfrcr;
	int		chipmask, i, j;
	int		nrchips, uartaddr, ioaddr;

#if DEBUG
	printk("stl_panelinit(brdp=%x,panelp=%x)\n", (int) brdp, (int) panelp);
#endif

	BRDENABLE(panelp->brdnr, panelp->pagenr);

/*
 *	Check that each chip is present and started up OK.
 */
	chipmask = 0;
	nrchips = panelp->nrports / CD1400_PORTS;
	for (i = 0; (i < nrchips); i++) {
		if (brdp->brdtype == BRD_ECHPCI) {
			outb((panelp->pagenr + (i >> 1)), brdp->ioctrl);
			ioaddr = panelp->iobase;
		} else {
			ioaddr = panelp->iobase + (EREG_BANKSIZE * (i >> 1));
		}
		uartaddr = (i & 0x01) ? 0x080 : 0;
		outb((GFRCR + uartaddr), ioaddr);
		outb(0, (ioaddr + EREG_DATA));
		outb((CCR + uartaddr), ioaddr);
		outb(CCR_RESETFULL, (ioaddr + EREG_DATA));
		outb(CCR_RESETFULL, (ioaddr + EREG_DATA));
		outb((GFRCR + uartaddr), ioaddr);
		for (j = 0; (j < CCR_MAXWAIT); j++) {
			if ((gfrcr = inb(ioaddr + EREG_DATA)) != 0)
				break;
		}
		if ((j >= CCR_MAXWAIT) || (gfrcr < 0x40) || (gfrcr > 0x60)) {
			printk("STALLION: cd1400 not responding, "
				"brd=%d panel=%d chip=%d\n",
				panelp->brdnr, panelp->panelnr, i);
			continue;
		}
		chipmask |= (0x1 << i);
		outb((PPR + uartaddr), ioaddr);
		outb(PPR_SCALAR, (ioaddr + EREG_DATA));
	}

	BRDDISABLE(panelp->brdnr);
	return(chipmask);
}

/*****************************************************************************/

/*
 *	Initialize hardware specific port registers.
 */

static void stl_cd1400portinit(stlbrd_t *brdp, stlpanel_t *panelp, stlport_t *portp)
{
#if DEBUG
	printk("stl_cd1400portinit(brdp=%x,panelp=%x,portp=%x)\n",
		(int) brdp, (int) panelp, (int) portp);
#endif

	if ((brdp == (stlbrd_t *) NULL) || (panelp == (stlpanel_t *) NULL) ||
	    (portp == (stlport_t *) NULL))
		return;

	portp->ioaddr = panelp->iobase + (((brdp->brdtype == BRD_ECHPCI) ||
		(portp->portnr < 8)) ? 0 : EREG_BANKSIZE);
	portp->uartaddr = (portp->portnr & 0x04) << 5;
	portp->pagenr = panelp->pagenr + (portp->portnr >> 3);

	BRDENABLE(portp->brdnr, portp->pagenr);
	stl_cd1400setreg(portp, CAR, (portp->portnr & 0x03));
	stl_cd1400setreg(portp, LIVR, (portp->portnr << 3));
	portp->hwid = stl_cd1400getreg(portp, GFRCR);
	BRDDISABLE(portp->brdnr);
}

/*****************************************************************************/

/*
 *	Wait for the command register to be ready. We will poll this,
 *	since it won't usually take too long to be ready.
 */

static void stl_cd1400ccrwait(stlport_t *portp)
{
	int	i;

	for (i = 0; (i < CCR_MAXWAIT); i++) {
		if (stl_cd1400getreg(portp, CCR) == 0) {
			return;
		}
	}

	printk("STALLION: cd1400 not responding, port=%d panel=%d brd=%d\n",
		portp->portnr, portp->panelnr, portp->brdnr);
}

/*****************************************************************************/

/*
 *	Set up the cd1400 registers for a port based on the termios port
 *	settings.
 */

static void stl_cd1400setport(stlport_t *portp, struct termios *tiosp)
{
	stlbrd_t	*brdp;
	unsigned long	flags;
	unsigned int	clkdiv, baudrate;
	unsigned char	cor1, cor2, cor3;
	unsigned char	cor4, cor5, ccr;
	unsigned char	srer, sreron, sreroff;
	unsigned char	mcor1, mcor2, rtpr;
	unsigned char	clk, div;

	cor1 = 0;
	cor2 = 0;
	cor3 = 0;
	cor4 = 0;
	cor5 = 0;
	ccr = 0;
	rtpr = 0;
	clk = 0;
	div = 0;
	mcor1 = 0;
	mcor2 = 0;
	sreron = 0;
	sreroff = 0;

	brdp = stl_brds[portp->brdnr];
	if (brdp == (stlbrd_t *) NULL)
		return;

/*
 *	Set up the RX char ignore mask with those RX error types we
 *	can ignore. We can get the cd1400 to help us out a little here,
 *	it will ignore parity errors and breaks for us.
 */
	portp->rxignoremsk = 0;
	if (tiosp->c_iflag & IGNPAR) {
		portp->rxignoremsk |= (ST_PARITY | ST_FRAMING | ST_OVERRUN);
		cor1 |= COR1_PARIGNORE;
	}
	if (tiosp->c_iflag & IGNBRK) {
		portp->rxignoremsk |= ST_BREAK;
		cor4 |= COR4_IGNBRK;
	}

	portp->rxmarkmsk = ST_OVERRUN;
	if (tiosp->c_iflag & (INPCK | PARMRK))
		portp->rxmarkmsk |= (ST_PARITY | ST_FRAMING);
	if (tiosp->c_iflag & BRKINT)
		portp->rxmarkmsk |= ST_BREAK;

/*
 *	Go through the char size, parity and stop bits and set all the
 *	option register appropriately.
 */
	switch (tiosp->c_cflag & CSIZE) {
	case CS5:
		cor1 |= COR1_CHL5;
		break;
	case CS6:
		cor1 |= COR1_CHL6;
		break;
	case CS7:
		cor1 |= COR1_CHL7;
		break;
	default:
		cor1 |= COR1_CHL8;
		break;
	}

	if (tiosp->c_cflag & CSTOPB)
		cor1 |= COR1_STOP2;
	else
		cor1 |= COR1_STOP1;

	if (tiosp->c_cflag & PARENB) {
		if (tiosp->c_cflag & PARODD)
			cor1 |= (COR1_PARENB | COR1_PARODD);
		else
			cor1 |= (COR1_PARENB | COR1_PAREVEN);
	} else {
		cor1 |= COR1_PARNONE;
	}

/*
 *	Set the RX FIFO threshold at 6 chars. This gives a bit of breathing
 *	space for hardware flow control and the like. This should be set to
 *	VMIN. Also here we will set the RX data timeout to 10ms - this should
 *	really be based on VTIME.
 */
	cor3 |= FIFO_RXTHRESHOLD;
	rtpr = 2;

/*
 *	Calculate the baud rate timers. For now we will just assume that
 *	the input and output baud are the same. Could have used a baud
 *	table here, but this way we can generate virtually any baud rate
 *	we like!
 */
	baudrate = tiosp->c_cflag & CBAUD;
	if (baudrate & CBAUDEX) {
		baudrate &= ~CBAUDEX;
		if ((baudrate < 1) || (baudrate > 4))
			tiosp->c_cflag &= ~CBAUDEX;
		else
			baudrate += 15;
	}
	baudrate = stl_baudrates[baudrate];
	if ((tiosp->c_cflag & CBAUD) == B38400) {
		if ((portp->flags & ASYNC_SPD_MASK) == ASYNC_SPD_HI)
			baudrate = 57600;
		else if ((portp->flags & ASYNC_SPD_MASK) == ASYNC_SPD_VHI)
			baudrate = 115200;
		else if ((portp->flags & ASYNC_SPD_MASK) == ASYNC_SPD_SHI)
			baudrate = 230400;
		else if ((portp->flags & ASYNC_SPD_MASK) == ASYNC_SPD_WARP)
			baudrate = 460800;
		else if ((portp->flags & ASYNC_SPD_MASK) == ASYNC_SPD_CUST)
			baudrate = (portp->baud_base / portp->custom_divisor);
	}
	if (baudrate > STL_CD1400MAXBAUD)
		baudrate = STL_CD1400MAXBAUD;

	if (baudrate > 0) {
		for (clk = 0; (clk < CD1400_NUMCLKS); clk++) {
			clkdiv = ((portp->clk / stl_cd1400clkdivs[clk]) / baudrate);
			if (clkdiv < 0x100)
				break;
		}
		div = (unsigned char) clkdiv;
	}

/*
 *	Check what form of modem signaling is required and set it up.
 */
	if ((tiosp->c_cflag & CLOCAL) == 0) {
		mcor1 |= MCOR1_DCD;
		mcor2 |= MCOR2_DCD;
		sreron |= SRER_MODEM;
		portp->flags |= ASYNC_CHECK_CD;
	} else {
		portp->flags &= ~ASYNC_CHECK_CD;
	}

/*
 *	Setup cd1400 enhanced modes if we can. In particular we want to
 *	handle as much of the flow control as possible automatically. As
 *	well as saving a few CPU cycles it will also greatly improve flow
 *	control reliability.
 */
	if (tiosp->c_iflag & IXON) {
		cor2 |= COR2_TXIBE;
		cor3 |= COR3_SCD12;
		if (tiosp->c_iflag & IXANY)
			cor2 |= COR2_IXM;
	}

	if (tiosp->c_cflag & CRTSCTS) {
		cor2 |= COR2_CTSAE;
		mcor1 |= FIFO_RTSTHRESHOLD;
	}

/*
 *	All cd1400 register values calculated so go through and set
 *	them all up.
 */

#if DEBUG
	printk("SETPORT: portnr=%d panelnr=%d brdnr=%d\n",
		portp->portnr, portp->panelnr, portp->brdnr);
	printk("    cor1=%x cor2=%x cor3=%x cor4=%x cor5=%x\n",
		cor1, cor2, cor3, cor4, cor5);
	printk("    mcor1=%x mcor2=%x rtpr=%x sreron=%x sreroff=%x\n",
		mcor1, mcor2, rtpr, sreron, sreroff);
	printk("    tcor=%x tbpr=%x rcor=%x rbpr=%x\n", clk, div, clk, div);
	printk("    schr1=%x schr2=%x schr3=%x schr4=%x\n",
		tiosp->c_cc[VSTART], tiosp->c_cc[VSTOP],
		tiosp->c_cc[VSTART], tiosp->c_cc[VSTOP]);
#endif

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	stl_cd1400setreg(portp, CAR, (portp->portnr & 0x3));
	srer = stl_cd1400getreg(portp, SRER);
	stl_cd1400setreg(portp, SRER, 0);
	if (stl_cd1400updatereg(portp, COR1, cor1))
		ccr = 1;
	if (stl_cd1400updatereg(portp, COR2, cor2))
		ccr = 1;
	if (stl_cd1400updatereg(portp, COR3, cor3))
		ccr = 1;
	if (ccr) {
		stl_cd1400ccrwait(portp);
		stl_cd1400setreg(portp, CCR, CCR_CORCHANGE);
	}
	stl_cd1400setreg(portp, COR4, cor4);
	stl_cd1400setreg(portp, COR5, cor5);
	stl_cd1400setreg(portp, MCOR1, mcor1);
	stl_cd1400setreg(portp, MCOR2, mcor2);
	if (baudrate > 0) {
		stl_cd1400setreg(portp, TCOR, clk);
		stl_cd1400setreg(portp, TBPR, div);
		stl_cd1400setreg(portp, RCOR, clk);
		stl_cd1400setreg(portp, RBPR, div);
	}
	stl_cd1400setreg(portp, SCHR1, tiosp->c_cc[VSTART]);
	stl_cd1400setreg(portp, SCHR2, tiosp->c_cc[VSTOP]);
	stl_cd1400setreg(portp, SCHR3, tiosp->c_cc[VSTART]);
	stl_cd1400setreg(portp, SCHR4, tiosp->c_cc[VSTOP]);
	stl_cd1400setreg(portp, RTPR, rtpr);
	mcor1 = stl_cd1400getreg(portp, MSVR1);
	if (mcor1 & MSVR1_DCD)
		portp->sigs |= TIOCM_CD;
	else
		portp->sigs &= ~TIOCM_CD;
	stl_cd1400setreg(portp, SRER, ((srer & ~sreroff) | sreron));
	BRDDISABLE(portp->brdnr);
	restore_flags(flags);
}

/*****************************************************************************/

/*
 *	Set the state of the DTR and RTS signals.
 */

static void stl_cd1400setsignals(stlport_t *portp, int dtr, int rts)
{
	unsigned char	msvr1, msvr2;
	unsigned long	flags;

#if DEBUG
	printk("stl_cd1400setsignals(portp=%x,dtr=%d,rts=%d)\n",
		(int) portp, dtr, rts);
#endif

	msvr1 = 0;
	msvr2 = 0;
	if (dtr > 0)
		msvr1 = MSVR1_DTR;
	if (rts > 0)
		msvr2 = MSVR2_RTS;

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	stl_cd1400setreg(portp, CAR, (portp->portnr & 0x03));
	if (rts >= 0)
		stl_cd1400setreg(portp, MSVR2, msvr2);
	if (dtr >= 0)
		stl_cd1400setreg(portp, MSVR1, msvr1);
	BRDDISABLE(portp->brdnr);
	restore_flags(flags);
}

/*****************************************************************************/

/*
 *	Return the state of the signals.
 */

static int stl_cd1400getsignals(stlport_t *portp)
{
	unsigned char	msvr1, msvr2;
	unsigned long	flags;
	int		sigs;

#if DEBUG
	printk("stl_cd1400getsignals(portp=%x)\n", (int) portp);
#endif

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	stl_cd1400setreg(portp, CAR, (portp->portnr & 0x03));
	msvr1 = stl_cd1400getreg(portp, MSVR1);
	msvr2 = stl_cd1400getreg(portp, MSVR2);
	BRDDISABLE(portp->brdnr);
	restore_flags(flags);

	sigs = 0;
	sigs |= (msvr1 & MSVR1_DCD) ? TIOCM_CD : 0;
	sigs |= (msvr1 & MSVR1_CTS) ? TIOCM_CTS : 0;
	sigs |= (msvr1 & MSVR1_DTR) ? TIOCM_DTR : 0;
	sigs |= (msvr2 & MSVR2_RTS) ? TIOCM_RTS : 0;
#if 0
	sigs |= (msvr1 & MSVR1_RI) ? TIOCM_RI : 0;
	sigs |= (msvr1 & MSVR1_DSR) ? TIOCM_DSR : 0;
#else
	sigs |= TIOCM_DSR;
#endif
	return(sigs);
}

/*****************************************************************************/

/*
 *	Enable/Disable the Transmitter and/or Receiver.
 */

static void stl_cd1400enablerxtx(stlport_t *portp, int rx, int tx)
{
	unsigned char	ccr;
	unsigned long	flags;

#if DEBUG
	printk("stl_cd1400enablerxtx(portp=%x,rx=%d,tx=%d)\n",
		(int) portp, rx, tx);
#endif
	ccr = 0;

	if (tx == 0)
		ccr |= CCR_TXDISABLE;
	else if (tx > 0)
		ccr |= CCR_TXENABLE;
	if (rx == 0)
		ccr |= CCR_RXDISABLE;
	else if (rx > 0)
		ccr |= CCR_RXENABLE;

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	stl_cd1400setreg(portp, CAR, (portp->portnr & 0x03));
	stl_cd1400ccrwait(portp);
	stl_cd1400setreg(portp, CCR, ccr);
	stl_cd1400ccrwait(portp);
	BRDDISABLE(portp->brdnr);
	restore_flags(flags);
}

/*****************************************************************************/

/*
 *	Start/stop the Transmitter and/or Receiver.
 */

static void stl_cd1400startrxtx(stlport_t *portp, int rx, int tx)
{
	unsigned char	sreron, sreroff;
	unsigned long	flags;

#if DEBUG
	printk("stl_cd1400startrxtx(portp=%x,rx=%d,tx=%d)\n",
		(int) portp, rx, tx);
#endif

	sreron = 0;
	sreroff = 0;
	if (tx == 0)
		sreroff |= (SRER_TXDATA | SRER_TXEMPTY);
	else if (tx == 1)
		sreron |= SRER_TXDATA;
	else if (tx >= 2)
		sreron |= SRER_TXEMPTY;
	if (rx == 0)
		sreroff |= SRER_RXDATA;
	else if (rx > 0)
		sreron |= SRER_RXDATA;

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	stl_cd1400setreg(portp, CAR, (portp->portnr & 0x03));
	stl_cd1400setreg(portp, SRER,
		((stl_cd1400getreg(portp, SRER) & ~sreroff) | sreron));
	BRDDISABLE(portp->brdnr);
	if (tx > 0)
		set_bit(ASYI_TXBUSY, &portp->istate);
	restore_flags(flags);
}

/*****************************************************************************/

/*
 *	Disable all interrupts from this port.
 */

static void stl_cd1400disableintrs(stlport_t *portp)
{
	unsigned long	flags;

#if DEBUG
	printk("stl_cd1400disableintrs(portp=%x)\n", (int) portp);
#endif
	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	stl_cd1400setreg(portp, CAR, (portp->portnr & 0x03));
	stl_cd1400setreg(portp, SRER, 0);
	BRDDISABLE(portp->brdnr);
	restore_flags(flags);
}

/*****************************************************************************/

static void stl_cd1400sendbreak(stlport_t *portp, int len)
{
	unsigned long	flags;

#if DEBUG
	printk("stl_cd1400sendbreak(portp=%x,len=%d)\n", (int) portp, len);
#endif

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	stl_cd1400setreg(portp, CAR, (portp->portnr & 0x03));
	stl_cd1400setreg(portp, SRER,
		((stl_cd1400getreg(portp, SRER) & ~SRER_TXDATA) |
		SRER_TXEMPTY));
	BRDDISABLE(portp->brdnr);
	portp->brklen = len;
	if (len == 1)
		portp->stats.txbreaks++;
	restore_flags(flags);
}

/*****************************************************************************/

/*
 *	Take flow control actions...
 */

static void stl_cd1400flowctrl(stlport_t *portp, int state)
{
	struct tty_struct	*tty;
	unsigned long		flags;

#if DEBUG
	printk("stl_cd1400flowctrl(portp=%x,state=%x)\n", (int) portp, state);
#endif

	if (portp == (stlport_t *) NULL)
		return;
	tty = portp->tty;
	if (tty == (struct tty_struct *) NULL)
		return;

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	stl_cd1400setreg(portp, CAR, (portp->portnr & 0x03));

	if (state) {
		if (tty->termios->c_iflag & IXOFF) {
			stl_cd1400ccrwait(portp);
			stl_cd1400setreg(portp, CCR, CCR_SENDSCHR1);
			portp->stats.rxxon++;
			stl_cd1400ccrwait(portp);
		}
/*
 *		Question: should we return RTS to what it was before? It may
 *		have been set by an ioctl... Suppose not, since if you have
 *		hardware flow control set then it is pretty silly to go and
 *		set the RTS line by hand.
 */
		if (tty->termios->c_cflag & CRTSCTS) {
			stl_cd1400setreg(portp, MCOR1,
				(stl_cd1400getreg(portp, MCOR1) |
				FIFO_RTSTHRESHOLD));
			stl_cd1400setreg(portp, MSVR2, MSVR2_RTS);
			portp->stats.rxrtson++;
		}
	} else {
		if (tty->termios->c_iflag & IXOFF) {
			stl_cd1400ccrwait(portp);
			stl_cd1400setreg(portp, CCR, CCR_SENDSCHR2);
			portp->stats.rxxoff++;
			stl_cd1400ccrwait(portp);
		}
		if (tty->termios->c_cflag & CRTSCTS) {
			stl_cd1400setreg(portp, MCOR1,
				(stl_cd1400getreg(portp, MCOR1) & 0xf0));
			stl_cd1400setreg(portp, MSVR2, 0);
			portp->stats.rxrtsoff++;
		}
	}

	BRDDISABLE(portp->brdnr);
	restore_flags(flags);
}

/*****************************************************************************/

/*
 *	Send a flow control character...
 */

static void stl_cd1400sendflow(stlport_t *portp, int state)
{
	struct tty_struct	*tty;
	unsigned long		flags;

#if DEBUG
	printk("stl_cd1400sendflow(portp=%x,state=%x)\n", (int) portp, state);
#endif

	if (portp == (stlport_t *) NULL)
		return;
	tty = portp->tty;
	if (tty == (struct tty_struct *) NULL)
		return;

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	stl_cd1400setreg(portp, CAR, (portp->portnr & 0x03));
	if (state) {
		stl_cd1400ccrwait(portp);
		stl_cd1400setreg(portp, CCR, CCR_SENDSCHR1);
		portp->stats.rxxon++;
		stl_cd1400ccrwait(portp);
	} else {
		stl_cd1400ccrwait(portp);
		stl_cd1400setreg(portp, CCR, CCR_SENDSCHR2);
		portp->stats.rxxoff++;
		stl_cd1400ccrwait(portp);
	}
	BRDDISABLE(portp->brdnr);
	restore_flags(flags);
}

/*****************************************************************************/

static void stl_cd1400flush(stlport_t *portp)
{
	unsigned long	flags;

#if DEBUG
	printk("stl_cd1400flush(portp=%x)\n", (int) portp);
#endif

	if (portp == (stlport_t *) NULL)
		return;

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	stl_cd1400setreg(portp, CAR, (portp->portnr & 0x03));
	stl_cd1400ccrwait(portp);
	stl_cd1400setreg(portp, CCR, CCR_TXFLUSHFIFO);
	stl_cd1400ccrwait(portp);
	portp->tx.tail = portp->tx.head;
	BRDDISABLE(portp->brdnr);
	restore_flags(flags);
}

/*****************************************************************************/

/*
 *	Return the current state of data flow on this port. This is only
 *	really interresting when determining if data has fully completed
 *	transmission or not... This is easy for the cd1400, it accurately
 *	maintains the busy port flag.
 */

static int stl_cd1400datastate(stlport_t *portp)
{
#if DEBUG
	printk("stl_cd1400datastate(portp=%x)\n", (int) portp);
#endif

	if (portp == (stlport_t *) NULL)
		return(0);

	return(test_bit(ASYI_TXBUSY, &portp->istate) ? 1 : 0);
}

/*****************************************************************************/

/*
 *	Interrupt service routine for cd1400 EasyIO boards.
 */

static void stl_cd1400eiointr(stlpanel_t *panelp, unsigned int iobase)
{
	unsigned char	svrtype;

#if DEBUG
	printk("stl_cd1400eiointr(panelp=%x,iobase=%x)\n",
		(int) panelp, iobase);
#endif

	outb(SVRR, iobase);
	svrtype = inb(iobase + EREG_DATA);
	if (panelp->nrports > 4) {
		outb((SVRR + 0x80), iobase);
		svrtype |= inb(iobase + EREG_DATA);
	}

	if (svrtype & SVRR_RX)
		stl_cd1400rxisr(panelp, iobase);
	else if (svrtype & SVRR_TX)
		stl_cd1400txisr(panelp, iobase);
	else if (svrtype & SVRR_MDM)
		stl_cd1400mdmisr(panelp, iobase);
}

/*****************************************************************************/

/*
 *	Interrupt service routine for cd1400 panels.
 */

static void stl_cd1400echintr(stlpanel_t *panelp, unsigned int iobase)
{
	unsigned char	svrtype;

#if DEBUG
	printk("stl_cd1400echintr(panelp=%x,iobase=%x)\n", (int) panelp,
		iobase);
#endif

	outb(SVRR, iobase);
	svrtype = inb(iobase + EREG_DATA);
	outb((SVRR + 0x80), iobase);
	svrtype |= inb(iobase + EREG_DATA);
	if (svrtype & SVRR_RX)
		stl_cd1400rxisr(panelp, iobase);
	else if (svrtype & SVRR_TX)
		stl_cd1400txisr(panelp, iobase);
	else if (svrtype & SVRR_MDM)
		stl_cd1400mdmisr(panelp, iobase);
}


/*****************************************************************************/

/*
 *	Unfortunately we need to handle breaks in the TX data stream, since
 *	this is the only way to generate them on the cd1400.
 */

static inline int stl_cd1400breakisr(stlport_t *portp, int ioaddr)
{
	if (portp->brklen == 1) {
		outb((COR2 + portp->uartaddr), ioaddr);
		outb((inb(ioaddr + EREG_DATA) | COR2_ETC),
			(ioaddr + EREG_DATA));
		outb((TDR + portp->uartaddr), ioaddr);
		outb(ETC_CMD, (ioaddr + EREG_DATA));
		outb(ETC_STARTBREAK, (ioaddr + EREG_DATA));
		outb((SRER + portp->uartaddr), ioaddr);
		outb((inb(ioaddr + EREG_DATA) & ~(SRER_TXDATA | SRER_TXEMPTY)),
			(ioaddr + EREG_DATA));
		return(1);
	} else if (portp->brklen > 1) {
		outb((TDR + portp->uartaddr), ioaddr);
		outb(ETC_CMD, (ioaddr + EREG_DATA));
		outb(ETC_STOPBREAK, (ioaddr + EREG_DATA));
		portp->brklen = -1;
		return(1);
	} else {
		outb((COR2 + portp->uartaddr), ioaddr);
		outb((inb(ioaddr + EREG_DATA) & ~COR2_ETC),
			(ioaddr + EREG_DATA));
		portp->brklen = 0;
	}
	return(0);
}

/*****************************************************************************/

/*
 *	Transmit interrupt handler. This has gotta be fast!  Handling TX
 *	chars is pretty simple, stuff as many as possible from the TX buffer
 *	into the cd1400 FIFO. Must also handle TX breaks here, since they
 *	are embedded as commands in the data stream. Oh no, had to use a goto!
 *	This could be optimized more, will do when I get time...
 *	In practice it is possible that interrupts are enabled but that the
 *	port has been hung up. Need to handle not having any TX buffer here,
 *	this is done by using the side effect that head and tail will also
 *	be NULL if the buffer has been freed.
 */

static void stl_cd1400txisr(stlpanel_t *panelp, int ioaddr)
{
	stlport_t	*portp;
	int		len, stlen;
	char		*head, *tail;
	unsigned char	ioack, srer;

#if DEBUG
	printk("stl_cd1400txisr(panelp=%x,ioaddr=%x)\n", (int) panelp, ioaddr);
#endif

	ioack = inb(ioaddr + EREG_TXACK);
	if (((ioack & panelp->ackmask) != 0) ||
	    ((ioack & ACK_TYPMASK) != ACK_TYPTX)) {
		printk("STALLION: bad TX interrupt ack value=%x\n", ioack);
		return;
	}
	portp = panelp->ports[(ioack >> 3)];

/*
 *	Unfortunately we need to handle breaks in the data stream, since
 *	this is the only way to generate them on the cd1400. Do it now if
 *	a break is to be sent.
 */
	if (portp->brklen != 0)
		if (stl_cd1400breakisr(portp, ioaddr))
			goto stl_txalldone;

	head = portp->tx.head;
	tail = portp->tx.tail;
	len = (head >= tail) ? (head - tail) : (STL_TXBUFSIZE - (tail - head));
	if ((len == 0) || ((len < STL_TXBUFLOW) &&
	    (test_bit(ASYI_TXLOW, &portp->istate) == 0))) {
		set_bit(ASYI_TXLOW, &portp->istate);
		MOD_INC_USE_COUNT;
		if (schedule_task(&portp->tqueue) == 0)
			MOD_DEC_USE_COUNT;
	}

	if (len == 0) {
		outb((SRER + portp->uartaddr), ioaddr);
		srer = inb(ioaddr + EREG_DATA);
		if (srer & SRER_TXDATA) {
			srer = (srer & ~SRER_TXDATA) | SRER_TXEMPTY;
		} else {
			srer &= ~(SRER_TXDATA | SRER_TXEMPTY);
			clear_bit(ASYI_TXBUSY, &portp->istate);
		}
		outb(srer, (ioaddr + EREG_DATA));
	} else {
		len = MIN(len, CD1400_TXFIFOSIZE);
		portp->stats.txtotal += len;
		stlen = MIN(len, ((portp->tx.buf + STL_TXBUFSIZE) - tail));
		outb((TDR + portp->uartaddr), ioaddr);
		outsb((ioaddr + EREG_DATA), tail, stlen);
		len -= stlen;
		tail += stlen;
		if (tail >= (portp->tx.buf + STL_TXBUFSIZE))
			tail = portp->tx.buf;
		if (len > 0) {
			outsb((ioaddr + EREG_DATA), tail, len);
			tail += len;
		}
		portp->tx.tail = tail;
	}

stl_txalldone:
	outb((EOSRR + portp->uartaddr), ioaddr);
	outb(0, (ioaddr + EREG_DATA));
}

/*****************************************************************************/

/*
 *	Receive character interrupt handler. Determine if we have good chars
 *	or bad chars and then process appropriately. Good chars are easy
 *	just shove the lot into the RX buffer and set all status byte to 0.
 *	If a bad RX char then process as required. This routine needs to be
 *	fast!  In practice it is possible that we get an interrupt on a port
 *	that is closed. This can happen on hangups - since they completely
 *	shutdown a port not in user context. Need to handle this case.
 */

static void stl_cd1400rxisr(stlpanel_t *panelp, int ioaddr)
{
	stlport_t		*portp;
	struct tty_struct	*tty;
	unsigned int		ioack, len, buflen;
	unsigned char		status;
	char			ch;

#if DEBUG
	printk("stl_cd1400rxisr(panelp=%x,ioaddr=%x)\n", (int) panelp, ioaddr);
#endif

	ioack = inb(ioaddr + EREG_RXACK);
	if ((ioack & panelp->ackmask) != 0) {
		printk("STALLION: bad RX interrupt ack value=%x\n", ioack);
		return;
	}
	portp = panelp->ports[(ioack >> 3)];
	tty = portp->tty;

	if ((ioack & ACK_TYPMASK) == ACK_TYPRXGOOD) {
		outb((RDCR + portp->uartaddr), ioaddr);
		len = inb(ioaddr + EREG_DATA);
		if ((tty == (struct tty_struct *) NULL) ||
		    (tty->flip.char_buf_ptr == (char *) NULL) ||
		    ((buflen = TTY_FLIPBUF_SIZE - tty->flip.count) == 0)) {
			len = MIN(len, sizeof(stl_unwanted));
			outb((RDSR + portp->uartaddr), ioaddr);
			insb((ioaddr + EREG_DATA), &stl_unwanted[0], len);
			portp->stats.rxlost += len;
			portp->stats.rxtotal += len;
		} else {
			len = MIN(len, buflen);
			if (len > 0) {
				outb((RDSR + portp->uartaddr), ioaddr);
				insb((ioaddr + EREG_DATA), tty->flip.char_buf_ptr, len);
				memset(tty->flip.flag_buf_ptr, 0, len);
				tty->flip.flag_buf_ptr += len;
				tty->flip.char_buf_ptr += len;
				tty->flip.count += len;
				tty_schedule_flip(tty);
				portp->stats.rxtotal += len;
			}
		}
	} else if ((ioack & ACK_TYPMASK) == ACK_TYPRXBAD) {
		outb((RDSR + portp->uartaddr), ioaddr);
		status = inb(ioaddr + EREG_DATA);
		ch = inb(ioaddr + EREG_DATA);
		if (status & ST_PARITY)
			portp->stats.rxparity++;
		if (status & ST_FRAMING)
			portp->stats.rxframing++;
		if (status & ST_OVERRUN)
			portp->stats.rxoverrun++;
		if (status & ST_BREAK)
			portp->stats.rxbreaks++;
		if (status & ST_SCHARMASK) {
			if ((status & ST_SCHARMASK) == ST_SCHAR1)
				portp->stats.txxon++;
			if ((status & ST_SCHARMASK) == ST_SCHAR2)
				portp->stats.txxoff++;
			goto stl_rxalldone;
		}
		if ((tty != (struct tty_struct *) NULL) &&
		    ((portp->rxignoremsk & status) == 0)) {
			if (portp->rxmarkmsk & status) {
				if (status & ST_BREAK) {
					status = TTY_BREAK;
					if (portp->flags & ASYNC_SAK) {
						do_SAK(tty);
						BRDENABLE(portp->brdnr, portp->pagenr);
					}
				} else if (status & ST_PARITY) {
					status = TTY_PARITY;
				} else if (status & ST_FRAMING) {
					status = TTY_FRAME;
				} else if(status & ST_OVERRUN) {
					status = TTY_OVERRUN;
				} else {
					status = 0;
				}
			} else {
				status = 0;
			}
			if (tty->flip.char_buf_ptr != (char *) NULL) {
				if (tty->flip.count < TTY_FLIPBUF_SIZE) {
					*tty->flip.flag_buf_ptr++ = status;
					*tty->flip.char_buf_ptr++ = ch;
					tty->flip.count++;
				}
				tty_schedule_flip(tty);
			}
		}
	} else {
		printk("STALLION: bad RX interrupt ack value=%x\n", ioack);
		return;
	}

stl_rxalldone:
	outb((EOSRR + portp->uartaddr), ioaddr);
	outb(0, (ioaddr + EREG_DATA));
}

/*****************************************************************************/

/*
 *	Modem interrupt handler. The is called when the modem signal line
 *	(DCD) has changed state. Leave most of the work to the off-level
 *	processing routine.
 */

static void stl_cd1400mdmisr(stlpanel_t *panelp, int ioaddr)
{
	stlport_t	*portp;
	unsigned int	ioack;
	unsigned char	misr;

#if DEBUG
	printk("stl_cd1400mdmisr(panelp=%x)\n", (int) panelp);
#endif

	ioack = inb(ioaddr + EREG_MDACK);
	if (((ioack & panelp->ackmask) != 0) ||
	    ((ioack & ACK_TYPMASK) != ACK_TYPMDM)) {
		printk("STALLION: bad MODEM interrupt ack value=%x\n", ioack);
		return;
	}
	portp = panelp->ports[(ioack >> 3)];

	outb((MISR + portp->uartaddr), ioaddr);
	misr = inb(ioaddr + EREG_DATA);
	if (misr & MISR_DCD) {
		set_bit(ASYI_DCDCHANGE, &portp->istate);
		MOD_INC_USE_COUNT;
		if (schedule_task(&portp->tqueue) == 0)
			MOD_DEC_USE_COUNT;
		portp->stats.modem++;
	}

	outb((EOSRR + portp->uartaddr), ioaddr);
	outb(0, (ioaddr + EREG_DATA));
}

/*****************************************************************************/
/*                      SC26198 HARDWARE FUNCTIONS                           */
/*****************************************************************************/

/*
 *	These functions get/set/update the registers of the sc26198 UARTs.
 *	Access to the sc26198 registers is via an address/data io port pair.
 *	(Maybe should make this inline...)
 */

static int stl_sc26198getreg(stlport_t *portp, int regnr)
{
	outb((regnr | portp->uartaddr), (portp->ioaddr + XP_ADDR));
	return(inb(portp->ioaddr + XP_DATA));
}

static void stl_sc26198setreg(stlport_t *portp, int regnr, int value)
{
	outb((regnr | portp->uartaddr), (portp->ioaddr + XP_ADDR));
	outb(value, (portp->ioaddr + XP_DATA));
}

static int stl_sc26198updatereg(stlport_t *portp, int regnr, int value)
{
	outb((regnr | portp->uartaddr), (portp->ioaddr + XP_ADDR));
	if (inb(portp->ioaddr + XP_DATA) != value) {
		outb(value, (portp->ioaddr + XP_DATA));
		return(1);
	}
	return(0);
}

/*****************************************************************************/

/*
 *	Functions to get and set the sc26198 global registers.
 */

static int stl_sc26198getglobreg(stlport_t *portp, int regnr)
{
	outb(regnr, (portp->ioaddr + XP_ADDR));
	return(inb(portp->ioaddr + XP_DATA));
}

#if 0
static void stl_sc26198setglobreg(stlport_t *portp, int regnr, int value)
{
	outb(regnr, (portp->ioaddr + XP_ADDR));
	outb(value, (portp->ioaddr + XP_DATA));
}
#endif

/*****************************************************************************/

/*
 *	Inbitialize the UARTs in a panel. We don't care what sort of board
 *	these ports are on - since the port io registers are almost
 *	identical when dealing with ports.
 */

static int stl_sc26198panelinit(stlbrd_t *brdp, stlpanel_t *panelp)
{
	int	chipmask, i;
	int	nrchips, ioaddr;

#if DEBUG
	printk("stl_sc26198panelinit(brdp=%x,panelp=%x)\n",
		(int) brdp, (int) panelp);
#endif

	BRDENABLE(panelp->brdnr, panelp->pagenr);

/*
 *	Check that each chip is present and started up OK.
 */
	chipmask = 0;
	nrchips = (panelp->nrports + 4) / SC26198_PORTS;
	if (brdp->brdtype == BRD_ECHPCI)
		outb(panelp->pagenr, brdp->ioctrl);

	for (i = 0; (i < nrchips); i++) {
		ioaddr = panelp->iobase + (i * 4); 
		outb(SCCR, (ioaddr + XP_ADDR));
		outb(CR_RESETALL, (ioaddr + XP_DATA));
		outb(TSTR, (ioaddr + XP_ADDR));
		if (inb(ioaddr + XP_DATA) != 0) {
			printk("STALLION: sc26198 not responding, "
				"brd=%d panel=%d chip=%d\n",
				panelp->brdnr, panelp->panelnr, i);
			continue;
		}
		chipmask |= (0x1 << i);
		outb(GCCR, (ioaddr + XP_ADDR));
		outb(GCCR_IVRTYPCHANACK, (ioaddr + XP_DATA));
		outb(WDTRCR, (ioaddr + XP_ADDR));
		outb(0xff, (ioaddr + XP_DATA));
	}

	BRDDISABLE(panelp->brdnr);
	return(chipmask);
}

/*****************************************************************************/

/*
 *	Initialize hardware specific port registers.
 */

static void stl_sc26198portinit(stlbrd_t *brdp, stlpanel_t *panelp, stlport_t *portp)
{
#if DEBUG
	printk("stl_sc26198portinit(brdp=%x,panelp=%x,portp=%x)\n",
		(int) brdp, (int) panelp, (int) portp);
#endif

	if ((brdp == (stlbrd_t *) NULL) || (panelp == (stlpanel_t *) NULL) ||
	    (portp == (stlport_t *) NULL))
		return;

	portp->ioaddr = panelp->iobase + ((portp->portnr < 8) ? 0 : 4);
	portp->uartaddr = (portp->portnr & 0x07) << 4;
	portp->pagenr = panelp->pagenr;
	portp->hwid = 0x1;

	BRDENABLE(portp->brdnr, portp->pagenr);
	stl_sc26198setreg(portp, IOPCR, IOPCR_SETSIGS);
	BRDDISABLE(portp->brdnr);
}

/*****************************************************************************/

/*
 *	Set up the sc26198 registers for a port based on the termios port
 *	settings.
 */

static void stl_sc26198setport(stlport_t *portp, struct termios *tiosp)
{
	stlbrd_t	*brdp;
	unsigned long	flags;
	unsigned int	baudrate;
	unsigned char	mr0, mr1, mr2, clk;
	unsigned char	imron, imroff, iopr, ipr;

	mr0 = 0;
	mr1 = 0;
	mr2 = 0;
	clk = 0;
	iopr = 0;
	imron = 0;
	imroff = 0;

	brdp = stl_brds[portp->brdnr];
	if (brdp == (stlbrd_t *) NULL)
		return;

/*
 *	Set up the RX char ignore mask with those RX error types we
 *	can ignore.
 */
	portp->rxignoremsk = 0;
	if (tiosp->c_iflag & IGNPAR)
		portp->rxignoremsk |= (SR_RXPARITY | SR_RXFRAMING |
			SR_RXOVERRUN);
	if (tiosp->c_iflag & IGNBRK)
		portp->rxignoremsk |= SR_RXBREAK;

	portp->rxmarkmsk = SR_RXOVERRUN;
	if (tiosp->c_iflag & (INPCK | PARMRK))
		portp->rxmarkmsk |= (SR_RXPARITY | SR_RXFRAMING);
	if (tiosp->c_iflag & BRKINT)
		portp->rxmarkmsk |= SR_RXBREAK;

/*
 *	Go through the char size, parity and stop bits and set all the
 *	option register appropriately.
 */
	switch (tiosp->c_cflag & CSIZE) {
	case CS5:
		mr1 |= MR1_CS5;
		break;
	case CS6:
		mr1 |= MR1_CS6;
		break;
	case CS7:
		mr1 |= MR1_CS7;
		break;
	default:
		mr1 |= MR1_CS8;
		break;
	}

	if (tiosp->c_cflag & CSTOPB)
		mr2 |= MR2_STOP2;
	else
		mr2 |= MR2_STOP1;

	if (tiosp->c_cflag & PARENB) {
		if (tiosp->c_cflag & PARODD)
			mr1 |= (MR1_PARENB | MR1_PARODD);
		else
			mr1 |= (MR1_PARENB | MR1_PAREVEN);
	} else {
		mr1 |= MR1_PARNONE;
	}

	mr1 |= MR1_ERRBLOCK;

/*
 *	Set the RX FIFO threshold at 8 chars. This gives a bit of breathing
 *	space for hardware flow control and the like. This should be set to
 *	VMIN.
 */
	mr2 |= MR2_RXFIFOHALF;

/*
 *	Calculate the baud rate timers. For now we will just assume that
 *	the input and output baud are the same. The sc26198 has a fixed
 *	baud rate table, so only discrete baud rates possible.
 */
	baudrate = tiosp->c_cflag & CBAUD;
	if (baudrate & CBAUDEX) {
		baudrate &= ~CBAUDEX;
		if ((baudrate < 1) || (baudrate > 4))
			tiosp->c_cflag &= ~CBAUDEX;
		else
			baudrate += 15;
	}
	baudrate = stl_baudrates[baudrate];
	if ((tiosp->c_cflag & CBAUD) == B38400) {
		if ((portp->flags & ASYNC_SPD_MASK) == ASYNC_SPD_HI)
			baudrate = 57600;
		else if ((portp->flags & ASYNC_SPD_MASK) == ASYNC_SPD_VHI)
			baudrate = 115200;
		else if ((portp->flags & ASYNC_SPD_MASK) == ASYNC_SPD_SHI)
			baudrate = 230400;
		else if ((portp->flags & ASYNC_SPD_MASK) == ASYNC_SPD_WARP)
			baudrate = 460800;
		else if ((portp->flags & ASYNC_SPD_MASK) == ASYNC_SPD_CUST)
			baudrate = (portp->baud_base / portp->custom_divisor);
	}
	if (baudrate > STL_SC26198MAXBAUD)
		baudrate = STL_SC26198MAXBAUD;

	if (baudrate > 0) {
		for (clk = 0; (clk < SC26198_NRBAUDS); clk++) {
			if (baudrate <= sc26198_baudtable[clk])
				break;
		}
	}

/*
 *	Check what form of modem signaling is required and set it up.
 */
	if (tiosp->c_cflag & CLOCAL) {
		portp->flags &= ~ASYNC_CHECK_CD;
	} else {
		iopr |= IOPR_DCDCOS;
		imron |= IR_IOPORT;
		portp->flags |= ASYNC_CHECK_CD;
	}

/*
 *	Setup sc26198 enhanced modes if we can. In particular we want to
 *	handle as much of the flow control as possible automatically. As
 *	well as saving a few CPU cycles it will also greatly improve flow
 *	control reliability.
 */
	if (tiosp->c_iflag & IXON) {
		mr0 |= MR0_SWFTX | MR0_SWFT;
		imron |= IR_XONXOFF;
	} else {
		imroff |= IR_XONXOFF;
	}
	if (tiosp->c_iflag & IXOFF)
		mr0 |= MR0_SWFRX;

	if (tiosp->c_cflag & CRTSCTS) {
		mr2 |= MR2_AUTOCTS;
		mr1 |= MR1_AUTORTS;
	}

/*
 *	All sc26198 register values calculated so go through and set
 *	them all up.
 */

#if DEBUG
	printk("SETPORT: portnr=%d panelnr=%d brdnr=%d\n",
		portp->portnr, portp->panelnr, portp->brdnr);
	printk("    mr0=%x mr1=%x mr2=%x clk=%x\n", mr0, mr1, mr2, clk);
	printk("    iopr=%x imron=%x imroff=%x\n", iopr, imron, imroff);
	printk("    schr1=%x schr2=%x schr3=%x schr4=%x\n",
		tiosp->c_cc[VSTART], tiosp->c_cc[VSTOP],
		tiosp->c_cc[VSTART], tiosp->c_cc[VSTOP]);
#endif

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	stl_sc26198setreg(portp, IMR, 0);
	stl_sc26198updatereg(portp, MR0, mr0);
	stl_sc26198updatereg(portp, MR1, mr1);
	stl_sc26198setreg(portp, SCCR, CR_RXERRBLOCK);
	stl_sc26198updatereg(portp, MR2, mr2);
	stl_sc26198updatereg(portp, IOPIOR,
		((stl_sc26198getreg(portp, IOPIOR) & ~IPR_CHANGEMASK) | iopr));

	if (baudrate > 0) {
		stl_sc26198setreg(portp, TXCSR, clk);
		stl_sc26198setreg(portp, RXCSR, clk);
	}

	stl_sc26198setreg(portp, XONCR, tiosp->c_cc[VSTART]);
	stl_sc26198setreg(portp, XOFFCR, tiosp->c_cc[VSTOP]);

	ipr = stl_sc26198getreg(portp, IPR);
	if (ipr & IPR_DCD)
		portp->sigs &= ~TIOCM_CD;
	else
		portp->sigs |= TIOCM_CD;

	portp->imr = (portp->imr & ~imroff) | imron;
	stl_sc26198setreg(portp, IMR, portp->imr);
	BRDDISABLE(portp->brdnr);
	restore_flags(flags);
}

/*****************************************************************************/

/*
 *	Set the state of the DTR and RTS signals.
 */

static void stl_sc26198setsignals(stlport_t *portp, int dtr, int rts)
{
	unsigned char	iopioron, iopioroff;
	unsigned long	flags;

#if DEBUG
	printk("stl_sc26198setsignals(portp=%x,dtr=%d,rts=%d)\n",
		(int) portp, dtr, rts);
#endif

	iopioron = 0;
	iopioroff = 0;
	if (dtr == 0)
		iopioroff |= IPR_DTR;
	else if (dtr > 0)
		iopioron |= IPR_DTR;
	if (rts == 0)
		iopioroff |= IPR_RTS;
	else if (rts > 0)
		iopioron |= IPR_RTS;

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	stl_sc26198setreg(portp, IOPIOR,
		((stl_sc26198getreg(portp, IOPIOR) & ~iopioroff) | iopioron));
	BRDDISABLE(portp->brdnr);
	restore_flags(flags);
}

/*****************************************************************************/

/*
 *	Return the state of the signals.
 */

static int stl_sc26198getsignals(stlport_t *portp)
{
	unsigned char	ipr;
	unsigned long	flags;
	int		sigs;

#if DEBUG
	printk("stl_sc26198getsignals(portp=%x)\n", (int) portp);
#endif

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	ipr = stl_sc26198getreg(portp, IPR);
	BRDDISABLE(portp->brdnr);
	restore_flags(flags);

	sigs = 0;
	sigs |= (ipr & IPR_DCD) ? 0 : TIOCM_CD;
	sigs |= (ipr & IPR_CTS) ? 0 : TIOCM_CTS;
	sigs |= (ipr & IPR_DTR) ? 0: TIOCM_DTR;
	sigs |= (ipr & IPR_RTS) ? 0: TIOCM_RTS;
	sigs |= TIOCM_DSR;
	return(sigs);
}

/*****************************************************************************/

/*
 *	Enable/Disable the Transmitter and/or Receiver.
 */

static void stl_sc26198enablerxtx(stlport_t *portp, int rx, int tx)
{
	unsigned char	ccr;
	unsigned long	flags;

#if DEBUG
	printk("stl_sc26198enablerxtx(portp=%x,rx=%d,tx=%d)\n",
		(int) portp, rx, tx);
#endif

	ccr = portp->crenable;
	if (tx == 0)
		ccr &= ~CR_TXENABLE;
	else if (tx > 0)
		ccr |= CR_TXENABLE;
	if (rx == 0)
		ccr &= ~CR_RXENABLE;
	else if (rx > 0)
		ccr |= CR_RXENABLE;

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	stl_sc26198setreg(portp, SCCR, ccr);
	BRDDISABLE(portp->brdnr);
	portp->crenable = ccr;
	restore_flags(flags);
}

/*****************************************************************************/

/*
 *	Start/stop the Transmitter and/or Receiver.
 */

static void stl_sc26198startrxtx(stlport_t *portp, int rx, int tx)
{
	unsigned char	imr;
	unsigned long	flags;

#if DEBUG
	printk("stl_sc26198startrxtx(portp=%x,rx=%d,tx=%d)\n",
		(int) portp, rx, tx);
#endif

	imr = portp->imr;
	if (tx == 0)
		imr &= ~IR_TXRDY;
	else if (tx == 1)
		imr |= IR_TXRDY;
	if (rx == 0)
		imr &= ~(IR_RXRDY | IR_RXBREAK | IR_RXWATCHDOG);
	else if (rx > 0)
		imr |= IR_RXRDY | IR_RXBREAK | IR_RXWATCHDOG;

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	stl_sc26198setreg(portp, IMR, imr);
	BRDDISABLE(portp->brdnr);
	portp->imr = imr;
	if (tx > 0)
		set_bit(ASYI_TXBUSY, &portp->istate);
	restore_flags(flags);
}

/*****************************************************************************/

/*
 *	Disable all interrupts from this port.
 */

static void stl_sc26198disableintrs(stlport_t *portp)
{
	unsigned long	flags;

#if DEBUG
	printk("stl_sc26198disableintrs(portp=%x)\n", (int) portp);
#endif

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	portp->imr = 0;
	stl_sc26198setreg(portp, IMR, 0);
	BRDDISABLE(portp->brdnr);
	restore_flags(flags);
}

/*****************************************************************************/

static void stl_sc26198sendbreak(stlport_t *portp, int len)
{
	unsigned long	flags;

#if DEBUG
	printk("stl_sc26198sendbreak(portp=%x,len=%d)\n", (int) portp, len);
#endif

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	if (len == 1) {
		stl_sc26198setreg(portp, SCCR, CR_TXSTARTBREAK);
		portp->stats.txbreaks++;
	} else {
		stl_sc26198setreg(portp, SCCR, CR_TXSTOPBREAK);
	}
	BRDDISABLE(portp->brdnr);
	restore_flags(flags);
}

/*****************************************************************************/

/*
 *	Take flow control actions...
 */

static void stl_sc26198flowctrl(stlport_t *portp, int state)
{
	struct tty_struct	*tty;
	unsigned long		flags;
	unsigned char		mr0;

#if DEBUG
	printk("stl_sc26198flowctrl(portp=%x,state=%x)\n", (int) portp, state);
#endif

	if (portp == (stlport_t *) NULL)
		return;
	tty = portp->tty;
	if (tty == (struct tty_struct *) NULL)
		return;

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);

	if (state) {
		if (tty->termios->c_iflag & IXOFF) {
			mr0 = stl_sc26198getreg(portp, MR0);
			stl_sc26198setreg(portp, MR0, (mr0 & ~MR0_SWFRXTX));
			stl_sc26198setreg(portp, SCCR, CR_TXSENDXON);
			mr0 |= MR0_SWFRX;
			portp->stats.rxxon++;
			stl_sc26198wait(portp);
			stl_sc26198setreg(portp, MR0, mr0);
		}
/*
 *		Question: should we return RTS to what it was before? It may
 *		have been set by an ioctl... Suppose not, since if you have
 *		hardware flow control set then it is pretty silly to go and
 *		set the RTS line by hand.
 */
		if (tty->termios->c_cflag & CRTSCTS) {
			stl_sc26198setreg(portp, MR1,
				(stl_sc26198getreg(portp, MR1) | MR1_AUTORTS));
			stl_sc26198setreg(portp, IOPIOR,
				(stl_sc26198getreg(portp, IOPIOR) | IOPR_RTS));
			portp->stats.rxrtson++;
		}
	} else {
		if (tty->termios->c_iflag & IXOFF) {
			mr0 = stl_sc26198getreg(portp, MR0);
			stl_sc26198setreg(portp, MR0, (mr0 & ~MR0_SWFRXTX));
			stl_sc26198setreg(portp, SCCR, CR_TXSENDXOFF);
			mr0 &= ~MR0_SWFRX;
			portp->stats.rxxoff++;
			stl_sc26198wait(portp);
			stl_sc26198setreg(portp, MR0, mr0);
		}
		if (tty->termios->c_cflag & CRTSCTS) {
			stl_sc26198setreg(portp, MR1,
				(stl_sc26198getreg(portp, MR1) & ~MR1_AUTORTS));
			stl_sc26198setreg(portp, IOPIOR,
				(stl_sc26198getreg(portp, IOPIOR) & ~IOPR_RTS));
			portp->stats.rxrtsoff++;
		}
	}

	BRDDISABLE(portp->brdnr);
	restore_flags(flags);
}

/*****************************************************************************/

/*
 *	Send a flow control character.
 */

static void stl_sc26198sendflow(stlport_t *portp, int state)
{
	struct tty_struct	*tty;
	unsigned long		flags;
	unsigned char		mr0;

#if DEBUG
	printk("stl_sc26198sendflow(portp=%x,state=%x)\n", (int) portp, state);
#endif

	if (portp == (stlport_t *) NULL)
		return;
	tty = portp->tty;
	if (tty == (struct tty_struct *) NULL)
		return;

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	if (state) {
		mr0 = stl_sc26198getreg(portp, MR0);
		stl_sc26198setreg(portp, MR0, (mr0 & ~MR0_SWFRXTX));
		stl_sc26198setreg(portp, SCCR, CR_TXSENDXON);
		mr0 |= MR0_SWFRX;
		portp->stats.rxxon++;
		stl_sc26198wait(portp);
		stl_sc26198setreg(portp, MR0, mr0);
	} else {
		mr0 = stl_sc26198getreg(portp, MR0);
		stl_sc26198setreg(portp, MR0, (mr0 & ~MR0_SWFRXTX));
		stl_sc26198setreg(portp, SCCR, CR_TXSENDXOFF);
		mr0 &= ~MR0_SWFRX;
		portp->stats.rxxoff++;
		stl_sc26198wait(portp);
		stl_sc26198setreg(portp, MR0, mr0);
	}
	BRDDISABLE(portp->brdnr);
	restore_flags(flags);
}

/*****************************************************************************/

static void stl_sc26198flush(stlport_t *portp)
{
	unsigned long	flags;

#if DEBUG
	printk("stl_sc26198flush(portp=%x)\n", (int) portp);
#endif

	if (portp == (stlport_t *) NULL)
		return;

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	stl_sc26198setreg(portp, SCCR, CR_TXRESET);
	stl_sc26198setreg(portp, SCCR, portp->crenable);
	BRDDISABLE(portp->brdnr);
	portp->tx.tail = portp->tx.head;
	restore_flags(flags);
}

/*****************************************************************************/

/*
 *	Return the current state of data flow on this port. This is only
 *	really interresting when determining if data has fully completed
 *	transmission or not... The sc26198 interrupt scheme cannot
 *	determine when all data has actually drained, so we need to
 *	check the port statusy register to be sure.
 */

static int stl_sc26198datastate(stlport_t *portp)
{
	unsigned long	flags;
	unsigned char	sr;

#if DEBUG
	printk("stl_sc26198datastate(portp=%x)\n", (int) portp);
#endif

	if (portp == (stlport_t *) NULL)
		return(0);
	if (test_bit(ASYI_TXBUSY, &portp->istate))
		return(1);

	save_flags(flags);
	cli();
	BRDENABLE(portp->brdnr, portp->pagenr);
	sr = stl_sc26198getreg(portp, SR);
	BRDDISABLE(portp->brdnr);
	restore_flags(flags);

	return((sr & SR_TXEMPTY) ? 0 : 1);
}

/*****************************************************************************/

/*
 *	Delay for a small amount of time, to give the sc26198 a chance
 *	to process a command...
 */

static void stl_sc26198wait(stlport_t *portp)
{
	int	i;

#if DEBUG
	printk("stl_sc26198wait(portp=%x)\n", (int) portp);
#endif

	if (portp == (stlport_t *) NULL)
		return;

	for (i = 0; (i < 20); i++)
		stl_sc26198getglobreg(portp, TSTR);
}

/*****************************************************************************/

/*
 *	If we are TX flow controlled and in IXANY mode then we may
 *	need to unflow control here. We gotta do this because of the
 *	automatic flow control modes of the sc26198.
 */

static inline void stl_sc26198txunflow(stlport_t *portp, struct tty_struct *tty)
{
	unsigned char	mr0;

	mr0 = stl_sc26198getreg(portp, MR0);
	stl_sc26198setreg(portp, MR0, (mr0 & ~MR0_SWFRXTX));
	stl_sc26198setreg(portp, SCCR, CR_HOSTXON);
	stl_sc26198wait(portp);
	stl_sc26198setreg(portp, MR0, mr0);
	clear_bit(ASYI_TXFLOWED, &portp->istate);
}

/*****************************************************************************/

/*
 *	Interrupt service routine for sc26198 panels.
 */

static void stl_sc26198intr(stlpanel_t *panelp, unsigned int iobase)
{
	stlport_t	*portp;
	unsigned int	iack;

/* 
 *	Work around bug in sc26198 chip... Cannot have A6 address
 *	line of UART high, else iack will be returned as 0.
 */
	outb(0, (iobase + 1));

	iack = inb(iobase + XP_IACK);
	portp = panelp->ports[(iack & IVR_CHANMASK) + ((iobase & 0x4) << 1)];

	if (iack & IVR_RXDATA)
		stl_sc26198rxisr(portp, iack);
	else if (iack & IVR_TXDATA)
		stl_sc26198txisr(portp);
	else
		stl_sc26198otherisr(portp, iack);
}

/*****************************************************************************/

/*
 *	Transmit interrupt handler. This has gotta be fast!  Handling TX
 *	chars is pretty simple, stuff as many as possible from the TX buffer
 *	into the sc26198 FIFO.
 *	In practice it is possible that interrupts are enabled but that the
 *	port has been hung up. Need to handle not having any TX buffer here,
 *	this is done by using the side effect that head and tail will also
 *	be NULL if the buffer has been freed.
 */

static void stl_sc26198txisr(stlport_t *portp)
{
	unsigned int	ioaddr;
	unsigned char	mr0;
	int		len, stlen;
	char		*head, *tail;

#if DEBUG
	printk("stl_sc26198txisr(portp=%x)\n", (int) portp);
#endif

	ioaddr = portp->ioaddr;
	head = portp->tx.head;
	tail = portp->tx.tail;
	len = (head >= tail) ? (head - tail) : (STL_TXBUFSIZE - (tail - head));
	if ((len == 0) || ((len < STL_TXBUFLOW) &&
	    (test_bit(ASYI_TXLOW, &portp->istate) == 0))) {
		set_bit(ASYI_TXLOW, &portp->istate);
		MOD_INC_USE_COUNT;
		if (schedule_task(&portp->tqueue) == 0)
			MOD_DEC_USE_COUNT;
	}

	if (len == 0) {
		outb((MR0 | portp->uartaddr), (ioaddr + XP_ADDR));
		mr0 = inb(ioaddr + XP_DATA);
		if ((mr0 & MR0_TXMASK) == MR0_TXEMPTY) {
			portp->imr &= ~IR_TXRDY;
			outb((IMR | portp->uartaddr), (ioaddr + XP_ADDR));
			outb(portp->imr, (ioaddr + XP_DATA));
			clear_bit(ASYI_TXBUSY, &portp->istate);
		} else {
			mr0 |= ((mr0 & ~MR0_TXMASK) | MR0_TXEMPTY);
			outb(mr0, (ioaddr + XP_DATA));
		}
	} else {
		len = MIN(len, SC26198_TXFIFOSIZE);
		portp->stats.txtotal += len;
		stlen = MIN(len, ((portp->tx.buf + STL_TXBUFSIZE) - tail));
		outb(GTXFIFO, (ioaddr + XP_ADDR));
		outsb((ioaddr + XP_DATA), tail, stlen);
		len -= stlen;
		tail += stlen;
		if (tail >= (portp->tx.buf + STL_TXBUFSIZE))
			tail = portp->tx.buf;
		if (len > 0) {
			outsb((ioaddr + XP_DATA), tail, len);
			tail += len;
		}
		portp->tx.tail = tail;
	}
}

/*****************************************************************************/

/*
 *	Receive character interrupt handler. Determine if we have good chars
 *	or bad chars and then process appropriately. Good chars are easy
 *	just shove the lot into the RX buffer and set all status byte to 0.
 *	If a bad RX char then process as required. This routine needs to be
 *	fast!  In practice it is possible that we get an interrupt on a port
 *	that is closed. This can happen on hangups - since they completely
 *	shutdown a port not in user context. Need to handle this case.
 */

static void stl_sc26198rxisr(stlport_t *portp, unsigned int iack)
{
	struct tty_struct	*tty;
	unsigned int		len, buflen, ioaddr;

#if DEBUG
	printk("stl_sc26198rxisr(portp=%x,iack=%x)\n", (int) portp, iack);
#endif

	tty = portp->tty;
	ioaddr = portp->ioaddr;
	outb(GIBCR, (ioaddr + XP_ADDR));
	len = inb(ioaddr + XP_DATA) + 1;

	if ((iack & IVR_TYPEMASK) == IVR_RXDATA) {
		if ((tty == (struct tty_struct *) NULL) ||
		    (tty->flip.char_buf_ptr == (char *) NULL) ||
		    ((buflen = TTY_FLIPBUF_SIZE - tty->flip.count) == 0)) {
			len = MIN(len, sizeof(stl_unwanted));
			outb(GRXFIFO, (ioaddr + XP_ADDR));
			insb((ioaddr + XP_DATA), &stl_unwanted[0], len);
			portp->stats.rxlost += len;
			portp->stats.rxtotal += len;
		} else {
			len = MIN(len, buflen);
			if (len > 0) {
				outb(GRXFIFO, (ioaddr + XP_ADDR));
				insb((ioaddr + XP_DATA), tty->flip.char_buf_ptr, len);
				memset(tty->flip.flag_buf_ptr, 0, len);
				tty->flip.flag_buf_ptr += len;
				tty->flip.char_buf_ptr += len;
				tty->flip.count += len;
				tty_schedule_flip(tty);
				portp->stats.rxtotal += len;
			}
		}
	} else {
		stl_sc26198rxbadchars(portp);
	}

/*
 *	If we are TX flow controlled and in IXANY mode then we may need
 *	to unflow control here. We gotta do this because of the automatic
 *	flow control modes of the sc26198.
 */
	if (test_bit(ASYI_TXFLOWED, &portp->istate)) {
		if ((tty != (struct tty_struct *) NULL) &&
		    (tty->termios != (struct termios *) NULL) &&
		    (tty->termios->c_iflag & IXANY)) {
			stl_sc26198txunflow(portp, tty);
		}
	}
}

/*****************************************************************************/

/*
 *	Process an RX bad character.
 */

static void inline stl_sc26198rxbadch(stlport_t *portp, unsigned char status, char ch)
{
	struct tty_struct	*tty;
	unsigned int		ioaddr;

	tty = portp->tty;
	ioaddr = portp->ioaddr;

	if (status & SR_RXPARITY)
		portp->stats.rxparity++;
	if (status & SR_RXFRAMING)
		portp->stats.rxframing++;
	if (status & SR_RXOVERRUN)
		portp->stats.rxoverrun++;
	if (status & SR_RXBREAK)
		portp->stats.rxbreaks++;

	if ((tty != (struct tty_struct *) NULL) &&
	    ((portp->rxignoremsk & status) == 0)) {
		if (portp->rxmarkmsk & status) {
			if (status & SR_RXBREAK) {
				status = TTY_BREAK;
				if (portp->flags & ASYNC_SAK) {
					do_SAK(tty);
					BRDENABLE(portp->brdnr, portp->pagenr);
				}
			} else if (status & SR_RXPARITY) {
				status = TTY_PARITY;
			} else if (status & SR_RXFRAMING) {
				status = TTY_FRAME;
			} else if(status & SR_RXOVERRUN) {
				status = TTY_OVERRUN;
			} else {
				status = 0;
			}
		} else {
			status = 0;
		}

		if (tty->flip.char_buf_ptr != (char *) NULL) {
			if (tty->flip.count < TTY_FLIPBUF_SIZE) {
				*tty->flip.flag_buf_ptr++ = status;
				*tty->flip.char_buf_ptr++ = ch;
				tty->flip.count++;
			}
			tty_schedule_flip(tty);
		}

		if (status == 0)
			portp->stats.rxtotal++;
	}
}

/*****************************************************************************/

/*
 *	Process all characters in the RX FIFO of the UART. Check all char
 *	status bytes as well, and process as required. We need to check
 *	all bytes in the FIFO, in case some more enter the FIFO while we
 *	are here. To get the exact character error type we need to switch
 *	into CHAR error mode (that is why we need to make sure we empty
 *	the FIFO).
 */

static void stl_sc26198rxbadchars(stlport_t *portp)
{
	unsigned char	status, mr1;
	char		ch;

/*
 *	To get the precise error type for each character we must switch
 *	back into CHAR error mode.
 */
	mr1 = stl_sc26198getreg(portp, MR1);
	stl_sc26198setreg(portp, MR1, (mr1 & ~MR1_ERRBLOCK));

	while ((status = stl_sc26198getreg(portp, SR)) & SR_RXRDY) {
		stl_sc26198setreg(portp, SCCR, CR_CLEARRXERR);
		ch = stl_sc26198getreg(portp, RXFIFO);
		stl_sc26198rxbadch(portp, status, ch);
	}

/*
 *	To get correct interrupt class we must switch back into BLOCK
 *	error mode.
 */
	stl_sc26198setreg(portp, MR1, mr1);
}

/*****************************************************************************/

/*
 *	Other interrupt handler. This includes modem signals, flow
 *	control actions, etc. Most stuff is left to off-level interrupt
 *	processing time.
 */

static void stl_sc26198otherisr(stlport_t *portp, unsigned int iack)
{
	unsigned char	cir, ipr, xisr;

#if DEBUG
	printk("stl_sc26198otherisr(portp=%x,iack=%x)\n", (int) portp, iack);
#endif

	cir = stl_sc26198getglobreg(portp, CIR);

	switch (cir & CIR_SUBTYPEMASK) {
	case CIR_SUBCOS:
		ipr = stl_sc26198getreg(portp, IPR);
		if (ipr & IPR_DCDCHANGE) {
			set_bit(ASYI_DCDCHANGE, &portp->istate);
			MOD_INC_USE_COUNT;
			if (schedule_task(&portp->tqueue) == 0)
				MOD_DEC_USE_COUNT;
			portp->stats.modem++;
		}
		break;
	case CIR_SUBXONXOFF:
		xisr = stl_sc26198getreg(portp, XISR);
		if (xisr & XISR_RXXONGOT) {
			set_bit(ASYI_TXFLOWED, &portp->istate);
			portp->stats.txxoff++;
		}
		if (xisr & XISR_RXXOFFGOT) {
			clear_bit(ASYI_TXFLOWED, &portp->istate);
			portp->stats.txxon++;
		}
		break;
	case CIR_SUBBREAK:
		stl_sc26198setreg(portp, SCCR, CR_BREAKRESET);
		stl_sc26198rxbadchars(portp);
		break;
	default:
		break;
	}
}

/*****************************************************************************/
