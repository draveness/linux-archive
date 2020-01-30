/*
 *  ebt_arpreply
 *
 *	Authors:
 *	Grzegorz Borowiak <grzes@gnu.univ.gda.pl>
 *	Bart De Schuymer <bdschuym@pandora.be>
 *
 *  August, 2003
 *
 */

#include <linux/netfilter_bridge/ebtables.h>
#include <linux/netfilter_bridge/ebt_arpreply.h>
#include <linux/if_arp.h>
#include <net/arp.h>
#include <linux/module.h>

static int ebt_target_reply(struct sk_buff **pskb, unsigned int hooknr,
   const struct net_device *in, const struct net_device *out,
   const void *data, unsigned int datalen)
{
	struct ebt_arpreply_info *info = (struct ebt_arpreply_info *)data;
	u32 sip, dip;
	struct arphdr ah;
	unsigned char sha[ETH_ALEN];
	struct sk_buff *skb = *pskb;

	if (skb_copy_bits(skb, 0, &ah, sizeof(ah)))
		return EBT_DROP;

	if (ah.ar_op != __constant_htons(ARPOP_REQUEST) || ah.ar_hln != ETH_ALEN
	    || ah.ar_pro != __constant_htons(ETH_P_IP) || ah.ar_pln != 4)
		return EBT_CONTINUE;

	if (skb_copy_bits(skb, sizeof(ah), &sha, ETH_ALEN))
		return EBT_DROP;

	if (skb_copy_bits(skb, sizeof(ah) + ETH_ALEN, &sip, sizeof(sip)))
		return EBT_DROP;

	if (skb_copy_bits(skb, sizeof(ah) + 2 * ETH_ALEN + sizeof(sip),
	    &dip, sizeof(dip)))
		return EBT_DROP;

	arp_send(ARPOP_REPLY, ETH_P_ARP, sip, (struct net_device *)in,
	         dip, sha, info->mac, sha);

	return info->target;
}

static int ebt_target_reply_check(const char *tablename, unsigned int hookmask,
   const struct ebt_entry *e, void *data, unsigned int datalen)
{
	struct ebt_arpreply_info *info = (struct ebt_arpreply_info *)data;

	if (datalen != EBT_ALIGN(sizeof(struct ebt_arpreply_info)))
		return -EINVAL;
	if (BASE_CHAIN && info->target == EBT_RETURN)
		return -EINVAL;
	if (e->ethproto != __constant_htons(ETH_P_ARP) ||
	    e->invflags & EBT_IPROTO)
		return -EINVAL;
	CLEAR_BASE_CHAIN_BIT;
	if (strcmp(tablename, "nat") || hookmask & ~(1 << NF_BR_PRE_ROUTING))
		return -EINVAL;
	return 0;
}

static struct ebt_target reply_target =
{
	.name		= EBT_ARPREPLY_TARGET,
	.target		= ebt_target_reply,
	.check		= ebt_target_reply_check,
	.me		= THIS_MODULE,
};

static int __init init(void)
{
	return ebt_register_target(&reply_target);
}

static void __exit fini(void)
{
	ebt_unregister_target(&reply_target);
}

module_init(init);
module_exit(fini);
MODULE_LICENSE("GPL");
