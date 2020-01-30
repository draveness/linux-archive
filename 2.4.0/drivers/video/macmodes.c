/*
 *  linux/drivers/video/macmodes.c -- Standard MacOS video modes
 *
 *	Copyright (C) 1998 Geert Uytterhoeven
 *
 *      2000 - Removal of OpenFirmware dependencies by:
 *      - Ani Joshi
 *      - Brad Douglas <brad@neruo.com>
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/config.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/string.h>

#ifdef CONFIG_FB_COMPAT_XPMAC
#include <asm/vc_ioctl.h>
#endif

#include <video/fbcon.h>
#include <video/macmodes.h>


    /*
     *  MacOS video mode definitions
     *
     *  Order IS important! If you change these, don't forget to update
     *  mac_modes[] below!
     */

#define DEFAULT_MODEDB_INDEX	0

static const struct fb_videomode mac_modedb[] = {
    {
	/* 640x480, 60 Hz, Non-Interlaced (25.175 MHz dotclock) */
	"mac5", 60, 640, 480, 39722, 32, 32, 33, 10, 96, 2,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 640x480, 67Hz, Non-Interlaced (30.0 MHz dotclock) */
	"mac6", 67, 640, 480, 33334, 80, 80, 39, 3, 64, 3,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 800x600, 56 Hz, Non-Interlaced (36.00 MHz dotclock) */
	"mac9", 56, 800, 600, 27778, 112, 40, 22, 1, 72, 2,
	FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    }, {
	/* 800x600, 60 Hz, Non-Interlaced (40.00 MHz dotclock) */
	"mac10", 60, 800, 600, 25000, 72, 56, 23, 1, 128, 4,
	FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    }, {
	/* 800x600, 72 Hz, Non-Interlaced (50.00 MHz dotclock) */
	"mac11", 72, 800, 600, 20000, 48, 72, 23, 37, 120, 6,
	FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    }, {
	/* 800x600, 75 Hz, Non-Interlaced (49.50 MHz dotclock) */
	"mac12", 75, 800, 600, 20203, 144, 32, 21, 1, 80, 3,
	FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    }, {
	/* 832x624, 75Hz, Non-Interlaced (57.6 MHz dotclock) */
	"mac13", 75, 832, 624, 17362, 208, 48, 39, 1, 64, 3,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1024x768, 60 Hz, Non-Interlaced (65.00 MHz dotclock) */
	"mac14", 60, 1024, 768, 15385, 144, 40, 29, 3, 136, 6,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1024x768, 72 Hz, Non-Interlaced (75.00 MHz dotclock) */
	"mac15", 72, 1024, 768, 13334, 128, 40, 29, 3, 136, 6,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1024x768, 75 Hz, Non-Interlaced (78.75 MHz dotclock) */
	"mac16", 75, 1024, 768, 12699, 176, 16, 28, 1, 96, 3,
	FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    }, {
	/* 1024x768, 75 Hz, Non-Interlaced (78.75 MHz dotclock) */
	"mac17", 75, 1024, 768, 12699, 160, 32, 28, 1, 96, 3,
	FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    }, {
	/* 1152x870, 75 Hz, Non-Interlaced (100.0 MHz dotclock) */
	"mac18", 75, 1152, 870, 10000, 128, 48, 39, 3, 128, 3,
	FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    }, {
	/* 1280x960, 75 Hz, Non-Interlaced (126.00 MHz dotclock) */
	"mac19", 75, 1280, 960, 7937, 224, 32, 36, 1, 144, 3,
	0, FB_VMODE_NONINTERLACED
    }, {
	/* 1280x1024, 75 Hz, Non-Interlaced (135.00 MHz dotclock) */
	"mac20", 75, 1280, 1024, 7408, 232, 64, 38, 1, 112, 3,
	FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
    },

#if 0
    /* Anyone who has timings for these? */
    {
	/* VMODE_512_384_60I: 512x384, 60Hz, Interlaced (NTSC) */
	"mac1", 60, 512, 384, pixclock, left, right, upper, lower, hslen, vslen,
	sync, FB_VMODE_INTERLACED
    }, {
	/* VMODE_512_384_60: 512x384, 60Hz, Non-Interlaced */
    	"mac2", 60, 512, 384, pixclock, left, right, upper, lower, hslen, vslen,
	sync, FB_VMODE_NONINTERLACED
    }, {
	/* VMODE_640_480_50I: 640x480, 50Hz, Interlaced (PAL) */
	"mac3", 50, 640, 480, pixclock, left, right, upper, lower, hslen, vslen,
	sync, FB_VMODE_INTERLACED
    }, {
	/* VMODE_640_480_60I: 640x480, 60Hz, Interlaced (NTSC) */
	"mac4", 60, 640, 480, pixclock, left, right, upper, lower, hslen, vslen,
	sync, FB_VMODE_INTERLACED
    }, {
	/* VMODE_640_870_75P: 640x870, 75Hz (portrait), Non-Interlaced */
	"mac7", 75, 640, 870, pixclock, left, right, upper, lower, hslen, vslen,
	sync, FB_VMODE_NONINTERLACED
    }, {
	/* VMODE_768_576_50I: 768x576, 50Hz (PAL full frame), Interlaced */
	"mac8", 50, 768, 576, pixclock, left, right, upper, lower, hslen, vslen,
	sync, FB_VMODE_INTERLACED
    },
#endif
};


    /*
     *  Mapping between MacOS video mode numbers and video mode definitions
     *
     *  These MUST be ordered in
     *    - increasing resolution
     *    - decreasing refresh rate
     */

static const struct mode_map {
    int vmode;
    const struct fb_videomode *mode;
} mac_modes[] = {
    /* 640x480 */
    { VMODE_640_480_67, &mac_modedb[1] },
    { VMODE_640_480_60, &mac_modedb[0] },
    /* 800x600 */
    { VMODE_800_600_75, &mac_modedb[5] },
    { VMODE_800_600_72, &mac_modedb[4] },
    { VMODE_800_600_60, &mac_modedb[3] },
    { VMODE_800_600_56, &mac_modedb[2] },
    /* 832x624 */
    { VMODE_832_624_75, &mac_modedb[6] },
    /* 1024x768 */
    { VMODE_1024_768_75, &mac_modedb[10] },
    { VMODE_1024_768_75V, &mac_modedb[9] },
    { VMODE_1024_768_70, &mac_modedb[8] },
    { VMODE_1024_768_60, &mac_modedb[7] },
    /* 1152x870 */
    { VMODE_1152_870_75, &mac_modedb[11] },
    /* 1280x960 */
    { VMODE_1280_960_75, &mac_modedb[12] },
    /* 1280x1024 */
    { VMODE_1280_1024_75, &mac_modedb[13] },
    { -1, NULL }
};


    /*
     *  Mapping between monitor sense values and MacOS video mode numbers
     */

static const struct monitor_map {
    int sense;
    int vmode;
} mac_monitors[] = {
    { 0x000, VMODE_1280_1024_75 },	/* 21" RGB */
    { 0x114, VMODE_640_870_75P },	/* Portrait Monochrome */
    { 0x221, VMODE_512_384_60 },	/* 12" RGB*/
    { 0x331, VMODE_1280_1024_75 },	/* 21" RGB (Radius) */
    { 0x334, VMODE_1280_1024_75 },	/* 21" mono (Radius) */
    { 0x335, VMODE_1280_1024_75 },	/* 21" mono */
    { 0x40A, VMODE_640_480_60I },	/* NTSC */
    { 0x51E, VMODE_640_870_75P },	/* Portrait RGB */
    { 0x603, VMODE_832_624_75 },	/* 12"-16" multiscan */
    { 0x60b, VMODE_1024_768_70 },	/* 13"-19" multiscan */
    { 0x623, VMODE_1152_870_75 },	/* 13"-21" multiscan */
    { 0x62b, VMODE_640_480_67 },	/* 13"/14" RGB */
    { 0x700, VMODE_640_480_50I },	/* PAL */
    { 0x714, VMODE_640_480_60I },	/* NTSC */
    { 0x717, VMODE_800_600_75 },	/* VGA */
    { 0x72d, VMODE_832_624_75 },	/* 16" RGB (Goldfish) */
    { 0x730, VMODE_768_576_50I },	/* PAL (Alternate) */
    { 0x73a, VMODE_1152_870_75 },	/* 3rd party 19" */
    { 0x73f, VMODE_640_480_67 },	/* no sense lines connected at all */
    { -1,    VMODE_640_480_60 },	/* catch-all, must be last */
};

#ifdef CONFIG_FB_COMPAT_XPMAC
struct fb_info *console_fb_info = NULL;
struct vc_mode display_info;

static u16 palette_red[16];
static u16 palette_green[16];
static u16 palette_blue[16];
static struct fb_cmap palette_cmap = {
    0, 16, palette_red, palette_green, palette_blue, NULL
};


int console_getmode(struct vc_mode *mode)
{
    *mode = display_info;
    return 0;
}

int console_setmode(struct vc_mode *mode, int doit)
{
    struct fb_var_screeninfo var;
    int cmode, err;

    if (!console_fb_info)
        return -EOPNOTSUPP;

    switch(mode->depth) {
        case 0:
        case 8:
            cmode = CMODE_8;
            break;
        case 16:
            cmode = CMODE_16;
            break;
        case 24:
        case 32:
            cmode = CMODE_32;
            break;
        default:
            return -EINVAL;
    }

    if ((err = mac_vmode_to_var(mode->mode, cmode, &var)))
        return err;

    var.activate = FB_ACTIVATE_TEST;
    err = console_fb_info->fbops->fb_set_var(&var, fg_console,
                                             console_fb_info);
    if (err || !doit)
        return err;
    else {
        int unit;

        var.activate = FB_ACTIVATE_NOW;
        for (unit = 0; unit < MAX_NR_CONSOLES; unit++)
            if (fb_display[unit].conp &&
                (GET_FB_IDX(console_fb_info->node) == con2fb_map[unit]))
                console_fb_info->fbops->fb_set_var(&var, unit,
                                                   console_fb_info);
    }

    return 0;
}

int console_setcmap(int n_entries, unsigned char *red, unsigned char *green,
                    unsigned char *blue)
{
    int i, j, n = 0, err;

    if (!console_fb_info)
        return -EOPNOTSUPP;

    for (i = 0; i < n_entries; i += n) {
        n = n_entries - i;
        if (n > 16)
            n = 16;
        palette_cmap.start = i;
        palette_cmap.len = n;

        for (j = 0; j < n; j++) {
            palette_cmap.red[j] = (red[i+j] << 8) | red[i+j];
            palette_cmap.green[j] = (green[i+j] << 8) | green[i+j];
            palette_cmap.blue[j] = (blue[i+j] << 8) | blue[i+j];
        }
        err = console_fb_info->fbops->fb_set_cmap(&palette_cmap, 1,
                                                  fg_console,
                                                  console_fb_info);
        if (err)
            return err;
    }

    return 0;
}

int console_powermode(int mode)
{
    if (mode == VC_POWERMODE_INQUIRY)
        return 0;
    if (mode < VESA_NO_BLANKING || mode > VESA_POWERDOWN)
        return -EINVAL;
    /* Not Supported */
    return -ENXIO;
}
#endif /* CONFIG_FB_COMPAT_XPMAC */


    /*
     *  Convert a MacOS vmode/cmode pair to a frame buffer video mode structure
     */

int mac_vmode_to_var(int vmode, int cmode, struct fb_var_screeninfo *var)
{
    const struct fb_videomode *mode = NULL;
    const struct mode_map *map;

    for (map = mac_modes; map->vmode != -1; map++)
	if (map->vmode == vmode) {
	    mode = map->mode;
	    break;
	}
    if (!mode)
	return -EINVAL;

    memset(var, 0, sizeof(struct fb_var_screeninfo));
    switch (cmode) {
	case CMODE_8:
	    var->bits_per_pixel = 8;
	    var->red.offset = 0;
	    var->red.length = 8;   
	    var->green.offset = 0;
	    var->green.length = 8;
	    var->blue.offset = 0;
	    var->blue.length = 8;
	    break;

	case CMODE_16:
	    var->bits_per_pixel = 16;
	    var->red.offset = 10;
	    var->red.length = 5;
	    var->green.offset = 5;
	    var->green.length = 5;
	    var->blue.offset = 0;
	    var->blue.length = 5;
	    break;

	case CMODE_32:
	    var->bits_per_pixel = 32;
	    var->red.offset = 16;
	    var->red.length = 8;
	    var->green.offset = 8;
	    var->green.length = 8;
	    var->blue.offset = 0;
	    var->blue.length = 8;
	    var->transp.offset = 24;
	    var->transp.length = 8;
	    break;

	default:
	    return -EINVAL;
    }
    var->xres = mode->xres;
    var->yres = mode->yres;
    var->xres_virtual = mode->xres;
    var->yres_virtual = mode->yres;
    var->height = -1;
    var->width = -1;
    var->pixclock = mode->pixclock;
    var->left_margin = mode->left_margin;
    var->right_margin = mode->right_margin;
    var->upper_margin = mode->upper_margin;
    var->lower_margin = mode->lower_margin;
    var->hsync_len = mode->hsync_len;
    var->vsync_len = mode->vsync_len;
    var->sync = mode->sync;
    var->vmode = mode->vmode;
    return 0;
}


    /*
     *  Convert a frame buffer video mode structure to a MacOS vmode/cmode pair
     */

int mac_var_to_vmode(const struct fb_var_screeninfo *var, int *vmode,
		     int *cmode)
{
    const struct fb_videomode *mode = NULL;
    const struct mode_map *map;

    if (var->bits_per_pixel <= 8)
	*cmode = CMODE_8;
    else if (var->bits_per_pixel <= 16)
	*cmode = CMODE_16;
    else if (var->bits_per_pixel <= 32)
	*cmode = CMODE_32;
    else
	return -EINVAL;

    for (map = mac_modes; map->vmode != -1; map++) {
	mode = map->mode;
	if (var->xres > mode->xres || var->yres > mode->yres)
	    continue;
	if (var->xres_virtual > mode->xres || var->yres_virtual > mode->yres)
	    continue;
	if (var->pixclock > mode->pixclock)
	    continue;
	if ((var->vmode & FB_VMODE_MASK) != mode->vmode)
	    continue;
	*vmode = map->vmode;
	return 0;
    }
    return -EINVAL;
}


    /*
     *  Convert a Mac monitor sense number to a MacOS vmode number
     */

int mac_map_monitor_sense(int sense)
{
    const struct monitor_map *map;

    for (map = mac_monitors; map->sense != -1; map++)
	if (map->sense == sense)
	    break;
    return map->vmode;
}


    /*
     *  Find a suitable video mode
     *
     *  If the name of the wanted mode begins with `mac', use the Mac video
     *  mode database, else fall back to the standard video mode database.
     */

int __init mac_find_mode(struct fb_var_screeninfo *var, struct fb_info *info,
			 const char *mode_option, unsigned int default_bpp)
{
    const struct fb_videomode *db = NULL;
    unsigned int dbsize = 0;

    if (mode_option && !strncmp(mode_option, "mac", 3)) {
	db = mac_modedb;
	dbsize = sizeof(mac_modedb)/sizeof(*mac_modedb);
    }
    return fb_find_mode(var, info, mode_option, db, dbsize,
			&mac_modedb[DEFAULT_MODEDB_INDEX], default_bpp);
}
