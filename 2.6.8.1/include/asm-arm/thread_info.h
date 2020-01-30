/*
 *  linux/include/asm-arm/thread_info.h
 *
 *  Copyright (C) 2002 Russell King.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __ASM_ARM_THREAD_INFO_H
#define __ASM_ARM_THREAD_INFO_H

#ifdef __KERNEL__

#include <asm/fpstate.h>

#ifndef __ASSEMBLY__

struct task_struct;
struct exec_domain;

#include <asm/ptrace.h>
#include <asm/types.h>
#include <asm/domain.h>

typedef unsigned long mm_segment_t;

struct cpu_context_save {
	__u32	r4;
	__u32	r5;
	__u32	r6;
	__u32	r7;
	__u32	r8;
	__u32	r9;
	__u32	sl;
	__u32	fp;
	__u32	sp;
	__u32	pc;
	__u32	extra[2];		/* Xscale 'acc' register, etc */
};

/*
 * low level task data that entry.S needs immediate access to.
 * __switch_to() assumes cpu_context follows immediately after cpu_domain.
 */
struct thread_info {
	unsigned long		flags;		/* low level flags */
	__s32			preempt_count;	/* 0 => preemptable, <0 => bug */
	mm_segment_t		addr_limit;	/* address limit */
	struct task_struct	*task;		/* main task structure */
	struct exec_domain	*exec_domain;	/* execution domain */
	__u32			cpu;		/* cpu */
	__u32			cpu_domain;	/* cpu domain */
	struct cpu_context_save	cpu_context;	/* cpu context */
	__u8			used_cp[16];	/* thread used copro */
	union fp_state		fpstate;
	union vfp_state		vfpstate;
	struct restart_block	restart_block;
};

#define INIT_THREAD_INFO(tsk)						\
{									\
	.task		= &tsk,						\
	.exec_domain	= &default_exec_domain,				\
	.flags		= 0,						\
	.preempt_count	= 1,						\
	.addr_limit	= KERNEL_DS,					\
	.cpu_domain	= domain_val(DOMAIN_USER, DOMAIN_MANAGER) |	\
			  domain_val(DOMAIN_KERNEL, DOMAIN_MANAGER) |	\
			  domain_val(DOMAIN_IO, DOMAIN_CLIENT),		\
	.restart_block	= {						\
		.fn	= do_no_restart_syscall,			\
	},								\
}

#define init_thread_info	(init_thread_union.thread_info)
#define init_stack		(init_thread_union.stack)

/*
 * how to get the thread information struct from C
 */
static inline struct thread_info *current_thread_info(void) __attribute_const__;

static inline struct thread_info *current_thread_info(void)
{
	register unsigned long sp asm ("sp");
	return (struct thread_info *)(sp & ~0x1fff);
}

#define THREAD_SIZE		(8192)

extern struct thread_info *alloc_thread_info(struct task_struct *task);
extern void free_thread_info(struct thread_info *);

#define get_thread_info(ti)	get_task_struct((ti)->task)
#define put_thread_info(ti)	put_task_struct((ti)->task)

#define thread_saved_pc(tsk)	\
	((unsigned long)(pc_pointer((tsk)->thread_info->cpu_context.pc)))
#define thread_saved_fp(tsk)	\
	((unsigned long)((tsk)->thread_info->cpu_context.fp))

#else /* !__ASSEMBLY__ */

#define TI_FLAGS	0
#define TI_PREEMPT	4
#define TI_ADDR_LIMIT	8
#define TI_TASK		12
#define TI_EXEC_DOMAIN	16
#define TI_CPU		20
#define TI_CPU_DOMAIN	24
#define TI_CPU_SAVE	28
#define TI_USED_CP	76
#define TI_FPSTATE	(TI_USED_CP+16)
#define TI_VFPSTATE	(TI_FPSTATE+FP_SIZE*4)

#endif

#define PREEMPT_ACTIVE	0x04000000

/*
 * thread information flags:
 *  TIF_SYSCALL_TRACE	- syscall trace active
 *  TIF_NOTIFY_RESUME	- resumption notification requested
 *  TIF_SIGPENDING	- signal pending
 *  TIF_NEED_RESCHED	- rescheduling necessary
 *  TIF_USEDFPU		- FPU was used by this task this quantum (SMP)
 *  TIF_POLLING_NRFLAG	- true if poll_idle() is polling TIF_NEED_RESCHED
 */
#define TIF_NOTIFY_RESUME	0
#define TIF_SIGPENDING		1
#define TIF_NEED_RESCHED	2
#define TIF_SYSCALL_TRACE	8
#define TIF_USED_FPU		16
#define TIF_POLLING_NRFLAG	17

#define _TIF_NOTIFY_RESUME	(1 << TIF_NOTIFY_RESUME)
#define _TIF_SIGPENDING		(1 << TIF_SIGPENDING)
#define _TIF_NEED_RESCHED	(1 << TIF_NEED_RESCHED)
#define _TIF_SYSCALL_TRACE	(1 << TIF_SYSCALL_TRACE)
#define _TIF_USED_FPU		(1 << TIF_USED_FPU)
#define _TIF_POLLING_NRFLAG	(1 << TIF_POLLING_NRFLAG)

/*
 * Change these and you break ASM code in entry-common.S
 */
#define _TIF_WORK_MASK		0x000000ff

#endif /* __KERNEL__ */
#endif /* __ASM_ARM_THREAD_INFO_H */
