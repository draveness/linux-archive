/*
 *	X.25 Packet Layer release 002
 *
 *	This is ALPHA test software. This code may break your machine,
 *	randomly fail to work with new releases, misbehave and/or generally
 *	screw up. It might even work. 
 *
 *	This code REQUIRES 2.1.15 or higher
 *
 *	This module:
 *		This module is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 *	History
 *	X.25 001	Jonathan Naylor	Started coding.
 *	X.25 002	Jonathan Naylor	Centralised disconnect handling.
 *					New timer architecture.
 *	2000-03-11	Henner Eisen	MSG_EOR handling more POSIX compliant.
 *	2000-03-22	Daniela Squassoni Allowed disabling/enabling of 
 *					  facilities negotiation and increased 
 *					  the throughput upper limit.
 *	2000-08-27	Arnaldo C. Melo s/suser/capable/ + micro cleanups
 *	2000-09-04	Henner Eisen	Set sock->state in x25_accept(). 
 *					Fixed x25_output() related skb leakage.
 *	2000-10-02	Henner Eisen	Made x25_kick() single threaded per socket.
 *	2000-10-27	Henner Eisen    MSG_DONTWAIT for fragment allocation.
 *	2000-11-14	Henner Eisen    Closing datalink from NETDEV_GOING_DOWN
 *	2002-10-06	Arnaldo C. Melo Get rid of cli/sti, move proc stuff to
 *					x25_proc.c, using seq_file
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/stat.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <linux/fcntl.h>
#include <linux/termios.h>	/* For TIOCINQ/OUTQ */
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/notifier.h>
#include <linux/init.h>
#include <net/x25.h>

int sysctl_x25_restart_request_timeout = X25_DEFAULT_T20;
int sysctl_x25_call_request_timeout    = X25_DEFAULT_T21;
int sysctl_x25_reset_request_timeout   = X25_DEFAULT_T22;
int sysctl_x25_clear_request_timeout   = X25_DEFAULT_T23;
int sysctl_x25_ack_holdback_timeout    = X25_DEFAULT_T2;

HLIST_HEAD(x25_list);
rwlock_t x25_list_lock = RW_LOCK_UNLOCKED;

static struct proto_ops x25_proto_ops;

static struct x25_address null_x25_address = {"               "};

int x25_addr_ntoa(unsigned char *p, struct x25_address *called_addr,
		  struct x25_address *calling_addr)
{
	int called_len, calling_len;
	char *called, *calling;
	int i;

	called_len  = (*p >> 0) & 0x0F;
	calling_len = (*p >> 4) & 0x0F;

	called  = called_addr->x25_addr;
	calling = calling_addr->x25_addr;
	p++;

	for (i = 0; i < (called_len + calling_len); i++) {
		if (i < called_len) {
			if (i % 2 != 0) {
				*called++ = ((*p >> 0) & 0x0F) + '0';
				p++;
			} else {
				*called++ = ((*p >> 4) & 0x0F) + '0';
			}
		} else {
			if (i % 2 != 0) {
				*calling++ = ((*p >> 0) & 0x0F) + '0';
				p++;
			} else {
				*calling++ = ((*p >> 4) & 0x0F) + '0';
			}
		}
	}

	*called = *calling = '\0';

	return 1 + (called_len + calling_len + 1) / 2;
}

int x25_addr_aton(unsigned char *p, struct x25_address *called_addr,
		  struct x25_address *calling_addr)
{
	unsigned int called_len, calling_len;
	char *called, *calling;
	int i;

	called  = called_addr->x25_addr;
	calling = calling_addr->x25_addr;

	called_len  = strlen(called);
	calling_len = strlen(calling);

	*p++ = (calling_len << 4) | (called_len << 0);

	for (i = 0; i < (called_len + calling_len); i++) {
		if (i < called_len) {
			if (i % 2 != 0) {
				*p |= (*called++ - '0') << 0;
				p++;
			} else {
				*p = 0x00;
				*p |= (*called++ - '0') << 4;
			}
		} else {
			if (i % 2 != 0) {
				*p |= (*calling++ - '0') << 0;
				p++;
			} else {
				*p = 0x00;
				*p |= (*calling++ - '0') << 4;
			}
		}
	}

	return 1 + (called_len + calling_len + 1) / 2;
}

/*
 *	Socket removal during an interrupt is now safe.
 */
static void x25_remove_socket(struct sock *sk)
{
	write_lock_bh(&x25_list_lock);
	sk_del_node_init(sk);
	write_unlock_bh(&x25_list_lock);
}

/*
 *	Kill all bound sockets on a dropped device.
 */
static void x25_kill_by_device(struct net_device *dev)
{
	struct sock *s;
	struct hlist_node *node;

	write_lock_bh(&x25_list_lock);

	sk_for_each(s, node, &x25_list)
		if (x25_sk(s)->neighbour && x25_sk(s)->neighbour->dev == dev)
			x25_disconnect(s, ENETUNREACH, 0, 0);

	write_unlock_bh(&x25_list_lock);
}

/*
 *	Handle device status changes.
 */
static int x25_device_event(struct notifier_block *this, unsigned long event,
			    void *ptr)
{
	struct net_device *dev = ptr;
	struct x25_neigh *nb;

	if (dev->type == ARPHRD_X25
#if defined(CONFIG_LLC) || defined(CONFIG_LLC_MODULE)
	 || dev->type == ARPHRD_ETHER
#endif
	 ) {
		switch (event) {
			case NETDEV_UP:
				x25_link_device_up(dev);
				break;
			case NETDEV_GOING_DOWN:
				nb = x25_get_neigh(dev);
				if (nb) {
					x25_terminate_link(nb);
					x25_neigh_put(nb);
				}
				break;
			case NETDEV_DOWN:
				x25_kill_by_device(dev);
				x25_route_device_down(dev);
				x25_link_device_down(dev);
				break;
		}
	}

	return NOTIFY_DONE;
}

/*
 *	Add a socket to the bound sockets list.
 */
static void x25_insert_socket(struct sock *sk)
{
	write_lock_bh(&x25_list_lock);
	sk_add_node(sk, &x25_list);
	write_unlock_bh(&x25_list_lock);
}

/*
 *	Find a socket that wants to accept the Call Request we just
 *	received.
 */
static struct sock *x25_find_listener(struct x25_address *addr)
{
	struct sock *s;
	struct hlist_node *node;

	read_lock_bh(&x25_list_lock);

	sk_for_each(s, node, &x25_list)
		if ((!strcmp(addr->x25_addr,
			     x25_sk(s)->source_addr.x25_addr) ||
		     !strcmp(addr->x25_addr,
			     null_x25_address.x25_addr)) &&
		     s->sk_state == TCP_LISTEN) {
			sock_hold(s);
			goto found;
		}
	s = NULL;
found:
	read_unlock_bh(&x25_list_lock);
	return s;
}

/*
 *	Find a connected X.25 socket given my LCI and neighbour.
 */
struct sock *__x25_find_socket(unsigned int lci, struct x25_neigh *nb)
{
	struct sock *s;
	struct hlist_node *node;

	sk_for_each(s, node, &x25_list)
		if (x25_sk(s)->lci == lci && x25_sk(s)->neighbour == nb) {
			sock_hold(s);
			goto found;
		}
	s = NULL;
found:
	return s;
}

struct sock *x25_find_socket(unsigned int lci, struct x25_neigh *nb)
{
	struct sock *s;

	read_lock_bh(&x25_list_lock);
	s = __x25_find_socket(lci, nb);
	read_unlock_bh(&x25_list_lock);
	return s;
}

/*
 *	Find a unique LCI for a given device.
 */
unsigned int x25_new_lci(struct x25_neigh *nb)
{
	unsigned int lci = 1;
	struct sock *sk;

	read_lock_bh(&x25_list_lock);

	while ((sk = __x25_find_socket(lci, nb)) != NULL) {
		sock_put(sk);
		if (++lci == 4096) {
			lci = 0;
			break;
		}
	}

	read_unlock_bh(&x25_list_lock);
	return lci;
}

/*
 *	Deferred destroy.
 */
void x25_destroy_socket(struct sock *);

/*
 *	handler for deferred kills.
 */
static void x25_destroy_timer(unsigned long data)
{
	x25_destroy_socket((struct sock *)data);
}

/*
 *	This is called from user mode and the timers. Thus it protects itself
 *	against interrupt users but doesn't worry about being called during
 *	work. Once it is removed from the queue no interrupt or bottom half
 *	will touch it and we are (fairly 8-) ) safe.
 *	Not static as it's used by the timer
 */
void x25_destroy_socket(struct sock *sk)
{
	struct sk_buff *skb;

	sock_hold(sk);
	lock_sock(sk);
	x25_stop_heartbeat(sk);
	x25_stop_timer(sk);

	x25_remove_socket(sk);
	x25_clear_queues(sk);		/* Flush the queues */

	while ((skb = skb_dequeue(&sk->sk_receive_queue)) != NULL) {
		if (skb->sk != sk) {		/* A pending connection */
			/*
			 * Queue the unaccepted socket for death
			 */
			sock_set_flag(skb->sk, SOCK_DEAD);
			x25_start_heartbeat(skb->sk);
			x25_sk(skb->sk)->state = X25_STATE_0;
		}

		kfree_skb(skb);
	}

	if (atomic_read(&sk->sk_wmem_alloc) ||
	    atomic_read(&sk->sk_rmem_alloc)) {
		/* Defer: outstanding buffers */
		sk->sk_timer.expires  = jiffies + 10 * HZ;
		sk->sk_timer.function = x25_destroy_timer;
		add_timer(&sk->sk_timer);
	} else {
		/* drop last reference so sock_put will free */
		__sock_put(sk);
	}

	release_sock(sk);
	sock_put(sk);
}

/*
 *	Handling for system calls applied via the various interfaces to a
 *	X.25 socket object.
 */

static int x25_setsockopt(struct socket *sock, int level, int optname,
			  char __user *optval, int optlen)
{
	int opt;
	struct sock *sk = sock->sk;
	int rc = -ENOPROTOOPT;

	if (level != SOL_X25 || optname != X25_QBITINCL)
		goto out;

	rc = -EINVAL;
	if (optlen < sizeof(int))
		goto out;

	rc = -EFAULT;
	if (get_user(opt, (int __user *)optval))
		goto out;

	x25_sk(sk)->qbitincl = !!opt;
	rc = 0;
out:
	return rc;
}

static int x25_getsockopt(struct socket *sock, int level, int optname,
			  char __user *optval, int __user *optlen)
{
	struct sock *sk = sock->sk;
	int val, len, rc = -ENOPROTOOPT;
	
	if (level != SOL_X25 || optname != X25_QBITINCL)
		goto out;

	rc = -EFAULT;
	if (get_user(len, optlen))
		goto out;

	len = min_t(unsigned int, len, sizeof(int));

	rc = -EINVAL;
	if (len < 0)
		goto out;
		
	rc = -EFAULT;
	if (put_user(len, optlen))
		goto out;

	val = x25_sk(sk)->qbitincl;
	rc = copy_to_user(optval, &val, len) ? -EFAULT : 0;
out:
	return rc;
}

static int x25_listen(struct socket *sock, int backlog)
{
	struct sock *sk = sock->sk;
	int rc = -EOPNOTSUPP;

	if (sk->sk_state != TCP_LISTEN) {
		memset(&x25_sk(sk)->dest_addr, 0, X25_ADDR_LEN);
		sk->sk_max_ack_backlog = backlog;
		sk->sk_state           = TCP_LISTEN;
		rc = 0;
	}

	return rc;
}

static struct sock *x25_alloc_socket(void)
{
	struct x25_opt *x25;
	struct sock *sk = sk_alloc(AF_X25, GFP_ATOMIC, 1, NULL);

	if (!sk)
		goto out;

	x25 = sk->sk_protinfo = kmalloc(sizeof(*x25), GFP_ATOMIC);
	if (!x25)
		goto frees;

	memset(x25, 0, sizeof(*x25));

	x25->sk = sk;

	sock_init_data(NULL, sk);
	sk_set_owner(sk, THIS_MODULE);

	skb_queue_head_init(&x25->ack_queue);
	skb_queue_head_init(&x25->fragment_queue);
	skb_queue_head_init(&x25->interrupt_in_queue);
	skb_queue_head_init(&x25->interrupt_out_queue);
out:
	return sk;
frees:
	sk_free(sk);
	sk = NULL;
	goto out;
}

void x25_init_timers(struct sock *sk);

static int x25_create(struct socket *sock, int protocol)
{
	struct sock *sk;
	struct x25_opt *x25;
	int rc = -ESOCKTNOSUPPORT;

	if (sock->type != SOCK_SEQPACKET || protocol)
		goto out;

	rc = -ENOMEM;
	if ((sk = x25_alloc_socket()) == NULL)
		goto out;

	x25 = x25_sk(sk);

	sock_init_data(sock, sk);
	sk_set_owner(sk, THIS_MODULE);

	x25_init_timers(sk);

	sock->ops    = &x25_proto_ops;
	sk->sk_protocol = protocol;
	sk->sk_backlog_rcv = x25_backlog_rcv;

	x25->t21   = sysctl_x25_call_request_timeout;
	x25->t22   = sysctl_x25_reset_request_timeout;
	x25->t23   = sysctl_x25_clear_request_timeout;
	x25->t2    = sysctl_x25_ack_holdback_timeout;
	x25->state = X25_STATE_0;

	x25->facilities.winsize_in  = X25_DEFAULT_WINDOW_SIZE;
	x25->facilities.winsize_out = X25_DEFAULT_WINDOW_SIZE;
	x25->facilities.pacsize_in  = X25_DEFAULT_PACKET_SIZE;
	x25->facilities.pacsize_out = X25_DEFAULT_PACKET_SIZE;
	x25->facilities.throughput  = X25_DEFAULT_THROUGHPUT;
	x25->facilities.reverse     = X25_DEFAULT_REVERSE;
	rc = 0;
out:
	return rc;
}

static struct sock *x25_make_new(struct sock *osk)
{
	struct sock *sk = NULL;
	struct x25_opt *x25, *ox25;

	if (osk->sk_type != SOCK_SEQPACKET)
		goto out;

	if ((sk = x25_alloc_socket()) == NULL)
		goto out;

	x25 = x25_sk(sk);

	sk->sk_type        = osk->sk_type;
	sk->sk_socket      = osk->sk_socket;
	sk->sk_priority    = osk->sk_priority;
	sk->sk_protocol    = osk->sk_protocol;
	sk->sk_rcvbuf      = osk->sk_rcvbuf;
	sk->sk_sndbuf      = osk->sk_sndbuf;
	sk->sk_debug       = osk->sk_debug;
	sk->sk_state       = TCP_ESTABLISHED;
	sk->sk_sleep       = osk->sk_sleep;
	sk->sk_zapped      = osk->sk_zapped;
	sk->sk_backlog_rcv = osk->sk_backlog_rcv;

	ox25 = x25_sk(osk);
	x25->t21        = ox25->t21;
	x25->t22        = ox25->t22;
	x25->t23        = ox25->t23;
	x25->t2         = ox25->t2;
	x25->facilities = ox25->facilities;
	x25->qbitincl   = ox25->qbitincl;

	x25_init_timers(sk);
out:
	return sk;
}

static int x25_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct x25_opt *x25;

	if (!sk)
		goto out;

	x25 = x25_sk(sk);

	switch (x25->state) {

		case X25_STATE_0:
		case X25_STATE_2:
			x25_disconnect(sk, 0, 0, 0);
			x25_destroy_socket(sk);
			goto out;

		case X25_STATE_1:
		case X25_STATE_3:
		case X25_STATE_4:
			x25_clear_queues(sk);
			x25_write_internal(sk, X25_CLEAR_REQUEST);
			x25_start_t23timer(sk);
			x25->state = X25_STATE_2;
			sk->sk_state	= TCP_CLOSE;
			sk->sk_shutdown	|= SEND_SHUTDOWN;
			sk->sk_state_change(sk);
			sock_set_flag(sk, SOCK_DEAD);
			sock_set_flag(sk, SOCK_DESTROY);
			break;
	}

	sock->sk	= NULL;	
	sk->sk_socket	= NULL;	/* Not used, but we should do this */
out:
	return 0;
}

static int x25_bind(struct socket *sock, struct sockaddr *uaddr, int addr_len)
{
	struct sock *sk = sock->sk;
	struct sockaddr_x25 *addr = (struct sockaddr_x25 *)uaddr;

	if (!sk->sk_zapped ||
	    addr_len != sizeof(struct sockaddr_x25) ||
	    addr->sx25_family != AF_X25)
		return -EINVAL;

	x25_sk(sk)->source_addr = addr->sx25_addr;
	x25_insert_socket(sk);
	sk->sk_zapped = 0;
	SOCK_DEBUG(sk, "x25_bind: socket is bound\n");

	return 0;
}

static int x25_wait_for_connection_establishment(struct sock *sk)
{
	DECLARE_WAITQUEUE(wait, current);
        int rc;

	add_wait_queue_exclusive(sk->sk_sleep, &wait);
	for (;;) {
		__set_current_state(TASK_INTERRUPTIBLE);
		rc = -ERESTARTSYS;
		if (signal_pending(current))
			break;
		rc = sock_error(sk);
		if (rc) {
			sk->sk_socket->state = SS_UNCONNECTED;
			break;
		}
		rc = 0;
		if (sk->sk_state != TCP_ESTABLISHED) {
			release_sock(sk);
			schedule();
			lock_sock(sk);
		} else
			break;
	}
	__set_current_state(TASK_RUNNING);
	remove_wait_queue(sk->sk_sleep, &wait);
	return rc;
}

static int x25_connect(struct socket *sock, struct sockaddr *uaddr,
		       int addr_len, int flags)
{
	struct sock *sk = sock->sk;
	struct x25_opt *x25 = x25_sk(sk);
	struct sockaddr_x25 *addr = (struct sockaddr_x25 *)uaddr;
	struct x25_route *rt;
	int rc = 0;

	lock_sock(sk);
	if (sk->sk_state == TCP_ESTABLISHED && sock->state == SS_CONNECTING) {
		sock->state = SS_CONNECTED;
		goto out; /* Connect completed during a ERESTARTSYS event */
	}

	rc = -ECONNREFUSED;
	if (sk->sk_state == TCP_CLOSE && sock->state == SS_CONNECTING) {
		sock->state = SS_UNCONNECTED;
		goto out;
	}

	rc = -EISCONN;	/* No reconnect on a seqpacket socket */
	if (sk->sk_state == TCP_ESTABLISHED)
		goto out;

	sk->sk_state   = TCP_CLOSE;	
	sock->state = SS_UNCONNECTED;

	rc = -EINVAL;
	if (addr_len != sizeof(struct sockaddr_x25) ||
	    addr->sx25_family != AF_X25)
		goto out;

	rc = -ENETUNREACH;
	rt = x25_get_route(&addr->sx25_addr);
	if (!rt)
		goto out;

	x25->neighbour = x25_get_neigh(rt->dev);
	if (!x25->neighbour)
		goto out_put_route;

	x25_limit_facilities(&x25->facilities, x25->neighbour);

	x25->lci = x25_new_lci(x25->neighbour);
	if (!x25->lci)
		goto out_put_neigh;

	rc = -EINVAL;
	if (sk->sk_zapped) /* Must bind first - autobinding does not work */
		goto out_put_neigh;

	if (!strcmp(x25->source_addr.x25_addr, null_x25_address.x25_addr))
		memset(&x25->source_addr, '\0', X25_ADDR_LEN);

	x25->dest_addr = addr->sx25_addr;

	/* Move to connecting socket, start sending Connect Requests */
	sock->state   = SS_CONNECTING;
	sk->sk_state  = TCP_SYN_SENT;

	x25->state = X25_STATE_1;

	x25_write_internal(sk, X25_CALL_REQUEST);

	x25_start_heartbeat(sk);
	x25_start_t21timer(sk);

	/* Now the loop */
	rc = -EINPROGRESS;
	if (sk->sk_state != TCP_ESTABLISHED && (flags & O_NONBLOCK))
		goto out_put_neigh;

	rc = x25_wait_for_connection_establishment(sk);
	if (rc)
		goto out_put_neigh;

	sock->state = SS_CONNECTED;
	rc = 0;
out_put_neigh:
	if (rc)
		x25_neigh_put(x25->neighbour);
out_put_route:
	x25_route_put(rt);
out:
	release_sock(sk);
	return rc;
}

static int x25_wait_for_data(struct sock *sk, int timeout)
{
	DECLARE_WAITQUEUE(wait, current);
	int rc = 0;

	add_wait_queue_exclusive(sk->sk_sleep, &wait);
	for (;;) {
		__set_current_state(TASK_INTERRUPTIBLE);
		if (sk->sk_shutdown & RCV_SHUTDOWN)
			break;
		rc = -ERESTARTSYS;
		if (signal_pending(current))
			break;
		rc = -EAGAIN;
		if (!timeout)
			break;
		rc = 0;
		if (skb_queue_empty(&sk->sk_receive_queue)) {
			release_sock(sk);
			timeout = schedule_timeout(timeout);
			lock_sock(sk);
		} else
			break;
	}
	__set_current_state(TASK_RUNNING);
	remove_wait_queue(sk->sk_sleep, &wait);
	return rc;
}
	
static int x25_accept(struct socket *sock, struct socket *newsock, int flags)
{
	struct sock *sk = sock->sk;
	struct sock *newsk;
	struct sk_buff *skb;
	int rc = -EINVAL;

	if (!sk || sk->sk_state != TCP_LISTEN)
		goto out;

	rc = -EOPNOTSUPP;
	if (sk->sk_type != SOCK_SEQPACKET)
		goto out;

	lock_sock(sk);
	rc = x25_wait_for_data(sk, sk->sk_rcvtimeo);
	if (rc)
		goto out2;
	skb = skb_dequeue(&sk->sk_receive_queue);
	rc = -EINVAL;
	if (!skb->sk)
		goto out2;
	newsk		 = skb->sk;
	newsk->sk_pair   = NULL;
	newsk->sk_socket = newsock;
	newsk->sk_sleep  = &newsock->wait;

	/* Now attach up the new socket */
	skb->sk = NULL;
	kfree_skb(skb);
	sk->sk_ack_backlog--;
	newsock->sk    = newsk;
	newsock->state = SS_CONNECTED;
	rc = 0;
out2:
	release_sock(sk);
out:
	return rc;
}

static int x25_getname(struct socket *sock, struct sockaddr *uaddr,
		       int *uaddr_len, int peer)
{
	struct sockaddr_x25 *sx25 = (struct sockaddr_x25 *)uaddr;
	struct sock *sk = sock->sk;
	struct x25_opt *x25 = x25_sk(sk);

	if (peer) {
		if (sk->sk_state != TCP_ESTABLISHED)
			return -ENOTCONN;
		sx25->sx25_addr = x25->dest_addr;
	} else
		sx25->sx25_addr = x25->source_addr;

	sx25->sx25_family = AF_X25;
	*uaddr_len = sizeof(*sx25);

	return 0;
}
 
int x25_rx_call_request(struct sk_buff *skb, struct x25_neigh *nb,
			unsigned int lci)
{
	struct sock *sk;
	struct sock *make;
	struct x25_opt *makex25;
	struct x25_address source_addr, dest_addr;
	struct x25_facilities facilities;
	int len, rc;

	/*
	 *	Remove the LCI and frame type.
	 */
	skb_pull(skb, X25_STD_MIN_LEN);

	/*
	 *	Extract the X.25 addresses and convert them to ASCII strings,
	 *	and remove them.
	 */
	skb_pull(skb, x25_addr_ntoa(skb->data, &source_addr, &dest_addr));

	/*
	 *	Find a listener for the particular address.
	 */
	sk = x25_find_listener(&source_addr);

	/*
	 *	We can't accept the Call Request.
	 */
	if (!sk || sk->sk_ack_backlog == sk->sk_max_ack_backlog)
		goto out_clear_request;

	/*
	 *	Try to reach a compromise on the requested facilities.
	 */
	if ((len = x25_negotiate_facilities(skb, sk, &facilities)) == -1)
		goto out_sock_put;

	/*
	 * current neighbour/link might impose additional limits
	 * on certain facilties
	 */

	x25_limit_facilities(&facilities, nb);

	/*
	 *	Try to create a new socket.
	 */
	make = x25_make_new(sk);
	if (!make)
		goto out_sock_put;

	/*
	 *	Remove the facilities, leaving any Call User Data.
	 */
	skb_pull(skb, len);

	skb->sk     = make;
	make->sk_state = TCP_ESTABLISHED;

	makex25 = x25_sk(make);
	makex25->lci           = lci;
	makex25->dest_addr     = dest_addr;
	makex25->source_addr   = source_addr;
	makex25->neighbour     = nb;
	makex25->facilities    = facilities;
	makex25->vc_facil_mask = x25_sk(sk)->vc_facil_mask;

	x25_write_internal(make, X25_CALL_ACCEPTED);

	/*
	 *	Incoming Call User Data.
	 */
	if (skb->len >= 0) {
		memcpy(makex25->calluserdata.cuddata, skb->data, skb->len);
		makex25->calluserdata.cudlength = skb->len;
	}

	makex25->state = X25_STATE_3;

	sk->sk_ack_backlog++;
	make->sk_pair = sk;

	x25_insert_socket(make);

	skb_queue_head(&sk->sk_receive_queue, skb);

	x25_start_heartbeat(make);

	if (!sock_flag(sk, SOCK_DEAD))
		sk->sk_data_ready(sk, skb->len);
	rc = 1;
	sock_put(sk);
out:
	return rc;
out_sock_put:
	sock_put(sk);
out_clear_request:
	rc = 0;
	x25_transmit_clear_request(nb, lci, 0x01);
	goto out;
}

static int x25_sendmsg(struct kiocb *iocb, struct socket *sock,
		       struct msghdr *msg, size_t len)
{
	struct sock *sk = sock->sk;
	struct x25_opt *x25 = x25_sk(sk);
	struct sockaddr_x25 *usx25 = (struct sockaddr_x25 *)msg->msg_name;
	struct sockaddr_x25 sx25;
	struct sk_buff *skb;
	unsigned char *asmptr;
	int noblock = msg->msg_flags & MSG_DONTWAIT;
	size_t size;
	int qbit = 0, rc = -EINVAL;

	if (msg->msg_flags & ~(MSG_DONTWAIT|MSG_OOB|MSG_EOR|MSG_CMSG_COMPAT))
		goto out;

	/* we currently don't support segmented records at the user interface */
	if (!(msg->msg_flags & (MSG_EOR|MSG_OOB)))
		goto out;

	rc = -EADDRNOTAVAIL;
	if (sk->sk_zapped)
		goto out;

	rc = -EPIPE;
	if (sk->sk_shutdown & SEND_SHUTDOWN) {
		send_sig(SIGPIPE, current, 0);
		goto out;
	}

	rc = -ENETUNREACH;
	if (!x25->neighbour)
		goto out;

	if (usx25) {
		rc = -EINVAL;
		if (msg->msg_namelen < sizeof(sx25))
			goto out;
		memcpy(&sx25, usx25, sizeof(sx25));
		rc = -EISCONN;
		if (strcmp(x25->dest_addr.x25_addr, sx25.sx25_addr.x25_addr))
			goto out;
		rc = -EINVAL;
		if (sx25.sx25_family != AF_X25)
			goto out;
	} else {
		/*
		 *	FIXME 1003.1g - if the socket is like this because
		 *	it has become closed (not started closed) we ought
		 *	to SIGPIPE, EPIPE;
		 */
		rc = -ENOTCONN;
		if (sk->sk_state != TCP_ESTABLISHED)
			goto out;

		sx25.sx25_family = AF_X25;
		sx25.sx25_addr   = x25->dest_addr;
	}

	SOCK_DEBUG(sk, "x25_sendmsg: sendto: Addresses built.\n");

	/* Build a packet */
	SOCK_DEBUG(sk, "x25_sendmsg: sendto: building packet.\n");

	if ((msg->msg_flags & MSG_OOB) && len > 32)
		len = 32;

	size = len + X25_MAX_L2_LEN + X25_EXT_MIN_LEN;

	skb = sock_alloc_send_skb(sk, size, noblock, &rc);
	if (!skb)
		goto out;
	X25_SKB_CB(skb)->flags = msg->msg_flags;

	skb_reserve(skb, X25_MAX_L2_LEN + X25_EXT_MIN_LEN);

	/*
	 *	Put the data on the end
	 */
	SOCK_DEBUG(sk, "x25_sendmsg: Copying user data\n");

	asmptr = skb->h.raw = skb_put(skb, len);

	rc = memcpy_fromiovec(asmptr, msg->msg_iov, len);
	if (rc)
		goto out_kfree_skb;

	/*
	 *	If the Q BIT Include socket option is in force, the first
	 *	byte of the user data is the logical value of the Q Bit.
	 */
	if (x25->qbitincl) {
		qbit = skb->data[0];
		skb_pull(skb, 1);
	}

	/*
	 *	Push down the X.25 header
	 */
	SOCK_DEBUG(sk, "x25_sendmsg: Building X.25 Header.\n");

	if (msg->msg_flags & MSG_OOB) {
		if (x25->neighbour->extended) {
			asmptr    = skb_push(skb, X25_STD_MIN_LEN);
			*asmptr++ = ((x25->lci >> 8) & 0x0F) | X25_GFI_EXTSEQ;
			*asmptr++ = (x25->lci >> 0) & 0xFF;
			*asmptr++ = X25_INTERRUPT;
		} else {
			asmptr    = skb_push(skb, X25_STD_MIN_LEN);
			*asmptr++ = ((x25->lci >> 8) & 0x0F) | X25_GFI_STDSEQ;
			*asmptr++ = (x25->lci >> 0) & 0xFF;
			*asmptr++ = X25_INTERRUPT;
		}
	} else {
		if (x25->neighbour->extended) {
			/* Build an Extended X.25 header */
			asmptr    = skb_push(skb, X25_EXT_MIN_LEN);
			*asmptr++ = ((x25->lci >> 8) & 0x0F) | X25_GFI_EXTSEQ;
			*asmptr++ = (x25->lci >> 0) & 0xFF;
			*asmptr++ = X25_DATA;
			*asmptr++ = X25_DATA;
		} else {
			/* Build an Standard X.25 header */
			asmptr    = skb_push(skb, X25_STD_MIN_LEN);
			*asmptr++ = ((x25->lci >> 8) & 0x0F) | X25_GFI_STDSEQ;
			*asmptr++ = (x25->lci >> 0) & 0xFF;
			*asmptr++ = X25_DATA;
		}

		if (qbit)
			skb->data[0] |= X25_Q_BIT;
	}

	SOCK_DEBUG(sk, "x25_sendmsg: Built header.\n");
	SOCK_DEBUG(sk, "x25_sendmsg: Transmitting buffer\n");

	rc = -ENOTCONN;
	if (sk->sk_state != TCP_ESTABLISHED)
		goto out_kfree_skb;

	if (msg->msg_flags & MSG_OOB)
		skb_queue_tail(&x25->interrupt_out_queue, skb);
	else {
	        len = x25_output(sk, skb);
		if (len < 0)
			kfree_skb(skb);
		else if (x25->qbitincl)
			len++;
	}

	/*
	 * lock_sock() is currently only used to serialize this x25_kick()
	 * against input-driven x25_kick() calls. It currently only blocks
	 * incoming packets for this socket and does not protect against
	 * any other socket state changes and is not called from anywhere
	 * else. As x25_kick() cannot block and as long as all socket
	 * operations are BKL-wrapped, we don't need take to care about
	 * purging the backlog queue in x25_release().
	 *
	 * Using lock_sock() to protect all socket operations entirely
	 * (and making the whole x25 stack SMP aware) unfortunately would
	 * require major changes to {send,recv}msg and skb allocation methods.
	 * -> 2.5 ;)
	 */
	lock_sock(sk);
	x25_kick(sk);
	release_sock(sk);
	rc = len;
out:
	return rc;
out_kfree_skb:
	kfree_skb(skb);
	goto out;
}


static int x25_recvmsg(struct kiocb *iocb, struct socket *sock,
		       struct msghdr *msg, size_t size,
		       int flags)
{
	struct sock *sk = sock->sk;
	struct x25_opt *x25 = x25_sk(sk);
	struct sockaddr_x25 *sx25 = (struct sockaddr_x25 *)msg->msg_name;
	size_t copied;
	int qbit;
	struct sk_buff *skb;
	unsigned char *asmptr;
	int rc = -ENOTCONN;

	/*
	 * This works for seqpacket too. The receiver has ordered the queue for
	 * us! We do one quick check first though
	 */
	if (sk->sk_state != TCP_ESTABLISHED)
		goto out;

	if (flags & MSG_OOB) {
		rc = -EINVAL;
		if (sock_flag(sk, SOCK_URGINLINE) ||
		    !skb_peek(&x25->interrupt_in_queue))
			goto out;

		skb = skb_dequeue(&x25->interrupt_in_queue);

		skb_pull(skb, X25_STD_MIN_LEN);

		/*
		 *	No Q bit information on Interrupt data.
		 */
		if (x25->qbitincl) {
			asmptr  = skb_push(skb, 1);
			*asmptr = 0x00;
		}

		msg->msg_flags |= MSG_OOB;
	} else {
		/* Now we can treat all alike */
		skb = skb_recv_datagram(sk, flags & ~MSG_DONTWAIT,
					flags & MSG_DONTWAIT, &rc);
		if (!skb)
			goto out;

		qbit = (skb->data[0] & X25_Q_BIT) == X25_Q_BIT;

		skb_pull(skb, x25->neighbour->extended ?
				X25_EXT_MIN_LEN : X25_STD_MIN_LEN);

		if (x25->qbitincl) {
			asmptr  = skb_push(skb, 1);
			*asmptr = qbit;
		}
	}

	skb->h.raw = skb->data;

	copied = skb->len;

	if (copied > size) {
		copied = size;
		msg->msg_flags |= MSG_TRUNC;
	}

	/* Currently, each datagram always contains a complete record */ 
	msg->msg_flags |= MSG_EOR;

	rc = skb_copy_datagram_iovec(skb, 0, msg->msg_iov, copied);
	if (rc)
		goto out_free_dgram;

	if (sx25) {
		sx25->sx25_family = AF_X25;
		sx25->sx25_addr   = x25->dest_addr;
	}

	msg->msg_namelen = sizeof(struct sockaddr_x25);

	lock_sock(sk);
	x25_check_rbuf(sk);
	release_sock(sk);
	rc = copied;
out_free_dgram:
	skb_free_datagram(sk, skb);
out:
	return rc;
}


static int x25_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	struct sock *sk = sock->sk;
	struct x25_opt *x25 = x25_sk(sk);
	void __user *argp = (void __user *)arg;
	int rc;

	switch (cmd) {
		case TIOCOUTQ: {
			int amount = sk->sk_sndbuf -
				     atomic_read(&sk->sk_wmem_alloc);
			if (amount < 0)
				amount = 0;
			rc = put_user(amount, (unsigned int __user *)argp);
			break;
		}

		case TIOCINQ: {
			struct sk_buff *skb;
			int amount = 0;
			/*
			 * These two are safe on a single CPU system as
			 * only user tasks fiddle here
			 */
			if ((skb = skb_peek(&sk->sk_receive_queue)) != NULL)
				amount = skb->len;
			rc = put_user(amount, (unsigned int __user *)argp);
			break;
		}

		case SIOCGSTAMP:
			rc = -EINVAL;
			if (sk)
				rc = sock_get_timestamp(sk, 
						(struct timeval __user *)argp); 
			break;
		case SIOCGIFADDR:
		case SIOCSIFADDR:
		case SIOCGIFDSTADDR:
		case SIOCSIFDSTADDR:
		case SIOCGIFBRDADDR:
		case SIOCSIFBRDADDR:
		case SIOCGIFNETMASK:
		case SIOCSIFNETMASK:
		case SIOCGIFMETRIC:
		case SIOCSIFMETRIC:
			rc = -EINVAL;
			break;
		case SIOCADDRT:
		case SIOCDELRT:
			rc = -EPERM;
			if (!capable(CAP_NET_ADMIN))
				break;
			rc = x25_route_ioctl(cmd, argp);
			break;
		case SIOCX25GSUBSCRIP:
			rc = x25_subscr_ioctl(cmd, argp);
			break;
		case SIOCX25SSUBSCRIP:
			rc = -EPERM;
			if (!capable(CAP_NET_ADMIN))
				break;
			rc = x25_subscr_ioctl(cmd, argp);
			break;
		case SIOCX25GFACILITIES: {
			struct x25_facilities fac = x25->facilities;
			rc = copy_to_user(argp, &fac,
					  sizeof(fac)) ? -EFAULT : 0;
			break;
		}

		case SIOCX25SFACILITIES: {
			struct x25_facilities facilities;
			rc = -EFAULT;
			if (copy_from_user(&facilities, argp,
					   sizeof(facilities)))
				break;
			rc = -EINVAL;
			if (sk->sk_state != TCP_LISTEN &&
			    sk->sk_state != TCP_CLOSE)
				break;
			if (facilities.pacsize_in < X25_PS16 ||
			    facilities.pacsize_in > X25_PS4096)
				break;
			if (facilities.pacsize_out < X25_PS16 ||
			    facilities.pacsize_out > X25_PS4096)
				break;
			if (facilities.winsize_in < 1 ||
			    facilities.winsize_in > 127)
				break;
			if (facilities.throughput < 0x03 ||
			    facilities.throughput > 0xDD)
				break;
			if (facilities.reverse && facilities.reverse != 1)
				break;
			x25->facilities = facilities;
			rc = 0;
			break;
		}

		case SIOCX25GCALLUSERDATA: {
			struct x25_calluserdata cud = x25->calluserdata;
			rc = copy_to_user(argp, &cud,
					  sizeof(cud)) ? -EFAULT : 0;
			break;
		}

		case SIOCX25SCALLUSERDATA: {
			struct x25_calluserdata calluserdata;

			rc = -EFAULT;
			if (copy_from_user(&calluserdata, argp,
					   sizeof(calluserdata)))
				break;
			rc = -EINVAL;
			if (calluserdata.cudlength > X25_MAX_CUD_LEN)
				break;
			x25->calluserdata = calluserdata;
			rc = 0;
			break;
		}

		case SIOCX25GCAUSEDIAG: {
			struct x25_causediag causediag;
			causediag = x25->causediag;
			rc = copy_to_user(argp, &causediag,
					  sizeof(causediag)) ? -EFAULT : 0;
			break;
		}

 		default:
			rc = dev_ioctl(cmd, argp);
			break;
	}

	return rc;
}

struct net_proto_family x25_family_ops = {
	.family =	AF_X25,
	.create =	x25_create,
	.owner	=	THIS_MODULE,
};

static struct proto_ops SOCKOPS_WRAPPED(x25_proto_ops) = {
	.family =	AF_X25,
	.owner =	THIS_MODULE,
	.release =	x25_release,
	.bind =		x25_bind,
	.connect =	x25_connect,
	.socketpair =	sock_no_socketpair,
	.accept =	x25_accept,
	.getname =	x25_getname,
	.poll =		datagram_poll,
	.ioctl =	x25_ioctl,
	.listen =	x25_listen,
	.shutdown =	sock_no_shutdown,
	.setsockopt =	x25_setsockopt,
	.getsockopt =	x25_getsockopt,
	.sendmsg =	x25_sendmsg,
	.recvmsg =	x25_recvmsg,
	.mmap =		sock_no_mmap,
	.sendpage =	sock_no_sendpage,
};

#include <linux/smp_lock.h>
SOCKOPS_WRAP(x25_proto, AF_X25);

static struct packet_type x25_packet_type = {
	.type =	__constant_htons(ETH_P_X25),
	.func =	x25_lapb_receive_frame,
};

struct notifier_block x25_dev_notifier = {
	.notifier_call = x25_device_event,
};

void x25_kill_by_neigh(struct x25_neigh *nb)
{
	struct sock *s;
	struct hlist_node *node;

	write_lock_bh(&x25_list_lock);

	sk_for_each(s, node, &x25_list)
		if (x25_sk(s)->neighbour == nb)
			x25_disconnect(s, ENETUNREACH, 0, 0);

	write_unlock_bh(&x25_list_lock);
}

static int __init x25_init(void)
{
	sock_register(&x25_family_ops);

	dev_add_pack(&x25_packet_type);

	register_netdevice_notifier(&x25_dev_notifier);

	printk(KERN_INFO "X.25 for Linux. Version 0.2 for Linux 2.1.15\n");

#ifdef CONFIG_SYSCTL
	x25_register_sysctl();
#endif
	x25_proc_init();
	return 0;
}
module_init(x25_init);

static void __exit x25_exit(void)
{
	x25_proc_exit();
	x25_link_free();
	x25_route_free();

#ifdef CONFIG_SYSCTL
	x25_unregister_sysctl();
#endif

	unregister_netdevice_notifier(&x25_dev_notifier);

	dev_remove_pack(&x25_packet_type);

	sock_unregister(AF_X25);
}
module_exit(x25_exit);

MODULE_AUTHOR("Jonathan Naylor <g4klx@g4klx.demon.co.uk>");
MODULE_DESCRIPTION("The X.25 Packet Layer network layer protocol");
MODULE_LICENSE("GPL");
MODULE_ALIAS_NETPROTO(PF_X25);
