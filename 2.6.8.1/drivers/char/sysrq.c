/* -*- linux-c -*-
 *
 *	$Id: sysrq.c,v 1.15 1998/08/23 14:56:41 mj Exp $
 *
 *	Linux Magic System Request Key Hacks
 *
 *	(c) 1997 Martin Mares <mj@atrey.karlin.mff.cuni.cz>
 *	based on ideas by Pavel Machek <pavel@atrey.karlin.mff.cuni.cz>
 *
 *	(c) 2000 Crutcher Dunnavant <crutcher+kernel@datastacks.com>
 *	overhauled to use key registration
 *	based upon discusions in irc://irc.openprojects.net/#kernelnewbies
 */

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/tty.h>
#include <linux/mount.h>
#include <linux/kdev_t.h>
#include <linux/major.h>
#include <linux/reboot.h>
#include <linux/sysrq.h>
#include <linux/kbd_kern.h>
#include <linux/quotaops.h>
#include <linux/smp_lock.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/suspend.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>		/* for fsync_bdev() */

#include <linux/spinlock.h>

#include <asm/ptrace.h>

extern void reset_vc(unsigned int);

/* Whether we react on sysrq keys or just ignore them */
int sysrq_enabled = 1;

/* Machine specific power off function */
void (*sysrq_power_off)(void);

/* Loglevel sysrq handler */
static void sysrq_handle_loglevel(int key, struct pt_regs *pt_regs,
				  struct tty_struct *tty) 
{
	int i;
	i = key - '0';
	console_loglevel = 7;
	printk("Loglevel set to %d\n", i);
	console_loglevel = i;
}	
static struct sysrq_key_op sysrq_loglevel_op = {
	.handler	= sysrq_handle_loglevel,
	.help_msg	= "loglevel0-8",
	.action_msg	= "Changing Loglevel",
};


/* SAK sysrq handler */
#ifdef CONFIG_VT
static void sysrq_handle_SAK(int key, struct pt_regs *pt_regs,
			     struct tty_struct *tty) 
{
	if (tty)
		do_SAK(tty);
	reset_vc(fg_console);
}
static struct sysrq_key_op sysrq_SAK_op = {
	.handler	= sysrq_handle_SAK,
	.help_msg	= "saK",
	.action_msg	= "SAK",
};
#endif

#ifdef CONFIG_VT
/* unraw sysrq handler */
static void sysrq_handle_unraw(int key, struct pt_regs *pt_regs,
			       struct tty_struct *tty) 
{
	struct kbd_struct *kbd = &kbd_table[fg_console];

	if (kbd)
		kbd->kbdmode = VC_XLATE;
}
static struct sysrq_key_op sysrq_unraw_op = {
	.handler	= sysrq_handle_unraw,
	.help_msg	= "unRaw",
	.action_msg	= "Keyboard mode set to XLATE",
};
#endif /* CONFIG_VT */

/* reboot sysrq handler */
static void sysrq_handle_reboot(int key, struct pt_regs *pt_regs,
				struct tty_struct *tty) 
{
	machine_restart(NULL);
}

static struct sysrq_key_op sysrq_reboot_op = {
	.handler	= sysrq_handle_reboot,
	.help_msg	= "reBoot",
	.action_msg	= "Resetting",
};

static void sysrq_handle_sync(int key, struct pt_regs *pt_regs,
			      struct tty_struct *tty) 
{
	emergency_sync();
}

static struct sysrq_key_op sysrq_sync_op = {
	.handler	= sysrq_handle_sync,
	.help_msg	= "Sync",
	.action_msg	= "Emergency Sync",
};

static void sysrq_handle_mountro(int key, struct pt_regs *pt_regs,
				 struct tty_struct *tty) 
{
	emergency_remount();
}

static struct sysrq_key_op sysrq_mountro_op = {
	.handler	= sysrq_handle_mountro,
	.help_msg	= "Unmount",
	.action_msg	= "Emergency Remount R/O",
};

/* END SYNC SYSRQ HANDLERS BLOCK */


/* SHOW SYSRQ HANDLERS BLOCK */

static void sysrq_handle_showregs(int key, struct pt_regs *pt_regs,
				  struct tty_struct *tty) 
{
	if (pt_regs)
		show_regs(pt_regs);
}
static struct sysrq_key_op sysrq_showregs_op = {
	.handler	= sysrq_handle_showregs,
	.help_msg	= "showPc",
	.action_msg	= "Show Regs",
};


static void sysrq_handle_showstate(int key, struct pt_regs *pt_regs,
				   struct tty_struct *tty) 
{
	show_state();
}
static struct sysrq_key_op sysrq_showstate_op = {
	.handler	= sysrq_handle_showstate,
	.help_msg	= "showTasks",
	.action_msg	= "Show State",
};


static void sysrq_handle_showmem(int key, struct pt_regs *pt_regs,
				 struct tty_struct *tty) 
{
	show_mem();
}
static struct sysrq_key_op sysrq_showmem_op = {
	.handler	= sysrq_handle_showmem,
	.help_msg	= "showMem",
	.action_msg	= "Show Memory",
};

/* SHOW SYSRQ HANDLERS BLOCK */


/* SIGNAL SYSRQ HANDLERS BLOCK */

/* signal sysrq helper function
 * Sends a signal to all user processes */
static void send_sig_all(int sig)
{
	struct task_struct *p;

	for_each_process(p) {
		if (p->mm && p->pid != 1)
			/* Not swapper, init nor kernel thread */
			force_sig(sig, p);
	}
}

static void sysrq_handle_term(int key, struct pt_regs *pt_regs,
			      struct tty_struct *tty) 
{
	send_sig_all(SIGTERM);
	console_loglevel = 8;
}
static struct sysrq_key_op sysrq_term_op = {
	.handler	= sysrq_handle_term,
	.help_msg	= "tErm",
	.action_msg	= "Terminate All Tasks",
};

static void sysrq_handle_kill(int key, struct pt_regs *pt_regs,
			      struct tty_struct *tty) 
{
	send_sig_all(SIGKILL);
	console_loglevel = 8;
}
static struct sysrq_key_op sysrq_kill_op = {
	.handler	= sysrq_handle_kill,
	.help_msg	= "kIll",
	.action_msg	= "Kill All Tasks",
};

/* END SIGNAL SYSRQ HANDLERS BLOCK */


/* Key Operations table and lock */
static spinlock_t sysrq_key_table_lock = SPIN_LOCK_UNLOCKED;
#define SYSRQ_KEY_TABLE_LENGTH 36
static struct sysrq_key_op *sysrq_key_table[SYSRQ_KEY_TABLE_LENGTH] = {
/* 0 */	&sysrq_loglevel_op,
/* 1 */	&sysrq_loglevel_op,
/* 2 */	&sysrq_loglevel_op,
/* 3 */	&sysrq_loglevel_op,
/* 4 */	&sysrq_loglevel_op,
/* 5 */	&sysrq_loglevel_op,
/* 6 */	&sysrq_loglevel_op,
/* 7 */	&sysrq_loglevel_op,
/* 8 */	&sysrq_loglevel_op,
/* 9 */	&sysrq_loglevel_op,
/* a */	NULL, /* Don't use for system provided sysrqs,
		 it is handled specially on the sparc
		 and will never arrive */
/* b */	&sysrq_reboot_op,
/* c */ NULL,
/* d */	NULL,
/* e */	&sysrq_term_op,
/* f */	NULL,
/* g */	NULL,
/* h */	NULL,
/* i */	&sysrq_kill_op,
/* j */	NULL,
#ifdef CONFIG_VT
/* k */	&sysrq_SAK_op,
#else
/* k */	NULL,
#endif
/* l */	NULL,
/* m */	&sysrq_showmem_op,
/* n */	NULL,
/* o */	NULL, /* This will often be registered
		 as 'Off' at init time */
/* p */	&sysrq_showregs_op,
/* q */	NULL,
#ifdef CONFIG_VT
/* r */	&sysrq_unraw_op,
#else
/* r */ NULL,
#endif
/* s */	&sysrq_sync_op,
/* t */	&sysrq_showstate_op,
/* u */	&sysrq_mountro_op,
/* v */	NULL, /* May be assigned at init time by SMP VOYAGER */
/* w */	NULL,
/* x */	NULL,
/* y */	NULL,
/* z */	NULL
};

/* key2index calculation, -1 on invalid index */
static int sysrq_key_table_key2index(int key) {
	int retval;
	if ((key >= '0') && (key <= '9')) {
		retval = key - '0';
	} else if ((key >= 'a') && (key <= 'z')) {
		retval = key + 10 - 'a';
	} else {
		retval = -1;
	}
	return retval;
}

/*
 * table lock and unlocking functions, exposed to modules
 */

void __sysrq_lock_table (void) { spin_lock(&sysrq_key_table_lock); }

void __sysrq_unlock_table (void) { spin_unlock(&sysrq_key_table_lock); }

/*
 * get and put functions for the table, exposed to modules.
 */

struct sysrq_key_op *__sysrq_get_key_op (int key) {
        struct sysrq_key_op *op_p;
        int i;
	
	i = sysrq_key_table_key2index(key);
        op_p = (i == -1) ? NULL : sysrq_key_table[i];
        return op_p;
}

void __sysrq_put_key_op (int key, struct sysrq_key_op *op_p) {
        int i;

	i = sysrq_key_table_key2index(key);
        if (i != -1)
                sysrq_key_table[i] = op_p;
}

/*
 * This is the non-locking version of handle_sysrq
 * It must/can only be called by sysrq key handlers,
 * as they are inside of the lock
 */

void __handle_sysrq(int key, struct pt_regs *pt_regs, struct tty_struct *tty)
{
	struct sysrq_key_op *op_p;
	int orig_log_level;
	int i, j;

	__sysrq_lock_table();
	orig_log_level = console_loglevel;
	console_loglevel = 7;
	printk(KERN_INFO "SysRq : ");

        op_p = __sysrq_get_key_op(key);
        if (op_p) {
		printk ("%s\n", op_p->action_msg);
		console_loglevel = orig_log_level;
		op_p->handler(key, pt_regs, tty);
	} else {
		printk("HELP : ");
		/* Only print the help msg once per handler */
		for (i=0; i<SYSRQ_KEY_TABLE_LENGTH; i++) 
		if (sysrq_key_table[i]) {
			for (j=0; sysrq_key_table[i] != sysrq_key_table[j]; j++);
			if (j == i)
				printk ("%s ", sysrq_key_table[i]->help_msg);
		}
		printk ("\n");
		console_loglevel = orig_log_level;
	}
	__sysrq_unlock_table();
}

/*
 * This function is called by the keyboard handler when SysRq is pressed
 * and any other keycode arrives.
 */

void handle_sysrq(int key, struct pt_regs *pt_regs, struct tty_struct *tty)
{
	if (!sysrq_enabled)
		return;
	__handle_sysrq(key, pt_regs, tty);
}

EXPORT_SYMBOL(handle_sysrq);
EXPORT_SYMBOL(__sysrq_lock_table);
EXPORT_SYMBOL(__sysrq_unlock_table);
EXPORT_SYMBOL(__sysrq_get_key_op);
EXPORT_SYMBOL(__sysrq_put_key_op);
