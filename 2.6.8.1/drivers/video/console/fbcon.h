/*
 *  linux/drivers/video/console/fbcon.h -- Low level frame buffer based console driver
 *
 *	Copyright (C) 1997 Geert Uytterhoeven
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of this archive
 *  for more details.
 */

#ifndef _VIDEO_FBCON_H
#define _VIDEO_FBCON_H

#include <linux/config.h>
#include <linux/types.h>
#include <linux/vt_buffer.h>
#include <linux/vt_kern.h>

#include <asm/io.h>

   /*
    *    This is the interface between the low-level console driver and the
    *    low-level frame buffer device
    */

struct display {
    /* Filled in by the frame buffer device */
    u_short inverse;                /* != 0 text black on white as default */
    /* Filled in by the low-level console driver */
    u_char *fontdata;
    int userfont;                   /* != 0 if fontdata kmalloc()ed */
    u_short scrollmode;             /* Scroll Method */
    short yscroll;                  /* Hardware scrolling */
    int vrows;                      /* number of virtual rows */
    int cursor_shape;
};

/* drivers/video/console/fbcon.c */
extern signed char con2fb_map[MAX_NR_CONSOLES];
extern int set_con2fb_map(int unit, int newidx);

    /*
     *  Attribute Decoding
     */

/* Color */
#define attr_fgcol(fgshift,s)    \
	(((s) >> (fgshift)) & 0x0f)
#define attr_bgcol(bgshift,s)    \
	(((s) >> (bgshift)) & 0x0f)
#define	attr_bgcol_ec(bgshift,vc) \
	((vc) ? (((vc)->vc_video_erase_char >> (bgshift)) & 0x0f) : 0)
#define attr_fgcol_ec(fgshift,vc) \
	((vc) ? (((vc)->vc_video_erase_char >> (fgshift)) & 0x0f) : 0)

/* Monochrome */
#define attr_bold(s) \
	((s) & 0x200)
#define attr_reverse(s, inverse) \
	(((s) & 0x800) ^ (inverse ? 0x800 : 0))
#define attr_underline(s) \
	((s) & 0x400)
#define attr_blink(s) \
	((s) & 0x8000)
	
    /*
     *  Scroll Method
     */
     
/* There are several methods fbcon can use to move text around the screen:
 *
 *                     Operation   Pan    Wrap
 *---------------------------------------------
 * SCROLL_MOVE         copyarea    No     No
 * SCROLL_PAN_MOVE     copyarea    Yes    No
 * SCROLL_WRAP_MOVE    copyarea    No     Yes
 * SCROLL_REDRAW       imageblit   No     No
 * SCROLL_PAN_REDRAW   imageblit   Yes    No
 * SCROLL_WRAP_REDRAW  imageblit   No     Yes
 *
 * (SCROLL_WRAP_REDRAW is not implemented yet)
 *
 * In general, fbcon will choose the best scrolling
 * method based on the rule below:
 *
 * Pan/Wrap > accel imageblit > accel copyarea >
 * soft imageblit > (soft copyarea)
 *
 * Exception to the rule: Pan + accel copyarea is
 * preferred over Pan + accel imageblit.
 *
 * The above is typical for PCI/AGP cards. Unless
 * overridden, fbcon will never use soft copyarea.
 *
 * If you need to override the above rule, set the
 * appropriate flags in fb_info->flags.  For example,
 * to prefer copyarea over imageblit, set
 * FBINFO_READS_FAST.
 *
 * Other notes:
 * + use the hardware engine to move the text
 *    (hw-accelerated copyarea() and fillrect())
 * + use hardware-supported panning on a large virtual screen
 * + amifb can not only pan, but also wrap the display by N lines
 *    (i.e. visible line i = physical line (i+N) % yres).
 * + read what's already rendered on the screen and
 *     write it in a different place (this is cfb_copyarea())
 * + re-render the text to the screen
 *
 * Whether to use wrapping or panning can only be figured out at
 * runtime (when we know whether our font height is a multiple
 * of the pan/wrap step)
 *
 */

#define SCROLL_MOVE	   0x001
#define SCROLL_PAN_MOVE	   0x002
#define SCROLL_WRAP_MOVE   0x003
#define SCROLL_REDRAW	   0x004
#define SCROLL_PAN_REDRAW  0x005

extern int fb_console_init(void);

#endif /* _VIDEO_FBCON_H */
