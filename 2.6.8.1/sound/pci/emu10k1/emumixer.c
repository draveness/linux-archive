/*
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>,
 *                   Takashi Iwai <tiwai@suse.de>
 *                   Creative Labs, Inc.
 *  Routines for control of EMU10K1 chips / mixer routines
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
#include <linux/time.h>
#include <linux/init.h>
#include <sound/core.h>
#include <sound/emu10k1.h>

#define chip_t emu10k1_t

#define AC97_ID_STAC9758	0x83847658

static int snd_emu10k1_spdif_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_IEC958;
	uinfo->count = 1;
	return 0;
}

static int snd_emu10k1_spdif_get(snd_kcontrol_t * kcontrol,
                                 snd_ctl_elem_value_t * ucontrol)
{
	emu10k1_t *emu = snd_kcontrol_chip(kcontrol);
	unsigned int idx = snd_ctl_get_ioffidx(kcontrol, &ucontrol->id);
	unsigned long flags;

	spin_lock_irqsave(&emu->reg_lock, flags);
	ucontrol->value.iec958.status[0] = (emu->spdif_bits[idx] >> 0) & 0xff;
	ucontrol->value.iec958.status[1] = (emu->spdif_bits[idx] >> 8) & 0xff;
	ucontrol->value.iec958.status[2] = (emu->spdif_bits[idx] >> 16) & 0xff;
	ucontrol->value.iec958.status[3] = (emu->spdif_bits[idx] >> 24) & 0xff;
	spin_unlock_irqrestore(&emu->reg_lock, flags);
        return 0;
}

static int snd_emu10k1_spdif_get_mask(snd_kcontrol_t * kcontrol,
				      snd_ctl_elem_value_t * ucontrol)
{
	ucontrol->value.iec958.status[0] = 0xff;
	ucontrol->value.iec958.status[1] = 0xff;
	ucontrol->value.iec958.status[2] = 0xff;
	ucontrol->value.iec958.status[3] = 0xff;
        return 0;
}

static int snd_emu10k1_spdif_put(snd_kcontrol_t * kcontrol,
                                 snd_ctl_elem_value_t * ucontrol)
{
	emu10k1_t *emu = snd_kcontrol_chip(kcontrol);
	unsigned int idx = snd_ctl_get_ioffidx(kcontrol, &ucontrol->id);
	int change;
	unsigned int val;
	unsigned long flags;

	val = (ucontrol->value.iec958.status[0] << 0) |
	      (ucontrol->value.iec958.status[1] << 8) |
	      (ucontrol->value.iec958.status[2] << 16) |
	      (ucontrol->value.iec958.status[3] << 24);
	spin_lock_irqsave(&emu->reg_lock, flags);
	change = val != emu->spdif_bits[idx];
	if (change) {
		snd_emu10k1_ptr_write(emu, SPCS0 + idx, 0, val);
		emu->spdif_bits[idx] = val;
	}
	spin_unlock_irqrestore(&emu->reg_lock, flags);
        return change;
}

static snd_kcontrol_new_t snd_emu10k1_spdif_mask_control =
{
	.access =	SNDRV_CTL_ELEM_ACCESS_READ,
        .iface =        SNDRV_CTL_ELEM_IFACE_MIXER,
        .name =         SNDRV_CTL_NAME_IEC958("",PLAYBACK,MASK),
	.count =	4,
        .info =         snd_emu10k1_spdif_info,
        .get =          snd_emu10k1_spdif_get_mask
};

static snd_kcontrol_new_t snd_emu10k1_spdif_control =
{
        .iface =	SNDRV_CTL_ELEM_IFACE_MIXER,
        .name =         SNDRV_CTL_NAME_IEC958("",PLAYBACK,DEFAULT),
	.count =	4,
        .info =         snd_emu10k1_spdif_info,
        .get =          snd_emu10k1_spdif_get,
        .put =          snd_emu10k1_spdif_put
};


static void update_emu10k1_fxrt(emu10k1_t *emu, int voice, unsigned char *route)
{
	if (emu->audigy) {
		snd_emu10k1_ptr_write(emu, A_FXRT1, voice,
				      snd_emu10k1_compose_audigy_fxrt1(route));
		snd_emu10k1_ptr_write(emu, A_FXRT2, voice,
				      snd_emu10k1_compose_audigy_fxrt2(route));
	} else {
		snd_emu10k1_ptr_write(emu, FXRT, voice,
				      snd_emu10k1_compose_send_routing(route));
	}
}

static void update_emu10k1_send_volume(emu10k1_t *emu, int voice, unsigned char *volume)
{
	snd_emu10k1_ptr_write(emu, PTRX_FXSENDAMOUNT_A, voice, volume[0]);
	snd_emu10k1_ptr_write(emu, PTRX_FXSENDAMOUNT_B, voice, volume[1]);
	snd_emu10k1_ptr_write(emu, PSST_FXSENDAMOUNT_C, voice, volume[2]);
	snd_emu10k1_ptr_write(emu, DSL_FXSENDAMOUNT_D, voice, volume[3]);
	if (emu->audigy) {
		unsigned int val = ((unsigned int)volume[4] << 24) |
			((unsigned int)volume[5] << 16) |
			((unsigned int)volume[6] << 8) |
			(unsigned int)volume[7];
		snd_emu10k1_ptr_write(emu, A_SENDAMOUNTS, voice, val);
	}
}

static int snd_emu10k1_send_routing_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	emu10k1_t *emu = snd_kcontrol_chip(kcontrol);
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = emu->audigy ? 3*8 : 3*4;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = emu->audigy ? 0x3f : 0x0f;
	return 0;
}

static int snd_emu10k1_send_routing_get(snd_kcontrol_t * kcontrol,
                                        snd_ctl_elem_value_t * ucontrol)
{
	unsigned long flags;
	emu10k1_t *emu = snd_kcontrol_chip(kcontrol);
	emu10k1_pcm_mixer_t *mix = &emu->pcm_mixer[snd_ctl_get_ioffidx(kcontrol, &ucontrol->id)];
	int voice, idx;
	int num_efx = emu->audigy ? 8 : 4;
	int mask = emu->audigy ? 0x3f : 0x0f;

	spin_lock_irqsave(&emu->reg_lock, flags);
	for (voice = 0; voice < 3; voice++)
		for (idx = 0; idx < num_efx; idx++)
			ucontrol->value.integer.value[(voice * num_efx) + idx] = 
				mix->send_routing[voice][idx] & mask;
	spin_unlock_irqrestore(&emu->reg_lock, flags);
        return 0;
}

static int snd_emu10k1_send_routing_put(snd_kcontrol_t * kcontrol,
                                        snd_ctl_elem_value_t * ucontrol)
{
	unsigned long flags;
	emu10k1_t *emu = snd_kcontrol_chip(kcontrol);
	emu10k1_pcm_mixer_t *mix = &emu->pcm_mixer[snd_ctl_get_ioffidx(kcontrol, &ucontrol->id)];
	int change = 0, voice, idx, val;
	int num_efx = emu->audigy ? 8 : 4;
	int mask = emu->audigy ? 0x3f : 0x0f;

	spin_lock_irqsave(&emu->reg_lock, flags);
	for (voice = 0; voice < 3; voice++)
		for (idx = 0; idx < num_efx; idx++) {
			val = ucontrol->value.integer.value[(voice * num_efx) + idx] & mask;
			if (mix->send_routing[voice][idx] != val) {
				mix->send_routing[voice][idx] = val;
				change = 1;
			}
		}	
	if (change && mix->epcm) {
		if (mix->epcm->voices[0] && mix->epcm->voices[1]) {
			update_emu10k1_fxrt(emu, mix->epcm->voices[0]->number,
					    &mix->send_routing[1][0]);
			update_emu10k1_fxrt(emu, mix->epcm->voices[1]->number,
					    &mix->send_routing[2][0]);
		} else if (mix->epcm->voices[0]) {
			update_emu10k1_fxrt(emu, mix->epcm->voices[0]->number,
					    &mix->send_routing[0][0]);
		}
	}
	spin_unlock_irqrestore(&emu->reg_lock, flags);
        return change;
}

static snd_kcontrol_new_t snd_emu10k1_send_routing_control =
{
	.access =	SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_INACTIVE,
        .iface =        SNDRV_CTL_ELEM_IFACE_MIXER,
        .name =         "EMU10K1 PCM Send Routing",
	.count =	32,
        .info =         snd_emu10k1_send_routing_info,
        .get =          snd_emu10k1_send_routing_get,
        .put =          snd_emu10k1_send_routing_put
};

static int snd_emu10k1_send_volume_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	emu10k1_t *emu = snd_kcontrol_chip(kcontrol);
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = emu->audigy ? 3*8 : 3*4;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 255;
	return 0;
}

static int snd_emu10k1_send_volume_get(snd_kcontrol_t * kcontrol,
                                       snd_ctl_elem_value_t * ucontrol)
{
	unsigned long flags;
	emu10k1_t *emu = snd_kcontrol_chip(kcontrol);
	emu10k1_pcm_mixer_t *mix = &emu->pcm_mixer[snd_ctl_get_ioffidx(kcontrol, &ucontrol->id)];
	int idx;
	int num_efx = emu->audigy ? 8 : 4;

	spin_lock_irqsave(&emu->reg_lock, flags);
	for (idx = 0; idx < 3*num_efx; idx++)
		ucontrol->value.integer.value[idx] = mix->send_volume[idx/num_efx][idx%num_efx];
	spin_unlock_irqrestore(&emu->reg_lock, flags);
        return 0;
}

static int snd_emu10k1_send_volume_put(snd_kcontrol_t * kcontrol,
                                       snd_ctl_elem_value_t * ucontrol)
{
	unsigned long flags;
	emu10k1_t *emu = snd_kcontrol_chip(kcontrol);
	emu10k1_pcm_mixer_t *mix = &emu->pcm_mixer[snd_ctl_get_ioffidx(kcontrol, &ucontrol->id)];
	int change = 0, idx, val;
	int num_efx = emu->audigy ? 8 : 4;

	spin_lock_irqsave(&emu->reg_lock, flags);
	for (idx = 0; idx < 3*num_efx; idx++) {
		val = ucontrol->value.integer.value[idx] & 255;
		if (mix->send_volume[idx/num_efx][idx%num_efx] != val) {
			mix->send_volume[idx/num_efx][idx%num_efx] = val;
			change = 1;
		}
	}
	if (change && mix->epcm) {
		if (mix->epcm->voices[0] && mix->epcm->voices[1]) {
			update_emu10k1_send_volume(emu, mix->epcm->voices[0]->number,
						   &mix->send_volume[1][0]);
			update_emu10k1_send_volume(emu, mix->epcm->voices[1]->number,
						   &mix->send_volume[2][0]);
		} else if (mix->epcm->voices[0]) {
			update_emu10k1_send_volume(emu, mix->epcm->voices[0]->number,
						   &mix->send_volume[0][0]);
		}
	}
	spin_unlock_irqrestore(&emu->reg_lock, flags);
        return change;
}

static snd_kcontrol_new_t snd_emu10k1_send_volume_control =
{
	.access =	SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_INACTIVE,
        .iface =        SNDRV_CTL_ELEM_IFACE_MIXER,
        .name =         "EMU10K1 PCM Send Volume",
	.count =	32,
        .info =         snd_emu10k1_send_volume_info,
        .get =          snd_emu10k1_send_volume_get,
        .put =          snd_emu10k1_send_volume_put
};

static int snd_emu10k1_attn_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 3;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 0xffff;
	return 0;
}

static int snd_emu10k1_attn_get(snd_kcontrol_t * kcontrol,
                                snd_ctl_elem_value_t * ucontrol)
{
	emu10k1_t *emu = snd_kcontrol_chip(kcontrol);
	emu10k1_pcm_mixer_t *mix = &emu->pcm_mixer[snd_ctl_get_ioffidx(kcontrol, &ucontrol->id)];
	unsigned long flags;
	int idx;

	spin_lock_irqsave(&emu->reg_lock, flags);
	for (idx = 0; idx < 3; idx++)
		ucontrol->value.integer.value[idx] = mix->attn[idx];
	spin_unlock_irqrestore(&emu->reg_lock, flags);
        return 0;
}

static int snd_emu10k1_attn_put(snd_kcontrol_t * kcontrol,
				snd_ctl_elem_value_t * ucontrol)
{
	unsigned long flags;
	emu10k1_t *emu = snd_kcontrol_chip(kcontrol);
	emu10k1_pcm_mixer_t *mix = &emu->pcm_mixer[snd_ctl_get_ioffidx(kcontrol, &ucontrol->id)];
	int change = 0, idx, val;

	spin_lock_irqsave(&emu->reg_lock, flags);
	for (idx = 0; idx < 3; idx++) {
		val = ucontrol->value.integer.value[idx] & 0xffff;
		if (mix->attn[idx] != val) {
			mix->attn[idx] = val;
			change = 1;
		}
	}
	if (change && mix->epcm) {
		if (mix->epcm->voices[0] && mix->epcm->voices[1]) {
			snd_emu10k1_ptr_write(emu, VTFT_VOLUMETARGET, mix->epcm->voices[0]->number, mix->attn[1]);
			snd_emu10k1_ptr_write(emu, VTFT_VOLUMETARGET, mix->epcm->voices[1]->number, mix->attn[2]);
		} else if (mix->epcm->voices[0]) {
			snd_emu10k1_ptr_write(emu, VTFT_VOLUMETARGET, mix->epcm->voices[0]->number, mix->attn[0]);
		}
	}
	spin_unlock_irqrestore(&emu->reg_lock, flags);
        return change;
}

static snd_kcontrol_new_t snd_emu10k1_attn_control =
{
	.access =	SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_INACTIVE,
        .iface =        SNDRV_CTL_ELEM_IFACE_MIXER,
        .name =         "EMU10K1 PCM Volume",
	.count =	32,
        .info =         snd_emu10k1_attn_info,
        .get =          snd_emu10k1_attn_get,
        .put =          snd_emu10k1_attn_put
};

static int snd_emu10k1_shared_spdif_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int snd_emu10k1_shared_spdif_get(snd_kcontrol_t * kcontrol,
					snd_ctl_elem_value_t * ucontrol)
{
	emu10k1_t *emu = snd_kcontrol_chip(kcontrol);

	if (emu->audigy)
		ucontrol->value.integer.value[0] = inl(emu->port + A_IOCFG) & A_IOCFG_GPOUT0 ? 1 : 0;
	else
		ucontrol->value.integer.value[0] = inl(emu->port + HCFG) & HCFG_GPOUT0 ? 1 : 0;
        return 0;
}

static int snd_emu10k1_shared_spdif_put(snd_kcontrol_t * kcontrol,
					snd_ctl_elem_value_t * ucontrol)
{
	unsigned long flags;
	emu10k1_t *emu = snd_kcontrol_chip(kcontrol);
	unsigned int reg, val;
	int change = 0;

	spin_lock_irqsave(&emu->reg_lock, flags);
	if (emu->audigy) {
		reg = inl(emu->port + A_IOCFG);
		val = ucontrol->value.integer.value[0] ? A_IOCFG_GPOUT0 : 0;
		change = (reg & A_IOCFG_GPOUT0) != val;
		if (change) {
			reg &= ~A_IOCFG_GPOUT0;
			reg |= val;
			outl(reg | val, emu->port + A_IOCFG);
		}
	}
	reg = inl(emu->port + HCFG);
	val = ucontrol->value.integer.value[0] ? HCFG_GPOUT0 : 0;
	change |= (reg & HCFG_GPOUT0) != val;
	if (change) {
		reg &= ~HCFG_GPOUT0;
		reg |= val;
		outl(reg | val, emu->port + HCFG);
	}
	spin_unlock_irqrestore(&emu->reg_lock, flags);
        return change;
}

static snd_kcontrol_new_t snd_emu10k1_shared_spdif __devinitdata =
{
	.iface =	SNDRV_CTL_ELEM_IFACE_MIXER,
	.name =		"SB Live Analog/Digital Output Jack",
	.info =		snd_emu10k1_shared_spdif_info,
	.get =		snd_emu10k1_shared_spdif_get,
	.put =		snd_emu10k1_shared_spdif_put
};

static snd_kcontrol_new_t snd_audigy_shared_spdif __devinitdata =
{
	.iface =	SNDRV_CTL_ELEM_IFACE_MIXER,
	.name =		"Audigy Analog/Digital Output Jack",
	.info =		snd_emu10k1_shared_spdif_info,
	.get =		snd_emu10k1_shared_spdif_get,
	.put =		snd_emu10k1_shared_spdif_put
};

/*
 */
static void snd_emu10k1_mixer_free_ac97(ac97_t *ac97)
{
	emu10k1_t *emu = snd_magic_cast(emu10k1_t, ac97->private_data, return);
	emu->ac97 = NULL;
}

/*
 */
static int remove_ctl(snd_card_t *card, const char *name)
{
	snd_ctl_elem_id_t id;
	memset(&id, 0, sizeof(id));
	strcpy(id.name, name);
	id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	return snd_ctl_remove_id(card, &id);
}

static snd_kcontrol_t *ctl_find(snd_card_t *card, const char *name)
{
	snd_ctl_elem_id_t sid;
	memset(&sid, 0, sizeof(sid));
	strcpy(sid.name, name);
	sid.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	return snd_ctl_find_id(card, &sid);
}

static int rename_ctl(snd_card_t *card, const char *src, const char *dst)
{
	snd_kcontrol_t *kctl = ctl_find(card, src);
	if (kctl) {
		strcpy(kctl->id.name, dst);
		return 0;
	}
	return -ENOENT;
}

int __devinit snd_emu10k1_mixer(emu10k1_t *emu)
{
	int err, pcm;
	snd_kcontrol_t *kctl;
	snd_card_t *card = emu->card;
	char **c;
	static char *emu10k1_remove_ctls[] = {
		/* no AC97 mono, surround, center/lfe */
		"Master Mono Playback Switch",
		"Master Mono Playback Volume",
		"PCM Out Path & Mute",
		"Mono Output Select",
		"Surround Playback Switch",
		"Surround Playback Volume",
		"Center Playback Switch",
		"Center Playback Volume",
		"LFE Playback Switch",
		"LFE Playback Volume",
		NULL
	};
	static char *emu10k1_rename_ctls[] = {
		"Surround Digital Playback Volume", "Surround Playback Volume",
		"Center Digital Playback Volume", "Center Playback Volume",
		"LFE Digital Playback Volume", "LFE Playback Volume",
		NULL
	};
	static char *audigy_remove_ctls[] = {
		/* Master/PCM controls on ac97 of Audigy has no effect */
		"PCM Playback Switch",
		"PCM Playback Volume",
		"Master Mono Playback Switch",
		"Master Mono Playback Volume",
		"Master Playback Switch",
		"Master Playback Volume",
		"PCM Out Path & Mute",
		"Mono Output Select",
		/* remove unused AC97 capture controls */
		"Capture Source",
		"Capture Switch",
		"Capture Volume",
		"Mic Select",
		"Video Playback Switch",
		"Video Playback Volume",
		"Mic Playback Switch",
		"Mic Playback Volume",
		NULL
	};
	static char *audigy_rename_ctls[] = {
		/* use conventional names */
		"Wave Playback Volume", "PCM Playback Volume",
		/* "Wave Capture Volume", "PCM Capture Volume", */
		"Wave Master Playback Volume", "Master Playback Volume",
		"AMic Playback Volume", "Mic Playback Volume",
		NULL
	};

	if (!emu->no_ac97) {
		ac97_bus_t bus, *pbus;
		ac97_t ac97;

		memset(&bus, 0, sizeof(bus));
		bus.write = snd_emu10k1_ac97_write;
		bus.read = snd_emu10k1_ac97_read;
		if ((err = snd_ac97_bus(emu->card, &bus, &pbus)) < 0)
			return err;
		
		memset(&ac97, 0, sizeof(ac97));
		ac97.private_data = emu;
		ac97.private_free = snd_emu10k1_mixer_free_ac97;
		if ((err = snd_ac97_mixer(pbus, &ac97, &emu->ac97)) < 0)
			return err;
		if (emu->audigy) {
			/* set master volume to 0 dB */
			snd_ac97_write(emu->ac97, AC97_MASTER, 0x0202);
			/* set capture source to mic */
			snd_ac97_write(emu->ac97, AC97_REC_SEL, 0x0000);
			c = audigy_remove_ctls;
		} else {
			/*
			 * Credits for cards based on STAC9758:
			 *   James Courtier-Dutton <James@superbug.demon.co.uk>
			 *   Voluspa <voluspa@comhem.se>
			 */
			if (emu->ac97->id == AC97_ID_STAC9758) {
				emu->rear_ac97 = 1;
				snd_emu10k1_ptr_write(emu, AC97SLOT, 0, AC97SLOT_CNTR|AC97SLOT_LFE|AC97SLOT_REAR_LEFT|AC97SLOT_REAR_RIGHT);
			}
			/* remove unused AC97 controls */
			snd_ac97_write(emu->ac97, AC97_SURROUND_MASTER, 0x0202);
			snd_ac97_write(emu->ac97, AC97_CENTER_LFE_MASTER, 0x0202);
			c = emu10k1_remove_ctls;
		}
		for (; *c; c++)
			remove_ctl(card, *c);
	} else {
		if (emu->APS)
			strcpy(emu->card->mixername, "EMU APS");
		else if (emu->audigy)
			strcpy(emu->card->mixername, "SB Audigy");
		else
			strcpy(emu->card->mixername, "Emu10k1");
	}

	if (emu->audigy)
		c = audigy_rename_ctls;
	else
		c = emu10k1_rename_ctls;
	for (; *c; c += 2)
		rename_ctl(card, c[0], c[1]);

	if ((kctl = emu->ctl_send_routing = snd_ctl_new1(&snd_emu10k1_send_routing_control, emu)) == NULL)
		return -ENOMEM;
	if ((err = snd_ctl_add(card, kctl)))
		return err;
	if ((kctl = emu->ctl_send_volume = snd_ctl_new1(&snd_emu10k1_send_volume_control, emu)) == NULL)
		return -ENOMEM;
	if ((err = snd_ctl_add(card, kctl)))
		return err;
	if ((kctl = emu->ctl_attn = snd_ctl_new1(&snd_emu10k1_attn_control, emu)) == NULL)
		return -ENOMEM;
	if ((err = snd_ctl_add(card, kctl)))
		return err;

	/* intiailize the routing and volume table for each pcm playback stream */
	for (pcm = 0; pcm < 32; pcm++) {
		emu10k1_pcm_mixer_t *mix;
		int v;
		
		mix = &emu->pcm_mixer[pcm];
		mix->epcm = NULL;

		for (v = 0; v < 4; v++)
			mix->send_routing[0][v] = 
				mix->send_routing[1][v] = 
				mix->send_routing[2][v] = v;
		
		memset(&mix->send_volume, 0, sizeof(mix->send_volume));
		mix->send_volume[0][0] = mix->send_volume[0][1] =
		mix->send_volume[1][0] = mix->send_volume[2][1] = 255;
		
		mix->attn[0] = mix->attn[1] = mix->attn[2] = 0xffff;
	}
	
	if (! emu->APS) { /* FIXME: APS has these controls? */
		/* sb live! and audigy */
		if ((kctl = snd_ctl_new1(&snd_emu10k1_spdif_mask_control, emu)) == NULL)
			return -ENOMEM;
		if ((err = snd_ctl_add(card, kctl)))
			return err;
		if ((kctl = ctl_find(card, SNDRV_CTL_NAME_IEC958("",PLAYBACK,DEFAULT))) != NULL) {
			/* already defined by ac97, remove it */
			/* FIXME: or do we need both controls? */
			remove_ctl(card, SNDRV_CTL_NAME_IEC958("",PLAYBACK,DEFAULT));
		}
		if ((kctl = snd_ctl_new1(&snd_emu10k1_spdif_control, emu)) == NULL)
			return -ENOMEM;
		if ((err = snd_ctl_add(card, kctl)))
			return err;
	}

	if (emu->audigy) {
		if ((kctl = snd_ctl_new1(&snd_audigy_shared_spdif, emu)) == NULL)
			return -ENOMEM;
		if ((err = snd_ctl_add(card, kctl)))
			return err;
	} else if (! emu->APS) {
		/* sb live! */
		if ((kctl = snd_ctl_new1(&snd_emu10k1_shared_spdif, emu)) == NULL)
			return -ENOMEM;
		if ((err = snd_ctl_add(card, kctl)))
			return err;
	}

	return 0;
}
