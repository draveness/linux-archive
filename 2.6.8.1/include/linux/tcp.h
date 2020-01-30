/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the TCP protocol.
 *
 * Version:	@(#)tcp.h	1.0.2	04/28/93
 *
 * Author:	Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _LINUX_TCP_H
#define _LINUX_TCP_H

#include <linux/types.h>
#include <asm/byteorder.h>

struct tcphdr {
	__u16	source;
	__u16	dest;
	__u32	seq;
	__u32	ack_seq;
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u16	res1:4,
		doff:4,
		fin:1,
		syn:1,
		rst:1,
		psh:1,
		ack:1,
		urg:1,
		ece:1,
		cwr:1;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__u16	doff:4,
		res1:4,
		cwr:1,
		ece:1,
		urg:1,
		ack:1,
		psh:1,
		rst:1,
		syn:1,
		fin:1;
#else
#error	"Adjust your <asm/byteorder.h> defines"
#endif	
	__u16	window;
	__u16	check;
	__u16	urg_ptr;
};


enum {
  TCP_ESTABLISHED = 1,
  TCP_SYN_SENT,
  TCP_SYN_RECV,
  TCP_FIN_WAIT1,
  TCP_FIN_WAIT2,
  TCP_TIME_WAIT,
  TCP_CLOSE,
  TCP_CLOSE_WAIT,
  TCP_LAST_ACK,
  TCP_LISTEN,
  TCP_CLOSING,	 /* now a valid state */

  TCP_MAX_STATES /* Leave at the end! */
};

#define TCP_STATE_MASK	0xF
#define TCP_ACTION_FIN	(1 << 7)

enum {
  TCPF_ESTABLISHED = (1 << 1),
  TCPF_SYN_SENT  = (1 << 2),
  TCPF_SYN_RECV  = (1 << 3),
  TCPF_FIN_WAIT1 = (1 << 4),
  TCPF_FIN_WAIT2 = (1 << 5),
  TCPF_TIME_WAIT = (1 << 6),
  TCPF_CLOSE     = (1 << 7),
  TCPF_CLOSE_WAIT = (1 << 8),
  TCPF_LAST_ACK  = (1 << 9),
  TCPF_LISTEN    = (1 << 10),
  TCPF_CLOSING   = (1 << 11) 
};

/*
 *	The union cast uses a gcc extension to avoid aliasing problems
 *  (union is compatible to any of its members)
 *  This means this part of the code is -fstrict-aliasing safe now.
 */
union tcp_word_hdr { 
	struct tcphdr hdr;
	__u32 		  words[5];
}; 

#define tcp_flag_word(tp) ( ((union tcp_word_hdr *)(tp))->words [3]) 

enum { 
	TCP_FLAG_CWR = __constant_htonl(0x00800000), 
	TCP_FLAG_ECE = __constant_htonl(0x00400000), 
	TCP_FLAG_URG = __constant_htonl(0x00200000), 
	TCP_FLAG_ACK = __constant_htonl(0x00100000), 
	TCP_FLAG_PSH = __constant_htonl(0x00080000), 
	TCP_FLAG_RST = __constant_htonl(0x00040000), 
	TCP_FLAG_SYN = __constant_htonl(0x00020000), 
	TCP_FLAG_FIN = __constant_htonl(0x00010000),
	TCP_RESERVED_BITS = __constant_htonl(0x0F000000),
	TCP_DATA_OFFSET = __constant_htonl(0xF0000000)
}; 

/* TCP socket options */
#define TCP_NODELAY		1	/* Turn off Nagle's algorithm. */
#define TCP_MAXSEG		2	/* Limit MSS */
#define TCP_CORK		3	/* Never send partially complete segments */
#define TCP_KEEPIDLE		4	/* Start keeplives after this period */
#define TCP_KEEPINTVL		5	/* Interval between keepalives */
#define TCP_KEEPCNT		6	/* Number of keepalives before death */
#define TCP_SYNCNT		7	/* Number of SYN retransmits */
#define TCP_LINGER2		8	/* Life time of orphaned FIN-WAIT-2 state */
#define TCP_DEFER_ACCEPT	9	/* Wake up listener only when data arrive */
#define TCP_WINDOW_CLAMP	10	/* Bound advertised window */
#define TCP_INFO		11	/* Information about this connection. */
#define TCP_QUICKACK		12	/* Block/reenable quick acks */

#define TCPI_OPT_TIMESTAMPS	1
#define TCPI_OPT_SACK		2
#define TCPI_OPT_WSCALE		4
#define TCPI_OPT_ECN		8

enum tcp_ca_state
{
	TCP_CA_Open = 0,
#define TCPF_CA_Open	(1<<TCP_CA_Open)
	TCP_CA_Disorder = 1,
#define TCPF_CA_Disorder (1<<TCP_CA_Disorder)
	TCP_CA_CWR = 2,
#define TCPF_CA_CWR	(1<<TCP_CA_CWR)
	TCP_CA_Recovery = 3,
#define TCPF_CA_Recovery (1<<TCP_CA_Recovery)
	TCP_CA_Loss = 4
#define TCPF_CA_Loss	(1<<TCP_CA_Loss)
};

struct tcp_info
{
	__u8	tcpi_state;
	__u8	tcpi_ca_state;
	__u8	tcpi_retransmits;
	__u8	tcpi_probes;
	__u8	tcpi_backoff;
	__u8	tcpi_options;
	__u8	tcpi_snd_wscale : 4, tcpi_rcv_wscale : 4;

	__u32	tcpi_rto;
	__u32	tcpi_ato;
	__u32	tcpi_snd_mss;
	__u32	tcpi_rcv_mss;

	__u32	tcpi_unacked;
	__u32	tcpi_sacked;
	__u32	tcpi_lost;
	__u32	tcpi_retrans;
	__u32	tcpi_fackets;

	/* Times. */
	__u32	tcpi_last_data_sent;
	__u32	tcpi_last_ack_sent;     /* Not remembered, sorry. */
	__u32	tcpi_last_data_recv;
	__u32	tcpi_last_ack_recv;

	/* Metrics. */
	__u32	tcpi_pmtu;
	__u32	tcpi_rcv_ssthresh;
	__u32	tcpi_rtt;
	__u32	tcpi_rttvar;
	__u32	tcpi_snd_ssthresh;
	__u32	tcpi_snd_cwnd;
	__u32	tcpi_advmss;
	__u32	tcpi_reordering;

	__u32	tcpi_rcv_rtt;
	__u32	tcpi_rcv_space;
};

#ifdef __KERNEL__

#include <linux/config.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <net/sock.h>

/* This defines a selective acknowledgement block. */
struct tcp_sack_block {
	__u32	start_seq;
	__u32	end_seq;
};

struct tcp_opt {
	int	tcp_header_len;	/* Bytes of tcp header to send		*/

/*
 *	Header prediction flags
 *	0x5?10 << 16 + snd_wnd in net byte order
 */
	__u32	pred_flags;

/*
 *	RFC793 variables by their proper names. This means you can
 *	read the code and the spec side by side (and laugh ...)
 *	See RFC793 and RFC1122. The RFC writes these in capitals.
 */
 	__u32	rcv_nxt;	/* What we want to receive next 	*/
 	__u32	snd_nxt;	/* Next sequence we send		*/

 	__u32	snd_una;	/* First byte we want an ack for	*/
 	__u32	snd_sml;	/* Last byte of the most recently transmitted small packet */
	__u32	rcv_tstamp;	/* timestamp of last received ACK (for keepalives) */
	__u32	lsndtime;	/* timestamp of last sent data packet (for restart window) */
	struct tcp_bind_bucket *bind_hash;
	/* Delayed ACK control data */
	struct {
		__u8	pending;	/* ACK is pending */
		__u8	quick;		/* Scheduled number of quick acks	*/
		__u8	pingpong;	/* The session is interactive		*/
		__u8	blocked;	/* Delayed ACK was blocked by socket lock*/
		__u32	ato;		/* Predicted tick of soft clock		*/
		unsigned long timeout;	/* Currently scheduled timeout		*/
		__u32	lrcvtime;	/* timestamp of last received data packet*/
		__u16	last_seg_size;	/* Size of last incoming segment	*/
		__u16	rcv_mss;	/* MSS used for delayed ACK decisions	*/ 
	} ack;

	/* Data for direct copy to user */
	struct {
		struct sk_buff_head	prequeue;
		struct task_struct	*task;
		struct iovec		*iov;
		int			memory;
		int			len;
	} ucopy;

	__u32	snd_wl1;	/* Sequence for window update		*/
	__u32	snd_wnd;	/* The window we expect to receive	*/
	__u32	max_window;	/* Maximal window ever seen from peer	*/
	__u32	pmtu_cookie;	/* Last pmtu seen by socket		*/
	__u32	mss_cache;	/* Cached effective mss, not including SACKS */
	__u16	mss_cache_std;	/* Like mss_cache, but without TSO */
	__u16	mss_clamp;	/* Maximal mss, negotiated at connection setup */
	__u16	ext_header_len;	/* Network protocol overhead (IP/IPv6 options) */
	__u16	ext2_header_len;/* Options depending on route */
	__u8	ca_state;	/* State of fast-retransmit machine 	*/
	__u8	retransmits;	/* Number of unrecovered RTO timeouts.	*/

	__u8	reordering;	/* Packet reordering metric.		*/
	__u8	frto_counter;	/* Number of new acks after RTO */
	__u32	frto_highmark;	/* snd_nxt when RTO occurred */

	__u8	unused_pad;
	__u8	defer_accept;	/* User waits for some data after accept() */
	/* one byte hole, try to pack */

/* RTT measurement */
	__u8	backoff;	/* backoff				*/
	__u32	srtt;		/* smothed round trip time << 3		*/
	__u32	mdev;		/* medium deviation			*/
	__u32	mdev_max;	/* maximal mdev for the last rtt period	*/
	__u32	rttvar;		/* smoothed mdev_max			*/
	__u32	rtt_seq;	/* sequence number to update rttvar	*/
	__u32	rto;		/* retransmit timeout			*/

	__u32	packets_out;	/* Packets which are "in flight"	*/
	__u32	left_out;	/* Packets which leaved network		*/
	__u32	retrans_out;	/* Retransmitted packets out		*/


/*
 *	Slow start and congestion control (see also Nagle, and Karn & Partridge)
 */
 	__u32	snd_ssthresh;	/* Slow start size threshold		*/
 	__u32	snd_cwnd;	/* Sending congestion window		*/
 	__u16	snd_cwnd_cnt;	/* Linear increase counter		*/
	__u16	snd_cwnd_clamp; /* Do not allow snd_cwnd to grow above this */
	__u32	snd_cwnd_used;
	__u32	snd_cwnd_stamp;

	/* Two commonly used timers in both sender and receiver paths. */
	unsigned long		timeout;
 	struct timer_list	retransmit_timer;	/* Resend (no ack)	*/
 	struct timer_list	delack_timer;		/* Ack delay 		*/

	struct sk_buff_head	out_of_order_queue; /* Out of order segments go here */

	struct tcp_func		*af_specific;	/* Operations which are AF_INET{4,6} specific	*/

 	__u32	rcv_wnd;	/* Current receiver window		*/
	__u32	rcv_wup;	/* rcv_nxt on last window update sent	*/
	__u32	write_seq;	/* Tail(+1) of data held in tcp send buffer */
	__u32	pushed_seq;	/* Last pushed seq, required to talk to windows */
	__u32	copied_seq;	/* Head of yet unread data		*/
/*
 *      Options received (usually on last packet, some only on SYN packets).
 */
	char	tstamp_ok,	/* TIMESTAMP seen on SYN packet		*/
		wscale_ok,	/* Wscale seen on SYN packet		*/
		sack_ok;	/* SACK seen on SYN packet		*/
	char	saw_tstamp;	/* Saw TIMESTAMP on last packet		*/
        __u8	snd_wscale;	/* Window scaling received from sender	*/
        __u8	rcv_wscale;	/* Window scaling to send to receiver	*/
	__u8	nonagle;	/* Disable Nagle algorithm?             */
	__u8	keepalive_probes; /* num of allowed keep alive probes	*/

/*	PAWS/RTTM data	*/
        __u32	rcv_tsval;	/* Time stamp value             	*/
        __u32	rcv_tsecr;	/* Time stamp echo reply        	*/
        __u32	ts_recent;	/* Time stamp to echo next		*/
        long	ts_recent_stamp;/* Time we stored ts_recent (for aging) */

/*	SACKs data	*/
	__u16	user_mss;  	/* mss requested by user in ioctl */
	__u8	dsack;		/* D-SACK is scheduled			*/
	__u8	eff_sacks;	/* Size of SACK array to send with next packet */
	struct tcp_sack_block duplicate_sack[1]; /* D-SACK block */
	struct tcp_sack_block selective_acks[4]; /* The SACKS themselves*/

	__u32	window_clamp;	/* Maximal window to advertise		*/
	__u32	rcv_ssthresh;	/* Current window clamp			*/
	__u8	probes_out;	/* unanswered 0 window probes		*/
	__u8	num_sacks;	/* Number of SACK blocks		*/
	__u16	advmss;		/* Advertised MSS			*/

	__u8	syn_retries;	/* num of allowed syn retries */
	__u8	ecn_flags;	/* ECN status bits.			*/
	__u16	prior_ssthresh; /* ssthresh saved at recovery start	*/
	__u32	lost_out;	/* Lost packets				*/
	__u32	sacked_out;	/* SACK'd packets			*/
	__u32	fackets_out;	/* FACK'd packets			*/
	__u32	high_seq;	/* snd_nxt at onset of congestion	*/

	__u32	retrans_stamp;	/* Timestamp of the last retransmit,
				 * also used in SYN-SENT to remember stamp of
				 * the first SYN. */
	__u32	undo_marker;	/* tracking retrans started here. */
	int	undo_retrans;	/* number of undoable retransmissions. */
	__u32	urg_seq;	/* Seq of received urgent pointer */
	__u16	urg_data;	/* Saved octet of OOB data and control flags */
	__u8	pending;	/* Scheduled timer event	*/
	__u8	urg_mode;	/* In urgent mode		*/
	__u32	snd_up;		/* Urgent pointer		*/

	/* The syn_wait_lock is necessary only to avoid proc interface having
	 * to grab the main lock sock while browsing the listening hash
	 * (otherwise it's deadlock prone).
	 * This lock is acquired in read mode only from listening_get_next()
	 * and it's acquired in write mode _only_ from code that is actively
	 * changing the syn_wait_queue. All readers that are holding
	 * the master sock lock don't need to grab this lock in read mode
	 * too as the syn_wait_queue writes are always protected from
	 * the main sock lock.
	 */
	rwlock_t		syn_wait_lock;
	struct tcp_listen_opt	*listen_opt;

	/* FIFO of established children */
	struct open_request	*accept_queue;
	struct open_request	*accept_queue_tail;

	unsigned int		keepalive_time;	  /* time before keep alive takes place */
	unsigned int		keepalive_intvl;  /* time interval between keep alive probes */
	int			linger2;

	unsigned long last_synq_overflow; 

/* Receiver side RTT estimation */
	struct {
		__u32	rtt;
		__u32	seq;
		__u32	time;
	} rcv_rtt_est;

/* Receiver queue space */
	struct {
		int	space;
		__u32	seq;
		__u32	time;
	} rcvq_space;

/* TCP Westwood structure */
        struct {
                __u32    bw_ns_est;        /* first bandwidth estimation..not too smoothed 8) */
                __u32    bw_est;           /* bandwidth estimate */
                __u32    rtt_win_sx;       /* here starts a new evaluation... */
                __u32    bk;
                __u32    snd_una;          /* used for evaluating the number of acked bytes */
                __u32    cumul_ack;
                __u32    accounted;
                __u32    rtt;
                __u32    rtt_min;          /* minimum observed RTT */
        } westwood;

/* Vegas variables */
	struct {
		__u32	beg_snd_nxt;	/* right edge during last RTT */
		__u32	beg_snd_una;	/* left edge  during last RTT */
		__u32	beg_snd_cwnd;	/* saves the size of the cwnd */
		__u8	do_vegas;	/* do vegas for this connection */
		__u8	doing_vegas_now;/* if true, do vegas for this RTT */
		__u16	cntRTT;		/* # of RTTs measured within last RTT */
		__u32	minRTT;		/* min of RTTs measured within last RTT (in usec) */
		__u32	baseRTT;	/* the min of all Vegas RTT measurements seen (in usec) */
	} vegas;

	/* BI TCP Parameters */
	struct {
		__u32	cnt;		/* increase cwnd by 1 after this number of ACKs */
		__u32 	last_max_cwnd;	/* last maximium snd_cwnd */
		__u32	last_cwnd;	/* the last snd_cwnd */
		__u32   last_stamp;     /* time when updated last_cwnd */
	} bictcp;
};

/* WARNING: don't change the layout of the members in tcp_sock! */
struct tcp_sock {
	struct sock	  sk;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	struct ipv6_pinfo *pinet6;
#endif
	struct inet_opt	  inet;
	struct tcp_opt	  tcp;
};

static inline struct tcp_opt * tcp_sk(const struct sock *__sk)
{
	return &((struct tcp_sock *)__sk)->tcp;
}

#endif

#endif	/* _LINUX_TCP_H */
