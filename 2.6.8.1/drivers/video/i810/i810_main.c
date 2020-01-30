 /*-*- linux-c -*-
 *  linux/drivers/video/i810_main.c -- Intel 810 frame buffer device
 *
 *      Copyright (C) 2001 Antonino Daplas<adaplas@pol.net>
 *      All Rights Reserved      
 *
 *      Contributors:
 *         Michael Vogt <mvogt@acm.org> - added support for Intel 815 chipsets
 *                                        and enabling the power-on state of 
 *                                        external VGA connectors for 
 *                                        secondary displays
 *
 *         Fredrik Andersson <krueger@shell.linux.se> - alpha testing of
 *                                        the VESA GTF
 *
 *         Brad Corrion <bcorrion@web-co.com> - alpha testing of customized
 *                                        timings support
 *
 *	The code framework is a modification of vfb.c by Geert Uytterhoeven.
 *      DotClock and PLL calculations are partly based on i810_driver.c 
 *              in xfree86 v4.0.3 by Precision Insight.
 *      Watermark calculation and tables are based on i810_wmark.c 
 *              in xfre86 v4.0.3 by Precision Insight.  Slight modifications 
 *              only to allow for integer operations instead of floating point.
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/module.h>
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/slab.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/pci_ids.h>
#include <linux/resource.h>
#include <linux/unistd.h>

#include <asm/io.h>
#include <asm/div64.h>

#ifdef CONFIG_MTRR
#include <asm/mtrr.h>
#endif 

#include <asm/page.h>

#include "i810_regs.h"
#include "i810.h"
#include "i810_main.h"

/* PCI */
static const char *i810_pci_list[] __devinitdata = {
	"Intel(R) 810 Framebuffer Device"                                 ,
	"Intel(R) 810-DC100 Framebuffer Device"                           ,
	"Intel(R) 810E Framebuffer Device"                                ,
	"Intel(R) 815 (Internal Graphics 100Mhz FSB) Framebuffer Device"  ,
	"Intel(R) 815 (Internal Graphics only) Framebuffer Device"        ,
	"Intel(R) 815 (Internal Graphics with AGP) Framebuffer Device"
};

static struct pci_device_id i810fb_pci_tbl[] = {
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82810_IG1,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82810_IG3,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, 1  },
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82810E_IG,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, 2 },
	/* mvo: added i815 PCI-ID */
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82815_100,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, 3 },
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82815_NOAGP,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, 4 },
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82815_CGC,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, 5 },
	{ 0 },
};

static struct pci_driver i810fb_driver = {
	.name     =	"i810fb",
	.id_table =	i810fb_pci_tbl,
	.probe    =	i810fb_init_pci,
	.remove   =	__exit_p(i810fb_remove_pci),
	.suspend  =     i810fb_suspend,
	.resume   =     i810fb_resume,
};

static int vram       __initdata = 4;
static int bpp        __initdata = 8;
static int mtrr       __initdata = 0;
static int accel      __initdata = 0;
static int hsync1     __initdata = 0;
static int hsync2     __initdata = 0;
static int vsync1     __initdata = 0;
static int vsync2     __initdata = 0;
static int xres       __initdata = 640;
static int yres       __initdata = 480;
static int vyres      __initdata = 0;
static int sync       __initdata = 0;
static int ext_vga    __initdata = 0;
static int dcolor     __initdata = 0;

/*------------------------------------------------------------*/

/**************************************************************
 *                Hardware Low Level Routines                 *
 **************************************************************/

/**
 * i810_screen_off - turns off/on display
 * @mmio: address of register space
 * @mode: on or off
 *
 * DESCRIPTION:
 * Blanks/unblanks the display
 */
static void i810_screen_off(u8 *mmio, u8 mode)
{
	u32 count = WAIT_COUNT;
	u8 val;

	i810_writeb(SR_INDEX, mmio, SR01);
	val = i810_readb(SR_DATA, mmio);
	val = (mode == OFF) ? val | SCR_OFF :
		val & ~SCR_OFF;

	while((i810_readw(DISP_SL, mmio) & 0xFFF) && count--);
	i810_writeb(SR_INDEX, mmio, SR01);
	i810_writeb(SR_DATA, mmio, val);
}

/**
 * i810_dram_off - turns off/on dram refresh
 * @mmio: address of register space
 * @mode: on or off
 *
 * DESCRIPTION:
 * Turns off DRAM refresh.  Must be off for only 2 vsyncs
 * before data becomes corrupt
 */
static void i810_dram_off(u8 *mmio, u8 mode)
{
	u8 val;

	val = i810_readb(DRAMCH, mmio);
	val &= DRAM_OFF;
	val = (mode == OFF) ? val : val | DRAM_ON;
	i810_writeb(DRAMCH, mmio, val);
}

/**
 * i810_protect_regs - allows rw/ro mode of certain VGA registers
 * @mmio: address of register space
 * @mode: protect/unprotect
 *
 * DESCRIPTION:
 * The IBM VGA standard allows protection of certain VGA registers.  
 * This will  protect or unprotect them. 
 */
static void i810_protect_regs(u8 *mmio, int mode)
{
	u8 reg;

	i810_writeb(CR_INDEX_CGA, mmio, CR11);
	reg = i810_readb(CR_DATA_CGA, mmio);
	reg = (mode == OFF) ? reg & ~0x80 :
		reg | 0x80;
 		
	i810_writeb(CR_INDEX_CGA, mmio, CR11);
	i810_writeb(CR_DATA_CGA, mmio, reg);
}

/**
 * i810_load_pll - loads values for the hardware PLL clock
 * @par: pointer to i810fb_par structure
 *
 * DESCRIPTION:
 * Loads the P, M, and N registers.  
 */
static void i810_load_pll(struct i810fb_par *par)
{
	u32 tmp1, tmp2;
	u8 *mmio = par->mmio_start_virtual;
	
	tmp1 = par->regs.M | par->regs.N << 16;
	tmp2 = i810_readl(DCLK_2D, mmio);
	tmp2 &= ~MN_MASK;
	i810_writel(DCLK_2D, mmio, tmp1 | tmp2);
	
	tmp1 = par->regs.P;
	tmp2 = i810_readl(DCLK_0DS, mmio);
	tmp2 &= ~(P_OR << 16);
	i810_writel(DCLK_0DS, mmio, (tmp1 << 16) | tmp2);

	i810_writeb(MSR_WRITE, mmio, par->regs.msr | 0xC8 | 1);

}

/**
 * i810_load_vga - load standard VGA registers
 * @par: pointer to i810fb_par structure
 *
 * DESCRIPTION:
 * Load values to VGA registers
 */
static void i810_load_vga(struct i810fb_par *par)
{	
	u8 *mmio = par->mmio_start_virtual;

	/* interlace */
	i810_writeb(CR_INDEX_CGA, mmio, CR70);
	i810_writeb(CR_DATA_CGA, mmio, par->interlace);

	i810_writeb(CR_INDEX_CGA, mmio, CR00);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr00);
	i810_writeb(CR_INDEX_CGA, mmio, CR01);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr01);
	i810_writeb(CR_INDEX_CGA, mmio, CR02);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr02);
	i810_writeb(CR_INDEX_CGA, mmio, CR03);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr03);
	i810_writeb(CR_INDEX_CGA, mmio, CR04);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr04);
	i810_writeb(CR_INDEX_CGA, mmio, CR05);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr05);
	i810_writeb(CR_INDEX_CGA, mmio, CR06);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr06);
	i810_writeb(CR_INDEX_CGA, mmio, CR09);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr09);
	i810_writeb(CR_INDEX_CGA, mmio, CR10);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr10);
	i810_writeb(CR_INDEX_CGA, mmio, CR11);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr11);
	i810_writeb(CR_INDEX_CGA, mmio, CR12);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr12);
	i810_writeb(CR_INDEX_CGA, mmio, CR15);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr15);
	i810_writeb(CR_INDEX_CGA, mmio, CR16);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr16);
}

/**
 * i810_load_vgax - load extended VGA registers
 * @par: pointer to i810fb_par structure
 *
 * DESCRIPTION:
 * Load values to extended VGA registers
 */
static void i810_load_vgax(struct i810fb_par *par)
{
	u8 *mmio = par->mmio_start_virtual;

	i810_writeb(CR_INDEX_CGA, mmio, CR30);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr30);
	i810_writeb(CR_INDEX_CGA, mmio, CR31);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr31);
	i810_writeb(CR_INDEX_CGA, mmio, CR32);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr32);
	i810_writeb(CR_INDEX_CGA, mmio, CR33);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr33);
	i810_writeb(CR_INDEX_CGA, mmio, CR35);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr35);
	i810_writeb(CR_INDEX_CGA, mmio, CR39);
	i810_writeb(CR_DATA_CGA, mmio, par->regs.cr39);
}

/**
 * i810_load_2d - load grahics registers
 * @par: pointer to i810fb_par structure
 *
 * DESCRIPTION:
 * Load values to graphics registers
 */
static void i810_load_2d(struct i810fb_par *par)
{
	u32 tmp;
	u8 tmp8, *mmio = par->mmio_start_virtual;

  	i810_writel(FW_BLC, mmio, par->watermark); 
	tmp = i810_readl(PIXCONF, mmio);
	tmp |= 1 | 1 << 20;
	i810_writel(PIXCONF, mmio, tmp);

	i810_writel(OVRACT, mmio, par->ovract);

	i810_writeb(GR_INDEX, mmio, GR10);
	tmp8 = i810_readb(GR_DATA, mmio);
	tmp8 |= 2;
	i810_writeb(GR_INDEX, mmio, GR10);
	i810_writeb(GR_DATA, mmio, tmp8);
}	

/**
 * i810_hires - enables high resolution mode
 * @mmio: address of register space
 */
static void i810_hires(u8 *mmio)
{
	u8 val;
	
	i810_writeb(CR_INDEX_CGA, mmio, CR80);
	val = i810_readb(CR_DATA_CGA, mmio);
	i810_writeb(CR_INDEX_CGA, mmio, CR80);
	i810_writeb(CR_DATA_CGA, mmio, val | 1);
}

/**
 * i810_load_pitch - loads the characters per line of the display
 * @par: pointer to i810fb_par structure
 *
 * DESCRIPTION:
 * Loads the characters per line
 */	
static void i810_load_pitch(struct i810fb_par *par)
{
	u32 tmp, pitch;
	u8 val, *mmio = par->mmio_start_virtual;
			
	pitch = par->pitch >> 3;
	i810_writeb(SR_INDEX, mmio, SR01);
	val = i810_readb(SR_DATA, mmio);
	val &= 0xE0;
	val |= 1 | 1 << 2;
	i810_writeb(SR_INDEX, mmio, SR01);
	i810_writeb(SR_DATA, mmio, val);

	tmp = pitch & 0xFF;
	i810_writeb(CR_INDEX_CGA, mmio, CR13);
	i810_writeb(CR_DATA_CGA, mmio, (u8) tmp);
	
	tmp = pitch >> 8;
	i810_writeb(CR_INDEX_CGA, mmio, CR41);
	val = i810_readb(CR_DATA_CGA, mmio) & ~0x0F;
	i810_writeb(CR_INDEX_CGA, mmio, CR41);
	i810_writeb(CR_DATA_CGA, mmio, (u8) tmp | val);
}

/**
 * i810_load_color - loads the color depth of the display
 * @par: pointer to i810fb_par structure
 *
 * DESCRIPTION:
 * Loads the color depth of the display and the graphics engine
 */
static void i810_load_color(struct i810fb_par *par)
{
	u8 *mmio = par->mmio_start_virtual;
	u32 reg1;
	u16 reg2;
	reg1 = i810_readl(PIXCONF, mmio) & ~(0xF0000 | 1 << 27);
	reg2 = i810_readw(BLTCNTL, mmio) & ~0x30;

	reg1 |= 0x8000 | par->pixconf;
	reg2 |= par->bltcntl;
	i810_writel(PIXCONF, mmio, reg1);
	i810_writew(BLTCNTL, mmio, reg2);
}

/**
 * i810_load_regs - loads all registers for the mode
 * @par: pointer to i810fb_par structure
 * 
 * DESCRIPTION:
 * Loads registers
 */
static void i810_load_regs(struct i810fb_par *par)
{
	u8 *mmio = par->mmio_start_virtual;

	i810_screen_off(mmio, OFF);
	i810_protect_regs(mmio, OFF);
	i810_dram_off(mmio, OFF);
	i810_load_pll(par);
	i810_load_vga(par);
	i810_load_vgax(par);
	i810_dram_off(mmio, ON);	
	i810_load_2d(par);
	i810_hires(mmio);
	i810_screen_off(mmio, ON);
	i810_protect_regs(mmio, ON);
	i810_load_color(par);
	i810_load_pitch(par);
}

static void i810_write_dac(u8 regno, u8 red, u8 green, u8 blue,
			  u8 *mmio)
{
	i810_writeb(CLUT_INDEX_WRITE, mmio, regno);
	i810_writeb(CLUT_DATA, mmio, red);
	i810_writeb(CLUT_DATA, mmio, green);
	i810_writeb(CLUT_DATA, mmio, blue); 	
}

static void i810_read_dac(u8 regno, u8 *red, u8 *green, u8 *blue,
			  u8 *mmio)
{
	i810_writeb(CLUT_INDEX_READ, mmio, regno);
	*red = i810_readb(CLUT_DATA, mmio);
	*green = i810_readb(CLUT_DATA, mmio);
	*blue = i810_readb(CLUT_DATA, mmio);
}

/************************************************************
 *                   VGA State Restore                      * 
 ************************************************************/
static void i810_restore_pll(struct i810fb_par *par)
{
	u32 tmp1, tmp2;
	u8 *mmio = par->mmio_start_virtual;
	
	tmp1 = par->hw_state.dclk_2d;
	tmp2 = i810_readl(DCLK_2D, mmio);
	tmp1 &= ~MN_MASK;
	tmp2 &= MN_MASK;
	i810_writel(DCLK_2D, mmio, tmp1 | tmp2);

	tmp1 = par->hw_state.dclk_1d;
	tmp2 = i810_readl(DCLK_1D, mmio);
	tmp1 &= ~MN_MASK;
	tmp2 &= MN_MASK;
	i810_writel(DCLK_1D, mmio, tmp1 | tmp2);

	i810_writel(DCLK_0DS, mmio, par->hw_state.dclk_0ds);
}

static void i810_restore_dac(struct i810fb_par *par)
{
	u32 tmp1, tmp2;
	u8 *mmio = par->mmio_start_virtual;

	tmp1 = par->hw_state.pixconf;
	tmp2 = i810_readl(PIXCONF, mmio);
	tmp1 &= DAC_BIT;
	tmp2 &= ~DAC_BIT;
	i810_writel(PIXCONF, mmio, tmp1 | tmp2);
}

static void i810_restore_vgax(struct i810fb_par *par)
{
	u8 i, j, *mmio = par->mmio_start_virtual;
	
	for (i = 0; i < 4; i++) {
		i810_writeb(CR_INDEX_CGA, mmio, CR30+i);
		i810_writeb(CR_DATA_CGA, mmio, *(&(par->hw_state.cr30) + i));
	}
	i810_writeb(CR_INDEX_CGA, mmio, CR35);
	i810_writeb(CR_DATA_CGA, mmio, par->hw_state.cr35);
	i810_writeb(CR_INDEX_CGA, mmio, CR39);
	i810_writeb(CR_DATA_CGA, mmio, par->hw_state.cr39);
	i810_writeb(CR_INDEX_CGA, mmio, CR41);
	i810_writeb(CR_DATA_CGA, mmio, par->hw_state.cr39);

	/*restore interlace*/
	i810_writeb(CR_INDEX_CGA, mmio, CR70);
	i = par->hw_state.cr70;
	i &= INTERLACE_BIT;
	j = i810_readb(CR_DATA_CGA, mmio);
	i810_writeb(CR_INDEX_CGA, mmio, CR70);
	i810_writeb(CR_DATA_CGA, mmio, j | i);

	i810_writeb(CR_INDEX_CGA, mmio, CR80);
	i810_writeb(CR_DATA_CGA, mmio, par->hw_state.cr80);
	i810_writeb(MSR_WRITE, mmio, par->hw_state.msr);
	i810_writeb(SR_INDEX, mmio, SR01);
	i = (par->hw_state.sr01) & ~0xE0 ;
	j = i810_readb(SR_DATA, mmio) & 0xE0;
	i810_writeb(SR_INDEX, mmio, SR01);
	i810_writeb(SR_DATA, mmio, i | j);
}

static void i810_restore_vga(struct i810fb_par *par)
{
	u8 i, *mmio = par->mmio_start_virtual;
	
	for (i = 0; i < 10; i++) {
		i810_writeb(CR_INDEX_CGA, mmio, CR00 + i);
		i810_writeb(CR_DATA_CGA, mmio, *((&par->hw_state.cr00) + i));
	}
	for (i = 0; i < 8; i++) {
		i810_writeb(CR_INDEX_CGA, mmio, CR10 + i);
		i810_writeb(CR_DATA_CGA, mmio, *((&par->hw_state.cr10) + i));
	}
}

static void i810_restore_addr_map(struct i810fb_par *par)
{
	u8 tmp, *mmio = par->mmio_start_virtual;

	i810_writeb(GR_INDEX, mmio, GR10);
	tmp = i810_readb(GR_DATA, mmio);
	tmp &= ADDR_MAP_MASK;
	tmp |= par->hw_state.gr10;
	i810_writeb(GR_INDEX, mmio, GR10);
	i810_writeb(GR_DATA, mmio, tmp);
}

static void i810_restore_2d(struct i810fb_par *par)
{
	u32 tmp_long;
	u16 tmp_word;
	u8 *mmio = par->mmio_start_virtual;

	tmp_word = i810_readw(BLTCNTL, mmio);
	tmp_word &= ~(3 << 4); 
	tmp_word |= par->hw_state.bltcntl;
	i810_writew(BLTCNTL, mmio, tmp_word);
       
	i810_dram_off(mmio, OFF);
	i810_writel(PIXCONF, mmio, par->hw_state.pixconf);
	i810_dram_off(mmio, ON);

	tmp_word = i810_readw(HWSTAM, mmio);
	tmp_word &= 3 << 13;
	tmp_word |= par->hw_state.hwstam;
	i810_writew(HWSTAM, mmio, tmp_word);

	tmp_long = i810_readl(FW_BLC, mmio);
	tmp_long &= FW_BLC_MASK;
	tmp_long |= par->hw_state.fw_blc;
	i810_writel(FW_BLC, mmio, tmp_long);

	i810_writel(HWS_PGA, mmio, par->hw_state.hws_pga); 
	i810_writew(IER, mmio, par->hw_state.ier);
	i810_writew(IMR, mmio, par->hw_state.imr);
	i810_writel(DPLYSTAS, mmio, par->hw_state.dplystas);
}

static void i810_restore_vga_state(struct i810fb_par *par)
{
	u8 *mmio = par->mmio_start_virtual;

	i810_screen_off(mmio, OFF);
	i810_protect_regs(mmio, OFF);
	i810_dram_off(mmio, OFF);
	i810_restore_pll(par);
	i810_restore_dac(par);
	i810_restore_vga(par);
	i810_restore_vgax(par);
	i810_restore_addr_map(par);
	i810_dram_off(mmio, ON);
	i810_restore_2d(par);
	i810_screen_off(mmio, ON);
	i810_protect_regs(mmio, ON);
}

/***********************************************************************
 *                         VGA State Save                              *
 ***********************************************************************/

static void i810_save_vgax(struct i810fb_par *par)
{
	u8 i, *mmio = par->mmio_start_virtual;

	for (i = 0; i < 4; i++) {
		i810_writeb(CR_INDEX_CGA, mmio, CR30 + i);
		*(&(par->hw_state.cr30) + i) = i810_readb(CR_DATA_CGA, mmio);
	}
	i810_writeb(CR_INDEX_CGA, mmio, CR35);
	par->hw_state.cr35 = i810_readb(CR_DATA_CGA, mmio);
	i810_writeb(CR_INDEX_CGA, mmio, CR39);
	par->hw_state.cr39 = i810_readb(CR_DATA_CGA, mmio);
	i810_writeb(CR_INDEX_CGA, mmio, CR41);
	par->hw_state.cr41 = i810_readb(CR_DATA_CGA, mmio);
	i810_writeb(CR_INDEX_CGA, mmio, CR70);
	par->hw_state.cr70 = i810_readb(CR_DATA_CGA, mmio);	
	par->hw_state.msr = i810_readb(MSR_READ, mmio);
	i810_writeb(CR_INDEX_CGA, mmio, CR80);
	par->hw_state.cr80 = i810_readb(CR_DATA_CGA, mmio);
	i810_writeb(SR_INDEX, mmio, SR01);
	par->hw_state.sr01 = i810_readb(SR_DATA, mmio);
}

static void i810_save_vga(struct i810fb_par *par)
{
	u8 i, *mmio = par->mmio_start_virtual;

	for (i = 0; i < 10; i++) {
		i810_writeb(CR_INDEX_CGA, mmio, CR00 + i);
		*((&par->hw_state.cr00) + i) = i810_readb(CR_DATA_CGA, mmio);
	}
	for (i = 0; i < 8; i++) {
		i810_writeb(CR_INDEX_CGA, mmio, CR10 + i);
		*((&par->hw_state.cr10) + i) = i810_readb(CR_DATA_CGA, mmio);
	}
}

static void i810_save_2d(struct i810fb_par *par)
{
	u8 *mmio = par->mmio_start_virtual;

	par->hw_state.dclk_2d = i810_readl(DCLK_2D, mmio);
	par->hw_state.dclk_1d = i810_readl(DCLK_1D, mmio);
	par->hw_state.dclk_0ds = i810_readl(DCLK_0DS, mmio);
	par->hw_state.pixconf = i810_readl(PIXCONF, mmio);
	par->hw_state.fw_blc = i810_readl(FW_BLC, mmio);
	par->hw_state.bltcntl = i810_readw(BLTCNTL, mmio);
	par->hw_state.hwstam = i810_readw(HWSTAM, mmio); 
	par->hw_state.hws_pga = i810_readl(HWS_PGA, mmio); 
	par->hw_state.ier = i810_readw(IER, mmio);
	par->hw_state.imr = i810_readw(IMR, mmio);
	par->hw_state.dplystas = i810_readl(DPLYSTAS, mmio);
}

static void i810_save_vga_state(struct i810fb_par *par)
{
	i810_save_vga(par);
	i810_save_vgax(par);
	i810_save_2d(par);
}

/************************************************************
 *                    Helpers                               * 
 ************************************************************/
/**
 * get_line_length - calculates buffer pitch in bytes
 * @par: pointer to i810fb_par structure
 * @xres_virtual: virtual resolution of the frame
 * @bpp: bits per pixel
 *
 * DESCRIPTION:
 * Calculates buffer pitch in bytes.  
 */
u32 get_line_length(struct i810fb_par *par, int xres_virtual, int bpp)
{
   	u32 length;
	
	length = xres_virtual*bpp;
	length = (length+31)&-32;
	length >>= 3;
	return length;
}

/**
 * i810_calc_dclk - calculates the P, M, and N values of a pixelclock value
 * @freq: target pixelclock in picoseconds
 * @m: where to write M register
 * @n: where to write N register
 * @p: where to write P register
 *
 * DESCRIPTION:
 * Based on the formula Freq_actual = (4*M*Freq_ref)/(N^P)
 * Repeatedly computes the Freq until the actual Freq is equal to
 * the target Freq or until the loop count is zero.  In the latter
 * case, the actual frequency nearest the target will be used.
 */
static void i810_calc_dclk(u32 freq, u32 *m, u32 *n, u32 *p)
{
	u32 m_reg, n_reg, p_divisor, n_target_max;
	u32 m_target, n_target, p_target, n_best, m_best, mod;
	u32 f_out, target_freq, diff = 0, mod_min, diff_min;

	diff_min = mod_min = 0xFFFFFFFF;
	n_best = m_best = m_target = f_out = 0;

	target_freq =  freq;
	n_target_max = 30;

	/*
	 * find P such that target freq is 16x reference freq (Hz). 
	 */
	p_divisor = 1;
	p_target = 0;
	while(!((1000000 * p_divisor)/(16 * 24 * target_freq)) && 
	      p_divisor <= 32) {
		p_divisor <<= 1;
		p_target++;
	}

	n_reg = m_reg = n_target = 3;	
	while (diff_min && mod_min && (n_target < n_target_max)) {
		f_out = (p_divisor * n_reg * 1000000)/(4 * 24 * m_reg);
		mod = (p_divisor * n_reg * 1000000) % (4 * 24 * m_reg);
		m_target = m_reg;
		n_target = n_reg;
		if (f_out <= target_freq) {
			n_reg++;
			diff = target_freq - f_out;
		} else {
			m_reg++;
			diff = f_out - target_freq;
		}

		if (diff_min > diff) {
			diff_min = diff;
			n_best = n_target;
			m_best = m_target;
		}		 

		if (!diff && mod_min > mod) {
			mod_min = mod;
			n_best = n_target;
			m_best = m_target;
		}
	} 
	if (m) *m = (m_best - 2) & 0x3FF;
	if (n) *n = (n_best - 2) & 0x3FF;
	if (p) *p = (p_target << 4);
}

/*************************************************************
 *                Hardware Cursor Routines                   *
 *************************************************************/

/**
 * i810_enable_cursor - show or hide the hardware cursor
 * @mmio: address of register space
 * @mode: show (1) or hide (0)
 *
 * Description:
 * Shows or hides the hardware cursor
 */
void i810_enable_cursor(u8 *mmio, int mode)
{
	u32 temp;
	
	temp = i810_readl(PIXCONF, mmio);
	temp = (mode == ON) ? temp | CURSOR_ENABLE_MASK :
		temp & ~CURSOR_ENABLE_MASK;

	i810_writel(PIXCONF, mmio, temp);
}

static void i810_reset_cursor_image(struct i810fb_par *par)
{
	u8 *addr = par->cursor_heap.virtual;
	int i, j;

	for (i = 64; i--; ) {
		for (j = 0; j < 8; j++) {             
			i810_writeb(j, addr, 0xff);   
			i810_writeb(j+8, addr, 0x00); 
		}	
		addr +=16;
	}
}

static void i810_load_cursor_image(int width, int height, u8 *data,
				   struct i810fb_par *par)
{
	u8 *addr = par->cursor_heap.virtual;
	int i, j, w = width/8;
	int mod = width % 8, t_mask, d_mask;
	
	t_mask = 0xff >> mod;
	d_mask = ~(0xff >> mod); 
	for (i = height; i--; ) {
		for (j = 0; j < w; j++) {
			i810_writeb(j+0, addr, 0x00);
			i810_writeb(j+8, addr, *data++);
		}
		if (mod) {
			i810_writeb(j+0, addr, t_mask);
			i810_writeb(j+8, addr, *data++ & d_mask);
		}
		addr += 16;
	}
}

static void i810_load_cursor_colors(int fg, int bg, struct fb_info *info)
{
	struct i810fb_par *par = (struct i810fb_par *) info->par;
	u8 *mmio = par->mmio_start_virtual, temp;
	u8 red, green, blue, trans;

	i810fb_getcolreg(bg, &red, &green, &blue, &trans, info);

	temp = i810_readb(PIXCONF1, mmio);
	i810_writeb(PIXCONF1, mmio, temp | EXTENDED_PALETTE);

	i810_write_dac(4, red, green, blue, mmio);

	i810_writeb(PIXCONF1, mmio, temp);

	i810fb_getcolreg(fg, &red, &green, &blue, &trans, info);
	temp = i810_readb(PIXCONF1, mmio);
	i810_writeb(PIXCONF1, mmio, temp | EXTENDED_PALETTE);

	i810_write_dac(5, red, green, blue, mmio);

	i810_writeb(PIXCONF1, mmio, temp);
}

/**
 * i810_init_cursor - initializes the cursor
 * @par: pointer to i810fb_par structure
 *
 * DESCRIPTION:
 * Initializes the cursor registers
 */
static void i810_init_cursor(struct i810fb_par *par)
{
	u8 *mmio = par->mmio_start_virtual;

	i810_enable_cursor(mmio, OFF);
	i810_writel(CURBASE, mmio, par->cursor_heap.physical);
	i810_writew(CURCNTR, mmio, COORD_ACTIVE | CURSOR_MODE_64_XOR);
}	

/*********************************************************************
 *                    Framebuffer hook helpers                       *
 *********************************************************************/
/**
 * i810_round_off -  Round off values to capability of hardware
 * @var: pointer to fb_var_screeninfo structure
 *
 * DESCRIPTION:
 * @var contains user-defined information for the mode to be set.
 * This will try modify those values to ones nearest the
 * capability of the hardware
 */
static void i810_round_off(struct fb_var_screeninfo *var)
{
	u32 xres, yres, vxres, vyres;

	/*
	 *  Presently supports only these configurations 
	 */

	xres = var->xres;
	yres = var->yres;
	vxres = var->xres_virtual;
	vyres = var->yres_virtual;

	var->bits_per_pixel += 7;
	var->bits_per_pixel &= ~7;
	
	if (var->bits_per_pixel < 8)
		var->bits_per_pixel = 8;
	if (var->bits_per_pixel > 32) 
		var->bits_per_pixel = 32;

	round_off_xres(&xres);
	if (xres < 40)
		xres = 40;
	if (xres > 2048) 
		xres = 2048;
	xres = (xres + 7) & ~7;

	if (vxres < xres) 
		vxres = xres;

	round_off_yres(&xres, &yres);
	if (yres < 1)
		yres = 1;
	if (yres >= 2048)
		yres = 2048;

	if (vyres < yres) 
		vyres = yres;

	if (var->bits_per_pixel == 32)
		var->accel_flags = 0;

	/* round of horizontal timings to nearest 8 pixels */
	var->left_margin = (var->left_margin + 4) & ~7;
	var->right_margin = (var->right_margin + 4) & ~7;
	var->hsync_len = (var->hsync_len + 4) & ~7;

	if (var->vmode & FB_VMODE_INTERLACED) {
		if (!((yres + var->upper_margin + var->vsync_len + 
		       var->lower_margin) & 1))
			var->upper_margin++;
	}
	
	var->xres = xres;
	var->yres = yres;
	var->xres_virtual = vxres;
	var->yres_virtual = vyres;
}	

/**
 * set_color_bitfields - sets rgba fields
 * @var: pointer to fb_var_screeninfo
 *
 * DESCRIPTION:
 * The length, offset and ordering  for each color field 
 * (red, green, blue)  will be set as specified 
 * by the hardware
 */  
static void set_color_bitfields(struct fb_var_screeninfo *var)
{
	switch (var->bits_per_pixel) {
	case 8:       
		var->red.offset = 0;
		var->red.length = 8;
		var->green.offset = 0;
		var->green.length = 8;
		var->blue.offset = 0;
		var->blue.length = 8;
		var->transp.offset = 0;
		var->transp.length = 0;
		break;
	case 16:
		var->green.length = (var->green.length == 5) ? 5 : 6;
		var->red.length = 5;
		var->blue.length = 5;
		var->transp.length = 6 - var->green.length;
		var->blue.offset = 0;
		var->green.offset = 5;
		var->red.offset = 5 + var->green.length;
		var->transp.offset =  (5 + var->red.offset) & 15;
		break;
	case 24:	/* RGB 888   */
	case 32:	/* RGBA 8888 */
		var->red.offset = 16;
		var->red.length = 8;
		var->green.offset = 8;
		var->green.length = 8;
		var->blue.offset = 0;
		var->blue.length = 8;
		var->transp.length = var->bits_per_pixel - 24;
		var->transp.offset = (var->transp.length) ? 24 : 0;
		break;
	}
	var->red.msb_right = 0;
	var->green.msb_right = 0;
	var->blue.msb_right = 0;
	var->transp.msb_right = 0;
}

/**
 * i810_check_params - check if contents in var are valid
 * @var: pointer to fb_var_screeninfo
 * @info: pointer to fb_info
 *
 * DESCRIPTION:
 * This will check if the framebuffer size is sufficient 
 * for the current mode and if the user's monitor has the 
 * required specifications to display the current mode.
 */
static int i810_check_params(struct fb_var_screeninfo *var, 
			     struct fb_info *info)
{
	struct i810fb_par *par = (struct i810fb_par *) info->par;
	int line_length, vidmem;
	u32 xres, yres, vxres, vyres;

	xres = var->xres;
	yres = var->yres;
	vxres = var->xres_virtual;
	vyres = var->yres_virtual;

	/*
	 *  Memory limit
	 */
	line_length = get_line_length(par, vxres, 
				      var->bits_per_pixel);

	vidmem = line_length*vyres;
	if (vidmem > par->fb.size) {
		vyres = par->fb.size/line_length;
		if (vyres < yres) {
			vyres = yres;
			vxres = par->fb.size/vyres;
			vxres /= var->bits_per_pixel >> 3;
			line_length = get_line_length(par, vxres, 
						      var->bits_per_pixel);
			vidmem = line_length * yres;
			if (vxres < xres) {
				printk("i810fb: required video memory, "
				       "%d bytes, for %dx%d-%d (virtual) "
				       "is out of range\n", 
				       vidmem, vxres, vyres, 
				       var->bits_per_pixel);
				return -ENOMEM;
			}
		}
	}
	/*
	 * Monitor limit
	 */
	switch (var->bits_per_pixel) {
	case 8:
		info->monspecs.dclkmax = 234000000;
		break;
	case 16:
		info->monspecs.dclkmax = 229000000;
		break;
	case 24:
	case 32:
		info->monspecs.dclkmax = 204000000;
		break;
	}
	info->monspecs.dclkmin = 15000000;

	if (fb_validate_mode(var, info)) {
		if (fb_get_mode(FB_MAXTIMINGS, 0, var, info))
			return -EINVAL;
	}
	
	var->xres = xres;
	var->yres = yres;
	var->xres_virtual = vxres;
	var->yres_virtual = vyres;
	return 0;
}	

/**
 * encode_fix - fill up fb_fix_screeninfo structure
 * @fix: pointer to fb_fix_screeninfo
 * @info: pointer to fb_info
 *
 * DESCRIPTION:
 * This will set up parameters that are unmodifiable by the user.
 */
static int encode_fix(struct fb_fix_screeninfo *fix, struct fb_info *info)
{
	struct i810fb_par *par = (struct i810fb_par *) info->par;

    	memset(fix, 0, sizeof(struct fb_fix_screeninfo));

    	strcpy(fix->id, "I810");
    	fix->smem_start = par->fb.physical;
    	fix->smem_len = par->fb.size;
    	fix->type = FB_TYPE_PACKED_PIXELS;
    	fix->type_aux = 0;
	fix->xpanstep = 8;
	fix->ypanstep = 1;

    	switch (info->var.bits_per_pixel) {
	case 8:
	    	fix->visual = FB_VISUAL_PSEUDOCOLOR;
	    	break;
	case 16:
	case 24:
	case 32:
		if (info->var.nonstd)
			fix->visual = FB_VISUAL_DIRECTCOLOR;
		else
			fix->visual = FB_VISUAL_TRUECOLOR;
	    	break;
	default:
		return -EINVAL;
	}
    	fix->ywrapstep = 0;
	fix->line_length = par->pitch;
	fix->mmio_start = par->mmio_start_phys;
	fix->mmio_len = MMIO_SIZE;
	fix->accel = FB_ACCEL_I810;

	return 0;
}

/**
 * decode_var - modify par according to contents of var
 * @var: pointer to fb_var_screeninfo
 * @par: pointer to i810fb_par
 *
 * DESCRIPTION:
 * Based on the contents of @var, @par will be dynamically filled up.
 * @par contains all information necessary to modify the hardware. 
*/
static void decode_var(const struct fb_var_screeninfo *var, 
		       struct i810fb_par *par)
{
	u32 xres, yres, vxres, vyres;

	xres = var->xres;
	yres = var->yres;
	vxres = var->xres_virtual;
	vyres = var->yres_virtual;

	switch (var->bits_per_pixel) {
	case 8:
		par->pixconf = PIXCONF8;
		par->bltcntl = 0;
		par->depth = 1;
		par->blit_bpp = BPP8;
		break;
	case 16:
		if (var->green.length == 5)
			par->pixconf = PIXCONF15;
		else
			par->pixconf = PIXCONF16;
		par->bltcntl = 16;
		par->depth = 2;
		par->blit_bpp = BPP16;
		break;
	case 24:
		par->pixconf = PIXCONF24;
		par->bltcntl = 32;
		par->depth = 3;
		par->blit_bpp = BPP24;
		break;
	case 32:
		par->pixconf = PIXCONF32;
		par->bltcntl = 0;
		par->depth = 4;
		par->blit_bpp = 3 << 24;
		break;
	}
	if (var->nonstd && var->bits_per_pixel != 8)
		par->pixconf |= 1 << 27;

	i810_calc_dclk(var->pixclock, &par->regs.M, 
		       &par->regs.N, &par->regs.P);
	i810fb_encode_registers(var, par, xres, yres);

	par->watermark = i810_get_watermark(var, par);
	par->pitch = get_line_length(par, vxres, var->bits_per_pixel);
}	

/**
 * i810fb_getcolreg - gets red, green and blue values of the hardware DAC
 * @regno: DAC index
 * @red: red
 * @green: green
 * @blue: blue
 * @transp: transparency (alpha)
 * @info: pointer to fb_info
 *
 * DESCRIPTION:
 * Gets the red, green and blue values of the hardware DAC as pointed by @regno
 * and writes them to @red, @green and @blue respectively
 */
static int i810fb_getcolreg(u8 regno, u8 *red, u8 *green, u8 *blue, 
			    u8 *transp, struct fb_info *info)
{
	struct i810fb_par *par = (struct i810fb_par *) info->par;
	u8 *mmio = par->mmio_start_virtual, temp;

	if (info->fix.visual == FB_VISUAL_DIRECTCOLOR) {
		if ((info->var.green.length == 5 && regno > 31) ||
		    (info->var.green.length == 6 && regno > 63))
			return 1;
	}

	temp = i810_readb(PIXCONF1, mmio);
	i810_writeb(PIXCONF1, mmio, temp & ~EXTENDED_PALETTE);

	if (info->fix.visual == FB_VISUAL_DIRECTCOLOR && 
	    info->var.green.length == 5) 
		i810_read_dac(regno * 8, red, green, blue, mmio);

	else if (info->fix.visual == FB_VISUAL_DIRECTCOLOR && 
		 info->var.green.length == 6) {
		u8 tmp;

		i810_read_dac(regno * 8, red, &tmp, blue, mmio);
		i810_read_dac(regno * 4, &tmp, green, &tmp, mmio);
	}
	else 
		i810_read_dac(regno, red, green, blue, mmio);

    	*transp = 0;
	i810_writeb(PIXCONF1, mmio, temp);

    	return 0;
}

/****************************************************************** 
 *           Framebuffer device-specific hooks                    *
 ******************************************************************/

static int i810fb_open(struct fb_info *info, int user)
{
	struct i810fb_par *par = (struct i810fb_par *) info->par;
	u32 count = atomic_read(&par->use_count);
	
	if (count == 0) {
		memset(&par->state, 0, sizeof(struct vgastate));
		par->state.flags = VGA_SAVE_CMAP;
		par->state.vgabase = (caddr_t) par->mmio_start_virtual;
		save_vga(&par->state);

		i810_save_vga_state(par);
	}

	atomic_inc(&par->use_count);
	
	return 0;
}

static int i810fb_release(struct fb_info *info, int user)
{
	struct i810fb_par *par = (struct i810fb_par *) info->par;
	u32 count;
	
	count = atomic_read(&par->use_count);
	if (count == 0)
		return -EINVAL;

	if (count == 1) {
		i810_restore_vga_state(par);
		restore_vga(&par->state);
	}

	atomic_dec(&par->use_count);
	
	return 0;
}


static int i810fb_setcolreg(unsigned regno, unsigned red, unsigned green, 
			    unsigned blue, unsigned transp, 
			    struct fb_info *info)
{
	struct i810fb_par *par = (struct i810fb_par *) info->par;
	u8 *mmio = par->mmio_start_virtual, temp;
	int i;

 	if (regno > 255) return 1;

	if (info->fix.visual == FB_VISUAL_DIRECTCOLOR) {
		if ((info->var.green.length == 5 && regno > 31) ||
		    (info->var.green.length == 6 && regno > 63))
			return 1;
	}

	if (info->var.grayscale)
		red = green = blue = (19595 * red + 38470 * green +
				      7471 * blue) >> 16;

	temp = i810_readb(PIXCONF1, mmio);
	i810_writeb(PIXCONF1, mmio, temp & ~EXTENDED_PALETTE);

	if (info->fix.visual == FB_VISUAL_DIRECTCOLOR && 
	    info->var.green.length == 5) {
		for (i = 0; i < 8; i++) 
			i810_write_dac((u8) (regno * 8) + i, (u8) red, 
				       (u8) green, (u8) blue, mmio);
	} else if (info->fix.visual == FB_VISUAL_DIRECTCOLOR && 
		 info->var.green.length == 6) {
		u8 r, g, b;

		if (regno < 32) {
			for (i = 0; i < 8; i++) 
				i810_write_dac((u8) (regno * 8) + i,
					       (u8) red, (u8) green, 
					       (u8) blue, mmio);
		}
		i810_read_dac((u8) (regno*4), &r, &g, &b, mmio);
		for (i = 0; i < 4; i++) 
			i810_write_dac((u8) (regno*4) + i, r, (u8) green, 
				       b, mmio);
	} else if (info->fix.visual == FB_VISUAL_PSEUDOCOLOR) {
		i810_write_dac((u8) regno, (u8) red, (u8) green,
			       (u8) blue, mmio);
	}

	i810_writeb(PIXCONF1, mmio, temp);

	if (regno < 16) {
		switch (info->var.bits_per_pixel) {
		case 16:	
			if (info->fix.visual == FB_VISUAL_DIRECTCOLOR) {
				if (info->var.green.length == 5) 
					((u32 *)info->pseudo_palette)[regno] = 
						(regno << 10) | (regno << 5) |
						regno;
				else
					((u32 *)info->pseudo_palette)[regno] = 
						(regno << 11) | (regno << 5) |
						regno;
			} else {
				if (info->var.green.length == 5) {
					/* RGB 555 */
					((u32 *)info->pseudo_palette)[regno] = 
						((red & 0xf800) >> 1) |
						((green & 0xf800) >> 6) |
						((blue & 0xf800) >> 11);
				} else {
					/* RGB 565 */
					((u32 *)info->pseudo_palette)[regno] =
						(red & 0xf800) |
						((green & 0xf800) >> 5) |
						((blue & 0xf800) >> 11);
				}
			}
			break;
		case 24:	/* RGB 888 */
		case 32:	/* RGBA 8888 */
			if (info->fix.visual == FB_VISUAL_DIRECTCOLOR) 
				((u32 *)info->pseudo_palette)[regno] = 
					(regno << 16) | (regno << 8) |
					regno;
			else 
				((u32 *)info->pseudo_palette)[regno] = 
					((red & 0xff00) << 8) |
					(green & 0xff00) |
					((blue & 0xff00) >> 8);
			break;
		}
	}
	return 0;
}

static int i810fb_pan_display(struct fb_var_screeninfo *var, 
			      struct fb_info *info)
{
	struct i810fb_par *par = (struct i810fb_par *) info->par;
	u32 total;
	
	total = var->xoffset * par->depth + 
		var->yoffset * info->fix.line_length;
	i810fb_load_front(total, info);

	return 0;
}

static int i810fb_blank (int blank_mode, struct fb_info *info)
{
	struct i810fb_par *par = (struct i810fb_par *) info->par;
	u8 *mmio = par->mmio_start_virtual;
	int mode = 0, pwr, scr_off = 0;
	
	pwr = i810_readl(PWR_CLKC, mmio);

	switch(blank_mode) {
	case VESA_NO_BLANKING:
		mode = POWERON;
		pwr |= 1;
		scr_off = ON;
		break;
	case VESA_VSYNC_SUSPEND:
		mode = STANDBY;
		pwr |= 1;
		scr_off = OFF;
		break;
	case VESA_HSYNC_SUSPEND:
		mode = SUSPEND;
		pwr |= 1;
		scr_off = OFF;
		break;
	case VESA_POWERDOWN:
		mode = POWERDOWN;
		pwr &= ~1;
		scr_off = OFF;
		break;
	default:
		return -EINVAL; 
	}
	i810_screen_off(mmio, scr_off);
	i810_writel(HVSYNC, mmio, mode);
	i810_writel(PWR_CLKC, mmio, pwr);
	return 0;
}

static int i810fb_set_par(struct fb_info *info)
{
	struct i810fb_par *par = (struct i810fb_par *) info->par;

	decode_var(&info->var, par);
	i810_load_regs(par);
	i810_init_cursor(par);

	encode_fix(&info->fix, info);

	if (info->var.accel_flags && !(par->dev_flags & LOCKUP)) 
		info->pixmap.scan_align = 2;
	else 
		info->pixmap.scan_align = 1;
	
	return 0;
}

static int i810fb_check_var(struct fb_var_screeninfo *var, 
			    struct fb_info *info)
{
	int err;

	if (IS_DVT) {
		var->vmode &= ~FB_VMODE_MASK;
		var->vmode |= FB_VMODE_NONINTERLACED;
	}
	if (var->vmode & FB_VMODE_DOUBLE) {
		var->vmode &= ~FB_VMODE_MASK;
		var->vmode |= FB_VMODE_NONINTERLACED;
	}

	i810_round_off(var);
	if ((err = i810_check_params(var, info)))
		return err;

	i810fb_fill_var_timings(var);
	set_color_bitfields(var);
	return 0;
}

static int i810fb_cursor(struct fb_info *info, struct fb_cursor *cursor)
{
	struct i810fb_par *par = (struct i810fb_par *)info->par;
	u8 *mmio = par->mmio_start_virtual;	
	u8 data[64 * 8];
	
	if (!info->var.accel_flags || par->dev_flags & LOCKUP) 
		return soft_cursor(info, cursor);

	if (cursor->image.width > 64 || cursor->image.height > 64)
		return -ENXIO;

	if ((i810_readl(CURBASE, mmio) & 0xf) != par->cursor_heap.physical)
		i810_init_cursor(par);

	i810_enable_cursor(mmio, OFF);

	if (cursor->set & FB_CUR_SETHOT)
		info->cursor.hot = cursor->hot;
	
	if (cursor->set & FB_CUR_SETPOS) {
		u32 tmp;

		info->cursor.image.dx = cursor->image.dx;
		info->cursor.image.dy = cursor->image.dy;
		
		tmp = cursor->image.dx - info->var.xoffset;
		tmp |= (cursor->image.dy - info->var.yoffset) << 16;
	    
		i810_writel(CURPOS, mmio, tmp);
	}

	if (cursor->set & FB_CUR_SETSIZE) {
		info->cursor.image.height = cursor->image.height;
		info->cursor.image.width = cursor->image.width;
		i810_reset_cursor_image(par);
	}

	if (cursor->set & FB_CUR_SETCMAP) {
		info->cursor.image.fg_color = cursor->image.fg_color;
		info->cursor.image.bg_color = cursor->image.bg_color;
		i810_load_cursor_colors(cursor->image.fg_color,
					cursor->image.bg_color,
					info);
	}

	if (cursor->set & FB_CUR_SETSHAPE) {
		int size = ((info->cursor.image.width + 7) >> 3) * 
			     info->cursor.image.height;
		int i;

		switch (info->cursor.rop) {
		case ROP_XOR:
			for (i = 0; i < size; i++)
				data[i] = cursor->image.data[i] ^ info->cursor.mask[i]; 
			break;
		case ROP_COPY:
		default:
			for (i = 0; i < size; i++)
				data[i] = cursor->image.data[i] & info->cursor.mask[i]; 
			break;
		}
		i810_load_cursor_image(info->cursor.image.width, 
				       info->cursor.image.height, data,
				       par);
	}

	if (info->cursor.enable)
		i810_enable_cursor(mmio, ON);
	return 0;
}

static struct fb_ops i810fb_ops __devinitdata = {
	.owner =             THIS_MODULE,
	.fb_open =           i810fb_open,
	.fb_release =        i810fb_release,
	.fb_check_var =      i810fb_check_var,
	.fb_set_par =        i810fb_set_par,
	.fb_setcolreg =      i810fb_setcolreg,
	.fb_blank =          i810fb_blank,
	.fb_pan_display =    i810fb_pan_display, 
	.fb_fillrect =       i810fb_fillrect,
	.fb_copyarea =       i810fb_copyarea,
	.fb_imageblit =      i810fb_imageblit,
	.fb_cursor =         i810fb_cursor,
	.fb_sync =           i810fb_sync,
};

/***********************************************************************
 *                         Power Management                            *
 ***********************************************************************/
static int i810fb_suspend(struct pci_dev *dev, u32 state)
{
	struct fb_info *info = pci_get_drvdata(dev);
	struct i810fb_par *par = (struct i810fb_par *) info->par;
	int blank = 0, prev_state = par->cur_state;

	if (state == prev_state)
		return 0;

	par->cur_state = state;

	switch (state) {
	case 1:
		blank = VESA_VSYNC_SUSPEND;
		break;
	case 2:
		blank = VESA_HSYNC_SUSPEND;
		break;
	case 3:
		blank = VESA_POWERDOWN;
		break;
	default:
		return -EINVAL;
	}
	info->fbops->fb_blank(blank, info);

	if (!prev_state) { 
		par->drm_agp->unbind_memory(par->i810_gtt.i810_fb_memory);
		par->drm_agp->unbind_memory(par->i810_gtt.i810_cursor_memory);
		pci_disable_device(dev);
	}
	pci_save_state(dev, par->pci_state);
	pci_set_power_state(dev, state);

	return 0;
}

static int i810fb_resume(struct pci_dev *dev) 
{
	struct fb_info *info = pci_get_drvdata(dev);
	struct i810fb_par *par = (struct i810fb_par *) info->par;

	if (par->cur_state == 0)
		return 0;

	pci_restore_state(dev, par->pci_state);
	pci_set_power_state(dev, 0);
	pci_enable_device(dev);
	par->drm_agp->bind_memory(par->i810_gtt.i810_fb_memory, 
				  par->fb.offset);
	par->drm_agp->bind_memory(par->i810_gtt.i810_cursor_memory, 
				  par->cursor_heap.offset);

	info->fbops->fb_blank(VESA_NO_BLANKING, info);

	par->cur_state = 0;

	return 0;
}
/***********************************************************************
 *                  AGP resource allocation                            *
 ***********************************************************************/
  
static void __devinit i810_fix_pointers(struct i810fb_par *par)
{
      	par->fb.physical = par->aperture.physical+(par->fb.offset << 12);
	par->fb.virtual = par->aperture.virtual+(par->fb.offset << 12);
	par->iring.physical = par->aperture.physical + 
		(par->iring.offset << 12);
	par->iring.virtual = par->aperture.virtual + 
		(par->iring.offset << 12);
	par->cursor_heap.virtual = par->aperture.virtual+
		(par->cursor_heap.offset << 12);
}

static void __devinit i810_fix_offsets(struct i810fb_par *par)
{
	if (vram + 1 > par->aperture.size >> 20)
		vram = (par->aperture.size >> 20) - 1;
	if (v_offset_default > (par->aperture.size >> 20))
		v_offset_default = (par->aperture.size >> 20);
	if (vram + v_offset_default + 1 > par->aperture.size >> 20)
		v_offset_default = (par->aperture.size >> 20) - (vram + 1);

	par->fb.size = vram << 20;
	par->fb.offset = v_offset_default << 20;
	par->fb.offset >>= 12;

	par->iring.offset = par->fb.offset + (par->fb.size >> 12);
	par->iring.size = RINGBUFFER_SIZE;

	par->cursor_heap.offset = par->iring.offset + (RINGBUFFER_SIZE >> 12);
	par->cursor_heap.size = 4096;
}

static int __devinit i810_alloc_agp_mem(struct fb_info *info)
{
	struct i810fb_par *par = (struct i810fb_par *) info->par;
	int size;
	
	i810_fix_offsets(par);
	size = par->fb.size + par->iring.size;

	par->drm_agp = (drm_agp_t *) inter_module_get("drm_agp");
	if (!par->drm_agp) {
		printk("i810fb: cannot acquire agp\n");
		return -ENODEV;
	}
	par->drm_agp->acquire(); 

	if (!(par->i810_gtt.i810_fb_memory = 
	      par->drm_agp->allocate_memory(size >> 12, AGP_NORMAL_MEMORY))) {
		printk("i810fb_alloc_fbmem: can't allocate framebuffer "
		       "memory\n");
		par->drm_agp->release();
		return -ENOMEM;
	}
	if (par->drm_agp->bind_memory(par->i810_gtt.i810_fb_memory, 
				      par->fb.offset)) {
		printk("i810fb_alloc_fbmem: can't bind framebuffer memory\n");
		par->drm_agp->release();
		return -EBUSY;
	}	
	
	if (!(par->i810_gtt.i810_cursor_memory = 
	      par->drm_agp->allocate_memory(par->cursor_heap.size >> 12, 
					    AGP_PHYSICAL_MEMORY))) {
		printk("i810fb_alloc_cursormem:  can't allocate" 
		       "cursor memory\n");
		par->drm_agp->release();
		return -ENOMEM;
	}
	if (par->drm_agp->bind_memory(par->i810_gtt.i810_cursor_memory, 
			    par->cursor_heap.offset)) {
		printk("i810fb_alloc_cursormem: cannot bind cursor memory\n");
		par->drm_agp->release();
		return -EBUSY;
	}	

	par->cursor_heap.physical = par->i810_gtt.i810_cursor_memory->physical;

	i810_fix_pointers(par);

	par->drm_agp->release();

	return 0;
}

/*************************************************************** 
 *                    Initialization                           * 
 ***************************************************************/

/**
 * i810_init_monspecs
 * @info: pointer to device specific info structure
 *
 * DESCRIPTION:
 * Sets the the user monitor's horizontal and vertical
 * frequency limits
 */
static void __devinit i810_init_monspecs(struct fb_info *info)
{
	if (!hsync1)
		hsync1 = HFMIN;
	if (!hsync2) 
		hsync2 = HFMAX;
	info->monspecs.hfmax = hsync2;
	info->monspecs.hfmin = hsync1;
	if (hsync2 < hsync1) 
		info->monspecs.hfmin = hsync2;

	if (!vsync1)
		vsync1 = VFMIN;
	if (!vsync2) 
		vsync2 = VFMAX;
	if (IS_DVT && vsync1 < 60)
		vsync1 = 60;
	info->monspecs.vfmax = vsync2;
	info->monspecs.vfmin = vsync1;		
	if (vsync2 < vsync1) 
		info->monspecs.vfmin = vsync2;
}

/**
 * i810_init_defaults - initializes default values to use
 * @par: pointer to i810fb_par structure
 * @info: pointer to current fb_info structure
 */
static void __devinit i810_init_defaults(struct i810fb_par *par, 
				      struct fb_info *info)
{
	if (voffset) 
		v_offset_default = voffset;
	else if (par->aperture.size > 32 * 1024 * 1024)
		v_offset_default = 16;
	else
		v_offset_default = 8;

	if (!vram) 
		vram = 1;

	if (accel) 
		par->dev_flags |= HAS_ACCELERATION;

	if (sync) 
		par->dev_flags |= ALWAYS_SYNC;

	if (bpp < 8)
		bpp = 8;
	
	if (!vyres) 
		vyres = (vram << 20)/(xres*bpp >> 3);

	par->i810fb_ops = i810fb_ops;
	info->var.xres = xres;
	info->var.yres = yres;
	info->var.yres_virtual = vyres;
	info->var.bits_per_pixel = bpp;

	if (dcolor)
		info->var.nonstd = 1;

	if (par->dev_flags & HAS_ACCELERATION) 
		info->var.accel_flags = 1;

	i810_init_monspecs(info);
}
	
/**
 * i810_init_device - initialize device
 * @par: pointer to i810fb_par structure
 */
static void __devinit i810_init_device(struct i810fb_par *par)
{
	u8 reg, *mmio = par->mmio_start_virtual;

	if (mtrr) set_mtrr(par);

	i810_init_cursor(par);

	/* mvo: enable external vga-connector (for laptops) */
	if (ext_vga) {
		i810_writel(HVSYNC, mmio, 0);
		i810_writel(PWR_CLKC, mmio, 3);
	}

	pci_read_config_byte(par->dev, 0x50, &reg);
	reg &= FREQ_MASK;
	par->mem_freq = (reg) ? 133 : 100;
}

static int __devinit 
i810_allocate_pci_resource(struct i810fb_par *par, 
			   const struct pci_device_id *entry)
{
	int err;

	if ((err = pci_enable_device(par->dev))) { 
		printk("i810fb_init: cannot enable device\n");
		return err;		
	}
	par->res_flags |= PCI_DEVICE_ENABLED;

	if (pci_resource_len(par->dev, 0) > 512 * 1024) {
		par->aperture.physical = pci_resource_start(par->dev, 0);
		par->aperture.size = pci_resource_len(par->dev, 0);
		par->mmio_start_phys = pci_resource_start(par->dev, 1);
	} else {
		par->aperture.physical = pci_resource_start(par->dev, 1);
		par->aperture.size = pci_resource_len(par->dev, 1);
		par->mmio_start_phys = pci_resource_start(par->dev, 0);
	}
	if (!par->aperture.size) {
		printk("i810fb_init: device is disabled\n");
		return -ENOMEM;
	}

	if (!request_mem_region(par->aperture.physical, 
				par->aperture.size, 
				i810_pci_list[entry->driver_data])) {
		printk("i810fb_init: cannot request framebuffer region\n");
		return -ENODEV;
	}
	par->res_flags |= FRAMEBUFFER_REQ;

	par->aperture.virtual = ioremap_nocache(par->aperture.physical, 
					par->aperture.size);
	if (!par->aperture.virtual) {
		printk("i810fb_init: cannot remap framebuffer region\n");
		return -ENODEV;
	}
  
	if (!request_mem_region(par->mmio_start_phys, 
				MMIO_SIZE, 
				i810_pci_list[entry->driver_data])) {
		printk("i810fb_init: cannot request mmio region\n");
		return -ENODEV;
	}
	par->res_flags |= MMIO_REQ;

	par->mmio_start_virtual = ioremap_nocache(par->mmio_start_phys, 
						  MMIO_SIZE);
	if (!par->mmio_start_virtual) {
		printk("i810fb_init: cannot remap mmio region\n");
		return -ENODEV;
	}

	return 0;
}
	
int __init i810fb_setup(char *options)
{
	char *this_opt, *suffix = NULL;

	if (!options || !*options)
		return 0;
	
	while ((this_opt = strsep(&options, ",")) != NULL) {
		if (!strncmp(this_opt, "mtrr", 4))
			mtrr = 1;
		else if (!strncmp(this_opt, "accel", 5))
			accel = 1;
		else if (!strncmp(this_opt, "ext_vga", 7))
			ext_vga = 1;
		else if (!strncmp(this_opt, "sync", 4))
			sync = 1;
		else if (!strncmp(this_opt, "vram:", 5))
			vram = (simple_strtoul(this_opt+5, NULL, 0));
		else if (!strncmp(this_opt, "voffset:", 8))
			voffset = (simple_strtoul(this_opt+8, NULL, 0));
		else if (!strncmp(this_opt, "xres:", 5))
			xres = simple_strtoul(this_opt+5, NULL, 0);
		else if (!strncmp(this_opt, "yres:", 5))
			yres = simple_strtoul(this_opt+5, NULL, 0);
		else if (!strncmp(this_opt, "vyres:", 6))
			vyres = simple_strtoul(this_opt+6, NULL, 0);
		else if (!strncmp(this_opt, "bpp:", 4))
			bpp = simple_strtoul(this_opt+4, NULL, 0);
		else if (!strncmp(this_opt, "hsync1:", 7)) {
			hsync1 = simple_strtoul(this_opt+7, &suffix, 0);
			if (strncmp(suffix, "H", 1)) 
				hsync1 *= 1000;
		} else if (!strncmp(this_opt, "hsync2:", 7)) {
			hsync2 = simple_strtoul(this_opt+7, &suffix, 0);
			if (strncmp(suffix, "H", 1)) 
				hsync2 *= 1000;
		} else if (!strncmp(this_opt, "vsync1:", 7)) 
			vsync1 = simple_strtoul(this_opt+7, NULL, 0);
		else if (!strncmp(this_opt, "vsync2:", 7))
			vsync2 = simple_strtoul(this_opt+7, NULL, 0);
		else if (!strncmp(this_opt, "dcolor", 6))
			dcolor = 1;
	}
	return 0;
}

static int __devinit i810fb_init_pci (struct pci_dev *dev, 
				   const struct pci_device_id *entry)
{
	struct fb_info    *info;
	struct i810fb_par *par = NULL;
	int err, vfreq, hfreq, pixclock;

	if (!(info = kmalloc(sizeof(struct fb_info), GFP_KERNEL))) {
		i810fb_release_resource(info, par);
		return -ENOMEM;
	}
	memset(info, 0, sizeof(struct fb_info));

	if(!(par = kmalloc(sizeof(struct i810fb_par), GFP_KERNEL))) {
		i810fb_release_resource(info, par);
		return -ENOMEM;
	}
	memset(par, 0, sizeof(struct i810fb_par));

	par->dev = dev;
	info->par = par;

	if (!(info->pixmap.addr = kmalloc(64*1024, GFP_KERNEL))) {
		i810fb_release_resource(info, par);
		return -ENOMEM;
	}
	memset(info->pixmap.addr, 0, 64*1024);
	info->pixmap.size = 64*1024;
	info->pixmap.buf_align = 8;
	info->pixmap.flags = FB_PIXMAP_SYSTEM;

	if ((err = i810_allocate_pci_resource(par, entry))) {
		i810fb_release_resource(info, par);
		return err;
	}

	i810_init_defaults(par, info);

	if ((err = i810_alloc_agp_mem(info))) {
		i810fb_release_resource(info, par);
		return err;
	}

	i810_init_device(par);        

	info->screen_base = par->fb.virtual;
	info->fbops = &par->i810fb_ops;
	info->pseudo_palette = par->pseudo_palette;
	info->flags = FBINFO_FLAG_DEFAULT;
	
	fb_alloc_cmap(&info->cmap, 256, 0);

	if ((err = info->fbops->fb_check_var(&info->var, info))) {
		i810fb_release_resource(info, par);
		return err;
	}
	encode_fix(&info->fix, info); 
	 	    
	i810fb_init_ringbuffer(info);
	err = register_framebuffer(info);
	if (err < 0) {
    		i810fb_release_resource(info, par); 
		printk("i810fb_init: cannot register framebuffer device\n");
    		return err;  
    	}   

	pci_set_drvdata(dev, info);
	pixclock = 1000000000/(info->var.pixclock);
	pixclock *= 1000;
	hfreq = pixclock/(info->var.xres + info->var.left_margin + 
			  info->var.hsync_len + info->var.right_margin);
	vfreq = hfreq/(info->var.yres + info->var.upper_margin +
		       info->var.vsync_len + info->var.lower_margin);

      	printk("I810FB: fb%d         : %s v%d.%d.%d%s\n"
      	       "I810FB: Video RAM   : %dK\n" 
	       "I810FB: Monitor     : H: %d-%d KHz V: %d-%d Hz\n"
	       "I810FB: Mode        : %dx%d-%dbpp@%dHz\n",
	       info->node,
	       i810_pci_list[entry->driver_data],
	       VERSION_MAJOR, VERSION_MINOR, VERSION_TEENIE, BRANCH_VERSION,
	       (int) par->fb.size>>10, info->monspecs.hfmin/1000,
	       info->monspecs.hfmax/1000, info->monspecs.vfmin,
	       info->monspecs.vfmax, info->var.xres, 
	       info->var.yres, info->var.bits_per_pixel, vfreq);
	return 0;
}

/***************************************************************
 *                     De-initialization                        *
 ***************************************************************/

static void i810fb_release_resource(struct fb_info *info, 
				    struct i810fb_par *par)
{
	if (par) {
		unset_mtrr(par);
		if (par->drm_agp) {
			drm_agp_t *agp = par->drm_agp;
			struct gtt_data *gtt = &par->i810_gtt;

			if (par->i810_gtt.i810_cursor_memory) 
				agp->free_memory(gtt->i810_cursor_memory);
			if (par->i810_gtt.i810_fb_memory) 
				agp->free_memory(gtt->i810_fb_memory);

			inter_module_put("drm_agp");
			par->drm_agp = NULL;
		}

		if (par->mmio_start_virtual) 
			iounmap(par->mmio_start_virtual);
		if (par->aperture.virtual) 
			iounmap(par->aperture.virtual);

		if (par->res_flags & FRAMEBUFFER_REQ)
			release_mem_region(par->aperture.physical, 
					   par->aperture.size);
		if (par->res_flags & MMIO_REQ)
			release_mem_region(par->mmio_start_phys, MMIO_SIZE);

		if (par->res_flags & PCI_DEVICE_ENABLED)
			pci_disable_device(par->dev); 

		kfree(par);
	}
	if (info) 
		kfree(info);
}

static void __exit i810fb_remove_pci(struct pci_dev *dev)
{
	struct fb_info *info = pci_get_drvdata(dev);
	struct i810fb_par *par = (struct i810fb_par *) info->par;

	unregister_framebuffer(info);  
	i810fb_release_resource(info, par);
	pci_set_drvdata(dev, NULL);
	printk("cleanup_module:  unloaded i810 framebuffer device\n");
}                                                	

#ifndef MODULE
int __init i810fb_init(void)
{
	if (agp_intel_init()) {
		printk("i810fb_init: cannot initialize intel agpgart\n");
		return -ENODEV;
	}

	if (pci_register_driver(&i810fb_driver) > 0)
		return 0;
	pci_unregister_driver(&i810fb_driver);
	return -ENODEV;
}
#endif 

/*********************************************************************
 *                          Modularization                           *
 *********************************************************************/

#ifdef MODULE

int __init i810fb_init(void)
{
	hsync1 *= 1000;
	hsync2 *= 1000;

	if (pci_register_driver(&i810fb_driver) > 0)
		return 0;
	pci_unregister_driver(&i810fb_driver);
	return -ENODEV;
}

MODULE_PARM(vram, "i");
MODULE_PARM_DESC(vram, "System RAM to allocate to framebuffer in MiB" 
		 " (default=4)");
MODULE_PARM(voffset, "i");
MODULE_PARM_DESC(voffset, "at what offset to place start of framebuffer "
                 "memory (0 to maximum aperture size), in MiB (default = 48)");
MODULE_PARM(bpp, "i");
MODULE_PARM_DESC(bpp, "Color depth for display in bits per pixel"
		 " (default = 8)");
MODULE_PARM(xres, "i");
MODULE_PARM_DESC(xres, "Horizontal resolution in pixels (default = 640)");
MODULE_PARM(yres, "i");
MODULE_PARM_DESC(yres, "Vertical resolution in scanlines (default = 480)");
MODULE_PARM(vyres, "i");
MODULE_PARM_DESC(vyres, "Virtual vertical resolution in scanlines"
		 " (default = 480)");
MODULE_PARM(hsync1, "i");
MODULE_PARM_DESC(hsync1, "Minimum horizontal frequency of monitor in KHz"
		 " (default = 31)");
MODULE_PARM(hsync2, "i");
MODULE_PARM_DESC(hsync2, "Maximum horizontal frequency of monitor in KHz"
		 " (default = 31)");
MODULE_PARM(vsync1, "i");
MODULE_PARM_DESC(vsync1, "Minimum vertical frequency of monitor in Hz"
		 " (default = 50)");
MODULE_PARM(vsync2, "i");
MODULE_PARM_DESC(vsync2, "Maximum vertical frequency of monitor in Hz" 
		 " (default = 60)");
MODULE_PARM(accel, "i");
MODULE_PARM_DESC(accel, "Use Acceleration (BLIT) engine (default = 0)");
MODULE_PARM(mtrr, "i");
MODULE_PARM_DESC(mtrr, "Use MTRR (default = 0)");
MODULE_PARM(ext_vga, "i");
MODULE_PARM_DESC(ext_vga, "Enable external VGA connector (default = 0)");
MODULE_PARM(sync, "i");
MODULE_PARM_DESC(sync, "wait for accel engine to finish drawing"
		 " (default = 0)");
MODULE_PARM(dcolor, "i");
MODULE_PARM_DESC(dcolor, "use DirectColor visuals"
		 " (default = 0 = TrueColor)");

MODULE_AUTHOR("Tony A. Daplas");
MODULE_DESCRIPTION("Framebuffer device for the Intel 810/815 and"
		   " compatible cards");
MODULE_LICENSE("GPL"); 

static void __exit i810fb_exit(void)
{
	pci_unregister_driver(&i810fb_driver);
}
module_init(i810fb_init);
module_exit(i810fb_exit);

#endif /* MODULE */


