
/*
 * DECnet       An implementation of the DECnet protocol suite for the LINUX
 *              operating system.  DECnet is implemented using the  BSD Socket
 *              interface as the means of communication with the user level.
 *
 *              DECnet Network Services Protocol (Output)
 *
 * Author:      Eduardo Marcelo Serrat <emserrat@geocities.com>
 *
 * Changes:
 *
 *    Steve Whitehouse:  Split into dn_nsp_in.c and dn_nsp_out.c from
 *                       original dn_nsp.c.
 *    Steve Whitehouse:  Updated to work with my new routing architecture.
 *    Steve Whitehouse:  Added changes from Eduardo Serrat's patches.
 *    Steve Whitehouse:  Now conninits have the "return" bit set.
 *    Steve Whitehouse:  Fixes to check alloc'd skbs are non NULL!
 *                       Moved output state machine into one function
 *    Steve Whitehouse:  New output state machine
 *         Paul Koning:  Connect Confirm message fix.
 *      Eduardo Serrat:  Fix to stop dn_nsp_do_disc() sending malformed packets.
 */

/******************************************************************************
    (c) 1995-1998 E.M. Serrat		emserrat@geocities.com
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
*******************************************************************************/

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/inet.h>
#include <linux/route.h>
#include <net/sock.h>
#include <asm/segment.h>
#include <asm/system.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/termios.h>      
#include <linux/interrupt.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/if_packet.h>
#include <net/neighbour.h>
#include <net/dst.h>
#include <net/dn_nsp.h>
#include <net/dn_dev.h>
#include <net/dn_route.h>


static int nsp_backoff[NSP_MAXRXTSHIFT + 1] = { 1, 2, 4, 8, 16, 32, 64, 64, 64, 64, 64, 64, 64 };

/*
 * If sk == NULL, then we assume that we are supposed to be making
 * a routing layer skb. If sk != NULL, then we are supposed to be
 * creating an skb for the NSP layer.
 *
 * The eventual aim is for each socket to have a cached header size
 * for its outgoing packets, and to set hdr from this when sk != NULL.
 */
struct sk_buff *dn_alloc_skb(struct sock *sk, int size, int pri)
{
	struct sk_buff *skb;
	int hdr = 64;

	if ((skb = alloc_skb(size + hdr, pri)) == NULL)
		return NULL;

	skb->protocol = __constant_htons(ETH_P_DNA_RT);
	skb->pkt_type = PACKET_OUTGOING;

	if (sk)
		skb_set_owner_w(skb, sk);

	skb_reserve(skb, hdr);

	return skb;
}

/*
 * Wrapper for the above, for allocs of data skbs. We try and get the
 * whole size thats been asked for (plus 11 bytes of header). If this
 * fails, then we try for any size over 16 bytes for SOCK_STREAMS.
 */
struct sk_buff *dn_alloc_send_skb(struct sock *sk, int *size, int noblock, int *err)
{
	int space;
	int len;
	struct sk_buff *skb = NULL;

	*err = 0;

	while(skb == NULL) {
		if (signal_pending(current)) {
			*err = ERESTARTSYS;
			break;
		}

		if (sk->shutdown & SEND_SHUTDOWN) {
			*err = EINVAL;
			break;
		}

		if (sk->err)
			break;

		len = *size + 11;
		space = sk->sndbuf - atomic_read(&sk->wmem_alloc);

		if (space < len) {
			if ((sk->socket->type == SOCK_STREAM) && (space >= (16 + 11)))
				len = space;
		}

		if (space < len) {
			set_bit(SOCK_ASYNC_NOSPACE, &sk->socket->flags);
			if (noblock) {
				*err = EWOULDBLOCK;
				break;
			}

			clear_bit(SOCK_ASYNC_WAITDATA, &sk->socket->flags);
			SOCK_SLEEP_PRE(sk)

			if ((sk->sndbuf - atomic_read(&sk->wmem_alloc)) < len)
				schedule();

			SOCK_SLEEP_POST(sk)
			continue;
		}

		if ((skb = dn_alloc_skb(sk, len, sk->allocation)) == NULL)
			continue;

		*size = len - 11;
	}

	return skb;
}

/*
 * Calculate persist timer based upon the smoothed round
 * trip time and the variance. Backoff according to the
 * nsp_backoff[] array.
 */
unsigned long dn_nsp_persist(struct sock *sk)
{
	struct dn_scp *scp = &sk->protinfo.dn;

	unsigned long t = ((scp->nsp_srtt >> 2) + scp->nsp_rttvar) >> 1;

	t *= nsp_backoff[scp->nsp_rxtshift];

	if (t < HZ) t = HZ;
	if (t > (600*HZ)) t = (600*HZ);

	if (scp->nsp_rxtshift < NSP_MAXRXTSHIFT)
		scp->nsp_rxtshift++;

	/* printk(KERN_DEBUG "rxtshift %lu, t=%lu\n", scp->nsp_rxtshift, t); */

	return t;
}

/*
 * This is called each time we get an estimate for the rtt
 * on the link.
 */
static void dn_nsp_rtt(struct sock *sk, long rtt)
{
	struct dn_scp *scp = &sk->protinfo.dn;
	long srtt = (long)scp->nsp_srtt;
	long rttvar = (long)scp->nsp_rttvar;
	long delta;

	/*
	 * If the jiffies clock flips over in the middle of timestamp
	 * gathering this value might turn out negative, so we make sure
	 * that is it always positive here.
	 */
	if (rtt < 0) 
		rtt = -rtt;
	/*
	 * Add new rtt to smoothed average
	 */
	delta = ((rtt << 3) - srtt);
	srtt += (delta >> 3);
	if (srtt >= 1) 
		scp->nsp_srtt = (unsigned long)srtt;
	else
		scp->nsp_srtt = 1;

	/*
	 * Add new rtt varience to smoothed varience
	 */
	delta >>= 1;
	rttvar += ((((delta>0)?(delta):(-delta)) - rttvar) >> 2);
	if (rttvar >= 1) 
		scp->nsp_rttvar = (unsigned long)rttvar;
	else
		scp->nsp_rttvar = 1;

	/* printk(KERN_DEBUG "srtt=%lu rttvar=%lu\n", scp->nsp_srtt, scp->nsp_rttvar); */
}

/*
 * Walk the queues, otherdata/linkservice first. Send as many
 * frames as the window allows, increment send counts on all
 * skbs which are sent. Reduce the window if we are retransmitting
 * frames.
 */
void dn_nsp_output(struct sock *sk)
{
	struct dn_scp *scp = &sk->protinfo.dn;
	unsigned long win = scp->snd_window;
	struct sk_buff *skb, *skb2, *list;
	struct dn_skb_cb *cb;
	int reduce_win = 0;

	/* printk(KERN_DEBUG "dn_nsp_output: ping\n"); */

	/*
	 * First we check for otherdata/linkservice messages
	 */
	skb = scp->other_xmit_queue.next;
	list = (struct sk_buff *)&scp->other_xmit_queue;
	while(win && (skb != list)) {
		if ((skb2 = skb_clone(skb, GFP_ATOMIC)) != NULL) {
			cb = (struct dn_skb_cb *)skb;
			if (cb->xmit_count > 0)
				reduce_win = 1;
			else
				cb->stamp = jiffies;
			cb->xmit_count++;
			skb2->sk = sk;
			dn_nsp_send(skb2);
		}
		skb = skb->next;
		win--;
	}

	/*
	 * If we may not send any data, we don't.
	 * Should this apply to otherdata as well ? - SJW
	 */
	if (scp->flowrem_sw != DN_SEND)
		goto recalc_window;

	skb = scp->data_xmit_queue.next;
	list = (struct sk_buff *)&scp->data_xmit_queue;
	while(win && (skb != list)) {
		if ((skb2 = skb_clone(skb, GFP_ATOMIC)) != NULL) {
			cb = (struct dn_skb_cb *)skb;
			if (cb->xmit_count > 0)
				reduce_win = 1;
			else
				cb->stamp = jiffies;
			cb->xmit_count++;
			skb2->sk = sk;
			dn_nsp_send(skb2);
		}
		skb = skb->next;
		win--;
	}

	/*
	 * If we've sent any frame more than once, we cut the
	 * send window size in half. There is always a minimum
	 * window size of one available.
	 */
recalc_window:
	if (reduce_win) {
		/* printk(KERN_DEBUG "Window reduction %ld\n", scp->snd_window); */
		scp->snd_window >>= 1;
		if (scp->snd_window < NSP_MIN_WINDOW)
			scp->snd_window = NSP_MIN_WINDOW;
	}
}

int dn_nsp_xmit_timeout(struct sock *sk)
{
	struct dn_scp *scp = &sk->protinfo.dn;

	dn_nsp_output(sk);

	if (skb_queue_len(&scp->data_xmit_queue) || skb_queue_len(&scp->other_xmit_queue))
		scp->persist = dn_nsp_persist(sk);

	return 0;
}

void dn_nsp_queue_xmit(struct sock *sk, struct sk_buff *skb, int oth)
{
	struct dn_scp *scp = &sk->protinfo.dn;
	struct dn_skb_cb *cb = (struct dn_skb_cb *)skb->cb;
	unsigned long t = ((scp->nsp_srtt >> 2) + scp->nsp_rttvar) >> 1;
	struct sk_buff *skb2;

	if (t < HZ) t = HZ;
	/*
	 * Slow start: If we have been idle for more than
	 * one RTT, then reset window to min size.
	 */
	if ((jiffies - scp->stamp) > t)
		scp->snd_window = NSP_MIN_WINDOW;

	/* printk(KERN_DEBUG "Window: %lu\n", scp->snd_window); */

	cb->xmit_count = 0;

	if (oth)
		skb_queue_tail(&scp->other_xmit_queue, skb);
	else
		skb_queue_tail(&scp->data_xmit_queue, skb);

	if (scp->flowrem_sw != DN_SEND)
		return;

	if ((skb2 = skb_clone(skb, GFP_ATOMIC)) != NULL) {
		cb->stamp = jiffies;
		cb->xmit_count++;
		skb2->sk = sk;
		dn_nsp_send(skb2);
	}
}

int dn_nsp_check_xmit_queue(struct sock *sk, struct sk_buff *skb, struct sk_buff_head *q, unsigned short acknum)
{
	struct dn_skb_cb *cb = (struct dn_skb_cb *)skb->cb;
	struct dn_scp *scp = &sk->protinfo.dn;
	struct sk_buff *skb2, *list, *ack = NULL;
	int wakeup = 0;
	unsigned long reftime = cb->stamp;
	unsigned long pkttime;
	unsigned short xmit_count;
	unsigned short segnum;

	skb2 = q->next;
	list = (struct sk_buff *)q;
	while(list != skb2) {
		struct dn_skb_cb *cb2 = (struct dn_skb_cb *)skb2->cb;

		if (before_or_equal(cb2->segnum, acknum))
			ack = skb2;

		/* printk(KERN_DEBUG "ack: %s %04x %04x\n", ack ? "ACK" : "SKIP", (int)cb2->segnum, (int)acknum); */

		skb2 = skb2->next;

		if (ack == NULL)
			continue;

		/* printk(KERN_DEBUG "check_xmit_queue: %04x, %d\n", acknum, cb2->xmit_count); */

		wakeup = 1;
		pkttime = cb2->stamp;
		xmit_count = cb2->xmit_count;
		segnum = cb2->segnum;
		skb_unlink(ack);
		kfree_skb(ack);
		ack = NULL;
		if (xmit_count == 1) {
			if (equal(segnum, acknum)) 
				dn_nsp_rtt(sk, (long)(pkttime - reftime));

			if (scp->snd_window < NSP_MAX_WINDOW)
				scp->snd_window++;
		}
	}

#if 0 /* Turned off due to possible interference in socket shutdown */
	if ((skb_queue_len(&scp->data_xmit_queue) == 0) &&
	    (skb_queue_len(&scp->other_xmit_queue) == 0))
		scp->persist = 0;
#endif

	return wakeup;
}

void dn_nsp_send_data_ack(struct sock *sk)
{
	struct sk_buff *skb = NULL;
	struct  nsp_data_ack_msg *msg;

	if ((skb = dn_alloc_skb(sk, 200, GFP_ATOMIC)) == NULL)
		return;
	
	msg = (struct nsp_data_ack_msg *)skb_put(skb,sizeof(*msg));

	msg->msgflg  = 0x04;			/* data ack message	*/
	msg->dstaddr = sk->protinfo.dn.addrrem;
	msg->srcaddr = sk->protinfo.dn.addrloc;
	msg->acknum  = dn_htons((sk->protinfo.dn.numdat_rcv & 0x0FFF) | 0x8000);

	sk->protinfo.dn.ackxmt_dat = sk->protinfo.dn.numdat_rcv;

	dn_nsp_send(skb);
}

void dn_nsp_send_oth_ack(struct sock *sk)
{
	struct sk_buff *skb = NULL;
	struct  nsp_data_ack_msg *msg;

	if ((skb = dn_alloc_skb(sk, 200, GFP_ATOMIC)) == NULL)
		return;
	
	msg = (struct nsp_data_ack_msg *)skb_put(skb,sizeof(*msg));

	msg->msgflg = 0x14;	/* oth ack message	*/
	msg->dstaddr = sk->protinfo.dn.addrrem;
	msg->srcaddr = sk->protinfo.dn.addrloc;
	msg->acknum  = dn_htons((sk->protinfo.dn.numoth_rcv & 0x0FFF) | 0x8000);

	sk->protinfo.dn.ackxmt_oth = sk->protinfo.dn.numoth_rcv;

	dn_nsp_send(skb);
}


void dn_send_conn_ack (struct sock *sk)
{
	struct dn_scp *scp = &sk->protinfo.dn;
	struct sk_buff *skb = NULL;
        struct nsp_conn_ack_msg *msg;

	if ((skb = dn_alloc_skb(sk, 3, sk->allocation)) == NULL)
		return;

        msg = (struct nsp_conn_ack_msg *)skb_put(skb, 3);
        msg->msgflg = 0x24;                   
	msg->dstaddr = scp->addrrem;

	dn_nsp_send(skb);	
}

void dn_nsp_delayed_ack(struct sock *sk)
{
	struct dn_scp *scp = &sk->protinfo.dn;

	if (scp->ackxmt_oth != scp->numoth_rcv)
		dn_nsp_send_oth_ack(sk);

	if (scp->ackxmt_dat != scp->numdat_rcv)
		dn_nsp_send_data_ack(sk);
}

static int dn_nsp_retrans_conn_conf(struct sock *sk)
{
	struct dn_scp *scp = &sk->protinfo.dn;

	if (scp->state == DN_CC)
		dn_send_conn_conf(sk, GFP_ATOMIC);

	return 0;
}

void dn_send_conn_conf(struct sock *sk, int gfp)
{
	struct dn_scp *scp = &sk->protinfo.dn;
	struct sk_buff *skb = NULL;
        struct nsp_conn_init_msg *msg;
	unsigned char len = scp->conndata_out.opt_optl;

	if ((skb = dn_alloc_skb(sk, 50 + scp->conndata_out.opt_optl, gfp)) == NULL)
		return;

        msg = (struct nsp_conn_init_msg *)skb_put(skb, sizeof(*msg));
        msg->msgflg = 0x28;                   
	msg->dstaddr = scp->addrrem;
        msg->srcaddr = scp->addrloc;
        msg->services = 0x01;
        msg->info = 0x03;
        msg->segsize = dn_htons(0x05B3);

	*skb_put(skb,1) = len;

	if (len > 0) 
		memcpy(skb_put(skb, len), scp->conndata_out.opt_data, len);
	

	dn_nsp_send(skb);

	scp->persist = dn_nsp_persist(sk);
	scp->persist_fxn = dn_nsp_retrans_conn_conf;
}


static __inline__ void dn_nsp_do_disc(struct sock *sk, unsigned char msgflg, 
			unsigned short reason, int gfp, struct dst_entry *dst,
			int ddl, unsigned char *dd, __u16 rem, __u16 loc)
{
	struct sk_buff *skb = NULL;
	int size = 7 + ddl + ((msgflg == NSP_DISCINIT) ? 1 : 0);
	unsigned char *msg;

	if ((dst == NULL) || (rem == 0)) {
		if (net_ratelimit())
			printk(KERN_DEBUG "DECnet: dn_nsp_do_disc: BUG! Please report this to SteveW@ACM.org rem=%u dst=%p\n", (unsigned)rem, dst);
		return;
	}

	if ((skb = dn_alloc_skb(sk, size, gfp)) == NULL)
		return;

	msg = skb_put(skb, size);
	*msg++ = msgflg;
	*(__u16 *)msg = rem;
	msg += 2;
	*(__u16 *)msg = loc;
	msg += 2;
	*(__u16 *)msg = dn_htons(reason);
	msg += 2;
	if (msgflg == NSP_DISCINIT)
		*msg++ = ddl;

	if (ddl) {
		memcpy(msg, dd, ddl);
	}

	/*
	 * This doesn't go via the dn_nsp_send() fucntion since we need
	 * to be able to send disc packets out which have no socket
	 * associations.
	 */
	skb->dst = dst_clone(dst);
	skb->dst->output(skb);
}


void dn_nsp_send_disc(struct sock *sk, unsigned char msgflg, 
			unsigned short reason, int gfp)
{
	struct dn_scp *scp = &sk->protinfo.dn;
	int ddl = 0;

	if (msgflg == NSP_DISCINIT)
		ddl = scp->discdata_out.opt_optl;

	if (reason == 0)
		reason = scp->discdata_out.opt_status;

	dn_nsp_do_disc(sk, msgflg, reason, gfp, sk->dst_cache, ddl, 
		scp->discdata_out.opt_data, scp->addrrem, scp->addrloc);
}


void dn_nsp_return_disc(struct sk_buff *skb, unsigned char msgflg, 
			unsigned short reason)
{
	struct dn_skb_cb *cb = (struct dn_skb_cb *)skb->cb;
	int ddl = 0;
	int gfp = GFP_ATOMIC;

	dn_nsp_do_disc(NULL, msgflg, reason, gfp, skb->dst, ddl, 
			NULL, cb->src_port, cb->dst_port);
}


void dn_nsp_send_lnk(struct sock *sk, unsigned short flgs)
{
	struct dn_scp *scp = &sk->protinfo.dn;
	struct sk_buff *skb = NULL;
	struct nsp_data_seg_msg	*msg;
	struct nsp_data_opt_msg	*msg1;
	struct dn_skb_cb *cb;

	if ((skb = dn_alloc_skb(sk, 80, GFP_ATOMIC)) == NULL)
		return;

	cb = (struct dn_skb_cb *)skb->cb;	
	msg = (struct nsp_data_seg_msg *)skb_put(skb, sizeof(*msg));
	msg->msgflg = 0x10;			/* Link svc message	*/
	msg->dstaddr = scp->addrrem;
	msg->srcaddr = scp->addrloc;

	msg1 = (struct nsp_data_opt_msg *)skb_put(skb, sizeof(*msg1));
	msg1->acknum = dn_htons((scp->ackxmt_oth & 0x0FFF) | 0x8000);
	msg1->segnum = dn_htons(cb->segnum = (scp->numoth++ & 0x0FFF));
        msg1->lsflgs = flgs;

	dn_nsp_queue_xmit(sk, skb, 1);

	scp->persist = dn_nsp_persist(sk);
	scp->persist_fxn = dn_nsp_xmit_timeout;

}

static int dn_nsp_retrans_conninit(struct sock *sk)
{
	struct dn_scp *scp = &sk->protinfo.dn;

	if (scp->state == DN_CI)
		dn_nsp_send_conninit(sk, NSP_RCI);

	return 0;
}

void dn_nsp_send_conninit(struct sock *sk, unsigned char msgflg)
{
	struct dn_scp *scp = &sk->protinfo.dn;
	struct sk_buff *skb = NULL;
	struct nsp_conn_init_msg *msg;
	unsigned char aux;
	unsigned char menuver;
	struct dn_skb_cb *cb;
	unsigned char type = 1;

	if ((skb = dn_alloc_skb(sk, 200, (msgflg == NSP_CI) ? sk->allocation : GFP_ATOMIC)) == NULL)
		return;

	cb  = (struct dn_skb_cb *)skb->cb;
	msg = (struct nsp_conn_init_msg *)skb_put(skb,sizeof(*msg));

	msg->msgflg	= msgflg;
	msg->dstaddr	= 0x0000;		/* Remote Node will assign it*/

	msg->srcaddr	= sk->protinfo.dn.addrloc;
	msg->services	= 1 | NSP_FC_NONE;	/* Requested flow control    */
	msg->info	= 0x03;			/* Version Number            */	
	msg->segsize	= dn_htons(1459);	/* Max segment size	     */	

	if (scp->peer.sdn_objnum)
		type = 0;

	skb_put(skb, dn_sockaddr2username(&scp->peer, skb->tail, type));
	skb_put(skb, dn_sockaddr2username(&scp->addr, skb->tail, 2));

	menuver = DN_MENUVER_ACC | DN_MENUVER_USR;
	if (scp->peer.sdn_flags & SDF_PROXY)
		menuver |= DN_MENUVER_PRX;
	if (scp->peer.sdn_flags & SDF_UICPROXY)
		menuver |= DN_MENUVER_UIC;

	*skb_put(skb, 1) = menuver;	/* Menu Version		*/
	
	aux = scp->accessdata.acc_userl;
	*skb_put(skb, 1) = aux;
	if (aux > 0)
	memcpy(skb_put(skb, aux), scp->accessdata.acc_user, aux);

	aux = scp->accessdata.acc_passl;
	*skb_put(skb, 1) = aux;
	if (aux > 0)
	memcpy(skb_put(skb, aux), scp->accessdata.acc_pass, aux);

	aux = scp->accessdata.acc_accl;
	*skb_put(skb, 1) = aux;
	if (aux > 0)
	memcpy(skb_put(skb, aux), scp->accessdata.acc_acc, aux);

	aux = scp->conndata_out.opt_optl;
	*skb_put(skb, 1) = aux;
	if (aux > 0)
	memcpy(skb_put(skb,aux), scp->conndata_out.opt_data, aux);

	sk->protinfo.dn.persist = dn_nsp_persist(sk);
	sk->protinfo.dn.persist_fxn = dn_nsp_retrans_conninit;

	cb->rt_flags = DN_RT_F_RQR;

	dn_nsp_send(skb);	
}

