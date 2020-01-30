/*
 *   ALSA modem driver for Intel ICH (i8x0) chipsets
 *
 *	Copyright (c) 2000 Jaroslav Kysela <perex@suse.cz>
 *
 *   This is modified (by Sasha Khapyorsky <sashak@smlink.com>) version
 *   of ALSA ICH sound driver intel8x0.c .
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */      

#include <sound/driver.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/gameport.h>
#include <linux/moduleparam.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/ac97_codec.h>
#include <sound/info.h>
#include <sound/mpu401.h>
#include <sound/initval.h>

MODULE_AUTHOR("Jaroslav Kysela <perex@suse.cz>");
MODULE_DESCRIPTION("Intel 82801AA,82901AB,i810,i820,i830,i840,i845,MX440 modem");
MODULE_LICENSE("GPL");
MODULE_CLASSES("{sound}");
MODULE_DEVICES("{{Intel,82801AA-ICH},"
		"{Intel,82901AB-ICH0},"
		"{Intel,82801BA-ICH2},"
		"{Intel,82801CA-ICH3},"
		"{Intel,82801DB-ICH4},"
		"{Intel,ICH5},"
	        "{Intel,MX440}}");


static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;	/* Index 0-MAX */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;	/* ID for this card */
static int enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;	/* Enable this card */
static int ac97_clock[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = 0};
static int boot_devs;

module_param_array(index, int, boot_devs, 0444);
MODULE_PARM_DESC(index, "Index value for Intel i8x0 modemcard.");
MODULE_PARM_SYNTAX(index, SNDRV_INDEX_DESC);
module_param_array(id, charp, boot_devs, 0444);
MODULE_PARM_DESC(id, "ID string for Intel i8x0 modemcard.");
MODULE_PARM_SYNTAX(id, SNDRV_ID_DESC);
module_param_array(enable, bool, boot_devs, 0444);
MODULE_PARM_DESC(enable, "Enable Intel i8x0 modemcard.");
MODULE_PARM_SYNTAX(enable, SNDRV_ENABLE_DESC);
module_param_array(ac97_clock, int, boot_devs, 0444);
MODULE_PARM_DESC(ac97_clock, "AC'97 codec clock (0 = auto-detect).");
MODULE_PARM_SYNTAX(ac97_clock, SNDRV_ENABLED ",default:0");

/*
 *  Direct registers
 */

#ifndef PCI_DEVICE_ID_INTEL_82801_6
#define PCI_DEVICE_ID_INTEL_82801_6     0x2416
#endif
#ifndef PCI_DEVICE_ID_INTEL_82901_6
#define PCI_DEVICE_ID_INTEL_82901_6     0x2426
#endif
#ifndef PCI_DEVICE_ID_INTEL_82801BA_6
#define PCI_DEVICE_ID_INTEL_82801BA_6   0x2446
#endif
#ifndef PCI_DEVICE_ID_INTEL_440MX_6
#define PCI_DEVICE_ID_INTEL_440MX_6     0x7196
#endif
#ifndef PCI_DEVICE_ID_INTEL_ICH3_6
#define PCI_DEVICE_ID_INTEL_ICH3_6	0x2486
#endif
#ifndef PCI_DEVICE_ID_INTEL_ICH4_6
#define PCI_DEVICE_ID_INTEL_ICH4_6	0x24c6
#endif
#ifndef PCI_DEVICE_ID_INTEL_ICH5_6
#define PCI_DEVICE_ID_INTEL_ICH5_6	0x24d6
#endif
#ifndef PCI_DEVICE_ID_SI_7013
#define PCI_DEVICE_ID_SI_7013		0x7013
#endif
#if 0
#ifndef PCI_DEVICE_ID_NVIDIA_MCP_AUDIO
#define PCI_DEVICE_ID_NVIDIA_MCP_AUDIO	0x01b1
#endif
#ifndef PCI_DEVICE_ID_NVIDIA_MCP2_AUDIO
#define PCI_DEVICE_ID_NVIDIA_MCP2_AUDIO	0x006a
#endif
#ifndef PCI_DEVICE_ID_NVIDIA_MCP3_AUDIO
#define PCI_DEVICE_ID_NVIDIA_MCP3_AUDIO	0x00da
#endif
#endif

enum { DEVICE_INTEL, DEVICE_SIS, DEVICE_ALI, DEVICE_NFORCE };

#define ICHREG(x) ICH_REG_##x

#define DEFINE_REGSET(name,base) \
enum { \
	ICH_REG_##name##_BDBAR	= base + 0x0,	/* dword - buffer descriptor list base address */ \
	ICH_REG_##name##_CIV	= base + 0x04,	/* byte - current index value */ \
	ICH_REG_##name##_LVI	= base + 0x05,	/* byte - last valid index */ \
	ICH_REG_##name##_SR	= base + 0x06,	/* byte - status register */ \
	ICH_REG_##name##_PICB	= base + 0x08,	/* word - position in current buffer */ \
	ICH_REG_##name##_PIV	= base + 0x0a,	/* byte - prefetched index value */ \
	ICH_REG_##name##_CR	= base + 0x0b,	/* byte - control register */ \
};

/* busmaster blocks */
DEFINE_REGSET(OFF, 0);		/* offset */

/* values for each busmaster block */

/* LVI */
#define ICH_REG_LVI_MASK		0x1f

/* SR */
#define ICH_FIFOE			0x10	/* FIFO error */
#define ICH_BCIS			0x08	/* buffer completion interrupt status */
#define ICH_LVBCI			0x04	/* last valid buffer completion interrupt */
#define ICH_CELV			0x02	/* current equals last valid */
#define ICH_DCH				0x01	/* DMA controller halted */

/* PIV */
#define ICH_REG_PIV_MASK		0x1f	/* mask */

/* CR */
#define ICH_IOCE			0x10	/* interrupt on completion enable */
#define ICH_FEIE			0x08	/* fifo error interrupt enable */
#define ICH_LVBIE			0x04	/* last valid buffer interrupt enable */
#define ICH_RESETREGS			0x02	/* reset busmaster registers */
#define ICH_STARTBM			0x01	/* start busmaster operation */


/* global block */
#define ICH_REG_GLOB_CNT		0x3c	/* dword - global control */
#define   ICH_TRIE		0x00000040	/* tertiary resume interrupt enable */
#define   ICH_SRIE		0x00000020	/* secondary resume interrupt enable */
#define   ICH_PRIE		0x00000010	/* primary resume interrupt enable */
#define   ICH_ACLINK		0x00000008	/* AClink shut off */
#define   ICH_AC97WARM		0x00000004	/* AC'97 warm reset */
#define   ICH_AC97COLD		0x00000002	/* AC'97 cold reset */
#define   ICH_GIE		0x00000001	/* GPI interrupt enable */
#define ICH_REG_GLOB_STA		0x40	/* dword - global status */
#define   ICH_TRI		0x20000000	/* ICH4: tertiary (AC_SDIN2) resume interrupt */
#define   ICH_TCR		0x10000000	/* ICH4: tertiary (AC_SDIN2) codec ready */
#define   ICH_BCS		0x08000000	/* ICH4: bit clock stopped */
#define   ICH_SPINT		0x04000000	/* ICH4: S/PDIF interrupt */
#define   ICH_P2INT		0x02000000	/* ICH4: PCM2-In interrupt */
#define   ICH_M2INT		0x01000000	/* ICH4: Mic2-In interrupt */
#define   ICH_SAMPLE_CAP	0x00c00000	/* ICH4: sample capability bits (RO) */
#define   ICH_MULTICHAN_CAP	0x00300000	/* ICH4: multi-channel capability bits (RO) */
#define   ICH_MD3		0x00020000	/* modem power down semaphore */
#define   ICH_AD3		0x00010000	/* audio power down semaphore */
#define   ICH_RCS		0x00008000	/* read completion status */
#define   ICH_BIT3		0x00004000	/* bit 3 slot 12 */
#define   ICH_BIT2		0x00002000	/* bit 2 slot 12 */
#define   ICH_BIT1		0x00001000	/* bit 1 slot 12 */
#define   ICH_SRI		0x00000800	/* secondary (AC_SDIN1) resume interrupt */
#define   ICH_PRI		0x00000400	/* primary (AC_SDIN0) resume interrupt */
#define   ICH_SCR		0x00000200	/* secondary (AC_SDIN1) codec ready */
#define   ICH_PCR		0x00000100	/* primary (AC_SDIN0) codec ready */
#define   ICH_MCINT		0x00000080	/* MIC capture interrupt */
#define   ICH_POINT		0x00000040	/* playback interrupt */
#define   ICH_PIINT		0x00000020	/* capture interrupt */
#define   ICH_NVSPINT		0x00000010	/* nforce spdif interrupt */
#define   ICH_MOINT		0x00000004	/* modem playback interrupt */
#define   ICH_MIINT		0x00000002	/* modem capture interrupt */
#define   ICH_GSCI		0x00000001	/* GPI status change interrupt */
#define ICH_REG_ACC_SEMA		0x44	/* byte - codec write semaphore */
#define   ICH_CAS		0x01		/* codec access semaphore */

#define ICH_MAX_FRAGS		32		/* max hw frags */


/*
 *  
 */

enum { ICHD_MDMIN, ICHD_MDMOUT, ICHD_MDMLAST = ICHD_MDMOUT };
enum { ALID_MDMIN, ALID_MDMOUT, ALID_MDMLAST = ALID_MDMOUT };

#define get_ichdev(substream) (ichdev_t *)(substream->runtime->private_data)

typedef struct {
	unsigned int ichd;			/* ich device number */
	unsigned long reg_offset;		/* offset to bmaddr */
	u32 *bdbar;				/* CPU address (32bit) */
	unsigned int bdbar_addr;		/* PCI bus address (32bit) */
	snd_pcm_substream_t *substream;
	unsigned int physbuf;			/* physical address (32bit) */
        unsigned int size;
        unsigned int fragsize;
        unsigned int fragsize1;
        unsigned int position;
        int frags;
        int lvi;
        int lvi_frag;
	int civ;
	int ack;
	int ack_reload;
	unsigned int ack_bit;
	unsigned int roff_sr;
	unsigned int roff_picb;
	unsigned int int_sta_mask;		/* interrupt status mask */
	unsigned int ali_slot;			/* ALI DMA slot */
	ac97_t *ac97;
} ichdev_t;

typedef struct _snd_intel8x0m intel8x0_t;
#define chip_t intel8x0_t

struct _snd_intel8x0m {
	unsigned int device_type;
	char ac97_name[64];
	char ctrl_name[64];

	int irq;

	unsigned int mmio;
	unsigned long addr;
	unsigned long remap_addr;
	struct resource *res;
	unsigned int bm_mmio;
	unsigned long bmaddr;
	unsigned long remap_bmaddr;
	struct resource *res_bm;

	struct pci_dev *pci;
	snd_card_t *card;

	int pcm_devs;
	snd_pcm_t *pcm[2];
	ichdev_t ichd[2];

	int in_ac97_init: 1;

	ac97_bus_t *ac97_bus;
	ac97_t *ac97;

	spinlock_t reg_lock;
	spinlock_t ac97_lock;
	
	struct snd_dma_device dma_dev;
	struct snd_dma_buffer bdbars;
	u32 bdbars_count;
	u32 int_sta_reg;		/* interrupt status register */
	u32 int_sta_mask;		/* interrupt status mask */
	unsigned int pcm_pos_shift;
};

static struct pci_device_id snd_intel8x0m_ids[] = {
	{ 0x8086, 0x2416, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL },	/* 82801AA */
	{ 0x8086, 0x2426, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL },	/* 82901AB */
	{ 0x8086, 0x2446, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL },	/* 82801BA */
	{ 0x8086, 0x2486, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL },	/* ICH3 */
	{ 0x8086, 0x24c6, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL }, /* ICH4 */
	{ 0x8086, 0x24d6, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL }, /* ICH5 */
	{ 0x8086, 0x7196, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL },	/* 440MX */
	{ 0x1022, 0x7446, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL },	/* AMD768 */
#if 0
	/* TODO: support needed */
	{ 0x1039, 0x7013, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_SIS },	/* SI7013 */
	{ 0x10de, 0x01b1, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_NFORCE },	/* NFORCE */
	{ 0x10de, 0x006a, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_NFORCE },	/* NFORCE2 */
	{ 0x10de, 0x00da, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_NFORCE },	/* NFORCE3 */
	{ 0x1022, 0x746d, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL },	/* AMD8111 */
	{ 0x10b9, 0x5455, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_ALI },   /* Ali5455 */
#endif
	{ 0, }
};

MODULE_DEVICE_TABLE(pci, snd_intel8x0m_ids);

/*
 *  Lowlevel I/O - busmaster
 */

static u8 igetbyte(intel8x0_t *chip, u32 offset)
{
	if (chip->bm_mmio)
		return readb(chip->remap_bmaddr + offset);
	else
		return inb(chip->bmaddr + offset);
}

static u16 igetword(intel8x0_t *chip, u32 offset)
{
	if (chip->bm_mmio)
		return readw(chip->remap_bmaddr + offset);
	else
		return inw(chip->bmaddr + offset);
}

static u32 igetdword(intel8x0_t *chip, u32 offset)
{
	if (chip->bm_mmio)
		return readl(chip->remap_bmaddr + offset);
	else
		return inl(chip->bmaddr + offset);
}

static void iputbyte(intel8x0_t *chip, u32 offset, u8 val)
{
	if (chip->bm_mmio)
		writeb(val, chip->remap_bmaddr + offset);
	else
		outb(val, chip->bmaddr + offset);
}

static void iputdword(intel8x0_t *chip, u32 offset, u32 val)
{
	if (chip->bm_mmio)
		writel(val, chip->remap_bmaddr + offset);
	else
		outl(val, chip->bmaddr + offset);
}

/*
 *  Lowlevel I/O - AC'97 registers
 */

static u16 iagetword(intel8x0_t *chip, u32 offset)
{
	if (chip->mmio)
		return readw(chip->remap_addr + offset);
	else
		return inw(chip->addr + offset);
}

static void iaputword(intel8x0_t *chip, u32 offset, u16 val)
{
	if (chip->mmio)
		writew(val, chip->remap_addr + offset);
	else
		outw(val, chip->addr + offset);
}

/*
 *  Basic I/O
 */

/*
 * access to AC97 codec via normal i/o (for ICH and SIS7013)
 */

/* return the GLOB_STA bit for the corresponding codec */
static unsigned int get_ich_codec_bit(intel8x0_t *chip, unsigned int codec)
{
	static unsigned int codec_bit[3] = {
		ICH_PCR, ICH_SCR, ICH_TCR
	};
	snd_assert(codec < 3, return ICH_PCR);
	return codec_bit[codec];
}

static int snd_intel8x0m_codec_semaphore(intel8x0_t *chip, unsigned int codec)
{
	int time;
	
	if (codec > 1)
		return -EIO;
	codec = get_ich_codec_bit(chip, codec);

	/* codec ready ? */
	if ((igetdword(chip, ICHREG(GLOB_STA)) & codec) == 0)
		return -EIO;

	/* Anyone holding a semaphore for 1 msec should be shot... */
	time = 100;
      	do {
      		if (!(igetbyte(chip, ICHREG(ACC_SEMA)) & ICH_CAS))
      			return 0;
		udelay(10);
	} while (time--);

	/* access to some forbidden (non existant) ac97 registers will not
	 * reset the semaphore. So even if you don't get the semaphore, still
	 * continue the access. We don't need the semaphore anyway. */
	snd_printk("codec_semaphore: semaphore is not ready [0x%x][0x%x]\n",
			igetbyte(chip, ICHREG(ACC_SEMA)), igetdword(chip, ICHREG(GLOB_STA)));
	iagetword(chip, 0);	/* clear semaphore flag */
	/* I don't care about the semaphore */
	return -EBUSY;
}
 
static void snd_intel8x0_codec_write(ac97_t *ac97,
				     unsigned short reg,
				     unsigned short val)
{
	intel8x0_t *chip = snd_magic_cast(intel8x0_t, ac97->private_data, return);
	
	spin_lock(&chip->ac97_lock);
	if (snd_intel8x0m_codec_semaphore(chip, ac97->num) < 0) {
		if (! chip->in_ac97_init)
			snd_printk("codec_write %d: semaphore is not ready for register 0x%x\n", ac97->num, reg);
	}
	iaputword(chip, reg + ac97->num * 0x80, val);
	spin_unlock(&chip->ac97_lock);
}

static unsigned short snd_intel8x0_codec_read(ac97_t *ac97,
					      unsigned short reg)
{
	intel8x0_t *chip = snd_magic_cast(intel8x0_t, ac97->private_data, return ~0);
	unsigned short res;
	unsigned int tmp;

	spin_lock(&chip->ac97_lock);
	if (snd_intel8x0m_codec_semaphore(chip, ac97->num) < 0) {
		if (! chip->in_ac97_init)
			snd_printk("codec_read %d: semaphore is not ready for register 0x%x\n", ac97->num, reg);
		res = 0xffff;
	} else {
		res = iagetword(chip, reg + ac97->num * 0x80);
		if ((tmp = igetdword(chip, ICHREG(GLOB_STA))) & ICH_RCS) {
			/* reset RCS and preserve other R/WC bits */
			iputdword(chip, ICHREG(GLOB_STA), tmp & ~(ICH_SRI|ICH_PRI|ICH_TRI|ICH_GSCI));
			if (! chip->in_ac97_init)
				snd_printk("codec_read %d: read timeout for register 0x%x\n", ac97->num, reg);
			res = 0xffff;
		}
	}
	spin_unlock(&chip->ac97_lock);
	return res;
}


/*
 * DMA I/O
 */
static void snd_intel8x0_setup_periods(intel8x0_t *chip, ichdev_t *ichdev) 
{
	int idx;
	u32 *bdbar = ichdev->bdbar;
	unsigned long port = ichdev->reg_offset;

	iputdword(chip, port + ICH_REG_OFF_BDBAR, ichdev->bdbar_addr);
	if (ichdev->size == ichdev->fragsize) {
		ichdev->ack_reload = ichdev->ack = 2;
		ichdev->fragsize1 = ichdev->fragsize >> 1;
		for (idx = 0; idx < (ICH_REG_LVI_MASK + 1) * 2; idx += 4) {
			bdbar[idx + 0] = cpu_to_le32(ichdev->physbuf);
			bdbar[idx + 1] = cpu_to_le32(0x80000000 | /* interrupt on completion */
						     ichdev->fragsize1 >> chip->pcm_pos_shift);
			bdbar[idx + 2] = cpu_to_le32(ichdev->physbuf + (ichdev->size >> 1));
			bdbar[idx + 3] = cpu_to_le32(0x80000000 | /* interrupt on completion */
						     ichdev->fragsize1 >> chip->pcm_pos_shift);
		}
		ichdev->frags = 2;
	} else {
		ichdev->ack_reload = ichdev->ack = 1;
		ichdev->fragsize1 = ichdev->fragsize;
		for (idx = 0; idx < (ICH_REG_LVI_MASK + 1) * 2; idx += 2) {
			bdbar[idx + 0] = cpu_to_le32(ichdev->physbuf + (((idx >> 1) * ichdev->fragsize) % ichdev->size));
			bdbar[idx + 1] = cpu_to_le32(0x80000000 | /* interrupt on completion */
						     ichdev->fragsize >> chip->pcm_pos_shift);
			// printk("bdbar[%i] = 0x%x [0x%x]\n", idx + 0, bdbar[idx + 0], bdbar[idx + 1]);
		}
		ichdev->frags = ichdev->size / ichdev->fragsize;
	}
	iputbyte(chip, port + ICH_REG_OFF_LVI, ichdev->lvi = ICH_REG_LVI_MASK);
	ichdev->civ = 0;
	iputbyte(chip, port + ICH_REG_OFF_CIV, 0);
	ichdev->lvi_frag = ICH_REG_LVI_MASK % ichdev->frags;
	ichdev->position = 0;
#if 0
	printk("lvi_frag = %i, frags = %i, period_size = 0x%x, period_size1 = 0x%x\n",
			ichdev->lvi_frag, ichdev->frags, ichdev->fragsize, ichdev->fragsize1);
#endif
	/* clear interrupts */
	iputbyte(chip, port + ichdev->roff_sr, ICH_FIFOE | ICH_BCIS | ICH_LVBCI);
}

/*
 *  Interrupt handler
 */

static inline void snd_intel8x0_update(intel8x0_t *chip, ichdev_t *ichdev)
{
	unsigned long port = ichdev->reg_offset;
	int civ, i, step;
	int ack = 0;

	civ = igetbyte(chip, port + ICH_REG_OFF_CIV);
	if (civ == ichdev->civ) {
		// snd_printd("civ same %d\n", civ);
		step = 1;
		ichdev->civ++;
		ichdev->civ &= ICH_REG_LVI_MASK;
	} else {
		step = civ - ichdev->civ;
		if (step < 0)
			step += ICH_REG_LVI_MASK + 1;
		// if (step != 1)
		//	snd_printd("step = %d, %d -> %d\n", step, ichdev->civ, civ);
		ichdev->civ = civ;
	}

	ichdev->position += step * ichdev->fragsize1;
	ichdev->position %= ichdev->size;
	ichdev->lvi += step;
	ichdev->lvi &= ICH_REG_LVI_MASK;
	iputbyte(chip, port + ICH_REG_OFF_LVI, ichdev->lvi);
	for (i = 0; i < step; i++) {
		ichdev->lvi_frag++;
		ichdev->lvi_frag %= ichdev->frags;
		ichdev->bdbar[ichdev->lvi * 2] = cpu_to_le32(ichdev->physbuf + ichdev->lvi_frag * ichdev->fragsize1);
	// printk("new: bdbar[%i] = 0x%x [0x%x], prefetch = %i, all = 0x%x, 0x%x\n", ichdev->lvi * 2, ichdev->bdbar[ichdev->lvi * 2], ichdev->bdbar[ichdev->lvi * 2 + 1], inb(ICH_REG_OFF_PIV + port), inl(port + 4), inb(port + ICH_REG_OFF_CR));
		if (--ichdev->ack == 0) {
			ichdev->ack = ichdev->ack_reload;
			ack = 1;
		}
	}
	if (ack && ichdev->substream) {
		spin_unlock(&chip->reg_lock);
		snd_pcm_period_elapsed(ichdev->substream);
		spin_lock(&chip->reg_lock);
	}
	iputbyte(chip, port + ichdev->roff_sr, ICH_FIFOE | ICH_BCIS | ICH_LVBCI);
}

static irqreturn_t snd_intel8x0_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	intel8x0_t *chip = snd_magic_cast(intel8x0_t, dev_id, return IRQ_NONE);
	ichdev_t *ichdev;
	unsigned int status;
	unsigned int i;

	spin_lock(&chip->reg_lock);
	status = igetdword(chip, chip->int_sta_reg);
	if ((status & chip->int_sta_mask) == 0) {
		if (status)
			iputdword(chip, chip->int_sta_reg, status);
		spin_unlock(&chip->reg_lock);
		return IRQ_NONE;
	}

	for (i = 0; i < chip->bdbars_count; i++) {
		ichdev = &chip->ichd[i];
		if (status & ichdev->int_sta_mask)
			snd_intel8x0_update(chip, ichdev);
	}

	/* ack them */
	iputdword(chip, chip->int_sta_reg, status & chip->int_sta_mask);
	spin_unlock(&chip->reg_lock);
	
	return IRQ_HANDLED;
}

/*
 *  PCM part
 */

static int snd_intel8x0_pcm_trigger(snd_pcm_substream_t *substream, int cmd)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);
	ichdev_t *ichdev = get_ichdev(substream);
	unsigned char val = 0;
	unsigned long port = ichdev->reg_offset;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		val = ICH_IOCE | ICH_STARTBM;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		val = 0;
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		val = ICH_IOCE;
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		val = ICH_IOCE | ICH_STARTBM;
		break;
	default:
		return -EINVAL;
	}
	iputbyte(chip, port + ICH_REG_OFF_CR, val);
	if (cmd == SNDRV_PCM_TRIGGER_STOP) {
		/* wait until DMA stopped */
		while (!(igetbyte(chip, port + ichdev->roff_sr) & ICH_DCH)) ;
		/* reset whole DMA things */
		iputbyte(chip, port + ICH_REG_OFF_CR, ICH_RESETREGS);
	}
	return 0;
}

static int snd_intel8x0_hw_params(snd_pcm_substream_t * substream,
				  snd_pcm_hw_params_t * hw_params)
{
	return snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
}

static int snd_intel8x0_hw_free(snd_pcm_substream_t * substream)
{
	return snd_pcm_lib_free_pages(substream);
}

static snd_pcm_uframes_t snd_intel8x0_pcm_pointer(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);
	ichdev_t *ichdev = get_ichdev(substream);
	size_t ptr1, ptr;

	ptr1 = igetword(chip, ichdev->reg_offset + ichdev->roff_picb) << chip->pcm_pos_shift;
	if (ptr1 != 0)
		ptr = ichdev->fragsize1 - ptr1;
	else
		ptr = 0;
	ptr += ichdev->position;
	if (ptr >= ichdev->size)
		return 0;
	return bytes_to_frames(substream->runtime, ptr);
}

static int snd_intel8x0m_pcm_trigger(snd_pcm_substream_t *substream, int cmd)
{
	ichdev_t *ichdev = get_ichdev(substream);
	/* hook off/on on start/stop */
	/* TODO: move it to ac97 controls */
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		snd_ac97_update_bits(ichdev->ac97, AC97_GPIO_STATUS,
				     AC97_GPIO_LINE1_OH, AC97_GPIO_LINE1_OH);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		snd_ac97_update_bits(ichdev->ac97, AC97_GPIO_STATUS,
				     AC97_GPIO_LINE1_OH, ~AC97_GPIO_LINE1_OH);
		break;
	default:
		return -EINVAL;
	}
	return snd_intel8x0_pcm_trigger(substream,cmd);
}

static int snd_intel8x0m_pcm_prepare(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	ichdev_t *ichdev = get_ichdev(substream);

	ichdev->physbuf = runtime->dma_addr;
	ichdev->size = snd_pcm_lib_buffer_bytes(substream);
	ichdev->fragsize = snd_pcm_lib_period_bytes(substream);
	snd_ac97_write(ichdev->ac97, AC97_LINE1_RATE, runtime->rate);
	snd_ac97_write(ichdev->ac97, AC97_LINE1_LEVEL, 0);
	snd_intel8x0_setup_periods(chip, ichdev);
	return 0;
}

static snd_pcm_hardware_t snd_intel8x0m_stream =
{
	.info =			(SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_BLOCK_TRANSFER |
				 SNDRV_PCM_INFO_MMAP_VALID |
				 SNDRV_PCM_INFO_PAUSE |
				 SNDRV_PCM_INFO_RESUME),
	.formats =		SNDRV_PCM_FMTBIT_S16_LE,
	.rates =		SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_KNOT,
	.rate_min =		8000,
	.rate_max =		16000,
	.channels_min =		1,
	.channels_max =		1,
	.buffer_bytes_max =	32 * 1024,
	.period_bytes_min =	32,
	.period_bytes_max =	32 * 1024,
	.periods_min =		1,
	.periods_max =		1024,
	.fifo_size =		0,
};


static int snd_intel8x0m_pcm_open(snd_pcm_substream_t * substream, ichdev_t *ichdev)
{
	static unsigned int rates[] = { 8000,  9600, 12000, 16000 };
	static snd_pcm_hw_constraint_list_t hw_constraints_rates = {
		.count = ARRAY_SIZE(rates),
		.list = rates,
		.mask = 0,
	};
	snd_pcm_runtime_t *runtime = substream->runtime;
	int err;

	ichdev->substream = substream;
	runtime->hw = snd_intel8x0m_stream;
	err = snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_RATE, &hw_constraints_rates);
	if ( err < 0 )
		return err;
	runtime->private_data = ichdev;
	return 0;
}

static int snd_intel8x0m_playback_open(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);

	return snd_intel8x0m_pcm_open(substream, &chip->ichd[ICHD_MDMOUT]);
}

static int snd_intel8x0m_playback_close(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);

	chip->ichd[ICHD_MDMOUT].substream = NULL;
	return 0;
}

static int snd_intel8x0m_capture_open(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);

	return snd_intel8x0m_pcm_open(substream, &chip->ichd[ICHD_MDMIN]);
}

static int snd_intel8x0m_capture_close(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);

	chip->ichd[ICHD_MDMIN].substream = NULL;
	return 0;
}


static snd_pcm_ops_t snd_intel8x0m_playback_ops = {
	.open =		snd_intel8x0m_playback_open,
	.close =	snd_intel8x0m_playback_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_intel8x0_hw_params,
	.hw_free =	snd_intel8x0_hw_free,
	.prepare =	snd_intel8x0m_pcm_prepare,
	.trigger =	snd_intel8x0m_pcm_trigger,
	.pointer =	snd_intel8x0_pcm_pointer,
};

static snd_pcm_ops_t snd_intel8x0m_capture_ops = {
	.open =		snd_intel8x0m_capture_open,
	.close =	snd_intel8x0m_capture_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_intel8x0_hw_params,
	.hw_free =	snd_intel8x0_hw_free,
	.prepare =	snd_intel8x0m_pcm_prepare,
	.trigger =	snd_intel8x0m_pcm_trigger,
	.pointer =	snd_intel8x0_pcm_pointer,
};


struct ich_pcm_table {
	char *suffix;
	snd_pcm_ops_t *playback_ops;
	snd_pcm_ops_t *capture_ops;
	size_t prealloc_size;
	size_t prealloc_max_size;
	int ac97_idx;
};

static int __devinit snd_intel8x0_pcm1(intel8x0_t *chip, int device, struct ich_pcm_table *rec)
{
	snd_pcm_t *pcm;
	int err;
	char name[32];

	if (rec->suffix)
		sprintf(name, "Intel ICH - %s", rec->suffix);
	else
		strcpy(name, "Intel ICH");
	err = snd_pcm_new(chip->card, name, device,
			  rec->playback_ops ? 1 : 0,
			  rec->capture_ops ? 1 : 0, &pcm);
	if (err < 0)
		return err;

	if (rec->playback_ops)
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, rec->playback_ops);
	if (rec->capture_ops)
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, rec->capture_ops);

	pcm->private_data = chip;
	pcm->info_flags = 0;
	if (rec->suffix)
		sprintf(pcm->name, "%s - %s", chip->card->shortname, rec->suffix);
	else
		strcpy(pcm->name, chip->card->shortname);
	chip->pcm[device] = pcm;

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV,
					      snd_dma_pci_data(chip->pci),
					      rec->prealloc_size,
					      rec->prealloc_max_size);

	return 0;
}

static struct ich_pcm_table intel_pcms[] __devinitdata = {
	{
		.suffix = "Modem",
		.playback_ops = &snd_intel8x0m_playback_ops,
		.capture_ops = &snd_intel8x0m_capture_ops,
		.prealloc_size = 4 * 1024,
		.prealloc_max_size = 16 * 1024,
	},
};

static int __devinit snd_intel8x0_pcm(intel8x0_t *chip)
{
	int i, tblsize, device, err;
	struct ich_pcm_table *tbl, *rec;

#if 1
	tbl = intel_pcms;
	tblsize = 1;
#else
	switch (chip->device_type) {
	case DEVICE_NFORCE:
		tbl = nforce_pcms;
		tblsize = ARRAY_SIZE(nforce_pcms);
		break;
	case DEVICE_ALI:
		tbl = ali_pcms;
		tblsize = ARRAY_SIZE(ali_pcms);
		break;
	default:
		tbl = intel_pcms;
		tblsize = 2;
		break;
	}
#endif
	device = 0;
	for (i = 0; i < tblsize; i++) {
		rec = tbl + i;
		if (i > 0 && rec->ac97_idx) {
			/* activate PCM only when associated AC'97 codec */
			if (! chip->ichd[rec->ac97_idx].ac97)
				continue;
		}
		err = snd_intel8x0_pcm1(chip, device, rec);
		if (err < 0)
			return err;
		device++;
	}

	chip->pcm_devs = device;
	return 0;
}
	

/*
 *  Mixer part
 */

static void snd_intel8x0_mixer_free_ac97_bus(ac97_bus_t *bus)
{
	intel8x0_t *chip = snd_magic_cast(intel8x0_t, bus->private_data, return);
	chip->ac97_bus = NULL;
}

static void snd_intel8x0_mixer_free_ac97(ac97_t *ac97)
{
	intel8x0_t *chip = snd_magic_cast(intel8x0_t, ac97->private_data, return);
	chip->ac97 = NULL;
}


static int __devinit snd_intel8x0_mixer(intel8x0_t *chip, int ac97_clock)
{
	ac97_bus_t bus, *pbus;
	ac97_t ac97, *x97;
	int err;
	unsigned int glob_sta = 0;

	chip->in_ac97_init = 1;
	memset(&bus, 0, sizeof(bus));
	bus.private_data = chip;
	bus.private_free = snd_intel8x0_mixer_free_ac97_bus;
	if (ac97_clock >= 8000 && ac97_clock <= 48000)
		bus.clock = ac97_clock;
	else
		bus.clock = 48000;
	
	memset(&ac97, 0, sizeof(ac97));
	ac97.private_data = chip;
	ac97.private_free = snd_intel8x0_mixer_free_ac97;
	ac97.scaps = AC97_SCAP_SKIP_AUDIO;

	glob_sta = igetdword(chip, ICHREG(GLOB_STA));
	bus.write = snd_intel8x0_codec_write;
	bus.read = snd_intel8x0_codec_read;
	bus.vra = 1;

	if ((err = snd_ac97_bus(chip->card, &bus, &pbus)) < 0)
		goto __err;
	chip->ac97_bus = pbus;
	ac97.pci = chip->pci;
	ac97.num = glob_sta & ICH_SCR ? 1 : 0;
	if ((err = snd_ac97_mixer(pbus, &ac97, &x97)) < 0) {
		snd_printk(KERN_ERR "Unable to initialize codec #%d\n", ac97.num);
		if (ac97.num == 0)
			goto __err;
		return err;
	}
	chip->ac97 = x97;
	if(ac97_is_modem(x97) && !chip->ichd[ICHD_MDMIN].ac97 ) {
		chip->ichd[ICHD_MDMIN].ac97 = x97;
		chip->ichd[ICHD_MDMOUT].ac97 = x97;
	}

	chip->in_ac97_init = 0;
	return 0;

 __err:
	/* clear the cold-reset bit for the next chance */
	if (chip->device_type != DEVICE_ALI)
		iputdword(chip, ICHREG(GLOB_CNT), igetdword(chip, ICHREG(GLOB_CNT)) & ~ICH_AC97COLD);
	return err;
}


/*
 *
 */

#define do_delay(chip) do {\
	set_current_state(TASK_UNINTERRUPTIBLE);\
	schedule_timeout(1);\
} while (0)

static int snd_intel8x0m_ich_chip_init(intel8x0_t *chip, int probing)
{
	unsigned long end_time;
	unsigned int cnt, status, nstatus;
	
	/* put logic to right state */
	/* first clear status bits */
	status = ICH_RCS | ICH_MIINT | ICH_MOINT;
	cnt = igetdword(chip, ICHREG(GLOB_STA));
	iputdword(chip, ICHREG(GLOB_STA), cnt & status);

	/* ACLink on, 2 channels */
	cnt = igetdword(chip, ICHREG(GLOB_CNT));
	cnt &= ~(ICH_ACLINK);
	/* finish cold or do warm reset */
	cnt |= (cnt & ICH_AC97COLD) == 0 ? ICH_AC97COLD : ICH_AC97WARM;
	iputdword(chip, ICHREG(GLOB_CNT), cnt);
	end_time = (jiffies + (HZ / 4)) + 1;
	do {
		if ((igetdword(chip, ICHREG(GLOB_CNT)) & ICH_AC97WARM) == 0)
			goto __ok;
		do_delay(chip);
	} while (time_after_eq(end_time, jiffies));
	snd_printk("AC'97 warm reset still in progress? [0x%x]\n", igetdword(chip, ICHREG(GLOB_CNT)));
	return -EIO;

      __ok:
	if (probing) {
		/* wait for any codec ready status.
		 * Once it becomes ready it should remain ready
		 * as long as we do not disable the ac97 link.
		 */
		end_time = jiffies + HZ;
		do {
			status = igetdword(chip, ICHREG(GLOB_STA)) & (ICH_PCR | ICH_SCR | ICH_TCR);
			if (status)
				break;
			do_delay(chip);
		} while (time_after_eq(end_time, jiffies));
		if (! status) {
			/* no codec is found */
			snd_printk(KERN_ERR "codec_ready: codec is not ready [0x%x]\n", igetdword(chip, ICHREG(GLOB_STA)));
			return -EIO;
		}

		/* up to two codecs (modem cannot be tertiary with ICH4) */
		nstatus = ICH_PCR | ICH_SCR;

		/* wait for other codecs ready status. */
		end_time = jiffies + HZ / 4;
		while (status != nstatus && time_after_eq(end_time, jiffies)) {
			do_delay(chip);
			status |= igetdword(chip, ICHREG(GLOB_STA)) & nstatus;
		}

	} else {
		/* resume phase */
		status = 0;
		if (chip->ac97)
			status |= get_ich_codec_bit(chip, chip->ac97->num);
		/* wait until all the probed codecs are ready */
		end_time = jiffies + HZ;
		do {
			nstatus = igetdword(chip, ICHREG(GLOB_STA)) & (ICH_PCR | ICH_SCR | ICH_TCR);
			if (status == nstatus)
				break;
			do_delay(chip);
		} while (time_after_eq(end_time, jiffies));
	}

      	return 0;
}

static int snd_intel8x0_chip_init(intel8x0_t *chip, int probing)
{
	unsigned int i;
	int err;
	
	if ((err = snd_intel8x0m_ich_chip_init(chip, probing)) < 0)
		return err;
	iagetword(chip, 0);	/* clear semaphore flag */

	/* disable interrupts */
	for (i = 0; i < chip->bdbars_count; i++)
		iputbyte(chip, ICH_REG_OFF_CR + chip->ichd[i].reg_offset, 0x00);
	/* reset channels */
	for (i = 0; i < chip->bdbars_count; i++)
		iputbyte(chip, ICH_REG_OFF_CR + chip->ichd[i].reg_offset, ICH_RESETREGS);
	/* initialize Buffer Descriptor Lists */
	for (i = 0; i < chip->bdbars_count; i++)
		iputdword(chip, ICH_REG_OFF_BDBAR + chip->ichd[i].reg_offset, chip->ichd[i].bdbar_addr);
	return 0;
}

static int snd_intel8x0_free(intel8x0_t *chip)
{
	unsigned int i;

	if (chip->irq < 0)
		goto __hw_end;
	/* disable interrupts */
	for (i = 0; i < chip->bdbars_count; i++)
		iputbyte(chip, ICH_REG_OFF_CR + chip->ichd[i].reg_offset, 0x00);
	/* reset channels */
	for (i = 0; i < chip->bdbars_count; i++)
		iputbyte(chip, ICH_REG_OFF_CR + chip->ichd[i].reg_offset, ICH_RESETREGS);
	/* --- */
	synchronize_irq(chip->irq);
      __hw_end:
	if (chip->bdbars.area)
		snd_dma_free_pages(&chip->dma_dev, &chip->bdbars);
	if (chip->remap_addr)
		iounmap((void *) chip->remap_addr);
	if (chip->remap_bmaddr)
		iounmap((void *) chip->remap_bmaddr);
	if (chip->res) {
		release_resource(chip->res);
		kfree_nocheck(chip->res);
	}
	if (chip->res_bm) {
		release_resource(chip->res_bm);
		kfree_nocheck(chip->res_bm);
	}
	if (chip->irq >= 0)
		free_irq(chip->irq, (void *)chip);
	snd_magic_kfree(chip);
	return 0;
}

#ifdef CONFIG_PM
/*
 * power management
 */
static int intel8x0m_suspend(snd_card_t *card, unsigned int state)
{
	intel8x0_t *chip = snd_magic_cast(intel8x0_t, card->pm_private_data, return -EINVAL);
	int i;

	for (i = 0; i < chip->pcm_devs; i++)
		snd_pcm_suspend_all(chip->pcm[i]);
	if (chip->ac97)
		snd_ac97_suspend(chip->ac97);
	snd_power_change_state(card, SNDRV_CTL_POWER_D3hot);
	return 0;
}

static int intel8x0m_resume(snd_card_t *card, unsigned int state)
{
	intel8x0_t *chip = snd_magic_cast(intel8x0_t, card->pm_private_data, return -EINVAL);
	pci_enable_device(chip->pci);
	pci_set_master(chip->pci);
	snd_intel8x0_chip_init(chip, 0);
	if (chip->ac97)
		snd_ac97_resume(chip->ac97);

	snd_power_change_state(card, SNDRV_CTL_POWER_D0);
	return 0;
}
#endif /* CONFIG_PM */

static void snd_intel8x0m_proc_read(snd_info_entry_t * entry,
				   snd_info_buffer_t * buffer)
{
	intel8x0_t *chip = snd_magic_cast(intel8x0_t, entry->private_data, return);
	unsigned int tmp;

	snd_iprintf(buffer, "Intel8x0m\n\n");
	if (chip->device_type == DEVICE_ALI)
		return;
	tmp = igetdword(chip, ICHREG(GLOB_STA));
	snd_iprintf(buffer, "Global control        : 0x%08x\n", igetdword(chip, ICHREG(GLOB_CNT)));
	snd_iprintf(buffer, "Global status         : 0x%08x\n", tmp);
	snd_iprintf(buffer, "AC'97 codecs ready    :%s%s%s%s\n",
			tmp & ICH_PCR ? " primary" : "",
			tmp & ICH_SCR ? " secondary" : "",
			tmp & ICH_TCR ? " tertiary" : "",
			(tmp & (ICH_PCR | ICH_SCR | ICH_TCR)) == 0 ? " none" : "");
}

static void __devinit snd_intel8x0m_proc_init(intel8x0_t * chip)
{
	snd_info_entry_t *entry;

	if (! snd_card_proc_new(chip->card, "intel8x0m", &entry))
		snd_info_set_text_ops(entry, chip, 1024, snd_intel8x0m_proc_read);
}

static int snd_intel8x0_dev_free(snd_device_t *device)
{
	intel8x0_t *chip = snd_magic_cast(intel8x0_t, device->device_data, return -ENXIO);
	return snd_intel8x0_free(chip);
}

struct ich_reg_info {
	unsigned int int_sta_mask;
	unsigned int offset;
};

static int __devinit snd_intel8x0m_create(snd_card_t * card,
					 struct pci_dev *pci,
					 unsigned long device_type,
					 intel8x0_t ** r_intel8x0)
{
	intel8x0_t *chip;
	int err;
	unsigned int i;
	unsigned int int_sta_masks;
	ichdev_t *ichdev;
	static snd_device_ops_t ops = {
		.dev_free =	snd_intel8x0_dev_free,
	};
	static struct ich_reg_info intel_regs[2] = {
		{ ICH_MIINT, 0 },
		{ ICH_MOINT, 0x10 },
	};
	struct ich_reg_info *tbl;

	*r_intel8x0 = NULL;

	if ((err = pci_enable_device(pci)) < 0)
		return err;

	chip = snd_magic_kcalloc(intel8x0_t, 0, GFP_KERNEL);
	if (chip == NULL)
		return -ENOMEM;
	spin_lock_init(&chip->reg_lock);
	spin_lock_init(&chip->ac97_lock);
	chip->device_type = device_type;
	chip->card = card;
	chip->pci = pci;
	chip->irq = -1;
	snd_intel8x0m_proc_init(chip);
	sprintf(chip->ac97_name, "%s - AC'97", card->shortname);
	sprintf(chip->ctrl_name, "%s - Controller", card->shortname);
	if (device_type == DEVICE_ALI) {
		/* ALI5455 has no ac97 region */
		chip->bmaddr = pci_resource_start(pci, 0);
		if ((chip->res_bm = request_region(chip->bmaddr, 256, chip->ctrl_name)) == NULL) {
			snd_printk("unable to grab ports 0x%lx-0x%lx\n", chip->bmaddr, chip->bmaddr + 256 - 1);
			snd_intel8x0_free(chip);
			return -EBUSY;
		}
		goto port_inited;
	}

	if (pci_resource_flags(pci, 2) & IORESOURCE_MEM) {	/* ICH4 and Nforce */
		chip->mmio = 1;
		chip->addr = pci_resource_start(pci, 2);
		if ((chip->res = request_mem_region(chip->addr, 512, chip->ac97_name)) == NULL) {
			snd_printk("unable to grab I/O memory 0x%lx-0x%lx\n", chip->addr, chip->addr + 512 - 1);
			snd_intel8x0_free(chip);
			return -EBUSY;
		}
		chip->remap_addr = (unsigned long) ioremap_nocache(chip->addr, 512);
		if (chip->remap_addr == 0) {
			snd_printk("AC'97 space ioremap problem\n");
			snd_intel8x0_free(chip);
			return -EIO;
		}
	} else {
		chip->addr = pci_resource_start(pci, 0);
		if ((chip->res = request_region(chip->addr, 256, chip->ac97_name)) == NULL) {
			snd_printk("unable to grab ports 0x%lx-0x%lx\n", chip->addr, chip->addr + 256 - 1);
			snd_intel8x0_free(chip);
			return -EBUSY;
		}
	}
	if (pci_resource_flags(pci, 3) & IORESOURCE_MEM) {	/* ICH4 */
		chip->bm_mmio = 1;
		chip->bmaddr = pci_resource_start(pci, 3);
		if ((chip->res_bm = request_mem_region(chip->bmaddr, 256, chip->ctrl_name)) == NULL) {
			snd_printk("unable to grab I/O memory 0x%lx-0x%lx\n", chip->bmaddr, chip->bmaddr + 512 - 1);
			snd_intel8x0_free(chip);
			return -EBUSY;
		}
		chip->remap_bmaddr = (unsigned long) ioremap_nocache(chip->bmaddr, 256);
		if (chip->remap_bmaddr == 0) {
			snd_printk("Controller space ioremap problem\n");
			snd_intel8x0_free(chip);
			return -EIO;
		}
	} else {
		chip->bmaddr = pci_resource_start(pci, 1);
		if ((chip->res_bm = request_region(chip->bmaddr, 128, chip->ctrl_name)) == NULL) {
			snd_printk("unable to grab ports 0x%lx-0x%lx\n", chip->bmaddr, chip->bmaddr + 128 - 1);
			snd_intel8x0_free(chip);
			return -EBUSY;
		}
	}

 port_inited:
	if (request_irq(pci->irq, snd_intel8x0_interrupt, SA_INTERRUPT|SA_SHIRQ, card->shortname, (void *)chip)) {
		snd_printk("unable to grab IRQ %d\n", pci->irq);
		snd_intel8x0_free(chip);
		return -EBUSY;
	}
	chip->irq = pci->irq;
	pci_set_master(pci);
	synchronize_irq(chip->irq);

	/* initialize offsets */
	chip->bdbars_count = 2;
	tbl = intel_regs;

	for (i = 0; i < chip->bdbars_count; i++) {
		ichdev = &chip->ichd[i];
		ichdev->ichd = i;
		ichdev->reg_offset = tbl[i].offset;
		ichdev->int_sta_mask = tbl[i].int_sta_mask;
		if (device_type == DEVICE_SIS) {
			/* SiS 7013 swaps the registers */
			ichdev->roff_sr = ICH_REG_OFF_PICB;
			ichdev->roff_picb = ICH_REG_OFF_SR;
		} else {
			ichdev->roff_sr = ICH_REG_OFF_SR;
			ichdev->roff_picb = ICH_REG_OFF_PICB;
		}
		if (device_type == DEVICE_ALI)
			ichdev->ali_slot = (ichdev->reg_offset - 0x40) / 0x10;
	}
	/* SIS7013 handles the pcm data in bytes, others are in words */
	chip->pcm_pos_shift = (device_type == DEVICE_SIS) ? 0 : 1;

	/* allocate buffer descriptor lists */
	/* the start of each lists must be aligned to 8 bytes */
	memset(&chip->dma_dev, 0, sizeof(chip->dma_dev));
	chip->dma_dev.type = SNDRV_DMA_TYPE_DEV;
	chip->dma_dev.dev = snd_dma_pci_data(pci);
	if (snd_dma_alloc_pages(&chip->dma_dev, chip->bdbars_count * sizeof(u32) * ICH_MAX_FRAGS * 2, &chip->bdbars) < 0) {
		snd_intel8x0_free(chip);
		return -ENOMEM;
	}
	/* tables must be aligned to 8 bytes here, but the kernel pages
	   are much bigger, so we don't care (on i386) */
	int_sta_masks = 0;
	for (i = 0; i < chip->bdbars_count; i++) {
		ichdev = &chip->ichd[i];
		ichdev->bdbar = ((u32 *)chip->bdbars.area) + (i * ICH_MAX_FRAGS * 2);
		ichdev->bdbar_addr = chip->bdbars.addr + (i * sizeof(u32) * ICH_MAX_FRAGS * 2);
		int_sta_masks |= ichdev->int_sta_mask;
	}
	chip->int_sta_reg = ICH_REG_GLOB_STA;
	chip->int_sta_mask = int_sta_masks;

	if ((err = snd_intel8x0_chip_init(chip, 1)) < 0) {
		snd_intel8x0_free(chip);
		return err;
	}

	snd_card_set_pm_callback(card, intel8x0m_suspend, intel8x0m_resume, chip);

	if ((err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, chip, &ops)) < 0) {
		snd_intel8x0_free(chip);
		return err;
	}

	snd_card_set_dev(card, &pci->dev);

	*r_intel8x0 = chip;
	return 0;
}

static struct shortname_table {
	unsigned int id;
	const char *s;
} shortnames[] __devinitdata = {
	{ PCI_DEVICE_ID_INTEL_82801_6, "Intel 82801AA-ICH" },
	{ PCI_DEVICE_ID_INTEL_82901_6, "Intel 82901AB-ICH0" },
	{ PCI_DEVICE_ID_INTEL_82801BA_6, "Intel 82801BA-ICH2" },
	{ PCI_DEVICE_ID_INTEL_440MX_6, "Intel 440MX" },
	{ PCI_DEVICE_ID_INTEL_ICH3_6, "Intel 82801CA-ICH3" },
	{ PCI_DEVICE_ID_INTEL_ICH4_6, "Intel 82801DB-ICH4" },
	{ PCI_DEVICE_ID_INTEL_ICH5_6, "Intel ICH5" },
	{ 0x7446, "AMD AMD768" },
#if 0
	{ PCI_DEVICE_ID_SI_7013, "SiS SI7013" },
	{ PCI_DEVICE_ID_NVIDIA_MCP_AUDIO, "NVidia nForce" },
	{ PCI_DEVICE_ID_NVIDIA_MCP2_AUDIO, "NVidia nForce2" },
	{ PCI_DEVICE_ID_NVIDIA_MCP3_AUDIO, "NVidia nForce3" },
	{ 0x5455, "ALi M5455" },
	{ 0x746d, "AMD AMD8111" },
#endif
	{ 0 },
};

static int __devinit snd_intel8x0m_probe(struct pci_dev *pci,
					const struct pci_device_id *pci_id)
{
	static int dev;
	snd_card_t *card;
	intel8x0_t *chip;
	int err;
	struct shortname_table *name;

	if (dev >= SNDRV_CARDS)
		return -ENODEV;
	if (!enable[dev]) {
		dev++;
		return -ENOENT;
	}

	card = snd_card_new(index[dev], id[dev], THIS_MODULE, 0);
	if (card == NULL)
		return -ENOMEM;

	switch (pci_id->driver_data) {
	case DEVICE_NFORCE:
		strcpy(card->driver, "NFORCE");
		break;
	default:
		strcpy(card->driver, "ICH");
		break;
	}

	strcpy(card->shortname, "Intel ICH");
	for (name = shortnames; name->id; name++) {
		if (pci->device == name->id) {
			strcpy(card->shortname, name->s);
			break;
		}
	}
	strcat(card->shortname," Modem");

	if ((err = snd_intel8x0m_create(card, pci, pci_id->driver_data, &chip)) < 0) {
		snd_card_free(card);
		return err;
	}

	if ((err = snd_intel8x0_mixer(chip, ac97_clock[dev])) < 0) {
		snd_card_free(card);
		return err;
	}
	if ((err = snd_intel8x0_pcm(chip)) < 0) {
		snd_card_free(card);
		return err;
	}
	
	sprintf(card->longname, "%s at 0x%lx, irq %i",
		card->shortname, chip->addr, chip->irq);

	if ((err = snd_card_register(card)) < 0) {
		snd_card_free(card);
		return err;
	}
	pci_set_drvdata(pci, card);
	dev++;
	return 0;
}

static void __devexit snd_intel8x0m_remove(struct pci_dev *pci)
{
	snd_card_free(pci_get_drvdata(pci));
	pci_set_drvdata(pci, NULL);
}

static struct pci_driver driver = {
	.name = "Intel ICH Modem",
	.id_table = snd_intel8x0m_ids,
	.probe = snd_intel8x0m_probe,
	.remove = __devexit_p(snd_intel8x0m_remove),
	SND_PCI_PM_CALLBACKS
};


static int __init alsa_card_intel8x0m_init(void)
{
	return pci_module_init(&driver);
}

static void __exit alsa_card_intel8x0m_exit(void)
{
	pci_unregister_driver(&driver);
}

module_init(alsa_card_intel8x0m_init)
module_exit(alsa_card_intel8x0m_exit)
