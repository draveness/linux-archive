/*
 * Network Checksum & Copy routine
 * 
 * Copyright (C) 1999 Hewlett-Packard Co
 * Copyright (C) 1999 Stephane Eranian <eranian@hpl.hp.com>
 *
 * Most of the code has been imported from Linux/Alpha
 */

#include <linux/types.h>
#include <linux/string.h>

#include <asm/uaccess.h>

/*
 * XXX Fixme: those 2 inlines are meant for debugging and will go away
 */
static inline unsigned
short from64to16(unsigned long x)
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

static inline
unsigned long do_csum_c(const unsigned char * buff, int len, unsigned int psum)
{
	int odd, count;
	unsigned long result = (unsigned long)psum;

	if (len <= 0)
		goto out;
	odd = 1 & (unsigned long) buff;
	if (odd) {
		result = *buff << 8;
		len--;
		buff++;
	}
	count = len >> 1;		/* nr of 16-bit words.. */
	if (count) {
		if (2 & (unsigned long) buff) {
			result += *(unsigned short *) buff;
			count--;
			len -= 2;
			buff += 2;
		}
		count >>= 1;		/* nr of 32-bit words.. */
		if (count) {
			if (4 & (unsigned long) buff) {
				result += *(unsigned int *) buff;
				count--;
				len -= 4;
				buff += 4;
			}
			count >>= 1;	/* nr of 64-bit words.. */
			if (count) {
				unsigned long carry = 0;
				do {
					unsigned long w = *(unsigned long *) buff;
					count--;
					buff += 8;
					result += carry;
					result += w;
					carry = (w > result);
				} while (count);
				result += carry;
				result = (result & 0xffffffff) + (result >> 32);
			}
			if (len & 4) {
				result += *(unsigned int *) buff;
				buff += 4;
			}
		}
		if (len & 2) {
			result += *(unsigned short *) buff;
			buff += 2;
		}
	}
	if (len & 1)
		result += *buff;

	result = from64to16(result);

	if (odd)
		result = ((result >> 8) & 0xff) | ((result & 0xff) << 8);

out:
	return result;
}

/*
 * XXX Fixme
 *
 * This is very ugly but temporary. THIS NEEDS SERIOUS ENHANCEMENTS.
 * But it's very tricky to get right even in C.
 */
extern unsigned long do_csum(const unsigned char *, int);

static unsigned int
do_csum_partial_copy_from_user (const char *src, char *dst, int len,
				unsigned int psum, int *errp)
{
	const unsigned char *psrc = src;
	unsigned long result;
	int cplen = len;
	int r = 0;

	/* XXX Fixme
	 * for now we separate the copy from checksum for obvious 
	 * alignment difficulties. Look at the Alpha code and you'll be
	 * scared.
	 */

	while ( cplen-- ) r |=__get_user(*dst++,psrc++);

	if ( r && errp ) *errp = r;

	result = do_csum(src, len);

	/* add in old sum, and carry.. */
	result += psum;
	/* 32+c bits -> 32 bits */
	result = (result & 0xffffffff) + (result >> 32);
	return result;
}

unsigned int
csum_partial_copy_from_user(const char *src, char *dst, int len,
			    unsigned int sum, int *errp)
{
	if (!access_ok(src, len, VERIFY_READ)) {
		*errp = -EFAULT;
		memset(dst, 0, len);
		return sum;
	}

	return do_csum_partial_copy_from_user(src, dst, len, sum, errp);
}

unsigned int
csum_partial_copy_nocheck(const char *src, char *dst, int len, unsigned int sum)
{
	return do_csum_partial_copy_from_user(src, dst, len, sum, NULL);
}

unsigned int
csum_partial_copy (const char *src, char *dst, int len, unsigned int sum)
{
	unsigned int ret;
	int error = 0;

	ret = do_csum_partial_copy_from_user(src, dst, len, sum, &error);
	if (error)
		printk("csum_partial_copy_old(): tell mingo to convert me!\n");

	return ret;
}

