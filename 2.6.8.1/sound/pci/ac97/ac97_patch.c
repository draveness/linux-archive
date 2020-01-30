/*
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
 *  Universal interface for Audio Codec '97
 *
 *  For more details look to AC '97 component specification revision 2.2
 *  by Intel Corporation (http://developer.intel.com) and to datasheets
 *  for specific codecs.
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
#include <sound/pcm.h>
#include <sound/control.h>
#include <sound/ac97_codec.h>
#include "ac97_patch.h"
#include "ac97_id.h"
#include "ac97_local.h"

#define chip_t ac97_t

/*
 *  Chip specific initialization
 */

static int patch_build_controls(ac97_t * ac97, const snd_kcontrol_new_t *controls, int count)
{
	int idx, err;

	for (idx = 0; idx < count; idx++)
		if ((err = snd_ctl_add(ac97->bus->card, snd_ac97_cnew(&controls[idx], ac97))) < 0)
			return err;
	return 0;
}

/* The following snd_ac97_ymf753_... items added by David Shust (dshust@shustring.com) */

/* It is possible to indicate to the Yamaha YMF753 the type of speakers being used. */
static int snd_ac97_ymf753_info_speaker(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	static char *texts[3] = {
		"Standard", "Small", "Smaller"
	};

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 3;
	if (uinfo->value.enumerated.item > 2)
		uinfo->value.enumerated.item = 2;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_ac97_ymf753_get_speaker(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	unsigned short val;

	val = ac97->regs[AC97_YMF753_3D_MODE_SEL];
	val = (val >> 10) & 3;
	if (val > 0)    /* 0 = invalid */
		val--;
	ucontrol->value.enumerated.item[0] = val;
	return 0;
}

static int snd_ac97_ymf753_put_speaker(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	unsigned short val;

	if (ucontrol->value.enumerated.item[0] > 2)
		return -EINVAL;
	val = (ucontrol->value.enumerated.item[0] + 1) << 10;
	return snd_ac97_update(ac97, AC97_YMF753_3D_MODE_SEL, val);
}

static const snd_kcontrol_new_t snd_ac97_ymf753_controls_speaker =
{
	.iface  = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name   = "3D Control - Speaker",
	.info   = snd_ac97_ymf753_info_speaker,
	.get    = snd_ac97_ymf753_get_speaker,
	.put    = snd_ac97_ymf753_put_speaker,
};

/* It is possible to indicate to the Yamaha YMF753 the source to direct to the S/PDIF output. */
static int snd_ac97_ymf753_spdif_source_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	static char *texts[2] = { "AC-Link", "A/D Converter" };

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 2;
	if (uinfo->value.enumerated.item > 1)
		uinfo->value.enumerated.item = 1;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_ac97_ymf753_spdif_source_get(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	unsigned short val;

	val = ac97->regs[AC97_YMF753_DIT_CTRL2];
	ucontrol->value.enumerated.item[0] = (val >> 1) & 1;
	return 0;
}

static int snd_ac97_ymf753_spdif_source_put(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	unsigned short val;

	if (ucontrol->value.enumerated.item[0] > 1)
		return -EINVAL;
	val = ucontrol->value.enumerated.item[0] << 1;
	return snd_ac97_update_bits(ac97, AC97_YMF753_DIT_CTRL2, 0x0002, val);
}

/* The AC'97 spec states that the S/PDIF signal is to be output at pin 48.
   The YMF753 will output the S/PDIF signal to pin 43, 47 (EAPD), or 48.
   By default, no output pin is selected, and the S/PDIF signal is not output.
   There is also a bit to mute S/PDIF output in a vendor-specific register. */
static int snd_ac97_ymf753_spdif_output_pin_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	static char *texts[3] = { "Disabled", "Pin 43", "Pin 48" };

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 3;
	if (uinfo->value.enumerated.item > 2)
		uinfo->value.enumerated.item = 2;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_ac97_ymf753_spdif_output_pin_get(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	unsigned short val;

	val = ac97->regs[AC97_YMF753_DIT_CTRL2];
	ucontrol->value.enumerated.item[0] = (val & 0x0008) ? 2 : (val & 0x0020) ? 1 : 0;
	return 0;
}

static int snd_ac97_ymf753_spdif_output_pin_put(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	unsigned short val;

	if (ucontrol->value.enumerated.item[0] > 2)
		return -EINVAL;
	val = (ucontrol->value.enumerated.item[0] == 2) ? 0x0008 :
	      (ucontrol->value.enumerated.item[0] == 1) ? 0x0020 : 0;
	return snd_ac97_update_bits(ac97, AC97_YMF753_DIT_CTRL2, 0x0028, val);
	/* The following can be used to direct S/PDIF output to pin 47 (EAPD).
	   snd_ac97_write_cache(ac97, 0x62, snd_ac97_read(ac97, 0x62) | 0x0008); */
}

static const snd_kcontrol_new_t snd_ac97_ymf753_controls_spdif[3] = {
	{
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= SNDRV_CTL_NAME_IEC958("",PLAYBACK,NONE) "Source",
		.info	= snd_ac97_ymf753_spdif_source_info,
		.get	= snd_ac97_ymf753_spdif_source_get,
		.put	= snd_ac97_ymf753_spdif_source_put,
	},
	{
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= SNDRV_CTL_NAME_IEC958("",PLAYBACK,NONE) "Output Pin",
		.info	= snd_ac97_ymf753_spdif_output_pin_info,
		.get	= snd_ac97_ymf753_spdif_output_pin_get,
		.put	= snd_ac97_ymf753_spdif_output_pin_put,
	},
	AC97_SINGLE(SNDRV_CTL_NAME_IEC958("",NONE,NONE) "Mute", AC97_YMF753_DIT_CTRL2, 2, 1, 1)
};

static int patch_yamaha_ymf753_3d(ac97_t * ac97)
{
	snd_kcontrol_t *kctl;
	int err;

	if ((err = snd_ctl_add(ac97->bus->card, kctl = snd_ac97_cnew(&snd_ac97_controls_3d[0], ac97))) < 0)
		return err;
	strcpy(kctl->id.name, "3D Control - Wide");
	kctl->private_value = AC97_3D_CONTROL | (9 << 8) | (7 << 16);
	snd_ac97_write_cache(ac97, AC97_3D_CONTROL, 0x0000);
	if ((err = snd_ctl_add(ac97->bus->card, snd_ac97_cnew(&snd_ac97_ymf753_controls_speaker, ac97))) < 0)
		return err;
	snd_ac97_write_cache(ac97, AC97_YMF753_3D_MODE_SEL, 0x0c00);
	return 0;
}

static int patch_yamaha_ymf753_post_spdif(ac97_t * ac97)
{
	int err;

	if ((err = patch_build_controls(ac97, snd_ac97_ymf753_controls_spdif, ARRAY_SIZE(snd_ac97_ymf753_controls_spdif))) < 0)
		return err;
	return 0;
}

static struct snd_ac97_build_ops patch_yamaha_ymf753_ops = {
	.build_3d	= patch_yamaha_ymf753_3d,
	.build_post_spdif = patch_yamaha_ymf753_post_spdif
};

int patch_yamaha_ymf753(ac97_t * ac97)
{
	/* Patch for Yamaha YMF753, Copyright (c) by David Shust, dshust@shustring.com.
	   This chip has nonstandard and extended behaviour with regard to its S/PDIF output.
	   The AC'97 spec states that the S/PDIF signal is to be output at pin 48.
	   The YMF753 will ouput the S/PDIF signal to pin 43, 47 (EAPD), or 48.
	   By default, no output pin is selected, and the S/PDIF signal is not output.
	   There is also a bit to mute S/PDIF output in a vendor-specific register.
	*/
	ac97->build_ops = &patch_yamaha_ymf753_ops;
	ac97->caps |= AC97_BC_BASS_TREBLE;
	ac97->caps |= 0x04 << 10; /* Yamaha 3D enhancement */
	return 0;
}

/*
 * May 2, 2003 Liam Girdwood <liam.girdwood@wolfsonmicro.com>
 *  removed broken wolfson00 patch.
 *  added support for WM9705,WM9708,WM9709,WM9710,WM9711,WM9712 and WM9717.
 */

int patch_wolfson03(ac97_t * ac97)
{
	/* This is known to work for the ViewSonic ViewPad 1000
	   Randolph Bentson <bentson@holmsjoen.com> */

	// WM9703/9707/9708/9717
	snd_ac97_write_cache(ac97, AC97_WM97XX_FMIXER_VOL, 0x0808);
	snd_ac97_write_cache(ac97, AC97_GENERAL_PURPOSE, 0x8000);
	return 0;
}
  
int patch_wolfson04(ac97_t * ac97)
{
	// WM9704M/9704Q
	// set front and rear mixer volume
	snd_ac97_write_cache(ac97, AC97_WM97XX_FMIXER_VOL, 0x0808);
	snd_ac97_write_cache(ac97, AC97_WM9704_RMIXER_VOL, 0x0808);
	
	// patch for DVD noise
	snd_ac97_write_cache(ac97, AC97_WM9704_TEST, 0x0200);
 
	// init vol
	snd_ac97_write_cache(ac97, AC97_WM9704_RPCM_VOL, 0x0808);
 
	// set rear surround volume
	snd_ac97_write_cache(ac97, AC97_SURROUND_MASTER, 0x0000);
	return 0;
}
  
int patch_wolfson05(ac97_t * ac97)
{
	// WM9705, WM9710
	// set front mixer volume
	snd_ac97_write_cache(ac97, AC97_WM97XX_FMIXER_VOL, 0x0808);
	return 0;
}

int patch_wolfson11(ac97_t * ac97)
{
	// WM9711, WM9712
	// set out3 volume
	snd_ac97_write_cache(ac97, AC97_WM9711_OUT3VOL, 0x0808);
	return 0;
}

/*
 * Tritech codec
 */
int patch_tritech_tr28028(ac97_t * ac97)
{
	snd_ac97_write_cache(ac97, 0x26, 0x0300);
	snd_ac97_write_cache(ac97, 0x26, 0x0000);
	snd_ac97_write_cache(ac97, AC97_SURROUND_MASTER, 0x0000);
	snd_ac97_write_cache(ac97, AC97_SPDIF, 0x0000);
	return 0;
}

/*
 * Sigmatel STAC97xx codecs
 */
static int patch_sigmatel_stac9700_3d(ac97_t * ac97)
{
	snd_kcontrol_t *kctl;
	int err;

	if ((err = snd_ctl_add(ac97->bus->card, kctl = snd_ac97_cnew(&snd_ac97_controls_3d[0], ac97))) < 0)
		return err;
	strcpy(kctl->id.name, "3D Control Sigmatel - Depth");
	kctl->private_value = AC97_3D_CONTROL | (3 << 16);
	snd_ac97_write_cache(ac97, AC97_3D_CONTROL, 0x0000);
	return 0;
}

static int patch_sigmatel_stac9708_3d(ac97_t * ac97)
{
	snd_kcontrol_t *kctl;
	int err;

	if ((err = snd_ctl_add(ac97->bus->card, kctl = snd_ac97_cnew(&snd_ac97_controls_3d[0], ac97))) < 0)
		return err;
	strcpy(kctl->id.name, "3D Control Sigmatel - Depth");
	kctl->private_value = AC97_3D_CONTROL | (3 << 16);
	if ((err = snd_ctl_add(ac97->bus->card, kctl = snd_ac97_cnew(&snd_ac97_controls_3d[0], ac97))) < 0)
		return err;
	strcpy(kctl->id.name, "3D Control Sigmatel - Rear Depth");
	kctl->private_value = AC97_3D_CONTROL | (2 << 8) | (3 << 16);
	snd_ac97_write_cache(ac97, AC97_3D_CONTROL, 0x0000);
	return 0;
}

static const snd_kcontrol_new_t snd_ac97_sigmatel_4speaker =
AC97_SINGLE("Sigmatel 4-Speaker Stereo Playback Switch", AC97_SIGMATEL_DAC2INVERT, 2, 1, 0);

static const snd_kcontrol_new_t snd_ac97_sigmatel_phaseinvert =
AC97_SINGLE("Sigmatel Surround Phase Inversion Playback Switch", AC97_SIGMATEL_DAC2INVERT, 3, 1, 0);

static const snd_kcontrol_new_t snd_ac97_sigmatel_controls[] = {
AC97_SINGLE("Sigmatel DAC 6dB Attenuate", AC97_SIGMATEL_ANALOG, 1, 1, 0),
AC97_SINGLE("Sigmatel ADC 6dB Attenuate", AC97_SIGMATEL_ANALOG, 0, 1, 0)
};

static int patch_sigmatel_stac97xx_specific(ac97_t * ac97)
{
	int err;

	snd_ac97_write_cache(ac97, AC97_SIGMATEL_ANALOG, snd_ac97_read(ac97, AC97_SIGMATEL_ANALOG) & ~0x0003);
	if (snd_ac97_try_bit(ac97, AC97_SIGMATEL_ANALOG, 1))
		if ((err = patch_build_controls(ac97, &snd_ac97_sigmatel_controls[0], 1)) < 0)
			return err;
	if (snd_ac97_try_bit(ac97, AC97_SIGMATEL_ANALOG, 0))
		if ((err = patch_build_controls(ac97, &snd_ac97_sigmatel_controls[1], 1)) < 0)
			return err;
	if (snd_ac97_try_bit(ac97, AC97_SIGMATEL_DAC2INVERT, 2))
		if ((err = patch_build_controls(ac97, &snd_ac97_sigmatel_4speaker, 1)) < 0)
			return err;
	if (snd_ac97_try_bit(ac97, AC97_SIGMATEL_DAC2INVERT, 3))
		if ((err = patch_build_controls(ac97, &snd_ac97_sigmatel_phaseinvert, 1)) < 0)
			return err;
	return 0;
}

static struct snd_ac97_build_ops patch_sigmatel_stac9700_ops = {
	.build_3d	= patch_sigmatel_stac9700_3d,
	.build_specific	= patch_sigmatel_stac97xx_specific
};

static struct snd_ac97_build_ops patch_sigmatel_stac9708_ops = {
	.build_3d	= patch_sigmatel_stac9708_3d,
	.build_specific	= patch_sigmatel_stac97xx_specific
};

int patch_sigmatel_stac9700(ac97_t * ac97)
{
	ac97->build_ops = &patch_sigmatel_stac9700_ops;
	return 0;
}

int patch_sigmatel_stac9708(ac97_t * ac97)
{
	unsigned int codec72, codec6c;

	ac97->build_ops = &patch_sigmatel_stac9708_ops;

	codec72 = snd_ac97_read(ac97, AC97_SIGMATEL_BIAS2) & 0x8000;
	codec6c = snd_ac97_read(ac97, AC97_SIGMATEL_ANALOG);

	if ((codec72==0) && (codec6c==0)) {
		snd_ac97_write_cache(ac97, AC97_SIGMATEL_CIC1, 0xabba);
		snd_ac97_write_cache(ac97, AC97_SIGMATEL_CIC2, 0x1000);
		snd_ac97_write_cache(ac97, AC97_SIGMATEL_BIAS1, 0xabba);
		snd_ac97_write_cache(ac97, AC97_SIGMATEL_BIAS2, 0x0007);
	} else if ((codec72==0x8000) && (codec6c==0)) {
		snd_ac97_write_cache(ac97, AC97_SIGMATEL_CIC1, 0xabba);
		snd_ac97_write_cache(ac97, AC97_SIGMATEL_CIC2, 0x1001);
		snd_ac97_write_cache(ac97, AC97_SIGMATEL_DAC2INVERT, 0x0008);
	} else if ((codec72==0x8000) && (codec6c==0x0080)) {
		/* nothing */
	}
	snd_ac97_write_cache(ac97, AC97_SIGMATEL_MULTICHN, 0x0000);
	return 0;
}

int patch_sigmatel_stac9721(ac97_t * ac97)
{
	ac97->build_ops = &patch_sigmatel_stac9700_ops;
	if (snd_ac97_read(ac97, AC97_SIGMATEL_ANALOG) == 0) {
		// patch for SigmaTel
		snd_ac97_write_cache(ac97, AC97_SIGMATEL_CIC1, 0xabba);
		snd_ac97_write_cache(ac97, AC97_SIGMATEL_CIC2, 0x4000);
		snd_ac97_write_cache(ac97, AC97_SIGMATEL_BIAS1, 0xabba);
		snd_ac97_write_cache(ac97, AC97_SIGMATEL_BIAS2, 0x0002);
	}
	snd_ac97_write_cache(ac97, AC97_SIGMATEL_MULTICHN, 0x0000);
	return 0;
}

int patch_sigmatel_stac9744(ac97_t * ac97)
{
	// patch for SigmaTel
	ac97->build_ops = &patch_sigmatel_stac9700_ops;
	snd_ac97_write_cache(ac97, AC97_SIGMATEL_CIC1, 0xabba);
	snd_ac97_write_cache(ac97, AC97_SIGMATEL_CIC2, 0x0000);	/* is this correct? --jk */
	snd_ac97_write_cache(ac97, AC97_SIGMATEL_BIAS1, 0xabba);
	snd_ac97_write_cache(ac97, AC97_SIGMATEL_BIAS2, 0x0002);
	snd_ac97_write_cache(ac97, AC97_SIGMATEL_MULTICHN, 0x0000);
	return 0;
}

int patch_sigmatel_stac9756(ac97_t * ac97)
{
	// patch for SigmaTel
	ac97->build_ops = &patch_sigmatel_stac9700_ops;
	snd_ac97_write_cache(ac97, AC97_SIGMATEL_CIC1, 0xabba);
	snd_ac97_write_cache(ac97, AC97_SIGMATEL_CIC2, 0x0000);	/* is this correct? --jk */
	snd_ac97_write_cache(ac97, AC97_SIGMATEL_BIAS1, 0xabba);
	snd_ac97_write_cache(ac97, AC97_SIGMATEL_BIAS2, 0x0002);
	snd_ac97_write_cache(ac97, AC97_SIGMATEL_MULTICHN, 0x0000);
	return 0;
}

static int snd_ac97_stac9758_output_jack_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t *uinfo)
{
	static char *texts[5] = { "Input/Disabled", "Front Output",
		"Rear Output", "Center/LFE Output", "Mixer Output" };

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 5;
	if (uinfo->value.enumerated.item > 4)
		uinfo->value.enumerated.item = 4;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_ac97_stac9758_output_jack_get(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t* ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	int shift = kcontrol->private_value;
	unsigned short val;

	val = ac97->regs[AC97_SIGMATEL_OUTSEL];
	if (!((val >> shift) & 4))
		ucontrol->value.enumerated.item[0] = 0;
	else
		ucontrol->value.enumerated.item[0] = 1 + ((val >> shift) & 3);
	return 0;
}

static int snd_ac97_stac9758_output_jack_put(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t *ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	int shift = kcontrol->private_value;
	unsigned short val;

	if (ucontrol->value.enumerated.item[0] > 4)
		return -EINVAL;
	if (ucontrol->value.enumerated.item[0] == 0)
		val = 0;
	else
		val = 4 | (ucontrol->value.enumerated.item[0] - 1);
	return snd_ac97_update_bits(ac97, AC97_SIGMATEL_OUTSEL,
				    7 << shift, val << shift);
}

static int snd_ac97_stac9758_input_jack_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t *uinfo)
{
	static char *texts[7] = { "Mic2 Jack", "Mic1 Jack", "Line In Jack",
		"Front Jack", "Rear Jack", "Center/LFE Jack", "Mute" };

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 7;
	if (uinfo->value.enumerated.item > 6)
		uinfo->value.enumerated.item = 6;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_ac97_stac9758_input_jack_get(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t* ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	int shift = kcontrol->private_value;
	unsigned short val;

	val = ac97->regs[AC97_SIGMATEL_INSEL];
	ucontrol->value.enumerated.item[0] = (val >> shift) & 7;
	return 0;
}

static int snd_ac97_stac9758_input_jack_put(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t *ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	int shift = kcontrol->private_value;

	return snd_ac97_update_bits(ac97, AC97_SIGMATEL_INSEL, 7 << shift,
				    ucontrol->value.enumerated.item[0] << shift);
}

static int snd_ac97_stac9758_phonesel_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t *uinfo)
{
	static char *texts[3] = { "None", "Front Jack", "Rear Jack" };

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 3;
	if (uinfo->value.enumerated.item > 2)
		uinfo->value.enumerated.item = 2;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_ac97_stac9758_phonesel_get(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t* ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);

	ucontrol->value.enumerated.item[0] = ac97->regs[AC97_SIGMATEL_IOMISC] & 3;
	return 0;
}

static int snd_ac97_stac9758_phonesel_put(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t *ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);

	return snd_ac97_update_bits(ac97, AC97_SIGMATEL_IOMISC, 3,
				    ucontrol->value.enumerated.item[0]);
}

#define STAC9758_OUTPUT_JACK(xname, shift) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.info = snd_ac97_stac9758_output_jack_info, \
	.get = snd_ac97_stac9758_output_jack_get, \
	.put = snd_ac97_stac9758_output_jack_put, \
	.private_value = shift }
#define STAC9758_INPUT_JACK(xname, shift) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.info = snd_ac97_stac9758_input_jack_info, \
	.get = snd_ac97_stac9758_input_jack_get, \
	.put = snd_ac97_stac9758_input_jack_put, \
	.private_value = shift }
static const snd_kcontrol_new_t snd_ac97_sigmatel_stac9758_controls[] = {
	STAC9758_OUTPUT_JACK("Mic1 Jack", 1),
	STAC9758_OUTPUT_JACK("LineIn Jack", 4),
	STAC9758_OUTPUT_JACK("Front Jack", 7),
	STAC9758_OUTPUT_JACK("Rear Jack", 10),
	STAC9758_OUTPUT_JACK("Center/LFE Jack", 13),
	STAC9758_INPUT_JACK("Mic Input Source", 0),
	STAC9758_INPUT_JACK("Line Input Source", 8),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Headphone Amp",
		.info = snd_ac97_stac9758_phonesel_info,
		.get = snd_ac97_stac9758_phonesel_get,
		.put = snd_ac97_stac9758_phonesel_put
	},
	AC97_SINGLE("Exchange Center/LFE", AC97_SIGMATEL_IOMISC, 4, 1, 0),
	AC97_SINGLE("Headphone +3dB Boost", AC97_SIGMATEL_IOMISC, 8, 1, 0)
};

static int patch_sigmatel_stac9758_specific(ac97_t *ac97)
{
	int err;

	err = patch_sigmatel_stac97xx_specific(ac97);
	if (err < 0)
		return err;
	err = patch_build_controls(ac97, snd_ac97_sigmatel_stac9758_controls,
				   ARRAY_SIZE(snd_ac97_sigmatel_stac9758_controls));
	if (err < 0)
		return err;
	return 0;
}

static struct snd_ac97_build_ops patch_sigmatel_stac9758_ops = {
	.build_3d	= patch_sigmatel_stac9700_3d,
	.build_specific	= patch_sigmatel_stac9758_specific
};

int patch_sigmatel_stac9758(ac97_t * ac97)
{
	static unsigned short regs[4] = {
		AC97_SIGMATEL_OUTSEL,
		AC97_SIGMATEL_IOMISC,
		AC97_SIGMATEL_INSEL,
		AC97_SIGMATEL_VARIOUS
	};
	static unsigned short def_regs[4] = {
		/* OUTSEL */ 0xd794,
		/* IOMISC */ 0x2001,
		/* INSEL */ 0x0201,
		/* VARIOUS */ 0x0040
	};
	static unsigned short m675_regs[4] = {
		/* OUTSEL */ 0x9040,
		/* IOMISC */ 0x2102,
		/* INSEL */ 0x0203,
		/* VARIOUS */ 0x0041
	};
	unsigned short *pregs = def_regs;
	int i;

	/* Gateway M675 notebook */
	if (ac97->pci && 
	    ac97->subsystem_vendor == 0x107b &&
	    ac97->subsystem_device == 0x0601)
	    	pregs = m675_regs;

	// patch for SigmaTel
	ac97->build_ops = &patch_sigmatel_stac9758_ops;
	for (i = 0; i < 4; i++)
		snd_ac97_write_cache(ac97, regs[i], pregs[i]);

	ac97->flags |= AC97_STEREO_MUTES;
	return 0;
}

/*
 * Cirrus Logic CS42xx codecs
 */
static const snd_kcontrol_new_t snd_ac97_cirrus_controls_spdif[2] = {
	AC97_SINGLE(SNDRV_CTL_NAME_IEC958("",PLAYBACK,SWITCH), AC97_CSR_SPDIF, 15, 1, 0),
	AC97_SINGLE(SNDRV_CTL_NAME_IEC958("",PLAYBACK,NONE) "AC97-SPSA", AC97_CSR_ACMODE, 0, 3, 0)
};

static int patch_cirrus_build_spdif(ac97_t * ac97)
{
	int err;

	if ((err = patch_build_controls(ac97, &snd_ac97_controls_spdif[0], 3)) < 0)
		return err;
	if ((err = patch_build_controls(ac97, &snd_ac97_cirrus_controls_spdif[0], 1)) < 0)
		return err;
	switch (ac97->id & AC97_ID_CS_MASK) {
	case AC97_ID_CS4205:
		if ((err = patch_build_controls(ac97, &snd_ac97_cirrus_controls_spdif[1], 1)) < 0)
			return err;
		break;
	}
	/* set default PCM S/PDIF params */
	/* consumer,PCM audio,no copyright,no preemphasis,PCM coder,original,48000Hz */
	snd_ac97_write_cache(ac97, AC97_CSR_SPDIF, 0x0a20);
	return 0;
}

static struct snd_ac97_build_ops patch_cirrus_ops = {
	.build_spdif = patch_cirrus_build_spdif
};

int patch_cirrus_spdif(ac97_t * ac97)
{
	/* Basically, the cs4201/cs4205/cs4297a has non-standard sp/dif registers.
	   WHY CAN'T ANYONE FOLLOW THE BLOODY SPEC?  *sigh*
	   - sp/dif EA ID is not set, but sp/dif is always present.
	   - enable/disable is spdif register bit 15.
	   - sp/dif control register is 0x68.  differs from AC97:
	   - valid is bit 14 (vs 15)
	   - no DRS
	   - only 44.1/48k [00 = 48, 01=44,1] (AC97 is 00=44.1, 10=48)
	   - sp/dif ssource select is in 0x5e bits 0,1.
	*/

	ac97->build_ops = &patch_cirrus_ops;
	ac97->flags |= AC97_CS_SPDIF; 
	ac97->rates[AC97_RATES_SPDIF] &= ~SNDRV_PCM_RATE_32000;
        ac97->ext_id |= AC97_EI_SPDIF;	/* force the detection of spdif */
	snd_ac97_write_cache(ac97, AC97_CSR_ACMODE, 0x0080);
	return 0;
}

int patch_cirrus_cs4299(ac97_t * ac97)
{
	/* force the detection of PC Beep */
	ac97->flags |= AC97_HAS_PC_BEEP;
	
	return patch_cirrus_spdif(ac97);
}

/*
 * Conexant codecs
 */
static const snd_kcontrol_new_t snd_ac97_conexant_controls_spdif[1] = {
	AC97_SINGLE(SNDRV_CTL_NAME_IEC958("",PLAYBACK,SWITCH), AC97_CXR_AUDIO_MISC, 3, 1, 0),
};

static int patch_conexant_build_spdif(ac97_t * ac97)
{
	int err;

	if ((err = patch_build_controls(ac97, &snd_ac97_controls_spdif[0], 3)) < 0)
		return err;
	if ((err = patch_build_controls(ac97, &snd_ac97_conexant_controls_spdif[0], 1)) < 0)
		return err;
	/* set default PCM S/PDIF params */
	/* consumer,PCM audio,no copyright,no preemphasis,PCM coder,original,48000Hz */
	snd_ac97_write_cache(ac97, AC97_CXR_AUDIO_MISC,
			     snd_ac97_read(ac97, AC97_CXR_AUDIO_MISC) & ~(AC97_CXR_SPDIFEN|AC97_CXR_COPYRGT|AC97_CXR_SPDIF_MASK));
	return 0;
}

static struct snd_ac97_build_ops patch_conexant_ops = {
	.build_spdif = patch_conexant_build_spdif
};

int patch_conexant(ac97_t * ac97)
{
	ac97->build_ops = &patch_conexant_ops;
	ac97->flags |= AC97_CX_SPDIF;
        ac97->ext_id |= AC97_EI_SPDIF;	/* force the detection of spdif */
	return 0;
}

/*
 * Analog Device AD18xx, AD19xx codecs
 */
int patch_ad1819(ac97_t * ac97)
{
	unsigned short scfg;

	// patch for Analog Devices
	scfg = snd_ac97_read(ac97, AC97_AD_SERIAL_CFG);
	snd_ac97_write_cache(ac97, AC97_AD_SERIAL_CFG, scfg | 0x7000); /* select all codecs */
	return 0;
}

static unsigned short patch_ad1881_unchained(ac97_t * ac97, int idx, unsigned short mask)
{
	unsigned short val;

	// test for unchained codec
	snd_ac97_update_bits(ac97, AC97_AD_SERIAL_CFG, 0x7000, mask);
	snd_ac97_write_cache(ac97, AC97_AD_CODEC_CFG, 0x0000);	/* ID0C, ID1C, SDIE = off */
	val = snd_ac97_read(ac97, AC97_VENDOR_ID2);
	if ((val & 0xff40) != 0x5340)
		return 0;
	ac97->spec.ad18xx.unchained[idx] = mask;
	ac97->spec.ad18xx.id[idx] = val;
	ac97->spec.ad18xx.codec_cfg[idx] = 0x0000;
	return mask;
}

static int patch_ad1881_chained1(ac97_t * ac97, int idx, unsigned short codec_bits)
{
	static int cfg_bits[3] = { 1<<12, 1<<14, 1<<13 };
	unsigned short val;
	
	snd_ac97_update_bits(ac97, AC97_AD_SERIAL_CFG, 0x7000, cfg_bits[idx]);
	snd_ac97_write_cache(ac97, AC97_AD_CODEC_CFG, 0x0004);	// SDIE
	val = snd_ac97_read(ac97, AC97_VENDOR_ID2);
	if ((val & 0xff40) != 0x5340)
		return 0;
	if (codec_bits)
		snd_ac97_write_cache(ac97, AC97_AD_CODEC_CFG, codec_bits);
	ac97->spec.ad18xx.chained[idx] = cfg_bits[idx];
	ac97->spec.ad18xx.id[idx] = val;
	ac97->spec.ad18xx.codec_cfg[idx] = codec_bits ? codec_bits : 0x0004;
	return 1;
}

static void patch_ad1881_chained(ac97_t * ac97, int unchained_idx, int cidx1, int cidx2)
{
	// already detected?
	if (ac97->spec.ad18xx.unchained[cidx1] || ac97->spec.ad18xx.chained[cidx1])
		cidx1 = -1;
	if (ac97->spec.ad18xx.unchained[cidx2] || ac97->spec.ad18xx.chained[cidx2])
		cidx2 = -1;
	if (cidx1 < 0 && cidx2 < 0)
		return;
	// test for chained codecs
	snd_ac97_update_bits(ac97, AC97_AD_SERIAL_CFG, 0x7000,
			     ac97->spec.ad18xx.unchained[unchained_idx]);
	snd_ac97_write_cache(ac97, AC97_AD_CODEC_CFG, 0x0002);		// ID1C
	ac97->spec.ad18xx.codec_cfg[unchained_idx] = 0x0002;
	if (cidx1 >= 0) {
		if (patch_ad1881_chained1(ac97, cidx1, 0x0006))		// SDIE | ID1C
			patch_ad1881_chained1(ac97, cidx2, 0);
		else if (patch_ad1881_chained1(ac97, cidx2, 0x0006))	// SDIE | ID1C
			patch_ad1881_chained1(ac97, cidx1, 0);
	} else if (cidx2 >= 0) {
		patch_ad1881_chained1(ac97, cidx2, 0);
	}
}

int patch_ad1881(ac97_t * ac97)
{
	static const char cfg_idxs[3][2] = {
		{2, 1},
		{0, 2},
		{0, 1}
	};
	
	// patch for Analog Devices
	unsigned short codecs[3];
	unsigned short val;
	int idx, num;

	init_MUTEX(&ac97->spec.ad18xx.mutex);

	val = snd_ac97_read(ac97, AC97_AD_SERIAL_CFG);
	snd_ac97_write_cache(ac97, AC97_AD_SERIAL_CFG, val);
	codecs[0] = patch_ad1881_unchained(ac97, 0, (1<<12));
	codecs[1] = patch_ad1881_unchained(ac97, 1, (1<<14));
	codecs[2] = patch_ad1881_unchained(ac97, 2, (1<<13));

	snd_runtime_check(codecs[0] | codecs[1] | codecs[2], goto __end);

	for (idx = 0; idx < 3; idx++)
		if (ac97->spec.ad18xx.unchained[idx])
			patch_ad1881_chained(ac97, idx, cfg_idxs[idx][0], cfg_idxs[idx][1]);

	if (ac97->spec.ad18xx.id[1]) {
		ac97->flags |= AC97_AD_MULTI;
		ac97->scaps |= AC97_SCAP_SURROUND_DAC;
	}
	if (ac97->spec.ad18xx.id[2]) {
		ac97->flags |= AC97_AD_MULTI;
		ac97->scaps |= AC97_SCAP_CENTER_LFE_DAC;
	}

      __end:
	/* select all codecs */
	snd_ac97_update_bits(ac97, AC97_AD_SERIAL_CFG, 0x7000, 0x7000);
	/* check if only one codec is present */
	for (idx = num = 0; idx < 3; idx++)
		if (ac97->spec.ad18xx.id[idx])
			num++;
	if (num == 1) {
		/* ok, deselect all ID bits */
		snd_ac97_write_cache(ac97, AC97_AD_CODEC_CFG, 0x0000);
		ac97->spec.ad18xx.codec_cfg[0] = 
			ac97->spec.ad18xx.codec_cfg[1] = 
			ac97->spec.ad18xx.codec_cfg[2] = 0x0000;
	}
	/* required for AD1886/AD1885 combination */
	ac97->ext_id = snd_ac97_read(ac97, AC97_EXTENDED_ID);
	if (ac97->spec.ad18xx.id[0]) {
		ac97->id &= 0xffff0000;
		ac97->id |= ac97->spec.ad18xx.id[0];
	}
	return 0;
}

static const snd_kcontrol_new_t snd_ac97_controls_ad1885[] = {
	AC97_SINGLE("Digital Mono Direct", AC97_AD_MISC, 11, 1, 0),
	/* AC97_SINGLE("Digital Audio Mode", AC97_AD_MISC, 12, 1, 0), */ /* seems problematic */
	AC97_SINGLE("Low Power Mixer", AC97_AD_MISC, 14, 1, 0),
	AC97_SINGLE("Zero Fill DAC", AC97_AD_MISC, 15, 1, 0),
};

static int patch_ad1885_specific(ac97_t * ac97)
{
	int err;

	if ((err = patch_build_controls(ac97, snd_ac97_controls_ad1885, ARRAY_SIZE(snd_ac97_controls_ad1885))) < 0)
		return err;
	return 0;
}

static struct snd_ac97_build_ops patch_ad1885_build_ops = {
	.build_specific = &patch_ad1885_specific
};

int patch_ad1885(ac97_t * ac97)
{
	unsigned short jack;

	patch_ad1881(ac97);
	/* This is required to deal with the Intel D815EEAL2 */
	/* i.e. Line out is actually headphone out from codec */

	/* turn off jack sense bits D8 & D9 */
	jack = snd_ac97_read(ac97, AC97_AD_JACK_SPDIF);
	snd_ac97_write_cache(ac97, AC97_AD_JACK_SPDIF, jack | 0x0300);

	/* set default */
	snd_ac97_write_cache(ac97, AC97_AD_MISC, 0x0404);

	ac97->build_ops = &patch_ad1885_build_ops;
	return 0;
}

int patch_ad1886(ac97_t * ac97)
{
	patch_ad1881(ac97);
	/* Presario700 workaround */
	/* for Jack Sense/SPDIF Register misetting causing */
	snd_ac97_write_cache(ac97, AC97_AD_JACK_SPDIF, 0x0010);
	return 0;
}

/* MISC bits */
#define AC97_AD198X_MBC		0x0003	/* mic boost */
#define AC97_AD198X_MBC_20	0x0000	/* +20dB */
#define AC97_AD198X_MBC_10	0x0001	/* +10dB */
#define AC97_AD198X_MBC_30	0x0002	/* +30dB */
#define AC97_AD198X_VREFD	0x0004	/* VREF high-Z */
#define AC97_AD198X_VREFH	0x0008	/* 2.25V, 3.7V */
#define AC97_AD198X_VREF_0	0x000c	/* 0V */
#define AC97_AD198X_SRU		0x0010	/* sample rate unlock */
#define AC97_AD198X_LOSEL	0x0020	/* LINE_OUT amplifiers input select */
#define AC97_AD198X_2MIC	0x0040	/* 2-channel mic select */
#define AC97_AD198X_SPRD	0x0080	/* SPREAD enable */
#define AC97_AD198X_DMIX0	0x0100	/* downmix mode: 0 = 6-to-4, 1 = 6-to-2 downmix */
#define AC97_AD198X_DMIX1	0x0200	/* downmix mode: 1 = enabled */
#define AC97_AD198X_HPSEL	0x0400	/* headphone amplifier input select */
#define AC97_AD198X_CLDIS	0x0800	/* center/lfe disable */
#define AC97_AD198X_LODIS	0x1000	/* LINE_OUT disable */
#define AC97_AD198X_MSPLT	0x2000	/* mute split */
#define AC97_AD198X_AC97NC	0x4000	/* AC97 no compatible mode */
#define AC97_AD198X_DACZ	0x8000	/* DAC zero-fill mode */


static int snd_ac97_ad198x_spdif_source_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	static char *texts[2] = { "AC-Link", "A/D Converter" };

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 2;
	if (uinfo->value.enumerated.item > 1)
		uinfo->value.enumerated.item = 1;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_ac97_ad198x_spdif_source_get(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	unsigned short val;

	val = ac97->regs[AC97_AD_SERIAL_CFG];
	ucontrol->value.enumerated.item[0] = (val >> 2) & 1;
	return 0;
}

static int snd_ac97_ad198x_spdif_source_put(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	unsigned short val;

	if (ucontrol->value.enumerated.item[0] > 1)
		return -EINVAL;
	val = ucontrol->value.enumerated.item[0] << 2;
	return snd_ac97_update_bits(ac97, AC97_AD_SERIAL_CFG, 0x0004, val);
}

static const snd_kcontrol_new_t snd_ac97_ad198x_spdif_source = {
	.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
	.name	= SNDRV_CTL_NAME_IEC958("",PLAYBACK,NONE) "Source",
	.info	= snd_ac97_ad198x_spdif_source_info,
	.get	= snd_ac97_ad198x_spdif_source_get,
	.put	= snd_ac97_ad198x_spdif_source_put,
};

static int patch_ad198x_post_spdif(ac97_t * ac97)
{
 	return patch_build_controls(ac97, &snd_ac97_ad198x_spdif_source, 1);
}

static struct snd_ac97_build_ops patch_ad1981a_build_ops = {
	.build_post_spdif = patch_ad198x_post_spdif
};

int patch_ad1981a(ac97_t *ac97)
{
	patch_ad1881(ac97);
	ac97->build_ops = &patch_ad1981a_build_ops;
	snd_ac97_update_bits(ac97, AC97_AD_MISC, AC97_AD198X_MSPLT, AC97_AD198X_MSPLT);
	ac97->flags |= AC97_STEREO_MUTES;
	return 0;
}

static const snd_kcontrol_new_t snd_ac97_ad198x_2cmic =
AC97_SINGLE("Stereo Mic", AC97_AD_MISC, 6, 1, 0);

static int patch_ad1981b_specific(ac97_t *ac97)
{
	return patch_build_controls(ac97, &snd_ac97_ad198x_2cmic, 1);
}

static struct snd_ac97_build_ops patch_ad1981b_build_ops = {
	.build_post_spdif = patch_ad198x_post_spdif,
	.build_specific = patch_ad1981b_specific
};

int patch_ad1981b(ac97_t *ac97)
{
	patch_ad1881(ac97);
	ac97->build_ops = &patch_ad1981b_build_ops;
	snd_ac97_update_bits(ac97, AC97_AD_MISC, AC97_AD198X_MSPLT, AC97_AD198X_MSPLT);
	ac97->flags |= AC97_STEREO_MUTES;
	return 0;
}

static int snd_ac97_ad1888_lohpsel_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int snd_ac97_ad1888_lohpsel_get(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t* ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	unsigned short val;

	val = ac97->regs[AC97_AD_MISC];
	ucontrol->value.integer.value[0] = !(val & AC97_AD198X_LOSEL);
	return 0;
}

static int snd_ac97_ad1888_lohpsel_put(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t *ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	unsigned short val;

	val = !ucontrol->value.integer.value[0]
		? (AC97_AD198X_LOSEL | AC97_AD198X_HPSEL) : 0;
	return snd_ac97_update_bits(ac97, AC97_AD_MISC,
				    AC97_AD198X_LOSEL | AC97_AD198X_HPSEL, val);
}

static int snd_ac97_ad1888_downmix_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t *uinfo)
{
	static char *texts[3] = {"Off", "6 -> 4", "6 -> 2"};

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 3;
	if (uinfo->value.enumerated.item > 2)
		uinfo->value.enumerated.item = 2;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_ac97_ad1888_downmix_get(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t* ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	unsigned short val;

	val = ac97->regs[AC97_AD_MISC];
	if (!(val & AC97_AD198X_DMIX1))
		ucontrol->value.enumerated.item[0] = 0;
	else
		ucontrol->value.enumerated.item[0] = 1 + ((val >> 8) & 1);
	return 0;
}

static int snd_ac97_ad1888_downmix_put(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t *ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	unsigned short val;

	if (ucontrol->value.enumerated.item[0] > 2)
		return -EINVAL;
	if (ucontrol->value.enumerated.item[0] == 0)
		val = 0;
	else
		val = AC97_AD198X_DMIX1 |
			((ucontrol->value.enumerated.item[0] - 1) << 8);
	return snd_ac97_update_bits(ac97, AC97_AD_MISC,
				    AC97_AD198X_DMIX0 | AC97_AD198X_DMIX1, val);
}

static const snd_kcontrol_new_t snd_ac97_ad1888_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Exchange Front/Surround",
		.info = snd_ac97_ad1888_lohpsel_info,
		.get = snd_ac97_ad1888_lohpsel_get,
		.put = snd_ac97_ad1888_lohpsel_put
	},
	AC97_SINGLE("Spread Front to Surround and Center/LFE", AC97_AD_MISC, 7, 1, 0),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Downmix",
		.info = snd_ac97_ad1888_downmix_info,
		.get = snd_ac97_ad1888_downmix_get,
		.put = snd_ac97_ad1888_downmix_put
	},
	AC97_SINGLE("Surround Jack as Input", AC97_AD_MISC, 12, 1, 0),
	AC97_SINGLE("Center/LFE Jack as Input", AC97_AD_MISC, 11, 1, 0),
};

static int patch_ad1888_specific(ac97_t *ac97)
{
	/* rename 0x04 as "Master" and 0x02 as "Master Surround" */
	snd_ac97_rename_ctl(ac97, "Master Playback Switch", "Master Surround Playback Switch");
	snd_ac97_rename_ctl(ac97, "Master Playback Volume", "Master Surround Playback Volume");
	snd_ac97_rename_ctl(ac97, "Headphone Playback Switch", "Master Playback Switch");
	snd_ac97_rename_ctl(ac97, "Headphone Playback Volume", "Master Playback Volume");
	return patch_build_controls(ac97, snd_ac97_ad1888_controls, ARRAY_SIZE(snd_ac97_ad1888_controls));
}

static struct snd_ac97_build_ops patch_ad1888_build_ops = {
	.build_post_spdif = patch_ad198x_post_spdif,
	.build_specific = patch_ad1888_specific
};

int patch_ad1888(ac97_t * ac97)
{
	unsigned short misc;
	
	patch_ad1881(ac97);
	ac97->build_ops = &patch_ad1888_build_ops;
	/* Switch FRONT/SURROUND LINE-OUT/HP-OUT default connection */
	/* it seems that most vendors connect line-out connector to headphone out of AC'97 */
	/* AD-compatible mode */
	/* Stereo mutes enabled */
	misc = snd_ac97_read(ac97, AC97_AD_MISC);
	snd_ac97_write_cache(ac97, AC97_AD_MISC, misc |
			     AC97_AD198X_LOSEL |
			     AC97_AD198X_HPSEL |
			     AC97_AD198X_MSPLT |
			     AC97_AD198X_AC97NC);
	ac97->flags |= AC97_STEREO_MUTES;
	return 0;
}

static int patch_ad1980_specific(ac97_t *ac97)
{
	int err;

	if ((err = patch_ad1888_specific(ac97)) < 0)
		return err;
	return patch_build_controls(ac97, &snd_ac97_ad198x_2cmic, 1);
}

static struct snd_ac97_build_ops patch_ad1980_build_ops = {
	.build_post_spdif = patch_ad198x_post_spdif,
	.build_specific = patch_ad1980_specific
};

int patch_ad1980(ac97_t * ac97)
{
	patch_ad1888(ac97);
	ac97->build_ops = &patch_ad1980_build_ops;
	return 0;
}

static const snd_kcontrol_new_t snd_ac97_ad1985_controls[] = {
	AC97_SINGLE("Center/LFE Jack as Mic", AC97_AD_SERIAL_CFG, 9, 1, 0),
	AC97_SINGLE("Exchange Center/LFE", AC97_AD_SERIAL_CFG, 3, 1, 0)
};

static int patch_ad1985_specific(ac97_t *ac97)
{
	int err;

	if ((err = patch_ad1980_specific(ac97)) < 0)
		return err;
	return patch_build_controls(ac97, snd_ac97_ad1985_controls, ARRAY_SIZE(snd_ac97_ad1985_controls));
}

static struct snd_ac97_build_ops patch_ad1985_build_ops = {
	.build_post_spdif = patch_ad198x_post_spdif,
	.build_specific = patch_ad1985_specific
};

int patch_ad1985(ac97_t * ac97)
{
	unsigned short misc;
	
	patch_ad1881(ac97);
	ac97->build_ops = &patch_ad1985_build_ops;
	misc = snd_ac97_read(ac97, AC97_AD_MISC);
	/* switch front/surround line-out/hp-out */
	/* center/LFE, mic in 3.75V mode */
	/* AD-compatible mode */
	/* Stereo mutes enabled */
	/* in accordance with ADI driver: misc | 0x5c28 */
	snd_ac97_write_cache(ac97, AC97_AD_MISC, misc |
			     AC97_AD198X_VREFH |
			     AC97_AD198X_LOSEL |
			     AC97_AD198X_HPSEL |
			     AC97_AD198X_CLDIS |
			     AC97_AD198X_LODIS |
			     AC97_AD198X_MSPLT |
			     AC97_AD198X_AC97NC);
	ac97->flags |= AC97_STEREO_MUTES;
	/* on AD1985 rev. 3, AC'97 revision bits are zero */
	ac97->ext_id = (ac97->ext_id & ~AC97_EI_REV_MASK) | AC97_EI_REV_23;
	return 0;
}

/*
 * realtek ALC65x codecs
 */
static int snd_ac97_alc650_mic_get(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t * ucontrol)
{
        ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
        ucontrol->value.integer.value[0] = (ac97->regs[AC97_ALC650_MULTICH] >> 10) & 1;
        return 0;
}

static int snd_ac97_alc650_mic_put(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t * ucontrol)
{
        ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	int change, val;
	val = !!(snd_ac97_read(ac97, AC97_ALC650_MULTICH) & (1 << 10));
	change = (ucontrol->value.integer.value[0] != val);
	if (change) {
		/* disable/enable vref */
		snd_ac97_update_bits(ac97, AC97_ALC650_CLOCK, 1 << 12,
				     ucontrol->value.integer.value[0] ? (1 << 12) : 0);
		/* turn on/off center-on-mic */
		snd_ac97_update_bits(ac97, AC97_ALC650_MULTICH, 1 << 10,
				     ucontrol->value.integer.value[0] ? (1 << 10) : 0);
		/* GPIO0 high for mic */
		snd_ac97_update_bits(ac97, AC97_ALC650_GPIO_STATUS, 0x100,
				     ucontrol->value.integer.value[0] ? 0 : 0x100);
        }
        return change;
}

static const snd_kcontrol_new_t snd_ac97_controls_alc650[] = {
	AC97_SINGLE("Duplicate Front", AC97_ALC650_MULTICH, 0, 1, 0),
	AC97_SINGLE("Surround Down Mix", AC97_ALC650_MULTICH, 1, 1, 0),
	AC97_SINGLE("Center/LFE Down Mix", AC97_ALC650_MULTICH, 2, 1, 0),
	AC97_SINGLE("Exchange Center/LFE", AC97_ALC650_MULTICH, 3, 1, 0),
	/* 4: Analog Input To Surround */
	/* 5: Analog Input To Center/LFE */
	/* 6: Independent Master Volume Right */
	/* 7: Independent Master Volume Left */
	/* 8: reserved */
	AC97_SINGLE("Line-In As Surround", AC97_ALC650_MULTICH, 9, 1, 0),
	/* 10: mic, see below */
	/* 11-13: in IEC958 controls */
	AC97_SINGLE("Swap Surround Slot", AC97_ALC650_MULTICH, 14, 1, 0),
#if 0 /* always set in patch_alc650 */
	AC97_SINGLE("IEC958 Input Clock Enable", AC97_ALC650_CLOCK, 0, 1, 0),
	AC97_SINGLE("IEC958 Input Pin Enable", AC97_ALC650_CLOCK, 1, 1, 0),
	AC97_SINGLE("Surround DAC Switch", AC97_ALC650_SURR_DAC_VOL, 15, 1, 1),
	AC97_DOUBLE("Surround DAC Volume", AC97_ALC650_SURR_DAC_VOL, 8, 0, 31, 1),
	AC97_SINGLE("Center/LFE DAC Switch", AC97_ALC650_LFE_DAC_VOL, 15, 1, 1),
	AC97_DOUBLE("Center/LFE DAC Volume", AC97_ALC650_LFE_DAC_VOL, 8, 0, 31, 1),
#endif
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Mic As Center/LFE",
		.info = snd_ac97_info_single,
		.get = snd_ac97_alc650_mic_get,
		.put = snd_ac97_alc650_mic_put,
		.private_value = AC97_SINGLE_VALUE(0, 0, 1, 0) /* only mask needed */
	},
};

static const snd_kcontrol_new_t snd_ac97_spdif_controls_alc650[] = {
        AC97_SINGLE("IEC958 Capture Switch", AC97_ALC650_MULTICH, 11, 1, 0),
        AC97_SINGLE("Analog to IEC958 Output", AC97_ALC650_MULTICH, 12, 1, 0),
        AC97_SINGLE("IEC958 Input Monitor", AC97_ALC650_MULTICH, 13, 1, 0),
};

static int patch_alc650_specific(ac97_t * ac97)
{
	int err;

	if ((err = patch_build_controls(ac97, snd_ac97_controls_alc650, ARRAY_SIZE(snd_ac97_controls_alc650))) < 0)
		return err;
	if (ac97->ext_id & AC97_EI_SPDIF) {
		if ((err = patch_build_controls(ac97, snd_ac97_spdif_controls_alc650, ARRAY_SIZE(snd_ac97_spdif_controls_alc650))) < 0)
			return err;
	}
	return 0;
}

static struct snd_ac97_build_ops patch_alc650_ops = {
	.build_specific	= patch_alc650_specific
};

int patch_alc650(ac97_t * ac97)
{
	unsigned short val;

	ac97->build_ops = &patch_alc650_ops;

	/* revision E or F */
	/* FIXME: what about revision D ? */
	ac97->spec.dev_flags = (ac97->id == 0x414c4722 ||
				ac97->id == 0x414c4723);

	/* enable AC97_ALC650_GPIO_SETUP, AC97_ALC650_CLOCK for R/W */
	snd_ac97_write_cache(ac97, AC97_ALC650_GPIO_STATUS, 
		snd_ac97_read(ac97, AC97_ALC650_GPIO_STATUS) | 0x8000);

	/* Enable SPDIF-IN only on Rev.E and above */
	val = snd_ac97_read(ac97, AC97_ALC650_CLOCK);
	/* SPDIF IN with pin 47 */
	if (ac97->spec.dev_flags)
		val |= 0x03; /* enable */
	else
		val &= ~0x03; /* disable */
	snd_ac97_write_cache(ac97, AC97_ALC650_CLOCK, val);

	/* set default: slot 3,4,7,8,6,9
	   spdif-in monitor off, analog-spdif off, spdif-in off
	   center on mic off, surround on line-in off
	   downmix off, duplicate front off
	*/
	snd_ac97_write_cache(ac97, AC97_ALC650_MULTICH, 0);

	/* set GPIO0 for mic bias */
	/* GPIO0 pin output, no interrupt, high */
	snd_ac97_write_cache(ac97, AC97_ALC650_GPIO_SETUP,
			     snd_ac97_read(ac97, AC97_ALC650_GPIO_SETUP) | 0x01);
	snd_ac97_write_cache(ac97, AC97_ALC650_GPIO_STATUS,
			     (snd_ac97_read(ac97, AC97_ALC650_GPIO_STATUS) | 0x100) & ~0x10);

	/* full DAC volume */
	snd_ac97_write_cache(ac97, AC97_ALC650_SURR_DAC_VOL, 0x0808);
	snd_ac97_write_cache(ac97, AC97_ALC650_LFE_DAC_VOL, 0x0808);
	return 0;
}

static int snd_ac97_alc655_mic_get(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t * ucontrol)
{
        ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
        ucontrol->value.integer.value[0] = (ac97->regs[AC97_ALC650_MULTICH] >> 10) & 1;
        return 0;
}

static int snd_ac97_alc655_mic_put(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t * ucontrol)
{
        ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
        int change;

	/* misc control; vrefout disable */
	snd_ac97_update_bits(ac97, AC97_ALC650_CLOCK, 1 << 12,
			     ucontrol->value.integer.value[0] ? (1 << 12) : 0);
	change = snd_ac97_update_bits(ac97, AC97_ALC650_MULTICH, 1 << 10,
				      ucontrol->value.integer.value[0] ? (1 << 10) : 0);
	return change;
}


static const snd_kcontrol_new_t snd_ac97_controls_alc655[] = {
	AC97_SINGLE("Duplicate Front", AC97_ALC650_MULTICH, 0, 1, 0),
	AC97_SINGLE("Line-In As Surround", AC97_ALC650_MULTICH, 9, 1, 0),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Mic As Center/LFE",
		.info = snd_ac97_info_single,
		.get = snd_ac97_alc655_mic_get,
		.put = snd_ac97_alc655_mic_put,
		.private_value = AC97_SINGLE_VALUE(0, 0, 1, 0) /* only mask needed */
	},
};

static int alc655_iec958_route_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t *uinfo)
{
	static char *texts_655[3] = { "PCM", "Analog In", "IEC958 In" };
	static char *texts_658[4] = { "PCM", "Analog1 In", "Analog2 In", "IEC958 In" };
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = ac97->spec.dev_flags ? 4 : 3;
	if (uinfo->value.enumerated.item >= uinfo->value.enumerated.items)
		uinfo->value.enumerated.item = uinfo->value.enumerated.items - 1;
	strcpy(uinfo->value.enumerated.name,
	       ac97->spec.dev_flags ?
	       texts_658[uinfo->value.enumerated.item] :
	       texts_655[uinfo->value.enumerated.item]);
	return 0;

}

static int alc655_iec958_route_get(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t *ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	unsigned short val;

	val = ac97->regs[AC97_ALC650_MULTICH];
	val = (val >> 12) & 3;
	if (ac97->spec.dev_flags && val == 3)
		val = 0;
	ucontrol->value.enumerated.item[0] = val;
	return 0;
}

static int alc655_iec958_route_put(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t *ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	return snd_ac97_update_bits(ac97, AC97_ALC650_MULTICH, 3 << 12,
				    (unsigned short)ucontrol->value.enumerated.item[0]);
}

static const snd_kcontrol_new_t snd_ac97_spdif_controls_alc655[] = {
        AC97_SINGLE("IEC958 Capture Switch", AC97_ALC650_MULTICH, 11, 1, 0),
        AC97_SINGLE("IEC958 Input Monitor", AC97_ALC650_MULTICH, 14, 1, 0),
	{
		.iface  = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name   = "IEC958 Playback Route",
		.info   = alc655_iec958_route_info,
		.get    = alc655_iec958_route_get,
		.put    = alc655_iec958_route_put,
	},
};

static int patch_alc655_specific(ac97_t * ac97)
{
	int err;

	if ((err = patch_build_controls(ac97, snd_ac97_controls_alc655, ARRAY_SIZE(snd_ac97_controls_alc655))) < 0)
		return err;
	if (ac97->ext_id & AC97_EI_SPDIF) {
		if ((err = patch_build_controls(ac97, snd_ac97_spdif_controls_alc655, ARRAY_SIZE(snd_ac97_spdif_controls_alc655))) < 0)
			return err;
	}
	return 0;
}

static struct snd_ac97_build_ops patch_alc655_ops = {
	.build_specific	= patch_alc655_specific
};

int patch_alc655(ac97_t * ac97)
{
	unsigned int val;

	ac97->spec.dev_flags = (ac97->id == 0x414c4780); /* ALC658 */

	ac97->build_ops = &patch_alc655_ops;

	/* adjust default values */
	val = snd_ac97_read(ac97, 0x7a); /* misc control */
	val |= (1 << 1); /* spdif input pin */
	val &= ~(1 << 12); /* vref enable */
	snd_ac97_write_cache(ac97, 0x7a, val);
	/* set default: spdif-in enabled,
	   spdif-in monitor off, spdif-in PCM off
	   center on mic off, surround on line-in off
	   duplicate front off
	*/
	snd_ac97_write_cache(ac97, AC97_ALC650_MULTICH, 1<<15);

	/* full DAC volume */
	snd_ac97_write_cache(ac97, AC97_ALC650_SURR_DAC_VOL, 0x0808);
	snd_ac97_write_cache(ac97, AC97_ALC650_LFE_DAC_VOL, 0x0808);
	return 0;
}

/*
 * C-Media CM97xx codecs
 */
static const snd_kcontrol_new_t snd_ac97_cm9738_controls[] = {
	AC97_SINGLE("Line-In As Surround", AC97_CM9738_VENDOR_CTRL, 10, 1, 0),
	AC97_SINGLE("Duplicate Front", AC97_CM9738_VENDOR_CTRL, 13, 1, 0),
};

static int patch_cm9738_specific(ac97_t * ac97)
{
	return patch_build_controls(ac97, snd_ac97_cm9738_controls, ARRAY_SIZE(snd_ac97_cm9738_controls));
}

static struct snd_ac97_build_ops patch_cm9738_ops = {
	.build_specific	= patch_cm9738_specific
};

int patch_cm9738(ac97_t * ac97)
{
	ac97->build_ops = &patch_cm9738_ops;
	return 0;
}

static int snd_ac97_cmedia_spdif_playback_source_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	static char *texts[] = { "Analog", "Digital" };

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 2;
	if (uinfo->value.enumerated.item > 1)
		uinfo->value.enumerated.item = 1;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_ac97_cmedia_spdif_playback_source_get(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	unsigned short val;

	val = ac97->regs[AC97_CM9739_SPDIF_CTRL];
	ucontrol->value.enumerated.item[0] = (val >> 1) & 0x01;
	return 0;
}

static int snd_ac97_cmedia_spdif_playback_source_put(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);

	return snd_ac97_update_bits(ac97, AC97_CM9739_SPDIF_CTRL,
				    0x01 << 1, 
				    (ucontrol->value.enumerated.item[0] & 0x01) << 1);
}

static const snd_kcontrol_new_t snd_ac97_cm9739_controls_spdif[] = {
	/* BIT 0: SPDI_EN - always true */
	{ /* BIT 1: SPDIFS */
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= SNDRV_CTL_NAME_IEC958("",PLAYBACK,NONE) "Source",
		.info	= snd_ac97_cmedia_spdif_playback_source_info,
		.get	= snd_ac97_cmedia_spdif_playback_source_get,
		.put	= snd_ac97_cmedia_spdif_playback_source_put,
	},
	/* BIT 2: IG_SPIV */
	AC97_SINGLE(SNDRV_CTL_NAME_IEC958("",CAPTURE,NONE) "Valid Switch", AC97_CM9739_SPDIF_CTRL, 2, 1, 0),
	/* BIT 3: SPI2F */
	AC97_SINGLE(SNDRV_CTL_NAME_IEC958("",CAPTURE,NONE) "Monitor", AC97_CM9739_SPDIF_CTRL, 3, 1, 0), 
	/* BIT 4: SPI2SDI */
	AC97_SINGLE(SNDRV_CTL_NAME_IEC958("",CAPTURE,SWITCH), AC97_CM9739_SPDIF_CTRL, 4, 1, 0),
	/* BIT 8: SPD32 - 32bit SPDIF - not supported yet */
};

static int snd_ac97_cm9739_center_mic_get(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	if (ac97->regs[AC97_CM9739_MULTI_CHAN] & 0x1000)
		ucontrol->value.integer.value[0] = 1;
	else
		ucontrol->value.integer.value[0] = 0;
	return 0;
}

static int snd_ac97_cm9739_center_mic_put(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ac97_t *ac97 = snd_kcontrol_chip(kcontrol);
	return snd_ac97_update_bits(ac97, AC97_CM9739_MULTI_CHAN, 0x3000,
				    ucontrol->value.integer.value[0] ? 
				    0x1000 : 0x2000);
}

static const snd_kcontrol_new_t snd_ac97_cm9739_controls[] = {
	AC97_SINGLE("Line-In As Surround", AC97_CM9739_MULTI_CHAN, 10, 1, 0),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Mic As Center/LFE",
		.info = snd_ac97_info_single,
		.get = snd_ac97_cm9739_center_mic_get,
		.put = snd_ac97_cm9739_center_mic_put,
		.private_value = AC97_SINGLE_VALUE(0, 0, 1, 0) /* only mask needed */
	},
};

static int patch_cm9739_specific(ac97_t * ac97)
{
	return patch_build_controls(ac97, snd_ac97_cm9739_controls, ARRAY_SIZE(snd_ac97_cm9739_controls));
}

static int patch_cm9739_post_spdif(ac97_t * ac97)
{
	return patch_build_controls(ac97, snd_ac97_cm9739_controls_spdif, ARRAY_SIZE(snd_ac97_cm9739_controls_spdif));
}

static struct snd_ac97_build_ops patch_cm9739_ops = {
	.build_specific	= patch_cm9739_specific,
	.build_post_spdif = patch_cm9739_post_spdif
};

int patch_cm9739(ac97_t * ac97)
{
	unsigned short val;

	ac97->build_ops = &patch_cm9739_ops;

	/* check spdif */
	val = snd_ac97_read(ac97, AC97_EXTENDED_STATUS);
	if (val & AC97_EA_SPCV) {
		/* enable spdif in */
		snd_ac97_write_cache(ac97, AC97_CM9739_SPDIF_CTRL,
				     snd_ac97_read(ac97, AC97_CM9739_SPDIF_CTRL) | 0x01);
	} else {
		ac97->ext_id &= ~AC97_EI_SPDIF; /* disable extended-id */
	}

	/* set-up multi channel */
	/* bit 14: 0 = SPDIF, 1 = EAPD */
	/* bit 13: enable internal vref output for mic */
	/* bit 12: disable center/lfe (swithable) */
	/* bit 10: disable surround/line (switchable) */
	/* bit 9: mix 2 surround off */
	/* bit 0: dB */
	val = (1 << 13);
	if (! (ac97->ext_id & AC97_EI_SPDIF))
		val |= (1 << 14);
	snd_ac97_write_cache(ac97, AC97_CM9739_MULTI_CHAN, val);

	/* FIXME: set up GPIO */
	snd_ac97_write_cache(ac97, 0x70, 0x0100);
	snd_ac97_write_cache(ac97, 0x72, 0x0020);

	return 0;
}

/*
 * VIA VT1616 codec
 */
static const snd_kcontrol_new_t snd_ac97_controls_vt1616[] = {
AC97_SINGLE("DC Offset removal", 0x5a, 10, 1, 0),
AC97_SINGLE("Alternate Level to Surround Out", 0x5a, 15, 1, 0),
AC97_SINGLE("Downmix LFE and Center to Front", 0x5a, 12, 1, 0),
AC97_SINGLE("Downmix Surround to Front", 0x5a, 11, 1, 0),
};

static int patch_vt1616_specific(ac97_t * ac97)
{
	int err;

	if (snd_ac97_try_bit(ac97, 0x5a, 9))
		if ((err = patch_build_controls(ac97, &snd_ac97_controls_vt1616[0], 1)) < 0)
			return err;
	if ((err = patch_build_controls(ac97, &snd_ac97_controls_vt1616[1], ARRAY_SIZE(snd_ac97_controls_vt1616) - 1)) < 0)
		return err;
	return 0;
}

static struct snd_ac97_build_ops patch_vt1616_ops = {
	.build_specific	= patch_vt1616_specific
};

int patch_vt1616(ac97_t * ac97)
{
	ac97->build_ops = &patch_vt1616_ops;
	return 0;
}

static const snd_kcontrol_new_t snd_ac97_controls_it2646[] = {
	AC97_SINGLE("Line-In As Surround", 0x76, 9, 1, 0),
	AC97_SINGLE("Mic As Center/LFE", 0x76, 10, 1, 0),
};

static const snd_kcontrol_new_t snd_ac97_spdif_controls_it2646[] = {
	AC97_SINGLE("IEC958 Capture Switch", 0x76, 11, 1, 0),
	AC97_SINGLE("Analog to IEC958 Output", 0x76, 12, 1, 0),
	AC97_SINGLE("IEC958 Input Monitor", 0x76, 13, 1, 0),
};

static int patch_it2646_specific(ac97_t * ac97)
{
	int err;
	if ((err = patch_build_controls(ac97, snd_ac97_controls_it2646, ARRAY_SIZE(snd_ac97_controls_it2646))) < 0)
		return err;
	if ((err = patch_build_controls(ac97, snd_ac97_spdif_controls_it2646, ARRAY_SIZE(snd_ac97_spdif_controls_it2646))) < 0)
		return err;
	return 0;
}

static struct snd_ac97_build_ops patch_it2646_ops = {
	.build_specific	= patch_it2646_specific
};

int patch_it2646(ac97_t * ac97)
{
	ac97->build_ops = &patch_it2646_ops;
	/* full DAC volume */
	snd_ac97_write_cache(ac97, 0x5E, 0x0808);
	snd_ac97_write_cache(ac97, 0x7A, 0x0808);
	return 0;
}

/* Si3036/8 specific registers */
#define AC97_SI3036_CHIP_ID     0x5a

int mpatch_si3036(ac97_t * ac97)
{
	//printk("mpatch_si3036: chip id = %x\n", snd_ac97_read(ac97, 0x5a));
	snd_ac97_write_cache(ac97, 0x5c, 0xf210 );
	snd_ac97_write_cache(ac97, 0x68, 0);
	return 0;
}
