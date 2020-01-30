/* $Id: misc.c,v 1.18 2000/08/26 02:38:03 anton Exp $
 * misc.c:  Miscellaneous prom functions that don't belong
 *          anywhere else.
 *
 * Copyright (C) 1995 David S. Miller (davem@caip.rutgers.edu)
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <asm/openprom.h>
#include <asm/oplib.h>
#include <asm/auxio.h>

extern void restore_current(void);

spinlock_t prom_lock = SPIN_LOCK_UNLOCKED;

/* Reset and reboot the machine with the command 'bcommand'. */
void
prom_reboot(char *bcommand)
{
	unsigned long flags;
	spin_lock_irqsave(&prom_lock, flags);
	(*(romvec->pv_reboot))(bcommand);
	/* Never get here. */
	restore_current();
	spin_unlock_irqrestore(&prom_lock, flags);
}

/* Forth evaluate the expression contained in 'fstring'. */
void
prom_feval(char *fstring)
{
	unsigned long flags;
	if(!fstring || fstring[0] == 0)
		return;
	spin_lock_irqsave(&prom_lock, flags);
	if(prom_vers == PROM_V0)
		(*(romvec->pv_fortheval.v0_eval))(strlen(fstring), fstring);
	else
		(*(romvec->pv_fortheval.v2_eval))(fstring);
	restore_current();
	spin_unlock_irqrestore(&prom_lock, flags);
}

/* We want to do this more nicely some day. */
#ifdef CONFIG_SUN_CONSOLE
extern void (*prom_palette)(int);
extern int serial_console;
#endif

/* Drop into the prom, with the chance to continue with the 'go'
 * prom command.
 */
void
prom_cmdline(void)
{
	extern void kernel_enter_debugger(void);
	extern void install_obp_ticker(void);
	extern void install_linux_ticker(void);
	unsigned long flags;
    
	kernel_enter_debugger();
#ifdef CONFIG_SUN_CONSOLE
	if(!serial_console && prom_palette)
		prom_palette (1);
#endif
	install_obp_ticker();
	spin_lock_irqsave(&prom_lock, flags);
	(*(romvec->pv_abort))();
	restore_current();
	spin_unlock_irqrestore(&prom_lock, flags);
	install_linux_ticker();
#ifdef CONFIG_SUN_AUXIO
	TURN_ON_LED;
#endif
#ifdef CONFIG_SUN_CONSOLE
	if(!serial_console && prom_palette)
		prom_palette (0);
#endif
}

/* Drop into the prom, but completely terminate the program.
 * No chance of continuing.
 */
void
prom_halt(void)
{
	unsigned long flags;
again:
	spin_lock_irqsave(&prom_lock, flags);
	(*(romvec->pv_halt))();
	/* Never get here. */
	restore_current();
	spin_unlock_irqrestore(&prom_lock, flags);
	goto again; /* PROM is out to get me -DaveM */
}

typedef void (*sfunc_t)(void);

/* Set prom sync handler to call function 'funcp'. */
void
prom_setsync(sfunc_t funcp)
{
	if(!funcp) return;
	*romvec->pv_synchook = funcp;
}

/* Get the idprom and stuff it into buffer 'idbuf'.  Returns the
 * format type.  'num_bytes' is the number of bytes that your idbuf
 * has space for.  Returns 0xff on error.
 */
unsigned char
prom_get_idprom(char *idbuf, int num_bytes)
{
	int len;

	len = prom_getproplen(prom_root_node, "idprom");
	if((len>num_bytes) || (len==-1)) return 0xff;
	if(!prom_getproperty(prom_root_node, "idprom", idbuf, num_bytes))
		return idbuf[0];

	return 0xff;
}

/* Get the major prom version number. */
int
prom_version(void)
{
	return romvec->pv_romvers;
}

/* Get the prom plugin-revision. */
int
prom_getrev(void)
{
	return prom_rev;
}

/* Get the prom firmware print revision. */
int
prom_getprev(void)
{
	return prom_prev;
}
