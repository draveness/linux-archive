#ifndef _ASM_IO_H
#define _ASM_IO_H

#include <linux/config.h>

/*
 * This file contains the definitions for the x86 IO instructions
 * inb/inw/inl/outb/outw/outl and the "string versions" of the same
 * (insb/insw/insl/outsb/outsw/outsl). You can also use "pausing"
 * versions of the single-IO instructions (inb_p/inw_p/..).
 *
 * This file is not meant to be obfuscating: it's just complicated
 * to (a) handle it all in a way that makes gcc able to optimize it
 * as well as possible and (b) trying to avoid writing the same thing
 * over and over again with slight variations and possibly making a
 * mistake somewhere.
 */

/*
 * Thanks to James van Artsdalen for a better timing-fix than
 * the two short jumps: using outb's to a nonexistent port seems
 * to guarantee better timings even on fast machines.
 *
 * On the other hand, I'd like to be sure of a non-existent port:
 * I feel a bit unsafe about using 0x80 (should be safe, though)
 *
 *		Linus
 */

 /*
  *  Bit simplified and optimized by Jan Hubicka
  *  Support of BIGMEM added by Gerhard Wichert, Siemens AG, July 1999.
  *
  *  isa_memset_io, isa_memcpy_fromio, isa_memcpy_toio added,
  *  isa_read[wl] and isa_write[wl] fixed
  *  - Arnaldo Carvalho de Melo <acme@conectiva.com.br>
  */

#define IO_SPACE_LIMIT 0xffff

#define XQUAD_PORTIO_BASE 0xfe400000
#define XQUAD_PORTIO_QUAD 0x40000  /* 256k per quad. */

#ifdef __KERNEL__

#include <linux/vmalloc.h>

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
	return __pa(address);
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
	return __va(address);
}

/*
 * Change "struct page" to physical address.
 */
#define page_to_phys(page)    ((dma_addr_t)page_to_pfn(page) << PAGE_SHIFT)

extern void * __ioremap(unsigned long offset, unsigned long size, unsigned long flags);

/**
 * ioremap     -   map bus memory into CPU space
 * @offset:    bus address of the memory
 * @size:      size of the resource to map
 *
 * ioremap performs a platform specific sequence of operations to
 * make bus memory CPU accessible via the readb/readw/readl/writeb/
 * writew/writel functions and the other mmio helpers. The returned
 * address is not guaranteed to be usable directly as a virtual
 * address. 
 */

static inline void * ioremap (unsigned long offset, unsigned long size)
{
	return __ioremap(offset, size, 0);
}

extern void * ioremap_nocache (unsigned long offset, unsigned long size);
extern void iounmap(void *addr);

/*
 * bt_ioremap() and bt_iounmap() are for temporary early boot-time
 * mappings, before the real ioremap() is functional.
 * A boot-time mapping is currently limited to at most 16 pages.
 */
extern void *bt_ioremap(unsigned long offset, unsigned long size);
extern void bt_iounmap(void *addr, unsigned long size);

/*
 * ISA I/O bus memory addresses are 1:1 with the physical address.
 */
#define isa_virt_to_bus virt_to_phys
#define isa_page_to_bus page_to_phys
#define isa_bus_to_virt phys_to_virt

/*
 * However PCI ones are not necessarily 1:1 and therefore these interfaces
 * are forbidden in portable PCI drivers.
 *
 * Allow them on x86 for legacy drivers, though.
 */
#define virt_to_bus virt_to_phys
#define bus_to_virt phys_to_virt

/*
 * readX/writeX() are used to access memory mapped devices. On some
 * architectures the memory mapped IO stuff needs to be accessed
 * differently. On the x86 architecture, we just read/write the
 * memory location directly.
 */

#define readb(addr) (*(volatile unsigned char *) (addr))
#define readw(addr) (*(volatile unsigned short *) (addr))
#define readl(addr) (*(volatile unsigned int *) (addr))
#define readb_relaxed(addr) readb(addr)
#define readw_relaxed(addr) readw(addr)
#define readl_relaxed(addr) readl(addr)
#define __raw_readb readb
#define __raw_readw readw
#define __raw_readl readl

#define writeb(b,addr) (*(volatile unsigned char *) (addr) = (b))
#define writew(b,addr) (*(volatile unsigned short *) (addr) = (b))
#define writel(b,addr) (*(volatile unsigned int *) (addr) = (b))
#define __raw_writeb writeb
#define __raw_writew writew
#define __raw_writel writel

#define memset_io(a,b,c)	memset((void *)(a),(b),(c))
#define memcpy_fromio(a,b,c)	__memcpy((a),(void *)(b),(c))
#define memcpy_toio(a,b,c)	__memcpy((void *)(a),(b),(c))

/*
 * ISA space is 'always mapped' on a typical x86 system, no need to
 * explicitly ioremap() it. The fact that the ISA IO space is mapped
 * to PAGE_OFFSET is pure coincidence - it does not mean ISA values
 * are physical addresses. The following constant pointer can be
 * used as the IO-area pointer (it can be iounmapped as well, so the
 * analogy with PCI is quite large):
 */
#define __ISA_IO_base ((char *)(PAGE_OFFSET))

#define isa_readb(a) readb(__ISA_IO_base + (a))
#define isa_readw(a) readw(__ISA_IO_base + (a))
#define isa_readl(a) readl(__ISA_IO_base + (a))
#define isa_writeb(b,a) writeb(b,__ISA_IO_base + (a))
#define isa_writew(w,a) writew(w,__ISA_IO_base + (a))
#define isa_writel(l,a) writel(l,__ISA_IO_base + (a))
#define isa_memset_io(a,b,c)		memset_io(__ISA_IO_base + (a),(b),(c))
#define isa_memcpy_fromio(a,b,c)	memcpy_fromio((a),__ISA_IO_base + (b),(c))
#define isa_memcpy_toio(a,b,c)		memcpy_toio(__ISA_IO_base + (a),(b),(c))


/*
 * Again, i386 does not require mem IO specific function.
 */

#define eth_io_copy_and_sum(a,b,c,d)		eth_copy_and_sum((a),(void *)(b),(c),(d))
#define isa_eth_io_copy_and_sum(a,b,c,d)	eth_copy_and_sum((a),(void *)(__ISA_IO_base + (b)),(c),(d))

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
	do {
		if (readb(io_addr) != *signature)
			goto out;
		io_addr++;
		signature++;
		length--;
	} while (length);
	retval = 1;
out:
	return retval;
}

/**
 *	isa_check_signature		-	find BIOS signatures
 *	@io_addr: mmio address to check 
 *	@signature:  signature block
 *	@length: length of signature
 *
 *	Perform a signature comparison with the ISA mmio address io_addr.
 *	Returns 1 on a match.
 *
 *	This function is deprecated. New drivers should use ioremap and
 *	check_signature.
 */
 

static inline int isa_check_signature(unsigned long io_addr,
	const unsigned char *signature, int length)
{
	int retval = 0;
	do {
		if (isa_readb(io_addr) != *signature)
			goto out;
		io_addr++;
		signature++;
		length--;
	} while (length);
	retval = 1;
out:
	return retval;
}

/*
 *	Cache management
 *
 *	This needed for two cases
 *	1. Out of order aware processors
 *	2. Accidentally out of order processors (PPro errata #51)
 */
 
#if defined(CONFIG_X86_OOSTORE) || defined(CONFIG_X86_PPRO_FENCE)

static inline void flush_write_buffers(void)
{
	__asm__ __volatile__ ("lock; addl $0,0(%%esp)": : :"memory");
}

#define dma_cache_inv(_start,_size)		flush_write_buffers()
#define dma_cache_wback(_start,_size)		flush_write_buffers()
#define dma_cache_wback_inv(_start,_size)	flush_write_buffers()

#else

/* Nothing to do */

#define dma_cache_inv(_start,_size)		do { } while (0)
#define dma_cache_wback(_start,_size)		do { } while (0)
#define dma_cache_wback_inv(_start,_size)	do { } while (0)
#define flush_write_buffers()

#endif

#endif /* __KERNEL__ */

#ifdef SLOW_IO_BY_JUMPING
#define __SLOW_DOWN_IO "jmp 1f; 1: jmp 1f; 1:"
#else
#define __SLOW_DOWN_IO "outb %%al,$0x80;"
#endif

static inline void slow_down_io(void) {
	__asm__ __volatile__(
		__SLOW_DOWN_IO
#ifdef REALLY_SLOW_IO
		__SLOW_DOWN_IO __SLOW_DOWN_IO __SLOW_DOWN_IO
#endif
		: : );
}

#ifdef CONFIG_X86_NUMAQ
extern void *xquad_portio;    /* Where the IO area was mapped */
#define XQUAD_PORT_ADDR(port, quad) (xquad_portio + (XQUAD_PORTIO_QUAD*quad) + port)
#define __BUILDIO(bwl,bw,type) \
static inline void out##bwl##_quad(unsigned type value, int port, int quad) { \
	if (xquad_portio) \
		write##bwl(value, XQUAD_PORT_ADDR(port, quad)); \
	else \
		out##bwl##_local(value, port); \
} \
static inline void out##bwl(unsigned type value, int port) { \
	out##bwl##_quad(value, port, 0); \
} \
static inline unsigned type in##bwl##_quad(int port, int quad) { \
	if (xquad_portio) \
		return read##bwl(XQUAD_PORT_ADDR(port, quad)); \
	else \
		return in##bwl##_local(port); \
} \
static inline unsigned type in##bwl(int port) { \
	return in##bwl##_quad(port, 0); \
}
#else
#define __BUILDIO(bwl,bw,type) \
static inline void out##bwl(unsigned type value, int port) { \
	out##bwl##_local(value, port); \
} \
static inline unsigned type in##bwl(int port) { \
	return in##bwl##_local(port); \
}
#endif


#define BUILDIO(bwl,bw,type) \
static inline void out##bwl##_local(unsigned type value, int port) { \
	__asm__ __volatile__("out" #bwl " %" #bw "0, %w1" : : "a"(value), "Nd"(port)); \
} \
static inline unsigned type in##bwl##_local(int port) { \
	unsigned type value; \
	__asm__ __volatile__("in" #bwl " %w1, %" #bw "0" : "=a"(value) : "Nd"(port)); \
	return value; \
} \
static inline void out##bwl##_local_p(unsigned type value, int port) { \
	out##bwl##_local(value, port); \
	slow_down_io(); \
} \
static inline unsigned type in##bwl##_local_p(int port) { \
	unsigned type value = in##bwl##_local(port); \
	slow_down_io(); \
	return value; \
} \
__BUILDIO(bwl,bw,type) \
static inline void out##bwl##_p(unsigned type value, int port) { \
	out##bwl(value, port); \
	slow_down_io(); \
} \
static inline unsigned type in##bwl##_p(int port) { \
	unsigned type value = in##bwl(port); \
	slow_down_io(); \
	return value; \
} \
static inline void outs##bwl(int port, const void *addr, unsigned long count) { \
	__asm__ __volatile__("rep; outs" #bwl : "+S"(addr), "+c"(count) : "d"(port)); \
} \
static inline void ins##bwl(int port, void *addr, unsigned long count) { \
	__asm__ __volatile__("rep; ins" #bwl : "+D"(addr), "+c"(count) : "d"(port)); \
}

BUILDIO(b,b,char)
BUILDIO(w,w,short)
BUILDIO(l,,int)

#endif
