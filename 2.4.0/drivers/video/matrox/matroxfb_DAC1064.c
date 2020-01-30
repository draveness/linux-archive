/*
 *
 * Hardware accelerated Matrox Millennium I, II, Mystique, G100, G200 and G400
 *
 * (c) 1998,1999,2000 Petr Vandrovec <vandrove@vc.cvut.cz>
 *
 * Version: 1.50 2000/08/10
 *
 * MTRR stuff: 1998 Tom Rini <trini@kernel.crashing.org>
 *
 * Contributors: "menion?" <menion@mindless.com>
 *                     Betatesting, fixes, ideas
 *
 *               "Kurt Garloff" <garloff@suse.de>
 *                     Betatesting, fixes, ideas, videomodes, videomodes timmings
 *
 *               "Tom Rini" <trini@kernel.crashing.org>
 *                     MTRR stuff, PPC cleanups, betatesting, fixes, ideas
 *
 *               "Bibek Sahu" <scorpio@dodds.net>
 *                     Access device through readb|w|l and write b|w|l
 *                     Extensive debugging stuff
 *
 *               "Daniel Haun" <haund@usa.net>
 *                     Testing, hardware cursor fixes
 *
 *               "Scott Wood" <sawst46+@pitt.edu>
 *                     Fixes
 *
 *               "Gerd Knorr" <kraxel@goldbach.isdn.cs.tu-berlin.de>
 *                     Betatesting
 *
 *               "Kelly French" <targon@hazmat.com>
 *               "Fernando Herrera" <fherrera@eurielec.etsit.upm.es>
 *                     Betatesting, bug reporting
 *
 *               "Pablo Bianucci" <pbian@pccp.com.ar>
 *                     Fixes, ideas, betatesting
 *
 *               "Inaky Perez Gonzalez" <inaky@peloncho.fis.ucm.es>
 *                     Fixes, enhandcements, ideas, betatesting
 *
 *               "Ryuichi Oikawa" <roikawa@rr.iiij4u.or.jp>
 *                     PPC betatesting, PPC support, backward compatibility
 *
 *               "Paul Womar" <Paul@pwomar.demon.co.uk>
 *               "Owen Waller" <O.Waller@ee.qub.ac.uk>
 *                     PPC betatesting
 *
 *               "Thomas Pornin" <pornin@bolet.ens.fr>
 *                     Alpha betatesting
 *
 *               "Pieter van Leuven" <pvl@iae.nl>
 *               "Ulf Jaenicke-Roessler" <ujr@physik.phy.tu-dresden.de>
 *                     G100 testing
 *
 *               "H. Peter Arvin" <hpa@transmeta.com>
 *                     Ideas
 *
 *               "Cort Dougan" <cort@cs.nmt.edu>
 *                     CHRP fixes and PReP cleanup
 *
 *               "Mark Vojkovich" <mvojkovi@ucsd.edu>
 *                     G400 support
 *
 *               "Ken Aaker" <kdaaker@rchland.vnet.ibm.com>
 *                     memtype extension (needed for GXT130P RS/6000 adapter)
 *
 * (following author is not in any relation with this code, but his code
 *  is included in this driver)
 *
 * Based on framebuffer driver for VBE 2.0 compliant graphic boards
 *     (c) 1998 Gerd Knorr <kraxel@cs.tu-berlin.de>
 *
 * (following author is not in any relation with this code, but his ideas
 *  were used when writting this driver)
 *
 *		 FreeVBE/AF (Matrox), "Shawn Hargreaves" <shawn@talula.demon.co.uk>
 *
 */

/* make checkconfig does not walk through include tree :-( */
#include <linux/config.h>

#include "matroxfb_DAC1064.h"
#include "matroxfb_misc.h"
#include "matroxfb_accel.h"
#include <linux/matroxfb.h>

#ifdef NEED_DAC1064
#define outDAC1064 matroxfb_DAC_out
#define inDAC1064 matroxfb_DAC_in

#define DAC1064_OPT_SCLK_PCI	0x00
#define DAC1064_OPT_SCLK_PLL	0x01
#define DAC1064_OPT_SCLK_EXT	0x02
#define DAC1064_OPT_SCLK_MASK	0x03
#define DAC1064_OPT_GDIV1	0x04	/* maybe it is GDIV2 on G100 ?! */
#define DAC1064_OPT_GDIV3	0x00
#define DAC1064_OPT_MDIV1	0x08
#define DAC1064_OPT_MDIV2	0x00
#define DAC1064_OPT_RESERVED	0x10

static void matroxfb_DAC1064_flashcursor(unsigned long ptr) {
#define minfo ((struct matrox_fb_info*)ptr)
	matroxfb_DAC_lock();
	outDAC1064(PMINFO M1064_XCURCTRL, inDAC1064(PMINFO M1064_XCURCTRL) ^ M1064_XCURCTRL_DIS ^ M1064_XCURCTRL_XGA);
	ACCESS_FBINFO(cursor.timer.expires) = jiffies + HZ/2;
	add_timer(&ACCESS_FBINFO(cursor.timer));
	matroxfb_DAC_unlock();
#undef minfo
}

static void matroxfb_DAC1064_createcursor(WPMINFO struct display* p) {
	vaddr_t cursorbase;
	u_int32_t xline;
	unsigned int i;
	unsigned int h, to;
	CRITFLAGS

	if (ACCESS_FBINFO(currcon_display) != p)
		return;

	matroxfb_createcursorshape(PMINFO p, p->var.vmode);

	xline = (~0) << (32 - ACCESS_FBINFO(cursor.w));
	cursorbase = ACCESS_FBINFO(video.vbase);
	h = ACCESS_FBINFO(features.DAC1064.cursorimage);

	CRITBEGIN

#ifdef __BIG_ENDIAN
	WaitTillIdle();
	mga_outl(M_OPMODE, M_OPMODE_32BPP);
#endif
	to = ACCESS_FBINFO(cursor.u);
	for (i = 0; i < to; i++) {
		mga_writel(cursorbase, h, 0);
		mga_writel(cursorbase, h+4, 0);
		mga_writel(cursorbase, h+8, ~0);
		mga_writel(cursorbase, h+12, ~0);
		h += 16;
	}
	to = ACCESS_FBINFO(cursor.d);
	for (; i < to; i++) {
		mga_writel(cursorbase, h, 0);
		mga_writel(cursorbase, h+4, xline);
		mga_writel(cursorbase, h+8, ~0);
		mga_writel(cursorbase, h+12, ~0);
		h += 16;
	}
	for (; i < 64; i++) {
		mga_writel(cursorbase, h, 0);
		mga_writel(cursorbase, h+4, 0);
		mga_writel(cursorbase, h+8, ~0);
		mga_writel(cursorbase, h+12, ~0);
		h += 16;
	}
#ifdef __BIG_ENDIAN
	mga_outl(M_OPMODE, ACCESS_FBINFO(accel.m_opmode));
#endif

	CRITEND
}

static void matroxfb_DAC1064_cursor(struct display* p, int mode, int x, int y) {
	unsigned long flags;
	MINFO_FROM_DISP(p);

	if (ACCESS_FBINFO(currcon_display) != p)
		return;

	if (mode == CM_ERASE) {
		if (ACCESS_FBINFO(cursor.state) != CM_ERASE) {
			del_timer_sync(&ACCESS_FBINFO(cursor.timer));
			matroxfb_DAC_lock_irqsave(flags);
			ACCESS_FBINFO(cursor.state) = CM_ERASE;
			outDAC1064(PMINFO M1064_XCURCTRL, M1064_XCURCTRL_DIS);
			matroxfb_DAC_unlock_irqrestore(flags);
		}
		return;
	}
	if ((p->conp->vc_cursor_type & CUR_HWMASK) != ACCESS_FBINFO(cursor.type))
		matroxfb_DAC1064_createcursor(PMINFO p);
	x *= fontwidth(p);
	y *= fontheight(p);
	y -= p->var.yoffset;
	if (p->var.vmode & FB_VMODE_DOUBLE)
		y *= 2;
	del_timer_sync(&ACCESS_FBINFO(cursor.timer));
	matroxfb_DAC_lock_irqsave(flags);
	if ((x != ACCESS_FBINFO(cursor.x)) || (y != ACCESS_FBINFO(cursor.y)) || ACCESS_FBINFO(cursor.redraw)) {
		ACCESS_FBINFO(cursor.redraw) = 0;
		ACCESS_FBINFO(cursor.x) = x;
		ACCESS_FBINFO(cursor.y) = y;
		x += 64;
		y += 64;
		outDAC1064(PMINFO M1064_XCURCTRL, M1064_XCURCTRL_DIS);
		mga_outb(M_RAMDAC_BASE+M1064_CURPOSXL, x);
		mga_outb(M_RAMDAC_BASE+M1064_CURPOSXH, x >> 8);
		mga_outb(M_RAMDAC_BASE+M1064_CURPOSYL, y);
		mga_outb(M_RAMDAC_BASE+M1064_CURPOSYH, y >> 8);
	}
	ACCESS_FBINFO(cursor.state) = CM_DRAW;
	if (ACCESS_FBINFO(devflags.blink))
		mod_timer(&ACCESS_FBINFO(cursor.timer), jiffies + HZ/2);
	outDAC1064(PMINFO M1064_XCURCTRL, M1064_XCURCTRL_XGA);
	matroxfb_DAC_unlock_irqrestore(flags);
}

static int matroxfb_DAC1064_setfont(struct display* p, int width, int height) {
	if (p && p->conp)
		matroxfb_DAC1064_createcursor(PMXINFO(p) p);
	return 0;
}

static int DAC1064_selhwcursor(WPMINFO struct display* p) {
	ACCESS_FBINFO(dispsw.cursor) = matroxfb_DAC1064_cursor;
	ACCESS_FBINFO(dispsw.set_font) = matroxfb_DAC1064_setfont;
	return 0;
}

static void DAC1064_calcclock(CPMINFO unsigned int freq, unsigned int fmax, unsigned int* in, unsigned int* feed, unsigned int* post) {
	unsigned int fvco;
	unsigned int p;

	DBG("DAC1064_calcclock")

	fvco = PLL_calcclock(PMINFO freq, fmax, in, feed, &p);
	
	if (ACCESS_FBINFO(devflags.g450dac)) {
		if (fvco <= 300000)		/* 276-324 */
			;
		else if (fvco <= 400000)	/* 378-438 */
			p |= 0x08;
		else if (fvco <= 550000)	/* 540-567 */
			p |= 0x10;
		else if (fvco <= 690000)	/* 675-695 */
			p |= 0x18;
		else if (fvco <= 800000)	/* 776-803 */
			p |= 0x20;
		else if (fvco <= 891000)	/* 891-891 */
			p |= 0x28;
		else if (fvco <= 940000)	/* 931-945 */
			p |= 0x30;
		else				/* <959 */
			p |= 0x38;
	} else {
		p = (1 << p) - 1;
		if (fvco <= 100000)
			;
		else if (fvco <= 140000)
			p |= 0x08;
		else if (fvco <= 180000)
			p |= 0x10;
		else
			p |= 0x18;
	}
	*post = p;
}

/* they must be in POS order */
static const unsigned char MGA1064_DAC_regs[] = {
		M1064_XCURADDL, M1064_XCURADDH, M1064_XCURCTRL,
		M1064_XCURCOL0RED, M1064_XCURCOL0GREEN, M1064_XCURCOL0BLUE,
		M1064_XCURCOL1RED, M1064_XCURCOL1GREEN, M1064_XCURCOL1BLUE,
		M1064_XCURCOL2RED, M1064_XCURCOL2GREEN, M1064_XCURCOL2BLUE,
		DAC1064_XVREFCTRL, M1064_XMULCTRL, M1064_XPIXCLKCTRL, M1064_XGENCTRL,
		M1064_XMISCCTRL,
		M1064_XGENIOCTRL, M1064_XGENIODATA, M1064_XZOOMCTRL, M1064_XSENSETEST,
		M1064_XCRCBITSEL,
		M1064_XCOLKEYMASKL, M1064_XCOLKEYMASKH, M1064_XCOLKEYL, M1064_XCOLKEYH };

static const unsigned char MGA1064_DAC[] = {
		0x00, 0x00, M1064_XCURCTRL_DIS,
		0x00, 0x00, 0x00, 	/* black */
		0xFF, 0xFF, 0xFF,	/* white */
		0xFF, 0x00, 0x00,	/* red */
		0x00, 0,
		M1064_XPIXCLKCTRL_PLL_UP | M1064_XPIXCLKCTRL_EN | M1064_XPIXCLKCTRL_SRC_PLL,
		M1064_XGENCTRL_VS_0 | M1064_XGENCTRL_ALPHA_DIS | M1064_XGENCTRL_BLACK_0IRE | M1064_XGENCTRL_NO_SYNC_ON_GREEN,
		M1064_XMISCCTRL_DAC_8BIT,
		0x00, 0x00, M1064_XZOOMCTRL_1, M1064_XSENSETEST_BCOMP | M1064_XSENSETEST_GCOMP | M1064_XSENSETEST_RCOMP | M1064_XSENSETEST_PDOWN,
		0x00,
		0x00, 0x00, 0xFF, 0xFF};

static void DAC1064_setpclk(CPMINFO struct matrox_hw_state* hw, unsigned long fout) {
	unsigned int m, n, p;

	DBG("DAC1064_setpclk")

	DAC1064_calcclock(PMINFO fout, ACCESS_FBINFO(max_pixel_clock), &m, &n, &p);
	hw->DACclk[0] = m;
	hw->DACclk[1] = n;
	hw->DACclk[2] = p;
}

static void DAC1064_setmclk(CPMINFO struct matrox_hw_state* hw, int oscinfo, unsigned long fmem){
	u_int32_t mx;

	DBG("DAC1064_setmclk")

	if (ACCESS_FBINFO(devflags.noinit)) {
		/* read MCLK and give up... */
		hw->DACclk[3] = inDAC1064(PMINFO DAC1064_XSYSPLLM);
		hw->DACclk[4] = inDAC1064(PMINFO DAC1064_XSYSPLLN);
		hw->DACclk[5] = inDAC1064(PMINFO DAC1064_XSYSPLLP);
		return;
	}
	mx = hw->MXoptionReg | 0x00000004;
	pci_write_config_dword(ACCESS_FBINFO(pcidev), PCI_OPTION_REG, mx);
	mx &= ~0x000000BB;
	if (oscinfo & DAC1064_OPT_GDIV1)
		mx |= 0x00000008;
	if (oscinfo & DAC1064_OPT_MDIV1)
		mx |= 0x00000010;
	if (oscinfo & DAC1064_OPT_RESERVED)
		mx |= 0x00000080;
	if ((oscinfo & DAC1064_OPT_SCLK_MASK) == DAC1064_OPT_SCLK_PLL) {
		/* select PCI clock until we have setup oscilator... */
		int clk;
		unsigned int m, n, p;

		/* powerup system PLL, select PCI clock */
		mx |= 0x00000020;
		pci_write_config_dword(ACCESS_FBINFO(pcidev), PCI_OPTION_REG, mx);
		mx &= ~0x00000004;
		pci_write_config_dword(ACCESS_FBINFO(pcidev), PCI_OPTION_REG, mx);

		/* !!! you must not access device if MCLK is not running !!!
		   Doing so cause immediate PCI lockup :-( Maybe they should
		   generate ABORT or I/O (parity...) error and Linux should
		   recover from this... (kill driver/process). But world is not
		   perfect... */
		/* (bit 2 of PCI_OPTION_REG must be 0... and bits 0,1 must not
		   select PLL... because of PLL can be stopped at this time) */
		DAC1064_calcclock(PMINFO fmem, ACCESS_FBINFO(max_pixel_clock), &m, &n, &p);
		outDAC1064(PMINFO DAC1064_XSYSPLLM, hw->DACclk[3] = m);
		outDAC1064(PMINFO DAC1064_XSYSPLLN, hw->DACclk[4] = n);
		outDAC1064(PMINFO DAC1064_XSYSPLLP, hw->DACclk[5] = p);
		for (clk = 65536; clk; --clk) {
			if (inDAC1064(PMINFO DAC1064_XSYSPLLSTAT) & 0x40)
				break;
		}
		if (!clk)
			printk(KERN_ERR "matroxfb: aiee, SYSPLL not locked\n");
		/* select PLL */
		mx |= 0x00000005;
	} else {
		/* select specified system clock source */
		mx |= oscinfo & DAC1064_OPT_SCLK_MASK;
	}
	pci_write_config_dword(ACCESS_FBINFO(pcidev), PCI_OPTION_REG, mx);
	mx &= ~0x00000004;
	pci_write_config_dword(ACCESS_FBINFO(pcidev), PCI_OPTION_REG, mx);
	hw->MXoptionReg = mx;
}

void DAC1064_global_init(CPMINFO struct matrox_hw_state* hw) {
	hw->DACreg[POS1064_XMISCCTRL] &= M1064_XMISCCTRL_DAC_WIDTHMASK;
	hw->DACreg[POS1064_XMISCCTRL] |= M1064_XMISCCTRL_LUT_EN;
	hw->DACreg[POS1064_XPIXCLKCTRL] = M1064_XPIXCLKCTRL_PLL_UP | M1064_XPIXCLKCTRL_EN | M1064_XPIXCLKCTRL_SRC_PLL;
	hw->DACreg[POS1064_XOUTPUTCONN] = 0x01;	/* output #1 enabled */
	if (ACCESS_FBINFO(output.ph) & MATROXFB_OUTPUT_CONN_SECONDARY) {
		if (ACCESS_FBINFO(devflags.g450dac)) {
			hw->DACreg[POS1064_XPIXCLKCTRL] = M1064_XPIXCLKCTRL_PLL_UP | M1064_XPIXCLKCTRL_EN | M1064_XPIXCLKCTRL_SRC_PLL2;
			hw->DACreg[POS1064_XOUTPUTCONN] = 0x05;	/* output #1 enabled; CRTC1 connected to output #2 */
		} else {
			hw->DACreg[POS1064_XPIXCLKCTRL] = M1064_XPIXCLKCTRL_PLL_UP | M1064_XPIXCLKCTRL_EN | M1064_XPIXCLKCTRL_SRC_EXT;
			hw->DACreg[POS1064_XMISCCTRL] |= GX00_XMISCCTRL_MFC_MAFC | G400_XMISCCTRL_VDO_MAFC12;
		}
	} else if (ACCESS_FBINFO(output.sh) & MATROXFB_OUTPUT_CONN_SECONDARY) {
		hw->DACreg[POS1064_XMISCCTRL] |= GX00_XMISCCTRL_MFC_MAFC | G400_XMISCCTRL_VDO_C2_MAFC12;
		hw->DACreg[POS1064_XOUTPUTCONN] = 0x09; /* output #1 enabled; CRTC2 connected to output #2 */
	} else if (ACCESS_FBINFO(output.ph) & MATROXFB_OUTPUT_CONN_DFP)
		hw->DACreg[POS1064_XMISCCTRL] |= GX00_XMISCCTRL_MFC_PANELLINK | G400_XMISCCTRL_VDO_MAFC12;
	else
		hw->DACreg[POS1064_XMISCCTRL] |= GX00_XMISCCTRL_MFC_DIS;

	if ((ACCESS_FBINFO(output.ph) | ACCESS_FBINFO(output.sh)) & MATROXFB_OUTPUT_CONN_PRIMARY)
		hw->DACreg[POS1064_XMISCCTRL] |= M1064_XMISCCTRL_DAC_EN;
}

void DAC1064_global_restore(CPMINFO const struct matrox_hw_state* hw) {
	outDAC1064(PMINFO M1064_XPIXCLKCTRL, hw->DACreg[POS1064_XPIXCLKCTRL]);
	outDAC1064(PMINFO M1064_XMISCCTRL, hw->DACreg[POS1064_XMISCCTRL]);
	if (ACCESS_FBINFO(devflags.accelerator) == FB_ACCEL_MATROX_MGAG400) {
		outDAC1064(PMINFO 0x20, 0x04);
		outDAC1064(PMINFO 0x1F, 0x00);
		if (ACCESS_FBINFO(devflags.g450dac)) {
			outDAC1064(PMINFO M1064_X8B, 0xCC);	/* only matrox know... */
			outDAC1064(PMINFO M1064_XOUTPUTCONN, hw->DACreg[POS1064_XOUTPUTCONN]);
		}
	}
}

static int DAC1064_init_1(CPMINFO struct matrox_hw_state* hw, struct my_timming* m, struct display *p) {
	DBG("DAC1064_init_1")

	memcpy(hw->DACreg, MGA1064_DAC, sizeof(MGA1064_DAC_regs));
	if (p->type == FB_TYPE_TEXT) {
		hw->DACreg[POS1064_XMISCCTRL] = M1064_XMISCCTRL_DAC_6BIT;
		hw->DACreg[POS1064_XMULCTRL] = M1064_XMULCTRL_DEPTH_8BPP
					     | M1064_XMULCTRL_GRAPHICS_PALETIZED;
	} else {
		switch (p->var.bits_per_pixel) {
		/* case 4: not supported by MGA1064 DAC */
		case 8:
			hw->DACreg[POS1064_XMULCTRL] = M1064_XMULCTRL_DEPTH_8BPP | M1064_XMULCTRL_GRAPHICS_PALETIZED;
			break;
		case 16:
			if (p->var.green.length == 5)
				hw->DACreg[POS1064_XMULCTRL] = M1064_XMULCTRL_DEPTH_15BPP_1BPP | M1064_XMULCTRL_GRAPHICS_PALETIZED;
			else
				hw->DACreg[POS1064_XMULCTRL] = M1064_XMULCTRL_DEPTH_16BPP | M1064_XMULCTRL_GRAPHICS_PALETIZED;
			break;
		case 24:
			hw->DACreg[POS1064_XMULCTRL] = M1064_XMULCTRL_DEPTH_24BPP | M1064_XMULCTRL_GRAPHICS_PALETIZED;
			break;
		case 32:
			hw->DACreg[POS1064_XMULCTRL] = M1064_XMULCTRL_DEPTH_32BPP | M1064_XMULCTRL_GRAPHICS_PALETIZED;
			break;
		default:
			return 1;	/* unsupported depth */
		}
	}

	DAC1064_global_init(PMINFO hw);
	hw->DACreg[POS1064_XVREFCTRL] = ACCESS_FBINFO(features.DAC1064.xvrefctrl);
	hw->DACreg[POS1064_XGENCTRL] &= ~M1064_XGENCTRL_SYNC_ON_GREEN_MASK;
	hw->DACreg[POS1064_XGENCTRL] |= (m->sync & FB_SYNC_ON_GREEN)?M1064_XGENCTRL_SYNC_ON_GREEN:M1064_XGENCTRL_NO_SYNC_ON_GREEN;
	hw->DACreg[POS1064_XCURADDL] = ACCESS_FBINFO(features.DAC1064.cursorimage) >> 10;
	hw->DACreg[POS1064_XCURADDH] = ACCESS_FBINFO(features.DAC1064.cursorimage) >> 18;
	return 0;
}

static int DAC1064_init_2(CPMINFO struct matrox_hw_state* hw, struct my_timming* m, struct display* p) {

	DBG("DAC1064_init_2")

	if (p->var.bits_per_pixel > 16) {	/* 256 entries */
		int i;

		for (i = 0; i < 256; i++) {
			hw->DACpal[i * 3 + 0] = i;
			hw->DACpal[i * 3 + 1] = i;
			hw->DACpal[i * 3 + 2] = i;
		}
	} else if (p->var.bits_per_pixel > 8) {
		if (p->var.green.length == 5) {	/* 0..31, 128..159 */
			int i;

			for (i = 0; i < 32; i++) {
				/* with p15 == 0 */
				hw->DACpal[i * 3 + 0] = i << 3;
				hw->DACpal[i * 3 + 1] = i << 3;
				hw->DACpal[i * 3 + 2] = i << 3;
				/* with p15 == 1 */
				hw->DACpal[(i + 128) * 3 + 0] = i << 3;
				hw->DACpal[(i + 128) * 3 + 1] = i << 3;
				hw->DACpal[(i + 128) * 3 + 2] = i << 3;
			}
		} else {
			int i;

			for (i = 0; i < 64; i++) {		/* 0..63 */
				hw->DACpal[i * 3 + 0] = i << 3;
				hw->DACpal[i * 3 + 1] = i << 2;
				hw->DACpal[i * 3 + 2] = i << 3;
			}
		}
	} else {
		memset(hw->DACpal, 0, 768);
	}
	return 0;
}

static void DAC1064_restore_1(WPMINFO const struct matrox_hw_state* hw, const struct matrox_hw_state* oldhw) {
	CRITFLAGS

	DBG("DAC1064_restore_1")

	CRITBEGIN

	outDAC1064(PMINFO DAC1064_XSYSPLLM, hw->DACclk[3]);
	outDAC1064(PMINFO DAC1064_XSYSPLLN, hw->DACclk[4]);
	outDAC1064(PMINFO DAC1064_XSYSPLLP, hw->DACclk[5]);
	if (!oldhw || memcmp(hw->DACreg, oldhw->DACreg, sizeof(MGA1064_DAC_regs))) {
		unsigned int i;

		for (i = 0; i < sizeof(MGA1064_DAC_regs); i++) {
			if ((i != POS1064_XPIXCLKCTRL) && (i != POS1064_XMISCCTRL))
				outDAC1064(PMINFO MGA1064_DAC_regs[i], hw->DACreg[i]);
		}
	}

	DAC1064_global_restore(PMINFO hw);

	CRITEND
};

static void DAC1064_restore_2(WPMINFO const struct matrox_hw_state* hw, const struct matrox_hw_state* oldhw, struct display* p) {
#ifdef DEBUG
	unsigned int i;
#endif

	DBG("DAC1064_restore_2")

	matrox_init_putc(PMINFO p, matroxfb_DAC1064_createcursor);
#ifdef DEBUG
	dprintk(KERN_DEBUG "DAC1064regs ");
	for (i = 0; i < sizeof(MGA1064_DAC_regs); i++) {
		dprintk("R%02X=%02X ", MGA1064_DAC_regs[i], hw->DACreg[i]);
		if ((i & 0x7) == 0x7) dprintk("\n" KERN_DEBUG "continuing... ");
	}
	dprintk("\n" KERN_DEBUG "DAC1064clk ");
	for (i = 0; i < 6; i++)
		dprintk("C%02X=%02X ", i, hw->DACclk[i]);
	dprintk("\n");
#endif
}

static int m1064_compute(void* outdev, struct my_timming* m, struct matrox_hw_state* hw) {
#define minfo ((struct matrox_fb_info*)outdev)
	DAC1064_setpclk(PMINFO hw, m->pixclock);
#undef minfo
	return 0;
}

static int m1064_program(void* outdev, const struct matrox_hw_state* hw) {
#define minfo ((struct matrox_fb_info*)outdev)
	int i;
	int tmout;
	CRITFLAGS

	CRITBEGIN

	for (i = 0; i < 3; i++)
		outDAC1064(PMINFO M1064_XPIXPLLCM + i, hw->DACclk[i]);
	for (tmout = 500000; tmout; tmout--) {
		if (inDAC1064(PMINFO M1064_XPIXPLLSTAT) & 0x40)
			break;
		udelay(10);
	};

	CRITEND

	if (!tmout)
		printk(KERN_ERR "matroxfb: Pixel PLL not locked after 5 secs\n");

#undef minfo
	return 0;
}

static int m1064_start(void* outdev) {
	/* nothing */
	return 0;
}

static void m1064_incuse(void* outdev) {
	/* nothing yet; MODULE_INC_USE in future... */
}

static void m1064_decuse(void* outdev) {
	/* nothing yet; MODULE_DEC_USE in future... */
}

static int m1064_setmode(void* outdev, u_int32_t mode) {
	if (mode != MATROXFB_OUTPUT_MODE_MONITOR)
		return -EINVAL;
	return 0;
}

static struct matrox_altout m1064 = {
	m1064_compute,
	m1064_program,
	m1064_start,
	m1064_incuse,
	m1064_decuse,
	m1064_setmode
};

#endif /* NEED_DAC1064 */

#ifdef CONFIG_FB_MATROX_MYSTIQUE
static int MGA1064_init(CPMINFO struct matrox_hw_state* hw, struct my_timming* m, struct display* p) {

	DBG("MGA1064_init")

	if (DAC1064_init_1(PMINFO hw, m, p)) return 1;
	if (matroxfb_vgaHWinit(PMINFO hw, m, p)) return 1;

	hw->MiscOutReg = 0xCB;
	if (m->sync & FB_SYNC_HOR_HIGH_ACT)
		hw->MiscOutReg &= ~0x40;
	if (m->sync & FB_SYNC_VERT_HIGH_ACT)
		hw->MiscOutReg &= ~0x80;
	if (m->sync & FB_SYNC_COMP_HIGH_ACT) /* should be only FB_SYNC_COMP */
		hw->CRTCEXT[3] |= 0x40;

	if (DAC1064_init_2(PMINFO hw, m, p)) return 1;
	return 0;
}
#endif

#ifdef CONFIG_FB_MATROX_G100
static int MGAG100_init(CPMINFO struct matrox_hw_state* hw, struct my_timming* m, struct display* p) {

	DBG("MGAG100_init")

	if (DAC1064_init_1(PMINFO hw, m, p)) return 1;
	hw->MXoptionReg &= ~0x2000;
	if (matroxfb_vgaHWinit(PMINFO hw, m, p)) return 1;

	hw->MiscOutReg = 0xEF;
	if (m->sync & FB_SYNC_HOR_HIGH_ACT)
		hw->MiscOutReg &= ~0x40;
	if (m->sync & FB_SYNC_VERT_HIGH_ACT)
		hw->MiscOutReg &= ~0x80;
	if (m->sync & FB_SYNC_COMP_HIGH_ACT) /* should be only FB_SYNC_COMP */
		hw->CRTCEXT[3] |= 0x40;

	if (DAC1064_init_2(PMINFO hw, m, p)) return 1;
	return 0;
}
#endif	/* G100 */

#ifdef CONFIG_FB_MATROX_MYSTIQUE
static void MGA1064_ramdac_init(WPMINFO struct matrox_hw_state* hw){

	DBG("MGA1064_ramdac_init");

	/* ACCESS_FBINFO(features.DAC1064.vco_freq_min) = 120000; */
	ACCESS_FBINFO(features.pll.vco_freq_min) = 62000;
	ACCESS_FBINFO(features.pll.ref_freq)	 = 14318;
	ACCESS_FBINFO(features.pll.feed_div_min) = 100;
	ACCESS_FBINFO(features.pll.feed_div_max) = 127;
	ACCESS_FBINFO(features.pll.in_div_min)	 = 1;
	ACCESS_FBINFO(features.pll.in_div_max)	 = 31;
	ACCESS_FBINFO(features.pll.post_shift_max) = 3;
	ACCESS_FBINFO(features.DAC1064.xvrefctrl) = DAC1064_XVREFCTRL_EXTERNAL;
	/* maybe cmdline MCLK= ?, doc says gclk=44MHz, mclk=66MHz... it was 55/83 with old values */
	DAC1064_setmclk(PMINFO hw, DAC1064_OPT_MDIV2 | DAC1064_OPT_GDIV3 | DAC1064_OPT_SCLK_PLL, 133333);
}
#endif

#ifdef CONFIG_FB_MATROX_G100
/* BIOS environ */
static int x7AF4 = 0x10;	/* flags, maybe 0x10 = SDRAM, 0x00 = SGRAM??? */
				/* G100 wants 0x10, G200 SGRAM does not care... */
#if 0
static int def50 = 0;	/* reg50, & 0x0F, & 0x3000 (only 0x0000, 0x1000, 0x2000 (0x3000 disallowed and treated as 0) */
#endif

static void MGAG100_progPixClock(CPMINFO int flags, int m, int n, int p){
	int reg;
	int selClk;
	int clk;

	DBG("MGAG100_progPixClock")

	outDAC1064(PMINFO M1064_XPIXCLKCTRL, inDAC1064(PMINFO M1064_XPIXCLKCTRL) | M1064_XPIXCLKCTRL_DIS |
		   M1064_XPIXCLKCTRL_PLL_UP);
	switch (flags & 3) {
		case 0:		reg = M1064_XPIXPLLAM; break;
		case 1:		reg = M1064_XPIXPLLBM; break;
		default:	reg = M1064_XPIXPLLCM; break;
	}
	outDAC1064(PMINFO reg++, m);
	outDAC1064(PMINFO reg++, n);
	outDAC1064(PMINFO reg, p);
	selClk = mga_inb(M_MISC_REG_READ) & ~0xC;
	/* there should be flags & 0x03 & case 0/1/else */
	/* and we should first select source and after that we should wait for PLL */
	/* and we are waiting for PLL with oscilator disabled... Is it right? */
	switch (flags & 0x03) {
		case 0x00:	break;
		case 0x01:	selClk |= 4; break;
		default:	selClk |= 0x0C; break;
	}
	mga_outb(M_MISC_REG, selClk);
	for (clk = 500000; clk; clk--) {
		if (inDAC1064(PMINFO M1064_XPIXPLLSTAT) & 0x40)
			break;
		udelay(10);
	};
	if (!clk)
		printk(KERN_ERR "matroxfb: Pixel PLL%c not locked after usual time\n", (reg-M1064_XPIXPLLAM-2)/4 + 'A');
	selClk = inDAC1064(PMINFO M1064_XPIXCLKCTRL) & ~M1064_XPIXCLKCTRL_SRC_MASK;
	switch (flags & 0x0C) {
		case 0x00:	selClk |= M1064_XPIXCLKCTRL_SRC_PCI; break;
		case 0x04:	selClk |= M1064_XPIXCLKCTRL_SRC_PLL; break;
		default:	selClk |= M1064_XPIXCLKCTRL_SRC_EXT; break;
	}
	outDAC1064(PMINFO M1064_XPIXCLKCTRL, selClk);
	outDAC1064(PMINFO M1064_XPIXCLKCTRL, inDAC1064(PMINFO M1064_XPIXCLKCTRL) & ~M1064_XPIXCLKCTRL_DIS);
}

static void MGAG100_setPixClock(CPMINFO int flags, int freq){
	unsigned int m, n, p;

	DBG("MGAG100_setPixClock")

	DAC1064_calcclock(PMINFO freq, ACCESS_FBINFO(max_pixel_clock), &m, &n, &p);
	MGAG100_progPixClock(PMINFO flags, m, n, p);
}
#endif

#ifdef CONFIG_FB_MATROX_MYSTIQUE
static int MGA1064_preinit(WPMINFO struct matrox_hw_state* hw){
	static const int vxres_mystique[] = { 512,        640, 768,  800,  832,  960,
					     1024, 1152, 1280,      1600, 1664, 1920,
					     2048,    0};
	DBG("MGA1064_preinit")

	/* ACCESS_FBINFO(capable.cfb4) = 0; ... preinitialized by 0 */
	ACCESS_FBINFO(capable.text) = 1;
	ACCESS_FBINFO(capable.vxres) = vxres_mystique;
	ACCESS_FBINFO(features.accel.has_cacheflush) = 1;
	ACCESS_FBINFO(cursor.timer.function) = matroxfb_DAC1064_flashcursor;

	ACCESS_FBINFO(primout) = &m1064;

	if (ACCESS_FBINFO(devflags.noinit))
		return 0;	/* do not modify settings */
	hw->MXoptionReg &= 0xC0000100;
	hw->MXoptionReg |= 0x00094E20;
	if (ACCESS_FBINFO(devflags.novga))
		hw->MXoptionReg &= ~0x00000100;
	if (ACCESS_FBINFO(devflags.nobios))
		hw->MXoptionReg &= ~0x40000000;
	if (ACCESS_FBINFO(devflags.nopciretry))
		hw->MXoptionReg |=  0x20000000;
	pci_write_config_dword(ACCESS_FBINFO(pcidev), PCI_OPTION_REG, hw->MXoptionReg);
	mga_setr(M_SEQ_INDEX, 0x01, 0x20);
	mga_outl(M_CTLWTST, 0x00000000);
	udelay(200);
	mga_outl(M_MACCESS, 0x00008000);
	udelay(100);
	mga_outl(M_MACCESS, 0x0000C000);
	return 0;
}

static void MGA1064_reset(WPMINFO struct matrox_hw_state* hw){

	DBG("MGA1064_reset");

	ACCESS_FBINFO(features.DAC1064.cursorimage) = ACCESS_FBINFO(video.len_usable) - 1024;
	if (ACCESS_FBINFO(devflags.hwcursor))
		ACCESS_FBINFO(video.len_usable) -= 1024;
	matroxfb_fastfont_init(MINFO);
	MGA1064_ramdac_init(PMINFO hw);
}
#endif

#ifdef CONFIG_FB_MATROX_G100
static int MGAG100_preinit(WPMINFO struct matrox_hw_state* hw){
	static const int vxres_g100[] = {  512,        640, 768,  800,  832,  960,
                                          1024, 1152, 1280,      1600, 1664, 1920,
                                          2048, 0};
        u_int32_t reg50;
#if 0
	u_int32_t q;
#endif

	DBG("MGAG100_preinit")

	/* there are some instabilities if in_div > 19 && vco < 61000 */
	if (ACCESS_FBINFO(devflags.g450dac)) {
		ACCESS_FBINFO(features.pll.vco_freq_min) = 130000;	/* my sample: >118 */
	} else {
		ACCESS_FBINFO(features.pll.vco_freq_min) = 62000;
	}
	ACCESS_FBINFO(features.pll.ref_freq)	 = 27000;
	ACCESS_FBINFO(features.pll.feed_div_min) = 7;
	ACCESS_FBINFO(features.pll.feed_div_max) = 127;
	ACCESS_FBINFO(features.pll.in_div_min)	 = 1;
	ACCESS_FBINFO(features.pll.in_div_max)	 = 31;
	ACCESS_FBINFO(features.pll.post_shift_max) = 3;
	ACCESS_FBINFO(features.DAC1064.xvrefctrl) = DAC1064_XVREFCTRL_G100_DEFAULT;
	/* ACCESS_FBINFO(capable.cfb4) = 0; ... preinitialized by 0 */
	ACCESS_FBINFO(capable.text) = 1;
	ACCESS_FBINFO(capable.vxres) = vxres_g100;
	ACCESS_FBINFO(features.accel.has_cacheflush) = 1;
	ACCESS_FBINFO(cursor.timer.function) = matroxfb_DAC1064_flashcursor;
	ACCESS_FBINFO(capable.plnwt) = ACCESS_FBINFO(devflags.accelerator) != FB_ACCEL_MATROX_MGAG100;

	ACCESS_FBINFO(primout) = &m1064;

	if (ACCESS_FBINFO(devflags.noinit))
		return 0;
	hw->MXoptionReg &= 0xC0000100;
	hw->MXoptionReg |= 0x00000020;
	if (ACCESS_FBINFO(devflags.novga))
		hw->MXoptionReg &= ~0x00000100;
	if (ACCESS_FBINFO(devflags.nobios))
		hw->MXoptionReg &= ~0x40000000;
	if (ACCESS_FBINFO(devflags.nopciretry))
		hw->MXoptionReg |=  0x20000000;
	pci_write_config_dword(ACCESS_FBINFO(pcidev), PCI_OPTION_REG, hw->MXoptionReg);
	DAC1064_setmclk(PMINFO hw, DAC1064_OPT_MDIV2 | DAC1064_OPT_GDIV3 | DAC1064_OPT_SCLK_PCI, 133333);

	if (ACCESS_FBINFO(devflags.accelerator) == FB_ACCEL_MATROX_MGAG100) {
		pci_read_config_dword(ACCESS_FBINFO(pcidev), 0x50, &reg50);
		reg50 &= ~0x3000;
		pci_write_config_dword(ACCESS_FBINFO(pcidev), 0x50, reg50);

		hw->MXoptionReg |= 0x1080;
		pci_write_config_dword(ACCESS_FBINFO(pcidev), PCI_OPTION_REG, hw->MXoptionReg);
		mga_outl(M_CTLWTST, 0x00000300);
		/* mga_outl(M_CTLWTST, 0x03258A31); */
		udelay(100);
		mga_outb(0x1C05, 0x00);
		mga_outb(0x1C05, 0x80);
		udelay(100);
		mga_outb(0x1C05, 0x40);
		mga_outb(0x1C05, 0xC0);
		udelay(100);
		reg50 &= ~0xFF;
		reg50 |=  0x07;
		pci_write_config_dword(ACCESS_FBINFO(pcidev), 0x50, reg50);
		/* it should help with G100 */
		mga_outb(M_GRAPHICS_INDEX, 6);
		mga_outb(M_GRAPHICS_DATA, (mga_inb(M_GRAPHICS_DATA) & 3) | 4);
		mga_setr(M_EXTVGA_INDEX, 0x03, 0x81);
		mga_setr(M_EXTVGA_INDEX, 0x04, 0x00);
		mga_writeb(ACCESS_FBINFO(video.vbase), 0x0000, 0xAA);
		mga_writeb(ACCESS_FBINFO(video.vbase), 0x0800, 0x55);
		mga_writeb(ACCESS_FBINFO(video.vbase), 0x4000, 0x55);
#if 0
		if (mga_readb(ACCESS_FBINFO(video.vbase), 0x0000) != 0xAA) {
			hw->MXoptionReg &= ~0x1000;
		}
#endif
		hw->MXoptionReg |= 0x00078020;
	} else 	if (ACCESS_FBINFO(devflags.accelerator) == FB_ACCEL_MATROX_MGAG200) {
		pci_read_config_dword(ACCESS_FBINFO(pcidev), 0x50, &reg50);
		reg50 &= ~0x3000;
		pci_write_config_dword(ACCESS_FBINFO(pcidev), 0x50, reg50);

		if (ACCESS_FBINFO(devflags.memtype) == -1)
			ACCESS_FBINFO(devflags.memtype) = 3;
		hw->MXoptionReg |= (ACCESS_FBINFO(devflags.memtype) & 7) << 10;
		if (ACCESS_FBINFO(devflags.sgram))
			hw->MXoptionReg |= 0x4000;
		mga_outl(M_CTLWTST, 0x042450A1);
		mga_outl(M_MEMRDBK, 0x00000108);
		udelay(200);
		mga_outl(M_MACCESS, 0x00000000);
		mga_outl(M_MACCESS, 0x00008000);
		udelay(100);
		mga_outw(M_MEMRDBK, 0x00000108);
		hw->MXoptionReg |= 0x00078020;
	} else {
		pci_read_config_dword(ACCESS_FBINFO(pcidev), 0x50, &reg50);
		reg50 &= ~0x00000100;
		reg50 |=  0x00000000;
		pci_write_config_dword(ACCESS_FBINFO(pcidev), 0x50, reg50);

		if (ACCESS_FBINFO(devflags.memtype) == -1)
			ACCESS_FBINFO(devflags.memtype) = 0;
		hw->MXoptionReg |= (ACCESS_FBINFO(devflags.memtype) & 7) << 10;
		if (ACCESS_FBINFO(devflags.sgram))
			hw->MXoptionReg |= 0x4000;
		mga_outl(M_CTLWTST, 0x042450A1);
		mga_outl(M_MEMRDBK, 0x00000108);
		udelay(200);
		mga_outl(M_MACCESS, 0x00000000);
		mga_outl(M_MACCESS, 0x00008000);
		udelay(100);
		mga_outl(M_MEMRDBK, 0x00000108);
		hw->MXoptionReg |= 0x00040020;
	}
	pci_write_config_dword(ACCESS_FBINFO(pcidev), PCI_OPTION_REG, hw->MXoptionReg);
	return 0;
}

static void MGAG100_reset(WPMINFO struct matrox_hw_state* hw){
	u_int8_t b;

	DBG("MGAG100_reset")

	ACCESS_FBINFO(features.DAC1064.cursorimage) = ACCESS_FBINFO(video.len_usable) - 1024;
	if (ACCESS_FBINFO(devflags.hwcursor))
		ACCESS_FBINFO(video.len_usable) -= 1024;
	matroxfb_fastfont_init(MINFO);

	{
#ifdef G100_BROKEN_IBM_82351
		u_int32_t d;

		find 1014/22 (IBM/82351); /* if found and bridging Matrox, do some strange stuff */
		pci_read_config_byte(ibm, PCI_SECONDARY_BUS, &b);
		if (b == ACCESS_FBINFO(pcidev)->bus->number) {
			pci_write_config_byte(ibm, PCI_COMMAND+1, 0);	/* disable back-to-back & SERR */
			pci_write_config_byte(ibm, 0x41, 0xF4);		/* ??? */
			pci_write_config_byte(ibm, PCI_IO_BASE, 0xF0);	/* ??? */
			pci_write_config_byte(ibm, PCI_IO_LIMIT, 0x00);	/* ??? */
		}
#endif
		if (!ACCESS_FBINFO(devflags.noinit)) {
			if (x7AF4 & 8) {
				hw->MXoptionReg |= 0x40;
				pci_write_config_dword(ACCESS_FBINFO(pcidev), PCI_OPTION_REG, hw->MXoptionReg);
			}
			mga_setr(M_EXTVGA_INDEX, 0x06, 0x50);
		}
	}
	DAC1064_setmclk(PMINFO hw, DAC1064_OPT_RESERVED | DAC1064_OPT_MDIV2 | DAC1064_OPT_GDIV1 | DAC1064_OPT_SCLK_PLL, 133333);
	if (ACCESS_FBINFO(devflags.noinit))
		return;
	MGAG100_setPixClock(PMINFO 4, 25175);
	MGAG100_setPixClock(PMINFO 5, 28322);
	if (x7AF4 & 0x10) {
		b = inDAC1064(PMINFO M1064_XGENIODATA) & ~1;
		outDAC1064(PMINFO M1064_XGENIODATA, b);
		b = inDAC1064(PMINFO M1064_XGENIOCTRL) | 1;
		outDAC1064(PMINFO M1064_XGENIOCTRL, b);
	}
}
#endif

#ifdef CONFIG_FB_MATROX_MYSTIQUE
static void MGA1064_restore(WPMINFO struct matrox_hw_state* hw, struct matrox_hw_state* oldhw, struct display* p) {
	int i;
	CRITFLAGS

	DBG("MGA1064_restore")

	CRITBEGIN

	pci_write_config_dword(ACCESS_FBINFO(pcidev), PCI_OPTION_REG, hw->MXoptionReg);
	mga_outb(M_IEN, 0x00);
	mga_outb(M_CACHEFLUSH, 0x00);

	CRITEND

	DAC1064_restore_1(PMINFO hw, oldhw);
	matroxfb_vgaHWrestore(PMINFO hw, oldhw);
	for (i = 0; i < 6; i++)
		mga_setr(M_EXTVGA_INDEX, i, hw->CRTCEXT[i]);
	DAC1064_restore_2(PMINFO hw, oldhw, p);
}
#endif

#ifdef CONFIG_FB_MATROX_G100
static void MGAG100_restore(WPMINFO struct matrox_hw_state* hw, struct matrox_hw_state* oldhw, struct display* p) {
	int i;
	CRITFLAGS

	DBG("MGAG100_restore")

	CRITBEGIN

	pci_write_config_dword(ACCESS_FBINFO(pcidev), PCI_OPTION_REG, hw->MXoptionReg);
	CRITEND

	DAC1064_restore_1(PMINFO hw, oldhw);
	matroxfb_vgaHWrestore(PMINFO hw, oldhw);
#ifdef CONFIG_FB_MATROX_32MB
	if (ACCESS_FBINFO(devflags.support32MB))
		mga_setr(M_EXTVGA_INDEX, 8, hw->CRTCEXT[8]);
#endif
	for (i = 0; i < 6; i++)
		mga_setr(M_EXTVGA_INDEX, i, hw->CRTCEXT[i]);
	DAC1064_restore_2(PMINFO hw, oldhw, p);
}
#endif

#ifdef CONFIG_FB_MATROX_MYSTIQUE
struct matrox_switch matrox_mystique = {
	MGA1064_preinit, MGA1064_reset, MGA1064_init, MGA1064_restore, DAC1064_selhwcursor
};
EXPORT_SYMBOL(matrox_mystique);
#endif

#ifdef CONFIG_FB_MATROX_G100
struct matrox_switch matrox_G100 = {
	MGAG100_preinit, MGAG100_reset, MGAG100_init, MGAG100_restore, DAC1064_selhwcursor
};
EXPORT_SYMBOL(matrox_G100);
#endif

#ifdef NEED_DAC1064
EXPORT_SYMBOL(DAC1064_global_init);
EXPORT_SYMBOL(DAC1064_global_restore);
#endif
