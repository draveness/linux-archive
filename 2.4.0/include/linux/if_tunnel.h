#ifndef _IF_TUNNEL_H_
#define _IF_TUNNEL_H_

#define SIOCGETTUNNEL   (SIOCDEVPRIVATE + 0)
#define SIOCADDTUNNEL   (SIOCDEVPRIVATE + 1)
#define SIOCDELTUNNEL   (SIOCDEVPRIVATE + 2)
#define SIOCCHGTUNNEL   (SIOCDEVPRIVATE + 3)

#define GRE_CSUM	__constant_htons(0x8000)
#define GRE_ROUTING	__constant_htons(0x4000)
#define GRE_KEY		__constant_htons(0x2000)
#define GRE_SEQ		__constant_htons(0x1000)
#define GRE_STRICT	__constant_htons(0x0800)
#define GRE_REC		__constant_htons(0x0700)
#define GRE_FLAGS	__constant_htons(0x00F8)
#define GRE_VERSION	__constant_htons(0x0007)

struct ip_tunnel_parm
{
	char			name[IFNAMSIZ];
	int			link;
	__u16			i_flags;
	__u16			o_flags;
	__u32			i_key;
	__u32			o_key;
	struct iphdr		iph;
};

#endif /* _IF_TUNNEL_H_ */
