/*
 * DECnet       An implementation of the DECnet protocol suite for the LINUX
 *              operating system.  DECnet is implemented using the  BSD Socket
 *              interface as the means of communication with the user level.
 *
 *              DECnet Routing Message Grabulator
 *
 *              (C) 2000 ChyGwyn Limited  -  http://www.chygwyn.com/
 *              This code may be copied under the GPL v.2 or at your option
 *              any later version.
 *
 * Author:      Steven Whitehouse <steve@chygwyn.com>
 *
 */
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/spinlock.h>
#include <linux/netlink.h>

#include <net/sock.h>
#include <net/flow.h>
#include <net/dn.h>
#include <net/dn_route.h>

#include <linux/netfilter_decnet.h>

static struct sock *dnrmg = NULL;


static struct sk_buff *dnrmg_build_message(struct sk_buff *rt_skb, int *errp)
{
	struct sk_buff *skb = NULL;
	size_t size;
	unsigned char *old_tail;
	struct nlmsghdr *nlh;
	unsigned char *ptr;
	struct nf_dn_rtmsg *rtm;

	size = NLMSG_SPACE(rt_skb->len);
	size += NLMSG_ALIGN(sizeof(struct nf_dn_rtmsg));
	skb = alloc_skb(size, GFP_ATOMIC);
	if (!skb)
		goto nlmsg_failure;
	old_tail = skb->tail;
	nlh = NLMSG_PUT(skb, 0, 0, 0, size - sizeof(*nlh));
	rtm = (struct nf_dn_rtmsg *)NLMSG_DATA(nlh);
	rtm->nfdn_ifindex = rt_skb->dev->ifindex;
	ptr = NFDN_RTMSG(rtm);
	memcpy(ptr, rt_skb->data, rt_skb->len);
	nlh->nlmsg_len = skb->tail - old_tail;
	return skb;

nlmsg_failure:
	if (skb)
		kfree_skb(skb);
	*errp = -ENOMEM;
	if (net_ratelimit())
		printk(KERN_ERR "dn_rtmsg: error creating netlink message\n");
	return NULL;
}

static void dnrmg_send_peer(struct sk_buff *skb)
{
	struct sk_buff *skb2;
	int status = 0;
	int group = 0;
	unsigned char flags = *skb->data;

	switch(flags & DN_RT_CNTL_MSK) {
		case DN_RT_PKT_L1RT:
			group = DNRMG_L1_GROUP;
			break;
		case DN_RT_PKT_L2RT:
			group = DNRMG_L2_GROUP;
			break;
		default:
			return;
	}

	skb2 = dnrmg_build_message(skb, &status);
	if (skb2 == NULL)
		return;
	NETLINK_CB(skb2).dst_groups = group;
	netlink_broadcast(dnrmg, skb2, 0, group, GFP_ATOMIC);
}


static unsigned int dnrmg_hook(unsigned int hook,
			struct sk_buff **pskb,
			const struct net_device *in,
			const struct net_device *out,
			int (*okfn)(struct sk_buff *))
{
	dnrmg_send_peer(*pskb);
	return NF_ACCEPT;
}


#define RCV_SKB_FAIL(err) do { netlink_ack(skb, nlh, (err)); return; } while (0)

static inline void dnrmg_receive_user_skb(struct sk_buff *skb)
{
	struct nlmsghdr *nlh = (struct nlmsghdr *)skb->data;

	if (nlh->nlmsg_len < sizeof(*nlh) || skb->len < nlh->nlmsg_len)
		return;

	if (!cap_raised(NETLINK_CB(skb).eff_cap, CAP_NET_ADMIN))
		RCV_SKB_FAIL(-EPERM);

	/* Eventually we might send routing messages too */

	RCV_SKB_FAIL(-EINVAL);
}

static void dnrmg_receive_user_sk(struct sock *sk, int len)
{
	struct sk_buff *skb;

	while((skb = skb_dequeue(&sk->sk_receive_queue)) != NULL) {
		dnrmg_receive_user_skb(skb);
		kfree_skb(skb);
	}
}

static struct nf_hook_ops dnrmg_ops = {
	.hook		= dnrmg_hook,
	.pf		= PF_DECnet,
	.hooknum	= NF_DN_ROUTE,
	.priority	= NF_DN_PRI_DNRTMSG,
};

static int __init init(void)
{
	int rv = 0;

	dnrmg = netlink_kernel_create(NETLINK_DNRTMSG, dnrmg_receive_user_sk);
	if (dnrmg == NULL) {
		printk(KERN_ERR "dn_rtmsg: Cannot create netlink socket");
		return -ENOMEM;
	}

	rv = nf_register_hook(&dnrmg_ops);
	if (rv) {
		sock_release(dnrmg->sk_socket);
	}

	return rv;
}

static void __exit fini(void)
{
	nf_unregister_hook(&dnrmg_ops);
	sock_release(dnrmg->sk_socket);
}


MODULE_DESCRIPTION("DECnet Routing Message Grabulator");
MODULE_AUTHOR("Steven Whitehouse <steve@chygwyn.com>");
MODULE_LICENSE("GPL");

module_init(init);
module_exit(fini);

