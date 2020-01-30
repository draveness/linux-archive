/* $Id: processor.h,v 1.68 2000/12/31 10:05:43 davem Exp $
 * include/asm-sparc64/processor.h
 *
 * Copyright (C) 1996 David S. Miller (davem@caip.rutgers.edu)
 */

#ifndef __ASM_SPARC64_PROCESSOR_H
#define __ASM_SPARC64_PROCESSOR_H

/*
 * Sparc64 implementation of macro that returns current
 * instruction pointer ("program counter").
 */
#define current_text_addr() ({ void *pc; __asm__("rd %%pc, %0" : "=r" (pc)); pc; })

#include <linux/config.h>
#include <asm/asi.h>
#include <asm/a.out.h>
#include <asm/pstate.h>
#include <asm/ptrace.h>
#include <asm/signal.h>
#include <asm/segment.h>

/* Bus types */
#define EISA_bus 0
#define EISA_bus__is_a_macro /* for versions in ksyms.c */
#define MCA_bus 0
#define MCA_bus__is_a_macro /* for versions in ksyms.c */

/* The sparc has no problems with write protection */
#define wp_works_ok 1
#define wp_works_ok__is_a_macro /* for versions in ksyms.c */

/* User lives in his very own context, and cannot reference us. */
#ifndef __ASSEMBLY__
#define TASK_SIZE	((unsigned long)-PGDIR_SIZE)
#else
#define TASK_SIZE	0xfffffffc00000000
#endif

#ifndef __ASSEMBLY__

#define NSWINS		7

typedef struct {
	unsigned char seg;
} mm_segment_t;

/* The Sparc processor specific thread struct. */
struct thread_struct {
	/* D$ line 1 */
	unsigned long ksp __attribute__ ((aligned(16)));
	unsigned char wstate, cwp, flags;
	mm_segment_t current_ds;
	unsigned char w_saved, fpdepth, fault_code, use_blkcommit;
	unsigned long fault_address;
	unsigned char fpsaved[7];
	unsigned char __pad2;
	
	/* D$ line 2, 3, 4 */
	struct pt_regs *kregs;
	unsigned long *utraps;
	unsigned char gsr[7];
	unsigned char __pad3;
	unsigned long xfsr[7];

	struct reg_window reg_window[NSWINS];
	unsigned long rwbuf_stkptrs[NSWINS];
	
	/* Performance counter state */
	u64 *user_cntd0, *user_cntd1;
	u64 kernel_cntd0, kernel_cntd1;
	u64 pcr_reg;
};

#endif /* !(__ASSEMBLY__) */

#define SPARC_FLAG_UNALIGNED    0x01    /* is allowed to do unaligned accesses	*/
#define SPARC_FLAG_NEWSIGNALS   0x02    /* task wants new-style signals		*/
#define SPARC_FLAG_32BIT        0x04    /* task is older 32-bit binary		*/
#define SPARC_FLAG_NEWCHILD     0x08    /* task is just-spawned child process	*/
#define SPARC_FLAG_PERFCTR	0x10    /* task has performance counters active	*/
#define SPARC_FLAG_MMAPSHARED	0x20    /* task wants a shared mmap             */

#define FAULT_CODE_WRITE	0x01	/* Write access, implies D-TLB		*/
#define FAULT_CODE_DTLB		0x02	/* Miss happened in D-TLB		*/
#define FAULT_CODE_ITLB		0x04	/* Miss happened in I-TLB		*/
#define FAULT_CODE_WINFIXUP	0x08	/* Miss happened during spill/fill	*/

#define INIT_MMAP { &init_mm, 0xfffff80000000000, 0xfffff80001000000, \
		    NULL, PAGE_SHARED , VM_READ | VM_WRITE | VM_EXEC, 1, NULL, NULL }

#define INIT_THREAD  {					\
/* ksp, wstate, cwp, flags, current_ds, */ 		\
   0,   0,      0,   0,     KERNEL_DS,			\
/* w_saved, fpdepth, fault_code, use_blkcommit, */	\
   0,       0,       0,          0,			\
/* fault_address, fpsaved, __pad2, kregs, */		\
   0,             { 0 },   0,      0,			\
/* utraps, gsr,   __pad3, xfsr, */			\
   0,	   { 0 }, 0,      { 0 },			\
/* reg_window */					\
   { { { 0, }, { 0, } }, }, 				\
/* rwbuf_stkptrs */					\
   { 0, 0, 0, 0, 0, 0, 0, },				\
/* user_cntd0, user_cndd1, kernel_cntd0, kernel_cntd0, pcr_reg */ \
   0,          0,          0,		 0,            0, \
}

#ifndef __ASSEMBLY__

/* Return saved PC of a blocked thread. */
extern __inline__ unsigned long thread_saved_pc(struct thread_struct *t)
{
	unsigned long ret = 0xdeadbeefUL;
	
	if (t->ksp) {
		unsigned long *sp;
		sp = (unsigned long *)(t->ksp + STACK_BIAS);
		if (((unsigned long)sp & (sizeof(long) - 1)) == 0UL &&
		    sp[14]) {
			unsigned long *fp;
			fp = (unsigned long *)(sp[14] + STACK_BIAS);
			if (((unsigned long)fp & (sizeof(long) - 1)) == 0UL)
				ret = fp[15];
		}
	}
	return ret;
}

/* On Uniprocessor, even in RMO processes see TSO semantics */
#ifdef CONFIG_SMP
#define TSTATE_INITIAL_MM	TSTATE_TSO
#else
#define TSTATE_INITIAL_MM	TSTATE_RMO
#endif

/* Do necessary setup to start up a newly executed thread. */
#define start_thread(regs, pc, sp) \
do { \
	regs->tstate = (regs->tstate & (TSTATE_CWP)) | (TSTATE_INITIAL_MM|TSTATE_IE) | (ASI_PNF << 24); \
	regs->tpc = ((pc & (~3)) - 4); \
	regs->tnpc = regs->tpc + 4; \
	regs->y = 0; \
	current->thread.wstate = (1 << 3); \
	if (current->thread.utraps) { \
		if (*(current->thread.utraps) < 2) \
			kfree (current->thread.utraps); \
		else \
			(*(current->thread.utraps))--; \
		current->thread.utraps = NULL; \
	} \
	__asm__ __volatile__( \
	"stx		%%g0, [%0 + %2 + 0x00]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x08]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x10]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x18]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x20]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x28]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x30]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x38]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x40]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x48]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x50]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x58]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x60]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x68]\n\t" \
	"stx		%1,   [%0 + %2 + 0x70]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x78]\n\t" \
	"wrpr		%%g0, (1 << 3), %%wstate\n\t" \
	: \
	: "r" (regs), "r" (sp - REGWIN_SZ - STACK_BIAS), \
	  "i" ((const unsigned long)(&((struct pt_regs *)0)->u_regs[0]))); \
} while(0)

#define start_thread32(regs, pc, sp) \
do { \
	pc &= 0x00000000ffffffffUL; \
	sp &= 0x00000000ffffffffUL; \
\
	regs->tstate = (regs->tstate & (TSTATE_CWP))|(TSTATE_INITIAL_MM|TSTATE_IE|TSTATE_AM); \
	regs->tpc = ((pc & (~3)) - 4); \
	regs->tnpc = regs->tpc + 4; \
	regs->y = 0; \
	current->thread.wstate = (2 << 3); \
	if (current->thread.utraps) { \
		if (*(current->thread.utraps) < 2) \
			kfree (current->thread.utraps); \
		else \
			(*(current->thread.utraps))--; \
		current->thread.utraps = NULL; \
	} \
	__asm__ __volatile__( \
	"stx		%%g0, [%0 + %2 + 0x00]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x08]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x10]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x18]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x20]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x28]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x30]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x38]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x40]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x48]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x50]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x58]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x60]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x68]\n\t" \
	"stx		%1,   [%0 + %2 + 0x70]\n\t" \
	"stx		%%g0, [%0 + %2 + 0x78]\n\t" \
	"wrpr		%%g0, (2 << 3), %%wstate\n\t" \
	: \
	: "r" (regs), "r" (sp - REGWIN32_SZ), \
	  "i" ((const unsigned long)(&((struct pt_regs *)0)->u_regs[0]))); \
} while(0)

/* Free all resources held by a thread. */
#define release_thread(tsk)		do { } while(0)

extern pid_t kernel_thread(int (*fn)(void *), void * arg, unsigned long flags);

#define copy_segments(tsk, mm)		do { } while (0)
#define release_segments(mm)		do { } while (0)

#define get_wchan(__TSK) \
({	extern void scheduling_functions_start_here(void); \
	extern void scheduling_functions_end_here(void); \
	unsigned long pc, fp, bias = 0; \
	unsigned long task_base = (unsigned long) (__TSK); \
	struct reg_window *rw; \
        unsigned long __ret = 0; \
	int count = 0; \
	if (!(__TSK) || (__TSK) == current || \
            (__TSK)->state == TASK_RUNNING) \
		goto __out; \
	bias = STACK_BIAS; \
	fp = (__TSK)->thread.ksp + bias; \
	do { \
		/* Bogus frame pointer? */ \
		if (fp < (task_base + sizeof(struct task_struct)) || \
		    fp >= (task_base + (2 * PAGE_SIZE))) \
			break; \
		rw = (struct reg_window *) fp; \
		pc = rw->ins[7]; \
		if (pc < ((unsigned long) scheduling_functions_start_here) || \
		    pc >= ((unsigned long) scheduling_functions_end_here)) { \
			__ret = pc; \
			goto __out; \
		} \
		fp = rw->ins[6] + bias; \
	} while (++count < 16); \
__out:	__ret; \
})

#define KSTK_EIP(tsk)  ((tsk)->thread.kregs->tpc)
#define KSTK_ESP(tsk)  ((tsk)->thread.kregs->u_regs[UREG_FP])

#ifdef __KERNEL__
#define THREAD_SIZE (2*PAGE_SIZE)
/* Allocation and freeing of task_struct and kernel stack. */
#define alloc_task_struct()   ((struct task_struct *)__get_free_pages(GFP_KERNEL, 1))
#define free_task_struct(tsk) free_pages((unsigned long)(tsk),1)
#define get_task_struct(tsk)      atomic_inc(&virt_to_page(tsk)->count)

#define init_task	(init_task_union.task)
#define init_stack	(init_task_union.stack)

#endif /* __KERNEL__ */

#endif /* !(__ASSEMBLY__) */

#endif /* !(__ASM_SPARC64_PROCESSOR_H) */
