/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Pseudo-driver for the loopback interface.
 *
 * Version:	@(#)loopback.c	1.0.4b	08/16/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Donald Becker, <becker@scyld.com>
 *
 *		Alan Cox	:	Fixed oddments for NET3.014
 *		Alan Cox	:	Rejig for NET3.029 snap #3
 *		Alan Cox	: 	Fixed NET3.029 bugs and sped up
 *		Larry McVoy	:	Tiny tweak to double performance
 *		Alan Cox	:	Backed out LMV's tweak - the linux mm
 *					can't take it...
 *              Michael Griffith:       Don't bother computing the checksums
 *                                      on packets received on the loopback
 *                                      interface.
 *		Alexey Kuznetsov:	Potential hang under some extreme
 *					cases removed.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/socket.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/in.h>
#include <linux/init.h>

#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/io.h>

#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <net/checksum.h>
#include <linux/if_ether.h>	/* For the statistics structure. */
#include <linux/if_arp.h>	/* For ARPHRD_ETHER */
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/percpu.h>

static DEFINE_PER_CPU(struct net_device_stats, loopback_stats);

#define LOOPBACK_OVERHEAD (128 + MAX_HEADER + 16 + 16)

/* KISS: just allocate small chunks and copy bits.
 *
 * So, in fact, this is documentation, explaining what we expect
 * of largesending device modulo TCP checksum, which is ignored for loopback.
 */

static void emulate_large_send_offload(struct sk_buff *skb)
{
	struct iphdr *iph = skb->nh.iph;
	struct tcphdr *th = (struct tcphdr*)(skb->nh.raw + (iph->ihl * 4));
	unsigned int doffset = (iph->ihl + th->doff) * 4;
	unsigned int mtu = skb_shinfo(skb)->tso_size + doffset;
	unsigned int offset = 0;
	u32 seq = ntohl(th->seq);
	u16 id  = ntohs(iph->id);

	while (offset + doffset < skb->len) {
		unsigned int frag_size = min(mtu, skb->len - offset) - doffset;
		struct sk_buff *nskb = alloc_skb(mtu + 32, GFP_ATOMIC);

		if (!nskb)
			break;
		skb_reserve(nskb, 32);
		nskb->mac.raw = nskb->data - 14;
		nskb->nh.raw = nskb->data;
		iph = nskb->nh.iph;
		memcpy(nskb->data, skb->nh.raw, doffset);
		if (skb_copy_bits(skb,
				  doffset + offset,
				  nskb->data + doffset,
				  frag_size))
			BUG();
		skb_put(nskb, doffset + frag_size);
		nskb->ip_summed = CHECKSUM_UNNECESSARY;
		nskb->dev = skb->dev;
		nskb->priority = skb->priority;
		nskb->protocol = skb->protocol;
		nskb->dst = dst_clone(skb->dst);
		memcpy(nskb->cb, skb->cb, sizeof(skb->cb));
		nskb->pkt_type = skb->pkt_type;

		th = (struct tcphdr*)(nskb->nh.raw + iph->ihl*4);
		iph->tot_len = htons(frag_size + doffset);
		iph->id = htons(id);
		iph->check = 0;
		iph->check = ip_fast_csum((unsigned char *) iph, iph->ihl);
		th->seq = htonl(seq);
		if (offset + doffset + frag_size < skb->len)
			th->fin = th->psh = 0;
		netif_rx(nskb);
		offset += frag_size;
		seq += frag_size;
		id++;
	}

	dev_kfree_skb(skb);
}

/*
 * The higher levels take care of making this non-reentrant (it's
 * called with bh's disabled).
 */
static int loopback_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct net_device_stats *lb_stats;

	skb_orphan(skb);

	skb->protocol=eth_type_trans(skb,dev);
	skb->dev=dev;
#ifndef LOOPBACK_MUST_CHECKSUM
	skb->ip_summed = CHECKSUM_UNNECESSARY;
#endif

	if (skb_shinfo(skb)->tso_size) {
		BUG_ON(skb->protocol != htons(ETH_P_IP));
		BUG_ON(skb->nh.iph->protocol != IPPROTO_TCP);

		emulate_large_send_offload(skb);
		return 0;
	}

	dev->last_rx = jiffies;

	lb_stats = &per_cpu(loopback_stats, get_cpu());
	lb_stats->rx_bytes += skb->len;
	lb_stats->tx_bytes += skb->len;
	lb_stats->rx_packets++;
	lb_stats->tx_packets++;
	put_cpu();

	netif_rx(skb);

	return(0);
}

static struct net_device_stats *get_stats(struct net_device *dev)
{
	struct net_device_stats *stats = dev->priv;
	int i;

	if (!stats) {
		return NULL;
	}

	memset(stats, 0, sizeof(struct net_device_stats));

	for (i=0; i < NR_CPUS; i++) {
		struct net_device_stats *lb_stats;

		if (!cpu_possible(i)) 
			continue;
		lb_stats = &per_cpu(loopback_stats, i);
		stats->rx_bytes   += lb_stats->rx_bytes;
		stats->tx_bytes   += lb_stats->tx_bytes;
		stats->rx_packets += lb_stats->rx_packets;
		stats->tx_packets += lb_stats->tx_packets;
	}
				
	return stats;
}

struct net_device loopback_dev = {
	.name	 		= "lo",
	.mtu			= (16 * 1024) + 20 + 20 + 12,
	.hard_start_xmit	= loopback_xmit,
	.hard_header		= eth_header,
	.hard_header_cache	= eth_header_cache,
	.header_cache_update	= eth_header_cache_update,
	.hard_header_len	= ETH_HLEN,	/* 14	*/
	.addr_len		= ETH_ALEN,	/* 6	*/
	.tx_queue_len		= 0,
	.type			= ARPHRD_LOOPBACK,	/* 0x0001*/
	.rebuild_header		= eth_rebuild_header,
	.flags			= IFF_LOOPBACK,
	.features 		= NETIF_F_SG|NETIF_F_FRAGLIST
				  |NETIF_F_NO_CSUM|NETIF_F_HIGHDMA
				  |NETIF_F_LLTX,
};

/* Setup and register the of the LOOPBACK device. */
int __init loopback_init(void)
{
	struct net_device_stats *stats;

	/* Can survive without statistics */
	stats = kmalloc(sizeof(struct net_device_stats), GFP_KERNEL);
	if (stats) {
		memset(stats, 0, sizeof(struct net_device_stats));
		loopback_dev.priv = stats;
		loopback_dev.get_stats = &get_stats;
	}
	
	return register_netdev(&loopback_dev);
};

EXPORT_SYMBOL(loopback_dev);
