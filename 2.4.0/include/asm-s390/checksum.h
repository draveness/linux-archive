#ifndef _S390_CHECKSUM_H
#define _S390_CHECKSUM_H

/*
 *  include/asm-s390/checksum.h
 *    S390 fast network checksum routines
 *    see also arch/S390/lib/checksum.c
 *
 *  S390 version
 *    Copyright (C) 1999 IBM Deutschland Entwicklung GmbH, IBM Corporation
 *    Author(s): Ulrich Hild        (first version)
 *               Martin Schwidefsky (heavily optimized CKSM version)
 *               D.J. Barrow        (third attempt) 
 */

#include <asm/uaccess.h>

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
unsigned int
csum_partial(const unsigned char * buff, int len, unsigned int sum);

/*
 * csum_partial as an inline function
 */
extern inline unsigned int 
csum_partial_inline(const unsigned char * buff, int len, unsigned int sum)
{
	__asm__ __volatile__ (
		"    lr   2,%1\n"    /* address in gpr 2 */
		"    lr   3,%2\n"    /* length in gpr 3 */
		"0:  cksm %0,2\n"    /* do checksum on longs */
		"    jo   0b\n"
                : "+&d" (sum)
		: "d" (buff), "d" (len)
                : "cc", "2", "3" );
	return sum;
}

/*
 * the same as csum_partial, but copies from src while it
 * checksums
 *
 * here even more important to align src and dst on a 32-bit (or even
 * better 64-bit) boundary
 */

extern inline unsigned int 
csum_partial_copy(const char *src, char *dst, int len,unsigned int sum)
{
	memcpy(dst,src,len);
        return csum_partial_inline(dst, len, sum);
}

/*
 * the same as csum_partial_copy, but copies from user space.
 *
 * here even more important to align src and dst on a 32-bit (or even
 * better 64-bit) boundary
 */

extern inline unsigned int 
csum_partial_copy_from_user(const char *src, char *dst,
                            int len, unsigned int sum, int *errp)
{
	if (copy_from_user(dst, src, len)) {
		*errp = -EFAULT;
		memset(dst, 0, len);
		return sum;
        }
        return csum_partial(dst, len, sum);
}

extern inline unsigned int
csum_partial_copy_nocheck (const char *src, char *dst, int len, unsigned int sum)
{
        memcpy(dst,src,len);
        return csum_partial_inline(dst, len, sum);
}

/*
 *      Fold a partial checksum without adding pseudo headers
 */
#if 1
unsigned short csum_fold(unsigned int sum);
#else
extern inline unsigned short
csum_fold(unsigned int sum)
{
	__asm__ __volatile__ (
		"    sr   3,3\n"   /* %0 = H*65536 + L */
		"    lr   2,%0\n"  /* %0 = H L, R2/R3 = H L / 0 0 */
		"    srdl 2,16\n"  /* %0 = H L, R2/R3 = 0 H / L 0 */
		"    alr  2,3\n"   /* %0 = H L, R2/R3 = L H / L 0 */
		"    alr  %0,2\n"  /* %0 = H+L+C L+H */
                "    srl  %0,16\n" /* %0 = H+L+C */
		: "+&d" (sum) : : "cc", "2", "3");
	return ((unsigned short) ~sum);
}
#endif

/*
 *	This is a version of ip_compute_csum() optimized for IP headers,
 *	which always checksum on 4 octet boundaries.
 *
 */
extern inline unsigned short
ip_fast_csum(unsigned char *iph, unsigned int ihl)
{
	unsigned long sum;

        __asm__ __volatile__ (
		"    sr   %0,%0\n"   /* set sum to zero */
                "    lr   2,%1\n"    /* address in gpr 2 */
                "    lr   3,%2\n"    /* length in gpr 3 */
                "0:  cksm %0,2\n"    /* do checksum on longs */
                "    jo   0b\n"
                : "=&d" (sum)
                : "d" (iph), "d" (ihl*4)
                : "cc", "2", "3" );
        return csum_fold(sum);
}

/*
 * computes the checksum of the TCP/UDP pseudo-header
 * returns a 32-bit checksum
 */
extern inline unsigned int 
csum_tcpudp_nofold(unsigned long saddr, unsigned long daddr,
                   unsigned short len, unsigned short proto,
                   unsigned int sum)
{
	__asm__ __volatile__ (
		"    sll   %3,16\n"
		"    or    %3,%4\n"  /* newproto=proto<<16 in hiword, len in lowword */
		"    alr   %1,%2\n"  /* saddr+=daddr */
		"    brc   12,0f\n"
		"    ahi   %1,1\n"   /* add carry */
		"0:  alr   %1,%3\n"  /* add saddr+=newproto */
		"    brc   12,1f\n"
		"    ahi   %1,1\n"   /* add carry again */
		"1:  alr   %0,%1\n"  /* sum+=saddr */
		"    brc   12,2f\n"
		"    ahi   %0,1\n"   /* add carry again */
		"2:"
		: "+&d" (sum)
		: "d" (saddr), "d" (daddr), "d" (proto), "d" (len)
		: "cc" );
	return sum;
}

/*
 * computes the checksum of the TCP/UDP pseudo-header
 * returns a 16-bit checksum, already complemented
 */

extern inline unsigned short int
csum_tcpudp_magic(unsigned long saddr, unsigned long daddr,
                  unsigned short len, unsigned short proto,
                  unsigned int sum)
{
	return csum_fold(csum_tcpudp_nofold(saddr,daddr,len,proto,sum));
}

/*
 * this routine is used for miscellaneous IP-like checksums, mainly
 * in icmp.c
 */

extern inline unsigned short
ip_compute_csum(unsigned char * buff, int len)
{
	return csum_fold(csum_partial(buff, len, 0));
}

#endif /* _S390_CHECKSUM_H */


