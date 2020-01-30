#ifndef _NET_DN_NSP_H
#define _NET_DN_NSP_H
/******************************************************************************
    (c) 1995-1998 E.M. Serrat		emserrat@geocities.com
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
*******************************************************************************/
/* dn_nsp.c functions prototyping */

extern void dn_nsp_send_data_ack(struct sock *sk);
extern void dn_nsp_send_oth_ack(struct sock *sk);
extern void dn_nsp_delayed_ack(struct sock *sk);
extern void dn_send_conn_ack(struct sock *sk);
extern void dn_send_conn_conf(struct sock *sk, int gfp);
extern void dn_nsp_send_disc(struct sock *sk, unsigned char type, 
				unsigned short reason, int gfp);
extern void dn_nsp_return_disc(struct sk_buff *skb, unsigned char type,
				unsigned short reason);
extern void dn_nsp_send_lnk(struct sock *sk, unsigned short flags);
extern void dn_nsp_send_conninit(struct sock *sk, unsigned char flags);

extern void dn_nsp_output(struct sock *sk);
extern int dn_nsp_check_xmit_queue(struct sock *sk, struct sk_buff *skb, struct sk_buff_head *q, unsigned short acknum);
extern void dn_nsp_queue_xmit(struct sock *sk, struct sk_buff *skb, int oob);
extern unsigned long dn_nsp_persist(struct sock *sk);
extern int dn_nsp_xmit_timeout(struct sock *sk);

extern int dn_nsp_rx(struct sk_buff *);
extern int dn_nsp_backlog_rcv(struct sock *sk, struct sk_buff *skb);

extern struct sk_buff *dn_alloc_skb(struct sock *sk, int size, int pri);
extern struct sk_buff *dn_alloc_send_skb(struct sock *sk, int *size, int noblock, int *err);

#define NSP_REASON_OK 0		/* No error */
#define NSP_REASON_NR 1		/* No resources */
#define NSP_REASON_UN 2		/* Unrecognised node name */
#define NSP_REASON_SD 3		/* Node shutting down */
#define NSP_REASON_ID 4		/* Invalid destination end user */
#define NSP_REASON_ER 5		/* End user lacks resources */
#define NSP_REASON_OB 6		/* Object too busy */
#define NSP_REASON_US 7		/* Unspecified error */
#define NSP_REASON_TP 8		/* Third-Party abort */
#define NSP_REASON_EA 9		/* End user has aborted the link */
#define NSP_REASON_IF 10	/* Invalid node name format */
#define NSP_REASON_LS 11	/* Local node shutdown */
#define NSP_REASON_LL 32	/* Node lacks logical-link resources */
#define NSP_REASON_LE 33	/* End user lacks logical-link resources */
#define NSP_REASON_UR 34	/* Unacceptable RQSTRID or PASSWORD field */
#define NSP_REASON_UA 36	/* Unacceptable ACCOUNT field */
#define NSP_REASON_TM 38	/* End user timed out logical link */
#define NSP_REASON_NU 39	/* Node unreachable */
#define NSP_REASON_NL 41	/* No-link message */
#define NSP_REASON_DC 42	/* Disconnect confirm */
#define NSP_REASON_IO 43	/* Image data field overflow */

#define NSP_DISCINIT 0x38
#define NSP_DISCCONF 0x48

/*------------------------- NSP - messages ------------------------------*/
/* Data Messages */
/*---------------*/

/* Data Messages    (data segment/interrupt/link service)               */

struct nsp_data_seg_msg
{
	unsigned char   msgflg          __attribute__((packed));
	unsigned short  dstaddr         __attribute__((packed));
	unsigned short  srcaddr         __attribute__((packed));
};

struct nsp_data_opt_msg
{
	unsigned short  acknum          __attribute__((packed));
	unsigned short  segnum          __attribute__((packed));
	unsigned short  lsflgs          __attribute__((packed));
};

struct nsp_data_opt_msg1
{
	unsigned short  acknum          __attribute__((packed));
	unsigned short  segnum          __attribute__((packed));
};


/* Acknowledgment Message (data/other data)                             */
struct nsp_data_ack_msg
{
	unsigned char   msgflg          __attribute__((packed));
	unsigned short  dstaddr         __attribute__((packed));
	unsigned short  srcaddr         __attribute__((packed));
	unsigned short  acknum          __attribute__((packed));
};

/* Connect Acknowledgment Message */
struct  nsp_conn_ack_msg
{
	unsigned char   msgflg          __attribute__((packed));
	unsigned short  dstaddr         __attribute__((packed));
};


/* Connect Initiate/Retransmit Initiate/Connect Confirm */
struct  nsp_conn_init_msg
{
	unsigned char   msgflg          __attribute__((packed));
#define NSP_CI      0x18            /* Connect Initiate     */
#define NSP_RCI     0x68            /* Retrans. Conn Init   */
	unsigned short  dstaddr         __attribute__((packed));
        unsigned short  srcaddr         __attribute__((packed));
        unsigned char   services        __attribute__((packed));
#define NSP_FC_NONE   0x00            /* Flow Control None    */
#define NSP_FC_SRC    0x04            /* Seg Req. Count       */
#define NSP_FC_SCMC   0x08            /* Sess. Control Mess   */
	unsigned char   info            __attribute__((packed));
        unsigned short  segsize         __attribute__((packed));
};

/* Disconnect Initiate/Disconnect Confirm */
struct  nsp_disconn_init_msg
{
	unsigned char   msgflg          __attribute__((packed));
        unsigned short  dstaddr         __attribute__((packed));
        unsigned short  srcaddr         __attribute__((packed));
        unsigned short  reason          __attribute__((packed));
};



struct  srcobj_fmt
{
	char            format          __attribute__((packed));
        unsigned char   task            __attribute__((packed));
        unsigned short  grpcode         __attribute__((packed));
        unsigned short  usrcode         __attribute__((packed));
        char            dlen            __attribute__((packed));
};

/*
 * A collection of functions for manipulating the sequence
 * numbers used in NSP. Similar in operation to the functions
 * of the same name in TCP.
 */
static __inline__ int before(unsigned short seq1, unsigned short seq2)
{
        seq1 &= 0x0fff;
        seq2 &= 0x0fff;

        return (int)((seq1 - seq2) & 0x0fff) > 2048;
}


static __inline__ int after(unsigned short seq1, unsigned short seq2)
{
        seq1 &= 0x0fff;
        seq2 &= 0x0fff;

        return (int)((seq2 - seq1) & 0x0fff) > 2048;
}

static __inline__ int equal(unsigned short seq1, unsigned short seq2)
{
        return ((seq1 ^ seq2) & 0x0fff) == 0;
}

static __inline__ int before_or_equal(unsigned short seq1, unsigned short seq2)
{
	return (before(seq1, seq2) || equal(seq1, seq2));
}

static __inline__ void seq_add(unsigned short *seq, unsigned short off)
{
        *seq += off;
        *seq &= 0x0fff;
}

static __inline__ int seq_next(unsigned short seq1, unsigned short seq2)
{
	return (((seq2&0x0fff) - (seq1&0x0fff)) == 1);
}

/*
 * Can we delay the ack ?
 */
static __inline__ int sendack(unsigned short seq)
{
        return (int)((seq & 0x1000) ? 0 : 1);
}

/*
 * Is socket congested ?
 */
static __inline__ int dn_congested(struct sock *sk)
{
        return atomic_read(&sk->rmem_alloc) > (sk->rcvbuf >> 1);
}

#endif /* _NET_DN_NSP_H */
