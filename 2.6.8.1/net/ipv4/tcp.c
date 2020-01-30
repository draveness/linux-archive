/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Implementation of the Transmission Control Protocol(TCP).
 *
 * Version:	$Id: tcp.c,v 1.216 2002/02/01 22:01:04 davem Exp $
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Mark Evans, <evansmp@uhura.aston.ac.uk>
 *		Corey Minyard <wf-rch!minyard@relay.EU.net>
 *		Florian La Roche, <flla@stud.uni-sb.de>
 *		Charles Hedrick, <hedrick@klinzhai.rutgers.edu>
 *		Linus Torvalds, <torvalds@cs.helsinki.fi>
 *		Alan Cox, <gw4pts@gw4pts.ampr.org>
 *		Matthew Dillon, <dillon@apollo.west.oic.com>
 *		Arnt Gulbrandsen, <agulbra@nvg.unit.no>
 *		Jorge Cwik, <jorge@laser.satlink.net>
 *
 * Fixes:
 *		Alan Cox	:	Numerous verify_area() calls
 *		Alan Cox	:	Set the ACK bit on a reset
 *		Alan Cox	:	Stopped it crashing if it closed while
 *					sk->inuse=1 and was trying to connect
 *					(tcp_err()).
 *		Alan Cox	:	All icmp error handling was broken
 *					pointers passed where wrong and the
 *					socket was looked up backwards. Nobody
 *					tested any icmp error code obviously.
 *		Alan Cox	:	tcp_err() now handled properly. It
 *					wakes people on errors. poll
 *					behaves and the icmp error race
 *					has gone by moving it into sock.c
 *		Alan Cox	:	tcp_send_reset() fixed to work for
 *					everything not just packets for
 *					unknown sockets.
 *		Alan Cox	:	tcp option processing.
 *		Alan Cox	:	Reset tweaked (still not 100%) [Had
 *					syn rule wrong]
 *		Herp Rosmanith  :	More reset fixes
 *		Alan Cox	:	No longer acks invalid rst frames.
 *					Acking any kind of RST is right out.
 *		Alan Cox	:	Sets an ignore me flag on an rst
 *					receive otherwise odd bits of prattle
 *					escape still
 *		Alan Cox	:	Fixed another acking RST frame bug.
 *					Should stop LAN workplace lockups.
 *		Alan Cox	: 	Some tidyups using the new skb list
 *					facilities
 *		Alan Cox	:	sk->keepopen now seems to work
 *		Alan Cox	:	Pulls options out correctly on accepts
 *		Alan Cox	:	Fixed assorted sk->rqueue->next errors
 *		Alan Cox	:	PSH doesn't end a TCP read. Switched a
 *					bit to skb ops.
 *		Alan Cox	:	Tidied tcp_data to avoid a potential
 *					nasty.
 *		Alan Cox	:	Added some better commenting, as the
 *					tcp is hard to follow
 *		Alan Cox	:	Removed incorrect check for 20 * psh
 *	Michael O'Reilly	:	ack < copied bug fix.
 *	Johannes Stille		:	Misc tcp fixes (not all in yet).
 *		Alan Cox	:	FIN with no memory -> CRASH
 *		Alan Cox	:	Added socket option proto entries.
 *					Also added awareness of them to accept.
 *		Alan Cox	:	Added TCP options (SOL_TCP)
 *		Alan Cox	:	Switched wakeup calls to callbacks,
 *					so the kernel can layer network
 *					sockets.
 *		Alan Cox	:	Use ip_tos/ip_ttl settings.
 *		Alan Cox	:	Handle FIN (more) properly (we hope).
 *		Alan Cox	:	RST frames sent on unsynchronised
 *					state ack error.
 *		Alan Cox	:	Put in missing check for SYN bit.
 *		Alan Cox	:	Added tcp_select_window() aka NET2E
 *					window non shrink trick.
 *		Alan Cox	:	Added a couple of small NET2E timer
 *					fixes
 *		Charles Hedrick :	TCP fixes
 *		Toomas Tamm	:	TCP window fixes
 *		Alan Cox	:	Small URG fix to rlogin ^C ack fight
 *		Charles Hedrick	:	Rewrote most of it to actually work
 *		Linus		:	Rewrote tcp_read() and URG handling
 *					completely
 *		Gerhard Koerting:	Fixed some missing timer handling
 *		Matthew Dillon  :	Reworked TCP machine states as per RFC
 *		Gerhard Koerting:	PC/TCP workarounds
 *		Adam Caldwell	:	Assorted timer/timing errors
 *		Matthew Dillon	:	Fixed another RST bug
 *		Alan Cox	:	Move to kernel side addressing changes.
 *		Alan Cox	:	Beginning work on TCP fastpathing
 *					(not yet usable)
 *		Arnt Gulbrandsen:	Turbocharged tcp_check() routine.
 *		Alan Cox	:	TCP fast path debugging
 *		Alan Cox	:	Window clamping
 *		Michael Riepe	:	Bug in tcp_check()
 *		Matt Dillon	:	More TCP improvements and RST bug fixes
 *		Matt Dillon	:	Yet more small nasties remove from the
 *					TCP code (Be very nice to this man if
 *					tcp finally works 100%) 8)
 *		Alan Cox	:	BSD accept semantics.
 *		Alan Cox	:	Reset on closedown bug.
 *	Peter De Schrijver	:	ENOTCONN check missing in tcp_sendto().
 *		Michael Pall	:	Handle poll() after URG properly in
 *					all cases.
 *		Michael Pall	:	Undo the last fix in tcp_read_urg()
 *					(multi URG PUSH broke rlogin).
 *		Michael Pall	:	Fix the multi URG PUSH problem in
 *					tcp_readable(), poll() after URG
 *					works now.
 *		Michael Pall	:	recv(...,MSG_OOB) never blocks in the
 *					BSD api.
 *		Alan Cox	:	Changed the semantics of sk->socket to
 *					fix a race and a signal problem with
 *					accept() and async I/O.
 *		Alan Cox	:	Relaxed the rules on tcp_sendto().
 *		Yury Shevchuk	:	Really fixed accept() blocking problem.
 *		Craig I. Hagan  :	Allow for BSD compatible TIME_WAIT for
 *					clients/servers which listen in on
 *					fixed ports.
 *		Alan Cox	:	Cleaned the above up and shrank it to
 *					a sensible code size.
 *		Alan Cox	:	Self connect lockup fix.
 *		Alan Cox	:	No connect to multicast.
 *		Ross Biro	:	Close unaccepted children on master
 *					socket close.
 *		Alan Cox	:	Reset tracing code.
 *		Alan Cox	:	Spurious resets on shutdown.
 *		Alan Cox	:	Giant 15 minute/60 second timer error
 *		Alan Cox	:	Small whoops in polling before an
 *					accept.
 *		Alan Cox	:	Kept the state trace facility since
 *					it's handy for debugging.
 *		Alan Cox	:	More reset handler fixes.
 *		Alan Cox	:	Started rewriting the code based on
 *					the RFC's for other useful protocol
 *					references see: Comer, KA9Q NOS, and
 *					for a reference on the difference
 *					between specifications and how BSD
 *					works see the 4.4lite source.
 *		A.N.Kuznetsov	:	Don't time wait on completion of tidy
 *					close.
 *		Linus Torvalds	:	Fin/Shutdown & copied_seq changes.
 *		Linus Torvalds	:	Fixed BSD port reuse to work first syn
 *		Alan Cox	:	Reimplemented timers as per the RFC
 *					and using multiple timers for sanity.
 *		Alan Cox	:	Small bug fixes, and a lot of new
 *					comments.
 *		Alan Cox	:	Fixed dual reader crash by locking
 *					the buffers (much like datagram.c)
 *		Alan Cox	:	Fixed stuck sockets in probe. A probe
 *					now gets fed up of retrying without
 *					(even a no space) answer.
 *		Alan Cox	:	Extracted closing code better
 *		Alan Cox	:	Fixed the closing state machine to
 *					resemble the RFC.
 *		Alan Cox	:	More 'per spec' fixes.
 *		Jorge Cwik	:	Even faster checksumming.
 *		Alan Cox	:	tcp_data() doesn't ack illegal PSH
 *					only frames. At least one pc tcp stack
 *					generates them.
 *		Alan Cox	:	Cache last socket.
 *		Alan Cox	:	Per route irtt.
 *		Matt Day	:	poll()->select() match BSD precisely on error
 *		Alan Cox	:	New buffers
 *		Marc Tamsky	:	Various sk->prot->retransmits and
 *					sk->retransmits misupdating fixed.
 *					Fixed tcp_write_timeout: stuck close,
 *					and TCP syn retries gets used now.
 *		Mark Yarvis	:	In tcp_read_wakeup(), don't send an
 *					ack if state is TCP_CLOSED.
 *		Alan Cox	:	Look up device on a retransmit - routes may
 *					change. Doesn't yet cope with MSS shrink right
 *					but it's a start!
 *		Marc Tamsky	:	Closing in closing fixes.
 *		Mike Shaver	:	RFC1122 verifications.
 *		Alan Cox	:	rcv_saddr errors.
 *		Alan Cox	:	Block double connect().
 *		Alan Cox	:	Small hooks for enSKIP.
 *		Alexey Kuznetsov:	Path MTU discovery.
 *		Alan Cox	:	Support soft errors.
 *		Alan Cox	:	Fix MTU discovery pathological case
 *					when the remote claims no mtu!
 *		Marc Tamsky	:	TCP_CLOSE fix.
 *		Colin (G3TNE)	:	Send a reset on syn ack replies in
 *					window but wrong (fixes NT lpd problems)
 *		Pedro Roque	:	Better TCP window handling, delayed ack.
 *		Joerg Reuter	:	No modification of locked buffers in
 *					tcp_do_retransmit()
 *		Eric Schenk	:	Changed receiver side silly window
 *					avoidance algorithm to BSD style
 *					algorithm. This doubles throughput
 *					against machines running Solaris,
 *					and seems to result in general
 *					improvement.
 *	Stefan Magdalinski	:	adjusted tcp_readable() to fix FIONREAD
 *	Willy Konynenberg	:	Transparent proxying support.
 *	Mike McLagan		:	Routing by source
 *		Keith Owens	:	Do proper merging with partial SKB's in
 *					tcp_do_sendmsg to avoid burstiness.
 *		Eric Schenk	:	Fix fast close down bug with
 *					shutdown() followed by close().
 *		Andi Kleen 	:	Make poll agree with SIGIO
 *	Salvatore Sanfilippo	:	Support SO_LINGER with linger == 1 and
 *					lingertime == 0 (RFC 793 ABORT Call)
 *	Hirokazu Takahashi	:	Use copy_from_user() instead of
 *					csum_and_copy_from_user() if possible.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or(at your option) any later version.
 *
 * Description of States:
 *
 *	TCP_SYN_SENT		sent a connection request, waiting for ack
 *
 *	TCP_SYN_RECV		received a connection request, sent ack,
 *				waiting for final ack in three-way handshake.
 *
 *	TCP_ESTABLISHED		connection established
 *
 *	TCP_FIN_WAIT1		our side has shutdown, waiting to complete
 *				transmission of remaining buffered data
 *
 *	TCP_FIN_WAIT2		all buffered data sent, waiting for remote
 *				to shutdown
 *
 *	TCP_CLOSING		both sides have shutdown but we still have
 *				data we have to finish sending
 *
 *	TCP_TIME_WAIT		timeout to catch resent junk before entering
 *				closed, can only be entered from FIN_WAIT2
 *				or CLOSING.  Required because the other end
 *				may not have gotten our last ACK causing it
 *				to retransmit the data packet (which we ignore)
 *
 *	TCP_CLOSE_WAIT		remote side has shutdown and is waiting for
 *				us to finish writing our data and to shutdown
 *				(we have to close() to move on to LAST_ACK)
 *
 *	TCP_LAST_ACK		out side has shutdown after remote has
 *				shutdown.  There may still be data in our
 *				buffer that we have to finish sending
 *
 *	TCP_CLOSE		socket is finished
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/smp_lock.h>
#include <linux/fs.h>
#include <linux/random.h>

#include <net/icmp.h>
#include <net/tcp.h>
#include <net/xfrm.h>
#include <net/ip.h>


#include <asm/uaccess.h>
#include <asm/ioctls.h>

int sysctl_tcp_fin_timeout = TCP_FIN_TIMEOUT;

DEFINE_SNMP_STAT(struct tcp_mib, tcp_statistics);

kmem_cache_t *tcp_openreq_cachep;
kmem_cache_t *tcp_bucket_cachep;
kmem_cache_t *tcp_timewait_cachep;

atomic_t tcp_orphan_count = ATOMIC_INIT(0);

int sysctl_tcp_default_win_scale = 7;

int sysctl_tcp_mem[3];
int sysctl_tcp_wmem[3] = { 4 * 1024, 16 * 1024, 128 * 1024 };
int sysctl_tcp_rmem[3] = { 4 * 1024, 87380, 87380 * 2 };

EXPORT_SYMBOL(sysctl_tcp_mem);
EXPORT_SYMBOL(sysctl_tcp_rmem);
EXPORT_SYMBOL(sysctl_tcp_wmem);

atomic_t tcp_memory_allocated;	/* Current allocated memory. */
atomic_t tcp_sockets_allocated;	/* Current number of TCP sockets. */

EXPORT_SYMBOL(tcp_memory_allocated);
EXPORT_SYMBOL(tcp_sockets_allocated);

/*
 * Pressure flag: try to collapse.
 * Technical note: it is used by multiple contexts non atomically.
 * All the sk_stream_mem_schedule() is of this nature: accounting
 * is strict, actions are advisory and have some latency.
 */
int tcp_memory_pressure;

EXPORT_SYMBOL(tcp_memory_pressure);

void tcp_enter_memory_pressure(void)
{
	if (!tcp_memory_pressure) {
		NET_INC_STATS(LINUX_MIB_TCPMEMORYPRESSURES);
		tcp_memory_pressure = 1;
	}
}

EXPORT_SYMBOL(tcp_enter_memory_pressure);

/*
 * LISTEN is a special case for poll..
 */
static __inline__ unsigned int tcp_listen_poll(struct sock *sk,
					       poll_table *wait)
{
	return tcp_sk(sk)->accept_queue ? (POLLIN | POLLRDNORM) : 0;
}

/*
 *	Wait for a TCP event.
 *
 *	Note that we don't need to lock the socket, as the upper poll layers
 *	take care of normal races (between the test and the event) and we don't
 *	go look at any of the socket buffers directly.
 */
unsigned int tcp_poll(struct file *file, struct socket *sock, poll_table *wait)
{
	unsigned int mask;
	struct sock *sk = sock->sk;
	struct tcp_opt *tp = tcp_sk(sk);

	poll_wait(file, sk->sk_sleep, wait);
	if (sk->sk_state == TCP_LISTEN)
		return tcp_listen_poll(sk, wait);

	/* Socket is not locked. We are protected from async events
	   by poll logic and correct handling of state changes
	   made by another threads is impossible in any case.
	 */

	mask = 0;
	if (sk->sk_err)
		mask = POLLERR;

	/*
	 * POLLHUP is certainly not done right. But poll() doesn't
	 * have a notion of HUP in just one direction, and for a
	 * socket the read side is more interesting.
	 *
	 * Some poll() documentation says that POLLHUP is incompatible
	 * with the POLLOUT/POLLWR flags, so somebody should check this
	 * all. But careful, it tends to be safer to return too many
	 * bits than too few, and you can easily break real applications
	 * if you don't tell them that something has hung up!
	 *
	 * Check-me.
	 *
	 * Check number 1. POLLHUP is _UNMASKABLE_ event (see UNIX98 and
	 * our fs/select.c). It means that after we received EOF,
	 * poll always returns immediately, making impossible poll() on write()
	 * in state CLOSE_WAIT. One solution is evident --- to set POLLHUP
	 * if and only if shutdown has been made in both directions.
	 * Actually, it is interesting to look how Solaris and DUX
	 * solve this dilemma. I would prefer, if PULLHUP were maskable,
	 * then we could set it on SND_SHUTDOWN. BTW examples given
	 * in Stevens' books assume exactly this behaviour, it explains
	 * why PULLHUP is incompatible with POLLOUT.	--ANK
	 *
	 * NOTE. Check for TCP_CLOSE is added. The goal is to prevent
	 * blocking on fresh not-connected or disconnected socket. --ANK
	 */
	if (sk->sk_shutdown == SHUTDOWN_MASK || sk->sk_state == TCP_CLOSE)
		mask |= POLLHUP;
	if (sk->sk_shutdown & RCV_SHUTDOWN)
		mask |= POLLIN | POLLRDNORM;

	/* Connected? */
	if ((1 << sk->sk_state) & ~(TCPF_SYN_SENT | TCPF_SYN_RECV)) {
		/* Potential race condition. If read of tp below will
		 * escape above sk->sk_state, we can be illegally awaken
		 * in SYN_* states. */
		if ((tp->rcv_nxt != tp->copied_seq) &&
		    (tp->urg_seq != tp->copied_seq ||
		     tp->rcv_nxt != tp->copied_seq + 1 ||
		     sock_flag(sk, SOCK_URGINLINE) || !tp->urg_data))
			mask |= POLLIN | POLLRDNORM;

		if (!(sk->sk_shutdown & SEND_SHUTDOWN)) {
			if (sk_stream_wspace(sk) >= sk_stream_min_wspace(sk)) {
				mask |= POLLOUT | POLLWRNORM;
			} else {  /* send SIGIO later */
				set_bit(SOCK_ASYNC_NOSPACE,
					&sk->sk_socket->flags);
				set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);

				/* Race breaker. If space is freed after
				 * wspace test but before the flags are set,
				 * IO signal will be lost.
				 */
				if (sk_stream_wspace(sk) >= sk_stream_min_wspace(sk))
					mask |= POLLOUT | POLLWRNORM;
			}
		}

		if (tp->urg_data & TCP_URG_VALID)
			mask |= POLLPRI;
	}
	return mask;
}

int tcp_ioctl(struct sock *sk, int cmd, unsigned long arg)
{
	struct tcp_opt *tp = tcp_sk(sk);
	int answ;

	switch (cmd) {
	case SIOCINQ:
		if (sk->sk_state == TCP_LISTEN)
			return -EINVAL;

		lock_sock(sk);
		if ((1 << sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV))
			answ = 0;
		else if (sock_flag(sk, SOCK_URGINLINE) ||
			 !tp->urg_data ||
			 before(tp->urg_seq, tp->copied_seq) ||
			 !before(tp->urg_seq, tp->rcv_nxt)) {
			answ = tp->rcv_nxt - tp->copied_seq;

			/* Subtract 1, if FIN is in queue. */
			if (answ && !skb_queue_empty(&sk->sk_receive_queue))
				answ -=
		       ((struct sk_buff *)sk->sk_receive_queue.prev)->h.th->fin;
		} else
			answ = tp->urg_seq - tp->copied_seq;
		release_sock(sk);
		break;
	case SIOCATMARK:
		answ = tp->urg_data && tp->urg_seq == tp->copied_seq;
		break;
	case SIOCOUTQ:
		if (sk->sk_state == TCP_LISTEN)
			return -EINVAL;

		if ((1 << sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV))
			answ = 0;
		else
			answ = tp->write_seq - tp->snd_una;
		break;
	default:
		return -ENOIOCTLCMD;
	};

	return put_user(answ, (int __user *)arg);
}


int tcp_listen_start(struct sock *sk)
{
	struct inet_opt *inet = inet_sk(sk);
	struct tcp_opt *tp = tcp_sk(sk);
	struct tcp_listen_opt *lopt;

	sk->sk_max_ack_backlog = 0;
	sk->sk_ack_backlog = 0;
	tp->accept_queue = tp->accept_queue_tail = NULL;
	tp->syn_wait_lock = RW_LOCK_UNLOCKED;
	tcp_delack_init(tp);

	lopt = kmalloc(sizeof(struct tcp_listen_opt), GFP_KERNEL);
	if (!lopt)
		return -ENOMEM;

	memset(lopt, 0, sizeof(struct tcp_listen_opt));
	for (lopt->max_qlen_log = 6; ; lopt->max_qlen_log++)
		if ((1 << lopt->max_qlen_log) >= sysctl_max_syn_backlog)
			break;
	get_random_bytes(&lopt->hash_rnd, 4);

	write_lock_bh(&tp->syn_wait_lock);
	tp->listen_opt = lopt;
	write_unlock_bh(&tp->syn_wait_lock);

	/* There is race window here: we announce ourselves listening,
	 * but this transition is still not validated by get_port().
	 * It is OK, because this socket enters to hash table only
	 * after validation is complete.
	 */
	sk->sk_state = TCP_LISTEN;
	if (!sk->sk_prot->get_port(sk, inet->num)) {
		inet->sport = htons(inet->num);

		sk_dst_reset(sk);
		sk->sk_prot->hash(sk);

		return 0;
	}

	sk->sk_state = TCP_CLOSE;
	write_lock_bh(&tp->syn_wait_lock);
	tp->listen_opt = NULL;
	write_unlock_bh(&tp->syn_wait_lock);
	kfree(lopt);
	return -EADDRINUSE;
}

/*
 *	This routine closes sockets which have been at least partially
 *	opened, but not yet accepted.
 */

static void tcp_listen_stop (struct sock *sk)
{
	struct tcp_opt *tp = tcp_sk(sk);
	struct tcp_listen_opt *lopt = tp->listen_opt;
	struct open_request *acc_req = tp->accept_queue;
	struct open_request *req;
	int i;

	tcp_delete_keepalive_timer(sk);

	/* make all the listen_opt local to us */
	write_lock_bh(&tp->syn_wait_lock);
	tp->listen_opt = NULL;
	write_unlock_bh(&tp->syn_wait_lock);
	tp->accept_queue = tp->accept_queue_tail = NULL;

	if (lopt->qlen) {
		for (i = 0; i < TCP_SYNQ_HSIZE; i++) {
			while ((req = lopt->syn_table[i]) != NULL) {
				lopt->syn_table[i] = req->dl_next;
				lopt->qlen--;
				tcp_openreq_free(req);

		/* Following specs, it would be better either to send FIN
		 * (and enter FIN-WAIT-1, it is normal close)
		 * or to send active reset (abort).
		 * Certainly, it is pretty dangerous while synflood, but it is
		 * bad justification for our negligence 8)
		 * To be honest, we are not able to make either
		 * of the variants now.			--ANK
		 */
			}
		}
	}
	BUG_TRAP(!lopt->qlen);

	kfree(lopt);

	while ((req = acc_req) != NULL) {
		struct sock *child = req->sk;

		acc_req = req->dl_next;

		local_bh_disable();
		bh_lock_sock(child);
		BUG_TRAP(!sock_owned_by_user(child));
		sock_hold(child);

		tcp_disconnect(child, O_NONBLOCK);

		sock_orphan(child);

		atomic_inc(&tcp_orphan_count);

		tcp_destroy_sock(child);

		bh_unlock_sock(child);
		local_bh_enable();
		sock_put(child);

		sk_acceptq_removed(sk);
		tcp_openreq_fastfree(req);
	}
	BUG_TRAP(!sk->sk_ack_backlog);
}

static inline void tcp_mark_push(struct tcp_opt *tp, struct sk_buff *skb)
{
	TCP_SKB_CB(skb)->flags |= TCPCB_FLAG_PSH;
	tp->pushed_seq = tp->write_seq;
}

static inline int forced_push(struct tcp_opt *tp)
{
	return after(tp->write_seq, tp->pushed_seq + (tp->max_window >> 1));
}

static inline void skb_entail(struct sock *sk, struct tcp_opt *tp,
			      struct sk_buff *skb)
{
	skb->csum = 0;
	TCP_SKB_CB(skb)->seq = tp->write_seq;
	TCP_SKB_CB(skb)->end_seq = tp->write_seq;
	TCP_SKB_CB(skb)->flags = TCPCB_FLAG_ACK;
	TCP_SKB_CB(skb)->sacked = 0;
	__skb_queue_tail(&sk->sk_write_queue, skb);
	sk_charge_skb(sk, skb);
	if (!sk->sk_send_head)
		sk->sk_send_head = skb;
	else if (tp->nonagle&TCP_NAGLE_PUSH)
		tp->nonagle &= ~TCP_NAGLE_PUSH; 
}

static inline void tcp_mark_urg(struct tcp_opt *tp, int flags,
				struct sk_buff *skb)
{
	if (flags & MSG_OOB) {
		tp->urg_mode = 1;
		tp->snd_up = tp->write_seq;
		TCP_SKB_CB(skb)->sacked |= TCPCB_URG;
	}
}

static inline void tcp_push(struct sock *sk, struct tcp_opt *tp, int flags,
			    int mss_now, int nonagle)
{
	if (sk->sk_send_head) {
		struct sk_buff *skb = sk->sk_write_queue.prev;
		if (!(flags & MSG_MORE) || forced_push(tp))
			tcp_mark_push(tp, skb);
		tcp_mark_urg(tp, flags, skb);
		__tcp_push_pending_frames(sk, tp, mss_now,
					  (flags & MSG_MORE) ? TCP_NAGLE_CORK : nonagle);
	}
}

static ssize_t do_tcp_sendpages(struct sock *sk, struct page **pages, int poffset,
			 size_t psize, int flags)
{
	struct tcp_opt *tp = tcp_sk(sk);
	int mss_now;
	int err;
	ssize_t copied;
	long timeo = sock_sndtimeo(sk, flags & MSG_DONTWAIT);

	/* Wait for a connection to finish. */
	if ((1 << sk->sk_state) & ~(TCPF_ESTABLISHED | TCPF_CLOSE_WAIT))
		if ((err = sk_stream_wait_connect(sk, &timeo)) != 0)
			goto out_err;

	clear_bit(SOCK_ASYNC_NOSPACE, &sk->sk_socket->flags);

	mss_now = tcp_current_mss(sk, !(flags&MSG_OOB));
	copied = 0;

	err = -EPIPE;
	if (sk->sk_err || (sk->sk_shutdown & SEND_SHUTDOWN))
		goto do_error;

	while (psize > 0) {
		struct sk_buff *skb = sk->sk_write_queue.prev;
		struct page *page = pages[poffset / PAGE_SIZE];
		int copy, i;
		int offset = poffset % PAGE_SIZE;
		int size = min_t(size_t, psize, PAGE_SIZE - offset);

		if (!sk->sk_send_head || (copy = mss_now - skb->len) <= 0) {
new_segment:
			if (!sk_stream_memory_free(sk))
				goto wait_for_sndbuf;

			skb = sk_stream_alloc_pskb(sk, 0, tp->mss_cache,
						   sk->sk_allocation);
			if (!skb)
				goto wait_for_memory;

			skb_entail(sk, tp, skb);
			copy = mss_now;
		}

		if (copy > size)
			copy = size;

		i = skb_shinfo(skb)->nr_frags;
		if (skb_can_coalesce(skb, i, page, offset)) {
			skb_shinfo(skb)->frags[i - 1].size += copy;
		} else if (i < MAX_SKB_FRAGS) {
			get_page(page);
			skb_fill_page_desc(skb, i, page, offset, copy);
		} else {
			tcp_mark_push(tp, skb);
			goto new_segment;
		}

		skb->len += copy;
		skb->data_len += copy;
		skb->ip_summed = CHECKSUM_HW;
		tp->write_seq += copy;
		TCP_SKB_CB(skb)->end_seq += copy;

		if (!copied)
			TCP_SKB_CB(skb)->flags &= ~TCPCB_FLAG_PSH;

		copied += copy;
		poffset += copy;
		if (!(psize -= copy))
			goto out;

		if (skb->len != mss_now || (flags & MSG_OOB))
			continue;

		if (forced_push(tp)) {
			tcp_mark_push(tp, skb);
			__tcp_push_pending_frames(sk, tp, mss_now, TCP_NAGLE_PUSH);
		} else if (skb == sk->sk_send_head)
			tcp_push_one(sk, mss_now);
		continue;

wait_for_sndbuf:
		set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
wait_for_memory:
		if (copied)
			tcp_push(sk, tp, flags & ~MSG_MORE, mss_now, TCP_NAGLE_PUSH);

		if ((err = sk_stream_wait_memory(sk, &timeo)) != 0)
			goto do_error;

		mss_now = tcp_current_mss(sk, !(flags&MSG_OOB));
	}

out:
	if (copied)
		tcp_push(sk, tp, flags, mss_now, tp->nonagle);
	return copied;

do_error:
	if (copied)
		goto out;
out_err:
	return sk_stream_error(sk, flags, err);
}

ssize_t tcp_sendpage(struct socket *sock, struct page *page, int offset,
		     size_t size, int flags)
{
	ssize_t res;
	struct sock *sk = sock->sk;

#define TCP_ZC_CSUM_FLAGS (NETIF_F_IP_CSUM | NETIF_F_NO_CSUM | NETIF_F_HW_CSUM)

	if (!(sk->sk_route_caps & NETIF_F_SG) ||
	    !(sk->sk_route_caps & TCP_ZC_CSUM_FLAGS))
		return sock_no_sendpage(sock, page, offset, size, flags);

#undef TCP_ZC_CSUM_FLAGS

	lock_sock(sk);
	TCP_CHECK_TIMER(sk);
	res = do_tcp_sendpages(sk, &page, offset, size, flags);
	TCP_CHECK_TIMER(sk);
	release_sock(sk);
	return res;
}

#define TCP_PAGE(sk)	(sk->sk_sndmsg_page)
#define TCP_OFF(sk)	(sk->sk_sndmsg_off)

static inline int select_size(struct sock *sk, struct tcp_opt *tp)
{
	int tmp = tp->mss_cache_std;

	if (sk->sk_route_caps & NETIF_F_SG) {
		int pgbreak = SKB_MAX_HEAD(MAX_TCP_HEADER);

		if (tmp >= pgbreak &&
		    tmp <= pgbreak + (MAX_SKB_FRAGS - 1) * PAGE_SIZE)
			tmp = pgbreak;
	}
	return tmp;
}

int tcp_sendmsg(struct kiocb *iocb, struct sock *sk, struct msghdr *msg,
		size_t size)
{
	struct iovec *iov;
	struct tcp_opt *tp = tcp_sk(sk);
	struct sk_buff *skb;
	int iovlen, flags;
	int mss_now;
	int err, copied;
	long timeo;

	lock_sock(sk);
	TCP_CHECK_TIMER(sk);

	flags = msg->msg_flags;
	timeo = sock_sndtimeo(sk, flags & MSG_DONTWAIT);

	/* Wait for a connection to finish. */
	if ((1 << sk->sk_state) & ~(TCPF_ESTABLISHED | TCPF_CLOSE_WAIT))
		if ((err = sk_stream_wait_connect(sk, &timeo)) != 0)
			goto out_err;

	/* This should be in poll */
	clear_bit(SOCK_ASYNC_NOSPACE, &sk->sk_socket->flags);

	mss_now = tcp_current_mss(sk, !(flags&MSG_OOB));

	/* Ok commence sending. */
	iovlen = msg->msg_iovlen;
	iov = msg->msg_iov;
	copied = 0;

	err = -EPIPE;
	if (sk->sk_err || (sk->sk_shutdown & SEND_SHUTDOWN))
		goto do_error;

	while (--iovlen >= 0) {
		int seglen = iov->iov_len;
		unsigned char __user *from = iov->iov_base;

		iov++;

		while (seglen > 0) {
			int copy;

			skb = sk->sk_write_queue.prev;

			if (!sk->sk_send_head ||
			    (copy = mss_now - skb->len) <= 0) {

new_segment:
				/* Allocate new segment. If the interface is SG,
				 * allocate skb fitting to single page.
				 */
				if (!sk_stream_memory_free(sk))
					goto wait_for_sndbuf;

				skb = sk_stream_alloc_pskb(sk, select_size(sk, tp),
							   0, sk->sk_allocation);
				if (!skb)
					goto wait_for_memory;

				/*
				 * Check whether we can use HW checksum.
				 */
				if (sk->sk_route_caps &
				    (NETIF_F_IP_CSUM | NETIF_F_NO_CSUM |
				     NETIF_F_HW_CSUM))
					skb->ip_summed = CHECKSUM_HW;

				skb_entail(sk, tp, skb);
				copy = mss_now;
			}

			/* Try to append data to the end of skb. */
			if (copy > seglen)
				copy = seglen;

			/* Where to copy to? */
			if (skb_tailroom(skb) > 0) {
				/* We have some space in skb head. Superb! */
				if (copy > skb_tailroom(skb))
					copy = skb_tailroom(skb);
				if ((err = skb_add_data(skb, from, copy)) != 0)
					goto do_fault;
			} else {
				int merge = 0;
				int i = skb_shinfo(skb)->nr_frags;
				struct page *page = TCP_PAGE(sk);
				int off = TCP_OFF(sk);

				if (skb_can_coalesce(skb, i, page, off) &&
				    off != PAGE_SIZE) {
					/* We can extend the last page
					 * fragment. */
					merge = 1;
				} else if (i == MAX_SKB_FRAGS ||
					   (!i &&
					   !(sk->sk_route_caps & NETIF_F_SG))) {
					/* Need to add new fragment and cannot
					 * do this because interface is non-SG,
					 * or because all the page slots are
					 * busy. */
					tcp_mark_push(tp, skb);
					goto new_segment;
				} else if (page) {
					/* If page is cached, align
					 * offset to L1 cache boundary
					 */
					off = (off + L1_CACHE_BYTES - 1) &
					      ~(L1_CACHE_BYTES - 1);
					if (off == PAGE_SIZE) {
						put_page(page);
						TCP_PAGE(sk) = page = NULL;
					}
				}

				if (!page) {
					/* Allocate new cache page. */
					if (!(page = sk_stream_alloc_page(sk)))
						goto wait_for_memory;
					off = 0;
				}

				if (copy > PAGE_SIZE - off)
					copy = PAGE_SIZE - off;

				/* Time to copy data. We are close to
				 * the end! */
				err = skb_copy_to_page(sk, from, skb, page,
						       off, copy);
				if (err) {
					/* If this page was new, give it to the
					 * socket so it does not get leaked.
					 */
					if (!TCP_PAGE(sk)) {
						TCP_PAGE(sk) = page;
						TCP_OFF(sk) = 0;
					}
					goto do_error;
				}

				/* Update the skb. */
				if (merge) {
					skb_shinfo(skb)->frags[i - 1].size +=
									copy;
				} else {
					skb_fill_page_desc(skb, i, page, off, copy);
					if (TCP_PAGE(sk)) {
						get_page(page);
					} else if (off + copy < PAGE_SIZE) {
						get_page(page);
						TCP_PAGE(sk) = page;
					}
				}

				TCP_OFF(sk) = off + copy;
			}

			if (!copied)
				TCP_SKB_CB(skb)->flags &= ~TCPCB_FLAG_PSH;

			tp->write_seq += copy;
			TCP_SKB_CB(skb)->end_seq += copy;

			from += copy;
			copied += copy;
			if ((seglen -= copy) == 0 && iovlen == 0)
				goto out;

			if (skb->len != mss_now || (flags & MSG_OOB))
				continue;

			if (forced_push(tp)) {
				tcp_mark_push(tp, skb);
				__tcp_push_pending_frames(sk, tp, mss_now, TCP_NAGLE_PUSH);
			} else if (skb == sk->sk_send_head)
				tcp_push_one(sk, mss_now);
			continue;

wait_for_sndbuf:
			set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
wait_for_memory:
			if (copied)
				tcp_push(sk, tp, flags & ~MSG_MORE, mss_now, TCP_NAGLE_PUSH);

			if ((err = sk_stream_wait_memory(sk, &timeo)) != 0)
				goto do_error;

			mss_now = tcp_current_mss(sk, !(flags&MSG_OOB));
		}
	}

out:
	if (copied)
		tcp_push(sk, tp, flags, mss_now, tp->nonagle);
	TCP_CHECK_TIMER(sk);
	release_sock(sk);
	return copied;

do_fault:
	if (!skb->len) {
		if (sk->sk_send_head == skb)
			sk->sk_send_head = NULL;
		__skb_unlink(skb, skb->list);
		sk_stream_free_skb(sk, skb);
	}

do_error:
	if (copied)
		goto out;
out_err:
	err = sk_stream_error(sk, flags, err);
	TCP_CHECK_TIMER(sk);
	release_sock(sk);
	return err;
}

/*
 *	Handle reading urgent data. BSD has very simple semantics for
 *	this, no blocking and very strange errors 8)
 */

static int tcp_recv_urg(struct sock *sk, long timeo,
			struct msghdr *msg, int len, int flags,
			int *addr_len)
{
	struct tcp_opt *tp = tcp_sk(sk);

	/* No URG data to read. */
	if (sock_flag(sk, SOCK_URGINLINE) || !tp->urg_data ||
	    tp->urg_data == TCP_URG_READ)
		return -EINVAL;	/* Yes this is right ! */

	if (sk->sk_state == TCP_CLOSE && !sock_flag(sk, SOCK_DONE))
		return -ENOTCONN;

	if (tp->urg_data & TCP_URG_VALID) {
		int err = 0;
		char c = tp->urg_data;

		if (!(flags & MSG_PEEK))
			tp->urg_data = TCP_URG_READ;

		/* Read urgent data. */
		msg->msg_flags |= MSG_OOB;

		if (len > 0) {
			if (!(flags & MSG_TRUNC))
				err = memcpy_toiovec(msg->msg_iov, &c, 1);
			len = 1;
		} else
			msg->msg_flags |= MSG_TRUNC;

		return err ? -EFAULT : len;
	}

	if (sk->sk_state == TCP_CLOSE || (sk->sk_shutdown & RCV_SHUTDOWN))
		return 0;

	/* Fixed the recv(..., MSG_OOB) behaviour.  BSD docs and
	 * the available implementations agree in this case:
	 * this call should never block, independent of the
	 * blocking state of the socket.
	 * Mike <pall@rz.uni-karlsruhe.de>
	 */
	return -EAGAIN;
}

/* Clean up the receive buffer for full frames taken by the user,
 * then send an ACK if necessary.  COPIED is the number of bytes
 * tcp_recvmsg has given to the user so far, it speeds up the
 * calculation of whether or not we must ACK for the sake of
 * a window update.
 */
static void cleanup_rbuf(struct sock *sk, int copied)
{
	struct tcp_opt *tp = tcp_sk(sk);
	int time_to_ack = 0;

#if TCP_DEBUG
	struct sk_buff *skb = skb_peek(&sk->sk_receive_queue);

	BUG_TRAP(!skb || before(tp->copied_seq, TCP_SKB_CB(skb)->end_seq));
#endif

	if (tcp_ack_scheduled(tp)) {
		   /* Delayed ACKs frequently hit locked sockets during bulk
		    * receive. */
		if (tp->ack.blocked ||
		    /* Once-per-two-segments ACK was not sent by tcp_input.c */
		    tp->rcv_nxt - tp->rcv_wup > tp->ack.rcv_mss ||
		    /*
		     * If this read emptied read buffer, we send ACK, if
		     * connection is not bidirectional, user drained
		     * receive buffer and there was a small segment
		     * in queue.
		     */
		    (copied > 0 && (tp->ack.pending & TCP_ACK_PUSHED) &&
		     !tp->ack.pingpong && !atomic_read(&sk->sk_rmem_alloc)))
			time_to_ack = 1;
	}

	/* We send an ACK if we can now advertise a non-zero window
	 * which has been raised "significantly".
	 *
	 * Even if window raised up to infinity, do not send window open ACK
	 * in states, where we will not receive more. It is useless.
	 */
	if (copied > 0 && !time_to_ack && !(sk->sk_shutdown & RCV_SHUTDOWN)) {
		__u32 rcv_window_now = tcp_receive_window(tp);

		/* Optimize, __tcp_select_window() is not cheap. */
		if (2*rcv_window_now <= tp->window_clamp) {
			__u32 new_window = __tcp_select_window(sk);

			/* Send ACK now, if this read freed lots of space
			 * in our buffer. Certainly, new_window is new window.
			 * We can advertise it now, if it is not less than current one.
			 * "Lots" means "at least twice" here.
			 */
			if (new_window && new_window >= 2 * rcv_window_now)
				time_to_ack = 1;
		}
	}
	if (time_to_ack)
		tcp_send_ack(sk);
}

static void tcp_prequeue_process(struct sock *sk)
{
	struct sk_buff *skb;
	struct tcp_opt *tp = tcp_sk(sk);

	NET_ADD_STATS_USER(LINUX_MIB_TCPPREQUEUED, skb_queue_len(&tp->ucopy.prequeue));

	/* RX process wants to run with disabled BHs, though it is not
	 * necessary */
	local_bh_disable();
	while ((skb = __skb_dequeue(&tp->ucopy.prequeue)) != NULL)
		sk->sk_backlog_rcv(sk, skb);
	local_bh_enable();

	/* Clear memory counter. */
	tp->ucopy.memory = 0;
}

static inline struct sk_buff *tcp_recv_skb(struct sock *sk, u32 seq, u32 *off)
{
	struct sk_buff *skb;
	u32 offset;

	skb_queue_walk(&sk->sk_receive_queue, skb) {
		offset = seq - TCP_SKB_CB(skb)->seq;
		if (skb->h.th->syn)
			offset--;
		if (offset < skb->len || skb->h.th->fin) {
			*off = offset;
			return skb;
		}
	}
	return NULL;
}

/*
 * This routine provides an alternative to tcp_recvmsg() for routines
 * that would like to handle copying from skbuffs directly in 'sendfile'
 * fashion.
 * Note:
 *	- It is assumed that the socket was locked by the caller.
 *	- The routine does not block.
 *	- At present, there is no support for reading OOB data
 *	  or for 'peeking' the socket using this routine
 *	  (although both would be easy to implement).
 */
int tcp_read_sock(struct sock *sk, read_descriptor_t *desc,
		  sk_read_actor_t recv_actor)
{
	struct sk_buff *skb;
	struct tcp_opt *tp = tcp_sk(sk);
	u32 seq = tp->copied_seq;
	u32 offset;
	int copied = 0;

	if (sk->sk_state == TCP_LISTEN)
		return -ENOTCONN;
	while ((skb = tcp_recv_skb(sk, seq, &offset)) != NULL) {
		if (offset < skb->len) {
			size_t used, len;

			len = skb->len - offset;
			/* Stop reading if we hit a patch of urgent data */
			if (tp->urg_data) {
				u32 urg_offset = tp->urg_seq - seq;
				if (urg_offset < len)
					len = urg_offset;
				if (!len)
					break;
			}
			used = recv_actor(desc, skb, offset, len);
			if (used <= len) {
				seq += used;
				copied += used;
				offset += used;
			}
			if (offset != skb->len)
				break;
		}
		if (skb->h.th->fin) {
			sk_eat_skb(sk, skb);
			++seq;
			break;
		}
		sk_eat_skb(sk, skb);
		if (!desc->count)
			break;
	}
	tp->copied_seq = seq;

	tcp_rcv_space_adjust(sk);

	/* Clean up data we have read: This will do ACK frames. */
	if (copied)
		cleanup_rbuf(sk, copied);
	return copied;
}

/*
 *	This routine copies from a sock struct into the user buffer.
 *
 *	Technical note: in 2.3 we work on _locked_ socket, so that
 *	tricks with *seq access order and skb->users are not required.
 *	Probably, code can be easily improved even more.
 */

int tcp_recvmsg(struct kiocb *iocb, struct sock *sk, struct msghdr *msg,
		size_t len, int nonblock, int flags, int *addr_len)
{
	struct tcp_opt *tp = tcp_sk(sk);
	int copied = 0;
	u32 peek_seq;
	u32 *seq;
	unsigned long used;
	int err;
	int target;		/* Read at least this many bytes */
	long timeo;
	struct task_struct *user_recv = NULL;

	lock_sock(sk);

	TCP_CHECK_TIMER(sk);

	err = -ENOTCONN;
	if (sk->sk_state == TCP_LISTEN)
		goto out;

	timeo = sock_rcvtimeo(sk, nonblock);

	/* Urgent data needs to be handled specially. */
	if (flags & MSG_OOB)
		goto recv_urg;

	seq = &tp->copied_seq;
	if (flags & MSG_PEEK) {
		peek_seq = tp->copied_seq;
		seq = &peek_seq;
	}

	target = sock_rcvlowat(sk, flags & MSG_WAITALL, len);

	do {
		struct sk_buff *skb;
		u32 offset;

		/* Are we at urgent data? Stop if we have read anything or have SIGURG pending. */
		if (tp->urg_data && tp->urg_seq == *seq) {
			if (copied)
				break;
			if (signal_pending(current)) {
				copied = timeo ? sock_intr_errno(timeo) : -EAGAIN;
				break;
			}
		}

		/* Next get a buffer. */

		skb = skb_peek(&sk->sk_receive_queue);
		do {
			if (!skb)
				break;

			/* Now that we have two receive queues this
			 * shouldn't happen.
			 */
			if (before(*seq, TCP_SKB_CB(skb)->seq)) {
				printk(KERN_INFO "recvmsg bug: copied %X "
				       "seq %X\n", *seq, TCP_SKB_CB(skb)->seq);
				break;
			}
			offset = *seq - TCP_SKB_CB(skb)->seq;
			if (skb->h.th->syn)
				offset--;
			if (offset < skb->len)
				goto found_ok_skb;
			if (skb->h.th->fin)
				goto found_fin_ok;
			BUG_TRAP(flags & MSG_PEEK);
			skb = skb->next;
		} while (skb != (struct sk_buff *)&sk->sk_receive_queue);

		/* Well, if we have backlog, try to process it now yet. */

		if (copied >= target && !sk->sk_backlog.tail)
			break;

		if (copied) {
			if (sk->sk_err ||
			    sk->sk_state == TCP_CLOSE ||
			    (sk->sk_shutdown & RCV_SHUTDOWN) ||
			    !timeo ||
			    signal_pending(current) ||
			    (flags & MSG_PEEK))
				break;
		} else {
			if (sock_flag(sk, SOCK_DONE))
				break;

			if (sk->sk_err) {
				copied = sock_error(sk);
				break;
			}

			if (sk->sk_shutdown & RCV_SHUTDOWN)
				break;

			if (sk->sk_state == TCP_CLOSE) {
				if (!sock_flag(sk, SOCK_DONE)) {
					/* This occurs when user tries to read
					 * from never connected socket.
					 */
					copied = -ENOTCONN;
					break;
				}
				break;
			}

			if (!timeo) {
				copied = -EAGAIN;
				break;
			}

			if (signal_pending(current)) {
				copied = sock_intr_errno(timeo);
				break;
			}
		}

		cleanup_rbuf(sk, copied);

		if (tp->ucopy.task == user_recv) {
			/* Install new reader */
			if (!user_recv && !(flags & (MSG_TRUNC | MSG_PEEK))) {
				user_recv = current;
				tp->ucopy.task = user_recv;
				tp->ucopy.iov = msg->msg_iov;
			}

			tp->ucopy.len = len;

			BUG_TRAP(tp->copied_seq == tp->rcv_nxt ||
				 (flags & (MSG_PEEK | MSG_TRUNC)));

			/* Ugly... If prequeue is not empty, we have to
			 * process it before releasing socket, otherwise
			 * order will be broken at second iteration.
			 * More elegant solution is required!!!
			 *
			 * Look: we have the following (pseudo)queues:
			 *
			 * 1. packets in flight
			 * 2. backlog
			 * 3. prequeue
			 * 4. receive_queue
			 *
			 * Each queue can be processed only if the next ones
			 * are empty. At this point we have empty receive_queue.
			 * But prequeue _can_ be not empty after 2nd iteration,
			 * when we jumped to start of loop because backlog
			 * processing added something to receive_queue.
			 * We cannot release_sock(), because backlog contains
			 * packets arrived _after_ prequeued ones.
			 *
			 * Shortly, algorithm is clear --- to process all
			 * the queues in order. We could make it more directly,
			 * requeueing packets from backlog to prequeue, if
			 * is not empty. It is more elegant, but eats cycles,
			 * unfortunately.
			 */
			if (skb_queue_len(&tp->ucopy.prequeue))
				goto do_prequeue;

			/* __ Set realtime policy in scheduler __ */
		}

		if (copied >= target) {
			/* Do not sleep, just process backlog. */
			release_sock(sk);
			lock_sock(sk);
		} else
			sk_wait_data(sk, &timeo);

		if (user_recv) {
			int chunk;

			/* __ Restore normal policy in scheduler __ */

			if ((chunk = len - tp->ucopy.len) != 0) {
				NET_ADD_STATS_USER(LINUX_MIB_TCPDIRECTCOPYFROMBACKLOG, chunk);
				len -= chunk;
				copied += chunk;
			}

			if (tp->rcv_nxt == tp->copied_seq &&
			    skb_queue_len(&tp->ucopy.prequeue)) {
do_prequeue:
				tcp_prequeue_process(sk);

				if ((chunk = len - tp->ucopy.len) != 0) {
					NET_ADD_STATS_USER(LINUX_MIB_TCPDIRECTCOPYFROMPREQUEUE, chunk);
					len -= chunk;
					copied += chunk;
				}
			}
		}
		if ((flags & MSG_PEEK) && peek_seq != tp->copied_seq) {
			if (net_ratelimit())
				printk(KERN_DEBUG "TCP(%s:%d): Application bug, race in MSG_PEEK.\n",
				       current->comm, current->pid);
			peek_seq = tp->copied_seq;
		}
		continue;

	found_ok_skb:
		/* Ok so how much can we use? */
		used = skb->len - offset;
		if (len < used)
			used = len;

		/* Do we have urgent data here? */
		if (tp->urg_data) {
			u32 urg_offset = tp->urg_seq - *seq;
			if (urg_offset < used) {
				if (!urg_offset) {
					if (!sock_flag(sk, SOCK_URGINLINE)) {
						++*seq;
						offset++;
						used--;
						if (!used)
							goto skip_copy;
					}
				} else
					used = urg_offset;
			}
		}

		if (!(flags & MSG_TRUNC)) {
			err = skb_copy_datagram_iovec(skb, offset,
						      msg->msg_iov, used);
			if (err) {
				/* Exception. Bailout! */
				if (!copied)
					copied = -EFAULT;
				break;
			}
		}

		*seq += used;
		copied += used;
		len -= used;

		tcp_rcv_space_adjust(sk);

skip_copy:
		if (tp->urg_data && after(tp->copied_seq, tp->urg_seq)) {
			tp->urg_data = 0;
			tcp_fast_path_check(sk, tp);
		}
		if (used + offset < skb->len)
			continue;

		if (skb->h.th->fin)
			goto found_fin_ok;
		if (!(flags & MSG_PEEK))
			sk_eat_skb(sk, skb);
		continue;

	found_fin_ok:
		/* Process the FIN. */
		++*seq;
		if (!(flags & MSG_PEEK))
			sk_eat_skb(sk, skb);
		break;
	} while (len > 0);

	if (user_recv) {
		if (skb_queue_len(&tp->ucopy.prequeue)) {
			int chunk;

			tp->ucopy.len = copied > 0 ? len : 0;

			tcp_prequeue_process(sk);

			if (copied > 0 && (chunk = len - tp->ucopy.len) != 0) {
				NET_ADD_STATS_USER(LINUX_MIB_TCPDIRECTCOPYFROMPREQUEUE, chunk);
				len -= chunk;
				copied += chunk;
			}
		}

		tp->ucopy.task = NULL;
		tp->ucopy.len = 0;
	}

	/* According to UNIX98, msg_name/msg_namelen are ignored
	 * on connected socket. I was just happy when found this 8) --ANK
	 */

	/* Clean up data we have read: This will do ACK frames. */
	cleanup_rbuf(sk, copied);

	TCP_CHECK_TIMER(sk);
	release_sock(sk);
	return copied;

out:
	TCP_CHECK_TIMER(sk);
	release_sock(sk);
	return err;

recv_urg:
	err = tcp_recv_urg(sk, timeo, msg, len, flags, addr_len);
	goto out;
}

/*
 *	State processing on a close. This implements the state shift for
 *	sending our FIN frame. Note that we only send a FIN for some
 *	states. A shutdown() may have already sent the FIN, or we may be
 *	closed.
 */

static unsigned char new_state[16] = {
  /* current state:        new state:      action:	*/
  /* (Invalid)		*/ TCP_CLOSE,
  /* TCP_ESTABLISHED	*/ TCP_FIN_WAIT1 | TCP_ACTION_FIN,
  /* TCP_SYN_SENT	*/ TCP_CLOSE,
  /* TCP_SYN_RECV	*/ TCP_FIN_WAIT1 | TCP_ACTION_FIN,
  /* TCP_FIN_WAIT1	*/ TCP_FIN_WAIT1,
  /* TCP_FIN_WAIT2	*/ TCP_FIN_WAIT2,
  /* TCP_TIME_WAIT	*/ TCP_CLOSE,
  /* TCP_CLOSE		*/ TCP_CLOSE,
  /* TCP_CLOSE_WAIT	*/ TCP_LAST_ACK  | TCP_ACTION_FIN,
  /* TCP_LAST_ACK	*/ TCP_LAST_ACK,
  /* TCP_LISTEN		*/ TCP_CLOSE,
  /* TCP_CLOSING	*/ TCP_CLOSING,
};

static int tcp_close_state(struct sock *sk)
{
	int next = (int)new_state[sk->sk_state];
	int ns = next & TCP_STATE_MASK;

	tcp_set_state(sk, ns);

	return next & TCP_ACTION_FIN;
}

/*
 *	Shutdown the sending side of a connection. Much like close except
 *	that we don't receive shut down or set_sock_flag(sk, SOCK_DEAD).
 */

void tcp_shutdown(struct sock *sk, int how)
{
	/*	We need to grab some memory, and put together a FIN,
	 *	and then put it into the queue to be sent.
	 *		Tim MacKenzie(tym@dibbler.cs.monash.edu.au) 4 Dec '92.
	 */
	if (!(how & SEND_SHUTDOWN))
		return;

	/* If we've already sent a FIN, or it's a closed state, skip this. */
	if ((1 << sk->sk_state) &
	    (TCPF_ESTABLISHED | TCPF_SYN_SENT |
	     TCPF_SYN_RECV | TCPF_CLOSE_WAIT)) {
		/* Clear out any half completed packets.  FIN if needed. */
		if (tcp_close_state(sk))
			tcp_send_fin(sk);
	}
}

/*
 * At this point, there should be no process reference to this
 * socket, and thus no user references at all.  Therefore we
 * can assume the socket waitqueue is inactive and nobody will
 * try to jump onto it.
 */
void tcp_destroy_sock(struct sock *sk)
{
	BUG_TRAP(sk->sk_state == TCP_CLOSE);
	BUG_TRAP(sock_flag(sk, SOCK_DEAD));

	/* It cannot be in hash table! */
	BUG_TRAP(sk_unhashed(sk));

	/* If it has not 0 inet_sk(sk)->num, it must be bound */
	BUG_TRAP(!inet_sk(sk)->num || tcp_sk(sk)->bind_hash);

#ifdef TCP_DEBUG
	if (sk->sk_zapped) {
		printk(KERN_DEBUG "TCP: double destroy sk=%p\n", sk);
		sock_hold(sk);
	}
	sk->sk_zapped = 1;
#endif

	sk->sk_prot->destroy(sk);

	sk_stream_kill_queues(sk);

	xfrm_sk_free_policy(sk);

#ifdef INET_REFCNT_DEBUG
	if (atomic_read(&sk->sk_refcnt) != 1) {
		printk(KERN_DEBUG "Destruction TCP %p delayed, c=%d\n",
		       sk, atomic_read(&sk->sk_refcnt));
	}
#endif

	atomic_dec(&tcp_orphan_count);
	sock_put(sk);
}

void tcp_close(struct sock *sk, long timeout)
{
	struct sk_buff *skb;
	int data_was_unread = 0;

	lock_sock(sk);
	sk->sk_shutdown = SHUTDOWN_MASK;

	if (sk->sk_state == TCP_LISTEN) {
		tcp_set_state(sk, TCP_CLOSE);

		/* Special case. */
		tcp_listen_stop(sk);

		goto adjudge_to_death;
	}

	/*  We need to flush the recv. buffs.  We do this only on the
	 *  descriptor close, not protocol-sourced closes, because the
	 *  reader process may not have drained the data yet!
	 */
	while ((skb = __skb_dequeue(&sk->sk_receive_queue)) != NULL) {
		u32 len = TCP_SKB_CB(skb)->end_seq - TCP_SKB_CB(skb)->seq -
			  skb->h.th->fin;
		data_was_unread += len;
		__kfree_skb(skb);
	}

	sk_stream_mem_reclaim(sk);

	/* As outlined in draft-ietf-tcpimpl-prob-03.txt, section
	 * 3.10, we send a RST here because data was lost.  To
	 * witness the awful effects of the old behavior of always
	 * doing a FIN, run an older 2.1.x kernel or 2.0.x, start
	 * a bulk GET in an FTP client, suspend the process, wait
	 * for the client to advertise a zero window, then kill -9
	 * the FTP client, wheee...  Note: timeout is always zero
	 * in such a case.
	 */
	if (data_was_unread) {
		/* Unread data was tossed, zap the connection. */
		NET_INC_STATS_USER(LINUX_MIB_TCPABORTONCLOSE);
		tcp_set_state(sk, TCP_CLOSE);
		tcp_send_active_reset(sk, GFP_KERNEL);
	} else if (sock_flag(sk, SOCK_LINGER) && !sk->sk_lingertime) {
		/* Check zero linger _after_ checking for unread data. */
		sk->sk_prot->disconnect(sk, 0);
		NET_INC_STATS_USER(LINUX_MIB_TCPABORTONDATA);
	} else if (tcp_close_state(sk)) {
		/* We FIN if the application ate all the data before
		 * zapping the connection.
		 */

		/* RED-PEN. Formally speaking, we have broken TCP state
		 * machine. State transitions:
		 *
		 * TCP_ESTABLISHED -> TCP_FIN_WAIT1
		 * TCP_SYN_RECV	-> TCP_FIN_WAIT1 (forget it, it's impossible)
		 * TCP_CLOSE_WAIT -> TCP_LAST_ACK
		 *
		 * are legal only when FIN has been sent (i.e. in window),
		 * rather than queued out of window. Purists blame.
		 *
		 * F.e. "RFC state" is ESTABLISHED,
		 * if Linux state is FIN-WAIT-1, but FIN is still not sent.
		 *
		 * The visible declinations are that sometimes
		 * we enter time-wait state, when it is not required really
		 * (harmless), do not send active resets, when they are
		 * required by specs (TCP_ESTABLISHED, TCP_CLOSE_WAIT, when
		 * they look as CLOSING or LAST_ACK for Linux)
		 * Probably, I missed some more holelets.
		 * 						--ANK
		 */
		tcp_send_fin(sk);
	}

	sk_stream_wait_close(sk, timeout);

adjudge_to_death:
	/* It is the last release_sock in its life. It will remove backlog. */
	release_sock(sk);


	/* Now socket is owned by kernel and we acquire BH lock
	   to finish close. No need to check for user refs.
	 */
	local_bh_disable();
	bh_lock_sock(sk);
	BUG_TRAP(!sock_owned_by_user(sk));

	sock_hold(sk);
	sock_orphan(sk);

	/*	This is a (useful) BSD violating of the RFC. There is a
	 *	problem with TCP as specified in that the other end could
	 *	keep a socket open forever with no application left this end.
	 *	We use a 3 minute timeout (about the same as BSD) then kill
	 *	our end. If they send after that then tough - BUT: long enough
	 *	that we won't make the old 4*rto = almost no time - whoops
	 *	reset mistake.
	 *
	 *	Nope, it was not mistake. It is really desired behaviour
	 *	f.e. on http servers, when such sockets are useless, but
	 *	consume significant resources. Let's do it with special
	 *	linger2	option.					--ANK
	 */

	if (sk->sk_state == TCP_FIN_WAIT2) {
		struct tcp_opt *tp = tcp_sk(sk);
		if (tp->linger2 < 0) {
			tcp_set_state(sk, TCP_CLOSE);
			tcp_send_active_reset(sk, GFP_ATOMIC);
			NET_INC_STATS_BH(LINUX_MIB_TCPABORTONLINGER);
		} else {
			int tmo = tcp_fin_time(tp);

			if (tmo > TCP_TIMEWAIT_LEN) {
				tcp_reset_keepalive_timer(sk, tcp_fin_time(tp));
			} else {
				atomic_inc(&tcp_orphan_count);
				tcp_time_wait(sk, TCP_FIN_WAIT2, tmo);
				goto out;
			}
		}
	}
	if (sk->sk_state != TCP_CLOSE) {
		sk_stream_mem_reclaim(sk);
		if (atomic_read(&tcp_orphan_count) > sysctl_tcp_max_orphans ||
		    (sk->sk_wmem_queued > SOCK_MIN_SNDBUF &&
		     atomic_read(&tcp_memory_allocated) > sysctl_tcp_mem[2])) {
			if (net_ratelimit())
				printk(KERN_INFO "TCP: too many of orphaned "
				       "sockets\n");
			tcp_set_state(sk, TCP_CLOSE);
			tcp_send_active_reset(sk, GFP_ATOMIC);
			NET_INC_STATS_BH(LINUX_MIB_TCPABORTONMEMORY);
		}
	}
	atomic_inc(&tcp_orphan_count);

	if (sk->sk_state == TCP_CLOSE)
		tcp_destroy_sock(sk);
	/* Otherwise, socket is reprieved until protocol close. */

out:
	bh_unlock_sock(sk);
	local_bh_enable();
	sock_put(sk);
}

/* These states need RST on ABORT according to RFC793 */

static inline int tcp_need_reset(int state)
{
	return (1 << state) &
	       (TCPF_ESTABLISHED | TCPF_CLOSE_WAIT | TCPF_FIN_WAIT1 |
		TCPF_FIN_WAIT2 | TCPF_SYN_RECV);
}

int tcp_disconnect(struct sock *sk, int flags)
{
	struct inet_opt *inet = inet_sk(sk);
	struct tcp_opt *tp = tcp_sk(sk);
	int err = 0;
	int old_state = sk->sk_state;

	if (old_state != TCP_CLOSE)
		tcp_set_state(sk, TCP_CLOSE);

	/* ABORT function of RFC793 */
	if (old_state == TCP_LISTEN) {
		tcp_listen_stop(sk);
	} else if (tcp_need_reset(old_state) ||
		   (tp->snd_nxt != tp->write_seq &&
		    (1 << old_state) & (TCPF_CLOSING | TCPF_LAST_ACK))) {
		/* The last check adjusts for discrepance of Linux wrt. RFC
		 * states
		 */
		tcp_send_active_reset(sk, gfp_any());
		sk->sk_err = ECONNRESET;
	} else if (old_state == TCP_SYN_SENT)
		sk->sk_err = ECONNRESET;

	tcp_clear_xmit_timers(sk);
	__skb_queue_purge(&sk->sk_receive_queue);
	sk_stream_writequeue_purge(sk);
	__skb_queue_purge(&tp->out_of_order_queue);

	inet->dport = 0;

	if (!(sk->sk_userlocks & SOCK_BINDADDR_LOCK))
		inet_reset_saddr(sk);

	sk->sk_shutdown = 0;
	sock_reset_flag(sk, SOCK_DONE);
	tp->srtt = 0;
	if ((tp->write_seq += tp->max_window + 2) == 0)
		tp->write_seq = 1;
	tp->backoff = 0;
	tp->snd_cwnd = 2;
	tp->probes_out = 0;
	tp->packets_out = 0;
	tp->snd_ssthresh = 0x7fffffff;
	tp->snd_cwnd_cnt = 0;
	tcp_set_ca_state(tp, TCP_CA_Open);
	tcp_clear_retrans(tp);
	tcp_delack_init(tp);
	sk->sk_send_head = NULL;
	tp->saw_tstamp = 0;
	tcp_sack_reset(tp);
	__sk_dst_reset(sk);

	BUG_TRAP(!inet->num || tp->bind_hash);

	sk->sk_error_report(sk);
	return err;
}

/*
 *	Wait for an incoming connection, avoid race
 *	conditions. This must be called with the socket locked.
 */
static int wait_for_connect(struct sock *sk, long timeo)
{
	struct tcp_opt *tp = tcp_sk(sk);
	DEFINE_WAIT(wait);
	int err;

	/*
	 * True wake-one mechanism for incoming connections: only
	 * one process gets woken up, not the 'whole herd'.
	 * Since we do not 'race & poll' for established sockets
	 * anymore, the common case will execute the loop only once.
	 *
	 * Subtle issue: "add_wait_queue_exclusive()" will be added
	 * after any current non-exclusive waiters, and we know that
	 * it will always _stay_ after any new non-exclusive waiters
	 * because all non-exclusive waiters are added at the
	 * beginning of the wait-queue. As such, it's ok to "drop"
	 * our exclusiveness temporarily when we get woken up without
	 * having to remove and re-insert us on the wait queue.
	 */
	for (;;) {
		prepare_to_wait_exclusive(sk->sk_sleep, &wait,
					  TASK_INTERRUPTIBLE);
		release_sock(sk);
		if (!tp->accept_queue)
			timeo = schedule_timeout(timeo);
		lock_sock(sk);
		err = 0;
		if (tp->accept_queue)
			break;
		err = -EINVAL;
		if (sk->sk_state != TCP_LISTEN)
			break;
		err = sock_intr_errno(timeo);
		if (signal_pending(current))
			break;
		err = -EAGAIN;
		if (!timeo)
			break;
	}
	finish_wait(sk->sk_sleep, &wait);
	return err;
}

/*
 *	This will accept the next outstanding connection.
 */

struct sock *tcp_accept(struct sock *sk, int flags, int *err)
{
	struct tcp_opt *tp = tcp_sk(sk);
	struct open_request *req;
	struct sock *newsk;
	int error;

	lock_sock(sk);

	/* We need to make sure that this socket is listening,
	 * and that it has something pending.
	 */
	error = -EINVAL;
	if (sk->sk_state != TCP_LISTEN)
		goto out;

	/* Find already established connection */
	if (!tp->accept_queue) {
		long timeo = sock_rcvtimeo(sk, flags & O_NONBLOCK);

		/* If this is a non blocking socket don't sleep */
		error = -EAGAIN;
		if (!timeo)
			goto out;

		error = wait_for_connect(sk, timeo);
		if (error)
			goto out;
	}

	req = tp->accept_queue;
	if ((tp->accept_queue = req->dl_next) == NULL)
		tp->accept_queue_tail = NULL;

 	newsk = req->sk;
	sk_acceptq_removed(sk);
	tcp_openreq_fastfree(req);
	BUG_TRAP(newsk->sk_state != TCP_SYN_RECV);
	release_sock(sk);
	return newsk;

out:
	release_sock(sk);
	*err = error;
	return NULL;
}

/*
 *	Socket option code for TCP.
 */
int tcp_setsockopt(struct sock *sk, int level, int optname, char __user *optval,
		   int optlen)
{
	struct tcp_opt *tp = tcp_sk(sk);
	int val;
	int err = 0;

	if (level != SOL_TCP)
		return tp->af_specific->setsockopt(sk, level, optname,
						   optval, optlen);

	if (optlen < sizeof(int))
		return -EINVAL;

	if (get_user(val, (int __user *)optval))
		return -EFAULT;

	lock_sock(sk);

	switch (optname) {
	case TCP_MAXSEG:
		/* Values greater than interface MTU won't take effect. However
		 * at the point when this call is done we typically don't yet
		 * know which interface is going to be used */
		if (val < 8 || val > MAX_TCP_WINDOW) {
			err = -EINVAL;
			break;
		}
		tp->user_mss = val;
		break;

	case TCP_NODELAY:
		if (val) {
			/* TCP_NODELAY is weaker than TCP_CORK, so that
			 * this option on corked socket is remembered, but
			 * it is not activated until cork is cleared.
			 *
			 * However, when TCP_NODELAY is set we make
			 * an explicit push, which overrides even TCP_CORK
			 * for currently queued segments.
			 */
			tp->nonagle |= TCP_NAGLE_OFF|TCP_NAGLE_PUSH;
			tcp_push_pending_frames(sk, tp);
		} else {
			tp->nonagle &= ~TCP_NAGLE_OFF;
		}
		break;

	case TCP_CORK:
		/* When set indicates to always queue non-full frames.
		 * Later the user clears this option and we transmit
		 * any pending partial frames in the queue.  This is
		 * meant to be used alongside sendfile() to get properly
		 * filled frames when the user (for example) must write
		 * out headers with a write() call first and then use
		 * sendfile to send out the data parts.
		 *
		 * TCP_CORK can be set together with TCP_NODELAY and it is
		 * stronger than TCP_NODELAY.
		 */
		if (val) {
			tp->nonagle |= TCP_NAGLE_CORK;
		} else {
			tp->nonagle &= ~TCP_NAGLE_CORK;
			if (tp->nonagle&TCP_NAGLE_OFF)
				tp->nonagle |= TCP_NAGLE_PUSH;
			tcp_push_pending_frames(sk, tp);
		}
		break;

	case TCP_KEEPIDLE:
		if (val < 1 || val > MAX_TCP_KEEPIDLE)
			err = -EINVAL;
		else {
			tp->keepalive_time = val * HZ;
			if (sock_flag(sk, SOCK_KEEPOPEN) &&
			    !((1 << sk->sk_state) &
			      (TCPF_CLOSE | TCPF_LISTEN))) {
				__u32 elapsed = tcp_time_stamp - tp->rcv_tstamp;
				if (tp->keepalive_time > elapsed)
					elapsed = tp->keepalive_time - elapsed;
				else
					elapsed = 0;
				tcp_reset_keepalive_timer(sk, elapsed);
			}
		}
		break;
	case TCP_KEEPINTVL:
		if (val < 1 || val > MAX_TCP_KEEPINTVL)
			err = -EINVAL;
		else
			tp->keepalive_intvl = val * HZ;
		break;
	case TCP_KEEPCNT:
		if (val < 1 || val > MAX_TCP_KEEPCNT)
			err = -EINVAL;
		else
			tp->keepalive_probes = val;
		break;
	case TCP_SYNCNT:
		if (val < 1 || val > MAX_TCP_SYNCNT)
			err = -EINVAL;
		else
			tp->syn_retries = val;
		break;

	case TCP_LINGER2:
		if (val < 0)
			tp->linger2 = -1;
		else if (val > sysctl_tcp_fin_timeout / HZ)
			tp->linger2 = 0;
		else
			tp->linger2 = val * HZ;
		break;

	case TCP_DEFER_ACCEPT:
		tp->defer_accept = 0;
		if (val > 0) {
			/* Translate value in seconds to number of
			 * retransmits */
			while (tp->defer_accept < 32 &&
			       val > ((TCP_TIMEOUT_INIT / HZ) <<
				       tp->defer_accept))
				tp->defer_accept++;
			tp->defer_accept++;
		}
		break;

	case TCP_WINDOW_CLAMP:
		if (!val) {
			if (sk->sk_state != TCP_CLOSE) {
				err = -EINVAL;
				break;
			}
			tp->window_clamp = 0;
		} else
			tp->window_clamp = val < SOCK_MIN_RCVBUF / 2 ?
						SOCK_MIN_RCVBUF / 2 : val;
		break;

	case TCP_QUICKACK:
		if (!val) {
			tp->ack.pingpong = 1;
		} else {
			tp->ack.pingpong = 0;
			if ((1 << sk->sk_state) &
			    (TCPF_ESTABLISHED | TCPF_CLOSE_WAIT) &&
			    tcp_ack_scheduled(tp)) {
				tp->ack.pending |= TCP_ACK_PUSHED;
				cleanup_rbuf(sk, 1);
				if (!(val & 1))
					tp->ack.pingpong = 1;
			}
		}
		break;

	default:
		err = -ENOPROTOOPT;
		break;
	};
	release_sock(sk);
	return err;
}

int tcp_getsockopt(struct sock *sk, int level, int optname, char __user *optval,
		   int __user *optlen)
{
	struct tcp_opt *tp = tcp_sk(sk);
	int val, len;

	if (level != SOL_TCP)
		return tp->af_specific->getsockopt(sk, level, optname,
						   optval, optlen);

	if (get_user(len, optlen))
		return -EFAULT;

	len = min_t(unsigned int, len, sizeof(int));

	if (len < 0)
		return -EINVAL;

	switch (optname) {
	case TCP_MAXSEG:
		val = tp->mss_cache_std;
		if (!val && ((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_LISTEN)))
			val = tp->user_mss;
		break;
	case TCP_NODELAY:
		val = !!(tp->nonagle&TCP_NAGLE_OFF);
		break;
	case TCP_CORK:
		val = !!(tp->nonagle&TCP_NAGLE_CORK);
		break;
	case TCP_KEEPIDLE:
		val = (tp->keepalive_time ? : sysctl_tcp_keepalive_time) / HZ;
		break;
	case TCP_KEEPINTVL:
		val = (tp->keepalive_intvl ? : sysctl_tcp_keepalive_intvl) / HZ;
		break;
	case TCP_KEEPCNT:
		val = tp->keepalive_probes ? : sysctl_tcp_keepalive_probes;
		break;
	case TCP_SYNCNT:
		val = tp->syn_retries ? : sysctl_tcp_syn_retries;
		break;
	case TCP_LINGER2:
		val = tp->linger2;
		if (val >= 0)
			val = (val ? : sysctl_tcp_fin_timeout) / HZ;
		break;
	case TCP_DEFER_ACCEPT:
		val = !tp->defer_accept ? 0 : ((TCP_TIMEOUT_INIT / HZ) <<
					       (tp->defer_accept - 1));
		break;
	case TCP_WINDOW_CLAMP:
		val = tp->window_clamp;
		break;
	case TCP_INFO: {
		struct tcp_info info;

		if (get_user(len, optlen))
			return -EFAULT;

		tcp_get_info(sk, &info);

		len = min_t(unsigned int, len, sizeof(info));
		if (put_user(len, optlen))
			return -EFAULT;
		if (copy_to_user(optval, &info, len))
			return -EFAULT;
		return 0;
	}
	case TCP_QUICKACK:
		val = !tp->ack.pingpong;
		break;
	default:
		return -ENOPROTOOPT;
	};

	if (put_user(len, optlen))
		return -EFAULT;
	if (copy_to_user(optval, &val, len))
		return -EFAULT;
	return 0;
}


extern void __skb_cb_too_small_for_tcp(int, int);
extern void tcpdiag_init(void);

static __initdata unsigned long thash_entries;
static int __init set_thash_entries(char *str)
{
	if (!str)
		return 0;
	thash_entries = simple_strtoul(str, &str, 0);
	return 1;
}
__setup("thash_entries=", set_thash_entries);

void __init tcp_init(void)
{
	struct sk_buff *skb = NULL;
	unsigned long goal;
	int order, i;

	if (sizeof(struct tcp_skb_cb) > sizeof(skb->cb))
		__skb_cb_too_small_for_tcp(sizeof(struct tcp_skb_cb),
					   sizeof(skb->cb));

	tcp_openreq_cachep = kmem_cache_create("tcp_open_request",
						   sizeof(struct open_request),
					       0, SLAB_HWCACHE_ALIGN,
					       NULL, NULL);
	if (!tcp_openreq_cachep)
		panic("tcp_init: Cannot alloc open_request cache.");

	tcp_bucket_cachep = kmem_cache_create("tcp_bind_bucket",
					      sizeof(struct tcp_bind_bucket),
					      0, SLAB_HWCACHE_ALIGN,
					      NULL, NULL);
	if (!tcp_bucket_cachep)
		panic("tcp_init: Cannot alloc tcp_bind_bucket cache.");

	tcp_timewait_cachep = kmem_cache_create("tcp_tw_bucket",
						sizeof(struct tcp_tw_bucket),
						0, SLAB_HWCACHE_ALIGN,
						NULL, NULL);
	if (!tcp_timewait_cachep)
		panic("tcp_init: Cannot alloc tcp_tw_bucket cache.");

	/* Size and allocate the main established and bind bucket
	 * hash tables.
	 *
	 * The methodology is similar to that of the buffer cache.
	 */
	if (num_physpages >= (128 * 1024))
		goal = num_physpages >> (21 - PAGE_SHIFT);
	else
		goal = num_physpages >> (23 - PAGE_SHIFT);

	if (thash_entries)
		goal = (thash_entries * sizeof(struct tcp_ehash_bucket)) >> PAGE_SHIFT;
	for (order = 0; (1UL << order) < goal; order++)
		;
	do {
		tcp_ehash_size = (1UL << order) * PAGE_SIZE /
			sizeof(struct tcp_ehash_bucket);
		tcp_ehash_size >>= 1;
		while (tcp_ehash_size & (tcp_ehash_size - 1))
			tcp_ehash_size--;
		tcp_ehash = (struct tcp_ehash_bucket *)
			__get_free_pages(GFP_ATOMIC, order);
	} while (!tcp_ehash && --order > 0);

	if (!tcp_ehash)
		panic("Failed to allocate TCP established hash table\n");
	for (i = 0; i < (tcp_ehash_size << 1); i++) {
		tcp_ehash[i].lock = RW_LOCK_UNLOCKED;
		INIT_HLIST_HEAD(&tcp_ehash[i].chain);
	}

	do {
		tcp_bhash_size = (1UL << order) * PAGE_SIZE /
			sizeof(struct tcp_bind_hashbucket);
		if ((tcp_bhash_size > (64 * 1024)) && order > 0)
			continue;
		tcp_bhash = (struct tcp_bind_hashbucket *)
			__get_free_pages(GFP_ATOMIC, order);
	} while (!tcp_bhash && --order >= 0);

	if (!tcp_bhash)
		panic("Failed to allocate TCP bind hash table\n");
	for (i = 0; i < tcp_bhash_size; i++) {
		tcp_bhash[i].lock = SPIN_LOCK_UNLOCKED;
		INIT_HLIST_HEAD(&tcp_bhash[i].chain);
	}

	/* Try to be a bit smarter and adjust defaults depending
	 * on available memory.
	 */
	if (order > 4) {
		sysctl_local_port_range[0] = 32768;
		sysctl_local_port_range[1] = 61000;
		sysctl_tcp_max_tw_buckets = 180000;
		sysctl_tcp_max_orphans = 4096 << (order - 4);
		sysctl_max_syn_backlog = 1024;
	} else if (order < 3) {
		sysctl_local_port_range[0] = 1024 * (3 - order);
		sysctl_tcp_max_tw_buckets >>= (3 - order);
		sysctl_tcp_max_orphans >>= (3 - order);
		sysctl_max_syn_backlog = 128;
	}
	tcp_port_rover = sysctl_local_port_range[0] - 1;

	sysctl_tcp_mem[0] =  768 << order;
	sysctl_tcp_mem[1] = 1024 << order;
	sysctl_tcp_mem[2] = 1536 << order;

	if (order < 3) {
		sysctl_tcp_wmem[2] = 64 * 1024;
		sysctl_tcp_rmem[0] = PAGE_SIZE;
		sysctl_tcp_rmem[1] = 43689;
		sysctl_tcp_rmem[2] = 2 * 43689;
	}

	printk(KERN_INFO "TCP: Hash tables configured "
	       "(established %d bind %d)\n",
	       tcp_ehash_size << 1, tcp_bhash_size);

	tcpdiag_init();
}

EXPORT_SYMBOL(tcp_accept);
EXPORT_SYMBOL(tcp_close);
EXPORT_SYMBOL(tcp_close_state);
EXPORT_SYMBOL(tcp_destroy_sock);
EXPORT_SYMBOL(tcp_disconnect);
EXPORT_SYMBOL(tcp_getsockopt);
EXPORT_SYMBOL(tcp_ioctl);
EXPORT_SYMBOL(tcp_openreq_cachep);
EXPORT_SYMBOL(tcp_poll);
EXPORT_SYMBOL(tcp_read_sock);
EXPORT_SYMBOL(tcp_recvmsg);
EXPORT_SYMBOL(tcp_sendmsg);
EXPORT_SYMBOL(tcp_sendpage);
EXPORT_SYMBOL(tcp_setsockopt);
EXPORT_SYMBOL(tcp_shutdown);
EXPORT_SYMBOL(tcp_statistics);
EXPORT_SYMBOL(tcp_timewait_cachep);
