/* aha152x.c -- Adaptec AHA-152x driver
 * Author: J�rgen E. Fischer, fischer@norbit.de
 * Copyright 1993-1999 J�rgen E. Fischer
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 *
 * $Id: aha152x.c,v 2.3 2000/11/04 16:40:26 fischer Exp $
 *
 * $Log: aha152x.c,v $
 * Revision 2.3  2000/11/04 16:40:26  fischer
 * - handle data overruns
 * - extend timeout for data phases
 *
 * Revision 2.2  2000/08/08 19:54:53  fischer
 * - minor changes
 *
 * Revision 2.1  2000/05/17 16:23:17  fischer
 * - signature update
 * - fix for data out w/o scatter gather
 *
 * Revision 2.0  1999/12/25 15:07:32  fischer
 * - interrupt routine completly reworked
 * - basic support for new eh code
 *
 * Revision 1.21  1999/11/10 23:46:36  fischer
 * - default to synchronous operation
 * - synchronous negotiation fixed
 * - added timeout to loops
 * - debugging output can be controlled through procfs
 *
 * Revision 1.20  1999/11/07 18:37:31  fischer
 * - synchronous operation works
 * - resid support for sg driver
 *
 * Revision 1.19  1999/11/02 22:39:59  fischer
 * - moved leading comments to README.aha152x
 * - new additional module parameters
 * - updates for 2.3
 * - support for the Tripace TC1550 controller
 * - interrupt handling changed
 *
 * Revision 1.18  1996/09/07 20:10:40  fischer
 * - fixed can_queue handling (multiple outstanding commands working again)
 *
 * Revision 1.17  1996/08/17 16:05:14  fischer
 * - biosparam improved
 * - interrupt verification
 * - updated documentation
 * - cleanups
 *
 * Revision 1.16  1996/06/09 00:04:56  root
 * - added configuration symbols for insmod (aha152x/aha152x1)
 *
 * Revision 1.15  1996/04/30 14:52:06  fischer
 * - proc info fixed
 * - support for extended translation for >1GB disks
 *
 * Revision 1.14  1996/01/17  15:11:20  fischer
 * - fixed lockup in MESSAGE IN phase after reconnection
 *
 * Revision 1.13  1996/01/09  02:15:53  fischer
 * - some cleanups
 * - moved request_irq behind controller initialization
 *   (to avoid spurious interrupts)
 *
 * Revision 1.12  1995/12/16  12:26:07  fischer
 * - barrier()s added
 * - configurable RESET delay added
 *
 * Revision 1.11  1995/12/06  21:18:35  fischer
 * - some minor updates
 *
 * Revision 1.10  1995/07/22  19:18:45  fischer
 * - support for 2 controllers
 * - started synchronous data transfers (not working yet)
 *
 * Revision 1.9  1995/03/18  09:20:24  root
 * - patches for PCMCIA and modules
 *
 * Revision 1.8  1995/01/21  22:07:19  root
 * - snarf_region => request_region
 * - aha152x_intr interface change
 *
 * Revision 1.7  1995/01/02  23:19:36  root
 * - updated COMMAND_SIZE to cmd_len
 * - changed sti() to restore_flags()
 * - fixed some #ifdef which generated warnings
 *
 * Revision 1.6  1994/11/24  20:35:27  root
 * - problem with odd number of bytes in fifo fixed
 *
 * Revision 1.5  1994/10/30  14:39:56  root
 * - abort code fixed
 * - debugging improved
 *
 * Revision 1.4  1994/09/12  11:33:01  root
 * - irqaction to request_irq
 * - abortion updated
 *
 * Revision 1.3  1994/08/04  13:53:05  root
 * - updates for mid-level-driver changes
 * - accept unexpected BUSFREE phase as error condition
 * - parity check now configurable
 *
 * Revision 1.2  1994/07/03  12:56:36  root
 * - cleaned up debugging code
 * - more tweaking on reset delays
 * - updated abort/reset code (pretty untested...)
 *
 * Revision 1.1  1994/05/28  21:18:49  root
 * - update for mid-level interface change (abort-reset)
 * - delays after resets adjusted for some slow devices
 *
 * Revision 1.0  1994/03/25  12:52:00  root
 * - Fixed "more data than expected" problem
 * - added new BIOS signatures
 *
 * Revision 0.102  1994/01/31  20:44:12  root
 * - minor changes in insw/outsw handling
 *
 * Revision 0.101  1993/12/13  01:16:27  root
 * - fixed STATUS phase (non-GOOD stati were dropped sometimes;
 *   fixes problems with CD-ROM sector size detection & media change)
 *
 * Revision 0.100  1993/12/10  16:58:47  root
 * - fix for unsuccessful selections in case of non-continuous id assignments
 *   on the scsi bus.
 *
 * Revision 0.99  1993/10/24  16:19:59  root
 * - fixed DATA IN (rare read errors gone)
 *
 * Revision 0.98  1993/10/17  12:54:44  root
 * - fixed some recent fixes (shame on me)
 * - moved initialization of scratch area to aha152x_queue
 *
 * Revision 0.97  1993/10/09  18:53:53  root
 * - DATA IN fixed. Rarely left data in the fifo.
 *
 * Revision 0.96  1993/10/03  00:53:59  root
 * - minor changes on DATA IN
 *
 * Revision 0.95  1993/09/24  10:36:01  root
 * - change handling of MSGI after reselection
 * - fixed sti/cli
 * - minor changes
 *
 * Revision 0.94  1993/09/18  14:08:22  root
 * - fixed bug in multiple outstanding command code
 * - changed detection
 * - support for kernel command line configuration
 * - reset corrected
 * - changed message handling
 *
 * Revision 0.93  1993/09/15  20:41:19  root
 * - fixed bugs with multiple outstanding commands
 *
 * Revision 0.92  1993/09/13  02:46:33  root
 * - multiple outstanding commands work (no problems with IBM drive)
 *
 * Revision 0.91  1993/09/12  20:51:46  root
 * added multiple outstanding commands
 * (some problem with this $%&? IBM device remain)
 *
 * Revision 0.9  1993/09/12  11:11:22  root
 * - corrected auto-configuration
 * - changed the auto-configuration (added some '#define's)
 * - added support for dis-/reconnection
 *
 * Revision 0.8  1993/09/06  23:09:39  root
 * - added support for the drive activity light
 * - minor changes
 *
 * Revision 0.7  1993/09/05  14:30:15  root
 * - improved phase detection
 * - now using the new snarf_region code of 0.99pl13
 *
 * Revision 0.6  1993/09/02  11:01:38  root
 * first public release; added some signatures and biosparam()
 *
 * Revision 0.5  1993/08/30  10:23:30  root
 * fixed timing problems with my IBM drive
 *
 * Revision 0.4  1993/08/29  14:06:52  root
 * fixed some problems with timeouts due incomplete commands
 *
 * Revision 0.3  1993/08/28  15:55:03  root
 * writing data works too.  mounted and worked on a dos partition
 *
 * Revision 0.2  1993/08/27  22:42:07  root
 * reading data works.  Mounted a msdos partition.
 *
 * Revision 0.1  1993/08/25  13:38:30  root
 * first "damn thing doesn't work" version
 *
 * Revision 0.0  1993/08/14  19:54:25  root
 * empty function bodies; detect() works.
 *
 *
 **************************************************************************
 
 see README.aha152x for configuration details

 **************************************************************************/

#include <linux/module.h>

#if defined(PCMCIA)
#undef MODULE
#endif

#include <linux/sched.h>
#include <asm/io.h>
#include <linux/blk.h>
#include "scsi.h"
#include "sd.h"
#include "hosts.h"
#include "constants.h"
#include <asm/system.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/wait.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <asm/semaphore.h>
#include <linux/spinlock.h>

#include "aha152x.h"
#include <linux/stat.h>

#include <scsi/scsicam.h>

/* DEFINES */

/* For PCMCIA cards, always use AUTOCONF */
#if defined(PCMCIA) || defined(MODULE)
#if !defined(AUTOCONF)
#define AUTOCONF
#endif
#endif

#if !defined(AUTOCONF) && !defined(SETUP0)
#error define AUTOCONF or SETUP0
#endif

#if defined(AHA152X_DEBUG)
#define DEBUG_DEFAULT debug_eh

#define DPRINTK(when,msgs...) \
	do { if(HOSTDATA(shpnt)->debug & (when)) printk(msgs); } while(0)

#define DO_LOCK(flags)	\
	do { \
		if(spin_is_locked(&QLOCK)) { \
			DPRINTK(debug_intr, DEBUG_LEAD "(%s:%d) already locked at %s:%d\n", CMDINFO(CURRENT_SC), __FUNCTION__, __LINE__, QLOCKER, QLOCKERL); \
		} \
		DPRINTK(debug_locks, DEBUG_LEAD "(%s:%d) locking\n", CMDINFO(CURRENT_SC), __FUNCTION__, __LINE__); \
		spin_lock_irqsave(&QLOCK,flags); \
		DPRINTK(debug_locks, DEBUG_LEAD "(%s:%d) locked\n", CMDINFO(CURRENT_SC), __FUNCTION__, __LINE__); \
		QLOCKER=__FUNCTION__; \
		QLOCKERL=__LINE__; \
	} while(0)

#define DO_UNLOCK(flags)	\
	do { \
		DPRINTK(debug_locks, DEBUG_LEAD "(%s:%d) unlocking (locked at %s:%d)\n", CMDINFO(CURRENT_SC), __FUNCTION__, __LINE__, QLOCKER, QLOCKERL); \
		spin_unlock_irqrestore(&QLOCK,flags); \
		DPRINTK(debug_locks, DEBUG_LEAD "(%s:%d) unlocked\n", CMDINFO(CURRENT_SC), __FUNCTION__, __LINE__); \
		QLOCKER="(not locked)"; \
		QLOCKERL=0; \
	} while(0)

#else
#define DPRINTK(when,msgs...)
#define	DO_LOCK(flags)		spin_lock_irqsave(&QLOCK,flags)
#define	DO_UNLOCK(flags)	spin_unlock_irqrestore(&QLOCK,flags)
#endif

#define LEAD		"(scsi%d:%d:%d) "
#define WARN_LEAD	KERN_WARNING	LEAD
#define INFO_LEAD	KERN_INFO	LEAD
#define NOTE_LEAD	KERN_NOTICE	LEAD
#define ERR_LEAD	KERN_ERR	LEAD
#define DEBUG_LEAD	KERN_DEBUG	LEAD
#define CMDINFO(cmd) \
			(cmd) ? ((cmd)->host->host_no) : -1, \
                        (cmd) ? ((cmd)->target & 0x0f) : -1, \
			(cmd) ? ((cmd)->lun & 0x07) : -1

#define DELAY_DEFAULT 100

/* possible irq range */
#if defined(PCMCIA)
#define IRQ_MIN 0
#define IRQ_MAX 16
#else
#define IRQ_MIN 9
#define IRQ_MAX 12
#endif
#define IRQS    IRQ_MAX-IRQ_MIN+1

enum {
	not_issued	= 0x0001,	/* command not yet issued */
	selecting	= 0x0002, 	/* target is beeing selected */
	identified	= 0x0004,	/* IDENTIFY was sent */
	disconnected	= 0x0008,	/* target disconnected */
	completed	= 0x0010,	/* target sent COMMAND COMPLETE */ 
	aborted		= 0x0020,	/* ABORT was sent */
	resetted	= 0x0040,	/* BUS DEVICE RESET was sent */
	spiordy		= 0x0080,	/* waiting for SPIORDY to raise */
	syncneg		= 0x0100,	/* synchronous negotiation in progress */
	aborting	= 0x0200,	/* ABORT is pending */
	resetting	= 0x0400,	/* BUS DEVICE RESET is pending */
};

#if defined(MODULE)
MODULE_AUTHOR("J�rgen Fischer");
MODULE_DESCRIPTION(AHA152X_REVID);
MODULE_PARM(io, "1-2i");
MODULE_PARM_DESC(io,"base io address of controller");
static int io[] = {0, 0};

MODULE_PARM(irq, "1-2i");
MODULE_PARM_DESC(irq,"interrupt for controller");
static int irq[] = {0, 0};

MODULE_PARM(scsiid, "1-2i");
MODULE_PARM_DESC(scsiid,"scsi id of controller");
static int scsiid[] = {7, 7};

MODULE_PARM(reconnect, "1-2i");
MODULE_PARM_DESC(reconnect,"allow targets to disconnect");
static int reconnect[] = {1, 1};

MODULE_PARM(parity, "1-2i");
MODULE_PARM_DESC(parity,"use scsi parity");
static int parity[] = {1, 1};

MODULE_PARM(sync, "1-2i");
MODULE_PARM_DESC(sync,"use synchronous transfers");
static int sync[] = {1, 1};

MODULE_PARM(delay, "1-2i");
MODULE_PARM_DESC(delay,"scsi reset delay");
static int delay[] = {DELAY_DEFAULT, DELAY_DEFAULT};

MODULE_PARM(exttrans, "1-2i");
MODULE_PARM_DESC(exttrans,"use extended translation");
static int exttrans[] = {0, 0};

#if !defined(AHA152X_DEBUG)
MODULE_PARM(aha152x, "1-8i");
MODULE_PARM_DESC(aha152x, "parameters for first controller");
static int aha152x[] = {0, 11, 7, 1, 1, 0, DELAY_DEFAULT, 0};

MODULE_PARM(aha152x1, "1-8i");
MODULE_PARM_DESC(aha152x1, "parameters for second controller");
static int aha152x1[] = {0, 11, 7, 1, 1, 0, DELAY_DEFAULT, 0};
#else
MODULE_PARM(debug, "1-2i");
MODULE_PARM_DESC(debug, "flags for driver debugging");
static int debug[] = {DEBUG_DEFAULT, DEBUG_DEFAULT};

MODULE_PARM(aha152x, "1-9i");
MODULE_PARM_DESC(aha152x, "parameters for first controller");
static int aha152x[]   = {0, 11, 7, 1, 1, 1, DELAY_DEFAULT, 0, DEBUG_DEFAULT};

MODULE_PARM(aha152x1, "1-9i");
MODULE_PARM_DESC(aha152x1, "parameters for second controller");
static int aha152x1[]  = {0, 11, 7, 1, 1, 1, DELAY_DEFAULT, 0, DEBUG_DEFAULT};
#endif /* !defined(AHA152X_DEBUG) */
#endif /* MODULE */

/* set by aha152x_setup according to the command line */
static int setup_count = 0;
static int registered_count = 0;
static struct aha152x_setup {
	int io_port;
	int irq;
	int scsiid;
	int reconnect;
	int parity;
	int synchronous;
	int delay;
	int ext_trans;
	int tc1550;
#if defined(AHA152X_DEBUG)
	int debug;
#endif
	char *conf;
} setup[2];

static struct Scsi_Host *aha152x_host[IRQS];

/*
 * internal states of the host
 *
 */ 
enum aha152x_state {
	idle=0,
	unknown,
	seldo,
	seldi,
	selto,
	busfree,
	msgo,
	cmd,
	msgi,
	status,
	datai,
	datao,
	parerr,
	rsti,
	maxstate
};

/*
 * current state information of the host
 *
 */
struct aha152x_hostdata {
	Scsi_Cmnd *issue_SC;
		/* pending commands to issue */

	Scsi_Cmnd *current_SC;
		/* current command on the bus */

	Scsi_Cmnd *disconnected_SC;
		/* commands that disconnected */

	Scsi_Cmnd *done_SC;
		/* command that was completed */

	spinlock_t lock;
		/* host lock */

#if defined(AHA152X_DEBUG)
	char *locker;	/* which function has the lock */
	int lockerl;	/* where did it get it */

	int debug;	/* current debugging setting */
#endif

#if defined(AHA152X_STAT)
	int           total_commands;
	int	      disconnections;
	int	      busfree_without_any_action;
	int	      busfree_without_old_command;
	int	      busfree_without_new_command;
	int	      busfree_without_done_command;
	int	      busfree_with_check_condition;
	int           count[maxstate];
	int           count_trans[maxstate];
	unsigned long time[maxstate];
#endif

	int commands;		/* current number of commands */

	int reconnect;		/* disconnection allowed */
	int parity;		/* parity checking enabled */
	int synchronous;	/* synchronous transferes enabled */
	int delay;		/* reset out delay */
	int ext_trans;		/* extended translation enabled */

	int swint; 		/* software-interrupt was fired during detect() */
	int service;		/* bh needs to be run */
	int in_intr;		/* bh is running */

	/* current state,
	   previous state,
	   last state different from current state */
	enum aha152x_state state, prevstate, laststate;

	int target;
		/* reconnecting target */

	unsigned char syncrate[8];
		/* current synchronous transfer agreements */

	unsigned char syncneg[8];
		/* 0: no negotiation;
		 * 1: negotiation in progress;
		 * 2: negotiation completed
		 */

	int cmd_i;
		/* number of sent bytes of current command */

	int msgi_len;
		/* number of received message bytes */
	unsigned char msgi[256];
		/* received message bytes */

	int msgo_i, msgo_len;	
		/* number of sent bytes and length of current messages */
	unsigned char msgo[256];
		/* pending messages */

	int data_len;
		/* number of sent/received bytes in dataphase */

	unsigned long io_port0;
	unsigned long io_port1;
};


/*
 * host specific command extension
 *
 */
struct aha152x_scdata {
	Scsi_Cmnd *next;	/* next sc in queue */
	Scsi_Cmnd *done;	/* done command */
	struct semaphore *sem;	/* semaphore to block on */
};


/* access macros for hostdata */

#define HOSTDATA(shpnt)		((struct aha152x_hostdata *) &shpnt->hostdata)

#define HOSTNO			((shpnt)->host_no)

#define CURRENT_SC		(HOSTDATA(shpnt)->current_SC)
#define DONE_SC			(HOSTDATA(shpnt)->done_SC)
#define ISSUE_SC		(HOSTDATA(shpnt)->issue_SC)
#define DISCONNECTED_SC		(HOSTDATA(shpnt)->disconnected_SC)
#define QLOCK			(HOSTDATA(shpnt)->lock)
#define QLOCKER			(HOSTDATA(shpnt)->locker)
#define QLOCKERL		(HOSTDATA(shpnt)->lockerl)

#define STATE			(HOSTDATA(shpnt)->state)
#define PREVSTATE		(HOSTDATA(shpnt)->prevstate)
#define LASTSTATE		(HOSTDATA(shpnt)->laststate)

#define RECONN_TARGET		(HOSTDATA(shpnt)->target)

#define CMD_I			(HOSTDATA(shpnt)->cmd_i)

#define MSGO(i)			(HOSTDATA(shpnt)->msgo[i])
#define MSGO_I			(HOSTDATA(shpnt)->msgo_i)
#define MSGOLEN			(HOSTDATA(shpnt)->msgo_len)
#define ADDMSGO(x)		(MSGOLEN<256 ? MSGO(MSGOLEN++)=x : aha152x_error(shpnt,"MSGO overflow"))

#define MSGI(i)			(HOSTDATA(shpnt)->msgi[i])
#define MSGILEN			(HOSTDATA(shpnt)->msgi_len)
#define ADDMSGI(x)		(MSGILEN<256 ? MSGI(MSGILEN++)=x : aha152x_error(shpnt,"MSGI overflow"))

#define DATA_LEN		(HOSTDATA(shpnt)->data_len)

#define SYNCRATE		(HOSTDATA(shpnt)->syncrate[CURRENT_SC->target])
#define SYNCNEG			(HOSTDATA(shpnt)->syncneg[CURRENT_SC->target])

#define DELAY			(HOSTDATA(shpnt)->delay)
#define EXT_TRANS		(HOSTDATA(shpnt)->ext_trans)
#define TC1550			(HOSTDATA(shpnt)->tc1550)
#define RECONNECT		(HOSTDATA(shpnt)->reconnect)
#define PARITY			(HOSTDATA(shpnt)->parity)
#define SYNCHRONOUS		(HOSTDATA(shpnt)->synchronous)

#define HOSTIOPORT0		(HOSTDATA(shpnt)->io_port0)
#define HOSTIOPORT1		(HOSTDATA(shpnt)->io_port1)

#define SCDATA(SCpnt)		((struct aha152x_scdata *) (SCpnt)->host_scribble)
#define SCNEXT(SCpnt)		SCDATA(SCpnt)->next
#define SCDONE(SCpnt)		SCDATA(SCpnt)->done
#define SCSEM(SCpnt)		SCDATA(SCpnt)->sem


/* state handling */
static void seldi_run(struct Scsi_Host *shpnt);
static void seldo_run(struct Scsi_Host *shpnt);
static void selto_run(struct Scsi_Host *shpnt);
static void busfree_run(struct Scsi_Host *shpnt);

static void msgo_init(struct Scsi_Host *shpnt);
static void msgo_run(struct Scsi_Host *shpnt);
static void msgo_end(struct Scsi_Host *shpnt);

static void cmd_init(struct Scsi_Host *shpnt);
static void cmd_run(struct Scsi_Host *shpnt);
static void cmd_end(struct Scsi_Host *shpnt);

static void datai_init(struct Scsi_Host *shpnt);
static void datai_run(struct Scsi_Host *shpnt);
static void datai_end(struct Scsi_Host *shpnt);

static void datao_init(struct Scsi_Host *shpnt);
static void datao_run(struct Scsi_Host *shpnt);
static void datao_end(struct Scsi_Host *shpnt);

static void status_run(struct Scsi_Host *shpnt);

static void msgi_run(struct Scsi_Host *shpnt);
static void msgi_end(struct Scsi_Host *shpnt);

static void parerr_run(struct Scsi_Host *shpnt);
static void rsti_run(struct Scsi_Host *shpnt);

static void complete(struct Scsi_Host *shpnt);

/*
 * driver states
 *
 */
static struct {
	char		*name;
	void		(*init)(struct Scsi_Host *);
	void		(*run)(struct Scsi_Host *);
	void		(*end)(struct Scsi_Host *);
	int		spio;
} states[] = {
	{ "idle",	0,		0,		0,		0},
	{ "unknown",	0,		0,		0,		0},
	{ "seldo",	0,		seldo_run,	0,		0},
	{ "seldi",	0,		seldi_run,	0,		0},
	{ "selto",	0,		selto_run,	0,		0},
	{ "busfree",	0,		busfree_run,	0,		0},
	{ "msgo",	msgo_init,	msgo_run,	msgo_end,	1},
	{ "cmd",	cmd_init,	cmd_run,	cmd_end,	1},
	{ "msgi",	0,		msgi_run,	msgi_end,	1},
	{ "status",	0,		status_run,	0,		1},
	{ "datai",	datai_init,	datai_run,	datai_end,	0},
	{ "datao",	datao_init,	datao_run,	datao_end,	0},
	{ "parerr",	0,		parerr_run,	0,		0},
	{ "rsti",	0,		rsti_run,	0,		0},
};

/* setup & interrupt */
static void intr(int irq, void *dev_id, struct pt_regs *);
static void reset_ports(struct Scsi_Host *shpnt);
static void aha152x_error(struct Scsi_Host *shpnt, char *msg);
static void done(struct Scsi_Host *shpnt, int error);
static int checksetup(struct aha152x_setup *setup);

/* diagnostics */
static void disp_ports(struct Scsi_Host *shpnt);
static void show_command(Scsi_Cmnd * ptr);
static void show_queues(struct Scsi_Host *shpnt);
static void disp_enintr(struct Scsi_Host *shpnt);

/* possible i/o addresses for the AIC-6260; default first */
static unsigned short ports[] = { 0x340, 0x140 };
#define PORT_COUNT (sizeof(ports) / sizeof(unsigned short))

#if !defined(SKIP_BIOSTEST)
/* possible locations for the Adaptec BIOS; defaults first */
static unsigned int addresses[] =
{
	0xdc000,		/* default first */
	0xc8000,
	0xcc000,
	0xd0000,
	0xd4000,
	0xd8000,
	0xe0000,
	0xeb800,		/* VTech Platinum SMP */
	0xf0000,
};
#define ADDRESS_COUNT (sizeof(addresses) / sizeof(unsigned int))

/* signatures for various AIC-6[23]60 based controllers.
   The point in detecting signatures is to avoid useless and maybe
   harmful probes on ports. I'm not sure that all listed boards pass
   auto-configuration. For those which fail the BIOS signature is
   obsolete, because user intervention to supply the configuration is
   needed anyway.  May be an information whether or not the BIOS supports
   extended translation could be also useful here. */
static struct signature {
	unsigned char *signature;
	int sig_offset;
	int sig_length;
} signatures[] =
{
	{ "Adaptec AHA-1520 BIOS",	0x102e, 21 },
		/* Adaptec 152x */
	{ "Adaptec AHA-1520B",		0x000b, 17 },
		/* Adaptec 152x rev B */
	{ "Adaptec AHA-1520B",		0x0026, 17 },
		/* Iomega Jaz Jet ISA (AIC6370Q) */
	{ "Adaptec ASW-B626 BIOS",	0x1029, 21 },
		/* on-board controller */
	{ "Adaptec BIOS: ASW-B626",	0x000f, 22 },
		/* on-board controller */
	{ "Adaptec ASW-B626 S2",	0x2e6c, 19 },
		/* on-board controller */
	{ "Adaptec BIOS:AIC-6360",	0x000c, 21 },
		/* on-board controller */
	{ "ScsiPro SP-360 BIOS",	0x2873, 19 },
		/* ScsiPro-Controller  */
	{ "GA-400 LOCAL BUS SCSI BIOS", 0x102e, 26 },
		/* Gigabyte Local-Bus-SCSI */
	{ "Adaptec BIOS:AVA-282X",	0x000c, 21 },
		/* Adaptec 282x */
	{ "Adaptec IBM Dock II SCSI",	0x2edd, 24 },
		/* IBM Thinkpad Dock II */
	{ "Adaptec BIOS:AHA-1532P",	0x001c, 22 },
		/* IBM Thinkpad Dock II SCSI */
	{ "DTC3520A Host Adapter BIOS", 0x318a, 26 },
		/* DTC 3520A ISA SCSI */
};

#define SIGNATURE_COUNT (sizeof(signatures) / sizeof(struct signature))
#endif


/*
 *  queue services:
 *
 */
static inline void append_SC(Scsi_Cmnd **SC, Scsi_Cmnd *new_SC)
{
	Scsi_Cmnd *end;

	SCNEXT(new_SC) = NULL;
	if (!*SC)
		*SC = new_SC;
	else {
		for (end = *SC; SCNEXT(end); end = SCNEXT(end))
			;
		SCNEXT(end) = new_SC;
	}
}

static inline Scsi_Cmnd *remove_first_SC(Scsi_Cmnd ** SC)
{
	Scsi_Cmnd *ptr;

	ptr = *SC;
	if (ptr) {
		*SC = SCNEXT(*SC);
		SCNEXT(ptr)=NULL;
	}
	return ptr;
}

static inline Scsi_Cmnd *remove_lun_SC(Scsi_Cmnd ** SC, int target, int lun)
{
	Scsi_Cmnd *ptr, *prev;

	for (ptr = *SC, prev = NULL;
	     ptr && ((ptr->target != target) || (ptr->lun != lun));
	     prev = ptr, ptr = SCNEXT(ptr))
	     ;

	if (ptr) {
		if (prev)
			SCNEXT(prev) = SCNEXT(ptr);
		else
			*SC = SCNEXT(ptr);

		SCNEXT(ptr)=NULL;
	}

	return ptr;
}

static inline Scsi_Cmnd *remove_SC(Scsi_Cmnd **SC, Scsi_Cmnd *SCp)
{
	Scsi_Cmnd *ptr, *prev;

	for (ptr = *SC, prev = NULL;
	     ptr && SCp!=ptr;
	     prev = ptr, ptr = SCNEXT(ptr))
	     ;

	if (ptr) {
		if (prev)
			SCNEXT(prev) = SCNEXT(ptr);
		else
			*SC = SCNEXT(ptr);

		SCNEXT(ptr)=NULL;
	}

	return ptr;
}

#if defined(PCMCIA) || !defined(MODULE)
void aha152x_setup(char *str, int *ints)
{
	if(setup_count>2) {
		printk(KERN_ERR "aha152x: you can only configure up to two controllers\n");
		return;
	}

	setup[setup_count].conf        = str;
	setup[setup_count].io_port     = ints[0] >= 1 ? ints[1] : 0x340;
	setup[setup_count].irq         = ints[0] >= 2 ? ints[2] : 11;
	setup[setup_count].scsiid      = ints[0] >= 3 ? ints[3] : 7;
	setup[setup_count].reconnect   = ints[0] >= 4 ? ints[4] : 1;
	setup[setup_count].parity      = ints[0] >= 5 ? ints[5] : 1;
	setup[setup_count].synchronous = ints[0] >= 6 ? ints[6] : 1;
	setup[setup_count].delay       = ints[0] >= 7 ? ints[7] : DELAY_DEFAULT;
	setup[setup_count].ext_trans   = ints[0] >= 8 ? ints[8] : 0;
#if defined(AHA152X_DEBUG)
	setup[setup_count].debug       = ints[0] >= 9 ? ints[9] : DEBUG_DEFAULT;
	if (ints[0] > 9) {
		printk(KERN_NOTICE "aha152x: usage: aha152x=<IOBASE>[,<IRQ>[,<SCSI ID>"
		       "[,<RECONNECT>[,<PARITY>[,<SYNCHRONOUS>[,<DELAY>[,<EXT_TRANS>[,<DEBUG>]]]]]]]]\n");
#else
	if (ints[0] > 8) {                                                /*}*/
		printk(KERN_NOTICE "aha152x: usage: aha152x=<IOBASE>[,<IRQ>[,<SCSI ID>"
		       "[,<RECONNECT>[,<PARITY>[,<SYNCHRONOUS>[,<DELAY>[,<EXT_TRANS>]]]]]]]\n");
#endif
		return;
	} else {
		setup_count++;
		return;
	}
}
#endif

#if !defined(MODULE)
static int __init do_setup(char *str)
{

#if defined(AHA152X_DEBUG)
	int ints[11];
#else
	int ints[10];
#endif
	int count=setup_count;

	get_options(str, sizeof(ints)/sizeof(int), ints);
	aha152x_setup(str,ints);

	return count<setup_count;
}

__setup("aha152x=", do_setup);
#endif

/*
 * Test, if port_base is valid.
 *
 */
static int aha152x_porttest(int io_port)
{
	int i;

	if (check_region(io_port, IO_RANGE))
		return 0;

	SETPORT(io_port + O_DMACNTRL1, 0);	/* reset stack pointer */
	for (i = 0; i < 16; i++)
		SETPORT(io_port + O_STACK, i);

	SETPORT(io_port + O_DMACNTRL1, 0);	/* reset stack pointer */
	for (i = 0; i < 16 && GETPORT(io_port + O_STACK) == i; i++)
		;

	return (i == 16);
}

static int tc1550_porttest(int io_port)
{
	int i;

	if (check_region(io_port, IO_RANGE))
		return 0;

	SETPORT(io_port + O_TC_DMACNTRL1, 0);	/* reset stack pointer */
	for (i = 0; i < 16; i++)
		SETPORT(io_port + O_STACK, i);

	SETPORT(io_port + O_TC_DMACNTRL1, 0);	/* reset stack pointer */
	for (i = 0; i < 16 && GETPORT(io_port + O_TC_STACK) == i; i++)
		;

	return (i == 16);
}

static int checksetup(struct aha152x_setup *setup)
{

#if !defined(PCMCIA)
	int i;
	for (i = 0; i < PORT_COUNT && (setup->io_port != ports[i]); i++)
		;

	if (i == PORT_COUNT)
		return 0;
#endif

	if(aha152x_porttest(setup->io_port)) {
          setup->tc1550=0;
        } else if(tc1550_porttest(setup->io_port)) {
          setup->tc1550=1;
        } else
          return 0;

	if ((setup->irq < IRQ_MIN) || (setup->irq > IRQ_MAX))
		return 0;

	if ((setup->scsiid < 0) || (setup->scsiid > 7))
		return 0;

	if ((setup->reconnect < 0) || (setup->reconnect > 1))
		return 0;

	if ((setup->parity < 0) || (setup->parity > 1))
		return 0;

	if ((setup->synchronous < 0) || (setup->synchronous > 1))
		return 0;

	if ((setup->ext_trans < 0) || (setup->ext_trans > 1))
		return 0;


	return 1;
}

static void swintr(int irqno, void *dev_id, struct pt_regs *regs)
{
	struct Scsi_Host *shpnt = aha152x_host[irqno - IRQ_MIN];

	if (!shpnt)
        	printk(KERN_ERR "aha152x%d: catched software interrupt for unknown controller.\n", HOSTNO);

	HOSTDATA(shpnt)->swint++;
}


int aha152x_detect(Scsi_Host_Template * tpnt)
{
	int i, j, ok;
#if defined(AUTOCONF)
	aha152x_config conf;
#endif
	tpnt->proc_name = "aha152x"; 

	for (i = 0; i < IRQS; i++)
		aha152x_host[i] = (struct Scsi_Host *) NULL;

	if (setup_count) {
		printk(KERN_INFO "aha152x: processing commandline: ");

		for (i = 0; i < setup_count; i++)
			if (!checksetup(&setup[i])) {
				printk(KERN_ERR "\naha152x: %s\n", setup[i].conf);
				printk(KERN_ERR "aha152x: invalid line\n");
			}
		printk("ok\n");
	}
#if defined(SETUP0)
	if (setup_count < 2) {
		struct aha152x_setup override = SETUP0;

		if (setup_count == 0 || (override.io_port != setup[0].io_port)) {
			if (!checksetup(&override)) {
				printk(KERN_ERR "\naha152x: invalid override SETUP0={0x%x,%d,%d,%d,%d,%d,%d,%d}\n",
				       override.io_port,
				       override.irq,
				       override.scsiid,
				       override.reconnect,
				       override.parity,
				       override.synchronous,
				       override.delay,
				       override.ext_trans);
			} else
				setup[setup_count++] = override;
		}
	}
#endif

#if defined(SETUP1)
	if (setup_count < 2) {
		struct aha152x_setup override = SETUP1;

		if (setup_count == 0 || (override.io_port != setup[0].io_port)) {
			if (!checksetup(&override)) {
				printk(KERN_ERR "\naha152x: invalid override SETUP1={0x%x,%d,%d,%d,%d,%d,%d,%d}\n",
				       override.io_port,
				       override.irq,
				       override.scsiid,
				       override.reconnect,
				       override.parity,
				       override.synchronous,
				       override.delay,
				       override.ext_trans);
			} else
				setup[setup_count++] = override;
		}
	}
#endif

#if defined(MODULE)
	if (setup_count<2 && (aha152x[0]!=0 || io[0]!=0 || irq[0]!=0)) {
		if(aha152x[0]!=0) {
			setup[setup_count].conf        = "";
			setup[setup_count].io_port     = aha152x[0];
			setup[setup_count].irq         = aha152x[1];
			setup[setup_count].scsiid      = aha152x[2];
			setup[setup_count].reconnect   = aha152x[3];
			setup[setup_count].parity      = aha152x[4];
			setup[setup_count].synchronous = aha152x[5];
			setup[setup_count].delay       = aha152x[6];
			setup[setup_count].ext_trans   = aha152x[7];
#if defined(AHA152X_DEBUG)
			setup[setup_count].debug       = aha152x[8];
#endif
	  	} else if(io[0]!=0 || irq[0]!=0) {
			if(io[0]!=0)  setup[setup_count].io_port = io[0];
			if(irq[0]!=0) setup[setup_count].irq     = irq[0];

	    		setup[setup_count].scsiid      = scsiid[0];
	    		setup[setup_count].reconnect   = reconnect[0];
	    		setup[setup_count].parity      = parity[0];
	    		setup[setup_count].synchronous = sync[0];
	    		setup[setup_count].delay       = delay[0];
	    		setup[setup_count].ext_trans   = exttrans[0];
#if defined(AHA152X_DEBUG)
			setup[setup_count].debug       = debug[0];
#endif
		}

          	if (checksetup(&setup[setup_count]))
			setup_count++;
		else
			printk(KERN_ERR "aha152x: invalid module params io=0x%x, irq=%d,scsiid=%d,reconnect=%d,parity=%d,sync=%d,delay=%d,exttrans=%d\n",
			       setup[setup_count].io_port,
			       setup[setup_count].irq,
			       setup[setup_count].scsiid,
			       setup[setup_count].reconnect,
			       setup[setup_count].parity,
			       setup[setup_count].synchronous,
			       setup[setup_count].delay,
			       setup[setup_count].ext_trans);
	}

	if (setup_count < 2 && (aha152x1[0]!=0 || io[1]!=0 || irq[1]!=0)) {
		if(aha152x1[0]!=0) {
			setup[setup_count].conf        = "";
			setup[setup_count].io_port     = aha152x1[0];
			setup[setup_count].irq         = aha152x1[1];
			setup[setup_count].scsiid      = aha152x1[2];
			setup[setup_count].reconnect   = aha152x1[3];
			setup[setup_count].parity      = aha152x1[4];
			setup[setup_count].synchronous = aha152x1[5];
			setup[setup_count].delay       = aha152x1[6];
			setup[setup_count].ext_trans   = aha152x1[7];
#if defined(AHA152X_DEBUG)
			setup[setup_count].debug       = aha152x1[8];
#endif
	  	} else if(io[1]!=0 || irq[1]!=0) {
			if(io[1]!=0)  setup[setup_count].io_port = io[1];
			if(irq[1]!=0) setup[setup_count].irq     = irq[1];

	    		setup[setup_count].scsiid      = scsiid[1];
	    		setup[setup_count].reconnect   = reconnect[1];
	    		setup[setup_count].parity      = parity[1];
	    		setup[setup_count].synchronous = sync[1];
	    		setup[setup_count].delay       = delay[1];
	    		setup[setup_count].ext_trans   = exttrans[1];
#if defined(AHA152X_DEBUG)
			setup[setup_count].debug       = debug[1];
#endif
		}
		if (checksetup(&setup[setup_count]))
			setup_count++;
		else
			printk(KERN_ERR "aha152x: invalid module params io=0x%x, irq=%d,scsiid=%d,reconnect=%d,parity=%d,sync=%d,delay=%d,exttrans=%d\n",
			       setup[setup_count].io_port,
			       setup[setup_count].irq,
			       setup[setup_count].scsiid,
			       setup[setup_count].reconnect,
			       setup[setup_count].parity,
			       setup[setup_count].synchronous,
			       setup[setup_count].delay,
			       setup[setup_count].ext_trans);
	}
#endif

#if defined(AUTOCONF)
	if (setup_count < 2) {
#if !defined(SKIP_BIOSTEST)
		ok = 0;
		for (i = 0; i < ADDRESS_COUNT && !ok; i++)
			for (j = 0; (j < SIGNATURE_COUNT) && !ok; j++)
				ok = isa_check_signature(addresses[i] + signatures[j].sig_offset,
								signatures[j].signature, signatures[j].sig_length);

		if (!ok && setup_count == 0)
			return 0;

		printk(KERN_INFO "aha152x: BIOS test: passed, ");
#else
		printk(KERN_INFO "aha152x: ");
#endif				/* !SKIP_BIOSTEST */

		ok = 0;
		for (i = 0; i < PORT_COUNT && setup_count < 2; i++) {
			if ((setup_count == 1) && (setup[0].io_port == ports[i]))
				continue;

			if (aha152x_porttest(ports[i])) {
				ok++;
				setup[setup_count].io_port = ports[i];
				setup[setup_count].tc1550  = 0;

				conf.cf_port =
				    (GETPORT(ports[i] + O_PORTA) << 8) + GETPORT(ports[i] + O_PORTB);

				setup[setup_count].irq = IRQ_MIN + conf.cf_irq;
				setup[setup_count].scsiid = conf.cf_id;
				setup[setup_count].reconnect = conf.cf_tardisc;
				setup[setup_count].parity = !conf.cf_parity;
				setup[setup_count].synchronous = conf.cf_syncneg;
				setup[setup_count].delay = DELAY_DEFAULT;
				setup[setup_count].ext_trans = 0;
#if defined(AHA152X_DEBUG)
				setup[setup_count].debug = DEBUG_DEFAULT;
#endif
				setup_count++;
			} else if (tc1550_porttest(ports[i])) {
				ok++;
				setup[setup_count].io_port = ports[i];
				setup[setup_count].tc1550  = 1;

				conf.cf_port =
				    (GETPORT(ports[i] + O_PORTA) << 8) + GETPORT(ports[i] + O_PORTB);

				setup[setup_count].irq = IRQ_MIN + conf.cf_irq;
				setup[setup_count].scsiid = conf.cf_id;
				setup[setup_count].reconnect = conf.cf_tardisc;
				setup[setup_count].parity = !conf.cf_parity;
				setup[setup_count].synchronous = conf.cf_syncneg;
				setup[setup_count].delay = DELAY_DEFAULT;
				setup[setup_count].ext_trans = 0;
#if defined(AHA152X_DEBUG)
				setup[setup_count].debug = DEBUG_DEFAULT;
#endif
				setup_count++;
			}
		}

		if (ok)
			printk("auto configuration: ok, ");
	}
#endif

	printk("detected %d controller(s)\n", setup_count);

	for (i=0; i<setup_count; i++) {
		struct Scsi_Host *shpnt;

		aha152x_host[setup[i].irq - IRQ_MIN] = shpnt =
		    scsi_register(tpnt, sizeof(struct aha152x_hostdata));

		if(!shpnt) {
			printk(KERN_ERR "aha152x: scsi_register failed\n");
			continue;
		}

		registered_count++;

		shpnt->io_port   = setup[i].io_port;
		shpnt->n_io_port = IO_RANGE;
		shpnt->irq       = setup[i].irq;

		if(!setup[i].tc1550) {
			HOSTIOPORT0 = setup[i].io_port;
			HOSTIOPORT1 = setup[i].io_port;
		} else {
			HOSTIOPORT0 = setup[i].io_port+0x10;
			HOSTIOPORT1 = setup[i].io_port-0x10;
		}

		ISSUE_SC	= 0;
		CURRENT_SC	= 0;
		DONE_SC		= 0;
		DISCONNECTED_SC	= 0;

		QLOCK		= SPIN_LOCK_UNLOCKED;

		STATE		= 0;
		PREVSTATE	= 0;
		LASTSTATE	= 0;

		MSGILEN		= 0;
		MSGOLEN		= 0;

		RECONNECT	= setup[i].reconnect;
		SYNCHRONOUS	= setup[i].synchronous;
		PARITY		= setup[i].parity;
		DELAY		= setup[i].delay;
		EXT_TRANS	= setup[i].ext_trans;
#if defined(AHA152X_DEBUG)
		HOSTDATA(shpnt)->debug = setup[i].debug;
#endif
		HOSTDATA(shpnt)->in_intr = 0;
		HOSTDATA(shpnt)->commands = 0;

#if defined(AHA152X_STAT)
		HOSTDATA(shpnt)->total_commands=0;
		HOSTDATA(shpnt)->disconnections=0;
		HOSTDATA(shpnt)->busfree_without_any_action=0;
		HOSTDATA(shpnt)->busfree_without_old_command=0;
		HOSTDATA(shpnt)->busfree_without_new_command=0;
		HOSTDATA(shpnt)->busfree_without_done_command=0;
		HOSTDATA(shpnt)->busfree_with_check_condition=0;
		for (j = idle; j<maxstate; j++) {
			HOSTDATA(shpnt)->count[j]=0;
			HOSTDATA(shpnt)->count_trans[j]=0;
			HOSTDATA(shpnt)->time[j]=0;
		}
#endif

		for (j = 0; j < 8; j++) {
			HOSTDATA(shpnt)->syncrate[j] = 0;
			HOSTDATA(shpnt)->syncneg[j] = 0;
		}

		SETPORT(SCSIID, setup[i].scsiid << 4);
		shpnt->this_id = setup[i].scsiid;

		if (setup[i].reconnect)
			shpnt->can_queue = AHA152X_MAXQUEUE;

#if 0
		if(!shpnt->hostt->use_new_eh_code) {
#endif
			/* RESET OUT */
			printk("aha152x: resetting bus...\n");
			SETPORT(SCSISEQ, SCSIRSTO);
			mdelay(256);
			SETPORT(SCSISEQ, 0);
			mdelay(DELAY);
#if 0
		}
#endif

		reset_ports(shpnt);

		printk(KERN_INFO
		       "aha152x%d%s: "
		       "vital data: rev=%x, "
		       "io=0x%03lx (0x%03lx/0x%03lx), "
		       "irq=%d, "
		       "scsiid=%d, "
		       "reconnect=%s, "
		       "parity=%s, "
		       "synchronous=%s, "
		       "delay=%d, "
		       "extended translation=%s\n",
		       HOSTNO, setup[i].tc1550 ? " (tc1550 mode)" : "",
		       GETPORT(REV) & 0x7,
		       shpnt->io_port, HOSTIOPORT0, HOSTIOPORT1,
		       shpnt->irq,
		       shpnt->this_id,
		       RECONNECT ? "enabled" : "disabled",
		       PARITY ? "enabled" : "disabled",
		       SYNCHRONOUS ? "enabled" : "disabled",
		       DELAY,
		       EXT_TRANS ? "enabled" : "disabled");

		request_region(shpnt->io_port, IO_RANGE, "aha152x");

		/* not expecting any interrupts */
		SETPORT(SIMODE0, 0);
		SETPORT(SIMODE1, 0);

		ok = request_irq(shpnt->irq, swintr, SA_INTERRUPT, "aha152x", shpnt);
		if (ok < 0) {
			if (ok==-EINVAL)
				printk(KERN_ERR "aha152x%d: bad IRQ %d.\n", HOSTNO, shpnt->irq);
			else if(ok==-EBUSY)
				printk(KERN_ERR "aha152x%d: IRQ %d already in use.\n", HOSTNO, shpnt->irq);
			else
				printk(KERN_ERR "aha152x%d: Unexpected error code %d on requesting IRQ %d.\n", HOSTNO, ok, shpnt->irq);

			printk(KERN_ERR "aha152x%d: driver needs an IRQ.\n", HOSTNO);

			scsi_unregister(shpnt);
			registered_count--;
			release_region(shpnt->io_port, IO_RANGE);
			aha152x_host[shpnt->irq - IRQ_MIN] = 0;
			shpnt = 0;
			continue;
		}
		HOSTDATA(shpnt)->swint = 0;

		printk(KERN_INFO "aha152x%d: trying software interrupt, ", HOSTNO);
		SETPORT(DMACNTRL0, SWINT|INTEN);
		spin_unlock_irq(&io_request_lock);
		mdelay(1000);
		spin_lock_irq(&io_request_lock);
		free_irq(shpnt->irq, shpnt);

		if (!HOSTDATA(shpnt)->swint) {
			if (TESTHI(DMASTAT, INTSTAT)) {
				printk("lost.\n");
			} else {
				printk("failed.\n");
			}

			printk(KERN_ERR "aha152x%d: IRQ %d possibly wrong.  Please verify.\n", HOSTNO, shpnt->irq);

			registered_count--;
			release_region(shpnt->io_port, IO_RANGE);
			aha152x_host[shpnt->irq - IRQ_MIN] = 0;
			scsi_unregister(shpnt);
			shpnt=NULL;
			continue;
		}
		printk("ok.\n");

		SETPORT(DMACNTRL0, INTEN);

		/* clear interrupts */
		SETPORT(SSTAT0, 0x7f);
		SETPORT(SSTAT1, 0xef);

		if (request_irq(shpnt->irq, intr, SA_INTERRUPT, "aha152x", shpnt) < 0) {
			printk(KERN_ERR "aha152x%d: failed to reassign interrupt.\n", HOSTNO);

			scsi_unregister(shpnt);
			registered_count--;
			release_region(shpnt->io_port, IO_RANGE);
			shpnt = aha152x_host[shpnt->irq - IRQ_MIN] = 0;
			continue;
		}
	}

	return registered_count>0;
}


int aha152x_release(struct Scsi_Host *shpnt)
{
	if (shpnt->irq)
		free_irq(shpnt->irq, shpnt);

	if (shpnt->io_port)
		release_region(shpnt->io_port, IO_RANGE);

	scsi_unregister(shpnt);

	return 0;
}

/*
 * setup controller to generate interrupts depending
 * on current state (lock has to be acquired)
 *
 */ 
static int setup_expected_interrupts(struct Scsi_Host *shpnt)
{
	ASSERT_LOCK(&QLOCK,1);

	if(CURRENT_SC) {
		CURRENT_SC->SCp.phase |= 1 << 16;
	
		if(CURRENT_SC->SCp.phase & selecting) {
			DPRINTK(debug_intr, DEBUG_LEAD "expecting: (seldo) (seltimo) (seldi)\n", CMDINFO(CURRENT_SC));
			SETPORT(SSTAT1, SELTO);
			SETPORT(SIMODE0, ENSELDO | (DISCONNECTED_SC ? ENSELDI : 0));
			SETPORT(SIMODE1, ENSELTIMO);
		} else {
			DPRINTK(debug_intr, DEBUG_LEAD "expecting: (phase change) (busfree) %s\n", CMDINFO(CURRENT_SC), CURRENT_SC->SCp.phase & spiordy ? "(spiordy)" : "");
			SETPORT(SIMODE0, (CURRENT_SC->SCp.phase & spiordy) ? ENSPIORDY : 0);
			SETPORT(SIMODE1, ENPHASEMIS | ENSCSIRST | ENSCSIPERR | ENBUSFREE); 
		}
	} else if(STATE==seldi) {
		DPRINTK(debug_intr, DEBUG_LEAD "expecting: (phase change) (identify)\n", CMDINFO(CURRENT_SC));
		SETPORT(SIMODE0, 0);
		SETPORT(SIMODE1, ENPHASEMIS | ENSCSIRST | ENSCSIPERR | ENBUSFREE); 
	} else {
		DPRINTK(debug_intr, DEBUG_LEAD "expecting: %s %s\n",
			CMDINFO(CURRENT_SC),
			DISCONNECTED_SC ? "(reselection)" : "",
			ISSUE_SC ? "(busfree)" : "");
		SETPORT(SIMODE0, DISCONNECTED_SC ? ENSELDI : 0);
		SETPORT(SIMODE1, ENSCSIRST | ( (ISSUE_SC||DONE_SC) ? ENBUSFREE : 0));
	}

	if(!HOSTDATA(shpnt)->in_intr)
		SETBITS(DMACNTRL0, INTEN);

	return TESTHI(DMASTAT, INTSTAT);
}


/* 
 *  Queue a command and setup interrupts for a free bus.
 */
int aha152x_internal_queue(Scsi_Cmnd *SCpnt, struct semaphore *sem, int phase, Scsi_Cmnd *done_SC, void (*done)(Scsi_Cmnd *))
{
	struct Scsi_Host *shpnt = SCpnt->host;
	unsigned long flags;

#if defined(AHA152X_DEBUG)
	if (HOSTDATA(shpnt)->debug & debug_queue) {
		printk(INFO_LEAD "queue: cmd_len=%d pieces=%d size=%u cmnd=",
		       CMDINFO(SCpnt), SCpnt->cmd_len, SCpnt->use_sg, SCpnt->request_bufflen);
		print_command(SCpnt->cmnd);
	}
#endif

	SCpnt->scsi_done	= done;
	SCpnt->resid 		= SCpnt->request_bufflen;
	SCpnt->SCp.phase	= not_issued | phase;
	SCpnt->SCp.Status	= CHECK_CONDITION;
	SCpnt->SCp.Message	= 0;
	SCpnt->SCp.have_data_in	= 0;
	SCpnt->SCp.sent_command	= 0;
	SCpnt->host_scribble    = kmalloc(sizeof(struct aha152x_scdata), GFP_ATOMIC);
	if(!SCpnt->host_scribble) {
		printk(ERR_LEAD "allocation failed\n", CMDINFO(SCpnt));
		return FAILED;
	}

	SCNEXT(SCpnt)		= 0;
	SCDONE(SCpnt)		= done_SC;
	SCSEM(SCpnt)		= sem;

	/* setup scratch area
	   SCp.ptr              : buffer pointer
	   SCp.this_residual    : buffer length
	   SCp.buffer           : next buffer
	   SCp.buffers_residual : left buffers in list
	   SCp.phase            : current state of the command */
	if (SCpnt->use_sg) {
		SCpnt->SCp.buffer           = (struct scatterlist *) SCpnt->request_buffer;
		SCpnt->SCp.ptr              = SCpnt->SCp.buffer->address;
		SCpnt->SCp.this_residual    = SCpnt->SCp.buffer->length;
		SCpnt->SCp.buffers_residual = SCpnt->use_sg - 1;
	} else {
		SCpnt->SCp.ptr              = (char *) SCpnt->request_buffer;
		SCpnt->SCp.this_residual    = SCpnt->request_bufflen;
		SCpnt->SCp.buffer           = NULL;
		SCpnt->SCp.buffers_residual = 0;
	}

	DO_LOCK(flags);

#if defined(AHA152X_STAT)
	HOSTDATA(shpnt)->total_commands++;
#endif

	/* Turn led on, when this is the first command. */
	HOSTDATA(shpnt)->commands++;
	if (HOSTDATA(shpnt)->commands==1)
		SETPORT(PORTA, 1);

	append_SC(&ISSUE_SC, SCpnt);

	if(!HOSTDATA(shpnt)->in_intr)
		setup_expected_interrupts(shpnt);

	DO_UNLOCK(flags);

	return 0;
}

int aha152x_queue(Scsi_Cmnd *SCpnt, void (*done)(Scsi_Cmnd *))
{
	if(*SCpnt->cmnd == REQUEST_SENSE) {
		SCpnt->result = 0;
		done(SCpnt);

		return SUCCESS;
	}

	return aha152x_internal_queue(SCpnt, 0, 0, 0, done);
}


/*
 *  run a command
 *
 */
void internal_done(Scsi_Cmnd *SCpnt)
{
#if 0
	struct Scsi_Host *shpnt = SCpnt->host;

	DPRINTK(debug_eh, INFO_LEAD "internal_done called\n", CMDINFO(SCpnt));
#endif
	if(SCSEM(SCpnt))
		up(SCSEM(SCpnt));
}

int aha152x_command(Scsi_Cmnd * SCpnt)
{
	DECLARE_MUTEX_LOCKED(sem);

	aha152x_internal_queue(SCpnt, &sem, 0, 0, internal_done);
	down(&sem);

	return SUCCESS;
}

/*
 *  Abort a command
 *
 */
int aha152x_abort(Scsi_Cmnd *SCpnt)
{
	struct Scsi_Host *shpnt = SCpnt->host;
	Scsi_Cmnd *ptr;
	unsigned long flags;

	if(!shpnt) {
		printk(ERR_LEAD "abort(%p): no host structure\n", CMDINFO(SCpnt), SCpnt);
		return FAILED;
	}

#if defined(AHA152X_DEBUG)
	if(HOSTDATA(shpnt)->debug & debug_eh) {
		printk(DEBUG_LEAD "abort(%p)", CMDINFO(SCpnt), SCpnt);
		show_queues(shpnt);
		mdelay(1000);
	}
#endif

	DO_LOCK(flags);

	ptr=remove_SC(&ISSUE_SC, SCpnt);

	if(ptr) {
		DPRINTK(debug_eh, DEBUG_LEAD "not yet issued - SUCCESS\n", CMDINFO(SCpnt));

		HOSTDATA(shpnt)->commands--;
		if (!HOSTDATA(shpnt)->commands)
			SETPORT(PORTA, 0);
		DO_UNLOCK(flags);

		kfree(SCpnt->host_scribble);
		SCpnt->host_scribble=0;

		return SUCCESS;
	} 

	DO_UNLOCK(flags);

	/*
	 * FIXME:
	 * for current command: queue ABORT for message out and raise ATN
	 * for disconnected command: pseudo SC with ABORT message or ABORT on reselection?
	 *
	 */

	printk(ERR_LEAD "cannot abort running or disconnected command\n", CMDINFO(SCpnt));

	return FAILED;
}

static void timer_expired(unsigned long p)
{
	struct semaphore *sem = (void *)p;

	printk(KERN_INFO "aha152x: timer expired\n");
	up(sem);
}

/*
 * Reset a device
 *
 * FIXME: never seen this live. might lockup...
 *
 */
int aha152x_device_reset(Scsi_Cmnd * SCpnt)
{
	struct Scsi_Host *shpnt = SCpnt->host;
	DECLARE_MUTEX_LOCKED(sem);
	struct timer_list timer;
	Scsi_Cmnd cmnd;

#if defined(AHA152X_DEBUG)
	if(HOSTDATA(shpnt)->debug & debug_eh) {
		printk(INFO_LEAD "aha152x_device_reset(%p)", CMDINFO(SCpnt), SCpnt);
		show_queues(shpnt);
		mdelay(1000);
	}
#endif

	if(CURRENT_SC==SCpnt) {
		printk(ERR_LEAD "cannot reset current device\n", CMDINFO(SCpnt));
		return FAILED;
	}

	cmnd.cmd_len         = 0;
	cmnd.host            = SCpnt->host;
	cmnd.target          = SCpnt->target;
	cmnd.lun             = SCpnt->lun;
	cmnd.use_sg          = 0;
	cmnd.request_buffer  = 0;
	cmnd.request_bufflen = 0;

	init_timer(&timer);
	timer.data     = (unsigned long) &sem;
	timer.expires  = jiffies + 10 * HZ;   /* 10s */
	timer.function = (void (*)(unsigned long)) timer_expired;
	add_timer(&timer);

	aha152x_internal_queue(&cmnd, &sem, resetting, 0, internal_done);

	down(&sem);

	del_timer(&timer);

	if(cmnd.SCp.phase & resetted) {
		return SUCCESS;
	} else {
		return FAILED;
	}
}

void free_hard_reset_SCs(struct Scsi_Host *shpnt, Scsi_Cmnd **SCs)
{
	Scsi_Cmnd *ptr;
	unsigned long flags;

	DO_LOCK(flags);

	ptr=*SCs;
	while(ptr) {
		Scsi_Cmnd *next = SCNEXT(ptr);

		if (!ptr->device->soft_reset) {
			DPRINTK(debug_eh, DEBUG_LEAD "disconnected command %p removed\n", CMDINFO(ptr), ptr);
			remove_SC(SCs, ptr);
			HOSTDATA(shpnt)->commands--;
			kfree(ptr->host_scribble);
			ptr->host_scribble=0;
		}

		ptr = next;
	}

	DO_UNLOCK(flags);
}

/*
 * Reset the bus
 *
 */
int aha152x_bus_reset(Scsi_Cmnd *SCpnt)
{
	struct Scsi_Host *shpnt = SCpnt->host;
	unsigned long flags;

#if defined(AHA152X_DEBUG)
	if(HOSTDATA(shpnt)->debug & debug_eh) {
		printk(DEBUG_LEAD "aha152x_bus_reset(%p)", CMDINFO(SCpnt), SCpnt);
		show_queues(shpnt);
		mdelay(1000);
	}
#endif

	free_hard_reset_SCs(shpnt, &ISSUE_SC);
	free_hard_reset_SCs(shpnt, &DISCONNECTED_SC);

	DPRINTK(debug_eh, DEBUG_LEAD "resetting bus\n", CMDINFO(SCpnt));

	SETPORT(SCSISEQ, SCSIRSTO);
	mdelay(256);
	SETPORT(SCSISEQ, 0);
	mdelay(DELAY);

	DPRINTK(debug_eh, DEBUG_LEAD "bus reset returns\n", CMDINFO(SCpnt));

	DO_LOCK(flags);
	setup_expected_interrupts(shpnt);
	if(HOSTDATA(shpnt)->commands==0)
		SETPORT(PORTA, 0);
	DO_UNLOCK(flags);

	return SUCCESS;
}


/*
 *  Restore default values to the AIC-6260 registers and reset the fifos
 *
 */
static void reset_ports(struct Scsi_Host *shpnt)
{
	unsigned long flags;

	/* disable interrupts */
	SETPORT(DMACNTRL0, RSTFIFO);

	SETPORT(SCSISEQ, 0);

	SETPORT(SXFRCTL1, 0);
	SETPORT(SCSISIG, 0);
	SETRATE(0);

	/* clear all interrupt conditions */
	SETPORT(SSTAT0, 0x7f);
	SETPORT(SSTAT1, 0xef);

	SETPORT(SSTAT4, SYNCERR | FWERR | FRERR);

	SETPORT(DMACNTRL0, 0);
	SETPORT(DMACNTRL1, 0);

	SETPORT(BRSTCNTRL, 0xf1);

	/* clear SCSI fifos and transfer count */
	SETPORT(SXFRCTL0, CH1|CLRCH1|CLRSTCNT);
	SETPORT(SXFRCTL0, CH1);

	DO_LOCK(flags);
	setup_expected_interrupts(shpnt);
	DO_UNLOCK(flags);
}

/*
 * Reset the host (bus and controller)
 *
 */
int aha152x_host_reset(Scsi_Cmnd * SCpnt)
{
#if defined(AHA152X_DEBUG)
	struct Scsi_Host *shpnt = SCpnt->host;
#endif

	DPRINTK(debug_eh, DEBUG_LEAD "aha152x_host_reset(%p)\n", CMDINFO(SCpnt), SCpnt);

	aha152x_bus_reset(SCpnt);

	DPRINTK(debug_eh, DEBUG_LEAD "resetting ports\n", CMDINFO(SCpnt));
	reset_ports(SCpnt->host);

	return SUCCESS;
}

/*
 * Return the "logical geometry"
 *
 */
int aha152x_biosparam(Scsi_Disk * disk, kdev_t dev, int *info_array)
{
	struct Scsi_Host *shpnt = disk->device->host;

	/* try default translation */
	info_array[0] = 64;
	info_array[1] = 32;
	info_array[2] = disk->capacity / (64 * 32);

	/* for disks >1GB do some guessing */
	if (info_array[2] >= 1024) {
		int info[3];

		/* try to figure out the geometry from the partition table */
		if (scsicam_bios_param(disk, dev, info) < 0 ||
		    !((info[0] == 64 && info[1] == 32) || (info[0] == 255 && info[1] == 63))) {
			if (EXT_TRANS) {
				printk(KERN_NOTICE
				       "aha152x: unable to verify geometry for disk with >1GB.\n"
				       "         using extended translation.\n");
				info_array[0] = 255;
				info_array[1] = 63;
				info_array[2] = disk->capacity / (255 * 63);
			} else {
				printk(KERN_NOTICE
				       "aha152x: unable to verify geometry for disk with >1GB.\n"
				       "         Using default translation. Please verify yourself.\n"
				       "         Perhaps you need to enable extended translation in the driver.\n"
				       "         See /usr/src/linux/drivers/scsi/README.aha152x for details.\n");
			}
		} else {
			info_array[0] = info[0];
			info_array[1] = info[1];
			info_array[2] = info[2];

			if (info[0] == 255 && !EXT_TRANS) {
				printk(KERN_NOTICE
				       "aha152x: current partition table is using extended translation.\n"
				       "         using it also, although it's not explictly enabled.\n");
			}
		}
	}

	return 0;
}

/*
 *  Internal done function
 *
 */
static void done(struct Scsi_Host *shpnt, int error)
{
	if (CURRENT_SC) {
		if(DONE_SC)
			printk(ERR_LEAD "there's already a completed command %p - will cause abort\n", CMDINFO(CURRENT_SC), DONE_SC);

		DONE_SC = CURRENT_SC;
		CURRENT_SC = 0;
		DONE_SC->result = error;
	} else
		printk(KERN_ERR "aha152x: done() called outside of command\n");
}

static struct tq_struct aha152x_tq;

/*
 * Run service completions on the card with interrupts enabled.
 *
 */
static void run(void)
{
	int i;
	for (i = 0; i < IRQS; i++) {
		struct Scsi_Host *shpnt = aha152x_host[i];
		if (shpnt && HOSTDATA(shpnt)->service) {
			HOSTDATA(shpnt)->service=0;
			complete(shpnt);
		}
	}
}

/*
 *    Interrupts handler
 *
 */

static void intr(int irqno, void *dev_id, struct pt_regs *regs)
{
	struct Scsi_Host *shpnt = aha152x_host[irqno - IRQ_MIN];

	if (!shpnt) {
		printk(KERN_ERR "aha152x: catched interrupt for unknown controller.\n");
		return;
	}

	/* no more interrupts from the controller, while we're busy.
	   INTEN is restored by the BH handler */
	CLRBITS(DMACNTRL0, INTEN);

#if 0
	/* check if there is already something to be
           serviced; should not happen */
	if(HOSTDATA(shpnt)->service) {
		printk(KERN_ERR "aha152x%d: lost interrupt (%d)\n", HOSTNO, HOSTDATA(shpnt)->service);
	        show_queues(shpnt);
	}
#endif
	
	/* Poke the BH handler */
	HOSTDATA(shpnt)->service++;
	aha152x_tq.routine = (void *) run;
	queue_task(&aha152x_tq, &tq_immediate);
	mark_bh(IMMEDIATE_BH);
}

/*
 * busfree phase
 * - handle completition/disconnection/error of current command
 * - start selection for next command (if any)
 */
static void busfree_run(struct Scsi_Host *shpnt)
{
	unsigned long flags;
#if defined(AHA152X_STAT)
	int action=0;
#endif

	SETPORT(SXFRCTL0, CH1|CLRCH1|CLRSTCNT);
	SETPORT(SXFRCTL0, CH1);

	SETPORT(SSTAT1, CLRBUSFREE);
	
	if(CURRENT_SC) {
#if defined(AHA152X_STAT)
		action++;
#endif
		CURRENT_SC->SCp.phase &= ~syncneg;

		if(CURRENT_SC->SCp.phase & completed) {
			/* target sent COMMAND COMPLETE */
			done(shpnt, (CURRENT_SC->SCp.Status & 0xff) | ((CURRENT_SC->SCp.Message & 0xff) << 8) | (DID_OK << 16));

		} else if(CURRENT_SC->SCp.phase & aborted) {
			DPRINTK(debug_eh, DEBUG_LEAD "ABORT sent\n", CMDINFO(CURRENT_SC));
			done(shpnt, (CURRENT_SC->SCp.Status & 0xff) | ((CURRENT_SC->SCp.Message & 0xff) << 8) | (DID_ABORT << 16));

		} else if(CURRENT_SC->SCp.phase & resetted) {
			DPRINTK(debug_eh, DEBUG_LEAD "BUS DEVICE RESET sent\n", CMDINFO(CURRENT_SC));
			done(shpnt, (CURRENT_SC->SCp.Status & 0xff) | ((CURRENT_SC->SCp.Message & 0xff) << 8) | (DID_RESET << 16));

		} else if(CURRENT_SC->SCp.phase & disconnected) {
			/* target sent DISCONNECT */
			DPRINTK(debug_selection, DEBUG_LEAD "target disconnected at %d/%d\n",
				CMDINFO(CURRENT_SC),
				CURRENT_SC->resid,
				CURRENT_SC->request_bufflen);
#if defined(AHA152X_STAT)
			HOSTDATA(shpnt)->disconnections++;
#endif
			append_SC(&DISCONNECTED_SC, CURRENT_SC);
			CURRENT_SC->SCp.phase |= 1 << 16;
			CURRENT_SC = 0;

		} else {
			done(shpnt, DID_ERROR << 16);
		}
#if defined(AHA152X_STAT)
	} else {
		HOSTDATA(shpnt)->busfree_without_old_command++;
#endif
	}

	DO_LOCK(flags);

	if(DONE_SC) {
#if defined(AHA152X_STAT)
		action++;
#endif
		if(SCDONE(DONE_SC)) {
			Scsi_Cmnd *ptr=DONE_SC;
			DONE_SC=SCDONE(DONE_SC);

#if 0
			if(HOSTDATA(shpnt)->debug & debug_eh) {
				printk(ERR_LEAD "received sense: ", CMDINFO(ptr));
				print_sense("bh", DONE_SC);
			}
#endif

			HOSTDATA(shpnt)->commands--;
			if (!HOSTDATA(shpnt)->commands)
				SETPORT(PORTA, 0);	/* turn led off */

			kfree(ptr->host_scribble);
			kfree(ptr);
		} else if(DONE_SC->SCp.Status==0x02) {
#if defined(AHA152X_STAT)
			HOSTDATA(shpnt)->busfree_with_check_condition++;
#endif
#if 0
			DPRINTK(debug_eh, ERR_LEAD "CHECK CONDITION found\n", CMDINFO(DONE_SC));
#endif

			if(!(DONE_SC->SCp.Status & not_issued)) {
				Scsi_Cmnd *cmnd = kmalloc(sizeof(Scsi_Cmnd), GFP_ATOMIC);

				if(cmnd) {
					Scsi_Cmnd *ptr=DONE_SC;
					DONE_SC=0;

#if 0
					DPRINTK(debug_eh, ERR_LEAD "requesting sense\n", CMDINFO(ptr));
#endif

					cmnd->cmnd[0]         = REQUEST_SENSE;
					cmnd->cmnd[1]         = 0;
					cmnd->cmnd[2]         = 0;
					cmnd->cmnd[3]         = 0;
					cmnd->cmnd[4]         = sizeof(ptr->sense_buffer);
					cmnd->cmnd[5]         = 0;
					cmnd->cmd_len	      = 6;
					cmnd->host            = ptr->host;
					cmnd->target          = ptr->target;
					cmnd->lun             = ptr->lun;
					cmnd->use_sg          = 0; 
					cmnd->request_buffer  = ptr->sense_buffer;
					cmnd->request_bufflen = sizeof(ptr->sense_buffer);
			
					DO_UNLOCK(flags);
					aha152x_internal_queue(cmnd, 0, 0, ptr, internal_done);
					DO_LOCK(flags);
				} else {
					printk(ERR_LEAD "allocation failed\n", CMDINFO(CURRENT_SC));
					if(cmnd)
						kfree(cmnd);
				}
			} else {
#if 0
				DPRINTK(debug_eh, ERR_LEAD "command not issued - CHECK CONDITION ignored\n", CMDINFO(DONE_SC));
#endif				
			}
		}

		if(DONE_SC && DONE_SC->scsi_done) {
			/* turn led off, when no commands are in the driver */
			HOSTDATA(shpnt)->commands--;
			if (!HOSTDATA(shpnt)->commands)
				SETPORT(PORTA, 0);	/* turn led off */

			kfree(DONE_SC->host_scribble);
			DONE_SC->host_scribble=0;

			DO_UNLOCK(flags);
			DPRINTK(debug_done, DEBUG_LEAD "calling scsi_done(%p)\n", CMDINFO(DONE_SC), DONE_SC);
                	DONE_SC->scsi_done(DONE_SC);
			DPRINTK(debug_done, DEBUG_LEAD "scsi_done(%p) returned\n", CMDINFO(DONE_SC), DONE_SC);
			DO_LOCK(flags);
		}

		DONE_SC=0;
#if defined(AHA152X_STAT)
	} else {
		HOSTDATA(shpnt)->busfree_without_done_command++;
#endif
	}

	if(ISSUE_SC)
		CURRENT_SC = remove_first_SC(&ISSUE_SC);

	DO_UNLOCK(flags);

	if(CURRENT_SC) {
#if defined(AHA152X_STAT)
		action++;
#endif
	    	CURRENT_SC->SCp.phase |= selecting;

		DPRINTK(debug_selection, DEBUG_LEAD "selecting target\n", CMDINFO(CURRENT_SC));

		/* clear selection timeout */
		SETPORT(SSTAT1, SELTO);

		SETPORT(SCSIID, (shpnt->this_id << OID_) | CURRENT_SC->target);
		SETPORT(SXFRCTL1, (PARITY ? ENSPCHK : 0 ) | ENSTIMER);
		SETPORT(SCSISEQ, ENSELO | ENAUTOATNO | (DISCONNECTED_SC ? ENRESELI : 0));
	} else {
#if defined(AHA152X_STAT)
		HOSTDATA(shpnt)->busfree_without_new_command++;
#endif
		SETPORT(SCSISEQ, DISCONNECTED_SC ? ENRESELI : 0);
	}

#if defined(AHA152X_STAT)
	if(!action)
		HOSTDATA(shpnt)->busfree_without_any_action++;
#endif
}

/*
 * Selection done (OUT)
 * - queue IDENTIFY message and SDTR to selected target for message out
 *   (ATN asserted automagically via ENAUTOATNO in busfree())
 */
static void seldo_run(struct Scsi_Host *shpnt)
{
	SETPORT(SCSISIG, 0);
	SETPORT(SSTAT1, CLRBUSFREE);
	SETPORT(SSTAT1, CLRPHASECHG);

    	CURRENT_SC->SCp.phase &= ~(selecting|not_issued);

	SETPORT(SCSISEQ, 0);

	if (TESTLO(SSTAT0, SELDO)) {
		printk(ERR_LEAD "aha152x: passing bus free condition\n", CMDINFO(CURRENT_SC));
		done(shpnt, DID_NO_CONNECT << 16);
		return;
	}

	SETPORT(SSTAT0, CLRSELDO);
	
	ADDMSGO(IDENTIFY(RECONNECT, CURRENT_SC->lun));

	if (CURRENT_SC->SCp.phase & aborting) {
		ADDMSGO(ABORT);
	} else if (CURRENT_SC->SCp.phase & resetting) {
		ADDMSGO(BUS_DEVICE_RESET);
	} else if (SYNCNEG==0 && SYNCHRONOUS) {
    		CURRENT_SC->SCp.phase |= syncneg;
		ADDMSGO(EXTENDED_MESSAGE);
		ADDMSGO(3);
		ADDMSGO(EXTENDED_SDTR);
		ADDMSGO(50);		/* 200ns */
		ADDMSGO(8);		/* 8 byte req/ack offset */

		SYNCNEG=1;		/* negotiation in progress */
	}

	SETRATE(SYNCRATE);
}

/*
 * Selection timeout
 * - return command to mid-level with failure cause
 *
 */
static void selto_run(struct Scsi_Host *shpnt)
{
	SETPORT(SCSISEQ, 0);		
	SETPORT(SSTAT1, CLRSELTIMO);

	DPRINTK(debug_selection, DEBUG_LEAD "selection timeout\n", CMDINFO(CURRENT_SC));

	if(!CURRENT_SC) {
		DPRINTK(debug_selection, DEBUG_LEAD "!CURRENT_SC\n", CMDINFO(CURRENT_SC));
		return;
	}

    	CURRENT_SC->SCp.phase &= ~selecting;

	if (CURRENT_SC->SCp.phase & aborted) {
		DPRINTK(debug_selection, DEBUG_LEAD "aborted\n", CMDINFO(CURRENT_SC));
		done(shpnt, DID_ABORT << 16);
	} else if (TESTLO(SSTAT0, SELINGO)) {
		DPRINTK(debug_selection, DEBUG_LEAD "arbitration not won\n", CMDINFO(CURRENT_SC));
		done(shpnt, DID_BUS_BUSY << 16);
	} else {
		/* ARBITRATION won, but SELECTION failed */
		DPRINTK(debug_selection, DEBUG_LEAD "selection failed\n", CMDINFO(CURRENT_SC));
		done(shpnt, DID_NO_CONNECT << 16);
	}
}

/*
 * Selection in done
 * - put current command back to issue queue
 *   (reconnection of a disconnected nexus instead
 *    of successful selection out)
 *
 */
static void seldi_run(struct Scsi_Host *shpnt)
{
	int selid;
	int target;
	unsigned long flags;

	SETPORT(SCSISIG, 0);
	SETPORT(SSTAT0, CLRSELDI);
	SETPORT(SSTAT1, CLRBUSFREE);
	SETPORT(SSTAT1, CLRPHASECHG);

	if(CURRENT_SC) {
		if(!(CURRENT_SC->SCp.phase & not_issued))
			printk(ERR_LEAD "command should not have been issued yet\n", CMDINFO(CURRENT_SC));

		DPRINTK(debug_selection, ERR_LEAD "command requeued - reselection\n", CMDINFO(CURRENT_SC));

		DO_LOCK(flags);
		append_SC(&ISSUE_SC, CURRENT_SC);
		DO_UNLOCK(flags);

		CURRENT_SC = 0;
	}

	if(!DISCONNECTED_SC) {
		DPRINTK(debug_selection, DEBUG_LEAD "unexpected SELDI ", CMDINFO(CURRENT_SC));
		return;
	}

	RECONN_TARGET=-1;

	selid = GETPORT(SELID) & ~(1 << shpnt->this_id);

	if (selid==0) {
		printk("aha152x%d: target id unknown (%02x)\n", HOSTNO, selid);
		return;
	}

	for(target=7; !(selid & (1 << target)); target--)
		;

	if(selid & ~(1 << target)) {
		printk("aha152x%d: multiple targets reconnected (%02x)\n",
		       HOSTNO, selid);
	}


	SETPORT(SCSIID, (shpnt->this_id << OID_) | target);
	SETPORT(SCSISEQ, 0);

	SETRATE(HOSTDATA(shpnt)->syncrate[target]);

	RECONN_TARGET=target;
	DPRINTK(debug_selection, DEBUG_LEAD "target %d reselected (%02x).\n", CMDINFO(CURRENT_SC), target, selid);
}

/*
 * message in phase
 * - handle initial message after reconnection to identify
 *   reconnecting nexus
 * - queue command on DISCONNECTED_SC on DISCONNECT message
 * - set completed flag on COMMAND COMPLETE
 *   (other completition code moved to busfree_run)
 * - handle response to SDTR
 * - clear synchronous transfer agreements on BUS RESET
 *
 * FIXME: what about SAVE POINTERS, RESTORE POINTERS?
 *
 */
static void msgi_run(struct Scsi_Host *shpnt)
{
	for(;;) {
		int sstat1 = GETPORT(SSTAT1);

		if(sstat1 & (PHASECHG|PHASEMIS|BUSFREE) || !(sstat1 & REQINIT))
			return;

		if(TESTLO(SSTAT0,SPIORDY)) {
			DPRINTK(debug_msgi, DEBUG_LEAD "!SPIORDY\n", CMDINFO(CURRENT_SC));
			return;
		}	

		ADDMSGI(GETPORT(SCSIDAT));

#if defined(AHA152X_DEBUG)
		if (HOSTDATA(shpnt)->debug & debug_msgi) {
			printk(INFO_LEAD "inbound message %02x ", CMDINFO(CURRENT_SC), MSGI(0));
			print_msg(&MSGI(0));
			printk("\n");
		}
#endif

		if(!CURRENT_SC) {
			if(LASTSTATE!=seldi) {
				printk(KERN_ERR "aha152x%d: message in w/o current command not after reselection\n", HOSTNO);
			}

			/*
	 	 	 * Handle reselection
	 		 */
			if(!(MSGI(0) & IDENTIFY_BASE)) {
				printk(KERN_ERR "aha152x%d: target didn't identify after reselection\n", HOSTNO);
				continue;
			}

			CURRENT_SC = remove_lun_SC(&DISCONNECTED_SC, RECONN_TARGET, MSGI(0) & 0x3f);

			if (!CURRENT_SC) {
				show_queues(shpnt);
				printk(KERN_ERR "aha152x%d: no disconnected command for target %d/%d\n", HOSTNO, RECONN_TARGET, MSGI(0) & 0x3f);
				continue;
			}

			DPRINTK(debug_msgi, DEBUG_LEAD "target reconnected\n", CMDINFO(CURRENT_SC));

			CURRENT_SC->SCp.Message = MSGI(0);
			CURRENT_SC->SCp.phase &= ~disconnected;

			MSGILEN=0;

			/* next message if any */
			continue;
		} 

		CURRENT_SC->SCp.Message = MSGI(0);

		switch (MSGI(0)) {
		case DISCONNECT:
			if (!RECONNECT)
				printk(WARN_LEAD "target was not allowed to disconnect\n", CMDINFO(CURRENT_SC));

			CURRENT_SC->SCp.phase |= disconnected;
			break;

		case COMMAND_COMPLETE:
			if(CURRENT_SC->SCp.phase & completed)
				DPRINTK(debug_msgi, DEBUG_LEAD "again COMMAND COMPLETE\n", CMDINFO(CURRENT_SC));

			CURRENT_SC->SCp.phase |= completed;
			break;

		case MESSAGE_REJECT:
			if (SYNCNEG==1) {
				printk(INFO_LEAD "Synchronous Data Transfer Request was rejected\n", CMDINFO(CURRENT_SC));
				SYNCNEG=2;	/* negotiation completed */
			} else
				printk(INFO_LEAD "inbound message (MESSAGE REJECT)\n", CMDINFO(CURRENT_SC));
			break;

		case SAVE_POINTERS:
			break;

		case RESTORE_POINTERS:
			break;

		case EXTENDED_MESSAGE:
			if(MSGILEN<2 || MSGILEN<MSGI(1)+2) {
				/* not yet completed */
				continue;
			}

			switch (MSGI(2)) {
			case EXTENDED_SDTR:
				{
					long ticks;

					if (MSGI(1) != 3) {
						printk(ERR_LEAD "SDTR message length!=3\n", CMDINFO(CURRENT_SC));
						break;
					}

					if (!HOSTDATA(shpnt)->synchronous)
						break;

					printk(INFO_LEAD, CMDINFO(CURRENT_SC));
					print_msg(&MSGI(0));
					printk("\n");

					ticks = (MSGI(3) * 4 + 49) / 50;

					if (syncneg) {
						/* negotiation in progress */
						if (ticks > 9 || MSGI(4) < 1 || MSGI(4) > 8) {
							ADDMSGO(MESSAGE_REJECT);
							printk(INFO_LEAD "received Synchronous Data Transfer Request invalid - rejected\n", CMDINFO(CURRENT_SC));
							break;
						}
						
						SYNCRATE |= ((ticks - 2) << 4) + MSGI(4);
					} else if (ticks <= 9 && MSGI(4) >= 1) {
						ADDMSGO(EXTENDED_MESSAGE);
						ADDMSGO(3);
						ADDMSGO(EXTENDED_SDTR);
						if (ticks < 4) {
							ticks = 4;
							ADDMSGO(50);
						} else
							ADDMSGO(MSGI(3));

						if (MSGI(4) > 8)
							MSGI(4) = 8;

						ADDMSGO(MSGI(4));

						SYNCRATE |= ((ticks - 2) << 4) + MSGI(4);
					} else {
						/* requested SDTR is too slow, do it asynchronously */
						printk(INFO_LEAD "Synchronous Data Transfer Request too slow - Rejecting\n", CMDINFO(CURRENT_SC));
						ADDMSGO(MESSAGE_REJECT);
					}

					SYNCNEG=2;		/* negotiation completed */
					SETRATE(SYNCRATE);
				}
				break;

			case BUS_DEVICE_RESET:
				{
					int i;

					for(i=0; i<8; i++) {
						HOSTDATA(shpnt)->syncrate[i]=0;
						HOSTDATA(shpnt)->syncneg[i]=0;
					}

				}
				break;

			case EXTENDED_MODIFY_DATA_POINTER:
			case EXTENDED_EXTENDED_IDENTIFY:
			case EXTENDED_WDTR:
			default:
				ADDMSGO(MESSAGE_REJECT);
				break;
			}
			break;
		}

		MSGILEN=0;
	}
}

static void msgi_end(struct Scsi_Host *shpnt)
{
	if(MSGILEN>0)
		printk(WARN_LEAD "target left before message completed (%d)\n", CMDINFO(CURRENT_SC), MSGILEN);

	if (MSGOLEN > 0 && !(GETPORT(SSTAT1) & BUSFREE)) {
		DPRINTK(debug_msgi, DEBUG_LEAD "msgo pending\n", CMDINFO(CURRENT_SC));
		SETPORT(SCSISIG, P_MSGI | SIG_ATNO);
	} 
}

/*
 * message out phase
 *
 */
static void msgo_init(struct Scsi_Host *shpnt)
{
	if(MSGOLEN==0) {
		if((CURRENT_SC->SCp.phase & syncneg) && SYNCNEG==2 && SYNCRATE==0) {
			ADDMSGO(IDENTIFY(RECONNECT, CURRENT_SC->lun));
		} else {
			printk(INFO_LEAD "unexpected MESSAGE OUT phase; rejecting\n", CMDINFO(CURRENT_SC));
			ADDMSGO(MESSAGE_REJECT);
		}
	}

#if defined(AHA152X_DEBUG)
	if(HOSTDATA(shpnt)->debug & debug_msgo) {
		int i;

		printk(DEBUG_LEAD "messages( ", CMDINFO(CURRENT_SC));
		for (i=0; i<MSGOLEN; i+=print_msg(&MSGO(i)), printk(" "))
			;
		printk(")\n");
	}
#endif
}

/*
 * message out phase
 *
 */
static void msgo_run(struct Scsi_Host *shpnt)
{
	if(MSGO_I==MSGOLEN)
		DPRINTK(debug_msgo, DEBUG_LEAD "messages all sent (%d/%d)\n", CMDINFO(CURRENT_SC), MSGO_I, MSGOLEN);

	while(MSGO_I<MSGOLEN) {
		DPRINTK(debug_msgo, DEBUG_LEAD "message byte %02x (%d/%d)\n", CMDINFO(CURRENT_SC), MSGO(MSGO_I), MSGO_I, MSGOLEN);

		if(TESTLO(SSTAT0, SPIORDY)) {
			DPRINTK(debug_msgo, DEBUG_LEAD "!SPIORDY\n", CMDINFO(CURRENT_SC));
			return;
		}

		if (MSGO_I==MSGOLEN-1) {
			/* Leave MESSAGE OUT after transfer */
			SETPORT(SSTAT1, CLRATNO);
		}


		if (MSGO(MSGO_I) & IDENTIFY_BASE)
			CURRENT_SC->SCp.phase |= identified;

		if (MSGO(MSGO_I)==ABORT)
			CURRENT_SC->SCp.phase |= aborted;

		if (MSGO(MSGO_I)==BUS_DEVICE_RESET)
			CURRENT_SC->SCp.phase |= resetted;

		SETPORT(SCSIDAT, MSGO(MSGO_I++));
	}
}

static void msgo_end(struct Scsi_Host *shpnt)
{
	if(MSGO_I<MSGOLEN) {
		printk(ERR_LEAD "message sent incompletely (%d/%d)\n", CMDINFO(CURRENT_SC), MSGO_I, MSGOLEN);
		if(SYNCNEG==1) {
			printk(INFO_LEAD "Synchronous Data Transfer Request was rejected\n", CMDINFO(CURRENT_SC));
			SYNCNEG=2;
		}
	}
		
	MSGO_I  = 0;
	MSGOLEN = 0;
}

/* 
 * command phase
 *
 */
static void cmd_init(struct Scsi_Host *shpnt)
{
	if (CURRENT_SC->SCp.sent_command) {
		printk(ERR_LEAD "command already sent\n", CMDINFO(CURRENT_SC));
		done(shpnt, DID_ERROR << 16);
		return;
	}

#if defined(AHA152X_DEBUG)
	if (HOSTDATA(shpnt)->debug & debug_cmd) {
		printk(DEBUG_LEAD "cmd_init: ", CMDINFO(CURRENT_SC));
		print_command(CURRENT_SC->cmnd);
	}
#endif

	CMD_I=0;
}

/*
 * command phase
 *
 */
static void cmd_run(struct Scsi_Host *shpnt)
{
	if(CMD_I==CURRENT_SC->cmd_len) {
		DPRINTK(debug_cmd, DEBUG_LEAD "command already completely sent (%d/%d)", CMDINFO(CURRENT_SC), CMD_I, CURRENT_SC->cmd_len);
		disp_ports(shpnt);
	}

	while(CMD_I<CURRENT_SC->cmd_len) {
		DPRINTK(debug_cmd, DEBUG_LEAD "command byte %02x (%d/%d)\n", CMDINFO(CURRENT_SC), CURRENT_SC->cmnd[CMD_I], CMD_I, CURRENT_SC->cmd_len);

		if(TESTLO(SSTAT0, SPIORDY)) {
			DPRINTK(debug_cmd, DEBUG_LEAD "!SPIORDY\n", CMDINFO(CURRENT_SC));
			return;
		}

		SETPORT(SCSIDAT, CURRENT_SC->cmnd[CMD_I++]);
	}
}

static void cmd_end(struct Scsi_Host *shpnt)
{
	if(CMD_I<CURRENT_SC->cmd_len)
		printk(ERR_LEAD "command sent incompletely (%d/%d)\n", CMDINFO(CURRENT_SC), CMD_I, CURRENT_SC->cmd_len);
	else
		CURRENT_SC->SCp.sent_command++;
}

/*
 * status phase
 *
 */
static void status_run(struct Scsi_Host *shpnt)
{
	if(TESTLO(SSTAT0,SPIORDY)) {
		DPRINTK(debug_status, DEBUG_LEAD "!SPIORDY\n", CMDINFO(CURRENT_SC));
		return;
	}

	CURRENT_SC->SCp.Status = GETPORT(SCSIDAT);

#if defined(AHA152X_DEBUG)
	if (HOSTDATA(shpnt)->debug & debug_status) {
		printk(DEBUG_LEAD "inbound status %02x ", CMDINFO(CURRENT_SC), CURRENT_SC->SCp.Status);
		print_status(CURRENT_SC->SCp.Status);
		printk("\n");
	}
#endif
}

/*
 * data in phase
 *
 */
static void datai_init(struct Scsi_Host *shpnt)
{
	SETPORT(DMACNTRL0, RSTFIFO);
	SETPORT(DMACNTRL0, RSTFIFO|ENDMA);

	SETPORT(SXFRCTL0, CH1|CLRSTCNT);
	SETPORT(SXFRCTL0, CH1|SCSIEN|DMAEN);

	SETPORT(SIMODE0, 0);
	SETPORT(SIMODE1, ENSCSIPERR | ENSCSIRST | ENPHASEMIS | ENBUSFREE);

	DATA_LEN=0;
	DPRINTK(debug_datai,
		DEBUG_LEAD "datai_init: request_bufflen=%d resid=%d\n",
		CMDINFO(CURRENT_SC), CURRENT_SC->request_bufflen, CURRENT_SC->resid);
}

static void datai_run(struct Scsi_Host *shpnt)
{
	unsigned int the_time;
	int fifodata, data_count;

	/*
	 * loop while the phase persists or the fifos are not empty
	 *
	 */
	while(TESTLO(DMASTAT, INTSTAT) || TESTLO(DMASTAT, DFIFOEMP) || TESTLO(SSTAT2, SEMPTY)) {
		/* FIXME: maybe this should be done by setting up
		 * STCNT to trigger ENSWRAP interrupt, instead of
		 * polling for DFIFOFULL
		 */
		the_time=jiffies + 10*HZ;
		while(TESTLO(DMASTAT, DFIFOFULL|INTSTAT) && time_before(jiffies,the_time))
			barrier();

		if(TESTLO(DMASTAT, DFIFOFULL|INTSTAT)) {
			printk(ERR_LEAD "datai timeout", CMDINFO(CURRENT_SC));
			disp_ports(shpnt);
			break;
		}

		if(TESTHI(DMASTAT, DFIFOFULL)) {
			fifodata = 128;
		} else {
			the_time=jiffies + HZ;
			while(TESTLO(SSTAT2, SEMPTY) && time_before(jiffies,the_time))
				barrier();

			if(TESTLO(SSTAT2, SEMPTY)) {
				printk(ERR_LEAD "datai sempty timeout", CMDINFO(CURRENT_SC));
				disp_ports(shpnt);
				break;
			}

			fifodata = GETPORT(FIFOSTAT);
		}

		if(CURRENT_SC->SCp.this_residual>0) {
			while(fifodata>0 && CURRENT_SC->SCp.this_residual>0) {
                        	data_count = fifodata>CURRENT_SC->SCp.this_residual ?
						CURRENT_SC->SCp.this_residual :
						fifodata;
				fifodata -= data_count;

                        	if(data_count & 1) {
					DPRINTK(debug_datai, DEBUG_LEAD "8bit\n", CMDINFO(CURRENT_SC));
                                	SETPORT(DMACNTRL0, ENDMA|_8BIT);
                                	*CURRENT_SC->SCp.ptr++ = GETPORT(DATAPORT);
                                	CURRENT_SC->SCp.this_residual--;
                                	DATA_LEN++;
                                	SETPORT(DMACNTRL0, ENDMA);
                        	}
	
                        	if(data_count > 1) {
					DPRINTK(debug_datai, DEBUG_LEAD "16bit(%d)\n", CMDINFO(CURRENT_SC), data_count);
                                	data_count >>= 1;
                                	insw(DATAPORT, CURRENT_SC->SCp.ptr, data_count);
                                	CURRENT_SC->SCp.ptr           += 2 * data_count;
                                	CURRENT_SC->SCp.this_residual -= 2 * data_count;
                                	DATA_LEN                      += 2 * data_count;
                        	}
	
                        	if(CURRENT_SC->SCp.this_residual==0 && CURRENT_SC->SCp.buffers_residual>0) {
                               		/* advance to next buffer */
                               		CURRENT_SC->SCp.buffers_residual--;
                               		CURRENT_SC->SCp.buffer++;
                               		CURRENT_SC->SCp.ptr           = CURRENT_SC->SCp.buffer->address;
                               		CURRENT_SC->SCp.this_residual = CURRENT_SC->SCp.buffer->length;
				} 
                	}
		} else if(fifodata>0) { 
			printk(ERR_LEAD "no buffers left for %d(%d) bytes (data overrun!?)\n", CMDINFO(CURRENT_SC), fifodata, GETPORT(FIFOSTAT));
                        SETPORT(DMACNTRL0, ENDMA|_8BIT);
			while(fifodata>0) {
				int data;
				data=GETPORT(DATAPORT);
				DPRINTK(debug_datai, DEBUG_LEAD "data=%02x\n", CMDINFO(CURRENT_SC), data);
				fifodata--;
				DATA_LEN++;
			}
                        SETPORT(DMACNTRL0, ENDMA|_8BIT);
		}
	}

	if(TESTLO(DMASTAT, INTSTAT) ||
	   TESTLO(DMASTAT, DFIFOEMP) ||
	   TESTLO(SSTAT2, SEMPTY) ||
	   GETPORT(FIFOSTAT)>0) {
	   	/*
		 * something went wrong, if there's something left in the fifos
		 * or the phase didn't change
		 */
		printk(ERR_LEAD "fifos should be empty and phase should have changed\n", CMDINFO(CURRENT_SC));
		disp_ports(shpnt);
	}

	if(DATA_LEN!=GETSTCNT()) {
		printk(ERR_LEAD
		       "manual transfer count differs from automatic (count=%d;stcnt=%d;diff=%d;fifostat=%d)",
		       CMDINFO(CURRENT_SC), DATA_LEN, GETSTCNT(), GETSTCNT()-DATA_LEN, GETPORT(FIFOSTAT));
		disp_ports(shpnt);
		mdelay(10000);
	}
}

static void datai_end(struct Scsi_Host *shpnt)
{
	CURRENT_SC->resid -= GETSTCNT();

	DPRINTK(debug_datai,
		DEBUG_LEAD "datai_end: request_bufflen=%d resid=%d stcnt=%d\n",
		CMDINFO(CURRENT_SC), CURRENT_SC->request_bufflen, CURRENT_SC->resid, GETSTCNT());

	SETPORT(SXFRCTL0, CH1|CLRSTCNT);
	SETPORT(DMACNTRL0, 0);
}

/*
 * data out phase
 *
 */
static void datao_init(struct Scsi_Host *shpnt)
{
	SETPORT(DMACNTRL0, WRITE_READ | RSTFIFO);
	SETPORT(DMACNTRL0, WRITE_READ | ENDMA);

	SETPORT(SXFRCTL0, CH1|CLRSTCNT);
	SETPORT(SXFRCTL0, CH1|SCSIEN|DMAEN);

	SETPORT(SIMODE0, 0);
	SETPORT(SIMODE1, ENSCSIPERR | ENSCSIRST | ENPHASEMIS | ENBUSFREE );

	DATA_LEN = CURRENT_SC->resid;

	DPRINTK(debug_datao,
		DEBUG_LEAD "datao_init: request_bufflen=%d; resid=%d\n",
		CMDINFO(CURRENT_SC), CURRENT_SC->request_bufflen, CURRENT_SC->resid);
}

static void datao_run(struct Scsi_Host *shpnt)
{
	unsigned int the_time;
	int data_count;

	/* until phase changes or all data sent */
	while(TESTLO(DMASTAT, INTSTAT) && CURRENT_SC->SCp.this_residual>0) {
		data_count = 128;
		if(data_count > CURRENT_SC->SCp.this_residual)
			data_count=CURRENT_SC->SCp.this_residual;

		if(TESTLO(DMASTAT, DFIFOEMP)) {
			printk(ERR_LEAD "datao fifo not empty (%d)", CMDINFO(CURRENT_SC), GETPORT(FIFOSTAT));
			disp_ports(shpnt);
			break;
		}

		if(data_count & 1) {
			SETPORT(DMACNTRL0,WRITE_READ|ENDMA|_8BIT);
			SETPORT(DATAPORT, *CURRENT_SC->SCp.ptr++);
			CURRENT_SC->SCp.this_residual--;
			CURRENT_SC->resid--;
			SETPORT(DMACNTRL0,WRITE_READ|ENDMA);
		}

		if(data_count > 1) {
			data_count >>= 1;
			outsw(DATAPORT, CURRENT_SC->SCp.ptr, data_count);
			CURRENT_SC->SCp.ptr           += 2 * data_count;
			CURRENT_SC->SCp.this_residual -= 2 * data_count;
			CURRENT_SC->resid             -= 2 * data_count;
	  	}

		if(CURRENT_SC->SCp.this_residual==0 && CURRENT_SC->SCp.buffers_residual>0) {
			/* advance to next buffer */
			CURRENT_SC->SCp.buffers_residual--;
			CURRENT_SC->SCp.buffer++;
			CURRENT_SC->SCp.ptr           = CURRENT_SC->SCp.buffer->address;
			CURRENT_SC->SCp.this_residual = CURRENT_SC->SCp.buffer->length;
		}

		the_time=jiffies+10*HZ;
		while(TESTLO(DMASTAT, DFIFOEMP|INTSTAT) && time_before(jiffies,the_time))
			barrier();

		if(TESTLO(DMASTAT, DFIFOEMP|INTSTAT)) {
			printk(ERR_LEAD "dataout timeout", CMDINFO(CURRENT_SC));
			disp_ports(shpnt);
			break;
		}
	}
}

static void datao_end(struct Scsi_Host *shpnt)
{
	if(TESTLO(DMASTAT, DFIFOEMP)) {
		int data_count = (DATA_LEN - CURRENT_SC->resid) - GETSTCNT();

		DPRINTK(debug_datao, DEBUG_LEAD "datao: %d bytes to resend (%d written, %d transfered)\n",
			CMDINFO(CURRENT_SC),
			data_count,
			DATA_LEN-CURRENT_SC->resid,
			GETSTCNT());

		CURRENT_SC->resid += data_count;

		if(CURRENT_SC->use_sg) {
			data_count -= CURRENT_SC->SCp.ptr - CURRENT_SC->SCp.buffer->address;
			while(data_count>0) {
				CURRENT_SC->SCp.buffer--;
				CURRENT_SC->SCp.buffers_residual++;
				data_count -= CURRENT_SC->SCp.buffer->length;
			}
			CURRENT_SC->SCp.ptr           = CURRENT_SC->SCp.buffer->address - data_count;
			CURRENT_SC->SCp.this_residual = CURRENT_SC->SCp.buffer->length + data_count;
		} else {
			CURRENT_SC->SCp.ptr           -= data_count;
			CURRENT_SC->SCp.this_residual += data_count;
		}
	}

	DPRINTK(debug_datao, DEBUG_LEAD "datao_end: request_bufflen=%d; resid=%d; stcnt=%d\n",
		CMDINFO(CURRENT_SC),
		CURRENT_SC->request_bufflen,
		CURRENT_SC->resid,
		GETSTCNT());

	SETPORT(SXFRCTL0, CH1|CLRCH1|CLRSTCNT);
	SETPORT(SXFRCTL0, CH1);

	SETPORT(DMACNTRL0, 0);
}

/*
 * figure out what state we're in
 *
 */
static int update_state(struct Scsi_Host *shpnt)
{
	int dataphase=0;
	unsigned int stat0 = GETPORT(SSTAT0);
	unsigned int stat1 = GETPORT(SSTAT1);

	PREVSTATE = STATE;
	STATE=unknown;

	if(stat1 & SCSIRSTI) {
		STATE=rsti;
		SETPORT(SCSISEQ,0);
		SETPORT(SSTAT1,SCSIRSTI);
  	} else if(stat0 & SELDI && PREVSTATE==busfree) {
		STATE=seldi;
	} else if(stat0 & SELDO && CURRENT_SC && (CURRENT_SC->SCp.phase & selecting)) {
		STATE=seldo;
	} else if(stat1 & SELTO) {
		STATE=selto;
	} else if(stat1 & BUSFREE) {
		STATE=busfree;
		SETPORT(SSTAT1,BUSFREE);
	} else if(stat1 & SCSIPERR) {
		STATE=parerr;
		SETPORT(SSTAT1,SCSIPERR);
	} else if(stat1 & REQINIT) {
		switch(GETPORT(SCSISIG) & P_MASK) {
		case P_MSGI:	STATE=msgi;	break;
		case P_MSGO:	STATE=msgo;	break;
		case P_DATAO:	STATE=datao;	break;
		case P_DATAI:	STATE=datai;	break;
		case P_STATUS:	STATE=status;	break;
		case P_CMD:	STATE=cmd;	break;
		}
		dataphase=1;
	}

	if((stat0 & SELDI) && STATE!=seldi && !dataphase) {
		printk(INFO_LEAD "reselection missed?", CMDINFO(CURRENT_SC));
		disp_ports(shpnt);
	}

	if(STATE!=PREVSTATE) {
		LASTSTATE=PREVSTATE;
	}

	return dataphase;
}

/*
 * handle parity error
 *
 * FIXME: in which phase?
 *
 */
static void parerr_run(struct Scsi_Host *shpnt)
{
	printk(ERR_LEAD "parity error\n", CMDINFO(CURRENT_SC));
	done(shpnt, DID_PARITY << 16);
}

/*
 * handle reset in
 *
 */
static void rsti_run(struct Scsi_Host *shpnt)
{
	Scsi_Cmnd *ptr;

	printk(KERN_NOTICE "aha152x%d: scsi reset in\n", HOSTNO);
	
	ptr=DISCONNECTED_SC;
	while(ptr) {
		Scsi_Cmnd *next = SCNEXT(ptr);

		if (!ptr->device->soft_reset) {
			remove_SC(&DISCONNECTED_SC, ptr);

			kfree(ptr->host_scribble);
			ptr->host_scribble=0;

			ptr->result =  DID_RESET << 16;
			ptr->scsi_done(ptr);
		}

		ptr = next;
	}

	if(CURRENT_SC && !CURRENT_SC->device->soft_reset)
		done(shpnt, DID_RESET << 16 );
}


/*
 * bottom-half handler
 *
 */
static void complete(struct Scsi_Host *shpnt)
{
	int dataphase;
	unsigned long flags;
	int pending;

	DO_LOCK(flags);
	if(HOSTDATA(shpnt)->in_intr!=0)
		aha152x_error(shpnt, "bottom-half already running!?");
	HOSTDATA(shpnt)->in_intr++;
	DO_UNLOCK(flags);

	/*
	 * loop while there are interrupt conditions pending
	 *
	 */
	do {
		unsigned long start = jiffies;
		dataphase=update_state(shpnt);

		DPRINTK(debug_phases, LEAD "start %s %s(%s)\n", CMDINFO(CURRENT_SC), states[STATE].name, states[PREVSTATE].name, states[LASTSTATE].name);

		/*
		 * end previous state
		 *
		 */
		if(PREVSTATE!=STATE && states[PREVSTATE].end)
			states[PREVSTATE].end(shpnt);

		/*
		 * disable SPIO mode if previous phase used it
		 * and this one doesn't
		 *
		 */
		if(states[PREVSTATE].spio && !states[STATE].spio) {
			SETPORT(SXFRCTL0, CH1);
			SETPORT(DMACNTRL0, 0);
			if(CURRENT_SC)
				CURRENT_SC->SCp.phase &= ~spiordy;
		}

		/*
		 * accept current dataphase phase
		 *
		 */
		if(dataphase) {
			SETPORT(SSTAT0, REQINIT);
			SETPORT(SCSISIG, GETPORT(SCSISIG) & P_MASK);
			SETPORT(SSTAT1, PHASECHG);  
		}
		
		/*
		 * enable SPIO mode if previous didn't use it
		 * and this one does
		 *
		 */
		if(!states[PREVSTATE].spio && states[STATE].spio) {
			SETPORT(DMACNTRL0, 0);
			SETPORT(SXFRCTL0, CH1|SPIOEN);
			if(CURRENT_SC)
				CURRENT_SC->SCp.phase |= spiordy;
		}
		
		/*
		 * initialize for new state
		 *
		 */
		if(PREVSTATE!=STATE && states[STATE].init)
			states[STATE].init(shpnt);
		
		/*
		 * handle current state
		 *
		 */
		if(states[STATE].run)
			states[STATE].run(shpnt);
		else
			printk(ERR_LEAD "unexpected state (%x)\n", CMDINFO(CURRENT_SC), STATE);
		
		/*
		 * setup controller to interrupt on
		 * the next expected condition and
		 * loop if it's already there
		 *
		 */
		DO_LOCK(flags);
		pending=setup_expected_interrupts(shpnt);
#if defined(AHA152X_STAT)
		HOSTDATA(shpnt)->count[STATE]++;
		if(PREVSTATE!=STATE)
			HOSTDATA(shpnt)->count_trans[STATE]++;
		HOSTDATA(shpnt)->time[STATE] += jiffies-start;
#endif
		DO_UNLOCK(flags);

		DPRINTK(debug_phases, LEAD "end %s %s(%s)\n", CMDINFO(CURRENT_SC), states[STATE].name, states[PREVSTATE].name, states[LASTSTATE].name);
	} while(pending);

	/*
	 * enable interrupts and leave bottom-half
	 *
	 */
	DO_LOCK(flags);
	HOSTDATA(shpnt)->in_intr--;
	SETBITS(DMACNTRL0, INTEN);
	DO_UNLOCK(flags);
}


/* 
 * Dump the current driver status and panic
 */
static void aha152x_error(struct Scsi_Host *shpnt, char *msg)
{
	printk(KERN_EMERG "\naha152x%d: %s\n", HOSTNO, msg);
	show_queues(shpnt);
	panic("aha152x panic\n");
}

/*
 * Display registers of AIC-6260
 */
static void disp_ports(struct Scsi_Host *shpnt)
{
#if defined(AHA152X_DEBUG)
	int s;

	printk("\n%s: %s(%s) ",
		CURRENT_SC ? "busy" : "waiting",
		states[STATE].name,
		states[PREVSTATE].name);

	s = GETPORT(SCSISEQ);
	printk("SCSISEQ( ");
	if (s & TEMODEO)
		printk("TARGET MODE ");
	if (s & ENSELO)
		printk("SELO ");
	if (s & ENSELI)
		printk("SELI ");
	if (s & ENRESELI)
		printk("RESELI ");
	if (s & ENAUTOATNO)
		printk("AUTOATNO ");
	if (s & ENAUTOATNI)
		printk("AUTOATNI ");
	if (s & ENAUTOATNP)
		printk("AUTOATNP ");
	if (s & SCSIRSTO)
		printk("SCSIRSTO ");
	printk(");");

	printk(" SCSISIG(");
	s = GETPORT(SCSISIG);
	switch (s & P_MASK) {
	case P_DATAO:
		printk("DATA OUT");
		break;
	case P_DATAI:
		printk("DATA IN");
		break;
	case P_CMD:
		printk("COMMAND");
		break;
	case P_STATUS:
		printk("STATUS");
		break;
	case P_MSGO:
		printk("MESSAGE OUT");
		break;
	case P_MSGI:
		printk("MESSAGE IN");
		break;
	default:
		printk("*illegal*");
		break;
	}

	printk("); ");

	printk("INTSTAT (%s); ", TESTHI(DMASTAT, INTSTAT) ? "hi" : "lo");

	printk("SSTAT( ");
	s = GETPORT(SSTAT0);
	if (s & TARGET)
		printk("TARGET ");
	if (s & SELDO)
		printk("SELDO ");
	if (s & SELDI)
		printk("SELDI ");
	if (s & SELINGO)
		printk("SELINGO ");
	if (s & SWRAP)
		printk("SWRAP ");
	if (s & SDONE)
		printk("SDONE ");
	if (s & SPIORDY)
		printk("SPIORDY ");
	if (s & DMADONE)
		printk("DMADONE ");

	s = GETPORT(SSTAT1);
	if (s & SELTO)
		printk("SELTO ");
	if (s & ATNTARG)
		printk("ATNTARG ");
	if (s & SCSIRSTI)
		printk("SCSIRSTI ");
	if (s & PHASEMIS)
		printk("PHASEMIS ");
	if (s & BUSFREE)
		printk("BUSFREE ");
	if (s & SCSIPERR)
		printk("SCSIPERR ");
	if (s & PHASECHG)
		printk("PHASECHG ");
	if (s & REQINIT)
		printk("REQINIT ");
	printk("); ");


	printk("SSTAT( ");

	s = GETPORT(SSTAT0) & GETPORT(SIMODE0);

	if (s & TARGET)
		printk("TARGET ");
	if (s & SELDO)
		printk("SELDO ");
	if (s & SELDI)
		printk("SELDI ");
	if (s & SELINGO)
		printk("SELINGO ");
	if (s & SWRAP)
		printk("SWRAP ");
	if (s & SDONE)
		printk("SDONE ");
	if (s & SPIORDY)
		printk("SPIORDY ");
	if (s & DMADONE)
		printk("DMADONE ");

	s = GETPORT(SSTAT1) & GETPORT(SIMODE1);

	if (s & SELTO)
		printk("SELTO ");
	if (s & ATNTARG)
		printk("ATNTARG ");
	if (s & SCSIRSTI)
		printk("SCSIRSTI ");
	if (s & PHASEMIS)
		printk("PHASEMIS ");
	if (s & BUSFREE)
		printk("BUSFREE ");
	if (s & SCSIPERR)
		printk("SCSIPERR ");
	if (s & PHASECHG)
		printk("PHASECHG ");
	if (s & REQINIT)
		printk("REQINIT ");
	printk("); ");

	printk("SXFRCTL0( ");

	s = GETPORT(SXFRCTL0);
	if (s & SCSIEN)
		printk("SCSIEN ");
	if (s & DMAEN)
		printk("DMAEN ");
	if (s & CH1)
		printk("CH1 ");
	if (s & CLRSTCNT)
		printk("CLRSTCNT ");
	if (s & SPIOEN)
		printk("SPIOEN ");
	if (s & CLRCH1)
		printk("CLRCH1 ");
	printk("); ");

	printk("SIGNAL( ");

	s = GETPORT(SCSISIG);
	if (s & SIG_ATNI)
		printk("ATNI ");
	if (s & SIG_SELI)
		printk("SELI ");
	if (s & SIG_BSYI)
		printk("BSYI ");
	if (s & SIG_REQI)
		printk("REQI ");
	if (s & SIG_ACKI)
		printk("ACKI ");
	printk("); ");

	printk("SELID (%02x), ", GETPORT(SELID));

	printk("STCNT (%d), ", GETSTCNT());
	
	printk("SSTAT2( ");

	s = GETPORT(SSTAT2);
	if (s & SOFFSET)
		printk("SOFFSET ");
	if (s & SEMPTY)
		printk("SEMPTY ");
	if (s & SFULL)
		printk("SFULL ");
	printk("); SFCNT (%d); ", s & (SFULL | SFCNT));

	s = GETPORT(SSTAT3);
	printk("SCSICNT (%d), OFFCNT(%d), ", (s & 0xf0) >> 4, s & 0x0f);

	printk("SSTAT4( ");
	s = GETPORT(SSTAT4);
	if (s & SYNCERR)
		printk("SYNCERR ");
	if (s & FWERR)
		printk("FWERR ");
	if (s & FRERR)
		printk("FRERR ");
	printk("); ");

	printk("DMACNTRL0( ");
	s = GETPORT(DMACNTRL0);
	printk("%s ", s & _8BIT ? "8BIT" : "16BIT");
	printk("%s ", s & DMA ? "DMA" : "PIO");
	printk("%s ", s & WRITE_READ ? "WRITE" : "READ");
	if (s & ENDMA)
		printk("ENDMA ");
	if (s & INTEN)
		printk("INTEN ");
	if (s & RSTFIFO)
		printk("RSTFIFO ");
	if (s & SWINT)
		printk("SWINT ");
	printk("); ");

	printk("DMASTAT( ");
	s = GETPORT(DMASTAT);
	if (s & ATDONE)
		printk("ATDONE ");
	if (s & WORDRDY)
		printk("WORDRDY ");
	if (s & DFIFOFULL)
		printk("DFIFOFULL ");
	if (s & DFIFOEMP)
		printk("DFIFOEMP ");
	printk(")\n");
#endif
}

/*
 * display enabled interrupts
 */
static void disp_enintr(struct Scsi_Host *shpnt)
{
	int s;

	printk(KERN_DEBUG "enabled interrupts ( ");

	s = GETPORT(SIMODE0);
	if (s & ENSELDO)
		printk("ENSELDO ");
	if (s & ENSELDI)
		printk("ENSELDI ");
	if (s & ENSELINGO)
		printk("ENSELINGO ");
	if (s & ENSWRAP)
		printk("ENSWRAP ");
	if (s & ENSDONE)
		printk("ENSDONE ");
	if (s & ENSPIORDY)
		printk("ENSPIORDY ");
	if (s & ENDMADONE)
		printk("ENDMADONE ");

	s = GETPORT(SIMODE1);
	if (s & ENSELTIMO)
		printk("ENSELTIMO ");
	if (s & ENATNTARG)
		printk("ENATNTARG ");
	if (s & ENPHASEMIS)
		printk("ENPHASEMIS ");
	if (s & ENBUSFREE)
		printk("ENBUSFREE ");
	if (s & ENSCSIPERR)
		printk("ENSCSIPERR ");
	if (s & ENPHASECHG)
		printk("ENPHASECHG ");
	if (s & ENREQINIT)
		printk("ENREQINIT ");
	printk(")\n");
}

/*
 * Show the command data of a command
 */
static void show_command(Scsi_Cmnd *ptr)
{
	printk(KERN_DEBUG "0x%08x: target=%d; lun=%d; cmnd=(",
	       (unsigned int) ptr, ptr->target, ptr->lun);

	print_command(ptr->cmnd);

	printk(KERN_DEBUG "); request_bufflen=%d; resid=%d; phase |",
	       ptr->request_bufflen, ptr->resid);

	if (ptr->SCp.phase & not_issued)
		printk("not issued|");
	if (ptr->SCp.phase & selecting)
		printk("selecting|");
	if (ptr->SCp.phase & identified)
		printk("identified|");
	if (ptr->SCp.phase & disconnected)
		printk("disconnected|");
	if (ptr->SCp.phase & completed)
		printk("completed|");
	if (ptr->SCp.phase & spiordy)
		printk("spiordy|");
	if (ptr->SCp.phase & syncneg)
		printk("syncneg|");
	if (ptr->SCp.phase & aborted)
		printk("aborted|");
	if (ptr->SCp.phase & resetted)
		printk("resetted|");
	printk("; next=0x%p\n", SCNEXT(ptr));
}

/*
 * Dump the queued data
 */
static void show_queues(struct Scsi_Host *shpnt)
{
	Scsi_Cmnd *ptr;
	unsigned long flags;

	DO_LOCK(flags);
	printk(KERN_DEBUG "\nqueue status:\nissue_SC:\n");
	for (ptr = ISSUE_SC; ptr; ptr = SCNEXT(ptr))
		show_command(ptr);
	DO_UNLOCK(flags);

	printk(KERN_DEBUG "current_SC:\n");
	if (CURRENT_SC)
		show_command(CURRENT_SC);
	else
		printk(KERN_DEBUG "none\n");

	printk(KERN_DEBUG "disconnected_SC:\n");
	for (ptr = DISCONNECTED_SC; ptr; ptr = SCNEXT(ptr))
		show_command(ptr);

	disp_ports(shpnt);
	disp_enintr(shpnt);
}

#undef SPRINTF
#define SPRINTF(args...) pos += sprintf(pos, ## args)

static int get_command(char *pos, Scsi_Cmnd * ptr)
{
	char *start = pos;
	int i;

	SPRINTF("0x%08x: target=%d; lun=%d; cmnd=( ",
		(unsigned int) ptr, ptr->target, ptr->lun);

	for (i = 0; i < COMMAND_SIZE(ptr->cmnd[0]); i++)
		SPRINTF("0x%02x ", ptr->cmnd[i]);

	SPRINTF("); resid=%d; residual=%d; buffers=%d; phase |",
		ptr->resid, ptr->SCp.this_residual, ptr->SCp.buffers_residual);

	if (ptr->SCp.phase & not_issued)
		SPRINTF("not issued|");
	if (ptr->SCp.phase & selecting)
		SPRINTF("selecting|");
	if (ptr->SCp.phase & disconnected)
		SPRINTF("disconnected|");
	if (ptr->SCp.phase & aborted)
		SPRINTF("aborted|");
	if (ptr->SCp.phase & identified)
		SPRINTF("identified|");
	if (ptr->SCp.phase & completed)
		SPRINTF("completed|");
	if (ptr->SCp.phase & spiordy)
		SPRINTF("spiordy|");
	if (ptr->SCp.phase & syncneg)
		SPRINTF("syncneg|");
	SPRINTF("; next=0x%p\n", SCNEXT(ptr));

	return (pos - start);
}

static int get_ports(struct Scsi_Host *shpnt, char *pos)
{
	char *start = pos;
	int s;

	SPRINTF("\n%s: %s(%s) ", CURRENT_SC ? "on bus" : "waiting", states[STATE].name, states[PREVSTATE].name);

	s = GETPORT(SCSISEQ);
	SPRINTF("SCSISEQ( ");
	if (s & TEMODEO)
		SPRINTF("TARGET MODE ");
	if (s & ENSELO)
		SPRINTF("SELO ");
	if (s & ENSELI)
		SPRINTF("SELI ");
	if (s & ENRESELI)
		SPRINTF("RESELI ");
	if (s & ENAUTOATNO)
		SPRINTF("AUTOATNO ");
	if (s & ENAUTOATNI)
		SPRINTF("AUTOATNI ");
	if (s & ENAUTOATNP)
		SPRINTF("AUTOATNP ");
	if (s & SCSIRSTO)
		SPRINTF("SCSIRSTO ");
	SPRINTF(");");

	SPRINTF(" SCSISIG(");
	s = GETPORT(SCSISIG);
	switch (s & P_MASK) {
	case P_DATAO:
		SPRINTF("DATA OUT");
		break;
	case P_DATAI:
		SPRINTF("DATA IN");
		break;
	case P_CMD:
		SPRINTF("COMMAND");
		break;
	case P_STATUS:
		SPRINTF("STATUS");
		break;
	case P_MSGO:
		SPRINTF("MESSAGE OUT");
		break;
	case P_MSGI:
		SPRINTF("MESSAGE IN");
		break;
	default:
		SPRINTF("*illegal*");
		break;
	}

	SPRINTF("); ");

	SPRINTF("INTSTAT (%s); ", TESTHI(DMASTAT, INTSTAT) ? "hi" : "lo");

	SPRINTF("SSTAT( ");
	s = GETPORT(SSTAT0);
	if (s & TARGET)
		SPRINTF("TARGET ");
	if (s & SELDO)
		SPRINTF("SELDO ");
	if (s & SELDI)
		SPRINTF("SELDI ");
	if (s & SELINGO)
		SPRINTF("SELINGO ");
	if (s & SWRAP)
		SPRINTF("SWRAP ");
	if (s & SDONE)
		SPRINTF("SDONE ");
	if (s & SPIORDY)
		SPRINTF("SPIORDY ");
	if (s & DMADONE)
		SPRINTF("DMADONE ");

	s = GETPORT(SSTAT1);
	if (s & SELTO)
		SPRINTF("SELTO ");
	if (s & ATNTARG)
		SPRINTF("ATNTARG ");
	if (s & SCSIRSTI)
		SPRINTF("SCSIRSTI ");
	if (s & PHASEMIS)
		SPRINTF("PHASEMIS ");
	if (s & BUSFREE)
		SPRINTF("BUSFREE ");
	if (s & SCSIPERR)
		SPRINTF("SCSIPERR ");
	if (s & PHASECHG)
		SPRINTF("PHASECHG ");
	if (s & REQINIT)
		SPRINTF("REQINIT ");
	SPRINTF("); ");


	SPRINTF("SSTAT( ");

	s = GETPORT(SSTAT0) & GETPORT(SIMODE0);

	if (s & TARGET)
		SPRINTF("TARGET ");
	if (s & SELDO)
		SPRINTF("SELDO ");
	if (s & SELDI)
		SPRINTF("SELDI ");
	if (s & SELINGO)
		SPRINTF("SELINGO ");
	if (s & SWRAP)
		SPRINTF("SWRAP ");
	if (s & SDONE)
		SPRINTF("SDONE ");
	if (s & SPIORDY)
		SPRINTF("SPIORDY ");
	if (s & DMADONE)
		SPRINTF("DMADONE ");

	s = GETPORT(SSTAT1) & GETPORT(SIMODE1);

	if (s & SELTO)
		SPRINTF("SELTO ");
	if (s & ATNTARG)
		SPRINTF("ATNTARG ");
	if (s & SCSIRSTI)
		SPRINTF("SCSIRSTI ");
	if (s & PHASEMIS)
		SPRINTF("PHASEMIS ");
	if (s & BUSFREE)
		SPRINTF("BUSFREE ");
	if (s & SCSIPERR)
		SPRINTF("SCSIPERR ");
	if (s & PHASECHG)
		SPRINTF("PHASECHG ");
	if (s & REQINIT)
		SPRINTF("REQINIT ");
	SPRINTF("); ");

	SPRINTF("SXFRCTL0( ");

	s = GETPORT(SXFRCTL0);
	if (s & SCSIEN)
		SPRINTF("SCSIEN ");
	if (s & DMAEN)
		SPRINTF("DMAEN ");
	if (s & CH1)
		SPRINTF("CH1 ");
	if (s & CLRSTCNT)
		SPRINTF("CLRSTCNT ");
	if (s & SPIOEN)
		SPRINTF("SPIOEN ");
	if (s & CLRCH1)
		SPRINTF("CLRCH1 ");
	SPRINTF("); ");

	SPRINTF("SIGNAL( ");

	s = GETPORT(SCSISIG);
	if (s & SIG_ATNI)
		SPRINTF("ATNI ");
	if (s & SIG_SELI)
		SPRINTF("SELI ");
	if (s & SIG_BSYI)
		SPRINTF("BSYI ");
	if (s & SIG_REQI)
		SPRINTF("REQI ");
	if (s & SIG_ACKI)
		SPRINTF("ACKI ");
	SPRINTF("); ");

	SPRINTF("SELID(%02x), ", GETPORT(SELID));

	SPRINTF("STCNT(%d), ", GETSTCNT());

	SPRINTF("SSTAT2( ");

	s = GETPORT(SSTAT2);
	if (s & SOFFSET)
		SPRINTF("SOFFSET ");
	if (s & SEMPTY)
		SPRINTF("SEMPTY ");
	if (s & SFULL)
		SPRINTF("SFULL ");
	SPRINTF("); SFCNT (%d); ", s & (SFULL | SFCNT));

	s = GETPORT(SSTAT3);
	SPRINTF("SCSICNT (%d), OFFCNT(%d), ", (s & 0xf0) >> 4, s & 0x0f);

	SPRINTF("SSTAT4( ");
	s = GETPORT(SSTAT4);
	if (s & SYNCERR)
		SPRINTF("SYNCERR ");
	if (s & FWERR)
		SPRINTF("FWERR ");
	if (s & FRERR)
		SPRINTF("FRERR ");
	SPRINTF("); ");

	SPRINTF("DMACNTRL0( ");
	s = GETPORT(DMACNTRL0);
	SPRINTF("%s ", s & _8BIT ? "8BIT" : "16BIT");
	SPRINTF("%s ", s & DMA ? "DMA" : "PIO");
	SPRINTF("%s ", s & WRITE_READ ? "WRITE" : "READ");
	if (s & ENDMA)
		SPRINTF("ENDMA ");
	if (s & INTEN)
		SPRINTF("INTEN ");
	if (s & RSTFIFO)
		SPRINTF("RSTFIFO ");
	if (s & SWINT)
		SPRINTF("SWINT ");
	SPRINTF("); ");

	SPRINTF("DMASTAT( ");
	s = GETPORT(DMASTAT);
	if (s & ATDONE)
		SPRINTF("ATDONE ");
	if (s & WORDRDY)
		SPRINTF("WORDRDY ");
	if (s & DFIFOFULL)
		SPRINTF("DFIFOFULL ");
	if (s & DFIFOEMP)
		SPRINTF("DFIFOEMP ");
	SPRINTF(")\n");

	SPRINTF("enabled interrupts( ");

	s = GETPORT(SIMODE0);
	if (s & ENSELDO)
		SPRINTF("ENSELDO ");
	if (s & ENSELDI)
		SPRINTF("ENSELDI ");
	if (s & ENSELINGO)
		SPRINTF("ENSELINGO ");
	if (s & ENSWRAP)
		SPRINTF("ENSWRAP ");
	if (s & ENSDONE)
		SPRINTF("ENSDONE ");
	if (s & ENSPIORDY)
		SPRINTF("ENSPIORDY ");
	if (s & ENDMADONE)
		SPRINTF("ENDMADONE ");

	s = GETPORT(SIMODE1);
	if (s & ENSELTIMO)
		SPRINTF("ENSELTIMO ");
	if (s & ENATNTARG)
		SPRINTF("ENATNTARG ");
	if (s & ENPHASEMIS)
		SPRINTF("ENPHASEMIS ");
	if (s & ENBUSFREE)
		SPRINTF("ENBUSFREE ");
	if (s & ENSCSIPERR)
		SPRINTF("ENSCSIPERR ");
	if (s & ENPHASECHG)
		SPRINTF("ENPHASECHG ");
	if (s & ENREQINIT)
		SPRINTF("ENREQINIT ");
	SPRINTF(")\n");

	return (pos - start);
}

int aha152x_set_info(char *buffer, int length, struct Scsi_Host *shpnt)
{
	if(!shpnt || !buffer || length<8 || strncmp("aha152x ", buffer, 8)!=0)
		return -EINVAL;

#if defined(AHA152X_DEBUG)
	if(length>14 && strncmp("debug ", buffer+8, 6)==0) {
		int debug = HOSTDATA(shpnt)->debug;

		HOSTDATA(shpnt)->debug = simple_strtoul(buffer+14, NULL, 0);

		printk(KERN_INFO "aha152x%d: debugging options set to 0x%04x (were 0x%04x)\n", HOSTNO, HOSTDATA(shpnt)->debug, debug);
	} else
#endif
#if defined(AHA152X_STAT)
	if(length>13 && strncmp("reset", buffer+8, 5)==0) {
		int i;

		HOSTDATA(shpnt)->total_commands=0;
		HOSTDATA(shpnt)->disconnections=0;
		HOSTDATA(shpnt)->busfree_without_any_action=0;
		HOSTDATA(shpnt)->busfree_without_old_command=0;
		HOSTDATA(shpnt)->busfree_without_new_command=0;
		HOSTDATA(shpnt)->busfree_without_done_command=0;
		HOSTDATA(shpnt)->busfree_with_check_condition=0;
		for (i = idle; i<maxstate; i++) {
			HOSTDATA(shpnt)->count[i]=0;
			HOSTDATA(shpnt)->count_trans[i]=0;
			HOSTDATA(shpnt)->time[i]=0;
		}

		printk(KERN_INFO "aha152x%d: stats reseted.\n", HOSTNO);

	} else
#endif
	{
		return -EINVAL;
	}


	return length;
}

#undef SPRINTF
#define SPRINTF(args...) \
	do { if(pos < buffer + length) pos += sprintf(pos, ## args); } while(0)

int aha152x_proc_info(char *buffer, char **start,
		      off_t offset, int length, int hostno, int inout)
{
	int i;
	char *pos = buffer;
	struct Scsi_Host *shpnt;
	Scsi_Cmnd *ptr;
	unsigned long flags;
	int thislength;

	for (i = 0, shpnt = (struct Scsi_Host *) NULL; i < IRQS; i++)
		if (aha152x_host[i] && aha152x_host[i]->host_no == hostno)
			shpnt = aha152x_host[i];

	if (!shpnt)
		return -ESRCH;

	DPRINTK(debug_procinfo, 
	       KERN_DEBUG "aha152x_proc_info: buffer=%p offset=%ld length=%d hostno=%d inout=%d\n",
	       buffer, offset, length, hostno, inout);


	if (inout)
		return aha152x_set_info(buffer, length, shpnt);

	SPRINTF(AHA152X_REVID "\n");

	SPRINTF("ioports 0x%04lx to 0x%04lx\n",
		shpnt->io_port, shpnt->io_port + shpnt->n_io_port - 1);
	SPRINTF("interrupt 0x%02x\n", shpnt->irq);
	SPRINTF("disconnection/reconnection %s\n",
		RECONNECT ? "enabled" : "disabled");
	SPRINTF("parity checking %s\n",
		PARITY ? "enabled" : "disabled");
	SPRINTF("synchronous transfers %s\n",
		SYNCHRONOUS ? "enabled" : "disabled");
	SPRINTF("%d commands currently queued\n", HOSTDATA(shpnt)->commands);

	if(SYNCHRONOUS) {
		SPRINTF("synchronously operating targets (tick=50 ns):\n");
		for (i = 0; i < 8; i++)
			if (HOSTDATA(shpnt)->syncrate[i] & 0x7f)
				SPRINTF("target %d: period %dT/%dns; req/ack offset %d\n",
					i,
					(((HOSTDATA(shpnt)->syncrate[i] & 0x70) >> 4) + 2),
					(((HOSTDATA(shpnt)->syncrate[i] & 0x70) >> 4) + 2) * 50,
				    HOSTDATA(shpnt)->syncrate[i] & 0x0f);
	}
#if defined(AHA152X_DEBUG)
#define PDEBUG(flags,txt) \
	if(HOSTDATA(shpnt)->debug & flags) SPRINTF("(%s) ", txt);

	SPRINTF("enabled debugging options: ");

	PDEBUG(debug_procinfo, "procinfo");
	PDEBUG(debug_queue, "queue");
	PDEBUG(debug_intr, "interrupt");
	PDEBUG(debug_selection, "selection");
	PDEBUG(debug_msgo, "message out");
	PDEBUG(debug_msgi, "message in");
	PDEBUG(debug_status, "status");
	PDEBUG(debug_cmd, "command");
	PDEBUG(debug_datai, "data in");
	PDEBUG(debug_datao, "data out");
	PDEBUG(debug_eh, "eh");
	PDEBUG(debug_locks, "locks");
	PDEBUG(debug_phases, "phases");

	SPRINTF("\n");
#endif

	SPRINTF("\nqueue status:\n");
	DO_LOCK(flags);
	if (ISSUE_SC) {
		SPRINTF("not yet issued commands:\n");
		for (ptr = ISSUE_SC; ptr; ptr = SCNEXT(ptr))
			pos += get_command(pos, ptr);
	} else
		SPRINTF("no not yet issued commands\n");
	DO_UNLOCK(flags);

	if (CURRENT_SC) {
		SPRINTF("current command:\n");
		pos += get_command(pos, CURRENT_SC);
	} else
		SPRINTF("no current command\n");

	if (DISCONNECTED_SC) {
		SPRINTF("disconnected commands:\n");
		for (ptr = DISCONNECTED_SC; ptr; ptr = SCNEXT(ptr))
			pos += get_command(pos, ptr);
	} else
		SPRINTF("no disconnected commands\n");

	pos += get_ports(shpnt, pos);

#if defined(AHA152X_STAT)
	SPRINTF("statistics:\n"
	        "total commands:               %d\n"
	        "disconnections:               %d\n"
		"busfree with check condition: %d\n"
		"busfree without old command:  %d\n"
		"busfree without new command:  %d\n"
		"busfree without done command: %d\n"
		"busfree without any action:   %d\n"
		"state      "
		"transitions  "
		"count        "
		"time\n",
		HOSTDATA(shpnt)->total_commands,
		HOSTDATA(shpnt)->disconnections,
		HOSTDATA(shpnt)->busfree_with_check_condition,
		HOSTDATA(shpnt)->busfree_without_old_command,
		HOSTDATA(shpnt)->busfree_without_new_command,
		HOSTDATA(shpnt)->busfree_without_done_command,
		HOSTDATA(shpnt)->busfree_without_any_action);
	for(i=0; i<maxstate; i++) {
		SPRINTF("%-10s %-12d %-12d %-12ld\n",
		        states[i].name,
			HOSTDATA(shpnt)->count_trans[i],
			HOSTDATA(shpnt)->count[i],
			HOSTDATA(shpnt)->time[i]);
	}
#endif

	DPRINTK(debug_procinfo, KERN_DEBUG "aha152x_proc_info: pos=%p\n", pos);

	thislength = pos - (buffer + offset);
	DPRINTK(debug_procinfo, KERN_DEBUG "aha152x_proc_info: length=%d thislength=%d\n", length, thislength);

	if(thislength<0) {
		DPRINTK(debug_procinfo, KERN_DEBUG "aha152x_proc_info: output too short\n");
		*start = 0;
		return 0;
	}

	thislength = thislength<length ? thislength : length;

	DPRINTK(debug_procinfo, KERN_DEBUG "aha152x_proc_info: return %d\n", thislength);

	*start = buffer + offset;
	return thislength < length ? thislength : length;
}

/* Eventually this will go into an include file, but this will be later */
static Scsi_Host_Template driver_template = AHA152X;

#include "scsi_module.c"
