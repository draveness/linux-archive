/*
 * arch/sh/lib/csum_parial.c
 *
 * This file contains network checksum routines that are better done
 * in an architecture-specific manner due to speed..
 */

#undef DEBUG

#include <linux/config.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <asm/byteorder.h>
#include <asm/uaccess.h>

static inline unsigned short from64to16(unsigned long long x)
{
	/* add up 32-bit words for 33 bits */
	x = (x & 0xffffffff) + (x >> 32);
	/* add up 16-bit and 17-bit words for 17+c bits */
	x = (x & 0xffff) + (x >> 16);
	/* add up 16-bit and 2-bit for 16+c bit */
	x = (x & 0xffff) + (x >> 16);
	/* add up carry.. */
	x = (x & 0xffff) + (x >> 16);
	return x;
}

static inline unsigned short foldto16(unsigned long x)
{
	/* add up 16-bit for 17 bits */
	x = (x & 0xffff) + (x >> 16);
	/* add up carry.. */
	x = (x & 0xffff) + (x >> 16);
	return x;
}

static inline unsigned short myfoldto16(unsigned long long x)
{
	/* Fold down to 32-bits so we don't loose in the typedef-less
	   network stack.  */
	/* 64 to 33 */
	x = (x & 0xffffffff) + (x >> 32);
	/* 33 to 32 */
	x = (x & 0xffffffff) + (x >> 32);

	/* add up 16-bit for 17 bits */
	x = (x & 0xffff) + (x >> 16);
	/* add up carry.. */
	x = (x & 0xffff) + (x >> 16);
	return x;
}

#define odd(x) ((x)&1)
#define U16(x) ntohs(x)

static unsigned long do_csum(const unsigned char *buff, int len)
{
	int odd, count;
	unsigned long result = 0;

	pr_debug("do_csum buff %p, len %d (0x%x)\n", buff, len, len);
#ifdef DEBUG
	for (i = 0; i < len; i++) {
		if ((i % 26) == 0)
			printk("\n");
		printk("%02X ", buff[i]);
	}
#endif

	if (len <= 0)
		goto out;

	odd = 1 & (unsigned long) buff;
	if (odd) {
		result = *buff << 8;
		len--;
		buff++;
	}
	count = len >> 1;	/* nr of 16-bit words.. */
	if (count) {
		if (2 & (unsigned long) buff) {
			result += *(unsigned short *) buff;
			count--;
			len -= 2;
			buff += 2;
		}
		count >>= 1;	/* nr of 32-bit words.. */
		if (count) {
			unsigned long carry = 0;
			do {
				unsigned long w = *(unsigned long *) buff;
				buff += 4;
				count--;
				result += carry;
				result += w;
				carry = (w > result);
			} while (count);
			result += carry;
			result = (result & 0xffff) + (result >> 16);
		}
		if (len & 2) {
			result += *(unsigned short *) buff;
			buff += 2;
		}
	}
	if (len & 1)
		result += *buff;
	result = foldto16(result);
	if (odd)
		result = ((result >> 8) & 0xff) | ((result & 0xff) << 8);

	pr_debug("\nCHECKSUM is 0x%x\n", result);

      out:
	return result;
}

/* computes the checksum of a memory block at buff, length len,
   and adds in "sum" (32-bit)  */
unsigned int csum_partial(const unsigned char *buff, int len, unsigned int sum)
{
	unsigned long long result = do_csum(buff, len);

	/* add in old sum, and carry.. */
	result += sum;
	/* 32+c bits -> 32 bits */
	result = (result & 0xffffffff) + (result >> 32);

	pr_debug("csum_partial, buff %p len %d sum 0x%x result=0x%016Lx\n",
		buff, len, sum, result);

	return result;
}

/* Copy while checksumming, otherwise like csum_partial.  */
unsigned int
csum_partial_copy(const char *src, char *dst, int len, unsigned int sum)
{
	sum = csum_partial(src, len, sum);
	memcpy(dst, src, len);

	return sum;
}

/* Copy from userspace and compute checksum.  If we catch an exception
   then zero the rest of the buffer.  */
unsigned int
csum_partial_copy_from_user(const char *src, char *dst, int len,
			    unsigned int sum, int *err_ptr)
{
	int missing;

	pr_debug
	    ("csum_partial_copy_from_user src %p, dest %p, len %d, sum %08x, err_ptr %p\n",
	     src, dst, len, sum, err_ptr);
	missing = copy_from_user(dst, src, len);
	pr_debug("  access_ok %d\n", __access_ok((unsigned long) src, len));
	pr_debug("  missing %d\n", missing);
	if (missing) {
		memset(dst + len - missing, 0, missing);
		*err_ptr = -EFAULT;
	}

	return csum_partial(dst, len, sum);
}

/* Copy to userspace and compute checksum.  */
unsigned int
csum_partial_copy_to_user(const char *src, char *dst, int len,
			  unsigned int sum, int *err_ptr)
{
	sum = csum_partial(src, len, sum);

	if (copy_to_user(dst, src, len))
		*err_ptr = -EFAULT;

	return sum;
}

/*
 *	This is a version of ip_compute_csum() optimized for IP headers,
 *	which always checksum on 4 octet boundaries.
 */
unsigned short ip_fast_csum(unsigned char *iph, unsigned int ihl)
{
	pr_debug("ip_fast_csum %p,%d\n", iph, ihl);

	return ~do_csum(iph, ihl * 4);
}

unsigned int csum_tcpudp_nofold(unsigned long saddr,
				unsigned long daddr,
				unsigned short len,
				unsigned short proto, unsigned int sum)
{
	unsigned long long result;

	pr_debug("ntohs(0x%x)=0x%x\n", 0xdead, ntohs(0xdead));
	pr_debug("htons(0x%x)=0x%x\n", 0xdead, htons(0xdead));

	result = ((unsigned long long) saddr +
		  (unsigned long long) daddr +
		  (unsigned long long) sum +
		  ((unsigned long long) ntohs(len) << 16) +
		  ((unsigned long long) proto << 8));

	/* Fold down to 32-bits so we don't loose in the typedef-less
	   network stack.  */
	/* 64 to 33 */
	result = (result & 0xffffffff) + (result >> 32);
	/* 33 to 32 */
	result = (result & 0xffffffff) + (result >> 32);

	pr_debug("%s saddr %x daddr %x len %x proto %x sum %x result %08Lx\n",
		__FUNCTION__, saddr, daddr, len, proto, sum, result);

	return result;
}

// Post SIM:
unsigned int
csum_partial_copy_nocheck(const char *src, char *dst, int len, unsigned int sum)
{
	//  unsigned dummy;
	pr_debug("csum_partial_copy_nocheck src %p dst %p len %d\n", src, dst,
		len);

	return csum_partial_copy(src, dst, len, sum);
}
