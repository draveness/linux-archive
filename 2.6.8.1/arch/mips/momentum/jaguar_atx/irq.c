/*
 * Copyright (C) 2002 Momentum Computer, Inc.
 * Author: Matthew Dharm, mdharm@momenco.com
 *
 * Based on work by:
 *   Copyright (C) 2000 RidgeRun, Inc.
 *   Author: RidgeRun, Inc.
 *   glonnon@ridgerun.com, skranz@ridgerun.com, stevej@ridgerun.com
 *
 *   Copyright 2001 MontaVista Software Inc.
 *   Author: Jun Sun, jsun@mvista.com or jsun@junsun.net
 *
 *   Copyright (C) 2000, 2001 Ralf Baechle (ralf@gnu.org)
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
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/signal.h>
#include <linux/types.h>
#include <asm/irq_cpu.h>
#include <asm/mipsregs.h>

extern asmlinkage void jaguar_handle_int(void);

static struct irqaction cascade_mv64340 = {
	no_action, SA_INTERRUPT, CPU_MASK_NONE, "MV64340-Cascade", NULL, NULL
};

void __init init_IRQ(void)
{
	/*
	 * Clear all of the interrupts while we change the able around a bit.
	 * int-handler is not on bootstrap
	 */
	clear_c0_status(ST0_IM);

	/* Sets the first-level interrupt dispatcher. */
	set_except_vector(0, jaguar_handle_int);
	init_generic_irq();
	mips_cpu_irq_init(0);
	rm7k_cpu_irq_init(8);

	/* set up the cascading interrupts */
	setup_irq(8, &cascade_mv64340);

	mv64340_irq_init(16);

	set_c0_status(ST0_IM);
}
