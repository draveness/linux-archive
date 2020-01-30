/*
 * BRIEF MODULE DESCRIPTION
 * Code to handle irqs on GT64120A boards
 *  Derived from mips/orion and Cort <cort@fsmlabs.com>
 *
 * Copyright (C) 2000 RidgeRun, Inc.
 * Author: RidgeRun, Inc.
 *   glonnon@ridgerun.com, skranz@ridgerun.com, stevej@ridgerun.com
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 *  THIS  SOFTWARE  IS PROVIDED   ``AS  IS'' AND   ANY  EXPRESS OR IMPLIED
 *  WARRANTIES,   INCLUDING, BUT NOT  LIMITED  TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
 *  NO  EVENT  SHALL   THE AUTHOR  BE    LIABLE FOR ANY   DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *  NOT LIMITED   TO, PROCUREMENT OF  SUBSTITUTE GOODS  OR SERVICES; LOSS OF
 *  USE, DATA,  OR PROFITS; OR  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 *  ANY THEORY OF LIABILITY, WHETHER IN  CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 *  THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the  GNU General Public License along
 *  with this program; if not, write  to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel_stat.h>
#include <linux/module.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/timex.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <asm/bitops.h>
#include <asm/bootinfo.h>
#include <asm/io.h>
#include <asm/mipsregs.h>
#include <asm/system.h>
#include <asm/gt64120.h>

asmlinkage inline void pci_intA(struct pt_regs *regs)
{
	do_IRQ(GT_INTA, regs);
}

asmlinkage inline void pci_intD(struct pt_regs *regs)
{
	do_IRQ(GT_INTD, regs);
}

static void disable_ev64120_irq(unsigned int irq_nr)
{
	unsigned long flags;

	local_irq_save(flags);
	if (irq_nr >= 8) {	// All PCI interrupts are on line 5 or 2
		clear_c0_status(9 << 10);
	} else {
		clear_c0_status(1 << (irq_nr + 8));
	}
	local_irq_restore(flags);
}

static void enable_ev64120_irq(unsigned int irq_nr)
{
	unsigned long flags;

	local_irq_save(flags);
	if (irq_nr >= 8)	// All PCI interrupts are on line 5 or 2
		set_c0_status(9 << 10);
	else
		set_c0_status(1 << (irq_nr + 8));
	local_irq_restore(flags);
}

static unsigned int startup_ev64120_irq(unsigned int irq)
{
	enable_ev64120_irq(irq);
	return 0;		/* Never anything pending  */
}

#define shutdown_ev64120_irq     disable_ev64120_irq
#define mask_and_ack_ev64120_irq disable_ev64120_irq

static void end_ev64120_irq(unsigned int irq)
{
	if (!(irq_desc[irq].status & (IRQ_DISABLED|IRQ_INPROGRESS)))
		enable_ev64120_irq(irq);
}

static struct hw_interrupt_type ev64120_irq_type = {
	.typename	= "EV64120",
	.startup	= startup_ev64120_irq,
	.shutdown	= shutdown_ev64120_irq,
	.enable		= enable_ev64120_irq,
	.disable	= disable_ev64120_irq,
	.ack		= mask_and_ack_ev64120_irq,
	.end		= end_ev64120_irq,
	.set_affinity	= NULL
};

void gt64120_irq_setup(void)
{
	extern asmlinkage void galileo_handle_int(void);

	/*
	 * Clear all of the interrupts while we change the able around a bit.
	 */
	clear_c0_status(ST0_IM);

	/* Sets the exception_handler array. */
	set_except_vector(0, galileo_handle_int);

	cli();

	/*
	 * Enable timer.  Other interrupts will be enabled as they are
	 * registered.
	 */
	set_c0_status(IE_IRQ2);
}

void __init init_IRQ(void)
{
	int i;

	/*  Let's initialize our IRQ descriptors  */
	for (i = 0; i < NR_IRQS; i++) {
		irq_desc[i].status = 0;
		irq_desc[i].handler = &no_irq_type;
		irq_desc[i].action = NULL;
		irq_desc[i].depth = 0;
		irq_desc[i].lock = SPIN_LOCK_UNLOCKED;
	}

	gt64120_irq_setup();
}
