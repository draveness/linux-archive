/* linux/net/inet/arp.c
 *
 * Version:	$Id: arp.c,v 1.90 2000/10/04 09:20:56 anton Exp $
 *
 * Copyright (C) 1994 by Florian  La Roche
 *
 * This module implements the Address Resolution Protocol ARP (RFC 826),
 * which is used to convert IP addresses (or in the future maybe other
 * high-level addresses) into a low-level hardware address (like an Ethernet
 * address).
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * Fixes:
 *		Alan Cox	:	Removed the Ethernet assumptions in 
 *					Florian's code
 *		Alan Cox	:	Fixed some small errors in the ARP 
 *					logic
 *		Alan Cox	:	Allow >4K in /proc
 *		Alan Cox	:	Make ARP add its own protocol entry
 *		Ross Martin     :       Rewrote arp_rcv() and arp_get_info()
 *		Stephen Henson	:	Add AX25 support to arp_get_info()
 *		Alan Cox	:	Drop data when a device is downed.
 *		Alan Cox	:	Use init_timer().
 *		Alan Cox	:	Double lock fixes.
 *		Martin Seine	:	Move the arphdr structure
 *					to if_arp.h for compatibility.
 *					with BSD based programs.
 *		Andrew Tridgell :       Added ARP netmask code and
 *					re-arranged proxy handling.
 *		Alan Cox	:	Changed to use notifiers.
 *		Niibe Yutaka	:	Reply for this device or proxies only.
 *		Alan Cox	:	Don't proxy across hardware types!
 *		Jonathan Naylor :	Added support for NET/ROM.
 *		Mike Shaver     :       RFC1122 checks.
 *		Jonathan Naylor :	Only lookup the hardware address for
 *					the correct hardware type.
 *		Germano Caronni	:	Assorted subtle races.
 *		Craig Schlenter :	Don't modify permanent entry 
 *					during arp_rcv.
 *		Russ Nelson	:	Tidied up a few bits.
 *		Alexey Kuznetsov:	Major changes to caching and behaviour,
 *					eg intelligent arp probing and 
 *					generation
 *					of host down events.
 *		Alan Cox	:	Missing unlock in device events.
 *		Eckes		:	ARP ioctl control errors.
 *		Alexey Kuznetsov:	Arp free fix.
 *		Manuel Rodriguez:	Gratuitous ARP.
 *              Jonathan Layes  :       Added arpd support through kerneld 
 *                                      message queue (960314)
 *		Mike Shaver	:	/proc/sys/net/ipv4/arp_* support
 *		Mike McLagan    :	Routing by source
 *		Stuart Cheshire	:	Metricom and grat arp fixes
 *					*** FOR 2.1 clean this up ***
 *		Lawrence V. Stefani: (08/12/96) Added FDDI support.
 *		Alan Cox 	:	Took the AP1000 nasty FDDI hack and
 *					folded into the mainstream FDDI code.
 *					Ack spit, Linus how did you allow that
 *					one in...
 *		Jes Sorensen	:	Make FDDI work again in 2.1.x and
 *					clean up the APFDDI & gen. FDDI bits.
 *		Alexey Kuznetsov:	new arp state machine;
 *					now it is in net/core/neighbour.c.
 */

/* RFC1122 Status:
   2.3.2.1 (ARP Cache Validation):
     MUST provide mechanism to flush stale cache entries (OK)
     SHOULD be able to configure cache timeout (OK)
     MUST throttle ARP retransmits (OK)
   2.3.2.2 (ARP Packet Queue):
     SHOULD save at least one packet from each "conversation" with an
       unresolved IP address.  (OK)
   950727 -- MS
*/
      
#include <linux/types.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/config.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/errno.h>
#include <linux/in.h>
#include <linux/mm.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/fddidevice.h>
#include <linux/if_arp.h>
#include <linux/trdevice.h>
#include <linux/skbuff.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/init.h>
#ifdef CONFIG_SYSCTL
#include <linux/sysctl.h>
#endif

#include <net/ip.h>
#include <net/icmp.h>
#include <net/route.h>
#include <net/protocol.h>
#include <net/tcp.h>
#include <net/sock.h>
#include <net/arp.h>
#if defined(CONFIG_AX25) || defined(CONFIG_AX25_MODULE)
#include <net/ax25.h>
#if defined(CONFIG_NETROM) || defined(CONFIG_NETROM_MODULE)
#include <net/netrom.h>
#endif
#endif
#ifdef CONFIG_ATM_CLIP
#include <net/atmclip.h>
#endif

#include <asm/system.h>
#include <asm/uaccess.h>



/*
 *	Interface to generic neighbour cache.
 */
static u32 arp_hash(const void *pkey, const struct net_device *dev);
static int arp_constructor(struct neighbour *neigh);
static void arp_solicit(struct neighbour *neigh, struct sk_buff *skb);
static void arp_error_report(struct neighbour *neigh, struct sk_buff *skb);
static void parp_redo(struct sk_buff *skb);

static struct neigh_ops arp_generic_ops =
{
	AF_INET,
	NULL,
	arp_solicit,
	arp_error_report,
	neigh_resolve_output,
	neigh_connected_output,
	dev_queue_xmit,
	dev_queue_xmit
};

static struct neigh_ops arp_hh_ops =
{
	AF_INET,
	NULL,
	arp_solicit,
	arp_error_report,
	neigh_resolve_output,
	neigh_resolve_output,
	dev_queue_xmit,
	dev_queue_xmit
};

static struct neigh_ops arp_direct_ops =
{
	AF_INET,
	NULL,
	NULL,
	NULL,
	dev_queue_xmit,
	dev_queue_xmit,
	dev_queue_xmit,
	dev_queue_xmit
};

struct neigh_ops arp_broken_ops =
{
	AF_INET,
	NULL,
	arp_solicit,
	arp_error_report,
	neigh_compat_output,
	neigh_compat_output,
	dev_queue_xmit,
	dev_queue_xmit,
};

struct neigh_table arp_tbl =
{
	NULL,
	AF_INET,
	sizeof(struct neighbour) + 4,
	4,
	arp_hash,
	arp_constructor,
	NULL,
	NULL,
	parp_redo,
	"arp_cache",
        { NULL, NULL, &arp_tbl, 0, NULL, NULL,
		  30*HZ, 1*HZ, 60*HZ, 30*HZ, 5*HZ, 3, 3, 0, 3, 1*HZ, (8*HZ)/10, 64, 1*HZ },
	30*HZ, 128, 512, 1024,
};

int arp_mc_map(u32 addr, u8 *haddr, struct net_device *dev, int dir)
{
	switch (dev->type) {
	case ARPHRD_ETHER:
	case ARPHRD_FDDI:
	case ARPHRD_IEEE802:
		ip_eth_mc_map(addr, haddr) ; 
		return 0 ; 
	case ARPHRD_IEEE802_TR:
		ip_tr_mc_map(addr, haddr) ; 
		return 0;
	default:
		if (dir) {
			memcpy(haddr, dev->broadcast, dev->addr_len);
			return 0;
		}
	}
	return -EINVAL;
}


static u32 arp_hash(const void *pkey, const struct net_device *dev)
{
	u32 hash_val;

	hash_val = *(u32*)pkey;
	hash_val ^= (hash_val>>16);
	hash_val ^= hash_val>>8;
	hash_val ^= hash_val>>3;
	hash_val = (hash_val^dev->ifindex)&NEIGH_HASHMASK;

	return hash_val;
}

static int arp_constructor(struct neighbour *neigh)
{
	u32 addr = *(u32*)neigh->primary_key;
	struct net_device *dev = neigh->dev;
	struct in_device *in_dev = in_dev_get(dev);

	if (in_dev == NULL)
		return -EINVAL;

	neigh->type = inet_addr_type(addr);
	if (in_dev->arp_parms)
		neigh->parms = in_dev->arp_parms;

	in_dev_put(in_dev);

	if (dev->hard_header == NULL) {
		neigh->nud_state = NUD_NOARP;
		neigh->ops = &arp_direct_ops;
		neigh->output = neigh->ops->queue_xmit;
	} else {
		/* Good devices (checked by reading texts, but only Ethernet is
		   tested)

		   ARPHRD_ETHER: (ethernet, apfddi)
		   ARPHRD_FDDI: (fddi)
		   ARPHRD_IEEE802: (tr)
		   ARPHRD_METRICOM: (strip)
		   ARPHRD_ARCNET:
		   etc. etc. etc.

		   ARPHRD_IPDDP will also work, if author repairs it.
		   I did not it, because this driver does not work even
		   in old paradigm.
		 */

#if 1
		/* So... these "amateur" devices are hopeless.
		   The only thing, that I can say now:
		   It is very sad that we need to keep ugly obsolete
		   code to make them happy.

		   They should be moved to more reasonable state, now
		   they use rebuild_header INSTEAD OF hard_start_xmit!!!
		   Besides that, they are sort of out of date
		   (a lot of redundant clones/copies, useless in 2.1),
		   I wonder why people believe that they work.
		 */
		switch (dev->type) {
		default:
			break;
		case ARPHRD_ROSE:	
#if defined(CONFIG_AX25) || defined(CONFIG_AX25_MODULE)
		case ARPHRD_AX25:
#if defined(CONFIG_NETROM) || defined(CONFIG_NETROM_MODULE)
		case ARPHRD_NETROM:
#endif
			neigh->ops = &arp_broken_ops;
			neigh->output = neigh->ops->output;
			return 0;
#endif
		;}
#endif
		if (neigh->type == RTN_MULTICAST) {
			neigh->nud_state = NUD_NOARP;
			arp_mc_map(addr, neigh->ha, dev, 1);
		} else if (dev->flags&(IFF_NOARP|IFF_LOOPBACK)) {
			neigh->nud_state = NUD_NOARP;
			memcpy(neigh->ha, dev->dev_addr, dev->addr_len);
		} else if (neigh->type == RTN_BROADCAST || dev->flags&IFF_POINTOPOINT) {
			neigh->nud_state = NUD_NOARP;
			memcpy(neigh->ha, dev->broadcast, dev->addr_len);
		}
		if (dev->hard_header_cache)
			neigh->ops = &arp_hh_ops;
		else
			neigh->ops = &arp_generic_ops;
		if (neigh->nud_state&NUD_VALID)
			neigh->output = neigh->ops->connected_output;
		else
			neigh->output = neigh->ops->output;
	}
	return 0;
}

static void arp_error_report(struct neighbour *neigh, struct sk_buff *skb)
{
	dst_link_failure(skb);
	kfree_skb(skb);
}

static void arp_solicit(struct neighbour *neigh, struct sk_buff *skb)
{
	u32 saddr;
	u8  *dst_ha = NULL;
	struct net_device *dev = neigh->dev;
	u32 target = *(u32*)neigh->primary_key;
	int probes = atomic_read(&neigh->probes);

	if (skb && inet_addr_type(skb->nh.iph->saddr) == RTN_LOCAL)
		saddr = skb->nh.iph->saddr;
	else
		saddr = inet_select_addr(dev, target, RT_SCOPE_LINK);

	if ((probes -= neigh->parms->ucast_probes) < 0) {
		if (!(neigh->nud_state&NUD_VALID))
			printk(KERN_DEBUG "trying to ucast probe in NUD_INVALID\n");
		dst_ha = neigh->ha;
		read_lock_bh(&neigh->lock);
	} else if ((probes -= neigh->parms->app_probes) < 0) {
#ifdef CONFIG_ARPD
		neigh_app_ns(neigh);
#endif
		return;
	}

	arp_send(ARPOP_REQUEST, ETH_P_ARP, target, dev, saddr,
		 dst_ha, dev->dev_addr, NULL);
	if (dst_ha)
		read_unlock_bh(&neigh->lock);
}

/* OBSOLETE FUNCTIONS */

/*
 *	Find an arp mapping in the cache. If not found, post a request.
 *
 *	It is very UGLY routine: it DOES NOT use skb->dst->neighbour,
 *	even if it exists. It is supposed that skb->dev was mangled
 *	by a virtual device (eql, shaper). Nobody but broken devices
 *	is allowed to use this function, it is scheduled to be removed. --ANK
 */

static int arp_set_predefined(int addr_hint, unsigned char * haddr, u32 paddr, struct net_device * dev)
{
	switch (addr_hint) {
	case RTN_LOCAL:
		printk(KERN_DEBUG "ARP: arp called for own IP address\n");
		memcpy(haddr, dev->dev_addr, dev->addr_len);
		return 1;
	case RTN_MULTICAST:
		arp_mc_map(paddr, haddr, dev, 1);
		return 1;
	case RTN_BROADCAST:
		memcpy(haddr, dev->broadcast, dev->addr_len);
		return 1;
	}
	return 0;
}


int arp_find(unsigned char *haddr, struct sk_buff *skb)
{
	struct net_device *dev = skb->dev;
	u32 paddr;
	struct neighbour *n;

	if (!skb->dst) {
		printk(KERN_DEBUG "arp_find is called with dst==NULL\n");
		kfree_skb(skb);
		return 1;
	}

	paddr = ((struct rtable*)skb->dst)->rt_gateway;

	if (arp_set_predefined(inet_addr_type(paddr), haddr, paddr, dev))
		return 0;

	n = __neigh_lookup(&arp_tbl, &paddr, dev, 1);

	if (n) {
		n->used = jiffies;
		if (n->nud_state&NUD_VALID || neigh_event_send(n, skb) == 0) {
			read_lock_bh(&n->lock);
 			memcpy(haddr, n->ha, dev->addr_len);
			read_unlock_bh(&n->lock);
			neigh_release(n);
			return 0;
		}
		neigh_release(n);
	} else
		kfree_skb(skb);
	return 1;
}

/* END OF OBSOLETE FUNCTIONS */

int arp_bind_neighbour(struct dst_entry *dst)
{
	struct net_device *dev = dst->dev;
	struct neighbour *n = dst->neighbour;

	if (dev == NULL)
		return -EINVAL;
	if (n == NULL) {
		u32 nexthop = ((struct rtable*)dst)->rt_gateway;
		if (dev->flags&(IFF_LOOPBACK|IFF_POINTOPOINT))
			nexthop = 0;
		n = __neigh_lookup_errno(
#ifdef CONFIG_ATM_CLIP
		    dev->type == ARPHRD_ATM ? &clip_tbl :
#endif
		    &arp_tbl, &nexthop, dev);
		if (IS_ERR(n))
			return PTR_ERR(n);
		dst->neighbour = n;
	}
	return 0;
}

/*
 *	Interface to link layer: send routine and receive handler.
 */

/*
 *	Create and send an arp packet. If (dest_hw == NULL), we create a broadcast
 *	message.
 */

void arp_send(int type, int ptype, u32 dest_ip, 
	      struct net_device *dev, u32 src_ip, 
	      unsigned char *dest_hw, unsigned char *src_hw,
	      unsigned char *target_hw)
{
	struct sk_buff *skb;
	struct arphdr *arp;
	unsigned char *arp_ptr;

	/*
	 *	No arp on this interface.
	 */
	
	if (dev->flags&IFF_NOARP)
		return;

	/*
	 *	Allocate a buffer
	 */
	
	skb = alloc_skb(sizeof(struct arphdr)+ 2*(dev->addr_len+4)
				+ dev->hard_header_len + 15, GFP_ATOMIC);
	if (skb == NULL)
		return;

	skb_reserve(skb, (dev->hard_header_len+15)&~15);
	skb->nh.raw = skb->data;
	arp = (struct arphdr *) skb_put(skb,sizeof(struct arphdr) + 2*(dev->addr_len+4));
	skb->dev = dev;
	skb->protocol = __constant_htons (ETH_P_ARP);
	if (src_hw == NULL)
		src_hw = dev->dev_addr;
	if (dest_hw == NULL)
		dest_hw = dev->broadcast;

	/*
	 *	Fill the device header for the ARP frame
	 */
	if (dev->hard_header &&
	    dev->hard_header(skb,dev,ptype,dest_hw,src_hw,skb->len) < 0)
		goto out;

	/*
	 * Fill out the arp protocol part.
	 *
	 * The arp hardware type should match the device type, except for FDDI,
	 * which (according to RFC 1390) should always equal 1 (Ethernet).
	 */
	/*
	 *	Exceptions everywhere. AX.25 uses the AX.25 PID value not the
	 *	DIX code for the protocol. Make these device structure fields.
	 */
	switch (dev->type) {
	default:
		arp->ar_hrd = htons(dev->type);
		arp->ar_pro = __constant_htons(ETH_P_IP);
		break;

#if defined(CONFIG_AX25) || defined(CONFIG_AX25_MODULE)
	case ARPHRD_AX25:
		arp->ar_hrd = __constant_htons(ARPHRD_AX25);
		arp->ar_pro = __constant_htons(AX25_P_IP);
		break;

#if defined(CONFIG_NETROM) || defined(CONFIG_NETROM_MODULE)
	case ARPHRD_NETROM:
		arp->ar_hrd = __constant_htons(ARPHRD_NETROM);
		arp->ar_pro = __constant_htons(AX25_P_IP);
		break;
#endif
#endif

#ifdef CONFIG_FDDI
	case ARPHRD_FDDI:
		arp->ar_hrd = __constant_htons(ARPHRD_ETHER);
		arp->ar_pro = __constant_htons(ETH_P_IP);
		break;
#endif
#ifdef CONFIG_TR
	case ARPHRD_IEEE802_TR:
		arp->ar_hrd = __constant_htons(ARPHRD_IEEE802);
		arp->ar_pro = __constant_htons(ETH_P_IP);
		break;
#endif
	}

	arp->ar_hln = dev->addr_len;
	arp->ar_pln = 4;
	arp->ar_op = htons(type);

	arp_ptr=(unsigned char *)(arp+1);

	memcpy(arp_ptr, src_hw, dev->addr_len);
	arp_ptr+=dev->addr_len;
	memcpy(arp_ptr, &src_ip,4);
	arp_ptr+=4;
	if (target_hw != NULL)
		memcpy(arp_ptr, target_hw, dev->addr_len);
	else
		memset(arp_ptr, 0, dev->addr_len);
	arp_ptr+=dev->addr_len;
	memcpy(arp_ptr, &dest_ip, 4);
	skb->dev = dev;

	dev_queue_xmit(skb);
	return;

out:
	kfree_skb(skb);
}

static void parp_redo(struct sk_buff *skb)
{
	arp_rcv(skb, skb->dev, NULL);
}

/*
 *	Receive an arp request by the device layer.
 */

int arp_rcv(struct sk_buff *skb, struct net_device *dev, struct packet_type *pt)
{
	struct arphdr *arp = skb->nh.arph;
	unsigned char *arp_ptr= (unsigned char *)(arp+1);
	struct rtable *rt;
	unsigned char *sha, *tha;
	u32 sip, tip;
	u16 dev_type = dev->type;
	int addr_type;
	struct in_device *in_dev = in_dev_get(dev);
	struct neighbour *n;

/*
 *	The hardware length of the packet should match the hardware length
 *	of the device.  Similarly, the hardware types should match.  The
 *	device should be ARP-able.  Also, if pln is not 4, then the lookup
 *	is not from an IP number.  We can't currently handle this, so toss
 *	it. 
 */  
	if (in_dev == NULL ||
	    arp->ar_hln != dev->addr_len    || 
	    dev->flags & IFF_NOARP ||
	    skb->pkt_type == PACKET_OTHERHOST ||
	    skb->pkt_type == PACKET_LOOPBACK ||
	    arp->ar_pln != 4)
		goto out;

	if ((skb = skb_share_check(skb, GFP_ATOMIC)) == NULL)
		goto out_of_mem;

	switch (dev_type) {
	default:	
		if (arp->ar_pro != __constant_htons(ETH_P_IP))
			goto out;
		if (htons(dev_type) != arp->ar_hrd)
			goto out;
		break;
#ifdef CONFIG_NET_ETHERNET
	case ARPHRD_ETHER:
		/*
		 * ETHERNET devices will accept ARP hardware types of either
		 * 1 (Ethernet) or 6 (IEEE 802.2).
		 */
		if (arp->ar_hrd != __constant_htons(ARPHRD_ETHER) &&
		    arp->ar_hrd != __constant_htons(ARPHRD_IEEE802))
			goto out;
		if (arp->ar_pro != __constant_htons(ETH_P_IP))
			goto out;
		break;
#endif
#ifdef CONFIG_TR
	case ARPHRD_IEEE802_TR:
		/*
		 * Token ring devices will accept ARP hardware types of either
		 * 1 (Ethernet) or 6 (IEEE 802.2).
		 */
		if (arp->ar_hrd != __constant_htons(ARPHRD_ETHER) &&
		    arp->ar_hrd != __constant_htons(ARPHRD_IEEE802))
			goto out;
		if (arp->ar_pro != __constant_htons(ETH_P_IP))
			goto out;
		break;
#endif
#ifdef CONFIG_FDDI
	case ARPHRD_FDDI:
		/*
		 * According to RFC 1390, FDDI devices should accept ARP hardware types
		 * of 1 (Ethernet).  However, to be more robust, we'll accept hardware
		 * types of either 1 (Ethernet) or 6 (IEEE 802.2).
		 */
		if (arp->ar_hrd != __constant_htons(ARPHRD_ETHER) &&
		    arp->ar_hrd != __constant_htons(ARPHRD_IEEE802))
			goto out;
		if (arp->ar_pro != __constant_htons(ETH_P_IP))
			goto out;
		break;
#endif
#if defined(CONFIG_AX25) || defined(CONFIG_AX25_MODULE)
	case ARPHRD_AX25:
		if (arp->ar_pro != __constant_htons(AX25_P_IP))
			goto out;
		if (arp->ar_hrd != __constant_htons(ARPHRD_AX25))
			goto out;
		break;
#if defined(CONFIG_NETROM) || defined(CONFIG_NETROM_MODULE)
	case ARPHRD_NETROM:
		if (arp->ar_pro != __constant_htons(AX25_P_IP))
			goto out;
		if (arp->ar_hrd != __constant_htons(ARPHRD_NETROM))
			goto out;
		break;
#endif
#endif
	}

	/* Understand only these message types */

	if (arp->ar_op != __constant_htons(ARPOP_REPLY) &&
	    arp->ar_op != __constant_htons(ARPOP_REQUEST))
		goto out;

/*
 *	Extract fields
 */
	sha=arp_ptr;
	arp_ptr += dev->addr_len;
	memcpy(&sip, arp_ptr, 4);
	arp_ptr += 4;
	tha=arp_ptr;
	arp_ptr += dev->addr_len;
	memcpy(&tip, arp_ptr, 4);
/* 
 *	Check for bad requests for 127.x.x.x and requests for multicast
 *	addresses.  If this is one such, delete it.
 */
	if (LOOPBACK(tip) || MULTICAST(tip))
		goto out;

/*
 *  Process entry.  The idea here is we want to send a reply if it is a
 *  request for us or if it is a request for someone else that we hold
 *  a proxy for.  We want to add an entry to our cache if it is a reply
 *  to us or if it is a request for our address.  
 *  (The assumption for this last is that if someone is requesting our 
 *  address, they are probably intending to talk to us, so it saves time 
 *  if we cache their address.  Their address is also probably not in 
 *  our cache, since ours is not in their cache.)
 * 
 *  Putting this another way, we only care about replies if they are to
 *  us, in which case we add them to the cache.  For requests, we care
 *  about those for us and those for our proxies.  We reply to both,
 *  and in the case of requests for us we add the requester to the arp 
 *  cache.
 */

	/* Special case: IPv4 duplicate address detection packet (RFC2131) */
	if (sip == 0) {
		if (arp->ar_op == __constant_htons(ARPOP_REQUEST) &&
		    inet_addr_type(tip) == RTN_LOCAL)
			arp_send(ARPOP_REPLY,ETH_P_ARP,tip,dev,tip,sha,dev->dev_addr,dev->dev_addr);
		goto out;
	}

	if (arp->ar_op == __constant_htons(ARPOP_REQUEST) &&
	    ip_route_input(skb, tip, sip, 0, dev) == 0) {

		rt = (struct rtable*)skb->dst;
		addr_type = rt->rt_type;

		if (addr_type == RTN_LOCAL) {
			n = neigh_event_ns(&arp_tbl, sha, &sip, dev);
			if (n) {
				arp_send(ARPOP_REPLY,ETH_P_ARP,sip,dev,tip,sha,dev->dev_addr,sha);
				neigh_release(n);
			}
			goto out;
		} else if (IN_DEV_FORWARD(in_dev)) {
			if ((rt->rt_flags&RTCF_DNAT) ||
			    (addr_type == RTN_UNICAST  && rt->u.dst.dev != dev &&
			     (IN_DEV_PROXY_ARP(in_dev) || pneigh_lookup(&arp_tbl, &tip, dev, 0)))) {
				n = neigh_event_ns(&arp_tbl, sha, &sip, dev);
				if (n)
					neigh_release(n);

				if (skb->stamp.tv_sec == 0 ||
				    skb->pkt_type == PACKET_HOST ||
				    in_dev->arp_parms->proxy_delay == 0) {
					arp_send(ARPOP_REPLY,ETH_P_ARP,sip,dev,tip,sha,dev->dev_addr,sha);
				} else {
					pneigh_enqueue(&arp_tbl, in_dev->arp_parms, skb);
					in_dev_put(in_dev);
					return 0;
				}
				goto out;
			}
		}
	}

	/* Update our ARP tables */

	n = __neigh_lookup(&arp_tbl, &sip, dev, 0);

#ifdef CONFIG_IP_ACCEPT_UNSOLICITED_ARP
	/* Unsolicited ARP is not accepted by default.
	   It is possible, that this option should be enabled for some
	   devices (strip is candidate)
	 */
	if (n == NULL &&
	    arp->ar_op == __constant_htons(ARPOP_REPLY) &&
	    inet_addr_type(sip) == RTN_UNICAST)
		n = __neigh_lookup(&arp_tbl, &sip, dev, -1);
#endif

	if (n) {
		int state = NUD_REACHABLE;
		int override = 0;

		/* If several different ARP replies follows back-to-back,
		   use the FIRST one. It is possible, if several proxy
		   agents are active. Taking the first reply prevents
		   arp trashing and chooses the fastest router.
		 */
		if (jiffies - n->updated >= n->parms->locktime)
			override = 1;

		/* Broadcast replies and request packets
		   do not assert neighbour reachability.
		 */
		if (arp->ar_op != __constant_htons(ARPOP_REPLY) ||
		    skb->pkt_type != PACKET_HOST)
			state = NUD_STALE;
		neigh_update(n, sha, state, override, 1);
		neigh_release(n);
	}

out:
	kfree_skb(skb);
	if (in_dev)
		in_dev_put(in_dev);
out_of_mem:
	return 0;
}



/*
 *	User level interface (ioctl, /proc)
 */

/*
 *	Set (create) an ARP cache entry.
 */

int arp_req_set(struct arpreq *r, struct net_device * dev)
{
	u32 ip = ((struct sockaddr_in *) &r->arp_pa)->sin_addr.s_addr;
	struct neighbour *neigh;
	int err;

	if (r->arp_flags&ATF_PUBL) {
		u32 mask = ((struct sockaddr_in *) &r->arp_netmask)->sin_addr.s_addr;
		if (mask && mask != 0xFFFFFFFF)
			return -EINVAL;
		if (!dev && (r->arp_flags & ATF_COM)) {
			dev = dev_getbyhwaddr(r->arp_ha.sa_family, r->arp_ha.sa_data);
			if (!dev)
				return -ENODEV;
		}
		if (mask) {
			if (pneigh_lookup(&arp_tbl, &ip, dev, 1) == NULL)
				return -ENOBUFS;
			return 0;
		}
		if (dev == NULL) {
			ipv4_devconf.proxy_arp = 1;
			return 0;
		}
		if (__in_dev_get(dev)) {
			__in_dev_get(dev)->cnf.proxy_arp = 1;
			return 0;
		}
		return -ENXIO;
	}

	if (r->arp_flags & ATF_PERM)
		r->arp_flags |= ATF_COM;
	if (dev == NULL) {
		struct rtable * rt;
		if ((err = ip_route_output(&rt, ip, 0, RTO_ONLINK, 0)) != 0)
			return err;
		dev = rt->u.dst.dev;
		ip_rt_put(rt);
		if (!dev)
			return -EINVAL;
	}
	if (r->arp_ha.sa_family != dev->type)	
		return -EINVAL;

	neigh = __neigh_lookup_errno(&arp_tbl, &ip, dev);
	err = PTR_ERR(neigh);
	if (!IS_ERR(neigh)) {
		unsigned state = NUD_STALE;
		if (r->arp_flags & ATF_PERM)
			state = NUD_PERMANENT;
		err = neigh_update(neigh, (r->arp_flags&ATF_COM) ?
				   r->arp_ha.sa_data : NULL, state, 1, 0);
		neigh_release(neigh);
	}
	return err;
}

static unsigned arp_state_to_flags(struct neighbour *neigh)
{
	unsigned flags = 0;
	if (neigh->nud_state&NUD_PERMANENT)
		flags = ATF_PERM|ATF_COM;
	else if (neigh->nud_state&NUD_VALID)
		flags = ATF_COM;
	return flags;
}

/*
 *	Get an ARP cache entry.
 */

static int arp_req_get(struct arpreq *r, struct net_device *dev)
{
	u32 ip = ((struct sockaddr_in *) &r->arp_pa)->sin_addr.s_addr;
	struct neighbour *neigh;
	int err = -ENXIO;

	neigh = neigh_lookup(&arp_tbl, &ip, dev);
	if (neigh) {
		read_lock_bh(&neigh->lock);
		memcpy(r->arp_ha.sa_data, neigh->ha, dev->addr_len);
		r->arp_flags = arp_state_to_flags(neigh);
		read_unlock_bh(&neigh->lock);
		r->arp_ha.sa_family = dev->type;
		strncpy(r->arp_dev, dev->name, sizeof(r->arp_dev));
		neigh_release(neigh);
		err = 0;
	}
	return err;
}

int arp_req_delete(struct arpreq *r, struct net_device * dev)
{
	int err;
	u32 ip = ((struct sockaddr_in *)&r->arp_pa)->sin_addr.s_addr;
	struct neighbour *neigh;

	if (r->arp_flags & ATF_PUBL) {
		u32 mask = ((struct sockaddr_in *) &r->arp_netmask)->sin_addr.s_addr;
		if (mask == 0xFFFFFFFF)
			return pneigh_delete(&arp_tbl, &ip, dev);
		if (mask == 0) {
			if (dev == NULL) {
				ipv4_devconf.proxy_arp = 0;
				return 0;
			}
			if (__in_dev_get(dev)) {
				__in_dev_get(dev)->cnf.proxy_arp = 0;
				return 0;
			}
			return -ENXIO;
		}
		return -EINVAL;
	}

	if (dev == NULL) {
		struct rtable * rt;
		if ((err = ip_route_output(&rt, ip, 0, RTO_ONLINK, 0)) != 0)
			return err;
		dev = rt->u.dst.dev;
		ip_rt_put(rt);
		if (!dev)
			return -EINVAL;
	}
	err = -ENXIO;
	neigh = neigh_lookup(&arp_tbl, &ip, dev);
	if (neigh) {
		if (neigh->nud_state&~NUD_NOARP)
			err = neigh_update(neigh, NULL, NUD_FAILED, 1, 0);
		neigh_release(neigh);
	}
	return err;
}

/*
 *	Handle an ARP layer I/O control request.
 */

int arp_ioctl(unsigned int cmd, void *arg)
{
	int err;
	struct arpreq r;
	struct net_device * dev = NULL;

	switch(cmd) {
		case SIOCDARP:
		case SIOCSARP:
			if (!capable(CAP_NET_ADMIN))
				return -EPERM;
		case SIOCGARP:
			err = copy_from_user(&r, arg, sizeof(struct arpreq));
			if (err)
				return -EFAULT;
			break;
		default:
			return -EINVAL;
	}

	if (r.arp_pa.sa_family != AF_INET)
		return -EPFNOSUPPORT;

	if (!(r.arp_flags & ATF_PUBL) &&
	    (r.arp_flags & (ATF_NETMASK|ATF_DONTPUB)))
		return -EINVAL;
	if (!(r.arp_flags & ATF_NETMASK))
		((struct sockaddr_in *)&r.arp_netmask)->sin_addr.s_addr=__constant_htonl(0xFFFFFFFFUL);

	rtnl_lock();
	if (r.arp_dev[0]) {
		err = -ENODEV;
		if ((dev = __dev_get_by_name(r.arp_dev)) == NULL)
			goto out;

		/* Mmmm... It is wrong... ARPHRD_NETROM==0 */
		if (!r.arp_ha.sa_family)
			r.arp_ha.sa_family = dev->type;
		err = -EINVAL;
		if ((r.arp_flags & ATF_COM) && r.arp_ha.sa_family != dev->type)
			goto out;
	} else if (cmd == SIOCGARP) {
		err = -ENODEV;
		goto out;
	}

	switch(cmd) {
	case SIOCDARP:
	        err = arp_req_delete(&r, dev);
		break;
	case SIOCSARP:
		err = arp_req_set(&r, dev);
		break;
	case SIOCGARP:
		err = arp_req_get(&r, dev);
		if (!err && copy_to_user(arg, &r, sizeof(r)))
			err = -EFAULT;
		break;
	}
out:
	rtnl_unlock();
	return err;
}

/*
 *	Write the contents of the ARP cache to a PROCfs file.
 */
#ifndef CONFIG_PROC_FS
static int arp_get_info(char *buffer, char **start, off_t offset, int length) { return 0; }
#else
#if defined(CONFIG_AX25) || defined(CONFIG_AX25_MODULE)
static char *ax2asc2(ax25_address *a, char *buf);
#endif
#define HBUFFERLEN 30

static int arp_get_info(char *buffer, char **start, off_t offset, int length)
{
	int len=0;
	off_t pos=0;
	int size;
	char hbuffer[HBUFFERLEN];
	int i,j,k;
	const char hexbuf[] =  "0123456789ABCDEF";

	size = sprintf(buffer,"IP address       HW type     Flags       HW address            Mask     Device\n");

	pos+=size;
	len+=size;

	for(i=0; i<=NEIGH_HASHMASK; i++) {
		struct neighbour *n;
		read_lock_bh(&arp_tbl.lock);
		for (n=arp_tbl.hash_buckets[i]; n; n=n->next) {
			struct net_device *dev = n->dev;
			int hatype = dev->type;

			/* Do not confuse users "arp -a" with magic entries */
			if (!(n->nud_state&~NUD_NOARP))
				continue;

			read_lock(&n->lock);

/*
 *	Convert hardware address to XX:XX:XX:XX ... form.
 */
#if defined(CONFIG_AX25) || defined(CONFIG_AX25_MODULE)
			if (hatype == ARPHRD_AX25 || hatype == ARPHRD_NETROM)
				ax2asc2((ax25_address *)n->ha, hbuffer);
			else {
#endif
			for (k=0,j=0;k<HBUFFERLEN-3 && j<dev->addr_len;j++) {
				hbuffer[k++]=hexbuf[(n->ha[j]>>4)&15 ];
				hbuffer[k++]=hexbuf[n->ha[j]&15     ];
				hbuffer[k++]=':';
			}
			hbuffer[--k]=0;

#if defined(CONFIG_AX25) || defined(CONFIG_AX25_MODULE)
		}
#endif

			{
				char tbuf[16];
				sprintf(tbuf, "%u.%u.%u.%u", NIPQUAD(*(u32*)n->primary_key));
				size = sprintf(buffer+len, "%-16s 0x%-10x0x%-10x%s"
							"     *        %s\n",
					tbuf,
					hatype,
					arp_state_to_flags(n), 
					hbuffer,
					dev->name);
			}

			read_unlock(&n->lock);

			len += size;
			pos += size;
		  
			if (pos <= offset)
				len=0;
			if (pos >= offset+length) {
				read_unlock_bh(&arp_tbl.lock);
 				goto done;
			}
		}
		read_unlock_bh(&arp_tbl.lock);
	}

	for (i=0; i<=PNEIGH_HASHMASK; i++) {
		struct pneigh_entry *n;
		for (n=arp_tbl.phash_buckets[i]; n; n=n->next) {
			struct net_device *dev = n->dev;
			int hatype = dev ? dev->type : 0;

			{
				char tbuf[16];
				sprintf(tbuf, "%u.%u.%u.%u", NIPQUAD(*(u32*)n->key));
				size = sprintf(buffer+len, "%-16s 0x%-10x0x%-10x%s"
							"     *        %s\n",
					tbuf,
					hatype,
 					ATF_PUBL|ATF_PERM,
					"00:00:00:00:00:00",
					dev ? dev->name : "*");
			}

			len += size;
			pos += size;
		  
			if (pos <= offset)
				len=0;
			if (pos >= offset+length)
				goto done;
		}
	}

done:
  
	*start = buffer+len-(pos-offset);	/* Start of wanted data */
	len = pos-offset;			/* Start slop */
	if (len>length)
		len = length;			/* Ending slop */
	if (len<0)
		len = 0;
	return len;
}
#endif

/* Note, that it is not on notifier chain.
   It is necessary, that this routine was called after route cache will be
   flushed.
 */
void arp_ifdown(struct net_device *dev)
{
	neigh_ifdown(&arp_tbl, dev);
}


/*
 *	Called once on startup.
 */

static struct packet_type arp_packet_type =
{
	__constant_htons(ETH_P_ARP),
	NULL,		/* All devices */
	arp_rcv,
	(void*)1,
	NULL
};

void __init arp_init (void)
{
	neigh_table_init(&arp_tbl);

	dev_add_pack(&arp_packet_type);

	proc_net_create ("arp", 0, arp_get_info);

#ifdef CONFIG_SYSCTL
	neigh_sysctl_register(NULL, &arp_tbl.parms, NET_IPV4, NET_IPV4_NEIGH, "ipv4");
#endif
}


#ifdef CONFIG_PROC_FS
#if defined(CONFIG_AX25) || defined(CONFIG_AX25_MODULE)

/*
 *	ax25 -> ASCII conversion
 */
char *ax2asc2(ax25_address *a, char *buf)
{
	char c, *s;
	int n;

	for (n = 0, s = buf; n < 6; n++) {
		c = (a->ax25_call[n] >> 1) & 0x7F;

		if (c != ' ') *s++ = c;
	}
	
	*s++ = '-';

	if ((n = ((a->ax25_call[6] >> 1) & 0x0F)) > 9) {
		*s++ = '1';
		n -= 10;
	}
	
	*s++ = n + '0';
	*s++ = '\0';

	if (*buf == '\0' || *buf == '-')
	   return "*";

	return buf;

}

#endif
#endif
