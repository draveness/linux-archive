/* $Id: isdn_common.c,v 1.114 2000/11/25 17:00:59 kai Exp $

 * Linux ISDN subsystem, common used functions (linklevel).
 *
 * Copyright 1994-1999  by Fritz Elfert (fritz@isdn4linux.de)
 * Copyright 1995,96    Thinking Objects Software GmbH Wuerzburg
 * Copyright 1995,96    by Michael Hipp (Michael.Hipp@student.uni-tuebingen.de)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/poll.h>
#include <linux/vmalloc.h>
#include <linux/isdn.h>
#include <linux/smp_lock.h>
#include "isdn_common.h"
#include "isdn_tty.h"
#include "isdn_net.h"
#include "isdn_ppp.h"
#ifdef CONFIG_ISDN_AUDIO
#include "isdn_audio.h"
#endif
#ifdef CONFIG_ISDN_DIVERSION_MODULE
#define CONFIG_ISDN_DIVERSION
#endif
#ifdef CONFIG_ISDN_DIVERSION
#include <linux/isdn_divertif.h>
#endif CONFIG_ISDN_DIVERSION
#include "isdn_v110.h"
#include "isdn_cards.h"
#include <linux/devfs_fs_kernel.h>

/* Debugflags */
#undef ISDN_DEBUG_STATCALLB

isdn_dev *dev;

static char *isdn_revision = "$Revision: 1.114 $";

extern char *isdn_net_revision;
extern char *isdn_tty_revision;
#ifdef CONFIG_ISDN_PPP
extern char *isdn_ppp_revision;
#else
static char *isdn_ppp_revision = ": none $";
#endif
#ifdef CONFIG_ISDN_AUDIO
extern char *isdn_audio_revision;
#else
static char *isdn_audio_revision = ": none $";
#endif
extern char *isdn_v110_revision;

#ifdef CONFIG_ISDN_DIVERSION
static isdn_divert_if *divert_if; /* = NULL */
#endif CONFIG_ISDN_DIVERSION


static int isdn_writebuf_stub(int, int, const u_char *, int, int);
static void set_global_features(void);
static void isdn_register_devfs(int);
static void isdn_unregister_devfs(int);
static int isdn_wildmat(char *s, char *p);

void
isdn_lock_drivers(void)
{
	int i;

	for (i = 0; i < dev->drivers; i++) {
		isdn_ctrl cmd;

		cmd.driver = i;
		cmd.arg = 0;
		cmd.command = ISDN_CMD_LOCK;
		isdn_command(&cmd);
		dev->drv[i]->locks++;
	}
}

void
isdn_MOD_INC_USE_COUNT(void)
{
	MOD_INC_USE_COUNT;
	isdn_lock_drivers();
}

void
isdn_unlock_drivers(void)
{
	int i;

	for (i = 0; i < dev->drivers; i++)
		if (dev->drv[i]->locks > 0) {
			isdn_ctrl cmd;

			cmd.driver = i;
			cmd.arg = 0;
			cmd.command = ISDN_CMD_UNLOCK;
			isdn_command(&cmd);
			dev->drv[i]->locks--;
		}
}

void
isdn_MOD_DEC_USE_COUNT(void)
{
	MOD_DEC_USE_COUNT;
	isdn_unlock_drivers();
}

#if defined(ISDN_DEBUG_NET_DUMP) || defined(ISDN_DEBUG_MODEM_DUMP)
void
isdn_dumppkt(char *s, u_char * p, int len, int dumplen)
{
	int dumpc;

	printk(KERN_DEBUG "%s(%d) ", s, len);
	for (dumpc = 0; (dumpc < dumplen) && (len); len--, dumpc++)
		printk(" %02x", *p++);
	printk("\n");
}
#endif

/*
 * I picked the pattern-matching-functions from an old GNU-tar version (1.10)
 * It was originally written and put to PD by rs@mirror.TMC.COM (Rich Salz)
 */
static int
isdn_star(char *s, char *p)
{
	while (isdn_wildmat(s, p)) {
		if (*++s == '\0')
			return (2);
	}
	return (0);
}

/*
 * Shell-type Pattern-matching for incoming caller-Ids
 * This function gets a string in s and checks, if it matches the pattern
 * given in p.
 *
 * Return:
 *   0 = match.
 *   1 = no match.
 *   2 = no match. Would eventually match, if s would be longer.
 *
 * Possible Patterns:
 *
 * '?'     matches one character
 * '*'     matches zero or more characters
 * [xyz]   matches the set of characters in brackets.
 * [^xyz]  matches any single character not in the set of characters
 */

static int
isdn_wildmat(char *s, char *p)
{
	register int last;
	register int matched;
	register int reverse;
	register int nostar = 1;

	if (!(*s) && !(*p))
		return(1);
	for (; *p; s++, p++)
		switch (*p) {
			case '\\':
				/*
				 * Literal match with following character,
				 * fall through.
				 */
				p++;
			default:
				if (*s != *p)
					return (*s == '\0')?2:1;
				continue;
			case '?':
				/* Match anything. */
				if (*s == '\0')
					return (2);
				continue;
			case '*':
				nostar = 0;	
				/* Trailing star matches everything. */
				return (*++p ? isdn_star(s, p) : 0);
			case '[':
				/* [^....] means inverse character class. */
				if ((reverse = (p[1] == '^')))
					p++;
				for (last = 0, matched = 0; *++p && (*p != ']'); last = *p)
					/* This next line requires a good C compiler. */
					if (*p == '-' ? *s <= *++p && *s >= last : *s == *p)
						matched = 1;
				if (matched == reverse)
					return (1);
				continue;
		}
	return (*s == '\0')?0:nostar;
}

int isdn_msncmp( const char * msn1, const char * msn2 )
{
	char TmpMsn1[ ISDN_MSNLEN ];
	char TmpMsn2[ ISDN_MSNLEN ];
	char *p;

	for ( p = TmpMsn1; *msn1 && *msn1 != ':'; )  // Strip off a SPID
		*p++ = *msn1++;
	*p = '\0';

	for ( p = TmpMsn2; *msn2 && *msn2 != ':'; )  // Strip off a SPID
		*p++ = *msn2++;
	*p = '\0';

	return isdn_wildmat( TmpMsn1, TmpMsn2 );
}

static void
isdn_free_queue(struct sk_buff_head *queue)
{
	struct sk_buff *skb;
	unsigned long flags;

	save_flags(flags);
	cli();
	if (skb_queue_len(queue))
		while ((skb = skb_dequeue(queue)))
			dev_kfree_skb(skb);
	restore_flags(flags);
}

int
isdn_dc2minor(int di, int ch)
{
	int i;
	for (i = 0; i < ISDN_MAX_CHANNELS; i++)
		if (dev->chanmap[i] == ch && dev->drvmap[i] == di)
			return i;
	return -1;
}

static int isdn_timer_cnt1 = 0;
static int isdn_timer_cnt2 = 0;
static int isdn_timer_cnt3 = 0;
static int isdn_timer_cnt4 = 0;

static void
isdn_timer_funct(ulong dummy)
{
	int tf = dev->tflags;
	if (tf & ISDN_TIMER_FAST) {
		if (tf & ISDN_TIMER_MODEMREAD)
			isdn_tty_readmodem();
		if (tf & ISDN_TIMER_MODEMPLUS)
			isdn_tty_modem_escape();
		if (tf & ISDN_TIMER_MODEMXMIT)
			isdn_tty_modem_xmit();
	}
	if (tf & ISDN_TIMER_SLOW) {
		if (++isdn_timer_cnt1 >= ISDN_TIMER_02SEC) {
			isdn_timer_cnt1 = 0;
			if (tf & ISDN_TIMER_NETDIAL)
				isdn_net_dial();
		}
		if (++isdn_timer_cnt2 >= ISDN_TIMER_1SEC) {
			isdn_timer_cnt2 = 0;
			if (tf & ISDN_TIMER_NETHANGUP)
				isdn_net_autohup();
			if (++isdn_timer_cnt3 > ISDN_TIMER_RINGING) {
				isdn_timer_cnt3 = 0;
				if (tf & ISDN_TIMER_MODEMRING)
					isdn_tty_modem_ring();
			}
			if (++isdn_timer_cnt4 > ISDN_TIMER_KEEPINT) {
				isdn_timer_cnt4 = 0;
				if (tf & ISDN_TIMER_KEEPALIVE)
					isdn_net_slarp_out();
			}
			if (tf & ISDN_TIMER_CARRIER)
				isdn_tty_carrier_timeout();
		}
	}
	if (tf) 
	{
		int flags;

		save_flags(flags);
		cli();
		mod_timer(&dev->timer, jiffies+ISDN_TIMER_RES);
		restore_flags(flags);
	}
}

void
isdn_timer_ctrl(int tf, int onoff)
{
	int flags;

	save_flags(flags);
	cli();
	if ((tf & ISDN_TIMER_SLOW) && (!(dev->tflags & ISDN_TIMER_SLOW))) {
		/* If the slow-timer wasn't activated until now */
		isdn_timer_cnt1 = 0;
		isdn_timer_cnt2 = 0;
	}
	if (onoff)
		dev->tflags |= tf;
	else
		dev->tflags &= ~tf;
	if (dev->tflags)
		mod_timer(&dev->timer, jiffies+ISDN_TIMER_RES);
	restore_flags(flags);
}

/*
 * Receive a packet from B-Channel. (Called from low-level-module)
 */
static void
isdn_receive_skb_callback(int di, int channel, struct sk_buff *skb)
{
	int i;

	if ((i = isdn_dc2minor(di, channel)) == -1) {
		dev_kfree_skb(skb);
		return;
	}
	/* Update statistics */
	dev->ibytes[i] += skb->len;
	
	/* First, try to deliver data to network-device */
	if (isdn_net_rcv_skb(i, skb))
		return;

	/* V.110 handling
	 * makes sense for async streams only, so it is
	 * called after possible net-device delivery.
	 */
	if (dev->v110[i]) {
		atomic_inc(&dev->v110use[i]);
		skb = isdn_v110_decode(dev->v110[i], skb);
		atomic_dec(&dev->v110use[i]);
		if (!skb)
			return;
	}

	/* No network-device found, deliver to tty or raw-channel */
	if (skb->len) {
		if (isdn_tty_rcv_skb(i, di, channel, skb))
			return;
		wake_up_interruptible(&dev->drv[di]->rcv_waitq[channel]);
	} else
		dev_kfree_skb(skb);
}

/*
 * Intercept command from Linklevel to Lowlevel.
 * If layer 2 protocol is V.110 and this is not supported by current
 * lowlevel-driver, use driver's transparent mode and handle V.110 in
 * linklevel instead.
 */
int
isdn_command(isdn_ctrl *cmd)
{
	if (cmd->driver == -1) {
		printk(KERN_WARNING "isdn_command command(%x) driver -1\n", cmd->command);
		return(1);
	}
	if (cmd->command == ISDN_CMD_SETL2) {
		int idx = isdn_dc2minor(cmd->driver, cmd->arg & 255);
		unsigned long l2prot = (cmd->arg >> 8) & 255;
		unsigned long features = (dev->drv[cmd->driver]->interface->features
						>> ISDN_FEATURE_L2_SHIFT) &
						ISDN_FEATURE_L2_MASK;
		unsigned long l2_feature = (1 << l2prot);

		switch (l2prot) {
			case ISDN_PROTO_L2_V11096:
			case ISDN_PROTO_L2_V11019:
			case ISDN_PROTO_L2_V11038:
			/* If V.110 requested, but not supported by
			 * HL-driver, set emulator-flag and change
			 * Layer-2 to transparent
			 */
				if (!(features & l2_feature)) {
					dev->v110emu[idx] = l2prot;
					cmd->arg = (cmd->arg & 255) |
						(ISDN_PROTO_L2_TRANS << 8);
				} else
					dev->v110emu[idx] = 0;
		}
	}
	return dev->drv[cmd->driver]->interface->command(cmd);
}

void
isdn_all_eaz(int di, int ch)
{
	isdn_ctrl cmd;

	if (di < 0)
		return;
	cmd.driver = di;
	cmd.arg = ch;
	cmd.command = ISDN_CMD_SETEAZ;
	cmd.parm.num[0] = '\0';
	isdn_command(&cmd);
}

/*
 * Begin of a CAPI like LL<->HL interface, currently used only for 
 * supplementary service (CAPI 2.0 part III)
 */
#include "avmb1/capicmd.h"  /* this should be moved in a common place */

int
isdn_capi_rec_hl_msg(capi_msg *cm) {
	
	int di;
	int ch;
	
	di = (cm->adr.Controller & 0x7f) -1;
	ch = isdn_dc2minor(di, (cm->adr.Controller>>8)& 0x7f);
	switch(cm->Command) {
		case CAPI_FACILITY:
			/* in the moment only handled in tty */
			return(isdn_tty_capi_facility(cm));
		default:
			return(-1);
	}
}

static int
isdn_status_callback(isdn_ctrl * c)
{
	int di;
	ulong flags;
	int i;
	int r;
	int retval = 0;
	isdn_ctrl cmd;
	isdn_net_dev *p;

	di = c->driver;
	i = isdn_dc2minor(di, c->arg);
	switch (c->command) {
		case ISDN_STAT_BSENT:
			if (i < 0)
				return -1;
			if (dev->global_flags & ISDN_GLOBAL_STOPPED)
				return 0;
			if (isdn_net_stat_callback(i, c))
				return 0;
			if (isdn_v110_stat_callback(i, c))
				return 0;
			if (isdn_tty_stat_callback(i, c))
				return 0;
			wake_up_interruptible(&dev->drv[di]->snd_waitq[c->arg]);
			break;
		case ISDN_STAT_STAVAIL:
			save_flags(flags);
			cli();
			dev->drv[di]->stavail += c->arg;
			restore_flags(flags);
			wake_up_interruptible(&dev->drv[di]->st_waitq);
			break;
		case ISDN_STAT_RUN:
			dev->drv[di]->flags |= DRV_FLAG_RUNNING;
			for (i = 0; i < ISDN_MAX_CHANNELS; i++)
				if (dev->drvmap[i] == di)
					isdn_all_eaz(di, dev->chanmap[i]);
			set_global_features();
			break;
		case ISDN_STAT_STOP:
			dev->drv[di]->flags &= ~DRV_FLAG_RUNNING;
			break;
		case ISDN_STAT_ICALL:
			if (i < 0)
				return -1;
#ifdef ISDN_DEBUG_STATCALLB
			printk(KERN_DEBUG "ICALL (net): %d %ld %s\n", di, c->arg, c->parm.num);
#endif
			if (dev->global_flags & ISDN_GLOBAL_STOPPED) {
				cmd.driver = di;
				cmd.arg = c->arg;
				cmd.command = ISDN_CMD_HANGUP;
				isdn_command(&cmd);
				return 0;
			}
			/* Try to find a network-interface which will accept incoming call */
			r = ((c->command == ISDN_STAT_ICALLW) ? 0 : isdn_net_find_icall(di, c->arg, i, &c->parm.setup));
			switch (r) {
				case 0:
					/* No network-device replies.
					 * Try ttyI's.
					 * These return 0 on no match, 1 on match and
					 * 3 on eventually match, if CID is longer.
					 */
                                        if (c->command == ISDN_STAT_ICALL)
					  if ((retval = isdn_tty_find_icall(di, c->arg, &c->parm.setup))) return(retval);
#ifdef CONFIG_ISDN_DIVERSION 
                                         if (divert_if)
                 	                  if ((retval = divert_if->stat_callback(c))) 
					    return(retval); /* processed */
#endif CONFIG_ISDN_DIVERSION                        
					if ((!retval) && (dev->drv[di]->flags & DRV_FLAG_REJBUS)) {
						/* No tty responding */
						cmd.driver = di;
						cmd.arg = c->arg;
						cmd.command = ISDN_CMD_HANGUP;
						isdn_command(&cmd);
						retval = 2;
					}
					break;
				case 1:
					/* Schedule connection-setup */
					isdn_net_dial();
					cmd.driver = di;
					cmd.arg = c->arg;
					cmd.command = ISDN_CMD_ACCEPTD;
					for ( p = dev->netdev; p; p = p->next )
						if ( p->local->isdn_channel == cmd.arg )
						{
							strcpy( cmd.parm.setup.eazmsn, p->local->msn );
							isdn_command(&cmd);
							retval = 1;
							break;
						}
					break;

				case 2:	/* For calling back, first reject incoming call ... */
				case 3:	/* Interface found, but down, reject call actively  */
					retval = 2;
					printk(KERN_INFO "isdn: Rejecting Call\n");
					cmd.driver = di;
					cmd.arg = c->arg;
					cmd.command = ISDN_CMD_HANGUP;
					isdn_command(&cmd);
					if (r == 3)
						break;
					/* Fall through */
				case 4:
					/* ... then start callback. */
					isdn_net_dial();
					break;
				case 5:
					/* Number would eventually match, if longer */
					retval = 3;
					break;
			}
#ifdef ISDN_DEBUG_STATCALLB
			printk(KERN_DEBUG "ICALL: ret=%d\n", retval);
#endif
			return retval;
			break;
		case ISDN_STAT_CINF:
			if (i < 0)
				return -1;
#ifdef ISDN_DEBUG_STATCALLB
			printk(KERN_DEBUG "CINF: %ld %s\n", c->arg, c->parm.num);
#endif
			if (dev->global_flags & ISDN_GLOBAL_STOPPED)
				return 0;
			if (strcmp(c->parm.num, "0"))
				isdn_net_stat_callback(i, c);
			isdn_tty_stat_callback(i, c);
			break;
		case ISDN_STAT_CAUSE:
#ifdef ISDN_DEBUG_STATCALLB
			printk(KERN_DEBUG "CAUSE: %ld %s\n", c->arg, c->parm.num);
#endif
			printk(KERN_INFO "isdn: %s,ch%ld cause: %s\n",
			       dev->drvid[di], c->arg, c->parm.num);
			isdn_tty_stat_callback(i, c);
#ifdef CONFIG_ISDN_DIVERSION
                        if (divert_if)
                         divert_if->stat_callback(c); 
#endif CONFIG_ISDN_DIVERSION
			break;
		case ISDN_STAT_DISPLAY:
#ifdef ISDN_DEBUG_STATCALLB
			printk(KERN_DEBUG "DISPLAY: %ld %s\n", c->arg, c->parm.display);
#endif
			isdn_tty_stat_callback(i, c);
#ifdef CONFIG_ISDN_DIVERSION
                        if (divert_if)
                         divert_if->stat_callback(c); 
#endif CONFIG_ISDN_DIVERSION
			break;
		case ISDN_STAT_DCONN:
			if (i < 0)
				return -1;
#ifdef ISDN_DEBUG_STATCALLB
			printk(KERN_DEBUG "DCONN: %ld\n", c->arg);
#endif
			if (dev->global_flags & ISDN_GLOBAL_STOPPED)
				return 0;
			/* Find any net-device, waiting for D-channel setup */
			if (isdn_net_stat_callback(i, c))
				break;
			isdn_v110_stat_callback(i, c);
			/* Find any ttyI, waiting for D-channel setup */
			if (isdn_tty_stat_callback(i, c)) {
				cmd.driver = di;
				cmd.arg = c->arg;
				cmd.command = ISDN_CMD_ACCEPTB;
				isdn_command(&cmd);
				break;
			}
			break;
		case ISDN_STAT_DHUP:
			if (i < 0)
				return -1;
#ifdef ISDN_DEBUG_STATCALLB
			printk(KERN_DEBUG "DHUP: %ld\n", c->arg);
#endif
			if (dev->global_flags & ISDN_GLOBAL_STOPPED)
				return 0;
			dev->drv[di]->online &= ~(1 << (c->arg));
			isdn_info_update();
			/* Signal hangup to network-devices */
			if (isdn_net_stat_callback(i, c))
				break;
			isdn_v110_stat_callback(i, c);
			if (isdn_tty_stat_callback(i, c))
				break;
#ifdef CONFIG_ISDN_DIVERSION
                        if (divert_if)
                         divert_if->stat_callback(c); 
#endif CONFIG_ISDN_DIVERSION
			break;
			break;
		case ISDN_STAT_BCONN:
			if (i < 0)
				return -1;
#ifdef ISDN_DEBUG_STATCALLB
			printk(KERN_DEBUG "BCONN: %ld\n", c->arg);
#endif
			/* Signal B-channel-connect to network-devices */
			if (dev->global_flags & ISDN_GLOBAL_STOPPED)
				return 0;
			dev->drv[di]->online |= (1 << (c->arg));
			isdn_info_update();
			if (isdn_net_stat_callback(i, c))
				break;
			isdn_v110_stat_callback(i, c);
			if (isdn_tty_stat_callback(i, c))
				break;
			break;
		case ISDN_STAT_BHUP:
			if (i < 0)
				return -1;
#ifdef ISDN_DEBUG_STATCALLB
			printk(KERN_DEBUG "BHUP: %ld\n", c->arg);
#endif
			if (dev->global_flags & ISDN_GLOBAL_STOPPED)
				return 0;
			dev->drv[di]->online &= ~(1 << (c->arg));
			isdn_info_update();
#ifdef CONFIG_ISDN_X25
			/* Signal hangup to network-devices */
			if (isdn_net_stat_callback(i, c))
				break;
#endif
			isdn_v110_stat_callback(i, c);
			if (isdn_tty_stat_callback(i, c))
				break;
			break;
		case ISDN_STAT_NODCH:
			if (i < 0)
				return -1;
#ifdef ISDN_DEBUG_STATCALLB
			printk(KERN_DEBUG "NODCH: %ld\n", c->arg);
#endif
			if (dev->global_flags & ISDN_GLOBAL_STOPPED)
				return 0;
			if (isdn_net_stat_callback(i, c))
				break;
			if (isdn_tty_stat_callback(i, c))
				break;
			break;
		case ISDN_STAT_ADDCH:
			if (isdn_add_channels(dev->drv[di], di, c->arg, 1))
				return -1;
			isdn_info_update();
			break;
		case ISDN_STAT_DISCH:
			save_flags(flags);
			cli();
			for (i = 0; i < ISDN_MAX_CHANNELS; i++)
				if ((dev->drvmap[i] == di) &&
				    (dev->chanmap[i] == c->arg)) {
				    if (c->parm.num[0])
				      dev->usage[i] &= ~ISDN_USAGE_DISABLED;
				    else
				      if (USG_NONE(dev->usage[i])) {
					dev->usage[i] |= ISDN_USAGE_DISABLED;
				      }
				      else 
					retval = -1;
				    break;
				}
			restore_flags(flags);
			isdn_info_update();
			break;
		case ISDN_STAT_UNLOAD:
			while (dev->drv[di]->locks > 0) {
				isdn_ctrl cmd;
				cmd.driver = di;
				cmd.arg = 0;
				cmd.command = ISDN_CMD_UNLOCK;
				isdn_command(&cmd);
				dev->drv[di]->locks--;
			}
			save_flags(flags);
			cli();
			isdn_tty_stat_callback(i, c);
			for (i = 0; i < ISDN_MAX_CHANNELS; i++)
				if (dev->drvmap[i] == di) {
					dev->drvmap[i] = -1;
					dev->chanmap[i] = -1;
					dev->usage[i] &= ~ISDN_USAGE_DISABLED;
					isdn_unregister_devfs(i);
				}
			dev->drivers--;
			dev->channels -= dev->drv[di]->channels;
			kfree(dev->drv[di]->rcverr);
			kfree(dev->drv[di]->rcvcount);
			for (i = 0; i < dev->drv[di]->channels; i++)
				isdn_free_queue(&dev->drv[di]->rpqueue[i]);
			kfree(dev->drv[di]->rpqueue);
			kfree(dev->drv[di]->rcv_waitq);
			kfree(dev->drv[di]);
			dev->drv[di] = NULL;
			dev->drvid[di][0] = '\0';
			isdn_info_update();
			set_global_features();
			restore_flags(flags);
			return 0;
		case ISDN_STAT_L1ERR:
			break;
		case CAPI_PUT_MESSAGE:
			return(isdn_capi_rec_hl_msg(&c->parm.cmsg));
#ifdef CONFIG_ISDN_TTY_FAX
		case ISDN_STAT_FAXIND:
			isdn_tty_stat_callback(i, c);
			break;
#endif
#ifdef CONFIG_ISDN_AUDIO
		case ISDN_STAT_AUDIO:
			isdn_tty_stat_callback(i, c);
			break;
#endif
#ifdef CONFIG_ISDN_DIVERSION
	        case ISDN_STAT_PROT:
	        case ISDN_STAT_REDIR:
                        if (divert_if)
                          return(divert_if->stat_callback(c));
#endif CONFIG_ISDN_DIVERSION
		default:
			return -1;
	}
	return 0;
}

/*
 * Get integer from char-pointer, set pointer to end of number
 */
int
isdn_getnum(char **p)
{
	int v = -1;

	while (*p[0] >= '0' && *p[0] <= '9')
		v = ((v < 0) ? 0 : (v * 10)) + (int) ((*p[0]++) - '0');
	return v;
}

#define DLE 0x10

/*
 * isdn_readbchan() tries to get data from the read-queue.
 * It MUST be called with interrupts off.
 *
 * Be aware that this is not an atomic operation when sleep != 0, even though 
 * interrupts are turned off! Well, like that we are currently only called
 * on behalf of a read system call on raw device files (which are documented
 * to be dangerous and for for debugging purpose only). The inode semaphore
 * takes care that this is not called for the same minor device number while
 * we are sleeping, but access is not serialized against simultaneous read()
 * from the corresponding ttyI device. Can other ugly events, like changes
 * of the mapping (di,ch)<->minor, happen during the sleep? --he 
 */
int
isdn_readbchan(int di, int channel, u_char * buf, u_char * fp, int len, wait_queue_head_t *sleep)
{
	int left;
	int count;
	int count_pull;
	int count_put;
	int dflag;
	struct sk_buff *skb;
	u_char *cp;

	if (!dev->drv[di])
		return 0;
	if (skb_queue_empty(&dev->drv[di]->rpqueue[channel])) {
		if (sleep)
			interruptible_sleep_on(sleep);
		else
			return 0;
	}
	left = MIN(len, dev->drv[di]->rcvcount[channel]);
	cp = buf;
	count = 0;
	while (left) {
		if (!(skb = skb_peek(&dev->drv[di]->rpqueue[channel])))
			break;
#ifdef CONFIG_ISDN_AUDIO
		if (ISDN_AUDIO_SKB_LOCK(skb))
			break;
		ISDN_AUDIO_SKB_LOCK(skb) = 1;
		if ((ISDN_AUDIO_SKB_DLECOUNT(skb)) || (dev->drv[di]->DLEflag & (1 << channel))) {
			char *p = skb->data;
			unsigned long DLEmask = (1 << channel);

			dflag = 0;
			count_pull = count_put = 0;
			while ((count_pull < skb->len) && (left > 0)) {
				left--;
				if (dev->drv[di]->DLEflag & DLEmask) {
					*cp++ = DLE;
					dev->drv[di]->DLEflag &= ~DLEmask;
				} else {
					*cp++ = *p;
					if (*p == DLE) {
						dev->drv[di]->DLEflag |= DLEmask;
						(ISDN_AUDIO_SKB_DLECOUNT(skb))--;
					}
					p++;
					count_pull++;
				}
				count_put++;
			}
			if (count_pull >= skb->len)
				dflag = 1;
		} else {
#endif
			/* No DLE's in buff, so simply copy it */
			dflag = 1;
			if ((count_pull = skb->len) > left) {
				count_pull = left;
				dflag = 0;
			}
			count_put = count_pull;
			memcpy(cp, skb->data, count_put);
			cp += count_put;
			left -= count_put;
#ifdef CONFIG_ISDN_AUDIO
		}
#endif
		count += count_put;
		if (fp) {
			memset(fp, 0, count_put);
			fp += count_put;
		}
		if (dflag) {
			/* We got all the data in this buff.
			 * Now we can dequeue it.
			 */
			if (fp)
				*(fp - 1) = 0xff;
#ifdef CONFIG_ISDN_AUDIO
			ISDN_AUDIO_SKB_LOCK(skb) = 0;
#endif
			skb = skb_dequeue(&dev->drv[di]->rpqueue[channel]);
			dev_kfree_skb(skb);
		} else {
			/* Not yet emptied this buff, so it
			 * must stay in the queue, for further calls
			 * but we pull off the data we got until now.
			 */
			skb_pull(skb, count_pull);
#ifdef CONFIG_ISDN_AUDIO
			ISDN_AUDIO_SKB_LOCK(skb) = 0;
#endif
		}
		dev->drv[di]->rcvcount[channel] -= count_put;
	}
	return count;
}

static __inline int
isdn_minor2drv(int minor)
{
	return (dev->drvmap[minor]);
}

static __inline int
isdn_minor2chan(int minor)
{
	return (dev->chanmap[minor]);
}

static char *
isdn_statstr(void)
{
	static char istatbuf[2048];
	char *p;
	int i;

	sprintf(istatbuf, "idmap:\t");
	p = istatbuf + strlen(istatbuf);
	for (i = 0; i < ISDN_MAX_CHANNELS; i++) {
		sprintf(p, "%s ", (dev->drvmap[i] < 0) ? "-" : dev->drvid[dev->drvmap[i]]);
		p = istatbuf + strlen(istatbuf);
	}
	sprintf(p, "\nchmap:\t");
	p = istatbuf + strlen(istatbuf);
	for (i = 0; i < ISDN_MAX_CHANNELS; i++) {
		sprintf(p, "%d ", dev->chanmap[i]);
		p = istatbuf + strlen(istatbuf);
	}
	sprintf(p, "\ndrmap:\t");
	p = istatbuf + strlen(istatbuf);
	for (i = 0; i < ISDN_MAX_CHANNELS; i++) {
		sprintf(p, "%d ", dev->drvmap[i]);
		p = istatbuf + strlen(istatbuf);
	}
	sprintf(p, "\nusage:\t");
	p = istatbuf + strlen(istatbuf);
	for (i = 0; i < ISDN_MAX_CHANNELS; i++) {
		sprintf(p, "%d ", dev->usage[i]);
		p = istatbuf + strlen(istatbuf);
	}
	sprintf(p, "\nflags:\t");
	p = istatbuf + strlen(istatbuf);
	for (i = 0; i < ISDN_MAX_DRIVERS; i++) {
		if (dev->drv[i]) {
			sprintf(p, "%ld ", dev->drv[i]->online);
			p = istatbuf + strlen(istatbuf);
		} else {
			sprintf(p, "? ");
			p = istatbuf + strlen(istatbuf);
		}
	}
	sprintf(p, "\nphone:\t");
	p = istatbuf + strlen(istatbuf);
	for (i = 0; i < ISDN_MAX_CHANNELS; i++) {
		sprintf(p, "%s ", dev->num[i]);
		p = istatbuf + strlen(istatbuf);
	}
	sprintf(p, "\n");
	return istatbuf;
}

/* Module interface-code */

void
isdn_info_update(void)
{
	infostruct *p = dev->infochain;

	while (p) {
		*(p->private) = 1;
		p = (infostruct *) p->next;
	}
	wake_up_interruptible(&(dev->info_waitq));
}

static ssize_t
isdn_read(struct file *file, char *buf, size_t count, loff_t * off)
{
	uint minor = MINOR(file->f_dentry->d_inode->i_rdev);
	int len = 0;
	ulong flags;
	int drvidx;
	int chidx;
	int retval;
	char *p;

	if (off != &file->f_pos)
		return -ESPIPE;

	lock_kernel();
	if (minor == ISDN_MINOR_STATUS) {
		if (!file->private_data) {
			if (file->f_flags & O_NONBLOCK) {
				retval = -EAGAIN;
				goto out;
			}
			interruptible_sleep_on(&(dev->info_waitq));
		}
		p = isdn_statstr();
		file->private_data = 0;
		if ((len = strlen(p)) <= count) {
			if (copy_to_user(buf, p, len)) {
				retval = -EFAULT;
				goto out;
			}
			*off += len;
			retval = len;
			goto out;
		}
		retval = 0;
		goto out;
	}
	if (!dev->drivers) {
		retval = -ENODEV;
		goto out;
	}
	if (minor < ISDN_MINOR_CTRL) {
		printk(KERN_WARNING "isdn_read minor %d obsolete!\n", minor);
		drvidx = isdn_minor2drv(minor);
		if (drvidx < 0) {
			retval = -ENODEV;
			goto out;
		}
		if (!(dev->drv[drvidx]->flags & DRV_FLAG_RUNNING)) {
			retval = -ENODEV;
			goto out;
		}
		chidx = isdn_minor2chan(minor);
		if (!(p = kmalloc(count, GFP_KERNEL))) {
			retval = -ENOMEM;
			goto out;
		}
		save_flags(flags);
		cli();
		len = isdn_readbchan(drvidx, chidx, p, 0, count,
				     &dev->drv[drvidx]->rcv_waitq[chidx]);
		*off += len;
		restore_flags(flags);
		if (copy_to_user(buf,p,len)) 
			len = -EFAULT;
		kfree(p);
		retval = len;
		goto out;
	}
	if (minor <= ISDN_MINOR_CTRLMAX) {
		drvidx = isdn_minor2drv(minor - ISDN_MINOR_CTRL);
		if (drvidx < 0) {
			retval = -ENODEV;
			goto out;
		}
		if (!dev->drv[drvidx]->stavail) {
			if (file->f_flags & O_NONBLOCK) {
				retval = -EAGAIN;
				goto out;
			}
			interruptible_sleep_on(&(dev->drv[drvidx]->st_waitq));
		}
		if (dev->drv[drvidx]->interface->readstat)
			len = dev->drv[drvidx]->interface->
			    readstat(buf, MIN(count, dev->drv[drvidx]->stavail),
				     1, drvidx, isdn_minor2chan(minor));
		else
			len = 0;
		save_flags(flags);
		cli();
		if (len)
			dev->drv[drvidx]->stavail -= len;
		else
			dev->drv[drvidx]->stavail = 0;
		restore_flags(flags);
		*off += len;
		retval = len;
		goto out;
	}
#ifdef CONFIG_ISDN_PPP
	if (minor <= ISDN_MINOR_PPPMAX) {
		retval = isdn_ppp_read(minor - ISDN_MINOR_PPP, file, buf, count);
		goto out;
	}
#endif
	retval = -ENODEV;
 out:
	unlock_kernel();
	return retval;
}

static loff_t
isdn_llseek(struct file *file, loff_t offset, int orig)
{
	return -ESPIPE;
}

static ssize_t
isdn_write(struct file *file, const char *buf, size_t count, loff_t * off)
{
	uint minor = MINOR(file->f_dentry->d_inode->i_rdev);
	int drvidx;
	int chidx;
	int retval;

	if (off != &file->f_pos)
		return -ESPIPE;

	if (minor == ISDN_MINOR_STATUS)
		return -EPERM;
	if (!dev->drivers)
		return -ENODEV;

	lock_kernel();
	if (minor < ISDN_MINOR_CTRL) {
		printk(KERN_WARNING "isdn_write minor %d obsolete!\n", minor);
		drvidx = isdn_minor2drv(minor);
		if (drvidx < 0) {
			retval = -ENODEV;
			goto out;
		}
		if (!(dev->drv[drvidx]->flags & DRV_FLAG_RUNNING)) {
			retval = -ENODEV;
			goto out;
		}
		chidx = isdn_minor2chan(minor);
		while (isdn_writebuf_stub(drvidx, chidx, buf, count, 1) != count)
			interruptible_sleep_on(&dev->drv[drvidx]->snd_waitq[chidx]);
		retval = count;
		goto out;
	}
	if (minor <= ISDN_MINOR_CTRLMAX) {
		drvidx = isdn_minor2drv(minor - ISDN_MINOR_CTRL);
		if (drvidx < 0) {
			retval = -ENODEV;
			goto out;
		}
		/*
		 * We want to use the isdnctrl device to load the firmware
		 *
		 if (!(dev->drv[drvidx]->flags & DRV_FLAG_RUNNING))
		 return -ENODEV;
		 */
		if (dev->drv[drvidx]->interface->writecmd)
			retval = dev->drv[drvidx]->interface->
				writecmd(buf, count, 1, drvidx, isdn_minor2chan(minor));
		else
			retval = count;
		goto out;
	}
#ifdef CONFIG_ISDN_PPP
	if (minor <= ISDN_MINOR_PPPMAX) {
		retval = isdn_ppp_write(minor - ISDN_MINOR_PPP, file, buf, count);
		goto out;
	}
#endif
	retval = -ENODEV;
 out:
	unlock_kernel();
	return retval;
}

static unsigned int
isdn_poll(struct file *file, poll_table * wait)
{
	unsigned int mask = 0;
	unsigned int minor = MINOR(file->f_dentry->d_inode->i_rdev);
	int drvidx = isdn_minor2drv(minor - ISDN_MINOR_CTRL);

	lock_kernel();
	if (minor == ISDN_MINOR_STATUS) {
		poll_wait(file, &(dev->info_waitq), wait);
		/* mask = POLLOUT | POLLWRNORM; */
		if (file->private_data) {
			mask |= POLLIN | POLLRDNORM;
		}
		goto out;
	}
	if (minor >= ISDN_MINOR_CTRL && minor <= ISDN_MINOR_CTRLMAX) {
		if (drvidx < 0) {
			/* driver deregistered while file open */
			mask = POLLHUP;
			goto out;
		}
		poll_wait(file, &(dev->drv[drvidx]->st_waitq), wait);
		mask = POLLOUT | POLLWRNORM;
		if (dev->drv[drvidx]->stavail) {
			mask |= POLLIN | POLLRDNORM;
		}
		goto out;
	}
#ifdef CONFIG_ISDN_PPP
	if (minor <= ISDN_MINOR_PPPMAX) {
		mask = isdn_ppp_poll(file, wait);
		goto out;
	}
#endif
	mask = POLLERR;
 out:
	unlock_kernel();
	return mask;
}


static int
isdn_ioctl(struct inode *inode, struct file *file, uint cmd, ulong arg)
{
	uint minor = MINOR(inode->i_rdev);
	isdn_ctrl c;
	int drvidx;
	int chidx;
	int ret;
	int i;
	char *p;
	char *s;
	union iocpar {
		char name[10];
		char bname[22];
		isdn_ioctl_struct iocts;
		isdn_net_ioctl_phone phone;
		isdn_net_ioctl_cfg cfg;
	} iocpar;

#define name  iocpar.name
#define bname iocpar.bname
#define iocts iocpar.iocts
#define phone iocpar.phone
#define cfg   iocpar.cfg

	if (minor == ISDN_MINOR_STATUS) {
		switch (cmd) {
			case IIOCGETDVR:
				return (TTY_DV +
					(NET_DV << 8) +
					(INF_DV << 16));
			case IIOCGETCPS:
				if (arg) {
					ulong *p = (ulong *) arg;
					int i;
					if ((ret = verify_area(VERIFY_WRITE, (void *) arg,
							       sizeof(ulong) * ISDN_MAX_CHANNELS * 2)))
						return ret;
					for (i = 0; i < ISDN_MAX_CHANNELS; i++) {
						put_user(dev->ibytes[i], p++);
						put_user(dev->obytes[i], p++);
					}
					return 0;
				} else
					return -EINVAL;
				break;
#ifdef CONFIG_NETDEVICES
			case IIOCNETGPN:
				/* Get peer phone number of a connected 
				 * isdn network interface */
				if (arg) {
					if (copy_from_user((char *) &phone, (char *) arg, sizeof(phone)))
						return -EFAULT;
					return isdn_net_getpeer(&phone, (isdn_net_ioctl_phone *) arg);
				} else
					return -EINVAL;
#endif
			default:
				return -EINVAL;
		}
	}
	if (!dev->drivers)
		return -ENODEV;
	if (minor < ISDN_MINOR_CTRL) {
		drvidx = isdn_minor2drv(minor);
		if (drvidx < 0)
			return -ENODEV;
		chidx = isdn_minor2chan(minor);
		if (!(dev->drv[drvidx]->flags & DRV_FLAG_RUNNING))
			return -ENODEV;
		return 0;
	}
	if (minor <= ISDN_MINOR_CTRLMAX) {
/*
 * isdn net devices manage lots of configuration variables as linked lists.
 * Those lists must only be manipulated from user space. Some of the ioctl's
 * service routines access user space and are not atomic. Therefor, ioctl's
 * manipulating the lists and ioctl's sleeping while accessing the lists
 * are serialized by means of a semaphore.
 */
		switch (cmd) {
			case IIOCNETDWRSET:
				printk(KERN_INFO "INFO: ISDN_DW_ABC_EXTENSION not enabled\n");
				return(-EINVAL);
			case IIOCNETLCR:
				printk(KERN_INFO "INFO: ISDN_ABC_LCR_SUPPORT not enabled\n");
				return -ENODEV;
#ifdef CONFIG_NETDEVICES
			case IIOCNETAIF:
				/* Add a network-interface */
				if (arg) {
					if (copy_from_user(name, (char *) arg, sizeof(name)))
						return -EFAULT;
					s = name;
				} else {
					s = NULL;
				}
				ret = down_interruptible(&dev->sem);
				if( ret ) return ret;
				if ((s = isdn_net_new(s, NULL))) {
					if (copy_to_user((char *) arg, s, strlen(s) + 1)){
						ret = -EFAULT;
					} else {
						ret = 0;
					}
				} else
					ret = -ENODEV;
				up(&dev->sem);
				return ret;
			case IIOCNETASL:
				/* Add a slave to a network-interface */
				if (arg) {
					if (copy_from_user(bname, (char *) arg, sizeof(bname) - 1))
						return -EFAULT;
				} else
					return -EINVAL;
				ret = down_interruptible(&dev->sem);
				if( ret ) return ret;
				if ((s = isdn_net_newslave(bname))) {
					if (copy_to_user((char *) arg, s, strlen(s) + 1)){
						ret = -EFAULT;
					} else {
						ret = 0;
					}
				} else
					ret = -ENODEV;
				up(&dev->sem);
				return ret;
			case IIOCNETDIF:
				/* Delete a network-interface */
				if (arg) {
					if (copy_from_user(name, (char *) arg, sizeof(name)))
						return -EFAULT;
					ret = down_interruptible(&dev->sem);
					if( ret ) return ret;
					ret = isdn_net_rm(name);
					up(&dev->sem);
					return ret;
				} else
					return -EINVAL;
			case IIOCNETSCF:
				/* Set configurable parameters of a network-interface */
				if (arg) {
					if (copy_from_user((char *) &cfg, (char *) arg, sizeof(cfg)))
						return -EFAULT;
					return isdn_net_setcfg(&cfg);
				} else
					return -EINVAL;
			case IIOCNETGCF:
				/* Get configurable parameters of a network-interface */
				if (arg) {
					if (copy_from_user((char *) &cfg, (char *) arg, sizeof(cfg)))
						return -EFAULT;
					if (!(ret = isdn_net_getcfg(&cfg))) {
						if (copy_to_user((char *) arg, (char *) &cfg, sizeof(cfg)))
							return -EFAULT;
					}
					return ret;
				} else
					return -EINVAL;
			case IIOCNETANM:
				/* Add a phone-number to a network-interface */
				if (arg) {
					if (copy_from_user((char *) &phone, (char *) arg, sizeof(phone)))
						return -EFAULT;
					ret = down_interruptible(&dev->sem);
					if( ret ) return ret;
					ret = isdn_net_addphone(&phone);
					up(&dev->sem);
					return ret;
				} else
					return -EINVAL;
			case IIOCNETGNM:
				/* Get list of phone-numbers of a network-interface */
				if (arg) {
					if (copy_from_user((char *) &phone, (char *) arg, sizeof(phone)))
						return -EFAULT;
					ret = down_interruptible(&dev->sem);
					if( ret ) return ret;
					ret = isdn_net_getphones(&phone, (char *) arg);
					up(&dev->sem);
					return ret;
				} else
					return -EINVAL;
			case IIOCNETDNM:
				/* Delete a phone-number of a network-interface */
				if (arg) {
					if (copy_from_user((char *) &phone, (char *) arg, sizeof(phone)))
						return -EFAULT;
					ret = down_interruptible(&dev->sem);
					if( ret ) return ret;
					ret = isdn_net_delphone(&phone);
					up(&dev->sem);
					return ret;
				} else
					return -EINVAL;
			case IIOCNETDIL:
				/* Force dialing of a network-interface */
				if (arg) {
					if (copy_from_user(name, (char *) arg, sizeof(name)))
						return -EFAULT;
					return isdn_net_force_dial(name);
				} else
					return -EINVAL;
#ifdef CONFIG_ISDN_PPP
			case IIOCNETALN:
				if (!arg)
					return -EINVAL;
				if (copy_from_user(name, (char *) arg, sizeof(name)))
					return -EFAULT;
				return isdn_ppp_dial_slave(name);
			case IIOCNETDLN:
				if (!arg)
					return -EINVAL;
				if (copy_from_user(name, (char *) arg, sizeof(name)))
					return -EFAULT;
				return isdn_ppp_hangup_slave(name);
#endif
			case IIOCNETHUP:
				/* Force hangup of a network-interface */
				if (!arg)
					return -EINVAL;
				if (copy_from_user(name, (char *) arg, sizeof(name)))
					return -EFAULT;
				return isdn_net_force_hangup(name);
				break;
#endif                          /* CONFIG_NETDEVICES */
			case IIOCSETVER:
				dev->net_verbose = arg;
				printk(KERN_INFO "isdn: Verbose-Level is %d\n", dev->net_verbose);
				return 0;
			case IIOCSETGST:
				if (arg)
					dev->global_flags |= ISDN_GLOBAL_STOPPED;
				else
					dev->global_flags &= ~ISDN_GLOBAL_STOPPED;
				printk(KERN_INFO "isdn: Global Mode %s\n",
				       (dev->global_flags & ISDN_GLOBAL_STOPPED) ? "stopped" : "running");
				return 0;
			case IIOCSETBRJ:
				drvidx = -1;
				if (arg) {
					int i;
					char *p;
					if (copy_from_user((char *) &iocts, (char *) arg,
					     sizeof(isdn_ioctl_struct)))
						return -EFAULT;
					if (strlen(iocts.drvid)) {
						if ((p = strchr(iocts.drvid, ',')))
							*p = 0;
						drvidx = -1;
						for (i = 0; i < ISDN_MAX_DRIVERS; i++)
							if (!(strcmp(dev->drvid[i], iocts.drvid))) {
								drvidx = i;
								break;
							}
					}
				}
				if (drvidx == -1)
					return -ENODEV;
				if (iocts.arg)
					dev->drv[drvidx]->flags |= DRV_FLAG_REJBUS;
				else
					dev->drv[drvidx]->flags &= ~DRV_FLAG_REJBUS;
				return 0;
			case IIOCSIGPRF:
				dev->profd = current;
				return 0;
				break;
			case IIOCGETPRF:
				/* Get all Modem-Profiles */
				if (arg) {
					char *p = (char *) arg;
					int i;

					if ((ret = verify_area(VERIFY_WRITE, (void *) arg,
					(ISDN_MODEM_NUMREG + ISDN_MSNLEN + ISDN_LMSNLEN)
						   * ISDN_MAX_CHANNELS)))
						return ret;

					for (i = 0; i < ISDN_MAX_CHANNELS; i++) {
						if (copy_to_user(p, dev->mdm.info[i].emu.profile,
						      ISDN_MODEM_NUMREG))
							return -EFAULT;
						p += ISDN_MODEM_NUMREG;
						if (copy_to_user(p, dev->mdm.info[i].emu.pmsn, ISDN_MSNLEN))
							return -EFAULT;
						p += ISDN_MSNLEN;
						if (copy_to_user(p, dev->mdm.info[i].emu.plmsn, ISDN_LMSNLEN))
							return -EFAULT;
						p += ISDN_LMSNLEN;
					}
					return (ISDN_MODEM_NUMREG + ISDN_MSNLEN + ISDN_LMSNLEN) * ISDN_MAX_CHANNELS;
				} else
					return -EINVAL;
				break;
			case IIOCSETPRF:
				/* Set all Modem-Profiles */
				if (arg) {
					char *p = (char *) arg;
					int i;

					if ((ret = verify_area(VERIFY_READ, (void *) arg,
					(ISDN_MODEM_NUMREG + ISDN_MSNLEN)
						   * ISDN_MAX_CHANNELS)))
						return ret;

					for (i = 0; i < ISDN_MAX_CHANNELS; i++) {
						if (copy_from_user(dev->mdm.info[i].emu.profile, p,
						     ISDN_MODEM_NUMREG))
							return -EFAULT;
						p += ISDN_MODEM_NUMREG;
						if (copy_from_user(dev->mdm.info[i].emu.pmsn, p, ISDN_MSNLEN))
							return -EFAULT;
						p += ISDN_MSNLEN;
					}
					return 0;
				} else
					return -EINVAL;
				break;
			case IIOCSETMAP:
			case IIOCGETMAP:
				/* Set/Get MSN->EAZ-Mapping for a driver */
				if (arg) {

					if (copy_from_user((char *) &iocts,
							    (char *) arg,
					     sizeof(isdn_ioctl_struct)))
						return -EFAULT;
					if (strlen(iocts.drvid)) {
						drvidx = -1;
						for (i = 0; i < ISDN_MAX_DRIVERS; i++)
							if (!(strcmp(dev->drvid[i], iocts.drvid))) {
								drvidx = i;
								break;
							}
					} else
						drvidx = 0;
					if (drvidx == -1)
						return -ENODEV;
					if (cmd == IIOCSETMAP) {
						int loop = 1;

						p = (char *) iocts.arg;
						i = 0;
						while (loop) {
							int j = 0;

							while (1) {
								if ((ret = verify_area(VERIFY_READ, p, 1)))
									return ret;
								get_user(bname[j], p++);
								switch (bname[j]) {
									case '\0':
										loop = 0;
										/* Fall through */
									case ',':
										bname[j] = '\0';
										strcpy(dev->drv[drvidx]->msn2eaz[i], bname);
										j = ISDN_MSNLEN;
										break;
									default:
										j++;
								}
								if (j >= ISDN_MSNLEN)
									break;
							}
							if (++i > 9)
								break;
						}
					} else {
						p = (char *) iocts.arg;
						for (i = 0; i < 10; i++) {
							sprintf(bname, "%s%s",
								strlen(dev->drv[drvidx]->msn2eaz[i]) ?
								dev->drv[drvidx]->msn2eaz[i] : "_",
								(i < 9) ? "," : "\0");
							if (copy_to_user(p, bname, strlen(bname) + 1))
								return -EFAULT;
							p += strlen(bname);
						}
					}
					return 0;
				} else
					return -EINVAL;
			case IIOCDBGVAR:
				if (arg) {
					if (copy_to_user((char *) arg, (char *) &dev, sizeof(ulong)))
						return -EFAULT;
					return 0;
				} else
					return -EINVAL;
				break;
			default:
				if ((cmd & IIOCDRVCTL) == IIOCDRVCTL)
					cmd = ((cmd >> _IOC_NRSHIFT) & _IOC_NRMASK) & ISDN_DRVIOCTL_MASK;
				else
					return -EINVAL;
				if (arg) {
					int i;
					char *p;
					if (copy_from_user((char *) &iocts, (char *) arg, sizeof(isdn_ioctl_struct)))
						return -EFAULT;
					if (strlen(iocts.drvid)) {
						if ((p = strchr(iocts.drvid, ',')))
							*p = 0;
						drvidx = -1;
						for (i = 0; i < ISDN_MAX_DRIVERS; i++)
							if (!(strcmp(dev->drvid[i], iocts.drvid))) {
								drvidx = i;
								break;
							}
					} else
						drvidx = 0;
					if (drvidx == -1)
						return -ENODEV;
					if ((ret = verify_area(VERIFY_WRITE, (void *) arg,
					     sizeof(isdn_ioctl_struct))))
						return ret;
					c.driver = drvidx;
					c.command = ISDN_CMD_IOCTL;
					c.arg = cmd;
					memcpy(c.parm.num, (char *) &iocts.arg, sizeof(ulong));
					ret = isdn_command(&c);
					memcpy((char *) &iocts.arg, c.parm.num, sizeof(ulong));
					if (copy_to_user((char *) arg, &iocts, sizeof(isdn_ioctl_struct)))
						return -EFAULT;
					return ret;
				} else
					return -EINVAL;
		}
	}
#ifdef CONFIG_ISDN_PPP
	if (minor <= ISDN_MINOR_PPPMAX)
		return (isdn_ppp_ioctl(minor - ISDN_MINOR_PPP, file, cmd, arg));
#endif
	return -ENODEV;

#undef name
#undef bname
#undef iocts
#undef phone
#undef cfg
}

/*
 * Open the device code.
 */
static int
isdn_open(struct inode *ino, struct file *filep)
{
	uint minor = MINOR(ino->i_rdev);
	int drvidx;
	int chidx;
	int retval = -ENODEV;


	if (minor == ISDN_MINOR_STATUS) {
		infostruct *p;

		if ((p = (infostruct *) kmalloc(sizeof(infostruct), GFP_KERNEL))) {
			p->next = (char *) dev->infochain;
			p->private = (char *) &(filep->private_data);
			dev->infochain = p;
			/* At opening we allow a single update */
			filep->private_data = (char *) 1;
			retval = 0;
			goto out;
		} else {
			retval = -ENOMEM;
			goto out;
		}
	}
	if (!dev->channels)
		goto out;
	if (minor < ISDN_MINOR_CTRL) {
		printk(KERN_WARNING "isdn_open minor %d obsolete!\n", minor);
		drvidx = isdn_minor2drv(minor);
		if (drvidx < 0)
			goto out;
		chidx = isdn_minor2chan(minor);
		if (!(dev->drv[drvidx]->flags & DRV_FLAG_RUNNING))
			goto out;
		if (!(dev->drv[drvidx]->online & (1 << chidx)))
			goto out;
		isdn_lock_drivers();
		retval = 0;
		goto out;
	}
	if (minor <= ISDN_MINOR_CTRLMAX) {
		drvidx = isdn_minor2drv(minor - ISDN_MINOR_CTRL);
		if (drvidx < 0)
			goto out;
		isdn_lock_drivers();
		retval = 0;
		goto out;
	}
#ifdef CONFIG_ISDN_PPP
	if (minor <= ISDN_MINOR_PPPMAX) {
		retval = isdn_ppp_open(minor - ISDN_MINOR_PPP, filep);
		if (retval == 0)
			isdn_lock_drivers();
		goto out;
	}
#endif
 out:
	return retval;
}

static int
isdn_close(struct inode *ino, struct file *filep)
{
	uint minor = MINOR(ino->i_rdev);

	lock_kernel();
	if (minor == ISDN_MINOR_STATUS) {
		infostruct *p = dev->infochain;
		infostruct *q = NULL;

		while (p) {
			if (p->private == (char *) &(filep->private_data)) {
				if (q)
					q->next = p->next;
				else
					dev->infochain = (infostruct *) (p->next);
				kfree(p);
				goto out;
			}
			q = p;
			p = (infostruct *) (p->next);
		}
		printk(KERN_WARNING "isdn: No private data while closing isdnctrl\n");
		goto out;
	}
	isdn_unlock_drivers();
	if (minor < ISDN_MINOR_CTRL)
		goto out;
	if (minor <= ISDN_MINOR_CTRLMAX) {
		if (dev->profd == current)
			dev->profd = NULL;
		goto out;
	}
#ifdef CONFIG_ISDN_PPP
	if (minor <= ISDN_MINOR_PPPMAX)
		isdn_ppp_release(minor - ISDN_MINOR_PPP, filep);
#endif

 out:
	unlock_kernel();
	return 0;
}

static struct file_operations isdn_fops =
{
	owner:		THIS_MODULE,
	llseek:		isdn_llseek,
	read:		isdn_read,
	write:		isdn_write,
	poll:		isdn_poll,
	ioctl:		isdn_ioctl,
	open:		isdn_open,
	release:	isdn_close,
};

char *
isdn_map_eaz2msn(char *msn, int di)
{
	driver *this = dev->drv[di];
	int i;

	if (strlen(msn) == 1) {
		i = msn[0] - '0';
		if ((i >= 0) && (i <= 9))
			if (strlen(this->msn2eaz[i]))
				return (this->msn2eaz[i]);
	}
	return (msn);
}

/*
 * Find an unused ISDN-channel, whose feature-flags match the
 * given L2- and L3-protocols.
 */
#define L2V (~(ISDN_FEATURE_L2_V11096|ISDN_FEATURE_L2_V11019|ISDN_FEATURE_L2_V11038))

int
isdn_get_free_channel(int usage, int l2_proto, int l3_proto, int pre_dev
		      ,int pre_chan, char *msn)
{
	int i;
	ulong flags;
	ulong features;
	ulong vfeatures;

	save_flags(flags);
	cli();
	features = ((1 << l2_proto) | (0x10000 << l3_proto));
	vfeatures = (((1 << l2_proto) | (0x10000 << l3_proto)) &
		     ~(ISDN_FEATURE_L2_V11096|ISDN_FEATURE_L2_V11019|ISDN_FEATURE_L2_V11038));
	/* If Layer-2 protocol is V.110, accept drivers with
	 * transparent feature even if these don't support V.110
	 * because we can emulate this in linklevel.
	 */
	for (i = 0; i < ISDN_MAX_CHANNELS; i++)
		if (USG_NONE(dev->usage[i]) &&
		    (dev->drvmap[i] != -1)) {
			int d = dev->drvmap[i];
			if ((dev->usage[i] & ISDN_USAGE_EXCLUSIVE) &&
			((pre_dev != d) || (pre_chan != dev->chanmap[i])))
				continue;
			if (!strcmp(isdn_map_eaz2msn(msn, d), "-"))
				continue;
			if (dev->usage[i] & ISDN_USAGE_DISABLED)
			        continue; /* usage not allowed */
			if (dev->drv[d]->flags & DRV_FLAG_RUNNING) {
				if (((dev->drv[d]->interface->features & features) == features) ||
				    (((dev->drv[d]->interface->features & vfeatures) == vfeatures) &&
				     (dev->drv[d]->interface->features & ISDN_FEATURE_L2_TRANS))) {
					if ((pre_dev < 0) || (pre_chan < 0)) {
						dev->usage[i] &= ISDN_USAGE_EXCLUSIVE;
						dev->usage[i] |= usage;
						isdn_info_update();
						restore_flags(flags);
						return i;
					} else {
						if ((pre_dev == d) && (pre_chan == dev->chanmap[i])) {
							dev->usage[i] &= ISDN_USAGE_EXCLUSIVE;
							dev->usage[i] |= usage;
							isdn_info_update();
							restore_flags(flags);
							return i;
						}
					}
				}
			}
		}
	restore_flags(flags);
	return -1;
}

/*
 * Set state of ISDN-channel to 'unused'
 */
void
isdn_free_channel(int di, int ch, int usage)
{
	int i;
	ulong flags;

	save_flags(flags);
	cli();
	for (i = 0; i < ISDN_MAX_CHANNELS; i++)
		if (((!usage) || ((dev->usage[i] & ISDN_USAGE_MASK) == usage)) &&
		    (dev->drvmap[i] == di) &&
		    (dev->chanmap[i] == ch)) {
			dev->usage[i] &= (ISDN_USAGE_NONE | ISDN_USAGE_EXCLUSIVE);
			strcpy(dev->num[i], "???");
			dev->ibytes[i] = 0;
			dev->obytes[i] = 0;
// 20.10.99 JIM, try to reinitialize v110 !
			dev->v110emu[i] = 0;
			atomic_set(&(dev->v110use[i]), 0);
			isdn_v110_close(dev->v110[i]);
			dev->v110[i] = NULL;
// 20.10.99 JIM, try to reinitialize v110 !
			isdn_info_update();
			isdn_free_queue(&dev->drv[di]->rpqueue[ch]);
		}
	restore_flags(flags);
}

/*
 * Cancel Exclusive-Flag for ISDN-channel
 */
void
isdn_unexclusive_channel(int di, int ch)
{
	int i;
	ulong flags;

	save_flags(flags);
	cli();
	for (i = 0; i < ISDN_MAX_CHANNELS; i++)
		if ((dev->drvmap[i] == di) &&
		    (dev->chanmap[i] == ch)) {
			dev->usage[i] &= ~ISDN_USAGE_EXCLUSIVE;
			isdn_info_update();
			restore_flags(flags);
			return;
		}
	restore_flags(flags);
}

/*
 *  writebuf replacement for SKB_ABLE drivers
 */
static int
isdn_writebuf_stub(int drvidx, int chan, const u_char * buf, int len,
		   int user)
{
	int ret;
	int hl = dev->drv[drvidx]->interface->hl_hdrlen;
	struct sk_buff *skb = alloc_skb(hl + len, GFP_ATOMIC);

	if (!skb)
		return 0;
	skb_reserve(skb, hl);
	if (user)
		copy_from_user(skb_put(skb, len), buf, len);
	else
		memcpy(skb_put(skb, len), buf, len);
	ret = dev->drv[drvidx]->interface->writebuf_skb(drvidx, chan, 1, skb);
	if (ret <= 0)
		dev_kfree_skb(skb);
	if (ret > 0)
		dev->obytes[isdn_dc2minor(drvidx, chan)] += ret;
	return ret;
}

/*
 * Return: length of data on success, -ERRcode on failure.
 */
int
isdn_writebuf_skb_stub(int drvidx, int chan, int ack, struct sk_buff *skb)
{
	int ret;
	struct sk_buff *nskb = NULL;
	int v110_ret = skb->len;
	int idx = isdn_dc2minor(drvidx, chan);

	if (dev->v110[idx]) {
		atomic_inc(&dev->v110use[idx]);
		nskb = isdn_v110_encode(dev->v110[idx], skb);
		atomic_dec(&dev->v110use[idx]);
		if (!nskb)
			return 0;
		v110_ret = *((int *)nskb->data);
		skb_pull(nskb, sizeof(int));
		if (!nskb->len) {
			dev_kfree_skb(nskb);
			return v110_ret;
		}
		/* V.110 must always be acknowledged */
		ack = 1;
		ret = dev->drv[drvidx]->interface->writebuf_skb(drvidx, chan, ack, nskb);
	} else {
		int hl = dev->drv[drvidx]->interface->hl_hdrlen;

		if( skb_headroom(skb) < hl ){
			/* 
			 * This should only occur when new HL driver with
			 * increased hl_hdrlen was loaded after netdevice
			 * was created and connected to the new driver.
			 *
			 * The V.110 branch (re-allocates on its own) does
			 * not need this
			 */
			struct sk_buff * skb_tmp;

			skb_tmp = skb_realloc_headroom(skb, hl);
			printk(KERN_DEBUG "isdn_writebuf_skb_stub: reallocating headroom%s\n", skb_tmp ? "" : " failed");
			if (!skb_tmp) return -ENOMEM; /* 0 better? */
			ret = dev->drv[drvidx]->interface->writebuf_skb(drvidx, chan, ack, skb_tmp);
			if( ret > 0 ){
				dev_kfree_skb(skb);
			} else {
				dev_kfree_skb(skb_tmp);
			}
		} else {
			ret = dev->drv[drvidx]->interface->writebuf_skb(drvidx, chan, ack, skb);
		}
	}
	if (ret > 0) {
		dev->obytes[idx] += ret;
		if (dev->v110[idx]) {
			atomic_inc(&dev->v110use[idx]);
			dev->v110[idx]->skbuser++;
			atomic_dec(&dev->v110use[idx]);
			/* For V.110 return unencoded data length */
			ret = v110_ret;
			/* if the complete frame was send we free the skb;
			   if not upper function will requeue the skb */ 
			if (ret == skb->len)
				dev_kfree_skb(skb);
		}
	} else
		if (dev->v110[idx])
			dev_kfree_skb(nskb);
	return ret;
}

int
register_isdn_module(isdn_module *m) {
	return 0;
}

int
unregister_isdn_module(isdn_module *m) {
	return 0;
}

int
isdn_add_channels(driver *d, int drvidx, int n, int adding)
{
	int j, k, m;
	ulong flags;

	init_waitqueue_head(&d->st_waitq);
	if (d->flags & DRV_FLAG_RUNNING)
		return -1;
       	if (n < 1) return 0;

	m = (adding) ? d->channels + n : n;

	if (dev->channels + n > ISDN_MAX_CHANNELS) {
		printk(KERN_WARNING "register_isdn: Max. %d channels supported\n",
		       ISDN_MAX_CHANNELS);
		return -1;
	}

	if ((adding) && (d->rcverr))
		kfree(d->rcverr);
	if (!(d->rcverr = (int *) kmalloc(sizeof(int) * m, GFP_KERNEL))) {
		printk(KERN_WARNING "register_isdn: Could not alloc rcverr\n");
		return -1;
	}
	memset((char *) d->rcverr, 0, sizeof(int) * m);

	if ((adding) && (d->rcvcount))
		kfree(d->rcvcount);
	if (!(d->rcvcount = (int *) kmalloc(sizeof(int) * m, GFP_KERNEL))) {
		printk(KERN_WARNING "register_isdn: Could not alloc rcvcount\n");
		if (!adding) kfree(d->rcverr);
		return -1;
	}
	memset((char *) d->rcvcount, 0, sizeof(int) * m);

	if ((adding) && (d->rpqueue)) {
		for (j = 0; j < d->channels; j++)
			isdn_free_queue(&d->rpqueue[j]);
		kfree(d->rpqueue);
	}
	if (!(d->rpqueue =
	      (struct sk_buff_head *) kmalloc(sizeof(struct sk_buff_head) * m, GFP_KERNEL))) {
		printk(KERN_WARNING "register_isdn: Could not alloc rpqueue\n");
		if (!adding) {
			kfree(d->rcvcount);
			kfree(d->rcverr);
		}
		return -1; 
	}
	for (j = 0; j < m; j++) {
		skb_queue_head_init(&d->rpqueue[j]);
	}

	if ((adding) && (d->rcv_waitq))
		kfree(d->rcv_waitq);
	d->rcv_waitq = (wait_queue_head_t *)
		kmalloc(sizeof(wait_queue_head_t) * 2 * m, GFP_KERNEL);
	if (!d->rcv_waitq) {
		printk(KERN_WARNING "register_isdn: Could not alloc rcv_waitq\n");
		if (!adding) {
			kfree(d->rpqueue);
			kfree(d->rcvcount);
			kfree(d->rcverr);
		}
		return -1;
	}
	d->snd_waitq = d->rcv_waitq + m;
	for (j = 0; j < m; j++) {
		init_waitqueue_head(&d->rcv_waitq[j]);
		init_waitqueue_head(&d->snd_waitq[j]);
	}

	dev->channels += n;
	save_flags(flags);
	cli();
	for (j = d->channels; j < m; j++)
		for (k = 0; k < ISDN_MAX_CHANNELS; k++)
			if (dev->chanmap[k] < 0) {
				dev->chanmap[k] = j;
				dev->drvmap[k] = drvidx;
				isdn_register_devfs(k);
				break;
			}
	restore_flags(flags);
	d->channels = m;
	return 0;
}

/*
 * Low-level-driver registration
 */

static void
set_global_features(void)
{
	int drvidx;

	dev->global_features = 0;
	for (drvidx = 0; drvidx < ISDN_MAX_DRIVERS; drvidx++) {
		if (!dev->drv[drvidx])
			continue;
		if (dev->drv[drvidx]->interface)
			dev->global_features |= dev->drv[drvidx]->interface->features;
	}
}

#ifdef CONFIG_ISDN_DIVERSION

static char *map_drvname(int di)
{
  if ((di < 0) || (di >= ISDN_MAX_DRIVERS)) 
    return(NULL);
  return(dev->drvid[di]); /* driver name */
} /* map_drvname */

static int map_namedrv(char *id)
{  int i;

   for (i = 0; i < ISDN_MAX_DRIVERS; i++)
    { if (!strcmp(dev->drvid[i],id)) 
        return(i);
    }
   return(-1);
} /* map_namedrv */

int DIVERT_REG_NAME(isdn_divert_if *i_div)
{
  if (i_div->if_magic != DIVERT_IF_MAGIC) 
    return(DIVERT_VER_ERR);
  switch (i_div->cmd)
    {
      case DIVERT_CMD_REL:
        if (divert_if != i_div) 
          return(DIVERT_REL_ERR);
        divert_if = NULL; /* free interface */
        MOD_DEC_USE_COUNT;
        return(DIVERT_NO_ERR);

      case DIVERT_CMD_REG:
        if (divert_if) 
          return(DIVERT_REG_ERR);
        i_div->ll_cmd = isdn_command; /* set command function */
        i_div->drv_to_name = map_drvname; 
        i_div->name_to_drv = map_namedrv; 
        MOD_INC_USE_COUNT;
        divert_if = i_div; /* remember interface */
        return(DIVERT_NO_ERR);

      default:
        return(DIVERT_CMD_ERR);   
    }
} /* DIVERT_REG_NAME */

EXPORT_SYMBOL(DIVERT_REG_NAME);

#endif CONFIG_ISDN_DIVERSION


EXPORT_SYMBOL(register_isdn);
EXPORT_SYMBOL(register_isdn_module);
EXPORT_SYMBOL(unregister_isdn_module);
#ifdef CONFIG_ISDN_PPP
EXPORT_SYMBOL(isdn_ppp_register_compressor);
EXPORT_SYMBOL(isdn_ppp_unregister_compressor);
#endif

int
register_isdn(isdn_if * i)
{
	driver *d;
	int j;
	ulong flags;
	int drvidx;

	if (dev->drivers >= ISDN_MAX_DRIVERS) {
		printk(KERN_WARNING "register_isdn: Max. %d drivers supported\n",
		       ISDN_MAX_DRIVERS);
		return 0;
	}
	if (!i->writebuf_skb) {
		printk(KERN_WARNING "register_isdn: No write routine given.\n");
		return 0;
	}
	if (!(d = (driver *) kmalloc(sizeof(driver), GFP_KERNEL))) {
		printk(KERN_WARNING "register_isdn: Could not alloc driver-struct\n");
		return 0;
	}
	memset((char *) d, 0, sizeof(driver));

	d->maxbufsize = i->maxbufsize;
	d->pktcount = 0;
	d->stavail = 0;
	d->flags = DRV_FLAG_LOADED;
	d->online = 0;
	d->interface = i;
	d->channels = 0;
	for (drvidx = 0; drvidx < ISDN_MAX_DRIVERS; drvidx++)
		if (!dev->drv[drvidx])
			break;
	if (isdn_add_channels(d, drvidx, i->channels, 0)) {
		kfree(d);
		return 0;
	}
	i->channels = drvidx;
	i->rcvcallb_skb = isdn_receive_skb_callback;
	i->statcallb = isdn_status_callback;
	if (!strlen(i->id))
		sprintf(i->id, "line%d", drvidx);
	save_flags(flags);
	cli();
	for (j = 0; j < drvidx; j++)
		if (!strcmp(i->id, dev->drvid[j]))
			sprintf(i->id, "line%d", drvidx);
	dev->drv[drvidx] = d;
	strcpy(dev->drvid[drvidx], i->id);
	isdn_info_update();
	dev->drivers++;
	set_global_features();
	restore_flags(flags);
	return 1;
}

/*
 *****************************************************************************
 * And now the modules code.
 *****************************************************************************
 */

extern int printk(const char *fmt,...);

#ifdef MODULE
#define isdn_init init_module
#endif

static char *
isdn_getrev(const char *revision)
{
	char *rev;
	char *p;

	if ((p = strchr(revision, ':'))) {
		rev = p + 2;
		p = strchr(rev, '$');
		*--p = 0;
	} else
		rev = "???";
	return rev;
}

#ifdef CONFIG_DEVFS_FS

static devfs_handle_t devfs_handle;

static void isdn_register_devfs(int k)
{
	char buf[11];

	sprintf (buf, "isdn%d", k);
	dev->devfs_handle_isdnX[k] =
	    devfs_register (devfs_handle, buf, DEVFS_FL_DEFAULT,
			    ISDN_MAJOR, ISDN_MINOR_B + k,0600 | S_IFCHR,
			    &isdn_fops, NULL);
	sprintf (buf, "isdnctrl%d", k);
	dev->devfs_handle_isdnctrlX[k] =
	    devfs_register (devfs_handle, buf, DEVFS_FL_DEFAULT,
			    ISDN_MAJOR, ISDN_MINOR_CTRL + k, 0600 | S_IFCHR,
			    &isdn_fops, NULL);
}

static void isdn_unregister_devfs(int k)
{
	devfs_unregister (dev->devfs_handle_isdnX[k]);
	devfs_unregister (dev->devfs_handle_isdnctrlX[k]);
}

static void isdn_init_devfs(void)
{
#  ifdef CONFIG_ISDN_PPP
	int i;
#  endif

	devfs_handle = devfs_mk_dir (NULL, "isdn", NULL);
#  ifdef CONFIG_ISDN_PPP
	for (i = 0; i < ISDN_MAX_CHANNELS; i++) {
		char buf[8];

		sprintf (buf, "ippp%d", i);
		dev->devfs_handle_ipppX[i] =
		    devfs_register (devfs_handle, buf, DEVFS_FL_DEFAULT,
				    ISDN_MAJOR, ISDN_MINOR_PPP + i,
				    0600 | S_IFCHR, &isdn_fops, NULL);
	}
#  endif

	dev->devfs_handle_isdninfo =
	    devfs_register (devfs_handle, "isdninfo", DEVFS_FL_DEFAULT,
			    ISDN_MAJOR, ISDN_MINOR_STATUS, 0600 | S_IFCHR,
			    &isdn_fops, NULL);
	dev->devfs_handle_isdnctrl =
	    devfs_register (devfs_handle, "isdnctrl", DEVFS_FL_DEFAULT,
			    ISDN_MAJOR, ISDN_MINOR_CTRL, 0600 | S_IFCHR, 
			    &isdn_fops, NULL);
}

static void isdn_cleanup_devfs(void)
{
#  ifdef CONFIG_ISDN_PPP
	int i;
	for (i = 0; i < ISDN_MAX_CHANNELS; i++) 
		devfs_unregister (dev->devfs_handle_ipppX[i]);
#  endif
	devfs_unregister (dev->devfs_handle_isdninfo);
	devfs_unregister (dev->devfs_handle_isdnctrl);
	devfs_unregister (devfs_handle);
}

#else   /* CONFIG_DEVFS_FS */
static void isdn_register_devfs(int dummy)
{
	return;
}

static void isdn_unregister_devfs(int dummy)
{
	return;
}

static void isdn_init_devfs(void)
{
    return;
}

static void isdn_cleanup_devfs(void)
{
    return;
}

#endif  /* CONFIG_DEVFS_FS */

/*
 * Allocate and initialize all data, register modem-devices
 */
int
isdn_init(void)
{
	int i;
	char tmprev[50];

	if (!(dev = (isdn_dev *) vmalloc(sizeof(isdn_dev)))) {
		printk(KERN_WARNING "isdn: Could not allocate device-struct.\n");
		return -EIO;
	}
	memset((char *) dev, 0, sizeof(isdn_dev));
	init_timer(&dev->timer);
	dev->timer.function = isdn_timer_funct;
	init_MUTEX(&dev->sem);
	init_waitqueue_head(&dev->info_waitq);
	for (i = 0; i < ISDN_MAX_CHANNELS; i++) {
		dev->drvmap[i] = -1;
		dev->chanmap[i] = -1;
		dev->m_idx[i] = -1;
		strcpy(dev->num[i], "???");
		init_waitqueue_head(&dev->mdm.info[i].open_wait);
		init_waitqueue_head(&dev->mdm.info[i].close_wait);
	}
	if (devfs_register_chrdev(ISDN_MAJOR, "isdn", &isdn_fops)) {
		printk(KERN_WARNING "isdn: Could not register control devices\n");
		vfree(dev);
		return -EIO;
	}
	isdn_init_devfs();
	if ((i = isdn_tty_modem_init()) < 0) {
		printk(KERN_WARNING "isdn: Could not register tty devices\n");
		if (i == -3)
			tty_unregister_driver(&dev->mdm.cua_modem);
		if (i <= -2)
			tty_unregister_driver(&dev->mdm.tty_modem);
		vfree(dev);
		isdn_cleanup_devfs();
		devfs_unregister_chrdev(ISDN_MAJOR, "isdn");
		return -EIO;
	}
#ifdef CONFIG_ISDN_PPP
	if (isdn_ppp_init() < 0) {
		printk(KERN_WARNING "isdn: Could not create PPP-device-structs\n");
		tty_unregister_driver(&dev->mdm.tty_modem);
		tty_unregister_driver(&dev->mdm.cua_modem);
		for (i = 0; i < ISDN_MAX_CHANNELS; i++)
			kfree(dev->mdm.info[i].xmit_buf - 4);
		isdn_cleanup_devfs();
		devfs_unregister_chrdev(ISDN_MAJOR, "isdn");
		vfree(dev);
		return -EIO;
	}
#endif                          /* CONFIG_ISDN_PPP */

	strcpy(tmprev, isdn_revision);
	printk(KERN_NOTICE "ISDN subsystem Rev: %s/", isdn_getrev(tmprev));
	strcpy(tmprev, isdn_tty_revision);
	printk("%s/", isdn_getrev(tmprev));
	strcpy(tmprev, isdn_net_revision);
	printk("%s/", isdn_getrev(tmprev));
	strcpy(tmprev, isdn_ppp_revision);
	printk("%s/", isdn_getrev(tmprev));
	strcpy(tmprev, isdn_audio_revision);
	printk("%s/", isdn_getrev(tmprev));
	strcpy(tmprev, isdn_v110_revision);
	printk("%s", isdn_getrev(tmprev));

#ifdef MODULE
	printk(" loaded\n");
#else
	printk("\n");
	isdn_cards_init();
#endif
	isdn_info_update();
	return 0;
}

#ifdef MODULE
/*
 * Unload module
 */
void
cleanup_module(void)
{
	int flags;
	int i;

#ifdef CONFIG_ISDN_PPP
	isdn_ppp_cleanup();
#endif
	save_flags(flags);
	cli();
	if (isdn_net_rmall() < 0) {
		printk(KERN_WARNING "isdn: net-device busy, remove cancelled\n");
		restore_flags(flags);
		return;
	}
	if (tty_unregister_driver(&dev->mdm.tty_modem)) {
		printk(KERN_WARNING "isdn: ttyI-device busy, remove cancelled\n");
		restore_flags(flags);
		return;
	}
	if (tty_unregister_driver(&dev->mdm.cua_modem)) {
		printk(KERN_WARNING "isdn: cui-device busy, remove cancelled\n");
		restore_flags(flags);
		return;
	}
	for (i = 0; i < ISDN_MAX_CHANNELS; i++) {
		isdn_tty_cleanup_xmit(&dev->mdm.info[i]);
		kfree(dev->mdm.info[i].xmit_buf - 4);
#ifdef CONFIG_ISDN_TTY_FAX
		kfree(dev->mdm.info[i].fax);
#endif
	}
	if (devfs_unregister_chrdev(ISDN_MAJOR, "isdn") != 0) {
		printk(KERN_WARNING "isdn: controldevice busy, remove cancelled\n");
		restore_flags(flags);
	} else {
		isdn_cleanup_devfs();
		del_timer(&dev->timer);
		restore_flags(flags);
		/* call vfree with interrupts enabled, else it will hang */
		vfree(dev);
		printk(KERN_NOTICE "ISDN-subsystem unloaded\n");
	}
}
#endif
