/*
 * Linux Socket Filter - Kernel level socket filtering
 *
 * Author:
 *     Jay Schulist <jschlst@turbolinux.com>
 *
 * Based on the design of:
 *     - The Berkeley Packet Filter
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * Andi Kleen - Fix a few bad bugs and races.
 */

#include <linux/config.h>
#if defined(CONFIG_FILTER)

#include <linux/module.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/fcntl.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/if_packet.h>
#include <net/ip.h>
#include <net/protocol.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <linux/filter.h>

/* No hurry in this branch */

static u8 *load_pointer(struct sk_buff *skb, int k)
{
	u8 *ptr = NULL;

	if (k>=SKF_NET_OFF)
		ptr = skb->nh.raw + k - SKF_NET_OFF;
	else if (k>=SKF_LL_OFF)
		ptr = skb->mac.raw + k - SKF_LL_OFF;

	if (ptr >= skb->head && ptr < skb->tail)
		return ptr;
	return NULL;
}

/**
 *	sk_run_filter	- 	run a filter on a socket
 *	@skb: buffer to run the filter on
 *	@filter: filter to apply
 *	@flen: length of filter
 *
 * Decode and apply filter instructions to the skb->data.
 * Return length to keep, 0 for none. skb is the data we are
 * filtering, filter is the array of filter instructions, and
 * len is the number of filter blocks in the array.
 */
 
int sk_run_filter(struct sk_buff *skb, struct sock_filter *filter, int flen)
{
	unsigned char *data = skb->data;
	/* len is UNSIGNED. Byte wide insns relies only on implicit
	   type casts to prevent reading arbitrary memory locations.
	 */
	unsigned int len = skb->len;
	struct sock_filter *fentry;	/* We walk down these */
	u32 A = 0;	   		/* Accumulator */
	u32 X = 0;   			/* Index Register */
	u32 mem[BPF_MEMWORDS];		/* Scratch Memory Store */
	int k;
	int pc;

	/*
	 * Process array of filter instructions.
	 */

	for(pc = 0; pc < flen; pc++)
	{
		fentry = &filter[pc];
			
		switch(fentry->code)
		{
			case BPF_ALU|BPF_ADD|BPF_X:
				A += X;
				continue;

			case BPF_ALU|BPF_ADD|BPF_K:
				A += fentry->k;
				continue;

			case BPF_ALU|BPF_SUB|BPF_X:
				A -= X;
				continue;

			case BPF_ALU|BPF_SUB|BPF_K:
				A -= fentry->k;
				continue;

			case BPF_ALU|BPF_MUL|BPF_X:
				A *= X;
				continue;

			case BPF_ALU|BPF_MUL|BPF_K:
				A *= fentry->k;
				continue;

			case BPF_ALU|BPF_DIV|BPF_X:
				if(X == 0)
					return (0);
				A /= X;
				continue;

			case BPF_ALU|BPF_DIV|BPF_K:
				if(fentry->k == 0)
					return (0);
				A /= fentry->k;
				continue;

			case BPF_ALU|BPF_AND|BPF_X:
				A &= X;
				continue;

			case BPF_ALU|BPF_AND|BPF_K:
				A &= fentry->k;
				continue;

			case BPF_ALU|BPF_OR|BPF_X:
				A |= X;
				continue;

			case BPF_ALU|BPF_OR|BPF_K:
				A |= fentry->k;
				continue;

			case BPF_ALU|BPF_LSH|BPF_X:
				A <<= X;
				continue;

			case BPF_ALU|BPF_LSH|BPF_K:
				A <<= fentry->k;
				continue;

			case BPF_ALU|BPF_RSH|BPF_X:
				A >>= X;
				continue;

			case BPF_ALU|BPF_RSH|BPF_K:
				A >>= fentry->k;
				continue;

			case BPF_ALU|BPF_NEG:
				A = -A;
				continue;

			case BPF_JMP|BPF_JA:
				pc += fentry->k;
				continue;

			case BPF_JMP|BPF_JGT|BPF_K:
				pc += (A > fentry->k) ? fentry->jt : fentry->jf;
				continue;

			case BPF_JMP|BPF_JGE|BPF_K:
				pc += (A >= fentry->k) ? fentry->jt : fentry->jf;
				continue;

			case BPF_JMP|BPF_JEQ|BPF_K:
				pc += (A == fentry->k) ? fentry->jt : fentry->jf;
				continue;

			case BPF_JMP|BPF_JSET|BPF_K:
				pc += (A & fentry->k) ? fentry->jt : fentry->jf;
				continue;

			case BPF_JMP|BPF_JGT|BPF_X:
				pc += (A > X) ? fentry->jt : fentry->jf;
				continue;

			case BPF_JMP|BPF_JGE|BPF_X:
				pc += (A >= X) ? fentry->jt : fentry->jf;
				continue;

			case BPF_JMP|BPF_JEQ|BPF_X:
				pc += (A == X) ? fentry->jt : fentry->jf;
				continue;

			case BPF_JMP|BPF_JSET|BPF_X:
				pc += (A & X) ? fentry->jt : fentry->jf;
				continue;

			case BPF_LD|BPF_W|BPF_ABS:
				k = fentry->k;
load_w:
				if(k+sizeof(u32) <= len) {
					A = ntohl(*(u32*)&data[k]);
					continue;
				}
				if (k<0) {
					u8 *ptr;

					if (k>=SKF_AD_OFF)
						break;
					if ((ptr = load_pointer(skb, k)) != NULL) {
						A = ntohl(*(u32*)ptr);
						continue;
					}
				}
				return 0;

			case BPF_LD|BPF_H|BPF_ABS:
				k = fentry->k;
load_h:
				if(k + sizeof(u16) <= len) {
					A = ntohs(*(u16*)&data[k]);
					continue;
				}
				if (k<0) {
					u8 *ptr;

					if (k>=SKF_AD_OFF)
						break;
					if ((ptr = load_pointer(skb, k)) != NULL) {
						A = ntohs(*(u16*)ptr);
						continue;
					}
				}
				return 0;

			case BPF_LD|BPF_B|BPF_ABS:
				k = fentry->k;
load_b:
				if(k < len) {
					A = data[k];
					continue;
				}
				if (k<0) {
					u8 *ptr;

					if (k>=SKF_AD_OFF)
						break;
					if ((ptr = load_pointer(skb, k)) != NULL) {
						A = *ptr;
						continue;
					}
				}
				return 0;

			case BPF_LD|BPF_W|BPF_LEN:
				A = len;
				continue;

			case BPF_LDX|BPF_W|BPF_LEN:
				X = len;
				continue;

			case BPF_LD|BPF_W|BPF_IND:
				k = X + fentry->k;
				goto load_w;

                       case BPF_LD|BPF_H|BPF_IND:
				k = X + fentry->k;
				goto load_h;

                       case BPF_LD|BPF_B|BPF_IND:
				k = X + fentry->k;
				goto load_b;

			case BPF_LDX|BPF_B|BPF_MSH:
				k = fentry->k;
				if(k >= len)
					return (0);
				X = (data[k] & 0xf) << 2;
				continue;

			case BPF_LD|BPF_IMM:
				A = fentry->k;
				continue;

			case BPF_LDX|BPF_IMM:
				X = fentry->k;
				continue;

			case BPF_LD|BPF_MEM:
				A = mem[fentry->k];
				continue;

			case BPF_LDX|BPF_MEM:
				X = mem[fentry->k];
				continue;

			case BPF_MISC|BPF_TAX:
				X = A;
				continue;

			case BPF_MISC|BPF_TXA:
				A = X;
				continue;

			case BPF_RET|BPF_K:
				return ((unsigned int)fentry->k);

			case BPF_RET|BPF_A:
				return ((unsigned int)A);

			case BPF_ST:
				mem[fentry->k] = A;
				continue;

			case BPF_STX:
				mem[fentry->k] = X;
				continue;

			default:
				/* Invalid instruction counts as RET */
				return (0);
		}

		/* Handle ancillary data, which are impossible
		   (or very difficult) to get parsing packet contents.
		 */
		switch (k-SKF_AD_OFF) {
		case SKF_AD_PROTOCOL:
			A = htons(skb->protocol);
			continue;
		case SKF_AD_PKTTYPE:
			A = skb->pkt_type;
			continue;
		case SKF_AD_IFINDEX:
			A = skb->dev->ifindex;
			continue;
		default:
			return 0;
		}
	}

	return (0);
}

/**
 *	sk_chk_filter - verify socket filter code
 *	@filter: filter to verify
 *	@flen: length of filter
 *
 * Check the user's filter code. If we let some ugly
 * filter code slip through kaboom! The filter must contain
 * no references or jumps that are out of range, no illegal instructions
 * and no backward jumps. It must end with a RET instruction
 *
 * Returns 0 if the rule set is legal or a negative errno code if not.
 */

int sk_chk_filter(struct sock_filter *filter, int flen)
{
	struct sock_filter *ftest;
        int pc;

       /*
        * Check the filter code now.
        */
	for(pc = 0; pc < flen; pc++)
	{
		/*
                 *	All jumps are forward as they are not signed
                 */
                 
                ftest = &filter[pc];
		if(BPF_CLASS(ftest->code) == BPF_JMP)
		{
			/*
			 *	But they mustn't jump off the end.
			 */
			if(BPF_OP(ftest->code) == BPF_JA)
			{
				/* Note, the large ftest->k might cause
				   loops. Compare this with conditional
				   jumps below, where offsets are limited. --ANK (981016)
				 */
				if (ftest->k >= (unsigned)(flen-pc-1))
					return (-EINVAL);
			}
                        else
			{
				/*
				 *	For conditionals both must be safe
				 */
 				if(pc + ftest->jt +1 >= flen || pc + ftest->jf +1 >= flen)
					return (-EINVAL);
			}
                }

                /*
                 *	Check that memory operations use valid addresses.
                 */
                 
                if (ftest->k >= BPF_MEMWORDS)
                {
                	/*
                	 *	But it might not be a memory operation...
                	 */
			switch (ftest->code) {
			case BPF_ST:	
			case BPF_STX:	
			case BPF_LD|BPF_MEM:	
			case BPF_LDX|BPF_MEM:	
                		return -EINVAL;
			}
		}
        }

	/*
	 *	The program must end with a return. We don't care where they
	 *	jumped within the script (its always forwards) but in the
	 *	end they _will_ hit this.
	 */
	 
        return (BPF_CLASS(filter[flen - 1].code) == BPF_RET)?0:-EINVAL;
}

/**
 *	sk_attach_filter - attach a socket filter
 *	@fprog: the filter program
 *	@sk: the socket to use
 *
 * Attach the user's filter code. We first run some sanity checks on
 * it to make sure it does not explode on us later. If an error
 * occurs or there is insufficient memory for the filter a negative
 * errno code is returned. On success the return is zero.
 */

int sk_attach_filter(struct sock_fprog *fprog, struct sock *sk)
{
	struct sk_filter *fp; 
	unsigned int fsize = sizeof(struct sock_filter) * fprog->len;
	int err;

	/* Make sure new filter is there and in the right amounts. */
        if (fprog->filter == NULL || fprog->len > BPF_MAXINSNS)
                return (-EINVAL);

	fp = (struct sk_filter *)sock_kmalloc(sk, fsize+sizeof(*fp), GFP_KERNEL);
	if(fp == NULL)
		return (-ENOMEM);

	if (copy_from_user(fp->insns, fprog->filter, fsize)) {
		sock_kfree_s(sk, fp, fsize+sizeof(*fp)); 
		return -EFAULT;
	}

	atomic_set(&fp->refcnt, 1);
	fp->len = fprog->len;

	if ((err = sk_chk_filter(fp->insns, fp->len))==0) {
		struct sk_filter *old_fp;

		spin_lock_bh(&sk->lock.slock);
		old_fp = sk->filter;
		sk->filter = fp;
		spin_unlock_bh(&sk->lock.slock);
		fp = old_fp;
	}

	if (fp)
		sk_filter_release(sk, fp);

	return (err);
}
#endif /* CONFIG_FILTER */
