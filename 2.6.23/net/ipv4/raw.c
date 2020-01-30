/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		RAW - implementation of IP "raw" sockets.
 *
 * Version:	$Id: raw.c,v 1.64 2002/02/01 22:01:04 davem Exp $
 *
 * Authors:	Ross Biro
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 * Fixes:
 *		Alan Cox	:	verify_area() fixed up
 *		Alan Cox	:	ICMP error handling
 *		Alan Cox	:	EMSGSIZE if you send too big a packet
 *		Alan Cox	: 	Now uses generic datagrams and shared
 *					skbuff library. No more peek crashes,
 *					no more backlogs
 *		Alan Cox	:	Checks sk->broadcast.
 *		Alan Cox	:	Uses skb_free_datagram/skb_copy_datagram
 *		Alan Cox	:	Raw passes ip options too
 *		Alan Cox	:	Setsocketopt added
 *		Alan Cox	:	Fixed error return for broadcasts
 *		Alan Cox	:	Removed wake_up calls
 *		Alan Cox	:	Use ttl/tos
 *		Alan Cox	:	Cleaned up old debugging
 *		Alan Cox	:	Use new kernel side addresses
 *	Arnt Gulbrandsen	:	Fixed MSG_DONTROUTE in raw sockets.
 *		Alan Cox	:	BSD style RAW socket demultiplexing.
 *		Alan Cox	:	Beginnings of mrouted support.
 *		Alan Cox	:	Added IP_HDRINCL option.
 *		Alan Cox	:	Skip broadcast check if BSDism set.
 *		David S. Miller	:	New socket lookup architecture.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */

#include <linux/types.h>
#include <asm/atomic.h>
#include <asm/byteorder.h>
#include <asm/current.h>
#include <asm/uaccess.h>
#include <asm/ioctls.h>
#include <linux/stddef.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/aio.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/sockios.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/mroute.h>
#include <linux/netdevice.h>
#include <linux/in_route.h>
#include <linux/route.h>
#include <linux/skbuff.h>
#include <net/dst.h>
#include <net/sock.h>
#include <linux/gfp.h>
#include <linux/ip.h>
#include <linux/net.h>
#include <net/ip.h>
#include <net/icmp.h>
#include <net/udp.h>
#include <net/raw.h>
#include <net/snmp.h>
#include <net/tcp_states.h>
#include <net/inet_common.h>
#include <net/checksum.h>
#include <net/xfrm.h>
#include <linux/rtnetlink.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>

struct hlist_head raw_v4_htable[RAWV4_HTABLE_SIZE];
DEFINE_RWLOCK(raw_v4_lock);

static void raw_v4_hash(struct sock *sk)
{
	struct hlist_head *head = &raw_v4_htable[inet_sk(sk)->num &
						 (RAWV4_HTABLE_SIZE - 1)];

	write_lock_bh(&raw_v4_lock);
	sk_add_node(sk, head);
	sock_prot_inc_use(sk->sk_prot);
	write_unlock_bh(&raw_v4_lock);
}

static void raw_v4_unhash(struct sock *sk)
{
	write_lock_bh(&raw_v4_lock);
	if (sk_del_node_init(sk))
		sock_prot_dec_use(sk->sk_prot);
	write_unlock_bh(&raw_v4_lock);
}

struct sock *__raw_v4_lookup(struct sock *sk, unsigned short num,
			     __be32 raddr, __be32 laddr,
			     int dif)
{
	struct hlist_node *node;

	sk_for_each_from(sk, node) {
		struct inet_sock *inet = inet_sk(sk);

		if (inet->num == num 					&&
		    !(inet->daddr && inet->daddr != raddr) 		&&
		    !(inet->rcv_saddr && inet->rcv_saddr != laddr)	&&
		    !(sk->sk_bound_dev_if && sk->sk_bound_dev_if != dif))
			goto found; /* gotcha */
	}
	sk = NULL;
found:
	return sk;
}

/*
 *	0 - deliver
 *	1 - block
 */
static __inline__ int icmp_filter(struct sock *sk, struct sk_buff *skb)
{
	int type;

	if (!pskb_may_pull(skb, sizeof(struct icmphdr)))
		return 1;

	type = icmp_hdr(skb)->type;
	if (type < 32) {
		__u32 data = raw_sk(sk)->filter.data;

		return ((1 << type) & data) != 0;
	}

	/* Do not block unknown ICMP types */
	return 0;
}

/* IP input processing comes here for RAW socket delivery.
 * Caller owns SKB, so we must make clones.
 *
 * RFC 1122: SHOULD pass TOS value up to the transport layer.
 * -> It does. And not only TOS, but all IP header.
 */
int raw_v4_input(struct sk_buff *skb, struct iphdr *iph, int hash)
{
	struct sock *sk;
	struct hlist_head *head;
	int delivered = 0;

	read_lock(&raw_v4_lock);
	head = &raw_v4_htable[hash];
	if (hlist_empty(head))
		goto out;
	sk = __raw_v4_lookup(__sk_head(head), iph->protocol,
			     iph->saddr, iph->daddr,
			     skb->dev->ifindex);

	while (sk) {
		delivered = 1;
		if (iph->protocol != IPPROTO_ICMP || !icmp_filter(sk, skb)) {
			struct sk_buff *clone = skb_clone(skb, GFP_ATOMIC);

			/* Not releasing hash table! */
			if (clone)
				raw_rcv(sk, clone);
		}
		sk = __raw_v4_lookup(sk_next(sk), iph->protocol,
				     iph->saddr, iph->daddr,
				     skb->dev->ifindex);
	}
out:
	read_unlock(&raw_v4_lock);
	return delivered;
}

void raw_err (struct sock *sk, struct sk_buff *skb, u32 info)
{
	struct inet_sock *inet = inet_sk(sk);
	const int type = icmp_hdr(skb)->type;
	const int code = icmp_hdr(skb)->code;
	int err = 0;
	int harderr = 0;

	/* Report error on raw socket, if:
	   1. User requested ip_recverr.
	   2. Socket is connected (otherwise the error indication
	      is useless without ip_recverr and error is hard.
	 */
	if (!inet->recverr && sk->sk_state != TCP_ESTABLISHED)
		return;

	switch (type) {
	default:
	case ICMP_TIME_EXCEEDED:
		err = EHOSTUNREACH;
		break;
	case ICMP_SOURCE_QUENCH:
		return;
	case ICMP_PARAMETERPROB:
		err = EPROTO;
		harderr = 1;
		break;
	case ICMP_DEST_UNREACH:
		err = EHOSTUNREACH;
		if (code > NR_ICMP_UNREACH)
			break;
		err = icmp_err_convert[code].errno;
		harderr = icmp_err_convert[code].fatal;
		if (code == ICMP_FRAG_NEEDED) {
			harderr = inet->pmtudisc != IP_PMTUDISC_DONT;
			err = EMSGSIZE;
		}
	}

	if (inet->recverr) {
		struct iphdr *iph = (struct iphdr*)skb->data;
		u8 *payload = skb->data + (iph->ihl << 2);

		if (inet->hdrincl)
			payload = skb->data;
		ip_icmp_error(sk, skb, err, 0, info, payload);
	}

	if (inet->recverr || harderr) {
		sk->sk_err = err;
		sk->sk_error_report(sk);
	}
}

static int raw_rcv_skb(struct sock * sk, struct sk_buff * skb)
{
	/* Charge it to the socket. */

	if (sock_queue_rcv_skb(sk, skb) < 0) {
		/* FIXME: increment a raw drops counter here */
		kfree_skb(skb);
		return NET_RX_DROP;
	}

	return NET_RX_SUCCESS;
}

int raw_rcv(struct sock *sk, struct sk_buff *skb)
{
	if (!xfrm4_policy_check(sk, XFRM_POLICY_IN, skb)) {
		kfree_skb(skb);
		return NET_RX_DROP;
	}
	nf_reset(skb);

	skb_push(skb, skb->data - skb_network_header(skb));

	raw_rcv_skb(sk, skb);
	return 0;
}

static int raw_send_hdrinc(struct sock *sk, void *from, size_t length,
			struct rtable *rt,
			unsigned int flags)
{
	struct inet_sock *inet = inet_sk(sk);
	int hh_len;
	struct iphdr *iph;
	struct sk_buff *skb;
	int err;

	if (length > rt->u.dst.dev->mtu) {
		ip_local_error(sk, EMSGSIZE, rt->rt_dst, inet->dport,
			       rt->u.dst.dev->mtu);
		return -EMSGSIZE;
	}
	if (flags&MSG_PROBE)
		goto out;

	hh_len = LL_RESERVED_SPACE(rt->u.dst.dev);

	skb = sock_alloc_send_skb(sk, length+hh_len+15,
				  flags&MSG_DONTWAIT, &err);
	if (skb == NULL)
		goto error;
	skb_reserve(skb, hh_len);

	skb->priority = sk->sk_priority;
	skb->dst = dst_clone(&rt->u.dst);

	skb_reset_network_header(skb);
	iph = ip_hdr(skb);
	skb_put(skb, length);

	skb->ip_summed = CHECKSUM_NONE;

	skb->transport_header = skb->network_header;
	err = memcpy_fromiovecend((void *)iph, from, 0, length);
	if (err)
		goto error_fault;

	/* We don't modify invalid header */
	if (length >= sizeof(*iph) && iph->ihl * 4U <= length) {
		if (!iph->saddr)
			iph->saddr = rt->rt_src;
		iph->check   = 0;
		iph->tot_len = htons(length);
		if (!iph->id)
			ip_select_ident(iph, &rt->u.dst, NULL);

		iph->check = ip_fast_csum((unsigned char *)iph, iph->ihl);
	}

	err = NF_HOOK(PF_INET, NF_IP_LOCAL_OUT, skb, NULL, rt->u.dst.dev,
		      dst_output);
	if (err > 0)
		err = inet->recverr ? net_xmit_errno(err) : 0;
	if (err)
		goto error;
out:
	return 0;

error_fault:
	err = -EFAULT;
	kfree_skb(skb);
error:
	IP_INC_STATS(IPSTATS_MIB_OUTDISCARDS);
	return err;
}

static int raw_probe_proto_opt(struct flowi *fl, struct msghdr *msg)
{
	struct iovec *iov;
	u8 __user *type = NULL;
	u8 __user *code = NULL;
	int probed = 0;
	unsigned int i;

	if (!msg->msg_iov)
		return 0;

	for (i = 0; i < msg->msg_iovlen; i++) {
		iov = &msg->msg_iov[i];
		if (!iov)
			continue;

		switch (fl->proto) {
		case IPPROTO_ICMP:
			/* check if one-byte field is readable or not. */
			if (iov->iov_base && iov->iov_len < 1)
				break;

			if (!type) {
				type = iov->iov_base;
				/* check if code field is readable or not. */
				if (iov->iov_len > 1)
					code = type + 1;
			} else if (!code)
				code = iov->iov_base;

			if (type && code) {
				if (get_user(fl->fl_icmp_type, type) ||
				    get_user(fl->fl_icmp_code, code))
					return -EFAULT;
				probed = 1;
			}
			break;
		default:
			probed = 1;
			break;
		}
		if (probed)
			break;
	}
	return 0;
}

static int raw_sendmsg(struct kiocb *iocb, struct sock *sk, struct msghdr *msg,
		       size_t len)
{
	struct inet_sock *inet = inet_sk(sk);
	struct ipcm_cookie ipc;
	struct rtable *rt = NULL;
	int free = 0;
	__be32 daddr;
	__be32 saddr;
	u8  tos;
	int err;

	err = -EMSGSIZE;
	if (len > 0xFFFF)
		goto out;

	/*
	 *	Check the flags.
	 */

	err = -EOPNOTSUPP;
	if (msg->msg_flags & MSG_OOB)	/* Mirror BSD error message */
		goto out;               /* compatibility */

	/*
	 *	Get and verify the address.
	 */

	if (msg->msg_namelen) {
		struct sockaddr_in *usin = (struct sockaddr_in*)msg->msg_name;
		err = -EINVAL;
		if (msg->msg_namelen < sizeof(*usin))
			goto out;
		if (usin->sin_family != AF_INET) {
			static int complained;
			if (!complained++)
				printk(KERN_INFO "%s forgot to set AF_INET in "
						 "raw sendmsg. Fix it!\n",
						 current->comm);
			err = -EAFNOSUPPORT;
			if (usin->sin_family)
				goto out;
		}
		daddr = usin->sin_addr.s_addr;
		/* ANK: I did not forget to get protocol from port field.
		 * I just do not know, who uses this weirdness.
		 * IP_HDRINCL is much more convenient.
		 */
	} else {
		err = -EDESTADDRREQ;
		if (sk->sk_state != TCP_ESTABLISHED)
			goto out;
		daddr = inet->daddr;
	}

	ipc.addr = inet->saddr;
	ipc.opt = NULL;
	ipc.oif = sk->sk_bound_dev_if;

	if (msg->msg_controllen) {
		err = ip_cmsg_send(msg, &ipc);
		if (err)
			goto out;
		if (ipc.opt)
			free = 1;
	}

	saddr = ipc.addr;
	ipc.addr = daddr;

	if (!ipc.opt)
		ipc.opt = inet->opt;

	if (ipc.opt) {
		err = -EINVAL;
		/* Linux does not mangle headers on raw sockets,
		 * so that IP options + IP_HDRINCL is non-sense.
		 */
		if (inet->hdrincl)
			goto done;
		if (ipc.opt->srr) {
			if (!daddr)
				goto done;
			daddr = ipc.opt->faddr;
		}
	}
	tos = RT_CONN_FLAGS(sk);
	if (msg->msg_flags & MSG_DONTROUTE)
		tos |= RTO_ONLINK;

	if (MULTICAST(daddr)) {
		if (!ipc.oif)
			ipc.oif = inet->mc_index;
		if (!saddr)
			saddr = inet->mc_addr;
	}

	{
		struct flowi fl = { .oif = ipc.oif,
				    .nl_u = { .ip4_u =
					      { .daddr = daddr,
						.saddr = saddr,
						.tos = tos } },
				    .proto = inet->hdrincl ? IPPROTO_RAW :
							     sk->sk_protocol,
				  };
		if (!inet->hdrincl) {
			err = raw_probe_proto_opt(&fl, msg);
			if (err)
				goto done;
		}

		security_sk_classify_flow(sk, &fl);
		err = ip_route_output_flow(&rt, &fl, sk, 1);
	}
	if (err)
		goto done;

	err = -EACCES;
	if (rt->rt_flags & RTCF_BROADCAST && !sock_flag(sk, SOCK_BROADCAST))
		goto done;

	if (msg->msg_flags & MSG_CONFIRM)
		goto do_confirm;
back_from_confirm:

	if (inet->hdrincl)
		err = raw_send_hdrinc(sk, msg->msg_iov, len,
					rt, msg->msg_flags);

	 else {
		if (!ipc.addr)
			ipc.addr = rt->rt_dst;
		lock_sock(sk);
		err = ip_append_data(sk, ip_generic_getfrag, msg->msg_iov, len, 0,
					&ipc, rt, msg->msg_flags);
		if (err)
			ip_flush_pending_frames(sk);
		else if (!(msg->msg_flags & MSG_MORE))
			err = ip_push_pending_frames(sk);
		release_sock(sk);
	}
done:
	if (free)
		kfree(ipc.opt);
	ip_rt_put(rt);

out:
	if (err < 0)
		return err;
	return len;

do_confirm:
	dst_confirm(&rt->u.dst);
	if (!(msg->msg_flags & MSG_PROBE) || len)
		goto back_from_confirm;
	err = 0;
	goto done;
}

static void raw_close(struct sock *sk, long timeout)
{
	/*
	 * Raw sockets may have direct kernel refereneces. Kill them.
	 */
	ip_ra_control(sk, 0, NULL);

	sk_common_release(sk);
}

/* This gets rid of all the nasties in af_inet. -DaveM */
static int raw_bind(struct sock *sk, struct sockaddr *uaddr, int addr_len)
{
	struct inet_sock *inet = inet_sk(sk);
	struct sockaddr_in *addr = (struct sockaddr_in *) uaddr;
	int ret = -EINVAL;
	int chk_addr_ret;

	if (sk->sk_state != TCP_CLOSE || addr_len < sizeof(struct sockaddr_in))
		goto out;
	chk_addr_ret = inet_addr_type(addr->sin_addr.s_addr);
	ret = -EADDRNOTAVAIL;
	if (addr->sin_addr.s_addr && chk_addr_ret != RTN_LOCAL &&
	    chk_addr_ret != RTN_MULTICAST && chk_addr_ret != RTN_BROADCAST)
		goto out;
	inet->rcv_saddr = inet->saddr = addr->sin_addr.s_addr;
	if (chk_addr_ret == RTN_MULTICAST || chk_addr_ret == RTN_BROADCAST)
		inet->saddr = 0;  /* Use device */
	sk_dst_reset(sk);
	ret = 0;
out:	return ret;
}

/*
 *	This should be easy, if there is something there
 *	we return it, otherwise we block.
 */

static int raw_recvmsg(struct kiocb *iocb, struct sock *sk, struct msghdr *msg,
		       size_t len, int noblock, int flags, int *addr_len)
{
	struct inet_sock *inet = inet_sk(sk);
	size_t copied = 0;
	int err = -EOPNOTSUPP;
	struct sockaddr_in *sin = (struct sockaddr_in *)msg->msg_name;
	struct sk_buff *skb;

	if (flags & MSG_OOB)
		goto out;

	if (addr_len)
		*addr_len = sizeof(*sin);

	if (flags & MSG_ERRQUEUE) {
		err = ip_recv_error(sk, msg, len);
		goto out;
	}

	skb = skb_recv_datagram(sk, flags, noblock, &err);
	if (!skb)
		goto out;

	copied = skb->len;
	if (len < copied) {
		msg->msg_flags |= MSG_TRUNC;
		copied = len;
	}

	err = skb_copy_datagram_iovec(skb, 0, msg->msg_iov, copied);
	if (err)
		goto done;

	sock_recv_timestamp(msg, sk, skb);

	/* Copy the address. */
	if (sin) {
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = ip_hdr(skb)->saddr;
		sin->sin_port = 0;
		memset(&sin->sin_zero, 0, sizeof(sin->sin_zero));
	}
	if (inet->cmsg_flags)
		ip_cmsg_recv(msg, skb);
	if (flags & MSG_TRUNC)
		copied = skb->len;
done:
	skb_free_datagram(sk, skb);
out:
	if (err)
		return err;
	return copied;
}

static int raw_init(struct sock *sk)
{
	struct raw_sock *rp = raw_sk(sk);

	if (inet_sk(sk)->num == IPPROTO_ICMP)
		memset(&rp->filter, 0, sizeof(rp->filter));
	return 0;
}

static int raw_seticmpfilter(struct sock *sk, char __user *optval, int optlen)
{
	if (optlen > sizeof(struct icmp_filter))
		optlen = sizeof(struct icmp_filter);
	if (copy_from_user(&raw_sk(sk)->filter, optval, optlen))
		return -EFAULT;
	return 0;
}

static int raw_geticmpfilter(struct sock *sk, char __user *optval, int __user *optlen)
{
	int len, ret = -EFAULT;

	if (get_user(len, optlen))
		goto out;
	ret = -EINVAL;
	if (len < 0)
		goto out;
	if (len > sizeof(struct icmp_filter))
		len = sizeof(struct icmp_filter);
	ret = -EFAULT;
	if (put_user(len, optlen) ||
	    copy_to_user(optval, &raw_sk(sk)->filter, len))
		goto out;
	ret = 0;
out:	return ret;
}

static int do_raw_setsockopt(struct sock *sk, int level, int optname,
			  char __user *optval, int optlen)
{
	if (optname == ICMP_FILTER) {
		if (inet_sk(sk)->num != IPPROTO_ICMP)
			return -EOPNOTSUPP;
		else
			return raw_seticmpfilter(sk, optval, optlen);
	}
	return -ENOPROTOOPT;
}

static int raw_setsockopt(struct sock *sk, int level, int optname,
			  char __user *optval, int optlen)
{
	if (level != SOL_RAW)
		return ip_setsockopt(sk, level, optname, optval, optlen);
	return do_raw_setsockopt(sk, level, optname, optval, optlen);
}

#ifdef CONFIG_COMPAT
static int compat_raw_setsockopt(struct sock *sk, int level, int optname,
				 char __user *optval, int optlen)
{
	if (level != SOL_RAW)
		return compat_ip_setsockopt(sk, level, optname, optval, optlen);
	return do_raw_setsockopt(sk, level, optname, optval, optlen);
}
#endif

static int do_raw_getsockopt(struct sock *sk, int level, int optname,
			  char __user *optval, int __user *optlen)
{
	if (optname == ICMP_FILTER) {
		if (inet_sk(sk)->num != IPPROTO_ICMP)
			return -EOPNOTSUPP;
		else
			return raw_geticmpfilter(sk, optval, optlen);
	}
	return -ENOPROTOOPT;
}

static int raw_getsockopt(struct sock *sk, int level, int optname,
			  char __user *optval, int __user *optlen)
{
	if (level != SOL_RAW)
		return ip_getsockopt(sk, level, optname, optval, optlen);
	return do_raw_getsockopt(sk, level, optname, optval, optlen);
}

#ifdef CONFIG_COMPAT
static int compat_raw_getsockopt(struct sock *sk, int level, int optname,
				 char __user *optval, int __user *optlen)
{
	if (level != SOL_RAW)
		return compat_ip_getsockopt(sk, level, optname, optval, optlen);
	return do_raw_getsockopt(sk, level, optname, optval, optlen);
}
#endif

static int raw_ioctl(struct sock *sk, int cmd, unsigned long arg)
{
	switch (cmd) {
		case SIOCOUTQ: {
			int amount = atomic_read(&sk->sk_wmem_alloc);
			return put_user(amount, (int __user *)arg);
		}
		case SIOCINQ: {
			struct sk_buff *skb;
			int amount = 0;

			spin_lock_bh(&sk->sk_receive_queue.lock);
			skb = skb_peek(&sk->sk_receive_queue);
			if (skb != NULL)
				amount = skb->len;
			spin_unlock_bh(&sk->sk_receive_queue.lock);
			return put_user(amount, (int __user *)arg);
		}

		default:
#ifdef CONFIG_IP_MROUTE
			return ipmr_ioctl(sk, cmd, (void __user *)arg);
#else
			return -ENOIOCTLCMD;
#endif
	}
}

struct proto raw_prot = {
	.name		   = "RAW",
	.owner		   = THIS_MODULE,
	.close		   = raw_close,
	.connect	   = ip4_datagram_connect,
	.disconnect	   = udp_disconnect,
	.ioctl		   = raw_ioctl,
	.init		   = raw_init,
	.setsockopt	   = raw_setsockopt,
	.getsockopt	   = raw_getsockopt,
	.sendmsg	   = raw_sendmsg,
	.recvmsg	   = raw_recvmsg,
	.bind		   = raw_bind,
	.backlog_rcv	   = raw_rcv_skb,
	.hash		   = raw_v4_hash,
	.unhash		   = raw_v4_unhash,
	.obj_size	   = sizeof(struct raw_sock),
#ifdef CONFIG_COMPAT
	.compat_setsockopt = compat_raw_setsockopt,
	.compat_getsockopt = compat_raw_getsockopt,
#endif
};

#ifdef CONFIG_PROC_FS
struct raw_iter_state {
	int bucket;
};

#define raw_seq_private(seq) ((struct raw_iter_state *)(seq)->private)

static struct sock *raw_get_first(struct seq_file *seq)
{
	struct sock *sk;
	struct raw_iter_state* state = raw_seq_private(seq);

	for (state->bucket = 0; state->bucket < RAWV4_HTABLE_SIZE; ++state->bucket) {
		struct hlist_node *node;

		sk_for_each(sk, node, &raw_v4_htable[state->bucket])
			if (sk->sk_family == PF_INET)
				goto found;
	}
	sk = NULL;
found:
	return sk;
}

static struct sock *raw_get_next(struct seq_file *seq, struct sock *sk)
{
	struct raw_iter_state* state = raw_seq_private(seq);

	do {
		sk = sk_next(sk);
try_again:
		;
	} while (sk && sk->sk_family != PF_INET);

	if (!sk && ++state->bucket < RAWV4_HTABLE_SIZE) {
		sk = sk_head(&raw_v4_htable[state->bucket]);
		goto try_again;
	}
	return sk;
}

static struct sock *raw_get_idx(struct seq_file *seq, loff_t pos)
{
	struct sock *sk = raw_get_first(seq);

	if (sk)
		while (pos && (sk = raw_get_next(seq, sk)) != NULL)
			--pos;
	return pos ? NULL : sk;
}

static void *raw_seq_start(struct seq_file *seq, loff_t *pos)
{
	read_lock(&raw_v4_lock);
	return *pos ? raw_get_idx(seq, *pos - 1) : SEQ_START_TOKEN;
}

static void *raw_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct sock *sk;

	if (v == SEQ_START_TOKEN)
		sk = raw_get_first(seq);
	else
		sk = raw_get_next(seq, v);
	++*pos;
	return sk;
}

static void raw_seq_stop(struct seq_file *seq, void *v)
{
	read_unlock(&raw_v4_lock);
}

static __inline__ char *get_raw_sock(struct sock *sp, char *tmpbuf, int i)
{
	struct inet_sock *inet = inet_sk(sp);
	__be32 dest = inet->daddr,
	       src = inet->rcv_saddr;
	__u16 destp = 0,
	      srcp  = inet->num;

	sprintf(tmpbuf, "%4d: %08X:%04X %08X:%04X"
		" %02X %08X:%08X %02X:%08lX %08X %5d %8d %lu %d %p",
		i, src, srcp, dest, destp, sp->sk_state,
		atomic_read(&sp->sk_wmem_alloc),
		atomic_read(&sp->sk_rmem_alloc),
		0, 0L, 0, sock_i_uid(sp), 0, sock_i_ino(sp),
		atomic_read(&sp->sk_refcnt), sp);
	return tmpbuf;
}

static int raw_seq_show(struct seq_file *seq, void *v)
{
	char tmpbuf[129];

	if (v == SEQ_START_TOKEN)
		seq_printf(seq, "%-127s\n",
			       "  sl  local_address rem_address   st tx_queue "
			       "rx_queue tr tm->when retrnsmt   uid  timeout "
			       "inode");
	else {
		struct raw_iter_state *state = raw_seq_private(seq);

		seq_printf(seq, "%-127s\n",
			   get_raw_sock(v, tmpbuf, state->bucket));
	}
	return 0;
}

static const struct seq_operations raw_seq_ops = {
	.start = raw_seq_start,
	.next  = raw_seq_next,
	.stop  = raw_seq_stop,
	.show  = raw_seq_show,
};

static int raw_seq_open(struct inode *inode, struct file *file)
{
	struct seq_file *seq;
	int rc = -ENOMEM;
	struct raw_iter_state *s;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		goto out;
	rc = seq_open(file, &raw_seq_ops);
	if (rc)
		goto out_kfree;

	seq = file->private_data;
	seq->private = s;
out:
	return rc;
out_kfree:
	kfree(s);
	goto out;
}

static const struct file_operations raw_seq_fops = {
	.owner	 = THIS_MODULE,
	.open	 = raw_seq_open,
	.read	 = seq_read,
	.llseek	 = seq_lseek,
	.release = seq_release_private,
};

int __init raw_proc_init(void)
{
	if (!proc_net_fops_create("raw", S_IRUGO, &raw_seq_fops))
		return -ENOMEM;
	return 0;
}

void __init raw_proc_exit(void)
{
	proc_net_remove("raw");
}
#endif /* CONFIG_PROC_FS */
