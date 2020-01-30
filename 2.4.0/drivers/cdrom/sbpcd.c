

/*
 *  sbpcd.c   CD-ROM device driver for the whole family of traditional,
 *            non-ATAPI IDE-style Matsushita/Panasonic CR-5xx drives.
 *            Works with SoundBlaster compatible cards and with "no-sound"
 *            interface cards like Lasermate, Panasonic CI-101P, Teac, ...
 *            Also for the Longshine LCS-7260 drive.
 *            Also for the IBM "External ISA CD-Rom" drive.
 *            Also for the CreativeLabs CD200 drive.
 *            Also for the TEAC CD-55A drive.
 *            Also for the ECS-AT "Vertos 100" drive.
 *            Not for Sanyo drives (but for the H94A, sjcd is there...).
 *            Not for any other Funai drives than the CD200 types (sometimes
 *             labelled E2550UA or MK4015 or 2800F).
 */

#define VERSION "v4.63 Andrew J. Kroll <ag784@freenet.buffalo.edu> Wed Jul 26 04:24:10 EDT 2000"

/*   Copyright (C) 1993, 1994, 1995  Eberhard Moenkeberg <emoenke@gwdg.de>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2, or (at your option)
 *   any later version.
 *
 *   You should have received a copy of the GNU General Public License
 *   (for example /usr/src/linux/COPYING); if not, write to the Free
 *   Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *   If you change this software, you should mail a .diff file with some
 *   description lines to emoenke@gwdg.de. I want to know about it.
 *
 *   If you are the editor of a Linux CD, you should enable sbpcd.c within
 *   your boot floppy kernel and send me one of your CDs for free.
 *
 *   If you would like to port the driver to an other operating system (f.e.
 *   FreeBSD or NetBSD) or use it as an information source, you shall not be
 *   restricted by the GPL under the following conditions:
 *     a) the source code of your work is freely available
 *     b) my part of the work gets mentioned at all places where your 
 *        authorship gets mentioned
 *     c) I receive a copy of your code together with a full installation
 *        package of your operating system for free.
 *
 *
 *  VERSION HISTORY
 *
 *  0.1  initial release, April/May 93, after mcd.c (Martin Harriss)
 *
 *  0.2  thek "repeat:"-loop in do_sbpcd_request did not check for
 *       end-of-request_queue (resulting in kernel panic).
 *       Flow control seems stable, but throughput is not better.  
 *
 *  0.3  interrupt locking totally eliminated (maybe "inb" and "outb"
 *       are still locking) - 0.2 made keyboard-type-ahead losses.
 *       check_sbpcd_media_change added (to use by isofs/inode.c)
 *       - but it detects almost nothing.
 *
 *  0.4  use MAJOR 25 definitely.
 *       Almost total re-design to support double-speed drives and
 *       "naked" (no sound) interface cards ("LaserMate" interface type).
 *       Flow control should be exact now.
 *       Don't occupy the SbPro IRQ line (not needed either); will
 *       live together with Hannu Savolainen's sndkit now.
 *       Speeded up data transfer to 150 kB/sec, with help from Kai
 *       Makisara, the "provider" of the "mt" tape utility.
 *       Give "SpinUp" command if necessary.
 *       First steps to support up to 4 drives (but currently only one).
 *       Implemented audio capabilities - workman should work, xcdplayer
 *       gives some problems.
 *       This version is still consuming too much CPU time, and
 *       sleeping still has to be worked on.
 *       During "long" implied seeks, it seems possible that a 
 *       ReadStatus command gets ignored. That gives the message
 *       "ResponseStatus timed out" (happens about 6 times here during
 *       a "ls -alR" of the YGGDRASIL LGX-Beta CD). Such a case is
 *       handled without data error, but it should get done better.
 *
 *  0.5  Free CPU during waits (again with help from Kai Makisara).
 *       Made it work together with the LILO/kernel setup standard.
 *       Included auto-probing code, as suggested by YGGDRASIL.
 *       Formal redesign to add DDI debugging.
 *       There are still flaws in IOCTL (workman with double speed drive).
 *
 *  1.0  Added support for all drive IDs (0...3, no longer only 0)
 *       and up to 4 drives on one controller.
 *       Added "#define MANY_SESSION" for "old" multi session CDs.
 *
 *  1.1  Do SpinUp for new drives, too.
 *       Revised for clean compile under "old" kernels (0.99pl9).
 *
 *  1.2  Found the "workman with double-speed drive" bug: use the driver's
 *       audio_state, not what the drive is reporting with ReadSubQ.
 *
 *  1.3  Minor cleanups.
 *       Refinements regarding Workman.
 *
 *  1.4  Read XA disks (PhotoCDs) with "old" drives, too (but only the first
 *       session - no chance to fully access a "multi-session" CD).
 *       This currently still is too slow (50 kB/sec) - but possibly
 *       the old drives won't do it faster.
 *       Implemented "door (un)lock" for new drives (still does not work
 *       as wanted - no lock possible after an unlock).
 *       Added some debugging printout for the UPC/EAN code - but my drives 
 *       return only zeroes. Is there no UPC/EAN code written?
 *
 *  1.5  Laborate with UPC/EAN code (not better yet).
 *       Adapt to kernel 1.1.8 change (have to explicitly include
 *       <linux/string.h> now).
 *
 *  1.6  Trying to read audio frames as data. Impossible with the current
 *       drive firmware levels, as it seems. Awaiting any hint. ;-)
 *       Changed "door unlock": repeat it until success.
 *       Changed CDROMSTOP routine (stop somewhat "softer" so that Workman
 *       won't get confused).
 *       Added a third interface type: Sequoia S-1000, as used with the SPEA
 *       Media FX sound card. This interface (usable for Sony and Mitsumi 
 *       drives, too) needs a special configuration setup and behaves like a 
 *       LaserMate type after that. Still experimental - I do not have such
 *       an interface.
 *       Use the "variable BLOCK_SIZE" feature (2048). But it does only work
 *       if you give the mount option "block=2048".
 *       The media_check routine is currently disabled; now that it gets
 *       called as it should I fear it must get synchronized for not to
 *       disturb the normal driver's activity.
 *
 *  2.0  Version number bumped - two reasons:
 *       - reading audio tracks as data works now with CR-562 and CR-563. We
 *       currently do it by an IOCTL (yet has to get standardized), one frame
 *       at a time; that is pretty slow. But it works.
 *       - we are maintaining now up to 4 interfaces (each up to 4 drives):
 *       did it the easy way - a different MAJOR (25, 26, ...) and a different
 *       copy of the driver (sbpcd.c, sbpcd2.c, sbpcd3.c, sbpcd4.c - only
 *       distinguished by the value of SBPCD_ISSUE and the driver's name),
 *       and a common sbpcd.h file.
 *       Bettered the "ReadCapacity error" problem with old CR-52x drives (the
 *       drives sometimes need a manual "eject/insert" before work): just
 *       reset the drive and do again. Needs lots of resets here and sometimes
 *       that does not cure, so this can't be the solution.
 *
 *  2.1  Found bug with multisession CDs (accessing frame 16).
 *       "read audio" works now with address type CDROM_MSF, too.
 *       Bigger audio frame buffer: allows reading max. 4 frames at time; this
 *       gives a significant speedup, but reading more than one frame at once
 *       gives missing chunks at each single frame boundary.
 *
 *  2.2  Kernel interface cleanups: timers, init, setup, media check.
 *
 *  2.3  Let "door lock" and "eject" live together.
 *       Implemented "close tray" (done automatically during open).
 *
 *  2.4  Use different names for device registering.
 *
 *  2.5  Added "#if EJECT" code (default: enabled) to automatically eject
 *       the tray during last call to "sbpcd_release".
 *       Added "#if JUKEBOX" code (default: disabled) to automatically eject
 *       the tray during call to "sbpcd_open" if no disk is in.
 *       Turn on the CD volume of "compatible" sound cards, too; just define
 *       SOUND_BASE (in sbpcd.h) accordingly (default: disabled).
 *
 *  2.6  Nothing new.  
 *
 *  2.7  Added CDROMEJECT_SW ioctl to set the "EJECT" behavior on the fly:
 *       0 disables, 1 enables auto-ejecting. Useful to keep the tray in
 *       during shutdown.
 *
 *  2.8  Added first support (still BETA, I need feedback or a drive) for
 *       the Longshine LCS-7260 drives. They appear as double-speed drives
 *       using the "old" command scheme, extended by tray control and door
 *       lock functions.
 *       Found (and fixed preliminary) a flaw with some multisession CDs: we
 *       have to re-direct not only the accesses to frame 16 (the isofs
 *       routines drive it up to max. 100), but also those to the continuation
 *       (repetition) frames (as far as they exist - currently set fix as
 *       16..20).
 *       Changed default of the "JUKEBOX" define. If you use this default,
 *       your tray will eject if you try to mount without a disk in. Next
 *       mount command will insert the tray - so, just fill in a disk. ;-)
 *
 *  2.9  Fulfilled the Longshine LCS-7260 support; with great help and
 *       experiments by Serge Robyns.
 *       First attempts to support the TEAC CD-55A drives; but still not
 *       usable yet.
 *       Implemented the CDROMMULTISESSION ioctl; this is an attempt to handle
 *       multi session CDs more "transparent" (redirection handling has to be
 *       done within the isofs routines, and only for the special purpose of
 *       obtaining the "right" volume descriptor; accesses to the raw device
 *       should not get redirected).
 *
 *  3.0  Just a "normal" increment, with some provisions to do it better. ;-)
 *       Introduced "#define READ_AUDIO" to specify the maximum number of 
 *       audio frames to grab with one request. This defines a buffer size
 *       within kernel space; a value of 0 will reserve no such space and
 *       disable the CDROMREADAUDIO ioctl. A value of 75 enables the reading
 *       of a whole second with one command, but will use a buffer of more
 *       than 172 kB.
 *       Started CD200 support. Drive detection should work, but nothing
 *       more.
 *
 *  3.1  Working to support the CD200 and the Teac CD-55A drives.
 *       AT-BUS style device numbering no longer used: use SCSI style now.
 *       So, the first "found" device has MINOR 0, regardless of the
 *       jumpered drive ID. This implies modifications to the /dev/sbpcd*
 *       entries for some people, but will help the DAU (german TLA, english:
 *       "newbie", maybe ;-) to install his "first" system from a CD.
 *
 *  3.2  Still testing with CD200 and CD-55A drives.
 *
 *  3.3  Working with CD200 support.
 *
 *  3.4  Auto-probing stops if an address of 0 is seen (to be entered with
 *       the kernel command line).
 *       Made the driver "loadable". If used as a module, "audio copy" is
 *       disabled, and the internal read ahead data buffer has a reduced size
 *       of 4 kB; so, throughput may be reduced a little bit with slow CPUs.
 *
 *  3.5  Provisions to handle weird photoCDs which have an interrupted
 *       "formatting" immediately after the last frames of some files: simply
 *       never "read ahead" with MultiSession CDs. By this, CPU usage may be
 *       increased with those CDs, and there may be a loss in speed.
 *       Re-structured the messaging system.
 *       The "loadable" version no longer has a limited READ_AUDIO buffer
 *       size.
 *       Removed "MANY_SESSION" handling for "old" multi session CDs.
 *       Added "private" IOCTLs CDROMRESET and CDROMVOLREAD.
 *       Started again to support the TEAC CD-55A drives, now that I found
 *       the money for "my own" drive. ;-)
 *       The TEAC CD-55A support is fairly working now.
 *       I have measured that the drive "delivers" at 600 kB/sec (even with
 *       bigger requests than the drive's 64 kB buffer can satisfy), but
 *       the "real" rate does not exceed 520 kB/sec at the moment. 
 *       Caused by the various changes to build in TEAC support, the timed
 *       loops are de-optimized at the moment (less throughput with CR-52x
 *       drives, and the TEAC will give speed only with SBP_BUFFER_FRAMES 64).
 *
 *  3.6  Fixed TEAC data read problems with SbPro interfaces.
 *       Initial size of the READ_AUDIO buffer is 0. Can get set to any size
 *       during runtime.
 *
 *  3.7  Introduced MAX_DRIVES for some poor interface cards (seen with TEAC
 *       drives) which allow only one drive (ID 0); this avoids repetitive
 *       detection under IDs 1..3. 
 *       Elongated cmd_out_T response waiting; necessary for photo CDs with
 *       a lot of sessions.
 *       Bettered the sbpcd_open() behavior with TEAC drives.
 *
 *  3.8  Elongated max_latency for CR-56x drives.
 *
 *  3.9  Finally fixed the long-known SoundScape/SPEA/Sequoia S-1000 interface
 *       configuration bug.
 *       Now Corey, Heiko, Ken, Leo, Vadim/Eric & Werner are invited to copy
 *       the config_spea() routine into their drivers. ;-)
 *
 *  4.0  No "big step" - normal version increment.
 *       Adapted the benefits from 1.3.33.
 *       Fiddled with CDROMREADAUDIO flaws.
 *       Avoid ReadCapacity command with CD200 drives (the MKE 1.01 version
 *       seems not to support it).
 *       Fulfilled "read audio" for CD200 drives, with help of Pete Heist
 *       (heistp@rpi.edu).
 *
 *  4.1  Use loglevel KERN_INFO with printk().
 *       Added support for "Vertos 100" drive ("ECS-AT") - it is very similar
 *       to the Longshine LCS-7260. Give feedback if you can - I never saw
 *       such a drive, and I have no specs.
 *
 *  4.2  Support for Teac 16-bit interface cards. Can't get auto-detected,
 *       so you have to jumper your card to 0x2C0. Still not 100% - come
 *       in contact if you can give qualified feedback.
 *       Use loglevel KERN_NOTICE with printk(). If you get annoyed by a
 *       flood of unwanted messages and the accompanied delay, try to read
 *       my documentation. Especially the Linux CDROM drivers have to do an
 *       important job for the newcomers, so the "distributed" version has
 *       to fit some special needs. Since generations, the flood of messages
 *       is user-configurable (even at runtime), but to get aware of this, one
 *       needs a special mental quality: the ability to read.
 *       
 *  4.3  CD200F does not like to receive a command while the drive is
 *       reading the ToC; still trying to solve it.
 *       Removed some redundant verify_area calls (yes, Heiko Eissfeldt
 *       is visiting all the Linux CDROM drivers ;-).
 *       
 *  4.4  Adapted one idea from tiensivu@pilot.msu.edu's "stripping-down"
 *       experiments: "KLOGD_PAUSE".
 *       Inhibited "play audio" attempts with data CDs. Provisions for a
 *       "data-safe" handling of "mixed" (data plus audio) Cds.
 *
 *  4.5  Meanwhile Gonzalo Tornaria <tornaria@cmat.edu.uy> (GTL) built a
 *       special end_request routine: we seem to have to take care for not
 *       to have two processes working at the request list. My understanding
 *       was and is that ll_rw_blk should not call do_sbpcd_request as long
 *       as there is still one call active (the first call will care for all
 *       outstanding I/Os, and if a second call happens, that is a bug in
 *       ll_rw_blk.c).
 *       "Check media change" without touching any drive.
 *
 *  4.6  Use a semaphore to synchronize multi-activity; elaborated by Rob
 *       Riggs <rriggs@tesser.com>. At the moment, we simply block "read"
 *       against "ioctl" and vice versa. This could be refined further, but
 *       I guess with almost no performance increase.
 *       Experiments to speed up the CD-55A; again with help of Rob Riggs
 *       (to be true, he gave both, idea & code. ;-)
 *
 *  4.61 Ported to Uniform CD-ROM driver by 
 *       Heiko Eissfeldt <heiko@colossus.escape.de> with additional
 *       changes by Erik Andersen <andersee@debian.org>
 *
 *  4.62 Fix a bug where playing audio left the drive in an unusable state.
 *         Heiko Eissfeldt <heiko@colossus.escape.de>
 *
 *  November 1999 -- Make kernel-parameter implementation work with 2.3.x 
 *	             Removed init_module & cleanup_module in favor of 
 *	             module_init & module_exit.
 *	             Torben Mathiasen <tmm@image.dk>
 *
 *  4.63 Bug fixes for audio annoyances, new legacy CDROM maintainer.
 *		Annoying things fixed:
 *		TOC reread on automated disk changes
 *		TOC reread on manual cd changes
 *		Play IOCTL tries to play CD before it's actually ready... sometimes.
 *		CD_AUDIO_COMPLETED state so workman (and other playes) can repeat play.
 *		Andrew J. Kroll <ag784@freenet.buffalo.edu> Wed Jul 26 04:24:10 EDT 2000
 *
 *
 *  TODO
 *     implement "read all subchannel data" (96 bytes per frame)
 *     remove alot of the virtual status bits and deal with hardware status
 *     move the change of cd for audio to a better place
 *     add debug levels to insmod parameters (trivial)
 *
 *     special thanks to Kai Makisara (kai.makisara@vtt.fi) for his fine
 *     elaborated speed-up experiments (and the fabulous results!), for
 *     the "push" towards load-free wait loops, and for the extensive mail
 *     thread which brought additional hints and bug fixes.
 *
 */

#ifndef SBPCD_ISSUE
#define SBPCD_ISSUE 1
#endif SBPCD_ISSUE

#include <linux/module.h>

#include <linux/version.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/timer.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/cdrom.h>
#include <linux/ioport.h>
#include <linux/devfs_fs_kernel.h>
#include <linux/major.h> 
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/init.h>
#include <linux/interrupt.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <stdarg.h>
#include <linux/config.h>
#include "sbpcd.h"

#if !(SBPCD_ISSUE-1)
#define MAJOR_NR MATSUSHITA_CDROM_MAJOR
#endif
#if !(SBPCD_ISSUE-2)
#define MAJOR_NR MATSUSHITA_CDROM2_MAJOR /* second driver issue */
#endif
#if !(SBPCD_ISSUE-3)
#define MAJOR_NR MATSUSHITA_CDROM3_MAJOR /* third driver issue */
#endif
#if !(SBPCD_ISSUE-4)
#define MAJOR_NR MATSUSHITA_CDROM4_MAJOR /* fourth driver issue */
#endif

#include <linux/blk.h>

/*==========================================================================*/
/*
 * provisions for more than 1 driver issues
 * currently up to 4 drivers, expandable
 */
#if !(SBPCD_ISSUE-1)
#define DO_SBPCD_REQUEST(a) do_sbpcd_request(a)
#define SBPCD_INIT(a) sbpcd_init(a)
#endif
#if !(SBPCD_ISSUE-2)
#define DO_SBPCD_REQUEST(a) do_sbpcd2_request(a)
#define SBPCD_INIT(a) sbpcd2_init(a)
#endif
#if !(SBPCD_ISSUE-3)
#define DO_SBPCD_REQUEST(a) do_sbpcd3_request(a)
#define SBPCD_INIT(a) sbpcd3_init(a)
#endif
#if !(SBPCD_ISSUE-4)
#define DO_SBPCD_REQUEST(a) do_sbpcd4_request(a)
#define SBPCD_INIT(a) sbpcd4_init(a)
#endif
/*==========================================================================*/
#if SBPCD_DIS_IRQ
#define SBPCD_CLI cli()
#define SBPCD_STI sti()
#else
#define SBPCD_CLI
#define SBPCD_STI
#endif SBPCD_DIS_IRQ
/*==========================================================================*/
/*
 * auto-probing address list
 * inspired by Adam J. Richter from Yggdrasil
 *
 * still not good enough - can cause a hang.
 *   example: a NE 2000 ethernet card at 300 will cause a hang probing 310.
 * if that happens, reboot and use the LILO (kernel) command line.
 * The possibly conflicting ethernet card addresses get NOT probed 
 * by default - to minimize the hang possibilities. 
 *
 * The SB Pro addresses get "mirrored" at 0x6xx and some more locations - to
 * avoid a type error, the 0x2xx-addresses must get checked before 0x6xx.
 *
 * send mail to emoenke@gwdg.de if your interface card is not FULLY
 * represented here.
 */
#if !(SBPCD_ISSUE-1)
static int sbpcd[] = 
{
	CDROM_PORT, SBPRO, /* probe with user's setup first */
#if DISTRIBUTION
	0x230, 1, /* Soundblaster Pro and 16 (default) */
#if 0
	0x300, 0, /* CI-101P (default), WDH-7001C (default),
		     Galaxy (default), Reveal (one default) */
	0x250, 1, /* OmniCD default, Soundblaster Pro and 16 */
	0x2C0, 3, /* Teac 16-bit cards */
	0x260, 1, /* OmniCD */
	0x320, 0, /* Lasermate, CI-101P, WDH-7001C, Galaxy, Reveal (other default),
		     Longshine LCS-6853 (default) */
	0x338, 0, /* Reveal Sound Wave 32 card model #SC600 */
	0x340, 0, /* Mozart sound card (default), Lasermate, CI-101P */
	0x360, 0, /* Lasermate, CI-101P */
	0x270, 1, /* Soundblaster 16 */
	0x670, 0, /* "sound card #9" */
	0x690, 0, /* "sound card #9" */
	0x338, 2, /* SPEA Media FX, Ensonic SoundScape (default) */
	0x328, 2, /* SPEA Media FX */
	0x348, 2, /* SPEA Media FX */
	0x634, 0, /* some newer sound cards */
	0x638, 0, /* some newer sound cards */
	0x230, 1, /* some newer sound cards */
	/* due to incomplete address decoding of the SbPro card, these must be last */
	0x630, 0, /* "sound card #9" (default) */
	0x650, 0, /* "sound card #9" */
#ifdef MODULE
	/*
	 * some "hazardous" locations (no harm with the loadable version)
	 * (will stop the bus if a NE2000 ethernet card resides at offset -0x10)
	 */
	0x330, 0, /* Lasermate, CI-101P, WDH-7001C */
	0x350, 0, /* Lasermate, CI-101P */
	0x358, 2, /* SPEA Media FX */
	0x370, 0, /* Lasermate, CI-101P */
	0x290, 1, /* Soundblaster 16 */
	0x310, 0, /* Lasermate, CI-101P, WDH-7001C */
#endif MODULE
#endif
#endif DISTRIBUTION
};
#else
static int sbpcd[] = {CDROM_PORT, SBPRO}; /* probe with user's setup only */
#endif
MODULE_PARM(sbpcd, "2i");

#define NUM_PROBE  (sizeof(sbpcd) / sizeof(int))

/*==========================================================================*/
/*
 * the external references:
 */
#if !(SBPCD_ISSUE-1)
#ifdef CONFIG_SBPCD2
extern int sbpcd2_init(void);
#endif
#ifdef CONFIG_SBPCD3
extern int sbpcd3_init(void);
#endif
#ifdef CONFIG_SBPCD4
extern int sbpcd4_init(void);
#endif
#endif

/*==========================================================================*/

#define INLINE inline

/*==========================================================================*/
/*
 * the forward references:
 */
static void sbp_sleep(u_int);
static void mark_timeout_delay(u_long);
static void mark_timeout_data(u_long);
#if 0
static void mark_timeout_audio(u_long);
#endif
static void sbp_read_cmd(struct request *req);
static int sbp_data(struct request *req);
static int cmd_out(void);
static int DiskInfo(void);
static int sbpcd_chk_disk_change(kdev_t);

/*==========================================================================*/

/*
 * pattern for printk selection:
 *
 * (1<<DBG_INF)  necessary information
 * (1<<DBG_BSZ)  BLOCK_SIZE trace
 * (1<<DBG_REA)  "read" status trace
 * (1<<DBG_CHK)  "media check" trace
 * (1<<DBG_TIM)  datarate timer test
 * (1<<DBG_INI)  initialization trace
 * (1<<DBG_TOC)  tell TocEntry values
 * (1<<DBG_IOC)  ioctl trace
 * (1<<DBG_STA)  "ResponseStatus" trace
 * (1<<DBG_ERR)  "cc_ReadError" trace
 * (1<<DBG_CMD)  "cmd_out" trace
 * (1<<DBG_WRN)  give explanation before auto-probing
 * (1<<DBG_MUL)  multi session code test
 * (1<<DBG_IDX)  "drive_id != 0" test code
 * (1<<DBG_IOX)  some special information
 * (1<<DBG_DID)  drive ID test
 * (1<<DBG_RES)  drive reset info
 * (1<<DBG_SPI)  SpinUp test info
 * (1<<DBG_IOS)  ioctl trace: "subchannel"
 * (1<<DBG_IO2)  ioctl trace: general
 * (1<<DBG_UPC)  show UPC info
 * (1<<DBG_XA1)  XA mode debugging
 * (1<<DBG_LCK)  door (un)lock info
 * (1<<DBG_SQ1)   dump SubQ frame
 * (1<<DBG_AUD)  "read audio" debugging
 * (1<<DBG_SEQ)  Sequoia interface configuration trace
 * (1<<DBG_LCS)  Longshine LCS-7260 debugging trace
 * (1<<DBG_CD2)  MKE/Funai CD200 debugging trace
 * (1<<DBG_TEA)  TEAC CD-55A debugging trace
 * (1<<DBG_ECS)  ECS-AT (Vertos-100) debugging trace
 * (1<<DBG_000)  unnecessary information
 */
#if DISTRIBUTION
static int sbpcd_debug = (1<<DBG_INF);
#else
static int sbpcd_debug = 0 & ((1<<DBG_INF) |
			  (1<<DBG_TOC) |
			  (1<<DBG_MUL) |
			  (1<<DBG_UPC));
#endif DISTRIBUTION

static int sbpcd_ioaddr = CDROM_PORT;	/* default I/O base address */
static int sbpro_type = SBPRO;
static unsigned char setup_done;
static unsigned char f_16bit;
static unsigned char do_16bit;
static int CDo_command, CDo_reset;
static int CDo_sel_i_d, CDo_enable;
static int CDi_info, CDi_status, CDi_data;
static struct cdrom_msf msf;
static struct cdrom_ti ti;
static struct cdrom_tochdr tochdr;
static struct cdrom_tocentry tocentry;
static struct cdrom_subchnl SC;
static struct cdrom_volctrl volctrl;
static struct cdrom_read_audio read_audio;

static unsigned char msgnum;
static char msgbuf[80];

static const char *str_sb = "SoundBlaster";
static const char *str_sb_l = "soundblaster";
static const char *str_lm = "LaserMate";
static const char *str_sp = "SPEA";
static const char *str_sp_l = "spea";
static const char *str_ss = "SoundScape";
static const char *str_ss_l = "soundscape";
static const char *str_t16 = "Teac16bit";
static const char *str_t16_l = "teac16bit";
static const char *type;

#if !(SBPCD_ISSUE-1)
static const char *major_name="sbpcd";
#endif
#if !(SBPCD_ISSUE-2)
static const char *major_name="sbpcd2";
#endif
#if !(SBPCD_ISSUE-3)
static const char *major_name="sbpcd3";
#endif
#if !(SBPCD_ISSUE-4)
static const char *major_name="sbpcd4";
#endif

/*==========================================================================*/

#if FUTURE
static DECLARE_WAIT_QUEUE_HEAD(sbp_waitq);
#endif FUTURE

static int teac=SBP_TEAC_SPEED;
static int buffers=SBP_BUFFER_FRAMES;

static u_char family0[]="MATSHITA"; /* MKE CR-521, CR-522, CR-523 */
static u_char family1[]="CR-56";    /* MKE CR-562, CR-563 */
static u_char family2[]="CD200";    /* MKE CD200, Funai CD200F */
static u_char familyL[]="LCS-7260"; /* Longshine LCS-7260 */
static u_char familyT[]="CD-55";    /* TEAC CD-55A */
static u_char familyV[]="ECS-AT";   /* ECS Vertos 100 */

static u_int recursion; /* internal testing only */
static u_int fatal_err; /* internal testing only */
static u_int response_count;
static u_int flags_cmd_out;
static u_char cmd_type;
static u_char drvcmd[10];
static u_char infobuf[20];
static u_char xa_head_buf[CD_XA_HEAD];
static u_char xa_tail_buf[CD_XA_TAIL];

#if OLD_BUSY
static volatile u_char busy_data;
static volatile u_char busy_audio; /* true semaphores would be safer */
#endif OLD_BUSY
static DECLARE_MUTEX(ioctl_read_sem);
static u_long timeout;
static volatile u_char timed_out_delay;
static volatile u_char timed_out_data;
#if 0
static volatile u_char timed_out_audio;
#endif
static u_int datarate= 1000000;
static u_int maxtim16=16000000;
static u_int maxtim04= 4000000;
static u_int maxtim02= 2000000;
static u_int maxtim_8=   30000;
#if LONG_TIMING
static u_int maxtim_data= 9000;
#else
static u_int maxtim_data= 3000;
#endif LONG_TIMING
#if DISTRIBUTION
static int n_retries=6;
#else
static int n_retries=6;
#endif
/*==========================================================================*/

static int ndrives;
static u_char drv_pattern[NR_SBPCD]={speed_auto,speed_auto,speed_auto,speed_auto};
static int sbpcd_blocksizes[NR_SBPCD];

/*==========================================================================*/
/*
 * drive space begins here (needed separate for each unit) 
 */
static int d; /* DriveStruct index: drive number */

static struct {
	char drv_id;           /* "jumpered" drive ID or -1 */
	char drv_sel;          /* drive select lines bits */
	
	char drive_model[9];
	u_char firmware_version[4];
	char f_eject;          /* auto-eject flag: 0 or 1 */
	u_char *sbp_buf;       /* Pointer to internal data buffer,
				  space allocated during sbpcd_init() */
	u_int sbp_bufsiz;      /* size of sbp_buf (# of frames) */
	int sbp_first_frame;   /* First frame in buffer */
	int sbp_last_frame;    /* Last frame in buffer  */
	int sbp_read_frames;   /* Number of frames being read to buffer */
	int sbp_current;       /* Frame being currently read */
	
	u_char mode;           /* read_mode: READ_M1, READ_M2, READ_SC, READ_AU */
	u_char *aud_buf;       /* Pointer to audio data buffer,
				  space allocated during sbpcd_init() */
	u_int sbp_audsiz;      /* size of aud_buf (# of raw frames) */
	u_int drv_type;
	u_char drv_options;
	int status_bits;
	u_char diskstate_flags;
	u_char sense_byte;
	
	u_char CD_changed;
	char open_count;
	u_char error_byte;
	
	u_char f_multisession;
	u_int lba_multi;
	int first_session;
	int last_session;
	int track_of_last_session;
	
	u_char audio_state;
	u_int pos_audio_start;
	u_int pos_audio_end;
	char vol_chan0;
	u_char vol_ctrl0;
	char vol_chan1;
	u_char vol_ctrl1;
#if 000 /* no supported drive has it */
	char vol_chan2;
	u_char vol_ctrl2;
	char vol_chan3;
	u_char vol_ctrl3;
#endif 000
	u_char volume_control; /* TEAC on/off bits */
	
	u_char SubQ_ctl_adr;
	u_char SubQ_trk;
	u_char SubQ_pnt_idx;
	u_int SubQ_run_tot;
	u_int SubQ_run_trk;
	u_char SubQ_whatisthis;
	
	u_char UPC_ctl_adr;
	u_char UPC_buf[7];
	
	int frame_size;
	int CDsize_frm;
	
	u_char xa_byte; /* 0x20: XA capabilities */
	u_char n_first_track; /* binary */
	u_char n_last_track; /* binary (not bcd), 0x01...0x63 */
	u_int size_msf; /* time of whole CD, position of LeadOut track */
	u_int size_blk;
	
	u_char TocEnt_nixbyte; /* em */
	u_char TocEnt_ctl_adr;
	u_char TocEnt_number;
	u_char TocEnt_format; /* em */
	u_int TocEnt_address;
#if SAFE_MIXED
	char has_data;
#endif SAFE_MIXED
	u_char ored_ctl_adr; /* to detect if CDROM contains data tracks */
	
	struct {
		u_char nixbyte; /* em */
		u_char ctl_adr; /* 0x4x: data, 0x0x: audio */
		u_char number;
		u_char format; /* em */ /* 0x00: lba, 0x01: msf */
		u_int address;
	} TocBuffer[MAX_TRACKS+1]; /* last entry faked */ 
	
	int in_SpinUp; /* CR-52x test flag */
	int n_bytes; /* TEAC awaited response count */
	u_char error_state, b3, b4; /* TEAC command error state */
	u_char f_drv_error; /* TEAC command error flag */
	u_char speed_byte;
	int frmsiz;
	u_char f_XA; /* 1: XA */
	u_char type_byte; /* 0, 1, 3 */
	u_char mode_xb_6;
	u_char mode_yb_7;
	u_char mode_xb_8;
	u_char delay;
	struct cdrom_device_info *sbpcd_infop;

} D_S[NR_SBPCD];

/*
 * drive space ends here (needed separate for each unit)
 */
/*==========================================================================*/
#if 0
unsigned long cli_sti; /* for saving the processor flags */
#endif
/*==========================================================================*/
static struct timer_list delay_timer = { function: mark_timeout_delay};
static struct timer_list data_timer = { function: mark_timeout_data};
#if 0
static struct timer_list audio_timer = { function: mark_timeout_audio};
#endif
/*==========================================================================*/
/*
 * DDI interface
 */
static void msg(int level, const char *fmt, ...)
{
#if DISTRIBUTION
#define MSG_LEVEL KERN_NOTICE
#else
#define MSG_LEVEL KERN_INFO
#endif DISTRIBUTION

	char buf[256];
	va_list args;
	
	if (!(sbpcd_debug&(1<<level))) return;
	
	msgnum++;
	if (msgnum>99) msgnum=0;
	sprintf(buf, MSG_LEVEL "%s-%d [%02d]:  ", major_name, d, msgnum);
	va_start(args, fmt);
	vsprintf(&buf[18], fmt, args);
	va_end(args);
	printk(buf);
#if KLOGD_PAUSE
	sbp_sleep(KLOGD_PAUSE); /* else messages get lost */
#endif KLOGD_PAUSE
	return;
}
/*==========================================================================*/
/*
 * DDI interface: runtime trace bit pattern maintenance
 */
static int sbpcd_dbg_ioctl(unsigned long arg, int level)
{
	switch(arg)
	{
	case 0:	/* OFF */
		sbpcd_debug = DBG_INF;
		break;
		
	default:
		if (arg>=128) sbpcd_debug &= ~(1<<(arg-128));
		else sbpcd_debug |= (1<<arg);
	}
	return (arg);
}
/*==========================================================================*/
static void mark_timeout_delay(u_long i)
{
	timed_out_delay=1;
#if 0
	msg(DBG_TIM,"delay timer expired.\n");
#endif
}
/*==========================================================================*/
static void mark_timeout_data(u_long i)
{
	timed_out_data=1;
#if 0
	msg(DBG_TIM,"data timer expired.\n");
#endif
}
/*==========================================================================*/
#if 0
static void mark_timeout_audio(u_long i)
{
	timed_out_audio=1;
#if 0
	msg(DBG_TIM,"audio timer expired.\n");
#endif
}
#endif
/*==========================================================================*/
/*
 * Wait a little while (used for polling the drive).
 */
static void sbp_sleep(u_int time)
{
	sti();
	current->state = TASK_INTERRUPTIBLE;
	schedule_timeout(time);
	sti();
}
/*==========================================================================*/
#define RETURN_UP(rc) {up(&ioctl_read_sem); return(rc);}
/*==========================================================================*/
/*
 *  convert logical_block_address to m-s-f_number (3 bytes only)
 */
static INLINE void lba2msf(int lba, u_char *msf)
{
	lba += CD_MSF_OFFSET;
	msf[0] = lba / (CD_SECS*CD_FRAMES);
	lba %= CD_SECS*CD_FRAMES;
	msf[1] = lba / CD_FRAMES;
	msf[2] = lba % CD_FRAMES;
}
/*==========================================================================*/
/*==========================================================================*/
/*
 *  convert msf-bin to msf-bcd
 */
static INLINE void bin2bcdx(u_char *p)  /* must work only up to 75 or 99 */
{
	*p=((*p/10)<<4)|(*p%10);
}
/*==========================================================================*/
static INLINE u_int blk2msf(u_int blk)
{
	MSF msf;
	u_int mm;
	
	msf.c[3] = 0;
	msf.c[2] = (blk + CD_MSF_OFFSET) / (CD_SECS * CD_FRAMES);
	mm = (blk + CD_MSF_OFFSET) % (CD_SECS * CD_FRAMES);
	msf.c[1] = mm / CD_FRAMES;
	msf.c[0] = mm % CD_FRAMES;
	return (msf.n);
}
/*==========================================================================*/
static INLINE u_int make16(u_char rh, u_char rl)
{
	return ((rh<<8)|rl);
}
/*==========================================================================*/
static INLINE u_int make32(u_int rh, u_int rl)
{
	return ((rh<<16)|rl);
}
/*==========================================================================*/
static INLINE u_char swap_nibbles(u_char i)
{
	return ((i<<4)|(i>>4));
}
/*==========================================================================*/
static INLINE u_char byt2bcd(u_char i)
{
	return (((i/10)<<4)+i%10);
}
/*==========================================================================*/
static INLINE u_char bcd2bin(u_char bcd)
{
	return ((bcd>>4)*10+(bcd&0x0F));
}
/*==========================================================================*/
static INLINE int msf2blk(int msfx)
{
	MSF msf;
	int i;
	
	msf.n=msfx;
	i=(msf.c[2] * CD_SECS + msf.c[1]) * CD_FRAMES + msf.c[0] - CD_MSF_OFFSET;
	if (i<0) return (0);
	return (i);
}
/*==========================================================================*/
/*
 *  convert m-s-f_number (3 bytes only) to logical_block_address 
 */
static INLINE int msf2lba(u_char *msf)
{
	int i;
	
	i=(msf[0] * CD_SECS + msf[1]) * CD_FRAMES + msf[2] - CD_MSF_OFFSET;
	if (i<0) return (0);
	return (i);
}
/*==========================================================================*/
/* evaluate cc_ReadError code */ 
static int sta2err(int sta)
{
	if (famT_drive)
	{
		if (sta==0x00) return (0);
		if (sta==0x01) return (-604); /* CRC error */
		if (sta==0x02) return (-602); /* drive not ready */
		if (sta==0x03) return (-607); /* unknown media */
		if (sta==0x04) return (-612); /* general failure */
		if (sta==0x05) return (0);
		if (sta==0x06) return (-ERR_DISKCHANGE); /* disk change */
		if (sta==0x0b) return (-612); /* general failure */
		if (sta==0xff) return (-612); /* general failure */
		return (0);
	}
	else
	{
		if (sta<=2) return (sta);
		if (sta==0x05) return (-604); /* CRC error */
		if (sta==0x06) return (-606); /* seek error */
		if (sta==0x0d) return (-606); /* seek error */
		if (sta==0x0e) return (-603); /* unknown command */
		if (sta==0x14) return (-603); /* unknown command */
		if (sta==0x0c) return (-611); /* read fault */
		if (sta==0x0f) return (-611); /* read fault */
		if (sta==0x10) return (-611); /* read fault */
		if (sta>=0x16) return (-612); /* general failure */
		if (sta==0x11) return (-ERR_DISKCHANGE); /* disk change (LCS: removed) */
		if (famL_drive)
			if (sta==0x12) return (-ERR_DISKCHANGE); /* disk change (inserted) */
		return (-602); /* drive not ready */
	}
}
/*==========================================================================*/
static INLINE void clr_cmdbuf(void)
{
	int i;
	
	for (i=0;i<10;i++) drvcmd[i]=0;
	cmd_type=0;
}
/*==========================================================================*/
static void flush_status(void)
{
	int i;
	
	sbp_sleep(15*HZ/10);
	for (i=maxtim_data;i!=0;i--) inb(CDi_status);
}
/*====================================================================*/
/*
 * CDi status loop for Teac CD-55A (Rob Riggs)
 *
 * This is needed because for some strange reason
 * the CD-55A can take a real long time to give a
 * status response. This seems to happen after we
 * issue a READ command where a long seek is involved.
 *
 * I tried to ensure that we get max throughput with
 * minimal busy waiting. We busy wait at first, then
 * "switch gears" and start sleeping. We sleep for
 * longer periods of time the longer we wait.
 *
 */
static int CDi_stat_loop_T(void)
{
	int	i, gear=1;
	u_long  timeout_1, timeout_2, timeout_3, timeout_4;

	timeout_1 = jiffies + HZ / 50;  /* sbp_sleep(0) for a short period */
	timeout_2 = jiffies + HZ / 5;	/* nap for no more than 200ms */
	timeout_3 = jiffies + 5 * HZ;	/* sleep for up to 5s */
	timeout_4 = jiffies + 45 * HZ;	/* long sleep for up to 45s. */
	do
          {
            i = inb(CDi_status);
            if (!(i&s_not_data_ready)) return (i);
            if (!(i&s_not_result_ready)) return (i);
            switch(gear)
              {
              case 4:
                sbp_sleep(HZ);
                if (time_after(jiffies, timeout_4)) gear++;
                msg(DBG_TEA, "CDi_stat_loop_T: long sleep active.\n");
                break;
              case 3:
                sbp_sleep(HZ/10);
                if (time_after(jiffies, timeout_3)) gear++;
                break;
              case 2:
                sbp_sleep(HZ/100);
                if (time_after(jiffies, timeout_2)) gear++;
                break;
              case 1:
                sbp_sleep(0);
                if (time_after(jiffies, timeout_1)) gear++;
              }
          } while (gear < 5);
	return -1;
}
/*==========================================================================*/
static int CDi_stat_loop(void)
{
	int i,j;
	
	for(timeout = jiffies + 10*HZ, i=maxtim_data; time_before(jiffies, timeout); )
	{
		for ( ;i!=0;i--)
		{
			j=inb(CDi_status);
			if (!(j&s_not_data_ready)) return (j);
			if (!(j&s_not_result_ready)) return (j);
			if (fam0L_drive) if (j&s_attention) return (j);
		}
		sbp_sleep(1);
		i = 1;
	}
	msg(DBG_LCS,"CDi_stat_loop failed in line %d\n", __LINE__);
	return (-1);
}
/*==========================================================================*/
#if 00000
/*==========================================================================*/
static int tst_DataReady(void)
{
	int i;
	
	i=inb(CDi_status);
	if (i&s_not_data_ready) return (0);
	return (1);
}
/*==========================================================================*/
static int tst_ResultReady(void)
{
	int i;
	
	i=inb(CDi_status);
	if (i&s_not_result_ready) return (0);
	return (1);
}
/*==========================================================================*/
static int tst_Attention(void)
{
	int i;
	
	i=inb(CDi_status);
	if (i&s_attention) return (1);
	return (0);
}
/*==========================================================================*/
#endif 00000
/*==========================================================================*/
static int ResponseInfo(void)
{
	int i,j,st=0;
	u_long timeout;
	
	for (i=0,timeout=jiffies+HZ;i<response_count;i++) 
	{
		for (j=maxtim_data; ; )
		{
			for ( ;j!=0;j-- )
			{
				st=inb(CDi_status);
				if (!(st&s_not_result_ready)) break;
			}
			if ((j!=0)||time_after_eq(jiffies, timeout)) break;
			sbp_sleep(1);
			j = 1;
		}
		if (time_after_eq(jiffies, timeout)) break;
		infobuf[i]=inb(CDi_info);
	}
#if 000
	while (!(inb(CDi_status)&s_not_result_ready))
	{
		infobuf[i++]=inb(CDi_info);
	}
	j=i-response_count;
	if (j>0) msg(DBG_INF,"ResponseInfo: got %d trailing bytes.\n",j);
#endif 000
	for (j=0;j<i;j++)
		sprintf(&msgbuf[j*3]," %02X",infobuf[j]);
	msgbuf[j*3]=0;
	msg(DBG_CMD,"ResponseInfo:%s (%d,%d)\n",msgbuf,response_count,i);
	j=response_count-i;
	if (j>0) return (-j);
	else return (i);
}
/*==========================================================================*/
static void EvaluateStatus(int st)
{
	D_S[d].status_bits=0;
	if (fam1_drive) D_S[d].status_bits=st|p_success;
	else if (fam0_drive)
	{
		if (st&p_caddin_old) D_S[d].status_bits |= p_door_closed|p_caddy_in;
		if (st&p_spinning) D_S[d].status_bits |= p_spinning;
		if (st&p_check) D_S[d].status_bits |= p_check;
 		if (st&p_success_old) D_S[d].status_bits |= p_success;
 		if (st&p_busy_old) D_S[d].status_bits |= p_busy_new;
		if (st&p_disk_ok) D_S[d].status_bits |= p_disk_ok;
	}
	else if (famLV_drive)
	{
 		D_S[d].status_bits |= p_success;
		if (st&p_caddin_old) D_S[d].status_bits |= p_disk_ok|p_caddy_in;
		if (st&p_spinning) D_S[d].status_bits |= p_spinning;
		if (st&p_check) D_S[d].status_bits |= p_check;
		if (st&p_busy_old) D_S[d].status_bits |= p_busy_new;
		if (st&p_lcs_door_closed) D_S[d].status_bits |= p_door_closed;
		if (st&p_lcs_door_locked) D_S[d].status_bits |= p_door_locked;
	}
	else if (fam2_drive)
	{
 		D_S[d].status_bits |= p_success;
		if (st&p2_check) D_S[d].status_bits |= p1_check;
		if (st&p2_door_closed) D_S[d].status_bits |= p1_door_closed;
		if (st&p2_disk_in) D_S[d].status_bits |= p1_disk_in;
		if (st&p2_busy1) D_S[d].status_bits |= p1_busy;
		if (st&p2_busy2) D_S[d].status_bits |= p1_busy;
		if (st&p2_spinning) D_S[d].status_bits |= p1_spinning;
		if (st&p2_door_locked) D_S[d].status_bits |= p1_door_locked;
		if (st&p2_disk_ok) D_S[d].status_bits |= p1_disk_ok;
	}
	else if (famT_drive)
	{
		return; /* still needs to get coded */
 		D_S[d].status_bits |= p_success;
		if (st&p2_check) D_S[d].status_bits |= p1_check;
		if (st&p2_door_closed) D_S[d].status_bits |= p1_door_closed;
		if (st&p2_disk_in) D_S[d].status_bits |= p1_disk_in;
		if (st&p2_busy1) D_S[d].status_bits |= p1_busy;
		if (st&p2_busy2) D_S[d].status_bits |= p1_busy;
		if (st&p2_spinning) D_S[d].status_bits |= p1_spinning;
		if (st&p2_door_locked) D_S[d].status_bits |= p1_door_locked;
		if (st&p2_disk_ok) D_S[d].status_bits |= p1_disk_ok;
	}
	return;
}
/*==========================================================================*/
static int get_state_T(void)
{
	int i;
	
	static int cmd_out_T(void);

	clr_cmdbuf();
	D_S[d].n_bytes=1;
	drvcmd[0]=CMDT_STATUS;
	i=cmd_out_T();
	if (i>=0) i=infobuf[0];
	else
	{
		msg(DBG_TEA,"get_state_T error %d\n", i);
		return (i);
	}
	if (i>=0)
		/* 2: closed, disk in */
		D_S[d].status_bits=p1_door_closed|p1_disk_in|p1_spinning|p1_disk_ok;
	else if (D_S[d].error_state==6)
	{
		/* 3: closed, disk in, changed ("06 xx xx") */
		D_S[d].status_bits=p1_door_closed|p1_disk_in;
		D_S[d].CD_changed=0xFF;
		D_S[d].diskstate_flags &= ~toc_bit;
	}
	else if ((D_S[d].error_state!=2)||(D_S[d].b3!=0x3A)||(D_S[d].b4==0x00))
	{
		/* 1: closed, no disk ("xx yy zz"or "02 3A 00") */
		D_S[d].status_bits=p1_door_closed;
		D_S[d].open_count=0;
	}
	else if (D_S[d].b4==0x01)
	{
		/* 0: open ("02 3A 01") */
		D_S[d].status_bits=0;
		D_S[d].open_count=0;
	}
	else
	{
		/* 1: closed, no disk ("02 3A xx") */
		D_S[d].status_bits=p1_door_closed;
		D_S[d].open_count=0;
	}
	return (D_S[d].status_bits);
}
/*==========================================================================*/
static int ResponseStatus(void)
{
	int i,j;
	u_long timeout;
	
	msg(DBG_STA,"doing ResponseStatus...\n");
	if (famT_drive) return (get_state_T());
	if (flags_cmd_out & f_respo3) timeout = jiffies;
	else if (flags_cmd_out & f_respo2) timeout = jiffies + 16*HZ;
	else timeout = jiffies + 4*HZ;
	j=maxtim_8;
	do
	{
		for ( ;j!=0;j--)
		{ 
			i=inb(CDi_status);
			if (!(i&s_not_result_ready)) break;
		}
		if ((j!=0)||time_after(jiffies, timeout)) break;
		sbp_sleep(1);
		j = 1;
	}
	while (1);
	if (j==0) 
	{
		if ((flags_cmd_out & f_respo3) == 0)
			msg(DBG_STA,"ResponseStatus: timeout.\n");
		D_S[d].status_bits=0;
		return (-401);
	}
	i=inb(CDi_info);
	msg(DBG_STA,"ResponseStatus: response %02X.\n", i);
	EvaluateStatus(i);
	msg(DBG_STA,"status_bits=%02X, i=%02X\n",D_S[d].status_bits,i);
	return (D_S[d].status_bits);
}
/*==========================================================================*/
static void cc_ReadStatus(void)
{
	int i;
	
	msg(DBG_STA,"giving cc_ReadStatus command\n");
	if (famT_drive) return;
	SBPCD_CLI;
	if (fam0LV_drive) OUT(CDo_command,CMD0_STATUS);
	else if (fam1_drive) OUT(CDo_command,CMD1_STATUS);
	else if (fam2_drive) OUT(CDo_command,CMD2_STATUS);
	if (!fam0LV_drive) for (i=0;i<6;i++) OUT(CDo_command,0);
	SBPCD_STI;
}
/*==========================================================================*/
static int cc_ReadError(void)
{
	int i;

	clr_cmdbuf();
	msg(DBG_ERR,"giving cc_ReadError command.\n");
	if (fam1_drive)
	{
		drvcmd[0]=CMD1_READ_ERR;
		response_count=8;
		flags_cmd_out=f_putcmd|f_ResponseStatus;
	}
	else if (fam0LV_drive)
	{
		drvcmd[0]=CMD0_READ_ERR;
		response_count=6;
		if (famLV_drive)
			flags_cmd_out=f_putcmd;
		else
			flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus;
	}
	else if (fam2_drive)
	{
		drvcmd[0]=CMD2_READ_ERR;
		response_count=6;
		flags_cmd_out=f_putcmd;
	}
	else if (famT_drive)
	{
		response_count=5;
		drvcmd[0]=CMDT_READ_ERR;
	}
	i=cmd_out();
	D_S[d].error_byte=0;
	msg(DBG_ERR,"cc_ReadError: cmd_out(CMDx_READ_ERR) returns %d (%02X)\n",i,i);
	if (i<0) return (i);
	if (fam0V_drive) i=1;
	else i=2;
	D_S[d].error_byte=infobuf[i];
	msg(DBG_ERR,"cc_ReadError: infobuf[%d] is %d (%02X)\n",i,D_S[d].error_byte,D_S[d].error_byte);
	i=sta2err(infobuf[i]);
        if (i==-ERR_DISKCHANGE)
        {
                D_S[d].CD_changed=0xFF;
                D_S[d].diskstate_flags &= ~toc_bit;
        }
	return (i);
}
/*==========================================================================*/
static int cmd_out_T(void)
{
#undef CMDT_TRIES
#define CMDT_TRIES 1000
#define TEST_FALSE_FF 1
	
	static int cc_DriveReset(void);
	int i, j, l=0, m, ntries;
	long flags;

	D_S[d].error_state=0;
	D_S[d].b3=0;
	D_S[d].b4=0;
	D_S[d].f_drv_error=0;
	for (i=0;i<10;i++) sprintf(&msgbuf[i*3]," %02X",drvcmd[i]);
	msgbuf[i*3]=0;
	msg(DBG_CMD,"cmd_out_T:%s\n",msgbuf);

	OUT(CDo_sel_i_d,0);
	OUT(CDo_enable,D_S[d].drv_sel);
	i=inb(CDi_status);
	do_16bit=0;
	if ((f_16bit)&&(!(i&0x80)))
	{
		do_16bit=1;
		msg(DBG_TEA,"cmd_out_T: do_16bit set.\n");
	}
	if (!(i&s_not_result_ready))
	do
	{
		j=inb(CDi_info);
		i=inb(CDi_status);
		sbp_sleep(0);
		msg(DBG_TEA,"cmd_out_T: spurious !s_not_result_ready. (%02X)\n", j);
	}
	while (!(i&s_not_result_ready));
	save_flags(flags); cli();
	for (i=0;i<10;i++) OUT(CDo_command,drvcmd[i]);
	restore_flags(flags);
	for (ntries=CMDT_TRIES;ntries>0;ntries--)
	{
		if (drvcmd[0]==CMDT_READ_VER) sbp_sleep(HZ); /* fixme */
#if 01
		OUT(CDo_sel_i_d,1);
#endif 01
		if (teac==2)
                  {
                    if ((i=CDi_stat_loop_T()) == -1) break;
                  }
		else
                  {
#if 0
                    OUT(CDo_sel_i_d,1);
#endif 0
                    i=inb(CDi_status);
                  }
		if (!(i&s_not_data_ready)) /* f.e. CMDT_DISKINFO */
		{
			OUT(CDo_sel_i_d,1);
			if (drvcmd[0]==CMDT_READ) return (0); /* handled elsewhere */
			if (drvcmd[0]==CMDT_DISKINFO)
			{
				l=0;
				do
                                {
                                        if (do_16bit)
                                        {
                                                i=inw(CDi_data);
                                                infobuf[l++]=i&0x0ff;
                                                infobuf[l++]=i>>8;
#if TEST_FALSE_FF
                                                if ((l==2)&&(infobuf[0]==0x0ff))
                                                {
                                                        infobuf[0]=infobuf[1];
                                                        l=1;
                                                        msg(DBG_TEA,"cmd_out_T: do_16bit: false first byte!\n");
                                                }
#endif TEST_FALSE_FF
                                        }
                                        else infobuf[l++]=inb(CDi_data);
                                        i=inb(CDi_status);
                                }
				while (!(i&s_not_data_ready));
				for (j=0;j<l;j++) sprintf(&msgbuf[j*3]," %02X",infobuf[j]);
				msgbuf[j*3]=0;
				msg(DBG_CMD,"cmd_out_T data response:%s\n", msgbuf);
			}
			else
			{
				msg(DBG_TEA,"cmd_out_T: data response with cmd_%02X!\n",
                                    drvcmd[0]);
				j=0;
				do
				{
                                        if (do_16bit) i=inw(CDi_data);
                                        else i=inb(CDi_data);
                                        j++;
                                        i=inb(CDi_status);
				}
				while (!(i&s_not_data_ready));
				msg(DBG_TEA,"cmd_out_T: data response: discarded %d bytes/words.\n", j);
				fatal_err++;
			}
		}
		i=inb(CDi_status);
		if (!(i&s_not_result_ready))
		{
			OUT(CDo_sel_i_d,0);
			if (drvcmd[0]==CMDT_DISKINFO) m=l;
			else m=0;
			do
			{
				infobuf[m++]=inb(CDi_info);
				i=inb(CDi_status);
			}
			while (!(i&s_not_result_ready));
			for (j=0;j<m;j++) sprintf(&msgbuf[j*3]," %02X",infobuf[j]);
			msgbuf[j*3]=0;
			msg(DBG_CMD,"cmd_out_T info response:%s\n", msgbuf);
			if (drvcmd[0]==CMDT_DISKINFO)
                        {
                                infobuf[0]=infobuf[l];
                                if (infobuf[0]!=0x02) return (l); /* data length */
                        }
			else if (infobuf[0]!=0x02) return (m); /* info length */
			do
			{
				++recursion;
				if (recursion>1) msg(DBG_TEA,"cmd_out_T READ_ERR recursion (%02X): %d !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n", drvcmd[0], recursion);
				clr_cmdbuf();
				drvcmd[0]=CMDT_READ_ERR;
				j=cmd_out_T(); /* !!! recursive here !!! */
				--recursion;
				sbp_sleep(1);
			}
			while (j<0);
			D_S[d].error_state=infobuf[2];
			D_S[d].b3=infobuf[3];
			D_S[d].b4=infobuf[4];
			if (D_S[d].f_drv_error)
			{
				D_S[d].f_drv_error=0;
				cc_DriveReset();
				D_S[d].error_state=2;
			}
			return (-D_S[d].error_state-400);
		}
		if (drvcmd[0]==CMDT_READ) return (0); /* handled elsewhere */
		if ((teac==0)||(ntries<(CMDT_TRIES-5))) sbp_sleep(HZ/10);
		else sbp_sleep(HZ/100);
		if (ntries>(CMDT_TRIES-50)) continue;
		msg(DBG_TEA,"cmd_out_T: next CMDT_TRIES (%02X): %d.\n", drvcmd[0], ntries-1);
	}
	D_S[d].f_drv_error=1;
	cc_DriveReset();
	D_S[d].error_state=2;
	return (-99);
}
/*==========================================================================*/
static int cmd_out(void)
{
	int i=0;
	
	if (famT_drive) return(cmd_out_T());
	
	if (flags_cmd_out&f_putcmd)
	{ 
		unsigned long flags;
		for (i=0;i<7;i++)
			sprintf(&msgbuf[i*3], " %02X", drvcmd[i]);
		msgbuf[i*3]=0;
		msg(DBG_CMD,"cmd_out:%s\n", msgbuf);
		save_flags(flags); cli();
		for (i=0;i<7;i++) OUT(CDo_command,drvcmd[i]);
		restore_flags(flags);
	}
	if (response_count!=0)
	{
		if (cmd_type!=0)
		{
			if (sbpro_type==1) OUT(CDo_sel_i_d,1);
			msg(DBG_INF,"misleaded to try ResponseData.\n");
			if (sbpro_type==1) OUT(CDo_sel_i_d,0);
			return (-22);
		}
		else i=ResponseInfo();
		if (i<0) return (i);
	}
	if (D_S[d].in_SpinUp) msg(DBG_SPI,"in_SpinUp: to CDi_stat_loop.\n");
	if (flags_cmd_out&f_lopsta)
	{
		i=CDi_stat_loop();
		if ((i<0)||!(i&s_attention)) return (-8);
	}
	if (!(flags_cmd_out&f_getsta)) goto LOC_229;
	
 LOC_228:
	if (D_S[d].in_SpinUp) msg(DBG_SPI,"in_SpinUp: to cc_ReadStatus.\n");
	cc_ReadStatus();
	
 LOC_229:
	if (flags_cmd_out&f_ResponseStatus) 
	{
		if (D_S[d].in_SpinUp) msg(DBG_SPI,"in_SpinUp: to ResponseStatus.\n");
		i=ResponseStatus();
		/* builds status_bits, returns orig. status or p_busy_new */
		if (i<0) return (i);
		if (flags_cmd_out&(f_bit1|f_wait_if_busy))
		{
			if (!st_check)
			{
				if ((flags_cmd_out&f_bit1)&&(i&p_success)) goto LOC_232;
				if ((!(flags_cmd_out&f_wait_if_busy))||(!st_busy)) goto LOC_228;
			}
		}
	}
 LOC_232:
	if (!(flags_cmd_out&f_obey_p_check)) return (0);
	if (!st_check) return (0);
	if (D_S[d].in_SpinUp) msg(DBG_SPI,"in_SpinUp: to cc_ReadError.\n");
	i=cc_ReadError();
	if (D_S[d].in_SpinUp) msg(DBG_SPI,"in_SpinUp: to cmd_out OK.\n");
	msg(DBG_000,"cmd_out: cc_ReadError=%d\n", i);
	return (i);
}
/*==========================================================================*/
static int cc_Seek(u_int pos, char f_blk_msf)
{
	int i;
	
  clr_cmdbuf();
	if (f_blk_msf>1) return (-3);
	if (fam0V_drive)
	{
		drvcmd[0]=CMD0_SEEK;
		if (f_blk_msf==1) pos=msf2blk(pos);
		drvcmd[2]=(pos>>16)&0x00FF;
		drvcmd[3]=(pos>>8)&0x00FF;
		drvcmd[4]=pos&0x00FF;
		if (fam0_drive)
		  flags_cmd_out = f_putcmd | f_respo2 | f_lopsta | f_getsta |
			f_ResponseStatus | f_obey_p_check | f_bit1;
		else
		  flags_cmd_out = f_putcmd;
	}
	else if (fam1L_drive)
	{
		drvcmd[0]=CMD1_SEEK; /* same as CMD1_ and CMDL_ */
		if (f_blk_msf==0) pos=blk2msf(pos);
		drvcmd[1]=(pos>>16)&0x00FF;
		drvcmd[2]=(pos>>8)&0x00FF;
		drvcmd[3]=pos&0x00FF;
		if (famL_drive)
			flags_cmd_out=f_putcmd|f_respo2|f_lopsta|f_getsta|f_ResponseStatus|f_obey_p_check|f_bit1;
		else
			flags_cmd_out=f_putcmd|f_respo2|f_ResponseStatus|f_obey_p_check;
	}
	else if (fam2_drive)
	{
		drvcmd[0]=CMD2_SEEK;
		if (f_blk_msf==0) pos=blk2msf(pos);
		drvcmd[2]=(pos>>24)&0x00FF;
		drvcmd[3]=(pos>>16)&0x00FF;
		drvcmd[4]=(pos>>8)&0x00FF;
		drvcmd[5]=pos&0x00FF;
		flags_cmd_out=f_putcmd|f_ResponseStatus;
	}
	else if (famT_drive)
	{
		drvcmd[0]=CMDT_SEEK;
		if (f_blk_msf==1) pos=msf2blk(pos);
		drvcmd[2]=(pos>>24)&0x00FF;
		drvcmd[3]=(pos>>16)&0x00FF;
		drvcmd[4]=(pos>>8)&0x00FF;
		drvcmd[5]=pos&0x00FF;
		D_S[d].n_bytes=1;
	}
	response_count=0;
	i=cmd_out();
	return (i);
}
/*==========================================================================*/
static int cc_SpinUp(void)
{
	int i;
	
	msg(DBG_SPI,"SpinUp.\n");
	D_S[d].in_SpinUp = 1;
	clr_cmdbuf();
	if (fam0LV_drive)
	{
		drvcmd[0]=CMD0_SPINUP;
		if (fam0L_drive)
		  flags_cmd_out=f_putcmd|f_respo2|f_lopsta|f_getsta|
		    f_ResponseStatus|f_obey_p_check|f_bit1;
		else
		  flags_cmd_out=f_putcmd;
	}
	else if (fam1_drive)
	{
		drvcmd[0]=CMD1_SPINUP;
		flags_cmd_out=f_putcmd|f_respo2|f_ResponseStatus|f_obey_p_check;
	}
	else if (fam2_drive)
	{
		drvcmd[0]=CMD2_TRAY_CTL;
		drvcmd[4]=0x01; /* "spinup" */
		flags_cmd_out=f_putcmd|f_respo2|f_ResponseStatus|f_obey_p_check;
	}
	else if (famT_drive)
	{
		drvcmd[0]=CMDT_TRAY_CTL;
		drvcmd[4]=0x03; /* "insert", it hopefully spins the drive up */
	}
	response_count=0;
	i=cmd_out();
	D_S[d].in_SpinUp = 0;
	return (i);
}
/*==========================================================================*/
static int cc_SpinDown(void)
{
	int i;
	
	if (fam0_drive) return (0);
	clr_cmdbuf();
	response_count=0;
	if (fam1_drive)
	{
		drvcmd[0]=CMD1_SPINDOWN;
		flags_cmd_out=f_putcmd|f_respo2|f_ResponseStatus|f_obey_p_check;
	}
	else if (fam2_drive)
	{
		drvcmd[0]=CMD2_TRAY_CTL;
		drvcmd[4]=0x02; /* "eject" */
		flags_cmd_out=f_putcmd|f_ResponseStatus;
	}
	else if (famL_drive)
	{
		drvcmd[0]=CMDL_SPINDOWN;
		drvcmd[1]=1;
		flags_cmd_out=f_putcmd|f_respo2|f_lopsta|f_getsta|f_ResponseStatus|f_obey_p_check|f_bit1;
	}
	else if (famV_drive)
	{
		drvcmd[0]=CMDV_SPINDOWN;
		flags_cmd_out=f_putcmd;
	}
	else if (famT_drive)
	{
		drvcmd[0]=CMDT_TRAY_CTL;
		drvcmd[4]=0x02; /* "eject" */
	}
	i=cmd_out();
	return (i);
}
/*==========================================================================*/
static int cc_get_mode_T(void)
{
	int i;
	
	clr_cmdbuf();
	response_count=10;
	drvcmd[0]=CMDT_GETMODE;
	drvcmd[4]=response_count;
	i=cmd_out_T();
	return (i);
}
/*==========================================================================*/
static int cc_set_mode_T(void)
{
	int i;
	
	clr_cmdbuf();
	response_count=1;
	drvcmd[0]=CMDT_SETMODE;
	drvcmd[1]=D_S[d].speed_byte;
	drvcmd[2]=D_S[d].frmsiz>>8;
	drvcmd[3]=D_S[d].frmsiz&0x0FF;
	drvcmd[4]=D_S[d].f_XA; /* 1: XA */
	drvcmd[5]=D_S[d].type_byte; /* 0, 1, 3 */
	drvcmd[6]=D_S[d].mode_xb_6;
	drvcmd[7]=D_S[d].mode_yb_7|D_S[d].volume_control;
	drvcmd[8]=D_S[d].mode_xb_8;
	drvcmd[9]=D_S[d].delay;
	i=cmd_out_T();
	return (i);
}
/*==========================================================================*/
static int cc_prep_mode_T(void)
{
	int i, j;
	
	i=cc_get_mode_T();
	if (i<0) return (i);
	for (i=0;i<10;i++)
		sprintf(&msgbuf[i*3], " %02X", infobuf[i]);
	msgbuf[i*3]=0;
	msg(DBG_TEA,"CMDT_GETMODE:%s\n", msgbuf);
	D_S[d].speed_byte=0x02; /* 0x02: auto quad, 0x82: quad, 0x81: double, 0x80: single */
	D_S[d].frmsiz=make16(infobuf[2],infobuf[3]);
	D_S[d].f_XA=infobuf[4];
	if (D_S[d].f_XA==0) D_S[d].type_byte=0;
	else D_S[d].type_byte=1;
	D_S[d].mode_xb_6=infobuf[6];
	D_S[d].mode_yb_7=1;
	D_S[d].mode_xb_8=infobuf[8];
	D_S[d].delay=0; /* 0, 1, 2, 3 */
	j=cc_set_mode_T();
	i=cc_get_mode_T();
	for (i=0;i<10;i++)
		sprintf(&msgbuf[i*3], " %02X", infobuf[i]);
	msgbuf[i*3]=0;
	msg(DBG_TEA,"CMDT_GETMODE:%s\n", msgbuf);
	return (j);
}
/*==========================================================================*/
static int cc_SetSpeed(u_char speed, u_char x1, u_char x2)
{
	int i;
	
	if (fam0LV_drive) return (0);
	clr_cmdbuf();
	response_count=0;
	if (fam1_drive)
	{
		drvcmd[0]=CMD1_SETMODE;
		drvcmd[1]=0x03;
		drvcmd[2]=speed;
		drvcmd[3]=x1;
		drvcmd[4]=x2;
		flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
	}
	else if (fam2_drive)
	{
		drvcmd[0]=CMD2_SETSPEED;
		if (speed&speed_auto)
		{
			drvcmd[2]=0xFF;
			drvcmd[3]=0xFF;
		}
		else
		{
			drvcmd[2]=0;
			drvcmd[3]=150;
		}
		flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
	}
	else if (famT_drive)
	{
		return (0);
	}
	i=cmd_out();
	return (i);
}
/*==========================================================================*/
static int cc_SetVolume(void)
{
	int i;
	u_char channel0,channel1,volume0,volume1;
	u_char control0,value0,control1,value1;
	
	D_S[d].diskstate_flags &= ~volume_bit;
	clr_cmdbuf();
	channel0=D_S[d].vol_chan0;
	volume0=D_S[d].vol_ctrl0;
	channel1=control1=D_S[d].vol_chan1;
	volume1=value1=D_S[d].vol_ctrl1;
	control0=value0=0;
	
	if (famV_drive) return (0);

	if (((D_S[d].drv_options&audio_mono)!=0)&&(D_S[d].drv_type>=drv_211))
	{
		if ((volume0!=0)&&(volume1==0))
		{
			volume1=volume0;
			channel1=channel0;
		}
		else if ((volume0==0)&&(volume1!=0))
		{
			volume0=volume1;
			channel0=channel1;
		}
	}
	if (channel0>1)
	{
		channel0=0;
		volume0=0;
	}
	if (channel1>1)
	{
		channel1=1;
		volume1=0;
	}
	
	if (fam1_drive)
	{
		control0=channel0+1;
		control1=channel1+1;
		value0=(volume0>volume1)?volume0:volume1;
		value1=value0;
		if (volume0==0) control0=0;
		if (volume1==0) control1=0;
		drvcmd[0]=CMD1_SETMODE;
		drvcmd[1]=0x05;
		drvcmd[3]=control0;
		drvcmd[4]=value0;
		drvcmd[5]=control1;
		drvcmd[6]=value1;
		flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
	}
	else if (fam2_drive)
	{
		control0=channel0+1;
		control1=channel1+1;
		value0=(volume0>volume1)?volume0:volume1;
		value1=value0;
		if (volume0==0) control0=0;
		if (volume1==0) control1=0;
		drvcmd[0]=CMD2_SETMODE;
		drvcmd[1]=0x0E;
		drvcmd[3]=control0;
		drvcmd[4]=value0;
		drvcmd[5]=control1;
		drvcmd[6]=value1;
		flags_cmd_out=f_putcmd|f_ResponseStatus;
	}
	else if (famL_drive)
	{
		if ((volume0==0)||(channel0!=0)) control0 |= 0x80;
		if ((volume1==0)||(channel1!=1)) control0 |= 0x40;
		if (volume0|volume1) value0=0x80;
		drvcmd[0]=CMDL_SETMODE;
		drvcmd[1]=0x03;
		drvcmd[4]=control0;
		drvcmd[5]=value0;
		flags_cmd_out=f_putcmd|f_lopsta|f_getsta|f_ResponseStatus|f_obey_p_check|f_bit1;
	}
	else if (fam0_drive) /* different firmware levels */
	{
		if (D_S[d].drv_type>=drv_300)
		{
			control0=volume0&0xFC;
			value0=volume1&0xFC;
			if ((volume0!=0)&&(volume0<4)) control0 |= 0x04;
			if ((volume1!=0)&&(volume1<4)) value0 |= 0x04;
			if (channel0!=0) control0 |= 0x01;
			if (channel1==1) value0 |= 0x01;
		}
		else
		{
			value0=(volume0>volume1)?volume0:volume1;
			if (D_S[d].drv_type<drv_211)
			{
				if (channel0!=0)
				{
					i=channel1;
					channel1=channel0;
					channel0=i;
					i=volume1;
					volume1=volume0;
					volume0=i;
				}
				if (channel0==channel1)
				{
					if (channel0==0)
					{
						channel1=1;
						volume1=0;
						volume0=value0;
					}
					else
					{
						channel0=0;
						volume0=0;
						volume1=value0;
					}
				}
			}
			
			if ((volume0!=0)&&(volume1!=0))
			{
				if (volume0==0xFF) volume1=0xFF;
				else if (volume1==0xFF) volume0=0xFF;
			}
			else if (D_S[d].drv_type<drv_201) volume0=volume1=value0;
			
			if (D_S[d].drv_type>=drv_201)
			{
				if (volume0==0) control0 |= 0x80;
				if (volume1==0) control0 |= 0x40;
			}
			if (D_S[d].drv_type>=drv_211)
			{
				if (channel0!=0) control0 |= 0x20;
				if (channel1!=1) control0 |= 0x10;
			}
		}
		drvcmd[0]=CMD0_SETMODE;
		drvcmd[1]=0x83;
		drvcmd[4]=control0;
		drvcmd[5]=value0;
		flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus|f_obey_p_check;
	}
	else if (famT_drive)
	{
		D_S[d].volume_control=0;
		if (!volume0) D_S[d].volume_control|=0x10;
		if (!volume1) D_S[d].volume_control|=0x20;
		i=cc_prep_mode_T();
		if (i<0) return (i);
	}
	if (!famT_drive)
	{
		response_count=0;
		i=cmd_out();
		if (i<0) return (i);
	}
	D_S[d].diskstate_flags |= volume_bit;
	return (0);
}
/*==========================================================================*/
static int GetStatus(void)
{
	int i;
	
	if (famT_drive) return (0);
	flags_cmd_out=f_getsta|f_ResponseStatus|f_obey_p_check;
	response_count=0;
	cmd_type=0;
	i=cmd_out();
	return (i);
}
/*==========================================================================*/
static int cc_DriveReset(void)
{
	int i;
	
	msg(DBG_RES,"cc_DriveReset called.\n");
	clr_cmdbuf();
	response_count=0;
	if (fam0LV_drive) OUT(CDo_reset,0x00);
	else if (fam1_drive)
	{
		drvcmd[0]=CMD1_RESET;
		flags_cmd_out=f_putcmd;
		i=cmd_out();
	}
	else if (fam2_drive)
	{
		drvcmd[0]=CMD2_RESET;
		flags_cmd_out=f_putcmd;
		i=cmd_out();
		OUT(CDo_reset,0x00);
	}
	else if (famT_drive)
	{
		OUT(CDo_sel_i_d,0);
		OUT(CDo_enable,D_S[d].drv_sel);
		OUT(CDo_command,CMDT_RESET);
		for (i=1;i<10;i++) OUT(CDo_command,0);
	}
	if (fam0LV_drive) sbp_sleep(5*HZ); /* wait 5 seconds */
	else sbp_sleep(1*HZ); /* wait a second */
#if 1
	if (famT_drive)
	{
		msg(DBG_TEA, "================CMDT_RESET given=================.\n");
		sbp_sleep(3*HZ);
	}
#endif 1
	flush_status();
	i=GetStatus();
	if (i<0) return i;
	if (!famT_drive)
		if (D_S[d].error_byte!=aud_12) return -501;
	return (0);
}

/*==========================================================================*/
static int SetSpeed(void)
{
	int i, speed;
	
	if (!(D_S[d].drv_options&(speed_auto|speed_300|speed_150))) return (0);
	speed=speed_auto;
	if (!(D_S[d].drv_options&speed_auto))
	{
		speed |= speed_300;
		if (!(D_S[d].drv_options&speed_300)) speed=0;
	}
	i=cc_SetSpeed(speed,0,0);
	return (i);
}

static void switch_drive(int i);

static int sbpcd_select_speed(struct cdrom_device_info *cdi, int speed)
{
  int i = MINOR(cdi->dev);

  if (i != d)
    switch_drive(i);

  return cc_SetSpeed(speed == 2 ? speed_300 : speed_150, 0, 0);
}

/*==========================================================================*/
static int DriveReset(void)
{
	int i;
	
	i=cc_DriveReset();
	if (i<0) return (-22);
	do
	{
		i=GetStatus();
		if ((i<0)&&(i!=-ERR_DISKCHANGE)) {
			return (-2); /* from sta2err */
		}
		if (!st_caddy_in) break;
		sbp_sleep(1);
	}
	while (!st_diskok);
#if 000
	D_S[d].CD_changed=1;
#endif
	if ((st_door_closed) && (st_caddy_in))
	{
		i=DiskInfo();
		if (i<0) return (-23);
	}
	return (0);
}

static int sbpcd_reset(struct cdrom_device_info *cdi)
{
  int i = MINOR(cdi->dev);

  if (i != d)
    switch_drive(i);

  return DriveReset();
}

/*==========================================================================*/
static int cc_PlayAudio(int pos_audio_start,int pos_audio_end)
{
	int i, j, n;
	
	if (D_S[d].audio_state==audio_playing) return (-EINVAL);
	clr_cmdbuf();
	response_count=0;
	if (famLV_drive)
	{
		drvcmd[0]=CMDL_PLAY;
		i=msf2blk(pos_audio_start);
		n=msf2blk(pos_audio_end)+1-i;
		drvcmd[1]=(i>>16)&0x00FF;
		drvcmd[2]=(i>>8)&0x00FF;
		drvcmd[3]=i&0x00FF;
		drvcmd[4]=(n>>16)&0x00FF;
		drvcmd[5]=(n>>8)&0x00FF;
		drvcmd[6]=n&0x00FF;
		if (famL_drive)
		flags_cmd_out = f_putcmd | f_respo2 | f_lopsta | f_getsta |
			f_ResponseStatus | f_obey_p_check | f_wait_if_busy;
		else
		  flags_cmd_out = f_putcmd;
	}
	else
	{
		j=1;
		if (fam1_drive)
		{
			drvcmd[0]=CMD1_PLAY_MSF;
			flags_cmd_out = f_putcmd | f_respo2 | f_ResponseStatus |
				f_obey_p_check | f_wait_if_busy;
		}
		else if (fam2_drive)
		{
			drvcmd[0]=CMD2_PLAY_MSF;
			flags_cmd_out = f_putcmd | f_ResponseStatus | f_obey_p_check;
		}
		else if (famT_drive)
		{
			drvcmd[0]=CMDT_PLAY_MSF;
			j=3;
			response_count=1;
		}
		else if (fam0_drive)
		{
			drvcmd[0]=CMD0_PLAY_MSF;
			flags_cmd_out = f_putcmd | f_respo2 | f_lopsta | f_getsta |
				f_ResponseStatus | f_obey_p_check | f_wait_if_busy;
		}
		drvcmd[j]=(pos_audio_start>>16)&0x00FF;
		drvcmd[j+1]=(pos_audio_start>>8)&0x00FF;
		drvcmd[j+2]=pos_audio_start&0x00FF;
		drvcmd[j+3]=(pos_audio_end>>16)&0x00FF;
		drvcmd[j+4]=(pos_audio_end>>8)&0x00FF;
		drvcmd[j+5]=pos_audio_end&0x00FF;
	}
	i=cmd_out();
	return (i);
}
/*==========================================================================*/
static int cc_Pause_Resume(int pau_res)
{
	int i;
	
	clr_cmdbuf();
	response_count=0;
	if (fam1_drive)
	{
		drvcmd[0]=CMD1_PAU_RES;
		if (pau_res!=1) drvcmd[1]=0x80;
		flags_cmd_out=f_putcmd|f_respo2|f_ResponseStatus|f_obey_p_check;
	}
	else if (fam2_drive)
	{
		drvcmd[0]=CMD2_PAU_RES;
		if (pau_res!=1) drvcmd[2]=0x01;
		flags_cmd_out=f_putcmd|f_ResponseStatus;
	}
	else if (fam0LV_drive)
	{
		drvcmd[0]=CMD0_PAU_RES;
		if (pau_res!=1) drvcmd[1]=0x80;
		if (famL_drive)
			flags_cmd_out=f_putcmd|f_respo2|f_lopsta|f_getsta|f_ResponseStatus|
				f_obey_p_check|f_bit1;
		else if (famV_drive)
		  flags_cmd_out=f_putcmd;
		else
			flags_cmd_out=f_putcmd|f_respo2|f_lopsta|f_getsta|f_ResponseStatus|
				f_obey_p_check;
	}
	else if (famT_drive)
	{
		if (pau_res==3)	return (cc_PlayAudio(D_S[d].pos_audio_start,D_S[d].pos_audio_end));
		else if (pau_res==1) drvcmd[0]=CMDT_PAUSE;
		else return (-56);
	}
	i=cmd_out();
	return (i);
}
/*==========================================================================*/
static int cc_LockDoor(char lock)
{
	int i;
	
	if (fam0_drive) return (0);
	msg(DBG_LCK,"cc_LockDoor: %d (drive %d)\n", lock, d);
	msg(DBG_LCS,"p_door_locked bit %d before\n", st_door_locked);
	clr_cmdbuf();
	response_count=0;
	if (fam1_drive)
	{
		drvcmd[0]=CMD1_LOCK_CTL;
		if (lock==1) drvcmd[1]=0x01;
		flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
	}
	else if (fam2_drive)
	{
		drvcmd[0]=CMD2_LOCK_CTL;
		if (lock==1) drvcmd[4]=0x01;
		flags_cmd_out=f_putcmd|f_ResponseStatus;
	}
	else if (famLV_drive)
	{
		drvcmd[0]=CMDL_LOCK_CTL;
		if (lock==1) drvcmd[1]=0x01;
		if (famL_drive)
		  flags_cmd_out=f_putcmd|f_respo2|f_lopsta|f_getsta|f_ResponseStatus|f_obey_p_check|f_bit1;
		else
		  flags_cmd_out=f_putcmd;
	}
	else if (famT_drive)
	{
		drvcmd[0]=CMDT_LOCK_CTL;
		if (lock==1) drvcmd[4]=0x01;
	}
	i=cmd_out();
	msg(DBG_LCS,"p_door_locked bit %d after\n", st_door_locked);
	return (i);
}
/*==========================================================================*/
/*==========================================================================*/
static int UnLockDoor(void)
{
	int i,j;
	
	j=20;
	do
	{
		i=cc_LockDoor(0);
		--j;
		sbp_sleep(1);
	}
	while ((i<0)&&(j));
	if (i<0)
	{
		cc_DriveReset();
		return -84;
	}
	return (0);
}
/*==========================================================================*/
static int LockDoor(void)
{
	int i,j;
	
	j=20;
	do
	{
		i=cc_LockDoor(1);
		--j;
		sbp_sleep(1);
	}
	while ((i<0)&&(j));
	if (j==0)
	{		
		cc_DriveReset();
		j=20;
		do
		{
			i=cc_LockDoor(1);
			--j;
			sbp_sleep(1);
		}
		while ((i<0)&&(j));
	}
	return (i);
}

static int sbpcd_lock_door(struct cdrom_device_info *cdi, int lock)
{
  return lock ? LockDoor() : UnLockDoor();
}

/*==========================================================================*/
static int cc_CloseTray(void)
{
	int i;
	
	if (fam0_drive) return (0);
	msg(DBG_LCK,"cc_CloseTray (drive %d)\n", d);
	msg(DBG_LCS,"p_door_closed bit %d before\n", st_door_closed);
	
	clr_cmdbuf();
	response_count=0;
	if (fam1_drive)
	{
		drvcmd[0]=CMD1_TRAY_CTL;
		flags_cmd_out=f_putcmd|f_respo2|f_ResponseStatus|f_obey_p_check;
	}
	else if (fam2_drive)
	{
		drvcmd[0]=CMD2_TRAY_CTL;
		drvcmd[1]=0x01;
		drvcmd[4]=0x03; /* "insert" */
		flags_cmd_out=f_putcmd|f_ResponseStatus;
	}
	else if (famLV_drive)
	{
		drvcmd[0]=CMDL_TRAY_CTL;
		if (famLV_drive)
		  flags_cmd_out=f_putcmd|f_respo2|f_lopsta|f_getsta|
			f_ResponseStatus|f_obey_p_check|f_bit1;
		else
		  flags_cmd_out=f_putcmd;
	}
	else if (famT_drive)
	{
		drvcmd[0]=CMDT_TRAY_CTL;
		drvcmd[4]=0x03; /* "insert" */
	}
	i=cmd_out();
	msg(DBG_LCS,"p_door_closed bit %d after\n", st_door_closed);

	i=cc_ReadError();
	flags_cmd_out |= f_respo2;
	cc_ReadStatus(); /* command: give 1-byte status */
	i=ResponseStatus();
	if (famT_drive&&(i<0))
	{
		cc_DriveReset();
		i=ResponseStatus();
#if 0
                sbp_sleep(HZ);
#endif 0
		i=ResponseStatus();
	}
	if (i<0)
	{
		msg(DBG_INF,"sbpcd cc_CloseTray: ResponseStatus timed out (%d).\n",i);
	}
	if (!(famT_drive))
	{
		if (!st_spinning)
		{
			cc_SpinUp();
			if (st_check) i=cc_ReadError();
			flags_cmd_out |= f_respo2;
			cc_ReadStatus();
			i=ResponseStatus();
		} else {
		}
	}
	i=DiskInfo();
	return (i);
}

static int sbpcd_tray_move(struct cdrom_device_info *cdi, int position)
{
	int i;
	int retval=0;
	i = MINOR(cdi->dev);
	switch_drive(i);
	/* DUH! --AJK */
	if(D_S[d].CD_changed != 0xFF) {
		D_S[d].CD_changed=0xFF;
		D_S[d].diskstate_flags &= ~cd_size_bit;
	}
	if (position == 1) {
		cc_SpinDown();
	} else {
		retval=cc_CloseTray();
	}
  return retval;
}

/*==========================================================================*/
static int cc_ReadSubQ(void)
{
	int i,j;

	D_S[d].diskstate_flags &= ~subq_bit;
	for (j=255;j>0;j--)
	{
		clr_cmdbuf();
		if (fam1_drive)
		{
			drvcmd[0]=CMD1_READSUBQ;
			flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
			response_count=11;
		}
		else if (fam2_drive)
		{
			drvcmd[0]=CMD2_READSUBQ;
			drvcmd[1]=0x02;
			drvcmd[3]=0x01;
			flags_cmd_out=f_putcmd;
			response_count=10;
		}
		else if (fam0LV_drive)
		{
			drvcmd[0]=CMD0_READSUBQ;
			drvcmd[1]=0x02;
			if (famLV_drive)
				flags_cmd_out=f_putcmd;
			else
				flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus|f_obey_p_check;
			response_count=13;
		}
		else if (famT_drive)
		{
			response_count=12;
			drvcmd[0]=CMDT_READSUBQ;
			drvcmd[1]=0x02;
			drvcmd[2]=0x40;
			drvcmd[3]=0x01;
			drvcmd[8]=response_count;
		}
		i=cmd_out();
		if (i<0) return (i);
		for (i=0;i<response_count;i++)
		{
			sprintf(&msgbuf[i*3], " %02X", infobuf[i]);
			msgbuf[i*3]=0;
			msg(DBG_SQ1,"cc_ReadSubQ:%s\n", msgbuf);
		}
		if (famT_drive) break;
		if (infobuf[0]!=0) break;
		if ((!st_spinning) || (j==1))
		{
			D_S[d].SubQ_ctl_adr=D_S[d].SubQ_trk=D_S[d].SubQ_pnt_idx=D_S[d].SubQ_whatisthis=0;
			D_S[d].SubQ_run_tot=D_S[d].SubQ_run_trk=0;
			return (0);
		}
	}
	if (famT_drive) D_S[d].SubQ_ctl_adr=infobuf[1];
	else D_S[d].SubQ_ctl_adr=swap_nibbles(infobuf[1]);
	D_S[d].SubQ_trk=byt2bcd(infobuf[2]);
	D_S[d].SubQ_pnt_idx=byt2bcd(infobuf[3]);
	if (fam0LV_drive) i=5;
	else if (fam12_drive) i=4;
	else if (famT_drive) i=8;
	D_S[d].SubQ_run_tot=make32(make16(0,infobuf[i]),make16(infobuf[i+1],infobuf[i+2])); /* msf-bin */
	i=7;
	if (fam0LV_drive) i=9;
	else if (fam12_drive) i=7;
	else if (famT_drive) i=4;
	D_S[d].SubQ_run_trk=make32(make16(0,infobuf[i]),make16(infobuf[i+1],infobuf[i+2])); /* msf-bin */
	D_S[d].SubQ_whatisthis=infobuf[i+3];
	D_S[d].diskstate_flags |= subq_bit;
	return (0);
}
/*==========================================================================*/
static int cc_ModeSense(void)
{
	int i;
	
	if (fam2_drive) return (0);
	if (famV_drive) return (0);
	D_S[d].diskstate_flags &= ~frame_size_bit;
	clr_cmdbuf();
	if (fam1_drive)
	{
		response_count=5;
		drvcmd[0]=CMD1_GETMODE;
		flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
	}
	else if (fam0L_drive)
	{
		response_count=2;
		drvcmd[0]=CMD0_GETMODE;
		if (famL_drive) flags_cmd_out=f_putcmd;
		else flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus|f_obey_p_check;
	}
	else if (famT_drive)
	{
		response_count=10;
		drvcmd[0]=CMDT_GETMODE;
		drvcmd[4]=response_count;
	}
	i=cmd_out();
	if (i<0) return (i);
	i=0;
	D_S[d].sense_byte=0;
	if (fam1_drive) D_S[d].sense_byte=infobuf[i++];
	else if (famT_drive)
	{
		if (infobuf[4]==0x01) D_S[d].xa_byte=0x20;
		else D_S[d].xa_byte=0;
		i=2;
	}
	D_S[d].frame_size=make16(infobuf[i],infobuf[i+1]);
	for (i=0;i<response_count;i++)
		sprintf(&msgbuf[i*3], " %02X", infobuf[i]);
	msgbuf[i*3]=0;
	msg(DBG_XA1,"cc_ModeSense:%s\n", msgbuf);
	
	D_S[d].diskstate_flags |= frame_size_bit;
	return (0);
}
/*==========================================================================*/
/*==========================================================================*/
static int cc_ModeSelect(int framesize)
{
	int i;
	
	if (fam2_drive) return (0);
	if (famV_drive) return (0);
	D_S[d].diskstate_flags &= ~frame_size_bit;
	clr_cmdbuf();
	D_S[d].frame_size=framesize;
	if (framesize==CD_FRAMESIZE_RAW) D_S[d].sense_byte=0x82;
	else D_S[d].sense_byte=0x00;
	
	msg(DBG_XA1,"cc_ModeSelect: %02X %04X\n",
	    D_S[d].sense_byte, D_S[d].frame_size);
	
	if (fam1_drive)
	{
		drvcmd[0]=CMD1_SETMODE;
		drvcmd[1]=0x00;
		drvcmd[2]=D_S[d].sense_byte;
		drvcmd[3]=(D_S[d].frame_size>>8)&0xFF;
		drvcmd[4]=D_S[d].frame_size&0xFF;
		flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
	}
	else if (fam0L_drive)
	{
		drvcmd[0]=CMD0_SETMODE;
		drvcmd[1]=0x00;
		drvcmd[2]=(D_S[d].frame_size>>8)&0xFF;
		drvcmd[3]=D_S[d].frame_size&0xFF;
		drvcmd[4]=0x00;
		if(famL_drive)
			flags_cmd_out=f_putcmd|f_lopsta|f_getsta|f_ResponseStatus|f_obey_p_check;
		else
			flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus|f_obey_p_check;
	}
	else if (famT_drive)
	{
		return (-1);
	}
	response_count=0;
	i=cmd_out();
	if (i<0) return (i);
	D_S[d].diskstate_flags |= frame_size_bit;
	return (0);
}
/*==========================================================================*/
static int cc_GetVolume(void)
{
	int i;
	u_char switches;
	u_char chan0=0;
	u_char vol0=0;
	u_char chan1=1;
	u_char vol1=0;
	
	if (famV_drive) return (0);
	D_S[d].diskstate_flags &= ~volume_bit;
	clr_cmdbuf();
	if (fam1_drive)
	{
		drvcmd[0]=CMD1_GETMODE;
		drvcmd[1]=0x05;
		response_count=5;
		flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
	}
	else if (fam2_drive)
	{
		drvcmd[0]=CMD2_GETMODE;
		drvcmd[1]=0x0E;
		response_count=5;
		flags_cmd_out=f_putcmd;
	}
	else if (fam0L_drive)
	{
		drvcmd[0]=CMD0_GETMODE;
		drvcmd[1]=0x03;
		response_count=2;
		if(famL_drive)
			flags_cmd_out=f_putcmd;
		else
			flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus|f_obey_p_check;
	}
	else if (famT_drive)
	{
		i=cc_get_mode_T();
		if (i<0) return (i);
	}
	if (!famT_drive)
	{
		i=cmd_out();
		if (i<0) return (i);
	}
	if (fam1_drive)
	{
		chan0=infobuf[1]&0x0F;
		vol0=infobuf[2];
		chan1=infobuf[3]&0x0F;
		vol1=infobuf[4];
		if (chan0==0)
		{
			chan0=1;
			vol0=0;
		}
		if (chan1==0)
		{
			chan1=2;
			vol1=0;
		}
		chan0 >>= 1;
		chan1 >>= 1;
	}
	else if (fam2_drive)
	{
		chan0=infobuf[1];
		vol0=infobuf[2];
		chan1=infobuf[3];
		vol1=infobuf[4];
	}
	else if (famL_drive)
	{
		chan0=0;
		chan1=1;
		vol0=vol1=infobuf[1];
		switches=infobuf[0];
		if ((switches&0x80)!=0) chan0=1;
		if ((switches&0x40)!=0) chan1=0;
	}
	else if (fam0_drive) /* different firmware levels */
	{
		chan0=0;
		chan1=1;
		vol0=vol1=infobuf[1];
		if (D_S[d].drv_type>=drv_201)
		{
			if (D_S[d].drv_type<drv_300)
			{
				switches=infobuf[0];
				if ((switches&0x80)!=0) vol0=0;
				if ((switches&0x40)!=0) vol1=0;
				if (D_S[d].drv_type>=drv_211)
				{
					if ((switches&0x20)!=0) chan0=1;
					if ((switches&0x10)!=0) chan1=0;
				}
			}
			else
			{
				vol0=infobuf[0];
				if ((vol0&0x01)!=0) chan0=1;
				if ((vol1&0x01)==0) chan1=0;
				vol0 &= 0xFC;
				vol1 &= 0xFC;
				if (vol0!=0) vol0 += 3;
				if (vol1!=0) vol1 += 3;
			}
		}
	}
	else if (famT_drive)
	{
		D_S[d].volume_control=infobuf[7];
		chan0=0;
		chan1=1;
		if (D_S[d].volume_control&0x10) vol0=0;
		else vol0=0xff;
		if (D_S[d].volume_control&0x20) vol1=0;
		else vol1=0xff;
	}
	D_S[d].vol_chan0=chan0;
	D_S[d].vol_ctrl0=vol0;
	D_S[d].vol_chan1=chan1;
	D_S[d].vol_ctrl1=vol1;
#if 000
	D_S[d].vol_chan2=2;
	D_S[d].vol_ctrl2=0xFF;
	D_S[d].vol_chan3=3;
	D_S[d].vol_ctrl3=0xFF;
#endif 000
	D_S[d].diskstate_flags |= volume_bit;
	return (0);
}
/*==========================================================================*/
static int cc_ReadCapacity(void)
{
	int i, j;
	
	if (fam2_drive) return (0); /* some firmware lacks this command */
	if (famLV_drive) return (0); /* some firmware lacks this command */
	if (famT_drive) return (0); /* done with cc_ReadTocDescr() */
	D_S[d].diskstate_flags &= ~cd_size_bit;
	for (j=3;j>0;j--)
	{
		clr_cmdbuf();
		if (fam1_drive)
		{
			drvcmd[0]=CMD1_CAPACITY;
			response_count=5;
			flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
		}
#if 00
		else if (fam2_drive)
		{
			drvcmd[0]=CMD2_CAPACITY;
			response_count=8;
			flags_cmd_out=f_putcmd;
		}
#endif
		else if (fam0_drive)
		{
			drvcmd[0]=CMD0_CAPACITY;
			response_count=5;
			flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus|f_obey_p_check;
		}
		i=cmd_out();
		if (i>=0) break;
		msg(DBG_000,"cc_ReadCapacity: cmd_out: err %d\n", i);
		cc_ReadError();
	}
	if (j==0) return (i);
	if (fam1_drive) D_S[d].CDsize_frm=msf2blk(make32(make16(0,infobuf[0]),make16(infobuf[1],infobuf[2])))+CD_MSF_OFFSET;
	else if (fam0_drive) D_S[d].CDsize_frm=make32(make16(0,infobuf[0]),make16(infobuf[1],infobuf[2]));
#if 00
	else if (fam2_drive) D_S[d].CDsize_frm=make32(make16(infobuf[0],infobuf[1]),make16(infobuf[2],infobuf[3]));
#endif
	D_S[d].diskstate_flags |= cd_size_bit;
	msg(DBG_000,"cc_ReadCapacity: %d frames.\n", D_S[d].CDsize_frm);
	return (0);
}
/*==========================================================================*/
static int cc_ReadTocDescr(void)
{
	int i;
	
	D_S[d].diskstate_flags &= ~toc_bit;
	clr_cmdbuf();
	if (fam1_drive)
	{
		drvcmd[0]=CMD1_DISKINFO;
		response_count=6;
		flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
	}
	else if (fam0LV_drive)
	{
		drvcmd[0]=CMD0_DISKINFO;
		response_count=6;
		if(famLV_drive)
			flags_cmd_out=f_putcmd;
		else
			flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus|f_obey_p_check;
	}
	else if (fam2_drive)
	{
		/* possibly longer timeout periods necessary */
		D_S[d].f_multisession=0;
		drvcmd[0]=CMD2_DISKINFO;
		drvcmd[1]=0x02;
		drvcmd[2]=0xAB;
		drvcmd[3]=0xFF; /* session */
		response_count=8;
		flags_cmd_out=f_putcmd;
	}
	else if (famT_drive)
	{
		D_S[d].f_multisession=0;
		response_count=12;
		drvcmd[0]=CMDT_DISKINFO;
		drvcmd[1]=0x02;
		drvcmd[6]=CDROM_LEADOUT;
		drvcmd[8]=response_count;
		drvcmd[9]=0x00;
	}
	i=cmd_out();
	if (i<0) return (i);
	if ((famT_drive)&&(i<response_count)) return (-100-i);
	if ((fam1_drive)||(fam2_drive)||(fam0LV_drive))
		D_S[d].xa_byte=infobuf[0];
	if (fam2_drive)
	{
		D_S[d].first_session=infobuf[1];
		D_S[d].last_session=infobuf[2];
		D_S[d].n_first_track=infobuf[3];
		D_S[d].n_last_track=infobuf[4];
		if (D_S[d].first_session!=D_S[d].last_session)
		{
			D_S[d].f_multisession=1;
			D_S[d].lba_multi=msf2blk(make32(make16(0,infobuf[5]),make16(infobuf[6],infobuf[7])));
		}
#if 0
		if (D_S[d].first_session!=D_S[d].last_session)
		{
			if (D_S[d].last_session<=20)
				zwanzig=D_S[d].last_session+1;
			else zwanzig=20;
			for (count=D_S[d].first_session;count<zwanzig;count++)
			{
				drvcmd[0]=CMD2_DISKINFO;
				drvcmd[1]=0x02;
				drvcmd[2]=0xAB;
				drvcmd[3]=count;
				response_count=8;
				flags_cmd_out=f_putcmd;
				i=cmd_out();
				if (i<0) return (i);
				D_S[d].msf_multi_n[count]=make32(make16(0,infobuf[5]),make16(infobuf[6],infobuf[7]));
			}
			D_S[d].diskstate_flags |= multisession_bit;
		}
#endif
		drvcmd[0]=CMD2_DISKINFO;
		drvcmd[1]=0x02;
		drvcmd[2]=0xAA;
		drvcmd[3]=0xFF;
		response_count=5;
		flags_cmd_out=f_putcmd;
		i=cmd_out();
		if (i<0) return (i);
		D_S[d].size_msf=make32(make16(0,infobuf[2]),make16(infobuf[3],infobuf[4]));
		D_S[d].size_blk=msf2blk(D_S[d].size_msf);
		D_S[d].CDsize_frm=D_S[d].size_blk+1;
	}
	else if (famT_drive)
	{
		D_S[d].size_msf=make32(make16(infobuf[8],infobuf[9]),make16(infobuf[10],infobuf[11]));
		D_S[d].size_blk=msf2blk(D_S[d].size_msf);
		D_S[d].CDsize_frm=D_S[d].size_blk+1;
		D_S[d].n_first_track=infobuf[2];
		D_S[d].n_last_track=infobuf[3];
	}
	else
	{
		D_S[d].n_first_track=infobuf[1];
		D_S[d].n_last_track=infobuf[2];
		D_S[d].size_msf=make32(make16(0,infobuf[3]),make16(infobuf[4],infobuf[5]));
		D_S[d].size_blk=msf2blk(D_S[d].size_msf);
		if (famLV_drive) D_S[d].CDsize_frm=D_S[d].size_blk+1;
	}
	D_S[d].diskstate_flags |= toc_bit;
	msg(DBG_TOC,"TocDesc: xa %02X firstt %02X lastt %02X size %08X firstses %02X lastsess %02X\n",
	    D_S[d].xa_byte,
	    D_S[d].n_first_track,
	    D_S[d].n_last_track,
	    D_S[d].size_msf,
	    D_S[d].first_session,
	    D_S[d].last_session);
	return (0);
}
/*==========================================================================*/
static int cc_ReadTocEntry(int num)
{
	int i;
	
	clr_cmdbuf();
	if (fam1_drive)
	{
		drvcmd[0]=CMD1_READTOC;
		drvcmd[2]=num;
		response_count=8;
		flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
	}
	else if (fam2_drive)
	{
		/* possibly longer timeout periods necessary */
		drvcmd[0]=CMD2_DISKINFO;
		drvcmd[1]=0x02;
		drvcmd[2]=num;
		response_count=5;
		flags_cmd_out=f_putcmd;
	}
	else if (fam0LV_drive)
	{
		drvcmd[0]=CMD0_READTOC;
		drvcmd[1]=0x02;
		drvcmd[2]=num;
		response_count=8;
		if (famLV_drive)
			flags_cmd_out=f_putcmd;
		else
		  flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus|f_obey_p_check;
	}
	else if (famT_drive)
	{
		response_count=12;
		drvcmd[0]=CMDT_DISKINFO;
		drvcmd[1]=0x02;
		drvcmd[6]=num;
		drvcmd[8]=response_count;
		drvcmd[9]=0x00;
	}
	i=cmd_out();
	if (i<0) return (i);
	if ((famT_drive)&&(i<response_count)) return (-100-i);
	if ((fam1_drive)||(fam0LV_drive))
	{
		D_S[d].TocEnt_nixbyte=infobuf[0];
		i=1;
	}
	else if (fam2_drive) i=0;
	else if (famT_drive) i=5;
	D_S[d].TocEnt_ctl_adr=swap_nibbles(infobuf[i++]);
	if ((fam1_drive)||(fam0L_drive))
	{
		D_S[d].TocEnt_number=infobuf[i++];
		D_S[d].TocEnt_format=infobuf[i];
	}
	else
	  {
	    D_S[d].TocEnt_number=num;
	    D_S[d].TocEnt_format=0;
	  }
	if (fam1_drive) i=4;
	else if (fam0LV_drive) i=5;
	else if (fam2_drive) i=2;
	else if (famT_drive) i=9;
	D_S[d].TocEnt_address=make32(make16(0,infobuf[i]),
				     make16(infobuf[i+1],infobuf[i+2]));
	for (i=0;i<response_count;i++)
		sprintf(&msgbuf[i*3], " %02X", infobuf[i]);
	msgbuf[i*3]=0;
	msg(DBG_ECS,"TocEntry:%s\n", msgbuf);
	msg(DBG_TOC,"TocEntry: %02X %02X %02X %02X %08X\n",
	    D_S[d].TocEnt_nixbyte, D_S[d].TocEnt_ctl_adr,
	    D_S[d].TocEnt_number, D_S[d].TocEnt_format,
	    D_S[d].TocEnt_address);
	return (0);
}
/*==========================================================================*/
static int cc_ReadPacket(void)
{
	int i;
	
	clr_cmdbuf();
	drvcmd[0]=CMD0_PACKET;
	drvcmd[1]=response_count;
	if(famL_drive) flags_cmd_out=f_putcmd;
	else if (fam01_drive)
		flags_cmd_out=f_putcmd|f_getsta|f_ResponseStatus|f_obey_p_check;
	else if (fam2_drive) return (-1); /* not implemented yet */
	else if (famT_drive)
	{
		return (-1);
	}
	i=cmd_out();
	return (i);
}
/*==========================================================================*/
static int convert_UPC(u_char *p)
{
	int i;
	
	p++;
	if (fam0L_drive) p[13]=0;
	for (i=0;i<7;i++)
	{
		if (fam1_drive) D_S[d].UPC_buf[i]=swap_nibbles(*p++);
		else if (fam0L_drive)
		{
			D_S[d].UPC_buf[i]=((*p++)<<4)&0xFF;
			D_S[d].UPC_buf[i] |= *p++;
		}
		else if (famT_drive)
		{
			return (-1);
		}
		else /* CD200 */
		{
			return (-1);
		}
	}
	D_S[d].UPC_buf[6] &= 0xF0;
	return (0);
}
/*==========================================================================*/
static int cc_ReadUPC(void)
{
	int i;
#if TEST_UPC
	int block, checksum;
#endif TEST_UPC
	
	if (fam2_drive) return (0); /* not implemented yet */
	if (famT_drive)	return (0); /* not implemented yet */
	if (famV_drive)	return (0); /* not implemented yet */
#if 1
	if (fam0_drive) return (0); /* but it should work */
#endif 1
	
	D_S[d].diskstate_flags &= ~upc_bit;
#if TEST_UPC
	for (block=CD_MSF_OFFSET+1;block<CD_MSF_OFFSET+200;block++)
	{
#endif TEST_UPC
		clr_cmdbuf();
		if (fam1_drive)
		{
			drvcmd[0]=CMD1_READ_UPC;
#if TEST_UPC
			drvcmd[1]=(block>>16)&0xFF;
			drvcmd[2]=(block>>8)&0xFF;
			drvcmd[3]=block&0xFF;
#endif TEST_UPC
			response_count=8;
			flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
		}
		else if (fam0L_drive)
		{
			drvcmd[0]=CMD0_READ_UPC;
#if TEST_UPC
			drvcmd[2]=(block>>16)&0xFF;
			drvcmd[3]=(block>>8)&0xFF;
			drvcmd[4]=block&0xFF;
#endif TEST_UPC
			response_count=0;
			flags_cmd_out=f_putcmd|f_lopsta|f_getsta|f_ResponseStatus|f_obey_p_check|f_bit1;
		}
		else if (fam2_drive)
		{
			return (-1);
		}
		else if (famT_drive)
		{
			return (-1);
		}
		i=cmd_out();
		if (i<0)
		{
			msg(DBG_000,"cc_ReadUPC cmd_out: err %d\n", i);
			return (i);
		}
		if (fam0L_drive)
		{
			response_count=16;
			if (famL_drive) flags_cmd_out=f_putcmd;
			i=cc_ReadPacket();
			if (i<0)
			{
				msg(DBG_000,"cc_ReadUPC ReadPacket: err %d\n", i);
				return (i);
			}
		}
#if TEST_UPC
		checksum=0;
#endif TEST_UPC
		for (i=0;i<(fam1_drive?8:16);i++)
		{
#if TEST_UPC
			checksum |= infobuf[i];
#endif TEST_UPC
			sprintf(&msgbuf[i*3], " %02X", infobuf[i]);
		}
		msgbuf[i*3]=0;
		msg(DBG_UPC,"UPC info:%s\n", msgbuf);
#if TEST_UPC
		if ((checksum&0x7F)!=0) break;
	}
#endif TEST_UPC
	D_S[d].UPC_ctl_adr=0;
	if (fam1_drive) i=0;
	else i=2;
	if ((infobuf[i]&0x80)!=0)
	{
		convert_UPC(&infobuf[i]);
		D_S[d].UPC_ctl_adr = (D_S[d].TocEnt_ctl_adr & 0xF0) | 0x02;
	}
	for (i=0;i<7;i++)
		sprintf(&msgbuf[i*3], " %02X", D_S[d].UPC_buf[i]);
	sprintf(&msgbuf[i*3], " (%02X)", D_S[d].UPC_ctl_adr);
	msgbuf[i*3+5]=0;
	msg(DBG_UPC,"UPC code:%s\n", msgbuf);
	D_S[d].diskstate_flags |= upc_bit;
	return (0);
}

static int sbpcd_get_mcn(struct cdrom_device_info *cdi, struct cdrom_mcn *mcn)
{
	int i;
	unsigned char *mcnp = mcn->medium_catalog_number;
	unsigned char *resp;

	D_S[d].diskstate_flags &= ~upc_bit;
	clr_cmdbuf();
	if (fam1_drive)
	{
		drvcmd[0]=CMD1_READ_UPC;
		response_count=8;
		flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
	}
	else if (fam0L_drive)
	{
		drvcmd[0]=CMD0_READ_UPC;
		response_count=0;
		flags_cmd_out=f_putcmd|f_lopsta|f_getsta|f_ResponseStatus|f_obey_p_check|f_bit1;
	}
	else if (fam2_drive)
	{
		return (-1);
	}
	else if (famT_drive)
	{
		return (-1);
	}
	i=cmd_out();
	if (i<0)
	{
		msg(DBG_000,"cc_ReadUPC cmd_out: err %d\n", i);
		return (i);
	}
	if (fam0L_drive)
	{
		response_count=16;
		if (famL_drive) flags_cmd_out=f_putcmd;
		i=cc_ReadPacket();
		if (i<0)
		{
			msg(DBG_000,"cc_ReadUPC ReadPacket: err %d\n", i);
			return (i);
		}
	}
	D_S[d].UPC_ctl_adr=0;
	if (fam1_drive) i=0;
	else i=2;

	resp = infobuf + i;
	if (*resp++ == 0x80) {
		/* packed bcd to single ASCII digits */
		*mcnp++ = (*resp >> 4)     + '0';
		*mcnp++ = (*resp++ & 0x0f) + '0';
		*mcnp++ = (*resp >> 4)     + '0';
		*mcnp++ = (*resp++ & 0x0f) + '0';
		*mcnp++ = (*resp >> 4)     + '0';
		*mcnp++ = (*resp++ & 0x0f) + '0';
		*mcnp++ = (*resp >> 4)     + '0';
		*mcnp++ = (*resp++ & 0x0f) + '0';
		*mcnp++ = (*resp >> 4)     + '0';
		*mcnp++ = (*resp++ & 0x0f) + '0';
		*mcnp++ = (*resp >> 4)     + '0';
		*mcnp++ = (*resp++ & 0x0f) + '0';
		*mcnp++ = (*resp >> 4)     + '0';
	}
	*mcnp = '\0';

	D_S[d].diskstate_flags |= upc_bit;
	return (0);
}

/*==========================================================================*/
static int cc_CheckMultiSession(void)
{
	int i;
	
	if (fam2_drive) return (0);
	D_S[d].f_multisession=0;
	D_S[d].lba_multi=0;
	if (fam0_drive) return (0);
	clr_cmdbuf();
	if (fam1_drive)
	{
		drvcmd[0]=CMD1_MULTISESS;
		response_count=6;
		flags_cmd_out=f_putcmd|f_ResponseStatus|f_obey_p_check;
		i=cmd_out();
		if (i<0) return (i);
		if ((infobuf[0]&0x80)!=0)
		{
			D_S[d].f_multisession=1;
			D_S[d].lba_multi=msf2blk(make32(make16(0,infobuf[1]),
							make16(infobuf[2],infobuf[3])));
		}
	}
	else if (famLV_drive)
	{
		drvcmd[0]=CMDL_MULTISESS;
		drvcmd[1]=3;
		drvcmd[2]=1;
		response_count=8;
		flags_cmd_out=f_putcmd;
		i=cmd_out();
		if (i<0) return (i);
		D_S[d].lba_multi=msf2blk(make32(make16(0,infobuf[5]),
						make16(infobuf[6],infobuf[7])));
	}
	else if (famT_drive)
	{
		response_count=12;
		drvcmd[0]=CMDT_DISKINFO;
		drvcmd[1]=0x02;
		drvcmd[6]=0;
		drvcmd[8]=response_count;
		drvcmd[9]=0x40;
		i=cmd_out();
		if (i<0) return (i);
		if (i<response_count) return (-100-i);
		D_S[d].first_session=infobuf[2];
		D_S[d].last_session=infobuf[3];
		D_S[d].track_of_last_session=infobuf[6];
		if (D_S[d].first_session!=D_S[d].last_session)
		{
			D_S[d].f_multisession=1;
			D_S[d].lba_multi=msf2blk(make32(make16(0,infobuf[9]),make16(infobuf[10],infobuf[11])));
		}
	}
	for (i=0;i<response_count;i++)
		sprintf(&msgbuf[i*3], " %02X", infobuf[i]);
	msgbuf[i*3]=0;
	msg(DBG_MUL,"MultiSession Info:%s (%d)\n", msgbuf, D_S[d].lba_multi);
	if (D_S[d].lba_multi>200)
	{
		D_S[d].f_multisession=1;
		msg(DBG_MUL,"MultiSession base: %06X\n", D_S[d].lba_multi);
	}
	return (0);
}
/*==========================================================================*/
#if FUTURE
static int cc_SubChanInfo(int frame, int count, u_char *buffer)
	/* "frame" is a RED BOOK (msf-bin) address */
{
	int i;
	
	if (fam0LV_drive) return (-ENOSYS); /* drive firmware lacks it */
	if (famT_drive)
	{
		return (-1);
	}
#if 0
	if (D_S[d].audio_state!=audio_playing) return (-ENODATA);
#endif
	clr_cmdbuf();
	drvcmd[0]=CMD1_SUBCHANINF;
	drvcmd[1]=(frame>>16)&0xFF;
	drvcmd[2]=(frame>>8)&0xFF;
	drvcmd[3]=frame&0xFF;
	drvcmd[5]=(count>>8)&0xFF;
	drvcmd[6]=count&0xFF;
	flags_cmd_out=f_putcmd|f_respo2|f_ResponseStatus|f_obey_p_check;
	cmd_type=READ_SC;
	D_S[d].frame_size=CD_FRAMESIZE_SUB;
	i=cmd_out(); /* which buffer to use? */
	return (i);
}
#endif FUTURE
/*==========================================================================*/
static void __init check_datarate(void)
{
	int i=0;
	
	msg(DBG_IOX,"check_datarate entered.\n");
	datarate=0;
#if TEST_STI
	for (i=0;i<=1000;i++) printk(".");
#endif
	/* set a timer to make (timed_out_delay!=0) after 1.1 seconds */
#if 1
	del_timer(&delay_timer);
#endif
	delay_timer.expires=jiffies+11*HZ/10;
	timed_out_delay=0;
	add_timer(&delay_timer);
#if 0
	msg(DBG_TIM,"delay timer started (11*HZ/10).\n");
#endif
	do
	{
		i=inb(CDi_status);
		datarate++;
#if 1
		if (datarate>0x6FFFFFFF) break;
#endif 00000
	}
	while (!timed_out_delay);
	del_timer(&delay_timer);
#if 0
	msg(DBG_TIM,"datarate: %04X\n", datarate);
#endif
	if (datarate<65536) datarate=65536;
	maxtim16=datarate*16;
	maxtim04=datarate*4;
	maxtim02=datarate*2;
	maxtim_8=datarate/32;
#if LONG_TIMING
	maxtim_data=datarate/100;
#else
	maxtim_data=datarate/300;
#endif LONG_TIMING
#if 0
	msg(DBG_TIM,"maxtim_8 %d, maxtim_data %d.\n", maxtim_8, maxtim_data);
#endif
}
/*==========================================================================*/
#if 0
static int c2_ReadError(int fam)
{
	int i;
	
	clr_cmdbuf();
	response_count=9;
	clr_respo_buf(9);
	if (fam==1)
	{
		drvcmd[0]=CMD0_READ_ERR; /* same as CMD1_ and CMDL_ */
		i=do_cmd(f_putcmd|f_lopsta|f_getsta|f_ResponseStatus);
	}
	else if (fam==2)
	{
		drvcmd[0]=CMD2_READ_ERR;
		i=do_cmd(f_putcmd);
	}
	else return (-1);
	return (i);
}
#endif
/*==========================================================================*/
static void __init ask_mail(void)
{
	int i;
	
	msg(DBG_INF, "please mail the following lines to emoenke@gwdg.de\n");
	msg(DBG_INF, "(don't mail if you are not using the actual kernel):\n");
	msg(DBG_INF, "%s\n", VERSION);
	msg(DBG_INF, "address %03X, type %s, drive %s (ID %d)\n",
	    CDo_command, type, D_S[d].drive_model, D_S[d].drv_id);
	for (i=0;i<12;i++)
		sprintf(&msgbuf[i*3], " %02X", infobuf[i]);
	msgbuf[i*3]=0;
	msg(DBG_INF,"infobuf =%s\n", msgbuf);
	for (i=0;i<12;i++)
		sprintf(&msgbuf[i*3], " %c ", infobuf[i]);
	msgbuf[i*3]=0;
	msg(DBG_INF,"infobuf =%s\n", msgbuf);
}
/*==========================================================================*/
static int __init check_version(void)
{
	int i, j, l;
	int teac_possible=0;
	
	msg(DBG_INI,"check_version: id=%d, d=%d.\n", D_S[d].drv_id, d);
	D_S[d].drv_type=0;

	/* check for CR-52x, CR-56x, LCS-7260 and ECS-AT */
	/* clear any pending error state */
	clr_cmdbuf();
	drvcmd[0]=CMD0_READ_ERR; /* same as CMD1_ and CMDL_ */
	response_count=9;
	flags_cmd_out=f_putcmd;
	i=cmd_out();
	if (i<0) msg(DBG_INI,"CMD0_READ_ERR returns %d (ok anyway).\n",i);
	/* read drive version */
	clr_cmdbuf();
	for (i=0;i<12;i++) infobuf[i]=0;
	drvcmd[0]=CMD0_READ_VER; /* same as CMD1_ and CMDL_ */
	response_count=12; /* fam1: only 11 */
	flags_cmd_out=f_putcmd;
	i=cmd_out();
	if (i<-1) msg(DBG_INI,"CMD0_READ_VER returns %d\n",i);
	if (i==-11) teac_possible++;
	j=0;
	for (i=0;i<12;i++) j+=infobuf[i];
	if (j)
	{
		for (i=0;i<12;i++)
			sprintf(&msgbuf[i*3], " %02X", infobuf[i]);
		msgbuf[i*3]=0;
		msg(DBG_ECS,"infobuf =%s\n", msgbuf);
		for (i=0;i<12;i++)
			sprintf(&msgbuf[i*3], " %c ", infobuf[i]);
		msgbuf[i*3]=0;
		msg(DBG_ECS,"infobuf =%s\n", msgbuf);
	}
	for (i=0;i<4;i++) if (infobuf[i]!=family1[i]) break;
	if (i==4)
	{
		D_S[d].drive_model[0]='C';
		D_S[d].drive_model[1]='R';
		D_S[d].drive_model[2]='-';
		D_S[d].drive_model[3]='5';
		D_S[d].drive_model[4]=infobuf[i++];
		D_S[d].drive_model[5]=infobuf[i++];
		D_S[d].drive_model[6]=0;
		D_S[d].drv_type=drv_fam1;
	}
	if (!D_S[d].drv_type)
	{
		for (i=0;i<8;i++) if (infobuf[i]!=family0[i]) break;
		if (i==8)
		{
			D_S[d].drive_model[0]='C';
			D_S[d].drive_model[1]='R';
			D_S[d].drive_model[2]='-';
			D_S[d].drive_model[3]='5';
			D_S[d].drive_model[4]='2';
			D_S[d].drive_model[5]='x';
			D_S[d].drive_model[6]=0;
			D_S[d].drv_type=drv_fam0;
		}
	}
	if (!D_S[d].drv_type)
	{
		for (i=0;i<8;i++) if (infobuf[i]!=familyL[i]) break;
		if (i==8)
		{
			for (j=0;j<8;j++)
				D_S[d].drive_model[j]=infobuf[j];
			D_S[d].drive_model[8]=0;
			D_S[d].drv_type=drv_famL;
		}
	}
	if (!D_S[d].drv_type)
	{
		for (i=0;i<6;i++) if (infobuf[i]!=familyV[i]) break;
		if (i==6)
		{
			for (j=0;j<6;j++)
				D_S[d].drive_model[j]=infobuf[j];
			D_S[d].drive_model[6]=0;
			D_S[d].drv_type=drv_famV;
			i+=2; /* 2 blanks before version */
		}
	}
	if (!D_S[d].drv_type)
	{
		/* check for CD200 */
		clr_cmdbuf();
		drvcmd[0]=CMD2_READ_ERR;
		response_count=9;
		flags_cmd_out=f_putcmd;
		i=cmd_out();
		if (i<0) msg(DBG_INI,"CMD2_READERR returns %d (ok anyway).\n",i);
		if (i<0) msg(DBG_000,"CMD2_READERR returns %d (ok anyway).\n",i);
		/* read drive version */
		clr_cmdbuf();
		for (i=0;i<12;i++) infobuf[i]=0;
		if (sbpro_type==1) OUT(CDo_sel_i_d,0);
#if 0
		OUT(CDo_reset,0);
		sbp_sleep(6*HZ);
		OUT(CDo_enable,D_S[d].drv_sel);
#endif 0
		drvcmd[0]=CMD2_READ_VER;
		response_count=12;
		flags_cmd_out=f_putcmd;
		i=cmd_out();
		if (i<0) msg(DBG_INI,"CMD2_READ_VER returns %d\n",i);
		if (i==-7) teac_possible++;
		j=0;
		for (i=0;i<12;i++) j+=infobuf[i];
		if (j)
		{
			for (i=0;i<12;i++)
				sprintf(&msgbuf[i*3], " %02X", infobuf[i]);
			msgbuf[i*3]=0;
			msg(DBG_IDX,"infobuf =%s\n", msgbuf);
			for (i=0;i<12;i++)
				sprintf(&msgbuf[i*3], " %c ", infobuf[i]);
			msgbuf[i*3]=0;
			msg(DBG_IDX,"infobuf =%s\n", msgbuf);
		}
		if (i>=0)
		{
			for (i=0;i<5;i++) if (infobuf[i]!=family2[i]) break;
			if (i==5)
			{
				D_S[d].drive_model[0]='C';
				D_S[d].drive_model[1]='D';
				D_S[d].drive_model[2]='2';
				D_S[d].drive_model[3]='0';
				D_S[d].drive_model[4]='0';
				D_S[d].drive_model[5]=infobuf[i++];
				D_S[d].drive_model[6]=infobuf[i++];
				D_S[d].drive_model[7]=0;
				D_S[d].drv_type=drv_fam2;
			}
		}
	}
	if (!D_S[d].drv_type)
	{
		/* check for TEAC CD-55A */
		msg(DBG_TEA,"teac_possible: %d\n",teac_possible);
		for (j=1;j<=((D_S[d].drv_id==0)?3:1);j++)
		{
			for (l=1;l<=((D_S[d].drv_id==0)?10:1);l++)
			{
				msg(DBG_TEA,"TEAC reset #%d-%d.\n", j, l);
				if (sbpro_type==1) OUT(CDo_reset,0);
				else
				{
					OUT(CDo_enable,D_S[d].drv_sel);
					OUT(CDo_sel_i_d,0);
					OUT(CDo_command,CMDT_RESET);
					for (i=0;i<9;i++) OUT(CDo_command,0);
				}
				sbp_sleep(5*HZ/10);
				OUT(CDo_enable,D_S[d].drv_sel);
				OUT(CDo_sel_i_d,0);
				i=inb(CDi_status);
				msg(DBG_TEA,"TEAC CDi_status: %02X.\n",i);
#if 0
				if (i&s_not_result_ready) continue; /* drive not present or ready */
#endif
				i=inb(CDi_info);
				msg(DBG_TEA,"TEAC CDi_info: %02X.\n",i);
				if (i==0x55) break; /* drive found */
			}
			if (i==0x55) break; /* drive found */
		}
		if (i==0x55) /* drive found */
		{
			msg(DBG_TEA,"TEAC drive found.\n");
			clr_cmdbuf();
			flags_cmd_out=f_putcmd;
			response_count=12;
			drvcmd[0]=CMDT_READ_VER;
			drvcmd[4]=response_count;
			for (i=0;i<12;i++) infobuf[i]=0;
			i=cmd_out_T();
			if (i!=0) msg(DBG_TEA,"cmd_out_T(CMDT_READ_VER) returns %d.\n",i);
			for (i=1;i<6;i++) if (infobuf[i]!=familyT[i-1]) break;
			if (i==6)
			{
				D_S[d].drive_model[0]='C';
				D_S[d].drive_model[1]='D';
				D_S[d].drive_model[2]='-';
				D_S[d].drive_model[3]='5';
				D_S[d].drive_model[4]='5';
				D_S[d].drive_model[5]=0;
				D_S[d].drv_type=drv_famT;
			}
		}
	}
	if (!D_S[d].drv_type)
	{
		msg(DBG_TEA,"no drive found at address %03X under ID %d.\n",CDo_command,D_S[d].drv_id);
		return (-522);
	}
	for (j=0;j<4;j++) D_S[d].firmware_version[j]=infobuf[i+j];
	if (famL_drive)
	{
	  u_char lcs_firm_e1[]="A E1";
	  u_char lcs_firm_f4[]="A4F4";
		
	  for (j=0;j<4;j++)
	    if (D_S[d].firmware_version[j]!=lcs_firm_e1[j]) break;
	  if (j==4) D_S[d].drv_type=drv_e1;
	  
	  for (j=0;j<4;j++)
	    if (D_S[d].firmware_version[j]!=lcs_firm_f4[j]) break;
	  if (j==4) D_S[d].drv_type=drv_f4;

	  if (D_S[d].drv_type==drv_famL) ask_mail();
	}
	else if (famT_drive)
	{
		j=infobuf[4]; /* one-byte version??? - here: 0x15 */
		if (j=='5')
		{
			D_S[d].firmware_version[0]=infobuf[7];
			D_S[d].firmware_version[1]=infobuf[8];
			D_S[d].firmware_version[2]=infobuf[10];
			D_S[d].firmware_version[3]=infobuf[11];
		}
		else
		{
			if (j!=0x15) ask_mail();
			D_S[d].firmware_version[0]='0';
			D_S[d].firmware_version[1]='.';
			D_S[d].firmware_version[2]='0'+(j>>4);
			D_S[d].firmware_version[3]='0'+(j&0x0f);
		}
	}
	else /* CR-52x, CR-56x, CD200, ECS-AT */
	{
		j = (D_S[d].firmware_version[0] & 0x0F) * 100 +
			(D_S[d].firmware_version[2] & 0x0F) *10 +
				(D_S[d].firmware_version[3] & 0x0F);
		if (fam0_drive)
		{
			if (j<200) D_S[d].drv_type=drv_199;
			else if (j<201) D_S[d].drv_type=drv_200;
			else if (j<210) D_S[d].drv_type=drv_201;
			else if (j<211) D_S[d].drv_type=drv_210;
			else if (j<300) D_S[d].drv_type=drv_211;
			else if (j>=300) D_S[d].drv_type=drv_300;
		}
		else if (fam1_drive)
		{
			if (j<100) D_S[d].drv_type=drv_099;
			else
			{
				D_S[d].drv_type=drv_100;
				if ((j!=500)&&(j!=102)) ask_mail();
			}
		}
		else if (fam2_drive)
		{
			if (D_S[d].drive_model[5]=='F')
			{
				if ((j!=1)&&(j!=35)&&(j!=200)&&(j!=210))
				  ask_mail(); /* unknown version at time */
			}
			else
			{
				msg(DBG_INF,"this CD200 drive is not fully supported yet - only audio will work.\n");
				if ((j!=101)&&(j!=35))
				  ask_mail(); /* unknown version at time */
			}
		}
		else if (famV_drive)
		  {
		    if ((j==100)||(j==150)) D_S[d].drv_type=drv_at;
		    ask_mail(); /* hopefully we get some feedback by this */
		  }
	}
	msg(DBG_LCS,"drive type %02X\n",D_S[d].drv_type);
	msg(DBG_INI,"check_version done.\n");
	return (0);
}
/*==========================================================================*/
static void switch_drive(int i)
{
	d=i;
	OUT(CDo_enable,D_S[d].drv_sel);
	msg(DBG_DID,"drive %d (ID=%d) activated.\n", i, D_S[d].drv_id);
	return;
}
/*==========================================================================*/
#ifdef PATH_CHECK
/*
 * probe for the presence of an interface card
 */
static int __init check_card(int port)
{
#undef N_RESPO
#define N_RESPO 20
	int i, j, k;
	u_char response[N_RESPO];
	u_char save_port0;
	u_char save_port3;
	
	msg(DBG_INI,"check_card entered.\n");
	save_port0=inb(port+0);
	save_port3=inb(port+3);
	
	for (j=0;j<NR_SBPCD;j++)
	{
		OUT(port+3,j) ; /* enable drive #j */
		OUT(port+0,CMD0_PATH_CHECK);
		for (i=10;i>0;i--) OUT(port+0,0);
		for (k=0;k<N_RESPO;k++) response[k]=0;
		for (k=0;k<N_RESPO;k++)
		{
			for (i=10000;i>0;i--)
			{
				if (inb(port+1)&s_not_result_ready) continue;
				response[k]=inb(port+0);
				break;
			}
		}
		for (i=0;i<N_RESPO;i++)
			sprintf(&msgbuf[i*3], " %02X", response[i]);
		msgbuf[i*3]=0;
		msg(DBG_TEA,"path check 00 (%d): %s\n", j, msgbuf);
		OUT(port+0,CMD0_PATH_CHECK);
		for (i=10;i>0;i--) OUT(port+0,0);
		for (k=0;k<N_RESPO;k++) response[k]=0xFF;
		for (k=0;k<N_RESPO;k++)
		{
			for (i=10000;i>0;i--)
			{
				if (inb(port+1)&s_not_result_ready) continue;
				response[k]=inb(port+0);
				break;
			}
		}
		for (i=0;i<N_RESPO;i++)
			sprintf(&msgbuf[i*3], " %02X", response[i]);
		msgbuf[i*3]=0;
		msg(DBG_TEA,"path check 00 (%d): %s\n", j, msgbuf);

		if (response[0]==0xAA)
			if (response[1]==0x55)
				return (0);
	}
	for (j=0;j<NR_SBPCD;j++)
	{
		OUT(port+3,j) ; /* enable drive #j */
		OUT(port+0,CMD2_READ_VER);
		for (i=10;i>0;i--) OUT(port+0,0);
		for (k=0;k<N_RESPO;k++) response[k]=0;
		for (k=0;k<N_RESPO;k++)
		{
			for (i=1000000;i>0;i--)
			{
				if (inb(port+1)&s_not_result_ready) continue;
				response[k]=inb(port+0);
				break;
			}
		}
		for (i=0;i<N_RESPO;i++)
			sprintf(&msgbuf[i*3], " %02X", response[i]);
		msgbuf[i*3]=0;
		msg(DBG_TEA,"path check 12 (%d): %s\n", j, msgbuf);

		OUT(port+0,CMD2_READ_VER);
		for (i=10;i>0;i--) OUT(port+0,0);
		for (k=0;k<N_RESPO;k++) response[k]=0xFF;
		for (k=0;k<N_RESPO;k++)
		{
			for (i=1000000;i>0;i--)
			{
				if (inb(port+1)&s_not_result_ready) continue;
				response[k]=inb(port+0);
				break;
			}
		}
		for (i=0;i<N_RESPO;i++)
			sprintf(&msgbuf[i*3], " %02X", response[i]);
		msgbuf[i*3]=0;
		msg(DBG_TEA,"path check 12 (%d): %s\n", j, msgbuf);

		if (response[0]==0xAA)
			if (response[1]==0x55)
				return (0);
	}
	OUT(port+0,save_port0);
	OUT(port+3,save_port3);
	return (0); /* in any case - no real "function" at time */
}
#endif PATH_CHECK
/*==========================================================================*/
/*==========================================================================*/
/*
 * probe for the presence of drives on the selected controller
 */
static int __init check_drives(void)
{
	int i, j;
	
	msg(DBG_INI,"check_drives entered.\n");
	ndrives=0;
	for (j=0;j<MAX_DRIVES;j++)
	{
		D_S[ndrives].drv_id=j;
		if (sbpro_type==1) D_S[ndrives].drv_sel=(j&0x01)<<1|(j&0x02)>>1;
		else D_S[ndrives].drv_sel=j;
		switch_drive(ndrives);
		msg(DBG_INI,"check_drives: drive %d (ID=%d) activated.\n",ndrives,j);
		msg(DBG_000,"check_drives: drive %d (ID=%d) activated.\n",ndrives,j);
		i=check_version();
		if (i<0) msg(DBG_INI,"check_version returns %d.\n",i);
		else
		{
			D_S[d].drv_options=drv_pattern[j];
			if (fam0L_drive) D_S[d].drv_options&=~(speed_auto|speed_300|speed_150);
			msg(DBG_INF, "Drive %d (ID=%d): %.9s (%.4s) at 0x%03X (type %d)\n",
			    d,
			    D_S[d].drv_id,
			    D_S[d].drive_model,
			    D_S[d].firmware_version,
			    CDo_command,
			    sbpro_type);
			ndrives++;
		}
	}
	for (j=ndrives;j<NR_SBPCD;j++) D_S[j].drv_id=-1;
	if (ndrives==0) return (-1);
	return (0);
}
/*==========================================================================*/
#if FUTURE
/*
 *  obtain if requested service disturbs current audio state
 */            
static int obey_audio_state(u_char audio_state, u_char func,u_char subfunc)
{
	switch (audio_state)                   /* audio status from controller  */
	{
	case aud_11: /* "audio play in progress" */
	case audx11:
		switch (func)                      /* DOS command code */
		{
		case cmd_07: /* input flush  */
		case cmd_0d: /* open device  */
		case cmd_0e: /* close device */
		case cmd_0c: /* ioctl output */
			return (1);
		case cmd_03: /* ioctl input  */
			switch (subfunc)
				/* DOS ioctl input subfunction */
			{
			case cxi_00:
			case cxi_06:
			case cxi_09:
				return (1);
			default:
				return (ERROR15);
			}
			return (1);
		default:
			return (ERROR15);
		}
		return (1);
	case aud_12:                  /* "audio play paused"      */
	case audx12:
		return (1);
	default:
		return (2);
	}
}
/*==========================================================================*/
/* allowed is only
 * ioctl_o, flush_input, open_device, close_device, 
 * tell_address, tell_volume, tell_capabiliti,
 * tell_framesize, tell_CD_changed, tell_audio_posi
 */
static int check_allowed1(u_char func1, u_char func2)
{
#if 000
	if (func1==ioctl_o) return (0);
	if (func1==read_long) return (-1);
	if (func1==read_long_prefetch) return (-1);
	if (func1==seek) return (-1);
	if (func1==audio_play) return (-1);
	if (func1==audio_pause) return (-1);
	if (func1==audio_resume) return (-1);
	if (func1!=ioctl_i) return (0);
	if (func2==tell_SubQ_run_tot) return (-1);
	if (func2==tell_cdsize) return (-1);
	if (func2==tell_TocDescrip) return (-1);
	if (func2==tell_TocEntry) return (-1);
	if (func2==tell_subQ_info) return (-1);
	if (fam1_drive) if (func2==tell_SubChanInfo) return (-1);
	if (func2==tell_UPC) return (-1);
#else
	return (0);
#endif 000
}
/*==========================================================================*/
static int check_allowed2(u_char func1, u_char func2)
{
#if 000
	if (func1==read_long) return (-1);
	if (func1==read_long_prefetch) return (-1);
	if (func1==seek) return (-1);
	if (func1==audio_play) return (-1);
  if (func1!=ioctl_o) return (0);
	if (fam1_drive)
	{
		if (func2==EjectDisk) return (-1);
		if (func2==CloseTray) return (-1);
	}
#else
	return (0);
#endif 000
}
/*==========================================================================*/
static int check_allowed3(u_char func1, u_char func2)
{
#if 000
	if (func1==ioctl_i)
	{
		if (func2==tell_address) return (0);
		if (func2==tell_capabiliti) return (0);
		if (func2==tell_CD_changed) return (0);
		if (fam0L_drive) if (func2==tell_SubChanInfo) return (0);
		return (-1);
	}
	if (func1==ioctl_o)
	{
		if (func2==DriveReset) return (0);
		if (fam0L_drive)
		{
			if (func2==EjectDisk) return (0);
			if (func2==LockDoor) return (0);
	  if (func2==CloseTray) return (0);
		}
		return (-1);
    }
	if (func1==flush_input) return (-1);
	if (func1==read_long) return (-1);
	if (func1==read_long_prefetch) return (-1);
	if (func1==seek) return (-1);
	if (func1==audio_play) return (-1);
	if (func1==audio_pause) return (-1);
	if (func1==audio_resume) return (-1);
#else
	return (0);
#endif 000
}
/*==========================================================================*/
static int seek_pos_audio_end(void)
{
	int i;

	i=msf2blk(D_S[d].pos_audio_end)-1;
	if (i<0) return (-1);
	i=cc_Seek(i,0);
	return (i);
}
#endif FUTURE
/*==========================================================================*/
static int ReadToC(void)
{
	int i, j;
	D_S[d].diskstate_flags &= ~toc_bit;
	D_S[d].ored_ctl_adr=0;
	/* special handling of CD-I HE */
	if ((D_S[d].n_first_track == 2 && D_S[d].n_last_track == 2) ||
             D_S[d].xa_byte == 0x10)
        {
		D_S[d].TocBuffer[1].nixbyte=0;
		D_S[d].TocBuffer[1].ctl_adr=0x40;
		D_S[d].TocBuffer[1].number=1;
		D_S[d].TocBuffer[1].format=0;
		D_S[d].TocBuffer[1].address=blk2msf(0);
		D_S[d].ored_ctl_adr |= 0x40;
		D_S[d].n_first_track = 1;
		D_S[d].n_last_track = 1;
		D_S[d].xa_byte = 0x10;
                j = 2;
        } else
	for (j=D_S[d].n_first_track;j<=D_S[d].n_last_track;j++)
	{
		i=cc_ReadTocEntry(j);
		if (i<0)
		{
			msg(DBG_INF,"cc_ReadTocEntry(%d) returns %d.\n",j,i);
			return (i);
		}
		D_S[d].TocBuffer[j].nixbyte=D_S[d].TocEnt_nixbyte;
		D_S[d].TocBuffer[j].ctl_adr=D_S[d].TocEnt_ctl_adr;
		D_S[d].TocBuffer[j].number=D_S[d].TocEnt_number;
		D_S[d].TocBuffer[j].format=D_S[d].TocEnt_format;
		D_S[d].TocBuffer[j].address=D_S[d].TocEnt_address;
		D_S[d].ored_ctl_adr |= D_S[d].TocEnt_ctl_adr;
	}
	/* fake entry for LeadOut Track */
	D_S[d].TocBuffer[j].nixbyte=0;
	D_S[d].TocBuffer[j].ctl_adr=0;
	D_S[d].TocBuffer[j].number=CDROM_LEADOUT;
	D_S[d].TocBuffer[j].format=0;
	D_S[d].TocBuffer[j].address=D_S[d].size_msf;
	
	D_S[d].diskstate_flags |= toc_bit;
	return (0);
}
/*==========================================================================*/
static int DiskInfo(void)
{
	int i, j;
	
	D_S[d].mode=READ_M1;
	
#undef LOOP_COUNT
#define LOOP_COUNT 10 /* needed for some "old" drives */
	
	msg(DBG_000,"DiskInfo entered.\n");
	for (j=1;j<LOOP_COUNT;j++)
	{
#if 0
		i=SetSpeed();
		if (i<0)
		{
			msg(DBG_INF,"DiskInfo: SetSpeed returns %d\n", i);
			continue;
		}
		i=cc_ModeSense();
		if (i<0)
		{
			msg(DBG_INF,"DiskInfo: cc_ModeSense returns %d\n", i);
			continue;
		}
#endif
		i=cc_ReadCapacity();
		if (i>=0) break;
		msg(DBG_INF,"DiskInfo: ReadCapacity #%d returns %d\n", j, i);
#if 0
		i=cc_DriveReset();
#endif
		if (!fam0_drive && j == 2) break;
	}
	if (j==LOOP_COUNT) return (-33); /* give up */
	
	i=cc_ReadTocDescr();
	if (i<0)
	{
		msg(DBG_INF,"DiskInfo: ReadTocDescr returns %d\n", i);
		return (i);
	}
	i=ReadToC();
	if (i<0)
	{
		msg(DBG_INF,"DiskInfo: ReadToC returns %d\n", i);
		return (i);
	}
	i=cc_CheckMultiSession();
	if (i<0)
	{
		msg(DBG_INF,"DiskInfo: cc_CheckMultiSession returns %d\n", i);
		return (i);
	}
	if (D_S[d].f_multisession) D_S[d].sbp_bufsiz=1;  /* possibly a weird PhotoCD */
	else D_S[d].sbp_bufsiz=buffers;
	i=cc_ReadTocEntry(D_S[d].n_first_track);
	if (i<0)
	{
		msg(DBG_INF,"DiskInfo: cc_ReadTocEntry(1) returns %d\n", i);
		return (i);
	}
	i=cc_ReadUPC();
	if (i<0) msg(DBG_INF,"DiskInfo: cc_ReadUPC returns %d\n", i);
	if ((fam0L_drive) && (D_S[d].xa_byte==0x20 || D_S[d].xa_byte == 0x10))
	{
		/* XA disk with old drive */
		cc_ModeSelect(CD_FRAMESIZE_RAW1);
		cc_ModeSense();
	}
	if (famT_drive)	cc_prep_mode_T();
	msg(DBG_000,"DiskInfo done.\n");
	return (0);
}

static int sbpcd_drive_status(struct cdrom_device_info *cdi, int slot_nr)
{
	int st;

	if (CDSL_CURRENT != slot_nr) {
		 /* we have no changer support */
		 return -EINVAL;
	}

        cc_ReadStatus();
	st=ResponseStatus();
	if (st<0)
	{
		msg(DBG_INF,"sbpcd_drive_status: timeout.\n");
		return (0);
	}
	msg(DBG_000,"Drive Status: door_locked =%d.\n", st_door_locked);
	msg(DBG_000,"Drive Status: door_closed =%d.\n", st_door_closed);
	msg(DBG_000,"Drive Status: caddy_in =%d.\n", st_caddy_in);
	msg(DBG_000,"Drive Status: disk_ok =%d.\n", st_diskok);
	msg(DBG_000,"Drive Status: spinning =%d.\n", st_spinning);
	msg(DBG_000,"Drive Status: busy =%d.\n", st_busy);

#if 0
  if (!(D_S[MINOR(cdi->dev)].status_bits & p_door_closed)) return CDS_TRAY_OPEN;
  if (D_S[MINOR(cdi->dev)].status_bits & p_disk_ok) return CDS_DISC_OK;
  if (D_S[MINOR(cdi->dev)].status_bits & p_disk_in) return CDS_DRIVE_NOT_READY;

  return CDS_NO_DISC;
#else
  if (D_S[MINOR(cdi->dev)].status_bits & p_spinning) return CDS_DISC_OK;
/*  return CDS_TRAY_OPEN; */
  return CDS_NO_DISC;
  
#endif

}


/*==========================================================================*/
#if FUTURE
/*
 *  called always if driver gets entered
 *  returns 0 or ERROR2 or ERROR15
 */
static int prepare(u_char func, u_char subfunc)
{
	int i;
	
	if (fam0L_drive)
	{
		i=inb(CDi_status);
		if (i&s_attention) GetStatus();
	}
	else if (fam1_drive) GetStatus();
	else if (fam2_drive) GetStatus();
	else if (famT_drive) GetStatus();
	if (D_S[d].CD_changed==0xFF)
	{
		D_S[d].diskstate_flags=0;
		D_S[d].audio_state=0;
		if (!st_diskok)
		{
			i=check_allowed1(func,subfunc);
			if (i<0) return (-2);
		}
		else 
		{
			i=check_allowed3(func,subfunc);
			if (i<0)
			{
				D_S[d].CD_changed=1;
				return (-15);
			}
		}
	}
	else
	{
		if (!st_diskok)
		{
			D_S[d].diskstate_flags=0;
			D_S[d].audio_state=0;
			i=check_allowed1(func,subfunc);
			if (i<0) return (-2);
		}
		else
		{ 
			if (st_busy)
			{
				if (D_S[d].audio_state!=audio_pausing)
				{
					i=check_allowed2(func,subfunc);
					if (i<0) return (-2);
				}
			}
			else
			{
				if (D_S[d].audio_state==audio_playing) seek_pos_audio_end();
				D_S[d].audio_state=0;
			}
			if (!frame_size_valid)
			{
				i=DiskInfo();
				if (i<0)
				{
					D_S[d].diskstate_flags=0;
					D_S[d].audio_state=0;
					i=check_allowed1(func,subfunc);
					if (i<0) return (-2);
				}
			}
		}
    }
	return (0);
}
#endif FUTURE
/*==========================================================================*/
/*==========================================================================*/
/*
 * Check the results of the "get status" command.
 */
static int sbp_status(void)
{
	int st;
	
	st=ResponseStatus();
	if (st<0)
	{
		msg(DBG_INF,"sbp_status: timeout.\n");
		return (0);
	}
	
	if (!st_spinning) msg(DBG_SPI,"motor got off - ignoring.\n");
	
	if (st_check) 
	{
		msg(DBG_INF,"st_check detected - retrying.\n");
		return (0);
	}
	if (!st_door_closed)
	{
		msg(DBG_INF,"door is open - retrying.\n");
		return (0);
	}
	if (!st_caddy_in)
	{
		msg(DBG_INF,"disk removed - retrying.\n");
		return (0);
	}
	if (!st_diskok) 
	{
		msg(DBG_INF,"!st_diskok detected - retrying.\n");
		return (0);
	}
	if (st_busy) 
	{
		msg(DBG_INF,"st_busy detected - retrying.\n");
		return (0);
	}
	return (1);
}
/*==========================================================================*/
		
static int sbpcd_get_last_session(struct cdrom_device_info *cdi, struct cdrom_multisession *ms_infp)
{
	ms_infp->addr_format = CDROM_LBA;
	ms_infp->addr.lba    = D_S[MINOR(cdi->dev)].lba_multi;
	if (D_S[MINOR(cdi->dev)].f_multisession)
		ms_infp->xa_flag=1; /* valid redirection address */
	else
		ms_infp->xa_flag=0; /* invalid redirection address */

	return  0;
}

/*==========================================================================*/
/*==========================================================================*/
/*
 * ioctl support
 */
static int sbpcd_dev_ioctl(struct cdrom_device_info *cdi, u_int cmd,
		      u_long arg)
{
	int i;
	
	msg(DBG_IO2,"ioctl(%d, 0x%08lX, 0x%08lX)\n",
	    MINOR(cdi->dev), cmd, arg);
	i=MINOR(cdi->dev);
	if ((i<0) || (i>=NR_SBPCD) || (D_S[i].drv_id==-1))
	{
		msg(DBG_INF, "ioctl: bad device: %04X\n", cdi->dev);
		return (-ENXIO);             /* no such drive */
	}
	down(&ioctl_read_sem);
	if (d!=i) switch_drive(i);
	
	msg(DBG_IO2,"ioctl: device %d, request %04X\n",i,cmd);
	switch (cmd) 		/* Sun-compatible */
	{
	case DDIOCSDBG:		/* DDI Debug */
		if (!capable(CAP_SYS_ADMIN)) RETURN_UP(-EPERM);
		i=sbpcd_dbg_ioctl(arg,1);
		RETURN_UP(i);
	case CDROMRESET:      /* hard reset the drive */
		msg(DBG_IOC,"ioctl: CDROMRESET entered.\n");
		i=DriveReset();
		D_S[d].audio_state=0;
		RETURN_UP(i);
		
	case CDROMREADMODE1:
		msg(DBG_IOC,"ioctl: CDROMREADMODE1 requested.\n");
#if SAFE_MIXED
		if (D_S[d].has_data>1) RETURN_UP(-EBUSY);
#endif SAFE_MIXED
		cc_ModeSelect(CD_FRAMESIZE);
		cc_ModeSense();
		D_S[d].mode=READ_M1;
		RETURN_UP(0);
		
	case CDROMREADMODE2: /* not usable at the moment */
		msg(DBG_IOC,"ioctl: CDROMREADMODE2 requested.\n");
#if SAFE_MIXED
		if (D_S[d].has_data>1) RETURN_UP(-EBUSY);
#endif SAFE_MIXED
		cc_ModeSelect(CD_FRAMESIZE_RAW1);
		cc_ModeSense();
		D_S[d].mode=READ_M2;
		RETURN_UP(0);
		
	case CDROMAUDIOBUFSIZ: /* configure the audio buffer size */
		msg(DBG_IOC,"ioctl: CDROMAUDIOBUFSIZ entered.\n");
		if (D_S[d].sbp_audsiz>0) vfree(D_S[d].aud_buf);
		D_S[d].aud_buf=NULL;
		D_S[d].sbp_audsiz=arg;
		
		if (D_S[d].sbp_audsiz>0)
		{
			D_S[d].aud_buf=(u_char *) vmalloc(D_S[d].sbp_audsiz*CD_FRAMESIZE_RAW);
			if (D_S[d].aud_buf==NULL)
			{
				msg(DBG_INF,"audio buffer (%d frames) not available.\n",D_S[d].sbp_audsiz);
				D_S[d].sbp_audsiz=0;
			}
			else msg(DBG_INF,"audio buffer size: %d frames.\n",D_S[d].sbp_audsiz);
		}
		RETURN_UP(D_S[d].sbp_audsiz);

	case CDROMREADAUDIO:
	{ /* start of CDROMREADAUDIO */
		int i=0, j=0, frame, block=0;
		u_int try=0;
		u_long timeout;
		u_char *p;
		u_int data_tries = 0;
		u_int data_waits = 0;
		u_int data_retrying = 0;
		int status_tries;
		int error_flag;
		
		msg(DBG_IOC,"ioctl: CDROMREADAUDIO entered.\n");
		if (fam0_drive) RETURN_UP(-EINVAL);
		if (famL_drive) RETURN_UP(-EINVAL);
		if (famV_drive) RETURN_UP(-EINVAL);
		if (famT_drive) RETURN_UP(-EINVAL);
#if SAFE_MIXED
		if (D_S[d].has_data>1) RETURN_UP(-EBUSY);
#endif SAFE_MIXED
		if (D_S[d].aud_buf==NULL) RETURN_UP(-EINVAL);
		i=verify_area(VERIFY_READ, (void *) arg, sizeof(struct cdrom_read_audio));
		if (i) RETURN_UP(i);
		copy_from_user(&read_audio, (void *) arg, sizeof(struct cdrom_read_audio));
		if (read_audio.nframes>D_S[d].sbp_audsiz) RETURN_UP(-EINVAL);
		i=verify_area(VERIFY_WRITE, read_audio.buf,
			      read_audio.nframes*CD_FRAMESIZE_RAW);
		if (i) RETURN_UP(i);
		
		if (read_audio.addr_format==CDROM_MSF) /* MSF-bin specification of where to start */
			block=msf2lba(&read_audio.addr.msf.minute);
		else if (read_audio.addr_format==CDROM_LBA) /* lba specification of where to start */
			block=read_audio.addr.lba;
		else RETURN_UP(-EINVAL);
#if 000
		i=cc_SetSpeed(speed_150,0,0);
		if (i) msg(DBG_AUD,"read_audio: SetSpeed error %d\n", i);
#endif
		msg(DBG_AUD,"read_audio: lba: %d, msf: %06X\n",
		    block, blk2msf(block));
		msg(DBG_AUD,"read_audio: before cc_ReadStatus.\n");
#if OLD_BUSY
		while (busy_data) sbp_sleep(HZ/10); /* wait a bit */
		busy_audio=1;
#endif OLD_BUSY
		error_flag=0;
		for (data_tries=5; data_tries>0; data_tries--)
		{
			msg(DBG_AUD,"data_tries=%d ...\n", data_tries);
			D_S[d].mode=READ_AU;
			cc_ModeSelect(CD_FRAMESIZE_RAW);
			cc_ModeSense();
			for (status_tries=3; status_tries > 0; status_tries--)
			{
				flags_cmd_out |= f_respo3;
				cc_ReadStatus();
				if (sbp_status() != 0) break;
				if (st_check) cc_ReadError();
				sbp_sleep(1);    /* wait a bit, try again */
			}
			if (status_tries == 0)
			{
				msg(DBG_AUD,"read_audio: sbp_status: failed after 3 tries in line %d.\n", __LINE__);
				continue;
			}
			msg(DBG_AUD,"read_audio: sbp_status: ok.\n");
			
			flags_cmd_out = f_putcmd | f_respo2 | f_ResponseStatus | f_obey_p_check;
			if (fam0L_drive)
			{
				flags_cmd_out |= f_lopsta | f_getsta | f_bit1;
				cmd_type=READ_M2;
				drvcmd[0]=CMD0_READ_XA; /* "read XA frames", old drives */
				drvcmd[1]=(block>>16)&0x000000ff;
				drvcmd[2]=(block>>8)&0x000000ff;
				drvcmd[3]=block&0x000000ff;
				drvcmd[4]=0;
				drvcmd[5]=read_audio.nframes; /* # of frames */
				drvcmd[6]=0;
			}
			else if (fam1_drive)
			{
				drvcmd[0]=CMD1_READ; /* "read frames", new drives */
				lba2msf(block,&drvcmd[1]); /* msf-bin format required */
				drvcmd[4]=0;
				drvcmd[5]=0;
				drvcmd[6]=read_audio.nframes; /* # of frames */
			}
			else if (fam2_drive)
			{
				drvcmd[0]=CMD2_READ_XA2;
				lba2msf(block,&drvcmd[1]); /* msf-bin format required */
				drvcmd[4]=0;
				drvcmd[5]=read_audio.nframes; /* # of frames */
				drvcmd[6]=0x11; /* raw mode */
			}
			else if (famT_drive) /* CD-55A: not tested yet */
			{
			}
			msg(DBG_AUD,"read_audio: before giving \"read\" command.\n");
			flags_cmd_out=f_putcmd;
			response_count=0;
			i=cmd_out();
			if (i<0) msg(DBG_INF,"error giving READ AUDIO command: %0d\n", i);
			sbp_sleep(0);
			msg(DBG_AUD,"read_audio: after giving \"read\" command.\n");
			for (frame=1;frame<2 && !error_flag; frame++)
			{
				try=maxtim_data;
				for (timeout=jiffies+9*HZ; ; )
				{
					for ( ; try!=0;try--)
					{
						j=inb(CDi_status);
						if (!(j&s_not_data_ready)) break;
						if (!(j&s_not_result_ready)) break;
						if (fam0L_drive) if (j&s_attention) break;
					}
					if (try != 0 || time_after_eq(jiffies, timeout)) break;
					if (data_retrying == 0) data_waits++;
					data_retrying = 1;
					sbp_sleep(1);
					try = 1;
				}
				if (try==0)
				{
					msg(DBG_INF,"read_audio: sbp_data: CDi_status timeout.\n");
					error_flag++;
					break;
				}
				msg(DBG_AUD,"read_audio: sbp_data: CDi_status ok.\n");
				if (j&s_not_data_ready)
				{
					msg(DBG_INF, "read_audio: sbp_data: DATA_READY timeout.\n");
					error_flag++;
					break;
				}
				msg(DBG_AUD,"read_audio: before reading data.\n");
				error_flag=0;
				p = D_S[d].aud_buf;
				if (sbpro_type==1) OUT(CDo_sel_i_d,1);
				if (do_16bit)
				{
					u_short *p2 = (u_short *) p;

					for (; (u_char *) p2 < D_S[d].aud_buf + read_audio.nframes*CD_FRAMESIZE_RAW;)
				  	{
						if ((inb_p(CDi_status)&s_not_data_ready)) continue;

						/* get one sample */
						*p2++ = inw_p(CDi_data);
						*p2++ = inw_p(CDi_data);
					}
				} else {
					for (; p < D_S[d].aud_buf + read_audio.nframes*CD_FRAMESIZE_RAW;)
				  	{
						if ((inb_p(CDi_status)&s_not_data_ready)) continue;

						/* get one sample */
						*p++ = inb_p(CDi_data);
						*p++ = inb_p(CDi_data);
						*p++ = inb_p(CDi_data);
						*p++ = inb_p(CDi_data);
					}
				}
				if (sbpro_type==1) OUT(CDo_sel_i_d,0);
				data_retrying = 0;
			}
			msg(DBG_AUD,"read_audio: after reading data.\n");
			if (error_flag)    /* must have been spurious D_RDY or (ATTN&&!D_RDY) */
			{
				msg(DBG_AUD,"read_audio: read aborted by drive\n");
#if 0000
				i=cc_DriveReset();                /* ugly fix to prevent a hang */
#else
				i=cc_ReadError();
#endif 0000
				continue;
			}
			if (fam0L_drive)
			{
				i=maxtim_data;
				for (timeout=jiffies+9*HZ; time_before(jiffies, timeout); timeout--)
				{
					for ( ;i!=0;i--)
					{
						j=inb(CDi_status);
						if (!(j&s_not_data_ready)) break;
						if (!(j&s_not_result_ready)) break;
						if (j&s_attention) break;
					}
					if (i != 0 || time_after_eq(jiffies, timeout)) break;
					sbp_sleep(0);
					i = 1;
				}
				if (i==0) msg(DBG_AUD,"read_audio: STATUS TIMEOUT AFTER READ");
				if (!(j&s_attention))
				{
					msg(DBG_AUD,"read_audio: sbp_data: timeout waiting DRV_ATTN - retrying\n");
					i=cc_DriveReset();  /* ugly fix to prevent a hang */
					continue;
				}
			}
			do
			{
				if (fam0L_drive) cc_ReadStatus();
				i=ResponseStatus();  /* builds status_bits, returns orig. status (old) or faked p_success (new) */
				if (i<0) { msg(DBG_AUD,
					       "read_audio: cc_ReadStatus error after read: %02X\n",
					       D_S[d].status_bits);
					   continue; /* FIXME */
				   }
			}
			while ((fam0L_drive)&&(!st_check)&&(!(i&p_success)));
			if (st_check)
			{
				i=cc_ReadError();
				msg(DBG_AUD,"read_audio: cc_ReadError was necessary after read: %02X\n",i);
				continue;
			}
			copy_to_user((u_char *) read_audio.buf,
				    (u_char *) D_S[d].aud_buf,
				    read_audio.nframes*CD_FRAMESIZE_RAW);
			msg(DBG_AUD,"read_audio: copy_to_user done.\n");
			break;
		}
		cc_ModeSelect(CD_FRAMESIZE);
		cc_ModeSense();
		D_S[d].mode=READ_M1;
#if OLD_BUSY
		busy_audio=0;
#endif OLD_BUSY
		if (data_tries == 0)
		{
			msg(DBG_AUD,"read_audio: failed after 5 tries in line %d.\n", __LINE__);
			RETURN_UP(-EIO);
		}
		msg(DBG_AUD,"read_audio: successful return.\n");
		RETURN_UP(0);
	} /* end of CDROMREADAUDIO */
		
	case BLKRASET:
		if(!capable(CAP_SYS_ADMIN)) RETURN_UP(-EACCES);
		if(!(cdi->dev)) RETURN_UP(-EINVAL);
		if(arg > 0xff) RETURN_UP(-EINVAL);
		read_ahead[MAJOR(cdi->dev)] = arg;
		RETURN_UP(0);
	default:
		msg(DBG_IOC,"ioctl: unknown function request %04X\n", cmd);
		RETURN_UP(-EINVAL);
	} /* end switch(cmd) */
}

static int sbpcd_audio_ioctl(struct cdrom_device_info *cdi, u_int cmd,
		       void * arg)
{
	int i, st, j;
	
	msg(DBG_IO2,"ioctl(%d, 0x%08lX, 0x%08p)\n",
	    MINOR(cdi->dev), cmd, arg);
	i=MINOR(cdi->dev);
	if ((i<0) || (i>=NR_SBPCD) || (D_S[i].drv_id==-1))
	{
		msg(DBG_INF, "ioctl: bad device: %04X\n", cdi->dev);
		return (-ENXIO);             /* no such drive */
	}
	down(&ioctl_read_sem);
	if (d!=i) switch_drive(i);
	
	msg(DBG_IO2,"ioctl: device %d, request %04X\n",i,cmd);
	switch (cmd) 		/* Sun-compatible */
	{
		
	case CDROMPAUSE:     /* Pause the drive */
		msg(DBG_IOC,"ioctl: CDROMPAUSE entered.\n");
		/* pause the drive unit when it is currently in PLAY mode,         */
		/* or reset the starting and ending locations when in PAUSED mode. */
		/* If applicable, at the next stopping point it reaches            */
		/* the drive will discontinue playing.                             */
		switch (D_S[d].audio_state)
		{
		case audio_playing:
			if (famL_drive) i=cc_ReadSubQ();
			else i=cc_Pause_Resume(1);
			if (i<0) RETURN_UP(-EIO);
			if (famL_drive) i=cc_Pause_Resume(1);
			else i=cc_ReadSubQ();
			if (i<0) RETURN_UP(-EIO);
			D_S[d].pos_audio_start=D_S[d].SubQ_run_tot;
			D_S[d].audio_state=audio_pausing;
			RETURN_UP(0);
		case audio_pausing:
			i=cc_Seek(D_S[d].pos_audio_start,1);
			if (i<0) RETURN_UP(-EIO);
			RETURN_UP(0);
		default:
			RETURN_UP(-EINVAL);
		}
		
	case CDROMRESUME: /* resume paused audio play */
		msg(DBG_IOC,"ioctl: CDROMRESUME entered.\n");
		/* resume playing audio tracks when a previous PLAY AUDIO call has  */
		/* been paused with a PAUSE command.                                */
		/* It will resume playing from the location saved in SubQ_run_tot.  */
		if (D_S[d].audio_state!=audio_pausing) RETURN_UP(-EINVAL);
		if (famL_drive)
			i=cc_PlayAudio(D_S[d].pos_audio_start,
				       D_S[d].pos_audio_end);
		else i=cc_Pause_Resume(3);
		if (i<0) RETURN_UP(-EIO);
		D_S[d].audio_state=audio_playing;
		RETURN_UP(0);
		
	case CDROMPLAYMSF:
		msg(DBG_IOC,"ioctl: CDROMPLAYMSF entered.\n");
#if SAFE_MIXED
		if (D_S[d].has_data>1) RETURN_UP(-EBUSY);
#endif SAFE_MIXED
		if (D_S[d].audio_state==audio_playing)
		{
			i=cc_Pause_Resume(1);
			if (i<0) RETURN_UP(-EIO);
			i=cc_ReadSubQ();
			if (i<0) RETURN_UP(-EIO);
			D_S[d].pos_audio_start=D_S[d].SubQ_run_tot;
			i=cc_Seek(D_S[d].pos_audio_start,1);
		}
		memcpy(&msf, (void *) arg, sizeof(struct cdrom_msf));
		/* values come as msf-bin */
		D_S[d].pos_audio_start = (msf.cdmsf_min0<<16) |
                        (msf.cdmsf_sec0<<8) |
				msf.cdmsf_frame0;
		D_S[d].pos_audio_end = (msf.cdmsf_min1<<16) |
			(msf.cdmsf_sec1<<8) |
				msf.cdmsf_frame1;
		msg(DBG_IOX,"ioctl: CDROMPLAYMSF %08X %08X\n",
		    D_S[d].pos_audio_start,D_S[d].pos_audio_end);
		i=cc_PlayAudio(D_S[d].pos_audio_start,D_S[d].pos_audio_end);
		if (i<0)
		{
			msg(DBG_INF,"ioctl: cc_PlayAudio returns %d\n",i);
			DriveReset();
			D_S[d].audio_state=0;
			RETURN_UP(-EIO);
		}
		D_S[d].audio_state=audio_playing;
		RETURN_UP(0);
		
	case CDROMPLAYTRKIND: /* Play a track.  This currently ignores index. */
		msg(DBG_IOC,"ioctl: CDROMPLAYTRKIND entered.\n");
#if SAFE_MIXED
		if (D_S[d].has_data>1) RETURN_UP(-EBUSY);
#endif SAFE_MIXED
		if (D_S[d].audio_state==audio_playing)
		{
			msg(DBG_IOX,"CDROMPLAYTRKIND: already audio_playing.\n");
#if 1
			RETURN_UP(0); /* just let us play on */
#else
			RETURN_UP(-EINVAL); /* play on, but say "error" */
#endif
		}
		memcpy(&ti,(void *) arg,sizeof(struct cdrom_ti));
		msg(DBG_IOX,"ioctl: trk0: %d, ind0: %d, trk1:%d, ind1:%d\n",
		    ti.cdti_trk0,ti.cdti_ind0,ti.cdti_trk1,ti.cdti_ind1);
		if (ti.cdti_trk0<D_S[d].n_first_track) RETURN_UP(-EINVAL);
		if (ti.cdti_trk0>D_S[d].n_last_track) RETURN_UP(-EINVAL);
		if (ti.cdti_trk1<ti.cdti_trk0) ti.cdti_trk1=ti.cdti_trk0;
		if (ti.cdti_trk1>D_S[d].n_last_track) ti.cdti_trk1=D_S[d].n_last_track;
		D_S[d].pos_audio_start=D_S[d].TocBuffer[ti.cdti_trk0].address;
		D_S[d].pos_audio_end=D_S[d].TocBuffer[ti.cdti_trk1+1].address;
		i=cc_PlayAudio(D_S[d].pos_audio_start,D_S[d].pos_audio_end);
		if (i<0)
		{
			msg(DBG_INF,"ioctl: cc_PlayAudio returns %d\n",i);
			DriveReset();
			D_S[d].audio_state=0;
			RETURN_UP(-EIO);
		}
		D_S[d].audio_state=audio_playing;
		RETURN_UP(0);
		
	case CDROMREADTOCHDR:        /* Read the table of contents header */
		msg(DBG_IOC,"ioctl: CDROMREADTOCHDR entered.\n");
		tochdr.cdth_trk0=D_S[d].n_first_track;
		tochdr.cdth_trk1=D_S[d].n_last_track;
		memcpy((void *) arg, &tochdr, sizeof(struct cdrom_tochdr));
		RETURN_UP(0);
		
	case CDROMREADTOCENTRY:      /* Read an entry in the table of contents */
		msg(DBG_IOC,"ioctl: CDROMREADTOCENTRY entered.\n");
		memcpy(&tocentry, (void *) arg, sizeof(struct cdrom_tocentry));
		i=tocentry.cdte_track;
		if (i==CDROM_LEADOUT) i=D_S[d].n_last_track+1;
		else if (i<D_S[d].n_first_track||i>D_S[d].n_last_track)
                  RETURN_UP(-EINVAL);
		tocentry.cdte_adr=D_S[d].TocBuffer[i].ctl_adr&0x0F;
		tocentry.cdte_ctrl=(D_S[d].TocBuffer[i].ctl_adr>>4)&0x0F;
		tocentry.cdte_datamode=D_S[d].TocBuffer[i].format;
		if (tocentry.cdte_format==CDROM_MSF) /* MSF-bin required */
		{
			tocentry.cdte_addr.msf.minute=(D_S[d].TocBuffer[i].address>>16)&0x00FF;
			tocentry.cdte_addr.msf.second=(D_S[d].TocBuffer[i].address>>8)&0x00FF;
			tocentry.cdte_addr.msf.frame=D_S[d].TocBuffer[i].address&0x00FF;
		}
		else if (tocentry.cdte_format==CDROM_LBA) /* blk required */
			tocentry.cdte_addr.lba=msf2blk(D_S[d].TocBuffer[i].address);
		else RETURN_UP(-EINVAL);
		memcpy((void *) arg, &tocentry, sizeof(struct cdrom_tocentry));
		RETURN_UP(0);
		
	case CDROMSTOP:      /* Spin down the drive */
		msg(DBG_IOC,"ioctl: CDROMSTOP entered.\n");
#if SAFE_MIXED
		if (D_S[d].has_data>1) RETURN_UP(-EBUSY);
#endif SAFE_MIXED
		i=cc_Pause_Resume(1);
		D_S[d].audio_state=0;
#if 0
		cc_DriveReset();
#endif
		RETURN_UP(i);
		
	case CDROMSTART:  /* Spin up the drive */
		msg(DBG_IOC,"ioctl: CDROMSTART entered.\n");
		cc_SpinUp();
		D_S[d].audio_state=0;
		RETURN_UP(0);
		
	case CDROMVOLCTRL:   /* Volume control */
		msg(DBG_IOC,"ioctl: CDROMVOLCTRL entered.\n");
		memcpy(&volctrl,(char *) arg,sizeof(volctrl));
		D_S[d].vol_chan0=0;
		D_S[d].vol_ctrl0=volctrl.channel0;
		D_S[d].vol_chan1=1;
		D_S[d].vol_ctrl1=volctrl.channel1;
		i=cc_SetVolume();
		RETURN_UP(0);
		
	case CDROMVOLREAD:   /* read Volume settings from drive */
		msg(DBG_IOC,"ioctl: CDROMVOLREAD entered.\n");
		st=cc_GetVolume();
		if (st<0) RETURN_UP(st);
		volctrl.channel0=D_S[d].vol_ctrl0;
		volctrl.channel1=D_S[d].vol_ctrl1;
		volctrl.channel2=0;
		volctrl.channel2=0;
		memcpy((void *)arg,&volctrl,sizeof(volctrl));
		RETURN_UP(0);

	case CDROMSUBCHNL:   /* Get subchannel info */
		msg(DBG_IOS,"ioctl: CDROMSUBCHNL entered.\n");
		/* Bogus, I can do better than this! --AJK
		if ((st_spinning)||(!subq_valid)) {
			i=cc_ReadSubQ();
			if (i<0) RETURN_UP(-EIO);
		}
		*/
		i=cc_ReadSubQ();
		if (i<0) {
			j=cc_ReadError(); /* clear out error status from drive */
			D_S[d].audio_state=CDROM_AUDIO_NO_STATUS;
			/* get and set the disk state here, 
			probably not the right place, but who cares!
			It makes it work properly! --AJK */
			if (D_S[d].CD_changed==0xFF) {
				msg(DBG_000,"Disk changed detect\n");
				D_S[d].diskstate_flags &= ~cd_size_bit;
			}
			RETURN_UP(-EIO);
		}
		if (D_S[d].CD_changed==0xFF) {
			/* reread the TOC because the disk has changed! --AJK */
			msg(DBG_000,"Disk changed STILL detected, rereading TOC!\n");
			i=DiskInfo();
			if(i==0) {
				D_S[d].CD_changed=0x00; /* cd has changed, procede, */
				RETURN_UP(-EIO); /* and get TOC, etc on next try! --AJK */
			} else {
				RETURN_UP(-EIO); /* we weren't ready yet! --AJK */
			}
		}
		memcpy(&SC, (void *) arg, sizeof(struct cdrom_subchnl));
		/* 
			This virtual crap is very bogus! 
			It doesn't detect when the cd is done playing audio!
			Lets do this right with proper hardware register reading!
		*/
		cc_ReadStatus();
		i=ResponseStatus();
		msg(DBG_000,"Drive Status: door_locked =%d.\n", st_door_locked);
		msg(DBG_000,"Drive Status: door_closed =%d.\n", st_door_closed);
		msg(DBG_000,"Drive Status: caddy_in =%d.\n", st_caddy_in);
		msg(DBG_000,"Drive Status: disk_ok =%d.\n", st_diskok);
		msg(DBG_000,"Drive Status: spinning =%d.\n", st_spinning);
		msg(DBG_000,"Drive Status: busy =%d.\n", st_busy);
		/* st_busy indicates if it's _ACTUALLY_ playing audio */
		switch (D_S[d].audio_state)
		{
		case audio_playing:
			if(st_busy==0) {
				/* CD has stopped playing audio --AJK */
				D_S[d].audio_state=audio_completed;
				SC.cdsc_audiostatus=CDROM_AUDIO_COMPLETED;
			} else {
				SC.cdsc_audiostatus=CDROM_AUDIO_PLAY;
			}
			break;
		case audio_pausing:
			SC.cdsc_audiostatus=CDROM_AUDIO_PAUSED;
			break;
		case audio_completed:
			SC.cdsc_audiostatus=CDROM_AUDIO_COMPLETED;
			break;
		default:
			SC.cdsc_audiostatus=CDROM_AUDIO_NO_STATUS;
			break;
		}
		SC.cdsc_adr=D_S[d].SubQ_ctl_adr;
		SC.cdsc_ctrl=D_S[d].SubQ_ctl_adr>>4;
		SC.cdsc_trk=bcd2bin(D_S[d].SubQ_trk);
		SC.cdsc_ind=bcd2bin(D_S[d].SubQ_pnt_idx);
		if (SC.cdsc_format==CDROM_LBA)
		{
			SC.cdsc_absaddr.lba=msf2blk(D_S[d].SubQ_run_tot);
			SC.cdsc_reladdr.lba=msf2blk(D_S[d].SubQ_run_trk);
		}
		else /* not only if (SC.cdsc_format==CDROM_MSF) */
		{
			SC.cdsc_absaddr.msf.minute=(D_S[d].SubQ_run_tot>>16)&0x00FF;
			SC.cdsc_absaddr.msf.second=(D_S[d].SubQ_run_tot>>8)&0x00FF;
			SC.cdsc_absaddr.msf.frame=D_S[d].SubQ_run_tot&0x00FF;
			SC.cdsc_reladdr.msf.minute=(D_S[d].SubQ_run_trk>>16)&0x00FF;
			SC.cdsc_reladdr.msf.second=(D_S[d].SubQ_run_trk>>8)&0x00FF;
			SC.cdsc_reladdr.msf.frame=D_S[d].SubQ_run_trk&0x00FF;
		}
		memcpy((void *) arg, &SC, sizeof(struct cdrom_subchnl));
		msg(DBG_IOS,"CDROMSUBCHNL: %1X %02X %08X %08X %02X %02X %06X %06X\n",
		    SC.cdsc_format,SC.cdsc_audiostatus,
		    SC.cdsc_adr,SC.cdsc_ctrl,
		    SC.cdsc_trk,SC.cdsc_ind,
		    SC.cdsc_absaddr,SC.cdsc_reladdr);
		RETURN_UP(0);
		
	default:
		msg(DBG_IOC,"ioctl: unknown function request %04X\n", cmd);
		RETURN_UP(-EINVAL);
	} /* end switch(cmd) */
}
/*==========================================================================*/
/*
 *  Take care of the different block sizes between cdrom and Linux.
 */
static void sbp_transfer(struct request *req)
{
	long offs;
	
	while ( (req->nr_sectors > 0) &&
	       (req->sector/4 >= D_S[d].sbp_first_frame) &&
	       (req->sector/4 <= D_S[d].sbp_last_frame) )
	{
		offs = (req->sector - D_S[d].sbp_first_frame * 4) * 512;
		memcpy(req->buffer, D_S[d].sbp_buf + offs, 512);
		req->nr_sectors--;
		req->sector++;
		req->buffer += 512;
	}
}
/*==========================================================================*/
/*
 *  special end_request for sbpcd to solve CURRENT==NULL bug. (GTL)
 *  GTL = Gonzalo Tornaria <tornaria@cmat.edu.uy>
 *
 *  This is a kludge so we don't need to modify end_request.
 *  We put the req we take out after INIT_REQUEST in the requests list,
 *  so that end_request will discard it. 
 *
 *  The bug could be present in other block devices, perhaps we
 *  should modify INIT_REQUEST and end_request instead, and
 *  change every block device.. 
 *
 *  Could be a race here?? Could e.g. a timer interrupt schedule() us?
 *  If so, we should copy end_request here, and do it right.. (or
 *  modify end_request and the block devices).
 *
 *  In any case, the race here would be much small than it was, and
 *  I couldn't reproduce..
 *
 *  The race could be: suppose CURRENT==NULL. We put our req in the list,
 *  and we are scheduled. Other process takes over, and gets into
 *  do_sbpcd_request. It sees CURRENT!=NULL (it is == to our req), so
 *  proceeds. It ends, so CURRENT is now NULL.. Now we awake somewhere in
 *  end_request, but now CURRENT==NULL... oops!
 *
 */
#undef DEBUG_GTL
static inline void sbpcd_end_request(struct request *req, int uptodate) {
	list_add(&req->queue, &req->q->queue_head);
	end_request(uptodate);
}
/*==========================================================================*/
/*
 *  I/O request routine, called from Linux kernel.
 */
static void DO_SBPCD_REQUEST(request_queue_t * q)
{
	u_int block;
	u_int nsect;
	int i, status_tries, data_tries;
	struct request *req;
#ifdef DEBUG_GTL
	static int xx_nr=0;
	int xnr;
#endif

 request_loop:
#ifdef DEBUG_GTL
	xnr=++xx_nr;

	if(QUEUE_EMPTY)
	{
		printk( "do_sbpcd_request[%di](NULL), Pid:%d, Time:%li\n",
			xnr, current->pid, jiffies);
		printk( "do_sbpcd_request[%do](NULL) end 0 (null), Time:%li\n",
			xnr, jiffies);
		CLEAR_INTR;
		return;
	}

	printk(" do_sbpcd_request[%di](%p:%ld+%ld), Pid:%d, Time:%li\n",
		xnr, CURRENT, CURRENT->sector, CURRENT->nr_sectors, current->pid, jiffies);
#endif
	INIT_REQUEST;
	req=CURRENT;		/* take out our request so no other */
	blkdev_dequeue_request(req);	/* task can fuck it up         GTL  */
	
	if (req->rq_status == RQ_INACTIVE)
		sbpcd_end_request(req, 0);
	if (req -> sector == -1)
		sbpcd_end_request(req, 0);
	spin_unlock_irq(&io_request_lock);

	down(&ioctl_read_sem);
	if (req->cmd != READ)
	{
		msg(DBG_INF, "bad cmd %d\n", req->cmd);
		goto err_done;
	}
	i = MINOR(req->rq_dev);
	if ( (i<0) || (i>=NR_SBPCD) || (D_S[i].drv_id==-1))
	{
		msg(DBG_INF, "do_request: bad device: %s\n",
			kdevname(req->rq_dev));
		goto err_done;
	}
#if OLD_BUSY
	while (busy_audio) sbp_sleep(HZ); /* wait a bit */
	busy_data=1;
#endif OLD_BUSY
	
	if (D_S[i].audio_state==audio_playing) goto err_done;
	if (d!=i) switch_drive(i);
	
	block = req->sector; /* always numbered as 512-byte-pieces */
	nsect = req->nr_sectors; /* always counted as 512-byte-pieces */
	
	msg(DBG_BSZ,"read sector %d (%d sectors)\n", block, nsect);
#if 0
	msg(DBG_MUL,"read LBA %d\n", block/4);
#endif
	
	sbp_transfer(req);
	/* if we satisfied the request from the buffer, we're done. */
	if (req->nr_sectors == 0)
	{
#ifdef DEBUG_GTL
		printk(" do_sbpcd_request[%do](%p:%ld+%ld) end 2, Time:%li\n",
			xnr, req, req->sector, req->nr_sectors, jiffies);
#endif
		up(&ioctl_read_sem);
		spin_lock_irq(&io_request_lock);
		sbpcd_end_request(req, 1);
		goto request_loop;
	}

#if FUTURE
	i=prepare(0,0); /* at moment not really a hassle check, but ... */
	if (i!=0)
		msg(DBG_INF,"\"prepare\" tells error %d -- ignored\n", i);
#endif FUTURE
	
	if (!st_spinning) cc_SpinUp();
	
	for (data_tries=n_retries; data_tries > 0; data_tries--)
	{
		for (status_tries=3; status_tries > 0; status_tries--)
		{
			flags_cmd_out |= f_respo3;
			cc_ReadStatus();
			if (sbp_status() != 0) break;
			if (st_check) cc_ReadError();
			sbp_sleep(1);    /* wait a bit, try again */
		}
		if (status_tries == 0)
		{
			msg(DBG_INF,"sbp_status: failed after 3 tries in line %d\n", __LINE__);
			break;
		}
		
		sbp_read_cmd(req);
		sbp_sleep(0);
		if (sbp_data(req) != 0)
		{
#if SAFE_MIXED
			D_S[d].has_data=2; /* is really a data disk */
#endif SAFE_MIXED
#ifdef DEBUG_GTL
			printk(" do_sbpcd_request[%do](%p:%ld+%ld) end 3, Time:%li\n",
				xnr, req, req->sector, req->nr_sectors, jiffies);
#endif
			up(&ioctl_read_sem);
			spin_lock_irq(&io_request_lock);
			sbpcd_end_request(req, 1);
			goto request_loop;
		}
	}
	
 err_done:
#if OLD_BUSY
	busy_data=0;
#endif OLD_BUSY
#ifdef DEBUG_GTL
	printk(" do_sbpcd_request[%do](%p:%ld+%ld) end 4 (error), Time:%li\n",
		xnr, req, req->sector, req->nr_sectors, jiffies);
#endif
	up(&ioctl_read_sem);
	sbp_sleep(0);    /* wait a bit, try again */
	spin_lock_irq(&io_request_lock);
	sbpcd_end_request(req, 0);
	goto request_loop;
}
/*==========================================================================*/
/*
 *  build and send the READ command.
 */
static void sbp_read_cmd(struct request *req)
{
#undef OLD

	int i;
	int block;
	
	D_S[d].sbp_first_frame=D_S[d].sbp_last_frame=-1;      /* purge buffer */
	D_S[d].sbp_current = 0;
	block=req->sector/4;
	if (block+D_S[d].sbp_bufsiz <= D_S[d].CDsize_frm)
		D_S[d].sbp_read_frames = D_S[d].sbp_bufsiz;
	else
	{
		D_S[d].sbp_read_frames=D_S[d].CDsize_frm-block;
		/* avoid reading past end of data */
		if (D_S[d].sbp_read_frames < 1)
		{
			msg(DBG_INF,"requested frame %d, CD size %d ???\n",
			    block, D_S[d].CDsize_frm);
			D_S[d].sbp_read_frames=1;
		}
	}
	
	flags_cmd_out = f_putcmd | f_respo2 | f_ResponseStatus | f_obey_p_check;
	clr_cmdbuf();
	if (famV_drive)
	  {
	    drvcmd[0]=CMDV_READ;
	    lba2msf(block,&drvcmd[1]); /* msf-bcd format required */
	    bin2bcdx(&drvcmd[1]);
	    bin2bcdx(&drvcmd[2]);
	    bin2bcdx(&drvcmd[3]);
	    drvcmd[4]=D_S[d].sbp_read_frames>>8;
	    drvcmd[5]=D_S[d].sbp_read_frames&0xff;
	    drvcmd[6]=0x02; /* flag "msf-bcd" */
	}
	else if (fam0L_drive)
	{
		flags_cmd_out |= f_lopsta | f_getsta | f_bit1;
		if (D_S[d].xa_byte==0x20)
		{
			cmd_type=READ_M2;
			drvcmd[0]=CMD0_READ_XA; /* "read XA frames", old drives */
			drvcmd[1]=(block>>16)&0x0ff;
			drvcmd[2]=(block>>8)&0x0ff;
			drvcmd[3]=block&0x0ff;
			drvcmd[4]=(D_S[d].sbp_read_frames>>8)&0x0ff;
			drvcmd[5]=D_S[d].sbp_read_frames&0x0ff;
		}
		else
		{
			drvcmd[0]=CMD0_READ; /* "read frames", old drives */
			if (D_S[d].drv_type>=drv_201)
			{
				lba2msf(block,&drvcmd[1]); /* msf-bcd format required */
				bin2bcdx(&drvcmd[1]);
				bin2bcdx(&drvcmd[2]);
				bin2bcdx(&drvcmd[3]);
			}
			else
			{
				drvcmd[1]=(block>>16)&0x0ff;
				drvcmd[2]=(block>>8)&0x0ff;
				drvcmd[3]=block&0x0ff;
			}
			drvcmd[4]=(D_S[d].sbp_read_frames>>8)&0x0ff;
			drvcmd[5]=D_S[d].sbp_read_frames&0x0ff;
			drvcmd[6]=(D_S[d].drv_type<drv_201)?0:2; /* flag "lba or msf-bcd format" */
		}
	}
	else if (fam1_drive)
	{
		drvcmd[0]=CMD1_READ;
		lba2msf(block,&drvcmd[1]); /* msf-bin format required */
		drvcmd[5]=(D_S[d].sbp_read_frames>>8)&0x0ff;
		drvcmd[6]=D_S[d].sbp_read_frames&0x0ff;
	}
	else if (fam2_drive)
	{
		drvcmd[0]=CMD2_READ;
		lba2msf(block,&drvcmd[1]); /* msf-bin format required */
		drvcmd[4]=(D_S[d].sbp_read_frames>>8)&0x0ff;
		drvcmd[5]=D_S[d].sbp_read_frames&0x0ff;
		drvcmd[6]=0x02;
	}
	else if (famT_drive)
	{
		drvcmd[0]=CMDT_READ;
		drvcmd[2]=(block>>24)&0x0ff;
		drvcmd[3]=(block>>16)&0x0ff;
		drvcmd[4]=(block>>8)&0x0ff;
		drvcmd[5]=block&0x0ff;
		drvcmd[7]=(D_S[d].sbp_read_frames>>8)&0x0ff;
		drvcmd[8]=D_S[d].sbp_read_frames&0x0ff;
	}
	flags_cmd_out=f_putcmd;
	response_count=0;
	i=cmd_out();
	if (i<0) msg(DBG_INF,"error giving READ command: %0d\n", i);
	return;
}
/*==========================================================================*/
/*
 *  Check the completion of the read-data command.  On success, read
 *  the D_S[d].sbp_bufsiz * 2048 bytes of data from the disk into buffer.
 */
static int sbp_data(struct request *req)
{
	int i=0, j=0, l, frame;
	u_int try=0;
	u_long timeout;
	u_char *p;
	u_int data_tries = 0;
	u_int data_waits = 0;
	u_int data_retrying = 0;
	int error_flag;
	int xa_count;
	int max_latency;
	int success;
	int wait;
	int duration;
	
	error_flag=0;
	success=0;
#if LONG_TIMING
	max_latency=9*HZ;
#else
	if (D_S[d].f_multisession) max_latency=15*HZ;
	else max_latency=5*HZ;
#endif
	duration=jiffies;
	for (frame=0;frame<D_S[d].sbp_read_frames&&!error_flag; frame++)
	{
		SBPCD_CLI;
		
		del_timer(&data_timer);
		data_timer.expires=jiffies+max_latency;
		timed_out_data=0;
		add_timer(&data_timer);
		while (!timed_out_data) 
		{
			if (D_S[d].f_multisession) try=maxtim_data*4;
			else try=maxtim_data;
			msg(DBG_000,"sbp_data: CDi_status loop: try=%d.\n",try);
			for ( ; try!=0;try--)
			{
				j=inb(CDi_status);
				if (!(j&s_not_data_ready)) break;;
				if (!(j&s_not_result_ready)) break;
				if (fam0LV_drive) if (j&s_attention) break;
			}
			if (!(j&s_not_data_ready)) goto data_ready;
			if (try==0)
			{
				if (data_retrying == 0) data_waits++;
				data_retrying = 1;
				msg(DBG_000,"sbp_data: CDi_status loop: sleeping.\n");
				sbp_sleep(1);
				try = 1;
			}
		}
		msg(DBG_INF,"sbp_data: CDi_status loop expired.\n");
	data_ready:
		del_timer(&data_timer);

		if (timed_out_data)
		{
			msg(DBG_INF,"sbp_data: CDi_status timeout (timed_out_data) (%02X).\n", j);
			error_flag++;
		}
		if (try==0)
		{
			msg(DBG_INF,"sbp_data: CDi_status timeout (try=0) (%02X).\n", j);
			error_flag++;
		}
		if (!(j&s_not_result_ready))
		{
			msg(DBG_INF, "sbp_data: RESULT_READY where DATA_READY awaited (%02X).\n", j);
			response_count=20;
			j=ResponseInfo();
			j=inb(CDi_status);
		}
		if (j&s_not_data_ready)
		{
			if ((D_S[d].ored_ctl_adr&0x40)==0)
				msg(DBG_INF, "CD contains no data tracks.\n");
			else msg(DBG_INF, "sbp_data: DATA_READY timeout (%02X).\n", j);
			error_flag++;
		}
		SBPCD_STI;
		if (error_flag) break;

		msg(DBG_000, "sbp_data: beginning to read.\n");
		p = D_S[d].sbp_buf + frame *  CD_FRAMESIZE;
		if (sbpro_type==1) OUT(CDo_sel_i_d,1);
		if (cmd_type==READ_M2) {
                        if (do_16bit) insw(CDi_data, xa_head_buf, CD_XA_HEAD>>1);
                        else insb(CDi_data, xa_head_buf, CD_XA_HEAD);
		}
		if (do_16bit) insw(CDi_data, p, CD_FRAMESIZE>>1);
		else insb(CDi_data, p, CD_FRAMESIZE);
		if (cmd_type==READ_M2) {
                        if (do_16bit) insw(CDi_data, xa_tail_buf, CD_XA_TAIL>>1);
                        else insb(CDi_data, xa_tail_buf, CD_XA_TAIL);
		}
		D_S[d].sbp_current++;
		if (sbpro_type==1) OUT(CDo_sel_i_d,0);
		if (cmd_type==READ_M2)
		{
			for (xa_count=0;xa_count<CD_XA_HEAD;xa_count++)
				sprintf(&msgbuf[xa_count*3], " %02X", xa_head_buf[xa_count]);
			msgbuf[xa_count*3]=0;
			msg(DBG_XA1,"xa head:%s\n", msgbuf);
		}
		data_retrying = 0;
		data_tries++;
		if (data_tries >= 1000)
		{
			msg(DBG_INF,"sbp_data() statistics: %d waits in %d frames.\n", data_waits, data_tries);
			data_waits = data_tries = 0;
		}
	}
	duration=jiffies-duration;
	msg(DBG_TEA,"time to read %d frames: %d jiffies .\n",frame,duration);
	if (famT_drive)
	{
		wait=8;
		do
		{
			if (teac==2)
                          {
                            if ((i=CDi_stat_loop_T()) == -1) break;
                          }
                        else
                          {
                            sbp_sleep(1);
                            OUT(CDo_sel_i_d,0); 
                            i=inb(CDi_status);
                          } 
			if (!(i&s_not_data_ready))
			{
				OUT(CDo_sel_i_d,1);
				j=0;
				do
				{
					if (do_16bit) i=inw(CDi_data);
					else i=inb(CDi_data);
					j++;
					i=inb(CDi_status);
				}
				while (!(i&s_not_data_ready));
				msg(DBG_TEA, "==========too much data (%d bytes/words)==============.\n", j);
			}
			if (!(i&s_not_result_ready))
			{
				OUT(CDo_sel_i_d,0);
				l=0;
				do
				{
					infobuf[l++]=inb(CDi_info);
					i=inb(CDi_status);
				}
				while (!(i&s_not_result_ready));
				if (infobuf[0]==0x00) success=1;
#if 1
				for (j=0;j<l;j++) sprintf(&msgbuf[j*3], " %02X", infobuf[j]);
				msgbuf[j*3]=0;
				msg(DBG_TEA,"sbp_data info response:%s\n", msgbuf);
#endif
				if (infobuf[0]==0x02)
				{
					error_flag++;
					do
					{
						++recursion;
						if (recursion>1) msg(DBG_TEA,"cmd_out_T READ_ERR recursion (sbp_data): %d !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",recursion);
						else msg(DBG_TEA,"sbp_data: CMDT_READ_ERR necessary.\n");
						clr_cmdbuf();
						drvcmd[0]=CMDT_READ_ERR;
						j=cmd_out_T(); /* !!! recursive here !!! */
						--recursion;
						sbp_sleep(1);
					}
					while (j<0);
					D_S[d].error_state=infobuf[2];
					D_S[d].b3=infobuf[3];
					D_S[d].b4=infobuf[4];
				}
				break;
			}
			else
			{
#if 0
				msg(DBG_TEA, "============= waiting for result=================.\n");
				sbp_sleep(1);
#endif
			}
		}
		while (wait--);
	}

	if (error_flag) /* must have been spurious D_RDY or (ATTN&&!D_RDY) */
	{
		msg(DBG_TEA, "================error flag: %d=================.\n", error_flag);
		msg(DBG_INF,"sbp_data: read aborted by drive.\n");
#if 1
		i=cc_DriveReset(); /* ugly fix to prevent a hang */
#else
		i=cc_ReadError();
#endif
		return (0);
	}
	
	if (fam0LV_drive)
	{
		SBPCD_CLI;
		i=maxtim_data;
		for (timeout=jiffies+HZ; time_before(jiffies, timeout); timeout--)
		{
			for ( ;i!=0;i--)
			{
				j=inb(CDi_status);
				if (!(j&s_not_data_ready)) break;
				if (!(j&s_not_result_ready)) break;
				if (j&s_attention) break;
			}
			if (i != 0 || time_after_eq(jiffies, timeout)) break;
			sbp_sleep(0);
			i = 1;
		}
		if (i==0) msg(DBG_INF,"status timeout after READ.\n");
		if (!(j&s_attention))
		{
			msg(DBG_INF,"sbp_data: timeout waiting DRV_ATTN - retrying.\n");
			i=cc_DriveReset();  /* ugly fix to prevent a hang */
			SBPCD_STI;
			return (0);
		}
		SBPCD_STI;
	}
	
#if 0
	if (!success)
#endif 0
		do
		{
			if (fam0LV_drive) cc_ReadStatus();
#if 1
			if (famT_drive) msg(DBG_TEA, "================before ResponseStatus=================.\n", i);
#endif 1
			i=ResponseStatus();  /* builds status_bits, returns orig. status (old) or faked p_success (new) */
#if 1
			if (famT_drive)	msg(DBG_TEA, "================ResponseStatus: %d=================.\n", i);
#endif 1
			if (i<0)
			{
				msg(DBG_INF,"bad cc_ReadStatus after read: %02X\n", D_S[d].status_bits);
				return (0);
			}
		}
		while ((fam0LV_drive)&&(!st_check)&&(!(i&p_success)));
	if (st_check)
	{
		i=cc_ReadError();
		msg(DBG_INF,"cc_ReadError was necessary after read: %d\n",i);
		return (0);
	}
	if (fatal_err)
	{
		fatal_err=0;
		D_S[d].sbp_first_frame=D_S[d].sbp_last_frame=-1;      /* purge buffer */
		D_S[d].sbp_current = 0;
		msg(DBG_INF,"sbp_data: fatal_err - retrying.\n");
		return (0);
	}
	
	D_S[d].sbp_first_frame = req -> sector / 4;
	D_S[d].sbp_last_frame = D_S[d].sbp_first_frame + D_S[d].sbp_read_frames - 1;
	sbp_transfer(req);
	return (1);
}
/*==========================================================================*/
/*==========================================================================*/
/*
 *  Open the device special file.  Check that a disk is in. Read TOC.
 */
static int sbpcd_open(struct cdrom_device_info *cdi, int purpose)
{
	int i;

	i = MINOR(cdi->dev);

	MOD_INC_USE_COUNT;
	down(&ioctl_read_sem);
	switch_drive(i);

	/*
	 * try to keep an "open" counter here and lock the door if 0->1.
	 */
	msg(DBG_LCK,"open_count: %d -> %d\n",
	    D_S[d].open_count,D_S[d].open_count+1);
	if (++D_S[d].open_count<=1)
	{
		i=LockDoor();
		D_S[d].open_count=1;
		if (famT_drive)	msg(DBG_TEA,"sbpcd_open: before i=DiskInfo();.\n");
		i=DiskInfo();
		if (famT_drive)	msg(DBG_TEA,"sbpcd_open: after i=DiskInfo();.\n");
		if ((D_S[d].ored_ctl_adr&0x40)==0)
		{		
			msg(DBG_INF,"CD contains no data tracks.\n");
#if SAFE_MIXED
			D_S[d].has_data=0;
#endif SAFE_MIXED
		}
#if SAFE_MIXED
		else if (D_S[d].has_data<1) D_S[d].has_data=1;
#endif SAFE_MIXED
	}
	if (!st_spinning) cc_SpinUp();
	RETURN_UP(0);
}
/*==========================================================================*/
/*
 *  On close, we flush all sbp blocks from the buffer cache.
 */
static void sbpcd_release(struct cdrom_device_info * cdi)
{
	int i;
	
	i = MINOR(cdi->dev);
	if ((i<0) || (i>=NR_SBPCD) || (D_S[i].drv_id==-1))
	{
		msg(DBG_INF, "release: bad device: %04X\n", cdi->dev);
		return ;
	}
	down(&ioctl_read_sem);
	switch_drive(i);
	/*
	 * try to keep an "open" counter here and unlock the door if 1->0.
	 */
	msg(DBG_LCK,"open_count: %d -> %d\n",
	    D_S[d].open_count,D_S[d].open_count-1);
	if (D_S[d].open_count>-2) /* CDROMEJECT may have been done */
	{
		if (--D_S[d].open_count<=0) 
		{
			D_S[d].sbp_first_frame=D_S[d].sbp_last_frame=-1;
			invalidate_buffers(cdi->dev);
			if (D_S[d].audio_state!=audio_playing)
				if (D_S[d].f_eject) cc_SpinDown();
			D_S[d].diskstate_flags &= ~cd_size_bit;
			D_S[d].open_count=0; 
#if SAFE_MIXED
			D_S[d].has_data=0;
#endif SAFE_MIXED
		}
	}
	up(&ioctl_read_sem);
	MOD_DEC_USE_COUNT;
	return ;
}
/*==========================================================================*/
/*
 *
 */
static int sbpcd_media_changed( struct cdrom_device_info *cdi, int disc_nr);
static struct cdrom_device_ops sbpcd_dops = {
	open:			sbpcd_open,
	release:		sbpcd_release,
	drive_status:		sbpcd_drive_status,
	media_changed:		sbpcd_media_changed,
	tray_move:		sbpcd_tray_move,
	lock_door:		sbpcd_lock_door,
	select_speed:		sbpcd_select_speed,
	get_last_session:	sbpcd_get_last_session,
	get_mcn:		sbpcd_get_mcn,
	reset:			sbpcd_reset,
	audio_ioctl:		sbpcd_audio_ioctl,
	dev_ioctl:		sbpcd_dev_ioctl,
	capability:		CDC_CLOSE_TRAY | CDC_OPEN_TRAY | CDC_LOCK |
				CDC_MULTI_SESSION | CDC_MEDIA_CHANGED |
				CDC_MCN | CDC_PLAY_AUDIO | CDC_IOCTLS,
	n_minors:		1,
};

static struct cdrom_device_info sbpcd_info = {
	ops:			&sbpcd_dops,
	speed:			2,
	capacity:		1,
	name:			"sbpcd",
};

/*==========================================================================*/
/*
 * accept "kernel command line" parameters 
 * (suggested by Peter MacDonald with SLS 1.03)
 *
 * This is only implemented for the first controller. Should be enough to
 * allow installing with a "strange" distribution kernel.
 *
 * use: tell LILO:
 *                 sbpcd=0x230,SoundBlaster
 *             or
 *                 sbpcd=0x300,LaserMate
 *             or
 *                 sbpcd=0x338,SoundScape
 *             or
 *                 sbpcd=0x2C0,Teac16bit
 *
 * (upper/lower case sensitive here - but all-lowercase is ok!!!).
 *
 * the address value has to be the CDROM PORT ADDRESS -
 * not the soundcard base address.
 * For the SPEA/SoundScape setup, DO NOT specify the "configuration port"
 * address, but the address which is really used for the CDROM (usually 8
 * bytes above).
 *
 */

#if (SBPCD_ISSUE-1)
static int sbpcd_setup(char *s)
#else
int sbpcd_setup(char *s)
#endif
{
	int p[4];
	(void)get_options(s, ARRAY_SIZE(p), p);
	setup_done++;
	msg(DBG_INI,"sbpcd_setup called with %04X,%s\n",p[1], s);
	sbpro_type=0; /* default: "LaserMate" */
	if (p[0]>1) sbpro_type=p[2];
	else if (!strcmp(s,str_sb)) sbpro_type=1;
	else if (!strcmp(s,str_sb_l)) sbpro_type=1;
	else if (!strcmp(s,str_sp)) sbpro_type=2;
	else if (!strcmp(s,str_sp_l)) sbpro_type=2;
	else if (!strcmp(s,str_ss)) sbpro_type=2;
	else if (!strcmp(s,str_ss_l)) sbpro_type=2;
	else if (!strcmp(s,str_t16)) sbpro_type=3;
	else if (!strcmp(s,str_t16_l)) sbpro_type=3;
	if (p[0]>0) sbpcd_ioaddr=p[1];
	
	CDo_command=sbpcd_ioaddr;
	CDi_info=sbpcd_ioaddr;
	CDi_status=sbpcd_ioaddr+1;
	CDo_sel_i_d=sbpcd_ioaddr+1;
	CDo_reset=sbpcd_ioaddr+2;
	CDo_enable=sbpcd_ioaddr+3; 
	f_16bit=0;
	if ((sbpro_type==1)||(sbpro_type==3))
	{
		CDi_data=sbpcd_ioaddr;
		if (sbpro_type==3)
                {
                        f_16bit=1;
                        sbpro_type=1;
                }
	}
	else CDi_data=sbpcd_ioaddr+2;

	return 1;
}

__setup("sbpcd=", sbpcd_setup);


/*==========================================================================*/
/*
 * Sequoia S-1000 CD-ROM Interface Configuration
 * as used within SPEA Media FX, Ensonic SoundScape and some Reveal cards
 * The soundcard has to get jumpered for the interface type "Panasonic"
 * (not Sony or Mitsumi) and to get soft-configured for
 *     -> configuration port address
 *     -> CDROM port offset (num_ports): has to be 8 here. Possibly this
 *        offset value determines the interface type (none, Panasonic,
 *        Mitsumi, Sony).
 *        The interface uses a configuration port (0x320, 0x330, 0x340, 0x350)
 *        some bytes below the real CDROM address.
 *         
 *        For the Panasonic style (LaserMate) interface and the configuration
 *        port 0x330, we have to use an offset of 8; so, the real CDROM port
 *        address is 0x338.
 */
static int __init config_spea(void)
{
	/*
         * base address offset between configuration port and CDROM port,
	 * this probably defines the interface type
         *   2 (type=??): 0x00
         *   8 (type=LaserMate):0x10
         *  16 (type=??):0x20
         *  32 (type=??):0x30
         */
	int n_ports=0x10;

	int irq_number=0; /* off:0x00, 2/9:0x01, 7:0x03, 12:0x05, 15:0x07 */
	int dma_channel=0; /* off: 0x00, 0:0x08, 1:0x18, 3:0x38, 5:0x58, 6:0x68 */
	int dack_polarity=0; /* L:0x00, H:0x80 */
	int drq_polarity=0x40; /* L:0x00, H:0x40 */
	int i;

#define SPEA_REG_1 sbpcd_ioaddr-0x08+4
#define SPEA_REG_2 sbpcd_ioaddr-0x08+5
	
	OUT(SPEA_REG_1,0xFF);
	i=inb(SPEA_REG_1);
	if (i!=0x0F)
	{
		msg(DBG_SEQ,"no SPEA interface at %04X present.\n", sbpcd_ioaddr);
		return (-1); /* no interface found */
	}
	OUT(SPEA_REG_1,0x04);
	OUT(SPEA_REG_2,0xC0);
	
	OUT(SPEA_REG_1,0x05);
	OUT(SPEA_REG_2,0x10|drq_polarity|dack_polarity);
	
#if 1
#define SPEA_PATTERN 0x80
#else
#define SPEA_PATTERN 0x00
#endif
	OUT(SPEA_REG_1,0x06);
	OUT(SPEA_REG_2,dma_channel|irq_number|SPEA_PATTERN);
	OUT(SPEA_REG_2,dma_channel|irq_number|SPEA_PATTERN);
	
	OUT(SPEA_REG_1,0x09);
	i=(inb(SPEA_REG_2)&0xCF)|n_ports;
	OUT(SPEA_REG_2,i);
	
	sbpro_type = 0; /* acts like a LaserMate interface now */
	msg(DBG_SEQ,"found SoundScape interface at %04X.\n", sbpcd_ioaddr);
	return (0);
}
/*==========================================================================*/
/*
 *  Test for presence of drive and initialize it.
 *  Called once at boot or load time.
 */

static devfs_handle_t devfs_handle;

#ifdef MODULE
int __init __SBPCD_INIT(void)
#else
int __init SBPCD_INIT(void)
#endif MODULE
{
	char nbuff[16];
	int i=0, j=0;
	int addr[2]={1, CDROM_PORT};
	int port_index;

	sti();
	
	msg(DBG_INF,"sbpcd.c %s\n", VERSION);
#ifndef MODULE
#if DISTRIBUTION
	if (!setup_done)
	{
		msg(DBG_INF,"Looking for Matsushita/Panasonic, CreativeLabs, Longshine, TEAC CD-ROM drives\n");
		msg(DBG_INF,"= = = = = = = = = = W A R N I N G = = = = = = = = = =\n");
		msg(DBG_INF,"Auto-Probing can cause a hang (f.e. touching an NE2000 card).\n");
		msg(DBG_INF,"If that happens, you have to reboot and use the\n");
		msg(DBG_INF,"LILO (kernel) command line feature like:\n");
		msg(DBG_INF,"   LILO boot: ... sbpcd=0x230,SoundBlaster\n");
		msg(DBG_INF,"or like:\n");
		msg(DBG_INF,"   LILO boot: ... sbpcd=0x300,LaserMate\n");
		msg(DBG_INF,"or like:\n");
		msg(DBG_INF,"   LILO boot: ... sbpcd=0x338,SoundScape\n");
		msg(DBG_INF,"with your REAL address.\n");
		msg(DBG_INF,"= = = = = = = = = = END of WARNING = = = = = == = = =\n");
	}
#endif DISTRIBUTION
	sbpcd[0]=sbpcd_ioaddr; /* possibly changed by kernel command line */
	sbpcd[1]=sbpro_type; /* possibly changed by kernel command line */
#endif MODULE
	
	for (port_index=0;port_index<NUM_PROBE;port_index+=2)
	{
		addr[1]=sbpcd[port_index];
		if (addr[1]==0) break;
		if (check_region(addr[1],4))
		{
			msg(DBG_INF,"check_region: %03X is not free.\n",addr[1]);
			continue;
		}
		if (sbpcd[port_index+1]==2) type=str_sp;
		else if (sbpcd[port_index+1]==1) type=str_sb;
		else if (sbpcd[port_index+1]==3) type=str_t16;
		else type=str_lm;
		sbpcd_setup((char *)type);
#if DISTRIBUTION
		msg(DBG_INF,"Scanning 0x%X (%s)...\n", CDo_command, type);
#endif DISTRIBUTION
		if (sbpcd[port_index+1]==2)
		{
			i=config_spea();
			if (i<0) continue;
		}
#ifdef PATH_CHECK
		if (check_card(addr[1])) continue;
#endif PATH_CHECK
		i=check_drives();
		msg(DBG_INI,"check_drives done.\n");
		if (i>=0) break; /* drive found */
	} /* end of cycling through the set of possible I/O port addresses */
	
	if (ndrives==0)
	{
		msg(DBG_INF, "No drive found.\n");
#ifdef MODULE
		return -EIO;
#else
		goto init_done;
#endif MODULE
	}
	
	if (port_index>0)
          {
            msg(DBG_INF, "You should read linux/Documentation/cdrom/sbpcd\n");
            msg(DBG_INF, "and then configure sbpcd.h for your hardware.\n");
          }
	check_datarate();
	msg(DBG_INI,"check_datarate done.\n");
	
	for (j=0;j<NR_SBPCD;j++)
	{
		if (D_S[j].drv_id==-1) continue;
		switch_drive(j);
#if 1
		if (!famL_drive) cc_DriveReset();
#endif 0
		if (!st_spinning) cc_SpinUp();
		D_S[j].sbp_first_frame = -1;  /* First frame in buffer */
		D_S[j].sbp_last_frame = -1;   /* Last frame in buffer  */
		D_S[j].sbp_read_frames = 0;   /* Number of frames being read to buffer */
		D_S[j].sbp_current = 0;       /* Frame being currently read */
		D_S[j].CD_changed=1;
		D_S[j].frame_size=CD_FRAMESIZE;
		D_S[j].f_eject=0;
#if EJECT
		if (!fam0_drive) D_S[j].f_eject=1;
#endif EJECT
		cc_ReadStatus();
		i=ResponseStatus();  /* returns orig. status or p_busy_new */
		if (famT_drive) i=ResponseStatus();  /* returns orig. status or p_busy_new */
		if (i<0)
		{
			if (i!=-402)
				msg(DBG_INF,"init: ResponseStatus returns %d.\n",i);
		}
		else
		{
			if (st_check)
			{
				i=cc_ReadError();
				msg(DBG_INI,"init: cc_ReadError returns %d\n",i);
			}
		}
		msg(DBG_INI,"init: first GetStatus: %d\n",i);
		msg(DBG_LCS,"init: first GetStatus: error_byte=%d\n",
		    D_S[j].error_byte);
		if (D_S[j].error_byte==aud_12)
		{
			timeout=jiffies+2*HZ;
			do
			{
				i=GetStatus();
				msg(DBG_INI,"init: second GetStatus: %02X\n",i);
				msg(DBG_LCS,
				    "init: second GetStatus: error_byte=%d\n",
				    D_S[j].error_byte);
				if (i<0) break;
				if (!st_caddy_in) break;
				}
			while ((!st_diskok)||time_after(jiffies, timeout));
		}
		i=SetSpeed();
		if (i>=0) D_S[j].CD_changed=1;
	}
	
	/*
	 * Turn on the CD audio channels.
	 * The addresses are obtained from SOUND_BASE (see sbpcd.h).
	 */
#if SOUND_BASE
	OUT(MIXER_addr,MIXER_CD_Volume); /* select SB Pro mixer register */
	OUT(MIXER_data,0xCC); /* one nibble per channel, max. value: 0xFF */
#endif SOUND_BASE
	
	if (devfs_register_blkdev(MAJOR_NR, major_name, &cdrom_fops) != 0)
	{
		msg(DBG_INF, "Can't get MAJOR %d for Matsushita CDROM\n", MAJOR_NR);
#ifdef MODULE
		return -EIO;
#else
		goto init_done;
#endif MODULE
	}
	blk_init_queue(BLK_DEFAULT_QUEUE(MAJOR_NR), DEVICE_REQUEST);
	blk_queue_headactive(BLK_DEFAULT_QUEUE(MAJOR_NR), 0);
	read_ahead[MAJOR_NR] = buffers * (CD_FRAMESIZE / 512);
	
	request_region(CDo_command,4,major_name);
	
	devfs_handle = devfs_mk_dir (NULL, "sbp", NULL);
	for (j=0;j<NR_SBPCD;j++)
	{
		struct cdrom_device_info * sbpcd_infop;

		if (D_S[j].drv_id==-1) continue;
		switch_drive(j);
#if SAFE_MIXED
		D_S[j].has_data=0;
#endif SAFE_MIXED
		/*
		 * allocate memory for the frame buffers
		 */
		D_S[j].aud_buf=NULL;
		D_S[j].sbp_audsiz=0;
		D_S[j].sbp_bufsiz=buffers;
		if (D_S[j].drv_type&drv_fam1)
			if (READ_AUDIO>0) D_S[j].sbp_audsiz=READ_AUDIO;
		D_S[j].sbp_buf=(u_char *) vmalloc(D_S[j].sbp_bufsiz*CD_FRAMESIZE);
		if (D_S[j].sbp_buf==NULL)
		{
			msg(DBG_INF,"data buffer (%d frames) not available.\n",D_S[j].sbp_bufsiz);
			if ((devfs_unregister_blkdev(MAJOR_NR, major_name) == -EINVAL))
			{
				printk("Can't unregister %s\n", major_name);
			}
			release_region(CDo_command,4);
			blk_cleanup_queue(BLK_DEFAULT_QUEUE(MAJOR_NR));
			return -EIO;
		}
#ifdef MODULE
		msg(DBG_INF,"data buffer size: %d frames.\n",buffers);
#endif MODULE
		if (D_S[j].sbp_audsiz>0)
		{
			D_S[j].aud_buf=(u_char *) vmalloc(D_S[j].sbp_audsiz*CD_FRAMESIZE_RAW);
			if (D_S[j].aud_buf==NULL) msg(DBG_INF,"audio buffer (%d frames) not available.\n",D_S[j].sbp_audsiz);
			else msg(DBG_INF,"audio buffer size: %d frames.\n",D_S[j].sbp_audsiz);
		}
                sbpcd_infop = vmalloc(sizeof (struct cdrom_device_info));
		if (sbpcd_infop == NULL)
		{
                        release_region(CDo_command,4);
			blk_cleanup_queue(BLK_DEFAULT_QUEUE(MAJOR_NR));
                        return -ENOMEM;
		}
		D_S[j].sbpcd_infop = sbpcd_infop;
		memcpy (sbpcd_infop, &sbpcd_info, sizeof(struct cdrom_device_info));
		sbpcd_infop->dev = MKDEV(MAJOR_NR, j);
		strncpy(sbpcd_infop->name,major_name, sizeof(sbpcd_infop->name)); 

		sprintf (nbuff, "c%dt%d/cd", SBPCD_ISSUE - 1, D_S[j].drv_id);
		sbpcd_infop->de =
		    devfs_register (devfs_handle, nbuff, DEVFS_FL_DEFAULT,
				    MAJOR_NR, j, S_IFBLK | S_IRUGO | S_IWUGO,
				    &cdrom_fops, NULL);
		if (register_cdrom(sbpcd_infop))
		{
                	printk(" sbpcd: Unable to register with Uniform CD-ROm driver\n");
		}
		/*
		 * set the block size
		 */
		sbpcd_blocksizes[j]=CD_FRAMESIZE;
	}
	blksize_size[MAJOR_NR]=sbpcd_blocksizes;

#ifndef MODULE
 init_done:
#if !(SBPCD_ISSUE-1)
#ifdef CONFIG_SBPCD2
	sbpcd2_init();
#endif CONFIG_SBPCD2
#ifdef CONFIG_SBPCD3
	sbpcd3_init();
#endif CONFIG_SBPCD3
#ifdef CONFIG_SBPCD4
	sbpcd4_init();
#endif CONFIG_SBPCD4
#endif !(SBPCD_ISSUE-1)
#endif MODULE
	return 0;
}
/*==========================================================================*/
#ifdef MODULE
void sbpcd_exit(void)
{
	int j;
	
	if ((devfs_unregister_blkdev(MAJOR_NR, major_name) == -EINVAL))
	{
		msg(DBG_INF, "What's that: can't unregister %s.\n", major_name);
		return;
	}
	release_region(CDo_command,4);
	blk_cleanup_queue(BLK_DEFAULT_QUEUE(MAJOR_NR));
	devfs_unregister (devfs_handle);
	for (j=0;j<NR_SBPCD;j++)
	{
		if (D_S[j].drv_id==-1) continue;
		vfree(D_S[j].sbp_buf);
		if (D_S[j].sbp_audsiz>0) vfree(D_S[j].aud_buf);
		if ((unregister_cdrom(D_S[j].sbpcd_infop) == -EINVAL))
		{
			msg(DBG_INF, "What's that: can't unregister info %s.\n", major_name);
			return;
		}
		vfree(D_S[j].sbpcd_infop);
	}
	msg(DBG_INF, "%s module released.\n", major_name);
}


#ifdef MODULE
module_init(__SBPCD_INIT) /*HACK!*/;
#endif
module_exit(sbpcd_exit);


#endif MODULE
/*==========================================================================*/
/*
 * Check if the media has changed in the CD-ROM drive.
 * used externally (isofs/inode.c, fs/buffer.c)
 */
static int sbpcd_chk_disk_change(kdev_t full_dev)
{
	int i;
	
	msg(DBG_CHK,"media_check (%d) called\n", MINOR(full_dev));
	i=MINOR(full_dev);
	
	if (D_S[i].CD_changed==0xFF)
        {
                D_S[i].CD_changed=0;
                msg(DBG_CHK,"medium changed (drive %d)\n", i);
		/* BUG! Should invalidate buffers! --AJK */
		invalidate_buffers(full_dev);
		D_S[d].diskstate_flags &= ~toc_bit;
		D_S[d].diskstate_flags &= ~cd_size_bit;
#if SAFE_MIXED
		D_S[d].has_data=0;
#endif SAFE_MIXED

                return (1);
        }
        else
                return (0);
}

static int sbpcd_media_changed( struct cdrom_device_info *cdi, int disc_nr)
{
   return sbpcd_chk_disk_change(cdi->dev);
}
/*==========================================================================*/
/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file. 
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-indent-level: 8
 * c-brace-imaginary-offset: 0
 * c-brace-offset: -8
 * c-argdecl-indent: 8
 * c-label-offset: -8
 * c-continued-statement-offset: 8
 * c-continued-brace-offset: 0
 * End:
 */
