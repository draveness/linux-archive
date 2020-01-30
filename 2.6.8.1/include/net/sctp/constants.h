/* SCTP kernel reference Implementation
 * (C) Copyright IBM Corp. 2001, 2004
 * Copyright (c) 1999-2000 Cisco, Inc.
 * Copyright (c) 1999-2001 Motorola, Inc.
 * Copyright (c) 2001 Intel Corp.
 *
 * This file is part of the SCTP kernel reference Implementation
 *
 * The SCTP reference implementation is free software;
 * you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * The SCTP reference implementation is distributed in the hope that it
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 *                 ************************
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU CC; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Please send any bug reports or fixes you make to the
 * email address(es):
 *    lksctp developers <lksctp-developers@lists.sourceforge.net>
 *
 * Or submit a bug report through the following website:
 *    http://www.sf.net/projects/lksctp
 *
 * Written or modified by:
 *   La Monte H.P. Yarroll <piggy@acm.org>
 *   Karl Knutson          <karl@athena.chicago.il.us>
 *   Randall Stewart       <randall@stewart.chicago.il.us>
 *   Ken Morneau           <kmorneau@cisco.com>
 *   Qiaobing Xie          <qxie1@motorola.com>
 *   Xingang Guo           <xingang.guo@intel.com>
 *   Sridhar Samudrala     <samudrala@us.ibm.com>
 *   Daisy Chang           <daisyc@us.ibm.com>
 *
 * Any bugs reported given to us we will try to fix... any fixes shared will
 * be incorporated into the next SCTP release.
 */

#ifndef __sctp_constants_h__
#define __sctp_constants_h__

#include <linux/tcp.h>  /* For TCP states used in sctp_sock_state_t */
#include <linux/sctp.h>
#include <linux/ipv6.h> /* For ipv6hdr. */
#include <net/sctp/user.h>

/* Value used for stream negotiation. */
enum { SCTP_MAX_STREAM = 0xffff };
enum { SCTP_DEFAULT_OUTSTREAMS = 10 };
enum { SCTP_DEFAULT_INSTREAMS = SCTP_MAX_STREAM };

/* Since CIDs are sparse, we need all four of the following
 * symbols.  CIDs are dense through SCTP_CID_BASE_MAX.
 */
#define SCTP_CID_BASE_MAX		SCTP_CID_SHUTDOWN_COMPLETE
#define SCTP_CID_MAX			SCTP_CID_ASCONF_ACK

#define SCTP_NUM_BASE_CHUNK_TYPES	(SCTP_CID_BASE_MAX + 1)
#define SCTP_NUM_CHUNK_TYPES		(SCTP_NUM_BASE_CHUNKTYPES + 2)

#define SCTP_NUM_ADDIP_CHUNK_TYPES	2

#define SCTP_NUM_PRSCTP_CHUNK_TYPES	1

/* These are the different flavours of event.  */
typedef enum {

	SCTP_EVENT_T_CHUNK = 1,
	SCTP_EVENT_T_TIMEOUT,
	SCTP_EVENT_T_OTHER,
	SCTP_EVENT_T_PRIMITIVE

} sctp_event_t;

#define SCTP_EVENT_T_MAX SCTP_EVENT_T_PRIMITIVE
#define SCTP_EVENT_T_NUM (SCTP_EVENT_T_MAX + 1)

/* As a convenience for the state machine, we append SCTP_EVENT_* and
 * SCTP_ULP_* to the list of possible chunks.
 */

typedef enum {
	SCTP_EVENT_TIMEOUT_NONE = 0,
	SCTP_EVENT_TIMEOUT_T1_COOKIE,
	SCTP_EVENT_TIMEOUT_T1_INIT,
	SCTP_EVENT_TIMEOUT_T2_SHUTDOWN,
	SCTP_EVENT_TIMEOUT_T3_RTX,
	SCTP_EVENT_TIMEOUT_T4_RTO,
	SCTP_EVENT_TIMEOUT_T5_SHUTDOWN_GUARD,
	SCTP_EVENT_TIMEOUT_HEARTBEAT,
	SCTP_EVENT_TIMEOUT_SACK,
	SCTP_EVENT_TIMEOUT_AUTOCLOSE,
} sctp_event_timeout_t;

#define SCTP_EVENT_TIMEOUT_MAX		SCTP_EVENT_TIMEOUT_AUTOCLOSE
#define SCTP_NUM_TIMEOUT_TYPES		(SCTP_EVENT_TIMEOUT_MAX + 1)

typedef enum {
	SCTP_EVENT_NO_PENDING_TSN = 0,
} sctp_event_other_t;

#define SCTP_EVENT_OTHER_MAX		SCTP_EVENT_NO_PENDING_TSN
#define SCTP_NUM_OTHER_TYPES		(SCTP_EVENT_OTHER_MAX + 1)

/* These are primitive requests from the ULP.  */
typedef enum {
	SCTP_PRIMITIVE_ASSOCIATE = 0,
	SCTP_PRIMITIVE_SHUTDOWN,
	SCTP_PRIMITIVE_ABORT,
	SCTP_PRIMITIVE_SEND,
	SCTP_PRIMITIVE_REQUESTHEARTBEAT,
	SCTP_PRIMITIVE_ASCONF,
} sctp_event_primitive_t;

#define SCTP_EVENT_PRIMITIVE_MAX	SCTP_PRIMITIVE_ASCONF
#define SCTP_NUM_PRIMITIVE_TYPES	(SCTP_EVENT_PRIMITIVE_MAX + 1)

/* We define here a utility type for manipulating subtypes.
 * The subtype constructors all work like this:
 *
 * 	sctp_subtype_t foo = SCTP_ST_CHUNK(SCTP_CID_INIT);
 */

typedef union {
	sctp_cid_t chunk;
	sctp_event_timeout_t timeout;
	sctp_event_other_t other;
	sctp_event_primitive_t primitive;
} sctp_subtype_t;

#define SCTP_SUBTYPE_CONSTRUCTOR(_name, _type, _elt) \
static inline sctp_subtype_t	\
SCTP_ST_## _name (_type _arg)		\
{ sctp_subtype_t _retval; _retval._elt = _arg; return _retval; }

SCTP_SUBTYPE_CONSTRUCTOR(CHUNK,		sctp_cid_t,		chunk)
SCTP_SUBTYPE_CONSTRUCTOR(TIMEOUT,	sctp_event_timeout_t,	timeout)
SCTP_SUBTYPE_CONSTRUCTOR(OTHER,		sctp_event_other_t,	other)
SCTP_SUBTYPE_CONSTRUCTOR(PRIMITIVE,	sctp_event_primitive_t,	primitive)


#define sctp_chunk_is_control(a) (a->chunk_hdr->type != SCTP_CID_DATA)
#define sctp_chunk_is_data(a) (a->chunk_hdr->type == SCTP_CID_DATA)

/* Calculate the actual data size in a data chunk */
#define SCTP_DATA_SNDSIZE(c) ((int)((unsigned long)(c->chunk_end)\
		       		- (unsigned long)(c->chunk_hdr)\
				- sizeof(sctp_data_chunk_t)))

/* This is a table of printable names of sctp_param_t's.  */
extern const char *sctp_param_tbl[];


#define SCTP_MAX_ERROR_CAUSE  SCTP_ERROR_NONEXIST_IP
#define SCTP_NUM_ERROR_CAUSE  10

/* Internal error codes */
typedef enum {

	SCTP_IERROR_NO_ERROR	        = 0,
	SCTP_IERROR_BASE		= 1000,
	SCTP_IERROR_NO_COOKIE,
	SCTP_IERROR_BAD_SIG,
	SCTP_IERROR_STALE_COOKIE,
	SCTP_IERROR_NOMEM,
	SCTP_IERROR_MALFORMED,
	SCTP_IERROR_BAD_TAG,
	SCTP_IERROR_BIG_GAP,
	SCTP_IERROR_DUP_TSN,
	SCTP_IERROR_HIGH_TSN,
	SCTP_IERROR_IGNORE_TSN,
	SCTP_IERROR_NO_DATA,
	SCTP_IERROR_BAD_STREAM,

} sctp_ierror_t;



/* SCTP state defines for internal state machine */
typedef enum {

	SCTP_STATE_EMPTY		= 0,
	SCTP_STATE_CLOSED		= 1,
	SCTP_STATE_COOKIE_WAIT		= 2,
	SCTP_STATE_COOKIE_ECHOED	= 3,
	SCTP_STATE_ESTABLISHED		= 4,
	SCTP_STATE_SHUTDOWN_PENDING	= 5,
	SCTP_STATE_SHUTDOWN_SENT	= 6,
	SCTP_STATE_SHUTDOWN_RECEIVED	= 7,
	SCTP_STATE_SHUTDOWN_ACK_SENT	= 8,

} sctp_state_t;

#define SCTP_STATE_MAX			SCTP_STATE_SHUTDOWN_ACK_SENT
#define SCTP_STATE_NUM_STATES		(SCTP_STATE_MAX + 1)

/* These are values for sk->state.
 * For a UDP-style SCTP socket, the states are defined as follows
 * - A socket in SCTP_SS_CLOSED state indicates that it is not willing to
 *   accept new associations, but it can initiate the creation of new ones.
 * - A socket in SCTP_SS_LISTENING state indicates that it is willing to
 *   accept new  associations and can initiate the creation of new ones.
 * - A socket in SCTP_SS_ESTABLISHED state indicates that it is a peeled off
 *   socket with one association.
 * For a TCP-style SCTP socket, the states are defined as follows
 * - A socket in SCTP_SS_CLOSED state indicates that it is not willing to
 *   accept new associations, but it can initiate the creation of new ones.
 * - A socket in SCTP_SS_LISTENING state indicates that it is willing to
 *   accept new associations, but cannot initiate the creation of new ones.
 * - A socket in SCTP_SS_ESTABLISHED state indicates that it has a single 
 *   association.
 */
typedef enum {
	SCTP_SS_CLOSED         = TCP_CLOSE,
	SCTP_SS_LISTENING      = TCP_LISTEN,
	SCTP_SS_ESTABLISHING   = TCP_SYN_SENT,
	SCTP_SS_ESTABLISHED    = TCP_ESTABLISHED,
	SCTP_SS_DISCONNECTING  = TCP_CLOSING,
} sctp_sock_state_t;

/* These functions map various type to printable names.  */
const char *sctp_cname(const sctp_subtype_t);	/* chunk types */
const char *sctp_oname(const sctp_subtype_t);	/* other events */
const char *sctp_tname(const sctp_subtype_t);	/* timeouts */
const char *sctp_pname(const sctp_subtype_t);	/* primitives */

/* This is a table of printable names of sctp_state_t's.  */
extern const char *sctp_state_tbl[], *sctp_evttype_tbl[], *sctp_status_tbl[];

/* Maximum chunk length considering padding requirements. */
enum { SCTP_MAX_CHUNK_LEN = ((1<<16) - sizeof(__u32)) };

/* Encourage Cookie-Echo bundling by pre-fragmenting chunks a little
 * harder (until reaching ESTABLISHED state).
 */
enum { SCTP_ARBITRARY_COOKIE_ECHO_LEN = 200 };

/* Guess at how big to make the TSN mapping array.
 * We guarantee that we can handle at least this big a gap between the
 * cumulative ACK and the highest TSN.  In practice, we can often
 * handle up to twice this value.
 *
 * NEVER make this more than 32767 (2^15-1).  The Gap Ack Blocks in a
 * SACK (see  section 3.3.4) are only 16 bits, so 2*SCTP_TSN_MAP_SIZE
 * must be less than 65535 (2^16 - 1), or we will have overflow
 * problems creating SACK's.
 */
#define SCTP_TSN_MAP_SIZE 2048
#define SCTP_TSN_MAX_GAP  65535

/* We will not record more than this many duplicate TSNs between two
 * SACKs.  The minimum PMTU is 576.  Remove all the headers and there
 * is enough room for 131 duplicate reports.  Round down to the
 * nearest power of 2.
 */
enum { SCTP_MIN_PMTU = 576 };
enum { SCTP_MAX_DUP_TSNS = 16 };
enum { SCTP_MAX_GABS = 16 };

typedef enum {
	SCTP_COUNTER_INIT_ERROR,
} sctp_counter_t;

/* How many counters does an association need? */
#define SCTP_NUMBER_COUNTERS	5

/* Here we define the default timers.  */

/* cookie timer def = ? seconds */
#define SCTP_DEFAULT_TIMEOUT_T1_COOKIE	(3 * HZ)

/* init timer def = 3 seconds  */
#define SCTP_DEFAULT_TIMEOUT_T1_INIT	(3 * HZ)

/* shutdown timer def = 300 ms */
#define SCTP_DEFAULT_TIMEOUT_T2_SHUTDOWN ((300 * HZ) / 1000)

/* 0 seconds + RTO */
#define SCTP_DEFAULT_TIMEOUT_HEARTBEAT	(10 * HZ)

/* recv timer def = 200ms (in usec) */
#define SCTP_DEFAULT_TIMEOUT_SACK	((200 * HZ) / 1000)
#define SCTP_DEFAULT_TIMEOUT_SACK_MAX	((500 * HZ) / 1000) /* 500 ms */

/* RTO.Initial              - 3  seconds
 * RTO.Min                  - 1  second
 * RTO.Max                  - 60 seconds
 * RTO.Alpha                - 1/8
 * RTO.Beta                 - 1/4
 */
#define SCTP_RTO_INITIAL	(3 * HZ)
#define SCTP_RTO_MIN		(1 * HZ)
#define SCTP_RTO_MAX		(60 * HZ)

#define SCTP_RTO_ALPHA          3   /* 1/8 when converted to right shifts. */
#define SCTP_RTO_BETA           2   /* 1/4 when converted to right shifts. */

/* Maximum number of new data packets that can be sent in a burst.  */
#define SCTP_MAX_BURST		4

#define SCTP_CLOCK_GRANULARITY	1	/* 1 jiffy */

#define SCTP_DEF_MAX_INIT 6
#define SCTP_DEF_MAX_SEND 10

#define SCTP_DEFAULT_COOKIE_LIFE_SEC	60 /* seconds */
#define SCTP_DEFAULT_COOKIE_LIFE_USEC	0  /* microseconds */

#define SCTP_DEFAULT_MINWINDOW	1500	/* default minimum rwnd size */
#define SCTP_DEFAULT_MAXWINDOW	65535	/* default rwnd size */
#define SCTP_DEFAULT_MAXSEGMENT 1500	/* MTU size, this is the limit
                                         * to which we will raise the P-MTU.
					 */
#define SCTP_DEFAULT_MINSEGMENT 512	/* MTU size ... if no mtu disc */
#define SCTP_HOW_MANY_SECRETS 2		/* How many secrets I keep */
#define SCTP_HOW_LONG_COOKIE_LIVE 3600	/* How many seconds the current
					 * secret will live?
					 */
#define SCTP_SECRET_SIZE 32		/* Number of octets in a 256 bits. */

#define SCTP_SIGNATURE_SIZE 20	        /* size of a SLA-1 signature */

#define SCTP_COOKIE_MULTIPLE 32 /* Pad out our cookie to make our hash
				 * functions simpler to write.
				 */

#if defined (CONFIG_SCTP_HMAC_MD5)
#define SCTP_COOKIE_HMAC_ALG "md5"
#elif defined (CONFIG_SCTP_HMAC_SHA1)
#define SCTP_COOKIE_HMAC_ALG "sha1"
#else
#define SCTP_COOKIE_HMAC_ALG NULL
#endif

/* These return values describe the success or failure of a number of
 * routines which form the lower interface to SCTP_outqueue.
 */
typedef enum {
	SCTP_XMIT_OK,
	SCTP_XMIT_PMTU_FULL,
	SCTP_XMIT_RWND_FULL,
	SCTP_XMIT_NAGLE_DELAY,
} sctp_xmit_t;

/* These are the commands for manipulating transports.  */
typedef enum {
	SCTP_TRANSPORT_UP,
	SCTP_TRANSPORT_DOWN,
} sctp_transport_cmd_t;

/* These are the address scopes defined mainly for IPv4 addresses
 * based on draft of SCTP IPv4 scoping <draft-stewart-tsvwg-sctp-ipv4-00.txt>.
 * These scopes are hopefully generic enough to be used on scoping both
 * IPv4 and IPv6 addresses in SCTP.
 * At this point, the IPv6 scopes will be mapped to these internal scopes
 * as much as possible.
 */
typedef enum {
	SCTP_SCOPE_GLOBAL,		/* IPv4 global addresses */
	SCTP_SCOPE_PRIVATE,		/* IPv4 private addresses */
	SCTP_SCOPE_LINK,		/* IPv4 link local address */
	SCTP_SCOPE_LOOPBACK,		/* IPv4 loopback address */
	SCTP_SCOPE_UNUSABLE,		/* IPv4 unusable addresses */
} sctp_scope_t;

/* Based on IPv4 scoping <draft-stewart-tsvwg-sctp-ipv4-00.txt>,
 * SCTP IPv4 unusable addresses: 0.0.0.0/8, 224.0.0.0/4, 198.18.0.0/24,
 * 192.88.99.0/24.
 * Also, RFC 8.4, non-unicast addresses are not considered valid SCTP
 * addresses.
 */
#define IS_IPV4_UNUSABLE_ADDRESS(a) \
	((INADDR_BROADCAST == *a) || \
	(MULTICAST(*a)) || \
	(((unsigned char *)(a))[0] == 0) || \
	((((unsigned char *)(a))[0] == 198) && \
	(((unsigned char *)(a))[1] == 18) && \
	(((unsigned char *)(a))[2] == 0)) || \
	((((unsigned char *)(a))[0] == 192) && \
	(((unsigned char *)(a))[1] == 88) && \
	(((unsigned char *)(a))[2] == 99)))

/* IPv4 Link-local addresses: 169.254.0.0/16.  */
#define IS_IPV4_LINK_ADDRESS(a) \
	((((unsigned char *)(a))[0] == 169) && \
	(((unsigned char *)(a))[1] == 254))

/* RFC 1918 "Address Allocation for Private Internets" defines the IPv4
 * private address space as the following:
 *
 * 10.0.0.0 - 10.255.255.255 (10/8 prefix)
 * 172.16.0.0.0 - 172.31.255.255 (172.16/12 prefix)
 * 192.168.0.0 - 192.168.255.255 (192.168/16 prefix)
 */
#define IS_IPV4_PRIVATE_ADDRESS(a) \
	((((unsigned char *)(a))[0] == 10) || \
	((((unsigned char *)(a))[0] == 172) && \
	(((unsigned char *)(a))[1] >= 16) && \
	(((unsigned char *)(a))[1] < 32)) || \
	((((unsigned char *)(a))[0] == 192) && \
	(((unsigned char *)(a))[1] == 168)))

/* Flags used for the bind address copy functions.  */
#define SCTP_ADDR6_ALLOWED	0x00000001	/* IPv6 address is allowed by
						   local sock family */
#define SCTP_ADDR4_PEERSUPP	0x00000002	/* IPv4 address is supported by
						   peer */
#define SCTP_ADDR6_PEERSUPP	0x00000004	/* IPv6 address is supported by
						   peer */

/* Reasons to retransmit. */
typedef enum {
	SCTP_RTXR_T3_RTX,
	SCTP_RTXR_FAST_RTX,
	SCTP_RTXR_PMTUD,
} sctp_retransmit_reason_t;

/* Reasons to lower cwnd. */
typedef enum {
	SCTP_LOWER_CWND_T3_RTX,
	SCTP_LOWER_CWND_FAST_RTX,
	SCTP_LOWER_CWND_ECNE,
	SCTP_LOWER_CWND_INACTIVE,
} sctp_lower_cwnd_t;

#endif /* __sctp_constants_h__ */
