/*********************************************************************
 *
 * Turtle Beach MultiSound Sound Card Driver for Linux
 * Linux 2.0/2.2 Version
 *
 * msnd_pinnacle.c / msnd_classic.c
 *
 * -- If MSND_CLASSIC is defined:
 *
 *     -> driver for Turtle Beach Classic/Monterey/Tahiti
 *
 * -- Else
 *
 *     -> driver for Turtle Beach Pinnacle/Fiji
 *
 * Copyright (C) 1998 Andrew Veliath
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * $Id: msnd_pinnacle.c,v 1.75 1999/03/21 16:50:09 andrewtv Exp $
 *
 ********************************************************************/

#include <linux/kernel.h>
#include <linux/config.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/malloc.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/smp_lock.h>
#include <asm/irq.h>
#include <asm/io.h>
#include "sound_config.h"
#include "sound_firmware.h"
#ifdef MSND_CLASSIC
#  define SLOWIO
#endif
#include "msnd.h"
#ifdef MSND_CLASSIC
#  ifdef CONFIG_MSNDCLAS_HAVE_BOOT
#    define HAVE_DSPCODEH
#  endif
#  include "msnd_classic.h"
#  define LOGNAME			"msnd_classic"
#else
#  ifdef CONFIG_MSNDPIN_HAVE_BOOT
#    define HAVE_DSPCODEH
#  endif
#  include "msnd_pinnacle.h"
#  define LOGNAME			"msnd_pinnacle"
#endif

#ifndef CONFIG_MSND_WRITE_NDELAY
#  define CONFIG_MSND_WRITE_NDELAY	1
#endif

#define get_play_delay_jiffies(size)	((size) * HZ *			\
					 dev.play_sample_size / 8 /	\
					 dev.play_sample_rate /		\
					 dev.play_channels)

#define get_rec_delay_jiffies(size)	((size) * HZ *			\
					 dev.rec_sample_size / 8 /	\
					 dev.rec_sample_rate /		\
					 dev.rec_channels)

static multisound_dev_t			dev;

#ifndef HAVE_DSPCODEH
static char				*dspini, *permini;
static int				sizeof_dspini, sizeof_permini;
#endif

static int				dsp_full_reset(void);
static void				dsp_write_flush(void);

static __inline__ int chk_send_dsp_cmd(multisound_dev_t *dev, register BYTE cmd)
{
	if (msnd_send_dsp_cmd(dev, cmd) == 0)
		return 0;
	dsp_full_reset();
	return msnd_send_dsp_cmd(dev, cmd);
}

static void reset_play_queue(void)
{
	int n;
	LPDAQD lpDAQ;

	dev.last_playbank = -1;
	isa_writew(PCTODSP_OFFSET(0 * DAQDS__size), dev.DAPQ + JQS_wHead);
	isa_writew(PCTODSP_OFFSET(0 * DAQDS__size), dev.DAPQ + JQS_wTail);

	for (n = 0, lpDAQ = dev.base + DAPQ_DATA_BUFF; n < 3; ++n, lpDAQ += DAQDS__size) {
		isa_writew(PCTODSP_BASED((DWORD)(DAP_BUFF_SIZE * n)), lpDAQ + DAQDS_wStart);
		isa_writew(0, lpDAQ + DAQDS_wSize);
		isa_writew(1, lpDAQ + DAQDS_wFormat);
		isa_writew(dev.play_sample_size, lpDAQ + DAQDS_wSampleSize);
		isa_writew(dev.play_channels, lpDAQ + DAQDS_wChannels);
		isa_writew(dev.play_sample_rate, lpDAQ + DAQDS_wSampleRate);
		isa_writew(HIMT_PLAY_DONE * 0x100 + n, lpDAQ + DAQDS_wIntMsg);
		isa_writew(n, lpDAQ + DAQDS_wFlags);
	}
}

static void reset_record_queue(void)
{
	int n;
	LPDAQD lpDAQ;
	unsigned long flags;

	dev.last_recbank = 2;
	isa_writew(PCTODSP_OFFSET(0 * DAQDS__size), dev.DARQ + JQS_wHead);
	isa_writew(PCTODSP_OFFSET(dev.last_recbank * DAQDS__size), dev.DARQ + JQS_wTail);

	/* Critical section: bank 1 access */
	spin_lock_irqsave(&dev.lock, flags);
	outb(HPBLKSEL_1, dev.io + HP_BLKS);
	isa_memset_io(dev.base, 0, DAR_BUFF_SIZE * 3);
	outb(HPBLKSEL_0, dev.io + HP_BLKS);
	spin_unlock_irqrestore(&dev.lock, flags);

	for (n = 0, lpDAQ = dev.base + DARQ_DATA_BUFF; n < 3; ++n, lpDAQ += DAQDS__size) {
		isa_writew(PCTODSP_BASED((DWORD)(DAR_BUFF_SIZE * n)) + 0x4000, lpDAQ + DAQDS_wStart);
		isa_writew(DAR_BUFF_SIZE, lpDAQ + DAQDS_wSize);
		isa_writew(1, lpDAQ + DAQDS_wFormat);
		isa_writew(dev.rec_sample_size, lpDAQ + DAQDS_wSampleSize);
		isa_writew(dev.rec_channels, lpDAQ + DAQDS_wChannels);
		isa_writew(dev.rec_sample_rate, lpDAQ + DAQDS_wSampleRate);
		isa_writew(HIMT_RECORD_DONE * 0x100 + n, lpDAQ + DAQDS_wIntMsg);
		isa_writew(n, lpDAQ + DAQDS_wFlags);
	}
}

static void reset_queues(void)
{
	if (dev.mode & FMODE_WRITE) {
		msnd_fifo_make_empty(&dev.DAPF);
		reset_play_queue();
	}
	if (dev.mode & FMODE_READ) {
		msnd_fifo_make_empty(&dev.DARF);
		reset_record_queue();
	}
}

static int dsp_set_format(struct file *file, int val)
{
	int data, i;
	LPDAQD lpDAQ, lpDARQ;

	lpDAQ = dev.base + DAPQ_DATA_BUFF;
	lpDARQ = dev.base + DARQ_DATA_BUFF;

	switch (val) {
	case AFMT_U8:
	case AFMT_S16_LE:
		data = val;
		break;
	default:
		data = DEFSAMPLESIZE;
		break;
	}

	for (i = 0; i < 3; ++i, lpDAQ += DAQDS__size, lpDARQ += DAQDS__size) {
		if (file->f_mode & FMODE_WRITE)
			isa_writew(data, lpDAQ + DAQDS_wSampleSize);
		if (file->f_mode & FMODE_READ)
			isa_writew(data, lpDARQ + DAQDS_wSampleSize);
	}
	if (file->f_mode & FMODE_WRITE)
		dev.play_sample_size = data;
	if (file->f_mode & FMODE_READ)
		dev.rec_sample_size = data;

	return data;
}

static int dsp_get_frag_size(void)
{
	int size;
	size = dev.fifosize / 4;
	if (size > 32 * 1024)
		size = 32 * 1024;
	return size;
}

static int dsp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int val, i, data, tmp;
	LPDAQD lpDAQ, lpDARQ;
        audio_buf_info abinfo;
	unsigned long flags;

	lpDAQ = dev.base + DAPQ_DATA_BUFF;
	lpDARQ = dev.base + DARQ_DATA_BUFF;

	switch (cmd) {
	case SNDCTL_DSP_SUBDIVIDE:
	case SNDCTL_DSP_SETFRAGMENT:
	case SNDCTL_DSP_SETDUPLEX:
	case SNDCTL_DSP_POST:
		return 0;

	case SNDCTL_DSP_GETIPTR:
	case SNDCTL_DSP_GETOPTR:
	case SNDCTL_DSP_MAPINBUF:
	case SNDCTL_DSP_MAPOUTBUF:
		return -EINVAL;

	case SNDCTL_DSP_GETOSPACE:
		if (!(file->f_mode & FMODE_WRITE))
			return -EINVAL;
		spin_lock_irqsave(&dev.lock, flags);
		abinfo.fragsize = dsp_get_frag_size();
                abinfo.bytes = dev.DAPF.n - dev.DAPF.len;
                abinfo.fragstotal = dev.DAPF.n / abinfo.fragsize;
                abinfo.fragments = abinfo.bytes / abinfo.fragsize;
		spin_unlock_irqrestore(&dev.lock, flags);
		return copy_to_user((void *)arg, &abinfo, sizeof(abinfo)) ? -EFAULT : 0;

	case SNDCTL_DSP_GETISPACE:
		if (!(file->f_mode & FMODE_READ))
			return -EINVAL;
		spin_lock_irqsave(&dev.lock, flags);
		abinfo.fragsize = dsp_get_frag_size();
                abinfo.bytes = dev.DARF.n - dev.DARF.len;
                abinfo.fragstotal = dev.DARF.n / abinfo.fragsize;
                abinfo.fragments = abinfo.bytes / abinfo.fragsize;
		spin_unlock_irqrestore(&dev.lock, flags);
		return copy_to_user((void *)arg, &abinfo, sizeof(abinfo)) ? -EFAULT : 0;

	case SNDCTL_DSP_RESET:
		dev.nresets = 0;
		reset_queues();
		return 0;

	case SNDCTL_DSP_SYNC:
		dsp_write_flush();
		return 0;

	case SNDCTL_DSP_GETBLKSIZE:
		tmp = dsp_get_frag_size();
		if (put_user(tmp, (int *)arg))
                        return -EFAULT;
		return 0;

	case SNDCTL_DSP_GETFMTS:
		val = AFMT_S16_LE | AFMT_U8;
		if (put_user(val, (int *)arg))
			return -EFAULT;
		return 0;

	case SNDCTL_DSP_SETFMT:
		if (get_user(val, (int *)arg))
			return -EFAULT;

		if (file->f_mode & FMODE_WRITE)
			data = val == AFMT_QUERY
				? dev.play_sample_size
				: dsp_set_format(file, val);
		else
			data = val == AFMT_QUERY
				? dev.rec_sample_size
				: dsp_set_format(file, val);

		if (put_user(data, (int *)arg))
			return -EFAULT;
		return 0;

	case SNDCTL_DSP_NONBLOCK:
		if (!test_bit(F_DISABLE_WRITE_NDELAY, &dev.flags) &&
		    file->f_mode & FMODE_WRITE)
			dev.play_ndelay = 1;
		if (file->f_mode & FMODE_READ)
			dev.rec_ndelay = 1;
		return 0;

	case SNDCTL_DSP_GETCAPS:
		val = DSP_CAP_DUPLEX | DSP_CAP_BATCH;
		if (put_user(val, (int *)arg))
			return -EFAULT;
		return 0;

	case SNDCTL_DSP_SPEED:
		if (get_user(val, (int *)arg))
			return -EFAULT;

		if (val < 8000)
			val = 8000;

		if (val > 48000)
			val = 48000;

		data = val;

		for (i = 0; i < 3; ++i, lpDAQ += DAQDS__size, lpDARQ += DAQDS__size) {
			if (file->f_mode & FMODE_WRITE)
				isa_writew(data, lpDAQ + DAQDS_wSampleRate);
			if (file->f_mode & FMODE_READ)
				isa_writew(data, lpDARQ + DAQDS_wSampleRate);
		}
		if (file->f_mode & FMODE_WRITE)
			dev.play_sample_rate = data;
		if (file->f_mode & FMODE_READ)
			dev.rec_sample_rate = data;

		if (put_user(data, (int *)arg))
			return -EFAULT;
		return 0;

	case SNDCTL_DSP_CHANNELS:
	case SNDCTL_DSP_STEREO:
		if (get_user(val, (int *)arg))
			return -EFAULT;

		if (cmd == SNDCTL_DSP_CHANNELS) {
			switch (val) {
			case 1:
			case 2:
				data = val;
				break;
			default:
				val = data = 2;
				break;
			}
		} else {
			switch (val) {
			case 0:
				data = 1;
				break;
			default:
				val = 1;
			case 1:
				data = 2;
				break;
			}
		}

		for (i = 0; i < 3; ++i, lpDAQ += DAQDS__size, lpDARQ += DAQDS__size) {
			if (file->f_mode & FMODE_WRITE)
				isa_writew(data, lpDAQ + DAQDS_wChannels);
			if (file->f_mode & FMODE_READ)
				isa_writew(data, lpDARQ + DAQDS_wChannels);
		}
		if (file->f_mode & FMODE_WRITE)
			dev.play_channels = data;
		if (file->f_mode & FMODE_READ)
			dev.rec_channels = data;

		if (put_user(val, (int *)arg))
			return -EFAULT;
		return 0;
	}

	return -EINVAL;
}

static int mixer_get(int d)
{
	if (d > 31)
		return -EINVAL;

	switch (d) {
	case SOUND_MIXER_VOLUME:
	case SOUND_MIXER_PCM:
	case SOUND_MIXER_LINE:
	case SOUND_MIXER_IMIX:
	case SOUND_MIXER_LINE1:
#ifndef MSND_CLASSIC
	case SOUND_MIXER_MIC:
	case SOUND_MIXER_SYNTH:
#endif
		return (dev.left_levels[d] >> 8) * 100 / 0xff | 
			(((dev.right_levels[d] >> 8) * 100 / 0xff) << 8);
	default:
		return 0;
	}
}

#define update_volm(a,b)						\
	isa_writew((dev.left_levels[a] >> 1) *				\
	       isa_readw(dev.SMA + SMA_wCurrMastVolLeft) / 0xffff,	\
	       dev.SMA + SMA_##b##Left);				\
	isa_writew((dev.right_levels[a] >> 1)  *			\
	       isa_readw(dev.SMA + SMA_wCurrMastVolRight) / 0xffff,	\
	       dev.SMA + SMA_##b##Right);

#define update_potm(d,s,ar)						\
	isa_writeb((dev.left_levels[d] >> 8) *				\
	       isa_readw(dev.SMA + SMA_wCurrMastVolLeft) / 0xffff,	\
	       dev.SMA + SMA_##s##Left);				\
	isa_writeb((dev.right_levels[d] >> 8) *				\
	       isa_readw(dev.SMA + SMA_wCurrMastVolRight) / 0xffff,	\
	       dev.SMA + SMA_##s##Right);				\
	if (msnd_send_word(&dev, 0, 0, ar) == 0)			\
		chk_send_dsp_cmd(&dev, HDEX_AUX_REQ);

#define update_pot(d,s,ar)				\
	isa_writeb(dev.left_levels[d] >> 8,		\
	       dev.SMA + SMA_##s##Left);		\
	isa_writeb(dev.right_levels[d] >> 8,		\
	       dev.SMA + SMA_##s##Right);		\
	if (msnd_send_word(&dev, 0, 0, ar) == 0)	\
		chk_send_dsp_cmd(&dev, HDEX_AUX_REQ);

static int mixer_set(int d, int value)
{
	int left = value & 0x000000ff;
	int right = (value & 0x0000ff00) >> 8;
	int bLeft, bRight;
	int wLeft, wRight;
	int updatemaster = 0;

	if (d > 31)
		return -EINVAL;

	bLeft = left * 0xff / 100;
	wLeft = left * 0xffff / 100;

	bRight = right * 0xff / 100;
	wRight = right * 0xffff / 100;

	dev.left_levels[d] = wLeft;
	dev.right_levels[d] = wRight;

	switch (d) {
		/* master volume unscaled controls */
	case SOUND_MIXER_LINE:			/* line pot control */
		/* scaled by IMIX in digital mix */
		isa_writeb(bLeft, dev.SMA + SMA_bInPotPosLeft);
		isa_writeb(bRight, dev.SMA + SMA_bInPotPosRight);
		if (msnd_send_word(&dev, 0, 0, HDEXAR_IN_SET_POTS) == 0)
			chk_send_dsp_cmd(&dev, HDEX_AUX_REQ);
		break;
#ifndef MSND_CLASSIC
	case SOUND_MIXER_MIC:			/* mic pot control */
		/* scaled by IMIX in digital mix */
		isa_writeb(bLeft, dev.SMA + SMA_bMicPotPosLeft);
		isa_writeb(bRight, dev.SMA + SMA_bMicPotPosRight);
		if (msnd_send_word(&dev, 0, 0, HDEXAR_MIC_SET_POTS) == 0)
			chk_send_dsp_cmd(&dev, HDEX_AUX_REQ);
		break;
#endif
	case SOUND_MIXER_VOLUME:		/* master volume */
		isa_writew(wLeft, dev.SMA + SMA_wCurrMastVolLeft);
		isa_writew(wRight, dev.SMA + SMA_wCurrMastVolRight);
		/* fall through */

	case SOUND_MIXER_LINE1:			/* aux pot control */
		/* scaled by master volume */
		/* fall through */

		/* digital controls */
	case SOUND_MIXER_SYNTH:			/* synth vol (dsp mix) */
	case SOUND_MIXER_PCM:			/* pcm vol (dsp mix) */
	case SOUND_MIXER_IMIX:			/* input monitor (dsp mix) */
		/* scaled by master volume */
		updatemaster = 1;
		break;

	default:
		return 0;
	}

	if (updatemaster) {
		/* update master volume scaled controls */
		update_volm(SOUND_MIXER_PCM, wCurrPlayVol);
		update_volm(SOUND_MIXER_IMIX, wCurrInVol);
#ifndef MSND_CLASSIC
		update_volm(SOUND_MIXER_SYNTH, wCurrMHdrVol);
#endif
		update_potm(SOUND_MIXER_LINE1, bAuxPotPos, HDEXAR_AUX_SET_POTS);
	}

	return mixer_get(d);
}

static void mixer_setup(void)
{
	update_pot(SOUND_MIXER_LINE, bInPotPos, HDEXAR_IN_SET_POTS);
	update_potm(SOUND_MIXER_LINE1, bAuxPotPos, HDEXAR_AUX_SET_POTS);
	update_volm(SOUND_MIXER_PCM, wCurrPlayVol);
	update_volm(SOUND_MIXER_IMIX, wCurrInVol);
#ifndef MSND_CLASSIC
	update_pot(SOUND_MIXER_MIC, bMicPotPos, HDEXAR_MIC_SET_POTS);
	update_volm(SOUND_MIXER_SYNTH, wCurrMHdrVol);
#endif
}

static unsigned long set_recsrc(unsigned long recsrc)
{
	if (dev.recsrc == recsrc)
		return dev.recsrc;
#ifdef HAVE_NORECSRC
	else if (recsrc == 0)
		dev.recsrc = 0;
#endif
	else
		dev.recsrc ^= recsrc;

#ifndef MSND_CLASSIC
	if (dev.recsrc & SOUND_MASK_IMIX) {
		if (msnd_send_word(&dev, 0, 0, HDEXAR_SET_ANA_IN) == 0)
			chk_send_dsp_cmd(&dev, HDEX_AUX_REQ);
	}
	else if (dev.recsrc & SOUND_MASK_SYNTH) {
		if (msnd_send_word(&dev, 0, 0, HDEXAR_SET_SYNTH_IN) == 0)
			chk_send_dsp_cmd(&dev, HDEX_AUX_REQ);
	}
	else if ((dev.recsrc & SOUND_MASK_DIGITAL1) && test_bit(F_HAVEDIGITAL, &dev.flags)) {
		if (msnd_send_word(&dev, 0, 0, HDEXAR_SET_DAT_IN) == 0)
      			chk_send_dsp_cmd(&dev, HDEX_AUX_REQ);
	}
	else {
#ifdef HAVE_NORECSRC
		/* Select no input (?) */
		dev.recsrc = 0;
#else
		dev.recsrc = SOUND_MASK_IMIX;
		if (msnd_send_word(&dev, 0, 0, HDEXAR_SET_ANA_IN) == 0)
			chk_send_dsp_cmd(&dev, HDEX_AUX_REQ);
#endif
	}
#endif /* MSND_CLASSIC */

	return dev.recsrc;
}

static unsigned long force_recsrc(unsigned long recsrc)
{
	dev.recsrc = 0;
	return set_recsrc(recsrc);
}

#define set_mixer_info()							\
		strncpy(info.id, "MSNDMIXER", sizeof(info.id));			\
		strncpy(info.name, "MultiSound Mixer", sizeof(info.name));

static int mixer_ioctl(unsigned int cmd, unsigned long arg)
{
	if (cmd == SOUND_MIXER_INFO) {
		mixer_info info;
		set_mixer_info();
		info.modify_counter = dev.mixer_mod_count;
		return copy_to_user((void *)arg, &info, sizeof(info));
	} else if (cmd == SOUND_OLD_MIXER_INFO) {
		_old_mixer_info info;
		set_mixer_info();
		return copy_to_user((void *)arg, &info, sizeof(info));
	} else if (cmd == SOUND_MIXER_PRIVATE1) {
		dev.nresets = 0;
		dsp_full_reset();
		return 0;
	} else if (((cmd >> 8) & 0xff) == 'M') {
		int val = 0;

		if (_SIOC_DIR(cmd) & _SIOC_WRITE) {
			switch (cmd & 0xff) {
			case SOUND_MIXER_RECSRC:
				if (get_user(val, (int *)arg))
					return -EFAULT;
				val = set_recsrc(val);
				break;

			default:
				if (get_user(val, (int *)arg))
					return -EFAULT;
				val = mixer_set(cmd & 0xff, val);
				break;
			}
			++dev.mixer_mod_count;
			return put_user(val, (int *)arg);
		} else {
			switch (cmd & 0xff) {
			case SOUND_MIXER_RECSRC:
				val = dev.recsrc;
				break;

			case SOUND_MIXER_DEVMASK:
			case SOUND_MIXER_STEREODEVS:
				val =   SOUND_MASK_PCM |
					SOUND_MASK_LINE |
					SOUND_MASK_IMIX |
					SOUND_MASK_LINE1 |
#ifndef MSND_CLASSIC
					SOUND_MASK_MIC |
					SOUND_MASK_SYNTH |
#endif
					SOUND_MASK_VOLUME;
				break;
				  
			case SOUND_MIXER_RECMASK:
#ifdef MSND_CLASSIC
				val =   0;
#else
				val =   SOUND_MASK_IMIX |
					SOUND_MASK_SYNTH;
				if (test_bit(F_HAVEDIGITAL, &dev.flags))
					val |= SOUND_MASK_DIGITAL1;
#endif
				break;
				  
			case SOUND_MIXER_CAPS:
				val =   SOUND_CAP_EXCL_INPUT;
				break;

			default:
				if ((val = mixer_get(cmd & 0xff)) < 0)
					return -EINVAL;
				break;
			}
		}

		return put_user(val, (int *)arg); 
	}

	return -EINVAL;
}

static int dev_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{
	int minor = MINOR(inode->i_rdev);

	if (cmd == OSS_GETVERSION) {
		int sound_version = SOUND_VERSION;
		return put_user(sound_version, (int *)arg);
	}

	if (minor == dev.dsp_minor)
		return dsp_ioctl(file, cmd, arg);
	else if (minor == dev.mixer_minor)
		return mixer_ioctl(cmd, arg);

	return -EINVAL;
}

static void dsp_write_flush(void)
{
	if (!(dev.mode & FMODE_WRITE) || !test_bit(F_WRITING, &dev.flags))
		return;
	set_bit(F_WRITEFLUSH, &dev.flags);
	interruptible_sleep_on_timeout(
		&dev.writeflush,
		get_play_delay_jiffies(dev.DAPF.len));
	clear_bit(F_WRITEFLUSH, &dev.flags);
	if (!signal_pending(current)) {
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(get_play_delay_jiffies(DAP_BUFF_SIZE));
	}
	clear_bit(F_WRITING, &dev.flags);
}

static void dsp_halt(struct file *file)
{
	if ((file ? file->f_mode : dev.mode) & FMODE_READ) {
		clear_bit(F_READING, &dev.flags);
		chk_send_dsp_cmd(&dev, HDEX_RECORD_STOP);
		msnd_disable_irq(&dev);
		if (file) {
			printk(KERN_DEBUG LOGNAME ": Stopping read for %p\n", file);
			dev.mode &= ~FMODE_READ;
		}
		clear_bit(F_AUDIO_READ_INUSE, &dev.flags);
	}
	if ((file ? file->f_mode : dev.mode) & FMODE_WRITE) {
		if (test_bit(F_WRITING, &dev.flags)) {
			dsp_write_flush();
			chk_send_dsp_cmd(&dev, HDEX_PLAY_STOP);
		}
		msnd_disable_irq(&dev);
		if (file) {
			printk(KERN_DEBUG LOGNAME ": Stopping write for %p\n", file);
			dev.mode &= ~FMODE_WRITE;
		}
		clear_bit(F_AUDIO_WRITE_INUSE, &dev.flags);
	}
}

static int dsp_release(struct file *file)
{
	dsp_halt(file);
	return 0;
}

static int dsp_open(struct file *file)
{
	if ((file ? file->f_mode : dev.mode) & FMODE_WRITE) {
		set_bit(F_AUDIO_WRITE_INUSE, &dev.flags);
		clear_bit(F_WRITING, &dev.flags);
		msnd_fifo_make_empty(&dev.DAPF);
		reset_play_queue();
		if (file) {
			printk(KERN_DEBUG LOGNAME ": Starting write for %p\n", file);
			dev.mode |= FMODE_WRITE;
		}
		msnd_enable_irq(&dev);
	}
	if ((file ? file->f_mode : dev.mode) & FMODE_READ) {
		set_bit(F_AUDIO_READ_INUSE, &dev.flags);
		clear_bit(F_READING, &dev.flags);
		msnd_fifo_make_empty(&dev.DARF);
		reset_record_queue();
		if (file) {
			printk(KERN_DEBUG LOGNAME ": Starting read for %p\n", file);
			dev.mode |= FMODE_READ;
		}
		msnd_enable_irq(&dev);
	}
	return 0;
}

static void set_default_play_audio_parameters(void)
{
	dev.play_sample_size = DEFSAMPLESIZE;
	dev.play_sample_rate = DEFSAMPLERATE;
	dev.play_channels = DEFCHANNELS;
}

static void set_default_rec_audio_parameters(void)
{
	dev.rec_sample_size = DEFSAMPLESIZE;
	dev.rec_sample_rate = DEFSAMPLERATE;
	dev.rec_channels = DEFCHANNELS;
}

static void set_default_audio_parameters(void)
{
	set_default_play_audio_parameters();
	set_default_rec_audio_parameters();
}

static int dev_open(struct inode *inode, struct file *file)
{
	int minor = MINOR(inode->i_rdev);
	int err = 0;

	if (minor == dev.dsp_minor) {
		if ((file->f_mode & FMODE_WRITE &&
		     test_bit(F_AUDIO_WRITE_INUSE, &dev.flags)) ||
		    (file->f_mode & FMODE_READ &&
		     test_bit(F_AUDIO_READ_INUSE, &dev.flags)))
			return -EBUSY;

		if ((err = dsp_open(file)) >= 0) {
			dev.nresets = 0;
			if (file->f_mode & FMODE_WRITE) {
				set_default_play_audio_parameters();
				if (!test_bit(F_DISABLE_WRITE_NDELAY, &dev.flags))
					dev.play_ndelay = (file->f_flags & O_NDELAY) ? 1 : 0;
				else
					dev.play_ndelay = 0;
			}
			if (file->f_mode & FMODE_READ) {
				set_default_rec_audio_parameters();
				dev.rec_ndelay = (file->f_flags & O_NDELAY) ? 1 : 0;
			}
		}
	}
	else if (minor == dev.mixer_minor) {
		/* nothing */
	} else
		err = -EINVAL;

	return err;
}

static int dev_release(struct inode *inode, struct file *file)
{
	int minor = MINOR(inode->i_rdev);
	int err = 0;

	lock_kernel();
	if (minor == dev.dsp_minor)
		err = dsp_release(file);
	else if (minor == dev.mixer_minor) {
		/* nothing */
	} else
		err = -EINVAL;
	unlock_kernel();
	return err;
}

static __inline__ int pack_DARQ_to_DARF(register int bank)
{
	register int size, n, timeout = 3;
	register WORD wTmp;
	LPDAQD DAQD;

	/* Increment the tail and check for queue wrap */
	wTmp = isa_readw(dev.DARQ + JQS_wTail) + PCTODSP_OFFSET(DAQDS__size);
	if (wTmp > isa_readw(dev.DARQ + JQS_wSize))
		wTmp = 0;
	while (wTmp == isa_readw(dev.DARQ + JQS_wHead) && timeout--)
		udelay(1);
	isa_writew(wTmp, dev.DARQ + JQS_wTail);

	/* Get our digital audio queue struct */
	DAQD = bank * DAQDS__size + dev.base + DARQ_DATA_BUFF;

	/* Get length of data */
	size = isa_readw(DAQD + DAQDS_wSize);

	/* Read data from the head (unprotected bank 1 access okay
           since this is only called inside an interrupt) */
	outb(HPBLKSEL_1, dev.io + HP_BLKS);
	if ((n = msnd_fifo_write(
		&dev.DARF,
		(char *)(dev.base + bank * DAR_BUFF_SIZE),
		size, 0)) <= 0) {
		outb(HPBLKSEL_0, dev.io + HP_BLKS);
		return n;
	}
	outb(HPBLKSEL_0, dev.io + HP_BLKS);

	return 1;
}

static __inline__ int pack_DAPF_to_DAPQ(register int start)
{
	register WORD DAPQ_tail;
	register int protect = start, nbanks = 0;
	LPDAQD DAQD;

	DAPQ_tail = isa_readw(dev.DAPQ + JQS_wTail);
	while (DAPQ_tail != isa_readw(dev.DAPQ + JQS_wHead) || start) {
		register int bank_num = DAPQ_tail / PCTODSP_OFFSET(DAQDS__size);
		register int n;
		unsigned long flags;

		/* Write the data to the new tail */
		if (protect) {
			/* Critical section: protect fifo in non-interrupt */
			spin_lock_irqsave(&dev.lock, flags);
			if ((n = msnd_fifo_read(
				&dev.DAPF,
				(char *)(dev.base + bank_num * DAP_BUFF_SIZE),
				DAP_BUFF_SIZE, 0)) < 0) {
				spin_unlock_irqrestore(&dev.lock, flags);
				return n;
			}
			spin_unlock_irqrestore(&dev.lock, flags);
		} else {
			if ((n = msnd_fifo_read(
				&dev.DAPF,
				(char *)(dev.base + bank_num * DAP_BUFF_SIZE),
				DAP_BUFF_SIZE, 0)) < 0) {
				return n;
			}
		}
		if (!n)
			break;

		if (start)
			start = 0;

		/* Get our digital audio queue struct */
		DAQD = bank_num * DAQDS__size + dev.base + DAPQ_DATA_BUFF;

		/* Write size of this bank */
		isa_writew(n, DAQD + DAQDS_wSize);
		++nbanks;

		/* Then advance the tail */
		DAPQ_tail = (++bank_num % 3) * PCTODSP_OFFSET(DAQDS__size);
		isa_writew(DAPQ_tail, dev.DAPQ + JQS_wTail);
		/* Tell the DSP to play the bank */
		msnd_send_dsp_cmd(&dev, HDEX_PLAY_START);
	}
	return nbanks;
}

static int dsp_read(char *buf, size_t len)
{
	int count = len;

	while (count > 0) {
		int n;
		unsigned long flags;

		/* Critical section: protect fifo in non-interrupt */
		spin_lock_irqsave(&dev.lock, flags);
		if ((n = msnd_fifo_read(&dev.DARF, buf, count, 1)) < 0) {
			printk(KERN_WARNING LOGNAME ": FIFO read error\n");
			spin_unlock_irqrestore(&dev.lock, flags);
			return n;
		}
		spin_unlock_irqrestore(&dev.lock, flags);
		buf += n;
		count -= n;

		if (!test_bit(F_READING, &dev.flags) && dev.mode & FMODE_READ) {
			dev.last_recbank = -1;
			if (chk_send_dsp_cmd(&dev, HDEX_RECORD_START) == 0)
				set_bit(F_READING, &dev.flags);
		}

		if (dev.rec_ndelay)
			return count == len ? -EAGAIN : len - count;

		if (count > 0) {
			set_bit(F_READBLOCK, &dev.flags);
			if (!interruptible_sleep_on_timeout(
				&dev.readblock,
				get_rec_delay_jiffies(DAR_BUFF_SIZE)))
				clear_bit(F_READING, &dev.flags);
			clear_bit(F_READBLOCK, &dev.flags);
			if (signal_pending(current))
				return -EINTR;
		}
	}

	return len - count;
}

static int dsp_write(const char *buf, size_t len)
{
	int count = len;

	while (count > 0) {
		int n;
		unsigned long flags;

		/* Critical section: protect fifo in non-interrupt */
		spin_lock_irqsave(&dev.lock, flags);
		if ((n = msnd_fifo_write(&dev.DAPF, buf, count, 1)) < 0) {
			printk(KERN_WARNING LOGNAME ": FIFO write error\n");
			spin_unlock_irqrestore(&dev.lock, flags);
			return n;
		}
		spin_unlock_irqrestore(&dev.lock, flags);
		buf += n;
		count -= n;

		if (!test_bit(F_WRITING, &dev.flags) && (dev.mode & FMODE_WRITE)) {
			dev.last_playbank = -1;
			if (pack_DAPF_to_DAPQ(1) > 0)
				set_bit(F_WRITING, &dev.flags);
		}

		if (dev.play_ndelay)
			return count == len ? -EAGAIN : len - count;

		if (count > 0) {
			set_bit(F_WRITEBLOCK, &dev.flags);
			interruptible_sleep_on_timeout(
				&dev.writeblock,
				get_play_delay_jiffies(DAP_BUFF_SIZE));
			clear_bit(F_WRITEBLOCK, &dev.flags);
			if (signal_pending(current))
				return -EINTR;
		}
	}

	return len - count;
}

static ssize_t dev_read(struct file *file, char *buf, size_t count, loff_t *off)
{
	int minor = MINOR(file->f_dentry->d_inode->i_rdev);
	if (minor == dev.dsp_minor)
		return dsp_read(buf, count);
	else
		return -EINVAL;
}

static ssize_t dev_write(struct file *file, const char *buf, size_t count, loff_t *off)
{
	int minor = MINOR(file->f_dentry->d_inode->i_rdev);
	if (minor == dev.dsp_minor)
		return dsp_write(buf, count);
	else
		return -EINVAL;
}

static __inline__ void eval_dsp_msg(register WORD wMessage)
{
	switch (HIBYTE(wMessage)) {
	case HIMT_PLAY_DONE:
		if (dev.last_playbank == LOBYTE(wMessage) || !test_bit(F_WRITING, &dev.flags))
			break;
		dev.last_playbank = LOBYTE(wMessage);

		if (pack_DAPF_to_DAPQ(0) <= 0) {
			if (!test_bit(F_WRITEBLOCK, &dev.flags)) {
				if (test_and_clear_bit(F_WRITEFLUSH, &dev.flags))
					wake_up_interruptible(&dev.writeflush);
			}
			clear_bit(F_WRITING, &dev.flags);
		}

		if (test_bit(F_WRITEBLOCK, &dev.flags))
			wake_up_interruptible(&dev.writeblock);
		break;

	case HIMT_RECORD_DONE:
		if (dev.last_recbank == LOBYTE(wMessage))
			break;
		dev.last_recbank = LOBYTE(wMessage);

		pack_DARQ_to_DARF(dev.last_recbank);

		if (test_bit(F_READBLOCK, &dev.flags))
			wake_up_interruptible(&dev.readblock);
		break;

	case HIMT_DSP:
		switch (LOBYTE(wMessage)) {
#ifndef MSND_CLASSIC
		case HIDSP_PLAY_UNDER:
#endif
		case HIDSP_INT_PLAY_UNDER:
/*			printk(KERN_DEBUG LOGNAME ": Play underflow\n"); */
			clear_bit(F_WRITING, &dev.flags);
			break;

		case HIDSP_INT_RECORD_OVER:
/*			printk(KERN_DEBUG LOGNAME ": Record overflow\n"); */
			clear_bit(F_READING, &dev.flags);
			break;

		default:
/*			printk(KERN_DEBUG LOGNAME ": DSP message %d 0x%02x\n",
			LOBYTE(wMessage), LOBYTE(wMessage)); */
			break;
		}
		break;

        case HIMT_MIDI_IN_UCHAR:
		if (dev.midi_in_interrupt)
			(*dev.midi_in_interrupt)(&dev);
		break;

	default:
/*		printk(KERN_DEBUG LOGNAME ": HIMT message %d 0x%02x\n", HIBYTE(wMessage), HIBYTE(wMessage)); */
		break;
	}
}

static void intr(int irq, void *dev_id, struct pt_regs *regs)
{
	/* Send ack to DSP */
	inb(dev.io + HP_RXL);

	/* Evaluate queued DSP messages */
	while (isa_readw(dev.DSPQ + JQS_wTail) != isa_readw(dev.DSPQ + JQS_wHead)) {
		register WORD wTmp;

		eval_dsp_msg(isa_readw(dev.pwDSPQData + 2*isa_readw(dev.DSPQ + JQS_wHead)));

		if ((wTmp = isa_readw(dev.DSPQ + JQS_wHead) + 1) > isa_readw(dev.DSPQ + JQS_wSize))
			isa_writew(0, dev.DSPQ + JQS_wHead);
		else
			isa_writew(wTmp, dev.DSPQ + JQS_wHead);
	}
}

static struct file_operations dev_fileops = {
	owner:		THIS_MODULE,
	read:		dev_read,
	write:		dev_write,
	ioctl:		dev_ioctl,
	open:		dev_open,
	release:	dev_release,
};

static int reset_dsp(void)
{
	int timeout = 100;

	outb(HPDSPRESET_ON, dev.io + HP_DSPR);
	mdelay(1);
#ifndef MSND_CLASSIC
	dev.info = inb(dev.io + HP_INFO);
#endif
	outb(HPDSPRESET_OFF, dev.io + HP_DSPR);
	mdelay(1);
	while (timeout-- > 0) {
		if (inb(dev.io + HP_CVR) == HP_CVR_DEF)
			return 0;
		mdelay(1);
	}
	printk(KERN_ERR LOGNAME ": Cannot reset DSP\n");

	return -EIO;
}

static int __init probe_multisound(void)
{
#ifndef MSND_CLASSIC
	char *xv, *rev = NULL;
	char *pin = "Pinnacle", *fiji = "Fiji";
	char *pinfiji = "Pinnacle/Fiji";
#endif

	if (check_region(dev.io, dev.numio)) {
		printk(KERN_ERR LOGNAME ": I/O port conflict\n");
		return -ENODEV;
	}
	request_region(dev.io, dev.numio, "probing");

	if (reset_dsp() < 0) {
		release_region(dev.io, dev.numio);
		return -ENODEV;
	}

#ifdef MSND_CLASSIC
	dev.name = "Classic/Tahiti/Monterey";
	printk(KERN_INFO LOGNAME ": %s, "
#else
	switch (dev.info >> 4) {
	case 0xf: xv = "<= 1.15"; break;
	case 0x1: xv = "1.18/1.2"; break;
	case 0x2: xv = "1.3"; break;
	case 0x3: xv = "1.4"; break;
	default: xv = "unknown"; break;
	}

	switch (dev.info & 0x7) {
	case 0x0: rev = "I"; dev.name = pin; break;
	case 0x1: rev = "F"; dev.name = pin; break;
	case 0x2: rev = "G"; dev.name = pin; break;
	case 0x3: rev = "H"; dev.name = pin; break;
	case 0x4: rev = "E"; dev.name = fiji; break;
	case 0x5: rev = "C"; dev.name = fiji; break;
	case 0x6: rev = "D"; dev.name = fiji; break;
	case 0x7:
		rev = "A-B (Fiji) or A-E (Pinnacle)";
		dev.name = pinfiji;
		break;
	}
	printk(KERN_INFO LOGNAME ": %s revision %s, Xilinx version %s, "
#endif /* MSND_CLASSIC */
	       "I/O 0x%x-0x%x, IRQ %d, memory mapped to 0x%lX-0x%lX\n",
	       dev.name,
#ifndef MSND_CLASSIC
	       rev, xv,
#endif
	       dev.io, dev.io + dev.numio - 1,
	       dev.irq,
	       dev.base, dev.base + 0x7fff);

	release_region(dev.io, dev.numio);
	return 0;
}

static int init_sma(void)
{
	static int initted;
	WORD mastVolLeft, mastVolRight;
	unsigned long flags;

#ifdef MSND_CLASSIC
	outb(dev.memid, dev.io + HP_MEMM);
#endif
	outb(HPBLKSEL_0, dev.io + HP_BLKS);
	if (initted) {
		mastVolLeft = isa_readw(dev.SMA + SMA_wCurrMastVolLeft);
		mastVolRight = isa_readw(dev.SMA + SMA_wCurrMastVolRight);
	} else
		mastVolLeft = mastVolRight = 0;
	isa_memset_io(dev.base, 0, 0x8000);

	/* Critical section: bank 1 access */
	spin_lock_irqsave(&dev.lock, flags);
	outb(HPBLKSEL_1, dev.io + HP_BLKS);
	isa_memset_io(dev.base, 0, 0x8000);
	outb(HPBLKSEL_0, dev.io + HP_BLKS);
	spin_unlock_irqrestore(&dev.lock, flags);

	dev.pwDSPQData = (dev.base + DSPQ_DATA_BUFF);
	dev.pwMODQData = (dev.base + MODQ_DATA_BUFF);
	dev.pwMIDQData = (dev.base + MIDQ_DATA_BUFF);

	/* Motorola 56k shared memory base */
	dev.SMA = dev.base + SMA_STRUCT_START;

	/* Digital audio play queue */
	dev.DAPQ = dev.base + DAPQ_OFFSET;
	msnd_init_queue(dev.DAPQ, DAPQ_DATA_BUFF, DAPQ_BUFF_SIZE);

	/* Digital audio record queue */
	dev.DARQ = dev.base + DARQ_OFFSET;
	msnd_init_queue(dev.DARQ, DARQ_DATA_BUFF, DARQ_BUFF_SIZE);

	/* MIDI out queue */
	dev.MODQ = dev.base + MODQ_OFFSET;
	msnd_init_queue(dev.MODQ, MODQ_DATA_BUFF, MODQ_BUFF_SIZE);

	/* MIDI in queue */
	dev.MIDQ = dev.base + MIDQ_OFFSET;
	msnd_init_queue(dev.MIDQ, MIDQ_DATA_BUFF, MIDQ_BUFF_SIZE);

	/* DSP -> host message queue */
	dev.DSPQ = dev.base + DSPQ_OFFSET;
	msnd_init_queue(dev.DSPQ, DSPQ_DATA_BUFF, DSPQ_BUFF_SIZE);

	/* Setup some DSP values */
#ifndef MSND_CLASSIC
	isa_writew(1, dev.SMA + SMA_wCurrPlayFormat);
	isa_writew(dev.play_sample_size, dev.SMA + SMA_wCurrPlaySampleSize);
	isa_writew(dev.play_channels, dev.SMA + SMA_wCurrPlayChannels);
	isa_writew(dev.play_sample_rate, dev.SMA + SMA_wCurrPlaySampleRate);
#endif
	isa_writew(dev.play_sample_rate, dev.SMA + SMA_wCalFreqAtoD);
	isa_writew(mastVolLeft, dev.SMA + SMA_wCurrMastVolLeft);
	isa_writew(mastVolRight, dev.SMA + SMA_wCurrMastVolRight);
#ifndef MSND_CLASSIC
	isa_writel(0x00010000, dev.SMA + SMA_dwCurrPlayPitch);
	isa_writel(0x00000001, dev.SMA + SMA_dwCurrPlayRate);
#endif
	isa_writew(0x303, dev.SMA + SMA_wCurrInputTagBits);

	initted = 1;

	return 0;
}

static int __init calibrate_adc(WORD srate)
{
	isa_writew(srate, dev.SMA + SMA_wCalFreqAtoD);
	if (dev.calibrate_signal == 0)
		isa_writew(isa_readw(dev.SMA + SMA_wCurrHostStatusFlags)
		       | 0x0001, dev.SMA + SMA_wCurrHostStatusFlags);
	else
		isa_writew(isa_readw(dev.SMA + SMA_wCurrHostStatusFlags)
		       & ~0x0001, dev.SMA + SMA_wCurrHostStatusFlags);
	if (msnd_send_word(&dev, 0, 0, HDEXAR_CAL_A_TO_D) == 0 &&
	    chk_send_dsp_cmd(&dev, HDEX_AUX_REQ) == 0) {
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ / 3);
		return 0;
	}
	printk(KERN_WARNING LOGNAME ": ADC calibration failed\n");

	return -EIO;
}

static int upload_dsp_code(void)
{
	outb(HPBLKSEL_0, dev.io + HP_BLKS);
#ifndef HAVE_DSPCODEH
	INITCODESIZE = mod_firmware_load(INITCODEFILE, &INITCODE);
	if (!INITCODE) {
		printk(KERN_ERR LOGNAME ": Error loading " INITCODEFILE);
		return -EBUSY;
	}

	PERMCODESIZE = mod_firmware_load(PERMCODEFILE, &PERMCODE);
	if (!PERMCODE) {
		printk(KERN_ERR LOGNAME ": Error loading " PERMCODEFILE);
		vfree(INITCODE);
		return -EBUSY;
	}
#endif
	isa_memcpy_toio(dev.base, PERMCODE, PERMCODESIZE);
	if (msnd_upload_host(&dev, INITCODE, INITCODESIZE) < 0) {
		printk(KERN_WARNING LOGNAME ": Error uploading to DSP\n");
		return -ENODEV;
	}
#ifdef HAVE_DSPCODEH
	printk(KERN_INFO LOGNAME ": DSP firmware uploaded (resident)\n");
#else
	printk(KERN_INFO LOGNAME ": DSP firmware uploaded\n");
#endif

#ifndef HAVE_DSPCODEH
	vfree(INITCODE);
	vfree(PERMCODE);
#endif

	return 0;
}

#ifdef MSND_CLASSIC
static void reset_proteus(void)
{
	outb(HPPRORESET_ON, dev.io + HP_PROR);
	mdelay(TIME_PRO_RESET);
	outb(HPPRORESET_OFF, dev.io + HP_PROR);
	mdelay(TIME_PRO_RESET_DONE);
}
#endif

static int initialize(void)
{
	int err, timeout;

#ifdef MSND_CLASSIC
	outb(HPWAITSTATE_0, dev.io + HP_WAIT);
	outb(HPBITMODE_16, dev.io + HP_BITM);

	reset_proteus();
#endif
	if ((err = init_sma()) < 0) {
		printk(KERN_WARNING LOGNAME ": Cannot initialize SMA\n");
		return err;
	}

	if ((err = reset_dsp()) < 0)
		return err;

	if ((err = upload_dsp_code()) < 0) {
		printk(KERN_WARNING LOGNAME ": Cannot upload DSP code\n");
		return err;
	}

	timeout = 200;
	while (isa_readw(dev.base)) {
		mdelay(1);
		if (!timeout--) {
			printk(KERN_DEBUG LOGNAME ": DSP reset timeout\n");
			return -EIO;
		}
	}

	mixer_setup();

	return 0;
}

static int dsp_full_reset(void)
{
	int rv;

	if (test_bit(F_RESETTING, &dev.flags) || ++dev.nresets > 10)
		return 0;

	set_bit(F_RESETTING, &dev.flags);
	printk(KERN_INFO LOGNAME ": DSP reset\n");
	dsp_halt(NULL);			/* Unconditionally halt */
	if ((rv = initialize()))
		printk(KERN_WARNING LOGNAME ": DSP reset failed\n");
	force_recsrc(dev.recsrc);
	dsp_open(NULL);
	clear_bit(F_RESETTING, &dev.flags);

	return rv;
}

static int __init attach_multisound(void)
{
	int err;

	if ((err = request_irq(dev.irq, intr, 0, dev.name, &dev)) < 0) {
		printk(KERN_ERR LOGNAME ": Couldn't grab IRQ %d\n", dev.irq);
		return err;
	}
	request_region(dev.io, dev.numio, dev.name);

        if ((err = dsp_full_reset()) < 0) {
		release_region(dev.io, dev.numio);
		free_irq(dev.irq, &dev);
		return err;
	}

	if ((err = msnd_register(&dev)) < 0) {
		printk(KERN_ERR LOGNAME ": Unable to register MultiSound\n");
		release_region(dev.io, dev.numio);
		free_irq(dev.irq, &dev);
		return err;
	}

	if ((dev.dsp_minor = register_sound_dsp(&dev_fileops, -1)) < 0) {
		printk(KERN_ERR LOGNAME ": Unable to register DSP operations\n");
		msnd_unregister(&dev);
		release_region(dev.io, dev.numio);
		free_irq(dev.irq, &dev);
		return dev.dsp_minor;
	}

	if ((dev.mixer_minor = register_sound_mixer(&dev_fileops, -1)) < 0) {
		printk(KERN_ERR LOGNAME ": Unable to register mixer operations\n");
		unregister_sound_mixer(dev.mixer_minor);
		msnd_unregister(&dev);
		release_region(dev.io, dev.numio);
		free_irq(dev.irq, &dev);
		return dev.mixer_minor;
	}

	dev.ext_midi_dev = dev.hdr_midi_dev = -1;

	disable_irq(dev.irq);
	calibrate_adc(dev.play_sample_rate);
#ifndef MSND_CLASSIC
	force_recsrc(SOUND_MASK_IMIX);
#endif

	return 0;
}

#ifdef MODULE
static void __exit unload_multisound(void)
{
	release_region(dev.io, dev.numio);
	free_irq(dev.irq, &dev);
	unregister_sound_mixer(dev.mixer_minor);
	unregister_sound_dsp(dev.dsp_minor);
	msnd_unregister(&dev);
}
#endif

#ifndef MSND_CLASSIC

/* Pinnacle/Fiji Logical Device Configuration */

static int __init msnd_write_cfg(int cfg, int reg, int value)
{
	outb(reg, cfg);
	outb(value, cfg + 1);
	if (value != inb(cfg + 1)) {
		printk(KERN_ERR LOGNAME ": msnd_write_cfg: I/O error\n");
		return -EIO;
	}
	return 0;
}

static int __init msnd_write_cfg_io0(int cfg, int num, WORD io)
{
	if (msnd_write_cfg(cfg, IREG_LOGDEVICE, num))
		return -EIO;
	if (msnd_write_cfg(cfg, IREG_IO0_BASEHI, HIBYTE(io)))
		return -EIO;
	if (msnd_write_cfg(cfg, IREG_IO0_BASELO, LOBYTE(io)))
		return -EIO;
	return 0;
}

static int __init msnd_write_cfg_io1(int cfg, int num, WORD io)
{
	if (msnd_write_cfg(cfg, IREG_LOGDEVICE, num))
		return -EIO;
	if (msnd_write_cfg(cfg, IREG_IO1_BASEHI, HIBYTE(io)))
		return -EIO;
	if (msnd_write_cfg(cfg, IREG_IO1_BASELO, LOBYTE(io)))
		return -EIO;
	return 0;
}

static int __init msnd_write_cfg_irq(int cfg, int num, WORD irq)
{
	if (msnd_write_cfg(cfg, IREG_LOGDEVICE, num))
		return -EIO;
	if (msnd_write_cfg(cfg, IREG_IRQ_NUMBER, LOBYTE(irq)))
		return -EIO;
	if (msnd_write_cfg(cfg, IREG_IRQ_TYPE, IRQTYPE_EDGE))
		return -EIO;
	return 0;
}

static int __init msnd_write_cfg_mem(int cfg, int num, int mem)
{
	WORD wmem;

	mem >>= 8;
	mem &= 0xfff;
	wmem = (WORD)mem;
	if (msnd_write_cfg(cfg, IREG_LOGDEVICE, num))
		return -EIO;
	if (msnd_write_cfg(cfg, IREG_MEMBASEHI, HIBYTE(wmem)))
		return -EIO;
	if (msnd_write_cfg(cfg, IREG_MEMBASELO, LOBYTE(wmem)))
		return -EIO;
	if (wmem && msnd_write_cfg(cfg, IREG_MEMCONTROL, (MEMTYPE_HIADDR | MEMTYPE_16BIT)))
		return -EIO;
	return 0;
}

static int __init msnd_activate_logical(int cfg, int num)
{
	if (msnd_write_cfg(cfg, IREG_LOGDEVICE, num))
		return -EIO;
	if (msnd_write_cfg(cfg, IREG_ACTIVATE, LD_ACTIVATE))
		return -EIO;
	return 0;
}

static int __init msnd_write_cfg_logical(int cfg, int num, WORD io0, WORD io1, WORD irq, int mem)
{
	if (msnd_write_cfg(cfg, IREG_LOGDEVICE, num))
		return -EIO;
	if (msnd_write_cfg_io0(cfg, num, io0))
		return -EIO;
	if (msnd_write_cfg_io1(cfg, num, io1))
		return -EIO;
	if (msnd_write_cfg_irq(cfg, num, irq))
		return -EIO;
	if (msnd_write_cfg_mem(cfg, num, mem))
		return -EIO;
	if (msnd_activate_logical(cfg, num))
		return -EIO;
	return 0;
}

typedef struct msnd_pinnacle_cfg_device {
	WORD io0, io1, irq;
	int mem;
} msnd_pinnacle_cfg_t[4];

static int __init msnd_pinnacle_cfg_devices(int cfg, int reset, msnd_pinnacle_cfg_t device)
{
	int i;

	/* Reset devices if told to */
	if (reset) {
		printk(KERN_INFO LOGNAME ": Resetting all devices\n");
		for (i = 0; i < 4; ++i)
			if (msnd_write_cfg_logical(cfg, i, 0, 0, 0, 0))
				return -EIO;
	}

	/* Configure specified devices */
	for (i = 0; i < 4; ++i) {

		switch (i) {
		case 0:		/* DSP */
			if (!(device[i].io0 && device[i].irq && device[i].mem))
				continue;
			break;
		case 1:		/* MPU */
			if (!(device[i].io0 && device[i].irq))
				continue;
			printk(KERN_INFO LOGNAME
			       ": Configuring MPU to I/O 0x%x IRQ %d\n",
			       device[i].io0, device[i].irq);
			break;
		case 2:		/* IDE */
			if (!(device[i].io0 && device[i].io1 && device[i].irq))
				continue;
			printk(KERN_INFO LOGNAME
			       ": Configuring IDE to I/O 0x%x, 0x%x IRQ %d\n",
			       device[i].io0, device[i].io1, device[i].irq);
			break;
		case 3:		/* Joystick */
			if (!(device[i].io0))
				continue;
			printk(KERN_INFO LOGNAME
			       ": Configuring joystick to I/O 0x%x\n",
			       device[i].io0);
			break;
		}

		/* Configure the device */
		if (msnd_write_cfg_logical(cfg, i, device[i].io0, device[i].io1, device[i].irq, device[i].mem))
			return -EIO;
	}

	return 0;
}
#endif

#ifdef MODULE
MODULE_AUTHOR				("Andrew Veliath <andrewtv@usa.net>");
MODULE_DESCRIPTION			("Turtle Beach " LONGNAME " Linux Driver");
MODULE_PARM				(io, "i");
MODULE_PARM				(irq, "i");
MODULE_PARM				(mem, "i");
MODULE_PARM				(write_ndelay, "i");
MODULE_PARM				(major, "i");
MODULE_PARM				(fifosize, "i");
MODULE_PARM				(calibrate_signal, "i");
#ifndef MSND_CLASSIC
MODULE_PARM				(digital, "i");
MODULE_PARM				(cfg, "i");
MODULE_PARM				(reset, "i");
MODULE_PARM				(mpu_io, "i");
MODULE_PARM				(mpu_irq, "i");
MODULE_PARM				(ide_io0, "i");
MODULE_PARM				(ide_io1, "i");
MODULE_PARM				(ide_irq, "i");
MODULE_PARM				(joystick_io, "i");
#endif

static int io __initdata =		-1;
static int irq __initdata =		-1;
static int mem __initdata =		-1;
static int write_ndelay __initdata =	-1;

#ifndef MSND_CLASSIC
/* Pinnacle/Fiji non-PnP Config Port */
static int cfg __initdata =		-1;

/* Extra Peripheral Configuration */
static int reset __initdata = 0;
static int mpu_io __initdata = 0;
static int mpu_irq __initdata = 0;
static int ide_io0 __initdata = 0;
static int ide_io1 __initdata = 0;
static int ide_irq __initdata = 0;
static int joystick_io __initdata = 0;

/* If we have the digital daugherboard... */
static int digital __initdata = 0;
#endif

static int fifosize __initdata =	DEFFIFOSIZE;
static int calibrate_signal __initdata = 0;

#else /* not a module */

static int write_ndelay __initdata =	-1;

#ifdef MSND_CLASSIC
static int io __initdata =		CONFIG_MSNDCLAS_IO;
static int irq __initdata =		CONFIG_MSNDCLAS_IRQ;
static int mem __initdata =		CONFIG_MSNDCLAS_MEM;
#else /* Pinnacle/Fiji */

static int io __initdata =		CONFIG_MSNDPIN_IO;
static int irq __initdata =		CONFIG_MSNDPIN_IRQ;
static int mem __initdata =		CONFIG_MSNDPIN_MEM;

/* Pinnacle/Fiji non-PnP Config Port */
#ifdef CONFIG_MSNDPIN_NONPNP
#  ifndef CONFIG_MSNDPIN_CFG
#    define CONFIG_MSNDPIN_CFG		0x250
#  endif
#else
#  ifdef CONFIG_MSNDPIN_CFG
#    undef CONFIG_MSNDPIN_CFG
#  endif
#  define CONFIG_MSNDPIN_CFG		-1
#endif
static int cfg __initdata =		CONFIG_MSNDPIN_CFG;
/* If not a module, we don't need to bother with reset=1 */
static int reset;

/* Extra Peripheral Configuration (Default: Disable) */
#ifndef CONFIG_MSNDPIN_MPU_IO
#  define CONFIG_MSNDPIN_MPU_IO		0
#endif
static int mpu_io __initdata =		CONFIG_MSNDPIN_MPU_IO;

#ifndef CONFIG_MSNDPIN_MPU_IRQ
#  define CONFIG_MSNDPIN_MPU_IRQ	0
#endif
static int mpu_irq __initdata =		CONFIG_MSNDPIN_MPU_IRQ;

#ifndef CONFIG_MSNDPIN_IDE_IO0
#  define CONFIG_MSNDPIN_IDE_IO0	0
#endif
static int ide_io0 __initdata =		CONFIG_MSNDPIN_IDE_IO0;

#ifndef CONFIG_MSNDPIN_IDE_IO1
#  define CONFIG_MSNDPIN_IDE_IO1	0
#endif
static int ide_io1 __initdata =		CONFIG_MSNDPIN_IDE_IO1;

#ifndef CONFIG_MSNDPIN_IDE_IRQ
#  define CONFIG_MSNDPIN_IDE_IRQ	0
#endif
static int ide_irq __initdata =		CONFIG_MSNDPIN_IDE_IRQ;

#ifndef CONFIG_MSNDPIN_JOYSTICK_IO
#  define CONFIG_MSNDPIN_JOYSTICK_IO	0
#endif
static int joystick_io __initdata =	CONFIG_MSNDPIN_JOYSTICK_IO;

/* Have SPDIF (Digital) Daughterboard */
#ifndef CONFIG_MSNDPIN_DIGITAL
#  define CONFIG_MSNDPIN_DIGITAL	0
#endif
static int digital __initdata =		CONFIG_MSNDPIN_DIGITAL;

#endif /* MSND_CLASSIC */

#ifndef CONFIG_MSND_FIFOSIZE
#  define CONFIG_MSND_FIFOSIZE		DEFFIFOSIZE
#endif
static int fifosize __initdata =	CONFIG_MSND_FIFOSIZE;

#ifndef CONFIG_MSND_CALSIGNAL
#  define CONFIG_MSND_CALSIGNAL		0
#endif
static int
calibrate_signal __initdata =		CONFIG_MSND_CALSIGNAL;
#endif /* MODULE */


static int __init msnd_init(void)
{
	int err;
#ifndef MSND_CLASSIC
	static msnd_pinnacle_cfg_t pinnacle_devs;
#endif /* MSND_CLASSIC */

	printk(KERN_INFO LOGNAME ": Turtle Beach " LONGNAME " Linux Driver Version "
	       VERSION ", Copyright (C) 1998 Andrew Veliath\n");

	if (io == -1 || irq == -1 || mem == -1)
		printk(KERN_WARNING LOGNAME ": io, irq and mem must be set\n");

	if (io == -1 ||
	    !(io == 0x290 ||
	      io == 0x260 ||
	      io == 0x250 ||
	      io == 0x240 ||
	      io == 0x230 ||
	      io == 0x220 ||
	      io == 0x210 ||
	      io == 0x3e0)) {
		printk(KERN_ERR LOGNAME ": \"io\" - DSP I/O base must be set to 0x210, 0x220, 0x230, 0x240, 0x250, 0x260, 0x290, or 0x3E0\n");
		return -EINVAL;
	}

	if (irq == -1 ||
	    !(irq == 5 ||
	      irq == 7 ||
	      irq == 9 ||
	      irq == 10 ||
	      irq == 11 ||
	      irq == 12)) {
		printk(KERN_ERR LOGNAME ": \"irq\" - must be set to 5, 7, 9, 10, 11 or 12\n");
		return -EINVAL;
	}

	if (mem == -1 ||
	    !(mem == 0xb0000 ||
	      mem == 0xc8000 ||
	      mem == 0xd0000 ||
	      mem == 0xd8000 ||
	      mem == 0xe0000 ||
	      mem == 0xe8000)) {
		printk(KERN_ERR LOGNAME ": \"mem\" - must be set to "
		       "0xb0000, 0xc8000, 0xd0000, 0xd8000, 0xe0000 or 0xe8000\n");
		return -EINVAL;
	}

#ifdef MSND_CLASSIC
	switch (irq) {
	case 5: dev.irqid = HPIRQ_5; break;
	case 7: dev.irqid = HPIRQ_7; break;
	case 9: dev.irqid = HPIRQ_9; break;
	case 10: dev.irqid = HPIRQ_10; break;
	case 11: dev.irqid = HPIRQ_11; break;
	case 12: dev.irqid = HPIRQ_12; break;
	}

	switch (mem) {
	case 0xb0000: dev.memid = HPMEM_B000; break;
	case 0xc8000: dev.memid = HPMEM_C800; break;
	case 0xd0000: dev.memid = HPMEM_D000; break;
	case 0xd8000: dev.memid = HPMEM_D800; break;
	case 0xe0000: dev.memid = HPMEM_E000; break;
	case 0xe8000: dev.memid = HPMEM_E800; break;
	}
#else
	if (cfg == -1) {
		printk(KERN_INFO LOGNAME ": Assuming PnP mode\n");
	} else if (cfg != 0x250 && cfg != 0x260 && cfg != 0x270) {
		printk(KERN_INFO LOGNAME ": Config port must be 0x250, 0x260 or 0x270 (or unspecified for PnP mode)\n");
		return -EINVAL;
	} else {
		printk(KERN_INFO LOGNAME ": Non-PnP mode: configuring at port 0x%x\n", cfg);

		/* DSP */
		pinnacle_devs[0].io0 = io;
		pinnacle_devs[0].irq = irq;
		pinnacle_devs[0].mem = mem;

		/* The following are Pinnacle specific */

		/* MPU */
		pinnacle_devs[1].io0 = mpu_io;
		pinnacle_devs[1].irq = mpu_irq;

		/* IDE */
		pinnacle_devs[2].io0 = ide_io0;
		pinnacle_devs[2].io1 = ide_io1;
		pinnacle_devs[2].irq = ide_irq;

		/* Joystick */
		pinnacle_devs[3].io0 = joystick_io;

		if (check_region(cfg, 2)) {
			printk(KERN_ERR LOGNAME ": Config port 0x%x conflict\n", cfg);
			return -EIO;
		}

		request_region(cfg, 2, "Pinnacle/Fiji Config");
		if (msnd_pinnacle_cfg_devices(cfg, reset, pinnacle_devs)) {
			printk(KERN_ERR LOGNAME ": Device configuration error\n");
			release_region(cfg, 2);
			return -EIO;
		}
		release_region(cfg, 2);
	}
#endif /* MSND_CLASSIC */

	if (fifosize < 16)
		fifosize = 16;

	if (fifosize > 1024)
		fifosize = 1024;

	set_default_audio_parameters();
#ifdef MSND_CLASSIC
	dev.type = msndClassic;
#else
	dev.type = msndPinnacle;
#endif
	dev.io = io;
	dev.numio = DSP_NUMIO;
	dev.irq = irq;
	dev.base = mem;
	dev.fifosize = fifosize * 1024;
	dev.calibrate_signal = calibrate_signal ? 1 : 0;
	dev.recsrc = 0;
	dev.dspq_data_buff = DSPQ_DATA_BUFF;
	dev.dspq_buff_size = DSPQ_BUFF_SIZE;
	if (write_ndelay == -1)
		write_ndelay = CONFIG_MSND_WRITE_NDELAY;
	if (write_ndelay)
		clear_bit(F_DISABLE_WRITE_NDELAY, &dev.flags);
	else
		set_bit(F_DISABLE_WRITE_NDELAY, &dev.flags);
#ifndef MSND_CLASSIC
	if (digital)
		set_bit(F_HAVEDIGITAL, &dev.flags);
#endif
	init_waitqueue_head(&dev.writeblock);
	init_waitqueue_head(&dev.readblock);
	init_waitqueue_head(&dev.writeflush);
	msnd_fifo_init(&dev.DAPF);
	msnd_fifo_init(&dev.DARF);
	spin_lock_init(&dev.lock);
	printk(KERN_INFO LOGNAME ": %u byte audio FIFOs (x2)\n", dev.fifosize);
	if ((err = msnd_fifo_alloc(&dev.DAPF, dev.fifosize)) < 0) {
		printk(KERN_ERR LOGNAME ": Couldn't allocate write FIFO\n");
		return err;
	}

	if ((err = msnd_fifo_alloc(&dev.DARF, dev.fifosize)) < 0) {
		printk(KERN_ERR LOGNAME ": Couldn't allocate read FIFO\n");
		msnd_fifo_free(&dev.DAPF);
		return err;
	}

	if ((err = probe_multisound()) < 0) {
		printk(KERN_ERR LOGNAME ": Probe failed\n");
		msnd_fifo_free(&dev.DAPF);
		msnd_fifo_free(&dev.DARF);
		return err;
	}

	if ((err = attach_multisound()) < 0) {
		printk(KERN_ERR LOGNAME ": Attach failed\n");
		msnd_fifo_free(&dev.DAPF);
		msnd_fifo_free(&dev.DARF);
		return err;
	}

	return 0;
}

static void __exit msdn_cleanup(void)
{
	unload_multisound();
	msnd_fifo_free(&dev.DAPF);
	msnd_fifo_free(&dev.DARF);
}

module_init(msnd_init);
module_exit(msdn_cleanup);
