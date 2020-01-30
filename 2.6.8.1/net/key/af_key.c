/*
 * net/key/af_key.c	An implementation of PF_KEYv2 sockets.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Authors:	Maxim Giryaev	<gem@asplinux.ru>
 *		David S. Miller	<davem@redhat.com>
 *		Alexey Kuznetsov <kuznet@ms2.inr.ac.ru>
 *		Kunihiro Ishiguro <kunihiro@ipinfusion.com>
 *		Kazunori MIYAZAWA / USAGI Project <miyazawa@linux-ipv6.org>
 *		Derek Atkins <derek@ihtfp.com>
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/socket.h>
#include <linux/pfkeyv2.h>
#include <linux/ipsec.h>
#include <linux/skbuff.h>
#include <linux/rtnetlink.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/proc_fs.h>
#include <linux/init.h>
#include <net/xfrm.h>

#include <net/sock.h>

#define _X2KEY(x) ((x) == XFRM_INF ? 0 : (x))
#define _KEY2X(x) ((x) == 0 ? XFRM_INF : (x))


/* List of all pfkey sockets. */
HLIST_HEAD(pfkey_table);
static DECLARE_WAIT_QUEUE_HEAD(pfkey_table_wait);
static rwlock_t pfkey_table_lock = RW_LOCK_UNLOCKED;
static atomic_t pfkey_table_users = ATOMIC_INIT(0);

static atomic_t pfkey_socks_nr = ATOMIC_INIT(0);

struct pfkey_opt {
	int	registered;
	int	promisc;
};
#define pfkey_sk(__sk) ((struct pfkey_opt *)(__sk)->sk_protinfo)

static void pfkey_sock_destruct(struct sock *sk)
{
	skb_queue_purge(&sk->sk_receive_queue);

	if (!sock_flag(sk, SOCK_DEAD)) {
		printk("Attempt to release alive pfkey socket: %p\n", sk);
		return;
	}

	BUG_TRAP(!atomic_read(&sk->sk_rmem_alloc));
	BUG_TRAP(!atomic_read(&sk->sk_wmem_alloc));

	kfree(pfkey_sk(sk));

	atomic_dec(&pfkey_socks_nr);
}

static void pfkey_table_grab(void)
{
	write_lock_bh(&pfkey_table_lock);

	if (atomic_read(&pfkey_table_users)) {
		DECLARE_WAITQUEUE(wait, current);

		add_wait_queue_exclusive(&pfkey_table_wait, &wait);
		for(;;) {
			set_current_state(TASK_UNINTERRUPTIBLE);
			if (atomic_read(&pfkey_table_users) == 0)
				break;
			write_unlock_bh(&pfkey_table_lock);
			schedule();
			write_lock_bh(&pfkey_table_lock);
		}

		__set_current_state(TASK_RUNNING);
		remove_wait_queue(&pfkey_table_wait, &wait);
	}
}

static __inline__ void pfkey_table_ungrab(void)
{
	write_unlock_bh(&pfkey_table_lock);
	wake_up(&pfkey_table_wait);
}

static __inline__ void pfkey_lock_table(void)
{
	/* read_lock() synchronizes us to pfkey_table_grab */

	read_lock(&pfkey_table_lock);
	atomic_inc(&pfkey_table_users);
	read_unlock(&pfkey_table_lock);
}

static __inline__ void pfkey_unlock_table(void)
{
	if (atomic_dec_and_test(&pfkey_table_users))
		wake_up(&pfkey_table_wait);
}


static struct proto_ops pfkey_ops;

static void pfkey_insert(struct sock *sk)
{
	pfkey_table_grab();
	sk_add_node(sk, &pfkey_table);
	pfkey_table_ungrab();
}

static void pfkey_remove(struct sock *sk)
{
	pfkey_table_grab();
	sk_del_node_init(sk);
	pfkey_table_ungrab();
}

static int pfkey_create(struct socket *sock, int protocol)
{
	struct sock *sk;
	struct pfkey_opt *pfk;
	int err;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;
	if (sock->type != SOCK_RAW)
		return -ESOCKTNOSUPPORT;
	if (protocol != PF_KEY_V2)
		return -EPROTONOSUPPORT;

	err = -ENOMEM;
	sk = sk_alloc(PF_KEY, GFP_KERNEL, 1, NULL);
	if (sk == NULL)
		goto out;
	
	sock->ops = &pfkey_ops;
	sock_init_data(sock, sk);
	sk_set_owner(sk, THIS_MODULE);

	err = -ENOMEM;
	pfk = sk->sk_protinfo = kmalloc(sizeof(*pfk), GFP_KERNEL);
	if (!pfk) {
		sk_free(sk);
		goto out;
	}
	memset(pfk, 0, sizeof(*pfk));

	sk->sk_family = PF_KEY;
	sk->sk_destruct = pfkey_sock_destruct;

	atomic_inc(&pfkey_socks_nr);

	pfkey_insert(sk);

	return 0;
out:
	return err;
}

static int pfkey_release(struct socket *sock)
{
	struct sock *sk = sock->sk;

	if (!sk)
		return 0;

	pfkey_remove(sk);

	sock_orphan(sk);
	sock->sk = NULL;
	skb_queue_purge(&sk->sk_write_queue);
	sock_put(sk);

	return 0;
}

static int pfkey_broadcast_one(struct sk_buff *skb, struct sk_buff **skb2,
			       int allocation, struct sock *sk)
{
	int err = -ENOBUFS;

	sock_hold(sk);
	if (*skb2 == NULL) {
		if (atomic_read(&skb->users) != 1) {
			*skb2 = skb_clone(skb, allocation);
		} else {
			*skb2 = skb;
			atomic_inc(&skb->users);
		}
	}
	if (*skb2 != NULL) {
		if (atomic_read(&sk->sk_rmem_alloc) <= sk->sk_rcvbuf) {
			skb_orphan(*skb2);
			skb_set_owner_r(*skb2, sk);
			skb_queue_tail(&sk->sk_receive_queue, *skb2);
			sk->sk_data_ready(sk, (*skb2)->len);
			*skb2 = NULL;
			err = 0;
		}
	}
	sock_put(sk);
	return err;
}

/* Send SKB to all pfkey sockets matching selected criteria.  */
#define BROADCAST_ALL		0
#define BROADCAST_ONE		1
#define BROADCAST_REGISTERED	2
#define BROADCAST_PROMISC_ONLY	4
static int pfkey_broadcast(struct sk_buff *skb, int allocation,
			   int broadcast_flags, struct sock *one_sk)
{
	struct sock *sk;
	struct hlist_node *node;
	struct sk_buff *skb2 = NULL;
	int err = -ESRCH;

	/* XXX Do we need something like netlink_overrun?  I think
	 * XXX PF_KEY socket apps will not mind current behavior.
	 */
	if (!skb)
		return -ENOMEM;

	pfkey_lock_table();
	sk_for_each(sk, node, &pfkey_table) {
		struct pfkey_opt *pfk = pfkey_sk(sk);
		int err2;

		/* Yes, it means that if you are meant to receive this
		 * pfkey message you receive it twice as promiscuous
		 * socket.
		 */
		if (pfk->promisc)
			pfkey_broadcast_one(skb, &skb2, allocation, sk);

		/* the exact target will be processed later */
		if (sk == one_sk)
			continue;
		if (broadcast_flags != BROADCAST_ALL) {
			if (broadcast_flags & BROADCAST_PROMISC_ONLY)
				continue;
			if ((broadcast_flags & BROADCAST_REGISTERED) &&
			    !pfk->registered)
				continue;
			if (broadcast_flags & BROADCAST_ONE)
				continue;
		}

		err2 = pfkey_broadcast_one(skb, &skb2, allocation, sk);

		/* Error is cleare after succecful sending to at least one
		 * registered KM */
		if ((broadcast_flags & BROADCAST_REGISTERED) && err)
			err = err2;
	}
	pfkey_unlock_table();

	if (one_sk != NULL)
		err = pfkey_broadcast_one(skb, &skb2, allocation, one_sk);

	if (skb2)
		kfree_skb(skb2);
	kfree_skb(skb);
	return err;
}

static inline void pfkey_hdr_dup(struct sadb_msg *new, struct sadb_msg *orig)
{
	*new = *orig;
}

static int pfkey_error(struct sadb_msg *orig, int err, struct sock *sk)
{
	struct sk_buff *skb = alloc_skb(sizeof(struct sadb_msg) + 16, GFP_KERNEL);
	struct sadb_msg *hdr;

	if (!skb)
		return -ENOBUFS;

	/* Woe be to the platform trying to support PFKEY yet
	 * having normal errnos outside the 1-255 range, inclusive.
	 */
	err = -err;
	if (err == ERESTARTSYS ||
	    err == ERESTARTNOHAND ||
	    err == ERESTARTNOINTR)
		err = EINTR;
	if (err >= 512)
		err = EINVAL;
	if (err <= 0 || err >= 256)
		BUG();

	hdr = (struct sadb_msg *) skb_put(skb, sizeof(struct sadb_msg));
	pfkey_hdr_dup(hdr, orig);
	hdr->sadb_msg_errno = (uint8_t) err;
	hdr->sadb_msg_len = (sizeof(struct sadb_msg) /
			     sizeof(uint64_t));

	pfkey_broadcast(skb, GFP_KERNEL, BROADCAST_ONE, sk);

	return 0;
}

static u8 sadb_ext_min_len[] = {
	[SADB_EXT_RESERVED]		= (u8) 0,
	[SADB_EXT_SA]			= (u8) sizeof(struct sadb_sa),
	[SADB_EXT_LIFETIME_CURRENT]	= (u8) sizeof(struct sadb_lifetime),
	[SADB_EXT_LIFETIME_HARD]	= (u8) sizeof(struct sadb_lifetime),
	[SADB_EXT_LIFETIME_SOFT]	= (u8) sizeof(struct sadb_lifetime),
	[SADB_EXT_ADDRESS_SRC]		= (u8) sizeof(struct sadb_address),
	[SADB_EXT_ADDRESS_DST]		= (u8) sizeof(struct sadb_address),
	[SADB_EXT_ADDRESS_PROXY]	= (u8) sizeof(struct sadb_address),
	[SADB_EXT_KEY_AUTH]		= (u8) sizeof(struct sadb_key),
	[SADB_EXT_KEY_ENCRYPT]		= (u8) sizeof(struct sadb_key),
	[SADB_EXT_IDENTITY_SRC]		= (u8) sizeof(struct sadb_ident),
	[SADB_EXT_IDENTITY_DST]		= (u8) sizeof(struct sadb_ident),
	[SADB_EXT_SENSITIVITY]		= (u8) sizeof(struct sadb_sens),
	[SADB_EXT_PROPOSAL]		= (u8) sizeof(struct sadb_prop),
	[SADB_EXT_SUPPORTED_AUTH]	= (u8) sizeof(struct sadb_supported),
	[SADB_EXT_SUPPORTED_ENCRYPT]	= (u8) sizeof(struct sadb_supported),
	[SADB_EXT_SPIRANGE]		= (u8) sizeof(struct sadb_spirange),
	[SADB_X_EXT_KMPRIVATE]		= (u8) sizeof(struct sadb_x_kmprivate),
	[SADB_X_EXT_POLICY]		= (u8) sizeof(struct sadb_x_policy),
	[SADB_X_EXT_SA2]		= (u8) sizeof(struct sadb_x_sa2),
	[SADB_X_EXT_NAT_T_TYPE]		= (u8) sizeof(struct sadb_x_nat_t_type),
	[SADB_X_EXT_NAT_T_SPORT]	= (u8) sizeof(struct sadb_x_nat_t_port),
	[SADB_X_EXT_NAT_T_DPORT]	= (u8) sizeof(struct sadb_x_nat_t_port),
	[SADB_X_EXT_NAT_T_OA]		= (u8) sizeof(struct sadb_address),
};

/* Verify sadb_address_{len,prefixlen} against sa_family.  */
static int verify_address_len(void *p)
{
	struct sadb_address *sp = p;
	struct sockaddr *addr = (struct sockaddr *)(sp + 1);
	struct sockaddr_in *sin;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	struct sockaddr_in6 *sin6;
#endif
	int len;

	switch (addr->sa_family) {
	case AF_INET:
		len  = sizeof(*sp) + sizeof(*sin) + (sizeof(uint64_t) - 1);
		len /= sizeof(uint64_t);
		if (sp->sadb_address_len != len ||
		    sp->sadb_address_prefixlen > 32)
			return -EINVAL;
		break;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	case AF_INET6:
		len  = sizeof(*sp) + sizeof(*sin6) + (sizeof(uint64_t) - 1);
		len /= sizeof(uint64_t);
		if (sp->sadb_address_len != len ||
		    sp->sadb_address_prefixlen > 128)
			return -EINVAL;
		break;
#endif
	default:
		/* It is user using kernel to keep track of security
		 * associations for another protocol, such as
		 * OSPF/RSVP/RIPV2/MIP.  It is user's job to verify
		 * lengths.
		 *
		 * XXX Actually, association/policy database is not yet
		 * XXX able to cope with arbitrary sockaddr families.
		 * XXX When it can, remove this -EINVAL.  -DaveM
		 */
		return -EINVAL;
		break;
	};

	return 0;
}

static int present_and_same_family(struct sadb_address *src,
				   struct sadb_address *dst)
{
	struct sockaddr *s_addr, *d_addr;

	if (!src || !dst)
		return 0;

	s_addr = (struct sockaddr *)(src + 1);
	d_addr = (struct sockaddr *)(dst + 1);
	if (s_addr->sa_family != d_addr->sa_family)
		return 0;
	if (s_addr->sa_family != AF_INET
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	    && s_addr->sa_family != AF_INET6
#endif
		)
		return 0;

	return 1;
}

static int parse_exthdrs(struct sk_buff *skb, struct sadb_msg *hdr, void **ext_hdrs)
{
	char *p = (char *) hdr;
	int len = skb->len;

	len -= sizeof(*hdr);
	p += sizeof(*hdr);
	while (len > 0) {
		struct sadb_ext *ehdr = (struct sadb_ext *) p;
		uint16_t ext_type;
		int ext_len;

		ext_len  = ehdr->sadb_ext_len;
		ext_len *= sizeof(uint64_t);
		ext_type = ehdr->sadb_ext_type;
		if (ext_len < sizeof(uint64_t) ||
		    ext_len > len ||
		    ext_type == SADB_EXT_RESERVED)
			return -EINVAL;

		if (ext_type <= SADB_EXT_MAX) {
			int min = (int) sadb_ext_min_len[ext_type];
			if (ext_len < min)
				return -EINVAL;
			if (ext_hdrs[ext_type-1] != NULL)
				return -EINVAL;
			if (ext_type == SADB_EXT_ADDRESS_SRC ||
			    ext_type == SADB_EXT_ADDRESS_DST ||
			    ext_type == SADB_EXT_ADDRESS_PROXY ||
			    ext_type == SADB_X_EXT_NAT_T_OA) {
				if (verify_address_len(p))
					return -EINVAL;
			}				
			ext_hdrs[ext_type-1] = p;
		}
		p   += ext_len;
		len -= ext_len;
	}

	return 0;
}

static uint16_t
pfkey_satype2proto(uint8_t satype)
{
	switch (satype) {
	case SADB_SATYPE_UNSPEC:
		return IPSEC_PROTO_ANY;
	case SADB_SATYPE_AH:
		return IPPROTO_AH;
	case SADB_SATYPE_ESP:
		return IPPROTO_ESP;
	case SADB_X_SATYPE_IPCOMP:
		return IPPROTO_COMP;
		break;
	default:
		return 0;
	}
	/* NOTREACHED */
}

static uint8_t
pfkey_proto2satype(uint16_t proto)
{
	switch (proto) {
	case IPPROTO_AH:
		return SADB_SATYPE_AH;
	case IPPROTO_ESP:
		return SADB_SATYPE_ESP;
	case IPPROTO_COMP:
		return SADB_X_SATYPE_IPCOMP;
		break;
	default:
		return 0;
	}
	/* NOTREACHED */
}

/* BTW, this scheme means that there is no way with PFKEY2 sockets to
 * say specifically 'just raw sockets' as we encode them as 255.
 */

static uint8_t pfkey_proto_to_xfrm(uint8_t proto)
{
	return (proto == IPSEC_PROTO_ANY ? 0 : proto);
}

static uint8_t pfkey_proto_from_xfrm(uint8_t proto)
{
	return (proto ? proto : IPSEC_PROTO_ANY);
}

static int pfkey_sadb_addr2xfrm_addr(struct sadb_address *addr,
				     xfrm_address_t *xaddr)
{
	switch (((struct sockaddr*)(addr + 1))->sa_family) {
	case AF_INET:
		xaddr->a4 = 
			((struct sockaddr_in *)(addr + 1))->sin_addr.s_addr;
		return AF_INET;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	case AF_INET6:
		memcpy(xaddr->a6, 
		       &((struct sockaddr_in6 *)(addr + 1))->sin6_addr,
		       sizeof(struct in6_addr));
		return AF_INET6;
#endif
	default:
		return 0;
	}
	/* NOTREACHED */
}

static struct  xfrm_state *pfkey_xfrm_state_lookup(struct sadb_msg *hdr, void **ext_hdrs)
{
	struct sadb_sa *sa;
	struct sadb_address *addr;
	uint16_t proto;
	unsigned short family;
	xfrm_address_t *xaddr;

	sa = (struct sadb_sa *) ext_hdrs[SADB_EXT_SA-1];
	if (sa == NULL)
		return NULL;

	proto = pfkey_satype2proto(hdr->sadb_msg_satype);
	if (proto == 0)
		return NULL;

	/* sadb_address_len should be checked by caller */
	addr = (struct sadb_address *) ext_hdrs[SADB_EXT_ADDRESS_DST-1];
	if (addr == NULL)
		return NULL;

	family = ((struct sockaddr *)(addr + 1))->sa_family;
	switch (family) {
	case AF_INET:
		xaddr = (xfrm_address_t *)&((struct sockaddr_in *)(addr + 1))->sin_addr;
		break;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	case AF_INET6:
		xaddr = (xfrm_address_t *)&((struct sockaddr_in6 *)(addr + 1))->sin6_addr;
		break;
#endif
	default:
		xaddr = NULL;
	}

	if (!xaddr)
		return NULL;

	return xfrm_state_lookup(xaddr, sa->sadb_sa_spi, proto, family);
}

#define PFKEY_ALIGN8(a) (1 + (((a) - 1) | (8 - 1)))
static int
pfkey_sockaddr_size(sa_family_t family)
{
	switch (family) {
	case AF_INET:
		return PFKEY_ALIGN8(sizeof(struct sockaddr_in));
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	case AF_INET6:
		return PFKEY_ALIGN8(sizeof(struct sockaddr_in6));
#endif
	default:
		return 0;
	}
	/* NOTREACHED */
}

static struct sk_buff * pfkey_xfrm_state2msg(struct xfrm_state *x, int add_keys, int hsc)
{
	struct sk_buff *skb;
	struct sadb_msg *hdr;
	struct sadb_sa *sa;
	struct sadb_lifetime *lifetime;
	struct sadb_address *addr;
	struct sadb_key *key;
	struct sadb_x_sa2 *sa2;
	struct sockaddr_in *sin;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	struct sockaddr_in6 *sin6;
#endif
	int size;
	int auth_key_size = 0;
	int encrypt_key_size = 0;
	int sockaddr_size;
	struct xfrm_encap_tmpl *natt = NULL;

	/* address family check */
	sockaddr_size = pfkey_sockaddr_size(x->props.family);
	if (!sockaddr_size)
		ERR_PTR(-EINVAL);

	/* base, SA, (lifetime (HSC),) address(SD), (address(P),)
	   key(AE), (identity(SD),) (sensitivity)> */
	size = sizeof(struct sadb_msg) +sizeof(struct sadb_sa) + 
		sizeof(struct sadb_lifetime) +
		((hsc & 1) ? sizeof(struct sadb_lifetime) : 0) +
		((hsc & 2) ? sizeof(struct sadb_lifetime) : 0) +
			sizeof(struct sadb_address)*2 + 
				sockaddr_size*2 +
					sizeof(struct sadb_x_sa2);
	/* identity & sensitivity */

	if ((x->props.family == AF_INET &&
	     x->sel.saddr.a4 != x->props.saddr.a4)
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	    || (x->props.family == AF_INET6 &&
		memcmp (x->sel.saddr.a6, x->props.saddr.a6, sizeof (struct in6_addr)))
#endif
		)
		size += sizeof(struct sadb_address) + sockaddr_size;

	if (add_keys) {
		if (x->aalg && x->aalg->alg_key_len) {
			auth_key_size = 
				PFKEY_ALIGN8((x->aalg->alg_key_len + 7) / 8); 
			size += sizeof(struct sadb_key) + auth_key_size;
		}
		if (x->ealg && x->ealg->alg_key_len) {
			encrypt_key_size = 
				PFKEY_ALIGN8((x->ealg->alg_key_len+7) / 8); 
			size += sizeof(struct sadb_key) + encrypt_key_size;
		}
	}
	if (x->encap)
		natt = x->encap;

	if (natt && natt->encap_type) {
		size += sizeof(struct sadb_x_nat_t_type);
		size += sizeof(struct sadb_x_nat_t_port);
		size += sizeof(struct sadb_x_nat_t_port);
	}

	skb =  alloc_skb(size + 16, GFP_ATOMIC);
	if (skb == NULL)
		return ERR_PTR(-ENOBUFS);

	/* call should fill header later */
	hdr = (struct sadb_msg *) skb_put(skb, sizeof(struct sadb_msg));
	memset(hdr, 0, size);	/* XXX do we need this ? */
	hdr->sadb_msg_len = size / sizeof(uint64_t);

	/* sa */
	sa = (struct sadb_sa *)  skb_put(skb, sizeof(struct sadb_sa));
	sa->sadb_sa_len = sizeof(struct sadb_sa)/sizeof(uint64_t);
	sa->sadb_sa_exttype = SADB_EXT_SA;
	sa->sadb_sa_spi = x->id.spi;
	sa->sadb_sa_replay = x->props.replay_window;
	sa->sadb_sa_state = SADB_SASTATE_DYING;
	if (x->km.state == XFRM_STATE_VALID && !x->km.dying)
		sa->sadb_sa_state = SADB_SASTATE_MATURE;
	else if (x->km.state == XFRM_STATE_ACQ)
		sa->sadb_sa_state = SADB_SASTATE_LARVAL;
	else if (x->km.state == XFRM_STATE_EXPIRED)
		sa->sadb_sa_state = SADB_SASTATE_DEAD;
	sa->sadb_sa_auth = 0;
	if (x->aalg) {
		struct xfrm_algo_desc *a = xfrm_aalg_get_byname(x->aalg->alg_name);
		sa->sadb_sa_auth = a ? a->desc.sadb_alg_id : 0;
	}
	sa->sadb_sa_encrypt = 0;
	BUG_ON(x->ealg && x->calg);
	if (x->ealg) {
		struct xfrm_algo_desc *a = xfrm_ealg_get_byname(x->ealg->alg_name);
		sa->sadb_sa_encrypt = a ? a->desc.sadb_alg_id : 0;
	}
	/* KAME compatible: sadb_sa_encrypt is overloaded with calg id */
	if (x->calg) {
		struct xfrm_algo_desc *a = xfrm_calg_get_byname(x->calg->alg_name);
		sa->sadb_sa_encrypt = a ? a->desc.sadb_alg_id : 0;
	}

	sa->sadb_sa_flags = 0;
	if (x->props.flags & XFRM_STATE_NOECN)
		sa->sadb_sa_flags |= SADB_SAFLAGS_NOECN;

	/* hard time */
	if (hsc & 2) {
		lifetime = (struct sadb_lifetime *)  skb_put(skb, 
							     sizeof(struct sadb_lifetime));
		lifetime->sadb_lifetime_len =
			sizeof(struct sadb_lifetime)/sizeof(uint64_t);
		lifetime->sadb_lifetime_exttype = SADB_EXT_LIFETIME_HARD;
		lifetime->sadb_lifetime_allocations =  _X2KEY(x->lft.hard_packet_limit);
		lifetime->sadb_lifetime_bytes = _X2KEY(x->lft.hard_byte_limit);
		lifetime->sadb_lifetime_addtime = x->lft.hard_add_expires_seconds;
		lifetime->sadb_lifetime_usetime = x->lft.hard_use_expires_seconds;
	}
	/* soft time */
	if (hsc & 1) {
		lifetime = (struct sadb_lifetime *)  skb_put(skb, 
							     sizeof(struct sadb_lifetime));
		lifetime->sadb_lifetime_len =
			sizeof(struct sadb_lifetime)/sizeof(uint64_t);
		lifetime->sadb_lifetime_exttype = SADB_EXT_LIFETIME_SOFT;
		lifetime->sadb_lifetime_allocations =  _X2KEY(x->lft.soft_packet_limit);
		lifetime->sadb_lifetime_bytes = _X2KEY(x->lft.soft_byte_limit);
		lifetime->sadb_lifetime_addtime = x->lft.soft_add_expires_seconds;
		lifetime->sadb_lifetime_usetime = x->lft.soft_use_expires_seconds;
	}
	/* current time */
	lifetime = (struct sadb_lifetime *)  skb_put(skb,
						     sizeof(struct sadb_lifetime));
	lifetime->sadb_lifetime_len =
		sizeof(struct sadb_lifetime)/sizeof(uint64_t);
	lifetime->sadb_lifetime_exttype = SADB_EXT_LIFETIME_CURRENT;
	lifetime->sadb_lifetime_allocations = x->curlft.packets;
	lifetime->sadb_lifetime_bytes = x->curlft.bytes;
	lifetime->sadb_lifetime_addtime = x->curlft.add_time;
	lifetime->sadb_lifetime_usetime = x->curlft.use_time;
	/* src address */
	addr = (struct sadb_address*) skb_put(skb, 
					      sizeof(struct sadb_address)+sockaddr_size);
	addr->sadb_address_len = 
		(sizeof(struct sadb_address)+sockaddr_size)/
			sizeof(uint64_t);
	addr->sadb_address_exttype = SADB_EXT_ADDRESS_SRC;
	/* "if the ports are non-zero, then the sadb_address_proto field, 
	   normally zero, MUST be filled in with the transport 
	   protocol's number." - RFC2367 */
	addr->sadb_address_proto = 0; 
	addr->sadb_address_reserved = 0;
	if (x->props.family == AF_INET) {
		addr->sadb_address_prefixlen = 32;

		sin = (struct sockaddr_in *) (addr + 1);
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = x->props.saddr.a4;
		sin->sin_port = 0;
		memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
	}
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	else if (x->props.family == AF_INET6) {
 		addr->sadb_address_prefixlen = 128;

		sin6 = (struct sockaddr_in6 *) (addr + 1);
		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = 0;
		sin6->sin6_flowinfo = 0;
 		memcpy(&sin6->sin6_addr, x->props.saddr.a6,
		       sizeof(struct in6_addr));
		sin6->sin6_scope_id = 0;
 	}
#endif
	else
		BUG();

	/* dst address */
	addr = (struct sadb_address*) skb_put(skb, 
					      sizeof(struct sadb_address)+sockaddr_size);
	addr->sadb_address_len = 
		(sizeof(struct sadb_address)+sockaddr_size)/
			sizeof(uint64_t);
	addr->sadb_address_exttype = SADB_EXT_ADDRESS_DST;
	addr->sadb_address_proto = 0; 
	addr->sadb_address_prefixlen = 32; /* XXX */ 
	addr->sadb_address_reserved = 0;
	if (x->props.family == AF_INET) {
		sin = (struct sockaddr_in *) (addr + 1);
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = x->id.daddr.a4;
		sin->sin_port = 0;
		memset(sin->sin_zero, 0, sizeof(sin->sin_zero));

		if (x->sel.saddr.a4 != x->props.saddr.a4) {
			addr = (struct sadb_address*) skb_put(skb, 
				sizeof(struct sadb_address)+sockaddr_size);
			addr->sadb_address_len = 
				(sizeof(struct sadb_address)+sockaddr_size)/
				sizeof(uint64_t);
			addr->sadb_address_exttype = SADB_EXT_ADDRESS_PROXY;
			addr->sadb_address_proto =
				pfkey_proto_from_xfrm(x->sel.proto);
			addr->sadb_address_prefixlen = x->sel.prefixlen_s;
			addr->sadb_address_reserved = 0;

			sin = (struct sockaddr_in *) (addr + 1);
			sin->sin_family = AF_INET;
			sin->sin_addr.s_addr = x->sel.saddr.a4;
			sin->sin_port = x->sel.sport;
			memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
		}
	}
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	else if (x->props.family == AF_INET6) {
		addr->sadb_address_prefixlen = 128;

		sin6 = (struct sockaddr_in6 *) (addr + 1);
		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = 0;
		sin6->sin6_flowinfo = 0;
		memcpy(&sin6->sin6_addr, x->id.daddr.a6, sizeof(struct in6_addr));
		sin6->sin6_scope_id = 0;

		if (memcmp (x->sel.saddr.a6, x->props.saddr.a6,
			    sizeof(struct in6_addr))) {
			addr = (struct sadb_address *) skb_put(skb, 
				sizeof(struct sadb_address)+sockaddr_size);
			addr->sadb_address_len = 
				(sizeof(struct sadb_address)+sockaddr_size)/
				sizeof(uint64_t);
			addr->sadb_address_exttype = SADB_EXT_ADDRESS_PROXY;
			addr->sadb_address_proto =
				pfkey_proto_from_xfrm(x->sel.proto);
			addr->sadb_address_prefixlen = x->sel.prefixlen_s;
			addr->sadb_address_reserved = 0;

			sin6 = (struct sockaddr_in6 *) (addr + 1);
			sin6->sin6_family = AF_INET6;
			sin6->sin6_port = x->sel.sport;
			sin6->sin6_flowinfo = 0;
			memcpy(&sin6->sin6_addr, x->sel.saddr.a6,
			       sizeof(struct in6_addr));
			sin6->sin6_scope_id = 0;
		}
	}
#endif
	else
		BUG();

	/* auth key */
	if (add_keys && auth_key_size) {
		key = (struct sadb_key *) skb_put(skb, 
						  sizeof(struct sadb_key)+auth_key_size);
		key->sadb_key_len = (sizeof(struct sadb_key) + auth_key_size) /
			sizeof(uint64_t);
		key->sadb_key_exttype = SADB_EXT_KEY_AUTH;
		key->sadb_key_bits = x->aalg->alg_key_len;
		key->sadb_key_reserved = 0;
		memcpy(key + 1, x->aalg->alg_key, (x->aalg->alg_key_len+7)/8);
	}
	/* encrypt key */
	if (add_keys && encrypt_key_size) {
		key = (struct sadb_key *) skb_put(skb, 
						  sizeof(struct sadb_key)+encrypt_key_size);
		key->sadb_key_len = (sizeof(struct sadb_key) + 
				     encrypt_key_size) / sizeof(uint64_t);
		key->sadb_key_exttype = SADB_EXT_KEY_ENCRYPT;
		key->sadb_key_bits = x->ealg->alg_key_len;
		key->sadb_key_reserved = 0;
		memcpy(key + 1, x->ealg->alg_key, 
		       (x->ealg->alg_key_len+7)/8);
	}

	/* sa */
	sa2 = (struct sadb_x_sa2 *)  skb_put(skb, sizeof(struct sadb_x_sa2));
	sa2->sadb_x_sa2_len = sizeof(struct sadb_x_sa2)/sizeof(uint64_t);
	sa2->sadb_x_sa2_exttype = SADB_X_EXT_SA2;
	sa2->sadb_x_sa2_mode = x->props.mode + 1;
	sa2->sadb_x_sa2_reserved1 = 0;
	sa2->sadb_x_sa2_reserved2 = 0;
	sa2->sadb_x_sa2_sequence = 0;
	sa2->sadb_x_sa2_reqid = x->props.reqid;

	if (natt && natt->encap_type) {
		struct sadb_x_nat_t_type *n_type;
		struct sadb_x_nat_t_port *n_port;

		/* type */
		n_type = (struct sadb_x_nat_t_type*) skb_put(skb, sizeof(*n_type));
		n_type->sadb_x_nat_t_type_len = sizeof(*n_type)/sizeof(uint64_t);
		n_type->sadb_x_nat_t_type_exttype = SADB_X_EXT_NAT_T_TYPE;
		n_type->sadb_x_nat_t_type_type = natt->encap_type;
		n_type->sadb_x_nat_t_type_reserved[0] = 0;
		n_type->sadb_x_nat_t_type_reserved[1] = 0;
		n_type->sadb_x_nat_t_type_reserved[2] = 0;

		/* source port */
		n_port = (struct sadb_x_nat_t_port*) skb_put(skb, sizeof (*n_port));
		n_port->sadb_x_nat_t_port_len = sizeof(*n_port)/sizeof(uint64_t);
		n_port->sadb_x_nat_t_port_exttype = SADB_X_EXT_NAT_T_SPORT;
		n_port->sadb_x_nat_t_port_port = natt->encap_sport;
		n_port->sadb_x_nat_t_port_reserved = 0;

		/* dest port */
		n_port = (struct sadb_x_nat_t_port*) skb_put(skb, sizeof (*n_port));
		n_port->sadb_x_nat_t_port_len = sizeof(*n_port)/sizeof(uint64_t);
		n_port->sadb_x_nat_t_port_exttype = SADB_X_EXT_NAT_T_DPORT;
		n_port->sadb_x_nat_t_port_port = natt->encap_dport;
		n_port->sadb_x_nat_t_port_reserved = 0;
	}

	return skb;
}

static struct xfrm_state * pfkey_msg2xfrm_state(struct sadb_msg *hdr, 
						void **ext_hdrs)
{
	struct xfrm_state *x; 
	struct sadb_lifetime *lifetime;
	struct sadb_sa *sa;
	struct sadb_key *key;
	uint16_t proto;
	int err;
	

	sa = (struct sadb_sa *) ext_hdrs[SADB_EXT_SA-1];
	if (!sa ||
	    !present_and_same_family(ext_hdrs[SADB_EXT_ADDRESS_SRC-1],
				     ext_hdrs[SADB_EXT_ADDRESS_DST-1]))
		return ERR_PTR(-EINVAL);
	if (hdr->sadb_msg_satype == SADB_SATYPE_ESP &&
	    !ext_hdrs[SADB_EXT_KEY_ENCRYPT-1])
		return ERR_PTR(-EINVAL);
	if (hdr->sadb_msg_satype == SADB_SATYPE_AH &&
	    !ext_hdrs[SADB_EXT_KEY_AUTH-1])
		return ERR_PTR(-EINVAL);
	if (!!ext_hdrs[SADB_EXT_LIFETIME_HARD-1] !=
	    !!ext_hdrs[SADB_EXT_LIFETIME_SOFT-1])
		return ERR_PTR(-EINVAL);

	proto = pfkey_satype2proto(hdr->sadb_msg_satype);
	if (proto == 0)
		return ERR_PTR(-EINVAL);

	/* default error is no buffer space */
	err = -ENOBUFS;

	/* RFC2367:

   Only SADB_SASTATE_MATURE SAs may be submitted in an SADB_ADD message.
   SADB_SASTATE_LARVAL SAs are created by SADB_GETSPI and it is not
   sensible to add a new SA in the DYING or SADB_SASTATE_DEAD state.
   Therefore, the sadb_sa_state field of all submitted SAs MUST be
   SADB_SASTATE_MATURE and the kernel MUST return an error if this is
   not true.

           However, KAME setkey always uses SADB_SASTATE_LARVAL.
	   Hence, we have to _ignore_ sadb_sa_state, which is also reasonable.
	 */
	if (sa->sadb_sa_auth > SADB_AALG_MAX ||
	    (hdr->sadb_msg_satype == SADB_X_SATYPE_IPCOMP &&
	     sa->sadb_sa_encrypt > SADB_X_CALG_MAX) ||
	    sa->sadb_sa_encrypt > SADB_EALG_MAX)
		return ERR_PTR(-EINVAL);
	key = (struct sadb_key*) ext_hdrs[SADB_EXT_KEY_AUTH-1];
	if (key != NULL &&
	    sa->sadb_sa_auth != SADB_X_AALG_NULL &&
	    ((key->sadb_key_bits+7) / 8 == 0 ||
	     (key->sadb_key_bits+7) / 8 > key->sadb_key_len * sizeof(uint64_t)))
		return ERR_PTR(-EINVAL);
	key = ext_hdrs[SADB_EXT_KEY_ENCRYPT-1];
	if (key != NULL &&
	    sa->sadb_sa_encrypt != SADB_EALG_NULL &&
	    ((key->sadb_key_bits+7) / 8 == 0 ||
	     (key->sadb_key_bits+7) / 8 > key->sadb_key_len * sizeof(uint64_t)))
		return ERR_PTR(-EINVAL);

	x = xfrm_state_alloc();
	if (x == NULL)
		return ERR_PTR(-ENOBUFS);

	x->id.proto = proto;
	x->id.spi = sa->sadb_sa_spi;
	x->props.replay_window = sa->sadb_sa_replay;
	if (sa->sadb_sa_flags & SADB_SAFLAGS_NOECN)
		x->props.flags |= XFRM_STATE_NOECN;

	lifetime = (struct sadb_lifetime*) ext_hdrs[SADB_EXT_LIFETIME_HARD-1];
	if (lifetime != NULL) {
		x->lft.hard_packet_limit = _KEY2X(lifetime->sadb_lifetime_allocations);
		x->lft.hard_byte_limit = _KEY2X(lifetime->sadb_lifetime_bytes);
		x->lft.hard_add_expires_seconds = lifetime->sadb_lifetime_addtime;
		x->lft.hard_use_expires_seconds = lifetime->sadb_lifetime_usetime;
	}
	lifetime = (struct sadb_lifetime*) ext_hdrs[SADB_EXT_LIFETIME_SOFT-1];
	if (lifetime != NULL) {
		x->lft.soft_packet_limit = _KEY2X(lifetime->sadb_lifetime_allocations);
		x->lft.soft_byte_limit = _KEY2X(lifetime->sadb_lifetime_bytes);
		x->lft.soft_add_expires_seconds = lifetime->sadb_lifetime_addtime;
		x->lft.soft_use_expires_seconds = lifetime->sadb_lifetime_usetime;
	}
	key = (struct sadb_key*) ext_hdrs[SADB_EXT_KEY_AUTH-1];
	if (sa->sadb_sa_auth) {
		int keysize = 0;
		struct xfrm_algo_desc *a = xfrm_aalg_get_byid(sa->sadb_sa_auth);
		if (!a) {
			err = -ENOSYS;
			goto out;
		}
		if (key)
			keysize = (key->sadb_key_bits + 7) / 8;
		x->aalg = kmalloc(sizeof(*x->aalg) + keysize, GFP_KERNEL);
		if (!x->aalg)
			goto out;
		strcpy(x->aalg->alg_name, a->name);
		x->aalg->alg_key_len = 0;
		if (key) {
			x->aalg->alg_key_len = key->sadb_key_bits;
			memcpy(x->aalg->alg_key, key+1, keysize);
		}
		x->props.aalgo = sa->sadb_sa_auth;
		/* x->algo.flags = sa->sadb_sa_flags; */
	}
	if (sa->sadb_sa_encrypt) {
		if (hdr->sadb_msg_satype == SADB_X_SATYPE_IPCOMP) {
			struct xfrm_algo_desc *a = xfrm_calg_get_byid(sa->sadb_sa_encrypt);
			if (!a) {
				err = -ENOSYS;
				goto out;
			}
			x->calg = kmalloc(sizeof(*x->calg), GFP_KERNEL);
			if (!x->calg)
				goto out;
			strcpy(x->calg->alg_name, a->name);
			x->props.calgo = sa->sadb_sa_encrypt;
		} else {
			int keysize = 0;
			struct xfrm_algo_desc *a = xfrm_ealg_get_byid(sa->sadb_sa_encrypt);
			if (!a) {
				err = -ENOSYS;
				goto out;
			}
			key = (struct sadb_key*) ext_hdrs[SADB_EXT_KEY_ENCRYPT-1];
			if (key)
				keysize = (key->sadb_key_bits + 7) / 8;
			x->ealg = kmalloc(sizeof(*x->ealg) + keysize, GFP_KERNEL);
			if (!x->ealg)
				goto out;
			strcpy(x->ealg->alg_name, a->name);
			x->ealg->alg_key_len = 0;
			if (key) {
				x->ealg->alg_key_len = key->sadb_key_bits;
				memcpy(x->ealg->alg_key, key+1, keysize);
			}
			x->props.ealgo = sa->sadb_sa_encrypt;
		}
	}
	/* x->algo.flags = sa->sadb_sa_flags; */

	x->props.family = pfkey_sadb_addr2xfrm_addr((struct sadb_address *) ext_hdrs[SADB_EXT_ADDRESS_SRC-1], 
						    &x->props.saddr);
	if (!x->props.family) {
		err = -EAFNOSUPPORT;
		goto out;
	}
	pfkey_sadb_addr2xfrm_addr((struct sadb_address *) ext_hdrs[SADB_EXT_ADDRESS_DST-1], 
				  &x->id.daddr);

	if (ext_hdrs[SADB_X_EXT_SA2-1]) {
		struct sadb_x_sa2 *sa2 = (void*)ext_hdrs[SADB_X_EXT_SA2-1];
		x->props.mode = sa2->sadb_x_sa2_mode;
		if (x->props.mode)
			x->props.mode--;
		x->props.reqid = sa2->sadb_x_sa2_reqid;
	}

	if (ext_hdrs[SADB_EXT_ADDRESS_PROXY-1]) {
		struct sadb_address *addr = ext_hdrs[SADB_EXT_ADDRESS_PROXY-1];

		/* Nobody uses this, but we try. */
		x->sel.family = pfkey_sadb_addr2xfrm_addr(addr, &x->sel.saddr);
		x->sel.prefixlen_s = addr->sadb_address_prefixlen;
	}

	if (ext_hdrs[SADB_X_EXT_NAT_T_TYPE-1]) {
		struct sadb_x_nat_t_type* n_type;
		struct xfrm_encap_tmpl *natt;

		x->encap = kmalloc(sizeof(*x->encap), GFP_KERNEL);
		if (!x->encap)
			goto out;

		natt = x->encap;
		n_type = ext_hdrs[SADB_X_EXT_NAT_T_TYPE-1];
		natt->encap_type = n_type->sadb_x_nat_t_type_type;

		switch (natt->encap_type) {
		case UDP_ENCAP_ESPINUDP:
		case UDP_ENCAP_ESPINUDP_NON_IKE:
			break;
		default:
			err = -ENOPROTOOPT;
			goto out;
		}

		if (ext_hdrs[SADB_X_EXT_NAT_T_SPORT-1]) {
			struct sadb_x_nat_t_port* n_port =
				ext_hdrs[SADB_X_EXT_NAT_T_SPORT-1];
			natt->encap_sport = n_port->sadb_x_nat_t_port_port;
		}
		if (ext_hdrs[SADB_X_EXT_NAT_T_DPORT-1]) {
			struct sadb_x_nat_t_port* n_port =
				ext_hdrs[SADB_X_EXT_NAT_T_DPORT-1];
			natt->encap_dport = n_port->sadb_x_nat_t_port_port;
		}
	}

	x->type = xfrm_get_type(proto, x->props.family);
	if (x->type == NULL) {
		err = -ENOPROTOOPT;
		goto out;
	}
	if (x->type->init_state(x, NULL)) {
		err = -EINVAL;
		goto out;
	}
	x->km.seq = hdr->sadb_msg_seq;
	x->km.state = XFRM_STATE_VALID;
	return x;

out:
	x->km.state = XFRM_STATE_DEAD;
	xfrm_state_put(x);
	return ERR_PTR(err);
}

static int pfkey_reserved(struct sock *sk, struct sk_buff *skb, struct sadb_msg *hdr, void **ext_hdrs)
{
	return -EOPNOTSUPP;
}

static int pfkey_getspi(struct sock *sk, struct sk_buff *skb, struct sadb_msg *hdr, void **ext_hdrs)
{
	struct sk_buff *resp_skb;
	struct sadb_x_sa2 *sa2;
	struct sadb_address *saddr, *daddr;
	struct sadb_msg *out_hdr;
	struct xfrm_state *x = NULL;
	u8 mode;
	u32 reqid;
	u8 proto;
	unsigned short family;
	xfrm_address_t *xsaddr = NULL, *xdaddr = NULL;

	if (!present_and_same_family(ext_hdrs[SADB_EXT_ADDRESS_SRC-1],
				     ext_hdrs[SADB_EXT_ADDRESS_DST-1]))
		return -EINVAL;

	proto = pfkey_satype2proto(hdr->sadb_msg_satype);
	if (proto == 0)
		return -EINVAL;

	if ((sa2 = ext_hdrs[SADB_X_EXT_SA2-1]) != NULL) {
		mode = sa2->sadb_x_sa2_mode - 1;
		reqid = sa2->sadb_x_sa2_reqid;
	} else {
		mode = 0;
		reqid = 0;
	}

	saddr = ext_hdrs[SADB_EXT_ADDRESS_SRC-1];
	daddr = ext_hdrs[SADB_EXT_ADDRESS_DST-1];

	family = ((struct sockaddr *)(saddr + 1))->sa_family;
	switch (family) {
	case AF_INET:
		xdaddr = (xfrm_address_t *)&((struct sockaddr_in *)(daddr + 1))->sin_addr.s_addr;
		xsaddr = (xfrm_address_t *)&((struct sockaddr_in *)(saddr + 1))->sin_addr.s_addr;
		break;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	case AF_INET6:
		xdaddr = (xfrm_address_t *)&((struct sockaddr_in6 *)(daddr + 1))->sin6_addr;
		xsaddr = (xfrm_address_t *)&((struct sockaddr_in6 *)(saddr + 1))->sin6_addr;
		break;
#endif
	}
	if (xdaddr)
		x = xfrm_find_acq(mode, reqid, proto, xdaddr, xsaddr, 1, family);

	if (x == NULL)
		return -ENOENT;

	resp_skb = ERR_PTR(-ENOENT);

	spin_lock_bh(&x->lock);
	if (x->km.state != XFRM_STATE_DEAD) {
		struct sadb_spirange *range = ext_hdrs[SADB_EXT_SPIRANGE-1];
		u32 min_spi, max_spi;

		if (range != NULL) {
			min_spi = range->sadb_spirange_min;
			max_spi = range->sadb_spirange_max;
		} else {
			min_spi = 0x100;
			max_spi = 0x0fffffff;
		}
		xfrm_alloc_spi(x, htonl(min_spi), htonl(max_spi));
		if (x->id.spi)
			resp_skb = pfkey_xfrm_state2msg(x, 0, 3);
	}
	spin_unlock_bh(&x->lock);

	if (IS_ERR(resp_skb)) {
		xfrm_state_put(x);
		return  PTR_ERR(resp_skb);
	}

	out_hdr = (struct sadb_msg *) resp_skb->data;
	out_hdr->sadb_msg_version = hdr->sadb_msg_version;
	out_hdr->sadb_msg_type = SADB_GETSPI;
	out_hdr->sadb_msg_satype = pfkey_proto2satype(proto);
	out_hdr->sadb_msg_errno = 0;
	out_hdr->sadb_msg_reserved = 0;
	out_hdr->sadb_msg_seq = hdr->sadb_msg_seq;
	out_hdr->sadb_msg_pid = hdr->sadb_msg_pid;

	xfrm_state_put(x);

	pfkey_broadcast(resp_skb, GFP_KERNEL, BROADCAST_ONE, sk);

	return 0;
}

static int pfkey_acquire(struct sock *sk, struct sk_buff *skb, struct sadb_msg *hdr, void **ext_hdrs)
{
	struct xfrm_state *x;

	if (hdr->sadb_msg_len != sizeof(struct sadb_msg)/8)
		return -EOPNOTSUPP;

	if (hdr->sadb_msg_seq == 0 || hdr->sadb_msg_errno == 0)
		return 0;

	x = xfrm_find_acq_byseq(hdr->sadb_msg_seq);
	if (x == NULL)
		return 0;

	spin_lock_bh(&x->lock);
	if (x->km.state == XFRM_STATE_ACQ) {
		x->km.state = XFRM_STATE_ERROR;
		wake_up(&km_waitq);
	}
	spin_unlock_bh(&x->lock);
	xfrm_state_put(x);
	return 0;
}


static int pfkey_add(struct sock *sk, struct sk_buff *skb, struct sadb_msg *hdr, void **ext_hdrs)
{
	struct sk_buff *out_skb;
	struct sadb_msg *out_hdr;
	struct xfrm_state *x;
	int err;

	xfrm_probe_algs();
	
	x = pfkey_msg2xfrm_state(hdr, ext_hdrs);
	if (IS_ERR(x))
		return PTR_ERR(x);

	if (hdr->sadb_msg_type == SADB_ADD)
		err = xfrm_state_add(x);
	else
		err = xfrm_state_update(x);

	if (err < 0) {
		x->km.state = XFRM_STATE_DEAD;
		xfrm_state_put(x);
		return err;
	}

	out_skb = pfkey_xfrm_state2msg(x, 0, 3);
	if (IS_ERR(out_skb))
		return  PTR_ERR(out_skb); /* XXX Should we return 0 here ? */

	out_hdr = (struct sadb_msg *) out_skb->data;
	out_hdr->sadb_msg_version = hdr->sadb_msg_version;
	out_hdr->sadb_msg_type = hdr->sadb_msg_type;
	out_hdr->sadb_msg_satype = pfkey_proto2satype(x->id.proto);
	out_hdr->sadb_msg_errno = 0;
	out_hdr->sadb_msg_reserved = 0;
	out_hdr->sadb_msg_seq = hdr->sadb_msg_seq;
	out_hdr->sadb_msg_pid = hdr->sadb_msg_pid;

	pfkey_broadcast(out_skb, GFP_ATOMIC, BROADCAST_ALL, sk);

	return 0;
}

static int pfkey_delete(struct sock *sk, struct sk_buff *skb, struct sadb_msg *hdr, void **ext_hdrs)
{
	struct xfrm_state *x;

	if (!ext_hdrs[SADB_EXT_SA-1] ||
	    !present_and_same_family(ext_hdrs[SADB_EXT_ADDRESS_SRC-1],
				     ext_hdrs[SADB_EXT_ADDRESS_DST-1]))
		return -EINVAL;

	x = pfkey_xfrm_state_lookup(hdr, ext_hdrs);
	if (x == NULL)
		return -ESRCH;

	if (xfrm_state_kern(x)) {
		xfrm_state_put(x);
		return -EPERM;
	}
	
	xfrm_state_delete(x);
	xfrm_state_put(x);

	pfkey_broadcast(skb_clone(skb, GFP_KERNEL), GFP_KERNEL, 
			BROADCAST_ALL, sk);

	return 0;
}

static int pfkey_get(struct sock *sk, struct sk_buff *skb, struct sadb_msg *hdr, void **ext_hdrs)
{
	__u8 proto;
	struct sk_buff *out_skb;
	struct sadb_msg *out_hdr;
	struct xfrm_state *x;

	if (!ext_hdrs[SADB_EXT_SA-1] ||
	    !present_and_same_family(ext_hdrs[SADB_EXT_ADDRESS_SRC-1],
				     ext_hdrs[SADB_EXT_ADDRESS_DST-1]))
		return -EINVAL;

	x = pfkey_xfrm_state_lookup(hdr, ext_hdrs);
	if (x == NULL)
		return -ESRCH;

	out_skb = pfkey_xfrm_state2msg(x, 1, 3);
	proto = x->id.proto;
	xfrm_state_put(x);
	if (IS_ERR(out_skb))
		return  PTR_ERR(out_skb);

	out_hdr = (struct sadb_msg *) out_skb->data;
	out_hdr->sadb_msg_version = hdr->sadb_msg_version;
	out_hdr->sadb_msg_type = SADB_DUMP;
	out_hdr->sadb_msg_satype = pfkey_proto2satype(proto);
	out_hdr->sadb_msg_errno = 0;
	out_hdr->sadb_msg_reserved = 0;
	out_hdr->sadb_msg_seq = hdr->sadb_msg_seq;
	out_hdr->sadb_msg_pid = hdr->sadb_msg_pid;
	pfkey_broadcast(out_skb, GFP_ATOMIC, BROADCAST_ONE, sk);

	return 0;
}

static struct sk_buff *compose_sadb_supported(struct sadb_msg *orig, int allocation)
{
	struct sk_buff *skb;
	struct sadb_msg *hdr;
	int len, auth_len, enc_len, i;

	auth_len = xfrm_count_auth_supported();
	if (auth_len) {
		auth_len *= sizeof(struct sadb_alg);
		auth_len += sizeof(struct sadb_supported);
	}
	
	enc_len = xfrm_count_enc_supported();
	if (enc_len) {
		enc_len *= sizeof(struct sadb_alg);
		enc_len += sizeof(struct sadb_supported);
	}
	
	len = enc_len + auth_len + sizeof(struct sadb_msg);

	skb = alloc_skb(len + 16, allocation);
	if (!skb)
		goto out_put_algs;

	hdr = (struct sadb_msg *) skb_put(skb, sizeof(*hdr));
	pfkey_hdr_dup(hdr, orig);
	hdr->sadb_msg_errno = 0;
	hdr->sadb_msg_len = len / sizeof(uint64_t);

	if (auth_len) {
		struct sadb_supported *sp;
		struct sadb_alg *ap;

		sp = (struct sadb_supported *) skb_put(skb, auth_len);
		ap = (struct sadb_alg *) (sp + 1);

		sp->sadb_supported_len = auth_len / sizeof(uint64_t);
		sp->sadb_supported_exttype = SADB_EXT_SUPPORTED_AUTH;

		for (i = 0; ; i++) {
			struct xfrm_algo_desc *aalg = xfrm_aalg_get_byidx(i);
			if (!aalg)
				break;
			if (aalg->available)
				*ap++ = aalg->desc;
		}
	}

	if (enc_len) {
		struct sadb_supported *sp;
		struct sadb_alg *ap;

		sp = (struct sadb_supported *) skb_put(skb, enc_len);
		ap = (struct sadb_alg *) (sp + 1);

		sp->sadb_supported_len = enc_len / sizeof(uint64_t);
		sp->sadb_supported_exttype = SADB_EXT_SUPPORTED_ENCRYPT;

		for (i = 0; ; i++) {
			struct xfrm_algo_desc *ealg = xfrm_ealg_get_byidx(i);
			if (!ealg)
				break;
			if (ealg->available)
				*ap++ = ealg->desc;
		}
	}

out_put_algs:
	return skb;
}

static int pfkey_register(struct sock *sk, struct sk_buff *skb, struct sadb_msg *hdr, void **ext_hdrs)
{
	struct pfkey_opt *pfk = pfkey_sk(sk);
	struct sk_buff *supp_skb;

	if (hdr->sadb_msg_satype > SADB_SATYPE_MAX)
		return -EINVAL;

	if (hdr->sadb_msg_satype != SADB_SATYPE_UNSPEC) {
		if (pfk->registered&(1<<hdr->sadb_msg_satype))
			return -EEXIST;
		pfk->registered |= (1<<hdr->sadb_msg_satype);
	}

	xfrm_probe_algs();
	
	supp_skb = compose_sadb_supported(hdr, GFP_KERNEL);
	if (!supp_skb) {
		if (hdr->sadb_msg_satype != SADB_SATYPE_UNSPEC)
			pfk->registered &= ~(1<<hdr->sadb_msg_satype);

		return -ENOBUFS;
	}

	pfkey_broadcast(supp_skb, GFP_KERNEL, BROADCAST_REGISTERED, sk);

	return 0;
}

static int pfkey_flush(struct sock *sk, struct sk_buff *skb, struct sadb_msg *hdr, void **ext_hdrs)
{
	unsigned proto;
	struct sk_buff *skb_out;
	struct sadb_msg *hdr_out;

	proto = pfkey_satype2proto(hdr->sadb_msg_satype);
	if (proto == 0)
		return -EINVAL;

	skb_out = alloc_skb(sizeof(struct sadb_msg) + 16, GFP_KERNEL);
	if (!skb_out)
		return -ENOBUFS;

	xfrm_state_flush(proto);

	hdr_out = (struct sadb_msg *) skb_put(skb_out, sizeof(struct sadb_msg));
	pfkey_hdr_dup(hdr_out, hdr);
	hdr_out->sadb_msg_errno = (uint8_t) 0;
	hdr_out->sadb_msg_len = (sizeof(struct sadb_msg) / sizeof(uint64_t));

	pfkey_broadcast(skb_out, GFP_KERNEL, BROADCAST_ALL, NULL);

	return 0;
}

struct pfkey_dump_data
{
	struct sk_buff *skb;
	struct sadb_msg *hdr;
	struct sock *sk;
};

static int dump_sa(struct xfrm_state *x, int count, void *ptr)
{
	struct pfkey_dump_data *data = ptr;
	struct sk_buff *out_skb;
	struct sadb_msg *out_hdr;

	out_skb = pfkey_xfrm_state2msg(x, 1, 3);
	if (IS_ERR(out_skb))
		return PTR_ERR(out_skb);

	out_hdr = (struct sadb_msg *) out_skb->data;
	out_hdr->sadb_msg_version = data->hdr->sadb_msg_version;
	out_hdr->sadb_msg_type = SADB_DUMP;
	out_hdr->sadb_msg_satype = pfkey_proto2satype(x->id.proto);
	out_hdr->sadb_msg_errno = 0;
	out_hdr->sadb_msg_reserved = 0;
	out_hdr->sadb_msg_seq = count;
	out_hdr->sadb_msg_pid = data->hdr->sadb_msg_pid;
	pfkey_broadcast(out_skb, GFP_ATOMIC, BROADCAST_ONE, data->sk);
	return 0;
}

static int pfkey_dump(struct sock *sk, struct sk_buff *skb, struct sadb_msg *hdr, void **ext_hdrs)
{
	u8 proto;
	struct pfkey_dump_data data = { .skb = skb, .hdr = hdr, .sk = sk };

	proto = pfkey_satype2proto(hdr->sadb_msg_satype);
	if (proto == 0)
		return -EINVAL;

	return xfrm_state_walk(proto, dump_sa, &data);
}

static int pfkey_promisc(struct sock *sk, struct sk_buff *skb, struct sadb_msg *hdr, void **ext_hdrs)
{
	struct pfkey_opt *pfk = pfkey_sk(sk);
	int satype = hdr->sadb_msg_satype;

	if (hdr->sadb_msg_len == (sizeof(*hdr) / sizeof(uint64_t))) {
		/* XXX we mangle packet... */
		hdr->sadb_msg_errno = 0;
		if (satype != 0 && satype != 1)
			return -EINVAL;
		pfk->promisc = satype;
	}
	pfkey_broadcast(skb_clone(skb, GFP_KERNEL), GFP_KERNEL, BROADCAST_ALL, NULL);
	return 0;
}

static int check_reqid(struct xfrm_policy *xp, int dir, int count, void *ptr)
{
	int i;
	u32 reqid = *(u32*)ptr;

	for (i=0; i<xp->xfrm_nr; i++) {
		if (xp->xfrm_vec[i].reqid == reqid)
			return -EEXIST;
	}
	return 0;
}

static u32 gen_reqid(void)
{
	u32 start;
	static u32 reqid = IPSEC_MANUAL_REQID_MAX;

	start = reqid;
	do {
		++reqid;
		if (reqid == 0)
			reqid = IPSEC_MANUAL_REQID_MAX+1;
		if (xfrm_policy_walk(check_reqid, (void*)&reqid) != -EEXIST)
			return reqid;
	} while (reqid != start);
	return 0;
}

static int
parse_ipsecrequest(struct xfrm_policy *xp, struct sadb_x_ipsecrequest *rq)
{
	struct xfrm_tmpl *t = xp->xfrm_vec + xp->xfrm_nr;
	struct sockaddr_in *sin;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	struct sockaddr_in6 *sin6;
#endif

	if (xp->xfrm_nr >= XFRM_MAX_DEPTH)
		return -ELOOP;

	if (rq->sadb_x_ipsecrequest_mode == 0)
		return -EINVAL;

	t->id.proto = rq->sadb_x_ipsecrequest_proto; /* XXX check proto */
	t->mode = rq->sadb_x_ipsecrequest_mode-1;
	if (rq->sadb_x_ipsecrequest_level == IPSEC_LEVEL_USE)
		t->optional = 1;
	else if (rq->sadb_x_ipsecrequest_level == IPSEC_LEVEL_UNIQUE) {
		t->reqid = rq->sadb_x_ipsecrequest_reqid;
		if (t->reqid > IPSEC_MANUAL_REQID_MAX)
			t->reqid = 0;
		if (!t->reqid && !(t->reqid = gen_reqid()))
			return -ENOBUFS;
	}

	/* addresses present only in tunnel mode */
	if (t->mode) {
		switch (xp->family) {
		case AF_INET:
			sin = (void*)(rq+1);
			if (sin->sin_family != AF_INET)
				return -EINVAL;
			t->saddr.a4 = sin->sin_addr.s_addr;
			sin++;
			if (sin->sin_family != AF_INET)
				return -EINVAL;
			t->id.daddr.a4 = sin->sin_addr.s_addr;
			break;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
		case AF_INET6:
			sin6 = (void *)(rq+1);
			if (sin6->sin6_family != AF_INET6)
				return -EINVAL;
			memcpy(t->saddr.a6, &sin6->sin6_addr, sizeof(struct in6_addr));
			sin6++;
			if (sin6->sin6_family != AF_INET6)
				return -EINVAL;
			memcpy(t->id.daddr.a6, &sin6->sin6_addr, sizeof(struct in6_addr));
			break;
#endif
		default:
			return -EINVAL;
		}
	}
	/* No way to set this via kame pfkey */
	t->aalgos = t->ealgos = t->calgos = ~0;
	xp->xfrm_nr++;
	return 0;
}

static int
parse_ipsecrequests(struct xfrm_policy *xp, struct sadb_x_policy *pol)
{
	int err;
	int len = pol->sadb_x_policy_len*8 - sizeof(struct sadb_x_policy);
	struct sadb_x_ipsecrequest *rq = (void*)(pol+1);

	while (len >= sizeof(struct sadb_x_ipsecrequest)) {
		if ((err = parse_ipsecrequest(xp, rq)) < 0)
			return err;
		len -= rq->sadb_x_ipsecrequest_len;
		rq = (void*)((u8*)rq + rq->sadb_x_ipsecrequest_len);
	}
	return 0;
}

static int pfkey_xfrm_policy2msg_size(struct xfrm_policy *xp)
{
	int sockaddr_size = pfkey_sockaddr_size(xp->family);
	int socklen = (xp->family == AF_INET ?
		       sizeof(struct sockaddr_in) :
		       sizeof(struct sockaddr_in6));

	return sizeof(struct sadb_msg) +
		(sizeof(struct sadb_lifetime) * 3) +
		(sizeof(struct sadb_address) * 2) + 
		(sockaddr_size * 2) +
		sizeof(struct sadb_x_policy) +
		(xp->xfrm_nr * (sizeof(struct sadb_x_ipsecrequest) +
				(socklen * 2)));
}

static struct sk_buff * pfkey_xfrm_policy2msg_prep(struct xfrm_policy *xp)
{
	struct sk_buff *skb;
	int size;

	size = pfkey_xfrm_policy2msg_size(xp);

	skb =  alloc_skb(size + 16, GFP_ATOMIC);
	if (skb == NULL)
		return ERR_PTR(-ENOBUFS);

	return skb;
}

static void pfkey_xfrm_policy2msg(struct sk_buff *skb, struct xfrm_policy *xp, int dir)
{
	struct sadb_msg *hdr;
	struct sadb_address *addr;
	struct sadb_lifetime *lifetime;
	struct sadb_x_policy *pol;
	struct sockaddr_in   *sin;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	struct sockaddr_in6  *sin6;
#endif
	int i;
	int size;
	int sockaddr_size = pfkey_sockaddr_size(xp->family);
	int socklen = (xp->family == AF_INET ?
		       sizeof(struct sockaddr_in) :
		       sizeof(struct sockaddr_in6));

	size = pfkey_xfrm_policy2msg_size(xp);

	/* call should fill header later */
	hdr = (struct sadb_msg *) skb_put(skb, sizeof(struct sadb_msg));
	memset(hdr, 0, size);	/* XXX do we need this ? */

	/* src address */
	addr = (struct sadb_address*) skb_put(skb, 
					      sizeof(struct sadb_address)+sockaddr_size);
	addr->sadb_address_len = 
		(sizeof(struct sadb_address)+sockaddr_size)/
			sizeof(uint64_t);
	addr->sadb_address_exttype = SADB_EXT_ADDRESS_SRC;
	addr->sadb_address_proto = pfkey_proto_from_xfrm(xp->selector.proto);
	addr->sadb_address_prefixlen = xp->selector.prefixlen_s;
	addr->sadb_address_reserved = 0;
	/* src address */
	if (xp->family == AF_INET) {
		sin = (struct sockaddr_in *) (addr + 1);
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = xp->selector.saddr.a4;
		sin->sin_port = xp->selector.sport;
		memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
	}
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	else if (xp->family == AF_INET6) {
		sin6 = (struct sockaddr_in6 *) (addr + 1);
		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = xp->selector.sport;
		sin6->sin6_flowinfo = 0;
		memcpy(&sin6->sin6_addr, xp->selector.saddr.a6,
		       sizeof(struct in6_addr));
		sin6->sin6_scope_id = 0;
	}
#endif
	else
		BUG();

	/* dst address */
	addr = (struct sadb_address*) skb_put(skb, 
					      sizeof(struct sadb_address)+sockaddr_size);
	addr->sadb_address_len =
		(sizeof(struct sadb_address)+sockaddr_size)/
			sizeof(uint64_t);
	addr->sadb_address_exttype = SADB_EXT_ADDRESS_DST;
	addr->sadb_address_proto = pfkey_proto_from_xfrm(xp->selector.proto);
	addr->sadb_address_prefixlen = xp->selector.prefixlen_d; 
	addr->sadb_address_reserved = 0;
	if (xp->family == AF_INET) {
		sin = (struct sockaddr_in *) (addr + 1);
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = xp->selector.daddr.a4;
		sin->sin_port = xp->selector.dport;
		memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
	}
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	else if (xp->family == AF_INET6) {
		sin6 = (struct sockaddr_in6 *) (addr + 1);
		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = xp->selector.dport;
		sin6->sin6_flowinfo = 0;
		memcpy(&sin6->sin6_addr, xp->selector.daddr.a6,
		       sizeof(struct in6_addr));
		sin6->sin6_scope_id = 0;
	}
#endif
	else
		BUG();

	/* hard time */
	lifetime = (struct sadb_lifetime *)  skb_put(skb, 
						     sizeof(struct sadb_lifetime));
	lifetime->sadb_lifetime_len =
		sizeof(struct sadb_lifetime)/sizeof(uint64_t);
	lifetime->sadb_lifetime_exttype = SADB_EXT_LIFETIME_HARD;
	lifetime->sadb_lifetime_allocations =  _X2KEY(xp->lft.hard_packet_limit);
	lifetime->sadb_lifetime_bytes = _X2KEY(xp->lft.hard_byte_limit);
	lifetime->sadb_lifetime_addtime = xp->lft.hard_add_expires_seconds;
	lifetime->sadb_lifetime_usetime = xp->lft.hard_use_expires_seconds;
	/* soft time */
	lifetime = (struct sadb_lifetime *)  skb_put(skb, 
						     sizeof(struct sadb_lifetime));
	lifetime->sadb_lifetime_len =
		sizeof(struct sadb_lifetime)/sizeof(uint64_t);
	lifetime->sadb_lifetime_exttype = SADB_EXT_LIFETIME_SOFT;
	lifetime->sadb_lifetime_allocations =  _X2KEY(xp->lft.soft_packet_limit);
	lifetime->sadb_lifetime_bytes = _X2KEY(xp->lft.soft_byte_limit);
	lifetime->sadb_lifetime_addtime = xp->lft.soft_add_expires_seconds;
	lifetime->sadb_lifetime_usetime = xp->lft.soft_use_expires_seconds;
	/* current time */
	lifetime = (struct sadb_lifetime *)  skb_put(skb, 
						     sizeof(struct sadb_lifetime));
	lifetime->sadb_lifetime_len =
		sizeof(struct sadb_lifetime)/sizeof(uint64_t);
	lifetime->sadb_lifetime_exttype = SADB_EXT_LIFETIME_CURRENT;
	lifetime->sadb_lifetime_allocations = xp->curlft.packets;
	lifetime->sadb_lifetime_bytes = xp->curlft.bytes;
	lifetime->sadb_lifetime_addtime = xp->curlft.add_time;
	lifetime->sadb_lifetime_usetime = xp->curlft.use_time;

	pol = (struct sadb_x_policy *)  skb_put(skb, sizeof(struct sadb_x_policy));
	pol->sadb_x_policy_len = sizeof(struct sadb_x_policy)/sizeof(uint64_t);
	pol->sadb_x_policy_exttype = SADB_X_EXT_POLICY;
	pol->sadb_x_policy_type = IPSEC_POLICY_DISCARD;
	if (xp->action == XFRM_POLICY_ALLOW) {
		if (xp->xfrm_nr)
			pol->sadb_x_policy_type = IPSEC_POLICY_IPSEC;
		else
			pol->sadb_x_policy_type = IPSEC_POLICY_NONE;
	}
	pol->sadb_x_policy_dir = dir+1;
	pol->sadb_x_policy_id = xp->index;
	pol->sadb_x_policy_priority = xp->priority;

	for (i=0; i<xp->xfrm_nr; i++) {
		struct sadb_x_ipsecrequest *rq;
		struct xfrm_tmpl *t = xp->xfrm_vec + i;
		int req_size;

		req_size = sizeof(struct sadb_x_ipsecrequest);
		if (t->mode)
			req_size += 2*socklen;
		else
			size -= 2*socklen;
		rq = (void*)skb_put(skb, req_size);
		pol->sadb_x_policy_len += req_size/8;
		memset(rq, 0, sizeof(*rq));
		rq->sadb_x_ipsecrequest_len = req_size;
		rq->sadb_x_ipsecrequest_proto = t->id.proto;
		rq->sadb_x_ipsecrequest_mode = t->mode+1;
		rq->sadb_x_ipsecrequest_level = IPSEC_LEVEL_REQUIRE;
		if (t->reqid)
			rq->sadb_x_ipsecrequest_level = IPSEC_LEVEL_UNIQUE;
		if (t->optional)
			rq->sadb_x_ipsecrequest_level = IPSEC_LEVEL_USE;
		rq->sadb_x_ipsecrequest_reqid = t->reqid;
		if (t->mode) {
			switch (xp->family) {
			case AF_INET:
				sin = (void*)(rq+1);
				sin->sin_family = AF_INET;
				sin->sin_addr.s_addr = t->saddr.a4;
				sin->sin_port = 0;
				memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
				sin++;
				sin->sin_family = AF_INET;
				sin->sin_addr.s_addr = t->id.daddr.a4;
				sin->sin_port = 0;
				memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
				break;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
			case AF_INET6:
				sin6 = (void*)(rq+1);
				sin6->sin6_family = AF_INET6;
				sin6->sin6_port = 0;
				sin6->sin6_flowinfo = 0;
				memcpy(&sin6->sin6_addr, t->saddr.a6,
				       sizeof(struct in6_addr));
				sin6->sin6_scope_id = 0;

				sin6++;
				sin6->sin6_family = AF_INET6;
				sin6->sin6_port = 0;
				sin6->sin6_flowinfo = 0;
				memcpy(&sin6->sin6_addr, t->id.daddr.a6,
				       sizeof(struct in6_addr));
				sin6->sin6_scope_id = 0;
				break;
#endif
			default:
				break;
			}
		}
	}
	hdr->sadb_msg_len = size / sizeof(uint64_t);
	hdr->sadb_msg_reserved = atomic_read(&xp->refcnt);
}

static int pfkey_spdadd(struct sock *sk, struct sk_buff *skb, struct sadb_msg *hdr, void **ext_hdrs)
{
	int err;
	struct sadb_lifetime *lifetime;
	struct sadb_address *sa;
	struct sadb_x_policy *pol;
	struct xfrm_policy *xp;
	struct sk_buff *out_skb;
	struct sadb_msg *out_hdr;

	if (!present_and_same_family(ext_hdrs[SADB_EXT_ADDRESS_SRC-1],
				     ext_hdrs[SADB_EXT_ADDRESS_DST-1]) ||
	    !ext_hdrs[SADB_X_EXT_POLICY-1])
		return -EINVAL;

	pol = ext_hdrs[SADB_X_EXT_POLICY-1];
	if (pol->sadb_x_policy_type > IPSEC_POLICY_IPSEC)
		return -EINVAL;
	if (!pol->sadb_x_policy_dir || pol->sadb_x_policy_dir >= IPSEC_DIR_MAX)
		return -EINVAL;

	xp = xfrm_policy_alloc(GFP_KERNEL);
	if (xp == NULL)
		return -ENOBUFS;

	xp->action = (pol->sadb_x_policy_type == IPSEC_POLICY_DISCARD ?
		      XFRM_POLICY_BLOCK : XFRM_POLICY_ALLOW);
	xp->priority = pol->sadb_x_policy_priority;

	sa = ext_hdrs[SADB_EXT_ADDRESS_SRC-1], 
	xp->family = pfkey_sadb_addr2xfrm_addr(sa, &xp->selector.saddr);
	if (!xp->family) {
		err = -EINVAL;
		goto out;
	}
	xp->selector.family = xp->family;
	xp->selector.prefixlen_s = sa->sadb_address_prefixlen;
	xp->selector.proto = pfkey_proto_to_xfrm(sa->sadb_address_proto);
	xp->selector.sport = ((struct sockaddr_in *)(sa+1))->sin_port;
	if (xp->selector.sport)
		xp->selector.sport_mask = ~0;

	sa = ext_hdrs[SADB_EXT_ADDRESS_DST-1], 
	pfkey_sadb_addr2xfrm_addr(sa, &xp->selector.daddr);
	xp->selector.prefixlen_d = sa->sadb_address_prefixlen;

	/* Amusing, we set this twice.  KAME apps appear to set same value
	 * in both addresses.
	 */
	xp->selector.proto = pfkey_proto_to_xfrm(sa->sadb_address_proto);

	xp->selector.dport = ((struct sockaddr_in *)(sa+1))->sin_port;
	if (xp->selector.dport)
		xp->selector.dport_mask = ~0;

	xp->lft.soft_byte_limit = XFRM_INF;
	xp->lft.hard_byte_limit = XFRM_INF;
	xp->lft.soft_packet_limit = XFRM_INF;
	xp->lft.hard_packet_limit = XFRM_INF;
	if ((lifetime = ext_hdrs[SADB_EXT_LIFETIME_HARD-1]) != NULL) {
		xp->lft.hard_packet_limit = _KEY2X(lifetime->sadb_lifetime_allocations);
		xp->lft.hard_byte_limit = _KEY2X(lifetime->sadb_lifetime_bytes);
		xp->lft.hard_add_expires_seconds = lifetime->sadb_lifetime_addtime;
		xp->lft.hard_use_expires_seconds = lifetime->sadb_lifetime_usetime;
	}
	if ((lifetime = ext_hdrs[SADB_EXT_LIFETIME_SOFT-1]) != NULL) {
		xp->lft.soft_packet_limit = _KEY2X(lifetime->sadb_lifetime_allocations);
		xp->lft.soft_byte_limit = _KEY2X(lifetime->sadb_lifetime_bytes);
		xp->lft.soft_add_expires_seconds = lifetime->sadb_lifetime_addtime;
		xp->lft.soft_use_expires_seconds = lifetime->sadb_lifetime_usetime;
	}
	xp->xfrm_nr = 0;
	if (pol->sadb_x_policy_type == IPSEC_POLICY_IPSEC &&
	    (err = parse_ipsecrequests(xp, pol)) < 0)
		goto out;

	out_skb = pfkey_xfrm_policy2msg_prep(xp);
	if (IS_ERR(out_skb)) {
		err =  PTR_ERR(out_skb);
		goto out;
	}

	err = xfrm_policy_insert(pol->sadb_x_policy_dir-1, xp,
				 hdr->sadb_msg_type != SADB_X_SPDUPDATE);
	if (err) {
		kfree_skb(out_skb);
		goto out;
	}

	pfkey_xfrm_policy2msg(out_skb, xp, pol->sadb_x_policy_dir-1);

	xfrm_pol_put(xp);

	out_hdr = (struct sadb_msg *) out_skb->data;
	out_hdr->sadb_msg_version = hdr->sadb_msg_version;
	out_hdr->sadb_msg_type = hdr->sadb_msg_type;
	out_hdr->sadb_msg_satype = 0;
	out_hdr->sadb_msg_errno = 0;
	out_hdr->sadb_msg_seq = hdr->sadb_msg_seq;
	out_hdr->sadb_msg_pid = hdr->sadb_msg_pid;
	pfkey_broadcast(out_skb, GFP_ATOMIC, BROADCAST_ALL, sk);
	return 0;

out:
	kfree(xp);
	return err;
}

static int pfkey_spddelete(struct sock *sk, struct sk_buff *skb, struct sadb_msg *hdr, void **ext_hdrs)
{
	int err;
	struct sadb_address *sa;
	struct sadb_x_policy *pol;
	struct xfrm_policy *xp;
	struct sk_buff *out_skb;
	struct sadb_msg *out_hdr;
	struct xfrm_selector sel;

	if (!present_and_same_family(ext_hdrs[SADB_EXT_ADDRESS_SRC-1],
				     ext_hdrs[SADB_EXT_ADDRESS_DST-1]) ||
	    !ext_hdrs[SADB_X_EXT_POLICY-1])
		return -EINVAL;

	pol = ext_hdrs[SADB_X_EXT_POLICY-1];
	if (!pol->sadb_x_policy_dir || pol->sadb_x_policy_dir >= IPSEC_DIR_MAX)
		return -EINVAL;

	memset(&sel, 0, sizeof(sel));

	sa = ext_hdrs[SADB_EXT_ADDRESS_SRC-1], 
	sel.family = pfkey_sadb_addr2xfrm_addr(sa, &sel.saddr);
	sel.prefixlen_s = sa->sadb_address_prefixlen;
	sel.proto = pfkey_proto_to_xfrm(sa->sadb_address_proto);
	sel.sport = ((struct sockaddr_in *)(sa+1))->sin_port;
	if (sel.sport)
		sel.sport_mask = ~0;

	sa = ext_hdrs[SADB_EXT_ADDRESS_DST-1], 
	pfkey_sadb_addr2xfrm_addr(sa, &sel.daddr);
	sel.prefixlen_d = sa->sadb_address_prefixlen;
	sel.proto = pfkey_proto_to_xfrm(sa->sadb_address_proto);
	sel.dport = ((struct sockaddr_in *)(sa+1))->sin_port;
	if (sel.dport)
		sel.dport_mask = ~0;

	xp = xfrm_policy_bysel(pol->sadb_x_policy_dir-1, &sel, 1);
	if (xp == NULL)
		return -ENOENT;

	err = 0;

	out_skb = pfkey_xfrm_policy2msg_prep(xp);
	if (IS_ERR(out_skb)) {
		err =  PTR_ERR(out_skb);
		goto out;
	}
	pfkey_xfrm_policy2msg(out_skb, xp, pol->sadb_x_policy_dir-1);

	out_hdr = (struct sadb_msg *) out_skb->data;
	out_hdr->sadb_msg_version = hdr->sadb_msg_version;
	out_hdr->sadb_msg_type = SADB_X_SPDDELETE;
	out_hdr->sadb_msg_satype = 0;
	out_hdr->sadb_msg_errno = 0;
	out_hdr->sadb_msg_seq = hdr->sadb_msg_seq;
	out_hdr->sadb_msg_pid = hdr->sadb_msg_pid;
	pfkey_broadcast(out_skb, GFP_ATOMIC, BROADCAST_ALL, sk);
	err = 0;

out:
	xfrm_pol_put(xp);
	return err;
}

static int pfkey_spdget(struct sock *sk, struct sk_buff *skb, struct sadb_msg *hdr, void **ext_hdrs)
{
	int err;
	struct sadb_x_policy *pol;
	struct xfrm_policy *xp;
	struct sk_buff *out_skb;
	struct sadb_msg *out_hdr;

	if ((pol = ext_hdrs[SADB_X_EXT_POLICY-1]) == NULL)
		return -EINVAL;

	xp = xfrm_policy_byid(0, pol->sadb_x_policy_id,
			      hdr->sadb_msg_type == SADB_X_SPDDELETE2);
	if (xp == NULL)
		return -ENOENT;

	err = 0;

	out_skb = pfkey_xfrm_policy2msg_prep(xp);
	if (IS_ERR(out_skb)) {
		err =  PTR_ERR(out_skb);
		goto out;
	}
	pfkey_xfrm_policy2msg(out_skb, xp, pol->sadb_x_policy_dir-1);

	out_hdr = (struct sadb_msg *) out_skb->data;
	out_hdr->sadb_msg_version = hdr->sadb_msg_version;
	out_hdr->sadb_msg_type = hdr->sadb_msg_type;
	out_hdr->sadb_msg_satype = 0;
	out_hdr->sadb_msg_errno = 0;
	out_hdr->sadb_msg_seq = hdr->sadb_msg_seq;
	out_hdr->sadb_msg_pid = hdr->sadb_msg_pid;
	pfkey_broadcast(out_skb, GFP_ATOMIC, BROADCAST_ALL, sk);
	err = 0;

out:
	xfrm_pol_put(xp);
	return err;
}

static int dump_sp(struct xfrm_policy *xp, int dir, int count, void *ptr)
{
	struct pfkey_dump_data *data = ptr;
	struct sk_buff *out_skb;
	struct sadb_msg *out_hdr;

	out_skb = pfkey_xfrm_policy2msg_prep(xp);
	if (IS_ERR(out_skb))
		return PTR_ERR(out_skb);

	pfkey_xfrm_policy2msg(out_skb, xp, dir);

	out_hdr = (struct sadb_msg *) out_skb->data;
	out_hdr->sadb_msg_version = data->hdr->sadb_msg_version;
	out_hdr->sadb_msg_type = SADB_X_SPDDUMP;
	out_hdr->sadb_msg_satype = SADB_SATYPE_UNSPEC;
	out_hdr->sadb_msg_errno = 0;
	out_hdr->sadb_msg_seq = count;
	out_hdr->sadb_msg_pid = data->hdr->sadb_msg_pid;
	pfkey_broadcast(out_skb, GFP_ATOMIC, BROADCAST_ONE, data->sk);
	return 0;
}

static int pfkey_spddump(struct sock *sk, struct sk_buff *skb, struct sadb_msg *hdr, void **ext_hdrs)
{
	struct pfkey_dump_data data = { .skb = skb, .hdr = hdr, .sk = sk };

	return xfrm_policy_walk(dump_sp, &data);
}

static int pfkey_spdflush(struct sock *sk, struct sk_buff *skb, struct sadb_msg *hdr, void **ext_hdrs)
{
	struct sk_buff *skb_out;
	struct sadb_msg *hdr_out;

	skb_out = alloc_skb(sizeof(struct sadb_msg) + 16, GFP_KERNEL);
	if (!skb_out)
		return -ENOBUFS;

	xfrm_policy_flush();

	hdr_out = (struct sadb_msg *) skb_put(skb_out, sizeof(struct sadb_msg));
	pfkey_hdr_dup(hdr_out, hdr);
	hdr_out->sadb_msg_errno = (uint8_t) 0;
	hdr_out->sadb_msg_len = (sizeof(struct sadb_msg) / sizeof(uint64_t));
	pfkey_broadcast(skb_out, GFP_KERNEL, BROADCAST_ALL, NULL);

	return 0;
}

typedef int (*pfkey_handler)(struct sock *sk, struct sk_buff *skb,
			     struct sadb_msg *hdr, void **ext_hdrs);
static pfkey_handler pfkey_funcs[SADB_MAX + 1] = {
	[SADB_RESERVED]		= pfkey_reserved,
	[SADB_GETSPI]		= pfkey_getspi,
	[SADB_UPDATE]		= pfkey_add,
	[SADB_ADD]		= pfkey_add,
	[SADB_DELETE]		= pfkey_delete,
	[SADB_GET]		= pfkey_get,
	[SADB_ACQUIRE]		= pfkey_acquire,
	[SADB_REGISTER]		= pfkey_register,
	[SADB_EXPIRE]		= NULL,
	[SADB_FLUSH]		= pfkey_flush,
	[SADB_DUMP]		= pfkey_dump,
	[SADB_X_PROMISC]	= pfkey_promisc,
	[SADB_X_PCHANGE]	= NULL,
	[SADB_X_SPDUPDATE]	= pfkey_spdadd,
	[SADB_X_SPDADD]		= pfkey_spdadd,
	[SADB_X_SPDDELETE]	= pfkey_spddelete,
	[SADB_X_SPDGET]		= pfkey_spdget,
	[SADB_X_SPDACQUIRE]	= NULL,
	[SADB_X_SPDDUMP]	= pfkey_spddump,
	[SADB_X_SPDFLUSH]	= pfkey_spdflush,
	[SADB_X_SPDSETIDX]	= pfkey_spdadd,
	[SADB_X_SPDDELETE2]	= pfkey_spdget,
};

static int pfkey_process(struct sock *sk, struct sk_buff *skb, struct sadb_msg *hdr)
{
	void *ext_hdrs[SADB_EXT_MAX];
	int err;

	pfkey_broadcast(skb_clone(skb, GFP_KERNEL), GFP_KERNEL,
			BROADCAST_PROMISC_ONLY, NULL);

	memset(ext_hdrs, 0, sizeof(ext_hdrs));
	err = parse_exthdrs(skb, hdr, ext_hdrs);
	if (!err) {
		err = -EOPNOTSUPP;
		if (pfkey_funcs[hdr->sadb_msg_type])
			err = pfkey_funcs[hdr->sadb_msg_type](sk, skb, hdr, ext_hdrs);
	}
	return err;
}

static struct sadb_msg *pfkey_get_base_msg(struct sk_buff *skb, int *errp)
{
	struct sadb_msg *hdr = NULL;

	if (skb->len < sizeof(*hdr)) {
		*errp = -EMSGSIZE;
	} else {
		hdr = (struct sadb_msg *) skb->data;
		if (hdr->sadb_msg_version != PF_KEY_V2 ||
		    hdr->sadb_msg_reserved != 0 ||
		    (hdr->sadb_msg_type <= SADB_RESERVED ||
		     hdr->sadb_msg_type > SADB_MAX)) {
			hdr = NULL;
			*errp = -EINVAL;
		} else if (hdr->sadb_msg_len != (skb->len /
						 sizeof(uint64_t)) ||
			   hdr->sadb_msg_len < (sizeof(struct sadb_msg) /
						sizeof(uint64_t))) {
			hdr = NULL;
			*errp = -EMSGSIZE;
		} else {
			*errp = 0;
		}
	}
	return hdr;
}

static inline int aalg_tmpl_set(struct xfrm_tmpl *t, struct xfrm_algo_desc *d)
{
	return t->aalgos & (1 << d->desc.sadb_alg_id);
}

static inline int ealg_tmpl_set(struct xfrm_tmpl *t, struct xfrm_algo_desc *d)
{
	return t->ealgos & (1 << d->desc.sadb_alg_id);
}

static int count_ah_combs(struct xfrm_tmpl *t)
{
	int i, sz = 0;

	for (i = 0; ; i++) {
		struct xfrm_algo_desc *aalg = xfrm_aalg_get_byidx(i);
		if (!aalg)
			break;
		if (aalg_tmpl_set(t, aalg) && aalg->available)
			sz += sizeof(struct sadb_comb);
	}
	return sz + sizeof(struct sadb_prop);
}

static int count_esp_combs(struct xfrm_tmpl *t)
{
	int i, k, sz = 0;

	for (i = 0; ; i++) {
		struct xfrm_algo_desc *ealg = xfrm_ealg_get_byidx(i);
		if (!ealg)
			break;
			
		if (!(ealg_tmpl_set(t, ealg) && ealg->available))
			continue;
			
		for (k = 1; ; k++) {
			struct xfrm_algo_desc *aalg = xfrm_aalg_get_byidx(k);
			if (!aalg)
				break;
				
			if (aalg_tmpl_set(t, aalg) && aalg->available)
				sz += sizeof(struct sadb_comb);
		}
	}
	return sz + sizeof(struct sadb_prop);
}

static void dump_ah_combs(struct sk_buff *skb, struct xfrm_tmpl *t)
{
	struct sadb_prop *p;
	int i;

	p = (struct sadb_prop*)skb_put(skb, sizeof(struct sadb_prop));
	p->sadb_prop_len = sizeof(struct sadb_prop)/8;
	p->sadb_prop_exttype = SADB_EXT_PROPOSAL;
	p->sadb_prop_replay = 32;
	memset(p->sadb_prop_reserved, 0, sizeof(p->sadb_prop_reserved));

	for (i = 0; ; i++) {
		struct xfrm_algo_desc *aalg = xfrm_aalg_get_byidx(i);
		if (!aalg)
			break;

		if (aalg_tmpl_set(t, aalg) && aalg->available) {
			struct sadb_comb *c;
			c = (struct sadb_comb*)skb_put(skb, sizeof(struct sadb_comb));
			memset(c, 0, sizeof(*c));
			p->sadb_prop_len += sizeof(struct sadb_comb)/8;
			c->sadb_comb_auth = aalg->desc.sadb_alg_id;
			c->sadb_comb_auth_minbits = aalg->desc.sadb_alg_minbits;
			c->sadb_comb_auth_maxbits = aalg->desc.sadb_alg_maxbits;
			c->sadb_comb_hard_addtime = 24*60*60;
			c->sadb_comb_soft_addtime = 20*60*60;
			c->sadb_comb_hard_usetime = 8*60*60;
			c->sadb_comb_soft_usetime = 7*60*60;
		}
	}
}

static void dump_esp_combs(struct sk_buff *skb, struct xfrm_tmpl *t)
{
	struct sadb_prop *p;
	int i, k;

	p = (struct sadb_prop*)skb_put(skb, sizeof(struct sadb_prop));
	p->sadb_prop_len = sizeof(struct sadb_prop)/8;
	p->sadb_prop_exttype = SADB_EXT_PROPOSAL;
	p->sadb_prop_replay = 32;
	memset(p->sadb_prop_reserved, 0, sizeof(p->sadb_prop_reserved));

	for (i=0; ; i++) {
		struct xfrm_algo_desc *ealg = xfrm_ealg_get_byidx(i);
		if (!ealg)
			break;
	
		if (!(ealg_tmpl_set(t, ealg) && ealg->available))
			continue;
			
		for (k = 1; ; k++) {
			struct sadb_comb *c;
			struct xfrm_algo_desc *aalg = xfrm_aalg_get_byidx(k);
			if (!aalg)
				break;
			if (!(aalg_tmpl_set(t, aalg) && aalg->available))
				continue;
			c = (struct sadb_comb*)skb_put(skb, sizeof(struct sadb_comb));
			memset(c, 0, sizeof(*c));
			p->sadb_prop_len += sizeof(struct sadb_comb)/8;
			c->sadb_comb_auth = aalg->desc.sadb_alg_id;
			c->sadb_comb_auth_minbits = aalg->desc.sadb_alg_minbits;
			c->sadb_comb_auth_maxbits = aalg->desc.sadb_alg_maxbits;
			c->sadb_comb_encrypt = ealg->desc.sadb_alg_id;
			c->sadb_comb_encrypt_minbits = ealg->desc.sadb_alg_minbits;
			c->sadb_comb_encrypt_maxbits = ealg->desc.sadb_alg_maxbits;
			c->sadb_comb_hard_addtime = 24*60*60;
			c->sadb_comb_soft_addtime = 20*60*60;
			c->sadb_comb_hard_usetime = 8*60*60;
			c->sadb_comb_soft_usetime = 7*60*60;
		}
	}
}

static int pfkey_send_notify(struct xfrm_state *x, int hard)
{
	struct sk_buff *out_skb;
	struct sadb_msg *out_hdr;
	int hsc = (hard ? 2 : 1);

	out_skb = pfkey_xfrm_state2msg(x, 0, hsc);
	if (IS_ERR(out_skb))
		return PTR_ERR(out_skb);

	out_hdr = (struct sadb_msg *) out_skb->data;
	out_hdr->sadb_msg_version = PF_KEY_V2;
	out_hdr->sadb_msg_type = SADB_EXPIRE;
	out_hdr->sadb_msg_satype = pfkey_proto2satype(x->id.proto);
	out_hdr->sadb_msg_errno = 0;
	out_hdr->sadb_msg_reserved = 0;
	out_hdr->sadb_msg_seq = 0;
	out_hdr->sadb_msg_pid = 0;

	pfkey_broadcast(out_skb, GFP_ATOMIC, BROADCAST_REGISTERED, NULL);
	return 0;
}

static u32 get_acqseq(void)
{
	u32 res;
	static u32 acqseq;
	static spinlock_t acqseq_lock = SPIN_LOCK_UNLOCKED;

	spin_lock_bh(&acqseq_lock);
	res = (++acqseq ? : ++acqseq);
	spin_unlock_bh(&acqseq_lock);
	return res;
}

static int pfkey_send_acquire(struct xfrm_state *x, struct xfrm_tmpl *t, struct xfrm_policy *xp, int dir)
{
	struct sk_buff *skb;
	struct sadb_msg *hdr;
	struct sadb_address *addr;
	struct sadb_x_policy *pol;
	struct sockaddr_in *sin;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	struct sockaddr_in6 *sin6;
#endif
	int sockaddr_size;
	int size;
	
	sockaddr_size = pfkey_sockaddr_size(x->props.family);
	if (!sockaddr_size)
		return -EINVAL;

	size = sizeof(struct sadb_msg) +
		(sizeof(struct sadb_address) * 2) +
		(sockaddr_size * 2) +
		sizeof(struct sadb_x_policy);
	
	if (x->id.proto == IPPROTO_AH)
		size += count_ah_combs(t);
	else if (x->id.proto == IPPROTO_ESP)
		size += count_esp_combs(t);

	skb =  alloc_skb(size + 16, GFP_ATOMIC);
	if (skb == NULL)
		return -ENOMEM;
	
	hdr = (struct sadb_msg *) skb_put(skb, sizeof(struct sadb_msg));
	hdr->sadb_msg_version = PF_KEY_V2;
	hdr->sadb_msg_type = SADB_ACQUIRE;
	hdr->sadb_msg_satype = pfkey_proto2satype(x->id.proto);
	hdr->sadb_msg_len = size / sizeof(uint64_t);
	hdr->sadb_msg_errno = 0;
	hdr->sadb_msg_reserved = 0;
	hdr->sadb_msg_seq = x->km.seq = get_acqseq();
	hdr->sadb_msg_pid = 0;

	/* src address */
	addr = (struct sadb_address*) skb_put(skb, 
					      sizeof(struct sadb_address)+sockaddr_size);
	addr->sadb_address_len = 
		(sizeof(struct sadb_address)+sockaddr_size)/
			sizeof(uint64_t);
	addr->sadb_address_exttype = SADB_EXT_ADDRESS_SRC;
	addr->sadb_address_proto = 0;
	addr->sadb_address_reserved = 0;
	if (x->props.family == AF_INET) {
		addr->sadb_address_prefixlen = 32;

		sin = (struct sockaddr_in *) (addr + 1);
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = x->props.saddr.a4;
		sin->sin_port = 0;
		memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
	}
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	else if (x->props.family == AF_INET6) {
		addr->sadb_address_prefixlen = 128;

		sin6 = (struct sockaddr_in6 *) (addr + 1);
		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = 0;
		sin6->sin6_flowinfo = 0;
		memcpy(&sin6->sin6_addr,
		       x->props.saddr.a6, sizeof(struct in6_addr));
		sin6->sin6_scope_id = 0;
	}
#endif
	else
		BUG();
	
	/* dst address */
	addr = (struct sadb_address*) skb_put(skb, 
					      sizeof(struct sadb_address)+sockaddr_size);
	addr->sadb_address_len =
		(sizeof(struct sadb_address)+sockaddr_size)/
			sizeof(uint64_t);
	addr->sadb_address_exttype = SADB_EXT_ADDRESS_DST;
	addr->sadb_address_proto = 0;
	addr->sadb_address_reserved = 0;
	if (x->props.family == AF_INET) {
		addr->sadb_address_prefixlen = 32; 

		sin = (struct sockaddr_in *) (addr + 1);
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = x->id.daddr.a4;
		sin->sin_port = 0;
		memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
	}
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	else if (x->props.family == AF_INET6) {
		addr->sadb_address_prefixlen = 128; 

		sin6 = (struct sockaddr_in6 *) (addr + 1);
		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = 0;
		sin6->sin6_flowinfo = 0;
		memcpy(&sin6->sin6_addr,
		       x->id.daddr.a6, sizeof(struct in6_addr));
		sin6->sin6_scope_id = 0;
	}
#endif
	else
		BUG();

	pol = (struct sadb_x_policy *)  skb_put(skb, sizeof(struct sadb_x_policy));
	pol->sadb_x_policy_len = sizeof(struct sadb_x_policy)/sizeof(uint64_t);
	pol->sadb_x_policy_exttype = SADB_X_EXT_POLICY;
	pol->sadb_x_policy_type = IPSEC_POLICY_IPSEC;
	pol->sadb_x_policy_dir = dir+1;
	pol->sadb_x_policy_id = xp->index;

	/* Set sadb_comb's. */
	if (x->id.proto == IPPROTO_AH)
		dump_ah_combs(skb, t);
	else if (x->id.proto == IPPROTO_ESP)
		dump_esp_combs(skb, t);

	return pfkey_broadcast(skb, GFP_ATOMIC, BROADCAST_REGISTERED, NULL);
}

static struct xfrm_policy *pfkey_compile_policy(u16 family, int opt,
                                                u8 *data, int len, int *dir)
{
	struct xfrm_policy *xp;
	struct sadb_x_policy *pol = (struct sadb_x_policy*)data;

	switch (family) {
	case AF_INET:
		if (opt != IP_IPSEC_POLICY) {
			*dir = -EOPNOTSUPP;
			return NULL;
		}
		break;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	case AF_INET6:
		if (opt != IPV6_IPSEC_POLICY) {
			*dir = -EOPNOTSUPP;
			return NULL;
		}
		break;
#endif
	default:
		*dir = -EINVAL;
		return NULL;
	}

	*dir = -EINVAL;

	if (len < sizeof(struct sadb_x_policy) ||
	    pol->sadb_x_policy_len*8 > len ||
	    pol->sadb_x_policy_type > IPSEC_POLICY_BYPASS ||
	    (!pol->sadb_x_policy_dir || pol->sadb_x_policy_dir > IPSEC_DIR_OUTBOUND))
		return NULL;

	xp = xfrm_policy_alloc(GFP_ATOMIC);
	if (xp == NULL) {
		*dir = -ENOBUFS;
		return NULL;
	}

	xp->action = (pol->sadb_x_policy_type == IPSEC_POLICY_DISCARD ?
		      XFRM_POLICY_BLOCK : XFRM_POLICY_ALLOW);

	xp->lft.soft_byte_limit = XFRM_INF;
	xp->lft.hard_byte_limit = XFRM_INF;
	xp->lft.soft_packet_limit = XFRM_INF;
	xp->lft.hard_packet_limit = XFRM_INF;
	xp->family = family;

	xp->xfrm_nr = 0;
	if (pol->sadb_x_policy_type == IPSEC_POLICY_IPSEC &&
	    (*dir = parse_ipsecrequests(xp, pol)) < 0)
		goto out;

	*dir = pol->sadb_x_policy_dir-1;
	return xp;

out:
	kfree(xp);
	return NULL;
}

static int pfkey_send_new_mapping(struct xfrm_state *x, xfrm_address_t *ipaddr, u16 sport)
{
	struct sk_buff *skb;
	struct sadb_msg *hdr;
	struct sadb_sa *sa;
	struct sadb_address *addr;
	struct sadb_x_nat_t_port *n_port;
	struct sockaddr_in *sin;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	struct sockaddr_in6 *sin6;
#endif
	int sockaddr_size;
	int size;
	__u8 satype = (x->id.proto == IPPROTO_ESP ? SADB_SATYPE_ESP : 0);
	struct xfrm_encap_tmpl *natt = NULL;

	sockaddr_size = pfkey_sockaddr_size(x->props.family);
	if (!sockaddr_size)
		return -EINVAL;

	if (!satype)
		return -EINVAL;

	if (!x->encap)
		return -EINVAL;

	natt = x->encap;

	/* Build an SADB_X_NAT_T_NEW_MAPPING message:
	 *
	 * HDR | SA | ADDRESS_SRC (old addr) | NAT_T_SPORT (old port) |
	 * ADDRESS_DST (new addr) | NAT_T_DPORT (new port)
	 */
	
	size = sizeof(struct sadb_msg) +
		sizeof(struct sadb_sa) +
		(sizeof(struct sadb_address) * 2) +
		(sockaddr_size * 2) +
		(sizeof(struct sadb_x_nat_t_port) * 2);
	
	skb =  alloc_skb(size + 16, GFP_ATOMIC);
	if (skb == NULL)
		return -ENOMEM;
	
	hdr = (struct sadb_msg *) skb_put(skb, sizeof(struct sadb_msg));
	hdr->sadb_msg_version = PF_KEY_V2;
	hdr->sadb_msg_type = SADB_X_NAT_T_NEW_MAPPING;
	hdr->sadb_msg_satype = satype;
	hdr->sadb_msg_len = size / sizeof(uint64_t);
	hdr->sadb_msg_errno = 0;
	hdr->sadb_msg_reserved = 0;
	hdr->sadb_msg_seq = x->km.seq = get_acqseq();
	hdr->sadb_msg_pid = 0;

	/* SA */
	sa = (struct sadb_sa *) skb_put(skb, sizeof(struct sadb_sa));
	sa->sadb_sa_len = sizeof(struct sadb_sa)/sizeof(uint64_t);
	sa->sadb_sa_exttype = SADB_EXT_SA;
	sa->sadb_sa_spi = x->id.spi;
	sa->sadb_sa_replay = 0;
	sa->sadb_sa_state = 0;
	sa->sadb_sa_auth = 0;
	sa->sadb_sa_encrypt = 0;
	sa->sadb_sa_flags = 0;

	/* ADDRESS_SRC (old addr) */
	addr = (struct sadb_address*)
		skb_put(skb, sizeof(struct sadb_address)+sockaddr_size);
	addr->sadb_address_len = 
		(sizeof(struct sadb_address)+sockaddr_size)/
			sizeof(uint64_t);
	addr->sadb_address_exttype = SADB_EXT_ADDRESS_SRC;
	addr->sadb_address_proto = 0;
	addr->sadb_address_reserved = 0;
	if (x->props.family == AF_INET) {
		addr->sadb_address_prefixlen = 32;

		sin = (struct sockaddr_in *) (addr + 1);
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = x->props.saddr.a4;
		sin->sin_port = 0;
		memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
	}
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	else if (x->props.family == AF_INET6) {
		addr->sadb_address_prefixlen = 128;

		sin6 = (struct sockaddr_in6 *) (addr + 1);
		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = 0;
		sin6->sin6_flowinfo = 0;
		memcpy(&sin6->sin6_addr,
		       x->props.saddr.a6, sizeof(struct in6_addr));
		sin6->sin6_scope_id = 0;
	}
#endif
	else
		BUG();

	/* NAT_T_SPORT (old port) */
	n_port = (struct sadb_x_nat_t_port*) skb_put(skb, sizeof (*n_port));
	n_port->sadb_x_nat_t_port_len = sizeof(*n_port)/sizeof(uint64_t);
	n_port->sadb_x_nat_t_port_exttype = SADB_X_EXT_NAT_T_SPORT;
	n_port->sadb_x_nat_t_port_port = natt->encap_sport;
	n_port->sadb_x_nat_t_port_reserved = 0;

	/* ADDRESS_DST (new addr) */
	addr = (struct sadb_address*)
		skb_put(skb, sizeof(struct sadb_address)+sockaddr_size);
	addr->sadb_address_len = 
		(sizeof(struct sadb_address)+sockaddr_size)/
			sizeof(uint64_t);
	addr->sadb_address_exttype = SADB_EXT_ADDRESS_DST;
	addr->sadb_address_proto = 0;
	addr->sadb_address_reserved = 0;
	if (x->props.family == AF_INET) {
		addr->sadb_address_prefixlen = 32;

		sin = (struct sockaddr_in *) (addr + 1);
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = ipaddr->a4;
		sin->sin_port = 0;
		memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
	}
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	else if (x->props.family == AF_INET6) {
		addr->sadb_address_prefixlen = 128;

		sin6 = (struct sockaddr_in6 *) (addr + 1);
		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = 0;
		sin6->sin6_flowinfo = 0;
		memcpy(&sin6->sin6_addr, &ipaddr->a6, sizeof(struct in6_addr));
		sin6->sin6_scope_id = 0;
	}
#endif
	else
		BUG();

	/* NAT_T_DPORT (new port) */
	n_port = (struct sadb_x_nat_t_port*) skb_put(skb, sizeof (*n_port));
	n_port->sadb_x_nat_t_port_len = sizeof(*n_port)/sizeof(uint64_t);
	n_port->sadb_x_nat_t_port_exttype = SADB_X_EXT_NAT_T_DPORT;
	n_port->sadb_x_nat_t_port_port = sport;
	n_port->sadb_x_nat_t_port_reserved = 0;

	return pfkey_broadcast(skb, GFP_ATOMIC, BROADCAST_REGISTERED, NULL);
}

static int pfkey_sendmsg(struct kiocb *kiocb,
			 struct socket *sock, struct msghdr *msg, size_t len)
{
	struct sock *sk = sock->sk;
	struct sk_buff *skb = NULL;
	struct sadb_msg *hdr = NULL;
	int err;

	err = -EOPNOTSUPP;
	if (msg->msg_flags & MSG_OOB)
		goto out;

	err = -EMSGSIZE;
	if ((unsigned)len > sk->sk_sndbuf - 32)
		goto out;

	err = -ENOBUFS;
	skb = alloc_skb(len, GFP_KERNEL);
	if (skb == NULL)
		goto out;

	err = -EFAULT;
	if (memcpy_fromiovec(skb_put(skb,len), msg->msg_iov, len))
		goto out;

	hdr = pfkey_get_base_msg(skb, &err);
	if (!hdr)
		goto out;

	down(&xfrm_cfg_sem);
	err = pfkey_process(sk, skb, hdr);
	up(&xfrm_cfg_sem);

out:
	if (err && hdr && pfkey_error(hdr, err, sk) == 0)
		err = 0;
	if (skb)
		kfree_skb(skb);

	return err ? : len;
}

static int pfkey_recvmsg(struct kiocb *kiocb,
			 struct socket *sock, struct msghdr *msg, size_t len,
			 int flags)
{
	struct sock *sk = sock->sk;
	struct sk_buff *skb;
	int copied, err;

	err = -EINVAL;
	if (flags & ~(MSG_PEEK|MSG_DONTWAIT|MSG_TRUNC|MSG_CMSG_COMPAT))
		goto out;

	msg->msg_namelen = 0;
	skb = skb_recv_datagram(sk, flags, flags & MSG_DONTWAIT, &err);
	if (skb == NULL)
		goto out;

	copied = skb->len;
	if (copied > len) {
		msg->msg_flags |= MSG_TRUNC;
		copied = len;
	}

	skb->h.raw = skb->data;
	err = skb_copy_datagram_iovec(skb, 0, msg->msg_iov, copied);
	if (err)
		goto out_free;

	sock_recv_timestamp(msg, sk, skb);

	err = (flags & MSG_TRUNC) ? skb->len : copied;

out_free:
	skb_free_datagram(sk, skb);
out:
	return err;
}

static struct proto_ops pfkey_ops = {
	.family		=	PF_KEY,
	.owner		=	THIS_MODULE,
	/* Operations that make no sense on pfkey sockets. */
	.bind		=	sock_no_bind,
	.connect	=	sock_no_connect,
	.socketpair	=	sock_no_socketpair,
	.accept		=	sock_no_accept,
	.getname	=	sock_no_getname,
	.ioctl		=	sock_no_ioctl,
	.listen		=	sock_no_listen,
	.shutdown	=	sock_no_shutdown,
	.setsockopt	=	sock_no_setsockopt,
	.getsockopt	=	sock_no_getsockopt,
	.mmap		=	sock_no_mmap,
	.sendpage	=	sock_no_sendpage,

	/* Now the operations that really occur. */
	.release	=	pfkey_release,
	.poll		=	datagram_poll,
	.sendmsg	=	pfkey_sendmsg,
	.recvmsg	=	pfkey_recvmsg,
};

static struct net_proto_family pfkey_family_ops = {
	.family	=	PF_KEY,
	.create	=	pfkey_create,
	.owner	=	THIS_MODULE,
};

#ifdef CONFIG_PROC_FS
static int pfkey_read_proc(char *buffer, char **start, off_t offset,
			   int length, int *eof, void *data)
{
	off_t pos = 0;
	off_t begin = 0;
	int len = 0;
	struct sock *s;
	struct hlist_node *node;

	len += sprintf(buffer,"sk       RefCnt Rmem   Wmem   User   Inode\n");

	read_lock(&pfkey_table_lock);

	sk_for_each(s, node, &pfkey_table) {
		len += sprintf(buffer+len,"%p %-6d %-6u %-6u %-6u %-6lu",
			       s,
			       atomic_read(&s->sk_refcnt),
			       atomic_read(&s->sk_rmem_alloc),
			       atomic_read(&s->sk_wmem_alloc),
			       sock_i_uid(s),
			       sock_i_ino(s)
			       );

		buffer[len++] = '\n';
		
		pos = begin + len;
		if (pos < offset) {
			len = 0;
			begin = pos;
		}
		if(pos > offset + length)
			goto done;
	}
	*eof = 1;

done:
	read_unlock(&pfkey_table_lock);

	*start = buffer + (offset - begin);
	len -= (offset - begin);

	if (len > length)
		len = length;
	if (len < 0)
		len = 0;

	return len;
}
#endif

static struct xfrm_mgr pfkeyv2_mgr =
{
	.id		= "pfkeyv2",
	.notify		= pfkey_send_notify,
	.acquire	= pfkey_send_acquire,
	.compile_policy	= pfkey_compile_policy,
	.new_mapping	= pfkey_send_new_mapping,
};

static void __exit ipsec_pfkey_exit(void)
{
	xfrm_unregister_km(&pfkeyv2_mgr);
	remove_proc_entry("net/pfkey", NULL);
	sock_unregister(PF_KEY);
}

static int __init ipsec_pfkey_init(void)
{
	sock_register(&pfkey_family_ops);
#ifdef CONFIG_PROC_FS
	create_proc_read_entry("net/pfkey", 0, NULL, pfkey_read_proc, NULL);
#endif
	xfrm_register_km(&pfkeyv2_mgr);
	return 0;
}

module_init(ipsec_pfkey_init);
module_exit(ipsec_pfkey_exit);
MODULE_LICENSE("GPL");
MODULE_ALIAS_NETPROTO(PF_KEY);
