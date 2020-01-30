/*
 *  linux/drivers/ide/ht6560b.c		Version 0.07	Feb  1, 2000
 *
 *  Copyright (C) 1995-2000  Linus Torvalds & author (see below)
 */

/*
 *
 *  Version 0.01        Initial version hacked out of ide.c
 *
 *  Version 0.02        Added support for PIO modes, auto-tune
 *
 *  Version 0.03        Some cleanups
 *
 *  Version 0.05        PIO mode cycle timings auto-tune using bus-speed
 *
 *  Version 0.06        Prefetch mode now defaults no OFF. To set
 *                      prefetch mode OFF/ON use "hdparm -p8/-p9".
 *                      Unmask irq is disabled when prefetch mode
 *                      is enabled.
 *
 *  Version 0.07        Trying to fix CD-ROM detection problem.
 *                      "Prefetch" mode bit OFF for ide disks and
 *                      ON for anything else.
 *
 *
 *  HT-6560B EIDE-controller support
 *  To activate controller support use kernel parameter "ide0=ht6560b".
 *  Use hdparm utility to enable PIO mode support.
 *
 *  Author:    Mikko Ala-Fossi            <maf@iki.fi>
 *             Jan Evert van Grootheest   <janevert@iae.nl>
 *
 *  Try:  http://www.maf.iki.fi/~maf/ht6560b/
 */

#define HT6560B_VERSION "v0.07"

#undef REALLY_SLOW_IO		/* most systems can safely undef this */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/ioport.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/ide.h>
#include <linux/init.h>

#include <asm/io.h>

#include "ide_modes.h"

/* #define DEBUG */  /* remove comments for DEBUG messages */

/*
 * The special i/o-port that HT-6560B uses to configuration:
 *    bit0 (0x01): "1" selects secondary interface
 *    bit2 (0x04): "1" enables FIFO function
 *    bit5 (0x20): "1" enables prefetched data read function  (???)
 *
 * The special i/o-port that HT-6560A uses to configuration:
 *    bit0 (0x01): "1" selects secondary interface
 *    bit1 (0x02): "1" enables prefetched data read function
 *    bit2 (0x04): "0" enables multi-master system	      (?)
 *    bit3 (0x08): "1" 3 cycle time, "0" 2 cycle time	      (?)
 */
#define HT_CONFIG_PORT	  0x3e6
#define HT_CONFIG(drivea) (byte)(((drivea)->drive_data & 0xff00) >> 8)
/*
 * FIFO + PREFETCH (both a/b-model)
 */
#define HT_CONFIG_DEFAULT 0x1c /* no prefetch */
/* #define HT_CONFIG_DEFAULT 0x3c */ /* with prefetch */
#define HT_SECONDARY_IF	  0x01
#define HT_PREFETCH_MODE  0x20

/*
 * ht6560b Timing values:
 *
 * I reviewed some assembler source listings of htide drivers and found
 * out how they setup those cycle time interfacing values, as they at Holtek
 * call them. IDESETUP.COM that is supplied with the drivers figures out
 * optimal values and fetches those values to drivers. I found out that
 * they use IDE_SELECT_REG to fetch timings to the ide board right after
 * interface switching. After that it was quite easy to add code to
 * ht6560b.c.
 *
 * IDESETUP.COM gave me values 0x24, 0x45, 0xaa, 0xff that worked fine
 * for hda and hdc. But hdb needed higher values to work, so I guess
 * that sometimes it is necessary to give higher value than IDESETUP
 * gives.   [see cmd640.c for an extreme example of this. -ml]
 *
 * Perhaps I should explain something about these timing values:
 * The higher nibble of value is the Recovery Time  (rt) and the lower nibble
 * of the value is the Active Time  (at). Minimum value 2 is the fastest and
 * the maximum value 15 is the slowest. Default values should be 15 for both.
 * So 0x24 means 2 for rt and 4 for at. Each of the drives should have
 * both values, and IDESETUP gives automatically rt=15 st=15 for CDROMs or
 * similar. If value is too small there will be all sorts of failures.
 *
 * Timing byte consists of
 *	High nibble:  Recovery Cycle Time  (rt)
 *	     The valid values range from 2 to 15. The default is 15.
 *
 *	Low nibble:   Active Cycle Time	   (at)
 *	     The valid values range from 2 to 15. The default is 15.
 *
 * You can obtain optimized timing values by running Holtek IDESETUP.COM
 * for DOS. DOS drivers get their timing values from command line, where
 * the first value is the Recovery Time and the second value is the
 * Active Time for each drive. Smaller value gives higher speed.
 * In case of failures you should probably fall back to a higher value.
 */
#define HT_TIMING(drivea) (byte)((drivea)->drive_data & 0x00ff)
#define HT_TIMING_DEFAULT 0xff

/*
 * This routine handles interface switching for the peculiar hardware design
 * on the F.G.I./Holtek HT-6560B VLB IDE interface.
 * The HT-6560B can only enable one IDE port at a time, and requires a
 * silly sequence (below) whenever we switch between primary and secondary.
 */

/*
 * This routine is invoked from ide.c to prepare for access to a given drive.
 */
static void ht6560b_selectproc (ide_drive_t *drive)
{
	unsigned long flags;
	static byte current_select = 0;
	static byte current_timing = 0;
	byte select, timing;
	
	__save_flags (flags);	/* local CPU only */
	__cli();		/* local CPU only */
	
	select = HT_CONFIG(drive);
	timing = HT_TIMING(drive);
	
	if (select != current_select || timing != current_timing) {
		current_select = select;
		current_timing = timing;
		if (drive->media != ide_disk || !drive->present)
			select |= HT_PREFETCH_MODE;
		(void) inb(HT_CONFIG_PORT);
		(void) inb(HT_CONFIG_PORT);
		(void) inb(HT_CONFIG_PORT);
		(void) inb(HT_CONFIG_PORT);
		outb(select, HT_CONFIG_PORT);
		/*
		 * Set timing for this drive:
		 */
		outb(timing, IDE_SELECT_REG);
		(void) inb(IDE_STATUS_REG);
#ifdef DEBUG
		printk("ht6560b: %s: select=%#x timing=%#x\n", drive->name, select, timing);
#endif
	}
	__restore_flags (flags);	/* local CPU only */
}

/*
 * Autodetection and initialization of ht6560b
 */
static int __init try_to_init_ht6560b(void)
{
	byte orig_value;
	int i;
	
	/* Autodetect ht6560b */
	if ((orig_value=inb(HT_CONFIG_PORT)) == 0xff)
		return 0;
	
	for (i=3;i>0;i--) {
		outb(0x00, HT_CONFIG_PORT);
		if (!( (~inb(HT_CONFIG_PORT)) & 0x3f )) {
			outb(orig_value, HT_CONFIG_PORT);
			return 0;
		}
	}
	outb(0x00, HT_CONFIG_PORT);
	if ((~inb(HT_CONFIG_PORT))& 0x3f) {
		outb(orig_value, HT_CONFIG_PORT);
		return 0;
	}
	/*
	 * Ht6560b autodetected
	 */
	outb(HT_CONFIG_DEFAULT, HT_CONFIG_PORT);
	outb(HT_TIMING_DEFAULT, 0x1f6);  /* IDE_SELECT_REG */
	(void) inb(0x1f7);               /* IDE_STATUS_REG */
	
	printk("\nht6560b " HT6560B_VERSION
	       ": chipset detected and initialized"
#ifdef DEBUG
	       " with debug enabled"
#endif
		);
	return 1;
}

static byte ht_pio2timings(ide_drive_t *drive, byte pio)
{
	int active_time, recovery_time;
	int active_cycles, recovery_cycles;
	ide_pio_data_t d;
	int bus_speed = system_bus_clock();
	
        if (pio) {
		pio = ide_get_best_pio_mode(drive, pio, 5, &d);
		
		/*
		 *  Just like opti621.c we try to calculate the
		 *  actual cycle time for recovery and activity
		 *  according system bus speed.
		 */
		active_time = ide_pio_timings[pio].active_time;
		recovery_time = d.cycle_time 
			- active_time
			- ide_pio_timings[pio].setup_time;
		/*
		 *  Cycle times should be Vesa bus cycles
		 */
		active_cycles   = (active_time   * bus_speed + 999) / 1000;
		recovery_cycles = (recovery_time * bus_speed + 999) / 1000;
		/*
		 *  Upper and lower limits
		 */
		if (active_cycles   < 2)  active_cycles   = 2;
		if (recovery_cycles < 2)  recovery_cycles = 2;
		if (active_cycles   > 15) active_cycles   = 15;
		if (recovery_cycles > 15) recovery_cycles = 0;  /* 0==16 */
		
#ifdef DEBUG
		printk("ht6560b: drive %s setting pio=%d recovery=%d (%dns) active=%d (%dns)\n", drive->name, pio, recovery_cycles, recovery_time, active_cycles, active_time);
#endif
		
		return (byte)((recovery_cycles << 4) | active_cycles);
	} else {
		
#ifdef DEBUG
		printk("ht6560b: drive %s setting pio=0\n", drive->name);
#endif
		
		return HT_TIMING_DEFAULT;    /* default setting */
	}
}

/*
 *  Enable/Disable so called prefetch mode
 */
static void ht_set_prefetch(ide_drive_t *drive, byte state)
{
	unsigned long flags;
	int t = HT_PREFETCH_MODE << 8;
	
	save_flags (flags);	/* all CPUs */
	cli();		        /* all CPUs */
	
	/*
	 *  Prefetch mode and unmask irq seems to conflict
	 */
	if (state) {
		drive->drive_data |= t;   /* enable prefetch mode */
		drive->no_unmask = 1;
		drive->unmask = 0;
	} else {
		drive->drive_data &= ~t;  /* disable prefetch mode */
		drive->no_unmask = 0;
	}
	
	restore_flags (flags);	/* all CPUs */
	
#ifdef DEBUG
	printk("ht6560b: drive %s prefetch mode %sabled\n", drive->name, (state ? "en" : "dis"));
#endif
}

static void tune_ht6560b (ide_drive_t *drive, byte pio)
{
	unsigned long flags;
	byte timing;
	
	switch (pio) {
	case 8:         /* set prefetch off */
	case 9:         /* set prefetch on */
		ht_set_prefetch(drive, pio & 1);
		return;
	}
	
	timing = ht_pio2timings(drive, pio);
	
	save_flags (flags);	/* all CPUs */
	cli();		        /* all CPUs */
	
	drive->drive_data &= 0xff00;
	drive->drive_data |= timing;
	
	restore_flags (flags);	/* all CPUs */
	
#ifdef DEBUG
	printk("ht6560b: drive %s tuned to pio mode %#x timing=%#x\n", drive->name, pio, timing);
#endif
}

void __init init_ht6560b (void)
{
	int t;
	
	if (check_region(HT_CONFIG_PORT,1)) {
		printk(KERN_ERR "ht6560b: PORT %#x ALREADY IN USE\n", HT_CONFIG_PORT);
	} else {
		if (try_to_init_ht6560b()) {
			request_region(HT_CONFIG_PORT, 1, ide_hwifs[0].name);
			ide_hwifs[0].chipset = ide_ht6560b;
			ide_hwifs[1].chipset = ide_ht6560b;
			ide_hwifs[0].selectproc = &ht6560b_selectproc;
			ide_hwifs[1].selectproc = &ht6560b_selectproc;
			ide_hwifs[0].tuneproc = &tune_ht6560b;
			ide_hwifs[1].tuneproc = &tune_ht6560b;
			ide_hwifs[0].serialized = 1;  /* is this needed? */
			ide_hwifs[1].serialized = 1;  /* is this needed? */
			ide_hwifs[0].mate = &ide_hwifs[1];
			ide_hwifs[1].mate = &ide_hwifs[0];
			ide_hwifs[1].channel = 1;
			
			/*
			 * Setting default configurations for drives
			 */
			t = (HT_CONFIG_DEFAULT << 8);
			t |= HT_TIMING_DEFAULT;
			ide_hwifs[0].drives[0].drive_data = t;
			ide_hwifs[0].drives[1].drive_data = t;
			t |= (HT_SECONDARY_IF << 8);
			ide_hwifs[1].drives[0].drive_data = t;
			ide_hwifs[1].drives[1].drive_data = t;
		} else
			printk(KERN_ERR "ht6560b: not found\n");
	}
}
