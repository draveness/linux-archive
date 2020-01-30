/*
 * Driver for PowerMac onboard soundchips
 * Copyright (c) 2001 by Takashi Iwai <tiwai@suse.de>
 *   based on dmasound.c.
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
 */


#ifndef __PMAC_H
#define __PMAC_H

#include <linux/version.h>
#include <sound/control.h>
#include <sound/pcm.h>
#include "awacs.h"

#include <linux/adb.h>
#ifdef CONFIG_ADB_CUDA
#include <linux/cuda.h>
#endif
#ifdef CONFIG_ADB_PMU
#include <linux/pmu.h>
#endif
#include <linux/nvram.h>
#include <linux/tty.h>
#include <linux/vt_kern.h>
#include <asm/dbdma.h>
#include <asm/prom.h>
#include <asm/machdep.h>

/* maximum number of fragments */
#define PMAC_MAX_FRAGS		32


#define PMAC_SUPPORT_AUTOMUTE

/*
 * typedefs
 */
typedef struct snd_pmac pmac_t;
typedef struct snd_pmac_stream pmac_stream_t;
typedef struct snd_pmac_beep pmac_beep_t;
typedef struct snd_pmac_dbdma pmac_dbdma_t;


/*
 * DBDMA space
 */
struct snd_pmac_dbdma {
	unsigned long addr;
	struct dbdma_cmd *cmds;
	void *space;
	int size;
};

/*
 * playback/capture stream
 */
struct snd_pmac_stream {
	int running;	/* boolean */

	int stream;	/* PLAYBACK/CAPTURE */

	int dma_size; /* in bytes */
	int period_size; /* in bytes */
	int buffer_size; /* in kbytes */
	int nperiods, cur_period;

	pmac_dbdma_t cmd;
	volatile struct dbdma_regs *dma;

	snd_pcm_substream_t *substream;

	unsigned int cur_freqs;		/* currently available frequences */
	unsigned int cur_formats;	/* currently available formats */
};


/*
 */

enum snd_pmac_model {
	PMAC_AWACS, PMAC_SCREAMER, PMAC_BURGUNDY, PMAC_DACA, PMAC_TUMBLER, PMAC_SNAPPER
};

struct snd_pmac {
	snd_card_t *card;

	/* h/w info */
	struct device_node *node;
	unsigned int revision;
	unsigned int manufacturer;
	unsigned int subframe;
	unsigned int device_id;
	enum snd_pmac_model model;

	unsigned int has_iic : 1;
	unsigned int is_pbook_3400 : 1;
	unsigned int is_pbook_G3 : 1;

	unsigned int can_byte_swap : 1;
	unsigned int can_duplex : 1;
	unsigned int can_capture : 1;

	unsigned int auto_mute : 1;
	unsigned int initialized : 1;
	unsigned int feature_is_set : 1;

	unsigned int of_requested;

	int num_freqs;
	int *freq_table;
	unsigned int freqs_ok;		/* bit flags */
	unsigned int formats_ok;	/* pcm hwinfo */
	int active;
	int rate_index;
	int format;			/* current format */

	spinlock_t reg_lock;
	volatile struct awacs_regs *awacs;
	int awacs_reg[8]; /* register cache */
	unsigned int hp_stat_mask;

	unsigned char *latch_base;
	unsigned char *macio_base;

	pmac_stream_t playback;
	pmac_stream_t capture;

	pmac_dbdma_t extra_dma;

	int irq, tx_irq, rx_irq;

	snd_pcm_t *pcm;

	pmac_beep_t *beep;

	unsigned int control_mask;	/* control mask */

	/* mixer stuffs */
	void *mixer_data;
	void (*mixer_free)(pmac_t *);
	snd_kcontrol_t *master_sw_ctl;
	snd_kcontrol_t *speaker_sw_ctl;
	snd_kcontrol_t *hp_detect_ctl;

	/* lowlevel callbacks */
	void (*set_format)(pmac_t *chip);
	void (*update_automute)(pmac_t *chip, int do_notify);
	int (*detect_headphone)(pmac_t *chip);
#ifdef CONFIG_PMAC_PBOOK
	void (*suspend)(pmac_t *chip);
	void (*resume)(pmac_t *chip);
#endif

};


/* exported functions */
int snd_pmac_new(snd_card_t *card, pmac_t **chip_return);
int snd_pmac_pcm_new(pmac_t *chip);
int snd_pmac_attach_beep(pmac_t *chip);

/* initialize mixer */
int snd_pmac_awacs_init(pmac_t *chip);
int snd_pmac_burgundy_init(pmac_t *chip);
int snd_pmac_daca_init(pmac_t *chip);
int snd_pmac_tumbler_init(pmac_t *chip);
int snd_pmac_tumbler_post_init(void);

/* i2c functions */
typedef struct snd_pmac_keywest {
	int addr;
	struct i2c_client *client;
	int id;
	int (*init_client)(struct snd_pmac_keywest *i2c);
	char *name;
} pmac_keywest_t;

int snd_pmac_keywest_init(pmac_keywest_t *i2c);
void snd_pmac_keywest_cleanup(pmac_keywest_t *i2c);
#define snd_pmac_keywest_write(i2c,cmd,len,data) i2c_smbus_write_block_data((i2c)->client, cmd, len, data)
#define snd_pmac_keywest_write_byte(i2c,cmd,data) i2c_smbus_write_byte_data((i2c)->client, cmd, data)

/* misc */
int snd_pmac_boolean_stereo_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t *uinfo);
int snd_pmac_boolean_mono_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t *uinfo);

int snd_pmac_add_automute(pmac_t *chip);

#define big_mdelay(msec) do {\
	set_current_state(TASK_UNINTERRUPTIBLE);\
	schedule_timeout(((msec) * HZ + 999) / 1000);\
} while (0)

#ifndef PMAC_SUPPORT_PCM_BEEP
#define snd_pmac_attach_beep(chip) 0
#define snd_pmac_beep_stop(chip)  /**/
#endif

#endif /* __PMAC_H */
