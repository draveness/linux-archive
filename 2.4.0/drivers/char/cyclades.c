#undef	BLOCKMOVE
#define	Z_WAKE
#undef	Z_EXT_CHARS_IN_BUFFER
static char rcsid[] =
"$Revision: 2.3.2.8 $$Date: 2000/07/06 18:14:16 $";

/*
 *  linux/drivers/char/cyclades.c
 *
 * This file contains the driver for the Cyclades async multiport
 * serial boards.
 *
 * Initially written by Randolph Bentson <bentson@grieg.seaslug.org>.
 * Modified and maintained by Marcio Saito <marcio@cyclades.com>.
 * Currently maintained by Ivan Passos <ivan@cyclades.com>.
 *
 * For Technical support and installation problems, please send e-mail
 * to support@cyclades.com.
 *
 * Much of the design and some of the code came from serial.c
 * which was copyright (C) 1991, 1992  Linus Torvalds.  It was
 * extensively rewritten by Theodore Ts'o, 8/16/92 -- 9/14/92,
 * and then fixed as suggested by Michael K. Johnson 12/12/92.
 *
 * This version supports shared IRQ's (only for PCI boards).
 *
 * $Log: cyclades.c,v $
 * Revision 2.3.2.8   2000/07/06 18:14:16 ivan
 * Fixed the PCI detection function to work properly on Alpha systems.
 * Implemented support for TIOCSERGETLSR ioctl.
 * Implemented full support for non-standard baud rates.
 *
 * Revision 2.3.2.7   2000/06/01 18:26:34 ivan
 * Request PLX I/O region, although driver doesn't use it, to avoid
 * problems with other drivers accessing it.
 * Removed count for on-board buffer characters in cy_chars_in_buffer
 * (Cyclades-Z only).
 *
 * Revision 2.3.2.6   2000/05/05 13:56:05 ivan
 * Driver now reports physical instead of virtual memory addresses.
 * Masks were added to some Cyclades-Z read accesses.
 * Implemented workaround for PLX9050 bug that would cause a system lockup
 * in certain systems, depending on the MMIO addresses allocated to the
 * board.
 * Changed the Tx interrupt programming in the CD1400 chips to boost up
 * performance (Cyclom-Y only).
 * Code is now compliant with the new module interface (module_[init|exit]).
 * Make use of the PCI helper functions to access PCI resources.
 * Did some code "housekeeping".
 *
 * Revision 2.3.2.5   2000/01/19 14:35:33 ivan
 * Fixed bug in cy_set_termios on CRTSCTS flag turnoff.
 *
 * Revision 2.3.2.4   2000/01/17 09:19:40 ivan
 * Fixed SMP locking in Cyclom-Y interrupt handler.
 *
 * Revision 2.3.2.3   1999/12/28 12:11:39 ivan
 * Added a new cyclades_card field called nports to allow the driver to
 * know the exact number of ports found by the Z firmware after its load;
 * RX buffer contention prevention logic on interrupt op mode revisited
 * (Cyclades-Z only);
 * Revisited printk's for Z debug;
 * Driver now makes sure that the constant SERIAL_XMIT_SIZE is defined;
 *
 * Revision 2.3.2.2   1999/10/01 11:27:43 ivan
 * Fixed bug in cyz_poll that would make all ports but port 0 
 * unable to transmit/receive data (Cyclades-Z only);
 * Implemented logic to prevent the RX buffer from being stuck with data
 * due to a driver / firmware race condition in interrupt op mode
 * (Cyclades-Z only);
 * Fixed bug in block_til_ready logic that would lead to a system crash;
 * Revisited cy_close spinlock usage;
 *
 * Revision 2.3.2.1   1999/09/28 11:01:22 ivan
 * Revisited CONFIG_PCI conditional compilation for PCI board support;
 * Implemented TIOCGICOUNT and TIOCMIWAIT ioctl support;
 * _Major_ cleanup on the Cyclades-Z interrupt support code / logic;
 * Removed CTS handling from the driver -- this is now completely handled
 * by the firmware (Cyclades-Z only);
 * Flush RX on-board buffers on a port open (Cyclades-Z only);
 * Fixed handling of ASYNC_SPD_* TTY flags;
 * Module unload now unmaps all memory area allocated by ioremap;
 *
 * Revision 2.3.1.1   1999/07/15 16:45:53 ivan
 * Removed CY_PROC conditional compilation;
 * Implemented SMP-awareness for the driver;
 * Implemented a new ISA IRQ autoprobe that uses the irq_probe_[on|off] 
 * functions;
 * The driver now accepts memory addresses (maddr=0xMMMMM) and IRQs
 * (irq=NN) as parameters (only for ISA boards);
 * Fixed bug in set_line_char that would prevent the Cyclades-Z 
 * ports from being configured at speeds above 115.2Kbps;
 * Fixed bug in cy_set_termios that would prevent XON/XOFF flow control
 * switching from working properly;
 * The driver now only prints IRQ info for the Cyclades-Z if it's 
 * configured to work in interrupt mode;
 *
 * Revision 2.2.2.3   1999/06/28 11:13:29 ivan
 * Added support for interrupt mode operation for the Z cards;
 * Removed the driver inactivity control for the Z;
 * Added a missing MOD_DEC_USE_COUNT in the cy_open function for when 
 * the Z firmware is not loaded yet;
 * Replaced the "manual" Z Tx flush buffer by a call to a FW command of 
 * same functionality;
 * Implemented workaround for IRQ setting loss on the PCI configuration 
 * registers after a PCI bridge EEPROM reload (affects PLX9060 only);
 *
 * Revision 2.2.2.2  1999/05/14 17:18:15 ivan
 * /proc entry location changed to /proc/tty/driver/cyclades;
 * Added support to shared IRQ's (only for PCI boards);
 * Added support for Cobalt Qube2 systems;
 * IRQ [de]allocation scheme revisited;
 * BREAK implementation changed in order to make use of the 'break_ctl'
 * TTY facility;
 * Fixed typo in TTY structure field 'driver_name';
 * Included a PCI bridge reset and EEPROM reload in the board 
 * initialization code (for both Y and Z series).
 *
 * Revision 2.2.2.1  1999/04/08 16:17:43 ivan
 * Fixed a bug in cy_wait_until_sent that was preventing the port to be 
 * closed properly after a SIGINT;
 * Module usage counter scheme revisited;
 * Added support to the upcoming Y PCI boards (i.e., support to additional
 * PCI Device ID's).
 * 
 * Revision 2.2.1.10 1999/01/20 16:14:29 ivan
 * Removed all unnecessary page-alignement operations in ioremap calls
 * (ioremap is currently safe for these operations).
 *
 * Revision 2.2.1.9  1998/12/30 18:18:30 ivan
 * Changed access to PLX PCI bridge registers from I/O to MMIO, in 
 * order to make PLX9050-based boards work with certain motherboards.
 *
 * Revision 2.2.1.8  1998/11/13 12:46:20 ivan
 * cy_close function now resets (correctly) the tty->closing flag;
 * JIFFIES_DIFF macro fixed.
 *
 * Revision 2.2.1.7  1998/09/03 12:07:28 ivan
 * Fixed bug in cy_close function, which was not informing HW of
 * which port should have the reception disabled before doing so;
 * fixed Cyclom-8YoP hardware detection bug.
 *
 * Revision 2.2.1.6  1998/08/20 17:15:39 ivan
 * Fixed bug in cy_close function, which causes malfunction
 * of one of the first 4 ports when a higher port is closed
 * (Cyclom-Y only).
 *
 * Revision 2.2.1.5  1998/08/10 18:10:28 ivan
 * Fixed Cyclom-4Yo hardware detection bug.
 *
 * Revision 2.2.1.4  1998/08/04 11:02:50 ivan
 * /proc/cyclades implementation with great collaboration of 
 * Marc Lewis <marc@blarg.net>;
 * cyy_interrupt was changed to avoid occurence of kernel oopses
 * during PPP operation.
 *
 * Revision 2.2.1.3  1998/06/01 12:09:10 ivan
 * General code review in order to comply with 2.1 kernel standards;
 * data loss prevention for slow devices revisited (cy_wait_until_sent
 * was created);
 * removed conditional compilation for new/old PCI structure support 
 * (now the driver only supports the new PCI structure).
 *
 * Revision 2.2.1.1  1998/03/19 16:43:12 ivan
 * added conditional compilation for new/old PCI structure support;
 * removed kernel series (2.0.x / 2.1.x) conditional compilation.
 *
 * Revision 2.1.1.3  1998/03/16 18:01:12 ivan
 * cleaned up the data loss fix;
 * fixed XON/XOFF handling once more (Cyclades-Z);
 * general review of the driver routines;
 * introduction of a mechanism to prevent data loss with slow 
 * printers, by forcing a delay before closing the port.
 *
 * Revision 2.1.1.2  1998/02/17 16:50:00 ivan
 * fixed detection/handling of new CD1400 in Ye boards;
 * fixed XON/XOFF handling (Cyclades-Z);
 * fixed data loss caused by a premature port close;
 * introduction of a flag that holds the CD1400 version ID per port
 * (used by the CYGETCD1400VER new ioctl).
 *
 * Revision 2.1.1.1  1997/12/03 17:31:19 ivan
 * Code review for the module cleanup routine;
 * fixed RTS and DTR status report for new CD1400's in get_modem_info;
 * includes anonymous changes regarding signal_pending.
 * 
 * Revision 2.1  1997/11/01 17:42:41 ivan
 * Changes in the driver to support Alpha systems (except 8Zo V_1);
 * BREAK fix for the Cyclades-Z boards;
 * driver inactivity control by FW implemented;
 * introduction of flag that allows driver to take advantage of 
 * a special CD1400 feature related to HW flow control;
 * added support for the CD1400  rev. J (Cyclom-Y boards);
 * introduction of ioctls to:
 *  - control the rtsdtr_inv flag (Cyclom-Y);
 *  - control the rflow flag (Cyclom-Y);
 *  - adjust the polling interval (Cyclades-Z);
 *
 * Revision 1.36.4.33  1997/06/27 19:00:00  ivan
 * Fixes related to kernel version conditional 
 * compilation.
 *  
 * Revision 1.36.4.32  1997/06/14 19:30:00  ivan
 * Compatibility issues between kernels 2.0.x and 
 * 2.1.x (mainly related to clear_bit function).
 *  
 * Revision 1.36.4.31  1997/06/03 15:30:00  ivan
 * Changes to define the memory window according to the 
 * board type.
 *  
 * Revision 1.36.4.30  1997/05/16 15:30:00  daniel
 * Changes to suport new cycladesZ boards.
 *
 * Revision 1.36.4.29  1997/05/12 11:30:00  daniel
 * Merge of Bentson's and Daniel's version 1.36.4.28.
 * Corrects bug in cy_detect_pci: check if there are more
 * ports than the number of static structs allocated.
 * Warning message during initialization if this driver is
 * used with the new generation of cycladesZ boards.  Those
 * will be supported only in next release of the driver.
 * Corrects bug in cy_detect_pci and cy_detect_isa that
 * returned wrong number of VALID boards, when a cyclomY
 * was found with no serial modules connected.
 * Changes to use current (2.1.x) kernel subroutine names
 * and created macros for compilation with 2.0.x kernel,
 * instead of the other way around.
 *
 * Revision 1.36.4.28  1997/05/?? ??:00:00  bentson
 * Change queue_task_irq_off to queue_task_irq.
 * The inline function queue_task_irq_off (tqueue.h)
 * was removed from latest releases of 2.1.x kernel.
 * Use of macro __init to mark the initialization
 * routines, so memory can be reused.
 * Also incorporate implementation of critical region
 * in function cleanup_module() created by anonymous
 * linuxer.
 *
 * Revision 1.36.4.28  1997/04/25 16:00:00  daniel
 * Change to support new firmware that solves DCD problem:
 * application could fail to receive SIGHUP signal when DCD
 * varying too fast.
 *
 * Revision 1.36.4.27  1997/03/26 10:30:00  daniel
 * Changed for suport linux versions 2.1.X.
 * Backward compatible with linux versions 2.0.X.
 * Corrected illegal use of filler field in
 * CH_CTRL struct.
 * Deleted some debug messages.
 *
 * Revision 1.36.4.26  1997/02/27 12:00:00  daniel
 * Included check for NULL tty pointer in cyz_poll.
 *
 * Revision 1.36.4.25  1997/02/26 16:28:30  bentson
 * Bill Foster at Blarg! Online services noticed that
 * some of the switch elements of -Z modem control
 * lacked a closing "break;"
 *
 * Revision 1.36.4.24  1997/02/24 11:00:00  daniel
 * Changed low water threshold for buffer xmit_buf
 *
 * Revision 1.36.4.23  1996/12/02 21:50:16  bentson
 * Marcio provided fix to modem status fetch for -Z
 *
 * Revision 1.36.4.22  1996/10/28 22:41:17  bentson
 * improve mapping of -Z control page (thanks to Steve
 * Price <stevep@fa.tdktca.com> for help on this)
 *
 * Revision 1.36.4.21  1996/09/10 17:00:10  bentson
 * shift from CPU-bound to memcopy in cyz_polling operation
 *
 * Revision 1.36.4.20  1996/09/09 18:30:32  Bentson
 * Added support to set and report higher speeds.
 *
 * Revision 1.36.4.19c  1996/08/09 10:00:00  Marcio Saito
 * Some fixes in the HW flow control for the BETA release.
 * Don't try to register the IRQ.
 *
 * Revision 1.36.4.19  1996/08/08 16:23:18  Bentson
 * make sure "cyc" appears in all kernel messages; all soft interrupts
 * handled by same routine; recognize out-of-band reception; comment
 * out some diagnostic messages; leave RTS/CTS flow control to hardware;
 * fix race condition in -Z buffer management; only -Y needs to explictly
 * flush chars; tidy up some startup messages;
 *
 * Revision 1.36.4.18  1996/07/25 18:57:31  bentson
 * shift MOD_INC_USE_COUNT location to match
 * serial.c; purge some diagnostic messages;
 *
 * Revision 1.36.4.17  1996/07/25 18:01:08  bentson
 * enable modem status messages and fetch & process them; note
 * time of last activity type for each port; set_line_char now
 * supports more than line 0 and treats 0 baud correctly;
 * get_modem_info senses rs_status;
 *
 * Revision 1.36.4.16  1996/07/20 08:43:15  bentson
 * barely works--now's time to turn on
 * more features 'til it breaks
 *
 * Revision 1.36.4.15  1996/07/19 22:30:06  bentson
 * check more -Z board status; shorten boot message
 *
 * Revision 1.36.4.14  1996/07/19 22:20:37  bentson
 * fix reference to ch_ctrl in startup; verify return
 * values from cyz_issue_cmd and cyz_update_channel;
 * more stuff to get modem control correct;
 *
 * Revision 1.36.4.13  1996/07/11 19:53:33  bentson
 * more -Z stuff folded in; re-order changes to put -Z stuff
 * after -Y stuff (to make changes clearer)
 *
 * Revision 1.36.4.12  1996/07/11 15:40:55  bentson
 * Add code to poll Cyclades-Z.  Add code to get & set RS-232 control.
 * Add code to send break.  Clear firmware ID word at startup (so
 * that other code won't talk to inactive board).
 *
 * Revision 1.36.4.11  1996/07/09 05:28:29  bentson
 * add code for -Z in set_line_char
 *
 * Revision 1.36.4.10  1996/07/08 19:28:37  bentson
 * fold more -Z stuff (or in some cases, error messages)
 * into driver; add text to "don't know what to do" messages.
 *
 * Revision 1.36.4.9  1996/07/08 18:38:38  bentson
 * moved compile-time flags near top of file; cosmetic changes
 * to narrow text (to allow 2-up printing); changed many declarations
 * to "static" to limit external symbols; shuffled code order to
 * coalesce -Y and -Z specific code, also to put internal functions
 * in order of tty_driver structure; added code to recognize -Z
 * ports (and for moment, do nothing or report error); add cy_startup
 * to parse boot command line for extra base addresses for ISA probes;
 *
 * Revision 1.36.4.8  1996/06/25 17:40:19  bentson
 * reorder some code, fix types of some vars (int vs. long),
 * add cy_setup to support user declared ISA addresses
 *
 * Revision 1.36.4.7  1996/06/21 23:06:18  bentson
 * dump ioctl based firmware load (it's now a user level
 * program); ensure uninitialzed ports cannot be used
 *
 * Revision 1.36.4.6  1996/06/20 23:17:19  bentson
 * rename vars and restructure some code
 *
 * Revision 1.36.4.5  1996/06/14 15:09:44  bentson
 * get right status back after boot load
 *
 * Revision 1.36.4.4  1996/06/13 19:51:44  bentson
 * successfully loads firmware
 *
 * Revision 1.36.4.3  1996/06/13 06:08:33  bentson
 * add more of the code for the boot/load ioctls
 *
 * Revision 1.36.4.2  1996/06/11 21:00:51  bentson
 * start to add Z functionality--starting with ioctl
 * for loading firmware
 *
 * Revision 1.36.4.1  1996/06/10 18:03:02  bentson
 * added code to recognize Z/PCI card at initialization; report
 * presence, but card is not initialized (because firmware needs
 * to be loaded)
 *
 * Revision 1.36.3.8  1996/06/07 16:29:00  bentson
 * starting minor number at zero; added missing verify_area
 * as noted by Heiko Eissfeldt <heiko@colossus.escape.de>
 *
 * Revision 1.36.3.7  1996/04/19 21:06:18  bentson
 * remove unneeded boot message & fix CLOCAL hardware flow
 * control (Miquel van Smoorenburg <miquels@Q.cistron.nl>);
 * remove unused diagnostic statements; minor 0 is first;
 *
 * Revision 1.36.3.6  1996/03/13 13:21:17  marcio
 * The kernel function vremap (available only in later 1.3.xx kernels)
 * allows the access to memory addresses above the RAM. This revision
 * of the driver supports PCI boards below 1Mb (device id 0x100) and
 * above 1Mb (device id 0x101).
 *
 * Revision 1.36.3.5  1996/03/07 15:20:17  bentson
 * Some global changes to interrupt handling spilled into
 * this driver--mostly unused arguments in system function
 * calls.  Also added change by Marcio Saito which should
 * reduce lost interrupts at startup by fast processors.
 *
 * Revision 1.36.3.4  1995/11/13  20:45:10  bentson
 * Changes by Corey Minyard <minyard@wf-rch.cirr.com> distributed
 * in 1.3.41 kernel to remove a possible race condition, extend
 * some error messages, and let the driver run as a loadable module
 * Change by Alan Wendt <alan@ez0.ezlink.com> to remove a
 * possible race condition.
 * Change by Marcio Saito <marcio@cyclades.com> to fix PCI addressing.
 *
 * Revision 1.36.3.3  1995/11/13  19:44:48  bentson
 * Changes by Linus Torvalds in 1.3.33 kernel distribution
 * required due to reordering of driver initialization.
 * Drivers are now initialized *after* memory management.
 *
 * Revision 1.36.3.2  1995/09/08  22:07:14  bentson
 * remove printk from ISR; fix typo
 *
 * Revision 1.36.3.1  1995/09/01  12:00:42  marcio
 * Minor fixes in the PCI board support. PCI function calls in
 * conditional compilation (CONFIG_PCI). Thanks to Jim Duncan
 * <duncan@okay.com>. "bad serial count" message removed.
 *
 * Revision 1.36.3  1995/08/22  09:19:42  marcio
 * Cyclom-Y/PCI support added. Changes in the cy_init routine and
 * board initialization. Changes in the boot messages. The driver
 * supports up to 4 boards and 64 ports by default.
 *
 * Revision 1.36.1.4  1995/03/29  06:14:14  bentson
 * disambiguate between Cyclom-16Y and Cyclom-32Ye;
 *
 * Revision 1.36.1.3  1995/03/23  22:15:35  bentson
 * add missing break in modem control block in ioctl switch statement
 * (discovered by Michael Edward Chastain <mec@jobe.shell.portal.com>);
 *
 * Revision 1.36.1.2  1995/03/22  19:16:22  bentson
 * make sure CTS flow control is set as soon as possible (thanks
 * to note from David Lambert <lambert@chesapeake.rps.slb.com>);
 *
 * Revision 1.36.1.1  1995/03/13  15:44:43  bentson
 * initialize defaults for receive threshold and stale data timeout;
 * cosmetic changes;
 *
 * Revision 1.36  1995/03/10  23:33:53  bentson
 * added support of chips 4-7 in 32 port Cyclom-Ye;
 * fix cy_interrupt pointer dereference problem
 * (Joe Portman <baron@aa.net>);
 * give better error response if open is attempted on non-existent port
 * (Zachariah Vaum <jchryslr@netcom.com>);
 * correct command timeout (Kenneth Lerman <lerman@@seltd.newnet.com>);
 * conditional compilation for -16Y on systems with fast, noisy bus;
 * comment out diagnostic print function;
 * cleaned up table of base addresses;
 * set receiver time-out period register to correct value,
 * set receive threshold to better default values,
 * set chip timer to more accurate 200 Hz ticking,
 * add code to monitor and modify receive parameters
 * (Rik Faith <faith@cs.unc.edu> Nick Simicich
 * <njs@scifi.emi.net>);
 *
 * Revision 1.35  1994/12/16  13:54:18  steffen
 * additional patch by Marcio Saito for board detection
 * Accidently left out in 1.34
 *
 * Revision 1.34  1994/12/10  12:37:12  steffen
 * This is the corrected version as suggested by Marcio Saito
 *
 * Revision 1.33  1994/12/01  22:41:18  bentson
 * add hooks to support more high speeds directly; add tytso
 * patch regarding CLOCAL wakeups
 *
 * Revision 1.32  1994/11/23  19:50:04  bentson
 * allow direct kernel control of higher signalling rates;
 * look for cards at additional locations
 *
 * Revision 1.31  1994/11/16  04:33:28  bentson
 * ANOTHER fix from Corey Minyard, minyard@wf-rch.cirr.com--
 * a problem in chars_in_buffer has been resolved by some
 * small changes;  this should yield smoother output
 *
 * Revision 1.30  1994/11/16  04:28:05  bentson
 * Fix from Corey Minyard, Internet: minyard@metronet.com,
 * UUCP: minyard@wf-rch.cirr.com, WORK: minyardbnr.ca, to
 * cy_hangup that appears to clear up much (all?) of the
 * DTR glitches; also he's added/cleaned-up diagnostic messages
 *
 * Revision 1.29  1994/11/16  04:16:07  bentson
 * add change proposed by Ralph Sims, ralphs@halcyon.com, to
 * operate higher speeds in same way as other serial ports;
 * add more serial ports (for up to two 16-port muxes).
 *
 * Revision 1.28  1994/11/04  00:13:16  root
 * turn off diagnostic messages
 *
 * Revision 1.27  1994/11/03  23:46:37  root
 * bunch of changes to bring driver into greater conformance
 * with the serial.c driver (looking for missed fixes)
 *
 * Revision 1.26  1994/11/03  22:40:36  root
 * automatic interrupt probing fixed.
 *
 * Revision 1.25  1994/11/03  20:17:02  root
 * start to implement auto-irq
 *
 * Revision 1.24  1994/11/03  18:01:55  root
 * still working on modem signals--trying not to drop DTR
 * during the getty/login processes
 *
 * Revision 1.23  1994/11/03  17:51:36  root
 * extend baud rate support; set receive threshold as function
 * of baud rate; fix some problems with RTS/CTS;
 *
 * Revision 1.22  1994/11/02  18:05:35  root
 * changed arguments to udelay to type long to get
 * delays to be of correct duration
 *
 * Revision 1.21  1994/11/02  17:37:30  root
 * employ udelay (after calibrating loops_per_second earlier
 * in init/main.c) instead of using home-grown delay routines
 *
 * Revision 1.20  1994/11/02  03:11:38  root
 * cy_chars_in_buffer forces a return value of 0 to let
 * login work (don't know why it does); some functions
 * that were returning EFAULT, now executes the code;
 * more work on deciding when to disable xmit interrupts;
 *
 * Revision 1.19  1994/11/01  20:10:14  root
 * define routine to start transmission interrupts (by enabling
 * transmit interrupts); directly enable/disable modem interrupts;
 *
 * Revision 1.18  1994/11/01  18:40:45  bentson
 * Don't always enable transmit interrupts in startup; interrupt on
 * TxMpty instead of TxRdy to help characters get out before shutdown;
 * restructure xmit interrupt to check for chars first and quit if
 * none are ready to go; modem status (MXVRx) is upright, _not_ inverted
 * (to my view);
 *
 * Revision 1.17  1994/10/30  04:39:45  bentson
 * rename serial_driver and callout_driver to cy_serial_driver and
 * cy_callout_driver to avoid linkage interference; initialize
 * info->type to PORT_CIRRUS; ruggedize paranoia test; elide ->port
 * from cyclades_port structure; add paranoia check to cy_close;
 *
 * Revision 1.16  1994/10/30  01:14:33  bentson
 * change major numbers; add some _early_ return statements;
 *
 * Revision 1.15  1994/10/29  06:43:15  bentson
 * final tidying up for clean compile;  enable some error reporting
 *
 * Revision 1.14  1994/10/28  20:30:22  Bentson
 * lots of changes to drag the driver towards the new tty_io
 * structures and operation.  not expected to work, but may
 * compile cleanly.
 *
 * Revision 1.13  1994/07/21  23:08:57  Bentson
 * add some diagnostic cruft; support 24 lines (for testing
 * both -8Y and -16Y cards; be more thorough in servicing all
 * chips during interrupt; add "volatile" a few places to
 * circumvent compiler optimizations; fix base & offset
 * computations in block_til_ready (was causing chip 0 to
 * stop operation)
 *
 * Revision 1.12  1994/07/19  16:42:11  Bentson
 * add some hackery for kernel version 1.1.8; expand
 * error messages; refine timing for delay loops and
 * declare loop params volatile
 *
 * Revision 1.11  1994/06/11  21:53:10  bentson
 * get use of save_car right in transmit interrupt service
 *
 * Revision 1.10.1.1  1994/06/11  21:31:18  bentson
 * add some diagnostic printing; try to fix save_car stuff
 *
 * Revision 1.10  1994/06/11  20:36:08  bentson
 * clean up compiler warnings
 *
 * Revision 1.9  1994/06/11  19:42:46  bentson
 * added a bunch of code to support modem signalling
 *
 * Revision 1.8  1994/06/11  17:57:07  bentson
 * recognize break & parity error
 *
 * Revision 1.7  1994/06/05  05:51:34  bentson
 * Reorder baud table to be monotonic; add cli to CP; discard
 * incoming characters and status if the line isn't open; start to
 * fold code into cy_throttle; start to port get_serial_info,
 * set_serial_info, get_modem_info, set_modem_info, and send_break
 * from serial.c; expand cy_ioctl; relocate and expand config_setup;
 * get flow control characters from tty struct; invalidate ports w/o
 * hardware;
 *
 * Revision 1.6  1994/05/31  18:42:21  bentson
 * add a loop-breaker in the interrupt service routine;
 * note when port is initialized so that it can be shut
 * down under the right conditions; receive works without
 * any obvious errors
 *
 * Revision 1.5  1994/05/30  00:55:02  bentson
 * transmit works without obvious errors
 *
 * Revision 1.4  1994/05/27  18:46:27  bentson
 * incorporated more code from lib_y.c; can now print short
 * strings under interrupt control to port zero; seems to
 * select ports/channels/lines correctly
 *
 * Revision 1.3  1994/05/25  22:12:44  bentson
 * shifting from multi-port on a card to proper multiplexor
 * data structures;  added skeletons of most routines
 *
 * Revision 1.2  1994/05/19  13:21:43  bentson
 * start to crib from other sources
 *
 */

/* If you need to install more boards than NR_CARDS, change the constant
   in the definition below. No other change is necessary to support up to
   eight boards. Beyond that you'll have to extend cy_isa_addresses. */

#define NR_CARDS        4

/*
   If the total number of ports is larger than NR_PORTS, change this
   constant in the definition below. No other change is necessary to
   support more boards/ports. */

#define NR_PORTS        256

#define ZE_V1_NPORTS	64
#define ZO_V1	0
#define ZO_V2	1
#define ZE_V1	2

#define	SERIAL_PARANOIA_CHECK
#undef	CY_DEBUG_OPEN
#undef	CY_DEBUG_THROTTLE
#undef	CY_DEBUG_OTHER
#undef	CY_DEBUG_IO
#undef	CY_DEBUG_COUNT
#undef	CY_DEBUG_DTR
#undef	CY_DEBUG_WAIT_UNTIL_SENT
#undef	CY_DEBUG_INTERRUPTS
#undef	CY_16Y_HACK
#undef	CY_ENABLE_MONITORING
#undef	CY_PCI_DEBUG

#if 0
#define PAUSE __asm__("nop");
#else
#define PAUSE ;
#endif

#define cy_min(a,b) (((a)<(b))?(a):(b))

/*
 * Include section 
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/serial.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/ptrace.h>
#include <linux/cyclades.h>
#include <linux/mm.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/spinlock.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/uaccess.h>
#include <asm/bitops.h>

#define	CY_LOCK(info,flags)					\
		do {						\
		spin_lock_irqsave(&cy_card[info->card].card_lock, flags); \
		} while (0)
		
#define	CY_UNLOCK(info,flags)					\
		do {						\
		spin_unlock_irqrestore(&cy_card[info->card].card_lock, flags); \
		} while (0)

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/version.h>

#include <linux/stat.h>
#include <linux/proc_fs.h>

#ifdef CONFIG_COBALT_27
#include <asm/page.h>
#include <asm/pgtable.h>

#define	CACHED_TO_UNCACHED(x)	(((unsigned long)(x) & \
				  (unsigned long)0x1fffffff) + KSEG1)
#endif

#define cy_put_user	put_user

static unsigned long 
cy_get_user(unsigned long *addr)
{
	unsigned long result = 0;
	int error = get_user (result, addr);
	if (error)
		printk ("cyclades: cy_get_user: error == %d\n", error);
	return result;
}

#ifndef MIN
#define MIN(a,b)        ((a) < (b) ? (a) : (b))
#endif

#define IS_CYC_Z(card) ((card).num_chips == -1)

#define Z_FPGA_CHECK(card) \
    ((cy_readl(&((struct RUNTIME_9060 *) \
		 ((card).ctl_addr))->init_ctrl) & (1<<17)) != 0)

#define ISZLOADED(card)	(((ZO_V1==cy_readl(&((struct RUNTIME_9060 *) \
			((card).ctl_addr))->mail_box_0)) || \
			Z_FPGA_CHECK(card)) && \
			(ZFIRM_ID==cy_readl(&((struct FIRM_ID *) \
			((card).base_addr+ID_ADDRESS))->signature)))

#ifndef SERIAL_XMIT_SIZE
#define	SERIAL_XMIT_SIZE	(MIN(PAGE_SIZE, 4096))
#endif
#define WAKEUP_CHARS		256

#define STD_COM_FLAGS (0)

#define	JIFFIES_DIFF(n, j)	((j) - (n))

static DECLARE_TASK_QUEUE(tq_cyclades);

static struct tty_driver cy_serial_driver, cy_callout_driver;
static int serial_refcount;

#ifdef CONFIG_ISA
/* This is the address lookup table. The driver will probe for
   Cyclom-Y/ISA boards at all addresses in here. If you want the
   driver to probe addresses at a different address, add it to
   this table.  If the driver is probing some other board and
   causing problems, remove the offending address from this table.
   The cy_setup function extracts additional addresses from the
   boot options line.  The form is "cyclades=address,address..."
*/

static unsigned char *cy_isa_addresses[] = {
        (unsigned char *) 0xD0000,
        (unsigned char *) 0xD2000,
        (unsigned char *) 0xD4000,
        (unsigned char *) 0xD6000,
        (unsigned char *) 0xD8000,
        (unsigned char *) 0xDA000,
        (unsigned char *) 0xDC000,
        (unsigned char *) 0xDE000,
        0,0,0,0,0,0,0,0
};
#define NR_ISA_ADDRS (sizeof(cy_isa_addresses)/sizeof(unsigned char*))

#ifdef MODULE
static int maddr[NR_CARDS] = { 0, };
static int irq[NR_CARDS]  = { 0, };

MODULE_PARM(maddr, "1-" __MODULE_STRING(NR_CARDS) "l");
MODULE_PARM(irq, "1-" __MODULE_STRING(NR_CARDS) "i");
#endif

#endif /* CONFIG_ISA */

/* This is the per-card data structure containing address, irq, number of
   channels, etc. This driver supports a maximum of NR_CARDS cards.
*/
static struct cyclades_card cy_card[NR_CARDS];

/* This is the per-channel data structure containing pointers, flags
 and variables for the port. This driver supports a maximum of NR_PORTS.
*/
static struct cyclades_port cy_port[NR_PORTS];

static int cy_next_channel; /* next minor available */

static struct tty_struct *serial_table[NR_PORTS];
static struct termios *serial_termios[NR_PORTS];
static struct termios *serial_termios_locked[NR_PORTS];

/*
 * tmp_buf is used as a temporary buffer by serial_write.  We need to
 * lock it in case the copy_from_user blocks while swapping in a page,
 * and some other program tries to do a serial write at the same time.
 * Since the lock will only come under contention when the system is
 * swapping and available memory is low, it makes sense to share one
 * buffer across all the serial ports, since it significantly saves
 * memory if large numbers of serial ports are open.  This buffer is
 * allocated when the first cy_open occurs.
 */
static unsigned char *tmp_buf;
DECLARE_MUTEX(tmp_buf_sem);

/*
 * This is used to look up the divisor speeds and the timeouts
 * We're normally limited to 15 distinct baud rates.  The extra
 * are accessed via settings in info->flags.
 *      0,     1,     2,     3,     4,     5,     6,     7,     8,     9,
 *     10,    11,    12,    13,    14,    15,    16,    17,    18,    19,
 *                                               HI            VHI
 *     20
 */
static int baud_table[] = {
       0,    50,    75,   110,   134,   150,   200,   300,   600,  1200,
    1800,  2400,  4800,  9600, 19200, 38400, 57600, 76800,115200,150000,
  230400,     0};

static char baud_co_25[] = {  /* 25 MHz clock option table */
    /* value =>    00    01   02    03    04 */
    /* divide by    8    32   128   512  2048 */
    0x00,  0x04,  0x04,  0x04,  0x04,  0x04,  0x03,  0x03,  0x03,  0x02,
    0x02,  0x02,  0x01,  0x01,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00};

static char baud_bpr_25[] = {  /* 25 MHz baud rate period table */
    0x00,  0xf5,  0xa3,  0x6f,  0x5c,  0x51,  0xf5,  0xa3,  0x51,  0xa3,
    0x6d,  0x51,  0xa3,  0x51,  0xa3,  0x51,  0x36,  0x29,  0x1b,  0x15};

static char baud_co_60[] = {  /* 60 MHz clock option table (CD1400 J) */
    /* value =>    00    01   02    03    04 */
    /* divide by    8    32   128   512  2048 */
    0x00,  0x00,  0x00,  0x04,  0x04,  0x04,  0x04,  0x04,  0x03,  0x03,
    0x03,  0x02,  0x02,  0x01,  0x01,  0x00,  0x00,  0x00,  0x00,  0x00,
    0x00};

static char baud_bpr_60[] = {  /* 60 MHz baud rate period table (CD1400 J) */
    0x00,  0x82,  0x21,  0xff,  0xdb,  0xc3,  0x92,  0x62,  0xc3,  0x62,
    0x41,  0xc3,  0x62,  0xc3,  0x62,  0xc3,  0x82,  0x62,  0x41,  0x32,
    0x21};

static char baud_cor3[] = {  /* receive threshold */
    0x0a,  0x0a,  0x0a,  0x0a,  0x0a,  0x0a,  0x0a,  0x0a,  0x0a,  0x0a,
    0x0a,  0x0a,  0x0a,  0x09,  0x09,  0x08,  0x08,  0x08,  0x08,  0x07,
    0x07};

/*
 * The Cyclades driver implements HW flow control as any serial driver.
 * The cyclades_port structure member rflow and the vector rflow_thr 
 * allows us to take advantage of a special feature in the CD1400 to avoid 
 * data loss even when the system interrupt latency is too high. These flags 
 * are to be used only with very special applications. Setting these flags 
 * requires the use of a special cable (DTR and RTS reversed). In the new 
 * CD1400-based boards (rev. 6.00 or later), there is no need for special 
 * cables.
 */

static char rflow_thr[] = {  /* rflow threshold */
    0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,
    0x00,  0x00,  0x00,  0x0a,  0x0a,  0x0a,  0x0a,  0x0a,  0x0a,  0x0a,
    0x0a};

/*  The Cyclom-Ye has placed the sequential chips in non-sequential
 *  address order.  This look-up table overcomes that problem.
 */
static int cy_chip_offset [] =
    { 0x0000,
      0x0400,
      0x0800,
      0x0C00,
      0x0200,
      0x0600,
      0x0A00,
      0x0E00
    };

/* PCI related definitions */

static unsigned short	cy_pci_nboard;
static unsigned short	cy_isa_nboard;
static unsigned short	cy_nboard;
#ifdef CONFIG_PCI
static unsigned short	cy_pci_dev_id[] = {
			    PCI_DEVICE_ID_CYCLOM_Y_Lo,	/* PCI < 1Mb */
			    PCI_DEVICE_ID_CYCLOM_Y_Hi,	/* PCI > 1Mb */
			    PCI_DEVICE_ID_CYCLOM_4Y_Lo,	/* 4Y PCI < 1Mb */
			    PCI_DEVICE_ID_CYCLOM_4Y_Hi,	/* 4Y PCI > 1Mb */
			    PCI_DEVICE_ID_CYCLOM_8Y_Lo,	/* 8Y PCI < 1Mb */
			    PCI_DEVICE_ID_CYCLOM_8Y_Hi,	/* 8Y PCI > 1Mb */
			    PCI_DEVICE_ID_CYCLOM_Z_Lo,	/* Z PCI < 1Mb */
			    PCI_DEVICE_ID_CYCLOM_Z_Hi,	/* Z PCI > 1Mb */
			    0				/* end of table */
			};
#endif

static void cy_start(struct tty_struct *);
static void set_line_char(struct cyclades_port *);
static int cyz_issue_cmd(struct cyclades_card *, uclong, ucchar, uclong);
#ifdef CONFIG_ISA
static unsigned detect_isa_irq (volatile ucchar *);
#endif /* CONFIG_ISA */

static int cyclades_get_proc_info(char *, char **, off_t , int , int *, void *);

#ifndef CONFIG_CYZ_INTR
static void cyz_poll(unsigned long);

/* The Cyclades-Z polling cycle is defined by this variable */
static long cyz_polling_cycle = CZ_DEF_POLL;

static int cyz_timeron = 0;
static struct timer_list cyz_timerlist = {
    function: cyz_poll
};
#else /* CONFIG_CYZ_INTR */
static void cyz_rx_restart(unsigned long);
static struct timer_list cyz_rx_full_timer[NR_PORTS];
#endif /* CONFIG_CYZ_INTR */

static inline int
serial_paranoia_check(struct cyclades_port *info,
                        kdev_t device, const char *routine)
{
#ifdef SERIAL_PARANOIA_CHECK
    static const char *badmagic =
        "cyc Warning: bad magic number for serial struct (%s) in %s\n";
    static const char *badinfo =
        "cyc Warning: null cyclades_port for (%s) in %s\n";
    static const char *badrange =
        "cyc Warning: cyclades_port out of range for (%s) in %s\n";

    if (!info) {
        printk(badinfo, kdevname(device), routine);
        return 1;
    }

    if( (long)info < (long)(&cy_port[0])
    || (long)(&cy_port[NR_PORTS]) < (long)info ){
        printk(badrange, kdevname(device), routine);
        return 1;
    }

    if (info->magic != CYCLADES_MAGIC) {
        printk(badmagic, kdevname(device), routine);
        return 1;
    }
#endif
        return 0;
} /* serial_paranoia_check */

/*
 * This routine is used by the interrupt handler to schedule
 * processing in the software interrupt portion of the driver
 * (also known as the "bottom half").  This can be called any
 * number of times for any channel without harm.
 */
static inline void
cy_sched_event(struct cyclades_port *info, int event)
{
    info->event |= 1 << event; /* remember what kind of event and who */
    queue_task(&info->tqueue, &tq_cyclades); /* it belongs to */
    mark_bh(CYCLADES_BH);                       /* then trigger event */
} /* cy_sched_event */


/*
 * This routine is used to handle the "bottom half" processing for the
 * serial driver, known also the "software interrupt" processing.
 * This processing is done at the kernel interrupt level, after the
 * cy#/_interrupt() has returned, BUT WITH INTERRUPTS TURNED ON.  This
 * is where time-consuming activities which can not be done in the
 * interrupt driver proper are done; the interrupt driver schedules
 * them using cy_sched_event(), and they get done here.
 *
 * This is done through one level of indirection--the task queue.
 * When a hardware interrupt service routine wants service by the
 * driver's bottom half, it enqueues the appropriate tq_struct (one
 * per port) to the tq_cyclades work queue and sets a request flag
 * via mark_bh for processing that queue.  When the time is right,
 * do_cyclades_bh is called (because of the mark_bh) and it requests
 * that the work queue be processed.
 *
 * Although this may seem unwieldy, it gives the system a way to
 * pass an argument (in this case the pointer to the cyclades_port
 * structure) to the bottom half of the driver.  Previous kernels
 * had to poll every port to see if that port needed servicing.
 */
static void
do_cyclades_bh(void)
{
    run_task_queue(&tq_cyclades);
} /* do_cyclades_bh */

static void
do_softint(void *private_)
{
  struct cyclades_port *info = (struct cyclades_port *) private_;
  struct tty_struct    *tty;

    tty = info->tty;
    if (!tty)
        return;

    if (test_and_clear_bit(Cy_EVENT_HANGUP, &info->event)) {
        tty_hangup(info->tty);
        wake_up_interruptible(&info->open_wait);
        info->flags &= ~(ASYNC_NORMAL_ACTIVE|
                             ASYNC_CALLOUT_ACTIVE);
    }
    if (test_and_clear_bit(Cy_EVENT_OPEN_WAKEUP, &info->event)) {
        wake_up_interruptible(&info->open_wait);
    }
#ifdef CONFIG_CYZ_INTR
    if (test_and_clear_bit(Cy_EVENT_Z_RX_FULL, &info->event)) {
	if (cyz_rx_full_timer[info->line].function == NULL) {
	    cyz_rx_full_timer[info->line].expires = jiffies + 1;
	    cyz_rx_full_timer[info->line].function = cyz_rx_restart;
	    cyz_rx_full_timer[info->line].data = (unsigned long)info;
	    add_timer(&cyz_rx_full_timer[info->line]);
	}
    }
#endif
    if (test_and_clear_bit(Cy_EVENT_DELTA_WAKEUP, &info->event)) {
	wake_up_interruptible(&info->delta_msr_wait);
    }
    if (test_and_clear_bit(Cy_EVENT_WRITE_WAKEUP, &info->event)) {
        if((tty->flags & (1<< TTY_DO_WRITE_WAKEUP))
        && tty->ldisc.write_wakeup){
            (tty->ldisc.write_wakeup)(tty);
        }
        wake_up_interruptible(&tty->write_wait);
    }
#ifdef Z_WAKE
    if (test_and_clear_bit(Cy_EVENT_SHUTDOWN_WAKEUP, &info->event)) {
        wake_up_interruptible(&info->shutdown_wait);
    }
#endif
} /* do_softint */


/***********************************************************/
/********* Start of block of Cyclom-Y specific code ********/

/* This routine waits up to 1000 micro-seconds for the previous
   command to the Cirrus chip to complete and then issues the
   new command.  An error is returned if the previous command
   didn't finish within the time limit.

   This function is only called from inside spinlock-protected code.
 */
static int
cyy_issue_cmd(volatile ucchar *base_addr, u_char cmd, int index)
{
  volatile int  i;

    /* Check to see that the previous command has completed */
    for(i = 0 ; i < 100 ; i++){
	if (cy_readb(base_addr+(CyCCR<<index)) == 0){
	    break;
	}
	udelay(10L);
    }
    /* if the CCR never cleared, the previous command
       didn't finish within the "reasonable time" */
    if (i == 100)	return (-1);

    /* Issue the new command */
    cy_writeb((u_long)base_addr+(CyCCR<<index), cmd);

    return(0);
} /* cyy_issue_cmd */

#ifdef CONFIG_ISA
/* ISA interrupt detection code */
static unsigned 
detect_isa_irq (volatile ucchar *address)
{
  int irq;
  unsigned long irqs, flags;
  int save_xir, save_car;
  int index = 0; /* IRQ probing is only for ISA */

    /* forget possible initially masked and pending IRQ */
    irq = probe_irq_off(probe_irq_on());

    /* Clear interrupts on the board first */
    cy_writeb((u_long)address + (Cy_ClrIntr<<index), 0);
			      /* Cy_ClrIntr is 0x1800 */

    irqs = probe_irq_on();
    /* Wait ... */
    udelay(5000L);

    /* Enable the Tx interrupts on the CD1400 */
    save_flags(flags); cli();
	cy_writeb((u_long)address + (CyCAR<<index), 0);
	cyy_issue_cmd(address, CyCHAN_CTL|CyENB_XMTR, index);

	cy_writeb((u_long)address + (CyCAR<<index), 0);
	cy_writeb((u_long)address + (CySRER<<index), 
		cy_readb(address + (CySRER<<index)) | CyTxRdy);
    restore_flags(flags);

    /* Wait ... */
    udelay(5000L);

    /* Check which interrupt is in use */
    irq = probe_irq_off(irqs);

    /* Clean up */
    save_xir = (u_char) cy_readb(address + (CyTIR<<index));
    save_car = cy_readb(address + (CyCAR<<index));
    cy_writeb((u_long)address + (CyCAR<<index), (save_xir & 0x3));
    cy_writeb((u_long)address + (CySRER<<index),
	cy_readb(address + (CySRER<<index)) & ~CyTxRdy);
    cy_writeb((u_long)address + (CyTIR<<index), (save_xir & 0x3f));
    cy_writeb((u_long)address + (CyCAR<<index), (save_car));
    cy_writeb((u_long)address + (Cy_ClrIntr<<index), 0);
			      /* Cy_ClrIntr is 0x1800 */

    return (irq > 0)? irq : 0;
}
#endif /* CONFIG_ISA */

/* The real interrupt service routine is called
   whenever the card wants its hand held--chars
   received, out buffer empty, modem change, etc.
 */
static void
cyy_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
  struct tty_struct *tty;
  int status;
  struct cyclades_card *cinfo;
  struct cyclades_port *info;
  volatile unsigned char *base_addr, *card_base_addr;
  int chip;
  int save_xir, channel, save_car;
  char data;
  volatile int char_count;
  int outch;
  int i,j,index;
  int too_many;
  int had_work;
  int mdm_change;
  int mdm_status;

    if((cinfo = (struct cyclades_card *)dev_id) == 0){
#ifdef CY_DEBUG_INTERRUPTS
	printk("cyy_interrupt: spurious interrupt %d\n\r", irq);
#endif
        return; /* spurious interrupt */
    }

    card_base_addr = (unsigned char *)cinfo->base_addr;
    index = cinfo->bus_index;


    /* This loop checks all chips in the card.  Make a note whenever
       _any_ chip had some work to do, as this is considered an
       indication that there will be more to do.  Only when no chip
       has any work does this outermost loop exit.
     */
    do{
        had_work = 0;
        for ( chip = 0 ; chip < cinfo->num_chips ; chip ++) {
            base_addr = (unsigned char *)
		       (cinfo->base_addr + (cy_chip_offset[chip]<<index));
            too_many = 0;
            while ( (status = cy_readb(base_addr+(CySVRR<<index))) != 0x00) {
                had_work++;
                /* The purpose of the following test is to ensure that
                   no chip can monopolize the driver.  This forces the
                   chips to be checked in a round-robin fashion (after
                   draining each of a bunch (1000) of characters).
                 */
                if(1000<too_many++){
                    break;
                }
                if (status & CySRReceive) { /* reception interrupt */
#ifdef CY_DEBUG_INTERRUPTS
		    printk("cyy_interrupt: rcvd intr, chip %d\n\r", chip);
#endif
                    /* determine the channel & change to that context */
		    spin_lock(&cinfo->card_lock);
                    save_xir = (u_char) cy_readb(base_addr+(CyRIR<<index));
                    channel = (u_short ) (save_xir & CyIRChannel);
                    i = channel + chip * 4 + cinfo->first_line;
                    info = &cy_port[i];
                    info->last_active = jiffies;
                    save_car = cy_readb(base_addr+(CyCAR<<index));
                    cy_writeb((u_long)base_addr+(CyCAR<<index), save_xir);

                    /* if there is nowhere to put the data, discard it */
                    if(info->tty == 0){
                        j = (cy_readb(base_addr+(CyRIVR<<index)) & CyIVRMask);
                        if ( j == CyIVRRxEx ) { /* exception */
                            data = cy_readb(base_addr+(CyRDSR<<index));
                        } else { /* normal character reception */
                            char_count = cy_readb(base_addr+(CyRDCR<<index));
                            while(char_count--){
                                data = cy_readb(base_addr+(CyRDSR<<index));
                            }
                        }
                    }else{ /* there is an open port for this data */
                        tty = info->tty;
                        j = (cy_readb(base_addr+(CyRIVR<<index)) & CyIVRMask);
                        if ( j == CyIVRRxEx ) { /* exception */
                            data = cy_readb(base_addr+(CyRDSR<<index));

			    /* For statistics only */
			    if (data & CyBREAK)
				info->icount.brk++;
			    else if(data & CyFRAME)
				info->icount.frame++;
			    else if(data & CyPARITY)
				info->icount.parity++;
			    else if(data & CyOVERRUN)
				info->icount.overrun++;

                            if(data & info->ignore_status_mask){
				info->icount.rx++;
                                continue;
                            }
                            if (tty->flip.count < TTY_FLIPBUF_SIZE){
                                tty->flip.count++;
                                if (data & info->read_status_mask){
                                    if(data & CyBREAK){
                                        *tty->flip.flag_buf_ptr++ =
	    						    TTY_BREAK;
                                        *tty->flip.char_buf_ptr++ =
					  cy_readb(base_addr+(CyRDSR<<index));
					info->icount.rx++;
                                        if (info->flags & ASYNC_SAK){
                                            do_SAK(tty);
                                        }
                                    }else if(data & CyFRAME){
                                        *tty->flip.flag_buf_ptr++ =
							    TTY_FRAME;
                                        *tty->flip.char_buf_ptr++ =
					  cy_readb(base_addr+(CyRDSR<<index));
					info->icount.rx++;
					info->idle_stats.frame_errs++;
                                    }else if(data & CyPARITY){
                                        *tty->flip.flag_buf_ptr++ =
							    TTY_PARITY;
                                        *tty->flip.char_buf_ptr++ =
					  cy_readb(base_addr+(CyRDSR<<index));
					info->icount.rx++;
					info->idle_stats.parity_errs++;
                                    }else if(data & CyOVERRUN){
                                        *tty->flip.flag_buf_ptr++ =
							    TTY_OVERRUN;
                                        *tty->flip.char_buf_ptr++ = 0;
					info->icount.rx++;
                                        /* If the flip buffer itself is
                                           overflowing, we still lose
                                           the next incoming character.
                                         */
                                        if(tty->flip.count
					           < TTY_FLIPBUF_SIZE){
                                            tty->flip.count++;
                                            *tty->flip.flag_buf_ptr++ =
							     TTY_NORMAL;
                                           *tty->flip.char_buf_ptr++ =
					    cy_readb(base_addr+(CyRDSR<<index));
					    info->icount.rx++;
                                        }
					info->idle_stats.overruns++;
                                    /* These two conditions may imply */
                                    /* a normal read should be done. */
                                    /* }else if(data & CyTIMEOUT){ */
                                    /* }else if(data & CySPECHAR){ */
                                    }else{
                                        *tty->flip.flag_buf_ptr++ = 0;
                                        *tty->flip.char_buf_ptr++ = 0;
					info->icount.rx++;
                                    }
                                }else{
                                    *tty->flip.flag_buf_ptr++ = 0;
                                    *tty->flip.char_buf_ptr++ = 0;
				    info->icount.rx++;
                                }
                            }else{
                                /* there was a software buffer
				   overrun and nothing could be
				   done about it!!! */
				info->icount.buf_overrun++;
				info->idle_stats.overruns++;
                            }
                        } else { /* normal character reception */
                            /* load # chars available from the chip */
                            char_count = cy_readb(base_addr+(CyRDCR<<index));

#ifdef CY_ENABLE_MONITORING
                            ++info->mon.int_count;
                            info->mon.char_count += char_count;
                            if (char_count > info->mon.char_max)
                               info->mon.char_max = char_count;
                            info->mon.char_last = char_count;
#endif
			    info->idle_stats.recv_bytes += char_count;
			    info->idle_stats.recv_idle   = jiffies;
                            while(char_count--){
                                if (tty->flip.count >= TTY_FLIPBUF_SIZE){
                                        break;
                                }
                                tty->flip.count++;
                                data = cy_readb(base_addr+(CyRDSR<<index));
                                *tty->flip.flag_buf_ptr++ = TTY_NORMAL;
                                *tty->flip.char_buf_ptr++ = data;
				info->icount.rx++;
#ifdef CY_16Y_HACK
                                udelay(10L);
#endif
                            }
                        }
                        queue_task(&tty->flip.tqueue, &tq_timer);
                    }
                    /* end of service */
                    cy_writeb((u_long)base_addr+(CyRIR<<index), (save_xir & 0x3f));
                    cy_writeb((u_long)base_addr+(CyCAR<<index), (save_car));
		    spin_unlock(&cinfo->card_lock);
                }


                if (status & CySRTransmit) { /* transmission interrupt */
                    /* Since we only get here when the transmit buffer
                       is empty, we know we can always stuff a dozen
                       characters. */
#ifdef CY_DEBUG_INTERRUPTS
		    printk("cyy_interrupt: xmit intr, chip %d\n\r", chip);
#endif

                    /* determine the channel & change to that context */
		    spin_lock(&cinfo->card_lock);
                    save_xir = (u_char) cy_readb(base_addr+(CyTIR<<index));
                    channel = (u_short ) (save_xir & CyIRChannel);
                    i = channel + chip * 4 + cinfo->first_line;
                    save_car = cy_readb(base_addr+(CyCAR<<index));
                    cy_writeb((u_long)base_addr+(CyCAR<<index), save_xir);

                    /* validate the port# (as configured and open) */
                    if( (i < 0) || (NR_PORTS <= i) ){
                        cy_writeb((u_long)base_addr+(CySRER<<index),
                             cy_readb(base_addr+(CySRER<<index)) & ~CyTxRdy);
                        goto txend;
                    }
                    info = &cy_port[i];
                    info->last_active = jiffies;
                    if(info->tty == 0){
                        cy_writeb((u_long)base_addr+(CySRER<<index),
                             cy_readb(base_addr+(CySRER<<index)) & ~CyTxRdy);
                        goto txdone;
                    }

                    /* load the on-chip space for outbound data */
                    char_count = info->xmit_fifo_size;

                    if(info->x_char) { /* send special char */
                        outch = info->x_char;
                        cy_writeb((u_long)base_addr+(CyTDR<<index), outch);
                        char_count--;
			info->icount.tx++;
                        info->x_char = 0;
                    }

                    if (info->breakon || info->breakoff) {
			if (info->breakon) {
			    cy_writeb((u_long)base_addr + (CyTDR<<index), 0); 
			    cy_writeb((u_long)base_addr + (CyTDR<<index), 0x81);
			    info->breakon = 0;
                            char_count -= 2;
			}
			if (info->breakoff) {
			    cy_writeb((u_long)base_addr + (CyTDR<<index), 0); 
			    cy_writeb((u_long)base_addr + (CyTDR<<index), 0x83);
			    info->breakoff = 0;
                            char_count -= 2;
			}
                    }

                    while (char_count-- > 0){
			if (!info->xmit_cnt){
			    if (cy_readb(base_addr+(CySRER<<index))&CyTxMpty) {
				cy_writeb((u_long)base_addr+(CySRER<<index),
					  cy_readb(base_addr+(CySRER<<index)) &
					  ~CyTxMpty);
			    } else {
				cy_writeb((u_long)base_addr+(CySRER<<index),
					  ((cy_readb(base_addr+(CySRER<<index))
					    & ~CyTxRdy)
					   | CyTxMpty));
			    }
			    goto txdone;
			}
			if (info->xmit_buf == 0){
                            cy_writeb((u_long)base_addr+(CySRER<<index),
				cy_readb(base_addr+(CySRER<<index)) & 
					~CyTxRdy);
                            goto txdone;
			}
			if (info->tty->stopped || info->tty->hw_stopped){
                            cy_writeb((u_long)base_addr+(CySRER<<index),
				cy_readb(base_addr+(CySRER<<index)) & 
					~CyTxRdy);
                            goto txdone;
			}
                        /* Because the Embedded Transmit Commands have
                           been enabled, we must check to see if the
			   escape character, NULL, is being sent.  If it
			   is, we must ensure that there is room for it
			   to be doubled in the output stream.  Therefore
			   we no longer advance the pointer when the
			   character is fetched, but rather wait until
			   after the check for a NULL output character.
			   This is necessary because there may not be
			   room for the two chars needed to send a NULL.)
                         */
                        outch = info->xmit_buf[info->xmit_tail];
                        if( outch ){
                            info->xmit_cnt--;
                            info->xmit_tail = (info->xmit_tail + 1)
                                                      & (SERIAL_XMIT_SIZE - 1);
                            cy_writeb((u_long)base_addr+(CyTDR<<index), outch);
			    info->icount.tx++;
                        }else{
                            if(char_count > 1){
                                info->xmit_cnt--;
                                info->xmit_tail = (info->xmit_tail + 1)
						      & (SERIAL_XMIT_SIZE - 1);
                                cy_writeb((u_long)base_addr+(CyTDR<<index), 
					  outch);
                                cy_writeb((u_long)base_addr+(CyTDR<<index), 0);
				info->icount.tx++;
                                char_count--;
                            }else{
                            }
                        }
                    }

        txdone:
                    if (info->xmit_cnt < WAKEUP_CHARS) {
                        cy_sched_event(info, Cy_EVENT_WRITE_WAKEUP);
                    }
        txend:
                    /* end of service */
                    cy_writeb((u_long)base_addr+(CyTIR<<index), 
			      (save_xir & 0x3f));
                    cy_writeb((u_long)base_addr+(CyCAR<<index), (save_car));
		    spin_unlock(&cinfo->card_lock);
                }

                if (status & CySRModem) {        /* modem interrupt */

                    /* determine the channel & change to that context */
		    spin_lock(&cinfo->card_lock);
                    save_xir = (u_char) cy_readb(base_addr+(CyMIR<<index));
                    channel = (u_short ) (save_xir & CyIRChannel);
                    info = &cy_port[channel + chip * 4
		                           + cinfo->first_line];
                    info->last_active = jiffies;
                    save_car = cy_readb(base_addr+(CyCAR<<index));
                    cy_writeb((u_long)base_addr+(CyCAR<<index), save_xir);

                    mdm_change = cy_readb(base_addr+(CyMISR<<index));
                    mdm_status = cy_readb(base_addr+(CyMSVR1<<index));

                    if(info->tty == 0){/* no place for data, ignore it*/
                        ;
                    }else{
			if (mdm_change & CyANY_DELTA) {
			    /* For statistics only */
			    if (mdm_change & CyDCD)	info->icount.dcd++;
			    if (mdm_change & CyCTS)	info->icount.cts++;
			    if (mdm_change & CyDSR)	info->icount.dsr++;
			    if (mdm_change & CyRI)	info->icount.rng++;

			    cy_sched_event(info, Cy_EVENT_DELTA_WAKEUP);
			}

                        if((mdm_change & CyDCD)
                        && (info->flags & ASYNC_CHECK_CD)){
                            if(mdm_status & CyDCD){
                                cy_sched_event(info,
				    Cy_EVENT_OPEN_WAKEUP);
                            }else if(!((info->flags
			                & ASYNC_CALLOUT_ACTIVE)
				 &&(info->flags
				    & ASYNC_CALLOUT_NOHUP))){
                                cy_sched_event(info,
				    Cy_EVENT_HANGUP);
                            }
                        }
                        if((mdm_change & CyCTS)
                        && (info->flags & ASYNC_CTS_FLOW)){
                            if(info->tty->hw_stopped){
                                if(mdm_status & CyCTS){
                                    /* cy_start isn't used
				         because... !!! */
                                    info->tty->hw_stopped = 0;
                                  cy_writeb((u_long)base_addr+(CySRER<<index),
                                       cy_readb(base_addr+(CySRER<<index)) | 
                                       CyTxRdy);
                                    cy_sched_event(info,
				        Cy_EVENT_WRITE_WAKEUP);
                                }
                            }else{
                                if(!(mdm_status & CyCTS)){
                                    /* cy_stop isn't used
				         because ... !!! */
                                    info->tty->hw_stopped = 1;
                                  cy_writeb((u_long)base_addr+(CySRER<<index),
                                       cy_readb(base_addr+(CySRER<<index)) & 
                                       ~CyTxRdy);
                                }
                            }
                        }
                        if(mdm_change & CyDSR){
                        }
                        if(mdm_change & CyRI){
                        }
                    }
                    /* end of service */
                    cy_writeb((u_long)base_addr+(CyMIR<<index), 
			      (save_xir & 0x3f));
                    cy_writeb((u_long)base_addr+(CyCAR<<index), save_car);
		    spin_unlock(&cinfo->card_lock);
                }
            }          /* end while status != 0 */
        }            /* end loop for chips... */
    } while(had_work);

   /* clear interrupts */
   spin_lock(&cinfo->card_lock);
   cy_writeb((u_long)card_base_addr + (Cy_ClrIntr<<index), 0);
                                /* Cy_ClrIntr is 0x1800 */
   spin_unlock(&cinfo->card_lock);
} /* cyy_interrupt */

/***********************************************************/
/********* End of block of Cyclom-Y specific code **********/
/******** Start of block of Cyclades-Z specific code *********/
/***********************************************************/

static int
cyz_fetch_msg( struct cyclades_card *cinfo,
	    uclong *channel, ucchar *cmd, uclong *param)
{
  struct FIRM_ID *firm_id;
  struct ZFW_CTRL *zfw_ctrl;
  struct BOARD_CTRL *board_ctrl;
  unsigned long loc_doorbell;

    firm_id = (struct FIRM_ID *)(cinfo->base_addr + ID_ADDRESS);
    if (!ISZLOADED(*cinfo)){
	return (-1);
    }
    zfw_ctrl = (struct ZFW_CTRL *)
		(cinfo->base_addr + 
		 (cy_readl(&firm_id->zfwctrl_addr) & 0xfffff));
    board_ctrl = &zfw_ctrl->board_ctrl;

    loc_doorbell = cy_readl(&((struct RUNTIME_9060 *)
                     (cinfo->ctl_addr))->loc_doorbell);
    if (loc_doorbell){
	*cmd = (char)(0xff & loc_doorbell);
	*channel = cy_readl(&board_ctrl->fwcmd_channel);
	*param = (uclong)cy_readl(&board_ctrl->fwcmd_param);
	cy_writel(&((struct RUNTIME_9060 *)(cinfo->ctl_addr))->loc_doorbell, 
                 0xffffffff);
	return 1;
    }
    return 0;
} /* cyz_fetch_msg */

static int
cyz_issue_cmd( struct cyclades_card *cinfo,
	    uclong channel, ucchar cmd, uclong param)
{
  struct FIRM_ID *firm_id;
  struct ZFW_CTRL *zfw_ctrl;
  struct BOARD_CTRL *board_ctrl;
  volatile uclong *pci_doorbell;
  int index;

    firm_id = (struct FIRM_ID *)(cinfo->base_addr + ID_ADDRESS);
    if (!ISZLOADED(*cinfo)){
	return (-1);
    }
    zfw_ctrl = (struct ZFW_CTRL *)
		(cinfo->base_addr + 
		 (cy_readl(&firm_id->zfwctrl_addr) & 0xfffff));
    board_ctrl = &zfw_ctrl->board_ctrl;

    index = 0;
    pci_doorbell = (uclong *)(&((struct RUNTIME_9060 *)
                               (cinfo->ctl_addr))->pci_doorbell);
    while( (cy_readl(pci_doorbell) & 0xff) != 0){
        if (index++ == 1000){
	    return((int)(cy_readl(pci_doorbell) & 0xff));
        }
	udelay(50L);
    }
    cy_writel((u_long)&board_ctrl->hcmd_channel, channel);
    cy_writel((u_long)&board_ctrl->hcmd_param , param);
    cy_writel((u_long)pci_doorbell, (long)cmd);

    return(0);
} /* cyz_issue_cmd */

static void
cyz_handle_rx(struct cyclades_port *info, volatile struct CH_CTRL *ch_ctrl,
	      volatile struct BUF_CTRL *buf_ctrl)
{
  struct cyclades_card *cinfo = &cy_card[info->card];
  struct tty_struct *tty = info->tty;
  volatile int char_count;
#ifdef BLOCKMOVE
  int small_count;
#else
  char data;
#endif
  volatile uclong rx_put, rx_get, new_rx_get, rx_bufsize, rx_bufaddr;

    rx_get = new_rx_get = cy_readl(&buf_ctrl->rx_get);
    rx_put = cy_readl(&buf_ctrl->rx_put);
    rx_bufsize = cy_readl(&buf_ctrl->rx_bufsize);
    rx_bufaddr = cy_readl(&buf_ctrl->rx_bufaddr);
    if (rx_put >= rx_get)
	char_count = rx_put - rx_get;
    else
	char_count = rx_put - rx_get + rx_bufsize;

    if ( char_count ) {
	info->last_active = jiffies;
	info->jiffies[1] = jiffies;

#ifdef CY_ENABLE_MONITORING
	info->mon.int_count++;
	info->mon.char_count += char_count;
	if (char_count > info->mon.char_max)
	    info->mon.char_max = char_count;
	info->mon.char_last = char_count;
#endif
	if(tty == 0){
	    /* flush received characters */
	    new_rx_get = (new_rx_get + char_count) & (rx_bufsize - 1);
	    info->rflush_count++;
	}else{
#ifdef BLOCKMOVE
	    /* we'd like to use memcpy(t, f, n) and memset(s, c, count)
	       for performance, but because of buffer boundaries, there
	       may be several steps to the operation */
	    while(0 < (small_count = 
		       cy_min((rx_bufsize - new_rx_get),
		       cy_min((TTY_FLIPBUF_SIZE - tty->flip.count), char_count))
		 )) {
		memcpy_fromio(tty->flip.char_buf_ptr,
			      (char *)(cinfo->base_addr
				       + rx_bufaddr + new_rx_get),
			      small_count);

		tty->flip.char_buf_ptr += small_count;
		memset(tty->flip.flag_buf_ptr, TTY_NORMAL, small_count);
		tty->flip.flag_buf_ptr += small_count;
		new_rx_get = (new_rx_get + small_count) & (rx_bufsize - 1);
		char_count -= small_count;
		info->icount.rx += small_count;
		info->idle_stats.recv_bytes += small_count;
		tty->flip.count += small_count;
	    }
#else
	    while(char_count--){
		if (tty->flip.count >= TTY_FLIPBUF_SIZE){
		    break;
		}
		data = cy_readb(cinfo->base_addr + rx_bufaddr + new_rx_get);
		new_rx_get = (new_rx_get + 1) & (rx_bufsize - 1);
		tty->flip.count++;
		*tty->flip.flag_buf_ptr++ = TTY_NORMAL;
		*tty->flip.char_buf_ptr++ = data;
		info->idle_stats.recv_bytes++;
		info->icount.rx++;
	    }
#endif
#ifdef CONFIG_CYZ_INTR
	    /* Recalculate the number of chars in the RX buffer and issue
	       a cmd in case it's higher than the RX high water mark */
	    rx_put = cy_readl(&buf_ctrl->rx_put);
	    if (rx_put >= rx_get)
		char_count = rx_put - rx_get;
	    else
		char_count = rx_put - rx_get + rx_bufsize;
	    if(char_count >= cy_readl(&buf_ctrl->rx_threshold)) {
		cy_sched_event(info, Cy_EVENT_Z_RX_FULL);
	    }
#endif
	    info->idle_stats.recv_idle = jiffies;
	    queue_task(&tty->flip.tqueue, &tq_timer);
	}
	/* Update rx_get */
	cy_writel(&buf_ctrl->rx_get, new_rx_get);
    }
}

static void
cyz_handle_tx(struct cyclades_port *info, volatile struct CH_CTRL *ch_ctrl,
	      volatile struct BUF_CTRL *buf_ctrl)
{
  struct cyclades_card *cinfo = &cy_card[info->card];
  struct tty_struct *tty = info->tty;
  char data;
  volatile int char_count;
#ifdef BLOCKMOVE
  int small_count;
#endif
  volatile uclong tx_put, tx_get, tx_bufsize, tx_bufaddr;

    if (info->xmit_cnt <= 0)	/* Nothing to transmit */
	return;

    tx_get = cy_readl(&buf_ctrl->tx_get);
    tx_put = cy_readl(&buf_ctrl->tx_put);
    tx_bufsize = cy_readl(&buf_ctrl->tx_bufsize);
    tx_bufaddr = cy_readl(&buf_ctrl->tx_bufaddr);
    if (tx_put >= tx_get)
	char_count = tx_get - tx_put - 1 + tx_bufsize;
    else
	char_count = tx_get - tx_put - 1;

    if ( char_count ) {

	if( tty == 0 ){
	    goto ztxdone;
	}

	if(info->x_char) { /* send special char */
	    data = info->x_char;

	    cy_writeb((cinfo->base_addr + tx_bufaddr + tx_put), data);
	    tx_put = (tx_put + 1) & (tx_bufsize - 1);
	    info->x_char = 0;
	    char_count--;
	    info->icount.tx++;
	    info->last_active = jiffies;
	    info->jiffies[2] = jiffies;
	}
#ifdef BLOCKMOVE
	while(0 < (small_count = 
		   cy_min((tx_bufsize - tx_put),
		   cy_min ((SERIAL_XMIT_SIZE - info->xmit_tail),
			cy_min(info->xmit_cnt, char_count))))){

	    memcpy_toio((char *)(cinfo->base_addr + tx_bufaddr + tx_put),
			&info->xmit_buf[info->xmit_tail],
			small_count);

	    tx_put = (tx_put + small_count) & (tx_bufsize - 1);
	    char_count -= small_count;
	    info->icount.tx += small_count;
	    info->xmit_cnt -= small_count;
	    info->xmit_tail = 
		(info->xmit_tail + small_count) & (SERIAL_XMIT_SIZE - 1);
	    info->last_active = jiffies;
	    info->jiffies[2] = jiffies;
	}
#else
	while (info->xmit_cnt && char_count){
	    data = info->xmit_buf[info->xmit_tail];
	    info->xmit_cnt--;
	    info->xmit_tail = (info->xmit_tail + 1) & (SERIAL_XMIT_SIZE - 1);

	    cy_writeb(cinfo->base_addr + tx_bufaddr + tx_put, data);
	    tx_put = (tx_put + 1) & (tx_bufsize - 1);
	    char_count--;
	    info->icount.tx++;
	    info->last_active = jiffies;
	    info->jiffies[2] = jiffies;
	}
#endif
    ztxdone:
	if (info->xmit_cnt < WAKEUP_CHARS) {
	    cy_sched_event(info, Cy_EVENT_WRITE_WAKEUP);
	}
	/* Update tx_put */
	cy_writel(&buf_ctrl->tx_put, tx_put);
    }
}

static void
cyz_handle_cmd(struct cyclades_card *cinfo)
{
  struct tty_struct *tty;
  struct cyclades_port *info;
  static volatile struct FIRM_ID *firm_id;
  static volatile struct ZFW_CTRL *zfw_ctrl;
  static volatile struct BOARD_CTRL *board_ctrl;
  static volatile struct CH_CTRL *ch_ctrl;
  static volatile struct BUF_CTRL *buf_ctrl;
  uclong channel;
  ucchar cmd;
  uclong param;
  uclong hw_ver, fw_ver;
  int special_count;
  int delta_count;

    firm_id = (struct FIRM_ID *)(cinfo->base_addr + ID_ADDRESS);
    zfw_ctrl = (struct ZFW_CTRL *)
		(cinfo->base_addr + 
		 (cy_readl(&firm_id->zfwctrl_addr) & 0xfffff));
    board_ctrl = &(zfw_ctrl->board_ctrl);
    fw_ver = cy_readl(&board_ctrl->fw_version);
    hw_ver = cy_readl(&((struct RUNTIME_9060 *)(cinfo->ctl_addr))->mail_box_0);

#ifdef CONFIG_CYZ_INTR
    if (!cinfo->nports)
	cinfo->nports = (int) cy_readl(&board_ctrl->n_channel);
#endif

    while(cyz_fetch_msg(cinfo, &channel, &cmd, &param) == 1) {
	special_count = 0;
	delta_count = 0;
	info = &cy_port[channel + cinfo->first_line];
	if((tty = info->tty) == 0) {
	    continue;
	}
	ch_ctrl = &(zfw_ctrl->ch_ctrl[channel]);
	buf_ctrl = &(zfw_ctrl->buf_ctrl[channel]);

	switch(cmd) {
	    case C_CM_PR_ERROR:
		tty->flip.count++;
		*tty->flip.flag_buf_ptr++ = TTY_PARITY;
		*tty->flip.char_buf_ptr++ = 0;
		info->icount.rx++;
		special_count++;
		break;
	    case C_CM_FR_ERROR:
		tty->flip.count++;
		*tty->flip.flag_buf_ptr++ = TTY_FRAME;
		*tty->flip.char_buf_ptr++ = 0;
		info->icount.rx++;
		special_count++;
		break;
	    case C_CM_RXBRK:
		tty->flip.count++;
		*tty->flip.flag_buf_ptr++ = TTY_BREAK;
		*tty->flip.char_buf_ptr++ = 0;
		info->icount.rx++;
		special_count++;
		break;
	    case C_CM_MDCD:
		info->icount.dcd++;
		delta_count++;
		if (info->flags & ASYNC_CHECK_CD){
		    if ((fw_ver > 241 ? 
			  ((u_long)param) : 
			  cy_readl(&ch_ctrl->rs_status)) & C_RS_DCD) {
			cy_sched_event(info, Cy_EVENT_OPEN_WAKEUP);
		    }else if(!((info->flags & ASYNC_CALLOUT_ACTIVE)
			     &&(info->flags & ASYNC_CALLOUT_NOHUP))){
			cy_sched_event(info, Cy_EVENT_HANGUP);
		    }
		}
		break;
	    case C_CM_MCTS:
		info->icount.cts++;
		delta_count++;
		break;
	    case C_CM_MRI:
		info->icount.rng++;
		delta_count++;
		break;
	    case C_CM_MDSR:
		info->icount.dsr++;
		delta_count++;
		break;
#ifdef Z_WAKE
	    case C_CM_IOCTLW:
		cy_sched_event(info, Cy_EVENT_SHUTDOWN_WAKEUP);
		break;
#endif
#ifdef CONFIG_CYZ_INTR
	    case C_CM_RXHIWM:
	    case C_CM_RXNNDT:
	    case C_CM_INTBACK2:
		/* Reception Interrupt */
#ifdef CY_DEBUG_INTERRUPTS
		printk("cyz_interrupt: rcvd intr, card %d, port %ld\n\r", 
			info->card, channel);
#endif
		cyz_handle_rx(info, ch_ctrl, buf_ctrl);
		break;
	    case C_CM_TXBEMPTY:
	    case C_CM_TXLOWWM:
	    case C_CM_INTBACK:
		/* Transmission Interrupt */
#ifdef CY_DEBUG_INTERRUPTS
		printk("cyz_interrupt: xmit intr, card %d, port %ld\n\r", 
			info->card, channel);
#endif
		cyz_handle_tx(info, ch_ctrl, buf_ctrl);
		break;
#endif /* CONFIG_CYZ_INTR */
	    case C_CM_FATAL:
		/* should do something with this !!! */
		break;
	    default:
		break;
	}
	if(delta_count)
	    cy_sched_event(info, Cy_EVENT_DELTA_WAKEUP);
	if(special_count)
	    queue_task(&tty->flip.tqueue, &tq_timer);
    }
}

#ifdef CONFIG_CYZ_INTR
static void
cyz_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
  struct cyclades_card *cinfo;

    if((cinfo = (struct cyclades_card *)dev_id) == 0){
#ifdef CY_DEBUG_INTERRUPTS
	printk("cyz_interrupt: spurious interrupt %d\n\r", irq);
#endif
        return; /* spurious interrupt */
    }

    if (!ISZLOADED(*cinfo)) {
#ifdef CY_DEBUG_INTERRUPTS
	printk("cyz_interrupt: board not yet loaded (IRQ%d).\n\r", irq);
#endif
	return;
    }

    /* Handle the interrupts */
    cyz_handle_cmd(cinfo);

    return;
} /* cyz_interrupt */

static void
cyz_rx_restart(unsigned long arg)
{
    struct cyclades_port *info = (struct cyclades_port *)arg;
    int retval;
    int card = info->card;
    uclong channel = (info->line) - (cy_card[card].first_line);
    unsigned long flags;

    CY_LOCK(info, flags);
    retval = cyz_issue_cmd(&cy_card[card], channel, C_CM_INTBACK2, 0L);
    if (retval != 0){
	printk("cyc:cyz_rx_restart retval on ttyC%d was %x\n", 
	       info->line, retval);
    }
    cyz_rx_full_timer[info->line].function = NULL;
    CY_UNLOCK(info, flags);
}

#else /* CONFIG_CYZ_INTR */

static void
cyz_poll(unsigned long arg)
{
  struct cyclades_card *cinfo;
  struct cyclades_port *info;
  struct tty_struct *tty;
  static volatile struct FIRM_ID *firm_id;
  static volatile struct ZFW_CTRL *zfw_ctrl;
  static volatile struct BOARD_CTRL *board_ctrl;
  static volatile struct CH_CTRL *ch_ctrl;
  static volatile struct BUF_CTRL *buf_ctrl;
  int card, port;

    cyz_timerlist.expires = jiffies + (HZ);
    for (card = 0 ; card < NR_CARDS ; card++){
	cinfo = &cy_card[card];

	if (!IS_CYC_Z(*cinfo)) continue;
	if (!ISZLOADED(*cinfo)) continue;

	firm_id = (struct FIRM_ID *)(cinfo->base_addr + ID_ADDRESS);
	zfw_ctrl = (struct ZFW_CTRL *)
		    (cinfo->base_addr + 
		     (cy_readl(&firm_id->zfwctrl_addr) & 0xfffff));
	board_ctrl = &(zfw_ctrl->board_ctrl);

	/* Skip first polling cycle to avoid racing conditions with the FW */
	if (!cinfo->intr_enabled) {
	    cinfo->nports = (int) cy_readl(&board_ctrl->n_channel);
	    cinfo->intr_enabled = 1;
	    continue;
	}

	cyz_handle_cmd(cinfo);

	for (port = 0 ; port < cinfo->nports ; port++) {
	    info = &cy_port[ port + cinfo->first_line ];
            tty = info->tty;
	    ch_ctrl = &(zfw_ctrl->ch_ctrl[port]);
	    buf_ctrl = &(zfw_ctrl->buf_ctrl[port]);

	    cyz_handle_rx(info, ch_ctrl, buf_ctrl);
	    cyz_handle_tx(info, ch_ctrl, buf_ctrl);
	}
	/* poll every 'cyz_polling_cycle' period */
	cyz_timerlist.expires = jiffies + cyz_polling_cycle;
    }
    add_timer(&cyz_timerlist);

    return;
} /* cyz_poll */

#endif /* CONFIG_CYZ_INTR */

/********** End of block of Cyclades-Z specific code *********/
/***********************************************************/


/* This is called whenever a port becomes active;
   interrupts are enabled and DTR & RTS are turned on.
 */
static int
startup(struct cyclades_port * info)
{
  unsigned long flags;
  int retval = 0;
  unsigned char *base_addr;
  int card,chip,channel,index;
  unsigned long page;

    card = info->card;
    channel = (info->line) - (cy_card[card].first_line);

    page = get_free_page(GFP_KERNEL);
    if (!page)
	return -ENOMEM;

    CY_LOCK(info, flags);

    if (info->flags & ASYNC_INITIALIZED){
	free_page(page);
	goto errout;
    }

    if (!info->type){
        if (info->tty){
            set_bit(TTY_IO_ERROR, &info->tty->flags);
        }
	free_page(page);
	goto errout;
    }

    if (info->xmit_buf)
	free_page(page);
    else
	info->xmit_buf = (unsigned char *) page;

    CY_UNLOCK(info, flags);

    set_line_char(info);

    if (!IS_CYC_Z(cy_card[card])) {
	chip = channel>>2;
	channel &= 0x03;
	index = cy_card[card].bus_index;
	base_addr = (unsigned char*)
		   (cy_card[card].base_addr + (cy_chip_offset[chip]<<index));

#ifdef CY_DEBUG_OPEN
	printk("cyc startup card %d, chip %d, channel %d, base_addr %lx\n",
	     card, chip, channel, (long)base_addr);/**/
#endif

	CY_LOCK(info, flags);

	cy_writeb((ulong)base_addr+(CyCAR<<index), (u_char)channel);

	cy_writeb((ulong)base_addr+(CyRTPR<<index), (info->default_timeout
		 ? info->default_timeout : 0x02)); /* 10ms rx timeout */

	cyy_issue_cmd(base_addr,CyCHAN_CTL|CyENB_RCVR|CyENB_XMTR,index);

	cy_writeb((ulong)base_addr+(CyCAR<<index), (u_char)channel);
	cy_writeb((ulong)base_addr+(CyMSVR1<<index), CyRTS);
	cy_writeb((ulong)base_addr+(CyMSVR2<<index), CyDTR);

#ifdef CY_DEBUG_DTR
	printk("cyc:startup raising DTR\n");
	printk("     status: 0x%x, 0x%x\n",
		cy_readb(base_addr+(CyMSVR1<<index)), 
                cy_readb(base_addr+(CyMSVR2<<index)));
#endif

	cy_writeb((u_long)base_addr+(CySRER<<index),
		cy_readb(base_addr+(CySRER<<index)) | CyRxData);
	info->flags |= ASYNC_INITIALIZED;

	if (info->tty){
	    clear_bit(TTY_IO_ERROR, &info->tty->flags);
	}
	info->xmit_cnt = info->xmit_head = info->xmit_tail = 0;
	info->breakon = info->breakoff = 0;
	memset((char *)&info->idle_stats, 0, sizeof(info->idle_stats));
	info->idle_stats.in_use    =
	info->idle_stats.recv_idle =
	info->idle_stats.xmit_idle = jiffies;

	CY_UNLOCK(info, flags);

    } else {
      struct FIRM_ID *firm_id;
      struct ZFW_CTRL *zfw_ctrl;
      struct BOARD_CTRL *board_ctrl;
      struct CH_CTRL *ch_ctrl;
      int retval;

	base_addr = (unsigned char*) (cy_card[card].base_addr);

        firm_id = (struct FIRM_ID *) (base_addr + ID_ADDRESS);
        if (!ISZLOADED(cy_card[card])){
	    return -ENODEV;
	}

	zfw_ctrl = (struct ZFW_CTRL *)
		    (cy_card[card].base_addr + 
		     (cy_readl(&firm_id->zfwctrl_addr) & 0xfffff));
	board_ctrl = &zfw_ctrl->board_ctrl;
	ch_ctrl = zfw_ctrl->ch_ctrl;

#ifdef CY_DEBUG_OPEN
	printk("cyc startup Z card %d, channel %d, base_addr %lx\n",
	     card, channel, (long)base_addr);/**/
#endif

	CY_LOCK(info, flags);

	cy_writel(&ch_ctrl[channel].op_mode, C_CH_ENABLE);
#ifdef Z_WAKE
#ifdef CONFIG_CYZ_INTR
	cy_writel(&ch_ctrl[channel].intr_enable, 
		  C_IN_TXBEMPTY|C_IN_TXLOWWM|C_IN_RXHIWM|C_IN_RXNNDT|
		  C_IN_IOCTLW|
		  C_IN_MDCD);
#else
	cy_writel(&ch_ctrl[channel].intr_enable, 
		  C_IN_IOCTLW|
		  C_IN_MDCD);
#endif /* CONFIG_CYZ_INTR */
#else
#ifdef CONFIG_CYZ_INTR
	cy_writel(&ch_ctrl[channel].intr_enable, 
		  C_IN_TXBEMPTY|C_IN_TXLOWWM|C_IN_RXHIWM|C_IN_RXNNDT|
		  C_IN_MDCD);
#else
	cy_writel(&ch_ctrl[channel].intr_enable, 
		  C_IN_MDCD);
#endif /* CONFIG_CYZ_INTR */
#endif /* Z_WAKE */

	retval = cyz_issue_cmd(&cy_card[card], channel, C_CM_IOCTL, 0L);
	if (retval != 0){
	    printk("cyc:startup(1) retval on ttyC%d was %x\n",
		   info->line, retval);
	}

	/* Flush RX buffers before raising DTR and RTS */
	retval = cyz_issue_cmd(&cy_card[card], channel, C_CM_FLUSH_RX, 0L);
	if (retval != 0){
	    printk("cyc:startup(2) retval on ttyC%d was %x\n",
		   info->line, retval);
	}

	/* set timeout !!! */
	/* set RTS and DTR !!! */
	cy_writel(&ch_ctrl[channel].rs_control,
             cy_readl(&ch_ctrl[channel].rs_control) | C_RS_RTS | C_RS_DTR) ;
	retval = cyz_issue_cmd(&cy_card[info->card],
	    channel, C_CM_IOCTLM, 0L);
	if (retval != 0){
	    printk("cyc:startup(3) retval on ttyC%d was %x\n",
		   info->line, retval);
	}
#ifdef CY_DEBUG_DTR
	    printk("cyc:startup raising Z DTR\n");
#endif

	/* enable send, recv, modem !!! */

	info->flags |= ASYNC_INITIALIZED;
	if (info->tty){
	    clear_bit(TTY_IO_ERROR, &info->tty->flags);
	}
	info->xmit_cnt = info->xmit_head = info->xmit_tail = 0;
	info->breakon = info->breakoff = 0;
	memset((char *)&info->idle_stats, 0, sizeof(info->idle_stats));
	info->idle_stats.in_use    =
	info->idle_stats.recv_idle =
	info->idle_stats.xmit_idle = jiffies;

	CY_UNLOCK(info, flags);
    }

#ifdef CY_DEBUG_OPEN
	printk(" cyc startup done\n");
#endif
	return 0;

errout:
	CY_UNLOCK(info, flags);
	return retval;
} /* startup */


static void
start_xmit( struct cyclades_port *info )
{
  unsigned long flags;
  unsigned char *base_addr;
  int card,chip,channel,index;

    card = info->card;
    channel = (info->line) - (cy_card[card].first_line);
    if (!IS_CYC_Z(cy_card[card])) {
	chip = channel>>2;
	channel &= 0x03;
	index = cy_card[card].bus_index;
	base_addr = (unsigned char*)
		       (cy_card[card].base_addr
		       + (cy_chip_offset[chip]<<index));

	CY_LOCK(info, flags);
	    cy_writeb((u_long)base_addr+(CyCAR<<index), channel);
	    cy_writeb((u_long)base_addr+(CySRER<<index), 
               cy_readb(base_addr+(CySRER<<index)) | CyTxRdy);
	CY_UNLOCK(info, flags);
    } else {
#ifdef CONFIG_CYZ_INTR
      int retval;

	CY_LOCK(info, flags);
	    retval = cyz_issue_cmd(&cy_card[card], channel, C_CM_INTBACK, 0L);
	    if (retval != 0){
		printk("cyc:start_xmit retval on ttyC%d was %x\n",
		       info->line, retval);
	    }
	CY_UNLOCK(info, flags);
#else /* CONFIG_CYZ_INTR */
	/* Don't have to do anything at this time */
#endif /* CONFIG_CYZ_INTR */
    }
} /* start_xmit */

/*
 * This routine shuts down a serial port; interrupts are disabled,
 * and DTR is dropped if the hangup on close termio flag is on.
 */
static void
shutdown(struct cyclades_port * info)
{
  unsigned long flags;
  unsigned char *base_addr;
  int card,chip,channel,index;

    if (!(info->flags & ASYNC_INITIALIZED)){
        return;
    }

    card = info->card;
    channel = info->line - cy_card[card].first_line;
    if (!IS_CYC_Z(cy_card[card])) {
	chip = channel>>2;
	channel &= 0x03;
	index = cy_card[card].bus_index;
	base_addr = (unsigned char*)
		       (cy_card[card].base_addr
		       + (cy_chip_offset[chip]<<index));

#ifdef CY_DEBUG_OPEN
    printk("cyc shutdown Y card %d, chip %d, channel %d, base_addr %lx\n",
		card, chip, channel, (long)base_addr);
#endif

	CY_LOCK(info, flags);

	    /* Clear delta_msr_wait queue to avoid mem leaks. */
	    wake_up_interruptible(&info->delta_msr_wait);

	    if (info->xmit_buf){
		unsigned char * temp;
		temp = info->xmit_buf;
		info->xmit_buf = 0;
		free_page((unsigned long) temp);
	    }
	    cy_writeb((u_long)base_addr+(CyCAR<<index), (u_char)channel);
	    if (!info->tty || (info->tty->termios->c_cflag & HUPCL)) {
		cy_writeb((u_long)base_addr+(CyMSVR1<<index), ~CyRTS);
		cy_writeb((u_long)base_addr+(CyMSVR2<<index), ~CyDTR);
#ifdef CY_DEBUG_DTR
		printk("cyc shutdown dropping DTR\n");
		printk("     status: 0x%x, 0x%x\n",
		    cy_readb(base_addr+(CyMSVR1<<index)), 
                    cy_readb(base_addr+(CyMSVR2<<index)));
#endif
	    }
	    cyy_issue_cmd(base_addr,CyCHAN_CTL|CyDIS_RCVR,index);
	     /* it may be appropriate to clear _XMIT at
	       some later date (after testing)!!! */

	    if (info->tty){
		set_bit(TTY_IO_ERROR, &info->tty->flags);
	    }
	    info->flags &= ~ASYNC_INITIALIZED;
	CY_UNLOCK(info, flags);
    } else {
      struct FIRM_ID *firm_id;
      struct ZFW_CTRL *zfw_ctrl;
      struct BOARD_CTRL *board_ctrl;
      struct CH_CTRL *ch_ctrl;
      int retval;

	base_addr = (unsigned char*) (cy_card[card].base_addr);
#ifdef CY_DEBUG_OPEN
    printk("cyc shutdown Z card %d, channel %d, base_addr %lx\n",
		card, channel, (long)base_addr);
#endif

        firm_id = (struct FIRM_ID *) (base_addr + ID_ADDRESS);
        if (!ISZLOADED(cy_card[card])) {
	    return;
	}

	zfw_ctrl = (struct ZFW_CTRL *)
		    (cy_card[card].base_addr + 
		     (cy_readl(&firm_id->zfwctrl_addr) & 0xfffff));
	board_ctrl = &(zfw_ctrl->board_ctrl);
	ch_ctrl = zfw_ctrl->ch_ctrl;

	CY_LOCK(info, flags);

	    if (info->xmit_buf){
		unsigned char * temp;
		temp = info->xmit_buf;
		info->xmit_buf = 0;
		free_page((unsigned long) temp);
	    }
	    
	    if (!info->tty || (info->tty->termios->c_cflag & HUPCL)) {
		cy_writel((u_long)&ch_ctrl[channel].rs_control,
                   (uclong)(cy_readl(&ch_ctrl[channel].rs_control) & 
                   ~(C_RS_RTS | C_RS_DTR)));
		retval = cyz_issue_cmd(&cy_card[info->card],
			channel, C_CM_IOCTLM, 0L);
		if (retval != 0){
		    printk("cyc:shutdown retval on ttyC%d was %x\n",
			   info->line, retval);
		}
#ifdef CY_DEBUG_DTR
		printk("cyc:shutdown dropping Z DTR\n");
#endif
	    }
	    
	    if (info->tty){
		set_bit(TTY_IO_ERROR, &info->tty->flags);
	    }
	    info->flags &= ~ASYNC_INITIALIZED;

	CY_UNLOCK(info, flags);
    }

#ifdef CY_DEBUG_OPEN
    printk(" cyc shutdown done\n");
#endif
    return;
} /* shutdown */


/*
 * ------------------------------------------------------------
 * cy_open() and friends
 * ------------------------------------------------------------
 */

static int
block_til_ready(struct tty_struct *tty, struct file * filp,
                           struct cyclades_port *info)
{
  DECLARE_WAITQUEUE(wait, current);
  struct cyclades_card *cinfo;
  unsigned long flags;
  int chip, channel,index;
  int retval;
  char *base_addr;

    cinfo = &cy_card[info->card];
    channel = info->line - cinfo->first_line;

    /*
     * If the device is in the middle of being closed, then block
     * until it's done, and then try again.
     */
    if (tty_hung_up_p(filp) || (info->flags & ASYNC_CLOSING)) {
	if (info->flags & ASYNC_CLOSING) {
            interruptible_sleep_on(&info->close_wait);
	}
        return ((info->flags & ASYNC_HUP_NOTIFY) ? -EAGAIN : -ERESTARTSYS);
    }

    /*
     * If this is a callout device, then just make sure the normal
     * device isn't being used.
     */
    if (tty->driver.subtype == SERIAL_TYPE_CALLOUT) {
        if (info->flags & ASYNC_NORMAL_ACTIVE){
            return -EBUSY;
        }
        if ((info->flags & ASYNC_CALLOUT_ACTIVE) &&
            (info->flags & ASYNC_SESSION_LOCKOUT) &&
            (info->session != current->session)){
            return -EBUSY;
        }
        if ((info->flags & ASYNC_CALLOUT_ACTIVE) &&
            (info->flags & ASYNC_PGRP_LOCKOUT) &&
            (info->pgrp != current->pgrp)){
            return -EBUSY;
        }
        info->flags |= ASYNC_CALLOUT_ACTIVE;
        return 0;
    }

    /*
     * If non-blocking mode is set, then make the check up front
     * and then exit.
     */
    if ((filp->f_flags & O_NONBLOCK) ||
	(tty->flags & (1 << TTY_IO_ERROR))) {
        if (info->flags & ASYNC_CALLOUT_ACTIVE){
            return -EBUSY;
        }
        info->flags |= ASYNC_NORMAL_ACTIVE;
        return 0;
    }

    /*
     * Block waiting for the carrier detect and the line to become
     * free (i.e., not in use by the callout).  While we are in
     * this loop, info->count is dropped by one, so that
     * cy_close() knows when to free things.  We restore it upon
     * exit, either normal or abnormal.
     */
    retval = 0;
    add_wait_queue(&info->open_wait, &wait);
#ifdef CY_DEBUG_OPEN
    printk("cyc block_til_ready before block: ttyC%d, count = %d\n",
           info->line, info->count);/**/
#endif
    CY_LOCK(info, flags);
    if (!tty_hung_up_p(filp))
	info->count--;
    CY_UNLOCK(info, flags);
#ifdef CY_DEBUG_COUNT
    printk("cyc block_til_ready: (%d): decrementing count to %d\n",
        current->pid, info->count);
#endif
    info->blocked_open++;

    if (!IS_CYC_Z(*cinfo)) {
	chip = channel>>2;
	channel &= 0x03;
	index = cinfo->bus_index;
	base_addr = (char *)(cinfo->base_addr
			    + (cy_chip_offset[chip]<<index));

	while (1) {
	    CY_LOCK(info, flags);
		if (!(info->flags & ASYNC_CALLOUT_ACTIVE) &&
		    (tty->termios->c_cflag & CBAUD)){
		    cy_writeb((u_long)base_addr+(CyCAR<<index), (u_char)channel);
		    cy_writeb((u_long)base_addr+(CyMSVR1<<index), CyRTS);
		    cy_writeb((u_long)base_addr+(CyMSVR2<<index), CyDTR);
#ifdef CY_DEBUG_DTR
		    printk("cyc:block_til_ready raising DTR\n");
		    printk("     status: 0x%x, 0x%x\n",
			cy_readb(base_addr+(CyMSVR1<<index)), 
                        cy_readb(base_addr+(CyMSVR2<<index)));
#endif
		}
	    CY_UNLOCK(info, flags);

	    set_current_state(TASK_INTERRUPTIBLE);
	    if (tty_hung_up_p(filp)
	    || !(info->flags & ASYNC_INITIALIZED) ){
		retval = ((info->flags & ASYNC_HUP_NOTIFY) ? 
		    -EAGAIN : -ERESTARTSYS);
		break;
	    }

	    CY_LOCK(info, flags);
		cy_writeb((u_long)base_addr+(CyCAR<<index), (u_char)channel);
		if (!(info->flags & ASYNC_CALLOUT_ACTIVE)
		&& !(info->flags & ASYNC_CLOSING)
		&& (C_CLOCAL(tty)
		    || (cy_readb(base_addr+(CyMSVR1<<index)) & CyDCD))) {
			CY_UNLOCK(info, flags);
			break;
		}
	    CY_UNLOCK(info, flags);

	    if (signal_pending(current)) {
		retval = -ERESTARTSYS;
		break;
	    }
#ifdef CY_DEBUG_OPEN
	    printk("cyc block_til_ready blocking: ttyC%d, count = %d\n",
		   info->line, info->count);/**/
#endif
	    schedule();
	}
    } else {
      struct FIRM_ID *firm_id;
      struct ZFW_CTRL *zfw_ctrl;
      struct BOARD_CTRL *board_ctrl;
      struct CH_CTRL *ch_ctrl;
      int retval;

	base_addr = (char *)(cinfo->base_addr);
	firm_id = (struct FIRM_ID *)
			(base_addr + ID_ADDRESS);
        if (!ISZLOADED(*cinfo)){
            current->state = TASK_RUNNING;
	    remove_wait_queue(&info->open_wait, &wait);
	    return -EINVAL;
	}

	zfw_ctrl = (struct ZFW_CTRL *)
		    (base_addr + (cy_readl(&firm_id->zfwctrl_addr) & 0xfffff));
	board_ctrl = &zfw_ctrl->board_ctrl;
	ch_ctrl = zfw_ctrl->ch_ctrl;

	while (1) {
	    if (!(info->flags & ASYNC_CALLOUT_ACTIVE) &&
		(tty->termios->c_cflag & CBAUD)){
		cy_writel(&ch_ctrl[channel].rs_control,
			cy_readl(&ch_ctrl[channel].rs_control) |
			(C_RS_RTS | C_RS_DTR));
		retval = cyz_issue_cmd(&cy_card[info->card],
				       channel, C_CM_IOCTLM, 0L);
		if (retval != 0){
		    printk("cyc:block_til_ready retval on ttyC%d was %x\n",
			   info->line, retval);
		}
#ifdef CY_DEBUG_DTR
		printk("cyc:block_til_ready raising Z DTR\n");
#endif
	    }

	    set_current_state(TASK_INTERRUPTIBLE);
	    if (tty_hung_up_p(filp)
	    || !(info->flags & ASYNC_INITIALIZED) ){
		retval = ((info->flags & ASYNC_HUP_NOTIFY) ?
		    -EAGAIN : -ERESTARTSYS);
		break;
	    }
	    if (!(info->flags & ASYNC_CALLOUT_ACTIVE)
	    && !(info->flags & ASYNC_CLOSING)
	    && (C_CLOCAL(tty)
	      || (cy_readl(&ch_ctrl[channel].rs_status) & C_RS_DCD))) {
		break;
	    }
	    if (signal_pending(current)) {
		retval = -ERESTARTSYS;
		break;
	    }
#ifdef CY_DEBUG_OPEN
	    printk("cyc block_til_ready blocking: ttyC%d, count = %d\n",
		   info->line, info->count);/**/
#endif
	    schedule();
	}
    }
    current->state = TASK_RUNNING;
    remove_wait_queue(&info->open_wait, &wait);
    if (!tty_hung_up_p(filp)){
	info->count++;
#ifdef CY_DEBUG_COUNT
	printk("cyc:block_til_ready (%d): incrementing count to %d\n",
	    current->pid, info->count);
#endif
    }
    info->blocked_open--;
#ifdef CY_DEBUG_OPEN
    printk("cyc:block_til_ready after blocking: ttyC%d, count = %d\n",
	   info->line, info->count);/**/
#endif
    if (retval)
	return retval;
    info->flags |= ASYNC_NORMAL_ACTIVE;
    return 0;
} /* block_til_ready */


/*
 * This routine is called whenever a serial port is opened.  It
 * performs the serial-specific initialization for the tty structure.
 */
static int
cy_open(struct tty_struct *tty, struct file * filp)
{
  struct cyclades_port  *info;
  int retval, line;
  unsigned long page;

    MOD_INC_USE_COUNT;
    line = MINOR(tty->device) - tty->driver.minor_start;
    if ((line < 0) || (NR_PORTS <= line)){
	MOD_DEC_USE_COUNT;
        return -ENODEV;
    }
    info = &cy_port[line];
    if (info->line < 0){
	MOD_DEC_USE_COUNT;
        return -ENODEV;
    }
    
    /* If the card's firmware hasn't been loaded,
       treat it as absent from the system.  This
       will make the user pay attention.
    */
    if (IS_CYC_Z(cy_card[info->card])) {
        if (!ISZLOADED(cy_card[info->card])) {
	    if (((ZE_V1 ==cy_readl(&((struct RUNTIME_9060 *)
		((cy_card[info->card]).ctl_addr))->mail_box_0)) &&
		Z_FPGA_CHECK(cy_card[info->card])) &&
		(ZFIRM_HLT==cy_readl(&((struct FIRM_ID *)
		((cy_card[info->card]).base_addr+ID_ADDRESS))->signature)))
	    {
		printk ("cyc:Cyclades-Z Error: you need an external power supply for this number of ports.\n\rFirmware halted.\r\n");
	    } else {
		printk("cyc:Cyclades-Z firmware not yet loaded\n");
	    }
	    MOD_DEC_USE_COUNT;
	    return -ENODEV;
	}
#ifdef CONFIG_CYZ_INTR
	else {
	    /* In case this Z board is operating in interrupt mode, its 
	       interrupts should be enabled as soon as the first open happens 
	       to one of its ports. */
	    if (!cy_card[info->card].intr_enabled) {
		/* Enable interrupts on the PLX chip */
		cy_writew(cy_card[info->card].ctl_addr+0x68,
			cy_readw(cy_card[info->card].ctl_addr+0x68)|0x0900);
		/* Enable interrupts on the FW */
		retval = cyz_issue_cmd(&cy_card[info->card], 
					0, C_CM_IRQ_ENBL, 0L);
		if (retval != 0){
		    printk("cyc:IRQ enable retval was %x\n", retval);
		}
		cy_card[info->card].intr_enabled = 1;
	    }
	}
#endif /* CONFIG_CYZ_INTR */
    }
#ifdef CY_DEBUG_OTHER
    printk("cyc:cy_open ttyC%d\n", info->line); /* */
#endif
    tty->driver_data = info;
    info->tty = tty;
    if (serial_paranoia_check(info, tty->device, "cy_open")){
        return -ENODEV;
    }
#ifdef CY_DEBUG_OPEN
    printk("cyc:cy_open ttyC%d, count = %d\n",
        info->line, info->count);/**/
#endif
    info->count++;
#ifdef CY_DEBUG_COUNT
    printk("cyc:cy_open (%d): incrementing count to %d\n",
        current->pid, info->count);
#endif
    if (!tmp_buf) {
	page = get_free_page(GFP_KERNEL);
	if (!page)
	    return -ENOMEM;
	if (tmp_buf)
	    free_page(page);
	else
	    tmp_buf = (unsigned char *) page;
    }

    /*
     * If the port is the middle of closing, bail out now
     */
    if (tty_hung_up_p(filp) || (info->flags & ASYNC_CLOSING)) {
	if (info->flags & ASYNC_CLOSING)
	    interruptible_sleep_on(&info->close_wait);
	return ((info->flags & ASYNC_HUP_NOTIFY) ? -EAGAIN : -ERESTARTSYS);
    }

    /*
     * Start up serial port
     */
    retval = startup(info);
    if (retval){
        return retval;
    }

    retval = block_til_ready(tty, filp, info);
    if (retval) {
#ifdef CY_DEBUG_OPEN
        printk("cyc:cy_open returning after block_til_ready with %d\n",
               retval);
#endif
        return retval;
    }

    if ((info->count == 1) && (info->flags & ASYNC_SPLIT_TERMIOS)) {
        if (tty->driver.subtype == SERIAL_TYPE_NORMAL)
            *tty->termios = info->normal_termios;
        else 
            *tty->termios = info->callout_termios;
    }

    info->session = current->session;
    info->pgrp = current->pgrp;

#ifdef CY_DEBUG_OPEN
    printk(" cyc:cy_open done\n");/**/
#endif

    return 0;
} /* cy_open */


/*
 * cy_wait_until_sent() --- wait until the transmitter is empty
 */
static void 
cy_wait_until_sent(struct tty_struct *tty, int timeout)
{
  struct cyclades_port * info = (struct cyclades_port *)tty->driver_data;
  unsigned char *base_addr;
  int card,chip,channel,index;
  unsigned long orig_jiffies, char_time;
	
    if (serial_paranoia_check(info, tty->device, "cy_wait_until_sent"))
	return;

    if (info->xmit_fifo_size == 0)
	return; /* Just in case.... */


    orig_jiffies = jiffies;
    /*
     * Set the check interval to be 1/5 of the estimated time to
     * send a single character, and make it at least 1.  The check
     * interval should also be less than the timeout.
     * 
     * Note: we have to use pretty tight timings here to satisfy
     * the NIST-PCTS.
     */
    char_time = (info->timeout - HZ/50) / info->xmit_fifo_size;
    char_time = char_time / 5;
    if (char_time == 0)
	char_time = 1;
    if (timeout < 0)
	timeout = 0;
    if (timeout)
	char_time = MIN(char_time, timeout);
    /*
     * If the transmitter hasn't cleared in twice the approximate
     * amount of time to send the entire FIFO, it probably won't
     * ever clear.  This assumes the UART isn't doing flow
     * control, which is currently the case.  Hence, if it ever
     * takes longer than info->timeout, this is probably due to a
     * UART bug of some kind.  So, we clamp the timeout parameter at
     * 2*info->timeout.
     */
    if (!timeout || timeout > 2*info->timeout)
	timeout = 2*info->timeout;
#ifdef CY_DEBUG_WAIT_UNTIL_SENT
    printk("In cy_wait_until_sent(%d) check=%lu...", timeout, char_time);
    printk("jiff=%lu...", jiffies);
#endif
    card = info->card;
    channel = (info->line) - (cy_card[card].first_line);
    if (!IS_CYC_Z(cy_card[card])) {
	chip = channel>>2;
	channel &= 0x03;
	index = cy_card[card].bus_index;
	base_addr = (unsigned char *)
		(cy_card[card].base_addr + (cy_chip_offset[chip]<<index));
	while (cy_readb(base_addr+(CySRER<<index)) & CyTxRdy) {
#ifdef CY_DEBUG_WAIT_UNTIL_SENT
	    printk("Not clean (jiff=%lu)...", jiffies);
#endif
	    current->state = TASK_INTERRUPTIBLE;
	    schedule_timeout(char_time);
	    if (signal_pending(current))
		break;
	    if (timeout && time_after(jiffies, orig_jiffies + timeout))
		break;
	}
	current->state = TASK_RUNNING;
    } else {
	// Nothing to do!
    }
    /* Run one more char cycle */
    current->state = TASK_INTERRUPTIBLE;
    schedule_timeout(char_time * 5);
    current->state = TASK_RUNNING;
#ifdef CY_DEBUG_WAIT_UNTIL_SENT
    printk("Clean (jiff=%lu)...done\n", jiffies);
#endif
}

/*
 * This routine is called when a particular tty device is closed.
 */
static void
cy_close(struct tty_struct *tty, struct file *filp)
{
  struct cyclades_port *info = (struct cyclades_port *)tty->driver_data;
  unsigned long flags;

#ifdef CY_DEBUG_OTHER
    printk("cyc:cy_close ttyC%d\n", info->line);
#endif

    if (!info || serial_paranoia_check(info, tty->device, "cy_close")){
        return;
    }

    CY_LOCK(info, flags);
    /* If the TTY is being hung up, nothing to do */
    if (tty_hung_up_p(filp)) {
	MOD_DEC_USE_COUNT;
	CY_UNLOCK(info, flags);
        return;
    }
        
#ifdef CY_DEBUG_OPEN
    printk("cyc:cy_close ttyC%d, count = %d\n", info->line, info->count);
#endif
    if ((tty->count == 1) && (info->count != 1)) {
        /*
         * Uh, oh.  tty->count is 1, which means that the tty
         * structure will be freed.  Info->count should always
         * be one in these conditions.  If it's greater than
         * one, we've got real problems, since it means the
         * serial port won't be shutdown.
         */
        printk("cyc:cy_close: bad serial port count; tty->count is 1, "
           "info->count is %d\n", info->count);
        info->count = 1;
    }
#ifdef CY_DEBUG_COUNT
    printk("cyc:cy_close at (%d): decrementing count to %d\n",
        current->pid, info->count - 1);
#endif
    if (--info->count < 0) {
#ifdef CY_DEBUG_COUNT
    printk("cyc:cyc_close setting count to 0\n");
#endif
        info->count = 0;
    }
    if (info->count) {
	MOD_DEC_USE_COUNT;
	CY_UNLOCK(info, flags);
        return;
    }
    info->flags |= ASYNC_CLOSING;
    /*
     * Save the termios structure, since this port may have
     * separate termios for callout and dialin.
     */
    if (info->flags & ASYNC_NORMAL_ACTIVE)
        info->normal_termios = *tty->termios;
    if (info->flags & ASYNC_CALLOUT_ACTIVE)
        info->callout_termios = *tty->termios;

    /*
    * Now we wait for the transmit buffer to clear; and we notify
    * the line discipline to only process XON/XOFF characters.
    */
    tty->closing = 1;
    CY_UNLOCK(info, flags);
    if (info->closing_wait != CY_CLOSING_WAIT_NONE) {
	tty_wait_until_sent(tty, info->closing_wait);
    }
    CY_LOCK(info, flags);

    if (!IS_CYC_Z(cy_card[info->card])) {
	int channel = info->line - cy_card[info->card].first_line;
	int index = cy_card[info->card].bus_index;
	unsigned char *base_addr = (unsigned char *)
			(cy_card[info->card].base_addr +
			 (cy_chip_offset[channel>>2] <<index));
	/* Stop accepting input */
	channel &= 0x03;
	cy_writeb((ulong)base_addr+(CyCAR<<index), (u_char)channel);
	cy_writeb((u_long)base_addr+(CySRER<<index),
			cy_readb(base_addr+(CySRER<<index)) & ~CyRxData);
	if (info->flags & ASYNC_INITIALIZED) {
	    /* Waiting for on-board buffers to be empty before closing 
	       the port */
	    CY_UNLOCK(info, flags);
	    cy_wait_until_sent(tty, info->timeout);
	    CY_LOCK(info, flags);
	}
    } else {
#ifdef Z_WAKE
	/* Waiting for on-board buffers to be empty before closing the port */
	unsigned char *base_addr = (unsigned char *) 
					cy_card[info->card].base_addr;
	struct FIRM_ID *firm_id = (struct FIRM_ID *) (base_addr + ID_ADDRESS);
	struct ZFW_CTRL *zfw_ctrl = (struct ZFW_CTRL *)
		(base_addr + (cy_readl(&firm_id->zfwctrl_addr) & 0xfffff));
	struct CH_CTRL *ch_ctrl = zfw_ctrl->ch_ctrl;
	int channel = info->line - cy_card[info->card].first_line;
	int retval;

	if (cy_readl(&ch_ctrl[channel].flow_status) != C_FS_TXIDLE) {
	    retval = cyz_issue_cmd(&cy_card[info->card], channel, 
				   C_CM_IOCTLW, 0L);
	    if (retval != 0){
		printk("cyc:cy_close retval on ttyC%d was %x\n",
		       info->line, retval);
	    }
	    CY_UNLOCK(info, flags);
	    interruptible_sleep_on(&info->shutdown_wait);
	    CY_LOCK(info, flags);
	}
#endif
    }

    CY_UNLOCK(info, flags);
    shutdown(info);
    if (tty->driver.flush_buffer)
        tty->driver.flush_buffer(tty);
    if (tty->ldisc.flush_buffer)
        tty->ldisc.flush_buffer(tty);
    CY_LOCK(info, flags);

    tty->closing = 0;
    info->event = 0;
    info->tty = 0;
    if (info->blocked_open) {
	CY_UNLOCK(info, flags);
        if (info->close_delay) {
            current->state = TASK_INTERRUPTIBLE;
            schedule_timeout(info->close_delay);
        }
        wake_up_interruptible(&info->open_wait);
	CY_LOCK(info, flags);
    }
    info->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CALLOUT_ACTIVE|
                     ASYNC_CLOSING);
    wake_up_interruptible(&info->close_wait);

#ifdef CY_DEBUG_OTHER
    printk(" cyc:cy_close done\n");
#endif

    MOD_DEC_USE_COUNT;
    CY_UNLOCK(info, flags);
    return;
} /* cy_close */


/* This routine gets called when tty_write has put something into
 * the write_queue.  The characters may come from user space or
 * kernel space.
 *
 * This routine will return the number of characters actually
 * accepted for writing.
 *
 * If the port is not already transmitting stuff, start it off by
 * enabling interrupts.  The interrupt service routine will then
 * ensure that the characters are sent.
 * If the port is already active, there is no need to kick it.
 *
 */
static int
cy_write(struct tty_struct * tty, int from_user,
           const unsigned char *buf, int count)
{
  struct cyclades_port *info = (struct cyclades_port *)tty->driver_data;
  unsigned long flags;
  int c, ret = 0;

#ifdef CY_DEBUG_IO
    printk("cyc:cy_write ttyC%d\n", info->line); /* */
#endif

    if (serial_paranoia_check(info, tty->device, "cy_write")){
        return 0;
    }
        
    if (!tty || !info->xmit_buf || !tmp_buf){
        return 0;
    }

    CY_LOCK(info, flags);
    if (from_user) {
	down(&tmp_buf_sem);
	while (1) {
	    c = MIN(count, MIN(SERIAL_XMIT_SIZE - info->xmit_cnt - 1,
				SERIAL_XMIT_SIZE - info->xmit_head));
	    if (c <= 0)
		break;

	    c -= copy_from_user(tmp_buf, buf, c);
	    if (!c) {
		if (!ret) {
		    ret = -EFAULT;
		}
		break;
	    }
	    c = MIN(c, MIN(SERIAL_XMIT_SIZE - info->xmit_cnt - 1,
			SERIAL_XMIT_SIZE - info->xmit_head));
	    memcpy(info->xmit_buf + info->xmit_head, tmp_buf, c);
	    info->xmit_head = ((info->xmit_head + c) & (SERIAL_XMIT_SIZE-1));
	    info->xmit_cnt += c;
	    buf += c;
	    count -= c;
	    ret += c;
	}
	up(&tmp_buf_sem);
    } else {
	while (1) {
	    c = MIN(count, MIN(SERIAL_XMIT_SIZE - info->xmit_cnt - 1, 
			SERIAL_XMIT_SIZE - info->xmit_head));
	    if (c <= 0) {
		break;
	    }
	    memcpy(info->xmit_buf + info->xmit_head, buf, c);
	    info->xmit_head = (info->xmit_head + c) & (SERIAL_XMIT_SIZE-1);
	    info->xmit_cnt += c;
	    buf += c;
	    count -= c;
	    ret += c;
	}
    }
    CY_UNLOCK(info, flags);

    info->idle_stats.xmit_bytes += ret;
    info->idle_stats.xmit_idle   = jiffies;

    if (info->xmit_cnt && !tty->stopped && !tty->hw_stopped) {
        start_xmit(info);
    }
    return ret;
} /* cy_write */


/*
 * This routine is called by the kernel to write a single
 * character to the tty device.  If the kernel uses this routine,
 * it must call the flush_chars() routine (if defined) when it is
 * done stuffing characters into the driver.  If there is no room
 * in the queue, the character is ignored.
 */
static void
cy_put_char(struct tty_struct *tty, unsigned char ch)
{
  struct cyclades_port *info = (struct cyclades_port *)tty->driver_data;
  unsigned long flags;

#ifdef CY_DEBUG_IO
    printk("cyc:cy_put_char ttyC%d\n", info->line);
#endif

    if (serial_paranoia_check(info, tty->device, "cy_put_char"))
        return;

    if (!tty || !info->xmit_buf)
        return;

    CY_LOCK(info, flags);
        if (info->xmit_cnt >= SERIAL_XMIT_SIZE - 1) {
	    CY_UNLOCK(info, flags);
            return;
        }

        info->xmit_buf[info->xmit_head++] = ch;
        info->xmit_head &= SERIAL_XMIT_SIZE - 1;
        info->xmit_cnt++;
	info->idle_stats.xmit_bytes++;
	info->idle_stats.xmit_idle = jiffies;
    CY_UNLOCK(info, flags);
} /* cy_put_char */


/*
 * This routine is called by the kernel after it has written a
 * series of characters to the tty device using put_char().  
 */
static void
cy_flush_chars(struct tty_struct *tty)
{
  struct cyclades_port *info = (struct cyclades_port *)tty->driver_data;
                                
#ifdef CY_DEBUG_IO
    printk("cyc:cy_flush_chars ttyC%d\n", info->line); /* */
#endif

    if (serial_paranoia_check(info, tty->device, "cy_flush_chars"))
        return;

    if (info->xmit_cnt <= 0 || tty->stopped
    || tty->hw_stopped || !info->xmit_buf)
        return;

    start_xmit(info);
} /* cy_flush_chars */


/*
 * This routine returns the numbers of characters the tty driver
 * will accept for queuing to be written.  This number is subject
 * to change as output buffers get emptied, or if the output flow
 * control is activated.
 */
static int
cy_write_room(struct tty_struct *tty)
{
  struct cyclades_port *info = (struct cyclades_port *)tty->driver_data;
  int   ret;
                                
#ifdef CY_DEBUG_IO
    printk("cyc:cy_write_room ttyC%d\n", info->line); /* */
#endif

    if (serial_paranoia_check(info, tty->device, "cy_write_room"))
        return 0;
    ret = SERIAL_XMIT_SIZE - info->xmit_cnt - 1;
    if (ret < 0)
        ret = 0;
    return ret;
} /* cy_write_room */


static int
cy_chars_in_buffer(struct tty_struct *tty)
{
  struct cyclades_port *info = (struct cyclades_port *)tty->driver_data;
  int card, channel;
                                
    if (serial_paranoia_check(info, tty->device, "cy_chars_in_buffer"))
        return 0;

    card = info->card;
    channel = (info->line) - (cy_card[card].first_line);

#ifdef Z_EXT_CHARS_IN_BUFFER
    if (!IS_CYC_Z(cy_card[card])) {
#endif /* Z_EXT_CHARS_IN_BUFFER */
#ifdef CY_DEBUG_IO
	printk("cyc:cy_chars_in_buffer ttyC%d %d\n",
		info->line, info->xmit_cnt); /* */
#endif
	return info->xmit_cnt;
#ifdef Z_EXT_CHARS_IN_BUFFER
    } else {
	static volatile struct FIRM_ID *firm_id;
	static volatile struct ZFW_CTRL *zfw_ctrl;
	static volatile struct CH_CTRL *ch_ctrl;
	static volatile struct BUF_CTRL *buf_ctrl;
	int char_count;
	volatile uclong tx_put, tx_get, tx_bufsize;

	firm_id = (struct FIRM_ID *)(cy_card[card].base_addr + ID_ADDRESS);
	zfw_ctrl = (struct ZFW_CTRL *)
		    (cy_card[card].base_addr + 
		     (cy_readl(&firm_id->zfwctrl_addr) & 0xfffff));
	ch_ctrl = &(zfw_ctrl->ch_ctrl[channel]);
	buf_ctrl = &(zfw_ctrl->buf_ctrl[channel]);

	tx_get = cy_readl(&buf_ctrl->tx_get);
	tx_put = cy_readl(&buf_ctrl->tx_put);
	tx_bufsize = cy_readl(&buf_ctrl->tx_bufsize);
	if (tx_put >= tx_get)
	    char_count = tx_put - tx_get;
	else
	    char_count = tx_put - tx_get + tx_bufsize;
#ifdef CY_DEBUG_IO
	printk("cyc:cy_chars_in_buffer ttyC%d %d\n",
		info->line, info->xmit_cnt + char_count); /* */
#endif
	return (info->xmit_cnt + char_count);
    }
#endif /* Z_EXT_CHARS_IN_BUFFER */
} /* cy_chars_in_buffer */


/*
 * ------------------------------------------------------------
 * cy_ioctl() and friends
 * ------------------------------------------------------------
 */

static void
cyy_baud_calc(struct cyclades_port *info, uclong baud)
{
    int co, co_val, bpr;
    uclong cy_clock = ((info->chip_rev >= CD1400_REV_J) ? 60000000 : 25000000);

    if (baud == 0) {
	info->tbpr = info->tco = info->rbpr = info->rco = 0;
	return;
    }

    /* determine which prescaler to use */
    for (co = 4, co_val = 2048; co; co--, co_val >>= 2) {
	if (cy_clock / co_val / baud > 63)
	    break;
    }

    bpr = (cy_clock / co_val * 2 / baud + 1) / 2;
    if (bpr > 255)
	bpr = 255;

    info->tbpr = info->rbpr = bpr;
    info->tco = info->rco = co;
}

/*
 * This routine finds or computes the various line characteristics.
 * It used to be called config_setup
 */
static void
set_line_char(struct cyclades_port * info)
{
  unsigned long flags;
  unsigned char *base_addr;
  int card,chip,channel,index;
  unsigned cflag, iflag;
  unsigned short chip_number;
  int baud, baud_rate = 0;
  int   i;


    if (!info->tty || !info->tty->termios){
        return;
    }
    if (info->line == -1){
        return;
    }
    cflag = info->tty->termios->c_cflag;
    iflag = info->tty->termios->c_iflag;

    /*
     * Set up the tty->alt_speed kludge
     */
    if (info->tty) {
	if ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_HI)
	    info->tty->alt_speed = 57600;
	if ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_VHI)
	    info->tty->alt_speed = 115200;
	if ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_SHI)
	    info->tty->alt_speed = 230400;
	if ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_WARP)
	    info->tty->alt_speed = 460800;
    }

    card = info->card;
    channel = (info->line) - (cy_card[card].first_line);
    chip_number = channel / 4;

    if (!IS_CYC_Z(cy_card[card])) {

	index = cy_card[card].bus_index;

	/* baud rate */
	baud = tty_get_baud_rate(info->tty);
	if ((baud == 38400) &&
	    ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_CUST)) {
	    if (info->custom_divisor)
		baud_rate = info->baud / info->custom_divisor;
	    else
		baud_rate = info->baud;
	} else if (baud > CD1400_MAX_SPEED) {
	    baud = CD1400_MAX_SPEED;
	}
	/* find the baud index */
	for (i = 0; i < 20; i++) {
	    if (baud == baud_table[i]) {
		break;
	    }
	}
	if (i == 20) {
	    i = 19; /* CD1400_MAX_SPEED */
	} 

	if ((baud == 38400) &&
	    ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_CUST)) {
	    cyy_baud_calc(info, baud_rate);
	} else {
	    if(info->chip_rev >= CD1400_REV_J) {
		/* It is a CD1400 rev. J or later */
		info->tbpr = baud_bpr_60[i]; /* Tx BPR */
		info->tco = baud_co_60[i]; /* Tx CO */
		info->rbpr = baud_bpr_60[i]; /* Rx BPR */
		info->rco = baud_co_60[i]; /* Rx CO */
	    } else {
		info->tbpr = baud_bpr_25[i]; /* Tx BPR */
		info->tco = baud_co_25[i]; /* Tx CO */
		info->rbpr = baud_bpr_25[i]; /* Rx BPR */
		info->rco = baud_co_25[i]; /* Rx CO */
	    }
	}
	if (baud_table[i] == 134) {
	    /* get it right for 134.5 baud */
	    info->timeout = (info->xmit_fifo_size*HZ*30/269) + 2;
	} else if ((baud == 38400) &&
		   ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_CUST)) {
	    info->timeout = (info->xmit_fifo_size*HZ*15/baud_rate) + 2;
	} else if (baud_table[i]) {
	    info->timeout = (info->xmit_fifo_size*HZ*15/baud_table[i]) + 2;
	    /* this needs to be propagated into the card info */
	} else {
	    info->timeout = 0;
	}
	/* By tradition (is it a standard?) a baud rate of zero
	   implies the line should be/has been closed.  A bit
	   later in this routine such a test is performed. */

	/* byte size and parity */
	info->cor5 = 0;
	info->cor4 = 0;
	info->cor3 = (info->default_threshold
		      ? info->default_threshold
		      : baud_cor3[i]); /* receive threshold */
	info->cor2 = CyETC;
	switch(cflag & CSIZE){
	case CS5:
	    info->cor1 = Cy_5_BITS;
	    break;
	case CS6:
	    info->cor1 = Cy_6_BITS;
	    break;
	case CS7:
	    info->cor1 = Cy_7_BITS;
	    break;
	case CS8:
	    info->cor1 = Cy_8_BITS;
	    break;
	}
	if(cflag & CSTOPB){
	    info->cor1 |= Cy_2_STOP;
	}
	if (cflag & PARENB){
	    if (cflag & PARODD){
		info->cor1 |= CyPARITY_O;
	    }else{
		info->cor1 |= CyPARITY_E;
	    }
	}else{
	    info->cor1 |= CyPARITY_NONE;
	}
	    
	/* CTS flow control flag */
	if (cflag & CRTSCTS){
	    info->flags |= ASYNC_CTS_FLOW;
	    info->cor2 |= CyCtsAE;
	}else{
	    info->flags &= ~ASYNC_CTS_FLOW;
	    info->cor2 &= ~CyCtsAE;
	}
	if (cflag & CLOCAL)
	    info->flags &= ~ASYNC_CHECK_CD;
	else
	    info->flags |= ASYNC_CHECK_CD;

	 /***********************************************
	    The hardware option, CyRtsAO, presents RTS when
	    the chip has characters to send.  Since most modems
	    use RTS as reverse (inbound) flow control, this
	    option is not used.  If inbound flow control is
	    necessary, DTR can be programmed to provide the
	    appropriate signals for use with a non-standard
	    cable.  Contact Marcio Saito for details.
	 ***********************************************/

	chip = channel>>2;
	channel &= 0x03;
	base_addr = (unsigned char*)
		       (cy_card[card].base_addr
		       + (cy_chip_offset[chip]<<index));

	CY_LOCK(info, flags);
	    cy_writeb((u_long)base_addr+(CyCAR<<index), (u_char)channel);

	   /* tx and rx baud rate */

	    cy_writeb((u_long)base_addr+(CyTCOR<<index), info->tco);
	    cy_writeb((u_long)base_addr+(CyTBPR<<index), info->tbpr);
	    cy_writeb((u_long)base_addr+(CyRCOR<<index), info->rco);
	    cy_writeb((u_long)base_addr+(CyRBPR<<index), info->rbpr);

	    /* set line characteristics  according configuration */

	    cy_writeb((u_long)base_addr+(CySCHR1<<index), 
		      START_CHAR(info->tty));
	    cy_writeb((u_long)base_addr+(CySCHR2<<index), 
		      STOP_CHAR(info->tty));
	    cy_writeb((u_long)base_addr+(CyCOR1<<index), info->cor1);
	    cy_writeb((u_long)base_addr+(CyCOR2<<index), info->cor2);
	    cy_writeb((u_long)base_addr+(CyCOR3<<index), info->cor3);
	    cy_writeb((u_long)base_addr+(CyCOR4<<index), info->cor4);
	    cy_writeb((u_long)base_addr+(CyCOR5<<index), info->cor5);

	    cyy_issue_cmd(base_addr,
		     CyCOR_CHANGE|CyCOR1ch|CyCOR2ch|CyCOR3ch,index);

	    cy_writeb((u_long)base_addr+(CyCAR<<index), 
		      (u_char)channel); /* !!! Is this needed? */
	    cy_writeb((u_long)base_addr+(CyRTPR<<index), (info->default_timeout
					         ? info->default_timeout
					         : 0x02)); /* 10ms rx timeout */

	    if (C_CLOCAL(info->tty)) {
		/* without modem intr */
		cy_writeb((u_long)base_addr+(CySRER<<index),
                   cy_readb(base_addr+(CySRER<<index)) | CyMdmCh); 
					/* act on 1->0 modem transitions */
                if ((cflag & CRTSCTS) && info->rflow) {
                        cy_writeb((u_long)base_addr+(CyMCOR1<<index), 
                                  (CyCTS|rflow_thr[i]));
                } else {
                        cy_writeb((u_long)base_addr+(CyMCOR1<<index), CyCTS);
                }
					/* act on 0->1 modem transitions */
		cy_writeb((u_long)base_addr+(CyMCOR2<<index), CyCTS);
	    } else {
		/* without modem intr */
		cy_writeb((u_long)base_addr+(CySRER<<index),
                   cy_readb(base_addr+(CySRER<<index)) | CyMdmCh); 
					/* act on 1->0 modem transitions */
                if ((cflag & CRTSCTS) && info->rflow) {
			cy_writeb((u_long)base_addr+(CyMCOR1<<index), 
        	                  (CyDSR|CyCTS|CyRI|CyDCD|rflow_thr[i]));
                } else {
			cy_writeb((u_long)base_addr+(CyMCOR1<<index), 
                                  CyDSR|CyCTS|CyRI|CyDCD);
                }
					/* act on 0->1 modem transitions */
		cy_writeb((u_long)base_addr+(CyMCOR2<<index), 
			  CyDSR|CyCTS|CyRI|CyDCD);
	    }

	    if(i == 0){ /* baud rate is zero, turn off line */
	        if (info->rtsdtr_inv) {
			cy_writeb((u_long)base_addr+(CyMSVR1<<index), ~CyRTS);
		} else {
                        cy_writeb((u_long)base_addr+(CyMSVR2<<index), ~CyDTR);
		}
#ifdef CY_DEBUG_DTR
		printk("cyc:set_line_char dropping DTR\n");
		printk("     status: 0x%x,
		    0x%x\n", cy_readb(base_addr+(CyMSVR1<<index)),
		    cy_readb(base_addr+(CyMSVR2<<index)));
#endif
	    }else{
                if (info->rtsdtr_inv) {
			cy_writeb((u_long)base_addr+(CyMSVR1<<index), CyRTS);
                } else {
			cy_writeb((u_long)base_addr+(CyMSVR2<<index), CyDTR);
                }
#ifdef CY_DEBUG_DTR
		printk("cyc:set_line_char raising DTR\n");
		printk("     status: 0x%x, 0x%x\n",
		    cy_readb(base_addr+(CyMSVR1<<index)),
		    cy_readb(base_addr+(CyMSVR2<<index)));
#endif
	    }

	    if (info->tty){
		clear_bit(TTY_IO_ERROR, &info->tty->flags);
	    }
	CY_UNLOCK(info, flags);

    } else {
      struct FIRM_ID *firm_id;
      struct ZFW_CTRL *zfw_ctrl;
      struct BOARD_CTRL *board_ctrl;
      struct CH_CTRL *ch_ctrl;
      struct BUF_CTRL *buf_ctrl;
      uclong sw_flow;
      int retval;

        firm_id = (struct FIRM_ID *)
			(cy_card[card].base_addr + ID_ADDRESS);
        if (!ISZLOADED(cy_card[card])) {
	    return;
	}

	zfw_ctrl = (struct ZFW_CTRL *)
		    (cy_card[card].base_addr + 
		     (cy_readl(&firm_id->zfwctrl_addr) & 0xfffff));
	board_ctrl = &zfw_ctrl->board_ctrl;
	ch_ctrl = &(zfw_ctrl->ch_ctrl[channel]);
	buf_ctrl = &zfw_ctrl->buf_ctrl[channel];

	/* baud rate */
	baud = tty_get_baud_rate(info->tty);
	if ((baud == 38400) &&
	    ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_CUST)) {
	    if (info->custom_divisor)
		baud_rate = info->baud / info->custom_divisor;
	    else
		baud_rate = info->baud;
	} else if (baud > CYZ_MAX_SPEED) {
	    baud = CYZ_MAX_SPEED;
	}
	cy_writel(&ch_ctrl->comm_baud , baud);

	if (baud == 134) {
	    /* get it right for 134.5 baud */
	    info->timeout = (info->xmit_fifo_size*HZ*30/269) + 2;
	} else if ((baud == 38400) &&
		   ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_CUST)) {
	    info->timeout = (info->xmit_fifo_size*HZ*15/baud_rate) + 2;
	} else if (baud) {
	    info->timeout = (info->xmit_fifo_size*HZ*15/baud) + 2;
	    /* this needs to be propagated into the card info */
	} else {
	    info->timeout = 0;
	}

	/* byte size and parity */
	switch(cflag & CSIZE){
	case CS5: cy_writel(&ch_ctrl->comm_data_l , C_DL_CS5); break;
	case CS6: cy_writel(&ch_ctrl->comm_data_l , C_DL_CS6); break;
	case CS7: cy_writel(&ch_ctrl->comm_data_l , C_DL_CS7); break;
	case CS8: cy_writel(&ch_ctrl->comm_data_l , C_DL_CS8); break;
	}
	if(cflag & CSTOPB){
	    cy_writel(&ch_ctrl->comm_data_l,
               cy_readl(&ch_ctrl->comm_data_l) | C_DL_2STOP);
	}else{
	    cy_writel(&ch_ctrl->comm_data_l,
               cy_readl(&ch_ctrl->comm_data_l) | C_DL_1STOP);
	}
	if (cflag & PARENB){
	    if (cflag & PARODD){
		cy_writel(&ch_ctrl->comm_parity , C_PR_ODD);
	    }else{
		cy_writel(&ch_ctrl->comm_parity , C_PR_EVEN);
	    }
	}else{
	    cy_writel(&ch_ctrl->comm_parity , C_PR_NONE);
	}

	/* CTS flow control flag */
	if (cflag & CRTSCTS){
	    cy_writel(&ch_ctrl->hw_flow,
               cy_readl(&ch_ctrl->hw_flow) | C_RS_CTS | C_RS_RTS);
	}else{
	    cy_writel(&ch_ctrl->hw_flow,
               cy_readl(&ch_ctrl->hw_flow) & ~(C_RS_CTS | C_RS_RTS));
	}
	/* As the HW flow control is done in firmware, the driver doesn't
	   need to care about it */
	info->flags &= ~ASYNC_CTS_FLOW;

	/* XON/XOFF/XANY flow control flags */
	sw_flow = 0;
	if (iflag & IXON){
	    sw_flow |= C_FL_OXX;
	    if (iflag & IXANY)
		sw_flow |= C_FL_OIXANY;
	}
	cy_writel(&ch_ctrl->sw_flow, sw_flow);

	retval = cyz_issue_cmd(&cy_card[card], channel, C_CM_IOCTL, 0L);
	if (retval != 0){
	    printk("cyc:set_line_char retval on ttyC%d was %x\n",
		   info->line, retval);
	}

	/* CD sensitivity */
	if (cflag & CLOCAL){
	    info->flags &= ~ASYNC_CHECK_CD;
	}else{
	    info->flags |= ASYNC_CHECK_CD;
	}

	if(baud == 0){ /* baud rate is zero, turn off line */
	    cy_writel(&ch_ctrl->rs_control,
               cy_readl(&ch_ctrl->rs_control) & ~C_RS_DTR);
#ifdef CY_DEBUG_DTR
	    printk("cyc:set_line_char dropping Z DTR\n");
#endif
	}else{
	    cy_writel(&ch_ctrl->rs_control,
               cy_readl(&ch_ctrl->rs_control) | C_RS_DTR);
#ifdef CY_DEBUG_DTR
	    printk("cyc:set_line_char raising Z DTR\n");
#endif
	}

	retval = cyz_issue_cmd( &cy_card[card], channel, C_CM_IOCTLM, 0L);
	if (retval != 0){
	    printk("cyc:set_line_char(2) retval on ttyC%d was %x\n",
		   info->line, retval);
	}

	if (info->tty){
	    clear_bit(TTY_IO_ERROR, &info->tty->flags);
	}
    }
} /* set_line_char */


static int
get_serial_info(struct cyclades_port * info,
                           struct serial_struct * retinfo)
{
  struct serial_struct tmp;
  struct cyclades_card *cinfo = &cy_card[info->card];

    if (!retinfo)
            return -EFAULT;
    memset(&tmp, 0, sizeof(tmp));
    tmp.type = info->type;
    tmp.line = info->line;
    tmp.port = info->card * 0x100 + info->line - cinfo->first_line;
    tmp.irq = cinfo->irq;
    tmp.flags = info->flags;
    tmp.close_delay = info->close_delay;
    tmp.baud_base = info->baud;
    tmp.custom_divisor = info->custom_divisor;
    tmp.hub6 = 0;               /*!!!*/
    return copy_to_user(retinfo,&tmp,sizeof(*retinfo))?-EFAULT:0;
} /* get_serial_info */


static int
set_serial_info(struct cyclades_port * info,
                           struct serial_struct * new_info)
{
  struct serial_struct new_serial;
  struct cyclades_port old_info;

    if (copy_from_user(&new_serial,new_info,sizeof(new_serial)))
	return -EFAULT;
    old_info = *info;

    if (!capable(CAP_SYS_ADMIN)) {
            if ((new_serial.close_delay != info->close_delay) ||
		(new_serial.baud_base != info->baud) ||
		((new_serial.flags & ASYNC_FLAGS & ~ASYNC_USR_MASK) !=
		 (info->flags & ASYNC_FLAGS & ~ASYNC_USR_MASK)))
                    return -EPERM;
            info->flags = ((info->flags & ~ASYNC_USR_MASK) |
                           (new_serial.flags & ASYNC_USR_MASK));
            info->baud = new_serial.baud_base;
	    info->custom_divisor = new_serial.custom_divisor;
            goto check_and_exit;
    }


    /*
     * OK, past this point, all the error checking has been done.
     * At this point, we start making changes.....
     */

    info->baud = new_serial.baud_base;
    info->custom_divisor = new_serial.custom_divisor;
    info->flags = ((info->flags & ~ASYNC_FLAGS) |
                    (new_serial.flags & ASYNC_FLAGS));
    info->close_delay = new_serial.close_delay * HZ/100;
    info->closing_wait = new_serial.closing_wait * HZ/100;

check_and_exit:
    if (info->flags & ASYNC_INITIALIZED){
        set_line_char(info);
        return 0;
    }else{
        return startup(info);
    }
} /* set_serial_info */

/*
 * get_lsr_info - get line status register info
 *
 * Purpose: Let user call ioctl() to get info when the UART physically
 *	    is emptied.  On bus types like RS485, the transmitter must
 *	    release the bus after transmitting. This must be done when
 *	    the transmit shift register is empty, not be done when the
 *	    transmit holding register is empty.  This functionality
 *	    allows an RS485 driver to be written in user space.
 */
static int get_lsr_info(struct cyclades_port *info, unsigned int *value)
{
    int card, chip, channel, index;
    unsigned char status;
    unsigned int result;
    unsigned long flags;
    unsigned char *base_addr;

    card = info->card;
    channel = (info->line) - (cy_card[card].first_line);
    if (!IS_CYC_Z(cy_card[card])) {
	chip = channel>>2;
	channel &= 0x03;
	index = cy_card[card].bus_index;
	base_addr = (unsigned char *)
		     (cy_card[card].base_addr + (cy_chip_offset[chip]<<index));

	CY_LOCK(info, flags);
	status = cy_readb(base_addr+(CySRER<<index)) & (CyTxRdy|CyTxMpty);
	CY_UNLOCK(info, flags);
	result = (status ? 0 : TIOCSER_TEMT);
    } else {
	/* Not supported yet */
	return -EINVAL;
    }
    return cy_put_user(result, (unsigned long *) value);
}

static int
get_modem_info(struct cyclades_port * info, unsigned int *value)
{
  int card,chip,channel,index;
  unsigned char *base_addr;
  unsigned long flags;
  unsigned char status;
  unsigned long lstatus;
  unsigned int result;
  struct FIRM_ID *firm_id;
  struct ZFW_CTRL *zfw_ctrl;
  struct BOARD_CTRL *board_ctrl;
  struct CH_CTRL *ch_ctrl;

    card = info->card;
    channel = (info->line) - (cy_card[card].first_line);
    if (!IS_CYC_Z(cy_card[card])) {
	chip = channel>>2;
	channel &= 0x03;
	index = cy_card[card].bus_index;
	base_addr = (unsigned char*)
		       (cy_card[card].base_addr
		       + (cy_chip_offset[chip]<<index));

	CY_LOCK(info, flags);
	    cy_writeb((u_long)base_addr+(CyCAR<<index), (u_char)channel);
	    status = cy_readb(base_addr+(CyMSVR1<<index));
	    status |= cy_readb(base_addr+(CyMSVR2<<index));
	CY_UNLOCK(info, flags);

        if (info->rtsdtr_inv) {
	    result =  ((status  & CyRTS) ? TIOCM_DTR : 0)
		    | ((status  & CyDTR) ? TIOCM_RTS : 0);
	} else {
	    result =  ((status  & CyRTS) ? TIOCM_RTS : 0)
		    | ((status  & CyDTR) ? TIOCM_DTR : 0);
	}
	result |=  ((status  & CyDCD) ? TIOCM_CAR : 0)
		 | ((status  & CyRI) ? TIOCM_RNG : 0)
		 | ((status  & CyDSR) ? TIOCM_DSR : 0)
		 | ((status  & CyCTS) ? TIOCM_CTS : 0);
    } else {
	base_addr = (unsigned char*) (cy_card[card].base_addr);

        if (cy_card[card].num_chips != -1){
	    return -EINVAL;
	}

	firm_id = (struct FIRM_ID *)
		    (cy_card[card].base_addr + ID_ADDRESS);
        if (ISZLOADED(cy_card[card])) {
	    zfw_ctrl = (struct ZFW_CTRL *)
			(cy_card[card].base_addr + 
			 (cy_readl(&firm_id->zfwctrl_addr) & 0xfffff));
	    board_ctrl = &zfw_ctrl->board_ctrl;
	    ch_ctrl = zfw_ctrl->ch_ctrl;
	    lstatus = cy_readl(&ch_ctrl[channel].rs_status);
	    result =  ((lstatus  & C_RS_RTS) ? TIOCM_RTS : 0)
		    | ((lstatus  & C_RS_DTR) ? TIOCM_DTR : 0)
	            | ((lstatus  & C_RS_DCD) ? TIOCM_CAR : 0)
		    | ((lstatus  & C_RS_RI) ? TIOCM_RNG : 0)
		    | ((lstatus  & C_RS_DSR) ? TIOCM_DSR : 0)
		    | ((lstatus  & C_RS_CTS) ? TIOCM_CTS : 0);
	}else{
	    result = 0;
	    return -ENODEV;
	}

    }
    return cy_put_user(result, value);
} /* get_modem_info */


static int
set_modem_info(struct cyclades_port * info, unsigned int cmd,
                          unsigned int *value)
{
  int card,chip,channel,index;
  unsigned char *base_addr;
  unsigned long flags;
  unsigned int arg = cy_get_user((unsigned long *) value);
  struct FIRM_ID *firm_id;
  struct ZFW_CTRL *zfw_ctrl;
  struct BOARD_CTRL *board_ctrl;
  struct CH_CTRL *ch_ctrl;
  int retval;

    card = info->card;
    channel = (info->line) - (cy_card[card].first_line);
    if (!IS_CYC_Z(cy_card[card])) {
	chip = channel>>2;
	channel &= 0x03;
	index = cy_card[card].bus_index;
	base_addr = (unsigned char*)
		       (cy_card[card].base_addr
		       + (cy_chip_offset[chip]<<index));

	switch (cmd) {
	case TIOCMBIS:
	    if (arg & TIOCM_RTS){
		CY_LOCK(info, flags);
		cy_writeb((u_long)base_addr+(CyCAR<<index), (u_char)channel);
                if (info->rtsdtr_inv) {
		    cy_writeb((u_long)base_addr+(CyMSVR2<<index), CyDTR);
                } else {
		    cy_writeb((u_long)base_addr+(CyMSVR1<<index), CyRTS);
                }
		CY_UNLOCK(info, flags);
	    }
	    if (arg & TIOCM_DTR){
		CY_LOCK(info, flags);
		cy_writeb((u_long)base_addr+(CyCAR<<index), (u_char)channel);
                if (info->rtsdtr_inv) {
		    cy_writeb((u_long)base_addr+(CyMSVR1<<index), CyRTS);
                } else {
		    cy_writeb((u_long)base_addr+(CyMSVR2<<index), CyDTR);
                }
#ifdef CY_DEBUG_DTR
		printk("cyc:set_modem_info raising DTR\n");
		printk("     status: 0x%x, 0x%x\n",
		    cy_readb(base_addr+(CyMSVR1<<index)), 
                    cy_readb(base_addr+(CyMSVR2<<index)));
#endif
		CY_UNLOCK(info, flags);
	    }
	    break;
	case TIOCMBIC:
	    if (arg & TIOCM_RTS){
		CY_LOCK(info, flags);
		cy_writeb((u_long)base_addr+(CyCAR<<index), 
                          (u_char)channel);
                if (info->rtsdtr_inv) {
		    	cy_writeb((u_long)base_addr+(CyMSVR2<<index), ~CyDTR);
                } else {
		    	cy_writeb((u_long)base_addr+(CyMSVR1<<index), ~CyRTS);
                }
		CY_UNLOCK(info, flags);
	    }
	    if (arg & TIOCM_DTR){
		CY_LOCK(info, flags);
		cy_writeb((u_long)base_addr+(CyCAR<<index), (u_char)channel);
                if (info->rtsdtr_inv) {
			cy_writeb((u_long)base_addr+(CyMSVR1<<index), ~CyRTS);
                } else {
			cy_writeb((u_long)base_addr+(CyMSVR2<<index), ~CyDTR);
                }
#ifdef CY_DEBUG_DTR
		printk("cyc:set_modem_info dropping DTR\n");
		printk("     status: 0x%x, 0x%x\n",
		    cy_readb(base_addr+(CyMSVR1<<index)), 
                    cy_readb(base_addr+(CyMSVR2<<index)));
#endif
		CY_UNLOCK(info, flags);
	    }
	    break;
	case TIOCMSET:
	    if (arg & TIOCM_RTS){
		CY_LOCK(info, flags);
	        cy_writeb((u_long)base_addr+(CyCAR<<index), (u_char)channel);
                if (info->rtsdtr_inv) {
			cy_writeb((u_long)base_addr+(CyMSVR2<<index), CyDTR);
                } else {
			cy_writeb((u_long)base_addr+(CyMSVR1<<index), CyRTS);
                }
		CY_UNLOCK(info, flags);
	    }else{
		CY_LOCK(info, flags);
		cy_writeb((u_long)base_addr+(CyCAR<<index), (u_char)channel);
                if (info->rtsdtr_inv) {
			cy_writeb((u_long)base_addr+(CyMSVR2<<index), ~CyDTR);
                } else {
			cy_writeb((u_long)base_addr+(CyMSVR1<<index), ~CyRTS);
                }
		CY_UNLOCK(info, flags);
	    }
	    if (arg & TIOCM_DTR){
		CY_LOCK(info, flags);
		cy_writeb((u_long)base_addr+(CyCAR<<index), (u_char)channel);
                if (info->rtsdtr_inv) {
			cy_writeb((u_long)base_addr+(CyMSVR1<<index), CyRTS);
                } else {
			cy_writeb((u_long)base_addr+(CyMSVR2<<index), CyDTR);
                }
#ifdef CY_DEBUG_DTR
		printk("cyc:set_modem_info raising DTR\n");
		printk("     status: 0x%x, 0x%x\n",
		    cy_readb(base_addr+(CyMSVR1<<index)), 
                    cy_readb(base_addr+(CyMSVR2<<index)));
#endif
		CY_UNLOCK(info, flags);
	    }else{
		CY_LOCK(info, flags);
		cy_writeb((u_long)base_addr+(CyCAR<<index), (u_char)channel);
                if (info->rtsdtr_inv) {
			cy_writeb((u_long)base_addr+(CyMSVR1<<index), ~CyRTS);
                } else {
			cy_writeb((u_long)base_addr+(CyMSVR2<<index), ~CyDTR);
                }

#ifdef CY_DEBUG_DTR
		printk("cyc:set_modem_info dropping DTR\n");
		printk("     status: 0x%x, 0x%x\n",
		    cy_readb(base_addr+(CyMSVR1<<index)), 
                    cy_readb(base_addr+(CyMSVR2<<index)));
#endif
		CY_UNLOCK(info, flags);
	    }
	    break;
	default:
	    return -EINVAL;
	}
    } else {
	base_addr = (unsigned char*) (cy_card[card].base_addr);

	firm_id = (struct FIRM_ID *)
		    (cy_card[card].base_addr + ID_ADDRESS);
        if (ISZLOADED(cy_card[card])) {
	    zfw_ctrl = (struct ZFW_CTRL *)
			(cy_card[card].base_addr + 
			 (cy_readl(&firm_id->zfwctrl_addr) & 0xfffff));
	    board_ctrl = &zfw_ctrl->board_ctrl;
	    ch_ctrl = zfw_ctrl->ch_ctrl;

	    switch (cmd) {
	    case TIOCMBIS:
		if (arg & TIOCM_RTS){
		    CY_LOCK(info, flags);
		    cy_writel(&ch_ctrl[channel].rs_control,
                       cy_readl(&ch_ctrl[channel].rs_control) | C_RS_RTS);
		    CY_UNLOCK(info, flags);
		}
		if (arg & TIOCM_DTR){
		    CY_LOCK(info, flags);
		    cy_writel(&ch_ctrl[channel].rs_control,
                       cy_readl(&ch_ctrl[channel].rs_control) | C_RS_DTR);
#ifdef CY_DEBUG_DTR
		    printk("cyc:set_modem_info raising Z DTR\n");
#endif
		    CY_UNLOCK(info, flags);
		}
		break;
	    case TIOCMBIC:
		if (arg & TIOCM_RTS){
		    CY_LOCK(info, flags);
		    cy_writel(&ch_ctrl[channel].rs_control,
                       cy_readl(&ch_ctrl[channel].rs_control) & ~C_RS_RTS);
		    CY_UNLOCK(info, flags);
		}
		if (arg & TIOCM_DTR){
		    CY_LOCK(info, flags);
		    cy_writel(&ch_ctrl[channel].rs_control,
                       cy_readl(&ch_ctrl[channel].rs_control) & ~C_RS_DTR);
#ifdef CY_DEBUG_DTR
		    printk("cyc:set_modem_info clearing Z DTR\n");
#endif
		    CY_UNLOCK(info, flags);
		}
		break;
	    case TIOCMSET:
		if (arg & TIOCM_RTS){
		    CY_LOCK(info, flags);
		    cy_writel(&ch_ctrl[channel].rs_control,
                       cy_readl(&ch_ctrl[channel].rs_control) | C_RS_RTS);
		    CY_UNLOCK(info, flags);
		}else{
		    CY_LOCK(info, flags);
		    cy_writel(&ch_ctrl[channel].rs_control,
                       cy_readl(&ch_ctrl[channel].rs_control) & ~C_RS_RTS);
		    CY_UNLOCK(info, flags);
		}
		if (arg & TIOCM_DTR){
		    CY_LOCK(info, flags);
		    cy_writel(&ch_ctrl[channel].rs_control,
                       cy_readl(&ch_ctrl[channel].rs_control) | C_RS_DTR);
#ifdef CY_DEBUG_DTR
		    printk("cyc:set_modem_info raising Z DTR\n");
#endif
		    CY_UNLOCK(info, flags);
		}else{
		    CY_LOCK(info, flags);
		    cy_writel(&ch_ctrl[channel].rs_control,
                       cy_readl(&ch_ctrl[channel].rs_control) & ~C_RS_DTR);
#ifdef CY_DEBUG_DTR
		    printk("cyc:set_modem_info clearing Z DTR\n");
#endif
		    CY_UNLOCK(info, flags);
		}
		break;
	    default:
		return -EINVAL;
	    }
	}else{
	    return -ENODEV;
	}
	CY_LOCK(info, flags);
        retval = cyz_issue_cmd(&cy_card[info->card],
				    channel, C_CM_IOCTLM,0L);
	if (retval != 0){
	    printk("cyc:set_modem_info retval on ttyC%d was %x\n",
		   info->line, retval);
	}
	CY_UNLOCK(info, flags);
    }
    return 0;
} /* set_modem_info */

/*
 * cy_break() --- routine which turns the break handling on or off
 */
static void
cy_break(struct tty_struct *tty, int break_state)
{
    struct cyclades_port * info = (struct cyclades_port *)tty->driver_data;
    unsigned long flags;

    if (serial_paranoia_check(info, tty->device, "cy_break"))
	return;

    CY_LOCK(info, flags);
    if (!IS_CYC_Z(cy_card[info->card])) {
        /* Let the transmit ISR take care of this (since it
	   requires stuffing characters into the output stream).
        */
	if (break_state == -1) {
	    if (!info->breakon) {
		info->breakon = 1;
		if (!info->xmit_cnt) {
		    CY_UNLOCK(info, flags);
		    start_xmit(info);
		    CY_LOCK(info, flags);
		}
	    }
	} else {
	    if (!info->breakoff) {
		info->breakoff = 1;
		if (!info->xmit_cnt) {
		    CY_UNLOCK(info, flags);
		    start_xmit(info);
		    CY_LOCK(info, flags);
		}
	    }
	}
    } else {
	int retval;

	if (break_state == -1) {
	    retval = cyz_issue_cmd(&cy_card[info->card],
		(info->line) - (cy_card[info->card].first_line),
		C_CM_SET_BREAK, 0L);
	    if (retval != 0) {
		printk("cyc:cy_break (set) retval on ttyC%d was %x\n",
		       info->line, retval);
	    }
	} else {
	    retval = cyz_issue_cmd(&cy_card[info->card],
		(info->line) - (cy_card[info->card].first_line),
		C_CM_CLR_BREAK, 0L);
	    if (retval != 0) {
		printk("cyc:cy_break (clr) retval on ttyC%d was %x\n",
		       info->line, retval);
	    }
	}
    }
    CY_UNLOCK(info, flags);
} /* cy_break */

static int
get_mon_info(struct cyclades_port * info, struct cyclades_monitor * mon)
{

    if(copy_to_user(mon, &info->mon, sizeof(struct cyclades_monitor)))
    	return -EFAULT;
    info->mon.int_count  = 0;
    info->mon.char_count = 0;
    info->mon.char_max   = 0;
    info->mon.char_last  = 0;
    return 0;
}/* get_mon_info */


static int
set_threshold(struct cyclades_port * info, unsigned long value)
{
  unsigned char *base_addr;
  int card,channel,chip,index;
  unsigned long flags;
   
    card = info->card;
    channel = info->line - cy_card[card].first_line;
    if (!IS_CYC_Z(cy_card[card])) {
	chip = channel>>2;
	channel &= 0x03;
	index = cy_card[card].bus_index;
	base_addr = (unsigned char*)
		       (cy_card[card].base_addr
		       + (cy_chip_offset[chip]<<index));

	info->cor3 &= ~CyREC_FIFO;
	info->cor3 |= value & CyREC_FIFO;

	CY_LOCK(info, flags);
	    cy_writeb((u_long)base_addr+(CyCOR3<<index), info->cor3);
	    cyy_issue_cmd(base_addr,CyCOR_CHANGE|CyCOR3ch,index);
	CY_UNLOCK(info, flags);
    } else {
	// Nothing to do!
    }
    return 0;
}/* set_threshold */


static int
get_threshold(struct cyclades_port * info, unsigned long *value)
{
  unsigned char *base_addr;
  int card,channel,chip,index;
  unsigned long tmp;
   
    card = info->card;
    channel = info->line - cy_card[card].first_line;
    if (!IS_CYC_Z(cy_card[card])) {
	chip = channel>>2;
	channel &= 0x03;
	index = cy_card[card].bus_index;
	base_addr = (unsigned char*)
		       (cy_card[card].base_addr
		       + (cy_chip_offset[chip]<<index));

	tmp = cy_readb(base_addr+(CyCOR3<<index)) & CyREC_FIFO;
	return cy_put_user(tmp,value);
    } else {
	// Nothing to do!
	return 0;
    }
}/* get_threshold */


static int
set_default_threshold(struct cyclades_port * info, unsigned long value)
{
    info->default_threshold = value & 0x0f;
    return 0;
}/* set_default_threshold */


static int
get_default_threshold(struct cyclades_port * info, unsigned long *value)
{
    return cy_put_user(info->default_threshold,value);
}/* get_default_threshold */


static int
set_timeout(struct cyclades_port * info, unsigned long value)
{
  unsigned char *base_addr;
  int card,channel,chip,index;
  unsigned long flags;
   
    card = info->card;
    channel = info->line - cy_card[card].first_line;
    if (!IS_CYC_Z(cy_card[card])) {
	chip = channel>>2;
	channel &= 0x03;
	index = cy_card[card].bus_index;
	base_addr = (unsigned char*)
		       (cy_card[card].base_addr
		       + (cy_chip_offset[chip]<<index));

	CY_LOCK(info, flags);
	    cy_writeb((u_long)base_addr+(CyRTPR<<index), value & 0xff);
	CY_UNLOCK(info, flags);
    } else {
	// Nothing to do!
    }
    return 0;
}/* set_timeout */


static int
get_timeout(struct cyclades_port * info, unsigned long *value)
{
  unsigned char *base_addr;
  int card,channel,chip,index;
  unsigned long tmp;
   
    card = info->card;
    channel = info->line - cy_card[card].first_line;
    if (!IS_CYC_Z(cy_card[card])) {
	chip = channel>>2;
	channel &= 0x03;
	index = cy_card[card].bus_index;
	base_addr = (unsigned char*)
		       (cy_card[card].base_addr
		       + (cy_chip_offset[chip]<<index));

	tmp = cy_readb(base_addr+(CyRTPR<<index));
	return cy_put_user(tmp,value);
    } else {
	// Nothing to do!
	return 0;
    }
}/* get_timeout */


static int
set_default_timeout(struct cyclades_port * info, unsigned long value)
{
    info->default_timeout = value & 0xff;
    return 0;
}/* set_default_timeout */


static int
get_default_timeout(struct cyclades_port * info, unsigned long *value)
{
    return cy_put_user(info->default_timeout,value);
}/* get_default_timeout */

/*
 * This routine allows the tty driver to implement device-
 * specific ioctl's.  If the ioctl number passed in cmd is
 * not recognized by the driver, it should return ENOIOCTLCMD.
 */
static int
cy_ioctl(struct tty_struct *tty, struct file * file,
            unsigned int cmd, unsigned long arg)
{
  struct cyclades_port * info = (struct cyclades_port *)tty->driver_data;
  struct cyclades_icount cprev, cnow;		/* kernel counter temps */
  struct serial_icounter_struct *p_cuser;	/* user space */
  int ret_val = 0;
  unsigned long flags;

    if (serial_paranoia_check(info, tty->device, "cy_ioctl"))
	return -ENODEV;

#ifdef CY_DEBUG_OTHER
    printk("cyc:cy_ioctl ttyC%d, cmd = %x arg = %lx\n",
        info->line, cmd, arg); /* */
#endif

    switch (cmd) {
        case CYGETMON:
            ret_val = get_mon_info(info, (struct cyclades_monitor *)arg);
            break;
        case CYGETTHRESH:
            ret_val = get_threshold(info, (unsigned long *)arg);
            break;
        case CYSETTHRESH:
            ret_val = set_threshold(info, (unsigned long)arg);
            break;
        case CYGETDEFTHRESH:
            ret_val = get_default_threshold(info, (unsigned long *)arg);
            break;
        case CYSETDEFTHRESH:
            ret_val = set_default_threshold(info, (unsigned long)arg);
            break;
        case CYGETTIMEOUT:
            ret_val = get_timeout(info, (unsigned long *)arg);
            break;
        case CYSETTIMEOUT:
            ret_val = set_timeout(info, (unsigned long)arg);
            break;
        case CYGETDEFTIMEOUT:
            ret_val = get_default_timeout(info, (unsigned long *)arg);
            break;
        case CYSETDEFTIMEOUT:
            ret_val = set_default_timeout(info, (unsigned long)arg);
            break;
	case CYSETRFLOW:
    	    info->rflow = (int)arg;
	    ret_val = 0;
	    break;
	case CYGETRFLOW:
	    ret_val = info->rflow;
	    break;
	case CYSETRTSDTR_INV:
    	    info->rtsdtr_inv = (int)arg;
	    ret_val = 0;
	    break;
	case CYGETRTSDTR_INV:
	    ret_val = info->rtsdtr_inv;
	    break;
	case CYGETCARDINFO:
            if (copy_to_user((void *)arg, (void *)&cy_card[info->card], 
			sizeof (struct cyclades_card))) {
		ret_val = -EFAULT;
		break;
	    }
	    ret_val = 0;
            break;
	case CYGETCD1400VER:
	    ret_val = info->chip_rev;
	    break;
#ifndef CONFIG_CYZ_INTR
	case CYZSETPOLLCYCLE:
            cyz_polling_cycle = (arg * HZ) / 1000;
	    ret_val = 0;
	    break;
	case CYZGETPOLLCYCLE:
            ret_val = (cyz_polling_cycle * 1000) / HZ;
	    break;
#endif /* CONFIG_CYZ_INTR */
	case CYSETWAIT:
    	    info->closing_wait = (unsigned short)arg * HZ/100;
	    ret_val = 0;
	    break;
	case CYGETWAIT:
	    ret_val = info->closing_wait / (HZ/100);
	    break;
        case TIOCMGET:
            ret_val = get_modem_info(info, (unsigned int *) arg);
            break;
        case TIOCMBIS:
        case TIOCMBIC:
        case TIOCMSET:
            ret_val = set_modem_info(info, cmd, (unsigned int *) arg);
            break;
        case TIOCGSERIAL:
            ret_val = get_serial_info(info, (struct serial_struct *) arg);
            break;
        case TIOCSSERIAL:
            ret_val = set_serial_info(info, (struct serial_struct *) arg);
            break;
	case TIOCSERGETLSR: /* Get line status register */
	    ret_val = get_lsr_info(info, (unsigned int *) arg);
	    break;
	/*
	 * Wait for any of the 4 modem inputs (DCD,RI,DSR,CTS) to change 
	 * - mask passed in arg for lines of interest
	 *   (use |'ed TIOCM_RNG/DSR/CD/CTS for masking)
	 * Caller should use TIOCGICOUNT to see which one it was
	 */
	case TIOCMIWAIT:
	    CY_LOCK(info, flags);
	    /* note the counters on entry */
	    cprev = info->icount;
	    CY_UNLOCK(info, flags);
	    while (1) {
		interruptible_sleep_on(&info->delta_msr_wait);
		/* see if a signal did it */
		if (signal_pending(current)) {
		    return -ERESTARTSYS;
		}

		CY_LOCK(info, flags);
		cnow = info->icount; /* atomic copy */
		CY_UNLOCK(info, flags);

		if (cnow.rng == cprev.rng && cnow.dsr == cprev.dsr && 
		    cnow.dcd == cprev.dcd && cnow.cts == cprev.cts) {
		    return -EIO; /* no change => error */
		}
		if ( ((arg & TIOCM_RNG) && (cnow.rng != cprev.rng)) || 
		     ((arg & TIOCM_DSR) && (cnow.dsr != cprev.dsr)) || 
		     ((arg & TIOCM_CD)  && (cnow.dcd != cprev.dcd)) || 
		     ((arg & TIOCM_CTS) && (cnow.cts != cprev.cts)) ) {
		    return 0;
		}
		cprev = cnow;
	    }
	    /* NOTREACHED */

	/*
	 * Get counter of input serial line interrupts (DCD,RI,DSR,CTS)
	 * Return: write counters to the user passed counter struct
	 * NB: both 1->0 and 0->1 transitions are counted except for
	 *     RI where only 0->1 is counted.
	 */
	case TIOCGICOUNT:
	    CY_LOCK(info, flags);
	    cnow = info->icount;
	    CY_UNLOCK(info, flags);
	    p_cuser = (struct serial_icounter_struct *) arg;
	    ret_val = put_user(cnow.cts, &p_cuser->cts);
	    if (ret_val) return ret_val;
	    ret_val = put_user(cnow.dsr, &p_cuser->dsr);
	    if (ret_val) return ret_val;
	    ret_val = put_user(cnow.rng, &p_cuser->rng);
	    if (ret_val) return ret_val;
	    ret_val = put_user(cnow.dcd, &p_cuser->dcd);
	    if (ret_val) return ret_val;
	    ret_val = put_user(cnow.rx, &p_cuser->rx);
	    if (ret_val) return ret_val;
	    ret_val = put_user(cnow.tx, &p_cuser->tx);
	    if (ret_val) return ret_val;
	    ret_val = put_user(cnow.frame, &p_cuser->frame);
	    if (ret_val) return ret_val;
	    ret_val = put_user(cnow.overrun, &p_cuser->overrun);
	    if (ret_val) return ret_val;
	    ret_val = put_user(cnow.parity, &p_cuser->parity);
	    if (ret_val) return ret_val;
	    ret_val = put_user(cnow.brk, &p_cuser->brk);
	    if (ret_val) return ret_val;
	    ret_val = put_user(cnow.buf_overrun, &p_cuser->buf_overrun);
	    if (ret_val) return ret_val;
	    ret_val = 0;
	    break;
        default:
            ret_val = -ENOIOCTLCMD;
    }

#ifdef CY_DEBUG_OTHER
    printk(" cyc:cy_ioctl done\n");
#endif

    return ret_val;
} /* cy_ioctl */


/*
 * This routine allows the tty driver to be notified when
 * device's termios settings have changed.  Note that a
 * well-designed tty driver should be prepared to accept the case
 * where old == NULL, and try to do something rational.
 */
static void
cy_set_termios(struct tty_struct *tty, struct termios * old_termios)
{
  struct cyclades_port *info = (struct cyclades_port *)tty->driver_data;

#ifdef CY_DEBUG_OTHER
    printk("cyc:cy_set_termios ttyC%d\n", info->line);
#endif

    if ((tty->termios->c_cflag == old_termios->c_cflag) &&
	((tty->termios->c_iflag & (IXON|IXANY)) == 
	 (old_termios->c_iflag & (IXON|IXANY))))
        return;
    set_line_char(info);

    if ((old_termios->c_cflag & CRTSCTS) &&
        !(tty->termios->c_cflag & CRTSCTS)) {
            tty->hw_stopped = 0;
            cy_start(tty);
    }
#if 0
    /*
     * No need to wake up processes in open wait, since they
     * sample the CLOCAL flag once, and don't recheck it.
     * XXX  It's not clear whether the current behavior is correct
     * or not.  Hence, this may change.....
     */
    if (!(old_termios->c_cflag & CLOCAL) &&
        (tty->termios->c_cflag & CLOCAL))
            wake_up_interruptible(&info->open_wait);
#endif

    return;
} /* cy_set_termios */

/* This routine is called by the upper-layer tty layer to signal
   that incoming characters should be throttled because the input
   buffers are close to full.
 */
static void
cy_throttle(struct tty_struct * tty)
{
  struct cyclades_port *info = (struct cyclades_port *)tty->driver_data;
  unsigned long flags;
  unsigned char *base_addr;
  int card,chip,channel,index;

#ifdef CY_DEBUG_THROTTLE
  char buf[64];

    printk("cyc:throttle %s: %d....ttyC%d\n", 
	   tty_name(tty, buf),
           tty->ldisc.chars_in_buffer(tty), info->line);
#endif

    if (serial_paranoia_check(info, tty->device, "cy_throttle")){
            return;
    }

    if (I_IXOFF(tty)) {
        info->x_char = STOP_CHAR(tty);
            /* Should use the "Send Special Character" feature!!! */
    }

    card = info->card;
    channel = info->line - cy_card[card].first_line;
    if (!IS_CYC_Z(cy_card[card])) {
	chip = channel>>2;
	channel &= 0x03;
	index = cy_card[card].bus_index;
	base_addr = (unsigned char*)
		       (cy_card[card].base_addr
		       + (cy_chip_offset[chip]<<index));

	CY_LOCK(info, flags);
	cy_writeb((u_long)base_addr+(CyCAR<<index), (u_char)channel);
	if (info->rtsdtr_inv) {
		cy_writeb((u_long)base_addr+(CyMSVR2<<index), ~CyDTR);
	} else {
		cy_writeb((u_long)base_addr+(CyMSVR1<<index), ~CyRTS);
	}
	CY_UNLOCK(info, flags);
    } else {
	// Nothing to do!
    }

    return;
} /* cy_throttle */


/*
 * This routine notifies the tty driver that it should signal
 * that characters can now be sent to the tty without fear of
 * overrunning the input buffers of the line disciplines.
 */
static void
cy_unthrottle(struct tty_struct * tty)
{
  struct cyclades_port *info = (struct cyclades_port *)tty->driver_data;
  unsigned long flags;
  unsigned char *base_addr;
  int card,chip,channel,index;

#ifdef CY_DEBUG_THROTTLE
  char buf[64];
        
    printk("cyc:unthrottle %s: %d....ttyC%d\n", 
	   tty_name(tty, buf),
           tty->ldisc.chars_in_buffer(tty), info->line);
#endif

    if (serial_paranoia_check(info, tty->device, "cy_unthrottle")){
            return;
    }

    if (I_IXOFF(tty)) {
	if (info->x_char)
	    info->x_char = 0;
	else
	    info->x_char = START_CHAR(tty);
            /* Should use the "Send Special Character" feature!!! */
    }

    card = info->card;
    channel = info->line - cy_card[card].first_line;
    if (!IS_CYC_Z(cy_card[card])) {
	chip = channel>>2;
	channel &= 0x03;
	index = cy_card[card].bus_index;
	base_addr = (unsigned char*)
		       (cy_card[card].base_addr
		       + (cy_chip_offset[chip]<<index));

	CY_LOCK(info, flags);
	cy_writeb((u_long)base_addr+(CyCAR<<index), (u_char)channel);
	if (info->rtsdtr_inv) {
		cy_writeb((u_long)base_addr+(CyMSVR2<<index), CyDTR);
	} else {
		cy_writeb((u_long)base_addr+(CyMSVR1<<index), CyRTS);
	}
	CY_UNLOCK(info, flags);
    }else{
	// Nothing to do!
    }

    return;
} /* cy_unthrottle */


/* cy_start and cy_stop provide software output flow control as a
   function of XON/XOFF, software CTS, and other such stuff.
*/
static void
cy_stop(struct tty_struct *tty)
{
  struct cyclades_card *cinfo;
  struct cyclades_port *info = (struct cyclades_port *)tty->driver_data;
  unsigned char *base_addr;
  int chip,channel,index;
  unsigned long flags;

#ifdef CY_DEBUG_OTHER
    printk("cyc:cy_stop ttyC%d\n", info->line); /* */
#endif

    if (serial_paranoia_check(info, tty->device, "cy_stop"))
        return;
        
    cinfo = &cy_card[info->card];
    channel = info->line - cinfo->first_line;
    if (!IS_CYC_Z(*cinfo)) {
        index = cinfo->bus_index;
        chip = channel>>2;
        channel &= 0x03;
        base_addr = (unsigned char*)
                   (cy_card[info->card].base_addr
                           + (cy_chip_offset[chip]<<index));

	CY_LOCK(info, flags);
            cy_writeb((u_long)base_addr+(CyCAR<<index),
	       (u_char)(channel & 0x0003)); /* index channel */
            cy_writeb((u_long)base_addr+(CySRER<<index), 
               cy_readb(base_addr+(CySRER<<index)) & ~CyTxRdy);
	CY_UNLOCK(info, flags);
    } else {
	// Nothing to do!
    }

    return;
} /* cy_stop */


static void
cy_start(struct tty_struct *tty)
{
  struct cyclades_card *cinfo;
  struct cyclades_port *info = (struct cyclades_port *)tty->driver_data;
  unsigned char *base_addr;
  int chip,channel,index;
  unsigned long flags;

#ifdef CY_DEBUG_OTHER
    printk("cyc:cy_start ttyC%d\n", info->line); /* */
#endif

    if (serial_paranoia_check(info, tty->device, "cy_start"))
        return;
        
    cinfo = &cy_card[info->card];
    channel = info->line - cinfo->first_line;
    index = cinfo->bus_index;
    if (!IS_CYC_Z(*cinfo)) {
        chip = channel>>2;
        channel &= 0x03;
        base_addr = (unsigned char*)
                       (cy_card[info->card].base_addr
		       + (cy_chip_offset[chip]<<index));

	CY_LOCK(info, flags);
            cy_writeb((u_long)base_addr+(CyCAR<<index),
	       (u_char)(channel & 0x0003)); /* index channel */
            cy_writeb((u_long)base_addr+(CySRER<<index), 
               cy_readb(base_addr+(CySRER<<index)) | CyTxRdy);
	CY_UNLOCK(info, flags);
    } else {
	// Nothing to do!
    }

    return;
} /* cy_start */


static void
cy_flush_buffer(struct tty_struct *tty)
{
  struct cyclades_port *info = (struct cyclades_port *)tty->driver_data;
  int card, channel, retval;
  unsigned long flags;
                                
#ifdef CY_DEBUG_IO
    printk("cyc:cy_flush_buffer ttyC%d\n", info->line); /* */
#endif

    if (serial_paranoia_check(info, tty->device, "cy_flush_buffer"))
        return;

    card = info->card;
    channel = (info->line) - (cy_card[card].first_line);

    CY_LOCK(info, flags);
    info->xmit_cnt = info->xmit_head = info->xmit_tail = 0;
    CY_UNLOCK(info, flags);

    if (IS_CYC_Z(cy_card[card])) { /* If it is a Z card, flush the on-board 
				      buffers as well */
	CY_LOCK(info, flags);
	retval = cyz_issue_cmd(&cy_card[card], channel, C_CM_FLUSH_TX, 0L);
	if (retval != 0) {
	    printk("cyc: flush_buffer retval on ttyC%d was %x\n",
		   info->line, retval);
	}
	CY_UNLOCK(info, flags);
    }
    wake_up_interruptible(&tty->write_wait);
    if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP))
	&& tty->ldisc.write_wakeup)
	    (tty->ldisc.write_wakeup)(tty);
} /* cy_flush_buffer */


/*
 * cy_hangup() --- called by tty_hangup() when a hangup is signaled.
 */
static void
cy_hangup(struct tty_struct *tty)
{
  struct cyclades_port * info = (struct cyclades_port *)tty->driver_data;
        
#ifdef CY_DEBUG_OTHER
    printk("cyc:cy_hangup ttyC%d\n", info->line); /* */
#endif

    if (serial_paranoia_check(info, tty->device, "cy_hangup"))
        return;

    cy_flush_buffer(tty);
    shutdown(info);
    info->event = 0;
    info->count = 0;
#ifdef CY_DEBUG_COUNT
    printk("cyc:cy_hangup (%d): setting count to 0\n", current->pid);
#endif
    info->tty = 0;
    info->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CALLOUT_ACTIVE);
    wake_up_interruptible(&info->open_wait);
} /* cy_hangup */


/*
 * ---------------------------------------------------------------------
 * cy_init() and friends
 *
 * cy_init() is called at boot-time to initialize the serial driver.
 * ---------------------------------------------------------------------
 */

/* initialize chips on Cyclom-Y card -- return number of valid
   chips (which is number of ports/4) */
static unsigned short __init
cyy_init_card(volatile ucchar *true_base_addr,int index)
{
  unsigned int chip_number;
  volatile ucchar* base_addr;

    cy_writeb((u_long)true_base_addr+(Cy_HwReset<<index), 0); 
						/* Cy_HwReset is 0x1400 */
    cy_writeb((u_long)true_base_addr+(Cy_ClrIntr<<index), 0); 
						/* Cy_ClrIntr is 0x1800 */
    udelay(500L);

    for(chip_number=0; chip_number<CyMAX_CHIPS_PER_CARD; chip_number++){
        base_addr = true_base_addr
	               + (cy_chip_offset[chip_number]<<index);
        mdelay(1);
        if(cy_readb(base_addr+(CyCCR<<index)) != 0x00){
            /*************
            printk(" chip #%d at %#6lx is never idle (CCR != 0)\n",
               chip_number, (unsigned long)base_addr);
            *************/
            return chip_number;
        }

        cy_writeb((u_long)base_addr+(CyGFRCR<<index), 0);
        udelay(10L);

        /* The Cyclom-16Y does not decode address bit 9 and therefore
           cannot distinguish between references to chip 0 and a non-
           existent chip 4.  If the preceding clearing of the supposed
           chip 4 GFRCR register appears at chip 0, there is no chip 4
           and this must be a Cyclom-16Y, not a Cyclom-32Ye.
        */
        if (chip_number == 4
        && cy_readb(true_base_addr
	    + (cy_chip_offset[0]<<index)
	    + (CyGFRCR<<index)) == 0){
            return chip_number;
        }

        cy_writeb((u_long)base_addr+(CyCCR<<index), CyCHIP_RESET);
        mdelay(1);

        if(cy_readb(base_addr+(CyGFRCR<<index)) == 0x00){
            /*
            printk(" chip #%d at %#6lx is not responding ",
               chip_number, (unsigned long)base_addr);
            printk("(GFRCR stayed 0)\n",
            */
            return chip_number;
        }
        if((0xf0 & (cy_readb(base_addr+(CyGFRCR<<index)))) != 0x40){
            /*
            printk(" chip #%d at %#6lx is not valid (GFRCR == %#2x)\n",
               chip_number, (unsigned long)base_addr,
	       base_addr[CyGFRCR<<index]);
            */
            return chip_number;
        }
        cy_writeb((u_long)base_addr+(CyGCR<<index), CyCH0_SERIAL);
        if (cy_readb(base_addr+(CyGFRCR<<index)) >= CD1400_REV_J){
	    /* It is a CD1400 rev. J or later */
	    /* Impossible to reach 5ms with this chip. 
	       Changed to 2ms instead (f = 500 Hz). */
	    cy_writeb((u_long)base_addr+(CyPPR<<index), CyCLOCK_60_2MS);
	} else {
	    /* f = 200 Hz */
	    cy_writeb((u_long)base_addr+(CyPPR<<index), CyCLOCK_25_5MS);
	}

    /*
        printk(" chip #%d at %#6lx is rev 0x%2x\n",
               chip_number, (unsigned long)base_addr,
	       cy_readb(base_addr+(CyGFRCR<<index)));
    */
    }
    return chip_number;
} /* cyy_init_card */

/*
 * ---------------------------------------------------------------------
 * cy_detect_isa() - Probe for Cyclom-Y/ISA boards.
 * sets global variables and return the number of ISA boards found.
 * ---------------------------------------------------------------------
 */
static int __init
cy_detect_isa(void)
{
#ifdef CONFIG_ISA
  unsigned short	cy_isa_irq,nboard;
  volatile ucchar	*cy_isa_address;
  unsigned short	i,j,cy_isa_nchan;
#ifdef MODULE
  int isparam = 0;
#endif

        nboard = 0;

#ifdef MODULE
	/* Check for module parameters */
	for(i = 0 ; i < NR_CARDS; i++) {
	    if (maddr[i] || i) {
		isparam = 1;
		cy_isa_addresses[i] = (ucchar *)maddr[i];
	    }
	    if (!maddr[i])
		break;
	}
#endif

        /* scan the address table probing for Cyclom-Y/ISA boards */
        for (i = 0 ; i < NR_ISA_ADDRS ; i++) {
                cy_isa_address = cy_isa_addresses[i];
                if (cy_isa_address  == 0x0000) {
                        return(nboard);
                }

                /* probe for CD1400... */
#if !defined(__alpha__)
		cy_isa_address = ioremap((ulong)cy_isa_address, CyISA_Ywin);
#endif
                cy_isa_nchan = CyPORTS_PER_CHIP * 
                     cyy_init_card(cy_isa_address,0);
                if (cy_isa_nchan == 0) {
                        continue;
                }

#ifdef MODULE
		if (isparam && irq[i])
		    cy_isa_irq = irq[i];
		else
#endif
                /* find out the board's irq by probing */
                cy_isa_irq = detect_isa_irq(cy_isa_address);
                if (cy_isa_irq == 0) {
                        printk("Cyclom-Y/ISA found at 0x%lx ",
                                (unsigned long) cy_isa_address);
                        printk("but the IRQ could not be detected.\n");
                        continue;
                }

                if((cy_next_channel+cy_isa_nchan) > NR_PORTS) {
                        printk("Cyclom-Y/ISA found at 0x%lx ",
                                (unsigned long) cy_isa_address);
                        printk("but no more channels are available.\n");
                        printk("Change NR_PORTS in cyclades.c and recompile kernel.\n");
                        return(nboard);
                }
                /* fill the next cy_card structure available */
                for (j = 0 ; j < NR_CARDS ; j++) {
                        if (cy_card[j].base_addr == 0)  break;
                }
                if (j == NR_CARDS) {    /* no more cy_cards available */
                        printk("Cyclom-Y/ISA found at 0x%lx ",
                                (unsigned long) cy_isa_address);
                        printk("but no more cards can be used .\n");
                        printk("Change NR_CARDS in cyclades.c and recompile kernel.\n");
                        return(nboard);
                }

                /* allocate IRQ */
                if(request_irq(cy_isa_irq, cyy_interrupt,
				   SA_INTERRUPT, "Cyclom-Y", &cy_card[j]))
                {
                        printk("Cyclom-Y/ISA found at 0x%lx ",
                                (unsigned long) cy_isa_address);
                        printk("but could not allocate IRQ#%d.\n",
                                cy_isa_irq);
                        return(nboard);
                }

                /* set cy_card */
                cy_card[j].base_addr = (u_long) cy_isa_address;
                cy_card[j].ctl_addr = 0;
                cy_card[j].irq = (int) cy_isa_irq;
                cy_card[j].bus_index = 0;
                cy_card[j].first_line = cy_next_channel;
                cy_card[j].num_chips = cy_isa_nchan/4;
                nboard++;
                        
                /* print message */
                printk("Cyclom-Y/ISA #%d: 0x%lx-0x%lx, IRQ%d, ",
                    j+1, (unsigned long) cy_isa_address,
                    (unsigned long)(cy_isa_address + (CyISA_Ywin - 1)),
		    cy_isa_irq);
                printk("%d channels starting from port %d.\n",
                        cy_isa_nchan, cy_next_channel);
                cy_next_channel += cy_isa_nchan;
        }
        return(nboard);
#else
        return(0);
#endif /* CONFIG_ISA */
} /* cy_detect_isa */

static void 
plx_init(uclong addr, uclong initctl)
{
    /* Reset PLX */
    cy_writel(addr + initctl, cy_readl(addr + initctl) | 0x40000000);
    udelay(100L);
    cy_writel(addr + initctl, cy_readl(addr + initctl) & ~0x40000000);

    /* Reload Config. Registers from EEPROM */
    cy_writel(addr + initctl, cy_readl(addr + initctl) | 0x20000000);
    udelay(100L);
    cy_writel(addr + initctl, cy_readl(addr + initctl) & ~0x20000000);
}

/*
 * ---------------------------------------------------------------------
 * cy_detect_pci() - Test PCI bus presence and Cyclom-Ye/PCI.
 * sets global variables and return the number of PCI boards found.
 * ---------------------------------------------------------------------
 */
static int __init
cy_detect_pci(void)
{
#ifdef CONFIG_PCI

  struct pci_dev	*pdev = NULL;
  unsigned char		cyy_rev_id;
  unsigned char         cy_pci_irq = 0;
  uclong		cy_pci_phys0, cy_pci_phys1, cy_pci_phys2;
  uclong		cy_pci_addr0, cy_pci_addr2;
  unsigned short        i,j,cy_pci_nchan, plx_ver;
  unsigned short        device_id,dev_index = 0;
  uclong		mailbox;
  uclong		Ze_addr0[NR_CARDS], Ze_addr2[NR_CARDS], ZeIndex = 0;
  uclong		Ze_phys0[NR_CARDS], Ze_phys2[NR_CARDS];
  unsigned char         Ze_irq[NR_CARDS];

        for (i = 0; i < NR_CARDS; i++) {
                /* look for a Cyclades card by vendor and device id */
                while((device_id = cy_pci_dev_id[dev_index]) != 0) {
                        if((pdev = pci_find_device(PCI_VENDOR_ID_CYCLADES,
                                        device_id, pdev)) == NULL) {
                                dev_index++;    /* try next device id */
                        } else {
                                break;          /* found a board */
                        }
                }

		if (device_id == 0)
		    break;

		if (pci_enable_device(pdev))
		    continue;

                /* read PCI configuration area */
		cy_pci_irq = pdev->irq;
		cy_pci_phys0 = pci_resource_start(pdev, 0);
		cy_pci_phys1 = pci_resource_start(pdev, 1);
		cy_pci_phys2 = pci_resource_start(pdev, 2);
		pci_read_config_byte(pdev, PCI_REVISION_ID, &cyy_rev_id);

		device_id &= ~PCI_DEVICE_ID_MASK;

    if ((device_id == PCI_DEVICE_ID_CYCLOM_Y_Lo)
	   || (device_id == PCI_DEVICE_ID_CYCLOM_Y_Hi)){
#ifdef CY_PCI_DEBUG
            printk("Cyclom-Y/PCI (bus=0x0%x, pci_id=0x%x, ",
		pdev->bus->number, pdev->devfn);
            printk("rev_id=%d) IRQ%d\n",
		cyy_rev_id, (int)cy_pci_irq);
            printk("Cyclom-Y/PCI:found  winaddr=0x%lx ctladdr=0x%lx\n",
		(ulong)cy_pci_phys2, (ulong)cy_pci_phys0);
#endif

		if (pci_resource_flags(pdev, 2) & IORESOURCE_IO) {
		    printk("  Warning: PCI I/O bit incorrectly set. "
			   "Ignoring it...\n");
		    pdev->resource[2].flags &= ~IORESOURCE_IO;
		}

		/* Although we don't use this I/O region, we should
		   request it from the kernel anyway, to avoid problems
		   with other drivers accessing it. */
		request_region(cy_pci_phys1, CyPCI_Yctl, "Cyclom-Y");

#if defined(__alpha__)
                if (device_id  == PCI_DEVICE_ID_CYCLOM_Y_Lo) { /* below 1M? */
		    printk("Cyclom-Y/PCI (bus=0x0%x, pci_id=0x%x, ",
			pdev->bus->number, pdev->devfn);
		    printk("rev_id=%d) IRQ%d\n",
		        cyy_rev_id, (int)cy_pci_irq);
                    printk("Cyclom-Y/PCI:found  winaddr=0x%lx ctladdr=0x%lx\n",
		        (ulong)cy_pci_phys2, (ulong)cy_pci_phys0);
	            printk("Cyclom-Y/PCI not supported for low addresses in "
                           "Alpha systems.\n");
		    i--;
	            continue;
                }
#endif
		cy_pci_addr0 = (ulong)ioremap(cy_pci_phys0, CyPCI_Yctl);
		cy_pci_addr2 = (ulong)ioremap(cy_pci_phys2, CyPCI_Ywin);

#ifdef CY_PCI_DEBUG
            printk("Cyclom-Y/PCI: relocate winaddr=0x%lx ctladdr=0x%lx\n",
		(u_long)cy_pci_addr2, (u_long)cy_pci_addr0);
#endif
                cy_pci_nchan = (unsigned short)(CyPORTS_PER_CHIP * 
                       cyy_init_card((volatile ucchar *)cy_pci_addr2, 1));
                if(cy_pci_nchan == 0) {
                        printk("Cyclom-Y PCI host card with ");
                        printk("no Serial-Modules at 0x%lx.\n",
			    (ulong) cy_pci_phys2);
                        i--;
                        continue;
                }
                if((cy_next_channel+cy_pci_nchan) > NR_PORTS) {
                        printk("Cyclom-Y/PCI found at 0x%lx ",
			    (ulong) cy_pci_phys2);
                        printk("but no channels are available.\n");
                        printk("Change NR_PORTS in cyclades.c and recompile kernel.\n");
                        return(i);
                }
                /* fill the next cy_card structure available */
                for (j = 0 ; j < NR_CARDS ; j++) {
                        if (cy_card[j].base_addr == 0)  break;
                }
                if (j == NR_CARDS) {    /* no more cy_cards available */
                        printk("Cyclom-Y/PCI found at 0x%lx ",
			    (ulong) cy_pci_phys2);
                        printk("but no more cards can be used.\n");
                        printk("Change NR_CARDS in cyclades.c and recompile kernel.\n");
                        return(i);
                }

                /* allocate IRQ */
                if(request_irq(cy_pci_irq, cyy_interrupt,
		        SA_SHIRQ, "Cyclom-Y", &cy_card[j]))
                {
                        printk("Cyclom-Y/PCI found at 0x%lx ",
			    (ulong) cy_pci_phys2);
                        printk("but could not allocate IRQ%d.\n",
			    cy_pci_irq);
                        return(i);
                }

                /* set cy_card */
                cy_card[j].base_phys = (ulong)cy_pci_phys2;
                cy_card[j].ctl_phys = (ulong)cy_pci_phys0;
                cy_card[j].base_addr = (ulong)cy_pci_addr2;
                cy_card[j].ctl_addr = (ulong)cy_pci_addr0;
                cy_card[j].irq = (int) cy_pci_irq;
                cy_card[j].bus_index = 1;
                cy_card[j].first_line = cy_next_channel;
                cy_card[j].num_chips = cy_pci_nchan/4;

                /* enable interrupts in the PCI interface */
		plx_ver = cy_readb(cy_pci_addr2 + CyPLX_VER) & 0x0f;
		switch (plx_ver) {
		    case PLX_9050:

		    cy_writeb(cy_pci_addr0+0x4c, 0x43);
		    break;

		    case PLX_9060:
		    case PLX_9080:
		    default: /* Old boards, use PLX_9060 */

		    plx_init(cy_pci_addr0, 0x6c);
		    /* For some yet unknown reason, once the PLX9060 reloads
		       the EEPROM, the IRQ is lost and, thus, we have to
		       re-write it to the PCI config. registers.
		       This will remain here until we find a permanent fix. */
		    pci_write_config_byte(pdev, PCI_INTERRUPT_LINE, cy_pci_irq);

		    cy_writew(cy_pci_addr0+0x68, 
			cy_readw(cy_pci_addr0+0x68)|0x0900);
		    break;
		}

                /* print message */
                printk("Cyclom-Y/PCI #%d: 0x%lx-0x%lx, IRQ%d, ",
		       j+1, 
		       (ulong)cy_pci_phys2, 
		       (ulong)(cy_pci_phys2 + CyPCI_Ywin - 1),
		       (int)cy_pci_irq);
                printk("%d channels starting from port %d.\n",
		    cy_pci_nchan, cy_next_channel);

                cy_next_channel += cy_pci_nchan;
    }else if (device_id == PCI_DEVICE_ID_CYCLOM_Z_Lo){
	    /* print message */
		printk("Cyclades-Z/PCI (bus=0x0%x, pci_id=0x%x, ",
		    pdev->bus->number, pdev->devfn);
		printk("rev_id=%d) IRQ%d\n",
		    cyy_rev_id, (int)cy_pci_irq);
		printk("Cyclades-Z/PCI: found winaddr=0x%lx ctladdr=0x%lx\n",
		    (ulong)cy_pci_phys2, (ulong)cy_pci_phys0);
	    printk("Cyclades-Z/PCI not supported for low addresses\n");
	    break;
    }else if (device_id == PCI_DEVICE_ID_CYCLOM_Z_Hi){
#ifdef CY_PCI_DEBUG
            printk("Cyclades-Z/PCI (bus=0x0%x, pci_id=0x%x, ",
	        pdev->bus->number, pdev->devfn);
            printk("rev_id=%d) IRQ%d\n",
		cyy_rev_id, (int)cy_pci_irq);
            printk("Cyclades-Z/PCI: found winaddr=0x%lx ctladdr=0x%lx\n",
                (ulong)cy_pci_phys2, (ulong)cy_pci_phys0);
#endif
		cy_pci_addr0 = (ulong)ioremap(cy_pci_phys0, CyPCI_Zctl);

		/* Disable interrupts on the PLX before resetting it */
		cy_writew(cy_pci_addr0+0x68,
			cy_readw(cy_pci_addr0+0x68) & ~0x0900);

		plx_init(cy_pci_addr0, 0x6c);
		/* For some yet unknown reason, once the PLX9060 reloads
		   the EEPROM, the IRQ is lost and, thus, we have to
		   re-write it to the PCI config. registers.
		   This will remain here until we find a permanent fix. */
		pci_write_config_byte(pdev, PCI_INTERRUPT_LINE, cy_pci_irq);

		mailbox = (uclong)cy_readl(&((struct RUNTIME_9060 *) 
			   cy_pci_addr0)->mail_box_0);

		if (pci_resource_flags(pdev, 2) & IORESOURCE_IO) {
		    printk("  Warning: PCI I/O bit incorrectly set. "
			   "Ignoring it...\n");
		    pdev->resource[2].flags &= ~IORESOURCE_IO;
		}

		/* Although we don't use this I/O region, we should
		   request it from the kernel anyway, to avoid problems
		   with other drivers accessing it. */
		request_region(cy_pci_phys1, CyPCI_Zctl, "Cyclades-Z");

		if (mailbox == ZE_V1) {
		    cy_pci_addr2 = (ulong)ioremap(cy_pci_phys2, CyPCI_Ze_win);
		    if (ZeIndex == NR_CARDS) {
			printk("Cyclades-Ze/PCI found at 0x%lx ",
				(ulong)cy_pci_phys2);
			printk("but no more cards can be used.\n");
                        printk("Change NR_CARDS in cyclades.c and recompile kernel.\n");
		    } else {
			Ze_phys0[ZeIndex] = cy_pci_phys0;
			Ze_phys2[ZeIndex] = cy_pci_phys2;
			Ze_addr0[ZeIndex] = cy_pci_addr0;
			Ze_addr2[ZeIndex] = cy_pci_addr2;
			Ze_irq[ZeIndex] = cy_pci_irq;
			ZeIndex++;
		    }
		    i--;
		    continue;
		} else {
		    cy_pci_addr2 = (ulong)ioremap(cy_pci_phys2, CyPCI_Zwin);
		}

#ifdef CY_PCI_DEBUG
            printk("Cyclades-Z/PCI: relocate winaddr=0x%lx ctladdr=0x%lx\n",
                (ulong)cy_pci_addr2, (ulong)cy_pci_addr0);
	    if (mailbox == ZO_V1) {
		cy_writel(&((struct RUNTIME_9060 *)
			  (cy_pci_addr0))->loc_addr_base, WIN_CREG);
		PAUSE
		printk("Cyclades-8Zo/PCI: FPGA id %lx, ver %lx\n",
		       (ulong)(0xff & cy_readl(&((struct CUSTOM_REG *)
		        (cy_pci_addr2))->fpga_id)),
		       (ulong)(0xff & cy_readl(&((struct CUSTOM_REG *)
		        (cy_pci_addr2))->fpga_version)));
		cy_writel(&((struct RUNTIME_9060 *)
			  (cy_pci_addr0))->loc_addr_base, WIN_RAM);
	    } else {
		printk("Cyclades-Z/PCI: New Cyclades-Z board.  FPGA not loaded\n");
	    }
#endif
	    /* The following clears the firmware id word.  This ensures
	       that the driver will not attempt to talk to the board
	       until it has been properly initialized.
	     */
		PAUSE
		if ((mailbox == ZO_V1) || (mailbox == ZO_V2))
		    cy_writel((ulong)(cy_pci_addr2+ID_ADDRESS), 0L);

                /* This must be a Cyclades-8Zo/PCI.  The extendable
                   version will have a different device_id and will
                   be allocated its maximum number of ports. */
                cy_pci_nchan = 8;

                if((cy_next_channel+cy_pci_nchan) > NR_PORTS) {
                        printk("Cyclades-8Zo/PCI found at 0x%lx ",
			    (ulong)cy_pci_phys2);
                        printk("but no channels are available.\n");
                        printk("Change NR_PORTS in cyclades.c and recompile kernel.\n");
                        return(i);
                }

                /* fill the next cy_card structure available */
                for (j = 0 ; j < NR_CARDS ; j++) {
                        if (cy_card[j].base_addr == 0)  break;
                }
                if (j == NR_CARDS) {    /* no more cy_cards available */
		    printk("Cyclades-8Zo/PCI found at 0x%lx ",
			(ulong)cy_pci_phys2);
		    printk("but no more cards can be used.\n");
                    printk("Change NR_CARDS in cyclades.c and recompile kernel.\n");
		    return(i);
                }

#ifdef CONFIG_CYZ_INTR
                /* allocate IRQ only if board has an IRQ */
		if( (cy_pci_irq != 0) && (cy_pci_irq != 255) ) {
		    if(request_irq(cy_pci_irq, cyz_interrupt,
			SA_SHIRQ, "Cyclades-Z", &cy_card[j]))
		    {
                        printk("Cyclom-8Zo/PCI found at 0x%lx ",
			    (ulong) cy_pci_phys2);
                        printk("but could not allocate IRQ%d.\n",
			    cy_pci_irq);
			return(i);
		    }
		}
#endif /* CONFIG_CYZ_INTR */


                /* set cy_card */
                cy_card[j].base_phys = cy_pci_phys2;
                cy_card[j].ctl_phys = cy_pci_phys0;
                cy_card[j].base_addr = cy_pci_addr2;
                cy_card[j].ctl_addr = cy_pci_addr0;
                cy_card[j].irq = (int) cy_pci_irq;
                cy_card[j].bus_index = 1;
                cy_card[j].first_line = cy_next_channel;
                cy_card[j].num_chips = -1;

                /* print message */
#ifdef CONFIG_CYZ_INTR
		/* don't report IRQ if board is no IRQ */
		if( (cy_pci_irq != 0) && (cy_pci_irq != 255) )
		    printk("Cyclades-8Zo/PCI #%d: 0x%lx-0x%lx, IRQ%d, ",
			j+1,(ulong)cy_pci_phys2,
			(ulong)(cy_pci_phys2 + CyPCI_Zwin - 1),
			(int)cy_pci_irq);
		else
#endif /* CONFIG_CYZ_INTR */
		    printk("Cyclades-8Zo/PCI #%d: 0x%lx-0x%lx, ",
			j+1,(ulong)cy_pci_phys2,
			(ulong)(cy_pci_phys2 + CyPCI_Zwin - 1));

                printk("%d channels starting from port %d.\n",
		    cy_pci_nchan,cy_next_channel);
                cy_next_channel += cy_pci_nchan;
    }
        }

        for (; ZeIndex != 0 && i < NR_CARDS; i++) {
	    cy_pci_phys0 = Ze_phys0[0];
	    cy_pci_phys2 = Ze_phys2[0];
	    cy_pci_addr0 = Ze_addr0[0];
	    cy_pci_addr2 = Ze_addr2[0];
	    cy_pci_irq = Ze_irq[0];
	    for (j = 0 ; j < ZeIndex-1 ; j++) {
		Ze_phys0[j] = Ze_phys0[j+1];
		Ze_phys2[j] = Ze_phys2[j+1];
		Ze_addr0[j] = Ze_addr0[j+1];
		Ze_addr2[j] = Ze_addr2[j+1];
		Ze_irq[j] = Ze_irq[j+1];
	    }
	    ZeIndex--;
		mailbox = (uclong)cy_readl(&((struct RUNTIME_9060 *) 
					   cy_pci_addr0)->mail_box_0);
#ifdef CY_PCI_DEBUG
            printk("Cyclades-Z/PCI: relocate winaddr=0x%lx ctladdr=0x%lx\n",
                (ulong)cy_pci_addr2, (ulong)cy_pci_addr0);
	    printk("Cyclades-Z/PCI: New Cyclades-Z board.  FPGA not loaded\n");
#endif
		PAUSE
                /* This must be the new Cyclades-Ze/PCI. */
                cy_pci_nchan = ZE_V1_NPORTS;

                if((cy_next_channel+cy_pci_nchan) > NR_PORTS) {
                        printk("Cyclades-Ze/PCI found at 0x%lx ",
			    (ulong)cy_pci_phys2);
                        printk("but no channels are available.\n");
                        printk("Change NR_PORTS in cyclades.c and recompile kernel.\n");
                        return(i);
                }

                /* fill the next cy_card structure available */
                for (j = 0 ; j < NR_CARDS ; j++) {
                        if (cy_card[j].base_addr == 0)  break;
                }
                if (j == NR_CARDS) {    /* no more cy_cards available */
		    printk("Cyclades-Ze/PCI found at 0x%lx ",
			(ulong)cy_pci_phys2);
		    printk("but no more cards can be used.\n");
                    printk("Change NR_CARDS in cyclades.c and recompile kernel.\n");
		    return(i);
                }

#ifdef CONFIG_CYZ_INTR
                /* allocate IRQ only if board has an IRQ */
		if( (cy_pci_irq != 0) && (cy_pci_irq != 255) ) {
		    if(request_irq(cy_pci_irq, cyz_interrupt,
			SA_SHIRQ, "Cyclades-Z", &cy_card[j]))
		    {
                        printk("Cyclom-Ze/PCI found at 0x%lx ",
			    (ulong) cy_pci_phys2);
                        printk("but could not allocate IRQ%d.\n",
			    cy_pci_irq);
			return(i);
		    }
		}
#endif /* CONFIG_CYZ_INTR */

                /* set cy_card */
                cy_card[j].base_phys = cy_pci_phys2;
                cy_card[j].ctl_phys = cy_pci_phys0;
                cy_card[j].base_addr = cy_pci_addr2;
                cy_card[j].ctl_addr = cy_pci_addr0;
                cy_card[j].irq = (int) cy_pci_irq;
                cy_card[j].bus_index = 1;
                cy_card[j].first_line = cy_next_channel;
                cy_card[j].num_chips = -1;

                /* print message */
#ifdef CONFIG_CYZ_INTR
		/* don't report IRQ if board is no IRQ */
		if( (cy_pci_irq != 0) && (cy_pci_irq != 255) )
		    printk("Cyclades-Ze/PCI #%d: 0x%lx-0x%lx, IRQ%d, ",
			j+1,(ulong)cy_pci_phys2,
			(ulong)(cy_pci_phys2 + CyPCI_Ze_win - 1),
			(int)cy_pci_irq);
		else
#endif /* CONFIG_CYZ_INTR */
		    printk("Cyclades-Ze/PCI #%d: 0x%lx-0x%lx, ",
			j+1,(ulong)cy_pci_phys2,
			(ulong)(cy_pci_phys2 + CyPCI_Ze_win - 1));

                printk("%d channels starting from port %d.\n",
		    cy_pci_nchan,cy_next_channel);
                cy_next_channel += cy_pci_nchan;
        }
	if (ZeIndex != 0) {
	    printk("Cyclades-Ze/PCI found at 0x%x ",
		(unsigned int) Ze_phys2[0]);
	    printk("but no more cards can be used.\n");
            printk("Change NR_CARDS in cyclades.c and recompile kernel.\n");
	}
        return(i);
#else
        return(0);
#endif /* ifdef CONFIG_PCI */
} /* cy_detect_pci */


/*
 * This routine prints out the appropriate serial driver version number
 * and identifies which options were configured into this driver.
 */
static inline void
show_version(void)
{
  char *rcsvers, *rcsdate, *tmp;
    rcsvers = strchr(rcsid, ' '); rcsvers++;
    tmp = strchr(rcsvers, ' '); *tmp++ = '\0';
    rcsdate = strchr(tmp, ' '); rcsdate++;
    tmp = strrchr(rcsdate, ' '); *tmp = '\0';
    printk("Cyclades driver %s %s\n",
        rcsvers, rcsdate);
    printk("        built %s %s\n",
	__DATE__, __TIME__);
} /* show_version */

static int 
cyclades_get_proc_info(char *buf, char **start, off_t offset, int length,
		       int *eof, void *data)
{
    struct cyclades_port  *info;
    int i;
    int len=0;
    off_t begin=0;
    off_t pos=0;
    int size;
    __u32 cur_jifs = jiffies;

    size = sprintf(buf, "Dev TimeOpen   BytesOut  IdleOut    BytesIn   IdleIn  Overruns  Ldisc\n");

    pos += size;
    len += size;

    /* Output one line for each known port */
    for (i = 0; i < NR_PORTS && cy_port[i].line >= 0; i++) {
	info = &cy_port[i];

	if (info->count)
	    size = sprintf(buf+len,
			"%3d %8lu %10lu %8lu %10lu %8lu %9lu %6ld\n",
			info->line,
			JIFFIES_DIFF(info->idle_stats.in_use, cur_jifs) / HZ,
			info->idle_stats.xmit_bytes,
			JIFFIES_DIFF(info->idle_stats.xmit_idle, cur_jifs) / HZ,
			info->idle_stats.recv_bytes,
			JIFFIES_DIFF(info->idle_stats.recv_idle, cur_jifs) / HZ,
			info->idle_stats.overruns,
			(long) info->tty->ldisc.num);
	else
	    size = sprintf(buf+len,
			"%3d %8lu %10lu %8lu %10lu %8lu %9lu %6ld\n",
			info->line, 0L, 0L, 0L, 0L, 0L, 0L, 0L);
	len += size;
	pos = begin + len;

	if (pos < offset) {
	    len   = 0;
	    begin = pos;
	}
	if (pos > offset + length)
	    goto done;
    }
    *eof = 1;
done:
    *start = buf + (offset - begin);	/* Start of wanted data */
    len -= (offset - begin);		/* Start slop */
    if (len > length)
	len = length;			/* Ending slop */
    if (len < 0)
	len = 0;
    return len;
}

/* The serial driver boot-time initialization code!
    Hardware I/O ports are mapped to character special devices on a
    first found, first allocated manner.  That is, this code searches
    for Cyclom cards in the system.  As each is found, it is probed
    to discover how many chips (and thus how many ports) are present.
    These ports are mapped to the tty ports 32 and upward in monotonic
    fashion.  If an 8-port card is replaced with a 16-port card, the
    port mapping on a following card will shift.

    This approach is different from what is used in the other serial
    device driver because the Cyclom is more properly a multiplexer,
    not just an aggregation of serial ports on one card.

    If there are more cards with more ports than have been
    statically allocated above, a warning is printed and the
    extra ports are ignored.
 */

int __init
cy_init(void)
{
  struct cyclades_port  *info;
  struct cyclades_card *cinfo;
  int number_z_boards = 0;
  int board,port,i,index;
  unsigned long mailbox;
  unsigned short chip_number;
  int nports;

    init_bh(CYCLADES_BH, do_cyclades_bh);

    show_version();

    /* Initialize the tty_driver structure */
    
    memset(&cy_serial_driver, 0, sizeof(struct tty_driver));
    cy_serial_driver.magic = TTY_DRIVER_MAGIC;
    cy_serial_driver.driver_name = "cyclades";
    cy_serial_driver.name = "ttyC";
    cy_serial_driver.major = CYCLADES_MAJOR;
    cy_serial_driver.minor_start = 0;
    cy_serial_driver.num = NR_PORTS;
    cy_serial_driver.type = TTY_DRIVER_TYPE_SERIAL;
    cy_serial_driver.subtype = SERIAL_TYPE_NORMAL;
    cy_serial_driver.init_termios = tty_std_termios;
    cy_serial_driver.init_termios.c_cflag =
            B9600 | CS8 | CREAD | HUPCL | CLOCAL;
    cy_serial_driver.flags = TTY_DRIVER_REAL_RAW;
    cy_serial_driver.refcount = &serial_refcount;
    cy_serial_driver.table = serial_table;
    cy_serial_driver.termios = serial_termios;
    cy_serial_driver.termios_locked = serial_termios_locked;

    cy_serial_driver.open = cy_open;
    cy_serial_driver.close = cy_close;
    cy_serial_driver.write = cy_write;
    cy_serial_driver.put_char = cy_put_char;
    cy_serial_driver.flush_chars = cy_flush_chars;
    cy_serial_driver.write_room = cy_write_room;
    cy_serial_driver.chars_in_buffer = cy_chars_in_buffer;
    cy_serial_driver.flush_buffer = cy_flush_buffer;
    cy_serial_driver.ioctl = cy_ioctl;
    cy_serial_driver.throttle = cy_throttle;
    cy_serial_driver.unthrottle = cy_unthrottle;
    cy_serial_driver.set_termios = cy_set_termios;
    cy_serial_driver.stop = cy_stop;
    cy_serial_driver.start = cy_start;
    cy_serial_driver.hangup = cy_hangup;
    cy_serial_driver.break_ctl = cy_break;
    cy_serial_driver.wait_until_sent = cy_wait_until_sent;
    cy_serial_driver.read_proc = cyclades_get_proc_info;

    /*
     * The callout device is just like normal device except for
     * major number and the subtype code.
     */
    cy_callout_driver = cy_serial_driver;
    cy_callout_driver.name = "cub";
    cy_callout_driver.major = CYCLADESAUX_MAJOR;
    cy_callout_driver.subtype = SERIAL_TYPE_CALLOUT;
    cy_callout_driver.read_proc = 0;
    cy_callout_driver.proc_entry = 0;


    if (tty_register_driver(&cy_serial_driver))
            panic("Couldn't register Cyclades serial driver\n");
    if (tty_register_driver(&cy_callout_driver))
            panic("Couldn't register Cyclades callout driver\n");

    for (i = 0; i < NR_CARDS; i++) {
            /* base_addr=0 indicates board not found */
            cy_card[i].base_addr = 0;
    }

    /* the code below is responsible to find the boards. Each different
       type of board has its own detection routine. If a board is found,
       the next cy_card structure available is set by the detection
       routine. These functions are responsible for checking the
       availability of cy_card and cy_port data structures and updating
       the cy_next_channel. */

    /* look for isa boards */
    cy_isa_nboard = cy_detect_isa();

    /* look for pci boards */
    cy_pci_nboard = cy_detect_pci();

    cy_nboard = cy_isa_nboard + cy_pci_nboard;

    /* invalidate remaining cy_card structures */
    for (i = 0 ; i < NR_CARDS ; i++) {
        if (cy_card[i].base_addr == 0) {
                cy_card[i].first_line = -1;
                cy_card[i].ctl_addr = 0;
                cy_card[i].irq = 0;
                cy_card[i].bus_index = 0;
                cy_card[i].first_line = 0;
                cy_card[i].num_chips = 0;
        }
    }
    /* invalidate remaining cy_port structures */
    for (i = cy_next_channel ; i < NR_PORTS ; i++) {
        cy_port[i].line = -1;
        cy_port[i].magic = -1;
    }

    /* initialize per-port data structures for each valid board found */
    for (board = 0 ; board < cy_nboard ; board++) {
            cinfo = &cy_card[board];
            if (cinfo->num_chips == -1) { /* Cyclades-Z */
		number_z_boards++;
		mailbox = cy_readl(&((struct RUNTIME_9060 *)
			     cy_card[board].ctl_addr)->mail_box_0);
		nports = (mailbox == ZE_V1) ? ZE_V1_NPORTS : 8;
		cinfo->intr_enabled = 0;
		cinfo->nports = 0; /* Will be correctly set later, after 
				      Z FW is loaded */
		spin_lock_init(&cinfo->card_lock);
                for (port = cinfo->first_line ;
                     port < cinfo->first_line + nports;
                     port++)
                {
                    info = &cy_port[port];
                    info->magic = CYCLADES_MAGIC;
                    info->type = PORT_STARTECH;
                    info->card = board;
                    info->line = port;
		    info->chip_rev = 0;
                    info->flags = STD_COM_FLAGS;
                    info->tty = 0;
		    if (mailbox == ZO_V1)
			info->xmit_fifo_size = CYZ_FIFO_SIZE;
		    else
			info->xmit_fifo_size = 4 * CYZ_FIFO_SIZE;
                    info->cor1 = 0;
                    info->cor2 = 0;
                    info->cor3 = 0;
                    info->cor4 = 0;
                    info->cor5 = 0;
                    info->tbpr = 0;
                    info->tco = 0;
                    info->rbpr = 0;
                    info->rco = 0;
		    info->custom_divisor = 0;
                    info->close_delay = 5*HZ/10;
		    info->closing_wait = CLOSING_WAIT_DELAY;
		    info->icount.cts = info->icount.dsr = 
			info->icount.rng = info->icount.dcd = 0;
		    info->icount.rx = info->icount.tx = 0;
		    info->icount.frame = info->icount.parity = 0;
		    info->icount.overrun = info->icount.brk = 0;
                    info->x_char = 0;
                    info->event = 0;
                    info->count = 0;
                    info->blocked_open = 0;
                    info->default_threshold = 0;
                    info->default_timeout = 0;
                    info->tqueue.routine = do_softint;
                    info->tqueue.data = info;
                    info->callout_termios =
		                cy_callout_driver.init_termios;
                    info->normal_termios =
		                cy_serial_driver.init_termios;
		    init_waitqueue_head(&info->open_wait);
		    init_waitqueue_head(&info->close_wait);
		    init_waitqueue_head(&info->shutdown_wait);
		    init_waitqueue_head(&info->delta_msr_wait);
                    /* info->session */
                    /* info->pgrp */
                    info->read_status_mask = 0;
                    /* info->timeout */
		    /* Bentson's vars */
                    info->jiffies[0] = 0;
                    info->jiffies[1] = 0;
                    info->jiffies[2] = 0;
                    info->rflush_count = 0;
#ifdef CONFIG_CYZ_INTR
		    cyz_rx_full_timer[port].function = NULL;
#endif
                }
                continue;
            }else{ /* Cyclom-Y of some kind*/
                index = cinfo->bus_index;
		spin_lock_init(&cinfo->card_lock);
		cinfo->nports = CyPORTS_PER_CHIP * cinfo->num_chips;
                for (port = cinfo->first_line ;
                     port < cinfo->first_line + cinfo->nports ;
                     port++)
                {
                    info = &cy_port[port];
                    info->magic = CYCLADES_MAGIC;
                    info->type = PORT_CIRRUS;
                    info->card = board;
                    info->line = port;
                    info->flags = STD_COM_FLAGS;
                    info->tty = 0;
                    info->xmit_fifo_size = CyMAX_CHAR_FIFO;
                    info->cor1 = CyPARITY_NONE|Cy_1_STOP|Cy_8_BITS;
                    info->cor2 = CyETC;
                    info->cor3 = 0x08; /* _very_ small rcv threshold */
                    info->cor4 = 0;
                    info->cor5 = 0;
		    info->custom_divisor = 0;
                    info->close_delay = 5*HZ/10;
		    info->closing_wait = CLOSING_WAIT_DELAY;
		    info->icount.cts = info->icount.dsr = 
			info->icount.rng = info->icount.dcd = 0;
		    info->icount.rx = info->icount.tx = 0;
		    info->icount.frame = info->icount.parity = 0;
		    info->icount.overrun = info->icount.brk = 0;
		    chip_number = (port - cinfo->first_line) / 4;
		    if ((info->chip_rev =
			 cy_readb(cinfo->base_addr +
				  (cy_chip_offset[chip_number]<<index) +
				  (CyGFRCR<<index))) >= CD1400_REV_J) {
                        /* It is a CD1400 rev. J or later */
                        info->tbpr = baud_bpr_60[13]; /* Tx BPR */
                        info->tco = baud_co_60[13]; /* Tx CO */
                        info->rbpr = baud_bpr_60[13]; /* Rx BPR */
                        info->rco = baud_co_60[13]; /* Rx CO */
                        info->rflow = 0;
                        info->rtsdtr_inv = 1;
                    } else {
                        info->tbpr = baud_bpr_25[13]; /* Tx BPR */
                        info->tco = baud_co_25[13]; /* Tx CO */
                        info->rbpr = baud_bpr_25[13]; /* Rx BPR */
                        info->rco = baud_co_25[13]; /* Rx CO */
                        info->rflow = 0;
                        info->rtsdtr_inv = 0;
                    }
                    info->x_char = 0;
                    info->event = 0;
                    info->count = 0;
                    info->blocked_open = 0;
                    info->default_threshold = 0;
                    info->default_timeout = 0;
                    info->tqueue.routine = do_softint;
                    info->tqueue.data = info;
                    info->callout_termios =
		               cy_callout_driver.init_termios;
                    info->normal_termios =
		               cy_serial_driver.init_termios;
		    init_waitqueue_head(&info->open_wait);
		    init_waitqueue_head(&info->close_wait);
		    init_waitqueue_head(&info->shutdown_wait);
		    init_waitqueue_head(&info->delta_msr_wait);
                    /* info->session */
                    /* info->pgrp */
                    info->read_status_mask =
		                  CyTIMEOUT| CySPECHAR| CyBREAK
                                  | CyPARITY| CyFRAME| CyOVERRUN;
                    /* info->timeout */
                }
            }
    }

#ifndef CONFIG_CYZ_INTR
    if (number_z_boards && !cyz_timeron){
	cyz_timeron++;
	cyz_timerlist.expires = jiffies + 1;
	add_timer(&cyz_timerlist);
#ifdef CY_PCI_DEBUG
	printk("Cyclades-Z polling initialized\n");
#endif
    }
#endif /* CONFIG_CYZ_INTR */

    return 0;
    
} /* cy_init */

#ifdef MODULE
void
cy_cleanup_module(void)
{
    int i;
    int e1, e2;
    unsigned long flags;

#ifndef CONFIG_CYZ_INTR
    if (cyz_timeron){
	cyz_timeron = 0;
	del_timer(&cyz_timerlist);
    }
#endif /* CONFIG_CYZ_INTR */

    save_flags(flags); cli();
    remove_bh(CYCLADES_BH);

    if ((e1 = tty_unregister_driver(&cy_serial_driver)))
            printk("cyc: failed to unregister Cyclades serial driver(%d)\n",
		e1);
    if ((e2 = tty_unregister_driver(&cy_callout_driver)))
            printk("cyc: failed to unregister Cyclades callout driver (%d)\n", 
		e2);

    restore_flags(flags);

    for (i = 0; i < NR_CARDS; i++) {
        if (cy_card[i].base_addr != 0) {
	    iounmap((void *)cy_card[i].base_addr);
	    if (cy_card[i].ctl_addr != 0)
		iounmap((void *)cy_card[i].ctl_addr);
	    if (cy_card[i].irq
#ifndef CONFIG_CYZ_INTR
		&& cy_card[i].num_chips != -1 /* not a Z card */
#endif /* CONFIG_CYZ_INTR */
	    )
		free_irq(cy_card[i].irq, &cy_card[i]);
        }
    }
    if (tmp_buf) {
	free_page((unsigned long) tmp_buf);
	tmp_buf = NULL;
    }
} /* cy_cleanup_module */

/* Module entry-points */
module_init(cy_init);
module_exit(cy_cleanup_module);

#else /* MODULE */
/* called by linux/init/main.c to parse command line options */
void
cy_setup(char *str, int *ints)
{
#ifdef CONFIG_ISA
  int i, j;

    for (i = 0 ; i < NR_ISA_ADDRS ; i++) {
        if (cy_isa_addresses[i] == 0) break;
    }
    for (j = 1; j <= ints[0]; j++){
        if ( i < NR_ISA_ADDRS ){
            cy_isa_addresses[i++] = (unsigned char *)(ints[j]);
        }
    }
#endif /* CONFIG_ISA */
} /* cy_setup */
#endif /* MODULE */

