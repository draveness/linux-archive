/* include/net/dsfield.h - Manipulation of the Differentiated Services field */

/* Written 1998-2000 by Werner Almesberger, EPFL ICA */


#ifndef __NET_DSFIELD_H
#define __NET_DSFIELD_H

#include <linux/types.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <asm/byteorder.h>


static inline __u8 ipv4_get_dsfield(struct iphdr *iph)
{
	return iph->tos;
}


static inline __u8 ipv6_get_dsfield(struct ipv6hdr *ipv6h)
{
	return ntohs(*(__u16 *) ipv6h) >> 4;
}


static inline void ipv4_change_dsfield(struct iphdr *iph,__u8 mask,
    __u8 value)
{
        __u32 check = ntohs(iph->check);
	__u8 dsfield;

	dsfield = (iph->tos & mask) | value;
	check += iph->tos;
	if ((check+1) >> 16) check = (check+1) & 0xffff;
	check -= dsfield;
	check += check >> 16; /* adjust carry */
	iph->check = htons(check);
	iph->tos = dsfield;
}


static inline void ipv6_change_dsfield(struct ipv6hdr *ipv6h,__u8 mask,
    __u8 value)
{
        __u16 tmp;

	tmp = ntohs(*(__u16 *) ipv6h);
	tmp = (tmp & ((mask << 4) | 0xf00f)) | (value << 4);
	*(__u16 *) ipv6h = htons(tmp);
}


#if 0 /* put this later into asm-i386 or such ... */

static inline void ip_change_dsfield(struct iphdr *iph,__u16 dsfield)
{
	__u16 check;

	__asm__ __volatile__("
		movw	10(%1),%0
		xchg	%b0,%h0
		addb	1(%1),%b0
		adcb	$0,%h0
		adcw	$1,%0
		cmc
		sbbw	%2,%0
		sbbw	$0,%0
		movb	%b2,1(%1)
		xchg	%b0,%h0
		movw	%0,10(%1)"
	    : "=&r" (check)
	    : "r" (iph), "r" (dsfield)
	    : "cc");
}

#endif

#endif
