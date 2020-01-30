/* r128_cce.c -- ATI Rage 128 driver -*- linux-c -*-
 * Created: Wed Apr  5 19:24:19 2000 by kevin@precisioninsight.com
 *
 * Copyright 2000 Precision Insight, Inc., Cedar Park, Texas.
 * Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
 * All Rights Reserved.
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
 *   Gareth Hughes <gareth@valinux.com>
 *
 */

#define __NO_VERSION__
#include "drmP.h"
#include "r128_drv.h"

#include <linux/interrupt.h>	/* For task queue support */
#include <linux/delay.h>


/* FIXME: Temporary CCE packet buffer */
u32 r128_cce_buffer[(1 << 14)] __attribute__ ((aligned (32)));

/* CCE microcode (from ATI) */
static u32 r128_cce_microcode[] = {
	0, 276838400, 0, 268449792, 2, 142, 2, 145, 0, 1076765731, 0,
	1617039951, 0, 774592877, 0, 1987540286, 0, 2307490946U, 0,
	599558925, 0, 589505315, 0, 596487092, 0, 589505315, 1,
	11544576, 1, 206848, 1, 311296, 1, 198656, 2, 912273422, 11,
	262144, 0, 0, 1, 33559837, 1, 7438, 1, 14809, 1, 6615, 12, 28,
	1, 6614, 12, 28, 2, 23, 11, 18874368, 0, 16790922, 1, 409600, 9,
	30, 1, 147854772, 16, 420483072, 3, 8192, 0, 10240, 1, 198656,
	1, 15630, 1, 51200, 10, 34858, 9, 42, 1, 33559823, 2, 10276, 1,
	15717, 1, 15718, 2, 43, 1, 15936948, 1, 570480831, 1, 14715071,
	12, 322123831, 1, 33953125, 12, 55, 1, 33559908, 1, 15718, 2,
	46, 4, 2099258, 1, 526336, 1, 442623, 4, 4194365, 1, 509952, 1,
	459007, 3, 0, 12, 92, 2, 46, 12, 176, 1, 15734, 1, 206848, 1,
	18432, 1, 133120, 1, 100670734, 1, 149504, 1, 165888, 1,
	15975928, 1, 1048576, 6, 3145806, 1, 15715, 16, 2150645232U, 2,
	268449859, 2, 10307, 12, 176, 1, 15734, 1, 15735, 1, 15630, 1,
	15631, 1, 5253120, 6, 3145810, 16, 2150645232U, 1, 15864, 2, 82,
	1, 343310, 1, 1064207, 2, 3145813, 1, 15728, 1, 7817, 1, 15729,
	3, 15730, 12, 92, 2, 98, 1, 16168, 1, 16167, 1, 16002, 1, 16008,
	1, 15974, 1, 15975, 1, 15990, 1, 15976, 1, 15977, 1, 15980, 0,
	15981, 1, 10240, 1, 5253120, 1, 15720, 1, 198656, 6, 110, 1,
	180224, 1, 103824738, 2, 112, 2, 3145839, 0, 536885440, 1,
	114880, 14, 125, 12, 206975, 1, 33559995, 12, 198784, 0,
	33570236, 1, 15803, 0, 15804, 3, 294912, 1, 294912, 3, 442370,
	1, 11544576, 0, 811612160, 1, 12593152, 1, 11536384, 1,
	14024704, 7, 310382726, 0, 10240, 1, 14796, 1, 14797, 1, 14793,
	1, 14794, 0, 14795, 1, 268679168, 1, 9437184, 1, 268449792, 1,
	198656, 1, 9452827, 1, 1075854602, 1, 1075854603, 1, 557056, 1,
	114880, 14, 159, 12, 198784, 1, 1109409213, 12, 198783, 1,
	1107312059, 12, 198784, 1, 1109409212, 2, 162, 1, 1075854781, 1,
	1073757627, 1, 1075854780, 1, 540672, 1, 10485760, 6, 3145894,
	16, 274741248, 9, 168, 3, 4194304, 3, 4209949, 0, 0, 0, 256, 14,
	174, 1, 114857, 1, 33560007, 12, 176, 0, 10240, 1, 114858, 1,
	33560018, 1, 114857, 3, 33560007, 1, 16008, 1, 114874, 1,
	33560360, 1, 114875, 1, 33560154, 0, 15963, 0, 256, 0, 4096, 1,
	409611, 9, 188, 0, 10240, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};


#define DO_REMAP(_m) (_m)->handle = drm_ioremap((_m)->offset, (_m)->size)

#define DO_REMAPFREE(_m)                                                    \
	do {                                                                \
		if ((_m)->handle && (_m)->size)                             \
			drm_ioremapfree((_m)->handle, (_m)->size);          \
	} while (0)

#define DO_FIND_MAP(_m, _o)                                                 \
	do {                                                                \
		int _i;                                                     \
		for (_i = 0; _i < dev->map_count; _i++) {                   \
			if (dev->maplist[_i]->offset == _o) {               \
				_m = dev->maplist[_i];                      \
				break;                                      \
			}                                                   \
		}                                                           \
	} while (0)


int R128_READ_PLL(drm_device_t *dev, int addr)
{
	drm_r128_private_t *dev_priv = dev->dev_private;

	R128_WRITE8(R128_CLOCK_CNTL_INDEX, addr & 0x1f);
	return R128_READ(R128_CLOCK_CNTL_DATA);
}

#if 0
static void r128_status( drm_r128_private_t *dev_priv )
{
	printk( "GUI_STAT           = 0x%08x\n",
		(unsigned int)R128_READ( R128_GUI_STAT ) );
	printk( "PM4_STAT           = 0x%08x\n",
		(unsigned int)R128_READ( R128_PM4_STAT ) );
	printk( "PM4_BUFFER_DL_WPTR = 0x%08x\n",
		(unsigned int)R128_READ( R128_PM4_BUFFER_DL_WPTR ) );
	printk( "PM4_BUFFER_DL_RPTR = 0x%08x\n",
		(unsigned int)R128_READ( R128_PM4_BUFFER_DL_RPTR ) );
	printk( "PM4_MICRO_CNTL     = 0x%08x\n",
		(unsigned int)R128_READ( R128_PM4_MICRO_CNTL ) );
	printk( "PM4_BUFFER_CNTL    = 0x%08x\n",
		(unsigned int)R128_READ( R128_PM4_BUFFER_CNTL ) );
}
#endif


/* ================================================================
 * Engine, FIFO control
 */

static int r128_do_pixcache_flush( drm_r128_private_t *dev_priv )
{
	u32 tmp;
	int i;

	tmp = R128_READ( R128_PC_NGUI_CTLSTAT ) | R128_PC_FLUSH_ALL;
	R128_WRITE( R128_PC_NGUI_CTLSTAT, tmp );

	for ( i = 0 ; i < dev_priv->usec_timeout ; i++ ) {
		if ( !(R128_READ( R128_PC_NGUI_CTLSTAT ) & R128_PC_BUSY) ) {
			return 0;
		}
		udelay( 1 );
	}

	DRM_ERROR( "%s failed!\n", __FUNCTION__ );
	return -EBUSY;
}

static int r128_do_wait_for_fifo( drm_r128_private_t *dev_priv, int entries )
{
	int i;

	for ( i = 0 ; i < dev_priv->usec_timeout ; i++ ) {
		int slots = R128_READ( R128_GUI_STAT ) & R128_GUI_FIFOCNT_MASK;
		if ( slots >= entries ) return 0;
		udelay( 1 );
	}

	DRM_ERROR( "%s failed!\n", __FUNCTION__ );
	return -EBUSY;
}

static int r128_do_wait_for_idle( drm_r128_private_t *dev_priv )
{
	int i, ret;

	ret = r128_do_wait_for_fifo( dev_priv, 64 );
	if ( !ret ) return ret;

	for ( i = 0 ; i < dev_priv->usec_timeout ; i++ ) {
		if ( !(R128_READ( R128_GUI_STAT ) & R128_GUI_ACTIVE) ) {
			r128_do_pixcache_flush( dev_priv );
			return 0;
		}
		udelay( 1 );
	}

	DRM_ERROR( "%s failed!\n", __FUNCTION__ );
	return -EBUSY;
}


/* ================================================================
 * CCE control, initialization
 */

/* Load the microcode for the CCE */
static void r128_cce_load_microcode( drm_r128_private_t *dev_priv )
{
	int i;

	r128_do_wait_for_idle( dev_priv );

	R128_WRITE( R128_PM4_MICROCODE_ADDR, 0 );
	for ( i = 0 ; i < 256 ; i++ ) {
		R128_WRITE( R128_PM4_MICROCODE_DATAH,
			    r128_cce_microcode[i * 2] );
		R128_WRITE( R128_PM4_MICROCODE_DATAL,
			    r128_cce_microcode[i * 2 + 1] );
	}
}

/* Flush any pending commands to the CCE.  This should only be used just
 * prior to a wait for idle, as it informs the engine that the command
 * stream is ending.
 */
static void r128_do_cce_flush( drm_r128_private_t *dev_priv )
{
	u32 tmp;

	tmp = R128_READ( R128_PM4_BUFFER_DL_WPTR ) | R128_PM4_BUFFER_DL_DONE;
	R128_WRITE( R128_PM4_BUFFER_DL_WPTR, tmp );
}

/* Wait for the CCE to go idle.
 */
static int r128_do_cce_idle( drm_r128_private_t *dev_priv )
{
	int i;

	for ( i = 0 ; i < dev_priv->usec_timeout ; i++ ) {
		if ( *dev_priv->ring.head == dev_priv->ring.tail ) {
			int pm4stat = R128_READ( R128_PM4_STAT );
			if ( ( (pm4stat & R128_PM4_FIFOCNT_MASK) >=
			       dev_priv->cce_fifo_size ) &&
			     !(pm4stat & (R128_PM4_BUSY |
					  R128_PM4_GUI_ACTIVE)) ) {
				return r128_do_pixcache_flush( dev_priv );
			}
		}
		udelay( 1 );
	}

#if 0
	DRM_ERROR( "failed!\n" );
	r128_status( dev_priv );
#endif
	return -EBUSY;
}

/* Start the Concurrent Command Engine.
 */
static void r128_do_cce_start( drm_r128_private_t *dev_priv )
{
	r128_do_wait_for_idle( dev_priv );

	R128_WRITE( R128_PM4_BUFFER_CNTL,
		    dev_priv->cce_mode | dev_priv->ring.size_l2qw );
	R128_READ( R128_PM4_BUFFER_ADDR ); /* as per the sample code */
	R128_WRITE( R128_PM4_MICRO_CNTL, R128_PM4_MICRO_FREERUN );

	dev_priv->cce_running = 1;
}

/* Reset the Concurrent Command Engine.  This will not flush any pending
 * commangs, so you must wait for the CCE command stream to complete
 * before calling this routine.
 */
static void r128_do_cce_reset( drm_r128_private_t *dev_priv )
{
	R128_WRITE( R128_PM4_BUFFER_DL_WPTR, 0 );
	R128_WRITE( R128_PM4_BUFFER_DL_RPTR, 0 );
	*dev_priv->ring.head = 0;
	dev_priv->ring.tail = 0;
}

/* Stop the Concurrent Command Engine.  This will not flush any pending
 * commangs, so you must flush the command stream and wait for the CCE
 * to go idle before calling this routine.
 */
static void r128_do_cce_stop( drm_r128_private_t *dev_priv )
{
	R128_WRITE( R128_PM4_MICRO_CNTL, 0 );
	R128_WRITE( R128_PM4_BUFFER_CNTL, R128_PM4_NONPM4 );

	dev_priv->cce_running = 0;
}

/* Reset the engine.  This will stop the CCE if it is running.
 */
static int r128_do_engine_reset( drm_device_t *dev )
{
	drm_r128_private_t *dev_priv = dev->dev_private;
	u32 clock_cntl_index, mclk_cntl, gen_reset_cntl;

	r128_do_pixcache_flush( dev_priv );

	clock_cntl_index = R128_READ( R128_CLOCK_CNTL_INDEX );
	mclk_cntl = R128_READ_PLL( dev, R128_MCLK_CNTL );

	R128_WRITE_PLL( R128_MCLK_CNTL,
			mclk_cntl | R128_FORCE_GCP | R128_FORCE_PIPE3D_CP );

	gen_reset_cntl = R128_READ( R128_GEN_RESET_CNTL );

	/* Taken from the sample code - do not change */
	R128_WRITE( R128_GEN_RESET_CNTL,
		    gen_reset_cntl | R128_SOFT_RESET_GUI );
	R128_READ( R128_GEN_RESET_CNTL );
	R128_WRITE( R128_GEN_RESET_CNTL,
		    gen_reset_cntl & ~R128_SOFT_RESET_GUI );
	R128_READ( R128_GEN_RESET_CNTL );

	R128_WRITE_PLL( R128_MCLK_CNTL, mclk_cntl );
	R128_WRITE( R128_CLOCK_CNTL_INDEX, clock_cntl_index );
	R128_WRITE( R128_GEN_RESET_CNTL, gen_reset_cntl );

	/* Reset the CCE ring */
	r128_do_cce_reset( dev_priv );

	/* The CCE is no longer running after an engine reset */
	dev_priv->cce_running = 0;

	/* Reset any pending vertex, indirect buffers */
	r128_freelist_reset( dev );

	return 0;
}

static void r128_cce_init_ring_buffer( drm_device_t *dev )
{
	drm_r128_private_t *dev_priv = dev->dev_private;
	u32 ring_start;
	u32 tmp;

	/* The manual (p. 2) says this address is in "VM space".  This
	 * means it's an offset from the start of AGP space.
	 */
	ring_start = dev_priv->cce_ring->offset - dev->agp->base;
	R128_WRITE( R128_PM4_BUFFER_OFFSET, ring_start | R128_AGP_OFFSET );

	R128_WRITE( R128_PM4_BUFFER_DL_WPTR, 0 );
	R128_WRITE( R128_PM4_BUFFER_DL_RPTR, 0 );

	/* DL_RPTR_ADDR is a physical address in AGP space. */
	*dev_priv->ring.head = 0;
	R128_WRITE( R128_PM4_BUFFER_DL_RPTR_ADDR,
		    dev_priv->ring_rptr->offset );

	/* Set watermark control */
	R128_WRITE( R128_PM4_BUFFER_WM_CNTL,
		    ((R128_WATERMARK_L/4) << R128_WMA_SHIFT)
		    | ((R128_WATERMARK_M/4) << R128_WMB_SHIFT)
		    | ((R128_WATERMARK_N/4) << R128_WMC_SHIFT)
		    | ((R128_WATERMARK_K/64) << R128_WB_WM_SHIFT) );

	/* Force read.  Why?  Because it's in the examples... */
	R128_READ( R128_PM4_BUFFER_ADDR );

	/* Turn on bus mastering */
	tmp = R128_READ( R128_BUS_CNTL ) & ~R128_BUS_MASTER_DIS;
	R128_WRITE( R128_BUS_CNTL, tmp );
}

static int r128_do_init_cce( drm_device_t *dev, drm_r128_init_t *init )
{
	drm_r128_private_t *dev_priv;
        int i;

	dev_priv = drm_alloc( sizeof(drm_r128_private_t), DRM_MEM_DRIVER );
	if ( dev_priv == NULL )
		return -ENOMEM;
	dev->dev_private = (void *)dev_priv;

	memset( dev_priv, 0, sizeof(drm_r128_private_t) );

	dev_priv->is_pci = init->is_pci;

	/* GH: We don't support PCI cards until PCI GART is implemented.
	 * Fail here so we can remove all checks for PCI cards around
	 * the CCE ring code.
	 */
	if ( dev_priv->is_pci ) {
		drm_free( dev_priv, sizeof(*dev_priv), DRM_MEM_DRIVER );
		dev->dev_private = NULL;
		return -EINVAL;
	}

	dev_priv->usec_timeout = init->usec_timeout;
	if ( dev_priv->usec_timeout < 1 ||
	     dev_priv->usec_timeout > R128_MAX_USEC_TIMEOUT ) {
		drm_free( dev_priv, sizeof(*dev_priv), DRM_MEM_DRIVER );
		dev->dev_private = NULL;
		return -EINVAL;
	}

	dev_priv->cce_mode = init->cce_mode;
	dev_priv->cce_secure = init->cce_secure;

	/* GH: Simple idle check.
	 */
	atomic_set( &dev_priv->idle_count, 0 );

	/* We don't support anything other than bus-mastering ring mode,
	 * but the ring can be in either AGP or PCI space for the ring
	 * read pointer.
	 */
	if ( ( init->cce_mode != R128_PM4_192BM ) &&
	     ( init->cce_mode != R128_PM4_128BM_64INDBM ) &&
	     ( init->cce_mode != R128_PM4_64BM_128INDBM ) &&
	     ( init->cce_mode != R128_PM4_64BM_64VCBM_64INDBM ) ) {
		drm_free( dev_priv, sizeof(*dev_priv), DRM_MEM_DRIVER );
		dev->dev_private = NULL;
		return -EINVAL;
	}

	switch ( init->cce_mode ) {
	case R128_PM4_NONPM4:
		dev_priv->cce_fifo_size = 0;
		break;
	case R128_PM4_192PIO:
	case R128_PM4_192BM:
		dev_priv->cce_fifo_size = 192;
		break;
	case R128_PM4_128PIO_64INDBM:
	case R128_PM4_128BM_64INDBM:
		dev_priv->cce_fifo_size = 128;
		break;
	case R128_PM4_64PIO_128INDBM:
	case R128_PM4_64BM_128INDBM:
	case R128_PM4_64PIO_64VCBM_64INDBM:
	case R128_PM4_64BM_64VCBM_64INDBM:
	case R128_PM4_64PIO_64VCPIO_64INDPIO:
		dev_priv->cce_fifo_size = 64;
		break;
	}

	dev_priv->fb_bpp	= init->fb_bpp;
	dev_priv->front_offset	= init->front_offset;
	dev_priv->front_pitch	= init->front_pitch;
	dev_priv->back_offset	= init->back_offset;
	dev_priv->back_pitch	= init->back_pitch;

	dev_priv->depth_bpp	= init->depth_bpp;
	dev_priv->depth_offset	= init->depth_offset;
	dev_priv->depth_pitch	= init->depth_pitch;
	dev_priv->span_offset	= init->span_offset;

	dev_priv->front_pitch_offset_c = (((dev_priv->front_pitch/8) << 21) |
					  (dev_priv->front_offset >> 5));
	dev_priv->back_pitch_offset_c = (((dev_priv->back_pitch/8) << 21) |
					 (dev_priv->back_offset >> 5));
	dev_priv->depth_pitch_offset_c = (((dev_priv->depth_pitch/8) << 21) |
					  (dev_priv->depth_offset >> 5) |
					  R128_DST_TILE);
	dev_priv->span_pitch_offset_c = (((dev_priv->depth_pitch/8) << 21) |
					 (dev_priv->span_offset >> 5));

	/* FIXME: We want multiple shared areas, including one shared
	 * only by the X Server and kernel module.
	 */
	for ( i = 0 ; i < dev->map_count ; i++ ) {
		if ( dev->maplist[i]->type == _DRM_SHM ) {
			dev_priv->sarea = dev->maplist[i];
			break;
		}
	}

	DO_FIND_MAP( dev_priv->fb, init->fb_offset );
	DO_FIND_MAP( dev_priv->mmio, init->mmio_offset );
	DO_FIND_MAP( dev_priv->cce_ring, init->ring_offset );
	DO_FIND_MAP( dev_priv->ring_rptr, init->ring_rptr_offset );
	DO_FIND_MAP( dev_priv->buffers, init->buffers_offset );

	if ( !dev_priv->is_pci ) {
		DO_FIND_MAP( dev_priv->agp_textures,
			     init->agp_textures_offset );
	}

	dev_priv->sarea_priv =
		(drm_r128_sarea_t *)((u8 *)dev_priv->sarea->handle +
				     init->sarea_priv_offset);

	DO_REMAP( dev_priv->cce_ring );
	DO_REMAP( dev_priv->ring_rptr );
	DO_REMAP( dev_priv->buffers );
#if 0
	if ( !dev_priv->is_pci ) {
		DO_REMAP( dev_priv->agp_textures );
	}
#endif

	dev_priv->ring.head = ((__volatile__ u32 *)
			       dev_priv->ring_rptr->handle);

	dev_priv->ring.start = (u32 *)dev_priv->cce_ring->handle;
	dev_priv->ring.end = ((u32 *)dev_priv->cce_ring->handle
			      + init->ring_size / sizeof(u32));
	dev_priv->ring.size = init->ring_size;
	dev_priv->ring.size_l2qw = drm_order( init->ring_size / 8 );

	dev_priv->ring.tail_mask =
		(dev_priv->ring.size / sizeof(u32)) - 1;

	dev_priv->sarea_priv->last_frame = 0;
	R128_WRITE( R128_LAST_FRAME_REG, dev_priv->sarea_priv->last_frame );

	dev_priv->sarea_priv->last_dispatch = 0;
	R128_WRITE( R128_LAST_DISPATCH_REG,
		    dev_priv->sarea_priv->last_dispatch );

	r128_cce_init_ring_buffer( dev );
	r128_cce_load_microcode( dev_priv );
	r128_do_engine_reset( dev );

	return 0;
}

static int r128_do_cleanup_cce( drm_device_t *dev )
{
	if ( dev->dev_private ) {
		drm_r128_private_t *dev_priv = dev->dev_private;

		DO_REMAPFREE( dev_priv->cce_ring );
		DO_REMAPFREE( dev_priv->ring_rptr );
		DO_REMAPFREE( dev_priv->buffers );
#if 0
		if ( !dev_priv->is_pci ) {
			DO_REMAPFREE( dev_priv->agp_textures );
		}
#endif

		drm_free( dev->dev_private, sizeof(drm_r128_private_t),
			  DRM_MEM_DRIVER );
		dev->dev_private = NULL;
	}

	return 0;
}

int r128_cce_init( struct inode *inode, struct file *filp,
		   unsigned int cmd, unsigned long arg )
{
        drm_file_t *priv = filp->private_data;
        drm_device_t *dev = priv->dev;
	drm_r128_init_t init;

	if ( copy_from_user( &init, (drm_r128_init_t *)arg, sizeof(init) ) )
		return -EFAULT;

	switch ( init.func ) {
	case R128_INIT_CCE:
		return r128_do_init_cce( dev, &init );
	case R128_CLEANUP_CCE:
		return r128_do_cleanup_cce( dev );
	}

	return -EINVAL;
}

int r128_cce_start( struct inode *inode, struct file *filp,
		    unsigned int cmd, unsigned long arg )
{
        drm_file_t *priv = filp->private_data;
        drm_device_t *dev = priv->dev;
	drm_r128_private_t *dev_priv = dev->dev_private;
	DRM_DEBUG( "%s\n", __FUNCTION__ );

	if ( !_DRM_LOCK_IS_HELD( dev->lock.hw_lock->lock ) ||
	     dev->lock.pid != current->pid ) {
		DRM_ERROR( "%s called without lock held\n", __FUNCTION__ );
		return -EINVAL;
	}
	if ( dev_priv->cce_running || dev_priv->cce_mode == R128_PM4_NONPM4 ) {
		DRM_DEBUG( "%s while CCE running\n", __FUNCTION__ );
		return 0;
	}

	r128_do_cce_start( dev_priv );

	return 0;
}

/* Stop the CCE.  The engine must have been idled before calling this
 * routine.
 */
int r128_cce_stop( struct inode *inode, struct file *filp,
		   unsigned int cmd, unsigned long arg )
{
        drm_file_t *priv = filp->private_data;
        drm_device_t *dev = priv->dev;
	drm_r128_private_t *dev_priv = dev->dev_private;
	drm_r128_cce_stop_t stop;
	int ret;
	DRM_DEBUG( "%s\n", __FUNCTION__ );

	if ( !_DRM_LOCK_IS_HELD( dev->lock.hw_lock->lock ) ||
	     dev->lock.pid != current->pid ) {
		DRM_ERROR( "%s called without lock held\n", __FUNCTION__ );
		return -EINVAL;
	}

	if ( copy_from_user( &stop, (drm_r128_init_t *)arg, sizeof(stop) ) )
		return -EFAULT;

	/* Flush any pending CCE commands.  This ensures any outstanding
	 * commands are exectuted by the engine before we turn it off.
	 */
	if ( stop.flush ) {
		r128_do_cce_flush( dev_priv );
	}

	/* If we fail to make the engine go idle, we return an error
	 * code so that the DRM ioctl wrapper can try again.
	 */
	if ( stop.idle ) {
		ret = r128_do_cce_idle( dev_priv );
		if ( ret < 0 ) return ret;
	}

	/* Finally, we can turn off the CCE.  If the engine isn't idle,
	 * we will get some dropped triangles as they won't be fully
	 * rendered before the CCE is shut down.
	 */
	r128_do_cce_stop( dev_priv );

	/* Reset the engine */
	r128_do_engine_reset( dev );

	return 0;
}

/* Just reset the CCE ring.  Called as part of an X Server engine reset.
 */
int r128_cce_reset( struct inode *inode, struct file *filp,
		    unsigned int cmd, unsigned long arg )
{
        drm_file_t *priv = filp->private_data;
        drm_device_t *dev = priv->dev;
	drm_r128_private_t *dev_priv = dev->dev_private;
	DRM_DEBUG( "%s\n", __FUNCTION__ );

	if ( !_DRM_LOCK_IS_HELD( dev->lock.hw_lock->lock ) ||
	     dev->lock.pid != current->pid ) {
		DRM_ERROR( "%s called without lock held\n", __FUNCTION__ );
		return -EINVAL;
	}
	if ( !dev_priv ) {
		DRM_DEBUG( "%s called before init done\n", __FUNCTION__ );
		return -EINVAL;
	}

	r128_do_cce_reset( dev_priv );

	/* The CCE is no longer running after an engine reset */
	dev_priv->cce_running = 0;

	return 0;
}

int r128_cce_idle( struct inode *inode, struct file *filp,
		   unsigned int cmd, unsigned long arg )
{
        drm_file_t *priv = filp->private_data;
        drm_device_t *dev = priv->dev;
	drm_r128_private_t *dev_priv = dev->dev_private;
	DRM_DEBUG( "%s\n", __FUNCTION__ );

	if ( !_DRM_LOCK_IS_HELD( dev->lock.hw_lock->lock ) ||
	     dev->lock.pid != current->pid ) {
		DRM_ERROR( "%s called without lock held\n", __FUNCTION__ );
		return -EINVAL;
	}

	if ( dev_priv->cce_running ) {
		r128_do_cce_flush( dev_priv );
	}

	return r128_do_cce_idle( dev_priv );
}

int r128_engine_reset( struct inode *inode, struct file *filp,
		       unsigned int cmd, unsigned long arg )
{
        drm_file_t *priv = filp->private_data;
        drm_device_t *dev = priv->dev;
	DRM_DEBUG( "%s\n", __FUNCTION__ );

	if ( !_DRM_LOCK_IS_HELD( dev->lock.hw_lock->lock ) ||
	     dev->lock.pid != current->pid ) {
		DRM_ERROR( "%s called without lock held\n", __FUNCTION__ );
		return -EINVAL;
	}

	return r128_do_engine_reset( dev );
}


/* ================================================================
 * Freelist management
 */
#define R128_BUFFER_USED	0xffffffff
#define R128_BUFFER_FREE	0

#if 0
static int r128_freelist_init( drm_device_t *dev )
{
	drm_device_dma_t *dma = dev->dma;
	drm_r128_private_t *dev_priv = dev->dev_private;
	drm_buf_t *buf;
	drm_r128_buf_priv_t *buf_priv;
	drm_r128_freelist_t *entry;
	int i;

	dev_priv->head = drm_alloc( sizeof(drm_r128_freelist_t),
				    DRM_MEM_DRIVER );
	if ( dev_priv->head == NULL )
		return -ENOMEM;

	memset( dev_priv->head, 0, sizeof(drm_r128_freelist_t) );
	dev_priv->head->age = R128_BUFFER_USED;

	for ( i = 0 ; i < dma->buf_count ; i++ ) {
		buf = dma->buflist[i];
		buf_priv = buf->dev_private;

		entry = drm_alloc( sizeof(drm_r128_freelist_t),
				   DRM_MEM_DRIVER );
		if ( !entry ) return -ENOMEM;

		entry->age = R128_BUFFER_FREE;
		entry->buf = buf;
		entry->prev = dev_priv->head;
		entry->next = dev_priv->head->next;
		if ( !entry->next )
			dev_priv->tail = entry;

		buf_priv->discard = 0;
		buf_priv->dispatched = 0;
		buf_priv->list_entry = entry;

		dev_priv->head->next = entry;

		if ( dev_priv->head->next )
			dev_priv->head->next->prev = entry;
	}

	return 0;

}
#endif

drm_buf_t *r128_freelist_get( drm_device_t *dev )
{
	drm_device_dma_t *dma = dev->dma;
	drm_r128_private_t *dev_priv = dev->dev_private;
	drm_r128_buf_priv_t *buf_priv;
	drm_buf_t *buf;
	int i, t;

	/* FIXME: Optimize -- use freelist code */

	for ( i = 0 ; i < dma->buf_count ; i++ ) {
		buf = dma->buflist[i];
		buf_priv = buf->dev_private;
		if ( buf->pid == 0 )
			return buf;
	}

	for ( t = 0 ; t < dev_priv->usec_timeout ; t++ ) {
		u32 done_age = R128_READ( R128_LAST_DISPATCH_REG );

		for ( i = 0 ; i < dma->buf_count ; i++ ) {
			buf = dma->buflist[i];
			buf_priv = buf->dev_private;
			if ( buf->pending && buf_priv->age <= done_age ) {
				/* The buffer has been processed, so it
				 * can now be used.
				 */
				buf->pending = 0;
				return buf;
			}
		}
		udelay( 1 );
	}

	DRM_ERROR( "returning NULL!\n" );
	return NULL;
}

void r128_freelist_reset( drm_device_t *dev )
{
	drm_device_dma_t *dma = dev->dma;
	int i;

	for ( i = 0 ; i < dma->buf_count ; i++ ) {
		drm_buf_t *buf = dma->buflist[i];
		drm_r128_buf_priv_t *buf_priv = buf->dev_private;
		buf_priv->age = 0;
	}
}


/* ================================================================
 * CCE packet submission
 */

int r128_wait_ring( drm_r128_private_t *dev_priv, int n )
{
	drm_r128_ring_buffer_t *ring = &dev_priv->ring;
	int i;

	for ( i = 0 ; i < dev_priv->usec_timeout ; i++ ) {
		ring->space = *ring->head - ring->tail;
		if ( ring->space <= 0 )
			ring->space += ring->size;

		if ( ring->space >= n )
			return 0;

		udelay( 1 );
	}

	return -EBUSY;
}

void r128_update_ring_snapshot( drm_r128_private_t *dev_priv )
{
	drm_r128_ring_buffer_t *ring = &dev_priv->ring;

	ring->space = *ring->head - ring->tail;
#if R128_PERFORMANCE_BOXES
	if ( ring->space == 0 )
		atomic_inc( &dev_priv->idle_count );
#endif
	if ( ring->space <= 0 )
		ring->space += ring->size;
}

#if 0
static int r128_verify_command( drm_r128_private_t *dev_priv,
				u32 cmd, int *size )
{
	int writing = 1;

	*size = 0;

	switch ( cmd & R128_CCE_PACKET_MASK ) {
	case R128_CCE_PACKET0:
		if ( (cmd & R128_CCE_PACKET0_REG_MASK) <= (0x1004 >> 2) &&
		     (cmd & R128_CCE_PACKET0_REG_MASK) !=
		     (R128_PM4_VC_FPU_SETUP >> 2) ) {
			writing = 0;
		}
		*size = ((cmd & R128_CCE_PACKET_COUNT_MASK) >> 16) + 2;
		break;

	case R128_CCE_PACKET1:
		if ( (cmd & R128_CCE_PACKET1_REG0_MASK) <= (0x1004 >> 2) &&
		     (cmd & R128_CCE_PACKET1_REG0_MASK) !=
		     (R128_PM4_VC_FPU_SETUP >> 2) ) {
			writing = 0;
		}
		if ( (cmd & R128_CCE_PACKET1_REG1_MASK) <= (0x1004 << 9) &&
		     (cmd & R128_CCE_PACKET1_REG1_MASK) !=
		     (R128_PM4_VC_FPU_SETUP << 9) ) {
			writing = 0;
		}
		*size = 3;
		break;

	case R128_CCE_PACKET2:
		break;

	case R128_CCE_PACKET3:
		*size = ((cmd & R128_CCE_PACKET_COUNT_MASK) >> 16) + 2;
		break;

	}

	return writing;
}

static int r128_submit_packet_ring_secure( drm_r128_private_t *dev_priv,
					   u32 *commands, int *count )
{
#if 0
	int write = dev_priv->sarea_priv->ring_write;
	int *write_ptr = dev_priv->ring_start + write;
	int c = *count;
	u32 tmp = 0;
	int psize = 0;
	int writing = 1;
	int timeout;

	while ( c > 0 ) {
		tmp = *commands++;
		if ( !psize ) {
			writing = r128_verify_command( dev_priv, tmp, &psize );
		}
		psize--;

		if ( writing ) {
			write++;
			*write_ptr++ = tmp;
		}
		if ( write >= dev_priv->ring_entries ) {
			write = 0;
			write_ptr = dev_priv->ring_start;
		}
		timeout = 0;
		while ( write == *dev_priv->ring_read_ptr ) {
			R128_READ( R128_PM4_BUFFER_DL_RPTR );
			if ( timeout++ >= dev_priv->usec_timeout )
				return -EBUSY;
			udelay( 1 );
		}
		c--;
	}

	if ( write < 32 ) {
		memcpy( dev_priv->ring_end,
			dev_priv->ring_start,
			write * sizeof(u32) );
	}

	/* Make sure WC cache has been flushed */
	r128_flush_write_combine();

	dev_priv->sarea_priv->ring_write = write;
	R128_WRITE( R128_PM4_BUFFER_DL_WPTR, write );

	*count = 0;
#endif
	return 0;
}

static int r128_submit_packet_ring_insecure( drm_r128_private_t *dev_priv,
					     u32 *commands, int *count )
{
#if 0
	int write = dev_priv->sarea_priv->ring_write;
	int *write_ptr = dev_priv->ring_start + write;
	int c = *count;
	int timeout;

	while ( c > 0 ) {
		write++;
		*write_ptr++ = *commands++;
		if ( write >= dev_priv->ring_entries ) {
			write = 0;
			write_ptr = dev_priv->ring_start;
		}

		timeout = 0;
		while ( write == *dev_priv->ring_read_ptr ) {
			R128_READ( R128_PM4_BUFFER_DL_RPTR );
			if ( timeout++ >= dev_priv->usec_timeout )
				return -EBUSY;
			udelay( 1 );
		}
		c--;
	}

	if ( write < 32 ) {
		memcpy( dev_priv->ring_end,
			dev_priv->ring_start,
			write * sizeof(u32) );
	}

	/* Make sure WC cache has been flushed */
	r128_flush_write_combine();

	dev_priv->sarea_priv->ring_write = write;
	R128_WRITE( R128_PM4_BUFFER_DL_WPTR, write );

	*count = 0;
#endif
	return 0;
}
#endif

/* Internal packet submission routine.  This uses the insecure versions
 * of the packet submission functions, and thus should only be used for
 * packets generated inside the kernel module.
 */
int r128_do_submit_packet( drm_r128_private_t *dev_priv,
			   u32 *buffer, int count )
{
	int c = count;
	int ret = 0;

#if 0
	int left = 0;

	if ( c >= dev_priv->ring_entries ) {
		c = dev_priv->ring_entries - 1;
		left = count - c;
	}

	/* Since this is only used by the kernel we can use the
	 * insecure ring buffer submit packet routine.
	 */
	ret = r128_submit_packet_ring_insecure( dev_priv, buffer, &c );
	c += left;
#endif

	return ( ret < 0 ) ? ret : c;
}

/* External packet submission routine.  This uses the secure versions
 * by default, and can thus submit packets received from user space.
 */
int r128_cce_packet( struct inode *inode, struct file *filp,
		     unsigned int cmd, unsigned long arg )
{
        drm_file_t *priv = filp->private_data;
        drm_device_t *dev = priv->dev;
	drm_r128_private_t *dev_priv = dev->dev_private;
	drm_r128_packet_t packet;
	u32 *buffer;
	int c;
	int size;
	int ret = 0;

#if 0
	/* GH: Disable packet submission for now.
	 */
	return -EINVAL;
#endif

	if ( !_DRM_LOCK_IS_HELD( dev->lock.hw_lock->lock ) ||
	     dev->lock.pid != current->pid ) {
		DRM_ERROR( "r128_submit_packet called without lock held\n" );
		return -EINVAL;
	}

	if ( copy_from_user( &packet, (drm_r128_packet_t *)arg,
			     sizeof(packet) ) )
		return -EFAULT;

#if 0
	c = packet.count;
	size = c * sizeof(*buffer);

	{
		int left = 0;

		if ( c >= dev_priv->ring_entries ) {
			c = dev_priv->ring_entries - 1;
			size = c * sizeof(*buffer);
			left = packet.count - c;
		}

		buffer = kmalloc( size, 0 );
		if ( buffer == NULL)
			return -ENOMEM;
		if ( copy_from_user( buffer, packet.buffer, size ) )
			return -EFAULT;

		if ( dev_priv->cce_secure ) {
			ret = r128_submit_packet_ring_secure( dev_priv,
							      buffer, &c );
		} else {
			ret = r128_submit_packet_ring_insecure( dev_priv,
								buffer, &c );
		}
		c += left;
	}

	kfree( buffer );
#else
	c = 0;
#endif

	packet.count = c;
	if ( copy_to_user( (drm_r128_packet_t *)arg, &packet,
			   sizeof(packet) ) )
		return -EFAULT;

	if ( ret ) {
		return ret;
	} else if ( c > 0 ) {
		return -EAGAIN;
	}
	return 0;
}

#if 0
static int r128_send_vertbufs( drm_device_t *dev, drm_r128_vertex_t *v )
{
	drm_device_dma_t    *dma      = dev->dma;
	drm_r128_private_t  *dev_priv = dev->dev_private;
	drm_r128_buf_priv_t *buf_priv;
	drm_buf_t           *buf;
	int                  i, ret;
	RING_LOCALS;

	/* Make sure we have valid data */
	for (i = 0; i < v->send_count; i++) {
		int idx = v->send_indices[i];

		if (idx < 0 || idx >= dma->buf_count) {
			DRM_ERROR("Index %d (of %d max)\n",
				  idx, dma->buf_count - 1);
			return -EINVAL;
		}
		buf = dma->buflist[idx];
		if (buf->pid != current->pid) {
			DRM_ERROR("Process %d using buffer owned by %d\n",
				  current->pid, buf->pid);
			return -EINVAL;
		}
		if (buf->pending) {
			DRM_ERROR("Sending pending buffer:"
				  " buffer %d, offset %d\n",
				  v->send_indices[i], i);
			return -EINVAL;
		}
	}

	/* Wait for idle, if we've wrapped to make sure that all pending
           buffers have been processed */
	if (dev_priv->submit_age == R128_MAX_VBUF_AGE) {
		if ((ret = r128_do_cce_idle(dev)) < 0) return ret;
		dev_priv->submit_age = 0;
		r128_freelist_reset(dev);
	}

	/* Make sure WC cache has been flushed (if in PIO mode) */
	if (!dev_priv->cce_is_bm_mode) r128_flush_write_combine();

	/* FIXME: Add support for sending vertex buffer to the CCE here
	   instead of in client code.  The v->prim holds the primitive
	   type that should be drawn.  Loop over the list buffers in
	   send_indices[] and submit a packet for each VB.

	   This will require us to loop over the clip rects here as
	   well, which implies that we extend the kernel driver to allow
	   cliprects to be stored here.  Note that the cliprects could
	   possibly come from the X server instead of the client, but
	   this will require additional changes to the DRI to allow for
	   this optimization. */

	/* Submit a CCE packet that writes submit_age to R128_VB_AGE_REG */
#if 0
	cce_buffer[0] = R128CCE0(R128_CCE_PACKET0, R128_VB_AGE_REG, 0);
	cce_buffer[1] = dev_priv->submit_age;

	if ((ret = r128_do_submit_packet(dev, cce_buffer, 2)) < 0) {
		/* Until we add support for sending VBs to the CCE in
		   this routine, we can recover from this error.  After
		   we add that support, we won't be able to easily
		   recover, so we will probably have to implement
		   another mechanism for handling timeouts from packets
		   submitted directly by the kernel. */
		return ret;
	}
#else
	BEGIN_RING( 2 );

	OUT_RING( CCE_PACKET0( R128_VB_AGE_REG, 0 ) );
	OUT_RING( dev_priv->submit_age );

	ADVANCE_RING();
#endif
	/* Now that the submit packet request has succeeded, we can mark
           the buffers as pending */
	for (i = 0; i < v->send_count; i++) {
		buf = dma->buflist[v->send_indices[i]];
		buf->pending = 1;

		buf_priv      = buf->dev_private;
		buf_priv->age = dev_priv->submit_age;
	}

	dev_priv->submit_age++;

	return 0;
}
#endif




static int r128_cce_get_buffers( drm_device_t *dev, drm_dma_t *d )
{
	int i;
	drm_buf_t *buf;

	for ( i = d->granted_count ; i < d->request_count ; i++ ) {
		buf = r128_freelist_get( dev );
		if ( !buf ) return -EAGAIN;

		buf->pid = current->pid;

		if ( copy_to_user( &d->request_indices[i], &buf->idx,
				   sizeof(buf->idx) ) )
			return -EFAULT;
		if ( copy_to_user( &d->request_sizes[i], &buf->total,
				   sizeof(buf->total) ) )
			return -EFAULT;

		d->granted_count++;
	}
	return 0;
}

int r128_cce_buffers( struct inode *inode, struct file *filp,
		      unsigned int cmd, unsigned long arg )
{
	drm_file_t *priv = filp->private_data;
	drm_device_t *dev = priv->dev;
	drm_device_dma_t *dma = dev->dma;
	int ret = 0;
	drm_dma_t d;

	if ( copy_from_user( &d, (drm_dma_t *) arg, sizeof(d) ) )
		return -EFAULT;

	if ( !_DRM_LOCK_IS_HELD( dev->lock.hw_lock->lock ) ||
	     dev->lock.pid != current->pid ) {
		DRM_ERROR( "%s called without lock held\n", __FUNCTION__ );
		return -EINVAL;
	}

	/* Please don't send us buffers.
	 */
	if ( d.send_count != 0 ) {
		DRM_ERROR( "Process %d trying to send %d buffers via drmDMA\n",
			   current->pid, d.send_count );
		return -EINVAL;
	}

	/* We'll send you buffers.
	 */
	if ( d.request_count < 0 || d.request_count > dma->buf_count ) {
		DRM_ERROR( "Process %d trying to get %d buffers (of %d max)\n",
			   current->pid, d.request_count, dma->buf_count );
		return -EINVAL;
	}

	d.granted_count = 0;

	if ( d.request_count ) {
		ret = r128_cce_get_buffers( dev, &d );
	}

	if ( copy_to_user( (drm_dma_t *) arg, &d, sizeof(d) ) )
		return -EFAULT;

	return ret;
}
