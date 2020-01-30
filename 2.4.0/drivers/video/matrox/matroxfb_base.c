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
 *               "Samuel Hocevar" <sam@via.ecp.fr>
 *                     Fixes
 *
 *               "Anton Altaparmakov" <AntonA@bigfoot.com>
 *                     G400 MAX/non-MAX distinction
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

/* make checkconfig does not check included files... */
#include <linux/config.h>

#include "matroxfb_base.h"
#include "matroxfb_misc.h"
#include "matroxfb_accel.h"
#include "matroxfb_DAC1064.h"
#include "matroxfb_Ti3026.h"
#include "matroxfb_maven.h"
#include "matroxfb_crtc2.h"
#include <linux/matroxfb.h>
#include <asm/uaccess.h>

#ifdef CONFIG_PPC
unsigned char nvram_read_byte(int);
static int default_vmode = VMODE_NVRAM;
static int default_cmode = CMODE_NVRAM;
#endif

static void matroxfb_unregister_device(struct matrox_fb_info* minfo);

/* --------------------------------------------------------------------- */

/*
 * card parameters
 */

/* --------------------------------------------------------------------- */

static struct fb_var_screeninfo vesafb_defined = {
	640,480,640,480,/* W,H, W, H (virtual) load xres,xres_virtual*/
	0,0,		/* virtual -> visible no offset */
	8,		/* depth -> load bits_per_pixel */
	0,		/* greyscale ? */
	{0,0,0},	/* R */
	{0,0,0},	/* G */
	{0,0,0},	/* B */
	{0,0,0},	/* transparency */
	0,		/* standard pixel format */
	FB_ACTIVATE_NOW,
	-1,-1,
	FB_ACCELF_TEXT,	/* accel flags */
	39721L,48L,16L,33L,10L,
	96L,2L,~0,	/* No sync info */
	FB_VMODE_NONINTERLACED,
	{0,0,0,0,0,0}
};



/* --------------------------------------------------------------------- */

static void matrox_pan_var(WPMINFO struct fb_var_screeninfo *var) {
	unsigned int pos;
	unsigned short p0, p1, p2;
#ifdef CONFIG_FB_MATROX_32MB
	unsigned int p3;
#endif
	struct display *disp;
	CRITFLAGS

	DBG("matrox_pan_var")

	if (ACCESS_FBINFO(dead))
		return;

	disp = ACCESS_FBINFO(currcon_display);
	if (disp->type == FB_TYPE_TEXT) {
		pos = var->yoffset / fontheight(disp) * disp->next_line / ACCESS_FBINFO(devflags.textstep) + var->xoffset / (fontwidth(disp)?fontwidth(disp):8);
	} else {
		pos = (var->yoffset * var->xres_virtual + var->xoffset) * ACCESS_FBINFO(curr.final_bppShift) / 32;
		pos += ACCESS_FBINFO(curr.ydstorg.chunks);
	}
	p0 = ACCESS_FBINFO(currenthw)->CRTC[0x0D] = pos & 0xFF;
	p1 = ACCESS_FBINFO(currenthw)->CRTC[0x0C] = (pos & 0xFF00) >> 8;
	p2 = ACCESS_FBINFO(currenthw)->CRTCEXT[0] = (ACCESS_FBINFO(currenthw)->CRTCEXT[0] & 0xB0) | ((pos >> 16) & 0x0F) | ((pos >> 14) & 0x40);
#ifdef CONFIG_FB_MATROX_32MB
	p3 = ACCESS_FBINFO(currenthw)->CRTCEXT[8] = pos >> 21;
#endif

	CRITBEGIN

	mga_setr(M_CRTC_INDEX, 0x0D, p0);
	mga_setr(M_CRTC_INDEX, 0x0C, p1);
#ifdef CONFIG_FB_MATROX_32MB
	if (ACCESS_FBINFO(devflags.support32MB))
		mga_setr(M_EXTVGA_INDEX, 0x08, p3);
#endif
	mga_setr(M_EXTVGA_INDEX, 0x00, p2);

	CRITEND
}

static void matroxfb_remove(WPMINFO int dummy) {
	/* Currently we are holding big kernel lock on all dead & usecount updates.
	 * Destroy everything after all users release it. Especially do not unregister
	 * framebuffer and iounmap memory, neither fbmem nor fbcon-cfb* does not check
	 * for device unplugged when in use.
	 * In future we should point mmio.vbase & video.vbase somewhere where we can
	 * write data without causing too much damage...
	 */

	ACCESS_FBINFO(dead) = 1;
	if (ACCESS_FBINFO(usecount)) {
		/* destroy it later */
		return;
	}
	matroxfb_unregister_device(MINFO);
	unregister_framebuffer(&ACCESS_FBINFO(fbcon));
	del_timer_sync(&ACCESS_FBINFO(cursor.timer));
#ifdef CONFIG_MTRR
	if (ACCESS_FBINFO(mtrr.vram_valid))
		mtrr_del(ACCESS_FBINFO(mtrr.vram), ACCESS_FBINFO(video.base), ACCESS_FBINFO(video.len));
#endif
	mga_iounmap(ACCESS_FBINFO(mmio.vbase));
	mga_iounmap(ACCESS_FBINFO(video.vbase));
	release_mem_region(ACCESS_FBINFO(video.base), ACCESS_FBINFO(video.len_maximum));
	release_mem_region(ACCESS_FBINFO(mmio.base), 16384);
#ifdef CONFIG_FB_MATROX_MULTIHEAD
	kfree(ACCESS_FBINFO(fbcon.disp));
	kfree(minfo);
#endif
}

	/*
	 * Open/Release the frame buffer device
	 */

static int matroxfb_open(struct fb_info *info, int user)
{
#define minfo ((struct matrox_fb_info*)info)
	DBG_LOOP("matroxfb_open")

	if (ACCESS_FBINFO(dead)) {
		return -ENXIO;
	}
	ACCESS_FBINFO(usecount)++;
#undef minfo
	return(0);
}

static int matroxfb_release(struct fb_info *info, int user)
{
#define minfo ((struct matrox_fb_info*)info)
	DBG_LOOP("matroxfb_release")

	if (!(--ACCESS_FBINFO(usecount)) && ACCESS_FBINFO(dead)) {
		matroxfb_remove(PMINFO 0);
	}
#undef minfo
	return(0);
}

static int matroxfb_pan_display(struct fb_var_screeninfo *var, int con,
		struct fb_info* info) {
#define minfo ((struct matrox_fb_info*)info)

	DBG("matroxfb_pan_display")

	if (var->vmode & FB_VMODE_YWRAP) {
		if (var->yoffset < 0 || var->yoffset >= fb_display[con].var.yres_virtual || var->xoffset)
			return -EINVAL;
	} else {
		if (var->xoffset+fb_display[con].var.xres > fb_display[con].var.xres_virtual ||
		    var->yoffset+fb_display[con].var.yres > fb_display[con].var.yres_virtual)
			return -EINVAL;
	}
	if (con == ACCESS_FBINFO(currcon))
		matrox_pan_var(PMINFO var);
	fb_display[con].var.xoffset = var->xoffset;
	fb_display[con].var.yoffset = var->yoffset;
	if (var->vmode & FB_VMODE_YWRAP)
		fb_display[con].var.vmode |= FB_VMODE_YWRAP;
	else
		fb_display[con].var.vmode &= ~FB_VMODE_YWRAP;
	return 0;
#undef minfo
}

static int matroxfb_updatevar(int con, struct fb_info *info)
{
#define minfo ((struct matrox_fb_info*)info)
	DBG("matroxfb_updatevar");

	matrox_pan_var(PMINFO &fb_display[con].var);
	return 0;
#undef minfo
}

static int matroxfb_get_final_bppShift(CPMINFO int bpp) {
	int bppshft2;

	DBG("matroxfb_get_final_bppShift")

	bppshft2 = bpp;
	if (!bppshft2) {
		return 8;
	}
	if (isInterleave(MINFO))
		bppshft2 >>= 1;
	if (ACCESS_FBINFO(devflags.video64bits))
		bppshft2 >>= 1;
	return bppshft2;
}

static int matroxfb_test_and_set_rounding(CPMINFO int xres, int bpp) {
	int over;
	int rounding;

	DBG("matroxfb_test_and_set_rounding")

	switch (bpp) {
		case 0:		return xres;
		case 4:		rounding = 128;
				break;
		case 8:		rounding = 64;	/* doc says 64; 32 is OK for G400 */
				break;
		case 16:	rounding = 32;
				break;
		case 24:	rounding = 64;	/* doc says 64; 32 is OK for G400 */
				break;
		default:	rounding = 16;
				/* on G400, 16 really does not work */
				if (ACCESS_FBINFO(devflags.accelerator) == FB_ACCEL_MATROX_MGAG400)
					rounding = 32;
				break;
	}
	if (isInterleave(MINFO)) {
		rounding *= 2;
	}
	over = xres % rounding;
	if (over)
		xres += rounding-over;
	return xres;
}

static int matroxfb_pitch_adjust(CPMINFO int xres, int bpp) {
	const int* width;
	int xres_new;

	DBG("matroxfb_pitch_adjust")

	if (!bpp) return xres;

	width = ACCESS_FBINFO(capable.vxres);

	if (ACCESS_FBINFO(devflags.precise_width)) {
		while (*width) {
			if ((*width >= xres) && (matroxfb_test_and_set_rounding(PMINFO *width, bpp) == *width)) {
				break;
			}
			width++;
		}
		xres_new = *width;
	} else {
		xres_new = matroxfb_test_and_set_rounding(PMINFO xres, bpp);
	}
	if (!xres_new) return 0;
	if (xres != xres_new) {
		printk(KERN_INFO "matroxfb: cannot set xres to %d, rounded up to %d\n", xres, xres_new);
	}
	return xres_new;
}

static int matroxfb_get_cmap_len(struct fb_var_screeninfo *var) {

	DBG("matroxfb_get_cmap_len")

	switch (var->bits_per_pixel) {
#ifdef FBCON_HAS_VGATEXT
		case 0:
			return 16;	/* pseudocolor... 16 entries HW palette */
#endif
#ifdef FBCON_HAS_CFB4
		case 4:
			return 16;	/* pseudocolor... 16 entries HW palette */
#endif
#ifdef FBCON_HAS_CFB8
		case 8:
			return 256;	/* pseudocolor... 256 entries HW palette */
#endif
#ifdef FBCON_HAS_CFB16
		case 16:
			return 16;	/* directcolor... 16 entries SW palette */
					/* Mystique: truecolor, 16 entries SW palette, HW palette hardwired into 1:1 mapping */
#endif
#ifdef FBCON_HAS_CFB24
		case 24:
			return 16;	/* directcolor... 16 entries SW palette */
					/* Mystique: truecolor, 16 entries SW palette, HW palette hardwired into 1:1 mapping */
#endif
#ifdef FBCON_HAS_CFB32
		case 32:
			return 16;	/* directcolor... 16 entries SW palette */
					/* Mystique: truecolor, 16 entries SW palette, HW palette hardwired into 1:1 mapping */
#endif
	}
	return 16;	/* return something reasonable... or panic()? */
}

static int matroxfb_decode_var(CPMINFO struct display* p, struct fb_var_screeninfo *var, int *visual, int *video_cmap_len, unsigned int* ydstorg) {
	unsigned int vramlen;
	unsigned int memlen;

	DBG("matroxfb_decode_var")

	switch (var->bits_per_pixel) {
#ifdef FBCON_HAS_VGATEXT
		case 0:	 if (!ACCESS_FBINFO(capable.text)) return -EINVAL;
			 break;
#endif
#ifdef FBCON_HAS_CFB4
		case 4:	 if (!ACCESS_FBINFO(capable.cfb4)) return -EINVAL;
			 break;
#endif
#ifdef FBCON_HAS_CFB8
		case 8:	 break;
#endif
#ifdef FBCON_HAS_CFB16
		case 16: break;
#endif
#ifdef FBCON_HAS_CFB24
		case 24: break;
#endif
#ifdef FBCON_HAS_CFB32
		case 32: break;
#endif
		default: return -EINVAL;
	}
	*ydstorg = 0;
	vramlen = ACCESS_FBINFO(video.len_usable);
	if (var->yres_virtual < var->yres)
		var->yres_virtual = var->yres;
	if (var->xres_virtual < var->xres)
		var->xres_virtual = var->xres;
	if (var->bits_per_pixel) {
		var->xres_virtual = matroxfb_pitch_adjust(PMINFO var->xres_virtual, var->bits_per_pixel);
		memlen = var->xres_virtual * var->bits_per_pixel * var->yres_virtual / 8;
		if (memlen > vramlen) {
			var->yres_virtual = vramlen * 8 / (var->xres_virtual * var->bits_per_pixel);
			memlen = var->xres_virtual * var->bits_per_pixel * var->yres_virtual / 8;
		}
		/* There is hardware bug that no line can cross 4MB boundary */
		/* give up for CFB24, it is impossible to easy workaround it */
		/* for other try to do something */
		if (!ACCESS_FBINFO(capable.cross4MB) && (memlen > 0x400000)) {
			if (var->bits_per_pixel == 24) {
				/* sorry */
			} else {
				unsigned int linelen;
				unsigned int m1 = linelen = var->xres_virtual * var->bits_per_pixel / 8;
				unsigned int m2 = PAGE_SIZE;	/* or 128 if you do not need PAGE ALIGNED address */
				unsigned int max_yres;

				while (m1) {
					int t;

					while (m2 >= m1) m2 -= m1;
					t = m1;
					m1 = m2;
					m2 = t;
				}
				m2 = linelen * PAGE_SIZE / m2;
				*ydstorg = m2 = 0x400000 % m2;
				max_yres = (vramlen - m2) / linelen;
				if (var->yres_virtual > max_yres)
					var->yres_virtual = max_yres;
			}
		}
	} else {
		matrox_text_round(PMINFO var, p);
#if 0
/* we must limit pixclock by mclk...
   Millennium I:    66 MHz = 15000
   Millennium II:   61 MHz = 16300
   Millennium G200: 83 MHz = 12000 */
		if (var->pixclock < 15000)
			var->pixclock = 15000;	/* limit for "normal" gclk & mclk */
#endif
	}
	/* YDSTLEN contains only signed 16bit value */
	if (var->yres_virtual > 32767)
		var->yres_virtual = 32767;
	/* we must round yres/xres down, we already rounded y/xres_virtual up
	   if it was possible. We should return -EINVAL, but I disagree */
	if (var->yres_virtual < var->yres)
		var->yres = var->yres_virtual;
	if (var->xres_virtual < var->xres)
		var->xres = var->xres_virtual;
	if (var->xoffset + var->xres > var->xres_virtual)
		var->xoffset = var->xres_virtual - var->xres;
	if (var->yoffset + var->yres > var->yres_virtual)
		var->yoffset = var->yres_virtual - var->yres;

	if (var->bits_per_pixel == 0) {
		var->red.offset = 0;
		var->red.length = 6;
		var->green.offset = 0;
		var->green.length = 6;
		var->blue.offset = 0;
		var->blue.length = 6;
		var->transp.offset = 0;
		var->transp.length = 0;
		*visual = MX_VISUAL_PSEUDOCOLOR;
	} else if (var->bits_per_pixel == 4) {
		var->red.offset = 0;
		var->red.length = 8;
		var->green.offset = 0;
		var->green.length = 8;
		var->blue.offset = 0;
		var->blue.length = 8;
		var->transp.offset = 0;
		var->transp.length = 0;
		*visual = MX_VISUAL_PSEUDOCOLOR;
	} else if (var->bits_per_pixel <= 8) {
		var->red.offset = 0;
		var->red.length = 8;
		var->green.offset = 0;
		var->green.length = 8;
		var->blue.offset = 0;
		var->blue.length = 8;
		var->transp.offset = 0;
		var->transp.length = 0;
		*visual = MX_VISUAL_PSEUDOCOLOR;
	} else {
		if (var->bits_per_pixel <= 16) {
			if (var->green.length == 5) {
				var->red.offset    = 10;
				var->red.length    = 5;
				var->green.offset  = 5;
				var->green.length  = 5;
				var->blue.offset   = 0;
				var->blue.length   = 5;
				var->transp.offset = 15;
				var->transp.length = 1;
			} else {
				var->red.offset    = 11;
				var->red.length    = 5;
				var->green.offset  = 5;
				var->green.length  = 6;
				var->blue.offset   = 0;
				var->blue.length   = 5;
				var->transp.offset = 0;
				var->transp.length = 0;
			}
		} else if (var->bits_per_pixel <= 24) {
			var->red.offset    = 16;
			var->red.length    = 8;
			var->green.offset  = 8;
			var->green.length  = 8;
			var->blue.offset   = 0;
			var->blue.length   = 8;
			var->transp.offset = 0;
			var->transp.length = 0;
		} else {
			var->red.offset    = 16;
			var->red.length    = 8;
			var->green.offset  = 8;
			var->green.length  = 8;
			var->blue.offset   = 0;
			var->blue.length   = 8;
			var->transp.offset = 24;
			var->transp.length = 8;
		}
		dprintk("matroxfb: truecolor: "
		       "size=%d:%d:%d:%d, shift=%d:%d:%d:%d\n",
		       var->transp.length,
		       var->red.length,
		       var->green.length,
		       var->blue.length,
		       var->transp.offset,
		       var->red.offset,
		       var->green.offset,
		       var->blue.offset);
		*visual = MX_VISUAL_DIRECTCOLOR;
	}
	*video_cmap_len = matroxfb_get_cmap_len(var);
	dprintk(KERN_INFO "requested %d*%d/%dbpp (%d*%d)\n", var->xres, var->yres, var->bits_per_pixel,
				var->xres_virtual, var->yres_virtual);
	return 0;
}

static int matrox_setcolreg(unsigned regno, unsigned red, unsigned green,
			    unsigned blue, unsigned transp,
			    struct fb_info *fb_info)
{
	struct display* p;
#ifdef CONFIG_FB_MATROX_MULTIHEAD
	struct matrox_fb_info* minfo = (struct matrox_fb_info*)fb_info;
#endif

	DBG("matrox_setcolreg")

	/*
	 *  Set a single color register. The values supplied are
	 *  already rounded down to the hardware's capabilities
	 *  (according to the entries in the `var' structure). Return
	 *  != 0 for invalid regno.
	 */

	if (regno >= ACCESS_FBINFO(curr.cmap_len))
		return 1;

	ACCESS_FBINFO(palette[regno].red)   = red;
	ACCESS_FBINFO(palette[regno].green) = green;
	ACCESS_FBINFO(palette[regno].blue)  = blue;
	ACCESS_FBINFO(palette[regno].transp) = transp;

	p = ACCESS_FBINFO(currcon_display);
	if (p->var.grayscale) {
		/* gray = 0.30*R + 0.59*G + 0.11*B */
		red = green = blue = (red * 77 + green * 151 + blue * 28) >> 8;
	}

	red = CNVT_TOHW(red, p->var.red.length);
	green = CNVT_TOHW(green, p->var.green.length);
	blue = CNVT_TOHW(blue, p->var.blue.length);
	transp = CNVT_TOHW(transp, p->var.transp.length);

	switch (p->var.bits_per_pixel) {
#if defined(FBCON_HAS_CFB8) || defined(FBCON_HAS_CFB4) || defined(FBCON_HAS_VGATEXT)
#ifdef FBCON_HAS_VGATEXT
	case 0:
#endif
#ifdef FBCON_HAS_CFB4
	case 4:
#endif
#ifdef FBCON_HAS_CFB8
	case 8:
#endif
		mga_outb(M_DAC_REG, regno);
		mga_outb(M_DAC_VAL, red);
		mga_outb(M_DAC_VAL, green);
		mga_outb(M_DAC_VAL, blue);
		break;
#endif
#ifdef FBCON_HAS_CFB16
	case 16:
		ACCESS_FBINFO(cmap.cfb16[regno]) =
			(red << p->var.red.offset)     |
			(green << p->var.green.offset) |
			(blue << p->var.blue.offset)   |
			(transp << p->var.transp.offset); /* for 1:5:5:5 */
		break;
#endif
#ifdef FBCON_HAS_CFB24
	case 24:
		ACCESS_FBINFO(cmap.cfb24[regno]) =
			(red   << p->var.red.offset)   |
			(green << p->var.green.offset) |
			(blue  << p->var.blue.offset);
		break;
#endif
#ifdef FBCON_HAS_CFB32
	case 32:
		ACCESS_FBINFO(cmap.cfb32[regno]) =
			(red   << p->var.red.offset)   |
			(green << p->var.green.offset) |
			(blue  << p->var.blue.offset)  |
			(transp << p->var.transp.offset);	/* 8:8:8:8 */
		break;
#endif
	}
	return 0;
}

static void do_install_cmap(WPMINFO struct display* dsp)
{
	DBG("do_install_cmap")

	if (dsp->cmap.len)
		fb_set_cmap(&dsp->cmap, 1, matrox_setcolreg, &ACCESS_FBINFO(fbcon));
	else
		fb_set_cmap(fb_default_cmap(ACCESS_FBINFO(curr.cmap_len)),
			    1, matrox_setcolreg, &ACCESS_FBINFO(fbcon));
}

static int matroxfb_get_fix(struct fb_fix_screeninfo *fix, int con,
			 struct fb_info *info)
{
	struct display* p;
	DBG("matroxfb_get_fix")

#define minfo ((struct matrox_fb_info*)info)

	if (ACCESS_FBINFO(dead)) {
		return -ENXIO;
	}

	if (con >= 0)
		p = fb_display + con;
	else
		p = ACCESS_FBINFO(fbcon.disp);

	memset(fix, 0, sizeof(struct fb_fix_screeninfo));
	strcpy(fix->id,"MATROX");

	fix->smem_start = ACCESS_FBINFO(video.base) + ACCESS_FBINFO(curr.ydstorg.bytes);
	fix->smem_len = ACCESS_FBINFO(video.len_usable) - ACCESS_FBINFO(curr.ydstorg.bytes);
	fix->type = p->type;
	fix->type_aux = p->type_aux;
	fix->visual = p->visual;
	fix->xpanstep = 8; /* 8 for 8bpp, 4 for 16bpp, 2 for 32bpp */
	fix->ypanstep = 1;
	fix->ywrapstep = 0;
	fix->line_length = p->line_length;
	fix->mmio_start = ACCESS_FBINFO(mmio.base);
	fix->mmio_len = ACCESS_FBINFO(mmio.len);
	fix->accel = ACCESS_FBINFO(devflags.accelerator);
	return 0;
#undef minfo
}

static int matroxfb_get_var(struct fb_var_screeninfo *var, int con,
			 struct fb_info *info)
{
#define minfo ((struct matrox_fb_info*)info)
	DBG("matroxfb_get_var")

	if(con < 0)
		*var=ACCESS_FBINFO(fbcon.disp)->var;
	else
		*var=fb_display[con].var;
	return 0;
#undef minfo
}

static int matroxfb_set_var(struct fb_var_screeninfo *var, int con,
			 struct fb_info *info)
{
#define minfo ((struct matrox_fb_info*)info)
	int err;
	int visual;
	int cmap_len;
	unsigned int ydstorg;
	struct display* display;
	int chgvar;

	DBG("matroxfb_set_var")

	if (ACCESS_FBINFO(dead)) {
		return -ENXIO;
	}

	if (con >= 0)
		display = fb_display + con;
	else
		display = ACCESS_FBINFO(fbcon.disp);
	if ((err = matroxfb_decode_var(PMINFO display, var, &visual, &cmap_len, &ydstorg)) != 0)
		return err;
	switch (var->activate & FB_ACTIVATE_MASK) {
		case FB_ACTIVATE_TEST:	return 0;
		case FB_ACTIVATE_NXTOPEN:	/* ?? */
		case FB_ACTIVATE_NOW:	break;	/* continue */
		default:		return -EINVAL; /* unknown */
	}
	if (con >= 0) {
		chgvar = ((display->var.xres != var->xres) ||
		    (display->var.yres != var->yres) ||
                    (display->var.xres_virtual != var->xres_virtual) ||
		    (display->var.yres_virtual != var->yres_virtual) ||
		    (display->var.bits_per_pixel != var->bits_per_pixel) ||
		    memcmp(&display->var.red, &var->red, sizeof(var->red)) ||
		    memcmp(&display->var.green, &var->green, sizeof(var->green)) ||
		    memcmp(&display->var.blue, &var->blue, sizeof(var->blue)));
	} else {
		chgvar = 0;
	}
	display->var = *var;
	/* cmap */
	display->screen_base = vaddr_va(ACCESS_FBINFO(video.vbase)) + ydstorg;
	display->visual = visual;
	display->ypanstep = 1;
	display->ywrapstep = 0;
	if (var->bits_per_pixel) {
		display->type = FB_TYPE_PACKED_PIXELS;
		display->type_aux = 0;
		display->next_line = display->line_length = (var->xres_virtual * var->bits_per_pixel) >> 3;
	} else {
		display->type = FB_TYPE_TEXT;
		display->type_aux = ACCESS_FBINFO(devflags.text_type_aux);
		display->next_line = display->line_length = (var->xres_virtual / (fontwidth(display)?fontwidth(display):8)) * ACCESS_FBINFO(devflags.textstep);
	}
	display->can_soft_blank = 1;
	display->inverse = ACCESS_FBINFO(devflags.inverse);
	/* conp, fb_info, vrows, cursor_x, cursor_y, fgcol, bgcol */
	/* next_plane, fontdata, _font*, userfont */
	initMatrox(PMINFO display);	/* dispsw */
	/* dispsw, scrollmode, yscroll */
	/* fgshift, bgshift, charmask */
	if (chgvar && info && info->changevar)
		info->changevar(con);
	if (con == ACCESS_FBINFO(currcon)) {
		unsigned int pos;

		ACCESS_FBINFO(curr.cmap_len) = cmap_len;
		if (display->type == FB_TYPE_TEXT) {
			/* textmode must be in first megabyte, so no ydstorg allowed */
			ACCESS_FBINFO(curr.ydstorg.bytes) = 0;
			ACCESS_FBINFO(curr.ydstorg.chunks) = 0;
			ACCESS_FBINFO(curr.ydstorg.pixels) = 0;
		} else {
			ydstorg += ACCESS_FBINFO(devflags.ydstorg);
			ACCESS_FBINFO(curr.ydstorg.bytes) = ydstorg;
			ACCESS_FBINFO(curr.ydstorg.chunks) = ydstorg >> (isInterleave(MINFO)?3:2);
			if (var->bits_per_pixel == 4)
				ACCESS_FBINFO(curr.ydstorg.pixels) = ydstorg;
			else
				ACCESS_FBINFO(curr.ydstorg.pixels) = (ydstorg * 8) / var->bits_per_pixel;
		}
		ACCESS_FBINFO(curr.final_bppShift) = matroxfb_get_final_bppShift(PMINFO var->bits_per_pixel);
		if (visual == MX_VISUAL_PSEUDOCOLOR) {
			int i;

			for (i = 0; i < 16; i++) {
				int j;

				j = color_table[i];
				ACCESS_FBINFO(palette[i].red)   = default_red[j];
				ACCESS_FBINFO(palette[i].green) = default_grn[j];
				ACCESS_FBINFO(palette[i].blue)  = default_blu[j];
			}
		}

		{	struct my_timming mt;
			struct matrox_hw_state* hw;
			struct matrox_hw_state* ohw;

			matroxfb_var2my(var, &mt);
			/* CRTC1 delays */
			switch (var->bits_per_pixel) {
				case  0:	mt.delay = 31 + 0; break;
				case 16:	mt.delay = 21 + 8; break;
				case 24:	mt.delay = 17 + 8; break;
				case 32:	mt.delay = 16 + 8; break;
				default:	mt.delay = 31 + 8; break;
			}

			hw = ACCESS_FBINFO(newhw);
			ohw = ACCESS_FBINFO(currenthw);

			/* copy last setting... */
			memcpy(hw, ohw, sizeof(*hw));

			del_timer_sync(&ACCESS_FBINFO(cursor.timer));
			ACCESS_FBINFO(cursor.state) = CM_ERASE;

			ACCESS_FBINFO(hw_switch->init(PMINFO hw, &mt, display));
			if (display->type == FB_TYPE_TEXT) {
				if (fontheight(display))
					pos = var->yoffset / fontheight(display) * display->next_line / ACCESS_FBINFO(devflags.textstep) + var->xoffset / (fontwidth(display)?fontwidth(display):8);
				else
					pos = 0;
			} else {
				pos = (var->yoffset * var->xres_virtual + var->xoffset) * ACCESS_FBINFO(curr.final_bppShift) / 32;
				pos += ACCESS_FBINFO(curr.ydstorg.chunks);
			}

			hw->CRTC[0x0D] = pos & 0xFF;
			hw->CRTC[0x0C] = (pos & 0xFF00) >> 8;
			hw->CRTCEXT[0] = (hw->CRTCEXT[0] & 0xF0) | ((pos >> 16) & 0x0F) | ((pos >> 14) & 0x40);
			hw->CRTCEXT[8] = pos >> 21;
			if (ACCESS_FBINFO(output.ph) & (MATROXFB_OUTPUT_CONN_PRIMARY | MATROXFB_OUTPUT_CONN_DFP)) {
				if (ACCESS_FBINFO(primout))
					ACCESS_FBINFO(primout)->compute(MINFO, &mt, hw);
			}
			if (ACCESS_FBINFO(output.ph) & MATROXFB_OUTPUT_CONN_SECONDARY) {
				down_read(&ACCESS_FBINFO(altout.lock));
				if (ACCESS_FBINFO(altout.output))
					ACCESS_FBINFO(altout.output)->compute(ACCESS_FBINFO(altout.device), &mt, hw);
				up_read(&ACCESS_FBINFO(altout.lock));
			}
			ACCESS_FBINFO(hw_switch->restore(PMINFO hw, ohw, display));
			if (ACCESS_FBINFO(output.ph) & (MATROXFB_OUTPUT_CONN_PRIMARY | MATROXFB_OUTPUT_CONN_DFP)) {
				if (ACCESS_FBINFO(primout))
					ACCESS_FBINFO(primout)->program(MINFO, hw);
			}
			if (ACCESS_FBINFO(output.ph) & MATROXFB_OUTPUT_CONN_SECONDARY) {
				down_read(&ACCESS_FBINFO(altout.lock));
				if (ACCESS_FBINFO(altout.output))
					ACCESS_FBINFO(altout.output)->program(ACCESS_FBINFO(altout.device), hw);
				up_read(&ACCESS_FBINFO(altout.lock));
			}
			ACCESS_FBINFO(cursor.redraw) = 1;
			ACCESS_FBINFO(currenthw) = hw;
			ACCESS_FBINFO(newhw) = ohw;
			if (ACCESS_FBINFO(output.ph) & (MATROXFB_OUTPUT_CONN_PRIMARY | MATROXFB_OUTPUT_CONN_DFP)) {
				if (ACCESS_FBINFO(primout))
					ACCESS_FBINFO(primout)->start(MINFO);
			}
			if (ACCESS_FBINFO(output.ph) & MATROXFB_OUTPUT_CONN_SECONDARY) {
				down_read(&ACCESS_FBINFO(altout.lock));
				if (ACCESS_FBINFO(altout.output))
					ACCESS_FBINFO(altout.output)->start(ACCESS_FBINFO(altout.device));
				up_read(&ACCESS_FBINFO(altout.lock));
			}
			matrox_cfbX_init(PMINFO display);
			do_install_cmap(PMINFO display);
#if defined(CONFIG_FB_COMPAT_XPMAC)
			if (console_fb_info == &ACCESS_FBINFO(fbcon)) {
				int vmode, cmode;

				display_info.width = var->xres;
				display_info.height = var->yres;
				display_info.depth = var->bits_per_pixel;
				display_info.pitch = (var->xres_virtual)*(var->bits_per_pixel)/8;
				if (mac_var_to_vmode(var, &vmode, &cmode))
					display_info.mode = 0;
				else
					display_info.mode = vmode;
				strcpy(display_info.name, ACCESS_FBINFO(matrox_name));
				display_info.fb_address = ACCESS_FBINFO(video.base);
				display_info.cmap_adr_address = 0;
				display_info.cmap_data_address = 0;
				display_info.disp_reg_address = ACCESS_FBINFO(mmio.base);
			}
#endif /* CONFIG_FB_COMPAT_XPMAC */
		}
	}
	return 0;
#undef minfo
}

static int matrox_getcolreg(unsigned regno, unsigned *red, unsigned *green,
			    unsigned *blue, unsigned *transp,
			    struct fb_info *info)
{

	DBG("matrox_getcolreg")

#define minfo ((struct matrox_fb_info*)info)
	/*
	 *  Read a single color register and split it into colors/transparent.
	 *  Return != 0 for invalid regno.
	 */

	if (regno >= ACCESS_FBINFO(curr.cmap_len))
		return 1;

	*red   = ACCESS_FBINFO(palette[regno].red);
	*green = ACCESS_FBINFO(palette[regno].green);
	*blue  = ACCESS_FBINFO(palette[regno].blue);
	*transp = ACCESS_FBINFO(palette[regno].transp);
	return 0;
#undef minfo
}

static int matroxfb_get_cmap(struct fb_cmap *cmap, int kspc, int con,
			     struct fb_info *info)
{
#define minfo ((struct matrox_fb_info*)info)
	struct display* dsp = (con < 0) ? ACCESS_FBINFO(fbcon.disp)
					: fb_display + con;

	DBG("matroxfb_get_cmap")

	if (ACCESS_FBINFO(dead)) {
		return -ENXIO;
	}

	if (con == ACCESS_FBINFO(currcon)) /* current console? */
		return fb_get_cmap(cmap, kspc, matrox_getcolreg, info);
	else if (dsp->cmap.len) /* non default colormap? */
		fb_copy_cmap(&dsp->cmap, cmap, kspc ? 0 : 2);
	else
		fb_copy_cmap(fb_default_cmap(matroxfb_get_cmap_len(&dsp->var)),
			     cmap, kspc ? 0 : 2);
	return 0;
#undef minfo
}

static int matroxfb_set_cmap(struct fb_cmap *cmap, int kspc, int con,
			     struct fb_info *info)
{
	unsigned int cmap_len;
	struct display* dsp = (con < 0) ? info->disp : (fb_display + con);
#define minfo ((struct matrox_fb_info*)info)

	DBG("matroxfb_set_cmap")

	if (ACCESS_FBINFO(dead)) {
		return -ENXIO;
	}

	cmap_len = matroxfb_get_cmap_len(&dsp->var);
	if (dsp->cmap.len != cmap_len) {
		int err;

		err = fb_alloc_cmap(&dsp->cmap, cmap_len, 0);
		if (err)
			return err;
	}
	if (con == ACCESS_FBINFO(currcon)) {			/* current console? */
		return fb_set_cmap(cmap, kspc, matrox_setcolreg, info);
	} else
		fb_copy_cmap(cmap, &dsp->cmap, kspc ? 0 : 1);
	return 0;
#undef minfo
}

static int matroxfb_switch(int con, struct fb_info *info);

static int matroxfb_get_vblank(CPMINFO struct fb_vblank *vblank)
{
	unsigned int sts1;

	memset(vblank, 0, sizeof(*vblank));
	vblank->flags = FB_VBLANK_HAVE_VCOUNT | FB_VBLANK_HAVE_VSYNC |
			FB_VBLANK_HAVE_VBLANK | FB_VBLANK_HAVE_HBLANK;
	sts1 = mga_inb(M_INSTS1);
	vblank->vcount = mga_inl(M_VCOUNT);
	/* BTW, on my PIII/450 with G400, reading M_INSTS1
	   byte makes this call about 12% slower (1.70 vs. 2.05 us
	   per ioctl()) */
	if (sts1 & 1)
		vblank->flags |= FB_VBLANK_HBLANKING;
	if (sts1 & 8)
		vblank->flags |= FB_VBLANK_VSYNCING;
	if (vblank->count >= ACCESS_FBINFO(currcon_display)->var.yres)
		vblank->flags |= FB_VBLANK_VBLANKING;
	vblank->hcount = 0;
	vblank->count = 0;
	return 0;
}

static int matroxfb_ioctl(struct inode *inode, struct file *file,
			  unsigned int cmd, unsigned long arg, int con,
			  struct fb_info *info)
{
#define minfo ((struct matrox_fb_info*)info)
	DBG("matroxfb_ioctl")

	if (ACCESS_FBINFO(dead)) {
		return -ENXIO;
	}

	switch (cmd) {
		case FBIOGET_VBLANK:
			{
				struct fb_vblank vblank;
				int err;

				err = matroxfb_get_vblank(PMINFO &vblank);
				if (err)
					return err;
				if (copy_to_user((struct fb_vblank*)arg, &vblank, sizeof(vblank)))
					return -EFAULT;
				return 0;
			}
		case MATROXFB_SET_OUTPUT_MODE:
			{
				struct matroxioc_output_mode mom;
				int val;

				if (copy_from_user(&mom, (struct matroxioc_output_mode*)arg, sizeof(mom)))
					return -EFAULT;
				if (mom.output >= sizeof(u_int32_t))
					return -EINVAL;
				switch (mom.output) {
					case MATROXFB_OUTPUT_PRIMARY:
						if (mom.mode != MATROXFB_OUTPUT_MODE_MONITOR)
							return -EINVAL;
						/* mode did not change... */
						return 0;
					case MATROXFB_OUTPUT_SECONDARY:
						val = -EINVAL;
						down_read(&ACCESS_FBINFO(altout.lock));
						if (ACCESS_FBINFO(altout.output) && ACCESS_FBINFO(altout.device))
							val = ACCESS_FBINFO(altout.output)->setmode(ACCESS_FBINFO(altout.device), mom.mode);
						up_read(&ACCESS_FBINFO(altout.lock));
						if (val != 1)
							return val;
						if (ACCESS_FBINFO(output.ph) & MATROXFB_OUTPUT_CONN_SECONDARY)
							matroxfb_switch(ACCESS_FBINFO(currcon), info);
						if (ACCESS_FBINFO(output.sh) & MATROXFB_OUTPUT_CONN_SECONDARY) {
							struct matroxfb_dh_fb_info* crtc2;

							down_read(&ACCESS_FBINFO(crtc2.lock));
							crtc2 = (struct matroxfb_dh_fb_info*)(ACCESS_FBINFO(crtc2.info));
							if (crtc2)
								crtc2->fbcon.switch_con(crtc2->currcon, &crtc2->fbcon);
							up_read(&ACCESS_FBINFO(crtc2.lock));
						}
						return 0;
					case MATROXFB_OUTPUT_DFP:
						if (!(ACCESS_FBINFO(output.all) & MATROXFB_OUTPUT_CONN_DFP))
							return -ENXIO;
						if (mom.mode!= MATROXFB_OUTPUT_MODE_MONITOR)
							return -EINVAL;
						/* mode did not change... */
						return 0;
					default:
						return -EINVAL;
				}
				return 0;
			}
		case MATROXFB_GET_OUTPUT_MODE:
			{
				struct matroxioc_output_mode mom;
				int val;

				if (copy_from_user(&mom, (struct matroxioc_output_mode*)arg, sizeof(mom)))
					return -EFAULT;
				if (mom.output >= sizeof(u_int32_t))
					return -EINVAL;
				switch (mom.output) {
					case MATROXFB_OUTPUT_PRIMARY:
						mom.mode = MATROXFB_OUTPUT_MODE_MONITOR;
						break;
					case MATROXFB_OUTPUT_SECONDARY:
						val = -EINVAL;
						down_read(&ACCESS_FBINFO(altout.lock));
						if (ACCESS_FBINFO(altout.output) && ACCESS_FBINFO(altout.device))
							val = ACCESS_FBINFO(altout.output)->getmode(ACCESS_FBINFO(altout.device), &mom.mode);
						up_read(&ACCESS_FBINFO(altout.lock));
						if (val)
							return val;
						break;
					case MATROXFB_OUTPUT_DFP:
						if (!(ACCESS_FBINFO(output.all) & MATROXFB_OUTPUT_CONN_DFP))
							return -ENXIO;
						mom.mode = MATROXFB_OUTPUT_MODE_MONITOR;
						break;
					default:
						return -EINVAL;
				}
				if (copy_to_user((struct matroxioc_output_mode*)arg, &mom, sizeof(mom)))
					return -EFAULT;
				return 0;
			}
		case MATROXFB_SET_OUTPUT_CONNECTION:
			{
				u_int32_t tmp;

				if (copy_from_user(&tmp, (u_int32_t*)arg, sizeof(tmp)))
					return -EFAULT;
				if (tmp & ~ACCESS_FBINFO(output.all))
					return -EINVAL;
				if (tmp & ACCESS_FBINFO(output.sh))
					return -EINVAL;
				if (tmp & MATROXFB_OUTPUT_CONN_DFP) {
					if (tmp & MATROXFB_OUTPUT_CONN_SECONDARY)
						return -EINVAL;
					if (ACCESS_FBINFO(output.sh))
						return -EINVAL;
				}
				if (tmp == ACCESS_FBINFO(output.ph))
					return 0;
				ACCESS_FBINFO(output.ph) = tmp;
				matroxfb_switch(ACCESS_FBINFO(currcon), info);
				return 0;
			}
		case MATROXFB_GET_OUTPUT_CONNECTION:
			{
				if (put_user(ACCESS_FBINFO(output.ph), (u_int32_t*)arg))
					return -EFAULT;
				return 0;
			}
		case MATROXFB_GET_AVAILABLE_OUTPUTS:
			{
				u_int32_t tmp;

				tmp = ACCESS_FBINFO(output.all) & ~ACCESS_FBINFO(output.sh);
				if (ACCESS_FBINFO(output.ph) & MATROXFB_OUTPUT_CONN_DFP)
					tmp &= ~MATROXFB_OUTPUT_CONN_SECONDARY;
				if (ACCESS_FBINFO(output.ph) & MATROXFB_OUTPUT_CONN_SECONDARY)
					tmp &= ~MATROXFB_OUTPUT_CONN_DFP;
				if (put_user(tmp, (u_int32_t*)arg))
					return -EFAULT;
				return 0;
			}
		case MATROXFB_GET_ALL_OUTPUTS:
			{
				if (put_user(ACCESS_FBINFO(output.all), (u_int32_t*)arg))
					return -EFAULT;
				return 0;
			}
	}
	return -EINVAL;
#undef minfo
}

static struct fb_ops matroxfb_ops = {
	owner:		THIS_MODULE,
	fb_open:	matroxfb_open,
	fb_release:	matroxfb_release,
	fb_get_fix:	matroxfb_get_fix,
	fb_get_var:	matroxfb_get_var,
	fb_set_var:	matroxfb_set_var,
	fb_get_cmap:	matroxfb_get_cmap,
	fb_set_cmap:	matroxfb_set_cmap,
	fb_pan_display:	matroxfb_pan_display,
	fb_ioctl:	matroxfb_ioctl,
};

static int matroxfb_switch(int con, struct fb_info *info)
{
#define minfo ((struct matrox_fb_info*)info)
	struct fb_cmap* cmap;
	struct display *p;

	DBG("matroxfb_switch");

	if (ACCESS_FBINFO(currcon) >= 0) {
		/* Do we have to save the colormap? */
		cmap = &(ACCESS_FBINFO(currcon_display)->cmap);
		dprintk(KERN_DEBUG "switch1: con = %d, cmap.len = %d\n", ACCESS_FBINFO(currcon), cmap->len);

		if (cmap->len) {
			dprintk(KERN_DEBUG "switch1a: %p %p %p %p\n", cmap->red, cmap->green, cmap->blue, cmap->transp);
			fb_get_cmap(cmap, 1, matrox_getcolreg, info);
#ifdef DEBUG
			if (cmap->red) {
				dprintk(KERN_DEBUG "switch1r: %X\n", cmap->red[0]);
			}
#endif
		}
	}
	ACCESS_FBINFO(currcon) = con;
	if (con < 0)
		p = ACCESS_FBINFO(fbcon.disp);
	else
		p = fb_display + con;
	ACCESS_FBINFO(currcon_display) = p;
	p->var.activate = FB_ACTIVATE_NOW;
#ifdef DEBUG
	cmap = &p->cmap;
	dprintk(KERN_DEBUG "switch2: con = %d, cmap.len = %d\n", con, cmap->len);
	dprintk(KERN_DEBUG "switch2a: %p %p %p %p\n", cmap->red, cmap->green, cmap->blue, cmap->transp);
	if (p->cmap.red) {
		dprintk(KERN_DEBUG "switch2r: %X\n", cmap->red[0]);
	}
#endif
	matroxfb_set_var(&p->var, con, info);
#ifdef DEBUG
	dprintk(KERN_DEBUG "switch3: con = %d, cmap.len = %d\n", con, cmap->len);
	dprintk(KERN_DEBUG "switch3a: %p %p %p %p\n", cmap->red, cmap->green, cmap->blue, cmap->transp);
	if (p->cmap.red) {
		dprintk(KERN_DEBUG "switch3r: %X\n", cmap->red[0]);
	}
#endif
	return 0;
#undef minfo
}

/* 0 unblank, 1 blank, 2 no vsync, 3 no hsync, 4 off */

static void matroxfb_blank(int blank, struct fb_info *info)
{
#define minfo ((struct matrox_fb_info*)info)
	int seq;
	int crtc;
	CRITFLAGS

	DBG("matroxfb_blank")

	if (ACCESS_FBINFO(dead))
		return;

	switch (blank) {
		case 1:  seq = 0x20; crtc = 0x00; break; /* works ??? */
		case 2:  seq = 0x20; crtc = 0x10; break;
		case 3:  seq = 0x20; crtc = 0x20; break;
		case 4:  seq = 0x20; crtc = 0x30; break;
		default: seq = 0x00; crtc = 0x00; break;
	}

	CRITBEGIN

	mga_outb(M_SEQ_INDEX, 1);
	mga_outb(M_SEQ_DATA, (mga_inb(M_SEQ_DATA) & ~0x20) | seq);
	mga_outb(M_EXTVGA_INDEX, 1);
	mga_outb(M_EXTVGA_DATA, (mga_inb(M_EXTVGA_DATA) & ~0x30) | crtc);

	CRITEND

#undef minfo
}

#define RSDepth(X)	(((X) >> 8) & 0x0F)
#define RS8bpp		0x1
#define RS15bpp		0x2
#define RS16bpp		0x3
#define RS32bpp		0x4
#define RS4bpp		0x5
#define RS24bpp		0x6
#define RSText		0x7
#define RSText8		0x8
/* 9-F */
static struct { struct fb_bitfield red, green, blue, transp; int bits_per_pixel; } colors[] = {
	{ {  0, 8, 0}, { 0, 8, 0}, { 0, 8, 0}, {  0, 0, 0},  8 },
	{ { 10, 5, 0}, { 5, 5, 0}, { 0, 5, 0}, { 15, 1, 0}, 16 },
	{ { 11, 5, 0}, { 5, 6, 0}, { 0, 5, 0}, {  0, 0, 0}, 16 },
	{ { 16, 8, 0}, { 8, 8, 0}, { 0, 8, 0}, { 24, 8, 0}, 32 },
	{ {  0, 8, 0}, { 0, 8, 0}, { 0, 8, 0}, {  0, 0, 0},  4 },
	{ { 16, 8, 0}, { 8, 8, 0}, { 0, 8, 0}, {  0, 0, 0}, 24 },
	{ {  0, 6, 0}, { 0, 6, 0}, { 0, 6, 0}, {  0, 0, 0},  0 },	/* textmode with (default) VGA8x16 */
	{ {  0, 6, 0}, { 0, 6, 0}, { 0, 6, 0}, {  0, 0, 0},  0 },	/* textmode hardwired to VGA8x8 */
};

/* initialized by setup, see explanation at end of file (search for MODULE_PARM_DESC) */
static unsigned int mem;		/* "matrox:mem:xxxxxM" */
static int option_precise_width = 1;	/* cannot be changed, option_precise_width==0 must imply noaccel */
static int inv24;			/* "matrox:inv24" */
static int cross4MB = -1;		/* "matrox:cross4MB" */
static int disabled;			/* "matrox:disabled" */
static int noaccel;			/* "matrox:noaccel" */
static int nopan;			/* "matrox:nopan" */
static int no_pci_retry;		/* "matrox:nopciretry" */
static int novga;			/* "matrox:novga" */
static int nobios;			/* "matrox:nobios" */
static int noinit = 1;			/* "matrox:init" */
static int inverse;			/* "matrox:inverse" */
static int hwcursor = 1;		/* "matrox:nohwcursor" */
static int blink = 1;			/* "matrox:noblink" */
static int sgram;			/* "matrox:sgram" */
#ifdef CONFIG_MTRR
static int mtrr = 1;			/* "matrox:nomtrr" */
#endif
static int grayscale;			/* "matrox:grayscale" */
static unsigned int fastfont;		/* "matrox:fastfont:xxxxx" */
static int dev = -1;			/* "matrox:dev:xxxxx" */
static unsigned int vesa = ~0;		/* "matrox:vesa:xxxxx" */
static int depth = -1;			/* "matrox:depth:xxxxx" */
static unsigned int xres;		/* "matrox:xres:xxxxx" */
static unsigned int yres;		/* "matrox:yres:xxxxx" */
static unsigned int upper = ~0;		/* "matrox:upper:xxxxx" */
static unsigned int lower = ~0;		/* "matrox:lower:xxxxx" */
static unsigned int vslen;		/* "matrox:vslen:xxxxx" */
static unsigned int left = ~0;		/* "matrox:left:xxxxx" */
static unsigned int right = ~0;		/* "matrox:right:xxxxx" */
static unsigned int hslen;		/* "matrox:hslen:xxxxx" */
static unsigned int pixclock;		/* "matrox:pixclock:xxxxx" */
static int sync = -1;			/* "matrox:sync:xxxxx" */
static unsigned int fv;			/* "matrox:fv:xxxxx" */
static unsigned int fh;			/* "matrox:fh:xxxxxk" */
static unsigned int maxclk;		/* "matrox:maxclk:xxxxM" */
static int dfp;				/* "matrox:dfp */
static int memtype = -1;		/* "matrox:memtype:xxx" */
static char fontname[64];		/* "matrox:font:xxxxx" */

#ifndef MODULE
static char videomode[64];		/* "matrox:mode:xxxxx" or "matrox:xxxxx" */
#endif

static int matroxfb_getmemory(WPMINFO unsigned int maxSize, unsigned int *realSize){
	vaddr_t vm;
	unsigned int offs;
	unsigned int offs2;
	unsigned char store;
	unsigned char bytes[32];
	unsigned char* tmp;

	DBG("matroxfb_getmemory")

	vm = ACCESS_FBINFO(video.vbase);
	maxSize &= ~0x1FFFFF;	/* must be X*2MB (really it must be 2 or X*4MB) */
	/* at least 2MB */
	if (maxSize < 0x0200000) return 0;
	if (maxSize > 0x2000000) maxSize = 0x2000000;

	mga_outb(M_EXTVGA_INDEX, 0x03);
	mga_outb(M_EXTVGA_DATA, mga_inb(M_EXTVGA_DATA) | 0x80);

	store = mga_readb(vm, 0x1234);
	tmp = bytes;
	for (offs = 0x100000; offs < maxSize; offs += 0x200000)
		*tmp++ = mga_readb(vm, offs);
	for (offs = 0x100000; offs < maxSize; offs += 0x200000)
		mga_writeb(vm, offs, 0x02);
	if (ACCESS_FBINFO(features.accel.has_cacheflush))
		mga_outb(M_CACHEFLUSH, 0x00);
	else
		mga_writeb(vm, 0x1234, 0x99);
	for (offs = 0x100000; offs < maxSize; offs += 0x200000) {
		if (mga_readb(vm, offs) != 0x02)
			break;
		mga_writeb(vm, offs, mga_readb(vm, offs) - 0x02);
		if (mga_readb(vm, offs))
			break;
	}
	tmp = bytes;
	for (offs2 = 0x100000; offs2 < maxSize; offs2 += 0x200000)
		mga_writeb(vm, offs2, *tmp++);
	mga_writeb(vm, 0x1234, store);

	mga_outb(M_EXTVGA_INDEX, 0x03);
	mga_outb(M_EXTVGA_DATA, mga_inb(M_EXTVGA_DATA) & ~0x80);

	*realSize = offs - 0x100000;
#ifdef CONFIG_FB_MATROX_MILLENIUM
	ACCESS_FBINFO(interleave) = !(!isMillenium(MINFO) || ((offs - 0x100000) & 0x3FFFFF));
#endif
	return 1;
}

struct video_board {
	int maxvram;
	int maxdisplayable;
	int accelID;
	struct matrox_switch* lowlevel;
		 };
#ifdef CONFIG_FB_MATROX_MILLENIUM
static struct video_board vbMillennium		= {0x0800000, 0x0800000, FB_ACCEL_MATROX_MGA2064W,	&matrox_millennium};
static struct video_board vbMillennium2		= {0x1000000, 0x0800000, FB_ACCEL_MATROX_MGA2164W,	&matrox_millennium};
static struct video_board vbMillennium2A	= {0x1000000, 0x0800000, FB_ACCEL_MATROX_MGA2164W_AGP,	&matrox_millennium};
#endif	/* CONFIG_FB_MATROX_MILLENIUM */
#ifdef CONFIG_FB_MATROX_MYSTIQUE
static struct video_board vbMystique		= {0x0800000, 0x0800000, FB_ACCEL_MATROX_MGA1064SG,	&matrox_mystique};
#endif	/* CONFIG_FB_MATROX_MYSTIQUE */
#ifdef CONFIG_FB_MATROX_G100
static struct video_board vbG100		= {0x0800000, 0x0800000, FB_ACCEL_MATROX_MGAG100,	&matrox_G100};
static struct video_board vbG200		= {0x1000000, 0x1000000, FB_ACCEL_MATROX_MGAG200,	&matrox_G100};
#ifdef CONFIG_FB_MATROX_32MB
/* from doc it looks like that accelerator can draw only to low 16MB :-( Direct accesses & displaying are OK for
   whole 32MB */
static struct video_board vbG400		= {0x2000000, 0x1000000, FB_ACCEL_MATROX_MGAG400,	&matrox_G100};
#else
static struct video_board vbG400		= {0x2000000, 0x1000000, FB_ACCEL_MATROX_MGAG400,	&matrox_G100};
#endif
#endif

#define DEVF_VIDEO64BIT		0x0001
#define	DEVF_SWAPS		0x0002
#define DEVF_MILLENNIUM		0x0004
#define	DEVF_MILLENNIUM2	0x0008
#define DEVF_CROSS4MB		0x0010
#define DEVF_TEXT4B		0x0020
#define DEVF_DDC_8_2		0x0040
#define DEVF_DMA		0x0080
#define DEVF_SUPPORT32MB	0x0100
#define DEVF_ANY_VXRES		0x0200
#define DEVF_TEXT16B		0x0400
#define DEVF_CRTC2		0x0800
#define DEVF_MAVEN_CAPABLE	0x1000
#define DEVF_PANELLINK_CAPABLE	0x2000
#define DEVF_G450DAC		0x4000

#define DEVF_GCORE	(DEVF_VIDEO64BIT | DEVF_SWAPS | DEVF_CROSS4MB | DEVF_DDC_8_2)
#define DEVF_G2CORE	(DEVF_GCORE | DEVF_ANY_VXRES | DEVF_MAVEN_CAPABLE | DEVF_PANELLINK_CAPABLE)
#define DEVF_G100	(DEVF_GCORE) /* no doc, no vxres... */
#define DEVF_G200	(DEVF_G2CORE)
#define DEVF_G400	(DEVF_G2CORE | DEVF_SUPPORT32MB | DEVF_TEXT16B | DEVF_CRTC2)
/* if you'll find how to drive DFP... */
#define DEVF_G450	(DEVF_GCORE | DEVF_ANY_VXRES | DEVF_SUPPORT32MB | DEVF_TEXT16B | DEVF_CRTC2 | DEVF_G450DAC)

static struct board {
	unsigned short vendor, device, rev, svid, sid;
	unsigned int flags;
	unsigned int maxclk;
	struct video_board* base;
	const char* name;
		} dev_list[] = {
#ifdef CONFIG_FB_MATROX_MILLENIUM
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_MIL,	0xFF,
		0,			0,
		DEVF_MILLENNIUM | DEVF_TEXT4B,
		230000,
		&vbMillennium,
		"Millennium (PCI)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_MIL_2,	0xFF,
		0,			0,
		DEVF_MILLENNIUM | DEVF_MILLENNIUM2 | DEVF_SWAPS,
		220000,
		&vbMillennium2,
		"Millennium II (PCI)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_MIL_2_AGP,	0xFF,
		0,			0,
		DEVF_MILLENNIUM | DEVF_MILLENNIUM2 | DEVF_SWAPS,
		250000,
		&vbMillennium2A,
		"Millennium II (AGP)"},
#endif
#ifdef CONFIG_FB_MATROX_MYSTIQUE
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_MYS,	0x02,
		0,			0,
		DEVF_VIDEO64BIT,
		180000,
		&vbMystique,
		"Mystique (PCI)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_MYS,	0xFF,
		0,			0,
		DEVF_VIDEO64BIT | DEVF_SWAPS,
		220000,
		&vbMystique,
		"Mystique 220 (PCI)"},
#endif
#ifdef CONFIG_FB_MATROX_G100
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G100,	0xFF,
		PCI_SS_VENDOR_ID_MATROX,	PCI_SS_ID_MATROX_MGA_G100_PCI,
		DEVF_G100,
		230000,
		&vbG100,
		"MGA-G100 (PCI)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G100,	0xFF,
		0,			0,
		DEVF_G100,
		230000,
		&vbG100,
		"unknown G100 (PCI)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G100_AGP,	0xFF,
		PCI_SS_VENDOR_ID_MATROX,	PCI_SS_ID_MATROX_GENERIC,
		DEVF_G100,
		230000,
		&vbG100,
		"MGA-G100 (AGP)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G100_AGP,	0xFF,
		PCI_SS_VENDOR_ID_MATROX,	PCI_SS_ID_MATROX_MGA_G100_AGP,
		DEVF_G100,
		230000,
		&vbG100,
		"MGA-G100 (AGP)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G100_AGP,	0xFF,
		PCI_SS_VENDOR_ID_SIEMENS_NIXDORF,	PCI_SS_ID_SIEMENS_MGA_G100_AGP,
		DEVF_G100,
		230000,
		&vbG100,
		"MGA-G100 (AGP)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G100_AGP,	0xFF,
		PCI_SS_VENDOR_ID_MATROX,	PCI_SS_ID_MATROX_PRODUCTIVA_G100_AGP,
		DEVF_G100,
		230000,
		&vbG100,
		"Productiva G100 (AGP)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G100_AGP,	0xFF,
		0,			0,
		DEVF_G100,
		230000,
		&vbG100,
		"unknown G100 (AGP)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G200_PCI,	0xFF,
		0,			0,
		DEVF_G200,
		250000,
		&vbG200,
		"unknown G200 (PCI)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G200_AGP,	0xFF,
		PCI_SS_VENDOR_ID_MATROX,	PCI_SS_ID_MATROX_GENERIC,
		DEVF_G200,
		220000,
		&vbG200,
		"MGA-G200 (AGP)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G200_AGP,	0xFF,
		PCI_SS_VENDOR_ID_MATROX,	PCI_SS_ID_MATROX_MYSTIQUE_G200_AGP,
		DEVF_G200,
		230000,
		&vbG200,
		"Mystique G200 (AGP)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G200_AGP,	0xFF,
		PCI_SS_VENDOR_ID_MATROX,	PCI_SS_ID_MATROX_MILLENIUM_G200_AGP,
		DEVF_G200,
		250000,
		&vbG200,
		"Millennium G200 (AGP)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G200_AGP,	0xFF,
		PCI_SS_VENDOR_ID_MATROX,	PCI_SS_ID_MATROX_MARVEL_G200_AGP,
		DEVF_G200,
		230000,
		&vbG200,
		"Marvel G200 (AGP)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G200_AGP,	0xFF,
		PCI_SS_VENDOR_ID_SIEMENS_NIXDORF,	PCI_SS_ID_SIEMENS_MGA_G200_AGP,
		DEVF_G200,
		230000,
		&vbG200,
		"MGA-G200 (AGP)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G200_AGP,	0xFF,
		0,			0,
		DEVF_G200,
		230000,
		&vbG200,
		"unknown G200 (AGP)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G400_AGP,	0x80,
		PCI_SS_VENDOR_ID_MATROX,	PCI_SS_ID_MATROX_MILLENNIUM_G400_MAX_AGP,
		DEVF_G400,
		360000,
		&vbG400,
		"Millennium G400 MAX (AGP)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G400_AGP,	0x80,
		0,			0,
		DEVF_G400,
		300000,
		&vbG400,
		"unknown G400 (AGP)"},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G400_AGP,	0xFF,
		0,			0,
		DEVF_G450,
		500000,		/* ??? vco goes up to 900MHz... */
		&vbG400,
		"unknown G450 (AGP)"},
#endif
	{0,			0,				0xFF,
		0,			0,
		0,
		0,
		NULL,
		NULL}};

#ifndef MODULE
static struct fb_videomode defaultmode = {
	/* 640x480 @ 60Hz, 31.5 kHz */
	NULL, 60, 640, 480, 39721, 40, 24, 32, 11, 96, 2,
	0, FB_VMODE_NONINTERLACED
};
#endif /* !MODULE */

static int hotplug = 0;

static int initMatrox2(WPMINFO struct display* d, struct board* b){
	unsigned long ctrlptr_phys = 0;
	unsigned long video_base_phys = 0;
	unsigned int memsize;
	struct matrox_hw_state* hw = ACCESS_FBINFO(currenthw);
	int err;

	DBG("initMatrox2")

	/* set default values... */
	vesafb_defined.accel_flags = FB_ACCELF_TEXT;

	ACCESS_FBINFO(hw_switch) = b->base->lowlevel;
	ACCESS_FBINFO(devflags.accelerator) = b->base->accelID;
	ACCESS_FBINFO(max_pixel_clock) = b->maxclk;

	printk(KERN_INFO "matroxfb: Matrox %s detected\n", b->name);
	ACCESS_FBINFO(capable.plnwt) = 1;
	ACCESS_FBINFO(devflags.video64bits) = b->flags & DEVF_VIDEO64BIT;
	if (b->flags & DEVF_TEXT4B) {
		ACCESS_FBINFO(devflags.vgastep) = 4;
		ACCESS_FBINFO(devflags.textmode) = 4;
		ACCESS_FBINFO(devflags.text_type_aux) = FB_AUX_TEXT_MGA_STEP16;
	} else if (b->flags & DEVF_TEXT16B) {
		ACCESS_FBINFO(devflags.vgastep) = 16;
		ACCESS_FBINFO(devflags.textmode) = 1;
		ACCESS_FBINFO(devflags.text_type_aux) = FB_AUX_TEXT_MGA_STEP16;
	} else {
		ACCESS_FBINFO(devflags.vgastep) = 8;
		ACCESS_FBINFO(devflags.textmode) = 1;
		ACCESS_FBINFO(devflags.text_type_aux) = FB_AUX_TEXT_MGA_STEP8;
	}
#ifdef CONFIG_FB_MATROX_32MB
	ACCESS_FBINFO(devflags.support32MB) = b->flags & DEVF_SUPPORT32MB;
#endif
	ACCESS_FBINFO(devflags.precise_width) = !(b->flags & DEVF_ANY_VXRES);
	ACCESS_FBINFO(devflags.crtc2) = b->flags & DEVF_CRTC2;
	ACCESS_FBINFO(devflags.maven_capable) = b->flags & DEVF_MAVEN_CAPABLE;
	if (b->flags & DEVF_PANELLINK_CAPABLE) {
		ACCESS_FBINFO(output.all) |= MATROXFB_OUTPUT_CONN_DFP;
		if (dfp)
			ACCESS_FBINFO(output.ph) |= MATROXFB_OUTPUT_CONN_DFP;
	}
	ACCESS_FBINFO(devflags.g450dac) = b->flags & DEVF_G450DAC;
	ACCESS_FBINFO(devflags.textstep) = ACCESS_FBINFO(devflags.vgastep) * ACCESS_FBINFO(devflags.textmode);
	ACCESS_FBINFO(devflags.textvram) = 65536 / ACCESS_FBINFO(devflags.textmode);

	if (ACCESS_FBINFO(capable.cross4MB) < 0)
		ACCESS_FBINFO(capable.cross4MB) = b->flags & DEVF_CROSS4MB;
	if (b->flags & DEVF_SWAPS) {
		ctrlptr_phys = pci_resource_start(ACCESS_FBINFO(pcidev), 1);
		video_base_phys = pci_resource_start(ACCESS_FBINFO(pcidev), 0);
	} else {
		ctrlptr_phys = pci_resource_start(ACCESS_FBINFO(pcidev), 0);
		video_base_phys = pci_resource_start(ACCESS_FBINFO(pcidev), 1);
	}
	err = -EINVAL;
	if (!ctrlptr_phys) {
		printk(KERN_ERR "matroxfb: control registers are not available, matroxfb disabled\n");
		goto fail;
	}
	if (!video_base_phys) {
		printk(KERN_ERR "matroxfb: video RAM is not available in PCI address space, matroxfb disabled\n");
		goto fail;
	}
	memsize = b->base->maxvram;
	if (!request_mem_region(ctrlptr_phys, 16384, "matroxfb MMIO")) {
		goto fail;
	}
	if (!request_mem_region(video_base_phys, memsize, "matroxfb FB")) {
		goto failCtrlMR;
	}
	ACCESS_FBINFO(video.len_maximum) = memsize;
	/* convert mem (autodetect k, M) */
	if (mem < 1024) mem *= 1024;
	if (mem < 0x00100000) mem *= 1024;

	if (mem && (mem < memsize))
		memsize = mem;
	err = -ENOMEM;
	if (mga_ioremap(ctrlptr_phys, 16384, MGA_IOREMAP_MMIO, &ACCESS_FBINFO(mmio.vbase))) {
		printk(KERN_ERR "matroxfb: cannot ioremap(%lX, 16384), matroxfb disabled\n", ctrlptr_phys);
		goto failVideoMR;
	}
	ACCESS_FBINFO(mmio.base) = ctrlptr_phys;
	ACCESS_FBINFO(mmio.len) = 16384;
	ACCESS_FBINFO(video.base) = video_base_phys;
	if (mga_ioremap(video_base_phys, memsize, MGA_IOREMAP_FB, &ACCESS_FBINFO(video.vbase))) {
		printk(KERN_ERR "matroxfb: cannot ioremap(%lX, %d), matroxfb disabled\n",
			video_base_phys, memsize);
		goto failCtrlIO;
	}
	{
		u_int32_t cmd;
		u_int32_t mga_option;

		pci_read_config_dword(ACCESS_FBINFO(pcidev), PCI_OPTION_REG, &mga_option);
		pci_read_config_dword(ACCESS_FBINFO(pcidev), PCI_COMMAND, &cmd);
		mga_option &= 0x7FFFFFFF; /* clear BIG_ENDIAN */
		mga_option |= MX_OPTION_BSWAP;
                /* disable palette snooping */
                cmd &= ~PCI_COMMAND_VGA_PALETTE;
		if (pci_find_device(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82437, NULL)) {
			if (!(mga_option & 0x20000000) && !ACCESS_FBINFO(devflags.nopciretry)) {
				printk(KERN_WARNING "matroxfb: Disabling PCI retries due to i82437 present\n");
			}
			mga_option |= 0x20000000;
			ACCESS_FBINFO(devflags.nopciretry) = 1;
		}
		pci_write_config_dword(ACCESS_FBINFO(pcidev), PCI_COMMAND, cmd);
		pci_write_config_dword(ACCESS_FBINFO(pcidev), PCI_OPTION_REG, mga_option);
		hw->MXoptionReg = mga_option;

		/* select non-DMA memory for PCI_MGA_DATA, otherwise dump of PCI cfg space can lock PCI bus */
		/* maybe preinit() candidate, but it is same... for all devices... at this time... */
		pci_write_config_dword(ACCESS_FBINFO(pcidev), PCI_MGA_INDEX, 0x00003C00);
	}

	err = -ENXIO;
	if (ACCESS_FBINFO(hw_switch)->preinit(PMINFO hw)) {
		goto failVideoIO;
	}

	err = -ENOMEM;
	if (!matroxfb_getmemory(PMINFO memsize, &ACCESS_FBINFO(video.len)) || !ACCESS_FBINFO(video.len)) {
		printk(KERN_ERR "matroxfb: cannot determine memory size\n");
		goto failVideoIO;
	}
	ACCESS_FBINFO(devflags.ydstorg) = 0;

	ACCESS_FBINFO(currcon) = -1;
	ACCESS_FBINFO(currcon_display) = d;
	mga_iounmap(ACCESS_FBINFO(video.vbase));
	ACCESS_FBINFO(video.base) = video_base_phys;
	if (mga_ioremap(video_base_phys, ACCESS_FBINFO(video.len), MGA_IOREMAP_FB, &ACCESS_FBINFO(video.vbase))) {
		printk(KERN_ERR "matroxfb: cannot ioremap(%lX, %d), matroxfb disabled\n",
			video_base_phys, ACCESS_FBINFO(video.len));
		goto failCtrlIO;
	}
	ACCESS_FBINFO(video.len_usable) = ACCESS_FBINFO(video.len);
	if (ACCESS_FBINFO(video.len_usable) > b->base->maxdisplayable)
		ACCESS_FBINFO(video.len_usable) = b->base->maxdisplayable;
#ifdef CONFIG_MTRR
	if (mtrr) {
		ACCESS_FBINFO(mtrr.vram) = mtrr_add(video_base_phys, ACCESS_FBINFO(video.len), MTRR_TYPE_WRCOMB, 1);
		ACCESS_FBINFO(mtrr.vram_valid) = 1;
		printk(KERN_INFO "matroxfb: MTRR's turned on\n");
	}
#endif	/* CONFIG_MTRR */

	if (!ACCESS_FBINFO(devflags.novga))
		request_region(0x3C0, 32, "matrox");
	ACCESS_FBINFO(hw_switch->reset(PMINFO hw));

	ACCESS_FBINFO(fbcon.monspecs.hfmin) = 0;
	ACCESS_FBINFO(fbcon.monspecs.hfmax) = fh;
	ACCESS_FBINFO(fbcon.monspecs.vfmin) = 0;
	ACCESS_FBINFO(fbcon.monspecs.vfmax) = fv;
	ACCESS_FBINFO(fbcon.monspecs.dpms) = 0;	/* TBD */

	/* static settings */
	if ((depth == RSText8) && (!*ACCESS_FBINFO(fbcon.fontname))) {
		strcpy(ACCESS_FBINFO(fbcon.fontname), "VGA8x8");
	}
	vesafb_defined.red = colors[depth-1].red;
	vesafb_defined.green = colors[depth-1].green;
	vesafb_defined.blue = colors[depth-1].blue;
	vesafb_defined.bits_per_pixel = colors[depth-1].bits_per_pixel;
	vesafb_defined.grayscale = grayscale;
	vesafb_defined.vmode = 0;
	if (noaccel)
		vesafb_defined.accel_flags &= ~FB_ACCELF_TEXT;

	strcpy(ACCESS_FBINFO(fbcon.modename), "MATROX VGA");
	ACCESS_FBINFO(fbcon.changevar) = NULL;
	ACCESS_FBINFO(fbcon.node) = -1;
	ACCESS_FBINFO(fbcon.fbops) = &matroxfb_ops;
	ACCESS_FBINFO(fbcon.disp) = d;
	ACCESS_FBINFO(fbcon.switch_con) = &matroxfb_switch;
	ACCESS_FBINFO(fbcon.updatevar) = &matroxfb_updatevar;
	ACCESS_FBINFO(fbcon.blank) = &matroxfb_blank;
	/* after __init time we are like module... no logo */
	ACCESS_FBINFO(fbcon.flags) = hotplug ? FBINFO_FLAG_MODULE : FBINFO_FLAG_DEFAULT;
	ACCESS_FBINFO(video.len_usable) &= PAGE_MASK;

#ifndef MODULE
	/* mode database is marked __init!!! */
	if (!hotplug) {
		fb_find_mode(&vesafb_defined, &ACCESS_FBINFO(fbcon), videomode[0]?videomode:NULL,
			NULL, 0, &defaultmode, vesafb_defined.bits_per_pixel);
	}
#endif /* !MODULE */

	/* mode modifiers */
	if (hslen)
		vesafb_defined.hsync_len = hslen;
	if (vslen)
		vesafb_defined.vsync_len = vslen;
	if (left != ~0)
		vesafb_defined.left_margin = left;
	if (right != ~0)
		vesafb_defined.right_margin = right;
	if (upper != ~0)
		vesafb_defined.upper_margin = upper;
	if (lower != ~0)
		vesafb_defined.lower_margin = lower;
	if (xres)
		vesafb_defined.xres = xres;
	if (yres)
		vesafb_defined.yres = yres;
	if (sync != -1)
		vesafb_defined.sync = sync;
	else if (vesafb_defined.sync == ~0) {
		vesafb_defined.sync = 0;
		if (yres < 400)
			vesafb_defined.sync |= FB_SYNC_HOR_HIGH_ACT;
		else if (yres < 480)
			vesafb_defined.sync |= FB_SYNC_VERT_HIGH_ACT;
	}

	/* fv, fh, maxclk limits was specified */
	{
		unsigned int tmp;

		if (fv) {
			tmp = fv * (vesafb_defined.upper_margin + vesafb_defined.yres
			          + vesafb_defined.lower_margin + vesafb_defined.vsync_len);
			if ((tmp < fh) || (fh == 0)) fh = tmp;
		}
		if (fh) {
			tmp = fh * (vesafb_defined.left_margin + vesafb_defined.xres
			          + vesafb_defined.right_margin + vesafb_defined.hsync_len);
			if ((tmp < maxclk) || (maxclk == 0)) maxclk = tmp;
		}
		maxclk = (maxclk + 499) / 500;
		if (maxclk) {
			tmp = (2000000000 + maxclk) / maxclk;
			if (tmp > pixclock) pixclock = tmp;
		}
	}
	if (pixclock) {
		if (pixclock < 2000)		/* > 500MHz */
			pixclock = 4000;	/* 250MHz */
		if (pixclock > 1000000)
			pixclock = 1000000;	/* 1MHz */
		vesafb_defined.pixclock = pixclock;
	}

	/* FIXME: Where to move this?! */
#if defined(CONFIG_PPC)
#if defined(CONFIG_FB_COMPAT_XPMAC)
	strcpy(ACCESS_FBINFO(matrox_name), "MTRX,");	/* OpenFirmware naming convension */
	strncat(ACCESS_FBINFO(matrox_name), b->name, 26);
	if (!console_fb_info)
		console_fb_info = &ACCESS_FBINFO(fbcon);
#endif
	if ((xres <= 640) && (yres <= 480)) {
		struct fb_var_screeninfo var;
		if (default_vmode == VMODE_NVRAM) {
			default_vmode = nvram_read_byte(NV_VMODE);
			if (default_vmode <= 0 || default_vmode > VMODE_MAX)
				default_vmode = VMODE_CHOOSE;
		}
		if (default_vmode <= 0 || default_vmode > VMODE_MAX)
			default_vmode = VMODE_640_480_60;
		if (default_cmode == CMODE_NVRAM)
			default_cmode = nvram_read_byte(NV_CMODE);
		if (default_cmode < CMODE_8 || default_cmode > CMODE_32)
			default_cmode = CMODE_8;
		if (!mac_vmode_to_var(default_vmode, default_cmode, &var)) {
			var.accel_flags = vesafb_defined.accel_flags;
			var.xoffset = var.yoffset = 0;
			vesafb_defined = var; /* Note: mac_vmode_to_var() doesnot set all parameters */
		}
	}
#endif /* CONFIG_PPC */
	vesafb_defined.xres_virtual = vesafb_defined.xres;
	if (nopan) {
		vesafb_defined.yres_virtual = vesafb_defined.yres;
	} else {
		vesafb_defined.yres_virtual = 65536; /* large enough to be INF, but small enough
							to yres_virtual * xres_virtual < 2^32 */
	}
	err = -EINVAL;
	if (matroxfb_set_var(&vesafb_defined, -2, &ACCESS_FBINFO(fbcon))) {
		printk(KERN_ERR "matroxfb: cannot set required parameters\n");
		goto failVideoIO;
	}

	printk(KERN_INFO "matroxfb: %dx%dx%dbpp (virtual: %dx%d)\n",
		vesafb_defined.xres, vesafb_defined.yres, vesafb_defined.bits_per_pixel,
		vesafb_defined.xres_virtual, vesafb_defined.yres_virtual);
	printk(KERN_INFO "matroxfb: framebuffer at 0x%lX, mapped to 0x%p, size %d\n",
		ACCESS_FBINFO(video.base), vaddr_va(ACCESS_FBINFO(video.vbase)), ACCESS_FBINFO(video.len));

/* We do not have to set currcon to 0... register_framebuffer do it for us on first console
 * and we do not want currcon == 0 for subsequent framebuffers */

	if (register_framebuffer(&ACCESS_FBINFO(fbcon)) < 0) {
		goto failVideoIO;
	}
	printk("fb%d: %s frame buffer device\n",
	       GET_FB_IDX(ACCESS_FBINFO(fbcon.node)), ACCESS_FBINFO(fbcon.modename));
	if (ACCESS_FBINFO(currcon) < 0) {
		/* there is no console on this fb... but we have to initialize hardware
		 * until someone tells me what is proper thing to do */
		printk(KERN_INFO "fb%d: initializing hardware\n",
			GET_FB_IDX(ACCESS_FBINFO(fbcon.node)));
		matroxfb_set_var(&vesafb_defined, -1, &ACCESS_FBINFO(fbcon));
	}
	return 0;
failVideoIO:;
	mga_iounmap(ACCESS_FBINFO(video.vbase));
failCtrlIO:;
	mga_iounmap(ACCESS_FBINFO(mmio.vbase));
failVideoMR:;
	release_mem_region(video_base_phys, ACCESS_FBINFO(video.len_maximum));
failCtrlMR:;
	release_mem_region(ctrlptr_phys, 16384);
fail:;
	return err;
}

LIST_HEAD(matroxfb_list);
LIST_HEAD(matroxfb_driver_list);

#define matroxfb_l(x) list_entry(x, struct matrox_fb_info, next_fb)
#define matroxfb_driver_l(x) list_entry(x, struct matroxfb_driver, node)
int matroxfb_register_driver(struct matroxfb_driver* drv) {
	struct matrox_fb_info* minfo;

	list_add(&drv->node, &matroxfb_driver_list);
	for (minfo = matroxfb_l(matroxfb_list.next);
	     minfo != matroxfb_l(&matroxfb_list);
	     minfo = matroxfb_l(minfo->next_fb.next)) {
		void* p;

		if (minfo->drivers_count == MATROXFB_MAX_FB_DRIVERS)
			continue;
		p = drv->probe(minfo);
		if (p) {
			minfo->drivers_data[minfo->drivers_count] = p;
			minfo->drivers[minfo->drivers_count++] = drv;
		}
	}
	return 0;
}

void matroxfb_unregister_driver(struct matroxfb_driver* drv) {
	struct matrox_fb_info* minfo;

	list_del(&drv->node);
	for (minfo = matroxfb_l(matroxfb_list.next);
	     minfo != matroxfb_l(&matroxfb_list);
	     minfo = matroxfb_l(minfo->next_fb.next)) {
		int i;

		for (i = 0; i < minfo->drivers_count; ) {
			if (minfo->drivers[i] == drv) {
				if (drv && drv->remove)
					drv->remove(minfo, minfo->drivers_data[i]);
				minfo->drivers[i] = minfo->drivers[--minfo->drivers_count];
				minfo->drivers_data[i] = minfo->drivers_data[minfo->drivers_count];
			} else
				i++;
		}
	}
}

static void matroxfb_register_device(struct matrox_fb_info* minfo) {
	struct matroxfb_driver* drv;
	int i = 0;
	list_add(&ACCESS_FBINFO(next_fb), &matroxfb_list);
	for (drv = matroxfb_driver_l(matroxfb_driver_list.next);
	     drv != matroxfb_driver_l(&matroxfb_driver_list);
	     drv = matroxfb_driver_l(drv->node.next)) {
		if (drv && drv->probe) {
			void *p = drv->probe(minfo);
			if (p) {
				minfo->drivers_data[i] = p;
				minfo->drivers[i++] = drv;
				if (i == MATROXFB_MAX_FB_DRIVERS)
					break;
			}
		}
	}
	minfo->drivers_count = i;
}

static void matroxfb_unregister_device(struct matrox_fb_info* minfo) {
	int i;

	list_del(&ACCESS_FBINFO(next_fb));
	for (i = 0; i < minfo->drivers_count; i++) {
		struct matroxfb_driver* drv = minfo->drivers[i];

		if (drv && drv->remove)
			drv->remove(minfo, minfo->drivers_data[i]);
	}
}

static int matroxfb_probe(struct pci_dev* pdev, const struct pci_device_id* dummy) {
	struct board* b;
	u_int8_t rev;
	u_int16_t svid;
	u_int16_t sid;
	struct matrox_fb_info* minfo;
	struct display* d;
	int err;
	u_int32_t cmd;
#ifndef CONFIG_FB_MATROX_MULTIHEAD
	static int registered = 0;
#endif
	DBG("matroxfb_probe")

	pci_read_config_byte(pdev, PCI_REVISION_ID, &rev);
	svid = pdev->subsystem_vendor;
	sid = pdev->subsystem_device;
	for (b = dev_list; b->vendor; b++) {
		if ((b->vendor != pdev->vendor) || (b->device != pdev->device) || (b->rev < rev)) continue;
		if (b->svid)
			if ((b->svid != svid) || (b->sid != sid)) continue;
		break;
	}
	/* not match... */
	if (!b->vendor)
		return -1;
	if (dev > 0) {
		/* not requested one... */
		dev--;
		return -1;
	}
	pci_read_config_dword(pdev, PCI_COMMAND, &cmd);
	if (pci_enable_device(pdev)) {
		return -1;
	}

#ifdef CONFIG_FB_MATROX_MULTIHEAD
	minfo = (struct matrox_fb_info*)kmalloc(sizeof(*minfo), GFP_KERNEL);
	if (!minfo)
		return -1;
	d = (struct display*)kmalloc(sizeof(*d), GFP_KERNEL);
	if (!d) {
		kfree(minfo);
		return -1;
	}
#else
	if (registered)	/* singlehead driver... */
		return -1;
	minfo = &matroxfb_global_mxinfo;
	d = &global_disp;
#endif
	memset(MINFO, 0, sizeof(*MINFO));
	memset(d, 0, sizeof(*d));

	ACCESS_FBINFO(currenthw) = &ACCESS_FBINFO(hw1);
	ACCESS_FBINFO(newhw) = &ACCESS_FBINFO(hw2);
	ACCESS_FBINFO(pcidev) = pdev;
	ACCESS_FBINFO(dead) = 0;
	ACCESS_FBINFO(usecount) = 0;
	pdev->driver_data = MINFO;
	/* CMDLINE */
	memcpy(ACCESS_FBINFO(fbcon.fontname), fontname, sizeof(ACCESS_FBINFO(fbcon.fontname)));
	/* DEVFLAGS */
	ACCESS_FBINFO(devflags.inverse) = inverse;
	ACCESS_FBINFO(devflags.memtype) = memtype;
	if (memtype != -1)
		noinit = 0;
	if (cmd & PCI_COMMAND_MEMORY) {
		ACCESS_FBINFO(devflags.novga) = novga;
		ACCESS_FBINFO(devflags.nobios) = nobios;
		ACCESS_FBINFO(devflags.noinit) = noinit;
		/* subsequent heads always needs initialization and must not enable BIOS */
		novga = 1;
		nobios = 1;
		noinit = 0;
	} else {
		ACCESS_FBINFO(devflags.novga) = 1;
		ACCESS_FBINFO(devflags.nobios) = 1;
		ACCESS_FBINFO(devflags.noinit) = 0;
	}

	ACCESS_FBINFO(devflags.nopciretry) = no_pci_retry;
	ACCESS_FBINFO(devflags.mga_24bpp_fix) = inv24;
	ACCESS_FBINFO(devflags.precise_width) = option_precise_width;
	ACCESS_FBINFO(devflags.hwcursor) = hwcursor;
	ACCESS_FBINFO(devflags.blink) = blink;
	ACCESS_FBINFO(devflags.sgram) = sgram;
	ACCESS_FBINFO(capable.cross4MB) = cross4MB;

	ACCESS_FBINFO(fastfont.size) = fastfont;

	ACCESS_FBINFO(cursor.state) = CM_ERASE;
	init_timer (&ACCESS_FBINFO(cursor.timer));
	ACCESS_FBINFO(cursor.timer.data) = (unsigned long)MINFO;
	spin_lock_init(&ACCESS_FBINFO(lock.DAC));
	spin_lock_init(&ACCESS_FBINFO(lock.accel));
	init_rwsem(&ACCESS_FBINFO(crtc2.lock));
	init_rwsem(&ACCESS_FBINFO(altout.lock));

	ACCESS_FBINFO(output.all) = MATROXFB_OUTPUT_CONN_PRIMARY;
	ACCESS_FBINFO(output.ph) = MATROXFB_OUTPUT_CONN_PRIMARY;
	ACCESS_FBINFO(output.sh) = 0;

	err = initMatrox2(PMINFO d, b);
	if (!err) {
#ifndef CONFIG_FB_MATROX_MULTIHEAD
		registered = 1;
#endif
		matroxfb_register_device(MINFO);
		return 0;
	}
#ifdef CONFIG_FB_MATROX_MULTIHEAD
	kfree(d);
	kfree(minfo);
#endif
	return -1;
}

static void pci_remove_matrox(struct pci_dev* pdev) {
	struct matrox_fb_info* minfo;

	minfo = pdev->driver_data;
	matroxfb_remove(PMINFO 1);
}

static struct pci_device_id matroxfb_devices[] __devinitdata = {
#ifdef CONFIG_FB_MATROX_MILLENIUM
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_MIL,
		PCI_ANY_ID,	PCI_ANY_ID,	0, 0, 0},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_MIL_2,
		PCI_ANY_ID,	PCI_ANY_ID,	0, 0, 0},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_MIL_2_AGP,
		PCI_ANY_ID,	PCI_ANY_ID,	0, 0, 0},
#endif
#ifdef CONFIG_FB_MATROX_MYSTIQUE
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_MYS,
		PCI_ANY_ID,	PCI_ANY_ID,	0, 0, 0},
#endif
#ifdef CONFIG_FB_MATROX_G100
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G100,
		PCI_ANY_ID,	PCI_ANY_ID,	0, 0, 0},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G100_AGP,
		PCI_ANY_ID,	PCI_ANY_ID,	0, 0, 0},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G200_PCI,
		PCI_ANY_ID,	PCI_ANY_ID,	0, 0, 0},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G200_AGP,
		PCI_ANY_ID,	PCI_ANY_ID,	0, 0, 0},
	{PCI_VENDOR_ID_MATROX,	PCI_DEVICE_ID_MATROX_G400_AGP,
		PCI_ANY_ID,	PCI_ANY_ID,	0, 0, 0},
#endif
	{0,			0,
		0,		0,		0, 0, 0}
};

MODULE_DEVICE_TABLE(pci, matroxfb_devices);

static struct pci_driver matroxfb_driver = {
	name:		"matroxfb",
	id_table:	matroxfb_devices,
	probe:		matroxfb_probe,
	remove:		pci_remove_matrox,
};

/* **************************** init-time only **************************** */

#define RSResolution(X)	((X) & 0x0F)
#define RS640x400	1
#define RS640x480	2
#define RS800x600	3
#define RS1024x768	4
#define RS1280x1024	5
#define RS1600x1200	6
#define RS768x576	7
#define RS960x720	8
#define RS1152x864	9
#define RS1408x1056	10
#define RS640x350	11
#define RS1056x344	12	/* 132 x 43 text */
#define RS1056x400	13	/* 132 x 50 text */
#define RS1056x480	14	/* 132 x 60 text */
#define RSNoxNo		15
/* 10-FF */
static struct { int xres, yres, left, right, upper, lower, hslen, vslen, vfreq; } timmings[] __initdata = {
	{  640,  400,  48, 16, 39,  8,  96, 2, 70 },
	{  640,  480,  48, 16, 33, 10,  96, 2, 60 },
	{  800,  600, 144, 24, 28,  8, 112, 6, 60 },
	{ 1024,  768, 160, 32, 30,  4, 128, 4, 60 },
	{ 1280, 1024, 224, 32, 32,  4, 136, 4, 60 },
	{ 1600, 1200, 272, 48, 32,  5, 152, 5, 60 },
	{  768,  576, 144, 16, 28,  6, 112, 4, 60 },
	{  960,  720, 144, 24, 28,  8, 112, 4, 60 },
	{ 1152,  864, 192, 32, 30,  4, 128, 4, 60 },
	{ 1408, 1056, 256, 40, 32,  5, 144, 5, 60 },
	{  640,  350,  48, 16, 39,  8,  96, 2, 70 },
	{ 1056,  344,  96, 24, 59, 44, 160, 2, 70 },
	{ 1056,  400,  96, 24, 39,  8, 160, 2, 70 },
	{ 1056,  480,  96, 24, 36, 12, 160, 3, 60 },
	{    0,    0,  ~0, ~0, ~0, ~0,   0, 0,  0 }
};

#define RSCreate(X,Y)	((X) | ((Y) << 8))
static struct { unsigned int vesa; unsigned int info; } *RSptr, vesamap[] __initdata = {
/* default must be first */
#ifdef FBCON_HAS_CFB8
	{    ~0, RSCreate(RSNoxNo,     RS8bpp ) },
	{ 0x101, RSCreate(RS640x480,   RS8bpp ) },
	{ 0x100, RSCreate(RS640x400,   RS8bpp ) },
	{ 0x180, RSCreate(RS768x576,   RS8bpp ) },
	{ 0x103, RSCreate(RS800x600,   RS8bpp ) },
	{ 0x188, RSCreate(RS960x720,   RS8bpp ) },
	{ 0x105, RSCreate(RS1024x768,  RS8bpp ) },
	{ 0x190, RSCreate(RS1152x864,  RS8bpp ) },
	{ 0x107, RSCreate(RS1280x1024, RS8bpp ) },
	{ 0x198, RSCreate(RS1408x1056, RS8bpp ) },
	{ 0x11C, RSCreate(RS1600x1200, RS8bpp ) },
#endif
#ifdef FBCON_HAS_CFB16
	{    ~0, RSCreate(RSNoxNo,     RS15bpp) },
	{ 0x110, RSCreate(RS640x480,   RS15bpp) },
	{ 0x181, RSCreate(RS768x576,   RS15bpp) },
	{ 0x113, RSCreate(RS800x600,   RS15bpp) },
	{ 0x189, RSCreate(RS960x720,   RS15bpp) },
	{ 0x116, RSCreate(RS1024x768,  RS15bpp) },
	{ 0x191, RSCreate(RS1152x864,  RS15bpp) },
	{ 0x119, RSCreate(RS1280x1024, RS15bpp) },
	{ 0x199, RSCreate(RS1408x1056, RS15bpp) },
	{ 0x11D, RSCreate(RS1600x1200, RS15bpp) },
	{ 0x111, RSCreate(RS640x480,   RS16bpp) },
	{ 0x182, RSCreate(RS768x576,   RS16bpp) },
	{ 0x114, RSCreate(RS800x600,   RS16bpp) },
	{ 0x18A, RSCreate(RS960x720,   RS16bpp) },
	{ 0x117, RSCreate(RS1024x768,  RS16bpp) },
	{ 0x192, RSCreate(RS1152x864,  RS16bpp) },
	{ 0x11A, RSCreate(RS1280x1024, RS16bpp) },
	{ 0x19A, RSCreate(RS1408x1056, RS16bpp) },
	{ 0x11E, RSCreate(RS1600x1200, RS16bpp) },
#endif
#ifdef FBCON_HAS_CFB24
	{    ~0, RSCreate(RSNoxNo,     RS24bpp) },
	{ 0x1B2, RSCreate(RS640x480,   RS24bpp) },
	{ 0x184, RSCreate(RS768x576,   RS24bpp) },
	{ 0x1B5, RSCreate(RS800x600,   RS24bpp) },
	{ 0x18C, RSCreate(RS960x720,   RS24bpp) },
	{ 0x1B8, RSCreate(RS1024x768,  RS24bpp) },
	{ 0x194, RSCreate(RS1152x864,  RS24bpp) },
	{ 0x1BB, RSCreate(RS1280x1024, RS24bpp) },
	{ 0x19C, RSCreate(RS1408x1056, RS24bpp) },
	{ 0x1BF, RSCreate(RS1600x1200, RS24bpp) },
#endif
#ifdef FBCON_HAS_CFB32
	{    ~0, RSCreate(RSNoxNo,     RS32bpp) },
	{ 0x112, RSCreate(RS640x480,   RS32bpp) },
	{ 0x183, RSCreate(RS768x576,   RS32bpp) },
	{ 0x115, RSCreate(RS800x600,   RS32bpp) },
	{ 0x18B, RSCreate(RS960x720,   RS32bpp) },
	{ 0x118, RSCreate(RS1024x768,  RS32bpp) },
	{ 0x193, RSCreate(RS1152x864,  RS32bpp) },
	{ 0x11B, RSCreate(RS1280x1024, RS32bpp) },
	{ 0x19B, RSCreate(RS1408x1056, RS32bpp) },
	{ 0x11F, RSCreate(RS1600x1200, RS32bpp) },
#endif
#ifdef FBCON_HAS_VGATEXT
	{    ~0, RSCreate(RSNoxNo,     RSText ) },
	{ 0x002, RSCreate(RS640x400,   RSText ) },	/* 80x25 */
	{ 0x003, RSCreate(RS640x400,   RSText ) },	/* 80x25 */
	{ 0x007, RSCreate(RS640x400,   RSText ) },	/* 80x25 */
	{ 0x1C0, RSCreate(RS640x400,   RSText8) },	/* 80x50 */
	{ 0x108, RSCreate(RS640x480,   RSText8) },	/* 80x60 */
	{ 0x109, RSCreate(RS1056x400,  RSText ) },	/* 132x25 */
	{ 0x10A, RSCreate(RS1056x344,  RSText8) },	/* 132x43 */
	{ 0x10B, RSCreate(RS1056x400,  RSText8) },	/* 132x50 */
	{ 0x10C, RSCreate(RS1056x480,  RSText8) },	/* 132x60 */
#endif
#ifdef FBCON_HAS_CFB4
	{    ~0, RSCreate(RSNoxNo,     RS4bpp ) },
	{ 0x010, RSCreate(RS640x350,   RS4bpp ) },
	{ 0x012, RSCreate(RS640x480,   RS4bpp ) },
	{ 0x102, RSCreate(RS800x600,   RS4bpp ) },
	{ 0x104, RSCreate(RS1024x768,  RS4bpp ) },
	{ 0x106, RSCreate(RS1280x1024, RS4bpp ) },
#endif
	{     0, 0				}};

static void __init matroxfb_init_params(void) {
	/* fh from kHz to Hz */
	if (fh < 1000)
		fh *= 1000;	/* 1kHz minimum */
	/* maxclk */
	if (maxclk < 1000) maxclk *= 1000;	/* kHz -> Hz, MHz -> kHz */
	if (maxclk < 1000000) maxclk *= 1000;	/* kHz -> Hz, 1MHz minimum */
	/* fix VESA number */
	if (vesa != ~0)
		vesa &= 0x1DFF;		/* mask out clearscreen, acceleration and so on */

	/* static settings */
	for (RSptr = vesamap; RSptr->vesa; RSptr++) {
		if (RSptr->vesa == vesa) break;
	}
	if (!RSptr->vesa) {
		printk(KERN_ERR "Invalid vesa mode 0x%04X\n", vesa);
		RSptr = vesamap;
	}
	{
		int res = RSResolution(RSptr->info)-1;
		if (left == ~0)
			left = timmings[res].left;
		if (!xres)
			xres = timmings[res].xres;
		if (right == ~0)
			right = timmings[res].right;
		if (!hslen)
			hslen = timmings[res].hslen;
		if (upper == ~0)
			upper = timmings[res].upper;
		if (!yres)
			yres = timmings[res].yres;
		if (lower == ~0)
			lower = timmings[res].lower;
		if (!vslen)
			vslen = timmings[res].vslen;
		if (!(fv||fh||maxclk||pixclock))
			fv = timmings[res].vfreq;
		if (depth == -1)
			depth = RSDepth(RSptr->info);
	}
}

static void __init matrox_init(void) {
	matroxfb_init_params();
	pci_register_driver(&matroxfb_driver);
	dev = -1;	/* accept all new devices... */
}

/* **************************** exit-time only **************************** */

static void __exit matrox_done(void) {
	pci_unregister_driver(&matroxfb_driver);
}

#ifndef MODULE

/* ************************* init in-kernel code ************************** */

int __init matroxfb_setup(char *options) {
	char *this_opt;

	DBG("matroxfb_setup")

	fontname[0] = '\0';

	if (!options || !*options)
		return 0;

	for(this_opt=strtok(options,","); this_opt; this_opt=strtok(NULL,",")) {
		if (!*this_opt) continue;

		dprintk("matroxfb_setup: option %s\n", this_opt);

		if (!strncmp(this_opt, "dev:", 4))
			dev = simple_strtoul(this_opt+4, NULL, 0);
		else if (!strncmp(this_opt, "depth:", 6)) {
			switch (simple_strtoul(this_opt+6, NULL, 0)) {
				case 0: depth = RSText; break;
				case 4: depth = RS4bpp; break;
				case 8: depth = RS8bpp; break;
				case 15:depth = RS15bpp; break;
				case 16:depth = RS16bpp; break;
				case 24:depth = RS24bpp; break;
				case 32:depth = RS32bpp; break;
				default:
					printk(KERN_ERR "matroxfb: unsupported color depth\n");
			}
		} else if (!strncmp(this_opt, "xres:", 5))
			xres = simple_strtoul(this_opt+5, NULL, 0);
		else if (!strncmp(this_opt, "yres:", 5))
			yres = simple_strtoul(this_opt+5, NULL, 0);
		else if (!strncmp(this_opt, "vslen:", 6))
			vslen = simple_strtoul(this_opt+6, NULL, 0);
		else if (!strncmp(this_opt, "hslen:", 6))
			hslen = simple_strtoul(this_opt+6, NULL, 0);
		else if (!strncmp(this_opt, "left:", 5))
			left = simple_strtoul(this_opt+5, NULL, 0);
		else if (!strncmp(this_opt, "right:", 6))
			right = simple_strtoul(this_opt+6, NULL, 0);
		else if (!strncmp(this_opt, "upper:", 6))
			upper = simple_strtoul(this_opt+6, NULL, 0);
		else if (!strncmp(this_opt, "lower:", 6))
			lower = simple_strtoul(this_opt+6, NULL, 0);
		else if (!strncmp(this_opt, "pixclock:", 9))
			pixclock = simple_strtoul(this_opt+9, NULL, 0);
		else if (!strncmp(this_opt, "sync:", 5))
			sync = simple_strtoul(this_opt+5, NULL, 0);
		else if (!strncmp(this_opt, "vesa:", 5))
			vesa = simple_strtoul(this_opt+5, NULL, 0);
		else if (!strncmp(this_opt, "font:", 5))
			strncpy(fontname, this_opt+5, sizeof(fontname)-1);
		else if (!strncmp(this_opt, "maxclk:", 7))
			maxclk = simple_strtoul(this_opt+7, NULL, 0);
		else if (!strncmp(this_opt, "fh:", 3))
			fh = simple_strtoul(this_opt+3, NULL, 0);
		else if (!strncmp(this_opt, "fv:", 3))
			fv = simple_strtoul(this_opt+3, NULL, 0);
		else if (!strncmp(this_opt, "mem:", 4))
			mem = simple_strtoul(this_opt+4, NULL, 0);
		else if (!strncmp(this_opt, "mode:", 5))
			strncpy(videomode, this_opt+5, sizeof(videomode)-1);
#ifdef CONFIG_PPC
		else if (!strncmp(this_opt, "vmode:", 6)) {
			unsigned int vmode = simple_strtoul(this_opt+6, NULL, 0);
			if (vmode > 0 && vmode <= VMODE_MAX)
				default_vmode = vmode;
		} else if (!strncmp(this_opt, "cmode:", 6)) {
			unsigned int cmode = simple_strtoul(this_opt+6, NULL, 0);
			switch (cmode) {
				case 0:
				case 8:
					default_cmode = CMODE_8;
					break;
				case 15:
				case 16:
					default_cmode = CMODE_16;
					break;
				case 24:
				case 32:
					default_cmode = CMODE_32;
					break;
			}
		}
#endif
		else if (!strncmp(this_opt, "fastfont:", 9))
			fastfont = simple_strtoul(this_opt+9, NULL, 0);
		else if (!strcmp(this_opt, "nofastfont"))	/* fastfont:N and nofastfont (nofastfont = fastfont:0) */
			fastfont = 0;
		else if (!strcmp(this_opt, "disabled"))	/* nodisabled does not exist */
			disabled = 1;
		else if (!strcmp(this_opt, "enabled"))	/* noenabled does not exist */
			disabled = 0;
		else if (!strcmp(this_opt, "sgram"))	/* nosgram == sdram */
			sgram = 1;
		else if (!strcmp(this_opt, "sdram"))
			sgram = 0;
		else if (!strncmp(this_opt, "memtype:", 8))
			memtype = simple_strtoul(this_opt+8, NULL, 0);
		else {
			int value = 1;

			if (!strncmp(this_opt, "no", 2)) {
				value = 0;
				this_opt += 2;
			}
			if (! strcmp(this_opt, "inverse"))
				inverse = value;
			else if (!strcmp(this_opt, "accel"))
				noaccel = !value;
			else if (!strcmp(this_opt, "pan"))
				nopan = !value;
			else if (!strcmp(this_opt, "pciretry"))
				no_pci_retry = !value;
			else if (!strcmp(this_opt, "vga"))
				novga = !value;
			else if (!strcmp(this_opt, "bios"))
				nobios = !value;
			else if (!strcmp(this_opt, "init"))
				noinit = !value;
#ifdef CONFIG_MTRR
			else if (!strcmp(this_opt, "mtrr"))
				mtrr = value;
#endif
			else if (!strcmp(this_opt, "inv24"))
				inv24 = value;
			else if (!strcmp(this_opt, "cross4MB"))
				cross4MB = value;
			else if (!strcmp(this_opt, "hwcursor"))
				hwcursor = value;
			else if (!strcmp(this_opt, "blink"))
				blink = value;
			else if (!strcmp(this_opt, "grayscale"))
				grayscale = value;
			else if (!strcmp(this_opt, "dfp"))
				dfp = value;
			else {
				strncpy(videomode, this_opt, sizeof(videomode)-1);
			}
		}
	}
	return 0;
}

static int __init initialized = 0;

int __init matroxfb_init(void)
{
	DBG("matroxfb_init")

	if (disabled)
		return -ENXIO;
	if (!initialized) {
		initialized = 1;
		matrox_init();
	}
	/* never return failure, user can hotplug matrox later... */
	return 0;
}

#else

/* *************************** init module code **************************** */

MODULE_AUTHOR("(c) 1998,1999 Petr Vandrovec <vandrove@vc.cvut.cz>");
MODULE_DESCRIPTION("Accelerated FBDev driver for Matrox Millennium/Mystique/G100/G200/G400");
MODULE_PARM(mem, "i");
MODULE_PARM_DESC(mem, "Size of available memory in MB, KB or B (2,4,8,12,16MB, default=autodetect)");
MODULE_PARM(disabled, "i");
MODULE_PARM_DESC(disabled, "Disabled (0 or 1=disabled) (default=0)");
MODULE_PARM(noaccel, "i");
MODULE_PARM_DESC(noaccel, "Do not use accelerating engine (0 or 1=disabled) (default=0)");
MODULE_PARM(nopan, "i");
MODULE_PARM_DESC(nopan, "Disable pan on startup (0 or 1=disabled) (default=0)");
MODULE_PARM(no_pci_retry, "i");
MODULE_PARM_DESC(no_pci_retry, "PCI retries enabled (0 or 1=disabled) (default=0)");
MODULE_PARM(novga, "i");
MODULE_PARM_DESC(novga, "VGA I/O (0x3C0-0x3DF) disabled (0 or 1=disabled) (default=0)");
MODULE_PARM(nobios, "i");
MODULE_PARM_DESC(nobios, "Disables ROM BIOS (0 or 1=disabled) (default=do not change BIOS state)");
MODULE_PARM(noinit, "i");
MODULE_PARM_DESC(noinit, "Disables W/SG/SD-RAM and bus interface initialization (0 or 1=do not initialize) (default=0)");
MODULE_PARM(memtype, "i");
MODULE_PARM_DESC(memtype, "Memory type for G200/G400 (see Documentation/fb/matroxfb.txt for explanation) (default=3 for G200, 0 for G400)");
MODULE_PARM(mtrr, "i");
MODULE_PARM_DESC(mtrr, "This speeds up video memory accesses (0=disabled or 1) (default=1)");
MODULE_PARM(sgram, "i");
MODULE_PARM_DESC(sgram, "Indicates that G200/G400 has SGRAM memory (0=SDRAM, 1=SGRAM) (default=0)");
MODULE_PARM(inv24, "i");
MODULE_PARM_DESC(inv24, "Inverts clock polarity for 24bpp and loop frequency > 100MHz (default=do not invert polarity)");
MODULE_PARM(inverse, "i");
MODULE_PARM_DESC(inverse, "Inverse (0 or 1) (default=0)");
#ifdef CONFIG_FB_MATROX_MULTIHEAD
MODULE_PARM(dev, "i");
MODULE_PARM_DESC(dev, "Multihead support, attach to device ID (0..N) (default=all working)");
#else
MODULE_PARM(dev, "i");
MODULE_PARM_DESC(dev, "Multihead support, attach to device ID (0..N) (default=first working)");
#endif
MODULE_PARM(vesa, "i");
MODULE_PARM_DESC(vesa, "Startup videomode (0x000-0x1FF) (default=0x101)");
MODULE_PARM(xres, "i");
MODULE_PARM_DESC(xres, "Horizontal resolution (px), overrides xres from vesa (default=vesa)");
MODULE_PARM(yres, "i");
MODULE_PARM_DESC(yres, "Vertical resolution (scans), overrides yres from vesa (default=vesa)");
MODULE_PARM(upper, "i");
MODULE_PARM_DESC(upper, "Upper blank space (scans), overrides upper from vesa (default=vesa)");
MODULE_PARM(lower, "i");
MODULE_PARM_DESC(lower, "Lower blank space (scans), overrides lower from vesa (default=vesa)");
MODULE_PARM(vslen, "i");
MODULE_PARM_DESC(vslen, "Vertical sync length (scans), overrides lower from vesa (default=vesa)");
MODULE_PARM(left, "i");
MODULE_PARM_DESC(left, "Left blank space (px), overrides left from vesa (default=vesa)");
MODULE_PARM(right, "i");
MODULE_PARM_DESC(right, "Right blank space (px), overrides right from vesa (default=vesa)");
MODULE_PARM(hslen, "i");
MODULE_PARM_DESC(hslen, "Horizontal sync length (px), overrides hslen from vesa (default=vesa)");
MODULE_PARM(pixclock, "i");
MODULE_PARM_DESC(pixclock, "Pixelclock (ns), overrides pixclock from vesa (default=vesa)");
MODULE_PARM(sync, "i");
MODULE_PARM_DESC(sync, "Sync polarity, overrides sync from vesa (default=vesa)");
MODULE_PARM(depth, "i");
MODULE_PARM_DESC(depth, "Color depth (0=text,8,15,16,24,32) (default=vesa)");
MODULE_PARM(maxclk, "i");
MODULE_PARM_DESC(maxclk, "Startup maximal clock, 0-999MHz, 1000-999999kHz, 1000000-INF Hz");
MODULE_PARM(fh, "i");
MODULE_PARM_DESC(fh, "Startup horizontal frequency, 0-999kHz, 1000-INF Hz");
MODULE_PARM(fv, "i");
MODULE_PARM_DESC(fv, "Startup vertical frequency, 0-INF Hz\n"
"You should specify \"fv:max_monitor_vsync,fh:max_monitor_hsync,maxclk:max_monitor_dotclock\"\n");
MODULE_PARM(hwcursor, "i");
MODULE_PARM_DESC(hwcursor, "Enables hardware cursor (0 or 1) (default=0)");
MODULE_PARM(blink, "i");
MODULE_PARM_DESC(blink, "Enables hardware cursor blinking (0 or 1) (default=1)");
MODULE_PARM(fastfont, "i");
MODULE_PARM_DESC(fastfont, "Specifies, how much memory should be used for font data (0, 1024-65536 are reasonable) (default=0)");
MODULE_PARM(grayscale, "i");
MODULE_PARM_DESC(grayscale, "Sets display into grayscale. Works perfectly with paletized videomode (4, 8bpp), some limitations apply to 16, 24 and 32bpp videomodes (default=nograyscale)");
MODULE_PARM(cross4MB, "i");
MODULE_PARM_DESC(cross4MB, "Specifies that 4MB boundary can be in middle of line. (default=autodetected)");
MODULE_PARM(dfp, "i");
MODULE_PARM_DESC(dfp, "Specifies whether to use digital flat panel interface of G200/G400 (0 or 1) (default=0)");
#ifdef CONFIG_PPC
MODULE_PARM(vmode, "i");
MODULE_PARM_DESC(vmode, "Specify the vmode mode number that should be used (640x480 default)");
MODULE_PARM(cmode, "i");
MODULE_PARM_DESC(cmode, "Specify the video depth that should be used (8bit default)");
#endif

int __init init_module(void){

	DBG("init_module")

	if (disabled)
		return -ENXIO;

	if (depth == 0)
		depth = RSText;
	else if (depth == 4)
		depth = RS4bpp;
	else if (depth == 8)
		depth = RS8bpp;
	else if (depth == 15)
		depth = RS15bpp;
	else if (depth == 16)
		depth = RS16bpp;
	else if (depth == 24)
		depth = RS24bpp;
	else if (depth == 32)
		depth = RS32bpp;
	else if (depth != -1) {
		printk(KERN_ERR "matroxfb: depth %d is not supported, using default\n", depth);
		depth = -1;
	}
	matrox_init();
	/* never return failure; user can hotplug matrox later... */
	return 0;
}
#endif	/* MODULE */

module_exit(matrox_done);
EXPORT_SYMBOL(matroxfb_register_driver);
EXPORT_SYMBOL(matroxfb_unregister_driver);

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
