#ifndef __LINUX_MROUTE_H
#define __LINUX_MROUTE_H

#include <linux/sockios.h>
#include <linux/in.h>

/*
 *	Based on the MROUTING 3.5 defines primarily to keep
 *	source compatibility with BSD.
 *
 *	See the mrouted code for the original history.
 *
 *      Protocol Independent Multicast (PIM) data structures included
 *      Carlos Picoto (cap@di.fc.ul.pt)
 *
 */

#define MRT_BASE	200
#define MRT_INIT	(MRT_BASE)	/* Activate the kernel mroute code 	*/
#define MRT_DONE	(MRT_BASE+1)	/* Shutdown the kernel mroute		*/
#define MRT_ADD_VIF	(MRT_BASE+2)	/* Add a virtual interface		*/
#define MRT_DEL_VIF	(MRT_BASE+3)	/* Delete a virtual interface		*/
#define MRT_ADD_MFC	(MRT_BASE+4)	/* Add a multicast forwarding entry	*/
#define MRT_DEL_MFC	(MRT_BASE+5)	/* Delete a multicast forwarding entry	*/
#define MRT_VERSION	(MRT_BASE+6)	/* Get the kernel multicast version	*/
#define MRT_ASSERT	(MRT_BASE+7)	/* Activate PIM assert mode		*/
#define MRT_PIM		(MRT_BASE+8)	/* enable PIM code	*/

#define SIOCGETVIFCNT	SIOCPROTOPRIVATE	/* IP protocol privates */
#define SIOCGETSGCNT	(SIOCPROTOPRIVATE+1)
#define SIOCGETRPF	(SIOCPROTOPRIVATE+2)

#define MAXVIFS		32	
typedef unsigned long vifbitmap_t;	/* User mode code depends on this lot */
typedef unsigned short vifi_t;
#define ALL_VIFS	((vifi_t)(-1))

/*
 *	Same idea as select
 */
 
#define VIFM_SET(n,m)	((m)|=(1<<(n)))
#define VIFM_CLR(n,m)	((m)&=~(1<<(n)))
#define VIFM_ISSET(n,m)	((m)&(1<<(n)))
#define VIFM_CLRALL(m)	((m)=0)
#define VIFM_COPY(mfrom,mto)	((mto)=(mfrom))
#define VIFM_SAME(m1,m2)	((m1)==(m2))

/*
 *	Passed by mrouted for an MRT_ADD_VIF - again we use the
 *	mrouted 3.6 structures for compatibility
 */
 
struct vifctl {
	vifi_t	vifc_vifi;		/* Index of VIF */
	unsigned char vifc_flags;	/* VIFF_ flags */
	unsigned char vifc_threshold;	/* ttl limit */
	unsigned int vifc_rate_limit;	/* Rate limiter values (NI) */
	struct in_addr vifc_lcl_addr;	/* Our address */
	struct in_addr vifc_rmt_addr;	/* IPIP tunnel addr */
};

#define VIFF_TUNNEL	0x1	/* IPIP tunnel */
#define VIFF_SRCRT	0x2	/* NI */
#define VIFF_REGISTER	0x4	/* register vif	*/

/*
 *	Cache manipulation structures for mrouted and PIMd
 */
 
struct mfcctl
{
	struct in_addr mfcc_origin;		/* Origin of mcast	*/
	struct in_addr mfcc_mcastgrp;		/* Group in question	*/
	vifi_t	mfcc_parent;			/* Where it arrived	*/
	unsigned char mfcc_ttls[MAXVIFS];	/* Where it is going	*/
	unsigned int mfcc_pkt_cnt;		/* pkt count for src-grp */
	unsigned int mfcc_byte_cnt;
	unsigned int mfcc_wrong_if;
	int	     mfcc_expire;
};

/* 
 *	Group count retrieval for mrouted
 */
 
struct sioc_sg_req
{
	struct in_addr src;
	struct in_addr grp;
	unsigned long pktcnt;
	unsigned long bytecnt;
	unsigned long wrong_if;
};

/*
 *	To get vif packet counts
 */

struct sioc_vif_req
{
	vifi_t	vifi;		/* Which iface */
	unsigned long icount;	/* In packets */
	unsigned long ocount;	/* Out packets */
	unsigned long ibytes;	/* In bytes */
	unsigned long obytes;	/* Out bytes */
};

/*
 *	This is the format the mroute daemon expects to see IGMP control
 *	data. Magically happens to be like an IP packet as per the original
 */
 
struct igmpmsg
{
	__u32 unused1,unused2;
	unsigned char im_msgtype;		/* What is this */
	unsigned char im_mbz;			/* Must be zero */
	unsigned char im_vif;			/* Interface (this ought to be a vifi_t!) */
	unsigned char unused3;
	struct in_addr im_src,im_dst;
};

/*
 *	That's all usermode folks
 */

#ifdef __KERNEL__
#include <net/sock.h>

extern int ip_mroute_setsockopt(struct sock *, int, char __user *, int);
extern int ip_mroute_getsockopt(struct sock *, int, char __user *, int __user *);
extern int ipmr_ioctl(struct sock *sk, int cmd, void __user *arg);
extern void ip_mr_init(void);


struct vif_device
{
	struct net_device 	*dev;			/* Device we are using */
	unsigned long	bytes_in,bytes_out;
	unsigned long	pkt_in,pkt_out;		/* Statistics 			*/
	unsigned long	rate_limit;		/* Traffic shaping (NI) 	*/
	unsigned char	threshold;		/* TTL threshold 		*/
	unsigned short	flags;			/* Control flags 		*/
	__be32		local,remote;		/* Addresses(remote for tunnels)*/
	int		link;			/* Physical interface index	*/
};

#define VIFF_STATIC 0x8000

struct mfc_cache 
{
	struct mfc_cache *next;			/* Next entry on cache line 	*/
	__be32 mfc_mcastgrp;			/* Group the entry belongs to 	*/
	__be32 mfc_origin;			/* Source of packet 		*/
	vifi_t mfc_parent;			/* Source interface		*/
	int mfc_flags;				/* Flags on line		*/

	union {
		struct {
			unsigned long expires;
			struct sk_buff_head unresolved;	/* Unresolved buffers		*/
		} unres;
		struct {
			unsigned long last_assert;
			int minvif;
			int maxvif;
			unsigned long bytes;
			unsigned long pkt;
			unsigned long wrong_if;
			unsigned char ttls[MAXVIFS];	/* TTL thresholds		*/
		} res;
	} mfc_un;
};

#define MFC_STATIC		1
#define MFC_NOTIFY		2

#define MFC_LINES		64

#ifdef __BIG_ENDIAN
#define MFC_HASH(a,b)	(((((__force u32)(__be32)a)>>24)^(((__force u32)(__be32)b)>>26))&(MFC_LINES-1))
#else
#define MFC_HASH(a,b)	((((__force u32)(__be32)a)^(((__force u32)(__be32)b)>>2))&(MFC_LINES-1))
#endif		

#endif


#define MFC_ASSERT_THRESH (3*HZ)		/* Maximal freq. of asserts */

/*
 *	Pseudo messages used by mrouted
 */

#define IGMPMSG_NOCACHE		1		/* Kern cache fill request to mrouted */
#define IGMPMSG_WRONGVIF	2		/* For PIM assert processing (unused) */
#define IGMPMSG_WHOLEPKT	3		/* For PIM Register processing */

#ifdef __KERNEL__

#define PIM_V1_VERSION		__constant_htonl(0x10000000)
#define PIM_V1_REGISTER		1

#define PIM_VERSION		2
#define PIM_REGISTER		1

#define PIM_NULL_REGISTER	__constant_htonl(0x40000000)

/* PIMv2 register message header layout (ietf-draft-idmr-pimvsm-v2-00.ps */

struct pimreghdr
{
	__u8	type;
	__u8	reserved;
	__be16	csum;
	__be32	flags;
};

extern int pim_rcv_v1(struct sk_buff *);

struct rtmsg;
extern int ipmr_get_route(struct sk_buff *skb, struct rtmsg *rtm, int nowait);
#endif

#endif
