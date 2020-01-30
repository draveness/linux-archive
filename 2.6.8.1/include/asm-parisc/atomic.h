#ifndef _ASM_PARISC_ATOMIC_H_
#define _ASM_PARISC_ATOMIC_H_

#include <linux/config.h>
#include <asm/system.h>
/* Copyright (C) 2000 Philipp Rumpf <prumpf@tux.org>.  */

/*
 * Atomic operations that C can't guarantee us.  Useful for
 * resource counting etc..
 *
 * And probably incredibly slow on parisc.  OTOH, we don't
 * have to write any serious assembly.   prumpf
 */

#ifdef CONFIG_SMP
#include <asm/cache.h>		/* we use L1_CACHE_BYTES */

typedef spinlock_t atomic_lock_t;

/* Use an array of spinlocks for our atomic_ts.
 * Hash function to index into a different SPINLOCK.
 * Since "a" is usually an address, use one spinlock per cacheline.
 */
#  define ATOMIC_HASH_SIZE 4
#  define ATOMIC_HASH(a) (&(__atomic_hash[ (((unsigned long) a)/L1_CACHE_BYTES) & (ATOMIC_HASH_SIZE-1) ]))

extern atomic_lock_t __atomic_hash[ATOMIC_HASH_SIZE] __lock_aligned;

static inline void atomic_spin_lock(atomic_lock_t *a)
{
	while (__ldcw(a) == 0)
		while (a->lock[0] == 0);
}

static inline void atomic_spin_unlock(atomic_lock_t *a)
{
	a->lock[0] = 1;
}

#else
#  define ATOMIC_HASH_SIZE 1
#  define ATOMIC_HASH(a)	(0)
#  define atomic_spin_lock(x) (void)(x)
#  define atomic_spin_unlock(x) do { } while(0)
#endif

/* copied from <linux/spinlock.h> and modified */
#define atomic_spin_lock_irqsave(lock, flags)	do { 	\
	local_irq_save(flags);				\
	atomic_spin_lock(lock); 			\
} while (0)

#define atomic_spin_unlock_irqrestore(lock, flags) do {	\
	atomic_spin_unlock(lock);			\
	local_irq_restore(flags);			\
} while (0)

/* Note that we need not lock read accesses - aligned word writes/reads
 * are atomic, so a reader never sees unconsistent values.
 *
 * Cache-line alignment would conflict with, for example, linux/module.h
 */

typedef struct { volatile long counter; } atomic_t;


/* This should get optimized out since it's never called.
** Or get a link error if xchg is used "wrong".
*/
extern void __xchg_called_with_bad_pointer(void);


/* __xchg32/64 defined in arch/parisc/lib/bitops.c */
extern unsigned long __xchg8(char, char *);
extern unsigned long __xchg32(int, int *);
#ifdef __LP64__
extern unsigned long __xchg64(unsigned long, unsigned long *);
#endif

/* optimizer better get rid of switch since size is a constant */
static __inline__ unsigned long __xchg(unsigned long x, __volatile__ void * ptr,
                                       int size)
{

	switch(size) {
#ifdef __LP64__
	case 8: return __xchg64(x,(unsigned long *) ptr);
#endif
	case 4: return __xchg32((int) x, (int *) ptr);
	case 1: return __xchg8((char) x, (char *) ptr);
	}
	__xchg_called_with_bad_pointer();
	return x;
}


/*
** REVISIT - Abandoned use of LDCW in xchg() for now:
** o need to test sizeof(*ptr) to avoid clearing adjacent bytes
** o and while we are at it, could __LP64__ code use LDCD too?
**
**	if (__builtin_constant_p(x) && (x == NULL))
**		if (((unsigned long)p & 0xf) == 0)
**			return __ldcw(p);
*/
#define xchg(ptr,x) \
	((__typeof__(*(ptr)))__xchg((unsigned long)(x),(ptr),sizeof(*(ptr))))


#define __HAVE_ARCH_CMPXCHG	1

/* bug catcher for when unsupported size is used - won't link */
extern void __cmpxchg_called_with_bad_pointer(void);

/* __cmpxchg_u32/u64 defined in arch/parisc/lib/bitops.c */
extern unsigned long __cmpxchg_u32(volatile unsigned int *m, unsigned int old, unsigned int new_);
extern unsigned long __cmpxchg_u64(volatile unsigned long *ptr, unsigned long old, unsigned long new_);

/* don't worry...optimizer will get rid of most of this */
static __inline__ unsigned long
__cmpxchg(volatile void *ptr, unsigned long old, unsigned long new_, int size)
{
	switch(size) {
#ifdef __LP64__
	case 8: return __cmpxchg_u64((unsigned long *)ptr, old, new_);
#endif
	case 4: return __cmpxchg_u32((unsigned int *)ptr, (unsigned int) old, (unsigned int) new_);
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



/* It's possible to reduce all atomic operations to either
 * __atomic_add_return, atomic_set and atomic_read (the latter
 * is there only for consistency).
 */

static __inline__ int __atomic_add_return(int i, atomic_t *v)
{
	int ret;
	unsigned long flags;
	atomic_spin_lock_irqsave(ATOMIC_HASH(v), flags);

	ret = (v->counter += i);

	atomic_spin_unlock_irqrestore(ATOMIC_HASH(v), flags);
	return ret;
}

static __inline__ void atomic_set(atomic_t *v, int i) 
{
	unsigned long flags;
	atomic_spin_lock_irqsave(ATOMIC_HASH(v), flags);

	v->counter = i;

	atomic_spin_unlock_irqrestore(ATOMIC_HASH(v), flags);
}

static __inline__ int atomic_read(const atomic_t *v)
{
	return v->counter;
}

/* exported interface */

#define atomic_add(i,v)	((void)(__atomic_add_return( ((int)i),(v))))
#define atomic_sub(i,v)	((void)(__atomic_add_return(-((int)i),(v))))
#define atomic_inc(v)	((void)(__atomic_add_return(   1,(v))))
#define atomic_dec(v)	((void)(__atomic_add_return(  -1,(v))))

#define atomic_add_return(i,v)	(__atomic_add_return( ((int)i),(v)))
#define atomic_sub_return(i,v)	(__atomic_add_return(-((int)i),(v)))
#define atomic_inc_return(v)	(__atomic_add_return(   1,(v)))
#define atomic_dec_return(v)	(__atomic_add_return(  -1,(v)))

#define atomic_add_negative(a, v)	(atomic_add_return((a), (v)) < 0)

/*
 * atomic_inc_and_test - increment and test
 * @v: pointer of type atomic_t
 *
 * Atomically increments @v by 1
 * and returns true if the result is zero, or false for all
 * other cases.
 */
#define atomic_inc_and_test(v) (atomic_inc_return(v) == 0)

#define atomic_dec_and_test(v)	(atomic_dec_return(v) == 0)

#define ATOMIC_INIT(i)	{ (i) }

#define smp_mb__before_atomic_dec()	smp_mb()
#define smp_mb__after_atomic_dec()	smp_mb()
#define smp_mb__before_atomic_inc()	smp_mb()
#define smp_mb__after_atomic_inc()	smp_mb()

#endif
