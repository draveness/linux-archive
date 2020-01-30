#ifndef __ASM_SH_CHECKSUM_H
#define __ASM_SH_CHECKSUM_H

/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1999 by Kaz Kojima & Niibe Yutaka
 */

#include <linux/config.h>

/*
 * computes the checksum of a memory block at buff, length len,
 * and adds in "sum" (32-bit)
 *
 * returns a 32-bit number suitable for feeding into itself
 * or csum_tcpudp_magic
 *
 * this function must be called with even lengths, except
 * for the last fragment, which may be odd
 *
 * it's best to have buff aligned on a 32-bit boundary
 */
asmlinkage unsigned int csum_partial(const unsigned char * buff, int len, unsigned int sum);

/*
 * the same as csum_partial, but copies from src while it
 * checksums, and handles user-space pointer exceptions correctly, when needed.
 *
 * here even more important to align src and dst on a 32-bit (or even
 * better 64-bit) boundary
 */

asmlinkage unsigned int csum_partial_copy_generic( const char *src, char *dst, int len, int sum,
						   int *src_err_ptr, int *dst_err_ptr);

/*
 *	Note: when you get a NULL pointer exception here this means someone
 *	passed in an incorrect kernel address to one of these functions. 
 *	
 *	If you use these functions directly please don't forget the 
 *	verify_area().
 */
static __inline__
unsigned int csum_partial_copy_nocheck ( const char *src, char *dst,
					int len, int sum)
{
	return csum_partial_copy_generic ( src, dst, len, sum, NULL, NULL);
}

static __inline__
unsigned int csum_partial_copy_from_user ( const char *src, char *dst,
						int len, int sum, int *err_ptr)
{
	return csum_partial_copy_generic ( src, dst, len, sum, err_ptr, NULL);
}

/*
 *	Fold a partial checksum
 */

static __inline__ unsigned int csum_fold(unsigned int sum)
{
	unsigned int __dummy;
	__asm__("swap.w %0, %1\n\t"
		"extu.w	%0, %0\n\t"
		"extu.w	%1, %1\n\t"
		"add	%1, %0\n\t"
		"swap.w	%0, %1\n\t"
		"add	%1, %0\n\t"
		"not	%0, %0\n\t"
		: "=r" (sum), "=&r" (__dummy)
		: "0" (sum)
		: "t");
	return sum;
}

/*
 *	This is a version of ip_compute_csum() optimized for IP headers,
 *	which always checksum on 4 octet boundaries.
 *
 *      i386 version by Jorge Cwik <jorge@laser.satlink.net>, adapted
 *      for linux by * Arnt Gulbrandsen.
 */
static __inline__ unsigned short ip_fast_csum(unsigned char * iph, unsigned int ihl)
{
	unsigned int sum, __dummy0, __dummy1;

	__asm__ __volatile__(
		"mov.l	@%1+, %0\n\t"
		"mov.l	@%1+, %3\n\t"
		"add	#-2, %2\n\t"
		"clrt\n\t"
		"1:\t"
		"addc	%3, %0\n\t"
		"movt	%4\n\t"
		"mov.l	@%1+, %3\n\t"
		"dt	%2\n\t"
		"bf/s	1b\n\t"
		" cmp/eq #1, %4\n\t"
		"addc	%3, %0\n\t"
		"addc	%2, %0"	    /* Here %2 is 0, add carry-bit */
	/* Since the input registers which are loaded with iph and ihl
	   are modified, we must also specify them as outputs, or gcc
	   will assume they contain their original values. */
	: "=r" (sum), "=r" (iph), "=r" (ihl), "=&r" (__dummy0), "=&z" (__dummy1)
	: "1" (iph), "2" (ihl)
	: "t");

	return	csum_fold(sum);
}

static __inline__ unsigned long csum_tcpudp_nofold(unsigned long saddr,
						   unsigned long daddr,
						   unsigned short len,
						   unsigned short proto,
						   unsigned int sum) 
{
#ifdef __LITTLE_ENDIAN__
	unsigned long len_proto = (ntohs(len)<<16)+proto*256;
#else
	unsigned long len_proto = (proto<<16)+len;
#endif
	__asm__("clrt\n\t"
		"addc	%0, %1\n\t"
		"addc	%2, %1\n\t"
		"addc	%3, %1\n\t"
		"movt	%0\n\t"
		"add	%1, %0"
		: "=r" (sum), "=r" (len_proto)
		: "r" (daddr), "r" (saddr), "1" (len_proto), "0" (sum)
		: "t");
	return sum;
}

/*
 * computes the checksum of the TCP/UDP pseudo-header
 * returns a 16-bit checksum, already complemented
 */
static __inline__ unsigned short int csum_tcpudp_magic(unsigned long saddr,
						       unsigned long daddr,
						       unsigned short len,
						       unsigned short proto,
						       unsigned int sum) 
{
	return csum_fold(csum_tcpudp_nofold(saddr,daddr,len,proto,sum));
}

/*
 * this routine is used for miscellaneous IP-like checksums, mainly
 * in icmp.c
 */

static __inline__ unsigned short ip_compute_csum(unsigned char * buff, int len)
{
    return csum_fold (csum_partial(buff, len, 0));
}

#define _HAVE_ARCH_IPV6_CSUM
#ifdef CONFIG_IPV6
static __inline__ unsigned short int csum_ipv6_magic(struct in6_addr *saddr,
						     struct in6_addr *daddr,
						     __u32 len,
						     unsigned short proto,
						     unsigned int sum) 
{
	unsigned int __dummy;
	__asm__("clrt\n\t"
		"mov.l	@(0,%2), %1\n\t"
		"addc	%1, %0\n\t"
		"mov.l	@(4,%2), %1\n\t"
		"addc	%1, %0\n\t"
		"mov.l	@(8,%2), %1\n\t"
		"addc	%1, %0\n\t"
		"mov.l	@(12,%2), %1\n\t"
		"addc	%1, %0\n\t"
		"mov.l	@(0,%3), %1\n\t"
		"addc	%1, %0\n\t"
		"mov.l	@(4,%3), %1\n\t"
		"addc	%1, %0\n\t"
		"mov.l	@(8,%3), %1\n\t"
		"addc	%1, %0\n\t"
		"mov.l	@(12,%3), %1\n\t"
		"addc	%1, %0\n\t"
		"addc	%4, %0\n\t"
		"addc	%5, %0\n\t"
		"movt	%1\n\t"
		"add	%1, %0\n"
		: "=r" (sum), "=&r" (__dummy)
		: "r" (saddr), "r" (daddr), 
		  "r" (htonl(len)), "r" (htonl(proto)), "0" (sum)
		: "t");

	return csum_fold(sum);
}
#endif

/* 
 *	Copy and checksum to user
 */
#define HAVE_CSUM_COPY_USER
static __inline__ unsigned int csum_and_copy_to_user (const char *src, 
						      char __user *dst,
						      int len, int sum,
						      int *err_ptr)
{
	if (access_ok(VERIFY_WRITE, dst, len))
		return csum_partial_copy_generic(src, dst, len, sum, NULL, err_ptr);

	if (len)
		*err_ptr = -EFAULT;

	return -1; /* invalid checksum */
}
#endif /* __ASM_SH_CHECKSUM_H */
