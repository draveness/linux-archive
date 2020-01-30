/*
 * Copyright (C) 2004 Anton Blanchard <anton@au.ibm.com>, IBM
 *
 * Based on alpha version.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/oprofile.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/errno.h>
#include <asm/ptrace.h>
#include <asm/system.h>

#include "op_impl.h"

extern struct op_ppc64_model op_model_rs64;
extern struct op_ppc64_model op_model_power4;
static struct op_ppc64_model *model;

extern void (*perf_irq)(struct pt_regs *);
static void (*save_perf_irq)(struct pt_regs *);

static struct op_counter_config ctr[OP_MAX_COUNTER];
static struct op_system_config sys;

static void op_handle_interrupt(struct pt_regs *regs)
{
	model->handle_interrupt(regs, ctr);
}

static int op_ppc64_setup(void)
{
	/* Install our interrupt handler into the existing hook.  */
	save_perf_irq = perf_irq;
	perf_irq = op_handle_interrupt;

	mb();

	/* Pre-compute the values to stuff in the hardware registers.  */
	model->reg_setup(ctr, &sys, model->num_counters);

	/* Configure the registers on all cpus.  */
	on_each_cpu(model->cpu_setup, NULL, 0, 1);

	return 0;
}

static void op_ppc64_shutdown(void)
{
	/*
	 * We need to be sure we have cleared all pending exceptions before
	 * removing the interrupt handler. For the moment we play it safe and
	 * leave it in
	 */
#if 0
	mb();

	/* Remove our interrupt handler. We may be removing this module. */
	perf_irq = save_perf_irq;
#endif
}

static void op_ppc64_cpu_start(void *dummy)
{
	model->start(ctr);
}

static int op_ppc64_start(void)
{
	on_each_cpu(op_ppc64_cpu_start, NULL, 0, 1);
	return 0;
}

static inline void op_ppc64_cpu_stop(void *dummy)
{
	model->stop();
}

static void op_ppc64_stop(void)
{
	on_each_cpu(op_ppc64_cpu_stop, NULL, 0, 1);
}

static int op_ppc64_create_files(struct super_block *sb, struct dentry *root)
{
	int i;

	for (i = 0; i < model->num_counters; ++i) {
		struct dentry *dir;
		char buf[3];

		snprintf(buf, sizeof buf, "%d", i);
		dir = oprofilefs_mkdir(sb, root, buf);

		oprofilefs_create_ulong(sb, dir, "enabled", &ctr[i].enabled);
		oprofilefs_create_ulong(sb, dir, "event", &ctr[i].event);
		oprofilefs_create_ulong(sb, dir, "count", &ctr[i].count);
		/*
		 * We dont support per counter user/kernel selection, but
		 * we leave the entries because userspace expects them
		 */
		oprofilefs_create_ulong(sb, dir, "kernel", &ctr[i].kernel);
		oprofilefs_create_ulong(sb, dir, "user", &ctr[i].user);
		oprofilefs_create_ulong(sb, dir, "unit_mask", &ctr[i].unit_mask);
	}

	oprofilefs_create_ulong(sb, root, "enable_kernel", &sys.enable_kernel);
	oprofilefs_create_ulong(sb, root, "enable_user", &sys.enable_user);

	return 0;
}

static struct oprofile_operations oprof_ppc64_ops = {
	.create_files	= op_ppc64_create_files,
	.setup		= op_ppc64_setup,
	.shutdown	= op_ppc64_shutdown,
	.start		= op_ppc64_start,
	.stop		= op_ppc64_stop,
	.cpu_type	= NULL		/* To be filled in below. */
};

int __init oprofile_arch_init(struct oprofile_operations **ops)
{
	unsigned int pvr;

	pvr = _get_PVR();

	switch (PVR_VER(pvr)) {
		case PV_630:
		case PV_630p:
			model = &op_model_rs64;
			model->num_counters = 8;
			oprof_ppc64_ops.cpu_type = "ppc64/power3";
			break;

		case PV_NORTHSTAR:
		case PV_PULSAR:
		case PV_ICESTAR:
		case PV_SSTAR:
			model = &op_model_rs64;
			model->num_counters = 8;
			oprof_ppc64_ops.cpu_type = "ppc64/rs64";
			break;

		case PV_POWER4:
		case PV_POWER4p:
			model = &op_model_power4;
			model->num_counters = 8;
			oprof_ppc64_ops.cpu_type = "ppc64/power4";
			break;

		case PV_970:
		case PV_970FX:
			model = &op_model_power4;
			model->num_counters = 8;
			oprof_ppc64_ops.cpu_type = "ppc64/970";
			break;

		case PV_POWER5:
		case PV_POWER5p:
			model = &op_model_power4;
			model->num_counters = 6;
			oprof_ppc64_ops.cpu_type = "ppc64/power5";
			break;

		default:
			return -ENODEV;
	}

	*ops = &oprof_ppc64_ops;

	printk(KERN_INFO "oprofile: using %s performance monitoring.\n",
	       oprof_ppc64_ops.cpu_type);

	return 0;
}

void oprofile_arch_exit(void)
{
}
