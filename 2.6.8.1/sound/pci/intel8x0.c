/*
 *   ALSA driver for Intel ICH (i8x0) chipsets
 *
 *	Copyright (c) 2000 Jaroslav Kysela <perex@suse.cz>
 *
 *
 *   This code also contains alpha support for SiS 735 chipsets provided
 *   by Mike Pieper <mptei@users.sourceforge.net>. We have no datasheet
 *   for SiS735, so the code is not fully functional.
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
/* for 440MX workaround */
#include <asm/pgtable.h>
#include <asm/cacheflush.h>

MODULE_AUTHOR("Jaroslav Kysela <perex@suse.cz>");
MODULE_DESCRIPTION("Intel 82801AA,82901AB,i810,i820,i830,i840,i845,MX440; SiS 7012; Ali 5455");
MODULE_LICENSE("GPL");
MODULE_CLASSES("{sound}");
MODULE_DEVICES("{{Intel,82801AA-ICH},"
		"{Intel,82901AB-ICH0},"
		"{Intel,82801BA-ICH2},"
		"{Intel,82801CA-ICH3},"
		"{Intel,82801DB-ICH4},"
		"{Intel,ICH5},"
		"{Intel,ICH6},"
		"{Intel,6300ESB},"
		"{Intel,MX440},"
		"{SiS,SI7012},"
		"{NVidia,nForce Audio},"
		"{NVidia,nForce2 Audio},"
		"{AMD,AMD768},"
		"{AMD,AMD8111},"
	        "{ALI,M5455}}");

#if defined(CONFIG_GAMEPORT) || (defined(MODULE) && defined(CONFIG_GAMEPORT_MODULE))
#define SUPPORT_JOYSTICK 1
#endif
#define SUPPORT_MIDI 1

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;	/* Index 0-MAX */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;	/* ID for this card */
static int enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;	/* Enable this card */
static int ac97_clock[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = 0};
static int ac97_quirk[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = AC97_TUNE_DEFAULT};
#ifdef SUPPORT_JOYSTICK
static int joystick[SNDRV_CARDS];
#endif
#ifdef SUPPORT_MIDI
static int mpu_port[SNDRV_CARDS]; /* disabled */
#endif
static int boot_devs;

module_param_array(index, int, boot_devs, 0444);
MODULE_PARM_DESC(index, "Index value for Intel i8x0 soundcard.");
MODULE_PARM_SYNTAX(index, SNDRV_INDEX_DESC);
module_param_array(id, charp, boot_devs, 0444);
MODULE_PARM_DESC(id, "ID string for Intel i8x0 soundcard.");
MODULE_PARM_SYNTAX(id, SNDRV_ID_DESC);
module_param_array(enable, bool, boot_devs, 0444);
MODULE_PARM_DESC(enable, "Enable Intel i8x0 soundcard.");
MODULE_PARM_SYNTAX(enable, SNDRV_ENABLE_DESC);
module_param_array(ac97_clock, int, boot_devs, 0444);
MODULE_PARM_DESC(ac97_clock, "AC'97 codec clock (0 = auto-detect).");
MODULE_PARM_SYNTAX(ac97_clock, SNDRV_ENABLED ",default:0");
module_param_array(ac97_quirk, int, boot_devs, 0444);
MODULE_PARM_DESC(ac97_quirk, "AC'97 workaround for strange hardware.");
MODULE_PARM_SYNTAX(ac97_quirk, SNDRV_ENABLED ",allows:{{-1,4}},dialog:list,default:-1");
#ifdef SUPPORT_JOYSTICK
module_param_array(joystick, bool, boot_devs, 0444);
MODULE_PARM_DESC(joystick, "Enable joystick for Intel i8x0 soundcard.");
MODULE_PARM_SYNTAX(joystick, SNDRV_ENABLED "," SNDRV_BOOLEAN_FALSE_DESC);
#endif
#ifdef SUPPORT_MIDI
module_param_array(mpu_port, int, boot_devs, 0444);
MODULE_PARM_DESC(mpu_port, "MPU401 port # for Intel i8x0 driver.");
MODULE_PARM_SYNTAX(mpu_port, SNDRV_ENABLED ",allows:{{0},{0x330},{0x300}},dialog:list");
#endif

/*
 *  Direct registers
 */

#ifndef PCI_DEVICE_ID_INTEL_82801
#define PCI_DEVICE_ID_INTEL_82801       0x2415
#endif
#ifndef PCI_DEVICE_ID_INTEL_82901
#define PCI_DEVICE_ID_INTEL_82901       0x2425
#endif
#ifndef PCI_DEVICE_ID_INTEL_82801BA
#define PCI_DEVICE_ID_INTEL_82801BA     0x2445
#endif
#ifndef PCI_DEVICE_ID_INTEL_440MX
#define PCI_DEVICE_ID_INTEL_440MX       0x7195
#endif
#ifndef PCI_DEVICE_ID_INTEL_ICH3
#define PCI_DEVICE_ID_INTEL_ICH3	0x2485
#endif
#ifndef PCI_DEVICE_ID_INTEL_ICH4
#define PCI_DEVICE_ID_INTEL_ICH4	0x24c5
#endif
#ifndef PCI_DEVICE_ID_INTEL_ICH5
#define PCI_DEVICE_ID_INTEL_ICH5	0x24d5
#endif
#ifndef PCI_DEVICE_ID_INTEL_ESB_5
#define PCI_DEVICE_ID_INTEL_ESB_5	0x25a6
#endif
#ifndef PCI_DEVICE_ID_INTEL_ICH6_3
#define PCI_DEVICE_ID_INTEL_ICH6_3	0x266e
#endif
#ifndef PCI_DEVICE_ID_SI_7012
#define PCI_DEVICE_ID_SI_7012		0x7012
#endif
#ifndef PCI_DEVICE_ID_NVIDIA_MCP_AUDIO
#define PCI_DEVICE_ID_NVIDIA_MCP_AUDIO	0x01b1
#endif
#ifndef PCI_DEVICE_ID_NVIDIA_MCP2_AUDIO
#define PCI_DEVICE_ID_NVIDIA_MCP2_AUDIO	0x006a
#endif
#ifndef PCI_DEVICE_ID_NVIDIA_MCP3_AUDIO
#define PCI_DEVICE_ID_NVIDIA_MCP3_AUDIO	0x00da
#endif
#ifndef PCI_DEVICE_ID_NVIDIA_CK8S_AUDIO
#define PCI_DEVICE_ID_NVIDIA_CK8S_AUDIO	0x00ea
#endif

enum { DEVICE_INTEL, DEVICE_INTEL_ICH4, DEVICE_SIS, DEVICE_ALI, DEVICE_NFORCE };

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
DEFINE_REGSET(PI, 0x00);	/* PCM in */
DEFINE_REGSET(PO, 0x10);	/* PCM out */
DEFINE_REGSET(MC, 0x20);	/* Mic in */

/* ICH4 busmaster blocks */
DEFINE_REGSET(MC2, 0x40);	/* Mic in 2 */
DEFINE_REGSET(PI2, 0x50);	/* PCM in 2 */
DEFINE_REGSET(SP, 0x60);	/* SPDIF out */

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
#define ICH_REG_GLOB_CNT		0x2c	/* dword - global control */
#define   ICH_PCM_SPDIF_MASK	0xc0000000	/* s/pdif pcm slot mask (ICH4) */
#define   ICH_PCM_SPDIF_NONE	0x00000000	/* reserved - undefined */
#define   ICH_PCM_SPDIF_78	0x40000000	/* s/pdif pcm on slots 7&8 */
#define   ICH_PCM_SPDIF_69	0x80000000	/* s/pdif pcm on slots 6&9 */
#define   ICH_PCM_SPDIF_1011	0xc0000000	/* s/pdif pcm on slots 10&11 */
#define   ICH_PCM_20BIT		0x00400000	/* 20-bit samples (ICH4) */
#define   ICH_PCM_246_MASK	0x00300000	/* 6 channels (not all chips) */
#define   ICH_PCM_6		0x00200000	/* 6 channels (not all chips) */
#define   ICH_PCM_4		0x00100000	/* 4 channels (not all chips) */
#define   ICH_PCM_2		0x00000000	/* 2 channels (stereo) */
#define   ICH_SIS_PCM_246_MASK	0x000000c0	/* 6 channels (SIS7012) */
#define   ICH_SIS_PCM_6		0x00000080	/* 6 channels (SIS7012) */
#define   ICH_SIS_PCM_4		0x00000040	/* 4 channels (SIS7012) */
#define   ICH_SIS_PCM_2		0x00000000	/* 2 channels (SIS7012) */
#define   ICH_TRIE		0x00000040	/* tertiary resume interrupt enable */
#define   ICH_SRIE		0x00000020	/* secondary resume interrupt enable */
#define   ICH_PRIE		0x00000010	/* primary resume interrupt enable */
#define   ICH_ACLINK		0x00000008	/* AClink shut off */
#define   ICH_AC97WARM		0x00000004	/* AC'97 warm reset */
#define   ICH_AC97COLD		0x00000002	/* AC'97 cold reset */
#define   ICH_GIE		0x00000001	/* GPI interrupt enable */
#define ICH_REG_GLOB_STA		0x30	/* dword - global status */
#define   ICH_TRI		0x20000000	/* ICH4: tertiary (AC_SDIN2) resume interrupt */
#define   ICH_TCR		0x10000000	/* ICH4: tertiary (AC_SDIN2) codec ready */
#define   ICH_BCS		0x08000000	/* ICH4: bit clock stopped */
#define   ICH_SPINT		0x04000000	/* ICH4: S/PDIF interrupt */
#define   ICH_P2INT		0x02000000	/* ICH4: PCM2-In interrupt */
#define   ICH_M2INT		0x01000000	/* ICH4: Mic2-In interrupt */
#define   ICH_SAMPLE_CAP	0x00c00000	/* ICH4: sample capability bits (RO) */
#define   ICH_SAMPLE_16_20	0x00400000	/* ICH4: 16- and 20-bit samples */
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
#define ICH_REG_ACC_SEMA		0x34	/* byte - codec write semaphore */
#define   ICH_CAS		0x01		/* codec access semaphore */
#define ICH_REG_SDM		0x80
#define   ICH_DI2L_MASK		0x000000c0	/* PCM In 2, Mic In 2 data in line */
#define   ICH_DI2L_SHIFT	6
#define   ICH_DI1L_MASK		0x00000030	/* PCM In 1, Mic In 1 data in line */
#define   ICH_DI1L_SHIFT	4
#define   ICH_SE		0x00000008	/* steer enable */
#define   ICH_LDI_MASK		0x00000003	/* last codec read data input */

#define ICH_MAX_FRAGS		32		/* max hw frags */


/*
 * registers for Ali5455
 */

/* ALi 5455 busmaster blocks */
DEFINE_REGSET(AL_PI, 0x40);	/* ALi PCM in */
DEFINE_REGSET(AL_PO, 0x50);	/* Ali PCM out */
DEFINE_REGSET(AL_MC, 0x60);	/* Ali Mic in */
DEFINE_REGSET(AL_CDC_SPO, 0x70);	/* Ali Codec SPDIF out */
DEFINE_REGSET(AL_CENTER, 0x80);		/* Ali center out */
DEFINE_REGSET(AL_LFE, 0x90);		/* Ali center out */
DEFINE_REGSET(AL_CLR_SPI, 0xa0);	/* Ali Controller SPDIF in */
DEFINE_REGSET(AL_CLR_SPO, 0xb0);	/* Ali Controller SPDIF out */
DEFINE_REGSET(AL_I2S, 0xc0);	/* Ali I2S in */
DEFINE_REGSET(AL_PI2, 0xd0);	/* Ali PCM2 in */
DEFINE_REGSET(AL_MC2, 0xe0);	/* Ali Mic2 in */

enum {
	ICH_REG_ALI_SCR = 0x00,		/* System Control Register */
	ICH_REG_ALI_SSR = 0x04,		/* System Status Register  */
	ICH_REG_ALI_DMACR = 0x08,	/* DMA Control Register    */
	ICH_REG_ALI_FIFOCR1 = 0x0c,	/* FIFO Control Register 1  */
	ICH_REG_ALI_INTERFACECR = 0x10,	/* Interface Control Register */
	ICH_REG_ALI_INTERRUPTCR = 0x14,	/* Interrupt control Register */
	ICH_REG_ALI_INTERRUPTSR = 0x18,	/* Interrupt  Status Register */
	ICH_REG_ALI_FIFOCR2 = 0x1c,	/* FIFO Control Register 2   */
	ICH_REG_ALI_CPR = 0x20,		/* Command Port Register     */
	ICH_REG_ALI_CPR_ADDR = 0x22,	/* ac97 addr write */
	ICH_REG_ALI_SPR = 0x24,		/* Status Port Register      */
	ICH_REG_ALI_SPR_ADDR = 0x26,	/* ac97 addr read */
	ICH_REG_ALI_FIFOCR3 = 0x2c,	/* FIFO Control Register 3  */
	ICH_REG_ALI_TTSR = 0x30,	/* Transmit Tag Slot Register */
	ICH_REG_ALI_RTSR = 0x34,	/* Receive Tag Slot  Register */
	ICH_REG_ALI_CSPSR = 0x38,	/* Command/Status Port Status Register */
	ICH_REG_ALI_CAS = 0x3c,		/* Codec Write Semaphore Register */
	ICH_REG_ALI_HWVOL = 0xf0,	/* hardware volume control/status */
	ICH_REG_ALI_I2SCR = 0xf4,	/* I2S control/status */
	ICH_REG_ALI_SPDIFCSR = 0xf8,	/* spdif channel status register  */
	ICH_REG_ALI_SPDIFICS = 0xfc,	/* spdif interface control/status  */
};

#define ALI_CAS_SEM_BUSY	0x80000000
#define ALI_CPR_ADDR_SECONDARY	0x100
#define ALI_CPR_ADDR_READ	0x80
#define ALI_CSPSR_CODEC_READY	0x08
#define ALI_CSPSR_READ_OK	0x02
#define ALI_CSPSR_WRITE_OK	0x01

/* interrupts for the whole chip by interrupt status register finish */
 
#define ALI_INT_MICIN2		(1<<26)
#define ALI_INT_PCMIN2		(1<<25)
#define ALI_INT_I2SIN		(1<<24)
#define ALI_INT_SPDIFOUT	(1<<23)	/* controller spdif out INTERRUPT */
#define ALI_INT_SPDIFIN		(1<<22)
#define ALI_INT_LFEOUT		(1<<21)
#define ALI_INT_CENTEROUT	(1<<20)
#define ALI_INT_CODECSPDIFOUT	(1<<19)
#define ALI_INT_MICIN		(1<<18)
#define ALI_INT_PCMOUT		(1<<17)
#define ALI_INT_PCMIN		(1<<16)
#define ALI_INT_CPRAIS		(1<<7)	/* command port available */
#define ALI_INT_SPRAIS		(1<<5)	/* status port available */
#define ALI_INT_GPIO		(1<<1)
#define ALI_INT_MASK		(ALI_INT_SPDIFOUT|ALI_INT_CODECSPDIFOUT|ALI_INT_MICIN|ALI_INT_PCMOUT|ALI_INT_PCMIN)

#define ICH_ALI_SC_RESET	(1<<31)	/* master reset */
#define ICH_ALI_SC_AC97_DBL	(1<<30)
#define ICH_ALI_SC_CODEC_SPDF	(3<<20)	/* 1=7/8, 2=6/9, 3=10/11 */
#define ICH_ALI_SC_IN_BITS	(3<<18)
#define ICH_ALI_SC_OUT_BITS	(3<<16)
#define ICH_ALI_SC_6CH_CFG	(3<<14)
#define ICH_ALI_SC_PCM_4	(1<<8)
#define ICH_ALI_SC_PCM_6	(2<<8)
#define ICH_ALI_SC_PCM_246_MASK	(3<<8)

#define ICH_ALI_SS_SEC_ID	(3<<5)
#define ICH_ALI_SS_PRI_ID	(3<<3)

#define ICH_ALI_IF_AC97SP	(1<<21)
#define ICH_ALI_IF_MC		(1<<20)
#define ICH_ALI_IF_PI		(1<<19)
#define ICH_ALI_IF_MC2		(1<<18)
#define ICH_ALI_IF_PI2		(1<<17)
#define ICH_ALI_IF_LINE_SRC	(1<<15)	/* 0/1 = slot 3/6 */
#define ICH_ALI_IF_MIC_SRC	(1<<14)	/* 0/1 = slot 3/6 */
#define ICH_ALI_IF_SPDF_SRC	(3<<12)	/* 00 = PCM, 01 = AC97-in, 10 = spdif-in, 11 = i2s */
#define ICH_ALI_IF_AC97_OUT	(3<<8)	/* 00 = PCM, 10 = spdif-in, 11 = i2s */
#define ICH_ALI_IF_PO_SPDF	(1<<3)
#define ICH_ALI_IF_PO		(1<<1)

/*
 *  
 */

enum { ICHD_PCMIN, ICHD_PCMOUT, ICHD_MIC, ICHD_MIC2, ICHD_PCM2IN, ICHD_SPBAR, ICHD_LAST = ICHD_SPBAR };
enum { NVD_PCMIN, NVD_PCMOUT, NVD_MIC, NVD_SPBAR, NVD_LAST = NVD_SPBAR };
enum { ALID_PCMIN, ALID_PCMOUT, ALID_MIC, ALID_AC97SPDIFOUT, ALID_SPDIFIN, ALID_SPDIFOUT, ALID_LAST = ALID_SPDIFOUT };

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
	unsigned int pos_shift;
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
	struct ac97_pcm *pcm;
	int pcm_open_flag;
} ichdev_t;

typedef struct _snd_intel8x0 intel8x0_t;
#define chip_t intel8x0_t

struct _snd_intel8x0 {
	unsigned int device_type;
	char ac97_name[32];
	char ctrl_name[32];

	int irq;

	unsigned int mmio;
	unsigned long addr;
	unsigned long remap_addr;
	struct resource *res;
	unsigned int bm_mmio;
	unsigned long bmaddr;
	unsigned long remap_bmaddr;
	struct resource *res_bm;

	struct snd_dma_device dma_dev;

	struct pci_dev *pci;
	snd_card_t *card;

	int pcm_devs;
	snd_pcm_t *pcm[6];
	ichdev_t ichd[6];

	int multi4: 1,
	    multi6: 1,
	    smp20bit: 1;
	int in_ac97_init: 1,
	    in_sdin_init: 1;
	int fix_nocache: 1; /* workaround for 440MX */

	ac97_bus_t *ac97_bus;
	ac97_t *ac97[3];
	unsigned int ac97_sdin[3];

	snd_rawmidi_t *rmidi;

	spinlock_t reg_lock;
	spinlock_t ac97_lock;
	
	u32 bdbars_count;
	struct snd_dma_buffer bdbars;
	u32 int_sta_reg;		/* interrupt status register */
	u32 int_sta_mask;		/* interrupt status mask */

#ifdef CONFIG_PM
	u32 pci_state[64 / sizeof(u32)];
#endif
};

static struct pci_device_id snd_intel8x0_ids[] = {
	{ 0x8086, 0x2415, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL },	/* 82801AA */
	{ 0x8086, 0x2425, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL },	/* 82901AB */
	{ 0x8086, 0x2445, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL },	/* 82801BA */
	{ 0x8086, 0x2485, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL },	/* ICH3 */
	{ 0x8086, 0x24c5, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL_ICH4 }, /* ICH4 */
	{ 0x8086, 0x24d5, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL_ICH4 }, /* ICH5 */
	{ 0x8086, 0x25a6, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL_ICH4 }, /* ESB */
	{ 0x8086, 0x266e, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL_ICH4 }, /* ICH6 */
	{ 0x8086, 0x7195, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL },	/* 440MX */
	{ 0x1039, 0x7012, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_SIS },	/* SI7012 */
	{ 0x10de, 0x01b1, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_NFORCE },	/* NFORCE */
	{ 0x10de, 0x006a, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_NFORCE },	/* NFORCE2 */
	{ 0x10de, 0x00da, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_NFORCE },	/* NFORCE3 */
	{ 0x10de, 0x00ea, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_NFORCE },	/* CK8S */
	{ 0x1022, 0x746d, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL },	/* AMD8111 */
	{ 0x1022, 0x7445, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_INTEL },	/* AMD768 */
	{ 0x10b9, 0x5455, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DEVICE_ALI },   /* Ali5455 */
	{ 0, }
};

MODULE_DEVICE_TABLE(pci, snd_intel8x0_ids);

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

static void iputword(intel8x0_t *chip, u32 offset, u16 val)
{
	if (chip->bm_mmio)
		writew(val, chip->remap_bmaddr + offset);
	else
		outw(val, chip->bmaddr + offset);
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
 * access to AC97 codec via normal i/o (for ICH and SIS7012)
 */

/* return the GLOB_STA bit for the corresponding codec */
static unsigned int get_ich_codec_bit(intel8x0_t *chip, unsigned int codec)
{
	static unsigned int codec_bit[3] = {
		ICH_PCR, ICH_SCR, ICH_TCR
	};
	snd_assert(codec < 3, return ICH_PCR);
	if (chip->device_type == DEVICE_INTEL_ICH4)
		codec = chip->ac97_sdin[codec];
	return codec_bit[codec];
}

static int snd_intel8x0_codec_semaphore(intel8x0_t *chip, unsigned int codec)
{
	int time;
	
	if (codec > 2)
		return -EIO;
	if (chip->in_sdin_init) {
		/* we don't know the ready bit assignment at the moment */
		/* so we check any */
		codec = ICH_PCR | ICH_SCR | ICH_TCR;
	} else {
		codec = get_ich_codec_bit(chip, codec);
	}

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
	if (snd_intel8x0_codec_semaphore(chip, ac97->num) < 0) {
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
	if (snd_intel8x0_codec_semaphore(chip, ac97->num) < 0) {
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
 * access to AC97 for Ali5455
 */
static int snd_intel8x0_ali_codec_ready(intel8x0_t *chip, int mask)
{
	int count = 0;
	for (count = 0; count < 0x7f; count++) {
		int val = igetbyte(chip, ICHREG(ALI_CSPSR));
		if (val & mask)
			return 0;
	}
	snd_printd(KERN_WARNING "intel8x0: AC97 codec ready timeout.\n");
	return -EBUSY;
}

static int snd_intel8x0_ali_codec_semaphore(intel8x0_t *chip)
{
	int time = 100;
	while (time-- && (igetdword(chip, ICHREG(ALI_CAS)) & ALI_CAS_SEM_BUSY))
		udelay(1);
	if (! time)
		snd_printk(KERN_WARNING "ali_codec_semaphore timeout\n");
	return snd_intel8x0_ali_codec_ready(chip, ALI_CSPSR_CODEC_READY);
}

static unsigned short snd_intel8x0_ali_codec_read(ac97_t *ac97, unsigned short reg)
{
	intel8x0_t *chip = snd_magic_cast(intel8x0_t, ac97->private_data, return ~0);
	unsigned short data = 0xffff;

	spin_lock(&chip->ac97_lock);
	if (snd_intel8x0_ali_codec_semaphore(chip))
		goto __err;
	reg |= ALI_CPR_ADDR_READ;
	if (ac97->num)
		reg |= ALI_CPR_ADDR_SECONDARY;
	iputword(chip, ICHREG(ALI_CPR_ADDR), reg);
	if (snd_intel8x0_ali_codec_ready(chip, ALI_CSPSR_READ_OK))
		goto __err;
	data = igetword(chip, ICHREG(ALI_SPR));
 __err:
	spin_unlock(&chip->ac97_lock);
	return data;
}

static void snd_intel8x0_ali_codec_write(ac97_t *ac97, unsigned short reg, unsigned short val)
{
	intel8x0_t *chip = snd_magic_cast(intel8x0_t, ac97->private_data, return);

	spin_lock(&chip->ac97_lock);
	if (snd_intel8x0_ali_codec_semaphore(chip)) {
		spin_unlock(&chip->ac97_lock);
		return;
	}
	iputword(chip, ICHREG(ALI_CPR), val);
	if (ac97->num)
		reg |= ALI_CPR_ADDR_SECONDARY;
	iputword(chip, ICHREG(ALI_CPR_ADDR), reg);
	snd_intel8x0_ali_codec_ready(chip, ALI_CSPSR_WRITE_OK);
	spin_unlock(&chip->ac97_lock);
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
						     ichdev->fragsize1 >> ichdev->pos_shift);
			bdbar[idx + 2] = cpu_to_le32(ichdev->physbuf + (ichdev->size >> 1));
			bdbar[idx + 3] = cpu_to_le32(0x80000000 | /* interrupt on completion */
						     ichdev->fragsize1 >> ichdev->pos_shift);
		}
		ichdev->frags = 2;
	} else {
		ichdev->ack_reload = ichdev->ack = 1;
		ichdev->fragsize1 = ichdev->fragsize;
		for (idx = 0; idx < (ICH_REG_LVI_MASK + 1) * 2; idx += 2) {
			bdbar[idx + 0] = cpu_to_le32(ichdev->physbuf + (((idx >> 1) * ichdev->fragsize) % ichdev->size));
			bdbar[idx + 1] = cpu_to_le32(0x80000000 | /* interrupt on completion */
						     ichdev->fragsize >> ichdev->pos_shift);
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

#ifdef __i386__
/*
 * Intel 82443MX running a 100MHz processor system bus has a hardware bug,
 * which aborts PCI busmaster for audio transfer.  A workaround is to set
 * the pages as non-cached.  For details, see the errata in
 *	http://www.intel.com/design/chipsets/specupdt/245051.htm
 */
static void fill_nocache(void *buf, int size, int nocache)
{
	size = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	change_page_attr(virt_to_page(buf), size, nocache ? PAGE_KERNEL_NOCACHE : PAGE_KERNEL);
	global_flush_tlb();
}
#else
#define fill_nocache(buf,size,nocache)
#endif

/*
 *  Interrupt handler
 */

static inline void snd_intel8x0_update(intel8x0_t *chip, ichdev_t *ichdev)
{
	unsigned long port = ichdev->reg_offset;
	int status, civ, i, step;
	int ack = 0;

	status = igetbyte(chip, port + ichdev->roff_sr);
	civ = igetbyte(chip, port + ICH_REG_OFF_CIV);
	if (!(status & ICH_BCIS)) {
		step = 0;
	} else if (civ == ichdev->civ) {
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
	iputbyte(chip, port + ichdev->roff_sr,
		 status & (ICH_FIFOE | ICH_BCIS | ICH_LVBCI));
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
		if (status) {
			/* ack */
			iputdword(chip, chip->int_sta_reg, status);
			/* some Nforce[2] boards have problems when
			   IRQ_NONE is returned here.
			*/
			if (chip->device_type != DEVICE_NFORCE)
				status = 0;
		}
		spin_unlock(&chip->reg_lock);
		return IRQ_RETVAL(status);
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

static int snd_intel8x0_ali_trigger(snd_pcm_substream_t *substream, int cmd)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);
	ichdev_t *ichdev = get_ichdev(substream);
	unsigned long port = ichdev->reg_offset;
	static int fiforeg[] = { ICHREG(ALI_FIFOCR1), ICHREG(ALI_FIFOCR2), ICHREG(ALI_FIFOCR3) };
	unsigned int val, fifo;

	val = igetdword(chip, ICHREG(ALI_DMACR));
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			/* clear FIFO for synchronization of channels */
			fifo = igetdword(chip, fiforeg[ichdev->ali_slot / 4]);
			fifo &= ~(0xff << (ichdev->ali_slot % 4));  
			fifo |= 0x83 << (ichdev->ali_slot % 4); 
			iputdword(chip, fiforeg[ichdev->ali_slot / 4], fifo);
		}
		iputbyte(chip, port + ICH_REG_OFF_CR, ICH_IOCE);
		val &= ~(1 << (ichdev->ali_slot + 16)); /* clear PAUSE flag */
		iputdword(chip, ICHREG(ALI_DMACR), val | (1 << ichdev->ali_slot)); /* start DMA */
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		iputdword(chip, ICHREG(ALI_DMACR), val | (1 << (ichdev->ali_slot + 16))); /* pause */
		iputbyte(chip, port + ICH_REG_OFF_CR, 0);
		while (igetbyte(chip, port + ICH_REG_OFF_CR))
			;
		if (cmd == SNDRV_PCM_TRIGGER_PAUSE_PUSH)
			break;
		/* reset whole DMA things */
		iputbyte(chip, port + ICH_REG_OFF_CR, ICH_RESETREGS);
		/* clear interrupts */
		iputbyte(chip, port + ICH_REG_OFF_SR, igetbyte(chip, port + ICH_REG_OFF_SR) | 0x1e);
		iputdword(chip, ICHREG(ALI_INTERRUPTSR),
			  igetdword(chip, ICHREG(ALI_INTERRUPTSR)) & ichdev->int_sta_mask);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int snd_intel8x0_hw_params(snd_pcm_substream_t * substream,
				  snd_pcm_hw_params_t * hw_params)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);
	ichdev_t *ichdev = get_ichdev(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	size_t size = params_buffer_bytes(hw_params);
	int err;

	if (chip->fix_nocache && runtime->dma_area && runtime->dma_bytes < size)
		fill_nocache(runtime->dma_area, runtime->dma_bytes, 0); /* clear */
	err = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
	if (err < 0)
		return err;
	if (chip->fix_nocache && err > 0)
		fill_nocache(runtime->dma_area, runtime->dma_bytes, 1);
	if (ichdev->pcm_open_flag) {
		snd_ac97_pcm_close(ichdev->pcm);
		ichdev->pcm_open_flag = 0;
	}
	err = snd_ac97_pcm_open(ichdev->pcm, params_rate(hw_params),
				params_channels(hw_params),
				ichdev->pcm->r[0].slots);
	if (err >= 0) {
		ichdev->pcm_open_flag = 1;
		/* FIXME: hack to enable spdif support */
		if (ichdev->ichd == ICHD_PCMOUT && chip->device_type == DEVICE_SIS)
			snd_ac97_set_rate(ichdev->pcm->r[0].codec[0], AC97_SPDIF, params_rate(hw_params));
	}
	return err;
}

static int snd_intel8x0_hw_free(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);
	ichdev_t *ichdev = get_ichdev(substream);

	if (ichdev->pcm_open_flag) {
		snd_ac97_pcm_close(ichdev->pcm);
		ichdev->pcm_open_flag = 0;
	}
	if (chip->fix_nocache && substream->runtime->dma_area)
		fill_nocache(substream->runtime->dma_area, substream->runtime->dma_bytes, 0);
	return snd_pcm_lib_free_pages(substream);
}

static void snd_intel8x0_setup_pcm_out(intel8x0_t *chip,
				       int channels, int sample_bits)
{
	unsigned int cnt;
	switch (chip->device_type) {
	case DEVICE_ALI:
		cnt = igetdword(chip, ICHREG(ALI_SCR));
		cnt &= ~ICH_ALI_SC_PCM_246_MASK;
		if (chip->multi4 && channels == 4)
			cnt |= ICH_ALI_SC_PCM_4;
		else if (chip->multi6 && channels == 6)
			cnt |= ICH_ALI_SC_PCM_6;
		iputdword(chip, ICHREG(ALI_SCR), cnt);
		break;
	case DEVICE_SIS:
		cnt = igetdword(chip, ICHREG(GLOB_CNT));
		cnt &= ~ICH_SIS_PCM_246_MASK;
		if (chip->multi4 && channels == 4)
			cnt |= ICH_SIS_PCM_4;
		else if (chip->multi6 && channels == 6)
			cnt |= ICH_SIS_PCM_6;
		iputdword(chip, ICHREG(GLOB_CNT), cnt);
		break;
	default:
		cnt = igetdword(chip, ICHREG(GLOB_CNT));
		cnt &= ~(ICH_PCM_246_MASK | ICH_PCM_20BIT);
		if (chip->multi4 && channels == 4)
			cnt |= ICH_PCM_4;
		else if (chip->multi6 && channels == 6)
			cnt |= ICH_PCM_6;
		if (chip->device_type == DEVICE_NFORCE) {
			/* reset to 2ch once to keep the 6 channel data in alignment,
			 * to start from Front Left always
			 */
			iputdword(chip, ICHREG(GLOB_CNT), (cnt & 0xcfffff));
			mdelay(50); /* grrr... */
		} else if (chip->device_type == DEVICE_INTEL_ICH4) {
			if (sample_bits > 16)
				cnt |= ICH_PCM_20BIT;
		}
		iputdword(chip, ICHREG(GLOB_CNT), cnt);
		break;
	}
}

static int snd_intel8x0_pcm_prepare(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	ichdev_t *ichdev = get_ichdev(substream);

	ichdev->physbuf = runtime->dma_addr;
	ichdev->size = snd_pcm_lib_buffer_bytes(substream);
	ichdev->fragsize = snd_pcm_lib_period_bytes(substream);
	if (ichdev->ichd == ICHD_PCMOUT) {
		spin_lock(&chip->reg_lock);
		snd_intel8x0_setup_pcm_out(chip, runtime->channels,
					   runtime->sample_bits);
		spin_unlock(&chip->reg_lock);
		if (chip->device_type == DEVICE_INTEL_ICH4) {
			ichdev->pos_shift = (runtime->sample_bits > 16) ? 2 : 1;
		}
	}
	snd_intel8x0_setup_periods(chip, ichdev);
	return 0;
}

static snd_pcm_uframes_t snd_intel8x0_pcm_pointer(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);
	ichdev_t *ichdev = get_ichdev(substream);
	unsigned long flags;
	size_t ptr1, ptr;

	ptr1 = igetword(chip, ichdev->reg_offset + ichdev->roff_picb) << ichdev->pos_shift;
	if (ptr1 != 0)
		ptr = ichdev->fragsize1 - ptr1;
	else
		ptr = 0;
	spin_lock_irqsave(&chip->reg_lock, flags);
	ptr += ichdev->position;
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	if (ptr >= ichdev->size)
		return 0;
	return bytes_to_frames(substream->runtime, ptr);
}

static snd_pcm_hardware_t snd_intel8x0_stream =
{
	.info =			(SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_BLOCK_TRANSFER |
				 SNDRV_PCM_INFO_MMAP_VALID |
				 SNDRV_PCM_INFO_PAUSE |
				 SNDRV_PCM_INFO_RESUME),
	.formats =		SNDRV_PCM_FMTBIT_S16_LE,
	.rates =		SNDRV_PCM_RATE_48000,
	.rate_min =		48000,
	.rate_max =		48000,
	.channels_min =		2,
	.channels_max =		2,
	.buffer_bytes_max =	128 * 1024,
	.period_bytes_min =	32,
	.period_bytes_max =	128 * 1024,
	.periods_min =		1,
	.periods_max =		1024,
	.fifo_size =		0,
};

static unsigned int channels4[] = {
	2, 4,
};

#define CHANNELS4 sizeof(channels4) / sizeof(channels4[0])

static snd_pcm_hw_constraint_list_t hw_constraints_channels4 = {
	.count = CHANNELS4,
	.list = channels4,
	.mask = 0,
};

static unsigned int channels6[] = {
	2, 4, 6,
};

#define CHANNELS6 sizeof(channels6) / sizeof(channels6[0])

static snd_pcm_hw_constraint_list_t hw_constraints_channels6 = {
	.count = CHANNELS6,
	.list = channels6,
	.mask = 0,
};

static int snd_intel8x0_pcm_open(snd_pcm_substream_t * substream, ichdev_t *ichdev)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	int err;

	ichdev->substream = substream;
	runtime->hw = snd_intel8x0_stream;
	runtime->hw.rates = ichdev->pcm->rates;
	snd_pcm_limit_hw_rates(runtime);
	if (chip->device_type == DEVICE_SIS) {
		runtime->hw.buffer_bytes_max = 64*1024;
		runtime->hw.period_bytes_max = 64*1024;
	}
	if ((err = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS)) < 0)
		return err;
	runtime->private_data = ichdev;
	return 0;
}

static int snd_intel8x0_playback_open(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	int err;

	err = snd_intel8x0_pcm_open(substream, &chip->ichd[ICHD_PCMOUT]);
	if (err < 0)
		return err;

	if (chip->multi6) {
		runtime->hw.channels_max = 6;
		snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS, &hw_constraints_channels6);
	} else if (chip->multi4) {
		runtime->hw.channels_max = 4;
		snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS, &hw_constraints_channels4);
	}
	if (chip->smp20bit)
		runtime->hw.formats |= SNDRV_PCM_FMTBIT_S32_LE;
	return 0;
}

static int snd_intel8x0_playback_close(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);

	chip->ichd[ICHD_PCMOUT].substream = NULL;
	return 0;
}

static int snd_intel8x0_capture_open(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);

	return snd_intel8x0_pcm_open(substream, &chip->ichd[ICHD_PCMIN]);
}

static int snd_intel8x0_capture_close(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);

	chip->ichd[ICHD_PCMIN].substream = NULL;
	return 0;
}

static int snd_intel8x0_mic_open(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);

	return snd_intel8x0_pcm_open(substream, &chip->ichd[ICHD_MIC]);
}

static int snd_intel8x0_mic_close(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);

	chip->ichd[ICHD_MIC].substream = NULL;
	return 0;
}

static int snd_intel8x0_mic2_open(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);

	return snd_intel8x0_pcm_open(substream, &chip->ichd[ICHD_MIC2]);
}

static int snd_intel8x0_mic2_close(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);

	chip->ichd[ICHD_MIC2].substream = NULL;
	return 0;
}

static int snd_intel8x0_capture2_open(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);

	return snd_intel8x0_pcm_open(substream, &chip->ichd[ICHD_PCM2IN]);
}

static int snd_intel8x0_capture2_close(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);

	chip->ichd[ICHD_PCM2IN].substream = NULL;
	return 0;
}

static int snd_intel8x0_spdif_open(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);
	int idx = chip->device_type == DEVICE_NFORCE ? NVD_SPBAR : ICHD_SPBAR;

	return snd_intel8x0_pcm_open(substream, &chip->ichd[idx]);
}

static int snd_intel8x0_spdif_close(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);
	int idx = chip->device_type == DEVICE_NFORCE ? NVD_SPBAR : ICHD_SPBAR;

	chip->ichd[idx].substream = NULL;
	return 0;
}

static int snd_intel8x0_ali_ac97spdifout_open(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);
	unsigned long flags;
	unsigned int val;

	spin_lock_irqsave(&chip->reg_lock, flags);
	val = igetdword(chip, ICHREG(ALI_INTERFACECR));
	val |= ICH_ALI_IF_AC97SP;
	/* also needs to set ALI_SC_CODEC_SPDF correctly */
	spin_unlock_irqrestore(&chip->reg_lock, flags);

	return snd_intel8x0_pcm_open(substream, &chip->ichd[ALID_AC97SPDIFOUT]);
}

static int snd_intel8x0_ali_ac97spdifout_close(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);
	unsigned long flags;
	unsigned int val;

	chip->ichd[ALID_AC97SPDIFOUT].substream = NULL;
	spin_lock_irqsave(&chip->reg_lock, flags);
	val = igetdword(chip, ICHREG(ALI_INTERFACECR));
	val &= ~ICH_ALI_IF_AC97SP;
	spin_unlock_irqrestore(&chip->reg_lock, flags);

	return 0;
}

static int snd_intel8x0_ali_spdifin_open(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);

	return snd_intel8x0_pcm_open(substream, &chip->ichd[ALID_SPDIFIN]);
}

static int snd_intel8x0_ali_spdifin_close(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);

	chip->ichd[ALID_SPDIFIN].substream = NULL;
	return 0;
}

#if 0 // NYI
static int snd_intel8x0_ali_spdifout_open(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);

	return snd_intel8x0_pcm_open(substream, &chip->ichd[ALID_SPDIFOUT]);
}

static int snd_intel8x0_ali_spdifout_close(snd_pcm_substream_t * substream)
{
	intel8x0_t *chip = snd_pcm_substream_chip(substream);

	chip->ichd[ALID_SPDIFOUT].substream = NULL;
	return 0;
}
#endif

static snd_pcm_ops_t snd_intel8x0_playback_ops = {
	.open =		snd_intel8x0_playback_open,
	.close =	snd_intel8x0_playback_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_intel8x0_hw_params,
	.hw_free =	snd_intel8x0_hw_free,
	.prepare =	snd_intel8x0_pcm_prepare,
	.trigger =	snd_intel8x0_pcm_trigger,
	.pointer =	snd_intel8x0_pcm_pointer,
};

static snd_pcm_ops_t snd_intel8x0_capture_ops = {
	.open =		snd_intel8x0_capture_open,
	.close =	snd_intel8x0_capture_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_intel8x0_hw_params,
	.hw_free =	snd_intel8x0_hw_free,
	.prepare =	snd_intel8x0_pcm_prepare,
	.trigger =	snd_intel8x0_pcm_trigger,
	.pointer =	snd_intel8x0_pcm_pointer,
};

static snd_pcm_ops_t snd_intel8x0_capture_mic_ops = {
	.open =		snd_intel8x0_mic_open,
	.close =	snd_intel8x0_mic_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_intel8x0_hw_params,
	.hw_free =	snd_intel8x0_hw_free,
	.prepare =	snd_intel8x0_pcm_prepare,
	.trigger =	snd_intel8x0_pcm_trigger,
	.pointer =	snd_intel8x0_pcm_pointer,
};

static snd_pcm_ops_t snd_intel8x0_capture_mic2_ops = {
	.open =		snd_intel8x0_mic2_open,
	.close =	snd_intel8x0_mic2_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_intel8x0_hw_params,
	.hw_free =	snd_intel8x0_hw_free,
	.prepare =	snd_intel8x0_pcm_prepare,
	.trigger =	snd_intel8x0_pcm_trigger,
	.pointer =	snd_intel8x0_pcm_pointer,
};

static snd_pcm_ops_t snd_intel8x0_capture2_ops = {
	.open =		snd_intel8x0_capture2_open,
	.close =	snd_intel8x0_capture2_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_intel8x0_hw_params,
	.hw_free =	snd_intel8x0_hw_free,
	.prepare =	snd_intel8x0_pcm_prepare,
	.trigger =	snd_intel8x0_pcm_trigger,
	.pointer =	snd_intel8x0_pcm_pointer,
};

static snd_pcm_ops_t snd_intel8x0_spdif_ops = {
	.open =		snd_intel8x0_spdif_open,
	.close =	snd_intel8x0_spdif_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_intel8x0_hw_params,
	.hw_free =	snd_intel8x0_hw_free,
	.prepare =	snd_intel8x0_pcm_prepare,
	.trigger =	snd_intel8x0_pcm_trigger,
	.pointer =	snd_intel8x0_pcm_pointer,
};

static snd_pcm_ops_t snd_intel8x0_ali_playback_ops = {
	.open =		snd_intel8x0_playback_open,
	.close =	snd_intel8x0_playback_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_intel8x0_hw_params,
	.hw_free =	snd_intel8x0_hw_free,
	.prepare =	snd_intel8x0_pcm_prepare,
	.trigger =	snd_intel8x0_ali_trigger,
	.pointer =	snd_intel8x0_pcm_pointer,
};

static snd_pcm_ops_t snd_intel8x0_ali_capture_ops = {
	.open =		snd_intel8x0_capture_open,
	.close =	snd_intel8x0_capture_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_intel8x0_hw_params,
	.hw_free =	snd_intel8x0_hw_free,
	.prepare =	snd_intel8x0_pcm_prepare,
	.trigger =	snd_intel8x0_ali_trigger,
	.pointer =	snd_intel8x0_pcm_pointer,
};

static snd_pcm_ops_t snd_intel8x0_ali_capture_mic_ops = {
	.open =		snd_intel8x0_mic_open,
	.close =	snd_intel8x0_mic_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_intel8x0_hw_params,
	.hw_free =	snd_intel8x0_hw_free,
	.prepare =	snd_intel8x0_pcm_prepare,
	.trigger =	snd_intel8x0_ali_trigger,
	.pointer =	snd_intel8x0_pcm_pointer,
};

static snd_pcm_ops_t snd_intel8x0_ali_ac97spdifout_ops = {
	.open =		snd_intel8x0_ali_ac97spdifout_open,
	.close =	snd_intel8x0_ali_ac97spdifout_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_intel8x0_hw_params,
	.hw_free =	snd_intel8x0_hw_free,
	.prepare =	snd_intel8x0_pcm_prepare,
	.trigger =	snd_intel8x0_ali_trigger,
	.pointer =	snd_intel8x0_pcm_pointer,
};

static snd_pcm_ops_t snd_intel8x0_ali_spdifin_ops = {
	.open =		snd_intel8x0_ali_spdifin_open,
	.close =	snd_intel8x0_ali_spdifin_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_intel8x0_hw_params,
	.hw_free =	snd_intel8x0_hw_free,
	.prepare =	snd_intel8x0_pcm_prepare,
	.trigger =	snd_intel8x0_pcm_trigger,
	.pointer =	snd_intel8x0_pcm_pointer,
};

#if 0 // NYI
static snd_pcm_ops_t snd_intel8x0_ali_spdifout_ops = {
	.open =		snd_intel8x0_ali_spdifout_open,
	.close =	snd_intel8x0_ali_spdifout_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_intel8x0_hw_params,
	.hw_free =	snd_intel8x0_hw_free,
	.prepare =	snd_intel8x0_pcm_prepare,
	.trigger =	snd_intel8x0_pcm_trigger,
	.pointer =	snd_intel8x0_pcm_pointer,
};
#endif // NYI

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

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV, snd_dma_pci_data(chip->pci),
					      rec->prealloc_size, rec->prealloc_max_size);

	return 0;
}

static struct ich_pcm_table intel_pcms[] __devinitdata = {
	{
		.playback_ops = &snd_intel8x0_playback_ops,
		.capture_ops = &snd_intel8x0_capture_ops,
		.prealloc_size = 64 * 1024,
		.prealloc_max_size = 128 * 1024,
	},
	{
		.suffix = "MIC ADC",
		.capture_ops = &snd_intel8x0_capture_mic_ops,
		.prealloc_size = 0,
		.prealloc_max_size = 128 * 1024,
		.ac97_idx = ICHD_MIC,
	},
	{
		.suffix = "MIC2 ADC",
		.capture_ops = &snd_intel8x0_capture_mic2_ops,
		.prealloc_size = 0,
		.prealloc_max_size = 128 * 1024,
		.ac97_idx = ICHD_MIC2,
	},
	{
		.suffix = "ADC2",
		.capture_ops = &snd_intel8x0_capture2_ops,
		.prealloc_size = 0,
		.prealloc_max_size = 128 * 1024,
		.ac97_idx = ICHD_PCM2IN,
	},
	{
		.suffix = "IEC958",
		.playback_ops = &snd_intel8x0_spdif_ops,
		.prealloc_size = 64 * 1024,
		.prealloc_max_size = 128 * 1024,
		.ac97_idx = ICHD_SPBAR,
	},
};

static struct ich_pcm_table nforce_pcms[] __devinitdata = {
	{
		.playback_ops = &snd_intel8x0_playback_ops,
		.capture_ops = &snd_intel8x0_capture_ops,
		.prealloc_size = 64 * 1024,
		.prealloc_max_size = 128 * 1024,
	},
	{
		.suffix = "MIC ADC",
		.capture_ops = &snd_intel8x0_capture_mic_ops,
		.prealloc_size = 0,
		.prealloc_max_size = 128 * 1024,
		.ac97_idx = NVD_MIC,
	},
	{
		.suffix = "IEC958",
		.playback_ops = &snd_intel8x0_spdif_ops,
		.prealloc_size = 64 * 1024,
		.prealloc_max_size = 128 * 1024,
		.ac97_idx = NVD_SPBAR,
	},
};

static struct ich_pcm_table ali_pcms[] __devinitdata = {
	{
		.playback_ops = &snd_intel8x0_ali_playback_ops,
		.capture_ops = &snd_intel8x0_ali_capture_ops,
		.prealloc_size = 64 * 1024,
		.prealloc_max_size = 128 * 1024,
	},
	{
		.suffix = "MIC ADC",
		.capture_ops = &snd_intel8x0_ali_capture_mic_ops,
		.prealloc_size = 0,
		.prealloc_max_size = 128 * 1024,
		.ac97_idx = ALID_MIC,
	},
	{
		.suffix = "IEC958",
		.playback_ops = &snd_intel8x0_ali_ac97spdifout_ops,
		.capture_ops = &snd_intel8x0_ali_spdifin_ops,
		.prealloc_size = 64 * 1024,
		.prealloc_max_size = 128 * 1024,
		.ac97_idx = ALID_AC97SPDIFOUT,
	},
#if 0 // NYI
	{
		.suffix = "HW IEC958",
		.playback_ops = &snd_intel8x0_ali_spdifout_ops,
		.prealloc_size = 64 * 1024,
		.prealloc_max_size = 128 * 1024,
	},
#endif
};

static int __devinit snd_intel8x0_pcm(intel8x0_t *chip)
{
	int i, tblsize, device, err;
	struct ich_pcm_table *tbl, *rec;

	switch (chip->device_type) {
	case DEVICE_INTEL_ICH4:
		tbl = intel_pcms;
		tblsize = ARRAY_SIZE(intel_pcms);
		break;
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

	device = 0;
	for (i = 0; i < tblsize; i++) {
		rec = tbl + i;
		if (i > 0 && rec->ac97_idx) {
			/* activate PCM only when associated AC'97 codec */
			if (! chip->ichd[rec->ac97_idx].pcm)
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
	chip->ac97[ac97->num] = NULL;
}

static struct ac97_pcm ac97_pcm_defs[] __devinitdata = {
	/* front PCM */
	{
		.exclusive = 1,
		.r = {	{
				.slots = (1 << AC97_SLOT_PCM_LEFT) |
					 (1 << AC97_SLOT_PCM_RIGHT) |
					 (1 << AC97_SLOT_PCM_CENTER) |
					 (1 << AC97_SLOT_PCM_SLEFT) |
					 (1 << AC97_SLOT_PCM_SRIGHT) |
					 (1 << AC97_SLOT_LFE)
			}
		}
	},
	/* PCM IN #1 */
	{
		.stream = 1,
		.exclusive = 1,
		.r = {	{
				.slots = (1 << AC97_SLOT_PCM_LEFT) |
					 (1 << AC97_SLOT_PCM_RIGHT)
			}
		}
	},
	/* MIC IN #1 */
	{
		.stream = 1,
		.exclusive = 1,
		.r = {	{
				.slots = (1 << AC97_SLOT_MIC)
			}
		}
	},
	/* S/PDIF PCM */
	{
		.exclusive = 1,
		.spdif = 1,
		.r = {	{
				.slots = (1 << AC97_SLOT_SPDIF_LEFT2) |
					 (1 << AC97_SLOT_SPDIF_RIGHT2)
			}
		}
	},
	/* PCM IN #2 */
	{
		.stream = 1,
		.exclusive = 1,
		.r = {	{
				.slots = (1 << AC97_SLOT_PCM_LEFT) |
					 (1 << AC97_SLOT_PCM_RIGHT)
			}
		}
	},
	/* MIC IN #2 */
	{
		.stream = 1,
		.exclusive = 1,
		.r = {	{
				.slots = (1 << AC97_SLOT_MIC)
			}
		}
	},
};

static struct ac97_quirk ac97_quirks[] __devinitdata = {
	{
		.vendor = 0x0e11,
		.device = 0x00b8,
		.name = "Compaq Evo D510C",
		.type = AC97_TUNE_HP_ONLY
	},
	{
		.vendor = 0x1014,
		.device = 0x1f00,
		.name = "MS-9128",
		.type = AC97_TUNE_ALC_JACK
	},
	{
		.vendor = 0x1028,
		.device = 0x00d8,
		.name = "Dell Precision 530",	/* AD1885 */
		.type = AC97_TUNE_HP_ONLY
	},
	{
		.vendor = 0x1028,
		.device = 0x0126,
		.name = "Dell Optiplex GX260",	/* AD1981A */
		.type = AC97_TUNE_HP_ONLY
	},
	{	/* FIXME: which codec? */
		.vendor = 0x103c,
		.device = 0x00c3,
		.name = "Hewlett-Packard onboard",
		.type = AC97_TUNE_HP_ONLY
	},
 	{
		.vendor = 0x1043,
		.device = 0x80f3,
		.name = "ASUS ICH5/AD1985",
		.type = AC97_TUNE_AD_SHARING
	},
	{
		.vendor = 0x10f1,
		.device = 0x2665,
		.name = "Fujitsu-Siemens Celsius",	/* AD1981? */
		.type = AC97_TUNE_HP_ONLY
	},
	{
		.vendor = 0x10f1,
		.device = 0x2885,
		.name = "AMD64 Mobo",	/* ALC650 */
		.type = AC97_TUNE_HP_ONLY
	},
	{
		.vendor = 0x110a,
		.device = 0x0056,
		.name = "Fujitsu-Siemens Scenic",	/* AD1981? */
		.type = AC97_TUNE_HP_ONLY
	},
	{
		.vendor = 0x11d4,
		.device = 0x5375,
		.name = "ADI AD1985 (discrete)",
		.type = AC97_TUNE_HP_ONLY
	},
	{
		.vendor = 0x1462,
		.device = 0x5470,
		.name = "MSI P4 ATX 645 Ultra",
		.type = AC97_TUNE_HP_ONLY
	},
	{
		.vendor = 0x1734,
		.device = 0x0088,
		.name = "Fujitsu-Siemens D1522",	/* AD1981 */
		.type = AC97_TUNE_HP_ONLY
	},
	{
		.vendor = 0x8086,
		.device = 0x2000,
		.mask = 0xfff0,
		.name = "Intel ICH5/AD1985",
		.type = AC97_TUNE_AD_SHARING
	},
	{
		.vendor = 0x8086,
		.device = 0x4000,
		.mask = 0xfff0,
		.name = "Intel ICH5/AD1985",
		.type = AC97_TUNE_AD_SHARING
	},
	{
		.vendor = 0x8086,
		.device = 0x4856,
		.name = "Intel D845WN (82801BA)",
		.type = AC97_TUNE_SWAP_HP
	},
	{
		.vendor = 0x8086,
		.device = 0x4d44,
		.name = "Intel D850EMV2",	/* AD1885 */
		.type = AC97_TUNE_HP_ONLY
	},
	{
		.vendor = 0x8086,
		.device = 0x6000,
		.mask = 0xfff0,
		.name = "Intel ICH5/AD1985",
		.type = AC97_TUNE_AD_SHARING
	},
	{
		.vendor = 0x8086,
		.device = 0xe000,
		.mask = 0xfff0,
		.name = "Intel ICH5/AD1985",
		.type = AC97_TUNE_AD_SHARING
	},
#if 0 /* FIXME: this seems wrong on most boards */
	{
		.vendor = 0x8086,
		.device = 0xa000,
		.mask = 0xfff0,
		.name = "Intel ICH5/AD1985",
		.type = AC97_TUNE_HP_ONLY
	},
#endif
	{ } /* terminator */
};

static int __devinit snd_intel8x0_mixer(intel8x0_t *chip, int ac97_clock, int ac97_quirk)
{
	ac97_bus_t bus, *pbus;
	ac97_t ac97, *x97;
	int err;
	unsigned int i, codecs;
	unsigned int glob_sta = 0;
	int spdif_idx = -1; /* disabled */

	switch (chip->device_type) {
	case DEVICE_NFORCE:
		spdif_idx = NVD_SPBAR;
		break;
	case DEVICE_ALI:
		spdif_idx = ALID_AC97SPDIFOUT;
		break;
	default:
		if (chip->device_type == DEVICE_INTEL_ICH4)
			spdif_idx = ICHD_SPBAR;
		break;
	};

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
	ac97.scaps = AC97_SCAP_SKIP_MODEM;
	if (chip->device_type != DEVICE_ALI) {
		glob_sta = igetdword(chip, ICHREG(GLOB_STA));
		bus.write = snd_intel8x0_codec_write;
		bus.read = snd_intel8x0_codec_read;
		if (chip->device_type == DEVICE_INTEL_ICH4) {
			codecs = 0;
			if (glob_sta & ICH_PCR)
				codecs++;
			if (glob_sta & ICH_SCR)
				codecs++;
			if (glob_sta & ICH_TCR)
				codecs++;
			chip->in_sdin_init = 1;
			for (i = 0; i < codecs; i++) {
				ac97.num = i;
				snd_intel8x0_codec_read(&ac97, 0);
				chip->ac97_sdin[i] = igetbyte(chip, ICHREG(SDM)) & ICH_LDI_MASK;
			}
			ac97.num = 0;
			chip->in_sdin_init = 0;
		} else {
			codecs = glob_sta & ICH_SCR ? 2 : 1;
		}
		bus.vra = 1;
	} else {
		bus.write = snd_intel8x0_ali_codec_write;
		bus.read = snd_intel8x0_ali_codec_read;
		codecs = 1;
		/* detect the secondary codec */
		for (i = 0; i < 100; i++) {
			unsigned int reg = igetdword(chip, ICHREG(ALI_RTSR));
			if (reg & 0x40) {
				codecs = 2;
				break;
			}
			iputdword(chip, ICHREG(ALI_RTSR), reg | 0x40);
			udelay(1);
		}
		/* FIXME: my test board doens't work well with VRA... */
		bus.vra = 0;
	}
	if ((err = snd_ac97_bus(chip->card, &bus, &pbus)) < 0)
		goto __err;
	chip->ac97_bus = pbus;
	ac97.pci = chip->pci;
	for (i = 0; i < codecs; i++) {
		ac97.num = i;
		if ((err = snd_ac97_mixer(pbus, &ac97, &x97)) < 0) {
			if (err != -EACCES)
				snd_printk(KERN_ERR "Unable to initialize codec #%d\n", i);
			if (i == 0)
				goto __err;
			continue;
		}
		chip->ac97[i] = x97;
	}
	/* tune up the primary codec */
	snd_ac97_tune_hardware(chip->ac97[0], ac97_quirks, ac97_quirk);
	/* enable separate SDINs for ICH4 */
	if (chip->device_type == DEVICE_INTEL_ICH4)
		pbus->isdin = 1;
	/* find the available PCM streams */
	i = ARRAY_SIZE(ac97_pcm_defs);
	if (chip->device_type != DEVICE_INTEL_ICH4)
		i -= 2;		/* do not allocate PCM2IN and MIC2 */
	if (spdif_idx < 0)
		i--;		/* do not allocate S/PDIF */
	err = snd_ac97_pcm_assign(pbus, i, ac97_pcm_defs);
	if (err < 0)
		goto __err;
	chip->ichd[ICHD_PCMOUT].pcm = &pbus->pcms[0];
	chip->ichd[ICHD_PCMIN].pcm = &pbus->pcms[1];
	chip->ichd[ICHD_MIC].pcm = &pbus->pcms[2];
	if (spdif_idx >= 0)
		chip->ichd[spdif_idx].pcm = &pbus->pcms[3];
	if (chip->device_type == DEVICE_INTEL_ICH4) {
		chip->ichd[ICHD_PCM2IN].pcm = &pbus->pcms[4];
		chip->ichd[ICHD_MIC2].pcm = &pbus->pcms[5];
	}
	/* enable separate SDINs for ICH4 */
	if (chip->device_type == DEVICE_INTEL_ICH4) {
		struct ac97_pcm *pcm = chip->ichd[ICHD_PCM2IN].pcm;
		u8 tmp = igetbyte(chip, ICHREG(SDM));
		tmp &= ~(ICH_DI2L_MASK|ICH_DI1L_MASK);
		if (pcm) {
			tmp |= ICH_SE;	/* steer enable for multiple SDINs */
			tmp |= chip->ac97_sdin[0] << ICH_DI1L_SHIFT;
			for (i = 1; i < 4; i++) {
				if (pcm->r[0].codec[i]) {
					tmp |= chip->ac97_sdin[pcm->r[0].codec[1]->num] << ICH_DI2L_SHIFT;
					break;
				}
			}
		} else {
			tmp &= ~ICH_SE; /* steer disable */
		}
		iputbyte(chip, ICHREG(SDM), tmp);
	}
	if (pbus->pcms[0].r[0].slots & (1 << AC97_SLOT_PCM_SLEFT)) {
		chip->multi4 = 1;
		if (pbus->pcms[0].r[0].slots & (1 << AC97_SLOT_LFE))
			chip->multi6 = 1;
	}
	if (chip->device_type == DEVICE_INTEL_ICH4) {
		if ((igetdword(chip, ICHREG(GLOB_STA)) & ICH_SAMPLE_CAP) == ICH_SAMPLE_16_20)
			chip->smp20bit = 1;
	}
	if (chip->device_type == DEVICE_NFORCE) {
		/* 48kHz only */
		chip->ichd[spdif_idx].pcm->rates = SNDRV_PCM_RATE_48000;
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

static void do_ali_reset(intel8x0_t *chip)
{
	iputdword(chip, ICHREG(ALI_SCR), ICH_ALI_SC_RESET);
	iputdword(chip, ICHREG(ALI_FIFOCR1), 0x83838383);
	iputdword(chip, ICHREG(ALI_FIFOCR2), 0x83838383);
	iputdword(chip, ICHREG(ALI_FIFOCR3), 0x83838383);
	iputdword(chip, ICHREG(ALI_INTERFACECR),
		  ICH_ALI_IF_MC|ICH_ALI_IF_PI|ICH_ALI_IF_PO);
	iputdword(chip, ICHREG(ALI_INTERRUPTCR), 0x00000000);
	iputdword(chip, ICHREG(ALI_INTERRUPTSR), 0x00000000);
}

#define do_delay(chip) do {\
	set_current_state(TASK_UNINTERRUPTIBLE);\
	schedule_timeout(1);\
} while (0)

static int snd_intel8x0_ich_chip_init(intel8x0_t *chip, int probing)
{
	unsigned long end_time;
	unsigned int cnt, status, nstatus;
	
	/* put logic to right state */
	/* first clear status bits */
	status = ICH_RCS | ICH_MCINT | ICH_POINT | ICH_PIINT;
	if (chip->device_type == DEVICE_NFORCE)
		status |= ICH_NVSPINT;
	cnt = igetdword(chip, ICHREG(GLOB_STA));
	iputdword(chip, ICHREG(GLOB_STA), cnt & status);

	/* ACLink on, 2 channels */
	cnt = igetdword(chip, ICHREG(GLOB_CNT));
	cnt &= ~(ICH_ACLINK | ICH_PCM_246_MASK);
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

		if (chip->device_type == DEVICE_INTEL_ICH4)
			/* ICH4 can have three codecs */
			nstatus = ICH_PCR | ICH_SCR | ICH_TCR;
		else
			/* others up to two codecs */
			nstatus = ICH_PCR | ICH_SCR;

		/* wait for other codecs ready status. */
		end_time = jiffies + HZ / 4;
		while (status != nstatus && time_after_eq(end_time, jiffies)) {
			do_delay(chip);
			status |= igetdword(chip, ICHREG(GLOB_STA)) & nstatus;
		}

	} else {
		/* resume phase */
		int i;
		status = 0;
		for (i = 0; i < 3; i++)
			if (chip->ac97[i])
				status |= get_ich_codec_bit(chip, i);
		/* wait until all the probed codecs are ready */
		end_time = jiffies + HZ;
		do {
			nstatus = igetdword(chip, ICHREG(GLOB_STA)) & (ICH_PCR | ICH_SCR | ICH_TCR);
			if (status == nstatus)
				break;
			do_delay(chip);
		} while (time_after_eq(end_time, jiffies));
	}

	if (chip->device_type == DEVICE_SIS) {
		/* unmute the output on SIS7012 */
		iputword(chip, 0x4c, igetword(chip, 0x4c) | 1);
	}
	if (chip->device_type == DEVICE_NFORCE) {
		/* enable SPDIF interrupt */
		unsigned int val;
		pci_read_config_dword(chip->pci, 0x4c, &val);
		val |= 0x1000000;
		pci_write_config_dword(chip->pci, 0x4c, val);
	}
      	return 0;
}

static int snd_intel8x0_ali_chip_init(intel8x0_t *chip, int probing)
{
	u32 reg;
	int i = 0;

	reg = igetdword(chip, ICHREG(ALI_SCR));
	if ((reg & 2) == 0)	/* Cold required */
		reg |= 2;
	else
		reg |= 1;	/* Warm */
	reg &= ~0x80000000;	/* ACLink on */
	iputdword(chip, ICHREG(ALI_SCR), reg);

	for (i = 0; i < HZ / 2; i++) {
		if (! (igetdword(chip, ICHREG(ALI_INTERRUPTSR)) & ALI_INT_GPIO))
			goto __ok;
		do_delay(chip);
	}
	snd_printk(KERN_ERR "AC'97 reset failed.\n");
	if (probing)
		return -EIO;

 __ok:
	for (i = 0; i < HZ / 2; i++) {
		reg = igetdword(chip, ICHREG(ALI_RTSR));
		if (reg & 0x80) /* primary codec */
			break;
		iputdword(chip, ICHREG(ALI_RTSR), reg | 0x80);
		do_delay(chip);
	}

	do_ali_reset(chip);
	return 0;
}

static int snd_intel8x0_chip_init(intel8x0_t *chip, int probing)
{
	unsigned int i;
	int err;
	
	if (chip->device_type != DEVICE_ALI) {
		if ((err = snd_intel8x0_ich_chip_init(chip, probing)) < 0)
			return err;
		iagetword(chip, 0);	/* clear semaphore flag */
	} else {
		if ((err = snd_intel8x0_ali_chip_init(chip, probing)) < 0)
			return err;
	}

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
	if (chip->device_type == DEVICE_NFORCE) {
		/* stop the spdif interrupt */
		unsigned int val;
		pci_read_config_dword(chip->pci, 0x4c, &val);
		val &= ~0x1000000;
		pci_write_config_dword(chip->pci, 0x4c, val);
	}
	/* --- */
	synchronize_irq(chip->irq);
      __hw_end:
	if (chip->bdbars.area) {
		if (chip->fix_nocache)
			fill_nocache(chip->bdbars.area, chip->bdbars.bytes, 0);
		snd_dma_free_pages(&chip->dma_dev, &chip->bdbars);
	}
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
static int intel8x0_suspend(snd_card_t *card, unsigned int state)
{
	intel8x0_t *chip = snd_magic_cast(intel8x0_t, card->pm_private_data, return -EINVAL);
	int i;

	for (i = 0; i < chip->pcm_devs; i++)
		snd_pcm_suspend_all(chip->pcm[i]);
	for (i = 0; i < 3; i++)
		if (chip->ac97[i])
			snd_ac97_suspend(chip->ac97[i]);
	pci_save_state(chip->pci, chip->pci_state);
	snd_power_change_state(card, SNDRV_CTL_POWER_D3hot);
	return 0;
}

static int intel8x0_resume(snd_card_t *card, unsigned int state)
{
	intel8x0_t *chip = snd_magic_cast(intel8x0_t, card->pm_private_data, return -EINVAL);
	int i;

	pci_restore_state(chip->pci, chip->pci_state);
	pci_enable_device(chip->pci);
	pci_set_master(chip->pci);
	snd_intel8x0_chip_init(chip, 0);

	/* refill nocache */
	if (chip->fix_nocache)
		fill_nocache(chip->bdbars.area, chip->bdbars.bytes, 1);

	for (i = 0; i < 3; i++)
		if (chip->ac97[i])
			snd_ac97_resume(chip->ac97[i]);

	/* refill nocache */
	if (chip->fix_nocache) {
		for (i = 0; i < chip->bdbars_count; i++) {
			ichdev_t *ichdev = &chip->ichd[i];
			if (ichdev->substream) {
				snd_pcm_runtime_t *runtime = ichdev->substream->runtime;
				if (runtime->dma_area)
					fill_nocache(runtime->dma_area, runtime->dma_bytes, 1);
			}
		}
	}

	snd_power_change_state(card, SNDRV_CTL_POWER_D0);
	return 0;
}
#endif /* CONFIG_PM */

#define INTEL8X0_TESTBUF_SIZE	32768	/* enough large for one shot */

static void __devinit intel8x0_measure_ac97_clock(intel8x0_t *chip)
{
	snd_pcm_substream_t *subs;
	ichdev_t *ichdev;
	unsigned long port;
	unsigned long pos, t;
	unsigned long flags;
	struct timeval start_time, stop_time;

	if (chip->ac97_bus->clock != 48000)
		return; /* specified in module option */

	subs = chip->pcm[0]->streams[0].substream;
	if (! subs || subs->dma_buffer.bytes < INTEL8X0_TESTBUF_SIZE) {
		snd_printk("no playback buffer allocated - aborting measure ac97 clock\n");
		return;
	}
	ichdev = &chip->ichd[ICHD_PCMOUT];
	ichdev->physbuf = subs->dma_buffer.addr;
	ichdev->size = chip->ichd[ICHD_PCMOUT].fragsize = INTEL8X0_TESTBUF_SIZE;
	ichdev->substream = NULL; /* don't process interrupts */

	/* set rate */
	if (snd_ac97_set_rate(chip->ac97[0], AC97_PCM_FRONT_DAC_RATE, 48000) < 0) {
		snd_printk(KERN_ERR "cannot set ac97 rate: clock = %d\n", chip->ac97_bus->clock);
		return;
	}
	snd_intel8x0_setup_periods(chip, ichdev);
	port = ichdev->reg_offset;
	spin_lock_irqsave(&chip->reg_lock, flags);
	/* trigger */
	if (chip->device_type != DEVICE_ALI)
		iputbyte(chip, port + ICH_REG_OFF_CR, ICH_IOCE | ICH_STARTBM);
	else {
		iputbyte(chip, port + ICH_REG_OFF_CR, ICH_IOCE);
		iputdword(chip, ICHREG(ALI_DMACR), 1 << ichdev->ali_slot);
	}
	do_gettimeofday(&start_time);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
#if 0
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(HZ / 20);
#else
	/* FIXME: schedule() can take too long time and overlap the boundary.. */
	mdelay(50);
#endif
	spin_lock_irqsave(&chip->reg_lock, flags);
	/* check the position */
	pos = ichdev->fragsize1;
	pos -= igetword(chip, ichdev->reg_offset + ichdev->roff_picb) << ichdev->pos_shift;
	pos += ichdev->position;
	do_gettimeofday(&stop_time);
	/* stop */
	if (chip->device_type == DEVICE_ALI) {
		iputdword(chip, ICHREG(ALI_DMACR), 1 << (ichdev->ali_slot + 8));
		iputbyte(chip, port + ICH_REG_OFF_CR, 0);
		while (igetbyte(chip, port + ICH_REG_OFF_CR))
			;
	} else {
		iputbyte(chip, port + ICH_REG_OFF_CR, 0);
		while (!(igetbyte(chip, port + ichdev->roff_sr) & ICH_DCH))
			;
	}
	iputbyte(chip, port + ICH_REG_OFF_CR, ICH_RESETREGS);
	spin_unlock_irqrestore(&chip->reg_lock, flags);

	t = stop_time.tv_sec - start_time.tv_sec;
	t *= 1000000;
	t += stop_time.tv_usec - start_time.tv_usec;
	printk(KERN_INFO "%s: measured %lu usecs\n", __FUNCTION__, t);
	if (t == 0) {
		snd_printk(KERN_ERR "?? calculation error..\n");
		return;
	}
	pos = (pos / 4) * 1000;
	pos = (pos / t) * 1000 + ((pos % t) * 1000) / t;
	if (pos < 40000 || pos >= 60000) 
		/* abnormal value. hw problem? */
		printk(KERN_INFO "intel8x0: measured clock %ld rejected\n", pos);
	else if (pos < 47500 || pos > 48500)
		/* not 48000Hz, tuning the clock.. */
		chip->ac97_bus->clock = (chip->ac97_bus->clock * 48000) / pos;
	printk(KERN_INFO "intel8x0: clocking to %d\n", chip->ac97_bus->clock);
}

static void snd_intel8x0_proc_read(snd_info_entry_t * entry,
				   snd_info_buffer_t * buffer)
{
	intel8x0_t *chip = snd_magic_cast(intel8x0_t, entry->private_data, return);
	unsigned int tmp;

	snd_iprintf(buffer, "Intel8x0\n\n");
	if (chip->device_type == DEVICE_ALI)
		return;
	tmp = igetdword(chip, ICHREG(GLOB_STA));
	snd_iprintf(buffer, "Global control        : 0x%08x\n", igetdword(chip, ICHREG(GLOB_CNT)));
	snd_iprintf(buffer, "Global status         : 0x%08x\n", tmp);
	if (chip->device_type == DEVICE_INTEL_ICH4)
		snd_iprintf(buffer, "SDM                   : 0x%08x\n", igetdword(chip, ICHREG(SDM)));
	snd_iprintf(buffer, "AC'97 codecs ready    :%s%s%s%s\n",
			tmp & ICH_PCR ? " primary" : "",
			tmp & ICH_SCR ? " secondary" : "",
			tmp & ICH_TCR ? " tertiary" : "",
			(tmp & (ICH_PCR | ICH_SCR | ICH_TCR)) == 0 ? " none" : "");
	if (chip->device_type == DEVICE_INTEL_ICH4)
		snd_iprintf(buffer, "AC'97 codecs SDIN     : %i %i %i\n",
			chip->ac97_sdin[0],
			chip->ac97_sdin[1],
			chip->ac97_sdin[2]);
}

static void __devinit snd_intel8x0_proc_init(intel8x0_t * chip)
{
	snd_info_entry_t *entry;

	if (! snd_card_proc_new(chip->card, "intel8x0", &entry))
		snd_info_set_text_ops(entry, chip, 1024, snd_intel8x0_proc_read);
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

static int __devinit snd_intel8x0_create(snd_card_t * card,
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

	static unsigned int bdbars[] = {
		3, /* DEVICE_INTEL */
		6, /* DEVICE_INTEL_ICH4 */
		3, /* DEVICE_SIS */
		6, /* DEVICE_ALI */
		4, /* DEVICE_NFORCE */
	};
	static struct ich_reg_info intel_regs[6] = {
		{ ICH_PIINT, 0 },
		{ ICH_POINT, 0x10 },
		{ ICH_MCINT, 0x20 },
		{ ICH_M2INT, 0x40 },
		{ ICH_P2INT, 0x50 },
		{ ICH_SPINT, 0x60 },
	};
	static struct ich_reg_info nforce_regs[4] = {
		{ ICH_PIINT, 0 },
		{ ICH_POINT, 0x10 },
		{ ICH_MCINT, 0x20 },
		{ ICH_NVSPINT, 0x70 },
	};
	static struct ich_reg_info ali_regs[6] = {
		{ ALI_INT_PCMIN, 0x40 },
		{ ALI_INT_PCMOUT, 0x50 },
		{ ALI_INT_MICIN, 0x60 },
		{ ALI_INT_CODECSPDIFOUT, 0x70 },
		{ ALI_INT_SPDIFIN, 0xa0 },
		{ ALI_INT_SPDIFOUT, 0xb0 },
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
	snd_intel8x0_proc_init(chip);
	sprintf(chip->ac97_name, "%s - AC'97", card->shortname);
	sprintf(chip->ctrl_name, "%s - Controller", card->shortname);
	if (pci->vendor == PCI_VENDOR_ID_INTEL &&
	    pci->device == PCI_DEVICE_ID_INTEL_440MX)
		chip->fix_nocache = 1; /* enable workaround */
	if (device_type == DEVICE_ALI) {
		/* ALI5455 has no ac97 region */
		chip->bmaddr = pci_resource_start(pci, 0);
		if ((chip->res_bm = request_region(chip->bmaddr, 256, chip->ctrl_name)) == NULL) {
			snd_printk("unable to grab ports 0x%lx-0x%lx\n", chip->bmaddr, chip->bmaddr + 64 - 1);
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
		if ((chip->res_bm = request_region(chip->bmaddr, 64, chip->ctrl_name)) == NULL) {
			snd_printk("unable to grab ports 0x%lx-0x%lx\n", chip->bmaddr, chip->bmaddr + 64 - 1);
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

	chip->bdbars_count = bdbars[device_type];

	/* initialize offsets */
	switch (device_type) {
	case DEVICE_NFORCE:
		tbl = nforce_regs;
		break;
	case DEVICE_ALI:
		tbl = ali_regs;
		break;
	default:
		tbl = intel_regs;
		break;
	}
	for (i = 0; i < chip->bdbars_count; i++) {
		ichdev = &chip->ichd[i];
		ichdev->ichd = i;
		ichdev->reg_offset = tbl[i].offset;
		ichdev->int_sta_mask = tbl[i].int_sta_mask;
		if (device_type == DEVICE_SIS) {
			/* SiS 7012 swaps the registers */
			ichdev->roff_sr = ICH_REG_OFF_PICB;
			ichdev->roff_picb = ICH_REG_OFF_SR;
		} else {
			ichdev->roff_sr = ICH_REG_OFF_SR;
			ichdev->roff_picb = ICH_REG_OFF_PICB;
		}
		if (device_type == DEVICE_ALI)
			ichdev->ali_slot = (ichdev->reg_offset - 0x40) / 0x10;
		/* SIS7012 handles the pcm data in bytes, others are in samples */
		ichdev->pos_shift = (device_type == DEVICE_SIS) ? 0 : 1;
	}

	memset(&chip->dma_dev, 0, sizeof(chip->dma_dev));
	chip->dma_dev.type = SNDRV_DMA_TYPE_DEV;
	chip->dma_dev.dev = snd_dma_pci_data(pci);

	/* allocate buffer descriptor lists */
	/* the start of each lists must be aligned to 8 bytes */
	if (snd_dma_alloc_pages(&chip->dma_dev, chip->bdbars_count * sizeof(u32) * ICH_MAX_FRAGS * 2, &chip->bdbars) < 0) {
		snd_intel8x0_free(chip);
		snd_printk(KERN_ERR "intel8x0: cannot allocate buffer descriptors\n");
		return -ENOMEM;
	}
	/* tables must be aligned to 8 bytes here, but the kernel pages
	   are much bigger, so we don't care (on i386) */
	/* workaround for 440MX */
	if (chip->fix_nocache)
		fill_nocache(chip->bdbars.area, chip->bdbars.bytes, 1);
	int_sta_masks = 0;
	for (i = 0; i < chip->bdbars_count; i++) {
		ichdev = &chip->ichd[i];
		ichdev->bdbar = ((u32 *)chip->bdbars.area) + (i * ICH_MAX_FRAGS * 2);
		ichdev->bdbar_addr = chip->bdbars.addr + (i * sizeof(u32) * ICH_MAX_FRAGS * 2);
		int_sta_masks |= ichdev->int_sta_mask;
	}
	chip->int_sta_reg = device_type == DEVICE_ALI ? ICH_REG_ALI_INTERRUPTSR : ICH_REG_GLOB_STA;
	chip->int_sta_mask = int_sta_masks;

	if ((err = snd_intel8x0_chip_init(chip, 1)) < 0) {
		snd_intel8x0_free(chip);
		return err;
	}

	snd_card_set_pm_callback(card, intel8x0_suspend, intel8x0_resume, chip);

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
	{ PCI_DEVICE_ID_INTEL_82801, "Intel 82801AA-ICH" },
	{ PCI_DEVICE_ID_INTEL_82901, "Intel 82901AB-ICH0" },
	{ PCI_DEVICE_ID_INTEL_82801BA, "Intel 82801BA-ICH2" },
	{ PCI_DEVICE_ID_INTEL_440MX, "Intel 440MX" },
	{ PCI_DEVICE_ID_INTEL_ICH3, "Intel 82801CA-ICH3" },
	{ PCI_DEVICE_ID_INTEL_ICH4, "Intel 82801DB-ICH4" },
	{ PCI_DEVICE_ID_INTEL_ICH5, "Intel ICH5" },
	{ PCI_DEVICE_ID_INTEL_ESB_5, "Intel 6300ESB" },
	{ PCI_DEVICE_ID_INTEL_ICH6_3, "Intel ICH6" },
	{ PCI_DEVICE_ID_SI_7012, "SiS SI7012" },
	{ PCI_DEVICE_ID_NVIDIA_MCP_AUDIO, "NVidia nForce" },
	{ PCI_DEVICE_ID_NVIDIA_MCP2_AUDIO, "NVidia nForce2" },
	{ PCI_DEVICE_ID_NVIDIA_MCP3_AUDIO, "NVidia nForce3" },
	{ PCI_DEVICE_ID_NVIDIA_CK8S_AUDIO, "NVidia CK8S" },
	{ 0x746d, "AMD AMD8111" },
	{ 0x7445, "AMD AMD768" },
	{ 0x5455, "ALi M5455" },
	{ 0, NULL },
};

static int __devinit snd_intel8x0_probe(struct pci_dev *pci,
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

	if ((err = snd_intel8x0_create(card, pci, pci_id->driver_data, &chip)) < 0) {
		snd_card_free(card);
		return err;
	}

	if ((err = snd_intel8x0_mixer(chip, ac97_clock[dev], ac97_quirk[dev])) < 0) {
		snd_card_free(card);
		return err;
	}
	if ((err = snd_intel8x0_pcm(chip)) < 0) {
		snd_card_free(card);
		return err;
	}
	
	if (mpu_port[dev] == 0x300 || mpu_port[dev] == 0x330) {
		if ((err = snd_mpu401_uart_new(card, 0, MPU401_HW_INTEL8X0,
					       mpu_port[dev], 0,
					       -1, 0, &chip->rmidi)) < 0) {
			printk(KERN_ERR "intel8x0: no UART401 device at 0x%x, skipping.\n", mpu_port[dev]);
			mpu_port[dev] = 0;
		}
	} else
		mpu_port[dev] = 0;

	sprintf(card->longname, "%s at 0x%lx, irq %i",
		card->shortname, chip->addr, chip->irq);

	if (! ac97_clock[dev])
		intel8x0_measure_ac97_clock(chip);

	if ((err = snd_card_register(card)) < 0) {
		snd_card_free(card);
		return err;
	}
	pci_set_drvdata(pci, card);
	dev++;
	return 0;
}

static void __devexit snd_intel8x0_remove(struct pci_dev *pci)
{
	snd_card_free(pci_get_drvdata(pci));
	pci_set_drvdata(pci, NULL);
}

static struct pci_driver driver = {
	.name = "Intel ICH",
	.id_table = snd_intel8x0_ids,
	.probe = snd_intel8x0_probe,
	.remove = __devexit_p(snd_intel8x0_remove),
	SND_PCI_PM_CALLBACKS
};


#if defined(SUPPORT_JOYSTICK) || defined(SUPPORT_MIDI)
/*
 * initialize joystick/midi addresses
 */

#ifdef SUPPORT_JOYSTICK
/* there is only one available device, so we keep it here */
static struct pci_dev *ich_gameport_pci;
static struct gameport ich_gameport = { .io = 0x200 };
#endif

static int __devinit snd_intel8x0_joystick_probe(struct pci_dev *pci,
						 const struct pci_device_id *id)
{
	u16 val;
	static int dev;
	if (dev >= SNDRV_CARDS)
		return -ENODEV;
	if (!enable[dev]) {
		dev++;
		return -ENOENT;
	}

	pci_read_config_word(pci, 0xe6, &val);
#ifdef SUPPORT_JOYSTICK
	val &= ~0x100;
	if (joystick[dev]) {
		if (! request_region(ich_gameport.io, 8, "ICH gameport")) {
			printk(KERN_WARNING "intel8x0: cannot grab gameport 0x%x\n",  ich_gameport.io);
			joystick[dev] = 0;
		} else {
			ich_gameport_pci = pci;
			gameport_register_port(&ich_gameport);
			val |= 0x100;
		}
	}
#endif
#ifdef SUPPORT_MIDI
	val &= ~0x20;
	if (mpu_port[dev] > 0) {
		if (mpu_port[dev] == 0x300 || mpu_port[dev] == 0x330) {
			u8 b;
			val |= 0x20;
			pci_read_config_byte(pci, 0xe2, &b);
			if (mpu_port[dev] == 0x300)
				b |= 0x08;
			else
				b &= ~0x08;
			pci_write_config_byte(pci, 0xe2, b);
		}
	}
#endif
	pci_write_config_word(pci, 0xe6, val);
	return 0;
}

static void __devexit snd_intel8x0_joystick_remove(struct pci_dev *pci)
{
	u16 val;
#ifdef SUPPORT_JOYSTICK
	if (ich_gameport_pci == pci) {
		gameport_unregister_port(&ich_gameport);
		release_region(ich_gameport.io, 8);
		ich_gameport_pci = NULL;
	}
#endif
	/* disable joystick and MIDI */
	pci_read_config_word(pci, 0xe6, &val);
	val &= ~0x120;
	pci_write_config_word(pci, 0xe6, val);
}

static struct pci_device_id snd_intel8x0_joystick_ids[] = {
	{ 0x8086, 0x2410, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },	/* 82801AA */
	{ 0x8086, 0x2420, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },	/* 82901AB */
	{ 0x8086, 0x2440, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 }, /* ICH2 */
	{ 0x8086, 0x244c, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 }, /* ICH2M */
	{ 0x8086, 0x248c, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },	/* ICH3 */
	// { 0x8086, 0x7195, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },	/* 440MX */
	// { 0x1039, 0x7012, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },	/* SI7012 */
	{ 0x10de, 0x01b2, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },	/* NFORCE */
	{ 0x10de, 0x006b, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },	/* NFORCE2 */
	{ 0x10de, 0x00db, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },	/* NFORCE3 */
	{ 0, }
};

static struct pci_driver joystick_driver = {
	.name = "Intel ICH Joystick",
	.id_table = snd_intel8x0_joystick_ids,
	.probe = snd_intel8x0_joystick_probe,
	.remove = __devexit_p(snd_intel8x0_joystick_remove),
};

static int have_joystick;
#endif

static int __init alsa_card_intel8x0_init(void)
{
	int err;

        if ((err = pci_module_init(&driver)) < 0)
                return err;

#if defined(SUPPORT_JOYSTICK) || defined(SUPPORT_MIDI)
	if (pci_module_init(&joystick_driver) < 0) {
		snd_printdd(KERN_INFO "no joystick found\n");
		have_joystick = 0;
	} else {
		snd_printdd(KERN_INFO "joystick(s) found\n");
		have_joystick = 1;
	}
#endif
        return 0;

}

static void __exit alsa_card_intel8x0_exit(void)
{
	pci_unregister_driver(&driver);
#if defined(SUPPORT_JOYSTICK) || defined(SUPPORT_MIDI)
	if (have_joystick)
		pci_unregister_driver(&joystick_driver);
#endif
}

module_init(alsa_card_intel8x0_init)
module_exit(alsa_card_intel8x0_exit)
