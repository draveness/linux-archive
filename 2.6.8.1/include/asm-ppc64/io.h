#ifndef _PPC64_IO_H
#define _PPC64_IO_H

/* 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/config.h>
#include <asm/page.h>
#include <asm/byteorder.h>
#ifdef CONFIG_PPC_ISERIES 
#include <asm/iSeries/iSeries_io.h>
#endif  
#include <asm/memory.h>
#include <asm/delay.h>

#define __ide_mm_insw(p, a, c) _insw_ns((volatile u16 *)(p), (a), (c))
#define __ide_mm_insl(p, a, c) _insl_ns((volatile u32 *)(p), (a), (c))
#define __ide_mm_outsw(p, a, c) _outsw_ns((volatile u16 *)(p), (a), (c))
#define __ide_mm_outsl(p, a, c) _outsl_ns((volatile u32 *)(p), (a), (c))


#define SIO_CONFIG_RA	0x398
#define SIO_CONFIG_RD	0x399

#define SLOW_DOWN_IO

extern unsigned long isa_io_base;
extern unsigned long pci_io_base;

#ifdef CONFIG_PPC_ISERIES
/* __raw_* accessors aren't supported on iSeries */
#define __raw_readb(addr)	{ BUG(); 0; }
#define __raw_readw(addr)       { BUG(); 0; }
#define __raw_readl(addr)       { BUG(); 0; }
#define __raw_readq(addr)       { BUG(); 0; }
#define __raw_writeb(v, addr)   { BUG(); 0; }
#define __raw_writew(v, addr)   { BUG(); 0; }
#define __raw_writel(v, addr)   { BUG(); 0; }
#define __raw_writeq(v, addr)   { BUG(); 0; }
#define readb(addr)		iSeries_Read_Byte((void*)(addr))  
#define readw(addr)		iSeries_Read_Word((void*)(addr))  
#define readl(addr)		iSeries_Read_Long((void*)(addr))
#define writeb(data, addr)	iSeries_Write_Byte(data,((void*)(addr)))
#define writew(data, addr)	iSeries_Write_Word(data,((void*)(addr)))
#define writel(data, addr)	iSeries_Write_Long(data,((void*)(addr)))
#define memset_io(a,b,c)	iSeries_memset_io((void *)(a),(b),(c))
#define memcpy_fromio(a,b,c)	iSeries_memcpy_fromio((void *)(a), (void *)(b), (c))
#define memcpy_toio(a,b,c)	iSeries_memcpy_toio((void *)(a), (void *)(b), (c))
#define inb(addr)		readb(((unsigned long)(addr)))  
#define inw(addr)		readw(((unsigned long)(addr)))  
#define inl(addr)		readl(((unsigned long)(addr)))
#define outb(data,addr)		writeb(data,((unsigned long)(addr)))  
#define outw(data,addr)		writew(data,((unsigned long)(addr)))  
#define outl(data,addr)		writel(data,((unsigned long)(addr)))
/*
 * The *_ns versions below don't do byte-swapping.
 * Neither do the standard versions now, these are just here
 * for older code.
 */
#define insw_ns(port, buf, ns)	_insw_ns((u16 *)((port)+pci_io_base), (buf), (ns))
#define insl_ns(port, buf, nl)	_insl_ns((u32 *)((port)+pci_io_base), (buf), (nl))
#else
#define __raw_readb(addr)       (*(volatile unsigned char *)(addr))
#define __raw_readw(addr)       (*(volatile unsigned short *)(addr))
#define __raw_readl(addr)       (*(volatile unsigned int *)(addr))
#define __raw_readq(addr)       (*(volatile unsigned long *)(addr))
#define __raw_writeb(v, addr)   (*(volatile unsigned char *)(addr) = (v))
#define __raw_writew(v, addr)   (*(volatile unsigned short *)(addr) = (v))
#define __raw_writel(v, addr)   (*(volatile unsigned int *)(addr) = (v))
#define __raw_writeq(v, addr)   (*(volatile unsigned long *)(addr) = (v))
#define readb(addr)		eeh_readb((void*)(addr))  
#define readw(addr)		eeh_readw((void*)(addr))  
#define readl(addr)		eeh_readl((void*)(addr))
#define readq(addr)		eeh_readq((void*)(addr))
#define writeb(data, addr)	eeh_writeb((data), ((void*)(addr)))
#define writew(data, addr)	eeh_writew((data), ((void*)(addr)))
#define writel(data, addr)	eeh_writel((data), ((void*)(addr)))
#define writeq(data, addr)	eeh_writeq((data), ((void*)(addr)))
#define memset_io(a,b,c)	eeh_memset_io((void *)(a),(b),(c))
#define memcpy_fromio(a,b,c)	eeh_memcpy_fromio((a),(void *)(b),(c))
#define memcpy_toio(a,b,c)	eeh_memcpy_toio((void *)(a),(b),(c))
#define inb(port)		eeh_inb((unsigned long)port)
#define outb(val, port)		eeh_outb(val, (unsigned long)port)
#define inw(port)		eeh_inw((unsigned long)port)
#define outw(val, port)		eeh_outw(val, (unsigned long)port)
#define inl(port)		eeh_inl((unsigned long)port)
#define outl(val, port)		eeh_outl(val, (unsigned long)port)

/*
 * The insw/outsw/insl/outsl macros don't do byte-swapping.
 * They are only used in practice for transferring buffers which
 * are arrays of bytes, and byte-swapping is not appropriate in
 * that case.  - paulus */
#define insb(port, buf, ns)	eeh_insb((port), (buf), (ns))
#define insw(port, buf, ns)	eeh_insw_ns((port), (buf), (ns))
#define insl(port, buf, nl)	eeh_insl_ns((port), (buf), (nl))
#define insw_ns(port, buf, ns)	eeh_insw_ns((port), (buf), (ns))
#define insl_ns(port, buf, nl)	eeh_insl_ns((port), (buf), (nl))

#define outsb(port, buf, ns)  _outsb((u8 *)((port)+pci_io_base), (buf), (ns))
#define outsw(port, buf, ns)  _outsw_ns((u16 *)((port)+pci_io_base), (buf), (ns))
#define outsl(port, buf, nl)  _outsl_ns((u32 *)((port)+pci_io_base), (buf), (nl))

#endif

#define readb_relaxed(addr) readb(addr)
#define readw_relaxed(addr) readw(addr)
#define readl_relaxed(addr) readl(addr)
#define readq_relaxed(addr) readq(addr)

extern void _insb(volatile u8 *port, void *buf, int ns);
extern void _outsb(volatile u8 *port, const void *buf, int ns);
extern void _insw(volatile u16 *port, void *buf, int ns);
extern void _outsw(volatile u16 *port, const void *buf, int ns);
extern void _insl(volatile u32 *port, void *buf, int nl);
extern void _outsl(volatile u32 *port, const void *buf, int nl);
extern void _insw_ns(volatile u16 *port, void *buf, int ns);
extern void _outsw_ns(volatile u16 *port, const void *buf, int ns);
extern void _insl_ns(volatile u32 *port, void *buf, int nl);
extern void _outsl_ns(volatile u32 *port, const void *buf, int nl);

/*
 * output pause versions need a delay at least for the
 * w83c105 ide controller in a p610.
 */
#define inb_p(port)             inb(port)
#define outb_p(val, port)       (udelay(1), outb((val), (port)))
#define inw_p(port)             inw(port)
#define outw_p(val, port)       (udelay(1), outw((val), (port)))
#define inl_p(port)             inl(port)
#define outl_p(val, port)       (udelay(1), outl((val), (port)))

/*
 * The *_ns versions below don't do byte-swapping.
 * Neither do the standard versions now, these are just here
 * for older code.
 */
#define outsw_ns(port, buf, ns)	_outsw_ns((u16 *)((port)+pci_io_base), (buf), (ns))
#define outsl_ns(port, buf, nl)	_outsl_ns((u32 *)((port)+pci_io_base), (buf), (nl))


#define IO_SPACE_LIMIT ~(0UL)


#ifdef __KERNEL__
extern int __ioremap_explicit(unsigned long p_addr, unsigned long v_addr,
		     	      unsigned long size, unsigned long flags);
extern void *__ioremap(unsigned long address, unsigned long size,
		       unsigned long flags);

/**
 * ioremap     -   map bus memory into CPU space
 * @address:   bus address of the memory
 * @size:      size of the resource to map
 *
 * ioremap performs a platform specific sequence of operations to
 * make bus memory CPU accessible via the readb/readw/readl/writeb/
 * writew/writel functions and the other mmio helpers. The returned
 * address is not guaranteed to be usable directly as a virtual
 * address.
 */
extern void *ioremap(unsigned long address, unsigned long size);

#define ioremap_nocache(addr, size)	ioremap((addr), (size))
extern int iounmap_explicit(void *addr, unsigned long size);
extern void iounmap(void *addr);
extern void * reserve_phb_iospace(unsigned long size);

/**
 *	virt_to_phys	-	map virtual addresses to physical
 *	@address: address to remap
 *
 *	The returned physical address is the physical (CPU) mapping for
 *	the memory address given. It is only valid to use this function on
 *	addresses directly mapped or allocated via kmalloc.
 *
 *	This function does not give bus mappings for DMA transfers. In
 *	almost all conceivable cases a device driver should not be using
 *	this function
 */
static inline unsigned long virt_to_phys(volatile void * address)
{
	return __pa((unsigned long)address);
}

/**
 *	phys_to_virt	-	map physical address to virtual
 *	@address: address to remap
 *
 *	The returned virtual address is a current CPU mapping for
 *	the memory address given. It is only valid to use this function on
 *	addresses that have a kernel mapping
 *
 *	This function does not handle bus mappings for DMA transfers. In
 *	almost all conceivable cases a device driver should not be using
 *	this function
 */
static inline void * phys_to_virt(unsigned long address)
{
	return (void *)__va(address);
}

/*
 * Change "struct page" to physical address.
 */
#define page_to_phys(page)	(page_to_pfn(page) << PAGE_SHIFT)

/* We do NOT want virtual merging, it would put too much pressure on
 * our iommu allocator. Instead, we want drivers to be smart enough
 * to coalesce sglists that happen to have been mapped in a contiguous
 * way by the iommu
 */
#define BIO_VMERGE_BOUNDARY	0

#endif /* __KERNEL__ */

static inline void iosync(void)
{
        __asm__ __volatile__ ("sync" : : : "memory");
}

/* Enforce in-order execution of data I/O. 
 * No distinction between read/write on PPC; use eieio for all three.
 */
#define iobarrier_rw() eieio()
#define iobarrier_r()  eieio()
#define iobarrier_w()  eieio()

/*
 * 8, 16 and 32 bit, big and little endian I/O operations, with barrier.
 * These routines do not perform EEH-related I/O address translation,
 * and should not be used directly by device drivers.  Use inb/readb
 * instead.
 */
static inline int in_8(volatile unsigned char *addr)
{
	int ret;

	__asm__ __volatile__("lbz%U1%X1 %0,%1; twi 0,%0,0; isync"
			     : "=r" (ret) : "m" (*addr));
	return ret;
}

static inline void out_8(volatile unsigned char *addr, int val)
{
	__asm__ __volatile__("stb%U0%X0 %1,%0; sync"
			     : "=m" (*addr) : "r" (val));
}

static inline int in_le16(volatile unsigned short *addr)
{
	int ret;

	__asm__ __volatile__("lhbrx %0,0,%1; twi 0,%0,0; isync"
			     : "=r" (ret) : "r" (addr), "m" (*addr));
	return ret;
}

static inline int in_be16(volatile unsigned short *addr)
{
	int ret;

	__asm__ __volatile__("lhz%U1%X1 %0,%1; twi 0,%0,0; isync"
			     : "=r" (ret) : "m" (*addr));
	return ret;
}

static inline void out_le16(volatile unsigned short *addr, int val)
{
	__asm__ __volatile__("sthbrx %1,0,%2; sync"
			     : "=m" (*addr) : "r" (val), "r" (addr));
}

static inline void out_be16(volatile unsigned short *addr, int val)
{
	__asm__ __volatile__("sth%U0%X0 %1,%0; sync"
			     : "=m" (*addr) : "r" (val));
}

static inline unsigned in_le32(volatile unsigned *addr)
{
	unsigned ret;

	__asm__ __volatile__("lwbrx %0,0,%1; twi 0,%0,0; isync"
			     : "=r" (ret) : "r" (addr), "m" (*addr));
	return ret;
}

static inline unsigned in_be32(volatile unsigned *addr)
{
	unsigned ret;

	__asm__ __volatile__("lwz%U1%X1 %0,%1; twi 0,%0,0; isync"
			     : "=r" (ret) : "m" (*addr));
	return ret;
}

static inline void out_le32(volatile unsigned *addr, int val)
{
	__asm__ __volatile__("stwbrx %1,0,%2; sync" : "=m" (*addr)
			     : "r" (val), "r" (addr));
}

static inline void out_be32(volatile unsigned *addr, int val)
{
	__asm__ __volatile__("stw%U0%X0 %1,%0; sync"
			     : "=m" (*addr) : "r" (val));
}

static inline unsigned long in_le64(volatile unsigned long *addr)
{
	unsigned long tmp, ret;

	__asm__ __volatile__(
			     "ld %1,0(%2)\n"
			     "twi 0,%1,0\n"
			     "isync\n"
			     "rldimi %0,%1,5*8,1*8\n"
			     "rldimi %0,%1,3*8,2*8\n"
			     "rldimi %0,%1,1*8,3*8\n"
			     "rldimi %0,%1,7*8,4*8\n"
			     "rldicl %1,%1,32,0\n"
			     "rlwimi %0,%1,8,8,31\n"
			     "rlwimi %0,%1,24,16,23\n"
			     : "=r" (ret) , "=r" (tmp) : "b" (addr) , "m" (*addr));
	return ret;
}

static inline unsigned long in_be64(volatile unsigned long *addr)
{
	unsigned long ret;

	__asm__ __volatile__("ld %0,0(%1); twi 0,%0,0; isync"
			     : "=r" (ret) : "m" (*addr));
	return ret;
}

static inline void out_le64(volatile unsigned long *addr, unsigned long val)
{
	unsigned long tmp;

	__asm__ __volatile__(
			     "rldimi %0,%1,5*8,1*8\n"
			     "rldimi %0,%1,3*8,2*8\n"
			     "rldimi %0,%1,1*8,3*8\n"
			     "rldimi %0,%1,7*8,4*8\n"
			     "rldicl %1,%1,32,0\n"
			     "rlwimi %0,%1,8,8,31\n"
			     "rlwimi %0,%1,24,16,23\n"
			     "std %0,0(%3)\n"
			     "sync"
			     : "=&r" (tmp) , "=&r" (val) : "1" (val) , "b" (addr) , "m" (*addr));
}

static inline void out_be64(volatile unsigned long *addr, unsigned long val)
{
	__asm__ __volatile__("std%U0%X0 %1,%0; sync" : "=m" (*addr) : "r" (val));
}

#ifndef CONFIG_PPC_ISERIES 
#include <asm/eeh.h>
#endif

#ifdef __KERNEL__

/**
 *	check_signature		-	find BIOS signatures
 *	@io_addr: mmio address to check
 *	@signature:  signature block
 *	@length: length of signature
 *
 *	Perform a signature comparison with the mmio address io_addr. This
 *	address should have been obtained by ioremap.
 *	Returns 1 on a match.
 */
static inline int check_signature(unsigned long io_addr,
	const unsigned char *signature, int length)
{
	int retval = 0;
#ifndef CONFIG_PPC_ISERIES 
	do {
		if (readb(io_addr) != *signature)
			goto out;
		io_addr++;
		signature++;
		length--;
	} while (length);
	retval = 1;
out:
#endif
	return retval;
}

/* Nothing to do */

#define dma_cache_inv(_start,_size)		do { } while (0)
#define dma_cache_wback(_start,_size)		do { } while (0)
#define dma_cache_wback_inv(_start,_size)	do { } while (0)

#endif /* __KERNEL__ */

#endif /* _PPC64_IO_H */
