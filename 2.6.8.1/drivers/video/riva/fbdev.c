/*
 * linux/drivers/video/riva/fbdev.c - nVidia RIVA 128/TNT/TNT2 fb driver
 *
 * Maintained by Ani Joshi <ajoshi@shell.unixbox.com>
 *
 * Copyright 1999-2000 Jeff Garzik
 *
 * Contributors:
 *
 *	Ani Joshi:  Lots of debugging and cleanup work, really helped
 *	get the driver going
 *
 *	Ferenc Bakonyi:  Bug fixes, cleanup, modularization
 *
 *	Jindrich Makovicka:  Accel code help, hw cursor, mtrr
 *
 *	Paul Richards:  Bug fixes, updates
 *
 * Initial template from skeletonfb.c, created 28 Dec 1997 by Geert Uytterhoeven
 * Includes riva_hw.c from nVidia, see copyright below.
 * KGI code provided the basis for state storage, init, and mode switching.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 * Known bugs and issues:
 *	restoring text mode fails
 *	doublescan modes are broken
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/pci.h>
#ifdef CONFIG_MTRR
#include <asm/mtrr.h>
#endif
#ifdef CONFIG_PPC_OF
#include <asm/prom.h>
#include <asm/pci-bridge.h>
#endif
#ifdef CONFIG_PMAC_BACKLIGHT
#include <asm/backlight.h>
#endif

#include "rivafb.h"
#include "nvreg.h"

#ifndef CONFIG_PCI		/* sanity check */
#error This driver requires PCI support.
#endif

/* version number of this driver */
#define RIVAFB_VERSION "0.9.5b"

/* ------------------------------------------------------------------------- *
 *
 * various helpful macros and constants
 *
 * ------------------------------------------------------------------------- */
#ifdef CONFIG_FB_RIVA_DEBUG
#define NVTRACE          printk
#else
#define NVTRACE          if(0) printk
#endif

#define NVTRACE_ENTER(...)  NVTRACE("%s START\n", __FUNCTION__)
#define NVTRACE_LEAVE(...)  NVTRACE("%s END\n", __FUNCTION__)

#ifdef CONFIG_FB_RIVA_DEBUG
#define assert(expr) \
	if(!(expr)) { \
	printk( "Assertion failed! %s,%s,%s,line=%d\n",\
	#expr,__FILE__,__FUNCTION__,__LINE__); \
	BUG(); \
	}
#else
#define assert(expr)
#endif

#define PFX "rivafb: "

/* macro that allows you to set overflow bits */
#define SetBitField(value,from,to) SetBF(to,GetBF(value,from))
#define SetBit(n)		(1<<(n))
#define Set8Bits(value)		((value)&0xff)

/* HW cursor parameters */
#define MAX_CURS		32

/* ------------------------------------------------------------------------- *
 *
 * prototypes
 *
 * ------------------------------------------------------------------------- */

static int rivafb_blank(int blank, struct fb_info *info);

/* ------------------------------------------------------------------------- *
 *
 * card identification
 *
 * ------------------------------------------------------------------------- */

enum riva_chips {
	CH_RIVA_128 = 0,
	CH_RIVA_TNT,
	CH_RIVA_TNT2,
	CH_RIVA_UTNT2,
	CH_RIVA_VTNT2,
	CH_RIVA_UVTNT2,
	CH_RIVA_ITNT2,
	CH_GEFORCE_SDR,
	CH_GEFORCE_DDR,
	CH_QUADRO,
	CH_GEFORCE2_MX,
	CH_GEFORCE2_MX2,
	CH_GEFORCE2_GO,
	CH_QUADRO2_MXR,
	CH_GEFORCE2_GTS,
	CH_GEFORCE2_GTS2,
	CH_GEFORCE2_ULTRA,
	CH_QUADRO2_PRO,
	CH_GEFORCE4_MX_460,
	CH_GEFORCE4_MX_440,
	CH_GEFORCE4_MX_420,
	CH_GEFORCE4_440_GO,
	CH_GEFORCE4_420_GO,
	CH_GEFORCE4_420_GO_M32,
	CH_QUADRO4_500XGL,
	CH_GEFORCE4_440_GO_M64,
	CH_QUADRO4_200,
	CH_QUADRO4_550XGL,
	CH_QUADRO4_500_GOGL,
	CH_IGEFORCE2,
	CH_GEFORCE3,
	CH_GEFORCE3_1,
	CH_GEFORCE3_2,
	CH_QUADRO_DDC,
	CH_GEFORCE4_TI_4600,
	CH_GEFORCE4_TI_4400,
	CH_GEFORCE4_TI_4200,
	CH_QUADRO4_900XGL,
	CH_QUADRO4_750XGL,
	CH_QUADRO4_700XGL
};

/* directly indexed by riva_chips enum, above */
static struct riva_chip_info {
	const char *name;
	unsigned arch_rev;
} riva_chip_info[] __initdata = {
	{ "RIVA-128", NV_ARCH_03 },
	{ "RIVA-TNT", NV_ARCH_04 },
	{ "RIVA-TNT2", NV_ARCH_04 },
	{ "RIVA-UTNT2", NV_ARCH_04 },
	{ "RIVA-VTNT2", NV_ARCH_04 },
	{ "RIVA-UVTNT2", NV_ARCH_04 },
	{ "RIVA-ITNT2", NV_ARCH_04 },
	{ "GeForce-SDR", NV_ARCH_10 },
	{ "GeForce-DDR", NV_ARCH_10 },
	{ "Quadro", NV_ARCH_10 },
	{ "GeForce2-MX", NV_ARCH_10 },
	{ "GeForce2-MX", NV_ARCH_10 },
	{ "GeForce2-GO", NV_ARCH_10 },
	{ "Quadro2-MXR", NV_ARCH_10 },
	{ "GeForce2-GTS", NV_ARCH_10 },
	{ "GeForce2-GTS", NV_ARCH_10 },
	{ "GeForce2-ULTRA", NV_ARCH_10 },
	{ "Quadro2-PRO", NV_ARCH_10 },
	{ "GeForce4-MX-460", NV_ARCH_10 },
	{ "GeForce4-MX-440", NV_ARCH_10 },
	{ "GeForce4-MX-420", NV_ARCH_10 },
	{ "GeForce4-440-GO", NV_ARCH_10 },
	{ "GeForce4-420-GO", NV_ARCH_10 },
	{ "GeForce4-420-GO-M32", NV_ARCH_10 },
	{ "Quadro4-500-XGL", NV_ARCH_10 },
	{ "GeForce4-440-GO-M64", NV_ARCH_10 },
	{ "Quadro4-200", NV_ARCH_10 },
	{ "Quadro4-550-XGL", NV_ARCH_10 },
	{ "Quadro4-500-GOGL", NV_ARCH_10 },
	{ "GeForce2", NV_ARCH_10 },
	{ "GeForce3", NV_ARCH_20 }, 
	{ "GeForce3 Ti 200", NV_ARCH_20 },
	{ "GeForce3 Ti 500", NV_ARCH_20 },
	{ "Quadro DDC", NV_ARCH_20 },
	{ "GeForce4 Ti 4600", NV_ARCH_20 },
	{ "GeForce4 Ti 4400", NV_ARCH_20 },
	{ "GeForce4 Ti 4200", NV_ARCH_20 },
	{ "Quadro4-900-XGL", NV_ARCH_20 },
	{ "Quadro4-750-XGL", NV_ARCH_20 },
	{ "Quadro4-700-XGL", NV_ARCH_20 }
};

static struct pci_device_id rivafb_pci_tbl[] = {
	{ PCI_VENDOR_ID_NVIDIA_SGS, PCI_DEVICE_ID_NVIDIA_SGS_RIVA128,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_RIVA_128 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_TNT,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_RIVA_TNT },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_TNT2,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_RIVA_TNT2 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_UTNT2,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_RIVA_UTNT2 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_VTNT2,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_RIVA_VTNT2 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_UVTNT2,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_RIVA_VTNT2 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_ITNT2,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_RIVA_ITNT2 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE_SDR,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE_SDR },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE_DDR,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE_DDR },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_QUADRO,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_QUADRO },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE2_MX,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE2_MX },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE2_MX2,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE2_MX2 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE2_GO,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE2_GO },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_QUADRO2_MXR,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_QUADRO2_MXR },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE2_GTS,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE2_GTS },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE2_GTS2,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE2_GTS2 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE2_ULTRA,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE2_ULTRA },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_QUADRO2_PRO,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_QUADRO2_PRO },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE4_MX_460,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE4_MX_460 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE4_MX_440,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE4_MX_440 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE4_MX_420,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE4_MX_420 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE4_440_GO,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE4_440_GO },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE4_420_GO,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE4_420_GO },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE4_420_GO_M32,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE4_420_GO_M32 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_QUADRO4_500XGL,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_QUADRO4_500XGL },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE4_440_GO_M64,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE4_440_GO_M64 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_QUADRO4_200,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_QUADRO4_200 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_QUADRO4_550XGL,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_QUADRO4_550XGL },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_QUADRO4_500_GOGL,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_QUADRO4_500_GOGL },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_IGEFORCE2,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_IGEFORCE2 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE3,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE3 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE3_1,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE3_1 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE3_2,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE3_2 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_QUADRO_DDC,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_QUADRO_DDC },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE4_TI_4600,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE4_TI_4600 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE4_TI_4400,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE4_TI_4400 },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_GEFORCE4_TI_4200,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE4_TI_4200 },
 	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_QUADRO4_900XGL,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_QUADRO4_900XGL },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_QUADRO4_750XGL,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_QUADRO4_750XGL },
	{ PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_QUADRO4_700XGL,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_QUADRO4_700XGL },
	{ 0, } /* terminate list */
};
MODULE_DEVICE_TABLE(pci, rivafb_pci_tbl);

/* ------------------------------------------------------------------------- *
 *
 * global variables
 *
 * ------------------------------------------------------------------------- */

/* command line data, set in rivafb_setup() */
static u32 pseudo_palette[17];
static int flatpanel __initdata = -1; /* Autodetect later */
static int forceCRTC __initdata = -1;
#ifdef CONFIG_MTRR
static int nomtrr __initdata = 0;
#endif

static char *mode_option __initdata = NULL;
static int  strictmode       = 0;

static struct fb_fix_screeninfo rivafb_fix = {
	.id		= "nVidia",
	.type		= FB_TYPE_PACKED_PIXELS,
	.xpanstep	= 1,
	.ypanstep	= 1,
};

static struct fb_var_screeninfo rivafb_default_var = {
	.xres		= 640,
	.yres		= 480,
	.xres_virtual	= 640,
	.yres_virtual	= 480,
	.bits_per_pixel	= 8,
	.red		= {0, 8, 0},
	.green		= {0, 8, 0},
	.blue		= {0, 8, 0},
	.transp		= {0, 0, 0},
	.activate	= FB_ACTIVATE_NOW,
	.height		= -1,
	.width		= -1,
	.accel_flags	= FB_ACCELF_TEXT,
	.pixclock	= 39721,
	.left_margin	= 40,
	.right_margin	= 24,
	.upper_margin	= 32,
	.lower_margin	= 11,
	.hsync_len	= 96,
	.vsync_len	= 2,
	.vmode		= FB_VMODE_NONINTERLACED
};

/* from GGI */
static const struct riva_regs reg_template = {
	{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,	/* ATTR */
	 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
	 0x41, 0x01, 0x0F, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* CRT  */
	 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE3,	/* 0x10 */
	 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* 0x20 */
	 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* 0x30 */
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00,							/* 0x40 */
	 },
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,	/* GRA  */
	 0xFF},
	{0x03, 0x01, 0x0F, 0x00, 0x0E},				/* SEQ  */
	0xEB							/* MISC */
};

/*
 * Backlight control
 */
#ifdef CONFIG_PMAC_BACKLIGHT

static int riva_backlight_levels[] = {
    0x158,
    0x192,
    0x1c6,
    0x200,
    0x234,
    0x268,
    0x2a2,
    0x2d6,
    0x310,
    0x344,
    0x378,
    0x3b2,
    0x3e6,
    0x41a,
    0x454,
    0x534,
};

static int riva_set_backlight_enable(int on, int level, void *data);
static int riva_set_backlight_level(int level, void *data);
static struct backlight_controller riva_backlight_controller = {
	riva_set_backlight_enable,
	riva_set_backlight_level
};
#endif /* CONFIG_PMAC_BACKLIGHT */

/* ------------------------------------------------------------------------- *
 *
 * MMIO access macros
 *
 * ------------------------------------------------------------------------- */

static inline void CRTCout(struct riva_par *par, unsigned char index,
			   unsigned char val)
{
	VGA_WR08(par->riva.PCIO, 0x3d4, index);
	VGA_WR08(par->riva.PCIO, 0x3d5, val);
}

static inline unsigned char CRTCin(struct riva_par *par,
				   unsigned char index)
{
	VGA_WR08(par->riva.PCIO, 0x3d4, index);
	return (VGA_RD08(par->riva.PCIO, 0x3d5));
}

static inline void GRAout(struct riva_par *par, unsigned char index,
			  unsigned char val)
{
	VGA_WR08(par->riva.PVIO, 0x3ce, index);
	VGA_WR08(par->riva.PVIO, 0x3cf, val);
}

static inline unsigned char GRAin(struct riva_par *par,
				  unsigned char index)
{
	VGA_WR08(par->riva.PVIO, 0x3ce, index);
	return (VGA_RD08(par->riva.PVIO, 0x3cf));
}

static inline void SEQout(struct riva_par *par, unsigned char index,
			  unsigned char val)
{
	VGA_WR08(par->riva.PVIO, 0x3c4, index);
	VGA_WR08(par->riva.PVIO, 0x3c5, val);
}

static inline unsigned char SEQin(struct riva_par *par,
				  unsigned char index)
{
	VGA_WR08(par->riva.PVIO, 0x3c4, index);
	return (VGA_RD08(par->riva.PVIO, 0x3c5));
}

static inline void ATTRout(struct riva_par *par, unsigned char index,
			   unsigned char val)
{
	VGA_WR08(par->riva.PCIO, 0x3c0, index);
	VGA_WR08(par->riva.PCIO, 0x3c0, val);
}

static inline unsigned char ATTRin(struct riva_par *par,
				   unsigned char index)
{
	VGA_WR08(par->riva.PCIO, 0x3c0, index);
	return (VGA_RD08(par->riva.PCIO, 0x3c1));
}

static inline void MISCout(struct riva_par *par, unsigned char val)
{
	VGA_WR08(par->riva.PVIO, 0x3c2, val);
}

static inline unsigned char MISCin(struct riva_par *par)
{
	return (VGA_RD08(par->riva.PVIO, 0x3cc));
}

static u8 byte_rev[256] = {
	0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0,
	0x10, 0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70, 0xf0,
	0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8,
	0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8, 0x78, 0xf8,
	0x04, 0x84, 0x44, 0xc4, 0x24, 0xa4, 0x64, 0xe4,
	0x14, 0x94, 0x54, 0xd4, 0x34, 0xb4, 0x74, 0xf4,
	0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec,
	0x1c, 0x9c, 0x5c, 0xdc, 0x3c, 0xbc, 0x7c, 0xfc,
	0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2,
	0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2,
	0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea,
	0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa,
	0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6,
	0x16, 0x96, 0x56, 0xd6, 0x36, 0xb6, 0x76, 0xf6,
	0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee,
	0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe,
	0x01, 0x81, 0x41, 0xc1, 0x21, 0xa1, 0x61, 0xe1,
	0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1,
	0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9,
	0x19, 0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9,
	0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5,
	0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5,
	0x0d, 0x8d, 0x4d, 0xcd, 0x2d, 0xad, 0x6d, 0xed,
	0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd,
	0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3,
	0x13, 0x93, 0x53, 0xd3, 0x33, 0xb3, 0x73, 0xf3,
	0x0b, 0x8b, 0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb,
	0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb,
	0x07, 0x87, 0x47, 0xc7, 0x27, 0xa7, 0x67, 0xe7,
	0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
	0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef,
	0x1f, 0x9f, 0x5f, 0xdf, 0x3f, 0xbf, 0x7f, 0xff,
};

static inline void reverse_order(u32 *l)
{
	u8 *a = (u8 *)l;
	*a = byte_rev[*a], a++;
	*a = byte_rev[*a], a++;
	*a = byte_rev[*a], a++;
	*a = byte_rev[*a];
}

/* ------------------------------------------------------------------------- *
 *
 * cursor stuff
 *
 * ------------------------------------------------------------------------- */

/**
 * rivafb_load_cursor_image - load cursor image to hardware
 * @data: address to monochrome bitmap (1 = foreground color, 0 = background)
 * @par:  pointer to private data
 * @w:    width of cursor image in pixels
 * @h:    height of cursor image in scanlines
 * @bg:   background color (ARGB1555) - alpha bit determines opacity
 * @fg:   foreground color (ARGB1555)
 *
 * DESCRIPTiON:
 * Loads cursor image based on a monochrome source and mask bitmap.  The
 * image bits determines the color of the pixel, 0 for background, 1 for
 * foreground.  Only the affected region (as determined by @w and @h 
 * parameters) will be updated.
 *
 * CALLED FROM:
 * rivafb_cursor()
 */
static void rivafb_load_cursor_image(struct riva_par *par, u8 *data8,
				     u16 bg, u16 fg, u32 w, u32 h)
{
	int i, j, k = 0;
	u32 b, tmp;
	u32 *data = (u32 *)data8;

	for (i = 0; i < h; i++) {
		b = *data++;
		reverse_order(&b);
		
		for (j = 0; j < w/2; j++) {
			tmp = 0;
#if defined (__BIG_ENDIAN)
			tmp = (b & (1 << 31)) ? fg << 16 : bg << 16;
			b <<= 1;
			tmp |= (b & (1 << 31)) ? fg : bg;
			b <<= 1;
#else
			tmp = (b & 1) ? fg : bg;
			b >>= 1;
			tmp |= (b & 1) ? fg << 16 : bg << 16;
			b >>= 1;
#endif
			writel(tmp, &par->riva.CURSOR[k++]);
		}
		k += (MAX_CURS - w)/2;
	}
}

/* ------------------------------------------------------------------------- *
 *
 * general utility functions
 *
 * ------------------------------------------------------------------------- */

/**
 * riva_wclut - set CLUT entry
 * @chip: pointer to RIVA_HW_INST object
 * @regnum: register number
 * @red: red component
 * @green: green component
 * @blue: blue component
 *
 * DESCRIPTION:
 * Sets color register @regnum.
 *
 * CALLED FROM:
 * rivafb_setcolreg()
 */
static void riva_wclut(RIVA_HW_INST *chip,
		       unsigned char regnum, unsigned char red,
		       unsigned char green, unsigned char blue)
{
	VGA_WR08(chip->PDIO, 0x3c8, regnum);
	VGA_WR08(chip->PDIO, 0x3c9, red);
	VGA_WR08(chip->PDIO, 0x3c9, green);
	VGA_WR08(chip->PDIO, 0x3c9, blue);
}

/**
 * riva_rclut - read fromCLUT register
 * @chip: pointer to RIVA_HW_INST object
 * @regnum: register number
 * @red: red component
 * @green: green component
 * @blue: blue component
 *
 * DESCRIPTION:
 * Reads red, green, and blue from color register @regnum.
 *
 * CALLED FROM:
 * rivafb_setcolreg()
 */
static void riva_rclut(RIVA_HW_INST *chip,
		       unsigned char regnum, unsigned char *red,
		       unsigned char *green, unsigned char *blue)
{
	
	VGA_WR08(chip->PDIO, 0x3c8, regnum);
	*red = VGA_RD08(chip->PDIO, 0x3c9);
	*green = VGA_RD08(chip->PDIO, 0x3c9);
	*blue = VGA_RD08(chip->PDIO, 0x3c9);
}

/**
 * riva_save_state - saves current chip state
 * @par: pointer to riva_par object containing info for current riva board
 * @regs: pointer to riva_regs object
 *
 * DESCRIPTION:
 * Saves current chip state to @regs.
 *
 * CALLED FROM:
 * rivafb_probe()
 */
/* from GGI */
static void riva_save_state(struct riva_par *par, struct riva_regs *regs)
{
	int i;

	NVTRACE_ENTER();
	par->riva.LockUnlock(&par->riva, 0);

	par->riva.UnloadStateExt(&par->riva, &regs->ext);

	regs->misc_output = MISCin(par);

	for (i = 0; i < NUM_CRT_REGS; i++)
		regs->crtc[i] = CRTCin(par, i);

	for (i = 0; i < NUM_ATC_REGS; i++)
		regs->attr[i] = ATTRin(par, i);

	for (i = 0; i < NUM_GRC_REGS; i++)
		regs->gra[i] = GRAin(par, i);

	for (i = 0; i < NUM_SEQ_REGS; i++)
		regs->seq[i] = SEQin(par, i);
	NVTRACE_LEAVE();
}

/**
 * riva_load_state - loads current chip state
 * @par: pointer to riva_par object containing info for current riva board
 * @regs: pointer to riva_regs object
 *
 * DESCRIPTION:
 * Loads chip state from @regs.
 *
 * CALLED FROM:
 * riva_load_video_mode()
 * rivafb_probe()
 * rivafb_remove()
 */
/* from GGI */
static void riva_load_state(struct riva_par *par, struct riva_regs *regs)
{
	RIVA_HW_STATE *state = &regs->ext;
	int i;

	NVTRACE_ENTER();
	CRTCout(par, 0x11, 0x00);

	par->riva.LockUnlock(&par->riva, 0);

	par->riva.LoadStateExt(&par->riva, state);

	MISCout(par, regs->misc_output);

	for (i = 0; i < NUM_CRT_REGS; i++) {
		switch (i) {
		case 0x19:
		case 0x20 ... 0x40:
			break;
		default:
			CRTCout(par, i, regs->crtc[i]);
		}
	}

	for (i = 0; i < NUM_ATC_REGS; i++)
		ATTRout(par, i, regs->attr[i]);

	for (i = 0; i < NUM_GRC_REGS; i++)
		GRAout(par, i, regs->gra[i]);

	for (i = 0; i < NUM_SEQ_REGS; i++)
		SEQout(par, i, regs->seq[i]);
	NVTRACE_LEAVE();
}

/**
 * riva_load_video_mode - calculate timings
 * @info: pointer to fb_info object containing info for current riva board
 *
 * DESCRIPTION:
 * Calculate some timings and then send em off to riva_load_state().
 *
 * CALLED FROM:
 * rivafb_set_par()
 */
static void riva_load_video_mode(struct fb_info *info)
{
	int bpp, width, hDisplaySize, hDisplay, hStart,
	    hEnd, hTotal, height, vDisplay, vStart, vEnd, vTotal, dotClock;
	int hBlankStart, hBlankEnd, vBlankStart, vBlankEnd;
	struct riva_par *par = (struct riva_par *) info->par;
	struct riva_regs newmode;
	
	NVTRACE_ENTER();
	/* time to calculate */
	rivafb_blank(1, info);

	bpp = info->var.bits_per_pixel;
	if (bpp == 16 && info->var.green.length == 5)
		bpp = 15;
	width = info->var.xres_virtual;
	hDisplaySize = info->var.xres;
	hDisplay = (hDisplaySize / 8) - 1;
	hStart = (hDisplaySize + info->var.right_margin) / 8 - 1;
	hEnd = (hDisplaySize + info->var.right_margin +
		info->var.hsync_len) / 8 - 1;
	hTotal = (hDisplaySize + info->var.right_margin +
		  info->var.hsync_len + info->var.left_margin) / 8 - 5;
	hBlankStart = hDisplay;
	hBlankEnd = hTotal + 4;

	height = info->var.yres_virtual;
	vDisplay = info->var.yres - 1;
	vStart = info->var.yres + info->var.lower_margin - 1;
	vEnd = info->var.yres + info->var.lower_margin +
	       info->var.vsync_len - 1;
	vTotal = info->var.yres + info->var.lower_margin +
		 info->var.vsync_len + info->var.upper_margin + 2;
	vBlankStart = vDisplay;
	vBlankEnd = vTotal + 1;
	dotClock = 1000000000 / info->var.pixclock;

	memcpy(&newmode, &reg_template, sizeof(struct riva_regs));

	if ((info->var.vmode & FB_VMODE_MASK) == FB_VMODE_INTERLACED)
		vTotal |= 1;

	if (par->FlatPanel) {
		vStart = vTotal - 3;
		vEnd = vTotal - 2;
		vBlankStart = vStart;
		hStart = hTotal - 3;
		hEnd = hTotal - 2;
		hBlankEnd = hTotal + 4;
	}

	newmode.crtc[0x0] = Set8Bits (hTotal); 
	newmode.crtc[0x1] = Set8Bits (hDisplay);
	newmode.crtc[0x2] = Set8Bits (hBlankStart);
	newmode.crtc[0x3] = SetBitField (hBlankEnd, 4: 0, 4:0) | SetBit (7);
	newmode.crtc[0x4] = Set8Bits (hStart);
	newmode.crtc[0x5] = SetBitField (hBlankEnd, 5: 5, 7:7)
		| SetBitField (hEnd, 4: 0, 4:0);
	newmode.crtc[0x6] = SetBitField (vTotal, 7: 0, 7:0);
	newmode.crtc[0x7] = SetBitField (vTotal, 8: 8, 0:0)
		| SetBitField (vDisplay, 8: 8, 1:1)
		| SetBitField (vStart, 8: 8, 2:2)
		| SetBitField (vBlankStart, 8: 8, 3:3)
		| SetBit (4)
		| SetBitField (vTotal, 9: 9, 5:5)
		| SetBitField (vDisplay, 9: 9, 6:6)
		| SetBitField (vStart, 9: 9, 7:7);
	newmode.crtc[0x9] = SetBitField (vBlankStart, 9: 9, 5:5)
		| SetBit (6);
	newmode.crtc[0x10] = Set8Bits (vStart);
	newmode.crtc[0x11] = SetBitField (vEnd, 3: 0, 3:0)
		| SetBit (5);
	newmode.crtc[0x12] = Set8Bits (vDisplay);
	newmode.crtc[0x13] = (width / 8) * ((bpp + 1) / 8);
	newmode.crtc[0x15] = Set8Bits (vBlankStart);
	newmode.crtc[0x16] = Set8Bits (vBlankEnd);

	newmode.ext.screen = SetBitField(hBlankEnd,6:6,4:4)
		| SetBitField(vBlankStart,10:10,3:3)
		| SetBitField(vStart,10:10,2:2)
		| SetBitField(vDisplay,10:10,1:1)
		| SetBitField(vTotal,10:10,0:0);
	newmode.ext.horiz  = SetBitField(hTotal,8:8,0:0) 
		| SetBitField(hDisplay,8:8,1:1)
		| SetBitField(hBlankStart,8:8,2:2)
		| SetBitField(hStart,8:8,3:3);
	newmode.ext.extra  = SetBitField(vTotal,11:11,0:0)
		| SetBitField(vDisplay,11:11,2:2)
		| SetBitField(vStart,11:11,4:4)
		| SetBitField(vBlankStart,11:11,6:6); 

	if ((info->var.vmode & FB_VMODE_MASK) == FB_VMODE_INTERLACED) {
		int tmp = (hTotal >> 1) & ~1;
		newmode.ext.interlace = Set8Bits(tmp);
		newmode.ext.horiz |= SetBitField(tmp, 8:8,4:4);
	} else 
		newmode.ext.interlace = 0xff; /* interlace off */

	if (par->riva.Architecture >= NV_ARCH_10)
		par->riva.CURSOR = (U032 *)(info->screen_base + par->riva.CursorStart);

	if (info->var.sync & FB_SYNC_HOR_HIGH_ACT)
		newmode.misc_output &= ~0x40;
	else
		newmode.misc_output |= 0x40;
	if (info->var.sync & FB_SYNC_VERT_HIGH_ACT)
		newmode.misc_output &= ~0x80;
	else
		newmode.misc_output |= 0x80;	

	par->riva.CalcStateExt(&par->riva, &newmode.ext, bpp, width,
				  hDisplaySize, height, dotClock);

	newmode.ext.scale = par->riva.PRAMDAC[0x00000848/4] & 0xfff000ff;
	if (par->FlatPanel == 1) {
		newmode.ext.pixel |= (1 << 7);
		newmode.ext.scale |= (1 << 8);
	}
	if (par->SecondCRTC) {
		newmode.ext.head  = par->riva.PCRTC0[0x00000860/4] & ~0x00001000;
		newmode.ext.head2 = par->riva.PCRTC0[0x00002860/4] | 0x00001000;
		newmode.ext.crtcOwner = 3;
		newmode.ext.pllsel |= 0x20000800;
		newmode.ext.vpll2 = newmode.ext.vpll;
	} else if (par->riva.twoHeads) {
		newmode.ext.head  =  par->riva.PCRTC0[0x00000860/4] | 0x00001000;
		newmode.ext.head2 =  par->riva.PCRTC0[0x00002860/4] & ~0x00001000;
		newmode.ext.crtcOwner = 0;
		newmode.ext.vpll2 = par->riva.PRAMDAC0[0x00000520/4];
	}
	if (par->FlatPanel == 1) {
		newmode.ext.pixel |= (1 << 7);
		newmode.ext.scale |= (1 << 8);
	}
	newmode.ext.cursorConfig = 0x02000100;
	par->current_state = newmode;
	riva_load_state(par, &par->current_state);
	par->riva.LockUnlock(&par->riva, 0); /* important for HW cursor */
	rivafb_blank(0, info);
	NVTRACE_LEAVE();
}

static void riva_update_var(struct fb_var_screeninfo *var, struct fb_videomode *modedb)
{
	NVTRACE_ENTER();
	var->xres = var->xres_virtual = modedb->xres;
	var->yres = modedb->yres;
        if (var->yres_virtual < var->yres)
	    var->yres_virtual = var->yres;
        var->xoffset = var->yoffset = 0;
        var->pixclock = modedb->pixclock;
        var->left_margin = modedb->left_margin;
        var->right_margin = modedb->right_margin;
        var->upper_margin = modedb->upper_margin;
        var->lower_margin = modedb->lower_margin;
        var->hsync_len = modedb->hsync_len;
        var->vsync_len = modedb->vsync_len;
        var->sync = modedb->sync;
        var->vmode = modedb->vmode;
	NVTRACE_LEAVE();
}

/**
 * rivafb_do_maximize - 
 * @info: pointer to fb_info object containing info for current riva board
 * @var:
 * @nom:
 * @den:
 *
 * DESCRIPTION:
 * .
 *
 * RETURNS:
 * -EINVAL on failure, 0 on success
 * 
 *
 * CALLED FROM:
 * rivafb_check_var()
 */
static int rivafb_do_maximize(struct fb_info *info,
			      struct fb_var_screeninfo *var,
			      int nom, int den)
{
	static struct {
		int xres, yres;
	} modes[] = {
		{1600, 1280},
		{1280, 1024},
		{1024, 768},
		{800, 600},
		{640, 480},
		{-1, -1}
	};
	int i;

	NVTRACE_ENTER();
	/* use highest possible virtual resolution */
	if (var->xres_virtual == -1 && var->yres_virtual == -1) {
		printk(KERN_WARNING PFX
		       "using maximum available virtual resolution\n");
		for (i = 0; modes[i].xres != -1; i++) {
			if (modes[i].xres * nom / den * modes[i].yres <
			    info->fix.smem_len)
				break;
		}
		if (modes[i].xres == -1) {
			printk(KERN_ERR PFX
			       "could not find a virtual resolution that fits into video memory!!\n");
			NVTRACE("EXIT - EINVAL error\n");
			return -EINVAL;
		}
		var->xres_virtual = modes[i].xres;
		var->yres_virtual = modes[i].yres;

		printk(KERN_INFO PFX
		       "virtual resolution set to maximum of %dx%d\n",
		       var->xres_virtual, var->yres_virtual);
	} else if (var->xres_virtual == -1) {
		var->xres_virtual = (info->fix.smem_len * den /
			(nom * var->yres_virtual)) & ~15;
		printk(KERN_WARNING PFX
		       "setting virtual X resolution to %d\n", var->xres_virtual);
	} else if (var->yres_virtual == -1) {
		var->xres_virtual = (var->xres_virtual + 15) & ~15;
		var->yres_virtual = info->fix.smem_len * den /
			(nom * var->xres_virtual);
		printk(KERN_WARNING PFX
		       "setting virtual Y resolution to %d\n", var->yres_virtual);
	} else {
		var->xres_virtual = (var->xres_virtual + 15) & ~15;
		if (var->xres_virtual * nom / den * var->yres_virtual > info->fix.smem_len) {
			printk(KERN_ERR PFX
			       "mode %dx%dx%d rejected...resolution too high to fit into video memory!\n",
			       var->xres, var->yres, var->bits_per_pixel);
			NVTRACE("EXIT - EINVAL error\n");
			return -EINVAL;
		}
	}
	
	if (var->xres_virtual * nom / den >= 8192) {
		printk(KERN_WARNING PFX
		       "virtual X resolution (%d) is too high, lowering to %d\n",
		       var->xres_virtual, 8192 * den / nom - 16);
		var->xres_virtual = 8192 * den / nom - 16;
	}
	
	if (var->xres_virtual < var->xres) {
		printk(KERN_ERR PFX
		       "virtual X resolution (%d) is smaller than real\n", var->xres_virtual);
		return -EINVAL;
	}

	if (var->yres_virtual < var->yres) {
		printk(KERN_ERR PFX
		       "virtual Y resolution (%d) is smaller than real\n", var->yres_virtual);
		return -EINVAL;
	}
	if (var->yres_virtual > 0x7fff/nom)
		var->yres_virtual = 0x7fff/nom;
	if (var->xres_virtual > 0x7fff/nom)
		var->xres_virtual = 0x7fff/nom;
	NVTRACE_LEAVE();
	return 0;
}

static void
riva_set_pattern(struct riva_par *par, int clr0, int clr1, int pat0, int pat1)
{
	RIVA_FIFO_FREE(par->riva, Patt, 4);
	par->riva.Patt->Color0        = clr0;
	par->riva.Patt->Color1        = clr1;
	par->riva.Patt->Monochrome[0] = pat0;
	par->riva.Patt->Monochrome[1] = pat1;
}

/* acceleration routines */
inline void wait_for_idle(struct riva_par *par)
{
	while (par->riva.Busy(&par->riva));
}

/*
 * Set ROP.  Translate X rop into ROP3.  Internal routine.
 */
static void
riva_set_rop_solid(struct riva_par *par, int rop)
{
	riva_set_pattern(par, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
        RIVA_FIFO_FREE(par->riva, Rop, 1);
        par->riva.Rop->Rop3 = rop;

}

void riva_setup_accel(struct fb_info *info)
{
	struct riva_par *par = (struct riva_par *) info->par;

	RIVA_FIFO_FREE(par->riva, Clip, 2);
	par->riva.Clip->TopLeft     = 0x0;
	par->riva.Clip->WidthHeight = (info->var.xres_virtual & 0xffff) |
		(info->var.yres_virtual << 16);
	riva_set_rop_solid(par, 0xcc);
	wait_for_idle(par);
}

/**
 * riva_get_cmap_len - query current color map length
 * @var: standard kernel fb changeable data
 *
 * DESCRIPTION:
 * Get current color map length.
 *
 * RETURNS:
 * Length of color map
 *
 * CALLED FROM:
 * rivafb_setcolreg()
 */
static int riva_get_cmap_len(const struct fb_var_screeninfo *var)
{
	int rc = 256;		/* reasonable default */

	switch (var->green.length) {
	case 8:
		rc = 256;	/* 256 entries (2^8), 8 bpp and RGB8888 */
		break;
	case 5:
		rc = 32;	/* 32 entries (2^5), 16 bpp, RGB555 */
		break;
	case 6:
		rc = 64;	/* 64 entries (2^6), 16 bpp, RGB565 */
		break;		
	default:
		/* should not occur */
		break;
	}
	return rc;
}

/* ------------------------------------------------------------------------- *
 *
 * Backlight operations
 *
 * ------------------------------------------------------------------------- */

#ifdef CONFIG_PMAC_BACKLIGHT
static int riva_set_backlight_enable(int on, int level, void *data)
{
	struct riva_par *par = (struct riva_par *)data;
	U032 tmp_pcrt, tmp_pmc;

	tmp_pmc = par->riva.PMC[0x10F0/4] & 0x0000FFFF;
	tmp_pcrt = par->riva.PCRTC0[0x081C/4] & 0xFFFFFFFC;
	if(on && (level > BACKLIGHT_OFF)) {
		tmp_pcrt |= 0x1;
		tmp_pmc |= (1 << 31); // backlight bit
		tmp_pmc |= riva_backlight_levels[level-1] << 16; // level
	}
	par->riva.PCRTC0[0x081C/4] = tmp_pcrt;
	par->riva.PMC[0x10F0/4] = tmp_pmc;
	return 0;
}

static int riva_set_backlight_level(int level, void *data)
{
	return riva_set_backlight_enable(1, level, data);
}
#endif /* CONFIG_PMAC_BACKLIGHT */

/* ------------------------------------------------------------------------- *
 *
 * framebuffer operations
 *
 * ------------------------------------------------------------------------- */

static int rivafb_open(struct fb_info *info, int user)
{
	struct riva_par *par = (struct riva_par *) info->par;
	int cnt = atomic_read(&par->ref_count);

	NVTRACE_ENTER();
	if (!cnt) {
		memset(&par->state, 0, sizeof(struct vgastate));
		par->state.flags = VGA_SAVE_MODE  | VGA_SAVE_FONTS;
		/* save the DAC for Riva128 */
		if (par->riva.Architecture == NV_ARCH_03)
			par->state.flags |= VGA_SAVE_CMAP;
		save_vga(&par->state);

		RivaGetConfig(&par->riva, par->Chipset);
		/* vgaHWunlock() + riva unlock (0x7F) */
		CRTCout(par, 0x11, 0xFF);
		par->riva.LockUnlock(&par->riva, 0);
	
		riva_save_state(par, &par->initial_state);
	}
	atomic_inc(&par->ref_count);
	NVTRACE_LEAVE();
	return 0;
}

static int rivafb_release(struct fb_info *info, int user)
{
	struct riva_par *par = (struct riva_par *) info->par;
	int cnt = atomic_read(&par->ref_count);

	NVTRACE_ENTER();
	if (!cnt)
		return -EINVAL;
	if (cnt == 1) {
		par->riva.LockUnlock(&par->riva, 0);
		par->riva.LoadStateExt(&par->riva, &par->initial_state.ext);
		riva_load_state(par, &par->initial_state);
		restore_vga(&par->state);
		par->riva.LockUnlock(&par->riva, 1);
	}
	atomic_dec(&par->ref_count);
	NVTRACE_LEAVE();
	return 0;
}

static int rivafb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct fb_monspecs *specs = &info->monspecs;
	int nom, den;		/* translating from pixels->bytes */
	int mode_valid = 0;
	
	NVTRACE_ENTER();
	switch (var->bits_per_pixel) {
	case 1 ... 8:
		var->red.offset = var->green.offset = var->blue.offset = 0;
		var->red.length = var->green.length = var->blue.length = 8;
		var->bits_per_pixel = 8;
		nom = den = 1;
		break;
	case 9 ... 15:
		var->green.length = 5;
		/* fall through */
	case 16:
		var->bits_per_pixel = 16;
		if (var->green.length == 5) {
			/* 0rrrrrgg gggbbbbb */
			var->red.offset = 10;
			var->green.offset = 5;
			var->blue.offset = 0;
			var->red.length = 5;
			var->green.length = 5;
			var->blue.length = 5;
		} else {
			/* rrrrrggg gggbbbbb */
			var->red.offset = 11;
			var->green.offset = 5;
			var->blue.offset = 0;
			var->red.length = 5;
			var->green.length = 6;
			var->blue.length = 5;
		}
		nom = 2;
		den = 1;
		break;
	case 17 ... 32:
		var->red.length = var->green.length = var->blue.length = 8;
		var->bits_per_pixel = 32;
		var->red.offset = 16;
		var->green.offset = 8;
		var->blue.offset = 0;
		nom = 4;
		den = 1;
		break;
	default:
		printk(KERN_ERR PFX
		       "mode %dx%dx%d rejected...color depth not supported.\n",
		       var->xres, var->yres, var->bits_per_pixel);
		NVTRACE("EXIT, returning -EINVAL\n");
		return -EINVAL;
	}

	if (!strictmode) {
		if (!fb_validate_mode(var, info))
			mode_valid = 1;
	}

	/* find best mode from modedb */
	if (!mode_valid && specs->modedb_len) {
		int i, best, best_refresh, best_x, best_y, diff_x, diff_y;

		best_refresh = best = best_x = best_y = 0;
		diff_x = diff_y = -1;

		for (i = 0; i < specs->modedb_len; i++) {
			if (var->xres <= specs->modedb[i].xres &&
			    !(specs->modedb[i].flag & FB_MODE_IS_CALCULATED) &&
			    specs->modedb[i].xres - var->xres < diff_x) {
				best_x = specs->modedb[i].xres;
				diff_x = best_x - var->xres;
			}
			if (!diff_x) break;
		}

		if (diff_x != -1) {
			for (i = 0; i < specs->modedb_len; i++) {
				if (best_x == specs->modedb[i].xres &&
				    var->yres <= specs->modedb[i].yres &&
				    !(specs->modedb[i].flag &
				      FB_MODE_IS_CALCULATED) &&
				    specs->modedb[i].yres-var->yres < diff_y) {
					best_y = specs->modedb[i].yres;
					diff_y = best_y - var->yres;
				}
				if (!diff_y) break;
			}
		}

		if (diff_y != -1) {
			for (i = 0; i < specs->modedb_len; i++) {
				if (best_x == specs->modedb[i].xres &&
				    best_y == specs->modedb[i].yres &&
				    !(specs->modedb[i].flag &
				      FB_MODE_IS_CALCULATED) &&
				    specs->modedb[i].refresh > best_refresh) {
					best_refresh=specs->modedb[i].refresh;
					best = i;
				}
			}
		}

		if (best_refresh) {
			riva_update_var(var, &specs->modedb[best]);
			mode_valid = 1;
		}
	}

	/* calculate modeline if supported by monitor */
	if (!mode_valid && info->monspecs.gtf) {
		if (!fb_get_mode(FB_MAXTIMINGS, 0, var, info))
			mode_valid = 1;
	}
	if (!mode_valid && info->monspecs.modedb_len)
		return -EINVAL;

	if (var->xres_virtual < var->xres)
		var->xres_virtual = var->xres;
	if (var->yres_virtual <= var->yres)
		var->yres_virtual = -1;
	if (rivafb_do_maximize(info, var, nom, den) < 0)
		return -EINVAL;

	if (var->xoffset < 0)
		var->xoffset = 0;
	if (var->yoffset < 0)
		var->yoffset = 0;

	/* truncate xoffset and yoffset to maximum if too high */
	if (var->xoffset > var->xres_virtual - var->xres)
		var->xoffset = var->xres_virtual - var->xres - 1;

	if (var->yoffset > var->yres_virtual - var->yres)
		var->yoffset = var->yres_virtual - var->yres - 1;

	var->red.msb_right = 
	    var->green.msb_right =
	    var->blue.msb_right =
	    var->transp.offset = var->transp.length = var->transp.msb_right = 0;
	NVTRACE_LEAVE();
	return 0;
}

static int rivafb_set_par(struct fb_info *info)
{
	struct riva_par *par = (struct riva_par *) info->par;

	NVTRACE_ENTER();
	riva_common_setup(par);
	RivaGetConfig(&par->riva, par->Chipset);
	/* vgaHWunlock() + riva unlock (0x7F) */
	CRTCout(par, 0x11, 0xFF);
	par->riva.LockUnlock(&par->riva, 0);

	riva_load_video_mode(info);
	riva_setup_accel(info);
	
	memset_io(par->riva.CURSOR, 0, MAX_CURS * MAX_CURS * 2);
	info->fix.line_length = (info->var.xres_virtual * (info->var.bits_per_pixel >> 3));
	info->fix.visual = (info->var.bits_per_pixel == 8) ?
				FB_VISUAL_PSEUDOCOLOR : FB_VISUAL_DIRECTCOLOR;
	NVTRACE_LEAVE();
	return 0;
}

/**
 * rivafb_pan_display
 * @var: standard kernel fb changeable data
 * @con: TODO
 * @info: pointer to fb_info object containing info for current riva board
 *
 * DESCRIPTION:
 * Pan (or wrap, depending on the `vmode' field) the display using the
 * `xoffset' and `yoffset' fields of the `var' structure.
 * If the values don't fit, return -EINVAL.
 *
 * This call looks only at xoffset, yoffset and the FB_VMODE_YWRAP flag
 */
static int rivafb_pan_display(struct fb_var_screeninfo *var,
			      struct fb_info *info)
{
	struct riva_par *par = (struct riva_par *)info->par;
	unsigned int base;

	NVTRACE_ENTER();
	if (var->xoffset > (var->xres_virtual - var->xres))
		return -EINVAL;
	if (var->yoffset > (var->yres_virtual - var->yres))
		return -EINVAL;

	if (var->vmode & FB_VMODE_YWRAP) {
		if (var->yoffset < 0
		    || var->yoffset >= info->var.yres_virtual
		    || var->xoffset) return -EINVAL;
	} else {
		if (var->xoffset + info->var.xres > info->var.xres_virtual ||
		    var->yoffset + info->var.yres > info->var.yres_virtual)
			return -EINVAL;
	}

	base = var->yoffset * info->fix.line_length + var->xoffset;

	par->riva.SetStartAddress(&par->riva, base);

	info->var.xoffset = var->xoffset;
	info->var.yoffset = var->yoffset;

	if (var->vmode & FB_VMODE_YWRAP)
		info->var.vmode |= FB_VMODE_YWRAP;
	else
		info->var.vmode &= ~FB_VMODE_YWRAP;
	NVTRACE_LEAVE();
	return 0;
}

static int rivafb_blank(int blank, struct fb_info *info)
{
	struct riva_par *par= (struct riva_par *)info->par;
	unsigned char tmp, vesa;

	tmp = SEQin(par, 0x01) & ~0x20;	/* screen on/off */
	vesa = CRTCin(par, 0x1a) & ~0xc0;	/* sync on/off */

	NVTRACE_ENTER();
	if (blank) {
		tmp |= 0x20;
		switch (blank - 1) {
		case VESA_NO_BLANKING:
			break;
		case VESA_VSYNC_SUSPEND:
			vesa |= 0x80;
			break;
		case VESA_HSYNC_SUSPEND:
			vesa |= 0x40;
			break;
		case VESA_POWERDOWN:
			vesa |= 0xc0;
			break;
		}
	}
	SEQout(par, 0x01, tmp);
	CRTCout(par, 0x1a, vesa);

#ifdef CONFIG_PMAC_BACKLIGHT
	if ( par->FlatPanel && _machine == _MACH_Pmac) {
		set_backlight_enable(!blank);
	}
#endif

	NVTRACE_LEAVE();
	return 0;
}

/**
 * rivafb_setcolreg
 * @regno: register index
 * @red: red component
 * @green: green component
 * @blue: blue component
 * @transp: transparency
 * @info: pointer to fb_info object containing info for current riva board
 *
 * DESCRIPTION:
 * Set a single color register. The values supplied have a 16 bit
 * magnitude.
 *
 * RETURNS:
 * Return != 0 for invalid regno.
 *
 * CALLED FROM:
 * fbcmap.c:fb_set_cmap()
 */
static int rivafb_setcolreg(unsigned regno, unsigned red, unsigned green,
			  unsigned blue, unsigned transp,
			  struct fb_info *info)
{
	struct riva_par *par = (struct riva_par *)info->par;
	RIVA_HW_INST *chip = &par->riva;
	int i;

	if (regno >= riva_get_cmap_len(&info->var))
		return -EINVAL;

	if (info->var.grayscale) {
		/* gray = 0.30*R + 0.59*G + 0.11*B */
		red = green = blue =
		    (red * 77 + green * 151 + blue * 28) >> 8;
	}

	switch (info->var.bits_per_pixel) {
	case 8:
		/* "transparent" stuff is completely ignored. */
		riva_wclut(chip, regno, red >> 8, green >> 8, blue >> 8);
		break;
	case 16:
		if (info->var.green.length == 5) {
			if (regno < 16) {
				/* 0rrrrrgg gggbbbbb */
				((u32 *)info->pseudo_palette)[regno] =
					((red & 0xf800) >> 1) |
					((green & 0xf800) >> 6) |
					((blue & 0xf800) >> 11);
			}
			for (i = 0; i < 8; i++) 
				riva_wclut(chip, regno*8+i, red >> 8,
					   green >> 8, blue >> 8);
		} else {
			u8 r, g, b;

			if (regno < 16) {
				/* rrrrrggg gggbbbbb */
				((u32 *)info->pseudo_palette)[regno] =
					((red & 0xf800) >> 0) |
					((green & 0xf800) >> 5) |
					((blue & 0xf800) >> 11);
			}
			if (regno < 32) {
				for (i = 0; i < 8; i++) {
					riva_wclut(chip, regno*8+i, red >> 8, 
						   green >> 8, blue >> 8);
				}
			}
			for (i = 0; i < 4; i++) {
				riva_rclut(chip, regno*2+i, &r, &g, &b);
				riva_wclut(chip, regno*4+i, r, green >> 8, b);
			}
		}
		break;
	case 32:
		if (regno < 16) {
			((u32 *)info->pseudo_palette)[regno] =
				((red & 0xff00) << 8) |
				((green & 0xff00)) | ((blue & 0xff00) >> 8);
			
		}
		riva_wclut(chip, regno, red >> 8, green >> 8, blue >> 8);
		break;
	default:
		/* do nothing */
		break;
	}
	return 0;
}

/**
 * rivafb_fillrect - hardware accelerated color fill function
 * @info: pointer to fb_info structure
 * @rect: pointer to fb_fillrect structure
 *
 * DESCRIPTION:
 * This function fills up a region of framebuffer memory with a solid
 * color with a choice of two different ROP's, copy or invert.
 *
 * CALLED FROM:
 * framebuffer hook
 */
static void rivafb_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
	struct riva_par *par = (struct riva_par *) info->par;
	u_int color, rop = 0;

	if (info->var.bits_per_pixel == 8)
		color = rect->color;
	else
		color = ((u32 *)info->pseudo_palette)[rect->color];

	switch (rect->rop) {
	case ROP_XOR:
		rop = 0x66;
		break;
	case ROP_COPY:
	default:
		rop = 0xCC;
		break;
	}

	riva_set_rop_solid(par, rop);

	RIVA_FIFO_FREE(par->riva, Bitmap, 1);
	par->riva.Bitmap->Color1A = color;

	RIVA_FIFO_FREE(par->riva, Bitmap, 2);
	par->riva.Bitmap->UnclippedRectangle[0].TopLeft =
			(rect->dx << 16) | rect->dy;
	mb();
	par->riva.Bitmap->UnclippedRectangle[0].WidthHeight =
			(rect->width << 16) | rect->height;
	mb();
	riva_set_rop_solid(par, 0xcc);

}

/**
 * rivafb_copyarea - hardware accelerated blit function
 * @info: pointer to fb_info structure
 * @region: pointer to fb_copyarea structure
 *
 * DESCRIPTION:
 * This copies an area of pixels from one location to another
 *
 * CALLED FROM:
 * framebuffer hook
 */
static void rivafb_copyarea(struct fb_info *info, const struct fb_copyarea *region)
{
	struct riva_par *par = (struct riva_par *) info->par;

	RIVA_FIFO_FREE(par->riva, Blt, 3);
	par->riva.Blt->TopLeftSrc  = (region->sy << 16) | region->sx;
	par->riva.Blt->TopLeftDst  = (region->dy << 16) | region->dx;
	mb();
	par->riva.Blt->WidthHeight = (region->height << 16) | region->width;
	mb();
}

static inline void convert_bgcolor_16(u32 *col)
{
	*col = ((*col & 0x0000F800) << 8)
		| ((*col & 0x00007E0) << 5)
		| ((*col & 0x0000001F) << 3)
		|	   0xFF000000;
	mb();
}

/**
 * rivafb_imageblit: hardware accelerated color expand function
 * @info: pointer to fb_info structure
 * @image: pointer to fb_image structure
 *
 * DESCRIPTION:
 * If the source is a monochrome bitmap, the function fills up a a region
 * of framebuffer memory with pixels whose color is determined by the bit
 * setting of the bitmap, 1 - foreground, 0 - background.
 *
 * If the source is not a monochrome bitmap, color expansion is not done.
 * In this case, it is channeled to a software function.
 *
 * CALLED FROM:
 * framebuffer hook
 */
static void rivafb_imageblit(struct fb_info *info, 
			     const struct fb_image *image)
{
	struct riva_par *par = (struct riva_par *) info->par;
	u32 fgx = 0, bgx = 0, width, tmp;
	u8 *cdat = (u8 *) image->data;
	volatile u32 *d;
	int i, size;

	if (image->depth != 1) {
		cfb_imageblit(info, image);
		return;
	}

	switch (info->var.bits_per_pixel) {
	case 8:
		fgx = image->fg_color;
		bgx = image->bg_color;
		break;
	case 16:
		fgx = ((u32 *)info->pseudo_palette)[image->fg_color];
		bgx = ((u32 *)info->pseudo_palette)[image->bg_color];
		if (info->var.green.length == 6)
			convert_bgcolor_16(&bgx);	
		break;
	case 32:
		fgx = ((u32 *)info->pseudo_palette)[image->fg_color];
		bgx = ((u32 *)info->pseudo_palette)[image->bg_color];
		break;
	}

	RIVA_FIFO_FREE(par->riva, Bitmap, 7);
	par->riva.Bitmap->ClipE.TopLeft     = 
		(image->dy << 16) | (image->dx & 0xFFFF);
	par->riva.Bitmap->ClipE.BottomRight = 
		(((image->dy + image->height) << 16) |
		 ((image->dx + image->width) & 0xffff));
	par->riva.Bitmap->Color0E           = bgx;
	par->riva.Bitmap->Color1E           = fgx;
	par->riva.Bitmap->WidthHeightInE    = 
		(image->height << 16) | ((image->width + 31) & ~31);
	par->riva.Bitmap->WidthHeightOutE   = 
		(image->height << 16) | ((image->width + 31) & ~31);
	par->riva.Bitmap->PointE            = 
		(image->dy << 16) | (image->dx & 0xFFFF);

	d = &par->riva.Bitmap->MonochromeData01E;

	width = (image->width + 31)/32;
	size = width * image->height;
	while (size >= 16) {
		RIVA_FIFO_FREE(par->riva, Bitmap, 16);
		for (i = 0; i < 16; i++) {
			tmp = *((u32 *)cdat);
			cdat = (u8 *)((u32 *)cdat + 1);
			reverse_order(&tmp);
			d[i] = tmp;
		}
		size -= 16;
	}
	if (size) {
		RIVA_FIFO_FREE(par->riva, Bitmap, size);
		for (i = 0; i < size; i++) {
			tmp = *((u32 *) cdat);
			cdat = (u8 *)((u32 *)cdat + 1);
			reverse_order(&tmp);
			d[i] = tmp;
		}
	}
}

/**
 * rivafb_cursor - hardware cursor function
 * @info: pointer to info structure
 * @cursor: pointer to fbcursor structure
 *
 * DESCRIPTION:
 * A cursor function that supports displaying a cursor image via hardware.
 * Within the kernel, copy and invert rops are supported.  If exported
 * to user space, only the copy rop will be supported.
 *
 * CALLED FROM
 * framebuffer hook
 */
static int rivafb_cursor(struct fb_info *info, struct fb_cursor *cursor)
{
	struct riva_par *par = (struct riva_par *) info->par;
	u8 data[MAX_CURS * MAX_CURS/8];
	u16 fg, bg;
	int i;

	par->riva.ShowHideCursor(&par->riva, 0);

	if (cursor->set & FB_CUR_SETPOS) {
		u32 xx, yy, temp;

		info->cursor.image.dx = cursor->image.dx;
		info->cursor.image.dy = cursor->image.dy;
		yy = cursor->image.dy - info->var.yoffset;
		xx = cursor->image.dx - info->var.xoffset;
		temp = xx & 0xFFFF;
		temp |= yy << 16;

		par->riva.PRAMDAC[0x0000300/4] = temp;
	}

	if (cursor->set & FB_CUR_SETSIZE) {
		info->cursor.image.height = cursor->image.height;
		info->cursor.image.width = cursor->image.width;
		memset_io(par->riva.CURSOR, 0, MAX_CURS * MAX_CURS * 2);
	}

	if (cursor->set & FB_CUR_SETCMAP) {
		info->cursor.image.bg_color = cursor->image.bg_color;
		info->cursor.image.fg_color = cursor->image.fg_color;
	}

	if (cursor->set & (FB_CUR_SETSHAPE | FB_CUR_SETCMAP | FB_CUR_SETCUR)) {
		u32 bg_idx = info->cursor.image.bg_color;
		u32 fg_idx = info->cursor.image.fg_color;
		u32 s_pitch = (info->cursor.image.width+7) >> 3;
		u32 d_pitch = MAX_CURS/8;
		u8 *dat = (u8 *) cursor->image.data;
		u8 *msk = (u8 *) info->cursor.mask;
		u8 src[64];	
		
		info->cursor.image.data = cursor->image.data;
		switch (info->cursor.rop) {
		case ROP_XOR:
			for (i = 0; i < s_pitch * info->cursor.image.height;
			     i++)
				src[i] = dat[i] ^ msk[i];
			break;
		case ROP_COPY:
		default:
			for (i = 0; i < s_pitch * info->cursor.image.height;
			     i++)
				src[i] = dat[i] & msk[i];
			break;
		}
		
		fb_sysmove_buf_aligned(info, &info->sprite, data, d_pitch, src,
				       s_pitch, info->cursor.image.height);

		bg = ((info->cmap.red[bg_idx] & 0xf8) << 7) |
		     ((info->cmap.green[bg_idx] & 0xf8) << 2) |
		     ((info->cmap.blue[bg_idx] & 0xf8) >> 3) | 1 << 15;

		fg = ((info->cmap.red[fg_idx] & 0xf8) << 7) |
		     ((info->cmap.green[fg_idx] & 0xf8) << 2) |
		     ((info->cmap.blue[fg_idx] & 0xf8) >> 3) | 1 << 15;

		par->riva.LockUnlock(&par->riva, 0);

		rivafb_load_cursor_image(par, data, bg, fg,
					 info->cursor.image.width, 
					 info->cursor.image.height);
	}
	if (info->cursor.enable)
		par->riva.ShowHideCursor(&par->riva, 1);
	return 0;
}

static int rivafb_sync(struct fb_info *info)
{
	struct riva_par *par = (struct riva_par *)info->par;

	wait_for_idle(par);
	return 0;
}

/* ------------------------------------------------------------------------- *
 *
 * initialization helper functions
 *
 * ------------------------------------------------------------------------- */

/* kernel interface */
static struct fb_ops riva_fb_ops = {
	.owner 		= THIS_MODULE,
	.fb_open	= rivafb_open,
	.fb_release	= rivafb_release,
	.fb_check_var 	= rivafb_check_var,
	.fb_set_par 	= rivafb_set_par,
	.fb_setcolreg 	= rivafb_setcolreg,
	.fb_pan_display	= rivafb_pan_display,
	.fb_blank 	= rivafb_blank,
	.fb_fillrect 	= rivafb_fillrect,
	.fb_copyarea 	= rivafb_copyarea,
	.fb_imageblit 	= rivafb_imageblit,
	.fb_cursor	= rivafb_cursor,	
	.fb_sync 	= rivafb_sync,
};

static int __devinit riva_set_fbinfo(struct fb_info *info)
{
	unsigned int cmap_len;

	NVTRACE_ENTER();
	info->flags = FBINFO_DEFAULT
		    | FBINFO_HWACCEL_XPAN
		    | FBINFO_HWACCEL_YPAN
		    | FBINFO_HWACCEL_COPYAREA
		    | FBINFO_HWACCEL_FILLRECT
		    | FBINFO_HWACCEL_IMAGEBLIT
	            | FBINFO_MISC_MODESWITCHLATE;
	info->var = rivafb_default_var;
	info->fix.visual = (info->var.bits_per_pixel == 8) ?
				FB_VISUAL_PSEUDOCOLOR : FB_VISUAL_DIRECTCOLOR;
	info->pseudo_palette = pseudo_palette;

	cmap_len = riva_get_cmap_len(&info->var);
	fb_alloc_cmap(&info->cmap, cmap_len, 0);	

	info->pixmap.size = 64 * 1024;
	info->pixmap.buf_align = 4;
	info->pixmap.scan_align = 4;
	info->pixmap.flags = FB_PIXMAP_SYSTEM;
	info->var.yres_virtual = -1;
	NVTRACE_LEAVE();
	return (rivafb_check_var(&info->var, info));
}

#ifdef CONFIG_PPC_OF
static int riva_get_EDID_OF(struct fb_info *info, struct pci_dev *pd)
{
	struct riva_par *par = (struct riva_par *) info->par;
	struct device_node *dp;
	unsigned char *pedid = NULL;
	unsigned char *disptype = NULL;
	static char *propnames[] = {
		"DFP,EDID", "LCD,EDID", "EDID", "EDID1", "EDID,B", "EDID,A", NULL };
	int i;

	NVTRACE_ENTER();
	dp = pci_device_to_OF_node(pd);
	for (; dp != NULL; dp = dp->child) {
		disptype = (unsigned char *)get_property(dp, "display-type", NULL);
		if (disptype == NULL)
			continue;
		if (strncmp(disptype, "LCD", 3) != 0)
			continue;
		for (i = 0; propnames[i] != NULL; ++i) {
			pedid = (unsigned char *)
				get_property(dp, propnames[i], NULL);
			if (pedid != NULL) {
		par->EDID = pedid;
		return 1;
			}
		}
	}
	NVTRACE_LEAVE();
		return 0;
}
#endif /* CONFIG_PPC_OF */

static void riva_update_default_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct fb_monspecs *specs = &info->monspecs;
	struct fb_videomode modedb;

	NVTRACE_ENTER();
	/* respect mode options */
	if (mode_option) {
		fb_find_mode(var, info, mode_option,
			     specs->modedb, specs->modedb_len,
			     NULL, 8);
	} else if (specs->modedb != NULL) {
		/* get preferred timing */
		if (info->monspecs.misc & FB_MISC_1ST_DETAIL) {
			int i;

			for (i = 0; i < specs->modedb_len; i++) {
				if (specs->modedb[i].flag & FB_MODE_IS_FIRST) {
					modedb = specs->modedb[i];
					break;
				}
			}
		} else {
			/* otherwise, get first mode in database */
			modedb = specs->modedb[0];
		}
		var->bits_per_pixel = 8;
		riva_update_var(var, &modedb);
	}
	var->accel_flags |= FB_ACCELF_TEXT;
	NVTRACE_LEAVE();
}


static void riva_get_EDID(struct fb_info *info, struct pci_dev *pdev)
{
	struct riva_par *par;
	int i;

	NVTRACE_ENTER();
#ifdef CONFIG_PPC_OF
	if (!riva_get_EDID_OF(info, pdev))
		printk("rivafb: could not retrieve EDID from OF\n");
#else
	/* XXX use other methods later */
#ifdef CONFIG_FB_RIVA_I2C

	par = (struct riva_par *) info->par;
	riva_create_i2c_busses(par);
	for (i = par->bus; i >= 1; i--) {
		riva_probe_i2c_connector(par, i, &par->EDID);
		if (par->EDID) {
			printk("rivafb: Found EDID Block from BUS %i\n", i);
			break;
		}
	}
	riva_delete_i2c_busses(par);
#endif
#endif
	NVTRACE_LEAVE();
}


static void riva_get_edidinfo(struct fb_info *info)
{
	struct fb_var_screeninfo *var = &rivafb_default_var;
	struct riva_par *par = (struct riva_par *) info->par;

	fb_edid_to_monspecs(par->EDID, &info->monspecs);
	riva_update_default_var(var, info);

	/* if user specified flatpanel, we respect that */
	if (info->monspecs.input & FB_DISP_DDI)
		par->FlatPanel = 1;
}

/* ------------------------------------------------------------------------- *
 *
 * PCI bus
 *
 * ------------------------------------------------------------------------- */

static int __devinit rivafb_probe(struct pci_dev *pd,
			     	const struct pci_device_id *ent)
{
	struct riva_chip_info *rci = &riva_chip_info[ent->driver_data];
	struct riva_par *default_par;
	struct fb_info *info;

	NVTRACE_ENTER();
	assert(pd != NULL);
	assert(rci != NULL);

	info = kmalloc(sizeof(struct fb_info), GFP_KERNEL);
	if (!info)
		goto err_out;

	default_par = kmalloc(sizeof(struct riva_par), GFP_KERNEL);
	if (!default_par)
		goto err_out_kfree;

	memset(info, 0, sizeof(struct fb_info));
	memset(default_par, 0, sizeof(struct riva_par));
	default_par->pdev = pd;

	info->pixmap.addr = kmalloc(64 * 1024, GFP_KERNEL);
	if (info->pixmap.addr == NULL)
		goto err_out_kfree1;
	memset(info->pixmap.addr, 0, 64 * 1024);

	if (pci_enable_device(pd)) {
		printk(KERN_ERR PFX "cannot enable PCI device\n");
		goto err_out_enable;
	}

	if (pci_request_regions(pd, "rivafb")) {
		printk(KERN_ERR PFX "cannot request PCI regions\n");
		goto err_out_request;
	}

	strcat(rivafb_fix.id, rci->name);
	default_par->riva.Architecture = rci->arch_rev;

	default_par->Chipset = (pd->vendor << 16) | pd->device;
	printk(KERN_INFO PFX "nVidia device/chipset %X\n",default_par->Chipset);
	
	default_par->FlatPanel = flatpanel;
	if (flatpanel == 1)
		printk(KERN_INFO PFX "flatpanel support enabled\n");
	default_par->forceCRTC = forceCRTC;
	
	rivafb_fix.mmio_len = pci_resource_len(pd, 0);
	rivafb_fix.smem_len = pci_resource_len(pd, 1);

	{
		/* enable IO and mem if not already done */
		unsigned short cmd;

		pci_read_config_word(pd, PCI_COMMAND, &cmd);
		cmd |= (PCI_COMMAND_IO | PCI_COMMAND_MEMORY);
		pci_write_config_word(pd, PCI_COMMAND, cmd);
	}
	
	rivafb_fix.mmio_start = pci_resource_start(pd, 0);
	rivafb_fix.smem_start = pci_resource_start(pd, 1);

	default_par->ctrl_base = ioremap(rivafb_fix.mmio_start,
					 rivafb_fix.mmio_len);
	if (!default_par->ctrl_base) {
		printk(KERN_ERR PFX "cannot ioremap MMIO base\n");
		goto err_out_free_base0;
	}

	info->par = default_par;

	switch (default_par->riva.Architecture) {
	case NV_ARCH_03:
		/* Riva128's PRAMIN is in the "framebuffer" space
		 * Since these cards were never made with more than 8 megabytes
		 * we can safely allocate this separately.
		 */
		default_par->riva.PRAMIN = ioremap(rivafb_fix.smem_start + 0x00C00000, 0x00008000);
		if (!default_par->riva.PRAMIN) {
			printk(KERN_ERR PFX "cannot ioremap PRAMIN region\n");
			goto err_out_free_nv3_pramin;
		}
		rivafb_fix.accel = FB_ACCEL_NV3;
		default_par->bus = 1;
		break;
	case NV_ARCH_04:
	case NV_ARCH_10:
	case NV_ARCH_20:
		default_par->riva.PCRTC0 = (unsigned *)(default_par->ctrl_base + 0x00600000);
		default_par->riva.PRAMIN = (unsigned *)(default_par->ctrl_base + 0x00710000);
		rivafb_fix.accel = FB_ACCEL_NV4;
		default_par->bus = 2;
		break;
	}

	riva_common_setup(default_par);

	if (default_par->riva.Architecture == NV_ARCH_03) {
		default_par->riva.PCRTC = default_par->riva.PCRTC0 = default_par->riva.PGRAPH;
	}

	rivafb_fix.smem_len = riva_get_memlen(default_par) * 1024;
	default_par->dclk_max = riva_get_maxdclk(default_par) * 1000;

	info->screen_base = ioremap(rivafb_fix.smem_start,
				    rivafb_fix.smem_len);
	if (!info->screen_base) {
		printk(KERN_ERR PFX "cannot ioremap FB base\n");
		goto err_out_free_base1;
	}

#ifdef CONFIG_MTRR
	if (!nomtrr) {
		default_par->mtrr.vram = mtrr_add(rivafb_fix.smem_start,
					   	  rivafb_fix.smem_len,
					    	  MTRR_TYPE_WRCOMB, 1);
		if (default_par->mtrr.vram < 0) {
			printk(KERN_ERR PFX "unable to setup MTRR\n");
		} else {
			default_par->mtrr.vram_valid = 1;
			/* let there be speed */
			printk(KERN_INFO PFX "RIVA MTRR set to ON\n");
		}
	}
#endif /* CONFIG_MTRR */

	info->fbops = &riva_fb_ops;
	info->fix = rivafb_fix;
	riva_get_EDID(info, pd);
	riva_get_edidinfo(info);

	if (riva_set_fbinfo(info) < 0) {
		printk(KERN_ERR PFX "error setting initial video mode\n");
		goto err_out_iounmap_fb;
	}

	if (register_framebuffer(info) < 0) {
		printk(KERN_ERR PFX
			"error registering riva framebuffer\n");
		goto err_out_iounmap_fb;
	}

	pci_set_drvdata(pd, info);

	printk(KERN_INFO PFX
		"PCI nVidia NV%x framebuffer ver %s (%s, %dMB @ 0x%lX)\n",
		default_par->riva.Architecture,
		RIVAFB_VERSION,
		info->fix.id,
		info->fix.smem_len / (1024 * 1024),
		info->fix.smem_start);
#ifdef CONFIG_PMAC_BACKLIGHT
	if (default_par->FlatPanel && _machine == _MACH_Pmac)
	register_backlight_controller(&riva_backlight_controller,
						default_par, "mnca");
#endif
	NVTRACE_LEAVE();
	return 0;

err_out_iounmap_fb:
	iounmap(info->screen_base);
err_out_free_base1:
	if (default_par->riva.Architecture == NV_ARCH_03) 
		iounmap((caddr_t)default_par->riva.PRAMIN);
err_out_free_nv3_pramin:
	iounmap(default_par->ctrl_base);
err_out_free_base0:
	pci_release_regions(pd);
err_out_request:
	pci_disable_device(pd);
err_out_enable:
	kfree(info->pixmap.addr);
err_out_kfree1:
	kfree(default_par);
err_out_kfree:
	kfree(info);
err_out:
	return -ENODEV;
}

static void __exit rivafb_remove(struct pci_dev *pd)
{
	struct fb_info *info = pci_get_drvdata(pd);
	struct riva_par *par = (struct riva_par *) info->par;
	
	NVTRACE_ENTER();
	if (!info)
		return;

	unregister_framebuffer(info);
#ifdef CONFIG_MTRR
	if (par->mtrr.vram_valid)
		mtrr_del(par->mtrr.vram, info->fix.smem_start,
			 info->fix.smem_len);
#endif /* CONFIG_MTRR */

	iounmap(par->ctrl_base);
	iounmap(info->screen_base);
	if (par->riva.Architecture == NV_ARCH_03)
		iounmap((caddr_t)par->riva.PRAMIN);
	pci_release_regions(pd);
	pci_disable_device(pd);
	fb_destroy_modedb(info->monspecs.modedb);
	kfree(info->pixmap.addr);
	kfree(par);
	kfree(info);
	pci_set_drvdata(pd, NULL);
	NVTRACE_LEAVE();
}

/* ------------------------------------------------------------------------- *
 *
 * initialization
 *
 * ------------------------------------------------------------------------- */

#ifndef MODULE
int __init rivafb_setup(char *options)
{
	char *this_opt;

	NVTRACE_ENTER();
	if (!options || !*options)
		return 0;

	while ((this_opt = strsep(&options, ",")) != NULL) {
		if (!strncmp(this_opt, "forceCRTC", 9)) {
			char *p;
			
			p = this_opt + 9;
			if (!*p || !*(++p)) continue; 
			forceCRTC = *p - '0';
			if (forceCRTC < 0 || forceCRTC > 1) 
				forceCRTC = -1;
		} else if (!strncmp(this_opt, "flatpanel", 9)) {
			flatpanel = 1;
#ifdef CONFIG_MTRR
		} else if (!strncmp(this_opt, "nomtrr", 6)) {
			nomtrr = 1;
#endif
		} else if (!strncmp(this_opt, "strictmode", 10)) {
			strictmode = 1;
		} else
			mode_option = this_opt;
	}
	NVTRACE_LEAVE();
	return 0;
}
#endif /* !MODULE */

static struct pci_driver rivafb_driver = {
	.name		= "rivafb",
	.id_table	= rivafb_pci_tbl,
	.probe		= rivafb_probe,
	.remove		= __exit_p(rivafb_remove),
};



/* ------------------------------------------------------------------------- *
 *
 * modularization
 *
 * ------------------------------------------------------------------------- */

int __init rivafb_init(void)
{
	if (pci_register_driver(&rivafb_driver) > 0)
		return 0;
	pci_unregister_driver(&rivafb_driver);
	return -ENODEV;
}


#ifdef MODULE
static void __exit rivafb_exit(void)
{
	pci_unregister_driver(&rivafb_driver);
}

module_init(rivafb_init);
module_exit(rivafb_exit);

MODULE_PARM(flatpanel, "i");
MODULE_PARM_DESC(flatpanel, "Enables experimental flat panel support for some chipsets. (0 or 1=enabled) (default=0)");
MODULE_PARM(forceCRTC, "i");
MODULE_PARM_DESC(forceCRTC, "Forces usage of a particular CRTC in case autodetection fails. (0 or 1) (default=autodetect)");

#ifdef CONFIG_MTRR
MODULE_PARM(nomtrr, "i");
MODULE_PARM_DESC(nomtrr, "Disables MTRR support (0 or 1=disabled) (default=0)");
MODULE_PARM(strictmode, "i");
MODULE_PARM_DESC(strictmode, "Only use video modes from EDID");
#endif
#endif /* MODULE */

MODULE_AUTHOR("Ani Joshi, maintainer");
MODULE_DESCRIPTION("Framebuffer driver for nVidia Riva 128, TNT, TNT2, and the GeForce series");
MODULE_LICENSE("GPL");
