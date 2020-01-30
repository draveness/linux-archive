#ifndef _ASM_IA64_SPINLOCK_H
#define _ASM_IA64_SPINLOCK_H

/*
 * Copyright (C) 1998-2000 Hewlett-Packard Co
 * Copyright (C) 1998-2000 David Mosberger-Tang <davidm@hpl.hp.com>
 * Copyright (C) 1999 Walt Drummond <drummond@valinux.com>
 *
 * This file is used for SMP configurations only.
 */

#include <linux/kernel.h>

#include <asm/system.h>
#include <asm/bitops.h>
#include <asm/atomic.h>

#undef NEW_LOCK

#ifdef NEW_LOCK

typedef struct { 
	volatile unsigned int lock;
} spinlock_t;

#define SPIN_LOCK_UNLOCKED			(spinlock_t) { 0 }
#define spin_lock_init(x)			((x)->lock = 0) 

/*
 * Streamlined test_and_set_bit(0, (x)).  We use test-and-test-and-set
 * rather than a simple xchg to avoid writing the cache-line when
 * there is contention.
 */
#define spin_lock(x)									\
{											\
	register char *addr __asm__ ("r31") = (char *) &(x)->lock;			\
											\
	__asm__ __volatile__ (								\
		"mov r30=1\n"								\
		"mov ar.ccv=r0\n"							\
		";;\n"									\
		IA64_SEMFIX"cmpxchg4.acq r30=[%0],r30,ar.ccv\n"				\
		";;\n"									\
		"cmp.ne p15,p0=r30,r0\n"						\
		"(p15) br.call.spnt.few b7=ia64_spinlock_contention\n"			\
		";;\n"									\
		"1:\n"				/* force a new bundle */		\
		:: "r"(addr)								\
		: "ar.ccv", "ar.pfs", "b7", "p15", "r28", "r29", "r30", "memory");	\
}

#define spin_trylock(x)									\
({											\
	register long result;								\
											\
	__asm__ __volatile__ (								\
		"mov ar.ccv=r0\n"							\
		";;\n"									\
		IA64_SEMFIX"cmpxchg4.acq %0=[%2],%1,ar.ccv\n"				\
		: "=r"(result) : "r"(1), "r"(&(x)->lock) : "ar.ccv", "memory");		\
	(result == 0);									\
})

#define spin_is_locked(x)	((x)->lock != 0)
#define spin_unlock(x)		do {((spinlock_t *) x)->lock = 0;} while (0)
#define spin_unlock_wait(x)	do {} while ((x)->lock)

#else /* !NEW_LOCK */

typedef struct { 
	volatile unsigned int lock;
} spinlock_t;

#define SPIN_LOCK_UNLOCKED			(spinlock_t) { 0 }
#define spin_lock_init(x)			((x)->lock = 0)

/*
 * Streamlined test_and_set_bit(0, (x)).  We use test-and-test-and-set
 * rather than a simple xchg to avoid writing the cache-line when
 * there is contention.
 */
#define spin_lock(x) __asm__ __volatile__ (			\
	"mov ar.ccv = r0\n"					\
	"mov r29 = 1\n"						\
	";;\n"							\
	"1:\n"							\
	"ld4 r2 = [%0]\n"					\
	";;\n"							\
	"cmp4.eq p0,p7 = r0,r2\n"				\
	"(p7) br.cond.spnt.few 1b \n"				\
	IA64_SEMFIX"cmpxchg4.acq r2 = [%0], r29, ar.ccv\n"	\
	";;\n"							\
	"cmp4.eq p0,p7 = r0, r2\n"				\
	"(p7) br.cond.spnt.few 1b\n"				\
	";;\n"							\
	:: "r"(&(x)->lock) : "r2", "r29", "memory")

#define spin_is_locked(x)	((x)->lock != 0)
#define spin_unlock(x)		do {((spinlock_t *) x)->lock = 0; barrier(); } while (0)
#define spin_trylock(x)		(cmpxchg_acq(&(x)->lock, 0, 1) == 0)
#define spin_unlock_wait(x)	do { barrier(); } while ((x)->lock)

#endif /* !NEW_LOCK */

typedef struct {
	volatile int read_counter:31;
	volatile int write_lock:1;
} rwlock_t;
#define RW_LOCK_UNLOCKED (rwlock_t) { 0, 0 }

#define read_lock(rw)								\
do {										\
	int tmp = 0;								\
	__asm__ __volatile__ ("1:\t"IA64_SEMFIX"fetchadd4.acq %0 = [%1], 1\n"	\
			      ";;\n"						\
			      "tbit.nz p6,p0 = %0, 31\n"			\
			      "(p6) br.cond.sptk.few 2f\n"			\
			      ".section .text.lock,\"ax\"\n"			\
			      "2:\t"IA64_SEMFIX"fetchadd4.rel %0 = [%1], -1\n"	\
			      ";;\n"						\
			      "3:\tld4.acq %0 = [%1]\n"				\
			      ";;\n"						\
			      "tbit.nz p6,p0 = %0, 31\n"			\
			      "(p6) br.cond.sptk.few 3b\n"			\
			      "br.cond.sptk.few 1b\n"				\
			      ";;\n"						\
			      ".previous\n"					\
			      : "=&r" (tmp)					\
			      : "r" (rw): "memory");				\
} while(0)

#define read_unlock(rw)								\
do {										\
	int tmp = 0;								\
	__asm__ __volatile__ (IA64_SEMFIX"fetchadd4.rel %0 = [%1], -1\n"	\
			      : "=r" (tmp)					\
			      : "r" (rw)					\
			      : "memory");					\
} while(0)

#define write_lock(rw)								\
do {										\
 	__asm__ __volatile__ (							\
		"mov ar.ccv = r0\n"						\
		"movl r29 = 0x80000000\n"					\
		";;\n"								\
		"1:\n"								\
		"ld4 r2 = [%0]\n"						\
		";;\n"								\
		"cmp4.eq p0,p7 = r0,r2\n"					\
		"(p7) br.cond.spnt.few 1b \n"					\
		IA64_SEMFIX"cmpxchg4.acq r2 = [%0], r29, ar.ccv\n"		\
		";;\n"								\
		"cmp4.eq p0,p7 = r0, r2\n"					\
		"(p7) br.cond.spnt.few 1b\n"					\
		";;\n"								\
		:: "r"(rw) : "r2", "r29", "memory");				\
} while(0)

/*
 * clear_bit() has "acq" semantics; we're really need "rel" semantics,
 * but for simplicity, we simply do a fence for now...
 */
#define write_unlock(x)				({clear_bit(31, (x)); mb();})

#endif /*  _ASM_IA64_SPINLOCK_H */
