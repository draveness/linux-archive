/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Implementation of the Transmission Control Protocol(TCP).
 *
 * Version:	$Id: tcp_timer.c,v 1.80 2000/10/03 07:29:01 anton Exp $
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
 */

#include <net/tcp.h>

int sysctl_tcp_syn_retries = TCP_SYN_RETRIES; 
int sysctl_tcp_synack_retries = TCP_SYNACK_RETRIES; 
int sysctl_tcp_keepalive_time = TCP_KEEPALIVE_TIME;
int sysctl_tcp_keepalive_probes = TCP_KEEPALIVE_PROBES;
int sysctl_tcp_keepalive_intvl = TCP_KEEPALIVE_INTVL;
int sysctl_tcp_retries1 = TCP_RETR1;
int sysctl_tcp_retries2 = TCP_RETR2;
int sysctl_tcp_orphan_retries;

static void tcp_write_timer(unsigned long);
static void tcp_delack_timer(unsigned long);
static void tcp_keepalive_timer (unsigned long data);

const char timer_bug_msg[] = KERN_DEBUG "tcpbug: unknown timer value\n";

/*
 * Using different timers for retransmit, delayed acks and probes
 * We may wish use just one timer maintaining a list of expire jiffies 
 * to optimize.
 */

void tcp_init_xmit_timers(struct sock *sk)
{
	struct tcp_opt *tp = &sk->tp_pinfo.af_tcp;

	init_timer(&tp->retransmit_timer);
	tp->retransmit_timer.function=&tcp_write_timer;
	tp->retransmit_timer.data = (unsigned long) sk;
	tp->pending = 0;

	init_timer(&tp->delack_timer);
	tp->delack_timer.function=&tcp_delack_timer;
	tp->delack_timer.data = (unsigned long) sk;
	tp->ack.pending = 0;

	init_timer(&sk->timer);
	sk->timer.function=&tcp_keepalive_timer;
	sk->timer.data = (unsigned long) sk;
}

void tcp_clear_xmit_timers(struct sock *sk)
{
	struct tcp_opt *tp = &sk->tp_pinfo.af_tcp;

	tp->pending = 0;
	if (timer_pending(&tp->retransmit_timer) &&
	    del_timer(&tp->retransmit_timer))
		__sock_put(sk);

	tp->ack.pending = 0;
	tp->ack.blocked = 0;
	if (timer_pending(&tp->delack_timer) &&
	    del_timer(&tp->delack_timer))
		__sock_put(sk);

	if(timer_pending(&sk->timer) && del_timer(&sk->timer))
		__sock_put(sk);
}

static void tcp_write_err(struct sock *sk)
{
	sk->err = sk->err_soft ? : ETIMEDOUT;
	sk->error_report(sk);

	tcp_done(sk);
	NET_INC_STATS_BH(TCPAbortOnTimeout);
}

/* Do not allow orphaned sockets to eat all our resources.
 * This is direct violation of TCP specs, but it is required
 * to prevent DoS attacks. It is called when a retransmission timeout
 * or zero probe timeout occurs on orphaned socket.
 *
 * Criterium is still not confirmed experimentally and may change.
 * We kill the socket, if:
 * 1. If number of orphaned sockets exceeds an administratively configured
 *    limit.
 * 2. If we have strong memory pressure.
 */
static int tcp_out_of_resources(struct sock *sk, int do_reset)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	int orphans = atomic_read(&tcp_orphan_count);

	/* If peer does not open window for long time, or did not transmit 
	 * anything for long time, penalize it. */
	if ((s32)(tcp_time_stamp - tp->lsndtime) > 2*TCP_RTO_MAX || !do_reset)
		orphans <<= 1;

	/* If some dubious ICMP arrived, penalize even more. */
	if (sk->err_soft)
		orphans <<= 1;

	if (orphans >= sysctl_tcp_max_orphans ||
	    (sk->wmem_queued > SOCK_MIN_SNDBUF &&
	     atomic_read(&tcp_memory_allocated) > sysctl_tcp_mem[2])) {
		if (net_ratelimit())
			printk(KERN_INFO "Out of socket memory\n");

		/* Catch exceptional cases, when connection requires reset.
		 *      1. Last segment was sent recently. */
		if ((s32)(tcp_time_stamp - tp->lsndtime) <= TCP_TIMEWAIT_LEN ||
		    /*  2. Window is closed. */
		    (!tp->snd_wnd && !tp->packets_out))
			do_reset = 1;
		if (do_reset)
			tcp_send_active_reset(sk, GFP_ATOMIC);
		tcp_done(sk);
		NET_INC_STATS_BH(TCPAbortOnMemory);
		return 1;
	}
	return 0;
}

/* Calculate maximal number or retries on an orphaned socket. */
static int tcp_orphan_retries(struct sock *sk, int alive)
{
	int retries = sysctl_tcp_orphan_retries; /* May be zero. */

	/* We know from an ICMP that something is wrong. */
	if (sk->err_soft && !alive)
		retries = 0;

	/* However, if socket sent something recently, select some safe
	 * number of retries. 8 corresponds to >100 seconds with minimal
	 * RTO of 200msec. */
	if (retries == 0 && alive)
		retries = 8;
	return retries;
}

/* A write timeout has occurred. Process the after effects. */
static int tcp_write_timeout(struct sock *sk)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	int retry_until;

	if ((1<<sk->state)&(TCPF_SYN_SENT|TCPF_SYN_RECV)) {
		if (tp->retransmits)
			dst_negative_advice(&sk->dst_cache);
		retry_until = tp->syn_retries ? : sysctl_tcp_syn_retries;
	} else {
		if (tp->retransmits >= sysctl_tcp_retries1) {
			/* NOTE. draft-ietf-tcpimpl-pmtud-01.txt requires pmtu black
			   hole detection. :-(

			   It is place to make it. It is not made. I do not want
			   to make it. It is disguisting. It does not work in any
			   case. Let me to cite the same draft, which requires for
			   us to implement this:

   "The one security concern raised by this memo is that ICMP black holes
   are often caused by over-zealous security administrators who block
   all ICMP messages.  It is vitally important that those who design and
   deploy security systems understand the impact of strict filtering on
   upper-layer protocols.  The safest web site in the world is worthless
   if most TCP implementations cannot transfer data from it.  It would
   be far nicer to have all of the black holes fixed rather than fixing
   all of the TCP implementations."

                           Golden words :-).
		   */

			dst_negative_advice(&sk->dst_cache);
		}

		retry_until = sysctl_tcp_retries2;
		if (sk->dead) {
			int alive = (tp->rto < TCP_RTO_MAX);
 
			retry_until = tcp_orphan_retries(sk, alive);

			if (tcp_out_of_resources(sk, alive || tp->retransmits < retry_until))
				return 1;
		}
	}

	if (tp->retransmits >= retry_until) {
		/* Has it gone just too far? */
		tcp_write_err(sk);
		return 1;
	}
	return 0;
}

static void tcp_delack_timer(unsigned long data)
{
	struct sock *sk = (struct sock*)data;
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);

	bh_lock_sock(sk);
	if (sk->lock.users) {
		/* Try again later. */
		tp->ack.blocked = 1;
		NET_INC_STATS_BH(DelayedACKLocked);
		if (!mod_timer(&tp->delack_timer, jiffies + TCP_DELACK_MIN))
			sock_hold(sk);
		goto out_unlock;
	}

	tcp_mem_reclaim(sk);

	if (sk->state == TCP_CLOSE || !(tp->ack.pending&TCP_ACK_TIMER))
		goto out;

	if ((long)(tp->ack.timeout - jiffies) > 0) {
		if (!mod_timer(&tp->delack_timer, tp->ack.timeout))
			sock_hold(sk);
		goto out;
	}
	tp->ack.pending &= ~TCP_ACK_TIMER;

	if (skb_queue_len(&tp->ucopy.prequeue)) {
		struct sk_buff *skb;

		net_statistics[smp_processor_id()*2].TCPSchedulerFailed += skb_queue_len(&tp->ucopy.prequeue);

		while ((skb = __skb_dequeue(&tp->ucopy.prequeue)) != NULL)
			sk->backlog_rcv(sk, skb);

		tp->ucopy.memory = 0;
	}

	if (tcp_ack_scheduled(tp)) {
		if (!tp->ack.pingpong) {
			/* Delayed ACK missed: inflate ATO. */
			tp->ack.ato = min(tp->ack.ato<<1, tp->rto);
		} else {
			/* Delayed ACK missed: leave pingpong mode and
			 * deflate ATO.
			 */
			tp->ack.pingpong = 0;
			tp->ack.ato = TCP_ATO_MIN;
		}
		tcp_send_ack(sk);
		NET_INC_STATS_BH(DelayedACKs);
	}
	TCP_CHECK_TIMER(sk);

out:
	if (tcp_memory_pressure)
		tcp_mem_reclaim(sk);
out_unlock:
	bh_unlock_sock(sk);
	sock_put(sk);
}

static void tcp_probe_timer(struct sock *sk)
{
	struct tcp_opt *tp = &sk->tp_pinfo.af_tcp;
	int max_probes;

	if (tp->packets_out || !tp->send_head) {
		tp->probes_out = 0;
		return;
	}

	/* *WARNING* RFC 1122 forbids this
	 *
	 * It doesn't AFAIK, because we kill the retransmit timer -AK
	 *
	 * FIXME: We ought not to do it, Solaris 2.5 actually has fixing
	 * this behaviour in Solaris down as a bug fix. [AC]
	 *
	 * Let me to explain. probes_out is zeroed by incoming ACKs
	 * even if they advertise zero window. Hence, connection is killed only
	 * if we received no ACKs for normal connection timeout. It is not killed
	 * only because window stays zero for some time, window may be zero
	 * until armageddon and even later. We are in full accordance
	 * with RFCs, only probe timer combines both retransmission timeout
	 * and probe timeout in one bottle.				--ANK
	 */
	max_probes = sysctl_tcp_retries2;

	if (sk->dead) {
		int alive = ((tp->rto<<tp->backoff) < TCP_RTO_MAX);
 
		max_probes = tcp_orphan_retries(sk, alive);

		if (tcp_out_of_resources(sk, alive || tp->probes_out <= max_probes))
			return;
	}

	if (tp->probes_out > max_probes) {
		tcp_write_err(sk);
	} else {
		/* Only send another probe if we didn't close things up. */
		tcp_send_probe0(sk);
	}
}

/*
 *	The TCP retransmit timer.
 */

static void tcp_retransmit_timer(struct sock *sk)
{
	struct tcp_opt *tp = &sk->tp_pinfo.af_tcp;

	if (tp->packets_out == 0)
		goto out;

	BUG_TRAP(!skb_queue_empty(&sk->write_queue));

	if (tcp_write_timeout(sk))
		goto out;

	if (tp->retransmits == 0) {
		if (tp->ca_state == TCP_CA_Disorder || tp->ca_state == TCP_CA_Recovery) {
			if (tp->sack_ok) {
				if (tp->ca_state == TCP_CA_Recovery)
					NET_INC_STATS_BH(TCPSackRecoveryFail);
				else
					NET_INC_STATS_BH(TCPSackFailures);
			} else {
				if (tp->ca_state == TCP_CA_Recovery)
					NET_INC_STATS_BH(TCPRenoRecoveryFail);
				else
					NET_INC_STATS_BH(TCPRenoFailures);
			}
		} else if (tp->ca_state == TCP_CA_Loss) {
			NET_INC_STATS_BH(TCPLossFailures);
		} else {
			NET_INC_STATS_BH(TCPTimeouts);
		}
	}

	tcp_enter_loss(sk, 0);

	if (tcp_retransmit_skb(sk, skb_peek(&sk->write_queue)) > 0) {
		/* Retransmission failed because of local congestion,
		 * do not backoff.
		 */
		if (!tp->retransmits)
			tp->retransmits=1;
		tcp_reset_xmit_timer(sk, TCP_TIME_RETRANS,
				     min(tp->rto, TCP_RESOURCE_PROBE_INTERVAL));
		goto out;
	}

	/* Increase the timeout each time we retransmit.  Note that
	 * we do not increase the rtt estimate.  rto is initialized
	 * from rtt, but increases here.  Jacobson (SIGCOMM 88) suggests
	 * that doubling rto each time is the least we can get away with.
	 * In KA9Q, Karn uses this for the first few times, and then
	 * goes to quadratic.  netBSD doubles, but only goes up to *64,
	 * and clamps at 1 to 64 sec afterwards.  Note that 120 sec is
	 * defined in the protocol as the maximum possible RTT.  I guess
	 * we'll have to use something other than TCP to talk to the
	 * University of Mars.
	 *
	 * PAWS allows us longer timeouts and large windows, so once
	 * implemented ftp to mars will work nicely. We will have to fix
	 * the 120 second clamps though!
	 */
	tp->backoff++;
	tp->retransmits++;
	tp->rto = min(tp->rto << 1, TCP_RTO_MAX);
	tcp_reset_xmit_timer(sk, TCP_TIME_RETRANS, tp->rto);
	if (tp->retransmits > sysctl_tcp_retries1)
		__sk_dst_reset(sk);

out:;
}

static void tcp_write_timer(unsigned long data)
{
	struct sock *sk = (struct sock*)data;
	struct tcp_opt *tp = &sk->tp_pinfo.af_tcp;
	int event;

	bh_lock_sock(sk);
	if (sk->lock.users) {
		/* Try again later */
		if (!mod_timer(&tp->retransmit_timer, jiffies + (HZ/20)))
			sock_hold(sk);
		goto out_unlock;
	}

	if (sk->state == TCP_CLOSE || !tp->pending)
		goto out;

	if ((long)(tp->timeout - jiffies) > 0) {
		if (!mod_timer(&tp->retransmit_timer, tp->timeout))
			sock_hold(sk);
		goto out;
	}

	event = tp->pending;
	tp->pending = 0;

	switch (event) {
	case TCP_TIME_RETRANS:
		tcp_retransmit_timer(sk);
		break;
	case TCP_TIME_PROBE0:
		tcp_probe_timer(sk);
		break;
	}
	TCP_CHECK_TIMER(sk);

out:
	tcp_mem_reclaim(sk);
out_unlock:
	bh_unlock_sock(sk);
	sock_put(sk);
}

/*
 *	Timer for listening sockets
 */

static void tcp_synack_timer(struct sock *sk)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	struct tcp_listen_opt *lopt = tp->listen_opt;
	int max_retries = tp->syn_retries ? : sysctl_tcp_synack_retries;
	int thresh = max_retries;
	unsigned long now = jiffies;
	struct open_request **reqp, *req;
	int i, budget;

	if (lopt == NULL || lopt->qlen == 0)
		return;

	/* Normally all the openreqs are young and become mature
	 * (i.e. converted to established socket) for first timeout.
	 * If synack was not acknowledged for 3 seconds, it means
	 * one of the following things: synack was lost, ack was lost,
	 * rtt is high or nobody planned to ack (i.e. synflood).
	 * When server is a bit loaded, queue is populated with old
	 * open requests, reducing effective size of queue.
	 * When server is well loaded, queue size reduces to zero
	 * after several minutes of work. It is not synflood,
	 * it is normal operation. The solution is pruning
	 * too old entries overriding normal timeout, when
	 * situation becomes dangerous.
	 *
	 * Essentially, we reserve half of room for young
	 * embrions; and abort old ones without pity, if old
	 * ones are about to clog our table.
	 */
	if (lopt->qlen>>(lopt->max_qlen_log-1)) {
		int young = (lopt->qlen_young<<1);

		while (thresh > 2) {
			if (lopt->qlen < young)
				break;
			thresh--;
			young <<= 1;
		}
	}

	if (tp->defer_accept)
		max_retries = tp->defer_accept;

	budget = 2*(TCP_SYNQ_HSIZE/(TCP_TIMEOUT_INIT/TCP_SYNQ_INTERVAL));
	i = lopt->clock_hand;

	do {
		reqp=&lopt->syn_table[i];
		while ((req = *reqp) != NULL) {
			if ((long)(now - req->expires) >= 0) {
				if ((req->retrans < thresh ||
				     (req->acked && req->retrans < max_retries))
				    && !req->class->rtx_syn_ack(sk, req, NULL)) {
					unsigned long timeo;

					if (req->retrans++ == 0)
						lopt->qlen_young--;
					timeo = min((TCP_TIMEOUT_INIT << req->retrans),
						    TCP_RTO_MAX);
					req->expires = now + timeo;
					reqp = &req->dl_next;
					continue;
				}

				/* Drop this request */
				write_lock(&tp->syn_wait_lock);
				*reqp = req->dl_next;
				write_unlock(&tp->syn_wait_lock);
				lopt->qlen--;
				if (req->retrans == 0)
					lopt->qlen_young--;
				tcp_openreq_free(req);
				continue;
			}
			reqp = &req->dl_next;
		}

		i = (i+1)&(TCP_SYNQ_HSIZE-1);

	} while (--budget > 0);

	lopt->clock_hand = i;

	if (lopt->qlen)
		tcp_reset_keepalive_timer(sk, TCP_SYNQ_INTERVAL);
}

void tcp_delete_keepalive_timer (struct sock *sk)
{
	if (timer_pending(&sk->timer) && del_timer (&sk->timer))
		__sock_put(sk);
}

void tcp_reset_keepalive_timer (struct sock *sk, unsigned long len)
{
	if (!mod_timer(&sk->timer, jiffies+len))
		sock_hold(sk);
}

void tcp_set_keepalive(struct sock *sk, int val)
{
	if ((1<<sk->state)&(TCPF_CLOSE|TCPF_LISTEN))
		return;

	if (val && !sk->keepopen)
		tcp_reset_keepalive_timer(sk, keepalive_time_when(&sk->tp_pinfo.af_tcp));
	else if (!val)
		tcp_delete_keepalive_timer(sk);
}


static void tcp_keepalive_timer (unsigned long data)
{
	struct sock *sk = (struct sock *) data;
	struct tcp_opt *tp = &sk->tp_pinfo.af_tcp;
	__u32 elapsed;

	/* Only process if socket is not in use. */
	bh_lock_sock(sk);
	if (sk->lock.users) {
		/* Try again later. */ 
		tcp_reset_keepalive_timer (sk, HZ/20);
		goto out;
	}

	if (sk->state == TCP_LISTEN) {
		tcp_synack_timer(sk);
		goto out;
	}

	if (sk->state == TCP_FIN_WAIT2 && sk->dead) {
		if (tp->linger2 >= 0) {
			int tmo = tcp_fin_time(tp) - TCP_TIMEWAIT_LEN;

			if (tmo > 0) {
				tcp_time_wait(sk, TCP_FIN_WAIT2, tmo);
				goto out;
			}
		}
		tcp_send_active_reset(sk, GFP_ATOMIC);
		goto death;
	}

	if (!sk->keepopen || sk->state == TCP_CLOSE)
		goto out;

	elapsed = keepalive_time_when(tp);

	/* It is alive without keepalive 8) */
	if (tp->packets_out || tp->send_head)
		goto resched;

	elapsed = tcp_time_stamp - tp->rcv_tstamp;

	if (elapsed >= keepalive_time_when(tp)) {
		if ((!tp->keepalive_probes && tp->probes_out >= sysctl_tcp_keepalive_probes) ||
		     (tp->keepalive_probes && tp->probes_out >= tp->keepalive_probes)) {
			tcp_send_active_reset(sk, GFP_ATOMIC);
			tcp_write_err(sk);
			goto out;
		}
		if (tcp_write_wakeup(sk) <= 0) {
			tp->probes_out++;
			elapsed = keepalive_intvl_when(tp);
		} else {
			/* If keepalive was lost due to local congestion,
			 * try harder.
			 */
			elapsed = TCP_RESOURCE_PROBE_INTERVAL;
		}
	} else {
		/* It is tp->rcv_tstamp + keepalive_time_when(tp) */
		elapsed = keepalive_time_when(tp) - elapsed;
	}

	TCP_CHECK_TIMER(sk);
	tcp_mem_reclaim(sk);

resched:
	tcp_reset_keepalive_timer (sk, elapsed);
	goto out;

death:	
	tcp_done(sk);

out:
	bh_unlock_sock(sk);
	sock_put(sk);
}
