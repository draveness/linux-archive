/* -*- linux-c -*-
 * APM BIOS driver for Linux
 * Copyright 1994-2000 Stephen Rothwell (sfr@linuxcare.com)
 *
 * Initial development of this driver was funded by NEC Australia P/L
 *	and NEC Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * October 1995, Rik Faith (faith@cs.unc.edu):
 *    Minor enhancements and updates (to the patch set) for 1.3.x
 *    Documentation
 * January 1996, Rik Faith (faith@cs.unc.edu):
 *    Make /proc/apm easy to format (bump driver version)
 * March 1996, Rik Faith (faith@cs.unc.edu):
 *    Prohibit APM BIOS calls unless apm_enabled.
 *    (Thanks to Ulrich Windl <Ulrich.Windl@rz.uni-regensburg.de>)
 * April 1996, Stephen Rothwell (Stephen.Rothwell@canb.auug.org.au)
 *    Version 1.0 and 1.1
 * May 1996, Version 1.2
 * Feb 1998, Version 1.3
 * Feb 1998, Version 1.4
 * Aug 1998, Version 1.5
 * Sep 1998, Version 1.6
 * Nov 1998, Version 1.7
 * Jan 1999, Version 1.8
 * Jan 1999, Version 1.9
 * Oct 1999, Version 1.10
 * Nov 1999, Version 1.11
 * Jan 2000, Version 1.12
 * Feb 2000, Version 1.13
 * Nov 2000, Version 1.14
 *
 * History:
 *    0.6b: first version in official kernel, Linux 1.3.46
 *    0.7: changed /proc/apm format, Linux 1.3.58
 *    0.8: fixed gcc 2.7.[12] compilation problems, Linux 1.3.59
 *    0.9: only call bios if bios is present, Linux 1.3.72
 *    1.0: use fixed device number, consolidate /proc/apm into this file,
 *         Linux 1.3.85
 *    1.1: support user-space standby and suspend, power off after system
 *         halted, Linux 1.3.98
 *    1.2: When resetting RTC after resume, take care so that the time
 *         is only incorrect by 30-60mS (vs. 1S previously) (Gabor J. Toth
 *         <jtoth@princeton.edu>); improve interaction between
 *         screen-blanking and gpm (Stephen Rothwell); Linux 1.99.4
 *    1.2a:Simple change to stop mysterious bug reports with SMP also added
 *	   levels to the printk calls. APM is not defined for SMP machines.
 *         The new replacment for it is, but Linux doesn't yet support this.
 *         Alan Cox Linux 2.1.55
 *    1.3: Set up a valid data descriptor 0x40 for buggy BIOS's
 *    1.4: Upgraded to support APM 1.2. Integrated ThinkPad suspend patch by
 *         Dean Gaudet <dgaudet@arctic.org>.
 *         C. Scott Ananian <cananian@alumni.princeton.edu> Linux 2.1.87
 *    1.5: Fix segment register reloading (in case of bad segments saved
 *         across BIOS call).
 *         Stephen Rothwell
 *    1.6: Cope with complier/assembler differences.
 *         Only try to turn off the first display device.
 *         Fix OOPS at power off with no APM BIOS by Jan Echternach
 *                   <echter@informatik.uni-rostock.de>
 *         Stephen Rothwell
 *    1.7: Modify driver's cached copy of the disabled/disengaged flags
 *         to reflect current state of APM BIOS.
 *         Chris Rankin <rankinc@bellsouth.net>
 *         Reset interrupt 0 timer to 100Hz after suspend
 *         Chad Miller <cmiller@surfsouth.com>
 *         Add CONFIG_APM_IGNORE_SUSPEND_BOUNCE
 *         Richard Gooch <rgooch@atnf.csiro.au>
 *         Allow boot time disabling of APM
 *         Make boot messages far less verbose by default
 *         Make asm safer
 *         Stephen Rothwell
 *    1.8: Add CONFIG_APM_RTC_IS_GMT
 *         Richard Gooch <rgooch@atnf.csiro.au>
 *         change APM_NOINTS to CONFIG_APM_ALLOW_INTS
 *         remove dependency on CONFIG_PROC_FS
 *         Stephen Rothwell
 *    1.9: Fix small typo.  <laslo@ilo.opole.pl>
 *         Try to cope with BIOS's that need to have all display
 *         devices blanked and not just the first one.
 *         Ross Paterson <ross@soi.city.ac.uk>
 *         Fix segment limit setting it has always been wrong as
 *         the segments needed to have byte granularity.
 *         Mark a few things __init.
 *         Add hack to allow power off of SMP systems by popular request.
 *         Use CONFIG_SMP instead of __SMP__
 *         Ignore BOUNCES for three seconds.
 *         Stephen Rothwell
 *   1.10: Fix for Thinkpad return code.
 *         Merge 2.2 and 2.3 drivers.
 *         Remove APM dependencies in arch/i386/kernel/process.c
 *         Remove APM dependencies in drivers/char/sysrq.c
 *         Reset time across standby.
 *         Allow more inititialisation on SMP.
 *         Remove CONFIG_APM_POWER_OFF and make it boot time
 *         configurable (default on).
 *         Make debug only a boot time parameter (remove APM_DEBUG).
 *         Try to blank all devices on any error.
 *   1.11: Remove APM dependencies in drivers/char/console.c
 *         Check nr_running to detect if we are idle (from
 *         Borislav Deianov <borislav@lix.polytechnique.fr>)
 *         Fix for bioses that don't zero the top part of the
 *         entrypoint offset (Mario Sitta <sitta@al.unipmn.it>)
 *         (reported by Panos Katsaloulis <teras@writeme.com>).
 *         Real mode power off patch (Walter Hofmann
 *         <Walter.Hofmann@physik.stud.uni-erlangen.de>).
 *   1.12: Remove CONFIG_SMP as the compiler will optimize
 *         the code away anyway (smp_num_cpus == 1 in UP)
 *         noted by Artur Skawina <skawina@geocities.com>.
 *         Make power off under SMP work again.
 *         Fix thinko with initial engaging of BIOS.
 *         Make sure power off only happens on CPU 0
 *         (Paul "Rusty" Russell <rusty@linuxcare.com>).
 *         Do error notification to user mode if BIOS calls fail.
 *         Move entrypoint offset fix to ...boot/setup.S
 *         where it belongs (Cosmos <gis88564@cis.nctu.edu.tw>).
 *         Remove smp-power-off. SMP users must now specify
 *         "apm=power-off" on the kernel command line. Suggested
 *         by Jim Avera <jima@hal.com>, modified by Alan Cox
 *         <alan@lxorguk.ukuu.org.uk>.
 *         Register the /proc/apm entry even on SMP so that
 *         scripts that check for it before doing power off
 *         work (Jim Avera <jima@hal.com>).
 *   1.13: Changes for new pm_ interfaces (Andy Henroid
 *         <andy_henroid@yahoo.com>).
 *         Modularize the code.
 *         Fix the Thinkpad (again) :-( (CONFIG_APM_IGNORE_MULTIPLE_SUSPENDS
 *         is now the way life works).
 *         Fix thinko in suspend() (wrong return).
 *         Notify drivers on critical suspend.
 *         Make kapmd absorb more idle time (Pavel Machek <pavel@suse.cz>
 *         modified by sfr).
 *         Disable interrupts while we are suspended (Andy Henroid
 *         <andy_henroid@yahoo.com> fixed by sfr).
 *         Make power off work on SMP again (Tony Hoyle
 *         <tmh@magenta-logic.com> and <zlatko@iskon.hr>) modified by sfr.
 *         Remove CONFIG_APM_SUSPEND_BOUNCE.  The bounce ignore
 *         interval is now configurable.
 *   1.14: Make connection version persist across module unload/load.
 *         Enable and engage power management earlier.
 *         Disengage power management on module unload.
 *
 * APM 1.1 Reference:
 *
 *   Intel Corporation, Microsoft Corporation. Advanced Power Management
 *   (APM) BIOS Interface Specification, Revision 1.1, September 1993.
 *   Intel Order Number 241704-001.  Microsoft Part Number 781-110-X01.
 *
 * [This document is available free from Intel by calling 800.628.8686 (fax
 * 916.356.6100) or 800.548.4725; or via anonymous ftp from
 * ftp://ftp.intel.com/pub/IAL/software_specs/apmv11.doc.  It is also
 * available from Microsoft by calling 206.882.8080.]
 *
 * APM 1.2 Reference:
 *   Intel Corporation, Microsoft Corporation. Advanced Power Management
 *   (APM) BIOS Interface Specification, Revision 1.2, February 1996.
 *
 * [This document is available from Microsoft at:
 *    http://www.microsoft.com/hwdev/busbios/amp_12.htm]
 */

#include <linux/config.h>
#include <linux/module.h>

#include <linux/poll.h>
#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/timer.h>
#include <linux/fcntl.h>
#include <linux/malloc.h>
#include <linux/stat.h>
#include <linux/proc_fs.h>
#include <linux/miscdevice.h>
#include <linux/apm_bios.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/pm.h>
#include <linux/kernel.h>
#include <linux/smp_lock.h>

#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/desc.h>

extern unsigned long get_cmos_time(void);
extern void machine_real_restart(unsigned char *, int);

#ifdef CONFIG_MAGIC_SYSRQ
extern void (*sysrq_power_off)(void);
#endif
#if defined(CONFIG_APM_DISPLAY_BLANK) && defined(CONFIG_VT)
extern int (*console_blank_hook)(int);
#endif

/*
 * The apm_bios device is one of the misc char devices.
 * This is its minor number.
 */
#define	APM_MINOR_DEV	134

/*
 * See Documentation/Config.help for the configuration options.
 *
 * Various options can be changed at boot time as follows:
 * (We allow underscores for compatibility with the modules code)
 *	apm=on/off			enable/disable APM
 *	    [no-]debug			log some debugging messages
 *	    [no-]power[-_]off		power off on shutdown
 */

/* KNOWN PROBLEM MACHINES:
 *
 * U: TI 4000M TravelMate: BIOS is *NOT* APM compliant
 *                         [Confirmed by TI representative]
 * ?: ACER 486DX4/75: uses dseg 0040, in violation of APM specification
 *                    [Confirmed by BIOS disassembly]
 *                    [This may work now ...]
 * P: Toshiba 1950S: battery life information only gets updated after resume
 * P: Midwest Micro Soundbook Elite DX2/66 monochrome: screen blanking
 * 	broken in BIOS [Reported by Garst R. Reese <reese@isn.net>]
 *
 * Legend: U = unusable with APM patches
 *         P = partially usable with APM patches
 */

/*
 * Define to always call the APM BIOS busy routine even if the clock was
 * not slowed by the idle routine.
 */
#define ALWAYS_CALL_BUSY

/*
 * Define to make the APM BIOS calls zero all data segment registers (so
 * that an incorrect BIOS implementation will cause a kernel panic if it
 * tries to write to arbitrary memory).
 */
#define APM_ZERO_SEGS

/*
 * Define to make all _set_limit calls use 64k limits.  The APM 1.1 BIOS is
 * supposed to provide limit information that it recognizes.  Many machines
 * do this correctly, but many others do not restrict themselves to their
 * claimed limit.  When this happens, they will cause a segmentation
 * violation in the kernel at boot time.  Most BIOS's, however, will
 * respect a 64k limit, so we use that.  If you want to be pedantic and
 * hold your BIOS to its claims, then undefine this.
 */
#define APM_RELAX_SEGMENTS

/*
 * Define to re-initialize the interrupt 0 timer to 100 Hz after a suspend.
 * This patched by Chad Miller <cmiller@surfsouth.com>, original code by
 * David Chen <chen@ctpa04.mit.edu>
 */
#undef INIT_TIMER_AFTER_SUSPEND

#ifdef INIT_TIMER_AFTER_SUSPEND
#include <linux/timex.h>
#include <asm/io.h>
#include <linux/delay.h>
#endif

/*
 * Need to poll the APM BIOS every second
 */
#define APM_CHECK_TIMEOUT	(HZ)

/*
 * Ignore suspend events for this amount of time after a resume
 */
#define DEFAULT_BOUNCE_INTERVAL		(3 * HZ)

/*
 * Save a segment register away
 */
#define savesegment(seg, where) \
		__asm__ __volatile__("movl %%" #seg ",%0" : "=m" (where))

/*
 * Maximum number of events stored
 */
#define APM_MAX_EVENTS		20

/*
 * The per-file APM data
 */
struct apm_user {
	int		magic;
	struct apm_user *	next;
	int		suser: 1;
	int		suspend_wait: 1;
	int		suspend_result;
	int		suspends_pending;
	int		standbys_pending;
	int		suspends_read;
	int		standbys_read;
	int		event_head;
	int		event_tail;
	apm_event_t	events[APM_MAX_EVENTS];
};

/*
 * The magic number in apm_user
 */
#define APM_BIOS_MAGIC		0x4101

/*
 * Local variables
 */
static struct {
	unsigned long	offset;
	unsigned short	segment;
}				apm_bios_entry;
#ifdef CONFIG_APM_CPU_IDLE
static int			clock_slowed;
#endif
static int			suspends_pending;
static int			standbys_pending;
static int			waiting_for_resume;
static int			ignore_normal_resume;
static int			bounce_interval = DEFAULT_BOUNCE_INTERVAL;

#ifdef CONFIG_APM_RTC_IS_GMT
#	define	clock_cmos_diff	0
#	define	got_clock_diff	1
#else
static long			clock_cmos_diff;
static int			got_clock_diff;
#endif
static int			debug;
static int			apm_disabled;
#ifdef CONFIG_SMP
static int			power_off;
#else
static int			power_off = 1;
#endif
static int			exit_kapmd;
static int			kapmd_running;

static DECLARE_WAIT_QUEUE_HEAD(apm_waitqueue);
static DECLARE_WAIT_QUEUE_HEAD(apm_suspend_waitqueue);
static struct apm_user *	user_list;

static char			driver_version[] = "1.14";	/* no spaces */

static char *	apm_event_name[] = {
	"system standby",
	"system suspend",
	"normal resume",
	"critical resume",
	"low battery",
	"power status change",
	"update time",
	"critical suspend",
	"user standby",
	"user suspend",
	"system standby resume",
	"capabilities change"
};
#define NR_APM_EVENT_NAME	\
		(sizeof(apm_event_name) / sizeof(apm_event_name[0]))

typedef struct lookup_t {
	int	key;
	char *	msg;
} lookup_t;

static const lookup_t error_table[] = {
/* N/A	{ APM_SUCCESS,		"Operation succeeded" }, */
	{ APM_DISABLED,		"Power management disabled" },
	{ APM_CONNECTED,	"Real mode interface already connected" },
	{ APM_NOT_CONNECTED,	"Interface not connected" },
	{ APM_16_CONNECTED,	"16 bit interface already connected" },
/* N/A	{ APM_16_UNSUPPORTED,	"16 bit interface not supported" }, */
	{ APM_32_CONNECTED,	"32 bit interface already connected" },
	{ APM_32_UNSUPPORTED,	"32 bit interface not supported" },
	{ APM_BAD_DEVICE,	"Unrecognized device ID" },
	{ APM_BAD_PARAM,	"Parameter out of range" },
	{ APM_NOT_ENGAGED,	"Interface not engaged" },
	{ APM_BAD_FUNCTION,     "Function not supported" },
	{ APM_RESUME_DISABLED,	"Resume timer disabled" },
	{ APM_BAD_STATE,	"Unable to enter requested state" },
/* N/A	{ APM_NO_EVENTS,	"No events pending" }, */
	{ APM_NO_ERROR,		"BIOS did not set a return code" },
	{ APM_NOT_PRESENT,	"No APM present" }
};
#define ERROR_COUNT	(sizeof(error_table)/sizeof(lookup_t))

/*
 * These are the actual BIOS calls.  Depending on APM_ZERO_SEGS and
 * CONFIG_APM_ALLOW_INTS, we are being really paranoid here!  Not only
 * are interrupts disabled, but all the segment registers (except SS)
 * are saved and zeroed this means that if the BIOS tries to reference
 * any data without explicitly loading the segment registers, the kernel
 * will fault immediately rather than have some unforeseen circumstances
 * for the rest of the kernel.  And it will be very obvious!  :-) Doing
 * this depends on CS referring to the same physical memory as DS so that
 * DS can be zeroed before the call. Unfortunately, we can't do anything
 * about the stack segment/pointer.  Also, we tell the compiler that
 * everything could change.
 *
 * Also, we KNOW that for the non error case of apm_bios_call, there
 * is no useful data returned in the low order 8 bits of eax.
 */
#ifndef CONFIG_APM_ALLOW_INTS
#	define APM_DO_CLI	__cli()
#else
#	define APM_DO_CLI	__sti()
#endif
#ifdef APM_ZERO_SEGS
#	define APM_DECL_SEGS \
		unsigned int saved_fs; unsigned int saved_gs;
#	define APM_DO_SAVE_SEGS \
		savesegment(fs, saved_fs); savesegment(gs, saved_gs)
#	define APM_DO_ZERO_SEGS \
		"pushl %%ds\n\t" \
		"pushl %%es\n\t" \
		"xorl %%edx, %%edx\n\t" \
		"mov %%dx, %%ds\n\t" \
		"mov %%dx, %%es\n\t" \
		"mov %%dx, %%fs\n\t" \
		"mov %%dx, %%gs\n\t"
#	define APM_DO_POP_SEGS \
		"popl %%es\n\t" \
		"popl %%ds\n\t"
#	define APM_DO_RESTORE_SEGS \
		loadsegment(fs, saved_fs); loadsegment(gs, saved_gs)
#else
#	define APM_DECL_SEGS
#	define APM_DO_SAVE_SEGS
#	define APM_DO_ZERO_SEGS
#	define APM_DO_POP_SEGS
#	define APM_DO_RESTORE_SEGS
#endif

static u8 apm_bios_call(u32 func, u32 ebx_in, u32 ecx_in,
	u32 *eax, u32 *ebx, u32 *ecx, u32 *edx, u32 *esi)
{
	APM_DECL_SEGS
	unsigned long	flags;

	__save_flags(flags);
	APM_DO_CLI;
	APM_DO_SAVE_SEGS;
	/*
	 * N.B. We do NOT need a cld after the BIOS call
	 * because we always save and restore the flags.
	 */
	__asm__ __volatile__(APM_DO_ZERO_SEGS
		"pushl %%edi\n\t"
		"pushl %%ebp\n\t"
		"lcall %%cs:" SYMBOL_NAME_STR(apm_bios_entry) "\n\t"
		"setc %%al\n\t"
		"popl %%ebp\n\t"
		"popl %%edi\n\t"
		APM_DO_POP_SEGS
		: "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx),
		  "=S" (*esi)
		: "a" (func), "b" (ebx_in), "c" (ecx_in)
		: "memory", "cc");
	APM_DO_RESTORE_SEGS;
	__restore_flags(flags);
	return *eax & 0xff;
}

/*
 * This version only returns one value (usually an error code)
 */

static u8 apm_bios_call_simple(u32 func, u32 ebx_in, u32 ecx_in, u32 *eax)
{
	u8		error;
	APM_DECL_SEGS
	unsigned long	flags;

	__save_flags(flags);
	APM_DO_CLI;
	APM_DO_SAVE_SEGS;
	{
		int	cx, dx, si;

		/*
		 * N.B. We do NOT need a cld after the BIOS call
		 * because we always save and restore the flags.
		 */
		__asm__ __volatile__(APM_DO_ZERO_SEGS
			"pushl %%edi\n\t"
			"pushl %%ebp\n\t"
			"lcall %%cs:" SYMBOL_NAME_STR(apm_bios_entry) "\n\t"
			"setc %%bl\n\t"
			"popl %%ebp\n\t"
			"popl %%edi\n\t"
			APM_DO_POP_SEGS
			: "=a" (*eax), "=b" (error), "=c" (cx), "=d" (dx),
			  "=S" (si)
			: "a" (func), "b" (ebx_in), "c" (ecx_in)
			: "memory", "cc");
	}
	APM_DO_RESTORE_SEGS;
	__restore_flags(flags);
	return error;
}

static int __init apm_driver_version(u_short *val)
{
	u32	eax;

	if (apm_bios_call_simple(APM_FUNC_VERSION, 0, *val, &eax))
		return (eax >> 8) & 0xff;
	*val = eax;
	return APM_SUCCESS;
}

static int apm_get_event(apm_event_t *event, apm_eventinfo_t *info)
{
	u32	eax;
	u32	ebx;
	u32	ecx;
	u32	dummy;

	if (apm_bios_call(APM_FUNC_GET_EVENT, 0, 0, &eax, &ebx, &ecx,
			&dummy, &dummy))
		return (eax >> 8) & 0xff;
	*event = ebx;
	if (apm_info.connection_version < 0x0102)
		*info = ~0; /* indicate info not valid */
	else
		*info = ecx;
	return APM_SUCCESS;
}

static int set_power_state(u_short what, u_short state)
{
	u32	eax;

	if (apm_bios_call_simple(APM_FUNC_SET_STATE, what, state, &eax))
		return (eax >> 8) & 0xff;
	return APM_SUCCESS;
}

static int apm_set_power_state(u_short state)
{
	return set_power_state(APM_DEVICE_ALL, state);
}

#ifdef CONFIG_APM_CPU_IDLE
static int apm_do_idle(void)
{
	u32	dummy;

	if (apm_bios_call_simple(APM_FUNC_IDLE, 0, 0, &dummy))
		return 0;

#ifdef ALWAYS_CALL_BUSY
	clock_slowed = 1;
#else
	clock_slowed = (apm_info.bios.flags & APM_IDLE_SLOWS_CLOCK) != 0;
#endif
	return 1;
}

static void apm_do_busy(void)
{
	u32	dummy;

	if (clock_slowed) {
		(void) apm_bios_call_simple(APM_FUNC_BUSY, 0, 0, &dummy);
		clock_slowed = 0;
	}
}

#if 0
extern int hlt_counter;

/*
 * If no process has been interested in this
 * CPU for some time, we want to wake up the
 * power management thread - we probably want
 * to conserve power.
 */
#define HARD_IDLE_TIMEOUT (HZ/3)

/* This should wake up kapmd and ask it to slow the CPU */
#define powermanagement_idle()  do { } while (0)

/*
 * This is the idle thing.
 */
static void apm_cpu_idle(void)
{
	unsigned int start_idle;

	start_idle = jiffies;
	while (1) {
		if (!current->need_resched) {
			if (jiffies - start_idle < HARD_IDLE_TIMEOUT) {
				if (!current_cpu_data.hlt_works_ok)
					continue;
				if (hlt_counter)
					continue;
				__cli();
				if (!current->need_resched)
					safe_halt();
				else
					__sti();
				continue;
			}

			/*
			 * Ok, do some power management - we've been idle for too long
			 */
			powermanagement_idle();
		}

		schedule();
		check_pgt_cache();
		start_idle = jiffies;
	}
}
#endif
#endif

#ifdef CONFIG_SMP
static int apm_magic(void * unused)
{
	while (1)
		schedule();
}
#endif

static void apm_power_off(void)
{
#ifdef CONFIG_APM_REAL_MODE_POWER_OFF
	unsigned char	po_bios_call[] = {
		0xb8, 0x00, 0x10,	/* movw  $0x1000,ax  */
		0x8e, 0xd0,		/* movw  ax,ss       */
		0xbc, 0x00, 0xf0,	/* movw  $0xf000,sp  */
		0xb8, 0x07, 0x53,	/* movw  $0x5307,ax  */
		0xbb, 0x01, 0x00,	/* movw  $0x0001,bx  */
		0xb9, 0x03, 0x00,	/* movw  $0x0003,cx  */
		0xcd, 0x15		/* int   $0x15       */
	};
#endif

	/*
	 * This may be called on an SMP machine.
	 */
#ifdef CONFIG_SMP
	/* Some bioses don't like being called from CPU != 0 */
	while (cpu_number_map(smp_processor_id()) != 0) {
		kernel_thread(apm_magic, NULL,
			CLONE_FS | CLONE_FILES | CLONE_SIGHAND | SIGCHLD);
		schedule();
	}
#endif
#ifdef CONFIG_APM_REAL_MODE_POWER_OFF
	machine_real_restart(po_bios_call, sizeof(po_bios_call));
#else
	(void) apm_set_power_state(APM_STATE_OFF);
#endif
}

#ifdef CONFIG_APM_DO_ENABLE
static int apm_enable_power_management(int enable)
{
	u32	eax;

	if ((enable == 0) && (apm_info.bios.flags & APM_BIOS_DISENGAGED))
		return APM_NOT_ENGAGED;
	if (apm_bios_call_simple(APM_FUNC_ENABLE_PM, APM_DEVICE_BALL,
			enable, &eax))
		return (eax >> 8) & 0xff;
	if (enable)
		apm_info.bios.flags &= ~APM_BIOS_DISABLED;
	else
		apm_info.bios.flags |= APM_BIOS_DISABLED;
	return APM_SUCCESS;
}
#endif

static int apm_get_power_status(u_short *status, u_short *bat, u_short *life)
{
	u32	eax;
	u32	ebx;
	u32	ecx;
	u32	edx;
	u32	dummy;

	if (apm_info.get_power_status_broken)
		return APM_32_UNSUPPORTED;
	if (apm_bios_call(APM_FUNC_GET_STATUS, APM_DEVICE_ALL, 0,
			&eax, &ebx, &ecx, &edx, &dummy))
		return (eax >> 8) & 0xff;
	*status = ebx;
	*bat = ecx;
	*life = edx;
	return APM_SUCCESS;
}

#if 0
static int apm_get_battery_status(u_short which, u_short *status,
				  u_short *bat, u_short *life, u_short *nbat)
{
	u32	eax;
	u32	ebx;
	u32	ecx;
	u32	edx;
	u32	esi;

	if (apm_info.connection_version < 0x0102) {
		/* pretend we only have one battery. */
		if (which != 1)
			return APM_BAD_DEVICE;
		*nbat = 1;
		return apm_get_power_status(status, bat, life);
	}

	if (apm_bios_call(APM_FUNC_GET_STATUS, (0x8000 | (which)), 0, &eax,
			&ebx, &ecx, &edx, &esi))
		return (eax >> 8) & 0xff;
	*status = ebx;
	*bat = ecx;
	*life = edx;
	*nbat = esi;
	return APM_SUCCESS;
}
#endif

static int apm_engage_power_management(u_short device, int enable)
{
	u32	eax;

	if ((enable == 0) && (device == APM_DEVICE_ALL)
	    && (apm_info.bios.flags & APM_BIOS_DISABLED))
		return APM_DISABLED;
	if (apm_bios_call_simple(APM_FUNC_ENGAGE_PM, device, enable, &eax))
		return (eax >> 8) & 0xff;
	if (device == APM_DEVICE_ALL) {
		if (enable)
			apm_info.bios.flags &= ~APM_BIOS_DISENGAGED;
		else
			apm_info.bios.flags |= APM_BIOS_DISENGAGED;
	}
	return APM_SUCCESS;
}

static void apm_error(char *str, int err)
{
	int	i;

	for (i = 0; i < ERROR_COUNT; i++)
		if (error_table[i].key == err) break;
	if (i < ERROR_COUNT)
		printk(KERN_NOTICE "apm: %s: %s\n", str, error_table[i].msg);
	else
		printk(KERN_NOTICE "apm: %s: unknown error code %#2.2x\n",
			str, err);
}

#if defined(CONFIG_APM_DISPLAY_BLANK) && defined(CONFIG_VT)
static int apm_console_blank(int blank)
{
	int	error;
	u_short	state;

	state = blank ? APM_STATE_STANDBY : APM_STATE_READY;
	/* Blank the first display device */
	error = set_power_state(0x100, state);
	if ((error != APM_SUCCESS) && (error != APM_NO_ERROR)) {
		/* try to blank them all instead */
		error = set_power_state(0x1ff, state);
		if ((error != APM_SUCCESS) && (error != APM_NO_ERROR))
			/* try to blank device one instead */
			error = set_power_state(0x101, state);
	}
	if ((error == APM_SUCCESS) || (error == APM_NO_ERROR))
		return 1;
	apm_error("set display", error);
	return 0;
}
#endif

static int queue_empty(struct apm_user *as)
{
	return as->event_head == as->event_tail;
}

static apm_event_t get_queued_event(struct apm_user *as)
{
	as->event_tail = (as->event_tail + 1) % APM_MAX_EVENTS;
	return as->events[as->event_tail];
}

static void queue_event(apm_event_t event, struct apm_user *sender)
{
	struct apm_user *	as;

	if (user_list == NULL)
		return;
	for (as = user_list; as != NULL; as = as->next) {
		if (as == sender)
			continue;
		as->event_head = (as->event_head + 1) % APM_MAX_EVENTS;
		if (as->event_head == as->event_tail) {
			static int notified;

			if (notified++ == 0)
			    printk(KERN_ERR "apm: an event queue overflowed\n");
			as->event_tail = (as->event_tail + 1) % APM_MAX_EVENTS;
		}
		as->events[as->event_head] = event;
		if (!as->suser)
			continue;
		switch (event) {
		case APM_SYS_SUSPEND:
		case APM_USER_SUSPEND:
			as->suspends_pending++;
			suspends_pending++;
			break;

		case APM_SYS_STANDBY:
		case APM_USER_STANDBY:
			as->standbys_pending++;
			standbys_pending++;
			break;
		}
	}
	wake_up_interruptible(&apm_waitqueue);
}

static void set_time(void)
{
	unsigned long	flags;

	if (got_clock_diff) {	/* Must know time zone in order to set clock */
		save_flags(flags);
		cli();
		CURRENT_TIME = get_cmos_time() + clock_cmos_diff;
		restore_flags(flags);
	}
}

static void get_time_diff(void)
{
#ifndef CONFIG_APM_RTC_IS_GMT
	unsigned long	flags;

	/*
	 * Estimate time zone so that set_time can update the clock
	 */
	save_flags(flags);
	clock_cmos_diff = -get_cmos_time();
	cli();
	clock_cmos_diff += CURRENT_TIME;
	got_clock_diff = 1;
	restore_flags(flags);
#endif
}

static void reinit_timer(void)
{
#ifdef INIT_TIMER_AFTER_SUSPEND
	unsigned long	flags;

	save_flags(flags);
	cli();
	/* set the clock to 100 Hz */
	outb_p(0x34,0x43);		/* binary, mode 2, LSB/MSB, ch 0 */
	udelay(10);
	outb_p(LATCH & 0xff , 0x40);	/* LSB */
	udelay(10);
	outb(LATCH >> 8 , 0x40);	/* MSB */
	udelay(10);
	restore_flags(flags);
#endif
}

static int send_event(apm_event_t event)
{
	switch (event) {
	case APM_SYS_SUSPEND:
	case APM_CRITICAL_SUSPEND:
	case APM_USER_SUSPEND:
		/* map all suspends to ACPI D3 */
		if (pm_send_all(PM_SUSPEND, (void *)3)) {
			if (event == APM_CRITICAL_SUSPEND) {
				printk(KERN_CRIT "apm: Critical suspend was vetoed, expect armageddon\n" );
				return 0;
			}
			if (apm_info.connection_version > 0x100)
				apm_set_power_state(APM_STATE_REJECT);
			return 0;
		}
		break;
	case APM_NORMAL_RESUME:
	case APM_CRITICAL_RESUME:
		/* map all resumes to ACPI D0 */
		(void) pm_send_all(PM_RESUME, (void *)0);
		break;
	}

	return 1;
}

static int suspend(void)
{
	int		err;
	struct apm_user	*as;

	get_time_diff();
	cli();
	err = apm_set_power_state(APM_STATE_SUSPEND);
	reinit_timer();
	set_time();
	if (err == APM_NO_ERROR)
		err = APM_SUCCESS;
	if (err != APM_SUCCESS)
		apm_error("suspend", err);
	send_event(APM_NORMAL_RESUME);
	sti();
	queue_event(APM_NORMAL_RESUME, NULL);
	for (as = user_list; as != NULL; as = as->next) {
		as->suspend_wait = 0;
		as->suspend_result = ((err == APM_SUCCESS) ? 0 : -EIO);
	}
	ignore_normal_resume = 1;
	wake_up_interruptible(&apm_suspend_waitqueue);
	return err;
}

static void standby(void)
{
	int	err;

	get_time_diff();
	err = apm_set_power_state(APM_STATE_STANDBY);
	if ((err != APM_SUCCESS) && (err != APM_NO_ERROR))
		apm_error("standby", err);
}

static apm_event_t get_event(void)
{
	int		error;
	apm_event_t	event;
	apm_eventinfo_t	info;

	static int notified;

	/* we don't use the eventinfo */
	error = apm_get_event(&event, &info);
	if (error == APM_SUCCESS)
		return event;

	if ((error != APM_NO_EVENTS) && (notified++ == 0))
		apm_error("get_event", error);

	return 0;
}

static void check_events(void)
{
	apm_event_t		event;
	static unsigned long	last_resume;
	static int		ignore_bounce;

	while ((event = get_event()) != 0) {
		if (debug) {
			if (event <= NR_APM_EVENT_NAME)
				printk(KERN_DEBUG "apm: received %s notify\n",
				       apm_event_name[event - 1]);
			else
				printk(KERN_DEBUG "apm: received unknown "
				       "event 0x%02x\n", event);
		}
		if (ignore_bounce
		    && ((jiffies - last_resume) > bounce_interval))
			ignore_bounce = 0;
		if (ignore_normal_resume && (event != APM_NORMAL_RESUME))
			ignore_normal_resume = 0;

		switch (event) {
		case APM_SYS_STANDBY:
		case APM_USER_STANDBY:
			if (send_event(event)) {
				queue_event(event, NULL);
				if (standbys_pending <= 0)
					standby();
			}
			break;

		case APM_USER_SUSPEND:
#ifdef CONFIG_APM_IGNORE_USER_SUSPEND
			if (apm_info.connection_version > 0x100)
				apm_set_power_state(APM_STATE_REJECT);
			break;
#endif
		case APM_SYS_SUSPEND:
			if (ignore_bounce) {
				if (apm_info.connection_version > 0x100)
					apm_set_power_state(APM_STATE_REJECT);
				break;
			}
			/*
			 * If we are already processing a SUSPEND,
			 * then further SUSPEND events from the BIOS
			 * will be ignored.  We also return here to
			 * cope with the fact that the Thinkpads keep
			 * sending a SUSPEND event until something else
			 * happens!
			 */
			if (waiting_for_resume)
				return;
			if (send_event(event)) {
				queue_event(event, NULL);
				waiting_for_resume = 1;
				if (suspends_pending <= 0)
					(void) suspend();
			}
			break;

		case APM_NORMAL_RESUME:
		case APM_CRITICAL_RESUME:
		case APM_STANDBY_RESUME:
			waiting_for_resume = 0;
			last_resume = jiffies;
			ignore_bounce = 1;
			if ((event != APM_NORMAL_RESUME)
			    || (ignore_normal_resume == 0)) {
				set_time();
				send_event(event);
				queue_event(event, NULL);
			}
			break;

		case APM_CAPABILITY_CHANGE:
		case APM_LOW_BATTERY:
		case APM_POWER_STATUS_CHANGE:
			send_event(event);
			queue_event(event, NULL);
			break;

		case APM_UPDATE_TIME:
			set_time();
			break;

		case APM_CRITICAL_SUSPEND:
			send_event(event);
			/*
			 * We can only hope it worked - we are not allowed
			 * to reject a critical suspend.
			 */
			(void) suspend();
			break;
		}
	}
}

static void apm_event_handler(void)
{
	static int	pending_count = 4;
	int		err;

	if ((standbys_pending > 0) || (suspends_pending > 0)) {
		if ((apm_info.connection_version > 0x100) && (pending_count-- <= 0)) {
			pending_count = 4;
			if (debug)
				printk(KERN_DEBUG "apm: setting state busy\n");
			err = apm_set_power_state(APM_STATE_BUSY);
			if (err)
				apm_error("busy", err);
		}
	} else
		pending_count = 4;
	check_events();
}

/*
 * This is the APM thread main loop.
 *
 * Check whether we're the only running process to
 * decide if we should just power down.
 *
 */
#define system_idle() (nr_running == 1)

static void apm_mainloop(void)
{
	int timeout = HZ;
	DECLARE_WAITQUEUE(wait, current);

	add_wait_queue(&apm_waitqueue, &wait);
	set_current_state(TASK_INTERRUPTIBLE);
	for (;;) {
		/* Nothing to do, just sleep for the timeout */
		timeout = 2*timeout;
		if (timeout > APM_CHECK_TIMEOUT)
			timeout = APM_CHECK_TIMEOUT;
		schedule_timeout(timeout);
		if (exit_kapmd)
			break;

		/*
		 * Ok, check all events, check for idle (and mark us sleeping
		 * so as not to count towards the load average)..
		 */
		set_current_state(TASK_INTERRUPTIBLE);
		apm_event_handler();
#ifdef CONFIG_APM_CPU_IDLE
		if (!system_idle())
			continue;
		if (apm_do_idle()) {
			unsigned long start = jiffies;
			while ((!exit_kapmd) && system_idle()) {
				apm_do_idle();
				if ((jiffies - start) > APM_CHECK_TIMEOUT) {
					apm_event_handler();
					start = jiffies;
				}
			}
			apm_do_busy();
			apm_event_handler();
			timeout = 1;
		}
#endif
	}
	remove_wait_queue(&apm_waitqueue, &wait);
}

static int check_apm_user(struct apm_user *as, const char *func)
{
	if ((as == NULL) || (as->magic != APM_BIOS_MAGIC)) {
		printk(KERN_ERR "apm: %s passed bad filp\n", func);
		return 1;
	}
	return 0;
}

static ssize_t do_read(struct file *fp, char *buf, size_t count, loff_t *ppos)
{
	struct apm_user *	as;
	int			i;
	apm_event_t		event;
	DECLARE_WAITQUEUE(wait, current);

	as = fp->private_data;
	if (check_apm_user(as, "read"))
		return -EIO;
	if (count < sizeof(apm_event_t))
		return -EINVAL;
	if (queue_empty(as)) {
		if (fp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		add_wait_queue(&apm_waitqueue, &wait);
repeat:
		set_current_state(TASK_INTERRUPTIBLE);
		if (queue_empty(as) && !signal_pending(current)) {
			schedule();
			goto repeat;
		}
		set_current_state(TASK_RUNNING);
		remove_wait_queue(&apm_waitqueue, &wait);
	}
	i = count;
	while ((i >= sizeof(event)) && !queue_empty(as)) {
		event = get_queued_event(as);
		if (copy_to_user(buf, &event, sizeof(event))) {
			if (i < count)
				break;
			return -EFAULT;
		}
		switch (event) {
		case APM_SYS_SUSPEND:
		case APM_USER_SUSPEND:
			as->suspends_read++;
			break;

		case APM_SYS_STANDBY:
		case APM_USER_STANDBY:
			as->standbys_read++;
			break;
		}
		buf += sizeof(event);
		i -= sizeof(event);
	}
	if (i < count)
		return count - i;
	if (signal_pending(current))
		return -ERESTARTSYS;
	return 0;
}

static unsigned int do_poll(struct file *fp, poll_table * wait)
{
	struct apm_user * as;

	as = fp->private_data;
	if (check_apm_user(as, "poll"))
		return 0;
	poll_wait(fp, &apm_waitqueue, wait);
	if (!queue_empty(as))
		return POLLIN | POLLRDNORM;
	return 0;
}

static int do_ioctl(struct inode * inode, struct file *filp,
		    u_int cmd, u_long arg)
{
	struct apm_user *	as;
	DECLARE_WAITQUEUE(wait, current);

	as = filp->private_data;
	if (check_apm_user(as, "ioctl"))
		return -EIO;
	if (!as->suser)
		return -EPERM;
	switch (cmd) {
	case APM_IOC_STANDBY:
		if (as->standbys_read > 0) {
			as->standbys_read--;
			as->standbys_pending--;
			standbys_pending--;
		} else if (!send_event(APM_USER_STANDBY))
			return -EAGAIN;
		else
			queue_event(APM_USER_STANDBY, as);
		if (standbys_pending <= 0)
			standby();
		break;
	case APM_IOC_SUSPEND:
		if (as->suspends_read > 0) {
			as->suspends_read--;
			as->suspends_pending--;
			suspends_pending--;
		} else if (!send_event(APM_USER_SUSPEND))
			return -EAGAIN;
		else
			queue_event(APM_USER_SUSPEND, as);
		if (suspends_pending <= 0) {
			if (suspend() != APM_SUCCESS)
				return -EIO;
		} else {
			as->suspend_wait = 1;
			add_wait_queue(&apm_suspend_waitqueue, &wait);
			while (1) {
				set_current_state(TASK_INTERRUPTIBLE);
				if ((as->suspend_wait == 0)
				    || signal_pending(current))
					break;
				schedule();
			}
			set_current_state(TASK_RUNNING);
			remove_wait_queue(&apm_suspend_waitqueue, &wait);
			return as->suspend_result;
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int do_release(struct inode * inode, struct file * filp)
{
	struct apm_user *	as;

	as = filp->private_data;
	if (check_apm_user(as, "release"))
		return 0;
	filp->private_data = NULL;
	lock_kernel();
	if (as->standbys_pending > 0) {
		standbys_pending -= as->standbys_pending;
		if (standbys_pending <= 0)
			standby();
	}
	if (as->suspends_pending > 0) {
		suspends_pending -= as->suspends_pending;
		if (suspends_pending <= 0)
			(void) suspend();
	}
	if (user_list == as)
		user_list = as->next;
	else {
		struct apm_user *	as1;

		for (as1 = user_list;
		     (as1 != NULL) && (as1->next != as);
		     as1 = as1->next)
			;
		if (as1 == NULL)
			printk(KERN_ERR "apm: filp not in user list\n");
		else
			as1->next = as->next;
	}
	unlock_kernel();
	kfree(as);
	return 0;
}

static int do_open(struct inode * inode, struct file * filp)
{
	struct apm_user *	as;

	as = (struct apm_user *)kmalloc(sizeof(*as), GFP_KERNEL);
	if (as == NULL) {
		printk(KERN_ERR "apm: cannot allocate struct of size %d bytes\n",
		       sizeof(*as));
		return -ENOMEM;
	}
	as->magic = APM_BIOS_MAGIC;
	as->event_tail = as->event_head = 0;
	as->suspends_pending = as->standbys_pending = 0;
	as->suspends_read = as->standbys_read = 0;
	/*
	 * XXX - this is a tiny bit broken, when we consider BSD
         * process accounting. If the device is opened by root, we
	 * instantly flag that we used superuser privs. Who knows,
	 * we might close the device immediately without doing a
	 * privileged operation -- cevans
	 */
	as->suser = capable(CAP_SYS_ADMIN);
	as->next = user_list;
	user_list = as;
	filp->private_data = as;
	return 0;
}

static int apm_get_info(char *buf, char **start, off_t fpos, int length)
{
	char *		p;
	unsigned short	bx;
	unsigned short	cx;
	unsigned short	dx;
	int		error;
	unsigned short  ac_line_status = 0xff;
	unsigned short  battery_status = 0xff;
	unsigned short  battery_flag   = 0xff;
	int		percentage     = -1;
	int             time_units     = -1;
	char            *units         = "?";

	p = buf;

	if ((smp_num_cpus == 1) &&
	    !(error = apm_get_power_status(&bx, &cx, &dx))) {
		ac_line_status = (bx >> 8) & 0xff;
		battery_status = bx & 0xff;
		if ((cx & 0xff) != 0xff)
			percentage = cx & 0xff;

		if (apm_info.connection_version > 0x100) {
			battery_flag = (cx >> 8) & 0xff;
			if (dx != 0xffff) {
				units = (dx & 0x8000) ? "min" : "sec";
				time_units = dx & 0x7fff;
			}
		}
	}
	/* Arguments, with symbols from linux/apm_bios.h.  Information is
	   from the Get Power Status (0x0a) call unless otherwise noted.

	   0) Linux driver version (this will change if format changes)
	   1) APM BIOS Version.  Usually 1.0, 1.1 or 1.2.
	   2) APM flags from APM Installation Check (0x00):
	      bit 0: APM_16_BIT_SUPPORT
	      bit 1: APM_32_BIT_SUPPORT
	      bit 2: APM_IDLE_SLOWS_CLOCK
	      bit 3: APM_BIOS_DISABLED
	      bit 4: APM_BIOS_DISENGAGED
	   3) AC line status
	      0x00: Off-line
	      0x01: On-line
	      0x02: On backup power (BIOS >= 1.1 only)
	      0xff: Unknown
	   4) Battery status
	      0x00: High
	      0x01: Low
	      0x02: Critical
	      0x03: Charging
	      0x04: Selected battery not present (BIOS >= 1.2 only)
	      0xff: Unknown
	   5) Battery flag
	      bit 0: High
	      bit 1: Low
	      bit 2: Critical
	      bit 3: Charging
	      bit 7: No system battery
	      0xff: Unknown
	   6) Remaining battery life (percentage of charge):
	      0-100: valid
	      -1: Unknown
	   7) Remaining battery life (time units):
	      Number of remaining minutes or seconds
	      -1: Unknown
	   8) min = minutes; sec = seconds */

	p += sprintf(p, "%s %d.%d 0x%02x 0x%02x 0x%02x 0x%02x %d%% %d %s\n",
		     driver_version,
		     (apm_info.bios.version >> 8) & 0xff,
		     apm_info.bios.version & 0xff,
		     apm_info.bios.flags,
		     ac_line_status,
		     battery_status,
		     battery_flag,
		     percentage,
		     time_units,
		     units);

	return p - buf;
}

static int apm(void *unused)
{
	unsigned short	bx;
	unsigned short	cx;
	unsigned short	dx;
	int		error;
	char *		power_stat;
	char *		bat_stat;

	kapmd_running = 1;

	daemonize();

	strcpy(current->comm, "kapm-idled");
	sigfillset(&current->blocked);
	current->tty = NULL;	/* get rid of controlling tty */

	if (apm_info.connection_version == 0) {
		apm_info.connection_version = apm_info.bios.version;
		if (apm_info.connection_version > 0x100) {
			/*
			 * We only support BIOSs up to version 1.2
			 */
			if (apm_info.connection_version > 0x0102)
				apm_info.connection_version = 0x0102;
			error = apm_driver_version(&apm_info.connection_version);
			if (error != APM_SUCCESS) {
				apm_error("driver version", error);
				/* Fall back to an APM 1.0 connection. */
				apm_info.connection_version = 0x100;
			}
		}
	}

	if (debug)
		printk(KERN_INFO "apm: Connection version %d.%d\n",
			(apm_info.connection_version >> 8) & 0xff,
			apm_info.connection_version & 0xff);

#ifdef CONFIG_APM_DO_ENABLE
	if (apm_info.bios.flags & APM_BIOS_DISABLED) {
		/*
		 * This call causes my NEC UltraLite Versa 33/C to hang if it
		 * is booted with PM disabled but not in the docking station.
		 * Unfortunate ...
		 */
		error = apm_enable_power_management(1);
		if (error) {
			apm_error("enable power management", error);
			return -1;
		}
	}
#endif

	if ((apm_info.bios.flags & APM_BIOS_DISENGAGED)
	    && (apm_info.connection_version > 0x0100)) {
		error = apm_engage_power_management(APM_DEVICE_ALL, 1);
		if (error) {
			apm_error("engage power management", error);
			return -1;
		}
	}

	if (debug && (smp_num_cpus == 1)) {
		error = apm_get_power_status(&bx, &cx, &dx);
		if (error)
			printk(KERN_INFO "apm: power status not available\n");
		else {
			switch ((bx >> 8) & 0xff) {
			case 0: power_stat = "off line"; break;
			case 1: power_stat = "on line"; break;
			case 2: power_stat = "on backup power"; break;
			default: power_stat = "unknown"; break;
			}
			switch (bx & 0xff) {
			case 0: bat_stat = "high"; break;
			case 1: bat_stat = "low"; break;
			case 2: bat_stat = "critical"; break;
			case 3: bat_stat = "charging"; break;
			default: bat_stat = "unknown"; break;
			}
			printk(KERN_INFO
			       "apm: AC %s, battery status %s, battery life ",
			       power_stat, bat_stat);
			if ((cx & 0xff) == 0xff)
				printk("unknown\n");
			else
				printk("%d%%\n", cx & 0xff);
			if (apm_info.connection_version > 0x100) {
				printk(KERN_INFO
				       "apm: battery flag 0x%02x, battery life ",
				       (cx >> 8) & 0xff);
				if (dx == 0xffff)
					printk("unknown\n");
				else
					printk("%d %s\n", dx & 0x7fff,
						(dx & 0x8000) ?
						"minutes" : "seconds");
			}
		}
	}

	/* Install our power off handler.. */
	if (power_off)
		pm_power_off = apm_power_off;
#ifdef CONFIG_MAGIC_SYSRQ
	sysrq_power_off = apm_power_off;
#endif
	if (smp_num_cpus == 1) {
#if defined(CONFIG_APM_DISPLAY_BLANK) && defined(CONFIG_VT)
		console_blank_hook = apm_console_blank;
#endif
		apm_mainloop();
#if defined(CONFIG_APM_DISPLAY_BLANK) && defined(CONFIG_VT)
		console_blank_hook = NULL;
#endif
	}
	kapmd_running = 0;

	return 0;
}

#ifndef MODULE
static int __init apm_setup(char *str)
{
	int	invert;

	while ((str != NULL) && (*str != '\0')) {
		if (strncmp(str, "off", 3) == 0)
			apm_disabled = 1;
		if (strncmp(str, "on", 2) == 0)
			apm_disabled = 0;
		invert = (strncmp(str, "no-", 3) == 0);
		if (invert)
			str += 3;
		if (strncmp(str, "debug", 5) == 0)
			debug = !invert;
		if ((strncmp(str, "power-off", 9) == 0) ||
		    (strncmp(str, "power_off", 9) == 0))
			power_off = !invert;
		if ((strncmp(str, "bounce-interval=", 16) == 0) ||
		    (strncmp(str, "bounce_interval=", 16) == 0))
			bounce_interval = simple_strtol(str + 16, NULL, 0);
		str = strchr(str, ',');
		if (str != NULL)
			str += strspn(str, ", \t");
	}
	return 1;
}

__setup("apm=", apm_setup);
#endif

static struct file_operations apm_bios_fops = {
	owner:		THIS_MODULE,
	read:		do_read,
	poll:		do_poll,
	ioctl:		do_ioctl,
	open:		do_open,
	release:	do_release,
};

static struct miscdevice apm_device = {
	APM_MINOR_DEV,
	"apm_bios",
	&apm_bios_fops
};

/*
 * Just start the APM thread. We do NOT want to do APM BIOS
 * calls from anything but the APM thread, if for no other reason
 * than the fact that we don't trust the APM BIOS. This way,
 * most common APM BIOS problems that lead to protection errors
 * etc will have at least some level of being contained...
 *
 * In short, if something bad happens, at least we have a choice
 * of just killing the apm thread..
 */
static int __init apm_init(void)
{
	struct proc_dir_entry *apm_proc;

	if (apm_info.bios.version == 0) {
		printk(KERN_INFO "apm: BIOS not found.\n");
		return -ENODEV;
	}
	printk(KERN_INFO
		"apm: BIOS version %d.%d Flags 0x%02x (Driver version %s)\n",
		((apm_info.bios.version >> 8) & 0xff),
		(apm_info.bios.version & 0xff),
		apm_info.bios.flags,
		driver_version);
	if ((apm_info.bios.flags & APM_32_BIT_SUPPORT) == 0) {
		printk(KERN_INFO "apm: no 32 bit BIOS support\n");
		return -ENODEV;
	}

	/*
	 * Fix for the Compaq Contura 3/25c which reports BIOS version 0.1
	 * but is reportedly a 1.0 BIOS.
	 */
	if (apm_info.bios.version == 0x001)
		apm_info.bios.version = 0x100;

	/* BIOS < 1.2 doesn't set cseg_16_len */
	if (apm_info.bios.version < 0x102)
		apm_info.bios.cseg_16_len = 0; /* 64k */

	if (debug) {
		printk(KERN_INFO "apm: entry %x:%lx cseg16 %x dseg %x",
			apm_info.bios.cseg, apm_info.bios.offset,
			apm_info.bios.cseg_16, apm_info.bios.dseg);
		if (apm_info.bios.version > 0x100)
			printk(" cseg len %x, dseg len %x",
				apm_info.bios.cseg_len,
				apm_info.bios.dseg_len);
		if (apm_info.bios.version > 0x101)
			printk(" cseg16 len %x", apm_info.bios.cseg_16_len);
		printk("\n");
	}

	if (apm_disabled) {
		printk(KERN_NOTICE "apm: disabled on user request.\n");
		return -ENODEV;
	}
	if ((smp_num_cpus > 1) && !power_off) {
		printk(KERN_NOTICE "apm: disabled - APM is not SMP safe.\n");
		return -ENODEV;
	}
	if (PM_IS_ACTIVE()) {
		printk(KERN_NOTICE "apm: overridden by ACPI.\n");
		return -ENODEV;
	}
	pm_active = 1;

	/*
	 * Set up a segment that references the real mode segment 0x40
	 * that extends up to the end of page zero (that we have reserved).
	 * This is for buggy BIOS's that refer to (real mode) segment 0x40
	 * even though they are called in protected mode.
	 */
	set_base(gdt[APM_40 >> 3],
		 __va((unsigned long)0x40 << 4));
	_set_limit((char *)&gdt[APM_40 >> 3], 4095 - (0x40 << 4));

	apm_bios_entry.offset = apm_info.bios.offset;
	apm_bios_entry.segment = APM_CS;
	set_base(gdt[APM_CS >> 3],
		 __va((unsigned long)apm_info.bios.cseg << 4));
	set_base(gdt[APM_CS_16 >> 3],
		 __va((unsigned long)apm_info.bios.cseg_16 << 4));
	set_base(gdt[APM_DS >> 3],
		 __va((unsigned long)apm_info.bios.dseg << 4));
#ifndef APM_RELAX_SEGMENTS
	if (apm_info.bios.version == 0x100) {
#endif
		/* For ASUS motherboard, Award BIOS rev 110 (and others?) */
		_set_limit((char *)&gdt[APM_CS >> 3], 64 * 1024 - 1);
		/* For some unknown machine. */
		_set_limit((char *)&gdt[APM_CS_16 >> 3], 64 * 1024 - 1);
		/* For the DEC Hinote Ultra CT475 (and others?) */
		_set_limit((char *)&gdt[APM_DS >> 3], 64 * 1024 - 1);
#ifndef APM_RELAX_SEGMENTS
	} else {
		_set_limit((char *)&gdt[APM_CS >> 3],
			(apm_info.bios.cseg_len - 1) & 0xffff);
		_set_limit((char *)&gdt[APM_CS_16 >> 3],
			(apm_info.bios.cseg_16_len - 1) & 0xffff);
		_set_limit((char *)&gdt[APM_DS >> 3],
			(apm_info.bios.dseg_len - 1) & 0xffff);
	}
#endif

	apm_proc = create_proc_info_entry("apm", 0, NULL, apm_get_info);
	if (apm_proc)
		SET_MODULE_OWNER(apm_proc);

	kernel_thread(apm, NULL, CLONE_FS | CLONE_FILES | CLONE_SIGHAND | SIGCHLD);

	if (smp_num_cpus > 1) {
		printk(KERN_NOTICE
		   "apm: disabled - APM is not SMP safe (power off active).\n");
		return 0;
	}

	misc_register(&apm_device);

	return 0;
}

static void __exit apm_exit(void)
{
	int	error;

	if (((apm_info.bios.flags & APM_BIOS_DISENGAGED) == 0)
	    && (apm_info.connection_version > 0x0100)) {
		error = apm_engage_power_management(APM_DEVICE_ALL, 0);
		if (error)
			apm_error("disengage power management", error);
	}
	misc_deregister(&apm_device);
	remove_proc_entry("apm", NULL);
#ifdef CONFIG_MAGIC_SYSRQ
	sysrq_power_off = NULL;
#endif
	if (power_off)
		pm_power_off = NULL;
	exit_kapmd = 1;
	while (kapmd_running)
		schedule();
	pm_active = 0;
}

module_init(apm_init);
module_exit(apm_exit);

MODULE_AUTHOR("Stephen Rothwell");
MODULE_DESCRIPTION("Advanced Power Management");
MODULE_PARM(debug, "i");
MODULE_PARM_DESC(debug, "Enable debug mode");
MODULE_PARM(power_off, "i");
MODULE_PARM_DESC(power_off, "Enable power off");
MODULE_PARM(bounce_interval, "i");
MODULE_PARM_DESC(bounce_interval, "Set the number of ticks to ignore suspend bounces");

EXPORT_NO_SYMBOLS;
