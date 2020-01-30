/*
 *	IPv6 input
 *	Linux INET6 implementation 
 *
 *	Authors:
 *	Pedro Roque		<roque@di.fc.ul.pt>
 *	Ian P. Morris		<I.P.Morris@soton.ac.uk>
 *
 *	$Id: ip6_input.c,v 1.19 2000/12/13 18:31:50 davem Exp $
 *
 *	Based in linux/net/ipv4/ip_input.c
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */
/* Changes
 *
 * 	Mitsuru KANDA @USAGI and
 * 	YOSHIFUJI Hideaki @USAGI: Remove ipv6_parse_exthdrs().
 */

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/sched.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/in6.h>
#include <linux/icmpv6.h>

#include <linux/netfilter.h>
#include <linux/netfilter_ipv6.h>

#include <net/sock.h>
#include <net/snmp.h>

#include <net/ipv6.h>
#include <net/protocol.h>
#include <net/transp_v6.h>
#include <net/rawv6.h>
#include <net/ndisc.h>
#include <net/ip6_route.h>
#include <net/addrconf.h>
#include <net/xfrm.h>



static inline int ip6_rcv_finish( struct sk_buff *skb) 
{
	if (skb->dst == NULL)
		ip6_route_input(skb);

	return dst_input(skb);
}

int ipv6_rcv(struct sk_buff *skb, struct net_device *dev, struct packet_type *pt)
{
	struct ipv6hdr *hdr;
	u32 		pkt_len;

	if (skb->pkt_type == PACKET_OTHERHOST)
		goto drop;

	IP6_INC_STATS_BH(IPSTATS_MIB_INRECEIVES);

	if ((skb = skb_share_check(skb, GFP_ATOMIC)) == NULL) {
		IP6_INC_STATS_BH(IPSTATS_MIB_INDISCARDS);
		goto out;
	}

	/* Store incoming device index. When the packet will
	   be queued, we cannot refer to skb->dev anymore.
	 */
	IP6CB(skb)->iif = dev->ifindex;

	if (skb->len < sizeof(struct ipv6hdr))
		goto err;

	if (!pskb_may_pull(skb, sizeof(struct ipv6hdr))) {
		IP6_INC_STATS_BH(IPSTATS_MIB_INHDRERRORS);
		goto drop;
	}

	hdr = skb->nh.ipv6h;

	if (hdr->version != 6)
		goto err;

	pkt_len = ntohs(hdr->payload_len);

	/* pkt_len may be zero if Jumbo payload option is present */
	if (pkt_len || hdr->nexthdr != NEXTHDR_HOP) {
		if (pkt_len + sizeof(struct ipv6hdr) > skb->len)
			goto truncated;
		if (pkt_len + sizeof(struct ipv6hdr) < skb->len) {
			if (__pskb_trim(skb, pkt_len + sizeof(struct ipv6hdr))){
				IP6_INC_STATS_BH(IPSTATS_MIB_INHDRERRORS);
				goto drop;
			}
			hdr = skb->nh.ipv6h;
			if (skb->ip_summed == CHECKSUM_HW)
				skb->ip_summed = CHECKSUM_NONE;
		}
	}

	if (hdr->nexthdr == NEXTHDR_HOP) {
		skb->h.raw = (u8*)(hdr+1);
		if (ipv6_parse_hopopts(skb, offsetof(struct ipv6hdr, nexthdr)) < 0) {
			IP6_INC_STATS_BH(IPSTATS_MIB_INHDRERRORS);
			return 0;
		}
		hdr = skb->nh.ipv6h;
	}

	return NF_HOOK(PF_INET6,NF_IP6_PRE_ROUTING, skb, dev, NULL, ip6_rcv_finish);
truncated:
	IP6_INC_STATS_BH(IPSTATS_MIB_INTRUNCATEDPKTS);
err:
	IP6_INC_STATS_BH(IPSTATS_MIB_INHDRERRORS);
drop:
	kfree_skb(skb);
out:
	return 0;
}

/*
 *	Deliver the packet to the host
 */


static inline int ip6_input_finish(struct sk_buff *skb)
{
	struct inet6_protocol *ipprot;
	struct sock *raw_sk;
	unsigned int nhoff;
	int nexthdr;
	u8 hash;
	int cksum_sub = 0;

	skb->h.raw = skb->nh.raw + sizeof(struct ipv6hdr);

	/*
	 *	Parse extension headers
	 */

	nexthdr = skb->nh.ipv6h->nexthdr;
	nhoff = offsetof(struct ipv6hdr, nexthdr);

	/* Skip hop-by-hop options, they are already parsed. */
	if (nexthdr == NEXTHDR_HOP) {
		nhoff = sizeof(struct ipv6hdr);
		nexthdr = skb->h.raw[0];
		skb->h.raw += (skb->h.raw[1]+1)<<3;
	}

	rcu_read_lock();
resubmit:
	if (!pskb_pull(skb, skb->h.raw - skb->data))
		goto discard;
	nexthdr = skb->nh.raw[nhoff];

	raw_sk = sk_head(&raw_v6_htable[nexthdr & (MAX_INET_PROTOS - 1)]);
	if (raw_sk)
		ipv6_raw_deliver(skb, nexthdr);

	hash = nexthdr & (MAX_INET_PROTOS - 1);
	if ((ipprot = inet6_protos[hash]) != NULL) {
		int ret;
		
		smp_read_barrier_depends();
		if (ipprot->flags & INET6_PROTO_FINAL) {
			struct ipv6hdr *hdr;	

			if (!cksum_sub && skb->ip_summed == CHECKSUM_HW) {
				skb->csum = csum_sub(skb->csum,
						     csum_partial(skb->nh.raw, skb->h.raw-skb->nh.raw, 0));
				cksum_sub++;
			}
			hdr = skb->nh.ipv6h;
			if (ipv6_addr_is_multicast(&hdr->daddr) &&
			    !ipv6_chk_mcast_addr(skb->dev, &hdr->daddr,
			    &hdr->saddr) &&
			    !ipv6_is_mld(skb, nexthdr))
				goto discard;
		}
		if (!(ipprot->flags & INET6_PROTO_NOPOLICY) &&
		    !xfrm6_policy_check(NULL, XFRM_POLICY_IN, skb)) 
			goto discard;
		
		ret = ipprot->handler(&skb, &nhoff);
		if (ret > 0)
			goto resubmit;
		else if (ret == 0)
			IP6_INC_STATS_BH(IPSTATS_MIB_INDELIVERS);
	} else {
		if (!raw_sk) {
			if (xfrm6_policy_check(NULL, XFRM_POLICY_IN, skb)) {
				IP6_INC_STATS_BH(IPSTATS_MIB_INUNKNOWNPROTOS);
				icmpv6_param_prob(skb, ICMPV6_UNK_NEXTHDR, nhoff);
			}
		} else {
			IP6_INC_STATS_BH(IPSTATS_MIB_INDELIVERS);
			kfree_skb(skb);
		}
	}
	rcu_read_unlock();
	return 0;

discard:
	IP6_INC_STATS_BH(IPSTATS_MIB_INDISCARDS);
	rcu_read_unlock();
	kfree_skb(skb);
	return 0;
}


int ip6_input(struct sk_buff *skb)
{
	return NF_HOOK(PF_INET6,NF_IP6_LOCAL_IN, skb, skb->dev, NULL, ip6_input_finish);
}

int ip6_mc_input(struct sk_buff *skb)
{
	struct ipv6hdr *hdr;
	int deliver;

	IP6_INC_STATS_BH(IPSTATS_MIB_INMCASTPKTS);

	hdr = skb->nh.ipv6h;
	deliver = likely(!(skb->dev->flags & (IFF_PROMISC|IFF_ALLMULTI))) ||
	    ipv6_chk_mcast_addr(skb->dev, &hdr->daddr, NULL);

	/*
	 *	IPv6 multicast router mode isnt currently supported.
	 */
#if 0
	if (ipv6_config.multicast_route) {
		int addr_type;

		addr_type = ipv6_addr_type(&hdr->daddr);

		if (!(addr_type & (IPV6_ADDR_LOOPBACK | IPV6_ADDR_LINKLOCAL))) {
			struct sk_buff *skb2;
			struct dst_entry *dst;

			dst = skb->dst;
			
			if (deliver) {
				skb2 = skb_clone(skb, GFP_ATOMIC);
				dst_output(skb2);
			} else {
				dst_output(skb);
				return 0;
			}
		}
	}
#endif

	if (likely(deliver)) {
		ip6_input(skb);
		return 0;
	}
	/* discard */
	kfree_skb(skb);

	return 0;
}
