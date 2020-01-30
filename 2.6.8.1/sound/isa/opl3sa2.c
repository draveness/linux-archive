/*
 *  Driver for Yamaha OPL3-SA[2,3] soundcards
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
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
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/pnp.h>
#include <linux/moduleparam.h>
#include <sound/core.h>
#include <sound/cs4231.h>
#include <sound/mpu401.h>
#include <sound/opl3.h>
#include <sound/initval.h>

#include <asm/io.h>

MODULE_AUTHOR("Jaroslav Kysela <perex@suse.cz>");
MODULE_DESCRIPTION("Yamaha OPL3SA2+");
MODULE_LICENSE("GPL");
MODULE_CLASSES("{sound}");
MODULE_DEVICES("{{Yamaha,YMF719E-S},"
		"{Genius,Sound Maker 3DX},"
		"{Yamaha,OPL3SA3},"
		"{Intel,AL440LX sound},"
	        "{NeoMagic,MagicWave 3DX}}");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;	/* Index 0-MAX */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;	/* ID for this card */
static int enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_ISAPNP; /* Enable this card */
#ifdef CONFIG_PNP
static int isapnp[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = 1};
#endif
static long port[SNDRV_CARDS] = SNDRV_DEFAULT_PORT;	/* 0xf86,0x370,0x100 */
static long sb_port[SNDRV_CARDS] = SNDRV_DEFAULT_PORT;	/* 0x220,0x240,0x260 */
static long wss_port[SNDRV_CARDS] = SNDRV_DEFAULT_PORT;/* 0x530,0xe80,0xf40,0x604 */
static long fm_port[SNDRV_CARDS] = SNDRV_DEFAULT_PORT;	/* 0x388 */
static long midi_port[SNDRV_CARDS] = SNDRV_DEFAULT_PORT;/* 0x330,0x300 */
static int irq[SNDRV_CARDS] = SNDRV_DEFAULT_IRQ;	/* 0,1,3,5,9,11,12,15 */
static int dma1[SNDRV_CARDS] = SNDRV_DEFAULT_DMA;	/* 1,3,5,6,7 */
static int dma2[SNDRV_CARDS] = SNDRV_DEFAULT_DMA;	/* 1,3,5,6,7 */
static int opl3sa3_ymode[SNDRV_CARDS] = { [0 ... (SNDRV_CARDS-1)] = 0 };   /* 0,1,2,3 */ /*SL Added*/
static int boot_devs;

module_param_array(index, int, boot_devs, 0444);
MODULE_PARM_DESC(index, "Index value for OPL3-SA soundcard.");
MODULE_PARM_SYNTAX(index, SNDRV_INDEX_DESC);
module_param_array(id, charp, boot_devs, 0444);
MODULE_PARM_DESC(id, "ID string for OPL3-SA soundcard.");
MODULE_PARM_SYNTAX(id, SNDRV_ID_DESC);
module_param_array(enable, bool, boot_devs, 0444);
MODULE_PARM_DESC(enable, "Enable OPL3-SA soundcard.");
MODULE_PARM_SYNTAX(enable, SNDRV_ENABLE_DESC);
#ifdef CONFIG_PNP
module_param_array(isapnp, bool, boot_devs, 0444);
MODULE_PARM_DESC(isapnp, "PnP detection for specified soundcard.");
MODULE_PARM_SYNTAX(isapnp, SNDRV_ISAPNP_DESC);
#endif
module_param_array(port, long, boot_devs, 0444);
MODULE_PARM_DESC(port, "Port # for OPL3-SA driver.");
MODULE_PARM_SYNTAX(port, SNDRV_ENABLED ",allows:{{0xf86},{0x370},{0x100}},dialog:list");
module_param_array(sb_port, long, boot_devs, 0444);
MODULE_PARM_DESC(sb_port, "SB port # for OPL3-SA driver.");
MODULE_PARM_SYNTAX(sb_port, SNDRV_ENABLED ",allows:{{0x220},{0x240},{0x260}},dialog:list");
module_param_array(wss_port, long, boot_devs, 0444);
MODULE_PARM_DESC(wss_port, "WSS port # for OPL3-SA driver.");
MODULE_PARM_SYNTAX(wss_port, SNDRV_ENABLED ",allows:{{0x530},{0xe80},{0xf40},{0x604}},dialog:list");
module_param_array(fm_port, long, boot_devs, 0444);
MODULE_PARM_DESC(fm_port, "FM port # for OPL3-SA driver.");
MODULE_PARM_SYNTAX(fm_port, SNDRV_ENABLED ",allows:{{0x388}},dialog:list");
module_param_array(midi_port, long, boot_devs, 0444);
MODULE_PARM_DESC(midi_port, "MIDI port # for OPL3-SA driver.");
MODULE_PARM_SYNTAX(midi_port, SNDRV_ENABLED ",allows:{{0x330},{0x300}},dialog:list");
module_param_array(irq, int, boot_devs, 0444);
MODULE_PARM_DESC(irq, "IRQ # for OPL3-SA driver.");
MODULE_PARM_SYNTAX(irq, SNDRV_ENABLED ",allows:{{0},{1},{3},{5},{9},{11},{12},{15}},dialog:list");
module_param_array(dma1, int, boot_devs, 0444);
MODULE_PARM_DESC(dma1, "DMA1 # for OPL3-SA driver.");
MODULE_PARM_SYNTAX(dma1, SNDRV_ENABLED ",allows:{{1},{3},{5},{6},{7}},dialog:list");
module_param_array(dma2, int, boot_devs, 0444);
MODULE_PARM_DESC(dma2, "DMA2 # for OPL3-SA driver.");
MODULE_PARM_SYNTAX(dma2, SNDRV_ENABLED ",allows:{{1},{3},{5},{6},{7}},dialog:list");
module_param_array(opl3sa3_ymode, int, boot_devs, 0444);
MODULE_PARM_DESC(opl3sa3_ymode, "Speaker size selection for 3D Enhancement mode: Desktop/Large Notebook/Small Notebook/HiFi.");
MODULE_PARM_SYNTAX(opl3sa3_ymode, SNDRV_ENABLED ",allows:{{0,3}},dialog:list");  /* SL Added */

/* control ports */
#define OPL3SA2_PM_CTRL		0x01
#define OPL3SA2_SYS_CTRL		0x02
#define OPL3SA2_IRQ_CONFIG	0x03
#define OPL3SA2_IRQ_STATUS	0x04
#define OPL3SA2_DMA_CONFIG	0x06
#define OPL3SA2_MASTER_LEFT	0x07
#define OPL3SA2_MASTER_RIGHT	0x08
#define OPL3SA2_MIC		0x09
#define OPL3SA2_MISC		0x0A

/* opl3sa3 only */
#define OPL3SA3_DGTL_DOWN	0x12
#define OPL3SA3_ANLG_DOWN	0x13
#define OPL3SA3_WIDE		0x14
#define OPL3SA3_BASS		0x15
#define OPL3SA3_TREBLE		0x16

/* power management bits */
#define OPL3SA2_PM_ADOWN		0x20
#define OPL3SA2_PM_PSV		0x04		
#define OPL3SA2_PM_PDN		0x02
#define OPL3SA2_PM_PDX		0x01

#define OPL3SA2_PM_D0	0x00
#define OPL3SA2_PM_D3	(OPL3SA2_PM_ADOWN|OPL3SA2_PM_PSV|OPL3SA2_PM_PDN|OPL3SA2_PM_PDX)

typedef struct snd_opl3sa2 opl3sa2_t;
#define chip_t opl3sa2_t

struct snd_opl3sa2 {
	snd_card_t *card;
	int version;		/* 2 or 3 */
	unsigned long port;	/* control port */
	struct resource *res_port; /* control port resource */
	int irq;
	int single_dma;
	spinlock_t reg_lock;
	snd_hwdep_t *synth;
	snd_rawmidi_t *rmidi;
	cs4231_t *cs4231;
#ifdef CONFIG_PNP
	struct pnp_dev *dev;
#endif
	unsigned char ctlregs[0x20];
	int ymode;		/* SL added */
	snd_kcontrol_t *master_switch;
	snd_kcontrol_t *master_volume;
#ifdef CONFIG_PM
	void (*cs4231_suspend)(cs4231_t *);
	void (*cs4231_resume)(cs4231_t *);
#endif
};

static snd_card_t *snd_opl3sa2_legacy[SNDRV_CARDS] = SNDRV_DEFAULT_PTR;

#ifdef CONFIG_PNP

static struct pnp_card_device_id snd_opl3sa2_pnpids[] = {
	/* Yamaha YMF719E-S (Genius Sound Maker 3DX) */
	{ .id = "YMH0020", .devs = { { "YMH0021" } } },
	/* Yamaha OPL3-SA3 (integrated on Intel's Pentium II AL440LX motherboard) */
	{ .id = "YMH0030", .devs = { { "YMH0021" } } },
	/* Yamaha OPL3-SA2 */
	{ .id = "YMH0800", .devs = { { "YMH0021" } } },
	/* Yamaha OPL3-SA2 */
	{ .id = "YMH0801", .devs = { { "YMH0021" } } },
	/* NeoMagic MagicWave 3DX */
	{ .id = "NMX2200", .devs = { { "YMH2210" } } },
	/* --- */
	{ .id = "" }	/* end */
};

MODULE_DEVICE_TABLE(pnp_card, snd_opl3sa2_pnpids);

#endif /* CONFIG_PNP */


/* read control port (w/o spinlock) */
static unsigned char __snd_opl3sa2_read(opl3sa2_t *chip, unsigned char reg)
{
	unsigned char result;
#if 0
	outb(0x1d, port);	/* password */
	printk("read [0x%lx] = 0x%x\n", port, inb(port));
#endif
	outb(reg, chip->port);	/* register */
	result = inb(chip->port + 1);
#if 0
	printk("read [0x%lx] = 0x%x [0x%x]\n", port, result, inb(port));
#endif
	return result;
}

/* read control port (with spinlock) */
static unsigned char snd_opl3sa2_read(opl3sa2_t *chip, unsigned char reg)
{
	unsigned long flags;
	unsigned char result;

	spin_lock_irqsave(&chip->reg_lock, flags);
	result = __snd_opl3sa2_read(chip, reg);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return result;
}

/* write control port (w/o spinlock) */
static void __snd_opl3sa2_write(opl3sa2_t *chip, unsigned char reg, unsigned char value)
{
#if 0
	outb(0x1d, port);	/* password */
#endif
	outb(reg, chip->port);	/* register */
	outb(value, chip->port + 1);
	chip->ctlregs[reg] = value;
}

/* write control port (with spinlock) */
static void snd_opl3sa2_write(opl3sa2_t *chip, unsigned char reg, unsigned char value)
{
	unsigned long flags;
	spin_lock_irqsave(&chip->reg_lock, flags);
	__snd_opl3sa2_write(chip, reg, value);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

static int __init snd_opl3sa2_detect(opl3sa2_t *chip)
{
	snd_card_t *card;
	unsigned long port;
	unsigned char tmp, tmp1;
	char str[2];

	card = chip->card;
	port = chip->port;
	if ((chip->res_port = request_region(port, 2, "OPL3-SA control")) == NULL) {
		snd_printk(KERN_ERR "opl3sa2: can't grab port 0x%lx\n", port);
		return -EBUSY;
	}
	// snd_printk("REG 0A = 0x%x\n", snd_opl3sa2_read(chip, 0x0a));
	chip->version = 0;
	tmp = snd_opl3sa2_read(chip, OPL3SA2_MISC);
	if (tmp == 0xff) {
		snd_printd("OPL3-SA [0x%lx] detect = 0x%x\n", port, tmp);
		return -ENODEV;
	}
	switch (tmp & 0x07) {
	case 0x01:
		chip->version = 2; /* YMF711 */
		break;
	default:
		chip->version = 3;
		/* 0x02 - standard */
		/* 0x03 - YM715B */
		/* 0x04 - YM719 - OPL-SA4? */
		/* 0x05 - OPL3-SA3 - Libretto 100 */
		break;
	}
	str[0] = chip->version + '0';
	str[1] = 0;
	strcat(card->shortname, str);
	snd_opl3sa2_write(chip, OPL3SA2_MISC, tmp ^ 7);
	if ((tmp1 = snd_opl3sa2_read(chip, OPL3SA2_MISC)) != tmp) {
		snd_printd("OPL3-SA [0x%lx] detect (1) = 0x%x (0x%x)\n", port, tmp, tmp1);
		return -ENODEV;
	}
	/* try if the MIC register is accesible */
	tmp = snd_opl3sa2_read(chip, OPL3SA2_MIC);
	snd_opl3sa2_write(chip, OPL3SA2_MIC, 0x8a);
	if (((tmp1 = snd_opl3sa2_read(chip, OPL3SA2_MIC)) & 0x9f) != 0x8a) {
		snd_printd("OPL3-SA [0x%lx] detect (2) = 0x%x (0x%x)\n", port, tmp, tmp1);
		return -ENODEV;
	}
	snd_opl3sa2_write(chip, OPL3SA2_MIC, 0x9f);
	/* initialization */
	/* Power Management - full on */
	snd_opl3sa2_write(chip, OPL3SA2_PM_CTRL, OPL3SA2_PM_D0);
	if (chip->version > 2) {
		/* ymode is bits 4&5 (of 0 to 7) on all but opl3sa2 versions */
		snd_opl3sa2_write(chip, OPL3SA2_SYS_CTRL, (chip->ymode << 4));
	} else {
		/* default for opl3sa2 versions */
		snd_opl3sa2_write(chip, OPL3SA2_SYS_CTRL, 0x00);
	}
	snd_opl3sa2_write(chip, OPL3SA2_IRQ_CONFIG, 0x0d);	/* Interrupt Channel Configuration - IRQ A = OPL3 + MPU + WSS */
	if (chip->single_dma) {
		snd_opl3sa2_write(chip, OPL3SA2_DMA_CONFIG, 0x03);	/* DMA Configuration - DMA A = WSS-R + WSS-P */
	} else {
		snd_opl3sa2_write(chip, OPL3SA2_DMA_CONFIG, 0x21);	/* DMA Configuration - DMA B = WSS-R, DMA A = WSS-P */
	}
	snd_opl3sa2_write(chip, OPL3SA2_MISC, 0x80 | (tmp & 7));	/* Miscellaneous - default */
	if (chip->version > 2) {
		snd_opl3sa2_write(chip, OPL3SA3_DGTL_DOWN, 0x00);	/* Digital Block Partial Power Down - default */
		snd_opl3sa2_write(chip, OPL3SA3_ANLG_DOWN, 0x00);	/* Analog Block Partial Power Down - default */
	}
	return 0;
}

static irqreturn_t snd_opl3sa2_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	unsigned short status;
	opl3sa2_t *chip = snd_magic_cast(opl3sa2_t, dev_id, return IRQ_NONE);
	int handled = 0;

	if (chip == NULL || chip->card == NULL)
		return IRQ_NONE;

	status = snd_opl3sa2_read(chip, OPL3SA2_IRQ_STATUS);

	if (status & 0x20) {
		handled = 1;
		snd_opl3_interrupt(chip->synth);
	}

	if ((status & 0x10) && chip->rmidi != NULL) {
		handled = 1;
		snd_mpu401_uart_interrupt(irq, chip->rmidi->private_data, regs);
	}

	if (status & 0x07) {	/* TI,CI,PI */
		handled = 1;
		snd_cs4231_interrupt(irq, chip->cs4231, regs);
	}

	if (status & 0x40) { /* hardware volume change */
		handled = 1;
		/* reading from Master Lch register at 0x07 clears this bit */
		snd_opl3sa2_read(chip, OPL3SA2_MASTER_RIGHT);
		snd_opl3sa2_read(chip, OPL3SA2_MASTER_LEFT);
		if (chip->master_switch && chip->master_volume) {
			snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->master_switch->id);
			snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->master_volume->id);
		}
	}
	return IRQ_RETVAL(handled);
}

#define OPL3SA2_SINGLE(xname, xindex, reg, shift, mask, invert) \
{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, .index = xindex, \
  .info = snd_opl3sa2_info_single, \
  .get = snd_opl3sa2_get_single, .put = snd_opl3sa2_put_single, \
  .private_value = reg | (shift << 8) | (mask << 16) | (invert << 24) }

static int snd_opl3sa2_info_single(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	int mask = (kcontrol->private_value >> 16) & 0xff;

	uinfo->type = mask == 1 ? SNDRV_CTL_ELEM_TYPE_BOOLEAN : SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = mask;
	return 0;
}

int snd_opl3sa2_get_single(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	opl3sa2_t *chip = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int reg = kcontrol->private_value & 0xff;
	int shift = (kcontrol->private_value >> 8) & 0xff;
	int mask = (kcontrol->private_value >> 16) & 0xff;
	int invert = (kcontrol->private_value >> 24) & 0xff;

	spin_lock_irqsave(&chip->reg_lock, flags);
	ucontrol->value.integer.value[0] = (chip->ctlregs[reg] >> shift) & mask;
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	if (invert)
		ucontrol->value.integer.value[0] = mask - ucontrol->value.integer.value[0];
	return 0;
}

int snd_opl3sa2_put_single(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	opl3sa2_t *chip = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int reg = kcontrol->private_value & 0xff;
	int shift = (kcontrol->private_value >> 8) & 0xff;
	int mask = (kcontrol->private_value >> 16) & 0xff;
	int invert = (kcontrol->private_value >> 24) & 0xff;
	int change;
	unsigned short val, oval;
	
	val = (ucontrol->value.integer.value[0] & mask);
	if (invert)
		val = mask - val;
	val <<= shift;
	spin_lock_irqsave(&chip->reg_lock, flags);
	oval = chip->ctlregs[reg];
	val = (oval & ~(mask << shift)) | val;
	change = val != oval;
	__snd_opl3sa2_write(chip, reg, val);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return change;
}

#define OPL3SA2_DOUBLE(xname, xindex, left_reg, right_reg, shift_left, shift_right, mask, invert) \
{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, .index = xindex, \
  .info = snd_opl3sa2_info_double, \
  .get = snd_opl3sa2_get_double, .put = snd_opl3sa2_put_double, \
  .private_value = left_reg | (right_reg << 8) | (shift_left << 16) | (shift_right << 19) | (mask << 24) | (invert << 22) }

int snd_opl3sa2_info_double(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	int mask = (kcontrol->private_value >> 24) & 0xff;

	uinfo->type = mask == 1 ? SNDRV_CTL_ELEM_TYPE_BOOLEAN : SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 2;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = mask;
	return 0;
}

int snd_opl3sa2_get_double(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	opl3sa2_t *chip = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int left_reg = kcontrol->private_value & 0xff;
	int right_reg = (kcontrol->private_value >> 8) & 0xff;
	int shift_left = (kcontrol->private_value >> 16) & 0x07;
	int shift_right = (kcontrol->private_value >> 19) & 0x07;
	int mask = (kcontrol->private_value >> 24) & 0xff;
	int invert = (kcontrol->private_value >> 22) & 1;
	
	spin_lock_irqsave(&chip->reg_lock, flags);
	ucontrol->value.integer.value[0] = (chip->ctlregs[left_reg] >> shift_left) & mask;
	ucontrol->value.integer.value[1] = (chip->ctlregs[right_reg] >> shift_right) & mask;
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	if (invert) {
		ucontrol->value.integer.value[0] = mask - ucontrol->value.integer.value[0];
		ucontrol->value.integer.value[1] = mask - ucontrol->value.integer.value[1];
	}
	return 0;
}

int snd_opl3sa2_put_double(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	opl3sa2_t *chip = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int left_reg = kcontrol->private_value & 0xff;
	int right_reg = (kcontrol->private_value >> 8) & 0xff;
	int shift_left = (kcontrol->private_value >> 16) & 0x07;
	int shift_right = (kcontrol->private_value >> 19) & 0x07;
	int mask = (kcontrol->private_value >> 24) & 0xff;
	int invert = (kcontrol->private_value >> 22) & 1;
	int change;
	unsigned short val1, val2, oval1, oval2;
	
	val1 = ucontrol->value.integer.value[0] & mask;
	val2 = ucontrol->value.integer.value[1] & mask;
	if (invert) {
		val1 = mask - val1;
		val2 = mask - val2;
	}
	val1 <<= shift_left;
	val2 <<= shift_right;
	spin_lock_irqsave(&chip->reg_lock, flags);
	if (left_reg != right_reg) {
		oval1 = chip->ctlregs[left_reg];
		oval2 = chip->ctlregs[right_reg];
		val1 = (oval1 & ~(mask << shift_left)) | val1;
		val2 = (oval2 & ~(mask << shift_right)) | val2;
		change = val1 != oval1 || val2 != oval2;
		__snd_opl3sa2_write(chip, left_reg, val1);
		__snd_opl3sa2_write(chip, right_reg, val2);
	} else {
		oval1 = chip->ctlregs[left_reg];
		val1 = (oval1 & ~((mask << shift_left) | (mask << shift_right))) | val1 | val2;
		change = val1 != oval1;
		__snd_opl3sa2_write(chip, left_reg, val1);
	}
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return change;
}

#define OPL3SA2_CONTROLS (sizeof(snd_opl3sa2_controls)/sizeof(snd_kcontrol_new_t))

static snd_kcontrol_new_t snd_opl3sa2_controls[] = {
OPL3SA2_DOUBLE("Master Playback Switch", 0, 0x07, 0x08, 7, 7, 1, 1),
OPL3SA2_DOUBLE("Master Playback Volume", 0, 0x07, 0x08, 0, 0, 15, 1),
OPL3SA2_SINGLE("Mic Playback Switch", 0, 0x09, 7, 1, 1),
OPL3SA2_SINGLE("Mic Playback Volume", 0, 0x09, 0, 31, 1)
};

#define OPL3SA2_TONE_CONTROLS (sizeof(snd_opl3sa2_tone_controls)/sizeof(snd_kcontrol_new_t))

static snd_kcontrol_new_t snd_opl3sa2_tone_controls[] = {
OPL3SA2_DOUBLE("3D Control - Wide", 0, 0x14, 0x14, 4, 0, 7, 0),
OPL3SA2_DOUBLE("Tone Control - Bass", 0, 0x15, 0x15, 4, 0, 7, 0),
OPL3SA2_DOUBLE("Tone Control - Treble", 0, 0x16, 0x16, 4, 0, 7, 0)
};

static void snd_opl3sa2_master_free(snd_kcontrol_t *kcontrol)
{
	opl3sa2_t *chip = snd_magic_cast(opl3sa2_t, _snd_kcontrol_chip(kcontrol), return);
	chip->master_switch = NULL;
	chip->master_volume = NULL;
}

static int __init snd_opl3sa2_mixer(opl3sa2_t *chip)
{
	snd_card_t *card = chip->card;
	snd_ctl_elem_id_t id1, id2;
	snd_kcontrol_t *kctl;
	unsigned int idx;
	int err;

	memset(&id1, 0, sizeof(id1));
	memset(&id2, 0, sizeof(id2));
	id1.iface = id2.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	/* reassign AUX0 to CD */
        strcpy(id1.name, "Aux Playback Switch");
        strcpy(id2.name, "CD Playback Switch");
        if ((err = snd_ctl_rename_id(card, &id1, &id2)) < 0)
                return err;
        strcpy(id1.name, "Aux Playback Volume");
        strcpy(id2.name, "CD Playback Volume");
        if ((err = snd_ctl_rename_id(card, &id1, &id2)) < 0)
                return err;
	/* reassign AUX1 to FM */
        strcpy(id1.name, "Aux Playback Switch"); id1.index = 1;
        strcpy(id2.name, "FM Playback Switch");
        if ((err = snd_ctl_rename_id(card, &id1, &id2)) < 0)
                return err;
        strcpy(id1.name, "Aux Playback Volume");
        strcpy(id2.name, "FM Playback Volume");
        if ((err = snd_ctl_rename_id(card, &id1, &id2)) < 0)
                return err;
	/* add OPL3SA2 controls */
	for (idx = 0; idx < OPL3SA2_CONTROLS; idx++) {
		if ((err = snd_ctl_add(card, kctl = snd_ctl_new1(&snd_opl3sa2_controls[idx], chip))) < 0)
			return err;
		switch (idx) {
		case 0: chip->master_switch = kctl; kctl->private_free = snd_opl3sa2_master_free; break;
		case 1: chip->master_volume = kctl; kctl->private_free = snd_opl3sa2_master_free; break;
		}
	}
	if (chip->version > 2) {
		for (idx = 0; idx < OPL3SA2_TONE_CONTROLS; idx++)
			if ((err = snd_ctl_add(card, snd_ctl_new1(&snd_opl3sa2_tone_controls[idx], chip))) < 0)
				return err;
	}
	return 0;
}

/* Power Management support functions */
#ifdef CONFIG_PM
static int snd_opl3sa2_suspend(snd_card_t *card, unsigned int state)
{
	opl3sa2_t *chip = snd_magic_cast(opl3sa2_t, card->pm_private_data, return -EINVAL);

	snd_pcm_suspend_all(chip->cs4231->pcm); /* stop before saving regs */
	chip->cs4231_suspend(chip->cs4231);

	/* power down */
	snd_opl3sa2_write(chip, OPL3SA2_PM_CTRL, OPL3SA2_PM_D3);

	snd_power_change_state(card, SNDRV_CTL_POWER_D3hot);
	return 0;
}

static int snd_opl3sa2_resume(snd_card_t *card, unsigned int state)
{
	opl3sa2_t *chip = snd_magic_cast(opl3sa2_t, card->pm_private_data, return -EINVAL);
	int i;

	/* power up */
	snd_opl3sa2_write(chip, OPL3SA2_PM_CTRL, OPL3SA2_PM_D0);

	/* restore registers */
	for (i = 2; i <= 0x0a; i++) {
		if (i != OPL3SA2_IRQ_STATUS)
			snd_opl3sa2_write(chip, i, chip->ctlregs[i]);
	}
	if (chip->version > 2) {
		for (i = 0x12; i <= 0x16; i++)
			snd_opl3sa2_write(chip, i, chip->ctlregs[i]);
	}
	/* restore cs4231 */
	chip->cs4231_resume(chip->cs4231);

	snd_power_change_state(card, SNDRV_CTL_POWER_D0);
	return 0;
}
#endif /* CONFIG_PM */

#ifdef CONFIG_PNP
static int __init snd_opl3sa2_pnp(int dev, opl3sa2_t *chip,
				  struct pnp_card_link *card,
				  const struct pnp_card_device_id *id)
{
	struct pnp_dev *pdev;
	struct pnp_resource_table * cfg = kmalloc(sizeof(struct pnp_resource_table), GFP_KERNEL);
	int err;

	if (!cfg)
		return -ENOMEM;
	pdev = chip->dev = pnp_request_card_device(card, id->devs[0].id, NULL);
	if (chip->dev == NULL) {
		kfree(cfg);
		return -EBUSY;
	}
	/* PnP initialization */
	pnp_init_resource_table(cfg);
	if (sb_port[dev] != SNDRV_AUTO_PORT)
		pnp_resource_change(&cfg->port_resource[0], sb_port[dev], 16);
	if (wss_port[dev] != SNDRV_AUTO_PORT)
		pnp_resource_change(&cfg->port_resource[1], wss_port[dev], 8);
	if (fm_port[dev] != SNDRV_AUTO_PORT)
		pnp_resource_change(&cfg->port_resource[2], fm_port[dev], 4);
	if (midi_port[dev] != SNDRV_AUTO_PORT)
		pnp_resource_change(&cfg->port_resource[3], midi_port[dev], 2);
	if (port[dev] != SNDRV_AUTO_PORT)
		pnp_resource_change(&cfg->port_resource[4], port[dev], 2);
	if (dma1[dev] != SNDRV_AUTO_DMA)
		pnp_resource_change(&cfg->dma_resource[0], dma1[dev], 1);
	if (dma2[dev] != SNDRV_AUTO_DMA)
		pnp_resource_change(&cfg->dma_resource[1], dma2[dev], 1);
	if (irq[dev] != SNDRV_AUTO_IRQ)
		pnp_resource_change(&cfg->irq_resource[0], irq[dev], 1);
	err = pnp_manual_config_dev(pdev, cfg, 0);
	if (err < 0)
		snd_printk(KERN_ERR "PnP manual resources are invalid, using auto config\n");
	err = pnp_activate_dev(pdev);
	if (err < 0) {
		kfree(cfg);
		snd_printk(KERN_ERR "PnP configure failure (out of resources?) err = %d\n", err);
		return -EBUSY;
	}
	sb_port[dev] = pnp_port_start(pdev, 0);
	wss_port[dev] = pnp_port_start(pdev, 1);
	fm_port[dev] = pnp_port_start(pdev, 2);
	midi_port[dev] = pnp_port_start(pdev, 3);
	port[dev] = pnp_port_start(pdev, 4);
	dma1[dev] = pnp_dma(pdev, 0);
	dma2[dev] = pnp_dma(pdev, 1);
	irq[dev] = pnp_irq(pdev, 0);
	snd_printdd("PnP OPL3-SA: sb port=0x%lx, wss port=0x%lx, fm port=0x%lx, midi port=0x%lx\n",
		sb_port[dev], wss_port[dev], fm_port[dev], midi_port[dev]);
	snd_printdd("PnP OPL3-SA: control port=0x%lx, dma1=%i, dma2=%i, irq=%i\n",
		port[dev], dma1[dev], dma2[dev], irq[dev]);
	kfree(cfg);
	return 0;
}
#endif /* CONFIG_PNP */

static int snd_opl3sa2_free(opl3sa2_t *chip)
{
	if (chip->irq >= 0)
		free_irq(chip->irq, (void *)chip);
	if (chip->res_port) {
		release_resource(chip->res_port);
		kfree_nocheck(chip->res_port);
	}
	snd_magic_kfree(chip);
	return 0;
}

static int snd_opl3sa2_dev_free(snd_device_t *device)
{
	opl3sa2_t *chip = snd_magic_cast(opl3sa2_t, device->device_data, return -ENXIO);
	return snd_opl3sa2_free(chip);
}

static int __devinit snd_opl3sa2_probe(int dev,
				       struct pnp_card_link *pcard,
				       const struct pnp_card_device_id *pid)
{
	int xirq, xdma1, xdma2;
	snd_card_t *card;
	struct snd_opl3sa2 *chip;
	cs4231_t *cs4231;
	opl3_t *opl3;
	static snd_device_ops_t ops = {
		.dev_free =	snd_opl3sa2_dev_free,
	};
	int err;

#ifdef CONFIG_PNP
	if (!isapnp[dev]) {
#endif
		if (port[dev] == SNDRV_AUTO_PORT) {
			snd_printk("specify port\n");
			return -EINVAL;
		}
		if (wss_port[dev] == SNDRV_AUTO_PORT) {
			snd_printk("specify wss_port\n");
			return -EINVAL;
		}
		if (fm_port[dev] == SNDRV_AUTO_PORT) {
			snd_printk("specify fm_port\n");
			return -EINVAL;
		}
		if (midi_port[dev] == SNDRV_AUTO_PORT) {
			snd_printk("specify midi_port\n");
			return -EINVAL;
		}
#ifdef CONFIG_PNP
	}
#endif
	card = snd_card_new(index[dev], id[dev], THIS_MODULE, 0);
	if (card == NULL)
		return -ENOMEM;
	strcpy(card->driver, "OPL3SA2");
	strcpy(card->shortname, "Yamaha OPL3-SA2");
	chip = snd_magic_kcalloc(opl3sa2_t, 0, GFP_KERNEL);
	if (chip == NULL) {
		err = -ENOMEM;
		goto __error;
	}
	spin_lock_init(&chip->reg_lock);
	chip->irq = -1;
	if ((err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, chip, &ops)) < 0)
		goto __error;
#ifdef CONFIG_PNP
	if (isapnp[dev]) {
		if ((err = snd_opl3sa2_pnp(dev, chip, pcard, pid)) < 0)
			goto __error;
		snd_card_set_dev(card, &pcard->card->dev);
	}
#endif
	chip->ymode = opl3sa3_ymode[dev] & 0x03 ; /* initialise this card from supplied (or default) parameter*/ 
	chip->card = card;
	chip->port = port[dev];
	xirq = irq[dev];
	xdma1 = dma1[dev];
	xdma2 = dma2[dev];
	if (xdma2 < 0)
		chip->single_dma = 1;
	if ((err = snd_opl3sa2_detect(chip)) < 0)
		goto __error;
	if (request_irq(xirq, snd_opl3sa2_interrupt, SA_INTERRUPT, "OPL3-SA2/3", (void *)chip)) {
		snd_printk(KERN_ERR "opl3sa2: can't grab IRQ %d\n", xirq);
		err = -ENODEV;
		goto __error;
	}
	chip->irq = xirq;
	if ((err = snd_cs4231_create(card,
				     wss_port[dev] + 4, -1,
				     xirq, xdma1, xdma2,
				     CS4231_HW_OPL3SA2,
				     CS4231_HWSHARE_IRQ,
				     &cs4231)) < 0) {
		snd_printd("Oops, WSS not detected at 0x%lx\n", wss_port[dev] + 4);
		goto __error;
	}
	chip->cs4231 = cs4231;
	if ((err = snd_cs4231_pcm(cs4231, 0, NULL)) < 0)
		goto __error;
	if ((err = snd_cs4231_mixer(cs4231)) < 0)
		goto __error;
	if ((err = snd_opl3sa2_mixer(chip)) < 0)
		goto __error;
	if ((err = snd_cs4231_timer(cs4231, 0, NULL)) < 0)
		goto __error;
	if (fm_port[dev] >= 0x340 && fm_port[dev] < 0x400) {
		if ((err = snd_opl3_create(card, fm_port[dev],
					   fm_port[dev] + 2,
					   OPL3_HW_OPL3, 0, &opl3)) < 0)
			goto __error;
		if ((err = snd_opl3_timer_new(opl3, 1, 2)) < 0)
			goto __error;
		if ((err = snd_opl3_hwdep_new(opl3, 0, 1, &chip->synth)) < 0)
			goto __error;
	}
	if (midi_port[dev] >= 0x300 && midi_port[dev] < 0x340) {
		if ((err = snd_mpu401_uart_new(card, 0, MPU401_HW_OPL3SA2,
					       midi_port[dev], 0,
					       xirq, 0, &chip->rmidi)) < 0)
			goto __error;
	}
#ifdef CONFIG_PM
	chip->cs4231_suspend = chip->cs4231->suspend;
	chip->cs4231_resume = chip->cs4231->resume;
	/* now clear callbacks for cs4231 */
	chip->cs4231->suspend = NULL;
	chip->cs4231->resume = NULL;
	snd_card_set_isa_pm_callback(card, snd_opl3sa2_suspend, snd_opl3sa2_resume, chip);
#endif

	sprintf(card->longname, "%s at 0x%lx, irq %d, dma %d",
		card->shortname, chip->port, xirq, xdma1);
	if (dma2 >= 0)
		sprintf(card->longname + strlen(card->longname), "&%d", xdma2);

	if ((err = snd_card_register(card)) < 0)
		goto __error;

	if (pcard)
		pnp_set_card_drvdata(pcard, card);
	else
		snd_opl3sa2_legacy[dev] = card;
	return 0;

 __error:
	snd_card_free(card);
	return err;
}

#ifdef CONFIG_PNP
static int __devinit snd_opl3sa2_pnp_detect(struct pnp_card_link *card,
					    const struct pnp_card_device_id *id)
{
        static int dev;
        int res;

        for ( ; dev < SNDRV_CARDS; dev++) {
                if (!enable[dev] && !isapnp[dev])
                        continue;
                res = snd_opl3sa2_probe(dev, card, id);
                if (res < 0)
                        return res;
                dev++;
                return 0;
        }
        return -ENODEV;
}

static void __devexit snd_opl3sa2_pnp_remove(struct pnp_card_link * pcard)
{
	snd_card_t *card = (snd_card_t *) pnp_get_card_drvdata(pcard);
        
	snd_card_disconnect(card);
	snd_card_free_in_thread(card);
}

static struct pnp_card_driver opl3sa2_pnpc_driver = {
	.flags = PNP_DRIVER_RES_DISABLE,
	.name = "opl3sa2",
	.id_table = snd_opl3sa2_pnpids,
	.probe = snd_opl3sa2_pnp_detect,
	.remove = __devexit_p(snd_opl3sa2_pnp_remove),
};
#endif /* CONFIG_PNP */

static int __init alsa_card_opl3sa2_init(void)
{
	int dev, cards = 0;

	for (dev = 0; dev < SNDRV_CARDS; dev++) {
		if (!enable[dev])
			continue;
#ifdef CONFIG_PNP
		if (isapnp[dev])
			continue;
#endif
		if (snd_opl3sa2_probe(dev, NULL, NULL) >= 0)
			cards++;
	}
#ifdef CONFIG_PNP
	cards += pnp_register_card_driver(&opl3sa2_pnpc_driver);
#endif
	if (!cards) {
#ifdef MODULE
		snd_printk(KERN_ERR "Yamaha OPL3-SA soundcard not found or device busy\n");
#endif
#ifdef CONFIG_PNP
		pnp_unregister_card_driver(&opl3sa2_pnpc_driver);
#endif
		return -ENODEV;
	}
	return 0;
}

static void __exit alsa_card_opl3sa2_exit(void)
{
	int idx;

#ifdef CONFIG_PNP
	/* PnP cards first */
	pnp_unregister_card_driver(&opl3sa2_pnpc_driver);
#endif
	for (idx = 0; idx < SNDRV_CARDS; idx++)
		snd_card_free(snd_opl3sa2_legacy[idx]);
}

module_init(alsa_card_opl3sa2_init)
module_exit(alsa_card_opl3sa2_exit)
