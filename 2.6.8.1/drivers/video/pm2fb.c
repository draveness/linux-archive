/*
 * Permedia2 framebuffer driver.
 *
 * 2.5/2.6 driver:
 * Copyright (c) 2003 Jim Hague (jim.hague@acm.org)
 *
 * based on 2.4 driver:
 * Copyright (c) 1998-2000 Ilario Nardinocchi (nardinoc@CS.UniBO.IT)
 * Copyright (c) 1999 Jakub Jelinek (jakub@redhat.com)
 *
 * and additional input from James Simmon's port of Hannu Mallat's tdfx
 * driver.
 *
 * $Id$
 *
 * I have a Creative Graphics Blaster Exxtreme card - pm2fb on x86.
 * I have no access to other pm2fb implementations, and cannot test
 * on them. Therefore for now I am omitting Sparc and CVision code.
 *
 * Multiple boards support has been on the TODO list for ages.
 * Don't expect this to change.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of this archive for
 * more details.
 *
 * 
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

#include <video/permedia2.h>
#include <video/cvisionppc.h>

#if !defined(__LITTLE_ENDIAN) && !defined(__BIG_ENDIAN)
#error	"The endianness of the target host has not been defined."
#endif

#if defined(__BIG_ENDIAN) && !defined(__sparc__)
#define PM2FB_BE_APERTURE
#endif

#if !defined(CONFIG_PCI)
#error "Only generic PCI cards supported."
#endif

#undef PM2FB_MASTER_DEBUG
#ifdef PM2FB_MASTER_DEBUG
#define DPRINTK(a,b...)	printk(KERN_DEBUG "pm2fb: %s: " a, __FUNCTION__ , ## b)
#else
#define DPRINTK(a,b...)
#endif

/*
 * The 2.4 driver calls reset_card() at init time, where it also sets the
 * initial mode. I don't think the driver should touch the chip until
 * the console sets a video mode. So I was calling this at the start
 * of setting a mode. However, certainly on 1280x1024 depth 16 on my
 * PCI Graphics Blaster Exxtreme this causes the display to smear
 * slightly.  I don't know why. Guesses to jim.hague@acm.org.
 */
#undef RESET_CARD_ON_MODE_SET

/*
 * Driver data 
 */
static char *mode __initdata = NULL;

/*
 * The XFree GLINT driver will (I think to implement hardware cursor
 * support on TVP4010 and similar where there is no RAMDAC - see
 * comment in set_video) always request +ve sync regardless of what
 * the mode requires. This screws me because I have a Sun
 * fixed-frequency monitor which absolutely has to have -ve sync. So
 * these flags allow the user to specify that requests for +ve sync
 * should be silently turned in -ve sync.
 */
static int lowhsync __initdata = 0;
static int lowvsync __initdata = 0;	

/*
 * The hardware state of the graphics card that isn't part of the
 * screeninfo.
 */
struct pm2fb_par
{
	pm2type_t	type;		/* Board type */
	u32		fb_size;	/* framebuffer memory size */
	unsigned char*	v_fb;		/* virtual address of frame buffer */
	unsigned char*	v_regs;		/* virtual address of p_regs */
	u32 	   	memclock;	/* memclock */
	u32		video;		/* video flags before blanking */
};

/*
 * Here we define the default structs fb_fix_screeninfo and fb_var_screeninfo
 * if we don't use modedb.
 */
static struct fb_fix_screeninfo pm2fb_fix __initdata = {
	.id =		"", 
	.type =		FB_TYPE_PACKED_PIXELS,
	.visual =	FB_VISUAL_PSEUDOCOLOR,
	.xpanstep =	1,
	.ypanstep =	1,
	.ywrapstep =	0, 
	.accel =	FB_ACCEL_NONE,
};

/*
 * Default video mode. In case the modedb doesn't work, or we're
 * a module (in which case modedb doesn't really work).
 */
static struct fb_var_screeninfo pm2fb_var __initdata = {
	/* "640x480, 8 bpp @ 60 Hz */
	.xres =		640,
	.yres =		480,
	.xres_virtual =	640,
	.yres_virtual =	480,
	.bits_per_pixel =8,
	.red =		{0, 8, 0},
	.blue =		{0, 8, 0},
	.green =	{0, 8, 0},
	.activate =	FB_ACTIVATE_NOW,
	.height =	-1,
	.width =	-1,
	.accel_flags =	0,
	.pixclock =	39721,
	.left_margin =	40,
	.right_margin =	24,
	.upper_margin =	32,
	.lower_margin =	11,
	.hsync_len =	96,
	.vsync_len =	2,
	.vmode =	FB_VMODE_NONINTERLACED
};

/*
 * Utility functions
 */

inline static u32 RD32(unsigned char* base, s32 off)
{
	return fb_readl(base + off);
}

inline static void WR32(unsigned char* base, s32 off, u32 v)
{
	fb_writel(v, base + off);
}

inline static u32 pm2_RD(struct pm2fb_par* p, s32 off)
{
	return RD32(p->v_regs, off);
}

inline static void pm2_WR(struct pm2fb_par* p, s32 off, u32 v)
{
	WR32(p->v_regs, off, v);
}

inline static u32 pm2_RDAC_RD(struct pm2fb_par* p, s32 idx)
{
	int index = PM2R_RD_INDEXED_DATA;
	switch (p->type) {
	case PM2_TYPE_PERMEDIA2:
		pm2_WR(p, PM2R_RD_PALETTE_WRITE_ADDRESS, idx);
		break;
	case PM2_TYPE_PERMEDIA2V:
		pm2_WR(p, PM2VR_RD_INDEX_LOW, idx & 0xff);
		index = PM2VR_RD_INDEXED_DATA;
		break;
	}	
	mb();
	return pm2_RD(p, index);
}

inline static void pm2_RDAC_WR(struct pm2fb_par* p, s32 idx, u32 v)
{
	int index = PM2R_RD_INDEXED_DATA;
	switch (p->type) {
	case PM2_TYPE_PERMEDIA2:
		pm2_WR(p, PM2R_RD_PALETTE_WRITE_ADDRESS, idx);
		break;
	case PM2_TYPE_PERMEDIA2V:
		pm2_WR(p, PM2VR_RD_INDEX_LOW, idx & 0xff);
		index = PM2VR_RD_INDEXED_DATA;
		break;
	}	
	mb();
	pm2_WR(p, index, v);
}

inline static u32 pm2v_RDAC_RD(struct pm2fb_par* p, s32 idx)
{
	pm2_WR(p, PM2VR_RD_INDEX_LOW, idx & 0xff);
	mb();
	return pm2_RD(p, PM2VR_RD_INDEXED_DATA);
}

inline static void pm2v_RDAC_WR(struct pm2fb_par* p, s32 idx, u32 v)
{
	pm2_WR(p, PM2VR_RD_INDEX_LOW, idx & 0xff);
	mb();
	pm2_WR(p, PM2VR_RD_INDEXED_DATA, v);
}

#ifdef CONFIG_FB_PM2_FIFO_DISCONNECT
#define WAIT_FIFO(p,a)
#else
inline static void WAIT_FIFO(struct pm2fb_par* p, u32 a)
{
	while( pm2_RD(p, PM2R_IN_FIFO_SPACE) < a );
	mb();
}
#endif

/*
 * partial products for the supported horizontal resolutions.
 */
#define PACKPP(p0,p1,p2)	(((p2) << 6) | ((p1) << 3) | (p0))
static const struct {
	u16 width;
	u16 pp;
} pp_table[] = {
	{ 32,	PACKPP(1, 0, 0) }, { 64,	PACKPP(1, 1, 0) },
	{ 96,	PACKPP(1, 1, 1) }, { 128,	PACKPP(2, 1, 1) },
	{ 160,	PACKPP(2, 2, 1) }, { 192,	PACKPP(2, 2, 2) },
	{ 224,	PACKPP(3, 2, 1) }, { 256,	PACKPP(3, 2, 2) },
	{ 288,	PACKPP(3, 3, 1) }, { 320,	PACKPP(3, 3, 2) },
	{ 384,	PACKPP(3, 3, 3) }, { 416,	PACKPP(4, 3, 1) },
	{ 448,	PACKPP(4, 3, 2) }, { 512,	PACKPP(4, 3, 3) },
	{ 544,	PACKPP(4, 4, 1) }, { 576,	PACKPP(4, 4, 2) },
	{ 640,	PACKPP(4, 4, 3) }, { 768,	PACKPP(4, 4, 4) },
	{ 800,	PACKPP(5, 4, 1) }, { 832,	PACKPP(5, 4, 2) },
	{ 896,	PACKPP(5, 4, 3) }, { 1024,	PACKPP(5, 4, 4) },
	{ 1056,	PACKPP(5, 5, 1) }, { 1088,	PACKPP(5, 5, 2) },
	{ 1152,	PACKPP(5, 5, 3) }, { 1280,	PACKPP(5, 5, 4) },
	{ 1536,	PACKPP(5, 5, 5) }, { 1568,	PACKPP(6, 5, 1) },
	{ 1600,	PACKPP(6, 5, 2) }, { 1664,	PACKPP(6, 5, 3) },
	{ 1792,	PACKPP(6, 5, 4) }, { 2048,	PACKPP(6, 5, 5) },
	{ 0,	0 } };

static u32 partprod(u32 xres)
{
	int i;

	for (i = 0; pp_table[i].width && pp_table[i].width != xres; i++)
		;
	if ( pp_table[i].width == 0 )
		DPRINTK("invalid width %u\n", xres);
	return pp_table[i].pp;
}

static u32 to3264(u32 timing, int bpp, int is64)
{
	switch (bpp) {
	case 8:
		timing >>= 2 + is64;
		break;
	case 16:
		timing >>= 1 + is64;
		break;
	case 24:
		timing = (timing * 3) >> (2 + is64);
		break;
	case 32:
		if (is64)
			timing >>= 1;
		break;
	}
	return timing;
}

static void pm2_mnp(u32 clk, unsigned char* mm, unsigned char* nn,
		    unsigned char* pp)
{
	unsigned char m;
	unsigned char n;
	unsigned char p;
	u32 f;
	s32 curr;
	s32 delta = 100000;

	*mm = *nn = *pp = 0;
	for (n = 2; n < 15; n++) {
		for (m = 2; m; m++) {
			f = PM2_REFERENCE_CLOCK * m / n;
			if (f >= 150000 && f <= 300000) {
				for ( p = 0; p < 5; p++, f >>= 1) {
					curr = ( clk > f ) ? clk - f : f - clk;
					if ( curr < delta ) {
						delta=curr;
						*mm=m;
						*nn=n;
						*pp=p;
					}
				}
			}
		}
	}
}

static void pm2v_mnp(u32 clk, unsigned char* mm, unsigned char* nn,
		     unsigned char* pp)
{
	unsigned char m;
	unsigned char n;
	unsigned char p;
	u32 f;
	s32 delta = 1000;

	*mm = *nn = *pp = 0;
	for (n = 1; n; n++) {
		for ( m = 1; m; m++) {
			for ( p = 0; p < 2; p++) {
				f = PM2_REFERENCE_CLOCK * n / (m * (1 << (p + 1)));
				if ( clk > f - delta && clk < f + delta ) {
					delta = ( clk > f ) ? clk - f : f - clk;
					*mm=m;
					*nn=n;
					*pp=p;
				}
			}
		}
	}
}

static void clear_palette(struct pm2fb_par* p) {
	int i=256;

	WAIT_FIFO(p, 1);
	pm2_WR(p, PM2R_RD_PALETTE_WRITE_ADDRESS, 0);
	wmb();
	while (i--) {
		WAIT_FIFO(p, 3);
		pm2_WR(p, PM2R_RD_PALETTE_DATA, 0);
		pm2_WR(p, PM2R_RD_PALETTE_DATA, 0);
		pm2_WR(p, PM2R_RD_PALETTE_DATA, 0);
	}
}

#ifdef RESET_CARD_ON_MODE_SET
static void reset_card(struct pm2fb_par* p)
{
	if (p->type == PM2_TYPE_PERMEDIA2V)
		pm2_WR(p, PM2VR_RD_INDEX_HIGH, 0);
	pm2_WR(p, PM2R_RESET_STATUS, 0);
	mb();
	while (pm2_RD(p, PM2R_RESET_STATUS) & PM2F_BEING_RESET)
		;
	mb();
#ifdef CONFIG_FB_PM2_FIFO_DISCONNECT
	DPRINTK("FIFO disconnect enabled\n");
	pm2_WR(p, PM2R_FIFO_DISCON, 1);
	mb();
#endif
}
#endif

static void reset_config(struct pm2fb_par* p)
{
	WAIT_FIFO(p, 52);
	pm2_WR(p, PM2R_CHIP_CONFIG, pm2_RD(p, PM2R_CHIP_CONFIG)&
	       ~(PM2F_VGA_ENABLE|PM2F_VGA_FIXED));
	pm2_WR(p, PM2R_BYPASS_WRITE_MASK, ~(0L));
	pm2_WR(p, PM2R_FRAMEBUFFER_WRITE_MASK, ~(0L));
	pm2_WR(p, PM2R_FIFO_CONTROL, 0);
	pm2_WR(p, PM2R_APERTURE_ONE, 0);
	pm2_WR(p, PM2R_APERTURE_TWO, 0);
	pm2_WR(p, PM2R_RASTERIZER_MODE, 0);
	pm2_WR(p, PM2R_DELTA_MODE, PM2F_DELTA_ORDER_RGB);
	pm2_WR(p, PM2R_LB_READ_FORMAT, 0);
	pm2_WR(p, PM2R_LB_WRITE_FORMAT, 0); 
	pm2_WR(p, PM2R_LB_READ_MODE, 0);
	pm2_WR(p, PM2R_LB_SOURCE_OFFSET, 0);
	pm2_WR(p, PM2R_FB_SOURCE_OFFSET, 0);
	pm2_WR(p, PM2R_FB_PIXEL_OFFSET, 0);
	pm2_WR(p, PM2R_FB_WINDOW_BASE, 0);
	pm2_WR(p, PM2R_LB_WINDOW_BASE, 0);
	pm2_WR(p, PM2R_FB_SOFT_WRITE_MASK, ~(0L));
	pm2_WR(p, PM2R_FB_HARD_WRITE_MASK, ~(0L));
	pm2_WR(p, PM2R_FB_READ_PIXEL, 0);
	pm2_WR(p, PM2R_DITHER_MODE, 0);
	pm2_WR(p, PM2R_AREA_STIPPLE_MODE, 0);
	pm2_WR(p, PM2R_DEPTH_MODE, 0);
	pm2_WR(p, PM2R_STENCIL_MODE, 0);
	pm2_WR(p, PM2R_TEXTURE_ADDRESS_MODE, 0);
	pm2_WR(p, PM2R_TEXTURE_READ_MODE, 0);
	pm2_WR(p, PM2R_TEXEL_LUT_MODE, 0);
	pm2_WR(p, PM2R_YUV_MODE, 0);
	pm2_WR(p, PM2R_COLOR_DDA_MODE, 0);
	pm2_WR(p, PM2R_TEXTURE_COLOR_MODE, 0);
	pm2_WR(p, PM2R_FOG_MODE, 0);
	pm2_WR(p, PM2R_ALPHA_BLEND_MODE, 0);
	pm2_WR(p, PM2R_LOGICAL_OP_MODE, 0);
	pm2_WR(p, PM2R_STATISTICS_MODE, 0);
	pm2_WR(p, PM2R_SCISSOR_MODE, 0);
	pm2_WR(p, PM2R_FILTER_MODE, PM2F_SYNCHRONIZATION);
	switch (p->type) {
	case PM2_TYPE_PERMEDIA2:
		pm2_RDAC_WR(p, PM2I_RD_MODE_CONTROL, 0); /* no overlay */
		pm2_RDAC_WR(p, PM2I_RD_CURSOR_CONTROL, 0);
		pm2_RDAC_WR(p, PM2I_RD_MISC_CONTROL, PM2F_RD_PALETTE_WIDTH_8);
		break;
	case PM2_TYPE_PERMEDIA2V:
		pm2v_RDAC_WR(p, PM2VI_RD_MISC_CONTROL, 1); /* 8bit */
		break;
	}
	pm2_RDAC_WR(p, PM2I_RD_COLOR_KEY_CONTROL, 0);
	pm2_RDAC_WR(p, PM2I_RD_OVERLAY_KEY, 0);
	pm2_RDAC_WR(p, PM2I_RD_RED_KEY, 0);
	pm2_RDAC_WR(p, PM2I_RD_GREEN_KEY, 0);
	pm2_RDAC_WR(p, PM2I_RD_BLUE_KEY, 0);
}

static void set_aperture(struct pm2fb_par* p, u32 depth)
{
	WAIT_FIFO(p, 4);
#ifdef __LITTLE_ENDIAN
	pm2_WR(p, PM2R_APERTURE_ONE, 0);
	pm2_WR(p, PM2R_APERTURE_TWO, 0);
#else
	switch (depth) {
	case 8:
	case 24:
		pm2_WR(p, PM2R_APERTURE_ONE, 0);
		pm2_WR(p, PM2R_APERTURE_TWO, 1);
		break;
	case 16:
		pm2_WR(p, PM2R_APERTURE_ONE, 2);
		pm2_WR(p, PM2R_APERTURE_TWO, 1);
		break;
	case 32:
		pm2_WR(p, PM2R_APERTURE_ONE, 1);
		pm2_WR(p, PM2R_APERTURE_TWO, 1);
		break;
	}
#endif
}

static void set_color(struct pm2fb_par* p, unsigned char regno,
		      unsigned char r, unsigned char g, unsigned char b)
{
	WAIT_FIFO(p, 4);
	pm2_WR(p, PM2R_RD_PALETTE_WRITE_ADDRESS, regno);
	wmb();
	pm2_WR(p, PM2R_RD_PALETTE_DATA, r);
	wmb();
	pm2_WR(p, PM2R_RD_PALETTE_DATA, g);
	wmb();
	pm2_WR(p, PM2R_RD_PALETTE_DATA, b);
}

static void set_pixclock(struct pm2fb_par* par, u32 clk)
{
	int i;
	unsigned char m, n, p;

	switch (par->type) {
	case PM2_TYPE_PERMEDIA2:
		pm2_mnp(clk, &m, &n, &p);
		WAIT_FIFO(par, 8);
		pm2_RDAC_WR(par, PM2I_RD_PIXEL_CLOCK_A3, 0);
		wmb();
		pm2_RDAC_WR(par, PM2I_RD_PIXEL_CLOCK_A1, m);
		pm2_RDAC_WR(par, PM2I_RD_PIXEL_CLOCK_A2, n);
		wmb();
		pm2_RDAC_WR(par, PM2I_RD_PIXEL_CLOCK_A3, 8|p);
		wmb();
		pm2_RDAC_RD(par, PM2I_RD_PIXEL_CLOCK_STATUS);
		rmb();
		for (i = 256;
		     i && !(pm2_RD(par, PM2R_RD_INDEXED_DATA) & PM2F_PLL_LOCKED);
		     i--)
			;
		break;
	case PM2_TYPE_PERMEDIA2V:
		pm2v_mnp(clk/2, &m, &n, &p);
		WAIT_FIFO(par, 8);
		pm2_WR(par, PM2VR_RD_INDEX_HIGH, PM2VI_RD_CLK0_PRESCALE >> 8);
		pm2v_RDAC_WR(par, PM2VI_RD_CLK0_PRESCALE, m);
		pm2v_RDAC_WR(par, PM2VI_RD_CLK0_FEEDBACK, n);
		pm2v_RDAC_WR(par, PM2VI_RD_CLK0_POSTSCALE, p);
		pm2_WR(par, PM2VR_RD_INDEX_HIGH, 0);
		break;
	}
}

static void set_video(struct pm2fb_par* p, u32 video) {
	u32 tmp;
	u32 vsync;

	vsync = video;

	DPRINTK("video = 0x%x\n", video);
	
	/*
	 * The hardware cursor needs +vsync to recognise vert retrace.
	 * We may not be using the hardware cursor, but the X Glint
	 * driver may well. So always set +hsync/+vsync and then set
	 * the RAMDAC to invert the sync if necessary.
	 */
	vsync &= ~(PM2F_HSYNC_MASK|PM2F_VSYNC_MASK);
	vsync |= PM2F_HSYNC_ACT_HIGH|PM2F_VSYNC_ACT_HIGH;

	WAIT_FIFO(p, 5);
	pm2_WR(p, PM2R_VIDEO_CONTROL, vsync);

	switch (p->type) {
	case PM2_TYPE_PERMEDIA2:
		tmp = PM2F_RD_PALETTE_WIDTH_8;
		if ((video & PM2F_HSYNC_MASK) == PM2F_HSYNC_ACT_LOW)
			tmp |= 4; /* invert hsync */
		if ((video & PM2F_VSYNC_MASK) == PM2F_VSYNC_ACT_LOW)
			tmp |= 8; /* invert vsync */
		pm2_RDAC_WR(p, PM2I_RD_MISC_CONTROL, tmp);
		break;
	case PM2_TYPE_PERMEDIA2V:
		tmp = 0;
		if ((video & PM2F_HSYNC_MASK) == PM2F_HSYNC_ACT_LOW)
			tmp |= 1; /* invert hsync */
		if ((video & PM2F_VSYNC_MASK) == PM2F_VSYNC_ACT_LOW)
			tmp |= 4; /* invert vsync */
		pm2v_RDAC_WR(p, PM2VI_RD_SYNC_CONTROL, tmp);
		pm2v_RDAC_WR(p, PM2VI_RD_MISC_CONTROL, 1);
		break;
	}
}

/*
 *
 */

/**
 *      pm2fb_check_var - Optional function. Validates a var passed in. 
 *      @var: frame buffer variable screen structure
 *      @info: frame buffer structure that represents a single frame buffer 
 *
 *	Checks to see if the hardware supports the state requested by
 *	var passed in.
 *
 *	Returns negative errno on error, or zero on success.
 */
static int pm2fb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	u32 lpitch;

	if (var->bits_per_pixel != 8  && var->bits_per_pixel != 16 &&
	    var->bits_per_pixel != 24 && var->bits_per_pixel != 32) {
		DPRINTK("depth not supported: %u\n", var->bits_per_pixel);
		return -EINVAL;
	}

	if (var->xres != var->xres_virtual) {
		DPRINTK("virtual x resolution != physical x resolution not supported\n");
		return -EINVAL;
	}

	if (var->yres > var->yres_virtual) {
		DPRINTK("virtual y resolution < physical y resolution not possible\n");
		return -EINVAL;
	}

	if (var->xoffset) {
		DPRINTK("xoffset not supported\n");
		return -EINVAL;
	}

	if ((var->vmode & FB_VMODE_MASK) == FB_VMODE_INTERLACED) {
		DPRINTK("interlace not supported\n");
		return -EINVAL;
	}

	var->xres = (var->xres + 15) & ~15; /* could sometimes be 8 */
	lpitch = var->xres * ((var->bits_per_pixel + 7)>>3);
  
	if (var->xres < 320 || var->xres > 1600) {
		DPRINTK("width not supported: %u\n", var->xres);
		return -EINVAL;
	}
  
	if (var->yres < 200 || var->yres > 1200) {
		DPRINTK("height not supported: %u\n", var->yres);
		return -EINVAL;
	}
  
	if (lpitch * var->yres_virtual > info->fix.smem_len) {
		DPRINTK("no memory for screen (%ux%ux%u)\n",
			var->xres, var->yres_virtual, var->bits_per_pixel);
		return -EINVAL;
	}
  
	if (PICOS2KHZ(var->pixclock) > PM2_MAX_PIXCLOCK) {
		DPRINTK("pixclock too high (%ldKHz)\n", PICOS2KHZ(var->pixclock));
		return -EINVAL;
	}

	switch(var->bits_per_pixel) {
	case 8:
		var->red.length = var->green.length = var->blue.length = 8;
		break;
	case 16:
		var->red.offset   = 11;
		var->red.length   = 5;
		var->green.offset = 5;
		var->green.length = 6;
		var->blue.offset  = 0;
		var->blue.length  = 5;
		break;
	case 24:
		var->red.offset	  = 16;
		var->green.offset = 8;
		var->blue.offset  = 0;
		var->red.length = var->green.length = var->blue.length = 8;
	case 32:
		var->red.offset   = 16;
		var->green.offset = 8;
		var->blue.offset  = 0;
		var->red.length = var->green.length = var->blue.length = 8;
		break;
	}
	var->height = var->width = -1;
  
	var->accel_flags = 0;	/* Can't mmap if this is on */
	
	DPRINTK("Checking graphics mode at %dx%d depth %d\n",
		var->xres, var->yres, var->bits_per_pixel);
	return 0;
}

/**
 *      pm2fb_set_par - Alters the hardware state.
 *      @info: frame buffer structure that represents a single frame buffer
 *
 *	Using the fb_var_screeninfo in fb_info we set the resolution of the
 *	this particular framebuffer.
 */
static int pm2fb_set_par(struct fb_info *info)
{
	struct pm2fb_par *par = (struct pm2fb_par *) info->par;
	u32 pixclock;
	u32 width, height, depth;
	u32 hsstart, hsend, hbend, htotal;
	u32 vsstart, vsend, vbend, vtotal;
	u32 stride;
	u32 base;
	u32 video = 0;
	u32 clrmode = PM2F_RD_COLOR_MODE_RGB;
	u32 txtmap = 0;
	u32 pixsize = 0;
	u32 clrformat = 0;
	u32 xres;
	int data64;

#ifdef RESET_CARD_ON_MODE_SET
	reset_card(par);
#endif
	reset_config(par);
	clear_palette(par);
    
	width = (info->var.xres_virtual + 7) & ~7;
	height = info->var.yres_virtual;
	depth = (info->var.bits_per_pixel + 7) & ~7;
	depth = (depth > 32) ? 32 : depth;
	data64 = depth > 8 || par->type == PM2_TYPE_PERMEDIA2V;

	xres = (info->var.xres + 31) & ~31;
	pixclock = PICOS2KHZ(info->var.pixclock);
	if (pixclock > PM2_MAX_PIXCLOCK) {
		DPRINTK("pixclock too high (%uKHz)\n", pixclock);
		return -EINVAL;
	}
    
	hsstart = to3264(info->var.right_margin, depth, data64);
	hsend = hsstart + to3264(info->var.hsync_len, depth, data64);
	hbend = hsend + to3264(info->var.left_margin, depth, data64);
	htotal = to3264(xres, depth, data64) + hbend - 1;
	vsstart = (info->var.lower_margin)
		? info->var.lower_margin - 1
		: 0;	/* FIXME! */
	vsend = info->var.lower_margin + info->var.vsync_len - 1;
	vbend = info->var.lower_margin + info->var.vsync_len + info->var.upper_margin;
	vtotal = info->var.yres + vbend - 1;
	stride = to3264(width, depth, 1);
	base = to3264(info->var.yoffset * xres + info->var.xoffset, depth, 1);
	if (data64)
		video |= PM2F_DATA_64_ENABLE;
    
	if (info->var.sync & FB_SYNC_HOR_HIGH_ACT) {
		if (lowhsync) {
			DPRINTK("ignoring +hsync, using -hsync.\n");
			video |= PM2F_HSYNC_ACT_LOW;
		} else
			video |= PM2F_HSYNC_ACT_HIGH;
	}
	else
		video |= PM2F_HSYNC_ACT_LOW;
	if (info->var.sync & FB_SYNC_VERT_HIGH_ACT) {
		if (lowvsync) {
			DPRINTK("ignoring +vsync, using -vsync.\n");
			video |= PM2F_VSYNC_ACT_LOW;
		} else
			video |= PM2F_VSYNC_ACT_HIGH;
	}
	else
		video |= PM2F_VSYNC_ACT_LOW;
	if ((info->var.vmode & FB_VMODE_MASK)==FB_VMODE_INTERLACED) {
		DPRINTK("interlaced not supported\n");
		return -EINVAL;
	}
	if ((info->var.vmode & FB_VMODE_MASK)==FB_VMODE_DOUBLE)
		video |= PM2F_LINE_DOUBLE;
	if (info->var.activate==FB_ACTIVATE_NOW)
		video |= PM2F_VIDEO_ENABLE;
	par->video = video;

	info->fix.visual =
		(depth == 8) ? FB_VISUAL_PSEUDOCOLOR : FB_VISUAL_TRUECOLOR;
	info->fix.line_length = info->var.xres * depth / 8;
	info->cmap.len = 256;

	/*
	 * Settings calculated. Now write them out.
	 */
	if (par->type == PM2_TYPE_PERMEDIA2V) {
		WAIT_FIFO(par, 1);
		pm2_WR(par, PM2VR_RD_INDEX_HIGH, 0);
	}
    
	set_aperture(par, depth);
    
	mb();
	WAIT_FIFO(par, 19);
	pm2_RDAC_WR(par, PM2I_RD_COLOR_KEY_CONTROL,
		    ( depth == 8 ) ? 0 : PM2F_COLOR_KEY_TEST_OFF);
	switch (depth) {
	case 8:
		pm2_WR(par, PM2R_FB_READ_PIXEL, 0);
		clrformat = 0x0e;
		break;
	case 16:
		pm2_WR(par, PM2R_FB_READ_PIXEL, 1);
		clrmode |= PM2F_RD_TRUECOLOR | 0x06;
		txtmap = PM2F_TEXTEL_SIZE_16;
		pixsize = 1;
		clrformat = 0x70;
		break;
	case 32:
		pm2_WR(par, PM2R_FB_READ_PIXEL, 2);
		clrmode |= PM2F_RD_TRUECOLOR | 0x08;
		txtmap = PM2F_TEXTEL_SIZE_32;
		pixsize = 2;
		clrformat = 0x20;
		break;
	case 24:
		pm2_WR(par, PM2R_FB_READ_PIXEL, 4);
		clrmode |= PM2F_RD_TRUECOLOR | 0x09;
#ifndef PM2FB_BE_APERTURE
		clrmode &= ~PM2F_RD_COLOR_MODE_RGB;
#endif
		txtmap = PM2F_TEXTEL_SIZE_24;
		pixsize = 4;
		clrformat = 0x20;
		break;
	}
	pm2_WR(par, PM2R_FB_WRITE_MODE, PM2F_FB_WRITE_ENABLE);
	pm2_WR(par, PM2R_FB_READ_MODE, partprod(xres));
	pm2_WR(par, PM2R_LB_READ_MODE, partprod(xres));
	pm2_WR(par, PM2R_TEXTURE_MAP_FORMAT, txtmap | partprod(xres));
	pm2_WR(par, PM2R_H_TOTAL, htotal);
	pm2_WR(par, PM2R_HS_START, hsstart);
	pm2_WR(par, PM2R_HS_END, hsend);
	pm2_WR(par, PM2R_HG_END, hbend);
	pm2_WR(par, PM2R_HB_END, hbend);
	pm2_WR(par, PM2R_V_TOTAL, vtotal);
	pm2_WR(par, PM2R_VS_START, vsstart);
	pm2_WR(par, PM2R_VS_END, vsend);
	pm2_WR(par, PM2R_VB_END, vbend);
	pm2_WR(par, PM2R_SCREEN_STRIDE, stride);
	wmb();
	pm2_WR(par, PM2R_WINDOW_ORIGIN, 0);
	pm2_WR(par, PM2R_SCREEN_SIZE, (height << 16) | width);
	pm2_WR(par, PM2R_SCISSOR_MODE, PM2F_SCREEN_SCISSOR_ENABLE);
	wmb();
	pm2_WR(par, PM2R_SCREEN_BASE, base);
	wmb();
	set_video(par, video);
	WAIT_FIFO(par, 4);
	switch (par->type) {
	case PM2_TYPE_PERMEDIA2:
		pm2_RDAC_WR(par, PM2I_RD_COLOR_MODE,
			    PM2F_RD_COLOR_MODE_RGB | PM2F_RD_GUI_ACTIVE | clrmode);
		break;
	case PM2_TYPE_PERMEDIA2V:
		pm2v_RDAC_WR(par, PM2VI_RD_PIXEL_SIZE, pixsize);
		pm2v_RDAC_WR(par, PM2VI_RD_COLOR_FORMAT, clrformat);
		break;
	}
	set_pixclock(par, pixclock);
	DPRINTK("Setting graphics mode at %dx%d depth %d\n",
		info->var.xres, info->var.yres, info->var.bits_per_pixel);
	return 0;	
}

/**
 *  	pm2fb_setcolreg - Sets a color register.
 *      @regno: boolean, 0 copy local, 1 get_user() function
 *      @red: frame buffer colormap structure
 *	@green: The green value which can be up to 16 bits wide 
 *	@blue:  The blue value which can be up to 16 bits wide.
 *	@transp: If supported the alpha value which can be up to 16 bits wide.	
 *      @info: frame buffer info structure
 * 
 *  	Set a single color register. The values supplied have a 16 bit
 *  	magnitude which needs to be scaled in this function for the hardware.
 *	Pretty much a direct lift from tdfxfb.c.
 * 
 *	Returns negative errno on error, or zero on success.
 */
static int pm2fb_setcolreg(unsigned regno, unsigned red, unsigned green,
			   unsigned blue, unsigned transp,
			   struct fb_info *info)
{
	struct pm2fb_par *par = (struct pm2fb_par *) info->par;

	if (regno >= info->cmap.len)  /* no. of hw registers */
		return 1;
	/*
	 * Program hardware... do anything you want with transp
	 */

	/* grayscale works only partially under directcolor */
	if (info->var.grayscale) {
		/* grayscale = 0.30*R + 0.59*G + 0.11*B */
		red = green = blue = (red * 77 + green * 151 + blue * 28) >> 8;
	}

	/* Directcolor:
	 *   var->{color}.offset contains start of bitfield
	 *   var->{color}.length contains length of bitfield
	 *   {hardwarespecific} contains width of DAC
	 *   cmap[X] is programmed to
	 *   (X << red.offset) | (X << green.offset) | (X << blue.offset)
	 *   RAMDAC[X] is programmed to (red, green, blue)
	 *
	 * Pseudocolor:
	 *    uses offset = 0 && length = DAC register width.
	 *    var->{color}.offset is 0
	 *    var->{color}.length contains widht of DAC
	 *    cmap is not used
	 *    DAC[X] is programmed to (red, green, blue)
	 * Truecolor:
	 *    does not use RAMDAC (usually has 3 of them).
	 *    var->{color}.offset contains start of bitfield
	 *    var->{color}.length contains length of bitfield
	 *    cmap is programmed to
	 *    (red << red.offset) | (green << green.offset) |
	 *    (blue << blue.offset) | (transp << transp.offset)
	 *    RAMDAC does not exist
	 */
#define CNVT_TOHW(val,width) ((((val)<<(width))+0x7FFF-(val))>>16)
	switch (info->fix.visual) {
	case FB_VISUAL_TRUECOLOR:
	case FB_VISUAL_PSEUDOCOLOR:
		red = CNVT_TOHW(red, info->var.red.length);
		green = CNVT_TOHW(green, info->var.green.length);
		blue = CNVT_TOHW(blue, info->var.blue.length);
		transp = CNVT_TOHW(transp, info->var.transp.length);
		set_color(par, regno, red, green, blue);
		break;
	case FB_VISUAL_DIRECTCOLOR:
		/* example here assumes 8 bit DAC. Might be different 
		 * for your hardware */	
		red = CNVT_TOHW(red, 8);       
		green = CNVT_TOHW(green, 8);
		blue = CNVT_TOHW(blue, 8);
		/* hey, there is bug in transp handling... */
		transp = CNVT_TOHW(transp, 8);
		break;
	}
#undef CNVT_TOHW
	/* Truecolor has hardware independent palette */
	if (info->fix.visual == FB_VISUAL_TRUECOLOR) {
		u32 v;

		if (regno >= 16)
			return 1;

		v = (red << info->var.red.offset) |
			(green << info->var.green.offset) |
			(blue << info->var.blue.offset) |
			(transp << info->var.transp.offset);

		switch (info->var.bits_per_pixel) {
		case 8:
			/* Yes some hand held devices have this. */ 
           		((u8*)(info->pseudo_palette))[regno] = v;
			break;	
   		case 16:
           		((u16*)(info->pseudo_palette))[regno] = v;
			break;
		case 24:
		case 32:	
           		((u32*)(info->pseudo_palette))[regno] = v;
			break;
		}
		return 0;
	}
	/* ... */
	return 0;
}

/**
 *      pm2fb_pan_display - Pans the display.
 *      @var: frame buffer variable screen structure
 *      @info: frame buffer structure that represents a single frame buffer
 *
 *	Pan (or wrap, depending on the `vmode' field) the display using the
 *  	`xoffset' and `yoffset' fields of the `var' structure.
 *  	If the values don't fit, return -EINVAL.
 *
 *      Returns negative errno on error, or zero on success.
 *
 */
static int pm2fb_pan_display(struct fb_var_screeninfo *var,
			     struct fb_info *info)
{
	struct pm2fb_par *p = (struct pm2fb_par *) info->par;
	u32 base;
	u32 depth;
	u32 xres;

	xres = (var->xres + 31) & ~31;
	depth = (var->bits_per_pixel + 7) & ~7;
	depth = (depth > 32) ? 32 : depth;
	base = to3264(var->yoffset * xres + var->xoffset, depth, 1);
	WAIT_FIFO(p, 1);
	pm2_WR(p, PM2R_SCREEN_BASE, base);    
	return 0;
}

/**
 *      pm2fb_blank - Blanks the display.
 *      @blank_mode: the blank mode we want. 
 *      @info: frame buffer structure that represents a single frame buffer
 *
 *      Blank the screen if blank_mode != 0, else unblank. Return 0 if
 *      blanking succeeded, != 0 if un-/blanking failed due to e.g. a 
 *      video mode which doesn't support it. Implements VESA suspend
 *      and powerdown modes on hardware that supports disabling hsync/vsync:
 *      blank_mode == 2: suspend vsync
 *      blank_mode == 3: suspend hsync
 *      blank_mode == 4: powerdown
 *
 *      Returns negative errno on error, or zero on success.
 *
 */
static int pm2fb_blank(int blank_mode, struct fb_info *info)
{
	struct pm2fb_par *par = (struct pm2fb_par *) info->par;
	u32 video = par->video;

	DPRINTK("blank_mode %d\n", blank_mode);

	/* Turn everything on, then disable as requested. */
	video |= (PM2F_VIDEO_ENABLE | PM2F_HSYNC_MASK | PM2F_VSYNC_MASK);

	switch (blank_mode) {
	case 0: 	/* Screen: On; HSync: On, VSync: On */
		break;
	case 1: 	/* Screen: Off; HSync: On, VSync: On */
		video &= ~PM2F_VIDEO_ENABLE;
		break;
	case 2: /* Screen: Off; HSync: On, VSync: Off */
		video &= ~(PM2F_VIDEO_ENABLE | PM2F_VSYNC_MASK | PM2F_BLANK_LOW );
		break;
	case 3: /* Screen: Off; HSync: Off, VSync: On */
		video &= ~(PM2F_VIDEO_ENABLE | PM2F_HSYNC_MASK | PM2F_BLANK_LOW );
		break;
	case 4: /* Screen: Off; HSync: Off, VSync: Off */
		video &= ~(PM2F_VIDEO_ENABLE | PM2F_VSYNC_MASK | PM2F_HSYNC_MASK|
			   PM2F_BLANK_LOW);
		break;
	}
	set_video(par, video);
	return 0;
}

/* ------------ Hardware Independent Functions ------------ */

/*
 *  Frame buffer operations
 */

static struct fb_ops pm2fb_ops = {
	.owner		= THIS_MODULE,
	.fb_check_var	= pm2fb_check_var,
	.fb_set_par	= pm2fb_set_par,
	.fb_setcolreg	= pm2fb_setcolreg,
	.fb_blank	= pm2fb_blank,
	.fb_pan_display	= pm2fb_pan_display,
	.fb_fillrect	= cfb_fillrect,
	.fb_copyarea	= cfb_copyarea,
	.fb_imageblit	= cfb_imageblit,
	.fb_cursor	= soft_cursor,
};

/*
 * PCI stuff
 */


/**
 * Device initialisation
 *
 * Initialise and allocate resource for PCI device.
 *
 * @param	pdev	PCI device.
 * @param	id	PCI device ID.
 */
static int __devinit pm2fb_probe(struct pci_dev *pdev,
				 const struct pci_device_id *id)
{
	struct pm2fb_par *default_par;
	struct fb_info *info;
	int size, err;
	u32 pci_mem_config;
	int err_retval = -ENXIO;

	err = pci_enable_device(pdev);
	if ( err ) {
		printk(KERN_WARNING "pm2fb: Can't enable pdev: %d\n", err);
		return err;
	}

	size = sizeof(struct pm2fb_par) + 256 * sizeof(u32);
	info = framebuffer_alloc(size, &pdev->dev);
	if ( !info )
		return -ENOMEM;
	default_par = (struct pm2fb_par *) info->par;

	switch (pdev->device) {
	case  PCI_DEVICE_ID_TI_TVP4020:
		strcpy(pm2fb_fix.id, "TVP4020");
		default_par->type = PM2_TYPE_PERMEDIA2;
		break;
	case  PCI_DEVICE_ID_3DLABS_PERMEDIA2:
		strcpy(pm2fb_fix.id, "Permedia2");
		default_par->type = PM2_TYPE_PERMEDIA2;
		break;
	case  PCI_DEVICE_ID_3DLABS_PERMEDIA2V:
		strcpy(pm2fb_fix.id, "Permedia2v");
		default_par->type = PM2_TYPE_PERMEDIA2V;
		break;
	}

	pm2fb_fix.mmio_start = pci_resource_start(pdev, 0);
	pm2fb_fix.mmio_len = PM2_REGS_SIZE;

#ifdef PM2FB_BE_APERTURE
	pm2fb_fix.mmio_start += PM2_REGS_SIZE;
#endif
    
	/* Registers - request region and map it. */
	if ( !request_mem_region(pm2fb_fix.mmio_start, pm2fb_fix.mmio_len,
				 "pm2fb regbase") ) {
		printk(KERN_WARNING "pm2fb: Can't reserve regbase.\n");
		goto err_exit_neither;
	}
	default_par->v_regs =
		ioremap_nocache(pm2fb_fix.mmio_start, pm2fb_fix.mmio_len);
	if ( !default_par->v_regs ) {
		printk(KERN_WARNING "pm2fb: Can't remap %s register area.\n",
		       pm2fb_fix.id);
		release_mem_region(pm2fb_fix.mmio_start, pm2fb_fix.mmio_len);
		goto err_exit_neither;
	}

	/* Now work out how big lfb is going to be. */
	pci_mem_config = RD32(default_par->v_regs, PM2R_MEM_CONFIG);
	switch(pci_mem_config & PM2F_MEM_CONFIG_RAM_MASK) {
	case PM2F_MEM_BANKS_1:
		default_par->fb_size=0x200000;
		break;
	case PM2F_MEM_BANKS_2:
		default_par->fb_size=0x400000;
		break;
	case PM2F_MEM_BANKS_3:
		default_par->fb_size=0x600000;
		break;
	case PM2F_MEM_BANKS_4:
		default_par->fb_size=0x800000;
		break;
	}
	default_par->memclock = CVPPC_MEMCLOCK;
	pm2fb_fix.smem_start = pci_resource_start(pdev, 1);
	pm2fb_fix.smem_len = default_par->fb_size;

	/* Linear frame buffer - request region and map it. */
	if ( !request_mem_region(pm2fb_fix.smem_start, pm2fb_fix.smem_len,
				 "pm2fb smem") ) {
		printk(KERN_WARNING "pm2fb: Can't reserve smem.\n");
		goto err_exit_mmio;
	}
	info->screen_base = default_par->v_fb =
		ioremap_nocache(pm2fb_fix.smem_start, pm2fb_fix.smem_len);
	if ( !default_par->v_fb ) {
		printk(KERN_WARNING "pm2fb: Can't ioremap smem area.\n");
		release_mem_region(pm2fb_fix.smem_start, pm2fb_fix.smem_len);
		goto err_exit_mmio;
	}

	info->fbops		= &pm2fb_ops;
	info->fix		= pm2fb_fix; 	
	info->pseudo_palette	= (void *)(default_par + 1); 
	info->flags		= FBINFO_FLAG_DEFAULT;

#ifndef MODULE
	if (!mode)
		mode = "640x480@60";
	 
	err = fb_find_mode(&info->var, info, mode, NULL, 0, NULL, 8); 
	if (!err || err == 4)
#endif
		info->var = pm2fb_var;

	if (fb_alloc_cmap(&info->cmap, 256, 0) < 0)
		goto err_exit_all;

	if (register_framebuffer(info) < 0)
		goto err_exit_both;

	printk(KERN_INFO "fb%d: %s frame buffer device, memory = %dK.\n",
	       info->node, info->fix.id, default_par->fb_size / 1024);

	/*
	 * Our driver data
	 */
	pci_set_drvdata(pdev, info);

	return 0;

 err_exit_all:
	fb_dealloc_cmap(&info->cmap);	
 err_exit_both:    
	iounmap((void*) pm2fb_fix.smem_start);
	release_mem_region(pm2fb_fix.smem_start, pm2fb_fix.smem_len);
 err_exit_mmio:
	iounmap((void*) pm2fb_fix.mmio_start);
	release_mem_region(pm2fb_fix.mmio_start, pm2fb_fix.mmio_len);
 err_exit_neither:
	framebuffer_release(info);
	return err_retval;
}

/**
 * Device removal.
 *
 * Release all device resources.
 *
 * @param	pdev	PCI device to clean up.
 */
static void __devexit pm2fb_remove(struct pci_dev *pdev)
{
	struct fb_info* info = pci_get_drvdata(pdev);
	struct fb_fix_screeninfo* fix = &info->fix;
    
	unregister_framebuffer(info);
    
	iounmap((void*) fix->smem_start);
	release_mem_region(fix->smem_start, fix->smem_len);
	iounmap((void*) fix->mmio_start);
	release_mem_region(fix->mmio_start, fix->mmio_len);

	pci_set_drvdata(pdev, NULL);
	kfree(info);
}

static struct pci_device_id pm2fb_id_table[] = {
	{ PCI_VENDOR_ID_TI, PCI_DEVICE_ID_TI_TVP4020,
	  PCI_ANY_ID, PCI_ANY_ID, PCI_BASE_CLASS_DISPLAY << 16,
	  0xff0000, 0 },
	{ PCI_VENDOR_ID_3DLABS, PCI_DEVICE_ID_3DLABS_PERMEDIA2,
	  PCI_ANY_ID, PCI_ANY_ID, PCI_BASE_CLASS_DISPLAY << 16,
	  0xff0000, 0 },
	{ PCI_VENDOR_ID_3DLABS, PCI_DEVICE_ID_3DLABS_PERMEDIA2V,
	  PCI_ANY_ID, PCI_ANY_ID, PCI_BASE_CLASS_DISPLAY << 16,
	  0xff0000, 0 },
	{ 0, }
};

static struct pci_driver pm2fb_driver = {
	.name		= "pm2fb",
	.id_table 	= pm2fb_id_table,
	.probe 		= pm2fb_probe,
	.remove 	= __devexit_p(pm2fb_remove),
};

MODULE_DEVICE_TABLE(pci, pm2fb_id_table);


/*
 *  Initialization
 */

int __init pm2fb_init(void)
{
	return pci_module_init(&pm2fb_driver);
}

/*
 *  Cleanup
 */

static void __exit pm2fb_exit(void)
{
	pci_unregister_driver(&pm2fb_driver);
}

/*
 *  Setup
 */

/**
 * Parse user speficied options.
 *
 * This is, comma-separated options following `video=pm2fb:'.
 */
int __init pm2fb_setup(char *options)
{
	char* this_opt;

	if (!options || !*options)
		return 0;

	while ((this_opt = strsep(&options, ",")) != NULL) {	
		if (!*this_opt)
			continue;
		if(!strcmp(this_opt, "lowhsync")) {
			lowhsync = 1;
		} else if(!strcmp(this_opt, "lowvsync")) {
			lowvsync = 1;
		} else {
			mode = this_opt;
		}
	}
	return 0;
}


/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */



#ifdef MODULE
module_init(pm2fb_init);
#endif 
module_exit(pm2fb_exit);

MODULE_PARM(mode,"s");
MODULE_PARM(lowhsync,"i");
MODULE_PARM(lowvsync,"i");

MODULE_AUTHOR("Jim Hague <jim.hague@acm.org>");
MODULE_DESCRIPTION("Permedia2 framebuffer device driver");
MODULE_LICENSE("GPL");
