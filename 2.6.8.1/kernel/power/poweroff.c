/*
 * poweroff.c - sysrq handler to gracefully power down machine.
 *
 * This file is released under the GPL v2
 */

#include <linux/kernel.h>
#include <linux/sysrq.h>
#include <linux/init.h>
#include <linux/pm.h>
#include <linux/workqueue.h>

/*
 * When the user hits Sys-Rq o to power down the machine this is the
 * callback we use.
 */

static void do_poweroff(void *dummy)
{
	if (pm_power_off)
		pm_power_off();
}

static DECLARE_WORK(poweroff_work, do_poweroff, NULL);

static void handle_poweroff(int key, struct pt_regs *pt_regs,
				struct tty_struct *tty)
{
	schedule_work(&poweroff_work);
}

static struct sysrq_key_op	sysrq_poweroff_op = {
	.handler        = handle_poweroff,
	.help_msg       = "powerOff",
	.action_msg     = "Power Off\n"
};

static int pm_sysrq_init(void)
{
	register_sysrq_key('o', &sysrq_poweroff_op);
	return 0;
}

subsys_initcall(pm_sysrq_init);
