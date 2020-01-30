/* $Id: atomic.h,v 1.21 2000/10/03 07:28:56 anton Exp $
 * atomic.h: Thankfully the V9 is at least reasonable for this
 *           stuff.
 *
 * Copyright (C) 1996, 1997, 2000 David S. Miller (davem@redhat.com)
 */

#ifndef __ARCH_SPARC64_ATOMIC__
#define __ARCH_SPARC64_ATOMIC__

typedef struct { volatile int counter; } atomic_t;
#define ATOMIC_INIT(i)	{ (i) }

#define atomic_read(v)		((v)->counter)
#define atomic_set(v, i)	(((v)->counter) = i)

extern int __atomic_add(int, atomic_t *);
extern int __atomic_sub(int, atomic_t *);

#define atomic_add(i, v) ((void)__atomic_add(i, v))
#define atomic_sub(i, v) ((void)__atomic_sub(i, v))

#define atomic_dec_return(v) __atomic_sub(1, v)
#define atomic_inc_return(v) __atomic_add(1, v)

#define atomic_sub_and_test(i, v) (__atomic_sub(i, v) == 0)
#define atomic_dec_and_test(v) (__atomic_sub(1, v) == 0)

#define atomic_inc(v) ((void)__atomic_add(1, v))
#define atomic_dec(v) ((void)__atomic_sub(1, v))

#endif /* !(__ARCH_SPARC64_ATOMIC__) */
