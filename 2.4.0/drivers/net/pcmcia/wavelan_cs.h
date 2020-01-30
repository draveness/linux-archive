/*
 *	Wavelan Pcmcia driver
 *
 *		Jean II - HPLB '96
 *
 * Reorganisation and extension of the driver.
 *
 * This file contain all definition and declarations necessary for the
 * wavelan pcmcia driver. This file is a private header, so it should
 * be included only on wavelan_cs.c !!!
 */

#ifndef WAVELAN_CS_H
#define WAVELAN_CS_H

/************************** DOCUMENTATION **************************/
/*
 * This driver provide a Linux interface to the Wavelan Pcmcia hardware
 * The Wavelan is a product of Lucent (http://www.wavelan.com/).
 * This division was formerly part of NCR and then AT&T.
 * Wavelan are also distributed by DEC (RoamAbout DS)...
 *
 * To know how to use this driver, read the PCMCIA HOWTO.
 * If you want to exploit the many other fonctionalities, look comments
 * in the code...
 *
 * This driver is the result of the effort of many peoples (see below).
 */

/* ------------------------ SPECIFIC NOTES ------------------------ */
/*
 * Web page
 * --------
 *	I try to maintain a web page with the Wireless LAN Howto at :
 *	    http://www.hpl.hp.com/personal/Jean_Tourrilhes/Linux/Wavelan.html
 *
 * Debugging and options
 * ---------------------
 *	You will find below a set of '#define" allowing a very fine control
 *	on the driver behaviour and the debug messages printed.
 *	The main options are :
 *	o WAVELAN_ROAMING, for the experimental roaming support.
 *	o SET_PSA_CRC, to have your card correctly recognised by
 *	  an access point and the Point-to-Point diagnostic tool.
 *	o USE_PSA_CONFIG, to read configuration from the PSA (EEprom)
 *	  (otherwise we always start afresh with some defaults)
 *
 * wavelan_cs.o is darn too big
 * -------------------------
 *	That's true ! There is a very simple way to reduce the driver
 *	object by 33% (yes !). Comment out the following line :
 *		#include <linux/wireless.h>
 *	Other compile options can also reduce the size of it...
 *
 * MAC address and hardware detection :
 * ----------------------------------
 *	The detection code of the wavelan chech that the first 3
 *	octets of the MAC address fit the company code. This type of
 *	detection work well for AT&T cards (because the AT&T code is
 *	hardcoded in wavelan.h), but of course will fail for other
 *	manufacturer.
 *
 *	If you are sure that your card is derived from the wavelan,
 *	here is the way to configure it :
 *	1) Get your MAC address
 *		a) With your card utilities (wfreqsel, instconf, ...)
 *		b) With the driver :
 *			o compile the kernel with DEBUG_CONFIG_INFO enabled
 *			o Boot and look the card messages
 *	2) Set your MAC code (3 octets) in MAC_ADDRESSES[][3] (wavelan.h)
 *	3) Compile & verify
 *	4) Send me the MAC code - I will include it in the next version...
 *
 */

/* --------------------- WIRELESS EXTENSIONS --------------------- */
/*
 * This driver is the first one to support "wireless extensions".
 * This set of extensions provide you some way to control the wireless
 * caracteristics of the hardware in a standard way and support for
 * applications for taking advantage of it (like Mobile IP).
 *
 * You will need to enable the CONFIG_NET_RADIO define in the kernel
 * configuration to enable the wireless extensions (this is the one
 * giving access to the radio network device choice).
 *
 * It might also be a good idea as well to fetch the wireless tools to
 * configure the device and play a bit.
 */

/* ---------------------------- FILES ---------------------------- */
/*
 * wavelan_cs.c :	The actual code for the driver - C functions
 *
 * wavelan_cs.h :	Private header : local types / vars for the driver
 *
 * wavelan.h :		Description of the hardware interface & structs
 *
 * i82593.h :		Description if the Ethernet controler
 */

/* --------------------------- HISTORY --------------------------- */
/*
 * The history of the Wavelan drivers is as complicated as history of
 * the Wavelan itself (NCR -> AT&T -> Lucent).
 *
 * All started with Anders Klemets <klemets@paul.rutgers.edu>,
 * writting a Wavelan ISA driver for the MACH microkernel. Girish
 * Welling <welling@paul.rutgers.edu> had also worked on it.
 * Keith Moore modify this for the Pcmcia hardware.
 * 
 * Robert Morris <rtm@das.harvard.edu> port these two drivers to BSDI
 * and add specific Pcmcia support (there is currently no equivalent
 * of the PCMCIA package under BSD...).
 *
 * Jim Binkley <jrb@cs.pdx.edu> port both BSDI drivers to FreeBSD.
 *
 * Bruce Janson <bruce@cs.usyd.edu.au> port the BSDI ISA driver to Linux.
 *
 * Anthony D. Joseph <adj@lcs.mit.edu> started modify Bruce driver
 * (with help of the BSDI PCMCIA driver) for PCMCIA.
 * Yunzhou Li <yunzhou@strat.iol.unh.edu> finished is work.
 * Joe Finney <joe@comp.lancs.ac.uk> patched the driver to start
 * correctly 2.00 cards (2.4 GHz with frequency selection).
 * David Hinds <dhinds@pcmcia.sourceforge.org> integrated the whole in his
 * Pcmcia package (+ bug corrections).
 *
 * I (Jean Tourrilhes - jt@hplb.hpl.hp.com) then started to make some
 * patchs to the Pcmcia driver. After, I added code in the ISA driver
 * for Wireless Extensions and full support of frequency selection
 * cards. Now, I'm doing the same to the Pcmcia driver + some
 * reorganisation.
 * Loeke Brederveld <lbrederv@wavelan.com> from Lucent has given me
 * much needed informations on the Wavelan hardware.
 */

/* By the way : for the copyright & legal stuff :
 * Almost everybody wrote code under GNU or BSD license (or alike),
 * and want that their original copyright remain somewhere in the
 * code (for myself, I go with the GPL).
 * Nobody want to take responsibility for anything, except the fame...
 */

/* --------------------------- CREDITS --------------------------- */
/*
 * Credits:
 *    Special thanks to Jan Hoogendoorn of AT&T GIS Utrecht and
 *	Loeke Brederveld of Lucent for providing extremely useful
 *	information about WaveLAN PCMCIA hardware
 *
 *    This driver is based upon several other drivers, in particular:
 *	David Hinds' Linux driver for the PCMCIA 3c589 ethernet adapter
 *	Bruce Janson's Linux driver for the AT-bus WaveLAN adapter
 *	Anders Klemets' PCMCIA WaveLAN adapter driver
 *	Robert Morris' BSDI driver for the PCMCIA WaveLAN adapter
 *
 * Additional Credits:
 *
 *    This software was originally developed under Linux 1.2.3
 *	(Slackware 2.0 distribution).
 *    And then under Linux 2.0.x (Debian 1.1 - pcmcia 2.8.18-23) with
 *	HP OmniBook 4000 & 5500.
 *
 *    It is based on other device drivers and information either written
 *    or supplied by:
 *	James Ashton (jaa101@syseng.anu.edu.au),
 *	Ajay Bakre (bakre@paul.rutgers.edu),
 *	Donald Becker (becker@super.org),
 *	Jim Binkley <jrb@cs.pdx.edu>,
 *	Loeke Brederveld <lbrederv@wavelan.com>,
 *	Allan Creighton (allanc@cs.su.oz.au),
 *	Brent Elphick <belphick@uwaterloo.ca>,
 *	Joe Finney <joe@comp.lancs.ac.uk>,
 *	Matthew Geier (matthew@cs.su.oz.au),
 *	Remo di Giovanni (remo@cs.su.oz.au),
 *	Mark Hagan (mhagan@wtcpost.daytonoh.NCR.COM),
 *	David Hinds <dhinds@pcmcia.sourceforge.org>,
 *	Jan Hoogendoorn (c/o marteijn@lucent.com),
 *      Bruce Janson <bruce@cs.usyd.edu.au>,
 *	Anthony D. Joseph <adj@lcs.mit.edu>,
 *	Anders Klemets (klemets@paul.rutgers.edu),
 *	Yunzhou Li <yunzhou@strat.iol.unh.edu>,
 *	Marc Meertens (mmeertens@lucent.com),
 *	Keith Moore,
 *	Robert Morris (rtm@das.harvard.edu),
 *	Ian Parkin (ian@cs.su.oz.au),
 *	John Rosenberg (johnr@cs.su.oz.au),
 *	George Rossi (george@phm.gov.au),
 *	Arthur Scott (arthur@cs.su.oz.au),
 *	Stanislav Sinyagin <stas@isf.ru>
 *	Peter Storey,
 *	Jean Tourrilhes <jt@hpl.hp.com>,
 *	Girish Welling (welling@paul.rutgers.edu)
 *	Clark Woodworth <clark@hiway1.exit109.com>
 *	Yongguang Zhang <ygz@isl.hrl.hac.com>...
 */

/* ------------------------- IMPROVEMENTS ------------------------- */
/*
 * I proudly present :
 *
 * Changes made in 2.8.22 :
 * ----------------------
 *	- improved wv_set_multicast_list
 *	- catch spurious interrupt
 *	- correct release of the device
 *
 * Changes mades in release :
 * ------------------------
 *	- Reorganisation of the code, function name change
 *	- Creation of private header (wavelan_cs.h)
 *	- Reorganised debug messages
 *	- More comments, history, ...
 *	- Configure earlier (in "insert" instead of "open")
 *        and do things only once
 *	- mmc_init : configure the PSA if not done
 *	- mmc_init : 2.00 detection better code for 2.00 init
 *	- better info at startup
 *	- Correct a HUGE bug (volatile & uncalibrated busy loop)
 *	  in wv_82593_cmd => config speedup
 *	- Stop receiving & power down on close (and power up on open)
 *	  use "ifconfig down" & "ifconfig up ; route add -net ..."
 *	- Send packets : add watchdog instead of pooling
 *	- Receive : check frame wrap around & try to recover some frames
 *	- wavelan_set_multicast_list : avoid reset
 *	- add wireless extensions (ioctl & get_wireless_stats)
 *	  get/set nwid/frequency on fly, info for /proc/net/wireless
 *	- Supress useless stuff from lp (net_local), but add link
 *	- More inlines
 *	- Lot of others minor details & cleanups
 *
 * Changes made in second release :
 * ------------------------------
 *	- Optimise wv_85893_reconfig stuff, fix potential problems
 *	- Change error values for ioctl
 *	- Non blocking wv_ru_stop() + call wv_reset() in case of problems
 *	- Remove development printk from wavelan_watchdog()
 *	- Remove of the watchdog to wavelan_close instead of wavelan_release
 *	  fix potential problems...
 *	- Start debugging suspend stuff (but it's still a bit weird)
 *	- Debug & optimize dump header/packet in Rx & Tx (debug)
 *	- Use "readb" and "writeb" to be kernel 2.1 compliant
 *	- Better handling of bogus interrupts
 *	- Wireless extension : SETSPY and GETSPY
 *	- Remove old stuff (stats - for those needing it, just ask me...)
 *	- Make wireless extensions optional
 *
 * Changes made in third release :
 * -----------------------------
 *	- cleanups & typos
 *	- modif wireless ext (spy -> only one pointer)
 *	- new private ioctl to set/get quality & level threshold
 *	- Init : correct default value of level threshold for pcmcia
 *	- kill watchdog in hw_reset
 *	- more 2.1 support (copy_to/from_user instead of memcpy_to/fromfs)
 *	- Add message level (debug stuff in /var/adm/debug & errors not
 *	  displayed at console and still in /var/adm/messages)
 *
 * Changes made in fourth release :
 * ------------------------------
 *	- multicast support (yes !) thanks to Yongguang Zhang.
 *
 * Changes made in fifth release (2.9.0) :
 * -------------------------------------
 *	- Revisited multicast code (it was mostly wrong).
 *	- protect code in wv_82593_reconfig with dev->tbusy (oups !)
 *
 * Changes made in sixth release (2.9.1a) :
 * --------------------------------------
 *	- Change the detection code for multi manufacturer code support
 *	- Correct bug (hang kernel) in init when we were "rejecting" a card 
 *
 * Changes made in seventh release (2.9.1b) :
 * ----------------------------------------
 *	- Update to wireless extensions changes
 *	- Silly bug in card initial configuration (psa_conf_status)
 *
 * Changes made in eigth release :
 * -----------------------------
 *	- Small bug in debug code (probably not the last one...)
 *	- 1.2.13 support (thanks to Clark Woodworth)
 *
 * Changes made for release in 2.9.2b :
 * ----------------------------------
 *	- Level threshold is now a standard wireless extension (version 4 !)
 *	- modules parameters types for kernel > 2.1.17
 *	- updated man page
 *	- Others cleanup from David Hinds
 *
 * Changes made for release in 2.9.5 :
 * ---------------------------------
 *	- byte count stats (courtesy of David Hinds)
 *	- Remove dev_tint stuff (courtesy of David Hinds)
 *	- Others cleanup from David Hinds
 *	- Encryption setting from Brent Elphick (thanks a lot !)
 *	- 'base' to 'u_long' for the Alpha (thanks to Stanislav Sinyagin)
 *
 * Changes made for release in 2.9.6 :
 * ---------------------------------
 *	- fix bug : no longuer disable watchdog in case of bogus interrupt
 *	- increase timeout in config code for picky hardware
 *	- mask unused bits in status (Wireless Extensions)
 *
 * Changes integrated by Justin Seger <jseger@MIT.EDU> & David Hinds :
 * -----------------------------------------------------------------
 *	- Roaming "hack" from Joe Finney <joe@comp.lancs.ac.uk>
 *	- PSA CRC code from Bob Gray <rgray@bald.cs.dartmouth.edu>
 *	- Better initialisation of the i82593 controller
 *	  from Joseph K. O'Sullivan <josullvn+@cs.cmu.edu>
 *
 * Changes made for release in 3.0.10 :
 * ----------------------------------
 *	- Fix eject "hang" of the driver under 2.2.X :
 *		o create wv_flush_stale_links()
 *		o Rename wavelan_release to wv_pcmcia_release & move up
 *		o move unregister_netdev to wavelan_detach()
 *		o wavelan_release() no longer call wavelan_detach()
 *		o Supress "release" timer
 *		o Other cleanups & fixes
 *	- New MAC address in the probe
 *	- Reorg PSA_CRC code (endian neutral & cleaner)
 *	- Correct initialisation of the i82593 from Lucent manual
 *	- Put back the watchdog, with larger timeout
 *	- TRANSMIT_NO_CRC is a "normal" error, so recover from it
 *	  from Derrick J Brashear <shadow@dementia.org>
 *	- Better handling of TX and RX normal failure conditions
 *	- #ifdef out all the roaming code
 *	- Add ESSID & "AP current address" ioctl stubs
 *	- General cleanup of the code
 *
 * Changes made for release in 3.0.13 :
 * ----------------------------------
 *	- Re-enable compilation of roaming code by default, but with
 *	  do_roaming = 0
 *	- Nuke `nwid=nwid^ntohs(beacon->domain_id)' in wl_roam_gather
 *	  at the demand of John Carol Langford <jcl@gs176.sp.cs.cmu.edu>
 *	- Introduced WAVELAN_ROAMING_EXT for incomplete ESSID stuff.
 *
 * Changes made for release in 3.0.15 :
 * ----------------------------------
 *	- Change e-mail and web page addresses
 *	- Watchdog timer is now correctly expressed in HZ, not in jiffies
 *	- Add channel number to the list of frequencies in range
 *	- Add the (short) list of bit-rates in range
 *	- Developp a new sensitivity... (sens.value & sens.fixed)
 *
 * Changes made for release in 3.1.2 :
 * ---------------------------------
 *	- Fix check for root permission (break instead of exit)
 *	- New nwid & encoding setting (Wireless Extension 9)
 *
 * Wishes & dreams:
 * ----------------
 *	- Cleanup and integrate the roaming code
 *	  (std debug, set DomainID, decay avg and co...)
 */

/***************************** INCLUDES *****************************/

/* Linux headers that we need */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/ptrace.h>
#include <linux/malloc.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/in.h>
#include <linux/delay.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/system.h>
#include <asm/bitops.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/ioport.h>
#include <linux/fcntl.h>

#ifdef CONFIG_NET_PCMCIA_RADIO
#include <linux/wireless.h>		/* Wireless extensions */
#endif	/* CONFIG_NET_PCMCIA_RADIO */

/* Pcmcia headers that we need */
#include <pcmcia/cs_types.h>
#include <pcmcia/cs.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/cisreg.h>
#include <pcmcia/ds.h>
#include <pcmcia/version.h>

/* Wavelan declarations */
#include "i82593.h"	/* Definitions for the Intel chip */

#include "wavelan.h"	/* Others bits of the hardware */

/************************** DRIVER OPTIONS **************************/
/*
 * `#define' or `#undef' the following constant to change the behaviour
 * of the driver...
 */
#define WAVELAN_ROAMING		/* Include experimental roaming code */
#undef WAVELAN_ROAMING_EXT	/* Enable roaming wireless extensions */
#undef SET_PSA_CRC		/* Set the CRC in PSA (slower) */
#define USE_PSA_CONFIG		/* Use info from the PSA */
#undef STRUCT_CHECK		/* Verify padding of structures */
#undef EEPROM_IS_PROTECTED	/* Doesn't seem to be necessary */
#define MULTICAST_AVOID		/* Avoid extra multicast (I'm sceptical) */
#undef SET_MAC_ADDRESS		/* Experimental */

#ifdef WIRELESS_EXT	/* If wireless extension exist in the kernel */
/* Warning : these stuff will slow down the driver... */
#define WIRELESS_SPY		/* Enable spying addresses */
#undef HISTOGRAM		/* Enable histogram of sig level... */
#endif

/****************************** DEBUG ******************************/

#undef DEBUG_MODULE_TRACE	/* Module insertion/removal */
#undef DEBUG_CALLBACK_TRACE	/* Calls made by Linux */
#undef DEBUG_INTERRUPT_TRACE	/* Calls to handler */
#undef DEBUG_INTERRUPT_INFO	/* type of interrupt & so on */
#define DEBUG_INTERRUPT_ERROR	/* problems */
#undef DEBUG_CONFIG_TRACE	/* Trace the config functions */
#undef DEBUG_CONFIG_INFO	/* What's going on... */
#define DEBUG_CONFIG_ERRORS	/* Errors on configuration */
#undef DEBUG_TX_TRACE		/* Transmission calls */
#undef DEBUG_TX_INFO		/* Header of the transmited packet */
#undef DEBUG_TX_FAIL		/* Normal failure conditions */
#define DEBUG_TX_ERROR		/* Unexpected conditions */
#undef DEBUG_RX_TRACE		/* Transmission calls */
#undef DEBUG_RX_INFO		/* Header of the transmited packet */
#undef DEBUG_RX_FAIL		/* Normal failure conditions */
#define DEBUG_RX_ERROR		/* Unexpected conditions */
#undef DEBUG_PACKET_DUMP	32	/* Dump packet on the screen */
#undef DEBUG_IOCTL_TRACE	/* Misc call by Linux */
#undef DEBUG_IOCTL_INFO		/* Various debug info */
#define DEBUG_IOCTL_ERROR	/* What's going wrong */
#define DEBUG_BASIC_SHOW	/* Show basic startup info */
#undef DEBUG_VERSION_SHOW	/* Print version info */
#undef DEBUG_PSA_SHOW		/* Dump psa to screen */
#undef DEBUG_MMC_SHOW		/* Dump mmc to screen */
#undef DEBUG_SHOW_UNUSED	/* Show also unused fields */
#undef DEBUG_I82593_SHOW	/* Show i82593 status */
#undef DEBUG_DEVICE_SHOW	/* Show device parameters */

/************************ CONSTANTS & MACROS ************************/

#ifdef DEBUG_VERSION_SHOW
static const char *version = "wavelan_cs.c : v21 (wireless extensions) 18/10/99\n";
#endif

/* Watchdog temporisation */
#define	WATCHDOG_JIFFIES	(256*HZ/100)

/* Fix a bug in some old wireless extension definitions */
#ifndef IW_ESSID_MAX_SIZE
#define IW_ESSID_MAX_SIZE	32
#endif

/* ------------------------ PRIVATE IOCTL ------------------------ */

#define SIOCSIPQTHR	SIOCDEVPRIVATE		/* Set quality threshold */
#define SIOCGIPQTHR	SIOCDEVPRIVATE + 1	/* Get quality threshold */
#define SIOCSIPROAM     SIOCDEVPRIVATE + 2      /* Set roaming state */
#define SIOCGIPROAM     SIOCDEVPRIVATE + 3      /* Get roaming state */

#define SIOCSIPHISTO	SIOCDEVPRIVATE + 6	/* Set histogram ranges */
#define SIOCGIPHISTO	SIOCDEVPRIVATE + 7	/* Get histogram values */

/*************************** WaveLAN Roaming  **************************/
#ifdef WAVELAN_ROAMING		/* Conditional compile, see above in options */

#define WAVELAN_ROAMING_DEBUG	 0	/* 1 = Trace of handover decisions */
					/* 2 = Info on each beacon rcvd... */
#define MAX_WAVEPOINTS		7	/* Max visible at one time */
#define WAVEPOINT_HISTORY	5	/* SNR sample history slow search */
#define WAVEPOINT_FAST_HISTORY	2	/* SNR sample history fast search */
#define SEARCH_THRESH_LOW	10	/* SNR to enter cell search */
#define SEARCH_THRESH_HIGH	13	/* SNR to leave cell search */
#define WAVELAN_ROAMING_DELTA	1	/* Hysteresis value (+/- SNR) */
#define CELL_TIMEOUT		2*HZ	/* in jiffies */

#define FAST_CELL_SEARCH	1	/* Boolean values... */
#define NWID_PROMISC		1	/* for code clarity. */

typedef struct wavepoint_beacon
{
  unsigned char		dsap,		/* Unused */
			ssap,		/* Unused */
			ctrl,		/* Unused */
			O,U,I,		/* Unused */
			spec_id1,	/* Unused */
			spec_id2,	/* Unused */
			pdu_type,	/* Unused */
			seq;		/* WavePoint beacon sequence number */
  unsigned short	domain_id,	/* WavePoint Domain ID */
			nwid;		/* WavePoint NWID */
} wavepoint_beacon;

typedef struct wavepoint_history
{
  unsigned short	nwid;		/* WavePoint's NWID */
  int			average_slow;	/* SNR running average */
  int			average_fast;	/* SNR running average */
  unsigned char	  sigqual[WAVEPOINT_HISTORY]; /* Ringbuffer of recent SNR's */
  unsigned char		qualptr;	/* Index into ringbuffer */
  unsigned char		last_seq;	/* Last seq. no seen for WavePoint */
  struct wavepoint_history *next;	/* Next WavePoint in table */
  struct wavepoint_history *prev;	/* Previous WavePoint in table */
  unsigned long		last_seen;	/* Time of last beacon recvd, jiffies */
} wavepoint_history;

struct wavepoint_table
{
  wavepoint_history	*head;		/* Start of ringbuffer */
  int			num_wavepoints;	/* No. of WavePoints visible */
  unsigned char		locked;		/* Table lock */
};

#endif	/* WAVELAN_ROAMING */

/****************************** TYPES ******************************/

/* Shortcuts */
typedef struct net_device	device;
typedef struct net_device_stats	en_stats;
typedef struct iw_statistics	iw_stats;
typedef struct iw_quality	iw_qual;
typedef struct iw_freq		iw_freq;
typedef struct net_local	net_local;
typedef struct timer_list	timer_list;

/* Basic types */
typedef u_char		mac_addr[WAVELAN_ADDR_SIZE];	/* Hardware address */

/*
 * Static specific data for the interface.
 *
 * For each network interface, Linux keep data in two structure. "device"
 * keep the generic data (same format for everybody) and "net_local" keep
 * the additional specific data.
 * Note that some of this specific data is in fact generic (en_stats, for
 * example).
 */
struct net_local
{
  spinlock_t	lock;
  dev_node_t 	node;		/* ???? What is this stuff ???? */
  device *	dev;		/* Reverse link... */
  dev_link_t *	link;		/* pcmcia structure */
  en_stats	stats;		/* Ethernet interface statistics */
  int		nresets;	/* Number of hw resets */
  u_char	configured;	/* If it is configured */
  u_char	reconfig_82593;	/* Need to reconfigure the controler */
  u_char	promiscuous;	/* Promiscuous mode */
  u_char	allmulticast;	/* All Multicast mode */
  int		mc_count;	/* Number of multicast addresses */
  timer_list	watchdog;	/* To avoid blocking state */

  u_char        status;		/* Current i82593 status */
  int   	stop;		/* Current i82593 Stop Hit Register */
  int   	rfp;		/* Last DMA machine receive pointer */
  int		overrunning;	/* Receiver overrun flag */

#ifdef WIRELESS_EXT
  iw_stats	wstats;		/* Wireless specific stats */
#endif

#ifdef WIRELESS_SPY
  int		spy_number;		/* Number of addresses to spy */
  mac_addr	spy_address[IW_MAX_SPY];	/* The addresses to spy */
  iw_qual	spy_stat[IW_MAX_SPY];		/* Statistics gathered */
#endif	/* WIRELESS_SPY */
#ifdef HISTOGRAM
  int		his_number;		/* Number of intervals */
  u_char	his_range[16];		/* Boundaries of interval ]n-1; n] */
  u_long	his_sum[16];		/* Sum in interval */
#endif	/* HISTOGRAM */
#ifdef WAVELAN_ROAMING
  u_long	domain_id;	/* Domain ID we lock on for roaming */
  int		filter_domains;	/* Check Domain ID of beacon found */
 struct wavepoint_table	wavepoint_table;	/* Table of visible WavePoints*/
  wavepoint_history *	curr_point;		/* Current wavepoint */
  int			cell_search;		/* Searching for new cell? */
  struct timer_list	cell_timer;		/* Garbage collection */
#endif	/* WAVELAN_ROAMING */
};

/**************************** PROTOTYPES ****************************/

#ifdef WAVELAN_ROAMING
/* ---------------------- ROAMING SUBROUTINES -----------------------*/

wavepoint_history *wl_roam_check(unsigned short nwid, net_local *lp);
wavepoint_history *wl_new_wavepoint(unsigned short nwid, unsigned char seq, net_local *lp);
void wl_del_wavepoint(wavepoint_history *wavepoint, net_local *lp);
void wl_cell_expiry(unsigned long data);
wavepoint_history *wl_best_sigqual(int fast_search, net_local *lp);
void wl_update_history(wavepoint_history *wavepoint, unsigned char sigqual, unsigned char seq);
void wv_roam_handover(wavepoint_history *wavepoint, net_local *lp);
void wv_nwid_filter(unsigned char mode, net_local *lp);
void wv_roam_init(struct net_device *dev);
void wv_roam_cleanup(struct net_device *dev);
#endif	/* WAVELAN_ROAMING */

/* ----------------------- MISC SUBROUTINES ------------------------ */
static void
	cs_error(client_handle_t, /* Report error to cardmgr */
		 int,
		 int);
/* ----------------- MODEM MANAGEMENT SUBROUTINES ----------------- */
static inline u_char		/* data */
	hasr_read(u_long);	/* Read the host interface : base address */
static inline void
	hacr_write(u_long,	/* Write to host interface : base address */
		   u_char),	/* data */
	hacr_write_slow(u_long,
		   u_char);
static void
	psa_read(device *,	/* Read the Parameter Storage Area */
		 int,		/* offset in PSA */
		 u_char *,	/* buffer to fill */
		 int),		/* size to read */
	psa_write(device *,	/* Write to the PSA */
		  int,		/* Offset in psa */
		  u_char *,	/* Buffer in memory */
		  int);		/* Length of buffer */
static inline void
	mmc_out(u_long,		/* Write 1 byte to the Modem Manag Control */
		u_short,
		u_char),
	mmc_write(u_long,	/* Write n bytes to the MMC */
		  u_char,
		  u_char *,
		  int);
static inline u_char		/* Read 1 byte from the MMC */
	mmc_in(u_long,
	       u_short);
static inline void
	mmc_read(u_long,	/* Read n bytes from the MMC */
		 u_char,
		 u_char *,
		 int),
	fee_wait(u_long,	/* Wait for frequency EEprom : base address */
		 int,		/* Base delay to wait for */
		 int);		/* Number of time to wait */
static void
	fee_read(u_long,	/* Read the frequency EEprom : base address */
		 u_short,	/* destination offset */
		 u_short *,	/* data buffer */
		 int);		/* number of registers */
/* ---------------------- I82593 SUBROUTINES ----------------------- */
static int
	wv_82593_cmd(device *,	/* synchronously send a command to i82593 */ 
		     char *,
		     int,
		     int);
static inline int
	wv_diag(device *);	/* Diagnostique the i82593 */
static int
	read_ringbuf(device *,	/* Read a receive buffer */
		     int,
		     char *,
		     int);
static inline void
	wv_82593_reconfig(device *);	/* Reconfigure the controler */
/* ------------------- DEBUG & INFO SUBROUTINES ------------------- */
static inline void
	wv_init_info(device *);	/* display startup info */
/* ------------------- IOCTL, STATS & RECONFIG ------------------- */
static en_stats	*
	wavelan_get_stats(device *);	/* Give stats /proc/net/dev */
/* ----------------------- PACKET RECEPTION ----------------------- */
static inline int
	wv_start_of_frame(device *,	/* Seek beggining of current frame */
			  int,	/* end of frame */
			  int);	/* start of buffer */
static inline void
	wv_packet_read(device *,	/* Read a packet from a frame */
		       int,
		       int),
	wv_packet_rcv(device *);	/* Read all packets waiting */
/* --------------------- PACKET TRANSMISSION --------------------- */
static inline void
	wv_packet_write(device *,	/* Write a packet to the Tx buffer */
			void *,
			short);
static int
	wavelan_packet_xmit(struct sk_buff *,	/* Send a packet */
			    device *);
/* -------------------- HARDWARE CONFIGURATION -------------------- */
static inline int
	wv_mmc_init(device *);	/* Initialize the modem */
static int
	wv_ru_stop(device *),	/* Stop the i82593 receiver unit */
	wv_ru_start(device *);	/* Start the i82593 receiver unit */
static int
	wv_82593_config(device *);	/* Configure the i82593 */
static inline int
	wv_pcmcia_reset(device *);	/* Reset the pcmcia interface */
static int
	wv_hw_config(device *);	/* Reset & configure the whole hardware */
static inline void
	wv_hw_reset(device *);	/* Same, + start receiver unit */
static inline int
	wv_pcmcia_config(dev_link_t *);	/* Configure the pcmcia interface */
static void
	wv_pcmcia_release(u_long),	/* Remove a device */
	wv_flush_stale_links(void);	/* "detach" all possible devices */
/* ---------------------- INTERRUPT HANDLING ---------------------- */
static void
wavelan_interrupt(int,	/* Interrupt handler */
		  void *,
		  struct pt_regs *);
static void
	wavelan_watchdog(u_long);	/* Transmission watchdog */
/* ------------------- CONFIGURATION CALLBACKS ------------------- */
static int
	wavelan_open(device *),		/* Open the device */
	wavelan_close(device *),	/* Close the device */
	wavelan_init(device *);		/* Do nothing */
static dev_link_t *
	wavelan_attach(void);		/* Create a new device */
static void
	wavelan_detach(dev_link_t *);	/* Destroy a removed device */
static int
	wavelan_event(event_t,		/* Manage pcmcia events */
		      int,
		      event_callback_args_t *);

/**************************** VARIABLES ****************************/

static dev_info_t dev_info = "wavelan_cs";
static dev_link_t *dev_list = NULL;	/* Linked list of devices */

/* WARNING : the following variable MUST be volatile
 * It is used by wv_82593_cmd to syncronise with wavelan_interrupt */ 
static volatile int	wv_wait_completed = 0;

/*
 * Parameters that can be set with 'insmod'
 * The exact syntax is 'insmod wavelan_cs.o <var>=<value>'
 */

/* Bit map of interrupts to choose from */
/* This means pick from 15, 14, 12, 11, 10, 9, 7, 5, 4 and 3 */
static int	irq_mask = 0xdeb8;
static int 	irq_list[4] = { -1 };

/* Shared memory speed, in ns */
static int	mem_speed = 0;

/* New module interface */
MODULE_PARM(irq_mask, "i");
MODULE_PARM(irq_list, "1-4i");
MODULE_PARM(mem_speed, "i");

#ifdef WAVELAN_ROAMING		/* Conditional compile, see above in options */
/* Enable roaming mode ? No ! Please keep this to 0 */
static int	do_roaming = 0;
MODULE_PARM(do_roaming, "i");
#endif	/* WAVELAN_ROAMING */

#endif	/* WAVELAN_CS_H */

