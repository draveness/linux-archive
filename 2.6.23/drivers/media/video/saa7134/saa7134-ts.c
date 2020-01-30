/*
 *
 * device driver for philips saa7134 based TV cards
 * video4linux video interface
 *
 * (c) 2001,02 Gerd Knorr <kraxel@bytesex.org> [SuSE Labs]
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include "saa7134-reg.h"
#include "saa7134.h"

/* ------------------------------------------------------------------ */

static unsigned int ts_debug  = 0;
module_param(ts_debug, int, 0644);
MODULE_PARM_DESC(ts_debug,"enable debug messages [ts]");

#define dprintk(fmt, arg...)	if (ts_debug) \
	printk(KERN_DEBUG "%s/ts: " fmt, dev->name , ## arg)

/* ------------------------------------------------------------------ */

static int buffer_activate(struct saa7134_dev *dev,
			   struct saa7134_buf *buf,
			   struct saa7134_buf *next)
{

	dprintk("buffer_activate [%p]",buf);
	buf->vb.state = STATE_ACTIVE;
	buf->top_seen = 0;

	if (NULL == next)
		next = buf;
	if (V4L2_FIELD_TOP == buf->vb.field) {
		dprintk("- [top]     buf=%p next=%p\n",buf,next);
		saa_writel(SAA7134_RS_BA1(5),saa7134_buffer_base(buf));
		saa_writel(SAA7134_RS_BA2(5),saa7134_buffer_base(next));
	} else {
		dprintk("- [bottom]  buf=%p next=%p\n",buf,next);
		saa_writel(SAA7134_RS_BA1(5),saa7134_buffer_base(next));
		saa_writel(SAA7134_RS_BA2(5),saa7134_buffer_base(buf));
	}

	/* start DMA */
	saa7134_set_dmabits(dev);

	mod_timer(&dev->ts_q.timeout, jiffies+BUFFER_TIMEOUT);
	return 0;
}

static int buffer_prepare(struct videobuf_queue *q, struct videobuf_buffer *vb,
			  enum v4l2_field field)
{
	struct saa7134_dev *dev = q->priv_data;
	struct saa7134_buf *buf = container_of(vb,struct saa7134_buf,vb);
	unsigned int lines, llength, size;
	u32 control;
	int err;

	dprintk("buffer_prepare [%p,%s]\n",buf,v4l2_field_names[field]);

	llength = TS_PACKET_SIZE;
	lines = dev->ts.nr_packets;

	size = lines * llength;
	if (0 != buf->vb.baddr  &&  buf->vb.bsize < size)
		return -EINVAL;

	if (buf->vb.size != size) {
		saa7134_dma_free(q,buf);
	}

	if (STATE_NEEDS_INIT == buf->vb.state) {
		buf->vb.width  = llength;
		buf->vb.height = lines;
		buf->vb.size   = size;
		buf->pt        = &dev->ts.pt_ts;

		err = videobuf_iolock(q,&buf->vb,NULL);
		if (err)
			goto oops;
		err = saa7134_pgtable_build(dev->pci,buf->pt,
					    buf->vb.dma.sglist,
					    buf->vb.dma.sglen,
					    saa7134_buffer_startpage(buf));
		if (err)
			goto oops;
	}

	/* dma: setup channel 5 (= TS) */
	control = SAA7134_RS_CONTROL_BURST_16 |
		  SAA7134_RS_CONTROL_ME |
		  (buf->pt->dma >> 12);

	saa_writeb(SAA7134_TS_DMA0, ((lines-1)&0xff));
	saa_writeb(SAA7134_TS_DMA1, (((lines-1)>>8)&0xff));
	saa_writeb(SAA7134_TS_DMA2, ((((lines-1)>>16)&0x3f) | 0x00)); /* TSNOPIT=0, TSCOLAP=0 */
	saa_writel(SAA7134_RS_PITCH(5),TS_PACKET_SIZE);
	saa_writel(SAA7134_RS_CONTROL(5),control);

	buf->vb.state = STATE_PREPARED;
	buf->activate = buffer_activate;
	buf->vb.field = field;
	return 0;

 oops:
	saa7134_dma_free(q,buf);
	return err;
}

static int
buffer_setup(struct videobuf_queue *q, unsigned int *count, unsigned int *size)
{
	struct saa7134_dev *dev = q->priv_data;

	*size = TS_PACKET_SIZE * dev->ts.nr_packets;
	if (0 == *count)
		*count = dev->ts.nr_bufs;
	*count = saa7134_buffer_count(*size,*count);
	return 0;
}

static void buffer_queue(struct videobuf_queue *q, struct videobuf_buffer *vb)
{
	struct saa7134_dev *dev = q->priv_data;
	struct saa7134_buf *buf = container_of(vb,struct saa7134_buf,vb);

	saa7134_buffer_queue(dev,&dev->ts_q,buf);
}

static void buffer_release(struct videobuf_queue *q, struct videobuf_buffer *vb)
{
	struct saa7134_buf *buf = container_of(vb,struct saa7134_buf,vb);

	saa7134_dma_free(q,buf);
}

struct videobuf_queue_ops saa7134_ts_qops = {
	.buf_setup    = buffer_setup,
	.buf_prepare  = buffer_prepare,
	.buf_queue    = buffer_queue,
	.buf_release  = buffer_release,
};
EXPORT_SYMBOL_GPL(saa7134_ts_qops);

/* ----------------------------------------------------------- */
/* exported stuff                                              */

static unsigned int tsbufs = 8;
module_param(tsbufs, int, 0444);
MODULE_PARM_DESC(tsbufs,"number of ts buffers, range 2-32");

static unsigned int ts_nr_packets = 64;
module_param(ts_nr_packets, int, 0444);
MODULE_PARM_DESC(ts_nr_packets,"size of a ts buffers (in ts packets)");

int saa7134_ts_init1(struct saa7134_dev *dev)
{
	/* sanitycheck insmod options */
	if (tsbufs < 2)
		tsbufs = 2;
	if (tsbufs > VIDEO_MAX_FRAME)
		tsbufs = VIDEO_MAX_FRAME;
	if (ts_nr_packets < 4)
		ts_nr_packets = 4;
	if (ts_nr_packets > 312)
		ts_nr_packets = 312;
	dev->ts.nr_bufs    = tsbufs;
	dev->ts.nr_packets = ts_nr_packets;

	INIT_LIST_HEAD(&dev->ts_q.queue);
	init_timer(&dev->ts_q.timeout);
	dev->ts_q.timeout.function = saa7134_buffer_timeout;
	dev->ts_q.timeout.data     = (unsigned long)(&dev->ts_q);
	dev->ts_q.dev              = dev;
	dev->ts_q.need_two         = 1;
	saa7134_pgtable_alloc(dev->pci,&dev->ts.pt_ts);

	/* init TS hw */
	saa_writeb(SAA7134_TS_SERIAL1, 0x00);  /* deactivate TS softreset */
	saa_writeb(SAA7134_TS_PARALLEL, 0xec); /* TSSOP high active, TSVAL high active, TSLOCK ignored */
	saa_writeb(SAA7134_TS_PARALLEL_SERIAL, (TS_PACKET_SIZE-1));
	saa_writeb(SAA7134_TS_DMA0, ((dev->ts.nr_packets-1)&0xff));
	saa_writeb(SAA7134_TS_DMA1, (((dev->ts.nr_packets-1)>>8)&0xff));
	saa_writeb(SAA7134_TS_DMA2, ((((dev->ts.nr_packets-1)>>16)&0x3f) | 0x00)); /* TSNOPIT=0, TSCOLAP=0 */

	return 0;
}

int saa7134_ts_fini(struct saa7134_dev *dev)
{
	saa7134_pgtable_free(dev->pci,&dev->ts.pt_ts);
	return 0;
}


void saa7134_irq_ts_done(struct saa7134_dev *dev, unsigned long status)
{
	enum v4l2_field field;

	spin_lock(&dev->slock);
	if (dev->ts_q.curr) {
		field = dev->ts_q.curr->vb.field;
		if (field == V4L2_FIELD_TOP) {
			if ((status & 0x100000) != 0x000000)
				goto done;
		} else {
			if ((status & 0x100000) != 0x100000)
				goto done;
		}
		saa7134_buffer_finish(dev,&dev->ts_q,STATE_DONE);
	}
	saa7134_buffer_next(dev,&dev->ts_q);

 done:
	spin_unlock(&dev->slock);
}

/* ----------------------------------------------------------- */
/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
