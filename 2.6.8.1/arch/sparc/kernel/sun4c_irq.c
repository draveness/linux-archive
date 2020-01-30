/*  sun4c_irq.c
 *  arch/sparc/kernel/sun4c_irq.c:
 *
 *  djhr: Hacked out of irq.c into a CPU dependent version.
 *
 *  Copyright (C) 1995 David S. Miller (davem@caip.rutgers.edu)
 *  Copyright (C) 1995 Miguel de Icaza (miguel@nuclecu.unam.mx)
 *  Copyright (C) 1995 Pete A. Zaitcev (zaitcev@yahoo.com)
 *  Copyright (C) 1996 Dave Redman (djhr@tadpole.co.uk)
 */

#include <linux/config.h>
#include <linux/errno.h>
#include <linux/linkage.h>
#include <linux/kernel_stat.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/ptrace.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/init.h>

#include <asm/ptrace.h>
#include <asm/processor.h>
#include <asm/system.h>
#include <asm/psr.h>
#include <asm/vaddrs.h>
#include <asm/timer.h>
#include <asm/openprom.h>
#include <asm/oplib.h>
#include <asm/traps.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <asm/sun4paddr.h>
#include <asm/idprom.h>
#include <asm/machines.h>
#include <asm/sbus.h>

#if 0
static struct resource sun4c_timer_eb = { "sun4c_timer" };
static struct resource sun4c_intr_eb = { "sun4c_intr" };
#endif

/* Pointer to the interrupt enable byte
 *
 * Dave Redman (djhr@tadpole.co.uk)
 * What you may not be aware of is that entry.S requires this variable.
 *
 *  --- linux_trap_nmi_sun4c --
 *
 * so don't go making it static, like I tried. sigh.
 */
unsigned char *interrupt_enable = NULL;

static int sun4c_pil_map[] = { 0, 1, 2, 3, 5, 7, 8, 9 };

unsigned int sun4c_sbint_to_irq(struct sbus_dev *sdev, unsigned int sbint)
{
	if (sbint >= sizeof(sun4c_pil_map)) {
		printk(KERN_ERR "%s: bogus SBINT %d\n", sdev->prom_name, sbint);
		BUG();
	}
	return sun4c_pil_map[sbint];
}

static void sun4c_disable_irq(unsigned int irq_nr)
{
	unsigned long flags;
	unsigned char current_mask, new_mask;
    
	local_irq_save(flags);
	irq_nr &= (NR_IRQS - 1);
	current_mask = *interrupt_enable;
	switch(irq_nr) {
	case 1:
		new_mask = ((current_mask) & (~(SUN4C_INT_E1)));
		break;
	case 8:
		new_mask = ((current_mask) & (~(SUN4C_INT_E8)));
		break;
	case 10:
		new_mask = ((current_mask) & (~(SUN4C_INT_E10)));
		break;
	case 14:
		new_mask = ((current_mask) & (~(SUN4C_INT_E14)));
		break;
	default:
		local_irq_restore(flags);
		return;
	}
	*interrupt_enable = new_mask;
	local_irq_restore(flags);
}

static void sun4c_enable_irq(unsigned int irq_nr)
{
	unsigned long flags;
	unsigned char current_mask, new_mask;
    
	local_irq_save(flags);
	irq_nr &= (NR_IRQS - 1);
	current_mask = *interrupt_enable;
	switch(irq_nr) {
	case 1:
		new_mask = ((current_mask) | SUN4C_INT_E1);
		break;
	case 8:
		new_mask = ((current_mask) | SUN4C_INT_E8);
		break;
	case 10:
		new_mask = ((current_mask) | SUN4C_INT_E10);
		break;
	case 14:
		new_mask = ((current_mask) | SUN4C_INT_E14);
		break;
	default:
		local_irq_restore(flags);
		return;
	}
	*interrupt_enable = new_mask;
	local_irq_restore(flags);
}

#define TIMER_IRQ  	10    /* Also at level 14, but we ignore that one. */
#define PROFILE_IRQ	14    /* Level14 ticker.. used by OBP for polling */

volatile struct sun4c_timer_info *sun4c_timers;

#ifdef CONFIG_SUN4
/* This is an ugly hack to work around the
   current timer code, and make it work with 
   the sun4/260 intersil 
   */
volatile struct sun4c_timer_info sun4_timer;
#endif

static void sun4c_clear_clock_irq(void)
{
	volatile unsigned int clear_intr;
#ifdef CONFIG_SUN4
	if (idprom->id_machtype == (SM_SUN4 | SM_4_260)) 
	  clear_intr = sun4_timer.timer_limit10;
	else
#endif
	clear_intr = sun4c_timers->timer_limit10;
}

static void sun4c_clear_profile_irq(int cpu)
{
	/* Errm.. not sure how to do this.. */
}

static void sun4c_load_profile_irq(int cpu, unsigned int limit)
{
	/* Errm.. not sure how to do this.. */
}

static void __init sun4c_init_timers(irqreturn_t (*counter_fn)(int, void *, struct pt_regs *))
{
	int irq;

	/* Map the Timer chip, this is implemented in hardware inside
	 * the cache chip on the sun4c.
	 */
#ifdef CONFIG_SUN4
	if (idprom->id_machtype == (SM_SUN4 | SM_4_260))
		sun4c_timers = &sun4_timer;
	else
#endif
	sun4c_timers = ioremap(SUN_TIMER_PHYSADDR,
	    sizeof(struct sun4c_timer_info));

	/* Have the level 10 timer tick at 100HZ.  We don't touch the
	 * level 14 timer limit since we are letting the prom handle
	 * them until we have a real console driver so L1-A works.
	 */
	sun4c_timers->timer_limit10 = (((1000000/HZ) + 1) << 10);
	master_l10_counter = &sun4c_timers->cur_count10;
	master_l10_limit = &sun4c_timers->timer_limit10;

	irq = request_irq(TIMER_IRQ,
			  counter_fn,
			  (SA_INTERRUPT | SA_STATIC_ALLOC),
			  "timer", NULL);
	if (irq) {
		prom_printf("time_init: unable to attach IRQ%d\n",TIMER_IRQ);
		prom_halt();
	}
    
#if 0
	/* This does not work on 4/330 */
	sun4c_enable_irq(10);
#endif
	claim_ticker14(NULL, PROFILE_IRQ, 0);
}

#ifdef CONFIG_SMP
static void sun4c_nop(void) {}
#endif

extern char *sun4m_irq_itoa(unsigned int irq);

void __init sun4c_init_IRQ(void)
{
	struct linux_prom_registers int_regs[2];
	int ie_node;

	if (ARCH_SUN4) {
		interrupt_enable = (char *)
		    ioremap(sun4_ie_physaddr, PAGE_SIZE);
	} else {
		struct resource phyres;

		ie_node = prom_searchsiblings (prom_getchild(prom_root_node),
				       	"interrupt-enable");
		if(ie_node == 0)
			panic("Cannot find /interrupt-enable node");

		/* Depending on the "address" property is bad news... */
		prom_getproperty(ie_node, "reg", (char *) int_regs, sizeof(int_regs));
		memset(&phyres, 0, sizeof(struct resource));
		phyres.flags = int_regs[0].which_io;
		phyres.start = int_regs[0].phys_addr;
		interrupt_enable = (char *) sbus_ioremap(&phyres, 0,
		    int_regs[0].reg_size, "sun4c_intr");
	}

	BTFIXUPSET_CALL(sbint_to_irq, sun4c_sbint_to_irq, BTFIXUPCALL_NORM);
	BTFIXUPSET_CALL(enable_irq, sun4c_enable_irq, BTFIXUPCALL_NORM);
	BTFIXUPSET_CALL(disable_irq, sun4c_disable_irq, BTFIXUPCALL_NORM);
	BTFIXUPSET_CALL(enable_pil_irq, sun4c_enable_irq, BTFIXUPCALL_NORM);
	BTFIXUPSET_CALL(disable_pil_irq, sun4c_disable_irq, BTFIXUPCALL_NORM);
	BTFIXUPSET_CALL(clear_clock_irq, sun4c_clear_clock_irq, BTFIXUPCALL_NORM);
	BTFIXUPSET_CALL(clear_profile_irq, sun4c_clear_profile_irq, BTFIXUPCALL_NOP);
	BTFIXUPSET_CALL(load_profile_irq, sun4c_load_profile_irq, BTFIXUPCALL_NOP);
	BTFIXUPSET_CALL(__irq_itoa, sun4m_irq_itoa, BTFIXUPCALL_NORM);
	sparc_init_timers = sun4c_init_timers;
#ifdef CONFIG_SMP
	BTFIXUPSET_CALL(set_cpu_int, sun4c_nop, BTFIXUPCALL_NOP);
	BTFIXUPSET_CALL(clear_cpu_int, sun4c_nop, BTFIXUPCALL_NOP);
	BTFIXUPSET_CALL(set_irq_udt, sun4c_nop, BTFIXUPCALL_NOP);
#endif
	*interrupt_enable = (SUN4C_INT_ENABLE);
	/* Cannot enable interrupts until OBP ticker is disabled. */
}
