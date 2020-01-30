/*
 *	X.25 Packet Layer release 002
 *
 *	This is ALPHA test software. This code may break your machine, randomly fail to work with new 
 *	releases, misbehave and/or generally screw up. It might even work. 
 *
 *	This code REQUIRES 2.1.15 or higher
 *
 *	This module:
 *		This module is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 *	History
 *	X.25 001	Jonathan Naylor	Started coding.
 *      2000-09-04	Henner Eisen	Prevent freeing a dangling skb.
 */

#include <linux/config.h>
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
#include <linux/stat.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <linux/fcntl.h>
#include <linux/termios.h>	/* For TIOCINQ/OUTQ */
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/notifier.h>
#include <linux/proc_fs.h>
#include <linux/if_arp.h>
#include <net/x25.h>

static int x25_receive_data(struct sk_buff *skb, struct x25_neigh *nb)
{
	struct sock *sk;
	unsigned short frametype;
	unsigned int lci;

	frametype = skb->data[2];
        lci = ((skb->data[0] << 8) & 0xF00) + ((skb->data[1] << 0) & 0x0FF);

	/*
	 *	LCI of zero is always for us, and its always a link control
	 *	frame.
	 */
	if (lci == 0) {
		x25_link_control(skb, nb, frametype);
		return 0;
	}

	/*
	 *	Find an existing socket.
	 */
	if ((sk = x25_find_socket(lci, nb)) != NULL) {
		int queued = 1;

		skb->h.raw = skb->data;
		bh_lock_sock(sk);
		if (!sock_owned_by_user(sk)) {
			queued = x25_process_rx_frame(sk, skb);
		} else {
			sk_add_backlog(sk, skb);
		}
		bh_unlock_sock(sk);
		return queued;
	}

	/*
	 *	Is is a Call Request ? if so process it.
	 */
	if (frametype == X25_CALL_REQUEST)
		return x25_rx_call_request(skb, nb, lci);

	/*
	 *	Its not a Call Request, nor is it a control frame.
	 *      Let caller throw it away.
	 */
/*
	x25_transmit_clear_request(nb, lci, 0x0D);
*/
	printk(KERN_DEBUG "x25_receive_data(): unknown frame type %2x\n",frametype);

	return 0;
}

int x25_lapb_receive_frame(struct sk_buff *skb, struct net_device *dev,
			   struct packet_type *ptype)
{
	struct sk_buff *nskb;
	struct x25_neigh *nb;

	nskb = skb_copy(skb, GFP_ATOMIC);
	if (!nskb)
		goto drop;
	kfree_skb(skb);
	skb = nskb;

	/*
	 * Packet received from unrecognised device, throw it away.
	 */
	nb = x25_get_neigh(dev);
	if (!nb) {
		printk(KERN_DEBUG "X.25: unknown neighbour - %s\n", dev->name);
		goto drop;
	}

	switch (skb->data[0]) {
		case 0x00:
			skb_pull(skb, 1);
			if (x25_receive_data(skb, nb)) {
				x25_neigh_put(nb);
				goto out;
			}
			break;
		case 0x01:
			x25_link_established(nb);
			break;
		case 0x02:
			x25_link_terminated(nb);
			break;
	}
	x25_neigh_put(nb);
drop:
	kfree_skb(skb);
out:
	return 0;
}

int x25_llc_receive_frame(struct sk_buff *skb, struct net_device *dev,
			  struct packet_type *ptype)
{
	struct x25_neigh *nb;
	int rc = 0;

	skb->sk = NULL;

	/*
	 * Packet received from unrecognised device, throw it away.
	 */
	nb = x25_get_neigh(dev);
	if (!nb) {
		printk(KERN_DEBUG "X.25: unknown_neighbour - %s\n", dev->name);
		kfree_skb(skb);
	} else {
		rc = x25_receive_data(skb, nb);
		x25_neigh_put(nb);
	}

	return rc;
}

void x25_establish_link(struct x25_neigh *nb)
{
	struct sk_buff *skb;
	unsigned char *ptr;

	switch (nb->dev->type) {
		case ARPHRD_X25:
			if ((skb = alloc_skb(1, GFP_ATOMIC)) == NULL) {
				printk(KERN_ERR "x25_dev: out of memory\n");
				return;
			}
			ptr  = skb_put(skb, 1);
			*ptr = 0x01;
			break;

#if defined(CONFIG_LLC) || defined(CONFIG_LLC_MODULE)
		case ARPHRD_ETHER:
			return;
#endif
		default:
			return;
	}

	skb->protocol = htons(ETH_P_X25);
	skb->dev      = nb->dev;

	dev_queue_xmit(skb);
}

void x25_terminate_link(struct x25_neigh *nb)
{
	struct sk_buff *skb;
	unsigned char *ptr;

#if defined(CONFIG_LLC) || defined(CONFIG_LLC_MODULE)
	if (nb->dev->type == ARPHRD_ETHER)
		return;
#endif
	if (nb->dev->type != ARPHRD_X25)
		return;

	skb = alloc_skb(1, GFP_ATOMIC);
	if (!skb) {
		printk(KERN_ERR "x25_dev: out of memory\n");
		return;
	}

	ptr  = skb_put(skb, 1);
	*ptr = 0x02;

	skb->protocol = htons(ETH_P_X25);
	skb->dev      = nb->dev;
	dev_queue_xmit(skb);
}

void x25_send_frame(struct sk_buff *skb, struct x25_neigh *nb)
{
	unsigned char *dptr;

	skb->nh.raw = skb->data;

	switch (nb->dev->type) {
		case ARPHRD_X25:
			dptr  = skb_push(skb, 1);
			*dptr = 0x00;
			break;

#if defined(CONFIG_LLC) || defined(CONFIG_LLC_MODULE)
		case ARPHRD_ETHER:
			kfree_skb(skb);
			return;
#endif
		default:
			kfree_skb(skb);
			return;
	}

	skb->protocol = htons(ETH_P_X25);
	skb->dev      = nb->dev;

	dev_queue_xmit(skb);
}
