/*
 *	TCP over IPv6
 *	Linux INET6 implementation 
 *
 *	Authors:
 *	Pedro Roque		<roque@di.fc.ul.pt>	
 *
 *	$Id: tcp_ipv6.c,v 1.144 2002/02/01 22:01:04 davem Exp $
 *
 *	Based on: 
 *	linux/net/ipv4/tcp.c
 *	linux/net/ipv4/tcp_input.c
 *	linux/net/ipv4/tcp_output.c
 *
 *	Fixes:
 *	Hideaki YOSHIFUJI	:	sin6_scope_id support
 *	YOSHIFUJI Hideaki @USAGI and:	Support IPV6_V6ONLY socket option, which
 *	Alexey Kuznetsov		allow both IPv4 and IPv6 sockets to bind
 *					a single port at the same time.
 *	YOSHIFUJI Hideaki @USAGI:	convert /proc/net/tcp6 to seq_file.
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/config.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/jiffies.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/netdevice.h>
#include <linux/init.h>
#include <linux/jhash.h>
#include <linux/ipsec.h>
#include <linux/times.h>

#include <linux/ipv6.h>
#include <linux/icmpv6.h>
#include <linux/random.h>

#include <net/tcp.h>
#include <net/ndisc.h>
#include <net/ipv6.h>
#include <net/transp_v6.h>
#include <net/addrconf.h>
#include <net/ip6_route.h>
#include <net/ip6_checksum.h>
#include <net/inet_ecn.h>
#include <net/protocol.h>
#include <net/xfrm.h>
#include <net/addrconf.h>
#include <net/snmp.h>

#include <asm/uaccess.h>

#include <linux/proc_fs.h>
#include <linux/seq_file.h>

static void	tcp_v6_send_reset(struct sk_buff *skb);
static void	tcp_v6_or_send_ack(struct sk_buff *skb, struct open_request *req);
static void	tcp_v6_send_check(struct sock *sk, struct tcphdr *th, int len, 
				  struct sk_buff *skb);

static int	tcp_v6_do_rcv(struct sock *sk, struct sk_buff *skb);
static int	tcp_v6_xmit(struct sk_buff *skb, int ipfragok);

static struct tcp_func ipv6_mapped;
static struct tcp_func ipv6_specific;

/* I have no idea if this is a good hash for v6 or not. -DaveM */
static __inline__ int tcp_v6_hashfn(struct in6_addr *laddr, u16 lport,
				    struct in6_addr *faddr, u16 fport)
{
	int hashent = (lport ^ fport);

	hashent ^= (laddr->s6_addr32[3] ^ faddr->s6_addr32[3]);
	hashent ^= hashent>>16;
	hashent ^= hashent>>8;
	return (hashent & (tcp_ehash_size - 1));
}

static __inline__ int tcp_v6_sk_hashfn(struct sock *sk)
{
	struct inet_opt *inet = inet_sk(sk);
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct in6_addr *laddr = &np->rcv_saddr;
	struct in6_addr *faddr = &np->daddr;
	__u16 lport = inet->num;
	__u16 fport = inet->dport;
	return tcp_v6_hashfn(laddr, lport, faddr, fport);
}

static inline int tcp_v6_bind_conflict(struct sock *sk,
				       struct tcp_bind_bucket *tb)
{
	struct sock *sk2;
	struct hlist_node *node;

	/* We must walk the whole port owner list in this case. -DaveM */
	sk_for_each_bound(sk2, node, &tb->owners) {
		if (sk != sk2 &&
		    (!sk->sk_bound_dev_if ||
		     !sk2->sk_bound_dev_if ||
		     sk->sk_bound_dev_if == sk2->sk_bound_dev_if) &&
		    (!sk->sk_reuse || !sk2->sk_reuse ||
		     sk2->sk_state == TCP_LISTEN) &&
		     ipv6_rcv_saddr_equal(sk, sk2))
			break;
	}

	return node != NULL;
}

/* Grrr, addr_type already calculated by caller, but I don't want
 * to add some silly "cookie" argument to this method just for that.
 * But it doesn't matter, the recalculation is in the rarest path
 * this function ever takes.
 */
static int tcp_v6_get_port(struct sock *sk, unsigned short snum)
{
	struct tcp_bind_hashbucket *head;
	struct tcp_bind_bucket *tb;
	struct hlist_node *node;
	int ret;

	local_bh_disable();
	if (snum == 0) {
		int low = sysctl_local_port_range[0];
		int high = sysctl_local_port_range[1];
		int remaining = (high - low) + 1;
		int rover;

		spin_lock(&tcp_portalloc_lock);
		rover = tcp_port_rover;
		do {	rover++;
			if ((rover < low) || (rover > high))
				rover = low;
			head = &tcp_bhash[tcp_bhashfn(rover)];
			spin_lock(&head->lock);
			tb_for_each(tb, node, &head->chain)
				if (tb->port == rover)
					goto next;
			break;
		next:
			spin_unlock(&head->lock);
		} while (--remaining > 0);
		tcp_port_rover = rover;
		spin_unlock(&tcp_portalloc_lock);

		/* Exhausted local port range during search? */
		ret = 1;
		if (remaining <= 0)
			goto fail;

		/* OK, here is the one we will use. */
		snum = rover;
	} else {
		head = &tcp_bhash[tcp_bhashfn(snum)];
		spin_lock(&head->lock);
		tb_for_each(tb, node, &head->chain)
			if (tb->port == snum)
				goto tb_found;
	}
	tb = NULL;
	goto tb_not_found;
tb_found:
	if (tb && !hlist_empty(&tb->owners)) {
		if (tb->fastreuse > 0 && sk->sk_reuse &&
		    sk->sk_state != TCP_LISTEN) {
			goto success;
		} else {
			ret = 1;
			if (tcp_v6_bind_conflict(sk, tb))
				goto fail_unlock;
		}
	}
tb_not_found:
	ret = 1;
	if (!tb && (tb = tcp_bucket_create(head, snum)) == NULL)
		goto fail_unlock;
	if (hlist_empty(&tb->owners)) {
		if (sk->sk_reuse && sk->sk_state != TCP_LISTEN)
			tb->fastreuse = 1;
		else
			tb->fastreuse = 0;
	} else if (tb->fastreuse &&
		   (!sk->sk_reuse || sk->sk_state == TCP_LISTEN))
		tb->fastreuse = 0;

success:
	if (!tcp_sk(sk)->bind_hash)
		tcp_bind_hash(sk, tb, snum);
	BUG_TRAP(tcp_sk(sk)->bind_hash == tb);
	ret = 0;

fail_unlock:
	spin_unlock(&head->lock);
fail:
	local_bh_enable();
	return ret;
}

static __inline__ void __tcp_v6_hash(struct sock *sk)
{
	struct hlist_head *list;
	rwlock_t *lock;

	BUG_TRAP(sk_unhashed(sk));

	if (sk->sk_state == TCP_LISTEN) {
		list = &tcp_listening_hash[tcp_sk_listen_hashfn(sk)];
		lock = &tcp_lhash_lock;
		tcp_listen_wlock();
	} else {
		sk->sk_hashent = tcp_v6_sk_hashfn(sk);
		list = &tcp_ehash[sk->sk_hashent].chain;
		lock = &tcp_ehash[sk->sk_hashent].lock;
		write_lock(lock);
	}

	__sk_add_node(sk, list);
	sock_prot_inc_use(sk->sk_prot);
	write_unlock(lock);
}


static void tcp_v6_hash(struct sock *sk)
{
	if (sk->sk_state != TCP_CLOSE) {
		struct tcp_opt *tp = tcp_sk(sk);

		if (tp->af_specific == &ipv6_mapped) {
			tcp_prot.hash(sk);
			return;
		}
		local_bh_disable();
		__tcp_v6_hash(sk);
		local_bh_enable();
	}
}

static struct sock *tcp_v6_lookup_listener(struct in6_addr *daddr, unsigned short hnum, int dif)
{
	struct sock *sk;
	struct hlist_node *node;
	struct sock *result = NULL;
	int score, hiscore;

	hiscore=0;
	read_lock(&tcp_lhash_lock);
	sk_for_each(sk, node, &tcp_listening_hash[tcp_lhashfn(hnum)]) {
		if (inet_sk(sk)->num == hnum && sk->sk_family == PF_INET6) {
			struct ipv6_pinfo *np = inet6_sk(sk);
			
			score = 1;
			if (!ipv6_addr_any(&np->rcv_saddr)) {
				if (ipv6_addr_cmp(&np->rcv_saddr, daddr))
					continue;
				score++;
			}
			if (sk->sk_bound_dev_if) {
				if (sk->sk_bound_dev_if != dif)
					continue;
				score++;
			}
			if (score == 3) {
				result = sk;
				break;
			}
			if (score > hiscore) {
				hiscore = score;
				result = sk;
			}
		}
	}
	if (result)
		sock_hold(result);
	read_unlock(&tcp_lhash_lock);
	return result;
}

/* Sockets in TCP_CLOSE state are _always_ taken out of the hash, so
 * we need not check it for TCP lookups anymore, thanks Alexey. -DaveM
 *
 * The sockhash lock must be held as a reader here.
 */

static inline struct sock *__tcp_v6_lookup_established(struct in6_addr *saddr, u16 sport,
						       struct in6_addr *daddr, u16 hnum,
						       int dif)
{
	struct tcp_ehash_bucket *head;
	struct sock *sk;
	struct hlist_node *node;
	__u32 ports = TCP_COMBINED_PORTS(sport, hnum);
	int hash;

	/* Optimize here for direct hit, only listening connections can
	 * have wildcards anyways.
	 */
	hash = tcp_v6_hashfn(daddr, hnum, saddr, sport);
	head = &tcp_ehash[hash];
	read_lock(&head->lock);
	sk_for_each(sk, node, &head->chain) {
		/* For IPV6 do the cheaper port and family tests first. */
		if(TCP_IPV6_MATCH(sk, saddr, daddr, ports, dif))
			goto hit; /* You sunk my battleship! */
	}
	/* Must check for a TIME_WAIT'er before going to listener hash. */
	sk_for_each(sk, node, &(head + tcp_ehash_size)->chain) {
		/* FIXME: acme: check this... */
		struct tcp_tw_bucket *tw = (struct tcp_tw_bucket *)sk;

		if(*((__u32 *)&(tw->tw_dport))	== ports	&&
		   sk->sk_family		== PF_INET6) {
			if(!ipv6_addr_cmp(&tw->tw_v6_daddr, saddr)	&&
			   !ipv6_addr_cmp(&tw->tw_v6_rcv_saddr, daddr)	&&
			   (!sk->sk_bound_dev_if || sk->sk_bound_dev_if == dif))
				goto hit;
		}
	}
	read_unlock(&head->lock);
	return NULL;

hit:
	sock_hold(sk);
	read_unlock(&head->lock);
	return sk;
}


static inline struct sock *__tcp_v6_lookup(struct in6_addr *saddr, u16 sport,
					   struct in6_addr *daddr, u16 hnum,
					   int dif)
{
	struct sock *sk;

	sk = __tcp_v6_lookup_established(saddr, sport, daddr, hnum, dif);

	if (sk)
		return sk;

	return tcp_v6_lookup_listener(daddr, hnum, dif);
}

inline struct sock *tcp_v6_lookup(struct in6_addr *saddr, u16 sport,
				  struct in6_addr *daddr, u16 dport,
				  int dif)
{
	struct sock *sk;

	local_bh_disable();
	sk = __tcp_v6_lookup(saddr, sport, daddr, ntohs(dport), dif);
	local_bh_enable();

	return sk;
}


/*
 * Open request hash tables.
 */

static u32 tcp_v6_synq_hash(struct in6_addr *raddr, u16 rport, u32 rnd)
{
	u32 a, b, c;

	a = raddr->s6_addr32[0];
	b = raddr->s6_addr32[1];
	c = raddr->s6_addr32[2];

	a += JHASH_GOLDEN_RATIO;
	b += JHASH_GOLDEN_RATIO;
	c += rnd;
	__jhash_mix(a, b, c);

	a += raddr->s6_addr32[3];
	b += (u32) rport;
	__jhash_mix(a, b, c);

	return c & (TCP_SYNQ_HSIZE - 1);
}

static struct open_request *tcp_v6_search_req(struct tcp_opt *tp,
					      struct open_request ***prevp,
					      __u16 rport,
					      struct in6_addr *raddr,
					      struct in6_addr *laddr,
					      int iif)
{
	struct tcp_listen_opt *lopt = tp->listen_opt;
	struct open_request *req, **prev;  

	for (prev = &lopt->syn_table[tcp_v6_synq_hash(raddr, rport, lopt->hash_rnd)];
	     (req = *prev) != NULL;
	     prev = &req->dl_next) {
		if (req->rmt_port == rport &&
		    req->class->family == AF_INET6 &&
		    !ipv6_addr_cmp(&req->af.v6_req.rmt_addr, raddr) &&
		    !ipv6_addr_cmp(&req->af.v6_req.loc_addr, laddr) &&
		    (!req->af.v6_req.iif || req->af.v6_req.iif == iif)) {
			BUG_TRAP(req->sk == NULL);
			*prevp = prev;
			return req;
		}
	}

	return NULL;
}

static __inline__ u16 tcp_v6_check(struct tcphdr *th, int len,
				   struct in6_addr *saddr, 
				   struct in6_addr *daddr, 
				   unsigned long base)
{
	return csum_ipv6_magic(saddr, daddr, len, IPPROTO_TCP, base);
}

static __u32 tcp_v6_init_sequence(struct sock *sk, struct sk_buff *skb)
{
	if (skb->protocol == htons(ETH_P_IPV6)) {
		return secure_tcpv6_sequence_number(skb->nh.ipv6h->daddr.s6_addr32,
						    skb->nh.ipv6h->saddr.s6_addr32,
						    skb->h.th->dest,
						    skb->h.th->source);
	} else {
		return secure_tcp_sequence_number(skb->nh.iph->daddr,
						  skb->nh.iph->saddr,
						  skb->h.th->dest,
						  skb->h.th->source);
	}
}

static int tcp_v6_check_established(struct sock *sk)
{
	struct inet_opt *inet = inet_sk(sk);
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct in6_addr *daddr = &np->rcv_saddr;
	struct in6_addr *saddr = &np->daddr;
	int dif = sk->sk_bound_dev_if;
	u32 ports = TCP_COMBINED_PORTS(inet->dport, inet->num);
	int hash = tcp_v6_hashfn(daddr, inet->num, saddr, inet->dport);
	struct tcp_ehash_bucket *head = &tcp_ehash[hash];
	struct sock *sk2;
	struct hlist_node *node;
	struct tcp_tw_bucket *tw;

	write_lock_bh(&head->lock);

	/* Check TIME-WAIT sockets first. */
	sk_for_each(sk2, node, &(head + tcp_ehash_size)->chain) {
		tw = (struct tcp_tw_bucket*)sk2;

		if(*((__u32 *)&(tw->tw_dport))	== ports	&&
		   sk2->sk_family		== PF_INET6	&&
		   !ipv6_addr_cmp(&tw->tw_v6_daddr, saddr)	&&
		   !ipv6_addr_cmp(&tw->tw_v6_rcv_saddr, daddr)	&&
		   sk2->sk_bound_dev_if == sk->sk_bound_dev_if) {
			struct tcp_opt *tp = tcp_sk(sk);

			if (tw->tw_ts_recent_stamp) {
				/* See comment in tcp_ipv4.c */
				tp->write_seq = tw->tw_snd_nxt + 65535 + 2;
				if (!tp->write_seq)
					tp->write_seq = 1;
				tp->ts_recent = tw->tw_ts_recent;
				tp->ts_recent_stamp = tw->tw_ts_recent_stamp;
				sock_hold(sk2);
				goto unique;
			} else
				goto not_unique;
		}
	}
	tw = NULL;

	/* And established part... */
	sk_for_each(sk2, node, &head->chain) {
		if(TCP_IPV6_MATCH(sk2, saddr, daddr, ports, dif))
			goto not_unique;
	}

unique:
	BUG_TRAP(sk_unhashed(sk));
	__sk_add_node(sk, &head->chain);
	sk->sk_hashent = hash;
	sock_prot_inc_use(sk->sk_prot);
	write_unlock_bh(&head->lock);

	if (tw) {
		/* Silly. Should hash-dance instead... */
		local_bh_disable();
		tcp_tw_deschedule(tw);
		NET_INC_STATS_BH(LINUX_MIB_TIMEWAITRECYCLED);
		local_bh_enable();

		tcp_tw_put(tw);
	}
	return 0;

not_unique:
	write_unlock_bh(&head->lock);
	return -EADDRNOTAVAIL;
}

static int tcp_v6_hash_connect(struct sock *sk)
{
	struct tcp_bind_hashbucket *head;
	struct tcp_bind_bucket *tb;

	/* XXX */
	if (inet_sk(sk)->num == 0) { 
		int err = tcp_v6_get_port(sk, inet_sk(sk)->num);
		if (err)
			return err;
		inet_sk(sk)->sport = htons(inet_sk(sk)->num);
	}

	head = &tcp_bhash[tcp_bhashfn(inet_sk(sk)->num)];
	tb = tb_head(head);

	spin_lock_bh(&head->lock);

	if (sk_head(&tb->owners) == sk && !sk->sk_bind_node.next) {
		__tcp_v6_hash(sk);
		spin_unlock_bh(&head->lock);
		return 0;
	} else {
		spin_unlock_bh(&head->lock);
		return tcp_v6_check_established(sk);
	}
}

static __inline__ int tcp_v6_iif(struct sk_buff *skb)
{
	return IP6CB(skb)->iif;
}

static int tcp_v6_connect(struct sock *sk, struct sockaddr *uaddr, 
			  int addr_len)
{
	struct sockaddr_in6 *usin = (struct sockaddr_in6 *) uaddr;
	struct inet_opt *inet = inet_sk(sk);
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct tcp_opt *tp = tcp_sk(sk);
	struct in6_addr *saddr = NULL;
	struct flowi fl;
	struct dst_entry *dst;
	int addr_type;
	int err;

	if (addr_len < SIN6_LEN_RFC2133) 
		return -EINVAL;

	if (usin->sin6_family != AF_INET6) 
		return(-EAFNOSUPPORT);

	memset(&fl, 0, sizeof(fl));

	if (np->sndflow) {
		fl.fl6_flowlabel = usin->sin6_flowinfo&IPV6_FLOWINFO_MASK;
		IP6_ECN_flow_init(fl.fl6_flowlabel);
		if (fl.fl6_flowlabel&IPV6_FLOWLABEL_MASK) {
			struct ip6_flowlabel *flowlabel;
			flowlabel = fl6_sock_lookup(sk, fl.fl6_flowlabel);
			if (flowlabel == NULL)
				return -EINVAL;
			ipv6_addr_copy(&usin->sin6_addr, &flowlabel->dst);
			fl6_sock_release(flowlabel);
		}
	}

	/*
  	 *	connect() to INADDR_ANY means loopback (BSD'ism).
  	 */
  	
  	if(ipv6_addr_any(&usin->sin6_addr))
		usin->sin6_addr.s6_addr[15] = 0x1; 

	addr_type = ipv6_addr_type(&usin->sin6_addr);

	if(addr_type & IPV6_ADDR_MULTICAST)
		return -ENETUNREACH;

	if (addr_type&IPV6_ADDR_LINKLOCAL) {
		if (addr_len >= sizeof(struct sockaddr_in6) &&
		    usin->sin6_scope_id) {
			/* If interface is set while binding, indices
			 * must coincide.
			 */
			if (sk->sk_bound_dev_if &&
			    sk->sk_bound_dev_if != usin->sin6_scope_id)
				return -EINVAL;

			sk->sk_bound_dev_if = usin->sin6_scope_id;
		}

		/* Connect to link-local address requires an interface */
		if (!sk->sk_bound_dev_if)
			return -EINVAL;
	}

	if (tp->ts_recent_stamp &&
	    ipv6_addr_cmp(&np->daddr, &usin->sin6_addr)) {
		tp->ts_recent = 0;
		tp->ts_recent_stamp = 0;
		tp->write_seq = 0;
	}

	ipv6_addr_copy(&np->daddr, &usin->sin6_addr);
	np->flow_label = fl.fl6_flowlabel;

	/*
	 *	TCP over IPv4
	 */

	if (addr_type == IPV6_ADDR_MAPPED) {
		u32 exthdrlen = tp->ext_header_len;
		struct sockaddr_in sin;

		SOCK_DEBUG(sk, "connect: ipv4 mapped\n");

		if (__ipv6_only_sock(sk))
			return -ENETUNREACH;

		sin.sin_family = AF_INET;
		sin.sin_port = usin->sin6_port;
		sin.sin_addr.s_addr = usin->sin6_addr.s6_addr32[3];

		tp->af_specific = &ipv6_mapped;
		sk->sk_backlog_rcv = tcp_v4_do_rcv;

		err = tcp_v4_connect(sk, (struct sockaddr *)&sin, sizeof(sin));

		if (err) {
			tp->ext_header_len = exthdrlen;
			tp->af_specific = &ipv6_specific;
			sk->sk_backlog_rcv = tcp_v6_do_rcv;
			goto failure;
		} else {
			ipv6_addr_set(&np->saddr, 0, 0, htonl(0x0000FFFF),
				      inet->saddr);
			ipv6_addr_set(&np->rcv_saddr, 0, 0, htonl(0x0000FFFF),
				      inet->rcv_saddr);
		}

		return err;
	}

	if (!ipv6_addr_any(&np->rcv_saddr))
		saddr = &np->rcv_saddr;

	fl.proto = IPPROTO_TCP;
	ipv6_addr_copy(&fl.fl6_dst, &np->daddr);
	ipv6_addr_copy(&fl.fl6_src,
		       (saddr ? saddr : &np->saddr));
	fl.oif = sk->sk_bound_dev_if;
	fl.fl_ip_dport = usin->sin6_port;
	fl.fl_ip_sport = inet->sport;

	if (np->opt && np->opt->srcrt) {
		struct rt0_hdr *rt0 = (struct rt0_hdr *)np->opt->srcrt;
		ipv6_addr_copy(&fl.fl6_dst, rt0->addr);
	}

	err = ip6_dst_lookup(sk, &dst, &fl);

	if (err)
		goto failure;

	if (saddr == NULL) {
		saddr = &fl.fl6_src;
		ipv6_addr_copy(&np->rcv_saddr, saddr);
	}

	/* set the source address */
	ipv6_addr_copy(&np->saddr, saddr);
	inet->rcv_saddr = LOOPBACK4_IPV6;

	ip6_dst_store(sk, dst, NULL);
	sk->sk_route_caps = dst->dev->features &
		~(NETIF_F_IP_CSUM | NETIF_F_TSO);

	tp->ext_header_len = 0;
	if (np->opt)
		tp->ext_header_len = np->opt->opt_flen + np->opt->opt_nflen;
	tp->ext2_header_len = dst->header_len;

	tp->mss_clamp = IPV6_MIN_MTU - sizeof(struct tcphdr) - sizeof(struct ipv6hdr);

	inet->dport = usin->sin6_port;

	tcp_set_state(sk, TCP_SYN_SENT);
	err = tcp_v6_hash_connect(sk);
	if (err)
		goto late_failure;

	if (!tp->write_seq)
		tp->write_seq = secure_tcpv6_sequence_number(np->saddr.s6_addr32,
							     np->daddr.s6_addr32,
							     inet->sport,
							     inet->dport);

	err = tcp_connect(sk);
	if (err)
		goto late_failure;

	return 0;

late_failure:
	tcp_set_state(sk, TCP_CLOSE);
	__sk_dst_reset(sk);
failure:
	inet->dport = 0;
	sk->sk_route_caps = 0;
	return err;
}

static void tcp_v6_err(struct sk_buff *skb, struct inet6_skb_parm *opt,
		int type, int code, int offset, __u32 info)
{
	struct ipv6hdr *hdr = (struct ipv6hdr*)skb->data;
	struct tcphdr *th = (struct tcphdr *)(skb->data+offset);
	struct ipv6_pinfo *np;
	struct sock *sk;
	int err;
	struct tcp_opt *tp; 
	__u32 seq;

	sk = tcp_v6_lookup(&hdr->daddr, th->dest, &hdr->saddr, th->source, skb->dev->ifindex);

	if (sk == NULL) {
		ICMP6_INC_STATS_BH(__in6_dev_get(skb->dev), ICMP6_MIB_INERRORS);
		return;
	}

	if (sk->sk_state == TCP_TIME_WAIT) {
		tcp_tw_put((struct tcp_tw_bucket*)sk);
		return;
	}

	bh_lock_sock(sk);
	if (sock_owned_by_user(sk))
		NET_INC_STATS_BH(LINUX_MIB_LOCKDROPPEDICMPS);

	if (sk->sk_state == TCP_CLOSE)
		goto out;

	tp = tcp_sk(sk);
	seq = ntohl(th->seq); 
	if (sk->sk_state != TCP_LISTEN &&
	    !between(seq, tp->snd_una, tp->snd_nxt)) {
		NET_INC_STATS_BH(LINUX_MIB_OUTOFWINDOWICMPS);
		goto out;
	}

	np = inet6_sk(sk);

	if (type == ICMPV6_PKT_TOOBIG) {
		struct dst_entry *dst = NULL;

		if (sock_owned_by_user(sk))
			goto out;
		if ((1 << sk->sk_state) & (TCPF_LISTEN | TCPF_CLOSE))
			goto out;

		/* icmp should have updated the destination cache entry */
		dst = __sk_dst_check(sk, np->dst_cookie);

		if (dst == NULL) {
			struct inet_opt *inet = inet_sk(sk);
			struct flowi fl;

			/* BUGGG_FUTURE: Again, it is not clear how
			   to handle rthdr case. Ignore this complexity
			   for now.
			 */
			memset(&fl, 0, sizeof(fl));
			fl.proto = IPPROTO_TCP;
			ipv6_addr_copy(&fl.fl6_dst, &np->daddr);
			ipv6_addr_copy(&fl.fl6_src, &np->saddr);
			fl.oif = sk->sk_bound_dev_if;
			fl.fl_ip_dport = inet->dport;
			fl.fl_ip_sport = inet->sport;

			if ((err = ip6_dst_lookup(sk, &dst, &fl))) {
				sk->sk_err_soft = -err;
				goto out;
			}
		} else
			dst_hold(dst);

		if (tp->pmtu_cookie > dst_pmtu(dst)) {
			tcp_sync_mss(sk, dst_pmtu(dst));
			tcp_simple_retransmit(sk);
		} /* else let the usual retransmit timer handle it */
		dst_release(dst);
		goto out;
	}

	icmpv6_err_convert(type, code, &err);

	/* Might be for an open_request */
	switch (sk->sk_state) {
		struct open_request *req, **prev;
	case TCP_LISTEN:
		if (sock_owned_by_user(sk))
			goto out;

		req = tcp_v6_search_req(tp, &prev, th->dest, &hdr->daddr,
					&hdr->saddr, tcp_v6_iif(skb));
		if (!req)
			goto out;

		/* ICMPs are not backlogged, hence we cannot get
		 * an established socket here.
		 */
		BUG_TRAP(req->sk == NULL);

		if (seq != req->snt_isn) {
			NET_INC_STATS_BH(LINUX_MIB_OUTOFWINDOWICMPS);
			goto out;
		}

		tcp_synq_drop(sk, req, prev);
		goto out;

	case TCP_SYN_SENT:
	case TCP_SYN_RECV:  /* Cannot happen.
			       It can, it SYNs are crossed. --ANK */ 
		if (!sock_owned_by_user(sk)) {
			TCP_INC_STATS_BH(TCP_MIB_ATTEMPTFAILS);
			sk->sk_err = err;
			sk->sk_error_report(sk);		/* Wake people up to see the error (see connect in sock.c) */

			tcp_done(sk);
		} else
			sk->sk_err_soft = err;
		goto out;
	}

	if (!sock_owned_by_user(sk) && np->recverr) {
		sk->sk_err = err;
		sk->sk_error_report(sk);
	} else
		sk->sk_err_soft = err;

out:
	bh_unlock_sock(sk);
	sock_put(sk);
}


static int tcp_v6_send_synack(struct sock *sk, struct open_request *req,
			      struct dst_entry *dst)
{
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct sk_buff * skb;
	struct ipv6_txoptions *opt = NULL;
	struct flowi fl;
	int err = -1;

	memset(&fl, 0, sizeof(fl));
	fl.proto = IPPROTO_TCP;
	ipv6_addr_copy(&fl.fl6_dst, &req->af.v6_req.rmt_addr);
	ipv6_addr_copy(&fl.fl6_src, &req->af.v6_req.loc_addr);
	fl.fl6_flowlabel = 0;
	fl.oif = req->af.v6_req.iif;
	fl.fl_ip_dport = req->rmt_port;
	fl.fl_ip_sport = inet_sk(sk)->sport;

	if (dst == NULL) {
		opt = np->opt;
		if (opt == NULL &&
		    np->rxopt.bits.srcrt == 2 &&
		    req->af.v6_req.pktopts) {
			struct sk_buff *pktopts = req->af.v6_req.pktopts;
			struct inet6_skb_parm *rxopt = IP6CB(pktopts);
			if (rxopt->srcrt)
				opt = ipv6_invert_rthdr(sk, (struct ipv6_rt_hdr*)(pktopts->nh.raw + rxopt->srcrt));
		}

		if (opt && opt->srcrt) {
			struct rt0_hdr *rt0 = (struct rt0_hdr *) opt->srcrt;
			ipv6_addr_copy(&fl.fl6_dst, rt0->addr);
		}

		err = ip6_dst_lookup(sk, &dst, &fl);
		if (err)
			goto done;
	}

	skb = tcp_make_synack(sk, dst, req);
	if (skb) {
		struct tcphdr *th = skb->h.th;

		th->check = tcp_v6_check(th, skb->len,
					 &req->af.v6_req.loc_addr, &req->af.v6_req.rmt_addr,
					 csum_partial((char *)th, skb->len, skb->csum));

		ipv6_addr_copy(&fl.fl6_dst, &req->af.v6_req.rmt_addr);
		err = ip6_xmit(sk, skb, &fl, opt, 0);
		if (err == NET_XMIT_CN)
			err = 0;
	}

done:
	dst_release(dst);
        if (opt && opt != np->opt)
		sock_kfree_s(sk, opt, opt->tot_len);
	return err;
}

static void tcp_v6_or_free(struct open_request *req)
{
	if (req->af.v6_req.pktopts)
		kfree_skb(req->af.v6_req.pktopts);
}

static struct or_calltable or_ipv6 = {
	.family		=	AF_INET6,
	.rtx_syn_ack	=	tcp_v6_send_synack,
	.send_ack	=	tcp_v6_or_send_ack,
	.destructor	=	tcp_v6_or_free,
	.send_reset	=	tcp_v6_send_reset
};

static int ipv6_opt_accepted(struct sock *sk, struct sk_buff *skb)
{
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct inet6_skb_parm *opt = IP6CB(skb);

	if (np->rxopt.all) {
		if ((opt->hop && np->rxopt.bits.hopopts) ||
		    ((IPV6_FLOWINFO_MASK&*(u32*)skb->nh.raw) &&
		     np->rxopt.bits.rxflow) ||
		    (opt->srcrt && np->rxopt.bits.srcrt) ||
		    ((opt->dst1 || opt->dst0) && np->rxopt.bits.dstopts))
			return 1;
	}
	return 0;
}


static void tcp_v6_send_check(struct sock *sk, struct tcphdr *th, int len, 
			      struct sk_buff *skb)
{
	struct ipv6_pinfo *np = inet6_sk(sk);

	if (skb->ip_summed == CHECKSUM_HW) {
		th->check = ~csum_ipv6_magic(&np->saddr, &np->daddr, len, IPPROTO_TCP,  0);
		skb->csum = offsetof(struct tcphdr, check);
	} else {
		th->check = csum_ipv6_magic(&np->saddr, &np->daddr, len, IPPROTO_TCP, 
					    csum_partial((char *)th, th->doff<<2, 
							 skb->csum));
	}
}


static void tcp_v6_send_reset(struct sk_buff *skb)
{
	struct tcphdr *th = skb->h.th, *t1; 
	struct sk_buff *buff;
	struct flowi fl;

	if (th->rst)
		return;

	if (!ipv6_unicast_destination(skb))
		return; 

	/*
	 * We need to grab some memory, and put together an RST,
	 * and then put it into the queue to be sent.
	 */

	buff = alloc_skb(MAX_HEADER + sizeof(struct ipv6hdr), GFP_ATOMIC);
	if (buff == NULL) 
	  	return;

	skb_reserve(buff, MAX_HEADER + sizeof(struct ipv6hdr));

	t1 = (struct tcphdr *) skb_push(buff,sizeof(struct tcphdr));

	/* Swap the send and the receive. */
	memset(t1, 0, sizeof(*t1));
	t1->dest = th->source;
	t1->source = th->dest;
	t1->doff = sizeof(*t1)/4;
	t1->rst = 1;
  
	if(th->ack) {
	  	t1->seq = th->ack_seq;
	} else {
		t1->ack = 1;
		t1->ack_seq = htonl(ntohl(th->seq) + th->syn + th->fin
				    + skb->len - (th->doff<<2));
	}

	buff->csum = csum_partial((char *)t1, sizeof(*t1), 0);

	memset(&fl, 0, sizeof(fl));
	ipv6_addr_copy(&fl.fl6_dst, &skb->nh.ipv6h->saddr);
	ipv6_addr_copy(&fl.fl6_src, &skb->nh.ipv6h->daddr);

	t1->check = csum_ipv6_magic(&fl.fl6_src, &fl.fl6_dst,
				    sizeof(*t1), IPPROTO_TCP,
				    buff->csum);

	fl.proto = IPPROTO_TCP;
	fl.oif = tcp_v6_iif(skb);
	fl.fl_ip_dport = t1->dest;
	fl.fl_ip_sport = t1->source;

	/* sk = NULL, but it is safe for now. RST socket required. */
	if (!ip6_dst_lookup(NULL, &buff->dst, &fl)) {
		ip6_xmit(NULL, buff, &fl, NULL, 0);
		TCP_INC_STATS_BH(TCP_MIB_OUTSEGS);
		TCP_INC_STATS_BH(TCP_MIB_OUTRSTS);
		return;
	}

	kfree_skb(buff);
}

static void tcp_v6_send_ack(struct sk_buff *skb, u32 seq, u32 ack, u32 win, u32 ts)
{
	struct tcphdr *th = skb->h.th, *t1;
	struct sk_buff *buff;
	struct flowi fl;
	int tot_len = sizeof(struct tcphdr);

	buff = alloc_skb(MAX_HEADER + sizeof(struct ipv6hdr), GFP_ATOMIC);
	if (buff == NULL)
		return;

	skb_reserve(buff, MAX_HEADER + sizeof(struct ipv6hdr));

	if (ts)
		tot_len += 3*4;

	t1 = (struct tcphdr *) skb_push(buff,tot_len);

	/* Swap the send and the receive. */
	memset(t1, 0, sizeof(*t1));
	t1->dest = th->source;
	t1->source = th->dest;
	t1->doff = tot_len/4;
	t1->seq = htonl(seq);
	t1->ack_seq = htonl(ack);
	t1->ack = 1;
	t1->window = htons(win);
	
	if (ts) {
		u32 *ptr = (u32*)(t1 + 1);
		*ptr++ = htonl((TCPOPT_NOP << 24) | (TCPOPT_NOP << 16) |
			       (TCPOPT_TIMESTAMP << 8) | TCPOLEN_TIMESTAMP);
		*ptr++ = htonl(tcp_time_stamp);
		*ptr = htonl(ts);
	}

	buff->csum = csum_partial((char *)t1, tot_len, 0);

	memset(&fl, 0, sizeof(fl));
	ipv6_addr_copy(&fl.fl6_dst, &skb->nh.ipv6h->saddr);
	ipv6_addr_copy(&fl.fl6_src, &skb->nh.ipv6h->daddr);

	t1->check = csum_ipv6_magic(&fl.fl6_src, &fl.fl6_dst,
				    tot_len, IPPROTO_TCP,
				    buff->csum);

	fl.proto = IPPROTO_TCP;
	fl.oif = tcp_v6_iif(skb);
	fl.fl_ip_dport = t1->dest;
	fl.fl_ip_sport = t1->source;

	if (!ip6_dst_lookup(NULL, &buff->dst, &fl)) {
		ip6_xmit(NULL, buff, &fl, NULL, 0);
		TCP_INC_STATS_BH(TCP_MIB_OUTSEGS);
		return;
	}

	kfree_skb(buff);
}

static void tcp_v6_timewait_ack(struct sock *sk, struct sk_buff *skb)
{
	struct tcp_tw_bucket *tw = (struct tcp_tw_bucket *)sk;

	tcp_v6_send_ack(skb, tw->tw_snd_nxt, tw->tw_rcv_nxt,
			tw->tw_rcv_wnd >> tw->tw_rcv_wscale, tw->tw_ts_recent);

	tcp_tw_put(tw);
}

static void tcp_v6_or_send_ack(struct sk_buff *skb, struct open_request *req)
{
	tcp_v6_send_ack(skb, req->snt_isn+1, req->rcv_isn+1, req->rcv_wnd, req->ts_recent);
}


static struct sock *tcp_v6_hnd_req(struct sock *sk,struct sk_buff *skb)
{
	struct open_request *req, **prev;
	struct tcphdr *th = skb->h.th;
	struct tcp_opt *tp = tcp_sk(sk);
	struct sock *nsk;

	/* Find possible connection requests. */
	req = tcp_v6_search_req(tp, &prev, th->source, &skb->nh.ipv6h->saddr,
				&skb->nh.ipv6h->daddr, tcp_v6_iif(skb));
	if (req)
		return tcp_check_req(sk, skb, req, prev);

	nsk = __tcp_v6_lookup_established(&skb->nh.ipv6h->saddr,
					  th->source,
					  &skb->nh.ipv6h->daddr,
					  ntohs(th->dest),
					  tcp_v6_iif(skb));

	if (nsk) {
		if (nsk->sk_state != TCP_TIME_WAIT) {
			bh_lock_sock(nsk);
			return nsk;
		}
		tcp_tw_put((struct tcp_tw_bucket*)nsk);
		return NULL;
	}

#if 0 /*def CONFIG_SYN_COOKIES*/
	if (!th->rst && !th->syn && th->ack)
		sk = cookie_v6_check(sk, skb, &(IPCB(skb)->opt));
#endif
	return sk;
}

static void tcp_v6_synq_add(struct sock *sk, struct open_request *req)
{
	struct tcp_opt *tp = tcp_sk(sk);
	struct tcp_listen_opt *lopt = tp->listen_opt;
	u32 h = tcp_v6_synq_hash(&req->af.v6_req.rmt_addr, req->rmt_port, lopt->hash_rnd);

	req->sk = NULL;
	req->expires = jiffies + TCP_TIMEOUT_INIT;
	req->retrans = 0;
	req->dl_next = lopt->syn_table[h];

	write_lock(&tp->syn_wait_lock);
	lopt->syn_table[h] = req;
	write_unlock(&tp->syn_wait_lock);

	tcp_synq_added(sk);
}


/* FIXME: this is substantially similar to the ipv4 code.
 * Can some kind of merge be done? -- erics
 */
static int tcp_v6_conn_request(struct sock *sk, struct sk_buff *skb)
{
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct tcp_opt tmptp, *tp = tcp_sk(sk);
	struct open_request *req = NULL;
	__u32 isn = TCP_SKB_CB(skb)->when;

	if (skb->protocol == htons(ETH_P_IP))
		return tcp_v4_conn_request(sk, skb);

	if (!ipv6_unicast_destination(skb))
		goto drop; 

	/*
	 *	There are no SYN attacks on IPv6, yet...	
	 */
	if (tcp_synq_is_full(sk) && !isn) {
		if (net_ratelimit())
			printk(KERN_INFO "TCPv6: dropping request, synflood is possible\n");
		goto drop;		
	}

	if (sk_acceptq_is_full(sk) && tcp_synq_young(sk) > 1)
		goto drop;

	req = tcp_openreq_alloc();
	if (req == NULL)
		goto drop;

	tcp_clear_options(&tmptp);
	tmptp.mss_clamp = IPV6_MIN_MTU - sizeof(struct tcphdr) - sizeof(struct ipv6hdr);
	tmptp.user_mss = tp->user_mss;

	tcp_parse_options(skb, &tmptp, 0);

	tmptp.tstamp_ok = tmptp.saw_tstamp;
	tcp_openreq_init(req, &tmptp, skb);

	req->class = &or_ipv6;
	ipv6_addr_copy(&req->af.v6_req.rmt_addr, &skb->nh.ipv6h->saddr);
	ipv6_addr_copy(&req->af.v6_req.loc_addr, &skb->nh.ipv6h->daddr);
	TCP_ECN_create_request(req, skb->h.th);
	req->af.v6_req.pktopts = NULL;
	if (ipv6_opt_accepted(sk, skb) ||
	    np->rxopt.bits.rxinfo ||
	    np->rxopt.bits.rxhlim) {
		atomic_inc(&skb->users);
		req->af.v6_req.pktopts = skb;
	}
	req->af.v6_req.iif = sk->sk_bound_dev_if;

	/* So that link locals have meaning */
	if (!sk->sk_bound_dev_if &&
	    ipv6_addr_type(&req->af.v6_req.rmt_addr) & IPV6_ADDR_LINKLOCAL)
		req->af.v6_req.iif = tcp_v6_iif(skb);

	if (isn == 0) 
		isn = tcp_v6_init_sequence(sk,skb);

	req->snt_isn = isn;

	if (tcp_v6_send_synack(sk, req, NULL))
		goto drop;

	tcp_v6_synq_add(sk, req);

	return 0;

drop:
	if (req)
		tcp_openreq_free(req);

	TCP_INC_STATS_BH(TCP_MIB_ATTEMPTFAILS);
	return 0; /* don't send reset */
}

static struct sock * tcp_v6_syn_recv_sock(struct sock *sk, struct sk_buff *skb,
					  struct open_request *req,
					  struct dst_entry *dst)
{
	struct ipv6_pinfo *newnp, *np = inet6_sk(sk);
	struct tcp6_sock *newtcp6sk;
	struct inet_opt *newinet;
	struct tcp_opt *newtp;
	struct sock *newsk;
	struct ipv6_txoptions *opt;

	if (skb->protocol == htons(ETH_P_IP)) {
		/*
		 *	v6 mapped
		 */

		newsk = tcp_v4_syn_recv_sock(sk, skb, req, dst);

		if (newsk == NULL) 
			return NULL;

		newtcp6sk = (struct tcp6_sock *)newsk;
		newtcp6sk->pinet6 = &newtcp6sk->inet6;

		newinet = inet_sk(newsk);
		newnp = inet6_sk(newsk);
		newtp = tcp_sk(newsk);

		memcpy(newnp, np, sizeof(struct ipv6_pinfo));

		ipv6_addr_set(&newnp->daddr, 0, 0, htonl(0x0000FFFF),
			      newinet->daddr);

		ipv6_addr_set(&newnp->saddr, 0, 0, htonl(0x0000FFFF),
			      newinet->saddr);

		ipv6_addr_copy(&newnp->rcv_saddr, &newnp->saddr);

		newtp->af_specific = &ipv6_mapped;
		newsk->sk_backlog_rcv = tcp_v4_do_rcv;
		newnp->pktoptions  = NULL;
		newnp->opt	   = NULL;
		newnp->mcast_oif   = tcp_v6_iif(skb);
		newnp->mcast_hops  = skb->nh.ipv6h->hop_limit;

		/* Charge newly allocated IPv6 socket. Though it is mapped,
		 * it is IPv6 yet.
		 */
#ifdef INET_REFCNT_DEBUG
		atomic_inc(&inet6_sock_nr);
#endif

		/* It is tricky place. Until this moment IPv4 tcp
		   worked with IPv6 af_tcp.af_specific.
		   Sync it now.
		 */
		tcp_sync_mss(newsk, newtp->pmtu_cookie);

		return newsk;
	}

	opt = np->opt;

	if (sk_acceptq_is_full(sk))
		goto out_overflow;

	if (np->rxopt.bits.srcrt == 2 &&
	    opt == NULL && req->af.v6_req.pktopts) {
		struct inet6_skb_parm *rxopt = IP6CB(req->af.v6_req.pktopts);
		if (rxopt->srcrt)
			opt = ipv6_invert_rthdr(sk, (struct ipv6_rt_hdr*)(req->af.v6_req.pktopts->nh.raw+rxopt->srcrt));
	}

	if (dst == NULL) {
		struct flowi fl;

		memset(&fl, 0, sizeof(fl));
		fl.proto = IPPROTO_TCP;
		ipv6_addr_copy(&fl.fl6_dst, &req->af.v6_req.rmt_addr);
		if (opt && opt->srcrt) {
			struct rt0_hdr *rt0 = (struct rt0_hdr *) opt->srcrt;
			ipv6_addr_copy(&fl.fl6_dst, rt0->addr);
		}
		ipv6_addr_copy(&fl.fl6_src, &req->af.v6_req.loc_addr);
		fl.oif = sk->sk_bound_dev_if;
		fl.fl_ip_dport = req->rmt_port;
		fl.fl_ip_sport = inet_sk(sk)->sport;

		if (ip6_dst_lookup(sk, &dst, &fl))
			goto out;
	} 

	newsk = tcp_create_openreq_child(sk, req, skb);
	if (newsk == NULL)
		goto out;

	/* Charge newly allocated IPv6 socket */
#ifdef INET_REFCNT_DEBUG
	atomic_inc(&inet6_sock_nr);
#endif

	ip6_dst_store(newsk, dst, NULL);
	newsk->sk_route_caps = dst->dev->features &
		~(NETIF_F_IP_CSUM | NETIF_F_TSO);

	newtcp6sk = (struct tcp6_sock *)newsk;
	newtcp6sk->pinet6 = &newtcp6sk->inet6;

	newtp = tcp_sk(newsk);
	newinet = inet_sk(newsk);
	newnp = inet6_sk(newsk);

	memcpy(newnp, np, sizeof(struct ipv6_pinfo));

	ipv6_addr_copy(&newnp->daddr, &req->af.v6_req.rmt_addr);
	ipv6_addr_copy(&newnp->saddr, &req->af.v6_req.loc_addr);
	ipv6_addr_copy(&newnp->rcv_saddr, &req->af.v6_req.loc_addr);
	newsk->sk_bound_dev_if = req->af.v6_req.iif;

	/* Now IPv6 options... 

	   First: no IPv4 options.
	 */
	newinet->opt = NULL;

	/* Clone RX bits */
	newnp->rxopt.all = np->rxopt.all;

	/* Clone pktoptions received with SYN */
	newnp->pktoptions = NULL;
	if (req->af.v6_req.pktopts) {
		newnp->pktoptions = skb_clone(req->af.v6_req.pktopts,
					      GFP_ATOMIC);
		kfree_skb(req->af.v6_req.pktopts);
		req->af.v6_req.pktopts = NULL;
		if (newnp->pktoptions)
			skb_set_owner_r(newnp->pktoptions, newsk);
	}
	newnp->opt	  = NULL;
	newnp->mcast_oif  = tcp_v6_iif(skb);
	newnp->mcast_hops = skb->nh.ipv6h->hop_limit;

	/* Clone native IPv6 options from listening socket (if any)

	   Yes, keeping reference count would be much more clever,
	   but we make one more one thing there: reattach optmem
	   to newsk.
	 */
	if (opt) {
		newnp->opt = ipv6_dup_options(newsk, opt);
		if (opt != np->opt)
			sock_kfree_s(sk, opt, opt->tot_len);
	}

	newtp->ext_header_len = 0;
	if (newnp->opt)
		newtp->ext_header_len = newnp->opt->opt_nflen +
					newnp->opt->opt_flen;
	newtp->ext2_header_len = dst->header_len;

	tcp_sync_mss(newsk, dst_pmtu(dst));
	newtp->advmss = dst_metric(dst, RTAX_ADVMSS);
	tcp_initialize_rcv_mss(newsk);

	newinet->daddr = newinet->saddr = newinet->rcv_saddr = LOOPBACK4_IPV6;

	__tcp_v6_hash(newsk);
	tcp_inherit_port(sk, newsk);

	return newsk;

out_overflow:
	NET_INC_STATS_BH(LINUX_MIB_LISTENOVERFLOWS);
out:
	NET_INC_STATS_BH(LINUX_MIB_LISTENDROPS);
	if (opt && opt != np->opt)
		sock_kfree_s(sk, opt, opt->tot_len);
	dst_release(dst);
	return NULL;
}

static int tcp_v6_checksum_init(struct sk_buff *skb)
{
	if (skb->ip_summed == CHECKSUM_HW) {
		skb->ip_summed = CHECKSUM_UNNECESSARY;
		if (!tcp_v6_check(skb->h.th,skb->len,&skb->nh.ipv6h->saddr,
				  &skb->nh.ipv6h->daddr,skb->csum))
			return 0;
		LIMIT_NETDEBUG(printk(KERN_DEBUG "hw tcp v6 csum failed\n"));
	}
	if (skb->len <= 76) {
		if (tcp_v6_check(skb->h.th,skb->len,&skb->nh.ipv6h->saddr,
				 &skb->nh.ipv6h->daddr,skb_checksum(skb, 0, skb->len, 0)))
			return -1;
		skb->ip_summed = CHECKSUM_UNNECESSARY;
	} else {
		skb->csum = ~tcp_v6_check(skb->h.th,skb->len,&skb->nh.ipv6h->saddr,
					  &skb->nh.ipv6h->daddr,0);
	}
	return 0;
}

/* The socket must have it's spinlock held when we get
 * here.
 *
 * We have a potential double-lock case here, so even when
 * doing backlog processing we use the BH locking scheme.
 * This is because we cannot sleep with the original spinlock
 * held.
 */
static int tcp_v6_do_rcv(struct sock *sk, struct sk_buff *skb)
{
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct tcp_opt *tp;
	struct sk_buff *opt_skb = NULL;

	/* Imagine: socket is IPv6. IPv4 packet arrives,
	   goes to IPv4 receive handler and backlogged.
	   From backlog it always goes here. Kerboom...
	   Fortunately, tcp_rcv_established and rcv_established
	   handle them correctly, but it is not case with
	   tcp_v6_hnd_req and tcp_v6_send_reset().   --ANK
	 */

	if (skb->protocol == htons(ETH_P_IP))
		return tcp_v4_do_rcv(sk, skb);

	if (sk_filter(sk, skb, 0))
		goto discard;

	/*
	 *	socket locking is here for SMP purposes as backlog rcv
	 *	is currently called with bh processing disabled.
	 */

	/* Do Stevens' IPV6_PKTOPTIONS.

	   Yes, guys, it is the only place in our code, where we
	   may make it not affecting IPv4.
	   The rest of code is protocol independent,
	   and I do not like idea to uglify IPv4.

	   Actually, all the idea behind IPV6_PKTOPTIONS
	   looks not very well thought. For now we latch
	   options, received in the last packet, enqueued
	   by tcp. Feel free to propose better solution.
	                                       --ANK (980728)
	 */
	if (np->rxopt.all)
		opt_skb = skb_clone(skb, GFP_ATOMIC);

	if (sk->sk_state == TCP_ESTABLISHED) { /* Fast path */
		TCP_CHECK_TIMER(sk);
		if (tcp_rcv_established(sk, skb, skb->h.th, skb->len))
			goto reset;
		TCP_CHECK_TIMER(sk);
		if (opt_skb)
			goto ipv6_pktoptions;
		return 0;
	}

	if (skb->len < (skb->h.th->doff<<2) || tcp_checksum_complete(skb))
		goto csum_err;

	if (sk->sk_state == TCP_LISTEN) { 
		struct sock *nsk = tcp_v6_hnd_req(sk, skb);
		if (!nsk)
			goto discard;

		/*
		 * Queue it on the new socket if the new socket is active,
		 * otherwise we just shortcircuit this and continue with
		 * the new socket..
		 */
 		if(nsk != sk) {
			if (tcp_child_process(sk, nsk, skb))
				goto reset;
			if (opt_skb)
				__kfree_skb(opt_skb);
			return 0;
		}
	}

	TCP_CHECK_TIMER(sk);
	if (tcp_rcv_state_process(sk, skb, skb->h.th, skb->len))
		goto reset;
	TCP_CHECK_TIMER(sk);
	if (opt_skb)
		goto ipv6_pktoptions;
	return 0;

reset:
	tcp_v6_send_reset(skb);
discard:
	if (opt_skb)
		__kfree_skb(opt_skb);
	kfree_skb(skb);
	return 0;
csum_err:
	TCP_INC_STATS_BH(TCP_MIB_INERRS);
	goto discard;


ipv6_pktoptions:
	/* Do you ask, what is it?

	   1. skb was enqueued by tcp.
	   2. skb is added to tail of read queue, rather than out of order.
	   3. socket is not in passive state.
	   4. Finally, it really contains options, which user wants to receive.
	 */
	tp = tcp_sk(sk);
	if (TCP_SKB_CB(opt_skb)->end_seq == tp->rcv_nxt &&
	    !((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_LISTEN))) {
		if (np->rxopt.bits.rxinfo)
			np->mcast_oif = tcp_v6_iif(opt_skb);
		if (np->rxopt.bits.rxhlim)
			np->mcast_hops = opt_skb->nh.ipv6h->hop_limit;
		if (ipv6_opt_accepted(sk, opt_skb)) {
			skb_set_owner_r(opt_skb, sk);
			opt_skb = xchg(&np->pktoptions, opt_skb);
		} else {
			__kfree_skb(opt_skb);
			opt_skb = xchg(&np->pktoptions, NULL);
		}
	}

	if (opt_skb)
		kfree_skb(opt_skb);
	return 0;
}

static int tcp_v6_rcv(struct sk_buff **pskb, unsigned int *nhoffp)
{
	struct sk_buff *skb = *pskb;
	struct tcphdr *th;	
	struct sock *sk;
	int ret;

	if (skb->pkt_type != PACKET_HOST)
		goto discard_it;

	/*
	 *	Count it even if it's bad.
	 */
	TCP_INC_STATS_BH(TCP_MIB_INSEGS);

	if (!pskb_may_pull(skb, sizeof(struct tcphdr)))
		goto discard_it;

	th = skb->h.th;

	if (th->doff < sizeof(struct tcphdr)/4)
		goto bad_packet;
	if (!pskb_may_pull(skb, th->doff*4))
		goto discard_it;

	if ((skb->ip_summed != CHECKSUM_UNNECESSARY &&
	     tcp_v6_checksum_init(skb) < 0))
		goto bad_packet;

	th = skb->h.th;
	TCP_SKB_CB(skb)->seq = ntohl(th->seq);
	TCP_SKB_CB(skb)->end_seq = (TCP_SKB_CB(skb)->seq + th->syn + th->fin +
				    skb->len - th->doff*4);
	TCP_SKB_CB(skb)->ack_seq = ntohl(th->ack_seq);
	TCP_SKB_CB(skb)->when = 0;
	TCP_SKB_CB(skb)->flags = ip6_get_dsfield(skb->nh.ipv6h);
	TCP_SKB_CB(skb)->sacked = 0;

	sk = __tcp_v6_lookup(&skb->nh.ipv6h->saddr, th->source,
			     &skb->nh.ipv6h->daddr, ntohs(th->dest), tcp_v6_iif(skb));

	if (!sk)
		goto no_tcp_socket;

process:
	if (sk->sk_state == TCP_TIME_WAIT)
		goto do_time_wait;

	if (!xfrm6_policy_check(sk, XFRM_POLICY_IN, skb))
		goto discard_and_relse;

	if (sk_filter(sk, skb, 0))
		goto discard_and_relse;

	skb->dev = NULL;

	bh_lock_sock(sk);
	ret = 0;
	if (!sock_owned_by_user(sk)) {
		if (!tcp_prequeue(sk, skb))
			ret = tcp_v6_do_rcv(sk, skb);
	} else
		sk_add_backlog(sk, skb);
	bh_unlock_sock(sk);

	sock_put(sk);
	return ret ? -1 : 0;

no_tcp_socket:
	if (!xfrm6_policy_check(NULL, XFRM_POLICY_IN, skb))
		goto discard_it;

	if (skb->len < (th->doff<<2) || tcp_checksum_complete(skb)) {
bad_packet:
		TCP_INC_STATS_BH(TCP_MIB_INERRS);
	} else {
		tcp_v6_send_reset(skb);
	}

discard_it:

	/*
	 *	Discard frame
	 */

	kfree_skb(skb);
	return 0;

discard_and_relse:
	sock_put(sk);
	goto discard_it;

do_time_wait:
	if (!xfrm6_policy_check(NULL, XFRM_POLICY_IN, skb)) {
		tcp_tw_put((struct tcp_tw_bucket *) sk);
		goto discard_it;
	}

	if (skb->len < (th->doff<<2) || tcp_checksum_complete(skb)) {
		TCP_INC_STATS_BH(TCP_MIB_INERRS);
		tcp_tw_put((struct tcp_tw_bucket *) sk);
		goto discard_it;
	}

	switch(tcp_timewait_state_process((struct tcp_tw_bucket *)sk,
					  skb, th, skb->len)) {
	case TCP_TW_SYN:
	{
		struct sock *sk2;

		sk2 = tcp_v6_lookup_listener(&skb->nh.ipv6h->daddr, ntohs(th->dest), tcp_v6_iif(skb));
		if (sk2 != NULL) {
			tcp_tw_deschedule((struct tcp_tw_bucket *)sk);
			tcp_tw_put((struct tcp_tw_bucket *)sk);
			sk = sk2;
			goto process;
		}
		/* Fall through to ACK */
	}
	case TCP_TW_ACK:
		tcp_v6_timewait_ack(sk, skb);
		break;
	case TCP_TW_RST:
		goto no_tcp_socket;
	case TCP_TW_SUCCESS:;
	}
	goto discard_it;
}

static int tcp_v6_rebuild_header(struct sock *sk)
{
	int err;
	struct dst_entry *dst;
	struct ipv6_pinfo *np = inet6_sk(sk);

	dst = __sk_dst_check(sk, np->dst_cookie);

	if (dst == NULL) {
		struct inet_opt *inet = inet_sk(sk);
		struct flowi fl;

		memset(&fl, 0, sizeof(fl));
		fl.proto = IPPROTO_TCP;
		ipv6_addr_copy(&fl.fl6_dst, &np->daddr);
		ipv6_addr_copy(&fl.fl6_src, &np->saddr);
		fl.fl6_flowlabel = np->flow_label;
		fl.oif = sk->sk_bound_dev_if;
		fl.fl_ip_dport = inet->dport;
		fl.fl_ip_sport = inet->sport;

		if (np->opt && np->opt->srcrt) {
			struct rt0_hdr *rt0 = (struct rt0_hdr *) np->opt->srcrt;
			ipv6_addr_copy(&fl.fl6_dst, rt0->addr);
		}

		err = ip6_dst_lookup(sk, &dst, &fl);

		if (err) {
			sk->sk_route_caps = 0;
			return err;
		}

		ip6_dst_store(sk, dst, NULL);
		sk->sk_route_caps = dst->dev->features &
			~(NETIF_F_IP_CSUM | NETIF_F_TSO);
		tcp_sk(sk)->ext2_header_len = dst->header_len;
	}

	return 0;
}

static int tcp_v6_xmit(struct sk_buff *skb, int ipfragok)
{
	struct sock *sk = skb->sk;
	struct inet_opt *inet = inet_sk(sk);
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct flowi fl;
	struct dst_entry *dst;

	memset(&fl, 0, sizeof(fl));
	fl.proto = IPPROTO_TCP;
	ipv6_addr_copy(&fl.fl6_dst, &np->daddr);
	ipv6_addr_copy(&fl.fl6_src, &np->saddr);
	fl.fl6_flowlabel = np->flow_label;
	IP6_ECN_flow_xmit(sk, fl.fl6_flowlabel);
	fl.oif = sk->sk_bound_dev_if;
	fl.fl_ip_sport = inet->sport;
	fl.fl_ip_dport = inet->dport;

	if (np->opt && np->opt->srcrt) {
		struct rt0_hdr *rt0 = (struct rt0_hdr *) np->opt->srcrt;
		ipv6_addr_copy(&fl.fl6_dst, rt0->addr);
	}

	dst = __sk_dst_check(sk, np->dst_cookie);

	if (dst == NULL) {
		int err = ip6_dst_lookup(sk, &dst, &fl);

		if (err) {
			sk->sk_err_soft = -err;
			return err;
		}

		ip6_dst_store(sk, dst, NULL);
		sk->sk_route_caps = dst->dev->features &
			~(NETIF_F_IP_CSUM | NETIF_F_TSO);
		tcp_sk(sk)->ext2_header_len = dst->header_len;
	}

	skb->dst = dst_clone(dst);

	/* Restore final destination back after routing done */
	ipv6_addr_copy(&fl.fl6_dst, &np->daddr);

	return ip6_xmit(sk, skb, &fl, np->opt, 0);
}

static void v6_addr2sockaddr(struct sock *sk, struct sockaddr * uaddr)
{
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) uaddr;

	sin6->sin6_family = AF_INET6;
	ipv6_addr_copy(&sin6->sin6_addr, &np->daddr);
	sin6->sin6_port	= inet_sk(sk)->dport;
	/* We do not store received flowlabel for TCP */
	sin6->sin6_flowinfo = 0;
	sin6->sin6_scope_id = 0;
	if (sk->sk_bound_dev_if &&
	    ipv6_addr_type(&sin6->sin6_addr) & IPV6_ADDR_LINKLOCAL)
		sin6->sin6_scope_id = sk->sk_bound_dev_if;
}

static int tcp_v6_remember_stamp(struct sock *sk)
{
	/* Alas, not yet... */
	return 0;
}

static struct tcp_func ipv6_specific = {
	.queue_xmit	=	tcp_v6_xmit,
	.send_check	=	tcp_v6_send_check,
	.rebuild_header	=	tcp_v6_rebuild_header,
	.conn_request	=	tcp_v6_conn_request,
	.syn_recv_sock	=	tcp_v6_syn_recv_sock,
	.remember_stamp	=	tcp_v6_remember_stamp,
	.net_header_len	=	sizeof(struct ipv6hdr),

	.setsockopt	=	ipv6_setsockopt,
	.getsockopt	=	ipv6_getsockopt,
	.addr2sockaddr	=	v6_addr2sockaddr,
	.sockaddr_len	=	sizeof(struct sockaddr_in6)
};

/*
 *	TCP over IPv4 via INET6 API
 */

static struct tcp_func ipv6_mapped = {
	.queue_xmit	=	ip_queue_xmit,
	.send_check	=	tcp_v4_send_check,
	.rebuild_header	=	tcp_v4_rebuild_header,
	.conn_request	=	tcp_v6_conn_request,
	.syn_recv_sock	=	tcp_v6_syn_recv_sock,
	.remember_stamp	=	tcp_v4_remember_stamp,
	.net_header_len	=	sizeof(struct iphdr),

	.setsockopt	=	ipv6_setsockopt,
	.getsockopt	=	ipv6_getsockopt,
	.addr2sockaddr	=	v6_addr2sockaddr,
	.sockaddr_len	=	sizeof(struct sockaddr_in6)
};



/* NOTE: A lot of things set to zero explicitly by call to
 *       sk_alloc() so need not be done here.
 */
static int tcp_v6_init_sock(struct sock *sk)
{
	struct tcp_opt *tp = tcp_sk(sk);

	skb_queue_head_init(&tp->out_of_order_queue);
	tcp_init_xmit_timers(sk);
	tcp_prequeue_init(tp);

	tp->rto  = TCP_TIMEOUT_INIT;
	tp->mdev = TCP_TIMEOUT_INIT;

	/* So many TCP implementations out there (incorrectly) count the
	 * initial SYN frame in their delayed-ACK and congestion control
	 * algorithms that we must have the following bandaid to talk
	 * efficiently to them.  -DaveM
	 */
	tp->snd_cwnd = 2;

	/* See draft-stevens-tcpca-spec-01 for discussion of the
	 * initialization of these values.
	 */
	tp->snd_ssthresh = 0x7fffffff;
	tp->snd_cwnd_clamp = ~0;
	tp->mss_cache = 536;

	tp->reordering = sysctl_tcp_reordering;

	sk->sk_state = TCP_CLOSE;

	tp->af_specific = &ipv6_specific;

	sk->sk_write_space = sk_stream_write_space;
	sk->sk_use_write_queue = 1;

	sk->sk_sndbuf = sysctl_tcp_wmem[1];
	sk->sk_rcvbuf = sysctl_tcp_rmem[1];

	atomic_inc(&tcp_sockets_allocated);

	return 0;
}

static int tcp_v6_destroy_sock(struct sock *sk)
{
	extern int tcp_v4_destroy_sock(struct sock *sk);

	tcp_v4_destroy_sock(sk);
	return inet6_destroy_sock(sk);
}

/* Proc filesystem TCPv6 sock list dumping. */
static void get_openreq6(struct seq_file *seq, 
			 struct sock *sk, struct open_request *req, int i, int uid)
{
	struct in6_addr *dest, *src;
	int ttd = req->expires - jiffies;

	if (ttd < 0)
		ttd = 0;

	src = &req->af.v6_req.loc_addr;
	dest = &req->af.v6_req.rmt_addr;
	seq_printf(seq,
		   "%4d: %08X%08X%08X%08X:%04X %08X%08X%08X%08X:%04X "
		   "%02X %08X:%08X %02X:%08lX %08X %5d %8d %d %d %p\n",
		   i,
		   src->s6_addr32[0], src->s6_addr32[1],
		   src->s6_addr32[2], src->s6_addr32[3],
		   ntohs(inet_sk(sk)->sport),
		   dest->s6_addr32[0], dest->s6_addr32[1],
		   dest->s6_addr32[2], dest->s6_addr32[3],
		   ntohs(req->rmt_port),
		   TCP_SYN_RECV,
		   0,0, /* could print option size, but that is af dependent. */
		   1,   /* timers active (only the expire timer) */  
		   jiffies_to_clock_t(ttd), 
		   req->retrans,
		   uid,
		   0,  /* non standard timer */  
		   0, /* open_requests have no inode */
		   0, req);
}

static void get_tcp6_sock(struct seq_file *seq, struct sock *sp, int i)
{
	struct in6_addr *dest, *src;
	__u16 destp, srcp;
	int timer_active;
	unsigned long timer_expires;
	struct inet_opt *inet = inet_sk(sp);
	struct tcp_opt *tp = tcp_sk(sp);
	struct ipv6_pinfo *np = inet6_sk(sp);

	dest  = &np->daddr;
	src   = &np->rcv_saddr;
	destp = ntohs(inet->dport);
	srcp  = ntohs(inet->sport);
	if (tp->pending == TCP_TIME_RETRANS) {
		timer_active	= 1;
		timer_expires	= tp->timeout;
	} else if (tp->pending == TCP_TIME_PROBE0) {
		timer_active	= 4;
		timer_expires	= tp->timeout;
	} else if (timer_pending(&sp->sk_timer)) {
		timer_active	= 2;
		timer_expires	= sp->sk_timer.expires;
	} else {
		timer_active	= 0;
		timer_expires = jiffies;
	}

	seq_printf(seq,
		   "%4d: %08X%08X%08X%08X:%04X %08X%08X%08X%08X:%04X "
		   "%02X %08X:%08X %02X:%08lX %08X %5d %8d %lu %d %p %u %u %u %u %d\n",
		   i,
		   src->s6_addr32[0], src->s6_addr32[1],
		   src->s6_addr32[2], src->s6_addr32[3], srcp,
		   dest->s6_addr32[0], dest->s6_addr32[1],
		   dest->s6_addr32[2], dest->s6_addr32[3], destp,
		   sp->sk_state, 
		   tp->write_seq-tp->snd_una, tp->rcv_nxt-tp->copied_seq,
		   timer_active,
		   jiffies_to_clock_t(timer_expires - jiffies),
		   tp->retransmits,
		   sock_i_uid(sp),
		   tp->probes_out,
		   sock_i_ino(sp),
		   atomic_read(&sp->sk_refcnt), sp,
		   tp->rto, tp->ack.ato, (tp->ack.quick<<1)|tp->ack.pingpong,
		   tp->snd_cwnd, tp->snd_ssthresh>=0xFFFF?-1:tp->snd_ssthresh
		   );
}

static void get_timewait6_sock(struct seq_file *seq, 
			       struct tcp_tw_bucket *tw, int i)
{
	struct in6_addr *dest, *src;
	__u16 destp, srcp;
	int ttd = tw->tw_ttd - jiffies;

	if (ttd < 0)
		ttd = 0;

	dest  = &tw->tw_v6_daddr;
	src   = &tw->tw_v6_rcv_saddr;
	destp = ntohs(tw->tw_dport);
	srcp  = ntohs(tw->tw_sport);

	seq_printf(seq,
		   "%4d: %08X%08X%08X%08X:%04X %08X%08X%08X%08X:%04X "
		   "%02X %08X:%08X %02X:%08lX %08X %5d %8d %d %d %p\n",
		   i,
		   src->s6_addr32[0], src->s6_addr32[1],
		   src->s6_addr32[2], src->s6_addr32[3], srcp,
		   dest->s6_addr32[0], dest->s6_addr32[1],
		   dest->s6_addr32[2], dest->s6_addr32[3], destp,
		   tw->tw_substate, 0, 0,
		   3, jiffies_to_clock_t(ttd), 0, 0, 0, 0,
		   atomic_read(&tw->tw_refcnt), tw);
}

#ifdef CONFIG_PROC_FS
static int tcp6_seq_show(struct seq_file *seq, void *v)
{
	struct tcp_iter_state *st;

	if (v == SEQ_START_TOKEN) {
		seq_puts(seq,
			 "  sl  "
			 "local_address                         "
			 "remote_address                        "
			 "st tx_queue rx_queue tr tm->when retrnsmt"
			 "   uid  timeout inode\n");
		goto out;
	}
	st = seq->private;

	switch (st->state) {
	case TCP_SEQ_STATE_LISTENING:
	case TCP_SEQ_STATE_ESTABLISHED:
		get_tcp6_sock(seq, v, st->num);
		break;
	case TCP_SEQ_STATE_OPENREQ:
		get_openreq6(seq, st->syn_wait_sk, v, st->num, st->uid);
		break;
	case TCP_SEQ_STATE_TIME_WAIT:
		get_timewait6_sock(seq, v, st->num);
		break;
	}
out:
	return 0;
}

static struct file_operations tcp6_seq_fops;
static struct tcp_seq_afinfo tcp6_seq_afinfo = {
	.owner		= THIS_MODULE,
	.name		= "tcp6",
	.family		= AF_INET6,
	.seq_show	= tcp6_seq_show,
	.seq_fops	= &tcp6_seq_fops,
};

int __init tcp6_proc_init(void)
{
	return tcp_proc_register(&tcp6_seq_afinfo);
}

void tcp6_proc_exit(void)
{
	tcp_proc_unregister(&tcp6_seq_afinfo);
}
#endif

struct proto tcpv6_prot = {
	.name			= "TCPv6",
	.close			= tcp_close,
	.connect		= tcp_v6_connect,
	.disconnect		= tcp_disconnect,
	.accept			= tcp_accept,
	.ioctl			= tcp_ioctl,
	.init			= tcp_v6_init_sock,
	.destroy		= tcp_v6_destroy_sock,
	.shutdown		= tcp_shutdown,
	.setsockopt		= tcp_setsockopt,
	.getsockopt		= tcp_getsockopt,
	.sendmsg		= tcp_sendmsg,
	.recvmsg		= tcp_recvmsg,
	.backlog_rcv		= tcp_v6_do_rcv,
	.hash			= tcp_v6_hash,
	.unhash			= tcp_unhash,
	.get_port		= tcp_v6_get_port,
	.enter_memory_pressure	= tcp_enter_memory_pressure,
	.sockets_allocated	= &tcp_sockets_allocated,
	.memory_allocated	= &tcp_memory_allocated,
	.memory_pressure	= &tcp_memory_pressure,
	.sysctl_mem		= sysctl_tcp_mem,
	.sysctl_wmem		= sysctl_tcp_wmem,
	.sysctl_rmem		= sysctl_tcp_rmem,
	.max_header		= MAX_TCP_HEADER,
};

static struct inet6_protocol tcpv6_protocol = {
	.handler	=	tcp_v6_rcv,
	.err_handler	=	tcp_v6_err,
	.flags		=	INET6_PROTO_NOPOLICY|INET6_PROTO_FINAL,
};

extern struct proto_ops inet6_stream_ops;

static struct inet_protosw tcpv6_protosw = {
	.type		=	SOCK_STREAM,
	.protocol	=	IPPROTO_TCP,
	.prot		=	&tcpv6_prot,
	.ops		=	&inet6_stream_ops,
	.capability	=	-1,
	.no_check	=	0,
	.flags		=	INET_PROTOSW_PERMANENT,
};

void __init tcpv6_init(void)
{
	/* register inet6 protocol */
	if (inet6_add_protocol(&tcpv6_protocol, IPPROTO_TCP) < 0)
		printk(KERN_ERR "tcpv6_init: Could not register protocol\n");
	inet6_register_protosw(&tcpv6_protosw);
}
