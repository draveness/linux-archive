/* r128_drv.h -- Private header for r128 driver -*- linux-c -*-
 * Created: Mon Dec 13 09:51:11 1999 by faith@precisioninsight.com
 *
 * Copyright 1999 Precision Insight, Inc., Cedar Park, Texas.
 * Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Rickard E. (Rik) Faith <faith@valinux.com>
 *   Kevin E. Martin <martin@valinux.com>
 *   Gareth Hughes <gareth@valinux.com>
 *
 */

#ifndef __R128_DRV_H__
#define __R128_DRV_H__

typedef struct drm_r128_freelist {
   	unsigned int age;
   	drm_buf_t *buf;
   	struct drm_r128_freelist *next;
   	struct drm_r128_freelist *prev;
} drm_r128_freelist_t;

typedef struct drm_r128_ring_buffer {
	u32 *start;
	u32 *end;
	int size;
	int size_l2qw;

	volatile u32 *head;
	u32 tail;
	u32 tail_mask;
	int space;
} drm_r128_ring_buffer_t;

typedef struct drm_r128_private {
	drm_r128_ring_buffer_t ring;
	drm_r128_sarea_t *sarea_priv;

	int cce_mode;
	int cce_fifo_size;
	int cce_secure;
	int cce_running;

   	drm_r128_freelist_t *head;
   	drm_r128_freelist_t *tail;

	int usec_timeout;
	int is_pci;

	atomic_t idle_count;

	unsigned int fb_bpp;
	unsigned int front_offset;
	unsigned int front_pitch;
	unsigned int back_offset;
	unsigned int back_pitch;

	unsigned int depth_bpp;
	unsigned int depth_offset;
	unsigned int depth_pitch;
	unsigned int span_offset;

	u32 front_pitch_offset_c;
	u32 back_pitch_offset_c;
	u32 depth_pitch_offset_c;
	u32 span_pitch_offset_c;

	drm_map_t *sarea;
	drm_map_t *fb;
	drm_map_t *mmio;
	drm_map_t *cce_ring;
	drm_map_t *ring_rptr;
	drm_map_t *buffers;
	drm_map_t *agp_textures;
} drm_r128_private_t;

typedef struct drm_r128_buf_priv {
	u32 age;
	int prim;
	int discard;
	int dispatched;
   	drm_r128_freelist_t *list_entry;
} drm_r128_buf_priv_t;

				/* r128_drv.c */
extern int  r128_version( struct inode *inode, struct file *filp,
			  unsigned int cmd, unsigned long arg );
extern int  r128_open( struct inode *inode, struct file *filp );
extern int  r128_release( struct inode *inode, struct file *filp );
extern int  r128_ioctl( struct inode *inode, struct file *filp,
			unsigned int cmd, unsigned long arg );
extern int  r128_lock( struct inode *inode, struct file *filp,
		       unsigned int cmd, unsigned long arg );
extern int  r128_unlock( struct inode *inode, struct file *filp,
			 unsigned int cmd, unsigned long arg );

				/* r128_cce.c */
extern int r128_cce_init( struct inode *inode, struct file *filp,
			  unsigned int cmd, unsigned long arg );
extern int r128_cce_start( struct inode *inode, struct file *filp,
			   unsigned int cmd, unsigned long arg );
extern int r128_cce_stop( struct inode *inode, struct file *filp,
			  unsigned int cmd, unsigned long arg );
extern int r128_cce_reset( struct inode *inode, struct file *filp,
			   unsigned int cmd, unsigned long arg );
extern int r128_cce_idle( struct inode *inode, struct file *filp,
			  unsigned int cmd, unsigned long arg );
extern int r128_engine_reset( struct inode *inode, struct file *filp,
			      unsigned int cmd, unsigned long arg );
extern int r128_cce_packet( struct inode *inode, struct file *filp,
			    unsigned int cmd, unsigned long arg );
extern int r128_cce_buffers( struct inode *inode, struct file *filp,
			     unsigned int cmd, unsigned long arg );

extern void r128_freelist_reset( drm_device_t *dev );
extern drm_buf_t *r128_freelist_get( drm_device_t *dev );

extern int r128_wait_ring( drm_r128_private_t *dev_priv, int n );
extern void r128_update_ring_snapshot( drm_r128_private_t *dev_priv );

				/* r128_state.c */
extern int r128_cce_clear( struct inode *inode, struct file *filp,
			   unsigned int cmd, unsigned long arg );
extern int r128_cce_swap( struct inode *inode, struct file *filp,
			  unsigned int cmd, unsigned long arg );
extern int r128_cce_vertex( struct inode *inode, struct file *filp,
			    unsigned int cmd, unsigned long arg );
extern int r128_cce_indices( struct inode *inode, struct file *filp,
			     unsigned int cmd, unsigned long arg );
extern int r128_cce_blit( struct inode *inode, struct file *filp,
			  unsigned int cmd, unsigned long arg );
extern int r128_cce_depth( struct inode *inode, struct file *filp,
			   unsigned int cmd, unsigned long arg );
extern int r128_cce_stipple( struct inode *inode, struct file *filp,
			     unsigned int cmd, unsigned long arg );

				/* r128_bufs.c */
extern int r128_addbufs(struct inode *inode, struct file *filp,
			unsigned int cmd, unsigned long arg);
extern int r128_mapbufs(struct inode *inode, struct file *filp,
			unsigned int cmd, unsigned long arg);

				/* r128_context.c */
extern int  r128_resctx(struct inode *inode, struct file *filp,
			unsigned int cmd, unsigned long arg);
extern int  r128_addctx(struct inode *inode, struct file *filp,
		        unsigned int cmd, unsigned long arg);
extern int  r128_modctx(struct inode *inode, struct file *filp,
		        unsigned int cmd, unsigned long arg);
extern int  r128_getctx(struct inode *inode, struct file *filp,
		        unsigned int cmd, unsigned long arg);
extern int  r128_switchctx(struct inode *inode, struct file *filp,
			   unsigned int cmd, unsigned long arg);
extern int  r128_newctx(struct inode *inode, struct file *filp,
			unsigned int cmd, unsigned long arg);
extern int  r128_rmctx(struct inode *inode, struct file *filp,
		       unsigned int cmd, unsigned long arg);

extern int  r128_context_switch(drm_device_t *dev, int old, int new);
extern int  r128_context_switch_complete(drm_device_t *dev, int new);


/* Register definitions, register access macros and drmAddMap constants
 * for Rage 128 kernel driver.
 */

#define R128_AUX_SC_CNTL		0x1660
#	define R128_AUX1_SC_EN			(1 << 0)
#	define R128_AUX1_SC_MODE_OR		(0 << 1)
#	define R128_AUX1_SC_MODE_NAND		(1 << 1)
#	define R128_AUX2_SC_EN			(1 << 2)
#	define R128_AUX2_SC_MODE_OR		(0 << 3)
#	define R128_AUX2_SC_MODE_NAND		(1 << 3)
#	define R128_AUX3_SC_EN			(1 << 4)
#	define R128_AUX3_SC_MODE_OR		(0 << 5)
#	define R128_AUX3_SC_MODE_NAND		(1 << 5)
#define R128_AUX1_SC_LEFT		0x1664
#define R128_AUX1_SC_RIGHT		0x1668
#define R128_AUX1_SC_TOP		0x166c
#define R128_AUX1_SC_BOTTOM		0x1670
#define R128_AUX2_SC_LEFT		0x1674
#define R128_AUX2_SC_RIGHT		0x1678
#define R128_AUX2_SC_TOP		0x167c
#define R128_AUX2_SC_BOTTOM		0x1680
#define R128_AUX3_SC_LEFT		0x1684
#define R128_AUX3_SC_RIGHT		0x1688
#define R128_AUX3_SC_TOP		0x168c
#define R128_AUX3_SC_BOTTOM		0x1690

#define R128_BRUSH_DATA0		0x1480
#define R128_BUS_CNTL			0x0030
#	define R128_BUS_MASTER_DIS		(1 << 6)

#define R128_CLOCK_CNTL_INDEX		0x0008
#define R128_CLOCK_CNTL_DATA		0x000c
#	define R128_PLL_WR_EN			(1 << 7)

#define R128_CONSTANT_COLOR_C		0x1d34

#define R128_DP_GUI_MASTER_CNTL		0x146c
#       define R128_GMC_SRC_PITCH_OFFSET_CNTL	(1    <<  0)
#       define R128_GMC_DST_PITCH_OFFSET_CNTL	(1    <<  1)
#	define R128_GMC_BRUSH_SOLID_COLOR	(13   <<  4)
#	define R128_GMC_BRUSH_NONE		(15   <<  4)
#	define R128_GMC_DST_16BPP		(4    <<  8)
#	define R128_GMC_DST_24BPP		(5    <<  8)
#	define R128_GMC_DST_32BPP		(6    <<  8)
#       define R128_GMC_DST_DATATYPE_SHIFT	8
#	define R128_GMC_SRC_DATATYPE_COLOR	(3    << 12)
#	define R128_DP_SRC_SOURCE_MEMORY	(2    << 24)
#	define R128_DP_SRC_SOURCE_HOST_DATA	(3    << 24)
#	define R128_GMC_CLR_CMP_CNTL_DIS	(1    << 28)
#	define R128_GMC_AUX_CLIP_DIS		(1    << 29)
#	define R128_GMC_WR_MSK_DIS		(1    << 30)
#	define R128_ROP3_S			0x00cc0000
#	define R128_ROP3_P			0x00f00000
#define R128_DP_WRITE_MASK		0x16cc
#define R128_DST_PITCH_OFFSET_C		0x1c80
#	define R128_DST_TILE			(1 << 31)

#define R128_GEN_RESET_CNTL		0x00f0
#	define R128_SOFT_RESET_GUI		(1 <<  0)

#define R128_GUI_SCRATCH_REG0		0x15e0
#define R128_GUI_SCRATCH_REG1		0x15e4
#define R128_GUI_SCRATCH_REG2		0x15e8
#define R128_GUI_SCRATCH_REG3		0x15ec
#define R128_GUI_SCRATCH_REG4		0x15f0
#define R128_GUI_SCRATCH_REG5		0x15f4

#define R128_GUI_STAT			0x1740
#	define R128_GUI_FIFOCNT_MASK		0x0fff
#	define R128_GUI_ACTIVE			(1 << 31)

#define R128_MCLK_CNTL			0x000f
#	define R128_FORCE_GCP			(1 << 16)
#	define R128_FORCE_PIPE3D_CP		(1 << 17)
#	define R128_FORCE_RCP			(1 << 18)

#define R128_PC_GUI_CTLSTAT		0x1748
#define R128_PC_NGUI_CTLSTAT		0x0184
#	define R128_PC_FLUSH_GUI		(3 << 0)
#	define R128_PC_RI_GUI			(1 << 2)
#	define R128_PC_FLUSH_ALL		0x00ff
#	define R128_PC_BUSY			(1 << 31)

#define R128_PRIM_TEX_CNTL_C		0x1cb0

#define R128_SCALE_3D_CNTL		0x1a00
#define R128_SEC_TEX_CNTL_C		0x1d00
#define R128_SEC_TEXTURE_BORDER_COLOR_C	0x1d3c
#define R128_SETUP_CNTL			0x1bc4
#define R128_STEN_REF_MASK_C		0x1d40

#define R128_TEX_CNTL_C			0x1c9c
#	define R128_TEX_CACHE_FLUSH		(1 << 23)

#define R128_WINDOW_XY_OFFSET		0x1bcc


/* CCE registers
 */
#define R128_PM4_BUFFER_OFFSET		0x0700
#define R128_PM4_BUFFER_CNTL		0x0704
#	define R128_PM4_MASK			(15 << 28)
#	define R128_PM4_NONPM4			(0  << 28)
#	define R128_PM4_192PIO			(1  << 28)
#	define R128_PM4_192BM			(2  << 28)
#	define R128_PM4_128PIO_64INDBM		(3  << 28)
#	define R128_PM4_128BM_64INDBM		(4  << 28)
#	define R128_PM4_64PIO_128INDBM		(5  << 28)
#	define R128_PM4_64BM_128INDBM		(6  << 28)
#	define R128_PM4_64PIO_64VCBM_64INDBM	(7  << 28)
#	define R128_PM4_64BM_64VCBM_64INDBM	(8  << 28)
#	define R128_PM4_64PIO_64VCPIO_64INDPIO	(15 << 28)

#define R128_PM4_BUFFER_WM_CNTL		0x0708
#	define R128_WMA_SHIFT			0
#	define R128_WMB_SHIFT			8
#	define R128_WMC_SHIFT			16
#	define R128_WB_WM_SHIFT			24

#define R128_PM4_BUFFER_DL_RPTR_ADDR	0x070c
#define R128_PM4_BUFFER_DL_RPTR		0x0710
#define R128_PM4_BUFFER_DL_WPTR		0x0714
#	define R128_PM4_BUFFER_DL_DONE		(1 << 31)

#define R128_PM4_VC_FPU_SETUP		0x071c

#define R128_PM4_IW_INDOFF		0x0738
#define R128_PM4_IW_INDSIZE		0x073c

#define R128_PM4_STAT			0x07b8
#	define R128_PM4_FIFOCNT_MASK		0x0fff
#	define R128_PM4_BUSY			(1 << 16)
#	define R128_PM4_GUI_ACTIVE		(1 << 31)

#define R128_PM4_MICROCODE_ADDR		0x07d4
#define R128_PM4_MICROCODE_RADDR	0x07d8
#define R128_PM4_MICROCODE_DATAH	0x07dc
#define R128_PM4_MICROCODE_DATAL	0x07e0

#define R128_PM4_BUFFER_ADDR		0x07f0
#define R128_PM4_MICRO_CNTL		0x07fc
#	define R128_PM4_MICRO_FREERUN		(1 << 30)

#define R128_PM4_FIFO_DATA_EVEN		0x1000
#define R128_PM4_FIFO_DATA_ODD		0x1004


/* CCE command packets
 */
#define R128_CCE_PACKET0		0x00000000
#define R128_CCE_PACKET1		0x40000000
#define R128_CCE_PACKET2		0x80000000
#define R128_CCE_PACKET3		0xC0000000
#	define R128_CNTL_HOSTDATA_BLT		0x00009400
#	define R128_CNTL_PAINT_MULTI		0x00009A00
#	define R128_CNTL_BITBLT_MULTI		0x00009B00
#	define R128_3D_RNDR_GEN_INDX_PRIM	0x00002300

#define R128_CCE_PACKET_MASK		0xC0000000
#define R128_CCE_PACKET_COUNT_MASK	0x3fff0000
#define R128_CCE_PACKET0_REG_MASK	0x000007ff
#define R128_CCE_PACKET1_REG0_MASK	0x000007ff
#define R128_CCE_PACKET1_REG1_MASK	0x003ff800

#define R128_CCE_VC_CNTL_PRIM_TYPE_NONE		0x00000000
#define R128_CCE_VC_CNTL_PRIM_TYPE_POINT	0x00000001
#define R128_CCE_VC_CNTL_PRIM_TYPE_LINE		0x00000002
#define R128_CCE_VC_CNTL_PRIM_TYPE_POLY_LINE	0x00000003
#define R128_CCE_VC_CNTL_PRIM_TYPE_TRI_LIST	0x00000004
#define R128_CCE_VC_CNTL_PRIM_TYPE_TRI_FAN	0x00000005
#define R128_CCE_VC_CNTL_PRIM_TYPE_TRI_STRIP	0x00000006
#define R128_CCE_VC_CNTL_PRIM_TYPE_TRI_TYPE2	0x00000007
#define R128_CCE_VC_CNTL_PRIM_WALK_IND		0x00000010
#define R128_CCE_VC_CNTL_PRIM_WALK_LIST		0x00000020
#define R128_CCE_VC_CNTL_PRIM_WALK_RING		0x00000030
#define R128_CCE_VC_CNTL_NUM_SHIFT		16

#define R128_DATATYPE_CI8		2
#define R128_DATATYPE_ARGB1555		3
#define R128_DATATYPE_RGB565		4
#define R128_DATATYPE_RGB888		5
#define R128_DATATYPE_ARGB8888		6
#define R128_DATATYPE_RGB332		7
#define R128_DATATYPE_RGB8		9
#define R128_DATATYPE_ARGB4444		15

/* Constants */
#define R128_AGP_OFFSET			0x02000000

#define R128_WATERMARK_L		16
#define R128_WATERMARK_M		8
#define R128_WATERMARK_N		8
#define R128_WATERMARK_K		128

#define R128_MAX_USEC_TIMEOUT	100000	/* 100 ms */

#define R128_LAST_FRAME_REG		R128_GUI_SCRATCH_REG0
#define R128_LAST_DISPATCH_REG		R128_GUI_SCRATCH_REG1
#define R128_MAX_VB_AGE			0xffffffff

#define R128_MAX_VB_VERTS		(0xffff)


#define R128_BASE(reg)		((u32)(dev_priv->mmio->handle))
#define R128_ADDR(reg)		(R128_BASE(reg) + reg)

#define R128_DEREF(reg)		*(__volatile__ int *)R128_ADDR(reg)
#define R128_READ(reg)		R128_DEREF(reg)
#define R128_WRITE(reg,val)	do { R128_DEREF(reg) = val; } while (0)

#define R128_DEREF8(reg)	*(__volatile__ char *)R128_ADDR(reg)
#define R128_READ8(reg)		R128_DEREF8(reg)
#define R128_WRITE8(reg,val)	do { R128_DEREF8(reg) = val; } while (0)

#define R128_WRITE_PLL(addr,val)                                              \
do {                                                                          \
	R128_WRITE8(R128_CLOCK_CNTL_INDEX, ((addr) & 0x1f) | R128_PLL_WR_EN); \
	R128_WRITE(R128_CLOCK_CNTL_DATA, (val));                              \
} while (0)

extern int R128_READ_PLL(drm_device_t *dev, int addr);

#define R128CCE0(p,r,n)   ((p) | ((n) << 16) | ((r) >> 2))
#define R128CCE1(p,r1,r2) ((p) | (((r2) >> 2) << 11) | ((r1) >> 2))
#define R128CCE2(p)       ((p))
#define R128CCE3(p,n)     ((p) | ((n) << 16))




#define CCE_PACKET0( reg, n )		(R128_CCE_PACKET0 |		\
					 ((n) << 16) | ((reg) >> 2))
#define CCE_PACKET1( reg0, reg1 )	(R128_CCE_PACKET1 |		\
					 (((reg1) >> 2) << 11) | ((reg0) >> 2))
#define CCE_PACKET2()			(R128_CCE_PACKET2)
#define CCE_PACKET3( pkt, n )		(R128_CCE_PACKET3 |		\
					 (pkt) | ((n) << 16))


#define r128_flush_write_combine()		mb()


#define R128_VERBOSE	0

#define RING_LOCALS	int write; unsigned int tail_mask; volatile u32 *ring;

#define BEGIN_RING( n ) do {						\
	if ( R128_VERBOSE ) {						\
		DRM_INFO( "BEGIN_RING( %d ) in %s\n",			\
			   n, __FUNCTION__ );				\
	}								\
	if ( dev_priv->ring.space < n * sizeof(u32) ) {			\
		r128_wait_ring( dev_priv, n * sizeof(u32) );		\
	}								\
	dev_priv->ring.space -= n * sizeof(u32);			\
	ring = dev_priv->ring.start;					\
	write = dev_priv->ring.tail;					\
	tail_mask = dev_priv->ring.tail_mask;				\
} while (0)

#define ADVANCE_RING() do {						\
	if ( R128_VERBOSE ) {						\
		DRM_INFO( "ADVANCE_RING() tail=0x%06x wr=0x%06x\n",	\
			  write, dev_priv->ring.tail );			\
	}								\
	r128_flush_write_combine();					\
	dev_priv->ring.tail = write;					\
	R128_WRITE( R128_PM4_BUFFER_DL_WPTR, write );			\
} while (0)

#define OUT_RING( x ) do {						\
	if ( R128_VERBOSE ) {						\
		DRM_INFO( "   OUT_RING( 0x%08x ) at 0x%x\n",		\
			   (unsigned int)(x), write );			\
	}								\
	ring[write++] = x;						\
	write &= tail_mask;						\
} while (0)

#define R128_PERFORMANCE_BOXES	0

#endif /* __R128_DRV_H__ */
