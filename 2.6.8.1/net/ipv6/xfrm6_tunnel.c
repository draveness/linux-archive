/*
 * Copyright (C)2003,2004 USAGI/WIDE Project
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
 *
 * Authors	Mitsuru KANDA  <mk@linux-ipv6.org>
 * 		YOSHIFUJI Hideaki <yoshfuji@linux-ipv6.org>
 *
 * Based on net/ipv4/xfrm4_tunnel.c
 *
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/xfrm.h>
#include <linux/list.h>
#include <net/ip.h>
#include <net/xfrm.h>
#include <net/ipv6.h>
#include <net/protocol.h>
#include <linux/ipv6.h>
#include <linux/icmpv6.h>

#ifdef CONFIG_IPV6_XFRM6_TUNNEL_DEBUG
# define X6TDEBUG	3
#else
# define X6TDEBUG	1
#endif

#define X6TPRINTK(fmt, args...)		printk(fmt, ## args)
#define X6TNOPRINTK(fmt, args...)	do { ; } while(0)

#if X6TDEBUG >= 1
# define X6TPRINTK1	X6TPRINTK
#else
# define X6TPRINTK1	X6TNOPRINTK
#endif

#if X6TDEBUG >= 3
# define X6TPRINTK3	X6TPRINTK
#else
# define X6TPRINTK3	X6TNOPRINTK
#endif

/*
 * xfrm_tunnel_spi things are for allocating unique id ("spi") 
 * per xfrm_address_t.
 */
struct xfrm6_tunnel_spi {
	struct hlist_node list_byaddr;
	struct hlist_node list_byspi;
	xfrm_address_t addr;
	u32 spi;
	atomic_t refcnt;
#ifdef XFRM6_TUNNEL_SPI_MAGIC
	u32 magic;
#endif
};

#ifdef CONFIG_IPV6_XFRM6_TUNNEL_DEBUG
# define XFRM6_TUNNEL_SPI_MAGIC 0xdeadbeef
#endif

static rwlock_t xfrm6_tunnel_spi_lock = RW_LOCK_UNLOCKED;

static u32 xfrm6_tunnel_spi;

#define XFRM6_TUNNEL_SPI_MIN	1
#define XFRM6_TUNNEL_SPI_MAX	0xffffffff

static kmem_cache_t *xfrm6_tunnel_spi_kmem;

#define XFRM6_TUNNEL_SPI_BYADDR_HSIZE 256
#define XFRM6_TUNNEL_SPI_BYSPI_HSIZE 256

static struct hlist_head xfrm6_tunnel_spi_byaddr[XFRM6_TUNNEL_SPI_BYADDR_HSIZE];
static struct hlist_head xfrm6_tunnel_spi_byspi[XFRM6_TUNNEL_SPI_BYSPI_HSIZE];

#ifdef XFRM6_TUNNEL_SPI_MAGIC
static int x6spi_check_magic(const struct xfrm6_tunnel_spi *x6spi,
			     const char *name)
{
	if (unlikely(x6spi->magic != XFRM6_TUNNEL_SPI_MAGIC)) {
		X6TPRINTK3(KERN_DEBUG "%s(): x6spi object "
				      "at %p has corrupted magic %08x "
				      "(should be %08x)\n",
			   name, x6spi, x6spi->magic, XFRM6_TUNNEL_SPI_MAGIC);
		return -1;
	}
	return 0;
}
#else
static int inline x6spi_check_magic(const struct xfrm6_tunnel_spi *x6spi,
				    const char *name)
{
	return 0;
}
#endif

#define X6SPI_CHECK_MAGIC(x6spi) x6spi_check_magic((x6spi), __FUNCTION__)


static unsigned inline xfrm6_tunnel_spi_hash_byaddr(xfrm_address_t *addr)
{
	unsigned h;

	X6TPRINTK3(KERN_DEBUG "%s(addr=%p)\n", __FUNCTION__, addr);

	h = addr->a6[0] ^ addr->a6[1] ^ addr->a6[2] ^ addr->a6[3];
	h ^= h >> 16;
	h ^= h >> 8;
	h &= XFRM6_TUNNEL_SPI_BYADDR_HSIZE - 1;

	X6TPRINTK3(KERN_DEBUG "%s() = %u\n", __FUNCTION__, h);

	return h;
}

static unsigned inline xfrm6_tunnel_spi_hash_byspi(u32 spi)
{
	return spi % XFRM6_TUNNEL_SPI_BYSPI_HSIZE;
}


static int xfrm6_tunnel_spi_init(void)
{
	int i;

	X6TPRINTK3(KERN_DEBUG "%s()\n", __FUNCTION__);

	xfrm6_tunnel_spi = 0;
	xfrm6_tunnel_spi_kmem = kmem_cache_create("xfrm6_tunnel_spi",
						  sizeof(struct xfrm6_tunnel_spi),
						  0, SLAB_HWCACHE_ALIGN,
						  NULL, NULL);
	if (!xfrm6_tunnel_spi_kmem) {
		X6TPRINTK1(KERN_ERR
			   "%s(): failed to allocate xfrm6_tunnel_spi_kmem\n",
		           __FUNCTION__);
		return -ENOMEM;
	}

	for (i = 0; i < XFRM6_TUNNEL_SPI_BYADDR_HSIZE; i++)
		INIT_HLIST_HEAD(&xfrm6_tunnel_spi_byaddr[i]);
	for (i = 0; i < XFRM6_TUNNEL_SPI_BYSPI_HSIZE; i++)
		INIT_HLIST_HEAD(&xfrm6_tunnel_spi_byspi[i]);
	return 0;
}

static void xfrm6_tunnel_spi_fini(void)
{
	int i;

	X6TPRINTK3(KERN_DEBUG "%s()\n", __FUNCTION__);

	for (i = 0; i < XFRM6_TUNNEL_SPI_BYADDR_HSIZE; i++) {
		if (!hlist_empty(&xfrm6_tunnel_spi_byaddr[i]))
			goto err;
	}
	for (i = 0; i < XFRM6_TUNNEL_SPI_BYSPI_HSIZE; i++) {
		if (!hlist_empty(&xfrm6_tunnel_spi_byspi[i]))
			goto err;
	}
	kmem_cache_destroy(xfrm6_tunnel_spi_kmem);
	xfrm6_tunnel_spi_kmem = NULL;
	return;
err:
	X6TPRINTK1(KERN_ERR "%s(): table is not empty\n", __FUNCTION__);
	return;
}

static struct xfrm6_tunnel_spi *__xfrm6_tunnel_spi_lookup(xfrm_address_t *saddr)
{
	struct xfrm6_tunnel_spi *x6spi;
	struct hlist_node *pos;

	X6TPRINTK3(KERN_DEBUG "%s(saddr=%p)\n", __FUNCTION__, saddr);

	hlist_for_each_entry(x6spi, pos,
			     &xfrm6_tunnel_spi_byaddr[xfrm6_tunnel_spi_hash_byaddr(saddr)],
			     list_byaddr) {
		if (memcmp(&x6spi->addr, saddr, sizeof(x6spi->addr)) == 0) {
			X6SPI_CHECK_MAGIC(x6spi);
			X6TPRINTK3(KERN_DEBUG "%s() = %p(%u)\n", __FUNCTION__, x6spi, x6spi->spi);
			return x6spi;
		}
	}

	X6TPRINTK3(KERN_DEBUG "%s() = NULL(0)\n", __FUNCTION__);
	return NULL;
}

u32 xfrm6_tunnel_spi_lookup(xfrm_address_t *saddr)
{
	struct xfrm6_tunnel_spi *x6spi;
	u32 spi;

	X6TPRINTK3(KERN_DEBUG "%s(saddr=%p)\n", __FUNCTION__, saddr);

	read_lock_bh(&xfrm6_tunnel_spi_lock);
	x6spi = __xfrm6_tunnel_spi_lookup(saddr);
	spi = x6spi ? x6spi->spi : 0;
	read_unlock_bh(&xfrm6_tunnel_spi_lock);
	return spi;
}

EXPORT_SYMBOL(xfrm6_tunnel_spi_lookup);

static u32 __xfrm6_tunnel_alloc_spi(xfrm_address_t *saddr)
{
	u32 spi;
	struct xfrm6_tunnel_spi *x6spi;
	struct hlist_node *pos;
	unsigned index;

	X6TPRINTK3(KERN_DEBUG "%s(saddr=%p)\n", __FUNCTION__, saddr);

	if (xfrm6_tunnel_spi < XFRM6_TUNNEL_SPI_MIN ||
	    xfrm6_tunnel_spi >= XFRM6_TUNNEL_SPI_MAX)
		xfrm6_tunnel_spi = XFRM6_TUNNEL_SPI_MIN;
	else
		xfrm6_tunnel_spi++;

	for (spi = xfrm6_tunnel_spi; spi <= XFRM6_TUNNEL_SPI_MAX; spi++) {
		index = xfrm6_tunnel_spi_hash_byspi(spi);
		hlist_for_each_entry(x6spi, pos, 
				     &xfrm6_tunnel_spi_byspi[index], 
				     list_byspi) {
			if (x6spi->spi == spi)
				goto try_next_1;
		}
		xfrm6_tunnel_spi = spi;
		goto alloc_spi;
try_next_1:;
	}
	for (spi = XFRM6_TUNNEL_SPI_MIN; spi < xfrm6_tunnel_spi; spi++) {
		index = xfrm6_tunnel_spi_hash_byspi(spi);
		hlist_for_each_entry(x6spi, pos, 
				     &xfrm6_tunnel_spi_byspi[index], 
				     list_byspi) {
			if (x6spi->spi == spi)
				goto try_next_2;
		}
		xfrm6_tunnel_spi = spi;
		goto alloc_spi;
try_next_2:;
	}
	spi = 0;
	goto out;
alloc_spi:
	X6TPRINTK3(KERN_DEBUG "%s(): allocate new spi for "
			      "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x\n", 
			      __FUNCTION__, 
			      NIP6(*(struct in6_addr *)saddr));
	x6spi = kmem_cache_alloc(xfrm6_tunnel_spi_kmem, SLAB_ATOMIC);
	if (!x6spi) {
		X6TPRINTK1(KERN_ERR "%s(): kmem_cache_alloc() failed\n", 
			   __FUNCTION__);
		goto out;
	}
#ifdef XFRM6_TUNNEL_SPI_MAGIC
	x6spi->magic = XFRM6_TUNNEL_SPI_MAGIC;
#endif
	memcpy(&x6spi->addr, saddr, sizeof(x6spi->addr));
	x6spi->spi = spi;
	atomic_set(&x6spi->refcnt, 1);

	hlist_add_head(&x6spi->list_byspi, &xfrm6_tunnel_spi_byspi[index]);

	index = xfrm6_tunnel_spi_hash_byaddr(saddr);
	hlist_add_head(&x6spi->list_byaddr, &xfrm6_tunnel_spi_byaddr[index]);
	X6SPI_CHECK_MAGIC(x6spi);
out:
	X6TPRINTK3(KERN_DEBUG "%s() = %u\n", __FUNCTION__, spi);
	return spi;
}

u32 xfrm6_tunnel_alloc_spi(xfrm_address_t *saddr)
{
	struct xfrm6_tunnel_spi *x6spi;
	u32 spi;

	X6TPRINTK3(KERN_DEBUG "%s(saddr=%p)\n", __FUNCTION__, saddr);

	write_lock_bh(&xfrm6_tunnel_spi_lock);
	x6spi = __xfrm6_tunnel_spi_lookup(saddr);
	if (x6spi) {
		atomic_inc(&x6spi->refcnt);
		spi = x6spi->spi;
	} else
		spi = __xfrm6_tunnel_alloc_spi(saddr);
	write_unlock_bh(&xfrm6_tunnel_spi_lock);

	X6TPRINTK3(KERN_DEBUG "%s() = %u\n", __FUNCTION__, spi);

	return spi;
}

EXPORT_SYMBOL(xfrm6_tunnel_alloc_spi);

void xfrm6_tunnel_free_spi(xfrm_address_t *saddr)
{
	struct xfrm6_tunnel_spi *x6spi;
	struct hlist_node *pos, *n;

	X6TPRINTK3(KERN_DEBUG "%s(saddr=%p)\n", __FUNCTION__, saddr);

	write_lock_bh(&xfrm6_tunnel_spi_lock);

	hlist_for_each_entry_safe(x6spi, pos, n, 
				  &xfrm6_tunnel_spi_byaddr[xfrm6_tunnel_spi_hash_byaddr(saddr)],
				  list_byaddr)
	{
		if (memcmp(&x6spi->addr, saddr, sizeof(x6spi->addr)) == 0) {
			X6TPRINTK3(KERN_DEBUG "%s(): x6spi object "
					      "for %04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x "
					      "found at %p\n",
				   __FUNCTION__, 
				   NIP6(*(struct in6_addr *)saddr),
				   x6spi);
			X6SPI_CHECK_MAGIC(x6spi);
			if (atomic_dec_and_test(&x6spi->refcnt)) {
				hlist_del(&x6spi->list_byaddr);
				hlist_del(&x6spi->list_byspi);
				kmem_cache_free(xfrm6_tunnel_spi_kmem, x6spi);
				break;
			}
		}
	}
	write_unlock_bh(&xfrm6_tunnel_spi_lock);
}

EXPORT_SYMBOL(xfrm6_tunnel_free_spi);

static int xfrm6_tunnel_output(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct ipv6hdr *top_iph;

	top_iph = (struct ipv6hdr *)skb->data;
	top_iph->payload_len = htons(skb->len - sizeof(struct ipv6hdr));

	return 0;
}

static int xfrm6_tunnel_input(struct xfrm_state *x, struct xfrm_decap_state *decap, struct sk_buff *skb)
{
	if (!pskb_may_pull(skb, sizeof(struct ipv6hdr))) 
		return -EINVAL;

	skb->mac.raw = skb->nh.raw;
	skb->nh.raw = skb->data;
	dst_release(skb->dst);
	skb->dst = NULL;
	skb->protocol = htons(ETH_P_IPV6);
	skb->pkt_type = PACKET_HOST;
	netif_rx(skb);

	return 0;
}

static struct xfrm6_tunnel *xfrm6_tunnel_handler;
static DECLARE_MUTEX(xfrm6_tunnel_sem);

int xfrm6_tunnel_register(struct xfrm6_tunnel *handler)
{
	int ret;

	down(&xfrm6_tunnel_sem);
	ret = 0;
	if (xfrm6_tunnel_handler != NULL)
		ret = -EINVAL;
	if (!ret)
		xfrm6_tunnel_handler = handler;
	up(&xfrm6_tunnel_sem);

	return ret;
}

EXPORT_SYMBOL(xfrm6_tunnel_register);

int xfrm6_tunnel_deregister(struct xfrm6_tunnel *handler)
{
	int ret;

	down(&xfrm6_tunnel_sem);
	ret = 0;
	if (xfrm6_tunnel_handler != handler)
		ret = -EINVAL;
	if (!ret)
		xfrm6_tunnel_handler = NULL;
	up(&xfrm6_tunnel_sem);

	synchronize_net();

	return ret;
}

EXPORT_SYMBOL(xfrm6_tunnel_deregister);

static int xfrm6_tunnel_rcv(struct sk_buff **pskb, unsigned int *nhoffp)
{
	struct sk_buff *skb = *pskb;
	struct xfrm6_tunnel *handler = xfrm6_tunnel_handler;
	struct xfrm_state *x = NULL;
	struct ipv6hdr *iph = skb->nh.ipv6h;
	int err = 0;
	u32 spi;

	/* device-like_ip6ip6_handler() */
	if (handler) {
		err = handler->handler(pskb, nhoffp);
		if (!err)
			goto out;
	}

	spi = xfrm6_tunnel_spi_lookup((xfrm_address_t *)&iph->saddr);
	x = xfrm_state_lookup((xfrm_address_t *)&iph->daddr, 
			spi,
			IPPROTO_IPV6, AF_INET6);

	if (!x)
		goto drop;

	spin_lock(&x->lock);

	if (unlikely(x->km.state != XFRM_STATE_VALID))
		goto drop_unlock;

	err = xfrm6_tunnel_input(x, NULL, skb);
	if (err)
		goto drop_unlock;

	x->curlft.bytes += skb->len;
	x->curlft.packets++; 
	spin_unlock(&x->lock); 
	xfrm_state_put(x); 

out:
	return 0;

drop_unlock:
	spin_unlock(&x->lock);
	xfrm_state_put(x);
drop:
	kfree_skb(skb);
	return -1;
}

static void xfrm6_tunnel_err(struct sk_buff *skb, struct inet6_skb_parm *opt,
			     int type, int code, int offset, __u32 info)
{
	struct xfrm6_tunnel *handler = xfrm6_tunnel_handler;

	/* call here first for device-like ip6ip6 err handling */
	if (handler) {
		handler->err_handler(skb, opt, type, code, offset, info);
		return;
	}

	/* xfrm6_tunnel native err handling */
	switch (type) {
	case ICMPV6_DEST_UNREACH: 
		switch (code) {
		case ICMPV6_NOROUTE: 
		case ICMPV6_ADM_PROHIBITED:
		case ICMPV6_NOT_NEIGHBOUR:
		case ICMPV6_ADDR_UNREACH:
		case ICMPV6_PORT_UNREACH:
		default:
			X6TPRINTK3(KERN_DEBUG
				   "xfrm6_tunnel: Destination Unreach.\n");
			break;
		}
		break;
	case ICMPV6_PKT_TOOBIG:
			X6TPRINTK3(KERN_DEBUG 
				   "xfrm6_tunnel: Packet Too Big.\n");
		break;
	case ICMPV6_TIME_EXCEED:
		switch (code) {
		case ICMPV6_EXC_HOPLIMIT:
			X6TPRINTK3(KERN_DEBUG
				   "xfrm6_tunnel: Too small Hoplimit.\n");
			break;
		case ICMPV6_EXC_FRAGTIME:
		default: 
			break;
		}
		break;
	case ICMPV6_PARAMPROB:
		switch (code) {
		case ICMPV6_HDR_FIELD: break;
		case ICMPV6_UNK_NEXTHDR: break;
		case ICMPV6_UNK_OPTION: break;
		}
		break;
	default:
		break;
	}
	return;
}

static int xfrm6_tunnel_init_state(struct xfrm_state *x, void *args)
{
	if (!x->props.mode)
		return -EINVAL;

	x->props.header_len = sizeof(struct ipv6hdr);

	return 0;
}

static void xfrm6_tunnel_destroy(struct xfrm_state *x)
{
	xfrm6_tunnel_free_spi((xfrm_address_t *)&x->props.saddr);
}

static struct xfrm_type xfrm6_tunnel_type = {
	.description	= "IP6IP6",
	.owner          = THIS_MODULE,
	.proto		= IPPROTO_IPV6,
	.init_state	= xfrm6_tunnel_init_state,
	.destructor	= xfrm6_tunnel_destroy,
	.input		= xfrm6_tunnel_input,
	.output		= xfrm6_tunnel_output,
};

static struct inet6_protocol xfrm6_tunnel_protocol = {
	.handler	= xfrm6_tunnel_rcv,
	.err_handler	= xfrm6_tunnel_err, 
	.flags          = INET6_PROTO_NOPOLICY|INET6_PROTO_FINAL,
};

void __init xfrm6_tunnel_init(void)
{
	X6TPRINTK3(KERN_DEBUG "%s()\n", __FUNCTION__);

	if (xfrm_register_type(&xfrm6_tunnel_type, AF_INET6) < 0) {
		X6TPRINTK1(KERN_ERR
			   "xfrm6_tunnel init: can't add xfrm type\n");
		return;
	}
	if (inet6_add_protocol(&xfrm6_tunnel_protocol, IPPROTO_IPV6) < 0) {
		X6TPRINTK1(KERN_ERR
			   "xfrm6_tunnel init(): can't add protocol\n");
		xfrm_unregister_type(&xfrm6_tunnel_type, AF_INET6);
		return;
	}
	if (xfrm6_tunnel_spi_init() < 0) {
		X6TPRINTK1(KERN_ERR
			   "xfrm6_tunnel init: failed to initialize spi\n");
		inet6_del_protocol(&xfrm6_tunnel_protocol, IPPROTO_IPV6);
		xfrm_unregister_type(&xfrm6_tunnel_type, AF_INET6);
		return;
	}
}

void __exit xfrm6_tunnel_fini(void)
{
	X6TPRINTK3(KERN_DEBUG "%s()\n", __FUNCTION__);

	xfrm6_tunnel_spi_fini();
	if (inet6_del_protocol(&xfrm6_tunnel_protocol, IPPROTO_IPV6) < 0)
		X6TPRINTK1(KERN_ERR 
			   "xfrm6_tunnel close: can't remove protocol\n");
	if (xfrm_unregister_type(&xfrm6_tunnel_type, AF_INET6) < 0)
		X6TPRINTK1(KERN_ERR
			   "xfrm6_tunnel close: can't remove xfrm type\n");
}
