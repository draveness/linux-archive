/* This kernel module is used to modify the connection mark values, or
 * to optionally restore the skb nfmark from the connection mark
 *
 * Copyright (C) 2002,2004 MARA Systems AB <http://www.marasystems.com>
 * by Henrik Nordstrom <hno@marasystems.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <net/checksum.h>

MODULE_AUTHOR("Henrik Nordstrom <hno@marasytems.com>");
MODULE_DESCRIPTION("IP tables CONNMARK matching module");
MODULE_LICENSE("GPL");
MODULE_ALIAS("ipt_CONNMARK");

#include <linux/netfilter/x_tables.h>
#include <linux/netfilter/xt_CONNMARK.h>
#include <net/netfilter/nf_conntrack_ecache.h>

static unsigned int
target(struct sk_buff **pskb,
       const struct net_device *in,
       const struct net_device *out,
       unsigned int hooknum,
       const struct xt_target *target,
       const void *targinfo)
{
	const struct xt_connmark_target_info *markinfo = targinfo;
	struct nf_conn *ct;
	enum ip_conntrack_info ctinfo;
	u_int32_t diff;
	u_int32_t mark;
	u_int32_t newmark;

	ct = nf_ct_get(*pskb, &ctinfo);
	if (ct) {
		switch(markinfo->mode) {
		case XT_CONNMARK_SET:
			newmark = (ct->mark & ~markinfo->mask) | markinfo->mark;
			if (newmark != ct->mark) {
				ct->mark = newmark;
				nf_conntrack_event_cache(IPCT_MARK, *pskb);
			}
			break;
		case XT_CONNMARK_SAVE:
			newmark = (ct->mark & ~markinfo->mask) |
				  ((*pskb)->mark & markinfo->mask);
			if (ct->mark != newmark) {
				ct->mark = newmark;
				nf_conntrack_event_cache(IPCT_MARK, *pskb);
			}
			break;
		case XT_CONNMARK_RESTORE:
			mark = (*pskb)->mark;
			diff = (ct->mark ^ mark) & markinfo->mask;
			(*pskb)->mark = mark ^ diff;
			break;
		}
	}

	return XT_CONTINUE;
}

static bool
checkentry(const char *tablename,
	   const void *entry,
	   const struct xt_target *target,
	   void *targinfo,
	   unsigned int hook_mask)
{
	const struct xt_connmark_target_info *matchinfo = targinfo;

	if (nf_ct_l3proto_try_module_get(target->family) < 0) {
		printk(KERN_WARNING "can't load conntrack support for "
				    "proto=%d\n", target->family);
		return false;
	}
	if (matchinfo->mode == XT_CONNMARK_RESTORE) {
		if (strcmp(tablename, "mangle") != 0) {
			printk(KERN_WARNING "CONNMARK: restore can only be "
			       "called from \"mangle\" table, not \"%s\"\n",
			       tablename);
			return false;
		}
	}
	if (matchinfo->mark > 0xffffffff || matchinfo->mask > 0xffffffff) {
		printk(KERN_WARNING "CONNMARK: Only supports 32bit mark\n");
		return false;
	}
	return true;
}

static void
destroy(const struct xt_target *target, void *targinfo)
{
	nf_ct_l3proto_module_put(target->family);
}

#ifdef CONFIG_COMPAT
struct compat_xt_connmark_target_info {
	compat_ulong_t	mark, mask;
	u_int8_t	mode;
	u_int8_t	__pad1;
	u_int16_t	__pad2;
};

static void compat_from_user(void *dst, void *src)
{
	const struct compat_xt_connmark_target_info *cm = src;
	struct xt_connmark_target_info m = {
		.mark	= cm->mark,
		.mask	= cm->mask,
		.mode	= cm->mode,
	};
	memcpy(dst, &m, sizeof(m));
}

static int compat_to_user(void __user *dst, void *src)
{
	const struct xt_connmark_target_info *m = src;
	struct compat_xt_connmark_target_info cm = {
		.mark	= m->mark,
		.mask	= m->mask,
		.mode	= m->mode,
	};
	return copy_to_user(dst, &cm, sizeof(cm)) ? -EFAULT : 0;
}
#endif /* CONFIG_COMPAT */

static struct xt_target xt_connmark_target[] __read_mostly = {
	{
		.name		= "CONNMARK",
		.family		= AF_INET,
		.checkentry	= checkentry,
		.destroy	= destroy,
		.target		= target,
		.targetsize	= sizeof(struct xt_connmark_target_info),
#ifdef CONFIG_COMPAT
		.compatsize	= sizeof(struct compat_xt_connmark_target_info),
		.compat_from_user = compat_from_user,
		.compat_to_user	= compat_to_user,
#endif
		.me		= THIS_MODULE
	},
	{
		.name		= "CONNMARK",
		.family		= AF_INET6,
		.checkentry	= checkentry,
		.destroy	= destroy,
		.target		= target,
		.targetsize	= sizeof(struct xt_connmark_target_info),
		.me		= THIS_MODULE
	},
};

static int __init xt_connmark_init(void)
{
	return xt_register_targets(xt_connmark_target,
				   ARRAY_SIZE(xt_connmark_target));
}

static void __exit xt_connmark_fini(void)
{
	xt_unregister_targets(xt_connmark_target,
			      ARRAY_SIZE(xt_connmark_target));
}

module_init(xt_connmark_init);
module_exit(xt_connmark_fini);
