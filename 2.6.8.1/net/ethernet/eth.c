/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Ethernet-type device handling.
 *
 * Version:	@(#)eth.c	1.0.7	05/25/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Mark Evans, <evansmp@uhura.aston.ac.uk>
 *		Florian  La Roche, <rzsfl@rz.uni-sb.de>
 *		Alan Cox, <gw4pts@gw4pts.ampr.org>
 * 
 * Fixes:
 *		Mr Linux	: Arp problems
 *		Alan Cox	: Generic queue tidyup (very tiny here)
 *		Alan Cox	: eth_header ntohs should be htons
 *		Alan Cox	: eth_rebuild_header missing an htons and
 *				  minor other things.
 *		Tegge		: Arp bug fixes. 
 *		Florian		: Removed many unnecessary functions, code cleanup
 *				  and changes for new arp and skbuff.
 *		Alan Cox	: Redid header building to reflect new format.
 *		Alan Cox	: ARP only when compiled with CONFIG_INET
 *		Greg Page	: 802.2 and SNAP stuff.
 *		Alan Cox	: MAC layer pointers/new format.
 *		Paul Gortmaker	: eth_copy_and_sum shouldn't csum padding.
 *		Alan Cox	: Protect against forwarding explosions with
 *				  older network drivers and IFF_ALLMULTI.
 *	Christer Weinigel	: Better rebuild header message.
 *             Andrew Morton    : 26Feb01: kill ether_setup() - use netdev_boot_setup().
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/ip.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/errno.h>
#include <linux/config.h>
#include <linux/init.h>
#include <net/dst.h>
#include <net/arp.h>
#include <net/sock.h>
#include <net/ipv6.h>
#include <net/ip.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/checksum.h>

extern int __init netdev_boot_setup(char *str);

__setup("ether=", netdev_boot_setup);

/*
 *	 Create the Ethernet MAC header for an arbitrary protocol layer 
 *
 *	saddr=NULL	means use device source address
 *	daddr=NULL	means leave destination address (eg unresolved arp)
 */

int eth_header(struct sk_buff *skb, struct net_device *dev, unsigned short type,
	   void *daddr, void *saddr, unsigned len)
{
	struct ethhdr *eth = (struct ethhdr *)skb_push(skb,ETH_HLEN);

	/* 
	 *	Set the protocol type. For a packet of type ETH_P_802_3 we put the length
	 *	in here instead. It is up to the 802.2 layer to carry protocol information.
	 */
	
	if(type!=ETH_P_802_3) 
		eth->h_proto = htons(type);
	else
		eth->h_proto = htons(len);

	/*
	 *	Set the source hardware address. 
	 */
	 
	if(saddr)
		memcpy(eth->h_source,saddr,dev->addr_len);
	else
		memcpy(eth->h_source,dev->dev_addr,dev->addr_len);

	/*
	 *	Anyway, the loopback-device should never use this function... 
	 */

	if (dev->flags & (IFF_LOOPBACK|IFF_NOARP)) 
	{
		memset(eth->h_dest, 0, dev->addr_len);
		return ETH_HLEN;
	}
	
	if(daddr)
	{
		memcpy(eth->h_dest,daddr,dev->addr_len);
		return ETH_HLEN;
	}
	
	return -ETH_HLEN;
}


/*
 *	Rebuild the Ethernet MAC header. This is called after an ARP
 *	(or in future other address resolution) has completed on this
 *	sk_buff. We now let ARP fill in the other fields.
 *
 *	This routine CANNOT use cached dst->neigh!
 *	Really, it is used only when dst->neigh is wrong.
 */

int eth_rebuild_header(struct sk_buff *skb)
{
	struct ethhdr *eth = (struct ethhdr *)skb->data;
	struct net_device *dev = skb->dev;

	switch (eth->h_proto)
	{
#ifdef CONFIG_INET
	case __constant_htons(ETH_P_IP):
 		return arp_find(eth->h_dest, skb);
#endif	
	default:
		printk(KERN_DEBUG
		       "%s: unable to resolve type %X addresses.\n", 
		       dev->name, (int)eth->h_proto);
		
		memcpy(eth->h_source, dev->dev_addr, dev->addr_len);
		break;
	}

	return 0;
}


/*
 *	Determine the packet's protocol ID. The rule here is that we 
 *	assume 802.3 if the type field is short enough to be a length.
 *	This is normal practice and works for any 'now in use' protocol.
 */
 
unsigned short eth_type_trans(struct sk_buff *skb, struct net_device *dev)
{
	struct ethhdr *eth;
	unsigned char *rawp;
	
	skb->mac.raw=skb->data;
	skb_pull(skb,ETH_HLEN);
	eth= skb->mac.ethernet;
#ifdef CONFIG_NET_CLS_ACT
	skb->input_dev = dev;
#endif
	
	if(*eth->h_dest&1)
	{
		if(memcmp(eth->h_dest,dev->broadcast, ETH_ALEN)==0)
			skb->pkt_type=PACKET_BROADCAST;
		else
			skb->pkt_type=PACKET_MULTICAST;
	}
	
	/*
	 *	This ALLMULTI check should be redundant by 1.4
	 *	so don't forget to remove it.
	 *
	 *	Seems, you forgot to remove it. All silly devices
	 *	seems to set IFF_PROMISC.
	 */
	 
	else if(1 /*dev->flags&IFF_PROMISC*/)
	{
		if(memcmp(eth->h_dest,dev->dev_addr, ETH_ALEN))
			skb->pkt_type=PACKET_OTHERHOST;
	}
	
	if (ntohs(eth->h_proto) >= 1536)
		return eth->h_proto;
		
	rawp = skb->data;
	
	/*
	 *	This is a magic hack to spot IPX packets. Older Novell breaks
	 *	the protocol design and runs IPX over 802.3 without an 802.2 LLC
	 *	layer. We look for FFFF which isn't a used 802.2 SSAP/DSAP. This
	 *	won't work for fault tolerant netware but does for the rest.
	 */
	if (*(unsigned short *)rawp == 0xFFFF)
		return htons(ETH_P_802_3);
		
	/*
	 *	Real 802.2 LLC
	 */
	return htons(ETH_P_802_2);
}

int eth_header_parse(struct sk_buff *skb, unsigned char *haddr)
{
	struct ethhdr *eth = skb->mac.ethernet;
	memcpy(haddr, eth->h_source, ETH_ALEN);
	return ETH_ALEN;
}

int eth_header_cache(struct neighbour *neigh, struct hh_cache *hh)
{
	unsigned short type = hh->hh_type;
	struct ethhdr *eth;
	struct net_device *dev = neigh->dev;

	eth = (struct ethhdr*)
		(((u8*)hh->hh_data) + (HH_DATA_OFF(sizeof(*eth))));

	if (type == __constant_htons(ETH_P_802_3))
		return -1;

	eth->h_proto = type;
	memcpy(eth->h_source, dev->dev_addr, dev->addr_len);
	memcpy(eth->h_dest, neigh->ha, dev->addr_len);
	hh->hh_len = ETH_HLEN;
	return 0;
}

/*
 * Called by Address Resolution module to notify changes in address.
 */

void eth_header_cache_update(struct hh_cache *hh, struct net_device *dev, unsigned char * haddr)
{
	memcpy(((u8*)hh->hh_data) + HH_DATA_OFF(sizeof(struct ethhdr)),
	       haddr, dev->addr_len);
}

EXPORT_SYMBOL(eth_type_trans);
