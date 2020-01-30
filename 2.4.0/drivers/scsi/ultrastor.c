/*
 *	ultrastor.c	Copyright (C) 1992 David B. Gentzel
 *	Low-level SCSI driver for UltraStor 14F, 24F, and 34F
 *	by David B. Gentzel, Whitfield Software Services, Carnegie, PA
 *	    (gentzel@nova.enet.dec.com)
 *  scatter/gather added by Scott Taylor (n217cg@tamuts.tamu.edu)
 *  24F and multiple command support by John F. Carr (jfc@athena.mit.edu)
 *    John's work modified by Caleb Epstein (cae@jpmorgan.com) and 
 *    Eric Youngdale (ericy@cais.com).
 *	Thanks to UltraStor for providing the necessary documentation
 */

/*
 * TODO:
 *	1. Find out why scatter/gather is limited to 16 requests per command.
 *         This is fixed, at least on the 24F, as of version 1.12 - CAE.
 *	2. Look at command linking (mscp.command_link and
 *	   mscp.command_link_id).  (Does not work with many disks, 
 *				and no performance increase.  ERY).
 *	3. Allow multiple adapters.
 */

/*
 * NOTES:
 *    The UltraStor 14F, 24F, and 34F are a family of intelligent, high
 *    performance SCSI-2 host adapters.  They all support command queueing
 *    and scatter/gather I/O.  Some of them can also emulate the standard
 *    WD1003 interface for use with OS's which don't support SCSI.  Here
 *    is the scoop on the various models:
 *	14F - ISA first-party DMA HA with floppy support and WD1003 emulation.
 *	14N - ISA HA with floppy support.  I think that this is a non-DMA
 *	      HA.  Nothing further known.
 *	24F - EISA Bus Master HA with floppy support and WD1003 emulation.
 *	34F - VL-Bus Bus Master HA with floppy support (no WD1003 emulation).
 *
 *    The 14F, 24F, and 34F are supported by this driver.
 *
 *    Places flagged with a triple question-mark are things which are either
 *    unfinished, questionable, or wrong.
 */

/* Changes from version 1.11 alpha to 1.12
 *
 * Increased the size of the scatter-gather list to 33 entries for
 * the 24F adapter (it was 16).  I don't have the specs for the 14F
 * or the 34F, so they may support larger s-g lists as well.
 *
 * Caleb Epstein <cae@jpmorgan.com>
 */

/* Changes from version 1.9 to 1.11
 *
 * Patches to bring this driver up to speed with the default kernel
 * driver which supports only the 14F and 34F adapters.  This version
 * should compile cleanly into 0.99.13, 0.99.12 and probably 0.99.11.
 *
 * Fixes from Eric Youngdale to fix a few possible race conditions and
 * several problems with bit testing operations (insufficient
 * parentheses).
 *
 * Removed the ultrastor_abort() and ultrastor_reset() functions
 * (enclosed them in #if 0 / #endif).  These functions, at least on
 * the 24F, cause the SCSI bus to do odd things and generally lead to
 * kernel panics and machine hangs.  This is like the Adaptec code.
 *
 * Use check/snarf_region for 14f, 34f to avoid I/O space address conflicts.
 */

/* Changes from version 1.8 to version 1.9
 *
 *  0.99.11 patches (cae@jpmorgan.com) */

/* Changes from version 1.7 to version 1.8
 *
 * Better error reporting.
 */

/* Changes from version 1.6 to version 1.7
 *
 * Removed CSIR command code.
 *
 * Better race condition avoidance (xchgb function added).
 *
 * Set ICM and OGM status to zero at probe (24F)
 *
 * reset sends soft reset to UltraStor adapter
 *
 * reset adapter if adapter interrupts with an invalid MSCP address
 *
 * handle aborted command interrupt (24F)
 *
 */

/* Changes from version 1.5 to version 1.6:
 *
 * Read MSCP address from ICM _before_ clearing the interrupt flag.
 * This fixes a race condition.
 */

/* Changes from version 1.4 to version 1.5:
 *
 * Abort now calls done when multiple commands are enabled.
 *
 * Clear busy when aborted command finishes, not when abort is called.
 *
 * More debugging messages for aborts.
 */

/* Changes from version 1.3 to version 1.4:
 *
 * Enable automatic request of sense data on error (requires newer version
 * of scsi.c to be useful).
 *
 * Fix PORT_OVERRIDE for 14F.
 *
 * Fix abort and reset to work properly (config.aborted wasn't cleared
 * after it was tested, so after a command abort no further commands would
 * work).
 *
 * Boot time test to enable SCSI bus reset (defaults to not allowing reset).
 *
 * Fix test for OGM busy -- the busy bit is in different places on the 24F.
 *
 * Release ICM slot by clearing first byte on 24F.
 */

#ifdef MODULE
#include <linux/module.h>
#endif

#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/proc_fs.h>
#include <linux/spinlock.h>
#include <asm/io.h>
#include <asm/bitops.h>
#include <asm/system.h>
#include <asm/dma.h>

#define ULTRASTOR_PRIVATE	/* Get the private stuff from ultrastor.h */
#include <linux/blk.h>
#include "scsi.h"
#include "hosts.h"
#include "ultrastor.h"
#include "sd.h"
#include<linux/stat.h>

#define FALSE 0
#define TRUE 1

#ifndef ULTRASTOR_DEBUG
#define ULTRASTOR_DEBUG (UD_ABORT|UD_CSIR|UD_RESET)
#endif

#define VERSION "1.12"

#define PACKED		__attribute__((packed))
#define ALIGNED(x)	__attribute__((aligned(x)))


/* The 14F uses an array of 4-byte ints for its scatter/gather list.
   The data can be unaligned, but need not be.  It's easier to give
   the list normal alignment since it doesn't need to fit into a
   packed structure.  */

typedef struct {
  unsigned int address;
  unsigned int num_bytes;
} ultrastor_sg_list;


/* MailBox SCSI Command Packet.  Basic command structure for communicating
   with controller. */
struct mscp {
  unsigned char opcode: 3;		/* type of command */
  unsigned char xdir: 2;		/* data transfer direction */
  unsigned char dcn: 1;		/* disable disconnect */
  unsigned char ca: 1;		/* use cache (if available) */
  unsigned char sg: 1;		/* scatter/gather operation */
  unsigned char target_id: 3;		/* target SCSI id */
  unsigned char ch_no: 2;		/* SCSI channel (always 0 for 14f) */
  unsigned char lun: 3;		/* logical unit number */
  unsigned int transfer_data PACKED;	/* transfer data pointer */
  unsigned int transfer_data_length PACKED;	/* length in bytes */
  unsigned int command_link PACKED;	/* for linking command chains */
  unsigned char scsi_command_link_id;	/* identifies command in chain */
  unsigned char number_of_sg_list;	/* (if sg is set) 8 bytes per list */
  unsigned char length_of_sense_byte;
  unsigned char length_of_scsi_cdbs;	/* 6, 10, or 12 */
  unsigned char scsi_cdbs[12];	/* SCSI commands */
  unsigned char adapter_status;	/* non-zero indicates HA error */
  unsigned char target_status;	/* non-zero indicates target error */
  unsigned int sense_data PACKED;
  /* The following fields are for software only.  They are included in
     the MSCP structure because they are associated with SCSI requests.  */
  void (*done)(Scsi_Cmnd *);
  Scsi_Cmnd *SCint;
  ultrastor_sg_list sglist[ULTRASTOR_24F_MAX_SG]; /* use larger size for 24F */
};


/* Port addresses (relative to the base address) */
#define U14F_PRODUCT_ID(port) ((port) + 0x4)
#define CONFIG(port) ((port) + 0x6)

/* Port addresses relative to the doorbell base address.  */
#define LCL_DOORBELL_MASK(port) ((port) + 0x0)
#define LCL_DOORBELL_INTR(port) ((port) + 0x1)
#define SYS_DOORBELL_MASK(port) ((port) + 0x2)
#define SYS_DOORBELL_INTR(port) ((port) + 0x3)


/* Used to store configuration info read from config i/o registers.  Most of
   this is not used yet, but might as well save it.
   
   This structure also holds port addresses that are not at the same offset
   on the 14F and 24F.
   
   This structure holds all data that must be duplicated to support multiple
   adapters.  */

static struct ultrastor_config
{
  unsigned short port_address;		/* base address of card */
  unsigned short doorbell_address;	/* base address of doorbell CSRs */
  unsigned short ogm_address;		/* base address of OGM */
  unsigned short icm_address;		/* base address of ICM */
  const void *bios_segment;
  unsigned char interrupt: 4;
  unsigned char dma_channel: 3;
  unsigned char bios_drive_number: 1;
  unsigned char heads;
  unsigned char sectors;
  unsigned char ha_scsi_id: 3;
  unsigned char subversion: 4;
  unsigned char revision;
  /* The slot number is used to distinguish the 24F (slot != 0) from
     the 14F and 34F (slot == 0). */
  unsigned char slot;

#ifdef PRINT_U24F_VERSION
  volatile int csir_done;
#endif

  /* A pool of MSCP structures for this adapter, and a bitmask of
     busy structures.  (If ULTRASTOR_14F_MAX_CMDS == 1, a 1 byte
     busy flag is used instead.)  */

#if ULTRASTOR_MAX_CMDS == 1
  unsigned char mscp_busy;
#else
  unsigned short mscp_free;
#endif
  volatile unsigned char aborted[ULTRASTOR_MAX_CMDS];
  struct mscp mscp[ULTRASTOR_MAX_CMDS];
} config = {0};

/* Set this to 1 to reset the SCSI bus on error.  */
int ultrastor_bus_reset = 0;


/* Allowed BIOS base addresses (NULL indicates reserved) */
static const void *const bios_segment_table[8] = {
  NULL,	     (void *)0xC4000, (void *)0xC8000, (void *)0xCC000,
  (void *)0xD0000, (void *)0xD4000, (void *)0xD8000, (void *)0xDC000,
};

/* Allowed IRQs for 14f */
static const unsigned char interrupt_table_14f[4] = { 15, 14, 11, 10 };

/* Allowed DMA channels for 14f (0 indicates reserved) */
static const unsigned char dma_channel_table_14f[4] = { 5, 6, 7, 0 };

/* Head/sector mappings allowed by 14f */
static const struct {
  unsigned char heads;
  unsigned char sectors;
} mapping_table[4] = { { 16, 63 }, { 64, 32 }, { 64, 63 }, { 64, 32 } };

#ifndef PORT_OVERRIDE
/* ??? A probe of address 0x310 screws up NE2000 cards */
static const unsigned short ultrastor_ports_14f[] = {
  0x330, 0x340, /*0x310,*/ 0x230, 0x240, 0x210, 0x130, 0x140,
};
#endif

static void ultrastor_interrupt(int, void *, struct pt_regs *);
static void do_ultrastor_interrupt(int, void *, struct pt_regs *);
static inline void build_sg_list(struct mscp *, Scsi_Cmnd *SCpnt);


static inline int find_and_clear_bit_16(unsigned short *field)
{
  int rv;
  unsigned long flags;

  save_flags(flags);
  cli();
  if (*field == 0) panic("No free mscp");
  asm("xorl %0,%0\n0:\tbsfw %1,%w0\n\tbtr %0,%1\n\tjnc 0b"
      : "=&r" (rv), "=m" (*field) : "1" (*field));
  restore_flags(flags);
  return rv;
}

/* This has been re-implemented with the help of Richard Earnshaw,
   <rwe@pegasus.esprit.ec.org> and works with gcc-2.5.8 and gcc-2.6.0.
   The instability noted by jfc below appears to be a bug in
   gcc-2.5.x when compiling w/o optimization.  --Caleb

   This asm is fragile: it doesn't work without the casts and it may
   not work without optimization.  Maybe I should add a swap builtin
   to gcc.  --jfc  */
static inline unsigned char xchgb(unsigned char reg,
				  volatile unsigned char *mem)
{
  __asm__ ("xchgb %0,%1" : "=q" (reg), "=m" (*mem) : "0" (reg));
  return reg;
}

#if ULTRASTOR_DEBUG & (UD_COMMAND | UD_ABORT)

static void log_ultrastor_abort(register struct ultrastor_config *config,
				int command)
{
  static char fmt[80] = "abort %d (%x); MSCP free pool: %x;";
  register int i;
  int flags;
  save_flags(flags);
  cli();

  for (i = 0; i < ULTRASTOR_MAX_CMDS; i++)
    {
      fmt[20 + i*2] = ' ';
      if (! (config->mscp_free & (1 << i)))
	fmt[21 + i*2] = '0' + config->mscp[i].target_id;
      else
	fmt[21 + i*2] = '-';
    }
  fmt[20 + ULTRASTOR_MAX_CMDS * 2] = '\n';
  fmt[21 + ULTRASTOR_MAX_CMDS * 2] = 0;
  printk(fmt, command, &config->mscp[command], config->mscp_free);
  restore_flags(flags);
}
#endif

static int ultrastor_14f_detect(Scsi_Host_Template * tpnt)
{
    size_t i;
    unsigned char in_byte, version_byte = 0;
    struct config_1 {
      unsigned char bios_segment: 3;
      unsigned char removable_disks_as_fixed: 1;
      unsigned char interrupt: 2;
    unsigned char dma_channel: 2;
    } config_1;
    struct config_2 {
      unsigned char ha_scsi_id: 3;
      unsigned char mapping_mode: 2;
      unsigned char bios_drive_number: 1;
      unsigned char tfr_port: 2;
    } config_2;

#if (ULTRASTOR_DEBUG & UD_DETECT)
    printk("US14F: detect: called\n");
#endif

    /* If a 24F has already been configured, don't look for a 14F.  */
    if (config.bios_segment)
	return FALSE;

#ifdef PORT_OVERRIDE
    if(check_region(PORT_OVERRIDE, 0xc)) {
      printk("Ultrastor I/O space already in use\n");
      return FALSE;
    };
    config.port_address = PORT_OVERRIDE;
#else
    for (i = 0; i < ARRAY_SIZE(ultrastor_ports_14f); i++) {
      if(check_region(ultrastor_ports_14f[i], 0x0c)) continue;
      config.port_address = ultrastor_ports_14f[i];
#endif

#if (ULTRASTOR_DEBUG & UD_DETECT)
	printk("US14F: detect: testing port address %03X\n", config.port_address);
#endif

	in_byte = inb(U14F_PRODUCT_ID(config.port_address));
	if (in_byte != US14F_PRODUCT_ID_0) {
#if (ULTRASTOR_DEBUG & UD_DETECT)
# ifdef PORT_OVERRIDE
	    printk("US14F: detect: wrong product ID 0 - %02X\n", in_byte);
# else
	    printk("US14F: detect: no adapter at port %03X\n", config.port_address);
# endif
#endif
#ifdef PORT_OVERRIDE
	    return FALSE;
#else
	    continue;
#endif
	}
	in_byte = inb(U14F_PRODUCT_ID(config.port_address) + 1);
	/* Only upper nibble is significant for Product ID 1 */
	if ((in_byte & 0xF0) != US14F_PRODUCT_ID_1) {
#if (ULTRASTOR_DEBUG & UD_DETECT)
# ifdef PORT_OVERRIDE
	    printk("US14F: detect: wrong product ID 1 - %02X\n", in_byte);
# else
	    printk("US14F: detect: no adapter at port %03X\n", config.port_address);
# endif
#endif
#ifdef PORT_OVERRIDE
	    return FALSE;
#else
	    continue;
#endif
	}
	version_byte = in_byte;
#ifndef PORT_OVERRIDE
	break;
    }
    if (i == ARRAY_SIZE(ultrastor_ports_14f)) {
# if (ULTRASTOR_DEBUG & UD_DETECT)
	printk("US14F: detect: no port address found!\n");
# endif
	return FALSE;
    }
#endif

#if (ULTRASTOR_DEBUG & UD_DETECT)
    printk("US14F: detect: adapter found at port address %03X\n",
	   config.port_address);
#endif

    /* Set local doorbell mask to disallow bus reset unless
       ultrastor_bus_reset is true.  */
    outb(ultrastor_bus_reset ? 0xc2 : 0x82, LCL_DOORBELL_MASK(config.port_address));

    /* All above tests passed, must be the right thing.  Get some useful
       info. */

    request_region(config.port_address, 0x0c,"ultrastor"); 
    /* Register the I/O space that we use */

    *(char *)&config_1 = inb(CONFIG(config.port_address + 0));
    *(char *)&config_2 = inb(CONFIG(config.port_address + 1));
    config.bios_segment = bios_segment_table[config_1.bios_segment];
    config.doorbell_address = config.port_address;
    config.ogm_address = config.port_address + 0x8;
    config.icm_address = config.port_address + 0xC;
    config.interrupt = interrupt_table_14f[config_1.interrupt];
    config.ha_scsi_id = config_2.ha_scsi_id;
    config.heads = mapping_table[config_2.mapping_mode].heads;
    config.sectors = mapping_table[config_2.mapping_mode].sectors;
    config.bios_drive_number = config_2.bios_drive_number;
    config.subversion = (version_byte & 0x0F);
    if (config.subversion == U34F)
	config.dma_channel = 0;
    else
	config.dma_channel = dma_channel_table_14f[config_1.dma_channel];

    if (!config.bios_segment) {
#if (ULTRASTOR_DEBUG & UD_DETECT)
	printk("US14F: detect: not detected.\n");
#endif
	return FALSE;
    }

    /* Final consistency check, verify previous info. */
    if (config.subversion != U34F)
	if (!config.dma_channel || !(config_2.tfr_port & 0x2)) {
#if (ULTRASTOR_DEBUG & UD_DETECT)
	    printk("US14F: detect: consistency check failed\n");
#endif
	    return FALSE;
	}

    /* If we were TRULY paranoid, we could issue a host adapter inquiry
       command here and verify the data returned.  But frankly, I'm
       exhausted! */

    /* Finally!  Now I'm satisfied... */
#if (ULTRASTOR_DEBUG & UD_DETECT)
    printk("US14F: detect: detect succeeded\n"
	   "  Port address: %03X\n"
	   "  BIOS segment: %05X\n"
	   "  Interrupt: %u\n"
	   "  DMA channel: %u\n"
	   "  H/A SCSI ID: %u\n"
	   "  Subversion: %u\n",
	   config.port_address, config.bios_segment, config.interrupt,
	   config.dma_channel, config.ha_scsi_id, config.subversion);
#endif
    tpnt->this_id = config.ha_scsi_id;
    tpnt->unchecked_isa_dma = (config.subversion != U34F);

#if ULTRASTOR_MAX_CMDS > 1
    config.mscp_free = ~0;
#endif

    if (request_irq(config.interrupt, do_ultrastor_interrupt, 0, "Ultrastor", NULL)) {
	printk("Unable to allocate IRQ%u for UltraStor controller.\n",
	       config.interrupt);
	return FALSE;
    }
    if (config.dma_channel && request_dma(config.dma_channel,"Ultrastor")) {
	printk("Unable to allocate DMA channel %u for UltraStor controller.\n",
	       config.dma_channel);
	free_irq(config.interrupt, NULL);
	return FALSE;
    }
    tpnt->sg_tablesize = ULTRASTOR_14F_MAX_SG;
    printk("UltraStor driver version" VERSION ".  Using %d SG lists.\n",
	   ULTRASTOR_14F_MAX_SG);

    return TRUE;
}

static int ultrastor_24f_detect(Scsi_Host_Template * tpnt)
{
  register int i;
  struct Scsi_Host * shpnt = NULL;

#if (ULTRASTOR_DEBUG & UD_DETECT)
  printk("US24F: detect");
#endif

  /* probe each EISA slot at slot address C80 */
  for (i = 1; i < 15; i++)
    {
      unsigned char config_1, config_2;
      unsigned short addr = (i << 12) | ULTRASTOR_24F_PORT;

      if (inb(addr) != US24F_PRODUCT_ID_0 &&
	  inb(addr+1) != US24F_PRODUCT_ID_1 &&
	  inb(addr+2) != US24F_PRODUCT_ID_2)
	continue;

      config.revision = inb(addr+3);
      config.slot = i;
      if (! (inb(addr+4) & 1))
	{
#if (ULTRASTOR_DEBUG & UD_DETECT)
	  printk("U24F: found disabled card in slot %u\n", i);
#endif
	  continue;
	}
#if (ULTRASTOR_DEBUG & UD_DETECT)
      printk("U24F: found card in slot %u\n", i);
#endif
      config_1 = inb(addr + 5);
      config.bios_segment = bios_segment_table[config_1 & 7];
      switch(config_1 >> 4)
	{
	case 1:
	  config.interrupt = 15;
	  break;
	case 2:
	  config.interrupt = 14;
	  break;
	case 4:
	  config.interrupt = 11;
	  break;
	case 8:
	  config.interrupt = 10;
	  break;
	default:
	  printk("U24F: invalid IRQ\n");
	  return FALSE;
	}
      if (request_irq(config.interrupt, do_ultrastor_interrupt, 0, "Ultrastor", NULL))
	{
	  printk("Unable to allocate IRQ%u for UltraStor controller.\n",
		 config.interrupt);
	  return FALSE;
	}
      /* BIOS addr set */
      /* base port set */
      config.port_address = addr;
      config.doorbell_address = addr + 12;
      config.ogm_address = addr + 0x17;
      config.icm_address = addr + 0x1C;
      config_2 = inb(addr + 7);
      config.ha_scsi_id = config_2 & 7;
      config.heads = mapping_table[(config_2 >> 3) & 3].heads;
      config.sectors = mapping_table[(config_2 >> 3) & 3].sectors;
#if (ULTRASTOR_DEBUG & UD_DETECT)
      printk("US24F: detect: detect succeeded\n"
	     "  Port address: %03X\n"
	     "  BIOS segment: %05X\n"
	     "  Interrupt: %u\n"
	     "  H/A SCSI ID: %u\n",
	     config.port_address, config.bios_segment,
	     config.interrupt, config.ha_scsi_id);
#endif
      tpnt->this_id = config.ha_scsi_id;
      tpnt->unchecked_isa_dma = 0;
      tpnt->sg_tablesize = ULTRASTOR_24F_MAX_SG;

      shpnt = scsi_register(tpnt, 0);
      shpnt->irq = config.interrupt;
      shpnt->dma_channel = config.dma_channel;
      shpnt->io_port = config.port_address;

#if ULTRASTOR_MAX_CMDS > 1
      config.mscp_free = ~0;
#endif
      /* Mark ICM and OGM free */
      outb(0, addr + 0x16);
      outb(0, addr + 0x1B);

      /* Set local doorbell mask to disallow bus reset unless
	 ultrastor_bus_reset is true.  */
      outb(ultrastor_bus_reset ? 0xc2 : 0x82, LCL_DOORBELL_MASK(addr+12));
      outb(0x02, SYS_DOORBELL_MASK(addr+12));
      printk("UltraStor driver version " VERSION ".  Using %d SG lists.\n",
	     tpnt->sg_tablesize);
      return TRUE;
    }
  return FALSE;
}

int ultrastor_detect(Scsi_Host_Template * tpnt)
{
    tpnt->proc_name = "ultrastor";
  return ultrastor_14f_detect(tpnt) || ultrastor_24f_detect(tpnt);
}

const char *ultrastor_info(struct Scsi_Host * shpnt)
{
    static char buf[64];

    if (config.slot)
      sprintf(buf, "UltraStor 24F SCSI @ Slot %u IRQ%u",
	      config.slot, config.interrupt);
    else if (config.subversion)
      sprintf(buf, "UltraStor 34F SCSI @ Port %03X BIOS %05X IRQ%u",
	      config.port_address, (int)config.bios_segment,
	      config.interrupt);
    else
      sprintf(buf, "UltraStor 14F SCSI @ Port %03X BIOS %05X IRQ%u DMA%u",
	      config.port_address, (int)config.bios_segment,
	      config.interrupt, config.dma_channel);
    return buf;
}

static inline void build_sg_list(register struct mscp *mscp, Scsi_Cmnd *SCpnt)
{
	struct scatterlist *sl;
	long transfer_length = 0;
	int i, max;

	sl = (struct scatterlist *) SCpnt->request_buffer;
	max = SCpnt->use_sg;
	for (i = 0; i < max; i++) {
		mscp->sglist[i].address = virt_to_bus(sl[i].address);
		mscp->sglist[i].num_bytes = sl[i].length;
		transfer_length += sl[i].length;
	}
	mscp->number_of_sg_list = max;
	mscp->transfer_data = virt_to_bus(mscp->sglist);
	/* ??? May not be necessary.  Docs are unclear as to whether transfer
	   length field is ignored or whether it should be set to the total
	   number of bytes of the transfer.  */
	mscp->transfer_data_length = transfer_length;
}

int ultrastor_queuecommand(Scsi_Cmnd *SCpnt, void (*done)(Scsi_Cmnd *))
{
    register struct mscp *my_mscp;
#if ULTRASTOR_MAX_CMDS > 1
    int mscp_index;
#endif
    unsigned int status;
    int flags;

    /* Next test is for debugging; "can't happen" */
    if ((config.mscp_free & ((1U << ULTRASTOR_MAX_CMDS) - 1)) == 0)
	panic("ultrastor_queuecommand: no free MSCP\n");
    mscp_index = find_and_clear_bit_16(&config.mscp_free);

    /* Has the command been aborted?  */
    if (xchgb(0xff, &config.aborted[mscp_index]) != 0)
      {
	status = DID_ABORT << 16;
	goto aborted;
      }

    my_mscp = &config.mscp[mscp_index];

#if 1
    /* This way is faster.  */
    *(unsigned char *)my_mscp = OP_SCSI | (DTD_SCSI << 3);
#else
    my_mscp->opcode = OP_SCSI;
    my_mscp->xdir = DTD_SCSI;
    my_mscp->dcn = FALSE;
#endif
    /* Tape drives don't work properly if the cache is used.  The SCSI
       READ command for a tape doesn't have a block offset, and the adapter
       incorrectly assumes that all reads from the tape read the same
       blocks.  Results will depend on read buffer size and other disk
       activity. 

       ???  Which other device types should never use the cache?   */
    my_mscp->ca = SCpnt->device->type != TYPE_TAPE;
    my_mscp->target_id = SCpnt->target;
    my_mscp->ch_no = 0;
    my_mscp->lun = SCpnt->lun;
    if (SCpnt->use_sg) {
	/* Set scatter/gather flag in SCSI command packet */
	my_mscp->sg = TRUE;
	build_sg_list(my_mscp, SCpnt);
    } else {
	/* Unset scatter/gather flag in SCSI command packet */
	my_mscp->sg = FALSE;
	my_mscp->transfer_data = virt_to_bus(SCpnt->request_buffer);
	my_mscp->transfer_data_length = SCpnt->request_bufflen;
    }
    my_mscp->command_link = 0;		/*???*/
    my_mscp->scsi_command_link_id = 0;	/*???*/
    my_mscp->length_of_sense_byte = sizeof SCpnt->sense_buffer;
    my_mscp->length_of_scsi_cdbs = SCpnt->cmd_len;
    memcpy(my_mscp->scsi_cdbs, SCpnt->cmnd, my_mscp->length_of_scsi_cdbs);
    my_mscp->adapter_status = 0;
    my_mscp->target_status = 0;
    my_mscp->sense_data = virt_to_bus(&SCpnt->sense_buffer);
    my_mscp->done = done;
    my_mscp->SCint = SCpnt;
    SCpnt->host_scribble = (unsigned char *)my_mscp;

    /* Find free OGM slot.  On 24F, look for OGM status byte == 0.
       On 14F and 34F, wait for local interrupt pending flag to clear.  */

  retry:
    if (config.slot)
	while (inb(config.ogm_address - 1) != 0 &&
	       config.aborted[mscp_index] == 0xff) barrier();

    /* else??? */

    while ((inb(LCL_DOORBELL_INTR(config.doorbell_address)) & 
	    (config.slot ? 2 : 1)) 
	   && config.aborted[mscp_index] == 0xff) barrier();

    /* To avoid race conditions, make the code to write to the adapter
       atomic.  This simplifies the abort code.  */

    save_flags(flags);
    cli();

    if (inb(LCL_DOORBELL_INTR(config.doorbell_address)) &
	(config.slot ? 2 : 1))
      {
      restore_flags(flags);
      goto retry;
      }

    status = xchgb(0, &config.aborted[mscp_index]);
    if (status != 0xff) {
	restore_flags(flags);

#if ULTRASTOR_DEBUG & (UD_COMMAND | UD_ABORT)
	printk("USx4F: queuecommand: aborted\n");
#if ULTRASTOR_MAX_CMDS > 1
	log_ultrastor_abort(&config, mscp_index);
#endif
#endif
	status <<= 16;

      aborted:
	set_bit(mscp_index, &config.mscp_free);
	/* If the driver queues commands, call the done proc here.  Otherwise
	   return an error.  */
#if ULTRASTOR_MAX_CMDS > 1
	SCpnt->result = status;
	done(SCpnt);
	return 0;
#else
	return status;
#endif
    }

    /* Store pointer in OGM address bytes */
    outl(virt_to_bus(my_mscp), config.ogm_address);

    /* Issue OGM interrupt */
    if (config.slot) {
	/* Write OGM command register on 24F */
	outb(1, config.ogm_address - 1);
	outb(0x2, LCL_DOORBELL_INTR(config.doorbell_address));
    } else {
	outb(0x1, LCL_DOORBELL_INTR(config.doorbell_address));
    }

    restore_flags(flags);

#if (ULTRASTOR_DEBUG & UD_COMMAND)
    printk("USx4F: queuecommand: returning\n");
#endif

    return 0;
}

/* This code must deal with 2 cases:

   1. The command has not been written to the OGM.  In this case, set
   the abort flag and return.

   2. The command has been written to the OGM and is stuck somewhere in
   the adapter.

   2a.  On a 24F, ask the adapter to abort the command.  It will interrupt
   when it does.

   2b.  Call the command's done procedure.

 */

int ultrastor_abort(Scsi_Cmnd *SCpnt)
{
#if ULTRASTOR_DEBUG & UD_ABORT
    char out[108];
    unsigned char icm_status = 0, ogm_status = 0;
    unsigned int icm_addr = 0, ogm_addr = 0;
#endif
    unsigned int mscp_index;
    unsigned char old_aborted;
    void (*done)(Scsi_Cmnd *);

    if(config.slot) 
      return SCSI_ABORT_SNOOZE;  /* Do not attempt an abort for the 24f */

    /* Simple consistency checking */
    if(!SCpnt->host_scribble)
      return SCSI_ABORT_NOT_RUNNING;

    mscp_index = ((struct mscp *)SCpnt->host_scribble) - config.mscp;
    if (mscp_index >= ULTRASTOR_MAX_CMDS)
	panic("Ux4F aborting invalid MSCP");

#if ULTRASTOR_DEBUG & UD_ABORT
    if (config.slot)
      {
	int port0 = (config.slot << 12) | 0xc80;
	int i;
	int flags;
	save_flags(flags);
	cli();
	strcpy(out, "OGM %d:%x ICM %d:%x ports:  ");
	for (i = 0; i < 16; i++)
	  {
	    unsigned char p = inb(port0 + i);
	    out[28 + i * 3] = "0123456789abcdef"[p >> 4];
	    out[29 + i * 3] = "0123456789abcdef"[p & 15];
	    out[30 + i * 3] = ' ';
	  }
	out[28 + i * 3] = '\n';
	out[29 + i * 3] = 0;
	ogm_status = inb(port0 + 22);
	ogm_addr = (unsigned int)bus_to_virt(inl(port0 + 23));
	icm_status = inb(port0 + 27);
	icm_addr = (unsigned int)bus_to_virt(inl(port0 + 28));
	restore_flags(flags);
      }

    /* First check to see if an interrupt is pending.  I suspect the SiS
       chipset loses interrupts.  (I also suspect is mangles data, but
       one bug at a time... */
    if (config.slot ? inb(config.icm_address - 1) == 2 :
	(inb(SYS_DOORBELL_INTR(config.doorbell_address)) & 1))
      {
	int flags;
	save_flags(flags);
	printk("Ux4F: abort while completed command pending\n");
	restore_flags(flags);
	cli();
	ultrastor_interrupt(0, NULL, NULL);
	restore_flags(flags);
	return SCSI_ABORT_SUCCESS;  /* FIXME - is this correct? -ERY */
      }
#endif

    old_aborted = xchgb(DID_ABORT, &config.aborted[mscp_index]);

    /* aborted == 0xff is the signal that queuecommand has not yet sent
       the command.  It will notice the new abort flag and fail.  */
    if (old_aborted == 0xff)
	return SCSI_ABORT_SUCCESS;

    /* On 24F, send an abort MSCP request.  The adapter will interrupt
       and the interrupt handler will call done.  */
    if (config.slot && inb(config.ogm_address - 1) == 0)
      {
	int flags;

	save_flags(flags);
	cli();
	outl(virt_to_bus(&config.mscp[mscp_index]), config.ogm_address);
	inb(0xc80);	/* delay */
	outb(0x80, config.ogm_address - 1);
	outb(0x2, LCL_DOORBELL_INTR(config.doorbell_address));
#if ULTRASTOR_DEBUG & UD_ABORT
	log_ultrastor_abort(&config, mscp_index);
	printk(out, ogm_status, ogm_addr, icm_status, icm_addr);
#endif
	restore_flags(flags);
	return SCSI_ABORT_PENDING;
      }

#if ULTRASTOR_DEBUG & UD_ABORT
    log_ultrastor_abort(&config, mscp_index);
#endif

    /* Can't request a graceful abort.  Either this is not a 24F or
       the OGM is busy.  Don't free the command -- the adapter might
       still be using it.  Setting SCint = 0 causes the interrupt
       handler to ignore the command.  */

    /* FIXME - devices that implement soft resets will still be running
       the command after a bus reset.  We would probably rather leave
       the command in the queue.  The upper level code will automatically
       leave the command in the active state instead of requeueing it. ERY */

#if ULTRASTOR_DEBUG & UD_ABORT
    if (config.mscp[mscp_index].SCint != SCpnt)
	printk("abort: command mismatch, %p != %p\n",
	       config.mscp[mscp_index].SCint, SCpnt);
#endif
    if (config.mscp[mscp_index].SCint == 0)
	return SCSI_ABORT_NOT_RUNNING;

    if (config.mscp[mscp_index].SCint != SCpnt) panic("Bad abort");
    config.mscp[mscp_index].SCint = 0;
    done = config.mscp[mscp_index].done;
    config.mscp[mscp_index].done = 0;
    SCpnt->result = DID_ABORT << 16;
    /* I worry about reentrancy in scsi.c  */
    done(SCpnt);

    /* Need to set a timeout here in case command never completes.  */
    return SCSI_ABORT_SUCCESS;
}

int ultrastor_reset(Scsi_Cmnd * SCpnt, unsigned int reset_flags)
{
    int flags;
    register int i;
#if (ULTRASTOR_DEBUG & UD_RESET)
    printk("US14F: reset: called\n");
#endif

    if(config.slot)
      return SCSI_RESET_PUNT;  /* Do not attempt a reset for the 24f */

    save_flags(flags);
    cli();

    /* Reset the adapter and SCSI bus.  The SCSI bus reset can be
       inhibited by clearing ultrastor_bus_reset before probe.  */
    outb(0xc0, LCL_DOORBELL_INTR(config.doorbell_address));
    if (config.slot)
      {
	outb(0, config.ogm_address - 1);
	outb(0, config.icm_address - 1);
      }

#if ULTRASTOR_MAX_CMDS == 1
    if (config.mscp_busy && config.mscp->done && config.mscp->SCint)
      {
	config.mscp->SCint->result = DID_RESET << 16;
	config.mscp->done(config.mscp->SCint);
      }
    config.mscp->SCint = 0;
#else
    for (i = 0; i < ULTRASTOR_MAX_CMDS; i++)
      {
	if (! (config.mscp_free & (1 << i)) &&
	    config.mscp[i].done && config.mscp[i].SCint)
	  {
	    config.mscp[i].SCint->result = DID_RESET << 16;
	    config.mscp[i].done(config.mscp[i].SCint);
	    config.mscp[i].done = 0;
	  }
	config.mscp[i].SCint = 0;
      }
#endif

    /* FIXME - if the device implements soft resets, then the command
       will still be running.  ERY */

    memset((unsigned char *)config.aborted, 0, sizeof config.aborted);
#if ULTRASTOR_MAX_CMDS == 1
    config.mscp_busy = 0;
#else
    config.mscp_free = ~0;
#endif

    restore_flags(flags);
    return SCSI_RESET_SUCCESS;

}

int ultrastor_biosparam(Disk * disk, kdev_t dev, int * dkinfo)
{
    int size = disk->capacity;
    unsigned int s = config.heads * config.sectors;

    dkinfo[0] = config.heads;
    dkinfo[1] = config.sectors;
    dkinfo[2] = size / s;	/* Ignore partial cylinders */
#if 0
    if (dkinfo[2] > 1024)
	dkinfo[2] = 1024;
#endif
    return 0;
}

static void ultrastor_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
    unsigned int status;
#if ULTRASTOR_MAX_CMDS > 1
    unsigned int mscp_index;
#endif
    register struct mscp *mscp;
    void (*done)(Scsi_Cmnd *);
    Scsi_Cmnd *SCtmp;

#if ULTRASTOR_MAX_CMDS == 1
    mscp = &config.mscp[0];
#else
    mscp = (struct mscp *)bus_to_virt(inl(config.icm_address));
    mscp_index = mscp - config.mscp;
    if (mscp_index >= ULTRASTOR_MAX_CMDS) {
	printk("Ux4F interrupt: bad MSCP address %x\n", (unsigned int) mscp);
	/* A command has been lost.  Reset and report an error
	   for all commands.  */
	ultrastor_reset(NULL, 0);
	return;
    }
#endif

    /* Clean ICM slot (set ICMINT bit to 0) */
    if (config.slot) {
	unsigned char icm_status = inb(config.icm_address - 1);
#if ULTRASTOR_DEBUG & (UD_INTERRUPT|UD_ERROR|UD_ABORT)
	if (icm_status != 1 && icm_status != 2)
	    printk("US24F: ICM status %x for MSCP %d (%x)\n", icm_status,
		   mscp_index, (unsigned int) mscp);
#endif
	/* The manual says clear interrupt then write 0 to ICM status.
	   This seems backwards, but I'll do it anyway.  --jfc */
	outb(2, SYS_DOORBELL_INTR(config.doorbell_address));
	outb(0, config.icm_address - 1);
	if (icm_status == 4) {
	    printk("UltraStor abort command failed\n");
	    return;
	}
	if (icm_status == 3) {
	    void (*done)(Scsi_Cmnd *) = mscp->done;
	    if (done) {
		mscp->done = 0;
		mscp->SCint->result = DID_ABORT << 16;
		done(mscp->SCint);
	    }
	    return;
	}
    } else {
	outb(1, SYS_DOORBELL_INTR(config.doorbell_address));
    }

    SCtmp = mscp->SCint;
    mscp->SCint = NULL;

    if (SCtmp == 0)
      {
#if ULTRASTOR_DEBUG & (UD_ABORT|UD_INTERRUPT)
	printk("MSCP %d (%x): no command\n", mscp_index, (unsigned int) mscp);
#endif	
#if ULTRASTOR_MAX_CMDS == 1
	config.mscp_busy = FALSE;
#else
	set_bit(mscp_index, &config.mscp_free);
#endif
	config.aborted[mscp_index] = 0;
	return;
      }

    /* Save done locally and zero before calling.  This is needed as
       once we call done, we may get another command queued before this
       interrupt service routine can return. */
    done = mscp->done;
    mscp->done = 0;

    /* Let the higher levels know that we're done */
    switch (mscp->adapter_status)
      {
      case 0:
	status = DID_OK << 16;
	break;
      case 0x01:	/* invalid command */
      case 0x02:	/* invalid parameters */
      case 0x03:	/* invalid data list */
      default:
	status = DID_ERROR << 16;
	break;
      case 0x84:	/* SCSI bus abort */
	status = DID_ABORT << 16;
	break;
      case 0x91:
	status = DID_TIME_OUT << 16;
	break;
      }

    SCtmp->result = status | mscp->target_status;

    SCtmp->host_scribble = 0;

    /* Free up mscp block for next command */
#if ULTRASTOR_MAX_CMDS == 1
    config.mscp_busy = FALSE;
#else
    set_bit(mscp_index, &config.mscp_free);
#endif

#if ULTRASTOR_DEBUG & (UD_ABORT|UD_INTERRUPT)
    if (config.aborted[mscp_index])
	printk("Ux4 interrupt: MSCP %d (%x) aborted = %d\n",
	       mscp_index, (unsigned int) mscp, config.aborted[mscp_index]);
#endif
    config.aborted[mscp_index] = 0;

    if (done)
	done(SCtmp);
    else
	printk("US14F: interrupt: unexpected interrupt\n");

    if (config.slot ? inb(config.icm_address - 1) :
       (inb(SYS_DOORBELL_INTR(config.doorbell_address)) & 1))
#if (ULTRASTOR_DEBUG & UD_MULTI_CMD)
      printk("Ux4F: multiple commands completed\n");
#else
      ;
#endif

#if (ULTRASTOR_DEBUG & UD_INTERRUPT)
    printk("USx4F: interrupt: returning\n");
#endif
}

static void do_ultrastor_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
    unsigned long flags;

    spin_lock_irqsave(&io_request_lock, flags);
    ultrastor_interrupt(irq, dev_id, regs);
    spin_unlock_irqrestore(&io_request_lock, flags);
}

/* Eventually this will go into an include file, but this will be later */
static Scsi_Host_Template driver_template = ULTRASTOR_14F;

#include "scsi_module.c"
