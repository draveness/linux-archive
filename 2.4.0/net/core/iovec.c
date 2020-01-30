/*
 *	iovec manipulation routines.
 *
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 *	Fixes:
 *		Andrew Lunn	:	Errors in iovec copying.
 *		Pedro Roque	:	Added memcpy_fromiovecend and
 *					csum_..._fromiovecend.
 *		Andi Kleen	:	fixed error handling for 2.1
 *		Alexey Kuznetsov:	2.1 optimisations
 *		Andi Kleen	:	Fix csum*fromiovecend for IPv6.
 */


#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/malloc.h>
#include <linux/net.h>
#include <linux/in6.h>
#include <asm/uaccess.h>
#include <asm/byteorder.h>
#include <net/checksum.h>
#include <net/sock.h>

/*
 *	Verify iovec. The caller must ensure that the iovec is big enough
 *	to hold the message iovec.
 *
 *	Save time not doing verify_area. copy_*_user will make this work
 *	in any case.
 */

int verify_iovec(struct msghdr *m, struct iovec *iov, char *address, int mode)
{
	int size, err, ct;
	
	if(m->msg_namelen)
	{
		if(mode==VERIFY_READ)
		{
			err=move_addr_to_kernel(m->msg_name, m->msg_namelen, address);
			if(err<0)
				goto out;
		}
		
		m->msg_name = address;
	} else
		m->msg_name = NULL;

	err = -EFAULT;
	size = m->msg_iovlen * sizeof(struct iovec);
	if (copy_from_user(iov, m->msg_iov, size))
		goto out;
	m->msg_iov=iov;

	for (err = 0, ct = 0; ct < m->msg_iovlen; ct++) {
		err += iov[ct].iov_len;
		/* Goal is not to verify user data, but to prevent returning
		   negative value, which is interpreted as errno.
		   Overflow is still possible, but it is harmless.
		 */
		if (err < 0)
			return -EMSGSIZE;
	}
out:
	return err;
}

/*
 *	Copy kernel to iovec. Returns -EFAULT on error.
 *
 *	Note: this modifies the original iovec.
 */
 
int memcpy_toiovec(struct iovec *iov, unsigned char *kdata, int len)
{
	int err = -EFAULT; 

	while(len>0)
	{
		if(iov->iov_len)
		{
			int copy = min(iov->iov_len, len);
			if (copy_to_user(iov->iov_base, kdata, copy))
				goto out;
			kdata+=copy;
			len-=copy;
			iov->iov_len-=copy;
			iov->iov_base+=copy;
		}
		iov++;
	}
	err = 0;
out:
	return err;
}

/* Copy and checkum skb to user iovec. Caller _must_ check that
   skb will fit to this iovec.

   Returns: 0       - success.
            -EINVAL - checksum failure.
	    -EFAULT - fault during copy. Beware, in this case iovec can be
	              modified!
 */

int copy_and_csum_toiovec(struct iovec *iov, struct sk_buff *skb, int hlen)
{
	unsigned int csum;
	int chunk = skb->len - hlen;

	/* Skip filled elements. Pretty silly, look at memcpy_toiovec, though 8) */
	while (iov->iov_len == 0)
		iov++;

	if (iov->iov_len < chunk) {
		if ((unsigned short)csum_fold(csum_partial(skb->h.raw, chunk+hlen, skb->csum)))
			goto csum_error;
		if (memcpy_toiovec(iov, skb->h.raw + hlen, chunk))
			goto fault;
	} else {
		int err = 0;
		csum = csum_partial(skb->h.raw, hlen, skb->csum);
		csum = csum_and_copy_to_user(skb->h.raw+hlen, iov->iov_base,
					     chunk, csum, &err);
		if (err || ((unsigned short)csum_fold(csum)))
			goto csum_error;
		iov->iov_len -= chunk;
		iov->iov_base += chunk;
	}
	return 0;

csum_error:
	return -EINVAL;

fault:
	return -EFAULT;
}

/*
 *	In kernel copy to iovec. Returns -EFAULT on error.
 *
 *	Note: this modifies the original iovec.
 */
 
void memcpy_tokerneliovec(struct iovec *iov, unsigned char *kdata, int len)
{
	while(len>0)
	{
		if(iov->iov_len)
		{
			int copy = min(iov->iov_len, len);
			memcpy(iov->iov_base, kdata, copy);
			kdata+=copy;
			len-=copy;
			iov->iov_len-=copy;
			iov->iov_base+=copy;
		}
		iov++;
	}
}


/*
 *	Copy iovec to kernel. Returns -EFAULT on error.
 *
 *	Note: this modifies the original iovec.
 */
 
int memcpy_fromiovec(unsigned char *kdata, struct iovec *iov, int len)
{
	int err = -EFAULT; 

	while(len>0)
	{
		if(iov->iov_len)
		{
			int copy = min(len, iov->iov_len);
			if (copy_from_user(kdata, iov->iov_base, copy))
				goto out;
			len-=copy;
			kdata+=copy;
			iov->iov_base+=copy;
			iov->iov_len-=copy;
		}
		iov++;
	}
	err = 0;
out:
	return err; 
}


/*
 *	For use with ip_build_xmit
 */

int memcpy_fromiovecend(unsigned char *kdata, struct iovec *iov, int offset,
			int len)
{
	int err = -EFAULT;

	/* Skip over the finished iovecs */
	while(offset >= iov->iov_len)
	{
		offset -= iov->iov_len;
		iov++;
	}

	while (len > 0)
	{
		u8 *base = iov->iov_base + offset;
		int copy = min(len, iov->iov_len - offset);

		offset = 0;
		if (copy_from_user(kdata, base, copy))
			goto out;
		len   -= copy;
		kdata += copy;
		iov++;
	}
	err = 0;
out:
	return err;
}

/*
 *	And now for the all-in-one: copy and checksum from a user iovec
 *	directly to a datagram
 *	Calls to csum_partial but the last must be in 32 bit chunks
 *
 *	ip_build_xmit must ensure that when fragmenting only the last
 *	call to this function will be unaligned also.
 */

int csum_partial_copy_fromiovecend(unsigned char *kdata, struct iovec *iov,
				 int offset, unsigned int len, int *csump)
{
	int csum = *csump;
	int partial_cnt = 0, err = 0;

	/* Skip over the finished iovecs */
	while (offset >= iov->iov_len)
	{
		offset -= iov->iov_len;
		iov++;
	}

	while (len > 0)
	{
		u8 *base = iov->iov_base + offset;
		unsigned int copy = min(len, iov->iov_len - offset);

		offset = 0;
		/* There is a remnant from previous iov. */
		if (partial_cnt)
		{
			int par_len = 4 - partial_cnt;

			/* iov component is too short ... */
			if (par_len > copy) {
				if (copy_from_user(kdata, base, copy))
					goto out_fault;
				kdata += copy;
				base  += copy;
				partial_cnt += copy;
				len   -= copy;
				iov++;
				if (len)
					continue;
				*csump = csum_partial(kdata - partial_cnt,
							 partial_cnt, csum);
				goto out;
			}
			if (copy_from_user(kdata, base, par_len))
				goto out_fault;
			csum = csum_partial(kdata - partial_cnt, 4, csum);
			kdata += par_len;
			base  += par_len;
			copy  -= par_len;
			len   -= par_len;
			partial_cnt = 0;
		}

		if (len > copy)
		{
			partial_cnt = copy % 4;
			if (partial_cnt)
			{
				copy -= partial_cnt;
				if (copy_from_user(kdata + copy, base + copy,
				 		partial_cnt))
					goto out_fault;
			}
		}

		if (copy) {
			csum = csum_and_copy_from_user(base, kdata, copy,
							csum, &err);
			if (err)
				goto out;
		}
		len   -= copy + partial_cnt;
		kdata += copy + partial_cnt;
		iov++;
	}
        *csump = csum;
out:
	return err;

out_fault:
	err = -EFAULT;
	goto out;
}
