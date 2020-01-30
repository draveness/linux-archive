/*
 * Copyright (C) 1999 Cort Dougan <cort@cs.nmt.edu>
 */
#ifndef __PPC_SYSTEM_H
#define __PPC_SYSTEM_H

#include <linux/config.h>
#include <linux/kernel.h>

#include <asm/atomic.h>
#include <asm/hw_irq.h>

/*
 * Memory barrier.
 * The sync instruction guarantees that all memory accesses initiated
 * by this processor have been performed (with respect to all other
 * mechanisms that access memory).  The eieio instruction is a barrier
 * providing an ordering (separately) for (a) cacheable stores and (b)
 * loads and stores to non-cacheable memory (e.g. I/O devices).
 *
 * mb() prevents loads and stores being reordered across this point.
 * rmb() prevents loads being reordered across this point.
 * wmb() prevents stores being reordered across this point.
 * read_barrier_depends() prevents data-dependent loads being reordered
 *	across this point (nop on PPC).
 *
 * We can use the eieio instruction for wmb, but since it doesn't
 * give any ordering guarantees about loads, we have to use the
 * stronger but slower sync instruction for mb and rmb.
 */
#define mb()  __asm__ __volatile__ ("sync" : : : "memory")
#define rmb()  __asm__ __volatile__ ("sync" : : : "memory")
#define wmb()  __asm__ __volatile__ ("eieio" : : : "memory")
#define read_barrier_depends()  do { } while(0)

#define set_mb(var, value)	do { var = value; mb(); } while (0)
#define set_wmb(var, value)	do { var = value; wmb(); } while (0)

#ifdef CONFIG_SMP
#define smp_mb()	mb()
#define smp_rmb()	rmb()
#define smp_wmb()	wmb()
#define smp_read_barrier_depends()	read_barrier_depends()
#else
#define smp_mb()	barrier()
#define smp_rmb()	barrier()
#define smp_wmb()	barrier()
#define smp_read_barrier_depends()	do { } while(0)
#endif /* CONFIG_SMP */

#ifdef __KERNEL__
struct task_struct;
struct pt_regs;

extern void print_backtrace(unsigned long *);
extern void show_regs(struct pt_regs * regs);
extern void flush_instruction_cache(void);
extern void hard_reset_now(void);
extern void poweroff_now(void);
#ifdef CONFIG_6xx
extern long _get_L2CR(void);
extern long _get_L3CR(void);
extern void _set_L2CR(unsigned long);
extern void _set_L3CR(unsigned long);
#else
#define _get_L2CR()	0L
#define _get_L3CR()	0L
#define _set_L2CR(val)	do { } while(0)
#define _set_L3CR(val)	do { } while(0)
#endif
extern void via_cuda_init(void);
extern void pmac_nvram_init(void);
extern void read_rtc_time(void);
extern void pmac_find_display(void);
extern void giveup_fpu(struct task_struct *);
extern void enable_kernel_fp(void);
extern void giveup_altivec(struct task_struct *);
extern void load_up_altivec(struct task_struct *);
extern void giveup_spe(struct task_struct *);
extern void load_up_spe(struct task_struct *);
extern int fix_alignment(struct pt_regs *);
extern void cvt_fd(float *from, double *to, unsigned long *fpscr);
extern void cvt_df(double *from, float *to, unsigned long *fpscr);
extern int call_rtas(const char *, int, int, unsigned long *, ...);
extern void cacheable_memzero(void *p, unsigned int nb);
extern int do_page_fault(struct pt_regs *, unsigned long, unsigned long);
extern void bad_page_fault(struct pt_regs *, unsigned long, int);
extern void die(const char *, struct pt_regs *, long);

struct device_node;
extern void note_scsi_host(struct device_node *, void *);

extern struct task_struct *__switch_to(struct task_struct *,
	struct task_struct *);
#define switch_to(prev, next, last)	((last) = __switch_to((prev), (next)))

struct thread_struct;
extern struct task_struct *_switch(struct thread_struct *prev,
				   struct thread_struct *next);

extern unsigned int rtas_data;

static __inline__ unsigned long
xchg_u32(volatile void *p, unsigned long val)
{
	unsigned long prev;

	__asm__ __volatile__ ("\n\
1:	lwarx	%0,0,%2 \n"
	PPC405_ERR77(0,%2)
"	stwcx.	%3,0,%2 \n\
	bne-	1b"
	: "=&r" (prev), "=m" (*(volatile unsigned long *)p)
	: "r" (p), "r" (val), "m" (*(volatile unsigned long *)p)
	: "cc", "memory");

	return prev;
}

/*
 * This function doesn't exist, so you'll get a linker error
 * if something tries to do an invalid xchg().
 */
extern void __xchg_called_with_bad_pointer(void);

#define xchg(ptr,x) ((__typeof__(*(ptr)))__xchg((unsigned long)(x),(ptr),sizeof(*(ptr))))
#define tas(ptr) (xchg((ptr),1))

static inline unsigned long __xchg(unsigned long x, volatile void *ptr, int size)
{
	switch (size) {
	case 4:
		return (unsigned long) xchg_u32(ptr, x);
#if 0	/* xchg_u64 doesn't exist on 32-bit PPC */
	case 8:
		return (unsigned long) xchg_u64(ptr, x);
#endif /* 0 */
	}
	__xchg_called_with_bad_pointer();
	return x;


}

extern inline void * xchg_ptr(void * m, void * val)
{
	return (void *) xchg_u32(m, (unsigned long) val);
}


#define __HAVE_ARCH_CMPXCHG	1

static __inline__ unsigned long
__cmpxchg_u32(volatile int *p, int old, int new)
{
	int prev;

	__asm__ __volatile__ ("\n\
1:	lwarx	%0,0,%2 \n\
	cmpw	0,%0,%3 \n\
	bne	2f \n"
	PPC405_ERR77(0,%2)
"	stwcx.	%4,0,%2 \n\
	bne-	1b\n"
#ifdef CONFIG_SMP
"	sync\n"
#endif /* CONFIG_SMP */
"2:"
	: "=&r" (prev), "=m" (*p)
	: "r" (p), "r" (old), "r" (new), "m" (*p)
	: "cc", "memory");

	return prev;
}

/* This function doesn't exist, so you'll get a linker error
   if something tries to do an invalid cmpxchg().  */
extern void __cmpxchg_called_with_bad_pointer(void);

static __inline__ unsigned long
__cmpxchg(volatile void *ptr, unsigned long old, unsigned long new, int size)
{
	switch (size) {
	case 4:
		return __cmpxchg_u32(ptr, old, new);
#if 0	/* we don't have __cmpxchg_u64 on 32-bit PPC */
	case 8:
		return __cmpxchg_u64(ptr, old, new);
#endif /* 0 */
	}
	__cmpxchg_called_with_bad_pointer();
	return old;
}

#define cmpxchg(ptr,o,n)						 \
  ({									 \
     __typeof__(*(ptr)) _o_ = (o);					 \
     __typeof__(*(ptr)) _n_ = (n);					 \
     (__typeof__(*(ptr))) __cmpxchg((ptr), (unsigned long)_o_,		 \
				    (unsigned long)_n_, sizeof(*(ptr))); \
  })

#endif /* __KERNEL__ */
#endif /* __PPC_SYSTEM_H */
