#ifndef _X86_64_BITOPS_H
#define _X86_64_BITOPS_H

/*
 * Copyright 1992, Linus Torvalds.
 */

#include <linux/config.h>

#ifdef CONFIG_SMP
#define LOCK_PREFIX "lock ; "
#else
#define LOCK_PREFIX ""
#endif

#define ADDR (*(volatile long *) addr)

/**
 * set_bit - Atomically set a bit in memory
 * @nr: the bit to set
 * @addr: the address to start counting from
 *
 * This function is atomic and may not be reordered.  See __set_bit()
 * if you do not require the atomic guarantees.
 * Note that @nr may be almost arbitrarily large; this function is not
 * restricted to acting on a single-word quantity.
 */
static __inline__ void set_bit(long nr, volatile void * addr)
{
	__asm__ __volatile__( LOCK_PREFIX
		"btsq %1,%0"
		:"=m" (ADDR)
		:"dIr" (nr) : "memory");
}

/**
 * __set_bit - Set a bit in memory
 * @nr: the bit to set
 * @addr: the address to start counting from
 *
 * Unlike set_bit(), this function is non-atomic and may be reordered.
 * If it's called on the same region of memory simultaneously, the effect
 * may be that only one operation succeeds.
 */
static __inline__ void __set_bit(int nr, volatile void * addr)
{
	__asm__ volatile(
		"btsl %1,%0"
		:"=m" (ADDR)
		:"dIr" (nr) : "memory");
}

/**
 * clear_bit - Clears a bit in memory
 * @nr: Bit to clear
 * @addr: Address to start counting from
 *
 * clear_bit() is atomic and may not be reordered.  However, it does
 * not contain a memory barrier, so if it is used for locking purposes,
 * you should call smp_mb__before_clear_bit() and/or smp_mb__after_clear_bit()
 * in order to ensure changes are visible on other processors.
 */
static __inline__ void clear_bit(int nr, volatile void * addr)
{
	__asm__ __volatile__( LOCK_PREFIX
		"btrl %1,%0"
		:"=m" (ADDR)
		:"dIr" (nr));
}

static __inline__ void __clear_bit(int nr, volatile void * addr)
{
	__asm__ __volatile__(
		"btrl %1,%0"
		:"=m" (ADDR)
		:"dIr" (nr));
}

#define smp_mb__before_clear_bit()	barrier()
#define smp_mb__after_clear_bit()	barrier()

/**
 * __change_bit - Toggle a bit in memory
 * @nr: the bit to change
 * @addr: the address to start counting from
 *
 * Unlike change_bit(), this function is non-atomic and may be reordered.
 * If it's called on the same region of memory simultaneously, the effect
 * may be that only one operation succeeds.
 */
static __inline__ void __change_bit(int nr, volatile void * addr)
{
	__asm__ __volatile__(
		"btcl %1,%0"
		:"=m" (ADDR)
		:"dIr" (nr));
}

/**
 * change_bit - Toggle a bit in memory
 * @nr: Bit to change
 * @addr: Address to start counting from
 *
 * change_bit() is atomic and may not be reordered.
 * Note that @nr may be almost arbitrarily large; this function is not
 * restricted to acting on a single-word quantity.
 */
static __inline__ void change_bit(int nr, volatile void * addr)
{
	__asm__ __volatile__( LOCK_PREFIX
		"btcl %1,%0"
		:"=m" (ADDR)
		:"dIr" (nr));
}

/**
 * test_and_set_bit - Set a bit and return its old value
 * @nr: Bit to set
 * @addr: Address to count from
 *
 * This operation is atomic and cannot be reordered.  
 * It also implies a memory barrier.
 */
static __inline__ int test_and_set_bit(int nr, volatile void * addr)
{
	int oldbit;

	__asm__ __volatile__( LOCK_PREFIX
		"btsl %2,%1\n\tsbbl %0,%0"
		:"=r" (oldbit),"=m" (ADDR)
		:"dIr" (nr) : "memory");
	return oldbit;
}

/**
 * __test_and_set_bit - Set a bit and return its old value
 * @nr: Bit to set
 * @addr: Address to count from
 *
 * This operation is non-atomic and can be reordered.  
 * If two examples of this operation race, one can appear to succeed
 * but actually fail.  You must protect multiple accesses with a lock.
 */
static __inline__ int __test_and_set_bit(int nr, volatile void * addr)
{
	int oldbit;

	__asm__(
		"btsl %2,%1\n\tsbbl %0,%0"
		:"=r" (oldbit),"=m" (ADDR)
		:"dIr" (nr));
	return oldbit;
}

/**
 * test_and_clear_bit - Clear a bit and return its old value
 * @nr: Bit to clear
 * @addr: Address to count from
 *
 * This operation is atomic and cannot be reordered.  
 * It also implies a memory barrier.
 */
static __inline__ int test_and_clear_bit(int nr, volatile void * addr)
{
	int oldbit;

	__asm__ __volatile__( LOCK_PREFIX
		"btrl %2,%1\n\tsbbl %0,%0"
		:"=r" (oldbit),"=m" (ADDR)
		:"dIr" (nr) : "memory");
	return oldbit;
}

/**
 * __test_and_clear_bit - Clear a bit and return its old value
 * @nr: Bit to clear
 * @addr: Address to count from
 *
 * This operation is non-atomic and can be reordered.  
 * If two examples of this operation race, one can appear to succeed
 * but actually fail.  You must protect multiple accesses with a lock.
 */
static __inline__ int __test_and_clear_bit(int nr, volatile void * addr)
{
	int oldbit;

	__asm__(
		"btrl %2,%1\n\tsbbl %0,%0"
		:"=r" (oldbit),"=m" (ADDR)
		:"dIr" (nr));
	return oldbit;
}

/* WARNING: non atomic and it can be reordered! */
static __inline__ int __test_and_change_bit(int nr, volatile void * addr)
{
	int oldbit;

	__asm__ __volatile__(
		"btcl %2,%1\n\tsbbl %0,%0"
		:"=r" (oldbit),"=m" (ADDR)
		:"dIr" (nr) : "memory");
	return oldbit;
}

/**
 * test_and_change_bit - Change a bit and return its old value
 * @nr: Bit to change
 * @addr: Address to count from
 *
 * This operation is atomic and cannot be reordered.  
 * It also implies a memory barrier.
 */
static __inline__ int test_and_change_bit(int nr, volatile void * addr)
{
	int oldbit;

	__asm__ __volatile__( LOCK_PREFIX
		"btcl %2,%1\n\tsbbl %0,%0"
		:"=r" (oldbit),"=m" (ADDR)
		:"dIr" (nr) : "memory");
	return oldbit;
}

#if 0 /* Fool kernel-doc since it doesn't do macros yet */
/**
 * test_bit - Determine whether a bit is set
 * @nr: bit number to test
 * @addr: Address to start counting from
 */
static int test_bit(int nr, const volatile void * addr);
#endif

static __inline__ int constant_test_bit(int nr, const volatile void * addr)
{
	return ((1UL << (nr & 31)) & (((const volatile unsigned int *) addr)[nr >> 5])) != 0;
}

static __inline__ int variable_test_bit(int nr, volatile const void * addr)
{
	int oldbit;

	__asm__ __volatile__(
		"btl %2,%1\n\tsbbl %0,%0"
		:"=r" (oldbit)
		:"m" (ADDR),"dIr" (nr));
	return oldbit;
}

#define test_bit(nr,addr) \
(__builtin_constant_p(nr) ? \
 constant_test_bit((nr),(addr)) : \
 variable_test_bit((nr),(addr)))

#undef ADDR

/**
 * find_first_zero_bit - find the first zero bit in a memory region
 * @addr: The address to start the search at
 * @size: The maximum size to search
 *
 * Returns the bit-number of the first zero bit, not the number of the byte
 * containing a bit.
 */
static __inline__ int find_first_zero_bit(const unsigned long * addr, unsigned size)
{
	int d0, d1, d2;
	int res;

	if (!size)
		return 0;
	__asm__ __volatile__(
		"movl $-1,%%eax\n\t"
		"xorl %%edx,%%edx\n\t"
		"repe; scasl\n\t"
		"je 1f\n\t"
		"xorl -4(%%rdi),%%eax\n\t"
		"subq $4,%%rdi\n\t"
		"bsfl %%eax,%%edx\n"
		"1:\tsubq %%rbx,%%rdi\n\t"
		"shlq $3,%%rdi\n\t"
		"addq %%rdi,%%rdx"
		:"=d" (res), "=&c" (d0), "=&D" (d1), "=&a" (d2)
		:"1" ((size + 31) >> 5), "2" (addr), "b" (addr) : "memory");
	return res;
}

/**
 * find_next_zero_bit - find the first zero bit in a memory region
 * @addr: The address to base the search on
 * @offset: The bitnumber to start searching at
 * @size: The maximum size to search
 */
static __inline__ int find_next_zero_bit (const unsigned long * addr, int size, int offset)
{
	unsigned long * p = ((unsigned long *) addr) + (offset >> 6);
	unsigned long set = 0;
	unsigned long res, bit = offset&63;
	
	if (bit) {
		/*
		 * Look for zero in first word
		 */
		__asm__("bsfq %1,%0\n\t"
			"cmoveq %2,%0"
			: "=r" (set)
			: "r" (~(*p >> bit)), "r"(64L));
		if (set < (64 - bit))
			return set + offset;
		set = 64 - bit;
		p++;
	}
	/*
	 * No zero yet, search remaining full words for a zero
	 */
	res = find_first_zero_bit ((const unsigned long *)p, size - 64 * (p - (unsigned long *) addr));
	return (offset + set + res);
}


/**
 * find_first_bit - find the first set bit in a memory region
 * @addr: The address to start the search at
 * @size: The maximum size to search
 *
 * Returns the bit-number of the first set bit, not the number of the byte
 * containing a bit.
 */
static __inline__ int find_first_bit(const unsigned long * addr, unsigned size)
{
	int d0, d1;
	int res;

	/* This looks at memory. Mark it volatile to tell gcc not to move it around */
	__asm__ __volatile__(
		"xorl %%eax,%%eax\n\t"
		"repe; scasl\n\t"
		"jz 1f\n\t"
		"leaq -4(%%rdi),%%rdi\n\t"
		"bsfl (%%rdi),%%eax\n"
		"1:\tsubq %%rbx,%%rdi\n\t"
		"shll $3,%%edi\n\t"
		"addl %%edi,%%eax"
		:"=a" (res), "=&c" (d0), "=&D" (d1)
		:"1" ((size + 31) >> 5), "2" (addr), "b" (addr) : "memory");
	return res;
}

/**
 * find_next_bit - find the first set bit in a memory region
 * @addr: The address to base the search on
 * @offset: The bitnumber to start searching at
 * @size: The maximum size to search
 */
static __inline__ int find_next_bit(const unsigned long * addr, int size, int offset)
{
	const unsigned long * p = addr + (offset >> 6);
	unsigned long set = 0, bit = offset & 63, res;
	
	if (bit) {
		/*
		 * Look for nonzero in the first 64 bits:
		 */
		__asm__("bsfq %1,%0\n\t"
			"cmoveq %2,%0\n\t"
			: "=r" (set)
			: "r" (*p >> bit), "r" (64L));
		if (set < (64 - bit))
			return set + offset;
		set = 64 - bit;
		p++;
	}
	/*
	 * No set bit yet, search remaining full words for a bit
	 */
	res = find_first_bit (p, size - 64 * (p - addr));
	return (offset + set + res);
}

/* 
 * Find string of zero bits in a bitmap. -1 when not found.
 */ 
extern unsigned long 
find_next_zero_string(unsigned long *bitmap, long start, long nbits, int len);

static inline void set_bit_string(unsigned long *bitmap, unsigned long i, 
				  int len) 
{ 
	unsigned long end = i + len; 
	while (i < end) {
		__set_bit(i, bitmap); 
		i++;
	}
} 

static inline void __clear_bit_string(unsigned long *bitmap, unsigned long i, 
				    int len) 
{ 
	unsigned long end = i + len; 
	while (i < end) {
		__clear_bit(i, bitmap); 
		i++;
	}
} 

/**
 * ffz - find first zero in word.
 * @word: The word to search
 *
 * Undefined if no zero exists, so code should check against ~0UL first.
 */
static __inline__ unsigned long ffz(unsigned long word)
{
	__asm__("bsfq %1,%0"
		:"=r" (word)
		:"r" (~word));
	return word;
}

/**
 * __ffs - find first bit in word.
 * @word: The word to search
 *
 * Undefined if no bit exists, so code should check against 0 first.
 */
static __inline__ unsigned long __ffs(unsigned long word)
{
	__asm__("bsfq %1,%0"
		:"=r" (word)
		:"rm" (word));
	return word;
}

#ifdef __KERNEL__

static inline int sched_find_first_bit(const unsigned long *b)
{
	if (b[0])
		return __ffs(b[0]);
	if (b[1])
		return __ffs(b[1]) + 64;
	if (b[2])
		return __ffs(b[2]) + 128;
}

/**
 * ffs - find first bit set
 * @x: the word to search
 *
 * This is defined the same way as
 * the libc and compiler builtin ffs routines, therefore
 * differs in spirit from the above ffz (man ffs).
 */
static __inline__ int ffs(int x)
{
	int r;

	__asm__("bsfl %1,%0\n\t"
		"cmovzl %2,%0" 
		: "=r" (r) : "rm" (x), "r" (-1));
	return r+1;
}

/**
 * hweightN - returns the hamming weight of a N-bit word
 * @x: the word to weigh
 *
 * The Hamming Weight of a number is the total number of bits set in it.
 */

#define hweight64(x) generic_hweight64(x)
#define hweight32(x) generic_hweight32(x)
#define hweight16(x) generic_hweight16(x)
#define hweight8(x) generic_hweight8(x)

#endif /* __KERNEL__ */

#ifdef __KERNEL__

#define ext2_set_bit(nr,addr) \
	__test_and_set_bit((nr),(unsigned long*)addr)
#define ext2_set_bit_atomic(lock,nr,addr) \
	        test_and_set_bit((nr),(unsigned long*)addr)
#define ext2_clear_bit(nr, addr) \
	__test_and_clear_bit((nr),(unsigned long*)addr)
#define ext2_clear_bit_atomic(lock,nr,addr) \
	        test_and_clear_bit((nr),(unsigned long*)addr)
#define ext2_test_bit(nr, addr)      test_bit((nr),(unsigned long*)addr)
#define ext2_find_first_zero_bit(addr, size) \
	find_first_zero_bit((unsigned long*)addr, size)
#define ext2_find_next_zero_bit(addr, size, off) \
	find_next_zero_bit((unsigned long*)addr, size, off)

/* Bitmap functions for the minix filesystem.  */
#define minix_test_and_set_bit(nr,addr) __test_and_set_bit(nr,(void*)addr)
#define minix_set_bit(nr,addr) __set_bit(nr,(void*)addr)
#define minix_test_and_clear_bit(nr,addr) __test_and_clear_bit(nr,(void*)addr)
#define minix_test_bit(nr,addr) test_bit(nr,(void*)addr)
#define minix_find_first_zero_bit(addr,size) \
	find_first_zero_bit((void*)addr,size)

/* find last set bit */
#define fls(x) generic_fls(x)

#define ARCH_HAS_ATOMIC_UNSIGNED 1

#endif /* __KERNEL__ */

#endif /* _X86_64_BITOPS_H */
