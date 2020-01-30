/*
 *  linux/drivers/ide/pci/piix.c	Version 0.44	March 20, 2003
 *
 *  Copyright (C) 1998-1999 Andrzej Krzysztofowicz, Author and Maintainer
 *  Copyright (C) 1998-2000 Andre Hedrick <andre@linux-ide.org>
 *  Copyright (C) 2003 Red Hat Inc <alan@redhat.com>
 *
 *  May be copied or modified under the terms of the GNU General Public License
 *
 *  PIO mode setting function for Intel chipsets.  
 *  For use instead of BIOS settings.
 *
 * 40-41
 * 42-43
 * 
 *                 41
 *                 43
 *
 * | PIO 0       | c0 | 80 | 0 | 	piix_tune_drive(drive, 0);
 * | PIO 2 | SW2 | d0 | 90 | 4 | 	piix_tune_drive(drive, 2);
 * | PIO 3 | MW1 | e1 | a1 | 9 | 	piix_tune_drive(drive, 3);
 * | PIO 4 | MW2 | e3 | a3 | b | 	piix_tune_drive(drive, 4);
 * 
 * sitre = word40 & 0x4000; primary
 * sitre = word42 & 0x4000; secondary
 *
 * 44 8421|8421    hdd|hdb
 * 
 * 48 8421         hdd|hdc|hdb|hda udma enabled
 *
 *    0001         hda
 *    0010         hdb
 *    0100         hdc
 *    1000         hdd
 *
 * 4a 84|21        hdb|hda
 * 4b 84|21        hdd|hdc
 *
 *    ata-33/82371AB
 *    ata-33/82371EB
 *    ata-33/82801AB            ata-66/82801AA
 *    00|00 udma 0              00|00 reserved
 *    01|01 udma 1              01|01 udma 3
 *    10|10 udma 2              10|10 udma 4
 *    11|11 reserved            11|11 reserved
 *
 * 54 8421|8421    ata66 drive|ata66 enable
 *
 * pci_read_config_word(HWIF(drive)->pci_dev, 0x40, &reg40);
 * pci_read_config_word(HWIF(drive)->pci_dev, 0x42, &reg42);
 * pci_read_config_word(HWIF(drive)->pci_dev, 0x44, &reg44);
 * pci_read_config_byte(HWIF(drive)->pci_dev, 0x48, &reg48);
 * pci_read_config_word(HWIF(drive)->pci_dev, 0x4a, &reg4a);
 * pci_read_config_byte(HWIF(drive)->pci_dev, 0x54, &reg54);
 *
 * Documentation
 *	Publically available from Intel web site. Errata documentation
 * is also publically available. As an aide to anyone hacking on this
 * driver the list of errata that are relevant is below.going back to
 * PIIX4. Older device documentation is now a bit tricky to find.
 *
 * Errata of note:
 *
 * Unfixable
 *	PIIX4    errata #9	- Only on ultra obscure hw
 *	ICH3	 errata #13     - Not observed to affect real hw
 *				  by Intel
 *
 * Things we must deal with
 *	PIIX4	errata #10	- BM IDE hang with non UDMA
 *				  (must stop/start dma to recover)
 *	440MX   errata #15	- As PIIX4 errata #10
 *	PIIX4	errata #15	- Must not read control registers
 * 				  during a PIO transfer
 *	440MX   errata #13	- As PIIX4 errata #15
 *	ICH2	errata #21	- DMA mode 0 doesn't work right
 *	ICH0/1  errata #55	- As ICH2 errata #21
 *	ICH2	spec c #9	- Extra operations needed to handle
 *				  drive hotswap [NOT YET SUPPORTED]
 *	ICH2    spec c #20	- IDE PRD must not cross a 64K boundary
 *				  and must be dword aligned
 *	ICH2    spec c #24	- UDMA mode 4,5 t85/86 should be 6ns not 3.3
 *
 * Should have been BIOS fixed:
 *	450NX:	errata #19	- DMA hangs on old 450NX
 *	450NX:  errata #20	- DMA hangs on old 450NX
 *	450NX:  errata #25	- Corruption with DMA on old 450NX
 *	ICH3    errata #15      - IDE deadlock under high load
 *				  (BIOS must set dev 31 fn 0 bit 23)
 *	ICH3	errata #18	- Don't use native mode
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/pci.h>
#include <linux/hdreg.h>
#include <linux/ide.h>
#include <linux/delay.h>
#include <linux/init.h>

#include <asm/io.h>

#include "piix.h"

static int no_piix_dma;
#if defined(DISPLAY_PIIX_TIMINGS) && defined(CONFIG_PROC_FS)
#include <linux/stat.h>
#include <linux/proc_fs.h>

static u8 piix_proc = 0;
#define PIIX_MAX_DEVS		5
static struct pci_dev *piix_devs[PIIX_MAX_DEVS];
static int n_piix_devs;

/**
 *	piix_get_info		-	fill in /proc for PIIX ide
 *	@buffer: buffer to fill
 *	@addr: address of user start in buffer
 *	@offset: offset into 'file'
 *	@count: buffer count
 *
 *	Walks the PIIX devices and outputs summary data on the tuning and
 *	anything else that will help with debugging
 */
 
static int piix_get_info (char *buffer, char **addr, off_t offset, int count)
{
	char *p = buffer;
	int i;

	for (i = 0; i < n_piix_devs; i++) {
		struct pci_dev *dev	= piix_devs[i];
		unsigned long bibma = pci_resource_start(dev, 4);
	        u16 reg40 = 0, psitre = 0, reg42 = 0, ssitre = 0;
		u8  c0 = 0, c1 = 0, reg54 = 0, reg55 = 0;
		u8  reg44 = 0, reg48 = 0, reg4a = 0, reg4b = 0;

		p += sprintf(p, "\nController: %d\n", i);
		p += sprintf(p, "\n                                Intel ");
		switch(dev->device) {
			case PCI_DEVICE_ID_INTEL_82801EB_1:
				p += sprintf(p, "PIIX4 SATA 150 ");
				break;
			case PCI_DEVICE_ID_INTEL_82801BA_8:
			case PCI_DEVICE_ID_INTEL_82801BA_9:
			case PCI_DEVICE_ID_INTEL_82801CA_10:
			case PCI_DEVICE_ID_INTEL_82801CA_11:
			case PCI_DEVICE_ID_INTEL_82801DB_10:
			case PCI_DEVICE_ID_INTEL_82801DB_11:
			case PCI_DEVICE_ID_INTEL_82801EB_11:
			case PCI_DEVICE_ID_INTEL_82801E_11:
			case PCI_DEVICE_ID_INTEL_ESB_2:
			case PCI_DEVICE_ID_INTEL_ICH6_19:
				p += sprintf(p, "PIIX4 Ultra 100 ");
				break;
			case PCI_DEVICE_ID_INTEL_82372FB_1:
			case PCI_DEVICE_ID_INTEL_82801AA_1:
				p += sprintf(p, "PIIX4 Ultra 66 ");
				break;
			case PCI_DEVICE_ID_INTEL_82451NX:
			case PCI_DEVICE_ID_INTEL_82801AB_1:
			case PCI_DEVICE_ID_INTEL_82443MX_1:
			case PCI_DEVICE_ID_INTEL_82371AB:
				p += sprintf(p, "PIIX4 Ultra 33 ");
				break;
			case PCI_DEVICE_ID_INTEL_82371SB_1:
				p += sprintf(p, "PIIX3 ");
				break;
			case PCI_DEVICE_ID_INTEL_82371MX:
				p += sprintf(p, "MPIIX ");
				break;
			case PCI_DEVICE_ID_INTEL_82371FB_1:
			case PCI_DEVICE_ID_INTEL_82371FB_0:
			default:
				p += sprintf(p, "PIIX ");
				break;
		}
		p += sprintf(p, "Chipset.\n");

		if (dev->device == PCI_DEVICE_ID_INTEL_82371MX)
			continue;

		pci_read_config_word(dev, 0x40, &reg40);
		pci_read_config_word(dev, 0x42, &reg42);
		pci_read_config_byte(dev, 0x44, &reg44);
		pci_read_config_byte(dev, 0x48, &reg48);
		pci_read_config_byte(dev, 0x4a, &reg4a);
		pci_read_config_byte(dev, 0x4b, &reg4b);
		pci_read_config_byte(dev, 0x54, &reg54);
		pci_read_config_byte(dev, 0x55, &reg55);

		psitre = (reg40 & 0x4000) ? 1 : 0;
		ssitre = (reg42 & 0x4000) ? 1 : 0;

		/*
		 * at that point bibma+0x2 et bibma+0xa are byte registers
		 * to investigate:
		 */
		c0 = inb(bibma + 0x02);
		c1 = inb(bibma + 0x0a);

		p += sprintf(p, "--------------- Primary Channel "
				"---------------- Secondary Channel "
				"-------------\n");
		p += sprintf(p, "                %sabled "
				"                        %sabled\n",
				(c0&0x80) ? "dis" : " en",
				(c1&0x80) ? "dis" : " en");
		p += sprintf(p, "--------------- drive0 --------- drive1 "
				"-------- drive0 ---------- drive1 ------\n");
		p += sprintf(p, "DMA enabled:    %s              %s "
				"            %s               %s\n",
				(c0&0x20) ? "yes" : "no ",
				(c0&0x40) ? "yes" : "no ",
				(c1&0x20) ? "yes" : "no ",
				(c1&0x40) ? "yes" : "no " );
		p += sprintf(p, "UDMA enabled:   %s              %s "
				"            %s               %s\n",
				(reg48&0x01) ? "yes" : "no ",
				(reg48&0x02) ? "yes" : "no ",
				(reg48&0x04) ? "yes" : "no ",
				(reg48&0x08) ? "yes" : "no " );
		p += sprintf(p, "UDMA enabled:   %s                %s "
				"              %s                 %s\n",
				((reg54&0x11) &&
				 (reg55&0x10) && (reg4a&0x01)) ? "5" :
				((reg54&0x11) && (reg4a&0x02)) ? "4" :
				((reg54&0x11) && (reg4a&0x01)) ? "3" :
				(reg4a&0x02) ? "2" :
				(reg4a&0x01) ? "1" :
				(reg4a&0x00) ? "0" : "X",
				((reg54&0x22) &&
				 (reg55&0x20) && (reg4a&0x10)) ? "5" :
				((reg54&0x22) && (reg4a&0x20)) ? "4" :
				((reg54&0x22) && (reg4a&0x10)) ? "3" :
				(reg4a&0x20) ? "2" :
				(reg4a&0x10) ? "1" :
				(reg4a&0x00) ? "0" : "X",
				((reg54&0x44) &&
				 (reg55&0x40) && (reg4b&0x03)) ? "5" :
				((reg54&0x44) && (reg4b&0x02)) ? "4" :
				((reg54&0x44) && (reg4b&0x01)) ? "3" :
				(reg4b&0x02) ? "2" :
				(reg4b&0x01) ? "1" :
				(reg4b&0x00) ? "0" : "X",
				((reg54&0x88) &&
				 (reg55&0x80) && (reg4b&0x30)) ? "5" :
				((reg54&0x88) && (reg4b&0x20)) ? "4" :
				((reg54&0x88) && (reg4b&0x10)) ? "3" :
				(reg4b&0x20) ? "2" :
				(reg4b&0x10) ? "1" :
				(reg4b&0x00) ? "0" : "X");

		p += sprintf(p, "UDMA\n");
		p += sprintf(p, "DMA\n");
		p += sprintf(p, "PIO\n");

		/*
		 * FIXME.... Add configuration junk data....blah blah......
		 */
	}
	return p-buffer;	 /* => must be less than 4k! */
}
#endif  /* defined(DISPLAY_PIIX_TIMINGS) && defined(CONFIG_PROC_FS) */

/**
 *	piix_ratemask		-	compute rate mask for PIIX IDE
 *	@drive: IDE drive to compute for
 *
 *	Returns the available modes for the PIIX IDE controller.
 */
 
static u8 piix_ratemask (ide_drive_t *drive)
{
	struct pci_dev *dev	= HWIF(drive)->pci_dev;
	u8 mode;

	switch(dev->device) {
		case PCI_DEVICE_ID_INTEL_82801EB_1:
			mode = 3;
			break;
		/* UDMA 100 capable */
		case PCI_DEVICE_ID_INTEL_82801BA_8:
		case PCI_DEVICE_ID_INTEL_82801BA_9:
		case PCI_DEVICE_ID_INTEL_82801CA_10:
		case PCI_DEVICE_ID_INTEL_82801CA_11:
		case PCI_DEVICE_ID_INTEL_82801E_11:
		case PCI_DEVICE_ID_INTEL_82801DB_10:
		case PCI_DEVICE_ID_INTEL_82801DB_11:
		case PCI_DEVICE_ID_INTEL_82801EB_11:
		case PCI_DEVICE_ID_INTEL_ESB_2:
		case PCI_DEVICE_ID_INTEL_ICH6_19:
			mode = 3;
			break;
		/* UDMA 66 capable */
		case PCI_DEVICE_ID_INTEL_82801AA_1:
		case PCI_DEVICE_ID_INTEL_82372FB_1:
			mode = 2;
			break;
		/* UDMA 33 capable */
		case PCI_DEVICE_ID_INTEL_82371AB:
		case PCI_DEVICE_ID_INTEL_82443MX_1:
		case PCI_DEVICE_ID_INTEL_82451NX:
		case PCI_DEVICE_ID_INTEL_82801AB_1:
			return 1;
		/* Non UDMA capable (MWDMA2) */
		case PCI_DEVICE_ID_INTEL_82371SB_1:
		case PCI_DEVICE_ID_INTEL_82371FB_1:
		case PCI_DEVICE_ID_INTEL_82371FB_0:
		case PCI_DEVICE_ID_INTEL_82371MX:
		default:
			return 0;
	}
	
	/*
	 *	If we are UDMA66 capable fall back to UDMA33 
	 *	if the drive cannot see an 80pin cable.
	 */
	if (!eighty_ninty_three(drive))
		mode = min(mode, (u8)1);
	return mode;
}

/**
 *	piix_dma_2_pio		-	return the PIO mode matching DMA
 *	@xfer_rate: transfer speed
 *
 *	Returns the nearest equivalent PIO timing for the PIO or DMA
 *	mode requested by the controller.
 */
 
static u8 piix_dma_2_pio (u8 xfer_rate) {
	switch(xfer_rate) {
		case XFER_UDMA_6:
		case XFER_UDMA_5:
		case XFER_UDMA_4:
		case XFER_UDMA_3:
		case XFER_UDMA_2:
		case XFER_UDMA_1:
		case XFER_UDMA_0:
		case XFER_MW_DMA_2:
		case XFER_PIO_4:
			return 4;
		case XFER_MW_DMA_1:
		case XFER_PIO_3:
			return 3;
		case XFER_SW_DMA_2:
		case XFER_PIO_2:
			return 2;
		case XFER_MW_DMA_0:
		case XFER_SW_DMA_1:
		case XFER_SW_DMA_0:
		case XFER_PIO_1:
		case XFER_PIO_0:
		case XFER_PIO_SLOW:
		default:
			return 0;
	}
}

/**
 *	piix_tune_drive		-	tune a drive attached to a PIIX
 *	@drive: drive to tune
 *	@pio: desired PIO mode
 *
 *	Set the interface PIO mode based upon  the settings done by AMI BIOS
 *	(might be useful if drive is not registered in CMOS for any reason).
 */
static void piix_tune_drive (ide_drive_t *drive, u8 pio)
{
	ide_hwif_t *hwif	= HWIF(drive);
	struct pci_dev *dev	= hwif->pci_dev;
	int is_slave		= (&hwif->drives[1] == drive);
	int master_port		= hwif->channel ? 0x42 : 0x40;
	int slave_port		= 0x44;
	unsigned long flags;
	u16 master_data;
	u8 slave_data;
				 /* ISP  RTC */
	u8 timings[][2]	= { { 0, 0 },
			    { 0, 0 },
			    { 1, 0 },
			    { 2, 1 },
			    { 2, 3 }, };

	pio = ide_get_best_pio_mode(drive, pio, 5, NULL);
	spin_lock_irqsave(&ide_lock, flags);
	pci_read_config_word(dev, master_port, &master_data);
	if (is_slave) {
		master_data = master_data | 0x4000;
		if (pio > 1)
			/* enable PPE, IE and TIME */
			master_data = master_data | 0x0070;
		pci_read_config_byte(dev, slave_port, &slave_data);
		slave_data = slave_data & (hwif->channel ? 0x0f : 0xf0);
		slave_data = slave_data | (((timings[pio][0] << 2) | timings[pio][1]) << (hwif->channel ? 4 : 0));
	} else {
		master_data = master_data & 0xccf8;
		if (pio > 1)
			/* enable PPE, IE and TIME */
			master_data = master_data | 0x0007;
		master_data = master_data | (timings[pio][0] << 12) | (timings[pio][1] << 8);
	}
	pci_write_config_word(dev, master_port, master_data);
	if (is_slave)
		pci_write_config_byte(dev, slave_port, slave_data);
	spin_unlock_irqrestore(&ide_lock, flags);
}

/**
 *	piix_tune_chipset	-	tune a PIIX interface
 *	@drive: IDE drive to tune
 *	@xferspeed: speed to configure
 *
 *	Set a PIIX interface channel to the desired speeds. This involves
 *	requires the right timing data into the PIIX configuration space
 *	then setting the drive parameters appropriately
 */
 
static int piix_tune_chipset (ide_drive_t *drive, u8 xferspeed)
{
	ide_hwif_t *hwif	= HWIF(drive);
	struct pci_dev *dev	= hwif->pci_dev;
	u8 maslave		= hwif->channel ? 0x42 : 0x40;
	u8 speed		= ide_rate_filter(piix_ratemask(drive), xferspeed);
	int a_speed		= 3 << (drive->dn * 4);
	int u_flag		= 1 << drive->dn;
	int v_flag		= 0x01 << drive->dn;
	int w_flag		= 0x10 << drive->dn;
	int u_speed		= 0;
	int			sitre;
	u16			reg4042, reg4a;
	u8			reg48, reg54, reg55;

	pci_read_config_word(dev, maslave, &reg4042);
	sitre = (reg4042 & 0x4000) ? 1 : 0;
	pci_read_config_byte(dev, 0x48, &reg48);
	pci_read_config_word(dev, 0x4a, &reg4a);
	pci_read_config_byte(dev, 0x54, &reg54);
	pci_read_config_byte(dev, 0x55, &reg55);

	switch(speed) {
		case XFER_UDMA_4:
		case XFER_UDMA_2:	u_speed = 2 << (drive->dn * 4); break;
		case XFER_UDMA_5:
		case XFER_UDMA_3:
		case XFER_UDMA_1:	u_speed = 1 << (drive->dn * 4); break;
		case XFER_UDMA_0:	u_speed = 0 << (drive->dn * 4); break;
		case XFER_MW_DMA_2:
		case XFER_MW_DMA_1:
		case XFER_SW_DMA_2:	break;
		case XFER_PIO_4:
		case XFER_PIO_3:
		case XFER_PIO_2:
		case XFER_PIO_0:	break;
		default:		return -1;
	}

	if (speed >= XFER_UDMA_0) {
		if (!(reg48 & u_flag))
			pci_write_config_byte(dev, 0x48, reg48 | u_flag);
		if (speed == XFER_UDMA_5) {
			pci_write_config_byte(dev, 0x55, (u8) reg55|w_flag);
		} else {
			pci_write_config_byte(dev, 0x55, (u8) reg55 & ~w_flag);
		}
		if ((reg4a & a_speed) != u_speed)
			pci_write_config_word(dev, 0x4a, (reg4a & ~a_speed) | u_speed);
		if (speed > XFER_UDMA_2) {
			if (!(reg54 & v_flag))
				pci_write_config_byte(dev, 0x54, reg54 | v_flag);
		} else
			pci_write_config_byte(dev, 0x54, reg54 & ~v_flag);
	} else {
		if (reg48 & u_flag)
			pci_write_config_byte(dev, 0x48, reg48 & ~u_flag);
		if (reg4a & a_speed)
			pci_write_config_word(dev, 0x4a, reg4a & ~a_speed);
		if (reg54 & v_flag)
			pci_write_config_byte(dev, 0x54, reg54 & ~v_flag);
		if (reg55 & w_flag)
			pci_write_config_byte(dev, 0x55, (u8) reg55 & ~w_flag);
	}

	piix_tune_drive(drive, piix_dma_2_pio(speed));
	return (ide_config_drive_speed(drive, speed));
}

/**
 *	piix_faulty_dma0		-	check for DMA0 errata
 *	@hwif: IDE interface to check
 *
 *	If an ICH/ICH0/ICH2 interface is is operating in multi-word
 *	DMA mode with 600nS cycle time the IDE PIO prefetch buffer will
 *	inadvertently provide an extra piece of secondary data to the primary
 *	device resulting in data corruption.
 *
 *	With such a device this test function returns true. This allows
 *	our tuning code to follow Intel recommendations and use PIO on
 *	such devices.
 */
 
static int piix_faulty_dma0(ide_hwif_t *hwif)
{
	switch(hwif->pci_dev->device)
	{
		case PCI_DEVICE_ID_INTEL_82801AA_1:	/* ICH */
		case PCI_DEVICE_ID_INTEL_82801AB_1:	/* ICH0 */
		case PCI_DEVICE_ID_INTEL_82801BA_8:	/* ICH2 */
		case PCI_DEVICE_ID_INTEL_82801BA_9:	/* ICH2 */
			return 1;
	}
	return 0;
}

/**
 *	piix_config_drive_for_dma	-	configure drive for DMA
 *	@drive: IDE drive to configure
 *
 *	Set up a PIIX interface channel for the best available speed.
 *	We prefer UDMA if it is available and then MWDMA. If DMA is 
 *	not available we switch to PIO and return 0. 
 */
 
static int piix_config_drive_for_dma (ide_drive_t *drive)
{
	u8 speed = ide_dma_speed(drive, piix_ratemask(drive));
	
	/* Some ICH devices cannot support DMA mode 0 */
	if(speed == XFER_MW_DMA_0 && piix_faulty_dma0(HWIF(drive)))
		speed = 0;

	/* If no DMA speed was available or the chipset has DMA bugs
	   then disable DMA and use PIO */
	   
	if (!speed || no_piix_dma) {
		u8 tspeed = ide_get_best_pio_mode(drive, 255, 5, NULL);
		speed = piix_dma_2_pio(XFER_PIO_0 + tspeed);
	}

	(void) piix_tune_chipset(drive, speed);
	return ide_dma_enable(drive);
}

/**
 *	piix_config_drive_xfer_rate	-	set up an IDE device
 *	@drive: IDE drive to configure
 *
 *	Set up the PIIX interface for the best available speed on this
 *	interface, preferring DMA to PIO.
 */
 
static int piix_config_drive_xfer_rate (ide_drive_t *drive)
{
	ide_hwif_t *hwif	= HWIF(drive);
	struct hd_driveid *id	= drive->id;

	drive->init_speed = 0;

	if ((id->capability & 1) && drive->autodma) {
		/* Consult the list of known "bad" drives */
		if (__ide_dma_bad_drive(drive))
			goto fast_ata_pio;
		if (id->field_valid & 4) {
			if (id->dma_ultra & hwif->ultra_mask) {
				/* Force if Capable UltraDMA */
				if ((id->field_valid & 2) &&
				    (!piix_config_drive_for_dma(drive)))
					goto try_dma_modes;
			}
		} else if (id->field_valid & 2) {
try_dma_modes:
			if ((id->dma_mword & hwif->mwdma_mask) ||
			    (id->dma_1word & hwif->swdma_mask)) {
				/* Force if Capable regular DMA modes */
				if (!piix_config_drive_for_dma(drive))
					goto no_dma_set;
			}
		} else if (__ide_dma_good_drive(drive) &&
			   (id->eide_dma_time < 150)) {
			/* Consult the list of known "good" drives */
			if (!piix_config_drive_for_dma(drive))
				goto no_dma_set;
		} else {
			goto fast_ata_pio;
		}
		return hwif->ide_dma_on(drive);
	} else if ((id->capability & 8) || (id->field_valid & 2)) {
fast_ata_pio:
no_dma_set:
		hwif->tuneproc(drive, 255);
		return hwif->ide_dma_off_quietly(drive);
	}
	/* IORDY not supported */
	return 0;
}

/**
 *	init_chipset_piix	-	set up the PIIX chipset
 *	@dev: PCI device to set up
 *	@name: Name of the device
 *
 *	Initialize the PCI device as required. For the PIIX this turns
 *	out to be nice and simple
 */
 
static unsigned int __devinit init_chipset_piix (struct pci_dev *dev, const char *name)
{
        switch(dev->device) {
		case PCI_DEVICE_ID_INTEL_82801EB_1:
		case PCI_DEVICE_ID_INTEL_82801AA_1:
		case PCI_DEVICE_ID_INTEL_82801AB_1:
		case PCI_DEVICE_ID_INTEL_82801BA_8:
		case PCI_DEVICE_ID_INTEL_82801BA_9:
		case PCI_DEVICE_ID_INTEL_82801CA_10:
		case PCI_DEVICE_ID_INTEL_82801CA_11:
		case PCI_DEVICE_ID_INTEL_82801DB_10:
		case PCI_DEVICE_ID_INTEL_82801DB_11:
		case PCI_DEVICE_ID_INTEL_82801EB_11:
		case PCI_DEVICE_ID_INTEL_82801E_11:
		case PCI_DEVICE_ID_INTEL_ESB_2:
		case PCI_DEVICE_ID_INTEL_ICH6_19:
		{
			unsigned int extra = 0;
			pci_read_config_dword(dev, 0x54, &extra);
			pci_write_config_dword(dev, 0x54, extra|0x400);
		}
		default:
			break;
	}

#if defined(DISPLAY_PIIX_TIMINGS) && defined(CONFIG_PROC_FS)
	piix_devs[n_piix_devs++] = dev;

	if (!piix_proc) {
		piix_proc = 1;
		ide_pci_create_host_proc("piix", piix_get_info);
	}
#endif /* DISPLAY_PIIX_TIMINGS && CONFIG_PROC_FS */
	return 0;
}

/**
 *	init_hwif_piix		-	fill in the hwif for the PIIX
 *	@hwif: IDE interface
 *
 *	Set up the ide_hwif_t for the PIIX interface according to the
 *	capabilities of the hardware.
 */

static void __devinit init_hwif_piix(ide_hwif_t *hwif)
{
	u8 reg54h = 0, reg55h = 0, ata66 = 0;
	u8 mask = hwif->channel ? 0xc0 : 0x30;

#ifndef CONFIG_IA64
	if (!hwif->irq)
		hwif->irq = hwif->channel ? 15 : 14;
#endif /* CONFIG_IA64 */

	if (hwif->pci_dev->device == PCI_DEVICE_ID_INTEL_82371MX) {
		/* This is a painful system best to let it self tune for now */
		return;
	}

	hwif->autodma = 0;
	hwif->tuneproc = &piix_tune_drive;
	hwif->speedproc = &piix_tune_chipset;
	hwif->drives[0].autotune = 1;
	hwif->drives[1].autotune = 1;

	if (!hwif->dma_base)
		return;

	hwif->atapi_dma = 1;
	hwif->ultra_mask = 0x3f;
	hwif->mwdma_mask = 0x06;
	hwif->swdma_mask = 0x04;

	switch(hwif->pci_dev->device) {
		case PCI_DEVICE_ID_INTEL_82371MX:
			hwif->mwdma_mask = 0x80;
			hwif->swdma_mask = 0x80;
		case PCI_DEVICE_ID_INTEL_82371FB_0:
		case PCI_DEVICE_ID_INTEL_82371FB_1:
		case PCI_DEVICE_ID_INTEL_82371SB_1:
			hwif->ultra_mask = 0x80;
			break;
		case PCI_DEVICE_ID_INTEL_82371AB:
		case PCI_DEVICE_ID_INTEL_82443MX_1:
		case PCI_DEVICE_ID_INTEL_82451NX:
		case PCI_DEVICE_ID_INTEL_82801AB_1:
			hwif->ultra_mask = 0x07;
			break;
		default:
			pci_read_config_byte(hwif->pci_dev, 0x54, &reg54h);
			pci_read_config_byte(hwif->pci_dev, 0x55, &reg55h);
			ata66 = (reg54h & mask) ? 1 : 0;
			break;
	}

	if (!(hwif->udma_four))
		hwif->udma_four = ata66;
	hwif->ide_dma_check = &piix_config_drive_xfer_rate;
	if (!noautodma)
		hwif->autodma = 1;

	hwif->drives[1].autodma = hwif->autodma;
	hwif->drives[0].autodma = hwif->autodma;
}

/**
 *	init_setup_piix		-	callback for IDE initialize
 *	@dev: PIIX PCI device
 *	@d: IDE pci info
 *
 *	Enable the xp fixup for the PIIX controller and then perform
 *	a standard ide PCI setup
 */

static void __devinit init_setup_piix(struct pci_dev *dev, ide_pci_device_t *d)
{
	ide_setup_pci_device(dev, d);
}

/**
 *	piix_init_one	-	called when a PIIX is found
 *	@dev: the piix device
 *	@id: the matching pci id
 *
 *	Called when the PCI registration layer (or the IDE initialization)
 *	finds a device matching our IDE device tables.
 */
 
static int __devinit piix_init_one(struct pci_dev *dev, const struct pci_device_id *id)
{
	ide_pci_device_t *d = &piix_pci_info[id->driver_data];

	d->init_setup(dev, d);
	return 0;
}

/**
 *	piix_check_450nx	-	Check for problem 450NX setup
 *	
 *	Check for the present of 450NX errata #19 and errata #25. If
 *	they are found, disable use of DMA IDE
 */

static void __devinit piix_check_450nx(void)
{
	struct pci_dev *pdev = NULL;
	u16 cfg;
	u8 rev;
	while((pdev=pci_find_device(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82454NX, pdev))!=NULL)
	{
		/* Look for 450NX PXB. Check for problem configurations
		   A PCI quirk checks bit 6 already */
		pci_read_config_byte(pdev, PCI_REVISION_ID, &rev);
		pci_read_config_word(pdev, 0x41, &cfg);
		/* Only on the original revision: IDE DMA can hang */
		if(rev == 0x00)
			no_piix_dma = 1;
		/* On all revisions below 5 PXB bus lock must be disabled for IDE */
		else if(cfg & (1<<14) && rev < 5)
			no_piix_dma = 2;
	}
	if(no_piix_dma)
		printk(KERN_WARNING "piix: 450NX errata present, disabling IDE DMA.\n");
	if(no_piix_dma == 2)
		printk(KERN_WARNING "piix: A BIOS update may resolve this.\n");
}		

static struct pci_device_id piix_pci_tbl[] = {
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82371FB_0, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82371FB_1, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 1},
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82371MX,   PCI_ANY_ID, PCI_ANY_ID, 0, 0, 2},
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82371SB_1, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 3},
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82371AB,   PCI_ANY_ID, PCI_ANY_ID, 0, 0, 4},
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801AB_1, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 5},
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82443MX_1, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 6},
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801AA_1, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 7},
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82372FB_1, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 8},
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82451NX,   PCI_ANY_ID, PCI_ANY_ID, 0, 0, 9},
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801BA_9, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 10},
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801BA_8, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 11},
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801CA_10,PCI_ANY_ID, PCI_ANY_ID, 0, 0, 12},
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801CA_11,PCI_ANY_ID, PCI_ANY_ID, 0, 0, 13},
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801DB_11,PCI_ANY_ID, PCI_ANY_ID, 0, 0, 14},
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801EB_11,PCI_ANY_ID, PCI_ANY_ID, 0, 0, 15},
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801E_11, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 16},
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801DB_10,PCI_ANY_ID, PCI_ANY_ID, 0, 0, 17},
#ifdef CONFIG_BLK_DEV_IDE_SATA
 	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801EB_1, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 18},
#endif
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ESB_2, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 19},
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ICH6_19, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 20},
	{ 0, },
};
MODULE_DEVICE_TABLE(pci, piix_pci_tbl);

static struct pci_driver driver = {
	.name		= "PIIX IDE",
	.id_table	= piix_pci_tbl,
	.probe		= piix_init_one,
};

static int __init piix_ide_init(void)
{
	piix_check_450nx();
	return ide_pci_register_driver(&driver);
}

module_init(piix_ide_init);

MODULE_AUTHOR("Andre Hedrick, Andrzej Krzysztofowicz");
MODULE_DESCRIPTION("PCI driver module for Intel PIIX IDE");
MODULE_LICENSE("GPL");
