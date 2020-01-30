#ifndef AEC62XX_H
#define AEC62XX_H

#include <linux/config.h>
#include <linux/pci.h>
#include <linux/ide.h>

#define DISPLAY_AEC62XX_TIMINGS

struct chipset_bus_clock_list_entry {
	byte		xfer_speed;
	byte		chipset_settings;
	byte		ultra_settings;
};

struct chipset_bus_clock_list_entry aec6xxx_33_base [] = {
	{	XFER_UDMA_6,	0x31,	0x07	},
	{	XFER_UDMA_5,	0x31,	0x06	},
	{	XFER_UDMA_4,	0x31,	0x05	},
	{	XFER_UDMA_3,	0x31,	0x04	},
	{	XFER_UDMA_2,	0x31,	0x03	},
	{	XFER_UDMA_1,	0x31,	0x02	},
	{	XFER_UDMA_0,	0x31,	0x01	},

	{	XFER_MW_DMA_2,	0x31,	0x00	},
	{	XFER_MW_DMA_1,	0x31,	0x00	},
	{	XFER_MW_DMA_0,	0x0a,	0x00	},
	{	XFER_PIO_4,	0x31,	0x00	},
	{	XFER_PIO_3,	0x33,	0x00	},
	{	XFER_PIO_2,	0x08,	0x00	},
	{	XFER_PIO_1,	0x0a,	0x00	},
	{	XFER_PIO_0,	0x00,	0x00	},
	{	0,		0x00,	0x00	}
};

struct chipset_bus_clock_list_entry aec6xxx_34_base [] = {
	{	XFER_UDMA_6,	0x41,	0x06	},
	{	XFER_UDMA_5,	0x41,	0x05	},
	{	XFER_UDMA_4,	0x41,	0x04	},
	{	XFER_UDMA_3,	0x41,	0x03	},
	{	XFER_UDMA_2,	0x41,	0x02	},
	{	XFER_UDMA_1,	0x41,	0x01	},
	{	XFER_UDMA_0,	0x41,	0x01	},

	{	XFER_MW_DMA_2,	0x41,	0x00	},
	{	XFER_MW_DMA_1,	0x42,	0x00	},
	{	XFER_MW_DMA_0,	0x7a,	0x00	},
	{	XFER_PIO_4,	0x41,	0x00	},
	{	XFER_PIO_3,	0x43,	0x00	},
	{	XFER_PIO_2,	0x78,	0x00	},
	{	XFER_PIO_1,	0x7a,	0x00	},
	{	XFER_PIO_0,	0x70,	0x00	},
	{	0,		0x00,	0x00	}
};


#ifndef HIGH_4
#define HIGH_4(H)		((H)=(H>>4))
#endif
#ifndef LOW_4
#define LOW_4(L)		((L)=(L-((L>>4)<<4)))
#endif
#ifndef SPLIT_BYTE
#define SPLIT_BYTE(B,H,L)	((H)=(B>>4), (L)=(B-((B>>4)<<4)))
#endif
#ifndef MAKE_WORD
#define MAKE_WORD(W,HB,LB)	((W)=((HB<<8)+LB))
#endif

#define BUSCLOCK(D)	\
	((struct chipset_bus_clock_list_entry *) pci_get_drvdata((D)))

static void init_setup_aec6x80(struct pci_dev *, ide_pci_device_t *);
static void init_setup_aec62xx(struct pci_dev *, ide_pci_device_t *);
static unsigned int init_chipset_aec62xx(struct pci_dev *, const char *);
static void init_hwif_aec62xx(ide_hwif_t *);
static void init_dma_aec62xx(ide_hwif_t *, unsigned long);

static ide_pci_device_t aec62xx_chipsets[] __devinitdata = {
	{	/* 0 */
		.name		= "AEC6210",
		.init_setup	= init_setup_aec62xx,
		.init_chipset	= init_chipset_aec62xx,
		.init_hwif	= init_hwif_aec62xx,
		.init_dma	= init_dma_aec62xx,
		.channels	= 2,
		.autodma	= AUTODMA,
		.enablebits	= {{0x4a,0x02,0x02}, {0x4a,0x04,0x04}},
		.bootable	= OFF_BOARD,
	},{	/* 1 */
		.name		= "AEC6260",
		.init_setup	= init_setup_aec62xx,
		.init_chipset	= init_chipset_aec62xx,
		.init_hwif	= init_hwif_aec62xx,
		.init_dma	= init_dma_aec62xx,
		.channels	= 2,
		.autodma	= NOAUTODMA,
		.bootable	= OFF_BOARD,
	},{	/* 2 */
		.name		= "AEC6260R",
		.init_setup	= init_setup_aec62xx,
		.init_chipset	= init_chipset_aec62xx,
		.init_hwif	= init_hwif_aec62xx,
		.init_dma	= init_dma_aec62xx,
		.channels	= 2,
		.autodma	= AUTODMA,
		.enablebits	= {{0x4a,0x02,0x02}, {0x4a,0x04,0x04}},
		.bootable	= NEVER_BOARD,
	},{	/* 3 */
		.name		= "AEC6X80",
		.init_setup	= init_setup_aec6x80,
		.init_chipset	= init_chipset_aec62xx,
		.init_hwif	= init_hwif_aec62xx,
		.init_dma	= init_dma_aec62xx,
		.channels	= 2,
		.autodma	= AUTODMA,
		.bootable	= OFF_BOARD,
	},{	/* 4 */
		.name		= "AEC6X80R",
		.init_setup	= init_setup_aec6x80,
		.init_chipset	= init_chipset_aec62xx,
		.init_hwif	= init_hwif_aec62xx,
		.init_dma	= init_dma_aec62xx,
		.channels	= 2,
		.autodma	= AUTODMA,
		.enablebits	= {{0x4a,0x02,0x02}, {0x4a,0x04,0x04}},
		.bootable	= OFF_BOARD,
	}
};

#endif /* AEC62XX_H */
