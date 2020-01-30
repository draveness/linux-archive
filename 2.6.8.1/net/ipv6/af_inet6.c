/*
 *	PF_INET6 socket protocol family
 *	Linux INET6 implementation 
 *
 *	Authors:
 *	Pedro Roque		<roque@di.fc.ul.pt>	
 *
 *	Adapted from linux/net/ipv4/af_inet.c
 *
 *	$Id: af_inet6.c,v 1.66 2002/02/01 22:01:04 davem Exp $
 *
 * 	Fixes:
 *	piggy, Karl Knutson	:	Socket protocol table
 * 	Hideaki YOSHIFUJI	:	sin6_scope_id support
 * 	Arnaldo Melo		: 	check proc_net_create return, cleanups
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
#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/init.h>

#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/icmpv6.h>
#include <linux/smp_lock.h>

#include <net/ip.h>
#include <net/ipv6.h>
#include <net/udp.h>
#include <net/tcp.h>
#include <net/ipip.h>
#include <net/protocol.h>
#include <net/inet_common.h>
#include <net/transp_v6.h>
#include <net/ip6_route.h>
#include <net/addrconf.h>
#ifdef CONFIG_IPV6_TUNNEL
#include <net/ip6_tunnel.h>
#endif

#include <asm/uaccess.h>
#include <asm/system.h>

MODULE_AUTHOR("Cast of dozens");
MODULE_DESCRIPTION("IPv6 protocol stack for Linux");
MODULE_LICENSE("GPL");

/* IPv6 procfs goodies... */

#ifdef CONFIG_PROC_FS
extern int raw6_proc_init(void);
extern void raw6_proc_exit(void);
extern int tcp6_proc_init(void);
extern void tcp6_proc_exit(void);
extern int udp6_proc_init(void);
extern void udp6_proc_exit(void);
extern int ipv6_misc_proc_init(void);
extern void ipv6_misc_proc_exit(void);
extern int ac6_proc_init(void);
extern void ac6_proc_exit(void);
extern int if6_proc_init(void);
extern void if6_proc_exit(void);
#endif

int sysctl_ipv6_bindv6only;

#ifdef INET_REFCNT_DEBUG
atomic_t inet6_sock_nr;
#endif

/* Per protocol sock slabcache */
kmem_cache_t *tcp6_sk_cachep;
kmem_cache_t *udp6_sk_cachep;
kmem_cache_t *raw6_sk_cachep;

/* The inetsw table contains everything that inet_create needs to
 * build a new socket.
 */
static struct list_head inetsw6[SOCK_MAX];
static spinlock_t inetsw6_lock = SPIN_LOCK_UNLOCKED;

static void inet6_sock_destruct(struct sock *sk)
{
	inet_sock_destruct(sk);

#ifdef INET_REFCNT_DEBUG
	atomic_dec(&inet6_sock_nr);
#endif
}

static __inline__ kmem_cache_t *inet6_sk_slab(int protocol)
{
        kmem_cache_t* rc = tcp6_sk_cachep;

        if (protocol == IPPROTO_UDP)
                rc = udp6_sk_cachep;
        else if (protocol == IPPROTO_RAW)
                rc = raw6_sk_cachep;
        return rc;
}

static __inline__ int inet6_sk_size(int protocol)
{
        int rc = sizeof(struct tcp6_sock);

        if (protocol == IPPROTO_UDP)
                rc = sizeof(struct udp6_sock);
        else if (protocol == IPPROTO_RAW)
                rc = sizeof(struct raw6_sock);
        return rc;
}

static __inline__ struct ipv6_pinfo *inet6_sk_generic(struct sock *sk)
{
	struct ipv6_pinfo *rc = (&((struct tcp6_sock *)sk)->inet6);

        if (sk->sk_protocol == IPPROTO_UDP)
                rc = (&((struct udp6_sock *)sk)->inet6);
        else if (sk->sk_protocol == IPPROTO_RAW)
                rc = (&((struct raw6_sock *)sk)->inet6);
        return rc;
}

static int inet6_create(struct socket *sock, int protocol)
{
	struct inet_opt *inet;
	struct ipv6_pinfo *np;
	struct sock *sk;
	struct tcp6_sock* tcp6sk;
	struct list_head *p;
	struct inet_protosw *answer;

	sk = sk_alloc(PF_INET6, GFP_KERNEL, inet6_sk_size(protocol),
		      inet6_sk_slab(protocol));
	if (sk == NULL) 
		goto do_oom;

	/* Look for the requested type/protocol pair. */
	answer = NULL;
	rcu_read_lock();
	list_for_each_rcu(p, &inetsw6[sock->type]) {
		answer = list_entry(p, struct inet_protosw, list);

		/* Check the non-wild match. */
		if (protocol == answer->protocol) {
			if (protocol != IPPROTO_IP)
				break;
		} else {
			/* Check for the two wild cases. */
			if (IPPROTO_IP == protocol) {
				protocol = answer->protocol;
				break;
			}
			if (IPPROTO_IP == answer->protocol)
				break;
		}
		answer = NULL;
	}

	if (!answer)
		goto free_and_badtype;
	if (answer->capability > 0 && !capable(answer->capability))
		goto free_and_badperm;
	if (!protocol)
		goto free_and_noproto;

	sock->ops = answer->ops;
	sock_init_data(sock, sk);
	sk_set_owner(sk, THIS_MODULE);

	sk->sk_prot = answer->prot;
	sk->sk_no_check = answer->no_check;
	if (INET_PROTOSW_REUSE & answer->flags)
		sk->sk_reuse = 1;
	rcu_read_unlock();

	inet = inet_sk(sk);

	if (SOCK_RAW == sock->type) {
		inet->num = protocol;
		if (IPPROTO_RAW == protocol)
			inet->hdrincl = 1;
	}

	sk->sk_destruct		= inet6_sock_destruct;
	sk->sk_zapped		= 0;
	sk->sk_family		= PF_INET6;
	sk->sk_protocol		= protocol;

	sk->sk_backlog_rcv	= answer->prot->backlog_rcv;

	tcp6sk		= (struct tcp6_sock *)sk;
	tcp6sk->pinet6 = np = inet6_sk_generic(sk);
	np->hop_limit	= -1;
	np->mcast_hops	= -1;
	np->mc_loop	= 1;
	np->pmtudisc	= IPV6_PMTUDISC_WANT;
	np->ipv6only	= sysctl_ipv6_bindv6only;
	
	/* Init the ipv4 part of the socket since we can have sockets
	 * using v6 API for ipv4.
	 */
	inet->uc_ttl	= -1;

	inet->mc_loop	= 1;
	inet->mc_ttl	= 1;
	inet->mc_index	= 0;
	inet->mc_list	= NULL;

	if (ipv4_config.no_pmtu_disc)
		inet->pmtudisc = IP_PMTUDISC_DONT;
	else
		inet->pmtudisc = IP_PMTUDISC_WANT;


#ifdef INET_REFCNT_DEBUG
	atomic_inc(&inet6_sock_nr);
	atomic_inc(&inet_sock_nr);
#endif
	if (inet->num) {
		/* It assumes that any protocol which allows
		 * the user to assign a number at socket
		 * creation time automatically shares.
		 */
		inet->sport = ntohs(inet->num);
		sk->sk_prot->hash(sk);
	}
	if (sk->sk_prot->init) {
		int err = sk->sk_prot->init(sk);
		if (err != 0) {
			sk_common_release(sk);
			return err;
		}
	}
	return 0;

free_and_badtype:
	rcu_read_unlock();
	sk_free(sk);
	return -ESOCKTNOSUPPORT;
free_and_badperm:
	rcu_read_unlock();
	sk_free(sk);
	return -EPERM;
free_and_noproto:
	rcu_read_unlock();
	sk_free(sk);
	return -EPROTONOSUPPORT;
do_oom:
	return -ENOBUFS;
}


/* bind for INET6 API */
int inet6_bind(struct socket *sock, struct sockaddr *uaddr, int addr_len)
{
	struct sockaddr_in6 *addr=(struct sockaddr_in6 *)uaddr;
	struct sock *sk = sock->sk;
	struct inet_opt *inet = inet_sk(sk);
	struct ipv6_pinfo *np = inet6_sk(sk);
	__u32 v4addr = 0;
	unsigned short snum;
	int addr_type = 0;
	int err = 0;

	/* If the socket has its own bind function then use it. */
	if (sk->sk_prot->bind)
		return sk->sk_prot->bind(sk, uaddr, addr_len);

	if (addr_len < SIN6_LEN_RFC2133)
		return -EINVAL;
	addr_type = ipv6_addr_type(&addr->sin6_addr);
	if ((addr_type & IPV6_ADDR_MULTICAST) && sock->type == SOCK_STREAM)
		return -EINVAL;

	snum = ntohs(addr->sin6_port);
	if (snum && snum < PROT_SOCK && !capable(CAP_NET_BIND_SERVICE))
		return -EACCES;

	lock_sock(sk);

	/* Check these errors (active socket, double bind). */
	if (sk->sk_state != TCP_CLOSE || inet->num) {
		err = -EINVAL;
		goto out;
	}

	/* Check if the address belongs to the host. */
	if (addr_type == IPV6_ADDR_MAPPED) {
		v4addr = addr->sin6_addr.s6_addr32[3];
		if (inet_addr_type(v4addr) != RTN_LOCAL) {
			err = -EADDRNOTAVAIL;
			goto out;
		}
	} else {
		if (addr_type != IPV6_ADDR_ANY) {
			struct net_device *dev = NULL;

			if (addr_type & IPV6_ADDR_LINKLOCAL) {
				if (addr_len >= sizeof(struct sockaddr_in6) &&
				    addr->sin6_scope_id) {
					/* Override any existing binding, if another one
					 * is supplied by user.
					 */
					sk->sk_bound_dev_if = addr->sin6_scope_id;
				}
				
				/* Binding to link-local address requires an interface */
				if (!sk->sk_bound_dev_if) {
					err = -EINVAL;
					goto out;
				}
				dev = dev_get_by_index(sk->sk_bound_dev_if);
				if (!dev) {
					err = -ENODEV;
					goto out;
				}
			}

			/* ipv4 addr of the socket is invalid.  Only the
			 * unspecified and mapped address have a v4 equivalent.
			 */
			v4addr = LOOPBACK4_IPV6;
			if (!(addr_type & IPV6_ADDR_MULTICAST))	{
				if (!ipv6_chk_addr(&addr->sin6_addr, dev, 0)) {
					if (dev)
						dev_put(dev);
					err = -EADDRNOTAVAIL;
					goto out;
				}
			}
			if (dev)
				dev_put(dev);
		}
	}

	inet->rcv_saddr = v4addr;
	inet->saddr = v4addr;

	ipv6_addr_copy(&np->rcv_saddr, &addr->sin6_addr);
		
	if (!(addr_type & IPV6_ADDR_MULTICAST))
		ipv6_addr_copy(&np->saddr, &addr->sin6_addr);

	/* Make sure we are allowed to bind here. */
	if (sk->sk_prot->get_port(sk, snum)) {
		inet_reset_saddr(sk);
		err = -EADDRINUSE;
		goto out;
	}

	if (addr_type != IPV6_ADDR_ANY)
		sk->sk_userlocks |= SOCK_BINDADDR_LOCK;
	if (snum)
		sk->sk_userlocks |= SOCK_BINDPORT_LOCK;
	inet->sport = ntohs(inet->num);
	inet->dport = 0;
	inet->daddr = 0;
out:
	release_sock(sk);
	return err;
}

int inet6_release(struct socket *sock)
{
	struct sock *sk = sock->sk;

	if (sk == NULL)
		return -EINVAL;

	/* Free mc lists */
	ipv6_sock_mc_close(sk);

	/* Free ac lists */
	ipv6_sock_ac_close(sk);

	return inet_release(sock);
}

int inet6_destroy_sock(struct sock *sk)
{
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct sk_buff *skb;
	struct ipv6_txoptions *opt;

	/*
	 *	Release destination entry
	 */

	sk_dst_reset(sk);

	/* Release rx options */

	if ((skb = xchg(&np->pktoptions, NULL)) != NULL)
		kfree_skb(skb);

	/* Free flowlabels */
	fl6_free_socklist(sk);

	/* Free tx options */

	if ((opt = xchg(&np->opt, NULL)) != NULL)
		sock_kfree_s(sk, opt, opt->tot_len);

	return 0;
}

/*
 *	This does both peername and sockname.
 */
 
int inet6_getname(struct socket *sock, struct sockaddr *uaddr,
		 int *uaddr_len, int peer)
{
	struct sockaddr_in6 *sin=(struct sockaddr_in6 *)uaddr;
	struct sock *sk = sock->sk;
	struct inet_opt *inet = inet_sk(sk);
	struct ipv6_pinfo *np = inet6_sk(sk);
  
	sin->sin6_family = AF_INET6;
	sin->sin6_flowinfo = 0;
	sin->sin6_scope_id = 0;
	if (peer) {
		if (!inet->dport)
			return -ENOTCONN;
		if (((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_SYN_SENT)) &&
		    peer == 1)
			return -ENOTCONN;
		sin->sin6_port = inet->dport;
		ipv6_addr_copy(&sin->sin6_addr, &np->daddr);
		if (np->sndflow)
			sin->sin6_flowinfo = np->flow_label;
	} else {
		if (ipv6_addr_any(&np->rcv_saddr))
			ipv6_addr_copy(&sin->sin6_addr, &np->saddr);
		else
			ipv6_addr_copy(&sin->sin6_addr, &np->rcv_saddr);

		sin->sin6_port = inet->sport;
	}
	if (ipv6_addr_type(&sin->sin6_addr) & IPV6_ADDR_LINKLOCAL)
		sin->sin6_scope_id = sk->sk_bound_dev_if;
	*uaddr_len = sizeof(*sin);
	return(0);
}

int inet6_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	struct sock *sk = sock->sk;
	int err = -EINVAL;

	switch(cmd) 
	{
	case SIOCGSTAMP:
		return sock_get_timestamp(sk, (struct timeval __user *)arg);

	case SIOCADDRT:
	case SIOCDELRT:
	  
		return(ipv6_route_ioctl(cmd,(void __user *)arg));

	case SIOCSIFADDR:
		return addrconf_add_ifaddr((void __user *) arg);
	case SIOCDIFADDR:
		return addrconf_del_ifaddr((void __user *) arg);
	case SIOCSIFDSTADDR:
		return addrconf_set_dstaddr((void __user *) arg);
	default:
		if (!sk->sk_prot->ioctl ||
		    (err = sk->sk_prot->ioctl(sk, cmd, arg)) == -ENOIOCTLCMD)
			return(dev_ioctl(cmd,(void __user *) arg));		
		return err;
	}
	/*NOTREACHED*/
	return(0);
}

struct proto_ops inet6_stream_ops = {
	.family =	PF_INET6,
	.owner =	THIS_MODULE,
	.release =	inet6_release,
	.bind =		inet6_bind,
	.connect =	inet_stream_connect,		/* ok		*/
	.socketpair =	sock_no_socketpair,		/* a do nothing	*/
	.accept =	inet_accept,			/* ok		*/
	.getname =	inet6_getname, 
	.poll =		tcp_poll,			/* ok		*/
	.ioctl =	inet6_ioctl,			/* must change  */
	.listen =	inet_listen,			/* ok		*/
	.shutdown =	inet_shutdown,			/* ok		*/
	.setsockopt =	sock_common_setsockopt,		/* ok		*/
	.getsockopt =	sock_common_getsockopt,		/* ok		*/
	.sendmsg =	inet_sendmsg,			/* ok		*/
	.recvmsg =	sock_common_recvmsg,		/* ok		*/
	.mmap =		sock_no_mmap,
	.sendpage =	tcp_sendpage
};

struct proto_ops inet6_dgram_ops = {
	.family =	PF_INET6,
	.owner =	THIS_MODULE,
	.release =	inet6_release,
	.bind =		inet6_bind,
	.connect =	inet_dgram_connect,		/* ok		*/
	.socketpair =	sock_no_socketpair,		/* a do nothing	*/
	.accept =	sock_no_accept,			/* a do nothing	*/
	.getname =	inet6_getname, 
	.poll =		datagram_poll,			/* ok		*/
	.ioctl =	inet6_ioctl,			/* must change  */
	.listen =	sock_no_listen,			/* ok		*/
	.shutdown =	inet_shutdown,			/* ok		*/
	.setsockopt =	sock_common_setsockopt,		/* ok		*/
	.getsockopt =	sock_common_getsockopt,		/* ok		*/
	.sendmsg =	inet_sendmsg,			/* ok		*/
	.recvmsg =	sock_common_recvmsg,		/* ok		*/
	.mmap =		sock_no_mmap,
	.sendpage =	sock_no_sendpage,
};

static struct net_proto_family inet6_family_ops = {
	.family = PF_INET6,
	.create = inet6_create,
	.owner	= THIS_MODULE,
};

#ifdef CONFIG_SYSCTL
extern void ipv6_sysctl_register(void);
extern void ipv6_sysctl_unregister(void);
#endif

static struct inet_protosw rawv6_protosw = {
	.type		= SOCK_RAW,
	.protocol	= IPPROTO_IP,	/* wild card */
	.prot		= &rawv6_prot,
	.ops		= &inet6_dgram_ops,
	.capability	= CAP_NET_RAW,
	.no_check	= UDP_CSUM_DEFAULT,
	.flags		= INET_PROTOSW_REUSE,
};

void
inet6_register_protosw(struct inet_protosw *p)
{
	struct list_head *lh;
	struct inet_protosw *answer;
	int protocol = p->protocol;
	struct list_head *last_perm;

	spin_lock_bh(&inetsw6_lock);

	if (p->type >= SOCK_MAX)
		goto out_illegal;

	/* If we are trying to override a permanent protocol, bail. */
	answer = NULL;
	last_perm = &inetsw6[p->type];
	list_for_each(lh, &inetsw6[p->type]) {
		answer = list_entry(lh, struct inet_protosw, list);

		/* Check only the non-wild match. */
		if (INET_PROTOSW_PERMANENT & answer->flags) {
			if (protocol == answer->protocol)
				break;
			last_perm = lh;
		}

		answer = NULL;
	}
	if (answer)
		goto out_permanent;

	/* Add the new entry after the last permanent entry if any, so that
	 * the new entry does not override a permanent entry when matched with
	 * a wild-card protocol. But it is allowed to override any existing
	 * non-permanent entry.  This means that when we remove this entry, the 
	 * system automatically returns to the old behavior.
	 */
	list_add_rcu(&p->list, last_perm);
out:
	spin_unlock_bh(&inetsw6_lock);
	return;

out_permanent:
	printk(KERN_ERR "Attempt to override permanent protocol %d.\n",
	       protocol);
	goto out;

out_illegal:
	printk(KERN_ERR
	       "Ignoring attempt to register invalid socket type %d.\n",
	       p->type);
	goto out;
}

void
inet6_unregister_protosw(struct inet_protosw *p)
{
	if (INET_PROTOSW_PERMANENT & p->flags) {
		printk(KERN_ERR
		       "Attempt to unregister permanent protocol %d.\n",
		       p->protocol);
	} else {
		spin_lock_bh(&inetsw6_lock);
		list_del_rcu(&p->list);
		spin_unlock_bh(&inetsw6_lock);

		synchronize_net();
	}
}

int
snmp6_mib_init(void *ptr[2], size_t mibsize, size_t mibalign)
{
	if (ptr == NULL)
		return -EINVAL;

	ptr[0] = __alloc_percpu(mibsize, mibalign);
	if (!ptr[0])
		goto err0;

	ptr[1] = __alloc_percpu(mibsize, mibalign);
	if (!ptr[1])
		goto err1;

	return 0;

err1:
	free_percpu(ptr[0]);
	ptr[0] = NULL;
err0:
	return -ENOMEM;
}

void
snmp6_mib_free(void *ptr[2])
{
	if (ptr == NULL)
		return;
	free_percpu(ptr[0]);
	free_percpu(ptr[1]);
	ptr[0] = ptr[1] = NULL;
}

static int __init init_ipv6_mibs(void)
{
	if (snmp6_mib_init((void **)ipv6_statistics, sizeof (struct ipstats_mib),
			   __alignof__(struct ipstats_mib)) < 0)
		goto err_ip_mib;
	if (snmp6_mib_init((void **)icmpv6_statistics, sizeof (struct icmpv6_mib),
			   __alignof__(struct icmpv6_mib)) < 0)
		goto err_icmp_mib;
	if (snmp6_mib_init((void **)udp_stats_in6, sizeof (struct udp_mib),
			   __alignof__(struct udp_mib)) < 0)
		goto err_udp_mib;
	return 0;

err_udp_mib:
	snmp6_mib_free((void **)icmpv6_statistics);
err_icmp_mib:
	snmp6_mib_free((void **)ipv6_statistics);
err_ip_mib:
	return -ENOMEM;
	
}

static void cleanup_ipv6_mibs(void)
{
	snmp6_mib_free((void **)ipv6_statistics);
	snmp6_mib_free((void **)icmpv6_statistics);
	snmp6_mib_free((void **)udp_stats_in6);
}

extern int ipv6_misc_proc_init(void);

static int __init inet6_init(void)
{
	struct sk_buff *dummy_skb;
        struct list_head *r;
	int err;

#ifdef MODULE
#if 0 /* FIXME --RR */
	if (!mod_member_present(&__this_module, can_unload))
	  return -EINVAL;

	__this_module.can_unload = &ipv6_unload;
#endif
#endif

	if (sizeof(struct inet6_skb_parm) > sizeof(dummy_skb->cb))
	{
		printk(KERN_CRIT "inet6_proto_init: size fault\n");
		return -EINVAL;
	}
	/* allocate our sock slab caches */
        tcp6_sk_cachep = kmem_cache_create("tcp6_sock",
					   sizeof(struct tcp6_sock), 0,
                                           SLAB_HWCACHE_ALIGN, NULL, NULL);
        udp6_sk_cachep = kmem_cache_create("udp6_sock",
					   sizeof(struct udp6_sock), 0,
                                           SLAB_HWCACHE_ALIGN, NULL, NULL);
        raw6_sk_cachep = kmem_cache_create("raw6_sock",
					   sizeof(struct raw6_sock), 0,
                                           SLAB_HWCACHE_ALIGN, NULL, NULL);
        if (!tcp6_sk_cachep || !udp6_sk_cachep || !raw6_sk_cachep)
                printk(KERN_CRIT "%s: Can't create protocol sock SLAB "
		       "caches!\n", __FUNCTION__);

	/* Register the socket-side information for inet6_create.  */
	for(r = &inetsw6[0]; r < &inetsw6[SOCK_MAX]; ++r)
		INIT_LIST_HEAD(r);

	/* We MUST register RAW sockets before we create the ICMP6,
	 * IGMP6, or NDISC control sockets.
	 */
	inet6_register_protosw(&rawv6_protosw);

	/* Register the family here so that the init calls below will
	 * be able to create sockets. (?? is this dangerous ??)
	 */
	(void) sock_register(&inet6_family_ops);

	/* Initialise ipv6 mibs */
	err = init_ipv6_mibs();
	if (err)
		goto init_mib_fail;
	
	/*
	 *	ipngwg API draft makes clear that the correct semantics
	 *	for TCP and UDP is to consider one TCP and UDP instance
	 *	in a host availiable by both INET and INET6 APIs and
	 *	able to communicate via both network protocols.
	 */

#ifdef CONFIG_SYSCTL
	ipv6_sysctl_register();
#endif
	err = icmpv6_init(&inet6_family_ops);
	if (err)
		goto icmp_fail;
	err = ndisc_init(&inet6_family_ops);
	if (err)
		goto ndisc_fail;
	err = igmp6_init(&inet6_family_ops);
	if (err)
		goto igmp_fail;
	/* Create /proc/foo6 entries. */
#ifdef CONFIG_PROC_FS
	err = -ENOMEM;
	if (raw6_proc_init())
		goto proc_raw6_fail;
	if (tcp6_proc_init())
		goto proc_tcp6_fail;
	if (udp6_proc_init())
		goto proc_udp6_fail;
	if (ipv6_misc_proc_init())
		goto proc_misc6_fail;

	if (ac6_proc_init())
		goto proc_anycast6_fail;
	if (if6_proc_init())
		goto proc_if6_fail;
#endif
	ipv6_packet_init();
	ip6_route_init();
	ip6_flowlabel_init();
	addrconf_init();
	sit_init();

	/* Init v6 extension headers. */
	ipv6_rthdr_init();
	ipv6_frag_init();
	ipv6_nodata_init();
	ipv6_destopt_init();

	/* Init v6 transport protocols. */
	udpv6_init();
	tcpv6_init();

	return 0;

#ifdef CONFIG_PROC_FS
proc_if6_fail:
	ac6_proc_exit();
proc_anycast6_fail:
	ipv6_misc_proc_exit();
proc_misc6_fail:
	udp6_proc_exit();
proc_udp6_fail:
	tcp6_proc_exit();
proc_tcp6_fail:
	raw6_proc_exit();
proc_raw6_fail:
	igmp6_cleanup();
#endif
igmp_fail:
	ndisc_cleanup();
ndisc_fail:
	icmpv6_cleanup();
icmp_fail:
#ifdef CONFIG_SYSCTL
	ipv6_sysctl_unregister();
#endif
	cleanup_ipv6_mibs();
init_mib_fail:
	return err;
}
module_init(inet6_init);

static void __exit inet6_exit(void)
{
	/* First of all disallow new sockets creation. */
	sock_unregister(PF_INET6);
#ifdef CONFIG_PROC_FS
	if6_proc_exit();
	ac6_proc_exit();
 	ipv6_misc_proc_exit();
 	udp6_proc_exit();
 	tcp6_proc_exit();
 	raw6_proc_exit();
#endif
	/* Cleanup code parts. */
	sit_cleanup();
	ip6_flowlabel_cleanup();
	addrconf_cleanup();
	ip6_route_cleanup();
	ipv6_packet_cleanup();
	igmp6_cleanup();
	ndisc_cleanup();
	icmpv6_cleanup();
#ifdef CONFIG_SYSCTL
	ipv6_sysctl_unregister();	
#endif
	cleanup_ipv6_mibs();
	kmem_cache_destroy(tcp6_sk_cachep);
	kmem_cache_destroy(udp6_sk_cachep);
	kmem_cache_destroy(raw6_sk_cachep);
}
module_exit(inet6_exit);

MODULE_ALIAS_NETPROTO(PF_INET6);
