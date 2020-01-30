/*
 *  Routines for Gravis UltraSound soundcards - Synthesizer
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
#include <linux/time.h>
#include <sound/core.h>
#include <sound/gus.h>

/*
 *
 */

int snd_gus_iwffff_put_sample(void *private_data, iwffff_wave_t *wave,
			      char __user *data, long len, int atomic)
{
	snd_gus_card_t *gus = snd_magic_cast(snd_gus_card_t, private_data, return -ENXIO);
	snd_gf1_mem_block_t *block;
	int err;

	if (wave->format & IWFFFF_WAVE_ROM)
		return 0;	/* it's probably ok - verify the address? */
	if (wave->format & IWFFFF_WAVE_STEREO)
		return -EINVAL;	/* not supported */
	block = snd_gf1_mem_alloc(&gus->gf1.mem_alloc,
				  SNDRV_GF1_MEM_OWNER_WAVE_IWFFFF,
				  NULL, wave->size,
				  wave->format & IWFFFF_WAVE_16BIT, 1,
				  wave->share_id);
	if (block == NULL)
		return -ENOMEM;
	err = snd_gus_dram_write(gus, data,
				 block->ptr, wave->size);
	if (err < 0) {
		snd_gf1_mem_lock(&gus->gf1.mem_alloc, 0);
		snd_gf1_mem_xfree(&gus->gf1.mem_alloc, block);
		snd_gf1_mem_lock(&gus->gf1.mem_alloc, 1);
		return err;
	}
	wave->address.memory = block->ptr;
	return 0;
}

int snd_gus_iwffff_get_sample(void *private_data, iwffff_wave_t *wave,
			      char __user *data, long len, int atomic)
{
	snd_gus_card_t *gus = snd_magic_cast(snd_gus_card_t, private_data, return -ENXIO);

	return snd_gus_dram_read(gus, data, wave->address.memory, wave->size,
				 wave->format & IWFFFF_WAVE_ROM ? 1 : 0);
}

int snd_gus_iwffff_remove_sample(void *private_data, iwffff_wave_t *wave,
				 int atomic)
{
	snd_gus_card_t *gus = snd_magic_cast(snd_gus_card_t, private_data, return -ENXIO);

	if (wave->format & IWFFFF_WAVE_ROM)
		return 0;	/* it's probably ok - verify the address? */	
	return snd_gf1_mem_free(&gus->gf1.mem_alloc, wave->address.memory);
}

/*
 *
 */

int snd_gus_gf1_put_sample(void *private_data, gf1_wave_t *wave,
			   char __user *data, long len, int atomic)
{
	snd_gus_card_t *gus = snd_magic_cast(snd_gus_card_t, private_data, return -ENXIO);
	snd_gf1_mem_block_t *block;
	int err;

	if (wave->format & GF1_WAVE_STEREO)
		return -EINVAL;	/* not supported */
	block = snd_gf1_mem_alloc(&gus->gf1.mem_alloc,
				  SNDRV_GF1_MEM_OWNER_WAVE_GF1,
				  NULL, wave->size,
				  wave->format & GF1_WAVE_16BIT, 1,
				  wave->share_id);
	if (block == NULL)
		return -ENOMEM;
	err = snd_gus_dram_write(gus, data,
				 block->ptr, wave->size);
	if (err < 0) {
		snd_gf1_mem_lock(&gus->gf1.mem_alloc, 0);
		snd_gf1_mem_xfree(&gus->gf1.mem_alloc, block);
		snd_gf1_mem_lock(&gus->gf1.mem_alloc, 1);
		return err;
	}
	wave->address.memory = block->ptr;
	return 0;
}

int snd_gus_gf1_get_sample(void *private_data, gf1_wave_t *wave,
			   char __user *data, long len, int atomic)
{
	snd_gus_card_t *gus = snd_magic_cast(snd_gus_card_t, private_data, return -ENXIO);

	return snd_gus_dram_read(gus, data, wave->address.memory, wave->size, 0);
}

int snd_gus_gf1_remove_sample(void *private_data, gf1_wave_t *wave,
			      int atomic)
{
	snd_gus_card_t *gus = snd_magic_cast(snd_gus_card_t, private_data, return -ENXIO);

	return snd_gf1_mem_free(&gus->gf1.mem_alloc, wave->address.memory);
}

/*
 *
 */

int snd_gus_simple_put_sample(void *private_data, simple_instrument_t *instr,
			      char __user *data, long len, int atomic)
{
	snd_gus_card_t *gus = snd_magic_cast(snd_gus_card_t, private_data, return -ENXIO);
	snd_gf1_mem_block_t *block;
	int err;

	if (instr->format & SIMPLE_WAVE_STEREO)
		return -EINVAL;	/* not supported */
	block = snd_gf1_mem_alloc(&gus->gf1.mem_alloc,
				  SNDRV_GF1_MEM_OWNER_WAVE_SIMPLE,
				  NULL, instr->size,
				  instr->format & SIMPLE_WAVE_16BIT, 1,
				  instr->share_id);
	if (block == NULL)
		return -ENOMEM;
	err = snd_gus_dram_write(gus, data, block->ptr, instr->size);
	if (err < 0) {
		snd_gf1_mem_lock(&gus->gf1.mem_alloc, 0);
		snd_gf1_mem_xfree(&gus->gf1.mem_alloc, block);
		snd_gf1_mem_lock(&gus->gf1.mem_alloc, 1);
		return err;
	}
	instr->address.memory = block->ptr;
	return 0;
}

int snd_gus_simple_get_sample(void *private_data, simple_instrument_t *instr,
			      char __user *data, long len, int atomic)
{
	snd_gus_card_t *gus = snd_magic_cast(snd_gus_card_t, private_data, return -ENXIO);

	return snd_gus_dram_read(gus, data, instr->address.memory, instr->size, 0);
}

int snd_gus_simple_remove_sample(void *private_data, simple_instrument_t *instr,
			         int atomic)
{
	snd_gus_card_t *gus = snd_magic_cast(snd_gus_card_t, private_data, return -ENXIO);

	return snd_gf1_mem_free(&gus->gf1.mem_alloc, instr->address.memory);
}
