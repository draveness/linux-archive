/*
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
 *  Routines for control of YMF724/740/744/754 chips
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
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <sound/core.h>
#include <sound/control.h>
#include <sound/info.h>
#include <sound/ymfpci.h>
#include <sound/asoundef.h>
#include <sound/mpu401.h>

#include <asm/io.h>

#define chip_t ymfpci_t

/*
 *  constants
 */

/*
 *  common I/O routines
 */

static void snd_ymfpci_irq_wait(ymfpci_t *chip);

static inline u8 snd_ymfpci_readb(ymfpci_t *chip, u32 offset)
{
	return readb(chip->reg_area_virt + offset);
}

static inline void snd_ymfpci_writeb(ymfpci_t *chip, u32 offset, u8 val)
{
	writeb(val, chip->reg_area_virt + offset);
}

static inline u16 snd_ymfpci_readw(ymfpci_t *chip, u32 offset)
{
	return readw(chip->reg_area_virt + offset);
}

static inline void snd_ymfpci_writew(ymfpci_t *chip, u32 offset, u16 val)
{
	writew(val, chip->reg_area_virt + offset);
}

static inline u32 snd_ymfpci_readl(ymfpci_t *chip, u32 offset)
{
	return readl(chip->reg_area_virt + offset);
}

static inline void snd_ymfpci_writel(ymfpci_t *chip, u32 offset, u32 val)
{
	writel(val, chip->reg_area_virt + offset);
}

static int snd_ymfpci_codec_ready(ymfpci_t *chip, int secondary)
{
	signed long end_time;
	u32 reg = secondary ? YDSXGR_SECSTATUSADR : YDSXGR_PRISTATUSADR;
	
	end_time = (jiffies + ((3 * HZ) / 4)) + 1;
	do {
		if ((snd_ymfpci_readw(chip, reg) & 0x8000) == 0)
			return 0;
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(1);
	} while (end_time - (signed long)jiffies >= 0);
	snd_printk("codec_ready: codec %i is not ready [0x%x]\n", secondary, snd_ymfpci_readw(chip, reg));
	return -EBUSY;
}

static void snd_ymfpci_codec_write(ac97_t *ac97, u16 reg, u16 val)
{
	ymfpci_t *chip = snd_magic_cast(ymfpci_t, ac97->private_data, return);
	u32 cmd;
	
	snd_ymfpci_codec_ready(chip, 0);
	cmd = ((YDSXG_AC97WRITECMD | reg) << 16) | val;
	snd_ymfpci_writel(chip, YDSXGR_AC97CMDDATA, cmd);
}

static u16 snd_ymfpci_codec_read(ac97_t *ac97, u16 reg)
{
	ymfpci_t *chip = snd_magic_cast(ymfpci_t, ac97->private_data, return -ENXIO);

	if (snd_ymfpci_codec_ready(chip, 0))
		return ~0;
	snd_ymfpci_writew(chip, YDSXGR_AC97CMDADR, YDSXG_AC97READCMD | reg);
	if (snd_ymfpci_codec_ready(chip, 0))
		return ~0;
	if (chip->device_id == PCI_DEVICE_ID_YAMAHA_744 && chip->rev < 2) {
		int i;
		for (i = 0; i < 600; i++)
			snd_ymfpci_readw(chip, YDSXGR_PRISTATUSDATA);
	}
	return snd_ymfpci_readw(chip, YDSXGR_PRISTATUSDATA);
}

/*
 *  Misc routines
 */

static u32 snd_ymfpci_calc_delta(u32 rate)
{
	switch (rate) {
	case 8000:	return 0x02aaab00;
	case 11025:	return 0x03accd00;
	case 16000:	return 0x05555500;
	case 22050:	return 0x07599a00;
	case 32000:	return 0x0aaaab00;
	case 44100:	return 0x0eb33300;
	default:	return ((rate << 16) / 375) << 5;
	}
}

static u32 def_rate[8] = {
	100, 2000, 8000, 11025, 16000, 22050, 32000, 48000
};

static u32 snd_ymfpci_calc_lpfK(u32 rate)
{
	u32 i;
	static u32 val[8] = {
		0x00570000, 0x06AA0000, 0x18B20000, 0x20930000,
		0x2B9A0000, 0x35A10000, 0x3EAA0000, 0x40000000
	};
	
	if (rate == 44100)
		return 0x40000000;	/* FIXME: What's the right value? */
	for (i = 0; i < 8; i++)
		if (rate <= def_rate[i])
			return val[i];
	return val[0];
}

static u32 snd_ymfpci_calc_lpfQ(u32 rate)
{
	u32 i;
	static u32 val[8] = {
		0x35280000, 0x34A70000, 0x32020000, 0x31770000,
		0x31390000, 0x31C90000, 0x33D00000, 0x40000000
	};
	
	if (rate == 44100)
		return 0x370A0000;
	for (i = 0; i < 8; i++)
		if (rate <= def_rate[i])
			return val[i];
	return val[0];
}

/*
 *  Hardware start management
 */

static void snd_ymfpci_hw_start(ymfpci_t *chip)
{
	unsigned long flags;

	spin_lock_irqsave(&chip->reg_lock, flags);
	if (chip->start_count++ > 0)
		goto __end;
	snd_ymfpci_writel(chip, YDSXGR_MODE,
			  snd_ymfpci_readl(chip, YDSXGR_MODE) | 3);
	chip->active_bank = snd_ymfpci_readl(chip, YDSXGR_CTRLSELECT) & 1;
      __end:
      	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

static void snd_ymfpci_hw_stop(ymfpci_t *chip)
{
	unsigned long flags;
	long timeout = 1000;

	spin_lock_irqsave(&chip->reg_lock, flags);
	if (--chip->start_count > 0)
		goto __end;
	snd_ymfpci_writel(chip, YDSXGR_MODE,
			  snd_ymfpci_readl(chip, YDSXGR_MODE) & ~3);
	while (timeout-- > 0) {
		if ((snd_ymfpci_readl(chip, YDSXGR_STATUS) & 2) == 0)
			break;
	}
	if (atomic_read(&chip->interrupt_sleep_count)) {
		atomic_set(&chip->interrupt_sleep_count, 0);
		wake_up(&chip->interrupt_sleep);
	}
      __end:
      	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/*
 *  Playback voice management
 */

static int voice_alloc(ymfpci_t *chip, ymfpci_voice_type_t type, int pair, ymfpci_voice_t **rvoice)
{
	ymfpci_voice_t *voice, *voice2;
	int idx;
	
	*rvoice = NULL;
	for (idx = 0; idx < YDSXG_PLAYBACK_VOICES; idx += pair ? 2 : 1) {
		voice = &chip->voices[idx];
		voice2 = pair ? &chip->voices[idx+1] : NULL;
		if (voice->use || (voice2 && voice2->use))
			continue;
		voice->use = 1;
		if (voice2)
			voice2->use = 1;
		switch (type) {
		case YMFPCI_PCM:
			voice->pcm = 1;
			if (voice2)
				voice2->pcm = 1;
			break;
		case YMFPCI_SYNTH:
			voice->synth = 1;
			break;
		case YMFPCI_MIDI:
			voice->midi = 1;
			break;
		}
		snd_ymfpci_hw_start(chip);
		if (voice2)
			snd_ymfpci_hw_start(chip);
		*rvoice = voice;
		return 0;
	}
	return -ENOMEM;
}

int snd_ymfpci_voice_alloc(ymfpci_t *chip, ymfpci_voice_type_t type, int pair, ymfpci_voice_t **rvoice)
{
	unsigned long flags;
	int result;
	
	snd_assert(rvoice != NULL, return -EINVAL);
	snd_assert(!pair || type == YMFPCI_PCM, return -EINVAL);
	
	spin_lock_irqsave(&chip->voice_lock, flags);
	for (;;) {
		result = voice_alloc(chip, type, pair, rvoice);
		if (result == 0 || type != YMFPCI_PCM)
			break;
		/* TODO: synth/midi voice deallocation */
		break;
	}
	spin_unlock_irqrestore(&chip->voice_lock, flags);	
	return result;		
}

int snd_ymfpci_voice_free(ymfpci_t *chip, ymfpci_voice_t *pvoice)
{
	unsigned long flags;
	
	snd_assert(pvoice != NULL, return -EINVAL);
	snd_ymfpci_hw_stop(chip);
	spin_lock_irqsave(&chip->voice_lock, flags);
	pvoice->use = pvoice->pcm = pvoice->synth = pvoice->midi = 0;
	pvoice->ypcm = NULL;
	pvoice->interrupt = NULL;
	spin_unlock_irqrestore(&chip->voice_lock, flags);
	return 0;
}

/*
 *  PCM part
 */

static void snd_ymfpci_pcm_interrupt(ymfpci_t *chip, ymfpci_voice_t *voice)
{
	ymfpci_pcm_t *ypcm;
	u32 pos, delta;
	
	if ((ypcm = voice->ypcm) == NULL)
		return;
	if (ypcm->substream == NULL)
		return;
	spin_lock(&chip->reg_lock);
	if (ypcm->running) {
		pos = le32_to_cpu(voice->bank[chip->active_bank].start);
		if (pos < ypcm->last_pos)
			delta = pos + (ypcm->buffer_size - ypcm->last_pos);
		else
			delta = pos - ypcm->last_pos;
		ypcm->period_pos += delta;
		ypcm->last_pos = pos;
		if (ypcm->period_pos >= ypcm->period_size) {
			// printk("done - active_bank = 0x%x, start = 0x%x\n", chip->active_bank, voice->bank[chip->active_bank].start);
			ypcm->period_pos %= ypcm->period_size;
			spin_unlock(&chip->reg_lock);
			snd_pcm_period_elapsed(ypcm->substream);
			spin_lock(&chip->reg_lock);
		}
	}
	spin_unlock(&chip->reg_lock);
}

static void snd_ymfpci_pcm_capture_interrupt(snd_pcm_substream_t *substream)
{
	snd_pcm_runtime_t *runtime = substream->runtime;
	ymfpci_pcm_t *ypcm = snd_magic_cast(ymfpci_pcm_t, runtime->private_data, return);
	ymfpci_t *chip = ypcm->chip;
	u32 pos, delta;
	
	spin_lock(&chip->reg_lock);
	if (ypcm->running) {
		pos = le32_to_cpu(chip->bank_capture[ypcm->capture_bank_number][chip->active_bank]->start) >> ypcm->shift;
		if (pos < ypcm->last_pos)
			delta = pos + (ypcm->buffer_size - ypcm->last_pos);
		else
			delta = pos - ypcm->last_pos;
		ypcm->period_pos += delta;
		ypcm->last_pos = pos;
		if (ypcm->period_pos >= ypcm->period_size) {
			ypcm->period_pos %= ypcm->period_size;
			// printk("done - active_bank = 0x%x, start = 0x%x\n", chip->active_bank, voice->bank[chip->active_bank].start);
			spin_unlock(&chip->reg_lock);
			snd_pcm_period_elapsed(substream);
			spin_lock(&chip->reg_lock);
		}
	}
	spin_unlock(&chip->reg_lock);
}

static int snd_ymfpci_playback_trigger(snd_pcm_substream_t * substream,
				       int cmd)
{
	ymfpci_t *chip = snd_pcm_substream_chip(substream);
	ymfpci_pcm_t *ypcm = snd_magic_cast(ymfpci_pcm_t, substream->runtime->private_data, return -ENXIO);
	int result = 0;

	spin_lock(&chip->reg_lock);
	if (ypcm->voices[0] == NULL) {
		result = -EINVAL;
		goto __unlock;
	}
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		chip->ctrl_playback[ypcm->voices[0]->number + 1] = cpu_to_le32(ypcm->voices[0]->bank_addr);
		if (ypcm->voices[1] != NULL)
			chip->ctrl_playback[ypcm->voices[1]->number + 1] = cpu_to_le32(ypcm->voices[1]->bank_addr);
		ypcm->running = 1;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		chip->ctrl_playback[ypcm->voices[0]->number + 1] = 0;
		if (ypcm->voices[1] != NULL)
			chip->ctrl_playback[ypcm->voices[1]->number + 1] = 0;
		ypcm->running = 0;
		break;
	default:
		result = -EINVAL;
		break;
	}
      __unlock:
	spin_unlock(&chip->reg_lock);
	return result;
}
static int snd_ymfpci_capture_trigger(snd_pcm_substream_t * substream,
				      int cmd)
{
	ymfpci_t *chip = snd_pcm_substream_chip(substream);
	ymfpci_pcm_t *ypcm = snd_magic_cast(ymfpci_pcm_t, substream->runtime->private_data, return -ENXIO);
	int result = 0;
	u32 tmp;

	spin_lock(&chip->reg_lock);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		tmp = snd_ymfpci_readl(chip, YDSXGR_MAPOFREC) | (1 << ypcm->capture_bank_number);
		snd_ymfpci_writel(chip, YDSXGR_MAPOFREC, tmp);
		ypcm->running = 1;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		tmp = snd_ymfpci_readl(chip, YDSXGR_MAPOFREC) & ~(1 << ypcm->capture_bank_number);
		snd_ymfpci_writel(chip, YDSXGR_MAPOFREC, tmp);
		ypcm->running = 0;
		break;
	default:
		result = -EINVAL;
		break;
	}
	spin_unlock(&chip->reg_lock);
	return result;
}

static int snd_ymfpci_pcm_voice_alloc(ymfpci_pcm_t *ypcm, int voices)
{
	int err;

	if (ypcm->voices[1] != NULL && voices < 2) {
		snd_ymfpci_voice_free(ypcm->chip, ypcm->voices[1]);
		ypcm->voices[1] = NULL;
	}
	if (voices == 1 && ypcm->voices[0] != NULL)
		return 0;		/* already allocated */
	if (voices == 2 && ypcm->voices[0] != NULL && ypcm->voices[1] != NULL)
		return 0;		/* already allocated */
	if (voices > 1) {
		if (ypcm->voices[0] != NULL && ypcm->voices[1] == NULL) {
			snd_ymfpci_voice_free(ypcm->chip, ypcm->voices[0]);
			ypcm->voices[0] = NULL;
		}		
	}
	err = snd_ymfpci_voice_alloc(ypcm->chip, YMFPCI_PCM, voices > 1, &ypcm->voices[0]);
	if (err < 0)
		return err;
	ypcm->voices[0]->ypcm = ypcm;
	ypcm->voices[0]->interrupt = snd_ymfpci_pcm_interrupt;
	if (voices > 1) {
		ypcm->voices[1] = &ypcm->chip->voices[ypcm->voices[0]->number + 1];
		ypcm->voices[1]->ypcm = ypcm;
	}
	return 0;
}

static void snd_ymfpci_pcm_init_voice(ymfpci_voice_t *voice, int stereo,
				      int rate, int w_16, unsigned long addr,
				      unsigned int end,
				      int output_front, int output_rear)
{
	u32 format;
	u32 delta = snd_ymfpci_calc_delta(rate);
	u32 lpfQ = snd_ymfpci_calc_lpfQ(rate);
	u32 lpfK = snd_ymfpci_calc_lpfK(rate);
	snd_ymfpci_playback_bank_t *bank;
	unsigned int nbank;

	snd_assert(voice != NULL, return);
	format = (stereo ? 0x00010000 : 0) | (w_16 ? 0 : 0x80000000);
	for (nbank = 0; nbank < 2; nbank++) {
		bank = &voice->bank[nbank];
		bank->format = cpu_to_le32(format);
		bank->loop_default = 0;
		bank->base = cpu_to_le32(addr);
		bank->loop_start = 0;
		bank->loop_end = cpu_to_le32(end);
		bank->loop_frac = 0;
		bank->eg_gain_end = cpu_to_le32(0x40000000);
		bank->lpfQ = cpu_to_le32(lpfQ);
		bank->status = 0;
		bank->num_of_frames = 0;
		bank->loop_count = 0;
		bank->start = 0;
		bank->start_frac = 0;
		bank->delta =
		bank->delta_end = cpu_to_le32(delta);
		bank->lpfK =
		bank->lpfK_end = cpu_to_le32(lpfK);
		bank->eg_gain = cpu_to_le32(0x40000000);
		bank->lpfD1 =
		bank->lpfD2 = 0;

		bank->left_gain = 
		bank->right_gain =
		bank->left_gain_end =
		bank->right_gain_end =
		bank->eff1_gain =
		bank->eff2_gain =
		bank->eff3_gain =
		bank->eff1_gain_end =
		bank->eff2_gain_end =
		bank->eff3_gain_end = 0;

		if (!stereo) {
			if (output_front) {
				bank->left_gain = 
				bank->right_gain =
				bank->left_gain_end =
				bank->right_gain_end = cpu_to_le32(0x40000000);
			}
			if (output_rear) {
				bank->eff2_gain =
				bank->eff2_gain_end =
				bank->eff3_gain =
				bank->eff3_gain_end = cpu_to_le32(0x40000000);
			}
		} else {
			if (output_front) {
				if ((voice->number & 1) == 0) {
					bank->left_gain =
					bank->left_gain_end = cpu_to_le32(0x40000000);
				} else {
					bank->format |= cpu_to_le32(1);
					bank->right_gain =
					bank->right_gain_end = cpu_to_le32(0x40000000);
				}
			}
			if (output_rear) {
				if ((voice->number & 1) == 0) {
					bank->eff3_gain =
					bank->eff3_gain_end = cpu_to_le32(0x40000000);
				} else {
					bank->format |= cpu_to_le32(1);
					bank->eff2_gain =
					bank->eff2_gain_end = cpu_to_le32(0x40000000);
				}
			}
		}
	}
}

static int __devinit snd_ymfpci_ac3_init(ymfpci_t *chip)
{
	if (snd_dma_alloc_pages(&chip->dma_dev, 4096, &chip->ac3_tmp_base) < 0)
		return -ENOMEM;

	chip->bank_effect[3][0]->base =
	chip->bank_effect[3][1]->base = cpu_to_le32(chip->ac3_tmp_base.addr);
	chip->bank_effect[3][0]->loop_end =
	chip->bank_effect[3][1]->loop_end = cpu_to_le32(1024);
	chip->bank_effect[4][0]->base =
	chip->bank_effect[4][1]->base = cpu_to_le32(chip->ac3_tmp_base.addr + 2048);
	chip->bank_effect[4][0]->loop_end =
	chip->bank_effect[4][1]->loop_end = cpu_to_le32(1024);

	spin_lock_irq(&chip->reg_lock);
	snd_ymfpci_writel(chip, YDSXGR_MAPOFEFFECT,
			  snd_ymfpci_readl(chip, YDSXGR_MAPOFEFFECT) | 3 << 3);
	spin_unlock_irq(&chip->reg_lock);
	return 0;
}

static int snd_ymfpci_ac3_done(ymfpci_t *chip)
{
	spin_lock_irq(&chip->reg_lock);
	snd_ymfpci_writel(chip, YDSXGR_MAPOFEFFECT,
			  snd_ymfpci_readl(chip, YDSXGR_MAPOFEFFECT) & ~(3 << 3));
	spin_unlock_irq(&chip->reg_lock);
	// snd_ymfpci_irq_wait(chip);
	if (chip->ac3_tmp_base.area) {
		snd_dma_free_pages(&chip->dma_dev, &chip->ac3_tmp_base);
		chip->ac3_tmp_base.area = NULL;
	}
	return 0;
}

static int snd_ymfpci_playback_hw_params(snd_pcm_substream_t * substream,
					 snd_pcm_hw_params_t * hw_params)
{
	snd_pcm_runtime_t *runtime = substream->runtime;
	ymfpci_pcm_t *ypcm = snd_magic_cast(ymfpci_pcm_t, runtime->private_data, return -ENXIO);
	int err;

	if ((err = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params))) < 0)
		return err;
	if ((err = snd_ymfpci_pcm_voice_alloc(ypcm, params_channels(hw_params))) < 0)
		return err;
	return 0;
}

static int snd_ymfpci_playback_hw_free(snd_pcm_substream_t * substream)
{
	ymfpci_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	ymfpci_pcm_t *ypcm;
	
	if (runtime->private_data == NULL)
		return 0;
	ypcm = snd_magic_cast(ymfpci_pcm_t, runtime->private_data, return -ENXIO);

	/* wait, until the PCI operations are not finished */
	snd_ymfpci_irq_wait(chip);
	snd_pcm_lib_free_pages(substream);
	if (ypcm->voices[1]) {
		snd_ymfpci_voice_free(chip, ypcm->voices[1]);
		ypcm->voices[1] = NULL;
	}
	if (ypcm->voices[0]) {
		snd_ymfpci_voice_free(chip, ypcm->voices[0]);
		ypcm->voices[0] = NULL;
	}
	return 0;
}

static int snd_ymfpci_playback_prepare(snd_pcm_substream_t * substream)
{
	// ymfpci_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	ymfpci_pcm_t *ypcm = snd_magic_cast(ymfpci_pcm_t, runtime->private_data, return -ENXIO);
	unsigned int nvoice;

	ypcm->period_size = runtime->period_size;
	ypcm->buffer_size = runtime->buffer_size;
	ypcm->period_pos = 0;
	ypcm->last_pos = 0;
	for (nvoice = 0; nvoice < runtime->channels; nvoice++)
		snd_ymfpci_pcm_init_voice(ypcm->voices[nvoice],
					  runtime->channels == 2,
					  runtime->rate,
					  snd_pcm_format_width(runtime->format) == 16,
					  runtime->dma_addr,
					  ypcm->buffer_size,
					  ypcm->output_front,
					  ypcm->output_rear);
	return 0;
}

static int snd_ymfpci_capture_hw_params(snd_pcm_substream_t * substream,
					snd_pcm_hw_params_t * hw_params)
{
	return snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
}

static int snd_ymfpci_capture_hw_free(snd_pcm_substream_t * substream)
{
	ymfpci_t *chip = snd_pcm_substream_chip(substream);

	/* wait, until the PCI operations are not finished */
	snd_ymfpci_irq_wait(chip);
	return snd_pcm_lib_free_pages(substream);
}

static int snd_ymfpci_capture_prepare(snd_pcm_substream_t * substream)
{
	ymfpci_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	ymfpci_pcm_t *ypcm = snd_magic_cast(ymfpci_pcm_t, runtime->private_data, return -ENXIO);
	snd_ymfpci_capture_bank_t * bank;
	int nbank;
	u32 rate, format;

	ypcm->period_size = runtime->period_size;
	ypcm->buffer_size = runtime->buffer_size;
	ypcm->period_pos = 0;
	ypcm->last_pos = 0;
	ypcm->shift = 0;
	rate = ((48000 * 4096) / runtime->rate) - 1;
	format = 0;
	if (runtime->channels == 2) {
		format |= 2;
		ypcm->shift++;
	}
	if (snd_pcm_format_width(runtime->format) == 8)
		format |= 1;
	else
		ypcm->shift++;
	switch (ypcm->capture_bank_number) {
	case 0:
		snd_ymfpci_writel(chip, YDSXGR_RECFORMAT, format);
		snd_ymfpci_writel(chip, YDSXGR_RECSLOTSR, rate);
		break;
	case 1:
		snd_ymfpci_writel(chip, YDSXGR_ADCFORMAT, format);
		snd_ymfpci_writel(chip, YDSXGR_ADCSLOTSR, rate);
		break;
	}
	for (nbank = 0; nbank < 2; nbank++) {
		bank = chip->bank_capture[ypcm->capture_bank_number][nbank];
		bank->base = cpu_to_le32(runtime->dma_addr);
		bank->loop_end = cpu_to_le32(ypcm->buffer_size << ypcm->shift);
		bank->start = 0;
		bank->num_of_loops = 0;
	}
	return 0;
}

static snd_pcm_uframes_t snd_ymfpci_playback_pointer(snd_pcm_substream_t * substream)
{
	ymfpci_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	ymfpci_pcm_t *ypcm = snd_magic_cast(ymfpci_pcm_t, runtime->private_data, return -ENXIO);
	ymfpci_voice_t *voice = ypcm->voices[0];

	if (!(ypcm->running && voice))
		return 0;
	return le32_to_cpu(voice->bank[chip->active_bank].start);
}

static snd_pcm_uframes_t snd_ymfpci_capture_pointer(snd_pcm_substream_t * substream)
{
	ymfpci_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	ymfpci_pcm_t *ypcm = snd_magic_cast(ymfpci_pcm_t, runtime->private_data, return -ENXIO);

	if (!ypcm->running)
		return 0;
	return le32_to_cpu(chip->bank_capture[ypcm->capture_bank_number][chip->active_bank]->start) >> ypcm->shift;
}

static void snd_ymfpci_irq_wait(ymfpci_t *chip)
{
	wait_queue_t wait;
	int loops = 4;

	while (loops-- > 0) {
		if ((snd_ymfpci_readl(chip, YDSXGR_MODE) & 3) == 0)
		 	continue;
		init_waitqueue_entry(&wait, current);
		add_wait_queue(&chip->interrupt_sleep, &wait);
		atomic_inc(&chip->interrupt_sleep_count);
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ/20);
		remove_wait_queue(&chip->interrupt_sleep, &wait);
	}
}

static irqreturn_t snd_ymfpci_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	ymfpci_t *chip = snd_magic_cast(ymfpci_t, dev_id, return IRQ_NONE);
	u32 status, nvoice, mode;
	ymfpci_voice_t *voice;

	status = snd_ymfpci_readl(chip, YDSXGR_STATUS);
	if (status & 0x80000000) {
		chip->active_bank = snd_ymfpci_readl(chip, YDSXGR_CTRLSELECT) & 1;
		spin_lock(&chip->voice_lock);
		for (nvoice = 0; nvoice < YDSXG_PLAYBACK_VOICES; nvoice++) {
			voice = &chip->voices[nvoice];
			if (voice->interrupt)
				voice->interrupt(chip, voice);
		}
		for (nvoice = 0; nvoice < YDSXG_CAPTURE_VOICES; nvoice++) {
			if (chip->capture_substream[nvoice])
				snd_ymfpci_pcm_capture_interrupt(chip->capture_substream[nvoice]);
		}
#if 0
		for (nvoice = 0; nvoice < YDSXG_EFFECT_VOICES; nvoice++) {
			if (chip->effect_substream[nvoice])
				snd_ymfpci_pcm_effect_interrupt(chip->effect_substream[nvoice]);
		}
#endif
		spin_unlock(&chip->voice_lock);
		spin_lock(&chip->reg_lock);
		snd_ymfpci_writel(chip, YDSXGR_STATUS, 0x80000000);
		mode = snd_ymfpci_readl(chip, YDSXGR_MODE) | 2;
		snd_ymfpci_writel(chip, YDSXGR_MODE, mode);
		spin_unlock(&chip->reg_lock);

		if (atomic_read(&chip->interrupt_sleep_count)) {
			atomic_set(&chip->interrupt_sleep_count, 0);
			wake_up(&chip->interrupt_sleep);
		}
	}

	status = snd_ymfpci_readw(chip, YDSXGR_INTFLAG);
	if (status & 1) {
		if (chip->timer)
			snd_timer_interrupt(chip->timer, chip->timer->sticks);
	}
	snd_ymfpci_writew(chip, YDSXGR_INTFLAG, status);

	if (chip->rawmidi)
		snd_mpu401_uart_interrupt(irq, chip->rawmidi->private_data, regs);
	return IRQ_HANDLED;
}

static snd_pcm_hardware_t snd_ymfpci_playback =
{
	.info =			(SNDRV_PCM_INFO_MMAP |
				 SNDRV_PCM_INFO_MMAP_VALID | 
				 SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_BLOCK_TRANSFER |
				 SNDRV_PCM_INFO_PAUSE |
				 SNDRV_PCM_INFO_RESUME),
	.formats =		SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S16_LE,
	.rates =		SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_48000,
	.rate_min =		8000,
	.rate_max =		48000,
	.channels_min =		1,
	.channels_max =		2,
	.buffer_bytes_max =	256 * 1024, /* FIXME: enough? */
	.period_bytes_min =	64,
	.period_bytes_max =	256 * 1024, /* FIXME: enough? */
	.periods_min =		3,
	.periods_max =		1024,
	.fifo_size =		0,
};

static snd_pcm_hardware_t snd_ymfpci_capture =
{
	.info =			(SNDRV_PCM_INFO_MMAP |
				 SNDRV_PCM_INFO_MMAP_VALID |
				 SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_BLOCK_TRANSFER |
				 SNDRV_PCM_INFO_PAUSE |
				 SNDRV_PCM_INFO_RESUME),
	.formats =		SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S16_LE,
	.rates =		SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_48000,
	.rate_min =		8000,
	.rate_max =		48000,
	.channels_min =		1,
	.channels_max =		2,
	.buffer_bytes_max =	256 * 1024, /* FIXME: enough? */
	.period_bytes_min =	64,
	.period_bytes_max =	256 * 1024, /* FIXME: enough? */
	.periods_min =		3,
	.periods_max =		1024,
	.fifo_size =		0,
};

static void snd_ymfpci_pcm_free_substream(snd_pcm_runtime_t *runtime)
{
	ymfpci_pcm_t *ypcm = snd_magic_cast(ymfpci_pcm_t, runtime->private_data, return);
	
	if (ypcm)
		snd_magic_kfree(ypcm);
}

static int snd_ymfpci_playback_open_1(snd_pcm_substream_t * substream)
{
	ymfpci_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	ymfpci_pcm_t *ypcm;

	ypcm = snd_magic_kcalloc(ymfpci_pcm_t, 0, GFP_KERNEL);
	if (ypcm == NULL)
		return -ENOMEM;
	ypcm->chip = chip;
	ypcm->type = PLAYBACK_VOICE;
	ypcm->substream = substream;
	runtime->hw = snd_ymfpci_playback;
	runtime->private_data = ypcm;
	runtime->private_free = snd_ymfpci_pcm_free_substream;
	/* FIXME? True value is 256/48 = 5.33333 ms */
	snd_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_PERIOD_TIME, 5333, UINT_MAX);
	return 0;
}

/* call with spinlock held */
static void ymfpci_open_extension(ymfpci_t *chip)
{
	if (! chip->rear_opened) {
		if (! chip->spdif_opened) /* set AC3 */
			snd_ymfpci_writel(chip, YDSXGR_MODE,
					  snd_ymfpci_readl(chip, YDSXGR_MODE) | (1 << 30));
		/* enable second codec (4CHEN) */
		snd_ymfpci_writew(chip, YDSXGR_SECCONFIG,
				  (snd_ymfpci_readw(chip, YDSXGR_SECCONFIG) & ~0x0330) | 0x0010);
	}
}

/* call with spinlock held */
static void ymfpci_close_extension(ymfpci_t *chip)
{
	if (! chip->rear_opened) {
		if (! chip->spdif_opened)
			snd_ymfpci_writel(chip, YDSXGR_MODE,
					  snd_ymfpci_readl(chip, YDSXGR_MODE) & ~(1 << 30));
		snd_ymfpci_writew(chip, YDSXGR_SECCONFIG,
				  (snd_ymfpci_readw(chip, YDSXGR_SECCONFIG) & ~0x0330) & ~0x0010);
	}
}

static int snd_ymfpci_playback_open(snd_pcm_substream_t * substream)
{
	ymfpci_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	ymfpci_pcm_t *ypcm;
	unsigned long flags;
	int err;
	
	if ((err = snd_ymfpci_playback_open_1(substream)) < 0)
		return err;
	ypcm = snd_magic_cast(ymfpci_pcm_t, runtime->private_data, return 0);
	ypcm->output_front = 1;
	ypcm->output_rear = chip->mode_dup4ch ? 1 : 0;
	spin_lock_irqsave(&chip->reg_lock, flags);
	if (ypcm->output_rear) {
		ymfpci_open_extension(chip);
		chip->rear_opened++;
	}
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return 0;
}

static int snd_ymfpci_playback_spdif_open(snd_pcm_substream_t * substream)
{
	ymfpci_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	ymfpci_pcm_t *ypcm;
	unsigned long flags;
	int err;
	
	if ((err = snd_ymfpci_playback_open_1(substream)) < 0)
		return err;
	ypcm = snd_magic_cast(ymfpci_pcm_t, runtime->private_data, return 0);
	ypcm->output_front = 0;
	ypcm->output_rear = 1;
	spin_lock_irqsave(&chip->reg_lock, flags);
	snd_ymfpci_writew(chip, YDSXGR_SPDIFOUTCTRL,
			  snd_ymfpci_readw(chip, YDSXGR_SPDIFOUTCTRL) | 2);
	ymfpci_open_extension(chip);
	chip->spdif_pcm_bits = chip->spdif_bits;
	snd_ymfpci_writew(chip, YDSXGR_SPDIFOUTSTATUS, chip->spdif_pcm_bits);
	chip->spdif_opened++;
	spin_unlock_irqrestore(&chip->reg_lock, flags);

	chip->spdif_pcm_ctl->vd[0].access &= ~SNDRV_CTL_ELEM_ACCESS_INACTIVE;
	snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE |
		       SNDRV_CTL_EVENT_MASK_INFO, &chip->spdif_pcm_ctl->id);
	return 0;
}

static int snd_ymfpci_playback_4ch_open(snd_pcm_substream_t * substream)
{
	ymfpci_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	ymfpci_pcm_t *ypcm;
	unsigned long flags;
	int err;
	
	if ((err = snd_ymfpci_playback_open_1(substream)) < 0)
		return err;
	ypcm = snd_magic_cast(ymfpci_pcm_t, runtime->private_data, return 0);
	ypcm->output_front = 0;
	ypcm->output_rear = 1;
	spin_lock_irqsave(&chip->reg_lock, flags);
	ymfpci_open_extension(chip);
	chip->rear_opened++;
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return 0;
}

static int snd_ymfpci_capture_open(snd_pcm_substream_t * substream,
				   u32 capture_bank_number)
{
	ymfpci_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	ymfpci_pcm_t *ypcm;

	ypcm = snd_magic_kcalloc(ymfpci_pcm_t, 0, GFP_KERNEL);
	if (ypcm == NULL)
		return -ENOMEM;
	ypcm->chip = chip;
	ypcm->type = capture_bank_number + CAPTURE_REC;
	ypcm->substream = substream;	
	ypcm->capture_bank_number = capture_bank_number;
	chip->capture_substream[capture_bank_number] = substream;
	runtime->hw = snd_ymfpci_capture;
	/* FIXME? True value is 256/48 = 5.33333 ms */
	snd_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_PERIOD_TIME, 5333, UINT_MAX);
	runtime->private_data = ypcm;
	runtime->private_free = snd_ymfpci_pcm_free_substream;
	snd_ymfpci_hw_start(chip);
	return 0;
}

static int snd_ymfpci_capture_rec_open(snd_pcm_substream_t * substream)
{
	return snd_ymfpci_capture_open(substream, 0);
}

static int snd_ymfpci_capture_ac97_open(snd_pcm_substream_t * substream)
{
	return snd_ymfpci_capture_open(substream, 1);
}

static int snd_ymfpci_playback_close_1(snd_pcm_substream_t * substream)
{
	return 0;
}

static int snd_ymfpci_playback_close(snd_pcm_substream_t * substream)
{
	ymfpci_t *chip = snd_pcm_substream_chip(substream);
	ymfpci_pcm_t *ypcm = snd_magic_cast(ymfpci_pcm_t, substream->runtime->private_data, return -ENXIO);
	unsigned long flags;

	spin_lock_irqsave(&chip->reg_lock, flags);
	if (ypcm->output_rear && chip->rear_opened > 0) {
		chip->rear_opened--;
		ymfpci_close_extension(chip);
	}
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return snd_ymfpci_playback_close_1(substream);
}

static int snd_ymfpci_playback_spdif_close(snd_pcm_substream_t * substream)
{
	ymfpci_t *chip = snd_pcm_substream_chip(substream);
	unsigned long flags;

	spin_lock_irqsave(&chip->reg_lock, flags);
	chip->spdif_opened = 0;
	ymfpci_close_extension(chip);
	snd_ymfpci_writew(chip, YDSXGR_SPDIFOUTCTRL,
			  snd_ymfpci_readw(chip, YDSXGR_SPDIFOUTCTRL) & ~2);
	snd_ymfpci_writew(chip, YDSXGR_SPDIFOUTSTATUS, chip->spdif_bits);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	chip->spdif_pcm_ctl->vd[0].access |= SNDRV_CTL_ELEM_ACCESS_INACTIVE;
	snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE |
		       SNDRV_CTL_EVENT_MASK_INFO, &chip->spdif_pcm_ctl->id);
	return snd_ymfpci_playback_close_1(substream);
}

static int snd_ymfpci_playback_4ch_close(snd_pcm_substream_t * substream)
{
	ymfpci_t *chip = snd_pcm_substream_chip(substream);
	unsigned long flags;

	spin_lock_irqsave(&chip->reg_lock, flags);
	if (chip->rear_opened > 0) {
		chip->rear_opened--;
		ymfpci_close_extension(chip);
	}
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return snd_ymfpci_playback_close_1(substream);
}

static int snd_ymfpci_capture_close(snd_pcm_substream_t * substream)
{
	ymfpci_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	ymfpci_pcm_t *ypcm = snd_magic_cast(ymfpci_pcm_t, runtime->private_data, return -ENXIO);

	if (ypcm != NULL) {
		chip->capture_substream[ypcm->capture_bank_number] = NULL;
		snd_ymfpci_hw_stop(chip);
	}
	return 0;
}

static snd_pcm_ops_t snd_ymfpci_playback_ops = {
	.open =			snd_ymfpci_playback_open,
	.close =		snd_ymfpci_playback_close,
	.ioctl =		snd_pcm_lib_ioctl,
	.hw_params =		snd_ymfpci_playback_hw_params,
	.hw_free =		snd_ymfpci_playback_hw_free,
	.prepare =		snd_ymfpci_playback_prepare,
	.trigger =		snd_ymfpci_playback_trigger,
	.pointer =		snd_ymfpci_playback_pointer,
};

static snd_pcm_ops_t snd_ymfpci_capture_rec_ops = {
	.open =			snd_ymfpci_capture_rec_open,
	.close =		snd_ymfpci_capture_close,
	.ioctl =		snd_pcm_lib_ioctl,
	.hw_params =		snd_ymfpci_capture_hw_params,
	.hw_free =		snd_ymfpci_capture_hw_free,
	.prepare =		snd_ymfpci_capture_prepare,
	.trigger =		snd_ymfpci_capture_trigger,
	.pointer =		snd_ymfpci_capture_pointer,
};

static void snd_ymfpci_pcm_free(snd_pcm_t *pcm)
{
	ymfpci_t *chip = snd_magic_cast(ymfpci_t, pcm->private_data, return);
	chip->pcm = NULL;
	snd_pcm_lib_preallocate_free_for_all(pcm);
}

int __devinit snd_ymfpci_pcm(ymfpci_t *chip, int device, snd_pcm_t ** rpcm)
{
	snd_pcm_t *pcm;
	int err;

	if (rpcm)
		*rpcm = NULL;
	if ((err = snd_pcm_new(chip->card, "YMFPCI", device, 32, 1, &pcm)) < 0)
		return err;
	pcm->private_data = chip;
	pcm->private_free = snd_ymfpci_pcm_free;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &snd_ymfpci_playback_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &snd_ymfpci_capture_rec_ops);

	/* global setup */
	pcm->info_flags = 0;
	strcpy(pcm->name, "YMFPCI");
	chip->pcm = pcm;

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV,
					      snd_dma_pci_data(chip->pci), 64*1024, 256*1024);

	if (rpcm)
		*rpcm = pcm;
	return 0;
}

static snd_pcm_ops_t snd_ymfpci_capture_ac97_ops = {
	.open =			snd_ymfpci_capture_ac97_open,
	.close =		snd_ymfpci_capture_close,
	.ioctl =		snd_pcm_lib_ioctl,
	.hw_params =		snd_ymfpci_capture_hw_params,
	.hw_free =		snd_ymfpci_capture_hw_free,
	.prepare =		snd_ymfpci_capture_prepare,
	.trigger =		snd_ymfpci_capture_trigger,
	.pointer =		snd_ymfpci_capture_pointer,
};

static void snd_ymfpci_pcm2_free(snd_pcm_t *pcm)
{
	ymfpci_t *chip = snd_magic_cast(ymfpci_t, pcm->private_data, return);
	chip->pcm2 = NULL;
	snd_pcm_lib_preallocate_free_for_all(pcm);
}

int __devinit snd_ymfpci_pcm2(ymfpci_t *chip, int device, snd_pcm_t ** rpcm)
{
	snd_pcm_t *pcm;
	int err;

	if (rpcm)
		*rpcm = NULL;
	if ((err = snd_pcm_new(chip->card, "YMFPCI - PCM2", device, 0, 1, &pcm)) < 0)
		return err;
	pcm->private_data = chip;
	pcm->private_free = snd_ymfpci_pcm2_free;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &snd_ymfpci_capture_ac97_ops);

	/* global setup */
	pcm->info_flags = 0;
	sprintf(pcm->name, "YMFPCI - %s",
		chip->device_id == PCI_DEVICE_ID_YAMAHA_754 ? "Direct Recording" : "AC'97");
	chip->pcm2 = pcm;

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV,
					      snd_dma_pci_data(chip->pci), 64*1024, 256*1024);

	if (rpcm)
		*rpcm = pcm;
	return 0;
}

static snd_pcm_ops_t snd_ymfpci_playback_spdif_ops = {
	.open =			snd_ymfpci_playback_spdif_open,
	.close =		snd_ymfpci_playback_spdif_close,
	.ioctl =		snd_pcm_lib_ioctl,
	.hw_params =		snd_ymfpci_playback_hw_params,
	.hw_free =		snd_ymfpci_playback_hw_free,
	.prepare =		snd_ymfpci_playback_prepare,
	.trigger =		snd_ymfpci_playback_trigger,
	.pointer =		snd_ymfpci_playback_pointer,
};

static void snd_ymfpci_pcm_spdif_free(snd_pcm_t *pcm)
{
	ymfpci_t *chip = snd_magic_cast(ymfpci_t, pcm->private_data, return);
	chip->pcm_spdif = NULL;
	snd_pcm_lib_preallocate_free_for_all(pcm);
}

int __devinit snd_ymfpci_pcm_spdif(ymfpci_t *chip, int device, snd_pcm_t ** rpcm)
{
	snd_pcm_t *pcm;
	int err;

	if (rpcm)
		*rpcm = NULL;
	if ((err = snd_pcm_new(chip->card, "YMFPCI - IEC958", device, 1, 0, &pcm)) < 0)
		return err;
	pcm->private_data = chip;
	pcm->private_free = snd_ymfpci_pcm_spdif_free;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &snd_ymfpci_playback_spdif_ops);

	/* global setup */
	pcm->info_flags = 0;
	strcpy(pcm->name, "YMFPCI - IEC958");
	chip->pcm_spdif = pcm;

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV,
					      snd_dma_pci_data(chip->pci), 64*1024, 256*1024);

	if (rpcm)
		*rpcm = pcm;
	return 0;
}

static snd_pcm_ops_t snd_ymfpci_playback_4ch_ops = {
	.open =			snd_ymfpci_playback_4ch_open,
	.close =		snd_ymfpci_playback_4ch_close,
	.ioctl =		snd_pcm_lib_ioctl,
	.hw_params =		snd_ymfpci_playback_hw_params,
	.hw_free =		snd_ymfpci_playback_hw_free,
	.prepare =		snd_ymfpci_playback_prepare,
	.trigger =		snd_ymfpci_playback_trigger,
	.pointer =		snd_ymfpci_playback_pointer,
};

static void snd_ymfpci_pcm_4ch_free(snd_pcm_t *pcm)
{
	ymfpci_t *chip = snd_magic_cast(ymfpci_t, pcm->private_data, return);
	chip->pcm_4ch = NULL;
	snd_pcm_lib_preallocate_free_for_all(pcm);
}

int __devinit snd_ymfpci_pcm_4ch(ymfpci_t *chip, int device, snd_pcm_t ** rpcm)
{
	snd_pcm_t *pcm;
	int err;

	if (rpcm)
		*rpcm = NULL;
	if ((err = snd_pcm_new(chip->card, "YMFPCI - Rear", device, 1, 0, &pcm)) < 0)
		return err;
	pcm->private_data = chip;
	pcm->private_free = snd_ymfpci_pcm_4ch_free;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &snd_ymfpci_playback_4ch_ops);

	/* global setup */
	pcm->info_flags = 0;
	strcpy(pcm->name, "YMFPCI - Rear PCM");
	chip->pcm_4ch = pcm;

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV,
					      snd_dma_pci_data(chip->pci), 64*1024, 256*1024);

	if (rpcm)
		*rpcm = pcm;
	return 0;
}

static int snd_ymfpci_spdif_default_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_IEC958;
	uinfo->count = 1;
	return 0;
}

static int snd_ymfpci_spdif_default_get(snd_kcontrol_t * kcontrol,
					snd_ctl_elem_value_t * ucontrol)
{
	ymfpci_t *chip = snd_kcontrol_chip(kcontrol);
	unsigned long flags;

	spin_lock_irqsave(&chip->reg_lock, flags);
	ucontrol->value.iec958.status[0] = (chip->spdif_bits >> 0) & 0xff;
	ucontrol->value.iec958.status[1] = (chip->spdif_bits >> 8) & 0xff;
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return 0;
}

static int snd_ymfpci_spdif_default_put(snd_kcontrol_t * kcontrol,
					 snd_ctl_elem_value_t * ucontrol)
{
	ymfpci_t *chip = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	unsigned int val;
	int change;

	val = ((ucontrol->value.iec958.status[0] & 0x3e) << 0) |
	      (ucontrol->value.iec958.status[1] << 8);
	spin_lock_irqsave(&chip->reg_lock, flags);
	change = chip->spdif_bits != val;
	chip->spdif_bits = val;
	if ((snd_ymfpci_readw(chip, YDSXGR_SPDIFOUTCTRL) & 1) && chip->pcm_spdif == NULL)
		snd_ymfpci_writew(chip, YDSXGR_SPDIFOUTSTATUS, chip->spdif_bits);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return change;
}

static snd_kcontrol_new_t snd_ymfpci_spdif_default __devinitdata =
{
	.iface =	SNDRV_CTL_ELEM_IFACE_PCM,
	.name =         SNDRV_CTL_NAME_IEC958("",PLAYBACK,DEFAULT),
	.info =		snd_ymfpci_spdif_default_info,
	.get =		snd_ymfpci_spdif_default_get,
	.put =		snd_ymfpci_spdif_default_put
};

static int snd_ymfpci_spdif_mask_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_IEC958;
	uinfo->count = 1;
	return 0;
}

static int snd_ymfpci_spdif_mask_get(snd_kcontrol_t * kcontrol,
				      snd_ctl_elem_value_t * ucontrol)
{
	ymfpci_t *chip = snd_kcontrol_chip(kcontrol);
	unsigned long flags;

	spin_lock_irqsave(&chip->reg_lock, flags);
	ucontrol->value.iec958.status[0] = 0x3e;
	ucontrol->value.iec958.status[1] = 0xff;
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return 0;
}

static snd_kcontrol_new_t snd_ymfpci_spdif_mask __devinitdata =
{
	.access =	SNDRV_CTL_ELEM_ACCESS_READ,
	.iface =	SNDRV_CTL_ELEM_IFACE_PCM,
	.name =         SNDRV_CTL_NAME_IEC958("",PLAYBACK,CON_MASK),
	.info =		snd_ymfpci_spdif_mask_info,
	.get =		snd_ymfpci_spdif_mask_get,
};

static int snd_ymfpci_spdif_stream_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_IEC958;
	uinfo->count = 1;
	return 0;
}

static int snd_ymfpci_spdif_stream_get(snd_kcontrol_t * kcontrol,
					snd_ctl_elem_value_t * ucontrol)
{
	ymfpci_t *chip = snd_kcontrol_chip(kcontrol);
	unsigned long flags;

	spin_lock_irqsave(&chip->reg_lock, flags);
	ucontrol->value.iec958.status[0] = (chip->spdif_pcm_bits >> 0) & 0xff;
	ucontrol->value.iec958.status[1] = (chip->spdif_pcm_bits >> 8) & 0xff;
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return 0;
}

static int snd_ymfpci_spdif_stream_put(snd_kcontrol_t * kcontrol,
					snd_ctl_elem_value_t * ucontrol)
{
	ymfpci_t *chip = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	unsigned int val;
	int change;

	val = ((ucontrol->value.iec958.status[0] & 0x3e) << 0) |
	      (ucontrol->value.iec958.status[1] << 8);
	spin_lock_irqsave(&chip->reg_lock, flags);
	change = chip->spdif_pcm_bits != val;
	chip->spdif_pcm_bits = val;
	if ((snd_ymfpci_readw(chip, YDSXGR_SPDIFOUTCTRL) & 2))
		snd_ymfpci_writew(chip, YDSXGR_SPDIFOUTSTATUS, chip->spdif_pcm_bits);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return change;
}

static snd_kcontrol_new_t snd_ymfpci_spdif_stream __devinitdata =
{
	.access =	SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_INACTIVE,
	.iface =	SNDRV_CTL_ELEM_IFACE_PCM,
	.name =         SNDRV_CTL_NAME_IEC958("",PLAYBACK,PCM_STREAM),
	.info =		snd_ymfpci_spdif_stream_info,
	.get =		snd_ymfpci_spdif_stream_get,
	.put =		snd_ymfpci_spdif_stream_put
};

static int snd_ymfpci_drec_source_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t *info)
{
	static char *texts[3] = {"AC'97", "IEC958", "ZV Port"};

	info->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	info->count = 1;
	info->value.enumerated.items = 3;
	if (info->value.enumerated.item > 2)
		info->value.enumerated.item = 2;
	strcpy(info->value.enumerated.name, texts[info->value.enumerated.item]);
	return 0;
}

static int snd_ymfpci_drec_source_get(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t *value)
{
	ymfpci_t *chip = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	u16 reg;

	spin_lock_irqsave(&chip->reg_lock, flags);
	reg = snd_ymfpci_readw(chip, YDSXGR_GLOBALCTRL);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	if (!(reg & 0x100))
		value->value.enumerated.item[0] = 0;
	else
		value->value.enumerated.item[0] = 1 + ((reg & 0x200) != 0);
	return 0;
}

static int snd_ymfpci_drec_source_put(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t *value)
{
	ymfpci_t *chip = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	u16 reg, old_reg;

	spin_lock_irqsave(&chip->reg_lock, flags);
	old_reg = snd_ymfpci_readw(chip, YDSXGR_GLOBALCTRL);
	if (value->value.enumerated.item[0] == 0)
		reg = old_reg & ~0x100;
	else
		reg = (old_reg & ~0x300) | 0x100 | ((value->value.enumerated.item[0] == 2) << 9);
	snd_ymfpci_writew(chip, YDSXGR_GLOBALCTRL, reg);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return reg != old_reg;
}

static snd_kcontrol_new_t snd_ymfpci_drec_source __devinitdata = {
	.access =	SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.iface =	SNDRV_CTL_ELEM_IFACE_MIXER,
	.name =		"Direct Recording Source",
	.info =		snd_ymfpci_drec_source_info,
	.get =		snd_ymfpci_drec_source_get,
	.put =		snd_ymfpci_drec_source_put
};

/*
 *  Mixer controls
 */

#define YMFPCI_SINGLE(xname, xindex, reg) \
{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, .index = xindex, \
  .info = snd_ymfpci_info_single, \
  .get = snd_ymfpci_get_single, .put = snd_ymfpci_put_single, \
  .private_value = reg }

static int snd_ymfpci_info_single(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	unsigned int mask = 1;

	switch (kcontrol->private_value) {
	case YDSXGR_SPDIFOUTCTRL: break;
	case YDSXGR_SPDIFINCTRL: break;
	default: return -EINVAL;
	}
	uinfo->type = mask == 1 ? SNDRV_CTL_ELEM_TYPE_BOOLEAN : SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = mask;
	return 0;
}

static int snd_ymfpci_get_single(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ymfpci_t *chip = snd_kcontrol_chip(kcontrol);
	int reg = kcontrol->private_value;
	unsigned int shift = 0, mask = 1, invert = 0;
	
	switch (kcontrol->private_value) {
	case YDSXGR_SPDIFOUTCTRL: break;
	case YDSXGR_SPDIFINCTRL: break;
	default: return -EINVAL;
	}
	ucontrol->value.integer.value[0] = (snd_ymfpci_readl(chip, reg) >> shift) & mask;
	if (invert)
		ucontrol->value.integer.value[0] = mask - ucontrol->value.integer.value[0];
	return 0;
}

static int snd_ymfpci_put_single(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ymfpci_t *chip = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int reg = kcontrol->private_value;
	unsigned int shift = 0, mask = 1, invert = 0;
	int change;
	unsigned int val, oval;
	
	switch (kcontrol->private_value) {
	case YDSXGR_SPDIFOUTCTRL: break;
	case YDSXGR_SPDIFINCTRL: break;
	default: return -EINVAL;
	}
	val = (ucontrol->value.integer.value[0] & mask);
	if (invert)
		val = mask - val;
	val <<= shift;
	spin_lock_irqsave(&chip->reg_lock, flags);
	oval = snd_ymfpci_readl(chip, reg);
	val = (oval & ~(mask << shift)) | val;
	change = val != oval;
	snd_ymfpci_writel(chip, reg, val);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return change;
}

#define YMFPCI_DOUBLE(xname, xindex, reg) \
{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, .index = xindex, \
  .info = snd_ymfpci_info_double, \
  .get = snd_ymfpci_get_double, .put = snd_ymfpci_put_double, \
  .private_value = reg }

static int snd_ymfpci_info_double(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	unsigned int reg = kcontrol->private_value;
	unsigned int mask = 16383;

	if (reg < 0x80 || reg >= 0xc0)
		return -EINVAL;
	uinfo->type = mask == 1 ? SNDRV_CTL_ELEM_TYPE_BOOLEAN : SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 2;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = mask;
	return 0;
}

static int snd_ymfpci_get_double(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ymfpci_t *chip = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	unsigned int reg = kcontrol->private_value;
	unsigned int shift_left = 0, shift_right = 16, mask = 16383, invert = 0;
	unsigned int val;
	
	if (reg < 0x80 || reg >= 0xc0)
		return -EINVAL;
	spin_lock_irqsave(&chip->reg_lock, flags);
	val = snd_ymfpci_readl(chip, reg);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	ucontrol->value.integer.value[0] = (val >> shift_left) & mask;
	ucontrol->value.integer.value[1] = (val >> shift_right) & mask;
	if (invert) {
		ucontrol->value.integer.value[0] = mask - ucontrol->value.integer.value[0];
		ucontrol->value.integer.value[1] = mask - ucontrol->value.integer.value[1];
	}
	return 0;
}

static int snd_ymfpci_put_double(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ymfpci_t *chip = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	unsigned int reg = kcontrol->private_value;
	unsigned int shift_left = 0, shift_right = 16, mask = 16383, invert = 0;
	int change;
	unsigned int val1, val2, oval;
	
	if (reg < 0x80 || reg >= 0xc0)
		return -EINVAL;
	val1 = ucontrol->value.integer.value[0] & mask;
	val2 = ucontrol->value.integer.value[1] & mask;
	if (invert) {
		val1 = mask - val1;
		val2 = mask - val2;
	}
	val1 <<= shift_left;
	val2 <<= shift_right;
	spin_lock_irqsave(&chip->reg_lock, flags);
	oval = snd_ymfpci_readl(chip, reg);
	val1 = (oval & ~((mask << shift_left) | (mask << shift_right))) | val1 | val2;
	change = val1 != oval;
	snd_ymfpci_writel(chip, reg, val1);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return change;
}

/*
 * 4ch duplication
 */
static int snd_ymfpci_info_dup4ch(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int snd_ymfpci_get_dup4ch(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ymfpci_t *chip = snd_kcontrol_chip(kcontrol);
	ucontrol->value.integer.value[0] = chip->mode_dup4ch;
	return 0;
}

static int snd_ymfpci_put_dup4ch(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ymfpci_t *chip = snd_kcontrol_chip(kcontrol);
	int change;
	change = (ucontrol->value.integer.value[0] != chip->mode_dup4ch);
	if (change)
		chip->mode_dup4ch = !!ucontrol->value.integer.value[0];
	return change;
}


#define YMFPCI_CONTROLS (sizeof(snd_ymfpci_controls)/sizeof(snd_kcontrol_new_t))

static snd_kcontrol_new_t snd_ymfpci_controls[] __devinitdata = {
YMFPCI_DOUBLE("Wave Playback Volume", 0, YDSXGR_NATIVEDACOUTVOL),
YMFPCI_DOUBLE("Wave Capture Volume", 0, YDSXGR_NATIVEDACLOOPVOL),
YMFPCI_DOUBLE("Digital Capture Volume", 0, YDSXGR_NATIVEDACINVOL),
YMFPCI_DOUBLE("Digital Capture Volume", 1, YDSXGR_NATIVEADCINVOL),
YMFPCI_DOUBLE("ADC Playback Volume", 0, YDSXGR_PRIADCOUTVOL),
YMFPCI_DOUBLE("ADC Capture Volume", 0, YDSXGR_PRIADCLOOPVOL),
YMFPCI_DOUBLE("ADC Playback Volume", 1, YDSXGR_SECADCOUTVOL),
YMFPCI_DOUBLE("ADC Capture Volume", 1, YDSXGR_SECADCLOOPVOL),
YMFPCI_DOUBLE("FM Legacy Volume", 0, YDSXGR_LEGACYOUTVOL),
YMFPCI_DOUBLE(SNDRV_CTL_NAME_IEC958("AC97 ", PLAYBACK,VOLUME), 0, YDSXGR_ZVOUTVOL),
YMFPCI_DOUBLE(SNDRV_CTL_NAME_IEC958("", CAPTURE,VOLUME), 0, YDSXGR_ZVLOOPVOL),
YMFPCI_DOUBLE(SNDRV_CTL_NAME_IEC958("AC97 ",PLAYBACK,VOLUME), 1, YDSXGR_SPDIFOUTVOL),
YMFPCI_DOUBLE(SNDRV_CTL_NAME_IEC958("",CAPTURE,VOLUME), 1, YDSXGR_SPDIFLOOPVOL),
YMFPCI_SINGLE(SNDRV_CTL_NAME_IEC958("",PLAYBACK,SWITCH), 0, YDSXGR_SPDIFOUTCTRL),
YMFPCI_SINGLE(SNDRV_CTL_NAME_IEC958("",CAPTURE,SWITCH), 0, YDSXGR_SPDIFINCTRL),
{
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "4ch Duplication",
	.info = snd_ymfpci_info_dup4ch,
	.get = snd_ymfpci_get_dup4ch,
	.put = snd_ymfpci_put_dup4ch,
},
};


/*
 * GPIO
 */

static int snd_ymfpci_get_gpio_out(ymfpci_t *chip, int pin)
{
	u16 reg, mode;
	unsigned long flags;

	spin_lock_irqsave(&chip->reg_lock, flags);
	reg = snd_ymfpci_readw(chip, YDSXGR_GPIOFUNCENABLE);
	reg &= ~(1 << (pin + 8));
	reg |= (1 << pin);
	snd_ymfpci_writew(chip, YDSXGR_GPIOFUNCENABLE, reg);
	/* set the level mode for input line */
	mode = snd_ymfpci_readw(chip, YDSXGR_GPIOTYPECONFIG);
	mode &= ~(3 << (pin * 2));
	snd_ymfpci_writew(chip, YDSXGR_GPIOTYPECONFIG, mode);
	snd_ymfpci_writew(chip, YDSXGR_GPIOFUNCENABLE, reg | (1 << (pin + 8)));
	mode = snd_ymfpci_readw(chip, YDSXGR_GPIOINSTATUS);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return (mode >> pin) & 1;
}

static int snd_ymfpci_set_gpio_out(ymfpci_t *chip, int pin, int enable)
{
	u16 reg;
	unsigned long flags;

	spin_lock_irqsave(&chip->reg_lock, flags);
	reg = snd_ymfpci_readw(chip, YDSXGR_GPIOFUNCENABLE);
	reg &= ~(1 << pin);
	reg &= ~(1 << (pin + 8));
	snd_ymfpci_writew(chip, YDSXGR_GPIOFUNCENABLE, reg);
	snd_ymfpci_writew(chip, YDSXGR_GPIOOUTCTRL, enable << pin);
	snd_ymfpci_writew(chip, YDSXGR_GPIOFUNCENABLE, reg | (1 << (pin + 8)));
	spin_unlock_irqrestore(&chip->reg_lock, flags);

	return 0;
}

static int snd_ymfpci_gpio_sw_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int snd_ymfpci_gpio_sw_get(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t *ucontrol)
{
	ymfpci_t *chip = snd_kcontrol_chip(kcontrol);
	int pin = (int)kcontrol->private_value;
	ucontrol->value.integer.value[0] = snd_ymfpci_get_gpio_out(chip, pin);
	return 0;
}

static int snd_ymfpci_gpio_sw_put(snd_kcontrol_t *kcontrol, snd_ctl_elem_value_t *ucontrol)
{
	ymfpci_t *chip = snd_kcontrol_chip(kcontrol);
	int pin = (int)kcontrol->private_value;

	if (snd_ymfpci_get_gpio_out(chip, pin) != ucontrol->value.integer.value[0]) {
		snd_ymfpci_set_gpio_out(chip, pin, !!ucontrol->value.integer.value[0]);
		ucontrol->value.integer.value[0] = snd_ymfpci_get_gpio_out(chip, pin);
		return 1;
	}
	return 0;
}

static snd_kcontrol_new_t snd_ymfpci_rear_shared __devinitdata = {
	.name = "Shared Rear/Line-In Switch",
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.info = snd_ymfpci_gpio_sw_info,
	.get = snd_ymfpci_gpio_sw_get,
	.put = snd_ymfpci_gpio_sw_put,
	.private_value = 2,
};


/*
 *  Mixer routines
 */

static void snd_ymfpci_mixer_free_ac97_bus(ac97_bus_t *bus)
{
	ymfpci_t *chip = snd_magic_cast(ymfpci_t, bus->private_data, return);
	chip->ac97_bus = NULL;
}

static void snd_ymfpci_mixer_free_ac97(ac97_t *ac97)
{
	ymfpci_t *chip = snd_magic_cast(ymfpci_t, ac97->private_data, return);
	chip->ac97 = NULL;
}

int __devinit snd_ymfpci_mixer(ymfpci_t *chip, int rear_switch)
{
	ac97_bus_t bus;
	ac97_t ac97;
	snd_kcontrol_t *kctl;
	unsigned int idx;
	int err;

	memset(&bus, 0, sizeof(bus));
	bus.write = snd_ymfpci_codec_write;
	bus.read = snd_ymfpci_codec_read;
	bus.private_data = chip;
	bus.private_free = snd_ymfpci_mixer_free_ac97_bus;
	if ((err = snd_ac97_bus(chip->card, &bus, &chip->ac97_bus)) < 0)
		return err;

	memset(&ac97, 0, sizeof(ac97));
	ac97.private_data = chip;
	ac97.private_free = snd_ymfpci_mixer_free_ac97;
	if ((err = snd_ac97_mixer(chip->ac97_bus, &ac97, &chip->ac97)) < 0)
		return err;

	for (idx = 0; idx < YMFPCI_CONTROLS; idx++) {
		if ((err = snd_ctl_add(chip->card, snd_ctl_new1(&snd_ymfpci_controls[idx], chip))) < 0)
			return err;
	}

	/* add S/PDIF control */
	snd_assert(chip->pcm_spdif != NULL, return -EIO);
	if ((err = snd_ctl_add(chip->card, kctl = snd_ctl_new1(&snd_ymfpci_spdif_default, chip))) < 0)
		return err;
	kctl->id.device = chip->pcm_spdif->device;
	if ((err = snd_ctl_add(chip->card, kctl = snd_ctl_new1(&snd_ymfpci_spdif_mask, chip))) < 0)
		return err;
	kctl->id.device = chip->pcm_spdif->device;
	if ((err = snd_ctl_add(chip->card, kctl = snd_ctl_new1(&snd_ymfpci_spdif_stream, chip))) < 0)
		return err;
	kctl->id.device = chip->pcm_spdif->device;
	chip->spdif_pcm_ctl = kctl;

	/* direct recording source */
	if (chip->device_id == PCI_DEVICE_ID_YAMAHA_754 &&
	    (err = snd_ctl_add(chip->card, kctl = snd_ctl_new1(&snd_ymfpci_drec_source, chip))) < 0)
		return err;

	/*
	 * shared rear/line-in
	 */
	if (rear_switch) {
		if ((err = snd_ctl_add(chip->card, snd_ctl_new1(&snd_ymfpci_rear_shared, chip))) < 0)
			return err;
	}

	return 0;
}


/*
 * timer
 */

static int snd_ymfpci_timer_start(snd_timer_t *timer)
{
	ymfpci_t *chip;
	unsigned long flags;
	unsigned int count;

	chip = snd_timer_chip(timer);
	count = timer->sticks - 1;
	if (count == 0) /* minimum time is 20.8 us */
		count = 1;
	spin_lock_irqsave(&chip->reg_lock, flags);
	snd_ymfpci_writew(chip, YDSXGR_TIMERCOUNT, count);
	snd_ymfpci_writeb(chip, YDSXGR_TIMERCTRL, 0x03);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return 0;
}

static int snd_ymfpci_timer_stop(snd_timer_t *timer)
{
	ymfpci_t *chip;
	unsigned long flags;

	chip = snd_timer_chip(timer);
	spin_lock_irqsave(&chip->reg_lock, flags);
	snd_ymfpci_writeb(chip, YDSXGR_TIMERCTRL, 0x00);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return 0;
}

static int snd_ymfpci_timer_precise_resolution(snd_timer_t *timer,
					       unsigned long *num, unsigned long *den)
{
	*num = 1;
	*den = 96000;
	return 0;
}

static struct _snd_timer_hardware snd_ymfpci_timer_hw = {
	.flags = SNDRV_TIMER_HW_AUTO,
	.resolution = 10417, /* 1/2fs = 10.41666...us */
	.ticks = 65536,
	.start = snd_ymfpci_timer_start,
	.stop = snd_ymfpci_timer_stop,
	.precise_resolution = snd_ymfpci_timer_precise_resolution,
};

int __devinit snd_ymfpci_timer(ymfpci_t *chip, int device)
{
	snd_timer_t *timer = NULL;
	snd_timer_id_t tid;
	int err;

	tid.dev_class = SNDRV_TIMER_CLASS_CARD;
	tid.dev_sclass = SNDRV_TIMER_SCLASS_NONE;
	tid.card = chip->card->number;
	tid.device = device;
	tid.subdevice = 0;
	if ((err = snd_timer_new(chip->card, "YMFPCI", &tid, &timer)) >= 0) {
		strcpy(timer->name, "YMFPCI timer");
		timer->private_data = chip;
		timer->hw = snd_ymfpci_timer_hw;
	}
	chip->timer = timer;
	return err;
}


/*
 *  proc interface
 */

static void snd_ymfpci_proc_read(snd_info_entry_t *entry, 
				 snd_info_buffer_t * buffer)
{
	ymfpci_t *chip = snd_magic_cast(ymfpci_t, entry->private_data, return);
	int i;
	
	snd_iprintf(buffer, "YMFPCI\n\n");
	for (i = 0; i <= YDSXGR_WORKBASE; i += 4)
		snd_iprintf(buffer, "%04x: %04x\n", i, snd_ymfpci_readl(chip, i));
}

static int __devinit snd_ymfpci_proc_init(snd_card_t * card, ymfpci_t *chip)
{
	snd_info_entry_t *entry;
	
	if (! snd_card_proc_new(card, "ymfpci", &entry))
		snd_info_set_text_ops(entry, chip, 1024, snd_ymfpci_proc_read);
	return 0;
}

/*
 *  initialization routines
 */

static void snd_ymfpci_aclink_reset(struct pci_dev * pci)
{
	u8 cmd;

	pci_read_config_byte(pci, PCIR_DSXG_CTRL, &cmd);
#if 0 // force to reset
	if (cmd & 0x03) {
#endif
		pci_write_config_byte(pci, PCIR_DSXG_CTRL, cmd & 0xfc);
		pci_write_config_byte(pci, PCIR_DSXG_CTRL, cmd | 0x03);
		pci_write_config_byte(pci, PCIR_DSXG_CTRL, cmd & 0xfc);
		pci_write_config_word(pci, PCIR_DSXG_PWRCTRL1, 0);
		pci_write_config_word(pci, PCIR_DSXG_PWRCTRL2, 0);
#if 0
	}
#endif
}

static void snd_ymfpci_enable_dsp(ymfpci_t *chip)
{
	snd_ymfpci_writel(chip, YDSXGR_CONFIG, 0x00000001);
}

static void snd_ymfpci_disable_dsp(ymfpci_t *chip)
{
	u32 val;
	int timeout = 1000;

	val = snd_ymfpci_readl(chip, YDSXGR_CONFIG);
	if (val)
		snd_ymfpci_writel(chip, YDSXGR_CONFIG, 0x00000000);
	while (timeout-- > 0) {
		val = snd_ymfpci_readl(chip, YDSXGR_STATUS);
		if ((val & 0x00000002) == 0)
			break;
	}
}

#include "ymfpci_image.h"

static void snd_ymfpci_download_image(ymfpci_t *chip)
{
	int i;
	u16 ctrl;
	unsigned long *inst;

	snd_ymfpci_writel(chip, YDSXGR_NATIVEDACOUTVOL, 0x00000000);
	snd_ymfpci_disable_dsp(chip);
	snd_ymfpci_writel(chip, YDSXGR_MODE, 0x00010000);
	snd_ymfpci_writel(chip, YDSXGR_MODE, 0x00000000);
	snd_ymfpci_writel(chip, YDSXGR_MAPOFREC, 0x00000000);
	snd_ymfpci_writel(chip, YDSXGR_MAPOFEFFECT, 0x00000000);
	snd_ymfpci_writel(chip, YDSXGR_PLAYCTRLBASE, 0x00000000);
	snd_ymfpci_writel(chip, YDSXGR_RECCTRLBASE, 0x00000000);
	snd_ymfpci_writel(chip, YDSXGR_EFFCTRLBASE, 0x00000000);
	ctrl = snd_ymfpci_readw(chip, YDSXGR_GLOBALCTRL);
	snd_ymfpci_writew(chip, YDSXGR_GLOBALCTRL, ctrl & ~0x0007);

	/* setup DSP instruction code */
	for (i = 0; i < YDSXG_DSPLENGTH / 4; i++)
		snd_ymfpci_writel(chip, YDSXGR_DSPINSTRAM + (i << 2), DspInst[i]);

	/* setup control instruction code */
	switch (chip->device_id) {
	case PCI_DEVICE_ID_YAMAHA_724F:
	case PCI_DEVICE_ID_YAMAHA_740C:
	case PCI_DEVICE_ID_YAMAHA_744:
	case PCI_DEVICE_ID_YAMAHA_754:
		inst = CntrlInst1E;
		break;
	default:
		inst = CntrlInst;
		break;
	}
	for (i = 0; i < YDSXG_CTRLLENGTH / 4; i++)
		snd_ymfpci_writel(chip, YDSXGR_CTRLINSTRAM + (i << 2), inst[i]);

	snd_ymfpci_enable_dsp(chip);
}

static int __devinit snd_ymfpci_memalloc(ymfpci_t *chip)
{
	long size, playback_ctrl_size;
	int voice, bank, reg;
	u8 *ptr;
	dma_addr_t ptr_addr;

	playback_ctrl_size = 4 + 4 * YDSXG_PLAYBACK_VOICES;
	chip->bank_size_playback = snd_ymfpci_readl(chip, YDSXGR_PLAYCTRLSIZE) << 2;
	chip->bank_size_capture = snd_ymfpci_readl(chip, YDSXGR_RECCTRLSIZE) << 2;
	chip->bank_size_effect = snd_ymfpci_readl(chip, YDSXGR_EFFCTRLSIZE) << 2;
	chip->work_size = YDSXG_DEFAULT_WORK_SIZE;
	
	size = ((playback_ctrl_size + 0x00ff) & ~0x00ff) +
	       ((chip->bank_size_playback * 2 * YDSXG_PLAYBACK_VOICES + 0x00ff) & ~0x00ff) +
	       ((chip->bank_size_capture * 2 * YDSXG_CAPTURE_VOICES + 0x00ff) & ~0x00ff) +
	       ((chip->bank_size_effect * 2 * YDSXG_EFFECT_VOICES + 0x00ff) & ~0x00ff) +
	       chip->work_size;
	/* work_ptr must be aligned to 256 bytes, but it's already
	   covered with the kernel page allocation mechanism */
	if (snd_dma_alloc_pages(&chip->dma_dev, size, &chip->work_ptr) < 0) 
		return -ENOMEM;
	ptr = chip->work_ptr.area;
	ptr_addr = chip->work_ptr.addr;
	memset(ptr, 0, size);	/* for sure */

	chip->bank_base_playback = ptr;
	chip->bank_base_playback_addr = ptr_addr;
	chip->ctrl_playback = (u32 *)ptr;
	chip->ctrl_playback[0] = cpu_to_le32(YDSXG_PLAYBACK_VOICES);
	ptr += (playback_ctrl_size + 0x00ff) & ~0x00ff;
	ptr_addr += (playback_ctrl_size + 0x00ff) & ~0x00ff;
	for (voice = 0; voice < YDSXG_PLAYBACK_VOICES; voice++) {
		chip->voices[voice].number = voice;
		chip->voices[voice].bank = (snd_ymfpci_playback_bank_t *)ptr;
		chip->voices[voice].bank_addr = ptr_addr;
		for (bank = 0; bank < 2; bank++) {
			chip->bank_playback[voice][bank] = (snd_ymfpci_playback_bank_t *)ptr;
			ptr += chip->bank_size_playback;
			ptr_addr += chip->bank_size_playback;
		}
	}
	ptr = (char *)(((unsigned long)ptr + 0x00ff) & ~0x00ff);
	ptr_addr = (ptr_addr + 0x00ff) & ~0x00ff;
	chip->bank_base_capture = ptr;
	chip->bank_base_capture_addr = ptr_addr;
	for (voice = 0; voice < YDSXG_CAPTURE_VOICES; voice++)
		for (bank = 0; bank < 2; bank++) {
			chip->bank_capture[voice][bank] = (snd_ymfpci_capture_bank_t *)ptr;
			ptr += chip->bank_size_capture;
			ptr_addr += chip->bank_size_capture;
		}
	ptr = (char *)(((unsigned long)ptr + 0x00ff) & ~0x00ff);
	ptr_addr = (ptr_addr + 0x00ff) & ~0x00ff;
	chip->bank_base_effect = ptr;
	chip->bank_base_effect_addr = ptr_addr;
	for (voice = 0; voice < YDSXG_EFFECT_VOICES; voice++)
		for (bank = 0; bank < 2; bank++) {
			chip->bank_effect[voice][bank] = (snd_ymfpci_effect_bank_t *)ptr;
			ptr += chip->bank_size_effect;
			ptr_addr += chip->bank_size_effect;
		}
	ptr = (char *)(((unsigned long)ptr + 0x00ff) & ~0x00ff);
	ptr_addr = (ptr_addr + 0x00ff) & ~0x00ff;
	chip->work_base = ptr;
	chip->work_base_addr = ptr_addr;
	
	snd_assert(ptr + chip->work_size == chip->work_ptr.area + chip->work_ptr.bytes, );

	snd_ymfpci_writel(chip, YDSXGR_PLAYCTRLBASE, chip->bank_base_playback_addr);
	snd_ymfpci_writel(chip, YDSXGR_RECCTRLBASE, chip->bank_base_capture_addr);
	snd_ymfpci_writel(chip, YDSXGR_EFFCTRLBASE, chip->bank_base_effect_addr);
	snd_ymfpci_writel(chip, YDSXGR_WORKBASE, chip->work_base_addr);
	snd_ymfpci_writel(chip, YDSXGR_WORKSIZE, chip->work_size >> 2);

	/* S/PDIF output initialization */
	chip->spdif_bits = chip->spdif_pcm_bits = SNDRV_PCM_DEFAULT_CON_SPDIF & 0xffff;
	snd_ymfpci_writew(chip, YDSXGR_SPDIFOUTCTRL, 0);
	snd_ymfpci_writew(chip, YDSXGR_SPDIFOUTSTATUS, chip->spdif_bits);

	/* S/PDIF input initialization */
	snd_ymfpci_writew(chip, YDSXGR_SPDIFINCTRL, 0);

	/* digital mixer setup */
	for (reg = 0x80; reg < 0xc0; reg += 4)
		snd_ymfpci_writel(chip, reg, 0);
	snd_ymfpci_writel(chip, YDSXGR_NATIVEDACOUTVOL, 0x3fff3fff);
	snd_ymfpci_writel(chip, YDSXGR_ZVOUTVOL, 0x3fff3fff);
	snd_ymfpci_writel(chip, YDSXGR_SPDIFOUTVOL, 0x3fff3fff);
	snd_ymfpci_writel(chip, YDSXGR_NATIVEADCINVOL, 0x3fff3fff);
	snd_ymfpci_writel(chip, YDSXGR_NATIVEDACINVOL, 0x3fff3fff);
	snd_ymfpci_writel(chip, YDSXGR_PRIADCLOOPVOL, 0x3fff3fff);
	snd_ymfpci_writel(chip, YDSXGR_LEGACYOUTVOL, 0x3fff3fff);
	
	return 0;
}

static int snd_ymfpci_free(ymfpci_t *chip)
{
	u16 ctrl;

	snd_assert(chip != NULL, return -EINVAL);

	if (chip->res_reg_area) {	/* don't touch busy hardware */
		snd_ymfpci_writel(chip, YDSXGR_NATIVEDACOUTVOL, 0);
		snd_ymfpci_writel(chip, YDSXGR_BUF441OUTVOL, 0);
		snd_ymfpci_writel(chip, YDSXGR_LEGACYOUTVOL, 0);
		snd_ymfpci_writel(chip, YDSXGR_STATUS, ~0);
		snd_ymfpci_disable_dsp(chip);
		snd_ymfpci_writel(chip, YDSXGR_PLAYCTRLBASE, 0);
		snd_ymfpci_writel(chip, YDSXGR_RECCTRLBASE, 0);
		snd_ymfpci_writel(chip, YDSXGR_EFFCTRLBASE, 0);
		snd_ymfpci_writel(chip, YDSXGR_WORKBASE, 0);
		snd_ymfpci_writel(chip, YDSXGR_WORKSIZE, 0);
		ctrl = snd_ymfpci_readw(chip, YDSXGR_GLOBALCTRL);
		snd_ymfpci_writew(chip, YDSXGR_GLOBALCTRL, ctrl & ~0x0007);
	}

	snd_ymfpci_ac3_done(chip);

	/* Set PCI device to D3 state */
#if 0
	/* FIXME: temporarily disabled, otherwise we cannot fire up
	 * the chip again unless reboot.  ACPI bug?
	 */
	pci_set_power_state(chip->pci, 3);
#endif

#ifdef CONFIG_PM
	if (chip->saved_regs)
		vfree(chip->saved_regs);
#endif
	if (chip->mpu_res) {
		release_resource(chip->mpu_res);
		kfree_nocheck(chip->mpu_res);
	}
	if (chip->fm_res) {
		release_resource(chip->fm_res);
		kfree_nocheck(chip->fm_res);
	}
#ifdef SUPPORT_JOYSTICK
	if (chip->joystick_res) {
		if (chip->gameport.io)
			gameport_unregister_port(&chip->gameport);
		release_resource(chip->joystick_res);
		kfree_nocheck(chip->joystick_res);
	}
#endif
	if (chip->reg_area_virt)
		iounmap((void *)chip->reg_area_virt);
	if (chip->work_ptr.area)
		snd_dma_free_pages(&chip->dma_dev, &chip->work_ptr);
	
	if (chip->irq >= 0)
		free_irq(chip->irq, (void *)chip);
	if (chip->res_reg_area) {
		release_resource(chip->res_reg_area);
		kfree_nocheck(chip->res_reg_area);
	}

	pci_write_config_word(chip->pci, 0x40, chip->old_legacy_ctrl);
	
	snd_magic_kfree(chip);
	return 0;
}

static int snd_ymfpci_dev_free(snd_device_t *device)
{
	ymfpci_t *chip = snd_magic_cast(ymfpci_t, device->device_data, return -ENXIO);
	return snd_ymfpci_free(chip);
}

#ifdef CONFIG_PM
static int saved_regs_index[] = {
	/* spdif */
	YDSXGR_SPDIFOUTCTRL,
	YDSXGR_SPDIFOUTSTATUS,
	YDSXGR_SPDIFINCTRL,
	/* volumes */
	YDSXGR_PRIADCLOOPVOL,
	YDSXGR_NATIVEDACINVOL,
	YDSXGR_NATIVEDACOUTVOL,
	// YDSXGR_BUF441OUTVOL,
	YDSXGR_NATIVEADCINVOL,
	YDSXGR_SPDIFLOOPVOL,
	YDSXGR_SPDIFOUTVOL,
	YDSXGR_ZVOUTVOL,
	YDSXGR_LEGACYOUTVOL,
	/* address bases */
	YDSXGR_PLAYCTRLBASE,
	YDSXGR_RECCTRLBASE,
	YDSXGR_EFFCTRLBASE,
	YDSXGR_WORKBASE,
	/* capture set up */
	YDSXGR_MAPOFREC,
	YDSXGR_RECFORMAT,
	YDSXGR_RECSLOTSR,
	YDSXGR_ADCFORMAT,
	YDSXGR_ADCSLOTSR,
};
#define YDSXGR_NUM_SAVED_REGS	ARRAY_SIZE(saved_regs_index)

static int snd_ymfpci_suspend(snd_card_t *card, unsigned int state)
{
	ymfpci_t *chip = snd_magic_cast(ymfpci_t, card->pm_private_data, return -EINVAL);
	unsigned int i;
	
	snd_pcm_suspend_all(chip->pcm);
	snd_pcm_suspend_all(chip->pcm2);
	snd_pcm_suspend_all(chip->pcm_spdif);
	snd_pcm_suspend_all(chip->pcm_4ch);
	snd_ac97_suspend(chip->ac97);
	for (i = 0; i < YDSXGR_NUM_SAVED_REGS; i++)
		chip->saved_regs[i] = snd_ymfpci_readl(chip, saved_regs_index[i]);
	chip->saved_ydsxgr_mode = snd_ymfpci_readl(chip, YDSXGR_MODE);
	snd_ymfpci_writel(chip, YDSXGR_NATIVEDACOUTVOL, 0);
	snd_ymfpci_disable_dsp(chip);
	snd_power_change_state(card, SNDRV_CTL_POWER_D3hot);
	return 0;
}

static int snd_ymfpci_resume(snd_card_t *card, unsigned int state)
{
	ymfpci_t *chip = snd_magic_cast(ymfpci_t, card->pm_private_data, return -EINVAL);
	unsigned int i;

	pci_enable_device(chip->pci);
	pci_set_master(chip->pci);
	snd_ymfpci_aclink_reset(chip->pci);
	snd_ymfpci_codec_ready(chip, 0);
	snd_ymfpci_download_image(chip);
	udelay(100);

	for (i = 0; i < YDSXGR_NUM_SAVED_REGS; i++)
		snd_ymfpci_writel(chip, saved_regs_index[i], chip->saved_regs[i]);

	snd_ac97_resume(chip->ac97);

	/* start hw again */
	if (chip->start_count > 0) {
		unsigned long flags;
		spin_lock_irqsave(&chip->reg_lock, flags);
		snd_ymfpci_writel(chip, YDSXGR_MODE, chip->saved_ydsxgr_mode);
		chip->active_bank = snd_ymfpci_readl(chip, YDSXGR_CTRLSELECT);
		spin_unlock_irqrestore(&chip->reg_lock, flags);
	}
	snd_power_change_state(card, SNDRV_CTL_POWER_D0);
	return 0;
}
#endif /* CONFIG_PM */

int __devinit snd_ymfpci_create(snd_card_t * card,
				struct pci_dev * pci,
				unsigned short old_legacy_ctrl,
				ymfpci_t ** rchip)
{
	ymfpci_t *chip;
	int err;
	static snd_device_ops_t ops = {
		.dev_free =	snd_ymfpci_dev_free,
	};
	
	*rchip = NULL;

	/* enable PCI device */
	if ((err = pci_enable_device(pci)) < 0)
		return err;

	chip = snd_magic_kcalloc(ymfpci_t, 0, GFP_KERNEL);
	if (chip == NULL)
		return -ENOMEM;
	chip->old_legacy_ctrl = old_legacy_ctrl;
	spin_lock_init(&chip->reg_lock);
	spin_lock_init(&chip->voice_lock);
	init_waitqueue_head(&chip->interrupt_sleep);
	atomic_set(&chip->interrupt_sleep_count, 0);
	chip->card = card;
	chip->pci = pci;
	chip->irq = -1;
	chip->device_id = pci->device;
	pci_read_config_byte(pci, PCI_REVISION_ID, (u8 *)&chip->rev);
	chip->reg_area_phys = pci_resource_start(pci, 0);
	chip->reg_area_virt = (unsigned long)ioremap_nocache(chip->reg_area_phys, 0x8000);
	pci_set_master(pci);

	if ((chip->res_reg_area = request_mem_region(chip->reg_area_phys, 0x8000, "YMFPCI")) == NULL) {
		snd_printk("unable to grab memory region 0x%lx-0x%lx\n", chip->reg_area_phys, chip->reg_area_phys + 0x8000 - 1);
		snd_ymfpci_free(chip);
		return -EBUSY;
	}
	if (request_irq(pci->irq, snd_ymfpci_interrupt, SA_INTERRUPT|SA_SHIRQ, "YMFPCI", (void *) chip)) {
		snd_printk("unable to grab IRQ %d\n", pci->irq);
		snd_ymfpci_free(chip);
		return -EBUSY;
	}
	chip->irq = pci->irq;

	memset(&chip->dma_dev, 0, sizeof(chip->dma_dev));
	chip->dma_dev.type = SNDRV_DMA_TYPE_DEV;
	chip->dma_dev.dev = snd_dma_pci_data(pci);

	snd_ymfpci_aclink_reset(pci);
	if (snd_ymfpci_codec_ready(chip, 0) < 0) {
		snd_ymfpci_free(chip);
		return -EIO;
	}

	snd_ymfpci_download_image(chip);

	udelay(100); /* seems we need a delay after downloading image.. */

	if (snd_ymfpci_memalloc(chip) < 0) {
		snd_ymfpci_free(chip);
		return -EIO;
	}

	if ((err = snd_ymfpci_ac3_init(chip)) < 0) {
		snd_ymfpci_free(chip);
		return err;
	}

#ifdef CONFIG_PM
	chip->saved_regs = vmalloc(YDSXGR_NUM_SAVED_REGS * sizeof(u32));
	if (chip->saved_regs == NULL) {
		snd_ymfpci_free(chip);
		return -ENOMEM;
	}
	snd_card_set_pm_callback(card, snd_ymfpci_suspend, snd_ymfpci_resume, chip);
#endif

	snd_ymfpci_proc_init(card, chip);

	if ((err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, chip, &ops)) < 0) {
		snd_ymfpci_free(chip);
		return err;
	}

	snd_card_set_dev(card, &pci->dev);

	*rchip = chip;
	return 0;
}
