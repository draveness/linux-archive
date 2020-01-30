/*
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
 *                   Creative Labs, Inc.
 *  Routines for control of EMU10K1 chips
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
#include <sound/core.h>
#include <sound/emu10k1.h>

unsigned int snd_emu10k1_ptr_read(emu10k1_t * emu, unsigned int reg, unsigned int chn)
{
	unsigned long flags;
	unsigned int regptr, val;
	unsigned int mask;

	mask = emu->audigy ? A_PTR_ADDRESS_MASK : PTR_ADDRESS_MASK;
	regptr = ((reg << 16) & mask) | (chn & PTR_CHANNELNUM_MASK);

	if (reg & 0xff000000) {
		unsigned char size, offset;
		
		size = (reg >> 24) & 0x3f;
		offset = (reg >> 16) & 0x1f;
		mask = ((1 << size) - 1) << offset;
		
		spin_lock_irqsave(&emu->emu_lock, flags);
		outl(regptr, emu->port + PTR);
		val = inl(emu->port + DATA);
		spin_unlock_irqrestore(&emu->emu_lock, flags);
		
		return (val & mask) >> offset;
	} else {
		spin_lock_irqsave(&emu->emu_lock, flags);
		outl(regptr, emu->port + PTR);
		val = inl(emu->port + DATA);
		spin_unlock_irqrestore(&emu->emu_lock, flags);
		return val;
	}
}

void snd_emu10k1_ptr_write(emu10k1_t *emu, unsigned int reg, unsigned int chn, unsigned int data)
{
	unsigned int regptr;
	unsigned long flags;
	unsigned int mask;

	mask = emu->audigy ? A_PTR_ADDRESS_MASK : PTR_ADDRESS_MASK;
	regptr = ((reg << 16) & mask) | (chn & PTR_CHANNELNUM_MASK);

	if (reg & 0xff000000) {
		unsigned char size, offset;

		size = (reg >> 24) & 0x3f;
		offset = (reg >> 16) & 0x1f;
		mask = ((1 << size) - 1) << offset;
		data = (data << offset) & mask;

		spin_lock_irqsave(&emu->emu_lock, flags);
		outl(regptr, emu->port + PTR);
		data |= inl(emu->port + DATA) & ~mask;
		outl(data, emu->port + DATA);
		spin_unlock_irqrestore(&emu->emu_lock, flags);		
	} else {
		spin_lock_irqsave(&emu->emu_lock, flags);
		outl(regptr, emu->port + PTR);
		outl(data, emu->port + DATA);
		spin_unlock_irqrestore(&emu->emu_lock, flags);
	}
}

void snd_emu10k1_intr_enable(emu10k1_t *emu, unsigned int intrenb)
{
	unsigned long flags;
	unsigned int enable;

	spin_lock_irqsave(&emu->emu_lock, flags);
	enable = inl(emu->port + INTE) | intrenb;
	outl(enable, emu->port + INTE);
	spin_unlock_irqrestore(&emu->emu_lock, flags);
}

void snd_emu10k1_intr_disable(emu10k1_t *emu, unsigned int intrenb)
{
	unsigned long flags;
	unsigned int enable;

	spin_lock_irqsave(&emu->emu_lock, flags);
	enable = inl(emu->port + INTE) & ~intrenb;
	outl(enable, emu->port + INTE);
	spin_unlock_irqrestore(&emu->emu_lock, flags);
}

void snd_emu10k1_voice_intr_enable(emu10k1_t *emu, unsigned int voicenum)
{
	unsigned long flags;
	unsigned int val;

	spin_lock_irqsave(&emu->emu_lock, flags);
	/* voice interrupt */
	if (voicenum >= 32) {
		outl(CLIEH << 16, emu->port + PTR);
		val = inl(emu->port + DATA);
		val |= 1 << (voicenum - 32);
	} else {
		outl(CLIEL << 16, emu->port + PTR);
		val = inl(emu->port + DATA);
		val |= 1 << voicenum;
	}
	outl(val, emu->port + DATA);
	spin_unlock_irqrestore(&emu->emu_lock, flags);
}

void snd_emu10k1_voice_intr_disable(emu10k1_t *emu, unsigned int voicenum)
{
	unsigned long flags;
	unsigned int val;

	spin_lock_irqsave(&emu->emu_lock, flags);
	/* voice interrupt */
	if (voicenum >= 32) {
		outl(CLIEH << 16, emu->port + PTR);
		val = inl(emu->port + DATA);
		val &= ~(1 << (voicenum - 32));
	} else {
		outl(CLIEL << 16, emu->port + PTR);
		val = inl(emu->port + DATA);
		val &= ~(1 << voicenum);
	}
	outl(val, emu->port + DATA);
	spin_unlock_irqrestore(&emu->emu_lock, flags);
}

void snd_emu10k1_voice_intr_ack(emu10k1_t *emu, unsigned int voicenum)
{
	unsigned long flags;

	spin_lock_irqsave(&emu->emu_lock, flags);
	/* voice interrupt */
	if (voicenum >= 32) {
		outl(CLIPH << 16, emu->port + PTR);
		voicenum = 1 << (voicenum - 32);
	} else {
		outl(CLIPL << 16, emu->port + PTR);
		voicenum = 1 << voicenum;
	}
	outl(voicenum, emu->port + DATA);
	spin_unlock_irqrestore(&emu->emu_lock, flags);
}

void snd_emu10k1_voice_set_loop_stop(emu10k1_t *emu, unsigned int voicenum)
{
	unsigned long flags;
	unsigned int sol;

	spin_lock_irqsave(&emu->emu_lock, flags);
	/* voice interrupt */
	if (voicenum >= 32) {
		outl(SOLEH << 16, emu->port + PTR);
		sol = inl(emu->port + DATA);
		sol |= 1 << (voicenum - 32);
	} else {
		outl(SOLEL << 16, emu->port + PTR);
		sol = inl(emu->port + DATA);
		sol |= 1 << voicenum;
	}
	outl(sol, emu->port + DATA);
	spin_unlock_irqrestore(&emu->emu_lock, flags);
}

void snd_emu10k1_voice_clear_loop_stop(emu10k1_t *emu, unsigned int voicenum)
{
	unsigned long flags;
	unsigned int sol;

	spin_lock_irqsave(&emu->emu_lock, flags);
	/* voice interrupt */
	if (voicenum >= 32) {
		outl(SOLEH << 16, emu->port + PTR);
		sol = inl(emu->port + DATA);
		sol &= ~(1 << (voicenum - 32));
	} else {
		outl(SOLEL << 16, emu->port + PTR);
		sol = inl(emu->port + DATA);
		sol &= ~(1 << voicenum);
	}
	outl(sol, emu->port + DATA);
	spin_unlock_irqrestore(&emu->emu_lock, flags);
}

void snd_emu10k1_wait(emu10k1_t *emu, unsigned int wait)
{
	volatile unsigned count;
	unsigned int newtime = 0, curtime;

	curtime = inl(emu->port + WC) >> 6;
	while (wait-- > 0) {
		count = 0;
		while (count++ < 16384) {
			newtime = inl(emu->port + WC) >> 6;
			if (newtime != curtime)
				break;
		}
		if (count >= 16384)
			break;
		curtime = newtime;
	}
}

unsigned short snd_emu10k1_ac97_read(ac97_t *ac97, unsigned short reg)
{
	emu10k1_t *emu = snd_magic_cast(emu10k1_t, ac97->private_data, return -ENXIO);
	unsigned long flags;
	unsigned short val;

	spin_lock_irqsave(&emu->emu_lock, flags);
	outb(reg, emu->port + AC97ADDRESS);
	val = inw(emu->port + AC97DATA);
	spin_unlock_irqrestore(&emu->emu_lock, flags);
	return val;
}

void snd_emu10k1_ac97_write(ac97_t *ac97, unsigned short reg, unsigned short data)
{
	emu10k1_t *emu = snd_magic_cast(emu10k1_t, ac97->private_data, return);
	unsigned long flags;

	spin_lock_irqsave(&emu->emu_lock, flags);
	outb(reg, emu->port + AC97ADDRESS);
	outw(data, emu->port + AC97DATA);
	spin_unlock_irqrestore(&emu->emu_lock, flags);
}

/*
 *  convert rate to pitch
 */

unsigned int snd_emu10k1_rate_to_pitch(unsigned int rate)
{
	static u32 logMagTable[128] = {
		0x00000, 0x02dfc, 0x05b9e, 0x088e6, 0x0b5d6, 0x0e26f, 0x10eb3, 0x13aa2,
		0x1663f, 0x1918a, 0x1bc84, 0x1e72e, 0x2118b, 0x23b9a, 0x2655d, 0x28ed5,
		0x2b803, 0x2e0e8, 0x30985, 0x331db, 0x359eb, 0x381b6, 0x3a93d, 0x3d081,
		0x3f782, 0x41e42, 0x444c1, 0x46b01, 0x49101, 0x4b6c4, 0x4dc49, 0x50191,
		0x5269e, 0x54b6f, 0x57006, 0x59463, 0x5b888, 0x5dc74, 0x60029, 0x623a7,
		0x646ee, 0x66a00, 0x68cdd, 0x6af86, 0x6d1fa, 0x6f43c, 0x7164b, 0x73829,
		0x759d4, 0x77b4f, 0x79c9a, 0x7bdb5, 0x7dea1, 0x7ff5e, 0x81fed, 0x8404e,
		0x86082, 0x88089, 0x8a064, 0x8c014, 0x8df98, 0x8fef1, 0x91e20, 0x93d26,
		0x95c01, 0x97ab4, 0x9993e, 0x9b79f, 0x9d5d9, 0x9f3ec, 0xa11d8, 0xa2f9d,
		0xa4d3c, 0xa6ab5, 0xa8808, 0xaa537, 0xac241, 0xadf26, 0xafbe7, 0xb1885,
		0xb3500, 0xb5157, 0xb6d8c, 0xb899f, 0xba58f, 0xbc15e, 0xbdd0c, 0xbf899,
		0xc1404, 0xc2f50, 0xc4a7b, 0xc6587, 0xc8073, 0xc9b3f, 0xcb5ed, 0xcd07c,
		0xceaec, 0xd053f, 0xd1f73, 0xd398a, 0xd5384, 0xd6d60, 0xd8720, 0xda0c3,
		0xdba4a, 0xdd3b4, 0xded03, 0xe0636, 0xe1f4e, 0xe384a, 0xe512c, 0xe69f3,
		0xe829f, 0xe9b31, 0xeb3a9, 0xecc08, 0xee44c, 0xefc78, 0xf148a, 0xf2c83,
		0xf4463, 0xf5c2a, 0xf73da, 0xf8b71, 0xfa2f0, 0xfba57, 0xfd1a7, 0xfe8df
	};
	static char logSlopeTable[128] = {
		0x5c, 0x5c, 0x5b, 0x5a, 0x5a, 0x59, 0x58, 0x58,
		0x57, 0x56, 0x56, 0x55, 0x55, 0x54, 0x53, 0x53,
		0x52, 0x52, 0x51, 0x51, 0x50, 0x50, 0x4f, 0x4f,
		0x4e, 0x4d, 0x4d, 0x4d, 0x4c, 0x4c, 0x4b, 0x4b,
		0x4a, 0x4a, 0x49, 0x49, 0x48, 0x48, 0x47, 0x47,
		0x47, 0x46, 0x46, 0x45, 0x45, 0x45, 0x44, 0x44,
		0x43, 0x43, 0x43, 0x42, 0x42, 0x42, 0x41, 0x41,
		0x41, 0x40, 0x40, 0x40, 0x3f, 0x3f, 0x3f, 0x3e,
		0x3e, 0x3e, 0x3d, 0x3d, 0x3d, 0x3c, 0x3c, 0x3c,
		0x3b, 0x3b, 0x3b, 0x3b, 0x3a, 0x3a, 0x3a, 0x39,
		0x39, 0x39, 0x39, 0x38, 0x38, 0x38, 0x38, 0x37,
		0x37, 0x37, 0x37, 0x36, 0x36, 0x36, 0x36, 0x35,
		0x35, 0x35, 0x35, 0x34, 0x34, 0x34, 0x34, 0x34,
		0x33, 0x33, 0x33, 0x33, 0x32, 0x32, 0x32, 0x32,
		0x32, 0x31, 0x31, 0x31, 0x31, 0x31, 0x30, 0x30,
		0x30, 0x30, 0x30, 0x2f, 0x2f, 0x2f, 0x2f, 0x2f
	};
	int i;

	if (rate == 0)
		return 0;	/* Bail out if no leading "1" */
	rate *= 11185;		/* Scale 48000 to 0x20002380 */
	for (i = 31; i > 0; i--) {
		if (rate & 0x80000000) {	/* Detect leading "1" */
			return (((unsigned int) (i - 15) << 20) +
			       logMagTable[0x7f & (rate >> 24)] +
					(0x7f & (rate >> 17)) *
					logSlopeTable[0x7f & (rate >> 24)]);
		}
		rate <<= 1;
	}

	return 0;		/* Should never reach this point */
}

/*
 *  Returns an attenuation based upon a cumulative volume value
 *  Algorithm calculates 0x200 - 0x10 log2 (input)
 */
 
unsigned char snd_emu10k1_sum_vol_attn(unsigned int value)
{
	unsigned short count = 16, ans;

	if (value == 0)
		return 0xFF;

	/* Find first SET bit. This is the integer part of the value */
	while ((value & 0x10000) == 0) {
		value <<= 1;
		count--;
	}

	/* The REST of the data is the fractional part. */
	ans = (unsigned short) (0x110 - ((count << 4) + ((value & 0x0FFFFL) >> 12)));
	if (ans > 0xFF)
		ans = 0xFF;

	return (unsigned char) ans;
}
