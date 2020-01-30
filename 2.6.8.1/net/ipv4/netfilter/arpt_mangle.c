/* module that allows mangling of the arp payload */
#include <linux/module.h>
#include <linux/netfilter_arp/arpt_mangle.h>
#include <net/sock.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bart De Schuymer <bdschuym@pandora.be>");
MODULE_DESCRIPTION("arptables arp payload mangle target");

static unsigned int
target(struct sk_buff **pskb, unsigned int hooknum, const struct net_device *in,
   const struct net_device *out, const void *targinfo, void *userinfo)
{
	const struct arpt_mangle *mangle = targinfo;
	struct arphdr *arp;
	unsigned char *arpptr;
	int pln, hln;

	if (skb_shared(*pskb) || skb_cloned(*pskb)) {
		struct sk_buff *nskb;

		nskb = skb_copy(*pskb, GFP_ATOMIC);
		if (!nskb)
			return NF_DROP;
		if ((*pskb)->sk)
			skb_set_owner_w(nskb, (*pskb)->sk);
		kfree_skb(*pskb);
		*pskb = nskb;
	}

	arp = (*pskb)->nh.arph;
	arpptr = (*pskb)->nh.raw + sizeof(*arp);
	pln = arp->ar_pln;
	hln = arp->ar_hln;
	/* We assume that pln and hln were checked in the match */
	if (mangle->flags & ARPT_MANGLE_SDEV) {
		if (ARPT_DEV_ADDR_LEN_MAX < hln ||
		   (arpptr + hln > (**pskb).tail))
			return NF_DROP;
		memcpy(arpptr, mangle->src_devaddr, hln);
	}
	arpptr += hln;
	if (mangle->flags & ARPT_MANGLE_SIP) {
		if (ARPT_MANGLE_ADDR_LEN_MAX < pln ||
		   (arpptr + pln > (**pskb).tail))
			return NF_DROP;
		memcpy(arpptr, &mangle->u_s.src_ip, pln);
	}
	arpptr += pln;
	if (mangle->flags & ARPT_MANGLE_TDEV) {
		if (ARPT_DEV_ADDR_LEN_MAX < hln ||
		   (arpptr + hln > (**pskb).tail))
			return NF_DROP;
		memcpy(arpptr, mangle->tgt_devaddr, hln);
	}
	arpptr += hln;
	if (mangle->flags & ARPT_MANGLE_TIP) {
		if (ARPT_MANGLE_ADDR_LEN_MAX < pln ||
		   (arpptr + pln > (**pskb).tail))
			return NF_DROP;
		memcpy(arpptr, &mangle->u_t.tgt_ip, pln);
	}
	return mangle->target;
}

static int
checkentry(const char *tablename, const struct arpt_entry *e, void *targinfo,
   unsigned int targinfosize, unsigned int hook_mask)
{
	const struct arpt_mangle *mangle = targinfo;

	if (mangle->flags & ~ARPT_MANGLE_MASK ||
	    !(mangle->flags & ARPT_MANGLE_MASK))
		return 0;

	if (mangle->target != NF_DROP && mangle->target != NF_ACCEPT &&
	   mangle->target != ARPT_CONTINUE)
		return 0;
	return 1;
}

static struct arpt_target arpt_mangle_reg
= {
        .name		= "mangle",
        .target		= target,
        .checkentry	= checkentry,
        .me		= THIS_MODULE,
};

static int __init init(void)
{
	if (arpt_register_target(&arpt_mangle_reg))
		return -EINVAL;

	return 0;
}

static void __exit fini(void)
{
	arpt_unregister_target(&arpt_mangle_reg);
}

module_init(init);
module_exit(fini);
