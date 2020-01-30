/*
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
 *                   Creative Labs, Inc.
 *  Routines for effect processor FX8010
 *
 *  BUGS:
 *    --
 *
 *  TODO:
 *    --
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
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <sound/core.h>
#include <sound/emu10k1.h>

#define chip_t emu10k1_t

#if 0		/* for testing purposes - digital out -> capture */
#define EMU10K1_CAPTURE_DIGITAL_OUT
#endif
#if 0		/* for testing purposes - set S/PDIF to AC3 output */
#define EMU10K1_SET_AC3_IEC958
#endif
#if 0		/* for testing purposes - feed the front signal to Center/LFE outputs */
#define EMU10K1_CENTER_LFE_FROM_FRONT
#endif

/*
 *  Tables
 */ 

static char *fxbuses[16] = {
	/* 0x00 */ "PCM Left",
	/* 0x01 */ "PCM Right",
	/* 0x02 */ "PCM Surround Left",
	/* 0x03 */ "PCM Surround Right",
	/* 0x04 */ "MIDI Left",
	/* 0x05 */ "MIDI Right",
	/* 0x06 */ "Center",
	/* 0x07 */ "LFE",
	/* 0x08 */ NULL,
	/* 0x09 */ NULL,
	/* 0x0a */ NULL,
	/* 0x0b */ NULL,
	/* 0x0c */ "MIDI Reverb",
	/* 0x0d */ "MIDI Chorus",
	/* 0x0e */ NULL,
	/* 0x0f */ NULL
};

static char *creative_ins[16] = {
	/* 0x00 */ "AC97 Left",
	/* 0x01 */ "AC97 Right",
	/* 0x02 */ "TTL IEC958 Left",
	/* 0x03 */ "TTL IEC958 Right",
	/* 0x04 */ "Zoom Video Left",
	/* 0x05 */ "Zoom Video Right",
	/* 0x06 */ "Optical IEC958 Left",
	/* 0x07 */ "Optical IEC958 Right",
	/* 0x08 */ "Line/Mic 1 Left",
	/* 0x09 */ "Line/Mic 1 Right",
	/* 0x0a */ "Coaxial IEC958 Left",
	/* 0x0b */ "Coaxial IEC958 Right",
	/* 0x0c */ "Line/Mic 2 Left",
	/* 0x0d */ "Line/Mic 2 Right",
	/* 0x0e */ NULL,
	/* 0x0f */ NULL
};

static char *audigy_ins[16] = {
	/* 0x00 */ "AC97 Left",
	/* 0x01 */ "AC97 Right",
	/* 0x02 */ "Audigy CD Left",
	/* 0x03 */ "Audigy CD Right",
	/* 0x04 */ "Optical IEC958 Left",
	/* 0x05 */ "Optical IEC958 Right",
	/* 0x06 */ NULL,
	/* 0x07 */ NULL,
	/* 0x08 */ "Line/Mic 2 Left",
	/* 0x09 */ "Line/Mic 2 Right",
	/* 0x0a */ "SPDIF Left",
	/* 0x0b */ "SPDIF Right",
	/* 0x0c */ "Aux2 Left",
	/* 0x0d */ "Aux2 Right",
	/* 0x0e */ NULL,
	/* 0x0f */ NULL
};

static char *creative_outs[32] = {
	/* 0x00 */ "AC97 Left",
	/* 0x01 */ "AC97 Right",
	/* 0x02 */ "Optical IEC958 Left",
	/* 0x03 */ "Optical IEC958 Right",
	/* 0x04 */ "Center",
	/* 0x05 */ "LFE",
	/* 0x06 */ "Headphone Left",
	/* 0x07 */ "Headphone Right",
	/* 0x08 */ "Surround Left",
	/* 0x09 */ "Surround Right",
	/* 0x0a */ "PCM Capture Left",
	/* 0x0b */ "PCM Capture Right",
	/* 0x0c */ "MIC Capture",
	/* 0x0d */ "AC97 Surround Left",
	/* 0x0e */ "AC97 Surround Right",
	/* 0x0f */ NULL,
	/* 0x10 */ NULL,
	/* 0x11 */ "Analog Center",
	/* 0x12 */ "Analog LFE",
	/* 0x13 */ NULL,
	/* 0x14 */ NULL,
	/* 0x15 */ NULL,
	/* 0x16 */ NULL,
	/* 0x17 */ NULL,
	/* 0x18 */ NULL,
	/* 0x19 */ NULL,
	/* 0x1a */ NULL,
	/* 0x1b */ NULL,
	/* 0x1c */ NULL,
	/* 0x1d */ NULL,
	/* 0x1e */ NULL,
	/* 0x1f */ NULL,
};

static char *audigy_outs[32] = {
	/* 0x00 */ "Digital Front Left",
	/* 0x01 */ "Digital Front Right",
	/* 0x02 */ "Digital Center",
	/* 0x03 */ "Digital LEF",
	/* 0x04 */ "Headphone Left",
	/* 0x05 */ "Headphone Right",
	/* 0x06 */ "Digital Rear Left",
	/* 0x07 */ "Digital Rear Right",
	/* 0x08 */ "Front Left",
	/* 0x09 */ "Front Right",
	/* 0x0a */ "Center",
	/* 0x0b */ "LFE",
	/* 0x0c */ NULL,
	/* 0x0d */ NULL,
	/* 0x0e */ "Rear Left",
	/* 0x0f */ "Rear Right",
	/* 0x10 */ "AC97 Front Left",
	/* 0x11 */ "AC97 Front Right",
	/* 0x12 */ "ADC Caputre Left",
	/* 0x13 */ "ADC Capture Right",
	/* 0x14 */ NULL,
	/* 0x15 */ NULL,
	/* 0x16 */ NULL,
	/* 0x17 */ NULL,
	/* 0x18 */ NULL,
	/* 0x19 */ NULL,
	/* 0x1a */ NULL,
	/* 0x1b */ NULL,
	/* 0x1c */ NULL,
	/* 0x1d */ NULL,
	/* 0x1e */ NULL,
	/* 0x1f */ NULL,
};

static const u32 bass_table[41][5] = {
	{ 0x3e4f844f, 0x84ed4cc3, 0x3cc69927, 0x7b03553a, 0xc4da8486 },
	{ 0x3e69a17a, 0x84c280fb, 0x3cd77cd4, 0x7b2f2a6f, 0xc4b08d1d },
	{ 0x3e82ff42, 0x849991d5, 0x3ce7466b, 0x7b5917c6, 0xc48863ee },
	{ 0x3e9bab3c, 0x847267f0, 0x3cf5ffe8, 0x7b813560, 0xc461f22c },
	{ 0x3eb3b275, 0x844ced29, 0x3d03b295, 0x7ba79a1c, 0xc43d223b },
	{ 0x3ecb2174, 0x84290c8b, 0x3d106714, 0x7bcc5ba3, 0xc419dfa5 },
	{ 0x3ee2044b, 0x8406b244, 0x3d1c2561, 0x7bef8e77, 0xc3f8170f },
	{ 0x3ef86698, 0x83e5cb96, 0x3d26f4d8, 0x7c114600, 0xc3d7b625 },
	{ 0x3f0e5390, 0x83c646c9, 0x3d30dc39, 0x7c319498, 0xc3b8ab97 },
	{ 0x3f23d60b, 0x83a81321, 0x3d39e1af, 0x7c508b9c, 0xc39ae704 },
	{ 0x3f38f884, 0x838b20d2, 0x3d420ad2, 0x7c6e3b75, 0xc37e58f1 },
	{ 0x3f4dc52c, 0x836f60ef, 0x3d495cab, 0x7c8ab3a6, 0xc362f2be },
	{ 0x3f6245e8, 0x8354c565, 0x3d4fdbb8, 0x7ca602d6, 0xc348a69b },
	{ 0x3f76845f, 0x833b40ec, 0x3d558bf0, 0x7cc036df, 0xc32f677c },
	{ 0x3f8a8a03, 0x8322c6fb, 0x3d5a70c4, 0x7cd95cd7, 0xc317290b },
	{ 0x3f9e6014, 0x830b4bc3, 0x3d5e8d25, 0x7cf1811a, 0xc2ffdfa5 },
	{ 0x3fb20fae, 0x82f4c420, 0x3d61e37f, 0x7d08af56, 0xc2e9804a },
	{ 0x3fc5a1cc, 0x82df2592, 0x3d6475c3, 0x7d1ef294, 0xc2d40096 },
	{ 0x3fd91f55, 0x82ca6632, 0x3d664564, 0x7d345541, 0xc2bf56b9 },
	{ 0x3fec9120, 0x82b67cac, 0x3d675356, 0x7d48e138, 0xc2ab796e },
	{ 0x40000000, 0x82a36037, 0x3d67a012, 0x7d5c9fc9, 0xc2985fee },
	{ 0x401374c7, 0x8291088a, 0x3d672b93, 0x7d6f99c3, 0xc28601f2 },
	{ 0x4026f857, 0x827f6dd7, 0x3d65f559, 0x7d81d77c, 0xc27457a3 },
	{ 0x403a939f, 0x826e88c5, 0x3d63fc63, 0x7d9360d4, 0xc2635996 },
	{ 0x404e4faf, 0x825e5266, 0x3d613f32, 0x7da43d42, 0xc25300c6 },
	{ 0x406235ba, 0x824ec434, 0x3d5dbbc3, 0x7db473d7, 0xc243468e },
	{ 0x40764f1f, 0x823fd80c, 0x3d596f8f, 0x7dc40b44, 0xc23424a2 },
	{ 0x408aa576, 0x82318824, 0x3d545787, 0x7dd309e2, 0xc2259509 },
	{ 0x409f4296, 0x8223cf0b, 0x3d4e7012, 0x7de175b5, 0xc2179218 },
	{ 0x40b430a0, 0x8216a7a1, 0x3d47b505, 0x7def5475, 0xc20a1670 },
	{ 0x40c97a0a, 0x820a0d12, 0x3d4021a1, 0x7dfcab8d, 0xc1fd1cf5 },
	{ 0x40df29a6, 0x81fdfad6, 0x3d37b08d, 0x7e098028, 0xc1f0a0ca },
	{ 0x40f54ab1, 0x81f26ca9, 0x3d2e5bd1, 0x7e15d72b, 0xc1e49d52 },
	{ 0x410be8da, 0x81e75e89, 0x3d241cce, 0x7e21b544, 0xc1d90e24 },
	{ 0x41231051, 0x81dcccb3, 0x3d18ec37, 0x7e2d1ee6, 0xc1cdef10 },
	{ 0x413acdd0, 0x81d2b39e, 0x3d0cc20a, 0x7e38184e, 0xc1c33c13 },
	{ 0x41532ea7, 0x81c90ffb, 0x3cff9585, 0x7e42a58b, 0xc1b8f15a },
	{ 0x416c40cd, 0x81bfdeb2, 0x3cf15d21, 0x7e4cca7c, 0xc1af0b3f },
	{ 0x418612ea, 0x81b71cdc, 0x3ce20e85, 0x7e568ad3, 0xc1a58640 },
	{ 0x41a0b465, 0x81aec7c5, 0x3cd19e7c, 0x7e5fea1e, 0xc19c5f03 },
	{ 0x41bc3573, 0x81a6dcea, 0x3cc000e9, 0x7e68ebc2, 0xc1939250 }
};

static const u32 treble_table[41][5] = {
	{ 0x0125cba9, 0xfed5debd, 0x00599b6c, 0x0d2506da, 0xfa85b354 },
	{ 0x0142f67e, 0xfeb03163, 0x0066cd0f, 0x0d14c69d, 0xfa914473 },
	{ 0x016328bd, 0xfe860158, 0x0075b7f2, 0x0d03eb27, 0xfa9d32d2 },
	{ 0x0186b438, 0xfe56c982, 0x00869234, 0x0cf27048, 0xfaa97fca },
	{ 0x01adf358, 0xfe21f5fe, 0x00999842, 0x0ce051c2, 0xfab62ca5 },
	{ 0x01d949fa, 0xfde6e287, 0x00af0d8d, 0x0ccd8b4a, 0xfac33aa7 },
	{ 0x02092669, 0xfda4d8bf, 0x00c73d4c, 0x0cba1884, 0xfad0ab07 },
	{ 0x023e0268, 0xfd5b0e4a, 0x00e27b54, 0x0ca5f509, 0xfade7ef2 },
	{ 0x0278645c, 0xfd08a2b0, 0x01012509, 0x0c911c63, 0xfaecb788 },
	{ 0x02b8e091, 0xfcac9d1a, 0x0123a262, 0x0c7b8a14, 0xfafb55df },
	{ 0x03001a9a, 0xfc45e9ce, 0x014a6709, 0x0c65398f, 0xfb0a5aff },
	{ 0x034ec6d7, 0xfbd3576b, 0x0175f397, 0x0c4e2643, 0xfb19c7e4 },
	{ 0x03a5ac15, 0xfb5393ee, 0x01a6d6ed, 0x0c364b94, 0xfb299d7c },
	{ 0x0405a562, 0xfac52968, 0x01ddafae, 0x0c1da4e2, 0xfb39dca5 },
	{ 0x046fa3fe, 0xfa267a66, 0x021b2ddd, 0x0c042d8d, 0xfb4a8631 },
	{ 0x04e4b17f, 0xf975be0f, 0x0260149f, 0x0be9e0f2, 0xfb5b9ae0 },
	{ 0x0565f220, 0xf8b0fbe5, 0x02ad3c29, 0x0bceba73, 0xfb6d1b60 },
	{ 0x05f4a745, 0xf7d60722, 0x030393d4, 0x0bb2b578, 0xfb7f084d },
	{ 0x06923236, 0xf6e279bd, 0x03642465, 0x0b95cd75, 0xfb916233 },
	{ 0x07401713, 0xf5d3aef9, 0x03d01283, 0x0b77fded, 0xfba42984 },
	{ 0x08000000, 0xf4a6bd88, 0x0448a161, 0x0b594278, 0xfbb75e9f },
	{ 0x08d3c097, 0xf3587131, 0x04cf35a4, 0x0b3996c9, 0xfbcb01cb },
	{ 0x09bd59a2, 0xf1e543f9, 0x05655880, 0x0b18f6b2, 0xfbdf1333 },
	{ 0x0abefd0f, 0xf04956ca, 0x060cbb12, 0x0af75e2c, 0xfbf392e8 },
	{ 0x0bdb123e, 0xee806984, 0x06c739fe, 0x0ad4c962, 0xfc0880dd },
	{ 0x0d143a94, 0xec85d287, 0x0796e150, 0x0ab134b0, 0xfc1ddce5 },
	{ 0x0e6d5664, 0xea547598, 0x087df0a0, 0x0a8c9cb6, 0xfc33a6ad },
	{ 0x0fe98a2a, 0xe7e6ba35, 0x097edf83, 0x0a66fe5b, 0xfc49ddc2 },
	{ 0x118c4421, 0xe536813a, 0x0a9c6248, 0x0a4056d7, 0xfc608185 },
	{ 0x1359422e, 0xe23d19eb, 0x0bd96efb, 0x0a18a3bf, 0xfc77912c },
	{ 0x1554982b, 0xdef33645, 0x0d3942bd, 0x09efe312, 0xfc8f0bc1 },
	{ 0x1782b68a, 0xdb50deb1, 0x0ebf676d, 0x09c6133f, 0xfca6f019 },
	{ 0x19e8715d, 0xd74d64fd, 0x106fb999, 0x099b3337, 0xfcbf3cd6 },
	{ 0x1c8b07b8, 0xd2df56ab, 0x124e6ec8, 0x096f4274, 0xfcd7f060 },
	{ 0x1f702b6d, 0xcdfc6e92, 0x14601c10, 0x0942410b, 0xfcf108e5 },
	{ 0x229e0933, 0xc89985cd, 0x16a9bcfa, 0x09142fb5, 0xfd0a8451 },
	{ 0x261b5118, 0xc2aa8409, 0x1930bab6, 0x08e50fdc, 0xfd24604d },
	{ 0x29ef3f5d, 0xbc224f28, 0x1bfaf396, 0x08b4e3aa, 0xfd3e9a3b },
	{ 0x2e21a59b, 0xb4f2ba46, 0x1f0ec2d6, 0x0883ae15, 0xfd592f33 },
	{ 0x32baf44b, 0xad0c7429, 0x227308a3, 0x085172eb, 0xfd741bfd },
	{ 0x37c4448b, 0xa45ef51d, 0x262f3267, 0x081e36dc, 0xfd8f5d14 }
};

static const u32 db_table[101] = {
	0x00000000, 0x01571f82, 0x01674b41, 0x01783a1b, 0x0189f540,
	0x019c8651, 0x01aff763, 0x01c45306, 0x01d9a446, 0x01eff6b8,
	0x0207567a, 0x021fd03d, 0x0239714c, 0x02544792, 0x027061a1,
	0x028dcebb, 0x02ac9edc, 0x02cce2bf, 0x02eeabe8, 0x03120cb0,
	0x0337184e, 0x035de2df, 0x03868173, 0x03b10a18, 0x03dd93e9,
	0x040c3713, 0x043d0cea, 0x04702ff3, 0x04a5bbf2, 0x04ddcdfb,
	0x0518847f, 0x0555ff62, 0x05966005, 0x05d9c95d, 0x06206005,
	0x066a4a52, 0x06b7b067, 0x0708bc4c, 0x075d9a01, 0x07b6779d,
	0x08138561, 0x0874f5d5, 0x08dafde1, 0x0945d4ed, 0x09b5b4fd,
	0x0a2adad1, 0x0aa58605, 0x0b25f936, 0x0bac7a24, 0x0c3951d8,
	0x0ccccccc, 0x0d673b17, 0x0e08f093, 0x0eb24510, 0x0f639481,
	0x101d3f2d, 0x10dfa9e6, 0x11ab3e3f, 0x12806ac3, 0x135fa333,
	0x144960c5, 0x153e2266, 0x163e6cfe, 0x174acbb7, 0x1863d04d,
	0x198a1357, 0x1abe349f, 0x1c00db77, 0x1d52b712, 0x1eb47ee6,
	0x2026f30f, 0x21aadcb6, 0x23410e7e, 0x24ea64f9, 0x26a7c71d,
	0x287a26c4, 0x2a62812c, 0x2c61df84, 0x2e795779, 0x30aa0bcf,
	0x32f52cfe, 0x355bf9d8, 0x37dfc033, 0x3a81dda4, 0x3d43c038,
	0x4026e73c, 0x432ce40f, 0x46575af8, 0x49a8040f, 0x4d20ac2a,
	0x50c335d3, 0x54919a57, 0x588dead1, 0x5cba514a, 0x611911ea,
	0x65ac8c2f, 0x6a773c39, 0x6f7bbc23, 0x74bcc56c, 0x7a3d3272,
	0x7fffffff,
};

static const u32 onoff_table[2] = {
	0x00000000, 0x00000001
};

/*
 */
 
static inline mm_segment_t snd_enter_user(void)
{
	mm_segment_t fs = get_fs();
	set_fs(get_ds());
	return fs;
}

static inline void snd_leave_user(mm_segment_t fs)
{
	set_fs(fs);
}

/*
 *   controls
 */

static int snd_emu10k1_gpr_ctl_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	snd_emu10k1_fx8010_ctl_t *ctl = (snd_emu10k1_fx8010_ctl_t *)kcontrol->private_value;

	if (ctl->min == 0 && ctl->max == 1)
		uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	else
		uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = ctl->vcount;
	uinfo->value.integer.min = ctl->min;
	uinfo->value.integer.max = ctl->max;
	return 0;
}

static int snd_emu10k1_gpr_ctl_get(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	emu10k1_t *emu = snd_kcontrol_chip(kcontrol);
	snd_emu10k1_fx8010_ctl_t *ctl = (snd_emu10k1_fx8010_ctl_t *)kcontrol->private_value;
	unsigned long flags;
	unsigned int i;
	
	spin_lock_irqsave(&emu->reg_lock, flags);
	for (i = 0; i < ctl->vcount; i++)
		ucontrol->value.integer.value[i] = ctl->value[i];
	spin_unlock_irqrestore(&emu->reg_lock, flags);
	return 0;
}

static int snd_emu10k1_gpr_ctl_put(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	emu10k1_t *emu = snd_kcontrol_chip(kcontrol);
	snd_emu10k1_fx8010_ctl_t *ctl = (snd_emu10k1_fx8010_ctl_t *)kcontrol->private_value;
	unsigned long flags;
	unsigned int nval, val;
	unsigned int i, j;
	int change = 0;
	
	spin_lock_irqsave(&emu->reg_lock, flags);
	for (i = 0; i < ctl->vcount; i++) {
		nval = ucontrol->value.integer.value[i];
		if (nval < ctl->min)
			nval = ctl->min;
		if (nval > ctl->max)
			nval = ctl->max;
		if (nval != ctl->value[i])
			change = 1;
		val = ctl->value[i] = nval;
		switch (ctl->translation) {
		case EMU10K1_GPR_TRANSLATION_NONE:
			snd_emu10k1_ptr_write(emu, emu->gpr_base + ctl->gpr[i], 0, val);
			break;
		case EMU10K1_GPR_TRANSLATION_TABLE100:
			snd_emu10k1_ptr_write(emu, emu->gpr_base + ctl->gpr[i], 0, db_table[val]);
			break;
		case EMU10K1_GPR_TRANSLATION_BASS:
			snd_runtime_check((ctl->count % 5) == 0 && (ctl->count / 5) == ctl->vcount, change = -EIO; goto __error);
			for (j = 0; j < 5; j++)
				snd_emu10k1_ptr_write(emu, emu->gpr_base + ctl->gpr[j * ctl->vcount + i], 0, bass_table[val][j]);
			break;
		case EMU10K1_GPR_TRANSLATION_TREBLE:
			snd_runtime_check((ctl->count % 5) == 0 && (ctl->count / 5) == ctl->vcount, change = -EIO; goto __error);
			for (j = 0; j < 5; j++)
				snd_emu10k1_ptr_write(emu, emu->gpr_base + ctl->gpr[j * ctl->vcount + i], 0, treble_table[val][j]);
			break;
		case EMU10K1_GPR_TRANSLATION_ONOFF:
			snd_emu10k1_ptr_write(emu, emu->gpr_base + ctl->gpr[i], 0, onoff_table[val]);
			break;
		}
	}
      __error:
	spin_unlock_irqrestore(&emu->reg_lock, flags);
	return change;
}

/*
 *   Interrupt handler
 */

static void snd_emu10k1_fx8010_interrupt(emu10k1_t *emu)
{
	snd_emu10k1_fx8010_irq_t *irq, *nirq;

	irq = emu->fx8010.irq_handlers;
	while (irq) {
		nirq = irq->next;	/* irq ptr can be removed from list */
		if (snd_emu10k1_ptr_read(emu, emu->gpr_base + irq->gpr_running, 0) & 0xffff0000) {
			if (irq->handler)
				irq->handler(emu, irq->private_data);
			snd_emu10k1_ptr_write(emu, emu->gpr_base + irq->gpr_running, 0, 1);
		}
		irq = nirq;
	}
}

static int snd_emu10k1_fx8010_register_irq_handler(emu10k1_t *emu,
						   snd_fx8010_irq_handler_t *handler,
						   unsigned char gpr_running,
						   void *private_data,
						   snd_emu10k1_fx8010_irq_t **r_irq)
{
	snd_emu10k1_fx8010_irq_t *irq;
	unsigned long flags;
	
	snd_runtime_check(emu, return -EINVAL);
	snd_runtime_check(handler, return -EINVAL);
	irq = kmalloc(sizeof(*irq), GFP_ATOMIC);
	if (irq == NULL)
		return -ENOMEM;
	irq->handler = handler;
	irq->gpr_running = gpr_running;
	irq->private_data = private_data;
	irq->next = NULL;
	spin_lock_irqsave(&emu->fx8010.irq_lock, flags);
	if (emu->fx8010.irq_handlers == NULL) {
		emu->fx8010.irq_handlers = irq;
		emu->dsp_interrupt = snd_emu10k1_fx8010_interrupt;
		snd_emu10k1_intr_enable(emu, INTE_FXDSPENABLE);
	} else {
		irq->next = emu->fx8010.irq_handlers;
		emu->fx8010.irq_handlers = irq;
	}
	spin_unlock_irqrestore(&emu->fx8010.irq_lock, flags);
	if (r_irq)
		*r_irq = irq;
	return 0;
}

static int snd_emu10k1_fx8010_unregister_irq_handler(emu10k1_t *emu,
						     snd_emu10k1_fx8010_irq_t *irq)
{
	snd_emu10k1_fx8010_irq_t *tmp;
	unsigned long flags;
	
	snd_runtime_check(irq, return -EINVAL);
	spin_lock_irqsave(&emu->fx8010.irq_lock, flags);
	if ((tmp = emu->fx8010.irq_handlers) == irq) {
		emu->fx8010.irq_handlers = tmp->next;
		if (emu->fx8010.irq_handlers == NULL) {
			snd_emu10k1_intr_disable(emu, INTE_FXDSPENABLE);
			emu->dsp_interrupt = NULL;
		}
	} else {
		while (tmp && tmp->next != irq)
			tmp = tmp->next;
		if (tmp)
			tmp->next = tmp->next->next;
	}
	spin_unlock_irqrestore(&emu->fx8010.irq_lock, flags);
	kfree(irq);
	return 0;
}

/*
 *   PCM streams
 */

#define INITIAL_TRAM_SHIFT     14
#define INITIAL_TRAM_POS(size) ((((size) / 2) - INITIAL_TRAM_SHIFT) - 1)

static void snd_emu10k1_fx8010_playback_irq(emu10k1_t *emu, void *private_data)
{
	snd_pcm_substream_t *substream = snd_magic_cast(snd_pcm_substream_t, private_data, return);
	snd_pcm_period_elapsed(substream);
}

static void snd_emu10k1_fx8010_playback_tram_poke1(unsigned short *dst_left,
						   unsigned short *dst_right,
						   unsigned short *src,
						   unsigned int count,
						   unsigned int tram_shift)
{
	// printk("tram_poke1: dst_left = 0x%p, dst_right = 0x%p, src = 0x%p, count = 0x%x\n", dst_left, dst_right, src, count);
	if ((tram_shift & 1) == 0) {
		while (count--) {
			*dst_left-- = *src++;
			*dst_right-- = *src++;
		}
	} else {
		while (count--) {
			*dst_right-- = *src++;
			*dst_left-- = *src++;
		}
	}
}

static void snd_emu10k1_fx8010_playback_tram_poke(emu10k1_t *emu,
						  unsigned int *tram_pos,
						  unsigned int *tram_shift,
						  unsigned int tram_size,
						  unsigned short *src,
						  unsigned int frames)
{
	unsigned int count;

	while (frames > *tram_pos) {
		count = *tram_pos + 1;
		snd_emu10k1_fx8010_playback_tram_poke1((unsigned short *)emu->fx8010.etram_pages.area + *tram_pos,
						       (unsigned short *)emu->fx8010.etram_pages.area + *tram_pos + tram_size / 2,
						       src, count, *tram_shift);
		src += count * 2;
		frames -= count;
		*tram_pos = (tram_size / 2) - 1;
		(*tram_shift)++;
	}
	snd_emu10k1_fx8010_playback_tram_poke1((unsigned short *)emu->fx8010.etram_pages.area + *tram_pos,
					       (unsigned short *)emu->fx8010.etram_pages.area + *tram_pos + tram_size / 2,
					       src, frames, *tram_shift++);
	*tram_pos -= frames;
}

static int snd_emu10k1_fx8010_playback_transfer(snd_pcm_substream_t *substream)
{
	emu10k1_t *emu = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_emu10k1_fx8010_pcm_t *pcm = &emu->fx8010.pcm[substream->number];
	snd_pcm_uframes_t appl_ptr = runtime->control->appl_ptr;
	snd_pcm_sframes_t diff = appl_ptr - pcm->appl_ptr;
	snd_pcm_uframes_t buffer_size = pcm->buffer_size / 2;

	if (diff) {
		if (diff < -(snd_pcm_sframes_t) (runtime->boundary / 2))
			diff += runtime->boundary;
		pcm->sw_ready += diff;
		pcm->appl_ptr = appl_ptr;
	}
	while (pcm->hw_ready < buffer_size &&
	       pcm->sw_ready > 0) {
	       	size_t hw_to_end = buffer_size - pcm->hw_data;
	       	size_t sw_to_end = (runtime->buffer_size << 2) - pcm->sw_data;
	       	size_t tframes = buffer_size - pcm->hw_ready;
	       	if (pcm->sw_ready < tframes)
	       		tframes = pcm->sw_ready;
	       	if (hw_to_end < tframes)
	       		tframes = hw_to_end;
	       	if (sw_to_end < tframes)
	       		tframes = sw_to_end;
	       	snd_emu10k1_fx8010_playback_tram_poke(emu, &pcm->tram_pos, &pcm->tram_shift,
	       					      pcm->buffer_size,
	       					      (unsigned short *)(runtime->dma_area + (pcm->sw_data << 2)),
	       					      tframes);
		pcm->hw_data += tframes;
		if (pcm->hw_data == buffer_size)
			pcm->hw_data = 0;
		pcm->sw_data += tframes;
		if (pcm->sw_data == runtime->buffer_size)
			pcm->sw_data = 0;
		pcm->hw_ready += tframes;
		pcm->sw_ready -= tframes;
	}
	return 0;
}

static int snd_emu10k1_fx8010_playback_hw_params(snd_pcm_substream_t * substream,
						 snd_pcm_hw_params_t * hw_params)
{
	return snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
}

static int snd_emu10k1_fx8010_playback_hw_free(snd_pcm_substream_t * substream)
{
	emu10k1_t *emu = snd_pcm_substream_chip(substream);
	snd_emu10k1_fx8010_pcm_t *pcm = &emu->fx8010.pcm[substream->number];
	unsigned int i;

	for (i = 0; i < pcm->channels; i++)
		snd_emu10k1_ptr_write(emu, TANKMEMADDRREGBASE + 0x80 + pcm->etram[i], 0, 0);
	snd_pcm_lib_free_pages(substream);
	return 0;
}

static int snd_emu10k1_fx8010_playback_prepare(snd_pcm_substream_t * substream)
{
	emu10k1_t *emu = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_emu10k1_fx8010_pcm_t *pcm = &emu->fx8010.pcm[substream->number];
	unsigned int i;
	
	// printk("prepare: etram_pages = 0x%p, dma_area = 0x%x, buffer_size = 0x%x (0x%x)\n", emu->fx8010.etram_pages, runtime->dma_area, runtime->buffer_size, runtime->buffer_size << 2);
	pcm->sw_data = pcm->sw_io = pcm->sw_ready = 0;
	pcm->hw_data = pcm->hw_io = pcm->hw_ready = 0;
	pcm->tram_pos = INITIAL_TRAM_POS(pcm->buffer_size);
	pcm->tram_shift = 0;
	pcm->appl_ptr = 0;
	snd_emu10k1_ptr_write(emu, emu->gpr_base + pcm->gpr_running, 0, 0);	/* reset */
	snd_emu10k1_ptr_write(emu, emu->gpr_base + pcm->gpr_trigger, 0, 0);	/* reset */
	snd_emu10k1_ptr_write(emu, emu->gpr_base + pcm->gpr_size, 0, runtime->buffer_size);
	snd_emu10k1_ptr_write(emu, emu->gpr_base + pcm->gpr_ptr, 0, 0);		/* reset ptr number */
	snd_emu10k1_ptr_write(emu, emu->gpr_base + pcm->gpr_count, 0, runtime->period_size);
	snd_emu10k1_ptr_write(emu, emu->gpr_base + pcm->gpr_tmpcount, 0, runtime->period_size);
	for (i = 0; i < pcm->channels; i++)
		snd_emu10k1_ptr_write(emu, TANKMEMADDRREGBASE + 0x80 + pcm->etram[i], 0, (TANKMEMADDRREG_READ|TANKMEMADDRREG_ALIGN) + i * (runtime->buffer_size / pcm->channels));
	return 0;
}

static int snd_emu10k1_fx8010_playback_trigger(snd_pcm_substream_t * substream, int cmd)
{
	emu10k1_t *emu = snd_pcm_substream_chip(substream);
	snd_emu10k1_fx8010_pcm_t *pcm = &emu->fx8010.pcm[substream->number];
	unsigned long flags;
	int result = 0;

	spin_lock_irqsave(&emu->reg_lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		/* follow thru */
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
#ifdef EMU10K1_SET_AC3_IEC958
	{
		int i;
		for (i = 0; i < 3; i++) {
			unsigned int bits;
			bits = SPCS_CLKACCY_1000PPM | SPCS_SAMPLERATE_48 |
			       SPCS_CHANNELNUM_LEFT | SPCS_SOURCENUM_UNSPEC | SPCS_GENERATIONSTATUS |
			       0x00001200 | SPCS_EMPHASIS_NONE | SPCS_COPYRIGHT | SPCS_NOTAUDIODATA;
			snd_emu10k1_ptr_write(emu, SPCS0 + i, 0, bits);
		}
	}
#endif
		result = snd_emu10k1_fx8010_register_irq_handler(emu, snd_emu10k1_fx8010_playback_irq, pcm->gpr_running, substream, &pcm->irq);
		if (result < 0)
			goto __err;
		snd_emu10k1_fx8010_playback_transfer(substream);	/* roll the ball */
		snd_emu10k1_ptr_write(emu, emu->gpr_base + pcm->gpr_trigger, 0, 1);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		snd_emu10k1_fx8010_unregister_irq_handler(emu, pcm->irq); pcm->irq = NULL;
		snd_emu10k1_ptr_write(emu, emu->gpr_base + pcm->gpr_trigger, 0, 0);
		pcm->tram_pos = INITIAL_TRAM_POS(pcm->buffer_size);
		pcm->tram_shift = 0;
		break;
	default:
		result = -EINVAL;
		break;
	}
      __err:
	spin_unlock_irqrestore(&emu->reg_lock, flags);
	return result;
}

static snd_pcm_uframes_t snd_emu10k1_fx8010_playback_pointer(snd_pcm_substream_t * substream)
{
	emu10k1_t *emu = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_emu10k1_fx8010_pcm_t *pcm = &emu->fx8010.pcm[substream->number];
	size_t ptr;
	snd_pcm_sframes_t frames;

	if (!snd_emu10k1_ptr_read(emu, emu->gpr_base + pcm->gpr_trigger, 0))
		return 0;
	ptr = snd_emu10k1_ptr_read(emu, emu->gpr_base + pcm->gpr_ptr, 0);
	frames = ptr - pcm->hw_io;
	if (frames < 0)
		frames += runtime->buffer_size;
	pcm->hw_io = ptr;
	pcm->hw_ready -= frames;
	pcm->sw_io += frames;
	if (pcm->sw_io >= runtime->buffer_size)
		pcm->sw_io -= runtime->buffer_size;
	snd_emu10k1_fx8010_playback_transfer(substream);
	return pcm->sw_io;
}

static snd_pcm_hardware_t snd_emu10k1_fx8010_playback =
{
	.info =			(SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
				 /* SNDRV_PCM_INFO_MMAP_VALID | */ SNDRV_PCM_INFO_PAUSE),
	.formats =		SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S16_LE,
	.rates =		SNDRV_PCM_RATE_48000,
	.rate_min =		48000,
	.rate_max =		48000,
	.channels_min =		1,
	.channels_max =		1,
	.buffer_bytes_max =	(128*1024),
	.period_bytes_min =	1024,
	.period_bytes_max =	(128*1024),
	.periods_min =		1,
	.periods_max =		1024,
	.fifo_size =		0,
};

static int snd_emu10k1_fx8010_playback_open(snd_pcm_substream_t * substream)
{
	emu10k1_t *emu = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_emu10k1_fx8010_pcm_t *pcm = &emu->fx8010.pcm[substream->number];

	runtime->hw = snd_emu10k1_fx8010_playback;
	runtime->hw.channels_min = runtime->hw.channels_max = pcm->channels;
	runtime->hw.period_bytes_max = (pcm->buffer_size * 2) / 2;
	spin_lock(&emu->reg_lock);
	if (pcm->valid == 0) {
		spin_unlock(&emu->reg_lock);
		return -ENODEV;
	}
	pcm->opened = 1;
	spin_unlock(&emu->reg_lock);
	return 0;
}

static int snd_emu10k1_fx8010_playback_close(snd_pcm_substream_t * substream)
{
	emu10k1_t *emu = snd_pcm_substream_chip(substream);
	snd_emu10k1_fx8010_pcm_t *pcm = &emu->fx8010.pcm[substream->number];

	spin_lock(&emu->reg_lock);
	pcm->opened = 0;
	spin_unlock(&emu->reg_lock);
	return 0;
}

static snd_pcm_ops_t snd_emu10k1_fx8010_playback_ops = {
	.open =			snd_emu10k1_fx8010_playback_open,
	.close =		snd_emu10k1_fx8010_playback_close,
	.ioctl =		snd_pcm_lib_ioctl,
	.hw_params =		snd_emu10k1_fx8010_playback_hw_params,
	.hw_free =		snd_emu10k1_fx8010_playback_hw_free,
	.prepare =		snd_emu10k1_fx8010_playback_prepare,
	.trigger =		snd_emu10k1_fx8010_playback_trigger,
	.pointer =		snd_emu10k1_fx8010_playback_pointer,
	.ack =			snd_emu10k1_fx8010_playback_transfer,
};

static void snd_emu10k1_fx8010_pcm_free(snd_pcm_t *pcm)
{
	emu10k1_t *emu = snd_magic_cast(emu10k1_t, pcm->private_data, return);
	emu->pcm_fx8010 = NULL;
	snd_pcm_lib_preallocate_free_for_all(pcm);
}

int snd_emu10k1_fx8010_pcm(emu10k1_t * emu, int device, snd_pcm_t ** rpcm)
{
	snd_pcm_t *pcm;
	int err;

	if (rpcm)
		*rpcm = NULL;

	if ((err = snd_pcm_new(emu->card, "emu10k1", device, 8, 0, &pcm)) < 0)
		return err;

	pcm->private_data = emu;
	pcm->private_free = snd_emu10k1_fx8010_pcm_free;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &snd_emu10k1_fx8010_playback_ops);

	pcm->info_flags = 0;
	strcpy(pcm->name, "EMU10K1 FX8010");
	emu->pcm_fx8010 = pcm;
	
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV, snd_dma_pci_data(emu->pci), 64*1024, 0);

	if (rpcm)
		*rpcm = pcm;

	return 0;
}

/*************************************************************************
 * EMU10K1 effect manager
 *************************************************************************/

static void snd_emu10k1_write_op(emu10k1_fx8010_code_t *icode, unsigned int *ptr,
				 u32 op, u32 r, u32 a, u32 x, u32 y)
{
	snd_assert(*ptr < 512, return);
	set_bit(*ptr, icode->code_valid);
	icode->code[*ptr    ][0] = ((x & 0x3ff) << 10) | (y & 0x3ff);
	icode->code[(*ptr)++][1] = ((op & 0x0f) << 20) | ((r & 0x3ff) << 10) | (a & 0x3ff);
}

#define OP(icode, ptr, op, r, a, x, y) \
	snd_emu10k1_write_op(icode, ptr, op, r, a, x, y)

static void snd_emu10k1_audigy_write_op(emu10k1_fx8010_code_t *icode, unsigned int *ptr,
					u32 op, u32 r, u32 a, u32 x, u32 y)
{
	snd_assert(*ptr < 512, return);
	set_bit(*ptr, icode->code_valid);
	icode->code[*ptr    ][0] = ((x & 0x7ff) << 12) | (y & 0x7ff);
	icode->code[(*ptr)++][1] = ((op & 0x0f) << 24) | ((r & 0x7ff) << 12) | (a & 0x7ff);
}

#define A_OP(icode, ptr, op, r, a, x, y) \
	snd_emu10k1_audigy_write_op(icode, ptr, op, r, a, x, y)

void snd_emu10k1_efx_write(emu10k1_t *emu, unsigned int pc, unsigned int data)
{
	pc += emu->audigy ? A_MICROCODEBASE : MICROCODEBASE;
	snd_emu10k1_ptr_write(emu, pc, 0, data);
}

unsigned int snd_emu10k1_efx_read(emu10k1_t *emu, unsigned int pc)
{
	pc += emu->audigy ? A_MICROCODEBASE : MICROCODEBASE;
	return snd_emu10k1_ptr_read(emu, pc, 0);
}

static void snd_emu10k1_gpr_poke(emu10k1_t *emu, emu10k1_fx8010_code_t *icode)
{
	int gpr;

	for (gpr = 0; gpr < 0x100; gpr++) {
		if (!test_bit(gpr, icode->gpr_valid))
			continue;
		snd_emu10k1_ptr_write(emu, emu->gpr_base + gpr, 0, icode->gpr_map[gpr]);
	}
}

static void snd_emu10k1_gpr_peek(emu10k1_t *emu, emu10k1_fx8010_code_t *icode)
{
	int gpr;

	for (gpr = 0; gpr < 0x100; gpr++) {
		set_bit(gpr, icode->gpr_valid);
		icode->gpr_map[gpr] = snd_emu10k1_ptr_read(emu, emu->gpr_base + gpr, 0);
	}
}

static void snd_emu10k1_tram_poke(emu10k1_t *emu, emu10k1_fx8010_code_t *icode)
{
	int tram;

	for (tram = 0; tram < 0xa0; tram++) {
		if (!test_bit(tram, icode->tram_valid))
			continue;
		snd_emu10k1_ptr_write(emu, TANKMEMDATAREGBASE + tram, 0, icode->tram_data_map[tram]);
		snd_emu10k1_ptr_write(emu, TANKMEMADDRREGBASE + tram, 0, icode->tram_addr_map[tram]);
	}
}

static void snd_emu10k1_tram_peek(emu10k1_t *emu, emu10k1_fx8010_code_t *icode)
{
	int tram;

	memset(icode->tram_valid, 0, sizeof(icode->tram_valid));
	for (tram = 0; tram < 0xa0; tram++) {
		set_bit(tram, icode->tram_valid);
		icode->tram_data_map[tram] = snd_emu10k1_ptr_read(emu, TANKMEMDATAREGBASE + tram, 0);
		icode->tram_addr_map[tram] = snd_emu10k1_ptr_read(emu, TANKMEMADDRREGBASE + tram, 0);
	}
}

static void snd_emu10k1_code_poke(emu10k1_t *emu, emu10k1_fx8010_code_t *icode)
{
	u32 pc;

	for (pc = 0; pc < 512; pc++) {
		if (!test_bit(pc, icode->code_valid))
			continue;
		snd_emu10k1_efx_write(emu, pc * 2, icode->code[pc][0]);
		snd_emu10k1_efx_write(emu, pc * 2 + 1, icode->code[pc][1]);
	}
}

static void snd_emu10k1_code_peek(emu10k1_t *emu, emu10k1_fx8010_code_t *icode)
{
	u32 pc;

	memset(icode->code_valid, 0, sizeof(icode->code_valid));
	for (pc = 0; pc < 512; pc++) {
		set_bit(pc, icode->code_valid);
		icode->code[pc][0] = snd_emu10k1_efx_read(emu, pc * 2);
		icode->code[pc][1] = snd_emu10k1_efx_read(emu, pc * 2 + 1);
	}
}

static snd_emu10k1_fx8010_ctl_t *snd_emu10k1_look_for_ctl(emu10k1_t *emu, snd_ctl_elem_id_t *id)
{
	snd_emu10k1_fx8010_ctl_t *ctl;
	snd_kcontrol_t *kcontrol;
	struct list_head *list;
	
	list_for_each(list, &emu->fx8010.gpr_ctl) {
		ctl = emu10k1_gpr_ctl(list);
		kcontrol = ctl->kcontrol;
		if (kcontrol->id.iface == id->iface &&
		    !strcmp(kcontrol->id.name, id->name) &&
		    kcontrol->id.index == id->index)
			return ctl;
	}
	return NULL;
}

static int snd_emu10k1_verify_controls(emu10k1_t *emu, emu10k1_fx8010_code_t *icode)
{
	unsigned int i;
	snd_ctl_elem_id_t __user *_id;
	snd_ctl_elem_id_t id;
	emu10k1_fx8010_control_gpr_t __user *_gctl;
	emu10k1_fx8010_control_gpr_t gctl;
	
	for (i = 0, _id = icode->gpr_del_controls;
	     i < icode->gpr_del_control_count; i++, _id++) {
	     	if (copy_from_user(&id, _id, sizeof(id)))
	     		return -EFAULT;
		if (snd_emu10k1_look_for_ctl(emu, &id) == NULL)
			return -ENOENT;
	}
	for (i = 0, _gctl = icode->gpr_add_controls;
	     i < icode->gpr_add_control_count; i++, _gctl++) {
		if (copy_from_user(&gctl, _gctl, sizeof(gctl)))
			return -EFAULT;
		if (snd_emu10k1_look_for_ctl(emu, &gctl.id))
			continue;
		down_read(&emu->card->controls_rwsem);
		if (snd_ctl_find_id(emu->card, &gctl.id) != NULL) {
			up_read(&emu->card->controls_rwsem);
			return -EEXIST;
		}
		up_read(&emu->card->controls_rwsem);
		if (gctl.id.iface != SNDRV_CTL_ELEM_IFACE_MIXER &&
		    gctl.id.iface != SNDRV_CTL_ELEM_IFACE_PCM)
			return -EINVAL;
	}
	for (i = 0, _gctl = icode->gpr_list_controls;
	     i < icode->gpr_list_control_count; i++, _gctl++) {
	     	/* FIXME: we need to check the WRITE access */
		if (copy_from_user(&gctl, _gctl, sizeof(gctl)))
			return -EFAULT;
	}
	return 0;
}

static void snd_emu10k1_ctl_private_free(snd_kcontrol_t *kctl)
{
	snd_emu10k1_fx8010_ctl_t *ctl;
	
	ctl = (snd_emu10k1_fx8010_ctl_t *)kctl->private_value;
	kctl->private_value = 0;
	list_del(&ctl->list);
	kfree(ctl);
}

static void snd_emu10k1_add_controls(emu10k1_t *emu, emu10k1_fx8010_code_t *icode)
{
	unsigned int i, j;
	emu10k1_fx8010_control_gpr_t __user *_gctl;
	emu10k1_fx8010_control_gpr_t gctl;
	snd_emu10k1_fx8010_ctl_t *ctl, nctl;
	snd_kcontrol_new_t knew;
	snd_kcontrol_t *kctl;
	snd_ctl_elem_value_t *val;

	val = (snd_ctl_elem_value_t *)kmalloc(sizeof(*val), GFP_KERNEL);
	if (!val)
		return;
	for (i = 0, _gctl = icode->gpr_add_controls;
	     i < icode->gpr_add_control_count; i++, _gctl++) {
		if (copy_from_user(&gctl, _gctl, sizeof(gctl)))
			break;
		snd_runtime_check(gctl.id.iface == SNDRV_CTL_ELEM_IFACE_MIXER ||
		                  gctl.id.iface == SNDRV_CTL_ELEM_IFACE_PCM, continue);
		snd_runtime_check(gctl.id.name[0] != '\0', continue);
		ctl = snd_emu10k1_look_for_ctl(emu, &gctl.id);
		memset(&knew, 0, sizeof(knew));
		knew.iface = gctl.id.iface;
		knew.name = gctl.id.name;
		knew.index = gctl.id.index;
		knew.device = gctl.id.device;
		knew.subdevice = gctl.id.subdevice;
		knew.info = snd_emu10k1_gpr_ctl_info;
		knew.get = snd_emu10k1_gpr_ctl_get;
		knew.put = snd_emu10k1_gpr_ctl_put;
		memset(&nctl, 0, sizeof(nctl));
		nctl.vcount = gctl.vcount;
		nctl.count = gctl.count;
		for (j = 0; j < 32; j++) {
			nctl.gpr[j] = gctl.gpr[j];
			nctl.value[j] = ~gctl.value[j];	/* inverted, we want to write new value in gpr_ctl_put() */
			val->value.integer.value[j] = gctl.value[j];
		}
		nctl.min = gctl.min;
		nctl.max = gctl.max;
		nctl.translation = gctl.translation;
		if (ctl == NULL) {
			ctl = (snd_emu10k1_fx8010_ctl_t *)kmalloc(sizeof(*ctl), GFP_KERNEL);
			if (ctl == NULL)
				continue;
			knew.private_value = (unsigned long)ctl;
			memcpy(ctl, &nctl, sizeof(nctl));
			if (snd_ctl_add(emu->card, kctl = snd_ctl_new1(&knew, emu)) < 0) {
				kfree(ctl);
				continue;
			}
			kctl->private_free = snd_emu10k1_ctl_private_free;
			ctl->kcontrol = kctl;
			list_add_tail(&ctl->list, &emu->fx8010.gpr_ctl);
		} else {
			/* overwrite */
			nctl.list = ctl->list;
			nctl.kcontrol = ctl->kcontrol;
			memcpy(ctl, &nctl, sizeof(nctl));
			snd_ctl_notify(emu->card, SNDRV_CTL_EVENT_MASK_VALUE |
			                          SNDRV_CTL_EVENT_MASK_INFO, &ctl->kcontrol->id);
		}
		snd_emu10k1_gpr_ctl_put(ctl->kcontrol, val);
	}
	kfree(val);
}

static void snd_emu10k1_del_controls(emu10k1_t *emu, emu10k1_fx8010_code_t *icode)
{
	unsigned int i;
	snd_ctl_elem_id_t id;
	snd_ctl_elem_id_t __user *_id;
	snd_emu10k1_fx8010_ctl_t *ctl;
	snd_card_t *card = emu->card;
	
	for (i = 0, _id = icode->gpr_del_controls;
	     i < icode->gpr_del_control_count; i++, _id++) {
	     	snd_runtime_check(copy_from_user(&id, _id, sizeof(id)) == 0, continue);
		down_write(&card->controls_rwsem);
		ctl = snd_emu10k1_look_for_ctl(emu, &id);
		if (ctl)
			snd_ctl_remove(card, ctl->kcontrol);
		up_write(&card->controls_rwsem);
	}
}

static int snd_emu10k1_list_controls(emu10k1_t *emu, emu10k1_fx8010_code_t *icode)
{
	unsigned int i = 0, j;
	unsigned int total = 0;
	emu10k1_fx8010_control_gpr_t gctl;
	emu10k1_fx8010_control_gpr_t __user *_gctl;
	snd_emu10k1_fx8010_ctl_t *ctl;
	snd_ctl_elem_id_t *id;
	struct list_head *list;

	_gctl = icode->gpr_list_controls;	
	list_for_each(list, &emu->fx8010.gpr_ctl) {
		ctl = emu10k1_gpr_ctl(list);
		total++;
		if (_gctl && i < icode->gpr_list_control_count) {
			memset(&gctl, 0, sizeof(gctl));
			id = &ctl->kcontrol->id;
			gctl.id.iface = id->iface;
			strlcpy(gctl.id.name, id->name, sizeof(gctl.id.name));
			gctl.id.index = id->index;
			gctl.id.device = id->device;
			gctl.id.subdevice = id->subdevice;
			gctl.vcount = ctl->vcount;
			gctl.count = ctl->count;
			for (j = 0; j < 32; j++) {
				gctl.gpr[j] = ctl->gpr[j];
				gctl.value[j] = ctl->value[j];
			}
			gctl.min = ctl->min;
			gctl.max = ctl->max;
			gctl.translation = ctl->translation;
			if (copy_to_user(_gctl, &gctl, sizeof(gctl)))
				return -EFAULT;
			_gctl++;
			i++;
		}
	}
	icode->gpr_list_control_total = total;
	return 0;
}

static int snd_emu10k1_icode_poke(emu10k1_t *emu, emu10k1_fx8010_code_t *icode)
{
	int err = 0;

	down(&emu->fx8010.lock);
	if ((err = snd_emu10k1_verify_controls(emu, icode)) < 0)
		goto __error;
	strlcpy(emu->fx8010.name, icode->name, sizeof(emu->fx8010.name));
	/* stop FX processor - this may be dangerous, but it's better to miss
	   some samples than generate wrong ones - [jk] */
	if (emu->audigy)
		snd_emu10k1_ptr_write(emu, A_DBG, 0, emu->fx8010.dbg | A_DBG_SINGLE_STEP);
	else
		snd_emu10k1_ptr_write(emu, DBG, 0, emu->fx8010.dbg | EMU10K1_DBG_SINGLE_STEP);
	/* ok, do the main job */
	snd_emu10k1_del_controls(emu, icode);
	snd_emu10k1_gpr_poke(emu, icode);
	snd_emu10k1_tram_poke(emu, icode);
	snd_emu10k1_code_poke(emu, icode);
	snd_emu10k1_add_controls(emu, icode);
	/* start FX processor when the DSP code is updated */
	if (emu->audigy)
		snd_emu10k1_ptr_write(emu, A_DBG, 0, emu->fx8010.dbg);
	else
		snd_emu10k1_ptr_write(emu, DBG, 0, emu->fx8010.dbg);
      __error:
	up(&emu->fx8010.lock);
	return err;
}

static int snd_emu10k1_icode_peek(emu10k1_t *emu, emu10k1_fx8010_code_t *icode)
{
	int err;

	down(&emu->fx8010.lock);
	strlcpy(icode->name, emu->fx8010.name, sizeof(icode->name));
	/* ok, do the main job */
	snd_emu10k1_gpr_peek(emu, icode);
	snd_emu10k1_tram_peek(emu, icode);
	snd_emu10k1_code_peek(emu, icode);
	err = snd_emu10k1_list_controls(emu, icode);
	up(&emu->fx8010.lock);
	return err;
}

static int snd_emu10k1_ipcm_poke(emu10k1_t *emu, emu10k1_fx8010_pcm_t *ipcm)
{
	unsigned int i;
	int err = 0;
	snd_emu10k1_fx8010_pcm_t *pcm;

	if (ipcm->substream >= EMU10K1_FX8010_PCM_COUNT)
		return -EINVAL;
	if (ipcm->channels > 32)
		return -EINVAL;
	pcm = &emu->fx8010.pcm[ipcm->substream];
	down(&emu->fx8010.lock);
	spin_lock_irq(&emu->reg_lock);
	if (pcm->opened) {
		err = -EBUSY;
		goto __error;
	}
	if (ipcm->channels == 0) {	/* remove */
		pcm->valid = 0;
	} else {
		/* FIXME: we need to add universal code to the PCM transfer routine */
		if (ipcm->channels != 2) {
			err = -EINVAL;
			goto __error;
		}
		pcm->valid = 1;
		pcm->opened = 0;
		pcm->channels = ipcm->channels;
		pcm->tram_start = ipcm->tram_start;
		pcm->buffer_size = ipcm->buffer_size;
		pcm->gpr_size = ipcm->gpr_size;
		pcm->gpr_count = ipcm->gpr_count;
		pcm->gpr_tmpcount = ipcm->gpr_tmpcount;
		pcm->gpr_ptr = ipcm->gpr_ptr;
		pcm->gpr_trigger = ipcm->gpr_trigger;
		pcm->gpr_running = ipcm->gpr_running;
		for (i = 0; i < pcm->channels; i++)
			pcm->etram[i] = ipcm->etram[i];
	}
      __error:
	spin_unlock_irq(&emu->reg_lock);
	up(&emu->fx8010.lock);
	return err;
}

static int snd_emu10k1_ipcm_peek(emu10k1_t *emu, emu10k1_fx8010_pcm_t *ipcm)
{
	unsigned int i;
	int err = 0;
	snd_emu10k1_fx8010_pcm_t *pcm;

	if (ipcm->substream >= EMU10K1_FX8010_PCM_COUNT)
		return -EINVAL;
	pcm = &emu->fx8010.pcm[ipcm->substream];
	down(&emu->fx8010.lock);
	spin_lock_irq(&emu->reg_lock);
	ipcm->channels = pcm->channels;
	ipcm->tram_start = pcm->tram_start;
	ipcm->buffer_size = pcm->buffer_size;
	ipcm->gpr_size = pcm->gpr_size;
	ipcm->gpr_ptr = pcm->gpr_ptr;
	ipcm->gpr_count = pcm->gpr_count;
	ipcm->gpr_tmpcount = pcm->gpr_tmpcount;
	ipcm->gpr_trigger = pcm->gpr_trigger;
	ipcm->gpr_running = pcm->gpr_running;
	for (i = 0; i < pcm->channels; i++)
		ipcm->etram[i] = pcm->etram[i];
	ipcm->res1 = ipcm->res2 = 0;
	ipcm->pad = 0;
	spin_unlock_irq(&emu->reg_lock);
	up(&emu->fx8010.lock);
	return err;
}

#define SND_EMU10K1_GPR_CONTROLS	41
#define SND_EMU10K1_INPUTS		10
#define SND_EMU10K1_PLAYBACK_CHANNELS	6
#define SND_EMU10K1_CAPTURE_CHANNELS	4

static void __devinit snd_emu10k1_init_mono_control(emu10k1_fx8010_control_gpr_t *ctl, const char *name, int gpr, int defval)
{
	ctl->id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	strcpy(ctl->id.name, name);
	ctl->vcount = ctl->count = 1;
	ctl->gpr[0] = gpr + 0; ctl->value[0] = defval;
	ctl->min = 0;
	ctl->max = 100;
	ctl->translation = EMU10K1_GPR_TRANSLATION_TABLE100;	
}

static void __devinit snd_emu10k1_init_stereo_control(emu10k1_fx8010_control_gpr_t *ctl, const char *name, int gpr, int defval)
{
	ctl->id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	strcpy(ctl->id.name, name);
	ctl->vcount = ctl->count = 2;
	ctl->gpr[0] = gpr + 0; ctl->value[0] = defval;
	ctl->gpr[1] = gpr + 1; ctl->value[1] = defval;
	ctl->min = 0;
	ctl->max = 100;
	ctl->translation = EMU10K1_GPR_TRANSLATION_TABLE100;
}

static void __devinit snd_emu10k1_init_mono_onoff_control(emu10k1_fx8010_control_gpr_t *ctl, const char *name, int gpr, int defval)
{
	ctl->id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	strcpy(ctl->id.name, name);
	ctl->vcount = ctl->count = 1;
	ctl->gpr[0] = gpr + 0; ctl->value[0] = defval;
	ctl->min = 0;
	ctl->max = 1;
	ctl->translation = EMU10K1_GPR_TRANSLATION_ONOFF;
}

static void __devinit snd_emu10k1_init_stereo_onoff_control(emu10k1_fx8010_control_gpr_t *ctl, const char *name, int gpr, int defval)
{
	ctl->id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	strcpy(ctl->id.name, name);
	ctl->vcount = ctl->count = 2;
	ctl->gpr[0] = gpr + 0; ctl->value[0] = defval;
	ctl->gpr[1] = gpr + 1; ctl->value[1] = defval;
	ctl->min = 0;
	ctl->max = 1;
	ctl->translation = EMU10K1_GPR_TRANSLATION_ONOFF;
}


/*
 * initial DSP configuration for Audigy
 */

static int __devinit _snd_emu10k1_audigy_init_efx(emu10k1_t *emu)
{
	int err, i, z, gpr, nctl;
	const int playback = 10;
	const int capture = playback + (SND_EMU10K1_PLAYBACK_CHANNELS * 2); /* we reserve 10 voices */
	const int stereo_mix = capture + 2;
	const int tmp = 0x88;
	u32 ptr;
	emu10k1_fx8010_code_t *icode;
	emu10k1_fx8010_control_gpr_t *controls, *ctl;
	mm_segment_t seg;

	spin_lock_init(&emu->fx8010.irq_lock);
	INIT_LIST_HEAD(&emu->fx8010.gpr_ctl);

	if ((icode = snd_kcalloc(sizeof(emu10k1_fx8010_code_t), GFP_KERNEL)) == NULL)
		return -ENOMEM;
	if ((controls = snd_kcalloc(sizeof(emu10k1_fx8010_control_gpr_t) * SND_EMU10K1_GPR_CONTROLS, GFP_KERNEL)) == NULL) {
		kfree(icode);
		return -ENOMEM;
	}

	/* clear free GPRs */
	for (i = 0; i < 256; i++)
		set_bit(i, icode->gpr_valid);

	strcpy(icode->name, "Audigy DSP code for ALSA");
	ptr = 0;
	nctl = 0;
	gpr = stereo_mix + 10;

	/* stop FX processor */
	snd_emu10k1_ptr_write(emu, A_DBG, 0, (emu->fx8010.dbg = 0) | A_DBG_SINGLE_STEP);

	/* PCM front Playback Volume (independent from stereo mix) */
	A_OP(icode, &ptr, iMAC0, A_GPR(playback), A_C_00000000, A_GPR(gpr), A_FXBUS(FXBUS_PCM_LEFT_FRONT));
	A_OP(icode, &ptr, iMAC0, A_GPR(playback+1), A_C_00000000, A_GPR(gpr+1), A_FXBUS(FXBUS_PCM_RIGHT_FRONT));
	snd_emu10k1_init_stereo_control(&controls[nctl++], "PCM Front Playback Volume", gpr, 100);
	gpr += 2;
	
	/* PCM Surround Playback (independent from stereo mix) */
	A_OP(icode, &ptr, iMAC0, A_GPR(playback+2), A_C_00000000, A_GPR(gpr), A_FXBUS(FXBUS_PCM_LEFT_REAR));
	A_OP(icode, &ptr, iMAC0, A_GPR(playback+3), A_C_00000000, A_GPR(gpr+1), A_FXBUS(FXBUS_PCM_RIGHT_REAR));
	snd_emu10k1_init_stereo_control(&controls[nctl++], "PCM Surround Playback Volume", gpr, 100);
	gpr += 2;

	/* PCM Center Playback (independent from stereo mix) */
	A_OP(icode, &ptr, iMAC0, A_GPR(playback+4), A_C_00000000, A_GPR(gpr), A_FXBUS(FXBUS_PCM_CENTER));
	snd_emu10k1_init_mono_control(&controls[nctl++], "PCM Center Playback Volume", gpr, 100);
	gpr++;

	/* PCM LFE Playback (independent from stereo mix) */
	A_OP(icode, &ptr, iMAC0, A_GPR(playback+5), A_C_00000000, A_GPR(gpr), A_FXBUS(FXBUS_PCM_LFE));
	snd_emu10k1_init_mono_control(&controls[nctl++], "PCM LFE Playback Volume", gpr, 100);
	gpr++;
	
	/*
	 * Stereo Mix
	 */
	/* Wave (PCM) Playback Volume (will be renamed later) */
	A_OP(icode, &ptr, iMAC0, A_GPR(stereo_mix), A_C_00000000, A_GPR(gpr), A_FXBUS(FXBUS_PCM_LEFT));
	A_OP(icode, &ptr, iMAC0, A_GPR(stereo_mix+1), A_C_00000000, A_GPR(gpr+1), A_FXBUS(FXBUS_PCM_RIGHT));
	snd_emu10k1_init_stereo_control(&controls[nctl++], "Wave Playback Volume", gpr, 100);
	gpr += 2;

	/* Music Playback */
	A_OP(icode, &ptr, iMAC0, A_GPR(stereo_mix+0), A_GPR(stereo_mix+0), A_GPR(gpr), A_FXBUS(FXBUS_MIDI_LEFT));
	A_OP(icode, &ptr, iMAC0, A_GPR(stereo_mix+1), A_GPR(stereo_mix+1), A_GPR(gpr+1), A_FXBUS(FXBUS_MIDI_RIGHT));
	snd_emu10k1_init_stereo_control(&controls[nctl++], "Music Playback Volume", gpr, 100);
	gpr += 2;

	/* Wave (PCM) Capture */
	A_OP(icode, &ptr, iMAC0, A_GPR(capture+0), A_C_00000000, A_GPR(gpr), A_FXBUS(FXBUS_PCM_LEFT));
	A_OP(icode, &ptr, iMAC0, A_GPR(capture+1), A_C_00000000, A_GPR(gpr+1), A_FXBUS(FXBUS_PCM_RIGHT));
	snd_emu10k1_init_stereo_control(&controls[nctl++], "PCM Capture Volume", gpr, 0);
	gpr += 2;

	/* Music Capture */
	A_OP(icode, &ptr, iMAC0, A_GPR(capture+0), A_GPR(capture+0), A_GPR(gpr), A_FXBUS(FXBUS_MIDI_LEFT));
	A_OP(icode, &ptr, iMAC0, A_GPR(capture+1), A_GPR(capture+1), A_GPR(gpr+1), A_FXBUS(FXBUS_MIDI_RIGHT));
	snd_emu10k1_init_stereo_control(&controls[nctl++], "Music Capture Volume", gpr, 0);
	gpr += 2;

	/*
	 * inputs
	 */
#define A_ADD_VOLUME_IN(var,vol,input) \
A_OP(icode, &ptr, iMAC0, A_GPR(var), A_GPR(var), A_GPR(vol), A_EXTIN(input))

	/* AC'97 Playback Volume - used only for mic (renamed later) */
	A_ADD_VOLUME_IN(stereo_mix, gpr, A_EXTIN_AC97_L);
	A_ADD_VOLUME_IN(stereo_mix+1, gpr+1, A_EXTIN_AC97_R);
	snd_emu10k1_init_stereo_control(&controls[nctl++], "AMic Playback Volume", gpr, 0);
	gpr += 2;
	/* AC'97 Capture Volume - used only for mic */
	A_ADD_VOLUME_IN(capture, gpr, A_EXTIN_AC97_L);
	A_ADD_VOLUME_IN(capture+1, gpr+1, A_EXTIN_AC97_R);
	snd_emu10k1_init_stereo_control(&controls[nctl++], "Mic Capture Volume", gpr, 0);
	gpr += 2;

	/* mic capture buffer */	
	A_OP(icode, &ptr, iINTERP, A_EXTOUT(A_EXTOUT_MIC_CAP), A_EXTIN(A_EXTIN_AC97_L), 0xcd, A_EXTIN(A_EXTIN_AC97_R));

	/* Audigy CD Playback Volume */
	A_ADD_VOLUME_IN(stereo_mix, gpr, A_EXTIN_SPDIF_CD_L);
	A_ADD_VOLUME_IN(stereo_mix+1, gpr+1, A_EXTIN_SPDIF_CD_R);
	snd_emu10k1_init_stereo_control(&controls[nctl++],
					emu->no_ac97 ? "CD Playback Volume" : "Audigy CD Playback Volume",
					gpr, 0);
	gpr += 2;
	/* Audigy CD Capture Volume */
	A_ADD_VOLUME_IN(capture, gpr, A_EXTIN_SPDIF_CD_L);
	A_ADD_VOLUME_IN(capture+1, gpr+1, A_EXTIN_SPDIF_CD_R);
	snd_emu10k1_init_stereo_control(&controls[nctl++],
					emu->no_ac97 ? "CD Capture Volume" : "Audigy CD Capture Volume",
					gpr, 0);
	gpr += 2;

 	/* Optical SPDIF Playback Volume */
	A_ADD_VOLUME_IN(stereo_mix, gpr, A_EXTIN_OPT_SPDIF_L);
	A_ADD_VOLUME_IN(stereo_mix+1, gpr+1, A_EXTIN_OPT_SPDIF_R);
	snd_emu10k1_init_stereo_control(&controls[nctl++], "IEC958 Optical Playback Volume", gpr, 0);
	gpr += 2;
	/* Optical SPDIF Capture Volume */
	A_ADD_VOLUME_IN(capture, gpr, A_EXTIN_OPT_SPDIF_L);
	A_ADD_VOLUME_IN(capture+1, gpr+1, A_EXTIN_OPT_SPDIF_R);
	snd_emu10k1_init_stereo_control(&controls[nctl++], "IEC958 Optical Capture Volume", gpr, 0);
	gpr += 2;

	/* Line2 Playback Volume */
	A_ADD_VOLUME_IN(stereo_mix, gpr, A_EXTIN_LINE2_L);
	A_ADD_VOLUME_IN(stereo_mix+1, gpr+1, A_EXTIN_LINE2_R);
	snd_emu10k1_init_stereo_control(&controls[nctl++],
					emu->no_ac97 ? "Line Playback Volume" : "Line2 Playback Volume",
					gpr, 0);
	gpr += 2;
	/* Line2 Capture Volume */
	A_ADD_VOLUME_IN(capture, gpr, A_EXTIN_LINE2_L);
	A_ADD_VOLUME_IN(capture+1, gpr+1, A_EXTIN_LINE2_R);
	snd_emu10k1_init_stereo_control(&controls[nctl++],
					emu->no_ac97 ? "Line Capture Volume" : "Line2 Capture Volume",
					gpr, 0);
	gpr += 2;
        
	/* Philips ADC Playback Volume */
	A_ADD_VOLUME_IN(stereo_mix, gpr, A_EXTIN_ADC_L);
	A_ADD_VOLUME_IN(stereo_mix+1, gpr+1, A_EXTIN_ADC_R);
	snd_emu10k1_init_stereo_control(&controls[nctl++], "Analog Mix Playback Volume", gpr, 0);
	gpr += 2;
	/* Philips ADC Capture Volume */
	A_ADD_VOLUME_IN(capture, gpr, A_EXTIN_ADC_L);
	A_ADD_VOLUME_IN(capture+1, gpr+1, A_EXTIN_ADC_R);
	snd_emu10k1_init_stereo_control(&controls[nctl++], "Analog Mix Capture Volume", gpr, 0);
	gpr += 2;

	/* Aux2 Playback Volume */
	A_ADD_VOLUME_IN(stereo_mix, gpr, A_EXTIN_AUX2_L);
	A_ADD_VOLUME_IN(stereo_mix+1, gpr+1, A_EXTIN_AUX2_R);
	snd_emu10k1_init_stereo_control(&controls[nctl++],
					emu->no_ac97 ? "Aux Playback Volume" : "Aux2 Playback Volume",
					gpr, 0);
	gpr += 2;
	/* Aux2 Capture Volume */
	A_ADD_VOLUME_IN(capture, gpr, A_EXTIN_AUX2_L);
	A_ADD_VOLUME_IN(capture+1, gpr+1, A_EXTIN_AUX2_R);
	snd_emu10k1_init_stereo_control(&controls[nctl++],
					emu->no_ac97 ? "Aux Capture Volume" : "Aux2 Capture Volume",
					gpr, 0);
	gpr += 2;
	
	/* Stereo Mix Front Playback Volume */
	A_OP(icode, &ptr, iMAC0, A_GPR(playback), A_GPR(playback), A_GPR(gpr), A_GPR(stereo_mix));
	A_OP(icode, &ptr, iMAC0, A_GPR(playback+1), A_GPR(playback+1), A_GPR(gpr+1), A_GPR(stereo_mix+1));
	snd_emu10k1_init_stereo_control(&controls[nctl++], "Front Playback Volume", gpr, 100);
	gpr += 2;
	
	/* Stereo Mix Surround Playback */
	A_OP(icode, &ptr, iMAC0, A_GPR(playback+2), A_GPR(playback+2), A_GPR(gpr), A_GPR(stereo_mix));
	A_OP(icode, &ptr, iMAC0, A_GPR(playback+3), A_GPR(playback+3), A_GPR(gpr+1), A_GPR(stereo_mix+1));
	snd_emu10k1_init_stereo_control(&controls[nctl++], "Surround Playback Volume", gpr, 0);
	gpr += 2;

	/* Stereo Mix Center Playback */
	/* Center = sub = Left/2 + Right/2 */
	A_OP(icode, &ptr, iINTERP, A_GPR(tmp), A_GPR(stereo_mix), 0xcd, A_GPR(stereo_mix+1));
	A_OP(icode, &ptr, iMAC0, A_GPR(playback+4), A_GPR(playback+4), A_GPR(gpr), A_GPR(tmp));
	snd_emu10k1_init_mono_control(&controls[nctl++], "Center Playback Volume", gpr, 0);
	gpr++;

	/* Stereo Mix LFE Playback */
	A_OP(icode, &ptr, iMAC0, A_GPR(playback+5), A_GPR(playback+5), A_GPR(gpr), A_GPR(tmp));
	snd_emu10k1_init_mono_control(&controls[nctl++], "LFE Playback Volume", gpr, 0);
	gpr++;

	/*
	 * outputs
	 */
#define A_PUT_OUTPUT(out,src) A_OP(icode, &ptr, iACC3, A_EXTOUT(out), A_C_00000000, A_C_00000000, A_GPR(src))
#define A_PUT_STEREO_OUTPUT(out1,out2,src) \
	{A_PUT_OUTPUT(out1,src); A_PUT_OUTPUT(out2,src+1);}

#define _A_SWITCH(icode, ptr, dst, src, sw) \
	A_OP((icode), ptr, iMACINT0, dst, A_C_00000000, src, sw);
#define A_SWITCH(icode, ptr, dst, src, sw) \
		_A_SWITCH(icode, ptr, A_GPR(dst), A_GPR(src), A_GPR(sw))
#define _A_SWITCH_NEG(icode, ptr, dst, src) \
	A_OP((icode), ptr, iANDXOR, dst, src, A_C_00000001, A_C_00000001);
#define A_SWITCH_NEG(icode, ptr, dst, src) \
		_A_SWITCH_NEG(icode, ptr, A_GPR(dst), A_GPR(src))


	/*
	 *  Process tone control
	 */
	A_OP(icode, &ptr, iACC3, A_GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 0), A_GPR(playback + 0), A_C_00000000, A_C_00000000); /* left */
	A_OP(icode, &ptr, iACC3, A_GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 1), A_GPR(playback + 1), A_C_00000000, A_C_00000000); /* right */
	A_OP(icode, &ptr, iACC3, A_GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 2), A_GPR(playback + 2), A_C_00000000, A_C_00000000); /* rear left */
	A_OP(icode, &ptr, iACC3, A_GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 3), A_GPR(playback + 3), A_C_00000000, A_C_00000000); /* rear right */
	A_OP(icode, &ptr, iACC3, A_GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 4), A_GPR(playback + 4), A_C_00000000, A_C_00000000); /* center */
	A_OP(icode, &ptr, iACC3, A_GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 5), A_GPR(playback + 5), A_C_00000000, A_C_00000000); /* LFE */

	ctl = &controls[nctl + 0];
	ctl->id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	strcpy(ctl->id.name, "Tone Control - Bass");
	ctl->vcount = 2;
	ctl->count = 10;
	ctl->min = 0;
	ctl->max = 40;
	ctl->value[0] = ctl->value[1] = 20;
	ctl->translation = EMU10K1_GPR_TRANSLATION_BASS;
	ctl = &controls[nctl + 1];
	ctl->id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	strcpy(ctl->id.name, "Tone Control - Treble");
	ctl->vcount = 2;
	ctl->count = 10;
	ctl->min = 0;
	ctl->max = 40;
	ctl->value[0] = ctl->value[1] = 20;
	ctl->translation = EMU10K1_GPR_TRANSLATION_TREBLE;

#define BASS_GPR	0x8c
#define TREBLE_GPR	0x96

	for (z = 0; z < 5; z++) {
		int j;
		for (j = 0; j < 2; j++) {
			controls[nctl + 0].gpr[z * 2 + j] = BASS_GPR + z * 2 + j;
			controls[nctl + 1].gpr[z * 2 + j] = TREBLE_GPR + z * 2 + j;
		}
	}
	for (z = 0; z < 3; z++) {		/* front/rear/center-lfe */
		int j, k, l, d;
		for (j = 0; j < 2; j++) {	/* left/right */
			k = 0xb0 + (z * 8) + (j * 4);
			l = 0xe0 + (z * 8) + (j * 4);
			d = playback + SND_EMU10K1_PLAYBACK_CHANNELS + z * 2 + j;

			A_OP(icode, &ptr, iMAC0, A_C_00000000, A_C_00000000, A_GPR(d), A_GPR(BASS_GPR + 0 + j));
			A_OP(icode, &ptr, iMACMV, A_GPR(k+1), A_GPR(k), A_GPR(k+1), A_GPR(BASS_GPR + 4 + j));
			A_OP(icode, &ptr, iMACMV, A_GPR(k), A_GPR(d), A_GPR(k), A_GPR(BASS_GPR + 2 + j));
			A_OP(icode, &ptr, iMACMV, A_GPR(k+3), A_GPR(k+2), A_GPR(k+3), A_GPR(BASS_GPR + 8 + j));
			A_OP(icode, &ptr, iMAC0, A_GPR(k+2), A_GPR_ACCU, A_GPR(k+2), A_GPR(BASS_GPR + 6 + j));
			A_OP(icode, &ptr, iACC3, A_GPR(k+2), A_GPR(k+2), A_GPR(k+2), A_C_00000000);

			A_OP(icode, &ptr, iMAC0, A_C_00000000, A_C_00000000, A_GPR(k+2), A_GPR(TREBLE_GPR + 0 + j));
			A_OP(icode, &ptr, iMACMV, A_GPR(l+1), A_GPR(l), A_GPR(l+1), A_GPR(TREBLE_GPR + 4 + j));
			A_OP(icode, &ptr, iMACMV, A_GPR(l), A_GPR(k+2), A_GPR(l), A_GPR(TREBLE_GPR + 2 + j));
			A_OP(icode, &ptr, iMACMV, A_GPR(l+3), A_GPR(l+2), A_GPR(l+3), A_GPR(TREBLE_GPR + 8 + j));
			A_OP(icode, &ptr, iMAC0, A_GPR(l+2), A_GPR_ACCU, A_GPR(l+2), A_GPR(TREBLE_GPR + 6 + j));
			A_OP(icode, &ptr, iMACINT0, A_GPR(l+2), A_C_00000000, A_GPR(l+2), A_C_00000010);

			A_OP(icode, &ptr, iACC3, A_GPR(d), A_GPR(l+2), A_C_00000000, A_C_00000000);

			if (z == 2)	/* center */
				break;
		}
	}
	nctl += 2;

#undef BASS_GPR
#undef TREBLE_GPR

	for (z = 0; z < 6; z++) {
		A_SWITCH(icode, &ptr, tmp + 0, playback + SND_EMU10K1_PLAYBACK_CHANNELS + z, gpr + 0);
		A_SWITCH_NEG(icode, &ptr, tmp + 1, gpr + 0);
		A_SWITCH(icode, &ptr, tmp + 1, playback + z, tmp + 1);
		A_OP(icode, &ptr, iACC3, A_GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + z), A_GPR(tmp + 0), A_GPR(tmp + 1), A_C_00000000);
	}
	snd_emu10k1_init_stereo_onoff_control(controls + nctl++, "Tone Control - Switch", gpr, 0);
	gpr += 2;

	/* Master volume (will be renamed later) */
	A_OP(icode, &ptr, iMAC0, A_GPR(playback+0+SND_EMU10K1_PLAYBACK_CHANNELS), A_C_00000000, A_GPR(gpr), A_GPR(playback+0+SND_EMU10K1_PLAYBACK_CHANNELS));
	A_OP(icode, &ptr, iMAC0, A_GPR(playback+1+SND_EMU10K1_PLAYBACK_CHANNELS), A_C_00000000, A_GPR(gpr+1), A_GPR(playback+1+SND_EMU10K1_PLAYBACK_CHANNELS));
	A_OP(icode, &ptr, iMAC0, A_GPR(playback+2+SND_EMU10K1_PLAYBACK_CHANNELS), A_C_00000000, A_GPR(gpr+1), A_GPR(playback+2+SND_EMU10K1_PLAYBACK_CHANNELS));
	A_OP(icode, &ptr, iMAC0, A_GPR(playback+3+SND_EMU10K1_PLAYBACK_CHANNELS), A_C_00000000, A_GPR(gpr+1), A_GPR(playback+3+SND_EMU10K1_PLAYBACK_CHANNELS));
	A_OP(icode, &ptr, iMAC0, A_GPR(playback+4+SND_EMU10K1_PLAYBACK_CHANNELS), A_C_00000000, A_GPR(gpr+1), A_GPR(playback+4+SND_EMU10K1_PLAYBACK_CHANNELS));
	A_OP(icode, &ptr, iMAC0, A_GPR(playback+5+SND_EMU10K1_PLAYBACK_CHANNELS), A_C_00000000, A_GPR(gpr+1), A_GPR(playback+5+SND_EMU10K1_PLAYBACK_CHANNELS));
	snd_emu10k1_init_stereo_control(&controls[nctl++], "Wave Master Playback Volume", gpr, 0);
	gpr += 2;

	/* analog speakers */
	A_PUT_STEREO_OUTPUT(A_EXTOUT_AFRONT_L, A_EXTOUT_AFRONT_R, playback + SND_EMU10K1_PLAYBACK_CHANNELS);
	A_PUT_STEREO_OUTPUT(A_EXTOUT_AREAR_L, A_EXTOUT_AREAR_R, playback+2 + SND_EMU10K1_PLAYBACK_CHANNELS);
	A_PUT_OUTPUT(A_EXTOUT_ACENTER, playback+4 + SND_EMU10K1_PLAYBACK_CHANNELS);
	A_PUT_OUTPUT(A_EXTOUT_ALFE, playback+5 + SND_EMU10K1_PLAYBACK_CHANNELS);

	/* headphone */
	A_PUT_STEREO_OUTPUT(A_EXTOUT_HEADPHONE_L, A_EXTOUT_HEADPHONE_R, playback + SND_EMU10K1_PLAYBACK_CHANNELS);

	/* digital outputs */
	/* A_PUT_STEREO_OUTPUT(A_EXTOUT_FRONT_L, A_EXTOUT_FRONT_R, playback + SND_EMU10K1_PLAYBACK_CHANNELS); */

	/* IEC958 Optical Raw Playback Switch */ 
	icode->gpr_map[gpr++] = 0x1008;
	icode->gpr_map[gpr++] = 0xffff0000;
	for (z = 0; z < 2; z++) {
		A_OP(icode, &ptr, iMAC0, A_GPR(tmp + 2), A_FXBUS(FXBUS_PT_LEFT + z), A_C_00000000, A_C_00000000);
		A_OP(icode, &ptr, iSKIP, A_GPR_COND, A_GPR_COND, A_GPR(gpr - 2), A_C_00000001);
		A_OP(icode, &ptr, iACC3, A_GPR(tmp + 2), A_C_00000000, A_C_00010000, A_GPR(tmp + 2));
		A_OP(icode, &ptr, iANDXOR, A_GPR(tmp + 2), A_GPR(tmp + 2), A_GPR(gpr - 1), A_C_00000000);
		A_SWITCH(icode, &ptr, tmp + 0, tmp + 2, gpr + z);
		A_SWITCH_NEG(icode, &ptr, tmp + 1, gpr + z);
		A_SWITCH(icode, &ptr, tmp + 1, playback + SND_EMU10K1_PLAYBACK_CHANNELS + z, tmp + 1);
		A_OP(icode, &ptr, iACC3, A_EXTOUT(A_EXTOUT_FRONT_L + z), A_GPR(tmp + 0), A_GPR(tmp + 1), A_C_00000000);
	}
	snd_emu10k1_init_stereo_onoff_control(controls + nctl++, "IEC958 Optical Raw Playback Switch", gpr, 0);
	gpr += 2;
	
	A_PUT_STEREO_OUTPUT(A_EXTOUT_REAR_L, A_EXTOUT_REAR_R, playback+2 + SND_EMU10K1_PLAYBACK_CHANNELS);
	A_PUT_OUTPUT(A_EXTOUT_CENTER, playback+4 + SND_EMU10K1_PLAYBACK_CHANNELS);
	A_PUT_OUTPUT(A_EXTOUT_LFE, playback+5 + SND_EMU10K1_PLAYBACK_CHANNELS);

	/* ADC buffer */
	A_PUT_OUTPUT(A_EXTOUT_ADC_CAP_L, capture);
	A_PUT_OUTPUT(A_EXTOUT_ADC_CAP_R, capture+1);

	/*
	 * ok, set up done..
	 */

	if (gpr > tmp) {
		snd_BUG();
		err = -EIO;
		goto __err;
	}
	/* clear remaining instruction memory */
	while (ptr < 0x200)
		A_OP(icode, &ptr, 0x0f, 0xc0, 0xc0, 0xcf, 0xc0);

	seg = snd_enter_user();
	icode->gpr_add_control_count = nctl;
	icode->gpr_add_controls = controls;
	err = snd_emu10k1_icode_poke(emu, icode);
	snd_leave_user(seg);

 __err:
	kfree(controls);
	kfree(icode);
	return err;
}


/*
 * initial DSP configuration for Emu10k1
 */

/* when volume = max, then copy only to avoid volume modification */
/* with iMAC0 (negative values) */
static void __devinit _volume(emu10k1_fx8010_code_t *icode, u32 *ptr, u32 dst, u32 src, u32 vol)
{
	OP(icode, ptr, iMAC0, dst, C_00000000, src, vol);
	OP(icode, ptr, iANDXOR, C_00000000, vol, C_ffffffff, C_7fffffff);
	OP(icode, ptr, iSKIP, GPR_COND, GPR_COND, CC_REG_NONZERO, C_00000001);
	OP(icode, ptr, iACC3, dst, src, C_00000000, C_00000000);
}
static void __devinit _volume_add(emu10k1_fx8010_code_t *icode, u32 *ptr, u32 dst, u32 src, u32 vol)
{
	OP(icode, ptr, iANDXOR, C_00000000, vol, C_ffffffff, C_7fffffff);
	OP(icode, ptr, iSKIP, GPR_COND, GPR_COND, CC_REG_NONZERO, C_00000002);
	OP(icode, ptr, iMACINT0, dst, dst, src, C_00000001);
	OP(icode, ptr, iSKIP, C_00000000, C_7fffffff, C_7fffffff, C_00000001);
	OP(icode, ptr, iMAC0, dst, dst, src, vol);
}
static void __devinit _volume_out(emu10k1_fx8010_code_t *icode, u32 *ptr, u32 dst, u32 src, u32 vol)
{
	OP(icode, ptr, iANDXOR, C_00000000, vol, C_ffffffff, C_7fffffff);
	OP(icode, ptr, iSKIP, GPR_COND, GPR_COND, CC_REG_NONZERO, C_00000002);
	OP(icode, ptr, iACC3, dst, src, C_00000000, C_00000000);
	OP(icode, ptr, iSKIP, C_00000000, C_7fffffff, C_7fffffff, C_00000001);
	OP(icode, ptr, iMAC0, dst, C_00000000, src, vol);
}

#define VOLUME(icode, ptr, dst, src, vol) \
		_volume(icode, ptr, GPR(dst), GPR(src), GPR(vol))
#define VOLUME_IN(icode, ptr, dst, src, vol) \
		_volume(icode, ptr, GPR(dst), EXTIN(src), GPR(vol))
#define VOLUME_ADD(icode, ptr, dst, src, vol) \
		_volume_add(icode, ptr, GPR(dst), GPR(src), GPR(vol))
#define VOLUME_ADDIN(icode, ptr, dst, src, vol) \
		_volume_add(icode, ptr, GPR(dst), EXTIN(src), GPR(vol))
#define VOLUME_OUT(icode, ptr, dst, src, vol) \
		_volume_out(icode, ptr, EXTOUT(dst), GPR(src), GPR(vol))
#define _SWITCH(icode, ptr, dst, src, sw) \
	OP((icode), ptr, iMACINT0, dst, C_00000000, src, sw);
#define SWITCH(icode, ptr, dst, src, sw) \
		_SWITCH(icode, ptr, GPR(dst), GPR(src), GPR(sw))
#define SWITCH_IN(icode, ptr, dst, src, sw) \
		_SWITCH(icode, ptr, GPR(dst), EXTIN(src), GPR(sw))
#define _SWITCH_NEG(icode, ptr, dst, src) \
	OP((icode), ptr, iANDXOR, dst, src, C_00000001, C_00000001);
#define SWITCH_NEG(icode, ptr, dst, src) \
		_SWITCH_NEG(icode, ptr, GPR(dst), GPR(src))


static int __devinit _snd_emu10k1_init_efx(emu10k1_t *emu)
{
	int err, i, z, gpr, tmp, playback, capture;
	u32 ptr;
	emu10k1_fx8010_code_t *icode;
	emu10k1_fx8010_pcm_t *ipcm;
	emu10k1_fx8010_control_gpr_t *controls, *ctl;
	mm_segment_t seg;

	spin_lock_init(&emu->fx8010.irq_lock);
	INIT_LIST_HEAD(&emu->fx8010.gpr_ctl);

	if ((icode = snd_kcalloc(sizeof(emu10k1_fx8010_code_t), GFP_KERNEL)) == NULL)
		return -ENOMEM;
	if ((controls = snd_kcalloc(sizeof(emu10k1_fx8010_control_gpr_t) * SND_EMU10K1_GPR_CONTROLS, GFP_KERNEL)) == NULL) {
		kfree(icode);
		return -ENOMEM;
	}
	if ((ipcm = snd_kcalloc(sizeof(emu10k1_fx8010_pcm_t), GFP_KERNEL)) == NULL) {
		kfree(controls);
		kfree(icode);
		return -ENOMEM;
	}
	
	/* clear free GPRs */
	for (i = 0; i < 256; i++)
		set_bit(i, icode->gpr_valid);

	/* clear TRAM data & address lines */
	for (i = 0; i < 160; i++)
		set_bit(i, icode->tram_valid);

	strcpy(icode->name, "SB Live! FX8010 code for ALSA v1.2 by Jaroslav Kysela");
	ptr = 0; i = 0;
	/* we have 10 inputs */
	playback = SND_EMU10K1_INPUTS;
	/* we have 6 playback channels and tone control doubles */
	capture = playback + (SND_EMU10K1_PLAYBACK_CHANNELS * 2);
	gpr = capture + SND_EMU10K1_CAPTURE_CHANNELS;
	tmp = 0x88;	/* we need 4 temporary GPR */
	/* from 0x8c to 0xff is the area for tone control */

	/* stop FX processor */
	snd_emu10k1_ptr_write(emu, DBG, 0, (emu->fx8010.dbg = 0) | EMU10K1_DBG_SINGLE_STEP);

	/*
	 *  Process FX Buses
	 */
	OP(icode, &ptr, iMACINT0, GPR(0), C_00000000, FXBUS(FXBUS_PCM_LEFT), C_00000004);
	OP(icode, &ptr, iMACINT0, GPR(1), C_00000000, FXBUS(FXBUS_PCM_RIGHT), C_00000004);
	OP(icode, &ptr, iMACINT0, GPR(2), C_00000000, FXBUS(FXBUS_MIDI_LEFT), C_00000004);
	OP(icode, &ptr, iMACINT0, GPR(3), C_00000000, FXBUS(FXBUS_MIDI_RIGHT), C_00000004);
	OP(icode, &ptr, iMACINT0, GPR(4), C_00000000, FXBUS(FXBUS_PCM_LEFT_REAR), C_00000004);
	OP(icode, &ptr, iMACINT0, GPR(5), C_00000000, FXBUS(FXBUS_PCM_RIGHT_REAR), C_00000004);
	OP(icode, &ptr, iMACINT0, GPR(6), C_00000000, FXBUS(FXBUS_PCM_CENTER), C_00000004);
	OP(icode, &ptr, iMACINT0, GPR(7), C_00000000, FXBUS(FXBUS_PCM_LFE), C_00000004);
	OP(icode, &ptr, iMACINT0, GPR(8), C_00000000, C_00000000, C_00000000);	/* S/PDIF left */
	OP(icode, &ptr, iMACINT0, GPR(9), C_00000000, C_00000000, C_00000000);	/* S/PDIF right */

	/* Raw S/PDIF PCM */
	ipcm->substream = 0;
	ipcm->channels = 2;
	ipcm->tram_start = 0;
	ipcm->buffer_size = (64 * 1024) / 2;
	ipcm->gpr_size = gpr++;
	ipcm->gpr_ptr = gpr++;
	ipcm->gpr_count = gpr++;
	ipcm->gpr_tmpcount = gpr++;
	ipcm->gpr_trigger = gpr++;
	ipcm->gpr_running = gpr++;
	ipcm->etram[0] = 0;
	ipcm->etram[1] = 1;

	icode->gpr_map[gpr + 0] = 0xfffff000;
	icode->gpr_map[gpr + 1] = 0xffff0000;
	icode->gpr_map[gpr + 2] = 0x70000000;
	icode->gpr_map[gpr + 3] = 0x00000007;
	icode->gpr_map[gpr + 4] = 0x001f << 11;
	icode->gpr_map[gpr + 5] = 0x001c << 11;
	icode->gpr_map[gpr + 6] = (0x22  - 0x01) - 1;	/* skip at 01 to 22 */
	icode->gpr_map[gpr + 7] = (0x22  - 0x06) - 1;	/* skip at 06 to 22 */
	icode->gpr_map[gpr + 8] = 0x2000000 + (2<<11);
	icode->gpr_map[gpr + 9] = 0x4000000 + (2<<11);
	icode->gpr_map[gpr + 10] = 1<<11;
	icode->gpr_map[gpr + 11] = (0x24 - 0x0a) - 1;	/* skip at 0a to 24 */
	icode->gpr_map[gpr + 12] = 0;

	/* if the trigger flag is not set, skip */
	/* 00: */ OP(icode, &ptr, iMAC0, C_00000000, GPR(ipcm->gpr_trigger), C_00000000, C_00000000);
	/* 01: */ OP(icode, &ptr, iSKIP, GPR_COND, GPR_COND, CC_REG_ZERO, GPR(gpr + 6));
	/* if the running flag is set, we're running */
	/* 02: */ OP(icode, &ptr, iMAC0, C_00000000, GPR(ipcm->gpr_running), C_00000000, C_00000000);
	/* 03: */ OP(icode, &ptr, iSKIP, GPR_COND, GPR_COND, CC_REG_NONZERO, C_00000004);
	/* wait until ((GPR_DBAC>>11) & 0x1f) == 0x1c) */
	/* 04: */ OP(icode, &ptr, iANDXOR, GPR(tmp + 0), GPR_DBAC, GPR(gpr + 4), C_00000000);
	/* 05: */ OP(icode, &ptr, iMACINT0, C_00000000, GPR(tmp + 0), C_ffffffff, GPR(gpr + 5));
	/* 06: */ OP(icode, &ptr, iSKIP, GPR_COND, GPR_COND, CC_REG_NONZERO, GPR(gpr + 7));
	/* 07: */ OP(icode, &ptr, iACC3, GPR(gpr + 12), C_00000010, C_00000001, C_00000000);

	/* 08: */ OP(icode, &ptr, iANDXOR, GPR(ipcm->gpr_running), GPR(ipcm->gpr_running), C_00000000, C_00000001);
	/* 09: */ OP(icode, &ptr, iACC3, GPR(gpr + 12), GPR(gpr + 12), C_ffffffff, C_00000000);
	/* 0a: */ OP(icode, &ptr, iSKIP, GPR_COND, GPR_COND, CC_REG_NONZERO, GPR(gpr + 11));
	/* 0b: */ OP(icode, &ptr, iACC3, GPR(gpr + 12), C_00000001, C_00000000, C_00000000);

	/* 0c: */ OP(icode, &ptr, iANDXOR, GPR(tmp + 0), ETRAM_DATA(ipcm->etram[0]), GPR(gpr + 0), C_00000000);
	/* 0d: */ OP(icode, &ptr, iLOG, GPR(tmp + 0), GPR(tmp + 0), GPR(gpr + 3), C_00000000);
	/* 0e: */ OP(icode, &ptr, iANDXOR, GPR(8), GPR(tmp + 0), GPR(gpr + 1), GPR(gpr + 2));
	/* 0f: */ OP(icode, &ptr, iSKIP, C_00000000, GPR_COND, CC_REG_MINUS, C_00000001);
	/* 10: */ OP(icode, &ptr, iANDXOR, GPR(8), GPR(8), GPR(gpr + 1), GPR(gpr + 2));

	/* 11: */ OP(icode, &ptr, iANDXOR, GPR(tmp + 0), ETRAM_DATA(ipcm->etram[1]), GPR(gpr + 0), C_00000000);
	/* 12: */ OP(icode, &ptr, iLOG, GPR(tmp + 0), GPR(tmp + 0), GPR(gpr + 3), C_00000000);
	/* 13: */ OP(icode, &ptr, iANDXOR, GPR(9), GPR(tmp + 0), GPR(gpr + 1), GPR(gpr + 2));
	/* 14: */ OP(icode, &ptr, iSKIP, C_00000000, GPR_COND, CC_REG_MINUS, C_00000001);
	/* 15: */ OP(icode, &ptr, iANDXOR, GPR(9), GPR(9), GPR(gpr + 1), GPR(gpr + 2));

	/* 16: */ OP(icode, &ptr, iACC3, GPR(tmp + 0), GPR(ipcm->gpr_ptr), C_00000001, C_00000000);
	/* 17: */ OP(icode, &ptr, iMACINT0, C_00000000, GPR(tmp + 0), C_ffffffff, GPR(ipcm->gpr_size));
	/* 18: */ OP(icode, &ptr, iSKIP, GPR_COND, GPR_COND, CC_REG_MINUS, C_00000001);
	/* 19: */ OP(icode, &ptr, iACC3, GPR(tmp + 0), C_00000000, C_00000000, C_00000000);
	/* 1a: */ OP(icode, &ptr, iACC3, GPR(ipcm->gpr_ptr), GPR(tmp + 0), C_00000000, C_00000000);
	
	/* 1b: */ OP(icode, &ptr, iACC3, GPR(ipcm->gpr_tmpcount), GPR(ipcm->gpr_tmpcount), C_ffffffff, C_00000000);
	/* 1c: */ OP(icode, &ptr, iSKIP, GPR_COND, GPR_COND, CC_REG_NONZERO, C_00000002);
	/* 1d: */ OP(icode, &ptr, iACC3, GPR(ipcm->gpr_tmpcount), GPR(ipcm->gpr_count), C_00000000, C_00000000);
	/* 1e: */ OP(icode, &ptr, iACC3, GPR_IRQ, C_80000000, C_00000000, C_00000000);
	/* 1f: */ OP(icode, &ptr, iANDXOR, GPR(ipcm->gpr_running), GPR(ipcm->gpr_running), C_00000001, C_00010000);

	/* 20: */ OP(icode, &ptr, iANDXOR, GPR(ipcm->gpr_running), GPR(ipcm->gpr_running), C_00010000, C_00000001);
	/* 21: */ OP(icode, &ptr, iSKIP, C_00000000, C_7fffffff, C_7fffffff, C_00000002);

	/* 22: */ OP(icode, &ptr, iMACINT1, ETRAM_ADDR(ipcm->etram[0]), GPR(gpr + 8), GPR_DBAC, C_ffffffff);
	/* 23: */ OP(icode, &ptr, iMACINT1, ETRAM_ADDR(ipcm->etram[1]), GPR(gpr + 9), GPR_DBAC, C_ffffffff);

	/* 24: */
	gpr += 13;

	/* Wave Playback Volume */
	for (z = 0; z < 2; z++)
		VOLUME(icode, &ptr, playback + z, z, gpr + z);
	snd_emu10k1_init_stereo_control(controls + i++, "Wave Playback Volume", gpr, 100);
	gpr += 2;

	/* Wave Surround Playback Volume */
	for (z = 0; z < 2; z++)
		VOLUME(icode, &ptr, playback + 2 + z, z, gpr + z);
	snd_emu10k1_init_stereo_control(controls + i++, "Wave Surround Playback Volume", gpr, 0);
	gpr += 2;
	
	/* Wave Center/LFE Playback Volume */
	OP(icode, &ptr, iACC3, GPR(tmp + 0), FXBUS(FXBUS_PCM_LEFT), FXBUS(FXBUS_PCM_RIGHT), C_00000000);
	OP(icode, &ptr, iMACINT0, GPR(tmp + 0), C_00000000, GPR(tmp + 0), C_00000002);
	VOLUME(icode, &ptr, playback + 4, tmp + 0, gpr);
	snd_emu10k1_init_mono_control(controls + i++, "Wave Center Playback Volume", gpr++, 0);
	VOLUME(icode, &ptr, playback + 5, tmp + 0, gpr);
	snd_emu10k1_init_mono_control(controls + i++, "Wave LFE Playback Volume", gpr++, 0);

	/* Wave Capture Volume + Switch */
	for (z = 0; z < 2; z++) {
		SWITCH(icode, &ptr, tmp + 0, z, gpr + 2 + z);
		VOLUME(icode, &ptr, capture + z, tmp + 0, gpr + z);
	}
	snd_emu10k1_init_stereo_control(controls + i++, "Wave Capture Volume", gpr, 0);
	snd_emu10k1_init_stereo_onoff_control(controls + i++, "Wave Capture Switch", gpr + 2, 0);
	gpr += 4;

	/* Music Playback Volume */
	for (z = 0; z < 2; z++)
		VOLUME_ADD(icode, &ptr, playback + z, 2 + z, gpr + z);
	snd_emu10k1_init_stereo_control(controls + i++, "Music Playback Volume", gpr, 100);
	gpr += 2;

	/* Music Capture Volume + Switch */
	for (z = 0; z < 2; z++) {
		SWITCH(icode, &ptr, tmp + 0, 2 + z, gpr + 2 + z);
		VOLUME_ADD(icode, &ptr, capture + z, tmp + 0, gpr + z);
	}
	snd_emu10k1_init_stereo_control(controls + i++, "Music Capture Volume", gpr, 0);
	snd_emu10k1_init_stereo_onoff_control(controls + i++, "Music Capture Switch", gpr + 2, 0);
	gpr += 4;

	/* Surround Digital Playback Volume (renamed later without Digital) */
	for (z = 0; z < 2; z++)
		VOLUME_ADD(icode, &ptr, playback + 2 + z, 4 + z, gpr + z);
	snd_emu10k1_init_stereo_control(controls + i++, "Surround Digital Playback Volume", gpr, 100);
	gpr += 2;

	/* Surround Capture Volume + Switch */
	for (z = 0; z < 2; z++) {
		SWITCH(icode, &ptr, tmp + 0, 4 + z, gpr + 2 + z);
		VOLUME_ADD(icode, &ptr, capture + z, tmp + 0, gpr + z);
	}
	snd_emu10k1_init_stereo_control(controls + i++, "Surround Capture Volume", gpr, 0);
	snd_emu10k1_init_stereo_onoff_control(controls + i++, "Surround Capture Switch", gpr + 2, 0);
	gpr += 4;

	/* Center Playback Volume (renamed later without Digital) */
	VOLUME_ADD(icode, &ptr, playback + 4, 6, gpr);
	snd_emu10k1_init_mono_control(controls + i++, "Center Digital Playback Volume", gpr++, 100);

	/* LFE Playback Volume + Switch (renamed later without Digital) */
	VOLUME_ADD(icode, &ptr, playback + 5, 7, gpr);
	snd_emu10k1_init_mono_control(controls + i++, "LFE Digital Playback Volume", gpr++, 100);

	/*
	 *  Process inputs
	 */

	if (emu->fx8010.extin_mask & ((1<<EXTIN_AC97_L)|(1<<EXTIN_AC97_R))) {
		/* AC'97 Playback Volume */
		VOLUME_ADDIN(icode, &ptr, playback + 0, EXTIN_AC97_L, gpr); gpr++;
		VOLUME_ADDIN(icode, &ptr, playback + 1, EXTIN_AC97_R, gpr); gpr++;
		snd_emu10k1_init_stereo_control(controls + i++, "AC97 Playback Volume", gpr-2, 0);
		/* AC'97 Capture Volume */
		VOLUME_ADDIN(icode, &ptr, capture + 0, EXTIN_AC97_L, gpr); gpr++;
		VOLUME_ADDIN(icode, &ptr, capture + 1, EXTIN_AC97_R, gpr); gpr++;
		snd_emu10k1_init_stereo_control(controls + i++, "AC97 Capture Volume", gpr-2, 100);
	}
	
	if (emu->fx8010.extin_mask & ((1<<EXTIN_SPDIF_CD_L)|(1<<EXTIN_SPDIF_CD_R))) {
		/* IEC958 TTL Playback Volume */
		for (z = 0; z < 2; z++)
			VOLUME_ADDIN(icode, &ptr, playback + z, EXTIN_SPDIF_CD_L + z, gpr + z);
		snd_emu10k1_init_stereo_control(controls + i++, "IEC958 TTL Playback Volume", gpr, 0);
		gpr += 2;
	
		/* IEC958 TTL Capture Volume + Switch */
		for (z = 0; z < 2; z++) {
			SWITCH_IN(icode, &ptr, tmp + 0, EXTIN_SPDIF_CD_L + z, gpr + 2 + z);
			VOLUME_ADD(icode, &ptr, capture + z, tmp + 0, gpr + z);
		}
		snd_emu10k1_init_stereo_control(controls + i++, "IEC958 TTL Capture Volume", gpr, 0);
		snd_emu10k1_init_stereo_onoff_control(controls + i++, "IEC958 TTL Capture Switch", gpr + 2, 0);
		gpr += 4;
	}
	
	if (emu->fx8010.extin_mask & ((1<<EXTIN_ZOOM_L)|(1<<EXTIN_ZOOM_R))) {
		/* Zoom Video Playback Volume */
		for (z = 0; z < 2; z++)
			VOLUME_ADDIN(icode, &ptr, playback + z, EXTIN_ZOOM_L + z, gpr + z);
		snd_emu10k1_init_stereo_control(controls + i++, "Zoom Video Playback Volume", gpr, 0);
		gpr += 2;
	
		/* Zoom Video Capture Volume + Switch */
		for (z = 0; z < 2; z++) {
			SWITCH_IN(icode, &ptr, tmp + 0, EXTIN_ZOOM_L + z, gpr + 2 + z);
			VOLUME_ADD(icode, &ptr, capture + z, tmp + 0, gpr + z);
		}
		snd_emu10k1_init_stereo_control(controls + i++, "Zoom Video Capture Volume", gpr, 0);
		snd_emu10k1_init_stereo_onoff_control(controls + i++, "Zoom Video Capture Switch", gpr + 2, 0);
		gpr += 4;
	}
	
	if (emu->fx8010.extin_mask & ((1<<EXTIN_TOSLINK_L)|(1<<EXTIN_TOSLINK_R))) {
		/* IEC958 Optical Playback Volume */
		for (z = 0; z < 2; z++)
			VOLUME_ADDIN(icode, &ptr, playback + z, EXTIN_TOSLINK_L + z, gpr + z);
		snd_emu10k1_init_stereo_control(controls + i++, "IEC958 LiveDrive Playback Volume", gpr, 0);
		gpr += 2;
	
		/* IEC958 Optical Capture Volume */
		for (z = 0; z < 2; z++) {
			SWITCH_IN(icode, &ptr, tmp + 0, EXTIN_TOSLINK_L + z, gpr + 2 + z);
			VOLUME_ADD(icode, &ptr, capture + z, tmp + 0, gpr + z);
		}
		snd_emu10k1_init_stereo_control(controls + i++, "IEC958 LiveDrive Capture Volume", gpr, 0);
		snd_emu10k1_init_stereo_onoff_control(controls + i++, "IEC958 LiveDrive Capture Switch", gpr + 2, 0);
		gpr += 4;
	}
	
	if (emu->fx8010.extin_mask & ((1<<EXTIN_LINE1_L)|(1<<EXTIN_LINE1_R))) {
		/* Line LiveDrive Playback Volume */
		for (z = 0; z < 2; z++)
			VOLUME_ADDIN(icode, &ptr, playback + z, EXTIN_LINE1_L + z, gpr + z);
		snd_emu10k1_init_stereo_control(controls + i++, "Line LiveDrive Playback Volume", gpr, 0);
		gpr += 2;
	
		/* Line LiveDrive Capture Volume + Switch */
		for (z = 0; z < 2; z++) {
			SWITCH_IN(icode, &ptr, tmp + 0, EXTIN_LINE1_L + z, gpr + 2 + z);
			VOLUME_ADD(icode, &ptr, capture + z, tmp + 0, gpr + z);
		}
		snd_emu10k1_init_stereo_control(controls + i++, "Line LiveDrive Capture Volume", gpr, 0);
		snd_emu10k1_init_stereo_onoff_control(controls + i++, "Line LiveDrive Capture Switch", gpr + 2, 0);
		gpr += 4;
	}
	
	if (emu->fx8010.extin_mask & ((1<<EXTIN_COAX_SPDIF_L)|(1<<EXTIN_COAX_SPDIF_R))) {
		/* IEC958 Coax Playback Volume */
		for (z = 0; z < 2; z++)
			VOLUME_ADDIN(icode, &ptr, playback + z, EXTIN_COAX_SPDIF_L + z, gpr + z);
		snd_emu10k1_init_stereo_control(controls + i++, "IEC958 Coaxial Playback Volume", gpr, 0);
		gpr += 2;
	
		/* IEC958 Coax Capture Volume + Switch */
		for (z = 0; z < 2; z++) {
			SWITCH_IN(icode, &ptr, tmp + 0, EXTIN_COAX_SPDIF_L + z, gpr + 2 + z);
			VOLUME_ADD(icode, &ptr, capture + z, tmp + 0, gpr + z);
		}
		snd_emu10k1_init_stereo_control(controls + i++, "IEC958 Coaxial Capture Volume", gpr, 0);
		snd_emu10k1_init_stereo_onoff_control(controls + i++, "IEC958 Coaxial Capture Switch", gpr + 2, 0);
		gpr += 4;
	}
	
	if (emu->fx8010.extin_mask & ((1<<EXTIN_LINE2_L)|(1<<EXTIN_LINE2_R))) {
		/* Line LiveDrive Playback Volume */
		for (z = 0; z < 2; z++)
			VOLUME_ADDIN(icode, &ptr, playback + z, EXTIN_LINE2_L + z, gpr + z);
		snd_emu10k1_init_stereo_control(controls + i++, "Line2 LiveDrive Playback Volume", gpr, 0);
		controls[i-1].id.index = 1;
		gpr += 2;
	
		/* Line LiveDrive Capture Volume */
		for (z = 0; z < 2; z++) {
			SWITCH_IN(icode, &ptr, tmp + 0, EXTIN_LINE2_L + z, gpr + 2 + z);
			VOLUME_ADD(icode, &ptr, capture + z, tmp + 0, gpr + z);
		}
		snd_emu10k1_init_stereo_control(controls + i++, "Line2 LiveDrive Capture Volume", gpr, 0);
		controls[i-1].id.index = 1;
		snd_emu10k1_init_stereo_onoff_control(controls + i++, "Line2 LiveDrive Capture Switch", gpr + 2, 0);
		controls[i-1].id.index = 1;
		gpr += 4;
	}

	/*
	 *  Process tone control
	 */
	OP(icode, &ptr, iACC3, GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 0), GPR(playback + 0), C_00000000, C_00000000); /* left */
	OP(icode, &ptr, iACC3, GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 1), GPR(playback + 1), C_00000000, C_00000000); /* right */
	OP(icode, &ptr, iACC3, GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 2), GPR(playback + 2), C_00000000, C_00000000); /* rear left */
	OP(icode, &ptr, iACC3, GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 3), GPR(playback + 3), C_00000000, C_00000000); /* rear right */
	OP(icode, &ptr, iACC3, GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 4), GPR(playback + 4), C_00000000, C_00000000); /* center */
	OP(icode, &ptr, iACC3, GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 5), GPR(playback + 5), C_00000000, C_00000000); /* LFE */

	ctl = &controls[i + 0];
	ctl->id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	strcpy(ctl->id.name, "Tone Control - Bass");
	ctl->vcount = 2;
	ctl->count = 10;
	ctl->min = 0;
	ctl->max = 40;
	ctl->value[0] = ctl->value[1] = 20;
	ctl->translation = EMU10K1_GPR_TRANSLATION_BASS;
	ctl = &controls[i + 1];
	ctl->id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	strcpy(ctl->id.name, "Tone Control - Treble");
	ctl->vcount = 2;
	ctl->count = 10;
	ctl->min = 0;
	ctl->max = 40;
	ctl->value[0] = ctl->value[1] = 20;
	ctl->translation = EMU10K1_GPR_TRANSLATION_TREBLE;

#define BASS_GPR	0x8c
#define TREBLE_GPR	0x96

	for (z = 0; z < 5; z++) {
		int j;
		for (j = 0; j < 2; j++) {
			controls[i + 0].gpr[z * 2 + j] = BASS_GPR + z * 2 + j;
			controls[i + 1].gpr[z * 2 + j] = TREBLE_GPR + z * 2 + j;
		}
	}
	for (z = 0; z < 3; z++) {		/* front/rear/center-lfe */
		int j, k, l, d;
		for (j = 0; j < 2; j++) {	/* left/right */
			k = 0xa0 + (z * 8) + (j * 4);
			l = 0xd0 + (z * 8) + (j * 4);
			d = playback + SND_EMU10K1_PLAYBACK_CHANNELS + z * 2 + j;

			OP(icode, &ptr, iMAC0, C_00000000, C_00000000, GPR(d), GPR(BASS_GPR + 0 + j));
			OP(icode, &ptr, iMACMV, GPR(k+1), GPR(k), GPR(k+1), GPR(BASS_GPR + 4 + j));
			OP(icode, &ptr, iMACMV, GPR(k), GPR(d), GPR(k), GPR(BASS_GPR + 2 + j));
			OP(icode, &ptr, iMACMV, GPR(k+3), GPR(k+2), GPR(k+3), GPR(BASS_GPR + 8 + j));
			OP(icode, &ptr, iMAC0, GPR(k+2), GPR_ACCU, GPR(k+2), GPR(BASS_GPR + 6 + j));
			OP(icode, &ptr, iACC3, GPR(k+2), GPR(k+2), GPR(k+2), C_00000000);

			OP(icode, &ptr, iMAC0, C_00000000, C_00000000, GPR(k+2), GPR(TREBLE_GPR + 0 + j));
			OP(icode, &ptr, iMACMV, GPR(l+1), GPR(l), GPR(l+1), GPR(TREBLE_GPR + 4 + j));
			OP(icode, &ptr, iMACMV, GPR(l), GPR(k+2), GPR(l), GPR(TREBLE_GPR + 2 + j));
			OP(icode, &ptr, iMACMV, GPR(l+3), GPR(l+2), GPR(l+3), GPR(TREBLE_GPR + 8 + j));
			OP(icode, &ptr, iMAC0, GPR(l+2), GPR_ACCU, GPR(l+2), GPR(TREBLE_GPR + 6 + j));
			OP(icode, &ptr, iMACINT0, GPR(l+2), C_00000000, GPR(l+2), C_00000010);

			OP(icode, &ptr, iACC3, GPR(d), GPR(l+2), C_00000000, C_00000000);

			if (z == 2)	/* center */
				break;
		}
	}
	i += 2;

#undef BASS_GPR
#undef TREBLE_GPR

	for (z = 0; z < 6; z++) {
		SWITCH(icode, &ptr, tmp + 0, playback + SND_EMU10K1_PLAYBACK_CHANNELS + z, gpr + 0);
		SWITCH_NEG(icode, &ptr, tmp + 1, gpr + 0);
		SWITCH(icode, &ptr, tmp + 1, playback + z, tmp + 1);
		OP(icode, &ptr, iACC3, GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + z), GPR(tmp + 0), GPR(tmp + 1), C_00000000);
	}
	snd_emu10k1_init_stereo_onoff_control(controls + i++, "Tone Control - Switch", gpr, 0);
	gpr += 2;

	/*
	 *  Process outputs
	 */
	if (emu->fx8010.extout_mask & ((1<<EXTOUT_AC97_L)|(1<<EXTOUT_AC97_R))) {
		/* AC'97 Playback Volume */

		for (z = 0; z < 2; z++)
			OP(icode, &ptr, iACC3, EXTOUT(EXTOUT_AC97_L + z), GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + z), C_00000000, C_00000000);
	}

	if (emu->fx8010.extout_mask & ((1<<EXTOUT_TOSLINK_L)|(1<<EXTOUT_TOSLINK_R))) {
		/* IEC958 Optical Raw Playback Switch */

		for (z = 0; z < 2; z++) {
			SWITCH(icode, &ptr, tmp + 0, 8 + z, gpr + z);
			SWITCH_NEG(icode, &ptr, tmp + 1, gpr + z);
			SWITCH(icode, &ptr, tmp + 1, playback + SND_EMU10K1_PLAYBACK_CHANNELS + z, tmp + 1);
			OP(icode, &ptr, iACC3, EXTOUT(EXTOUT_TOSLINK_L + z), GPR(tmp + 0), GPR(tmp + 1), C_00000000);
#ifdef EMU10K1_CAPTURE_DIGITAL_OUT
	 		OP(icode, &ptr, iACC3, EXTOUT(EXTOUT_ADC_CAP_L + z), GPR(tmp + 0), GPR(tmp + 1), C_00000000);
#endif
		}

		snd_emu10k1_init_stereo_onoff_control(controls + i++, "IEC958 Optical Raw Playback Switch", gpr, 0);
		gpr += 2;
	}

	if (emu->fx8010.extout_mask & ((1<<EXTOUT_HEADPHONE_L)|(1<<EXTOUT_HEADPHONE_R))) {
		/* Headphone Playback Volume */

		for (z = 0; z < 2; z++) {
			SWITCH(icode, &ptr, tmp + 0, playback + SND_EMU10K1_PLAYBACK_CHANNELS + 4 + z, gpr + 2 + z);
			SWITCH_NEG(icode, &ptr, tmp + 1, gpr + 2 + z);
			SWITCH(icode, &ptr, tmp + 1, playback + SND_EMU10K1_PLAYBACK_CHANNELS + z, tmp + 1);
			OP(icode, &ptr, iACC3, GPR(tmp + 0), GPR(tmp + 0), GPR(tmp + 1), C_00000000);
			VOLUME_OUT(icode, &ptr, EXTOUT_HEADPHONE_L + z, tmp + 0, gpr + z);
		}

		snd_emu10k1_init_stereo_control(controls + i++, "Headphone Playback Volume", gpr + 0, 0);
		controls[i-1].id.index = 1;	/* AC'97 can have also Headphone control */
		snd_emu10k1_init_mono_onoff_control(controls + i++, "Headphone Center Playback Switch", gpr + 2, 0);
		controls[i-1].id.index = 1;
		snd_emu10k1_init_mono_onoff_control(controls + i++, "Headphone LFE Playback Switch", gpr + 3, 0);
		controls[i-1].id.index = 1;

		gpr += 4;
	}
	
	if (emu->fx8010.extout_mask & ((1<<EXTOUT_REAR_L)|(1<<EXTOUT_REAR_R)))
		for (z = 0; z < 2; z++)
			OP(icode, &ptr, iACC3, EXTOUT(EXTOUT_REAR_L + z), GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 2 + z), C_00000000, C_00000000);

	if (emu->fx8010.extout_mask & ((1<<EXTOUT_AC97_REAR_L)|(1<<EXTOUT_AC97_REAR_R)))
		for (z = 0; z < 2; z++)
			OP(icode, &ptr, iACC3, EXTOUT(EXTOUT_AC97_REAR_L + z), GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 2 + z), C_00000000, C_00000000);

	if (emu->fx8010.extout_mask & (1<<EXTOUT_AC97_CENTER)) {
#ifndef EMU10K1_CENTER_LFE_FROM_FRONT
		OP(icode, &ptr, iACC3, EXTOUT(EXTOUT_AC97_CENTER), GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 4), C_00000000, C_00000000);
		OP(icode, &ptr, iACC3, EXTOUT(EXTOUT_ACENTER), GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 4), C_00000000, C_00000000);
#else
		OP(icode, &ptr, iACC3, EXTOUT(EXTOUT_AC97_CENTER), GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 0), C_00000000, C_00000000);
		OP(icode, &ptr, iACC3, EXTOUT(EXTOUT_ACENTER), GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 0), C_00000000, C_00000000);
#endif
	}

	if (emu->fx8010.extout_mask & (1<<EXTOUT_AC97_LFE)) {
#ifndef EMU10K1_CENTER_LFE_FROM_FRONT
		OP(icode, &ptr, iACC3, EXTOUT(EXTOUT_AC97_LFE), GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 5), C_00000000, C_00000000);
		OP(icode, &ptr, iACC3, EXTOUT(EXTOUT_ALFE), GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 5), C_00000000, C_00000000);
#else
		OP(icode, &ptr, iACC3, EXTOUT(EXTOUT_AC97_LFE), GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 1), C_00000000, C_00000000);
		OP(icode, &ptr, iACC3, EXTOUT(EXTOUT_ALFE), GPR(playback + SND_EMU10K1_PLAYBACK_CHANNELS + 1), C_00000000, C_00000000);
#endif
	}
	
#ifndef EMU10K1_CAPTURE_DIGITAL_OUT
	for (z = 0; z < 2; z++)
 		OP(icode, &ptr, iACC3, EXTOUT(EXTOUT_ADC_CAP_L + z), GPR(capture + z), C_00000000, C_00000000);
#endif
	
	if (emu->fx8010.extout_mask & (1<<EXTOUT_MIC_CAP))
		OP(icode, &ptr, iACC3, EXTOUT(EXTOUT_MIC_CAP), GPR(capture + 2), C_00000000, C_00000000);

	if (gpr > tmp) {
		snd_BUG();
		err = -EIO;
		goto __err;
	}
	if (i > SND_EMU10K1_GPR_CONTROLS) {
		snd_BUG();
		err = -EIO;
		goto __err;
	}
	
	/* clear remaining instruction memory */
	while (ptr < 0x200)
		OP(icode, &ptr, iACC3, C_00000000, C_00000000, C_00000000, C_00000000);

	if ((err = snd_emu10k1_fx8010_tram_setup(emu, ipcm->buffer_size)) < 0)
		goto __err;
	seg = snd_enter_user();
	icode->gpr_add_control_count = i;
	icode->gpr_add_controls = controls;
	err = snd_emu10k1_icode_poke(emu, icode);
	snd_leave_user(seg);
	if (err >= 0)
		err = snd_emu10k1_ipcm_poke(emu, ipcm);
      __err:
	kfree(ipcm);
	kfree(controls);
	kfree(icode);
	return err;
}

int __devinit snd_emu10k1_init_efx(emu10k1_t *emu)
{
	if (emu->audigy)
		return _snd_emu10k1_audigy_init_efx(emu);
	else
		return _snd_emu10k1_init_efx(emu);
}

void snd_emu10k1_free_efx(emu10k1_t *emu)
{
	/* stop processor */
	if (emu->audigy)
		snd_emu10k1_ptr_write(emu, A_DBG, 0, emu->fx8010.dbg = A_DBG_SINGLE_STEP);
	else
		snd_emu10k1_ptr_write(emu, DBG, 0, emu->fx8010.dbg = EMU10K1_DBG_SINGLE_STEP);
}

#if 0 // FIXME: who use them?
int snd_emu10k1_fx8010_tone_control_activate(emu10k1_t *emu, int output)
{
	snd_runtime_check(output >= 0 && output < 6, return -EINVAL);
	snd_emu10k1_ptr_write(emu, emu->gpr_base + 0x94 + output, 0, 1);
	return 0;
}

int snd_emu10k1_fx8010_tone_control_deactivate(emu10k1_t *emu, int output)
{
	snd_runtime_check(output >= 0 && output < 6, return -EINVAL);
	snd_emu10k1_ptr_write(emu, emu->gpr_base + 0x94 + output, 0, 0);
	return 0;
}
#endif

int snd_emu10k1_fx8010_tram_setup(emu10k1_t *emu, u32 size)
{
	u8 size_reg = 0;

	/* size is in samples */
	if (size != 0) {
		size = (size - 1) >> 13;

		while (size) {
			size >>= 1;
			size_reg++;
		}
		size = 0x2000 << size_reg;
	}
	if (emu->fx8010.etram_pages.bytes == size)
		return 0;
	spin_lock_irq(&emu->emu_lock);
	outl(HCFG_LOCKTANKCACHE_MASK | inl(emu->port + HCFG), emu->port + HCFG);
	spin_unlock_irq(&emu->emu_lock);
	snd_emu10k1_ptr_write(emu, TCB, 0, 0);
	snd_emu10k1_ptr_write(emu, TCBS, 0, 0);
	if (emu->fx8010.etram_pages.area != NULL) {
		snd_dma_free_pages(&emu->dma_dev, &emu->fx8010.etram_pages);
		emu->fx8010.etram_pages.area = NULL;
		emu->fx8010.etram_pages.bytes = 0;
	}

	if (size > 0) {
		if (snd_dma_alloc_pages(&emu->dma_dev, size * 2, &emu->fx8010.etram_pages) < 0)
			return -ENOMEM;
		memset(emu->fx8010.etram_pages.area, 0, size * 2);
		snd_emu10k1_ptr_write(emu, TCB, 0, emu->fx8010.etram_pages.addr);
		snd_emu10k1_ptr_write(emu, TCBS, 0, size_reg);
		spin_lock_irq(&emu->emu_lock);
		outl(inl(emu->port + HCFG) & ~HCFG_LOCKTANKCACHE_MASK, emu->port + HCFG);
		spin_unlock_irq(&emu->emu_lock);	
	}

	return 0;
}

static int snd_emu10k1_fx8010_open(snd_hwdep_t * hw, struct file *file)
{
	return 0;
}

static void copy_string(char *dst, char *src, char *null, int idx)
{
	if (src == NULL)
		sprintf(dst, "%s %02X", null, idx);
	else
		strcpy(dst, src);
}

static int snd_emu10k1_fx8010_info(emu10k1_t *emu, emu10k1_fx8010_info_t *info)
{
	char **fxbus, **extin, **extout;
	unsigned short fxbus_mask, extin_mask, extout_mask;
	int res;

	memset(info, 0, sizeof(info));
	info->card = emu->card_type;
	info->internal_tram_size = emu->fx8010.itram_size;
	info->external_tram_size = emu->fx8010.etram_pages.bytes;
	fxbus = fxbuses;
	extin = emu->audigy ? audigy_ins : creative_ins;
	extout = emu->audigy ? audigy_outs : creative_outs;
	fxbus_mask = emu->fx8010.fxbus_mask;
	extin_mask = emu->fx8010.extin_mask;
	extout_mask = emu->fx8010.extout_mask;
	for (res = 0; res < 16; res++, fxbus++, extin++, extout++) {
		copy_string(info->fxbus_names[res], fxbus_mask & (1 << res) ? *fxbus : NULL, "FXBUS", res);
		copy_string(info->extin_names[res], extin_mask & (1 << res) ? *extin : NULL, "Unused", res);
		copy_string(info->extout_names[res], extout_mask & (1 << res) ? *extout : NULL, "Unused", res);
	}
	for (res = 16; res < 32; res++, extout++)
		copy_string(info->extout_names[res], extout_mask & (1 << res) ? *extout : NULL, "Unused", res);
	info->gpr_controls = emu->fx8010.gpr_count;
	return 0;
}

static int snd_emu10k1_fx8010_ioctl(snd_hwdep_t * hw, struct file *file, unsigned int cmd, unsigned long arg)
{
	emu10k1_t *emu = snd_magic_cast(emu10k1_t, hw->private_data, return -ENXIO);
	emu10k1_fx8010_info_t *info;
	emu10k1_fx8010_code_t *icode;
	emu10k1_fx8010_pcm_t *ipcm;
	unsigned int addr;
	void __user *argp = (void __user *)arg;
	int res;
	
	switch (cmd) {
	case SNDRV_EMU10K1_IOCTL_INFO:
		info = (emu10k1_fx8010_info_t *)kmalloc(sizeof(*info), GFP_KERNEL);
		if (!info)
			return -ENOMEM;
		if ((res = snd_emu10k1_fx8010_info(emu, info)) < 0) {
			kfree(info);
			return res;
		}
		if (copy_to_user(argp, info, sizeof(*info))) {
			kfree(info);
			return -EFAULT;
		}
		kfree(info);
		return 0;
	case SNDRV_EMU10K1_IOCTL_CODE_POKE:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		icode = (emu10k1_fx8010_code_t *)kmalloc(sizeof(*icode), GFP_KERNEL);
		if (icode == NULL)
			return -ENOMEM;
		if (copy_from_user(icode, argp, sizeof(*icode))) {
			kfree(icode);
			return -EFAULT;
		}
		res = snd_emu10k1_icode_poke(emu, icode);
		kfree(icode);
		return res;
	case SNDRV_EMU10K1_IOCTL_CODE_PEEK:
		icode = (emu10k1_fx8010_code_t *)kmalloc(sizeof(*icode), GFP_KERNEL);
		if (icode == NULL)
			return -ENOMEM;
		if (copy_from_user(icode, argp, sizeof(*icode))) {
			kfree(icode);
			return -EFAULT;
		}
		res = snd_emu10k1_icode_peek(emu, icode);
		if (res == 0 && copy_to_user(argp, icode, sizeof(*icode))) {
			kfree(icode);
			return -EFAULT;
		}
		kfree(icode);
		return res;
	case SNDRV_EMU10K1_IOCTL_PCM_POKE:
		if (emu->audigy)
			return -EINVAL;
		ipcm = (emu10k1_fx8010_pcm_t *)kmalloc(sizeof(*ipcm), GFP_KERNEL);
		if (ipcm == NULL)
			return -ENOMEM;
		if (copy_from_user(ipcm, argp, sizeof(*ipcm))) {
			kfree(ipcm);
			return -EFAULT;
		}
		res = snd_emu10k1_ipcm_poke(emu, ipcm);
		kfree(ipcm);
		return res;
	case SNDRV_EMU10K1_IOCTL_PCM_PEEK:
		if (emu->audigy)
			return -EINVAL;
		ipcm = (emu10k1_fx8010_pcm_t *)snd_kcalloc(sizeof(*ipcm), GFP_KERNEL);
		if (ipcm == NULL)
			return -ENOMEM;
		if (copy_from_user(ipcm, argp, sizeof(*ipcm))) {
			kfree(ipcm);
			return -EFAULT;
		}
		res = snd_emu10k1_ipcm_peek(emu, ipcm);
		if (res == 0 && copy_to_user(argp, ipcm, sizeof(*ipcm))) {
			kfree(ipcm);
			return -EFAULT;
		}
		kfree(ipcm);
		return res;
	case SNDRV_EMU10K1_IOCTL_TRAM_SETUP:
		if (emu->audigy)
			return -EINVAL;
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (get_user(addr, (unsigned int __user *)argp))
			return -EFAULT;
		down(&emu->fx8010.lock);
		res = snd_emu10k1_fx8010_tram_setup(emu, addr);
		up(&emu->fx8010.lock);
		return res;
	case SNDRV_EMU10K1_IOCTL_STOP:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (emu->audigy)
			snd_emu10k1_ptr_write(emu, A_DBG, 0, emu->fx8010.dbg |= A_DBG_SINGLE_STEP);
		else
			snd_emu10k1_ptr_write(emu, DBG, 0, emu->fx8010.dbg |= EMU10K1_DBG_SINGLE_STEP);
		return 0;
	case SNDRV_EMU10K1_IOCTL_CONTINUE:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (emu->audigy)
			snd_emu10k1_ptr_write(emu, A_DBG, 0, emu->fx8010.dbg = 0);
		else
			snd_emu10k1_ptr_write(emu, DBG, 0, emu->fx8010.dbg = 0);
		return 0;
	case SNDRV_EMU10K1_IOCTL_ZERO_TRAM_COUNTER:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (emu->audigy)
			snd_emu10k1_ptr_write(emu, A_DBG, 0, emu->fx8010.dbg | A_DBG_ZC);
		else
			snd_emu10k1_ptr_write(emu, DBG, 0, emu->fx8010.dbg | EMU10K1_DBG_ZC);
		udelay(10);
		if (emu->audigy)
			snd_emu10k1_ptr_write(emu, A_DBG, 0, emu->fx8010.dbg);
		else
			snd_emu10k1_ptr_write(emu, DBG, 0, emu->fx8010.dbg);
		return 0;
	case SNDRV_EMU10K1_IOCTL_SINGLE_STEP:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (get_user(addr, (unsigned int __user *)argp))
			return -EFAULT;
		if (addr > 0x1ff)
			return -EINVAL;
		if (emu->audigy)
			snd_emu10k1_ptr_write(emu, A_DBG, 0, emu->fx8010.dbg |= A_DBG_SINGLE_STEP | addr);
		else
			snd_emu10k1_ptr_write(emu, DBG, 0, emu->fx8010.dbg |= EMU10K1_DBG_SINGLE_STEP | addr);
		udelay(10);
		if (emu->audigy)
			snd_emu10k1_ptr_write(emu, A_DBG, 0, emu->fx8010.dbg |= A_DBG_SINGLE_STEP | A_DBG_STEP_ADDR | addr);
		else
			snd_emu10k1_ptr_write(emu, DBG, 0, emu->fx8010.dbg |= EMU10K1_DBG_SINGLE_STEP | EMU10K1_DBG_STEP | addr);
		return 0;
	case SNDRV_EMU10K1_IOCTL_DBG_READ:
		if (emu->audigy)
			addr = snd_emu10k1_ptr_read(emu, A_DBG, 0);
		else
			addr = snd_emu10k1_ptr_read(emu, DBG, 0);
		if (put_user(addr, (unsigned int __user *)argp))
			return -EFAULT;
		return 0;
	}
	return -ENOTTY;
}

static int snd_emu10k1_fx8010_release(snd_hwdep_t * hw, struct file *file)
{
	return 0;
}

int __devinit snd_emu10k1_fx8010_new(emu10k1_t *emu, int device, snd_hwdep_t ** rhwdep)
{
	snd_hwdep_t *hw;
	int err;
	
	if (rhwdep)
		*rhwdep = NULL;
	if ((err = snd_hwdep_new(emu->card, "FX8010", device, &hw)) < 0)
		return err;
	strcpy(hw->name, "EMU10K1 (FX8010)");
	hw->iface = SNDRV_HWDEP_IFACE_EMU10K1;
	hw->ops.open = snd_emu10k1_fx8010_open;
	hw->ops.ioctl = snd_emu10k1_fx8010_ioctl;
	hw->ops.release = snd_emu10k1_fx8010_release;
	hw->private_data = emu;
	if (rhwdep)
		*rhwdep = hw;
	return 0;
}
