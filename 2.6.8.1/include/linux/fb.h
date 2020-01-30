#ifndef _LINUX_FB_H
#define _LINUX_FB_H

#include <asm/types.h>

/* Definitions of frame buffers						*/

#define FB_MAJOR		29
#define FB_MAX			32	/* sufficient for now */

/* ioctls
   0x46 is 'F'								*/
#define FBIOGET_VSCREENINFO	0x4600
#define FBIOPUT_VSCREENINFO	0x4601
#define FBIOGET_FSCREENINFO	0x4602
#define FBIOGETCMAP		0x4604
#define FBIOPUTCMAP		0x4605
#define FBIOPAN_DISPLAY		0x4606
#ifdef __KERNEL__
#define FBIO_CURSOR            _IOWR('F', 0x08, struct fb_cursor_user)
#else
#define FBIO_CURSOR            _IOWR('F', 0x08, struct fb_cursor)
#endif
/* 0x4607-0x460B are defined below */
/* #define FBIOGET_MONITORSPEC	0x460C */
/* #define FBIOPUT_MONITORSPEC	0x460D */
/* #define FBIOSWITCH_MONIBIT	0x460E */
#define FBIOGET_CON2FBMAP	0x460F
#define FBIOPUT_CON2FBMAP	0x4610
#define FBIOBLANK		0x4611		/* arg: 0 or vesa level + 1 */
#define FBIOGET_VBLANK		_IOR('F', 0x12, struct fb_vblank)
#define FBIO_ALLOC              0x4613
#define FBIO_FREE               0x4614
#define FBIOGET_GLYPH           0x4615
#define FBIOGET_HWCINFO         0x4616
#define FBIOPUT_MODEINFO        0x4617
#define FBIOGET_DISPINFO        0x4618


#define FB_TYPE_PACKED_PIXELS		0	/* Packed Pixels	*/
#define FB_TYPE_PLANES			1	/* Non interleaved planes */
#define FB_TYPE_INTERLEAVED_PLANES	2	/* Interleaved planes	*/
#define FB_TYPE_TEXT			3	/* Text/attributes	*/
#define FB_TYPE_VGA_PLANES		4	/* EGA/VGA planes	*/

#define FB_AUX_TEXT_MDA		0	/* Monochrome text */
#define FB_AUX_TEXT_CGA		1	/* CGA/EGA/VGA Color text */
#define FB_AUX_TEXT_S3_MMIO	2	/* S3 MMIO fasttext */
#define FB_AUX_TEXT_MGA_STEP16	3	/* MGA Millenium I: text, attr, 14 reserved bytes */
#define FB_AUX_TEXT_MGA_STEP8	4	/* other MGAs:      text, attr,  6 reserved bytes */

#define FB_AUX_VGA_PLANES_VGA4		0	/* 16 color planes (EGA/VGA) */
#define FB_AUX_VGA_PLANES_CFB4		1	/* CFB4 in planes (VGA) */
#define FB_AUX_VGA_PLANES_CFB8		2	/* CFB8 in planes (VGA) */

#define FB_VISUAL_MONO01		0	/* Monochr. 1=Black 0=White */
#define FB_VISUAL_MONO10		1	/* Monochr. 1=White 0=Black */
#define FB_VISUAL_TRUECOLOR		2	/* True color	*/
#define FB_VISUAL_PSEUDOCOLOR		3	/* Pseudo color (like atari) */
#define FB_VISUAL_DIRECTCOLOR		4	/* Direct color */
#define FB_VISUAL_STATIC_PSEUDOCOLOR	5	/* Pseudo color readonly */

#define FB_ACCEL_NONE		0	/* no hardware accelerator	*/
#define FB_ACCEL_ATARIBLITT	1	/* Atari Blitter		*/
#define FB_ACCEL_AMIGABLITT	2	/* Amiga Blitter                */
#define FB_ACCEL_S3_TRIO64	3	/* Cybervision64 (S3 Trio64)    */
#define FB_ACCEL_NCR_77C32BLT	4	/* RetinaZ3 (NCR 77C32BLT)      */
#define FB_ACCEL_S3_VIRGE	5	/* Cybervision64/3D (S3 ViRGE)	*/
#define FB_ACCEL_ATI_MACH64GX	6	/* ATI Mach 64GX family		*/
#define FB_ACCEL_DEC_TGA	7	/* DEC 21030 TGA		*/
#define FB_ACCEL_ATI_MACH64CT	8	/* ATI Mach 64CT family		*/
#define FB_ACCEL_ATI_MACH64VT	9	/* ATI Mach 64CT family VT class */
#define FB_ACCEL_ATI_MACH64GT	10	/* ATI Mach 64CT family GT class */
#define FB_ACCEL_SUN_CREATOR	11	/* Sun Creator/Creator3D	*/
#define FB_ACCEL_SUN_CGSIX	12	/* Sun cg6			*/
#define FB_ACCEL_SUN_LEO	13	/* Sun leo/zx			*/
#define FB_ACCEL_IMS_TWINTURBO	14	/* IMS Twin Turbo		*/
#define FB_ACCEL_3DLABS_PERMEDIA2 15	/* 3Dlabs Permedia 2		*/
#define FB_ACCEL_MATROX_MGA2064W 16	/* Matrox MGA2064W (Millenium)	*/
#define FB_ACCEL_MATROX_MGA1064SG 17	/* Matrox MGA1064SG (Mystique)	*/
#define FB_ACCEL_MATROX_MGA2164W 18	/* Matrox MGA2164W (Millenium II) */
#define FB_ACCEL_MATROX_MGA2164W_AGP 19	/* Matrox MGA2164W (Millenium II) */
#define FB_ACCEL_MATROX_MGAG100	20	/* Matrox G100 (Productiva G100) */
#define FB_ACCEL_MATROX_MGAG200	21	/* Matrox G200 (Myst, Mill, ...) */
#define FB_ACCEL_SUN_CG14	22	/* Sun cgfourteen		 */
#define FB_ACCEL_SUN_BWTWO	23	/* Sun bwtwo			*/
#define FB_ACCEL_SUN_CGTHREE	24	/* Sun cgthree			*/
#define FB_ACCEL_SUN_TCX	25	/* Sun tcx			*/
#define FB_ACCEL_MATROX_MGAG400	26	/* Matrox G400			*/
#define FB_ACCEL_NV3		27	/* nVidia RIVA 128              */
#define FB_ACCEL_NV4		28	/* nVidia RIVA TNT		*/
#define FB_ACCEL_NV5		29	/* nVidia RIVA TNT2		*/
#define FB_ACCEL_CT_6555x	30	/* C&T 6555x			*/
#define FB_ACCEL_3DFX_BANSHEE	31	/* 3Dfx Banshee			*/
#define FB_ACCEL_ATI_RAGE128	32	/* ATI Rage128 family		*/
#define FB_ACCEL_IGS_CYBER2000	33	/* CyberPro 2000		*/
#define FB_ACCEL_IGS_CYBER2010	34	/* CyberPro 2010		*/
#define FB_ACCEL_IGS_CYBER5000	35	/* CyberPro 5000		*/
#define FB_ACCEL_SIS_GLAMOUR    36	/* SiS 300/630/540              */
#define FB_ACCEL_3DLABS_PERMEDIA3 37	/* 3Dlabs Permedia 3		*/
#define FB_ACCEL_ATI_RADEON	38	/* ATI Radeon family		*/
#define FB_ACCEL_I810           39      /* Intel 810/815                */
#define FB_ACCEL_SIS_GLAMOUR_2  40	/* SiS 315, 650, 740		*/
#define FB_ACCEL_SIS_XABRE      41	/* SiS 330 ("Xabre")		*/

#define FB_ACCEL_NEOMAGIC_NM2070 90	/* NeoMagic NM2070              */
#define FB_ACCEL_NEOMAGIC_NM2090 91	/* NeoMagic NM2090              */
#define FB_ACCEL_NEOMAGIC_NM2093 92	/* NeoMagic NM2093              */
#define FB_ACCEL_NEOMAGIC_NM2097 93	/* NeoMagic NM2097              */
#define FB_ACCEL_NEOMAGIC_NM2160 94	/* NeoMagic NM2160              */
#define FB_ACCEL_NEOMAGIC_NM2200 95	/* NeoMagic NM2200              */
#define FB_ACCEL_NEOMAGIC_NM2230 96	/* NeoMagic NM2230              */
#define FB_ACCEL_NEOMAGIC_NM2360 97	/* NeoMagic NM2360              */
#define FB_ACCEL_NEOMAGIC_NM2380 98	/* NeoMagic NM2380              */


struct fb_fix_screeninfo {
	char id[16];			/* identification string eg "TT Builtin" */
	unsigned long smem_start;	/* Start of frame buffer mem */
					/* (physical address) */
	__u32 smem_len;			/* Length of frame buffer mem */
	__u32 type;			/* see FB_TYPE_*		*/
	__u32 type_aux;			/* Interleave for interleaved Planes */
	__u32 visual;			/* see FB_VISUAL_*		*/ 
	__u16 xpanstep;			/* zero if no hardware panning  */
	__u16 ypanstep;			/* zero if no hardware panning  */
	__u16 ywrapstep;		/* zero if no hardware ywrap    */
	__u32 line_length;		/* length of a line in bytes    */
	unsigned long mmio_start;	/* Start of Memory Mapped I/O   */
					/* (physical address) */
	__u32 mmio_len;			/* Length of Memory Mapped I/O  */
	__u32 accel;			/* Indicate to driver which	*/
					/*  specific chip/card we have	*/
	__u16 reserved[3];		/* Reserved for future compatibility */
};

/* Interpretation of offset for color fields: All offsets are from the right,
 * inside a "pixel" value, which is exactly 'bits_per_pixel' wide (means: you
 * can use the offset as right argument to <<). A pixel afterwards is a bit
 * stream and is written to video memory as that unmodified. This implies
 * big-endian byte order if bits_per_pixel is greater than 8.
 */
struct fb_bitfield {
	__u32 offset;			/* beginning of bitfield	*/
	__u32 length;			/* length of bitfield		*/
	__u32 msb_right;		/* != 0 : Most significant bit is */ 
					/* right */ 
};

#define FB_NONSTD_HAM		1	/* Hold-And-Modify (HAM)        */

#define FB_ACTIVATE_NOW		0	/* set values immediately (or vbl)*/
#define FB_ACTIVATE_NXTOPEN	1	/* activate on next open	*/
#define FB_ACTIVATE_TEST	2	/* don't set, round up impossible */
#define FB_ACTIVATE_MASK       15
					/* values			*/
#define FB_ACTIVATE_VBL	       16	/* activate values on next vbl  */
#define FB_CHANGE_CMAP_VBL     32	/* change colormap on vbl	*/
#define FB_ACTIVATE_ALL	       64	/* change all VCs on this fb	*/
#define FB_ACTIVATE_FORCE     128	/* force apply even when no change*/

#define FB_ACCELF_TEXT		1	/* (OBSOLETE) see fb_info.flags and vc_mode */

#define FB_SYNC_HOR_HIGH_ACT	1	/* horizontal sync high active	*/
#define FB_SYNC_VERT_HIGH_ACT	2	/* vertical sync high active	*/
#define FB_SYNC_EXT		4	/* external sync		*/
#define FB_SYNC_COMP_HIGH_ACT	8	/* composite sync high active   */
#define FB_SYNC_BROADCAST	16	/* broadcast video timings      */
					/* vtotal = 144d/288n/576i => PAL  */
					/* vtotal = 121d/242n/484i => NTSC */
#define FB_SYNC_ON_GREEN	32	/* sync on green */

#define FB_VMODE_NONINTERLACED  0	/* non interlaced */
#define FB_VMODE_INTERLACED	1	/* interlaced	*/
#define FB_VMODE_DOUBLE		2	/* double scan */
#define FB_VMODE_MASK		255

#define FB_VMODE_YWRAP		256	/* ywrap instead of panning     */
#define FB_VMODE_SMOOTH_XPAN	512	/* smooth xpan possible (internally used) */
#define FB_VMODE_CONUPDATE	512	/* don't update x/yoffset	*/

#define PICOS2KHZ(a) (1000000000UL/(a))
#define KHZ2PICOS(a) (1000000000UL/(a))

struct fb_var_screeninfo {
	__u32 xres;			/* visible resolution		*/
	__u32 yres;
	__u32 xres_virtual;		/* virtual resolution		*/
	__u32 yres_virtual;
	__u32 xoffset;			/* offset from virtual to visible */
	__u32 yoffset;			/* resolution			*/

	__u32 bits_per_pixel;		/* guess what			*/
	__u32 grayscale;		/* != 0 Graylevels instead of colors */

	struct fb_bitfield red;		/* bitfield in fb mem if true color, */
	struct fb_bitfield green;	/* else only length is significant */
	struct fb_bitfield blue;
	struct fb_bitfield transp;	/* transparency			*/	

	__u32 nonstd;			/* != 0 Non standard pixel format */

	__u32 activate;			/* see FB_ACTIVATE_*		*/

	__u32 height;			/* height of picture in mm    */
	__u32 width;			/* width of picture in mm     */

	__u32 accel_flags;		/* (OBSOLETE) see fb_info.flags */

	/* Timing: All values in pixclocks, except pixclock (of course) */
	__u32 pixclock;			/* pixel clock in ps (pico seconds) */
	__u32 left_margin;		/* time from sync to picture	*/
	__u32 right_margin;		/* time from picture to sync	*/
	__u32 upper_margin;		/* time from sync to picture	*/
	__u32 lower_margin;
	__u32 hsync_len;		/* length of horizontal sync	*/
	__u32 vsync_len;		/* length of vertical sync	*/
	__u32 sync;			/* see FB_SYNC_*		*/
	__u32 vmode;			/* see FB_VMODE_*		*/
	__u32 rotate;			/* angle we rotate counter clockwise */
	__u32 reserved[5];		/* Reserved for future compatibility */
};

struct fb_cmap {
	__u32 start;			/* First entry	*/
	__u32 len;			/* Number of entries */
	__u16 *red;			/* Red values	*/
	__u16 *green;
	__u16 *blue;
	__u16 *transp;			/* transparency, can be NULL */
};

struct fb_con2fbmap {
	__u32 console;
	__u32 framebuffer;
};

/* VESA Blanking Levels */
#define VESA_NO_BLANKING        0
#define VESA_VSYNC_SUSPEND      1
#define VESA_HSYNC_SUSPEND      2
#define VESA_POWERDOWN          3

/* Definitions below are used in the parsed monitor specs */
#define FB_DPMS_ACTIVE_OFF	1
#define FB_DPMS_SUSPEND		2
#define FB_DPMS_STANDBY		4

#define FB_DISP_DDI		1
#define FB_DISP_ANA_700_300	2
#define FB_DISP_ANA_714_286	4
#define FB_DISP_ANA_1000_400	8
#define FB_DISP_ANA_700_000	16

#define FB_DISP_MONO		32
#define FB_DISP_RGB		64
#define FB_DISP_MULTI		128
#define FB_DISP_UNKNOWN		256

#define FB_SIGNAL_NONE		0
#define FB_SIGNAL_BLANK_BLANK	1
#define FB_SIGNAL_SEPARATE	2
#define FB_SIGNAL_COMPOSITE	4
#define FB_SIGNAL_SYNC_ON_GREEN	8
#define FB_SIGNAL_SERRATION_ON	16

#define FB_MISC_PRIM_COLOR	1
#define FB_MISC_1ST_DETAIL	2	/* First Detailed Timing is preferred */

struct fb_chroma {
	__u32 redx;	/* in fraction of 1024 */
	__u32 greenx;
	__u32 bluex;
	__u32 whitex;
	__u32 redy;
	__u32 greeny;
	__u32 bluey;
	__u32 whitey;
};

struct fb_monspecs {
	struct fb_chroma chroma;
	struct fb_videomode *modedb;	/* mode database */
	__u8  manufacturer[4];		/* Manufacturer */
	__u8  monitor[14];		/* Monitor String */
	__u8  serial_no[14];		/* Serial Number */
	__u8  ascii[14];		/* ? */
	__u32 modedb_len;		/* mode database length */
	__u32 model;			/* Monitor Model */
	__u32 serial;			/* Serial Number - Integer */
	__u32 year;			/* Year manufactured */
	__u32 week;			/* Week Manufactured */
	__u32 hfmin;			/* hfreq lower limit (Hz) */
	__u32 hfmax;			/* hfreq upper limit (Hz) */
	__u32 dclkmin;			/* pixelclock lower limit (Hz) */
	__u32 dclkmax;			/* pixelclock upper limit (Hz) */
	__u16 input;			/* display type - see FB_DISP_* */
	__u16 dpms;			/* DPMS support - see FB_DPMS_ */
	__u16 signal;			/* Signal Type - see FB_SIGNAL_* */
	__u16 vfmin;			/* vfreq lower limit (Hz) */
	__u16 vfmax;			/* vfreq upper limit (Hz) */
	__u16 gamma;			/* Gamma - in fractions of 100 */
	__u16 gtf	: 1;		/* supports GTF */
	__u16 misc;			/* Misc flags - see FB_MISC_* */
	__u8  version;			/* EDID version... */
	__u8  revision;			/* ...and revision */
	__u8  max_x;			/* Maximum horizontal size (cm) */
	__u8  max_y;			/* Maximum vertical size (cm) */
};

#define FB_VBLANK_VBLANKING	0x001	/* currently in a vertical blank */
#define FB_VBLANK_HBLANKING	0x002	/* currently in a horizontal blank */
#define FB_VBLANK_HAVE_VBLANK	0x004	/* vertical blanks can be detected */
#define FB_VBLANK_HAVE_HBLANK	0x008	/* horizontal blanks can be detected */
#define FB_VBLANK_HAVE_COUNT	0x010	/* global retrace counter is available */
#define FB_VBLANK_HAVE_VCOUNT	0x020	/* the vcount field is valid */
#define FB_VBLANK_HAVE_HCOUNT	0x040	/* the hcount field is valid */
#define FB_VBLANK_VSYNCING	0x080	/* currently in a vsync */
#define FB_VBLANK_HAVE_VSYNC	0x100	/* verical syncs can be detected */

struct fb_vblank {
	__u32 flags;			/* FB_VBLANK flags */
	__u32 count;			/* counter of retraces since boot */
	__u32 vcount;			/* current scanline position */
	__u32 hcount;			/* current scandot position */
	__u32 reserved[4];		/* reserved for future compatibility */
};

/* Internal HW accel */
#define ROP_COPY 0
#define ROP_XOR  1

struct fb_copyarea {
	__u32 dx;
	__u32 dy;
	__u32 width;
	__u32 height;
	__u32 sx;
	__u32 sy;
};

struct fb_fillrect {
	__u32 dx;	/* screen-relative */
	__u32 dy;
	__u32 width;
	__u32 height;
	__u32 color;
	__u32 rop;
};

struct fb_image {
	__u32 dx;		/* Where to place image */
	__u32 dy;
	__u32 width;		/* Size of image */
	__u32 height;
	__u32 fg_color;		/* Only used when a mono bitmap */
	__u32 bg_color;
	__u8  depth;		/* Depth of the image */
	const char *data;	/* Pointer to image data */
	struct fb_cmap cmap;	/* color map info */
};

/*
 * hardware cursor control
 */

#define FB_CUR_SETCUR   0x01
#define FB_CUR_SETPOS   0x02
#define FB_CUR_SETHOT   0x04
#define FB_CUR_SETCMAP  0x08
#define FB_CUR_SETSHAPE 0x10
#define FB_CUR_SETSIZE	0x20
#define FB_CUR_SETALL   0xFF

struct fbcurpos {
	__u16 x, y;
};

struct fb_cursor {
	__u16 set;		/* what to set */
	__u16 enable;		/* cursor on/off */
	__u16 rop;		/* bitop operation */
	const char *mask;	/* cursor mask bits */
	struct fbcurpos hot;	/* cursor hot spot */
	struct fb_image	image;	/* Cursor image */
};

#ifdef __KERNEL__

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/tty.h>
#include <linux/device.h>
#include <linux/workqueue.h>
#include <linux/devfs_fs_kernel.h>
#include <linux/notifier.h>
#include <asm/io.h>

struct vm_area_struct;
struct fb_info;
struct device;
struct file;

struct fb_cmap_user {
	__u32 start;			/* First entry	*/
	__u32 len;			/* Number of entries */
	__u16 __user *red;		/* Red values	*/
	__u16 __user *green;
	__u16 __user *blue;
	__u16 __user *transp;		/* transparency, can be NULL */
};

struct fb_image_user {
	__u32 dx;			/* Where to place image */
	__u32 dy;
	__u32 width;			/* Size of image */
	__u32 height;
	__u32 fg_color;			/* Only used when a mono bitmap */
	__u32 bg_color;
	__u8  depth;			/* Depth of the image */
	const char __user *data;	/* Pointer to image data */
	struct fb_cmap_user cmap;	/* color map info */
};

struct fb_cursor_user {
	__u16 set;			/* what to set */
	__u16 enable;			/* cursor on/off */
	__u16 rop;			/* bitop operation */
	const char __user *mask;	/* cursor mask bits */
	struct fbcurpos hot;		/* cursor hot spot */
	struct fb_image_user image;	/* Cursor image */
};

/*
 * Register/unregister for framebuffer events
 */

/*	The resolution of the passed in fb_info about to change */ 
#define FB_EVENT_MODE_CHANGE		0x01
/*	The display on this fb_info is beeing suspended, no access to the
 *	framebuffer is allowed any more after that call returns
 */
#define FB_EVENT_SUSPEND		0x02
/*	The display on this fb_info was resumed, you can restore the display
 *	if you own it
 */
#define FB_EVENT_RESUME			0x03

extern int fb_register_client(struct notifier_block *nb);
extern int fb_unregister_client(struct notifier_block *nb);

/*
 * Pixmap structure definition
 *
 * The purpose of this structure is to translate data
 * from the hardware independent format of fbdev to what
 * format the hardware needs.
 */

#define FB_PIXMAP_DEFAULT 1     /* used internally by fbcon */
#define FB_PIXMAP_SYSTEM  2     /* memory is in system RAM  */
#define FB_PIXMAP_IO      4     /* memory is iomapped       */
#define FB_PIXMAP_SYNC    256   /* set if GPU can DMA       */

struct fb_pixmap {
	u8  *addr;		/* pointer to memory			*/
	u32 size;		/* size of buffer in bytes		*/
	u32 offset;		/* current offset to buffer		*/
	u32 buf_align;		/* byte alignment of each bitmap	*/
	u32 scan_align;		/* alignment per scanline		*/
	u32 access_align;	/* alignment per read/write		*/
	u32 flags;		/* see FB_PIXMAP_*			*/
	/* access methods */
	void (*outbuf)(struct fb_info *info, u8 *addr, u8 *src, unsigned int size);
	u8   (*inbuf) (struct fb_info *info, u8 *addr);
};


/*
 * Frame buffer operations
 *
 * LOCKING NOTE: those functions must _ALL_ be called with the console
 * semaphore held, this is the only suitable locking mecanism we have
 * in 2.6. Some may be called at interrupt time at this point though.
 */

struct fb_ops {
	/* open/release and usage marking */
	struct module *owner;
	int (*fb_open)(struct fb_info *info, int user);
	int (*fb_release)(struct fb_info *info, int user);

	/* For framebuffers with strange non linear layouts or that do not
	 * work with normal memory mapped access
	 */
	ssize_t (*fb_read)(struct file *file, char __user *buf, size_t count, loff_t *ppos);
	ssize_t (*fb_write)(struct file *file, const char __user *buf, size_t count, loff_t *ppos);

	/* checks var and eventually tweaks it to something supported,
	 * DO NOT MODIFY PAR */
	int (*fb_check_var)(struct fb_var_screeninfo *var, struct fb_info *info);

	/* set the video mode according to info->var */
	int (*fb_set_par)(struct fb_info *info);

	/* set color register */
	int (*fb_setcolreg)(unsigned regno, unsigned red, unsigned green,
			    unsigned blue, unsigned transp, struct fb_info *info);

	/* blank display */
	int (*fb_blank)(int blank, struct fb_info *info);

	/* pan display */
	int (*fb_pan_display)(struct fb_var_screeninfo *var, struct fb_info *info);

	/* Draws a rectangle */
	void (*fb_fillrect) (struct fb_info *info, const struct fb_fillrect *rect);
	/* Copy data from area to another */
	void (*fb_copyarea) (struct fb_info *info, const struct fb_copyarea *region);
	/* Draws a image to the display */
	void (*fb_imageblit) (struct fb_info *info, const struct fb_image *image);

	/* Draws cursor */
	int (*fb_cursor) (struct fb_info *info, struct fb_cursor *cursor);

	/* Rotates the display */
	void (*fb_rotate)(struct fb_info *info, int angle);

	/* wait for blit idle, optional */
	int (*fb_sync)(struct fb_info *info);

	/* perform fb specific ioctl (optional) */
	int (*fb_ioctl)(struct inode *inode, struct file *file, unsigned int cmd,
			unsigned long arg, struct fb_info *info);

	/* perform fb specific mmap */
	int (*fb_mmap)(struct fb_info *info, struct file *file, struct vm_area_struct *vma);
};

/* FBINFO_* = fb_info.flags bit flags */
#define FBINFO_MODULE		0x0001	/* Low-level driver is a module */
#define FBINFO_HWACCEL_DISABLED	0x0002
	/* When FBINFO_HWACCEL_DISABLED is set:
	 *  Hardware acceleration is turned off.  Software implementations
	 *  of required functions (copyarea(), fillrect(), and imageblit())
	 *  takes over; acceleration engine should be in a quiescent state */

/* hints */
#define FBINFO_PARTIAL_PAN_OK	0x0040 /* otw use pan only for double-buffering */
#define FBINFO_READS_FAST	0x0080 /* soft-copy faster than rendering */

/* hardware supported ops */
/*  semantics: when a bit is set, it indicates that the operation is
 *   accelerated by hardware.
 *  required functions will still work even if the bit is not set.
 *  optional functions may not even exist if the flag bit is not set.
 */
#define FBINFO_HWACCEL_NONE		0x0000
#define FBINFO_HWACCEL_COPYAREA		0x0100 /* required */
#define FBINFO_HWACCEL_FILLRECT		0x0200 /* required */
#define FBINFO_HWACCEL_IMAGEBLIT	0x0400 /* required */
#define FBINFO_HWACCEL_ROTATE		0x0800 /* optional */
#define FBINFO_HWACCEL_XPAN		0x1000 /* optional */
#define FBINFO_HWACCEL_YPAN		0x2000 /* optional */
#define FBINFO_HWACCEL_YWRAP		0x4000 /* optional */

#define FBINFO_MISC_MODECHANGEUSER     0x10000 /* mode change request
						  from userspace */
#define FBINFO_MISC_MODESWITCH         0x20000 /* mode switch */
#define FBINFO_MISC_MODESWITCHLATE     0x40000 /* init hardware later */

struct fb_info {
	int node;
	int flags;
	struct fb_var_screeninfo var;	/* Current var */
	struct fb_fix_screeninfo fix;	/* Current fix */
	struct fb_monspecs monspecs;	/* Current Monitor specs */
	struct fb_cursor cursor;	/* Current cursor */	
	struct work_struct queue;	/* Framebuffer event queue */
	struct timer_list cursor_timer; /* Cursor timer */
	struct fb_pixmap pixmap;	/* Image hardware mapper */
	struct fb_pixmap sprite;	/* Cursor hardware mapper */
	struct fb_cmap cmap;		/* Current cmap */
	struct fb_ops *fbops;
	char *screen_base;		/* Virtual address */
	int currcon;			/* Current VC. */
	void *pseudo_palette;		/* Fake palette of 16 colors */ 
#define FBINFO_STATE_RUNNING	0
#define FBINFO_STATE_SUSPENDED	1
	u32 state;			/* Hardware state i.e suspend */

	/* From here on everything is device dependent */
	void *par;	
};

#ifdef MODULE
#define FBINFO_DEFAULT	FBINFO_MODULE
#else
#define FBINFO_DEFAULT	0
#endif

// This will go away
#define FBINFO_FLAG_MODULE	FBINFO_MODULE
#define FBINFO_FLAG_DEFAULT	FBINFO_DEFAULT

/* This will go away
 * fbset currently hacks in FB_ACCELF_TEXT into var.accel_flags
 * when it wants to turn the acceleration engine on.  This is
 * really a separate operation, and should be modified via sysfs.
 *  But for now, we leave it broken with the following define
 */
#define STUPID_ACCELF_TEXT_SHIT

// This will go away
#if defined(__sparc__)

/* We map all of our framebuffers such that big-endian accesses
 * are what we want, so the following is sufficient.
 */

// This will go away
#define fb_readb sbus_readb
#define fb_readw sbus_readw
#define fb_readl sbus_readl
#define fb_readq sbus_readq
#define fb_writeb sbus_writeb
#define fb_writew sbus_writew
#define fb_writel sbus_writel
#define fb_writeq sbus_writeq
#define fb_memset sbus_memset_io

#elif defined(__i386__) || defined(__alpha__) || defined(__x86_64__) || defined(__hppa__) || (defined(__sh__) && !defined(__SH5__)) || defined(__powerpc__)

#define fb_readb __raw_readb
#define fb_readw __raw_readw
#define fb_readl __raw_readl
#define fb_readq __raw_readq
#define fb_writeb __raw_writeb
#define fb_writew __raw_writew
#define fb_writel __raw_writel
#define fb_writeq __raw_writeq
#define fb_memset memset_io

#else

#define fb_readb(addr) (*(volatile u8 *) (addr))
#define fb_readw(addr) (*(volatile u16 *) (addr))
#define fb_readl(addr) (*(volatile u32 *) (addr))
#define fb_readq(addr) (*(volatile u64 *) (addr))
#define fb_writeb(b,addr) (*(volatile u8 *) (addr) = (b))
#define fb_writew(b,addr) (*(volatile u16 *) (addr) = (b))
#define fb_writel(b,addr) (*(volatile u32 *) (addr) = (b))
#define fb_writeq(b,addr) (*(volatile u64 *) (addr) = (b))
#define fb_memset memset

#endif

    /*
     *  `Generic' versions of the frame buffer device operations
     */

extern int fb_set_var(struct fb_info *info, struct fb_var_screeninfo *var); 
extern int fb_pan_display(struct fb_info *info, struct fb_var_screeninfo *var); 
extern int fb_blank(struct fb_info *info, int blank);
extern int soft_cursor(struct fb_info *info, struct fb_cursor *cursor);
extern void cfb_fillrect(struct fb_info *info, const struct fb_fillrect *rect); 
extern void cfb_copyarea(struct fb_info *info, const struct fb_copyarea *area); 
extern void cfb_imageblit(struct fb_info *info, const struct fb_image *image);

/* drivers/video/fbmem.c */
extern int register_framebuffer(struct fb_info *fb_info);
extern int unregister_framebuffer(struct fb_info *fb_info);
extern int fb_prepare_logo(struct fb_info *fb_info);
extern int fb_show_logo(struct fb_info *fb_info);
extern char* fb_get_buffer_offset(struct fb_info *info, struct fb_pixmap *buf, u32 size);
extern void fb_iomove_buf_unaligned(struct fb_info *info, struct fb_pixmap *buf,
				u8 *dst, u32 d_pitch, u8 *src, u32 idx,
				u32 height, u32 shift_high, u32 shift_low, u32 mod);
extern void fb_iomove_buf_aligned(struct fb_info *info, struct fb_pixmap *buf,
				u8 *dst, u32 d_pitch, u8 *src, u32 s_pitch,
				u32 height);
extern void fb_sysmove_buf_unaligned(struct fb_info *info, struct fb_pixmap *buf,
				u8 *dst, u32 d_pitch, u8 *src, u32 idx,
				u32 height, u32 shift_high, u32 shift_low, u32 mod);
extern void fb_sysmove_buf_aligned(struct fb_info *info, struct fb_pixmap *buf,
				u8 *dst, u32 d_pitch, u8 *src, u32 s_pitch,
				u32 height);
extern void fb_load_cursor_image(struct fb_info *);
extern void fb_set_suspend(struct fb_info *info, int state);

extern struct fb_info *registered_fb[FB_MAX];
extern int num_registered_fb;

/* drivers/video/fbsysfs.c */
extern struct fb_info *framebuffer_alloc(size_t size, struct device *dev);
extern void framebuffer_release(struct fb_info *info);

/* drivers/video/fbmon.c */
#define FB_MAXTIMINGS		0
#define FB_VSYNCTIMINGS		1
#define FB_HSYNCTIMINGS		2
#define FB_DCLKTIMINGS		3
#define FB_IGNOREMON		0x100

#define FB_MODE_IS_UNKNOWN	0
#define FB_MODE_IS_DETAILED	1
#define FB_MODE_IS_STANDARD	2
#define FB_MODE_IS_VESA		4
#define FB_MODE_IS_CALCULATED	8
#define FB_MODE_IS_FIRST	16

extern int fbmon_valid_timings(u_int pixclock, u_int htotal, u_int vtotal,
			       const struct fb_info *fb_info);
extern int fbmon_dpms(const struct fb_info *fb_info);
extern int fb_get_mode(int flags, u32 val, struct fb_var_screeninfo *var,
		       struct fb_info *info);
extern int fb_validate_mode(const struct fb_var_screeninfo *var,
			    struct fb_info *info);
extern int fb_parse_edid(unsigned char *edid, struct fb_var_screeninfo *var);
extern int fb_get_monitor_limits(unsigned char *edid, struct fb_monspecs *specs);
extern void fb_edid_to_monspecs(unsigned char *edid, struct fb_monspecs *specs);
extern int fb_get_monitor_limits(unsigned char *edid, struct fb_monspecs *specs);
extern struct fb_videomode *fb_create_modedb(unsigned char *edid, int *dbsize);
extern void fb_destroy_modedb(struct fb_videomode *modedb);

/* drivers/video/modedb.c */
#define VESA_MODEDB_SIZE 34
extern const struct fb_videomode vesa_modes[];

/* drivers/video/fbcmap.c */
extern int fb_alloc_cmap(struct fb_cmap *cmap, int len, int transp);
extern void fb_dealloc_cmap(struct fb_cmap *cmap);
extern int fb_copy_cmap(struct fb_cmap *from, struct fb_cmap *to);
extern int fb_cmap_to_user(struct fb_cmap *from, struct fb_cmap_user *to);
extern int fb_set_cmap(struct fb_cmap *cmap, struct fb_info *fb_info);
extern int fb_set_user_cmap(struct fb_cmap_user *cmap, struct fb_info *fb_info);
extern struct fb_cmap *fb_default_cmap(int len);
extern void fb_invert_cmaps(void);

struct fb_videomode {
	const char *name;	/* optional */
	u32 refresh;		/* optional */
	u32 xres;
	u32 yres;
	u32 pixclock;
	u32 left_margin;
	u32 right_margin;
	u32 upper_margin;
	u32 lower_margin;
	u32 hsync_len;
	u32 vsync_len;
	u32 sync;
	u32 vmode;
	u32 flag;
};

extern int fb_find_mode(struct fb_var_screeninfo *var,
			struct fb_info *info, const char *mode_option,
			const struct fb_videomode *db,
			unsigned int dbsize,
			const struct fb_videomode *default_mode,
			unsigned int default_bpp);

#endif /* __KERNEL__ */

#endif /* _LINUX_FB_H */
