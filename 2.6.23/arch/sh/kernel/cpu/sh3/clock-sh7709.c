/*
 * arch/sh/kernel/cpu/sh3/clock-sh7709.c
 *
 * SH7709 support for the clock framework
 *
 *  Copyright (C) 2005  Andriy Skulysh
 *
 * Based on arch/sh/kernel/cpu/sh3/clock-sh7705.c
 *  Copyright (C) 2005  Paul Mundt
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <asm/clock.h>
#include <asm/freq.h>
#include <asm/io.h>

static int stc_multipliers[] = { 1, 2, 4, 8, 3, 6, 1, 1 };
static int ifc_divisors[]    = { 1, 2, 4, 1, 3, 1, 1, 1 };
static int pfc_divisors[]    = { 1, 2, 4, 1, 3, 6, 1, 1 };

static void set_bus_parent(struct clk *clk)
{
	struct clk *bus_clk = clk_get(NULL, "bus_clk");
	clk->parent = bus_clk;
	clk_put(bus_clk);
}

static void master_clk_init(struct clk *clk)
{
	int frqcr = ctrl_inw(FRQCR);
	int idx = ((frqcr & 0x2000) >> 11) | (frqcr & 0x0003);

	clk->rate *= pfc_divisors[idx];
}

static struct clk_ops sh7709_master_clk_ops = {
	.init		= master_clk_init,
};

static void module_clk_recalc(struct clk *clk)
{
	int frqcr = ctrl_inw(FRQCR);
	int idx = ((frqcr & 0x2000) >> 11) | (frqcr & 0x0003);

	clk->rate = clk->parent->rate / pfc_divisors[idx];
}

static struct clk_ops sh7709_module_clk_ops = {
#ifdef CLOCK_MODE_0_1_2_7
	.init		= set_bus_parent,
#endif
	.recalc		= module_clk_recalc,
};

static void bus_clk_recalc(struct clk *clk)
{
	int frqcr = ctrl_inw(FRQCR);
	int idx = (frqcr & 0x0080) ?
		((frqcr & 0x8000) >> 13) | ((frqcr & 0x0030) >> 4) : 1;

	clk->rate = clk->parent->rate * stc_multipliers[idx];
}

static struct clk_ops sh7709_bus_clk_ops = {
	.recalc		= bus_clk_recalc,
};

static void cpu_clk_recalc(struct clk *clk)
{
	int frqcr = ctrl_inw(FRQCR);
	int idx = ((frqcr & 0x4000) >> 12) | ((frqcr & 0x000c) >> 2);

	clk->rate = clk->parent->rate / ifc_divisors[idx];
}

static struct clk_ops sh7709_cpu_clk_ops = {
	.init		= set_bus_parent,
	.recalc		= cpu_clk_recalc,
};

static struct clk_ops *sh7709_clk_ops[] = {
	&sh7709_master_clk_ops,
	&sh7709_module_clk_ops,
	&sh7709_bus_clk_ops,
	&sh7709_cpu_clk_ops,
};

void __init arch_init_clk_ops(struct clk_ops **ops, int idx)
{
	if (idx < ARRAY_SIZE(sh7709_clk_ops))
		*ops = sh7709_clk_ops[idx];
}
