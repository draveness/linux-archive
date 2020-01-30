/*
 *
 * Procedures for interfacing to the RTAS on CHRP machines.
 *
 * Peter Bergner, IBM	March 2001.
 * Copyright (C) 2001 IBM.
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/init.h>

#include <asm/prom.h>
#include <asm/rtas.h>
#include <asm/semaphore.h>
#include <asm/machdep.h>
#include <asm/paca.h>
#include <asm/page.h>
#include <asm/param.h>
#include <asm/system.h>
#include <asm/abs_addr.h>
#include <asm/udbg.h>
#include <asm/delay.h>
#include <asm/uaccess.h>

struct flash_block_list_header rtas_firmware_flash_list = {0, NULL};

struct rtas_t rtas = { 
	.lock = SPIN_LOCK_UNLOCKED
};

char rtas_err_buf[RTAS_ERROR_LOG_MAX];

spinlock_t rtas_data_buf_lock = SPIN_LOCK_UNLOCKED;
char rtas_data_buf[RTAS_DATA_BUF_SIZE]__page_aligned;

void
call_rtas_display_status(char c)
{
	struct rtas_args *args = &rtas.args;
	unsigned long s;

	spin_lock_irqsave(&rtas.lock, s);

	args->token = 10;
	args->nargs = 1;
	args->nret  = 1;
	args->rets  = (rtas_arg_t *)&(args->args[1]);
	args->args[0] = (int)c;

	enter_rtas(__pa(args));

	spin_unlock_irqrestore(&rtas.lock, s);
}

int
rtas_token(const char *service)
{
	int *tokp;
	if (rtas.dev == NULL) {
		PPCDBG(PPCDBG_RTAS,"\tNo rtas device in device-tree...\n");
		return RTAS_UNKNOWN_SERVICE;
	}
	tokp = (int *) get_property(rtas.dev, service, NULL);
	return tokp ? *tokp : RTAS_UNKNOWN_SERVICE;
}


/** Return a copy of the detailed error text associated with the
 *  most recent failed call to rtas.  Because the error text
 *  might go stale if there are any other intervening rtas calls,
 *  this routine must be called atomically with whatever produced
 *  the error (i.e. with rtas.lock still held from the previous call).
 */
static int
__fetch_rtas_last_error(void)
{
	struct rtas_args err_args, save_args;

	err_args.token = rtas_token("rtas-last-error");
	err_args.nargs = 2;
	err_args.nret = 1;
	err_args.rets = (rtas_arg_t *)&(err_args.args[2]);

	err_args.args[0] = (rtas_arg_t)__pa(rtas_err_buf);
	err_args.args[1] = RTAS_ERROR_LOG_MAX;
	err_args.args[2] = 0;

	save_args = rtas.args;
	rtas.args = err_args;

	PPCDBG(PPCDBG_RTAS, "\tentering rtas with 0x%lx\n",
	       __pa(&err_args));
	enter_rtas(__pa(&rtas.args));
	PPCDBG(PPCDBG_RTAS, "\treturned from rtas ...\n");

	err_args = rtas.args;
	rtas.args = save_args;

	return err_args.rets[0];
}

int rtas_call(int token, int nargs, int nret, int *outputs, ...)
{
	va_list list;
	int i, logit = 0;
	unsigned long s;
	struct rtas_args *rtas_args;
	char * buff_copy = NULL;
	int ret;

	PPCDBG(PPCDBG_RTAS, "Entering rtas_call\n");
	PPCDBG(PPCDBG_RTAS, "\ttoken    = 0x%x\n", token);
	PPCDBG(PPCDBG_RTAS, "\tnargs    = %d\n", nargs);
	PPCDBG(PPCDBG_RTAS, "\tnret     = %d\n", nret);
	PPCDBG(PPCDBG_RTAS, "\t&outputs = 0x%lx\n", outputs);
	if (token == RTAS_UNKNOWN_SERVICE)
		return -1;

	/* Gotta do something different here, use global lock for now... */
	spin_lock_irqsave(&rtas.lock, s);
	rtas_args = &rtas.args;

	rtas_args->token = token;
	rtas_args->nargs = nargs;
	rtas_args->nret  = nret;
	rtas_args->rets  = (rtas_arg_t *)&(rtas_args->args[nargs]);
	va_start(list, outputs);
	for (i = 0; i < nargs; ++i) {
		rtas_args->args[i] = va_arg(list, rtas_arg_t);
		PPCDBG(PPCDBG_RTAS, "\tnarg[%d] = 0x%x\n", i, rtas_args->args[i]);
	}
	va_end(list);

	for (i = 0; i < nret; ++i)
		rtas_args->rets[i] = 0;

	PPCDBG(PPCDBG_RTAS, "\tentering rtas with 0x%lx\n",
		__pa(rtas_args));
	enter_rtas(__pa(rtas_args));
	PPCDBG(PPCDBG_RTAS, "\treturned from rtas ...\n");

	/* A -1 return code indicates that the last command couldn't
	   be completed due to a hardware error. */
	if (rtas_args->rets[0] == -1)
		logit = (__fetch_rtas_last_error() == 0);

	ifppcdebug(PPCDBG_RTAS) {
		for(i=0; i < nret ;i++)
			udbg_printf("\tnret[%d] = 0x%lx\n", i, (ulong)rtas_args->rets[i]);
	}

	if (nret > 1 && outputs != NULL)
		for (i = 0; i < nret-1; ++i)
			outputs[i] = rtas_args->rets[i+1];
	ret = (nret > 0)? rtas_args->rets[0]: 0;

	/* Log the error in the unlikely case that there was one. */
	if (unlikely(logit)) {
		buff_copy = kmalloc(RTAS_ERROR_LOG_MAX, GFP_ATOMIC);
		if (buff_copy) {
			memcpy(buff_copy, rtas_err_buf, RTAS_ERROR_LOG_MAX);
		}
	}

	/* Gotta do something different here, use global lock for now... */
	spin_unlock_irqrestore(&rtas.lock, s);

	if (buff_copy) {
		log_error(buff_copy, ERR_TYPE_RTAS_LOG, 0);
		kfree(buff_copy);
	}
	return ret;
}

/* Given an RTAS status code of 990n compute the hinted delay of 10^n
 * (last digit) milliseconds.  For now we bound at n=5 (100 sec).
 */
unsigned int
rtas_extended_busy_delay_time(int status)
{
	int order = status - 9900;
	unsigned long ms;

	if (order < 0)
		order = 0;	/* RTC depends on this for -2 clock busy */
	else if (order > 5)
		order = 5;	/* bound */

	/* Use microseconds for reasonable accuracy */
	for (ms=1; order > 0; order--)
		ms *= 10;

	return ms; 
}

int
rtas_get_power_level(int powerdomain, int *level)
{
	int token = rtas_token("get-power-level");
	int rc;

	if (token == RTAS_UNKNOWN_SERVICE)
		return RTAS_UNKNOWN_OP;

	while ((rc = rtas_call(token, 1, 2, level, powerdomain)) == RTAS_BUSY)
		udelay(1);
	return rc;
}

int
rtas_set_power_level(int powerdomain, int level, int *setlevel)
{
	int token = rtas_token("set-power-level");
	unsigned int wait_time;
	int rc;

	if (token == RTAS_UNKNOWN_SERVICE)
		return RTAS_UNKNOWN_OP;

	while (1) {
		rc = rtas_call(token, 2, 2, setlevel, powerdomain, level);
		if (rc == RTAS_BUSY)
			udelay(1);
		else if (rtas_is_extended_busy(rc)) {
			wait_time = rtas_extended_busy_delay_time(rc);
			udelay(wait_time * 1000);
		} else
			break;
	}
	return rc;
}

int
rtas_get_sensor(int sensor, int index, int *state)
{
	int token = rtas_token("get-sensor-state");
	unsigned int wait_time;
	int rc;

	if (token == RTAS_UNKNOWN_SERVICE)
		return RTAS_UNKNOWN_OP;

	while (1) {
		rc = rtas_call(token, 2, 2, state, sensor, index);
		if (rc == RTAS_BUSY)
			udelay(1);
		else if (rtas_is_extended_busy(rc)) {
			wait_time = rtas_extended_busy_delay_time(rc);
			udelay(wait_time * 1000);
		} else
			break;
	}
	return rc;
}

int
rtas_set_indicator(int indicator, int index, int new_value)
{
	int token = rtas_token("set-indicator");
	unsigned int wait_time;
	int rc;

	if (token == RTAS_UNKNOWN_SERVICE)
		return RTAS_UNKNOWN_OP;

	while (1) {
		rc = rtas_call(token, 3, 1, NULL, indicator, index, new_value);
		if (rc == RTAS_BUSY)
			udelay(1);
		else if (rtas_is_extended_busy(rc)) {
			wait_time = rtas_extended_busy_delay_time(rc);
			udelay(wait_time * 1000);
		}
		else
			break;
	}

	return rc;
}

#define FLASH_BLOCK_LIST_VERSION (1UL)
static void
rtas_flash_firmware(void)
{
	unsigned long image_size;
	struct flash_block_list *f, *next, *flist;
	unsigned long rtas_block_list;
	int i, status, update_token;

	update_token = rtas_token("ibm,update-flash-64-and-reboot");
	if (update_token == RTAS_UNKNOWN_SERVICE) {
		printk(KERN_ALERT "FLASH: ibm,update-flash-64-and-reboot is not available -- not a service partition?\n");
		printk(KERN_ALERT "FLASH: firmware will not be flashed\n");
		return;
	}

	/* NOTE: the "first" block list is a global var with no data
	 * blocks in the kernel data segment.  We do this because
	 * we want to ensure this block_list addr is under 4GB.
	 */
	rtas_firmware_flash_list.num_blocks = 0;
	flist = (struct flash_block_list *)&rtas_firmware_flash_list;
	rtas_block_list = virt_to_abs(flist);
	if (rtas_block_list >= 4UL*1024*1024*1024) {
		printk(KERN_ALERT "FLASH: kernel bug...flash list header addr above 4GB\n");
		return;
	}

	printk(KERN_ALERT "FLASH: preparing saved firmware image for flash\n");
	/* Update the block_list in place. */
	image_size = 0;
	for (f = flist; f; f = next) {
		/* Translate data addrs to absolute */
		for (i = 0; i < f->num_blocks; i++) {
			f->blocks[i].data = (char *)virt_to_abs(f->blocks[i].data);
			image_size += f->blocks[i].length;
		}
		next = f->next;
		/* Don't translate NULL pointer for last entry */
		if (f->next)
			f->next = (struct flash_block_list *)virt_to_abs(f->next);
		else
			f->next = NULL;
		/* make num_blocks into the version/length field */
		f->num_blocks = (FLASH_BLOCK_LIST_VERSION << 56) | ((f->num_blocks+1)*16);
	}

	printk(KERN_ALERT "FLASH: flash image is %ld bytes\n", image_size);
	printk(KERN_ALERT "FLASH: performing flash and reboot\n");
	ppc_md.progress("Flashing        \n", 0x0);
	ppc_md.progress("Please Wait...  ", 0x0);
	printk(KERN_ALERT "FLASH: this will take several minutes.  Do not power off!\n");
	status = rtas_call(update_token, 1, 1, NULL, rtas_block_list);
	switch (status) {	/* should only get "bad" status */
	    case 0:
		printk(KERN_ALERT "FLASH: success\n");
		break;
	    case -1:
		printk(KERN_ALERT "FLASH: hardware error.  Firmware may not be not flashed\n");
		break;
	    case -3:
		printk(KERN_ALERT "FLASH: image is corrupt or not correct for this platform.  Firmware not flashed\n");
		break;
	    case -4:
		printk(KERN_ALERT "FLASH: flash failed when partially complete.  System may not reboot\n");
		break;
	    default:
		printk(KERN_ALERT "FLASH: unknown flash return code %d\n", status);
		break;
	}
}

void rtas_flash_bypass_warning(void)
{
	printk(KERN_ALERT "FLASH: firmware flash requires a reboot\n");
	printk(KERN_ALERT "FLASH: the firmware image will NOT be flashed\n");
}


void
rtas_restart(char *cmd)
{
	if (rtas_firmware_flash_list.next)
		rtas_flash_firmware();

	printk("RTAS system-reboot returned %d\n",
	       rtas_call(rtas_token("system-reboot"), 0, 1, NULL));
	for (;;);
}

void
rtas_power_off(void)
{
	if (rtas_firmware_flash_list.next)
		rtas_flash_bypass_warning();
	/* allow power on only with power button press */
	printk("RTAS power-off returned %d\n",
	       rtas_call(rtas_token("power-off"), 2, 1, NULL, -1, -1));
	for (;;);
}

void
rtas_halt(void)
{
	if (rtas_firmware_flash_list.next)
		rtas_flash_bypass_warning();
	rtas_power_off();
}

/* Must be in the RMO region, so we place it here */
static char rtas_os_term_buf[2048];

void rtas_os_term(char *str)
{
	int status;

	snprintf(rtas_os_term_buf, 2048, "OS panic: %s", str);

	do {
		status = rtas_call(rtas_token("ibm,os-term"), 1, 1, NULL,
				   __pa(rtas_os_term_buf));

		if (status == RTAS_BUSY)
			udelay(1);
		else if (status != 0)
			printk(KERN_EMERG "ibm,os-term call failed %d\n",
			       status);
	} while (status == RTAS_BUSY);
}

unsigned long rtas_rmo_buf = 0;

asmlinkage int ppc_rtas(struct rtas_args __user *uargs)
{
	struct rtas_args args;
	unsigned long flags;
	char * buff_copy;
	int nargs;
	int err_rc = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (copy_from_user(&args, uargs, 3 * sizeof(u32)) != 0)
		return -EFAULT;

	nargs = args.nargs;
	if (nargs > ARRAY_SIZE(args.args)
	    || args.nret > ARRAY_SIZE(args.args)
	    || nargs + args.nret > ARRAY_SIZE(args.args))
		return -EINVAL;

	/* Copy in args. */
	if (copy_from_user(args.args, uargs->args,
			   nargs * sizeof(rtas_arg_t)) != 0)
		return -EFAULT;

	buff_copy = kmalloc(RTAS_ERROR_LOG_MAX, GFP_KERNEL);

	spin_lock_irqsave(&rtas.lock, flags);

	rtas.args = args;
	enter_rtas(__pa(&rtas.args));
	args = rtas.args;

	args.rets = &args.args[nargs];

	/* A -1 return code indicates that the last command couldn't
	   be completed due to a hardware error. */
	if (args.rets[0] == -1) {
		err_rc = __fetch_rtas_last_error();
		if ((err_rc == 0) && buff_copy) {
			memcpy(buff_copy, rtas_err_buf, RTAS_ERROR_LOG_MAX);
		}
	}

	spin_unlock_irqrestore(&rtas.lock, flags);

	if (buff_copy) {
		if ((args.rets[0] == -1) && (err_rc == 0)) {
			log_error(buff_copy, ERR_TYPE_RTAS_LOG, 0);
		}
		kfree(buff_copy);
	}

	/* Copy out args. */
	if (copy_to_user(uargs->args + nargs,
			 args.args + nargs,
			 args.nret * sizeof(rtas_arg_t)) != 0)
		return -EFAULT;

	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU
/* This version can't take the spinlock, because it never returns */

struct rtas_args rtas_stop_self_args = {
	/* The token is initialized for real in setup_system() */
	.token = RTAS_UNKNOWN_SERVICE,
	.nargs = 0,
	.nret = 1,
	.rets = &rtas_stop_self_args.args[0],
};

void rtas_stop_self(void)
{
	struct rtas_args *rtas_args = &rtas_stop_self_args;

	local_irq_disable();

	BUG_ON(rtas_args->token == RTAS_UNKNOWN_SERVICE);

	printk("cpu %u (hwid %u) Ready to die...\n",
	       smp_processor_id(), hard_smp_processor_id());
	enter_rtas(__pa(rtas_args));

	panic("Alas, I survived.\n");
}
#endif /* CONFIG_HOTPLUG_CPU */

EXPORT_SYMBOL(rtas_firmware_flash_list);
EXPORT_SYMBOL(rtas_token);
EXPORT_SYMBOL(rtas_call);
EXPORT_SYMBOL(rtas_data_buf);
EXPORT_SYMBOL(rtas_data_buf_lock);
EXPORT_SYMBOL(rtas_extended_busy_delay_time);
EXPORT_SYMBOL(rtas_get_sensor);
EXPORT_SYMBOL(rtas_get_power_level);
EXPORT_SYMBOL(rtas_set_power_level);
EXPORT_SYMBOL(rtas_set_indicator);
