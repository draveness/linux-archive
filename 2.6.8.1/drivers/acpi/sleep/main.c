/*
 * sleep.c - ACPI sleep support.
 *
 * Copyright (c) 2000-2003 Patrick Mochel
 * Copyright (c) 2003 Open Source Development Lab
 *
 * This file is released under the GPLv2.
 *
 */

#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/dmi.h>
#include <linux/device.h>
#include <linux/suspend.h>
#include <acpi/acpi_bus.h>
#include <acpi/acpi_drivers.h>
#include "sleep.h"

u8 sleep_states[ACPI_S_STATE_COUNT];

static struct pm_ops acpi_pm_ops;

extern void do_suspend_lowlevel_s4bios(void);
extern void do_suspend_lowlevel(void);

static u32 acpi_suspend_states[] = {
	[PM_SUSPEND_ON]		= ACPI_STATE_S0,
	[PM_SUSPEND_STANDBY]	= ACPI_STATE_S1,
	[PM_SUSPEND_MEM]	= ACPI_STATE_S3,
	[PM_SUSPEND_DISK]	= ACPI_STATE_S4,
};

static int init_8259A_after_S1;

/**
 *	acpi_pm_prepare - Do preliminary suspend work.
 *	@state:		suspend state we're entering.
 *
 *	Make sure we support the state. If we do, and we need it, set the
 *	firmware waking vector and do arch-specific nastiness to get the 
 *	wakeup code to the waking vector. 
 */

static int acpi_pm_prepare(u32 state)
{
	u32 acpi_state = acpi_suspend_states[state];

	if (!sleep_states[acpi_state])
		return -EPERM;

	/* do we have a wakeup address for S2 and S3? */
	/* Here, we support only S4BIOS, those we set the wakeup address */
	/* S4OS is only supported for now via swsusp.. */
	if (state == PM_SUSPEND_MEM || state == PM_SUSPEND_DISK) {
		if (!acpi_wakeup_address)
			return -EFAULT;
		acpi_set_firmware_waking_vector(
			(acpi_physical_address) acpi_wakeup_address);
	}
	ACPI_FLUSH_CPU_CACHE();
	acpi_enter_sleep_state_prep(acpi_state);
	return 0;
}


/**
 *	acpi_pm_enter - Actually enter a sleep state.
 *	@state:		State we're entering.
 *
 *	Flush caches and go to sleep. For STR or STD, we have to call 
 *	arch-specific assembly, which in turn call acpi_enter_sleep_state().
 *	It's unfortunate, but it works. Please fix if you're feeling frisky.
 */

static int acpi_pm_enter(u32 state)
{
	acpi_status status = AE_OK;
	unsigned long flags = 0;
	u32 acpi_state = acpi_suspend_states[state];

	ACPI_FLUSH_CPU_CACHE();

	/* Do arch specific saving of state. */
	if (state > PM_SUSPEND_STANDBY) {
		int error = acpi_save_state_mem();
		if (error)
			return error;
	}


	local_irq_save(flags);
	switch (state)
	{
	case PM_SUSPEND_STANDBY:
		barrier();
		status = acpi_enter_sleep_state(acpi_state);
		break;

	case PM_SUSPEND_MEM:
		do_suspend_lowlevel();
		break;

	case PM_SUSPEND_DISK:
		if (acpi_pm_ops.pm_disk_mode == PM_DISK_PLATFORM)
			status = acpi_enter_sleep_state(acpi_state);
		else
			do_suspend_lowlevel_s4bios();
		break;
	default:
		return -EINVAL;
	}
	local_irq_restore(flags);
	printk(KERN_DEBUG "Back to C!\n");

	/* restore processor state
	 * We should only be here if we're coming back from STR or STD.
	 * And, in the case of the latter, the memory image should have already
	 * been loaded from disk.
	 */
	if (state > PM_SUSPEND_STANDBY)
		acpi_restore_state_mem();


	return ACPI_SUCCESS(status) ? 0 : -EFAULT;
}


/**
 *	acpi_pm_finish - Finish up suspend sequence.
 *	@state:		State we're coming out of.
 *
 *	This is called after we wake back up (or if entering the sleep state
 *	failed). 
 */

static int acpi_pm_finish(u32 state)
{
	acpi_leave_sleep_state(state);

	/* reset firmware waking vector */
	acpi_set_firmware_waking_vector((acpi_physical_address) 0);

	if (init_8259A_after_S1) {
		printk("Broken toshiba laptop -> kicking interrupts\n");
		init_8259A(0);
	}
	return 0;
}


int acpi_suspend(u32 acpi_state)
{
	u32 states[] = {
		[1]	= PM_SUSPEND_STANDBY,
		[3]	= PM_SUSPEND_MEM,
		[4]	= PM_SUSPEND_DISK,
	};

	if (acpi_state <= 4 && states[acpi_state])
		return pm_suspend(states[acpi_state]);
	return -EINVAL;
}

static struct pm_ops acpi_pm_ops = {
	.prepare	= acpi_pm_prepare,
	.enter		= acpi_pm_enter,
	.finish		= acpi_pm_finish,
};


/*
 * Toshiba fails to preserve interrupts over S1, reinitialization
 * of 8259 is needed after S1 resume.
 */
static int __init init_ints_after_s1(struct dmi_system_id *d)
{
	printk(KERN_WARNING "%s with broken S1 detected.\n", d->ident);
	init_8259A_after_S1 = 1;
	return 0;
}

static struct dmi_system_id __initdata acpisleep_dmi_table[] = {
	{
		.callback = init_ints_after_s1,
		.ident = "Toshiba Satellite 4030cdt",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "S4030CDT/4.3"), },
	},
	{ },
};

static int __init acpi_sleep_init(void)
{
	int			i = 0;

	dmi_check_system(acpisleep_dmi_table);

	if (acpi_disabled)
		return 0;

	printk(KERN_INFO PREFIX "(supports");
	for (i=0; i<ACPI_S_STATE_COUNT; i++) {
		acpi_status status;
		u8 type_a, type_b;
		status = acpi_get_sleep_type_data(i, &type_a, &type_b);
		if (ACPI_SUCCESS(status)) {
			sleep_states[i] = 1;
			printk(" S%d", i);
		}
		if (i == ACPI_STATE_S4) {
			if (acpi_gbl_FACS->S4bios_f) {
				sleep_states[i] = 1;
				printk(" S4bios");
				acpi_pm_ops.pm_disk_mode = PM_DISK_FIRMWARE;
			} else if (sleep_states[i])
				acpi_pm_ops.pm_disk_mode = PM_DISK_PLATFORM;
		}
	}
	printk(")\n");

	pm_set_ops(&acpi_pm_ops);
	return 0;
}

late_initcall(acpi_sleep_init);
