/*
 * Generic Generic NCR5380 driver
 *	
 * Copyright 1993, Drew Eckhardt
 *	Visionary Computing
 *	(Unix and Linux consulting and custom programming)
 *	drew@colorado.edu
 *      +1 (303) 440-4894
 *
 * NCR53C400 extensions (c) 1994,1995,1996, Kevin Lentin
 *    K.Lentin@cs.monash.edu.au
 *
 * NCR53C400A extensions (c) 1996, Ingmar Baumgart
 *    ingmar@gonzo.schwaben.de
 *
 * DTC3181E extensions (c) 1997, Ronald van Cuijlenborg
 * ronald.van.cuijlenborg@tip.nl or nutty@dds.nl
 *
 * Added ISAPNP support for DTC436 adapters,
 * Thomas Sailer, sailer@ife.ee.ethz.ch
 *
 * ALPHA RELEASE 1. 
 *
 * For more information, please consult 
 *
 * NCR 5380 Family
 * SCSI Protocol Controller
 * Databook
 *
 * NCR Microelectronics
 * 1635 Aeroplaza Drive
 * Colorado Springs, CO 80916
 * 1+ (719) 578-3400
 * 1+ (800) 334-5454
 */

/* 
 * TODO : flesh out DMA support, find some one actually using this (I have
 * 	a memory mapped Trantor board that works fine)
 */

/*
 * Options :
 *
 * PARITY - enable parity checking.  Not supported.
 *
 * SCSI2 - enable support for SCSI-II tagged queueing.  Untested.
 *
 * USLEEP - enable support for devices that don't disconnect.  Untested.
 *
 * The card is detected and initialized in one of several ways : 
 * 1.  With command line overrides - NCR5380=port,irq may be 
 *     used on the LILO command line to override the defaults.
 *
 * 2.  With the GENERIC_NCR5380_OVERRIDE compile time define.  This is 
 *     specified as an array of address, irq, dma, board tuples.  Ie, for
 *     one board at 0x350, IRQ5, no dma, I could say  
 *     -DGENERIC_NCR5380_OVERRIDE={{0xcc000, 5, DMA_NONE, BOARD_NCR5380}}
 * 
 * -1 should be specified for no or DMA interrupt, -2 to autoprobe for an 
 * 	IRQ line if overridden on the command line.
 *
 * 3.  When included as a module, with arguments passed on the command line:
 *         ncr_irq=xx	the interrupt
 *         ncr_addr=xx  the port or base address (for port or memory
 *              	mapped, resp.)
 *         ncr_dma=xx	the DMA
 *         ncr_5380=1	to set up for a NCR5380 board
 *         ncr_53c400=1	to set up for a NCR53C400 board
 *     e.g.
 *     modprobe g_NCR5380 ncr_irq=5 ncr_addr=0x350 ncr_5380=1
 *       for a port mapped NCR5380 board or
 *     modprobe g_NCR5380 ncr_irq=255 ncr_addr=0xc8000 ncr_53c400=1
 *       for a memory mapped NCR53C400 board with interrupts disabled.
 * 
 * 255 should be specified for no or DMA interrupt, 254 to autoprobe for an 
 * 	IRQ line if overridden on the command line.
 *     
 */
 
/*
 * $Log: generic_NCR5380.c,v $
 */

/* settings for DTC3181E card with only Mustek scanner attached */
#define USLEEP
#define USLEEP_POLL	1
#define USLEEP_SLEEP	20
#define USLEEP_WAITLONG	500

#define AUTOPROBE_IRQ
#define AUTOSENSE

#include <linux/config.h>

#ifdef CONFIG_SCSI_GENERIC_NCR53C400
#define NCR53C400_PSEUDO_DMA 1
#define PSEUDO_DMA
#define NCR53C400
#define NCR5380_STATS
#undef NCR5380_STAT_LIMIT
#endif
#if defined(CONFIG_SCSI_G_NCR5380_PORT) && defined(CONFIG_SCSI_G_NCR5380_MEM)
#error You can not configure the Generic NCR 5380 SCSI Driver for memory mapped I/O and port mapped I/O at the same time (yet)
#endif
#if !defined(CONFIG_SCSI_G_NCR5380_PORT) && !defined(CONFIG_SCSI_G_NCR5380_MEM)
#error You must configure the Generic NCR 5380 SCSI Driver for one of memory mapped I/O and port mapped I/O.
#endif

#include <asm/system.h>
#include <asm/io.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/blk.h>
#include "scsi.h"
#include "hosts.h"
#include "g_NCR5380.h"
#include "NCR5380.h"
#include "constants.h"
#include "sd.h"
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/isapnp.h>

#define NCR_NOT_SET 0
static int ncr_irq=NCR_NOT_SET;
static int ncr_dma=NCR_NOT_SET;
static int ncr_addr=NCR_NOT_SET;
static int ncr_5380=NCR_NOT_SET;
static int ncr_53c400=NCR_NOT_SET;
static int ncr_53c400a=NCR_NOT_SET;
static int dtc_3181e=NCR_NOT_SET;

static struct override {
	NCR5380_implementation_fields;
    int irq;
    int dma;
    int board;	/* Use NCR53c400, Ricoh, etc. extensions ? */
} overrides 
#ifdef GENERIC_NCR5380_OVERRIDE 
    [] __initdata = GENERIC_NCR5380_OVERRIDE
#else
    [1] __initdata = {{0,},};
#endif

#define NO_OVERRIDES (sizeof(overrides) / sizeof(struct override))

/*
 * Function : static internal_setup(int board, char *str, int *ints)
 *
 * Purpose : LILO command line initialization of the overrides array,
 * 
 * Inputs : board - either BOARD_NCR5380 for a normal NCR5380 board, 
 * 	or BOARD_NCR53C400 for a NCR53C400 board. str - unused, ints - 
 *	array of integer parameters with ints[0] equal to the number of ints.
 *
 */

static void __init internal_setup(int board, char *str, int *ints){
    static int commandline_current = 0;
    switch (board) {
    case BOARD_NCR5380:
	if (ints[0] != 2 && ints[0] != 3) {
	    printk("generic_NCR5380_setup : usage ncr5380=" STRVAL(NCR5380_map_name) ",irq,dma\n");
	    return;
	}
	break;
    case BOARD_NCR53C400:
	if (ints[0] != 2) {
	    printk("generic_NCR53C400_setup : usage ncr53c400=" STRVAL(NCR5380_map_name) ",irq\n");
	    return;
	}
	break;
    case BOARD_NCR53C400A:
	if (ints[0] != 2) {
	    printk("generic_NCR53C400A_setup : usage ncr53c400a=" STRVAL(NCR5380_map_name) ",irq\n");
	    return;
	}
	break;
    case BOARD_DTC3181E:
	if (ints[0] != 2) {
	    printk("generic_DTC3181E_setup : usage dtc3181e=" STRVAL(NCR5380_map_name) ",irq\n");
	    return;
	}
	break;
    }

    if (commandline_current < NO_OVERRIDES) {
	overrides[commandline_current].NCR5380_map_name = (NCR5380_map_type)ints[1];
	overrides[commandline_current].irq = ints[2];
	if (ints[0] == 3) 
	    overrides[commandline_current].dma = ints[3];
	else 
	    overrides[commandline_current].dma = DMA_NONE;
	overrides[commandline_current].board = board;
	    ++commandline_current;
    }
}

/*
 * Function : generic_NCR5380_setup (char *str, int *ints)
 *
 * Purpose : LILO command line initialization of the overrides array,
 * 
 * Inputs : str - unused, ints - array of integer parameters with ints[0] 
 * 	equal to the number of ints.
 */

void __init generic_NCR5380_setup (char *str, int *ints){
    internal_setup (BOARD_NCR5380, str, ints);
}

/*
 * Function : generic_NCR53C400_setup (char *str, int *ints)
 *
 * Purpose : LILO command line initialization of the overrides array,
 * 
 * Inputs : str - unused, ints - array of integer parameters with ints[0] 
 * 	equal to the number of ints.
 */

void __init generic_NCR53C400_setup (char *str, int *ints){
    internal_setup (BOARD_NCR53C400, str, ints);
}

/*
 * Function : generic_NCR53C400A_setup (char *str, int *ints)
 *
 * Purpose : LILO command line initialization of the overrides array,
 * 
 * Inputs : str - unused, ints - array of integer parameters with ints[0] 
 * 	equal to the number of ints.
 */

void generic_NCR53C400A_setup (char *str, int *ints) {
    internal_setup (BOARD_NCR53C400A, str, ints);
}

/*
 * Function : generic_DTC3181E_setup (char *str, int *ints)
 *
 * Purpose : LILO command line initialization of the overrides array,
 * 
 * Inputs : str - unused, ints - array of integer parameters with ints[0] 
 * 	equal to the number of ints.
 */

void generic_DTC3181E_setup (char *str, int *ints) {
    internal_setup (BOARD_DTC3181E, str, ints);
}

/* 
 * Function : int generic_NCR5380_detect(Scsi_Host_Template * tpnt)
 *
 * Purpose : initializes generic NCR5380 driver based on the 
 *	command line / compile time port and irq definitions.
 *
 * Inputs : tpnt - template for this SCSI adapter.
 * 
 * Returns : 1 if a host adapter was found, 0 if not.
 *
 */

int __init generic_NCR5380_detect(Scsi_Host_Template * tpnt){
    static int current_override = 0;
    int count, i;
    u_int *ports;
    u_int ncr_53c400a_ports[] = {0x280, 0x290, 0x300, 0x310, 0x330,
    		     		0x340, 0x348, 0x350, 0};
    u_int dtc_3181e_ports[] = {0x220, 0x240, 0x280, 0x2a0, 0x2c0,
    			     0x300, 0x320, 0x340, 0};
    int flags = 0;
    struct Scsi_Host *instance;

    if (ncr_irq != NCR_NOT_SET)
	overrides[0].irq=ncr_irq;
    if (ncr_dma != NCR_NOT_SET)
	overrides[0].dma=ncr_dma;
    if (ncr_addr != NCR_NOT_SET)
	overrides[0].NCR5380_map_name=(NCR5380_map_type)ncr_addr;
    if (ncr_5380 != NCR_NOT_SET)
	overrides[0].board=BOARD_NCR5380;
    else if (ncr_53c400 != NCR_NOT_SET)
	overrides[0].board=BOARD_NCR53C400;
    else if (ncr_53c400a != NCR_NOT_SET)
        overrides[0].board=BOARD_NCR53C400A;
    else if (dtc_3181e != NCR_NOT_SET)
        overrides[0].board=BOARD_DTC3181E;

    if (!current_override && isapnp_present()) {
	    struct pci_dev *dev = NULL;
	    count = 0;
	    while ((dev = isapnp_find_dev(NULL, ISAPNP_VENDOR('D','T','C'), ISAPNP_FUNCTION(0x436e), dev))) {
		    if (count >= NO_OVERRIDES)
			    break;
		    if (!dev->active && dev->prepare(dev) < 0) {
			    printk(KERN_ERR "dtc436e probe: prepare failed\n");
			    continue;
		    }
		    if (!(dev->resource[0].flags & IORESOURCE_IO))
			    continue;
		    if (!dev->active && dev->activate(dev) < 0) {
			    printk(KERN_ERR "dtc436e probe: activate failed\n");
			    continue;
		    }
		    if (dev->irq_resource[0].flags & IORESOURCE_IRQ)
			    overrides[count].irq=dev->irq_resource[0].start;
		    else
			    overrides[count].irq=IRQ_NONE;
		    if (dev->dma_resource[0].flags & IORESOURCE_DMA)
			    overrides[count].dma=dev->dma_resource[0].start;
		    else
			    overrides[count].dma=DMA_NONE;
		    overrides[count].NCR5380_map_name=(NCR5380_map_type)dev->resource[0].start;
		    overrides[count].board=BOARD_DTC3181E;
		    count++;
	    }
    }

    tpnt->proc_name = "g_NCR5380";

    for (count = 0; current_override < NO_OVERRIDES; ++current_override) {
	if (!(overrides[current_override].NCR5380_map_name))
	    continue;

	ports = 0;
	switch (overrides[current_override].board) {
	case BOARD_NCR5380:
	    flags = FLAG_NO_PSEUDO_DMA;
	    break;
	case BOARD_NCR53C400:
	    flags = FLAG_NCR53C400;
	    break;
	case BOARD_NCR53C400A:
	    flags = FLAG_NO_PSEUDO_DMA;
            ports = ncr_53c400a_ports;
	    break;
	case BOARD_DTC3181E:
	    flags = FLAG_NO_PSEUDO_DMA | FLAG_DTC3181E;
	    ports = dtc_3181e_ports;
	    break;
	}

#ifdef CONFIG_SCSI_G_NCR5380_PORT
	if (ports) {
	    /* wakeup sequence for the NCR53C400A and DTC3181E*/

	    /* Disable the adapter and look for a free io port */
	    outb(0x59, 0x779);
	    outb(0xb9, 0x379);
	    outb(0xc5, 0x379);
	    outb(0xae, 0x379);
	    outb(0xa6, 0x379);
	    outb(0x00, 0x379);

	    if (overrides[current_override].NCR5380_map_name != PORT_AUTO)
	        for(i=0; ports[i]; i++) {
	            if (overrides[current_override].NCR5380_map_name == ports[i])
	                break;
	        }
	    else
	        for(i=0; ports[i]; i++) {
	            if ((!check_region(ports[i], 16)) && (inb(ports[i]) == 0xff))
	                break;
		}
	    if (ports[i]) {
	        outb(0x59, 0x779);
	        outb(0xb9, 0x379);
	        outb(0xc5, 0x379);
	        outb(0xae, 0x379);
	        outb(0xa6, 0x379);
	        outb(0x80 | i, 0x379);          /* set io port to be used */
        	outb(0xc0, ports[i] + 9);
	        if (inb(ports[i] + 9) != 0x80)
	            continue;
	        else
	            overrides[current_override].NCR5380_map_name=ports[i];
	    } else
	        continue;
	}

	request_region(overrides[current_override].NCR5380_map_name,
					NCR5380_region_size, "ncr5380");
#else
	if(check_mem_region(overrides[current_override].NCR5380_map_name,
		NCR5380_region_size))
		continue;
	request_mem_region(overrides[current_override].NCR5380_map_name,
					NCR5380_region_size, "ncr5380");
#endif
	instance = scsi_register (tpnt, sizeof(struct NCR5380_hostdata));
	if(instance == NULL)
	{
#ifdef CONFIG_SCSI_G_NCR5380_PORT
		release_region(overrides[current_override].NCR5380_map_name,
	                                        NCR5380_region_size);
#else
		release_mem_region(overrides[current_override].NCR5380_map_name,
	                                  	NCR5380_region_size);
#endif
	}
	
	instance->NCR5380_instance_name = overrides[current_override].NCR5380_map_name;

	NCR5380_init(instance, flags);

	if (overrides[current_override].irq != IRQ_AUTO)
	    instance->irq = overrides[current_override].irq;
	else 
	    instance->irq = NCR5380_probe_irq(instance, 0xffff);

	if (instance->irq != IRQ_NONE) 
	    if (request_irq(instance->irq, do_generic_NCR5380_intr, SA_INTERRUPT, "NCR5380", NULL)) {
		printk("scsi%d : IRQ%d not free, interrupts disabled\n", 
		    instance->host_no, instance->irq);
		instance->irq = IRQ_NONE;
	    } 

	if (instance->irq == IRQ_NONE) {
	    printk("scsi%d : interrupts not enabled. for better interactive performance,\n", instance->host_no);
	    printk("scsi%d : please jumper the board for a free IRQ.\n", instance->host_no);
	}

	printk("scsi%d : at " STRVAL(NCR5380_map_name) " 0x%x", instance->host_no, (unsigned int)instance->NCR5380_instance_name);
	if (instance->irq == IRQ_NONE)
	    printk (" interrupts disabled");
	else 
	    printk (" irq %d", instance->irq);
	printk(" options CAN_QUEUE=%d  CMD_PER_LUN=%d release=%d",
	    CAN_QUEUE, CMD_PER_LUN, GENERIC_NCR5380_PUBLIC_RELEASE);
	NCR5380_print_options(instance);
	printk("\n");

	++current_override;
	++count;
    }
    return count;
}

const char * generic_NCR5380_info (struct Scsi_Host* host) {
    static const char string[]="Generic NCR5380/53C400 Driver";
    return string;
}

int generic_NCR5380_release_resources(struct Scsi_Host * instance)
{
    NCR5380_local_declare();

    NCR5380_setup(instance);

#ifdef CONFIG_SCSI_G_NCR5380_PORT
    release_region(instance->NCR5380_instance_name, NCR5380_region_size);
#else
    release_mem_region(instance->NCR5380_instance_name, NCR5380_region_size);
#endif    

    if (instance->irq != IRQ_NONE)
	free_irq(instance->irq, NULL);

	return 0;
}

#ifdef BIOSPARAM
/*
 * Function : int generic_NCR5380_biosparam(Disk * disk, kdev_t dev, int *ip)
 *
 * Purpose : Generates a BIOS / DOS compatible H-C-S mapping for 
 *	the specified device / size.
 * 
 * Inputs : size = size of device in sectors (512 bytes), dev = block device
 *	major / minor, ip[] = {heads, sectors, cylinders}  
 *
 * Returns : always 0 (success), initializes ip
 *	
 */

/* 
 * XXX Most SCSI boards use this mapping, I could be incorrect.  Some one
 * using hard disks on a trantor should verify that this mapping corresponds
 * to that used by the BIOS / ASPI driver by running the linux fdisk program
 * and matching the H_C_S coordinates to what DOS uses.
 */

int generic_NCR5380_biosparam(Disk * disk, kdev_t dev, int *ip)
{
  int size = disk->capacity;
  ip[0] = 64;
  ip[1] = 32;
  ip[2] = size >> 11;
  return 0;
}
#endif

#if NCR53C400_PSEUDO_DMA
static inline int NCR5380_pread (struct Scsi_Host *instance, unsigned char *dst,    int len)
{
    int blocks = len / 128;
    int start = 0;
    int bl;
#ifdef CONFIG_SCSI_G_NCR5380_PORT
    int i;
#endif 

    NCR5380_local_declare();

    NCR5380_setup(instance);

#if (NDEBUG & NDEBUG_C400_PREAD)
    printk("53C400r: About to read %d blocks for %d bytes\n", blocks, len);
#endif

    NCR5380_write(C400_CONTROL_STATUS_REG, CSR_BASE | CSR_TRANS_DIR);
    NCR5380_write(C400_BLOCK_COUNTER_REG, blocks);
    while (1) {
    
#if (NDEBUG & NDEBUG_C400_PREAD)
	printk("53C400r: %d blocks left\n", blocks);
#endif

	if ((bl=NCR5380_read(C400_BLOCK_COUNTER_REG)) == 0) {
#if (NDEBUG & NDEBUG_C400_PREAD)
	    if (blocks)
		printk("53C400r: blocks still == %d\n", blocks);
	    else
		printk("53C400r: Exiting loop\n");
#endif
	    break;
	}

#if 1
	if (NCR5380_read(C400_CONTROL_STATUS_REG) & CSR_GATED_53C80_IRQ) {
	    printk("53C400r: Got 53C80_IRQ start=%d, blocks=%d\n", start, blocks);
	    return -1;
	}
#endif

#if (NDEBUG & NDEBUG_C400_PREAD)
	printk("53C400r: Waiting for buffer, bl=%d\n", bl);
#endif

	while (NCR5380_read(C400_CONTROL_STATUS_REG) & CSR_HOST_BUF_NOT_RDY)
	    ;
#if (NDEBUG & NDEBUG_C400_PREAD)
	printk("53C400r: Transferring 128 bytes\n");
#endif

#ifdef CONFIG_SCSI_G_NCR5380_PORT
	for (i=0; i<128; i++)
	    dst[start+i] = NCR5380_read(C400_HOST_BUFFER);
#else
	/* implies CONFIG_SCSI_G_NCR5380_MEM */
	isa_memcpy_fromio(dst+start,NCR53C400_host_buffer+NCR5380_map_name,128);
#endif
	start+=128;
	blocks--;
    }

    if (blocks) {
#if (NDEBUG & NDEBUG_C400_PREAD)
	printk("53C400r: EXTRA: Waiting for buffer\n");
#endif
	while (NCR5380_read(C400_CONTROL_STATUS_REG) & CSR_HOST_BUF_NOT_RDY)
	    ;

#if (NDEBUG & NDEBUG_C400_PREAD)
	printk("53C400r: Transferring EXTRA 128 bytes\n");
#endif
#ifdef CONFIG_SCSI_G_NCR5380_PORT
	for (i=0; i<128; i++)
	    dst[start+i] = NCR5380_read(C400_HOST_BUFFER);
#else
	/* implies CONFIG_SCSI_G_NCR5380_MEM */
	isa_memcpy_fromio(dst+start,NCR53C400_host_buffer+NCR5380_map_name,128);
#endif
	start+=128;
	blocks--;
    }
#if (NDEBUG & NDEBUG_C400_PREAD)
    else
	printk("53C400r: No EXTRA required\n");
#endif

#if (NDEBUG & NDEBUG_C400_PREAD)
    printk("53C400r: Final values: blocks=%d   start=%d\n", blocks, start);
#endif

    if (!(NCR5380_read(C400_CONTROL_STATUS_REG) & CSR_GATED_53C80_IRQ))
	printk("53C400r: no 53C80 gated irq after transfer");
#if (NDEBUG & NDEBUG_C400_PREAD)
    else
	printk("53C400r: Got 53C80 interrupt and tried to clear it\n");
#endif

/* DON'T DO THIS - THEY NEVER ARRIVE!
    printk("53C400r: Waiting for 53C80 registers\n");
    while (NCR5380_read(C400_CONTROL_STATUS_REG) & CSR_53C80_REG)
	;
*/

    if (!(NCR5380_read(BUS_AND_STATUS_REG) & BASR_END_DMA_TRANSFER))
	printk("53C400r: no end dma signal\n");
#if (NDEBUG & NDEBUG_C400_PREAD)
    else
	printk("53C400r: end dma as expected\n");
#endif

    NCR5380_write(MODE_REG, MR_BASE);
    NCR5380_read(RESET_PARITY_INTERRUPT_REG);
    return 0;
}
		
static inline int NCR5380_pwrite (struct Scsi_Host *instance, unsigned char *src,    int len)
{
    int blocks = len / 128;
    int start = 0;
    int i;
    int bl;
    NCR5380_local_declare();

    NCR5380_setup(instance);

#if (NDEBUG & NDEBUG_C400_PWRITE)
    printk("53C400w: About to write %d blocks for %d bytes\n", blocks, len);
#endif

    NCR5380_write(C400_CONTROL_STATUS_REG, CSR_BASE);
    NCR5380_write(C400_BLOCK_COUNTER_REG, blocks);
    while (1) {
	if (NCR5380_read(C400_CONTROL_STATUS_REG) & CSR_GATED_53C80_IRQ) {
	    printk("53C400w: Got 53C80_IRQ start=%d, blocks=%d\n", start, blocks);
	    return -1;
	}

	if ((bl=NCR5380_read(C400_BLOCK_COUNTER_REG)) == 0) {
#if (NDEBUG & NDEBUG_C400_PWRITE)
	    if (blocks)
		printk("53C400w: exiting loop, blocks still == %d\n", blocks);
	    else
		printk("53C400w: exiting loop\n");
#endif
	    break;
	}

#if (NDEBUG & NDEBUG_C400_PWRITE)
	printk("53C400w: %d blocks left\n", blocks);

	printk("53C400w: waiting for buffer, bl=%d\n", bl);
#endif
	while (NCR5380_read(C400_CONTROL_STATUS_REG) & CSR_HOST_BUF_NOT_RDY)
	    ;

#if (NDEBUG & NDEBUG_C400_PWRITE)
	printk("53C400w: transferring 128 bytes\n");
#endif
#ifdef CONFIG_SCSI_G_NCR5380_PORT
	for (i=0; i<128; i++)
	    NCR5380_write(C400_HOST_BUFFER, src[start+i]);
#else
	/* implies CONFIG_SCSI_G_NCR5380_MEM */
	isa_memcpy_toio(NCR53C400_host_buffer+NCR5380_map_name,src+start,128);
#endif
	start+=128;
	blocks--;
    }
    if (blocks) {
#if (NDEBUG & NDEBUG_C400_PWRITE)
	printk("53C400w: EXTRA waiting for buffer\n");
#endif
	while (NCR5380_read(C400_CONTROL_STATUS_REG) & CSR_HOST_BUF_NOT_RDY)
	    ;

#if (NDEBUG & NDEBUG_C400_PWRITE)
	printk("53C400w: transferring EXTRA 128 bytes\n");
#endif
#ifdef CONFIG_SCSI_G_NCR5380_PORT
	for (i=0; i<128; i++)
	    NCR5380_write(C400_HOST_BUFFER, src[start+i]);
#else
	/* implies CONFIG_SCSI_G_NCR5380_MEM */
	isa_memcpy_toio(NCR53C400_host_buffer+NCR5380_map_name,src+start,128);
#endif
	start+=128;
	blocks--;
    }
#if (NDEBUG & NDEBUG_C400_PWRITE)
    else
	printk("53C400w: No EXTRA required\n");
#endif
    
#if (NDEBUG & NDEBUG_C400_PWRITE)
    printk("53C400w: Final values: blocks=%d   start=%d\n", blocks, start);
#endif

#if 0
    printk("53C400w: waiting for registers to be available\n");
    THEY NEVER DO!
    while (NCR5380_read(C400_CONTROL_STATUS_REG) & CSR_53C80_REG)
	;
    printk("53C400w: Got em\n");
#endif

    /* Let's wait for this instead - could be ugly */
    /* All documentation says to check for this. Maybe my hardware is too
     * fast. Waiting for it seems to work fine! KLL
     */
    while (!(i = NCR5380_read(C400_CONTROL_STATUS_REG) & CSR_GATED_53C80_IRQ))
	;

    /*
     * I know. i is certainly != 0 here but the loop is new. See previous
     * comment.
     */
    if (i) {
#if (NDEBUG & NDEBUG_C400_PWRITE)
	printk("53C400w: got 53C80 gated irq (last block)\n");
#endif
	if (!((i=NCR5380_read(BUS_AND_STATUS_REG)) & BASR_END_DMA_TRANSFER))
	    printk("53C400w: No END OF DMA bit - WHOOPS! BASR=%0x\n",i);
#if (NDEBUG & NDEBUG_C400_PWRITE)
	else
	    printk("53C400w: Got END OF DMA\n");
#endif
    }
    else
	printk("53C400w: no 53C80 gated irq after transfer (last block)\n");

#if 0
    if (!(NCR5380_read(BUS_AND_STATUS_REG) & BASR_END_DMA_TRANSFER)) {
	printk("53C400w: no end dma signal\n");
    }
#endif

#if (NDEBUG & NDEBUG_C400_PWRITE)
    printk("53C400w: waiting for last byte...\n");
#endif
    while (!(NCR5380_read(TARGET_COMMAND_REG) & TCR_LAST_BYTE_SENT))
    	;

#if (NDEBUG & NDEBUG_C400_PWRITE)
    printk("53C400w:     got last byte.\n");
    printk("53C400w: pwrite exiting with status 0, whoopee!\n");
#endif
    return 0;
}
#endif /* PSEUDO_DMA */

#include "NCR5380.c"

#define PRINTP(x) len += sprintf(buffer+len, x)
#define ANDP ,

static int sprint_opcode(char* buffer, int len, int opcode) {
    int start = len;
    PRINTP("0x%02x " ANDP opcode);
    return len-start;
}

static int sprint_command (char* buffer, int len, unsigned char *command) {
    int i,s,start=len;
    len += sprint_opcode(buffer, len, command[0]);
    for ( i = 1, s = COMMAND_SIZE(command[0]); i < s; ++i)
	PRINTP("%02x " ANDP command[i]);
    PRINTP("\n");
    return len-start;
}

static int sprint_Scsi_Cmnd (char* buffer, int len, Scsi_Cmnd *cmd) {
    int start = len;
    PRINTP("host number %d destination target %d, lun %d\n" ANDP
       cmd->host->host_no ANDP
       cmd->target ANDP
       cmd->lun);
    PRINTP("        command = ");
    len += sprint_command (buffer, len, cmd->cmnd);
    return len-start;
}

int generic_NCR5380_proc_info(char* buffer, char** start, off_t offset, int length, int hostno, int inout)
{
    int len = 0;
    NCR5380_local_declare();
    unsigned long flags;
    unsigned char status;
    int i;
    struct Scsi_Host *scsi_ptr;
    Scsi_Cmnd *ptr;
    struct NCR5380_hostdata *hostdata;
#ifdef NCR5380_STATS
    Scsi_Device *dev;
    extern const char *const scsi_device_types[MAX_SCSI_DEVICE_CODE];
#endif
    save_flags(flags);
    cli();

    for (scsi_ptr = first_instance; scsi_ptr; scsi_ptr=scsi_ptr->next)
	if (scsi_ptr->host_no == hostno)
	    break;
    NCR5380_setup(scsi_ptr);
    hostdata = (struct NCR5380_hostdata *)scsi_ptr->hostdata;

    PRINTP("SCSI host number %d : %s\n" ANDP scsi_ptr->host_no ANDP scsi_ptr->hostt->name);
    PRINTP("Generic NCR5380 driver version %d\n" ANDP GENERIC_NCR5380_PUBLIC_RELEASE);
    PRINTP("NCR5380 core version %d\n" ANDP NCR5380_PUBLIC_RELEASE);
#ifdef NCR53C400
    PRINTP("NCR53C400 extension version %d\n" ANDP NCR53C400_PUBLIC_RELEASE);
    PRINTP("NCR53C400 card%s detected\n" ANDP  (((struct NCR5380_hostdata *)scsi_ptr->hostdata)->flags & FLAG_NCR53C400)?"":" not");
# if NCR53C400_PSEUDO_DMA
    PRINTP("NCR53C400 pseudo DMA used\n");
# endif
#else
    PRINTP("NO NCR53C400 driver extensions\n");
#endif
    PRINTP("Using %s mapping at %s 0x%lx, " ANDP STRVAL(NCR5380_map_config) ANDP STRVAL(NCR5380_map_name) ANDP scsi_ptr->NCR5380_instance_name);
    if (scsi_ptr->irq == IRQ_NONE)
	PRINTP("no interrupt\n");
    else
	PRINTP("on interrupt %d\n" ANDP scsi_ptr->irq);

#ifdef NCR5380_STATS
    if (hostdata->connected || hostdata->issue_queue || hostdata->disconnected_queue)
	PRINTP("There are commands pending, transfer rates may be crud\n");
    if (hostdata->pendingr)
	PRINTP("  %d pending reads" ANDP hostdata->pendingr);
    if (hostdata->pendingw)
	PRINTP("  %d pending writes" ANDP hostdata->pendingw);
    if (hostdata->pendingr || hostdata->pendingw)
	PRINTP("\n");
    for (dev = scsi_ptr->host_queue; dev; dev=dev->next) {
	    unsigned long br = hostdata->bytes_read[dev->id];
	    unsigned long bw = hostdata->bytes_write[dev->id];
	    long tr = hostdata->time_read[dev->id] / HZ;
	    long tw = hostdata->time_write[dev->id] / HZ;

	    PRINTP("  T:%d %s " ANDP dev->id ANDP (dev->type < MAX_SCSI_DEVICE_CODE) ? scsi_device_types[(int)dev->type] : "Unknown");
	    for (i=0; i<8; i++)
		if (dev->vendor[i] >= 0x20)
		    *(buffer+(len++)) = dev->vendor[i];
	    *(buffer+(len++)) = ' ';
	    for (i=0; i<16; i++)
		if (dev->model[i] >= 0x20)
		    *(buffer+(len++)) = dev->model[i];
	    *(buffer+(len++)) = ' ';
	    for (i=0; i<4; i++)
		if (dev->rev[i] >= 0x20)
		    *(buffer+(len++)) = dev->rev[i];
	    *(buffer+(len++)) = ' ';
				    
	    PRINTP("\n%10ld kb read    in %5ld secs" ANDP br/1024 ANDP tr);
	    if (tr)
		PRINTP(" @ %5ld bps" ANDP br / tr); 

	    PRINTP("\n%10ld kb written in %5ld secs" ANDP bw/1024 ANDP tw);
	    if (tw)
		PRINTP(" @ %5ld bps" ANDP bw / tw); 
	    PRINTP("\n");
    }
#endif
	
    status = NCR5380_read(STATUS_REG);
    if (!(status & SR_REQ))
	PRINTP("REQ not asserted, phase unknown.\n");
    else {
	for (i = 0; (phases[i].value != PHASE_UNKNOWN) &&
		    (phases[i].value != (status & PHASE_MASK)); ++i)
	    ;
	PRINTP("Phase %s\n" ANDP phases[i].name);
    }

    if (!hostdata->connected) {
	PRINTP("No currently connected command\n");
    } else {
	len += sprint_Scsi_Cmnd (buffer, len, (Scsi_Cmnd *) hostdata->connected);
    }

    PRINTP("issue_queue\n");

    for (ptr = (Scsi_Cmnd *) hostdata->issue_queue; ptr;
		ptr = (Scsi_Cmnd *) ptr->host_scribble)
	len += sprint_Scsi_Cmnd (buffer, len, ptr);

    PRINTP("disconnected_queue\n");

    for (ptr = (Scsi_Cmnd *) hostdata->disconnected_queue; ptr;
		ptr = (Scsi_Cmnd *) ptr->host_scribble)
	len += sprint_Scsi_Cmnd (buffer, len, ptr);
	
    *start = buffer + offset;
    len -= offset;
    if (len > length)
	    len = length;
    restore_flags(flags);
    return len;
}

#undef PRINTP
#undef ANDP

/* Eventually this will go into an include file, but this will be later */
static Scsi_Host_Template driver_template = GENERIC_NCR5380;

#include <linux/module.h>
#include "scsi_module.c"

#ifdef MODULE 

MODULE_PARM(ncr_irq, "i");
MODULE_PARM(ncr_dma, "i");
MODULE_PARM(ncr_addr, "i");
MODULE_PARM(ncr_5380, "i");
MODULE_PARM(ncr_53c400, "i");
MODULE_PARM(ncr_53c400a, "i");
MODULE_PARM(dtc_3181e, "i");

#endif

