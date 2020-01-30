/*
 *  drivers/s390/cio/cio.c
 *   S/390 common I/O routines -- low level i/o calls
 *   $Revision: 1.123 $
 *
 *    Copyright (C) 1999-2002 IBM Deutschland Entwicklung GmbH,
 *			      IBM Corporation
 *    Author(s): Ingo Adlung (adlung@de.ibm.com)
 *		 Cornelia Huck (cohuck@de.ibm.com)
 *		 Arnd Bergmann (arndb@de.ibm.com)
 *		 Martin Schwidefsky (schwidefsky@de.ibm.com)
 */

#include <linux/module.h>
#include <linux/config.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/kernel_stat.h>

#include <asm/hardirq.h>
#include <asm/cio.h>
#include <asm/delay.h>
#include <asm/irq.h>

#include "airq.h"
#include "cio.h"
#include "css.h"
#include "chsc.h"
#include "ioasm.h"
#include "blacklist.h"
#include "cio_debug.h"

debug_info_t *cio_debug_msg_id;
debug_info_t *cio_debug_trace_id;
debug_info_t *cio_debug_crw_id;

int cio_show_msg;

static int __init
cio_setup (char *parm)
{
	if (!strcmp (parm, "yes"))
		cio_show_msg = 1;
	else if (!strcmp (parm, "no"))
		cio_show_msg = 0;
	else
		printk (KERN_ERR "cio_setup : invalid cio_msg parameter '%s'",
			parm);
	return 1;
}

__setup ("cio_msg=", cio_setup);

/*
 * Function: cio_debug_init
 * Initializes three debug logs (under /proc/s390dbf) for common I/O:
 * - cio_msg logs the messages which are printk'ed when CONFIG_DEBUG_IO is on
 * - cio_trace logs the calling of different functions
 * - cio_crw logs the messages which are printk'ed when CONFIG_DEBUG_CRW is on
 * debug levels depend on CONFIG_DEBUG_IO resp. CONFIG_DEBUG_CRW
 */
static int __init
cio_debug_init (void)
{
	cio_debug_msg_id = debug_register ("cio_msg", 4, 4, 16*sizeof (long));
	if (!cio_debug_msg_id)
		goto out_unregister;
	debug_register_view (cio_debug_msg_id, &debug_sprintf_view);
	debug_set_level (cio_debug_msg_id, 2);
	cio_debug_trace_id = debug_register ("cio_trace", 4, 4, 8);
	if (!cio_debug_trace_id)
		goto out_unregister;
	debug_register_view (cio_debug_trace_id, &debug_hex_ascii_view);
	debug_set_level (cio_debug_trace_id, 2);
	cio_debug_crw_id = debug_register ("cio_crw", 2, 4, 16*sizeof (long));
	if (!cio_debug_crw_id)
		goto out_unregister;
	debug_register_view (cio_debug_crw_id, &debug_sprintf_view);
	debug_set_level (cio_debug_crw_id, 2);
	pr_debug("debugging initialized\n");
	return 0;

out_unregister:
	if (cio_debug_msg_id)
		debug_unregister (cio_debug_msg_id);
	if (cio_debug_trace_id)
		debug_unregister (cio_debug_trace_id);
	if (cio_debug_crw_id)
		debug_unregister (cio_debug_crw_id);
	pr_debug("could not initialize debugging\n");
	return -1;
}

arch_initcall (cio_debug_init);

int
cio_set_options (struct subchannel *sch, int flags)
{
       sch->options.suspend = (flags & DOIO_ALLOW_SUSPEND) != 0;
       sch->options.prefetch = (flags & DOIO_DENY_PREFETCH) != 0;
       sch->options.inter = (flags & DOIO_SUPPRESS_INTER) != 0;
       return 0;
}

/* FIXME: who wants to use this? */
int
cio_get_options (struct subchannel *sch)
{
       int flags;

       flags = 0;
       if (sch->options.suspend)
		flags |= DOIO_ALLOW_SUSPEND;
       if (sch->options.prefetch)
		flags |= DOIO_DENY_PREFETCH;
       if (sch->options.inter)
		flags |= DOIO_SUPPRESS_INTER;
       return flags;
}

/*
 * Use tpi to get a pending interrupt, call the interrupt handler and
 * return a pointer to the subchannel structure.
 */
static inline int
cio_tpi(void)
{
	struct tpi_info *tpi_info;
	struct subchannel *sch;
	struct irb *irb;

	tpi_info = (struct tpi_info *) __LC_SUBCHANNEL_ID;
	if (tpi (NULL) != 1)
		return 0;
	irb = (struct irb *) __LC_IRB;
	/* Store interrupt response block to lowcore. */
	if (tsch (tpi_info->irq, irb) != 0)
		/* Not status pending or not operational. */
		return 1;
	sch = (struct subchannel *)(unsigned long)tpi_info->intparm;
	if (!sch)
		return 1;
	irq_enter ();
	spin_lock(&sch->lock);
	memcpy (&sch->schib.scsw, &irb->scsw, sizeof (struct scsw));
	if (sch->driver && sch->driver->irq)
		sch->driver->irq(&sch->dev);
	spin_unlock(&sch->lock);
	irq_exit ();
	return 1;
}

static inline int
cio_start_handle_notoper(struct subchannel *sch, __u8 lpm)
{
	char dbf_text[15];

	if (lpm != 0)
		sch->lpm &= ~lpm;
	else
		sch->lpm = 0;

	stsch (sch->irq, &sch->schib);

	CIO_MSG_EVENT(0, "cio_start: 'not oper' status for "
		      "subchannel %04x!\n", sch->irq);
	sprintf(dbf_text, "no%s", sch->dev.bus_id);
	CIO_TRACE_EVENT(0, dbf_text);
	CIO_HEX_EVENT(0, &sch->schib, sizeof (struct schib));

	return (sch->lpm ? -EACCES : -ENODEV);
}

int
cio_start (struct subchannel *sch,	/* subchannel structure */
	   struct ccw1 * cpa,		/* logical channel prog addr */
	   __u8 lpm)			/* logical path mask */
{
	char dbf_txt[15];
	int ccode;

	CIO_TRACE_EVENT (4, "stIO");
	CIO_TRACE_EVENT (4, sch->dev.bus_id);

	/* sch is always under 2G. */
	sch->orb.intparm = (__u32)(unsigned long)sch;
	sch->orb.fmt = 1;

	sch->orb.pfch = sch->options.prefetch == 0;
	sch->orb.spnd = sch->options.suspend;
	sch->orb.ssic = sch->options.suspend && sch->options.inter;
	sch->orb.lpm = (lpm != 0) ? (lpm & sch->opm) : sch->lpm;
#ifdef CONFIG_ARCH_S390X
	/*
	 * for 64 bit we always support 64 bit IDAWs with 4k page size only
	 */
	sch->orb.c64 = 1;
	sch->orb.i2k = 0;
#endif
	sch->orb.cpa = (__u32) __pa (cpa);

	/*
	 * Issue "Start subchannel" and process condition code
	 */
	ccode = ssch (sch->irq, &sch->orb);
	sprintf (dbf_txt, "ccode:%d", ccode);
	CIO_TRACE_EVENT (4, dbf_txt);

	switch (ccode) {
	case 0:
		/*
		 * initialize device status information
		 */
		sch->schib.scsw.actl |= SCSW_ACTL_START_PEND;
		return 0;
	case 1:		/* status pending */
	case 2:		/* busy */
		return -EBUSY;
	default:		/* device/path not operational */
		return cio_start_handle_notoper(sch, lpm);
	}
}

/*
 * resume suspended I/O operation
 */
int
cio_resume (struct subchannel *sch)
{
	char dbf_txt[15];
	int ccode;

	CIO_TRACE_EVENT (4, "resIO");
	CIO_TRACE_EVENT (4, sch->dev.bus_id);

	ccode = rsch (sch->irq);

	sprintf (dbf_txt, "ccode:%d", ccode);
	CIO_TRACE_EVENT (4, dbf_txt);

	switch (ccode) {
	case 0:
		sch->schib.scsw.actl |= SCSW_ACTL_RESUME_PEND;
		return 0;
	case 1:
		return -EBUSY;
	case 2:
		return -EINVAL;
	default:
		/*
		 * useless to wait for request completion
		 *  as device is no longer operational !
		 */
		return -ENODEV;
	}
}

/*
 * halt I/O operation
 */
int
cio_halt(struct subchannel *sch)
{
	char dbf_txt[15];
	int ccode;

	if (!sch)
		return -ENODEV;

	CIO_TRACE_EVENT (2, "haltIO");
	CIO_TRACE_EVENT (2, sch->dev.bus_id);

	/*
	 * Issue "Halt subchannel" and process condition code
	 */
	ccode = hsch (sch->irq);

	sprintf (dbf_txt, "ccode:%d", ccode);
	CIO_TRACE_EVENT (2, dbf_txt);

	switch (ccode) {
	case 0:
		sch->schib.scsw.actl |= SCSW_ACTL_HALT_PEND;
		return 0;
	case 1:		/* status pending */
	case 2:		/* busy */
		return -EBUSY;
	default:		/* device not operational */
		return -ENODEV;
	}
}

/*
 * Clear I/O operation
 */
int
cio_clear(struct subchannel *sch)
{
	char dbf_txt[15];
	int ccode;

	if (!sch)
		return -ENODEV;

	CIO_TRACE_EVENT (2, "clearIO");
	CIO_TRACE_EVENT (2, sch->dev.bus_id);

	/*
	 * Issue "Clear subchannel" and process condition code
	 */
	ccode = csch (sch->irq);

	sprintf (dbf_txt, "ccode:%d", ccode);
	CIO_TRACE_EVENT (2, dbf_txt);

	switch (ccode) {
	case 0:
		sch->schib.scsw.actl |= SCSW_ACTL_CLEAR_PEND;
		return 0;
	default:		/* device not operational */
		return -ENODEV;
	}
}

/*
 * Function: cio_cancel
 * Issues a "Cancel Subchannel" on the specified subchannel
 * Note: We don't need any fancy intparms and flags here
 *	 since xsch is executed synchronously.
 * Only for common I/O internal use as for now.
 */
int
cio_cancel (struct subchannel *sch)
{
	char dbf_txt[15];
	int ccode;

	if (!sch)
		return -ENODEV;

	CIO_TRACE_EVENT (2, "cancelIO");
	CIO_TRACE_EVENT (2, sch->dev.bus_id);

	ccode = xsch (sch->irq);

	sprintf (dbf_txt, "ccode:%d", ccode);
	CIO_TRACE_EVENT (2, dbf_txt);

	switch (ccode) {
	case 0:		/* success */
		/* Update information in scsw. */
		stsch (sch->irq, &sch->schib);
		return 0;
	case 1:		/* status pending */
		return -EBUSY;
	case 2:		/* not applicable */
		return -EINVAL;
	default:	/* not oper */
		return -ENODEV;
	}
}

/*
 * Function: cio_modify
 * Issues a "Modify Subchannel" on the specified subchannel
 */
int
cio_modify (struct subchannel *sch)
{
	int ccode, retry, ret;

	ret = 0;
	for (retry = 0; retry < 5; retry++) {
		ccode = msch_err (sch->irq, &sch->schib);
		if (ccode < 0)	/* -EIO if msch gets a program check. */
			return ccode;
		switch (ccode) {
		case 0: /* successfull */
			return 0;
		case 1:	/* status pending */
			return -EBUSY;
		case 2:	/* busy */
			udelay (100);	/* allow for recovery */
			ret = -EBUSY;
			break;
		case 3:	/* not operational */
			return -ENODEV;
		}
	}
	return ret;
}

/*
 * Enable subchannel.
 */
int
cio_enable_subchannel (struct subchannel *sch, unsigned int isc)
{
	char dbf_txt[15];
	int ccode;
	int retry;
	int ret;

	CIO_TRACE_EVENT (2, "ensch");
	CIO_TRACE_EVENT (2, sch->dev.bus_id);

	ccode = stsch (sch->irq, &sch->schib);
	if (ccode)
		return -ENODEV;

	sch->schib.pmcw.ena = 1;
	sch->schib.pmcw.isc = isc;
	sch->schib.pmcw.intparm = (__u32)(unsigned long)sch;
	for (retry = 5, ret = 0; retry > 0; retry--) {
		ret = cio_modify(sch);
		if (ret == -ENODEV)
			break;
		if (ret == -EIO)
			/*
			 * Got a program check in cio_modify. Try without
			 * the concurrent sense bit the next time.
			 */
			sch->schib.pmcw.csense = 0;
		if (ret == 0) {
			stsch (sch->irq, &sch->schib);
			if (sch->schib.pmcw.ena)
				break;
		}
		if (ret == -EBUSY) {
			struct irb irb;
			if (tsch(sch->irq, &irb) != 0)
				break;
		}
	}
	sprintf (dbf_txt, "ret:%d", ret);
	CIO_TRACE_EVENT (2, dbf_txt);
	return ret;
}

/*
 * Disable subchannel.
 */
int
cio_disable_subchannel (struct subchannel *sch)
{
	char dbf_txt[15];
	int ccode;
	int retry;
	int ret;

	CIO_TRACE_EVENT (2, "dissch");
	CIO_TRACE_EVENT (2, sch->dev.bus_id);

	ccode = stsch (sch->irq, &sch->schib);
	if (ccode == 3)		/* Not operational. */
		return -ENODEV;

	if (sch->schib.scsw.actl != 0)
		/*
		 * the disable function must not be called while there are
		 *  requests pending for completion !
		 */
		return -EBUSY;


	sch->schib.pmcw.ena = 0;
	for (retry = 5, ret = 0; retry > 0; retry--) {
		ret = cio_modify(sch);
		if (ret == -ENODEV)
			break;
		if (ret == -EBUSY)
			/*
			 * The subchannel is busy or status pending.
			 * We'll disable when the next interrupt was delivered
			 * via the state machine.
			 */
			break;
		if (ret == 0) {
			stsch (sch->irq, &sch->schib);
			if (!sch->schib.pmcw.ena)
				break;
		}
	}
	sprintf (dbf_txt, "ret:%d", ret);
	CIO_TRACE_EVENT (2, dbf_txt);
	return ret;
}

/*
 * cio_validate_subchannel()
 *
 * Find out subchannel type and initialize struct subchannel.
 * Return codes:
 *   SUBCHANNEL_TYPE_IO for a normal io subchannel
 *   SUBCHANNEL_TYPE_CHSC for a chsc subchannel
 *   SUBCHANNEL_TYPE_MESSAGE for a messaging subchannel
 *   SUBCHANNEL_TYPE_ADM for a adm(?) subchannel
 *   -ENXIO for non-defined subchannels
 *   -ENODEV for subchannels with invalid device number or blacklisted devices
 */
int
cio_validate_subchannel (struct subchannel *sch, unsigned int irq)
{
	char dbf_txt[15];
	int ccode;

	sprintf (dbf_txt, "valsch%x", irq);
	CIO_TRACE_EVENT (4, dbf_txt);

	/* Nuke all fields. */
	memset(sch, 0, sizeof(struct subchannel));

	spin_lock_init(&sch->lock);

	/* Set a name for the subchannel */
	snprintf (sch->dev.bus_id, BUS_ID_SIZE, "0.0.%04x", irq);

	/*
	 * The first subchannel that is not-operational (ccode==3)
	 *  indicates that there aren't any more devices available.
	 */
	sch->irq = irq;
	ccode = stsch (irq, &sch->schib);
	if (ccode)
		return -ENXIO;

	/* Copy subchannel type from path management control word. */
	sch->st = sch->schib.pmcw.st;

	/*
	 * ... just being curious we check for non I/O subchannels
	 */
	if (sch->st != 0) {
		CIO_DEBUG(KERN_INFO, 0,
			  "Subchannel %04X reports "
			  "non-I/O subchannel type %04X\n",
			  sch->irq, sch->st);
		/* We stop here for non-io subchannels. */
		return sch->st;
	}

	/* Initialization for io subchannels. */
	if (!sch->schib.pmcw.dnv)
		/* io subchannel but device number is invalid. */
		return -ENODEV;

	/* Devno is valid. */
	if (is_blacklisted (sch->schib.pmcw.dev)) {
		/*
		 * This device must not be known to Linux. So we simply
		 * say that there is no device and return ENODEV.
		 */
		CIO_MSG_EVENT(0, "Blacklisted device detected "
			      "at devno %04X\n", sch->schib.pmcw.dev);
		return -ENODEV;
	}
	sch->opm = 0xff;
	chsc_validate_chpids(sch);
	sch->lpm = sch->schib.pmcw.pim &
		sch->schib.pmcw.pam &
		sch->schib.pmcw.pom &
		sch->opm;

	CIO_DEBUG(KERN_INFO, 0,
		  "Detected device %04X on subchannel %04X"
		  " - PIM = %02X, PAM = %02X, POM = %02X\n",
		  sch->schib.pmcw.dev, sch->irq, sch->schib.pmcw.pim,
		  sch->schib.pmcw.pam, sch->schib.pmcw.pom);

	/*
	 * We now have to initially ...
	 *  ... set "interruption subclass"
	 *  ... enable "concurrent sense"
	 *  ... enable "multipath mode" if more than one
	 *	  CHPID is available. This is done regardless
	 *	  whether multiple paths are available for us.
	 */
	sch->schib.pmcw.isc = 3;	/* could be smth. else */
	sch->schib.pmcw.csense = 1;	/* concurrent sense */
	sch->schib.pmcw.ena = 0;
	if ((sch->lpm & (sch->lpm - 1)) != 0)
		sch->schib.pmcw.mp = 1;	/* multipath mode */
	return 0;
}

/*
 * do_IRQ() handles all normal I/O device IRQ's (the special
 *	    SMP cross-CPU interrupts have their own specific
 *	    handlers).
 *
 */
void
do_IRQ (struct pt_regs *regs)
{
	struct tpi_info *tpi_info;
	struct subchannel *sch;
	struct irb *irb;

	irq_enter ();
	asm volatile ("mc 0,0");
	if (S390_lowcore.int_clock >= S390_lowcore.jiffy_timer)
		account_ticks(regs);
	/*
	 * Get interrupt information from lowcore
	 */
	tpi_info = (struct tpi_info *) __LC_SUBCHANNEL_ID;
	irb = (struct irb *) __LC_IRB;
	do {
		kstat_cpu(smp_processor_id()).irqs[IO_INTERRUPT]++;
		/*
		 * Non I/O-subchannel thin interrupts are processed differently
		 */
		if (tpi_info->adapter_IO == 1 &&
		    tpi_info->int_type == IO_INTERRUPT_TYPE) {
			do_adapter_IO();
			continue;
		}
		sch = (struct subchannel *)(unsigned long)tpi_info->intparm;
		if (sch)
			spin_lock(&sch->lock);
		/* Store interrupt response block to lowcore. */
		if (tsch (tpi_info->irq, irb) == 0 && sch) {
			/* Keep subchannel information word up to date. */
			memcpy (&sch->schib.scsw, &irb->scsw,
				sizeof (irb->scsw));
			/* Call interrupt handler if there is one. */
			if (sch->driver && sch->driver->irq)
				sch->driver->irq(&sch->dev);
		}
		if (sch)
			spin_unlock(&sch->lock);
		/*
		 * Are more interrupts pending?
		 * If so, the tpi instruction will update the lowcore
		 * to hold the info for the next interrupt.
		 * We don't do this for VM because a tpi drops the cpu
		 * out of the sie which costs more cycles than it saves.
		 */
	} while (!MACHINE_IS_VM && tpi (NULL) != 0);
	irq_exit ();
}

#ifdef CONFIG_CCW_CONSOLE
static struct subchannel console_subchannel;
static int console_subchannel_in_use;

/*
 * busy wait for the next interrupt on the console
 */
void
wait_cons_dev (void)
{
	unsigned long cr6      __attribute__ ((aligned (8)));
	unsigned long save_cr6 __attribute__ ((aligned (8)));

	/* 
	 * before entering the spinlock we may already have
	 * processed the interrupt on a different CPU...
	 */
	if (!console_subchannel_in_use)
		return;

	/* disable all but isc 7 (console device) */
	__ctl_store (save_cr6, 6, 6);
	cr6 = 0x01000000;
	__ctl_load (cr6, 6, 6);

	do {
		spin_unlock(&console_subchannel.lock);
		if (!cio_tpi())
			cpu_relax();
		spin_lock(&console_subchannel.lock);
	} while (console_subchannel.schib.scsw.actl != 0);
	/*
	 * restore previous isc value
	 */
	__ctl_load (save_cr6, 6, 6);
}

static int
cio_console_irq(void)
{
	int irq;
	
	if (console_irq != -1) {
		/* VM provided us with the irq number of the console. */
		if (stsch(console_irq, &console_subchannel.schib) != 0 ||
		    !console_subchannel.schib.pmcw.dnv)
			return -1;
		console_devno = console_subchannel.schib.pmcw.dev;
	} else if (console_devno != -1) {
		/* At least the console device number is known. */
		for (irq = 0; irq < __MAX_SUBCHANNELS; irq++) {
			if (stsch(irq, &console_subchannel.schib) != 0)
				break;
			if (console_subchannel.schib.pmcw.dnv &&
			    console_subchannel.schib.pmcw.dev ==
			    console_devno) {
				console_irq = irq;
				break;
			}
		}
		if (console_irq == -1)
			return -1;
	} else {
		/* unlike in 2.4, we cannot autoprobe here, since
		 * the channel subsystem is not fully initialized.
		 * With some luck, the HWC console can take over */
		printk(KERN_WARNING "No ccw console found!\n");
		return -1;
	}
	return console_irq;
}

struct subchannel *
cio_probe_console(void)
{
	int irq, ret;

	if (xchg(&console_subchannel_in_use, 1) != 0)
		return ERR_PTR(-EBUSY);
	irq = cio_console_irq();
	if (irq == -1) {
		console_subchannel_in_use = 0;
		return ERR_PTR(-ENODEV);
	}
	memset(&console_subchannel, 0, sizeof(struct subchannel));
	ret = cio_validate_subchannel(&console_subchannel, irq);
	if (ret) {
		console_subchannel_in_use = 0;
		return ERR_PTR(-ENODEV);
	}

	/*
	 * enable console I/O-interrupt subclass 7
	 */
	ctl_set_bit(6, 24);
	console_subchannel.schib.pmcw.isc = 7;
	console_subchannel.schib.pmcw.intparm =
		(__u32)(unsigned long)&console_subchannel;
	ret = cio_modify(&console_subchannel);
	if (ret) {
		console_subchannel_in_use = 0;
		return ERR_PTR(ret);
	}
	return &console_subchannel;
}

void
cio_release_console(void)
{
	console_subchannel.schib.pmcw.intparm = 0;
	cio_modify(&console_subchannel);
	ctl_clear_bit(6, 24);
	console_subchannel_in_use = 0;
}

/* Bah... hack to catch console special sausages. */
int
cio_is_console(int irq)
{
	if (!console_subchannel_in_use)
		return 0;
	return (irq == console_subchannel.irq);
}

struct subchannel *
cio_get_console_subchannel(void)
{
	if (!console_subchannel_in_use)
		return 0;
	return &console_subchannel;
}

#endif
static inline int
__disable_subchannel_easy(unsigned int schid, struct schib *schib)
{
	int retry, cc;

	cc = 0;
	for (retry=0;retry<3;retry++) {
		schib->pmcw.ena = 0;
		cc = msch(schid, schib);
		if (cc)
			return (cc==3?-ENODEV:-EBUSY);
		stsch(schid, schib);
		if (!schib->pmcw.ena)
			return 0;
	}
	return -EBUSY; /* uhm... */
}

static inline int
__clear_subchannel_easy(unsigned int schid)
{
	int retry;

	if (csch(schid))
		return -ENODEV;
	for (retry=0;retry<20;retry++) {
		struct tpi_info ti;

		if (tpi(&ti)) {
			tsch(schid, (struct irb *)__LC_IRB);
			return 0;
		}
		udelay(100);
	}
	return -EBUSY;
}

extern void do_reipl(unsigned long devno);
/* Make sure all subchannels are quiet before we re-ipl an lpar. */
void
reipl(unsigned long devno)
{
	unsigned int schid;

	local_irq_disable();
	for (schid=0;schid<=highest_subchannel;schid++) {
		struct schib schib;
		if (stsch(schid, &schib))
			goto out;
		if (!schib.pmcw.ena)
			continue;
		switch(__disable_subchannel_easy(schid, &schib)) {
		case 0:
		case -ENODEV:
			break;
		default: /* -EBUSY */
			if (__clear_subchannel_easy(schid))
				break; /* give up... */
			stsch(schid, &schib);
			__disable_subchannel_easy(schid, &schib);
		}
	}
out:
	do_reipl(devno);
}
