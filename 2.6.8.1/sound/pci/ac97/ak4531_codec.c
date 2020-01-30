/*
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
 *  Universal routines for AK4531 codec
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
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/ak4531_codec.h>

MODULE_AUTHOR("Jaroslav Kysela <perex@suse.cz>");
MODULE_DESCRIPTION("Universal routines for AK4531 codec");
MODULE_LICENSE("GPL");

#define chip_t ak4531_t

static void snd_ak4531_proc_init(snd_card_t * card, ak4531_t * ak4531);

/*
 *
 */
 
#if 0

static void snd_ak4531_dump(ak4531_t *ak4531)
{
	int idx;
	
	for (idx = 0; idx < 0x19; idx++)
		printk("ak4531 0x%x: 0x%x\n", idx, ak4531->regs[idx]);
}

#endif

/*
 *
 */

#define AK4531_SINGLE(xname, xindex, reg, shift, mask, invert) \
{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, .index = xindex, \
  .info = snd_ak4531_info_single, \
  .get = snd_ak4531_get_single, .put = snd_ak4531_put_single, \
  .private_value = reg | (shift << 16) | (mask << 24) | (invert << 22) }

static int snd_ak4531_info_single(snd_kcontrol_t * kcontrol, snd_ctl_elem_info_t * uinfo)
{
	int mask = (kcontrol->private_value >> 24) & 0xff;

	uinfo->type = mask == 1 ? SNDRV_CTL_ELEM_TYPE_BOOLEAN : SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = mask;
	return 0;
}
 
static int snd_ak4531_get_single(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ak4531_t *ak4531 = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int reg = kcontrol->private_value & 0xff;
	int shift = (kcontrol->private_value >> 16) & 0x07;
	int mask = (kcontrol->private_value >> 24) & 0xff;
	int invert = (kcontrol->private_value >> 22) & 1;
	int val;

	spin_lock_irqsave(&ak4531->reg_lock, flags);
	val = (ak4531->regs[reg] >> shift) & mask;
	spin_unlock_irqrestore(&ak4531->reg_lock, flags);
	if (invert) {
		val = mask - val;
	}
	ucontrol->value.integer.value[0] = val;
	return 0;
}                                                                                                                                                                                                                                                                                                            

static int snd_ak4531_put_single(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ak4531_t *ak4531 = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int reg = kcontrol->private_value & 0xff;
	int shift = (kcontrol->private_value >> 16) & 0x07;
	int mask = (kcontrol->private_value >> 24) & 0xff;
	int invert = (kcontrol->private_value >> 22) & 1;
	int change;
	int val;

	val = ucontrol->value.integer.value[0] & mask;
	if (invert) {
		val = mask - val;
	}
	val <<= shift;
	spin_lock_irqsave(&ak4531->reg_lock, flags);
	val = (ak4531->regs[reg] & ~(mask << shift)) | val;
	change = val != ak4531->regs[reg];
	ak4531->write(ak4531, reg, ak4531->regs[reg] = val);
	spin_unlock_irqrestore(&ak4531->reg_lock, flags);
	return change;
}                                                                                                                                                                                                                                                                                                            

#define AK4531_DOUBLE(xname, xindex, left_reg, right_reg, left_shift, right_shift, mask, invert) \
{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, .index = xindex, \
  .info = snd_ak4531_info_double, \
  .get = snd_ak4531_get_double, .put = snd_ak4531_put_double, \
  .private_value = left_reg | (right_reg << 8) | (left_shift << 16) | (right_shift << 19) | (mask << 24) | (invert << 22) }

static int snd_ak4531_info_double(snd_kcontrol_t * kcontrol, snd_ctl_elem_info_t * uinfo)
{
	int mask = (kcontrol->private_value >> 24) & 0xff;

	uinfo->type = mask == 1 ? SNDRV_CTL_ELEM_TYPE_BOOLEAN : SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 2;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = mask;
	return 0;
}
 
static int snd_ak4531_get_double(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ak4531_t *ak4531 = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int left_reg = kcontrol->private_value & 0xff;
	int right_reg = (kcontrol->private_value >> 8) & 0xff;
	int left_shift = (kcontrol->private_value >> 16) & 0x07;
	int right_shift = (kcontrol->private_value >> 19) & 0x07;
	int mask = (kcontrol->private_value >> 24) & 0xff;
	int invert = (kcontrol->private_value >> 22) & 1;
	int left, right;

	spin_lock_irqsave(&ak4531->reg_lock, flags);
	left = (ak4531->regs[left_reg] >> left_shift) & mask;
	right = (ak4531->regs[right_reg] >> right_shift) & mask;
	spin_unlock_irqrestore(&ak4531->reg_lock, flags);
	if (invert) {
		left = mask - left;
		right = mask - right;
	}
	ucontrol->value.integer.value[0] = left;
	ucontrol->value.integer.value[1] = right;
	return 0;
}                                                                                                                                                                                                                                                                                                            

static int snd_ak4531_put_double(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ak4531_t *ak4531 = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int left_reg = kcontrol->private_value & 0xff;
	int right_reg = (kcontrol->private_value >> 8) & 0xff;
	int left_shift = (kcontrol->private_value >> 16) & 0x07;
	int right_shift = (kcontrol->private_value >> 19) & 0x07;
	int mask = (kcontrol->private_value >> 24) & 0xff;
	int invert = (kcontrol->private_value >> 22) & 1;
	int change;
	int left, right;

	left = ucontrol->value.integer.value[0] & mask;
	right = ucontrol->value.integer.value[1] & mask;
	if (invert) {
		left = mask - left;
		right = mask - right;
	}
	left <<= left_shift;
	right <<= right_shift;
	spin_lock_irqsave(&ak4531->reg_lock, flags);
	if (left_reg == right_reg) {
		left = (ak4531->regs[left_reg] & ~((mask << left_shift) | (mask << right_shift))) | left | right;
		change = left != ak4531->regs[left_reg];
		ak4531->write(ak4531, left_reg, ak4531->regs[left_reg] = left);
	} else {
		left = (ak4531->regs[left_reg] & ~(mask << left_shift)) | left;
		right = (ak4531->regs[right_reg] & ~(mask << right_shift)) | right;
		change = left != ak4531->regs[left_reg] || right != ak4531->regs[right_reg];
		ak4531->write(ak4531, left_reg, ak4531->regs[left_reg] = left);
		ak4531->write(ak4531, right_reg, ak4531->regs[right_reg] = right);
	}
	spin_unlock_irqrestore(&ak4531->reg_lock, flags);
	return change;
}                                                                                                                                                                                                                                                                                                            

#define AK4531_INPUT_SW(xname, xindex, reg1, reg2, left_shift, right_shift) \
{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, .index = xindex, \
  .info = snd_ak4531_info_input_sw, \
  .get = snd_ak4531_get_input_sw, .put = snd_ak4531_put_input_sw, \
  .private_value = reg1 | (reg2 << 8) | (left_shift << 16) | (right_shift << 24) }

static int snd_ak4531_info_input_sw(snd_kcontrol_t * kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 4;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}
 
static int snd_ak4531_get_input_sw(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ak4531_t *ak4531 = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int reg1 = kcontrol->private_value & 0xff;
	int reg2 = (kcontrol->private_value >> 8) & 0xff;
	int left_shift = (kcontrol->private_value >> 16) & 0x0f;
	int right_shift = (kcontrol->private_value >> 24) & 0x0f;

	spin_lock_irqsave(&ak4531->reg_lock, flags);
	ucontrol->value.integer.value[0] = (ak4531->regs[reg1] >> left_shift) & 1;
	ucontrol->value.integer.value[1] = (ak4531->regs[reg2] >> left_shift) & 1;
	ucontrol->value.integer.value[2] = (ak4531->regs[reg1] >> right_shift) & 1;
	ucontrol->value.integer.value[3] = (ak4531->regs[reg2] >> right_shift) & 1;
	spin_unlock_irqrestore(&ak4531->reg_lock, flags);
	return 0;
}                                                                                                                                                                                                                                                                                                            

static int snd_ak4531_put_input_sw(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ak4531_t *ak4531 = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int reg1 = kcontrol->private_value & 0xff;
	int reg2 = (kcontrol->private_value >> 8) & 0xff;
	int left_shift = (kcontrol->private_value >> 16) & 0x0f;
	int right_shift = (kcontrol->private_value >> 24) & 0x0f;
	int change;
	int val1, val2;

	spin_lock_irqsave(&ak4531->reg_lock, flags);
	val1 = ak4531->regs[reg1] & ~((1 << left_shift) | (1 << right_shift));
	val2 = ak4531->regs[reg2] & ~((1 << left_shift) | (1 << right_shift));
	val1 |= (ucontrol->value.integer.value[0] & 1) << left_shift;
	val2 |= (ucontrol->value.integer.value[1] & 1) << left_shift;
	val1 |= (ucontrol->value.integer.value[2] & 1) << right_shift;
	val2 |= (ucontrol->value.integer.value[3] & 1) << right_shift;
	change = val1 != ak4531->regs[reg1] || val2 != ak4531->regs[reg2];
	ak4531->write(ak4531, reg1, ak4531->regs[reg1] = val1);
	ak4531->write(ak4531, reg2, ak4531->regs[reg2] = val2);
	spin_unlock_irqrestore(&ak4531->reg_lock, flags);
	return change;
}                                                                                                                                                                                                                                                                                                            

#define AK4531_CONTROLS (sizeof(snd_ak4531_controls)/sizeof(snd_kcontrol_new_t))

static snd_kcontrol_new_t snd_ak4531_controls[] = {

AK4531_DOUBLE("Master Playback Switch", 0, AK4531_LMASTER, AK4531_RMASTER, 7, 7, 1, 1),
AK4531_DOUBLE("Master Playback Volume", 0, AK4531_LMASTER, AK4531_RMASTER, 0, 0, 0x1f, 1),

AK4531_SINGLE("Master Mono Playback Switch", 0, AK4531_MONO_OUT, 7, 1, 1),
AK4531_SINGLE("Master Mono Playback Volume", 0, AK4531_MONO_OUT, 0, 0x07, 1),

AK4531_DOUBLE("PCM Switch", 0, AK4531_LVOICE, AK4531_RVOICE, 7, 7, 1, 1),
AK4531_DOUBLE("PCM Volume", 0, AK4531_LVOICE, AK4531_RVOICE, 0, 0, 0x1f, 1),
AK4531_DOUBLE("PCM Playback Switch", 0, AK4531_OUT_SW2, AK4531_OUT_SW2, 3, 2, 1, 0),
AK4531_DOUBLE("PCM Capture Switch", 0, AK4531_LIN_SW2, AK4531_RIN_SW2, 2, 2, 1, 0),

AK4531_DOUBLE("PCM Switch", 1, AK4531_LFM, AK4531_RFM, 7, 7, 1, 1),
AK4531_DOUBLE("PCM Volume", 1, AK4531_LFM, AK4531_RFM, 0, 0, 0x1f, 1),
AK4531_DOUBLE("PCM Playback Switch", 1, AK4531_OUT_SW1, AK4531_OUT_SW1, 6, 5, 1, 0),
AK4531_INPUT_SW("PCM Capture Route", 1, AK4531_LIN_SW1, AK4531_RIN_SW1, 6, 5),

AK4531_DOUBLE("CD Switch", 0, AK4531_LCD, AK4531_RCD, 7, 7, 1, 1),
AK4531_DOUBLE("CD Volume", 0, AK4531_LCD, AK4531_RCD, 0, 0, 0x1f, 1),
AK4531_DOUBLE("CD Playback Switch", 0, AK4531_OUT_SW1, AK4531_OUT_SW1, 2, 1, 1, 0),
AK4531_INPUT_SW("CD Capture Route", 0, AK4531_LIN_SW1, AK4531_RIN_SW1, 2, 1),

AK4531_DOUBLE("Line Switch", 0, AK4531_LLINE, AK4531_RLINE, 7, 7, 1, 1),
AK4531_DOUBLE("Line Volume", 0, AK4531_LLINE, AK4531_RLINE, 0, 0, 0x1f, 1),
AK4531_DOUBLE("Line Playback Switch", 0, AK4531_OUT_SW1, AK4531_OUT_SW1, 4, 3, 1, 0),
AK4531_INPUT_SW("Line Capture Route", 0, AK4531_LIN_SW1, AK4531_RIN_SW1, 4, 3),

AK4531_DOUBLE("Aux Switch", 0, AK4531_LAUXA, AK4531_RAUXA, 7, 7, 1, 1),
AK4531_DOUBLE("Aux Volume", 0, AK4531_LAUXA, AK4531_RAUXA, 0, 0, 0x1f, 1),
AK4531_DOUBLE("Aux Playback Switch", 0, AK4531_OUT_SW2, AK4531_OUT_SW2, 5, 4, 1, 0),
AK4531_INPUT_SW("Aux Capture Route", 0, AK4531_LIN_SW2, AK4531_RIN_SW2, 4, 3),

AK4531_SINGLE("Mono Switch", 0, AK4531_MONO1, 7, 1, 1),
AK4531_SINGLE("Mono Volume", 0, AK4531_MONO1, 0, 0x1f, 1),
AK4531_SINGLE("Mono Playback Switch", 0, AK4531_OUT_SW2, 0, 1, 0),
AK4531_DOUBLE("Mono Capture Switch", 0, AK4531_LIN_SW2, AK4531_RIN_SW2, 0, 0, 1, 0),

AK4531_SINGLE("Mono Switch", 1, AK4531_MONO2, 7, 1, 1),
AK4531_SINGLE("Mono Volume", 1, AK4531_MONO2, 0, 0x1f, 1),
AK4531_SINGLE("Mono Playback Switch", 1, AK4531_OUT_SW2, 1, 1, 0),
AK4531_DOUBLE("Mono Capture Switch", 1, AK4531_LIN_SW2, AK4531_RIN_SW2, 1, 1, 1, 0),

AK4531_SINGLE("Mic Volume", 0, AK4531_MIC, 0, 0x1f, 1),
AK4531_SINGLE("Mic Switch", 0, AK4531_MIC, 7, 1, 1),
AK4531_SINGLE("Mic Playback Switch", 0, AK4531_OUT_SW1, 0, 1, 0),
AK4531_DOUBLE("Mic Capture Switch", 0, AK4531_LIN_SW1, AK4531_RIN_SW1, 0, 0, 1, 0),

AK4531_DOUBLE("Mic Bypass Capture Switch", 0, AK4531_LIN_SW2, AK4531_RIN_SW2, 7, 7, 1, 0),
AK4531_DOUBLE("Mono1 Bypass Capture Switch", 0, AK4531_LIN_SW2, AK4531_RIN_SW2, 6, 6, 1, 0),
AK4531_DOUBLE("Mono2 Bypass Capture Switch", 0, AK4531_LIN_SW2, AK4531_RIN_SW2, 5, 5, 1, 0),

AK4531_SINGLE("AD Input Select", 0, AK4531_AD_IN, 0, 1, 0),
AK4531_SINGLE("Mic Boost (+30dB)", 0, AK4531_MIC_GAIN, 0, 1, 0)
};

static int snd_ak4531_free(ak4531_t *ak4531)
{
	if (ak4531) {
		if (ak4531->private_free)
			ak4531->private_free(ak4531);
		snd_magic_kfree(ak4531);
	}
	return 0;
}

static int snd_ak4531_dev_free(snd_device_t *device)
{
	ak4531_t *ak4531 = snd_magic_cast(ak4531_t, device->device_data, return -ENXIO);
	return snd_ak4531_free(ak4531);
}

static u8 snd_ak4531_initial_map[0x19 + 1] = {
	0x9f,		/* 00: Master Volume Lch */
	0x9f,		/* 01: Master Volume Rch */
	0x9f,		/* 02: Voice Volume Lch */
	0x9f,		/* 03: Voice Volume Rch */
	0x9f,		/* 04: FM Volume Lch */
	0x9f,		/* 05: FM Volume Rch */
	0x9f,		/* 06: CD Audio Volume Lch */
	0x9f,		/* 07: CD Audio Volume Rch */
	0x9f,		/* 08: Line Volume Lch */
	0x9f,		/* 09: Line Volume Rch */
	0x9f,		/* 0a: Aux Volume Lch */
	0x9f,		/* 0b: Aux Volume Rch */
	0x9f,		/* 0c: Mono1 Volume */
	0x9f,		/* 0d: Mono2 Volume */
	0x9f,		/* 0e: Mic Volume */
	0x87,		/* 0f: Mono-out Volume */
	0x00,		/* 10: Output Mixer SW1 */
	0x00,		/* 11: Output Mixer SW2 */
	0x00,		/* 12: Lch Input Mixer SW1 */
	0x00,		/* 13: Rch Input Mixer SW1 */
	0x00,		/* 14: Lch Input Mixer SW2 */
	0x00,		/* 15: Rch Input Mixer SW2 */
	0x00,		/* 16: Reset & Power Down */
	0x00,		/* 17: Clock Select */
	0x00,		/* 18: AD Input Select */
	0x01		/* 19: Mic Amp Setup */
};

int snd_ak4531_mixer(snd_card_t * card, ak4531_t * _ak4531, ak4531_t ** rak4531)
{
	unsigned int idx;
	int err;
	ak4531_t * ak4531;
	static snd_device_ops_t ops = {
		.dev_free =	snd_ak4531_dev_free,
	};

	snd_assert(rak4531 != NULL, return -EINVAL);
	*rak4531 = NULL;
	snd_assert(card != NULL && _ak4531 != NULL, return -EINVAL);
	ak4531 = snd_magic_kcalloc(ak4531_t, 0, GFP_KERNEL);
	if (ak4531 == NULL)
		return -ENOMEM;
	*ak4531 = *_ak4531;
	spin_lock_init(&ak4531->reg_lock);
	if ((err = snd_component_add(card, "AK4531")) < 0) {
		snd_ak4531_free(ak4531);
		return err;
	}
	strcpy(card->mixername, "Asahi Kasei AK4531");
	ak4531->write(ak4531, AK4531_RESET, 0x03);	/* no RST, PD */
	udelay(100);
	ak4531->write(ak4531, AK4531_CLOCK, 0x00);	/* CODEC ADC and CODEC DAC use {LR,B}CLK2 and run off LRCLK2 PLL */
	for (idx = 0; idx < 0x19; idx++) {
		if (idx == AK4531_RESET || idx == AK4531_CLOCK)
			continue;
		ak4531->write(ak4531, idx, ak4531->regs[idx] = snd_ak4531_initial_map[idx]);	/* recording source is mixer */
	}
	for (idx = 0; idx < AK4531_CONTROLS; idx++) {
		if ((err = snd_ctl_add(card, snd_ctl_new1(&snd_ak4531_controls[idx], ak4531))) < 0) {
			snd_ak4531_free(ak4531);
			return err;
		}
	}
	snd_ak4531_proc_init(card, ak4531);
	if ((err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, ak4531, &ops)) < 0) {
		snd_ak4531_free(ak4531);
		return err;
	}

#if 0
	snd_ak4531_dump(ak4531);
#endif
	*rak4531 = ak4531;
	return 0;
}

/*

 */

static void snd_ak4531_proc_read(snd_info_entry_t *entry, 
				 snd_info_buffer_t * buffer)
{
	ak4531_t *ak4531 = snd_magic_cast(ak4531_t, entry->private_data, return);

	snd_iprintf(buffer, "Asahi Kasei AK4531\n\n");
	snd_iprintf(buffer, "Recording source   : %s\n"
		    "MIC gain           : %s\n",
		    ak4531->regs[AK4531_AD_IN] & 1 ? "external" : "mixer",
		    ak4531->regs[AK4531_MIC_GAIN] & 1 ? "+30dB" : "+0dB");
}

static void snd_ak4531_proc_init(snd_card_t * card, ak4531_t * ak4531)
{
	snd_info_entry_t *entry;

	if (! snd_card_proc_new(card, "ak4531", &entry))
		snd_info_set_text_ops(entry, ak4531, 1024, snd_ak4531_proc_read);
}

EXPORT_SYMBOL(snd_ak4531_mixer);

/*
 *  INIT part
 */

static int __init alsa_ak4531_init(void)
{
	return 0;
}

static void __exit alsa_ak4531_exit(void)
{
}

module_init(alsa_ak4531_init)
module_exit(alsa_ak4531_exit)
