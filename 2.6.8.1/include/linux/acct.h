/*
 *  BSD Process Accounting for Linux - Definitions
 *
 *  Author: Marco van Wieringen (mvw@planets.elm.net)
 *
 *  This header file contains the definitions needed to implement
 *  BSD-style process accounting. The kernel accounting code and all
 *  user-level programs that try to do something useful with the
 *  process accounting log must include this file.
 *
 *  Copyright (C) 1995 - 1997 Marco van Wieringen - ELM Consultancy B.V.
 *
 */

#ifndef _LINUX_ACCT_H
#define _LINUX_ACCT_H

#include <linux/types.h>
#include <asm/param.h>
#include <asm/byteorder.h>

/* 
 *  comp_t is a 16-bit "floating" point number with a 3-bit base 8
 *  exponent and a 13-bit fraction.
 *  comp2_t is 24-bit with 5-bit base 2 exponent and 20 bit fraction
 *  (leading 1 not stored).
 *  See linux/kernel/acct.c for the specific encoding systems used.
 */

typedef __u16	comp_t;
typedef __u32	comp2_t;

/*
 *   accounting file record
 *
 *   This structure contains all of the information written out to the
 *   process accounting file whenever a process exits.
 */

#define ACCT_COMM	16

struct acct
{
	char		ac_flag;		/* Flags */
	char		ac_version;		/* Always set to ACCT_VERSION */
	/* for binary compatibility back until 2.0 */
	__u16		ac_uid16;		/* LSB of Real User ID */
	__u16		ac_gid16;		/* LSB of Real Group ID */
	__u16		ac_tty;			/* Control Terminal */
	__u32		ac_btime;		/* Process Creation Time */
	comp_t		ac_utime;		/* User Time */
	comp_t		ac_stime;		/* System Time */
	comp_t		ac_etime;		/* Elapsed Time */
	comp_t		ac_mem;			/* Average Memory Usage */
	comp_t		ac_io;			/* Chars Transferred */
	comp_t		ac_rw;			/* Blocks Read or Written */
	comp_t		ac_minflt;		/* Minor Pagefaults */
	comp_t		ac_majflt;		/* Major Pagefaults */
	comp_t		ac_swaps;		/* Number of Swaps */
/* m68k had no padding here. */
#if !defined(CONFIG_M68K) || !defined(__KERNEL__)
	__u16		ac_ahz;			/* AHZ */
#endif
	__u32		ac_exitcode;		/* Exitcode */
	char		ac_comm[ACCT_COMM + 1];	/* Command Name */
	__u8		ac_etime_hi;		/* Elapsed Time MSB */
	__u16		ac_etime_lo;		/* Elapsed Time LSB */
	__u32		ac_uid;			/* Real User ID */
	__u32		ac_gid;			/* Real Group ID */
};

struct acct_v3
{
	char		ac_flag;		/* Flags */
	char		ac_version;		/* Always set to ACCT_VERSION */
	__u16		ac_tty;			/* Control Terminal */
	__u32		ac_exitcode;		/* Exitcode */
	__u32		ac_uid;			/* Real User ID */
	__u32		ac_gid;			/* Real Group ID */
	__u32		ac_pid;			/* Process ID */
	__u32		ac_ppid;		/* Parent Process ID */
	__u32		ac_btime;		/* Process Creation Time */
#ifdef __KERNEL__
	__u32		ac_etime;		/* Elapsed Time */
#else
	float		ac_etime;		/* Elapsed Time */
#endif
	comp_t		ac_utime;		/* User Time */
	comp_t		ac_stime;		/* System Time */
	comp_t		ac_mem;			/* Average Memory Usage */
	comp_t		ac_io;			/* Chars Transferred */
	comp_t		ac_rw;			/* Blocks Read or Written */
	comp_t		ac_minflt;		/* Minor Pagefaults */
	comp_t		ac_majflt;		/* Major Pagefaults */
	comp_t		ac_swaps;		/* Number of Swaps */
	char		ac_comm[ACCT_COMM];	/* Command Name */
};

/*
 *  accounting flags
 */
				/* bit set when the process ... */
#define AFORK		0x01	/* ... executed fork, but did not exec */
#define ASU		0x02	/* ... used super-user privileges */
#define ACOMPAT		0x04	/* ... used compatibility mode (VAX only not used) */
#define ACORE		0x08	/* ... dumped core */
#define AXSIG		0x10	/* ... was killed by a signal */

#ifdef __BIG_ENDIAN
#define ACCT_BYTEORDER	0x80	/* accounting file is big endian */
#else
#define ACCT_BYTEORDER	0x00	/* accounting file is little endian */
#endif

#ifdef __KERNEL__

#include <linux/config.h>

#ifdef CONFIG_BSD_PROCESS_ACCT
struct super_block;
extern void acct_auto_close(struct super_block *sb);
extern void acct_process(long exitcode);
#else
#define acct_auto_close(x)	do { } while (0)
#define acct_process(x)		do { } while (0)
#endif

/*
 * ACCT_VERSION numbers as yet defined:
 * 0: old format (until 2.6.7) with 16 bit uid/gid
 * 1: extended variant (binary compatible on M68K)
 * 2: extended variant (binary compatible on everything except M68K)
 * 3: new binary incompatible format (64 bytes)
 * 4: new binary incompatible format (128 bytes)
 * 5: new binary incompatible format (128 bytes, second half)
 *
 */

#ifdef CONFIG_BSD_PROCESS_ACCT_V3
#define ACCT_VERSION	3
#define AHZ		100
typedef struct acct_v3 acct_t;
#else
#ifdef CONFIG_M68K
#define ACCT_VERSION	1
#else
#define ACCT_VERSION	2
#endif
#define AHZ		(USER_HZ)
typedef struct acct acct_t;
#endif

#else
#define ACCT_VERSION	2
#define AHZ		(HZ)
#endif	/* __KERNEL */

#ifdef __KERNEL__
/*
 * Yet another set of HZ to *HZ helper functions.
 * See <linux/times.h> for the original.
 */

static inline u32 jiffies_to_AHZ(unsigned long x)
{
#if (TICK_NSEC % (NSEC_PER_SEC / AHZ)) == 0
	return x / (HZ / USER_HZ);
#else
        u64 tmp = (u64)x * TICK_NSEC;
        do_div(tmp, (NSEC_PER_SEC / AHZ));
        return (long)tmp;
#endif
}

static inline u64 jiffies_64_to_AHZ(u64 x)
{
#if (TICK_NSEC % (NSEC_PER_SEC / AHZ)) == 0
#if HZ != AHZ
	do_div(x, HZ / AHZ);
#endif
#else
	x *= TICK_NSEC;
	do_div(x, (NSEC_PER_SEC / AHZ));
#endif
       return x;
}

#endif  /* __KERNEL */

#endif	/* _LINUX_ACCT_H */
