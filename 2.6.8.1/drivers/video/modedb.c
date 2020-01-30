/*
 *  linux/drivers/video/modedb.c -- Standard video mode database management
 *
 *	Copyright (C) 1999 Geert Uytterhoeven
 *
 *	2001 - Documented with DocBook
 *	- Brad Douglas <brad@neruo.com>
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/module.h>
#include <linux/tty.h>
#include <linux/fb.h>
#include <linux/sched.h>

#undef DEBUG

#define name_matches(v, s, l) \
    ((v).name && !strncmp((s), (v).name, (l)) && strlen((v).name) == (l))
#define res_matches(v, x, y) \
    ((v).xres == (x) && (v).yres == (y))

#ifdef DEBUG
#define DPRINTK(fmt, args...)	printk(KERN_DEBUG "%s: " fmt, __FUNCTION__ , ## args)
#else
#define DPRINTK(fmt, args...)
#endif


const char *global_mode_option = NULL;


    /*
     *  Standard video mode definitions (taken from XFree86)
     */

#define DEFAULT_MODEDB_INDEX	0

static const struct fb_videomode modedb[] = {
    {
	/* 640x400 @ 70 Hz, 31.5 kHz hsync */
	NULL, 70, 640, 400, 39721, 40, 24, 39, 9, 96, 2,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 640x480 @ 60 Hz, 31.5 kHz hsync */
	NULL, 60, 640, 480, 39721, 40, 24, 32, 11, 96, 2,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 800x600 @ 56 Hz, 35.15 kHz hsync */
	NULL, 56, 800, 600, 27777, 128, 24, 22, 1, 72, 2,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1024x768 @ 87 Hz interlaced, 35.5 kHz hsync */
	NULL, 87, 1024, 768, 22271, 56, 24, 33, 8, 160, 8,
	0, FB_VMODE_INTERLACED
    }, {
	/* 640x400 @ 85 Hz, 37.86 kHz hsync */
	NULL, 85, 640, 400, 31746, 96, 32, 41, 1, 64, 3,
	FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    }, {
	/* 640x480 @ 72 Hz, 36.5 kHz hsync */
	NULL, 72, 640, 480, 31746, 144, 40, 30, 8, 40, 3,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 640x480 @ 75 Hz, 37.50 kHz hsync */
	NULL, 75, 640, 480, 31746, 120, 16, 16, 1, 64, 3,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 800x600 @ 60 Hz, 37.8 kHz hsync */
	NULL, 60, 800, 600, 25000, 88, 40, 23, 1, 128, 4,
	FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    }, {
	/* 640x480 @ 85 Hz, 43.27 kHz hsync */
	NULL, 85, 640, 480, 27777, 80, 56, 25, 1, 56, 3,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1152x864 @ 89 Hz interlaced, 44 kHz hsync */
	NULL, 69, 1152, 864, 15384, 96, 16, 110, 1, 216, 10,
	0, FB_VMODE_INTERLACED
    }, {
	/* 800x600 @ 72 Hz, 48.0 kHz hsync */
	NULL, 72, 800, 600, 20000, 64, 56, 23, 37, 120, 6,
	FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    }, {
	/* 1024x768 @ 60 Hz, 48.4 kHz hsync */
	NULL, 60, 1024, 768, 15384, 168, 8, 29, 3, 144, 6,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 640x480 @ 100 Hz, 53.01 kHz hsync */
	NULL, 100, 640, 480, 21834, 96, 32, 36, 8, 96, 6,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1152x864 @ 60 Hz, 53.5 kHz hsync */
	NULL, 60, 1152, 864, 11123, 208, 64, 16, 4, 256, 8,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 800x600 @ 85 Hz, 55.84 kHz hsync */
	NULL, 85, 800, 600, 16460, 160, 64, 36, 16, 64, 5,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1024x768 @ 70 Hz, 56.5 kHz hsync */
	NULL, 70, 1024, 768, 13333, 144, 24, 29, 3, 136, 6,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1280x1024 @ 87 Hz interlaced, 51 kHz hsync */
	NULL, 87, 1280, 1024, 12500, 56, 16, 128, 1, 216, 12,
	0, FB_VMODE_INTERLACED
    }, {
	/* 800x600 @ 100 Hz, 64.02 kHz hsync */
	NULL, 100, 800, 600, 14357, 160, 64, 30, 4, 64, 6,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1024x768 @ 76 Hz, 62.5 kHz hsync */
	NULL, 76, 1024, 768, 11764, 208, 8, 36, 16, 120, 3,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1152x864 @ 70 Hz, 62.4 kHz hsync */
	NULL, 70, 1152, 864, 10869, 106, 56, 20, 1, 160, 10,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1280x1024 @ 61 Hz, 64.2 kHz hsync */
	NULL, 61, 1280, 1024, 9090, 200, 48, 26, 1, 184, 3,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1400x1050 @ 60Hz, 63.9 kHz hsync */
	NULL, 68, 1400, 1050, 9259, 136, 40, 13, 1, 112, 3,
	0, FB_VMODE_NONINTERLACED   	
    }, {
	/* 1400x1050 @ 75,107 Hz, 82,392 kHz +hsync +vsync*/
	NULL, 75, 1400, 1050, 9271, 120, 56, 13, 0, 112, 3,
	FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    }, {
	/* 1400x1050 @ 60 Hz, ? kHz +hsync +vsync*/
        NULL, 60, 1400, 1050, 9259, 128, 40, 12, 0, 112, 3,
	FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    }, {
	/* 1024x768 @ 85 Hz, 70.24 kHz hsync */
	NULL, 85, 1024, 768, 10111, 192, 32, 34, 14, 160, 6,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1152x864 @ 78 Hz, 70.8 kHz hsync */
	NULL, 78, 1152, 864, 9090, 228, 88, 32, 0, 84, 12,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1280x1024 @ 70 Hz, 74.59 kHz hsync */
	NULL, 70, 1280, 1024, 7905, 224, 32, 28, 8, 160, 8,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1600x1200 @ 60Hz, 75.00 kHz hsync */
	NULL, 60, 1600, 1200, 6172, 304, 64, 46, 1, 192, 3,
	FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    }, {
	/* 1152x864 @ 84 Hz, 76.0 kHz hsync */
	NULL, 84, 1152, 864, 7407, 184, 312, 32, 0, 128, 12,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1280x1024 @ 74 Hz, 78.85 kHz hsync */
	NULL, 74, 1280, 1024, 7407, 256, 32, 34, 3, 144, 3,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1024x768 @ 100Hz, 80.21 kHz hsync */
	NULL, 100, 1024, 768, 8658, 192, 32, 21, 3, 192, 10,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1280x1024 @ 76 Hz, 81.13 kHz hsync */
	NULL, 76, 1280, 1024, 7407, 248, 32, 34, 3, 104, 3,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1600x1200 @ 70 Hz, 87.50 kHz hsync */
	NULL, 70, 1600, 1200, 5291, 304, 64, 46, 1, 192, 3,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1152x864 @ 100 Hz, 89.62 kHz hsync */
	NULL, 100, 1152, 864, 7264, 224, 32, 17, 2, 128, 19,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1280x1024 @ 85 Hz, 91.15 kHz hsync */
	NULL, 85, 1280, 1024, 6349, 224, 64, 44, 1, 160, 3,
	FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    }, {
	/* 1600x1200 @ 75 Hz, 93.75 kHz hsync */
	NULL, 75, 1600, 1200, 4938, 304, 64, 46, 1, 192, 3,
	FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    }, {
	/* 1600x1200 @ 85 Hz, 105.77 kHz hsync */
	NULL, 85, 1600, 1200, 4545, 272, 16, 37, 4, 192, 3,
	FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    }, {
	/* 1280x1024 @ 100 Hz, 107.16 kHz hsync */
	NULL, 100, 1280, 1024, 5502, 256, 32, 26, 7, 128, 15,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1800x1440 @ 64Hz, 96.15 kHz hsync  */
	NULL, 64, 1800, 1440, 4347, 304, 96, 46, 1, 192, 3,
	FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    }, {
	/* 1800x1440 @ 70Hz, 104.52 kHz hsync  */
	NULL, 70, 1800, 1440, 4000, 304, 96, 46, 1, 192, 3,
	FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    }, {
	/* 512x384 @ 78 Hz, 31.50 kHz hsync */
	NULL, 78, 512, 384, 49603, 48, 16, 16, 1, 64, 3,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 512x384 @ 85 Hz, 34.38 kHz hsync */
	NULL, 85, 512, 384, 45454, 48, 16, 16, 1, 64, 3,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 320x200 @ 70 Hz, 31.5 kHz hsync, 8:5 aspect ratio */
	NULL, 70, 320, 200, 79440, 16, 16, 20, 4, 48, 1,
	0, FB_VMODE_DOUBLE
    }, {
	/* 320x240 @ 60 Hz, 31.5 kHz hsync, 4:3 aspect ratio */
	NULL, 60, 320, 240, 79440, 16, 16, 16, 5, 48, 1,
	0, FB_VMODE_DOUBLE
    }, {
	/* 320x240 @ 72 Hz, 36.5 kHz hsync */
	NULL, 72, 320, 240, 63492, 16, 16, 16, 4, 48, 2,
	0, FB_VMODE_DOUBLE
    }, {
	/* 400x300 @ 56 Hz, 35.2 kHz hsync, 4:3 aspect ratio */
	NULL, 56, 400, 300, 55555, 64, 16, 10, 1, 32, 1,
	0, FB_VMODE_DOUBLE
    }, {
	/* 400x300 @ 60 Hz, 37.8 kHz hsync */
	NULL, 60, 400, 300, 50000, 48, 16, 11, 1, 64, 2,
	0, FB_VMODE_DOUBLE
    }, {
	/* 400x300 @ 72 Hz, 48.0 kHz hsync */
	NULL, 72, 400, 300, 40000, 32, 24, 11, 19, 64, 3,
	0, FB_VMODE_DOUBLE
    }, {
	/* 480x300 @ 56 Hz, 35.2 kHz hsync, 8:5 aspect ratio */
	NULL, 56, 480, 300, 46176, 80, 16, 10, 1, 40, 1,
	0, FB_VMODE_DOUBLE
    }, {
	/* 480x300 @ 60 Hz, 37.8 kHz hsync */
	NULL, 60, 480, 300, 41858, 56, 16, 11, 1, 80, 2,
	0, FB_VMODE_DOUBLE
    }, {
	/* 480x300 @ 63 Hz, 39.6 kHz hsync */
	NULL, 63, 480, 300, 40000, 56, 16, 11, 1, 80, 2,
	0, FB_VMODE_DOUBLE
    }, {
	/* 480x300 @ 72 Hz, 48.0 kHz hsync */
	NULL, 72, 480, 300, 33386, 40, 24, 11, 19, 80, 3,
	0, FB_VMODE_DOUBLE
    },
};

const struct fb_videomode vesa_modes[] = {
	/* 0 640x350-85 VESA */
	{ NULL, 85, 640, 350, 31746,  96, 32, 60, 32, 64, 3,
	  FB_SYNC_HOR_HIGH_ACT, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA},
	/* 1 640x400-85 VESA */
	{ NULL, 85, 640, 400, 31746,  96, 32, 41, 01, 64, 3,
	  FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 2 720x400-85 VESA */
	{ NULL, 85, 721, 400, 28169, 108, 36, 42, 01, 72, 3,
	  FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 3 640x480-60 VESA */
	{ NULL, 60, 640, 480, 39682,  48, 16, 33, 10, 96, 2, 
	  0, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 4 640x480-72 VESA */
	{ NULL, 72, 640, 480, 31746, 128, 24, 29, 9, 40, 2, 
	  0, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 5 640x480-75 VESA */
	{ NULL, 75, 640, 480, 31746, 120, 16, 16, 01, 64, 3,
	  0, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 6 640x480-85 VESA */
	{ NULL, 85, 640, 480, 27777, 80, 56, 25, 01, 56, 3,
	  0, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 7 800x600-56 VESA */
	{ NULL, 56, 800, 600, 27777, 128, 24, 22, 01, 72, 2,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 8 800x600-60 VESA */
	{ NULL, 60, 800, 600, 25000, 88, 40, 23, 01, 128, 4,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 9 800x600-72 VESA */
	{ NULL, 72, 800, 600, 20000, 64, 56, 23, 37, 120, 6,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 10 800x600-75 VESA */
	{ NULL, 75, 800, 600, 20202, 160, 16, 21, 01, 80, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 11 800x600-85 VESA */
	{ NULL, 85, 800, 600, 17761, 152, 32, 27, 01, 64, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
        /* 12 1024x768i-43 VESA */
	{ NULL, 53, 1024, 768, 22271, 56, 8, 41, 0, 176, 8,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_INTERLACED, FB_MODE_IS_VESA },
	/* 13 1024x768-60 VESA */
	{ NULL, 60, 1024, 768, 15384, 160, 24, 29, 3, 136, 6,
	  0, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 14 1024x768-70 VESA */
	{ NULL, 70, 1024, 768, 13333, 144, 24, 29, 3, 136, 6,
	  0, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 15 1024x768-75 VESA */
	{ NULL, 75, 1024, 768, 12690, 176, 16, 28, 1, 96, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 16 1024x768-85 VESA */
	{ NULL, 85, 1024, 768, 10582, 208, 48, 36, 1, 96, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 17 1152x864-75 VESA */
	{ NULL, 75, 1153, 864, 9259, 256, 64, 32, 1, 128, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 18 1280x960-60 VESA */
	{ NULL, 60, 1280, 960, 9259, 312, 96, 36, 1, 112, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 19 1280x960-85 VESA */
	{ NULL, 85, 1280, 960, 6734, 224, 64, 47, 1, 160, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 20 1280x1024-60 VESA */
	{ NULL, 60, 1280, 1024, 9259, 248, 48, 38, 1, 112, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 21 1280x1024-75 VESA */
	{ NULL, 75, 1280, 1024, 7407, 248, 16, 38, 1, 144, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 22 1280x1024-85 VESA */
	{ NULL, 85, 1280, 1024, 6349, 224, 64, 44, 1, 160, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 23 1600x1200-60 VESA */
	{ NULL, 60, 1600, 1200, 6172, 304, 64, 46, 1, 192, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 24 1600x1200-65 VESA */
	{ NULL, 65, 1600, 1200, 5698, 304,  64, 46, 1, 192, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 25 1600x1200-70 VESA */
	{ NULL, 70, 1600, 1200, 5291, 304, 64, 46, 1, 192, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 26 1600x1200-75 VESA */
	{ NULL, 75, 1600, 1200, 4938, 304, 64, 46, 1, 192, 3, 
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 27 1600x1200-85 VESA */
	{ NULL, 85, 1600, 1200, 4357, 304, 64, 46, 1, 192, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 28 1792x1344-60 VESA */
	{ NULL, 60, 1792, 1344, 4882, 328, 128, 46, 1, 200, 3,
	  FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 29 1792x1344-75 VESA */
	{ NULL, 75, 1792, 1344, 3831, 352, 96, 69, 1, 216, 3,
	  FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 30 1856x1392-60 VESA */
	{ NULL, 60, 1856, 1392, 4580, 352, 96, 43, 1, 224, 3,
	  FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 31 1856x1392-75 VESA */
	{ NULL, 75, 1856, 1392, 3472, 352, 128, 104, 1, 224, 3,
	  FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 32 1920x1440-60 VESA */
	{ NULL, 60, 1920, 1440, 4273, 344, 128, 56, 1, 200, 3,
	  FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 33 1920x1440-75 VESA */
	{ NULL, 60, 1920, 1440, 3367, 352, 144, 56, 1, 224, 3,
	  FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
};

static int my_atoi(const char *name)
{
    int val = 0;

    for (;; name++) {
	switch (*name) {
	    case '0'...'9':
		val = 10*val+(*name-'0');
		break;
	    default:
		return val;
	}
    }
}

/**
 *	fb_try_mode - test a video mode
 *	@var: frame buffer user defined part of display
 *	@info: frame buffer info structure
 *	@mode: frame buffer video mode structure
 *	@bpp: color depth in bits per pixel
 *
 *	Tries a video mode to test it's validity for device @info.
 *
 *	Returns 1 on success.
 *
 */

int fb_try_mode(struct fb_var_screeninfo *var, struct fb_info *info,
		  const struct fb_videomode *mode, unsigned int bpp)
{
    int err = 0;

    DPRINTK("Trying mode %s %dx%d-%d@%d\n", mode->name ? mode->name : "noname",
	    mode->xres, mode->yres, bpp, mode->refresh);
    var->xres = mode->xres;
    var->yres = mode->yres;
    var->xres_virtual = mode->xres;
    var->yres_virtual = mode->yres;
    var->xoffset = 0;
    var->yoffset = 0;
    var->bits_per_pixel = bpp;
    var->activate |= FB_ACTIVATE_TEST;
    var->pixclock = mode->pixclock;
    var->left_margin = mode->left_margin;
    var->right_margin = mode->right_margin;
    var->upper_margin = mode->upper_margin;
    var->lower_margin = mode->lower_margin;
    var->hsync_len = mode->hsync_len;
    var->vsync_len = mode->vsync_len;
    var->sync = mode->sync;
    var->vmode = mode->vmode;
    if (info->fbops->fb_check_var)
    	err = info->fbops->fb_check_var(var, info);
    var->activate &= ~FB_ACTIVATE_TEST;
    return err;
}

/**
 *	fb_find_mode - finds a valid video mode
 *	@var: frame buffer user defined part of display
 *	@info: frame buffer info structure
 *	@mode_option: string video mode to find
 *	@db: video mode database
 *	@dbsize: size of @db
 *	@default_mode: default video mode to fall back to
 *	@default_bpp: default color depth in bits per pixel
 *
 *	Finds a suitable video mode, starting with the specified mode
 *	in @mode_option with fallback to @default_mode.  If
 *	@default_mode fails, all modes in the video mode database will
 *	be tried.
 *
 *	Valid mode specifiers for @mode_option:
 *
 *	<xres>x<yres>[-<bpp>][@<refresh>] or
 *	<name>[-<bpp>][@<refresh>]
 *
 *	with <xres>, <yres>, <bpp> and <refresh> decimal numbers and
 *	<name> a string.
 *
 *	NOTE: The passed struct @var is _not_ cleared!  This allows you
 *	to supply values for e.g. the grayscale and accel_flags fields.
 *
 *	Returns zero for failure, 1 if using specified @mode_option,
 *	2 if using specified @mode_option with an ignored refresh rate,
 *	3 if default mode is used, 4 if fall back to any valid mode.
 *
 */

int fb_find_mode(struct fb_var_screeninfo *var,
		 struct fb_info *info, const char *mode_option,
		 const struct fb_videomode *db, unsigned int dbsize,
		 const struct fb_videomode *default_mode,
		 unsigned int default_bpp)
{
    int i, j;

    /* Set up defaults */
    if (!db) {
	db = modedb;
	dbsize = sizeof(modedb)/sizeof(*modedb);
    }
    if (!default_mode)
	default_mode = &modedb[DEFAULT_MODEDB_INDEX];
    if (!default_bpp)
	default_bpp = 8;

    /* Did the user specify a video mode? */
    if (mode_option || (mode_option = global_mode_option)) {
	const char *name = mode_option;
	unsigned int namelen = strlen(name);
	int res_specified = 0, bpp_specified = 0, refresh_specified = 0;
	unsigned int xres = 0, yres = 0, bpp = default_bpp, refresh = 0;
	int yres_specified = 0;
	u32 best = -1, diff = -1;

	for (i = namelen-1; i >= 0; i--) {
	    switch (name[i]) {
		case '@':
		    namelen = i;
		    if (!refresh_specified && !bpp_specified &&
			!yres_specified) {
			refresh = my_atoi(&name[i+1]);
			refresh_specified = 1;
		    } else
			goto done;
		    break;
		case '-':
		    namelen = i;
		    if (!bpp_specified && !yres_specified) {
			bpp = my_atoi(&name[i+1]);
			bpp_specified = 1;
		    } else
			goto done;
		    break;
		case 'x':
		    if (!yres_specified) {
			yres = my_atoi(&name[i+1]);
			yres_specified = 1;
		    } else
			goto done;
		    break;
		case '0'...'9':
		    break;
		default:
		    goto done;
	    }
	}
	if (i < 0 && yres_specified) {
	    xres = my_atoi(name);
	    res_specified = 1;
	}
done:
	for (i = refresh_specified; i >= 0; i--) {
	    DPRINTK("Trying specified video mode%s %ix%i\n",
		    i ? "" : " (ignoring refresh rate)", xres, yres);
	    for (j = 0; j < dbsize; j++)
		if ((name_matches(db[j], name, namelen) ||
		     (res_specified && res_matches(db[j], xres, yres))) &&
		    (!i || db[j].refresh == refresh) &&
		    !fb_try_mode(var, info, &db[j], bpp))
		    return 2-i;
	}
	DPRINTK("Trying best-fit modes\n");
	for (i = 0; i < dbsize; i++) {
	    if (xres <= db[i].xres && yres <= db[i].yres) {
		DPRINTK("Trying %ix%i\n", db[i].xres, db[i].yres);
		if (!fb_try_mode(var, info, &db[i], bpp)) {
		    if (diff > (db[i].xres - xres) + (db[i].yres - yres)) {
			diff = (db[i].xres - xres) + (db[i].yres - yres);
			best = i;
		    }
		}
	    }
	}
	if (best != -1) {
	    fb_try_mode(var, info, &db[best], bpp);
	    return 5;
	}
    }

    DPRINTK("Trying default video mode\n");
    if (!fb_try_mode(var, info, default_mode, default_bpp))
	return 3;

    DPRINTK("Trying all modes\n");
    for (i = 0; i < dbsize; i++)
	if (!fb_try_mode(var, info, &db[i], default_bpp))
	    return 4;

    DPRINTK("No valid mode found\n");
    return 0;
}

EXPORT_SYMBOL(vesa_modes);
EXPORT_SYMBOL(fb_find_mode);
