/*
 *  linux/arch/m68k/mac/config.c
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 */

/*
 * Miscellaneous linux stuff
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/console.h>
#include <linux/interrupt.h>
/* keyb */
#include <linux/random.h>
#include <linux/delay.h>
/* keyb */
#include <linux/init.h>
#include <linux/vt_kern.h>

#define BOOTINFO_COMPAT_1_0
#include <asm/setup.h>
#include <asm/bootinfo.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/pgtable.h>
#include <asm/rtc.h>
#include <asm/machdep.h>

#include <asm/macintosh.h>
#include <asm/macints.h>
#include <asm/machw.h>

#include <asm/mac_iop.h>
#include <asm/mac_via.h>
#include <asm/mac_oss.h>
#include <asm/mac_psc.h>

/* Mac bootinfo struct */

struct mac_booter_data mac_bi_data;
int mac_bisize = sizeof mac_bi_data;

struct mac_hw_present mac_hw_present;

/* New m68k bootinfo stuff and videobase */

extern int m68k_num_memory;
extern struct mem_info m68k_memory[NUM_MEMINFO];

extern struct mem_info m68k_ramdisk;

extern char m68k_command_line[CL_SIZE];

void *mac_env;		/* Loaded by the boot asm */

/* The phys. video addr. - might be bogus on some machines */
unsigned long mac_orig_videoaddr;

/* Mac specific timer functions */
extern unsigned long mac_gettimeoffset (void);
extern int mac_hwclk (int, struct rtc_time *);
extern int mac_set_clock_mmss (unsigned long);
extern int show_mac_interrupts(struct seq_file *, void *);
extern void iop_preinit(void);
extern void iop_init(void);
extern void via_init(void);
extern void via_init_clock(irqreturn_t (*func)(int, void *, struct pt_regs *));
extern void via_flush_cache(void);
extern void oss_init(void);
extern void psc_init(void);
extern void baboon_init(void);

extern void mac_mksound(unsigned int, unsigned int);

extern void nubus_sweep_video(void);

/* Mac specific debug functions (in debug.c) */
extern void mac_debug_init(void);
extern void mac_debugging_long(int, long);

static void mac_get_model(char *str);

void mac_bang(int irq, void *vector, struct pt_regs *p)
{
	printk(KERN_INFO "Resetting ...\n");
	mac_reset();
}

static void mac_sched_init(irqreturn_t (*vector)(int, void *, struct pt_regs *))
{
	via_init_clock(vector);
}

#if 0
void mac_waitbut (void)
{
	;
}
#endif

extern irqreturn_t mac_default_handler(int, void *, struct pt_regs *);

irqreturn_t (*mac_handlers[8])(int, void *, struct pt_regs *)=
{
	mac_default_handler,
	mac_default_handler,
	mac_default_handler,
	mac_default_handler,
	mac_default_handler,
	mac_default_handler,
	mac_default_handler,
	mac_default_handler
};

/*
 * Parse a Macintosh-specific record in the bootinfo
 */

int __init mac_parse_bootinfo(const struct bi_record *record)
{
    int unknown = 0;
    const u_long *data = record->data;

    switch (record->tag) {
	case BI_MAC_MODEL:
	    mac_bi_data.id = *data;
	    break;
	case BI_MAC_VADDR:
	    mac_bi_data.videoaddr = *data;
	    break;
	case BI_MAC_VDEPTH:
	    mac_bi_data.videodepth = *data;
	    break;
	case BI_MAC_VROW:
	    mac_bi_data.videorow = *data;
	    break;
	case BI_MAC_VDIM:
	    mac_bi_data.dimensions = *data;
	    break;
	case BI_MAC_VLOGICAL:
	    mac_bi_data.videological = VIDEOMEMBASE + (*data & ~VIDEOMEMMASK);
	    mac_orig_videoaddr = *data;
	    break;
	case BI_MAC_SCCBASE:
	    mac_bi_data.sccbase = *data;
	    break;
	case BI_MAC_BTIME:
	    mac_bi_data.boottime = *data;
	    break;
	case BI_MAC_GMTBIAS:
	    mac_bi_data.gmtbias = *data;
	    break;
	case BI_MAC_MEMSIZE:
	    mac_bi_data.memsize = *data;
	    break;
	case BI_MAC_CPUID:
	    mac_bi_data.cpuid = *data;
	    break;
        case BI_MAC_ROMBASE:
	    mac_bi_data.rombase = *data;
	    break;
	default:
	    unknown = 1;
    }
    return(unknown);
}

/*
 * Flip into 24bit mode for an instant - flushes the L2 cache card. We
 * have to disable interrupts for this. Our IRQ handlers will crap
 * themselves if they take an IRQ in 24bit mode!
 */

static void mac_cache_card_flush(int writeback)
{
	unsigned long flags;
	local_irq_save(flags);
	via_flush_cache();
	local_irq_restore(flags);
}

void __init config_mac(void)
{
	if (!MACH_IS_MAC) {
	  printk(KERN_ERR "ERROR: no Mac, but config_mac() called!! \n");
	}

	mach_sched_init      = mac_sched_init;
	mach_init_IRQ        = mac_init_IRQ;
	mach_request_irq     = mac_request_irq;
	mach_free_irq        = mac_free_irq;
	enable_irq           = mac_enable_irq;
	disable_irq          = mac_disable_irq;
	mach_get_model	 = mac_get_model;
	mach_default_handler = &mac_handlers;
	mach_get_irq_list    = show_mac_interrupts;
	mach_gettimeoffset   = mac_gettimeoffset;
#warning move to adb/via init
#if 0
	mach_hwclk           = mac_hwclk;
#endif
	mach_set_clock_mmss	 = mac_set_clock_mmss;
	mach_reset           = mac_reset;
	mach_halt            = mac_poweroff;
	mach_power_off       = mac_poweroff;
#ifdef CONFIG_DUMMY_CONSOLE
	conswitchp	         = &dummy_con;
#endif
	mach_max_dma_address = 0xffffffff;
#if 0
	mach_debug_init	 = mac_debug_init;
#endif
#if defined(CONFIG_INPUT_M68K_BEEP) || defined(CONFIG_INPUT_M68K_BEEP_MODULE)
        mach_beep            = mac_mksound;
#endif
#ifdef CONFIG_HEARTBEAT
#if 0
	mach_heartbeat = mac_heartbeat;
	mach_heartbeat_irq = IRQ_MAC_TIMER;
#endif
#endif

	/*
	 * Determine hardware present
	 */

	mac_identify();
	mac_report_hardware();

	/* AFAIK only the IIci takes a cache card.  The IIfx has onboard
	   cache ... someone needs to figure out how to tell if it's on or
	   not. */

	if (macintosh_config->ident == MAC_MODEL_IICI
	    || macintosh_config->ident == MAC_MODEL_IIFX) {
		mach_l2_flush = mac_cache_card_flush;
	}

	/*
	 * Check for machine specific fixups.
	 */

#ifdef OLD_NUBUS_CODE
	 nubus_sweep_video();
#endif
}


/*
 *	Macintosh Table: hardcoded model configuration data.
 *
 *	Much of this was defined by Alan, based on who knows what docs.
 *	I've added a lot more, and some of that was pure guesswork based
 *	on hardware pages present on the Mac web site. Possibly wildly
 *	inaccurate, so look here if a new Mac model won't run. Example: if
 *	a Mac crashes immediately after the VIA1 registers have been dumped
 *	to the screen, it probably died attempting to read DirB on a RBV.
 *	Meaning it should have MAC_VIA_IIci here :-)
 */

struct mac_model *macintosh_config;
EXPORT_SYMBOL(macintosh_config);

static struct mac_model mac_data_table[]=
{
	/*
	 *	We'll pretend to be a Macintosh II, that's pretty safe.
	 */

	{
		.ident		= MAC_MODEL_II,
		.name		= "Unknown",
		.adb_type	= MAC_ADB_II,
		.via_type	= MAC_VIA_II,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	},

	/*
	 *	Original MacII hardware
	 *
	 */

	{
		.ident		= MAC_MODEL_II,
		.name		= "II",
		.adb_type	= MAC_ADB_II,
		.via_type	= MAC_VIA_II,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_IIX,
		.name		= "IIx",
		.adb_type	= MAC_ADB_II,
		.via_type	= MAC_VIA_II,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_IICX,
		.name		= "IIcx",
		.adb_type	= MAC_ADB_II,
		.via_type	= MAC_VIA_II,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_SE30,
		.name		= "SE/30",
		.adb_type	= MAC_ADB_II,
		.via_type	= MAC_VIA_II,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	},

	/*
	 *	Weirdified MacII hardware - all subtley different. Gee thanks
	 *	Apple. All these boxes seem to have VIA2 in a different place to
	 *	the MacII (+1A000 rather than +4000)
	 * CSA: see http://developer.apple.com/technotes/hw/hw_09.html
	 */

	{
		.ident		= MAC_MODEL_IICI,
		.name		= "IIci",
		.adb_type	= MAC_ADB_II,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_IIFX,
		.name		= "IIfx",
		.adb_type	= MAC_ADB_IOP,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_IOP,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_IISI,
		.name		= "IIsi",
		.adb_type	= MAC_ADB_IISI,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_IIVI,
		.name		= "IIvi",
		.adb_type	= MAC_ADB_IISI,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_IIVX,
		.name		= "IIvx",
		.adb_type	= MAC_ADB_IISI,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	},

	/*
	 *	Classic models (guessing: similar to SE/30 ?? Nope, similar to LC ...)
	 */

	{
		.ident		= MAC_MODEL_CLII,
		.name		= "Classic II",
		.adb_type	= MAC_ADB_IISI,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_CCL,
		.name		= "Color Classic",
		.adb_type	= MAC_ADB_CUDA,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS},

	/*
	 *	Some Mac LC machines. Basically the same as the IIci, ADB like IIsi
	 */

	{
		.ident		= MAC_MODEL_LC,
		.name		= "LC",
		.adb_type	= MAC_ADB_IISI,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_LCII,
		.name		= "LC II",
		.adb_type	= MAC_ADB_IISI,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_LCIII,
		.name		= "LC III",
		.adb_type	= MAC_ADB_IISI,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	},

	/*
	 *	Quadra. Video is at 0xF9000000, via is like a MacII. We label it differently
	 *	as some of the stuff connected to VIA2 seems different. Better SCSI chip and
	 *	onboard ethernet using a NatSemi SONIC except the 660AV and 840AV which use an
	 *	AMD 79C940 (MACE).
	 *	The 700, 900 and 950 have some I/O chips in the wrong place to
	 *	confuse us. The 840AV has a SCSI location of its own (same as
	 *	the 660AV).
	 */

	{
		.ident		= MAC_MODEL_Q605,
		.name		= "Quadra 605",
		.adb_type	= MAC_ADB_CUDA,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_QUADRA,
		.scc_type	= MAC_SCC_QUADRA,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_Q605_ACC,
		.name		= "Quadra 605",
		.adb_type	= MAC_ADB_CUDA,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_QUADRA,
		.scc_type	= MAC_SCC_QUADRA,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_Q610,
		.name		= "Quadra 610",
		.adb_type	= MAC_ADB_II,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_QUADRA,
		.scc_type	= MAC_SCC_QUADRA,
		.ether_type	= MAC_ETHER_SONIC,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_Q630,
		.name		= "Quadra 630",
		.adb_type	= MAC_ADB_CUDA,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_QUADRA,
		.ide_type	= MAC_IDE_QUADRA,
		.scc_type	= MAC_SCC_QUADRA,
		.ether_type	= MAC_ETHER_SONIC,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_Q650,
		.name		= "Quadra 650",
		.adb_type	= MAC_ADB_II,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_QUADRA,
		.scc_type	= MAC_SCC_QUADRA,
		.ether_type	= MAC_ETHER_SONIC,
		.nubus_type	= MAC_NUBUS
	},
	/*	The Q700 does have a NS Sonic */
	{
		.ident		= MAC_MODEL_Q700,
		.name		= "Quadra 700",
		.adb_type	= MAC_ADB_II,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_QUADRA2,
		.scc_type	= MAC_SCC_QUADRA,
		.ether_type	= MAC_ETHER_SONIC,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_Q800,
		.name		= "Quadra 800",
		.adb_type	= MAC_ADB_II,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_QUADRA,
		.scc_type	= MAC_SCC_QUADRA,
		.ether_type	= MAC_ETHER_SONIC,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_Q840,
		.name		= "Quadra 840AV",
		.adb_type	= MAC_ADB_CUDA,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_QUADRA3,
		.scc_type	= MAC_SCC_PSC,
		.ether_type	= MAC_ETHER_MACE,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_Q900,
		.name		= "Quadra 900",
		.adb_type	= MAC_ADB_IOP,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_QUADRA2,
		.scc_type	= MAC_SCC_IOP,
		.ether_type	= MAC_ETHER_SONIC,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_Q950,
		.name		= "Quadra 950",
		.adb_type	= MAC_ADB_IOP,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_QUADRA2,
		.scc_type	= MAC_SCC_IOP,
		.ether_type	= MAC_ETHER_SONIC,
		.nubus_type	= MAC_NUBUS
	},

	/*
	 *	Performa - more LC type machines
	 */

	{
		.ident		= MAC_MODEL_P460,
		.name		=  "Performa 460",
		.adb_type	= MAC_ADB_IISI,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_P475,
		.name		=  "Performa 475",
		.adb_type	= MAC_ADB_CUDA,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_QUADRA,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_P475F,
		.name		=  "Performa 475",
		.adb_type	= MAC_ADB_CUDA,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_QUADRA,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_P520,
		.name		=  "Performa 520",
		.adb_type	= MAC_ADB_CUDA,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_P550,
		.name		=  "Performa 550",
		.adb_type	= MAC_ADB_CUDA,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	},
	/* These have the comm slot, and therefore the possibility of SONIC ethernet */
	{
		.ident		= MAC_MODEL_P575,
		.name		= "Performa 575",
		.adb_type	= MAC_ADB_CUDA,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_QUADRA,
		.scc_type	= MAC_SCC_II,
		.ether_type	= MAC_ETHER_SONIC,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_P588,
		.name		= "Performa 588",
		.adb_type	= MAC_ADB_CUDA,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_QUADRA,
		.ide_type	= MAC_IDE_QUADRA,
		.scc_type	= MAC_SCC_II,
		.ether_type	= MAC_ETHER_SONIC,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_TV,
		.name		= "TV",
		.adb_type	= MAC_ADB_CUDA,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_P600,
		.name		= "Performa 600",
		.adb_type	= MAC_ADB_IISI,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_II,
		.nubus_type	= MAC_NUBUS
	},

	/*
	 *	Centris - just guessing again; maybe like Quadra
	 */

	/* The C610 may or may not have SONIC.  We probe to make sure */
	{
		.ident		= MAC_MODEL_C610,
		.name		= "Centris 610",
		.adb_type	= MAC_ADB_II,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_QUADRA,
		.scc_type	= MAC_SCC_QUADRA,
		.ether_type	= MAC_ETHER_SONIC,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_C650,
		.name		= "Centris 650",
		.adb_type	= MAC_ADB_II,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_QUADRA,
		.scc_type	= MAC_SCC_QUADRA,
		.ether_type	= MAC_ETHER_SONIC,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_C660,
		.name		= "Centris 660AV",
		.adb_type	= MAC_ADB_CUDA,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_QUADRA3,
		.scc_type	= MAC_SCC_PSC,
		.ether_type	= MAC_ETHER_MACE,
		.nubus_type	= MAC_NUBUS
	},

	/*
	 * The PowerBooks all the same "Combo" custom IC for SCSI and SCC
	 * and a PMU (in two variations?) for ADB. Most of them use the
	 * Quadra-style VIAs. A few models also have IDE from hell.
	 */

	{
		.ident		= MAC_MODEL_PB140,
		.name		= "PowerBook 140",
		.adb_type	= MAC_ADB_PB1,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_QUADRA,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_PB145,
		.name		= "PowerBook 145",
		.adb_type	= MAC_ADB_PB1,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_QUADRA,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_PB150,
		.name		= "PowerBook 150",
		.adb_type	= MAC_ADB_PB1,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.ide_type	= MAC_IDE_PB,
		.scc_type	= MAC_SCC_QUADRA,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_PB160,
		.name		= "PowerBook 160",
		.adb_type	= MAC_ADB_PB1,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_QUADRA,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_PB165,
		.name		= "PowerBook 165",
		.adb_type	= MAC_ADB_PB1,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_QUADRA,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_PB165C,
		.name		= "PowerBook 165c",
		.adb_type	= MAC_ADB_PB1,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_QUADRA,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_PB170,
		.name		= "PowerBook 170",
		.adb_type	= MAC_ADB_PB1,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_QUADRA,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_PB180,
		.name		= "PowerBook 180",
		.adb_type	= MAC_ADB_PB1,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_QUADRA,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_PB180C,
		.name		= "PowerBook 180c",
		.adb_type	= MAC_ADB_PB1,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_QUADRA,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_PB190,
		.name		= "PowerBook 190",
		.adb_type	= MAC_ADB_PB2,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_OLD,
		.ide_type	= MAC_IDE_BABOON,
		.scc_type	= MAC_SCC_QUADRA,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_PB520,
		.name		= "PowerBook 520",
		.adb_type	= MAC_ADB_PB2,
		.via_type	= MAC_VIA_QUADRA,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_QUADRA,
		.ether_type	= MAC_ETHER_SONIC,
		.nubus_type	= MAC_NUBUS
	},

	/*
	 * PowerBook Duos are pretty much like normal PowerBooks
	 * All of these probably have onboard SONIC in the Dock which
	 * means we'll have to probe for it eventually.
	 *
	 * Are these reallly MAC_VIA_IIci? The developer notes for the
	 * Duos show pretty much the same custom parts as in most of
	 * the other PowerBooks which would imply MAC_VIA_QUADRA.
	 */

	{
		.ident		= MAC_MODEL_PB210,
		.name		= "PowerBook Duo 210",
		.adb_type	= MAC_ADB_PB2,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_QUADRA,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_PB230,
		.name		= "PowerBook Duo 230",
		.adb_type	= MAC_ADB_PB2,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_QUADRA,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_PB250,
		.name		= "PowerBook Duo 250",
		.adb_type	= MAC_ADB_PB2,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_QUADRA,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_PB270C,
		.name		= "PowerBook Duo 270c",
		.adb_type	= MAC_ADB_PB2,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_QUADRA,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_PB280,
		.name		= "PowerBook Duo 280",
		.adb_type	= MAC_ADB_PB2,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_QUADRA,
		.nubus_type	= MAC_NUBUS
	}, {
		.ident		= MAC_MODEL_PB280C,
		.name		= "PowerBook Duo 280c",
		.adb_type	= MAC_ADB_PB2,
		.via_type	= MAC_VIA_IIci,
		.scsi_type	= MAC_SCSI_OLD,
		.scc_type	= MAC_SCC_QUADRA,
		.nubus_type	= MAC_NUBUS
	},

	/*
	 *	Other stuff ??
	 */
	{
		.ident		= -1
	}
};

void mac_identify(void)
{
	struct mac_model *m;

	/* Penguin data useful? */
	int model = mac_bi_data.id;
	if (!model) {
		/* no bootinfo model id -> NetBSD booter was used! */
		/* XXX FIXME: breaks for model > 31 */
		model=(mac_bi_data.cpuid>>2)&63;
		printk (KERN_WARNING "No bootinfo model ID, using cpuid instead (hey, use Penguin!)\n");
	}

	macintosh_config = mac_data_table;
	for (m = macintosh_config ; m->ident != -1 ; m++) {
		if (m->ident == model) {
			macintosh_config = m;
			break;
		}
	}

	/* We need to pre-init the IOPs, if any. Otherwise */
	/* the serial console won't work if the user had   */
	/* the serial ports set to "Faster" mode in MacOS. */

	iop_preinit();
	mac_debug_init();

	printk (KERN_INFO "Detected Macintosh model: %d \n", model);

	/*
	 * Report booter data:
	 */
	printk (KERN_DEBUG " Penguin bootinfo data:\n");
	printk (KERN_DEBUG " Video: addr 0x%lx row 0x%lx depth %lx dimensions %ld x %ld\n",
		mac_bi_data.videoaddr, mac_bi_data.videorow,
		mac_bi_data.videodepth, mac_bi_data.dimensions & 0xFFFF,
		mac_bi_data.dimensions >> 16);
	printk (KERN_DEBUG " Videological 0x%lx phys. 0x%lx, SCC at 0x%lx \n",
		mac_bi_data.videological, mac_orig_videoaddr,
		mac_bi_data.sccbase);
	printk (KERN_DEBUG " Boottime: 0x%lx GMTBias: 0x%lx \n",
		mac_bi_data.boottime, mac_bi_data.gmtbias);
	printk (KERN_DEBUG " Machine ID: %ld CPUid: 0x%lx memory size: 0x%lx \n",
		mac_bi_data.id, mac_bi_data.cpuid, mac_bi_data.memsize);
#if 0
	printk ("Ramdisk: addr 0x%lx size 0x%lx\n",
		m68k_ramdisk.addr, m68k_ramdisk.size);
#endif

	/*
	 * TODO: set the various fields in macintosh_config->hw_present here!
	 */
	switch (macintosh_config->scsi_type) {
	case MAC_SCSI_OLD:
	  MACHW_SET(MAC_SCSI_80);
	  break;
	case MAC_SCSI_QUADRA:
	case MAC_SCSI_QUADRA2:
	case MAC_SCSI_QUADRA3:
	  MACHW_SET(MAC_SCSI_96);
	  if ((macintosh_config->ident == MAC_MODEL_Q900) ||
	      (macintosh_config->ident == MAC_MODEL_Q950))
	    MACHW_SET(MAC_SCSI_96_2);
	  break;
	default:
	  printk(KERN_WARNING "config.c: wtf: unknown scsi, using 53c80\n");
	  MACHW_SET(MAC_SCSI_80);
	  break;

	}
	iop_init();
	via_init();
	oss_init();
	psc_init();
	baboon_init();
}

void mac_report_hardware(void)
{
	printk(KERN_INFO "Apple Macintosh %s\n", macintosh_config->name);
}

static void mac_get_model(char *str)
{
	strcpy(str,"Macintosh ");
	strcat(str, macintosh_config->name);
}
