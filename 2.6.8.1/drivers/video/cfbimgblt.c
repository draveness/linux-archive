/*
 *  Generic BitBLT function for frame buffer with packed pixels of any depth.
 *
 *      Copyright (C)  June 1999 James Simmons
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of this archive for
 *  more details.
 *
 * NOTES:
 *
 *    This function copys a image from system memory to video memory. The
 *  image can be a bitmap where each 0 represents the background color and
 *  each 1 represents the foreground color. Great for font handling. It can
 *  also be a color image. This is determined by image_depth. The color image
 *  must be laid out exactly in the same format as the framebuffer. Yes I know
 *  their are cards with hardware that coverts images of various depths to the
 *  framebuffer depth. But not every card has this. All images must be rounded
 *  up to the nearest byte. For example a bitmap 12 bits wide must be two 
 *  bytes width. 
 *
 *  Tony: 
 *  Incorporate mask tables similar to fbcon-cfb*.c in 2.4 API.  This speeds 
 *  up the code significantly.
 *  
 *  Code for depths not multiples of BITS_PER_LONG is still kludgy, which is
 *  still processed a bit at a time.   
 *
 *  Also need to add code to deal with cards endians that are different than
 *  the native cpu endians. I also need to deal with MSB position in the word.
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/fb.h>
#include <asm/types.h>

#define DEBUG

#ifdef DEBUG
#define DPRINTK(fmt, args...) printk(KERN_DEBUG "%s: " fmt,__FUNCTION__,## args)
#else
#define DPRINTK(fmt, args...)
#endif

static u32 cfb_tab8[] = {
#if defined(__BIG_ENDIAN)
    0x00000000,0x000000ff,0x0000ff00,0x0000ffff,
    0x00ff0000,0x00ff00ff,0x00ffff00,0x00ffffff,
    0xff000000,0xff0000ff,0xff00ff00,0xff00ffff,
    0xffff0000,0xffff00ff,0xffffff00,0xffffffff
#elif defined(__LITTLE_ENDIAN)
    0x00000000,0xff000000,0x00ff0000,0xffff0000,
    0x0000ff00,0xff00ff00,0x00ffff00,0xffffff00,
    0x000000ff,0xff0000ff,0x00ff00ff,0xffff00ff,
    0x0000ffff,0xff00ffff,0x00ffffff,0xffffffff
#else
#error FIXME: No endianness??
#endif
};

static u32 cfb_tab16[] = {
#if defined(__BIG_ENDIAN)
    0x00000000, 0x0000ffff, 0xffff0000, 0xffffffff
#elif defined(__LITTLE_ENDIAN)
    0x00000000, 0xffff0000, 0x0000ffff, 0xffffffff
#else
#error FIXME: No endianness??
#endif
};

static u32 cfb_tab32[] = {
	0x00000000, 0xffffffff
};

#define FB_WRITEL fb_writel
#define FB_READL  fb_readl

#if defined (__BIG_ENDIAN)
#define LEFT_POS(bpp)          (32 - bpp)
#define SHIFT_HIGH(val, bits)  ((val) >> (bits))
#define SHIFT_LOW(val, bits)   ((val) << (bits))
#else
#define LEFT_POS(bpp)          (0)
#define SHIFT_HIGH(val, bits)  ((val) << (bits))
#define SHIFT_LOW(val, bits)   ((val) >> (bits))
#endif

static inline void color_imageblit(const struct fb_image *image, 
				   struct fb_info *p, u8 *dst1, 
				   u32 start_index,
				   u32 pitch_index)
{
	/* Draw the penguin */
	u32 *dst, *dst2, color = 0, val, shift;
	int i, n, bpp = p->var.bits_per_pixel;
	u32 null_bits = 32 - bpp;
	u32 *palette = (u32 *) p->pseudo_palette;
	const u8 *src = image->data;

	dst2 = (u32 *) dst1;
	for (i = image->height; i--; ) {
		n = image->width;
		dst = (u32 *) dst1;
		shift = 0;
		val = 0;
		
		if (start_index) {
			u32 start_mask = ~(SHIFT_HIGH(~(u32)0, start_index));
			val = FB_READL(dst) & start_mask;
			shift = start_index;
		}
		while (n--) {
			if (p->fix.visual == FB_VISUAL_TRUECOLOR ||
			    p->fix.visual == FB_VISUAL_DIRECTCOLOR )
				color = palette[*src];
			else
				color = *src;
			color <<= LEFT_POS(bpp);
			val |= SHIFT_HIGH(color, shift);
			if (shift >= null_bits) {
				FB_WRITEL(val, dst++);
	
				val = (shift == null_bits) ? 0 : 
					SHIFT_LOW(color, 32 - shift);
			}
			shift += bpp;
			shift &= (32 - 1);
			src++;
		}
		if (shift) {
			u32 end_mask = SHIFT_HIGH(~(u32)0, shift);

			FB_WRITEL((FB_READL(dst) & end_mask) | val, dst);
		}
		dst1 += p->fix.line_length;
		if (pitch_index) {
			dst2 += p->fix.line_length;
			dst1 = (u8 *)((long)dst2 & ~(sizeof(u32) - 1));

			start_index += pitch_index;
			start_index &= 32 - 1;
		}
	}
}

static inline void slow_imageblit(const struct fb_image *image, struct fb_info *p, 
				  u8 *dst1, u32 fgcolor,
				  u32 bgcolor, 
				  u32 start_index,
				  u32 pitch_index)
{
	u32 shift, color = 0, bpp = p->var.bits_per_pixel;
	u32 *dst, *dst2, val, pitch = p->fix.line_length;
	u32 null_bits = 32 - bpp;
	u32 spitch = (image->width+7)/8;
	const u8 *src = image->data, *s;
	u32 i, j, l;
	
	dst2 = (u32 *) dst1;

	for (i = image->height; i--; ) {
		shift = val = 0;
		l = 8;
		j = image->width;
		dst = (u32 *) dst1;
		s = src;

		/* write leading bits */
		if (start_index) {
			u32 start_mask = ~(SHIFT_HIGH(~(u32)0, start_index));
			val = FB_READL(dst) & start_mask;
			shift = start_index;
		}

		while (j--) {
			l--;
			color = (*s & (1 << l)) ? fgcolor : bgcolor;
			color <<= LEFT_POS(bpp);
			val |= SHIFT_HIGH(color, shift);
			
			/* Did the bitshift spill bits to the next long? */
			if (shift >= null_bits) {
				FB_WRITEL(val, dst++);
				val = (shift == null_bits) ? 0 :
					 SHIFT_LOW(color,32 - shift);
			}
			shift += bpp;
			shift &= (32 - 1);
			if (!l) { l = 8; s++; };
		}

		/* write trailing bits */
 		if (shift) {
			u32 end_mask = SHIFT_HIGH(~(u32)0, shift);

			FB_WRITEL((FB_READL(dst) & end_mask) | val, dst);
		}
		
		dst1 += pitch;
		src += spitch;	
		if (pitch_index) {
			dst2 += pitch;
			dst1 = (u8 *)((long)dst2 & ~(sizeof(u32) - 1));
			start_index += pitch_index;
			start_index &= 32 - 1;
		}
		
	}
}

/*
 * fast_imageblit - optimized monochrome color expansion
 *
 * Only if:  bits_per_pixel == 8, 16, or 32
 *           image->width is divisible by pixel/dword (ppw);
 *           fix->line_legth is divisible by 4;
 *           beginning and end of a scanline is dword aligned
 */
static inline void fast_imageblit(const struct fb_image *image, struct fb_info *p, 
				  u8 *dst1, u32 fgcolor, 
				  u32 bgcolor) 
{
	u32 fgx = fgcolor, bgx = bgcolor, bpp = p->var.bits_per_pixel;
	u32 ppw = 32/bpp, spitch = (image->width + 7)/8;
	u32 bit_mask, end_mask, eorx, shift;
	const char *s = image->data, *src;
	u32 *dst;
	u32 *tab = NULL;
	int i, j, k;
		
	switch (bpp) {
	case 8:
		tab = cfb_tab8;
		break;
	case 16:
		tab = cfb_tab16;
		break;
	case 32:
		tab = cfb_tab32;
		break;
	}

	for (i = ppw-1; i--; ) {
		fgx <<= bpp;
		bgx <<= bpp;
		fgx |= fgcolor;
		bgx |= bgcolor;
	}
	
	bit_mask = (1 << ppw) - 1;
	eorx = fgx ^ bgx;
	k = image->width/ppw;

	for (i = image->height; i--; ) {
		dst = (u32 *) dst1, shift = 8; src = s;
		
		for (j = k; j--; ) {
			shift -= ppw;
			end_mask = tab[(*src >> shift) & bit_mask];
			FB_WRITEL((end_mask & eorx)^bgx, dst++);
			if (!shift) { shift = 8; src++; }		
		}
		dst1 += p->fix.line_length;
		s += spitch;
	}
}	
	
void cfb_imageblit(struct fb_info *p, const struct fb_image *image)
{
	u32 fgcolor, bgcolor, start_index, bitstart, pitch_index = 0;
	u32 bpl = sizeof(u32), bpp = p->var.bits_per_pixel;
	u32 width = image->width, height = image->height; 
	u32 dx = image->dx, dy = image->dy;
	int x2, y2, vxres, vyres;
	u8 *dst1;

	if (p->state != FBINFO_STATE_RUNNING)
		return;

	vxres = p->var.xres_virtual;
	vyres = p->var.yres_virtual;
	/* 
	 * We could use hardware clipping but on many cards you get around
	 * hardware clipping by writing to framebuffer directly like we are
	 * doing here. 
	 */
	if (image->dx > vxres || image->dy > vyres)
		return;

	x2 = image->dx + image->width;
	y2 = image->dy + image->height;
	dx = image->dx > 0 ? image->dx : 0;
	dy = image->dy > 0 ? image->dy : 0;
	x2 = x2 < vxres ? x2 : vxres;
	y2 = y2 < vyres ? y2 : vyres;
	width  = x2 - dx;
	height = y2 - dy;

	bitstart = (dy * p->fix.line_length * 8) + (dx * bpp);
	start_index = bitstart & (32 - 1);
	pitch_index = (p->fix.line_length & (bpl - 1)) * 8;

	bitstart /= 8;
	bitstart &= ~(bpl - 1);
	dst1 = p->screen_base + bitstart;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	if (image->depth == 1) {
		if (p->fix.visual == FB_VISUAL_TRUECOLOR ||
		    p->fix.visual == FB_VISUAL_DIRECTCOLOR) {
			fgcolor = ((u32*)(p->pseudo_palette))[image->fg_color];
			bgcolor = ((u32*)(p->pseudo_palette))[image->bg_color];
		} else {
			fgcolor = image->fg_color;
			bgcolor = image->bg_color;
		}	
		
		if (32 % bpp == 0 && !start_index && !pitch_index && 
		    ((width & (32/bpp-1)) == 0) &&
		    bpp >= 8 && bpp <= 32) 			
			fast_imageblit(image, p, dst1, fgcolor, bgcolor);
		else 
			slow_imageblit(image, p, dst1, fgcolor, bgcolor,
					start_index, pitch_index);
	} else if (image->depth <= bpp) 
		color_imageblit(image, p, dst1, start_index, pitch_index);
}

EXPORT_SYMBOL(cfb_imageblit);

MODULE_AUTHOR("James Simmons <jsimmons@users.sf.net>");
MODULE_DESCRIPTION("Generic software accelerated imaging drawing");
MODULE_LICENSE("GPL");

