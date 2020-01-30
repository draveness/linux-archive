/*
 *  ebt_arp
 *
 *	Authors:
 *	Bart De Schuymer <bdschuym@pandora.be>
 *	Tim Gardner <timg@tpi.com>
 *
 *  April, 2002
 *
 */

#include <linux/netfilter_bridge/ebtables.h>
#include <linux/netfilter_bridge/ebt_arp.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/module.h>

static int ebt_filter_arp(const struct sk_buff *skb, const struct net_device *in,
   const struct net_device *out, const void *data, unsigned int datalen)
{
	struct ebt_arp_info *info = (struct ebt_arp_info *)data;
	struct arphdr _arph, *ah;

	ah = skb_header_pointer(skb, 0, sizeof(_arph), &_arph);
	if (ah == NULL)
		return EBT_NOMATCH;
	if (info->bitmask & EBT_ARP_OPCODE && FWINV(info->opcode !=
	   ah->ar_op, EBT_ARP_OPCODE))
		return EBT_NOMATCH;
	if (info->bitmask & EBT_ARP_HTYPE && FWINV(info->htype !=
	   ah->ar_hrd, EBT_ARP_HTYPE))
		return EBT_NOMATCH;
	if (info->bitmask & EBT_ARP_PTYPE && FWINV(info->ptype !=
	   ah->ar_pro, EBT_ARP_PTYPE))
		return EBT_NOMATCH;

	if (info->bitmask & (EBT_ARP_SRC_IP | EBT_ARP_DST_IP)) {
		__be32 saddr, daddr, *sap, *dap;

		if (ah->ar_pln != sizeof(__be32) || ah->ar_pro != htons(ETH_P_IP))
			return EBT_NOMATCH;
		sap = skb_header_pointer(skb, sizeof(struct arphdr) +
					ah->ar_hln, sizeof(saddr),
					&saddr);
		if (sap == NULL)
			return EBT_NOMATCH;
		dap = skb_header_pointer(skb, sizeof(struct arphdr) +
					2*ah->ar_hln+sizeof(saddr),
					sizeof(daddr), &daddr);
		if (dap == NULL)
			return EBT_NOMATCH;
		if (info->bitmask & EBT_ARP_SRC_IP &&
		    FWINV(info->saddr != (*sap & info->smsk), EBT_ARP_SRC_IP))
			return EBT_NOMATCH;
		if (info->bitmask & EBT_ARP_DST_IP &&
		    FWINV(info->daddr != (*dap & info->dmsk), EBT_ARP_DST_IP))
			return EBT_NOMATCH;
		if (info->bitmask & EBT_ARP_GRAT &&
		    FWINV(*dap != *sap, EBT_ARP_GRAT))
			return EBT_NOMATCH;
	}

	if (info->bitmask & (EBT_ARP_SRC_MAC | EBT_ARP_DST_MAC)) {
		unsigned char _mac[ETH_ALEN], *mp;
		uint8_t verdict, i;

		if (ah->ar_hln != ETH_ALEN || ah->ar_hrd != htons(ARPHRD_ETHER))
			return EBT_NOMATCH;
		if (info->bitmask & EBT_ARP_SRC_MAC) {
			mp = skb_header_pointer(skb, sizeof(struct arphdr),
						sizeof(_mac), &_mac);
			if (mp == NULL)
				return EBT_NOMATCH;
			verdict = 0;
			for (i = 0; i < 6; i++)
				verdict |= (mp[i] ^ info->smaddr[i]) &
				       info->smmsk[i];
			if (FWINV(verdict != 0, EBT_ARP_SRC_MAC))
				return EBT_NOMATCH;
		}

		if (info->bitmask & EBT_ARP_DST_MAC) {
			mp = skb_header_pointer(skb, sizeof(struct arphdr) +
						ah->ar_hln + ah->ar_pln,
						sizeof(_mac), &_mac);
			if (mp == NULL)
				return EBT_NOMATCH;
			verdict = 0;
			for (i = 0; i < 6; i++)
				verdict |= (mp[i] ^ info->dmaddr[i]) &
					info->dmmsk[i];
			if (FWINV(verdict != 0, EBT_ARP_DST_MAC))
				return EBT_NOMATCH;
		}
	}

	return EBT_MATCH;
}

static int ebt_arp_check(const char *tablename, unsigned int hookmask,
   const struct ebt_entry *e, void *data, unsigned int datalen)
{
	struct ebt_arp_info *info = (struct ebt_arp_info *)data;

	if (datalen != EBT_ALIGN(sizeof(struct ebt_arp_info)))
		return -EINVAL;
	if ((e->ethproto != htons(ETH_P_ARP) &&
	   e->ethproto != htons(ETH_P_RARP)) ||
	   e->invflags & EBT_IPROTO)
		return -EINVAL;
	if (info->bitmask & ~EBT_ARP_MASK || info->invflags & ~EBT_ARP_MASK)
		return -EINVAL;
	return 0;
}

static struct ebt_match filter_arp =
{
	.name		= EBT_ARP_MATCH,
	.match		= ebt_filter_arp,
	.check		= ebt_arp_check,
	.me		= THIS_MODULE,
};

static int __init ebt_arp_init(void)
{
	return ebt_register_match(&filter_arp);
}

static void __exit ebt_arp_fini(void)
{
	ebt_unregister_match(&filter_arp);
}

module_init(ebt_arp_init);
module_exit(ebt_arp_fini);
MODULE_LICENSE("GPL");
