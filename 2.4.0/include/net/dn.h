#ifndef _NET_DN_H
#define _NET_DN_H

#include <linux/dn.h>
#include <asm/byteorder.h>

typedef unsigned short dn_address;

#define dn_ntohs(x) le16_to_cpu((unsigned short)(x))
#define dn_htons(x) cpu_to_le16((unsigned short)(x))

struct dn_scp                                   /* Session Control Port */
{
        unsigned char           state;
#define DN_O     1                      /* Open                 */
#define DN_CR    2                      /* Connect Receive      */
#define DN_DR    3                      /* Disconnect Reject    */
#define DN_DRC   4                      /* Discon. Rej. Complete*/
#define DN_CC    5                      /* Connect Confirm      */
#define DN_CI    6                      /* Connect Initiate     */
#define DN_NR    7                      /* No resources         */
#define DN_NC    8                      /* No communication     */
#define DN_CD    9                      /* Connect Delivery     */
#define DN_RJ    10                     /* Rejected             */
#define DN_RUN   11                     /* Running              */
#define DN_DI    12                     /* Disconnect Initiate  */
#define DN_DIC   13                     /* Disconnect Complete  */
#define DN_DN    14                     /* Disconnect Notificat */
#define DN_CL    15                     /* Closed               */
#define DN_CN    16                     /* Closed Notification  */

        unsigned short          addrloc;
        unsigned short          addrrem;
        unsigned short          numdat;
        unsigned short          numoth;
        unsigned short          numoth_rcv;
        unsigned short          numdat_rcv;
        unsigned short          ackxmt_dat;
        unsigned short          ackxmt_oth;
        unsigned short          ackrcv_dat;
        unsigned short          ackrcv_oth;
        unsigned char           flowrem_sw;
	unsigned char		flowloc_sw;
#define DN_SEND         2
#define DN_DONTSEND     1
#define DN_NOCHANGE     0
	unsigned char		accept_mode;
	unsigned short          mss;
	unsigned long		seg_size; /* Running total of current segment */

	struct optdata_dn     conndata_in;
	struct optdata_dn     conndata_out;
	struct optdata_dn     discdata_in;
	struct optdata_dn     discdata_out;
        struct accessdata_dn  accessdata;

        struct sockaddr_dn addr; /* Local address  */
	struct sockaddr_dn peer; /* Remote address */

	/*
	 * In this case the RTT estimation is not specified in the
	 * docs, nor is any back off algorithm. Here we follow well
	 * known tcp algorithms with a few small variations.
	 *
	 * snd_window: Max number of packets we send before we wait for
	 *             an ack to come back. This will become part of a
	 *             more complicated scheme when we support flow
	 *             control.
	 *
	 * nsp_srtt:   Round-Trip-Time (x8) in jiffies. This is a rolling
	 *             average.
	 * nsp_rttvar: Round-Trip-Time-Varience (x4) in jiffies. This is the
	 *             varience of the smoothed average (but calculated in
	 *             a simpler way than for normal statistical varience
	 *             calculations).
	 *
	 * nsp_rxtshift: Backoff counter. Value is zero normally, each time
	 *               a packet is lost is increases by one until an ack
	 *               is received. Its used to index an array of backoff
	 *               multipliers.
	 */
#define NSP_MIN_WINDOW 1
#define NSP_MAX_WINDOW 512
	unsigned long snd_window;
#define NSP_INITIAL_SRTT (HZ)
	unsigned long nsp_srtt;
#define NSP_INITIAL_RTTVAR (HZ*3)
	unsigned long nsp_rttvar;
#define NSP_MAXRXTSHIFT 12
	unsigned long nsp_rxtshift;

	/*
	 * Output queues, one for data, one for otherdata/linkservice
	 */
	struct sk_buff_head data_xmit_queue;
	struct sk_buff_head other_xmit_queue;

	/*
	 * Input queue for other data
	 */
	struct sk_buff_head other_receive_queue;
	int other_report;

	/*
	 * Stuff to do with the slow timer
	 */
	unsigned long stamp;          /* time of last transmit */
	unsigned long persist;
	int (*persist_fxn)(struct sock *sk);
	unsigned long keepalive;
	void (*keepalive_fxn)(struct sock *sk);

	/*
	 * This stuff is for the fast timer for delayed acks
	 */
	struct timer_list delack_timer;
	int delack_pending;
	void (*delack_fxn)(struct sock *sk);
};

/*
 * src,dst : Source and Destination DECnet addresses
 * hops : Number of hops through the network
 * dst_port, src_port : NSP port numbers
 * services, info : Useful data extracted from conninit messages
 * rt_flags : Routing flags byte
 * nsp_flags : NSP layer flags byte
 * segsize : Size of segment
 * segnum : Number, for data, otherdata and linkservice
 * xmit_count : Number of times we've transmitted this skb
 * stamp : Time stamp of first transmission, used in RTT calculations
 * iif: Input interface number
 *
 * As a general policy, this structure keeps all addresses in network
 * byte order, and all else in host byte order. Thus dst, src, dst_port
 * and src_port are in network order. All else is in host order.
 * 
 */
struct dn_skb_cb {
	unsigned short dst;
	unsigned short src;
	unsigned short hops;
	unsigned short dst_port;
	unsigned short src_port;
	unsigned char services;
	unsigned char info;
	unsigned char rt_flags;
	unsigned char nsp_flags;
	unsigned short segsize;
	unsigned short segnum;
	unsigned short xmit_count;
	unsigned long stamp;
	int iif;
};

static __inline__ dn_address dn_eth2dn(unsigned char *ethaddr)
{
	return ethaddr[4] | (ethaddr[5] << 8);
}

static __inline__ dn_address dn_saddr2dn(struct sockaddr_dn *saddr)
{
	return *(dn_address *)saddr->sdn_nodeaddr;
}

static __inline__ void dn_dn2eth(unsigned char *ethaddr, dn_address addr)
{
	ethaddr[0] = 0xAA;
	ethaddr[1] = 0x00;
	ethaddr[2] = 0x04;
	ethaddr[3] = 0x00;
	ethaddr[4] = (unsigned char)(addr & 0xff);
	ethaddr[5] = (unsigned char)(addr >> 8);
}

#define DN_MENUVER_ACC 0x01
#define DN_MENUVER_USR 0x02
#define DN_MENUVER_PRX 0x04
#define DN_MENUVER_UIC 0x08

extern struct sock *dn_sklist_find_listener(struct sockaddr_dn *addr);
extern struct sock *dn_find_by_skb(struct sk_buff *skb);
#define DN_ASCBUF_LEN 9
extern char *dn_addr2asc(dn_address, char *);
extern int dn_destroy_timer(struct sock *sk);

extern int dn_sockaddr2username(struct sockaddr_dn *addr, unsigned char *buf, unsigned char type);
extern int dn_username2sockaddr(unsigned char *data, int len, struct sockaddr_dn *addr, unsigned char *type);

extern void dn_start_slow_timer(struct sock *sk);
extern void dn_stop_slow_timer(struct sock *sk);
extern void dn_start_fast_timer(struct sock *sk);
extern void dn_stop_fast_timer(struct sock *sk);

extern dn_address decnet_address;
extern unsigned char decnet_ether_address[6];
extern int decnet_debug_level;
extern int decnet_time_wait;
extern int decnet_dn_count;
extern int decnet_di_count;
extern int decnet_dr_count;

#endif /* _NET_DN_H */
