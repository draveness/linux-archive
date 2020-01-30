/* $Id: system.h,v 1.69 2002/02/09 19:49:31 davem Exp $ */
#ifndef __SPARC64_SYSTEM_H
#define __SPARC64_SYSTEM_H

#include <linux/config.h>
#include <asm/ptrace.h>
#include <asm/processor.h>
#include <asm/visasm.h>

#ifndef __ASSEMBLY__
/*
 * Sparc (general) CPU types
 */
enum sparc_cpu {
  sun4        = 0x00,
  sun4c       = 0x01,
  sun4m       = 0x02,
  sun4d       = 0x03,
  sun4e       = 0x04,
  sun4u       = 0x05, /* V8 ploos ploos */
  sun_unknown = 0x06,
  ap1000      = 0x07, /* almost a sun4m */
};
                  
#define sparc_cpu_model sun4u

/* This cannot ever be a sun4c nor sun4 :) That's just history. */
#define ARCH_SUN4C_SUN4 0
#define ARCH_SUN4 0

#endif

#define setipl(__new_ipl) \
	__asm__ __volatile__("wrpr	%0, %%pil"  : : "r" (__new_ipl) : "memory")

#define local_irq_disable() \
	__asm__ __volatile__("wrpr	15, %%pil" : : : "memory")

#define local_irq_enable() \
	__asm__ __volatile__("wrpr	0, %%pil" : : : "memory")

#define getipl() \
({ unsigned long retval; __asm__ __volatile__("rdpr	%%pil, %0" : "=r" (retval)); retval; })

#define swap_pil(__new_pil) \
({	unsigned long retval; \
	__asm__ __volatile__("rdpr	%%pil, %0\n\t" \
			     "wrpr	%1, %%pil" \
			     : "=&r" (retval) \
			     : "r" (__new_pil) \
			     : "memory"); \
	retval; \
})

#define read_pil_and_cli() \
({	unsigned long retval; \
	__asm__ __volatile__("rdpr	%%pil, %0\n\t" \
			     "wrpr	15, %%pil" \
			     : "=r" (retval) \
			     : : "memory"); \
	retval; \
})

#define local_save_flags(flags)		((flags) = getipl())
#define local_irq_save(flags)		((flags) = read_pil_and_cli())
#define local_irq_restore(flags)		setipl((flags))

/* On sparc64 IRQ flags are the PIL register.  A value of zero
 * means all interrupt levels are enabled, any other value means
 * only IRQ levels greater than that value will be received.
 * Consequently this means that the lowest IRQ level is one.
 */
#define irqs_disabled()		\
({	unsigned long flags;	\
	local_save_flags(flags);\
	(flags > 0);		\
})

#define nop() 		__asm__ __volatile__ ("nop")

#define membar(type)	__asm__ __volatile__ ("membar " type : : : "memory");
#define mb()		\
	membar("#LoadLoad | #LoadStore | #StoreStore | #StoreLoad");
#define rmb()		membar("#LoadLoad")
#define wmb()		membar("#StoreStore")
#define read_barrier_depends()		do { } while(0)
#define set_mb(__var, __value) \
	do { __var = __value; membar("#StoreLoad | #StoreStore"); } while(0)
#define set_wmb(__var, __value) \
	do { __var = __value; membar("#StoreStore"); } while(0)

#ifdef CONFIG_SMP
#define smp_mb()	mb()
#define smp_rmb()	rmb()
#define smp_wmb()	wmb()
#define smp_read_barrier_depends()	read_barrier_depends()
#else
#define smp_mb()	__asm__ __volatile__("":::"memory");
#define smp_rmb()	__asm__ __volatile__("":::"memory");
#define smp_wmb()	__asm__ __volatile__("":::"memory");
#define smp_read_barrier_depends()	do { } while(0)
#endif

#define flushi(addr)	__asm__ __volatile__ ("flush %0" : : "r" (addr) : "memory")

#define flushw_all()	__asm__ __volatile__("flushw")

/* Performance counter register access. */
#define read_pcr(__p)  __asm__ __volatile__("rd	%%pcr, %0" : "=r" (__p))
#define write_pcr(__p) __asm__ __volatile__("wr	%0, 0x0, %%pcr" : : "r" (__p));
#define read_pic(__p)  __asm__ __volatile__("rd %%pic, %0" : "=r" (__p))

/* Blackbird errata workaround.  See commentary in
 * arch/sparc64/kernel/smp.c:smp_percpu_timer_interrupt()
 * for more information.
 */
#define reset_pic()    						\
	__asm__ __volatile__("ba,pt	%xcc, 99f\n\t"		\
			     ".align	64\n"			\
			  "99:wr	%g0, 0x0, %pic\n\t"	\
			     "rd	%pic, %g0")

#ifndef __ASSEMBLY__

extern void sun_do_break(void);
extern int serial_console;
extern int stop_a_enabled;

static __inline__ int con_is_present(void)
{
	return serial_console ? 0 : 1;
}

extern void synchronize_user_stack(void);

extern void __flushw_user(void);
#define flushw_user() __flushw_user()

#define flush_user_windows flushw_user
#define flush_register_windows flushw_all

#define prepare_arch_switch(rq, next)		\
do {	spin_lock(&(next)->switch_lock);	\
	spin_unlock(&(rq)->lock);		\
	flushw_all();				\
} while (0)

#define finish_arch_switch(rq, prev)		\
do {	spin_unlock_irq(&(prev)->switch_lock);	\
} while (0)

#define task_running(rq, p) \
	((rq)->curr == (p) || spin_is_locked(&(p)->switch_lock))

	/* See what happens when you design the chip correctly?
	 *
	 * We tell gcc we clobber all non-fixed-usage registers except
	 * for l0/l1.  It will use one for 'next' and the other to hold
	 * the output value of 'last'.  'next' is not referenced again
	 * past the invocation of switch_to in the scheduler, so we need
	 * not preserve it's value.  Hairy, but it lets us remove 2 loads
	 * and 2 stores in this critical code path.  -DaveM
	 */
#if __GNUC__ >= 3
#define EXTRA_CLOBBER ,"%l1"
#else
#define EXTRA_CLOBBER
#endif
#define switch_to(prev, next, last)					\
do {	if (test_thread_flag(TIF_PERFCTR)) {				\
		unsigned long __tmp;					\
		read_pcr(__tmp);					\
		current_thread_info()->pcr_reg = __tmp;			\
		read_pic(__tmp);					\
		current_thread_info()->kernel_cntd0 += (unsigned int)(__tmp);\
		current_thread_info()->kernel_cntd1 += ((__tmp) >> 32);	\
	}								\
	flush_tlb_pending();						\
	save_and_clear_fpu();						\
	/* If you are tempted to conditionalize the following */	\
	/* so that ASI is only written if it changes, think again. */	\
	__asm__ __volatile__("wr %%g0, %0, %%asi"			\
	: : "r" (__thread_flag_byte_ptr(next->thread_info)[TI_FLAG_BYTE_CURRENT_DS]));\
	__asm__ __volatile__(						\
	"mov	%%g4, %%g5\n\t"						\
	"wrpr	%%g0, 0x95, %%pstate\n\t"				\
	"stx	%%i6, [%%sp + 2047 + 0x70]\n\t"				\
	"stx	%%i7, [%%sp + 2047 + 0x78]\n\t"				\
	"rdpr	%%wstate, %%o5\n\t"					\
	"stx	%%o6, [%%g6 + %3]\n\t"					\
	"stb	%%o5, [%%g6 + %2]\n\t"					\
	"rdpr	%%cwp, %%o5\n\t"					\
	"stb	%%o5, [%%g6 + %5]\n\t"					\
	"mov	%1, %%g6\n\t"						\
	"ldub	[%1 + %5], %%g1\n\t"					\
	"wrpr	%%g1, %%cwp\n\t"					\
	"ldx	[%%g6 + %3], %%o6\n\t"					\
	"ldub	[%%g6 + %2], %%o5\n\t"					\
	"ldx	[%%g6 + %4], %%o7\n\t"					\
	"mov	%%g6, %%l2\n\t"						\
	"wrpr	%%o5, 0x0, %%wstate\n\t"				\
	"ldx	[%%sp + 2047 + 0x70], %%i6\n\t"				\
	"ldx	[%%sp + 2047 + 0x78], %%i7\n\t"				\
	"wrpr	%%g0, 0x94, %%pstate\n\t"				\
	"mov	%%l2, %%g6\n\t"						\
	"ldx	[%%g6 + %7], %%g4\n\t"					\
	"wrpr	%%g0, 0x96, %%pstate\n\t"				\
	"andcc	%%o7, %6, %%g0\n\t"					\
	"beq,pt %%icc, 1f\n\t"						\
	" mov	%%g5, %0\n\t"						\
	"b,a ret_from_syscall\n\t"					\
	"1:\n\t"							\
	: "=&r" (last)							\
	: "0" (next->thread_info),					\
	  "i" (TI_WSTATE), "i" (TI_KSP), "i" (TI_FLAGS), "i" (TI_CWP),	\
	  "i" (_TIF_NEWCHILD), "i" (TI_TASK)				\
	: "cc",								\
	        "g1", "g2", "g3",       "g5",       "g7",		\
	              "l2", "l3", "l4", "l5", "l6", "l7",		\
	  "i0", "i1", "i2", "i3", "i4", "i5",				\
	  "o0", "o1", "o2", "o3", "o4", "o5",       "o7" EXTRA_CLOBBER);\
	/* If you fuck with this, update ret_from_syscall code too. */	\
	if (test_thread_flag(TIF_PERFCTR)) {				\
		write_pcr(current_thread_info()->pcr_reg);		\
		reset_pic();						\
	}								\
} while(0)

static __inline__ unsigned long xchg32(__volatile__ unsigned int *m, unsigned int val)
{
	__asm__ __volatile__(
"	mov		%0, %%g5\n"
"1:	lduw		[%2], %%g7\n"
"	cas		[%2], %%g7, %0\n"
"	cmp		%%g7, %0\n"
"	bne,a,pn	%%icc, 1b\n"
"	 mov		%%g5, %0\n"
"	membar		#StoreLoad | #StoreStore\n"
	: "=&r" (val)
	: "0" (val), "r" (m)
	: "g5", "g7", "cc", "memory");
	return val;
}

static __inline__ unsigned long xchg64(__volatile__ unsigned long *m, unsigned long val)
{
	__asm__ __volatile__(
"	mov		%0, %%g5\n"
"1:	ldx		[%2], %%g7\n"
"	casx		[%2], %%g7, %0\n"
"	cmp		%%g7, %0\n"
"	bne,a,pn	%%xcc, 1b\n"
"	 mov		%%g5, %0\n"
"	membar		#StoreLoad | #StoreStore\n"
	: "=&r" (val)
	: "0" (val), "r" (m)
	: "g5", "g7", "cc", "memory");
	return val;
}

#define xchg(ptr,x) ((__typeof__(*(ptr)))__xchg((unsigned long)(x),(ptr),sizeof(*(ptr))))
#define tas(ptr) (xchg((ptr),1))

extern void __xchg_called_with_bad_pointer(void);

static __inline__ unsigned long __xchg(unsigned long x, __volatile__ void * ptr,
				       int size)
{
	switch (size) {
	case 4:
		return xchg32(ptr, x);
	case 8:
		return xchg64(ptr, x);
	};
	__xchg_called_with_bad_pointer();
	return x;
}

extern void die_if_kernel(char *str, struct pt_regs *regs) __attribute__ ((noreturn));

/* 
 * Atomic compare and exchange.  Compare OLD with MEM, if identical,
 * store NEW in MEM.  Return the initial value in MEM.  Success is
 * indicated by comparing RETURN with OLD.
 */

#define __HAVE_ARCH_CMPXCHG 1

static __inline__ unsigned long
__cmpxchg_u32(volatile int *m, int old, int new)
{
	__asm__ __volatile__("cas [%2], %3, %0\n\t"
			     "membar #StoreLoad | #StoreStore"
			     : "=&r" (new)
			     : "0" (new), "r" (m), "r" (old)
			     : "memory");

	return new;
}

static __inline__ unsigned long
__cmpxchg_u64(volatile long *m, unsigned long old, unsigned long new)
{
	__asm__ __volatile__("casx [%2], %3, %0\n\t"
			     "membar #StoreLoad | #StoreStore"
			     : "=&r" (new)
			     : "0" (new), "r" (m), "r" (old)
			     : "memory");

	return new;
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
		case 8:
			return __cmpxchg_u64(ptr, old, new);
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

#endif /* !(__ASSEMBLY__) */

#endif /* !(__SPARC64_SYSTEM_H) */
