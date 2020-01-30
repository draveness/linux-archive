/*
 * Functions for accessing OPL4 devices
 * Copyright (c) 2003 by Clemens Ladisch <clemens@ladisch.de>
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include "opl4_local.h"
#include <sound/initval.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <asm/io.h>

MODULE_AUTHOR("Clemens Ladisch <clemens@ladisch.de>");
MODULE_DESCRIPTION("OPL4 driver");
MODULE_LICENSE("GPL");
MODULE_CLASSES("{sound}");

static void inline snd_opl4_wait(opl4_t *opl4)
{
	int timeout = 10;
	while ((inb(opl4->fm_port) & OPL4_STATUS_BUSY) && --timeout > 0)
		;
}

void snd_opl4_write(opl4_t *opl4, u8 reg, u8 value)
{
	unsigned long flags;

	spin_lock_irqsave(&opl4->reg_lock, flags);

	snd_opl4_wait(opl4);
	outb(reg, opl4->pcm_port);

	snd_opl4_wait(opl4);
	outb(value, opl4->pcm_port + 1);

	spin_unlock_irqrestore(&opl4->reg_lock, flags);
}

u8 snd_opl4_read(opl4_t *opl4, u8 reg)
{
	unsigned long flags;
	u8 value;

	spin_lock_irqsave(&opl4->reg_lock, flags);

	snd_opl4_wait(opl4);
	outb(reg, opl4->pcm_port);

	snd_opl4_wait(opl4);
	value = inb(opl4->pcm_port + 1);

	spin_unlock_irqrestore(&opl4->reg_lock, flags);
	return value;
}

void snd_opl4_read_memory(opl4_t *opl4, char *buf, int offset, int size)
{
	u8 memcfg = snd_opl4_read(opl4, OPL4_REG_MEMORY_CONFIGURATION);
	snd_opl4_write(opl4, OPL4_REG_MEMORY_CONFIGURATION, memcfg | OPL4_MODE_BIT);

	snd_opl4_write(opl4, OPL4_REG_MEMORY_ADDRESS_HIGH, offset >> 16);
	snd_opl4_write(opl4, OPL4_REG_MEMORY_ADDRESS_MID, offset >> 8);
	snd_opl4_write(opl4, OPL4_REG_MEMORY_ADDRESS_LOW, offset);
	for (; size > 0; size--)
		*buf++ = snd_opl4_read(opl4, OPL4_REG_MEMORY_DATA);

	snd_opl4_write(opl4, OPL4_REG_MEMORY_CONFIGURATION, memcfg);
}

void snd_opl4_write_memory(opl4_t *opl4, const char *buf, int offset, int size)
{
	u8 memcfg = snd_opl4_read(opl4, OPL4_REG_MEMORY_CONFIGURATION);
	snd_opl4_write(opl4, OPL4_REG_MEMORY_CONFIGURATION, memcfg | OPL4_MODE_BIT);

	snd_opl4_write(opl4, OPL4_REG_MEMORY_ADDRESS_HIGH, offset >> 16);
	snd_opl4_write(opl4, OPL4_REG_MEMORY_ADDRESS_MID, offset >> 8);
	snd_opl4_write(opl4, OPL4_REG_MEMORY_ADDRESS_LOW, offset);
	for (; size > 0; size--)
		snd_opl4_write(opl4, OPL4_REG_MEMORY_DATA, *buf++);

	snd_opl4_write(opl4, OPL4_REG_MEMORY_CONFIGURATION, memcfg);
}

static void snd_opl4_enable_opl4(opl4_t *opl4)
{
	outb(OPL3_REG_MODE, opl4->fm_port + 2);
	inb(opl4->fm_port);
	inb(opl4->fm_port);
	outb(OPL3_OPL3_ENABLE | OPL3_OPL4_ENABLE, opl4->fm_port + 3);
	inb(opl4->fm_port);
	inb(opl4->fm_port);
}

static int snd_opl4_detect(opl4_t *opl4)
{
	u8 id1, id2;

	snd_opl4_enable_opl4(opl4);

	id1 = snd_opl4_read(opl4, OPL4_REG_MEMORY_CONFIGURATION);
	snd_printdd("OPL4[02]=%02x\n", id1);
	switch (id1 & OPL4_DEVICE_ID_MASK) {
	case 0x20:
		opl4->hardware = OPL3_HW_OPL4;
		break;
	case 0x40:
		opl4->hardware = OPL3_HW_OPL4_ML;
		break;
	default:
		return -ENODEV;
	}

	snd_opl4_write(opl4, OPL4_REG_MIX_CONTROL_FM, 0x00);
	snd_opl4_write(opl4, OPL4_REG_MIX_CONTROL_PCM, 0xff);
	id1 = snd_opl4_read(opl4, OPL4_REG_MIX_CONTROL_FM);
	id2 = snd_opl4_read(opl4, OPL4_REG_MIX_CONTROL_PCM);
	snd_printdd("OPL4 id1=%02x id2=%02x\n", id1, id2);
       	if (id1 != 0x00 || id2 != 0xff)
		return -ENODEV;

	snd_opl4_write(opl4, OPL4_REG_MIX_CONTROL_FM, 0x3f);
	snd_opl4_write(opl4, OPL4_REG_MIX_CONTROL_PCM, 0x3f);
	snd_opl4_write(opl4, OPL4_REG_MEMORY_CONFIGURATION, 0x00);
	return 0;
}

#if defined(CONFIG_SND_SEQUENCER) || (defined(MODULE) && defined(CONFIG_SND_SEQUENCER_MODULE))
static void snd_opl4_seq_dev_free(snd_seq_device_t *seq_dev)
{
	opl4_t *opl4 = snd_magic_cast(opl4_t, seq_dev->private_data, return);
	opl4->seq_dev = NULL;
}

static int snd_opl4_create_seq_dev(opl4_t *opl4, int seq_device)
{
	opl4->seq_dev_num = seq_device;
	if (snd_seq_device_new(opl4->card, seq_device, SNDRV_SEQ_DEV_ID_OPL4,
			       sizeof(opl4_t *), &opl4->seq_dev) >= 0) {
		strcpy(opl4->seq_dev->name, "OPL4 Wavetable");
		*(opl4_t **)SNDRV_SEQ_DEVICE_ARGPTR(opl4->seq_dev) = opl4;
		opl4->seq_dev->private_data = opl4;
		opl4->seq_dev->private_free = snd_opl4_seq_dev_free;
	}
	return 0;
}
#endif

static void snd_opl4_free(opl4_t *opl4)
{
#ifdef CONFIG_PROC_FS
	snd_opl4_free_proc(opl4);
#endif
	if (opl4->res_fm_port) {
		release_resource(opl4->res_fm_port);
		kfree_nocheck(opl4->res_fm_port);
	}
	if (opl4->res_pcm_port) {
		release_resource(opl4->res_pcm_port);
		kfree_nocheck(opl4->res_pcm_port);
	}
	snd_magic_kfree(opl4);
}

static int snd_opl4_dev_free(snd_device_t *device)
{
	opl4_t *opl4 = snd_magic_cast(opl4_t, device->device_data, return -ENXIO);
	snd_opl4_free(opl4);
	return 0;
}

int snd_opl4_create(snd_card_t *card,
		    unsigned long fm_port, unsigned long pcm_port,
		    int seq_device,
		    opl3_t **ropl3, opl4_t **ropl4)
{
	opl4_t *opl4;
	opl3_t *opl3;
	int err;
	static snd_device_ops_t ops = {
		.dev_free = snd_opl4_dev_free
	};

	if (ropl3)
		*ropl3 = NULL;
	if (ropl4)
		*ropl4 = NULL;

	opl4 = snd_magic_kcalloc(opl4_t, 0, GFP_KERNEL);
	if (!opl4)
		return -ENOMEM;

	opl4->res_fm_port = request_region(fm_port, 8, "OPL4 FM");
	opl4->res_pcm_port = request_region(pcm_port, 8, "OPL4 PCM/MIX");
	if (!opl4->res_fm_port || !opl4->res_pcm_port) {
		snd_printk(KERN_ERR "opl4: can't grab ports 0x%lx, 0x%lx\n", fm_port, pcm_port);
		snd_opl4_free(opl4);
		return -EBUSY;
	}

	opl4->card = card;
	opl4->fm_port = fm_port;
	opl4->pcm_port = pcm_port;
	spin_lock_init(&opl4->reg_lock);
	init_MUTEX(&opl4->access_mutex);

	err = snd_opl4_detect(opl4);
	if (err < 0) {
		snd_opl4_free(opl4);
		snd_printd("OPL4 chip not detected at %#lx/%#lx\n", fm_port, pcm_port);
		return err;
	}

	err = snd_opl3_create(card, fm_port, fm_port + 2, opl4->hardware, 1, &opl3);
	if (err < 0) {
		snd_opl4_free(opl4);
		return err;
	}

	/* opl3 initialization disabled opl4, so reenable */
	snd_opl4_enable_opl4(opl4); 

	err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, opl4, &ops);
	if (err < 0) {
		snd_device_free(card, opl3);
		snd_opl4_free(opl4);
		return err;
	}

	snd_opl4_create_mixer(opl4);
#ifdef CONFIG_PROC_FS
	snd_opl4_create_proc(opl4);
#endif

#if defined(CONFIG_SND_SEQUENCER) || (defined(MODULE) && defined(CONFIG_SND_SEQUENCER_MODULE))
	opl4->seq_client = -1;
	if (opl4->hardware < OPL3_HW_OPL4_ML)
		snd_opl4_create_seq_dev(opl4, seq_device);
#endif

	if (ropl3)
		*ropl3 = opl3;
	if (ropl4)
		*ropl4 = opl4;
	return 0;
}

EXPORT_SYMBOL(snd_opl4_write);
EXPORT_SYMBOL(snd_opl4_read);
EXPORT_SYMBOL(snd_opl4_write_memory);
EXPORT_SYMBOL(snd_opl4_read_memory);
EXPORT_SYMBOL(snd_opl4_create);

static int __init alsa_opl4_init(void)
{
	return 0;
}

static void __exit alsa_opl4_exit(void)
{
}

module_init(alsa_opl4_init)
module_exit(alsa_opl4_exit)
