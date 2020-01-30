/*
 * Copyright (C) 2000, 2001, 2002, 2003 Broadcom Corporation
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

/*
 * Setup code for the SWARM board
 */

#include <linux/config.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/bootmem.h>
#include <linux/blkdev.h>
#include <linux/init.h>

#include <asm/irq.h>
#include <asm/io.h>
#include <asm/bootinfo.h>
#include <asm/mipsregs.h>
#include <asm/reboot.h>
#include <asm/time.h>
#include <asm/traps.h>
#include <asm/pci_channel.h>
#include <asm/sibyte/sb1250.h>
#include <asm/sibyte/sb1250_regs.h>
#include <asm/sibyte/sb1250_genbus.h>
#include <asm/sibyte/board.h>

extern void sb1250_setup(void);

extern int xicor_probe(void);
extern int xicor_set_time(unsigned long);
extern unsigned long xicor_get_time(void);

extern int m41t81_probe(void);
extern int m41t81_set_time(unsigned long);
extern unsigned long m41t81_get_time(void);

const char *get_system_type(void)
{
	return "SiByte " SIBYTE_BOARD_NAME;
}

void __init swarm_timer_setup(struct irqaction *irq)
{
        /*
         * we don't set up irqaction, because we will deliver timer
         * interrupts through low-level (direct) meachanism.
         */

        /* We only need to setup the generic timer */
        sb1250_time_init();
}

int swarm_be_handler(struct pt_regs *regs, int is_fixup)
{
	if (!is_fixup && (regs->cp0_cause & 4)) {
		/* Data bus error - print PA */
#ifdef CONFIG_MIPS64
		printk("DBE physical address: %010lx\n",
		       __read_64bit_c0_register($26, 1));
#else
		printk("DBE physical address: %010llx\n",
		       __read_64bit_c0_split($26, 1));
#endif
	}
	return (is_fixup ? MIPS_BE_FIXUP : MIPS_BE_FATAL);
}

static int __init swarm_setup(void)
{
	extern int panic_timeout;

	sb1250_setup();

	panic_timeout = 5;  /* For debug.  */

	board_timer_setup = swarm_timer_setup;
	board_be_handler = swarm_be_handler;

	if (xicor_probe()) {
		printk("swarm setup: Xicor 1241 RTC detected.\n");
		rtc_get_time = xicor_get_time;
		rtc_set_time = xicor_set_time;
	}
 
	if (m41t81_probe()) {
		printk("swarm setup: M41T81 RTC detected.\n");
		rtc_get_time = m41t81_get_time;
		rtc_set_time = m41t81_set_time;
	}

	printk("This kernel optimized for "
#ifdef CONFIG_SIMULATION
	       "simulation"
#else
	       "board"
#endif
	       " runs "
#ifdef CONFIG_SIBYTE_CFE
	       "with"
#else
	       "without"
#endif
	       " CFE\n");

#ifdef CONFIG_VT
	screen_info = (struct screen_info) {
		0, 0,           /* orig-x, orig-y */
		0,              /* unused */
		52,             /* orig_video_page */
		3,              /* orig_video_mode */
		80,             /* orig_video_cols */
		4626, 3, 9,     /* unused, ega_bx, unused */
		25,             /* orig_video_lines */
		0x22,           /* orig_video_isVGA */
		16              /* orig_video_points */
       };
       /* XXXKW for CFE, get lines/cols from environment */
#endif

	return 0;
}

early_initcall(swarm_setup);

#ifdef LEDS_PHYS

#ifdef CONFIG_SIBYTE_CARMEL
/* XXXKW need to detect Monterey/LittleSur/etc */
#undef LEDS_PHYS
#define LEDS_PHYS MLEDS_PHYS
#endif

#define setled(index, c) \
  ((unsigned char *)(IOADDR(LEDS_PHYS)+0x20))[(3-(index))<<3] = (c)
void setleds(char *str)
{
	int i;
	for (i = 0; i < 4; i++) {
		if (!str[i]) {
			setled(i, ' ');
		} else {
			setled(i, str[i]);
		}
	}
}
#endif
