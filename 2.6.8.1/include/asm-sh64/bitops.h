#ifndef __ASM_SH64_BITOPS_H
#define __ASM_SH64_BITOPS_H

/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * include/asm-sh64/bitops.h
 *
 * Copyright (C) 2000, 2001  Paolo Alberelli
 * Copyright (C) 2003  Paul Mundt
 */

#ifdef __KERNEL__
#include <linux/compiler.h>
#include <asm/system.h>
/* For __swab32 */
#include <asm/byteorder.h>

static __inline__ void set_bit(int nr, volatile void * addr)
{
	int	mask;
	volatile unsigned int *a = addr;
	unsigned long flags;

	a += nr >> 5;
	mask = 1 << (nr & 0x1f);
	local_irq_save(flags);
	*a |= mask;
	local_irq_restore(flags);
}

static inline void __set_bit(int nr, void *addr)
{
	int	mask;
	unsigned int *a = addr;

	a += nr >> 5;
	mask = 1 << (nr & 0x1f);
	*a |= mask;
}

/*
 * clear_bit() doesn't provide any barrier for the compiler.
 */
#define smp_mb__before_clear_bit()	barrier()
#define smp_mb__after_clear_bit()	barrier()
static inline void clear_bit(int nr, volatile unsigned long *a)
{
	int	mask;
	unsigned long flags;

	a += nr >> 5;
	mask = 1 << (nr & 0x1f);
	local_irq_save(flags);
	*a &= ~mask;
	local_irq_restore(flags);
}

static inline void __clear_bit(int nr, volatile unsigned long *a)
{
	int	mask;

	a += nr >> 5;
	mask = 1 << (nr & 0x1f);
	*a &= ~mask;
}

static __inline__ void change_bit(int nr, volatile void * addr)
{
	int	mask;
	volatile unsigned int *a = addr;
	unsigned long flags;

	a += nr >> 5;
	mask = 1 << (nr & 0x1f);
	local_irq_save(flags);
	*a ^= mask;
	local_irq_restore(flags);
}

static __inline__ void __change_bit(int nr, volatile void * addr)
{
	int	mask;
	volatile unsigned int *a = addr;

	a += nr >> 5;
	mask = 1 << (nr & 0x1f);
	*a ^= mask;
}

static __inline__ int test_and_set_bit(int nr, volatile void * addr)
{
	int	mask, retval;
	volatile unsigned int *a = addr;
	unsigned long flags;

	a += nr >> 5;
	mask = 1 << (nr & 0x1f);
	local_irq_save(flags);
	retval = (mask & *a) != 0;
	*a |= mask;
	local_irq_restore(flags);

	return retval;
}

static __inline__ int __test_and_set_bit(int nr, volatile void * addr)
{
	int	mask, retval;
	volatile unsigned int *a = addr;

	a += nr >> 5;
	mask = 1 << (nr & 0x1f);
	retval = (mask & *a) != 0;
	*a |= mask;

	return retval;
}

static __inline__ int test_and_clear_bit(int nr, volatile void * addr)
{
	int	mask, retval;
	volatile unsigned int *a = addr;
	unsigned long flags;

	a += nr >> 5;
	mask = 1 << (nr & 0x1f);
	local_irq_save(flags);
	retval = (mask & *a) != 0;
	*a &= ~mask;
	local_irq_restore(flags);

	return retval;
}

static __inline__ int __test_and_clear_bit(int nr, volatile void * addr)
{
	int	mask, retval;
	volatile unsigned int *a = addr;

	a += nr >> 5;
	mask = 1 << (nr & 0x1f);
	retval = (mask & *a) != 0;
	*a &= ~mask;

	return retval;
}

static __inline__ int test_and_change_bit(int nr, volatile void * addr)
{
	int	mask, retval;
	volatile unsigned int *a = addr;
	unsigned long flags;

	a += nr >> 5;
	mask = 1 << (nr & 0x1f);
	local_irq_save(flags);
	retval = (mask & *a) != 0;
	*a ^= mask;
	local_irq_restore(flags);

	return retval;
}

static __inline__ int __test_and_change_bit(int nr, volatile void * addr)
{
	int	mask, retval;
	volatile unsigned int *a = addr;

	a += nr >> 5;
	mask = 1 << (nr & 0x1f);
	retval = (mask & *a) != 0;
	*a ^= mask;

	return retval;
}

static __inline__ int test_bit(int nr, const volatile void *addr)
{
	return 1UL & (((const volatile unsigned int *) addr)[nr >> 5] >> (nr & 31));
}

static __inline__ unsigned long ffz(unsigned long word)
{
	unsigned long result, __d2, __d3;

        __asm__("gettr  tr0, %2\n\t"
                "pta    $+32, tr0\n\t"
                "andi   %1, 1, %3\n\t"
                "beq    %3, r63, tr0\n\t"
                "pta    $+4, tr0\n"
                "0:\n\t"
                "shlri.l        %1, 1, %1\n\t"
                "addi   %0, 1, %0\n\t"
                "andi   %1, 1, %3\n\t"
                "beqi   %3, 1, tr0\n"
                "1:\n\t"
                "ptabs  %2, tr0\n\t"
                : "=r" (result), "=r" (word), "=r" (__d2), "=r" (__d3)
                : "0" (0L), "1" (word));

	return result;
}

/**
 * __ffs - find first bit in word
 * @word: The word to search
 *
 * Undefined if no bit exists, so code should check against 0 first.
 */
static inline unsigned long __ffs(unsigned long word)
{
	int r = 0;

	if (!word)
		return 0;
	if (!(word & 0xffff)) {
		word >>= 16;
		r += 16;
	}
	if (!(word & 0xff)) {
		word >>= 8;
		r += 8;
	}
	if (!(word & 0xf)) {
		word >>= 4;
		r += 4;
	}
	if (!(word & 3)) {
		word >>= 2;
		r += 2;
	}
	if (!(word & 1)) {
		word >>= 1;
		r += 1;
	}
	return r;
}

/**
 * find_next_bit - find the next set bit in a memory region
 * @addr: The address to base the search on
 * @offset: The bitnumber to start searching at
 * @size: The maximum size to search
 */
static inline unsigned long find_next_bit(const unsigned long *addr,
	unsigned long size, unsigned long offset)
{
	unsigned int *p = ((unsigned int *) addr) + (offset >> 5);
	unsigned int result = offset & ~31UL;
	unsigned int tmp;

	if (offset >= size)
		return size;
	size -= result;
	offset &= 31UL;
	if (offset) {
		tmp = *p++;
		tmp &= ~0UL << offset;
		if (size < 32)
			goto found_first;
		if (tmp)
			goto found_middle;
		size -= 32;
		result += 32;
	}
	while (size >= 32) {
		if ((tmp = *p++) != 0)
			goto found_middle;
		result += 32;
		size -= 32;
	}
	if (!size)
		return result;
	tmp = *p;

found_first:
	tmp &= ~0UL >> (32 - size);
	if (tmp == 0UL)        /* Are any bits set? */
		return result + size; /* Nope. */
found_middle:
	return result + __ffs(tmp);
}

/**
 * find_first_bit - find the first set bit in a memory region
 * @addr: The address to start the search at
 * @size: The maximum size to search
 *
 * Returns the bit-number of the first set bit, not the number of the byte
 * containing a bit.
 */
#define find_first_bit(addr, size) \
	find_next_bit((addr), (size), 0)


static inline int find_next_zero_bit(void *addr, int size, int offset)
{
	unsigned long *p = ((unsigned long *) addr) + (offset >> 5);
	unsigned long result = offset & ~31UL;
	unsigned long tmp;

	if (offset >= size)
		return size;
	size -= result;
	offset &= 31UL;
	if (offset) {
		tmp = *(p++);
		tmp |= ~0UL >> (32-offset);
		if (size < 32)
			goto found_first;
		if (~tmp)
			goto found_middle;
		size -= 32;
		result += 32;
	}
	while (size & ~31UL) {
		if (~(tmp = *(p++)))
			goto found_middle;
		result += 32;
		size -= 32;
	}
	if (!size)
		return result;
	tmp = *p;

found_first:
	tmp |= ~0UL << size;
found_middle:
	return result + ffz(tmp);
}

#define find_first_zero_bit(addr, size) \
        find_next_zero_bit((addr), (size), 0)

/*
 * hweightN: returns the hamming weight (i.e. the number
 * of bits set) of a N-bit word
 */

#define hweight32(x)	generic_hweight32(x)
#define hweight16(x)	generic_hweight16(x)
#define hweight8(x)	generic_hweight8(x)

/*
 * Every architecture must define this function. It's the fastest
 * way of searching a 140-bit bitmap where the first 100 bits are
 * unlikely to be set. It's guaranteed that at least one of the 140
 * bits is cleared.
 */

static inline int sched_find_first_bit(unsigned long *b)
{
	if (unlikely(b[0]))
		return __ffs(b[0]);
	if (unlikely(b[1]))
		return __ffs(b[1]) + 32;
	if (unlikely(b[2]))
		return __ffs(b[2]) + 64;
	if (b[3])
		return __ffs(b[3]) + 96;
	return __ffs(b[4]) + 128;
}

/*
 * ffs: find first bit set. This is defined the same way as
 * the libc and compiler builtin ffs routines, therefore
 * differs in spirit from the above ffz (man ffs).
 */

#define ffs(x) generic_ffs(x)

/*
 * hweightN: returns the hamming weight (i.e. the number
 * of bits set) of a N-bit word
 */

#define hweight32(x) generic_hweight32(x)
#define hweight16(x) generic_hweight16(x)
#define hweight8(x) generic_hweight8(x)

#ifdef __LITTLE_ENDIAN__
#define ext2_set_bit(nr, addr) test_and_set_bit((nr), (addr))
#define ext2_clear_bit(nr, addr) test_and_clear_bit((nr), (addr))
#define ext2_test_bit(nr, addr) test_bit((nr), (addr))
#define ext2_find_first_zero_bit(addr, size) find_first_zero_bit((addr), (size))
#define ext2_find_next_zero_bit(addr, size, offset) \
                find_next_zero_bit((addr), (size), (offset))
#else
static __inline__ int ext2_set_bit(int nr, volatile void * addr)
{
	int		mask, retval;
	unsigned long	flags;
	volatile unsigned char	*ADDR = (unsigned char *) addr;

	ADDR += nr >> 3;
	mask = 1 << (nr & 0x07);
	local_irq_save(flags);
	retval = (mask & *ADDR) != 0;
	*ADDR |= mask;
	local_irq_restore(flags);
	return retval;
}

static __inline__ int ext2_clear_bit(int nr, volatile void * addr)
{
	int		mask, retval;
	unsigned long	flags;
	volatile unsigned char	*ADDR = (unsigned char *) addr;

	ADDR += nr >> 3;
	mask = 1 << (nr & 0x07);
	local_irq_save(flags);
	retval = (mask & *ADDR) != 0;
	*ADDR &= ~mask;
	local_irq_restore(flags);
	return retval;
}

static __inline__ int ext2_test_bit(int nr, const volatile void * addr)
{
	int			mask;
	const volatile unsigned char	*ADDR = (const unsigned char *) addr;

	ADDR += nr >> 3;
	mask = 1 << (nr & 0x07);
	return ((mask & *ADDR) != 0);
}

#define ext2_find_first_zero_bit(addr, size) \
        ext2_find_next_zero_bit((addr), (size), 0)

static __inline__ unsigned long ext2_find_next_zero_bit(void *addr, unsigned long size, unsigned long offset)
{
	unsigned long *p = ((unsigned long *) addr) + (offset >> 5);
	unsigned long result = offset & ~31UL;
	unsigned long tmp;

	if (offset >= size)
		return size;
	size -= result;
	offset &= 31UL;
	if(offset) {
		/* We hold the little endian value in tmp, but then the
		 * shift is illegal. So we could keep a big endian value
		 * in tmp, like this:
		 *
		 * tmp = __swab32(*(p++));
		 * tmp |= ~0UL >> (32-offset);
		 *
		 * but this would decrease preformance, so we change the
		 * shift:
		 */
		tmp = *(p++);
		tmp |= __swab32(~0UL >> (32-offset));
		if(size < 32)
			goto found_first;
		if(~tmp)
			goto found_middle;
		size -= 32;
		result += 32;
	}
	while(size & ~31UL) {
		if(~(tmp = *(p++)))
			goto found_middle;
		result += 32;
		size -= 32;
	}
	if(!size)
		return result;
	tmp = *p;

found_first:
	/* tmp is little endian, so we would have to swab the shift,
	 * see above. But then we have to swab tmp below for ffz, so
	 * we might as well do this here.
	 */
	return result + ffz(__swab32(tmp) | (~0UL << size));
found_middle:
	return result + ffz(__swab32(tmp));
}
#endif

#define ext2_set_bit_atomic(lock, nr, addr)		\
	({						\
		int ret;				\
		spin_lock(lock);			\
		ret = ext2_set_bit((nr), (addr));	\
		spin_unlock(lock);			\
		ret;					\
	})

#define ext2_clear_bit_atomic(lock, nr, addr)		\
	({						\
		int ret;				\
		spin_lock(lock);			\
		ret = ext2_clear_bit((nr), (addr));	\
		spin_unlock(lock);			\
		ret;					\
	})

/* Bitmap functions for the minix filesystem.  */
#define minix_test_and_set_bit(nr,addr) test_and_set_bit(nr,addr)
#define minix_set_bit(nr,addr) set_bit(nr,addr)
#define minix_test_and_clear_bit(nr,addr) test_and_clear_bit(nr,addr)
#define minix_test_bit(nr,addr) test_bit(nr,addr)
#define minix_find_first_zero_bit(addr,size) find_first_zero_bit(addr,size)

#define ffs(x)	generic_ffs(x)
#define fls(x)	generic_fls(x)

#endif /* __KERNEL__ */

#endif /* __ASM_SH64_BITOPS_H */
