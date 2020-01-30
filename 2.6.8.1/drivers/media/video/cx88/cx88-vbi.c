#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>

#include "cx88.h"

static unsigned int vbibufs = 4;
MODULE_PARM(vbibufs,"i");
MODULE_PARM_DESC(vbibufs,"number of vbi buffers, range 2-32");

static unsigned int vbi_debug = 0;
MODULE_PARM(vbi_debug,"i");
MODULE_PARM_DESC(vbi_debug,"enable debug messages [video]");

#define dprintk(level,fmt, arg...)	if (vbi_debug >= level) \
	printk(KERN_DEBUG "%s: " fmt, dev->name , ## arg)

/* ------------------------------------------------------------------ */

void cx8800_vbi_fmt(struct cx8800_dev *dev, struct v4l2_format *f)
{
	memset(&f->fmt.vbi,0,sizeof(f->fmt.vbi));

	f->fmt.vbi.samples_per_line = VBI_LINE_LENGTH;
	f->fmt.vbi.sample_format = V4L2_PIX_FMT_GREY;
	f->fmt.vbi.offset = 244;
	f->fmt.vbi.count[0] = VBI_LINE_COUNT;
	f->fmt.vbi.count[1] = VBI_LINE_COUNT;

	if (dev->tvnorm->id & V4L2_STD_525_60) {
		/* ntsc */
		f->fmt.vbi.sampling_rate = 28636363;
		f->fmt.vbi.start[0] = 10 -1;
		f->fmt.vbi.start[1] = 273 -1;

	} else if (V4L2_STD_625_50) {
		/* pal */
		f->fmt.vbi.sampling_rate = 35468950;
		f->fmt.vbi.start[0] = 7 -1;
		f->fmt.vbi.start[1] = 319 -1;
	}
}

int cx8800_start_vbi_dma(struct cx8800_dev    *dev,
			 struct cx88_dmaqueue *q,
			 struct cx88_buffer   *buf)
{
	/* setup fifo + format */
	cx88_sram_channel_setup(dev, &cx88_sram_channels[SRAM_CH24],
				buf->vb.width, buf->risc.dma);

	cx_write(MO_VBOS_CONTROL, ( (1 << 18) |  // comb filter delay fixup
				    (1 << 15) |  // enable vbi capture
				    (1 << 11) ));

	/* reset counter */
	cx_write(MO_VBI_GPCNTRL,0x3);
	q->count = 1;

	/* enable irqs */
	cx_set(MO_PCI_INTMSK, 0x00fc01);
	cx_set(MO_VID_INTMSK, 0x0f0088);

	/* enable capture */
	cx_set(VID_CAPTURE_CONTROL,0x18);

	/* start dma */
	cx_set(MO_DEV_CNTRL2, (1<<5));
	cx_set(MO_VID_DMACNTRL, 0x88);

	return 0;
}

int cx8800_restart_vbi_queue(struct cx8800_dev    *dev,
			     struct cx88_dmaqueue *q)
{
	struct cx88_buffer *buf;
	struct list_head *item;

	if (list_empty(&q->active))
		return 0;

	buf = list_entry(q->active.next, struct cx88_buffer, vb.queue);
	dprintk(2,"restart_queue [%p/%d]: restart dma\n",
		buf, buf->vb.i);
	cx8800_start_vbi_dma(dev, q, buf);
	list_for_each(item,&q->active) {
		buf = list_entry(item, struct cx88_buffer, vb.queue);
		buf->count = q->count++;
	}
	mod_timer(&q->timeout, jiffies+BUFFER_TIMEOUT);
	return 0;
}

void cx8800_vbi_timeout(unsigned long data)
{
	struct cx8800_dev *dev = (struct cx8800_dev*)data;
	struct cx88_dmaqueue *q = &dev->vbiq;
	struct cx88_buffer *buf;
	unsigned long flags;

	cx88_sram_channel_dump(dev, &cx88_sram_channels[SRAM_CH24]);

	cx_clear(MO_VID_DMACNTRL, 0x88);
	cx_clear(VID_CAPTURE_CONTROL, 0x18);

	spin_lock_irqsave(&dev->slock,flags);
	while (!list_empty(&q->active)) {
		buf = list_entry(q->active.next, struct cx88_buffer, vb.queue);
		list_del(&buf->vb.queue);
		buf->vb.state = STATE_ERROR;
		wake_up(&buf->vb.done);
		printk("%s: [%p/%d] timeout - dma=0x%08lx\n", dev->name,
		       buf, buf->vb.i, (unsigned long)buf->risc.dma);
	}
	cx8800_restart_vbi_queue(dev,q);
	spin_unlock_irqrestore(&dev->slock,flags);
}

/* ------------------------------------------------------------------ */

static int
vbi_setup(struct file *file, unsigned int *count, unsigned int *size)
{
	*size = VBI_LINE_COUNT * VBI_LINE_LENGTH * 2;
	if (0 == *count)
		*count = vbibufs;
	if (*count < 2)
		*count = 2;
	if (*count > 32)
		*count = 32;
	return 0;
}

static int
vbi_prepare(struct file *file, struct videobuf_buffer *vb,
	    enum v4l2_field field)
{
	struct cx8800_fh   *fh  = file->private_data;
	struct cx8800_dev  *dev = fh->dev;
	struct cx88_buffer *buf = (struct cx88_buffer*)vb;
	unsigned int size;
	int rc;

	size = VBI_LINE_COUNT * VBI_LINE_LENGTH * 2;
	if (0 != buf->vb.baddr  &&  buf->vb.bsize < size)
		return -EINVAL;

	if (STATE_NEEDS_INIT == buf->vb.state) {
		buf->vb.width  = VBI_LINE_LENGTH;
		buf->vb.height = VBI_LINE_COUNT;
		buf->vb.size   = size;
		buf->vb.field  = V4L2_FIELD_SEQ_TB;

		if (0 != (rc = videobuf_iolock(dev->pci,&buf->vb,NULL)))
			goto fail;
		cx88_risc_buffer(dev->pci, &buf->risc,
				 buf->vb.dma.sglist,
				 0, buf->vb.width * buf->vb.height,
				 buf->vb.width, 0,
				 buf->vb.height);
	}
	buf->vb.state = STATE_PREPARED;
	return 0;

 fail:
	cx88_free_buffer(dev->pci,buf);
	return rc;
}

static void
vbi_queue(struct file *file, struct videobuf_buffer *vb)
{
	struct cx88_buffer    *buf  = (struct cx88_buffer*)vb;
	struct cx88_buffer    *prev;
	struct cx8800_fh      *fh   = file->private_data;
	struct cx8800_dev     *dev  = fh->dev;
	struct cx88_dmaqueue  *q    = &dev->vbiq;

	/* add jump to stopper */
	buf->risc.jmp[0] = cpu_to_le32(RISC_JUMP | RISC_IRQ1 | 0x10000);
	buf->risc.jmp[1] = cpu_to_le32(q->stopper.dma);

	if (list_empty(&q->active)) {
		list_add_tail(&buf->vb.queue,&q->active);
		cx8800_start_vbi_dma(dev, q, buf);
		buf->vb.state = STATE_ACTIVE;
		buf->count    = q->count++;
		mod_timer(&q->timeout, jiffies+BUFFER_TIMEOUT);
		dprintk(2,"[%p/%d] vbi_queue - first active\n",
			buf, buf->vb.i);

	} else {
		prev = list_entry(q->active.prev, struct cx88_buffer, vb.queue);
		list_add_tail(&buf->vb.queue,&q->active);
		buf->vb.state = STATE_ACTIVE;
		buf->count    = q->count++;
		prev->risc.jmp[1] = cpu_to_le32(buf->risc.dma);
		dprintk(2,"[%p/%d] buffer_queue - append to active\n",
			buf, buf->vb.i);
	}
}

static void vbi_release(struct file *file, struct videobuf_buffer *vb)
{
	struct cx88_buffer *buf = (struct cx88_buffer*)vb;
	struct cx8800_fh   *fh  = file->private_data;

	cx88_free_buffer(fh->dev->pci,buf);
}

struct videobuf_queue_ops cx8800_vbi_qops = {
	.buf_setup    = vbi_setup,
	.buf_prepare  = vbi_prepare,
	.buf_queue    = vbi_queue,
	.buf_release  = vbi_release,
};

/* ------------------------------------------------------------------ */
/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
