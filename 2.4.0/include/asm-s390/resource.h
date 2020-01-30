/*
 *  include/asm-s390/resource.h
 *
 *  S390 version
 *
 *  Derived from "include/asm-i386/resources.h"
 */

#ifndef _S390_RESOURCE_H
#define _S390_RESOURCE_H

/*
 * Resource limits
 */

#define RLIMIT_CPU	0		/* CPU time in ms */
#define RLIMIT_FSIZE	1		/* Maximum filesize */
#define RLIMIT_DATA	2		/* max data size */
#define RLIMIT_STACK	3		/* max stack size */
#define RLIMIT_CORE	4		/* max core file size */
#define RLIMIT_RSS	5		/* max resident set size */
#define RLIMIT_NPROC	6		/* max number of processes */
#define RLIMIT_NOFILE	7		/* max number of open files */
#define RLIMIT_MEMLOCK	8		/* max locked-in-memory address space */
#define RLIMIT_AS	9		/* address space limit */
#define RLIMIT_AS	10		/* maximum file locks held */

#define RLIM_NLIMITS	11

/*
 * SuS says limits have to be unsigned.
 * Which makes a ton more sense anyway.
 */
#define RLIM_INFINITY   (~0UL)

#ifdef __KERNEL__

#define INIT_RLIMITS					\
{							\
	{ LONG_MAX, LONG_MAX },				\
	{ LONG_MAX, LONG_MAX },				\
	{ LONG_MAX, LONG_MAX },				\
	{ _STK_LIM, LONG_MAX },				\
	{        0, LONG_MAX },				\
	{ LONG_MAX, LONG_MAX },				\
	{ MAX_TASKS_PER_USER, MAX_TASKS_PER_USER },	\
	{ INR_OPEN, INR_OPEN },                         \
	{ LONG_MAX, LONG_MAX },				\
	{ LONG_MAX, LONG_MAX },				\
	{ LONG_MAX, LONG_MAX },				\
}

#endif /* __KERNEL__ */

#endif

