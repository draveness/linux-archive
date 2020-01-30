#ifndef _H8300_RESOURCE_H
#define _H8300_RESOURCE_H

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
#define RLIMIT_LOCKS	10		/* maximum file locks held */
#define RLIMIT_SIGPENDING 11		/* max number of pending signals */
#define RLIMIT_MSGQUEUE 12		/* maximum bytes in POSIX mqueues */

#define RLIM_NLIMITS	13

/*
 * SuS says limits have to be unsigned.
 * Which makes a ton more sense anyway.
 */
#define RLIM_INFINITY	(~0UL)

#ifdef __KERNEL__

#define INIT_RLIMITS					\
{							\
	{ RLIM_INFINITY, RLIM_INFINITY },		\
	{ RLIM_INFINITY, RLIM_INFINITY },		\
	{ RLIM_INFINITY, RLIM_INFINITY },		\
	{      _STK_LIM, RLIM_INFINITY },		\
	{             0, RLIM_INFINITY },		\
	{ RLIM_INFINITY, RLIM_INFINITY },		\
	{             0,             0 },		\
	{      INR_OPEN,     INR_OPEN  },		\
	{ RLIM_INFINITY, RLIM_INFINITY },		\
	{ RLIM_INFINITY, RLIM_INFINITY },		\
	{ RLIM_INFINITY, RLIM_INFINITY },		\
	{ MAX_SIGPENDING, MAX_SIGPENDING },		\
	{ MQ_BYTES_MAX, MQ_BYTES_MAX },			\
}

#endif /* __KERNEL__ */

#endif /* _H8300_RESOURCE_H */
