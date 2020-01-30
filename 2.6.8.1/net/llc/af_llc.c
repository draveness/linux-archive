/*
 * af_llc.c - LLC User Interface SAPs
 * Description:
 *   Functions in this module are implementation of socket based llc
 *   communications for the Linux operating system. Support of llc class
 *   one and class two is provided via SOCK_DGRAM and SOCK_STREAM
 *   respectively.
 *
 *   An llc2 connection is (mac + sap), only one llc2 sap connection
 *   is allowed per mac. Though one sap may have multiple mac + sap
 *   connections.
 *
 * Copyright (c) 2001 by Jay Schulist <jschlst@samba.org>
 *		 2002-2003 by Arnaldo Carvalho de Melo <acme@conectiva.com.br>
 *
 * This program can be redistributed or modified under the terms of the
 * GNU General Public License as published by the Free Software Foundation.
 * This program is distributed without any warranty or implied warranty
 * of merchantability or fitness for a particular purpose.
 *
 * See the GNU General Public License for more details.
 */
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/tcp.h>
#include <linux/rtnetlink.h>
#include <linux/init.h>
#include <net/llc.h>
#include <net/llc_sap.h>
#include <net/llc_pdu.h>
#include <net/llc_conn.h>

/* remember: uninitialized global data is zeroed because its in .bss */
static u16 llc_ui_sap_last_autoport = LLC_SAP_DYN_START;
static u16 llc_ui_sap_link_no_max[256];
static struct sockaddr_llc llc_ui_addrnull;
static struct proto_ops llc_ui_ops;

static int llc_ui_wait_for_conn(struct sock *sk, int timeout);
static int llc_ui_wait_for_disc(struct sock *sk, int timeout);
static int llc_ui_wait_for_data(struct sock *sk, int timeout);
static int llc_ui_wait_for_busy_core(struct sock *sk, int timeout);

#if 0
#define dprintk(args...) printk(KERN_DEBUG args)
#else
#define dprintk(args...)
#endif

/**
 *	llc_ui_next_link_no - return the next unused link number for a sap
 *	@sap: Address of sap to get link number from.
 *
 *	Return the next unused link number for a given sap.
 */
static __inline__ u16 llc_ui_next_link_no(int sap)
{
	return llc_ui_sap_link_no_max[sap]++;
}

/**
 *	llc_proto_type - return eth protocol for ARP header type
 *	@arphrd: ARP header type.
 *
 *	Given an ARP header type return the corresponding ethernet protocol.
 */
static __inline__ u16 llc_proto_type(u16 arphrd)
{
	return arphrd == ARPHRD_IEEE802_TR ?
		         htons(ETH_P_TR_802_2) : htons(ETH_P_802_2);
}

/**
 *	llc_ui_addr_null - determines if a address structure is null
 *	@addr: Address to test if null.
 */
static __inline__ u8 llc_ui_addr_null(struct sockaddr_llc *addr)
{
	return !memcmp(addr, &llc_ui_addrnull, sizeof(*addr));
}

/**
 *	llc_ui_header_len - return length of llc header based on operation
 *	@sk: Socket which contains a valid llc socket type.
 *	@addr: Complete sockaddr_llc structure received from the user.
 *
 *	Provide the length of the llc header depending on what kind of
 *	operation the user would like to perform and the type of socket.
 *	Returns the correct llc header length.
 */
static __inline__ u8 llc_ui_header_len(struct sock *sk,
				       struct sockaddr_llc *addr)
{
	u8 rc = LLC_PDU_LEN_U;

	if (addr->sllc_test || addr->sllc_xid)
		rc = LLC_PDU_LEN_U;
	else if (sk->sk_type == SOCK_STREAM)
		rc = LLC_PDU_LEN_I;
	return rc;
}

/**
 *	llc_ui_send_data - send data via reliable llc2 connection
 *	@sk: Connection the socket is using.
 *	@skb: Data the user wishes to send.
 *	@addr: Source and destination fields provided by the user.
 *	@noblock: can we block waiting for data?
 *
 *	Send data via reliable llc2 connection.
 *	Returns 0 upon success, non-zero if action did not succeed.
 */
static int llc_ui_send_data(struct sock* sk, struct sk_buff *skb, int noblock)
{
	struct llc_opt* llc = llc_sk(sk);
	int rc = 0;

	if (llc_data_accept_state(llc->state) || llc->p_flag) {
		int timeout = sock_sndtimeo(sk, noblock);

		rc = llc_ui_wait_for_busy_core(sk, timeout);
	}
	if (!rc)
		rc = llc_build_and_send_pkt(sk, skb);
	return rc;
}

static void llc_ui_sk_init(struct socket *sock, struct sock *sk)
{
	sk->sk_type	= sock->type;
	sk->sk_sleep	= &sock->wait;
	sk->sk_socket	= sock;
	sock->sk	= sk;
	sock->ops	= &llc_ui_ops;
}

/**
 *	llc_ui_create - alloc and init a new llc_ui socket
 *	@sock: Socket to initialize and attach allocated sk to.
 *	@protocol: Unused.
 *
 *	Allocate and initialize a new llc_ui socket, validate the user wants a
 *	socket type we have available.
 *	Returns 0 upon success, negative upon failure.
 */
static int llc_ui_create(struct socket *sock, int protocol)
{
	struct sock *sk;
	int rc = -ESOCKTNOSUPPORT;

	if (sock->type == SOCK_DGRAM || sock->type == SOCK_STREAM) {
		rc = -ENOMEM;
		sk = llc_sk_alloc(PF_LLC, GFP_KERNEL);
		if (sk) {
			rc = 0;
			llc_ui_sk_init(sock, sk);
		}
	}
	return rc;
}

/**
 *	llc_ui_release - shutdown socket
 *	@sock: Socket to release.
 *
 *	Shutdown and deallocate an existing socket.
 */
static int llc_ui_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct llc_opt *llc;

	if (!sk)
		goto out;
	sock_hold(sk);
	lock_sock(sk);
	llc = llc_sk(sk);
	dprintk("%s: closing local(%02X) remote(%02X)\n", __FUNCTION__,
		llc->laddr.lsap, llc->daddr.lsap);
	if (!llc_send_disc(sk))
		llc_ui_wait_for_disc(sk, sk->sk_rcvtimeo);
	if (!sk->sk_zapped)
		llc_sap_remove_socket(llc->sap, sk);
	release_sock(sk);
	if (llc->sap && hlist_empty(&llc->sap->sk_list.list)) {
		llc_release_sockets(llc->sap);
		llc_sap_close(llc->sap);
	}
	if (llc->dev)
		dev_put(llc->dev);
	sock_put(sk);
	llc_sk_free(sk);
out:
	return 0;
}

/**
 *	llc_ui_autoport - provide dynamically allocate SAP number
 *
 *	Provide the caller with a dynamically allocated SAP number according
 *	to the rules that are set in this function. Returns: 0, upon failure,
 *	SAP number otherwise.
 */
static int llc_ui_autoport(void)
{
	struct llc_sap *sap;
	int i, tries = 0;

	while (tries < LLC_SAP_DYN_TRIES) {
		for (i = llc_ui_sap_last_autoport;
		     i < LLC_SAP_DYN_STOP; i += 2) {
			sap = llc_sap_find(i);
			if (!sap) {
				llc_ui_sap_last_autoport = i + 2;
				goto out;
			}
		}
		llc_ui_sap_last_autoport = LLC_SAP_DYN_START;
		tries++;
	}
	i = 0;
out:
	return i;
}

/**
 *	llc_ui_autobind - Bind a socket to a specific address.
 *	@sk: Socket to bind an address to.
 *	@addr: Address the user wants the socket bound to.
 *
 *	Bind a socket to a specific address. For llc a user is able to bind to
 *	a specific sap only or mac + sap. If the user only specifies a sap and
 *	a null dmac (all zeros) the user is attempting to bind to an entire
 *	sap. This will stop anyone else on the local system from using that
 *	sap.  If someone else has a mac + sap open the bind to null + sap will
 *	fail.
 *	If the user desires to bind to a specific mac + sap, it is possible to
 *	have multiple sap connections via multiple macs.
 *	Bind and autobind for that matter must enforce the correct sap usage
 *	otherwise all hell will break loose.
 *	Returns: 0 upon success, negative otherwise.
 */
static int llc_ui_autobind(struct socket *sock, struct sockaddr_llc *addr)
{
	struct sock *sk = sock->sk;
	struct llc_opt *llc = llc_sk(sk);
	struct llc_sap *sap;
	int rc = -EINVAL;

	if (!sk->sk_zapped)
		goto out;
	rc = -ENODEV;
	llc->dev = dev_getfirstbyhwtype(addr->sllc_arphrd);
	if (!llc->dev)
		goto out;
	rc = -EUSERS;
	llc->laddr.lsap = llc_ui_autoport();
	if (!llc->laddr.lsap)
		goto out;
	rc = -EBUSY; /* some other network layer is using the sap */
	sap = llc_sap_open(llc->laddr.lsap, NULL);
	if (!sap)
		goto out;
	memcpy(llc->laddr.mac, llc->dev->dev_addr, IFHWADDRLEN);
	memcpy(&llc->addr, addr, sizeof(llc->addr));
	/* assign new connection to its SAP */
	llc_sap_add_socket(sap, sk);
	rc = sk->sk_zapped = 0;
out:
	return rc;
}

/**
 *	llc_ui_bind - bind a socket to a specific address.
 *	@sock: Socket to bind an address to.
 *	@uaddr: Address the user wants the socket bound to.
 *	@addrlen: Length of the uaddr structure.
 *
 *	Bind a socket to a specific address. For llc a user is able to bind to
 *	a specific sap only or mac + sap. If the user only specifies a sap and
 *	a null dmac (all zeros) the user is attempting to bind to an entire
 *	sap. This will stop anyone else on the local system from using that
 *	sap. If someone else has a mac + sap open the bind to null + sap will
 *	fail.
 *	If the user desires to bind to a specific mac + sap, it is possible to
 *	have multiple sap connections via multiple macs.
 *	Bind and autobind for that matter must enforce the correct sap usage
 *	otherwise all hell will break loose.
 *	Returns: 0 upon success, negative otherwise.
 */
static int llc_ui_bind(struct socket *sock, struct sockaddr *uaddr, int addrlen)
{
	struct sockaddr_llc *addr = (struct sockaddr_llc *)uaddr;
	struct sock *sk = sock->sk;
	struct llc_opt *llc = llc_sk(sk);
	struct llc_sap *sap;
	int rc = -EINVAL;

	dprintk("%s: binding %02X\n", __FUNCTION__, addr->sllc_sap);
	if (!sk->sk_zapped || addrlen != sizeof(*addr))
		goto out;
	rc = -EAFNOSUPPORT;
	if (addr->sllc_family != AF_LLC)
		goto out;
	if (!addr->sllc_sap) {
		rc = -EUSERS;
		addr->sllc_sap = llc_ui_autoport();
		if (!addr->sllc_sap)
			goto out;
	}
	sap = llc_sap_find(addr->sllc_sap);
	if (!sap) {
		sap = llc_sap_open(addr->sllc_sap, NULL);
		rc = -EBUSY; /* some other network layer is using the sap */
		if (!sap)
			goto out;
	} else {
		struct llc_addr laddr, daddr;
		struct sock *ask;

		memset(&laddr, 0, sizeof(laddr));
		memset(&daddr, 0, sizeof(daddr));
		/*
		 * FIXME: check if the the address is multicast,
		 * 	  only SOCK_DGRAM can do this.
		 */
		memcpy(laddr.mac, addr->sllc_mac, IFHWADDRLEN);
		laddr.lsap = addr->sllc_sap;
		rc = -EADDRINUSE; /* mac + sap clash. */
		ask = llc_lookup_established(sap, &daddr, &laddr);
		if (ask) {
			sock_put(ask);
			goto out;
		}
	}
	llc->laddr.lsap = addr->sllc_sap;
	memcpy(llc->laddr.mac, addr->sllc_mac, IFHWADDRLEN);
	memcpy(&llc->addr, addr, sizeof(llc->addr));
	/* assign new connection to its SAP */
	llc_sap_add_socket(sap, sk);
	rc = sk->sk_zapped = 0;
out:
	return rc;
}

/**
 *	llc_ui_shutdown - shutdown a connect llc2 socket.
 *	@sock: Socket to shutdown.
 *	@how: What part of the socket to shutdown.
 *
 *	Shutdown a connected llc2 socket. Currently this function only supports
 *	shutting down both sends and receives (2), we could probably make this
 *	function such that a user can shutdown only half the connection but not
 *	right now.
 *	Returns: 0 upon success, negative otherwise.
 */
static int llc_ui_shutdown(struct socket *sock, int how)
{
	struct sock *sk = sock->sk;
	int rc = -ENOTCONN;

	lock_sock(sk);
	if (sk->sk_state != TCP_ESTABLISHED)
		goto out;
	rc = -EINVAL;
	if (how != 2)
		goto out;
	rc = llc_send_disc(sk);
	if (!rc)
		rc = llc_ui_wait_for_disc(sk, sk->sk_rcvtimeo);
	/* Wake up anyone sleeping in poll */
	sk->sk_state_change(sk);
out:
	release_sock(sk);
	return rc;
}

/**
 *	llc_ui_connect - Connect to a remote llc2 mac + sap.
 *	@sock: Socket which will be connected to the remote destination.
 *	@uaddr: Remote and possibly the local address of the new connection.
 *	@addrlen: Size of uaddr structure.
 *	@flags: Operational flags specified by the user.
 *
 *	Connect to a remote llc2 mac + sap. The caller must specify the
 *	destination mac and address to connect to. If the user hasn't previously
 *	called bind(2) with a smac the address of the first interface of the
 *	specified arp type will be used.
 *	This function will autobind if user did not previously call bind.
 *	Returns: 0 upon success, negative otherwise.
 */
static int llc_ui_connect(struct socket *sock, struct sockaddr *uaddr,
			  int addrlen, int flags)
{
	struct sock *sk = sock->sk;
	struct llc_opt *llc = llc_sk(sk);
	struct sockaddr_llc *addr = (struct sockaddr_llc *)uaddr;
	struct net_device *dev;
	int rc = -EINVAL;

	lock_sock(sk);
	if (addrlen != sizeof(*addr))
		goto out;
	rc = -EAFNOSUPPORT;
	if (addr->sllc_family != AF_LLC)
		goto out;
	/* bind connection to sap if user hasn't done it. */
	if (sk->sk_zapped) {
		/* bind to sap with null dev, exclusive */
		rc = llc_ui_autobind(sock, addr);
		if (rc)
			goto out;
		llc->daddr.lsap = addr->sllc_sap;
		memcpy(llc->daddr.mac, addr->sllc_mac, IFHWADDRLEN);
	}
	dev = llc->dev;
	if (sk->sk_type != SOCK_STREAM)
		goto out;
	rc = -EALREADY;
	if (sock->state == SS_CONNECTING)
		goto out;
	sock->state = SS_CONNECTING;
	sk->sk_state   = TCP_SYN_SENT;
	llc->link   = llc_ui_next_link_no(llc->sap->laddr.lsap);
	rc = llc_establish_connection(sk, dev->dev_addr,
				      addr->sllc_mac, addr->sllc_sap);
	if (rc) {
		dprintk("%s: llc_ui_send_conn failed :-(\n", __FUNCTION__);
		sock->state  = SS_UNCONNECTED;
		sk->sk_state = TCP_CLOSE;
		goto out;
	}
	rc = llc_ui_wait_for_conn(sk, sk->sk_rcvtimeo);
	if (rc)
		dprintk("%s: llc_ui_wait_for_conn failed=%d\n", __FUNCTION__, rc);
out:
	release_sock(sk);
	return rc;
}

/**
 *	llc_ui_listen - allow a normal socket to accept incoming connections
 *	@sock: Socket to allow incoming connections on.
 *	@backlog: Number of connections to queue.
 *
 *	Allow a normal socket to accept incoming connections.
 *	Returns 0 upon success, negative otherwise.
 */
static int llc_ui_listen(struct socket *sock, int backlog)
{
	struct sock *sk = sock->sk;
	int rc = -EINVAL;

	lock_sock(sk);
	if (sock->state != SS_UNCONNECTED)
		goto out;
	rc = -EOPNOTSUPP;
	if (sk->sk_type != SOCK_STREAM)
		goto out;
	rc = -EAGAIN;
	if (sk->sk_zapped)
		goto out;
	rc = 0;
	if (!(unsigned)backlog)	/* BSDism */
		backlog = 1;
	sk->sk_max_ack_backlog = backlog;
	if (sk->sk_state != TCP_LISTEN) {
		sk->sk_ack_backlog = 0;
		sk->sk_state	   = TCP_LISTEN;
	}
	sk->sk_socket->flags |= __SO_ACCEPTCON;
out:
	release_sock(sk);
	return rc;
}

static int llc_ui_wait_for_disc(struct sock *sk, int timeout)
{
	DECLARE_WAITQUEUE(wait, current);
	int rc;

	add_wait_queue_exclusive(sk->sk_sleep, &wait);
	for (;;) {
		__set_current_state(TASK_INTERRUPTIBLE);
		rc = 0;
		if (sk->sk_state != TCP_CLOSE) {
			release_sock(sk);
			timeout = schedule_timeout(timeout);
			lock_sock(sk);
		} else
			break;
		rc = -ERESTARTSYS;
		if (signal_pending(current))
			break;
		rc = -EAGAIN;
		if (!timeout)
			break;
	}
	__set_current_state(TASK_RUNNING);
	remove_wait_queue(sk->sk_sleep, &wait);
	return rc;
}

static int llc_ui_wait_for_conn(struct sock *sk, int timeout)
{
	DECLARE_WAITQUEUE(wait, current);
	int rc;

	add_wait_queue_exclusive(sk->sk_sleep, &wait);
	for (;;) {
		__set_current_state(TASK_INTERRUPTIBLE);
		rc = -EAGAIN;
		if (sk->sk_state == TCP_CLOSE)
			break;
		rc = 0;
		if (sk->sk_state != TCP_ESTABLISHED) {
			release_sock(sk);
			timeout = schedule_timeout(timeout);
			lock_sock(sk);
		} else
			break;
		rc = -ERESTARTSYS;
		if (signal_pending(current))
			break;
		rc = -EAGAIN;
		if (!timeout)
			break;
	}
	__set_current_state(TASK_RUNNING);
	remove_wait_queue(sk->sk_sleep, &wait);
	return rc;
}

static int llc_ui_wait_for_data(struct sock *sk, int timeout)
{
	DECLARE_WAITQUEUE(wait, current);
	int rc = 0;

	add_wait_queue_exclusive(sk->sk_sleep, &wait);
	for (;;) {
		__set_current_state(TASK_INTERRUPTIBLE);
		if (sk->sk_shutdown & RCV_SHUTDOWN)
			break;
		/*
		 * Well, if we have backlog, try to process it now.
		 */
                if (sk->sk_backlog.tail) {
			release_sock(sk);
			lock_sock(sk);
		}
		rc = 0;
		if (skb_queue_empty(&sk->sk_receive_queue)) {
			release_sock(sk);
			timeout = schedule_timeout(timeout);
			lock_sock(sk);
		} else
			break;
		rc = -ERESTARTSYS;
		if (signal_pending(current))
			break;
		rc = -EAGAIN;
		if (!timeout)
			break;
	}
	__set_current_state(TASK_RUNNING);
	remove_wait_queue(sk->sk_sleep, &wait);
	return rc;
}

static int llc_ui_wait_for_busy_core(struct sock *sk, int timeout)
{
	DECLARE_WAITQUEUE(wait, current);
	struct llc_opt *llc = llc_sk(sk);
	int rc;

	add_wait_queue_exclusive(sk->sk_sleep, &wait);
	for (;;) {
		dprintk("%s: looping...\n", __FUNCTION__);
		__set_current_state(TASK_INTERRUPTIBLE);
		rc = -ENOTCONN;
		if (sk->sk_shutdown & RCV_SHUTDOWN)
			break;
		rc = 0;
		if (llc_data_accept_state(llc->state) || llc->p_flag) {
			release_sock(sk);
			timeout = schedule_timeout(timeout);
			lock_sock(sk);
		} else
			break;
		rc = -ERESTARTSYS;
		if (signal_pending(current))
			break;
		rc = -EAGAIN;
		if (!timeout)
			break;
	}
	__set_current_state(TASK_RUNNING);
	remove_wait_queue(sk->sk_sleep, &wait);
	return rc;
}

/**
 *	llc_ui_accept - accept a new incoming connection.
 *	@sock: Socket which connections arrive on.
 *	@newsock: Socket to move incoming connection to.
 *	@flags: User specified operational flags.
 *
 *	Accept a new incoming connection.
 *	Returns 0 upon success, negative otherwise.
 */
static int llc_ui_accept(struct socket *sock, struct socket *newsock, int flags)
{
	struct sock *sk = sock->sk, *newsk;
	struct llc_opt *llc, *newllc;
	struct sk_buff *skb;
	int rc = -EOPNOTSUPP;

	dprintk("%s: accepting on %02X\n", __FUNCTION__,
	        llc_sk(sk)->laddr.lsap);
	lock_sock(sk);
	if (sk->sk_type != SOCK_STREAM)
		goto out;
	rc = -EINVAL;
	if (sock->state != SS_UNCONNECTED || sk->sk_state != TCP_LISTEN)
		goto out;
	/* wait for a connection to arrive. */
	rc = llc_ui_wait_for_data(sk, sk->sk_rcvtimeo);
	if (rc)
		goto out;
	dprintk("%s: got a new connection on %02X\n", __FUNCTION__,
	        llc_sk(sk)->laddr.lsap);
	skb = skb_dequeue(&sk->sk_receive_queue);
	rc = -EINVAL;
	if (!skb->sk)
		goto frees;
	rc = 0;
	newsk = skb->sk;
	/* attach connection to a new socket. */
	llc_ui_sk_init(newsock, newsk);
	newsk->sk_pair		= NULL;
	newsk->sk_zapped	= 0;
	newsk->sk_state		= TCP_ESTABLISHED;
	newsock->state		= SS_CONNECTED;
	llc			= llc_sk(sk);
	newllc			= llc_sk(newsk);
	memcpy(&newllc->addr, &llc->addr, sizeof(newllc->addr));
	newllc->link = llc_ui_next_link_no(newllc->laddr.lsap);

	/* put original socket back into a clean listen state. */
	sk->sk_state = TCP_LISTEN;
	sk->sk_ack_backlog--;
	skb->sk = NULL;
	dprintk("%s: ok success on %02X, client on %02X\n", __FUNCTION__,
		llc_sk(sk)->addr.sllc_sap, newllc->daddr.lsap);
frees:
	kfree_skb(skb);
out:
	release_sock(sk);
	return rc;
}

/**
 *	llc_ui_recvmsg - copy received data to the socket user.
 *	@sock: Socket to copy data from.
 *	@msg: Various user space related information.
 *	@size: Size of user buffer.
 *	@flags: User specified flags.
 *
 *	Copy received data to the socket user.
 *	Returns non-negative upon success, negative otherwise.
 */
static int llc_ui_recvmsg(struct kiocb *iocb, struct socket *sock,
			  struct msghdr *msg, size_t size, int flags)
{
	struct sock *sk = sock->sk;
	struct sockaddr_llc *uaddr = (struct sockaddr_llc *)msg->msg_name;
	struct sk_buff *skb;
	size_t copied = 0;
	int rc = -ENOMEM, timeout;
	int noblock = flags & MSG_DONTWAIT;

	dprintk("%s: receiving in %02X from %02X\n", __FUNCTION__,
		llc_sk(sk)->laddr.lsap, llc_sk(sk)->daddr.lsap);
	lock_sock(sk);
	timeout = sock_rcvtimeo(sk, noblock);
	rc = llc_ui_wait_for_data(sk, timeout);
	if (rc) {
		dprintk("%s: llc_ui_wait_for_data failed recv "
			"in %02X from %02X\n", __FUNCTION__,
			llc_sk(sk)->laddr.lsap, llc_sk(sk)->daddr.lsap);
		goto out;
	}
	skb = skb_dequeue(&sk->sk_receive_queue);
	if (!skb) /* shutdown */
		goto out;
	copied = skb->len;
	if (copied > size)
		copied = size;
	rc = skb_copy_datagram_iovec(skb, 0, msg->msg_iov, copied);
	if (rc)
		goto dgram_free;
	if (skb->len > copied) {
		skb_pull(skb, copied);
		skb_queue_head(&sk->sk_receive_queue, skb);
	}
	if (uaddr)
		memcpy(uaddr, llc_ui_skb_cb(skb), sizeof(*uaddr));
	msg->msg_namelen = sizeof(*uaddr);
	if (!skb->list) {
dgram_free:
		kfree_skb(skb);
	}
out:
	release_sock(sk);
	return rc ? : copied;
}

/**
 *	llc_ui_sendmsg - Transmit data provided by the socket user.
 *	@sock: Socket to transmit data from.
 *	@msg: Various user related information.
 *	@len: Length of data to transmit.
 *
 *	Transmit data provided by the socket user.
 *	Returns non-negative upon success, negative otherwise.
 */
static int llc_ui_sendmsg(struct kiocb *iocb, struct socket *sock,
			  struct msghdr *msg, size_t len)
{
	struct sock *sk = sock->sk;
	struct llc_opt *llc = llc_sk(sk);
	struct sockaddr_llc *addr = (struct sockaddr_llc *)msg->msg_name;
	int flags = msg->msg_flags;
	int noblock = flags & MSG_DONTWAIT;
	struct net_device *dev;
	struct sk_buff *skb;
	size_t size = 0;
	int rc = -EINVAL, copied = 0, hdrlen;

	dprintk("%s: sending from %02X to %02X\n", __FUNCTION__,
		llc->laddr.lsap, llc->daddr.lsap);
	lock_sock(sk);
	if (addr) {
		if (msg->msg_namelen < sizeof(*addr))
			goto release;
	} else {
		if (llc_ui_addr_null(&llc->addr))
			goto release;
		addr = &llc->addr;
	}
	/* must bind connection to sap if user hasn't done it. */
	if (sk->sk_zapped) {
		/* bind to sap with null dev, exclusive. */
		rc = llc_ui_autobind(sock, addr);
		if (rc)
			goto release;
	}
	dev = llc->dev;
	hdrlen = dev->hard_header_len + llc_ui_header_len(sk, addr);
	size = hdrlen + len;
	if (size > dev->mtu)
		size = dev->mtu;
	copied = size - hdrlen;
	release_sock(sk);
	skb = sock_alloc_send_skb(sk, size, noblock, &rc);
	lock_sock(sk);
	if (!skb)
		goto release;
	skb->sk	      = sk;
	skb->dev      = dev;
	skb->protocol = llc_proto_type(addr->sllc_arphrd);
	skb_reserve(skb, hdrlen); 
	rc = memcpy_fromiovec(skb_put(skb, copied), msg->msg_iov, copied);
	if (rc)
		goto out;
	if (sk->sk_type == SOCK_DGRAM || addr->sllc_ua) {
		llc_build_and_send_ui_pkt(llc->sap, skb, addr->sllc_mac,
					  addr->sllc_sap);
		goto out;
	}
	if (addr->sllc_test) {
		llc_build_and_send_test_pkt(llc->sap, skb, addr->sllc_mac,
					    addr->sllc_sap);
		goto out;
	}
	if (addr->sllc_xid) {
		llc_build_and_send_xid_pkt(llc->sap, skb, addr->sllc_mac,
					   addr->sllc_sap);
		goto out;
	}
	rc = -ENOPROTOOPT;
	if (!(sk->sk_type == SOCK_STREAM && !addr->sllc_ua))
		goto out;
	rc = llc_ui_send_data(sk, skb, noblock);
	if (rc)
		dprintk("%s: llc_ui_send_data failed: %d\n", __FUNCTION__, rc);
out:
	if (rc)
		kfree_skb(skb);
release:
	if (rc)
		dprintk("%s: failed sending from %02X to %02X: %d\n",
			__FUNCTION__, llc->laddr.lsap, llc->daddr.lsap, rc);
	release_sock(sk);
	return rc ? : copied;
}

/**
 *	llc_ui_getname - return the address info of a socket
 *	@sock: Socket to get address of.
 *	@uaddr: Address structure to return information.
 *	@uaddrlen: Length of address structure.
 *	@peer: Does user want local or remote address information.
 *
 *	Return the address information of a socket.
 */
static int llc_ui_getname(struct socket *sock, struct sockaddr *uaddr,
			  int *uaddrlen, int peer)
{
	struct sockaddr_llc sllc;
	struct sock *sk = sock->sk;
	struct llc_opt *llc = llc_sk(sk);
	int rc = 0;

	lock_sock(sk);
	if (sk->sk_zapped)
		goto out;
	*uaddrlen = sizeof(sllc);
	memset(uaddr, 0, *uaddrlen);
	if (peer) {
		rc = -ENOTCONN;
		if (sk->sk_state != TCP_ESTABLISHED)
			goto out;
		if(llc->dev)
			sllc.sllc_arphrd = llc->dev->type;
		sllc.sllc_sap = llc->daddr.lsap;
		memcpy(&sllc.sllc_mac, &llc->daddr.mac, IFHWADDRLEN);
	} else {
		rc = -EINVAL;
		if (!llc->sap)
			goto out;
		sllc.sllc_sap = llc->sap->laddr.lsap;

		if (llc->dev) {
			sllc.sllc_arphrd = llc->dev->type;
			memcpy(&sllc.sllc_mac, &llc->dev->dev_addr,
			       IFHWADDRLEN);
		}
	}
	rc = 0;
	sllc.sllc_family = AF_LLC;
	memcpy(uaddr, &sllc, sizeof(sllc));
out:
	release_sock(sk);
	return rc;
}

/**
 *	llc_ui_ioctl - io controls for PF_LLC
 *	@sock: Socket to get/set info
 *	@cmd: command
 *	@arg: optional argument for cmd
 *
 *	get/set info on llc sockets
 */
static int llc_ui_ioctl(struct socket *sock, unsigned int cmd,
			unsigned long arg)
{
	return dev_ioctl(cmd, (void __user *)arg);
}

/**
 *	llc_ui_setsockopt - set various connection specific parameters.
 *	@sock: Socket to set options on.
 *	@level: Socket level user is requesting operations on.
 *	@optname: Operation name.
 *	@optval User provided operation data.
 *	@optlen: Length of optval.
 *
 *	Set various connection specific parameters.
 */
static int llc_ui_setsockopt(struct socket *sock, int level, int optname,
			     char __user *optval, int optlen)
{
	struct sock *sk = sock->sk;
	struct llc_opt *llc = llc_sk(sk);
	int rc = -EINVAL, opt;

	lock_sock(sk);
	if (level != SOL_LLC || optlen != sizeof(int))
		goto out;
	rc = get_user(opt, (int __user *)optval);
	if (rc)
		goto out;
	rc = -EINVAL;
	switch (optname) {
	case LLC_OPT_RETRY:
		if (opt > LLC_OPT_MAX_RETRY)
			goto out;
		llc->n2 = opt;
		break;
	case LLC_OPT_SIZE:
		if (opt > LLC_OPT_MAX_SIZE)
			goto out;
		llc->n1 = opt;
		break;
	case LLC_OPT_ACK_TMR_EXP:
		if (opt > LLC_OPT_MAX_ACK_TMR_EXP)
			goto out;
		llc->ack_timer.expire = opt;
		break;
	case LLC_OPT_P_TMR_EXP:
		if (opt > LLC_OPT_MAX_P_TMR_EXP)
			goto out;
		llc->pf_cycle_timer.expire = opt;
		break;
	case LLC_OPT_REJ_TMR_EXP:
		if (opt > LLC_OPT_MAX_REJ_TMR_EXP)
			goto out;
		llc->rej_sent_timer.expire = opt;
		break;
	case LLC_OPT_BUSY_TMR_EXP:
		if (opt > LLC_OPT_MAX_BUSY_TMR_EXP)
			goto out;
		llc->busy_state_timer.expire = opt;
		break;
	case LLC_OPT_TX_WIN:
		if (opt > LLC_OPT_MAX_WIN)
			goto out;
		llc->k = opt;
		break;
	case LLC_OPT_RX_WIN:
		if (opt > LLC_OPT_MAX_WIN)
			goto out;
		llc->rw = opt;
		break;
	default:
		rc = -ENOPROTOOPT;
		goto out;
	}
	rc = 0;
out:
	release_sock(sk);
	return rc;
}

/**
 *	llc_ui_getsockopt - get connection specific socket info
 *	@sock: Socket to get information from.
 *	@level: Socket level user is requesting operations on.
 *	@optname: Operation name.
 *	@optval: Variable to return operation data in.
 *	@optlen: Length of optval.
 *
 *	Get connection specific socket information.
 */
static int llc_ui_getsockopt(struct socket *sock, int level, int optname,
			     char __user *optval, int __user *optlen)
{
	struct sock *sk = sock->sk;
	struct llc_opt *llc = llc_sk(sk);
	int val = 0, len = 0, rc = -EINVAL;

	lock_sock(sk);
	if (level != SOL_LLC)
		goto out;
	rc = get_user(len, optlen);
	if (rc)
		goto out;
	rc = -EINVAL;
	if (len != sizeof(int))
		goto out;
	switch (optname) {
	case LLC_OPT_RETRY:
		val = llc->n2;				break;
	case LLC_OPT_SIZE:
		val = llc->n1;				break;
	case LLC_OPT_ACK_TMR_EXP:
		val = llc->ack_timer.expire;		break;
	case LLC_OPT_P_TMR_EXP:
		val = llc->pf_cycle_timer.expire;	break;
	case LLC_OPT_REJ_TMR_EXP:
		val = llc->rej_sent_timer.expire;	break;
	case LLC_OPT_BUSY_TMR_EXP:
		val = llc->busy_state_timer.expire;	break;
	case LLC_OPT_TX_WIN:
		val = llc->k;				break;
	case LLC_OPT_RX_WIN:
		val = llc->rw;				break;
	default:
		rc = -ENOPROTOOPT;
		goto out;
	}
	rc = 0;
	if (put_user(len, optlen) || copy_to_user(optval, &val, len))
		rc = -EFAULT;
out:
	release_sock(sk);
	return rc;
}

static struct net_proto_family llc_ui_family_ops = {
	.family = PF_LLC,
	.create = llc_ui_create,
	.owner	= THIS_MODULE,
};

static struct proto_ops llc_ui_ops = {
	.family	     = PF_LLC,
	.owner       = THIS_MODULE,
	.release     = llc_ui_release,
	.bind	     = llc_ui_bind,
	.connect     = llc_ui_connect,
	.socketpair  = sock_no_socketpair,
	.accept      = llc_ui_accept,
	.getname     = llc_ui_getname,
	.poll	     = datagram_poll,
	.ioctl       = llc_ui_ioctl,
	.listen      = llc_ui_listen,
	.shutdown    = llc_ui_shutdown,
	.setsockopt  = llc_ui_setsockopt,
	.getsockopt  = llc_ui_getsockopt,
	.sendmsg     = llc_ui_sendmsg,
	.recvmsg     = llc_ui_recvmsg,
	.mmap	     = sock_no_mmap,
	.sendpage    = sock_no_sendpage,
};

extern void llc_sap_handler(struct llc_sap *sap, struct sk_buff *skb);
extern void llc_conn_handler(struct llc_sap *sap, struct sk_buff *skb);

static int __init llc2_init(void)
{
	int rc;

	llc_build_offset_table();
	llc_station_init();
	llc_ui_sap_last_autoport = LLC_SAP_DYN_START;
	rc = llc_proc_init();
	if (!rc) {
		sock_register(&llc_ui_family_ops);
		llc_add_pack(LLC_DEST_SAP, llc_sap_handler);
		llc_add_pack(LLC_DEST_CONN, llc_conn_handler);
	}
	return rc;
}

static void __exit llc2_exit(void)
{
	llc_station_exit();
	llc_remove_pack(LLC_DEST_SAP);
	llc_remove_pack(LLC_DEST_CONN);
	sock_unregister(PF_LLC);
	llc_proc_exit();
}

module_init(llc2_init);
module_exit(llc2_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Procom 1997, Jay Schullist 2001, Arnaldo C. Melo 2001-2003");
MODULE_DESCRIPTION("IEEE 802.2 PF_LLC support");
MODULE_ALIAS_NETPROTO(PF_LLC);
