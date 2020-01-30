/*
 *  Matt Wu <Matt_Wu@acersoftech.com.cn>
 *  Apr 26, 2001
 *  Routines for control of ALi pci audio M5451
 *
 *  BUGS:
 *    --
 *
 *  TODO:
 *    --
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public Lcodecnse as published by
 *   the Free Software Foundation; either version 2 of the Lcodecnse, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public Lcodecnse for more details.
 *
 *   You should have received a copy of the GNU General Public Lcodecnse
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
#include <linux/moduleparam.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/info.h>
#include <sound/ac97_codec.h>
#include <sound/mpu401.h>
#include <sound/initval.h>

MODULE_AUTHOR("Matt Wu <Matt_Wu@acersoftech.com.cn>");
MODULE_DESCRIPTION("ALI M5451");
MODULE_LICENSE("GPL");
MODULE_CLASSES("{sound}");
MODULE_DEVICES("{{ALI,M5451,pci},{ALI,M5451}}");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;	/* Index 0-MAX */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;	/* ID for this card */
static int enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;
static int pcm_channels[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = 32};
static int spdif[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = 0};
static int boot_devs;

module_param_array(index, int, boot_devs, 0444);
MODULE_PARM_DESC(index, "Index value for ALI M5451 PCI Audio.");
MODULE_PARM_SYNTAX(index, SNDRV_INDEX_DESC);
module_param_array(id, charp, boot_devs, 0444);
MODULE_PARM_DESC(id, "ID string for ALI M5451 PCI Audio.");
MODULE_PARM_SYNTAX(id, SNDRV_ID_DESC);
module_param_array(enable, bool, boot_devs, 0444);
MODULE_PARM_DESC(enable, "Enable ALI 5451 PCI Audio.");
MODULE_PARM_SYNTAX(enable, SNDRV_ENABLE_DESC);
module_param_array(pcm_channels, int, boot_devs, 0444);
MODULE_PARM_DESC(pcm_channels, "PCM Channels");
MODULE_PARM_SYNTAX(pcm_channels, SNDRV_ENABLED ",default:32,allows:{{1,32}}");
module_param_array(spdif, bool, boot_devs, 0444);
MODULE_PARM_DESC(spdif, "Support SPDIF I/O");
MODULE_PARM_SYNTAX(spdif, SNDRV_ENABLED "," SNDRV_BOOLEAN_FALSE_DESC);

/*
 *  Debug part definitions
 */

//#define ALI_DEBUG

#ifdef ALI_DEBUG
#define snd_ali_printk(format, args...) printk(format, ##args);
#else
#define snd_ali_printk(format, args...)
#endif

/*
 *  Constants definition
 */

#ifndef PCI_VENDOR_ID_ALI
#define PCI_VENDOR_ID_ALI	0x10b9
#endif

#ifndef PCI_DEVICE_ID_ALI_5451
#define PCI_DEVICE_ID_ALI_5451	0x5451
#endif

#define DEVICE_ID_ALI5451	((PCI_VENDOR_ID_ALI<<16)|PCI_DEVICE_ID_ALI_5451)


#define ALI_CHANNELS		32

#define ALI_PCM_IN_CHANNEL	31
#define ALI_SPDIF_IN_CHANNEL	19
#define ALI_SPDIF_OUT_CHANNEL	15
#define ALI_CENTER_CHANNEL	24
#define ALI_LEF_CHANNEL		23
#define ALI_SURR_LEFT_CHANNEL	26
#define ALI_SURR_RIGHT_CHANNEL	25

#define	SNDRV_ALI_VOICE_TYPE_PCM	01
#define SNDRV_ALI_VOICE_TYPE_OTH	02

#define	ALI_5451_V02		0x02

/*
 *  Direct Registers
 */

#define ALI_LEGACY_DMAR0        0x00  // ADR0
#define ALI_LEGACY_DMAR4        0x04  // CNT0
#define ALI_LEGACY_DMAR11       0x0b  // MOD 
#define ALI_LEGACY_DMAR15       0x0f  // MMR 
#define ALI_MPUR0		0x20
#define ALI_MPUR1		0x21
#define ALI_MPUR2		0x22
#define ALI_MPUR3		0x23

#define	ALI_AC97_WRITE		0x40
#define ALI_AC97_READ		0x44

#define ALI_SCTRL		0x48
#define   ALI_SPDIF_OUT_ENABLE		0x20
#define ALI_AC97_GPIO		0x4c
#define ALI_SPDIF_CS		0x70
#define ALI_SPDIF_CTRL		0x74
#define   ALI_SPDIF_IN_FUNC_ENABLE	0x02
#define   ALI_SPDIF_IN_CH_STATUS	0x40
#define   ALI_SPDIF_OUT_CH_STATUS	0xbf
#define ALI_START		0x80
#define ALI_STOP		0x84
#define ALI_CSPF		0x90
#define ALI_AINT		0x98
#define ALI_GC_CIR		0xa0
	#define ENDLP_IE		0x00001000
	#define MIDLP_IE		0x00002000
#define ALI_AINTEN		0xa4
#define ALI_VOLUME		0xa8
#define ALI_SBDELTA_DELTA_R     0xac
#define ALI_MISCINT		0xb0
	#define ADDRESS_IRQ		0x00000020
	#define TARGET_REACHED		0x00008000
	#define MIXER_OVERFLOW		0x00000800
	#define MIXER_UNDERFLOW		0x00000400
#define ALI_SBBL_SBCL           0xc0
#define ALI_SBCTRL_SBE2R_SBDD   0xc4
#define ALI_STIMER		0xc8
#define ALI_GLOBAL_CONTROL	0xd4
#define   ALI_SPDIF_OUT_SEL_PCM		0x00000400 /* bit 10 */
#define   ALI_SPDIF_IN_SUPPORT		0x00000800 /* bit 11 */
#define   ALI_SPDIF_OUT_CH_ENABLE	0x00008000 /* bit 15 */
#define   ALI_SPDIF_IN_CH_ENABLE	0x00080000 /* bit 19 */
#define   ALI_PCM_IN_ENABLE		0x80000000 /* bit 31 */

#define ALI_CSO_ALPHA_FMS	0xe0
#define ALI_LBA			0xe4
#define ALI_ESO_DELTA		0xe8
#define ALI_GVSEL_PAN_VOC_CTRL_EC	0xf0
#define ALI_EBUF1		0xf4
#define ALI_EBUF2		0xf8

#define ALI_REG(codec, x) ((codec)->port + x)

typedef struct snd_stru_ali ali_t;
typedef struct snd_ali_stru_voice snd_ali_voice_t;
#define chip_t ali_t

typedef struct snd_ali_channel_control {
	// register data
	struct REGDATA {
		unsigned int start;
		unsigned int stop;
		unsigned int aint;
		unsigned int ainten;
	} data;
		
	// register addresses
	struct REGS {
		unsigned int start;
		unsigned int stop;
		unsigned int aint;
		unsigned int ainten;
		unsigned int ac97read;
		unsigned int ac97write;
	} regs;

} snd_ali_channel_control_t;

struct snd_ali_stru_voice {
	unsigned int number;
	int use: 1,
	    pcm: 1,
	    midi: 1,
	    mode: 1,
	    synth: 1;

	/* PCM data */
	ali_t *codec;
	snd_pcm_substream_t *substream;
	snd_ali_voice_t *extra;
	
	int running: 1;

	int eso;                /* final ESO value for channel */
	int count;              /* runtime->period_size */

	/* --- */

	void *private_data;
	void (*private_free)(void *private_data);
};


typedef struct snd_stru_alidev {

	snd_ali_voice_t voices[ALI_CHANNELS];	

	unsigned int	chcnt;			/* num of opened channels */
	unsigned int	chmap;			/* bitmap for opened channels */
	unsigned int synthcount;

} alidev_t;


#ifdef CONFIG_PM
#define ALI_GLOBAL_REGS		56
#define ALI_CHANNEL_REGS	8
typedef struct snd_ali_image {
	unsigned long regs[ALI_GLOBAL_REGS];
	unsigned long channel_regs[ALI_CHANNELS][ALI_CHANNEL_REGS];
} ali_image_t;
#endif


struct snd_stru_ali {
	unsigned long	irq;
	unsigned long	port;
	unsigned char	revision;

	unsigned int hw_initialized: 1;
	unsigned int spdif_support: 1;
	struct resource *res_port;

	struct pci_dev	*pci;
	struct pci_dev	*pci_m1533;
	struct pci_dev	*pci_m7101;

	snd_card_t	*card;
	snd_pcm_t	*pcm;
	alidev_t	synth;
	snd_ali_channel_control_t chregs;

	/* S/PDIF Mask */
	unsigned int	spdif_mask;

	unsigned int spurious_irq_count;
	unsigned int spurious_irq_max_delta;

	ac97_bus_t *ac97_bus;
	ac97_t *ac97;
	unsigned short	ac97_ext_id;
	unsigned short	ac97_ext_status;

	spinlock_t	reg_lock;
	spinlock_t	voice_alloc;

#ifdef CONFIG_PM
	ali_image_t *image;
#endif
};

static struct pci_device_id snd_ali_ids[] = {
	{0x10b9, 0x5451, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0, },
	{0, }
};
MODULE_DEVICE_TABLE(pci, snd_ali_ids);

static void snd_ali_clear_voices(ali_t *, unsigned int, unsigned int);
static unsigned short snd_ali_codec_peek(ali_t *, int, unsigned short);
static void snd_ali_codec_poke(ali_t *, int, unsigned short, unsigned short);

/*
 *  Debug Part
 */

#ifdef ALI_DEBUG

static void ali_read_regs(ali_t *codec, int channel)
{
	int i,j;
	unsigned int dwVal;

	printk("channel %d registers map:\n", channel);
	outb((unsigned char)(channel & 0x001f), ALI_REG(codec,ALI_GC_CIR));

	printk("    ");
	for(j=0;j<8;j++)
		printk("%2.2x       ", j*4);
	printk("\n");

	for (i=0; i<=0xf8/4;i++) {
		if(i%8 == 0)
			printk("%2.2x  ", (i*4/0x10)*0x10);
		dwVal = inl(ALI_REG(codec,i*4));
		printk("%8.8x ", dwVal);
		if ((i+1)%8 == 0)
			printk("\n");
	}
	printk("\n");
}
static void ali_read_cfg(unsigned int vendor, unsigned deviceid)
{
	unsigned int dwVal;
	struct pci_dev *pci_dev = NULL;
	int i,j;


        pci_dev = pci_find_device(vendor, deviceid, pci_dev);
        if (pci_dev == NULL)
                return ;

	printk("\nM%x PCI CFG\n", deviceid);
	printk("    ");
	for(j=0;j<8;j++)
		printk("%d        ",j);
	printk("\n");

	for(i=0;i<8;i++) {
		printk("%d   ",i);
		for(j=0;j<8;j++)
		{
			pci_read_config_dword(pci_dev, i*0x20+j*4, &dwVal);
			printk("%8.8x ", dwVal);
		}
		printk("\n");
	}
 }
static void ali_read_ac97regs(ali_t *codec, int secondary)
{
	unsigned short i,j;
	unsigned short wVal;

	printk("\ncodec %d registers map:\n", secondary);

	printk("    ");
	for(j=0;j<8;j++)
		printk("%2.2x   ",j*2);
	printk("\n");

	for (i=0; i<64;i++) {
		if(i%8 == 0)
			printk("%2.2x  ", (i/8)*0x10);
		wVal = snd_ali_codec_peek(codec, secondary, i*2);
		printk("%4.4x ", wVal);
		if ((i+1)%8 == 0)
			printk("\n");
	}
	printk("\n");
}

#endif

/*
 *  AC97 ACCESS
 */

static inline unsigned int snd_ali_5451_peek(ali_t *codec,
						unsigned int port )
{
	return (unsigned int)inl(ALI_REG(codec, port)); 
}

static inline void snd_ali_5451_poke(	ali_t *codec,
					unsigned int port,
					unsigned int val )
{
	outl((unsigned int)val, ALI_REG(codec, port));
}

static int snd_ali_codec_ready(	ali_t *codec,
				unsigned int port,
				int sched )
{
	unsigned long end_time;
	unsigned int res;
	
	end_time = jiffies + 10 * (HZ >> 2);
	do {
		res = snd_ali_5451_peek(codec,port);
		if (! (res & 0x8000))
			return 0;
		if (sched) {
			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_timeout(1);
		}
	} while (time_after_eq(end_time, jiffies));
	snd_ali_5451_poke(codec, port, res & ~0x8000);
	snd_printdd("ali_codec_ready: codec is not ready.\n ");
	return -EIO;
}

static int snd_ali_stimer_ready(ali_t *codec, int sched)
{
	unsigned long end_time;
	unsigned long dwChk1,dwChk2;
	
	dwChk1 = snd_ali_5451_peek(codec, ALI_STIMER);
	dwChk2 = snd_ali_5451_peek(codec, ALI_STIMER);

	end_time = jiffies + 10 * (HZ >> 2);
	do {
		dwChk2 = snd_ali_5451_peek(codec, ALI_STIMER);
		if (dwChk2 != dwChk1)
			return 0;
		if (sched) {
			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_timeout(1);
		}
	} while (time_after_eq(end_time, jiffies));
	snd_printk("ali_stimer_read: stimer is not ready.\n");
	return -EIO;
}

static void snd_ali_codec_poke(ali_t *codec,int secondary,
				     unsigned short reg,
				     unsigned short val)
{
	unsigned int dwVal = 0;
	unsigned int port = 0;

	if (reg >= 0x80) {
		snd_printk("ali_codec_poke: reg(%xh) invalid.\n", reg);
		return;
	}

	port = codec->chregs.regs.ac97write;

	if (snd_ali_codec_ready(codec, port, 0) < 0)
		return;
	if (snd_ali_stimer_ready(codec, 0) < 0)
		return;

	dwVal  = (unsigned int) (reg & 0xff);
	dwVal |= 0x8000 | (val << 16);
	if (secondary) dwVal |= 0x0080;
	if (codec->revision == ALI_5451_V02) dwVal |= 0x0100;

	snd_ali_5451_poke(codec,port,dwVal);

	return ;
}

static unsigned short snd_ali_codec_peek( ali_t *codec,
					  int secondary,
					  unsigned short reg)
{
	unsigned int dwVal = 0;
	unsigned int port = 0;

	if (reg >= 0x80) {
		snd_printk("ali_codec_peek: reg(%xh) invalid.\n", reg);
		return ~0;
	}

	port = codec->chregs.regs.ac97read;

	if (snd_ali_codec_ready(codec, port, 0) < 0)
		return ~0;
	if (snd_ali_stimer_ready(codec, 0) < 0)
		return ~0;

	dwVal  = (unsigned int) (reg & 0xff);
	dwVal |= 0x8000;				/* bit 15*/
	if (secondary) dwVal |= 0x0080;

	snd_ali_5451_poke(codec, port, dwVal);

	if (snd_ali_stimer_ready(codec, 0) < 0)
		return ~0;
	if (snd_ali_codec_ready(codec, port, 0) < 0)
		return ~0;
	
	return (snd_ali_5451_peek(codec, port) & 0xffff0000)>>16;
}

static void snd_ali_codec_write(ac97_t *ac97,
				unsigned short reg,
				unsigned short val )
{
	ali_t *codec = snd_magic_cast(ali_t, ac97->private_data, return);

	snd_ali_printk("codec_write: reg=%xh data=%xh.\n", reg, val);
	snd_ali_codec_poke(codec, 0, reg, val);
	return ;
}


static unsigned short snd_ali_codec_read(ac97_t *ac97, unsigned short reg)
{
	ali_t *codec = snd_magic_cast(ali_t, ac97->private_data, return -ENXIO);

	snd_ali_printk("codec_read reg=%xh.\n", reg);
	return (snd_ali_codec_peek(codec, 0, reg));
}

/*
 *	AC97 Reset
 */

static int snd_ali_reset_5451(ali_t *codec)
{
	struct pci_dev *pci_dev = NULL;
	unsigned short wCount, wReg;
	unsigned int   dwVal;
	
	if ((pci_dev = codec->pci_m1533) != NULL) {
		pci_read_config_dword(pci_dev, 0x7c, &dwVal);
		pci_write_config_dword(pci_dev, 0x7c, dwVal | 0x08000000);
		udelay(5000);
		pci_read_config_dword(pci_dev, 0x7c, &dwVal);
		pci_write_config_dword(pci_dev, 0x7c, dwVal & 0xf7ffffff);
		udelay(5000);
	}
	
	pci_dev = codec->pci;
	pci_read_config_dword(pci_dev, 0x44, &dwVal);
	pci_write_config_dword(pci_dev, 0x44, dwVal | 0x000c0000);
	udelay(500);
	pci_read_config_dword(pci_dev, 0x44, &dwVal);
	pci_write_config_dword(pci_dev, 0x44, dwVal & 0xfffbffff);
	udelay(5000);
	
	wCount = 200;
	while(wCount--) {
		wReg = snd_ali_codec_peek(codec, 0, AC97_POWERDOWN);
		if((wReg & 0x000f) == 0x000f)
			return 0;
		udelay(5000);
	}

	/* non-fatal if you have a non PM capable codec */
	/* snd_printk(KERN_WARNING "ali5451: reset time out\n"); */
	return 0;
}

#ifdef CODEC_RESET

static int snd_ali_reset_codec(ali_t *codec)
{
	struct pci_dev *pci_dev = NULL;
	unsigned char bVal = 0;
	unsigned int   dwVal;
	unsigned short wCount, wReg;

	pci_dev = codec->pci_m1533;
	
	pci_read_config_dword(pci_dev, 0x7c, &dwVal);
	pci_write_config_dword(pci_dev, 0x7c, dwVal | 0x08000000);
	udelay(5000);
	pci_read_config_dword(pci_dev, 0x7c, &dwVal);
	pci_write_config_dword(pci_dev, 0x7c, dwVal & 0xf7ffffff);
	udelay(5000);

	bVal = inb(ALI_REG(codec,ALI_SCTRL));
	bVal |= 0x02;
	outb(ALI_REG(codec,ALI_SCTRL),bVal);
	udelay(5000);
	bVal = inb(ALI_REG(codec,ALI_SCTRL));
	bVal &= 0xfd;
	outb(ALI_REG(codec,ALI_SCTRL),bVal);
	udelay(15000);

	wCount = 200;
	while(wCount--) {
		wReg = snd_ali_codec_read(codec->ac97, AC97_POWERDOWN);
		if((wReg & 0x000f) == 0x000f)
			return 0;
		udelay(5000);
	}
	return -1;
}

#endif

/*
 *  ALI 5451 Controller
 */

static void snd_ali_enable_special_channel(ali_t *codec, unsigned int channel)
{
	unsigned long dwVal = 0;

	dwVal  = inl(ALI_REG(codec,ALI_GLOBAL_CONTROL));
	dwVal |= 1 << (channel & 0x0000001f);
	outl(dwVal, ALI_REG(codec,ALI_GLOBAL_CONTROL));
}

static void snd_ali_disable_special_channel(ali_t *codec, unsigned int channel)
{
	unsigned long dwVal = 0;

	dwVal  = inl(ALI_REG(codec,ALI_GLOBAL_CONTROL));
	dwVal &= ~(1 << (channel & 0x0000001f));
	outl(dwVal, ALI_REG(codec,ALI_GLOBAL_CONTROL));
}

static void snd_ali_enable_address_interrupt(ali_t * codec)
{
	unsigned int gc;

	gc  = inl(ALI_REG(codec, ALI_GC_CIR));
	gc |= ENDLP_IE;
	gc |= MIDLP_IE;
	outl( gc, ALI_REG(codec, ALI_GC_CIR));
}

static void snd_ali_disable_address_interrupt(ali_t * codec)
{
	unsigned int gc;

	gc  = inl(ALI_REG(codec, ALI_GC_CIR));
	gc &= ~ENDLP_IE;
	gc &= ~MIDLP_IE;
	outl(gc, ALI_REG(codec, ALI_GC_CIR));
}

#if 0 // not used
static void snd_ali_enable_voice_irq(ali_t *codec, unsigned int channel)
{
	unsigned int mask;
	snd_ali_channel_control_t *pchregs = &(codec->chregs);

	snd_ali_printk("enable_voice_irq channel=%d\n",channel);
	
	mask = 1 << (channel & 0x1f);
	pchregs->data.ainten  = inl(ALI_REG(codec,pchregs->regs.ainten));
	pchregs->data.ainten |= mask;
	outl(pchregs->data.ainten,ALI_REG(codec,pchregs->regs.ainten));
}
#endif

static void snd_ali_disable_voice_irq(ali_t *codec, unsigned int channel)
{
	unsigned int mask;
	snd_ali_channel_control_t *pchregs = &(codec->chregs);

	snd_ali_printk("disable_voice_irq channel=%d\n",channel);

	mask = 1 << (channel & 0x1f);
	pchregs->data.ainten  = inl(ALI_REG(codec,pchregs->regs.ainten));
	pchregs->data.ainten &= ~mask;
	outl(pchregs->data.ainten,ALI_REG(codec,pchregs->regs.ainten));
}

static int snd_ali_alloc_pcm_channel(ali_t *codec, int channel)
{
	unsigned int idx =  channel & 0x1f;

	if (codec->synth.chcnt >= ALI_CHANNELS){
		snd_printk("ali_alloc_pcm_channel: no free channels.\n");
		return -1;
	}

	if (!(codec->synth.chmap & (1 << idx))) {
		codec->synth.chmap |= 1 << idx;
		codec->synth.chcnt++;
		snd_ali_printk("alloc_pcm_channel no. %d.\n",idx);
		return idx;
	}
	return -1;
}

static int snd_ali_find_free_channel(ali_t * codec, int rec)
{
	int idx;
	int result = -1;

	snd_ali_printk("find_free_channel: for %s\n",rec ? "rec" : "pcm");

	// recording
	if (rec) {
		if (codec->spdif_support &&
		    (inl(ALI_REG(codec, ALI_GLOBAL_CONTROL)) & ALI_SPDIF_IN_SUPPORT))
			idx = ALI_SPDIF_IN_CHANNEL;
		else
			idx = ALI_PCM_IN_CHANNEL;

		if ((result = snd_ali_alloc_pcm_channel(codec,idx)) >= 0) {
			return result;
		} else {
			snd_printk("ali_find_free_channel: record channel is busy now.\n");
			return -1;
		}
	}

	//playback...
	if (codec->spdif_support &&
	    (inl(ALI_REG(codec, ALI_GLOBAL_CONTROL)) & ALI_SPDIF_OUT_CH_ENABLE)) {
		idx = ALI_SPDIF_OUT_CHANNEL;
		if ((result = snd_ali_alloc_pcm_channel(codec,idx)) >= 0) {
			return result;
		} else {
			snd_printk("ali_find_free_channel: S/PDIF out channel is in busy now.\n");
		}
	}

	for (idx = 0; idx < ALI_CHANNELS; idx++) {
		if ((result = snd_ali_alloc_pcm_channel(codec,idx)) >= 0)
			return result;
	}
	snd_printk("ali_find_free_channel: no free channels.\n");
	return -1;
}

static void snd_ali_free_channel_pcm(ali_t *codec, int channel)
{
	unsigned int idx = channel & 0x0000001f;

	snd_ali_printk("free_channel_pcm channel=%d\n",channel);

	if (channel < 0 || channel >= ALI_CHANNELS)
		return;

	if (!(codec->synth.chmap & (1 << idx))) {
		snd_printk("ali_free_channel_pcm: channel %d is not in use.\n",channel);
		return;
	} else {
		codec->synth.chmap &= ~(1 << idx);
		codec->synth.chcnt--;
	}
}

#if 0 // not used
static void snd_ali_start_voice(ali_t * codec, unsigned int channel)
{
	unsigned int mask = 1 << (channel & 0x1f);
	
	snd_ali_printk("start_voice: channel=%d\n",channel);
	outl(mask, ALI_REG(codec,codec->chregs.regs.start));
}
#endif

static void snd_ali_stop_voice(ali_t * codec, unsigned int channel)
{
	unsigned int mask = 1 << (channel & 0x1f);

	snd_ali_printk("stop_voice: channel=%d\n",channel);
	outl(mask, ALI_REG(codec, codec->chregs.regs.stop));
}

/*
 *    S/PDIF Part
 */

static void snd_ali_delay(ali_t *codec,int interval)
{
	unsigned long  begintimer,currenttimer;

	begintimer   = inl(ALI_REG(codec, ALI_STIMER));
	currenttimer = inl(ALI_REG(codec, ALI_STIMER));

	while (currenttimer < begintimer + interval) {
		if(snd_ali_stimer_ready(codec, 1) < 0)
			break;
		currenttimer = inl(ALI_REG(codec,  ALI_STIMER));
	}
}

static void snd_ali_detect_spdif_rate(ali_t *codec)
{
	u16 wval  = 0;
	u16 count = 0;
	u8  bval = 0, R1 = 0, R2 = 0;

	bval  = inb(ALI_REG(codec,ALI_SPDIF_CTRL + 1));
	bval |= 0x1F;
	outb(bval,ALI_REG(codec,ALI_SPDIF_CTRL + 1));

	while (((R1 < 0x0B )||(R1 > 0x0E)) && (R1 != 0x12) && count <= 50000) {
		count ++;
		snd_ali_delay(codec, 6);
		bval = inb(ALI_REG(codec,ALI_SPDIF_CTRL + 1));
		R1 = bval & 0x1F;
	}

	if (count > 50000) {
		snd_printk("ali_detect_spdif_rate: timeout!\n");
		return;
	}

	count = 0;
	while (count++ <= 50000) {
		snd_ali_delay(codec, 6);
		bval = inb(ALI_REG(codec,ALI_SPDIF_CTRL + 1));
		R2 = bval & 0x1F;
		if (R2 != R1) R1 = R2; else break;
	}

	if (count > 50000) {
		snd_printk("ali_detect_spdif_rate: timeout!\n");
		return;
	}

	if (R2 >= 0x0b && R2 <= 0x0e) {
		wval  = inw(ALI_REG(codec,ALI_SPDIF_CTRL + 2));
		wval &= 0xE0F0;
		wval |= (u16)0x09 << 8 | (u16)0x05;
		outw(wval,ALI_REG(codec,ALI_SPDIF_CTRL + 2));

		bval  = inb(ALI_REG(codec,ALI_SPDIF_CS +3)) & 0xF0;
		outb(bval|0x02,ALI_REG(codec,ALI_SPDIF_CS + 3));
	} else if (R2 == 0x12) {
		wval  = inw(ALI_REG(codec,ALI_SPDIF_CTRL + 2));
		wval &= 0xE0F0;
		wval |= (u16)0x0E << 8 | (u16)0x08;
		outw(wval,ALI_REG(codec,ALI_SPDIF_CTRL + 2));

		bval  = inb(ALI_REG(codec,ALI_SPDIF_CS +3)) & 0xF0;
		outb(bval|0x03,ALI_REG(codec,ALI_SPDIF_CS + 3));
	}
}

static unsigned int snd_ali_get_spdif_in_rate(ali_t *codec)
{
	u32	dwRate = 0;
	u8	bval = 0;

	bval  = inb(ALI_REG(codec,ALI_SPDIF_CTRL));
	bval &= 0x7F;
	bval |= 0x40;
	outb(bval, ALI_REG(codec,ALI_SPDIF_CTRL));

	snd_ali_detect_spdif_rate(codec);

	bval  = inb(ALI_REG(codec,ALI_SPDIF_CS + 3));
	bval &= 0x0F;

	if (bval == 0) dwRate = 44100;
	if (bval == 1) dwRate = 48000;
	if (bval == 2) dwRate = 32000;

	return dwRate;
}

static void snd_ali_enable_spdif_in(ali_t *codec)
{	
	unsigned int dwVal;

	dwVal = inl(ALI_REG(codec, ALI_GLOBAL_CONTROL));
	dwVal |= ALI_SPDIF_IN_SUPPORT;
	outl(dwVal, ALI_REG(codec, ALI_GLOBAL_CONTROL));

	dwVal = inb(ALI_REG(codec, ALI_SPDIF_CTRL));
	dwVal |= 0x02;
	outb(dwVal, ALI_REG(codec, ALI_SPDIF_CTRL));

	snd_ali_enable_special_channel(codec, ALI_SPDIF_IN_CHANNEL);
}

static void snd_ali_disable_spdif_in(ali_t *codec)
{
	unsigned int dwVal;
	
	dwVal = inl(ALI_REG(codec, ALI_GLOBAL_CONTROL));
	dwVal &= ~ALI_SPDIF_IN_SUPPORT;
	outl(dwVal, ALI_REG(codec, ALI_GLOBAL_CONTROL));
	
	snd_ali_disable_special_channel(codec, ALI_SPDIF_IN_CHANNEL);	
}


static void snd_ali_set_spdif_out_rate(ali_t *codec, unsigned int rate)
{
	unsigned char  bVal;
	unsigned int  dwRate = 0;
	
	if (rate == 32000) dwRate = 0x300;
	if (rate == 44100) dwRate = 0;
	if (rate == 48000) dwRate = 0x200;
	
	bVal  = inb(ALI_REG(codec, ALI_SPDIF_CTRL));
	bVal &= (unsigned char)(~(1<<6));
	
	bVal |= 0x80;		//select right
	outb(bVal, ALI_REG(codec, ALI_SPDIF_CTRL));
	outb(dwRate | 0x20, ALI_REG(codec, ALI_SPDIF_CS + 2));
	
	bVal &= (~0x80);	//select left
	outb(bVal, ALI_REG(codec, ALI_SPDIF_CTRL));
	outw(rate | 0x10, ALI_REG(codec, ALI_SPDIF_CS + 2));
}

static void snd_ali_enable_spdif_out(ali_t *codec)
{
	unsigned short wVal;
	unsigned char bVal;

        struct pci_dev *pci_dev = NULL;

        pci_dev = codec->pci_m1533;
        if (pci_dev == NULL)
                return;
        pci_read_config_byte(pci_dev, 0x61, &bVal);
        bVal |= 0x40;
        pci_write_config_byte(pci_dev, 0x61, bVal);
        pci_read_config_byte(pci_dev, 0x7d, &bVal);
        bVal |= 0x01;
        pci_write_config_byte(pci_dev, 0x7d, bVal);

        pci_read_config_byte(pci_dev, 0x7e, &bVal);
        bVal &= (~0x20);
        bVal |= 0x10;
        pci_write_config_byte(pci_dev, 0x7e, bVal);

	bVal = inb(ALI_REG(codec, ALI_SCTRL));
	outb(bVal | ALI_SPDIF_OUT_ENABLE, ALI_REG(codec, ALI_SCTRL));

	bVal = inb(ALI_REG(codec, ALI_SPDIF_CTRL));
	outb(bVal & ALI_SPDIF_OUT_CH_STATUS, ALI_REG(codec, ALI_SPDIF_CTRL));
   
	{
   		wVal = inw(ALI_REG(codec, ALI_GLOBAL_CONTROL));
   		wVal |= ALI_SPDIF_OUT_SEL_PCM;
   		outw(wVal, ALI_REG(codec, ALI_GLOBAL_CONTROL));
		snd_ali_disable_special_channel(codec,ALI_SPDIF_OUT_CHANNEL);
   	}
}

static void snd_ali_enable_spdif_chnout(ali_t *codec)
{
	unsigned short wVal = 0;

	wVal  = inw(ALI_REG(codec, ALI_GLOBAL_CONTROL));
   	wVal &= ~ALI_SPDIF_OUT_SEL_PCM;
   	outw(wVal, ALI_REG(codec, ALI_GLOBAL_CONTROL));
/*
	wVal = inw(ALI_REG(codec, ALI_SPDIF_CS));
	if (flag & ALI_SPDIF_OUT_NON_PCM)
   		wVal |= 0x0002;
	else	
		wVal &= (~0x0002);
   	outw(wVal, ALI_REG(codec, ALI_SPDIF_CS));
*/
	snd_ali_enable_special_channel(codec,ALI_SPDIF_OUT_CHANNEL);
}

static void snd_ali_disable_spdif_chnout(ali_t *codec)
{
	unsigned short wVal = 0;
  	wVal  = inw(ALI_REG(codec, ALI_GLOBAL_CONTROL));
   	wVal |= ALI_SPDIF_OUT_SEL_PCM;
   	outw(wVal, ALI_REG(codec, ALI_GLOBAL_CONTROL));

	snd_ali_enable_special_channel(codec, ALI_SPDIF_OUT_CHANNEL);
}

static void snd_ali_disable_spdif_out(ali_t *codec)
{
	unsigned char  bVal;

	bVal = inb(ALI_REG(codec, ALI_SCTRL));
	outb(bVal & ~ALI_SPDIF_OUT_ENABLE, ALI_REG(codec, ALI_SCTRL));

	snd_ali_disable_spdif_chnout(codec);
}

static void snd_ali_update_ptr(ali_t *codec,int channel)
{
	snd_ali_voice_t *pvoice = NULL;
	snd_pcm_runtime_t *runtime;
	snd_ali_channel_control_t *pchregs = NULL;
	unsigned int old, mask;
#ifdef ALI_DEBUG
	unsigned int temp, cspf;
#endif

	pchregs = &(codec->chregs);

	// check if interrupt occurred for channel
	old  = pchregs->data.aint;
	mask = ((unsigned int) 1L) << (channel & 0x1f);

	if (!(old & mask))
		return;

	pvoice = &codec->synth.voices[channel];
	runtime = pvoice->substream->runtime;

	udelay(100);
	spin_lock(&codec->reg_lock);

	if (pvoice->pcm && pvoice->substream) {
		/* pcm interrupt */
#ifdef ALI_DEBUG
		outb((u8)(pvoice->number), ALI_REG(codec, ALI_GC_CIR));
		temp = inw(ALI_REG(codec, ALI_CSO_ALPHA_FMS + 2));
		cspf = (inl(ALI_REG(codec, ALI_CSPF)) & mask) == mask;
#endif
		if (pvoice->running) {
			snd_ali_printk("update_ptr: cso=%4.4x cspf=%d.\n",(u16)temp,cspf);
			spin_unlock(&codec->reg_lock);
			snd_pcm_period_elapsed(pvoice->substream);
			spin_lock(&codec->reg_lock);
		} else {
			snd_ali_stop_voice(codec, channel);
			snd_ali_disable_voice_irq(codec, channel);
		}	
	} else if (codec->synth.voices[channel].synth) {
		/* synth interrupt */
	} else if (codec->synth.voices[channel].midi) {
		/* midi interrupt */
	} else {
		/* unknown interrupt */
		snd_ali_stop_voice(codec, channel);
		snd_ali_disable_voice_irq(codec, channel);
	}
	spin_unlock(&codec->reg_lock);
	outl(mask,ALI_REG(codec,pchregs->regs.aint));
	pchregs->data.aint = old & (~mask);
}

static void snd_ali_interrupt(ali_t * codec)
{
	int channel;
	unsigned int audio_int;
	snd_ali_channel_control_t *pchregs = NULL;
	pchregs = &(codec->chregs);

	audio_int = inl(ALI_REG(codec, ALI_MISCINT));
	if (audio_int & ADDRESS_IRQ) {
		// get interrupt status for all channels
		pchregs->data.aint = inl(ALI_REG(codec,pchregs->regs.aint));
		for (channel = 0; channel < ALI_CHANNELS; channel++) {
			snd_ali_update_ptr(codec, channel);
		}
	}
	outl((TARGET_REACHED | MIXER_OVERFLOW | MIXER_UNDERFLOW),
		ALI_REG(codec,ALI_MISCINT));
}


static irqreturn_t snd_ali_card_interrupt(int irq,
				   void *dev_id,
				   struct pt_regs *regs)
{
	ali_t 	*codec = snd_magic_cast(ali_t, dev_id, return IRQ_NONE);

	if (codec == NULL)
		return IRQ_NONE;
	snd_ali_interrupt(codec);
	return IRQ_HANDLED;
}


static snd_ali_voice_t *snd_ali_alloc_voice(ali_t * codec, int type, int rec)
{
	snd_ali_voice_t *pvoice = NULL;
	unsigned long flags;
	int idx;

	snd_ali_printk("alloc_voice: type=%d rec=%d\n",type,rec);

	spin_lock_irqsave(&codec->voice_alloc, flags);
	if (type == SNDRV_ALI_VOICE_TYPE_PCM) {
		idx = snd_ali_find_free_channel(codec,rec);
		if(idx < 0) {
			snd_printk("ali_alloc_voice: err.\n");
			spin_unlock_irqrestore(&codec->voice_alloc, flags);
			return NULL;
		}
		pvoice = &(codec->synth.voices[idx]);
		pvoice->use = 1;
		pvoice->pcm = 1;
		pvoice->mode = rec;
		spin_unlock_irqrestore(&codec->voice_alloc, flags);
		return pvoice;
	}
	spin_unlock_irqrestore(&codec->voice_alloc, flags);
	return NULL;
}


static void snd_ali_free_voice(ali_t * codec, snd_ali_voice_t *pvoice)
{
	unsigned long flags;
	void (*private_free)(void *);
	void *private_data;

	snd_ali_printk("free_voice: channel=%d\n",pvoice->number);
	if (pvoice == NULL || !pvoice->use)
		return;
	snd_ali_clear_voices(codec, pvoice->number, pvoice->number);
	spin_lock_irqsave(&codec->voice_alloc, flags);
	private_free = pvoice->private_free;
	private_data = pvoice->private_data;
	pvoice->private_free = NULL;
	pvoice->private_data = NULL;
	if (pvoice->pcm) {
		snd_ali_free_channel_pcm(codec, pvoice->number);
	}
	pvoice->use = pvoice->pcm = pvoice->synth = 0;
	pvoice->substream = NULL;
	spin_unlock_irqrestore(&codec->voice_alloc, flags);
	if (private_free)
		private_free(private_data);
}


static void snd_ali_clear_voices(ali_t * codec,
			  unsigned int v_min,
			  unsigned int v_max)
{
	unsigned int i;

	for (i = v_min; i <= v_max; i++) {
		snd_ali_stop_voice(codec, i);
		snd_ali_disable_voice_irq(codec, i);
	}
}

static void snd_ali_write_voice_regs(ali_t * codec,
			 unsigned int Channel,
			 unsigned int LBA,
			 unsigned int CSO,
			 unsigned int ESO,
			 unsigned int DELTA,
			 unsigned int ALPHA_FMS,
			 unsigned int GVSEL,
			 unsigned int PAN,
			 unsigned int VOL,
			 unsigned int CTRL,
			 unsigned int EC)
{
	unsigned int ctlcmds[4];
	
	outb((unsigned char)(Channel & 0x001f),ALI_REG(codec,ALI_GC_CIR));

	ctlcmds[0] =  (CSO << 16) | (ALPHA_FMS & 0x0000ffff);
	ctlcmds[1] =  LBA;
	ctlcmds[2] =  (ESO << 16) | (DELTA & 0x0ffff);
	ctlcmds[3] =  (GVSEL << 31) |
		      ((PAN & 0x0000007f) << 24) |
		      ((VOL & 0x000000ff) << 16) |
		      ((CTRL & 0x0000000f) << 12) |
		      (EC & 0x00000fff);

	outb(Channel, ALI_REG(codec, ALI_GC_CIR));

	outl(ctlcmds[0], ALI_REG(codec,ALI_CSO_ALPHA_FMS));
	outl(ctlcmds[1], ALI_REG(codec,ALI_LBA));
	outl(ctlcmds[2], ALI_REG(codec,ALI_ESO_DELTA));
	outl(ctlcmds[3], ALI_REG(codec,ALI_GVSEL_PAN_VOC_CTRL_EC));

	outl(0x30000000, ALI_REG(codec, ALI_EBUF1));	/* Still Mode */
	outl(0x30000000, ALI_REG(codec, ALI_EBUF2));	/* Still Mode */
}

static unsigned int snd_ali_convert_rate(unsigned int rate, int rec)
{
	unsigned int delta;

	if (rate < 4000)  rate = 4000;
	if (rate > 48000) rate = 48000;

	if (rec) {
		if (rate == 44100)
			delta = 0x116a;
		else if (rate == 8000)
			delta = 0x6000;
		else if (rate == 48000)
			delta = 0x1000;
		else
			delta = ((48000 << 12) / rate) & 0x0000ffff;
	} else {
		if (rate == 44100)
			delta = 0xeb3;
		else if (rate == 8000)
			delta = 0x2ab;
		else if (rate == 48000)
			delta = 0x1000;
		else 
			delta = (((rate << 12) + rate) / 48000) & 0x0000ffff;
	}

	return delta;
}

static unsigned int snd_ali_control_mode(snd_pcm_substream_t *substream)
{
	unsigned int CTRL;
	snd_pcm_runtime_t *runtime = substream->runtime;

	/* set ctrl mode
	   CTRL default: 8-bit (unsigned) mono, loop mode enabled
	 */
	CTRL = 0x00000001;
	if (snd_pcm_format_width(runtime->format) == 16)
		CTRL |= 0x00000008;	// 16-bit data
	if (!snd_pcm_format_unsigned(runtime->format))
		CTRL |= 0x00000002;	// signed data
	if (runtime->channels > 1)
		CTRL |= 0x00000004;	// stereo data
	return CTRL;
}

/*
 *  PCM part
 */

static int snd_ali_ioctl(snd_pcm_substream_t * substream,
				  unsigned int cmd, void *arg)
{
	return snd_pcm_lib_ioctl(substream, cmd, arg);
}

static int snd_ali_trigger(snd_pcm_substream_t *substream,
			       int cmd)
				    
{
	ali_t *codec = snd_pcm_substream_chip(substream);
	struct list_head *pos;
	snd_pcm_substream_t *s;
	unsigned int what, whati, capture_flag;
	snd_ali_voice_t *pvoice = NULL, *evoice = NULL;
	unsigned int val;
	int do_start;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		do_start = 1; break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		do_start = 0; break;
	default:
		return -EINVAL;
	}

	what = whati = capture_flag = 0;
	snd_pcm_group_for_each(pos, substream) {
		s = snd_pcm_group_substream_entry(pos);
		if ((ali_t *) _snd_pcm_chip(s->pcm) == codec) {
			pvoice = (snd_ali_voice_t *) s->runtime->private_data;
			evoice = pvoice->extra;
			what |= 1 << (pvoice->number & 0x1f);
			if (evoice == NULL) {
				whati |= 1 << (pvoice->number & 0x1f);
			} else {
				whati |= 1 << (evoice->number & 0x1f);
				what |= 1 << (evoice->number & 0x1f);
			}
			if (do_start) {
				pvoice->running = 1;
				if (evoice != NULL)
					evoice->running = 1;
			} else {
				pvoice->running = 0;
				if (evoice != NULL)
					evoice->running = 0;
			}
			snd_pcm_trigger_done(s, substream);
			if (pvoice->mode)
				capture_flag = 1;
		}
	}
	spin_lock(&codec->reg_lock);
	if (! do_start) {
		outl(what, ALI_REG(codec, ALI_STOP));
	}
	val = inl(ALI_REG(codec, ALI_AINTEN));
	if (do_start) {
		val |= whati;
	} else {
		val &= ~whati;
	}
	outl(val, ALI_REG(codec, ALI_AINTEN));
	if (do_start) {
		outl(what, ALI_REG(codec, ALI_START));
	}
	snd_ali_printk("trigger: what=%xh whati=%xh\n",what,whati);
	spin_unlock(&codec->reg_lock);

	return 0;
}

static int snd_ali_playback_hw_params(snd_pcm_substream_t * substream,
				 snd_pcm_hw_params_t * hw_params)
{
	ali_t *codec = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_ali_voice_t *pvoice = (snd_ali_voice_t *) runtime->private_data;
	snd_ali_voice_t *evoice = pvoice->extra;
	int err;
	err = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
	if (err < 0) return err;
	
	/* voice management */

	if (params_buffer_size(hw_params)/2 != params_period_size(hw_params)) {
		if (evoice == NULL) {
			evoice = snd_ali_alloc_voice(codec, SNDRV_ALI_VOICE_TYPE_PCM, 0);
			if (evoice == NULL)
				return -ENOMEM;
			pvoice->extra = evoice;
			evoice->substream = substream;
		}
	} else {
		if (evoice != NULL) {
			snd_ali_free_voice(codec, evoice);
			pvoice->extra = evoice = NULL;
		}
	}

	return 0;
}

static int snd_ali_playback_hw_free(snd_pcm_substream_t * substream)
{
	ali_t *codec = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_ali_voice_t *pvoice = (snd_ali_voice_t *) runtime->private_data;
	snd_ali_voice_t *evoice = pvoice ? pvoice->extra : NULL;

	snd_pcm_lib_free_pages(substream);
	if (evoice != NULL) {
		snd_ali_free_voice(codec, evoice);
		pvoice->extra = NULL;
	}
	return 0;
}

static int snd_ali_capture_hw_params(snd_pcm_substream_t * substream,
				 snd_pcm_hw_params_t * hw_params)
{
	return snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
}

static int snd_ali_capture_hw_free(snd_pcm_substream_t * substream)
{
	return snd_pcm_lib_free_pages(substream);
}

static int snd_ali_playback_prepare(snd_pcm_substream_t * substream)
{
	ali_t *codec = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_ali_voice_t *pvoice = (snd_ali_voice_t *) runtime->private_data;
	snd_ali_voice_t *evoice = pvoice->extra;
	unsigned long flags;

	unsigned int LBA;
	unsigned int Delta;
	unsigned int ESO;
	unsigned int CTRL;
	unsigned int GVSEL;
	unsigned int PAN;
	unsigned int VOL;
	unsigned int EC;
	
	snd_ali_printk("playback_prepare ...\n");

	spin_lock_irqsave(&codec->reg_lock, flags);	
	
	/* set Delta (rate) value */
	Delta = snd_ali_convert_rate(runtime->rate, 0);

	if ((pvoice->number == ALI_SPDIF_IN_CHANNEL) || 
	    (pvoice->number == ALI_PCM_IN_CHANNEL))
		snd_ali_disable_special_channel(codec, pvoice->number);
	else if (codec->spdif_support &&
		 (inl(ALI_REG(codec, ALI_GLOBAL_CONTROL)) & ALI_SPDIF_OUT_CH_ENABLE)
		 && (pvoice->number == ALI_SPDIF_OUT_CHANNEL)) {
		snd_ali_set_spdif_out_rate(codec, runtime->rate);
		Delta = 0x1000;
	}
	
	/* set Loop Back Address */
	LBA = runtime->dma_addr;

	/* set interrupt count size */
	pvoice->count = runtime->period_size;

	/* set target ESO for channel */
	pvoice->eso = runtime->buffer_size; 

	snd_ali_printk("playback_prepare: eso=%xh count=%xh\n",pvoice->eso,pvoice->count);

	/* set ESO to capture first MIDLP interrupt */
	ESO = pvoice->eso -1;
	/* set ctrl mode */
	CTRL = snd_ali_control_mode(substream);

	GVSEL = 1;
	PAN = 0;
	VOL = 0;
	EC = 0;
	snd_ali_printk("playback_prepare:\n    ch=%d, Rate=%d Delta=%xh,GVSEL=%xh,PAN=%xh,CTRL=%xh\n",pvoice->number,runtime->rate,Delta,GVSEL,PAN,CTRL);
	snd_ali_write_voice_regs(    codec,
				     pvoice->number,
				     LBA,
				     0,	/* cso */
				     ESO,
				     Delta,
				     0,	/* alpha */
				     GVSEL,
				     PAN,
				     VOL,
				     CTRL,
				     EC);
	if (evoice != NULL) {
		evoice->count = pvoice->count;
		evoice->eso = pvoice->count << 1;
		ESO = evoice->eso - 1;
		snd_ali_write_voice_regs(codec,
				     evoice->number,
				     LBA,
				     0,	/* cso */
				     ESO,
				     Delta,
				     0,	/* alpha */
				     GVSEL,
				     (unsigned int)0x7f,
				     (unsigned int)0x3ff,
				     CTRL,
				     EC);
	}
	spin_unlock_irqrestore(&codec->reg_lock, flags);
	return 0;
}


static int snd_ali_capture_prepare(snd_pcm_substream_t * substream)
{
	ali_t *codec = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_ali_voice_t *pvoice = (snd_ali_voice_t *) runtime->private_data;
	unsigned long flags;
	unsigned int LBA;
	unsigned int Delta;
	unsigned int ESO;
	unsigned int CTRL;
	unsigned int GVSEL;
	unsigned int PAN;
	unsigned int VOL;
	unsigned int EC;
	u8	 bValue;

	spin_lock_irqsave(&codec->reg_lock, flags);

	snd_ali_printk("capture_prepare...\n");

	snd_ali_enable_special_channel(codec,pvoice->number);

	Delta = snd_ali_convert_rate(runtime->rate, 1);

	// Prepare capture intr channel
	if (pvoice->number == ALI_SPDIF_IN_CHANNEL) {

		unsigned int rate;
		
		if (codec->revision != ALI_5451_V02) {
			spin_unlock_irqrestore(&codec->reg_lock, flags);			
			return -1;
		}
		rate = snd_ali_get_spdif_in_rate(codec);
		if (rate == 0) {
			snd_printk("ali_capture_preapre: spdif rate detect err!\n");
			rate = 48000;
		}
		bValue = inb(ALI_REG(codec,ALI_SPDIF_CTRL));
		if (bValue & 0x10) {
			outb(bValue,ALI_REG(codec,ALI_SPDIF_CTRL));
			printk("clear SPDIF parity error flag.\n");
		}

		if (rate != 48000)
			Delta = ((rate << 12)/runtime->rate)&0x00ffff;
	}

	// set target ESO for channel 
	pvoice->eso = runtime->buffer_size; 

	// set interrupt count size 
	pvoice->count = runtime->period_size;

	// set Loop Back Address 
	LBA = runtime->dma_addr;

	// set ESO to capture first MIDLP interrupt 
	ESO = pvoice->eso - 1;
	CTRL = snd_ali_control_mode(substream);
	GVSEL = 0;
	PAN = 0x00;
	VOL = 0x00;
	EC = 0;

	snd_ali_write_voice_regs(    codec,
				     pvoice->number,
				     LBA,
				     0,	/* cso */
				     ESO,
				     Delta,
				     0,	/* alpha */
				     GVSEL,
				     PAN,
				     VOL,
				     CTRL,
				     EC);


	spin_unlock_irqrestore(&codec->reg_lock, flags);

	return 0;
}


static snd_pcm_uframes_t snd_ali_playback_pointer(snd_pcm_substream_t *substream)
{
	ali_t *codec = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_ali_voice_t *pvoice = (snd_ali_voice_t *) runtime->private_data;
	unsigned int cso;
	unsigned long flags;

	spin_lock_irqsave(&codec->reg_lock, flags);
	if (!pvoice->running) {
		spin_unlock_irqrestore(&codec->reg_lock, flags);
		return 0;
	}
	outb(pvoice->number, ALI_REG(codec, ALI_GC_CIR));
	cso = inw(ALI_REG(codec, ALI_CSO_ALPHA_FMS + 2));
	spin_unlock_irqrestore(&codec->reg_lock, flags);
	snd_ali_printk("playback pointer returned cso=%xh.\n", cso);

	return cso;
}


static snd_pcm_uframes_t snd_ali_capture_pointer(snd_pcm_substream_t *substream)
{
	ali_t *codec = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_ali_voice_t *pvoice = (snd_ali_voice_t *) runtime->private_data;
	unsigned int cso;
	unsigned long flags;

	spin_lock_irqsave(&codec->reg_lock, flags);
	if (!pvoice->running) {
		spin_unlock_irqrestore(&codec->reg_lock, flags);
		return 0;
	}
	outb(pvoice->number, ALI_REG(codec, ALI_GC_CIR));
	cso = inw(ALI_REG(codec, ALI_CSO_ALPHA_FMS + 2));
	spin_unlock_irqrestore(&codec->reg_lock, flags);

	return cso;
}

static snd_pcm_hardware_t snd_ali_playback =
{
	.info =			(SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_BLOCK_TRANSFER |
				 SNDRV_PCM_INFO_MMAP_VALID |
				 SNDRV_PCM_INFO_RESUME |
				 SNDRV_PCM_INFO_SYNC_START),
	.formats =		(SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S16_LE |
				 SNDRV_PCM_FMTBIT_S8 | SNDRV_PCM_FMTBIT_U16_LE),
	.rates =		SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_48000,
	.rate_min =		4000,
	.rate_max =		48000,
	.channels_min =		1,
	.channels_max =		2,
	.buffer_bytes_max =	(256*1024),
	.period_bytes_min =	64,
	.period_bytes_max =	(256*1024),
	.periods_min =		1,
	.periods_max =		1024,
	.fifo_size =		0,
};

/*
 *  Capture support device description
 */

static snd_pcm_hardware_t snd_ali_capture =
{
	.info =			(SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_BLOCK_TRANSFER |
				 SNDRV_PCM_INFO_MMAP_VALID |
				 SNDRV_PCM_INFO_RESUME |
				 SNDRV_PCM_INFO_SYNC_START),
	.formats =		(SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S16_LE |
				 SNDRV_PCM_FMTBIT_S8 | SNDRV_PCM_FMTBIT_U16_LE),
	.rates =		SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_48000,
	.rate_min =		4000,
	.rate_max =		48000,
	.channels_min =		1,
	.channels_max =		2,
	.buffer_bytes_max =	(128*1024),
	.period_bytes_min =	64,
	.period_bytes_max =	(128*1024),
	.periods_min =		1,
	.periods_max =		1024,
	.fifo_size =		0,
};

static void snd_ali_pcm_free_substream(snd_pcm_runtime_t *runtime)
{
	unsigned long flags;
	snd_ali_voice_t *pvoice = (snd_ali_voice_t *) runtime->private_data;
	ali_t *codec;

	if (pvoice) {
		codec = pvoice->codec;
		spin_lock_irqsave(&codec->reg_lock, flags);
		snd_ali_free_voice(pvoice->codec, pvoice);
		spin_unlock_irqrestore(&codec->reg_lock, flags);
	}
}

static int snd_ali_playback_open(snd_pcm_substream_t * substream)
{
	ali_t *codec = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_ali_voice_t *pvoice;
	unsigned long flags = 0;

	spin_lock_irqsave(&codec->reg_lock, flags);
	pvoice = snd_ali_alloc_voice(codec, SNDRV_ALI_VOICE_TYPE_PCM, 0);
	if (pvoice == NULL) {
		spin_unlock_irqrestore(&codec->reg_lock, flags);
		return -EAGAIN;
	}
	pvoice->codec = codec;
	spin_unlock_irqrestore(&codec->reg_lock, flags);

	pvoice->substream = substream;
	runtime->private_data = pvoice;
	runtime->private_free = snd_ali_pcm_free_substream;

	runtime->hw = snd_ali_playback;
	snd_pcm_set_sync(substream);
	snd_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, 0, 64*1024);
	return 0;
}


static int snd_ali_capture_open(snd_pcm_substream_t * substream)
{
	ali_t *codec = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_ali_voice_t *pvoice;
	unsigned long flags;

	spin_lock_irqsave(&codec->reg_lock, flags);
	pvoice = snd_ali_alloc_voice(codec, SNDRV_ALI_VOICE_TYPE_PCM, 1);
	if (pvoice == NULL) {
		spin_unlock_irqrestore(&codec->reg_lock, flags);
		return -EAGAIN;
	}
	pvoice->codec = codec;
	spin_unlock_irqrestore(&codec->reg_lock, flags);

	pvoice->substream = substream;
	runtime->private_data = pvoice;
	runtime->private_free = snd_ali_pcm_free_substream;
	runtime->hw = snd_ali_capture;
	snd_pcm_set_sync(substream);
	snd_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, 0, 64*1024);
	return 0;
}


static int snd_ali_playback_close(snd_pcm_substream_t * substream)
{
	return 0;
}

static int snd_ali_capture_close(snd_pcm_substream_t * substream)
{
	ali_t *codec = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_ali_voice_t *pvoice = (snd_ali_voice_t *) runtime->private_data;

	snd_ali_disable_special_channel(codec,pvoice->number);

	return 0;
}

static snd_pcm_ops_t snd_ali_playback_ops = {
	.open =		snd_ali_playback_open,
	.close =	snd_ali_playback_close,
	.ioctl =	snd_ali_ioctl,
	.hw_params =	snd_ali_playback_hw_params,
	.hw_free =	snd_ali_playback_hw_free,
	.prepare =	snd_ali_playback_prepare,
	.trigger =	snd_ali_trigger,
	.pointer =	snd_ali_playback_pointer,
};

static snd_pcm_ops_t snd_ali_capture_ops = {
	.open =		snd_ali_capture_open,
	.close =	snd_ali_capture_close,
	.ioctl =	snd_ali_ioctl,
	.hw_params =	snd_ali_capture_hw_params,
	.hw_free =	snd_ali_capture_hw_free,
	.prepare =	snd_ali_capture_prepare,
	.trigger =	snd_ali_trigger,
	.pointer =	snd_ali_capture_pointer,
};


static void snd_ali_pcm_free(snd_pcm_t *pcm)
{
	ali_t *codec = snd_magic_cast(ali_t, pcm->private_data, return);
	codec->pcm = NULL;
}

static int __devinit snd_ali_pcm(ali_t * codec, int device, snd_pcm_t ** rpcm)
{
	snd_pcm_t *pcm;
	int err;

	if (rpcm) *rpcm = NULL;
	err = snd_pcm_new(codec->card, "ALI 5451", device, ALI_CHANNELS, 1, &pcm);
	if (err < 0) {
		snd_printk("snd_ali_pcm: err called snd_pcm_new.\n");
		return err;
	}
	pcm->private_data = codec;
	pcm->private_free = snd_ali_pcm_free;
	pcm->info_flags = 0;
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &snd_ali_playback_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &snd_ali_capture_ops);

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV,
					      snd_dma_pci_data(codec->pci), 64*1024, 128*1024);

	pcm->info_flags = 0;
	pcm->dev_subclass = SNDRV_PCM_SUBCLASS_GENERIC_MIX;
	strcpy(pcm->name, "ALI 5451");
	codec->pcm = pcm;
	if (rpcm) *rpcm = pcm;
	return 0;
}

#define ALI5451_SPDIF(xname, xindex, value) \
{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, .index = xindex,\
.info = snd_ali5451_spdif_info, .get = snd_ali5451_spdif_get, \
.put = snd_ali5451_spdif_put, .private_value = value}

static int snd_ali5451_spdif_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t *uinfo)
{
        uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
        uinfo->count = 1;
        uinfo->value.integer.min = 0;
        uinfo->value.integer.max = 1;
        return 0;
}

static int snd_ali5451_spdif_get(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	unsigned long flags;
	ali_t *codec = snd_magic_cast(ali_t, kcontrol->private_data, -ENXIO);
	unsigned int enable;

	enable = ucontrol->value.integer.value[0] ? 1 : 0;

	spin_lock_irqsave(&codec->reg_lock, flags);
	switch(kcontrol->private_value) {
	case 0:
		enable = (codec->spdif_mask & 0x02) ? 1 : 0;
		break;
	case 1:
		enable = ((codec->spdif_mask & 0x02) && (codec->spdif_mask & 0x04)) ? 1 : 0;
		break;
	case 2:
		enable = (codec->spdif_mask & 0x01) ? 1 : 0;
		break;
	default:
		break;
	}
	ucontrol->value.integer.value[0] = enable;
	spin_unlock_irqrestore(&codec->reg_lock, flags);
	return 0;
}

static int snd_ali5451_spdif_put(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	unsigned long flags;
	ali_t *codec = snd_magic_cast(ali_t, kcontrol->private_data, -ENXIO);
	unsigned int change = 0, enable = 0;

	enable = ucontrol->value.integer.value[0] ? 1 : 0;

	spin_lock_irqsave(&codec->reg_lock, flags);
	switch (kcontrol->private_value) {
	case 0:
		change = (codec->spdif_mask & 0x02) ? 1 : 0;
		change = change ^ enable;
		if (change) {
			if (enable) {
				codec->spdif_mask |= 0x02;
				snd_ali_enable_spdif_out(codec);
			} else {
				codec->spdif_mask &= ~(0x02);
				codec->spdif_mask &= ~(0x04);
				snd_ali_disable_spdif_out(codec);
			}
		}
		break;
	case 1: 
		change = (codec->spdif_mask & 0x04) ? 1 : 0;
		change = change ^ enable;
		if (change && (codec->spdif_mask & 0x02)) {
			if (enable) {
				codec->spdif_mask |= 0x04;
				snd_ali_enable_spdif_chnout(codec);
			} else {
				codec->spdif_mask &= ~(0x04);
				snd_ali_disable_spdif_chnout(codec);
			}
		}
		break;
	case 2:
		change = (codec->spdif_mask & 0x01) ? 1 : 0;
		change = change ^ enable;
		if (change) {
			if (enable) {
				codec->spdif_mask |= 0x01;
				snd_ali_enable_spdif_in(codec);
			} else {
				codec->spdif_mask &= ~(0x01);
				snd_ali_disable_spdif_in(codec);
			}
		}
		break;
	default:
		break;
	}
	spin_unlock_irqrestore(&codec->reg_lock, flags);
	
	return change;
}

static snd_kcontrol_new_t snd_ali5451_mixer_spdif[] __devinitdata = {
	/* spdif aplayback switch */
	/* FIXME: "IEC958 Playback Switch" may conflict with one on ac97_codec */
	ALI5451_SPDIF("IEC958 Output switch", 0, 0),
	/* spdif out to spdif channel */
	ALI5451_SPDIF("IEC958 Channel Output Switch", 0, 1),
	/* spdif in from spdif channel */
	ALI5451_SPDIF(SNDRV_CTL_NAME_IEC958("",CAPTURE,SWITCH), 0, 2)
};

static void snd_ali_mixer_free_ac97_bus(ac97_bus_t *bus)
{
	ali_t *codec = snd_magic_cast(ali_t, bus->private_data, return);
	codec->ac97_bus = NULL;
}

static void snd_ali_mixer_free_ac97(ac97_t *ac97)
{
	ali_t *codec = snd_magic_cast(ali_t, ac97->private_data, return);
	codec->ac97 = NULL;
}

static int __devinit snd_ali_mixer(ali_t * codec)
{
	ac97_bus_t bus;
	ac97_t ac97;
	unsigned int idx;
	int err;

	memset(&bus, 0, sizeof(bus));
	bus.write = snd_ali_codec_write;
	bus.read = snd_ali_codec_read;
	bus.private_data = codec;
	bus.private_free = snd_ali_mixer_free_ac97_bus;
	if ((err = snd_ac97_bus(codec->card, &bus, &codec->ac97_bus)) < 0)
		return err;

	memset(&ac97, 0, sizeof(ac97));
	ac97.private_data = codec;
	ac97.private_free = snd_ali_mixer_free_ac97;
	if ((err = snd_ac97_mixer(codec->ac97_bus, &ac97, &codec->ac97)) < 0) {
		snd_printk("ali mixer creating error.\n");
		return err;
	}
	if (codec->spdif_support) {
		for(idx = 0; idx < ARRAY_SIZE(snd_ali5451_mixer_spdif); idx++) {
			err=snd_ctl_add(codec->card, snd_ctl_new1(&snd_ali5451_mixer_spdif[idx], codec));
			if (err < 0) return err;
		}
	}
	return 0;
}

#ifdef CONFIG_PM
static int ali_suspend(snd_card_t *card, unsigned int state)
{
	ali_t *chip = snd_magic_cast(ali_t, card->pm_private_data, return -EINVAL);
	ali_image_t *im;
	int i, j;

	im = chip->image;
	if (! im)
		return 0;

	snd_pcm_suspend_all(chip->pcm);
	snd_ac97_suspend(chip->ac97);

	spin_lock_irq(&chip->reg_lock);
	
	im->regs[ALI_MISCINT >> 2] = inl(ALI_REG(chip, ALI_MISCINT));
	// im->regs[ALI_START >> 2] = inl(ALI_REG(chip, ALI_START));
	im->regs[ALI_STOP >> 2] = inl(ALI_REG(chip, ALI_STOP));
	
	// disable all IRQ bits
	outl(0, ALI_REG(chip, ALI_MISCINT));
	
	for (i = 0; i < ALI_GLOBAL_REGS; i++) {	
		if ((i*4 == ALI_MISCINT) || (i*4 == ALI_STOP))
			continue;
		im->regs[i] = inl(ALI_REG(chip, i*4));
	}
	
	for (i = 0; i < ALI_CHANNELS; i++) {
		outb(i, ALI_REG(chip, ALI_GC_CIR));
		for (j = 0; j < ALI_CHANNEL_REGS; j++) 
			im->channel_regs[i][j] = inl(ALI_REG(chip, j*4 + 0xe0));
	}

	// stop all HW channel
	outl(0xffffffff, ALI_REG(chip, ALI_STOP));

	spin_unlock_irq(&chip->reg_lock);
	return 0;
}

static int ali_resume(snd_card_t *card, unsigned int state)
{
	ali_t *chip = snd_magic_cast(ali_t, card->pm_private_data, return -EINVAL);
	ali_image_t *im;
	int i, j;

	im = chip->image;
	if (! im)
		return 0;

	pci_enable_device(chip->pci);

	spin_lock_irq(&chip->reg_lock);
	
	for (i = 0; i < ALI_CHANNELS; i++) {
		outb(i, ALI_REG(chip, ALI_GC_CIR));
		for (j = 0; j < ALI_CHANNEL_REGS; j++) 
			outl(im->channel_regs[i][j], ALI_REG(chip, j*4 + 0xe0));
	}
	
	for (i = 0; i < ALI_GLOBAL_REGS; i++) {	
		if ((i*4 == ALI_MISCINT) || (i*4 == ALI_STOP) || (i*4 == ALI_START))
			continue;
		outl(im->regs[i], ALI_REG(chip, i*4));
	}
	
	// start HW channel
	outl(im->regs[ALI_START >> 2], ALI_REG(chip, ALI_START));
	// restore IRQ enable bits
	outl(im->regs[ALI_MISCINT >> 2], ALI_REG(chip, ALI_MISCINT));
	
	spin_unlock_irq(&chip->reg_lock);

	snd_ac97_resume(chip->ac97);
	
	return 0;
}
#endif /* CONFIG_PM */

static int snd_ali_free(ali_t * codec)
{
	if (codec->hw_initialized)
		snd_ali_disable_address_interrupt(codec);
	if (codec->irq >= 0) {
		synchronize_irq(codec->irq);
		free_irq(codec->irq, (void *)codec);
	}
	if (codec->res_port) {
		release_resource(codec->res_port);
		kfree_nocheck(codec->res_port);
	}
#ifdef CONFIG_PM
	if (codec->image)
		kfree(codec->image);
#endif
	snd_magic_kfree(codec);
	return 0;
}

static int snd_ali_chip_init(ali_t *codec)
{
	unsigned int legacy;
	unsigned char temp;
	struct pci_dev *pci_dev = NULL;

	snd_ali_printk("chip initializing ... \n");

	if (snd_ali_reset_5451(codec)) {
		snd_printk("ali_chip_init: reset 5451 error.\n");
		return -1;
	}

	if (codec->revision == ALI_5451_V02) {
        	pci_dev = codec->pci_m1533;
		pci_read_config_byte(pci_dev, 0x59, &temp);
		temp |= 0x80;
		pci_write_config_byte(pci_dev, 0x59, temp);
	
		pci_dev = codec->pci_m7101;
		pci_read_config_byte(pci_dev, 0xb8, &temp);
		temp |= 0x20;
		pci_write_config_byte(pci_dev, 0xB8, temp);
	}

	pci_read_config_dword(codec->pci, 0x44, &legacy);
	legacy &= 0xff00ff00;
	legacy |= 0x000800aa;
	pci_write_config_dword(codec->pci, 0x44, legacy);

	outl(0x80000001, ALI_REG(codec, ALI_GLOBAL_CONTROL));
	outl(0x00000000, ALI_REG(codec, ALI_AINTEN));
	outl(0xffffffff, ALI_REG(codec, ALI_AINT));
	outl(0x00000000, ALI_REG(codec, ALI_VOLUME));
	outb(0x10, 	 ALI_REG(codec, ALI_MPUR2));

	codec->ac97_ext_id = snd_ali_codec_peek(codec, 0, AC97_EXTENDED_ID);
	codec->ac97_ext_status = snd_ali_codec_peek(codec, 0, AC97_EXTENDED_STATUS);
	if (codec->spdif_support) {
		snd_ali_enable_spdif_out(codec);
		codec->spdif_mask = 0x00000002;
	}

	snd_ali_printk("chip initialize succeed.\n");
	return 0;

}

static int __devinit snd_ali_resources(ali_t *codec)
{
	snd_ali_printk("resouces allocation ...\n");
	if ((codec->res_port = request_region(codec->port, 0x100, "ALI 5451")) == NULL) {
		snd_printk("Unalbe to request io ports.\n");
		return -EBUSY;
	}

	if (request_irq(codec->pci->irq, snd_ali_card_interrupt, SA_INTERRUPT|SA_SHIRQ, "ALI 5451", (void *)codec)) {
		snd_printk("Unable to request irq.\n");
		return -EBUSY;
	}
	codec->irq = codec->pci->irq;
	snd_ali_printk("resouces allocated.\n");
	return 0;
}
static int snd_ali_dev_free(snd_device_t *device) 
{
	ali_t *codec=snd_magic_cast(ali_t, device->device_data, return -ENXIO);
	snd_ali_free(codec);
	return 0;
}

static int __devinit snd_ali_create(snd_card_t * card,
				    struct pci_dev *pci,
				    int pcm_streams,
				    int spdif_support,
				    ali_t ** r_ali)
{
	ali_t *codec;
	int i, err;
	unsigned short cmdw = 0;
	struct pci_dev *pci_dev = NULL;
        static snd_device_ops_t ops = {
		(snd_dev_free_t *)snd_ali_dev_free,
		NULL,
		NULL
        };

	*r_ali = NULL;

	snd_ali_printk("creating ...\n");

	/* enable PCI device */
	if ((err = pci_enable_device(pci)) < 0)
		return err;
	/* check, if we can restrict PCI DMA transfers to 31 bits */
	if (pci_set_dma_mask(pci, 0x7fffffff) < 0 ||
	    pci_set_consistent_dma_mask(pci, 0x7fffffff) < 0) {
		snd_printk("architecture does not support 31bit PCI busmaster DMA\n");
		return -ENXIO;
	}

	if ((codec = snd_magic_kcalloc(ali_t, 0, GFP_KERNEL)) == NULL)
		return -ENOMEM;

	spin_lock_init(&codec->reg_lock);
	spin_lock_init(&codec->voice_alloc);

	codec->card = card;
	codec->pci = pci;
	codec->irq = -1;
	codec->port = pci_resource_start(pci, 0);
	pci_read_config_byte(pci, PCI_REVISION_ID, &codec->revision);
	codec->spdif_support = spdif_support;

	if (pcm_streams < 1)
		pcm_streams = 1;
	if (pcm_streams > 32)
		pcm_streams = 32;
	
	pci_set_master(pci);
	pci_read_config_word(pci, PCI_COMMAND, &cmdw);
	if ((cmdw & PCI_COMMAND_IO) != PCI_COMMAND_IO) {
		cmdw |= PCI_COMMAND_IO;
		pci_write_config_word(pci, PCI_COMMAND, cmdw);
	}
	pci_set_master(pci);
	
	if (snd_ali_resources(codec)) {
		snd_ali_free(codec);
		return -EBUSY;
	}

	synchronize_irq(pci->irq);

	codec->synth.chmap = 0;
	codec->synth.chcnt = 0;
	codec->spdif_mask = 0;
	codec->synth.synthcount = 0;

	if (codec->revision == ALI_5451_V02)
		codec->chregs.regs.ac97read = ALI_AC97_WRITE;
	else
		codec->chregs.regs.ac97read = ALI_AC97_READ;
	codec->chregs.regs.ac97write = ALI_AC97_WRITE;

	codec->chregs.regs.start  = ALI_START;
	codec->chregs.regs.stop   = ALI_STOP;
	codec->chregs.regs.aint   = ALI_AINT;
	codec->chregs.regs.ainten = ALI_AINTEN;

	codec->chregs.data.start  = 0x00;
	codec->chregs.data.stop   = 0x00;
	codec->chregs.data.aint   = 0x00;
	codec->chregs.data.ainten = 0x00;

	/* M1533: southbridge */
       	pci_dev = pci_find_device(0x10b9, 0x1533, NULL);
	codec->pci_m1533 = pci_dev;
	if (! codec->pci_m1533) {
		snd_printk(KERN_ERR "ali5451: cannot find ALi 1533 chip.\n");
		snd_ali_free(codec);
		return -ENODEV;
	}
	/* M7101: power management */
       	pci_dev = pci_find_device(0x10b9, 0x7101, NULL);
	codec->pci_m7101 = pci_dev;
	if (! codec->pci_m7101 && codec->revision == ALI_5451_V02) {
		snd_printk(KERN_ERR "ali5451: cannot find ALi 7101 chip.\n");
		snd_ali_free(codec);
		return -ENODEV;
	}

	snd_ali_printk("snd_device_new is called.\n");
	if ((err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, codec, &ops)) < 0) {
		snd_ali_free(codec);
		return err;
	}

	/* initialise synth voices*/
	for (i = 0; i < ALI_CHANNELS; i++ ) {
		codec->synth.voices[i].number = i;
	}

	if ((err = snd_ali_chip_init(codec)) < 0) {
		snd_printk("ali create: chip init error.\n");
		return err;
	}

#ifdef CONFIG_PM
	codec->image = kmalloc(sizeof(*codec->image), GFP_KERNEL);
	if (! codec->image)
		snd_printk(KERN_WARNING "can't allocate apm buffer\n");
	else
		snd_card_set_pm_callback(card, ali_suspend, ali_resume, codec);
#endif

	snd_ali_enable_address_interrupt(codec);
	codec->hw_initialized = 1;

	*r_ali = codec;
	snd_ali_printk("created.\n");
	return 0;
}

static int __devinit snd_ali_probe(struct pci_dev *pci,
				   const struct pci_device_id *pci_id)
{
	static int dev;
	snd_card_t *card;
	ali_t *codec;
	int err;

	snd_ali_printk("probe ...\n");

        if (dev >= SNDRV_CARDS)
                return -ENODEV;
	if (!enable[dev]) {
		dev++;
		return -ENOENT;
	}

	card = snd_card_new(index[dev], id[dev], THIS_MODULE, 0);
	if (card == NULL)
		return -ENOMEM;

	if ((err = snd_ali_create(card, pci, pcm_channels[dev], spdif[dev], &codec)) < 0) {
		snd_card_free(card);
		return err;
	}

	snd_ali_printk("mixer building ...\n");
	if ((err = snd_ali_mixer(codec)) < 0) {
		snd_card_free(card);
		return err;
	}
	
	snd_ali_printk("pcm building ...\n");
	if ((err = snd_ali_pcm(codec, 0, NULL)) < 0) {
		snd_card_free(card);
		return err;
	}

	strcpy(card->driver, "ALI5451");
	strcpy(card->shortname, "ALI 5451");
	
	sprintf(card->longname, "%s at 0x%lx, irq %li",
		card->shortname, codec->port, codec->irq);

	snd_ali_printk("register card.\n");
	if ((err = snd_card_register(card)) < 0) {
		snd_card_free(card);
		return err;
	}
	pci_set_drvdata(pci, card);
	dev++;
	return 0;
}

static void __devexit snd_ali_remove(struct pci_dev *pci)
{
	snd_card_free(pci_get_drvdata(pci));
	pci_set_drvdata(pci, NULL);
}

static struct pci_driver driver = {
	.name = "ALI 5451",
	.id_table = snd_ali_ids,
	.probe = snd_ali_probe,
	.remove = __devexit_p(snd_ali_remove),
	SND_PCI_PM_CALLBACKS
};                                

static int __init alsa_card_ali_init(void)
{
	return pci_module_init(&driver);
}

static void __exit alsa_card_ali_exit(void)
{
	pci_unregister_driver(&driver);
}

module_init(alsa_card_ali_init)
module_exit(alsa_card_ali_exit)
