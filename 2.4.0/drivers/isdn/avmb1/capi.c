/*
 * $Id: capi.c,v 1.45 2000/12/02 19:47:29 kai Exp $
 *
 * CAPI 2.0 Interface for Linux
 *
 * Copyright 1996 by Carsten Paeth (calle@calle.in-berlin.de)
 *
 * $Log: capi.c,v $
 * Revision 1.45  2000/12/02 19:47:29  kai
 * Change the Makefiles to new style.
 * There may be problems there that I missed, so this shouldn't go into
 * an offical kernel any time soon.
 * However, if I didn't commit it, we wouldn't find the bugs...
 *
 * Revision 1.44  2000/11/25 17:00:59  kai
 * compatibility cleanup - final part for the time being
 *
 * Revision 1.43  2000/11/23 20:45:14  kai
 * fixed module_init/exit stuff
 * Note: compiled-in kernel doesn't work pre 2.2.18 anymore.
 *
 * Revision 1.42  2000/11/19 17:03:55  kai
 * compatibility cleanup - part 5
 *
 * Revision 1.41  2000/11/01 14:05:02  calle
 * - use module_init/module_exit from linux/init.h.
 * - all static struct variables are initialized with "membername:" now.
 * - avm_cs.c, let it work with newer pcmcia-cs.
 *
 * Revision 1.40  2000/10/24 15:15:04  calle
 * Workaround: pppd calls restoretty before reseting the ldisc and
 *   ldisc "ppp_sync" didn't support this. So we call n_tty_ioctl
 *   in the driver ioctl function. (remember: driver ioctl function is
 *   only called if ldisc ioctl function did not handle the call)
 *
 * Revision 1.39  2000/07/24 13:42:50  calle
 * - lock_kernel/unlock_kernel for _release functions. (from 2.4)
 *
 * Revision 1.38  2000/07/24 08:49:09  calle
 * - Bugfix: capiminor_del_all_ack completely wrong :-(
 *
 * Revision 1.37  2000/07/20 10:22:27  calle
 * - Made procfs function cleaner and removed variable "begin".
 *
 * Revision 1.36  2000/06/29 13:59:35  calle
 * - call to devfs_register was wrong
 *
 * Revision 1.35  2000/06/19 15:11:24  keil
 * avoid use of freed structs
 * changes from 2.4.0-ac21
 *
 * Revision 1.34  2000/06/18 16:09:54  keil
 * more changes for 2.4
 *
 * Revision 1.33  2000/05/18 16:35:43  calle
 * Uaaahh. Bad memory leak fixed.
 *
 * Revision 1.32  2000/04/21 12:38:42  calle
 * Bugfix: error in proc_ functions, begin-off => off-begin
 *
 * Revision 1.31  2000/04/03 13:29:24  calle
 * make Tim Waugh happy (module unload races in 2.3.99-pre3).
 * no real problem there, but now it is much cleaner ...
 *
 * Revision 1.30  2000/03/19 12:31:36  calle
 * PPP over CAPI raw driver disabled for now, ppp_generic has been changed.
 *
 * Revision 1.29  2000/03/13 17:48:13  calle
 * removed unused variable.
 *
 * Revision 1.28  2000/03/08 17:06:33  calle
 * - changes for devfs and 2.3.49
 * - capifs now configurable (no need with devfs)
 * - New Middleware ioctl CAPI_NCCI_GETUNIT
 * - Middleware again tested with 2.2.14 and 2.3.49 (with and without devfs)
 *
 * Revision 1.27  2000/03/06 18:00:23  calle
 * - Middleware extention now working with 2.3.49 (capifs).
 * - Fixed typos in debug section of capi.c
 * - Bugfix: Makefile corrected for b1pcmcia.c
 *
 * Revision 1.26  2000/03/03 16:48:38  calle
 * - Added CAPI2.0 Middleware support (CONFIG_ISDN_CAPI)
 *   It is now possible to create a connection with a CAPI2.0 applikation
 *   and than to handle the data connection from /dev/capi/ (capifs) and also
 *   using async or sync PPP on this connection.
 *   The two major device number 190 and 191 are not confirmed yet,
 *   but I want to save the code in cvs, before I go on.
 *
 * Revision 1.25  2000/03/03 16:37:11  kai
 * incorporated some cosmetic changes from the official kernel tree back
 * into CVS
 *
 * Revision 1.24  2000/03/03 15:50:42  calle
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
 * Revision 1.23  2000/02/26 01:00:53  keil
 * changes from 2.3.47
 *
 * Revision 1.22  1999/11/13 21:27:16  keil
 * remove KERNELVERSION
 *
 * Revision 1.21  1999/09/10 17:24:18  calle
 * Changes for proposed standard for CAPI2.0:
 * - AK148 "Linux Exention"
 *
 * Revision 1.20  1999/09/07 09:02:53  calle
 * SETDATA removed. Now inside the kernel the datapart of DATA_B3_REQ and
 * DATA_B3_IND is always directly after the CAPI message. The "Data" member
 * ist never used inside the kernel.
 *
 * Revision 1.19  1999/07/09 15:05:42  keil
 * compat.h is now isdn_compat.h
 *
 * Revision 1.18  1999/07/06 07:42:01  calle
 * - changes in /proc interface
 * - check and changed calls to [dev_]kfree_skb and [dev_]alloc_skb.
 *
 * Revision 1.17  1999/07/01 15:26:30  calle
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
 * Revision 1.16  1999/07/01 08:22:57  keil
 * compatibility macros now in <linux/isdn_compat.h>
 *
 * Revision 1.15  1999/06/21 15:24:11  calle
 * extend information in /proc.
 *
 * Revision 1.14  1999/06/10 16:51:03  calle
 * Bugfix: open/release of control device was not handled correct.
 *
 * Revision 1.13  1998/08/28 04:32:25  calle
 * Added patch send by Michael.Mueller4@post.rwth-aachen.de, to get AVM B1
 * driver running with 2.1.118.
 *
 * Revision 1.12  1998/05/26 22:39:34  he
 * sync'ed with 2.1.102 where appropriate (CAPABILITY changes)
 * concap typo
 * cleared dev.tbusy in isdn_net BCONN status callback
 *
 * Revision 1.11  1998/03/09 17:46:37  he
 * merged in 2.1.89 changes
 *
 * Revision 1.10  1998/02/13 07:09:13  calle
 * change for 2.1.86 (removing FREE_READ/FREE_WRITE from [dev]_kfree_skb()
 *
 * Revision 1.9  1998/01/31 11:14:44  calle
 * merged changes to 2.0 tree, prepare 2.1.82 to work.
 *
 * Revision 1.8  1997/11/04 06:12:08  calle
 * capi.c: new read/write in file_ops since 2.1.60
 * capidrv.c: prepared isdnlog interface for d2-trace in newer firmware.
 * capiutil.c: needs config.h (CONFIG_ISDN_DRV_AVMB1_VERBOSE_REASON)
 * compat.h: added #define LinuxVersionCode
 *
 * Revision 1.7  1997/10/11 10:29:34  calle
 * llseek() parameters changed in 2.1.56.
 *
 * Revision 1.6  1997/10/01 09:21:15  fritz
 * Removed old compatibility stuff for 2.0.X kernels.
 * From now on, this code is for 2.1.X ONLY!
 * Old stuff is still in the separate branch.
 *
 * Revision 1.5  1997/08/21 23:11:55  fritz
 * Added changes for kernels >= 2.1.45
 *
 * Revision 1.4  1997/05/27 15:17:50  fritz
 * Added changes for recent 2.1.x kernels:
 *   changed return type of isdn_close
 *   queue_task_* -> queue_task
 *   clear/set_bit -> test_and_... where apropriate.
 *   changed type of hard_header_cache parameter.
 *
 * Revision 1.3  1997/05/18 09:24:14  calle
 * added verbose disconnect reason reporting to avmb1.
 * some fixes in capi20 interface.
 * changed info messages for B1-PCI
 *
 * Revision 1.2  1997/03/05 21:17:59  fritz
 * Added capi_poll for compiling under 2.1.27
 *
 * Revision 1.1  1997/03/04 21:50:29  calle
 * Frirst version in isdn4linux
 *
 * Revision 2.2  1997/02/12 09:31:39  calle
 * new version
 *
 * Revision 1.1  1997/01/31 10:32:20  calle
 * Initial revision
 *
 */

#include <linux/config.h>
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
#include <linux/smp_lock.h>
#include <linux/timer.h>
#include <linux/wait.h>
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
#include <linux/tty.h>
#ifdef CONFIG_PPP
#include <linux/netdevice.h>
#include <linux/ppp_defs.h>
#include <linux/if_ppp.h>
#undef CAPI_PPP_ON_RAW_DEVICE
#endif /* CONFIG_PPP */
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */
#include <linux/skbuff.h>
#include <linux/proc_fs.h>
#include <linux/poll.h>
#include <linux/capi.h>
#include <linux/kernelcapi.h>
#include <linux/init.h>
#include <linux/devfs_fs_kernel.h>
#include "capiutil.h"
#include "capicmd.h"
#if defined(CONFIG_ISDN_CAPI_CAPIFS) || defined(CONFIG_ISDN_CAPI_CAPIFS_MODULE)
#include "capifs.h"
#endif
#include <linux/slab.h>

static char *revision = "$Revision: 1.45 $";

MODULE_AUTHOR("Carsten Paeth (calle@calle.in-berlin.de)");

#undef _DEBUG_REFCOUNT		/* alloc/free and open/close debug */
#undef _DEBUG_TTYFUNCS		/* call to tty_driver */
#undef _DEBUG_DATAFLOW		/* data flow */

/* -------- driver information -------------------------------------- */

int capi_major = 68;		/* allocated */
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
int capi_rawmajor = 190;
int capi_ttymajor = 191;
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */

MODULE_PARM(capi_major, "i");
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
MODULE_PARM(capi_rawmajor, "i");
MODULE_PARM(capi_ttymajor, "i");
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */

/* -------- defines ------------------------------------------------- */

#define CAPINC_MAX_RECVQUEUE	10
#define CAPINC_MAX_SENDQUEUE	10
#define CAPI_MAX_BLKSIZE	2048

/* -------- data structures ----------------------------------------- */

struct capidev;
struct capincci;
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
struct capiminor;

struct capiminor {
	struct capiminor *next;
	struct capincci  *nccip;
	unsigned int      minor;

	__u16		 applid;
	__u32		 ncci;
	__u16		 datahandle;
	__u16		 msgid;

	struct file      *file;
	struct tty_struct *tty;
	int                ttyinstop;
	int                ttyoutstop;
	struct sk_buff    *ttyskb;
	atomic_t           ttyopencount;

	struct sk_buff_head inqueue;
	int                 inbytes;
	struct sk_buff_head outqueue;
	int                 outbytes;

	/* for raw device */
	struct sk_buff_head recvqueue;
	wait_queue_head_t recvwait;
	wait_queue_head_t sendwait;
	
	/* transmit path */
	struct datahandle_queue {
		    struct datahandle_queue *next;
		    __u16                    datahandle;
	} *ackqueue;
	int nack;

};
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */

struct capincci {
	struct capincci *next;
	__u32		 ncci;
	struct capidev	*cdev;
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
	struct capiminor *minorp;
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */
};

struct capidev {
	struct capidev *next;
	struct file    *file;
	__u16		applid;
	__u16		errcode;
	unsigned int    minor;
	unsigned        userflags;

	struct sk_buff_head recvqueue;
	wait_queue_head_t recvwait;

	/* Statistic */
	unsigned long	nrecvctlpkt;
	unsigned long	nrecvdatapkt;
	unsigned long	nsentctlpkt;
	unsigned long	nsentdatapkt;
	
	struct capincci *nccis;
};

/* -------- global variables ---------------------------------------- */

static struct capi_interface *capifuncs = 0;
static struct capidev *capidev_openlist = 0;
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
static struct capiminor *minors = 0;
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */

static kmem_cache_t *capidev_cachep = 0;
static kmem_cache_t *capincci_cachep = 0;
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
static kmem_cache_t *capiminor_cachep = 0;
static kmem_cache_t *capidh_cachep = 0;
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */

#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
/* -------- datahandles --------------------------------------------- */

int capincci_add_ack(struct capiminor *mp, __u16 datahandle)
{
	struct datahandle_queue *n, **pp;

	n = (struct datahandle_queue *)
	kmem_cache_alloc(capidh_cachep, GFP_ATOMIC);
	if (!n) {
	   printk(KERN_ERR "capi: alloc datahandle failed\n");
	   return -1;
	}
	n->next = 0;
	n->datahandle = datahandle;
	for (pp = &mp->ackqueue; *pp; pp = &(*pp)->next) ;
	*pp = n;
	mp->nack++;
	return 0;
}

int capiminor_del_ack(struct capiminor *mp, __u16 datahandle)
{
	struct datahandle_queue **pp, *p;

	for (pp = &mp->ackqueue; *pp; pp = &(*pp)->next) {
 		if ((*pp)->datahandle == datahandle) {
			p = *pp;
			*pp = (*pp)->next;
			kmem_cache_free(capidh_cachep, p);
			mp->nack--;
			return 0;
		}
	}
	return -1;
}

void capiminor_del_all_ack(struct capiminor *mp)
{
	struct datahandle_queue **pp, *p;

	pp = &mp->ackqueue;
	while (*pp) {
		p = *pp;
		*pp = (*pp)->next;
		kmem_cache_free(capidh_cachep, p);
		mp->nack--;
	}
}


/* -------- struct capiminor ---------------------------------------- */

struct capiminor *capiminor_alloc(__u16 applid, __u32 ncci)
{
	struct capiminor *mp, **pp;
        unsigned int minor = 0;

	MOD_INC_USE_COUNT;
	mp = (struct capiminor *)kmem_cache_alloc(capiminor_cachep, GFP_ATOMIC);
	if (!mp) {
		MOD_DEC_USE_COUNT;
		printk(KERN_ERR "capi: can't alloc capiminor\n");
		return 0;
	}
#ifdef _DEBUG_REFCOUNT
	printk(KERN_DEBUG "capiminor_alloc %d\n", GET_USE_COUNT(THIS_MODULE));
#endif
	memset(mp, 0, sizeof(struct capiminor));
	mp->applid = applid;
	mp->ncci = ncci;
	mp->msgid = 0;
	atomic_set(&mp->ttyopencount,0);

	skb_queue_head_init(&mp->inqueue);
	skb_queue_head_init(&mp->outqueue);

	skb_queue_head_init(&mp->recvqueue);
	init_waitqueue_head(&mp->recvwait);
	init_waitqueue_head(&mp->sendwait);

	for (pp = &minors; *pp; pp = &(*pp)->next) {
		if ((*pp)->minor < minor)
			continue;
		if ((*pp)->minor > minor)
			break;
		minor++;
	}
	mp->minor = minor;
	mp->next = *pp;
	*pp = mp;
	return mp;
}

void capiminor_free(struct capiminor *mp)
{
	struct capiminor **pp;
	struct sk_buff *skb;

	pp = &minors;
	while (*pp) {
		if (*pp == mp) {
			*pp = (*pp)->next;
			if (mp->ttyskb) kfree_skb(mp->ttyskb);
			mp->ttyskb = 0;
			while ((skb = skb_dequeue(&mp->recvqueue)) != 0)
				kfree_skb(skb);
			while ((skb = skb_dequeue(&mp->inqueue)) != 0)
				kfree_skb(skb);
			while ((skb = skb_dequeue(&mp->outqueue)) != 0)
				kfree_skb(skb);
			capiminor_del_all_ack(mp);
			kmem_cache_free(capiminor_cachep, mp);
			MOD_DEC_USE_COUNT;
#ifdef _DEBUG_REFCOUNT
			printk(KERN_DEBUG "capiminor_free %d\n", GET_USE_COUNT(THIS_MODULE));
#endif
			return;
		} else {
			pp = &(*pp)->next;
		}
	}
}

struct capiminor *capiminor_find(unsigned int minor)
{
	struct capiminor *p;
	for (p = minors; p && p->minor != minor; p = p->next)
		;
	return p;
}
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */

/* -------- struct capincci ----------------------------------------- */

static struct capincci *capincci_alloc(struct capidev *cdev, __u32 ncci)
{
	struct capincci *np, **pp;
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
	struct capiminor *mp = 0;
	kdev_t kdev;
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */

	np = (struct capincci *)kmem_cache_alloc(capincci_cachep, GFP_ATOMIC);
	if (!np)
		return 0;
	memset(np, 0, sizeof(struct capincci));
	np->ncci = ncci;
	np->cdev = cdev;
	for (pp=&cdev->nccis; *pp; pp = &(*pp)->next)
		;
	*pp = np;
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
	mp = 0;
	if (cdev->userflags & CAPIFLAG_HIGHJACKING)
		mp = np->minorp = capiminor_alloc(cdev->applid, ncci);
	if (mp) {
		mp->nccip = np;
#ifdef _DEBUG_REFCOUNT
		printk(KERN_DEBUG "set mp->nccip\n");
#endif
#if defined(CONFIG_ISDN_CAPI_CAPIFS) || defined(CONFIG_ISDN_CAPI_CAPIFS_MODULE)
		kdev = MKDEV(capi_rawmajor, mp->minor);
		capifs_new_ncci('r', mp->minor, kdev);
		kdev = MKDEV(capi_ttymajor, mp->minor);
		capifs_new_ncci(0, mp->minor, kdev);
#endif
	}
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */
        return np;
}

static void capincci_free(struct capidev *cdev, __u32 ncci)
{
	struct capincci *np, **pp;
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
	struct capiminor *mp;
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */

	pp=&cdev->nccis;
	while (*pp) {
		np = *pp;
		if (ncci == 0xffffffff || np->ncci == ncci) {
			*pp = (*pp)->next;
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
			if ((mp = np->minorp) != 0) {
#if defined(CONFIG_ISDN_CAPI_CAPIFS) || defined(CONFIG_ISDN_CAPI_CAPIFS_MODULE)
				capifs_free_ncci('r', mp->minor);
				capifs_free_ncci(0, mp->minor);
#endif
				if (mp->tty) {
					mp->nccip = 0;
#ifdef _DEBUG_REFCOUNT
					printk(KERN_DEBUG "reset mp->nccip\n");
#endif
					tty_hangup(mp->tty);
				} else if (mp->file) {
					mp->nccip = 0;
#ifdef _DEBUG_REFCOUNT
					printk(KERN_DEBUG "reset mp->nccip\n");
#endif
					wake_up_interruptible(&mp->recvwait);
					wake_up_interruptible(&mp->sendwait);
				} else {
					capiminor_free(mp);
				}
			}
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */
			kmem_cache_free(capincci_cachep, np);
			if (*pp == 0) return;
		} else {
			pp = &(*pp)->next;
		}
	}
}

struct capincci *capincci_find(struct capidev *cdev, __u32 ncci)
{
	struct capincci *p;

	for (p=cdev->nccis; p ; p = p->next) {
		if (p->ncci == ncci)
			break;
	}
	return p;
}

/* -------- struct capidev ------------------------------------------ */

static struct capidev *capidev_alloc(struct file *file)
{
	struct capidev *cdev;
	struct capidev **pp;

	cdev = (struct capidev *)kmem_cache_alloc(capidev_cachep, GFP_KERNEL);
	if (!cdev)
		return 0;
	memset(cdev, 0, sizeof(struct capidev));
	cdev->file = file;
	cdev->minor = MINOR(file->f_dentry->d_inode->i_rdev);

	skb_queue_head_init(&cdev->recvqueue);
	init_waitqueue_head(&cdev->recvwait);
	pp=&capidev_openlist;
	while (*pp) pp = &(*pp)->next;
	*pp = cdev;
        return cdev;
}

static void capidev_free(struct capidev *cdev)
{
	struct capidev **pp;
	struct sk_buff *skb;

	if (cdev->applid)
		(*capifuncs->capi_release) (cdev->applid);
	cdev->applid = 0;

	while ((skb = skb_dequeue(&cdev->recvqueue)) != 0) {
		kfree_skb(skb);
	}
	
	pp=&capidev_openlist;
	while (*pp && *pp != cdev) pp = &(*pp)->next;
	if (*pp)
		*pp = cdev->next;

	kmem_cache_free(capidev_cachep, cdev);
}

static struct capidev *capidev_find(__u16 applid)
{
	struct capidev *p;
	for (p=capidev_openlist; p; p = p->next) {
		if (p->applid == applid)
			break;
	}
	return p;
}

#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
/* -------- handle data queue --------------------------------------- */

struct sk_buff *
gen_data_b3_resp_for(struct capiminor *mp, struct sk_buff *skb)
{
	struct sk_buff *nskb;
	nskb = alloc_skb(CAPI_DATA_B3_RESP_LEN, GFP_ATOMIC);
	if (nskb) {
		__u16 datahandle = CAPIMSG_U16(skb->data,CAPIMSG_BASELEN+4+4+2);
		unsigned char *s = skb_put(nskb, CAPI_DATA_B3_RESP_LEN);
		capimsg_setu16(s, 0, CAPI_DATA_B3_RESP_LEN);
		capimsg_setu16(s, 2, mp->applid);
		capimsg_setu8 (s, 4, CAPI_DATA_B3);
		capimsg_setu8 (s, 5, CAPI_RESP);
		capimsg_setu16(s, 6, mp->msgid++);
		capimsg_setu32(s, 8, mp->ncci);
		capimsg_setu16(s, 12, datahandle);
	}
	return nskb;
}

int handle_recv_skb(struct capiminor *mp, struct sk_buff *skb)
{
	struct sk_buff *nskb;
	unsigned int datalen;
	__u16 errcode, datahandle;

	datalen = skb->len - CAPIMSG_LEN(skb->data);
	if (mp->tty) {
		if (mp->tty->ldisc.receive_buf == 0) {
			printk(KERN_ERR "capi: ldisc has no receive_buf function\n");
			return -1;
		}
		if (mp->ttyinstop) {
#if defined(_DEBUG_DATAFLOW) || defined(_DEBUG_TTYFUNCS)
			printk(KERN_DEBUG "capi: recv tty throttled\n");
#endif
			return -1;
		}
		if (mp->tty->ldisc.receive_room &&
		    mp->tty->ldisc.receive_room(mp->tty) < datalen) {
#if defined(_DEBUG_DATAFLOW) || defined(_DEBUG_TTYFUNCS)
			printk(KERN_DEBUG "capi: no room in tty\n");
#endif
			return -1;
		}
		if ((nskb = gen_data_b3_resp_for(mp, skb)) == 0) {
			printk(KERN_ERR "capi: gen_data_b3_resp failed\n");
			return -1;
		}
		datahandle = CAPIMSG_U16(skb->data,CAPIMSG_BASELEN+4);
		errcode = (*capifuncs->capi_put_message)(mp->applid, nskb);
		if (errcode != CAPI_NOERROR) {
			printk(KERN_ERR "capi: send DATA_B3_RESP failed=%x\n",
					errcode);
			kfree_skb(nskb);
			return -1;
		}
		(void)skb_pull(skb, CAPIMSG_LEN(skb->data));
#ifdef _DEBUG_DATAFLOW
		printk(KERN_DEBUG "capi: DATA_B3_RESP %u len=%d => ldisc\n",
					datahandle, skb->len);
#endif
		mp->tty->ldisc.receive_buf(mp->tty, skb->data, 0, skb->len);
		kfree_skb(skb);
		return 0;

	} else if (mp->file) {
		if (skb_queue_len(&mp->recvqueue) > CAPINC_MAX_RECVQUEUE) {
#if defined(_DEBUG_DATAFLOW) || defined(_DEBUG_TTYFUNCS)
			printk(KERN_DEBUG "capi: no room in raw queue\n");
#endif
			return -1;
		}
		if ((nskb = gen_data_b3_resp_for(mp, skb)) == 0) {
			printk(KERN_ERR "capi: gen_data_b3_resp failed\n");
			return -1;
		}
		datahandle = CAPIMSG_U16(skb->data,CAPIMSG_BASELEN+4);
		errcode = (*capifuncs->capi_put_message)(mp->applid, nskb);
		if (errcode != CAPI_NOERROR) {
			printk(KERN_ERR "capi: send DATA_B3_RESP failed=%x\n",
					errcode);
			kfree_skb(nskb);
			return -1;
		}
		(void)skb_pull(skb, CAPIMSG_LEN(skb->data));
#ifdef _DEBUG_DATAFLOW
		printk(KERN_DEBUG "capi: DATA_B3_RESP %u len=%d => raw\n",
					datahandle, skb->len);
#endif
		skb_queue_tail(&mp->recvqueue, skb);
		wake_up_interruptible(&mp->recvwait);
		return 0;
	}
#ifdef _DEBUG_DATAFLOW
	printk(KERN_DEBUG "capi: currently no receiver\n");
#endif
	return -1;
}

void handle_minor_recv(struct capiminor *mp)
{
	struct sk_buff *skb;
	while ((skb = skb_dequeue(&mp->inqueue)) != 0) {
		unsigned int len = skb->len;
		mp->inbytes -= len;
		if (handle_recv_skb(mp, skb) < 0) {
			skb_queue_head(&mp->inqueue, skb);
			mp->inbytes += len;
			return;
		}
	}
}

int handle_minor_send(struct capiminor *mp)
{
	struct sk_buff *skb;
	__u16 len;
	int count = 0;
	__u16 errcode;
	__u16 datahandle;

	if (mp->tty && mp->ttyoutstop) {
#if defined(_DEBUG_DATAFLOW) || defined(_DEBUG_TTYFUNCS)
		printk(KERN_DEBUG "capi: send: tty stopped\n");
#endif
		return 0;
	}

	while ((skb = skb_dequeue(&mp->outqueue)) != 0) {
		datahandle = mp->datahandle;
		len = (__u16)skb->len;
		skb_push(skb, CAPI_DATA_B3_REQ_LEN);
		memset(skb->data, 0, CAPI_DATA_B3_REQ_LEN);
		capimsg_setu16(skb->data, 0, CAPI_DATA_B3_REQ_LEN);
		capimsg_setu16(skb->data, 2, mp->applid);
		capimsg_setu8 (skb->data, 4, CAPI_DATA_B3);
		capimsg_setu8 (skb->data, 5, CAPI_REQ);
		capimsg_setu16(skb->data, 6, mp->msgid++);
		capimsg_setu32(skb->data, 8, mp->ncci);	/* NCCI */
		capimsg_setu32(skb->data, 12, (__u32) skb->data); /* Data32 */
		capimsg_setu16(skb->data, 16, len);	/* Data length */
		capimsg_setu16(skb->data, 18, datahandle);
		capimsg_setu16(skb->data, 20, 0);	/* Flags */

		if (capincci_add_ack(mp, datahandle) < 0) {
			skb_pull(skb, CAPI_DATA_B3_REQ_LEN);
			skb_queue_head(&mp->outqueue, skb);
			return count;
		}
		errcode = (*capifuncs->capi_put_message) (mp->applid, skb);
		if (errcode == CAPI_NOERROR) {
			mp->datahandle++;
			count++;
			mp->outbytes -= len;
#ifdef _DEBUG_DATAFLOW
			printk(KERN_DEBUG "capi: DATA_B3_REQ %u len=%u\n",
							datahandle, len);
#endif
			continue;
		}
		capiminor_del_ack(mp, datahandle);

		if (errcode == CAPI_SENDQUEUEFULL) {
			skb_pull(skb, CAPI_DATA_B3_REQ_LEN);
			skb_queue_head(&mp->outqueue, skb);
			break;
		}

		/* ups, drop packet */
		printk(KERN_ERR "capi: put_message = %x\n", errcode);
		mp->outbytes -= len;
		kfree_skb(skb);
	}
	if (count)
		wake_up_interruptible(&mp->sendwait);
	return count;
}

#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */
/* -------- function called by lower level -------------------------- */

static void capi_signal(__u16 applid, void *param)
{
	struct capidev *cdev = (struct capidev *)param;
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
	struct capiminor *mp;
	__u16 datahandle;
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */
	struct capincci *np;
	struct sk_buff *skb = 0;
	__u32 ncci;

	(void) (*capifuncs->capi_get_message) (applid, &skb);
	if (!skb) {
		printk(KERN_ERR "BUG: capi_signal: no skb\n");
		return;
	}

	if (CAPIMSG_COMMAND(skb->data) != CAPI_DATA_B3) {
		skb_queue_tail(&cdev->recvqueue, skb);
		wake_up_interruptible(&cdev->recvwait);
		return;
	}
	ncci = CAPIMSG_CONTROL(skb->data);
	for (np = cdev->nccis; np && np->ncci != ncci; np = np->next)
		;
	if (!np) {
		printk(KERN_ERR "BUG: capi_signal: ncci not found\n");
		skb_queue_tail(&cdev->recvqueue, skb);
		wake_up_interruptible(&cdev->recvwait);
		return;
	}
#ifndef CONFIG_ISDN_CAPI_MIDDLEWARE
	skb_queue_tail(&cdev->recvqueue, skb);
	wake_up_interruptible(&cdev->recvwait);
#else /* CONFIG_ISDN_CAPI_MIDDLEWARE */
	mp = np->minorp;
	if (!mp) {
		skb_queue_tail(&cdev->recvqueue, skb);
		wake_up_interruptible(&cdev->recvwait);
		return;
	}


	if (CAPIMSG_SUBCOMMAND(skb->data) == CAPI_IND) {
		datahandle = CAPIMSG_U16(skb->data, CAPIMSG_BASELEN+4+4+2);
#ifdef _DEBUG_DATAFLOW
		printk(KERN_DEBUG "capi_signal: DATA_B3_IND %u len=%d\n",
				datahandle, skb->len-CAPIMSG_LEN(skb->data));
#endif
		skb_queue_tail(&mp->inqueue, skb);
		mp->inbytes += skb->len;
		handle_minor_recv(mp);

	} else if (CAPIMSG_SUBCOMMAND(skb->data) == CAPI_CONF) {

		datahandle = CAPIMSG_U16(skb->data, CAPIMSG_BASELEN+4);
#ifdef _DEBUG_DATAFLOW
		printk(KERN_DEBUG "capi_signal: DATA_B3_CONF %u 0x%x\n",
				datahandle,
				CAPIMSG_U16(skb->data, CAPIMSG_BASELEN+4+2));
#endif
		kfree_skb(skb);
		(void)capiminor_del_ack(mp, datahandle);
		if (mp->tty) {
			if (mp->tty->ldisc.write_wakeup)
				mp->tty->ldisc.write_wakeup(mp->tty);
		} else {
			wake_up_interruptible(&mp->sendwait);
		}
		(void)handle_minor_send(mp);

	} else {
		/* ups, let capi application handle it :-) */
		skb_queue_tail(&cdev->recvqueue, skb);
		wake_up_interruptible(&cdev->recvwait);
	}
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */
}

/* -------- file_operations for capidev ----------------------------- */

static loff_t
capi_llseek(struct file *file, loff_t offset, int origin)
{
	return -ESPIPE;
}

static ssize_t
capi_read(struct file *file, char *buf, size_t count, loff_t *ppos)
{
	struct capidev *cdev = (struct capidev *)file->private_data;
	struct sk_buff *skb;
	int retval;
	size_t copied;

	if (ppos != &file->f_pos)
		return -ESPIPE;

	if (!cdev->applid)
		return -ENODEV;

	if ((skb = skb_dequeue(&cdev->recvqueue)) == 0) {

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		for (;;) {
			interruptible_sleep_on(&cdev->recvwait);
			if ((skb = skb_dequeue(&cdev->recvqueue)) != 0)
				break;
			if (signal_pending(current))
				break;
		}
		if (skb == 0)
			return -ERESTARTNOHAND;
	}
	if (skb->len > count) {
		skb_queue_head(&cdev->recvqueue, skb);
		return -EMSGSIZE;
	}
	retval = copy_to_user(buf, skb->data, skb->len);
	if (retval) {
		skb_queue_head(&cdev->recvqueue, skb);
		return retval;
	}
	copied = skb->len;

	if (CAPIMSG_CMD(skb->data) == CAPI_DATA_B3_IND) {
		cdev->nrecvdatapkt++;
	} else {
		cdev->nrecvctlpkt++;
	}

	kfree_skb(skb);

	return copied;
}

static ssize_t
capi_write(struct file *file, const char *buf, size_t count, loff_t *ppos)
{
	struct capidev *cdev = (struct capidev *)file->private_data;
	struct sk_buff *skb;
	int retval;
	__u16 mlen;

        if (ppos != &file->f_pos)
		return -ESPIPE;

	if (!cdev->applid)
		return -ENODEV;

	skb = alloc_skb(count, GFP_USER);

	if ((retval = copy_from_user(skb_put(skb, count), buf, count))) {
		kfree_skb(skb);
		return retval;
	}
	mlen = CAPIMSG_LEN(skb->data);
	if (CAPIMSG_CMD(skb->data) == CAPI_DATA_B3_REQ) {
		if (mlen + CAPIMSG_DATALEN(skb->data) != count) {
			kfree_skb(skb);
			return -EINVAL;
		}
	} else {
		if (mlen != count) {
			kfree_skb(skb);
			return -EINVAL;
		}
	}
	CAPIMSG_SETAPPID(skb->data, cdev->applid);

	cdev->errcode = (*capifuncs->capi_put_message) (cdev->applid, skb);

	if (cdev->errcode) {
		kfree_skb(skb);
		return -EIO;
	}
	if (CAPIMSG_CMD(skb->data) == CAPI_DATA_B3_REQ) {
		cdev->nsentdatapkt++;
	} else {
		cdev->nsentctlpkt++;
	}
	return count;
}

static unsigned int
capi_poll(struct file *file, poll_table * wait)
{
	struct capidev *cdev = (struct capidev *)file->private_data;
	unsigned int mask = 0;

	if (!cdev->applid)
		return POLLERR;

	poll_wait(file, &(cdev->recvwait), wait);
	mask = POLLOUT | POLLWRNORM;
	if (!skb_queue_empty(&cdev->recvqueue))
		mask |= POLLIN | POLLRDNORM;
	return mask;
}

static int
capi_ioctl(struct inode *inode, struct file *file,
		      unsigned int cmd, unsigned long arg)
{
	struct capidev *cdev = (struct capidev *)file->private_data;
	capi_ioctl_struct data;
	int retval = -EINVAL;

	switch (cmd) {
	case CAPI_REGISTER:
		{
			retval = copy_from_user((void *) &data.rparams,
						(void *) arg, sizeof(struct capi_register_params));
			if (retval)
				return -EFAULT;
			if (cdev->applid)
				return -EEXIST;
			cdev->errcode = (*capifuncs->capi_register) (&data.rparams,
							  &cdev->applid);
			if (cdev->errcode) {
				cdev->applid = 0;
				return -EIO;
			}
			(void) (*capifuncs->capi_set_signal) (cdev->applid, capi_signal, cdev);
		}
		return (int)cdev->applid;

	case CAPI_GET_VERSION:
		{
			retval = copy_from_user((void *) &data.contr,
						(void *) arg,
						sizeof(data.contr));
			if (retval)
				return -EFAULT;
		        cdev->errcode = (*capifuncs->capi_get_version) (data.contr, &data.version);
			if (cdev->errcode)
				return -EIO;
			retval = copy_to_user((void *) arg,
					      (void *) &data.version,
					      sizeof(data.version));
			if (retval)
				return -EFAULT;
		}
		return 0;

	case CAPI_GET_SERIAL:
		{
			retval = copy_from_user((void *) &data.contr,
						(void *) arg,
						sizeof(data.contr));
			if (retval)
				return -EFAULT;
			cdev->errcode = (*capifuncs->capi_get_serial) (data.contr, data.serial);
			if (cdev->errcode)
				return -EIO;
			retval = copy_to_user((void *) arg,
					      (void *) data.serial,
					      sizeof(data.serial));
			if (retval)
				return -EFAULT;
		}
		return 0;
	case CAPI_GET_PROFILE:
		{
			retval = copy_from_user((void *) &data.contr,
						(void *) arg,
						sizeof(data.contr));
			if (retval)
				return -EFAULT;

			if (data.contr == 0) {
				cdev->errcode = (*capifuncs->capi_get_profile) (data.contr, &data.profile);
				if (cdev->errcode)
					return -EIO;

				retval = copy_to_user((void *) arg,
				      (void *) &data.profile.ncontroller,
				       sizeof(data.profile.ncontroller));

			} else {
				cdev->errcode = (*capifuncs->capi_get_profile) (data.contr, &data.profile);
				if (cdev->errcode)
					return -EIO;

				retval = copy_to_user((void *) arg,
						  (void *) &data.profile,
						   sizeof(data.profile));
			}
			if (retval)
				return -EFAULT;
		}
		return 0;

	case CAPI_GET_MANUFACTURER:
		{
			retval = copy_from_user((void *) &data.contr,
						(void *) arg,
						sizeof(data.contr));
			if (retval)
				return -EFAULT;
			cdev->errcode = (*capifuncs->capi_get_manufacturer) (data.contr, data.manufacturer);
			if (cdev->errcode)
				return -EIO;

			retval = copy_to_user((void *) arg, (void *) data.manufacturer,
					      sizeof(data.manufacturer));
			if (retval)
				return -EFAULT;

		}
		return 0;
	case CAPI_GET_ERRCODE:
		data.errcode = cdev->errcode;
		cdev->errcode = CAPI_NOERROR;
		if (arg) {
			retval = copy_to_user((void *) arg,
					      (void *) &data.errcode,
					      sizeof(data.errcode));
			if (retval)
				return -EFAULT;
		}
		return data.errcode;

	case CAPI_INSTALLED:
		if ((*capifuncs->capi_isinstalled)() == CAPI_NOERROR)
			return 0;
		return -ENXIO;

	case CAPI_MANUFACTURER_CMD:
		{
			struct capi_manufacturer_cmd mcmd;
			if (!capable(CAP_SYS_ADMIN))
				return -EPERM;
			retval = copy_from_user((void *) &mcmd, (void *) arg,
						sizeof(mcmd));
			if (retval)
				return -EFAULT;
			return (*capifuncs->capi_manufacturer) (mcmd.cmd, mcmd.data);
		}
		return 0;

	case CAPI_SET_FLAGS:
	case CAPI_CLR_FLAGS:
		{
			unsigned userflags;
			retval = copy_from_user((void *) &userflags,
						(void *) arg,
						sizeof(userflags));
			if (retval)
				return -EFAULT;
			if (cmd == CAPI_SET_FLAGS)
				cdev->userflags |= userflags;
			else
				cdev->userflags &= ~userflags;
		}
		return 0;

	case CAPI_GET_FLAGS:
		{
			retval = copy_to_user((void *) arg,
					      (void *) &cdev->userflags,
					      sizeof(cdev->userflags));
			if (retval)
				return -EFAULT;
		}
		return 0;

	case CAPI_NCCI_OPENCOUNT:
		{
			struct capincci *nccip;
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
			struct capiminor *mp;
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */
			unsigned ncci;
			int count = 0;
			retval = copy_from_user((void *) &ncci,
						(void *) arg,
						sizeof(ncci));
			if (retval)
				return -EFAULT;
			nccip = capincci_find(cdev, (__u32) ncci);
			if (!nccip)
				return 0;
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
			if ((mp = nccip->minorp) != 0) {
				count += atomic_read(&mp->ttyopencount);
				if (mp->file)
					count++;
			}
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */
			return count;
		}
		return 0;

#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
	case CAPI_NCCI_GETUNIT:
		{
			struct capincci *nccip;
			struct capiminor *mp;
			unsigned ncci;
			retval = copy_from_user((void *) &ncci,
						(void *) arg,
						sizeof(ncci));
			if (retval)
				return -EFAULT;
			nccip = capincci_find(cdev, (__u32) ncci);
			if (!nccip || (mp = nccip->minorp) == 0)
				return -ESRCH;
			return mp->minor;
		}
		return 0;
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */
	}
	return -EINVAL;
}

static int
capi_open(struct inode *inode, struct file *file)
{
	if (file->private_data)
		return -EEXIST;

	if ((file->private_data = capidev_alloc(file)) == 0)
		return -ENOMEM;

	MOD_INC_USE_COUNT;
#ifdef _DEBUG_REFCOUNT
	printk(KERN_DEBUG "capi_open %d\n", GET_USE_COUNT(THIS_MODULE));
#endif
	return 0;
}

static int
capi_release(struct inode *inode, struct file *file)
{
	struct capidev *cdev = (struct capidev *)file->private_data;

	lock_kernel();
	capincci_free(cdev, 0xffffffff);
	capidev_free(cdev);
	file->private_data = NULL;
	
	MOD_DEC_USE_COUNT;
#ifdef _DEBUG_REFCOUNT
	printk(KERN_DEBUG "capi_release %d\n", GET_USE_COUNT(THIS_MODULE));
#endif
	unlock_kernel();
	return 0;
}

static struct file_operations capi_fops =
{
	owner:		THIS_MODULE,
	llseek:		capi_llseek,
	read:		capi_read,
	write:		capi_write,
	poll:		capi_poll,
	ioctl:		capi_ioctl,
	open:		capi_open,
	release:	capi_release,
};

#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
/* -------- file_operations for capincci ---------------------------- */

static int
capinc_raw_open(struct inode *inode, struct file *file)
{
	struct capiminor *mp;

	if (file->private_data)
		return -EEXIST;
	if ((mp = capiminor_find(MINOR(file->f_dentry->d_inode->i_rdev))) == 0)
		return -ENXIO;
	if (mp->nccip == 0)
		return -ENXIO;
	if (mp->file)
		return -EBUSY;

#ifdef _DEBUG_REFCOUNT
	printk(KERN_DEBUG "capi_raw_open %d\n", GET_USE_COUNT(THIS_MODULE));
#endif

	mp->datahandle = 0;
	mp->file = file;
	file->private_data = (void *)mp;
	handle_minor_recv(mp);
	return 0;
}

static loff_t
capinc_raw_llseek(struct file *file, loff_t offset, int origin)
{
	return -ESPIPE;
}

static ssize_t
capinc_raw_read(struct file *file, char *buf, size_t count, loff_t *ppos)
{
	struct capiminor *mp = (struct capiminor *)file->private_data;
	struct sk_buff *skb;
	int retval;
	size_t copied = 0;

        if (ppos != &file->f_pos)
		return -ESPIPE;

	if (!mp || !mp->nccip)
		return -EINVAL;

	if ((skb = skb_dequeue(&mp->recvqueue)) == 0) {

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		for (;;) {
			interruptible_sleep_on(&mp->recvwait);
			if (mp->nccip == 0)
				return 0;
			if ((skb = skb_dequeue(&mp->recvqueue)) != 0)
				break;
			if (signal_pending(current))
				break;
		}
		if (skb == 0)
			return -ERESTARTNOHAND;
	}
	do {
		if (count < skb->len) {
			retval = copy_to_user(buf, skb->data, count);
			if (retval) {
				skb_queue_head(&mp->recvqueue, skb);
				return retval;
			}
			skb_pull(skb, count);
			skb_queue_head(&mp->recvqueue, skb);
			copied += count;
			return copied;
		} else {
			retval = copy_to_user(buf, skb->data, skb->len);
			if (retval) {
				skb_queue_head(&mp->recvqueue, skb);
				return copied;
			}
			copied += skb->len;
			count -= skb->len;
			buf += skb->len;
			kfree_skb(skb);
		}
	} while ((skb = skb_dequeue(&mp->recvqueue)) != 0);

	return copied;
}

static ssize_t
capinc_raw_write(struct file *file, const char *buf, size_t count, loff_t *ppos)
{
	struct capiminor *mp = (struct capiminor *)file->private_data;
	struct sk_buff *skb;
	int retval;

        if (ppos != &file->f_pos)
		return -ESPIPE;

	if (!mp || !mp->nccip)
		return -EINVAL;

	skb = alloc_skb(CAPI_DATA_B3_REQ_LEN+count, GFP_USER);

	skb_reserve(skb, CAPI_DATA_B3_REQ_LEN);
	if ((retval = copy_from_user(skb_put(skb, count), buf, count))) {
		kfree_skb(skb);
		return retval;
	}

	while (skb_queue_len(&mp->outqueue) > CAPINC_MAX_SENDQUEUE) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		interruptible_sleep_on(&mp->sendwait);
		if (mp->nccip == 0) {
			kfree_skb(skb);
			return -EIO;
		}
		if (signal_pending(current))
			return -ERESTARTNOHAND;
	}
	skb_queue_tail(&mp->outqueue, skb);
	mp->outbytes += skb->len;
	(void)handle_minor_send(mp);
	return count;
}

static unsigned int
capinc_raw_poll(struct file *file, poll_table * wait)
{
	struct capiminor *mp = (struct capiminor *)file->private_data;
	unsigned int mask = 0;

	if (!mp || !mp->nccip)
		return POLLERR|POLLHUP;

	poll_wait(file, &(mp->recvwait), wait);
	if (!skb_queue_empty(&mp->recvqueue))
		mask |= POLLIN | POLLRDNORM;
	poll_wait(file, &(mp->sendwait), wait);
	if (skb_queue_len(&mp->outqueue) > CAPINC_MAX_SENDQUEUE)
		mask = POLLOUT | POLLWRNORM;
	return mask;
}

static int
capinc_raw_ioctl(struct inode *inode, struct file *file,
		      unsigned int cmd, unsigned long arg)
{
	struct capiminor *mp = (struct capiminor *)file->private_data;
	if (!mp || !mp->nccip)
		return -EINVAL;

	switch (cmd) {
	}
	return -EINVAL;
}

static int
capinc_raw_release(struct inode *inode, struct file *file)
{
	struct capiminor *mp = (struct capiminor *)file->private_data;

	if (mp) {
		lock_kernel();
		mp->file = 0;
		if (mp->nccip == 0) {
			capiminor_free(mp);
			file->private_data = NULL;
		}
		unlock_kernel();
	}

#ifdef _DEBUG_REFCOUNT
	printk(KERN_DEBUG "capinc_raw_release %d\n", GET_USE_COUNT(THIS_MODULE));
#endif
	return 0;
}

static struct file_operations capinc_raw_fops =
{
	owner:		THIS_MODULE,
	llseek:		capinc_raw_llseek,
	read:		capinc_raw_read,
	write:		capinc_raw_write,
	poll:		capinc_raw_poll,
	ioctl:		capinc_raw_ioctl,
	open:		capinc_raw_open,
	release:	capinc_raw_release,
};

/* -------- tty_operations for capincci ----------------------------- */

int capinc_tty_open(struct tty_struct * tty, struct file * file)
{
	struct capiminor *mp;

	if ((mp = capiminor_find(MINOR(file->f_dentry->d_inode->i_rdev))) == 0)
		return -ENXIO;
	if (mp->nccip == 0)
		return -ENXIO;
	if (mp->file)
		return -EBUSY;

	skb_queue_head_init(&mp->recvqueue);
	init_waitqueue_head(&mp->recvwait);
	init_waitqueue_head(&mp->sendwait);
	tty->driver_data = (void *)mp;
#ifdef _DEBUG_REFCOUNT
	printk(KERN_DEBUG "capi_tty_open %d\n", GET_USE_COUNT(THIS_MODULE));
#endif
	if (atomic_read(&mp->ttyopencount) == 0)
		mp->tty = tty;
	atomic_inc(&mp->ttyopencount);
#ifdef _DEBUG_REFCOUNT
	printk(KERN_DEBUG "capinc_tty_open ocount=%d\n", atomic_read(&mp->ttyopencount));
#endif
	handle_minor_recv(mp);
	return 0;
}

void capinc_tty_close(struct tty_struct * tty, struct file * file)
{
	struct capiminor *mp;

	mp = (struct capiminor *)tty->driver_data;
	if (mp)	{
		if (atomic_dec_and_test(&mp->ttyopencount)) {
#ifdef _DEBUG_REFCOUNT
			printk(KERN_DEBUG "capinc_tty_close lastclose\n");
#endif
			tty->driver_data = (void *)0;
			mp->tty = 0;
		}
#ifdef _DEBUG_REFCOUNT
		printk(KERN_DEBUG "capinc_tty_close ocount=%d\n", atomic_read(&mp->ttyopencount));
#endif
		if (mp->nccip == 0)
			capiminor_free(mp);
	}

#ifdef _DEBUG_REFCOUNT
	printk(KERN_DEBUG "capinc_tty_close\n");
#endif
}

int capinc_tty_write(struct tty_struct * tty, int from_user,
		      const unsigned char *buf, int count)
{
	struct capiminor *mp = (struct capiminor *)tty->driver_data;
	struct sk_buff *skb;
	int retval;

#ifdef _DEBUG_TTYFUNCS
	printk(KERN_DEBUG "capinc_tty_write(from_user=%d,count=%d)\n",
				from_user, count);
#endif

	if (!mp || !mp->nccip) {
#ifdef _DEBUG_TTYFUNCS
		printk(KERN_DEBUG "capinc_tty_write: mp or mp->ncci NULL\n");
#endif
		return 0;
	}

	skb = mp->ttyskb;
	if (skb) {
		mp->ttyskb = 0;
		skb_queue_tail(&mp->outqueue, skb);
		mp->outbytes += skb->len;
	}

	skb = alloc_skb(CAPI_DATA_B3_REQ_LEN+count, GFP_ATOMIC);
	if (!skb) {
		printk(KERN_ERR "capinc_tty_write: alloc_skb failed\n");
		return -ENOMEM;
	}

	skb_reserve(skb, CAPI_DATA_B3_REQ_LEN);
	if (from_user) {
		if ((retval = copy_from_user(skb_put(skb, count), buf, count))) {
			kfree_skb(skb);
#ifdef _DEBUG_TTYFUNCS
			printk(KERN_DEBUG "capinc_tty_write: copy_from_user=%d\n", retval);
#endif
			return retval;
		}
	} else {
		memcpy(skb_put(skb, count), buf, count);
	}

	skb_queue_tail(&mp->outqueue, skb);
	mp->outbytes += skb->len;
	(void)handle_minor_send(mp);
	(void)handle_minor_recv(mp);
	return count;
}

void capinc_tty_put_char(struct tty_struct *tty, unsigned char ch)
{
	struct capiminor *mp = (struct capiminor *)tty->driver_data;
	struct sk_buff *skb;

#ifdef _DEBUG_TTYFUNCS
	printk(KERN_DEBUG "capinc_put_char(%u)\n", ch);
#endif

	if (!mp || !mp->nccip) {
#ifdef _DEBUG_TTYFUNCS
		printk(KERN_DEBUG "capinc_tty_put_char: mp or mp->ncci NULL\n");
#endif
		return;
	}

	skb = mp->ttyskb;
	if (skb) {
		if (skb_tailroom(skb) > 0) {
			*(skb_put(skb, 1)) = ch;
			return;
		}
		mp->ttyskb = 0;
		skb_queue_tail(&mp->outqueue, skb);
		mp->outbytes += skb->len;
		(void)handle_minor_send(mp);
	}
	skb = alloc_skb(CAPI_DATA_B3_REQ_LEN+CAPI_MAX_BLKSIZE, GFP_ATOMIC);
	if (skb) {
		skb_reserve(skb, CAPI_DATA_B3_REQ_LEN);
		*(skb_put(skb, 1)) = ch;
		mp->ttyskb = skb;
	} else {
		printk(KERN_ERR "capinc_put_char: char %u lost\n", ch);
	}
}

void capinc_tty_flush_chars(struct tty_struct *tty)
{
	struct capiminor *mp = (struct capiminor *)tty->driver_data;
	struct sk_buff *skb;

#ifdef _DEBUG_TTYFUNCS
	printk(KERN_DEBUG "capinc_tty_flush_chars\n");
#endif

	if (!mp || !mp->nccip) {
#ifdef _DEBUG_TTYFUNCS
		printk(KERN_DEBUG "capinc_tty_flush_chars: mp or mp->ncci NULL\n");
#endif
		return;
	}

	skb = mp->ttyskb;
	if (skb) {
		mp->ttyskb = 0;
		skb_queue_tail(&mp->outqueue, skb);
		mp->outbytes += skb->len;
		(void)handle_minor_send(mp);
	}
	(void)handle_minor_recv(mp);
}

int capinc_tty_write_room(struct tty_struct *tty)
{
	struct capiminor *mp = (struct capiminor *)tty->driver_data;
	int room;
	if (!mp || !mp->nccip) {
#ifdef _DEBUG_TTYFUNCS
		printk(KERN_DEBUG "capinc_tty_write_room: mp or mp->ncci NULL\n");
#endif
		return 0;
	}
	room = CAPINC_MAX_SENDQUEUE-skb_queue_len(&mp->outqueue);
	room *= CAPI_MAX_BLKSIZE;
#ifdef _DEBUG_TTYFUNCS
	printk(KERN_DEBUG "capinc_tty_write_room = %d\n", room);
#endif
	return room;
}

int capinc_tty_chars_in_buffer(struct tty_struct *tty)
{
	struct capiminor *mp = (struct capiminor *)tty->driver_data;
	if (!mp || !mp->nccip) {
#ifdef _DEBUG_TTYFUNCS
		printk(KERN_DEBUG "capinc_tty_chars_in_buffer: mp or mp->ncci NULL\n");
#endif
		return 0;
	}
#ifdef _DEBUG_TTYFUNCS
	printk(KERN_DEBUG "capinc_tty_chars_in_buffer = %d nack=%d sq=%d rq=%d\n",
			mp->outbytes, mp->nack,
			skb_queue_len(&mp->outqueue),
			skb_queue_len(&mp->inqueue));
#endif
	return mp->outbytes;
}

int capinc_tty_ioctl(struct tty_struct *tty, struct file * file,
		    unsigned int cmd, unsigned long arg)
{
	int error = 0;
	switch (cmd) {
	default:
		error = n_tty_ioctl (tty, file, cmd, arg);
		break;
	}
	return error;
}

void capinc_tty_set_termios(struct tty_struct *tty, struct termios * old)
{
#ifdef _DEBUG_TTYFUNCS
	printk(KERN_DEBUG "capinc_tty_set_termios\n");
#endif
}

void capinc_tty_throttle(struct tty_struct * tty)
{
	struct capiminor *mp = (struct capiminor *)tty->driver_data;
#ifdef _DEBUG_TTYFUNCS
	printk(KERN_DEBUG "capinc_tty_throttle\n");
#endif
	if (mp)
		mp->ttyinstop = 1;
}

void capinc_tty_unthrottle(struct tty_struct * tty)
{
	struct capiminor *mp = (struct capiminor *)tty->driver_data;
#ifdef _DEBUG_TTYFUNCS
	printk(KERN_DEBUG "capinc_tty_unthrottle\n");
#endif
	if (mp) {
		mp->ttyinstop = 0;
		handle_minor_recv(mp);
	}
}

void capinc_tty_stop(struct tty_struct *tty)
{
	struct capiminor *mp = (struct capiminor *)tty->driver_data;
#ifdef _DEBUG_TTYFUNCS
	printk(KERN_DEBUG "capinc_tty_stop\n");
#endif
	if (mp) {
		mp->ttyoutstop = 1;
	}
}

void capinc_tty_start(struct tty_struct *tty)
{
	struct capiminor *mp = (struct capiminor *)tty->driver_data;
#ifdef _DEBUG_TTYFUNCS
	printk(KERN_DEBUG "capinc_tty_start\n");
#endif
	if (mp) {
		mp->ttyoutstop = 0;
		(void)handle_minor_send(mp);
	}
}

void capinc_tty_hangup(struct tty_struct *tty)
{
#ifdef _DEBUG_TTYFUNCS
	printk(KERN_DEBUG "capinc_tty_hangup\n");
#endif
}

void capinc_tty_break_ctl(struct tty_struct *tty, int state)
{
#ifdef _DEBUG_TTYFUNCS
	printk(KERN_DEBUG "capinc_tty_break_ctl(%d)\n", state);
#endif
}

void capinc_tty_flush_buffer(struct tty_struct *tty)
{
#ifdef _DEBUG_TTYFUNCS
	printk(KERN_DEBUG "capinc_tty_flush_buffer\n");
#endif
}

void capinc_tty_set_ldisc(struct tty_struct *tty)
{
#ifdef _DEBUG_TTYFUNCS
	printk(KERN_DEBUG "capinc_tty_set_ldisc\n");
#endif
}

void capinc_tty_send_xchar(struct tty_struct *tty, char ch)
{
#ifdef _DEBUG_TTYFUNCS
	printk(KERN_DEBUG "capinc_tty_send_xchar(%d)\n", ch);
#endif
}

int capinc_tty_read_proc(char *page, char **start, off_t off,
			  int count, int *eof, void *data)
{
	return 0;
}

int capinc_write_proc(struct file *file, const char *buffer,
			  unsigned long count, void *data)
{
	return 0;
}

#define CAPINC_NR_PORTS 256
static struct tty_driver capinc_tty_driver;
static int capinc_tty_refcount;
static struct tty_struct *capinc_tty_table[CAPINC_NR_PORTS];
static struct termios *capinc_tty_termios[CAPINC_NR_PORTS];
static struct termios *capinc_tty_termios_locked[CAPINC_NR_PORTS];

int capinc_tty_init(void)
{
	struct tty_driver *drv = &capinc_tty_driver;

	/* Initialize the tty_driver structure */
	
	memset(drv, 0, sizeof(struct tty_driver));
	drv->magic = TTY_DRIVER_MAGIC;
#if (LINUX_VERSION_CODE > 0x20100)
	drv->driver_name = "capi_nc";
#endif
	drv->name = "capi/%d";
	drv->major = capi_ttymajor;
	drv->minor_start = 0;
	drv->num = CAPINC_NR_PORTS;
	drv->type = TTY_DRIVER_TYPE_SERIAL;
	drv->subtype = SERIAL_TYPE_NORMAL;
	drv->init_termios = tty_std_termios;
	drv->init_termios.c_iflag = ICRNL;
	drv->init_termios.c_oflag = OPOST | ONLCR;
	drv->init_termios.c_cflag = B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	drv->init_termios.c_lflag = 0;
	drv->flags = TTY_DRIVER_REAL_RAW|TTY_DRIVER_RESET_TERMIOS;
	drv->refcount = &capinc_tty_refcount;
	drv->table = capinc_tty_table;
	drv->termios = capinc_tty_termios;
	drv->termios_locked = capinc_tty_termios_locked;

	drv->open = capinc_tty_open;
	drv->close = capinc_tty_close;
	drv->write = capinc_tty_write;
	drv->put_char = capinc_tty_put_char;
	drv->flush_chars = capinc_tty_flush_chars;
	drv->write_room = capinc_tty_write_room;
	drv->chars_in_buffer = capinc_tty_chars_in_buffer;
	drv->ioctl = capinc_tty_ioctl;
	drv->set_termios = capinc_tty_set_termios;
	drv->throttle = capinc_tty_throttle;
	drv->unthrottle = capinc_tty_unthrottle;
	drv->stop = capinc_tty_stop;
	drv->start = capinc_tty_start;
	drv->hangup = capinc_tty_hangup;
#if (LINUX_VERSION_CODE >= 131394) /* Linux 2.1.66 */
	drv->break_ctl = capinc_tty_break_ctl;
#endif
	drv->flush_buffer = capinc_tty_flush_buffer;
	drv->set_ldisc = capinc_tty_set_ldisc;
#if (LINUX_VERSION_CODE >= 131343)
	drv->send_xchar = capinc_tty_send_xchar;
	drv->read_proc = capinc_tty_read_proc;
#endif
	if (tty_register_driver(drv)) {
		printk(KERN_ERR "Couldn't register capi_nc driver\n");
		return -1;
	}
	return 0;
}

void capinc_tty_exit(void)
{
	struct tty_driver *drv = &capinc_tty_driver;
	int retval;
	if ((retval = tty_unregister_driver(drv)))
		printk(KERN_ERR "capi: failed to unregister capi_nc driver (%d)\n", retval);
}

#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */

/* -------- /proc functions ----------------------------------------- */

/*
 * /proc/capi/capi20:
 *  minor applid nrecvctlpkt nrecvdatapkt nsendctlpkt nsenddatapkt
 */
static int proc_capidev_read_proc(char *page, char **start, off_t off,
                                       int count, int *eof, void *data)
{
        struct capidev *cdev;
	int len = 0;

	for (cdev=capidev_openlist; cdev; cdev = cdev->next) {
		len += sprintf(page+len, "%d %d %lu %lu %lu %lu\n",
			cdev->minor,
			cdev->applid,
			cdev->nrecvctlpkt,
			cdev->nrecvdatapkt,
			cdev->nsentctlpkt,
			cdev->nsentdatapkt);
		if (len <= off) {
			off -= len;
			len = 0;
		} else {
			if (len-off > count)
				goto endloop;
		}
	}
endloop:
	if (len < count)
		*eof = 1;
	if (len>count) len = count;
	if (len<0) len = 0;
	return len;
}

/*
 * /proc/capi/capi20ncci:
 *  applid ncci
 */
static int proc_capincci_read_proc(char *page, char **start, off_t off,
                                       int count, int *eof, void *data)
{
        struct capidev *cdev;
        struct capincci *np;
	int len = 0;

	for (cdev=capidev_openlist; cdev; cdev = cdev->next) {
		for (np=cdev->nccis; np; np = np->next) {
			len += sprintf(page+len, "%d 0x%x%s\n",
				cdev->applid,
				np->ncci,
#ifndef CONFIG_ISDN_CAPI_MIDDLEWARE
				"");
#else /* CONFIG_ISDN_CAPI_MIDDLEWARE */
				np->minorp && np->minorp->file ? " open" : "");
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */
			if (len <= off) {
				off -= len;
				len = 0;
			} else {
				if (len-off > count)
					goto endloop;
			}
		}
	}
endloop:
	*start = page+off;
	if (len < count)
		*eof = 1;
	if (len>count) len = count;
	if (len<0) len = 0;
	return len;
}

static struct procfsentries {
  char *name;
  mode_t mode;
  int (*read_proc)(char *page, char **start, off_t off,
                                       int count, int *eof, void *data);
  struct proc_dir_entry *procent;
} procfsentries[] = {
   /* { "capi",		  S_IFDIR, 0 }, */
   { "capi/capi20", 	  0	 , proc_capidev_read_proc },
   { "capi/capi20ncci",   0	 , proc_capincci_read_proc },
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

/* -------- init function and module interface ---------------------- */


static void __exit alloc_exit(void)
{
	if (capidev_cachep) {
		(void)kmem_cache_destroy(capidev_cachep);
		capidev_cachep = 0;
	}
	if (capincci_cachep) {
		(void)kmem_cache_destroy(capincci_cachep);
		capincci_cachep = 0;
	}
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
	if (capidh_cachep) {
		(void)kmem_cache_destroy(capidh_cachep);
		capidh_cachep = 0;
	}
	if (capiminor_cachep) {
		(void)kmem_cache_destroy(capiminor_cachep);
		capiminor_cachep = 0;
	}
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */
}

static int __init alloc_init(void)
{
	capidev_cachep = kmem_cache_create("capi20_dev",
					 sizeof(struct capidev),
					 0,
					 SLAB_HWCACHE_ALIGN,
					 NULL, NULL);
	if (!capidev_cachep) {
		alloc_exit();
		return -ENOMEM;
	}

	capincci_cachep = kmem_cache_create("capi20_ncci",
					 sizeof(struct capincci),
					 0,
					 SLAB_HWCACHE_ALIGN,
					 NULL, NULL);
	if (!capincci_cachep) {
		alloc_exit();
		return -ENOMEM;
	}
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
	capidh_cachep = kmem_cache_create("capi20_dh",
					 sizeof(struct datahandle_queue),
					 0,
					 SLAB_HWCACHE_ALIGN,
					 NULL, NULL);
	if (!capidh_cachep) {
		alloc_exit();
		return -ENOMEM;
	}
	capiminor_cachep = kmem_cache_create("capi20_minor",
					 sizeof(struct capiminor),
					 0,
					 SLAB_HWCACHE_ALIGN,
					 NULL, NULL);
	if (!capiminor_cachep) {
		alloc_exit();
		return -ENOMEM;
	}
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */
	return 0;
}

static void lower_callback(unsigned int cmd, __u32 contr, void *data)
{
	struct capi_ncciinfo *np;
	struct capidev *cdev;

	switch (cmd) {
	case KCI_CONTRUP:
		printk(KERN_INFO "capi: controller %hu up\n", contr);
		break;
	case KCI_CONTRDOWN:
		printk(KERN_INFO "capi: controller %hu down\n", contr);
		break;
	case KCI_NCCIUP:
		np = (struct capi_ncciinfo *)data;
		if ((cdev = capidev_find(np->applid)) == 0)
			return;
		(void)capincci_alloc(cdev, np->ncci);
		break;
	case KCI_NCCIDOWN:
		np = (struct capi_ncciinfo *)data;
		if ((cdev = capidev_find(np->applid)) == 0)
			return;
		(void)capincci_free(cdev, np->ncci);
		break;
	}
}

static struct capi_interface_user cuser = {
	name: "capi20",
	callback: lower_callback,
};

static char rev[10];

static int __init capi_init(void)
{
	char *p;

	MOD_INC_USE_COUNT;

	if ((p = strchr(revision, ':'))) {
		strcpy(rev, p + 2);
		p = strchr(rev, '$');
		*(p-1) = 0;
	} else
		strcpy(rev, "???");

	if (devfs_register_chrdev(capi_major, "capi20", &capi_fops)) {
		printk(KERN_ERR "capi20: unable to get major %d\n", capi_major);
		MOD_DEC_USE_COUNT;
		return -EIO;
	}

#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
	if (devfs_register_chrdev(capi_rawmajor, "capi/r%d", &capinc_raw_fops)) {
		devfs_unregister_chrdev(capi_major, "capi20");
		printk(KERN_ERR "capi20: unable to get major %d\n", capi_rawmajor);
		MOD_DEC_USE_COUNT;
		return -EIO;
	}
        devfs_register_series (NULL, "capi/r%u", CAPINC_NR_PORTS,
			      DEVFS_FL_DEFAULT,
                              capi_rawmajor, 0,
                              S_IFCHR | S_IRUSR | S_IWUSR,
                              &capinc_raw_fops, NULL);
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */
	devfs_register (NULL, "isdn/capi20", DEVFS_FL_DEFAULT,
			capi_major, 0, S_IFCHR | S_IRUSR | S_IWUSR,
			&capi_fops, NULL);
	printk(KERN_NOTICE "capi20: started up with major %d\n", capi_major);

	if ((capifuncs = attach_capi_interface(&cuser)) == 0) {

		MOD_DEC_USE_COUNT;
		devfs_unregister_chrdev(capi_major, "capi20");
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
		devfs_unregister_chrdev(capi_rawmajor, "capi/r%d");
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */
		devfs_unregister(devfs_find_handle(NULL, "capi20",
						   capi_major, 0,
						   DEVFS_SPECIAL_CHR, 0));
		return -EIO;
	}

#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
	if (capinc_tty_init() < 0) {
		(void) detach_capi_interface(&cuser);
		devfs_unregister_chrdev(capi_major, "capi20");
		devfs_unregister_chrdev(capi_rawmajor, "capi/r%d");
		MOD_DEC_USE_COUNT;
		return -ENOMEM;
	}
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */

	if (alloc_init() < 0) {
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
		unsigned int j;
		devfs_unregister_chrdev(capi_rawmajor, "capi/r%d");
		for (j = 0; j < CAPINC_NR_PORTS; j++) {
			char devname[32];
			sprintf(devname, "capi/r%u", j);
			devfs_unregister(devfs_find_handle(NULL, devname, capi_rawmajor, j, DEVFS_SPECIAL_CHR, 0));
		}
		capinc_tty_exit();
#endif /* CONFIG_ISDN_CAPI_MIDDLEWARE */
		(void) detach_capi_interface(&cuser);
		devfs_unregister_chrdev(capi_major, "capi20");
		devfs_unregister(devfs_find_handle(NULL, "capi20",
						   capi_major, 0,
						   DEVFS_SPECIAL_CHR, 0));
		MOD_DEC_USE_COUNT;
		return -ENOMEM;
	}

	(void)proc_init();

	printk(KERN_NOTICE "capi20: Rev%s: started up with major %d\n",
				rev, capi_major);

	MOD_DEC_USE_COUNT;
	return 0;
}

static void __exit capi_exit(void)
{
#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
	unsigned int j;
#endif
	alloc_exit();
	(void)proc_exit();

	devfs_unregister_chrdev(capi_major, "capi20");
	devfs_unregister(devfs_find_handle(NULL, "isdn/capi20", capi_major, 0, DEVFS_SPECIAL_CHR, 0));

#ifdef CONFIG_ISDN_CAPI_MIDDLEWARE
	capinc_tty_exit();
	devfs_unregister_chrdev(capi_rawmajor, "capi/r%d");
	for (j = 0; j < CAPINC_NR_PORTS; j++) {
		char devname[32];
		sprintf(devname, "capi/r%u", j);
		devfs_unregister(devfs_find_handle(NULL, devname, capi_rawmajor, j, DEVFS_SPECIAL_CHR, 0));
	}
#endif
	(void) detach_capi_interface(&cuser);
	printk(KERN_NOTICE "capi: Rev%s: unloaded\n", rev);
}

module_init(capi_init);
module_exit(capi_exit);
