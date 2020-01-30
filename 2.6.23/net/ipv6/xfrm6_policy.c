/*
 * xfrm6_policy.c: based on xfrm4_policy.c
 *
 * Authors:
 *	Mitsuru KANDA @USAGI
 * 	Kazunori MIYAZAWA @USAGI
 * 	Kunihiro Ishiguro <kunihiro@ipinfusion.com>
 * 		IPv6 support
 * 	YOSHIFUJI Hideaki
 * 		Split up af-specific portion
 *
 */

#include <linux/compiler.h>
#include <linux/netdevice.h>
#include <net/addrconf.h>
#include <net/xfrm.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/ip6_route.h>
#if defined(CONFIG_IPV6_MIP6) || defined(CONFIG_IPV6_MIP6_MODULE)
#include <net/mip6.h>
#endif

static struct dst_ops xfrm6_dst_ops;
static struct xfrm_policy_afinfo xfrm6_policy_afinfo;

static int xfrm6_dst_lookup(struct xfrm_dst **xdst, struct flowi *fl)
{
	struct dst_entry *dst = ip6_route_output(NULL, fl);
	int err = dst->error;
	if (!err)
		*xdst = (struct xfrm_dst *) dst;
	else
		dst_release(dst);
	return err;
}

static int xfrm6_get_saddr(xfrm_address_t *saddr, xfrm_address_t *daddr)
{
	struct rt6_info *rt;
	struct flowi fl_tunnel = {
		.nl_u = {
			.ip6_u = {
				.daddr = *(struct in6_addr *)&daddr->a6,
			},
		},
	};

	if (!xfrm6_dst_lookup((struct xfrm_dst **)&rt, &fl_tunnel)) {
		ipv6_get_saddr(&rt->u.dst, (struct in6_addr *)&daddr->a6,
			       (struct in6_addr *)&saddr->a6);
		dst_release(&rt->u.dst);
		return 0;
	}
	return -EHOSTUNREACH;
}

static struct dst_entry *
__xfrm6_find_bundle(struct flowi *fl, struct xfrm_policy *policy)
{
	struct dst_entry *dst;

	/* Still not clear if we should set fl->fl6_{src,dst}... */
	read_lock_bh(&policy->lock);
	for (dst = policy->bundles; dst; dst = dst->next) {
		struct xfrm_dst *xdst = (struct xfrm_dst*)dst;
		struct in6_addr fl_dst_prefix, fl_src_prefix;

		ipv6_addr_prefix(&fl_dst_prefix,
				 &fl->fl6_dst,
				 xdst->u.rt6.rt6i_dst.plen);
		ipv6_addr_prefix(&fl_src_prefix,
				 &fl->fl6_src,
				 xdst->u.rt6.rt6i_src.plen);
		if (ipv6_addr_equal(&xdst->u.rt6.rt6i_dst.addr, &fl_dst_prefix) &&
		    ipv6_addr_equal(&xdst->u.rt6.rt6i_src.addr, &fl_src_prefix) &&
		    xfrm_bundle_ok(policy, xdst, fl, AF_INET6,
				   (xdst->u.rt6.rt6i_dst.plen != 128 ||
				    xdst->u.rt6.rt6i_src.plen != 128))) {
			dst_clone(dst);
			break;
		}
	}
	read_unlock_bh(&policy->lock);
	return dst;
}

static inline struct in6_addr*
__xfrm6_bundle_addr_remote(struct xfrm_state *x, struct in6_addr *addr)
{
	return (x->type->remote_addr) ?
		(struct in6_addr*)x->type->remote_addr(x, (xfrm_address_t *)addr) :
		(struct in6_addr*)&x->id.daddr;
}

static inline struct in6_addr*
__xfrm6_bundle_addr_local(struct xfrm_state *x, struct in6_addr *addr)
{
	return (x->type->local_addr) ?
		(struct in6_addr*)x->type->local_addr(x, (xfrm_address_t *)addr) :
		(struct in6_addr*)&x->props.saddr;
}

static inline void
__xfrm6_bundle_len_inc(int *len, int *nflen, struct xfrm_state *x)
{
	if (x->type->flags & XFRM_TYPE_NON_FRAGMENT)
		*nflen += x->props.header_len;
	else
		*len += x->props.header_len;
}

static inline void
__xfrm6_bundle_len_dec(int *len, int *nflen, struct xfrm_state *x)
{
	if (x->type->flags & XFRM_TYPE_NON_FRAGMENT)
		*nflen -= x->props.header_len;
	else
		*len -= x->props.header_len;
}

/* Allocate chain of dst_entry's, attach known xfrm's, calculate
 * all the metrics... Shortly, bundle a bundle.
 */

static int
__xfrm6_bundle_create(struct xfrm_policy *policy, struct xfrm_state **xfrm, int nx,
		      struct flowi *fl, struct dst_entry **dst_p)
{
	struct dst_entry *dst, *dst_prev;
	struct rt6_info *rt0 = (struct rt6_info*)(*dst_p);
	struct rt6_info *rt  = rt0;
	struct flowi fl_tunnel = {
		.nl_u = {
			.ip6_u = {
				.saddr = fl->fl6_src,
				.daddr = fl->fl6_dst,
			}
		}
	};
	int i;
	int err = 0;
	int header_len = 0;
	int nfheader_len = 0;
	int trailer_len = 0;

	dst = dst_prev = NULL;
	dst_hold(&rt->u.dst);

	for (i = 0; i < nx; i++) {
		struct dst_entry *dst1 = dst_alloc(&xfrm6_dst_ops);
		struct xfrm_dst *xdst;

		if (unlikely(dst1 == NULL)) {
			err = -ENOBUFS;
			dst_release(&rt->u.dst);
			goto error;
		}

		if (!dst)
			dst = dst1;
		else {
			dst_prev->child = dst1;
			dst1->flags |= DST_NOHASH;
			dst_clone(dst1);
		}

		xdst = (struct xfrm_dst *)dst1;
		xdst->route = &rt->u.dst;
		xdst->genid = xfrm[i]->genid;
		if (rt->rt6i_node)
			xdst->route_cookie = rt->rt6i_node->fn_sernum;

		dst1->next = dst_prev;
		dst_prev = dst1;

		__xfrm6_bundle_len_inc(&header_len, &nfheader_len, xfrm[i]);
		trailer_len += xfrm[i]->props.trailer_len;

		if (xfrm[i]->props.mode == XFRM_MODE_TUNNEL ||
		    xfrm[i]->props.mode == XFRM_MODE_ROUTEOPTIMIZATION) {
			unsigned short encap_family = xfrm[i]->props.family;
			switch(encap_family) {
			case AF_INET:
				fl_tunnel.fl4_dst = xfrm[i]->id.daddr.a4;
				fl_tunnel.fl4_src = xfrm[i]->props.saddr.a4;
				break;
			case AF_INET6:
				ipv6_addr_copy(&fl_tunnel.fl6_dst, __xfrm6_bundle_addr_remote(xfrm[i], &fl->fl6_dst));

				ipv6_addr_copy(&fl_tunnel.fl6_src, __xfrm6_bundle_addr_local(xfrm[i], &fl->fl6_src));
				break;
			default:
				BUG_ON(1);
			}

			err = xfrm_dst_lookup((struct xfrm_dst **) &rt,
					      &fl_tunnel, encap_family);
			if (err)
				goto error;
		} else
			dst_hold(&rt->u.dst);
	}

	dst_prev->child = &rt->u.dst;
	dst->path = &rt->u.dst;
	if (rt->rt6i_node)
		((struct xfrm_dst *)dst)->path_cookie = rt->rt6i_node->fn_sernum;

	*dst_p = dst;
	dst = dst_prev;

	dst_prev = *dst_p;
	i = 0;
	for (; dst_prev != &rt->u.dst; dst_prev = dst_prev->child) {
		struct xfrm_dst *x = (struct xfrm_dst*)dst_prev;
		struct xfrm_state_afinfo *afinfo;

		dst_prev->xfrm = xfrm[i++];
		dst_prev->dev = rt->u.dst.dev;
		if (rt->u.dst.dev)
			dev_hold(rt->u.dst.dev);
		dst_prev->obsolete	= -1;
		dst_prev->flags	       |= DST_HOST;
		dst_prev->lastuse	= jiffies;
		dst_prev->header_len	= header_len;
		dst_prev->nfheader_len	= nfheader_len;
		dst_prev->trailer_len	= trailer_len;
		memcpy(&dst_prev->metrics, &x->route->metrics, sizeof(dst_prev->metrics));

		/* Copy neighbour for reachability confirmation */
		dst_prev->neighbour	= neigh_clone(rt->u.dst.neighbour);
		dst_prev->input		= rt->u.dst.input;
		/* XXX: When IPv4 is implemented as module and can be unloaded,
		 * we should manage reference to xfrm4_output in afinfo->output.
		 * Miyazawa
		 */
		afinfo = xfrm_state_get_afinfo(dst_prev->xfrm->props.family);
		if (!afinfo) {
			dst = *dst_p;
			goto error;
		}

		dst_prev->output = afinfo->output;
		xfrm_state_put_afinfo(afinfo);
		/* Sheit... I remember I did this right. Apparently,
		 * it was magically lost, so this code needs audit */
		x->u.rt6.rt6i_flags    = rt0->rt6i_flags&(RTCF_BROADCAST|RTCF_MULTICAST|RTCF_LOCAL);
		x->u.rt6.rt6i_metric   = rt0->rt6i_metric;
		x->u.rt6.rt6i_node     = rt0->rt6i_node;
		x->u.rt6.rt6i_gateway  = rt0->rt6i_gateway;
		memcpy(&x->u.rt6.rt6i_gateway, &rt0->rt6i_gateway, sizeof(x->u.rt6.rt6i_gateway));
		x->u.rt6.rt6i_dst      = rt0->rt6i_dst;
		x->u.rt6.rt6i_src      = rt0->rt6i_src;
		x->u.rt6.rt6i_idev     = rt0->rt6i_idev;
		in6_dev_hold(rt0->rt6i_idev);
		__xfrm6_bundle_len_dec(&header_len, &nfheader_len, x->u.dst.xfrm);
		trailer_len -= x->u.dst.xfrm->props.trailer_len;
	}

	xfrm_init_pmtu(dst);
	return 0;

error:
	if (dst)
		dst_free(dst);
	return err;
}

static inline void
_decode_session6(struct sk_buff *skb, struct flowi *fl)
{
	u16 offset = skb_network_header_len(skb);
	struct ipv6hdr *hdr = ipv6_hdr(skb);
	struct ipv6_opt_hdr *exthdr;
	const unsigned char *nh = skb_network_header(skb);
	u8 nexthdr = nh[IP6CB(skb)->nhoff];

	memset(fl, 0, sizeof(struct flowi));
	ipv6_addr_copy(&fl->fl6_dst, &hdr->daddr);
	ipv6_addr_copy(&fl->fl6_src, &hdr->saddr);

	while (pskb_may_pull(skb, nh + offset + 1 - skb->data)) {
		nh = skb_network_header(skb);
		exthdr = (struct ipv6_opt_hdr *)(nh + offset);

		switch (nexthdr) {
		case NEXTHDR_ROUTING:
		case NEXTHDR_HOP:
		case NEXTHDR_DEST:
			offset += ipv6_optlen(exthdr);
			nexthdr = exthdr->nexthdr;
			exthdr = (struct ipv6_opt_hdr *)(nh + offset);
			break;

		case IPPROTO_UDP:
		case IPPROTO_UDPLITE:
		case IPPROTO_TCP:
		case IPPROTO_SCTP:
		case IPPROTO_DCCP:
			if (pskb_may_pull(skb, nh + offset + 4 - skb->data)) {
				__be16 *ports = (__be16 *)exthdr;

				fl->fl_ip_sport = ports[0];
				fl->fl_ip_dport = ports[1];
			}
			fl->proto = nexthdr;
			return;

		case IPPROTO_ICMPV6:
			if (pskb_may_pull(skb, nh + offset + 2 - skb->data)) {
				u8 *icmp = (u8 *)exthdr;

				fl->fl_icmp_type = icmp[0];
				fl->fl_icmp_code = icmp[1];
			}
			fl->proto = nexthdr;
			return;

#if defined(CONFIG_IPV6_MIP6) || defined(CONFIG_IPV6_MIP6_MODULE)
		case IPPROTO_MH:
			if (pskb_may_pull(skb, nh + offset + 3 - skb->data)) {
				struct ip6_mh *mh;
				mh = (struct ip6_mh *)exthdr;

				fl->fl_mh_type = mh->ip6mh_type;
			}
			fl->proto = nexthdr;
			return;
#endif

		/* XXX Why are there these headers? */
		case IPPROTO_AH:
		case IPPROTO_ESP:
		case IPPROTO_COMP:
		default:
			fl->fl_ipsec_spi = 0;
			fl->proto = nexthdr;
			return;
		}
	}
}

static inline int xfrm6_garbage_collect(void)
{
	xfrm6_policy_afinfo.garbage_collect();
	return (atomic_read(&xfrm6_dst_ops.entries) > xfrm6_dst_ops.gc_thresh*2);
}

static void xfrm6_update_pmtu(struct dst_entry *dst, u32 mtu)
{
	struct xfrm_dst *xdst = (struct xfrm_dst *)dst;
	struct dst_entry *path = xdst->route;

	path->ops->update_pmtu(path, mtu);
}

static void xfrm6_dst_destroy(struct dst_entry *dst)
{
	struct xfrm_dst *xdst = (struct xfrm_dst *)dst;

	if (likely(xdst->u.rt6.rt6i_idev))
		in6_dev_put(xdst->u.rt6.rt6i_idev);
	xfrm_dst_destroy(xdst);
}

static void xfrm6_dst_ifdown(struct dst_entry *dst, struct net_device *dev,
			     int unregister)
{
	struct xfrm_dst *xdst;

	if (!unregister)
		return;

	xdst = (struct xfrm_dst *)dst;
	if (xdst->u.rt6.rt6i_idev->dev == dev) {
		struct inet6_dev *loopback_idev = in6_dev_get(&loopback_dev);
		BUG_ON(!loopback_idev);

		do {
			in6_dev_put(xdst->u.rt6.rt6i_idev);
			xdst->u.rt6.rt6i_idev = loopback_idev;
			in6_dev_hold(loopback_idev);
			xdst = (struct xfrm_dst *)xdst->u.dst.child;
		} while (xdst->u.dst.xfrm);

		__in6_dev_put(loopback_idev);
	}

	xfrm_dst_ifdown(dst, dev);
}

static struct dst_ops xfrm6_dst_ops = {
	.family =		AF_INET6,
	.protocol =		__constant_htons(ETH_P_IPV6),
	.gc =			xfrm6_garbage_collect,
	.update_pmtu =		xfrm6_update_pmtu,
	.destroy =		xfrm6_dst_destroy,
	.ifdown =		xfrm6_dst_ifdown,
	.gc_thresh =		1024,
	.entry_size =		sizeof(struct xfrm_dst),
};

static struct xfrm_policy_afinfo xfrm6_policy_afinfo = {
	.family =		AF_INET6,
	.dst_ops =		&xfrm6_dst_ops,
	.dst_lookup =		xfrm6_dst_lookup,
	.get_saddr = 		xfrm6_get_saddr,
	.find_bundle =		__xfrm6_find_bundle,
	.bundle_create =	__xfrm6_bundle_create,
	.decode_session =	_decode_session6,
};

static void __init xfrm6_policy_init(void)
{
	xfrm_policy_register_afinfo(&xfrm6_policy_afinfo);
}

static void xfrm6_policy_fini(void)
{
	xfrm_policy_unregister_afinfo(&xfrm6_policy_afinfo);
}

void __init xfrm6_init(void)
{
	xfrm6_policy_init();
	xfrm6_state_init();
}

void xfrm6_fini(void)
{
	//xfrm6_input_fini();
	xfrm6_policy_fini();
	xfrm6_state_fini();
}
