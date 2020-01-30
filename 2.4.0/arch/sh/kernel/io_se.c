/* $Id: io_se.c,v 1.5 2000/06/08 05:50:10 gniibe Exp $
 *
 * linux/arch/sh/kernel/io_se.c
 *
 * Copyright (C) 2000  Kazumoto Kojima
 *
 * I/O routine for Hitachi SolutionEngine.
 *
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <asm/io.h>
#include <asm/hitachi_se.h>

/* SH pcmcia io window base, start and end.  */
int sh_pcic_io_wbase = 0xb8400000;
int sh_pcic_io_start;
int sh_pcic_io_stop;
int sh_pcic_io_type;
int sh_pcic_io_dummy;

static inline void delay(void)
{
	ctrl_inw(0xa0000000);
}

/* MS7750 requires special versions of in*, out* routines, since
   PC-like io ports are located at upper half byte of 16-bit word which
   can be accessed only with 16-bit wide.  */

static inline volatile __u16 *
port2adr(unsigned int port)
{
	if (port >= 0x2000)
		return (volatile __u16 *) (PA_MRSHPC + (port - 0x2000));
	else if (port >= 0x1000)
		return (volatile __u16 *) (PA_83902 + (port << 1));
	else if (sh_pcic_io_start <= port && port <= sh_pcic_io_stop)
		return (volatile __u16 *) (sh_pcic_io_wbase + (port &~ 1));
	else
		return (volatile __u16 *) (PA_SUPERIO + (port << 1));
}

static inline int
shifted_port(unsigned int port)
{
	/* For IDE registers, value is not shifted */
	if ((0x1f0 <= port && port < 0x1f8) || port == 0x3f6)
		return 0;
	else
		return 1;
}

#define maybebadio(name,port) \
  printk("bad PC-like io %s for port 0x%x at 0x%08x\n", \
	 #name, (port), (__u32) __builtin_return_address(0))

unsigned long se_inb(unsigned int port)
{
	if (sh_pcic_io_start <= port && port <= sh_pcic_io_stop)
		return *(__u8 *) (sh_pcic_io_wbase + 0x40000 + port); 
	else if (shifted_port(port))
		return (*port2adr(port) >> 8); 
	else
		return (*port2adr(port))&0xff; 
}

unsigned long se_inb_p(unsigned int port)
{
	unsigned long v;

	if (sh_pcic_io_start <= port && port <= sh_pcic_io_stop)
		v = *(__u8 *) (sh_pcic_io_wbase + 0x40000 + port); 
	else if (shifted_port(port))
		v = (*port2adr(port) >> 8); 
	else
		v = (*port2adr(port))&0xff; 
	delay();
	return v;
}

unsigned long se_inw(unsigned int port)
{
	if (port >= 0x2000 ||
	    (sh_pcic_io_start <= port && port <= sh_pcic_io_stop))
		return *port2adr(port);
	else
		maybebadio(inw, port);
	return 0;
}

unsigned long se_inl(unsigned int port)
{
	maybebadio(inl, port);
	return 0;
}

void se_outb(unsigned long value, unsigned int port)
{
	if (sh_pcic_io_start <= port && port <= sh_pcic_io_stop)
		*(__u8 *)(sh_pcic_io_wbase + port) = value; 
	else if (shifted_port(port))
		*(port2adr(port)) = value << 8;
	else
		*(port2adr(port)) = value;
}

void se_outb_p(unsigned long value, unsigned int port)
{
	if (sh_pcic_io_start <= port && port <= sh_pcic_io_stop)
		*(__u8 *)(sh_pcic_io_wbase + port) = value; 
	else if (shifted_port(port))
		*(port2adr(port)) = value << 8;
	else
		*(port2adr(port)) = value;
	delay();
}

void se_outw(unsigned long value, unsigned int port)
{
	if (port >= 0x2000 ||
	    (sh_pcic_io_start <= port && port <= sh_pcic_io_stop))
		*port2adr(port) = value;
	else
		maybebadio(outw, port);
}

void se_outl(unsigned long value, unsigned int port)
{
	maybebadio(outl, port);
}

void se_insb(unsigned int port, void *addr, unsigned long count)
{
	volatile __u16 *p = port2adr(port);

	if (sh_pcic_io_start <= port && port <= sh_pcic_io_stop) {
		volatile __u8 *bp = (__u8 *) (sh_pcic_io_wbase + 0x40000 + port); 
		while (count--)
			*((__u8 *) addr)++ = *bp;
	} else if (shifted_port(port)) {
		while (count--)
			*((__u8 *) addr)++ = *p >> 8;
	} else {
		while (count--)
			*((__u8 *) addr)++ = *p;
	}
}

void se_insw(unsigned int port, void *addr, unsigned long count)
{
	volatile __u16 *p = port2adr(port);
	while (count--)
		*((__u16 *) addr)++ = *p;
}

void se_insl(unsigned int port, void *addr, unsigned long count)
{
	maybebadio(insl, port);
}

void se_outsb(unsigned int port, const void *addr, unsigned long count)
{
	volatile __u16 *p = port2adr(port);

	if (sh_pcic_io_start <= port && port <= sh_pcic_io_stop) {
		volatile __u8 *bp = (__u8 *) (sh_pcic_io_wbase + port); 
		while (count--)
			*bp = *((__u8 *) addr)++;
	} else if (shifted_port(port)) {
		while (count--)
			*p = *((__u8 *) addr)++ << 8;
	} else {
		while (count--)
			*p = *((__u8 *) addr)++;
	}
}

void se_outsw(unsigned int port, const void *addr, unsigned long count)
{
	volatile __u16 *p = port2adr(port);
	while (count--)
		*p = *((__u16 *) addr)++;
}

void se_outsl(unsigned int port, const void *addr, unsigned long count)
{
	maybebadio(outsw, port);
}

unsigned long se_readb(unsigned long addr)
{
	return *(volatile unsigned char*)addr;
}

unsigned long se_readw(unsigned long addr)
{
	return *(volatile unsigned short*)addr;
}

unsigned long se_readl(unsigned long addr)
{
	return *(volatile unsigned long*)addr;
}

void se_writeb(unsigned char b, unsigned long addr)
{
	*(volatile unsigned char*)addr = b;
}

void se_writew(unsigned short b, unsigned long addr)
{
	*(volatile unsigned short*)addr = b;
}

void se_writel(unsigned int b, unsigned long addr)
{
        *(volatile unsigned long*)addr = b;
}

/* Map ISA bus address to the real address. Only for PCMCIA.  */

/* ISA page descriptor.  */
static __u32 sh_isa_memmap[256];

static int
sh_isa_mmap(__u32 start, __u32 length, __u32 offset)
{
	int idx;

	if (start >= 0x100000 || (start & 0xfff) || (length != 0x1000))
		return -1;

	idx = start >> 12;
	sh_isa_memmap[idx] = 0xb8000000 + (offset &~ 0xfff);
#if 0
	printk("sh_isa_mmap: start %x len %x offset %x (idx %x paddr %x)\n",
	       start, length, offset, idx, sh_isa_memmap[idx]);
#endif
	return 0;
}

unsigned long
se_isa_port2addr(unsigned long offset)
{
	int idx;

	idx = (offset >> 12) & 0xff;
	offset &= 0xfff;
	return sh_isa_memmap[idx] + offset;
}
