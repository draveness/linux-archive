/*
 * Copyright (C) 1996 David S. Miller (dm@engr.sgi.com)
 * Copyright (C) 1997, 2001 Ralf Baechle (ralf@gnu.org)
 * Copyright (C) 2000 SiByte, Inc.
 *
 * Written by Justin Carlson of SiByte, Inc.
 *         and Kip Walker of Broadcom Corp.
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/smp.h>

#include <asm/io.h>
#include <asm/sibyte/sb1250.h>
#include <asm/sibyte/sb1250_regs.h>
#include <asm/sibyte/sb1250_dma.h>

#ifdef CONFIG_SB1_PASS_1_WORKAROUNDS
#define SB1_PREF_LOAD_STREAMED_HINT "0"
#define SB1_PREF_STORE_STREAMED_HINT "1"
#else
#define SB1_PREF_LOAD_STREAMED_HINT "4"
#define SB1_PREF_STORE_STREAMED_HINT "5"
#endif

#ifdef CONFIG_SIBYTE_DMA_PAGEOPS
static inline void clear_page_cpu(void *page)
#else
void clear_page(void *page)
#endif
{
	unsigned char *addr = (unsigned char *) page;
	unsigned char *end = addr + PAGE_SIZE;

	/*
	 * JDCXXX - This should be bottlenecked by the write buffer, but these
	 * things tend to be mildly unpredictable...should check this on the
	 * performance model
	 *
	 * We prefetch 4 lines ahead.  We're also "cheating" slightly here...
	 * since we know we're on an SB1, we force the assembler to take
	 * 64-bit operands to speed things up
	 */
	do {
		__asm__ __volatile__(
		"	.set	mips4		\n"
#ifdef CONFIG_CPU_HAS_PREFETCH
		"	pref	" SB1_PREF_STORE_STREAMED_HINT ",  0(%0)  \n"  /* Prefetch the first 4 lines */
		"	pref	" SB1_PREF_STORE_STREAMED_HINT ", 32(%0)  \n"
		"	pref	" SB1_PREF_STORE_STREAMED_HINT ", 64(%0)  \n"
		"	pref	" SB1_PREF_STORE_STREAMED_HINT ", 96(%0)  \n"
#endif
		"1:	sd	$0,  0(%0)	\n"  /* Throw out a cacheline of 0's */
		"	sd	$0,  8(%0)	\n"
		"	sd	$0, 16(%0)	\n"
		"	sd	$0, 24(%0)	\n"
#ifdef CONFIG_CPU_HAS_PREFETCH
		"	pref	" SB1_PREF_STORE_STREAMED_HINT ",128(%0)  \n"  /* Prefetch 4 lines ahead     */
#endif
		"	.set	mips0		\n"
		:
		: "r" (addr)
		: "memory");
		addr += 32;
	} while (addr != end);
}

#ifdef CONFIG_SIBYTE_DMA_PAGEOPS
static inline void copy_page_cpu(void *to, void *from)
#else
void copy_page(void *to, void *from)
#endif
{
	unsigned char *src = from;
	unsigned char *dst = to;
	unsigned char *end = src + PAGE_SIZE;

	/*
	 * This should be optimized in assembly...can't use ld/sd, though,
	 * because the top 32 bits could be nuked if we took an interrupt
	 * during the routine.	And this is not a good place to be cli()'ing
	 *
	 * The pref's used here are using "streaming" hints, which cause the
	 * copied data to be kicked out of the cache sooner.  A page copy often
	 * ends up copying a lot more data than is commonly used, so this seems
	 * to make sense in terms of reducing cache pollution, but I've no real
	 * performance data to back this up
	 */

	do {
		__asm__ __volatile__(
		"	.set	mips4					\n"
#ifdef CONFIG_CPU_HAS_PREFETCH
		"	pref	" SB1_PREF_LOAD_STREAMED_HINT  ",  0(%0)\n"  /* Prefetch the first 3 lines */
		"	pref	" SB1_PREF_STORE_STREAMED_HINT ",  0(%1)\n"
		"	pref	" SB1_PREF_LOAD_STREAMED_HINT  ",  32(%0)\n"
		"	pref	" SB1_PREF_STORE_STREAMED_HINT ",  32(%1)\n"
		"	pref	" SB1_PREF_LOAD_STREAMED_HINT  ",  64(%0)\n"
		"	pref	" SB1_PREF_STORE_STREAMED_HINT ",  64(%1)\n"
#endif
		"1:	lw	$2,  0(%0)	\n"  /* Block copy a cacheline */
		"	lw	$3,  4(%0)	\n"
		"	lw	$4,  8(%0)	\n"
		"	lw	$5, 12(%0)	\n"
		"	lw	$6, 16(%0)	\n"
		"	lw	$7, 20(%0)	\n"
		"	lw	$8, 24(%0)	\n"
		"	lw	$9, 28(%0)	\n"
#ifdef CONFIG_CPU_HAS_PREFETCH
		"	pref	" SB1_PREF_LOAD_STREAMED_HINT  ", 96(%0)  \n"  /* Prefetch ahead         */
		"	pref	" SB1_PREF_STORE_STREAMED_HINT ", 96(%1)  \n"
#endif
		"	sw	$2,  0(%1)	\n"
		"	sw	$3,  4(%1)	\n"
		"	sw	$4,  8(%1)	\n"
		"	sw	$5, 12(%1)	\n"
		"	sw	$6, 16(%1)	\n"
		"	sw	$7, 20(%1)	\n"
		"	sw	$8, 24(%1)	\n"
		"	sw	$9, 28(%1)	\n"
		"	.set	mips0		\n"
		:
		: "r" (src), "r" (dst)
		: "$2","$3","$4","$5","$6","$7","$8","$9","memory");
		src += 32;
		dst += 32;
	} while (src != end);
}


#ifdef CONFIG_SIBYTE_DMA_PAGEOPS

/*
 * Pad descriptors to cacheline, since each is exclusively owned by a
 * particular CPU. 
 */
typedef struct dmadscr_s {
	uint64_t  dscr_a;
	uint64_t  dscr_b;
	uint64_t  pad_a;
	uint64_t  pad_b;
} dmadscr_t;

static dmadscr_t page_descr[NR_CPUS] __attribute__((aligned(SMP_CACHE_BYTES)));

void sb1_dma_init(void)
{
	int cpu = smp_processor_id();
	uint64_t base_val = PHYSADDR(&page_descr[cpu]) | V_DM_DSCR_BASE_RINGSZ(1);

	__raw_writeq(base_val,
		     IOADDR(A_DM_REGISTER(cpu, R_DM_DSCR_BASE)));
	__raw_writeq(base_val | M_DM_DSCR_BASE_RESET,
		     IOADDR(A_DM_REGISTER(cpu, R_DM_DSCR_BASE)));
	__raw_writeq(base_val | M_DM_DSCR_BASE_ENABL,
		     IOADDR(A_DM_REGISTER(cpu, R_DM_DSCR_BASE)));
}

void clear_page(void *page)
{
	int cpu = smp_processor_id();

	/* if the page is above Kseg0, use old way */
	if (KSEGX(page) != CAC_BASE)
		return clear_page_cpu(page);

	page_descr[cpu].dscr_a = PHYSADDR(page) | M_DM_DSCRA_ZERO_MEM | M_DM_DSCRA_L2C_DEST | M_DM_DSCRA_INTERRUPT;
	page_descr[cpu].dscr_b = V_DM_DSCRB_SRC_LENGTH(PAGE_SIZE);
	__raw_writeq(1, IOADDR(A_DM_REGISTER(cpu, R_DM_DSCR_COUNT)));

	/*
	 * Don't really want to do it this way, but there's no
	 * reliable way to delay completion detection.
	 */
	while (!(__raw_readq(IOADDR(A_DM_REGISTER(cpu, R_DM_DSCR_BASE_DEBUG)) & M_DM_DSCR_BASE_INTERRUPT)))
		;
	__raw_readq(IOADDR(A_DM_REGISTER(cpu, R_DM_DSCR_BASE)));
}

void copy_page(void *to, void *from)
{
	unsigned long from_phys = PHYSADDR(from);
	unsigned long to_phys = PHYSADDR(to);
	int cpu = smp_processor_id();

	/* if either page is above Kseg0, use old way */
	if ((KSEGX(to) != CAC_BASE) || (KSEGX(from) != CAC_BASE))
		return copy_page_cpu(to, from);

	page_descr[cpu].dscr_a = PHYSADDR(to_phys) | M_DM_DSCRA_L2C_DEST | M_DM_DSCRA_INTERRUPT;
	page_descr[cpu].dscr_b = PHYSADDR(from_phys) | V_DM_DSCRB_SRC_LENGTH(PAGE_SIZE);
	__raw_writeq(1, IOADDR(A_DM_REGISTER(cpu, R_DM_DSCR_COUNT)));

	/*
	 * Don't really want to do it this way, but there's no
	 * reliable way to delay completion detection.
	 */
	while (!(__raw_readq(IOADDR(A_DM_REGISTER(cpu, R_DM_DSCR_BASE_DEBUG)) & M_DM_DSCR_BASE_INTERRUPT)))
		;
	__raw_readq(IOADDR(A_DM_REGISTER(cpu, R_DM_DSCR_BASE)));
}

#endif

EXPORT_SYMBOL(clear_page);
EXPORT_SYMBOL(copy_page);
