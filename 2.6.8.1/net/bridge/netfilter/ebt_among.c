/*
 *  ebt_among
 *
 *	Authors:
 *	Grzegorz Borowiak <grzes@gnu.univ.gda.pl>
 *
 *  August, 2003
 *
 */

#include <linux/netfilter_bridge/ebtables.h>
#include <linux/netfilter_bridge/ebt_among.h>
#include <linux/ip.h>
#include <linux/if_arp.h>
#include <linux/module.h>

static int ebt_mac_wormhash_contains(const struct ebt_mac_wormhash *wh,
				     const char *mac, uint32_t ip)
{
	/* You may be puzzled as to how this code works.
	 * Some tricks were used, refer to 
	 * 	include/linux/netfilter_bridge/ebt_among.h
	 * as there you can find a solution of this mystery.
	 */
	const struct ebt_mac_wormhash_tuple *p;
	int start, limit, i;
	uint32_t cmp[2] = { 0, 0 };
	int key = (const unsigned char) mac[5];

	memcpy(((char *) cmp) + 2, mac, 6);
	start = wh->table[key];
	limit = wh->table[key + 1];
	if (ip) {
		for (i = start; i < limit; i++) {
			p = &wh->pool[i];
			if (cmp[1] == p->cmp[1] && cmp[0] == p->cmp[0]) {
				if (p->ip == 0 || p->ip == ip) {
					return 1;
				}
			}
		}
	} else {
		for (i = start; i < limit; i++) {
			p = &wh->pool[i];
			if (cmp[1] == p->cmp[1] && cmp[0] == p->cmp[0]) {
				if (p->ip == 0) {
					return 1;
				}
			}
		}
	}
	return 0;
}

static int ebt_mac_wormhash_check_integrity(const struct ebt_mac_wormhash
					    *wh)
{
	int i;

	for (i = 0; i < 256; i++) {
		if (wh->table[i] > wh->table[i + 1])
			return -0x100 - i;
		if (wh->table[i] < 0)
			return -0x200 - i;
		if (wh->table[i] > wh->poolsize)
			return -0x300 - i;
	}
	if (wh->table[256] > wh->poolsize)
		return -0xc00;
	return 0;
}

static int get_ip_dst(const struct sk_buff *skb, uint32_t *addr)
{
	if (skb->mac.ethernet->h_proto == __constant_htons(ETH_P_IP)) {
		struct iphdr iph;

		if (skb_copy_bits(skb, 0, &iph, sizeof(iph)))
			return -1;
		*addr = iph.daddr;
	} else if (skb->mac.ethernet->h_proto == __constant_htons(ETH_P_ARP)) {
		struct arphdr arph;

		if (skb_copy_bits(skb, 0, &arph, sizeof(arph)) ||
		    arph.ar_pln != sizeof(uint32_t) || arph.ar_hln != ETH_ALEN)
			return -1;
		if (skb_copy_bits(skb, sizeof(struct arphdr) +
		    2 * ETH_ALEN + sizeof(uint32_t), addr, sizeof(uint32_t)))
			return -1;
	}
	return 0;
}

static int get_ip_src(const struct sk_buff *skb, uint32_t *addr)
{
	if (skb->mac.ethernet->h_proto == __constant_htons(ETH_P_IP)) {
		struct iphdr iph;

		if (skb_copy_bits(skb, 0, &iph, sizeof(iph)))
			return -1;
		*addr = iph.saddr;
	} else if (skb->mac.ethernet->h_proto == __constant_htons(ETH_P_ARP)) {
		struct arphdr arph;

		if (skb_copy_bits(skb, 0, &arph, sizeof(arph)) ||
		    arph.ar_pln != sizeof(uint32_t) || arph.ar_hln != ETH_ALEN)
			return -1;
		if (skb_copy_bits(skb, sizeof(struct arphdr) +
		    ETH_ALEN, addr, sizeof(uint32_t)))
			return -1;
	}
	return 0;
}

static int ebt_filter_among(const struct sk_buff *skb,
			    const struct net_device *in,
			    const struct net_device *out, const void *data,
			    unsigned int datalen)
{
	struct ebt_among_info *info = (struct ebt_among_info *) data;
	const char *dmac, *smac;
	const struct ebt_mac_wormhash *wh_dst, *wh_src;
	uint32_t dip = 0, sip = 0;

	wh_dst = ebt_among_wh_dst(info);
	wh_src = ebt_among_wh_src(info);

	if (wh_src) {
		smac = skb->mac.ethernet->h_source;
		if (get_ip_src(skb, &sip))
			return EBT_NOMATCH;
		if (!(info->bitmask & EBT_AMONG_SRC_NEG)) {
			/* we match only if it contains */
			if (!ebt_mac_wormhash_contains(wh_src, smac, sip))
				return EBT_NOMATCH;
		} else {
			/* we match only if it DOES NOT contain */
			if (ebt_mac_wormhash_contains(wh_src, smac, sip))
				return EBT_NOMATCH;
		}
	}

	if (wh_dst) {
		dmac = skb->mac.ethernet->h_dest;
		if (get_ip_dst(skb, &dip))
			return EBT_NOMATCH;
		if (!(info->bitmask & EBT_AMONG_DST_NEG)) {
			/* we match only if it contains */
			if (!ebt_mac_wormhash_contains(wh_dst, dmac, dip))
				return EBT_NOMATCH;
		} else {
			/* we match only if it DOES NOT contain */
			if (ebt_mac_wormhash_contains(wh_dst, dmac, dip))
				return EBT_NOMATCH;
		}
	}

	return EBT_MATCH;
}

static int ebt_among_check(const char *tablename, unsigned int hookmask,
			   const struct ebt_entry *e, void *data,
			   unsigned int datalen)
{
	struct ebt_among_info *info = (struct ebt_among_info *) data;
	int expected_length = sizeof(struct ebt_among_info);
	const struct ebt_mac_wormhash *wh_dst, *wh_src;
	int err;

	wh_dst = ebt_among_wh_dst(info);
	wh_src = ebt_among_wh_src(info);
	expected_length += ebt_mac_wormhash_size(wh_dst);
	expected_length += ebt_mac_wormhash_size(wh_src);

	if (datalen != EBT_ALIGN(expected_length)) {
		printk(KERN_WARNING
		       "ebtables: among: wrong size: %d"
		       "against expected %d, rounded to %Zd\n",
		       datalen, expected_length,
		       EBT_ALIGN(expected_length));
		return -EINVAL;
	}
	if (wh_dst && (err = ebt_mac_wormhash_check_integrity(wh_dst))) {
		printk(KERN_WARNING
		       "ebtables: among: dst integrity fail: %x\n", -err);
		return -EINVAL;
	}
	if (wh_src && (err = ebt_mac_wormhash_check_integrity(wh_src))) {
		printk(KERN_WARNING
		       "ebtables: among: src integrity fail: %x\n", -err);
		return -EINVAL;
	}
	return 0;
}

static struct ebt_match filter_among = {
	.name		= EBT_AMONG_MATCH, 
	.match		= ebt_filter_among, 
	.check		= ebt_among_check,
	.me		= THIS_MODULE,
};

static int __init init(void)
{
	return ebt_register_match(&filter_among);
}

static void __exit fini(void)
{
	ebt_unregister_match(&filter_among);
}

module_init(init);
module_exit(fini);
MODULE_LICENSE("GPL");
