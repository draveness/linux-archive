/*
 * Support for VIA 82Cxxx Audio Codecs
 * Copyright 1999,2000 Jeff Garzik <jgarzik@mandrakesoft.com>
 *
 * Distributed under the GNU GENERAL PUBLIC LICENSE (GPL) Version 2.
 * See the "COPYING" file distributed with this software for more info.
 *
 * For a list of known bugs (errata) and documentation,
 * see via-audio.pdf in linux/Documentation/DocBook.
 * If this documentation does not exist, run "make pdfdocs".
 * If "make pdfdocs" fails, obtain the documentation from
 * the driver's Website at
 * http://gtf.org/garzik/drivers/via82cxxx/
 *
 */


#define VIA_VERSION	"1.1.14"


#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/spinlock.h>
#include <linux/sound.h>
#include <linux/poll.h>
#include <linux/soundcard.h>
#include <linux/ac97_codec.h>
#include <linux/smp_lock.h>
#include <linux/ioport.h>
#include <linux/wrapper.h>
#include <asm/io.h>
#include <asm/delay.h>
#include <asm/uaccess.h>
#include <asm/hardirq.h>
#include <asm/semaphore.h>


#undef VIA_DEBUG	/* define to enable debugging output and checks */
#ifdef VIA_DEBUG
/* note: prints function name for you */
#define DPRINTK(fmt, args...) printk(KERN_DEBUG "%s: " fmt, __FUNCTION__ , ## args)
#else
#define DPRINTK(fmt, args...)
#endif

#undef VIA_NDEBUG	/* define to disable lightweight runtime checks */
#ifdef VIA_NDEBUG
#define assert(expr)
#else
#define assert(expr) \
        if(!(expr)) {					\
        printk( "Assertion failed! %s,%s,%s,line=%d\n",	\
        #expr,__FILE__,__FUNCTION__,__LINE__);		\
        }
#endif

#if defined(CONFIG_PROC_FS) && \
    defined(CONFIG_SOUND_VIA82CXXX_PROCFS)
#define VIA_PROC_FS 1
#endif

#define VIA_SUPPORT_MMAP 1 /* buggy, for now... */

#define MAX_CARDS	1

#define VIA_CARD_NAME	"VIA 82Cxxx Audio driver " VIA_VERSION
#define VIA_MODULE_NAME "via82cxxx"
#define PFX		VIA_MODULE_NAME ": "

#define VIA_COUNTER_LIMIT	100000

/* size of DMA buffers */
#define VIA_DMA_BUFFERS		16
#define VIA_DMA_BUF_SIZE	PAGE_SIZE

#ifndef AC97_PCM_LR_ADC_RATE
#  define AC97_PCM_LR_ADC_RATE AC97_PCM_LR_DAC_RATE
#endif

/* 82C686 function 5 (audio codec) PCI configuration registers */
#define VIA_ACLINK_CTRL		0x41
#define VIA_FUNC_ENABLE		0x42
#define VIA_PNP_CONTROL		0x43
#define VIA_FM_NMI_CTRL		0x48

/*
 * controller base 0 (scatter-gather) registers
 *
 * NOTE: Via datasheet lists first channel as "read"
 * channel and second channel as "write" channel.
 * I changed the naming of the constants to be more
 * clear than I felt the datasheet to be.
 */

#define VIA_BASE0_PCM_OUT_CHAN	0x00 /* output PCM to user */
#define VIA_BASE0_PCM_OUT_CHAN_STATUS 0x00
#define VIA_BASE0_PCM_OUT_CHAN_CTRL	0x01
#define VIA_BASE0_PCM_OUT_CHAN_TYPE	0x02
#define VIA_BASE0_PCM_OUT_BLOCK_COUNT	0x0C

#define VIA_BASE0_PCM_IN_CHAN		0x10 /* input PCM from user */
#define VIA_BASE0_PCM_IN_CHAN_STATUS	0x10
#define VIA_BASE0_PCM_IN_CHAN_CTRL	0x11
#define VIA_BASE0_PCM_IN_CHAN_TYPE	0x12

/* offsets from base */
#define VIA_PCM_STATUS			0x00
#define VIA_PCM_CONTROL			0x01
#define VIA_PCM_TYPE			0x02
#define VIA_PCM_TABLE_ADDR		0x04

/* XXX unused DMA channel for FM PCM data */
#define VIA_BASE0_FM_OUT_CHAN		0x20
#define VIA_BASE0_FM_OUT_CHAN_STATUS	0x20
#define VIA_BASE0_FM_OUT_CHAN_CTRL	0x21
#define VIA_BASE0_FM_OUT_CHAN_TYPE	0x22

#define VIA_BASE0_AC97_CTRL		0x80
#define VIA_BASE0_SGD_STATUS_SHADOW	0x84
#define VIA_BASE0_GPI_INT_ENABLE	0x8C
#define VIA_INTR_OUT			((1<<0) |  (1<<4) |  (1<<8))
#define VIA_INTR_IN			((1<<1) |  (1<<5) |  (1<<9))
#define VIA_INTR_FM			((1<<2) |  (1<<6) | (1<<10))
#define VIA_INTR_MASK		(VIA_INTR_OUT | VIA_INTR_IN | VIA_INTR_FM)

/* VIA_BASE0_AUDIO_xxx_CHAN_TYPE bits */
#define VIA_IRQ_ON_FLAG			(1<<0)	/* int on each flagged scatter block */
#define VIA_IRQ_ON_EOL			(1<<1)	/* int at end of scatter list */
#define VIA_INT_SEL_PCI_LAST_LINE_READ	(0)	/* int at PCI read of last line */
#define VIA_INT_SEL_LAST_SAMPLE_SENT	(1<<2)	/* int at last sample sent */
#define VIA_INT_SEL_ONE_LINE_LEFT	(1<<3)	/* int at less than one line to send */
#define VIA_PCM_FMT_STEREO		(1<<4)	/* PCM stereo format (bit clear == mono) */
#define VIA_PCM_FMT_16BIT		(1<<5)	/* PCM 16-bit format (bit clear == 8-bit) */
#define VIA_PCM_REC_FIFO		(1<<6)	/* PCM Recording FIFO */
#define VIA_RESTART_SGD_ON_EOL		(1<<7)	/* restart scatter-gather at EOL */
#define VIA_PCM_FMT_MASK		(VIA_PCM_FMT_STEREO|VIA_PCM_FMT_16BIT)
#define VIA_CHAN_TYPE_MASK		(VIA_RESTART_SGD_ON_EOL | \
					 VIA_IRQ_ON_FLAG | \
					 VIA_IRQ_ON_EOL)
#define VIA_CHAN_TYPE_INT_SELECT	(VIA_INT_SEL_LAST_SAMPLE_SENT)

/* PCI configuration register bits and masks */
#define VIA_CR40_AC97_READY	0x01
#define VIA_CR40_AC97_LOW_POWER	0x02
#define VIA_CR40_SECONDARY_READY 0x04

#define VIA_CR41_AC97_ENABLE	0x80 /* enable AC97 codec */
#define VIA_CR41_AC97_RESET	0x40 /* clear bit to reset AC97 */
#define VIA_CR41_AC97_WAKEUP	0x20 /* wake up from power-down mode */
#define VIA_CR41_AC97_SDO	0x10 /* force Serial Data Out (SDO) high */
#define VIA_CR41_VRA		0x08 /* enable variable sample rate */
#define VIA_CR41_PCM_ENABLE	0x04 /* AC Link SGD Read Channel PCM Data Output */
#define VIA_CR41_FM_PCM_ENABLE	0x02 /* AC Link FM Channel PCM Data Out */
#define VIA_CR41_SB_PCM_ENABLE	0x01 /* AC Link SB PCM Data Output */
#define VIA_CR41_BOOT_MASK	(VIA_CR41_AC97_ENABLE | \
				 VIA_CR41_AC97_WAKEUP | \
				 VIA_CR41_AC97_SDO)
#define VIA_CR41_RUN_MASK	(VIA_CR41_AC97_ENABLE | \
				 VIA_CR41_AC97_RESET | \
				 VIA_CR41_VRA | \
				 VIA_CR41_PCM_ENABLE)

#define VIA_CR42_SB_ENABLE	0x01
#define VIA_CR42_MIDI_ENABLE	0x02
#define VIA_CR42_FM_ENABLE	0x04
#define VIA_CR42_GAME_ENABLE	0x08

#define VIA_CR44_SECOND_CODEC_SUPPORT	(1 << 6)
#define VIA_CR44_AC_LINK_ACCESS		(1 << 7)

#define VIA_CR48_FM_TRAP_TO_NMI		(1 << 2)

/* controller base 0 register bitmasks */
#define VIA_INT_DISABLE_MASK		(~(0x01|0x02))
#define VIA_SGD_STOPPED			(1 << 2)
#define VIA_SGD_ACTIVE			(1 << 7)
#define VIA_SGD_TERMINATE		(1 << 6)
#define VIA_SGD_FLAG			(1 << 0)
#define VIA_SGD_EOL			(1 << 1)
#define VIA_SGD_START			(1 << 7)

#define VIA_CR80_FIRST_CODEC		0
#define VIA_CR80_SECOND_CODEC		(1 << 30)
#define VIA_CR80_FIRST_CODEC_VALID	(1 << 25)
#define VIA_CR80_VALID			(1 << 25)
#define VIA_CR80_SECOND_CODEC_VALID	(1 << 27)
#define VIA_CR80_BUSY			(1 << 24)
#define VIA_CR83_BUSY			(1)
#define VIA_CR83_FIRST_CODEC_VALID	(1 << 1)
#define VIA_CR80_READ			(1 << 23)
#define VIA_CR80_WRITE_MODE		0
#define VIA_CR80_REG_IDX(idx)		((((idx) & 0xFF) >> 1) << 16)

/* capabilities we announce */
#ifdef VIA_SUPPORT_MMAP
#define VIA_DSP_CAP (DSP_CAP_REVISION | DSP_CAP_DUPLEX | DSP_CAP_MMAP | \
		     DSP_CAP_TRIGGER | DSP_CAP_REALTIME)
#else
#define VIA_DSP_CAP (DSP_CAP_REVISION | DSP_CAP_DUPLEX | \
		     DSP_CAP_TRIGGER | DSP_CAP_REALTIME)
#endif

/* scatter-gather DMA table entry, exactly as passed to hardware */
struct via_sgd_table {
	u32 addr;
	u32 count;	/* includes additional VIA_xxx bits also */
};

#define VIA_EOL (1 << 31)
#define VIA_FLAG (1 << 30)
#define VIA_STOP (1 << 29)


enum via_channel_states {
	sgd_stopped = 0,
	sgd_in_progress = 1,
};


struct via_sgd_data {
	dma_addr_t handle;
	void *cpuaddr;
};


struct via_channel {
	atomic_t n_bufs;
	atomic_t hw_ptr;
	wait_queue_head_t wait;

	unsigned int sw_ptr;
	unsigned int slop_len;
	unsigned int n_irqs;
	int bytes;

	unsigned is_active : 1;
	unsigned is_record : 1;
	unsigned is_mapped : 1;
	unsigned is_enabled : 1;
	u8 pcm_fmt;		/* VIA_PCM_FMT_xxx */

	unsigned rate;		/* sample rate */

	volatile struct via_sgd_table *sgtable;
	dma_addr_t sgt_handle;

	struct via_sgd_data sgbuf [VIA_DMA_BUFFERS];

	long iobase;

	const char *name;
};


/* data stored for each chip */
struct via_info {
	struct pci_dev *pdev;
	long baseaddr;

	struct ac97_codec ac97;
	spinlock_t lock;
	int card_num;		/* unique card number, from 0 */

	int dev_dsp;		/* /dev/dsp index from register_sound_dsp() */

	unsigned rev_h : 1;

	struct semaphore syscall_sem;
	struct semaphore open_sem;

	struct via_channel ch_in;
	struct via_channel ch_out;
	struct via_channel ch_fm;
};


/* number of cards, used for assigning unique numbers to cards */
static unsigned via_num_cards = 0;



/****************************************************************
 *
 * prototypes
 *
 *
 */

static int via_init_one (struct pci_dev *dev, const struct pci_device_id *id);
static void via_remove_one (struct pci_dev *pdev);

static ssize_t via_dsp_read(struct file *file, char *buffer, size_t count, loff_t *ppos);
static ssize_t via_dsp_write(struct file *file, const char *buffer, size_t count, loff_t *ppos);
static unsigned int via_dsp_poll(struct file *file, struct poll_table_struct *wait);
static int via_dsp_ioctl (struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg);
static int via_dsp_open (struct inode *inode, struct file *file);
static int via_dsp_release(struct inode *inode, struct file *file);
#ifdef VIA_SUPPORT_MMAP
static int via_dsp_mmap(struct file *file, struct vm_area_struct *vma);
#endif

static u16 via_ac97_read_reg (struct ac97_codec *codec, u8 reg);
static void via_ac97_write_reg (struct ac97_codec *codec, u8 reg, u16 value);
static u8 via_ac97_wait_idle (struct via_info *card);

static void via_chan_free (struct via_info *card, struct via_channel *chan);
static void via_chan_clear (struct via_channel *chan);
static void via_chan_pcm_fmt (struct via_channel *chan, int reset);

#ifdef VIA_PROC_FS
static int via_init_proc (void);
static void via_cleanup_proc (void);
static int via_card_init_proc (struct via_info *card);
static void via_card_cleanup_proc (struct via_info *card);
#else
static inline int via_init_proc (void) { return 0; }
static inline void via_cleanup_proc (void) {}
static inline int via_card_init_proc (struct via_info *card) { return 0; }
static inline void via_card_cleanup_proc (struct via_info *card) {}
#endif


/****************************************************************
 *
 * Various data the driver needs
 *
 *
 */


static struct pci_device_id via_pci_tbl[] __initdata = {
	{ PCI_VENDOR_ID_VIA, PCI_DEVICE_ID_VIA_82C686_5, PCI_ANY_ID, PCI_ANY_ID, },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci,via_pci_tbl);


static struct pci_driver via_driver = {
	name:		VIA_MODULE_NAME,
	id_table:	via_pci_tbl,
	probe:		via_init_one,
	remove:		via_remove_one,
};


/****************************************************************
 *
 * Low-level base 0 register read/write helpers
 *
 *
 */

/**
 *	via_chan_stop - Terminate DMA on specified PCM channel
 *	@iobase: PCI base address for SGD channel registers
 *
 *	Terminate scatter-gather DMA operation for given
 *	channel (derived from @iobase), if DMA is active.
 *
 *	Note that @iobase is not the PCI base address,
 *	but the PCI base address plus an offset to
 *	one of three PCM channels supported by the chip.
 *
 */

static inline void via_chan_stop (int iobase)
{
	if (inb (iobase + VIA_PCM_STATUS) & VIA_SGD_ACTIVE)
		outb (VIA_SGD_TERMINATE, iobase + VIA_PCM_CONTROL);
}


/**
 *	via_chan_status_clear - Clear status flags on specified DMA channel
 *	@iobase: PCI base address for SGD channel registers
 *
 *	Clear any pending status flags for the given
 *	DMA channel (derived from @iobase), if any
 *	flags are asserted.
 *
 *	Note that @iobase is not the PCI base address,
 *	but the PCI base address plus an offset to
 *	one of three PCM channels supported by the chip.
 *
 */

static inline void via_chan_status_clear (int iobase)
{
	u8 tmp = inb (iobase + VIA_PCM_STATUS);

	if (tmp != 0)
		outb (tmp, iobase + VIA_PCM_STATUS);
}


/**
 *	sg_begin - Begin recording or playback on a PCM channel
 *	@chan: Channel for which DMA operation shall begin
 *
 *	Start scatter-gather DMA for the given channel.
 *
 */

static inline void sg_begin (struct via_channel *chan)
{
	outb (VIA_SGD_START, chan->iobase + VIA_PCM_CONTROL);
}


/****************************************************************
 *
 * Miscellaneous debris
 *
 *
 */


/**
 *	via_syscall_down - down the card-specific syscell semaphore
 *	@card: Private info for specified board
 *	@nonblock: boolean, non-zero if O_NONBLOCK is set
 *
 *	Encapsulates standard method of acquiring the syscall sem.
 *
 *	Returns negative errno on error, or zero for success.
 */

static inline int via_syscall_down (struct via_info *card, int nonblock)
{
	if (nonblock) {
		if (down_trylock (&card->syscall_sem))
			return -EAGAIN;
	} else {
		if (down_interruptible (&card->syscall_sem))
			return -ERESTARTSYS;
	}

	return 0;
}


/**
 *	via_stop_everything - Stop all audio operations
 *	@card: Private info for specified board
 *
 *	Stops all DMA operations and interrupts, and clear
 *	any pending status bits resulting from those operations.
 */

static void via_stop_everything (struct via_info *card)
{
	DPRINTK ("ENTER\n");

	assert (card != NULL);

	/*
	 * terminate any existing operations on audio read/write channels
	 */
	via_chan_stop (card->baseaddr + VIA_BASE0_PCM_OUT_CHAN);
	via_chan_stop (card->baseaddr + VIA_BASE0_PCM_IN_CHAN);
	via_chan_stop (card->baseaddr + VIA_BASE0_FM_OUT_CHAN);

	/*
	 * clear any existing stops / flags (sanity check mainly)
	 */
	via_chan_status_clear (card->baseaddr + VIA_BASE0_PCM_OUT_CHAN);
	via_chan_status_clear (card->baseaddr + VIA_BASE0_PCM_IN_CHAN);
	via_chan_status_clear (card->baseaddr + VIA_BASE0_FM_OUT_CHAN);

	/*
	 * clear any enabled interrupt bits, reset to 8-bit mono PCM mode
	 */
	outb (0, card->baseaddr + VIA_BASE0_PCM_OUT_CHAN_TYPE);
	outb (0, card->baseaddr + VIA_BASE0_PCM_IN_CHAN_TYPE);
	outb (0, card->baseaddr + VIA_BASE0_FM_OUT_CHAN_TYPE);
	DPRINTK ("EXIT\n");
}


/**
 *	via_set_rate - Set PCM rate for given channel
 *	@ac97: Pointer to generic codec info struct
 *	@chan: Private info for specified channel
 *	@rate: Desired PCM sample rate, in Khz
 *
 *	Sets the PCM sample rate for a channel.
 *
 *	Values for @rate are clamped to a range of 4000 Khz through 48000 Khz,
 *	due to hardware constraints.
 */

static int via_set_rate (struct ac97_codec *ac97,
			 struct via_channel *chan, unsigned rate)
{
	int rate_reg;

	DPRINTK ("ENTER, rate = %d\n", rate);

	if (rate > 48000)		rate = 48000;
	if (rate < 4000) 		rate = 4000;

	rate_reg = chan->is_record ? AC97_PCM_LR_ADC_RATE :
			    AC97_PCM_FRONT_DAC_RATE;

	via_ac97_write_reg (ac97, AC97_POWER_CONTROL,
		(via_ac97_read_reg (ac97, AC97_POWER_CONTROL) & ~0x0200) |
		0x0200);

	via_ac97_write_reg (ac97, rate_reg, rate);

	via_ac97_write_reg (ac97, AC97_POWER_CONTROL,
		via_ac97_read_reg (ac97, AC97_POWER_CONTROL) & ~0x0200);

	udelay (10);

	/* the hardware might return a value different than what we
	 * passed to it, so read the rate value back from hardware
	 * to see what we came up with
	 */
	chan->rate = via_ac97_read_reg (ac97, rate_reg);

	DPRINTK ("EXIT, returning rate %d Hz\n", chan->rate);
	return chan->rate;
}


/****************************************************************
 *
 * Channel-specific operations
 *
 *
 */


/**
 *	via_chan_init_defaults - Initialize a struct via_channel
 *	@card: Private audio chip info
 *	@chan: Channel to be initialized
 *
 *	Zero @chan, and then set all static defaults for the structure.
 */

static void via_chan_init_defaults (struct via_info *card, struct via_channel *chan)
{
	memset (chan, 0, sizeof (*chan));

	if (chan == &card->ch_out) {
		chan->name = "PCM-OUT";
		chan->iobase = card->baseaddr + VIA_BASE0_PCM_OUT_CHAN;
	} else if (chan == &card->ch_in) {
		chan->name = "PCM-IN";
		chan->iobase = card->baseaddr + VIA_BASE0_PCM_IN_CHAN;
		chan->is_record = 1;
	} else if (chan == &card->ch_fm) {
		chan->name = "PCM-OUT-FM";
		chan->iobase = card->baseaddr + VIA_BASE0_FM_OUT_CHAN;
	} else {
		BUG();
	}

	init_waitqueue_head (&chan->wait);

	chan->pcm_fmt = VIA_PCM_FMT_MASK;
	chan->is_enabled = 1;

	if (chan->is_record)
		atomic_set (&chan->n_bufs, 0);
	else
		atomic_set (&chan->n_bufs, VIA_DMA_BUFFERS);
	atomic_set (&chan->hw_ptr, 0);
}


/**
 *	via_chan_init - Initialize PCM channel
 *	@card: Private audio chip info
 *	@chan: Channel to be initialized
 *
 *	Performs all the preparations necessary to begin
 *	using a PCM channel.
 *
 *	Currently the preparations include allocating the
 *	scatter-gather DMA table and buffers, setting the
 *	PCM channel to a known state, and passing the
 *	address of the DMA table to the hardware.
 *
 *	Note that special care is taken when passing the
 *	DMA table address to hardware, because it was found
 *	during driver development that the hardware did not
 *	always "take" the address.
 */

static int via_chan_init (struct via_info *card, struct via_channel *chan)
{
	int i;

	DPRINTK ("ENTER\n");

	/* bzero channel structure, and init members to defaults */
	via_chan_init_defaults (card, chan);

	/* alloc DMA-able memory for scatter-gather table */
	chan->sgtable = pci_alloc_consistent (card->pdev,
		(sizeof (struct via_sgd_table) * VIA_DMA_BUFFERS),
		&chan->sgt_handle);
	if (!chan->sgtable) {
		printk (KERN_ERR PFX "DMA table alloc fail, aborting\n");
		DPRINTK ("EXIT\n");
		return -ENOMEM;
	}

	memset ((void*)chan->sgtable, 0,
		(sizeof (struct via_sgd_table) * VIA_DMA_BUFFERS));

	/* alloc DMA-able memory for scatter-gather buffers */
	for (i = 0; i < VIA_DMA_BUFFERS; i++) {
		chan->sgbuf[i].cpuaddr =
			pci_alloc_consistent (card->pdev, VIA_DMA_BUF_SIZE,
					      &chan->sgbuf[i].handle);

		if (!chan->sgbuf[i].cpuaddr)
			goto err_out_nomem;

		if (i < (VIA_DMA_BUFFERS - 1))
			chan->sgtable[i].count = cpu_to_le32 (VIA_DMA_BUF_SIZE | VIA_FLAG);
		else
			chan->sgtable[i].count = cpu_to_le32 (VIA_DMA_BUF_SIZE | VIA_EOL);
		chan->sgtable[i].addr = cpu_to_le32 (chan->sgbuf[i].handle);

#ifndef VIA_NDEBUG
		memset (chan->sgbuf[i].cpuaddr, 0xBC, VIA_DMA_BUF_SIZE);
#endif

#if 1
		DPRINTK ("dmabuf #%d (h=%lx, 32(h)=%lx, v2p=%lx, a=%p)\n",
			 i, (long)chan->sgbuf[i].handle,
			 (long)chan->sgtable[i].addr,
			 virt_to_phys(chan->sgbuf[i].cpuaddr),
			 chan->sgbuf[i].cpuaddr);
#endif

		assert ((VIA_DMA_BUF_SIZE % PAGE_SIZE) == 0);
	}

	/* stop any existing channel output */
	via_chan_clear (chan);
	via_chan_status_clear (chan->iobase);
	via_chan_pcm_fmt (chan, 1);

	/* set location of DMA-able scatter-gather info table */
	DPRINTK("outl (0x%X, 0x%04lX)\n",
		cpu_to_le32 (chan->sgt_handle),
		chan->iobase + VIA_PCM_TABLE_ADDR);

	via_ac97_wait_idle (card);
	outl (cpu_to_le32 (chan->sgt_handle),
	      chan->iobase + VIA_PCM_TABLE_ADDR);
	udelay (20);
	via_ac97_wait_idle (card);

	DPRINTK("inl (0x%lX) = %x\n",
		chan->iobase + VIA_PCM_TABLE_ADDR,
		inl(chan->iobase + VIA_PCM_TABLE_ADDR));

	DPRINTK ("EXIT\n");
	return 0;

err_out_nomem:
	printk (KERN_ERR PFX "DMA buffer alloc fail, aborting\n");
	via_chan_free (card, chan);
	DPRINTK ("EXIT\n");
	return -ENOMEM;
}


/**
 *	via_chan_free - Release a PCM channel
 *	@card: Private audio chip info
 *	@chan: Channel to be released
 *
 *	Performs all the functions necessary to clean up
 *	an initialized channel.
 *
 *	Currently these functions include disabled any
 *	active DMA operations, setting the PCM channel
 *	back to a known state, and releasing any allocated
 *	sound buffers.
 */

static void via_chan_free (struct via_info *card, struct via_channel *chan)
{
	int i;

	DPRINTK ("ENTER\n");

	synchronize_irq();

	spin_lock_irq (&card->lock);

	/* stop any existing channel output */
	via_chan_stop (chan->iobase);
	via_chan_status_clear (chan->iobase);
	via_chan_pcm_fmt (chan, 1);

	spin_unlock_irq (&card->lock);

	/* zero location of DMA-able scatter-gather info table */
	via_ac97_wait_idle(card);
	outl (0, chan->iobase + VIA_PCM_TABLE_ADDR);

	for (i = 0; i < VIA_DMA_BUFFERS; i++)
		if (chan->sgbuf[i].cpuaddr) {
			assert ((VIA_DMA_BUF_SIZE % PAGE_SIZE) == 0);
			pci_free_consistent (card->pdev, VIA_DMA_BUF_SIZE,
					     chan->sgbuf[i].cpuaddr,
					     chan->sgbuf[i].handle);
			chan->sgbuf[i].cpuaddr = NULL;
			chan->sgbuf[i].handle = 0;
		}

	if (chan->sgtable) {
		pci_free_consistent (card->pdev,
			(sizeof (struct via_sgd_table) * VIA_DMA_BUFFERS),
			(void*)chan->sgtable, chan->sgt_handle);
		chan->sgtable = NULL;
	}

	DPRINTK ("EXIT\n");
}


/**
 *	via_chan_pcm_fmt - Update PCM channel settings
 *	@chan: Channel to be updated
 *	@reset: Boolean.  If non-zero, channel will be reset
 *		to 8-bit mono mode.
 *
 *	Stores the settings of the current PCM format,
 *	8-bit or 16-bit, and mono/stereo, into the
 *	hardware settings for the specified channel.
 *	If @reset is non-zero, the channel is reset
 *	to 8-bit mono mode.  Otherwise, the channel
 *	is set to the values stored in the channel
 *	information struct @chan.
 */

static void via_chan_pcm_fmt (struct via_channel *chan, int reset)
{
	DPRINTK ("ENTER, pcm_fmt=0x%02X, reset=%s\n",
		 chan->pcm_fmt, reset ? "yes" : "no");

	assert (chan != NULL);

	if (reset)
		/* reset to 8-bit mono mode */
		chan->pcm_fmt = 0;

	/* enable interrupts on FLAG and EOL */
	chan->pcm_fmt |= VIA_CHAN_TYPE_MASK;

	/* if we are recording, enable recording fifo bit */
	if (chan->is_record)
		chan->pcm_fmt |= VIA_PCM_REC_FIFO;
	/* set interrupt select bits where applicable (PCM & FM out channels) */
	if (!chan->is_record)
		chan->pcm_fmt |= VIA_CHAN_TYPE_INT_SELECT;

	outb (chan->pcm_fmt, chan->iobase + 2);

	DPRINTK ("EXIT, pcm_fmt = 0x%02X, reg = 0x%02X\n",
		 chan->pcm_fmt,
		 inb (chan->iobase + 2));
}


/**
 *	via_chan_clear - Stop DMA channel operation, and reset pointers
 *	@chan: Channel to be cleared
 *
 *	Call via_chan_stop to halt DMA operations, and then resets
 *	all software pointers which track DMA operation.
 */

static void via_chan_clear (struct via_channel *chan)
{
	DPRINTK ("ENTER\n");
	via_chan_stop (chan->iobase);
	chan->is_active = 0;
	chan->is_mapped = 0;
	chan->is_enabled = 1;
	chan->slop_len = 0;
	chan->sw_ptr = 0;
	chan->n_irqs = 0;
	atomic_set (&chan->hw_ptr, 0);
	if (chan->is_record)
		atomic_set (&chan->n_bufs, 0);
	else
		atomic_set (&chan->n_bufs, VIA_DMA_BUFFERS);
	DPRINTK ("EXIT\n");
}


/**
 *	via_chan_set_speed - Set PCM sample rate for given channel
 *	@card: Private info for specified board
 *	@chan: Channel whose sample rate will be adjusted
 *	@val: New sample rate, in Khz
 *
 *	Helper function for the %SNDCTL_DSP_SPEED ioctl.  OSS semantics
 *	demand that all audio operations halt (if they are not already
 *	halted) when the %SNDCTL_DSP_SPEED is given.
 *
 *	This function halts all audio operations for the given channel
 *	@chan, and then calls via_set_rate to set the audio hardware
 *	to the new rate.
 */

static int via_chan_set_speed (struct via_info *card,
			       struct via_channel *chan, int val)
{
	DPRINTK ("ENTER, requested rate = %d\n", val);

	via_chan_clear (chan);

	val = via_set_rate (&card->ac97, chan, val);

	DPRINTK ("EXIT, returning %d\n", val);
	return val;
}


/**
 *	via_chan_set_fmt - Set PCM sample size for given channel
 *	@card: Private info for specified board
 *	@chan: Channel whose sample size will be adjusted
 *	@val: New sample size, use the %AFMT_xxx constants
 *
 *	Helper function for the %SNDCTL_DSP_SETFMT ioctl.  OSS semantics
 *	demand that all audio operations halt (if they are not already
 *	halted) when the %SNDCTL_DSP_SETFMT is given.
 *
 *	This function halts all audio operations for the given channel
 *	@chan, and then calls via_chan_pcm_fmt to set the audio hardware
 *	to the new sample size, either 8-bit or 16-bit.
 */

static int via_chan_set_fmt (struct via_info *card,
			     struct via_channel *chan, int val)
{
	DPRINTK ("ENTER, val=%s\n",
		 val == AFMT_U8 ? "AFMT_U8" :
	 	 val == AFMT_S16_LE ? "AFMT_S16_LE" :
		 "unknown");

	via_chan_clear (chan);

	assert (val != AFMT_QUERY); /* this case is handled elsewhere */

	switch (val) {
	case AFMT_S16_LE:
		if ((chan->pcm_fmt & VIA_PCM_FMT_16BIT) == 0) {
			chan->pcm_fmt |= VIA_PCM_FMT_16BIT;
			via_chan_pcm_fmt (chan, 0);
		}
		break;

	case AFMT_U8:
		if (chan->pcm_fmt & VIA_PCM_FMT_16BIT) {
			chan->pcm_fmt &= ~VIA_PCM_FMT_16BIT;
			via_chan_pcm_fmt (chan, 0);
		}
		break;

	default:
		DPRINTK ("unknown AFMT: 0x%X\n", val);
		val = AFMT_S16_LE;
	}

	DPRINTK ("EXIT\n");
	return val;
}


/**
 *	via_chan_set_stereo - Enable or disable stereo for a DMA channel
 *	@card: Private info for specified board
 *	@chan: Channel whose stereo setting will be adjusted
 *	@val: New sample size, use the %AFMT_xxx constants
 *
 *	Helper function for the %SNDCTL_DSP_CHANNELS and %SNDCTL_DSP_STEREO ioctls.  OSS semantics
 *	demand that all audio operations halt (if they are not already
 *	halted) when %SNDCTL_DSP_CHANNELS or SNDCTL_DSP_STEREO is given.
 *
 *	This function halts all audio operations for the given channel
 *	@chan, and then calls via_chan_pcm_fmt to set the audio hardware
 *	to enable or disable stereo.
 */

static int via_chan_set_stereo (struct via_info *card,
			        struct via_channel *chan, int val)
{
	DPRINTK ("ENTER, channels = %d\n", val);

	via_chan_clear (chan);

	switch (val) {

	/* mono */
	case 1:
		chan->pcm_fmt &= ~VIA_PCM_FMT_STEREO;
		via_chan_pcm_fmt (chan, 0);
		break;

	/* stereo */
	case 2:
		chan->pcm_fmt |= VIA_PCM_FMT_STEREO;
		via_chan_pcm_fmt (chan, 0);
		break;

	/* unknown */
	default:
		printk (KERN_WARNING PFX "unknown number of channels\n");
		val = -EINVAL;
		break;
	}

	DPRINTK ("EXIT, returning %d\n", val);
	return val;
}


#ifdef VIA_CHAN_DUMP_BUFS
/**
 *	via_chan_dump_bufs - Display DMA table contents
 *	@chan: Channel whose DMA table will be displayed
 *
 *	Debugging function which displays the contents of the
 *	scatter-gather DMA table for the given channel @chan.
 */

static void via_chan_dump_bufs (struct via_channel *chan)
{
	int i;

	for (i = 0; i < VIA_DMA_BUFFERS; i++) {
		DPRINTK ("#%02d: addr=%x, count=%u, flag=%d, eol=%d\n",
			 i, chan->sgtable[i].addr,
			 chan->sgtable[i].count & 0x00FFFFFF,
			 chan->sgtable[i].count & VIA_FLAG ? 1 : 0,
			 chan->sgtable[i].count & VIA_EOL ? 1 : 0);
	}
	DPRINTK ("buf_in_use = %d, nextbuf = %d\n",
		 atomic_read (&chan->buf_in_use),
		 atomic_read (&chan->sw_ptr));
}
#endif /* VIA_CHAN_DUMP_BUFS */


/**
 *	via_chan_flush_frag - Flush partially-full playback buffer to hardware
 *	@chan: Channel whose DMA table will be displayed
 *
 *	Flushes partially-full playback buffer to hardware.
 */

static void via_chan_flush_frag (struct via_channel *chan)
{
	DPRINTK ("ENTER\n");

	assert (chan->slop_len > 0);

	if (chan->sw_ptr == (VIA_DMA_BUFFERS - 1))
		chan->sw_ptr = 0;
	else
		chan->sw_ptr++;

	chan->slop_len = 0;

	assert (atomic_read (&chan->n_bufs) > 0);
	atomic_dec (&chan->n_bufs);

	DPRINTK ("EXIT\n");
}



/**
 *	via_chan_maybe_start - Initiate audio hardware DMA operation
 *	@chan: Channel whose DMA is to be started
 *
 *	Initiate DMA operation, if the DMA engine for the given
 *	channel @chan is not already active.
 */

static inline void via_chan_maybe_start (struct via_channel *chan)
{
	if (!chan->is_active && chan->is_enabled) {
		chan->is_active = 1;
		sg_begin (chan);
		DPRINTK("starting channel %s\n", chan->name);
	}
}


/****************************************************************
 *
 * Interface to ac97-codec module
 *
 *
 */

/**
 *	via_ac97_wait_idle - Wait until AC97 codec is not busy
 *	@card: Private info for specified board
 *
 *	Sleep until the AC97 codec is no longer busy.
 *	Returns the final value read from the SGD
 *	register being polled.
 */

static u8 via_ac97_wait_idle (struct via_info *card)
{
	u8 tmp8;
	int counter = VIA_COUNTER_LIMIT;

	DPRINTK ("ENTER/EXIT\n");

	assert (card != NULL);
	assert (card->pdev != NULL);

	do {
		udelay (15);

		tmp8 = inb (card->baseaddr + 0x83);
	} while ((tmp8 & VIA_CR83_BUSY) && (counter-- > 0));

	if (tmp8 & VIA_CR83_BUSY)
		printk (KERN_WARNING PFX "timeout waiting on AC97 codec\n");
	return tmp8;
}


/**
 *	via_ac97_read_reg - Read AC97 standard register
 *	@codec: Pointer to generic AC97 codec info
 *	@reg: Index of AC97 register to be read
 *
 *	Read the value of a single AC97 codec register,
 *	as defined by the Intel AC97 specification.
 *
 *	Defines the standard AC97 read-register operation
 *	required by the kernel's ac97_codec interface.
 *
 *	Returns the 16-bit value stored in the specified
 *	register.
 */

static u16 via_ac97_read_reg (struct ac97_codec *codec, u8 reg)
{
	unsigned long data;
	struct via_info *card;
	int counter;

	DPRINTK ("ENTER\n");

	assert (codec != NULL);
	assert (codec->private_data != NULL);

	card = codec->private_data;

	data = (reg << 16) | VIA_CR80_READ;

	outl (data, card->baseaddr + VIA_BASE0_AC97_CTRL);
	udelay (20);

	for (counter = VIA_COUNTER_LIMIT; counter > 0; counter--) {
		if (inl (card->baseaddr + 0x80) & VIA_CR80_VALID)
			goto out;

		udelay (15);
	}

	printk (KERN_WARNING PFX "timeout while reading AC97 codec (0x%lX)\n", data);
	goto err_out;

out:
	data = (unsigned long) inl (card->baseaddr + 0x80);
	outb (0x02, card->baseaddr + 0x83);

	if (((data & 0x007F0000) >> 16) == reg) {
		DPRINTK ("EXIT, success, data=0x%lx, retval=0x%lx\n",
			 data, data & 0x0000FFFF);
		return data & 0x0000FFFF;
	}

	printk (KERN_WARNING "via82cxxx_audio: not our index: reg=0x%x, newreg=0x%lx\n",
		reg, ((data & 0x007F0000) >> 16));

err_out:
	DPRINTK ("EXIT, returning 0\n");
	return 0;
}


/**
 *	via_ac97_write_reg - Write AC97 standard register
 *	@codec: Pointer to generic AC97 codec info
 *	@reg: Index of AC97 register to be written
 *	@value: Value to be written to AC97 register
 *
 *	Write the value of a single AC97 codec register,
 *	as defined by the Intel AC97 specification.
 *
 *	Defines the standard AC97 write-register operation
 *	required by the kernel's ac97_codec interface.
 */

static void via_ac97_write_reg (struct ac97_codec *codec, u8 reg, u16 value)
{
	u32 data;
	struct via_info *card;
	int counter;

	DPRINTK ("ENTER\n");

	assert (codec != NULL);
	assert (codec->private_data != NULL);

	card = codec->private_data;

	data = (reg << 16) + value;
	outl (data, card->baseaddr + VIA_BASE0_AC97_CTRL);
	udelay (10);

	for (counter = VIA_COUNTER_LIMIT; counter > 0; counter--) {
		if ((inb (card->baseaddr + 0x83) & VIA_CR83_BUSY) == 0)
			goto out;

		udelay (15);
	}

	printk (KERN_WARNING PFX "timeout after AC97 codec write (0x%X, 0x%X)\n", reg, value);

out:
	DPRINTK ("EXIT\n");
}


static int via_mixer_open (struct inode *inode, struct file *file)
{
	int minor = MINOR(inode->i_rdev);
	struct via_info *card;
	struct pci_dev *pdev;
	struct pci_driver *drvr;

	DPRINTK ("ENTER\n");

	pci_for_each_dev(pdev) {
		drvr = pci_dev_driver (pdev);
		if (drvr == &via_driver) {
			assert (pci_get_drvdata (pdev) != NULL);

			card = pci_get_drvdata (pdev);
			if (card->ac97.dev_mixer == minor)
				goto match;
		}
	}

	DPRINTK ("EXIT, returning -ENODEV\n");
	return -ENODEV;

match:
	file->private_data = &card->ac97;

	DPRINTK ("EXIT, returning 0\n");
	return 0;
}

static int via_mixer_ioctl (struct inode *inode, struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct ac97_codec *codec = file->private_data;
	struct via_info *card;
	int nonblock = (file->f_flags & O_NONBLOCK);
	int rc;

	DPRINTK ("ENTER\n");

	assert (codec != NULL);
	card = codec->private_data;
	assert (card != NULL);

	rc = via_syscall_down (card, nonblock);
	if (rc) goto out;

	rc = codec->mixer_ioctl(codec, cmd, arg);

	up (&card->syscall_sem);

out:
	DPRINTK ("EXIT, returning %d\n", rc);
	return rc;
}


static loff_t via_llseek(struct file *file, loff_t offset, int origin)
{
	DPRINTK ("ENTER\n");

	DPRINTK("EXIT, returning -ESPIPE\n");
	return -ESPIPE;
}


static struct file_operations via_mixer_fops = {
	owner:		THIS_MODULE,
	open:		via_mixer_open,
	llseek:		via_llseek,
	ioctl:		via_mixer_ioctl,
};


static int __init via_ac97_reset (struct via_info *card)
{
	struct pci_dev *pdev = card->pdev;
	u8 tmp8;
	u16 tmp16;

	DPRINTK ("ENTER\n");

	assert (pdev != NULL);

#ifndef NDEBUG
	{
		u8 r40,r41,r42,r43,r44,r48;
		pci_read_config_byte (card->pdev, 0x40, &r40);
		pci_read_config_byte (card->pdev, 0x41, &r41);
		pci_read_config_byte (card->pdev, 0x42, &r42);
		pci_read_config_byte (card->pdev, 0x43, &r43);
		pci_read_config_byte (card->pdev, 0x44, &r44);
		pci_read_config_byte (card->pdev, 0x48, &r48);
		DPRINTK("PCI config: %02X %02X %02X %02X %02X %02X\n",
			r40,r41,r42,r43,r44,r48);

		spin_lock_irq (&card->lock);
		DPRINTK ("regs==%02X %02X %02X %08X %08X %08X %08X\n",
			 inb (card->baseaddr + 0x00),
			 inb (card->baseaddr + 0x01),
			 inb (card->baseaddr + 0x02),
			 inl (card->baseaddr + 0x04),
			 inl (card->baseaddr + 0x0C),
			 inl (card->baseaddr + 0x80),
			 inl (card->baseaddr + 0x84));
		spin_unlock_irq (&card->lock);

	}
#endif

        /*
         * reset AC97 controller: enable, disable, enable
         * pause after each command for good luck
         */
        pci_write_config_byte (pdev, VIA_ACLINK_CTRL, VIA_CR41_AC97_ENABLE |
                               VIA_CR41_AC97_RESET | VIA_CR41_AC97_WAKEUP);
        udelay (100);

        pci_write_config_byte (pdev, VIA_ACLINK_CTRL, 0);
        udelay (100);

        pci_write_config_byte (pdev, VIA_ACLINK_CTRL,
			       VIA_CR41_AC97_ENABLE | VIA_CR41_PCM_ENABLE |
                               VIA_CR41_VRA | VIA_CR41_AC97_RESET);
        udelay (100);

#if 0 /* this breaks on K7M */
	/* disable legacy stuff */
	pci_write_config_byte (pdev, 0x42, 0x00);
	udelay(10);
#endif

	/* route FM trap to IRQ, disable FM trap */
	pci_write_config_byte (pdev, 0x48, 0x05);
	udelay(10);

	/* disable all codec GPI interrupts */
	outl (0, pci_resource_start (pdev, 0) + 0x8C);

	/* WARNING: this line is magic.  Remove this
	 * and things break. */
	/* enable variable rate, variable rate MIC ADC */
	tmp16 = via_ac97_read_reg (&card->ac97, 0x2A);
	via_ac97_write_reg (&card->ac97, 0x2A, tmp16 | (1<<0));

	pci_read_config_byte (pdev, VIA_ACLINK_CTRL, &tmp8);
	if ((tmp8 & (VIA_CR41_AC97_ENABLE | VIA_CR41_AC97_RESET)) == 0) {
		printk (KERN_ERR PFX "cannot enable AC97 controller, aborting\n");
		DPRINTK ("EXIT, tmp8=%X, returning -ENODEV\n", tmp8);
		return -ENODEV;
	}

	DPRINTK ("EXIT, returning 0\n");
	return 0;
}


static void via_ac97_codec_wait (struct ac97_codec *codec)
{
	assert (codec->private_data != NULL);
	via_ac97_wait_idle (codec->private_data);
}


static int __init via_ac97_init (struct via_info *card)
{
	int rc;
	u16 tmp16;

	DPRINTK ("ENTER\n");

	assert (card != NULL);

	memset (&card->ac97, 0, sizeof (card->ac97));
	card->ac97.private_data = card;
	card->ac97.codec_read = via_ac97_read_reg;
	card->ac97.codec_write = via_ac97_write_reg;
	card->ac97.codec_wait = via_ac97_codec_wait;

	card->ac97.dev_mixer = register_sound_mixer (&via_mixer_fops, -1);
	if (card->ac97.dev_mixer < 0) {
		printk (KERN_ERR PFX "unable to register AC97 mixer, aborting\n");
		DPRINTK("EXIT, returning -EIO\n");
		return -EIO;
	}

	rc = via_ac97_reset (card);
	if (rc) {
		printk (KERN_ERR PFX "unable to reset AC97 codec, aborting\n");
		goto err_out;
	}

	if (ac97_probe_codec (&card->ac97) == 0) {
		printk (KERN_ERR PFX "unable to probe AC97 codec, aborting\n");
		rc = -EIO;
		goto err_out;
	}

	/* enable variable rate, variable rate MIC ADC */
	tmp16 = via_ac97_read_reg (&card->ac97, 0x2A);
	via_ac97_write_reg (&card->ac97, 0x2A, tmp16 | (1<<0));

	DPRINTK ("EXIT, returning 0\n");
	return 0;

err_out:
	unregister_sound_mixer (card->ac97.dev_mixer);
	DPRINTK("EXIT, returning %d\n", rc);
	return rc;
}


static void via_ac97_cleanup (struct via_info *card)
{
	DPRINTK("ENTER\n");

	assert (card != NULL);
	assert (card->ac97.dev_mixer >= 0);

	unregister_sound_mixer (card->ac97.dev_mixer);

	DPRINTK("EXIT\n");
}



/****************************************************************
 *
 * Interrupt-related code
 *
 */

/**
 *	via_intr_channel - handle an interrupt for a single channel
 *	@chan: handle interrupt for this channel
 *
 *	This is the "meat" of the interrupt handler,
 *	containing the actions taken each time an interrupt
 *	occurs.  All communication and coordination with
 *	userspace takes place here.
 *
 *	Locking: inside card->lock
 */

static void via_intr_channel (struct via_channel *chan)
{
	u8 status;
	int n;

	/* check pertinent bits of status register for action bits */
	status = inb (chan->iobase) & (VIA_SGD_FLAG | VIA_SGD_EOL | VIA_SGD_STOPPED);
	if (!status)
		return;

	/* acknowledge any flagged bits ASAP */
	outb (status, chan->iobase);

	/* grab current h/w ptr value */
	n = atomic_read (&chan->hw_ptr);

	/* sanity check: make sure our h/w ptr doesn't have a weird value */
	assert (n >= 0);
	assert (n < VIA_DMA_BUFFERS);

	/* reset SGD data structure in memory to reflect a full buffer,
	 * and advance the h/w ptr, wrapping around to zero if needed
	 */
	if (n == (VIA_DMA_BUFFERS - 1)) {
		chan->sgtable[n].count = (VIA_DMA_BUF_SIZE | VIA_EOL);
		atomic_set (&chan->hw_ptr, 0);
	} else {
		chan->sgtable[n].count = (VIA_DMA_BUF_SIZE | VIA_FLAG);
		atomic_inc (&chan->hw_ptr);
	}

	/* accounting crap for SNDCTL_DSP_GETxPTR */
	chan->n_irqs++;
	chan->bytes += VIA_DMA_BUF_SIZE;
	if (chan->bytes < 0) /* handle overflow of 31-bit value */
		chan->bytes = VIA_DMA_BUF_SIZE;

	/* wake up anyone listening to see when interrupts occur */
	if (waitqueue_active (&chan->wait))
		wake_up_all (&chan->wait);

	DPRINTK ("%s intr, status=0x%02X, hwptr=0x%lX, chan->hw_ptr=%d\n",
		 chan->name, status, (long) inl (chan->iobase + 0x04),
		 atomic_read (&chan->hw_ptr));

	/* all following checks only occur when not in mmap(2) mode */
	if (chan->is_mapped)
		return;

	/* If we are recording, then n_bufs represents the number
	 * of buffers waiting to be handled by userspace.
	 * If we are playback, then n_bufs represents the number
	 * of buffers remaining to be filled by userspace.
	 * We increment here.  If we reach max buffers (VIA_DMA_BUFFERS),
	 * this indicates an underrun/overrun.  For this case under OSS,
	 * we stop the record/playback process.
	 */
	if (atomic_read (&chan->n_bufs) < VIA_DMA_BUFFERS)
		atomic_inc (&chan->n_bufs);
	assert (atomic_read (&chan->n_bufs) <= VIA_DMA_BUFFERS);

	if (atomic_read (&chan->n_bufs) == VIA_DMA_BUFFERS) {
		chan->is_active = 0;
		via_chan_stop (chan->iobase);
	}

	DPRINTK ("%s intr, channel n_bufs == %d\n", chan->name,
		 atomic_read (&chan->n_bufs));
}


static void via_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	struct via_info *card = dev_id;
	u32 status32;

	/* to minimize interrupt sharing costs, we use the SGD status
	 * shadow register to check the status of all inputs and
	 * outputs with a single 32-bit bus read.  If no interrupt
	 * conditions are flagged, we exit immediately
	 */
	status32 = inl (card->baseaddr + VIA_BASE0_SGD_STATUS_SHADOW);
	if (!(status32 & VIA_INTR_MASK))
		return;

	DPRINTK ("intr, status32 == 0x%08X\n", status32);

	/* synchronize interrupt handling under SMP.  this spinlock
	 * goes away completely on UP
	 */
	spin_lock (&card->lock);

	if (status32 & VIA_INTR_OUT)
		via_intr_channel (&card->ch_out);
	if (status32 & VIA_INTR_IN)
		via_intr_channel (&card->ch_in);
	if (status32 & VIA_INTR_FM)
		via_intr_channel (&card->ch_fm);

	spin_unlock (&card->lock);
}


/**
 *	via_interrupt_disable - Disable all interrupt-generating sources
 *	@card: Private info for specified board
 *
 *	Disables all interrupt-generation flags in the Via
 *	audio hardware registers.
 */

static void via_interrupt_disable (struct via_info *card)
{
	u8 tmp8;
	unsigned long flags;

	DPRINTK ("ENTER\n");

	assert (card != NULL);

	spin_lock_irqsave (&card->lock, flags);

	pci_read_config_byte (card->pdev, VIA_FM_NMI_CTRL, &tmp8);
	if ((tmp8 & VIA_CR48_FM_TRAP_TO_NMI) == 0) {
		tmp8 |= VIA_CR48_FM_TRAP_TO_NMI;
		pci_write_config_byte (card->pdev, VIA_FM_NMI_CTRL, tmp8);
	}

	outb (inb (card->baseaddr + VIA_BASE0_PCM_OUT_CHAN_TYPE) &
	      VIA_INT_DISABLE_MASK,
	      card->baseaddr + VIA_BASE0_PCM_OUT_CHAN_TYPE);
	outb (inb (card->baseaddr + VIA_BASE0_PCM_IN_CHAN_TYPE) &
	      VIA_INT_DISABLE_MASK,
	      card->baseaddr + VIA_BASE0_PCM_IN_CHAN_TYPE);
	outb (inb (card->baseaddr + VIA_BASE0_FM_OUT_CHAN_TYPE) &
	      VIA_INT_DISABLE_MASK,
	      card->baseaddr + VIA_BASE0_FM_OUT_CHAN_TYPE);

	spin_unlock_irqrestore (&card->lock, flags);

	DPRINTK ("EXIT\n");
}


/**
 *	via_interrupt_init - Initialize interrupt handling
 *	@card: Private info for specified board
 *
 *	Obtain and reserve IRQ for using in handling audio events.
 *	Also, disable any IRQ-generating resources, to make sure
 *	we don't get interrupts before we want them.
 */

static int via_interrupt_init (struct via_info *card)
{
	DPRINTK ("ENTER\n");

	assert (card != NULL);
	assert (card->pdev != NULL);

	/* check for sane IRQ number. can this ever happen? */
	if (card->pdev->irq < 2) {
		printk (KERN_ERR PFX "insane IRQ %d, aborting\n",
			card->pdev->irq);
		DPRINTK ("EXIT, returning -EIO\n");
		return -EIO;
	}

	if (request_irq (card->pdev->irq, via_interrupt, SA_SHIRQ, VIA_MODULE_NAME, card)) {
		printk (KERN_ERR PFX "unable to obtain IRQ %d, aborting\n",
			card->pdev->irq);
		DPRINTK ("EXIT, returning -EBUSY\n");
		return -EBUSY;
	}

	/* we don't want interrupts until we're opened */
	via_interrupt_disable (card);

	DPRINTK ("EXIT, returning 0\n");
	return 0;
}


/**
 *	via_interrupt_cleanup - Shutdown driver interrupt handling
 *	@card: Private info for specified board
 *
 *	Disable any potential interrupt sources in the Via audio
 *	hardware, and then release (un-reserve) the IRQ line
 *	in the kernel core.
 */

static void via_interrupt_cleanup (struct via_info *card)
{
	DPRINTK ("ENTER\n");

	assert (card != NULL);
	assert (card->pdev != NULL);

	via_interrupt_disable (card);

	free_irq (card->pdev->irq, card);

	DPRINTK ("EXIT\n");
}


/****************************************************************
 *
 * OSS DSP device
 *
 */

static struct file_operations via_dsp_fops = {
	owner:		THIS_MODULE,
	open:		via_dsp_open,
	release:	via_dsp_release,
	read:		via_dsp_read,
	write:		via_dsp_write,
	poll:		via_dsp_poll,
	llseek: 	via_llseek,
	ioctl:		via_dsp_ioctl,
#ifdef VIA_SUPPORT_MMAP
	mmap:		via_dsp_mmap,
#endif
};


static int __init via_dsp_init (struct via_info *card)
{
	u8 tmp8;

	DPRINTK ("ENTER\n");

	assert (card != NULL);

	/* turn off legacy features, if not already */
	pci_read_config_byte (card->pdev, VIA_FUNC_ENABLE, &tmp8);
	if (tmp8 & (VIA_CR42_SB_ENABLE | VIA_CR42_MIDI_ENABLE |
		    VIA_CR42_FM_ENABLE)) {
		tmp8 &= ~(VIA_CR42_SB_ENABLE | VIA_CR42_MIDI_ENABLE |
			  VIA_CR42_FM_ENABLE);
		pci_write_config_byte (card->pdev, VIA_FUNC_ENABLE, tmp8);
	}

	via_stop_everything (card);

	card->dev_dsp = register_sound_dsp (&via_dsp_fops, -1);
	if (card->dev_dsp < 0) {
		DPRINTK ("EXIT, returning -ENODEV\n");
		return -ENODEV;
	}
	DPRINTK ("EXIT, returning 0\n");
	return 0;
}


static void via_dsp_cleanup (struct via_info *card)
{
	DPRINTK ("ENTER\n");

	assert (card != NULL);
	assert (card->dev_dsp >= 0);

	via_stop_everything (card);

	unregister_sound_dsp (card->dev_dsp);

	DPRINTK ("EXIT\n");
}


#ifdef VIA_SUPPORT_MMAP
static struct page * via_mm_nopage (struct vm_area_struct * vma,
				    unsigned long address, int write_access)
{
	struct via_info *card = vma->vm_private_data;
	struct via_channel *chan = &card->ch_out;
	struct page *dmapage;
	unsigned long pgoff;
	int rd, wr;

	DPRINTK ("ENTER, start %lXh, ofs %lXh, pgoff %ld, addr %lXh, wr %d\n",
		 vma->vm_start,
		 address - vma->vm_start,
		 (address - vma->vm_start) >> PAGE_SHIFT,
		 address,
		 write_access);

	assert (VIA_DMA_BUF_SIZE == PAGE_SIZE);

        if (address > vma->vm_end) {
		DPRINTK ("EXIT, returning NOPAGE_SIGBUS\n");
		return NOPAGE_SIGBUS; /* Disallow mremap */
	}
        if (!card) {
		DPRINTK ("EXIT, returning NOPAGE_OOM\n");
		return NOPAGE_OOM;	/* Nothing allocated */
	}

	pgoff = vma->vm_pgoff + ((address - vma->vm_start) >> PAGE_SHIFT);
	rd = card->ch_in.is_mapped;
	wr = card->ch_out.is_mapped;

#ifndef VIA_NDEBUG
	{
	unsigned long max_bufs = VIA_DMA_BUFFERS;
	if (rd && wr) max_bufs *= 2;
	/* via_dsp_mmap() should ensure this */
	assert (pgoff < max_bufs);
	}
#endif

	/* if full-duplex (read+write) and we have two sets of bufs,
	 * then the playback buffers come first, sez soundcard.c */
	if (pgoff >= VIA_DMA_BUFFERS) {
		pgoff -= VIA_DMA_BUFFERS;
		chan = &card->ch_in;
	} else if (!wr)
		chan = &card->ch_in;

	assert ((((unsigned long)chan->sgbuf[pgoff].cpuaddr) % PAGE_SIZE) == 0);

	dmapage = virt_to_page (chan->sgbuf[pgoff].cpuaddr);
	DPRINTK ("EXIT, returning page %p for cpuaddr %lXh\n",
		 dmapage, (unsigned long) chan->sgbuf[pgoff].cpuaddr);
	get_page (dmapage);
	return dmapage;
}


#ifndef VM_RESERVED
static int via_mm_swapout (struct page *page, struct file *filp)
{
	return 0;
}
#endif /* VM_RESERVED */


struct vm_operations_struct via_mm_ops = {
	nopage:		via_mm_nopage,

#ifndef VM_RESERVED
	swapout:	via_mm_swapout,
#endif
};


static int via_dsp_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct via_info *card;
	int nonblock = (file->f_flags & O_NONBLOCK);
	int rc = -EINVAL, rd=0, wr=0;
	unsigned long max_size, size, start, offset;

	assert (file != NULL);
	assert (vma != NULL);
	card = file->private_data;
	assert (card != NULL);

	DPRINTK ("ENTER, start %lXh, size %ld, pgoff %ld\n",
		 vma->vm_start,
		 vma->vm_end - vma->vm_start,
		 vma->vm_pgoff);

	assert (VIA_DMA_BUF_SIZE == PAGE_SIZE);

	max_size = 0;
	if (file->f_mode & FMODE_READ) {
		rd = 1;
		max_size += (VIA_DMA_BUFFERS * VIA_DMA_BUF_SIZE);
	}
	if (file->f_mode & FMODE_WRITE) {
		wr = 1;
		max_size += (VIA_DMA_BUFFERS * VIA_DMA_BUF_SIZE);
	}

	start = vma->vm_start;
	offset = (vma->vm_pgoff << PAGE_SHIFT);
	size = vma->vm_end - vma->vm_start;

	/* some basic size/offset sanity checks */
	if (size > max_size)
		goto out;
	if (offset > max_size - size)
		goto out;

	rc = via_syscall_down (card, nonblock);
	if (rc) goto out;

	vma->vm_ops = &via_mm_ops;
	vma->vm_private_data = card;

#ifdef VM_RESERVED
	vma->vm_flags |= VM_RESERVED;
#endif

	if (rd)
		card->ch_in.is_mapped = 1;
	if (wr)
		card->ch_out.is_mapped = 1;

	up (&card->syscall_sem);
	rc = 0;

out:
	DPRINTK("EXIT, returning %d\n", rc);
	return rc;
}
#endif /* VIA_SUPPORT_MMAP */


static ssize_t via_dsp_do_read (struct via_info *card,
				char *userbuf, size_t count,
				int nonblock)
{
	const char *orig_userbuf = userbuf;
	struct via_channel *chan = &card->ch_in;
	size_t size;
	int n, tmp;

	/* if SGD has not yet been started, start it */
	via_chan_maybe_start (chan);

handle_one_block:
	/* just to be a nice neighbor */
	if (current->need_resched)
		schedule ();

	/* grab current channel software pointer.  In the case of
	 * recording, this is pointing to the next buffer that
	 * will receive data from the audio hardware.
	 */
	n = chan->sw_ptr;

	/* n_bufs represents the number of buffers waiting
	 * to be copied to userland.  sleep until at least
	 * one buffer has been read from the audio hardware.
	 */
	tmp = atomic_read (&chan->n_bufs);
	assert (tmp >= 0);
	assert (tmp <= VIA_DMA_BUFFERS);
	while (tmp == 0) {
		if (nonblock || !chan->is_active)
			return -EAGAIN;

		DPRINTK ("Sleeping on block %d\n", n);
		interruptible_sleep_on (&chan->wait);

		if (signal_pending (current))
			return -ERESTARTSYS;

		tmp = atomic_read (&chan->n_bufs);
	}

	/* Now that we have a buffer we can read from, send
	 * as much as sample data possible to userspace.
	 */
	while ((count > 0) && (chan->slop_len < VIA_DMA_BUF_SIZE)) {
		size_t slop_left = VIA_DMA_BUF_SIZE - chan->slop_len;

		size = (count < slop_left) ? count : slop_left;
		if (copy_to_user (userbuf,
				  chan->sgbuf[n].cpuaddr + chan->slop_len,
				  size))
			return -EFAULT;

		count -= size;
		chan->slop_len += size;
		userbuf += size;
	}

	/* If we didn't copy the buffer completely to userspace,
	 * stop now.
	 */
	if (chan->slop_len < VIA_DMA_BUF_SIZE)
		goto out;

	/*
	 * If we get to this point, we copied one buffer completely
	 * to userspace, give the buffer back to the hardware.
	 */

	/* advance channel software pointer to point to
	 * the next buffer from which we will copy
	 */
	if (chan->sw_ptr == (VIA_DMA_BUFFERS - 1))
		chan->sw_ptr = 0;
	else
		chan->sw_ptr++;

	/* mark one less buffer waiting to be processed */
	assert (atomic_read (&chan->n_bufs) > 0);
	atomic_dec (&chan->n_bufs);

	/* we are at a block boundary, there is no fragment data */
	chan->slop_len = 0;

	DPRINTK("Flushed block %u, sw_ptr now %u, n_bufs now %d\n",
		n, chan->sw_ptr, atomic_read (&chan->n_bufs));

	DPRINTK ("regs==%02X %02X %02X %08X %08X %08X %08X\n",
		 inb (card->baseaddr + 0x00),
		 inb (card->baseaddr + 0x01),
		 inb (card->baseaddr + 0x02),
		 inl (card->baseaddr + 0x04),
		 inl (card->baseaddr + 0x0C),
		 inl (card->baseaddr + 0x80),
		 inl (card->baseaddr + 0x84));

	if (count > 0)
		goto handle_one_block;

out:
	return userbuf - orig_userbuf;
}


static ssize_t via_dsp_read(struct file *file, char *buffer, size_t count, loff_t *ppos)
{
	struct via_info *card;
	int nonblock = (file->f_flags & O_NONBLOCK);
	int rc;

	DPRINTK ("ENTER, file=%p, buffer=%p, count=%u, ppos=%lu\n",
		 file, buffer, count, ppos ? ((unsigned long)*ppos) : 0);

	assert (file != NULL);
	assert (buffer != NULL);
	card = file->private_data;
	assert (card != NULL);

	if (ppos != &file->f_pos) {
		DPRINTK ("EXIT, returning -ESPIPE\n");
		return -ESPIPE;
	}

	rc = via_syscall_down (card, nonblock);
	if (rc) goto out;

	if (card->ch_in.is_mapped) {
		rc = -ENXIO;
		goto out_up;
	}

	rc = via_dsp_do_read (card, buffer, count, nonblock);

out_up:
	up (&card->syscall_sem);
out:
	DPRINTK("EXIT, returning %ld\n",(long) rc);
	return rc;
}


static ssize_t via_dsp_do_write (struct via_info *card,
				 const char *userbuf, size_t count,
				 int nonblock)
{
	const char *orig_userbuf = userbuf;
	struct via_channel *chan = &card->ch_out;
	volatile struct via_sgd_table *sgtable = chan->sgtable;
	size_t size;
	int n, tmp;

handle_one_block:
	/* just to be a nice neighbor */
	if (current->need_resched)
		schedule ();

	/* grab current channel software pointer.  In the case of
	 * playback, this is pointing to the next buffer that
	 * should receive data from userland.
	 */
	n = chan->sw_ptr;

	/* n_bufs represents the number of buffers remaining
	 * to be filled by userspace.  Sleep until
	 * at least one buffer is available for our use.
	 */
	tmp = atomic_read (&chan->n_bufs);
	assert (tmp >= 0);
	assert (tmp <= VIA_DMA_BUFFERS);
	while (tmp == 0) {
		if (nonblock || !chan->is_enabled)
			return -EAGAIN;

		DPRINTK ("Sleeping on block %d, tmp==%d, ir==%d\n", n, tmp, chan->is_record);
		interruptible_sleep_on (&chan->wait);

		if (signal_pending (current))
			return -ERESTARTSYS;

		tmp = atomic_read (&chan->n_bufs);
	}

	/* Now that we have a buffer we can write to, fill it up
	 * as much as possible with data from userspace.
	 */
	while ((count > 0) && (chan->slop_len < VIA_DMA_BUF_SIZE)) {
		size_t slop_left = VIA_DMA_BUF_SIZE - chan->slop_len;

		size = (count < slop_left) ? count : slop_left;
		if (copy_from_user (chan->sgbuf[n].cpuaddr + chan->slop_len,
				    userbuf, size))
			return -EFAULT;

		count -= size;
		chan->slop_len += size;
		userbuf += size;
	}

	/* If we didn't fill up the buffer with data, stop now.
	 * Put a 'stop' marker in the DMA table too, to tell the
	 * audio hardware to stop if it gets here.
	 */
	if (chan->slop_len < VIA_DMA_BUF_SIZE) {
		sgtable[n].count = cpu_to_le32 (chan->slop_len | VIA_EOL | VIA_STOP);
		goto out;
	}

	/*
	 * If we get to this point, we have filled a buffer with
	 * audio data, flush the buffer to audio hardware.
	 */

	/* Record the true size for the audio hardware to notice */
	if (n == (VIA_DMA_BUFFERS - 1))
		sgtable[n].count = cpu_to_le32 (VIA_DMA_BUF_SIZE | VIA_EOL);
	else
		sgtable[n].count = cpu_to_le32 (VIA_DMA_BUF_SIZE | VIA_FLAG);

	/* advance channel software pointer to point to
	 * the next buffer we will fill with data
	 */
	if (chan->sw_ptr == (VIA_DMA_BUFFERS - 1))
		chan->sw_ptr = 0;
	else
		chan->sw_ptr++;

	/* mark one less buffer as being available for userspace consumption */
	assert (atomic_read (&chan->n_bufs) > 0);
	atomic_dec (&chan->n_bufs);

	/* we are at a block boundary, there is no fragment data */
	chan->slop_len = 0;

	/* if SGD has not yet been started, start it */
	via_chan_maybe_start (chan);

	DPRINTK("Flushed block %u, sw_ptr now %u, n_bufs now %d\n",
		n, chan->sw_ptr, atomic_read (&chan->n_bufs));

	DPRINTK ("regs==%02X %02X %02X %08X %08X %08X %08X\n",
		 inb (card->baseaddr + 0x00),
		 inb (card->baseaddr + 0x01),
		 inb (card->baseaddr + 0x02),
		 inl (card->baseaddr + 0x04),
		 inl (card->baseaddr + 0x0C),
		 inl (card->baseaddr + 0x80),
		 inl (card->baseaddr + 0x84));

	if (count > 0)
		goto handle_one_block;

out:
	return userbuf - orig_userbuf;
}


static ssize_t via_dsp_write(struct file *file, const char *buffer, size_t count, loff_t *ppos)
{
	struct via_info *card;
	ssize_t rc;
	int nonblock = (file->f_flags & O_NONBLOCK);

	DPRINTK ("ENTER, file=%p, buffer=%p, count=%u, ppos=%lu\n",
		 file, buffer, count, ppos ? ((unsigned long)*ppos) : 0);

	assert (file != NULL);
	assert (buffer != NULL);
	card = file->private_data;
	assert (card != NULL);

	if (ppos != &file->f_pos) {
		DPRINTK ("EXIT, returning -ESPIPE\n");
		return -ESPIPE;
	}

	rc = via_syscall_down (card, nonblock);
	if (rc) goto out;

	if (card->ch_out.is_mapped) {
		rc = -ENXIO;
		goto out_up;
	}

	rc = via_dsp_do_write (card, buffer, count, nonblock);

out_up:
	up (&card->syscall_sem);
out:
	DPRINTK("EXIT, returning %ld\n",(long) rc);
	return rc;
}


static unsigned int via_dsp_poll(struct file *file, struct poll_table_struct *wait)
{
	struct via_info *card;
	unsigned int mask = 0, rd, wr;

	DPRINTK ("ENTER\n");

	assert (file != NULL);
	card = file->private_data;
	assert (card != NULL);

	rd = (file->f_mode & FMODE_READ);
	wr = (file->f_mode & FMODE_WRITE);

	if (wr && (atomic_read (&card->ch_out.n_bufs) == 0)) {
		assert (card->ch_out.is_active);
                poll_wait(file, &card->ch_out.wait, wait);
	}
        if (rd) {
		/* XXX is it ok, spec-wise, to start DMA here? */
		via_chan_maybe_start (&card->ch_in);
		if (atomic_read (&card->ch_in.n_bufs) == 0)
	                poll_wait(file, &card->ch_in.wait, wait);
	}

	if (wr && (atomic_read (&card->ch_out.n_bufs) > 0))
		mask |= POLLOUT | POLLWRNORM;
	if (rd && (atomic_read (&card->ch_in.n_bufs) > 0))
		mask |= POLLIN | POLLRDNORM;

	DPRINTK("EXIT, returning %u\n", mask);
	return mask;
}


/**
 *	via_dsp_drain_playback - sleep until all playback samples are flushed
 *	@card: Private info for specified board
 *	@chan: Channel to drain
 *	@nonblock: boolean, non-zero if O_NONBLOCK is set
 *
 *	Sleeps until all playback has been flushed to the audio
 *	hardware.
 *
 *	Locking: inside card->syscall_sem
 */

static int via_dsp_drain_playback (struct via_info *card,
				   struct via_channel *chan, int nonblock)
{
	DPRINTK ("ENTER, nonblock = %d\n", nonblock);

	if (chan->slop_len > 0)
		via_chan_flush_frag (chan);

	if (atomic_read (&chan->n_bufs) == VIA_DMA_BUFFERS)
		goto out;

	via_chan_maybe_start (chan);

	while (atomic_read (&chan->n_bufs) < VIA_DMA_BUFFERS) {
		if (nonblock) {
			DPRINTK ("EXIT, returning -EAGAIN\n");
			return -EAGAIN;
		}

#ifdef VIA_DEBUG
		{
		u8 r40,r41,r42,r43,r44,r48;
		pci_read_config_byte (card->pdev, 0x40, &r40);
		pci_read_config_byte (card->pdev, 0x41, &r41);
		pci_read_config_byte (card->pdev, 0x42, &r42);
		pci_read_config_byte (card->pdev, 0x43, &r43);
		pci_read_config_byte (card->pdev, 0x44, &r44);
		pci_read_config_byte (card->pdev, 0x48, &r48);
		DPRINTK("PCI config: %02X %02X %02X %02X %02X %02X\n",
			r40,r41,r42,r43,r44,r48);

		DPRINTK ("regs==%02X %02X %02X %08X %08X %08X %08X\n",
			 inb (card->baseaddr + 0x00),
			 inb (card->baseaddr + 0x01),
			 inb (card->baseaddr + 0x02),
			 inl (card->baseaddr + 0x04),
			 inl (card->baseaddr + 0x0C),
			 inl (card->baseaddr + 0x80),
			 inl (card->baseaddr + 0x84));
		}

		if (!chan->is_active)
			printk (KERN_ERR "sleeping but not active\n");
#endif

		DPRINTK ("sleeping, nbufs=%d\n", atomic_read (&chan->n_bufs));
		interruptible_sleep_on (&chan->wait);

		if (signal_pending (current)) {
			DPRINTK ("EXIT, returning -ERESTARTSYS\n");
			return -ERESTARTSYS;
		}
	}

#ifdef VIA_DEBUG
	{
		u8 r40,r41,r42,r43,r44,r48;
		pci_read_config_byte (card->pdev, 0x40, &r40);
		pci_read_config_byte (card->pdev, 0x41, &r41);
		pci_read_config_byte (card->pdev, 0x42, &r42);
		pci_read_config_byte (card->pdev, 0x43, &r43);
		pci_read_config_byte (card->pdev, 0x44, &r44);
		pci_read_config_byte (card->pdev, 0x48, &r48);
		DPRINTK("PCI config: %02X %02X %02X %02X %02X %02X\n",
			r40,r41,r42,r43,r44,r48);

		DPRINTK ("regs==%02X %02X %02X %08X %08X %08X %08X\n",
			 inb (card->baseaddr + 0x00),
			 inb (card->baseaddr + 0x01),
			 inb (card->baseaddr + 0x02),
			 inl (card->baseaddr + 0x04),
			 inl (card->baseaddr + 0x0C),
			 inl (card->baseaddr + 0x80),
			 inl (card->baseaddr + 0x84));

		DPRINTK ("final nbufs=%d\n", atomic_read (&chan->n_bufs));
	}
#endif

out:
	DPRINTK ("EXIT, returning 0\n");
	return 0;
}


/**
 *	via_dsp_ioctl_space - get information about channel buffering
 *	@card: Private info for specified board
 *	@chan: pointer to channel-specific info
 *	@arg: user buffer for returned information
 *
 *	Handles SNDCTL_DSP_GETISPACE and SNDCTL_DSP_GETOSPACE.
 *
 *	Locking: inside card->syscall_sem
 */

static int via_dsp_ioctl_space (struct via_info *card,
				struct via_channel *chan,
				void *arg)
{
	audio_buf_info info;

	info.fragstotal = VIA_DMA_BUFFERS;
	info.fragsize = VIA_DMA_BUF_SIZE;

	/* number of full fragments we can read/write without blocking */
	info.fragments = atomic_read (&chan->n_bufs);

	if ((chan->slop_len > 0) && (info.fragments > 0))
		info.fragments--;

	/* number of bytes that can be read or written immediately
	 * without blocking.
	 */
	info.bytes = (info.fragments * VIA_DMA_BUF_SIZE);
	if (chan->slop_len > 0)
		info.bytes += VIA_DMA_BUF_SIZE - chan->slop_len;

	DPRINTK ("EXIT, returning fragstotal=%d, fragsize=%d, fragments=%d, bytes=%d\n",
		info.fragstotal,
		info.fragsize,
		info.fragments,
		info.bytes);

	return copy_to_user (arg, &info, sizeof (info));
}


/**
 *	via_dsp_ioctl_ptr - get information about hardware buffer ptr
 *	@card: Private info for specified board
 *	@chan: pointer to channel-specific info
 *	@arg: user buffer for returned information
 *
 *	Handles SNDCTL_DSP_GETIPTR and SNDCTL_DSP_GETOPTR.
 *
 *	Locking: inside card->syscall_sem
 */

static int via_dsp_ioctl_ptr (struct via_info *card,
				struct via_channel *chan,
				void *arg)
{
	count_info info;

	spin_lock_irq (&card->lock);

	info.bytes = chan->bytes;
	info.blocks = chan->n_irqs;
	chan->n_irqs = 0;

	spin_unlock_irq (&card->lock);

	if (chan->is_active) {
		unsigned long extra;
		info.ptr = atomic_read (&chan->hw_ptr) * VIA_DMA_BUF_SIZE;
		extra = VIA_DMA_BUF_SIZE - inl (chan->iobase + VIA_BASE0_PCM_OUT_BLOCK_COUNT);
		info.ptr += extra;
		info.bytes += extra;
	} else {
		info.ptr = 0;
	}

	DPRINTK ("EXIT, returning bytes=%d, blocks=%d, ptr=%d\n",
		info.bytes,
		info.blocks,
		info.ptr);

	return copy_to_user (arg, &info, sizeof (info));
}


static int via_dsp_ioctl_trigger (struct via_channel *chan, int val)
{
	int enable, do_something;

	if (chan->is_record)
		enable = (val & PCM_ENABLE_INPUT);
	else
		enable = (val & PCM_ENABLE_OUTPUT);

	if (!chan->is_enabled && enable) {
		do_something = 1;
	} else if (chan->is_enabled && !enable) {
		do_something = -1;
	} else {
		do_something = 0;
	}

	DPRINTK ("enable=%d, do_something=%d\n",
		 enable, do_something);

	if (chan->is_active && do_something)
		return -EINVAL;

	if (do_something == 1) {
		chan->is_enabled = 1;
		via_chan_maybe_start (chan);
		DPRINTK ("Triggering input\n");
	}

	else if (do_something == -1) {
		chan->is_enabled = 0;
		DPRINTK ("Setup input trigger\n");
	}

	return 0;
}


static int via_dsp_ioctl (struct inode *inode, struct file *file,
			  unsigned int cmd, unsigned long arg)
{
	int rc, rd=0, wr=0, val=0;
	struct via_info *card;
	struct via_channel *chan;
	int nonblock = (file->f_flags & O_NONBLOCK);

	assert (file != NULL);
	card = file->private_data;
	assert (card != NULL);

	if (file->f_mode & FMODE_WRITE)
		wr = 1;
	if (file->f_mode & FMODE_READ)
		rd = 1;

	rc = via_syscall_down (card, nonblock);
	if (rc)
		return rc;
	rc = -EINVAL;

	switch (cmd) {

	/* OSS API version.  XXX unverified */
	case OSS_GETVERSION:
		DPRINTK("ioctl OSS_GETVERSION, EXIT, returning SOUND_VERSION\n");
		rc = put_user (SOUND_VERSION, (int *)arg);
		break;

	/* list of supported PCM data formats */
	case SNDCTL_DSP_GETFMTS:
		DPRINTK("DSP_GETFMTS, EXIT, returning AFMT U8|S16_LE\n");
                rc = put_user (AFMT_U8 | AFMT_S16_LE, (int *)arg);
		break;

	/* query or set current channel's PCM data format */
	case SNDCTL_DSP_SETFMT:
		if (get_user(val, (int *)arg)) {
			rc = -EFAULT;
			break;
		}
		DPRINTK("DSP_SETFMT, val==%d\n", val);
		if (val != AFMT_QUERY) {
			rc = 0;

			if (rc == 0 && rd)
				rc = via_chan_set_fmt (card, &card->ch_in, val);
			if (rc == 0 && wr)
				rc = via_chan_set_fmt (card, &card->ch_out, val);

			if (rc <= 0) {
				if (rc == 0)
					rc = -EINVAL;
				break;
			}
			val = rc;
		} else {
			if ((rd && (card->ch_in.pcm_fmt & VIA_PCM_FMT_16BIT)) ||
			    (wr && (card->ch_out.pcm_fmt & VIA_PCM_FMT_16BIT)))
				val = AFMT_S16_LE;
			else
				val = AFMT_U8;
		}
		DPRINTK("SETFMT EXIT, returning %d\n", val);
                rc = put_user (val, (int *)arg);
		break;

	/* query or set number of channels (1=mono, 2=stereo) */
        case SNDCTL_DSP_CHANNELS:
		if (get_user(val, (int *)arg)) {
			rc = -EFAULT;
			break;
		}
		DPRINTK("DSP_CHANNELS, val==%d\n", val);
		if (val != 0) {
			rc = 0;
			if (rc == 0 && rd)
				rc = via_chan_set_stereo (card, &card->ch_in, val);
			if (rc == 0 && wr)
				rc = via_chan_set_stereo (card, &card->ch_out, val);
			if (rc <= 0) {
				if (rc == 0)
					rc = -EINVAL;
				break;
			}
			val = rc;
		} else {
			if ((rd && (card->ch_in.pcm_fmt & VIA_PCM_FMT_STEREO)) ||
			    (wr && (card->ch_out.pcm_fmt & VIA_PCM_FMT_STEREO)))
				val = 2;
			else
				val = 1;
		}
		DPRINTK("CHANNELS EXIT, returning %d\n", val);
                rc = put_user (val, (int *)arg);
		break;

	/* enable (val is not zero) or disable (val == 0) stereo */
        case SNDCTL_DSP_STEREO:
		if (get_user(val, (int *)arg)) {
			rc = -EFAULT;
			break;
		}
		DPRINTK("DSP_STEREO, val==%d\n", val);
		rc = 0;

		if (rc == 0 && rd)
			rc = via_chan_set_stereo (card, &card->ch_in, val ? 2 : 1);
		if (rc == 0 && wr)
			rc = via_chan_set_stereo (card, &card->ch_out, val ? 2 : 1);

		if (rc <= 0) {
			if (rc == 0)
				rc = -EINVAL;
			break;
		}
		DPRINTK("STEREO EXIT, returning %d\n", val);
                rc = 0;
		break;

	/* query or set sampling rate */
        case SNDCTL_DSP_SPEED:
		if (get_user(val, (int *)arg)) {
			rc = -EFAULT;
			break;
		}
		DPRINTK("DSP_SPEED, val==%d\n", val);
		if (val < 0) {
			rc = -EINVAL;
			break;
		}
		if (val > 0) {
			rc = 0;

			if (rc == 0 && rd)
				rc = via_chan_set_speed (card, &card->ch_in, val);
			if (rc == 0 && wr)
				rc = via_chan_set_speed (card, &card->ch_out, val);

			if (rc <= 0) {
				if (rc == 0)
					rc = -EINVAL;
				break;
			}
			val = rc;
		} else {
			if (rd)
				val = card->ch_in.rate;
			else if (wr)
				val = card->ch_out.rate;
			else
				val = 0;
		}
		DPRINTK("SPEED EXIT, returning %d\n", val);
                rc = put_user (val, (int *)arg);
		break;

	/* wait until all buffers have been played, and then stop device */
	case SNDCTL_DSP_SYNC:
		DPRINTK ("DSP_SYNC\n");
		if (wr) {
			DPRINTK("SYNC EXIT (after calling via_dsp_drain_playback)\n");
			rc = via_dsp_drain_playback (card, &card->ch_out, nonblock);
		}
		break;

	/* stop recording/playback immediately */
        case SNDCTL_DSP_RESET:
		DPRINTK ("DSP_RESET\n");
		if (rd) {
			via_chan_clear (&card->ch_in);
			via_chan_pcm_fmt (&card->ch_in, 1);
		}
		if (wr) {
			via_chan_clear (&card->ch_out);
			via_chan_pcm_fmt (&card->ch_out, 1);
		}

		rc = 0;
		break;

	/* obtain bitmask of device capabilities, such as mmap, full duplex, etc. */
	case SNDCTL_DSP_GETCAPS:
		DPRINTK("DSP_GETCAPS\n");
		rc = put_user(VIA_DSP_CAP, (int *)arg);
		break;

	/* obtain bitmask of device capabilities, such as mmap, full duplex, etc. */
	case SNDCTL_DSP_GETBLKSIZE:
		DPRINTK("DSP_GETBLKSIZE\n");
		rc = put_user(VIA_DMA_BUF_SIZE, (int *)arg);
		break;

	/* obtain information about input buffering */
	case SNDCTL_DSP_GETISPACE:
		DPRINTK("DSP_GETISPACE\n");
		if (rd)
			rc = via_dsp_ioctl_space (card, &card->ch_in, (void*) arg);
		break;

	/* obtain information about output buffering */
	case SNDCTL_DSP_GETOSPACE:
		DPRINTK("DSP_GETOSPACE\n");
		if (wr)
			rc = via_dsp_ioctl_space (card, &card->ch_out, (void*) arg);
		break;

	/* obtain information about input hardware pointer */
	case SNDCTL_DSP_GETIPTR:
		DPRINTK("DSP_GETIPTR\n");
		if (rd)
			rc = via_dsp_ioctl_ptr (card, &card->ch_in, (void*) arg);
		break;

	/* obtain information about output hardware pointer */
	case SNDCTL_DSP_GETOPTR:
		DPRINTK("DSP_GETOPTR\n");
		if (wr)
			rc = via_dsp_ioctl_ptr (card, &card->ch_out, (void*) arg);
		break;

	/* return number of bytes remaining to be played by DMA engine */
	case SNDCTL_DSP_GETODELAY:
		{
		DPRINTK("DSP_GETODELAY\n");

		chan = &card->ch_out;

		if (!wr)
			break;

		val = VIA_DMA_BUFFERS - atomic_read (&chan->n_bufs);

		if (val > 0) {
			val *= VIA_DMA_BUF_SIZE;
			val -= VIA_DMA_BUF_SIZE -
			       inl (chan->iobase + VIA_BASE0_PCM_OUT_BLOCK_COUNT);
		}
		val += chan->slop_len;

		assert (val <= (VIA_DMA_BUF_SIZE * VIA_DMA_BUFFERS));

		DPRINTK("GETODELAY EXIT, val = %d bytes\n", val);
                rc = put_user (val, (int *)arg);
		break;
		}

	/* handle the quick-start of a channel,
	 * or the notification that a quick-start will
	 * occur in the future
	 */
	case SNDCTL_DSP_SETTRIGGER:
		if (get_user(val, (int *)arg)) {
			rc = -EFAULT;
			break;
		}
		DPRINTK("DSP_SETTRIGGER, rd=%d, wr=%d, act=%d/%d, en=%d/%d\n",
			rd, wr, card->ch_in.is_active, card->ch_out.is_active,
			card->ch_in.is_enabled, card->ch_out.is_enabled);

		rc = 0;

		if (rd)
			rc = via_dsp_ioctl_trigger (&card->ch_in, val);
		if (!rc && wr)
			rc = via_dsp_ioctl_trigger (&card->ch_out, val);

		break;

	/* Enable full duplex.  Since we do this as soon as we are opened
	 * with O_RDWR, this is mainly a no-op that always returns success.
	 */
	case SNDCTL_DSP_SETDUPLEX:
		DPRINTK("DSP_SETDUPLEX\n");
		if (!rd || !wr)
			break;
		rc = 0;
		break;

	/* set fragment size.  implemented as a successful no-op for now */
	case SNDCTL_DSP_SETFRAGMENT:
		if (get_user(val, (int *)arg)) {
			rc = -EFAULT;
			break;
		}
		DPRINTK("DSP_SETFRAGMENT, val==%d\n", val);

		DPRINTK ("SNDCTL_DSP_SETFRAGMENT (fragshift==0x%04X (%d), maxfrags==0x%04X (%d))\n",
			 val & 0xFFFF,
			 val & 0xFFFF,
			 (val >> 16) & 0xFFFF,
			 (val >> 16) & 0xFFFF);

		/* just to shut up some programs */
		rc = 0;
		break;

	/* inform device of an upcoming pause in input (or output). */
	case SNDCTL_DSP_POST:
		DPRINTK("DSP_POST\n");
		if (wr) {
			if (card->ch_out.slop_len > 0)
				via_chan_flush_frag (&card->ch_out);
			via_chan_maybe_start (&card->ch_out);
		}

		rc = 0;
		break;

	/* not implemented */
	default:
		DPRINTK ("unhandled ioctl, cmd==%u, arg==%p\n",
			 cmd, (void*) arg);
		break;
	}

	up (&card->syscall_sem);
	DPRINTK("EXIT, returning %d\n", rc);
	return rc;
}


static int via_dsp_open (struct inode *inode, struct file *file)
{
	int rc, minor = MINOR(inode->i_rdev);
	int got_read_chan = 0;
	struct via_info *card;
	struct pci_dev *pdev;
	struct via_channel *chan;
	struct pci_driver *drvr;
	int nonblock = (file->f_flags & O_NONBLOCK);

	DPRINTK ("ENTER, minor=%d, file->f_mode=0x%x\n", minor, file->f_mode);

	if (!(file->f_mode & (FMODE_READ | FMODE_WRITE))) {
		DPRINTK ("EXIT, returning -EINVAL\n");
		return -EINVAL;
	}

	card = NULL;
	pci_for_each_dev(pdev) {
		drvr = pci_dev_driver (pdev);
		if (drvr == &via_driver) {
			assert (pci_get_drvdata (pdev) != NULL);

			card = pci_get_drvdata (pdev);
			DPRINTK ("dev_dsp = %d, minor = %d, assn = %d\n",
				 card->dev_dsp, minor,
				 (card->dev_dsp ^ minor) & ~0xf);

			if (((card->dev_dsp ^ minor) & ~0xf) == 0)
				goto match;
		}
	}

	DPRINTK ("no matching %s found\n", card ? "minor" : "driver");
	return -ENODEV;

match:
	if (nonblock) {
		if (down_trylock (&card->open_sem)) {
			DPRINTK ("EXIT, returning -EAGAIN\n");
			return -EAGAIN;
		}
	} else {
		if (down_interruptible (&card->open_sem)) {
			DPRINTK ("EXIT, returning -ERESTARTSYS\n");
			return -ERESTARTSYS;
		}
	}

	file->private_data = card;
	DPRINTK("file->f_mode == 0x%x\n", file->f_mode);

	/* handle input from analog source */
	if (file->f_mode & FMODE_READ) {
		chan = &card->ch_in;

		rc = via_chan_init (card, chan);
		if (rc)
			goto err_out;

		got_read_chan = 1;

		/* why is this forced to 16-bit stereo in all drivers? */
		chan->pcm_fmt = VIA_PCM_FMT_16BIT | VIA_PCM_FMT_STEREO;

		via_chan_pcm_fmt (chan, 0);
		via_set_rate (&card->ac97, chan, 44100);
	}

	/* handle output to analog source */
	if (file->f_mode & FMODE_WRITE) {
		chan = &card->ch_out;

		rc = via_chan_init (card, chan);
		if (rc)
			goto err_out_read_chan;

		if ((minor & 0xf) == SND_DEV_DSP16) {
			chan->pcm_fmt |= VIA_PCM_FMT_16BIT;
			via_set_rate (&card->ac97, chan, 44100);
		} else {
			via_set_rate (&card->ac97, chan, 8000);
		}

		via_chan_pcm_fmt (chan, 0);
	}

	DPRINTK ("EXIT, returning 0\n");
	return 0;

err_out_read_chan:
	if (got_read_chan)
		via_chan_free (card, &card->ch_in);
err_out:
	up (&card->open_sem);
	DPRINTK("ERROR EXIT, returning %d\n", rc);
	return rc;
}


static int via_dsp_release(struct inode *inode, struct file *file)
{
	struct via_info *card;
	int nonblock = (file->f_flags & O_NONBLOCK);
	int rc;

	DPRINTK ("ENTER\n");

	assert (file != NULL);
	card = file->private_data;
	assert (card != NULL);

	rc = via_syscall_down (card, nonblock);
	if (rc) {
		DPRINTK ("EXIT (syscall_down error), rc=%d\n", rc);
		return rc;
	}

	if (file->f_mode & FMODE_WRITE) {
		rc = via_dsp_drain_playback (card, &card->ch_out, nonblock);
		if (rc)
			printk (KERN_DEBUG "via_audio: ignoring drain playback error %d\n", rc);

		via_chan_free (card, &card->ch_out);
	}

	if (file->f_mode & FMODE_READ)
		via_chan_free (card, &card->ch_in);

	up (&card->syscall_sem);
	up (&card->open_sem);

	DPRINTK("EXIT, returning 0\n");
	return 0;
}


/****************************************************************
 *
 * Chip setup and kernel registration
 *
 *
 */

static int __init via_init_one (struct pci_dev *pdev, const struct pci_device_id *id)
{
	int rc;
	struct via_info *card;
	u8 tmp;
	static int printed_version = 0;

	DPRINTK ("ENTER\n");

	if (printed_version++ == 0)
		printk (KERN_INFO "Via 686a audio driver " VIA_VERSION "\n");

	if (!request_region (pci_resource_start (pdev, 0),
	    		     pci_resource_len (pdev, 0),
			     VIA_MODULE_NAME)) {
		printk (KERN_ERR PFX "unable to obtain I/O resources, aborting\n");
		rc = -EBUSY;
		goto err_out;
	}

	if (pci_enable_device (pdev)) {
		rc = -EIO;
		goto err_out_none;
	}

	card = kmalloc (sizeof (*card), GFP_KERNEL);
	if (!card) {
		printk (KERN_ERR PFX "out of memory, aborting\n");
		rc = -ENOMEM;
		goto err_out_none;
	}

	pci_set_drvdata (pdev, card);

	memset (card, 0, sizeof (*card));
	card->pdev = pdev;
	card->baseaddr = pci_resource_start (pdev, 0);
	card->card_num = via_num_cards++;
	spin_lock_init (&card->lock);
	init_MUTEX (&card->syscall_sem);
	init_MUTEX (&card->open_sem);

	/* we must init these now, in case the intr handler needs them */
	via_chan_init_defaults (card, &card->ch_out);
	via_chan_init_defaults (card, &card->ch_in);
	via_chan_init_defaults (card, &card->ch_fm);

	/* if BAR 2 is present, chip is Rev H or later,
	 * which means it has a few extra features */
	if (pci_resource_start (pdev, 2) > 0)
		card->rev_h = 1;

	if (pdev->irq < 1) {
		printk (KERN_ERR PFX "invalid PCI IRQ %d, aborting\n", pdev->irq);
		rc = -ENODEV;
		goto err_out_kfree;
	}

	if (!(pci_resource_flags (pdev, 0) & IORESOURCE_IO)) {
		printk (KERN_ERR PFX "unable to locate I/O resources, aborting\n");
		rc = -ENODEV;
		goto err_out_kfree;
	}

	/*
	 * init AC97 mixer and codec
	 */
	rc = via_ac97_init (card);
	if (rc) {
		printk (KERN_ERR PFX "AC97 init failed, aborting\n");
		goto err_out_kfree;
	}

	/*
	 * init DSP device
	 */
	rc = via_dsp_init (card);
	if (rc) {
		printk (KERN_ERR PFX "DSP device init failed, aborting\n");
		goto err_out_have_mixer;
	}

	/*
	 * per-card /proc info
	 */
	rc = via_card_init_proc (card);
	if (rc) {
		printk (KERN_ERR PFX "card-specific /proc init failed, aborting\n");
		goto err_out_have_dsp;
	}

	/*
	 * init and turn on interrupts, as the last thing we do
	 */
	rc = via_interrupt_init (card);
	if (rc) {
		printk (KERN_ERR PFX "interrupt init failed, aborting\n");
		goto err_out_have_proc;
	}

	pci_read_config_byte (pdev, 0x3C, &tmp);
	if ((tmp & 0x0F) != pdev->irq) {
		printk (KERN_WARNING PFX "IRQ fixup, 0x3C==0x%02X\n", tmp);
		udelay (15);
		tmp &= 0xF0;
		tmp |= pdev->irq;
		pci_write_config_byte (pdev, 0x3C, tmp);
		DPRINTK("new 0x3c==0x%02x\n", tmp);
	} else {
		DPRINTK("IRQ reg 0x3c==0x%02x, irq==%d\n",
			tmp, tmp & 0x0F);
	}

	printk (KERN_INFO PFX "board #%d at 0x%04lX, IRQ %d\n",
		card->card_num + 1, card->baseaddr, pdev->irq);

	DPRINTK ("EXIT, returning 0\n");
	return 0;

err_out_have_proc:
	via_card_cleanup_proc (card);

err_out_have_dsp:
	via_dsp_cleanup (card);

err_out_have_mixer:
	via_ac97_cleanup (card);

err_out_kfree:
#ifndef VIA_NDEBUG
	memset (card, 0xAB, sizeof (*card)); /* poison memory */
#endif
	kfree (card);

err_out_none:
	release_region (pci_resource_start (pdev, 0), pci_resource_len (pdev, 0));
err_out:
	pci_set_drvdata (pdev, NULL);
	DPRINTK ("EXIT - returning %d\n", rc);
	return rc;
}


static void __exit via_remove_one (struct pci_dev *pdev)
{
	struct via_info *card;

	DPRINTK ("ENTER\n");

	assert (pdev != NULL);
	card = pci_get_drvdata (pdev);
	assert (card != NULL);

	via_interrupt_cleanup (card);
	via_card_cleanup_proc (card);
	via_dsp_cleanup (card);
	via_ac97_cleanup (card);

	release_region (pci_resource_start (pdev, 0), pci_resource_len (pdev, 0));

#ifndef VIA_NDEBUG
	memset (card, 0xAB, sizeof (*card)); /* poison memory */
#endif
	kfree (card);

	pci_set_drvdata (pdev, NULL);

	pci_set_power_state (pdev, 3); /* ...zzzzzz */

	DPRINTK ("EXIT\n");
	return;
}


/****************************************************************
 *
 * Driver initialization and cleanup
 *
 *
 */

static int __init init_via82cxxx_audio(void)
{
	int rc;

	DPRINTK ("ENTER\n");

	rc = via_init_proc ();
	if (rc) {
		DPRINTK ("EXIT, returning %d\n", rc);
		return rc;
	}

	rc = pci_register_driver (&via_driver);
	if (rc < 1) {
		if (rc == 0)
			pci_unregister_driver (&via_driver);
		via_cleanup_proc ();
		DPRINTK ("EXIT, returning -ENODEV\n");
		return -ENODEV;
	}

	DPRINTK ("EXIT, returning 0\n");
	return 0;
}


static void __exit cleanup_via82cxxx_audio(void)
{
	DPRINTK("ENTER\n");

	pci_unregister_driver (&via_driver);
	via_cleanup_proc ();

	DPRINTK("EXIT\n");
}


module_init(init_via82cxxx_audio);
module_exit(cleanup_via82cxxx_audio);

MODULE_AUTHOR("Jeff Garzik <jgarzik@mandrakesoft.com>");
MODULE_DESCRIPTION("DSP audio and mixer driver for Via 82Cxxx audio devices");
EXPORT_NO_SYMBOLS;



#ifdef VIA_PROC_FS

/****************************************************************
 *
 * /proc/driver/via/info
 *
 *
 */

static int via_info_read_proc (char *page, char **start, off_t off,
			       int count, int *eof, void *data)
{
#define YN(val,bit) (((val) & (bit)) ? "yes" : "no")
#define ED(val,bit) (((val) & (bit)) ? "enable" : "disable")

	int len = 0;
	u8 r40, r41, r42, r44;
	struct via_info *card = data;

	DPRINTK ("ENTER\n");

	assert (card != NULL);

	len += sprintf (page+len, VIA_CARD_NAME "\n\n");

	pci_read_config_byte (card->pdev, 0x40, &r40);
	pci_read_config_byte (card->pdev, 0x41, &r41);
	pci_read_config_byte (card->pdev, 0x42, &r42);
	pci_read_config_byte (card->pdev, 0x44, &r44);

	len += sprintf (page+len,
		"Via 82Cxxx PCI registers:\n"
		"\n"
		"40  Codec Ready: %s\n"
		"    Codec Low-power: %s\n"
		"    Secondary Codec Ready: %s\n"
		"\n"
		"41  Interface Enable: %s\n"
		"    De-Assert Reset: %s\n"
		"    Force SYNC high: %s\n"
		"    Force SDO high: %s\n"
		"    Variable Sample Rate On-Demand Mode: %s\n"
		"    SGD Read Channel PCM Data Out: %s\n"
		"    FM Channel PCM Data Out: %s\n"
		"    SB PCM Data Out: %s\n"
		"\n"
		"42  Game port enabled: %s\n"
		"    SoundBlaster enabled: %s\n"
		"    FM enabled: %s\n"
		"    MIDI enabled: %s\n"
		"\n"
		"44  AC-Link Interface Access: %s\n"
		"    Secondary Codec Support: %s\n"

		"\n",

		YN (r40, VIA_CR40_AC97_READY),
		YN (r40, VIA_CR40_AC97_LOW_POWER),
		YN (r40, VIA_CR40_SECONDARY_READY),

		ED (r41, VIA_CR41_AC97_ENABLE),
		YN (r41, (1 << 6)),
		YN (r41, (1 << 5)),
		YN (r41, (1 << 4)),
		ED (r41, (1 << 3)),
		ED (r41, (1 << 2)),
		ED (r41, (1 << 1)),
		ED (r41, (1 << 0)),

		YN (r42, VIA_CR42_GAME_ENABLE),
		YN (r42, VIA_CR42_SB_ENABLE),
		YN (r42, VIA_CR42_FM_ENABLE),
		YN (r42, VIA_CR42_MIDI_ENABLE),

		YN (r44, VIA_CR44_AC_LINK_ACCESS),
		YN (r44, VIA_CR44_SECOND_CODEC_SUPPORT)

		);

	DPRINTK("EXIT, returning %d\n", len);
	return len;

#undef YN
#undef ED
}


/****************************************************************
 *
 * /proc/driver/via/... setup and cleanup
 *
 *
 */

static int __init via_init_proc (void)
{
	DPRINTK ("ENTER\n");

	if (!proc_mkdir ("driver/via", 0))
		return -EIO;

	DPRINTK ("EXIT, returning 0\n");
	return 0;
}


static void via_cleanup_proc (void)
{
	DPRINTK ("ENTER\n");

	remove_proc_entry ("driver/via", NULL);

	DPRINTK ("EXIT\n");
}


static int __init via_card_init_proc (struct via_info *card)
{
	char s[32];
	int rc;

	DPRINTK ("ENTER\n");

	sprintf (s, "driver/via/%d", card->card_num);
	if (!proc_mkdir (s, 0)) {
		rc = -EIO;
		goto err_out_none;
	}

	sprintf (s, "driver/via/%d/info", card->card_num);
	if (!create_proc_read_entry (s, 0, 0, via_info_read_proc, card)) {
		rc = -EIO;
		goto err_out_dir;
	}

	sprintf (s, "driver/via/%d/ac97", card->card_num);
	if (!create_proc_read_entry (s, 0, 0, ac97_read_proc, &card->ac97)) {
		rc = -EIO;
		goto err_out_info;
	}

	DPRINTK ("EXIT, returning 0\n");
	return 0;

err_out_info:
	sprintf (s, "driver/via/%d/info", card->card_num);
	remove_proc_entry (s, NULL);

err_out_dir:
	sprintf (s, "driver/via/%d", card->card_num);
	remove_proc_entry (s, NULL);

err_out_none:
	DPRINTK ("EXIT, returning %d\n", rc);
	return rc;
}


static void via_card_cleanup_proc (struct via_info *card)
{
	char s[32];

	DPRINTK ("ENTER\n");

	sprintf (s, "driver/via/%d/ac97", card->card_num);
	remove_proc_entry (s, NULL);

	sprintf (s, "driver/via/%d/info", card->card_num);
	remove_proc_entry (s, NULL);

	sprintf (s, "driver/via/%d", card->card_num);
	remove_proc_entry (s, NULL);

	DPRINTK ("EXIT\n");
}

#endif /* VIA_PROC_FS */
