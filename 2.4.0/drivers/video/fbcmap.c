/*
 *  linux/drivers/video/fbcmap.c -- Colormap handling for frame buffer devices
 *
 *	Created 15 Jun 1997 by Geert Uytterhoeven
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/string.h>
#include <linux/module.h>
#include <linux/tty.h>
#include <linux/fb.h>
#include <linux/slab.h>

#include <asm/uaccess.h>

static u16 red2[] = {
    0x0000, 0xaaaa
};
static u16 green2[] = {
    0x0000, 0xaaaa
};
static u16 blue2[] = {
    0x0000, 0xaaaa
};

static u16 red4[] = {
    0x0000, 0xaaaa, 0x5555, 0xffff
};
static u16 green4[] = {
    0x0000, 0xaaaa, 0x5555, 0xffff
};
static u16 blue4[] = {
    0x0000, 0xaaaa, 0x5555, 0xffff
};

static u16 red8[] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa
};
static u16 green8[] = {
    0x0000, 0x0000, 0xaaaa, 0xaaaa, 0x0000, 0x0000, 0x5555, 0xaaaa
};
static u16 blue8[] = {
    0x0000, 0xaaaa, 0x0000, 0xaaaa, 0x0000, 0xaaaa, 0x0000, 0xaaaa
};

static u16 red16[] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa,
    0x5555, 0x5555, 0x5555, 0x5555, 0xffff, 0xffff, 0xffff, 0xffff
};
static u16 green16[] = {
    0x0000, 0x0000, 0xaaaa, 0xaaaa, 0x0000, 0x0000, 0x5555, 0xaaaa,
    0x5555, 0x5555, 0xffff, 0xffff, 0x5555, 0x5555, 0xffff, 0xffff
};
static u16 blue16[] = {
    0x0000, 0xaaaa, 0x0000, 0xaaaa, 0x0000, 0xaaaa, 0x0000, 0xaaaa,
    0x5555, 0xffff, 0x5555, 0xffff, 0x5555, 0xffff, 0x5555, 0xffff
};

static struct fb_cmap default_2_colors = {
    0, 2, red2, green2, blue2, NULL
};
static struct fb_cmap default_8_colors = {
    0, 8, red8, green8, blue8, NULL
};
static struct fb_cmap default_4_colors = {
    0, 4, red4, green4, blue4, NULL
};
static struct fb_cmap default_16_colors = {
    0, 16, red16, green16, blue16, NULL
};


    /*
     *  Allocate a colormap
     */

int fb_alloc_cmap(struct fb_cmap *cmap, int len, int transp)
{
    int size = len*sizeof(u16);

    if (cmap->len != len) {
	if (cmap->red)
	    kfree(cmap->red);
	if (cmap->green)
	    kfree(cmap->green);
	if (cmap->blue)
	    kfree(cmap->blue);
	if (cmap->transp)
	    kfree(cmap->transp);
	cmap->red = cmap->green = cmap->blue = cmap->transp = NULL;
	cmap->len = 0;
	if (!len)
	    return 0;
	if (!(cmap->red = kmalloc(size, GFP_ATOMIC)))
	    return -1;
	if (!(cmap->green = kmalloc(size, GFP_ATOMIC)))
	    return -1;
	if (!(cmap->blue = kmalloc(size, GFP_ATOMIC)))
	    return -1;
	if (transp) {
	    if (!(cmap->transp = kmalloc(size, GFP_ATOMIC)))
		return -1;
	} else
	    cmap->transp = NULL;
    }
    cmap->start = 0;
    cmap->len = len;
    fb_copy_cmap(fb_default_cmap(len), cmap, 0);
    return 0;
}


    /*
     *  Copy a colormap
     */

void fb_copy_cmap(struct fb_cmap *from, struct fb_cmap *to, int fsfromto)
{
    int size;
    int tooff = 0, fromoff = 0;

    if (to->start > from->start)
	fromoff = to->start-from->start;
    else
	tooff = from->start-to->start;
    size = to->len-tooff;
    if (size > from->len-fromoff)
	size = from->len-fromoff;
    if (size < 0)
	return;
    size *= sizeof(u16);
    
    switch (fsfromto) {
    case 0:
	memcpy(to->red+tooff, from->red+fromoff, size);
	memcpy(to->green+tooff, from->green+fromoff, size);
	memcpy(to->blue+tooff, from->blue+fromoff, size);
	if (from->transp && to->transp)
	    memcpy(to->transp+tooff, from->transp+fromoff, size);
        break;
    case 1:
	copy_from_user(to->red+tooff, from->red+fromoff, size);
	copy_from_user(to->green+tooff, from->green+fromoff, size);
	copy_from_user(to->blue+tooff, from->blue+fromoff, size);
	if (from->transp && to->transp)
            copy_from_user(to->transp+tooff, from->transp+fromoff, size);
	break;
    case 2:
	copy_to_user(to->red+tooff, from->red+fromoff, size);
	copy_to_user(to->green+tooff, from->green+fromoff, size);
	copy_to_user(to->blue+tooff, from->blue+fromoff, size);
	if (from->transp && to->transp)
	    copy_to_user(to->transp+tooff, from->transp+fromoff, size);
	break;
    }
}


    /*
     *  Get the colormap for a screen
     */

int fb_get_cmap(struct fb_cmap *cmap, int kspc,
    	    	int (*getcolreg)(u_int, u_int *, u_int *, u_int *, u_int *,
				 struct fb_info *),
		struct fb_info *info)
{
    int i, start;
    u16 *red, *green, *blue, *transp;
    u_int hred, hgreen, hblue, htransp;

    red = cmap->red;
    green = cmap->green;
    blue = cmap->blue;
    transp = cmap->transp;
    start = cmap->start;
    if (start < 0)
	return -EINVAL;
    for (i = 0; i < cmap->len; i++) {
	if (getcolreg(start++, &hred, &hgreen, &hblue, &htransp, info))
	    return 0;
	if (kspc) {
	    *red = hred;
	    *green = hgreen;
	    *blue = hblue;
	    if (transp)
		*transp = htransp;
	} else {
	    put_user(hred, red);
	    put_user(hgreen, green);
	    put_user(hblue, blue);
	    if (transp)
		put_user(htransp, transp);
	}
	red++;
	green++;
	blue++;
	if (transp)
	    transp++;
    }
    return 0;
}


    /*
     *  Set the colormap for a screen
     */

int fb_set_cmap(struct fb_cmap *cmap, int kspc,
    	    	int (*setcolreg)(u_int, u_int, u_int, u_int, u_int,
				 struct fb_info *),
		struct fb_info *info)
{
    int i, start;
    u16 *red, *green, *blue, *transp;
    u_int hred, hgreen, hblue, htransp;

    red = cmap->red;
    green = cmap->green;
    blue = cmap->blue;
    transp = cmap->transp;
    start = cmap->start;

    if (start < 0)
	return -EINVAL;
    for (i = 0; i < cmap->len; i++) {
	if (kspc) {
	    hred = *red;
	    hgreen = *green;
	    hblue = *blue;
	    htransp = transp ? *transp : 0;
	} else {
	    get_user(hred, red);
	    get_user(hgreen, green);
	    get_user(hblue, blue);
	    if (transp)
		get_user(htransp, transp);
	    else
		htransp = 0;
	}
	red++;
	green++;
	blue++;
	if (transp)
	    transp++;
	if (setcolreg(start++, hred, hgreen, hblue, htransp, info))
	    return 0;
    }
    return 0;
}


    /*
     *  Get the default colormap for a specific screen depth
     */

struct fb_cmap *fb_default_cmap(int len)
{
    if (len <= 2)
	return &default_2_colors;
    if (len <= 4)
	return &default_4_colors;
    if (len <= 8)
	return &default_8_colors;
    return &default_16_colors;
}


    /*
     *  Invert all default colormaps
     */

void fb_invert_cmaps(void)
{
    u_int i;

    for (i = 0; i < 2; i++) {
	red2[i] = ~red2[i];
	green2[i] = ~green2[i];
	blue2[i] = ~blue2[i];
    }
    for (i = 0; i < 4; i++) {
	red4[i] = ~red4[i];
	green4[i] = ~green4[i];
	blue4[i] = ~blue4[i];
    }
    for (i = 0; i < 8; i++) {
	red8[i] = ~red8[i];
	green8[i] = ~green8[i];
	blue8[i] = ~blue8[i];
    }
    for (i = 0; i < 16; i++) {
	red16[i] = ~red16[i];
	green16[i] = ~green16[i];
	blue16[i] = ~blue16[i];
    }
}


    /*
     *  Visible symbols for modules
     */

EXPORT_SYMBOL(fb_alloc_cmap);
EXPORT_SYMBOL(fb_copy_cmap);
EXPORT_SYMBOL(fb_get_cmap);
EXPORT_SYMBOL(fb_set_cmap);
EXPORT_SYMBOL(fb_default_cmap);
EXPORT_SYMBOL(fb_invert_cmaps);
