/*
 *	Handle incoming frames
 *	Linux ethernet bridge
 *
 *	Authors:
 *	Lennert Buytenhek		<buytenh@gnu.org>
 *
 *	$Id: br_input.c,v 1.10 2001/12/24 04:50:20 davem Exp $
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/netfilter_bridge.h>
#include "br_private.h"

const unsigned char bridge_ula[6] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x00 };

static int br_pass_frame_up_finish(struct sk_buff *skb)
{
#ifdef CONFIG_NETFILTER_DEBUG
	skb->nf_debug = 0;
#endif
	netif_rx(skb);

	return 0;
}

static void br_pass_frame_up(struct net_bridge *br, struct sk_buff *skb)
{
	struct net_device *indev;

	br->statistics.rx_packets++;
	br->statistics.rx_bytes += skb->len;

	indev = skb->dev;
	skb->dev = br->dev;

	NF_HOOK(PF_BRIDGE, NF_BR_LOCAL_IN, skb, indev, NULL,
			br_pass_frame_up_finish);
}

int br_handle_frame_finish(struct sk_buff *skb)
{
	struct net_bridge *br;
	unsigned char *dest;
	struct net_bridge_fdb_entry *dst;
	struct net_bridge_port *p;
	int passedup;

	dest = skb->mac.ethernet->h_dest;

	rcu_read_lock();
	p = skb->dev->br_port;
	smp_read_barrier_depends();

	if (p == NULL || p->state == BR_STATE_DISABLED) {
		kfree_skb(skb);
		goto out;
	}

	br = p->br;
	passedup = 0;
	if (br->dev->flags & IFF_PROMISC) {
		struct sk_buff *skb2;

		skb2 = skb_clone(skb, GFP_ATOMIC);
		if (skb2 != NULL) {
			passedup = 1;
			br_pass_frame_up(br, skb2);
		}
	}

	if (dest[0] & 1) {
		br_flood_forward(br, skb, !passedup);
		if (!passedup)
			br_pass_frame_up(br, skb);
		goto out;
	}

	dst = __br_fdb_get(br, dest);
	if (dst != NULL && dst->is_local) {
		if (!passedup)
			br_pass_frame_up(br, skb);
		else
			kfree_skb(skb);
		goto out;
	}

	if (dst != NULL) {
		br_forward(dst->dst, skb);
		goto out;
	}

	br_flood_forward(br, skb, 0);

out:
	rcu_read_unlock();
	return 0;
}

int br_handle_frame(struct sk_buff *skb)
{
	unsigned char *dest;
	struct net_bridge_port *p;

	dest = skb->mac.ethernet->h_dest;

	rcu_read_lock();
	p = skb->dev->br_port;
	if (p == NULL || p->state == BR_STATE_DISABLED)
		goto err;

	if (skb->mac.ethernet->h_source[0] & 1)
		goto err;

	if (p->state == BR_STATE_LEARNING ||
	    p->state == BR_STATE_FORWARDING)
		br_fdb_insert(p->br, p, skb->mac.ethernet->h_source, 0);

	if (p->br->stp_enabled &&
	    !memcmp(dest, bridge_ula, 5) &&
	    !(dest[5] & 0xF0)) {
		if (!dest[5]) {
			NF_HOOK(PF_BRIDGE, NF_BR_LOCAL_IN, skb, skb->dev, 
				NULL, br_stp_handle_bpdu);
			rcu_read_unlock();
			return 0;
		}
	}

	else if (p->state == BR_STATE_FORWARDING) {
		if (br_should_route_hook && br_should_route_hook(&skb)) {
			rcu_read_unlock();
			return -1;
		}

		if (!memcmp(p->br->dev->dev_addr, dest, ETH_ALEN))
			skb->pkt_type = PACKET_HOST;

		NF_HOOK(PF_BRIDGE, NF_BR_PRE_ROUTING, skb, skb->dev, NULL,
			br_handle_frame_finish);
		rcu_read_unlock();
		return 0;
	}

err:
	rcu_read_unlock();
	kfree_skb(skb);
	return 0;
}
