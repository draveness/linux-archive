/* Kernel module to match AH parameters. */

/* (C) 2001-2002 Andras Kis-Szabo <kisza@sch.bme.hu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/types.h>
#include <net/checksum.h>
#include <net/ipv6.h>

#include <linux/netfilter/x_tables.h>
#include <linux/netfilter_ipv6/ip6_tables.h>
#include <linux/netfilter_ipv6/ip6t_ah.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("IPv6 AH match");
MODULE_AUTHOR("Andras Kis-Szabo <kisza@sch.bme.hu>");

/* Returns 1 if the spi is matched by the range, 0 otherwise */
static inline bool
spi_match(u_int32_t min, u_int32_t max, u_int32_t spi, bool invert)
{
	bool r;

	pr_debug("ah spi_match:%c 0x%x <= 0x%x <= 0x%x",
		 invert ? '!' : ' ', min, spi, max);
	r = (spi >= min && spi <= max) ^ invert;
	pr_debug(" result %s\n", r ? "PASS" : "FAILED");
	return r;
}

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
	struct ip_auth_hdr _ah;
	const struct ip_auth_hdr *ah;
	const struct ip6t_ah *ahinfo = matchinfo;
	unsigned int ptr;
	unsigned int hdrlen = 0;
	int err;

	err = ipv6_find_hdr(skb, &ptr, NEXTHDR_AUTH, NULL);
	if (err < 0) {
		if (err != -ENOENT)
			*hotdrop = true;
		return false;
	}

	ah = skb_header_pointer(skb, ptr, sizeof(_ah), &_ah);
	if (ah == NULL) {
		*hotdrop = true;
		return false;
	}

	hdrlen = (ah->hdrlen + 2) << 2;

	pr_debug("IPv6 AH LEN %u %u ", hdrlen, ah->hdrlen);
	pr_debug("RES %04X ", ah->reserved);
	pr_debug("SPI %u %08X\n", ntohl(ah->spi), ntohl(ah->spi));

	pr_debug("IPv6 AH spi %02X ",
		 spi_match(ahinfo->spis[0], ahinfo->spis[1],
			   ntohl(ah->spi),
			   !!(ahinfo->invflags & IP6T_AH_INV_SPI)));
	pr_debug("len %02X %04X %02X ",
		 ahinfo->hdrlen, hdrlen,
		 (!ahinfo->hdrlen ||
		  (ahinfo->hdrlen == hdrlen) ^
		  !!(ahinfo->invflags & IP6T_AH_INV_LEN)));
	pr_debug("res %02X %04X %02X\n",
		 ahinfo->hdrres, ah->reserved,
		 !(ahinfo->hdrres && ah->reserved));

	return (ah != NULL)
	       &&
	       spi_match(ahinfo->spis[0], ahinfo->spis[1],
			 ntohl(ah->spi),
			 !!(ahinfo->invflags & IP6T_AH_INV_SPI))
	       &&
	       (!ahinfo->hdrlen ||
		(ahinfo->hdrlen == hdrlen) ^
		!!(ahinfo->invflags & IP6T_AH_INV_LEN))
	       &&
	       !(ahinfo->hdrres && ah->reserved);
}

/* Called when user tries to insert an entry of this type. */
static bool
checkentry(const char *tablename,
	  const void *entry,
	  const struct xt_match *match,
	  void *matchinfo,
	  unsigned int hook_mask)
{
	const struct ip6t_ah *ahinfo = matchinfo;

	if (ahinfo->invflags & ~IP6T_AH_INV_MASK) {
		pr_debug("ip6t_ah: unknown flags %X\n", ahinfo->invflags);
		return false;
	}
	return true;
}

static struct xt_match ah_match __read_mostly = {
	.name		= "ah",
	.family		= AF_INET6,
	.match		= match,
	.matchsize	= sizeof(struct ip6t_ah),
	.checkentry	= checkentry,
	.me		= THIS_MODULE,
};

static int __init ip6t_ah_init(void)
{
	return xt_register_match(&ah_match);
}

static void __exit ip6t_ah_fini(void)
{
	xt_unregister_match(&ah_match);
}

module_init(ip6t_ah_init);
module_exit(ip6t_ah_fini);
