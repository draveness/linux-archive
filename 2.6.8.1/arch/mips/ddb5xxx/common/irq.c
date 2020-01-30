/*
 * Copyright 2001 MontaVista Software Inc.
 * Author: jsun@mvista.com or jsun@junsun.net
 *
 * arch/mips/ddb5xxx/common/irq.c
 *     Common irq code for DDB boards.  This really should belong
 *	arch/mips/kernel/irq.c.  Need to talk to Ralf.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */
#include <linux/config.h>
#include <linux/init.h>
#include <asm/irq.h>

void (*irq_setup)(void);

#ifdef CONFIG_KGDB
static int kgdb_flag = 1;
static int __init nokgdb(char *str)
{
	kgdb_flag = 0;
	return 1;
}
__setup("nokgdb", nokgdb);
#endif

void __init init_IRQ(void)
{
#ifdef CONFIG_KGDB
	extern void breakpoint(void);
	extern void set_debug_traps(void);

	if (kgdb_flag) {
		printk("Wait for gdb client connection ...\n");
		set_debug_traps();
		breakpoint();
	}
#endif
	/* set up default irq controller */
	init_generic_irq();

	/* invoke board-specific irq setup */
	irq_setup();
}
