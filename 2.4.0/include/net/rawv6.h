#ifndef _NET_RAWV6_H
#define _NET_RAWV6_H

#ifdef __KERNEL__

#define RAWV6_HTABLE_SIZE	MAX_INET_PROTOS
extern struct sock *raw_v6_htable[RAWV6_HTABLE_SIZE];
extern rwlock_t raw_v6_lock;

extern struct sock * ipv6_raw_deliver(struct sk_buff *skb,
				      int nexthdr, unsigned long len);


extern struct sock *__raw_v6_lookup(struct sock *sk, unsigned short num,
				    struct in6_addr *loc_addr, struct in6_addr *rmt_addr);

extern int			rawv6_rcv(struct sock *sk,
					  struct sk_buff *skb, 
					  unsigned long len);


extern void			rawv6_err(struct sock *sk,
					  struct sk_buff *skb,
					  struct ipv6hdr *hdr,
					  struct inet6_skb_parm *opt,
					  int type, int code, 
					  unsigned char *buff, u32 info);

#endif

#endif
