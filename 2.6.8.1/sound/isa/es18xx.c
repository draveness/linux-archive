/*
 *  Driver for generic ESS AudioDrive ES18xx soundcards
 *  Copyright (c) by Christian Fischbach <fishbach@pool.informatik.rwth-aachen.de>
 *  Copyright (c) by Abramo Bagnara <abramo@alsa-project.org>
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
/* GENERAL NOTES:
 *
 * BUGS:
 * - There are pops (we can't delay in trigger function, cause midlevel 
 *   often need to trigger down and then up very quickly).
 *   Any ideas?
 * - Support for 16 bit DMA seems to be broken. I've no hardware to tune it.
 */

/*
 * ES1868  NOTES:
 * - The chip has one half duplex pcm (with very limited full duplex support).
 *
 * - Duplex stereophonic sound is impossible.
 * - Record and playback must share the same frequency rate.
 *
 * - The driver use dma2 for playback and dma1 for capture.
 */

/*
 * ES1869 NOTES:
 *
 * - there are a first full duplex pcm and a second playback only pcm
 *   (incompatible with first pcm capture)
 * 
 * - there is support for the capture volume and ESS Spatializer 3D effect.
 *
 * - contrarily to some pages in DS_1869.PDF the rates can be set
 *   independently.
 *
 * BUGS:
 *
 * - There is a major trouble I noted:
 *
 *   using both channel for playback stereo 16 bit samples at 44100 Hz
 *   the second pcm (Audio1) DMA slows down irregularly and sound is garbled.
 *   
 *   The same happens using Audio1 for captureing.
 *
 *   The Windows driver does not suffer of this (although it use Audio1
 *   only for captureing). I'm unable to discover why.
 *
 */


#include <sound/driver.h>
#include <asm/io.h>
#include <asm/dma.h>
#include <linux/init.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/pnp.h>
#include <linux/isapnp.h>
#include <linux/moduleparam.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/mpu401.h>
#include <sound/opl3.h>
#define SNDRV_LEGACY_AUTO_PROBE
#define SNDRV_LEGACY_FIND_FREE_IRQ
#define SNDRV_LEGACY_FIND_FREE_DMA
#include <sound/initval.h>

#define PFX "es18xx: "

struct _snd_es18xx {
	unsigned long port;		/* port of ESS chip */
	unsigned long mpu_port;		/* MPU-401 port of ESS chip */
	unsigned long fm_port;		/* FM port */
	unsigned long ctrl_port;	/* Control port of ESS chip */
	struct resource *res_port;
	struct resource *res_mpu_port;
	struct resource *res_ctrl_port;
	int irq;			/* IRQ number of ESS chip */
	int dma1;			/* DMA1 */
	int dma2;			/* DMA2 */
	unsigned short version;		/* version of ESS chip */
	int caps;			/* Chip capabilities */
	unsigned short audio2_vol;	/* volume level of audio2 */

	unsigned short active;		/* active channel mask */
	unsigned int dma1_size;
	unsigned int dma2_size;
	unsigned int dma1_shift;
	unsigned int dma2_shift;

	snd_card_t *card;
	snd_pcm_t *pcm;
	snd_pcm_substream_t *playback_a_substream;
	snd_pcm_substream_t *capture_a_substream;
	snd_pcm_substream_t *playback_b_substream;

	snd_rawmidi_t *rmidi;

	snd_kcontrol_t *hw_volume;
	snd_kcontrol_t *hw_switch;
	snd_kcontrol_t *master_volume;
	snd_kcontrol_t *master_switch;

	spinlock_t reg_lock;
	spinlock_t mixer_lock;
	spinlock_t ctrl_lock;
#ifdef CONFIG_PM
	unsigned char pm_reg;
#endif
};

#define AUDIO1_IRQ	0x01
#define AUDIO2_IRQ	0x02
#define HWV_IRQ		0x04
#define MPU_IRQ		0x08

#define ES18XX_PCM2	0x0001	/* Has two useable PCM */
#define ES18XX_SPATIALIZER 0x0002	/* Has 3D Spatializer */
#define ES18XX_RECMIX	0x0004	/* Has record mixer */
#define ES18XX_DUPLEX_MONO 0x0008	/* Has mono duplex only */
#define ES18XX_DUPLEX_SAME 0x0010	/* Playback and record must share the same rate */
#define ES18XX_NEW_RATE	0x0020	/* More precise rate setting */
#define ES18XX_AUXB	0x0040	/* AuxB mixer control */
#define ES18XX_HWV	0x0080	/* Has hardware volume */
#define ES18XX_MONO	0x0100	/* Mono_in mixer control */
#define ES18XX_I2S	0x0200	/* I2S mixer control */
#define ES18XX_MUTEREC	0x0400	/* Record source can be muted */
#define ES18XX_CONTROL	0x0800	/* Has control ports */

/* Power Management */
#define ES18XX_PM	0x07
#define ES18XX_PM_GPO0	0x01
#define ES18XX_PM_GPO1	0x02
#define ES18XX_PM_PDR	0x04
#define ES18XX_PM_ANA	0x08
#define ES18XX_PM_FM	0x020
#define ES18XX_PM_SUS	0x080

typedef struct _snd_es18xx es18xx_t;

#define chip_t es18xx_t

/* Lowlevel */

#define DAC1 0x01
#define ADC1 0x02
#define DAC2 0x04
#define MILLISECOND 10000

static int snd_es18xx_dsp_command(es18xx_t *chip, unsigned char val)
{
        int i;

        for(i = MILLISECOND; i; i--)
                if ((inb(chip->port + 0x0C) & 0x80) == 0) {
                        outb(val, chip->port + 0x0C);
                        return 0;
                }
        snd_printk("dsp_command: timeout (0x%x)\n", val);
        return -EINVAL;
}

static int snd_es18xx_dsp_get_byte(es18xx_t *chip)
{
        int i;

        for(i = MILLISECOND/10; i; i--)
                if (inb(chip->port + 0x0C) & 0x40)
                        return inb(chip->port + 0x0A);
        snd_printk("dsp_get_byte failed: 0x%lx = 0x%x!!!\n", chip->port + 0x0A, inb(chip->port + 0x0A));
        return -ENODEV;
}

#undef REG_DEBUG

static int snd_es18xx_write(es18xx_t *chip,
			    unsigned char reg, unsigned char data)
{
	unsigned long flags;
	int ret;
	
        spin_lock_irqsave(&chip->reg_lock, flags);
	ret = snd_es18xx_dsp_command(chip, reg);
	if (ret < 0)
		goto end;
        ret = snd_es18xx_dsp_command(chip, data);
 end:
        spin_unlock_irqrestore(&chip->reg_lock, flags);
#ifdef REG_DEBUG
	snd_printk("Reg %02x set to %02x\n", reg, data);
#endif
	return ret;
}

static int snd_es18xx_read(es18xx_t *chip, unsigned char reg)
{
	unsigned long flags;
	int ret, data;
        spin_lock_irqsave(&chip->reg_lock, flags);
	ret = snd_es18xx_dsp_command(chip, 0xC0);
	if (ret < 0)
		goto end;
        ret = snd_es18xx_dsp_command(chip, reg);
	if (ret < 0)
		goto end;
	data = snd_es18xx_dsp_get_byte(chip);
	ret = data;
#ifdef REG_DEBUG
	snd_printk("Reg %02x now is %02x (%d)\n", reg, data, ret);
#endif
 end:
        spin_unlock_irqrestore(&chip->reg_lock, flags);
	return ret;
}

/* Return old value */
static int snd_es18xx_bits(es18xx_t *chip, unsigned char reg,
			   unsigned char mask, unsigned char val)
{
        int ret;
	unsigned char old, new, oval;
	unsigned long flags;
        spin_lock_irqsave(&chip->reg_lock, flags);
        ret = snd_es18xx_dsp_command(chip, 0xC0);
	if (ret < 0)
		goto end;
        ret = snd_es18xx_dsp_command(chip, reg);
	if (ret < 0)
		goto end;
	ret = snd_es18xx_dsp_get_byte(chip);
	if (ret < 0) {
		goto end;
	}
	old = ret;
	oval = old & mask;
	if (val != oval) {
		ret = snd_es18xx_dsp_command(chip, reg);
		if (ret < 0)
			goto end;
		new = (old & ~mask) | (val & mask);
		ret = snd_es18xx_dsp_command(chip, new);
		if (ret < 0)
			goto end;
#ifdef REG_DEBUG
		snd_printk("Reg %02x was %02x, set to %02x (%d)\n", reg, old, new, ret);
#endif
	}
	ret = oval;
 end:
        spin_unlock_irqrestore(&chip->reg_lock, flags);
	return ret;
}

inline void snd_es18xx_mixer_write(es18xx_t *chip,
			    unsigned char reg, unsigned char data)
{
	unsigned long flags;
        spin_lock_irqsave(&chip->mixer_lock, flags);
        outb(reg, chip->port + 0x04);
        outb(data, chip->port + 0x05);
        spin_unlock_irqrestore(&chip->mixer_lock, flags);
#ifdef REG_DEBUG
	snd_printk("Mixer reg %02x set to %02x\n", reg, data);
#endif
}

inline int snd_es18xx_mixer_read(es18xx_t *chip, unsigned char reg)
{
	unsigned long flags;
	int data;
        spin_lock_irqsave(&chip->mixer_lock, flags);
        outb(reg, chip->port + 0x04);
	data = inb(chip->port + 0x05);
        spin_unlock_irqrestore(&chip->mixer_lock, flags);
#ifdef REG_DEBUG
	snd_printk("Mixer reg %02x now is %02x\n", reg, data);
#endif
        return data;
}

/* Return old value */
static inline int snd_es18xx_mixer_bits(es18xx_t *chip, unsigned char reg,
					unsigned char mask, unsigned char val)
{
	unsigned char old, new, oval;
	unsigned long flags;
        spin_lock_irqsave(&chip->mixer_lock, flags);
        outb(reg, chip->port + 0x04);
	old = inb(chip->port + 0x05);
	oval = old & mask;
	if (val != oval) {
		new = (old & ~mask) | (val & mask);
		outb(new, chip->port + 0x05);
#ifdef REG_DEBUG
		snd_printk("Mixer reg %02x was %02x, set to %02x\n", reg, old, new);
#endif
	}
        spin_unlock_irqrestore(&chip->mixer_lock, flags);
	return oval;
}

static inline int snd_es18xx_mixer_writable(es18xx_t *chip, unsigned char reg,
					    unsigned char mask)
{
	int old, expected, new;
	unsigned long flags;
        spin_lock_irqsave(&chip->mixer_lock, flags);
        outb(reg, chip->port + 0x04);
	old = inb(chip->port + 0x05);
	expected = old ^ mask;
	outb(expected, chip->port + 0x05);
	new = inb(chip->port + 0x05);
        spin_unlock_irqrestore(&chip->mixer_lock, flags);
#ifdef REG_DEBUG
	snd_printk("Mixer reg %02x was %02x, set to %02x, now is %02x\n", reg, old, expected, new);
#endif
	return expected == new;
}


static int snd_es18xx_reset(es18xx_t *chip)
{
	int i;
        outb(0x03, chip->port + 0x06);
        inb(chip->port + 0x06);
        outb(0x00, chip->port + 0x06);
        for(i = 0; i < MILLISECOND && !(inb(chip->port + 0x0E) & 0x80); i++);
        if (inb(chip->port + 0x0A) != 0xAA)
                return -1;
	return 0;
}

static int snd_es18xx_reset_fifo(es18xx_t *chip)
{
        outb(0x02, chip->port + 0x06);
        inb(chip->port + 0x06);
        outb(0x00, chip->port + 0x06);
	return 0;
}

static ratnum_t new_clocks[2] = {
	{
		.num = 793800,
		.den_min = 1,
		.den_max = 128,
		.den_step = 1,
	},
	{
		.num = 768000,
		.den_min = 1,
		.den_max = 128,
		.den_step = 1,
	}
};

static snd_pcm_hw_constraint_ratnums_t new_hw_constraints_clocks = {
	.nrats = 2,
	.rats = new_clocks,
};

static ratnum_t old_clocks[2] = {
	{
		.num = 795444,
		.den_min = 1,
		.den_max = 128,
		.den_step = 1,
	},
	{
		.num = 397722,
		.den_min = 1,
		.den_max = 128,
		.den_step = 1,
	}
};

static snd_pcm_hw_constraint_ratnums_t old_hw_constraints_clocks  = {
	.nrats = 2,
	.rats = old_clocks,
};


static void snd_es18xx_rate_set(es18xx_t *chip, 
				snd_pcm_substream_t *substream,
				int mode)
{
	unsigned int bits, div0;
	snd_pcm_runtime_t *runtime = substream->runtime;
	if (chip->caps & ES18XX_NEW_RATE) {
		if (runtime->rate_num == new_clocks[0].num)
			bits = 128 - runtime->rate_den;
		else
			bits = 256 - runtime->rate_den;
	} else {
		if (runtime->rate_num == old_clocks[0].num)
			bits = 256 - runtime->rate_den;
		else
			bits = 128 - runtime->rate_den;
	}

	/* set filter register */
	div0 = 256 - 7160000*20/(8*82*runtime->rate);
		
	if ((chip->caps & ES18XX_PCM2) && mode == DAC2) {
		snd_es18xx_mixer_write(chip, 0x70, bits);
		snd_es18xx_mixer_write(chip, 0x72, div0);
	} else {
		snd_es18xx_write(chip, 0xA1, bits);
		snd_es18xx_write(chip, 0xA2, div0);
	}
}

static int snd_es18xx_playback_hw_params(snd_pcm_substream_t * substream,
					 snd_pcm_hw_params_t * hw_params)
{
	es18xx_t *chip = snd_pcm_substream_chip(substream);
	int shift, err;

	shift = 0;
	if (params_channels(hw_params) == 2)
		shift++;
	if (snd_pcm_format_width(params_format(hw_params)) == 16)
		shift++;

	if (substream->number == 0 && (chip->caps & ES18XX_PCM2)) {
		if ((chip->caps & ES18XX_DUPLEX_MONO) &&
		    (chip->capture_a_substream) &&
		    params_channels(hw_params) != 1) {
			_snd_pcm_hw_param_setempty(hw_params, SNDRV_PCM_HW_PARAM_CHANNELS);
			return -EBUSY;
		}
		chip->dma2_shift = shift;
	} else {
		chip->dma1_shift = shift;
	}
	if ((err = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params))) < 0)
		return err;
	return 0;
}

static int snd_es18xx_pcm_hw_free(snd_pcm_substream_t * substream)
{
	return snd_pcm_lib_free_pages(substream);
}

static int snd_es18xx_playback1_prepare(es18xx_t *chip,
					snd_pcm_substream_t *substream)
{
	snd_pcm_runtime_t *runtime = substream->runtime;
	unsigned int size = snd_pcm_lib_buffer_bytes(substream);
	unsigned int count = snd_pcm_lib_period_bytes(substream);

	chip->dma2_size = size;

        snd_es18xx_rate_set(chip, substream, DAC2);

        /* Transfer Count Reload */
        count = 0x10000 - count;
        snd_es18xx_mixer_write(chip, 0x74, count & 0xff);
        snd_es18xx_mixer_write(chip, 0x76, count >> 8);

	/* Set format */
        snd_es18xx_mixer_bits(chip, 0x7A, 0x07,
			      ((runtime->channels == 1) ? 0x00 : 0x02) |
			      (snd_pcm_format_width(runtime->format) == 16 ? 0x01 : 0x00) |
			      (snd_pcm_format_unsigned(runtime->format) ? 0x00 : 0x04));

        /* Set DMA controller */
        snd_dma_program(chip->dma2, runtime->dma_addr, size, DMA_MODE_WRITE | DMA_AUTOINIT);

	return 0;
}

static int snd_es18xx_playback1_trigger(es18xx_t *chip,
					snd_pcm_substream_t * substream,
					int cmd)
{
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (chip->active & DAC2)
			return 0;
		chip->active |= DAC2;
                /* Start DMA */
		if (chip->dma2 >= 4)
			snd_es18xx_mixer_write(chip, 0x78, 0xb3);
		else
			snd_es18xx_mixer_write(chip, 0x78, 0x93);
#ifdef AVOID_POPS
		/* Avoid pops */
                udelay(100000);
		if (chip->caps & ES18XX_PCM2)
			/* Restore Audio 2 volume */
			snd_es18xx_mixer_write(chip, 0x7C, chip->audio2_vol);
		else
			/* Enable PCM output */
			snd_es18xx_dsp_command(chip, 0xD1);
#endif
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		if (!(chip->active & DAC2))
			return 0;
		chip->active &= ~DAC2;
                /* Stop DMA */
                snd_es18xx_mixer_write(chip, 0x78, 0x00);
#ifdef AVOID_POPS
                udelay(25000);
		if (chip->caps & ES18XX_PCM2)
			/* Set Audio 2 volume to 0 */
			snd_es18xx_mixer_write(chip, 0x7C, 0);
		else
			/* Disable PCM output */
			snd_es18xx_dsp_command(chip, 0xD3);
#endif
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int snd_es18xx_capture_hw_params(snd_pcm_substream_t * substream,
					snd_pcm_hw_params_t * hw_params)
{
	es18xx_t *chip = snd_pcm_substream_chip(substream);
	int shift, err;

	shift = 0;
	if ((chip->caps & ES18XX_DUPLEX_MONO) &&
	    chip->playback_a_substream &&
	    params_channels(hw_params) != 1) {
		_snd_pcm_hw_param_setempty(hw_params, SNDRV_PCM_HW_PARAM_CHANNELS);
		return -EBUSY;
	}
	if (params_channels(hw_params) == 2)
		shift++;
	if (snd_pcm_format_width(params_format(hw_params)) == 16)
		shift++;
	chip->dma1_shift = shift;
	if ((err = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params))) < 0)
		return err;
	return 0;
}

static int snd_es18xx_capture_prepare(snd_pcm_substream_t *substream)
{
        es18xx_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	unsigned int size = snd_pcm_lib_buffer_bytes(substream);
	unsigned int count = snd_pcm_lib_period_bytes(substream);

	chip->dma1_size = size;

	snd_es18xx_reset_fifo(chip);

        /* Set stereo/mono */
        snd_es18xx_bits(chip, 0xA8, 0x03, runtime->channels == 1 ? 0x02 : 0x01);

        snd_es18xx_rate_set(chip, substream, ADC1);

        /* Transfer Count Reload */
	count = 0x10000 - count;
	snd_es18xx_write(chip, 0xA4, count & 0xff);
	snd_es18xx_write(chip, 0xA5, count >> 8);

#ifdef AVOID_POPS
	udelay(100000);
#endif

        /* Set format */
        snd_es18xx_write(chip, 0xB7, 
                         snd_pcm_format_unsigned(runtime->format) ? 0x51 : 0x71);
        snd_es18xx_write(chip, 0xB7, 0x90 |
                         ((runtime->channels == 1) ? 0x40 : 0x08) |
                         (snd_pcm_format_width(runtime->format) == 16 ? 0x04 : 0x00) |
                         (snd_pcm_format_unsigned(runtime->format) ? 0x00 : 0x20));

        /* Set DMA controler */
        snd_dma_program(chip->dma1, runtime->dma_addr, size, DMA_MODE_READ | DMA_AUTOINIT);

	return 0;
}

static int snd_es18xx_capture_trigger(snd_pcm_substream_t *substream,
				      int cmd)
{
        es18xx_t *chip = snd_pcm_substream_chip(substream);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (chip->active & ADC1)
			return 0;
		chip->active |= ADC1;
                /* Start DMA */
                snd_es18xx_write(chip, 0xB8, 0x0f);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		if (!(chip->active & ADC1))
			return 0;
		chip->active &= ~ADC1;
                /* Stop DMA */
                snd_es18xx_write(chip, 0xB8, 0x00);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int snd_es18xx_playback2_prepare(es18xx_t *chip,
					snd_pcm_substream_t *substream)
{
	snd_pcm_runtime_t *runtime = substream->runtime;
	unsigned int size = snd_pcm_lib_buffer_bytes(substream);
	unsigned int count = snd_pcm_lib_period_bytes(substream);

	chip->dma1_size = size;

	snd_es18xx_reset_fifo(chip);

        /* Set stereo/mono */
        snd_es18xx_bits(chip, 0xA8, 0x03, runtime->channels == 1 ? 0x02 : 0x01);

        snd_es18xx_rate_set(chip, substream, DAC1);

        /* Transfer Count Reload */
	count = 0x10000 - count;
	snd_es18xx_write(chip, 0xA4, count & 0xff);
	snd_es18xx_write(chip, 0xA5, count >> 8);

        /* Set format */
        snd_es18xx_write(chip, 0xB6,
                         snd_pcm_format_unsigned(runtime->format) ? 0x80 : 0x00);
        snd_es18xx_write(chip, 0xB7, 
                         snd_pcm_format_unsigned(runtime->format) ? 0x51 : 0x71);
        snd_es18xx_write(chip, 0xB7, 0x90 |
                         (runtime->channels == 1 ? 0x40 : 0x08) |
                         (snd_pcm_format_width(runtime->format) == 16 ? 0x04 : 0x00) |
                         (snd_pcm_format_unsigned(runtime->format) ? 0x00 : 0x20));

        /* Set DMA controler */
        snd_dma_program(chip->dma1, runtime->dma_addr, size, DMA_MODE_WRITE | DMA_AUTOINIT);

	return 0;
}

static int snd_es18xx_playback2_trigger(es18xx_t *chip,
					snd_pcm_substream_t *substream,
					int cmd)
{
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (chip->active & DAC1)
			return 0;
		chip->active |= DAC1;
                /* Start DMA */
                snd_es18xx_write(chip, 0xB8, 0x05);
#ifdef AVOID_POPS
		/* Avoid pops */
                udelay(100000);
                /* Enable Audio 1 */
                snd_es18xx_dsp_command(chip, 0xD1);
#endif
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		if (!(chip->active & DAC1))
			return 0;
		chip->active &= ~DAC1;
                /* Stop DMA */
                snd_es18xx_write(chip, 0xB8, 0x00);
#ifdef AVOID_POPS
		/* Avoid pops */
                udelay(25000);
                /* Disable Audio 1 */
                snd_es18xx_dsp_command(chip, 0xD3);
#endif
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int snd_es18xx_playback_prepare(snd_pcm_substream_t *substream)
{
        es18xx_t *chip = snd_pcm_substream_chip(substream);
	if (substream->number == 0 && (chip->caps & ES18XX_PCM2))
		return snd_es18xx_playback1_prepare(chip, substream);
	else
		return snd_es18xx_playback2_prepare(chip, substream);
}

static int snd_es18xx_playback_trigger(snd_pcm_substream_t *substream,
				       int cmd)
{
        es18xx_t *chip = snd_pcm_substream_chip(substream);
	if (substream->number == 0 && (chip->caps & ES18XX_PCM2))
		return snd_es18xx_playback1_trigger(chip, substream, cmd);
	else
		return snd_es18xx_playback2_trigger(chip, substream, cmd);
}

static irqreturn_t snd_es18xx_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	es18xx_t *chip = snd_magic_cast(es18xx_t, dev_id, return IRQ_NONE);
	unsigned char status;

	if (chip->caps & ES18XX_CONTROL) {
		/* Read Interrupt status */
		status = inb(chip->ctrl_port + 6);
	} else {
		/* Read Interrupt status */
		status = snd_es18xx_mixer_read(chip, 0x7f) >> 4;
	}
#if 0
	else {
		status = 0;
		if (inb(chip->port + 0x0C) & 0x01)
			status |= AUDIO1_IRQ;
		if (snd_es18xx_mixer_read(chip, 0x7A) & 0x80)
			status |= AUDIO2_IRQ;
		if ((chip->caps & ES18XX_HWV) &&
		    snd_es18xx_mixer_read(chip, 0x64) & 0x10)
			status |= HWV_IRQ;
	}
#endif

	/* Audio 1 & Audio 2 */
        if (status & AUDIO2_IRQ) {
                if (chip->active & DAC2)
                	snd_pcm_period_elapsed(chip->playback_a_substream);
		/* ack interrupt */
                snd_es18xx_mixer_bits(chip, 0x7A, 0x80, 0x00);
        }
        if (status & AUDIO1_IRQ) {
                /* ok.. capture is active */
                if (chip->active & ADC1)
                	snd_pcm_period_elapsed(chip->capture_a_substream);
                /* ok.. playback2 is active */
                else if (chip->active & DAC1)
                	snd_pcm_period_elapsed(chip->playback_b_substream);
		/* ack interrupt */
		inb(chip->port + 0x0E);
        }

	/* MPU */
	if ((status & MPU_IRQ) && chip->rmidi)
		snd_mpu401_uart_interrupt(irq, chip->rmidi->private_data, regs);

	/* Hardware volume */
	if (status & HWV_IRQ) {
		int split = snd_es18xx_mixer_read(chip, 0x64) & 0x80;
		snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->hw_switch->id);
		snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->hw_volume->id);
		if (!split) {
			snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->master_switch->id);
			snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->master_volume->id);
		}
		/* ack interrupt */
		snd_es18xx_mixer_write(chip, 0x66, 0x00);
	}
	return IRQ_HANDLED;
}

static snd_pcm_uframes_t snd_es18xx_playback_pointer(snd_pcm_substream_t * substream)
{
        es18xx_t *chip = snd_pcm_substream_chip(substream);
	int pos;

	if (substream->number == 0 && (chip->caps & ES18XX_PCM2)) {
		if (!(chip->active & DAC2))
			return 0;
		pos = snd_dma_pointer(chip->dma2, chip->dma2_size);
		return pos >> chip->dma2_shift;
	} else {
		if (!(chip->active & DAC1))
			return 0;
		pos = snd_dma_pointer(chip->dma1, chip->dma1_size);
		return pos >> chip->dma1_shift;
	}
}

static snd_pcm_uframes_t snd_es18xx_capture_pointer(snd_pcm_substream_t * substream)
{
        es18xx_t *chip = snd_pcm_substream_chip(substream);
	int pos;

        if (!(chip->active & ADC1))
                return 0;
	pos = snd_dma_pointer(chip->dma1, chip->dma1_size);
	return pos >> chip->dma1_shift;
}

static snd_pcm_hardware_t snd_es18xx_playback =
{
	.info =			(SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_RESUME |
				 SNDRV_PCM_INFO_MMAP_VALID),
	.formats =		(SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S8 | 
				 SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_U16_LE),
	.rates =		SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_48000,
	.rate_min =		4000,
	.rate_max =		48000,
	.channels_min =		1,
	.channels_max =		2,
	.buffer_bytes_max =	65536,
	.period_bytes_min =	64,
	.period_bytes_max =	65536,
	.periods_min =		1,
	.periods_max =		1024,
	.fifo_size =		0,
};

static snd_pcm_hardware_t snd_es18xx_capture =
{
	.info =			(SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_RESUME |
				 SNDRV_PCM_INFO_MMAP_VALID),
	.formats =		(SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S8 | 
				 SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_U16_LE),
	.rates =		SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_48000,
	.rate_min =		4000,
	.rate_max =		48000,
	.channels_min =		1,
	.channels_max =		2,
	.buffer_bytes_max =	65536,
	.period_bytes_min =	64,
	.period_bytes_max =	65536,
	.periods_min =		1,
	.periods_max =		1024,
	.fifo_size =		0,
};

static int snd_es18xx_playback_open(snd_pcm_substream_t * substream)
{
	snd_pcm_runtime_t *runtime = substream->runtime;
        es18xx_t *chip = snd_pcm_substream_chip(substream);

	if (substream->number == 0 && (chip->caps & ES18XX_PCM2)) {
		if ((chip->caps & ES18XX_DUPLEX_MONO) &&
		    chip->capture_a_substream && 
		    chip->capture_a_substream->runtime->channels != 1)
			return -EAGAIN;
		chip->playback_a_substream = substream;
	} else if (substream->number <= 1) {
		if (chip->capture_a_substream)
			return -EAGAIN;
		chip->playback_b_substream = substream;
	} else {
		snd_BUG();
		return -EINVAL;
	}
	substream->runtime->hw = snd_es18xx_playback;
	snd_pcm_hw_constraint_ratnums(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
				      (chip->caps & ES18XX_NEW_RATE) ? &new_hw_constraints_clocks : &old_hw_constraints_clocks);
        return 0;
}

static int snd_es18xx_capture_open(snd_pcm_substream_t * substream)
{
	snd_pcm_runtime_t *runtime = substream->runtime;
        es18xx_t *chip = snd_pcm_substream_chip(substream);

        if (chip->playback_b_substream)
                return -EAGAIN;
	if ((chip->caps & ES18XX_DUPLEX_MONO) &&
	    chip->playback_a_substream &&
	    chip->playback_a_substream->runtime->channels != 1)
		return -EAGAIN;
        chip->capture_a_substream = substream;
	substream->runtime->hw = snd_es18xx_capture;
	snd_pcm_hw_constraint_ratnums(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
				      (chip->caps & ES18XX_NEW_RATE) ? &new_hw_constraints_clocks : &old_hw_constraints_clocks);
        return 0;
}

static int snd_es18xx_playback_close(snd_pcm_substream_t * substream)
{
        es18xx_t *chip = snd_pcm_substream_chip(substream);

	if (substream->number == 0 && (chip->caps & ES18XX_PCM2))
		chip->playback_a_substream = NULL;
	else
		chip->playback_b_substream = NULL;
	
	snd_pcm_lib_free_pages(substream);
	return 0;
}

static int snd_es18xx_capture_close(snd_pcm_substream_t * substream)
{
        es18xx_t *chip = snd_pcm_substream_chip(substream);

        chip->capture_a_substream = NULL;
	snd_pcm_lib_free_pages(substream);
        return 0;
}

/*
 *  MIXER part
 */

static int snd_es18xx_info_mux(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	static char *texts[8] = {
		"Mic", "Mic Master", "CD", "AOUT",
		"Mic1", "Mix", "Line", "Master"
	};

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 8;
	if (uinfo->value.enumerated.item > 7)
		uinfo->value.enumerated.item = 7;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_es18xx_get_mux(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	es18xx_t *chip = snd_kcontrol_chip(kcontrol);
	ucontrol->value.enumerated.item[0] = snd_es18xx_mixer_read(chip, 0x1c) & 0x07;
	return 0;
}

static int snd_es18xx_put_mux(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	es18xx_t *chip = snd_kcontrol_chip(kcontrol);
	unsigned char val = ucontrol->value.enumerated.item[0];
	
	if (val > 7)
		return -EINVAL;
	return snd_es18xx_mixer_bits(chip, 0x1c, 0x07, val) != val;
}

static int snd_es18xx_info_spatializer_enable(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int snd_es18xx_get_spatializer_enable(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	es18xx_t *chip = snd_kcontrol_chip(kcontrol);
	unsigned char val = snd_es18xx_mixer_read(chip, 0x50);
	ucontrol->value.integer.value[0] = !!(val & 8);
	return 0;
}

static int snd_es18xx_put_spatializer_enable(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	es18xx_t *chip = snd_kcontrol_chip(kcontrol);
	unsigned char oval, nval;
	int change;
	nval = ucontrol->value.integer.value[0] ? 0x0c : 0x04;
	oval = snd_es18xx_mixer_read(chip, 0x50) & 0x0c;
	change = nval != oval;
	if (change) {
		snd_es18xx_mixer_write(chip, 0x50, nval & ~0x04);
		snd_es18xx_mixer_write(chip, 0x50, nval);
	}
	return change;
}

static int snd_es18xx_info_hw_volume(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 2;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 63;
	return 0;
}

static int snd_es18xx_get_hw_volume(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	es18xx_t *chip = snd_kcontrol_chip(kcontrol);
	ucontrol->value.integer.value[0] = snd_es18xx_mixer_read(chip, 0x61) & 0x3f;
	ucontrol->value.integer.value[1] = snd_es18xx_mixer_read(chip, 0x63) & 0x3f;
	return 0;
}

static int snd_es18xx_info_hw_switch(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 2;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int snd_es18xx_get_hw_switch(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	es18xx_t *chip = snd_kcontrol_chip(kcontrol);
	ucontrol->value.integer.value[0] = !(snd_es18xx_mixer_read(chip, 0x61) & 0x40);
	ucontrol->value.integer.value[1] = !(snd_es18xx_mixer_read(chip, 0x63) & 0x40);
	return 0;
}

static void snd_es18xx_hwv_free(snd_kcontrol_t *kcontrol)
{
	es18xx_t *chip = snd_magic_cast(es18xx_t, _snd_kcontrol_chip(kcontrol), return);
	chip->master_volume = NULL;
	chip->master_switch = NULL;
	chip->hw_volume = NULL;
	chip->hw_switch = NULL;
}

static int snd_es18xx_reg_bits(es18xx_t *chip, unsigned char reg,
			       unsigned char mask, unsigned char val)
{
	if (reg < 0xa0)
		return snd_es18xx_mixer_bits(chip, reg, mask, val);
	else
		return snd_es18xx_bits(chip, reg, mask, val);
}

static int snd_es18xx_reg_read(es18xx_t *chip, unsigned char reg)
{
	if (reg < 0xa0)
		return snd_es18xx_mixer_read(chip, reg);
	else
		return snd_es18xx_read(chip, reg);
}

#define ES18XX_SINGLE(xname, xindex, reg, shift, mask, invert) \
{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, .index = xindex, \
  .info = snd_es18xx_info_single, \
  .get = snd_es18xx_get_single, .put = snd_es18xx_put_single, \
  .private_value = reg | (shift << 8) | (mask << 16) | (invert << 24) }

static int snd_es18xx_info_single(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	int mask = (kcontrol->private_value >> 16) & 0xff;

	uinfo->type = mask == 1 ? SNDRV_CTL_ELEM_TYPE_BOOLEAN : SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = mask;
	return 0;
}

static int snd_es18xx_get_single(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	es18xx_t *chip = snd_kcontrol_chip(kcontrol);
	int reg = kcontrol->private_value & 0xff;
	int shift = (kcontrol->private_value >> 8) & 0xff;
	int mask = (kcontrol->private_value >> 16) & 0xff;
	int invert = (kcontrol->private_value >> 24) & 0xff;
	int val;
	
	val = snd_es18xx_reg_read(chip, reg);
	ucontrol->value.integer.value[0] = (val >> shift) & mask;
	if (invert)
		ucontrol->value.integer.value[0] = mask - ucontrol->value.integer.value[0];
	return 0;
}

static int snd_es18xx_put_single(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	es18xx_t *chip = snd_kcontrol_chip(kcontrol);
	int reg = kcontrol->private_value & 0xff;
	int shift = (kcontrol->private_value >> 8) & 0xff;
	int mask = (kcontrol->private_value >> 16) & 0xff;
	int invert = (kcontrol->private_value >> 24) & 0xff;
	unsigned char val;
	
	val = (ucontrol->value.integer.value[0] & mask);
	if (invert)
		val = mask - val;
	mask <<= shift;
	val <<= shift;
	return snd_es18xx_reg_bits(chip, reg, mask, val) != val;
}

#define ES18XX_DOUBLE(xname, xindex, left_reg, right_reg, shift_left, shift_right, mask, invert) \
{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, .index = xindex, \
  .info = snd_es18xx_info_double, \
  .get = snd_es18xx_get_double, .put = snd_es18xx_put_double, \
  .private_value = left_reg | (right_reg << 8) | (shift_left << 16) | (shift_right << 19) | (mask << 24) | (invert << 22) }

static int snd_es18xx_info_double(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	int mask = (kcontrol->private_value >> 24) & 0xff;

	uinfo->type = mask == 1 ? SNDRV_CTL_ELEM_TYPE_BOOLEAN : SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 2;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = mask;
	return 0;
}

static int snd_es18xx_get_double(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	es18xx_t *chip = snd_kcontrol_chip(kcontrol);
	int left_reg = kcontrol->private_value & 0xff;
	int right_reg = (kcontrol->private_value >> 8) & 0xff;
	int shift_left = (kcontrol->private_value >> 16) & 0x07;
	int shift_right = (kcontrol->private_value >> 19) & 0x07;
	int mask = (kcontrol->private_value >> 24) & 0xff;
	int invert = (kcontrol->private_value >> 22) & 1;
	unsigned char left, right;
	
	left = snd_es18xx_reg_read(chip, left_reg);
	if (left_reg != right_reg)
		right = snd_es18xx_reg_read(chip, right_reg);
	else
		right = left;
	ucontrol->value.integer.value[0] = (left >> shift_left) & mask;
	ucontrol->value.integer.value[1] = (right >> shift_right) & mask;
	if (invert) {
		ucontrol->value.integer.value[0] = mask - ucontrol->value.integer.value[0];
		ucontrol->value.integer.value[1] = mask - ucontrol->value.integer.value[1];
	}
	return 0;
}

static int snd_es18xx_put_double(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	es18xx_t *chip = snd_kcontrol_chip(kcontrol);
	int left_reg = kcontrol->private_value & 0xff;
	int right_reg = (kcontrol->private_value >> 8) & 0xff;
	int shift_left = (kcontrol->private_value >> 16) & 0x07;
	int shift_right = (kcontrol->private_value >> 19) & 0x07;
	int mask = (kcontrol->private_value >> 24) & 0xff;
	int invert = (kcontrol->private_value >> 22) & 1;
	int change;
	unsigned char val1, val2, mask1, mask2;
	
	val1 = ucontrol->value.integer.value[0] & mask;
	val2 = ucontrol->value.integer.value[1] & mask;
	if (invert) {
		val1 = mask - val1;
		val2 = mask - val2;
	}
	val1 <<= shift_left;
	val2 <<= shift_right;
	mask1 = mask << shift_left;
	mask2 = mask << shift_right;
	if (left_reg != right_reg) {
		change = 0;
		if (snd_es18xx_reg_bits(chip, left_reg, mask1, val1) != val1)
			change = 1;
		if (snd_es18xx_reg_bits(chip, right_reg, mask2, val2) != val2)
			change = 1;
	} else {
		change = (snd_es18xx_reg_bits(chip, left_reg, mask1 | mask2, 
					      val1 | val2) != (val1 | val2));
	}
	return change;
}

static snd_kcontrol_new_t snd_es18xx_base_controls[] = {
ES18XX_DOUBLE("Master Playback Volume", 0, 0x60, 0x62, 0, 0, 63, 0),
ES18XX_DOUBLE("Master Playback Switch", 0, 0x60, 0x62, 6, 6, 1, 1),
ES18XX_DOUBLE("Line Playback Volume", 0, 0x3e, 0x3e, 4, 0, 15, 0),
ES18XX_DOUBLE("CD Playback Volume", 0, 0x38, 0x38, 4, 0, 15, 0),
ES18XX_DOUBLE("FM Playback Volume", 0, 0x36, 0x36, 4, 0, 15, 0),
ES18XX_DOUBLE("Mono Playback Volume", 0, 0x6d, 0x6d, 4, 0, 15, 0),
ES18XX_DOUBLE("Mic Playback Volume", 0, 0x1a, 0x1a, 4, 0, 15, 0),
ES18XX_DOUBLE("Aux Playback Volume", 0, 0x3a, 0x3a, 4, 0, 15, 0),
ES18XX_SINGLE("PC Speaker Playback Volume", 0, 0x3c, 0, 7, 0),
ES18XX_SINGLE("Record Monitor", 0, 0xa8, 3, 1, 0),
ES18XX_DOUBLE("Capture Volume", 0, 0xb4, 0xb4, 4, 0, 15, 0),
ES18XX_SINGLE("Capture Switch", 0, 0x1c, 4, 1, 1),
{
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Capture Source",
	.info = snd_es18xx_info_mux,
	.get = snd_es18xx_get_mux,
	.put = snd_es18xx_put_mux,
}
};

static snd_kcontrol_new_t snd_es18xx_mono_in_control = 
ES18XX_DOUBLE("Mono Input Playback Volume", 0, 0x6d, 0x6d, 4, 0, 15, 0);

static snd_kcontrol_new_t snd_es18xx_recmix_controls[] = {
ES18XX_DOUBLE("PCM Capture Volume", 0, 0x69, 0x69, 4, 0, 15, 0),
ES18XX_DOUBLE("Mic Capture Volume", 0, 0x68, 0x68, 4, 0, 15, 0),
ES18XX_DOUBLE("Line Capture Volume", 0, 0x6e, 0x6e, 4, 0, 15, 0),
ES18XX_DOUBLE("FM Capture Volume", 0, 0x6b, 0x6b, 4, 0, 15, 0),
ES18XX_DOUBLE("Mono Capture Volume", 0, 0x6f, 0x6f, 4, 0, 15, 0),
ES18XX_DOUBLE("CD Capture Volume", 0, 0x6a, 0x6a, 4, 0, 15, 0),
ES18XX_DOUBLE("Aux Capture Volume", 0, 0x6c, 0x6c, 4, 0, 15, 0)
};

static snd_kcontrol_new_t snd_es18xx_pcm1_controls[] = {
ES18XX_DOUBLE("PCM Playback Volume", 0, 0x14, 0x14, 4, 0, 15, 0),
};

static snd_kcontrol_new_t snd_es18xx_pcm2_controls[] = {
ES18XX_DOUBLE("PCM Playback Volume", 0, 0x7c, 0x7c, 4, 0, 15, 0),
ES18XX_DOUBLE("PCM Playback Volume", 1, 0x14, 0x14, 4, 0, 15, 0)
};

static snd_kcontrol_new_t snd_es18xx_spatializer_controls[] = {
ES18XX_SINGLE("3D Control - Level", 0, 0x52, 0, 63, 0),
{
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "3D Control - Switch",
	.info = snd_es18xx_info_spatializer_enable,
	.get = snd_es18xx_get_spatializer_enable,
	.put = snd_es18xx_put_spatializer_enable,
}
};

static snd_kcontrol_new_t snd_es18xx_micpre1_control = 
ES18XX_SINGLE("Mic Boost (+26dB)", 0, 0xa9, 2, 1, 0);

static snd_kcontrol_new_t snd_es18xx_micpre2_control =
ES18XX_SINGLE("Mic Boost (+26dB)", 0, 0x7d, 3, 1, 0);

static snd_kcontrol_new_t snd_es18xx_hw_volume_controls[] = {
{
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Hardware Master Playback Volume",
	.access = SNDRV_CTL_ELEM_ACCESS_READ,
	.info = snd_es18xx_info_hw_volume,
	.get = snd_es18xx_get_hw_volume,
},
{
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Hardware Master Playback Switch",
	.access = SNDRV_CTL_ELEM_ACCESS_READ,
	.info = snd_es18xx_info_hw_switch,
	.get = snd_es18xx_get_hw_switch,
},
ES18XX_SINGLE("Hardware Master Volume Split", 0, 0x64, 7, 1, 0),
};

#if 0
static int __devinit snd_es18xx_config_read(es18xx_t *chip, unsigned char reg)
{
	int data;
	unsigned long flags;
        spin_lock_irqsave(&chip->ctrl_lock, flags);
	outb(reg, chip->ctrl_port);
	data = inb(chip->ctrl_port + 1);
        spin_unlock_irqrestore(&chip->ctrl_lock, flags);
	return data;
}
#endif

static void __devinit snd_es18xx_config_write(es18xx_t *chip, 
					      unsigned char reg, unsigned char data)
{
	/* No need for spinlocks, this function is used only in
	   otherwise protected init code */
	outb(reg, chip->ctrl_port);
	outb(data, chip->ctrl_port + 1);
#ifdef REG_DEBUG
	snd_printk("Config reg %02x set to %02x\n", reg, data);
#endif
}

static int __devinit snd_es18xx_initialize(es18xx_t *chip)
{
	int mask = 0;

        /* enable extended mode */
        snd_es18xx_dsp_command(chip, 0xC6);
	/* Reset mixer registers */
	snd_es18xx_mixer_write(chip, 0x00, 0x00);

        /* Audio 1 DMA demand mode (4 bytes/request) */
        snd_es18xx_write(chip, 0xB9, 2);
	if (chip->caps & ES18XX_CONTROL) {
		/* Hardware volume IRQ */
		snd_es18xx_config_write(chip, 0x27, chip->irq);
		if (chip->fm_port > 0 && chip->fm_port != SNDRV_AUTO_PORT) {
			/* FM I/O */
			snd_es18xx_config_write(chip, 0x62, chip->fm_port >> 8);
			snd_es18xx_config_write(chip, 0x63, chip->fm_port & 0xff);
		}
		if (chip->mpu_port > 0 && chip->mpu_port != SNDRV_AUTO_PORT) {
			/* MPU-401 I/O */
			snd_es18xx_config_write(chip, 0x64, chip->mpu_port >> 8);
			snd_es18xx_config_write(chip, 0x65, chip->mpu_port & 0xff);
			/* MPU-401 IRQ */
			snd_es18xx_config_write(chip, 0x28, chip->irq);
		}
		/* Audio1 IRQ */
		snd_es18xx_config_write(chip, 0x70, chip->irq);
		/* Audio2 IRQ */
		snd_es18xx_config_write(chip, 0x72, chip->irq);
		/* Audio1 DMA */
		snd_es18xx_config_write(chip, 0x74, chip->dma1);
		/* Audio2 DMA */
		snd_es18xx_config_write(chip, 0x75, chip->dma2);

		/* Enable Audio 1 IRQ */
		snd_es18xx_write(chip, 0xB1, 0x50);
		/* Enable Audio 2 IRQ */
		snd_es18xx_mixer_write(chip, 0x7A, 0x40);
		/* Enable Audio 1 DMA */
		snd_es18xx_write(chip, 0xB2, 0x50);
		/* Enable MPU and hardware volume interrupt */
		snd_es18xx_mixer_write(chip, 0x64, 0x42);
	}
	else {
		int irqmask, dma1mask, dma2mask;
		switch (chip->irq) {
		case 2:
		case 9:
			irqmask = 0;
			break;
		case 5:
			irqmask = 1;
			break;
		case 7:
			irqmask = 2;
			break;
		case 10:
			irqmask = 3;
			break;
		default:
			snd_printk("invalid irq %d\n", chip->irq);
			return -ENODEV;
		}
		switch (chip->dma1) {
		case 0:
			dma1mask = 1;
			break;
		case 1:
			dma1mask = 2;
			break;
		case 3:
			dma1mask = 3;
			break;
		default:
			snd_printk("invalid dma1 %d\n", chip->dma1);
			return -ENODEV;
		}
		switch (chip->dma2) {
		case 0:
			dma2mask = 0;
			break;
		case 1:
			dma2mask = 1;
			break;
		case 3:
			dma2mask = 2;
			break;
		case 5:
			dma2mask = 3;
			break;
		default:
			snd_printk("invalid dma2 %d\n", chip->dma2);
			return -ENODEV;
		}

		/* Enable and set Audio 1 IRQ */
		snd_es18xx_write(chip, 0xB1, 0x50 | (irqmask << 2));
		/* Enable and set Audio 1 DMA */
		snd_es18xx_write(chip, 0xB2, 0x50 | (dma1mask << 2));
		/* Set Audio 2 DMA */
		snd_es18xx_mixer_bits(chip, 0x7d, 0x07, 0x04 | dma2mask);
		/* Enable Audio 2 IRQ and DMA
		   Set capture mixer input */
		snd_es18xx_mixer_write(chip, 0x7A, 0x68);
		/* Enable and set hardware volume interrupt */
		snd_es18xx_mixer_write(chip, 0x64, 0x06);
		if (chip->mpu_port > 0 && chip->mpu_port != SNDRV_AUTO_PORT) {
			/* MPU401 share irq with audio
			   Joystick enabled
			   FM enabled */
			snd_es18xx_mixer_write(chip, 0x40, 0x43 | (chip->mpu_port & 0xf0) >> 1);
		}
		snd_es18xx_mixer_write(chip, 0x7f, ((irqmask + 1) << 1) | 0x01);
	}
	if (chip->caps & ES18XX_NEW_RATE) {
		/* Change behaviour of register A1
		   4x oversampling
		   2nd channel DAC asynchronous */
		snd_es18xx_mixer_write(chip, 0x71, 0x32);
	}
	if (!(chip->caps & ES18XX_PCM2)) {
		/* Enable DMA FIFO */
		snd_es18xx_write(chip, 0xB7, 0x80);
	}
	if (chip->caps & ES18XX_SPATIALIZER) {
		/* Set spatializer parameters to recommended values */
		snd_es18xx_mixer_write(chip, 0x54, 0x8f);
		snd_es18xx_mixer_write(chip, 0x56, 0x95);
		snd_es18xx_mixer_write(chip, 0x58, 0x94);
		snd_es18xx_mixer_write(chip, 0x5a, 0x80);
	}
	/* Mute input source */
	if (chip->caps & ES18XX_MUTEREC)
		mask = 0x10;
	if (chip->caps & ES18XX_RECMIX)
		snd_es18xx_mixer_write(chip, 0x1c, 0x05 | mask);
	else {
		snd_es18xx_mixer_write(chip, 0x1c, 0x00 | mask);
		snd_es18xx_write(chip, 0xb4, 0x00);
	}
#ifndef AVOID_POPS
	/* Enable PCM output */
	snd_es18xx_dsp_command(chip, 0xD1);
#endif

        return 0;
}

static int __devinit snd_es18xx_identify(es18xx_t *chip)
{
	int hi,lo;

	/* reset */
	if (snd_es18xx_reset(chip) < 0) {
                snd_printk("reset at 0x%lx failed!!!\n", chip->port);
		return -ENODEV;
	}

	snd_es18xx_dsp_command(chip, 0xe7);
	hi = snd_es18xx_dsp_get_byte(chip);
	if (hi < 0) {
		return hi;
	}
	lo = snd_es18xx_dsp_get_byte(chip);
	if ((lo & 0xf0) != 0x80) {
		return -ENODEV;
	}
	if (hi == 0x48) {
		chip->version = 0x488;
		return 0;
	}
	if (hi != 0x68) {
		return -ENODEV;
	}
	if ((lo & 0x0f) < 8) {
		chip->version = 0x688;
		return 0;
	}
			
        outb(0x40, chip->port + 0x04);
	hi = inb(chip->port + 0x05);
	lo = inb(chip->port + 0x05);
	if (hi != lo) {
		chip->version = hi << 8 | lo;
		chip->ctrl_port = inb(chip->port + 0x05) << 8;
		chip->ctrl_port += inb(chip->port + 0x05);

		if ((chip->res_ctrl_port = request_region(chip->ctrl_port, 8, "ES18xx - CTRL")) == NULL) {
			snd_printk(KERN_ERR PFX "unable go grab port 0x%lx\n", chip->ctrl_port);
			return -EBUSY;
		}

		return 0;
	}

	/* If has Hardware volume */
	if (snd_es18xx_mixer_writable(chip, 0x64, 0x04)) {
		/* If has Audio2 */
		if (snd_es18xx_mixer_writable(chip, 0x70, 0x7f)) {
			/* If has volume count */
			if (snd_es18xx_mixer_writable(chip, 0x64, 0x20)) {
				chip->version = 0x1887;
			} else {
				chip->version = 0x1888;
			}
		} else {
			chip->version = 0x1788;
		}
	}
	else
		chip->version = 0x1688;
	return 0;
}

static int __devinit snd_es18xx_probe(es18xx_t *chip)
{
	if (snd_es18xx_identify(chip) < 0) {
		snd_printk(KERN_ERR PFX "[0x%lx] ESS chip not found\n", chip->port);
                return -ENODEV;
	}

	switch (chip->version) {
	case 0x1868:
		chip->caps = ES18XX_DUPLEX_MONO | ES18XX_DUPLEX_SAME | ES18XX_CONTROL | ES18XX_HWV;
		break;
	case 0x1869:
		chip->caps = ES18XX_PCM2 | ES18XX_SPATIALIZER | ES18XX_RECMIX | ES18XX_NEW_RATE | ES18XX_AUXB | ES18XX_MONO | ES18XX_MUTEREC | ES18XX_CONTROL | ES18XX_HWV;
		break;
	case 0x1878:
		chip->caps = ES18XX_DUPLEX_MONO | ES18XX_DUPLEX_SAME | ES18XX_I2S | ES18XX_CONTROL | ES18XX_HWV;
		break;
	case 0x1879:
		chip->caps = ES18XX_PCM2 | ES18XX_SPATIALIZER | ES18XX_RECMIX | ES18XX_NEW_RATE | ES18XX_AUXB | ES18XX_I2S | ES18XX_CONTROL | ES18XX_HWV;
		break;
	case 0x1887:
		chip->caps = ES18XX_PCM2 | ES18XX_RECMIX | ES18XX_AUXB | ES18XX_DUPLEX_SAME | ES18XX_HWV;
		break;
	case 0x1888:
		chip->caps = ES18XX_PCM2 | ES18XX_RECMIX | ES18XX_AUXB | ES18XX_DUPLEX_SAME | ES18XX_HWV;
		break;
	default:
                snd_printk("[0x%lx] unsupported chip ES%x\n",
                           chip->port, chip->version);
                return -ENODEV;
        }

        snd_printd("[0x%lx] ESS%x chip found\n", chip->port, chip->version);

	if (chip->dma1 == chip->dma2)
		chip->caps &= ~(ES18XX_PCM2 | ES18XX_DUPLEX_SAME);

        return snd_es18xx_initialize(chip);
}

static snd_pcm_ops_t snd_es18xx_playback_ops = {
	.open =		snd_es18xx_playback_open,
	.close =	snd_es18xx_playback_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_es18xx_playback_hw_params,
	.hw_free =	snd_es18xx_pcm_hw_free,
	.prepare =	snd_es18xx_playback_prepare,
	.trigger =	snd_es18xx_playback_trigger,
	.pointer =	snd_es18xx_playback_pointer,
};

static snd_pcm_ops_t snd_es18xx_capture_ops = {
	.open =		snd_es18xx_capture_open,
	.close =	snd_es18xx_capture_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_es18xx_capture_hw_params,
	.hw_free =	snd_es18xx_pcm_hw_free,
	.prepare =	snd_es18xx_capture_prepare,
	.trigger =	snd_es18xx_capture_trigger,
	.pointer =	snd_es18xx_capture_pointer,
};

static void snd_es18xx_pcm_free(snd_pcm_t *pcm)
{
	es18xx_t *codec = snd_magic_cast(es18xx_t, pcm->private_data, return);
	codec->pcm = NULL;
	snd_pcm_lib_preallocate_free_for_all(pcm);
}

int __devinit snd_es18xx_pcm(es18xx_t *chip, int device, snd_pcm_t ** rpcm)
{
        snd_pcm_t *pcm;
	char str[16];
	int err;

	if (rpcm)
		*rpcm = NULL;
	sprintf(str, "ES%x", chip->version);
	if (chip->caps & ES18XX_PCM2) {
		err = snd_pcm_new(chip->card, str, device, 2, 1, &pcm);
	} else {
		err = snd_pcm_new(chip->card, str, device, 1, 1, &pcm);
	}
        if (err < 0)
                return err;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &snd_es18xx_playback_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &snd_es18xx_capture_ops);

	/* global setup */
        pcm->private_data = chip;
	pcm->private_free = snd_es18xx_pcm_free;
        pcm->info_flags = 0;
	if (chip->caps & ES18XX_DUPLEX_SAME)
		pcm->info_flags |= SNDRV_PCM_INFO_JOINT_DUPLEX;
	if (! (chip->caps & ES18XX_PCM2))
		pcm->info_flags |= SNDRV_PCM_INFO_HALF_DUPLEX;
	sprintf(pcm->name, "ESS AudioDrive ES%x", chip->version);
        chip->pcm = pcm;

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV,
					      snd_dma_isa_data(),
					      64*1024,
					      chip->dma1 > 3 || chip->dma2 > 3 ? 128*1024 : 64*1024);

        if (rpcm)
        	*rpcm = pcm;
	return 0;
}

/* Power Management support functions */
#ifdef CONFIG_PM
static int snd_es18xx_suspend(snd_card_t *card, unsigned int state)
{
	es18xx_t *chip = snd_magic_cast(es18xx_t, card->pm_private_data, return -EINVAL);

	snd_pcm_suspend_all(chip->pcm);

	/* power down */
	chip->pm_reg = (unsigned char)snd_es18xx_read(chip, ES18XX_PM);
	chip->pm_reg |= (ES18XX_PM_FM | ES18XX_PM_SUS);
	snd_es18xx_write(chip, ES18XX_PM, chip->pm_reg);
	snd_es18xx_write(chip, ES18XX_PM, chip->pm_reg ^= ES18XX_PM_SUS);

	snd_power_change_state(card, SNDRV_CTL_POWER_D3hot);
	return 0;
}

static int snd_es18xx_resume(snd_card_t *card, unsigned int state)
{
	es18xx_t *chip = snd_magic_cast(es18xx_t, card->pm_private_data, return -EINVAL);

	/* restore PM register, we won't wake till (not 0x07) i/o activity though */
	snd_es18xx_write(chip, ES18XX_PM, chip->pm_reg ^= ES18XX_PM_FM);

	snd_power_change_state(card, SNDRV_CTL_POWER_D0);
	return 0;
}
#endif /* CONFIG_PM */

static int snd_es18xx_free(es18xx_t *chip)
{
	if (chip->res_port) {
		release_resource(chip->res_port);
		kfree_nocheck(chip->res_port);
	}
	if (chip->res_ctrl_port) {
		release_resource(chip->res_ctrl_port);
		kfree_nocheck(chip->res_ctrl_port);
	}
	if (chip->res_mpu_port) {
		release_resource(chip->res_mpu_port);
		kfree_nocheck(chip->res_mpu_port);
	}
	if (chip->irq >= 0)
		free_irq(chip->irq, (void *) chip);
	if (chip->dma1 >= 0) {
		disable_dma(chip->dma1);
		free_dma(chip->dma1);
	}
	if (chip->dma2 >= 0 && chip->dma1 != chip->dma2) {
		disable_dma(chip->dma2);
		free_dma(chip->dma2);
	}
	snd_magic_kfree(chip);
	return 0;
}

static int snd_es18xx_dev_free(snd_device_t *device)
{
	es18xx_t *chip = snd_magic_cast(es18xx_t, device->device_data, return -ENXIO);
	return snd_es18xx_free(chip);
}

static int __devinit snd_es18xx_new_device(snd_card_t * card,
					   unsigned long port,
					   unsigned long mpu_port,
					   unsigned long fm_port,
					   int irq, int dma1, int dma2,
					   es18xx_t ** rchip)
{
        es18xx_t *chip;
	static snd_device_ops_t ops = {
		.dev_free =	snd_es18xx_dev_free,
        };
	int err;

	*rchip = NULL;
        chip = snd_magic_kcalloc(es18xx_t, 0, GFP_KERNEL);
	if (chip == NULL)
		return -ENOMEM;
	spin_lock_init(&chip->reg_lock);
 	spin_lock_init(&chip->mixer_lock);
 	spin_lock_init(&chip->ctrl_lock);
        chip->card = card;
        chip->port = port;
        chip->mpu_port = mpu_port;
        chip->fm_port = fm_port;
        chip->irq = -1;
        chip->dma1 = -1;
        chip->dma2 = -1;
        chip->audio2_vol = 0x00;
	chip->active = 0;

	if ((chip->res_port = request_region(port, 16, "ES18xx")) == NULL) {
		snd_es18xx_free(chip);
		snd_printk(KERN_ERR PFX "unable to grap ports 0x%lx-0x%lx\n", port, port + 16 - 1);
		return -EBUSY;
	}

	if (request_irq(irq, snd_es18xx_interrupt, SA_INTERRUPT, "ES18xx", (void *) chip)) {
		snd_es18xx_free(chip);
		snd_printk(KERN_ERR PFX "unable to grap IRQ %d\n", irq);
		return -EBUSY;
	}
	chip->irq = irq;

	if (request_dma(dma1, "ES18xx DMA 1")) {
		snd_es18xx_free(chip);
		snd_printk(KERN_ERR PFX "unable to grap DMA1 %d\n", dma1);
		return -EBUSY;
	}
	chip->dma1 = dma1;

	if (dma2 != dma1 && request_dma(dma2, "ES18xx DMA 2")) {
		snd_es18xx_free(chip);
		snd_printk(KERN_ERR PFX "unable to grap DMA2 %d\n", dma2);
		return -EBUSY;
	}
	chip->dma2 = dma2;

        if (snd_es18xx_probe(chip) < 0) {
                snd_es18xx_free(chip);
                return -ENODEV;
        }
	if ((err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, chip, &ops)) < 0) {
		snd_es18xx_free(chip);
		return err;
	}
        *rchip = chip;
        return 0;
}

static int __devinit snd_es18xx_mixer(es18xx_t *chip)
{
	snd_card_t *card;
	int err;
	unsigned int idx;

	card = chip->card;

	strcpy(card->mixername, chip->pcm->name);

	for (idx = 0; idx < ARRAY_SIZE(snd_es18xx_base_controls); idx++) {
		snd_kcontrol_t *kctl;
		kctl = snd_ctl_new1(&snd_es18xx_base_controls[idx], chip);
		if (chip->caps & ES18XX_HWV) {
			switch (idx) {
			case 0:
				chip->master_volume = kctl;
				kctl->private_free = snd_es18xx_hwv_free;
				break;
			case 1:
				chip->master_switch = kctl;
				kctl->private_free = snd_es18xx_hwv_free;
				break;
			}
		}
		if ((err = snd_ctl_add(card, kctl)) < 0)
			return err;
	}
	if (chip->caps & ES18XX_PCM2) {
		for (idx = 0; idx < ARRAY_SIZE(snd_es18xx_pcm2_controls); idx++) {
			if ((err = snd_ctl_add(card, snd_ctl_new1(&snd_es18xx_pcm2_controls[idx], chip))) < 0)
				return err;
		} 
	} else {
		for (idx = 0; idx < ARRAY_SIZE(snd_es18xx_pcm1_controls); idx++) {
			if ((err = snd_ctl_add(card, snd_ctl_new1(&snd_es18xx_pcm1_controls[idx], chip))) < 0)
				return err;
		}
	}

	if (chip->caps & ES18XX_MONO) {
		if ((err = snd_ctl_add(card, snd_ctl_new1(&snd_es18xx_mono_in_control, chip))) < 0)
			return err;
	}
	if (chip->caps & ES18XX_RECMIX) {
		for (idx = 0; idx < ARRAY_SIZE(snd_es18xx_recmix_controls); idx++) {
			if ((err = snd_ctl_add(card, snd_ctl_new1(&snd_es18xx_recmix_controls[idx], chip))) < 0)
				return err;
		}
	}
	switch (chip->version) {
	default:
		if ((err = snd_ctl_add(card, snd_ctl_new1(&snd_es18xx_micpre1_control, chip))) < 0)
			return err;
		break;
	case 0x1869:
	case 0x1879:
		if ((err = snd_ctl_add(card, snd_ctl_new1(&snd_es18xx_micpre2_control, chip))) < 0)
			return err;
		break;
	}
	if (chip->caps & ES18XX_SPATIALIZER) {
		for (idx = 0; idx < ARRAY_SIZE(snd_es18xx_spatializer_controls); idx++) {
			if ((err = snd_ctl_add(card, snd_ctl_new1(&snd_es18xx_spatializer_controls[idx], chip))) < 0)
				return err;
		}
	}
	if (chip->caps & ES18XX_HWV) {
		for (idx = 0; idx < ARRAY_SIZE(snd_es18xx_hw_volume_controls); idx++) {
			snd_kcontrol_t *kctl;
			kctl = snd_ctl_new1(&snd_es18xx_hw_volume_controls[idx], chip);
			if (idx == 0)
				chip->hw_volume = kctl;
			else
				chip->hw_switch = kctl;
			kctl->private_free = snd_es18xx_hwv_free;
			if ((err = snd_ctl_add(card, kctl)) < 0)
				return err;
			
		}
	}
	return 0;
}
       

/* Card level */

MODULE_AUTHOR("Christian Fischbach <fishbach@pool.informatik.rwth-aachen.de>, Abramo Bagnara <abramo@alsa-project.org>");  
MODULE_DESCRIPTION("ESS ES18xx AudioDrive");
MODULE_LICENSE("GPL");
MODULE_CLASSES("{sound}");
MODULE_DEVICES("{{ESS,ES1868 PnP AudioDrive},"
		"{ESS,ES1869 PnP AudioDrive},"
		"{ESS,ES1878 PnP AudioDrive},"
		"{ESS,ES1879 PnP AudioDrive},"
		"{ESS,ES1887 PnP AudioDrive},"
		"{ESS,ES1888 PnP AudioDrive},"
		"{ESS,ES1887 AudioDrive},"
		"{ESS,ES1888 AudioDrive}}");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;	/* Index 0-MAX */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;	/* ID for this card */
static int enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_ISAPNP; /* Enable this card */
#ifdef CONFIG_PNP
static int isapnp[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = 1};
#endif
static long port[SNDRV_CARDS] = SNDRV_DEFAULT_PORT;	/* 0x220,0x240,0x260,0x280 */
#ifndef CONFIG_PNP_
static long mpu_port[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = -1};
#else
static long mpu_port[SNDRV_CARDS] = SNDRV_DEFAULT_PORT;
#endif
static long fm_port[SNDRV_CARDS] = SNDRV_DEFAULT_PORT;
static int irq[SNDRV_CARDS] = SNDRV_DEFAULT_IRQ;	/* 5,7,9,10 */
static int dma1[SNDRV_CARDS] = SNDRV_DEFAULT_DMA;	/* 0,1,3 */
static int dma2[SNDRV_CARDS] = SNDRV_DEFAULT_DMA;	/* 0,1,3 */
static int boot_devs;

module_param_array(index, int, boot_devs, 0444);
MODULE_PARM_DESC(index, "Index value for ES18xx soundcard.");
MODULE_PARM_SYNTAX(index, SNDRV_INDEX_DESC);
module_param_array(id, charp, boot_devs, 0444);
MODULE_PARM_DESC(id, "ID string for ES18xx soundcard.");
MODULE_PARM_SYNTAX(id, SNDRV_ID_DESC);
module_param_array(enable, bool, boot_devs, 0444);
MODULE_PARM_DESC(enable, "Enable ES18xx soundcard.");
MODULE_PARM_SYNTAX(enable, SNDRV_ENABLE_DESC);
#ifdef CONFIG_PNP
module_param_array(isapnp, bool, boot_devs, 0444);
MODULE_PARM_DESC(isapnp, "PnP detection for specified soundcard.");
MODULE_PARM_SYNTAX(isapnp, SNDRV_ISAPNP_DESC);
#endif
module_param_array(port, long, boot_devs, 0444);
MODULE_PARM_DESC(port, "Port # for ES18xx driver.");
MODULE_PARM_SYNTAX(port, SNDRV_ENABLED ",allows:{{0x220,0x280,0x20}},prefers:{0x220},base:16,dialog:list");
module_param_array(mpu_port, long, boot_devs, 0444);
MODULE_PARM_DESC(mpu_port, "MPU-401 port # for ES18xx driver.");
MODULE_PARM_SYNTAX(mpu_port, SNDRV_ENABLED ",allows:{{0x300,0x330,0x30},{0x800,0xffe,0x2}},prefers:{0x330,0x300},base:16,dialog:combo");
module_param_array(fm_port, long, boot_devs, 0444);
MODULE_PARM_DESC(fm_port, "FM port # for ES18xx driver.");
MODULE_PARM_SYNTAX(fm_port, SNDRV_ENABLED ",allows:{{0x388},{0x800,0xffc,0x4}},prefers:{0x388},base:16,dialog:combo");
module_param_array(irq, int, boot_devs, 0444);
MODULE_PARM_DESC(irq, "IRQ # for ES18xx driver.");
MODULE_PARM_SYNTAX(irq, SNDRV_IRQ_DESC ",prefers:{5}");
module_param_array(dma1, int, boot_devs, 0444);
MODULE_PARM_DESC(dma1, "DMA 1 # for ES18xx driver.");
MODULE_PARM_SYNTAX(dma1, SNDRV_DMA8_DESC ",prefers:{1}");
module_param_array(dma2, int, boot_devs, 0444);
MODULE_PARM_DESC(dma2, "DMA 2 # for ES18xx driver.");
MODULE_PARM_SYNTAX(dma2, SNDRV_ENABLED ",allows:{{0},{1},{3},{5}},dialog:list,prefers:{0}");

struct snd_audiodrive {
#ifdef CONFIG_PNP
	struct pnp_dev *dev;
	struct pnp_dev *devc;
#endif
};

static snd_card_t *snd_audiodrive_legacy[SNDRV_CARDS] = SNDRV_DEFAULT_PTR;

#ifdef CONFIG_PNP

static struct pnp_card_device_id snd_audiodrive_pnpids[] = {
	/* ESS 1868 (integrated on Compaq dual P-Pro motherboard and Genius 18PnP 3D) */
	{ .id = "ESS1868", .devs = { { "ESS1868" }, { "ESS0000" } } },
	/* ESS 1868 (integrated on Maxisound Cards) */
	{ .id = "ESS1868", .devs = { { "ESS8601" }, { "ESS8600" } } },
	/* ESS 1868 (integrated on Maxisound Cards) */
	{ .id = "ESS1868", .devs = { { "ESS8611" }, { "ESS8610" } } },
	/* ESS ES1869 Plug and Play AudioDrive */
	{ .id = "ESS0003", .devs = { { "ESS1869" }, { "ESS0006" } } },
	/* ESS 1869 */
	{ .id = "ESS1869", .devs = { { "ESS1869" }, { "ESS0006" } } },
	/* ESS 1878 */
	{ .id = "ESS1878", .devs = { { "ESS1878" }, { "ESS0004" } } },
	/* ESS 1879 */
	{ .id = "ESS1879", .devs = { { "ESS1879" }, { "ESS0009" } } },
	/* --- */
	{ .id = "" } /* end */
};

MODULE_DEVICE_TABLE(pnp_card, snd_audiodrive_pnpids);

static int __devinit snd_audiodrive_pnp(int dev, struct snd_audiodrive *acard,
					struct pnp_card_link *card,
					const struct pnp_card_device_id *id)
{
	struct pnp_dev *pdev;
	struct pnp_resource_table * cfg = kmalloc(sizeof(struct pnp_resource_table), GFP_KERNEL);
	int err;

	if (!cfg)
		return -ENOMEM;
	acard->dev = pnp_request_card_device(card, id->devs[0].id, NULL);
	if (acard->dev == NULL) {
		kfree(cfg);
		return -EBUSY;
	}
	acard->devc = pnp_request_card_device(card, id->devs[1].id, NULL);
	if (acard->devc == NULL) {
		kfree(cfg);
		return -EBUSY;
	}
	/* Control port initialization */
	err = pnp_activate_dev(acard->devc);
	if (err < 0) {
		snd_printk(KERN_ERR PFX "PnP control configure failure (out of resources?)\n");
		return -EAGAIN;
	}
	snd_printdd("pnp: port=0x%lx\n", pnp_port_start(acard->devc, 0));
	/* PnP initialization */
	pdev = acard->dev;
	pnp_init_resource_table(cfg);
	if (port[dev] != SNDRV_AUTO_PORT)
		pnp_resource_change(&cfg->port_resource[0], port[dev], 16);
	if (fm_port[dev] != SNDRV_AUTO_PORT)
		pnp_resource_change(&cfg->port_resource[1], fm_port[dev], 4);
	if (mpu_port[dev] != SNDRV_AUTO_PORT)
		pnp_resource_change(&cfg->port_resource[2], mpu_port[dev], 2);
	if (dma1[dev] != SNDRV_AUTO_DMA)
		pnp_resource_change(&cfg->dma_resource[0], dma1[dev], 1);
	if (dma2[dev] != SNDRV_AUTO_DMA)
		pnp_resource_change(&cfg->dma_resource[1], dma2[dev], 1);
	if (irq[dev] != SNDRV_AUTO_IRQ)
		pnp_resource_change(&cfg->irq_resource[0], irq[dev], 1);
	err = pnp_manual_config_dev(pdev, cfg, 0);
	if (err < 0)
		snd_printk(KERN_ERR PFX "PnP manual resources are invalid, using auto config\n");
	err = pnp_activate_dev(pdev);
	if (err < 0) {
		snd_printk(KERN_ERR PFX "PnP configure failure (out of resources?)\n");
		kfree(cfg);
		return -EBUSY;
	}
	/* ok. hack using Vendor-Defined Card-Level registers */
	/* skip csn and logdev initialization - already done in isapnp_configure */
	if (pnp_device_is_isapnp(pdev)) {
		isapnp_cfg_begin(isapnp_card_number(pdev), isapnp_csn_number(pdev));
		isapnp_write_byte(0x27, pnp_irq(pdev, 0));	/* Hardware Volume IRQ Number */
		if (mpu_port[dev] != SNDRV_AUTO_PORT)
			isapnp_write_byte(0x28, pnp_irq(pdev, 0)); /* MPU-401 IRQ Number */
		isapnp_write_byte(0x72, pnp_irq(pdev, 0));	/* second IRQ */
		isapnp_cfg_end();
	} else {
		snd_printk(KERN_ERR PFX "unable to install ISA PnP hack, expect malfunction\n");
	}
	port[dev] = pnp_port_start(pdev, 0);
	fm_port[dev] = pnp_port_start(pdev, 1);
	mpu_port[dev] = pnp_port_start(pdev, 2);
	dma1[dev] = pnp_dma(pdev, 0);
	dma2[dev] = pnp_dma(pdev, 1);
	irq[dev] = pnp_irq(pdev, 0);
	snd_printdd("PnP ES18xx: port=0x%lx, fm port=0x%lx, mpu port=0x%lx\n", port[dev], fm_port[dev], mpu_port[dev]);
	snd_printdd("PnP ES18xx: dma1=%i, dma2=%i, irq=%i\n", dma1[dev], dma2[dev], irq[dev]);
	kfree(cfg);
	return 0;
}
#endif /* CONFIG_PNP_ */

static int __devinit snd_audiodrive_probe(int dev, struct pnp_card_link *pcard,
					  const struct pnp_card_device_id *pid)
{
	static int possible_irqs[] = {5, 9, 10, 7, 11, 12, -1};
	static int possible_dmas[] = {1, 0, 3, 5, -1};
	int xirq, xdma1, xdma2;
	snd_card_t *card;
	struct snd_audiodrive *acard;
	snd_rawmidi_t *rmidi = NULL;
	es18xx_t *chip;
	opl3_t *opl3;
	int err;

	card = snd_card_new(index[dev], id[dev], THIS_MODULE,
			    sizeof(struct snd_audiodrive));
	if (card == NULL)
		return -ENOMEM;
	acard = (struct snd_audiodrive *)card->private_data;
#ifdef CONFIG_PNP
	if (isapnp[dev]) {
		if ((err = snd_audiodrive_pnp(dev, acard, pcard, pid)) < 0) {
			snd_card_free(card);
			return err;
		}
		snd_card_set_dev(card, &pcard->card->dev);
	}
#endif

	xirq = irq[dev];
	if (xirq == SNDRV_AUTO_IRQ) {
		if ((xirq = snd_legacy_find_free_irq(possible_irqs)) < 0) {
			snd_card_free(card);
			snd_printk("unable to find a free IRQ\n");
			return -EBUSY;
		}
	}
	xdma1 = dma1[dev];
        if (xdma1 == SNDRV_AUTO_DMA) {
                if ((xdma1 = snd_legacy_find_free_dma(possible_dmas)) < 0) {
                        snd_card_free(card);
                        snd_printk("unable to find a free DMA1\n");
                        return -EBUSY;
                }
        }
	xdma2 = dma2[dev];
        if (xdma2 == SNDRV_AUTO_DMA) {
                if ((xdma2 = snd_legacy_find_free_dma(possible_dmas)) < 0) {
                        snd_card_free(card);
                        snd_printk("unable to find a free DMA2\n");
                        return -EBUSY;
                }
        }

	if ((err = snd_es18xx_new_device(card,
					 port[dev],
					 mpu_port[dev],
					 fm_port[dev],
					 xirq, xdma1, xdma2,
					 &chip)) < 0) {
		snd_card_free(card);
		return err;
	}

	sprintf(card->driver, "ES%x", chip->version);
	sprintf(card->shortname, "ESS AudioDrive ES%x", chip->version);
	if (xdma1 != xdma2)
		sprintf(card->longname, "%s at 0x%lx, irq %d, dma1 %d, dma2 %d",
			card->shortname,
			chip->port,
			xirq, xdma1, xdma2);
	else
		sprintf(card->longname, "%s at 0x%lx, irq %d, dma %d",
			card->shortname,
			chip->port,
			xirq, xdma1);

	if ((err = snd_es18xx_pcm(chip, 0, NULL)) < 0) {
		snd_card_free(card);
		return err;
	}
	if ((err = snd_es18xx_mixer(chip)) < 0) {
		snd_card_free(card);
		return err;
	}

	if (fm_port[dev] > 0 && fm_port[dev] != SNDRV_AUTO_PORT) {
		if (snd_opl3_create(card, chip->fm_port, chip->fm_port + 2, OPL3_HW_OPL3, 0, &opl3) < 0) {
			snd_printk(KERN_ERR PFX "opl3 not detected at 0x%lx\n", chip->fm_port);
		} else {
			if ((err = snd_opl3_hwdep_new(opl3, 0, 1, NULL)) < 0) {
				snd_card_free(card);
				return err;
			}
		}
	}

	if (mpu_port[dev] > 0 && mpu_port[dev] != SNDRV_AUTO_PORT) {
		if ((err = snd_mpu401_uart_new(card, 0, MPU401_HW_ES18XX,
					       chip->mpu_port, 0,
					       xirq, 0,
					       &rmidi)) < 0) {
			snd_card_free(card);
			return err;
		}
		chip->rmidi = rmidi;
	}

	/* Power Management */
	snd_card_set_isa_pm_callback(card, snd_es18xx_suspend, snd_es18xx_resume, chip);

	if ((err = snd_card_register(card)) < 0) {
		snd_card_free(card);
		return err;
	}
	if (pcard)
		pnp_set_card_drvdata(pcard, card);
	else
		snd_audiodrive_legacy[dev] = card;
	return 0;
}

static int __devinit snd_audiodrive_probe_legacy_port(unsigned long xport)
{
	static int dev;
	int res;

	for ( ; dev < SNDRV_CARDS; dev++) {
		if (!enable[dev] || port[dev] != SNDRV_AUTO_PORT)
			continue;
#ifdef CONFIG_PNP
		if (isapnp[dev])
			continue;
#endif
		port[dev] = xport;
		res = snd_audiodrive_probe(dev, NULL, NULL);
		if (res < 0)
			port[dev] = SNDRV_AUTO_PORT;
		return res;
	}
	return -ENODEV;
}


#ifdef CONFIG_PNP
static int __devinit snd_audiodrive_pnp_detect(struct pnp_card_link *card,
					       const struct pnp_card_device_id *id)
{
	static int dev;
	int res;

	for ( ; dev < SNDRV_CARDS; dev++) {
		if (!enable[dev] || !isapnp[dev])
			continue;
                res = snd_audiodrive_probe(dev, card, id);
		if (res < 0)
			return res;
		dev++;
		return 0;
        }

        return -ENODEV;
}

static void __devexit snd_audiodrive_pnp_remove(struct pnp_card_link * pcard)
{
	snd_card_t *card = (snd_card_t *) pnp_get_card_drvdata(pcard);

	snd_card_disconnect(card);
	snd_card_free_in_thread(card);
}

static struct pnp_card_driver es18xx_pnpc_driver = {
	.flags = PNP_DRIVER_RES_DISABLE,
	.name = "es18xx",
	.id_table = snd_audiodrive_pnpids,
	.probe = snd_audiodrive_pnp_detect,
	.remove = __devexit_p(snd_audiodrive_pnp_remove),
};
#endif /* CONFIG_PNP */

static int __init alsa_card_es18xx_init(void)
{
	static unsigned long possible_ports[] = {0x220, 0x240, 0x260, 0x280, -1};
	int dev, cards = 0, i;

	/* legacy non-auto cards at first */
	for (dev = 0; dev < SNDRV_CARDS; dev++) {
		if (!enable[dev] || port[dev] == SNDRV_AUTO_PORT)
			continue;
#ifdef CONFIG_PNP
		if (isapnp[dev])
			continue;
#endif
		if (snd_audiodrive_probe(dev, NULL, NULL) >= 0)
			cards++;
	}
	/* legacy auto configured cards */
	i = snd_legacy_auto_probe(possible_ports, snd_audiodrive_probe_legacy_port);
	if (i > 0)
		cards += i;

#ifdef CONFIG_PNP
	/* ISA PnP cards at last */
	i = pnp_register_card_driver(&es18xx_pnpc_driver);
	if (i > 0)
		cards += i;

#endif
	if(!cards) {
#ifdef CONFIG_PNP
		pnp_unregister_card_driver(&es18xx_pnpc_driver);
#endif
#ifdef MODULE
		snd_printk(KERN_ERR "ESS AudioDrive ES18xx soundcard not found or device busy\n");
#endif
		return -ENODEV;
	}
	return 0;
}

static void __exit alsa_card_es18xx_exit(void)
{
	int idx;

#ifdef CONFIG_PNP
	/* PnP cards first */
	pnp_unregister_card_driver(&es18xx_pnpc_driver);
#endif
	for(idx = 0; idx < SNDRV_CARDS; idx++)
		snd_card_free(snd_audiodrive_legacy[idx]);
}

module_init(alsa_card_es18xx_init)
module_exit(alsa_card_es18xx_exit)
