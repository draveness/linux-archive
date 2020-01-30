/*
 * $Id: capidrv.c,v 1.39 2000/11/23 20:45:14 kai Exp $
 *
 * ISDN4Linux Driver, using capi20 interface (kernelcapi)
 *
 * Copyright 1997 by Carsten Paeth (calle@calle.in-berlin.de)
 *
 * $Log: capidrv.c,v $
 * Revision 1.39  2000/11/23 20:45:14  kai
 * fixed module_init/exit stuff
 * Note: compiled-in kernel doesn't work pre 2.2.18 anymore.
 *
 * Revision 1.38  2000/11/14 08:43:07  calle
 * Bugfix for v110. Connectparamters where setup for sync ...
 *
 * Revision 1.37  2000/11/01 14:05:02  calle
 * - use module_init/module_exit from linux/init.h.
 * - all static struct variables are initialized with "membername:" now.
 * - avm_cs.c, let it work with newer pcmcia-cs.
 *
 * Revision 1.36  2000/06/26 15:13:41  keil
 * features should be or'ed
 *
 * Revision 1.35  2000/06/19 15:11:25  keil
 * avoid use of freed structs
 * changes from 2.4.0-ac21
 *
 * Revision 1.34  2000/06/19 13:13:55  calle
 * Added Modemsupport!
 *
 * Revision 1.33  2000/05/06 00:52:36  kai
 * merged changes from kernel tree
 * fixed timer and net_device->name breakage
 *
 * Revision 1.32  2000/04/07 15:19:58  calle
 * remove warnings
 *
 * Revision 1.31  2000/04/06 15:01:25  calle
 * Bugfix: crash in capidrv.c when reseting a capi controller.
 * - changed code order on remove of controller.
 * - using tq_schedule for notifier in kcapi.c.
 * - now using spin_lock_irqsave() and spin_unlock_irqrestore().
 * strange: sometimes even MP hang on unload of isdn.o ...
 *
 * Revision 1.30  2000/03/03 15:50:42  calle
 * - kernel CAPI:
 *   - Changed parameter "param" in capi_signal from __u32 to void *.
 *   - rewrote notifier handling in kcapi.c
 *   - new notifier NCCI_UP and NCCI_DOWN
 * - User CAPI:
 *   - /dev/capi20 is now a cloning device.
 *   - middleware extentions prepared.
 * - capidrv.c
 *   - locking of list operations and module count updates.
 *
 * Revision 1.29  1999/12/06 17:13:06  calle
 * Added controller watchdog.
 *
 * Revision 1.28  1999/11/05 16:22:37  calle
 * Bugfix: Missing break in switch on ISDN_CMD_HANGUP.
 *
 * Revision 1.27  1999/09/16 15:13:04  calle
 * forgot to change paramter type of contr for lower_callback ...
 *
 * Revision 1.26  1999/08/06 07:41:16  calle
 * Added the "vbox patch". if (si1 == 1) si2 = 0;
 *
 * Revision 1.25  1999/08/04 10:10:11  calle
 * Bugfix: corrected /proc functions, added structure for new AVM cards.
 *
 * Revision 1.24  1999/07/20 06:48:02  calle
 * Bugfix: firmware version check for D2 trace was too restrictiv.
 *
 * Revision 1.23  1999/07/09 15:05:44  keil
 * compat.h is now isdn_compat.h
 *
 * Revision 1.22  1999/07/06 07:24:14  calle
 * Bugfix: call to kfree_skb in capidrv_signal was too early,
 *         thanks to Lars Heete <hel@admin.de>.
 *
 * Revision 1.21  1999/07/01 15:26:34  calle
 * complete new version (I love it):
 * + new hardware independed "capi_driver" interface that will make it easy to:
 *   - support other controllers with CAPI-2.0 (i.e. USB Controller)
 *   - write a CAPI-2.0 for the passive cards
 *   - support serial link CAPI-2.0 boxes.
 * + wrote "capi_driver" for all supported cards.
 * + "capi_driver" (supported cards) now have to be configured with
 *   make menuconfig, in the past all supported cards where included
 *   at once.
 * + new and better informations in /proc/capi/
 * + new ioctl to switch trace of capi messages per controller
 *   using "avmcapictrl trace [contr] on|off|...."
 * + complete testcircle with all supported cards and also the
 *   PCMCIA cards (now patch for pcmcia-cs-3.0.13 needed) done.
 *
 * Revision 1.20  1999/07/01 08:22:59  keil
 * compatibility macros now in <linux/isdn_compat.h>
 *
 * Revision 1.19  1999/06/29 16:16:54  calle
 * Let ISDN_CMD_UNLOAD work with open isdn devices without crash again.
 * Also right unlocking (ISDN_CMD_UNLOCK) is done now.
 * isdnlog should check returncode of read(2) calls.
 *
 * Revision 1.18  1999/06/21 15:24:15  calle
 * extend information in /proc.
 *
 * Revision 1.17  1999/06/10 16:53:55  calle
 * Removing of module b1pci will now remove card from lower level.
 *
 * Revision 1.16  1999/05/31 11:50:33  calle
 * Bugfix: In if_sendbuf, skb_push'ed DATA_B3 header was not skb_pull'ed
 *         on failure, result in data block with DATA_B3 header transmitted
 *
 * Revision 1.15  1999/05/25 21:26:16  calle
 * Include CAPI-Channelallocation (leased lines) from the 2.0 tree.
 *
 * Revision 1.14  1999/05/22 07:55:06  calle
 * Added *V110* to AVM B1 driver.
 *
 * Revision 1.13  1998/06/26 15:12:55  fritz
 * Added handling of STAT_ICALL with incomplete CPN.
 * Added AT&L for ttyI emulator.
 * Added more locking stuff in tty_write.
 *
 * Revision 1.12  1998/03/29 16:06:03  calle
 * changes from 2.0 tree merged.
 *
 * Revision 1.3.2.10  1998/03/20 14:38:24  calle
 * capidrv: prepared state machines for suspend/resume/hold
 * capidrv: fix bug in state machine if B1/T1 is out of nccis
 * b1capi: changed some errno returns.
 * b1capi: detect if you try to add same T1 to different io address.
 * b1capi: change number of nccis depending on number of channels.
 * b1lli: cosmetics
 *
 * Revision 1.3.2.9  1998/03/20 09:01:12  calle
 * Changes capi_register handling to get full support for 30 bchannels.
 *
 * Revision 1.3.2.8  1998/03/18 17:51:28  calle
 * added controller number to error messages
 *
 * Revision 1.3.2.7  1998/02/27 15:40:47  calle
 * T1 running with slow link. bugfix in capi_release.
 *
 * Revision 1.11  1998/02/13 07:09:15  calle
 * change for 2.1.86 (removing FREE_READ/FREE_WRITE from [dev]_kfree_skb()
 *
 * Revision 1.10  1998/02/02 19:52:23  calle
 * Fixed vbox (audio) acceptb.
 *
 * Revision 1.9  1998/01/31 11:14:45  calle
 * merged changes to 2.0 tree, prepare 2.1.82 to work.
 *
 * Revision 1.8  1997/11/04 06:12:09  calle
 * capi.c: new read/write in file_ops since 2.1.60
 * capidrv.c: prepared isdnlog interface for d2-trace in newer firmware.
 * capiutil.c: needs config.h (CONFIG_ISDN_DRV_AVMB1_VERBOSE_REASON)
 * compat.h: added #define LinuxVersionCode
 *
 * Revision 1.7  1997/10/11 10:36:34  calle
 * Added isdnlog support. patch to isdnlog needed.
 *
 * Revision 1.6  1997/10/11 10:25:55  calle
 * New interface for lowlevel drivers. BSENT with nr. of bytes sent,
 * allow sending without ACK.
 *
 * Revision 1.5  1997/10/01 09:21:16  fritz
 * Removed old compatibility stuff for 2.0.X kernels.
 * From now on, this code is for 2.1.X ONLY!
 * Old stuff is still in the separate branch.
 *
 * Revision 1.4  1997/07/13 12:22:43  calle
 * bug fix for more than one controller in connect_req.
 * debugoutput now with contrnr.
 *
 * Revision 1.3  1997/05/18 09:24:15  calle
 * added verbose disconnect reason reporting to avmb1.
 * some fixes in capi20 interface.
 * changed info messages for B1-PCI
 *
 * Revision 1.2  1997/03/05 21:19:59  fritz
 * Removed include of config.h (mkdep stated this is unneded).
 *
 * Revision 1.1  1997/03/04 21:50:31  calle
 * Frirst version in isdn4linux
 *
 * Revision 2.2  1997/02/12 09:31:39  calle
 * new version
 *
 * Revision 1.1  1997/01/31 10:32:20  calle
 * Initial revision
 *
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/sched.h>
#include <linux/malloc.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/signal.h>
#include <linux/mm.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/skbuff.h>
#include <linux/isdn.h>
#include <linux/isdnif.h>
#include <linux/proc_fs.h>
#include <linux/capi.h>
#include <linux/kernelcapi.h>
#include <linux/ctype.h>
#include <linux/init.h>
#include <asm/segment.h>

#include "capiutil.h"
#include "capicmd.h"
#include "capidrv.h"

static char *revision = "$Revision: 1.39 $";
static int debugmode = 0;

MODULE_AUTHOR("Carsten Paeth <calle@calle.in-berlin.de>");
MODULE_PARM(debugmode, "i");

/* -------- type definitions ----------------------------------------- */


struct capidrv_contr {

	struct capidrv_contr *next;

	__u32 contrnr;
	char name[20];

	/*
	 * for isdn4linux
	 */
	isdn_if interface;
	int myid;

	/*
	 * LISTEN state
	 */
	int state;
	__u32 cipmask;
	__u32 cipmask2;
        struct timer_list listentimer;

	/*
	 * ID of capi message sent
	 */
	__u16 msgid;

	/*
	 * B-Channels
	 */
	int nbchan;
	struct capidrv_bchan {
		struct capidrv_contr *contr;
		__u8 msn[ISDN_MSNLEN];
		int l2;
		int l3;
		__u8 num[ISDN_MSNLEN];
		__u8 mynum[ISDN_MSNLEN];
		int si1;
		int si2;
		int incoming;
		int disconnecting;
		struct capidrv_plci {
			struct capidrv_plci *next;
			__u32 plci;
			__u32 ncci;	/* ncci for CONNECT_ACTIVE_IND */
			__u16 msgid;	/* to identfy CONNECT_CONF */
			int chan;
			int state;
			int leasedline;
			struct capidrv_ncci {
				struct capidrv_ncci *next;
				struct capidrv_plci *plcip;
				__u32 ncci;
				__u16 msgid;	/* to identfy CONNECT_B3_CONF */
				int chan;
				int state;
				int oldstate;
				/* */
				__u16 datahandle;
				struct ncci_datahandle_queue {
				    struct ncci_datahandle_queue *next;
				    __u16                         datahandle;
				    int                           len;
				} *ackqueue;
			} *ncci_list;
		} *plcip;
		struct capidrv_ncci *nccip;
	} *bchans;

	struct capidrv_plci *plci_list;

	/* for q931 data */
	__u8  q931_buf[4096];
	__u8 *q931_read;
	__u8 *q931_write;
	__u8 *q931_end;
};


struct capidrv_data {
	__u16 appid;
	int ncontr;
	struct capidrv_contr *contr_list;

        /* statistic */
	unsigned long nrecvctlpkt;
	unsigned long nrecvdatapkt;
	unsigned long nsentctlpkt;
	unsigned long nsentdatapkt;
};

typedef struct capidrv_plci capidrv_plci;
typedef struct capidrv_ncci capidrv_ncci;
typedef struct capidrv_contr capidrv_contr;
typedef struct capidrv_data capidrv_data;
typedef struct capidrv_bchan capidrv_bchan;

/* -------- data definitions ----------------------------------------- */

static capidrv_data global;
static spinlock_t global_lock = SPIN_LOCK_UNLOCKED;
static struct capi_interface *capifuncs;

static void handle_dtrace_data(capidrv_contr *card,
	int send, int level2, __u8 *data, __u16 len);

/* -------- convert functions ---------------------------------------- */

static inline __u32 b1prot(int l2, int l3)
{
	switch (l2) {
	case ISDN_PROTO_L2_X75I:
	case ISDN_PROTO_L2_X75UI:
	case ISDN_PROTO_L2_X75BUI:
		return 0;
	case ISDN_PROTO_L2_HDLC:
	default:
		return 0;
	case ISDN_PROTO_L2_TRANS:
		return 1;
        case ISDN_PROTO_L2_V11096:
        case ISDN_PROTO_L2_V11019:
        case ISDN_PROTO_L2_V11038:
		return 2;
        case ISDN_PROTO_L2_FAX:
		return 4;
	case ISDN_PROTO_L2_MODEM:
		return 8;
	}
}

static inline __u32 b2prot(int l2, int l3)
{
	switch (l2) {
	case ISDN_PROTO_L2_X75I:
	case ISDN_PROTO_L2_X75UI:
	case ISDN_PROTO_L2_X75BUI:
	default:
		return 0;
	case ISDN_PROTO_L2_HDLC:
	case ISDN_PROTO_L2_TRANS:
        case ISDN_PROTO_L2_V11096:
        case ISDN_PROTO_L2_V11019:
        case ISDN_PROTO_L2_V11038:
	case ISDN_PROTO_L2_MODEM:
		return 1;
        case ISDN_PROTO_L2_FAX:
		return 4;
	}
}

static inline __u32 b3prot(int l2, int l3)
{
	switch (l2) {
	case ISDN_PROTO_L2_X75I:
	case ISDN_PROTO_L2_X75UI:
	case ISDN_PROTO_L2_X75BUI:
	case ISDN_PROTO_L2_HDLC:
	case ISDN_PROTO_L2_TRANS:
        case ISDN_PROTO_L2_V11096:
        case ISDN_PROTO_L2_V11019:
        case ISDN_PROTO_L2_V11038:
	case ISDN_PROTO_L2_MODEM:
	default:
		return 0;
        case ISDN_PROTO_L2_FAX:
		return 4;
	}
}

static _cstruct b1config_async_v110(__u16 rate)
{
	/* CAPI-Spec "B1 Configuration" */
	static unsigned char buf[9];
	buf[0] = 8; /* len */
	/* maximum bitrate */
	buf[1] = rate & 0xff; buf[2] = (rate >> 8) & 0xff;
	buf[3] = 8; buf[4] = 0; /* 8 bits per character */
	buf[5] = 0; buf[6] = 0; /* parity none */
	buf[7] = 0; buf[8] = 0; /* 1 stop bit */
	return buf;
}

static _cstruct b1config(int l2, int l3)
{
	switch (l2) {
	case ISDN_PROTO_L2_X75I:
	case ISDN_PROTO_L2_X75UI:
	case ISDN_PROTO_L2_X75BUI:
	case ISDN_PROTO_L2_HDLC:
	case ISDN_PROTO_L2_TRANS:
	default:
		return 0;
        case ISDN_PROTO_L2_V11096:
	    return b1config_async_v110(9600);
        case ISDN_PROTO_L2_V11019:
	    return b1config_async_v110(19200);
        case ISDN_PROTO_L2_V11038:
	    return b1config_async_v110(38400);
	}
}

static inline __u16 si2cip(__u8 si1, __u8 si2)
{
	static const __u8 cip[17][5] =
	{
	/*  0  1  2  3  4  */
		{0, 0, 0, 0, 0},	/*0 */
		{16, 16, 4, 26, 16},	/*1 */
		{17, 17, 17, 4, 4},	/*2 */
		{2, 2, 2, 2, 2},	/*3 */
		{18, 18, 18, 18, 18},	/*4 */
		{2, 2, 2, 2, 2},	/*5 */
		{0, 0, 0, 0, 0},	/*6 */
		{2, 2, 2, 2, 2},	/*7 */
		{2, 2, 2, 2, 2},	/*8 */
		{21, 21, 21, 21, 21},	/*9 */
		{19, 19, 19, 19, 19},	/*10 */
		{0, 0, 0, 0, 0},	/*11 */
		{0, 0, 0, 0, 0},	/*12 */
		{0, 0, 0, 0, 0},	/*13 */
		{0, 0, 0, 0, 0},	/*14 */
		{22, 22, 22, 22, 22},	/*15 */
		{27, 27, 27, 28, 27}	/*16 */
	};
	if (si1 > 16)
		si1 = 0;
	if (si2 > 4)
		si2 = 0;

	return (__u16) cip[si1][si2];
}

static inline __u8 cip2si1(__u16 cipval)
{
	static const __u8 si[32] =
	{7, 1, 7, 7, 1, 1, 7, 7,	/*0-7 */
	 7, 1, 0, 0, 0, 0, 0, 0,	/*8-15 */
	 1, 2, 4, 10, 9, 9, 15, 7,	/*16-23 */
	 7, 7, 1, 16, 16, 0, 0, 0};	/*24-31 */

	if (cipval > 31)
		cipval = 0;	/* .... */
	return si[cipval];
}

static inline __u8 cip2si2(__u16 cipval)
{
	static const __u8 si[32] =
	{0, 0, 0, 0, 2, 3, 0, 0,	/*0-7 */
	 0, 3, 0, 0, 0, 0, 0, 0,	/*8-15 */
	 1, 2, 0, 0, 9, 0, 0, 0,	/*16-23 */
	 0, 0, 3, 2, 3, 0, 0, 0};	/*24-31 */

	if (cipval > 31)
		cipval = 0;	/* .... */
	return si[cipval];
}


/* -------- controller managment ------------------------------------- */

static inline capidrv_contr *findcontrbydriverid(int driverid)
{
    	unsigned long flags;
	capidrv_contr *p;

	spin_lock_irqsave(&global_lock, flags);
	for (p = global.contr_list; p; p = p->next)
		if (p->myid == driverid)
			break;
	spin_unlock_irqrestore(&global_lock, flags);
	return p;
}

static capidrv_contr *findcontrbynumber(__u32 contr)
{
	unsigned long flags;
	capidrv_contr *p = global.contr_list;

	spin_lock_irqsave(&global_lock, flags);
	for (p = global.contr_list; p; p = p->next)
		if (p->contrnr == contr)
			break;
	spin_unlock_irqrestore(&global_lock, flags);
	return p;
}


/* -------- plci management ------------------------------------------ */

static capidrv_plci *new_plci(capidrv_contr * card, int chan)
{
	capidrv_plci *plcip;

	plcip = (capidrv_plci *) kmalloc(sizeof(capidrv_plci), GFP_ATOMIC);

	if (plcip == 0)
		return 0;

	memset(plcip, 0, sizeof(capidrv_plci));
	plcip->state = ST_PLCI_NONE;
	plcip->plci = 0;
	plcip->msgid = 0;
	plcip->chan = chan;
	plcip->next = card->plci_list;
	card->plci_list = plcip;
	card->bchans[chan].plcip = plcip;

	return plcip;
}

static capidrv_plci *find_plci_by_plci(capidrv_contr * card, __u32 plci)
{
	capidrv_plci *p;
	for (p = card->plci_list; p; p = p->next)
		if (p->plci == plci)
			return p;
	return 0;
}

static capidrv_plci *find_plci_by_msgid(capidrv_contr * card, __u16 msgid)
{
	capidrv_plci *p;
	for (p = card->plci_list; p; p = p->next)
		if (p->msgid == msgid)
			return p;
	return 0;
}

static capidrv_plci *find_plci_by_ncci(capidrv_contr * card, __u32 ncci)
{
	capidrv_plci *p;
	for (p = card->plci_list; p; p = p->next)
		if (p->plci == (ncci & 0xffff))
			return p;
	return 0;
}

static void free_plci(capidrv_contr * card, capidrv_plci * plcip)
{
	capidrv_plci **pp;

	for (pp = &card->plci_list; *pp; pp = &(*pp)->next) {
		if (*pp == plcip) {
			*pp = (*pp)->next;
			card->bchans[plcip->chan].plcip = 0;
			card->bchans[plcip->chan].disconnecting = 0;
			card->bchans[plcip->chan].incoming = 0;
			kfree(plcip);
			return;
		}
	}
	printk(KERN_ERR "capidrv-%d: free_plci %p (0x%x) not found, Huh?\n",
	       card->contrnr, plcip, plcip->plci);
}

/* -------- ncci management ------------------------------------------ */

static inline capidrv_ncci *new_ncci(capidrv_contr * card,
				     capidrv_plci * plcip,
				     __u32 ncci)
{
	capidrv_ncci *nccip;

	nccip = (capidrv_ncci *) kmalloc(sizeof(capidrv_ncci), GFP_ATOMIC);

	if (nccip == 0)
		return 0;

	memset(nccip, 0, sizeof(capidrv_ncci));
	nccip->ncci = ncci;
	nccip->state = ST_NCCI_NONE;
	nccip->plcip = plcip;
	nccip->chan = plcip->chan;
	nccip->datahandle = 0;

	nccip->next = plcip->ncci_list;
	plcip->ncci_list = nccip;

	card->bchans[plcip->chan].nccip = nccip;

	return nccip;
}

static inline capidrv_ncci *find_ncci(capidrv_contr * card, __u32 ncci)
{
	capidrv_plci *plcip;
	capidrv_ncci *p;

	if ((plcip = find_plci_by_ncci(card, ncci)) == 0)
		return 0;

	for (p = plcip->ncci_list; p; p = p->next)
		if (p->ncci == ncci)
			return p;
	return 0;
}

static inline capidrv_ncci *find_ncci_by_msgid(capidrv_contr * card,
					       __u32 ncci, __u16 msgid)
{
	capidrv_plci *plcip;
	capidrv_ncci *p;

	if ((plcip = find_plci_by_ncci(card, ncci)) == 0)
		return 0;

	for (p = plcip->ncci_list; p; p = p->next)
		if (p->msgid == msgid)
			return p;
	return 0;
}

static void free_ncci(capidrv_contr * card, struct capidrv_ncci *nccip)
{
	struct capidrv_ncci **pp;

	for (pp = &(nccip->plcip->ncci_list); *pp; pp = &(*pp)->next) {
		if (*pp == nccip) {
			*pp = (*pp)->next;
			break;
		}
	}
	card->bchans[nccip->chan].nccip = 0;
	kfree(nccip);
}

static int capidrv_add_ack(struct capidrv_ncci *nccip,
		           __u16 datahandle, int len)
{
	struct ncci_datahandle_queue *n, **pp;

	n = (struct ncci_datahandle_queue *)
		kmalloc(sizeof(struct ncci_datahandle_queue), GFP_ATOMIC);
	if (!n) {
	   printk(KERN_ERR "capidrv: kmalloc ncci_datahandle failed\n");
	   return -1;
	}
	n->next = 0;
	n->datahandle = datahandle;
	n->len = len;
	for (pp = &nccip->ackqueue; *pp; pp = &(*pp)->next) ;
	*pp = n;
	return 0;
}

static int capidrv_del_ack(struct capidrv_ncci *nccip, __u16 datahandle)
{
	struct ncci_datahandle_queue **pp, *p;
	int len;

	for (pp = &nccip->ackqueue; *pp; pp = &(*pp)->next) {
 		if ((*pp)->datahandle == datahandle) {
			p = *pp;
			len = p->len;
			*pp = (*pp)->next;
		        kfree(p);
			return len;
		}
	}
	return -1;
}

/* -------- convert and send capi message ---------------------------- */

static void send_message(capidrv_contr * card, _cmsg * cmsg)
{
	struct sk_buff *skb;
	size_t len;
	capi_cmsg2message(cmsg, cmsg->buf);
	len = CAPIMSG_LEN(cmsg->buf);
	skb = alloc_skb(len, GFP_ATOMIC);
	memcpy(skb_put(skb, len), cmsg->buf, len);
	(*capifuncs->capi_put_message) (global.appid, skb);
	global.nsentctlpkt++;
}

/* -------- state machine -------------------------------------------- */

struct listenstatechange {
	int actstate;
	int nextstate;
	int event;
};

static struct listenstatechange listentable[] =
{
  {ST_LISTEN_NONE, ST_LISTEN_WAIT_CONF, EV_LISTEN_REQ},
  {ST_LISTEN_ACTIVE, ST_LISTEN_ACTIVE_WAIT_CONF, EV_LISTEN_REQ},
  {ST_LISTEN_WAIT_CONF, ST_LISTEN_NONE, EV_LISTEN_CONF_ERROR},
  {ST_LISTEN_ACTIVE_WAIT_CONF, ST_LISTEN_ACTIVE, EV_LISTEN_CONF_ERROR},
  {ST_LISTEN_WAIT_CONF, ST_LISTEN_NONE, EV_LISTEN_CONF_EMPTY},
  {ST_LISTEN_ACTIVE_WAIT_CONF, ST_LISTEN_NONE, EV_LISTEN_CONF_EMPTY},
  {ST_LISTEN_WAIT_CONF, ST_LISTEN_ACTIVE, EV_LISTEN_CONF_OK},
  {ST_LISTEN_ACTIVE_WAIT_CONF, ST_LISTEN_ACTIVE, EV_LISTEN_CONF_OK},
  {},
};

static void listen_change_state(capidrv_contr * card, int event)
{
	struct listenstatechange *p = listentable;
	while (p->event) {
		if (card->state == p->actstate && p->event == event) {
			if (debugmode)
				printk(KERN_DEBUG "capidrv-%d: listen_change_state %d -> %d\n",
				       card->contrnr, card->state, p->nextstate);
			card->state = p->nextstate;
			return;
		}
		p++;
	}
	printk(KERN_ERR "capidrv-%d: listen_change_state state=%d event=%d ????\n",
	       card->contrnr, card->state, event);

}

/* ------------------------------------------------------------------ */

static void p0(capidrv_contr * card, capidrv_plci * plci)
{
	isdn_ctrl cmd;

	card->bchans[plci->chan].contr = 0;
	cmd.command = ISDN_STAT_DHUP;
	cmd.driver = card->myid;
	cmd.arg = plci->chan;
	card->interface.statcallb(&cmd);
	free_plci(card, plci);
}

/* ------------------------------------------------------------------ */

struct plcistatechange {
	int actstate;
	int nextstate;
	int event;
	void (*changefunc) (capidrv_contr * card, capidrv_plci * plci);
};

static struct plcistatechange plcitable[] =
{
  /* P-0 */
  {ST_PLCI_NONE, ST_PLCI_OUTGOING, EV_PLCI_CONNECT_REQ, 0},
  {ST_PLCI_NONE, ST_PLCI_ALLOCATED, EV_PLCI_FACILITY_IND_UP, 0},
  {ST_PLCI_NONE, ST_PLCI_INCOMING, EV_PLCI_CONNECT_IND, 0},
  {ST_PLCI_NONE, ST_PLCI_RESUMEING, EV_PLCI_RESUME_REQ, 0},
  /* P-0.1 */
  {ST_PLCI_OUTGOING, ST_PLCI_NONE, EV_PLCI_CONNECT_CONF_ERROR, p0},
  {ST_PLCI_OUTGOING, ST_PLCI_ALLOCATED, EV_PLCI_CONNECT_CONF_OK, 0},
  /* P-1 */
  {ST_PLCI_ALLOCATED, ST_PLCI_ACTIVE, EV_PLCI_CONNECT_ACTIVE_IND, 0},
  {ST_PLCI_ALLOCATED, ST_PLCI_DISCONNECTING, EV_PLCI_DISCONNECT_REQ, 0},
  {ST_PLCI_ALLOCATED, ST_PLCI_DISCONNECTING, EV_PLCI_FACILITY_IND_DOWN, 0},
  {ST_PLCI_ALLOCATED, ST_PLCI_DISCONNECTED, EV_PLCI_DISCONNECT_IND, 0},
  /* P-ACT */
  {ST_PLCI_ACTIVE, ST_PLCI_DISCONNECTING, EV_PLCI_DISCONNECT_REQ, 0},
  {ST_PLCI_ACTIVE, ST_PLCI_DISCONNECTING, EV_PLCI_FACILITY_IND_DOWN, 0},
  {ST_PLCI_ACTIVE, ST_PLCI_DISCONNECTED, EV_PLCI_DISCONNECT_IND, 0},
  {ST_PLCI_ACTIVE, ST_PLCI_HELD, EV_PLCI_HOLD_IND, 0},
  {ST_PLCI_ACTIVE, ST_PLCI_DISCONNECTING, EV_PLCI_SUSPEND_IND, 0},
  /* P-2 */
  {ST_PLCI_INCOMING, ST_PLCI_DISCONNECTING, EV_PLCI_CONNECT_REJECT, 0},
  {ST_PLCI_INCOMING, ST_PLCI_FACILITY_IND, EV_PLCI_FACILITY_IND_UP, 0},
  {ST_PLCI_INCOMING, ST_PLCI_ACCEPTING, EV_PLCI_CONNECT_RESP, 0},
  {ST_PLCI_INCOMING, ST_PLCI_DISCONNECTING, EV_PLCI_DISCONNECT_REQ, 0},
  {ST_PLCI_INCOMING, ST_PLCI_DISCONNECTING, EV_PLCI_FACILITY_IND_DOWN, 0},
  {ST_PLCI_INCOMING, ST_PLCI_DISCONNECTED, EV_PLCI_DISCONNECT_IND, 0},
  {ST_PLCI_INCOMING, ST_PLCI_DISCONNECTING, EV_PLCI_CD_IND, 0},
  /* P-3 */
  {ST_PLCI_FACILITY_IND, ST_PLCI_DISCONNECTING, EV_PLCI_CONNECT_REJECT, 0},
  {ST_PLCI_FACILITY_IND, ST_PLCI_ACCEPTING, EV_PLCI_CONNECT_ACTIVE_IND, 0},
  {ST_PLCI_FACILITY_IND, ST_PLCI_DISCONNECTING, EV_PLCI_DISCONNECT_REQ, 0},
  {ST_PLCI_FACILITY_IND, ST_PLCI_DISCONNECTING, EV_PLCI_FACILITY_IND_DOWN, 0},
  {ST_PLCI_FACILITY_IND, ST_PLCI_DISCONNECTED, EV_PLCI_DISCONNECT_IND, 0},
  /* P-4 */
  {ST_PLCI_ACCEPTING, ST_PLCI_ACTIVE, EV_PLCI_CONNECT_ACTIVE_IND, 0},
  {ST_PLCI_ACCEPTING, ST_PLCI_DISCONNECTING, EV_PLCI_DISCONNECT_REQ, 0},
  {ST_PLCI_ACCEPTING, ST_PLCI_DISCONNECTING, EV_PLCI_FACILITY_IND_DOWN, 0},
  {ST_PLCI_ACCEPTING, ST_PLCI_DISCONNECTED, EV_PLCI_DISCONNECT_IND, 0},
  /* P-5 */
  {ST_PLCI_DISCONNECTING, ST_PLCI_DISCONNECTED, EV_PLCI_DISCONNECT_IND, 0},
  /* P-6 */
  {ST_PLCI_DISCONNECTED, ST_PLCI_NONE, EV_PLCI_DISCONNECT_RESP, p0},
  /* P-0.Res */
  {ST_PLCI_RESUMEING, ST_PLCI_NONE, EV_PLCI_RESUME_CONF_ERROR, p0},
  {ST_PLCI_RESUMEING, ST_PLCI_RESUME, EV_PLCI_RESUME_CONF_OK, 0},
  /* P-RES */
  {ST_PLCI_RESUME, ST_PLCI_ACTIVE, EV_PLCI_RESUME_IND, 0},
  /* P-HELD */
  {ST_PLCI_HELD, ST_PLCI_ACTIVE, EV_PLCI_RETRIEVE_IND, 0},
  {},
};

static void plci_change_state(capidrv_contr * card, capidrv_plci * plci, int event)
{
	struct plcistatechange *p = plcitable;
	while (p->event) {
		if (plci->state == p->actstate && p->event == event) {
			if (debugmode)
				printk(KERN_DEBUG "capidrv-%d: plci_change_state:0x%x %d -> %d\n",
				  card->contrnr, plci->plci, plci->state, p->nextstate);
			plci->state = p->nextstate;
			if (p->changefunc)
				p->changefunc(card, plci);
			return;
		}
		p++;
	}
	printk(KERN_ERR "capidrv-%d: plci_change_state:0x%x state=%d event=%d ????\n",
	       card->contrnr, plci->plci, plci->state, event);
}

/* ------------------------------------------------------------------ */

static _cmsg cmsg;

static void n0(capidrv_contr * card, capidrv_ncci * ncci)
{
	isdn_ctrl cmd;

	capi_fill_DISCONNECT_REQ(&cmsg,
				 global.appid,
				 card->msgid++,
				 ncci->plcip->plci,
				 0,	/* BChannelinformation */
				 0,	/* Keypadfacility */
				 0,	/* Useruserdata */   /* $$$$ */
				 0	/* Facilitydataarray */
	);
	send_message(card, &cmsg);
	plci_change_state(card, ncci->plcip, EV_PLCI_DISCONNECT_REQ);

	cmd.command = ISDN_STAT_BHUP;
	cmd.driver = card->myid;
	cmd.arg = ncci->chan;
	card->interface.statcallb(&cmd);
	free_ncci(card, ncci);
}

/* ------------------------------------------------------------------ */

struct nccistatechange {
	int actstate;
	int nextstate;
	int event;
	void (*changefunc) (capidrv_contr * card, capidrv_ncci * ncci);
};

static struct nccistatechange nccitable[] =
{
  /* N-0 */
  {ST_NCCI_NONE, ST_NCCI_OUTGOING, EV_NCCI_CONNECT_B3_REQ, 0},
  {ST_NCCI_NONE, ST_NCCI_INCOMING, EV_NCCI_CONNECT_B3_IND, 0},
  /* N-0.1 */
  {ST_NCCI_OUTGOING, ST_NCCI_ALLOCATED, EV_NCCI_CONNECT_B3_CONF_OK, 0},
  {ST_NCCI_OUTGOING, ST_NCCI_NONE, EV_NCCI_CONNECT_B3_CONF_ERROR, n0},
  /* N-1 */
  {ST_NCCI_INCOMING, ST_NCCI_DISCONNECTING, EV_NCCI_CONNECT_B3_REJECT, 0},
  {ST_NCCI_INCOMING, ST_NCCI_ALLOCATED, EV_NCCI_CONNECT_B3_RESP, 0},
  {ST_NCCI_INCOMING, ST_NCCI_DISCONNECTED, EV_NCCI_DISCONNECT_B3_IND, 0},
  {ST_NCCI_INCOMING, ST_NCCI_DISCONNECTING, EV_NCCI_DISCONNECT_B3_REQ, 0},
  /* N-2 */
  {ST_NCCI_ALLOCATED, ST_NCCI_ACTIVE, EV_NCCI_CONNECT_B3_ACTIVE_IND, 0},
  {ST_NCCI_ALLOCATED, ST_NCCI_DISCONNECTED, EV_NCCI_DISCONNECT_B3_IND, 0},
  {ST_NCCI_ALLOCATED, ST_NCCI_DISCONNECTING, EV_NCCI_DISCONNECT_B3_REQ, 0},
  /* N-ACT */
  {ST_NCCI_ACTIVE, ST_NCCI_ACTIVE, EV_NCCI_RESET_B3_IND, 0},
  {ST_NCCI_ACTIVE, ST_NCCI_RESETING, EV_NCCI_RESET_B3_REQ, 0},
  {ST_NCCI_ACTIVE, ST_NCCI_DISCONNECTED, EV_NCCI_DISCONNECT_B3_IND, 0},
  {ST_NCCI_ACTIVE, ST_NCCI_DISCONNECTING, EV_NCCI_DISCONNECT_B3_REQ, 0},
  /* N-3 */
  {ST_NCCI_RESETING, ST_NCCI_ACTIVE, EV_NCCI_RESET_B3_IND, 0},
  {ST_NCCI_RESETING, ST_NCCI_DISCONNECTED, EV_NCCI_DISCONNECT_B3_IND, 0},
  {ST_NCCI_RESETING, ST_NCCI_DISCONNECTING, EV_NCCI_DISCONNECT_B3_REQ, 0},
  /* N-4 */
  {ST_NCCI_DISCONNECTING, ST_NCCI_DISCONNECTED, EV_NCCI_DISCONNECT_B3_IND, 0},
  {ST_NCCI_DISCONNECTING, ST_NCCI_PREVIOUS, EV_NCCI_DISCONNECT_B3_CONF_ERROR,0},
  /* N-5 */
  {ST_NCCI_DISCONNECTED, ST_NCCI_NONE, EV_NCCI_DISCONNECT_B3_RESP, n0},
  {},
};

static void ncci_change_state(capidrv_contr * card, capidrv_ncci * ncci, int event)
{
	struct nccistatechange *p = nccitable;
	while (p->event) {
		if (ncci->state == p->actstate && p->event == event) {
			if (debugmode)
				printk(KERN_DEBUG "capidrv-%d: ncci_change_state:0x%x %d -> %d\n",
				  card->contrnr, ncci->ncci, ncci->state, p->nextstate);
			if (p->nextstate == ST_NCCI_PREVIOUS) {
				ncci->state = ncci->oldstate;
				ncci->oldstate = p->actstate;
			} else {
				ncci->oldstate = p->actstate;
				ncci->state = p->nextstate;
			}
			if (p->changefunc)
				p->changefunc(card, ncci);
			return;
		}
		p++;
	}
	printk(KERN_ERR "capidrv-%d: ncci_change_state:0x%x state=%d event=%d ????\n",
	       card->contrnr, ncci->ncci, ncci->state, event);
}

/* ------------------------------------------------------------------- */

static inline int new_bchan(capidrv_contr * card)
{
	int i;
	for (i = 0; i < card->nbchan; i++) {
		if (card->bchans[i].plcip == 0) {
			card->bchans[i].disconnecting = 0;
			return i;
		}
	}
	return -1;
}

/* ------------------------------------------------------------------- */

static void handle_controller(_cmsg * cmsg)
{
	capidrv_contr *card = findcontrbynumber(cmsg->adr.adrController & 0x7f);

	if (!card) {
		printk(KERN_ERR "capidrv: %s from unknown controller 0x%x\n",
		       capi_cmd2str(cmsg->Command, cmsg->Subcommand),
		       cmsg->adr.adrController & 0x7f);
		return;
	}
	switch (CAPICMD(cmsg->Command, cmsg->Subcommand)) {

	case CAPI_LISTEN_CONF:	/* Controller */
		if (debugmode)
			printk(KERN_DEBUG "capidrv-%d: listenconf Info=0x%4x (%s) cipmask=0x%x\n",
			       card->contrnr, cmsg->Info, capi_info2str(cmsg->Info), card->cipmask);
		if (cmsg->Info) {
			listen_change_state(card, EV_LISTEN_CONF_ERROR);
		} else if (card->cipmask == 0) {
			listen_change_state(card, EV_LISTEN_CONF_EMPTY);
		} else {
			listen_change_state(card, EV_LISTEN_CONF_OK);
		}
		break;

	case CAPI_MANUFACTURER_IND:	/* Controller */
		if (   cmsg->ManuID == 0x214D5641
		    && cmsg->Class == 0
		    && cmsg->Function == 1) {
		   __u8  *data = cmsg->ManuData+3;
		   __u16  len = cmsg->ManuData[0];
		   __u16 layer;
		   int direction;
		   if (len == 255) {
		      len = (cmsg->ManuData[1] | (cmsg->ManuData[2] << 8));
		      data += 2;
		   }
		   len -= 2;
		   layer = ((*(data-1)) << 8) | *(data-2);
		   if (layer & 0x300)
			direction = (layer & 0x200) ? 0 : 1;
		   else direction = (layer & 0x800) ? 0 : 1;
		   if (layer & 0x0C00) {
		   	if ((layer & 0xff) == 0x80) {
		           handle_dtrace_data(card, direction, 1, data, len);
		           break;
		   	}
		   } else if ((layer & 0xff) < 0x80) {
		      handle_dtrace_data(card, direction, 0, data, len);
		      break;
		   }
	           printk(KERN_INFO "capidrv-%d: %s from controller 0x%x layer 0x%x, ignored\n",
                        card->contrnr, 
			capi_cmd2str(cmsg->Command, cmsg->Subcommand),
			cmsg->adr.adrController, layer);
                   break;
		}
		goto ignored;
	case CAPI_MANUFACTURER_CONF:	/* Controller */
		if (cmsg->ManuID == 0x214D5641) {
		   char *s = 0;
		   switch (cmsg->Class) {
		      case 0: break;
		      case 1: s = "unknown class"; break;
		      case 2: s = "unknown function"; break;
		      default: s = "unkown error"; break;
		   }
		   if (s)
	           printk(KERN_INFO "capidrv-%d: %s from controller 0x%x function %d: %s\n",
			card->contrnr,
			capi_cmd2str(cmsg->Command, cmsg->Subcommand),
			cmsg->adr.adrController,
			cmsg->Function, s);
		   break;
		}
		goto ignored;
	case CAPI_FACILITY_IND:	/* Controller/plci/ncci */
		goto ignored;
	case CAPI_FACILITY_CONF:	/* Controller/plci/ncci */
		goto ignored;
	case CAPI_INFO_IND:	/* Controller/plci */
		goto ignored;
	case CAPI_INFO_CONF:	/* Controller/plci */
		goto ignored;

	default:
		printk(KERN_ERR "capidrv-%d: got %s from controller 0x%x ???",
		       card->contrnr,
		       capi_cmd2str(cmsg->Command, cmsg->Subcommand),
		       cmsg->adr.adrController);
	}
	return;

      ignored:
	printk(KERN_INFO "capidrv-%d: %s from controller 0x%x ignored\n",
	       card->contrnr,
	       capi_cmd2str(cmsg->Command, cmsg->Subcommand),
	       cmsg->adr.adrController);
}

static void handle_incoming_call(capidrv_contr * card, _cmsg * cmsg)
{
	capidrv_plci *plcip;
	capidrv_bchan *bchan;
	isdn_ctrl cmd;
	int chan;

	if ((chan = new_bchan(card)) == -1) {
		printk(KERN_ERR "capidrv-%d: incoming call on not existing bchan ?\n", card->contrnr);
		return;
	}
	bchan = &card->bchans[chan];
	if ((plcip = new_plci(card, chan)) == 0) {
		printk(KERN_ERR "capidrv-%d: incoming call: no memory, sorry.\n", card->contrnr);
		return;
	}
	bchan->incoming = 1;
	plcip->plci = cmsg->adr.adrPLCI;
	plci_change_state(card, plcip, EV_PLCI_CONNECT_IND);

	cmd.command = ISDN_STAT_ICALL;
	cmd.driver = card->myid;
	cmd.arg = chan;
	memset(&cmd.parm.setup, 0, sizeof(cmd.parm.setup));
	strncpy(cmd.parm.setup.phone,
	        cmsg->CallingPartyNumber + 3,
		cmsg->CallingPartyNumber[0] - 2);
	strncpy(cmd.parm.setup.eazmsn,
	        cmsg->CalledPartyNumber + 2,
		cmsg->CalledPartyNumber[0] - 1);
	cmd.parm.setup.si1 = cip2si1(cmsg->CIPValue);
	cmd.parm.setup.si2 = cip2si2(cmsg->CIPValue);
	cmd.parm.setup.plan = cmsg->CallingPartyNumber[1];
	cmd.parm.setup.screen = cmsg->CallingPartyNumber[2];

	printk(KERN_INFO "capidrv-%d: incoming call %s,%d,%d,%s\n", 
			card->contrnr,
			cmd.parm.setup.phone,
			cmd.parm.setup.si1,
			cmd.parm.setup.si2,
			cmd.parm.setup.eazmsn);

	if (cmd.parm.setup.si1 == 1 && cmd.parm.setup.si2 != 0) {
		printk(KERN_INFO "capidrv-%d: patching si2=%d to 0 for VBOX\n", 
			card->contrnr,
			cmd.parm.setup.si2);
		cmd.parm.setup.si2 = 0;
	}

	switch (card->interface.statcallb(&cmd)) {
	case 0:
	case 3:
		/* No device matching this call.
		 * and isdn_common.c has send a HANGUP command
		 * which is ignored in state ST_PLCI_INCOMING,
		 * so we send RESP to ignore the call
		 */
		capi_cmsg_answer(cmsg);
		cmsg->Reject = 1;	/* ignore */
		send_message(card, cmsg);
		plci_change_state(card, plcip, EV_PLCI_CONNECT_REJECT);
		printk(KERN_INFO "capidrv-%d: incoming call %s,%d,%d,%s ignored\n",
			card->contrnr,
			cmd.parm.setup.phone,
			cmd.parm.setup.si1,
			cmd.parm.setup.si2,
			cmd.parm.setup.eazmsn);
		break;
	case 1:
		/* At least one device matching this call (RING on ttyI)
		 * HL-driver may send ALERTING on the D-channel in this
		 * case.
		 * really means: RING on ttyI or a net interface
		 * accepted this call already.
		 *
		 * If the call was accepted, state has already changed,
		 * and CONNECT_RESP already sent.
		 */
		if (plcip->state == ST_PLCI_INCOMING) {
			printk(KERN_INFO "capidrv-%d: incoming call %s,%d,%d,%s tty alerting\n",
				card->contrnr,
				cmd.parm.setup.phone,
				cmd.parm.setup.si1,
				cmd.parm.setup.si2,
				cmd.parm.setup.eazmsn);
			capi_fill_ALERT_REQ(cmsg,
					    global.appid,
					    card->msgid++,
					    plcip->plci,	/* adr */
					    0,	/* BChannelinformation */
					    0,	/* Keypadfacility */
					    0,	/* Useruserdata */
					    0	/* Facilitydataarray */
			);
			plcip->msgid = cmsg->Messagenumber;
			send_message(card, cmsg);
		} else {
			printk(KERN_INFO "capidrv-%d: incoming call %s,%d,%d,%s on netdev\n",
				card->contrnr,
				cmd.parm.setup.phone,
				cmd.parm.setup.si1,
				cmd.parm.setup.si2,
				cmd.parm.setup.eazmsn);
		}
		break;

	case 2:		/* Call will be rejected. */
		capi_cmsg_answer(cmsg);
		cmsg->Reject = 2;	/* reject call, normal call clearing */
		send_message(card, cmsg);
		plci_change_state(card, plcip, EV_PLCI_CONNECT_REJECT);
		break;

	default:
		/* An error happened. (Invalid parameters for example.) */
		capi_cmsg_answer(cmsg);
		cmsg->Reject = 8;	/* reject call,
					   destination out of order */
		send_message(card, cmsg);
		plci_change_state(card, plcip, EV_PLCI_CONNECT_REJECT);
		break;
	}
	return;
}

static void handle_plci(_cmsg * cmsg)
{
	capidrv_contr *card = findcontrbynumber(cmsg->adr.adrController & 0x7f);
	capidrv_plci *plcip;
	isdn_ctrl cmd;

	if (!card) {
		printk(KERN_ERR "capidrv: %s from unknown controller 0x%x\n",
		       capi_cmd2str(cmsg->Command, cmsg->Subcommand),
		       cmsg->adr.adrController & 0x7f);
		return;
	}
	switch (CAPICMD(cmsg->Command, cmsg->Subcommand)) {

	case CAPI_DISCONNECT_IND:	/* plci */
		if (cmsg->Reason) {
			printk(KERN_INFO "capidrv-%d: %s reason 0x%x (%s) for plci 0x%x\n",
			   card->contrnr,
			   capi_cmd2str(cmsg->Command, cmsg->Subcommand),
			       cmsg->Reason, capi_info2str(cmsg->Reason), cmsg->adr.adrPLCI);
		}
		if (!(plcip = find_plci_by_plci(card, cmsg->adr.adrPLCI))) {
			capi_cmsg_answer(cmsg);
			send_message(card, cmsg);
			goto notfound;
		}
		card->bchans[plcip->chan].disconnecting = 1;
		plci_change_state(card, plcip, EV_PLCI_DISCONNECT_IND);
		capi_cmsg_answer(cmsg);
		send_message(card, cmsg);
		plci_change_state(card, plcip, EV_PLCI_DISCONNECT_RESP);
		break;

	case CAPI_DISCONNECT_CONF:	/* plci */
		if (cmsg->Info) {
			printk(KERN_INFO "capidrv-%d: %s info 0x%x (%s) for plci 0x%x\n",
			   card->contrnr,
			   capi_cmd2str(cmsg->Command, cmsg->Subcommand),
			       cmsg->Info, capi_info2str(cmsg->Info), 
			       cmsg->adr.adrPLCI);
		}
		if (!(plcip = find_plci_by_plci(card, cmsg->adr.adrPLCI)))
			goto notfound;

		card->bchans[plcip->chan].disconnecting = 1;
		break;

	case CAPI_ALERT_CONF:	/* plci */
		if (cmsg->Info) {
			printk(KERN_INFO "capidrv-%d: %s info 0x%x (%s) for plci 0x%x\n",
			   card->contrnr,
			   capi_cmd2str(cmsg->Command, cmsg->Subcommand),
			       cmsg->Info, capi_info2str(cmsg->Info), 
			       cmsg->adr.adrPLCI);
		}
		break;

	case CAPI_CONNECT_IND:	/* plci */
		handle_incoming_call(card, cmsg);
		break;

	case CAPI_CONNECT_CONF:	/* plci */
		if (cmsg->Info) {
			printk(KERN_INFO "capidrv-%d: %s info 0x%x (%s) for plci 0x%x\n",
			   card->contrnr,
			   capi_cmd2str(cmsg->Command, cmsg->Subcommand),
			       cmsg->Info, capi_info2str(cmsg->Info), 
			       cmsg->adr.adrPLCI);
		}
		if (!(plcip = find_plci_by_msgid(card, cmsg->Messagenumber)))
			goto notfound;

		plcip->plci = cmsg->adr.adrPLCI;
		if (cmsg->Info) {
			plci_change_state(card, plcip, EV_PLCI_CONNECT_CONF_ERROR);
		} else {
			plci_change_state(card, plcip, EV_PLCI_CONNECT_CONF_OK);
		}
		break;

	case CAPI_CONNECT_ACTIVE_IND:	/* plci */

		if (!(plcip = find_plci_by_plci(card, cmsg->adr.adrPLCI)))
			goto notfound;

		if (card->bchans[plcip->chan].incoming) {
			capi_cmsg_answer(cmsg);
			send_message(card, cmsg);
			plci_change_state(card, plcip, EV_PLCI_CONNECT_ACTIVE_IND);
		} else {
			capidrv_ncci *nccip;
			capi_cmsg_answer(cmsg);
			send_message(card, cmsg);

			nccip = new_ncci(card, plcip, cmsg->adr.adrPLCI);

			if (!nccip) {
				printk(KERN_ERR "capidrv-%d: no mem for ncci, sorry\n", card->contrnr);
				break;	/* $$$$ */
			}
			capi_fill_CONNECT_B3_REQ(cmsg,
						 global.appid,
						 card->msgid++,
						 plcip->plci,	/* adr */
						 0	/* NCPI */
			);
			nccip->msgid = cmsg->Messagenumber;
			send_message(card, cmsg);
			cmd.command = ISDN_STAT_DCONN;
			cmd.driver = card->myid;
			cmd.arg = plcip->chan;
			card->interface.statcallb(&cmd);
			plci_change_state(card, plcip, EV_PLCI_CONNECT_ACTIVE_IND);
			ncci_change_state(card, nccip, EV_NCCI_CONNECT_B3_REQ);
		}
		break;

	case CAPI_INFO_IND:	/* Controller/plci */

		if (!(plcip = find_plci_by_plci(card, cmsg->adr.adrPLCI)))
			goto notfound;

		if (cmsg->InfoNumber == 0x4000) {
			if (cmsg->InfoElement[0] == 4) {
				cmd.command = ISDN_STAT_CINF;
				cmd.driver = card->myid;
				cmd.arg = plcip->chan;
				sprintf(cmd.parm.num, "%lu",
					(unsigned long)
					((__u32) cmsg->InfoElement[1]
				  | ((__u32) (cmsg->InfoElement[2]) << 8)
				 | ((__u32) (cmsg->InfoElement[3]) << 16)
					 | ((__u32) (cmsg->InfoElement[4]) << 24)));
				card->interface.statcallb(&cmd);
				break;
			}
		}
		printk(KERN_ERR "capidrv-%d: %s\n",
				card->contrnr, capi_cmsg2str(cmsg));
		break;

	case CAPI_CONNECT_ACTIVE_CONF:		/* plci */
		goto ignored;
	case CAPI_SELECT_B_PROTOCOL_CONF:	/* plci */
		goto ignored;
	case CAPI_FACILITY_IND:	/* Controller/plci/ncci */
		goto ignored;
	case CAPI_FACILITY_CONF:	/* Controller/plci/ncci */
		goto ignored;

	case CAPI_INFO_CONF:	/* Controller/plci */
		goto ignored;

	default:
		printk(KERN_ERR "capidrv-%d: got %s for plci 0x%x ???",
		       card->contrnr,
		       capi_cmd2str(cmsg->Command, cmsg->Subcommand),
		       cmsg->adr.adrPLCI);
	}
	return;
      ignored:
	printk(KERN_INFO "capidrv-%d: %s for plci 0x%x ignored\n",
	       card->contrnr,
	       capi_cmd2str(cmsg->Command, cmsg->Subcommand),
	       cmsg->adr.adrPLCI);
	return;
      notfound:
	printk(KERN_ERR "capidrv-%d: %s: plci 0x%x not found\n",
	       card->contrnr,
	       capi_cmd2str(cmsg->Command, cmsg->Subcommand),
	       cmsg->adr.adrPLCI);
	return;
}

static void handle_ncci(_cmsg * cmsg)
{
	capidrv_contr *card = findcontrbynumber(cmsg->adr.adrController & 0x7f);
	capidrv_plci *plcip;
	capidrv_ncci *nccip;
	isdn_ctrl cmd;
	int len;

	if (!card) {
		printk(KERN_ERR "capidrv: %s from unknown controller 0x%x\n",
		       capi_cmd2str(cmsg->Command, cmsg->Subcommand),
		       cmsg->adr.adrController & 0x7f);
		return;
	}
	switch (CAPICMD(cmsg->Command, cmsg->Subcommand)) {

	case CAPI_CONNECT_B3_ACTIVE_IND:	/* ncci */
		if (!(nccip = find_ncci(card, cmsg->adr.adrNCCI)))
			goto notfound;

		capi_cmsg_answer(cmsg);
		send_message(card, cmsg);
		ncci_change_state(card, nccip, EV_NCCI_CONNECT_B3_ACTIVE_IND);

		cmd.command = ISDN_STAT_BCONN;
		cmd.driver = card->myid;
		cmd.arg = nccip->chan;
		card->interface.statcallb(&cmd);

		printk(KERN_INFO "capidrv-%d: chan %d up with ncci 0x%x\n",
		       card->contrnr, nccip->chan, nccip->ncci);
		break;

	case CAPI_CONNECT_B3_ACTIVE_CONF:	/* ncci */
		goto ignored;

	case CAPI_CONNECT_B3_IND:	/* ncci */

		plcip = find_plci_by_ncci(card, cmsg->adr.adrNCCI);
		if (plcip) {
			nccip = new_ncci(card, plcip, cmsg->adr.adrNCCI);
			if (nccip) {
				ncci_change_state(card, nccip, EV_NCCI_CONNECT_B3_IND);
				capi_fill_CONNECT_B3_RESP(cmsg,
							  global.appid,
							  card->msgid++,
							  nccip->ncci,	/* adr */
							  0,	/* Reject */
							  0	/* NCPI */
				);
				send_message(card, cmsg);
				ncci_change_state(card, nccip, EV_NCCI_CONNECT_B3_RESP);
				break;
			}
			printk(KERN_ERR "capidrv-%d: no mem for ncci, sorry\n",							card->contrnr);
		} else {
			printk(KERN_ERR "capidrv-%d: %s: plci for ncci 0x%x not found\n",
			   card->contrnr,
			   capi_cmd2str(cmsg->Command, cmsg->Subcommand),
			       cmsg->adr.adrNCCI);
		}
		capi_fill_CONNECT_B3_RESP(cmsg,
					  global.appid,
					  card->msgid++,
					  cmsg->adr.adrNCCI,
					  2,	/* Reject */
					  0	/* NCPI */
		);
		send_message(card, cmsg);
		break;

	case CAPI_CONNECT_B3_CONF:	/* ncci */

		if (!(nccip = find_ncci_by_msgid(card,
						 cmsg->adr.adrNCCI,
						 cmsg->Messagenumber)))
			goto notfound;

		nccip->ncci = cmsg->adr.adrNCCI;
		if (cmsg->Info) {
			printk(KERN_INFO "capidrv-%d: %s info 0x%x (%s) for ncci 0x%x\n",
			   card->contrnr,
			   capi_cmd2str(cmsg->Command, cmsg->Subcommand),
			       cmsg->Info, capi_info2str(cmsg->Info), 
			       cmsg->adr.adrNCCI);
		}

		if (cmsg->Info)
			ncci_change_state(card, nccip, EV_NCCI_CONNECT_B3_CONF_ERROR);
		else
			ncci_change_state(card, nccip, EV_NCCI_CONNECT_B3_CONF_OK);
		break;

	case CAPI_CONNECT_B3_T90_ACTIVE_IND:	/* ncci */
		capi_cmsg_answer(cmsg);
		send_message(card, cmsg);
		break;

	case CAPI_DATA_B3_IND:	/* ncci */
		/* handled in handle_data() */
		goto ignored;

	case CAPI_DATA_B3_CONF:	/* ncci */
		if (!(nccip = find_ncci(card, cmsg->adr.adrNCCI)))
			goto notfound;

		len = capidrv_del_ack(nccip, cmsg->DataHandle);
		if (len < 0)
			break;
	        cmd.command = ISDN_STAT_BSENT;
	        cmd.driver = card->myid;
	        cmd.arg = nccip->chan;
		cmd.parm.length = len;
	        card->interface.statcallb(&cmd);
		break;

	case CAPI_DISCONNECT_B3_IND:	/* ncci */
		if (!(nccip = find_ncci(card, cmsg->adr.adrNCCI)))
			goto notfound;

		card->bchans[nccip->chan].disconnecting = 1;
		ncci_change_state(card, nccip, EV_NCCI_DISCONNECT_B3_IND);
		capi_cmsg_answer(cmsg);
		send_message(card, cmsg);
		ncci_change_state(card, nccip, EV_NCCI_DISCONNECT_B3_RESP);
		break;

	case CAPI_DISCONNECT_B3_CONF:	/* ncci */
		if (!(nccip = find_ncci(card, cmsg->adr.adrNCCI)))
			goto notfound;
		if (cmsg->Info) {
			printk(KERN_INFO "capidrv-%d: %s info 0x%x (%s) for ncci 0x%x\n",
			   card->contrnr,
			   capi_cmd2str(cmsg->Command, cmsg->Subcommand),
			       cmsg->Info, capi_info2str(cmsg->Info), 
			       cmsg->adr.adrNCCI);
			ncci_change_state(card, nccip, EV_NCCI_DISCONNECT_B3_CONF_ERROR);
		}
		break;

	case CAPI_RESET_B3_IND:	/* ncci */
		if (!(nccip = find_ncci(card, cmsg->adr.adrNCCI)))
			goto notfound;
		ncci_change_state(card, nccip, EV_NCCI_RESET_B3_IND);
		capi_cmsg_answer(cmsg);
		send_message(card, cmsg);
		break;

	case CAPI_RESET_B3_CONF:	/* ncci */
		goto ignored;	/* $$$$ */

	case CAPI_FACILITY_IND:	/* Controller/plci/ncci */
		goto ignored;
	case CAPI_FACILITY_CONF:	/* Controller/plci/ncci */
		goto ignored;

	default:
		printk(KERN_ERR "capidrv-%d: got %s for ncci 0x%x ???",
		       card->contrnr,
		       capi_cmd2str(cmsg->Command, cmsg->Subcommand),
		       cmsg->adr.adrNCCI);
	}
	return;
      ignored:
	printk(KERN_INFO "capidrv-%d: %s for ncci 0x%x ignored\n",
	       card->contrnr,
	       capi_cmd2str(cmsg->Command, cmsg->Subcommand),
	       cmsg->adr.adrNCCI);
	return;
      notfound:
	printk(KERN_ERR "capidrv-%d: %s: ncci 0x%x not found\n",
	       card->contrnr,
	       capi_cmd2str(cmsg->Command, cmsg->Subcommand),
	       cmsg->adr.adrNCCI);
}


static void handle_data(_cmsg * cmsg, struct sk_buff *skb)
{
	capidrv_contr *card = findcontrbynumber(cmsg->adr.adrController & 0x7f);
	capidrv_ncci *nccip;

	if (!card) {
		printk(KERN_ERR "capidrv: %s from unknown controller 0x%x\n",
		       capi_cmd2str(cmsg->Command, cmsg->Subcommand),
		       cmsg->adr.adrController & 0x7f);
		kfree_skb(skb);
		return;
	}
	if (!(nccip = find_ncci(card, cmsg->adr.adrNCCI))) {
		printk(KERN_ERR "capidrv-%d: %s: ncci 0x%x not found\n",
		       card->contrnr,
		       capi_cmd2str(cmsg->Command, cmsg->Subcommand),
		       cmsg->adr.adrNCCI);
		kfree_skb(skb);
		return;
	}
	(void) skb_pull(skb, CAPIMSG_LEN(skb->data));
	card->interface.rcvcallb_skb(card->myid, nccip->chan, skb);
	capi_cmsg_answer(cmsg);
	send_message(card, cmsg);
}

static _cmsg s_cmsg;

static void capidrv_signal(__u16 applid, void *dummy)
{
	struct sk_buff *skb = 0;

	while ((*capifuncs->capi_get_message) (global.appid, &skb) == CAPI_NOERROR) {
		capi_message2cmsg(&s_cmsg, skb->data);
		if (debugmode > 2)
			printk(KERN_DEBUG "capidrv_signal: applid=%d %s\n",
					applid, capi_cmsg2str(&s_cmsg));

		if (s_cmsg.Command == CAPI_DATA_B3
		    && s_cmsg.Subcommand == CAPI_IND) {
			handle_data(&s_cmsg, skb);
			global.nrecvdatapkt++;
			continue;
		}
		if ((s_cmsg.adr.adrController & 0xffffff00) == 0)
			handle_controller(&s_cmsg);
		else if ((s_cmsg.adr.adrPLCI & 0xffff0000) == 0)
			handle_plci(&s_cmsg);
		else
			handle_ncci(&s_cmsg);
		/*
		 * data of skb used in s_cmsg,
		 * free data when s_cmsg is not used again
		 * thanks to Lars Heete <hel@admin.de>
		 */
		kfree_skb(skb);
		global.nrecvctlpkt++;
	}
}

/* ------------------------------------------------------------------- */

#define PUTBYTE_TO_STATUS(card, byte) \
	do { \
		*(card)->q931_write++ = (byte); \
        	if ((card)->q931_write > (card)->q931_end) \
	  		(card)->q931_write = (card)->q931_buf; \
	} while (0)

static void handle_dtrace_data(capidrv_contr *card,
			     int send, int level2, __u8 *data, __u16 len)
{
    	__u8 *p, *end;
    	isdn_ctrl cmd;

    	if (!len) {
		printk(KERN_DEBUG "capidrv-%d: avmb1_q931_data: len == %d\n",
				card->contrnr, len);
		return;
	}

	if (level2) {
		PUTBYTE_TO_STATUS(card, 'D');
		PUTBYTE_TO_STATUS(card, '2');
        	PUTBYTE_TO_STATUS(card, send ? '>' : '<');
        	PUTBYTE_TO_STATUS(card, ':');
	} else {
        	PUTBYTE_TO_STATUS(card, 'D');
        	PUTBYTE_TO_STATUS(card, '3');
        	PUTBYTE_TO_STATUS(card, send ? '>' : '<');
        	PUTBYTE_TO_STATUS(card, ':');
    	}

	for (p = data, end = data+len; p < end; p++) {
		__u8 w;
		PUTBYTE_TO_STATUS(card, ' ');
		w = (*p >> 4) & 0xf;
		PUTBYTE_TO_STATUS(card, (w < 10) ? '0'+w : 'A'-10+w);
		w = *p & 0xf;
		PUTBYTE_TO_STATUS(card, (w < 10) ? '0'+w : 'A'-10+w);
	}
	PUTBYTE_TO_STATUS(card, '\n');

	cmd.command = ISDN_STAT_STAVAIL;
	cmd.driver = card->myid;
	cmd.arg = len*3+5;
	card->interface.statcallb(&cmd);
}

/* ------------------------------------------------------------------- */

static _cmsg cmdcmsg;

static int capidrv_ioctl(isdn_ctrl * c, capidrv_contr * card)
{
	switch (c->arg) {
	case 1:
		debugmode = (int)(*((unsigned int *)c->parm.num));
		printk(KERN_DEBUG "capidrv-%d: debugmode=%d\n",
				card->contrnr, debugmode);
		return 0;
	default:
		printk(KERN_DEBUG "capidrv-%d: capidrv_ioctl(%ld) called ??\n",
				card->contrnr, c->arg);
		return -EINVAL;
	}
	return -EINVAL;
}

/*
 * Handle leased lines (CAPI-Bundling)
 */

struct internal_bchannelinfo {
   unsigned short channelalloc;
   unsigned short operation;
   unsigned char  cmask[31];
};

static int decodeFVteln(char *teln, unsigned long *bmaskp, int *activep)
{
	unsigned long bmask = 0;
	int active = !0;
	char *s;
	int i;

	if (strncmp(teln, "FV:", 3) != 0)
		return 1;
	s = teln + 3;
	while (*s && *s == ' ') s++;
	if (!*s) return -2;
	if (*s == 'p' || *s == 'P') {
		active = 0;
		s++;
	}
	if (*s == 'a' || *s == 'A') {
		active = !0;
		s++;
	}
	while (*s) {
		int digit1 = 0;
		int digit2 = 0;
		if (!isdigit(*s)) return -3;
		while (isdigit(*s)) { digit1 = digit1*10 + (*s - '0'); s++; }
		if (digit1 <= 0 && digit1 > 30) return -4;
		if (*s == 0 || *s == ',' || *s == ' ') {
			bmask |= (1 << digit1);
			digit1 = 0;
			if (*s) s++;
			continue;
		}
		if (*s != '-') return -5;
		s++;
		if (!isdigit(*s)) return -3;
		while (isdigit(*s)) { digit2 = digit2*10 + (*s - '0'); s++; }
		if (digit2 <= 0 && digit2 > 30) return -4;
		if (*s == 0 || *s == ',' || *s == ' ') {
			if (digit1 > digit2)
				for (i = digit2; i <= digit1 ; i++)
					bmask |= (1 << i);
			else 
				for (i = digit1; i <= digit2 ; i++)
					bmask |= (1 << i);
			digit1 = digit2 = 0;
			if (*s) s++;
			continue;
		}
		return -6;
	}
	if (activep) *activep = active;
	if (bmaskp) *bmaskp = bmask;
	return 0;
}

static int FVteln2capi20(char *teln, __u8 AdditionalInfo[1+2+2+31])
{
	unsigned long bmask;
	int active;
	int rc, i;
   
	rc = decodeFVteln(teln, &bmask, &active);
	if (rc) return rc;
	/* Length */
	AdditionalInfo[0] = 2+2+31;
        /* Channel: 3 => use channel allocation */
        AdditionalInfo[1] = 3; AdditionalInfo[2] = 0;
	/* Operation: 0 => DTE mode, 1 => DCE mode */
        if (active) {
   		AdditionalInfo[3] = 0; AdditionalInfo[4] = 0;
   	} else {
   		AdditionalInfo[3] = 1; AdditionalInfo[4] = 0;
	}
	/* Channel mask array */
	AdditionalInfo[5] = 0; /* no D-Channel */
	for (i=1; i <= 30; i++)
		AdditionalInfo[5+i] = (bmask & (1 << i)) ? 0xff : 0;
	return 0;
}

static int capidrv_command(isdn_ctrl * c, capidrv_contr * card)
{
	isdn_ctrl cmd;
	struct capidrv_bchan *bchan;
	struct capidrv_plci *plcip;
	__u8 AdditionalInfo[1+2+2+31];
        int rc, isleasedline = 0;

	if (c->command == ISDN_CMD_IOCTL)
		return capidrv_ioctl(c, card);

	switch (c->command) {
	case ISDN_CMD_DIAL:{
			__u8 calling[ISDN_MSNLEN + 3];
			__u8 called[ISDN_MSNLEN + 2];

			if (debugmode)
				printk(KERN_DEBUG "capidrv-%d: ISDN_CMD_DIAL(ch=%ld,\"%s,%d,%d,%s\")\n",
					card->contrnr,
					c->arg,
				        c->parm.setup.phone,
				        c->parm.setup.si1,
				        c->parm.setup.si2,
				        c->parm.setup.eazmsn);

			bchan = &card->bchans[c->arg % card->nbchan];

			if (bchan->plcip) {
				printk(KERN_ERR "capidrv-%d: dail ch=%ld,\"%s,%d,%d,%s\" in use (plci=0x%x)\n",
					card->contrnr,
			        	c->arg, 
				        c->parm.setup.phone,
				        c->parm.setup.si1,
				        c->parm.setup.si2,
				        c->parm.setup.eazmsn,
				        bchan->plcip->plci);
				return 0;
			}
			bchan->si1 = c->parm.setup.si1;
			bchan->si2 = c->parm.setup.si2;

			strncpy(bchan->num, c->parm.setup.phone, sizeof(bchan->num));
			strncpy(bchan->mynum, c->parm.setup.eazmsn, sizeof(bchan->mynum));
                        rc = FVteln2capi20(bchan->num, AdditionalInfo);
			isleasedline = (rc == 0);
			if (rc < 0)
				printk(KERN_ERR "capidrv-%d: WARNING: illegal leased linedefinition \"%s\"\n", card->contrnr, bchan->num);

			if (isleasedline) {
				calling[0] = 0;
				called[0] = 0;
			        if (debugmode)
					printk(KERN_DEBUG "capidrv-%d: connecting leased line\n", card->contrnr);
			} else {
		        	calling[0] = strlen(bchan->mynum) + 2;
		        	calling[1] = 0;
		     		calling[2] = 0x80;
			   	strncpy(calling + 3, bchan->mynum, ISDN_MSNLEN);
				called[0] = strlen(bchan->num) + 1;
				called[1] = 0x80;
				strncpy(called + 2, bchan->num, ISDN_MSNLEN);
			}

			capi_fill_CONNECT_REQ(&cmdcmsg,
					      global.appid,
					      card->msgid++,
					      card->contrnr,	/* adr */
					  si2cip(bchan->si1, bchan->si2),	/* cipvalue */
					      called,	/* CalledPartyNumber */
					      calling,	/* CallingPartyNumber */
					      0,	/* CalledPartySubaddress */
					      0,	/* CallingPartySubaddress */
					    b1prot(bchan->l2, bchan->l3),	/* B1protocol */
					    b2prot(bchan->l2, bchan->l3),	/* B2protocol */
					    b3prot(bchan->l2, bchan->l3),	/* B3protocol */
					    b1config(bchan->l2, bchan->l3),	/* B1configuration */
					      0,	/* B2configuration */
					      0,	/* B3configuration */
					      0,	/* BC */
					      0,	/* LLC */
					      0,	/* HLC */
					      /* BChannelinformation */
					      isleasedline ? AdditionalInfo : 0,
					      0,	/* Keypadfacility */
					      0,	/* Useruserdata */
					      0		/* Facilitydataarray */
			    );
			if ((plcip = new_plci(card, (c->arg % card->nbchan))) == 0) {
				cmd.command = ISDN_STAT_DHUP;
				cmd.driver = card->myid;
				cmd.arg = (c->arg % card->nbchan);
				card->interface.statcallb(&cmd);
				return -1;
			}
			plcip->msgid = cmdcmsg.Messagenumber;
			plcip->leasedline = isleasedline;
			plci_change_state(card, plcip, EV_PLCI_CONNECT_REQ);
			send_message(card, &cmdcmsg);
			return 0;
		}

	case ISDN_CMD_ACCEPTD:

		bchan = &card->bchans[c->arg % card->nbchan];
		if (debugmode)
			printk(KERN_DEBUG "capidrv-%d: ISDN_CMD_ACCEPTD(ch=%ld) l2=%d l3=%d\n",
			       card->contrnr,
			       c->arg, bchan->l2, bchan->l3);

		capi_fill_CONNECT_RESP(&cmdcmsg,
				       global.appid,
				       card->msgid++,
				       bchan->plcip->plci,	/* adr */
				       0,	/* Reject */
				       b1prot(bchan->l2, bchan->l3),	/* B1protocol */
				       b2prot(bchan->l2, bchan->l3),	/* B2protocol */
				       b3prot(bchan->l2, bchan->l3),	/* B3protocol */
				       b1config(bchan->l2, bchan->l3),	/* B1configuration */
				       0,	/* B2configuration */
				       0,	/* B3configuration */
				       0,	/* ConnectedNumber */
				       0,	/* ConnectedSubaddress */
				       0,	/* LLC */
				       0,	/* BChannelinformation */
				       0,	/* Keypadfacility */
				       0,	/* Useruserdata */
				       0	/* Facilitydataarray */
		);
		capi_cmsg2message(&cmdcmsg, cmdcmsg.buf);
		plci_change_state(card, bchan->plcip, EV_PLCI_CONNECT_RESP);
		send_message(card, &cmdcmsg);
		return 0;

	case ISDN_CMD_ACCEPTB:
		if (debugmode)
			printk(KERN_DEBUG "capidrv-%d: ISDN_CMD_ACCEPTB(ch=%ld)\n",
			       card->contrnr,
			       c->arg);
		return -ENOSYS;

	case ISDN_CMD_HANGUP:
		if (debugmode)
			printk(KERN_DEBUG "capidrv-%d: ISDN_CMD_HANGUP(ch=%ld)\n",
			       card->contrnr,
			       c->arg);
		bchan = &card->bchans[c->arg % card->nbchan];

		if (bchan->disconnecting) {
			if (debugmode)
				printk(KERN_DEBUG "capidrv-%d: chan %ld already disconnecting ...\n",
				       card->contrnr,
				       c->arg);
			return 0;
		}
		if (bchan->nccip) {
			bchan->disconnecting = 1;
			capi_fill_DISCONNECT_B3_REQ(&cmdcmsg,
						    global.appid,
						    card->msgid++,
						    bchan->nccip->ncci,
						    0	/* NCPI */
			);
			ncci_change_state(card, bchan->nccip, EV_NCCI_DISCONNECT_B3_REQ);
			send_message(card, &cmdcmsg);
			return 0;
		} else if (bchan->plcip) {
			if (bchan->plcip->state == ST_PLCI_INCOMING) {
				/*
				 * just ignore, we a called from
				 * isdn_status_callback(),
				 * which will return 0 or 2, this is handled
				 * by the CONNECT_IND handler
				 */
				bchan->disconnecting = 1;
				return 0;
			} else if (bchan->plcip->plci) {
				bchan->disconnecting = 1;
				capi_fill_DISCONNECT_REQ(&cmdcmsg,
							 global.appid,
							 card->msgid++,
						      bchan->plcip->plci,
							 0,	/* BChannelinformation */
							 0,	/* Keypadfacility */
							 0,	/* Useruserdata */
							 0	/* Facilitydataarray */
				);
				plci_change_state(card, bchan->plcip, EV_PLCI_DISCONNECT_REQ);
				send_message(card, &cmdcmsg);
				return 0;
			} else {
				printk(KERN_ERR "capidrv-%d: chan %ld disconnect request while waiting for CONNECT_CONF\n",
				       card->contrnr,
				       c->arg);
				return -EINVAL;
			}
		}
		printk(KERN_ERR "capidrv-%d: chan %ld disconnect request on free channel\n",
				       card->contrnr,
				       c->arg);
		return -EINVAL;
/* ready */

	case ISDN_CMD_SETL2:
		if (debugmode)
			printk(KERN_DEBUG "capidrv-%d: set L2 on chan %ld to %ld\n",
			       card->contrnr,
			       (c->arg & 0xff), (c->arg >> 8));
		bchan = &card->bchans[(c->arg & 0xff) % card->nbchan];
		bchan->l2 = (c->arg >> 8);
		return 0;

	case ISDN_CMD_SETL3:
		if (debugmode)
			printk(KERN_DEBUG "capidrv-%d: set L3 on chan %ld to %ld\n",
			       card->contrnr,
			       (c->arg & 0xff), (c->arg >> 8));
		bchan = &card->bchans[(c->arg & 0xff) % card->nbchan];
		bchan->l3 = (c->arg >> 8);
		return 0;

	case ISDN_CMD_SETEAZ:
		if (debugmode)
			printk(KERN_DEBUG "capidrv-%d: set EAZ \"%s\" on chan %ld\n",
			       card->contrnr,
			       c->parm.num, c->arg);
		bchan = &card->bchans[c->arg % card->nbchan];
		strncpy(bchan->msn, c->parm.num, ISDN_MSNLEN);
		return 0;

	case ISDN_CMD_CLREAZ:
		if (debugmode)
			printk(KERN_DEBUG "capidrv-%d: clearing EAZ on chan %ld\n",
					card->contrnr, c->arg);
		bchan = &card->bchans[c->arg % card->nbchan];
		bchan->msn[0] = 0;
		return 0;

	case ISDN_CMD_LOCK:
		if (debugmode > 1)
			printk(KERN_DEBUG "capidrv-%d: ISDN_CMD_LOCK (%ld)\n", card->contrnr, c->arg);
		MOD_INC_USE_COUNT;
		break;

	case ISDN_CMD_UNLOCK:
		if (debugmode > 1)
			printk(KERN_DEBUG "capidrv-%d: ISDN_CMD_UNLOCK (%ld)\n",
					card->contrnr, c->arg);
		MOD_DEC_USE_COUNT;
		break;

/* never called */
	case ISDN_CMD_GETL2:
		if (debugmode)
			printk(KERN_DEBUG "capidrv-%d: ISDN_CMD_GETL2\n",
					card->contrnr);
		return -ENODEV;
	case ISDN_CMD_GETL3:
		if (debugmode)
			printk(KERN_DEBUG "capidrv-%d: ISDN_CMD_GETL3\n",
					card->contrnr);
		return -ENODEV;
	case ISDN_CMD_GETEAZ:
		if (debugmode)
			printk(KERN_DEBUG "capidrv-%d: ISDN_CMD_GETEAZ\n",
					card->contrnr);
		return -ENODEV;
	case ISDN_CMD_SETSIL:
		if (debugmode)
			printk(KERN_DEBUG "capidrv-%d: ISDN_CMD_SETSIL\n",
					card->contrnr);
		return -ENODEV;
	case ISDN_CMD_GETSIL:
		if (debugmode)
			printk(KERN_DEBUG "capidrv-%d: ISDN_CMD_GETSIL\n",
					card->contrnr);
		return -ENODEV;
	default:
		printk(KERN_ERR "capidrv-%d: ISDN_CMD_%d, Huh?\n",
					card->contrnr, c->command);
		return -EINVAL;
	}
	return 0;
}

static int if_command(isdn_ctrl * c)
{
	capidrv_contr *card = findcontrbydriverid(c->driver);

	if (card)
		return capidrv_command(c, card);

	printk(KERN_ERR
	     "capidrv: if_command %d called with invalid driverId %d!\n",
						c->command, c->driver);
	return -ENODEV;
}

static _cmsg sendcmsg;

static int if_sendbuf(int id, int channel, int doack, struct sk_buff *skb)
{
	capidrv_contr *card = findcontrbydriverid(id);
	capidrv_bchan *bchan;
	capidrv_ncci *nccip;
	int len = skb->len;
	size_t msglen;
	__u16 errcode;
	__u16 datahandle;

	if (!card) {
		printk(KERN_ERR "capidrv-%d: if_sendbuf called with invalid driverId %d!\n",
		       card->contrnr, id);
		return 0;
	}
	if (debugmode > 1)
		printk(KERN_DEBUG "capidrv-%d: sendbuf len=%d skb=%p doack=%d\n",
					card->contrnr, len, skb, doack);
	bchan = &card->bchans[channel % card->nbchan];
	nccip = bchan->nccip;
	if (!nccip || nccip->state != ST_NCCI_ACTIVE) {
		printk(KERN_ERR "capidrv-%d: if_sendbuf: %s:%d: chan not up!\n",
		       card->contrnr, card->name, channel);
		return 0;
	}
	datahandle = nccip->datahandle;
	capi_fill_DATA_B3_REQ(&sendcmsg, global.appid, card->msgid++,
			      nccip->ncci,	/* adr */
			      (__u32) skb->data,	/* Data */
			      skb->len,		/* DataLength */
			      datahandle,	/* DataHandle */
			      0	/* Flags */
	    );

	if (capidrv_add_ack(nccip, datahandle, doack ? skb->len : -1) < 0)
	   return 0;

	capi_cmsg2message(&sendcmsg, sendcmsg.buf);
	msglen = CAPIMSG_LEN(sendcmsg.buf);
	if (skb_headroom(skb) < msglen) {
		struct sk_buff *nskb = skb_realloc_headroom(skb, msglen);
		if (!nskb) {
			printk(KERN_ERR "capidrv-%d: if_sendbuf: no memory\n",
				card->contrnr);
		        (void)capidrv_del_ack(nccip, datahandle);
			return 0;
		}
		printk(KERN_DEBUG "capidrv-%d: only %d bytes headroom, need %d\n",
		       card->contrnr, skb_headroom(skb), msglen);
		memcpy(skb_push(nskb, msglen), sendcmsg.buf, msglen);
		errcode = (*capifuncs->capi_put_message) (global.appid, nskb);
		if (errcode == CAPI_NOERROR) {
			dev_kfree_skb(skb);
			nccip->datahandle++;
			global.nsentdatapkt++;
			return len;
		}
	        (void)capidrv_del_ack(nccip, datahandle);
	        dev_kfree_skb(nskb);
		return errcode == CAPI_SENDQUEUEFULL ? 0 : -1;
	} else {
		memcpy(skb_push(skb, msglen), sendcmsg.buf, msglen);
		errcode = (*capifuncs->capi_put_message) (global.appid, skb);
		if (errcode == CAPI_NOERROR) {
			nccip->datahandle++;
			global.nsentdatapkt++;
			return len;
		}
		skb_pull(skb, msglen);
	        (void)capidrv_del_ack(nccip, datahandle);
		return errcode == CAPI_SENDQUEUEFULL ? 0 : -1;
	}
}

static int if_readstat(__u8 *buf, int len, int user, int id, int channel)
{
	capidrv_contr *card = findcontrbydriverid(id);
	int count;
	__u8 *p;

	if (!card) {
		printk(KERN_ERR "capidrv-%d: if_readstat called with invalid driverId %d!\n",
		       card->contrnr, id);
		return -ENODEV;
	}

	for (p=buf, count=0; count < len; p++, count++) {
	        if (user)
	                put_user(*card->q931_read++, p);
	        else
	                *p = *card->q931_read++;
	        if (card->q931_read > card->q931_end)
	                card->q931_read = card->q931_buf;
	}
	return count;

}

static void enable_dchannel_trace(capidrv_contr *card)
{
        __u8 manufacturer[CAPI_MANUFACTURER_LEN];
        capi_version version;
	__u16 contr = card->contrnr;
	__u16 errcode;
	__u16 avmversion[3];

        errcode = (*capifuncs->capi_get_manufacturer)(contr, manufacturer);
        if (errcode != CAPI_NOERROR) {
	   printk(KERN_ERR "%s: can't get manufacturer (0x%x)\n",
			card->name, errcode);
	   return;
	}
	if (strstr(manufacturer, "AVM") == 0) {
	   printk(KERN_ERR "%s: not from AVM, no d-channel trace possible (%s)\n",
			card->name, manufacturer);
	   return;
	}
        errcode = (*capifuncs->capi_get_version)(contr, &version);
        if (errcode != CAPI_NOERROR) {
	   printk(KERN_ERR "%s: can't get version (0x%x)\n",
			card->name, errcode);
	   return;
	}
	avmversion[0] = (version.majormanuversion >> 4) & 0x0f;
	avmversion[1] = (version.majormanuversion << 4) & 0xf0;
	avmversion[1] |= (version.minormanuversion >> 4) & 0x0f;
	avmversion[2] |= version.minormanuversion & 0x0f;

        if (avmversion[0] > 3 || (avmversion[0] == 3 && avmversion[1] > 5)) {
		printk(KERN_INFO "%s: D2 trace enabled\n", card->name);
		capi_fill_MANUFACTURER_REQ(&cmdcmsg, global.appid,
					   card->msgid++,
					   contr,
					   0x214D5641,  /* ManuID */
					   0,           /* Class */
					   1,           /* Function */
					   (_cstruct)"\004\200\014\000\000");
	} else {
		printk(KERN_INFO "%s: D3 trace enabled\n", card->name);
		capi_fill_MANUFACTURER_REQ(&cmdcmsg, global.appid,
					   card->msgid++,
					   contr,
					   0x214D5641,  /* ManuID */
					   0,           /* Class */
					   1,           /* Function */
					   (_cstruct)"\004\002\003\000\000");
	}
	send_message(card, &cmdcmsg);
}


static void send_listen(capidrv_contr *card)
{
	capi_fill_LISTEN_REQ(&cmdcmsg, global.appid,
			     card->msgid++,
			     card->contrnr, /* controller */
			     1 << 6,	/* Infomask */
			     card->cipmask,
			     card->cipmask2,
			     0, 0);
	send_message(card, &cmdcmsg);
	listen_change_state(card, EV_LISTEN_REQ);
}

static void listentimerfunc(unsigned long x)
{
	capidrv_contr *card = (capidrv_contr *)x;
	if (card->state != ST_LISTEN_NONE && card->state != ST_LISTEN_ACTIVE)
		printk(KERN_ERR "%s: controller dead ??\n", card->name);
        send_listen(card);
	mod_timer(&card->listentimer, jiffies + 60*HZ);
}


static int capidrv_addcontr(__u16 contr, struct capi_profile *profp)
{
	capidrv_contr *card;
	long flags;
	isdn_ctrl cmd;
	char id[20];
	int i;

	MOD_INC_USE_COUNT;

	sprintf(id, "capidrv-%d", contr);
	if (!(card = (capidrv_contr *) kmalloc(sizeof(capidrv_contr), GFP_ATOMIC))) {
		printk(KERN_WARNING
		 "capidrv: (%s) Could not allocate contr-struct.\n", id);
		MOD_DEC_USE_COUNT;
		return -1;
	}
	memset(card, 0, sizeof(capidrv_contr));
	init_timer(&card->listentimer);
	strcpy(card->name, id);
	card->contrnr = contr;
	card->nbchan = profp->nbchannel;
	card->bchans = (capidrv_bchan *) kmalloc(sizeof(capidrv_bchan) * card->nbchan, GFP_ATOMIC);
	if (!card->bchans) {
		printk(KERN_WARNING
		"capidrv: (%s) Could not allocate bchan-structs.\n", id);
		kfree(card);
		MOD_DEC_USE_COUNT;
		return -1;
	}
	card->interface.channels = profp->nbchannel;
	card->interface.maxbufsize = 2048;
	card->interface.command = if_command;
	card->interface.writebuf_skb = if_sendbuf;
	card->interface.writecmd = 0;
	card->interface.readstat = if_readstat;
	card->interface.features = ISDN_FEATURE_L2_HDLC |
	    			   ISDN_FEATURE_L2_TRANS |
	    			   ISDN_FEATURE_L3_TRANS |
				   ISDN_FEATURE_P_UNKNOWN |
				   ISDN_FEATURE_L2_X75I |
				   ISDN_FEATURE_L2_X75UI |
				   ISDN_FEATURE_L2_X75BUI;
	if (profp->support1 & (1<<2))
		card->interface.features |= ISDN_FEATURE_L2_V11096 |
	    				    ISDN_FEATURE_L2_V11019 |
	    				    ISDN_FEATURE_L2_V11038;
	if (profp->support1 & (1<<8))
		card->interface.features |= ISDN_FEATURE_L2_MODEM;
	card->interface.hl_hdrlen = 22; /* len of DATA_B3_REQ */
	strncpy(card->interface.id, id, sizeof(card->interface.id) - 1);


	card->q931_read = card->q931_buf;
	card->q931_write = card->q931_buf;
	card->q931_end = card->q931_buf + sizeof(card->q931_buf) - 1;

	if (!register_isdn(&card->interface)) {
		printk(KERN_ERR "capidrv: Unable to register contr %s\n", id);
		kfree(card->bchans);
		kfree(card);
		MOD_DEC_USE_COUNT;
		return -1;
	}
	card->myid = card->interface.channels;

	spin_lock_irqsave(&global_lock, flags);
	card->next = global.contr_list;
	global.contr_list = card;
	global.ncontr++;
	spin_unlock_irqrestore(&global_lock, flags);

	memset(card->bchans, 0, sizeof(capidrv_bchan) * card->nbchan);
	for (i = 0; i < card->nbchan; i++) {
		card->bchans[i].contr = card;
	}

	cmd.command = ISDN_STAT_RUN;
	cmd.driver = card->myid;
	card->interface.statcallb(&cmd);

	card->cipmask = 0x1FFF03FF;	/* any */
	card->cipmask2 = 0;

	send_listen(card);

	card->listentimer.data = (unsigned long)card;
	card->listentimer.function = listentimerfunc;
	mod_timer(&card->listentimer, jiffies + 60*HZ);

	printk(KERN_INFO "%s: now up (%d B channels)\n",
		card->name, card->nbchan);

	enable_dchannel_trace(card);

	return 0;
}

static int capidrv_delcontr(__u16 contr)
{
	capidrv_contr **pp, *card;
	unsigned long flags;
	isdn_ctrl cmd;

	spin_lock_irqsave(&global_lock, flags);
	for (card = global.contr_list; card; card = card->next) {
		if (card->contrnr == contr)
			break;
	}
	if (!card) {
		spin_unlock_irqrestore(&global_lock, flags);
		printk(KERN_ERR "capidrv: delcontr: no contr %u\n", contr);
		return -1;
	}
	spin_unlock_irqrestore(&global_lock, flags);

	del_timer(&card->listentimer);

	if (debugmode)
		printk(KERN_DEBUG "capidrv-%d: id=%d unloading\n",
					card->contrnr, card->myid);

	cmd.command = ISDN_STAT_STOP;
	cmd.driver = card->myid;
	card->interface.statcallb(&cmd);

	while (card->nbchan) {

		cmd.command = ISDN_STAT_DISCH;
		cmd.driver = card->myid;
		cmd.arg = card->nbchan-1;
	        cmd.parm.num[0] = 0;
		if (debugmode)
			printk(KERN_DEBUG "capidrv-%d: id=%d disable chan=%ld\n",
					card->contrnr, card->myid, cmd.arg);
		card->interface.statcallb(&cmd);

		if (card->bchans[card->nbchan-1].nccip)
			free_ncci(card, card->bchans[card->nbchan-1].nccip);
		if (card->bchans[card->nbchan-1].plcip)
			free_plci(card, card->bchans[card->nbchan-1].plcip);
		if (card->plci_list)
			printk(KERN_ERR "capidrv: bug in free_plci()\n");
		card->nbchan--;
	}
	kfree(card->bchans);
	card->bchans = 0;

	if (debugmode)
		printk(KERN_DEBUG "capidrv-%d: id=%d isdn unload\n",
					card->contrnr, card->myid);

	cmd.command = ISDN_STAT_UNLOAD;
	cmd.driver = card->myid;
	card->interface.statcallb(&cmd);

	if (debugmode)
		printk(KERN_DEBUG "capidrv-%d: id=%d remove contr from list\n",
					card->contrnr, card->myid);

	spin_lock_irqsave(&global_lock, flags);
	for (pp = &global.contr_list; *pp; pp = &(*pp)->next) {
		if (*pp == card) {
			*pp = (*pp)->next;
			card->next = 0;
			global.ncontr--;
			break;
		}
	}
	spin_unlock_irqrestore(&global_lock, flags);

	printk(KERN_INFO "%s: now down.\n", card->name);

	kfree(card);

	MOD_DEC_USE_COUNT;

	return 0;
}


static void lower_callback(unsigned int cmd, __u32 contr, void *data)
{

	switch (cmd) {
	case KCI_CONTRUP:
		printk(KERN_INFO "capidrv: controller %hu up\n", contr);
		(void) capidrv_addcontr(contr, (capi_profile *) data);
		break;
	case KCI_CONTRDOWN:
		printk(KERN_INFO "capidrv: controller %hu down\n", contr);
		(void) capidrv_delcontr(contr);
		break;
	}
}

/*
 * /proc/capi/capidrv:
 * nrecvctlpkt nrecvdatapkt nsendctlpkt nsenddatapkt
 */
static int proc_capidrv_read_proc(char *page, char **start, off_t off,
                                       int count, int *eof, void *data)
{
	int len = 0;

	len += sprintf(page+len, "%lu %lu %lu %lu\n",
			global.nrecvctlpkt,
			global.nrecvdatapkt,
			global.nsentctlpkt,
			global.nsentdatapkt);
	if (off+count >= len)
	   *eof = 1;
	if (len < off)
           return 0;
	*start = page + off;
	return ((count < len-off) ? count : len-off);
}

static struct procfsentries {
  char *name;
  mode_t mode;
  int (*read_proc)(char *page, char **start, off_t off,
                                       int count, int *eof, void *data);
  struct proc_dir_entry *procent;
} procfsentries[] = {
   /* { "capi",		  S_IFDIR, 0 }, */
   { "capi/capidrv", 	  0	 , proc_capidrv_read_proc },
};

static void __init proc_init(void)
{
    int nelem = sizeof(procfsentries)/sizeof(procfsentries[0]);
    int i;

    for (i=0; i < nelem; i++) {
        struct procfsentries *p = procfsentries + i;
	p->procent = create_proc_entry(p->name, p->mode, 0);
	if (p->procent) p->procent->read_proc = p->read_proc;
    }
}

static void __exit proc_exit(void)
{
    int nelem = sizeof(procfsentries)/sizeof(procfsentries[0]);
    int i;

    for (i=nelem-1; i >= 0; i--) {
        struct procfsentries *p = procfsentries + i;
	if (p->procent) {
	   remove_proc_entry(p->name, 0);
	   p->procent = 0;
	}
    }
}

static struct capi_interface_user cuser = {
	name: "capidrv",
	callback: lower_callback
};

static int __init capidrv_init(void)
{
	struct capi_register_params rparam;
	capi_profile profile;
	char rev[10];
	char *p;
	__u32 ncontr, contr;
	__u16 errcode;

	MOD_INC_USE_COUNT;

	capifuncs = attach_capi_interface(&cuser);

	if (!capifuncs) {
		MOD_DEC_USE_COUNT;
		return -EIO;
	}

	if ((p = strchr(revision, ':'))) {
		strcpy(rev, p + 1);
		p = strchr(rev, '$');
		*p = 0;
	} else
		strcpy(rev, " ??? ");

	rparam.level3cnt = -2;  /* number of bchannels twice */
	rparam.datablkcnt = 16;
	rparam.datablklen = 2048;
	errcode = (*capifuncs->capi_register) (&rparam, &global.appid);
	if (errcode) {
		detach_capi_interface(&cuser);
		MOD_DEC_USE_COUNT;
		return -EIO;
	}

	errcode = (*capifuncs->capi_get_profile) (0, &profile);
	if (errcode != CAPI_NOERROR) {
		(void) (*capifuncs->capi_release) (global.appid);
		detach_capi_interface(&cuser);
		MOD_DEC_USE_COUNT;
		return -EIO;
	}

	(void) (*capifuncs->capi_set_signal) (global.appid, capidrv_signal, 0);

	ncontr = profile.ncontroller;
	for (contr = 1; contr <= ncontr; contr++) {
		errcode = (*capifuncs->capi_get_profile) (contr, &profile);
		if (errcode != CAPI_NOERROR)
			continue;
		(void) capidrv_addcontr(contr, &profile);
	}
	proc_init();

	printk(KERN_NOTICE "capidrv: Rev%s: loaded\n", rev);
	MOD_DEC_USE_COUNT;

	return 0;
}

static void __exit capidrv_exit(void)
{
	char rev[10];
	char *p;

	if ((p = strchr(revision, ':')) != 0) {
		strcpy(rev, p + 1);
		p = strchr(rev, '$');
		*p = 0;
	} else {
		strcpy(rev, " ??? ");
	}

	(void) (*capifuncs->capi_release) (global.appid);

	detach_capi_interface(&cuser);

	proc_exit();

	printk(KERN_NOTICE "capidrv: Rev%s: unloaded\n", rev);
}

module_init(capidrv_init);
module_exit(capidrv_exit);
