/*
 *  linux/arch/m68k/sun3/config.c
 *
 *  Copyright (C) 1996,1997 Pekka Pietik{inen
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 */

#include <stdarg.h>

#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/kd.h>
#include <linux/tty.h>
#include <linux/console.h>
#include <linux/init.h>
#include <linux/bootmem.h>

#include <asm/oplib.h>
#include <asm/setup.h>
#include <asm/contregs.h>
#include <asm/movs.h>
#include <asm/pgtable.h>
#include <asm/sun3-head.h>
#include <asm/sun3mmu.h>
#include <asm/machdep.h>
#include <asm/intersil.h>
#include <asm/irq.h>
#include <asm/segment.h>

extern char _text, _end;

char sun3_reserved_pmeg[SUN3_PMEGS_NUM];

extern unsigned long sun3_gettimeoffset(void);
extern int sun3_get_irq_list (char *);
extern void sun3_sched_init(void (*handler)(int, void *, struct pt_regs *));
extern void sun3_init_IRQ (void);
extern void (*sun3_default_handler[]) (int, void *, struct pt_regs *);
extern int sun3_request_irq (unsigned int irq, void (*handler)(int, void *, struct pt_regs *),
                              unsigned long flags, const char *devname, void *dev_id);
extern void sun3_free_irq (unsigned int irq, void *dev_id);
extern void sun3_enable_irq (unsigned int);
extern void sun3_disable_irq (unsigned int);
extern void sun3_enable_interrupts (void);
extern void sun3_disable_interrupts (void);
extern void sun3_get_model (char* model);
extern void idprom_init (void);
extern void sun3_gettod (int *yearp, int *monp, int *dayp,
                   int *hourp, int *minp, int *secp);
extern int sun3_hwclk(int set, struct hwclk_time *t);

extern void sun_serial_setup(void);
volatile char* clock_va; 
extern unsigned char* sun3_intreg;
extern unsigned long availmem;
unsigned long num_pages;

void __init sun3_init(void)
{
	unsigned char enable_register;
	int i;

	m68k_machtype= MACH_SUN3;
	m68k_cputype = CPU_68020;
	m68k_fputype = FPU_68881; /* mc68881 actually */
	m68k_mmutype = MMU_SUN3;
	clock_va    =          (char *) 0xfe06000;	/* dark  */
	sun3_intreg = (unsigned char *) 0xfe0a000;	/* magic */
	sun3_disable_interrupts();
	
	prom_init((void *)LINUX_OPPROM_BEGVM);
		
	GET_CONTROL_BYTE(AC_SENABLE,enable_register);
	enable_register |= 0x40; /* Enable FPU */	
	SET_CONTROL_BYTE(AC_SENABLE,enable_register);
	GET_CONTROL_BYTE(AC_SENABLE,enable_register);
	
	/* This code looks suspicious, because it doesn't subtract
           memory belonging to the kernel from the available space */


	memset(sun3_reserved_pmeg, 0, sizeof(sun3_reserved_pmeg));

	/* Reserve important PMEGS */
	/* FIXME: These should be probed instead of hardcoded */

	for (i=0; i<8; i++)		/* Kernel PMEGs */
		sun3_reserved_pmeg[i] = 1;

	sun3_reserved_pmeg[247] = 1;	/* ROM mapping  */
	sun3_reserved_pmeg[248] = 1;	/* AMD Ethernet */
	sun3_reserved_pmeg[251] = 1;	/* VB area      */
	sun3_reserved_pmeg[254] = 1;	/* main I/O     */

	sun3_reserved_pmeg[249] = 1;
	sun3_reserved_pmeg[252] = 1;
	sun3_reserved_pmeg[253] = 1;
	set_fs(KERNEL_DS);
}

/* Without this, Bad Things happen when something calls arch_reset. */
static void sun3_reboot (void)
{
	prom_reboot ("vmlinux");
}

static void sun3_halt (void)
{
	prom_halt ();
}

/* sun3 bootmem allocation */

void __init sun3_bootmem_alloc(unsigned long memory_start, unsigned long memory_end)
{
	unsigned long start_page;

	/* align start/end to page boundries */
	memory_start = ((memory_start + (PAGE_SIZE-1)) & PAGE_MASK);
	memory_end = memory_end & PAGE_MASK;
		
	start_page = __pa(memory_start) >> PAGE_SHIFT;
	num_pages = __pa(memory_end) >> PAGE_SHIFT;

	high_memory = (void *)memory_end;
	availmem = memory_start;

	availmem += init_bootmem(start_page, num_pages);
	availmem = (availmem + (PAGE_SIZE-1)) & PAGE_MASK;

	free_bootmem(__pa(availmem), memory_end - (availmem));
}
	

void __init config_sun3(void)
{
	unsigned long memory_start, memory_end;

	printk("ARCH: SUN3\n");
	idprom_init();

	/* Subtract kernel memory from available memory */

        mach_sched_init      =  sun3_sched_init; 
        mach_init_IRQ        =  sun3_init_IRQ;
        mach_default_handler = &sun3_default_handler;
        mach_request_irq     =  sun3_request_irq;
        mach_free_irq        =  sun3_free_irq;
//	mach_keyb_init       =  sun3_keyb_init;
	enable_irq     	     =  sun3_enable_irq;
        disable_irq  	     =  sun3_disable_irq;
        mach_get_irq_list    =  sun3_get_irq_list;
        mach_gettod          =  sun3_gettod;	
        mach_reset           =  sun3_reboot;
	mach_gettimeoffset   =  sun3_gettimeoffset;
	mach_get_model	     =  sun3_get_model;
	mach_hwclk           =  sun3_hwclk;
	mach_halt	     =  sun3_halt;
#ifndef CONFIG_SERIAL_CONSOLE
	conswitchp 	     = &dummy_con;
#endif

	memory_start = ((((int)&_end) + 0x2000) & ~0x1fff);
// PROM seems to want the last couple of physical pages. --m
	memory_end   = *(romvec->pv_sun3mem) + PAGE_OFFSET - 2*PAGE_SIZE;

	m68k_num_memory=1;
        m68k_memory[0].size=*(romvec->pv_sun3mem);
	
	sun3_bootmem_alloc(memory_start, memory_end);

	sun_serial_setup();


}

void __init sun3_sched_init(void (*timer_routine)(int, void *, struct pt_regs *))
{
	sun3_disable_interrupts();
        intersil_clock->cmd_reg=(INTERSIL_RUN|INTERSIL_INT_DISABLE|INTERSIL_24H_MODE);
        intersil_clock->int_reg=INTERSIL_HZ_100_MASK;
 	intersil_clear();
        sun3_enable_irq(5);
        intersil_clock->cmd_reg=(INTERSIL_RUN|INTERSIL_INT_ENABLE|INTERSIL_24H_MODE);
        sun3_enable_interrupts();
        intersil_clear();
}

