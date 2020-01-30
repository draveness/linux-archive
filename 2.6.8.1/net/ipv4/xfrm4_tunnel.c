/* xfrm4_tunnel.c: Generic IP tunnel transformer.
 *
 * Copyright (C) 2003 David S. Miller (davem@redhat.com)
 */

#include <linux/skbuff.h>
#include <net/xfrm.h>
#include <net/ip.h>
#include <net/protocol.h>

static int ipip_output(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct iphdr *iph;
	
	iph = skb->nh.iph;
	iph->tot_len = htons(skb->len);
	ip_send_check(iph);

	return 0;
}

static int ipip_xfrm_rcv(struct xfrm_state *x, struct xfrm_decap_state *decap, struct sk_buff *skb)
{
	return 0;
}

static struct xfrm_tunnel *ipip_handler;
static DECLARE_MUTEX(xfrm4_tunnel_sem);

int xfrm4_tunnel_register(struct xfrm_tunnel *handler)
{
	int ret;

	down(&xfrm4_tunnel_sem);
	ret = 0;
	if (ipip_handler != NULL)
		ret = -EINVAL;
	if (!ret)
		ipip_handler = handler;
	up(&xfrm4_tunnel_sem);

	return ret;
}

int xfrm4_tunnel_deregister(struct xfrm_tunnel *handler)
{
	int ret;

	down(&xfrm4_tunnel_sem);
	ret = 0;
	if (ipip_handler != handler)
		ret = -EINVAL;
	if (!ret)
		ipip_handler = NULL;
	up(&xfrm4_tunnel_sem);

	synchronize_net();

	return ret;
}

static int ipip_rcv(struct sk_buff *skb)
{
	struct xfrm_tunnel *handler = ipip_handler;

	/* Tunnel devices take precedence.  */
	if (handler && handler->handler(skb) == 0)
		return 0;

	return xfrm4_rcv_encap(skb, 0);
}

static void ipip_err(struct sk_buff *skb, u32 info)
{
	struct xfrm_tunnel *handler = ipip_handler;
	u32 arg = info;

	if (handler)
		handler->err_handler(skb, &arg);
}

static int ipip_init_state(struct xfrm_state *x, void *args)
{
	if (!x->props.mode)
		return -EINVAL;
	x->props.header_len = sizeof(struct iphdr);

	return 0;
}

static void ipip_destroy(struct xfrm_state *x)
{
}

static struct xfrm_type ipip_type = {
	.description	= "IPIP",
	.owner		= THIS_MODULE,
	.proto	     	= IPPROTO_IPIP,
	.init_state	= ipip_init_state,
	.destructor	= ipip_destroy,
	.input		= ipip_xfrm_rcv,
	.output		= ipip_output
};

static struct net_protocol ipip_protocol = {
	.handler	=	ipip_rcv,
	.err_handler	=	ipip_err,
	.no_policy	=	1,
};

static int __init ipip_init(void)
{
	if (xfrm_register_type(&ipip_type, AF_INET) < 0) {
		printk(KERN_INFO "ipip init: can't add xfrm type\n");
		return -EAGAIN;
	}
	if (inet_add_protocol(&ipip_protocol, IPPROTO_IPIP) < 0) {
		printk(KERN_INFO "ipip init: can't add protocol\n");
		xfrm_unregister_type(&ipip_type, AF_INET);
		return -EAGAIN;
	}
	return 0;
}

static void __exit ipip_fini(void)
{
	if (inet_del_protocol(&ipip_protocol, IPPROTO_IPIP) < 0)
		printk(KERN_INFO "ipip close: can't remove protocol\n");
	if (xfrm_unregister_type(&ipip_type, AF_INET) < 0)
		printk(KERN_INFO "ipip close: can't remove xfrm type\n");
}

module_init(ipip_init);
module_exit(ipip_fini);
MODULE_LICENSE("GPL");
