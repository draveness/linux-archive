/*
 *  arch/ppc/platforms/pmac_cpufreq.c
 *
 *  Copyright (C) 2002 - 2004 Benjamin Herrenschmidt <benh@kernel.crashing.org>
 *  Copyright (C) 2004        John Steele Scott <toojays@toojays.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/adb.h>
#include <linux/pmu.h>
#include <linux/slab.h>
#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/sysdev.h>
#include <linux/i2c.h>
#include <asm/prom.h>
#include <asm/machdep.h>
#include <asm/irq.h>
#include <asm/hardirq.h>
#include <asm/pmac_feature.h>
#include <asm/mmu_context.h>
#include <asm/sections.h>
#include <asm/cputable.h>
#include <asm/time.h>

/* WARNING !!! This will cause calibrate_delay() to be called,
 * but this is an __init function ! So you MUST go edit
 * init/main.c to make it non-init before enabling DEBUG_FREQ
 */
#undef DEBUG_FREQ

/*
 * There is a problem with the core cpufreq code on SMP kernels,
 * it won't recalculate the Bogomips properly
 */
#ifdef CONFIG_SMP
#warning "WARNING, CPUFREQ not recommended on SMP kernels"
#endif

extern void low_choose_7447a_dfs(int dfs);
extern void low_choose_750fx_pll(int pll);
extern void low_sleep_handler(void);
extern void openpic_suspend(struct sys_device *sysdev, u32 state);
extern void openpic_resume(struct sys_device *sysdev);
extern void enable_kernel_altivec(void);
extern void enable_kernel_fp(void);

/*
 * Currently, PowerMac cpufreq supports only high & low frequencies
 * that are set by the firmware
 */
static unsigned int low_freq;
static unsigned int hi_freq;
static unsigned int cur_freq;

/*
 * Different models uses different mecanisms to switch the frequency
 */
static int (*set_speed_proc)(int low_speed);

/*
 * Some definitions used by the various speedprocs
 */
static u32 voltage_gpio;
static u32 frequency_gpio;
static u32 slew_done_gpio;


#define PMAC_CPU_LOW_SPEED	1
#define PMAC_CPU_HIGH_SPEED	0

/* There are only two frequency states for each processor. Values
 * are in kHz for the time being.
 */
#define CPUFREQ_HIGH                  PMAC_CPU_HIGH_SPEED
#define CPUFREQ_LOW                   PMAC_CPU_LOW_SPEED

static struct cpufreq_frequency_table pmac_cpu_freqs[] = {
	{CPUFREQ_HIGH, 		0},
	{CPUFREQ_LOW,		0},
	{0,			CPUFREQ_TABLE_END},
};

static inline void wakeup_decrementer(void)
{
	set_dec(tb_ticks_per_jiffy);
	/* No currently-supported powerbook has a 601,
	 * so use get_tbl, not native
	 */
	last_jiffy_stamp(0) = tb_last_stamp = get_tbl();
}

#ifdef DEBUG_FREQ
static inline void debug_calc_bogomips(void)
{
	/* This will cause a recalc of bogomips and display the
	 * result. We backup/restore the value to avoid affecting the
	 * core cpufreq framework's own calculation.
	 */
	extern void calibrate_delay(void);

	unsigned long save_lpj = loops_per_jiffy;
	calibrate_delay();
	loops_per_jiffy = save_lpj;
}
#endif /* DEBUG_FREQ */

/* Switch CPU speed under 750FX CPU control
 */
static int __pmac cpu_750fx_cpu_speed(int low_speed)
{
#ifdef DEBUG_FREQ
	printk(KERN_DEBUG "HID1, before: %x\n", mfspr(SPRN_HID1));
#endif
#ifdef CONFIG_6xx
	low_choose_750fx_pll(low_speed);
#endif
#ifdef DEBUG_FREQ
	printk(KERN_DEBUG "HID1, after: %x\n", mfspr(SPRN_HID1));
	debug_calc_bogomips();
#endif

	return 0;
}

/* Switch CPU speed using DFS */
static int __pmac dfs_set_cpu_speed(int low_speed)
{
	if (low_speed == 0) {
		/* ramping up, set voltage first */
		pmac_call_feature(PMAC_FTR_WRITE_GPIO, NULL, voltage_gpio, 0x05);
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ/1000);
	} else {
		/* ramping down, enable aack delay first */
		pmac_call_feature(PMAC_FTR_AACK_DELAY_ENABLE, NULL, 1, 0);
	}

	/* set frequency */
	low_choose_7447a_dfs(low_speed);

	if (low_speed == 1) {
		/* ramping down, set voltage last */
		pmac_call_feature(PMAC_FTR_WRITE_GPIO, NULL, voltage_gpio, 0x04);
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ/1000);
	} else {
		/* ramping up, disable aack delay last */
		pmac_call_feature(PMAC_FTR_AACK_DELAY_ENABLE, NULL, 0, 0);
	}

	return 0;
}


/* Switch CPU speed using slewing GPIOs
 */
static int __pmac gpios_set_cpu_speed(int low_speed)
{
	int gpio;

	/* If ramping up, set voltage first */
	if (low_speed == 0) {
		pmac_call_feature(PMAC_FTR_WRITE_GPIO, NULL, voltage_gpio, 0x05);
		/* Delay is way too big but it's ok, we schedule */
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ/100);
	}

	/* Set frequency */
	pmac_call_feature(PMAC_FTR_WRITE_GPIO, NULL, frequency_gpio,
			  low_speed ? 0x04 : 0x05);
	udelay(200);
	do {
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(1);
		gpio = pmac_call_feature(PMAC_FTR_READ_GPIO, NULL, slew_done_gpio, 0);
	} while((gpio & 0x02) == 0);

	/* If ramping down, set voltage last */
	if (low_speed == 1) {
		pmac_call_feature(PMAC_FTR_WRITE_GPIO, NULL, voltage_gpio, 0x04);
		/* Delay is way too big but it's ok, we schedule */
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ/100);
	}

#ifdef DEBUG_FREQ
	debug_calc_bogomips();
#endif

	return 0;
}

/* Switch CPU speed under PMU control
 */
static int __pmac pmu_set_cpu_speed(int low_speed)
{
	struct adb_request req;
	unsigned long save_l2cr;
	unsigned long save_l3cr;

	preempt_disable();

#ifdef DEBUG_FREQ
	printk(KERN_DEBUG "HID1, before: %x\n", mfspr(SPRN_HID1));
#endif
	/* Disable all interrupt sources on openpic */
	openpic_suspend(NULL, 1);

	/* Make sure the decrementer won't interrupt us */
	asm volatile("mtdec %0" : : "r" (0x7fffffff));
	/* Make sure any pending DEC interrupt occuring while we did
	 * the above didn't re-enable the DEC */
	mb();
	asm volatile("mtdec %0" : : "r" (0x7fffffff));

	/* We can now disable MSR_EE */
	local_irq_disable();

	/* Giveup the FPU & vec */
	enable_kernel_fp();

#ifdef CONFIG_ALTIVEC
	if (cur_cpu_spec[0]->cpu_features & CPU_FTR_ALTIVEC)
		enable_kernel_altivec();
#endif /* CONFIG_ALTIVEC */

	/* Save & disable L2 and L3 caches */
	save_l3cr = _get_L3CR();	/* (returns -1 if not available) */
	save_l2cr = _get_L2CR();	/* (returns -1 if not available) */
	if (save_l3cr != 0xffffffff && (save_l3cr & L3CR_L3E) != 0)
		_set_L3CR(save_l3cr & 0x7fffffff);
	if (save_l2cr != 0xffffffff && (save_l2cr & L2CR_L2E) != 0)
		_set_L2CR(save_l2cr & 0x7fffffff);

	/* Send the new speed command. My assumption is that this command
	 * will cause PLL_CFG[0..3] to be changed next time CPU goes to sleep
	 */
	pmu_request(&req, NULL, 6, PMU_CPU_SPEED, 'W', 'O', 'O', 'F', low_speed);
	while (!req.complete)
		pmu_poll();

	/* Prepare the northbridge for the speed transition */
	pmac_call_feature(PMAC_FTR_SLEEP_STATE,NULL,1,1);

	/* Call low level code to backup CPU state and recover from
	 * hardware reset
	 */
	low_sleep_handler();

	/* Restore the northbridge */
	pmac_call_feature(PMAC_FTR_SLEEP_STATE,NULL,1,0);

	/* Restore L2 cache */
	if (save_l2cr != 0xffffffff && (save_l2cr & L2CR_L2E) != 0)
 		_set_L2CR(save_l2cr);
	/* Restore L3 cache */
	if (save_l3cr != 0xffffffff && (save_l3cr & L3CR_L3E) != 0)
 		_set_L3CR(save_l3cr);

	/* Restore userland MMU context */
	set_context(current->active_mm->context, current->active_mm->pgd);

#ifdef DEBUG_FREQ
	printk(KERN_DEBUG "HID1, after: %x\n", mfspr(SPRN_HID1));
#endif

	/* Restore low level PMU operations */
	pmu_unlock();

	/* Restore decrementer */
	wakeup_decrementer();

	/* Restore interrupts */
	openpic_resume(NULL);

	/* Let interrupts flow again ... */
	local_irq_enable();

#ifdef DEBUG_FREQ
	debug_calc_bogomips();
#endif

	preempt_enable();

	return 0;
}

static int __pmac do_set_cpu_speed(int speed_mode)
{
	struct cpufreq_freqs freqs;
	int rc;

	freqs.old = cur_freq;
	freqs.new = (speed_mode == PMAC_CPU_HIGH_SPEED) ? hi_freq : low_freq;
	freqs.cpu = smp_processor_id();

	if (freqs.old == freqs.new)
		return 0;

	cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
	set_speed_proc(speed_mode == PMAC_CPU_LOW_SPEED);
	cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);
	cur_freq = (speed_mode == PMAC_CPU_HIGH_SPEED) ? hi_freq : low_freq;

	return rc;
}

static int __pmac pmac_cpufreq_verify(struct cpufreq_policy *policy)
{
	return cpufreq_frequency_table_verify(policy, pmac_cpu_freqs);
}

static int __pmac pmac_cpufreq_target(	struct cpufreq_policy *policy,
					unsigned int target_freq,
					unsigned int relation)
{
	unsigned int    newstate = 0;

	if (cpufreq_frequency_table_target(policy, pmac_cpu_freqs,
			target_freq, relation, &newstate))
		return -EINVAL;

	return do_set_cpu_speed(newstate);
}

unsigned int __pmac pmac_get_one_cpufreq(int i)
{
	/* Supports only one CPU for now */
	return (i == 0) ? cur_freq : 0;
}

static int __pmac pmac_cpufreq_cpu_init(struct cpufreq_policy *policy)
{
	if (policy->cpu != 0)
		return -ENODEV;

	policy->governor = CPUFREQ_DEFAULT_GOVERNOR;
	policy->cpuinfo.transition_latency	= CPUFREQ_ETERNAL;
	policy->cur = cur_freq;

	return cpufreq_frequency_table_cpuinfo(policy, &pmac_cpu_freqs[0]);
}

static u32 __pmac read_gpio(struct device_node *np)
{
	u32 *reg = (u32 *)get_property(np, "reg", NULL);

	if (reg == NULL)
		return 0;
	/* That works for all keylargos but shall be fixed properly
	 * some day...
	 */
	return 0x50 + (*reg);
}

static struct cpufreq_driver pmac_cpufreq_driver = {
	.verify 	= pmac_cpufreq_verify,
	.target 	= pmac_cpufreq_target,
	.init		= pmac_cpufreq_cpu_init,
	.name		= "powermac",
	.owner		= THIS_MODULE,
};


static int __pmac pmac_cpufreq_init_MacRISC3(struct device_node *cpunode)
{
	struct device_node *volt_gpio_np = of_find_node_by_name(NULL,
								"voltage-gpio");
	struct device_node *freq_gpio_np = of_find_node_by_name(NULL,
								"frequency-gpio");
	struct device_node *slew_done_gpio_np = of_find_node_by_name(NULL,
								     "slewing-done");
	u32 *value;

	/*
	 * Check to see if it's GPIO driven or PMU only
	 *
	 * The way we extract the GPIO address is slightly hackish, but it
	 * works well enough for now. We need to abstract the whole GPIO
	 * stuff sooner or later anyway
	 */

	if (volt_gpio_np)
		voltage_gpio = read_gpio(volt_gpio_np);
	if (freq_gpio_np)
		frequency_gpio = read_gpio(freq_gpio_np);
	if (slew_done_gpio_np)
		slew_done_gpio = read_gpio(slew_done_gpio_np);

	/* If we use the frequency GPIOs, calculate the min/max speeds based
	 * on the bus frequencies
	 */
	if (frequency_gpio && slew_done_gpio) {
		int lenp, rc;
		u32 *freqs, *ratio;

		freqs = (u32 *)get_property(cpunode, "bus-frequencies", &lenp);
		lenp /= sizeof(u32);
		if (freqs == NULL || lenp != 2) {
			printk(KERN_ERR "cpufreq: bus-frequencies incorrect or missing\n");
			return 1;
		}
		ratio = (u32 *)get_property(cpunode, "processor-to-bus-ratio*2", NULL);
		if (ratio == NULL) {
			printk(KERN_ERR "cpufreq: processor-to-bus-ratio*2 missing\n");
			return 1;
		}

		/* Get the min/max bus frequencies */
		low_freq = min(freqs[0], freqs[1]);
		hi_freq = max(freqs[0], freqs[1]);

		/* Grrrr.. It _seems_ that the device-tree is lying on the low bus
		 * frequency, it claims it to be around 84Mhz on some models while
		 * it appears to be approx. 101Mhz on all. Let's hack around here...
		 * fortunately, we don't need to be too precise
		 */
		if (low_freq < 98000000)
			low_freq = 101000000;
			
		/* Convert those to CPU core clocks */
		low_freq = (low_freq * (*ratio)) / 2000;
		hi_freq = (hi_freq * (*ratio)) / 2000;

		/* Now we get the frequencies, we read the GPIO to see what is out current
		 * speed
		 */
		rc = pmac_call_feature(PMAC_FTR_READ_GPIO, NULL, frequency_gpio, 0);
		cur_freq = (rc & 0x01) ? hi_freq : low_freq;

		set_speed_proc = gpios_set_cpu_speed;
		return 1;
	}

	/* If we use the PMU, look for the min & max frequencies in the
	 * device-tree
	 */
	value = (u32 *)get_property(cpunode, "min-clock-frequency", NULL);
	if (!value)
		return 1;
	low_freq = (*value) / 1000;
	/* The PowerBook G4 12" (PowerBook6,1) has an error in the device-tree
	 * here */
	if (low_freq < 100000)
		low_freq *= 10;

	value = (u32 *)get_property(cpunode, "max-clock-frequency", NULL);
	if (!value)
		return 1;
	hi_freq = (*value) / 1000;
	set_speed_proc = pmu_set_cpu_speed;

	return 0;
}

static int __pmac pmac_cpufreq_init_7447A(struct device_node *cpunode)
{
	struct device_node *volt_gpio_np;
	u32 *reg;

	/* OF only reports the high frequency */
	hi_freq = cur_freq;
	low_freq = cur_freq/2;
	if (mfspr(HID1) & HID1_DFS)
		cur_freq = low_freq;
	else
		cur_freq = hi_freq;

	volt_gpio_np = of_find_node_by_name(NULL, "cpu-vcore-select");
	if (!volt_gpio_np){
		printk(KERN_ERR "cpufreq: missing cpu-vcore-select gpio\n");
		return 1;
	}

	reg = (u32 *)get_property(volt_gpio_np, "reg", NULL);
	voltage_gpio = *reg;
	set_speed_proc = dfs_set_cpu_speed;

	return 0;
}

/* Currently, we support the following machines:
 *
 *  - Titanium PowerBook 1Ghz (PMU based, 667Mhz & 1Ghz)
 *  - Titanium PowerBook 800 (PMU based, 667Mhz & 800Mhz)
 *  - Titanium PowerBook 400 (PMU based, 300Mhz & 400Mhz)
 *  - Titanium PowerBook 500 (PMU based, 300Mhz & 500Mhz)
 *  - iBook2 500 (PMU based, 400Mhz & 500Mhz)
 *  - iBook2 700 (CPU based, 400Mhz & 700Mhz, support low voltage)
 *  - Recent MacRISC3 laptops
 *  - iBook G4s and PowerBook G4s with 7447A CPUs
 */
static int __init pmac_cpufreq_setup(void)
{
	struct device_node	*cpunode;
	u32			*value;

	if (strstr(cmd_line, "nocpufreq"))
		return 0;

	/* Assume only one CPU */
	cpunode = find_type_devices("cpu");
	if (!cpunode)
		goto out;

	/* Get current cpu clock freq */
	value = (u32 *)get_property(cpunode, "clock-frequency", NULL);
	if (!value)
		goto out;
	cur_freq = (*value) / 1000;

	/*  Check for 7447A based iBook G4 or PowerBook */
	if (machine_is_compatible("PowerBook6,5") ||
	    machine_is_compatible("PowerBook6,4") ||
	    machine_is_compatible("PowerBook5,5") ||
	    machine_is_compatible("PowerBook5,4")) {
		pmac_cpufreq_init_7447A(cpunode);
	/* Check for other MacRISC3 machines */
	} else if (machine_is_compatible("PowerBook3,4") ||
		   machine_is_compatible("PowerBook3,5") ||
		   machine_is_compatible("MacRISC3")) {
		pmac_cpufreq_init_MacRISC3(cpunode);
	/* Else check for iBook2 500 */
	} else if (machine_is_compatible("PowerBook4,1")) {
		/* We only know about 500Mhz model */
		if (cur_freq < 450000 || cur_freq > 550000)
			goto out;
		hi_freq = cur_freq;
		low_freq = 400000;
		set_speed_proc = pmu_set_cpu_speed;
	}
	/* Else check for TiPb 400 & 500 */
	else if (machine_is_compatible("PowerBook3,2")) {
		/* We only know about the 400 MHz and the 500Mhz model
		 * they both have 300 MHz as low frequency
		 */
		if (cur_freq < 350000 || cur_freq > 550000)
			goto out;
		hi_freq = cur_freq;
		low_freq = 300000;
		set_speed_proc = pmu_set_cpu_speed;
	}
	/* Else check for 750FX */
	else if (PVR_VER(mfspr(PVR)) == 0x7000) {
		if (get_property(cpunode, "dynamic-power-step", NULL) == NULL)
			goto out;
		hi_freq = cur_freq;
		value = (u32 *)get_property(cpunode, "reduced-clock-frequency", NULL);
		if (!value)
			goto out;
		low_freq = (*value) / 1000;		
		set_speed_proc = cpu_750fx_cpu_speed;
	}
out:
	if (set_speed_proc == NULL)
		return -ENODEV;

	pmac_cpu_freqs[CPUFREQ_LOW].frequency = low_freq;
	pmac_cpu_freqs[CPUFREQ_HIGH].frequency = hi_freq;

	printk(KERN_INFO "Registering PowerMac CPU frequency driver\n");
	printk(KERN_INFO "Low: %d Mhz, High: %d Mhz, Boot: %d Mhz\n",
	       low_freq/1000, hi_freq/1000, cur_freq/1000);

	return cpufreq_register_driver(&pmac_cpufreq_driver);
}

module_init(pmac_cpufreq_setup);

