/*
 * device driver for philips saa7134 based TV cards
 * driver core
 *
 * (c) 2001-03 Gerd Knorr <kraxel@bytesex.org> [SuSE Labs]
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
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/kmod.h>
#include <linux/sound.h>
#include <linux/interrupt.h>

#include "saa7134-reg.h"
#include "saa7134.h"

MODULE_DESCRIPTION("v4l2 driver module for saa7130/34 based TV cards");
MODULE_AUTHOR("Gerd Knorr <kraxel@bytesex.org> [SuSE Labs]");
MODULE_LICENSE("GPL");

#define SAA7134_MAXBOARDS 8

/* ------------------------------------------------------------------ */

static unsigned int irq_debug = 0;
MODULE_PARM(irq_debug,"i");
MODULE_PARM_DESC(irq_debug,"enable debug messages [IRQ handler]");

static unsigned int core_debug = 0;
MODULE_PARM(core_debug,"i");
MODULE_PARM_DESC(core_debug,"enable debug messages [core]");

static unsigned int gpio_tracking = 0;
MODULE_PARM(gpio_tracking,"i");
MODULE_PARM_DESC(gpio_tracking,"enable debug messages [gpio]");

static unsigned int video_nr[] = {[0 ... (SAA7134_MAXBOARDS - 1)] = UNSET };
MODULE_PARM(video_nr,"1-" __stringify(SAA7134_MAXBOARDS) "i");
MODULE_PARM_DESC(video_nr,"video device number");

static unsigned int ts_nr[] = {[0 ... (SAA7134_MAXBOARDS - 1)] = UNSET };
MODULE_PARM(ts_nr,"1-" __stringify(SAA7134_MAXBOARDS) "i");
MODULE_PARM_DESC(ts_nr,"ts device number");

static unsigned int vbi_nr[] = {[0 ... (SAA7134_MAXBOARDS - 1)] = UNSET };
MODULE_PARM(vbi_nr,"1-" __stringify(SAA7134_MAXBOARDS) "i");
MODULE_PARM_DESC(vbi_nr,"vbi device number");

static unsigned int radio_nr[] = {[0 ... (SAA7134_MAXBOARDS - 1)] = UNSET };
MODULE_PARM(radio_nr,"1-" __stringify(SAA7134_MAXBOARDS) "i");
MODULE_PARM_DESC(radio_nr,"radio device number");

static unsigned int oss = 0;
MODULE_PARM(oss,"i");
MODULE_PARM_DESC(oss,"register oss devices (default: no)");

static unsigned int dsp_nr[] = {[0 ... (SAA7134_MAXBOARDS - 1)] = UNSET };
MODULE_PARM(dsp_nr,"1-" __stringify(SAA7134_MAXBOARDS) "i");
MODULE_PARM_DESC(dsp_nr,"oss dsp device number");

static unsigned int mixer_nr[] = {[0 ... (SAA7134_MAXBOARDS - 1)] = UNSET };
MODULE_PARM(mixer_nr,"1-" __stringify(SAA7134_MAXBOARDS) "i");
MODULE_PARM_DESC(mixer_nr,"oss mixer device number");

static unsigned int tuner[] = {[0 ... (SAA7134_MAXBOARDS - 1)] = UNSET };
MODULE_PARM(tuner,"1-" __stringify(SAA7134_MAXBOARDS) "i");
MODULE_PARM_DESC(tuner,"tuner type");

static unsigned int card[] = {[0 ... (SAA7134_MAXBOARDS - 1)] = UNSET };
MODULE_PARM(card,"1-" __stringify(SAA7134_MAXBOARDS) "i");
MODULE_PARM_DESC(card,"card type");

static unsigned int latency = UNSET;
MODULE_PARM(latency,"i");
MODULE_PARM_DESC(latency,"pci latency timer");

struct list_head  saa7134_devlist;
unsigned int      saa7134_devcount;

#define dprintk(fmt, arg...)	if (core_debug) \
	printk(KERN_DEBUG "%s/core: " fmt, dev->name , ## arg)

/* ------------------------------------------------------------------ */
/* debug help functions                                               */

static const char *v4l1_ioctls[] = {
	"0", "GCAP", "GCHAN", "SCHAN", "GTUNER", "STUNER", "GPICT", "SPICT",
	"CCAPTURE", "GWIN", "SWIN", "GFBUF", "SFBUF", "KEY", "GFREQ",
	"SFREQ", "GAUDIO", "SAUDIO", "SYNC", "MCAPTURE", "GMBUF", "GUNIT",
	"GCAPTURE", "SCAPTURE", "SPLAYMODE", "SWRITEMODE", "GPLAYINFO",
	"SMICROCODE", "GVBIFMT", "SVBIFMT" };
#define V4L1_IOCTLS ARRAY_SIZE(v4l1_ioctls)

static const char *v4l2_ioctls[] = {
	"QUERYCAP", "1", "ENUM_PIXFMT", "ENUM_FBUFFMT", "G_FMT", "S_FMT",
	"G_COMP", "S_COMP", "REQBUFS", "QUERYBUF", "G_FBUF", "S_FBUF",
	"G_WIN", "S_WIN", "PREVIEW", "QBUF", "16", "DQBUF", "STREAMON",
	"STREAMOFF", "G_PERF", "G_PARM", "S_PARM", "G_STD", "S_STD",
	"ENUMSTD", "ENUMINPUT", "G_CTRL", "S_CTRL", "G_TUNER", "S_TUNER",
	"G_FREQ", "S_FREQ", "G_AUDIO", "S_AUDIO", "35", "QUERYCTRL",
	"QUERYMENU", "G_INPUT", "S_INPUT", "ENUMCVT", "41", "42", "43",
	"44", "45",  "G_OUTPUT", "S_OUTPUT", "ENUMOUTPUT", "G_AUDOUT",
	"S_AUDOUT", "ENUMFX", "G_EFFECT", "S_EFFECT", "G_MODULATOR",
	"S_MODULATOR"
};
#define V4L2_IOCTLS ARRAY_SIZE(v4l2_ioctls)

static const char *osspcm_ioctls[] = {
	"RESET", "SYNC", "SPEED", "STEREO", "GETBLKSIZE", "SETFMT",
	"CHANNELS", "?", "POST", "SUBDIVIDE", "SETFRAGMENT", "GETFMTS",
	"GETOSPACE", "GETISPACE", "NONBLOCK", "GETCAPS", "GET/SETTRIGGER",
	"GETIPTR", "GETOPTR", "MAPINBUF", "MAPOUTBUF", "SETSYNCRO",
	"SETDUPLEX", "GETODELAY"
};
#define OSSPCM_IOCTLS ARRAY_SIZE(v4l2_ioctls)

void saa7134_print_ioctl(char *name, unsigned int cmd)
{
	char *dir;

	switch (_IOC_DIR(cmd)) {
	case _IOC_NONE:              dir = "--"; break;
	case _IOC_READ:              dir = "r-"; break;
	case _IOC_WRITE:             dir = "-w"; break;
	case _IOC_READ | _IOC_WRITE: dir = "rw"; break;
	default:                     dir = "??"; break;
	}
	switch (_IOC_TYPE(cmd)) {
	case 'v':
		printk(KERN_DEBUG "%s: ioctl 0x%08x (v4l1, %s, VIDIOC%s)\n",
		       name, cmd, dir, (_IOC_NR(cmd) < V4L1_IOCTLS) ?
		       v4l1_ioctls[_IOC_NR(cmd)] : "???");
		break;
	case 'V':
		printk(KERN_DEBUG "%s: ioctl 0x%08x (v4l2, %s, VIDIOC_%s)\n",
		       name, cmd, dir, (_IOC_NR(cmd) < V4L2_IOCTLS) ?
		       v4l2_ioctls[_IOC_NR(cmd)] : "???");
		break;
	case 'P':
		printk(KERN_DEBUG "%s: ioctl 0x%08x (oss dsp, %s, SNDCTL_DSP_%s)\n",
		       name, cmd, dir, (_IOC_NR(cmd) < OSSPCM_IOCTLS) ?
		       osspcm_ioctls[_IOC_NR(cmd)] : "???");
		break;
	case 'M':
		printk(KERN_DEBUG "%s: ioctl 0x%08x (oss mixer, %s, #%d)\n",
		       name, cmd, dir, _IOC_NR(cmd));
		break;
	default:
		printk(KERN_DEBUG "%s: ioctl 0x%08x (???, %s, #%d)\n",
		       name, cmd, dir, _IOC_NR(cmd));
	}
}

void saa7134_track_gpio(struct saa7134_dev *dev, char *msg)
{
	unsigned long mode,status;

	if (!gpio_tracking)
		return;
	/* rising SAA7134_GPIO_GPRESCAN reads the status */
	saa_andorb(SAA7134_GPIO_GPMODE3,SAA7134_GPIO_GPRESCAN,0);
	saa_andorb(SAA7134_GPIO_GPMODE3,SAA7134_GPIO_GPRESCAN,SAA7134_GPIO_GPRESCAN);
	mode   = saa_readl(SAA7134_GPIO_GPMODE0   >> 2) & 0xfffffff;
	status = saa_readl(SAA7134_GPIO_GPSTATUS0 >> 2) & 0xfffffff;
	printk(KERN_DEBUG
	       "%s: gpio: mode=0x%07lx in=0x%07lx out=0x%07lx [%s]\n",
	       dev->name, mode, (~mode) & status, mode & status, msg);
}

/* ------------------------------------------------------------------ */

#if 0
static char *dec1_bits[8] = {
	"DCSTD0", "DCSCT1", "WIPA", "GLIMB",
	"GLIMT", "SLTCA", "HLCK"
};
static char *dec2_bits[8] = {
	"RDCAP", "COPRO", "COLSTR", "TYPE3",
	NULL, "FIDT", "HLVLN", "INTL"
};
static char *scale1_bits[8] = {
	"VID_A", "VBI_A", NULL, NULL, "VID_B", "VBI_B"
};
static char *scale2_bits[8] = {
	"TRERR", "CFERR", "LDERR", "WASRST",
	"FIDSCI", "FIDSCO", "D6^D5", "TASK"
};

static void dump_statusreg(struct saa7134_dev *dev, int reg,
			   char *regname, char **bits)
{
	int value,i;

	value = saa_readb(reg);
	printk(KERN_DEBUG "%s: %s:", dev->name, regname);
	for (i = 7; i >= 0; i--) {
		if (NULL == bits[i])
			continue;
		printk(" %s=%d", bits[i], (value & (1 << i)) ? 1 : 0);
	}
	printk("\n");
}

static void dump_statusregs(struct saa7134_dev *dev)
{
	dump_statusreg(dev,SAA7134_STATUS_VIDEO1,"dec1",dec1_bits);
	dump_statusreg(dev,SAA7134_STATUS_VIDEO2,"dec2",dec2_bits);
	dump_statusreg(dev,SAA7134_SCALER_STATUS0,"scale0",scale1_bits);
	dump_statusreg(dev,SAA7134_SCALER_STATUS1,"scale1",scale2_bits);
}
#endif

/* ------------------------------------------------------------------ */

/* nr of (saa7134-)pages for the given buffer size */
int saa7134_buffer_pages(int size)
{
	size  = PAGE_ALIGN(size);
	size += PAGE_SIZE; /* for non-page-aligned buffers */
	size /= 4096;
	return size;
}

/* calc max # of buffers from size (must not exceed the 4MB virtual
 * address space per DMA channel) */
int saa7134_buffer_count(unsigned int size, unsigned int count)
{
	unsigned int maxcount;
	
	maxcount = 1024 / saa7134_buffer_pages(size);
	if (count > maxcount)
		count = maxcount;
	return count;
}

int saa7134_buffer_startpage(struct saa7134_buf *buf)
{
	return saa7134_buffer_pages(buf->vb.bsize) * buf->vb.i;
}

unsigned long saa7134_buffer_base(struct saa7134_buf *buf)
{
	unsigned long base;

	base  = saa7134_buffer_startpage(buf) * 4096;
	base += buf->vb.dma.sglist[0].offset;
	return base;
}

/* ------------------------------------------------------------------ */

int saa7134_pgtable_alloc(struct pci_dev *pci, struct saa7134_pgtable *pt)
{
        u32          *cpu;
        dma_addr_t   dma_addr;
	
	cpu = pci_alloc_consistent(pci, SAA7134_PGTABLE_SIZE, &dma_addr);
	if (NULL == cpu)
		return -ENOMEM;
	pt->size = SAA7134_PGTABLE_SIZE;
	pt->cpu  = cpu;
	pt->dma  = dma_addr;
	return 0;
}

int saa7134_pgtable_build(struct pci_dev *pci, struct saa7134_pgtable *pt,
			  struct scatterlist *list, unsigned int length,
			  unsigned int startpage)
{
	u32           *ptr;
	unsigned int  i,p;

	BUG_ON(NULL == pt || NULL == pt->cpu);

	ptr = pt->cpu + startpage;
	for (i = 0; i < length; i++, list++)
		for (p = 0; p * 4096 < list->length; p++, ptr++)
			*ptr = sg_dma_address(list) - list->offset;
	return 0;
}

void saa7134_pgtable_free(struct pci_dev *pci, struct saa7134_pgtable *pt)
{
	if (NULL == pt->cpu)
		return;
	pci_free_consistent(pci, pt->size, pt->cpu, pt->dma);
	pt->cpu = NULL;
}

/* ------------------------------------------------------------------ */

void saa7134_dma_free(struct saa7134_dev *dev,struct saa7134_buf *buf)
{
	if (in_interrupt())
		BUG();

	videobuf_waiton(&buf->vb,0,0);
	videobuf_dma_pci_unmap(dev->pci, &buf->vb.dma);
	videobuf_dma_free(&buf->vb.dma);
	buf->vb.state = STATE_NEEDS_INIT;
}

/* ------------------------------------------------------------------ */

int saa7134_buffer_queue(struct saa7134_dev *dev,
			 struct saa7134_dmaqueue *q,
			 struct saa7134_buf *buf)
{
	struct saa7134_buf *next = NULL;
#ifdef DEBUG_SPINLOCKS
	BUG_ON(!spin_is_locked(&dev->slock));
#endif
	
	dprintk("buffer_queue %p\n",buf);
	if (NULL == q->curr) {
		if (!q->need_two) {
			q->curr = buf;
			buf->activate(dev,buf,NULL);
		} else if (list_empty(&q->queue)) {
			list_add_tail(&buf->vb.queue,&q->queue);
			buf->vb.state = STATE_QUEUED;
		} else {
			next = list_entry(q->queue.next,struct saa7134_buf,
					  vb.queue);
			q->curr = buf;
			buf->activate(dev,buf,next);
		}
	} else {
		list_add_tail(&buf->vb.queue,&q->queue);
		buf->vb.state = STATE_QUEUED;
	}
	return 0;
}

void saa7134_buffer_finish(struct saa7134_dev *dev,
			   struct saa7134_dmaqueue *q,
			   unsigned int state)
{
#ifdef DEBUG_SPINLOCKS
	BUG_ON(!spin_is_locked(&dev->slock));
#endif
	dprintk("buffer_finish %p\n",q->curr);
	
	/* finish current buffer */
	q->curr->vb.state = state;
	do_gettimeofday(&q->curr->vb.ts);
	wake_up(&q->curr->vb.done);
	q->curr = NULL;
}

void saa7134_buffer_next(struct saa7134_dev *dev,
			 struct saa7134_dmaqueue *q)
{
	struct saa7134_buf *buf,*next = NULL;

#ifdef DEBUG_SPINLOCKS
	BUG_ON(!spin_is_locked(&dev->slock));
#endif
	BUG_ON(NULL != q->curr);

	if (!list_empty(&q->queue)) {
		/* activate next one from queue */
		buf = list_entry(q->queue.next,struct saa7134_buf,vb.queue);
		dprintk("buffer_next %p [prev=%p/next=%p]\n",
			buf,q->queue.prev,q->queue.next);
		list_del(&buf->vb.queue);
		if (!list_empty(&q->queue))
			next = list_entry(q->queue.next,struct saa7134_buf,
					  vb.queue);
		q->curr = buf;
		buf->activate(dev,buf,next);
		dprintk("buffer_next #2 prev=%p/next=%p\n",
			q->queue.prev,q->queue.next);
	} else {
		/* nothing to do -- just stop DMA */
		dprintk("buffer_next %p\n",NULL);
		saa7134_set_dmabits(dev);
		del_timer(&q->timeout);
	}
}

void saa7134_buffer_timeout(unsigned long data)
{
	struct saa7134_dmaqueue *q = (struct saa7134_dmaqueue*)data;
	struct saa7134_dev *dev = q->dev;
	unsigned long flags;

	spin_lock_irqsave(&dev->slock,flags);

	/* try to reset the hardware (SWRST) */
	saa_writeb(SAA7134_REGION_ENABLE, 0x00);
	saa_writeb(SAA7134_REGION_ENABLE, 0x80);
	saa_writeb(SAA7134_REGION_ENABLE, 0x00);

	/* flag current buffer as failed,
	   try to start over with the next one. */
	if (q->curr) {
		dprintk("timeout on %p\n",q->curr);
		saa7134_buffer_finish(dev,q,STATE_ERROR);
	}
	saa7134_buffer_next(dev,q);
	spin_unlock_irqrestore(&dev->slock,flags);
}

/* ------------------------------------------------------------------ */

int saa7134_set_dmabits(struct saa7134_dev *dev)
{
	u32 split, task=0, ctrl=0, irq=0;
	enum v4l2_field cap = V4L2_FIELD_ANY;
	enum v4l2_field ov  = V4L2_FIELD_ANY;

#ifdef DEBUG_SPINLOCKS
	BUG_ON(!spin_is_locked(&dev->slock));
#endif

	/* video capture -- dma 0 + video task A */
	if (dev->video_q.curr) {
		task |= 0x01;
		ctrl |= SAA7134_MAIN_CTRL_TE0;
		irq  |= SAA7134_IRQ1_INTE_RA0_1 |
			SAA7134_IRQ1_INTE_RA0_0;
	        cap = dev->video_q.curr->vb.field;
	}

	/* video capture -- dma 1+2 (planar modes) */
	if (dev->video_q.curr &&
	    dev->video_q.curr->fmt->planar) {
		ctrl |= SAA7134_MAIN_CTRL_TE4 |
			SAA7134_MAIN_CTRL_TE5;
	}

	/* screen overlay -- dma 0 + video task B */
	if (dev->ovenable) {
		task |= 0x10;
		ctrl |= SAA7134_MAIN_CTRL_TE1;
		ov = dev->ovfield;
	}

	/* vbi capture -- dma 0 + vbi task A+B */
	if (dev->vbi_q.curr) {
		task |= 0x22;
		ctrl |= SAA7134_MAIN_CTRL_TE2 |
			SAA7134_MAIN_CTRL_TE3;
		irq  |= SAA7134_IRQ1_INTE_RA0_7 |
			SAA7134_IRQ1_INTE_RA0_6 |
			SAA7134_IRQ1_INTE_RA0_5 |
			SAA7134_IRQ1_INTE_RA0_4;
	}

	/* audio capture -- dma 3 */
	if (dev->oss.dma_running) {
		ctrl |= SAA7134_MAIN_CTRL_TE6;
		irq  |= SAA7134_IRQ1_INTE_RA3_1 |
			SAA7134_IRQ1_INTE_RA3_0;
	}

	/* TS capture -- dma 5 */
	if (dev->ts_q.curr) {
		ctrl |= SAA7134_MAIN_CTRL_TE5;
		irq  |= SAA7134_IRQ1_INTE_RA2_3 |
			SAA7134_IRQ1_INTE_RA2_2 |
			SAA7134_IRQ1_INTE_RA2_1 |
			SAA7134_IRQ1_INTE_RA2_0;
	}
	
	/* set task conditions + field handling */
	if (V4L2_FIELD_HAS_BOTH(cap) || V4L2_FIELD_HAS_BOTH(ov) || cap == ov) {
		/* default config -- use full frames */
		saa_writeb(SAA7134_TASK_CONDITIONS(TASK_A), 0x0d);
		saa_writeb(SAA7134_TASK_CONDITIONS(TASK_B), 0x0d);
		saa_writeb(SAA7134_FIELD_HANDLING(TASK_A),  0x02);
		saa_writeb(SAA7134_FIELD_HANDLING(TASK_B),  0x02);
		split = 0;
	} else {
		/* split fields between tasks */
		if (V4L2_FIELD_TOP == cap) {
			/* odd A, even B, repeat */
			saa_writeb(SAA7134_TASK_CONDITIONS(TASK_A), 0x0d);
			saa_writeb(SAA7134_TASK_CONDITIONS(TASK_B), 0x0e);
		} else {
			/* odd B, even A, repeat */
			saa_writeb(SAA7134_TASK_CONDITIONS(TASK_A), 0x0e);
			saa_writeb(SAA7134_TASK_CONDITIONS(TASK_B), 0x0d);
		}
		saa_writeb(SAA7134_FIELD_HANDLING(TASK_A),  0x01);
		saa_writeb(SAA7134_FIELD_HANDLING(TASK_B),  0x01);
		split = 1;
	}
	
	/* irqs */
	saa_writeb(SAA7134_REGION_ENABLE, task);
	saa_writel(SAA7134_IRQ1,          irq);
	saa_andorl(SAA7134_MAIN_CTRL,
		   SAA7134_MAIN_CTRL_TE0 |
		   SAA7134_MAIN_CTRL_TE1 |
		   SAA7134_MAIN_CTRL_TE2 |
		   SAA7134_MAIN_CTRL_TE3 |
		   SAA7134_MAIN_CTRL_TE4 |
		   SAA7134_MAIN_CTRL_TE5 |
		   SAA7134_MAIN_CTRL_TE6,
		   ctrl);
	dprintk("dmabits: task=0x%02x ctrl=0x%02x irq=0x%x split=%s\n",
		task, ctrl, irq, split ? "no" : "yes");

	return 0;
}

/* ------------------------------------------------------------------ */
/* IRQ handler + helpers                                              */

static char *irqbits[] = {
	"DONE_RA0", "DONE_RA1", "DONE_RA2", "DONE_RA3",
	"AR", "PE", "PWR_ON", "RDCAP", "INTL", "FIDT", "MMC",
	"TRIG_ERR", "CONF_ERR", "LOAD_ERR",
	"GPIO16?", "GPIO18", "GPIO22", "GPIO23"
};
#define IRQBITS ARRAY_SIZE(irqbits)

static void print_irqstatus(struct saa7134_dev *dev, int loop,
			    unsigned long report, unsigned long status)
{
	unsigned int i;
	
	printk(KERN_DEBUG "%s/irq[%d,%ld]: r=0x%lx s=0x%02lx",
	       dev->name,loop,jiffies,report,status);
	for (i = 0; i < IRQBITS; i++) {
		if (!(report & (1 << i)))
			continue;
		printk(" %s",irqbits[i]);
	}
	if (report & SAA7134_IRQ_REPORT_DONE_RA0) {
		printk(" | RA0=%s,%s,%s,%ld",
		       (status & 0x40) ? "vbi"  : "video",
		       (status & 0x20) ? "b"    : "a",
		       (status & 0x10) ? "odd"  : "even",
		       (status & 0x0f));
	}
	printk("\n");
}

static irqreturn_t saa7134_irq(int irq, void *dev_id, struct pt_regs *regs)
{
	struct saa7134_dev *dev = (struct saa7134_dev*) dev_id;
	unsigned long report,status;
	int loop, handled = 0;

	for (loop = 0; loop < 10; loop++) {
		report = saa_readl(SAA7134_IRQ_REPORT);
		status = saa_readl(SAA7134_IRQ_STATUS);
		if (0 == report) {
			if (irq_debug > 1)
				printk(KERN_DEBUG "%s/irq: no (more) work\n",
				       dev->name);
			goto out;
		}
		handled = 1;
		saa_writel(SAA7134_IRQ_REPORT,report);
		if (irq_debug)
			print_irqstatus(dev,loop,report,status);

#if 0
		if (report & SAA7134_IRQ_REPORT_CONF_ERR)
			dump_statusregs(dev);
#endif

		if (report & SAA7134_IRQ_REPORT_INTL)
			saa7134_irq_video_intl(dev);

		if ((report & SAA7134_IRQ_REPORT_DONE_RA0) &&
		    (status & 0x60) == 0)
			saa7134_irq_video_done(dev,status);

		if ((report & SAA7134_IRQ_REPORT_DONE_RA0) &&
		    (status & 0x40) == 0x40)
			saa7134_irq_vbi_done(dev,status);

		if ((report & SAA7134_IRQ_REPORT_DONE_RA2) &&
		    card_has_ts(dev))
			saa7134_irq_ts_done(dev,status);

		if ((report & SAA7134_IRQ_REPORT_DONE_RA3))
			saa7134_irq_oss_done(dev,status);

		if ((report & (SAA7134_IRQ_REPORT_GPIO16 |
			       SAA7134_IRQ_REPORT_GPIO18)) &&
		    dev->remote)
			saa7134_input_irq(dev);

	};
	if (10 == loop) {
		print_irqstatus(dev,loop,report,status);
		if (report & SAA7134_IRQ_REPORT_PE) {
			/* disable all parity error */
			printk(KERN_WARNING "%s/irq: looping -- "
			       "clearing PE (parity error!) enable bit\n",dev->name);
			saa_clearl(SAA7134_IRQ2,SAA7134_IRQ2_INTE_PE);
		} else {
			/* disable all irqs */
			printk(KERN_WARNING "%s/irq: looping -- "
			       "clearing all enable bits\n",dev->name);
			saa_writel(SAA7134_IRQ1,0);
			saa_writel(SAA7134_IRQ2,0);
		}
	}

 out:
	return IRQ_RETVAL(handled);
}

/* ------------------------------------------------------------------ */

/* early init (no i2c, no irq) */
static int saa7134_hwinit1(struct saa7134_dev *dev)
{
	dprintk("hwinit1\n");

	saa_writel(SAA7134_IRQ1, 0);
	saa_writel(SAA7134_IRQ2, 0);
        init_MUTEX(&dev->lock);
	dev->slock = SPIN_LOCK_UNLOCKED;

	saa7134_track_gpio(dev,"pre-init");
	saa7134_video_init1(dev);
	saa7134_vbi_init1(dev);
	if (card_has_ts(dev))
		saa7134_ts_init1(dev);
	saa7134_input_init1(dev);

	switch (dev->pci->device) {
	case PCI_DEVICE_ID_PHILIPS_SAA7134:
	case PCI_DEVICE_ID_PHILIPS_SAA7133:
	case PCI_DEVICE_ID_PHILIPS_SAA7135:
		saa7134_oss_init1(dev);
		break;
	}
	
	/* RAM FIFO config */
	saa_writel(SAA7134_FIFO_SIZE, 0x08070503);
	saa_writel(SAA7134_THRESHOULD,0x02020202);
	
	/* enable audio + video processing */
	saa_writel(SAA7134_MAIN_CTRL,
		   SAA7134_MAIN_CTRL_VPLLE |
		   SAA7134_MAIN_CTRL_APLLE |
		   SAA7134_MAIN_CTRL_EXOSC |
		   SAA7134_MAIN_CTRL_EVFE1 |
		   SAA7134_MAIN_CTRL_EVFE2 |
		   SAA7134_MAIN_CTRL_ESFE  |
		   SAA7134_MAIN_CTRL_EBADC |
		   SAA7134_MAIN_CTRL_EBDAC);

	/* enable peripheral devices */
	saa_writeb(SAA7134_SPECIAL_MODE, 0x01);

	/* set vertical line numbering start (vbi needs this) */
	saa_writeb(SAA7134_SOURCE_TIMING2, 0x20);
	
	return 0;
}

/* late init (with i2c + irq) */
static int saa7134_hwinit2(struct saa7134_dev *dev)
{
	dprintk("hwinit2\n");

	saa7134_video_init2(dev);
	saa7134_tvaudio_init2(dev);

	/* enable IRQ's */
	saa_writel(SAA7134_IRQ1, 0);
	saa_writel(SAA7134_IRQ2,
		   SAA7134_IRQ2_INTE_GPIO18  |
		   SAA7134_IRQ2_INTE_GPIO18A |
		   SAA7134_IRQ2_INTE_GPIO16  |
		   SAA7134_IRQ2_INTE_SC2     |
		   SAA7134_IRQ2_INTE_SC1     |
		   SAA7134_IRQ2_INTE_SC0     |
		   /* SAA7134_IRQ2_INTE_DEC5    |  FIXME: TRIG_ERR ??? */
		   SAA7134_IRQ2_INTE_DEC3    |
		   SAA7134_IRQ2_INTE_DEC2    |
		   /* SAA7134_IRQ2_INTE_DEC1    | */
		   SAA7134_IRQ2_INTE_DEC0    |
		   SAA7134_IRQ2_INTE_PE      |
		   SAA7134_IRQ2_INTE_AR);

	return 0;
}

/* shutdown */
static int saa7134_hwfini(struct saa7134_dev *dev)
{
	dprintk("hwfini\n");

	switch (dev->pci->device) {
	case PCI_DEVICE_ID_PHILIPS_SAA7134:
	case PCI_DEVICE_ID_PHILIPS_SAA7133:
	case PCI_DEVICE_ID_PHILIPS_SAA7135:
		saa7134_oss_fini(dev);
		break;
	}
	if (card_has_ts(dev))
		saa7134_ts_fini(dev);
	saa7134_input_fini(dev);
	saa7134_vbi_fini(dev);
	saa7134_video_fini(dev);
	saa7134_tvaudio_fini(dev);
	return 0;
}

static void __devinit must_configure_manually(void)
{
	unsigned int i,p;

	printk(KERN_WARNING
	       "saa7134: <rant>\n"
	       "saa7134:  Congratulations!  Your TV card vendor saved a few\n"
	       "saa7134:  cents for a eeprom, thus your pci board has no\n"
	       "saa7134:  subsystem ID and I can't identify it automatically\n"
	       "saa7134: </rant>\n"
	       "saa7134: I feel better now.  Ok, here are the good news:\n"
	       "saa7134: You can use the card=<nr> insmod option to specify\n"
	       "saa7134: which board do you have.  The list:\n");
	for (i = 0; i < saa7134_bcount; i++) {
		printk(KERN_WARNING "saa7134:   card=%d -> %-40.40s",
		       i,saa7134_boards[i].name);
		for (p = 0; saa7134_pci_tbl[p].driver_data; p++) {
			if (saa7134_pci_tbl[p].driver_data != i)
				continue;
			printk(" %04x:%04x",
			       saa7134_pci_tbl[p].subvendor,
			       saa7134_pci_tbl[p].subdevice);
		}
		printk("\n");
	}
}

static struct video_device *vdev_init(struct saa7134_dev *dev,
				      struct video_device *template,
				      char *type)
{
	struct video_device *vfd;
	
	vfd = video_device_alloc();
	if (NULL == vfd)
		return NULL;
	*vfd = *template;
	vfd->minor   = -1;
	vfd->dev     = &dev->pci->dev;
	vfd->release = video_device_release;
	snprintf(vfd->name, sizeof(vfd->name), "%s %s (%s)",
		 dev->name, type, saa7134_boards[dev->board].name);
	return vfd;
}

static void saa7134_unregister_video(struct saa7134_dev *dev)
{
	if (dev->video_dev) {
		if (-1 != dev->video_dev->minor)
			video_unregister_device(dev->video_dev);
		else
			video_device_release(dev->video_dev);
		dev->video_dev = NULL;
	}
	if (dev->ts_dev) {
		if (-1 != dev->ts_dev->minor)
			video_unregister_device(dev->ts_dev);
		else
			video_device_release(dev->ts_dev);
		dev->ts_dev = NULL;
	}
	if (dev->vbi_dev) {
		if (-1 != dev->vbi_dev->minor)
			video_unregister_device(dev->vbi_dev);
		else
			video_device_release(dev->vbi_dev);
		dev->vbi_dev = NULL;
	}
	if (dev->radio_dev) {
		if (-1 != dev->radio_dev->minor)
			video_unregister_device(dev->radio_dev);
		else
			video_device_release(dev->radio_dev);
		dev->radio_dev = NULL;
	}
}

static int __devinit saa7134_initdev(struct pci_dev *pci_dev,
				     const struct pci_device_id *pci_id)
{
	struct saa7134_dev *dev;
	int err;

	dev = kmalloc(sizeof(*dev),GFP_KERNEL);
	if (NULL == dev)
		return -ENOMEM;
	memset(dev,0,sizeof(*dev));

	/* pci init */
	dev->pci = pci_dev;
	if (pci_enable_device(pci_dev)) {
		err = -EIO;
		goto fail1;
	}
	sprintf(dev->name,"saa%x[%d]",pci_dev->device,saa7134_devcount);

	/* pci quirks */
	if (pci_pci_problems) {
		if (pci_pci_problems & PCIPCI_TRITON)
			printk(KERN_INFO "%s: quirk: PCIPCI_TRITON\n", dev->name);
		if (pci_pci_problems & PCIPCI_NATOMA)
			printk(KERN_INFO "%s: quirk: PCIPCI_NATOMA\n", dev->name);
		if (pci_pci_problems & PCIPCI_VIAETBF)
			printk(KERN_INFO "%s: quirk: PCIPCI_VIAETBF\n", dev->name);
		if (pci_pci_problems & PCIPCI_VSFX)
			printk(KERN_INFO "%s: quirk: PCIPCI_VSFX\n",dev->name);
#ifdef PCIPCI_ALIMAGIK
		if (pci_pci_problems & PCIPCI_ALIMAGIK) {
			printk(KERN_INFO "%s: quirk: PCIPCI_ALIMAGIK -- latency fixup\n",
			       dev->name);
			latency = 0x0A;
		}
#endif
	}
	if (UNSET != latency) {
		printk(KERN_INFO "%s: setting pci latency timer to %d\n",
		       dev->name,latency);
		pci_write_config_byte(pci_dev, PCI_LATENCY_TIMER, latency);
	}

	/* print pci info */
	pci_read_config_byte(pci_dev, PCI_CLASS_REVISION, &dev->pci_rev);
        pci_read_config_byte(pci_dev, PCI_LATENCY_TIMER,  &dev->pci_lat);
        printk(KERN_INFO "%s: found at %s, rev: %d, irq: %d, "
	       "latency: %d, mmio: 0x%lx\n", dev->name,
	       pci_name(pci_dev), dev->pci_rev, pci_dev->irq,
	       dev->pci_lat,pci_resource_start(pci_dev,0));
	pci_set_master(pci_dev);
	if (!pci_dma_supported(pci_dev,0xffffffff)) {
		printk("%s: Oops: no 32bit PCI DMA ???\n",dev->name);
		err = -EIO;
		goto fail1;
	}

	/* board config */
	dev->board = pci_id->driver_data;
	if (card[saa7134_devcount] >= 0 &&
	    card[saa7134_devcount] < saa7134_bcount)
		dev->board = card[saa7134_devcount];
	if (SAA7134_BOARD_NOAUTO == dev->board) {
		must_configure_manually();
		dev->board = SAA7134_BOARD_UNKNOWN;
	}
	dev->tuner_type = saa7134_boards[dev->board].tuner_type;
	if (UNSET != tuner[saa7134_devcount])
		dev->tuner_type = tuner[saa7134_devcount];
        printk(KERN_INFO "%s: subsystem: %04x:%04x, board: %s [card=%d,%s]\n",
	       dev->name,pci_dev->subsystem_vendor,
	       pci_dev->subsystem_device,saa7134_boards[dev->board].name,
	       dev->board, card[saa7134_devcount] == dev->board ?
	       "insmod option" : "autodetected");

	/* get mmio */
	if (!request_mem_region(pci_resource_start(pci_dev,0),
				pci_resource_len(pci_dev,0),
				dev->name)) {
		err = -EBUSY;
		printk(KERN_ERR "%s: can't get MMIO memory @ 0x%lx\n",
		       dev->name,pci_resource_start(pci_dev,0));
		goto fail1;
	}
	dev->lmmio = ioremap(pci_resource_start(pci_dev,0), 0x1000);
	dev->bmmio = (__u8*)dev->lmmio;
	if (NULL == dev->lmmio) {
		err = -EIO;
		printk(KERN_ERR "%s: can't ioremap() MMIO memory\n",
		       dev->name);
		goto fail2;
	}

	/* initialize hardware #1 */
	saa7134_board_init(dev);
	saa7134_hwinit1(dev);

	/* get irq */
	err = request_irq(pci_dev->irq, saa7134_irq,
			  SA_SHIRQ | SA_INTERRUPT, dev->name, dev);
	if (err < 0) {
		printk(KERN_ERR "%s: can't get IRQ %d\n",
		       dev->name,pci_dev->irq);
		goto fail3;
	}

	/* wait a bit, register i2c bus */
	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout(HZ/10);
	saa7134_i2c_register(dev);

	/* initialize hardware #2 */
	saa7134_hwinit2(dev);

	/* load i2c helpers */
	if (TUNER_ABSENT != dev->tuner_type)
		request_module("tuner");
	if (saa7134_boards[dev->board].need_tda9887)
		request_module("tda9887");
  	if (card_has_ts(dev))
		request_module("saa6752hs");

#ifdef VIDIOC_G_PRIORITY
	v4l2_prio_init(&dev->prio);
#endif

	/* register v4l devices */
	dev->video_dev = vdev_init(dev,&saa7134_video_template,"video");
	err = video_register_device(dev->video_dev,VFL_TYPE_GRABBER,
				    video_nr[saa7134_devcount]);
	if (err < 0) {
		printk(KERN_INFO "%s: can't register video device\n",
		       dev->name);
		goto fail4;
	}
	printk(KERN_INFO "%s: registered device video%d [v4l2]\n",
	       dev->name,dev->video_dev->minor & 0x1f);

	if (card_has_ts(dev)) {
		dev->ts_dev = vdev_init(dev,&saa7134_ts_template,"ts");
		err = video_register_device(dev->ts_dev,VFL_TYPE_GRABBER,
					    ts_nr[saa7134_devcount]);
		if (err < 0) {
			printk(KERN_INFO "%s: can't register video device\n",
			       dev->name);
			goto fail4;
		}
		printk(KERN_INFO "%s: registered device video%d [ts]\n",
		       dev->name,dev->ts_dev->minor & 0x1f);
	}
	
	dev->vbi_dev = vdev_init(dev,&saa7134_vbi_template,"vbi");
	err = video_register_device(dev->vbi_dev,VFL_TYPE_VBI,
				    vbi_nr[saa7134_devcount]);
	if (err < 0)
		goto fail4;
	printk(KERN_INFO "%s: registered device vbi%d\n",
	       dev->name,dev->vbi_dev->minor & 0x1f);

	if (card_has_radio(dev)) {
		dev->radio_dev = vdev_init(dev,&saa7134_radio_template,"radio");
		err = video_register_device(dev->radio_dev,VFL_TYPE_RADIO,
					    radio_nr[saa7134_devcount]);
		if (err < 0)
			goto fail4;
		printk(KERN_INFO "%s: registered device radio%d\n",
		       dev->name,dev->radio_dev->minor & 0x1f);
	}

	/* register oss devices */
	switch (dev->pci->device) {
	case PCI_DEVICE_ID_PHILIPS_SAA7134:
	case PCI_DEVICE_ID_PHILIPS_SAA7133:
	case PCI_DEVICE_ID_PHILIPS_SAA7135:
		if (oss) {
			err = dev->oss.minor_dsp =
				register_sound_dsp(&saa7134_dsp_fops,
						   dsp_nr[saa7134_devcount]);
			if (err < 0) {
				goto fail4;
			}
			printk(KERN_INFO "%s: registered device dsp%d\n",
			       dev->name,dev->oss.minor_dsp >> 4);
			
			err = dev->oss.minor_mixer =
				register_sound_mixer(&saa7134_mixer_fops,
						     mixer_nr[saa7134_devcount]);
			if (err < 0)
				goto fail5;
			printk(KERN_INFO "%s: registered device mixer%d\n",
			       dev->name,dev->oss.minor_mixer >> 4);
		}
		break;
	}

	/* everything worked */
	list_add_tail(&dev->devlist,&saa7134_devlist);
	pci_set_drvdata(pci_dev,dev);
	saa7134_devcount++;
	return 0;

 fail5:
	switch (dev->pci->device) {
	case PCI_DEVICE_ID_PHILIPS_SAA7134:
	case PCI_DEVICE_ID_PHILIPS_SAA7133:
	case PCI_DEVICE_ID_PHILIPS_SAA7135:
		if (oss)
			unregister_sound_dsp(dev->oss.minor_dsp);
		break;
	}
 fail4:
	saa7134_unregister_video(dev);
	saa7134_i2c_unregister(dev);
	free_irq(pci_dev->irq, dev);
 fail3:
	saa7134_hwfini(dev);
	iounmap(dev->lmmio);
 fail2:
	release_mem_region(pci_resource_start(pci_dev,0),
			   pci_resource_len(pci_dev,0));
 fail1:
	kfree(dev);
	return err;
}

static void __devexit saa7134_finidev(struct pci_dev *pci_dev)
{
        struct saa7134_dev *dev = pci_get_drvdata(pci_dev);

	/* debugging ... */
	if (irq_debug) {
		u32 report = saa_readl(SAA7134_IRQ_REPORT);
		u32 status = saa_readl(SAA7134_IRQ_STATUS);
		print_irqstatus(dev,42,report,status);
	}

	/* disable peripheral devices */
	saa_writeb(SAA7134_SPECIAL_MODE,0);

	/* shutdown hardware */
	saa_writel(SAA7134_IRQ1,0);
	saa_writel(SAA7134_IRQ2,0);
	saa_writel(SAA7134_MAIN_CTRL,0);

	/* shutdown subsystems */
	saa7134_hwfini(dev);

	/* unregister */
	saa7134_i2c_unregister(dev);
	switch (dev->pci->device) {
	case PCI_DEVICE_ID_PHILIPS_SAA7134:
	case PCI_DEVICE_ID_PHILIPS_SAA7133:
	case PCI_DEVICE_ID_PHILIPS_SAA7135:
		if (oss) {
			unregister_sound_mixer(dev->oss.minor_mixer);
			unregister_sound_dsp(dev->oss.minor_dsp);
		}
		break;
	}
	saa7134_unregister_video(dev);

	/* release ressources */
	free_irq(pci_dev->irq, dev);
	iounmap(dev->lmmio);
	release_mem_region(pci_resource_start(pci_dev,0),
			   pci_resource_len(pci_dev,0));

#if 0  /* causes some trouble when reinserting the driver ... */
	pci_disable_device(pci_dev);
#endif
	pci_set_drvdata(pci_dev, NULL);

	/* free memory */
	list_del(&dev->devlist);
	saa7134_devcount--;
	kfree(dev);
}

static struct pci_driver saa7134_pci_driver = {
        .name     = "saa7134",
        .id_table = saa7134_pci_tbl,
        .probe    = saa7134_initdev,
        .remove   = saa7134_finidev,
};

static int saa7134_init(void)
{
	INIT_LIST_HEAD(&saa7134_devlist);
	printk(KERN_INFO "saa7130/34: v4l2 driver version %d.%d.%d loaded\n",
	       (SAA7134_VERSION_CODE >> 16) & 0xff,
	       (SAA7134_VERSION_CODE >>  8) & 0xff,
	       SAA7134_VERSION_CODE & 0xff);
#ifdef SNAPSHOT
	printk(KERN_INFO "saa7130/34: snapshot date %04d-%02d-%02d\n",
	       SNAPSHOT/10000, (SNAPSHOT/100)%100, SNAPSHOT%100);
#endif
	return pci_module_init(&saa7134_pci_driver);
}

static void saa7134_fini(void)
{
	pci_unregister_driver(&saa7134_pci_driver);
}

module_init(saa7134_init);
module_exit(saa7134_fini);

/* ----------------------------------------------------------- */
/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
