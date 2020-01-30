/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Implementation of the Transmission Control Protocol(TCP).
 *
 * Version:	$Id: tcp_input.c,v 1.205 2000/12/13 18:31:48 davem Exp $
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

/*
 * Changes:
 *		Pedro Roque	:	Fast Retransmit/Recovery.
 *					Two receive queues.
 *					Retransmit queue handled by TCP.
 *					Better retransmit timer handling.
 *					New congestion avoidance.
 *					Header prediction.
 *					Variable renaming.
 *
 *		Eric		:	Fast Retransmit.
 *		Randy Scott	:	MSS option defines.
 *		Eric Schenk	:	Fixes to slow start algorithm.
 *		Eric Schenk	:	Yet another double ACK bug.
 *		Eric Schenk	:	Delayed ACK bug fixes.
 *		Eric Schenk	:	Floyd style fast retrans war avoidance.
 *		David S. Miller	:	Don't allow zero congestion window.
 *		Eric Schenk	:	Fix retransmitter so that it sends
 *					next packet on ack of previous packet.
 *		Andi Kleen	:	Moved open_request checking here
 *					and process RSTs for open_requests.
 *		Andi Kleen	:	Better prune_queue, and other fixes.
 *		Andrey Savochkin:	Fix RTT measurements in the presnce of
 *					timestamps.
 *		Andrey Savochkin:	Check sequence numbers correctly when
 *					removing SACKs due to in sequence incoming
 *					data segments.
 *		Andi Kleen:		Make sure we never ack data there is not
 *					enough room for. Also make this condition
 *					a fatal error if it might still happen.
 *		Andi Kleen:		Add tcp_measure_rcv_mss to make 
 *					connections with MSS<min(MTU,ann. MSS)
 *					work without delayed acks. 
 *		Andi Kleen:		Process packets with PSH set in the
 *					fast path.
 *		J Hadi Salim:		ECN support
 *	 	Andrei Gurtov,
 *		Pasi Sarolahti,
 *		Panu Kuhlberg:		Experimental audit of TCP (re)transmission
 *					engine. Lots of bugs are found.
 */

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/sysctl.h>
#include <net/tcp.h>
#include <net/inet_common.h>
#include <linux/ipsec.h>


/* These are on by default so the code paths get tested.
 * For the final 2.2 this may be undone at our discretion. -DaveM
 */
int sysctl_tcp_timestamps = 1;
int sysctl_tcp_window_scaling = 1;
int sysctl_tcp_sack = 1;
int sysctl_tcp_fack = 1;
int sysctl_tcp_reordering = TCP_FASTRETRANS_THRESH;
#ifdef CONFIG_INET_ECN
int sysctl_tcp_ecn = 1;
#else
int sysctl_tcp_ecn = 0;
#endif
int sysctl_tcp_dsack = 1;
int sysctl_tcp_app_win = 31;
int sysctl_tcp_adv_win_scale = 2;

int sysctl_tcp_stdurg = 0;
int sysctl_tcp_rfc1337 = 0;
int sysctl_tcp_max_orphans = NR_FILE;

#define FLAG_DATA		0x01 /* Incoming frame contained data.		*/
#define FLAG_WIN_UPDATE		0x02 /* Incoming ACK was a window update.	*/
#define FLAG_DATA_ACKED		0x04 /* This ACK acknowledged new data.		*/
#define FLAG_RETRANS_DATA_ACKED	0x08 /* "" "" some of which was retransmitted.	*/
#define FLAG_SYN_ACKED		0x10 /* This ACK acknowledged SYN.		*/
#define FLAG_DATA_SACKED	0x20 /* New SACK.				*/
#define FLAG_ECE		0x40 /* ECE in this ACK				*/
#define FLAG_DATA_LOST		0x80 /* SACK detected data lossage.		*/
#define FLAG_SLOWPATH		0x100 /* Do not skip RFC checks for window update.*/

#define FLAG_ACKED		(FLAG_DATA_ACKED|FLAG_SYN_ACKED)
#define FLAG_NOT_DUP		(FLAG_DATA|FLAG_WIN_UPDATE|FLAG_ACKED)
#define FLAG_CA_ALERT		(FLAG_DATA_SACKED|FLAG_ECE)
#define FLAG_FORWARD_PROGRESS	(FLAG_ACKED|FLAG_DATA_SACKED)

#define IsReno(tp) ((tp)->sack_ok == 0)
#define IsFack(tp) ((tp)->sack_ok & 2)
#define IsDSack(tp) ((tp)->sack_ok & 4)

#define TCP_REMNANT (TCP_FLAG_FIN|TCP_FLAG_URG|TCP_FLAG_SYN|TCP_FLAG_PSH)

/* Adapt the MSS value used to make delayed ack decision to the 
 * real world.
 */ 
static __inline__ void tcp_measure_rcv_mss(struct tcp_opt *tp, struct sk_buff *skb)
{
	unsigned int len, lss;

	lss = tp->ack.last_seg_size; 
	tp->ack.last_seg_size = 0; 

	/* skb->len may jitter because of SACKs, even if peer
	 * sends good full-sized frames.
	 */
	len = skb->len;
	if (len >= tp->ack.rcv_mss) {
		tp->ack.rcv_mss = len;
		/* Dubious? Rather, it is final cut. 8) */
		if (tcp_flag_word(skb->h.th)&TCP_REMNANT)
			tp->ack.pending |= TCP_ACK_PUSHED;
	} else {
		/* Otherwise, we make more careful check taking into account,
		 * that SACKs block is variable.
		 *
		 * "len" is invariant segment length, including TCP header.
		 */
		len = skb->tail - skb->h.raw;
		if (len >= TCP_MIN_RCVMSS + sizeof(struct tcphdr) ||
		    /* If PSH is not set, packet should be
		     * full sized, provided peer TCP is not badly broken.
		     * This observation (if it is correct 8)) allows
		     * to handle super-low mtu links fairly.
		     */
		    (len >= TCP_MIN_MSS + sizeof(struct tcphdr) &&
		     !(tcp_flag_word(skb->h.th)&TCP_REMNANT))) {
			/* Subtract also invariant (if peer is RFC compliant),
			 * tcp header plus fixed timestamp option length.
			 * Resulting "len" is MSS free of SACK jitter.
			 */
			len -= tp->tcp_header_len;
			tp->ack.last_seg_size = len;
			if (len == lss) {
				tp->ack.rcv_mss = len;
				return;
			}
		}
		tp->ack.pending |= TCP_ACK_PUSHED;
	}
}

static void tcp_incr_quickack(struct tcp_opt *tp)
{
	unsigned quickacks = tp->rcv_wnd/(2*tp->ack.rcv_mss);

	if (quickacks==0)
		quickacks=2;
	if (quickacks > tp->ack.quick)
		tp->ack.quick = min(quickacks, TCP_MAX_QUICKACKS);
}

void tcp_enter_quickack_mode(struct tcp_opt *tp)
{
	tcp_incr_quickack(tp);
	tp->ack.pingpong = 0;
	tp->ack.ato = TCP_ATO_MIN;
}

/* Send ACKs quickly, if "quick" count is not exhausted
 * and the session is not interactive.
 */

static __inline__ int tcp_in_quickack_mode(struct tcp_opt *tp)
{
	return (tp->ack.quick && !tp->ack.pingpong);
}

/* Buffer size and advertised window tuning.
 *
 * 1. Tuning sk->sndbuf, when connection enters established state.
 */

static void tcp_fixup_sndbuf(struct sock *sk)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	int sndmem = tp->mss_clamp+MAX_TCP_HEADER+16+sizeof(struct sk_buff);

	if (sk->sndbuf < 3*sndmem)
		sk->sndbuf = min(3*sndmem, sysctl_tcp_wmem[2]);
}

/* 2. Tuning advertised window (window_clamp, rcv_ssthresh)
 *
 * All tcp_full_space() is split to two parts: "network" buffer, allocated
 * forward and advertised in receiver window (tp->rcv_wnd) and
 * "application buffer", required to isolate scheduling/application
 * latencies from network.
 * window_clamp is maximal advertised window. It can be less than
 * tcp_full_space(), in this case tcp_full_space() - window_clamp
 * is reserved for "application" buffer. The less window_clamp is
 * the smoother our behaviour from viewpoint of network, but the lower
 * throughput and the higher sensitivity of the connection to losses. 8)
 *
 * rcv_ssthresh is more strict window_clamp used at "slow start"
 * phase to predict further behaviour of this connection.
 * It is used for two goals:
 * - to enforce header prediction at sender, even when application
 *   requires some significant "application buffer". It is check #1.
 * - to prevent pruning of receive queue because of misprediction
 *   of receiver window. Check #2.
 *
 * The scheme does not work when sender sends good segments opening
 * window and then starts to feed us spagetti. But it should work
 * in common situations. Otherwise, we have to rely on queue collapsing.
 */

/* Slow part of check#2. */
static int
__tcp_grow_window(struct sock *sk, struct tcp_opt *tp, struct sk_buff *skb)
{
	/* Optimize this! */
	int truesize = tcp_win_from_space(skb->truesize)/2;
	int window = tcp_full_space(sk)/2;

	while (tp->rcv_ssthresh <= window) {
		if (truesize <= skb->len)
			return 2*tp->ack.rcv_mss;

		truesize >>= 1;
		window >>= 1;
	}
	return 0;
}

static __inline__ void
tcp_grow_window(struct sock *sk, struct tcp_opt *tp, struct sk_buff *skb)
{
	/* Check #1 */
	if (tp->rcv_ssthresh < tp->window_clamp &&
	    (int)tp->rcv_ssthresh < tcp_space(sk) &&
	    !tcp_memory_pressure) {
		int incr;

		/* Check #2. Increase window, if skb with such overhead
		 * will fit to rcvbuf in future.
		 */
		if (tcp_win_from_space(skb->truesize) <= skb->len)
			incr = 2*tp->advmss;
		else
			incr = __tcp_grow_window(sk, tp, skb);

		if (incr) {
			tp->rcv_ssthresh = min(tp->rcv_ssthresh + incr, tp->window_clamp);
			tp->ack.quick |= 1;
		}
	}
}

/* 3. Tuning rcvbuf, when connection enters established state. */

static void tcp_fixup_rcvbuf(struct sock *sk)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	int rcvmem = tp->advmss+MAX_TCP_HEADER+16+sizeof(struct sk_buff);

	/* Try to select rcvbuf so that 4 mss-sized segments
	 * will fit to window and correspoding skbs will fit to our rcvbuf.
	 * (was 3; 4 is minimum to allow fast retransmit to work.)
	 */
	while (tcp_win_from_space(rcvmem) < tp->advmss)
		rcvmem += 128;
	if (sk->rcvbuf < 4*rcvmem)
		sk->rcvbuf = min(4*rcvmem, sysctl_tcp_rmem[2]);
}

/* 4. Try to fixup all. It is made iimediately after connection enters
 *    established state.
 */
static void tcp_init_buffer_space(struct sock *sk)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	int maxwin;

	if (!(sk->userlocks&SOCK_RCVBUF_LOCK))
		tcp_fixup_rcvbuf(sk);
	if (!(sk->userlocks&SOCK_SNDBUF_LOCK))
		tcp_fixup_sndbuf(sk);

	maxwin = tcp_full_space(sk);

	if (tp->window_clamp >= maxwin) {
		tp->window_clamp = maxwin;

		if (sysctl_tcp_app_win && maxwin>4*tp->advmss)
			tp->window_clamp = max(maxwin-(maxwin>>sysctl_tcp_app_win), 4*tp->advmss);
	}

	/* Force reservation of one segment. */
	if (sysctl_tcp_app_win &&
	    tp->window_clamp > 2*tp->advmss &&
	    tp->window_clamp + tp->advmss > maxwin)
		tp->window_clamp = max(2*tp->advmss, maxwin-tp->advmss);

	tp->rcv_ssthresh = min(tp->rcv_ssthresh, tp->window_clamp);
	tp->snd_cwnd_stamp = tcp_time_stamp;
}

/* 5. Recalculate window clamp after socket hit its memory bounds. */
static void tcp_clamp_window(struct sock *sk, struct tcp_opt *tp)
{
	struct sk_buff *skb;
	int app_win = tp->rcv_nxt - tp->copied_seq;
	int ofo_win = 0;

	tp->ack.quick = 0;

	skb_queue_walk(&tp->out_of_order_queue, skb) {
		ofo_win += skb->len;
	}

	/* If overcommit is due to out of order segments,
	 * do not clamp window. Try to expand rcvbuf instead.
	 */
	if (ofo_win) {
		if (sk->rcvbuf < sysctl_tcp_rmem[2] &&
		    !(sk->userlocks&SOCK_RCVBUF_LOCK) &&
		    !tcp_memory_pressure &&
		    atomic_read(&tcp_memory_allocated) < sysctl_tcp_mem[0])
			sk->rcvbuf = min(atomic_read(&sk->rmem_alloc), sysctl_tcp_rmem[2]);
	}
	if (atomic_read(&sk->rmem_alloc) > sk->rcvbuf) {
		app_win += ofo_win;
		if (atomic_read(&sk->rmem_alloc) >= 2*sk->rcvbuf)
			app_win >>= 1;
		if (app_win > tp->ack.rcv_mss)
			app_win -= tp->ack.rcv_mss;
		app_win = max(app_win, 2*tp->advmss);

		if (!ofo_win)
			tp->window_clamp = min(tp->window_clamp, app_win);
		tp->rcv_ssthresh = min(tp->window_clamp, 2*tp->advmss);
	}
}

/* There is something which you must keep in mind when you analyze the
 * behavior of the tp->ato delayed ack timeout interval.  When a
 * connection starts up, we want to ack as quickly as possible.  The
 * problem is that "good" TCP's do slow start at the beginning of data
 * transmission.  The means that until we send the first few ACK's the
 * sender will sit on his end and only queue most of his data, because
 * he can only send snd_cwnd unacked packets at any given time.  For
 * each ACK we send, he increments snd_cwnd and transmits more of his
 * queue.  -DaveM
 */
static void tcp_event_data_recv(struct sock *sk, struct tcp_opt *tp, struct sk_buff *skb)
{
	u32 now;

	tcp_schedule_ack(tp);

	tcp_measure_rcv_mss(tp, skb);

	now = tcp_time_stamp;

	if (!tp->ack.ato) {
		/* The _first_ data packet received, initialize
		 * delayed ACK engine.
		 */
		tcp_enter_quickack_mode(tp);
	} else {
		int m = now - tp->ack.lrcvtime;

		if (m <= TCP_ATO_MIN/2) {
			/* The fastest case is the first. */
			tp->ack.ato = (tp->ack.ato>>1) + TCP_ATO_MIN/2;
		} else if (m < tp->ack.ato) {
			tp->ack.ato = (tp->ack.ato>>1) + m;
			if (tp->ack.ato > tp->rto)
				tp->ack.ato = tp->rto;
		} else if (m > tp->rto) {
			/* Too long gap. Apparently sender falled to
			 * restart window, so that we send ACKs quickly.
			 */
			tcp_incr_quickack(tp);
			tcp_mem_reclaim(sk);
		}
	}
	tp->ack.lrcvtime = now;

	TCP_ECN_check_ce(tp, skb);

	if (skb->len >= 128)
		tcp_grow_window(sk, tp, skb);
}

/* Called to compute a smoothed rtt estimate. The data fed to this
 * routine either comes from timestamps, or from segments that were
 * known _not_ to have been retransmitted [see Karn/Partridge
 * Proceedings SIGCOMM 87]. The algorithm is from the SIGCOMM 88
 * piece by Van Jacobson.
 * NOTE: the next three routines used to be one big routine.
 * To save cycles in the RFC 1323 implementation it was better to break
 * it up into three procedures. -- erics
 */
static __inline__ void tcp_rtt_estimator(struct tcp_opt *tp, __u32 mrtt)
{
	long m = mrtt; /* RTT */

	/*	The following amusing code comes from Jacobson's
	 *	article in SIGCOMM '88.  Note that rtt and mdev
	 *	are scaled versions of rtt and mean deviation.
	 *	This is designed to be as fast as possible 
	 *	m stands for "measurement".
	 *
	 *	On a 1990 paper the rto value is changed to:
	 *	RTO = rtt + 4 * mdev
	 *
	 * Funny. This algorithm seems to be very broken.
	 * These formulae increase RTO, when it should be decreased, increase
	 * too slowly, when it should be incresed fastly, decrease too fastly
	 * etc. I guess in BSD RTO takes ONE value, so that it is absolutely
	 * does not matter how to _calculate_ it. Seems, it was trap
	 * that VJ failed to avoid. 8)
	 */
	if(m == 0)
		m = 1;
	if (tp->srtt != 0) {
		m -= (tp->srtt >> 3);	/* m is now error in rtt est */
		tp->srtt += m;		/* rtt = 7/8 rtt + 1/8 new */
		if (m < 0) {
			m = -m;		/* m is now abs(error) */
			m -= (tp->mdev >> 2);   /* similar update on mdev */
			/* This is similar to one of Eifel findings.
			 * Eifel blocks mdev updates when rtt decreases.
			 * This solution is a bit different: we use finer gain
			 * for mdev in this case (alpha*beta).
			 * Like Eifel it also prevents growth of rto,
			 * but also it limits too fast rto decreases,
			 * happening in pure Eifel.
			 */
			if (m > 0)
				m >>= 3;
		} else {
			m -= (tp->mdev >> 2);   /* similar update on mdev */
		}
		tp->mdev += m;	    	/* mdev = 3/4 mdev + 1/4 new */
		if (tp->mdev > tp->mdev_max) {
			tp->mdev_max = tp->mdev;
			if (tp->mdev_max > tp->rttvar)
				tp->rttvar = tp->mdev_max;
		}
		if (after(tp->snd_una, tp->rtt_seq)) {
			if (tp->mdev_max < tp->rttvar)
				tp->rttvar -= (tp->rttvar-tp->mdev_max)>>2;
			tp->rtt_seq = tp->snd_una;
			tp->mdev_max = TCP_RTO_MIN;
		}
	} else {
		/* no previous measure. */
		tp->srtt = m<<3;	/* take the measured time to be rtt */
		tp->mdev = m<<2;	/* make sure rto = 3*rtt */
		tp->mdev_max = tp->rttvar = max(tp->mdev, TCP_RTO_MIN);
		tp->rtt_seq = tp->snd_nxt;
	}
}

/* Calculate rto without backoff.  This is the second half of Van Jacobson's
 * routine referred to above.
 */
static __inline__ void tcp_set_rto(struct tcp_opt *tp)
{
	/* Old crap is replaced with new one. 8)
	 *
	 * More seriously:
	 * 1. If rtt variance happened to be less 50msec, it is hallucination.
	 *    It cannot be less due to utterly erratic ACK generation made
	 *    at least by solaris and freebsd. "Erratic ACKs" has _nothing_
	 *    to do with delayed acks, because at cwnd>2 true delack timeout
	 *    is invisible. Actually, Linux-2.4 also generates erratic
	 *    ACKs in some curcumstances.
	 */
	tp->rto = (tp->srtt >> 3) + tp->rttvar;

	/* 2. Fixups made earlier cannot be right.
	 *    If we do not estimate RTO correctly without them,
	 *    all the algo is pure shit and should be replaced
	 *    with correct one. It is exaclty, which we pretend to do.
	 */
}

/* NOTE: clamping at TCP_RTO_MIN is not required, current algo
 * guarantees that rto is higher.
 */
static __inline__ void tcp_bound_rto(struct tcp_opt *tp)
{
	if (tp->rto > TCP_RTO_MAX)
		tp->rto = TCP_RTO_MAX;
}

/* Save metrics learned by this TCP session.
   This function is called only, when TCP finishes sucessfully
   i.e. when it enters TIME-WAIT or goes from LAST-ACK to CLOSE.
 */
void tcp_update_metrics(struct sock *sk)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	struct dst_entry *dst = __sk_dst_get(sk);

	dst_confirm(dst);

	if (dst && (dst->flags&DST_HOST)) {
		int m;

		if (tp->backoff || !tp->srtt) {
			/* This session failed to estimate rtt. Why?
			 * Probably, no packets returned in time.
			 * Reset our results.
			 */
			if (!(dst->mxlock&(1<<RTAX_RTT)))
				dst->rtt = 0;
			return;
		}

		m = dst->rtt - tp->srtt;

		/* If newly calculated rtt larger than stored one,
		 * store new one. Otherwise, use EWMA. Remember,
		 * rtt overestimation is always better than underestimation.
		 */
		if (!(dst->mxlock&(1<<RTAX_RTT))) {
			if (m <= 0)
				dst->rtt = tp->srtt;
			else
				dst->rtt -= (m>>3);
		}

		if (!(dst->mxlock&(1<<RTAX_RTTVAR))) {
			if (m < 0)
				m = -m;

			/* Scale deviation to rttvar fixed point */
			m >>= 1;
			if (m < tp->mdev)
				m = tp->mdev;

			if (m >= dst->rttvar)
				dst->rttvar = m;
			else
				dst->rttvar -= (dst->rttvar - m)>>2;
		}

		if (tp->snd_ssthresh >= 0xFFFF) {
			/* Slow start still did not finish. */
			if (dst->ssthresh &&
			    !(dst->mxlock&(1<<RTAX_SSTHRESH)) &&
			    (tp->snd_cwnd>>1) > dst->ssthresh)
				dst->ssthresh = (tp->snd_cwnd>>1);
			if (!(dst->mxlock&(1<<RTAX_CWND)) &&
			    tp->snd_cwnd > dst->cwnd)
				dst->cwnd = tp->snd_cwnd;
		} else if (tp->snd_cwnd > tp->snd_ssthresh &&
			   tp->ca_state == TCP_CA_Open) {
			/* Cong. avoidance phase, cwnd is reliable. */
			if (!(dst->mxlock&(1<<RTAX_SSTHRESH)))
				dst->ssthresh = max(tp->snd_cwnd>>1, tp->snd_ssthresh);
			if (!(dst->mxlock&(1<<RTAX_CWND)))
				dst->cwnd = (dst->cwnd + tp->snd_cwnd)>>1;
		} else {
			/* Else slow start did not finish, cwnd is non-sense,
			   ssthresh may be also invalid.
			 */
			if (!(dst->mxlock&(1<<RTAX_CWND)))
				dst->cwnd = (dst->cwnd + tp->snd_ssthresh)>>1;
			if (dst->ssthresh &&
			    !(dst->mxlock&(1<<RTAX_SSTHRESH)) &&
			    tp->snd_ssthresh > dst->ssthresh)
				dst->ssthresh = tp->snd_ssthresh;
		}

		if (!(dst->mxlock&(1<<RTAX_REORDERING))) {
			if (dst->reordering < tp->reordering &&
			    tp->reordering != sysctl_tcp_reordering)
				dst->reordering = tp->reordering;
		}
	}
}

/* Increase initial CWND conservatively: if estimated
 * RTT is low enough (<20msec) or if we have some preset ssthresh.
 *
 * Numbers are taken from RFC1414.
 */
__u32 tcp_init_cwnd(struct tcp_opt *tp)
{
	__u32 cwnd;

	if (tp->mss_cache > 1460)
		return 2;

	cwnd = (tp->mss_cache > 1095) ? 3 : 4;

	if (!tp->srtt || (tp->snd_ssthresh >= 0xFFFF && tp->srtt > ((HZ/50)<<3)))
		cwnd = 2;
	else if (cwnd > tp->snd_ssthresh)
		cwnd = tp->snd_ssthresh;

	return min(cwnd, tp->snd_cwnd_clamp);
}

/* Initialize metrics on socket. */

static void tcp_init_metrics(struct sock *sk)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	struct dst_entry *dst = __sk_dst_get(sk);

	if (dst == NULL)
		goto reset;

	dst_confirm(dst);

	if (dst->mxlock&(1<<RTAX_CWND))
		tp->snd_cwnd_clamp = dst->cwnd;
	if (dst->ssthresh) {
		tp->snd_ssthresh = dst->ssthresh;
		if (tp->snd_ssthresh > tp->snd_cwnd_clamp)
			tp->snd_ssthresh = tp->snd_cwnd_clamp;
	}
	if (dst->reordering && tp->reordering != dst->reordering) {
		tp->sack_ok &= ~2;
		tp->reordering = dst->reordering;
	}

	if (dst->rtt == 0)
		goto reset;

	if (!tp->srtt && dst->rtt < (TCP_TIMEOUT_INIT<<3))
		goto reset;

	/* Initial rtt is determined from SYN,SYN-ACK.
	 * The segment is small and rtt may appear much
	 * less than real one. Use per-dst memory
	 * to make it more realistic.
	 *
	 * A bit of theory. RTT is time passed after "normal" sized packet
	 * is sent until it is ACKed. In normal curcumstances sending small
	 * packets force peer to delay ACKs and calculation is correct too.
	 * The algorithm is adaptive and, provided we follow specs, it
	 * NEVER underestimate RTT. BUT! If peer tries to make some clever
	 * tricks sort of "quick acks" for time long enough to decrease RTT
	 * to low value, and then abruptly stops to do it and starts to delay
	 * ACKs, wait for troubles.
	 */
	if (dst->rtt > tp->srtt)
		tp->srtt = dst->rtt;
	if (dst->rttvar > tp->mdev) {
		tp->mdev = dst->rttvar;
		tp->mdev_max = tp->rttvar = max(tp->mdev, TCP_RTO_MIN);
	}
	tcp_set_rto(tp);
	tcp_bound_rto(tp);
	if (tp->rto < TCP_TIMEOUT_INIT && !tp->saw_tstamp)
		goto reset;
	tp->snd_cwnd = tcp_init_cwnd(tp);
	tp->snd_cwnd_stamp = tcp_time_stamp;
	return;

reset:
	/* Play conservative. If timestamps are not
	 * supported, TCP will fail to recalculate correct
	 * rtt, if initial rto is too small. FORGET ALL AND RESET!
	 */
	if (!tp->saw_tstamp && tp->srtt) {
		tp->srtt = 0;
		tp->mdev = tp->mdev_max = tp->rttvar = TCP_TIMEOUT_INIT;
		tp->rto = TCP_TIMEOUT_INIT;
	}
}

static void tcp_update_reordering(struct tcp_opt *tp, int metric, int ts)
{
	if (metric > tp->reordering) {
		tp->reordering = min(TCP_MAX_REORDERING, metric);

		/* This exciting event is worth to be remembered. 8) */
		if (ts)
			NET_INC_STATS_BH(TCPTSReorder);
		else if (IsReno(tp))
			NET_INC_STATS_BH(TCPRenoReorder);
		else if (IsFack(tp))
			NET_INC_STATS_BH(TCPFACKReorder);
		else
			NET_INC_STATS_BH(TCPSACKReorder);
#if FASTRETRANS_DEBUG > 1
		printk(KERN_DEBUG "Disorder%d %d %u f%u s%u rr%d\n",
		       tp->sack_ok, tp->ca_state,
		       tp->reordering, tp->fackets_out, tp->sacked_out,
		       tp->undo_marker ? tp->undo_retrans : 0);
#endif
		/* Disable FACK yet. */
		tp->sack_ok &= ~2;
	}
}

/* This procedure tags the retransmission queue when SACKs arrive.
 *
 * We have three tag bits: SACKED(S), RETRANS(R) and LOST(L).
 * Packets in queue with these bits set are counted in variables
 * sacked_out, retrans_out and lost_out, correspondingly.
 *
 * Valid combinations are:
 * Tag  InFlight	Description
 * 0	1		- orig segment is in flight.
 * S	0		- nothing flies, orig reached receiver.
 * L	0		- nothing flies, orig lost by net.
 * R	2		- both orig and retransmit are in flight.
 * L|R	1		- orig is lost, retransmit is in flight.
 * S|R  1		- orig reached receiver, retrans is still in flight.
 * (L|S|R is logically valid, it could occur when L|R is sacked,
 *  but it is equivalent to plain S and code short-curcuits it to S.
 *  L|S is logically invalid, it would mean -1 packet in flight 8))
 *
 * These 6 states form finite state machine, controlled by the following events:
 * 1. New ACK (+SACK) arrives. (tcp_sacktag_write_queue())
 * 2. Retransmission. (tcp_retransmit_skb(), tcp_xmit_retransmit_queue())
 * 3. Loss detection event of one of three flavors:
 *	A. Scoreboard estimator decided the packet is lost.
 *	   A'. Reno "three dupacks" marks head of queue lost.
 *	   A''. Its FACK modfication, head until snd.fack is lost.
 *	B. SACK arrives sacking data transmitted after never retransmitted
 *	   hole was sent out.
 *	C. SACK arrives sacking SND.NXT at the moment, when the
 *	   segment was retransmitted.
 * 4. D-SACK added new rule: D-SACK changes any tag to S.
 *
 * It is pleasant to note, that state diagram turns out to be commutative,
 * so that we are allowed not to be bothered by order of our actions,
 * when multiple events arrive simultaneously. (see the function below).
 *
 * Reordering detection.
 * --------------------
 * Reordering metric is maximal distance, which a packet can be displaced
 * in packet stream. With SACKs we can estimate it:
 *
 * 1. SACK fills old hole and the corresponding segment was not
 *    ever retransmitted -> reordering. Alas, we cannot use it
 *    when segment was retransmitted.
 * 2. The last flaw is solved with D-SACK. D-SACK arrives
 *    for retransmitted and already SACKed segment -> reordering..
 * Both of these heuristics are not used in Loss state, when we cannot
 * account for retransmits accurately.
 */
static int
tcp_sacktag_write_queue(struct sock *sk, struct sk_buff *ack_skb, u32 prior_snd_una)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	unsigned char *ptr = ack_skb->h.raw + TCP_SKB_CB(ack_skb)->sacked;
	struct tcp_sack_block *sp = (struct tcp_sack_block *)(ptr+2);
	int num_sacks = (ptr[1] - TCPOLEN_SACK_BASE)>>3;
	int reord = tp->packets_out;
	int prior_fackets;
	u32 lost_retrans = 0;
	int flag = 0;
	int i;

	if (!tp->sacked_out)
		tp->fackets_out = 0;
	prior_fackets = tp->fackets_out;

	for (i=0; i<num_sacks; i++, sp++) {
		struct sk_buff *skb;
		__u32 start_seq = ntohl(sp->start_seq);
		__u32 end_seq = ntohl(sp->end_seq);
		int fack_count = 0;
		int dup_sack = 0;

		/* Check for D-SACK. */
		if (i == 0) {
			u32 ack = TCP_SKB_CB(ack_skb)->ack_seq;

			if (before(start_seq, ack)) {
				dup_sack = 1;
				tp->sack_ok |= 4;
				NET_INC_STATS_BH(TCPDSACKRecv);
			} else if (num_sacks > 1 &&
				   !after(end_seq, ntohl(sp[1].end_seq)) &&
				   !before(start_seq, ntohl(sp[1].start_seq))) {
				dup_sack = 1;
				tp->sack_ok |= 4;
				NET_INC_STATS_BH(TCPDSACKOfoRecv);
			}

			/* D-SACK for already forgotten data...
			 * Do dumb counting. */
			if (dup_sack &&
			    !after(end_seq, prior_snd_una) &&
			    after(end_seq, tp->undo_marker))
				tp->undo_retrans--;

			/* Eliminate too old ACKs, but take into
			 * account more or less fresh ones, they can
			 * contain valid SACK info.
			 */
			if (before(ack, prior_snd_una-tp->max_window))
				return 0;
		}

		/* Event "B" in the comment above. */
		if (after(end_seq, tp->high_seq))
			flag |= FLAG_DATA_LOST;

		for_retrans_queue(skb, sk, tp) {
			u8 sacked = TCP_SKB_CB(skb)->sacked;
			int in_sack;

			/* The retransmission queue is always in order, so
			 * we can short-circuit the walk early.
			 */
			if(!before(TCP_SKB_CB(skb)->seq, end_seq))
				break;

			fack_count++;

			in_sack = !after(start_seq, TCP_SKB_CB(skb)->seq) &&
				!before(end_seq, TCP_SKB_CB(skb)->end_seq);

			/* Account D-SACK for retransmitted packet. */
			if ((dup_sack && in_sack) &&
			    (sacked & TCPCB_RETRANS) &&
			    after(TCP_SKB_CB(skb)->end_seq, tp->undo_marker))
				tp->undo_retrans--;

			/* The frame is ACKed. */
			if (!after(TCP_SKB_CB(skb)->end_seq, tp->snd_una)) {
				if (sacked&TCPCB_RETRANS) {
					if ((dup_sack && in_sack) &&
					    (sacked&TCPCB_SACKED_ACKED))
						reord = min(fack_count, reord);
				} else {
					/* If it was in a hole, we detected reordering. */
					if (fack_count < prior_fackets &&
					    !(sacked&TCPCB_SACKED_ACKED))
						reord = min(fack_count, reord);
				}

				/* Nothing to do; acked frame is about to be dropped. */
				continue;
			}

			if ((sacked&TCPCB_SACKED_RETRANS) &&
			    after(end_seq, TCP_SKB_CB(skb)->ack_seq) &&
			    (!lost_retrans || after(end_seq, lost_retrans)))
				lost_retrans = end_seq;

			if (!in_sack)
				continue;

			if (!(sacked&TCPCB_SACKED_ACKED)) {
				if (sacked & TCPCB_SACKED_RETRANS) {
					/* If the segment is not tagged as lost,
					 * we do not clear RETRANS, believing
					 * that retransmission is still in flight.
					 */
					if (sacked & TCPCB_LOST) {
						TCP_SKB_CB(skb)->sacked &= ~(TCPCB_LOST|TCPCB_SACKED_RETRANS);
						tp->lost_out--;
						tp->retrans_out--;
					}
				} else {
					/* New sack for not retransmitted frame,
					 * which was in hole. It is reordering.
					 */
					if (!(sacked & TCPCB_RETRANS) &&
					    fack_count < prior_fackets)
						reord = min(fack_count, reord);

					if (sacked & TCPCB_LOST) {
						TCP_SKB_CB(skb)->sacked &= ~TCPCB_LOST;
						tp->lost_out--;
					}
				}

				TCP_SKB_CB(skb)->sacked |= TCPCB_SACKED_ACKED;
				flag |= FLAG_DATA_SACKED;
				tp->sacked_out++;

				if (fack_count > tp->fackets_out)
					tp->fackets_out = fack_count;
			} else {
				if (dup_sack && (sacked&TCPCB_RETRANS))
					reord = min(fack_count, reord);
			}

			/* D-SACK. We can detect redundant retransmission
			 * in S|R and plain R frames and clear it.
			 * undo_retrans is decreased above, L|R frames
			 * are accounted above as well.
			 */
			if (dup_sack &&
			    (TCP_SKB_CB(skb)->sacked&TCPCB_SACKED_RETRANS)) {
				TCP_SKB_CB(skb)->sacked &= ~TCPCB_SACKED_RETRANS;
				tp->retrans_out--;
			}
		}
	}

	/* Check for lost retransmit. This superb idea is
	 * borrowed from "ratehalving". Event "C".
	 * Later note: FACK people cheated me again 8),
	 * we have to account for reordering! Ugly,
	 * but should help.
	 */
	if (lost_retrans && tp->ca_state == TCP_CA_Recovery) {
		struct sk_buff *skb;

		for_retrans_queue(skb, sk, tp) {
			if (after(TCP_SKB_CB(skb)->seq, lost_retrans))
				break;
			if (!after(TCP_SKB_CB(skb)->end_seq, tp->snd_una))
				continue;
			if ((TCP_SKB_CB(skb)->sacked&TCPCB_SACKED_RETRANS) &&
			    after(lost_retrans, TCP_SKB_CB(skb)->ack_seq) &&
			    (IsFack(tp) ||
			     !before(lost_retrans, TCP_SKB_CB(skb)->ack_seq+tp->reordering*tp->mss_cache))) {
				TCP_SKB_CB(skb)->sacked &= ~TCPCB_SACKED_RETRANS;
				tp->retrans_out--;

				if (!(TCP_SKB_CB(skb)->sacked&(TCPCB_LOST|TCPCB_SACKED_ACKED))) {
					tp->lost_out++;
					TCP_SKB_CB(skb)->sacked |= TCPCB_LOST;
					flag |= FLAG_DATA_SACKED;
					NET_INC_STATS_BH(TCPLostRetransmit);
				}
			}
		}
	}

	tp->left_out = tp->sacked_out + tp->lost_out;

	if (reord < tp->fackets_out && tp->ca_state != TCP_CA_Loss)
		tcp_update_reordering(tp, (tp->fackets_out+1)-reord, 0);

#if FASTRETRANS_DEBUG > 0
	BUG_TRAP((int)tp->sacked_out >= 0);
	BUG_TRAP((int)tp->lost_out >= 0);
	BUG_TRAP((int)tp->retrans_out >= 0);
	BUG_TRAP((int)tcp_packets_in_flight(tp) >= 0);
#endif
	return flag;
}

void tcp_clear_retrans(struct tcp_opt *tp)
{
	tp->left_out = 0;
	tp->retrans_out = 0;

	tp->fackets_out = 0;
	tp->sacked_out = 0;
	tp->lost_out = 0;

	tp->undo_marker = 0;
	tp->undo_retrans = 0;
}

/* Enter Loss state. If "how" is not zero, forget all SACK information
 * and reset tags completely, otherwise preserve SACKs. If receiver
 * dropped its ofo queue, we will know this due to reneging detection.
 */
void tcp_enter_loss(struct sock *sk, int how)
{
	struct tcp_opt *tp = &sk->tp_pinfo.af_tcp;
	struct sk_buff *skb;
	int cnt = 0;

	/* Reduce ssthresh if it has not yet been made inside this window. */
	if (tp->ca_state <= TCP_CA_Disorder ||
	    tp->snd_una == tp->high_seq ||
	    (tp->ca_state == TCP_CA_Loss && !tp->retransmits)) {
		tp->prior_ssthresh = tcp_current_ssthresh(tp);
		tp->snd_ssthresh = tcp_recalc_ssthresh(tp);
	}
	tp->snd_cwnd = 1;
	tp->snd_cwnd_cnt = 0;
	tp->snd_cwnd_stamp = tcp_time_stamp;

	tcp_clear_retrans(tp);

	/* Push undo marker, if it was plain RTO and nothing
	 * was retransmitted. */
	if (!how)
		tp->undo_marker = tp->snd_una;

	for_retrans_queue(skb, sk, tp) {
		cnt++;
		if (TCP_SKB_CB(skb)->sacked&TCPCB_RETRANS)
			tp->undo_marker = 0;
		TCP_SKB_CB(skb)->sacked &= (~TCPCB_TAGBITS)|TCPCB_SACKED_ACKED;
		if (!(TCP_SKB_CB(skb)->sacked&TCPCB_SACKED_ACKED) || how) {
			TCP_SKB_CB(skb)->sacked &= ~TCPCB_SACKED_ACKED;
			TCP_SKB_CB(skb)->sacked |= TCPCB_LOST;
			tp->lost_out++;
		} else {
			tp->sacked_out++;
			tp->fackets_out = cnt;
		}
	}
	tp->left_out = tp->sacked_out + tp->lost_out;

	tp->reordering = min(tp->reordering, sysctl_tcp_reordering);
	tp->ca_state = TCP_CA_Loss;
	tp->high_seq = tp->snd_nxt;
	TCP_ECN_queue_cwr(tp);
}

static int tcp_check_sack_reneging(struct sock *sk, struct tcp_opt *tp)
{
	struct sk_buff *skb;

	/* If ACK arrived pointing to a remembered SACK,
	 * it means that our remembered SACKs do not reflect
	 * real state of receiver i.e.
	 * receiver _host_ is heavily congested (or buggy).
	 * Do processing similar to RTO timeout.
	 */
	if ((skb = skb_peek(&sk->write_queue)) != NULL &&
	    (TCP_SKB_CB(skb)->sacked & TCPCB_SACKED_ACKED)) {
		NET_INC_STATS_BH(TCPSACKReneging);

		tcp_enter_loss(sk, 1);
		tp->retransmits++;
		tcp_retransmit_skb(sk, skb_peek(&sk->write_queue));
		tcp_reset_xmit_timer(sk, TCP_TIME_RETRANS, tp->rto);
		return 1;
	}
	return 0;
}

static inline int tcp_fackets_out(struct tcp_opt *tp)
{
	return IsReno(tp) ? tp->sacked_out+1 : tp->fackets_out;
}


/* Linux NewReno/SACK/FACK/ECN state machine.
 * --------------------------------------
 *
 * "Open"	Normal state, no dubious events, fast path.
 * "Disorder"   In all the respects it is "Open",
 *		but requires a bit more attention. It is entered when
 *		we see some SACKs or dupacks. It is split of "Open"
 *		mainly to move some processing from fast path to slow one.
 * "CWR"	CWND was reduced due to some Congestion Notification event.
 *		It can be ECN, ICMP source quench, local device congestion.
 * "Recovery"	CWND was reduced, we are fast-retransmitting.
 * "Loss"	CWND was reduced due to RTO timeout or SACK reneging.
 *
 * tcp_fastretrans_alert() is entered:
 * - each incoming ACK, if state is not "Open"
 * - when arrived ACK is unusual, namely:
 *	* SACK
 *	* Duplicate ACK.
 *	* ECN ECE.
 *
 * Counting packets in flight is pretty simple.
 *
 *	in_flight = packets_out - left_out + retrans_out
 *
 *	packets_out is SND.NXT-SND.UNA counted in packets.
 *
 *	retrans_out is number of retransmitted segments.
 *
 *	left_out is number of segments left network, but not ACKed yet.
 *
 *		left_out = sacked_out + lost_out
 *
 *     sacked_out: Packets, which arrived to receiver out of order
 *		   and hence not ACKed. With SACKs this number is simply
 *		   amount of SACKed data. Even without SACKs
 *		   it is easy to give pretty reliable estimate of this number,
 *		   counting duplicate ACKs.
 *
 *       lost_out: Packets lost by network. TCP has no explicit
 *		   "loss notification" feedback from network (for now).
 *		   It means that this number can be only _guessed_.
 *		   Actually, it is the heuristics to predict lossage that
 *		   distinguishes different algorithms.
 *
 *	F.e. after RTO, when all the queue is considered as lost,
 *	lost_out = packets_out and in_flight = retrans_out.
 *
 *		Essentially, we have now two algorithms counting
 *		lost packets.
 *
 *		FACK: It is the simplest heuristics. As soon as we decided
 *		that something is lost, we decide that _all_ not SACKed
 *		packets until the most forward SACK are lost. I.e.
 *		lost_out = fackets_out - sacked_out and left_out = fackets_out.
 *		It is absolutely correct estimate, if network does not reorder
 *		packets. And it loses any connection to reality when reordering
 *		takes place. We use FACK by default until reordering
 *		is suspected on the path to this destination.
 *
 *		NewReno: when Recovery is entered, we assume that one segment
 *		is lost (classic Reno). While we are in Recovery and
 *		a partial ACK arrives, we assume that one more packet
 *		is lost (NewReno). This heuristics are the same in NewReno
 *		and SACK.
 *
 *  Imagine, that's all! Forget about all this shamanism about CWND inflation
 *  deflation etc. CWND is real congestion window, never inflated, changes
 *  only according to classic VJ rules.
 *
 * Really tricky (and requiring careful tuning) part of algorithm
 * is hidden in functions tcp_time_to_recover() and tcp_xmit_retransmit_queue().
 * The first determines the moment _when_ we should reduce CWND and,
 * hence, slow down forward transmission. In fact, it determines the moment
 * when we decide that hole is caused by loss, rather than by a reorder.
 *
 * tcp_xmit_retransmit_queue() decides, _what_ we should retransmit to fill
 * holes, caused by lost packets.
 *
 * And the most logically complicated part of algorithm is undo
 * heuristics. We detect false retransmits due to both too early
 * fast retransmit (reordering) and underestimated RTO, analyzing
 * timestamps and D-SACKs. When we detect that some segments were
 * retransmitted by mistake and CWND reduction was wrong, we undo
 * window reduction and abort recovery phase. This logic is hidden
 * inside several functions named tcp_try_undo_<something>.
 */

/* This function decides, when we should leave Disordered state
 * and enter Recovery phase, reducing congestion window.
 *
 * Main question: may we further continue forward transmission
 * with the same cwnd?
 */
static int
tcp_time_to_recover(struct sock *sk, struct tcp_opt *tp)
{
	/* Trick#1: The loss is proven. */
	if (tp->lost_out)
		return 1;

	/* Not-A-Trick#2 : Classic rule... */
	if (tcp_fackets_out(tp) > tp->reordering)
		return 1;

	/* Trick#3: It is still not OK... But will it be useful to delay
	 * recovery more?
	 */
	if (tp->packets_out <= tp->reordering &&
	    tp->sacked_out >= max(tp->packets_out/2, sysctl_tcp_reordering) &&
	    !tcp_may_send_now(sk, tp)) {
		/* We have nothing to send. This connection is limited
		 * either by receiver window or by application.
		 */
		return 1;
	}

	return 0;
}

/* If we receive more dupacks than we expected counting segments
 * in assumption of absent reordering, interpret this as reordering.
 * The only another reason could be bug in receiver TCP.
 */
static void tcp_check_reno_reordering(struct tcp_opt *tp, int addend)
{
	if (tp->sacked_out + 1 > tp->packets_out) {
		tp->sacked_out = tp->packets_out ? tp->packets_out - 1 : 0;
		tcp_update_reordering(tp, tp->packets_out+addend, 0);
	}
}

/* Emulate SACKs for SACKless connection: account for a new dupack. */

static void tcp_add_reno_sack(struct tcp_opt *tp)
{
	++tp->sacked_out;
	tcp_check_reno_reordering(tp, 0);
	tp->left_out = tp->sacked_out + tp->lost_out;
}

/* Account for ACK, ACKing some data in Reno Recovery phase. */

static void tcp_remove_reno_sacks(struct sock *sk, struct tcp_opt *tp, int acked)
{
	if (acked > 0) {
		/* One ACK eated lost packet. Must eat! */
		BUG_TRAP(tp->lost_out == 0);

		/* The rest eat duplicate ACKs. */
		if (acked-1 >= tp->sacked_out)
			tp->sacked_out = 0;
		else
			tp->sacked_out -= acked-1;
	}
	tcp_check_reno_reordering(tp, acked);
	tp->left_out = tp->sacked_out + tp->lost_out;
}

static inline void tcp_reset_reno_sack(struct tcp_opt *tp)
{
	tp->sacked_out = 0;
	tp->left_out = tp->lost_out;
}

/* Mark head of queue up as lost. */
static void
tcp_mark_head_lost(struct sock *sk, struct tcp_opt *tp, int packets, u32 high_seq)
{
	struct sk_buff *skb;
	int cnt = packets;

	BUG_TRAP(cnt <= tp->packets_out);

	for_retrans_queue(skb, sk, tp) {
		if (--cnt < 0 || after(TCP_SKB_CB(skb)->end_seq, high_seq))
			break;
		if (!(TCP_SKB_CB(skb)->sacked&TCPCB_TAGBITS)) {
			TCP_SKB_CB(skb)->sacked |= TCPCB_LOST;
			tp->lost_out++;
		}
	}
	tp->left_out = tp->sacked_out + tp->lost_out;
}

/* Account newly detected lost packet(s) */

static void tcp_update_scoreboard(struct sock *sk, struct tcp_opt *tp)
{
	if (IsFack(tp)) {
		int lost = tp->fackets_out - tp->reordering;
		if (lost <= 0)
			lost = 1;
		tcp_mark_head_lost(sk, tp, lost, tp->high_seq);
	} else {
		tcp_mark_head_lost(sk, tp, 1, tp->high_seq);
	}
}

/* CWND moderation, preventing bursts due to too big ACKs
 * in dubious situations.
 */
static __inline__ void tcp_moderate_cwnd(struct tcp_opt *tp)
{
	tp->snd_cwnd = min(tp->snd_cwnd,
			   tcp_packets_in_flight(tp)+tcp_max_burst(tp));
	tp->snd_cwnd_stamp = tcp_time_stamp;
}

/* Decrease cwnd each second ack. */

static void tcp_cwnd_down(struct tcp_opt *tp)
{
	int decr = tp->snd_cwnd_cnt + 1;

	tp->snd_cwnd_cnt = decr&1;
	decr >>= 1;

	if (decr && tp->snd_cwnd > tp->snd_ssthresh/2)
		tp->snd_cwnd -= decr;

	tp->snd_cwnd = min(tp->snd_cwnd, tcp_packets_in_flight(tp)+1);
	tp->snd_cwnd_stamp = tcp_time_stamp;
}

/* Nothing was retransmitted or returned timestamp is less
 * than timestamp of the first retransmission.
 */
static __inline__ int tcp_packet_delayed(struct tcp_opt *tp)
{
	return !tp->retrans_stamp ||
		(tp->saw_tstamp && tp->rcv_tsecr &&
		 (__s32)(tp->rcv_tsecr - tp->retrans_stamp) < 0);
}

/* Undo procedures. */

#if FASTRETRANS_DEBUG > 1
static void DBGUNDO(struct sock *sk, struct tcp_opt *tp, const char *msg)
{
	printk(KERN_DEBUG "Undo %s %u.%u.%u.%u/%u c%u l%u ss%u/%u p%u\n",
	       msg,
	       NIPQUAD(sk->daddr), ntohs(sk->dport),
	       tp->snd_cwnd, tp->left_out,
	       tp->snd_ssthresh, tp->prior_ssthresh, tp->packets_out);
}
#else
#define DBGUNDO(x...) do { } while (0)
#endif

static void tcp_undo_cwr(struct tcp_opt *tp, int undo)
{
	if (tp->prior_ssthresh) {
		tp->snd_cwnd = max(tp->snd_cwnd, tp->snd_ssthresh<<1);
		if (undo && tp->prior_ssthresh > tp->snd_ssthresh) {
			tp->snd_ssthresh = tp->prior_ssthresh;
			TCP_ECN_withdraw_cwr(tp);
		}
	} else {
		tp->snd_cwnd = max(tp->snd_cwnd, tp->snd_ssthresh);
	}
	tcp_moderate_cwnd(tp);
	tp->snd_cwnd_stamp = tcp_time_stamp;
}

static inline int tcp_may_undo(struct tcp_opt *tp)
{
	return tp->undo_marker &&
		(!tp->undo_retrans || tcp_packet_delayed(tp));
}

/* People celebrate: "We love our President!" */
static int tcp_try_undo_recovery(struct sock *sk, struct tcp_opt *tp)
{
	if (tcp_may_undo(tp)) {
		/* Happy end! We did not retransmit anything
		 * or our original transmission succeeded.
		 */
		DBGUNDO(sk, tp, tp->ca_state == TCP_CA_Loss ? "loss" : "retrans");
		tcp_undo_cwr(tp, 1);
		if (tp->ca_state == TCP_CA_Loss)
			NET_INC_STATS_BH(TCPLossUndo);
		else
			NET_INC_STATS_BH(TCPFullUndo);
		tp->undo_marker = 0;
	}
	if (tp->snd_una == tp->high_seq && IsReno(tp)) {
		/* Hold old state until something *above* high_seq
		 * is ACKed. For Reno it is MUST to prevent false
		 * fast retransmits (RFC2582). SACK TCP is safe. */
		tcp_moderate_cwnd(tp);
		return 1;
	}
	tp->ca_state = TCP_CA_Open;
	return 0;
}

/* Try to undo cwnd reduction, because D-SACKs acked all retransmitted data */
static void tcp_try_undo_dsack(struct sock *sk, struct tcp_opt *tp)
{
	if (tp->undo_marker && !tp->undo_retrans) {
		DBGUNDO(sk, tp, "D-SACK");
		tcp_undo_cwr(tp, 1);
		tp->undo_marker = 0;
		NET_INC_STATS_BH(TCPDSACKUndo);
	}
}

/* Undo during fast recovery after partial ACK. */

static int tcp_try_undo_partial(struct sock *sk, struct tcp_opt *tp, int acked)
{
	/* Partial ACK arrived. Force Hoe's retransmit. */
	int failed = IsReno(tp) || tp->fackets_out>tp->reordering;

	if (tcp_may_undo(tp)) {
		/* Plain luck! Hole if filled with delayed
		 * packet, rather than with a retransmit.
		 */
		if (tp->retrans_out == 0)
			tp->retrans_stamp = 0;

		tcp_update_reordering(tp, tcp_fackets_out(tp)+acked, 1);

		DBGUNDO(sk, tp, "Hoe");
		tcp_undo_cwr(tp, 0);
		NET_INC_STATS_BH(TCPPartialUndo);

		/* So... Do not make Hoe's retransmit yet.
		 * If the first packet was delayed, the rest
		 * ones are most probably delayed as well.
		 */
		failed = 0;
	}
	return failed;
}

/* Undo during loss recovery after partial ACK. */
static int tcp_try_undo_loss(struct sock *sk, struct tcp_opt *tp)
{
	if (tcp_may_undo(tp)) {
		struct sk_buff *skb;
		for_retrans_queue(skb, sk, tp) {
			TCP_SKB_CB(skb)->sacked &= ~TCPCB_LOST;
		}
		DBGUNDO(sk, tp, "partial loss");
		tp->lost_out = 0;
		tp->left_out = tp->sacked_out;
		tcp_undo_cwr(tp, 1);
		NET_INC_STATS_BH(TCPLossUndo);
		tp->retransmits = 0;
		tp->undo_marker = 0;
		if (!IsReno(tp))
			tp->ca_state = TCP_CA_Open;
		return 1;
	}
	return 0;
}

static __inline__ void tcp_complete_cwr(struct tcp_opt *tp)
{
	tp->snd_cwnd = min(tp->snd_cwnd, tp->snd_ssthresh);
	tp->snd_cwnd_stamp = tcp_time_stamp;
}

static void tcp_try_to_open(struct sock *sk, struct tcp_opt *tp, int flag)
{
	tp->left_out = tp->sacked_out;

	if (tp->retrans_out == 0)
		tp->retrans_stamp = 0;

	if (flag&FLAG_ECE)
		tcp_enter_cwr(tp);

	if (tp->ca_state != TCP_CA_CWR) {
		int state = TCP_CA_Open;

		if (tp->left_out ||
		    tp->retrans_out ||
		    tp->undo_marker)
			state = TCP_CA_Disorder;

		if (tp->ca_state != state) {
			tp->ca_state = state;
			tp->high_seq = tp->snd_nxt;
		}
		tcp_moderate_cwnd(tp);
	} else {
		tcp_cwnd_down(tp);
	}
}

/* Process an event, which can update packets-in-flight not trivially.
 * Main goal of this function is to calculate new estimate for left_out,
 * taking into account both packets sitting in receiver's buffer and
 * packets lost by network.
 *
 * Besides that it does CWND reduction, when packet loss is detected
 * and changes state of machine.
 *
 * It does _not_ decide what to send, it is made in function
 * tcp_xmit_retransmit_queue().
 */
static void
tcp_fastretrans_alert(struct sock *sk, u32 prior_snd_una,
		      int prior_packets, int flag)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	int is_dupack = (tp->snd_una == prior_snd_una && !(flag&FLAG_NOT_DUP));

	/* Some technical things:
	 * 1. Reno does not count dupacks (sacked_out) automatically. */
	if (!tp->packets_out)
		tp->sacked_out = 0;
        /* 2. SACK counts snd_fack in packets inaccurately. */
	if (tp->sacked_out == 0)
		tp->fackets_out = 0;

        /* Now state machine starts.
	 * A. ECE, hence prohibit cwnd undoing, the reduction is required. */
	if (flag&FLAG_ECE)
		tp->prior_ssthresh = 0;

	/* B. In all the states check for reneging SACKs. */
	if (tp->sacked_out && tcp_check_sack_reneging(sk, tp))
		return;

	/* C. Process data loss notification, provided it is valid. */
	if ((flag&FLAG_DATA_LOST) &&
	    before(tp->snd_una, tp->high_seq) &&
	    tp->ca_state != TCP_CA_Open &&
	    tp->fackets_out > tp->reordering) {
		tcp_mark_head_lost(sk, tp, tp->fackets_out-tp->reordering, tp->high_seq);
		NET_INC_STATS_BH(TCPLoss);
	}

	/* D. Synchronize left_out to current state. */
	tp->left_out = tp->sacked_out + tp->lost_out;

	/* E. Check state exit conditions. State can be terminated
	 *    when high_seq is ACKed. */
	if (tp->ca_state == TCP_CA_Open) {
		BUG_TRAP(tp->retrans_out == 0);
		tp->retrans_stamp = 0;
	} else if (!before(tp->snd_una, tp->high_seq)) {
		switch (tp->ca_state) {
		case TCP_CA_Loss:
			tp->retransmits = 0;
			if (tcp_try_undo_recovery(sk, tp))
				return;
			break;

		case TCP_CA_CWR:
			/* CWR is to be held something *above* high_seq
			 * is ACKed for CWR bit to reach receiver. */
			if (tp->snd_una != tp->high_seq) {
				tcp_complete_cwr(tp);
				tp->ca_state = TCP_CA_Open;
			}
			break;

		case TCP_CA_Disorder:
			tcp_try_undo_dsack(sk, tp);
			tp->undo_marker = 0;
			tp->ca_state = TCP_CA_Open;
			break;

		case TCP_CA_Recovery:
			if (IsReno(tp))
				tcp_reset_reno_sack(tp);
			if (tcp_try_undo_recovery(sk, tp))
				return;
			tcp_complete_cwr(tp);
			break;
		}
	}

	/* F. Process state. */
	switch (tp->ca_state) {
	case TCP_CA_Recovery:
		if (prior_snd_una == tp->snd_una) {
			if (IsReno(tp) && is_dupack)
				tcp_add_reno_sack(tp);
		} else {
			int acked = prior_packets - tp->packets_out;
			if (IsReno(tp))
				tcp_remove_reno_sacks(sk, tp, acked);
			is_dupack = tcp_try_undo_partial(sk, tp, acked);
		}
		break;
	case TCP_CA_Loss:
		if (flag & FLAG_ACKED)
			tcp_reset_xmit_timer(sk, TCP_TIME_RETRANS, tp->rto);
		if (!tcp_try_undo_loss(sk, tp)) {
			tcp_moderate_cwnd(tp);
			tcp_xmit_retransmit_queue(sk);
			return;
		}
		if (tp->ca_state != TCP_CA_Open)
			return;
		/* Loss is undone; fall through to processing in Open state. */
	default:
		if (IsReno(tp)) {
			if (tp->snd_una != prior_snd_una)
				tcp_reset_reno_sack(tp);
			if (is_dupack)
				tcp_add_reno_sack(tp);
		}

		if (tp->ca_state == TCP_CA_Disorder)
			tcp_try_undo_dsack(sk, tp);

		if (!tcp_time_to_recover(sk, tp)) {
			tcp_try_to_open(sk, tp, flag);
			return;
		}

		/* Otherwise enter Recovery state */

		if (IsReno(tp))
			NET_INC_STATS_BH(TCPRenoRecovery);
		else
			NET_INC_STATS_BH(TCPSackRecovery);

		tp->high_seq = tp->snd_nxt;
		tp->prior_ssthresh = 0;
		tp->undo_marker = tp->snd_una;
		tp->undo_retrans = tp->retrans_out;

		if (tp->ca_state < TCP_CA_CWR) {
			if (!(flag&FLAG_ECE))
				tp->prior_ssthresh = tcp_current_ssthresh(tp);
			tp->snd_ssthresh = tcp_recalc_ssthresh(tp);
			TCP_ECN_queue_cwr(tp);
		}

		tp->snd_cwnd_cnt = 0;
		tp->ca_state = TCP_CA_Recovery;
	}

	if (is_dupack)
		tcp_update_scoreboard(sk, tp);
	tcp_cwnd_down(tp);
	tcp_xmit_retransmit_queue(sk);
}

/* Read draft-ietf-tcplw-high-performance before mucking
 * with this code. (Superceeds RFC1323)
 */
static void tcp_ack_saw_tstamp(struct tcp_opt *tp, int flag)
{
	__u32 seq_rtt;

	/* RTTM Rule: A TSecr value received in a segment is used to
	 * update the averaged RTT measurement only if the segment
	 * acknowledges some new data, i.e., only if it advances the
	 * left edge of the send window.
	 *
	 * See draft-ietf-tcplw-high-performance-00, section 3.3.
	 * 1998/04/10 Andrey V. Savochkin <saw@msu.ru>
	 */
	seq_rtt = tcp_time_stamp - tp->rcv_tsecr;
	tcp_rtt_estimator(tp, seq_rtt);
	tcp_set_rto(tp);
	if (tp->backoff) {
		if (!tp->retransmits || !(flag & FLAG_RETRANS_DATA_ACKED))
			tp->backoff = 0;
		else
			tp->rto <<= tp->backoff;
	}
	tcp_bound_rto(tp);
}

static void tcp_ack_no_tstamp(struct tcp_opt *tp, u32 seq_rtt, int flag)
{
	/* We don't have a timestamp. Can only use
	 * packets that are not retransmitted to determine
	 * rtt estimates. Also, we must not reset the
	 * backoff for rto until we get a non-retransmitted
	 * packet. This allows us to deal with a situation
	 * where the network delay has increased suddenly.
	 * I.e. Karn's algorithm. (SIGCOMM '87, p5.)
	 */

	if (flag & FLAG_RETRANS_DATA_ACKED)
		return;

	tcp_rtt_estimator(tp, seq_rtt);
	tcp_set_rto(tp);
	if (tp->backoff) {
		/* To relax it? We have valid sample as soon as we are
		 * here. Why not to clear backoff?
		 */
		if (!tp->retransmits)
			tp->backoff = 0;
		else
			tp->rto <<= tp->backoff;
	}
	tcp_bound_rto(tp);
}

static __inline__ void
tcp_ack_update_rtt(struct tcp_opt *tp, int flag, s32 seq_rtt)
{
	/* Note that peer MAY send zero echo. In this case it is ignored. (rfc1323) */
	if (tp->saw_tstamp && tp->rcv_tsecr)
		tcp_ack_saw_tstamp(tp, flag);
	else if (seq_rtt >= 0)
		tcp_ack_no_tstamp(tp, seq_rtt, flag);
}

/* This is Jacobson's slow start and congestion avoidance. 
 * SIGCOMM '88, p. 328.
 */
static __inline__ void tcp_cong_avoid(struct tcp_opt *tp)
{
        if (tp->snd_cwnd <= tp->snd_ssthresh) {
                /* In "safe" area, increase. */
		if (tp->snd_cwnd < tp->snd_cwnd_clamp)
			tp->snd_cwnd++;
	} else {
                /* In dangerous area, increase slowly.
		 * In theory this is tp->snd_cwnd += 1 / tp->snd_cwnd
		 */
		if (tp->snd_cwnd_cnt >= tp->snd_cwnd) {
			if (tp->snd_cwnd < tp->snd_cwnd_clamp)
				tp->snd_cwnd++;
			tp->snd_cwnd_cnt=0;
		} else
			tp->snd_cwnd_cnt++;
        }
}

/* Restart timer after forward progress on connection.
 * RFC2988 recommends (and BSD does) to restart timer to now+rto,
 * which is certainly wrong and effectively means that
 * rto includes one more _full_ rtt.
 *
 * For details see:
 * 	ftp://ftp.inr.ac.ru:/ip-routing/README.rto
 */

static __inline__ void tcp_ack_packets_out(struct sock *sk, struct tcp_opt *tp)
{
	if (tp->packets_out==0) {
		tcp_clear_xmit_timer(sk, TCP_TIME_RETRANS);
	} else {
		struct sk_buff *skb = skb_peek(&sk->write_queue);
		__u32 when = tp->rto + tp->rttvar - (tcp_time_stamp - TCP_SKB_CB(skb)->when);

		if ((__s32)when < (__s32)tp->rttvar)
			when = tp->rttvar;
		tcp_reset_xmit_timer(sk, TCP_TIME_RETRANS, when);
	}
}

/* Remove acknowledged frames from the retransmission queue. */
static int tcp_clean_rtx_queue(struct sock *sk)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	struct sk_buff *skb;
	__u32 now = tcp_time_stamp;
	int acked = 0;
	__s32 seq_rtt = -1;

	while((skb=skb_peek(&sk->write_queue)) && (skb != tp->send_head)) {
		struct tcp_skb_cb *scb = TCP_SKB_CB(skb); 
		__u8 sacked = scb->sacked;

		/* If our packet is before the ack sequence we can
		 * discard it as it's confirmed to have arrived at
		 * the other end.
		 */
		if (after(scb->end_seq, tp->snd_una))
			break;

		/* Initial outgoing SYN's get put onto the write_queue
		 * just like anything else we transmit.  It is not
		 * true data, and if we misinform our callers that
		 * this ACK acks real data, we will erroneously exit
		 * connection startup slow start one packet too
		 * quickly.  This is severely frowned upon behavior.
		 */
		if(!(scb->flags & TCPCB_FLAG_SYN)) {
			acked |= FLAG_DATA_ACKED;
		} else {
			acked |= FLAG_SYN_ACKED;
		}

		if (sacked) {
			if(sacked & TCPCB_RETRANS) {
				if(sacked & TCPCB_SACKED_RETRANS)
					tp->retrans_out--;
				acked |= FLAG_RETRANS_DATA_ACKED;
				seq_rtt = -1;
			} else if (seq_rtt < 0)
				seq_rtt = now - scb->when;
			if(sacked & TCPCB_SACKED_ACKED)
				tp->sacked_out--;
			if(sacked & TCPCB_LOST)
				tp->lost_out--;
			if(sacked & TCPCB_URG) {
				if (tp->urg_mode &&
				    !before(scb->end_seq, tp->snd_up))
					tp->urg_mode = 0;
			}
		} else if (seq_rtt < 0)
			seq_rtt = now - scb->when;
		if(tp->fackets_out)
			tp->fackets_out--;
		tp->packets_out--;
		__skb_unlink(skb, skb->list);
		tcp_free_skb(sk, skb);
	}

	if (acked&FLAG_ACKED) {
		tcp_ack_update_rtt(tp, acked, seq_rtt);
		tcp_ack_packets_out(sk, tp);
	}

#if FASTRETRANS_DEBUG > 0
	BUG_TRAP((int)tp->sacked_out >= 0);
	BUG_TRAP((int)tp->lost_out >= 0);
	BUG_TRAP((int)tp->retrans_out >= 0);
	if (tp->packets_out==0 && tp->sack_ok) {
		if (tp->lost_out) {
			printk(KERN_DEBUG "Leak l=%u %d\n", tp->lost_out, tp->ca_state);
			tp->lost_out = 0;
		}
		if (tp->sacked_out) {
			printk(KERN_DEBUG "Leak s=%u %d\n", tp->sacked_out, tp->ca_state);
			tp->sacked_out = 0;
		}
		if (tp->retrans_out) {
			printk(KERN_DEBUG "Leak r=%u %d\n", tp->retrans_out, tp->ca_state);
			tp->retrans_out = 0;
		}
	}
#endif
	return acked;
}

static void tcp_ack_probe(struct sock *sk)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);

	/* Was it a usable window open? */

	if (!after(TCP_SKB_CB(tp->send_head)->end_seq, tp->snd_una + tp->snd_wnd)) {
		tp->backoff = 0;
		tcp_clear_xmit_timer(sk, TCP_TIME_PROBE0);
		/* Socket must be waked up by subsequent tcp_data_snd_check().
		 * This function is not for random using!
		 */
	} else {
		tcp_reset_xmit_timer(sk, TCP_TIME_PROBE0,
				     min(tp->rto << tp->backoff, TCP_RTO_MAX));
	}
}

static __inline__ int tcp_ack_is_dubious(struct tcp_opt *tp, int flag)
{
	return (!(flag & FLAG_NOT_DUP) || (flag & FLAG_CA_ALERT) ||
		tp->ca_state != TCP_CA_Open);
}

static __inline__ int tcp_may_raise_cwnd(struct tcp_opt *tp, int flag)
{
	return (!(flag & FLAG_ECE) || tp->snd_cwnd < tp->snd_ssthresh) &&
		!((1<<tp->ca_state)&(TCPF_CA_Recovery|TCPF_CA_CWR));
}

/* Check that window update is acceptable.
 * The function assumes that snd_una<=ack<=snd_next.
 */
static __inline__ int
tcp_may_update_window(struct tcp_opt *tp, u32 ack, u32 ack_seq, u32 nwin)
{
	return (after(ack, tp->snd_una) ||
		after(ack_seq, tp->snd_wl1) ||
		(ack_seq == tp->snd_wl1 && nwin > tp->snd_wnd));
}

/* Update our send window.
 *
 * Window update algorithm, described in RFC793/RFC1122 (used in linux-2.2
 * and in FreeBSD. NetBSD's one is even worse.) is wrong.
 */
static int tcp_ack_update_window(struct sock *sk, struct tcp_opt *tp,
				 struct sk_buff *skb, u32 ack, u32 ack_seq)
{
	int flag = 0;
	u32 nwin = ntohs(skb->h.th->window) << tp->snd_wscale;

	if (tcp_may_update_window(tp, ack, ack_seq, nwin)) {
		flag |= FLAG_WIN_UPDATE;
		tcp_update_wl(tp, ack, ack_seq);

		if (tp->snd_wnd != nwin) {
			tp->snd_wnd = nwin;

			/* Note, it is the only place, where
			 * fast path is recovered for sending TCP.
			 */
			if (skb_queue_len(&tp->out_of_order_queue) == 0 &&
#ifdef TCP_FORMAL_WINDOW
			    tcp_receive_window(tp) &&
#endif
			    !tp->urg_data)
				tcp_fast_path_on(tp);

			if (nwin > tp->max_window) {
				tp->max_window = nwin;
				tcp_sync_mss(sk, tp->pmtu_cookie);
			}
		}
	}

	tp->snd_una = ack;

#ifdef TCP_DEBUG
	if (before(tp->snd_una + tp->snd_wnd, tp->snd_nxt)) {
		if (tp->snd_nxt-(tp->snd_una + tp->snd_wnd) >= (1<<tp->snd_wscale)
		    && net_ratelimit())
			printk(KERN_DEBUG "TCP: peer %u.%u.%u.%u:%u/%u shrinks window %u:%u:%u. Bad, what else can I say?\n",
			       NIPQUAD(sk->daddr), htons(sk->dport), sk->num,
			       tp->snd_una, tp->snd_wnd, tp->snd_nxt);
	}
#endif

	return flag;
}

/* This routine deals with incoming acks, but not outgoing ones. */
static int tcp_ack(struct sock *sk, struct sk_buff *skb, int flag)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	u32 prior_snd_una = tp->snd_una;
	u32 ack_seq = TCP_SKB_CB(skb)->seq;
	u32 ack = TCP_SKB_CB(skb)->ack_seq;
	u32 prior_in_flight;
	int prior_packets;

	/* If the ack is newer than sent or older than previous acks
	 * then we can probably ignore it.
	 */
	if (after(ack, tp->snd_nxt))
		goto uninteresting_ack;

	if (before(ack, prior_snd_una))
		goto old_ack;

	if (!(flag&FLAG_SLOWPATH) && after(ack, prior_snd_una)) {
		/* Window is constant, pure forward advance.
		 * No more checks are required.
		 * Note, we use the fact that SND.UNA>=SND.WL2.
		 */
		tcp_update_wl(tp, ack, ack_seq);
		tp->snd_una = ack;
		flag |= FLAG_WIN_UPDATE;

		NET_INC_STATS_BH(TCPHPAcks);
	} else {
		if (ack_seq != TCP_SKB_CB(skb)->end_seq)
			flag |= FLAG_DATA;
		else
			NET_INC_STATS_BH(TCPPureAcks);

		flag |= tcp_ack_update_window(sk, tp, skb, ack, ack_seq);

		if (TCP_SKB_CB(skb)->sacked)
			flag |= tcp_sacktag_write_queue(sk, skb, prior_snd_una);

		if (TCP_ECN_rcv_ecn_echo(tp, skb->h.th))
			flag |= FLAG_ECE;
	}

	/* We passed data and got it acked, remove any soft error
	 * log. Something worked...
	 */
	sk->err_soft = 0;
	tp->rcv_tstamp = tcp_time_stamp;
	if ((prior_packets = tp->packets_out) == 0)
		goto no_queue;

	prior_in_flight = tcp_packets_in_flight(tp);

	/* See if we can take anything off of the retransmit queue. */
	flag |= tcp_clean_rtx_queue(sk);

	if (tcp_ack_is_dubious(tp, flag)) {
		/* Advanve CWND, if state allows this. */
		if ((flag&FLAG_DATA_ACKED) && prior_in_flight >= tp->snd_cwnd &&
		    tcp_may_raise_cwnd(tp, flag))
			tcp_cong_avoid(tp);
		tcp_fastretrans_alert(sk, prior_snd_una, prior_packets, flag);
	} else {
		if ((flag&FLAG_DATA_ACKED) && prior_in_flight >= tp->snd_cwnd)
			tcp_cong_avoid(tp);
	}

	if ((flag & FLAG_FORWARD_PROGRESS) || !(flag&FLAG_NOT_DUP))
		dst_confirm(sk->dst_cache);

	return 1;

no_queue:
	tp->probes_out = 0;

	/* If this ack opens up a zero window, clear backoff.  It was
	 * being used to time the probes, and is probably far higher than
	 * it needs to be for normal retransmission.
	 */
	if (tp->send_head)
		tcp_ack_probe(sk);
	return 1;

old_ack:
	if (TCP_SKB_CB(skb)->sacked)
		tcp_sacktag_write_queue(sk, skb, prior_snd_una);

uninteresting_ack:
	SOCK_DEBUG(sk, "Ack %u out of %u:%u\n", ack, tp->snd_una, tp->snd_nxt);
	return 0;
}


/* Look for tcp options. Normally only called on SYN and SYNACK packets.
 * But, this can also be called on packets in the established flow when
 * the fast version below fails.
 */
void tcp_parse_options(struct sk_buff *skb, struct tcp_opt *tp, int estab)
{
	unsigned char *ptr;
	struct tcphdr *th = skb->h.th;
	int length=(th->doff*4)-sizeof(struct tcphdr);

	ptr = (unsigned char *)(th + 1);
	tp->saw_tstamp = 0;

	while(length>0) {
	  	int opcode=*ptr++;
		int opsize;

		switch (opcode) {
			case TCPOPT_EOL:
				return;
			case TCPOPT_NOP:	/* Ref: RFC 793 section 3.1 */
				length--;
				continue;
			default:
				opsize=*ptr++;
				if (opsize < 2) /* "silly options" */
					return;
				if (opsize > length)
					return;	/* don't parse partial options */
	  			switch(opcode) {
				case TCPOPT_MSS:
					if(opsize==TCPOLEN_MSS && th->syn && !estab) {
						u16 in_mss = ntohs(*(__u16 *)ptr);
						if (in_mss) {
							if (tp->user_mss && tp->user_mss < in_mss)
								in_mss = tp->user_mss;
							tp->mss_clamp = in_mss;
						}
					}
					break;
				case TCPOPT_WINDOW:
					if(opsize==TCPOLEN_WINDOW && th->syn && !estab)
						if (sysctl_tcp_window_scaling) {
							tp->wscale_ok = 1;
							tp->snd_wscale = *(__u8 *)ptr;
							if(tp->snd_wscale > 14) {
								if(net_ratelimit())
									printk("tcp_parse_options: Illegal window "
									       "scaling value %d >14 received.",
									       tp->snd_wscale);
								tp->snd_wscale = 14;
							}
						}
					break;
				case TCPOPT_TIMESTAMP:
					if(opsize==TCPOLEN_TIMESTAMP) {
						if ((estab && tp->tstamp_ok) ||
						    (!estab && sysctl_tcp_timestamps)) {
							tp->saw_tstamp = 1;
							tp->rcv_tsval = ntohl(*(__u32 *)ptr);
							tp->rcv_tsecr = ntohl(*(__u32 *)(ptr+4));
						}
					}
					break;
				case TCPOPT_SACK_PERM:
					if(opsize==TCPOLEN_SACK_PERM && th->syn && !estab) {
						if (sysctl_tcp_sack) {
							tp->sack_ok = 1;
							tcp_sack_reset(tp);
						}
					}
					break;

				case TCPOPT_SACK:
					if((opsize >= (TCPOLEN_SACK_BASE + TCPOLEN_SACK_PERBLOCK)) &&
					   !((opsize - TCPOLEN_SACK_BASE) % TCPOLEN_SACK_PERBLOCK) &&
					   tp->sack_ok) {
						TCP_SKB_CB(skb)->sacked = (ptr - 2) - (unsigned char *)th;
					}
	  			};
	  			ptr+=opsize-2;
	  			length-=opsize;
	  	};
	}
}

/* Fast parse options. This hopes to only see timestamps.
 * If it is wrong it falls back on tcp_parse_options().
 */
static __inline__ int tcp_fast_parse_options(struct sk_buff *skb, struct tcphdr *th, struct tcp_opt *tp)
{
	if (th->doff == sizeof(struct tcphdr)>>2) {
		tp->saw_tstamp = 0;
		return 0;
	} else if (tp->tstamp_ok &&
		   th->doff == (sizeof(struct tcphdr)>>2)+(TCPOLEN_TSTAMP_ALIGNED>>2)) {
		__u32 *ptr = (__u32 *)(th + 1);
		if (*ptr == __constant_ntohl((TCPOPT_NOP << 24) | (TCPOPT_NOP << 16)
					     | (TCPOPT_TIMESTAMP << 8) | TCPOLEN_TIMESTAMP)) {
			tp->saw_tstamp = 1;
			++ptr;
			tp->rcv_tsval = ntohl(*ptr);
			++ptr;
			tp->rcv_tsecr = ntohl(*ptr);
			return 1;
		}
	}
	tcp_parse_options(skb, tp, 1);
	return 1;
}

extern __inline__ void
tcp_store_ts_recent(struct tcp_opt *tp)
{
	tp->ts_recent = tp->rcv_tsval;
	tp->ts_recent_stamp = xtime.tv_sec;
}

extern __inline__ void
tcp_replace_ts_recent(struct tcp_opt *tp, u32 seq)
{
	if (tp->saw_tstamp && !after(seq, tp->rcv_wup)) {
		/* PAWS bug workaround wrt. ACK frames, the PAWS discard
		 * extra check below makes sure this can only happen
		 * for pure ACK frames.  -DaveM
		 *
		 * Not only, also it occurs for expired timestamps.
		 */

		if((s32)(tp->rcv_tsval - tp->ts_recent) >= 0 ||
		   xtime.tv_sec >= tp->ts_recent_stamp + TCP_PAWS_24DAYS)
			tcp_store_ts_recent(tp);
	}
}

/* Sorry, PAWS as specified is broken wrt. pure-ACKs -DaveM
 *
 * It is not fatal. If this ACK does _not_ change critical state (seqs, window)
 * it can pass through stack. So, the following predicate verifies that
 * this segment is not used for anything but congestion avoidance or
 * fast retransmit. Moreover, we even are able to eliminate most of such
 * second order effects, if we apply some small "replay" window (~RTO)
 * to timestamp space.
 *
 * All these measures still do not guarantee that we reject wrapped ACKs
 * on networks with high bandwidth, when sequence space is recycled fastly,
 * but it guarantees that such events will be very rare and do not affect
 * connection seriously. This doesn't look nice, but alas, PAWS is really
 * buggy extension.
 *
 * [ Later note. Even worse! It is buggy for segments _with_ data. RFC
 * states that events when retransmit arrives after original data are rare.
 * It is a blatant lie. VJ forgot about fast retransmit! 8)8) It is
 * the biggest problem on large power networks even with minor reordering.
 * OK, let's give it small replay window. If peer clock is even 1hz, it is safe
 * up to bandwidth of 18Gigabit/sec. 8) ]
 */

static int tcp_disordered_ack(struct tcp_opt *tp, struct sk_buff *skb)
{
	struct tcphdr *th = skb->h.th;
	u32 seq = TCP_SKB_CB(skb)->seq;
	u32 ack = TCP_SKB_CB(skb)->ack_seq;

	return (/* 1. Pure ACK with correct sequence number. */
		(th->ack && seq == TCP_SKB_CB(skb)->end_seq && seq == tp->rcv_nxt) &&

		/* 2. ... and duplicate ACK. */
		ack == tp->snd_una &&

		/* 3. ... and does not update window. */
		!tcp_may_update_window(tp, ack, seq, ntohs(th->window)<<tp->snd_wscale) &&

		/* 4. ... and sits in replay window. */
		(s32)(tp->ts_recent - tp->rcv_tsval) <= (tp->rto*1024)/HZ);
}

extern __inline__ int tcp_paws_discard(struct tcp_opt *tp, struct sk_buff *skb)
{
	return ((s32)(tp->ts_recent - tp->rcv_tsval) > TCP_PAWS_WINDOW &&
		xtime.tv_sec < tp->ts_recent_stamp + TCP_PAWS_24DAYS &&
		!tcp_disordered_ack(tp, skb));
}

static int __tcp_sequence(struct tcp_opt *tp, u32 seq, u32 end_seq)
{
	u32 end_window = tp->rcv_wup + tp->rcv_wnd;
#ifdef TCP_FORMAL_WINDOW
	u32 rcv_wnd = tcp_receive_window(tp);
#else
	u32 rcv_wnd = tp->rcv_wnd;
#endif

	if (rcv_wnd &&
	    after(end_seq, tp->rcv_nxt) &&
	    before(seq, end_window))
		return 1;
	if (seq != end_window)
		return 0;
	return (seq == end_seq);
}

/* This functions checks to see if the tcp header is actually acceptable.
 *
 * Actually, our check is seriously broken, we must accept RST,ACK,URG
 * even on zero window effectively trimming data. It is RFC, guys.
 * But our check is so beautiful, that I do not want to repair it
 * now. However, taking into account those stupid plans to start to
 * send some texts with RST, we have to handle at least this case. --ANK
 */
extern __inline__ int tcp_sequence(struct tcp_opt *tp, u32 seq, u32 end_seq, int rst)
{
#ifdef TCP_FORMAL_WINDOW
	u32 rcv_wnd = tcp_receive_window(tp);
#else
	u32 rcv_wnd = tp->rcv_wnd;
#endif
	if (seq == tp->rcv_nxt)
		return (rcv_wnd || (end_seq == seq) || rst);

	return __tcp_sequence(tp, seq, end_seq);
}

/* When we get a reset we do this. */
static void tcp_reset(struct sock *sk)
{
	/* We want the right error as BSD sees it (and indeed as we do). */
	switch (sk->state) {
		case TCP_SYN_SENT:
			sk->err = ECONNREFUSED;
			break;
		case TCP_CLOSE_WAIT:
			sk->err = EPIPE;
			break;
		case TCP_CLOSE:
			return;
		default:
			sk->err = ECONNRESET;
	}

	if (!sk->dead)
		sk->error_report(sk);

	tcp_done(sk);
}

/*
 * 	Process the FIN bit. This now behaves as it is supposed to work
 *	and the FIN takes effect when it is validly part of sequence
 *	space. Not before when we get holes.
 *
 *	If we are ESTABLISHED, a received fin moves us to CLOSE-WAIT
 *	(and thence onto LAST-ACK and finally, CLOSE, we never enter
 *	TIME-WAIT)
 *
 *	If we are in FINWAIT-1, a received FIN indicates simultaneous
 *	close and we go into CLOSING (and later onto TIME-WAIT)
 *
 *	If we are in FINWAIT-2, a received FIN moves us to TIME-WAIT.
 */
static void tcp_fin(struct sk_buff *skb, struct sock *sk, struct tcphdr *th)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);

	tp->fin_seq = TCP_SKB_CB(skb)->end_seq;
	tcp_schedule_ack(tp);

	sk->shutdown |= RCV_SHUTDOWN;
	sk->done = 1;

	switch(sk->state) {
		case TCP_SYN_RECV:
		case TCP_ESTABLISHED:
			/* Move to CLOSE_WAIT */
			tcp_set_state(sk, TCP_CLOSE_WAIT);
			tp->ack.pingpong = 1;
			break;

		case TCP_CLOSE_WAIT:
		case TCP_CLOSING:
			/* Received a retransmission of the FIN, do
			 * nothing.
			 */
			break;
		case TCP_LAST_ACK:
			/* RFC793: Remain in the LAST-ACK state. */
			break;

		case TCP_FIN_WAIT1:
			/* This case occurs when a simultaneous close
			 * happens, we must ack the received FIN and
			 * enter the CLOSING state.
			 */
			tcp_send_ack(sk);
			tcp_set_state(sk, TCP_CLOSING);
			break;
		case TCP_FIN_WAIT2:
			/* Received a FIN -- send ACK and enter TIME_WAIT. */
			tcp_send_ack(sk);
			tcp_time_wait(sk, TCP_TIME_WAIT, 0);
			break;
		default:
			/* Only TCP_LISTEN and TCP_CLOSE are left, in these
			 * cases we should never reach this piece of code.
			 */
			printk("tcp_fin: Impossible, sk->state=%d\n", sk->state);
			break;
	};

	/* It _is_ possible, that we have something out-of-order _after_ FIN.
	 * Probably, we should reset in this case. For now drop them.
	 */
	__skb_queue_purge(&tp->out_of_order_queue);
	if (tp->sack_ok)
		tcp_sack_reset(tp);
	tcp_mem_reclaim(sk);

	if (!sk->dead) {
		sk->state_change(sk);

		/* Do not send POLL_HUP for half duplex close. */
		if (sk->shutdown == SHUTDOWN_MASK || sk->state == TCP_CLOSE)
			sk_wake_async(sk, 1, POLL_HUP);
		else
			sk_wake_async(sk, 1, POLL_IN);
	}
}

static __inline__ int
tcp_sack_extend(struct tcp_sack_block *sp, u32 seq, u32 end_seq)
{
	if (!after(seq, sp->end_seq) && !after(sp->start_seq, end_seq)) {
		if (before(seq, sp->start_seq))
			sp->start_seq = seq;
		if (after(end_seq, sp->end_seq))
			sp->end_seq = end_seq;
		return 1;
	}
	return 0;
}

static __inline__ void tcp_dsack_set(struct tcp_opt *tp, u32 seq, u32 end_seq)
{
	if (tp->sack_ok && sysctl_tcp_dsack) {
		if (before(seq, tp->rcv_nxt))
			NET_INC_STATS_BH(TCPDSACKOldSent);
		else
			NET_INC_STATS_BH(TCPDSACKOfoSent);

		tp->dsack = 1;
		tp->duplicate_sack[0].start_seq = seq;
		tp->duplicate_sack[0].end_seq = end_seq;
		tp->eff_sacks = min(tp->num_sacks+1, 4-tp->tstamp_ok);
	}
}

static __inline__ void tcp_dsack_extend(struct tcp_opt *tp, u32 seq, u32 end_seq)
{
	if (!tp->dsack)
		tcp_dsack_set(tp, seq, end_seq);
	else
		tcp_sack_extend(tp->duplicate_sack, seq, end_seq);
}

static void tcp_send_dupack(struct sock *sk, struct sk_buff *skb)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);

	if (TCP_SKB_CB(skb)->end_seq != TCP_SKB_CB(skb)->seq &&
	    before(TCP_SKB_CB(skb)->seq, tp->rcv_nxt)) {
		NET_INC_STATS_BH(DelayedACKLost);
		tcp_enter_quickack_mode(tp);

		if (tp->sack_ok && sysctl_tcp_dsack) {
			u32 end_seq = TCP_SKB_CB(skb)->end_seq;

			if (after(TCP_SKB_CB(skb)->end_seq, tp->rcv_nxt))
				end_seq = tp->rcv_nxt;
			tcp_dsack_set(tp, TCP_SKB_CB(skb)->seq, end_seq);
		}
	}

	tcp_send_ack(sk);
}

/* These routines update the SACK block as out-of-order packets arrive or
 * in-order packets close up the sequence space.
 */
static void tcp_sack_maybe_coalesce(struct tcp_opt *tp)
{
	int this_sack;
	struct tcp_sack_block *sp = &tp->selective_acks[0];
	struct tcp_sack_block *swalk = sp+1;

	/* See if the recent change to the first SACK eats into
	 * or hits the sequence space of other SACK blocks, if so coalesce.
	 */
	for (this_sack = 1; this_sack < tp->num_sacks; ) {
		if (tcp_sack_extend(sp, swalk->start_seq, swalk->end_seq)) {
			int i;

			/* Zap SWALK, by moving every further SACK up by one slot.
			 * Decrease num_sacks.
			 */
			tp->num_sacks--;
			tp->eff_sacks = min(tp->num_sacks+tp->dsack, 4-tp->tstamp_ok);
			for(i=this_sack; i < tp->num_sacks; i++)
				sp[i] = sp[i+1];
			continue;
		}
		this_sack++, swalk++;
	}
}

static __inline__ void tcp_sack_swap(struct tcp_sack_block *sack1, struct tcp_sack_block *sack2)
{
	__u32 tmp;

	tmp = sack1->start_seq;
	sack1->start_seq = sack2->start_seq;
	sack2->start_seq = tmp;

	tmp = sack1->end_seq;
	sack1->end_seq = sack2->end_seq;
	sack2->end_seq = tmp;
}

static void tcp_sack_new_ofo_skb(struct sock *sk, u32 seq, u32 end_seq)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	struct tcp_sack_block *sp = &tp->selective_acks[0];
	int cur_sacks = tp->num_sacks;
	int this_sack;

	if (!cur_sacks)
		goto new_sack;

	for (this_sack=0; this_sack<cur_sacks; this_sack++, sp++) {
		if (tcp_sack_extend(sp, seq, end_seq)) {
			/* Rotate this_sack to the first one. */
			for (; this_sack>0; this_sack--, sp--)
				tcp_sack_swap(sp, sp-1);
			if (cur_sacks > 1)
				tcp_sack_maybe_coalesce(tp);
			return;
		}
	}

	/* Could not find an adjacent existing SACK, build a new one,
	 * put it at the front, and shift everyone else down.  We
	 * always know there is at least one SACK present already here.
	 *
	 * If the sack array is full, forget about the last one.
	 */
	if (this_sack >= 4) {
		this_sack--;
		tp->num_sacks--;
		sp--;
	}
	for(; this_sack > 0; this_sack--, sp--)
		*sp = *(sp-1);

new_sack:
	/* Build the new head SACK, and we're done. */
	sp->start_seq = seq;
	sp->end_seq = end_seq;
	tp->num_sacks++;
	tp->eff_sacks = min(tp->num_sacks+tp->dsack, 4-tp->tstamp_ok);
}

/* RCV.NXT advances, some SACKs should be eaten. */

static void tcp_sack_remove(struct tcp_opt *tp)
{
	struct tcp_sack_block *sp = &tp->selective_acks[0];
	int num_sacks = tp->num_sacks;
	int this_sack;

	/* Empty ofo queue, hence, all the SACKs are eaten. Clear. */
	if (skb_queue_len(&tp->out_of_order_queue) == 0) {
		tp->num_sacks = 0;
		tp->eff_sacks = tp->dsack;
		return;
	}

	for(this_sack = 0; this_sack < num_sacks; ) {
		/* Check if the start of the sack is covered by RCV.NXT. */
		if (!before(tp->rcv_nxt, sp->start_seq)) {
			int i;

			/* RCV.NXT must cover all the block! */
			BUG_TRAP(!before(tp->rcv_nxt, sp->end_seq));

			/* Zap this SACK, by moving forward any other SACKS. */
			for (i=this_sack+1; i < num_sacks; i++)
				sp[i-1] = sp[i];
			num_sacks--;
			continue;
		}
		this_sack++;
		sp++;
	}
	if (num_sacks != tp->num_sacks) {
		tp->num_sacks = num_sacks;
		tp->eff_sacks = min(tp->num_sacks+tp->dsack, 4-tp->tstamp_ok);
	}
}

/* This one checks to see if we can put data from the
 * out_of_order queue into the receive_queue.
 */
static void tcp_ofo_queue(struct sock *sk)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	__u32 dsack_high = tp->rcv_nxt;
	struct sk_buff *skb;

	while ((skb = skb_peek(&tp->out_of_order_queue)) != NULL) {
		if (after(TCP_SKB_CB(skb)->seq, tp->rcv_nxt))
			break;

		if (before(TCP_SKB_CB(skb)->seq, dsack_high)) {
			__u32 dsack = dsack_high;
			if (before(TCP_SKB_CB(skb)->end_seq, dsack_high))
				dsack_high = TCP_SKB_CB(skb)->end_seq;
			tcp_dsack_extend(tp, TCP_SKB_CB(skb)->seq, dsack);
		}

		if (!after(TCP_SKB_CB(skb)->end_seq, tp->rcv_nxt)) {
			SOCK_DEBUG(sk, "ofo packet was already received \n");
			__skb_unlink(skb, skb->list);
			__kfree_skb(skb);
			continue;
		}
		SOCK_DEBUG(sk, "ofo requeuing : rcv_next %X seq %X - %X\n",
			   tp->rcv_nxt, TCP_SKB_CB(skb)->seq,
			   TCP_SKB_CB(skb)->end_seq);

		__skb_unlink(skb, skb->list);
		__skb_queue_tail(&sk->receive_queue, skb);
		tp->rcv_nxt = TCP_SKB_CB(skb)->end_seq;
		if(skb->h.th->fin)
			tcp_fin(skb, sk, skb->h.th);
	}
}

static void tcp_data_queue(struct sock *sk, struct sk_buff *skb)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	int eaten = 0;

	if (tp->dsack) {
		tp->dsack = 0;
		tp->eff_sacks = min(tp->num_sacks, 4-tp->tstamp_ok);
	}

	/*  Queue data for delivery to the user.
	 *  Packets in sequence go to the receive queue.
	 *  Out of sequence packets to the out_of_order_queue.
	 */
	if (TCP_SKB_CB(skb)->seq == tp->rcv_nxt) {
		/* Ok. In sequence. */
		if (tp->ucopy.task == current &&
		    tp->copied_seq == tp->rcv_nxt &&
		    tp->ucopy.len &&
		    sk->lock.users &&
		    !tp->urg_data) {
			int chunk = min(skb->len, tp->ucopy.len);

			__set_current_state(TASK_RUNNING);

			local_bh_enable();
			if (memcpy_toiovec(tp->ucopy.iov, skb->data, chunk)) {
				sk->err = EFAULT;
				sk->error_report(sk);
			}
			local_bh_disable();
			tp->ucopy.len -= chunk;
			tp->copied_seq += chunk;
			eaten = (chunk == skb->len && !skb->h.th->fin);
		}

		if (!eaten) {
queue_and_out:
			tcp_set_owner_r(skb, sk);
			__skb_queue_tail(&sk->receive_queue, skb);
		}
		tp->rcv_nxt = TCP_SKB_CB(skb)->end_seq;
		if(skb->len)
			tcp_event_data_recv(sk, tp, skb);
		if(skb->h.th->fin)
			tcp_fin(skb, sk, skb->h.th);

		if (skb_queue_len(&tp->out_of_order_queue)) {
			tcp_ofo_queue(sk);

			/* RFC2581. 4.2. SHOULD send immediate ACK, when
			 * gap in queue is filled.
			 */
			if (skb_queue_len(&tp->out_of_order_queue) == 0)
				tp->ack.pingpong = 0;
		}

		if(tp->num_sacks)
			tcp_sack_remove(tp);

		/* Turn on fast path. */ 
		if (skb_queue_len(&tp->out_of_order_queue) == 0 &&
#ifdef TCP_FORMAL_WINDOW
		    tcp_receive_window(tp) &&
#endif
		    !tp->urg_data)
			tcp_fast_path_on(tp);

		if (eaten) {
			__kfree_skb(skb);
		} else if (!sk->dead)
			sk->data_ready(sk, 0);
		return;
	}

#ifdef TCP_DEBUG
	/* An old packet, either a retransmit or some packet got lost. */
	if (!after(TCP_SKB_CB(skb)->end_seq, tp->rcv_nxt)) {
		/* A retransmit, 2nd most common case.  Force an imediate ack.
		 * 
		 * It is impossible, seq is checked by top level.
		 */
		printk("BUG: retransmit in tcp_data_queue: seq %X\n", TCP_SKB_CB(skb)->seq);
		tcp_enter_quickack_mode(tp);
		tcp_schedule_ack(tp);
		__kfree_skb(skb);
		return;
	}
#endif

	tcp_enter_quickack_mode(tp);

	if (before(TCP_SKB_CB(skb)->seq, tp->rcv_nxt)) {
		/* Partial packet, seq < rcv_next < end_seq */
		SOCK_DEBUG(sk, "partial packet: rcv_next %X seq %X - %X\n",
			   tp->rcv_nxt, TCP_SKB_CB(skb)->seq,
			   TCP_SKB_CB(skb)->end_seq);

		tcp_dsack_set(tp, TCP_SKB_CB(skb)->seq, tp->rcv_nxt);
		goto queue_and_out;
	}

	TCP_ECN_check_ce(tp, skb);

	/* Disable header prediction. */
	tp->pred_flags = 0;
	tcp_schedule_ack(tp);

	SOCK_DEBUG(sk, "out of order segment: rcv_next %X seq %X - %X\n",
		   tp->rcv_nxt, TCP_SKB_CB(skb)->seq, TCP_SKB_CB(skb)->end_seq);

	tcp_set_owner_r(skb, sk);

	if (skb_peek(&tp->out_of_order_queue) == NULL) {
		/* Initial out of order segment, build 1 SACK. */
		if(tp->sack_ok) {
			tp->num_sacks = 1;
			tp->dsack = 0;
			tp->eff_sacks = 1;
			tp->selective_acks[0].start_seq = TCP_SKB_CB(skb)->seq;
			tp->selective_acks[0].end_seq = TCP_SKB_CB(skb)->end_seq;
		}
		__skb_queue_head(&tp->out_of_order_queue,skb);
	} else {
		struct sk_buff *skb1=tp->out_of_order_queue.prev;
		u32 seq = TCP_SKB_CB(skb)->seq;
		u32 end_seq = TCP_SKB_CB(skb)->end_seq;

		if (seq == TCP_SKB_CB(skb1)->end_seq) {
			__skb_append(skb1, skb);

			if (tp->num_sacks == 0 ||
			    tp->selective_acks[0].end_seq != seq)
				goto add_sack;

			/* Common case: data arrive in order after hole. */
			tp->selective_acks[0].end_seq = end_seq;
			return;
		}

		/* Find place to insert this segment. */
		do {
			if (!after(TCP_SKB_CB(skb1)->seq, seq))
				break;
		} while ((skb1=skb1->prev) != (struct sk_buff*)&tp->out_of_order_queue);

		/* Do skb overlap to previous one? */
		if (skb1 != (struct sk_buff*)&tp->out_of_order_queue &&
		    before(seq, TCP_SKB_CB(skb1)->end_seq)) {
			if (!after(end_seq, TCP_SKB_CB(skb1)->end_seq)) {
				/* All the bits are present. Drop. */
				__kfree_skb(skb);
				tcp_dsack_set(tp, seq, end_seq);
				goto add_sack;
			}
			if (after(seq, TCP_SKB_CB(skb1)->seq)) {
				/* Partial overlap. */
				tcp_dsack_set(tp, seq, TCP_SKB_CB(skb1)->end_seq);
			} else {
				skb1 = skb1->prev;
			}
		}
		__skb_insert(skb, skb1, skb1->next, &tp->out_of_order_queue);
		
		/* And clean segments covered by new one as whole. */
		while ((skb1 = skb->next) != (struct sk_buff*)&tp->out_of_order_queue &&
		       after(end_seq, TCP_SKB_CB(skb1)->seq)) {
		       if (before(end_seq, TCP_SKB_CB(skb1)->end_seq)) {
			       tcp_dsack_extend(tp, TCP_SKB_CB(skb1)->seq, end_seq);
			       break;
		       }
		       __skb_unlink(skb1, skb1->list);
		       tcp_dsack_extend(tp, TCP_SKB_CB(skb1)->seq, TCP_SKB_CB(skb1)->end_seq);
		       __kfree_skb(skb1);
		}

add_sack:
		if (tp->sack_ok)
			tcp_sack_new_ofo_skb(sk, seq, end_seq);
	}
}


static void tcp_collapse_queue(struct sock *sk, struct sk_buff_head *q)
{
	struct sk_buff *skb = skb_peek(q);
	struct sk_buff *skb_next;

	while (skb &&
	       skb != (struct sk_buff *)q &&
	       (skb_next = skb->next) != (struct sk_buff *)q) {
		struct tcp_skb_cb *scb = TCP_SKB_CB(skb);
		struct tcp_skb_cb *scb_next = TCP_SKB_CB(skb_next);

		if (scb->end_seq == scb_next->seq &&
		    skb_tailroom(skb) >= skb_next->len &&
#define TCP_DONT_COLLAPSE (TCP_FLAG_FIN|TCP_FLAG_URG|TCP_FLAG_SYN)
		    !(tcp_flag_word(skb->h.th)&TCP_DONT_COLLAPSE) &&
		    !(tcp_flag_word(skb_next->h.th)&TCP_DONT_COLLAPSE)) {
			/* OK to collapse two skbs to one */
			memcpy(skb_put(skb, skb_next->len), skb_next->data, skb_next->len);
			__skb_unlink(skb_next, skb_next->list);
			scb->end_seq = scb_next->end_seq;
			__kfree_skb(skb_next);
			NET_INC_STATS_BH(TCPRcvCollapsed);
		} else {
			/* Lots of spare tailroom, reallocate this skb to trim it. */
			if (tcp_win_from_space(skb->truesize) > skb->len &&
			    skb_tailroom(skb) > sizeof(struct sk_buff) + 16) {
				struct sk_buff *nskb;

				nskb = skb_copy_expand(skb, skb_headroom(skb), 0, GFP_ATOMIC);
				if (nskb) {
					tcp_set_owner_r(nskb, sk);
					memcpy(nskb->data-skb_headroom(skb),
					       skb->data-skb_headroom(skb),
					       skb_headroom(skb));
					__skb_append(skb, nskb);
					__skb_unlink(skb, skb->list);
					__kfree_skb(skb);
				}
			}
			skb = skb_next;
		}
	}
}

/* Clean the out_of_order queue if we can, trying to get
 * the socket within its memory limits again.
 *
 * Return less than zero if we should start dropping frames
 * until the socket owning process reads some of the data
 * to stabilize the situation.
 */
static int tcp_prune_queue(struct sock *sk)
{
	struct tcp_opt *tp = &sk->tp_pinfo.af_tcp; 

	SOCK_DEBUG(sk, "prune_queue: c=%x\n", tp->copied_seq);

	NET_INC_STATS_BH(PruneCalled);

	if (atomic_read(&sk->rmem_alloc) >= sk->rcvbuf)
		tcp_clamp_window(sk, tp);
	else if (tcp_memory_pressure)
		tp->rcv_ssthresh = min(tp->rcv_ssthresh, 4*tp->advmss);

	tcp_collapse_queue(sk, &sk->receive_queue);
	tcp_collapse_queue(sk, &tp->out_of_order_queue);
	tcp_mem_reclaim(sk);

	if (atomic_read(&sk->rmem_alloc) <= sk->rcvbuf)
		return 0;

	/* Collapsing did not help, destructive actions follow.
	 * This must not ever occur. */

	/* First, purge the out_of_order queue. */
	if (skb_queue_len(&tp->out_of_order_queue)) {
		net_statistics[smp_processor_id()*2].OfoPruned += skb_queue_len(&tp->out_of_order_queue);
		__skb_queue_purge(&tp->out_of_order_queue);

		/* Reset SACK state.  A conforming SACK implementation will
		 * do the same at a timeout based retransmit.  When a connection
		 * is in a sad state like this, we care only about integrity
		 * of the connection not performance.
		 */
		if(tp->sack_ok)
			tcp_sack_reset(tp);
		tcp_mem_reclaim(sk);
	}

	if(atomic_read(&sk->rmem_alloc) <= sk->rcvbuf)
		return 0;

	/* If we are really being abused, tell the caller to silently
	 * drop receive data on the floor.  It will get retransmitted
	 * and hopefully then we'll have sufficient space.
	 */
	NET_INC_STATS_BH(RcvPruned);

	/* Massive buffer overcommit. */
	return -1;
}

static inline int tcp_rmem_schedule(struct sock *sk, struct sk_buff *skb)
{
	return (int)skb->truesize <= sk->forward_alloc ||
		tcp_mem_schedule(sk, skb->truesize, 1);
}

/*
 *	This routine handles the data.  If there is room in the buffer,
 *	it will be have already been moved into it.  If there is no
 *	room, then we will just have to discard the packet.
 */

static void tcp_data(struct sk_buff *skb, struct sock *sk, unsigned int len)
{
	struct tcphdr *th;
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);

	th = skb->h.th;
	skb_pull(skb, th->doff*4);
	skb_trim(skb, len - (th->doff*4));

        if (skb->len == 0 && !th->fin)
		goto drop;

	TCP_ECN_accept_cwr(tp, skb);

	/* 
	 *	If our receive queue has grown past its limits shrink it.
	 *	Make sure to do this before moving rcv_nxt, otherwise
	 *	data might be acked for that we don't have enough room.
	 */
	if (atomic_read(&sk->rmem_alloc) > sk->rcvbuf ||
	    !tcp_rmem_schedule(sk, skb)) {
		if (tcp_prune_queue(sk) < 0 || !tcp_rmem_schedule(sk, skb))
			goto drop;
	}

	tcp_data_queue(sk, skb);

#ifdef TCP_DEBUG
	if (before(tp->rcv_nxt, tp->copied_seq)) {
		printk(KERN_DEBUG "*** tcp.c:tcp_data bug acked < copied\n");
		tp->rcv_nxt = tp->copied_seq;
	}
#endif
	return;

drop:
	__kfree_skb(skb);
}

/* RFC2861, slow part. Adjust cwnd, after it was not full during one rto.
 * As additional protections, we do not touch cwnd in retransmission phases,
 * and if application hit its sndbuf limit recently.
 */
void tcp_cwnd_application_limited(struct sock *sk)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);

	if (tp->ca_state == TCP_CA_Open &&
	    sk->socket && !test_bit(SOCK_NOSPACE, &sk->socket->flags)) {
		/* Limited by application or receiver window. */
		u32 win_used = max(tp->snd_cwnd_used, 2);
		if (win_used < tp->snd_cwnd) {
			tp->snd_ssthresh = tcp_current_ssthresh(tp);
			tp->snd_cwnd = (tp->snd_cwnd+win_used)>>1;
		}
		tp->snd_cwnd_used = 0;
	}
	tp->snd_cwnd_stamp = tcp_time_stamp;
}


/* When incoming ACK allowed to free some skb from write_queue,
 * we remember this event in flag tp->queue_shrunk and wake up socket
 * on the exit from tcp input handler.
 */
static void tcp_new_space(struct sock *sk)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);

	if (tp->packets_out < tp->snd_cwnd &&
	    !(sk->userlocks&SOCK_SNDBUF_LOCK) &&
	    !tcp_memory_pressure &&
	    atomic_read(&tcp_memory_allocated) < sysctl_tcp_mem[0]) {
		int sndmem, demanded;

		sndmem = tp->mss_clamp+MAX_TCP_HEADER+16+sizeof(struct sk_buff);
		demanded = max(tp->snd_cwnd, tp->reordering+1);
		sndmem *= 2*demanded;
		if (sndmem > sk->sndbuf)
			sk->sndbuf = min(sndmem, sysctl_tcp_wmem[2]);
		tp->snd_cwnd_stamp = tcp_time_stamp;
	}

	/* Wakeup users. */
	if (tcp_wspace(sk) >= tcp_min_write_space(sk)) {
		struct socket *sock = sk->socket;

		clear_bit(SOCK_NOSPACE, &sock->flags);

		if (sk->sleep && waitqueue_active(sk->sleep))
			wake_up_interruptible(sk->sleep);

		if (sock->fasync_list && !(sk->shutdown&SEND_SHUTDOWN))
			sock_wake_async(sock, 2, POLL_OUT);

		/* Satisfy those who hook write_space() callback. */
		if (sk->write_space != tcp_write_space)
			sk->write_space(sk);
	}
}

static inline void tcp_check_space(struct sock *sk)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);

	if (tp->queue_shrunk) {
		tp->queue_shrunk = 0;
		if (sk->socket && test_bit(SOCK_NOSPACE, &sk->socket->flags))
			tcp_new_space(sk);
	}
}

static void __tcp_data_snd_check(struct sock *sk, struct sk_buff *skb)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);

	if (after(TCP_SKB_CB(skb)->end_seq, tp->snd_una + tp->snd_wnd) ||
	    tcp_packets_in_flight(tp) >= tp->snd_cwnd ||
	    tcp_write_xmit(sk))
		tcp_check_probe_timer(sk, tp);
}

static __inline__ void tcp_data_snd_check(struct sock *sk)
{
	struct sk_buff *skb = sk->tp_pinfo.af_tcp.send_head;

	if (skb != NULL)
		__tcp_data_snd_check(sk, skb);
	tcp_check_space(sk);
}

/*
 * Check if sending an ack is needed.
 */
static __inline__ void __tcp_ack_snd_check(struct sock *sk, int ofo_possible)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);

	    /* More than one full frame received... */
	if (((tp->rcv_nxt - tp->rcv_wup) > tp->ack.rcv_mss
	     /* ... and right edge of window advances far enough.
	      * (tcp_recvmsg() will send ACK otherwise). Or...
	      */
	     && __tcp_select_window(sk) >= tp->rcv_wnd) ||
	    /* We ACK each frame or... */
	    tcp_in_quickack_mode(tp) ||
	    /* We have out of order data. */
	    (ofo_possible &&
	     skb_peek(&tp->out_of_order_queue) != NULL)) {
		/* Then ack it now */
		tcp_send_ack(sk);
	} else {
		/* Else, send delayed ack. */
		tcp_send_delayed_ack(sk);
	}
}

static __inline__ void tcp_ack_snd_check(struct sock *sk)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	if (!tcp_ack_scheduled(tp)) {
		/* We sent a data segment already. */
		return;
	}
	__tcp_ack_snd_check(sk, 1);
}

/*
 *	This routine is only called when we have urgent data
 *	signalled. Its the 'slow' part of tcp_urg. It could be
 *	moved inline now as tcp_urg is only called from one
 *	place. We handle URGent data wrong. We have to - as
 *	BSD still doesn't use the correction from RFC961.
 *	For 1003.1g we should support a new option TCP_STDURG to permit
 *	either form (or just set the sysctl tcp_stdurg).
 */
 
static void tcp_check_urg(struct sock * sk, struct tcphdr * th)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	u32 ptr = ntohs(th->urg_ptr);

	if (ptr && !sysctl_tcp_stdurg)
		ptr--;
	ptr += ntohl(th->seq);

	/* Ignore urgent data that we've already seen and read. */
	if (after(tp->copied_seq, ptr))
		return;

	/* Do we already have a newer (or duplicate) urgent pointer? */
	if (tp->urg_data && !after(ptr, tp->urg_seq))
		return;

	/* Tell the world about our new urgent pointer. */
	if (sk->proc != 0) {
		if (sk->proc > 0)
			kill_proc(sk->proc, SIGURG, 1);
		else
			kill_pg(-sk->proc, SIGURG, 1);
		sk_wake_async(sk, 3, POLL_PRI);
	}

	/* We may be adding urgent data when the last byte read was
	 * urgent. To do this requires some care. We cannot just ignore
	 * tp->copied_seq since we would read the last urgent byte again
	 * as data, nor can we alter copied_seq until this data arrives
	 * or we break the sematics of SIOCATMARK (and thus sockatmark())
	 */
	if (tp->urg_seq == tp->copied_seq)
		tp->copied_seq++;	/* Move the copied sequence on correctly */
	tp->urg_data = TCP_URG_NOTYET;
	tp->urg_seq = ptr;

	/* Disable header prediction. */
	tp->pred_flags = 0;
}

/* This is the 'fast' part of urgent handling. */
static inline void tcp_urg(struct sock *sk, struct tcphdr *th, unsigned long len)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);

	/* Check if we get a new urgent pointer - normally not. */
	if (th->urg)
		tcp_check_urg(sk,th);

	/* Do we wait for any urgent data? - normally not... */
	if (tp->urg_data == TCP_URG_NOTYET) {
		u32 ptr = tp->urg_seq - ntohl(th->seq) + (th->doff*4);

		/* Is the urgent pointer pointing into this packet? */	 
		if (ptr < len) {
			tp->urg_data = TCP_URG_VALID | *(ptr + (unsigned char *) th);
			if (!sk->dead)
				sk->data_ready(sk,0);
		}
	}
}

static int tcp_copy_to_iovec(struct sock *sk, struct sk_buff *skb, int hlen)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	int chunk = skb->len - hlen;
	int err;

	local_bh_enable();
	if (skb->ip_summed==CHECKSUM_UNNECESSARY)
		err = memcpy_toiovec(tp->ucopy.iov, skb->h.raw + hlen, chunk);
	else
		err = copy_and_csum_toiovec(tp->ucopy.iov, skb, hlen);

	if (!err) {
update:
		tp->ucopy.len -= chunk;
		tp->copied_seq += chunk;
		local_bh_disable();
		return 0;
	}

	if (err == -EFAULT) {
		sk->err = EFAULT;
		sk->error_report(sk);
		goto update;
	}

	local_bh_disable();
	return err;
}

static int __tcp_checksum_complete_user(struct sock *sk, struct sk_buff *skb)
{
	int result;

	if (sk->lock.users) {
		local_bh_enable();
		result = __tcp_checksum_complete(skb);
		local_bh_disable();
	} else {
		result = __tcp_checksum_complete(skb);
	}
	return result;
}

static __inline__ int
tcp_checksum_complete_user(struct sock *sk, struct sk_buff *skb)
{
	return skb->ip_summed != CHECKSUM_UNNECESSARY &&
		__tcp_checksum_complete_user(sk, skb);
}

/*
 *	TCP receive function for the ESTABLISHED state. 
 *
 *	It is split into a fast path and a slow path. The fast path is 
 * 	disabled when:
 *	- A zero window was announced from us - zero window probing
 *        is only handled properly in the slow path. 
 *	  [ NOTE: actually, it was made incorrectly and nobody ever noticed
 *	    this! Reason is clear: 1. Correct senders do not send
 *	    to zero window. 2. Even if a sender sends to zero window,
 *	    nothing terrible occurs.
 *
 *	    For now I cleaned this and fast path is really always disabled,
 *	    when window is zero, but I would be more happy to remove these
 *	    checks. Code will be only cleaner and _faster_.    --ANK
 *	
 *	    Later note. I've just found that slow path also accepts
 *	    out of window segments, look at tcp_sequence(). So...
 *	    it is the last argument: I repair all and comment out
 *	    repaired code by TCP_FORMAL_WINDOW.
 *	    [ I remember one rhyme from a chidren's book. (I apologize,
 *	      the trasnlation is not rhymed 8)): people in one (jewish) village
 *	      decided to build sauna, but divided to two parties.
 *	      The first one insisted that battens should not be dubbed,
 *	      another objected that foots will suffer of splinters,
 *	      the first fended that dubbed wet battens are too slippy
 *	      and people will fall and it is much more serious!
 *	      Certaiinly, all they went to rabbi.
 *	      After some thinking, he judged: "Do not be lazy!
 *	      Certainly, dub the battens! But put them by dubbed surface down."
 *          ]
 *        ]
 *
 *	- Out of order segments arrived.
 *	- Urgent data is expected.
 *	- There is no buffer space left
 *	- Unexpected TCP flags/window values/header lengths are received
 *	  (detected by checking the TCP header against pred_flags) 
 *	- Data is sent in both directions. Fast path only supports pure senders
 *	  or pure receivers (this means either the sequence number or the ack
 *	  value must stay constant)
 *	- Unexpected TCP option.
 *
 *	When these conditions are not satisfied it drops into a standard 
 *	receive procedure patterned after RFC793 to handle all cases.
 *	The first three cases are guaranteed by proper pred_flags setting,
 *	the rest is checked inline. Fast processing is turned on in 
 *	tcp_data_queue when everything is OK.
 */
int tcp_rcv_established(struct sock *sk, struct sk_buff *skb,
			struct tcphdr *th, unsigned len)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);

	/*
	 *	Header prediction.
	 *	The code losely follows the one in the famous 
	 *	"30 instruction TCP receive" Van Jacobson mail.
	 *	
	 *	Van's trick is to deposit buffers into socket queue 
	 *	on a device interrupt, to call tcp_recv function
	 *	on the receive process context and checksum and copy
	 *	the buffer to user space. smart...
	 *
	 *	Our current scheme is not silly either but we take the 
	 *	extra cost of the net_bh soft interrupt processing...
	 *	We do checksum and copy also but from device to kernel.
	 */

	tp->saw_tstamp = 0;

	/*	pred_flags is 0xS?10 << 16 + snd_wnd
	 *	if header_predition is to be made
	 *	'S' will always be tp->tcp_header_len >> 2
	 *	'?' will be 0 for the fast path, otherwise pred_flags is 0 to
	 *  turn it off	(when there are holes in the receive 
	 *	 space for instance)
	 *	PSH flag is ignored.
	 */

	if ((tcp_flag_word(th) & TCP_HP_BITS) == tp->pred_flags &&
		TCP_SKB_CB(skb)->seq == tp->rcv_nxt) {
		int tcp_header_len = tp->tcp_header_len;

		/* Timestamp header prediction: tcp_header_len
		 * is automatically equal to th->doff*4 due to pred_flags
		 * match.
		 */

		/* Check timestamp */
		if (tcp_header_len == sizeof(struct tcphdr) + TCPOLEN_TSTAMP_ALIGNED) {
			__u32 *ptr = (__u32 *)(th + 1);

			/* No? Slow path! */
			if (*ptr != __constant_ntohl((TCPOPT_NOP << 24) | (TCPOPT_NOP << 16)
						     | (TCPOPT_TIMESTAMP << 8) | TCPOLEN_TIMESTAMP))
				goto slow_path;

			tp->saw_tstamp = 1;
			++ptr; 
			tp->rcv_tsval = ntohl(*ptr);
			++ptr;
			tp->rcv_tsecr = ntohl(*ptr);

			/* If PAWS failed, check it more carefully in slow path */
			if ((s32)(tp->rcv_tsval - tp->ts_recent) < 0)
				goto slow_path;

			/* Predicted packet is in window by definition.
			 * seq == rcv_nxt and rcv_wup <= rcv_nxt.
			 * Hence, check seq<=rcv_wup reduces to:
			 */
			if (tp->rcv_nxt == tp->rcv_wup)
				tcp_store_ts_recent(tp);
		}

		if (len <= tcp_header_len) {
			/* Bulk data transfer: sender */
			if (len == tcp_header_len) {
				/* We know that such packets are checksummed
				 * on entry.
				 */
				tcp_ack(sk, skb, 0);
				__kfree_skb(skb); 
				tcp_data_snd_check(sk);
				return 0;
			} else { /* Header too small */
				TCP_INC_STATS_BH(TcpInErrs);
				goto discard;
			}
		} else {
			int eaten = 0;

			if (tp->ucopy.task == current &&
			    tp->copied_seq == tp->rcv_nxt &&
			    len - tcp_header_len <= tp->ucopy.len &&
			    sk->lock.users) {
				eaten = 1;

				NET_INC_STATS_BH(TCPHPHitsToUser);

				__set_current_state(TASK_RUNNING);

				if (tcp_copy_to_iovec(sk, skb, tcp_header_len))
					goto csum_error;

				__skb_pull(skb,tcp_header_len);

				tp->rcv_nxt = TCP_SKB_CB(skb)->end_seq;
			} else {
				if (tcp_checksum_complete_user(sk, skb))
					goto csum_error;

				if ((int)skb->truesize > sk->forward_alloc)
					goto step5;

				NET_INC_STATS_BH(TCPHPHits);

				/* Bulk data transfer: receiver */
				__skb_pull(skb,tcp_header_len);
				__skb_queue_tail(&sk->receive_queue, skb);
				tcp_set_owner_r(skb, sk);
				tp->rcv_nxt = TCP_SKB_CB(skb)->end_seq;
			}

			tcp_event_data_recv(sk, tp, skb);

			if (TCP_SKB_CB(skb)->ack_seq != tp->snd_una) {
				/* Well, only one small jumplet in fast path... */
				tcp_ack(sk, skb, FLAG_DATA);
				tcp_data_snd_check(sk);
				if (!tcp_ack_scheduled(tp))
					goto no_ack;
			}

			if (eaten) {
				if (tcp_in_quickack_mode(tp)) {
					tcp_send_ack(sk);
				} else {
					tcp_send_delayed_ack(sk);
				}
			} else {
				__tcp_ack_snd_check(sk, 0);
			}

no_ack:
			if (eaten)
				__kfree_skb(skb);
			else
				sk->data_ready(sk, 0);
			return 0;
		}
	}

slow_path:
	if (len < (th->doff<<2) || tcp_checksum_complete_user(sk, skb))
		goto csum_error;

	/*
	 * RFC1323: H1. Apply PAWS check first.
	 */
	if (tcp_fast_parse_options(skb, th, tp) && tp->saw_tstamp &&
	    tcp_paws_discard(tp, skb)) {
		if (!th->rst) {
			NET_INC_STATS_BH(PAWSEstabRejected);
			tcp_send_dupack(sk, skb);
			goto discard;
		}
		/* Resets are accepted even if PAWS failed.

		   ts_recent update must be made after we are sure
		   that the packet is in window.
		 */
	}

	/*
	 *	Standard slow path.
	 */

	if (!tcp_sequence(tp, TCP_SKB_CB(skb)->seq, TCP_SKB_CB(skb)->end_seq, th->rst)) {
		/* RFC793, page 37: "In all states except SYN-SENT, all reset
		 * (RST) segments are validated by checking their SEQ-fields."
		 * And page 69: "If an incoming segment is not acceptable,
		 * an acknowledgment should be sent in reply (unless the RST bit
		 * is set, if so drop the segment and return)".
		 */
		if (!th->rst)
			tcp_send_dupack(sk, skb);
		goto discard;
	}

	if(th->rst) {
		tcp_reset(sk);
		goto discard;
	}

	tcp_replace_ts_recent(tp, TCP_SKB_CB(skb)->seq);

	if(th->syn && TCP_SKB_CB(skb)->seq != tp->syn_seq) {
		TCP_INC_STATS_BH(TcpInErrs);
		NET_INC_STATS_BH(TCPAbortOnSyn);
		tcp_reset(sk);
		return 1;
	}

step5:
	if(th->ack)
		tcp_ack(sk, skb, FLAG_SLOWPATH);

	/* Process urgent data. */
	tcp_urg(sk, th, len);

	/* step 7: process the segment text */
	tcp_data(skb, sk, len);

	tcp_data_snd_check(sk);
	tcp_ack_snd_check(sk);
	return 0;

csum_error:
	TCP_INC_STATS_BH(TcpInErrs);

discard:
	__kfree_skb(skb);
	return 0;
}

static int tcp_rcv_synsent_state_process(struct sock *sk, struct sk_buff *skb,
					 struct tcphdr *th, unsigned len)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	int saved_clamp = tp->mss_clamp;

	tcp_parse_options(skb, tp, 0);

	if (th->ack) {
		/* rfc793:
		 * "If the state is SYN-SENT then
		 *    first check the ACK bit
		 *      If the ACK bit is set
		 *	  If SEG.ACK =< ISS, or SEG.ACK > SND.NXT, send
		 *        a reset (unless the RST bit is set, if so drop
		 *        the segment and return)"
		 *
		 *  We do not send data with SYN, so that RFC-correct
		 *  test reduces to:
		 */
		if (TCP_SKB_CB(skb)->ack_seq != tp->snd_nxt)
			goto reset_and_undo;

		if (tp->saw_tstamp && tp->rcv_tsecr &&
		    !between(tp->rcv_tsecr, tp->retrans_stamp, tcp_time_stamp)) {
			NET_INC_STATS_BH(PAWSActiveRejected);
			goto reset_and_undo;
		}

		/* Now ACK is acceptable.
		 *
		 * "If the RST bit is set
		 *    If the ACK was acceptable then signal the user "error:
		 *    connection reset", drop the segment, enter CLOSED state,
		 *    delete TCB, and return."
		 */

		if (th->rst) {
			tcp_reset(sk);
			goto discard;
		}

		/* rfc793:
		 *   "fifth, if neither of the SYN or RST bits is set then
		 *    drop the segment and return."
		 *
		 *    See note below!
		 *                                        --ANK(990513)
		 */
		if (!th->syn)
			goto discard_and_undo;

		/* rfc793:
		 *   "If the SYN bit is on ...
		 *    are acceptable then ...
		 *    (our SYN has been ACKed), change the connection
		 *    state to ESTABLISHED..."
		 */

		TCP_ECN_rcv_synack(tp, th);

		tp->snd_wl1 = TCP_SKB_CB(skb)->seq;
		tcp_ack(sk, skb, FLAG_SLOWPATH);

		/* Ok.. it's good. Set up sequence numbers and
		 * move to established.
		 */
		tp->rcv_nxt = TCP_SKB_CB(skb)->seq+1;
		tp->rcv_wup = TCP_SKB_CB(skb)->seq+1;

		/* RFC1323: The window in SYN & SYN/ACK segments is
		 * never scaled.
		 */
		tp->snd_wnd = ntohs(th->window);
		tcp_init_wl(tp, TCP_SKB_CB(skb)->ack_seq, TCP_SKB_CB(skb)->seq);
		tp->syn_seq = TCP_SKB_CB(skb)->seq;
		tp->fin_seq = TCP_SKB_CB(skb)->seq;

		if (tp->wscale_ok == 0) {
			tp->snd_wscale = tp->rcv_wscale = 0;
			tp->window_clamp = min(tp->window_clamp,65535);
		}

		if (tp->saw_tstamp) {
			tp->tstamp_ok = 1;
			tp->tcp_header_len =
				sizeof(struct tcphdr) + TCPOLEN_TSTAMP_ALIGNED;
			tp->advmss -= TCPOLEN_TSTAMP_ALIGNED;
			tcp_store_ts_recent(tp);
		} else {
			tp->tcp_header_len = sizeof(struct tcphdr);
		}

		if (tp->sack_ok && sysctl_tcp_fack)
			tp->sack_ok |= 2;

		tcp_sync_mss(sk, tp->pmtu_cookie);
		tcp_initialize_rcv_mss(sk);
		tcp_init_metrics(sk);
		tcp_init_buffer_space(sk);

		if (sk->keepopen)
			tcp_reset_keepalive_timer(sk, keepalive_time_when(tp));

		if (tp->snd_wscale == 0)
			__tcp_fast_path_on(tp, tp->snd_wnd);
		else
			tp->pred_flags = 0;

		/* Remember, tcp_poll() does not lock socket!
		 * Change state from SYN-SENT only after copied_seq
		 * is initilized. */
		tp->copied_seq = tp->rcv_nxt;
		mb();
		tcp_set_state(sk, TCP_ESTABLISHED);

		if(!sk->dead) {
			sk->state_change(sk);
			sk_wake_async(sk, 0, POLL_OUT);
		}

		if (tp->write_pending || tp->defer_accept) {
			/* Save one ACK. Data will be ready after
			 * several ticks, if write_pending is set.
			 *
			 * It may be deleted, but with this feature tcpdumps
			 * look so _wonderfully_ clever, that I was not able
			 * to stand against the temptation 8)     --ANK
			 */
			tcp_schedule_ack(tp);
			tp->ack.lrcvtime = tcp_time_stamp;
			tcp_enter_quickack_mode(tp);
			tcp_reset_xmit_timer(sk, TCP_TIME_DACK, TCP_DELACK_MAX);

discard:
			__kfree_skb(skb);
			return 0;
		} else {
			tcp_send_ack(sk);
		}
		return -1;
	}

	/* No ACK in the segment */

	if (th->rst) {
		/* rfc793:
		 * "If the RST bit is set
		 *
		 *      Otherwise (no ACK) drop the segment and return."
		 */

		goto discard_and_undo;
	}

	/* PAWS check. */
	if (tp->ts_recent_stamp && tp->saw_tstamp && tcp_paws_check(tp, 0))
		goto discard_and_undo;

	if (th->syn) {
		/* We see SYN without ACK. It is attempt of
		 * simultaneous connect with crossed SYNs.
		 * Particularly, it can be connect to self.
		 */
		tcp_set_state(sk, TCP_SYN_RECV);

		if (tp->saw_tstamp) {
			tp->tstamp_ok = 1;
			tcp_store_ts_recent(tp);
			tp->tcp_header_len =
				sizeof(struct tcphdr) + TCPOLEN_TSTAMP_ALIGNED;
		} else {
			tp->tcp_header_len = sizeof(struct tcphdr);
		}

		tp->rcv_nxt = TCP_SKB_CB(skb)->seq + 1;
		tp->rcv_wup = TCP_SKB_CB(skb)->seq + 1;

		/* RFC1323: The window in SYN & SYN/ACK segments is
		 * never scaled.
		 */
		tp->snd_wnd = ntohs(th->window);
		tp->snd_wl1 = TCP_SKB_CB(skb)->seq;
		tp->max_window = tp->snd_wnd;

		tcp_sync_mss(sk, tp->pmtu_cookie);
		tcp_initialize_rcv_mss(sk);

		TCP_ECN_rcv_syn(tp, th);

		tcp_send_synack(sk);
#if 0
		/* Note, we could accept data and URG from this segment.
		 * There are no obstacles to make this.
		 *
		 * However, if we ignore data in ACKless segments sometimes,
		 * we have no reasons to accept it sometimes.
		 * Also, seems the code doing it in step6 of tcp_rcv_state_process
		 * is not flawless. So, discard packet for sanity.
		 * Uncomment this return to process the data.
		 */
		return -1;
#else
		goto discard;
#endif
	}
	/* "fifth, if neither of the SYN or RST bits is set then
	 * drop the segment and return."
	 */

discard_and_undo:
	tcp_clear_options(tp);
	tp->mss_clamp = saved_clamp;
	goto discard;

reset_and_undo:
	tcp_clear_options(tp);
	tp->mss_clamp = saved_clamp;
	return 1;
}


/*
 *	This function implements the receiving procedure of RFC 793 for
 *	all states except ESTABLISHED and TIME_WAIT. 
 *	It's called from both tcp_v4_rcv and tcp_v6_rcv and should be
 *	address independent.
 */
	
int tcp_rcv_state_process(struct sock *sk, struct sk_buff *skb,
			  struct tcphdr *th, unsigned len)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	int queued = 0;

	tp->saw_tstamp = 0;

	switch (sk->state) {
	case TCP_CLOSE:
		goto discard;

	case TCP_LISTEN:
		if(th->ack)
			return 1;

		if(th->syn) {
			if(tp->af_specific->conn_request(sk, skb) < 0)
				return 1;

			/* Now we have several options: In theory there is 
			 * nothing else in the frame. KA9Q has an option to 
			 * send data with the syn, BSD accepts data with the
			 * syn up to the [to be] advertised window and 
			 * Solaris 2.1 gives you a protocol error. For now 
			 * we just ignore it, that fits the spec precisely 
			 * and avoids incompatibilities. It would be nice in
			 * future to drop through and process the data.
			 *
			 * Now that TTCP is starting to be used we ought to 
			 * queue this data.
			 * But, this leaves one open to an easy denial of
		 	 * service attack, and SYN cookies can't defend
			 * against this problem. So, we drop the data
			 * in the interest of security over speed.
			 */
			goto discard;
		}
		goto discard;

	case TCP_SYN_SENT:
		queued = tcp_rcv_synsent_state_process(sk, skb, th, len);
		if (queued >= 0)
			return queued;
		queued = 0;
		goto step6;
	}

	if (tcp_fast_parse_options(skb, th, tp) && tp->saw_tstamp &&
	    tcp_paws_discard(tp, skb)) {
		if (!th->rst) {
			NET_INC_STATS_BH(PAWSEstabRejected);
			tcp_send_dupack(sk, skb);
			goto discard;
		}
		/* Reset is accepted even if it did not pass PAWS. */
	}

	/* step 1: check sequence number */
	if (!tcp_sequence(tp, TCP_SKB_CB(skb)->seq, TCP_SKB_CB(skb)->end_seq, th->rst)) {
		if (!th->rst)
			tcp_send_dupack(sk, skb);
		goto discard;
	}

	/* step 2: check RST bit */
	if(th->rst) {
		tcp_reset(sk);
		goto discard;
	}

	tcp_replace_ts_recent(tp, TCP_SKB_CB(skb)->seq);

	/* step 3: check security and precedence [ignored] */

	/*	step 4:
	 *
	 *	Check for a SYN, and ensure it matches the SYN we were
	 *	first sent. We have to handle the rather unusual (but valid)
	 *	sequence that KA9Q derived products may generate of
	 *
	 *	SYN
	 *				SYN|ACK Data
	 *	ACK	(lost)
	 *				SYN|ACK Data + More Data
	 *	.. we must ACK not RST...
	 *
	 *	We keep syn_seq as the sequence space occupied by the 
	 *	original syn. 
	 */

	if (th->syn && TCP_SKB_CB(skb)->seq != tp->syn_seq) {
		NET_INC_STATS_BH(TCPAbortOnSyn);
		tcp_reset(sk);
		return 1;
	}

	/* step 5: check the ACK field */
	if (th->ack) {
		int acceptable = tcp_ack(sk, skb, FLAG_SLOWPATH);

		switch(sk->state) {
		case TCP_SYN_RECV:
			if (acceptable) {
				tp->copied_seq = tp->rcv_nxt;
				mb();
				tcp_set_state(sk, TCP_ESTABLISHED);

				/* Note, that this wakeup is only for marginal
				 * crossed SYN case. Passively open sockets
				 * are not waked up, because sk->sleep == NULL
				 * and sk->socket == NULL.
				 */
				if (sk->socket) {
					sk->state_change(sk);
					sk_wake_async(sk,0,POLL_OUT);
				}

				tp->snd_una = TCP_SKB_CB(skb)->ack_seq;
				tp->snd_wnd = ntohs(th->window) << tp->snd_wscale;
				tcp_init_wl(tp, TCP_SKB_CB(skb)->ack_seq, TCP_SKB_CB(skb)->seq);

				/* tcp_ack considers this ACK as duplicate
				 * and does not calculate rtt.
				 * Fix it at least with timestamps.
				 */
				if (tp->saw_tstamp && tp->rcv_tsecr && !tp->srtt)
					tcp_ack_saw_tstamp(tp, 0);

				if (tp->tstamp_ok)
					tp->advmss -= TCPOLEN_TSTAMP_ALIGNED;

				tcp_init_metrics(sk);
				tcp_initialize_rcv_mss(sk);
				tcp_init_buffer_space(sk);
				tcp_fast_path_on(tp);
			} else {
				return 1;
			}
			break;

		case TCP_FIN_WAIT1:
			if (tp->snd_una == tp->write_seq) {
				tcp_set_state(sk, TCP_FIN_WAIT2);
				sk->shutdown |= SEND_SHUTDOWN;
				dst_confirm(sk->dst_cache);

				if (!sk->dead) {
					/* Wake up lingering close() */
					sk->state_change(sk);
				} else {
					int tmo;

					if (tp->linger2 < 0 ||
					    (TCP_SKB_CB(skb)->end_seq != TCP_SKB_CB(skb)->seq &&
					     after(TCP_SKB_CB(skb)->end_seq - th->fin, tp->rcv_nxt))) {
						tcp_done(sk);
						NET_INC_STATS_BH(TCPAbortOnData);
						return 1;
					}

					tmo = tcp_fin_time(tp);
					if (tmo > TCP_TIMEWAIT_LEN) {
						tcp_reset_keepalive_timer(sk, tmo - TCP_TIMEWAIT_LEN);
					} else if (th->fin || sk->lock.users) {
						/* Bad case. We could lose such FIN otherwise.
						 * It is not a big problem, but it looks confusing
						 * and not so rare event. We still can lose it now,
						 * if it spins in bh_lock_sock(), but it is really
						 * marginal case.
						 */
						tcp_reset_keepalive_timer(sk, tmo);
					} else {
						tcp_time_wait(sk, TCP_FIN_WAIT2, tmo);
						goto discard;
					}
				}
			}
			break;

		case TCP_CLOSING:
			if (tp->snd_una == tp->write_seq) {
				tcp_time_wait(sk, TCP_TIME_WAIT, 0);
				goto discard;
			}
			break;

		case TCP_LAST_ACK:
			if (tp->snd_una == tp->write_seq) {
				tcp_update_metrics(sk);
				tcp_done(sk);
				goto discard;
			}
			break;
		}
	} else
		goto discard;

step6:
	/* step 6: check the URG bit */
	tcp_urg(sk, th, len);

	/* step 7: process the segment text */
	switch (sk->state) {
	case TCP_CLOSE_WAIT:
	case TCP_CLOSING:
		if (!before(TCP_SKB_CB(skb)->seq, tp->fin_seq))
			break;
	case TCP_FIN_WAIT1:
	case TCP_FIN_WAIT2:
		/* RFC 793 says to queue data in these states,
		 * RFC 1122 says we MUST send a reset. 
		 * BSD 4.4 also does reset.
		 */
		if (sk->shutdown & RCV_SHUTDOWN) {
			if (TCP_SKB_CB(skb)->end_seq != TCP_SKB_CB(skb)->seq &&
			    after(TCP_SKB_CB(skb)->end_seq - th->fin, tp->rcv_nxt)) {
				NET_INC_STATS_BH(TCPAbortOnData);
				tcp_reset(sk);
				return 1;
			}
		}
		/* Fall through */
	case TCP_ESTABLISHED: 
		tcp_data(skb, sk, len);
		queued = 1;
		break;
	}

	/* tcp_data could move socket to TIME-WAIT */
	if (sk->state != TCP_CLOSE) {
		tcp_data_snd_check(sk);
		tcp_ack_snd_check(sk);
	}

	if (!queued) { 
discard:
		__kfree_skb(skb);
	}
	return 0;
}
