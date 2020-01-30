
/* sx.c -- driver for the Specialix SX series cards. 
 *
 *  This driver will also support the older SI, and XIO cards.
 *
 *
 *   (C) 1998 - 2000  R.E.Wolff@BitWizard.nl
 *
 *  Simon Allen (simonallen@cix.compulink.co.uk) wrote a previous
 *  version of this driver. Some fragments may have been copied. (none
 *  yet :-)
 *
 * Specialix pays for the development and support of this driver.
 * Please DO contact support@specialix.co.uk if you require
 * support. But please read the documentation (sx.txt) first.
 *
 *
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License as
 *      published by the Free Software Foundation; either version 2 of
 *      the License, or (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be
 *      useful, but WITHOUT ANY WARRANTY; without even the implied
 *      warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *      PURPOSE.  See the GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public
 *      License along with this program; if not, write to the Free
 *      Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139,
 *      USA.
 *
 * Revision history:
 * $Log: sx.c,v $
 * Revision 1.33  2000/03/09 10:00:00  pvdl,wolff
 * - Fixed module and port counting
 * - Fixed signal handling
 * - Fixed an Ooops
 * 
 * Revision 1.32  2000/03/07 09:00:00  wolff,pvdl
 * - Fixed some sx_dprintk typos
 * - added detection for an invalid board/module configuration
 *
 * Revision 1.31  2000/03/06 12:00:00  wolff,pvdl
 * - Added support for EISA
 *
 * Revision 1.30  2000/01/21 17:43:06  wolff
 * - Added support for SX+
 *
 * Revision 1.26  1999/08/05 15:22:14  wolff
 * - Port to 2.3.x
 * - Reformatted to Linus' liking.
 *
 * Revision 1.25  1999/07/30 14:24:08  wolff
 * Had accidentally left "gs_debug" set to "-1" instead of "off" (=0).
 *
 * Revision 1.24  1999/07/28 09:41:52  wolff
 * - I noticed the remark about use-count straying in sx.txt. I checked
 *   sx_open, and found a few places where that could happen. I hope it's
 *   fixed now.
 *
 * Revision 1.23  1999/07/28 08:56:06  wolff
 * - Fixed crash when sx_firmware run twice.
 * - Added sx_slowpoll as a module parameter (I guess nobody really wanted
 *   to change it from the default... )
 * - Fixed a stupid editing problem I introduced in 1.22.
 * - Fixed dropping characters on a termios change.
 *
 * Revision 1.22  1999/07/26 21:01:43  wolff
 * Russell Brown noticed that I had overlooked 4 out of six modem control
 * signals in sx_getsignals. Ooops.
 *
 * Revision 1.21  1999/07/23 09:11:33  wolff
 * I forgot to free dynamically allocated memory when the driver is unloaded.
 *
 * Revision 1.20  1999/07/20 06:25:26  wolff
 * The "closing wait" wasn't honoured. Thanks to James Griffiths for
 * reporting this.
 *
 * Revision 1.19  1999/07/11 08:59:59  wolff
 * Fixed an oops in close, when an open was pending. Changed the memtest
 * a bit. Should also test the board in word-mode, however my card fails the
 * memtest then. I still have to figure out what is wrong...
 *
 * Revision 1.18  1999/06/10 09:38:42  wolff
 * Changed the format of the firmware revision from %04x to %x.%02x .
 *
 * Revision 1.17  1999/06/04 09:44:35  wolff
 * fixed problem: reference to pci stuff when config_pci was off...
 * Thanks to Jorge Novo for noticing this.
 *
 * Revision 1.16  1999/06/02 08:30:15  wolff
 * added/removed the workaround for the DCD bug in the Firmware.
 * A bit more debugging code to locate that...
 *
 * Revision 1.15  1999/06/01 11:35:30  wolff
 * when DCD is left low (floating?), on TA's the firmware first tells us
 * that DCD is high, but after a short while suddenly comes to the
 * conclusion that it is low. All this would be fine, if it weren't that
 * Unix requires us to send a "hangup" signal in that case. This usually
 * all happens BEFORE the program has had a chance to ioctl the device
 * into clocal mode..
 *
 * Revision 1.14  1999/05/25 11:18:59  wolff
 * Added PCI-fix.
 * Added checks for return code of sx_sendcommand.
 * Don't issue "reconfig" if port isn't open yet. (bit us on TA modules...)
 *
 * Revision 1.13  1999/04/29 15:18:01  wolff
 * Fixed an "oops" that showed on SuSE 6.0 systems.
 * Activate DTR again after stty 0.
 *
 * Revision 1.12  1999/04/29 07:49:52  wolff
 * Improved "stty 0" handling a bit. (used to change baud to 9600 assuming
 *     the connection would be dropped anyway. That is not always the case,
 *     and confuses people).
 * Told the card to always monitor the modem signals.
 * Added support for dynamic  gs_debug adjustments.
 * Now tells the rest of the system the number of ports.
 *
 * Revision 1.11  1999/04/24 11:11:30  wolff
 * Fixed two stupid typos in the memory test.
 *
 * Revision 1.10  1999/04/24 10:53:39  wolff
 * Added some of Christian's suggestions.
 * Fixed an HW_COOK_IN bug (ISIG was not in I_OTHER. We used to trust the
 * card to send the signal to the process.....)
 *
 * Revision 1.9  1999/04/23 07:26:38  wolff
 * Included Christian Lademann's 2.0 compile-warning fixes and interrupt
 *    assignment redesign.
 * Cleanup of some other stuff.
 *
 * Revision 1.8  1999/04/16 13:05:30  wolff
 * fixed a DCD change unnoticed bug.
 *
 * Revision 1.7  1999/04/14 22:19:51  wolff
 * Fixed typo that showed up in 2.0.x builds (get_user instead of Get_user!)
 *
 * Revision 1.6  1999/04/13 18:40:20  wolff
 * changed misc-minor to 161, as assigned by HPA.
 *
 * Revision 1.5  1999/04/13 15:12:25  wolff
 * Fixed use-count leak when "hangup" occurred.
 * Added workaround for a stupid-PCIBIOS bug.
 *
 *
 * Revision 1.4  1999/04/01 22:47:40  wolff
 * Fixed < 1M linux-2.0 problem.
 * (vremap isn't compatible with ioremap in that case)
 *
 * Revision 1.3  1999/03/31 13:45:45  wolff
 * Firmware loading is now done through a separate IOCTL.
 *
 * Revision 1.2  1999/03/28 12:22:29  wolff
 * rcs cleanup
 *
 * Revision 1.1  1999/03/28 12:10:34  wolff
 * Readying for release on 2.0.x (sorry David, 1.01 becomes 1.1 for RCS). 
 *
 * Revision 0.12  1999/03/28 09:20:10  wolff
 * Fixed problem in 0.11, continueing cleanup.
 *
 * Revision 0.11  1999/03/28 08:46:44  wolff
 * cleanup. Not good.
 *
 * Revision 0.10  1999/03/28 08:09:43  wolff
 * Fixed loosing characters on close.
 *
 * Revision 0.9  1999/03/21 22:52:01  wolff
 * Ported back to 2.2.... (minor things)
 *
 * Revision 0.8  1999/03/21 22:40:33  wolff
 * Port to 2.0
 *
 * Revision 0.7  1999/03/21 19:06:34  wolff
 * Fixed hangup processing.
 *
 * Revision 0.6  1999/02/05 08:45:14  wolff
 * fixed real_raw problems. Inclusion into kernel imminent.
 *
 * Revision 0.5  1998/12/21 23:51:06  wolff
 * Snatched a nasty bug: sx_transmit_chars was getting re-entered, and it
 * shouldn't have. THATs why I want to have transmit interrupts even when
 * the buffer is empty.
 *
 * Revision 0.4  1998/12/17 09:34:46  wolff
 * PPP works. ioctl works. Basically works!
 *
 * Revision 0.3  1998/12/15 13:05:18  wolff
 * It works! Wow! Gotta start implementing IOCTL and stuff....
 *
 * Revision 0.2  1998/12/01 08:33:53  wolff
 * moved over to 2.1.130
 *
 * Revision 0.1  1998/11/03 21:23:51  wolff
 * Initial revision. Detects SX card.
 *
 * */


#define RCS_ID "$Id: sx.c,v 1.33 2000/03/08 10:01:02 wolff, pvdl Exp $"
#define RCS_REV "$Revision: 1.33 $"


#include <linux/module.h>
#include <linux/config.h> 
#include <linux/kdev_t.h>
#include <asm/io.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/mm.h>
#include <linux/serial.h>
#include <linux/fcntl.h>
#include <linux/major.h>
#include <linux/delay.h>
#include <linux/tqueue.h>
#include <linux/version.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/miscdevice.h>

/* The 3.0.0 version of sxboards/sxwindow.h  uses BYTE and WORD.... */
#define BYTE u8
#define WORD u16

/* .... but the 3.0.4 version uses _u8 and _u16. */
#define _u8 u8
#define _u16 u16

#include "sxboards.h"
#include "sxwindow.h"

#include <linux/compatmac.h>
#include <linux/generic_serial.h>
#include "sx.h"


/* I don't think that this driver can handle more than 256 ports on
   one machine. You'll have to increase the number of boards in sx.h
   if you want more than 4 boards.  */


/* Why the hell am I defining these here? */
#define SX_TYPE_NORMAL 1
#define SX_TYPE_CALLOUT 2


#ifndef PCI_DEVICE_ID_SPECIALIX_SX_XIO_IO8
#define PCI_DEVICE_ID_SPECIALIX_SX_XIO_IO8 0x2000
#endif


/* Configurable options: 
   (Don't be too sure that it'll work if you toggle them) */

/* Am I paranoid or not ? ;-) */
#undef SX_PARANOIA_CHECK


/* 20 -> 2000 per second. The card should rate-limit interrupts at 100
   Hz, but it is user configurable. I don't recommend going above 1000
   Hz. The interrupt ratelimit might trigger if the interrupt is
   shared with a very active other device. */
#define IRQ_RATE_LIMIT 20

/* Sharing interrupts is possible now. If the other device wants more
   than 2000 interrupts per second, we'd gracefully decline further
   interrupts. That's not what we want. On the other hand, if the
   other device interrupts 2000 times a second, don't use the SX
   interrupt. Use polling. */
#undef IRQ_RATE_LIMIT


#if 0
/* Not implemented */
/* 
 * The following defines are mostly for testing purposes. But if you need
 * some nice reporting in your syslog, you can define them also.
 */
#define SX_REPORT_FIFO
#define SX_REPORT_OVERRUN
#endif 


/* Function prototypes */
static void sx_disable_tx_interrupts (void * ptr); 
static void sx_enable_tx_interrupts (void * ptr); 
static void sx_disable_rx_interrupts (void * ptr); 
static void sx_enable_rx_interrupts (void * ptr); 
static int  sx_get_CD (void * ptr); 
static void sx_shutdown_port (void * ptr);
static int  sx_set_real_termios (void  *ptr);
static void sx_hungup (void  *ptr);
static void sx_close (void  *ptr);
static int sx_chars_in_buffer (void * ptr);
static int sx_init_board (struct sx_board *board);
static int sx_init_portstructs (int nboards, int nports);
static int sx_fw_ioctl (struct inode *inode, struct file *filp,
                        unsigned int cmd, unsigned long arg);
static int sx_init_drivers(void);


static struct tty_driver sx_driver, sx_callout_driver;

static struct tty_struct * sx_table[SX_NPORTS];
static struct termios ** sx_termios;
static struct termios ** sx_termios_locked;

static struct sx_board boards[SX_NBOARDS];
static struct sx_port *sx_ports;
static int sx_refcount;
static int sx_initialized;
static int sx_nports;
static int sx_debug;


/* You can have the driver poll your card. 
    - Set sx_poll to 1 to poll every timer tick (10ms on Intel). 
      This is used when the card cannot use an interrupt for some reason.

    - set sx_slowpoll to 100 to do an extra poll once a second (on Intel). If 
      the driver misses an interrupt (report this if it DOES happen to you!)
      everything will continue to work.... 
 */
static int sx_poll = 1;
static int sx_slowpoll;

/* The card limits the number of interrupts per second. 
   At 115k2 "100" should be sufficient. 
   If you're using higher baudrates, you can increase this...
 */

static int sx_maxints = 100;

/* These are the only open spaces in my computer. Yours may have more
   or less.... -- REW 
   duh: Card at 0xa0000 is possible on HP Netserver?? -- pvdl
*/
static int sx_probe_addrs[]= {0xc0000, 0xd0000, 0xe0000, 
                              0xc8000, 0xd8000, 0xe8000};
static int si_probe_addrs[]= {0xc0000, 0xd0000, 0xe0000, 
                              0xc8000, 0xd8000, 0xe8000, 0xa0000};

#define NR_SX_ADDRS (sizeof(sx_probe_addrs)/sizeof (int))
#define NR_SI_ADDRS (sizeof(si_probe_addrs)/sizeof (int))


/* Set the mask to all-ones. This alas, only supports 32 interrupts. 
   Some architectures may need more. */
static int sx_irqmask = -1;

MODULE_PARM(sx_probe_addrs, "i");
MODULE_PARM(si_probe_addrs, "i");
MODULE_PARM(sx_poll, "i");
MODULE_PARM(sx_slowpoll, "i");
MODULE_PARM(sx_maxints, "i");
MODULE_PARM(sx_debug, "i");
MODULE_PARM(sx_irqmask, "i");

static struct real_driver sx_real_driver = {
	sx_disable_tx_interrupts,
	sx_enable_tx_interrupts,
	sx_disable_rx_interrupts,
	sx_enable_rx_interrupts,
	sx_get_CD,
	sx_shutdown_port, 
	sx_set_real_termios, 
	sx_chars_in_buffer,
	sx_close,
	sx_hungup,
};


/* 
   This driver can spew a whole lot of debugging output at you. If you
   need maximum performance, you should disable the DEBUG define. To
   aid in debugging in the field, I'm leaving the compile-time debug
   features enabled, and disable them "runtime". That allows me to
   instruct people with problems to enable debugging without requiring
   them to recompile... 
*/
#define DEBUG


#ifdef DEBUG
#define sx_dprintk(f, str...) if (sx_debug & f) printk (str)
#else
#define sx_dprintk(f, str...) /* nothing */
#endif



#define func_enter() sx_dprintk (SX_DEBUG_FLOW, "sx: enter " __FUNCTION__ "\n")
#define func_exit()  sx_dprintk (SX_DEBUG_FLOW, "sx: exit  " __FUNCTION__ "\n")

#define func_enter2() sx_dprintk (SX_DEBUG_FLOW, "sx: enter " __FUNCTION__ \
                                  "(port %d)\n", port->line)




/* 
 *  Firmware loader driver specific routines
 *
 */

static struct file_operations sx_fw_fops = {
	owner:		THIS_MODULE,
	ioctl:		sx_fw_ioctl,
};

static struct miscdevice sx_fw_device = {
	SXCTL_MISC_MINOR, "sxctl", &sx_fw_fops
};





#ifdef SX_PARANOIA_CHECK

/* This doesn't work. Who's paranoid around here? Not me! */

static inline int sx_paranoia_check(struct sx_port const * port,
				    kdev_t device, const char *routine)
{

	static const char *badmagic =
	  KERN_ERR "sx: Warning: bad sx port magic number for device %s in %s\n";
	static const char *badinfo =
	  KERN_ERR "sx: Warning: null sx port for device %s in %s\n";
 
	if (!port) {
		printk(badinfo, kdevname(device), routine);
		return 1;
	}
	if (port->magic != SX_MAGIC) {
		printk(badmagic, kdevname(device), routine);
		return 1;
	}

	return 0;
}
#else
#define sx_paranoia_check(a,b,c) 0
#endif

/* The timeouts. First try 30 times as fast as possible. Then give
   the card some time to breathe between accesses. (Otherwise the
   processor on the card might not be able to access its OWN bus... */

#define TIMEOUT_1 30
#define TIMEOUT_2 1000000


#ifdef DEBUG
static void my_hd (unsigned char *addr, int len)
{
	int i, j, ch;

	for (i=0;i<len;i+=16) {
		printk ("%08x ", (int) addr+i);
		for (j=0;j<16;j++) {
			printk ("%02x %s", addr[j+i], (j==7)?" ":"");
		}
		for (j=0;j<16;j++) {
			ch = addr[j+i];
			printk ("%c", (ch < 0x20)?'.':((ch > 0x7f)?'.':ch));
		}
		printk ("\n");
	}
}
#endif



/* This needs redoing for Alpha -- REW -- Done. */

static inline void write_sx_byte (struct sx_board *board, int offset, u8 byte)
{
	writeb (byte, board->base+offset);
}

static inline u8 read_sx_byte (struct sx_board *board, int offset)
{
	return readb (board->base+offset);
}


static inline void write_sx_word (struct sx_board *board, int offset, u16 word)
{
	writew (word, board->base+offset);
}

static inline u16 read_sx_word (struct sx_board *board, int offset)
{
	return readw (board->base + offset);
}


static int sx_busy_wait_eq (struct sx_board *board, 
                     	    int offset, int mask, int correctval)
{
	int i;

	func_enter ();

	for (i=0; i < TIMEOUT_1 > 0;i++) 
		if ((read_sx_byte (board, offset) & mask) == correctval) {
			func_exit ();
			return 1;
		}

	for (i=0; i < TIMEOUT_2 > 0;i++) {
		if ((read_sx_byte (board, offset) & mask) == correctval) {
			func_exit ();
			return 1;
		}
		udelay (1);
	}

	func_exit ();
	return 0;
}


static int sx_busy_wait_neq (struct sx_board *board, 
                      	     int offset, int mask, int badval)
{
	int i;

	func_enter ();

	for (i=0; i < TIMEOUT_1 > 0;i++) 
		if ((read_sx_byte (board, offset) & mask) != badval) {
			func_exit ();
			return 1;
		}

	for (i=0; i < TIMEOUT_2 > 0;i++) {
		if ((read_sx_byte (board, offset) & mask) != badval) {
			func_exit ();
			return 1;
		}
		udelay (1);
	}

	func_exit ();
	return 0;
}



/* 5.6.4 of 6210028 r2.3 */
static int sx_reset (struct sx_board *board)
{
	func_enter ();

	if (IS_SX_BOARD (board)) {

		write_sx_byte (board, SX_CONFIG, 0);
		write_sx_byte (board, SX_RESET, 1); /* Value doesn't matter */

		if (!sx_busy_wait_eq (board, SX_RESET_STATUS, 1, 0)) {
			printk (KERN_INFO "sx: Card doesn't respond to reset....\n");
			return 0;
		}
	} else if (IS_EISA_BOARD(board)) {
		outb(board->irq<<4, board->eisa_base+0xc02);
	} else {
		/* Gory details of the SI/ISA board */
		write_sx_byte (board, SI2_ISA_RESET,    SI2_ISA_RESET_SET);
		write_sx_byte (board, SI2_ISA_IRQ11,    SI2_ISA_IRQ11_CLEAR);
		write_sx_byte (board, SI2_ISA_IRQ12,    SI2_ISA_IRQ12_CLEAR);
		write_sx_byte (board, SI2_ISA_IRQ15,    SI2_ISA_IRQ15_CLEAR);
		write_sx_byte (board, SI2_ISA_INTCLEAR, SI2_ISA_INTCLEAR_CLEAR);
		write_sx_byte (board, SI2_ISA_IRQSET,   SI2_ISA_IRQSET_CLEAR);
	}

	func_exit ();
	return 1;
}


/* This doesn't work on machines where "NULL" isn't 0 */
/* If you have one of those, someone will need to write 
   the equivalent of this, which will amount to about 3 lines. I don't
   want to complicate this right now. -- REW
   (See, I do write comments every now and then :-) */
#define OFFSETOF(strct, elem) ((long)&(((struct strct *)NULL)->elem))


#define CHAN_OFFSET(port,elem) (port->ch_base + OFFSETOF (_SXCHANNEL, elem))
#define MODU_OFFSET(board,addr,elem)    (addr + OFFSETOF (_SXMODULE, elem))
#define  BRD_OFFSET(board,elem)                (OFFSETOF (_SXCARD, elem))


#define sx_write_channel_byte(port, elem, val) \
   write_sx_byte (port->board, CHAN_OFFSET (port, elem), val)

#define sx_read_channel_byte(port, elem) \
   read_sx_byte (port->board, CHAN_OFFSET (port, elem))

#define sx_write_channel_word(port, elem, val) \
   write_sx_word (port->board, CHAN_OFFSET (port, elem), val)

#define sx_read_channel_word(port, elem) \
   read_sx_word (port->board, CHAN_OFFSET (port, elem))


#define sx_write_module_byte(board, addr, elem, val) \
   write_sx_byte (board, MODU_OFFSET (board, addr, elem), val)

#define sx_read_module_byte(board, addr, elem) \
   read_sx_byte (board, MODU_OFFSET (board, addr, elem))

#define sx_write_module_word(board, addr, elem, val) \
   write_sx_word (board, MODU_OFFSET (board, addr, elem), val)

#define sx_read_module_word(board, addr, elem) \
   read_sx_word (board, MODU_OFFSET (board, addr, elem))


#define sx_write_board_byte(board, elem, val) \
   write_sx_byte (board, BRD_OFFSET (board, elem), val)

#define sx_read_board_byte(board, elem) \
   read_sx_byte (board, BRD_OFFSET (board, elem))

#define sx_write_board_word(board, elem, val) \
   write_sx_word (board, BRD_OFFSET (board, elem), val)

#define sx_read_board_word(board, elem) \
   read_sx_word (board, BRD_OFFSET (board, elem))


static int sx_start_board (struct sx_board *board)
{
	if (IS_SX_BOARD (board)) {
		write_sx_byte (board, SX_CONFIG, SX_CONF_BUSEN);
	} else if (IS_EISA_BOARD(board)) {
		write_sx_byte(board, SI2_EISA_OFF, SI2_EISA_VAL);
		outb((board->irq<<4)|4, board->eisa_base+0xc02);
	} else {
		/* Don't bug me about the clear_set. 
		   I haven't the foggiest idea what it's about -- REW */
		write_sx_byte (board, SI2_ISA_RESET,    SI2_ISA_RESET_CLEAR);
		write_sx_byte (board, SI2_ISA_INTCLEAR, SI2_ISA_INTCLEAR_SET);
	}
	return 1;
}

#define SX_IRQ_REG_VAL(board) \
        ((board->flags & SX_ISA_BOARD)?(board->irq << 4):0)

/* Note. The SX register is write-only. Therefore, we have to enable the
   bus too. This is a no-op, if you don't mess with this driver... */
static int sx_start_interrupts (struct sx_board *board)
{

	/* Don't call this with board->irq == 0 */

	if (IS_SX_BOARD(board)) {
		write_sx_byte (board, SX_CONFIG, SX_IRQ_REG_VAL (board) | 
		                                 SX_CONF_BUSEN | 
		                                 SX_CONF_HOSTIRQ);
	} else if (IS_EISA_BOARD(board)) {
		inb(board->eisa_base+0xc03);  
	} else {
		switch (board->irq) {
		case 11:write_sx_byte (board, SI2_ISA_IRQ11, SI2_ISA_IRQ11_SET);break;
		case 12:write_sx_byte (board, SI2_ISA_IRQ12, SI2_ISA_IRQ12_SET);break;
		case 15:write_sx_byte (board, SI2_ISA_IRQ15, SI2_ISA_IRQ15_SET);break;
		default:printk (KERN_INFO "sx: SI/XIO card doesn't support interrupt %d.\n", 
		                board->irq);
		return 0;
		}
		write_sx_byte (board, SI2_ISA_INTCLEAR, SI2_ISA_INTCLEAR_SET);
	}

	return 1;
}


static int sx_send_command (struct sx_port *port, 
                     	    int command, int mask, int newstat)
{
	func_enter2 ();
	write_sx_byte (port->board, CHAN_OFFSET (port, hi_hstat), command);
	func_exit ();
	return sx_busy_wait_eq (port->board, CHAN_OFFSET (port, hi_hstat), mask, newstat);
}


static char *mod_type_s (int module_type)
{
	switch (module_type) {
	case TA4:       return "TA4";
	case TA8:       return "TA8";
	case TA4_ASIC:  return "TA4_ASIC";
	case TA8_ASIC:  return "TA8_ASIC";
	case MTA_CD1400:return "MTA_CD1400";
	case SXDC:      return "SXDC";
	default:return "Unknown/invalid";
	}
}


static char *pan_type_s (int pan_type)
{
	switch (pan_type) {
	case MOD_RS232DB25:     return "MOD_RS232DB25";
	case MOD_RS232RJ45:     return "MOD_RS232RJ45";
	case MOD_RS422DB25:     return "MOD_RS422DB25";
	case MOD_PARALLEL:      return "MOD_PARALLEL";
	case MOD_2_RS232DB25:   return "MOD_2_RS232DB25";
	case MOD_2_RS232RJ45:   return "MOD_2_RS232RJ45";
	case MOD_2_RS422DB25:   return "MOD_2_RS422DB25";
	case MOD_RS232DB25MALE: return "MOD_RS232DB25MALE";
	case MOD_2_PARALLEL:    return "MOD_2_PARALLEL";
	case MOD_BLANK:         return "empty";
	default:return "invalid";
	}
}


static int mod_compat_type (int module_type)
{
	return module_type >> 4;
}

static void sx_reconfigure_port(struct sx_port *port)
{
	if (sx_read_channel_byte (port, hi_hstat) == HS_IDLE_OPEN) {
		if (sx_send_command (port, HS_CONFIG, -1, HS_IDLE_OPEN) != 1) {
			printk (KERN_WARNING "sx: Sent reconfigure command, but card didn't react.\n");
		}
	} else {
		sx_dprintk (SX_DEBUG_TERMIOS, 
		            "sx: Not sending reconfigure: port isn't open (%02x).\n", 
		            sx_read_channel_byte (port, hi_hstat));
	}	
}

static void sx_setsignals (struct sx_port *port, int dtr, int rts)
{
	int t;
	func_enter2 ();

	t = sx_read_channel_byte (port, hi_op);
	if (dtr >= 0) t = dtr? (t | OP_DTR): (t & ~OP_DTR);
	if (rts >= 0) t = rts? (t | OP_RTS): (t & ~OP_RTS);
	sx_write_channel_byte (port, hi_op, t);
	sx_dprintk (SX_DEBUG_MODEMSIGNALS, "setsignals: %d/%d\n", dtr, rts);

	func_exit ();
}



static int sx_getsignals (struct sx_port *port)
{
	int i_stat,o_stat;

	o_stat = sx_read_channel_byte (port, hi_op);
	i_stat = sx_read_channel_byte (port, hi_ip);

	sx_dprintk (SX_DEBUG_MODEMSIGNALS, "getsignals: %d/%d  (%d/%d) %02x/%02x\n",
	            (o_stat & OP_DTR) != 0, (o_stat & OP_RTS) != 0,
	            port->c_dcd, sx_get_CD (port),
	            sx_read_channel_byte (port, hi_ip),
	            sx_read_channel_byte (port, hi_state));

	return (((o_stat & OP_DTR)?TIOCM_DTR:0) |
	        ((o_stat & OP_RTS)?TIOCM_RTS:0) |
	        ((i_stat & IP_CTS)?TIOCM_CTS:0) |
	        ((i_stat & IP_DCD)?TIOCM_CAR:0) |
	        ((i_stat & IP_DSR)?TIOCM_DSR:0) |
	        ((i_stat & IP_RI)?TIOCM_RNG:0)
	        );
}


static void sx_set_baud (struct sx_port *port)
{
	int t;

	if (port->board->ta_type == MOD_SXDC) {
		switch (port->gs.baud) {
		  /* Save some typing work... */
#define e(x) case x:t= BAUD_ ## x ; break
			e(50);e(75);e(110);e(150);e(200);e(300);e(600);
                        e(1200);e(1800);e(2000);e(2400);e(4800);e(7200);
                        e(9600);e(14400);e(19200);e(28800);e(38400);
                        e(56000);e(57600);e(64000);e(76800);e(115200);
			e(128000);e(150000);e(230400);e(256000);e(460800);
                        e(921600);
		case 134    :t = BAUD_134_5;   break;
		case 0      :t = -1;
								 break;
		default:
			/* Can I return "invalid"? */
			t = BAUD_9600;
			printk (KERN_INFO "sx: unsupported baud rate: %d.\n", port->gs.baud);
			break;
		}
#undef e
		if (t > 0) {
			/* The baud rate is not set to 0, so we're enabeling DTR... -- REW */
			sx_setsignals (port, 1, -1); 
			/* XXX This is not TA & MTA compatible */
			sx_write_channel_byte (port, hi_csr, 0xff);

			sx_write_channel_byte (port, hi_txbaud, t);
			sx_write_channel_byte (port, hi_rxbaud, t);
		} else {
			sx_setsignals (port, 0, -1);
		}
	} else {
		switch (port->gs.baud) {
#define e(x) case x:t= CSR_ ## x ; break
			e(75);e(150);e(300);e(600);e(1200);e(2400);e(4800);
                        e(1800);e(9600);
			e(19200);e(57600);e(38400);
			/* TA supports 110, but not 115200, MTA supports 115200, but not 110 */
		case 110: 
			if (port->board->ta_type == MOD_TA) {
				t = CSR_110;
				break;
			} else {
				t = CSR_9600;
				printk (KERN_INFO "sx: Unsupported baud rate: %d.\n", port->gs.baud);
				break;
			}
		case 115200: 
			if (port->board->ta_type == MOD_TA) {
				t = CSR_9600;
				printk (KERN_INFO "sx: Unsupported baud rate: %d.\n", port->gs.baud);
				break;
			} else {
				t = CSR_110;
				break;
			}
		case 0      :t = -1;
								 break;
		default:
			t = CSR_9600;
			printk (KERN_INFO "sx: Unsupported baud rate: %d.\n", port->gs.baud);
			break;
		}
#undef e
		if (t >= 0) {
			sx_setsignals (port, 1, -1);
			sx_write_channel_byte (port, hi_csr, t * 0x11);
		} else {
			sx_setsignals (port, 0, -1);
		}
	}
}


/* Simon Allen's version of this routine was 225 lines long. 85 is a lot
   better. -- REW */

static int sx_set_real_termios (void *ptr)
{
	struct sx_port *port = ptr;

	func_enter2();

	if (!port->gs.tty)
		return 0;

	/* What is this doing here? -- REW
	   Ha! figured it out. It is to allow you to get DTR active again
	   if you've dropped it with stty 0. Moved to set_baud, where it
	   belongs (next to the drop dtr if baud == 0) -- REW */
	/* sx_setsignals (port, 1, -1); */

	sx_set_baud (port);

#define CFLAG port->gs.tty->termios->c_cflag
	sx_write_channel_byte (port, hi_mr1,
	                       (C_PARENB (port->gs.tty)? MR1_WITH:MR1_NONE) |
	                       (C_PARODD (port->gs.tty)? MR1_ODD:MR1_EVEN) |
	                       (C_CRTSCTS(port->gs.tty)? MR1_RTS_RXFLOW:0) |
	                       (((CFLAG & CSIZE)==CS8) ? MR1_8_BITS:0) |
	                       (((CFLAG & CSIZE)==CS7) ? MR1_7_BITS:0) |
	                       (((CFLAG & CSIZE)==CS6) ? MR1_6_BITS:0) |
	                       (((CFLAG & CSIZE)==CS5) ? MR1_5_BITS:0) );

	sx_write_channel_byte (port, hi_mr2,
	                       (C_CRTSCTS(port->gs.tty)?MR2_CTS_TXFLOW:0) |
	                       (C_CSTOPB (port->gs.tty)?MR2_2_STOP:MR2_1_STOP));

	switch (CFLAG & CSIZE) {
	case CS8:sx_write_channel_byte (port, hi_mask, 0xff);break;
	case CS7:sx_write_channel_byte (port, hi_mask, 0x7f);break;
	case CS6:sx_write_channel_byte (port, hi_mask, 0x3f);break;
	case CS5:sx_write_channel_byte (port, hi_mask, 0x1f);break;
	default:
		printk (KERN_INFO "sx: Invalid wordsize: %d\n", CFLAG & CSIZE);
		break;
	}

	sx_write_channel_byte (port, hi_prtcl, 
	                       (I_IXON   (port->gs.tty)?SP_TXEN:0) |
	                       (I_IXOFF  (port->gs.tty)?SP_RXEN:0) |
	                       (I_IXANY  (port->gs.tty)?SP_TANY:0) |
	                       SP_DCEN);

	sx_write_channel_byte (port, hi_break, 
	                       I_OTHER(port->gs.tty) ? 0:
	                       (I_IGNBRK(port->gs.tty)?BR_IGN:0 |
	                        I_BRKINT(port->gs.tty)?BR_INT:0));

	sx_write_channel_byte (port, hi_txon,  START_CHAR (port->gs.tty));
	sx_write_channel_byte (port, hi_rxon,  START_CHAR (port->gs.tty));
	sx_write_channel_byte (port, hi_txoff, STOP_CHAR  (port->gs.tty));
	sx_write_channel_byte (port, hi_rxoff, STOP_CHAR  (port->gs.tty));

	sx_reconfigure_port(port);

	/* Tell line discipline whether we will do input cooking */
	if(I_OTHER(port->gs.tty)) {
		clear_bit(TTY_HW_COOK_IN, &port->gs.tty->flags);
	} else {
		set_bit(TTY_HW_COOK_IN, &port->gs.tty->flags);
	}
	sx_dprintk (SX_DEBUG_TERMIOS, "iflags: %x(%d) ", 
	            port->gs.tty->termios->c_iflag, 
	            I_OTHER(port->gs.tty));


/* Tell line discipline whether we will do output cooking.
 * If OPOST is set and no other output flags are set then we can do output
 * processing.  Even if only *one* other flag in the O_OTHER group is set
 * we do cooking in software.
 */
	if(O_OPOST(port->gs.tty) && !O_OTHER(port->gs.tty)) {
		set_bit(TTY_HW_COOK_OUT, &port->gs.tty->flags);
	} else {
		clear_bit(TTY_HW_COOK_OUT, &port->gs.tty->flags);
	}
	sx_dprintk (SX_DEBUG_TERMIOS, "oflags: %x(%d)\n", 
	            port->gs.tty->termios->c_oflag, 
	            O_OTHER(port->gs.tty));
	/* port->c_dcd = sx_get_CD (port); */
	func_exit ();
	return 0;
}



/* ********************************************************************** *
 *                   the interrupt related routines                       *
 * ********************************************************************** */

/* Note:
   Other drivers use the macro "MIN" to calculate how much to copy.
   This has the disadvantage that it will evaluate parts twice. That's
   expensive when it's IO (and the compiler cannot optimize those away!).
   Moreover, I'm not sure that you're race-free. 

   I assign a value, and then only allow the value to decrease. This
   is always safe. This makes the code a few lines longer, and you
   know I'm dead against that, but I think it is required in this
   case.  */


static void sx_transmit_chars (struct sx_port *port)
{
	int c;
	int tx_ip;
	int txroom;

	func_enter2 ();
	sx_dprintk (SX_DEBUG_TRANSMIT, "Port %p: transmit %d chars\n", 
	            port, port->gs.xmit_cnt);

	if (test_and_set_bit (SX_PORT_TRANSMIT_LOCK, &port->locks)) {
		return;
	}

	while (1) {
		c = port->gs.xmit_cnt;

		sx_dprintk (SX_DEBUG_TRANSMIT, "Copying %d ", c);
		tx_ip  = sx_read_channel_byte (port, hi_txipos);

		/* Took me 5 minutes to deduce this formula. 
		   Luckily it is literally in the manual in section 6.5.4.3.5 */
		txroom = (sx_read_channel_byte (port, hi_txopos) - tx_ip - 1) & 0xff;

		/* Don't copy more bytes than there is room for in the buffer */
		if (c > txroom)
			c = txroom;
		sx_dprintk (SX_DEBUG_TRANSMIT, " %d(%d) ", c, txroom );

		/* Don't copy past the end of the hardware transmit buffer */
		if (c > 0x100 - tx_ip) 
			c = 0x100 - tx_ip;

		sx_dprintk (SX_DEBUG_TRANSMIT, " %d(%d) ", c, 0x100-tx_ip );

		/* Don't copy pas the end of the source buffer */
		if (c > SERIAL_XMIT_SIZE - port->gs.xmit_tail) 
			c = SERIAL_XMIT_SIZE - port->gs.xmit_tail;

		sx_dprintk (SX_DEBUG_TRANSMIT, " %d(%ld) \n", 
		            c, SERIAL_XMIT_SIZE- port->gs.xmit_tail);

		/* If for one reason or another, we can't copy more data, we're done! */
		if (c == 0) break;


		memcpy_toio (port->board->base + CHAN_OFFSET(port,hi_txbuf) + tx_ip, 
		             port->gs.xmit_buf + port->gs.xmit_tail, c);

		/* Update the pointer in the card */
		sx_write_channel_byte (port, hi_txipos, (tx_ip+c) & 0xff);

		/* Update the kernel buffer end */
		port->gs.xmit_tail = (port->gs.xmit_tail + c) & (SERIAL_XMIT_SIZE-1);

		/* This one last. (this is essential)
		   It would allow others to start putting more data into the buffer! */
		port->gs.xmit_cnt -= c;
	}

	if (port->gs.xmit_cnt == 0) {
		sx_disable_tx_interrupts (port);
	}

	if ((port->gs.xmit_cnt <= port->gs.wakeup_chars) && port->gs.tty) {
		if ((port->gs.tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
		    port->gs.tty->ldisc.write_wakeup)
			(port->gs.tty->ldisc.write_wakeup)(port->gs.tty);
		sx_dprintk (SX_DEBUG_TRANSMIT, "Waking up.... ldisc (%d)....\n",
		            port->gs.wakeup_chars); 
		wake_up_interruptible(&port->gs.tty->write_wait);
	}

	clear_bit (SX_PORT_TRANSMIT_LOCK, &port->locks);
	func_exit ();
}


/* Note the symmetry between receiving chars and transmitting them!
   Note: The kernel should have implemented both a receive buffer and
   a transmit buffer. */

/* Inlined: Called only once. Remove the inline when you add another call */
static inline void sx_receive_chars (struct sx_port *port)
{
	int c;
	int rx_op;
	struct tty_struct *tty;
	int copied=0;

	func_enter2 ();
	tty = port->gs.tty;
	while (1) {
		rx_op = sx_read_channel_byte (port, hi_rxopos);
		c = (sx_read_channel_byte (port, hi_rxipos) - rx_op) & 0xff;

		sx_dprintk (SX_DEBUG_RECEIVE, "rxop=%d, c = %d.\n", rx_op, c); 

		/* Don't copy more bytes than there is room for in the buffer */
		if (tty->flip.count + c > TTY_FLIPBUF_SIZE) 
			c = TTY_FLIPBUF_SIZE - tty->flip.count;

		sx_dprintk (SX_DEBUG_RECEIVE, "c = %d.\n", c); 

		/* Don't copy past the end of the hardware receive buffer */
		if (rx_op + c > 0x100) c = 0x100 - rx_op;

		sx_dprintk (SX_DEBUG_RECEIVE, "c = %d.\n", c);

		/* If for one reason or another, we can't copy more data, we're done! */
		if (c == 0) break;

		sx_dprintk (SX_DEBUG_RECEIVE , "Copying over %d chars. First is %d at %lx\n", c, 
		            read_sx_byte (port->board, CHAN_OFFSET(port,hi_rxbuf) + rx_op),
		            CHAN_OFFSET(port, hi_rxbuf)); 
		memcpy_fromio (tty->flip.char_buf_ptr, 
		               port->board->base + CHAN_OFFSET(port,hi_rxbuf) + rx_op, c);
		memset(tty->flip.flag_buf_ptr, TTY_NORMAL, c);

		/* Update the kernel buffer end */
		tty->flip.count += c;
		tty->flip.char_buf_ptr += c;
		tty->flip.flag_buf_ptr += c;

		/* This one last. ( Not essential.)
		   It allows the card to start putting more data into the buffer! 
		   Update the pointer in the card */
		sx_write_channel_byte (port, hi_rxopos, (rx_op + c) & 0xff);

		copied += c;
	}
	if (copied) {
		struct timeval tv;

		do_gettimeofday (&tv);
		sx_dprintk (SX_DEBUG_RECEIVE, 
		            "pushing flipq port %d (%3d chars): %d.%06d  (%d/%d)\n", 
		            port->line, copied, 
		            (int) (tv.tv_sec % 60), (int)tv.tv_usec, tty->raw, tty->real_raw);

		/* Tell the rest of the system the news. Great news. New characters! */
		tty_flip_buffer_push (tty);
		/*    tty_schedule_flip (tty); */
	}

	func_exit ();
}

/* Inlined: it is called only once. Remove the inline if you add another 
   call */
static inline void sx_check_modem_signals (struct sx_port *port)
{
	int hi_state;
	int c_dcd;

	hi_state = sx_read_channel_byte (port, hi_state);
	sx_dprintk (SX_DEBUG_MODEMSIGNALS, "Checking modem signals (%d/%d)\n",
	            port->c_dcd, sx_get_CD (port));

	if (hi_state & ST_BREAK) {
		hi_state &= ~ST_BREAK;
		sx_dprintk (SX_DEBUG_MODEMSIGNALS, "got a break.\n");

		sx_write_channel_byte (port, hi_state, hi_state);
		if (port->gs.flags & ASYNC_SAK) {
			do_SAK (port->gs.tty);
		}
	}
	if (hi_state & ST_DCD) {
		hi_state &= ~ST_DCD;
		sx_dprintk (SX_DEBUG_MODEMSIGNALS, "got a DCD change.\n");
		sx_write_channel_byte (port, hi_state, hi_state);
		c_dcd = sx_get_CD (port);
		sx_dprintk (SX_DEBUG_MODEMSIGNALS, "DCD is now %d\n", c_dcd);
		if (c_dcd != port->c_dcd) {
			port->c_dcd = c_dcd;
			if (sx_get_CD (port)) {
				/* DCD went UP */
				if( (~(port->gs.flags & ASYNC_NORMAL_ACTIVE) || 
						 ~(port->gs.flags & ASYNC_CALLOUT_ACTIVE)) &&
						(sx_read_channel_byte(port, hi_hstat) != HS_IDLE_CLOSED)) {
					/* Are we blocking in open?*/
					sx_dprintk (SX_DEBUG_MODEMSIGNALS, "DCD active, unblocking open\n");
					wake_up_interruptible(&port->gs.open_wait);
				} else {
					sx_dprintk (SX_DEBUG_MODEMSIGNALS, "DCD raised. Ignoring.\n");
				}
			} else {
				/* DCD went down! */
				if (!((port->gs.flags & ASYNC_CALLOUT_ACTIVE) &&
				      (port->gs.flags & ASYNC_CALLOUT_NOHUP))) {
					sx_dprintk (SX_DEBUG_MODEMSIGNALS, "DCD dropped. hanging up....\n");
					tty_hangup (port->gs.tty);
				} else {
					sx_dprintk (SX_DEBUG_MODEMSIGNALS, "DCD dropped. ignoring.\n");
				}
			}
		} else {
			sx_dprintk (SX_DEBUG_MODEMSIGNALS, "Hmmm. card told us DCD changed, but it didn't.\n");
		}
	}
}


/* This is what an interrupt routine should look like. 
 * Small, elegant, clear.
 */

static void sx_interrupt (int irq, void *ptr, struct pt_regs *regs)
{
	struct sx_board *board = ptr;
	struct sx_port *port;
	int i;

	/*   func_enter ();  */
	sx_dprintk (SX_DEBUG_FLOW, "sx: enter sx_interrupt (%d/%d)\n", irq, board->irq); 

	/* AAargh! The order in which to do these things is essential and
	   not trivial. 

	   - Rate limit goes before "recursive". Otherwise a series of
	     recursive calls will hang the machine in the interrupt routine. 

	   - hardware twiddling goes before "recursive". Otherwise when we
	     poll the card, and a recursive interrupt happens, we wont
	     ack the card, so it might keep on interrupting us. (especially
	     level sensitive interrupt systems like PCI).

	   - Rate limit goes before hardware twiddling. Otherwise we won't
	     catch a card that has gone bonkers.

	   - The "initialized" test goes after the hardware twiddling. Otherwise
	     the card will stick us in the interrupt routine again.

	   - The initialized test goes before recursive. 
	*/



#ifdef IRQ_RATE_LIMIT
	/* Aaargh! I'm ashamed. This costs more lines-of-code than the
	   actual interrupt routine!. (Well, used to when I wrote that comment) */
	{
		static int lastjif;
		static int nintr=0;

		if (lastjif == jiffies) {
			if (++nintr > IRQ_RATE_LIMIT) {
				free_irq (board->irq, board);
				printk (KERN_ERR "sx: Too many interrupts. Turning off interrupt %d.\n", 
					      board->irq);
			}
		} else {
			lastjif = jiffies;
			nintr = 0;
		}
	}
#endif


	if (board->irq == irq) {
		/* Tell the card we've noticed the interrupt. */

		sx_write_board_word (board, cc_int_pending, 0);
		if (IS_SX_BOARD (board)) {
			write_sx_byte (board, SX_RESET_IRQ, 1);
		} else if (IS_EISA_BOARD(board)) {
			inb(board->eisa_base+0xc03);
			write_sx_word(board, 8, 0); 
		} else {
			write_sx_byte (board, SI2_ISA_INTCLEAR, SI2_ISA_INTCLEAR_CLEAR);
			write_sx_byte (board, SI2_ISA_INTCLEAR, SI2_ISA_INTCLEAR_SET);
		}
	}

	if (!sx_initialized) return;
	if (!(board->flags & SX_BOARD_INITIALIZED)) return;

	if (test_and_set_bit (SX_BOARD_INTR_LOCK, &board->locks)) {
		printk (KERN_ERR "Recursive interrupt! (%d)\n", board->irq);
		return;
	}

	 for (i=0;i<board->nports;i++) {
		port = &board->ports[i];
		if (port->gs.flags & GS_ACTIVE) {
			if (sx_read_channel_byte (port, hi_state)) {
				sx_dprintk (SX_DEBUG_INTERRUPTS, 
				            "Port %d: modem signal change?... \n", i);
				sx_check_modem_signals (port); 
			}
			if (port->gs.xmit_cnt) {
				sx_transmit_chars (port);
			}
			if (!(port->gs.flags & SX_RX_THROTTLE)) {
				sx_receive_chars (port);
			}
		}
	}

	clear_bit (SX_BOARD_INTR_LOCK, &board->locks);

	sx_dprintk (SX_DEBUG_FLOW, "sx: exit sx_interrupt (%d/%d)\n", irq, board->irq); 
	/*  func_exit ();  */
}


static void sx_pollfunc (unsigned long data)
{
	struct sx_board *board = (struct sx_board *) data;

	func_enter ();

	sx_interrupt (0, board, NULL);

	init_timer(&board->timer);

	board->timer.expires = jiffies + sx_poll;
	add_timer (&board->timer);
	func_exit ();
}



/* ********************************************************************** *
 *                Here are the routines that actually                     *
 *              interface with the generic_serial driver                  *
 * ********************************************************************** */

/* Ehhm. I don't know how to fiddle with interrupts on the SX card. --REW */
/* Hmm. Ok I figured it out. You don't.  */

static void sx_disable_tx_interrupts (void * ptr) 
{
	struct sx_port *port = ptr; 
	func_enter2();

	port->gs.flags &= ~GS_TX_INTEN;

	func_exit();
}


static void sx_enable_tx_interrupts (void * ptr) 
{
	struct sx_port *port = ptr; 
	int data_in_buffer;
	func_enter2();

	/* First transmit the characters that we're supposed to */
	sx_transmit_chars (port);

	/* The sx card will never interrupt us if we don't fill the buffer
	   past 25%. So we keep considering interrupts off if that's the case. */
	data_in_buffer = (sx_read_channel_byte (port, hi_txipos) - 
	                  sx_read_channel_byte (port, hi_txopos)) & 0xff;

	/* XXX Must be "HIGH_WATER" for SI card according to doc. */
	if (data_in_buffer < LOW_WATER) 
		port->gs.flags &= ~GS_TX_INTEN;

	func_exit();
}


static void sx_disable_rx_interrupts (void * ptr) 
{
	/*  struct sx_port *port = ptr; */
	func_enter();

	func_exit();
}

static void sx_enable_rx_interrupts (void * ptr) 
{
	/*  struct sx_port *port = ptr; */
	func_enter();

	func_exit();
}


/* Jeez. Isn't this simple? */
static int sx_get_CD (void * ptr) 
{
	struct sx_port *port = ptr;
	func_enter2();

	func_exit();
	return ((sx_read_channel_byte (port, hi_ip) & IP_DCD) != 0);
}


/* Jeez. Isn't this simple? */
static int sx_chars_in_buffer (void * ptr) 
{
	struct sx_port *port = ptr;
	func_enter2();

	func_exit();
	return ((sx_read_channel_byte (port, hi_txipos) - 
	         sx_read_channel_byte (port, hi_txopos)) & 0xff);
}


static void sx_shutdown_port (void * ptr) 
{
	struct sx_port *port = ptr; 

	func_enter();

	port->gs.flags &= ~ GS_ACTIVE;
	if (port->gs.tty && (port->gs.tty->termios->c_cflag & HUPCL)) {
		sx_setsignals (port, 0, 0);
		sx_reconfigure_port(port);
	}

	func_exit();
}





/* ********************************************************************** *
 *                Here are the routines that actually                     *
 *               interface with the rest of the system                    *
 * ********************************************************************** */

static int sx_open  (struct tty_struct * tty, struct file * filp)
{
	struct sx_port *port;
	int retval, line;

	func_enter();

	if (!sx_initialized) {
		return -EIO;
	}

	line = MINOR(tty->device);
	sx_dprintk (SX_DEBUG_OPEN, "%d: opening line %d. tty=%p ctty=%p, np=%d)\n", 
	            current->pid, line, tty, current->tty, sx_nports);

	if ((line < 0) || (line >= SX_NPORTS) || (line >= sx_nports))
		return -ENODEV;

	port = & sx_ports[line];
	port->c_dcd = 0; /* Make sure that the first interrupt doesn't detect a
	                    1 -> 0 transition. */


	sx_dprintk (SX_DEBUG_OPEN, "port = %p c_dcd = %d\n", port, port->c_dcd);

	tty->driver_data = port;
	port->gs.tty = tty;
	if (!port->gs.count)
		MOD_INC_USE_COUNT;
	port->gs.count++;

	sx_dprintk (SX_DEBUG_OPEN, "starting port\n");

	/*
	 * Start up serial port
	 */
	retval = gs_init_port(&port->gs);
	sx_dprintk (SX_DEBUG_OPEN, "done gs_init\n");
	if (retval) {
		port->gs.count--;
		if (port->gs.count) MOD_DEC_USE_COUNT;
		return retval;
	}

	port->gs.flags |= GS_ACTIVE;
	sx_setsignals (port, 1,1);

#if 0
	if (sx_debug & SX_DEBUG_OPEN)
		my_hd ((unsigned char *)port, sizeof (*port));
#else
	if (sx_debug & SX_DEBUG_OPEN)
		my_hd ((unsigned char *)port->board->base + port->ch_base, 
		       sizeof (*port));
#endif

	if (sx_send_command (port, HS_LOPEN, -1, HS_IDLE_OPEN) != 1) {
		printk (KERN_ERR "sx: Card didn't respond to LOPEN command.\n");
		port->gs.count--;
		if (!port->gs.count) MOD_DEC_USE_COUNT;
		return -EIO;
	}

	retval = gs_block_til_ready(port, filp);
	sx_dprintk (SX_DEBUG_OPEN, "Block til ready returned %d. Count=%d\n", 
	            retval, port->gs.count);

	if (retval) {
		/* 
		 * Don't lower gs.count here because sx_close() will be called later
		 */ 

		return retval;
	}
	/* tty->low_latency = 1; */

	if ((port->gs.count == 1) && (port->gs.flags & ASYNC_SPLIT_TERMIOS)) {
		if (tty->driver.subtype == SERIAL_TYPE_NORMAL)
			*tty->termios = port->gs.normal_termios;
		else 
			*tty->termios = port->gs.callout_termios;
		sx_set_real_termios (port);
	}

	port->gs.session = current->session;
	port->gs.pgrp = current->pgrp;
	port->c_dcd = sx_get_CD (port);
	sx_dprintk (SX_DEBUG_OPEN, "at open: cd=%d\n", port->c_dcd);
	func_exit();
	return 0;

}


/* I haven't the foggiest why the decrement use count has to happen
   here. The whole linux serial drivers stuff needs to be redesigned.
   My guess is that this is a hack to minimize the impact of a bug
   elsewhere. Thinking about it some more. (try it sometime) Try
   running minicom on a serial port that is driven by a modularized
   driver. Have the modem hangup. Then remove the driver module. Then
   exit minicom.  I expect an "oops".  -- REW */
static void sx_hungup (void *ptr)
{
  /*
	struct sx_port *port = ptr; 
  */
	func_enter ();

	/* Don't force the SX card to close. mgetty doesn't like it !!!!!! -- pvdl */
	/* For some reson we added this code. Don't know why anymore ;-( -- pvdl */
	/*
	sx_setsignals (port, 0, 0);
	sx_reconfigure_port(port);	
	sx_send_command (port, HS_CLOSE, 0, 0);

	if (sx_read_channel_byte (port, hi_hstat) != HS_IDLE_CLOSED) {
		if (sx_send_command (port, HS_FORCE_CLOSED, -1, HS_IDLE_CLOSED) != 1) {
			printk (KERN_ERR 
			        "sx: sent the force_close command, but card didn't react\n");
		} else
			sx_dprintk (SX_DEBUG_CLOSE, "sent the force_close command.\n");
	}
	*/
	MOD_DEC_USE_COUNT;
	func_exit ();
}


static void sx_close (void *ptr)
{
	struct sx_port *port = ptr; 
	/* Give the port 5 seconds to close down. */
	int to = 5 * HZ; 

	func_enter ();

	sx_setsignals (port, 0, 0);
	sx_reconfigure_port(port);	
	sx_send_command (port, HS_CLOSE, 0, 0);

	while (to-- && (sx_read_channel_byte (port, hi_hstat) != HS_IDLE_CLOSED)) {
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout (1);
		if (signal_pending (current))
				break;
	}
	current->state = TASK_RUNNING;
	if (sx_read_channel_byte (port, hi_hstat) != HS_IDLE_CLOSED) {
		if (sx_send_command (port, HS_FORCE_CLOSED, -1, HS_IDLE_CLOSED) != 1) {
			printk (KERN_ERR 
			        "sx: sent the force_close command, but card didn't react\n");
		} else
			sx_dprintk (SX_DEBUG_CLOSE, "sent the force_close command.\n");
	}

	sx_dprintk (SX_DEBUG_CLOSE, "waited %d jiffies for close. count=%d\n", 
	            5 * HZ - to - 1, port->gs.count);

	if(port->gs.count) {
		sx_dprintk(SX_DEBUG_CLOSE, "WARNING port count:%d\n", port->gs.count);
		port->gs.count = 0;
	}

	MOD_DEC_USE_COUNT;
	func_exit ();
}



/* This is relatively thorough. But then again it is only 20 lines. */
#define MARCHUP    for (i=min;i<max;i++) 
#define MARCHDOWN  for (i=max-1;i>=min;i--)
#define W0         write_sx_byte (board, i, 0x55)
#define W1         write_sx_byte (board, i, 0xaa)
#define R0         if (read_sx_byte (board, i) != 0x55) return 1
#define R1         if (read_sx_byte (board, i) != 0xaa) return 1

/* This memtest takes a human-noticable time. You normally only do it
   once a boot, so I guess that it is worth it. */
static int do_memtest (struct sx_board *board, int min, int max)
{
	int i;

	/* This is a marchb. Theoretically, marchb catches much more than
	   simpler tests. In practise, the longer test just catches more
	   intermittent errors. -- REW
	   (For the theory behind memory testing see: 
	   Testing Semiconductor Memories by A.J. van de Goor.) */
	MARCHUP	 {W0;}
	MARCHUP   {R0;W1;R1;W0;R0;W1;}
	MARCHUP   {R1;W0;W1;}
	MARCHDOWN {R1;W0;W1;W0;}
	MARCHDOWN {R0;W1;W0;}

	return 0;
}


#undef MARCHUP
#undef MARCHDOWN
#undef W0
#undef W1
#undef R0
#undef R1

#define MARCHUP    for (i=min;i<max;i+=2) 
#define MARCHDOWN  for (i=max-1;i>=min;i-=2)
#define W0         write_sx_word (board, i, 0x55aa)
#define W1         write_sx_word (board, i, 0xaa55)
#define R0         if (read_sx_word (board, i) != 0x55aa) return 1
#define R1         if (read_sx_word (board, i) != 0xaa55) return 1

/* This memtest takes a human-noticable time. You normally only do it
   once a boot, so I guess that it is worth it. */
static int do_memtest_w (struct sx_board *board, int min, int max)
{
	int i;

	MARCHUP   {W0;}
	MARCHUP   {R0;W1;R1;W0;R0;W1;}
	MARCHUP   {R1;W0;W1;}
	MARCHDOWN {R1;W0;W1;W0;}
	MARCHDOWN {R0;W1;W0;}

	return 0;
}


static int sx_fw_ioctl (struct inode *inode, struct file *filp,
                        unsigned int cmd, unsigned long arg)
{
	int rc = 0;
	int *descr = (int *)arg, i;
	static struct sx_board *board = NULL;
	int nbytes, offset, data;
	char *tmp;

	func_enter();

#if 0 
	/* Removed superuser check: Sysops can use the permissions on the device
	   file to restrict access. Recommendation: Root only. (root.root 600) */
	if (!suser ()) {
		return -EPERM;
	}
#endif

	sx_dprintk (SX_DEBUG_FIRMWARE, "IOCTL %x: %lx\n", cmd, arg);

	if (!board) board = &boards[0];
	if (board->flags & SX_BOARD_PRESENT) {
		sx_dprintk (SX_DEBUG_FIRMWARE, "Board present! (%x)\n", 
		            board->flags);
	} else {
		sx_dprintk (SX_DEBUG_FIRMWARE, "Board not present! (%x) all:", 
		            board->flags);
		for (i=0;i< SX_NBOARDS;i++)
			sx_dprintk (SX_DEBUG_FIRMWARE, "<%x> ", boards[i].flags);
		sx_dprintk (SX_DEBUG_FIRMWARE, "\n");
		return -EIO;
	}

	switch (cmd) {
	case SXIO_SET_BOARD:
		sx_dprintk (SX_DEBUG_FIRMWARE, "set board to %ld\n", arg);
		if (arg > SX_NBOARDS) return -EIO;
		sx_dprintk (SX_DEBUG_FIRMWARE, "not out of range\n");
		if (!(boards[arg].flags	& SX_BOARD_PRESENT)) return -EIO;
		sx_dprintk (SX_DEBUG_FIRMWARE, ".. and present!\n");
		board = &boards[arg];
		break;
	case SXIO_GET_TYPE:
		rc = -ENOENT; /* If we manage to miss one, return error. */
		if (IS_SX_BOARD (board)) rc = SX_TYPE_SX;
		if (IS_CF_BOARD (board)) rc = SX_TYPE_CF;
		if (IS_SI_BOARD (board)) rc = SX_TYPE_SI;
		if (IS_EISA_BOARD (board)) rc = SX_TYPE_SI;
		sx_dprintk (SX_DEBUG_FIRMWARE, "returning type= %d\n", rc);
		break;
	case SXIO_DO_RAMTEST:
		if (sx_initialized) /* Already initialized: better not ramtest the board.  */
			return -EPERM;
		if (IS_SX_BOARD (board)) {
			rc          = do_memtest   (board, 0, 0x7000);
			if (!rc) rc = do_memtest   (board, 0, 0x7000);
			/*if (!rc) rc = do_memtest_w (board, 0, 0x7000);*/
		} else {
			rc             = do_memtest   (board, 0, 0x7ff8);
			/* if (!rc) rc = do_memtest_w (board, 0, 0x7ff8); */
		}
		sx_dprintk (SX_DEBUG_FIRMWARE, "returning memtest result= %d\n", rc);
		break;
	case SXIO_DOWNLOAD:
		if (sx_initialized) /* Already initialized */
			return -EEXIST;
		if (!sx_reset (board)) 
			return -EIO;
		sx_dprintk (SX_DEBUG_INIT, "reset the board...\n");

		tmp = kmalloc (SX_CHUNK_SIZE, GFP_USER);
		if (!tmp) return -ENOMEM;
		Get_user (nbytes, descr++);
		Get_user (offset, descr++); 
		Get_user (data,	 descr++);
		while (nbytes && data) {
			for (i=0;i<nbytes;i += SX_CHUNK_SIZE) {
				copy_from_user (tmp, (char *)data+i, 
				                (i+SX_CHUNK_SIZE>nbytes)?nbytes-i:SX_CHUNK_SIZE);
				memcpy_toio    ((char *) (board->base2 + offset + i), tmp, 
				                (i+SX_CHUNK_SIZE>nbytes)?nbytes-i:SX_CHUNK_SIZE);
			}

			Get_user (nbytes, descr++);
			Get_user (offset, descr++); 
			Get_user (data,   descr++);
		}
		kfree (tmp);
		sx_nports += sx_init_board (board);
		rc = sx_nports;
		break;
	case SXIO_INIT:
		if (sx_initialized) /* Already initialized */
			return -EEXIST;
		/* This is not allowed until all boards are initialized... */
		for (i=0;i<SX_NBOARDS;i++) {
			if ( (boards[i].flags & SX_BOARD_PRESENT) &&
			     !(boards[i].flags & SX_BOARD_INITIALIZED))
				return -EIO;
		}
		for (i=0;i<SX_NBOARDS;i++)
			if (!(boards[i].flags & SX_BOARD_PRESENT)) break;

		sx_dprintk (SX_DEBUG_FIRMWARE, "initing portstructs, %d boards, "
		            "%d channels, first board: %d ports\n", 
		            i, sx_nports, boards[0].nports);
		rc = sx_init_portstructs (i, sx_nports);
		sx_init_drivers ();
		if (rc >= 0) 
			sx_initialized++;
		break;
	case SXIO_SETDEBUG:
		sx_debug = arg;
		break;
	case SXIO_GETDEBUG:
		rc = sx_debug;
		break;
	case SXIO_GETGSDEBUG:
	case SXIO_SETGSDEBUG:
		rc = -EINVAL;
		break;
	case SXIO_GETNPORTS:
		rc = sx_nports;
		break;
	default:
		printk (KERN_WARNING "Unknown ioctl on firmware device (%x).\n", cmd);
		break;
	}
	func_exit ();
	return rc;
}


static int sx_ioctl (struct tty_struct * tty, struct file * filp, 
                     unsigned int cmd, unsigned long arg)
{
	int rc;
	struct sx_port *port = tty->driver_data;
	int ival;

	/* func_enter2(); */

	rc = 0;
	switch (cmd) {
	case TIOCGSOFTCAR:
		rc = Put_user(((tty->termios->c_cflag & CLOCAL) ? 1 : 0),
		              (unsigned int *) arg);
		break;
	case TIOCSSOFTCAR:
		if ((rc = verify_area(VERIFY_READ, (void *) arg,
		                      sizeof(int))) == 0) {
			Get_user(ival, (unsigned int *) arg);
			tty->termios->c_cflag =
				(tty->termios->c_cflag & ~CLOCAL) |
				(ival ? CLOCAL : 0);
		}
		break;
	case TIOCGSERIAL:
		if ((rc = verify_area(VERIFY_WRITE, (void *) arg,
		                      sizeof(struct serial_struct))) == 0)
			gs_getserial(&port->gs, (struct serial_struct *) arg);
		break;
	case TIOCSSERIAL:
		if ((rc = verify_area(VERIFY_READ, (void *) arg,
		                      sizeof(struct serial_struct))) == 0)
			rc = gs_setserial(&port->gs, (struct serial_struct *) arg);
		break;
	case TIOCMGET:
		if ((rc = verify_area(VERIFY_WRITE, (void *) arg,
		                      sizeof(unsigned int))) == 0) {
			ival = sx_getsignals(port);
			put_user(ival, (unsigned int *) arg);
		}
		break;
	case TIOCMBIS:
		if ((rc = verify_area(VERIFY_READ, (void *) arg,
		                      sizeof(unsigned int))) == 0) {
			Get_user(ival, (unsigned int *) arg);
			sx_setsignals(port, ((ival & TIOCM_DTR) ? 1 : -1),
			                     ((ival & TIOCM_RTS) ? 1 : -1));
			sx_reconfigure_port(port);
		}
		break;
	case TIOCMBIC:
		if ((rc = verify_area(VERIFY_READ, (void *) arg,
		                      sizeof(unsigned int))) == 0) {
			Get_user(ival, (unsigned int *) arg);
			sx_setsignals(port, ((ival & TIOCM_DTR) ? 0 : -1),
			                     ((ival & TIOCM_RTS) ? 0 : -1));
			sx_reconfigure_port(port);
		}
		break;
	case TIOCMSET:
		if ((rc = verify_area(VERIFY_READ, (void *) arg,
		                      sizeof(unsigned int))) == 0) {
			Get_user(ival, (unsigned int *) arg);
			sx_setsignals(port, ((ival & TIOCM_DTR) ? 1 : 0),
			                     ((ival & TIOCM_RTS) ? 1 : 0));
			sx_reconfigure_port(port);
		}
		break;

	default:
		rc = -ENOIOCTLCMD;
		break;
	}

	/* func_exit(); */
	return rc;
}


/* The throttle/unthrottle scheme for the Specialix card is different
 * from other drivers and deserves some explanation. 
 * The Specialix hardware takes care of XON/XOFF
 * and CTS/RTS flow control itself.  This means that all we have to
 * do when signalled by the upper tty layer to throttle/unthrottle is
 * to make a note of it here.  When we come to read characters from the
 * rx buffers on the card (sx_receive_chars()) we look to see if the
 * upper layer can accept more (as noted here in sx_rx_throt[]). 
 * If it can't we simply don't remove chars from the cards buffer. 
 * When the tty layer can accept chars, we again note that here and when
 * sx_receive_chars() is called it will remove them from the cards buffer.
 * The card will notice that a ports buffer has drained below some low
 * water mark and will unflow control the line itself, using whatever
 * flow control scheme is in use for that port. -- Simon Allen
 */

static void sx_throttle (struct tty_struct * tty)
{
	struct sx_port *port = (struct sx_port *)tty->driver_data;

	func_enter2();
	/* If the port is using any type of input flow
	 * control then throttle the port.
	 */
	if((tty->termios->c_cflag & CRTSCTS) || (I_IXOFF(tty)) ) {
		port->gs.flags |= SX_RX_THROTTLE;
	}
	func_exit();
}


static void sx_unthrottle (struct tty_struct * tty)
{
	struct sx_port *port = (struct sx_port *)tty->driver_data;

	func_enter2();
	/* Always unthrottle even if flow control is not enabled on
	 * this port in case we disabled flow control while the port
	 * was throttled
	 */
	port->gs.flags &= ~SX_RX_THROTTLE;
	func_exit();
	return;
}


/* ********************************************************************** *
 *                    Here are the initialization routines.               *
 * ********************************************************************** */




static int sx_init_board (struct sx_board *board)
{
	int addr;
	int chans;
	int type;

	func_enter();

	/* This is preceded by downloading the download code. */

	board->flags |= SX_BOARD_INITIALIZED;

	if (read_sx_byte (board, 0))
		/* CF boards may need this. */
		write_sx_byte(board,0, 0);

	/* This resets the processor again, to make sure it didn't do any
	   foolish things while we were downloading the image */
	if (!sx_reset (board))
		return 0;

	sx_start_board (board);
	udelay (10);
	if (!sx_busy_wait_neq (board, 0, 0xff, 0)) {
		printk (KERN_ERR "sx: Ooops. Board won't initialize.\n");
		return 0;
	}

	/* Ok. So now the processor on the card is running. It gathered
	   some info for us... */
	sx_dprintk (SX_DEBUG_INIT, "The sxcard structure:\n");
	if (sx_debug & SX_DEBUG_INIT) my_hd ((char *)(board->base), 0x10);
	sx_dprintk (SX_DEBUG_INIT, "the first sx_module structure:\n");
	if (sx_debug & SX_DEBUG_INIT) my_hd ((char *)(board->base + 0x80), 0x30);

	sx_dprintk (SX_DEBUG_INIT, 
	            "init_status: %x, %dk memory, firmware V%x.%02x,\n", 
	            read_sx_byte (board, 0), read_sx_byte(board, 1), 
	            read_sx_byte (board, 5), read_sx_byte(board, 4));

	if (read_sx_byte (board, 0) == 0xff) {
		printk (KERN_INFO "sx: No modules found. Sorry.\n");
		board->nports = 0;
		return 0;
	}

	chans = 0;

	if (IS_SX_BOARD(board)) {
		sx_write_board_word (board, cc_int_count, sx_maxints);
	} else {
		if (sx_maxints)
			sx_write_board_word (board, cc_int_count, SI_PROCESSOR_CLOCK/8/sx_maxints);
	}

	/* grab the first module type... */
	/*  board->ta_type = mod_compat_type (read_sx_byte (board, 0x80 + 0x08)); */
	board->ta_type = mod_compat_type (sx_read_module_byte (board, 0x80, mc_chip));

	/* XXX byteorder */
	for (addr = 0x80;addr != 0;addr = read_sx_word (board, addr) & 0x7fff) {
		type = sx_read_module_byte (board, addr, mc_chip);
		sx_dprintk (SX_DEBUG_INIT, "Module at %x: %d channels\n", 
		            addr, read_sx_byte (board, addr + 2));

		chans += sx_read_module_byte (board, addr, mc_type);

		sx_dprintk (SX_DEBUG_INIT, "module is an %s, which has %s/%s panels\n", 
		            mod_type_s (type),
		            pan_type_s (sx_read_module_byte (board, addr, mc_mods) & 0xf),
		            pan_type_s (sx_read_module_byte (board, addr, mc_mods) >> 4));

		sx_dprintk (SX_DEBUG_INIT, "CD1400 versions: %x/%x, ASIC version: %x\n", 
		            sx_read_module_byte (board, addr, mc_rev1),
		            sx_read_module_byte (board, addr, mc_rev2),
		            sx_read_module_byte (board, addr, mc_mtaasic_rev));

		/* The following combinations are illegal: It should theoretically
		   work, but timing problems make the bus HANG. */

		if (mod_compat_type (type) != board->ta_type) {
			printk (KERN_ERR "sx: This is an invalid configuration.\n"
			        "Don't mix TA/MTA/SXDC on the same hostadapter.\n");
			chans=0;
			break;
		}
		if ((IS_EISA_BOARD(board) || 
		     IS_SI_BOARD(board)) && (mod_compat_type(type) == 4)) {
			printk (KERN_ERR "sx: This is an invalid configuration.\n"
			        "Don't use SXDCs on an SI/XIO adapter.\n");
			chans=0;
			break;
		}
#if 0 /* Problem fixed: firmware 3.05 */
		if (IS_SX_BOARD(board) && (type == TA8)) {
			/* There are some issues with the firmware and the DCD/RTS
			   lines. It might work if you tie them together or something.
			   It might also work if you get a newer sx_firmware.	Therefore
			   this is just a warning. */
			printk (KERN_WARNING "sx: The SX host doesn't work too well "
			        "with the TA8 adapters.\nSpecialix is working on it.\n");
		}
#endif
	}

	if (chans) {
		/* board->flags |= SX_BOARD_PRESENT; */
		if(board->irq > 0) {
			/* fixed irq, probably PCI */
			if(sx_irqmask & (1 << board->irq)) { /* may we use this irq? */
				if(request_irq(board->irq, sx_interrupt, SA_SHIRQ | SA_INTERRUPT, "sx", board)) {
					printk(KERN_ERR "sx: Cannot allocate irq %d.\n", board->irq);
					board->irq = 0;
				}
			} else
				board->irq = 0;
		} else if(board->irq < 0 && sx_irqmask) {
			/* auto-allocate irq */
			int irqnr;
			int irqmask = sx_irqmask & (IS_SX_BOARD(board) ? SX_ISA_IRQ_MASK : SI2_ISA_IRQ_MASK);
			for(irqnr = 15; irqnr > 0; irqnr--)
				if(irqmask & (1 << irqnr))
					if(! request_irq(irqnr, sx_interrupt, SA_SHIRQ | SA_INTERRUPT, "sx", board))
						break;
			if(! irqnr)
				printk(KERN_ERR "sx: Cannot allocate IRQ.\n");
			board->irq = irqnr;
		} else
			board->irq = 0;

		if (board->irq) {
			/* Found a valid interrupt, start up interrupts! */
			sx_dprintk (SX_DEBUG_INIT, "Using irq %d.\n", board->irq);
			sx_start_interrupts (board);
			board->poll = sx_slowpoll;
			board->flags |= SX_IRQ_ALLOCATED;
		} else {
			/* no irq: setup board for polled operation */
			board->poll = sx_poll;
			sx_dprintk (SX_DEBUG_INIT, "Using poll-interval %d.\n", board->poll);
		}

		/* The timer should be initialized anyway: That way we can safely
			 del_timer it when the module is unloaded. */
		init_timer (&board->timer);

		if (board->poll) {
			board->timer.data = (unsigned long) board;
			board->timer.function = sx_pollfunc;
			board->timer.expires = jiffies + board->poll;
			add_timer (&board->timer);
		}
	} else {
		board->irq = 0;
	}

	board->nports = chans;
	sx_dprintk (SX_DEBUG_INIT, "returning %d ports.", board->nports);

	func_exit();
	return chans;
}


static void printheader(void)
{
	static int header_printed;

	if (!header_printed) {
		printk (KERN_INFO "Specialix SX driver "
		        "(C) 1998/1999 R.E.Wolff@BitWizard.nl \n");
		printk (KERN_INFO "sx: version %s\n", RCS_ID);
		header_printed = 1;
	}
}


static int probe_sx (struct sx_board *board)
{
	struct vpd_prom vpdp;
	char *p;
	int i;

	func_enter();

	if (!IS_CF_BOARD (board)) {    
		sx_dprintk (SX_DEBUG_PROBE, "Going to verify vpd prom at %x.\n", 
		            board->base + SX_VPD_ROM);

		if (sx_debug & SX_DEBUG_PROBE)
			my_hd ((char *)(board->base + SX_VPD_ROM), 0x40);

		p = (char *) &vpdp;
		for (i=0;i< sizeof (struct vpd_prom);i++)
			*p++ = read_sx_byte (board, SX_VPD_ROM + i*2);

		if (sx_debug & SX_DEBUG_PROBE)
			my_hd ((char *)&vpdp, 0x20);

		sx_dprintk (SX_DEBUG_PROBE, "checking identifier...\n");

		if (strncmp (vpdp.identifier, SX_VPD_IDENT_STRING, 16) != 0) {
			sx_dprintk (SX_DEBUG_PROBE, "Got non-SX identifier: '%s'\n", 
			            vpdp.identifier); 
			return 0;
		}
	}

	printheader ();

	if (!IS_CF_BOARD (board)) {
		printk (KERN_DEBUG "sx: Found an SX board at %x\n", board->hw_base);
		printk (KERN_DEBUG "sx: hw_rev: %d, assembly level: %d, uniq ID:%08x, ", 
		        vpdp.hwrev, vpdp.hwass, vpdp.uniqid);
		printk (           "Manufactured: %d/%d\n", 
		        1970 + vpdp.myear, vpdp.mweek);


		if ((((vpdp.uniqid >> 24) & SX_UNIQUEID_MASK) != SX_PCI_UNIQUEID1) &&
		    (((vpdp.uniqid >> 24) & SX_UNIQUEID_MASK) != SX_ISA_UNIQUEID1)) {
			/* This might be a bit harsh. This was the primary reason the
			   SX/ISA card didn't work at first... */
			printk (KERN_ERR "sx: Hmm. Not an SX/PCI or SX/ISA card. Sorry: giving up.\n");
			return (0);
		}

		if (((vpdp.uniqid >> 24) & SX_UNIQUEID_MASK) == SX_ISA_UNIQUEID1) {
			if (board->base & 0x8000) {
				printk (KERN_WARNING "sx: Warning: There may be hardware problems with the card at %x.\n", board->base);
				printk (KERN_WARNING "sx: Read sx.txt for more info.\n");
			}
		}
	}

	board->nports = -1;

	/* This resets the processor, and keeps it off the bus. */
	if (!sx_reset (board)) 
		return 0;
	sx_dprintk (SX_DEBUG_INIT, "reset the board...\n");

	board->flags |= SX_BOARD_PRESENT;

	func_exit();
	return 1;
}



/* Specialix probes for this card at 32k increments from 640k to 16M.
   I consider machines with less than 16M unlikely nowadays, so I'm
   not probing above 1Mb. Also, 0xa0000, 0xb0000, are taken by the VGA
   card. 0xe0000 and 0xf0000 are taken by the BIOS. That only leaves 
   0xc0000, 0xc8000, 0xd0000 and 0xd8000 . */

static int probe_si (struct sx_board *board)
{
	int i;

	func_enter();
	sx_dprintk (SX_DEBUG_PROBE, "Going to verify SI signature %x.\n", 
	            board->base + SI2_ISA_ID_BASE);

	if (sx_debug & SX_DEBUG_PROBE)
		my_hd ((char *)(board->base + SI2_ISA_ID_BASE), 0x8);

	if (!IS_EISA_BOARD(board)) {
		for (i=0;i<8;i++) {
			if ((read_sx_byte (board, SI2_ISA_ID_BASE+7-i) & 7) != i) {
				return 0;
			}
		}
	}

	printheader ();

	printk (KERN_DEBUG "sx: Found an SI board at %x\n", board->hw_base);
	/* Compared to the SX boards, it is a complete guess as to what
		 this card is up to... */

	board->nports = -1;

	/* This resets the processor, and keeps it off the bus. */
	if (!sx_reset (board)) 
		return 0;
	sx_dprintk (SX_DEBUG_INIT, "reset the board...\n");

	board->flags |= SX_BOARD_PRESENT;

	func_exit();
	return 1;
}


static int sx_init_drivers(void)
{
	int error;

	func_enter();

	memset(&sx_driver, 0, sizeof(sx_driver));
	sx_driver.magic = TTY_DRIVER_MAGIC;
	sx_driver.driver_name = "specialix_sx";
	sx_driver.name = "ttyX";
	sx_driver.major = SX_NORMAL_MAJOR;
	sx_driver.num = sx_nports;
	sx_driver.type = TTY_DRIVER_TYPE_SERIAL;
	sx_driver.subtype = SX_TYPE_NORMAL;
	sx_driver.init_termios = tty_std_termios;
	sx_driver.init_termios.c_cflag =
	  B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	sx_driver.flags = TTY_DRIVER_REAL_RAW;
	sx_driver.refcount = &sx_refcount;
	sx_driver.table = sx_table;
	sx_driver.termios = sx_termios;
	sx_driver.termios_locked = sx_termios_locked;

	sx_driver.open	= sx_open;
	sx_driver.close = gs_close;
	sx_driver.write = gs_write;
	sx_driver.put_char = gs_put_char;
	sx_driver.flush_chars = gs_flush_chars;
	sx_driver.write_room = gs_write_room;
	sx_driver.chars_in_buffer = gs_chars_in_buffer;
	sx_driver.flush_buffer = gs_flush_buffer;
	sx_driver.ioctl = sx_ioctl;
	sx_driver.throttle = sx_throttle;
	sx_driver.unthrottle = sx_unthrottle;
	sx_driver.set_termios = gs_set_termios;
	sx_driver.stop = gs_stop;
	sx_driver.start = gs_start;
	sx_driver.hangup = gs_hangup;

	sx_callout_driver = sx_driver;
	sx_callout_driver.name = "cux";
	sx_callout_driver.major = SX_CALLOUT_MAJOR;
	sx_callout_driver.subtype = SX_TYPE_CALLOUT;

	if ((error = tty_register_driver(&sx_driver))) {
		printk(KERN_ERR "sx: Couldn't register sx driver, error = %d\n",
		       error);
		return 1;
	}
	if ((error = tty_register_driver(&sx_callout_driver))) {
		tty_unregister_driver(&sx_driver);
		printk(KERN_ERR "sx: Couldn't register sx callout driver, error = %d\n",
		       error);
		return 1;
	}

	func_exit();
	return 0;
}


static void * ckmalloc (int size)
{
	void *p;

	p = kmalloc(size, GFP_KERNEL);
	if (p) 
		memset(p, 0, size);
	return p;
}


static int sx_init_portstructs (int nboards, int nports)
{
	struct sx_board *board;
	struct sx_port *port;
	int i, j;
	int addr, chans;
	int portno;

	func_enter();

	/* Many drivers statically allocate the maximum number of ports
	   There is no reason not to allocate them dynamically. Is there? -- REW */
	sx_ports          = ckmalloc(nports * sizeof (struct sx_port));
	if (!sx_ports)
		return -ENOMEM;

	sx_termios        = ckmalloc(nports * sizeof (struct termios *));
	if (!sx_termios) {
		kfree (sx_ports);
		return -ENOMEM;
	}

	sx_termios_locked = ckmalloc(nports * sizeof (struct termios *));
	if (!sx_termios_locked) {
		kfree (sx_ports);
		kfree (sx_termios);
		return -ENOMEM;
	}

	/* Adjust the values in the "driver" */
	sx_driver.termios = sx_termios;
	sx_driver.termios_locked = sx_termios_locked;

	port = sx_ports;
	for (i = 0; i < nboards; i++) {
		board = &boards[i];
		board->ports = port;
		for (j=0; j < boards[i].nports;j++) {
			sx_dprintk (SX_DEBUG_INIT, "initing port %d\n", j);
			port->gs.callout_termios = tty_std_termios;
			port->gs.normal_termios	= tty_std_termios;
			port->gs.magic = SX_MAGIC;
			port->gs.close_delay = HZ/2;
			port->gs.closing_wait = 30 * HZ;
			port->board = board;
			port->gs.rd = &sx_real_driver;
#ifdef NEW_WRITE_LOCKING
			port->gs.port_write_sem = MUTEX;
#endif
			/*
			 * Initializing wait queue
			 */
			init_waitqueue_head(&port->gs.open_wait);
			init_waitqueue_head(&port->gs.close_wait); 		
			
			port++;
		}
	}

	port = sx_ports;
	portno = 0;
	for (i = 0; i < nboards; i++) {
		board = &boards[i];
		board->port_base = portno;
		/* Possibly the configuration was rejected. */
		sx_dprintk (SX_DEBUG_PROBE, "Board has %d channels\n", board->nports);
		if (board->nports <= 0) continue;
		/* XXX byteorder ?? */
		for (addr = 0x80;addr != 0;addr = read_sx_word (board, addr) & 0x7fff) {
			chans = sx_read_module_byte (board, addr, mc_type); 
			sx_dprintk (SX_DEBUG_PROBE, "Module at %x: %d channels\n", addr, chans);
			sx_dprintk (SX_DEBUG_PROBE, "Port at");
			for (j=0;j<chans;j++) {
				/* The "sx-way" is the way it SHOULD be done. That way in the 
				   future, the firmware may for example pack the structures a bit
				   more efficient. Neil tells me it isn't going to happen anytime
				   soon though. */
				if (IS_SX_BOARD(board))
					port->ch_base = sx_read_module_word (board, addr+j*2, mc_chan_pointer);
				else
					port->ch_base = addr + 0x100 + 0x300*j;

				sx_dprintk (SX_DEBUG_PROBE, " %x", port->ch_base);
				port->line = portno++;
				port++;
			}
			sx_dprintk (SX_DEBUG_PROBE, "\n");
		}
		/* This has to be done earlier. */
		/* board->flags |= SX_BOARD_INITIALIZED; */
	}

	func_exit();
	return 0;
}

static void __exit sx_release_drivers(void)
{
	func_enter();
	tty_unregister_driver(&sx_driver);
	tty_unregister_driver(&sx_callout_driver);
	func_exit();
}

#ifdef TWO_ZERO
#define PDEV unsigned char pci_bus, unsigned pci_fun
#define pdev pci_bus, pci_fun
#else
#define PDEV   struct pci_dev *pdev
#endif


#ifdef CONFIG_PCI
 /******************************************************** 
 * Setting bit 17 in the CNTRL register of the PLX 9050  * 
 * chip forces a retry on writes while a read is pending.*
 * This is to prevent the card locking up on Intel Xeon  *
 * multiprocessor systems with the NX chipset.    -- NV  *
 ********************************************************/

/* Newer cards are produced with this bit set from the configuration
   EEprom.  As the bit is read/write for the CPU, we can fix it here,
   if we detect that it isn't set correctly. -- REW */

static void fix_sx_pci (PDEV, struct sx_board *board)
{
	unsigned int hwbase;
	unsigned long rebase;
	unsigned int t;

#define CNTRL_REG_OFFSET        0x50
#define CNTRL_REG_GOODVALUE     0x18260000

	pci_read_config_dword(pdev, PCI_BASE_ADDRESS_0, &hwbase);
	hwbase &= PCI_BASE_ADDRESS_MEM_MASK;
	rebase = (ulong) ioremap(hwbase, 0x80);
	t = readl (rebase + CNTRL_REG_OFFSET);
	if (t != CNTRL_REG_GOODVALUE) {
		printk (KERN_DEBUG "sx: performing cntrl reg fix: %08x -> %08x\n", t, CNTRL_REG_GOODVALUE); 
		writel (CNTRL_REG_GOODVALUE, rebase + CNTRL_REG_OFFSET);
	}
	my_iounmap (hwbase, rebase);
}
#endif


static int __init sx_init(void) 
{
	int i;
	int found = 0;
	int eisa_slot;
	struct sx_board *board;

#ifdef CONFIG_PCI
#ifndef TWO_ZERO
	struct pci_dev *pdev = NULL;
#else
	unsigned char pci_bus, pci_fun;
	/* in 2.2.x pdev is a pointer defining a PCI device. In 2.0 its the bus/fn */
#endif
	unsigned int tint;
	unsigned short tshort;
#endif

	func_enter();
	sx_dprintk (SX_DEBUG_INIT, "Initing sx module... (sx_debug=%d)\n", sx_debug);
	if (abs ((long) (&sx_debug) - sx_debug) < 0x10000) {
		printk (KERN_WARNING "sx: sx_debug is an address, instead of a value. "
		        "Assuming -1.\n");
		printk ("(%p)\n", &sx_debug);
		sx_debug=-1;
	}

#ifdef CONFIG_PCI
	if (pci_present ()) {
#ifndef TWO_ZERO
		while ((pdev = pci_find_device (PCI_VENDOR_ID_SPECIALIX, 
		                                PCI_DEVICE_ID_SPECIALIX_SX_XIO_IO8, 
			                              pdev))) {
			if (pci_enable_device(pdev))
				continue;
#else
			for (i=0;i< SX_NBOARDS;i++) {
				if (pcibios_find_device (PCI_VENDOR_ID_SPECIALIX, 
				                         PCI_DEVICE_ID_SPECIALIX_SX_XIO_IO8, i,
					                       &pci_bus, &pci_fun)) break;
#endif
			/* Specialix has a whole bunch of cards with
			   0x2000 as the device ID. They say its because
			   the standard requires it. Stupid standard. */
			/* It seems that reading a word doesn't work reliably on 2.0.
			   Also, reading a non-aligned dword doesn't work. So we read the
			   whole dword at 0x2c and extract the word at 0x2e (SUBSYSTEM_ID)
			   ourselves */
			/* I don't know why the define doesn't work, constant 0x2c does --REW */ 
			pci_read_config_dword (pdev, 0x2c, &tint);
			tshort = (tint >> 16) & 0xffff;
			sx_dprintk (SX_DEBUG_PROBE, "Got a specialix card: %x.\n", tint);
			/* sx_dprintk (SX_DEBUG_PROBE, "pdev = %d/%d	(%x)\n", pdev, tint); */ 
			if ((tshort != 0x0200) && (tshort != 0x0300)) {
				sx_dprintk (SX_DEBUG_PROBE, "But it's not an SX card (%d)...\n", 
				            tshort);
				continue;
			}
			board = &boards[found];

			board->flags &= ~SX_BOARD_TYPE;
			board->flags |= (tshort == 0x200)?SX_PCI_BOARD:
			                                  SX_CFPCI_BOARD;

			/* CF boards use base address 3.... */
			if (IS_CF_BOARD (board))
				board->hw_base = pci_resource_start (pdev, 3);
			else
				board->hw_base = pci_resource_start (pdev, 2);
			board->base2 = 
			board->base = (ulong) ioremap(board->hw_base, WINDOW_LEN (board));
			if (!board->base) {
				printk(KERN_ERR "ioremap failed\n");
				/* XXX handle error */
			}

			/* Most of the stuff on the CF board is offset by
			   0x18000 ....  */
			if (IS_CF_BOARD (board)) board->base += 0x18000;

			board->irq = get_irq (pdev);

			sx_dprintk (SX_DEBUG_PROBE, "Got a specialix card: %x/%x(%d) %x.\n", 
			            tint, boards[found].base, board->irq, board->flags);

			if (probe_sx (board)) {
				found++;
				fix_sx_pci (pdev, board);
			} else 
				my_iounmap (board->hw_base, board->base);
		}
	}
#endif

	for (i=0;i<NR_SX_ADDRS;i++) {
		board = &boards[found];
		board->hw_base = sx_probe_addrs[i];
		board->base2 =
		board->base = (ulong) ioremap(board->hw_base, SX_WINDOW_LEN);
		board->flags &= ~SX_BOARD_TYPE;
		board->flags |=	SX_ISA_BOARD;
		board->irq = sx_irqmask?-1:0;

		if (probe_sx (board)) {
			found++;
		} else {
			my_iounmap (board->hw_base, board->base);
		}
	}

	for (i=0;i<NR_SI_ADDRS;i++) {
		board = &boards[found];
		board->hw_base = si_probe_addrs[i];
		board->base2 =
		board->base = (ulong) ioremap(board->hw_base, SI2_ISA_WINDOW_LEN);
		board->flags &= ~SX_BOARD_TYPE;
		board->flags |=  SI_ISA_BOARD;
		board->irq = sx_irqmask ?-1:0;

		if (probe_si (board)) {
			found++;
		} else {
			my_iounmap (board->hw_base, board->base);
		}
	}

        sx_dprintk(SX_DEBUG_PROBE, "Probing for EISA cards\n");
        for(eisa_slot=0x1000; eisa_slot<0x10000; eisa_slot+=0x1000)
        {
                if((inb(eisa_slot+0xc80)==0x4d) &&
                   (inb(eisa_slot+0xc81)==0x98))
                {
			sx_dprintk(SX_DEBUG_PROBE, "%s : Signature found in EISA slot %d, Product %d Rev %d\n",
			                        "XIO", (eisa_slot>>12), inb(eisa_slot+0xc82), inb(eisa_slot+0xc83));

			board = &boards[found];
			board->eisa_base = eisa_slot;
			board->flags &= ~SX_BOARD_TYPE;
			board->flags |= SI_EISA_BOARD;

			board->hw_base = (((inb(0xc01+eisa_slot) << 8) + inb(0xc00+eisa_slot)) << 16);
			board->base2 =
			board->base = (ulong) ioremap(board->hw_base, SI2_EISA_WINDOW_LEN);

			sx_dprintk(SX_DEBUG_PROBE, "IO hw_base address: %x\n", board->hw_base);
			sx_dprintk(SX_DEBUG_PROBE, "base: %x\n", board->base);
			board->irq = inb(board->eisa_base+0xc02)>>4; 
			sx_dprintk(SX_DEBUG_PROBE, "IRQ: %d\n", board->irq);
			
			probe_si(board);

			found++;
		}
	}
	if (found) {
		printk (KERN_INFO "sx: total of %d boards detected.\n", found);

		if (misc_register(&sx_fw_device) < 0) {
			printk(KERN_ERR "SX: Unable to register firmware loader driver.\n");
			return -EIO;
		}
	}

	func_exit();
	return found?0:-EIO;
}


static void __exit sx_exit (void)
{
	int i; 
	struct sx_board *board;

	func_enter();
	for (i = 0; i < SX_NBOARDS; i++) {
		board = &boards[i];
		if (board->flags & SX_BOARD_INITIALIZED) {
			sx_dprintk (SX_DEBUG_CLEANUP, "Cleaning up board at %x\n", board->base);
			/* The board should stop messing with us.
			   (actually I mean the interrupt) */
			sx_reset (board);
			if ((board->irq) && (board->flags & SX_IRQ_ALLOCATED))
				free_irq (board->irq, board);

			/* It is safe/allowed to del_timer a non-active timer */
			del_timer (& board->timer);
			my_iounmap (board->hw_base, board->base);
		}
	}
	if (misc_deregister(&sx_fw_device) < 0) {
		printk (KERN_INFO "sx: couldn't deregister firmware loader device\n");
	}
	sx_dprintk (SX_DEBUG_CLEANUP, "Cleaning up drivers (%d)\n", sx_initialized);
	if (sx_initialized)
		sx_release_drivers ();

	kfree (sx_ports);
	kfree (sx_termios);
	kfree (sx_termios_locked);
	func_exit();
}

module_init(sx_init);
module_exit(sx_exit);

