/*
 * PXA250/210 Power Management Routines
 *
 * Original code for the SA11x0:
 * Copyright (c) 2001 Cliff Brake <cbrake@accelent.com>
 *
 * Modified for the PXA250 by Nicolas Pitre:
 * Copyright (c) 2002 Monta Vista Software, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/suspend.h>
#include <linux/errno.h>
#include <linux/time.h>

#include <asm/hardware.h>
#include <asm/memory.h>
#include <asm/system.h>
#include <asm/arch/pm.h>
#include <asm/arch/pxa-regs.h>
#include <asm/arch/lubbock.h>
#include <asm/mach/time.h>

struct pxa_cpu_pm_fns *pxa_cpu_pm_fns;
static unsigned long *sleep_save;

int pxa_pm_enter(suspend_state_t state)
{
	unsigned long sleep_save_checksum = 0, checksum = 0;
	int i;

#ifdef CONFIG_IWMMXT
	/* force any iWMMXt context to ram **/
	if (elf_hwcap & HWCAP_IWMMXT)
		iwmmxt_task_disable(NULL);
#endif

	pxa_cpu_pm_fns->save(sleep_save);

	/* Clear sleep reset status */
	RCSR = RCSR_SMR;

	/* before sleeping, calculate and save a checksum */
	for (i = 0; i < pxa_cpu_pm_fns->save_size - 1; i++)
		sleep_save_checksum += sleep_save[i];

	/* *** go zzz *** */
	pxa_cpu_pm_fns->enter(state);
	cpu_init();

	/* after sleeping, validate the checksum */
	for (i = 0; i < pxa_cpu_pm_fns->save_size - 1; i++)
		checksum += sleep_save[i];

	/* if invalid, display message and wait for a hardware reset */
	if (checksum != sleep_save_checksum) {
#ifdef CONFIG_ARCH_LUBBOCK
		LUB_HEXLED = 0xbadbadc5;
#endif
		while (1)
			pxa_cpu_pm_fns->enter(state);
	}

	pxa_cpu_pm_fns->restore(sleep_save);

	pr_debug("*** made it back from resume\n");

	return 0;
}

EXPORT_SYMBOL_GPL(pxa_pm_enter);

unsigned long sleep_phys_sp(void *sp)
{
	return virt_to_phys(sp);
}

static int pxa_pm_valid(suspend_state_t state)
{
	if (pxa_cpu_pm_fns)
		return pxa_cpu_pm_fns->valid(state);

	return -EINVAL;
}

static struct pm_ops pxa_pm_ops = {
	.valid		= pxa_pm_valid,
	.enter		= pxa_pm_enter,
};

static int __init pxa_pm_init(void)
{
	if (!pxa_cpu_pm_fns) {
		printk(KERN_ERR "no valid pxa_cpu_pm_fns defined\n");
		return -EINVAL;
	}

	sleep_save = kmalloc(pxa_cpu_pm_fns->save_size, GFP_KERNEL);
	if (!sleep_save) {
		printk(KERN_ERR "failed to alloc memory for pm save\n");
		return -ENOMEM;
	}

	pm_set_ops(&pxa_pm_ops);
	return 0;
}

device_initcall(pxa_pm_init);
