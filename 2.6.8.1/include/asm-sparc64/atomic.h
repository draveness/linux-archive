/* $Id: atomic.h,v 1.22 2001/07/11 23:56:07 davem Exp $
 * atomic.h: Thankfully the V9 is at least reasonable for this
 *           stuff.
 *
 * Copyright (C) 1996, 1997, 2000 David S. Miller (davem@redhat.com)
 */

#ifndef __ARCH_SPARC64_ATOMIC__
#define __ARCH_SPARC64_ATOMIC__

#include <linux/types.h>

typedef struct { volatile int counter; } atomic_t;
typedef struct { volatile __s64 counter; } atomic64_t;

#define ATOMIC_INIT(i)		{ (i) }
#define ATOMIC64_INIT(i)	{ (i) }

#define atomic_read(v)		((v)->counter)
#define atomic64_read(v)	((v)->counter)

#define atomic_set(v, i)	(((v)->counter) = i)
#define atomic64_set(v, i)	(((v)->counter) = i)

extern int __atomic_add(int, atomic_t *);
extern int __atomic64_add(__s64, atomic64_t *);

extern int __atomic_sub(int, atomic_t *);
extern int __atomic64_sub(__s64, atomic64_t *);

#define atomic_add(i, v) ((void)__atomic_add(i, v))
#define atomic64_add(i, v) ((void)__atomic64_add(i, v))

#define atomic_sub(i, v) ((void)__atomic_sub(i, v))
#define atomic64_sub(i, v) ((void)__atomic64_sub(i, v))

#define atomic_dec_return(v) __atomic_sub(1, v)
#define atomic64_dec_return(v) __atomic64_sub(1, v)

#define atomic_inc_return(v) __atomic_add(1, v)
#define atomic64_inc_return(v) __atomic64_add(1, v)

/*
 * atomic_inc_and_test - increment and test
 * @v: pointer of type atomic_t
 *
 * Atomically increments @v by 1
 * and returns true if the result is zero, or false for all
 * other cases.
 */
#define atomic_inc_and_test(v) (atomic_inc_return(v) == 0)

#define atomic_sub_and_test(i, v) (__atomic_sub(i, v) == 0)
#define atomic64_sub_and_test(i, v) (__atomic64_sub(i, v) == 0)

#define atomic_dec_and_test(v) (__atomic_sub(1, v) == 0)
#define atomic64_dec_and_test(v) (__atomic64_sub(1, v) == 0)

#define atomic_inc(v) ((void)__atomic_add(1, v))
#define atomic64_inc(v) ((void)__atomic64_add(1, v))

#define atomic_dec(v) ((void)__atomic_sub(1, v))
#define atomic64_dec(v) ((void)__atomic64_sub(1, v))

#define atomic_add_negative(i, v) (__atomic_add(i, v) < 0)
#define atomic64_add_negative(i, v) (__atomic64_add(i, v) < 0)

/* Atomic operations are already serializing */
#define smp_mb__before_atomic_dec()	barrier()
#define smp_mb__after_atomic_dec()	barrier()
#define smp_mb__before_atomic_inc()	barrier()
#define smp_mb__after_atomic_inc()	barrier()

#endif /* !(__ARCH_SPARC64_ATOMIC__) */
