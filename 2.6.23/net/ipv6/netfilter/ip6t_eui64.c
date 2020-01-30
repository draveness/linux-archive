/* Kernel module to match EUI64 address parameters. */

/* (C) 2001-2002 Andras Kis-Szabo <kisza@sch.bme.hu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/ipv6.h>
#include <linux/if_ether.h>

#include <linux/netfilter/x_tables.h>
#include <linux/netfilter_ipv6/ip6_tables.h>

MODULE_DESCRIPTION("IPv6 EUI64 address checking match");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andras Kis-Szabo <kisza@sch.bme.hu>");

static bool
match(const struct sk_buff *skb,
      const struct net_device *in,
      const struct net_device *out,
      const struct xt_match *match,
      const void *matchinfo,
      int offset,
      unsigned int protoff,
      bool *hotdrop)
{
	unsigned char eui64[8];
	int i = 0;

	if (!(skb_mac_header(skb) >= skb->head &&
	      skb_mac_header(skb) + ETH_HLEN <= skb->data) &&
	    offset != 0) {
		*hotdrop = true;
		return false;
	}

	memset(eui64, 0, sizeof(eui64));

	if (eth_hdr(skb)->h_proto == htons(ETH_P_IPV6)) {
		if (ipv6_hdr(skb)->version == 0x6) {
			memcpy(eui64, eth_hdr(skb)->h_source, 3);
			memcpy(eui64 + 5, eth_hdr(skb)->h_source + 3, 3);
			eui64[3] = 0xff;
			eui64[4] = 0xfe;
			eui64[0] |= 0x02;

			i = 0;
			while (ipv6_hdr(skb)->saddr.s6_addr[8 + i] == eui64[i]
			       && i < 8)
				i++;

			if (i == 8)
				return true;
		}
	}

	return false;
}

static struct xt_match eui64_match __read_mostly = {
	.name		= "eui64",
	.family		= AF_INET6,
	.match		= match,
	.matchsize	= sizeof(int),
	.hooks		= (1 << NF_IP6_PRE_ROUTING) | (1 << NF_IP6_LOCAL_IN) |
			  (1 << NF_IP6_FORWARD),
	.me		= THIS_MODULE,
};

static int __init ip6t_eui64_init(void)
{
	return xt_register_match(&eui64_match);
}

static void __exit ip6t_eui64_fini(void)
{
	xt_unregister_match(&eui64_match);
}

module_init(ip6t_eui64_init);
module_exit(ip6t_eui64_fini);
