/*
 * memory.c: memory initialisation code.
 *
 * Copyright (C) 1998 Harald Koerfgen, Frieder Streffer and Paul M. Antoine
 *
 * $Id: memory.c,v 1.3 1999/10/09 00:00:58 ralf Exp $
 */
#include <linux/init.h>
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/bootmem.h>

#include <asm/addrspace.h>
#include <asm/page.h>

#include <asm/dec/machtype.h>

#include "prom.h"

typedef struct {
	int pagesize;
	unsigned char bitmap[0];
} memmap;

extern int (*rex_getbitmap)(memmap *);

#undef PROM_DEBUG

#ifdef PROM_DEBUG
extern int (*prom_printf)(char *, ...);
#endif

volatile unsigned long mem_err = 0;	/* So we know an error occured */

extern char _end;

#define PFN_UP(x)	(((x) + PAGE_SIZE-1) >> PAGE_SHIFT)
#define PFN_ALIGN(x)	(((unsigned long)(x) + (PAGE_SIZE - 1)) & PAGE_MASK)

/*
 * Probe memory in 4MB chunks, waiting for an error to tell us we've fallen
 * off the end of real memory.  Only suitable for the 2100/3100's (PMAX).
 */

#define CHUNK_SIZE 0x400000

unsigned long __init pmax_get_memory_size(void)
{
	volatile unsigned char *memory_page, dummy;
	char	old_handler[0x80];
	extern char genexcept_early;

	/* Install exception handler */
	memcpy(&old_handler, (void *)(KSEG0 + 0x80), 0x80);
	memcpy((void *)(KSEG0 + 0x80), &genexcept_early, 0x80);

	/* read unmapped and uncached (KSEG1)
	 * DECstations have at least 4MB RAM
	 * Assume less than 480MB of RAM, as this is max for 5000/2xx
	 * FIXME this should be replaced by the first free page!
	 */
	for (memory_page = (unsigned char *) KSEG1 + CHUNK_SIZE;
	     (mem_err== 0) && (memory_page < ((unsigned char *) KSEG1+0x1E000000));
  	     memory_page += CHUNK_SIZE) {
		dummy = *memory_page;
	}
	memcpy((void *)(KSEG0 + 0x80), &old_handler, 0x80);
	return (unsigned long)memory_page - KSEG1 - CHUNK_SIZE;
}

/*
 * Use the REX prom calls to get hold of the memory bitmap, and thence
 * determine memory size.
 */
unsigned long __init rex_get_memory_size(void)
{
	int i, bitmap_size;
	unsigned long mem_size = 0;
	memmap *bm;

	/* some free 64k */
	bm = (memmap *) 0x80028000;

	bitmap_size = rex_getbitmap(bm);

	for (i = 0; i < bitmap_size; i++) {
		/* FIXME: very simplistically only add full sets of pages */
		if (bm->bitmap[i] == 0xff)
			mem_size += (8 * bm->pagesize);
	}

	return (mem_size);
}

void __init prom_meminit(unsigned int magic)
{
	unsigned long free_start, free_end, start_pfn, bootmap_size;
	unsigned long mem_size = 0;

	if (magic != REX_PROM_MAGIC)
		mem_size = pmax_get_memory_size();
	else
		mem_size = rex_get_memory_size();

	free_start = PHYSADDR(PFN_ALIGN(&_end));
	free_end = mem_size;
	start_pfn = PFN_UP((unsigned long)&_end);

#ifdef PROM_DEBUG
	prom_printf("free_start: 0x%08x\n", free_start);
	prom_printf("free_end: 0x%08x\n", free_end);
	prom_printf("start_pfn: 0x%08x\n", start_pfn);
#endif

	/* Register all the contiguous memory with the bootmem allocator
	   and free it.  Be careful about the bootmem freemap.  */
	bootmap_size = init_bootmem(start_pfn, mem_size >> PAGE_SHIFT);
	free_bootmem(free_start + bootmap_size, free_end - free_start - bootmap_size);
}

int __init page_is_ram(unsigned long pagenr)
{
        return 1;
}

void prom_free_prom_memory (void)
{
	unsigned long addr, end;
	extern	char _ftext;

	/*
	 * Free everything below the kernel itself but leave
	 * the first page reserved for the exception handlers.
	 */

#ifdef CONFIG_DECLANCE
	/*
	 * Leave 128 KB reserved for Lance memory for
	 * IOASIC DECstations.
	 *
	 * XXX: save this address for use in dec_lance.c?
	 */
	if (IOASIC)
		end = PHYSADDR(&_ftext) - 0x00020000;
	else
#endif
		end = PHYSADDR(&_ftext);

	addr = PAGE_SIZE;
	while (addr < end) {
		ClearPageReserved(virt_to_page(addr));
		set_page_count(virt_to_page(addr), 1);
		free_page(addr);
		addr += PAGE_SIZE;
	}

	printk("Freeing unused PROM memory: %ldk freed\n",
	       (end - PAGE_SIZE) >> 10);
}
