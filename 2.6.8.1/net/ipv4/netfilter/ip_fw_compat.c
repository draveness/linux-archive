/* Compatibility framework for ipchains and ipfwadm support; designed
   to look as much like the 2.2 infrastructure as possible. */

/* (C) 1999-2001 Paul `Rusty' Russell
 * (C) 2002-2004 Netfilter Core Team <coreteam@netfilter.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

struct notifier_block;

#include <linux/netfilter_ipv4.h>
#include <linux/ip.h>
#include <net/icmp.h>
#include <linux/if.h>
#include <linux/inetdevice.h>
#include <linux/netdevice.h>
#include <linux/module.h>
#include <asm/uaccess.h>
#include <net/ip.h>
#include <net/route.h>
#include <linux/netfilter_ipv4/compat_firewall.h>
#include <linux/netfilter_ipv4/ip_conntrack.h>
#include <linux/netfilter_ipv4/ip_conntrack_core.h>
#include "ip_fw_compat.h"

static struct firewall_ops *fwops;

#ifdef CONFIG_IP_VS
/* From ip_vs_core.c */
extern unsigned int
check_for_ip_vs_out(struct sk_buff **skb_p, int (*okfn)(struct sk_buff *));
#endif

/* They call these; we do what they want. */
int register_firewall(int pf, struct firewall_ops *fw)
{
	if (pf != PF_INET) {
		printk("Attempt to register non-IP firewall module.\n");
		return -EINVAL;
	}
	if (fwops) {
		printk("Attempt to register multiple firewall modules.\n");
		return -EBUSY;
	}

	fwops = fw;
	return 0;
}

int unregister_firewall(int pf, struct firewall_ops *fw)
{
	fwops = NULL;
	return 0;
}

static unsigned int
fw_in(unsigned int hooknum,
      struct sk_buff **pskb,
      const struct net_device *in,
      const struct net_device *out,
      int (*okfn)(struct sk_buff *))
{
	int ret = FW_BLOCK;
	u_int16_t redirpt;

	/* Assume worse case: any hook could change packet */
	(*pskb)->nfcache |= NFC_UNKNOWN | NFC_ALTERED;
	if ((*pskb)->ip_summed == CHECKSUM_HW)
		if (skb_checksum_help(pskb, (out == NULL)))
			return NF_DROP;

	switch (hooknum) {
	case NF_IP_PRE_ROUTING:
		if (fwops->fw_acct_in)
			fwops->fw_acct_in(fwops, PF_INET,
					  (struct net_device *)in,
					  &redirpt, pskb);

		if ((*pskb)->nh.iph->frag_off & htons(IP_MF|IP_OFFSET)) {
			*pskb = ip_ct_gather_frags(*pskb);

			if (!*pskb)
				return NF_STOLEN;
		}

		ret = fwops->fw_input(fwops, PF_INET, (struct net_device *)in,
				      &redirpt, pskb);
		break;

	case NF_IP_FORWARD:
		/* Connection will only be set if it was
                   demasqueraded: if so, skip forward chain. */
		if ((*pskb)->nfct)
			ret = FW_ACCEPT;
		else ret = fwops->fw_forward(fwops, PF_INET,
					     (struct net_device *)out,
					     &redirpt, pskb);
		break;

	case NF_IP_POST_ROUTING:
		ret = fwops->fw_output(fwops, PF_INET,
				       (struct net_device *)out,
				       &redirpt, pskb);
		if (ret == FW_ACCEPT || ret == FW_SKIP) {
			if (fwops->fw_acct_out)
				fwops->fw_acct_out(fwops, PF_INET,
						   (struct net_device *)out,
						   &redirpt,
						   pskb);

			/* ip_conntrack_confirm return NF_DROP or NF_ACCEPT */
			if (ip_conntrack_confirm(*pskb) == NF_DROP)
				ret = FW_BLOCK;
		}
		break;
	}

	switch (ret) {
	case FW_REJECT: {
		/* Alexey says:
		 *
		 * Generally, routing is THE FIRST thing to make, when
		 * packet enters IP stack. Before packet is routed you
		 * cannot call any service routines from IP stack.  */
		struct iphdr *iph = (*pskb)->nh.iph;

		if ((*pskb)->dst != NULL
		    || ip_route_input(*pskb, iph->daddr, iph->saddr, iph->tos,
				      (struct net_device *)in) == 0)
			icmp_send(*pskb, ICMP_DEST_UNREACH, ICMP_PORT_UNREACH,
				  0);
		return NF_DROP;
	}

	case FW_ACCEPT:
	case FW_SKIP:
		if (hooknum == NF_IP_PRE_ROUTING) {
			check_for_demasq(pskb);
			check_for_redirect(*pskb);
		} else if (hooknum == NF_IP_POST_ROUTING) {
			check_for_unredirect(*pskb);
			/* Handle ICMP errors from client here */
			if ((*pskb)->nh.iph->protocol == IPPROTO_ICMP
			    && (*pskb)->nfct)
				check_for_masq_error(pskb);
		}
		return NF_ACCEPT;

	case FW_MASQUERADE:
		if (hooknum == NF_IP_FORWARD) {
#ifdef CONFIG_IP_VS
			/* check if it is for ip_vs */
			if (check_for_ip_vs_out(pskb, okfn) == NF_STOLEN)
				return NF_STOLEN;
#endif
			return do_masquerade(pskb, out);
		}
		else return NF_ACCEPT;

	case FW_REDIRECT:
		if (hooknum == NF_IP_PRE_ROUTING)
			return do_redirect(*pskb, in, redirpt);
		else return NF_ACCEPT;

	default:
		/* FW_BLOCK */
		return NF_DROP;
	}
}

static unsigned int fw_confirm(unsigned int hooknum,
			       struct sk_buff **pskb,
			       const struct net_device *in,
			       const struct net_device *out,
			       int (*okfn)(struct sk_buff *))
{
	return ip_conntrack_confirm(*pskb);
}

extern int ip_fw_ctl(int optval, void *m, unsigned int len);

static int sock_fn(struct sock *sk, int optval, void __user *user, unsigned int len)
{
	/* MAX of:
	   2.2: sizeof(struct ip_fwtest) (~14x4 + 3x4 = 17x4)
	   2.2: sizeof(struct ip_fwnew) (~1x4 + 15x4 + 3x4 + 3x4 = 22x4)
	   2.0: sizeof(struct ip_fw) (~25x4)

	   We can't include both 2.0 and 2.2 headers, they conflict.
	   Hence, 200 is a good number. --RR */
	char tmp_fw[200];
	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	if (len > sizeof(tmp_fw) || len < 1)
		return -EINVAL;

	if (copy_from_user(&tmp_fw, user, len))
		return -EFAULT;

	return -ip_fw_ctl(optval, &tmp_fw, len);
}

static struct nf_hook_ops preroute_ops = {
	.hook		= fw_in,
	.owner		= THIS_MODULE,
	.pf		= PF_INET,
	.hooknum	= NF_IP_PRE_ROUTING,
	.priority	= NF_IP_PRI_FILTER,
};

static struct nf_hook_ops postroute_ops = {
	.hook		= fw_in,
	.owner		= THIS_MODULE,
	.pf		= PF_INET,
	.hooknum	= NF_IP_POST_ROUTING,
	.priority	= NF_IP_PRI_FILTER,
};

static struct nf_hook_ops forward_ops = {
	.hook		= fw_in,
	.owner		= THIS_MODULE,
	.pf		= PF_INET,
	.hooknum	= NF_IP_FORWARD,
	.priority	= NF_IP_PRI_FILTER,
};

static struct nf_hook_ops local_in_ops = {
	.hook		= fw_confirm,
	.owner		= THIS_MODULE,
	.pf		= PF_INET,
	.hooknum	= NF_IP_LOCAL_IN,
	.priority	= NF_IP_PRI_LAST - 1,
};

static struct nf_sockopt_ops sock_ops = {
	.pf		= PF_INET,
	.set_optmin	= 64,
	.set_optmax	= 64 + 1024 + 1,
	.set		= &sock_fn,
};

extern int ipfw_init_or_cleanup(int init);

static int init_or_cleanup(int init)
{
	int ret = 0;

	if (!init) goto cleanup;

	ret = nf_register_sockopt(&sock_ops);

	if (ret < 0)
		goto cleanup_nothing;

	ret = ipfw_init_or_cleanup(1);
	if (ret < 0)
		goto cleanup_sockopt;

	ret = masq_init();
	if (ret < 0)
		goto cleanup_ipfw;

	nf_register_hook(&preroute_ops);
	nf_register_hook(&postroute_ops);
	nf_register_hook(&forward_ops);
	nf_register_hook(&local_in_ops);

	return ret;

 cleanup:
	nf_unregister_hook(&preroute_ops);
	nf_unregister_hook(&postroute_ops);
	nf_unregister_hook(&forward_ops);
	nf_unregister_hook(&local_in_ops);

	masq_cleanup();

 cleanup_ipfw:
	ipfw_init_or_cleanup(0);

 cleanup_sockopt:
	nf_unregister_sockopt(&sock_ops);

 cleanup_nothing:
	return ret;
}

static int __init init(void)
{
	return init_or_cleanup(1);
}

static void __exit fini(void)
{
	init_or_cleanup(0);
}

module_init(init);
module_exit(fini);
