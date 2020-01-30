/*
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
 *                   Abramo Bagnara <abramo@alsa-project.org>
 *                   Cirrus Logic, Inc.
 *  Routines for control of Cirrus Logic CS461x chips
 *
 *  KNOWN BUGS:
 *    - Sometimes the SPDIF input DSP tasks get's unsynchronized
 *      and the SPDIF get somewhat "distorcionated", or/and left right channel
 *      are swapped. To get around this problem when it happens, mute and unmute 
 *      the SPDIF input mixer controll.
 *    - On the Hercules Game Theater XP the amplifier are sometimes turned
 *      off on inadecuate moments which causes distorcions on sound.
 *
 *  TODO:
 *    - Secondary CODEC on some soundcards
 *    - SPDIF input support for other sample rates then 48khz
 *    - Posibility to mix the SPDIF output with analog sources.
 *    - PCM channels for Center and LFE on secondary codec
 *
 *  NOTE: with CONFIG_SND_CS46XX_NEW_DSP unset uses old DSP image (which
 *        is default configuration), no SPDIF, no secondary codec, no
 *        multi channel PCM.  But known to work.
 *
 *  FINALLY: A credit to the developers Tom and Jordan 
 *           at Cirrus for have helping me out with the DSP, however we
 *           still don't have sufficient documentation and technical
 *           references to be able to implement all fancy feutures
 *           supported by the cs46xx DSP's. 
 *           Benny <benny@hostmobility.com>
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
#include <linux/pci.h>
#include <linux/pm.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/gameport.h>

#include <sound/core.h>
#include <sound/control.h>
#include <sound/info.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/cs46xx.h>

#include <asm/io.h>

#include "cs46xx_lib.h"
#include "dsp_spos.h"

static void amp_voyetra(cs46xx_t *chip, int change);

static unsigned short snd_cs46xx_codec_read(cs46xx_t *chip,
					    unsigned short reg,
					    int codec_index)
{
	int count;
	unsigned short result,tmp;
	u32 offset = 0;
	snd_assert ( (codec_index == CS46XX_PRIMARY_CODEC_INDEX) ||
		     (codec_index == CS46XX_SECONDARY_CODEC_INDEX),
		     return -EINVAL);

	if (codec_index == CS46XX_SECONDARY_CODEC_INDEX)
		offset = CS46XX_SECONDARY_CODEC_OFFSET;

	/*
	 *  1. Write ACCAD = Command Address Register = 46Ch for AC97 register address
	 *  2. Write ACCDA = Command Data Register = 470h    for data to write to AC97 
	 *  3. Write ACCTL = Control Register = 460h for initiating the write7---55
	 *  4. Read ACCTL = 460h, DCV should be reset by now and 460h = 17h
	 *  5. if DCV not cleared, break and return error
	 *  6. Read ACSTS = Status Register = 464h, check VSTS bit
	 */

	snd_cs46xx_peekBA0(chip, BA0_ACSDA + offset);

	tmp = snd_cs46xx_peekBA0(chip, BA0_ACCTL);
	if ((tmp & ACCTL_VFRM) == 0) {
		snd_printk(KERN_WARNING  "cs46xx: ACCTL_VFRM not set 0x%x\n",tmp);
		snd_cs46xx_pokeBA0(chip, BA0_ACCTL, (tmp & (~ACCTL_ESYN)) | ACCTL_VFRM );
		mdelay(50);
		tmp = snd_cs46xx_peekBA0(chip, BA0_ACCTL + offset);
		snd_cs46xx_pokeBA0(chip, BA0_ACCTL, tmp | ACCTL_ESYN | ACCTL_VFRM );

	}

	/*
	 *  Setup the AC97 control registers on the CS461x to send the
	 *  appropriate command to the AC97 to perform the read.
	 *  ACCAD = Command Address Register = 46Ch
	 *  ACCDA = Command Data Register = 470h
	 *  ACCTL = Control Register = 460h
	 *  set DCV - will clear when process completed
	 *  set CRW - Read command
	 *  set VFRM - valid frame enabled
	 *  set ESYN - ASYNC generation enabled
	 *  set RSTN - ARST# inactive, AC97 codec not reset
	 */

	snd_cs46xx_pokeBA0(chip, BA0_ACCAD, reg);
	snd_cs46xx_pokeBA0(chip, BA0_ACCDA, 0);
	if (codec_index == CS46XX_PRIMARY_CODEC_INDEX) {
		snd_cs46xx_pokeBA0(chip, BA0_ACCTL,/* clear ACCTL_DCV */ ACCTL_CRW | 
				   ACCTL_VFRM | ACCTL_ESYN |
				   ACCTL_RSTN);
		snd_cs46xx_pokeBA0(chip, BA0_ACCTL, ACCTL_DCV | ACCTL_CRW |
				   ACCTL_VFRM | ACCTL_ESYN |
				   ACCTL_RSTN);
	} else {
		snd_cs46xx_pokeBA0(chip, BA0_ACCTL, ACCTL_DCV | ACCTL_TC |
				   ACCTL_CRW | ACCTL_VFRM | ACCTL_ESYN |
				   ACCTL_RSTN);
	}

	/*
	 *  Wait for the read to occur.
	 */
	for (count = 0; count < 1000; count++) {
		/*
		 *  First, we want to wait for a short time.
	 	 */
		udelay(10);
		/*
		 *  Now, check to see if the read has completed.
		 *  ACCTL = 460h, DCV should be reset by now and 460h = 17h
		 */
		if (!(snd_cs46xx_peekBA0(chip, BA0_ACCTL) & ACCTL_DCV))
			goto ok1;
	}

	snd_printk("AC'97 read problem (ACCTL_DCV), reg = 0x%x\n", reg);
	result = 0xffff;
	goto end;
	
 ok1:
	/*
	 *  Wait for the valid status bit to go active.
	 */
	for (count = 0; count < 100; count++) {
		/*
		 *  Read the AC97 status register.
		 *  ACSTS = Status Register = 464h
		 *  VSTS - Valid Status
		 */
		if (snd_cs46xx_peekBA0(chip, BA0_ACSTS + offset) & ACSTS_VSTS)
			goto ok2;
		udelay(10);
	}
	
	snd_printk("AC'97 read problem (ACSTS_VSTS), codec_index %d, reg = 0x%x\n", codec_index, reg);
	result = 0xffff;
	goto end;

 ok2:
	/*
	 *  Read the data returned from the AC97 register.
	 *  ACSDA = Status Data Register = 474h
	 */
#if 0
	printk("e) reg = 0x%x, val = 0x%x, BA0_ACCAD = 0x%x\n", reg,
			snd_cs46xx_peekBA0(chip, BA0_ACSDA),
			snd_cs46xx_peekBA0(chip, BA0_ACCAD));
#endif

	//snd_cs46xx_peekBA0(chip, BA0_ACCAD);
	result = snd_cs46xx_peekBA0(chip, BA0_ACSDA + offset);
 end:
	return result;
}

static unsigned short snd_cs46xx_ac97_read(ac97_t * ac97,
					    unsigned short reg)
{
	cs46xx_t *chip = snd_magic_cast(cs46xx_t, ac97->private_data, return -ENXIO);
	unsigned short val;
	int codec_index = -1;

	/* UGGLY: nr_ac97_codecs == 0 primery codec detection is in progress */
	if (ac97 == chip->ac97[CS46XX_PRIMARY_CODEC_INDEX] || chip->nr_ac97_codecs == 0)
		codec_index = CS46XX_PRIMARY_CODEC_INDEX;
	/* UGGLY: nr_ac97_codecs == 1 secondary codec detection is in progress */
	else if (ac97 == chip->ac97[CS46XX_SECONDARY_CODEC_INDEX] || chip->nr_ac97_codecs == 1)
		codec_index = CS46XX_SECONDARY_CODEC_INDEX;
	else
		snd_assert(0, return 0xffff);
	chip->active_ctrl(chip, 1);
	val = snd_cs46xx_codec_read(chip, reg, codec_index);
	chip->active_ctrl(chip, -1);

	/* HACK: voyetra uses EAPD bit in the reverse way.
	 * we flip the bit to show the mixer status correctly
	 */
	if (reg == AC97_POWERDOWN && chip->amplifier_ctrl == amp_voyetra)
		val ^= 0x8000;

	return val;
}


static void snd_cs46xx_codec_write(cs46xx_t *chip,
				   unsigned short reg,
				   unsigned short val,
				   int codec_index)
{
	int count;

	snd_assert ((codec_index == CS46XX_PRIMARY_CODEC_INDEX) ||
		    (codec_index == CS46XX_SECONDARY_CODEC_INDEX),
		    return);

	/*
	 *  1. Write ACCAD = Command Address Register = 46Ch for AC97 register address
	 *  2. Write ACCDA = Command Data Register = 470h    for data to write to AC97
	 *  3. Write ACCTL = Control Register = 460h for initiating the write
	 *  4. Read ACCTL = 460h, DCV should be reset by now and 460h = 07h
	 *  5. if DCV not cleared, break and return error
	 */

	/*
	 *  Setup the AC97 control registers on the CS461x to send the
	 *  appropriate command to the AC97 to perform the read.
	 *  ACCAD = Command Address Register = 46Ch
	 *  ACCDA = Command Data Register = 470h
	 *  ACCTL = Control Register = 460h
	 *  set DCV - will clear when process completed
	 *  reset CRW - Write command
	 *  set VFRM - valid frame enabled
	 *  set ESYN - ASYNC generation enabled
	 *  set RSTN - ARST# inactive, AC97 codec not reset
         */
	snd_cs46xx_pokeBA0(chip, BA0_ACCAD , reg);
	snd_cs46xx_pokeBA0(chip, BA0_ACCDA , val);
	snd_cs46xx_peekBA0(chip, BA0_ACCTL);

	if (codec_index == CS46XX_PRIMARY_CODEC_INDEX) {
		snd_cs46xx_pokeBA0(chip, BA0_ACCTL, /* clear ACCTL_DCV */ ACCTL_VFRM |
				   ACCTL_ESYN | ACCTL_RSTN);
		snd_cs46xx_pokeBA0(chip, BA0_ACCTL, ACCTL_DCV | ACCTL_VFRM |
				   ACCTL_ESYN | ACCTL_RSTN);
	} else {
		snd_cs46xx_pokeBA0(chip, BA0_ACCTL, ACCTL_DCV | ACCTL_TC |
				   ACCTL_VFRM | ACCTL_ESYN | ACCTL_RSTN);
	}

	for (count = 0; count < 4000; count++) {
		/*
		 *  First, we want to wait for a short time.
		 */
		udelay(10);
		/*
		 *  Now, check to see if the write has completed.
		 *  ACCTL = 460h, DCV should be reset by now and 460h = 07h
		 */
		if (!(snd_cs46xx_peekBA0(chip, BA0_ACCTL) & ACCTL_DCV)) {
			return;
		}
	}
	snd_printk("AC'97 write problem, codec_index = %d, reg = 0x%x, val = 0x%x\n", codec_index, reg, val);
}

static void snd_cs46xx_ac97_write(ac97_t *ac97,
				   unsigned short reg,
				   unsigned short val)
{
	cs46xx_t *chip = snd_magic_cast(cs46xx_t, ac97->private_data, return);
	int codec_index = -1;

	/* UGGLY: nr_ac97_codecs == 0 primery codec detection is in progress */
	if (ac97 == chip->ac97[CS46XX_PRIMARY_CODEC_INDEX] || chip->nr_ac97_codecs == 0)
		codec_index = CS46XX_PRIMARY_CODEC_INDEX;
	/* UGGLY: nr_ac97_codecs == 1 secondary codec detection is in progress */
	else  if (ac97 == chip->ac97[CS46XX_SECONDARY_CODEC_INDEX] || chip->nr_ac97_codecs == 1)
		codec_index = CS46XX_SECONDARY_CODEC_INDEX;
	else
		snd_assert(0,return);

	/* HACK: voyetra uses EAPD bit in the reverse way.
	 * we flip the bit to show the mixer status correctly
	 */
	if (reg == AC97_POWERDOWN && chip->amplifier_ctrl == amp_voyetra)
		val ^= 0x8000;

	chip->active_ctrl(chip, 1);
	snd_cs46xx_codec_write(chip, reg, val, codec_index);
	chip->active_ctrl(chip, -1);
}


/*
 *  Chip initialization
 */

int snd_cs46xx_download(cs46xx_t *chip,
			u32 *src,
                        unsigned long offset,
                        unsigned long len)
{
	unsigned long dst;
	unsigned int bank = offset >> 16;
	offset = offset & 0xffff;

	snd_assert(!(offset & 3) && !(len & 3), return -EINVAL);
	dst = chip->region.idx[bank+1].remap_addr + offset;
	len /= sizeof(u32);

	/* writel already converts 32-bit value to right endianess */
	while (len-- > 0) {
		writel(*src++, dst);
		dst += sizeof(u32);
	}
	return 0;
}

#ifdef CONFIG_SND_CS46XX_NEW_DSP

#include "imgs/cwc4630.h"
#include "imgs/cwcasync.h"
#include "imgs/cwcsnoop.h"
#include "imgs/cwcbinhack.h"
#include "imgs/cwcdma.h"

int snd_cs46xx_clear_BA1(cs46xx_t *chip,
                         unsigned long offset,
                         unsigned long len) 
{
	unsigned long dst;
	unsigned int bank = offset >> 16;
	offset = offset & 0xffff;

	snd_assert(!(offset & 3) && !(len & 3), return -EINVAL);
	dst = chip->region.idx[bank+1].remap_addr + offset;
	len /= sizeof(u32);

	/* writel already converts 32-bit value to right endianess */
	while (len-- > 0) {
		writel(0, dst);
		dst += sizeof(u32);
	}
	return 0;
}

#else /* old DSP image */

#include "cs46xx_image.h"

int snd_cs46xx_download_image(cs46xx_t *chip)
{
	int idx, err;
	unsigned long offset = 0;

	for (idx = 0; idx < BA1_MEMORY_COUNT; idx++) {
		if ((err = snd_cs46xx_download(chip,
					       &BA1Struct.map[offset],
					       BA1Struct.memory[idx].offset,
					       BA1Struct.memory[idx].size)) < 0)
			return err;
		offset += BA1Struct.memory[idx].size >> 2;
	}	
	return 0;
}
#endif /* CONFIG_SND_CS46XX_NEW_DSP */

/*
 *  Chip reset
 */

static void snd_cs46xx_reset(cs46xx_t *chip)
{
	int idx;

	/*
	 *  Write the reset bit of the SP control register.
	 */
	snd_cs46xx_poke(chip, BA1_SPCR, SPCR_RSTSP);

	/*
	 *  Write the control register.
	 */
	snd_cs46xx_poke(chip, BA1_SPCR, SPCR_DRQEN);

	/*
	 *  Clear the trap registers.
	 */
	for (idx = 0; idx < 8; idx++) {
		snd_cs46xx_poke(chip, BA1_DREG, DREG_REGID_TRAP_SELECT + idx);
		snd_cs46xx_poke(chip, BA1_TWPR, 0xFFFF);
	}
	snd_cs46xx_poke(chip, BA1_DREG, 0);

	/*
	 *  Set the frame timer to reflect the number of cycles per frame.
	 */
	snd_cs46xx_poke(chip, BA1_FRMT, 0xadf);
}

static int cs46xx_wait_for_fifo(cs46xx_t * chip,int retry_timeout) 
{
	u32 i, status = 0;
	/*
	 * Make sure the previous FIFO write operation has completed.
	 */
	for(i = 0; i < 50; i++){
		status = snd_cs46xx_peekBA0(chip, BA0_SERBST);
    
		if( !(status & SERBST_WBSY) )
			break;

		mdelay(retry_timeout);
	}
  
	if(status & SERBST_WBSY) {
		snd_printk( KERN_ERR "cs46xx: failure waiting for FIFO command to complete\n");

		return -EINVAL;
	}

	return 0;
}

static void snd_cs46xx_clear_serial_FIFOs(cs46xx_t *chip)
{
	int idx, powerdown = 0;
	unsigned int tmp;

	/*
	 *  See if the devices are powered down.  If so, we must power them up first
	 *  or they will not respond.
	 */
	tmp = snd_cs46xx_peekBA0(chip, BA0_CLKCR1);
	if (!(tmp & CLKCR1_SWCE)) {
		snd_cs46xx_pokeBA0(chip, BA0_CLKCR1, tmp | CLKCR1_SWCE);
		powerdown = 1;
	}

	/*
	 *  We want to clear out the serial port FIFOs so we don't end up playing
	 *  whatever random garbage happens to be in them.  We fill the sample FIFOS
	 *  with zero (silence).
	 */
	snd_cs46xx_pokeBA0(chip, BA0_SERBWP, 0);

	/*
	 *  Fill all 256 sample FIFO locations.
	 */
	for (idx = 0; idx < 0xFF; idx++) {
		/*
		 *  Make sure the previous FIFO write operation has completed.
		 */
		if (cs46xx_wait_for_fifo(chip,1)) {
			snd_printdd ("failed waiting for FIFO at addr (%02X)\n",idx);

			if (powerdown)
				snd_cs46xx_pokeBA0(chip, BA0_CLKCR1, tmp);
          
			break;
		}
		/*
		 *  Write the serial port FIFO index.
		 */
		snd_cs46xx_pokeBA0(chip, BA0_SERBAD, idx);
		/*
		 *  Tell the serial port to load the new value into the FIFO location.
		 */
		snd_cs46xx_pokeBA0(chip, BA0_SERBCM, SERBCM_WRC);
	}
	/*
	 *  Now, if we powered up the devices, then power them back down again.
	 *  This is kinda ugly, but should never happen.
	 */
	if (powerdown)
		snd_cs46xx_pokeBA0(chip, BA0_CLKCR1, tmp);
}

static void snd_cs46xx_proc_start(cs46xx_t *chip)
{
	int cnt;

	/*
	 *  Set the frame timer to reflect the number of cycles per frame.
	 */
	snd_cs46xx_poke(chip, BA1_FRMT, 0xadf);
	/*
	 *  Turn on the run, run at frame, and DMA enable bits in the local copy of
	 *  the SP control register.
	 */
	snd_cs46xx_poke(chip, BA1_SPCR, SPCR_RUN | SPCR_RUNFR | SPCR_DRQEN);
	/*
	 *  Wait until the run at frame bit resets itself in the SP control
	 *  register.
	 */
	for (cnt = 0; cnt < 25; cnt++) {
		udelay(50);
		if (!(snd_cs46xx_peek(chip, BA1_SPCR) & SPCR_RUNFR))
			break;
	}

	if (snd_cs46xx_peek(chip, BA1_SPCR) & SPCR_RUNFR)
		snd_printk("SPCR_RUNFR never reset\n");
}

static void snd_cs46xx_proc_stop(cs46xx_t *chip)
{
	/*
	 *  Turn off the run, run at frame, and DMA enable bits in the local copy of
	 *  the SP control register.
	 */
	snd_cs46xx_poke(chip, BA1_SPCR, 0);
}

/*
 *  Sample rate routines
 */

#define GOF_PER_SEC 200

static void snd_cs46xx_set_play_sample_rate(cs46xx_t *chip, unsigned int rate)
{
	unsigned long flags;
	unsigned int tmp1, tmp2;
	unsigned int phiIncr;
	unsigned int correctionPerGOF, correctionPerSec;

	/*
	 *  Compute the values used to drive the actual sample rate conversion.
	 *  The following formulas are being computed, using inline assembly
	 *  since we need to use 64 bit arithmetic to compute the values:
	 *
	 *  phiIncr = floor((Fs,in * 2^26) / Fs,out)
	 *  correctionPerGOF = floor((Fs,in * 2^26 - Fs,out * phiIncr) /
         *                                   GOF_PER_SEC)
         *  ulCorrectionPerSec = Fs,in * 2^26 - Fs,out * phiIncr -M
         *                       GOF_PER_SEC * correctionPerGOF
	 *
	 *  i.e.
	 *
	 *  phiIncr:other = dividend:remainder((Fs,in * 2^26) / Fs,out)
	 *  correctionPerGOF:correctionPerSec =
	 *      dividend:remainder(ulOther / GOF_PER_SEC)
	 */
	tmp1 = rate << 16;
	phiIncr = tmp1 / 48000;
	tmp1 -= phiIncr * 48000;
	tmp1 <<= 10;
	phiIncr <<= 10;
	tmp2 = tmp1 / 48000;
	phiIncr += tmp2;
	tmp1 -= tmp2 * 48000;
	correctionPerGOF = tmp1 / GOF_PER_SEC;
	tmp1 -= correctionPerGOF * GOF_PER_SEC;
	correctionPerSec = tmp1;

	/*
	 *  Fill in the SampleRateConverter control block.
	 */
	spin_lock_irqsave(&chip->reg_lock, flags);
	snd_cs46xx_poke(chip, BA1_PSRC,
	  ((correctionPerSec << 16) & 0xFFFF0000) | (correctionPerGOF & 0xFFFF));
	snd_cs46xx_poke(chip, BA1_PPI, phiIncr);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

static void snd_cs46xx_set_capture_sample_rate(cs46xx_t *chip, unsigned int rate)
{
	unsigned long flags;
	unsigned int phiIncr, coeffIncr, tmp1, tmp2;
	unsigned int correctionPerGOF, correctionPerSec, initialDelay;
	unsigned int frameGroupLength, cnt;

	/*
	 *  We can only decimate by up to a factor of 1/9th the hardware rate.
	 *  Correct the value if an attempt is made to stray outside that limit.
	 */
	if ((rate * 9) < 48000)
		rate = 48000 / 9;

	/*
	 *  We can not capture at at rate greater than the Input Rate (48000).
	 *  Return an error if an attempt is made to stray outside that limit.
	 */
	if (rate > 48000)
		rate = 48000;

	/*
	 *  Compute the values used to drive the actual sample rate conversion.
	 *  The following formulas are being computed, using inline assembly
	 *  since we need to use 64 bit arithmetic to compute the values:
	 *
	 *     coeffIncr = -floor((Fs,out * 2^23) / Fs,in)
	 *     phiIncr = floor((Fs,in * 2^26) / Fs,out)
	 *     correctionPerGOF = floor((Fs,in * 2^26 - Fs,out * phiIncr) /
	 *                                GOF_PER_SEC)
	 *     correctionPerSec = Fs,in * 2^26 - Fs,out * phiIncr -
	 *                          GOF_PER_SEC * correctionPerGOF
	 *     initialDelay = ceil((24 * Fs,in) / Fs,out)
	 *
	 * i.e.
	 *
	 *     coeffIncr = neg(dividend((Fs,out * 2^23) / Fs,in))
	 *     phiIncr:ulOther = dividend:remainder((Fs,in * 2^26) / Fs,out)
	 *     correctionPerGOF:correctionPerSec =
	 * 	    dividend:remainder(ulOther / GOF_PER_SEC)
	 *     initialDelay = dividend(((24 * Fs,in) + Fs,out - 1) / Fs,out)
	 */

	tmp1 = rate << 16;
	coeffIncr = tmp1 / 48000;
	tmp1 -= coeffIncr * 48000;
	tmp1 <<= 7;
	coeffIncr <<= 7;
	coeffIncr += tmp1 / 48000;
	coeffIncr ^= 0xFFFFFFFF;
	coeffIncr++;
	tmp1 = 48000 << 16;
	phiIncr = tmp1 / rate;
	tmp1 -= phiIncr * rate;
	tmp1 <<= 10;
	phiIncr <<= 10;
	tmp2 = tmp1 / rate;
	phiIncr += tmp2;
	tmp1 -= tmp2 * rate;
	correctionPerGOF = tmp1 / GOF_PER_SEC;
	tmp1 -= correctionPerGOF * GOF_PER_SEC;
	correctionPerSec = tmp1;
	initialDelay = ((48000 * 24) + rate - 1) / rate;

	/*
	 *  Fill in the VariDecimate control block.
	 */
	spin_lock_irqsave(&chip->reg_lock, flags);
	snd_cs46xx_poke(chip, BA1_CSRC,
		((correctionPerSec << 16) & 0xFFFF0000) | (correctionPerGOF & 0xFFFF));
	snd_cs46xx_poke(chip, BA1_CCI, coeffIncr);
	snd_cs46xx_poke(chip, BA1_CD,
		(((BA1_VARIDEC_BUF_1 + (initialDelay << 2)) << 16) & 0xFFFF0000) | 0x80);
	snd_cs46xx_poke(chip, BA1_CPI, phiIncr);
	spin_unlock_irqrestore(&chip->reg_lock, flags);

	/*
	 *  Figure out the frame group length for the write back task.  Basically,
	 *  this is just the factors of 24000 (2^6*3*5^3) that are not present in
	 *  the output sample rate.
	 */
	frameGroupLength = 1;
	for (cnt = 2; cnt <= 64; cnt *= 2) {
		if (((rate / cnt) * cnt) != rate)
			frameGroupLength *= 2;
	}
	if (((rate / 3) * 3) != rate) {
		frameGroupLength *= 3;
	}
	for (cnt = 5; cnt <= 125; cnt *= 5) {
		if (((rate / cnt) * cnt) != rate) 
			frameGroupLength *= 5;
        }

	/*
	 * Fill in the WriteBack control block.
	 */
	spin_lock_irqsave(&chip->reg_lock, flags);
	snd_cs46xx_poke(chip, BA1_CFG1, frameGroupLength);
	snd_cs46xx_poke(chip, BA1_CFG2, (0x00800000 | frameGroupLength));
	snd_cs46xx_poke(chip, BA1_CCST, 0x0000FFFF);
	snd_cs46xx_poke(chip, BA1_CSPB, ((65536 * rate) / 24000));
	snd_cs46xx_poke(chip, (BA1_CSPB + 4), 0x0000FFFF);
	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

/*
 *  PCM part
 */

static int snd_cs46xx_playback_transfer(snd_pcm_substream_t *substream)
{
	/* cs46xx_t *chip = snd_pcm_substream_chip(substream); */
	snd_pcm_runtime_t *runtime = substream->runtime;
	cs46xx_pcm_t * cpcm = snd_magic_cast(cs46xx_pcm_t, runtime->private_data, return -ENXIO);
	snd_pcm_uframes_t appl_ptr = runtime->control->appl_ptr;
	snd_pcm_sframes_t diff = appl_ptr - cpcm->appl_ptr;
	int buffer_size = runtime->period_size * CS46XX_FRAGS << cpcm->shift;

	if (diff) {
		if (diff < -(snd_pcm_sframes_t) (runtime->boundary / 2))
			diff += runtime->boundary;
		cpcm->sw_ready += diff * (1 << cpcm->shift);
		cpcm->appl_ptr = appl_ptr;
	}
	while (cpcm->hw_ready < buffer_size && 
	       cpcm->sw_ready > 0) {
		size_t hw_to_end = buffer_size - cpcm->hw_data;
		size_t sw_to_end = cpcm->sw_bufsize - cpcm->sw_data;
		size_t bytes = buffer_size - cpcm->hw_ready;
		if (cpcm->sw_ready < (int)bytes)
			bytes = cpcm->sw_ready;
		if (hw_to_end < bytes)
			bytes = hw_to_end;
		if (sw_to_end < bytes)
			bytes = sw_to_end;
		memcpy(cpcm->hw_buf.area + cpcm->hw_data,
		       runtime->dma_area + cpcm->sw_data,
		       bytes);
		cpcm->hw_data += bytes;
		if ((int)cpcm->hw_data == buffer_size)
			cpcm->hw_data = 0;
		cpcm->sw_data += bytes;
		if (cpcm->sw_data == cpcm->sw_bufsize)
			cpcm->sw_data = 0;
		cpcm->hw_ready += bytes;
		cpcm->sw_ready -= bytes;
	}
	return 0;
}

static int snd_cs46xx_capture_transfer(snd_pcm_substream_t *substream)
{
	cs46xx_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_pcm_uframes_t appl_ptr = runtime->control->appl_ptr;
	snd_pcm_sframes_t diff = appl_ptr - chip->capt.appl_ptr;
	int buffer_size = runtime->period_size * CS46XX_FRAGS << chip->capt.shift;

	if (diff) {
		if (diff < -(snd_pcm_sframes_t) (runtime->boundary / 2))
			diff += runtime->boundary;
		chip->capt.sw_ready -= diff * (1 << chip->capt.shift);
		chip->capt.appl_ptr = appl_ptr;
	}
	while (chip->capt.hw_ready > 0 && 
	       chip->capt.sw_ready < (int)chip->capt.sw_bufsize) {
		size_t hw_to_end = buffer_size - chip->capt.hw_data;
		size_t sw_to_end = chip->capt.sw_bufsize - chip->capt.sw_data;
		size_t bytes = chip->capt.sw_bufsize - chip->capt.sw_ready;
		if (chip->capt.hw_ready < (int)bytes)
			bytes = chip->capt.hw_ready;
		if (hw_to_end < bytes)
			bytes = hw_to_end;
		if (sw_to_end < bytes)
			bytes = sw_to_end;
		memcpy(runtime->dma_area + chip->capt.sw_data,
		       chip->capt.hw_buf.area + chip->capt.hw_data,
		       bytes);
		chip->capt.hw_data += bytes;
		if ((int)chip->capt.hw_data == buffer_size)
			chip->capt.hw_data = 0;
		chip->capt.sw_data += bytes;
		if (chip->capt.sw_data == chip->capt.sw_bufsize)
			chip->capt.sw_data = 0;
		chip->capt.hw_ready -= bytes;
		chip->capt.sw_ready += bytes;
	}
	return 0;
}

static snd_pcm_uframes_t snd_cs46xx_playback_direct_pointer(snd_pcm_substream_t * substream)
{
	cs46xx_t *chip = snd_pcm_substream_chip(substream);
	size_t ptr;
	cs46xx_pcm_t *cpcm = snd_magic_cast(cs46xx_pcm_t, substream->runtime->private_data, return -ENXIO);
	snd_assert (cpcm->pcm_channel,return -ENXIO);

#ifdef CONFIG_SND_CS46XX_NEW_DSP
	ptr = snd_cs46xx_peek(chip, (cpcm->pcm_channel->pcm_reader_scb->address + 2) << 2);
#else
	ptr = snd_cs46xx_peek(chip, BA1_PBA);
#endif
	ptr -= cpcm->hw_buf.addr;
	return ptr >> cpcm->shift;
}

static snd_pcm_uframes_t snd_cs46xx_playback_indirect_pointer(snd_pcm_substream_t * substream)
{
	cs46xx_t *chip = snd_pcm_substream_chip(substream);
	size_t ptr;
	cs46xx_pcm_t *cpcm = snd_magic_cast(cs46xx_pcm_t, substream->runtime->private_data, return -ENXIO);
	ssize_t bytes;
	int buffer_size = substream->runtime->period_size * CS46XX_FRAGS << cpcm->shift;

#ifdef CONFIG_SND_CS46XX_NEW_DSP
	snd_assert (cpcm->pcm_channel,return -ENXIO);
	ptr = snd_cs46xx_peek(chip, (cpcm->pcm_channel->pcm_reader_scb->address + 2) << 2);
#else
	ptr = snd_cs46xx_peek(chip, BA1_PBA);
#endif
	ptr -= cpcm->hw_buf.addr;

	bytes = ptr - cpcm->hw_io;

	if (bytes < 0)
		bytes += buffer_size;
	cpcm->hw_io = ptr;
	cpcm->hw_ready -= bytes;
	cpcm->sw_io += bytes;
	if (cpcm->sw_io >= cpcm->sw_bufsize)
		cpcm->sw_io -= cpcm->sw_bufsize;
	snd_cs46xx_playback_transfer(substream);
	return cpcm->sw_io >> cpcm->shift;
}

static snd_pcm_uframes_t snd_cs46xx_capture_direct_pointer(snd_pcm_substream_t * substream)
{
	cs46xx_t *chip = snd_pcm_substream_chip(substream);
	size_t ptr = snd_cs46xx_peek(chip, BA1_CBA) - chip->capt.hw_buf.addr;
	return ptr >> chip->capt.shift;
}

static snd_pcm_uframes_t snd_cs46xx_capture_indirect_pointer(snd_pcm_substream_t * substream)
{
	cs46xx_t *chip = snd_pcm_substream_chip(substream);
	size_t ptr = snd_cs46xx_peek(chip, BA1_CBA) - chip->capt.hw_buf.addr;
	ssize_t bytes = ptr - chip->capt.hw_io;
	int buffer_size = substream->runtime->period_size * CS46XX_FRAGS << chip->capt.shift;

	if (bytes < 0)
		bytes += buffer_size;
	chip->capt.hw_io = ptr;
	chip->capt.hw_ready += bytes;
	chip->capt.sw_io += bytes;
	if (chip->capt.sw_io >= chip->capt.sw_bufsize)
		chip->capt.sw_io -= chip->capt.sw_bufsize;
	snd_cs46xx_capture_transfer(substream);
	return chip->capt.sw_io >> chip->capt.shift;
}

static int snd_cs46xx_playback_trigger(snd_pcm_substream_t * substream,
				       int cmd)
{
	cs46xx_t *chip = snd_pcm_substream_chip(substream);
	/*snd_pcm_runtime_t *runtime = substream->runtime;*/
	int result = 0;

#ifdef CONFIG_SND_CS46XX_NEW_DSP
	cs46xx_pcm_t *cpcm = snd_magic_cast(cs46xx_pcm_t, substream->runtime->private_data, return -ENXIO);
#else
	spin_lock(&chip->reg_lock);
#endif

#ifdef CONFIG_SND_CS46XX_NEW_DSP

	if (! cpcm->pcm_channel) {
		return -ENXIO;
	}
#endif
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
#ifdef CONFIG_SND_CS46XX_NEW_DSP
		/* magic value to unmute PCM stream  playback volume */
		snd_cs46xx_poke(chip, (cpcm->pcm_channel->pcm_reader_scb->address + 
				       SCBVolumeCtrl) << 2, 0x80008000);

		if (cpcm->pcm_channel->unlinked)
			cs46xx_dsp_pcm_link(chip,cpcm->pcm_channel);

		if (substream->runtime->periods != CS46XX_FRAGS)
			snd_cs46xx_playback_transfer(substream);
#else
		if (substream->runtime->periods != CS46XX_FRAGS)
			snd_cs46xx_playback_transfer(substream);
		{ unsigned int tmp;
		tmp = snd_cs46xx_peek(chip, BA1_PCTL);
		tmp &= 0x0000ffff;
		snd_cs46xx_poke(chip, BA1_PCTL, chip->play_ctl | tmp);
		}
#endif
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
#ifdef CONFIG_SND_CS46XX_NEW_DSP
		/* magic mute channel */
		snd_cs46xx_poke(chip, (cpcm->pcm_channel->pcm_reader_scb->address + 
				       SCBVolumeCtrl) << 2, 0xffffffff);

		if (!cpcm->pcm_channel->unlinked)
			cs46xx_dsp_pcm_unlink(chip,cpcm->pcm_channel);
#else
		{ unsigned int tmp;
		tmp = snd_cs46xx_peek(chip, BA1_PCTL);
		tmp &= 0x0000ffff;
		snd_cs46xx_poke(chip, BA1_PCTL, tmp);
		}
#endif
		break;
	default:
		result = -EINVAL;
		break;
	}

#ifndef CONFIG_SND_CS46XX_NEW_DSP
	spin_unlock(&chip->reg_lock);
#endif

	return result;
}

static int snd_cs46xx_capture_trigger(snd_pcm_substream_t * substream,
				      int cmd)
{
	cs46xx_t *chip = snd_pcm_substream_chip(substream);
	unsigned int tmp;
	int result = 0;

	spin_lock(&chip->reg_lock);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		tmp = snd_cs46xx_peek(chip, BA1_CCTL);
		tmp &= 0xffff0000;
		snd_cs46xx_poke(chip, BA1_CCTL, chip->capt.ctl | tmp);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		tmp = snd_cs46xx_peek(chip, BA1_CCTL);
		tmp &= 0xffff0000;
		snd_cs46xx_poke(chip, BA1_CCTL, tmp);
		break;
	default:
		result = -EINVAL;
		break;
	}
	spin_unlock(&chip->reg_lock);

	return result;
}

#ifdef CONFIG_SND_CS46XX_NEW_DSP
static int _cs46xx_adjust_sample_rate (cs46xx_t *chip, cs46xx_pcm_t *cpcm,
				       int sample_rate) 
{

	/* If PCMReaderSCB and SrcTaskSCB not created yet ... */
	if ( cpcm->pcm_channel == NULL) {
		cpcm->pcm_channel = cs46xx_dsp_create_pcm_channel (chip, sample_rate, 
								   cpcm, cpcm->hw_buf.addr,cpcm->pcm_channel_id);
		if (cpcm->pcm_channel == NULL) {
			snd_printk(KERN_ERR "cs46xx: failed to create virtual PCM channel\n");
			return -ENOMEM;
		}
		cpcm->pcm_channel->sample_rate = sample_rate;
	} else
	/* if sample rate is changed */
	if ((int)cpcm->pcm_channel->sample_rate != sample_rate) {
		int unlinked = cpcm->pcm_channel->unlinked;
		cs46xx_dsp_destroy_pcm_channel (chip,cpcm->pcm_channel);

		if ( (cpcm->pcm_channel = cs46xx_dsp_create_pcm_channel (chip, sample_rate, cpcm, 
									 cpcm->hw_buf.addr,
									 cpcm->pcm_channel_id)) == NULL) {
			snd_printk(KERN_ERR "cs46xx: failed to re-create virtual PCM channel\n");
			return -ENOMEM;
		}

		if (!unlinked) cs46xx_dsp_pcm_link (chip,cpcm->pcm_channel);
		cpcm->pcm_channel->sample_rate = sample_rate;
	}

	return 0;
}
#endif


static int snd_cs46xx_playback_hw_params(snd_pcm_substream_t * substream,
					 snd_pcm_hw_params_t * hw_params)
{
	snd_pcm_runtime_t *runtime = substream->runtime;
	cs46xx_pcm_t *cpcm;
	int err;
#ifdef CONFIG_SND_CS46XX_NEW_DSP
	cs46xx_t *chip = snd_pcm_substream_chip(substream);
	int sample_rate = params_rate(hw_params);
	int period_size = params_period_bytes(hw_params);
#endif
	cpcm = snd_magic_cast(cs46xx_pcm_t, runtime->private_data, return -ENXIO);

#ifdef CONFIG_SND_CS46XX_NEW_DSP
	snd_assert (sample_rate != 0, return -ENXIO);

	down (&chip->spos_mutex);

	if (_cs46xx_adjust_sample_rate (chip,cpcm,sample_rate)) {
		up (&chip->spos_mutex);
		return -ENXIO;
	}

	snd_assert (cpcm->pcm_channel != NULL);
	if (!cpcm->pcm_channel) {
		up (&chip->spos_mutex);
		return -ENXIO;
	}


	if (cs46xx_dsp_pcm_channel_set_period (chip,cpcm->pcm_channel,period_size)) {
		 up (&chip->spos_mutex);
		 return -EINVAL;
	 }

	snd_printdd ("period_size (%d), periods (%d) buffer_size(%d)\n",
		     period_size, params_periods(hw_params),
		     params_buffer_bytes(hw_params));
#endif

	if (params_periods(hw_params) == CS46XX_FRAGS) {
		if (runtime->dma_area != cpcm->hw_buf.area)
			snd_pcm_lib_free_pages(substream);
		runtime->dma_area = cpcm->hw_buf.area;
		runtime->dma_addr = cpcm->hw_buf.addr;
		runtime->dma_bytes = cpcm->hw_buf.bytes;


#ifdef CONFIG_SND_CS46XX_NEW_DSP
		if (cpcm->pcm_channel_id == DSP_PCM_MAIN_CHANNEL) {
			substream->ops = &snd_cs46xx_playback_ops;
		} else if (cpcm->pcm_channel_id == DSP_PCM_REAR_CHANNEL) {
			substream->ops = &snd_cs46xx_playback_rear_ops;
		} else if (cpcm->pcm_channel_id == DSP_PCM_CENTER_LFE_CHANNEL) {
			substream->ops = &snd_cs46xx_playback_clfe_ops;
		} else if (cpcm->pcm_channel_id == DSP_IEC958_CHANNEL) {
			substream->ops = &snd_cs46xx_playback_iec958_ops;
		} else {
			snd_assert(0);
		}
#else
		substream->ops = &snd_cs46xx_playback_ops;
#endif

	} else {
		if (runtime->dma_area == cpcm->hw_buf.area) {
			runtime->dma_area = NULL;
			runtime->dma_addr = 0;
			runtime->dma_bytes = 0;
		}
		if ((err = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params))) < 0) {
#ifdef CONFIG_SND_CS46XX_NEW_DSP
			up (&chip->spos_mutex);
#endif
			return err;
		}

#ifdef CONFIG_SND_CS46XX_NEW_DSP
		if (cpcm->pcm_channel_id == DSP_PCM_MAIN_CHANNEL) {
			substream->ops = &snd_cs46xx_playback_indirect_ops;
		} else if (cpcm->pcm_channel_id == DSP_PCM_REAR_CHANNEL) {
			substream->ops = &snd_cs46xx_playback_indirect_rear_ops;
		} else if (cpcm->pcm_channel_id == DSP_PCM_CENTER_LFE_CHANNEL) {
			substream->ops = &snd_cs46xx_playback_indirect_clfe_ops;
		} else if (cpcm->pcm_channel_id == DSP_IEC958_CHANNEL) {
			substream->ops = &snd_cs46xx_playback_indirect_iec958_ops;
		} else {
			snd_assert(0);
		}
#else
		substream->ops = &snd_cs46xx_playback_indirect_ops;
#endif

	}

#ifdef CONFIG_SND_CS46XX_NEW_DSP
	up (&chip->spos_mutex);
#endif

	return 0;
}

static int snd_cs46xx_playback_hw_free(snd_pcm_substream_t * substream)
{
	/*cs46xx_t *chip = snd_pcm_substream_chip(substream);*/
	snd_pcm_runtime_t *runtime = substream->runtime;
	cs46xx_pcm_t *cpcm;

	cpcm = snd_magic_cast(cs46xx_pcm_t, runtime->private_data, return -ENXIO);

	/* if play_back open fails, then this function
	   is called and cpcm can actually be NULL here */
	if (!cpcm) return -ENXIO;

	if (runtime->dma_area != cpcm->hw_buf.area)
		snd_pcm_lib_free_pages(substream);
    
	runtime->dma_area = NULL;
	runtime->dma_addr = 0;
	runtime->dma_bytes = 0;

	return 0;
}

static int snd_cs46xx_playback_prepare(snd_pcm_substream_t * substream)
{
	unsigned int tmp;
	unsigned int pfie;
	cs46xx_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	cs46xx_pcm_t *cpcm;

	cpcm = snd_magic_cast(cs46xx_pcm_t, runtime->private_data, return -ENXIO);

#ifdef CONFIG_SND_CS46XX_NEW_DSP
    snd_assert (cpcm->pcm_channel != NULL, return -ENXIO);

	pfie = snd_cs46xx_peek(chip, (cpcm->pcm_channel->pcm_reader_scb->address + 1) << 2 );
	pfie &= ~0x0000f03f;
#else
	/* old dsp */
	pfie = snd_cs46xx_peek(chip, BA1_PFIE);
 	pfie &= ~0x0000f03f;
#endif

	cpcm->shift = 2;
	/* if to convert from stereo to mono */
	if (runtime->channels == 1) {
		cpcm->shift--;
		pfie |= 0x00002000;
	}
	/* if to convert from 8 bit to 16 bit */
	if (snd_pcm_format_width(runtime->format) == 8) {
		cpcm->shift--;
		pfie |= 0x00001000;
	}
	/* if to convert to unsigned */
	if (snd_pcm_format_unsigned(runtime->format))
		pfie |= 0x00008000;

	/* Never convert byte order when sample stream is 8 bit */
	if (snd_pcm_format_width(runtime->format) != 8) {
		/* convert from big endian to little endian */
		if (snd_pcm_format_big_endian(runtime->format))
			pfie |= 0x00004000;
	}
	
	cpcm->sw_bufsize = snd_pcm_lib_buffer_bytes(substream);
	cpcm->sw_data = cpcm->sw_io = cpcm->sw_ready = 0;
	cpcm->hw_data = cpcm->hw_io = cpcm->hw_ready = 0;
	cpcm->appl_ptr = 0;

#ifdef CONFIG_SND_CS46XX_NEW_DSP

	tmp = snd_cs46xx_peek(chip, (cpcm->pcm_channel->pcm_reader_scb->address) << 2);
	tmp &= ~0x000003ff;
	tmp |= (4 << cpcm->shift) - 1;
	/* playback transaction count register */
	snd_cs46xx_poke(chip, (cpcm->pcm_channel->pcm_reader_scb->address) << 2, tmp);

	/* playback format && interrupt enable */
	snd_cs46xx_poke(chip, (cpcm->pcm_channel->pcm_reader_scb->address + 1) << 2, pfie | cpcm->pcm_channel->pcm_slot);
#else
	snd_cs46xx_poke(chip, BA1_PBA, cpcm->hw_buf.addr);
	tmp = snd_cs46xx_peek(chip, BA1_PDTC);
	tmp &= ~0x000003ff;
	tmp |= (4 << cpcm->shift) - 1;
	snd_cs46xx_poke(chip, BA1_PDTC, tmp);
	snd_cs46xx_poke(chip, BA1_PFIE, pfie);
	snd_cs46xx_set_play_sample_rate(chip, runtime->rate);
#endif

	return 0;
}

static int snd_cs46xx_capture_hw_params(snd_pcm_substream_t * substream,
					snd_pcm_hw_params_t * hw_params)
{
	cs46xx_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	int err;

#ifdef CONFIG_SND_CS46XX_NEW_DSP
	cs46xx_dsp_pcm_ostream_set_period (chip, params_period_bytes(hw_params));
#endif
	if (runtime->periods == CS46XX_FRAGS) {
		if (runtime->dma_area != chip->capt.hw_buf.area)
			snd_pcm_lib_free_pages(substream);
		runtime->dma_area = chip->capt.hw_buf.area;
		runtime->dma_addr = chip->capt.hw_buf.addr;
		runtime->dma_bytes = chip->capt.hw_buf.bytes;
		substream->ops = &snd_cs46xx_capture_ops;
	} else {
		if (runtime->dma_area == chip->capt.hw_buf.area) {
			runtime->dma_area = NULL;
			runtime->dma_addr = 0;
			runtime->dma_bytes = 0;
		}
		if ((err = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params))) < 0)
			return err;
		substream->ops = &snd_cs46xx_capture_indirect_ops;
	}

	return 0;
}

static int snd_cs46xx_capture_hw_free(snd_pcm_substream_t * substream)
{
	cs46xx_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;

	if (runtime->dma_area != chip->capt.hw_buf.area)
		snd_pcm_lib_free_pages(substream);
	runtime->dma_area = NULL;
	runtime->dma_addr = 0;
	runtime->dma_bytes = 0;

	return 0;
}

static int snd_cs46xx_capture_prepare(snd_pcm_substream_t * substream)
{
	cs46xx_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;

	snd_cs46xx_poke(chip, BA1_CBA, chip->capt.hw_buf.addr);
	chip->capt.shift = 2;
	chip->capt.sw_bufsize = snd_pcm_lib_buffer_bytes(substream);
	chip->capt.sw_data = chip->capt.sw_io = chip->capt.sw_ready = 0;
	chip->capt.hw_data = chip->capt.hw_io = chip->capt.hw_ready = 0;
	chip->capt.appl_ptr = 0;
	snd_cs46xx_set_capture_sample_rate(chip, runtime->rate);

	return 0;
}

static irqreturn_t snd_cs46xx_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	cs46xx_t *chip = snd_magic_cast(cs46xx_t, dev_id, return IRQ_NONE);
	u32 status1;
#ifdef CONFIG_SND_CS46XX_NEW_DSP
	dsp_spos_instance_t * ins = chip->dsp_spos_instance;
	u32 status2;
	int i;
	cs46xx_pcm_t *cpcm = NULL;
#endif

	/*
	 *  Read the Interrupt Status Register to clear the interrupt
	 */
	status1 = snd_cs46xx_peekBA0(chip, BA0_HISR);
	if ((status1 & 0x7fffffff) == 0) {
		snd_cs46xx_pokeBA0(chip, BA0_HICR, HICR_CHGM | HICR_IEV);
		return IRQ_NONE;
	}

#ifdef CONFIG_SND_CS46XX_NEW_DSP
	status2 = snd_cs46xx_peekBA0(chip, BA0_HSR0);

	for (i = 0; i < DSP_MAX_PCM_CHANNELS; ++i) {
		if (i <= 15) {
			if ( status1 & (1 << i) ) {
				if (i == CS46XX_DSP_CAPTURE_CHANNEL) {
					if (chip->capt.substream)
						snd_pcm_period_elapsed(chip->capt.substream);
				} else {
					if (ins->pcm_channels[i].active &&
					    ins->pcm_channels[i].private_data &&
					    !ins->pcm_channels[i].unlinked) {
						cpcm = snd_magic_cast(cs46xx_pcm_t, ins->pcm_channels[i].private_data, continue);
						snd_pcm_period_elapsed(cpcm->substream);
					}
				}
			}
		} else {
			if ( status2 & (1 << (i - 16))) {
				if (ins->pcm_channels[i].active && 
				    ins->pcm_channels[i].private_data &&
				    !ins->pcm_channels[i].unlinked) {
					cpcm = snd_magic_cast(cs46xx_pcm_t, ins->pcm_channels[i].private_data, continue);
					snd_pcm_period_elapsed(cpcm->substream);
				}
			}
		}
	}

#else
	/* old dsp */
	if ((status1 & HISR_VC0) && chip->playback_pcm) {
		if (chip->playback_pcm->substream)
			snd_pcm_period_elapsed(chip->playback_pcm->substream);
	}
	if ((status1 & HISR_VC1) && chip->pcm) {
		if (chip->capt.substream)
			snd_pcm_period_elapsed(chip->capt.substream);
	}
#endif

	if ((status1 & HISR_MIDI) && chip->rmidi) {
		unsigned char c;
		
		spin_lock(&chip->reg_lock);
		while ((snd_cs46xx_peekBA0(chip, BA0_MIDSR) & MIDSR_RBE) == 0) {
			c = snd_cs46xx_peekBA0(chip, BA0_MIDRP);
			if ((chip->midcr & MIDCR_RIE) == 0)
				continue;
			snd_rawmidi_receive(chip->midi_input, &c, 1);
		}
		while ((snd_cs46xx_peekBA0(chip, BA0_MIDSR) & MIDSR_TBF) == 0) {
			if ((chip->midcr & MIDCR_TIE) == 0)
				break;
			if (snd_rawmidi_transmit(chip->midi_output, &c, 1) != 1) {
				chip->midcr &= ~MIDCR_TIE;
				snd_cs46xx_pokeBA0(chip, BA0_MIDCR, chip->midcr);
				break;
			}
			snd_cs46xx_pokeBA0(chip, BA0_MIDWP, c);
		}
		spin_unlock(&chip->reg_lock);
	}
	/*
	 *  EOI to the PCI part....reenables interrupts
	 */
	snd_cs46xx_pokeBA0(chip, BA0_HICR, HICR_CHGM | HICR_IEV);

	return IRQ_HANDLED;
}

static snd_pcm_hardware_t snd_cs46xx_playback =
{
	.info =			(SNDRV_PCM_INFO_MMAP |
				 SNDRV_PCM_INFO_INTERLEAVED | 
				 SNDRV_PCM_INFO_BLOCK_TRANSFER |
				 SNDRV_PCM_INFO_RESUME),
	.formats =		(SNDRV_PCM_FMTBIT_S8 | SNDRV_PCM_FMTBIT_U8 |
				 SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S16_BE |
				 SNDRV_PCM_FMTBIT_U16_LE | SNDRV_PCM_FMTBIT_U16_BE),
	.rates =		SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_48000,
	.rate_min =		5500,
	.rate_max =		48000,
	.channels_min =		1,
	.channels_max =		2,
	.buffer_bytes_max =	(256 * 1024),
	.period_bytes_min =	CS46XX_MIN_PERIOD_SIZE,
	.period_bytes_max =	CS46XX_MAX_PERIOD_SIZE,
	.periods_min =		CS46XX_FRAGS,
	.periods_max =		1024,
	.fifo_size =		0,
};

static snd_pcm_hardware_t snd_cs46xx_capture =
{
	.info =			(SNDRV_PCM_INFO_MMAP |
				 SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_BLOCK_TRANSFER |
				 SNDRV_PCM_INFO_RESUME),
	.formats =		SNDRV_PCM_FMTBIT_S16_LE,
	.rates =		SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_48000,
	.rate_min =		5500,
	.rate_max =		48000,
	.channels_min =		2,
	.channels_max =		2,
	.buffer_bytes_max =	(256 * 1024),
	.period_bytes_min =	CS46XX_MIN_PERIOD_SIZE,
	.period_bytes_max =	CS46XX_MAX_PERIOD_SIZE,
	.periods_min =		CS46XX_FRAGS,
	.periods_max =		1024,
	.fifo_size =		0,
};

#ifdef CONFIG_SND_CS46XX_NEW_DSP

static unsigned int period_sizes[] = { 32, 64, 128, 256, 512, 1024, 2048 };

#define PERIOD_SIZES sizeof(period_sizes) / sizeof(period_sizes[0])

static snd_pcm_hw_constraint_list_t hw_constraints_period_sizes = {
	.count = PERIOD_SIZES,
	.list = period_sizes,
	.mask = 0
};

#endif

static void snd_cs46xx_pcm_free_substream(snd_pcm_runtime_t *runtime)
{
	cs46xx_pcm_t * cpcm = snd_magic_cast(cs46xx_pcm_t, runtime->private_data, return);
	
	if (cpcm)
		snd_magic_kfree(cpcm);
}

static int _cs46xx_playback_open_channel (snd_pcm_substream_t * substream,int pcm_channel_id)
{
	cs46xx_t *chip = snd_pcm_substream_chip(substream);
	cs46xx_pcm_t * cpcm;
	snd_pcm_runtime_t *runtime = substream->runtime;

	cpcm = snd_magic_kcalloc(cs46xx_pcm_t, 0, GFP_KERNEL);
	if (cpcm == NULL)
		return -ENOMEM;
	if (snd_dma_alloc_pages(&chip->dma_dev, PAGE_SIZE, &cpcm->hw_buf) < 0) {
		snd_magic_kfree(cpcm);
		return -ENOMEM;
	}

	runtime->hw = snd_cs46xx_playback;
	runtime->private_data = cpcm;
	runtime->private_free = snd_cs46xx_pcm_free_substream;

	cpcm->substream = substream;
#ifdef CONFIG_SND_CS46XX_NEW_DSP
	down (&chip->spos_mutex);
	cpcm->pcm_channel = NULL; 
	cpcm->pcm_channel_id = pcm_channel_id;


	snd_pcm_hw_constraint_list(runtime, 0,
				   SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 
				   &hw_constraints_period_sizes);

	up (&chip->spos_mutex);
#else
	chip->playback_pcm = cpcm; /* HACK */
#endif

	if (chip->accept_valid)
		substream->runtime->hw.info |= SNDRV_PCM_INFO_MMAP_VALID;
	chip->active_ctrl(chip, 1);

	return 0;
}

static int snd_cs46xx_playback_open(snd_pcm_substream_t * substream)
{
	snd_printdd("open front channel\n");
	return _cs46xx_playback_open_channel(substream,DSP_PCM_MAIN_CHANNEL);
}

#ifdef CONFIG_SND_CS46XX_NEW_DSP
static int snd_cs46xx_playback_open_rear(snd_pcm_substream_t * substream)
{
	snd_printdd("open rear channel\n");

	return _cs46xx_playback_open_channel(substream,DSP_PCM_REAR_CHANNEL);
}

static int snd_cs46xx_playback_open_clfe(snd_pcm_substream_t * substream)
{
	snd_printdd("open center - LFE channel\n");

	return _cs46xx_playback_open_channel(substream,DSP_PCM_CENTER_LFE_CHANNEL);
}

static int snd_cs46xx_playback_open_iec958(snd_pcm_substream_t * substream)
{
	cs46xx_t *chip = snd_pcm_substream_chip(substream);

	snd_printdd("open raw iec958 channel\n");

	down (&chip->spos_mutex);
	cs46xx_iec958_pre_open (chip);
	up (&chip->spos_mutex);

	return _cs46xx_playback_open_channel(substream,DSP_IEC958_CHANNEL);
}

static int snd_cs46xx_playback_close(snd_pcm_substream_t * substream);

static int snd_cs46xx_playback_close_iec958(snd_pcm_substream_t * substream)
{
	int err;
	cs46xx_t *chip = snd_pcm_substream_chip(substream);
  
	snd_printdd("close raw iec958 channel\n");

	err = snd_cs46xx_playback_close(substream);

	down (&chip->spos_mutex);
	cs46xx_iec958_post_close (chip);
	up (&chip->spos_mutex);

	return err;
}
#endif

static int snd_cs46xx_capture_open(snd_pcm_substream_t * substream)
{
	cs46xx_t *chip = snd_pcm_substream_chip(substream);

	if (snd_dma_alloc_pages(&chip->dma_dev, PAGE_SIZE, &chip->capt.hw_buf) < 0)
		return -ENOMEM;
	chip->capt.substream = substream;
	substream->runtime->hw = snd_cs46xx_capture;

	if (chip->accept_valid)
		substream->runtime->hw.info |= SNDRV_PCM_INFO_MMAP_VALID;

	chip->active_ctrl(chip, 1);

#ifdef CONFIG_SND_CS46XX_NEW_DSP
	snd_pcm_hw_constraint_list(substream->runtime, 0,
				   SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 
				   &hw_constraints_period_sizes);
#endif
	return 0;
}

static int snd_cs46xx_playback_close(snd_pcm_substream_t * substream)
{
	cs46xx_t *chip = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	cs46xx_pcm_t * cpcm;

	cpcm = snd_magic_cast(cs46xx_pcm_t, runtime->private_data, return -ENXIO);

	/* when playback_open fails, then cpcm can be NULL */
	if (!cpcm) return -ENXIO;

#ifdef CONFIG_SND_CS46XX_NEW_DSP
	down (&chip->spos_mutex);
	if (cpcm->pcm_channel) {
		cs46xx_dsp_destroy_pcm_channel(chip,cpcm->pcm_channel);
		cpcm->pcm_channel = NULL;
	}
	up (&chip->spos_mutex);
#else
	chip->playback_pcm = NULL;
#endif

	cpcm->substream = NULL;
	snd_dma_free_pages(&chip->dma_dev, &cpcm->hw_buf);
	chip->active_ctrl(chip, -1);

	return 0;
}

static int snd_cs46xx_capture_close(snd_pcm_substream_t * substream)
{
	cs46xx_t *chip = snd_pcm_substream_chip(substream);

	chip->capt.substream = NULL;
	snd_dma_free_pages(&chip->dma_dev, &chip->capt.hw_buf);
	chip->active_ctrl(chip, -1);

	return 0;
}

#ifdef CONFIG_SND_CS46XX_NEW_DSP
snd_pcm_ops_t snd_cs46xx_playback_rear_ops = {
	.open =			snd_cs46xx_playback_open_rear,
	.close =		snd_cs46xx_playback_close,
	.ioctl =		snd_pcm_lib_ioctl,
	.hw_params =		snd_cs46xx_playback_hw_params,
	.hw_free =		snd_cs46xx_playback_hw_free,
	.prepare =		snd_cs46xx_playback_prepare,
	.trigger =		snd_cs46xx_playback_trigger,
	.pointer =		snd_cs46xx_playback_direct_pointer,
};

snd_pcm_ops_t snd_cs46xx_playback_indirect_rear_ops = {
	.open =			snd_cs46xx_playback_open_rear,
	.close =		snd_cs46xx_playback_close,
	.ioctl =		snd_pcm_lib_ioctl,
	.hw_params =		snd_cs46xx_playback_hw_params,
	.hw_free =		snd_cs46xx_playback_hw_free,
	.prepare =		snd_cs46xx_playback_prepare,
	.trigger =		snd_cs46xx_playback_trigger,
	.pointer =		snd_cs46xx_playback_indirect_pointer,
	.ack =			snd_cs46xx_playback_transfer,
};

snd_pcm_ops_t snd_cs46xx_playback_clfe_ops = {
	.open =			snd_cs46xx_playback_open_clfe,
	.close =		snd_cs46xx_playback_close,
	.ioctl =		snd_pcm_lib_ioctl,
	.hw_params =		snd_cs46xx_playback_hw_params,
	.hw_free =		snd_cs46xx_playback_hw_free,
	.prepare =		snd_cs46xx_playback_prepare,
	.trigger =		snd_cs46xx_playback_trigger,
	.pointer =		snd_cs46xx_playback_direct_pointer,
};

snd_pcm_ops_t snd_cs46xx_playback_indirect_clfe_ops = {
	.open =			snd_cs46xx_playback_open_clfe,
	.close =		snd_cs46xx_playback_close,
	.ioctl =		snd_pcm_lib_ioctl,
	.hw_params =		snd_cs46xx_playback_hw_params,
	.hw_free =		snd_cs46xx_playback_hw_free,
	.prepare =		snd_cs46xx_playback_prepare,
	.trigger =		snd_cs46xx_playback_trigger,
	.pointer =		snd_cs46xx_playback_indirect_pointer,
	.ack =			snd_cs46xx_playback_transfer,
};

snd_pcm_ops_t snd_cs46xx_playback_iec958_ops = {
	.open =			snd_cs46xx_playback_open_iec958,
	.close =		snd_cs46xx_playback_close_iec958,
	.ioctl =		snd_pcm_lib_ioctl,
	.hw_params =		snd_cs46xx_playback_hw_params,
	.hw_free =		snd_cs46xx_playback_hw_free,
	.prepare =		snd_cs46xx_playback_prepare,
	.trigger =		snd_cs46xx_playback_trigger,
	.pointer =		snd_cs46xx_playback_direct_pointer,
};

snd_pcm_ops_t snd_cs46xx_playback_indirect_iec958_ops = {
	.open =			snd_cs46xx_playback_open_iec958,
	.close =		snd_cs46xx_playback_close_iec958,
	.ioctl =		snd_pcm_lib_ioctl,
	.hw_params =		snd_cs46xx_playback_hw_params,
	.hw_free =		snd_cs46xx_playback_hw_free,
	.prepare =		snd_cs46xx_playback_prepare,
	.trigger =		snd_cs46xx_playback_trigger,
	.pointer =		snd_cs46xx_playback_indirect_pointer,
	.ack =			snd_cs46xx_playback_transfer,
};

#endif

snd_pcm_ops_t snd_cs46xx_playback_ops = {
	.open =			snd_cs46xx_playback_open,
	.close =		snd_cs46xx_playback_close,
	.ioctl =		snd_pcm_lib_ioctl,
	.hw_params =		snd_cs46xx_playback_hw_params,
	.hw_free =		snd_cs46xx_playback_hw_free,
	.prepare =		snd_cs46xx_playback_prepare,
	.trigger =		snd_cs46xx_playback_trigger,
	.pointer =		snd_cs46xx_playback_direct_pointer,
};

snd_pcm_ops_t snd_cs46xx_playback_indirect_ops = {
	.open =			snd_cs46xx_playback_open,
	.close =		snd_cs46xx_playback_close,
	.ioctl =		snd_pcm_lib_ioctl,
	.hw_params =		snd_cs46xx_playback_hw_params,
	.hw_free =		snd_cs46xx_playback_hw_free,
	.prepare =		snd_cs46xx_playback_prepare,
	.trigger =		snd_cs46xx_playback_trigger,
	.pointer =		snd_cs46xx_playback_indirect_pointer,
	.ack =			snd_cs46xx_playback_transfer,
};

snd_pcm_ops_t snd_cs46xx_capture_ops = {
	.open =			snd_cs46xx_capture_open,
	.close =		snd_cs46xx_capture_close,
	.ioctl =		snd_pcm_lib_ioctl,
	.hw_params =		snd_cs46xx_capture_hw_params,
	.hw_free =		snd_cs46xx_capture_hw_free,
	.prepare =		snd_cs46xx_capture_prepare,
	.trigger =		snd_cs46xx_capture_trigger,
	.pointer =		snd_cs46xx_capture_direct_pointer,
};

snd_pcm_ops_t snd_cs46xx_capture_indirect_ops = {
	.open =			snd_cs46xx_capture_open,
	.close =		snd_cs46xx_capture_close,
	.ioctl =		snd_pcm_lib_ioctl,
	.hw_params =		snd_cs46xx_capture_hw_params,
	.hw_free =		snd_cs46xx_capture_hw_free,
	.prepare =		snd_cs46xx_capture_prepare,
	.trigger =		snd_cs46xx_capture_trigger,
	.pointer =		snd_cs46xx_capture_indirect_pointer,
	.ack =			snd_cs46xx_capture_transfer,
};

static void snd_cs46xx_pcm_free(snd_pcm_t *pcm)
{
	cs46xx_t *chip = snd_magic_cast(cs46xx_t, pcm->private_data, return);
	chip->pcm = NULL;
	snd_pcm_lib_preallocate_free_for_all(pcm);
}

#ifdef CONFIG_SND_CS46XX_NEW_DSP
static void snd_cs46xx_pcm_rear_free(snd_pcm_t *pcm)
{
	cs46xx_t *chip = snd_magic_cast(cs46xx_t, pcm->private_data, return);
	chip->pcm_rear = NULL;
	snd_pcm_lib_preallocate_free_for_all(pcm);
}

static void snd_cs46xx_pcm_center_lfe_free(snd_pcm_t *pcm)
{
	cs46xx_t *chip = snd_magic_cast(cs46xx_t, pcm->private_data, return);
	chip->pcm_center_lfe = NULL;
	snd_pcm_lib_preallocate_free_for_all(pcm);
}

static void snd_cs46xx_pcm_iec958_free(snd_pcm_t *pcm)
{
	cs46xx_t *chip = snd_magic_cast(cs46xx_t, pcm->private_data, return);
	chip->pcm_iec958 = NULL;
	snd_pcm_lib_preallocate_free_for_all(pcm);
}

#define MAX_PLAYBACK_CHANNELS	(DSP_MAX_PCM_CHANNELS - 1)
#else
#define MAX_PLAYBACK_CHANNELS	1
#endif

int __devinit snd_cs46xx_pcm(cs46xx_t *chip, int device, snd_pcm_t ** rpcm)
{
	snd_pcm_t *pcm;
	int err;

	if (rpcm)
		*rpcm = NULL;
	if ((err = snd_pcm_new(chip->card, "CS46xx", device, MAX_PLAYBACK_CHANNELS, 1, &pcm)) < 0)
		return err;

	pcm->private_data = chip;
	pcm->private_free = snd_cs46xx_pcm_free;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &snd_cs46xx_playback_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &snd_cs46xx_capture_ops);

	/* global setup */
	pcm->info_flags = 0;
	strcpy(pcm->name, "CS46xx");
	chip->pcm = pcm;

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV,
					      snd_dma_pci_data(chip->pci), 64*1024, 256*1024);

	if (rpcm)
		*rpcm = pcm;

	return 0;
}


#ifdef CONFIG_SND_CS46XX_NEW_DSP
int __devinit snd_cs46xx_pcm_rear(cs46xx_t *chip, int device, snd_pcm_t ** rpcm)
{
	snd_pcm_t *pcm;
	int err;

	if (rpcm)
		*rpcm = NULL;

	if ((err = snd_pcm_new(chip->card, "CS46xx - Rear", device, MAX_PLAYBACK_CHANNELS, 0, &pcm)) < 0)
		return err;

	pcm->private_data = chip;
	pcm->private_free = snd_cs46xx_pcm_rear_free;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &snd_cs46xx_playback_rear_ops);

	/* global setup */
	pcm->info_flags = 0;
	strcpy(pcm->name, "CS46xx - Rear");
	chip->pcm_rear = pcm;

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV,
					      snd_dma_pci_data(chip->pci), 64*1024, 256*1024);

	if (rpcm)
		*rpcm = pcm;

	return 0;
}

int __devinit snd_cs46xx_pcm_center_lfe(cs46xx_t *chip, int device, snd_pcm_t ** rpcm)
{
	snd_pcm_t *pcm;
	int err;

	if (rpcm)
		*rpcm = NULL;

	if ((err = snd_pcm_new(chip->card, "CS46xx - Center LFE", device, MAX_PLAYBACK_CHANNELS, 0, &pcm)) < 0)
		return err;

	pcm->private_data = chip;
	pcm->private_free = snd_cs46xx_pcm_center_lfe_free;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &snd_cs46xx_playback_clfe_ops);

	/* global setup */
	pcm->info_flags = 0;
	strcpy(pcm->name, "CS46xx - Center LFE");
	chip->pcm_center_lfe = pcm;

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV,
					      snd_dma_pci_data(chip->pci), 64*1024, 256*1024);

	if (rpcm)
		*rpcm = pcm;

	return 0;
}

int __devinit snd_cs46xx_pcm_iec958(cs46xx_t *chip, int device, snd_pcm_t ** rpcm)
{
	snd_pcm_t *pcm;
	int err;

	if (rpcm)
		*rpcm = NULL;

	if ((err = snd_pcm_new(chip->card, "CS46xx - IEC958", device, 1, 0, &pcm)) < 0)
		return err;

	pcm->private_data = chip;
	pcm->private_free = snd_cs46xx_pcm_iec958_free;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &snd_cs46xx_playback_iec958_ops);

	/* global setup */
	pcm->info_flags = 0;
	strcpy(pcm->name, "CS46xx - IEC958");
	chip->pcm_rear = pcm;

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV,
					      snd_dma_pci_data(chip->pci), 64*1024, 256*1024);

	if (rpcm)
		*rpcm = pcm;

	return 0;
}
#endif

/*
 *  Mixer routines
 */
static void snd_cs46xx_mixer_free_ac97_bus(ac97_bus_t *bus)
{
	cs46xx_t *chip = snd_magic_cast(cs46xx_t, bus->private_data, return);

	chip->ac97_bus = NULL;
}

static void snd_cs46xx_mixer_free_ac97(ac97_t *ac97)
{
	cs46xx_t *chip = snd_magic_cast(cs46xx_t, ac97->private_data, return);

	snd_assert ((ac97 == chip->ac97[CS46XX_PRIMARY_CODEC_INDEX]) ||
		    (ac97 == chip->ac97[CS46XX_SECONDARY_CODEC_INDEX]),
		    return);

	if (ac97 == chip->ac97[CS46XX_PRIMARY_CODEC_INDEX]) {
		chip->ac97[CS46XX_PRIMARY_CODEC_INDEX] = NULL;
		chip->eapd_switch = NULL;
	}
	else
		chip->ac97[CS46XX_SECONDARY_CODEC_INDEX] = NULL;
}

static int snd_cs46xx_vol_info(snd_kcontrol_t *kcontrol, 
			       snd_ctl_elem_info_t *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 2;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 0x7fff;
	return 0;
}

static int snd_cs46xx_vol_get(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);
	int reg = kcontrol->private_value;
	unsigned int val = snd_cs46xx_peek(chip, reg);
	ucontrol->value.integer.value[0] = 0xffff - (val >> 16);
	ucontrol->value.integer.value[1] = 0xffff - (val & 0xffff);
	return 0;
}

static int snd_cs46xx_vol_put(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);
	int reg = kcontrol->private_value;
	unsigned int val = ((0xffff - ucontrol->value.integer.value[0]) << 16 | 
			    (0xffff - ucontrol->value.integer.value[1]));
	unsigned int old = snd_cs46xx_peek(chip, reg);
	int change = (old != val);

	if (change) {
		snd_cs46xx_poke(chip, reg, val);
	}

	return change;
}

#ifdef CONFIG_SND_CS46XX_NEW_DSP

static int snd_cs46xx_vol_dac_get(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = chip->dsp_spos_instance->dac_volume_left;
	ucontrol->value.integer.value[1] = chip->dsp_spos_instance->dac_volume_right;

	return 0;
}

static int snd_cs46xx_vol_dac_put(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);
	int change = 0;

	if (chip->dsp_spos_instance->dac_volume_right != ucontrol->value.integer.value[0] ||
	    chip->dsp_spos_instance->dac_volume_left != ucontrol->value.integer.value[1]) {
		cs46xx_dsp_set_dac_volume(chip,
					  ucontrol->value.integer.value[0],
					  ucontrol->value.integer.value[1]);
		change = 1;
	}

	return change;
}

#if 0
static int snd_cs46xx_vol_iec958_get(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = chip->dsp_spos_instance->spdif_input_volume_left;
	ucontrol->value.integer.value[1] = chip->dsp_spos_instance->spdif_input_volume_right;
	return 0;
}

static int snd_cs46xx_vol_iec958_put(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);
	int change = 0;

	if (chip->dsp_spos_instance->spdif_input_volume_left  != ucontrol->value.integer.value[0] ||
	    chip->dsp_spos_instance->spdif_input_volume_right!= ucontrol->value.integer.value[1]) {
		cs46xx_dsp_set_iec958_volume (chip,
					      ucontrol->value.integer.value[0],
					      ucontrol->value.integer.value[1]);
		change = 1;
	}

	return change;
}
#endif

static int snd_mixer_boolean_info(snd_kcontrol_t *kcontrol, 
				  snd_ctl_elem_info_t *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int snd_cs46xx_iec958_get(snd_kcontrol_t *kcontrol, 
                                 snd_ctl_elem_value_t *ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);
	int reg = kcontrol->private_value;

	if (reg == CS46XX_MIXER_SPDIF_OUTPUT_ELEMENT)
		ucontrol->value.integer.value[0] = (chip->dsp_spos_instance->spdif_status_out & DSP_SPDIF_STATUS_OUTPUT_ENABLED);
	else
		ucontrol->value.integer.value[0] = chip->dsp_spos_instance->spdif_status_in;

	return 0;
}

static int snd_cs46xx_iec958_put(snd_kcontrol_t *kcontrol, 
                                  snd_ctl_elem_value_t *ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);
	int change, res;

	switch (kcontrol->private_value) {
	case CS46XX_MIXER_SPDIF_OUTPUT_ELEMENT:
		down (&chip->spos_mutex);
		change = (chip->dsp_spos_instance->spdif_status_out & DSP_SPDIF_STATUS_OUTPUT_ENABLED);
		if (ucontrol->value.integer.value[0] && !change) 
			cs46xx_dsp_enable_spdif_out(chip);
		else if (change && !ucontrol->value.integer.value[0])
			cs46xx_dsp_disable_spdif_out(chip);

		res = (change != (chip->dsp_spos_instance->spdif_status_out & DSP_SPDIF_STATUS_OUTPUT_ENABLED));
		up (&chip->spos_mutex);
		break;
	case CS46XX_MIXER_SPDIF_INPUT_ELEMENT:
		change = chip->dsp_spos_instance->spdif_status_in;
		if (ucontrol->value.integer.value[0] && !change) {
			cs46xx_dsp_enable_spdif_in(chip);
			/* restore volume */
		}
		else if (change && !ucontrol->value.integer.value[0])
			cs46xx_dsp_disable_spdif_in(chip);
		
		res = (change != chip->dsp_spos_instance->spdif_status_in);
		break;
	default:
		res = -EINVAL;
		snd_assert(0, (void)0);
	}

	return res;
}

static int snd_cs46xx_adc_capture_get(snd_kcontrol_t *kcontrol, 
                                      snd_ctl_elem_value_t *ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);
	dsp_spos_instance_t * ins = chip->dsp_spos_instance;

	if (ins->adc_input != NULL) 
		ucontrol->value.integer.value[0] = 1;
	else 
		ucontrol->value.integer.value[0] = 0;
	
	return 0;
}

static int snd_cs46xx_adc_capture_put(snd_kcontrol_t *kcontrol, 
                                      snd_ctl_elem_value_t *ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);
	dsp_spos_instance_t * ins = chip->dsp_spos_instance;
	int change = 0;

	if (ucontrol->value.integer.value[0] && !ins->adc_input) {
		cs46xx_dsp_enable_adc_capture(chip);
		change = 1;
	} else  if (!ucontrol->value.integer.value[0] && ins->adc_input) {
		cs46xx_dsp_disable_adc_capture(chip);
		change = 1;
	}
	return change;
}

static int snd_cs46xx_pcm_capture_get(snd_kcontrol_t *kcontrol, 
                                      snd_ctl_elem_value_t *ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);
	dsp_spos_instance_t * ins = chip->dsp_spos_instance;

	if (ins->pcm_input != NULL) 
		ucontrol->value.integer.value[0] = 1;
	else 
		ucontrol->value.integer.value[0] = 0;

	return 0;
}


static int snd_cs46xx_pcm_capture_put(snd_kcontrol_t *kcontrol, 
                                      snd_ctl_elem_value_t *ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);
	dsp_spos_instance_t * ins = chip->dsp_spos_instance;
	int change = 0;

	if (ucontrol->value.integer.value[0] && !ins->pcm_input) {
		cs46xx_dsp_enable_pcm_capture(chip);
		change = 1;
	} else  if (!ucontrol->value.integer.value[0] && ins->pcm_input) {
		cs46xx_dsp_disable_pcm_capture(chip);
		change = 1;
	}

	return change;
}

static int snd_herc_spdif_select_get(snd_kcontrol_t *kcontrol, 
                                     snd_ctl_elem_value_t *ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);

	int val1 = snd_cs46xx_peekBA0(chip, BA0_EGPIODR);

	if (val1 & EGPIODR_GPOE0)
		ucontrol->value.integer.value[0] = 1;
	else
		ucontrol->value.integer.value[0] = 0;

	return 0;
}

/*
 *	Game Theatre XP card - EGPIO[0] is used to select SPDIF input optical or coaxial.
 */ 
static int snd_herc_spdif_select_put(snd_kcontrol_t *kcontrol, 
                                       snd_ctl_elem_value_t *ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);
	int val1 = snd_cs46xx_peekBA0(chip, BA0_EGPIODR);
	int val2 = snd_cs46xx_peekBA0(chip, BA0_EGPIOPTR);

	if (ucontrol->value.integer.value[0]) {
		/* optical is default */
		snd_cs46xx_pokeBA0(chip, BA0_EGPIODR, 
				   EGPIODR_GPOE0 | val1);  /* enable EGPIO0 output */
		snd_cs46xx_pokeBA0(chip, BA0_EGPIOPTR, 
				   EGPIOPTR_GPPT0 | val2); /* open-drain on output */
	} else {
		/* coaxial */
		snd_cs46xx_pokeBA0(chip, BA0_EGPIODR,  val1 & ~EGPIODR_GPOE0); /* disable */
		snd_cs46xx_pokeBA0(chip, BA0_EGPIOPTR, val2 & ~EGPIOPTR_GPPT0); /* disable */
	}

	/* checking diff from the EGPIO direction register 
	   should be enough */
	return (val1 != (int)snd_cs46xx_peekBA0(chip, BA0_EGPIODR));
}


static int snd_cs46xx_spdif_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_IEC958;
	uinfo->count = 1;
	return 0;
}

static int snd_cs46xx_spdif_default_get(snd_kcontrol_t * kcontrol,
					snd_ctl_elem_value_t * ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);
	dsp_spos_instance_t * ins = chip->dsp_spos_instance;

	down (&chip->spos_mutex);
	ucontrol->value.iec958.status[0] = _wrap_all_bits((ins->spdif_csuv_default >> 24) & 0xff);
	ucontrol->value.iec958.status[1] = _wrap_all_bits((ins->spdif_csuv_default >> 16) & 0xff);
	ucontrol->value.iec958.status[2] = 0;
	ucontrol->value.iec958.status[3] = _wrap_all_bits((ins->spdif_csuv_default) & 0xff);
	up (&chip->spos_mutex);

	return 0;
}

static int snd_cs46xx_spdif_default_put(snd_kcontrol_t * kcontrol,
					snd_ctl_elem_value_t * ucontrol)
{
	cs46xx_t * chip = snd_kcontrol_chip(kcontrol);
	dsp_spos_instance_t * ins = chip->dsp_spos_instance;
	unsigned int val;
	int change;

	down (&chip->spos_mutex);
	val = ((unsigned int)_wrap_all_bits(ucontrol->value.iec958.status[0]) << 24) |
		((unsigned int)_wrap_all_bits(ucontrol->value.iec958.status[2]) << 16) |
		((unsigned int)_wrap_all_bits(ucontrol->value.iec958.status[3]))  |
		/* left and right validity bit */
		(1 << 13) | (1 << 12);


	change = (unsigned int)ins->spdif_csuv_default != val;
	ins->spdif_csuv_default = val;

	if ( !(ins->spdif_status_out & DSP_SPDIF_STATUS_PLAYBACK_OPEN) )
		cs46xx_poke_via_dsp (chip,SP_SPDOUT_CSUV,val);

	up (&chip->spos_mutex);

	return change;
}

static int snd_cs46xx_spdif_mask_get(snd_kcontrol_t * kcontrol,
				     snd_ctl_elem_value_t * ucontrol)
{
	ucontrol->value.iec958.status[0] = 0xff;
	ucontrol->value.iec958.status[1] = 0xff;
	ucontrol->value.iec958.status[2] = 0x00;
	ucontrol->value.iec958.status[3] = 0xff;
	return 0;
}

static int snd_cs46xx_spdif_stream_get(snd_kcontrol_t * kcontrol,
                                         snd_ctl_elem_value_t * ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);
	dsp_spos_instance_t * ins = chip->dsp_spos_instance;

	down (&chip->spos_mutex);
	ucontrol->value.iec958.status[0] = _wrap_all_bits((ins->spdif_csuv_stream >> 24) & 0xff);
	ucontrol->value.iec958.status[1] = _wrap_all_bits((ins->spdif_csuv_stream >> 16) & 0xff);
	ucontrol->value.iec958.status[2] = 0;
	ucontrol->value.iec958.status[3] = _wrap_all_bits((ins->spdif_csuv_stream) & 0xff);
	up (&chip->spos_mutex);

	return 0;
}

static int snd_cs46xx_spdif_stream_put(snd_kcontrol_t * kcontrol,
                                        snd_ctl_elem_value_t * ucontrol)
{
	cs46xx_t * chip = snd_kcontrol_chip(kcontrol);
	dsp_spos_instance_t * ins = chip->dsp_spos_instance;
	unsigned int val;
	int change;

	down (&chip->spos_mutex);
	val = ((unsigned int)_wrap_all_bits(ucontrol->value.iec958.status[0]) << 24) |
		((unsigned int)_wrap_all_bits(ucontrol->value.iec958.status[1]) << 16) |
		((unsigned int)_wrap_all_bits(ucontrol->value.iec958.status[3])) |
		/* left and right validity bit */
		(1 << 13) | (1 << 12);


	change = ins->spdif_csuv_stream != val;
	ins->spdif_csuv_stream = val;

	if ( ins->spdif_status_out & DSP_SPDIF_STATUS_PLAYBACK_OPEN )
		cs46xx_poke_via_dsp (chip,SP_SPDOUT_CSUV,val);

	up (&chip->spos_mutex);

	return change;
}

#endif /* CONFIG_SND_CS46XX_NEW_DSP */


#ifdef CONFIG_SND_CS46XX_DEBUG_GPIO
static int snd_cs46xx_egpio_select_info(snd_kcontrol_t *kcontrol, 
                                        snd_ctl_elem_info_t *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 8;
	return 0;
}

static int snd_cs46xx_egpio_select_get(snd_kcontrol_t *kcontrol, 
                                       snd_ctl_elem_value_t *ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);
	ucontrol->value.integer.value[0] = chip->current_gpio;

	return 0;
}

static int snd_cs46xx_egpio_select_put(snd_kcontrol_t *kcontrol, 
                                       snd_ctl_elem_value_t *ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);
	int change = (chip->current_gpio != ucontrol->value.integer.value[0]);
	chip->current_gpio = ucontrol->value.integer.value[0];

	return change;
}


static int snd_cs46xx_egpio_get(snd_kcontrol_t *kcontrol, 
                                       snd_ctl_elem_value_t *ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);
	int reg = kcontrol->private_value;

	snd_printdd ("put: reg = %04x, gpio %02x\n",reg,chip->current_gpio);
	ucontrol->value.integer.value[0] = 
		(snd_cs46xx_peekBA0(chip, reg) & (1 << chip->current_gpio)) ? 1 : 0;
  
	return 0;
}

static int snd_cs46xx_egpio_put(snd_kcontrol_t *kcontrol, 
                                       snd_ctl_elem_value_t *ucontrol)
{
	cs46xx_t *chip = snd_kcontrol_chip(kcontrol);
	int reg = kcontrol->private_value;
	int val = snd_cs46xx_peekBA0(chip, reg);
	int oldval = val;
	snd_printdd ("put: reg = %04x, gpio %02x\n",reg,chip->current_gpio);

	if (ucontrol->value.integer.value[0])
		val |= (1 << chip->current_gpio);
	else
		val &= ~(1 << chip->current_gpio);

	snd_cs46xx_pokeBA0(chip, reg,val);
	snd_printdd ("put: val %08x oldval %08x\n",val,oldval);

	return (oldval != val);
}
#endif /* CONFIG_SND_CS46XX_DEBUG_GPIO */

static snd_kcontrol_new_t snd_cs46xx_controls[] __devinitdata = {
{
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "DAC Volume",
	.info = snd_cs46xx_vol_info,
#ifndef CONFIG_SND_CS46XX_NEW_DSP
	.get = snd_cs46xx_vol_get,
	.put = snd_cs46xx_vol_put,
	.private_value = BA1_PVOL,
#else
	.get = snd_cs46xx_vol_dac_get,
	.put = snd_cs46xx_vol_dac_put,
#endif
},

{
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "ADC Volume",
	.info = snd_cs46xx_vol_info,
	.get = snd_cs46xx_vol_get,
	.put = snd_cs46xx_vol_put,
#ifndef CONFIG_SND_CS46XX_NEW_DSP
	.private_value = BA1_CVOL,
#else
	.private_value = (VARIDECIMATE_SCB_ADDR + 0xE) << 2,
#endif
},
#ifdef CONFIG_SND_CS46XX_NEW_DSP
{
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "ADC Capture Switch",
	.info = snd_mixer_boolean_info,
	.get = snd_cs46xx_adc_capture_get,
	.put = snd_cs46xx_adc_capture_put
},
{
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "DAC Capture Switch",
	.info = snd_mixer_boolean_info,
	.get = snd_cs46xx_pcm_capture_get,
	.put = snd_cs46xx_pcm_capture_put
},
{
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "IEC958 Output Switch",
	.info = snd_mixer_boolean_info,
	.get = snd_cs46xx_iec958_get,
	.put = snd_cs46xx_iec958_put,
	.private_value = CS46XX_MIXER_SPDIF_OUTPUT_ELEMENT,
},
{
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "IEC958 Input Switch",
	.info = snd_mixer_boolean_info,
	.get = snd_cs46xx_iec958_get,
	.put = snd_cs46xx_iec958_put,
	.private_value = CS46XX_MIXER_SPDIF_INPUT_ELEMENT,
},
#if 0
/* Input IEC958 volume does not work for the moment. (Benny) */
{
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "IEC958 Input Volume",
	.info = snd_cs46xx_vol_info,
	.get = snd_cs46xx_vol_iec958_get,
	.put = snd_cs46xx_vol_iec958_put,
	.private_value = (ASYNCRX_SCB_ADDR + 0xE) << 2,
},
#endif
{
	.iface = SNDRV_CTL_ELEM_IFACE_PCM,
	.name =  SNDRV_CTL_NAME_IEC958("",PLAYBACK,DEFAULT),
	.info =	 snd_cs46xx_spdif_info,
	.get =	 snd_cs46xx_spdif_default_get,
	.put =   snd_cs46xx_spdif_default_put,
},
{
	.iface = SNDRV_CTL_ELEM_IFACE_PCM,
	.name =	 SNDRV_CTL_NAME_IEC958("",PLAYBACK,MASK),
	.info =	 snd_cs46xx_spdif_info,
        .get =	 snd_cs46xx_spdif_mask_get,
	.access = SNDRV_CTL_ELEM_ACCESS_READ
},
{
	.iface = SNDRV_CTL_ELEM_IFACE_PCM,
	.name =	 SNDRV_CTL_NAME_IEC958("",PLAYBACK,PCM_STREAM),
	.info =	 snd_cs46xx_spdif_info,
	.get =	 snd_cs46xx_spdif_stream_get,
	.put =	 snd_cs46xx_spdif_stream_put
},

#endif
#ifdef CONFIG_SND_CS46XX_DEBUG_GPIO
{
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "EGPIO select",
	.info = snd_cs46xx_egpio_select_info,
	.get = snd_cs46xx_egpio_select_get,
	.put = snd_cs46xx_egpio_select_put,
	.private_value = 0,
},
{
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "EGPIO Input/Output",
	.info = snd_mixer_boolean_info,
	.get = snd_cs46xx_egpio_get,
	.put = snd_cs46xx_egpio_put,
	.private_value = BA0_EGPIODR,
},
{
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "EGPIO CMOS/Open drain",
	.info = snd_mixer_boolean_info,
	.get = snd_cs46xx_egpio_get,
	.put = snd_cs46xx_egpio_put,
	.private_value = BA0_EGPIOPTR,
},
{
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "EGPIO On/Off",
	.info = snd_mixer_boolean_info,
	.get = snd_cs46xx_egpio_get,
	.put = snd_cs46xx_egpio_put,
	.private_value = BA0_EGPIOSR,
},
#endif
};

#ifdef CONFIG_SND_CS46XX_NEW_DSP
/* Only available on the Hercules Game Theater XP soundcard */
static snd_kcontrol_new_t snd_hercules_controls[] __devinitdata = {
{
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Optical/Coaxial SPDIF Input Switch",
	.info = snd_mixer_boolean_info,
	.get = snd_herc_spdif_select_get,
	.put = snd_herc_spdif_select_put,
},
};


static void snd_cs46xx_codec_reset (ac97_t * ac97)
{
	unsigned long end_time;
	int err;
	cs46xx_t * chip = snd_magic_cast(cs46xx_t,ac97->private_data,return /* -ENXIO */);

	/* reset to defaults */
	snd_ac97_write(ac97, AC97_RESET, 0);	

	/* set the desired CODEC mode */
	if (chip->nr_ac97_codecs == 0) {
		snd_printdd("cs46xx: CODOEC1 mode %04x\n",0x0);
		snd_cs46xx_ac97_write(ac97,AC97_CSR_ACMODE,0x0);
	} else if (chip->nr_ac97_codecs == 1) {
		snd_printdd("cs46xx: CODOEC2 mode %04x\n",0x3);
		snd_cs46xx_ac97_write(ac97,AC97_CSR_ACMODE,0x3);
	} else {
		snd_assert(0); /* should never happen ... */
	}

	udelay(50);

	/* it's necessary to wait awhile until registers are accessible after RESET */
	/* because the PCM or MASTER volume registers can be modified, */
	/* the REC_GAIN register is used for tests */
	end_time = jiffies + HZ;
	do {
		unsigned short ext_mid;
    
		/* use preliminary reads to settle the communication */
		snd_ac97_read(ac97, AC97_RESET);
		snd_ac97_read(ac97, AC97_VENDOR_ID1);
		snd_ac97_read(ac97, AC97_VENDOR_ID2);
		/* modem? */
		ext_mid = snd_ac97_read(ac97, AC97_EXTENDED_MID);
		if (ext_mid != 0xffff && (ext_mid & 1) != 0)
			return;

		/* test if we can write to the record gain volume register */
		snd_ac97_write_cache(ac97, AC97_REC_GAIN, 0x8a05);
		if ((err = snd_ac97_read(ac97, AC97_REC_GAIN)) == 0x8a05)
			return;

		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ/100);
	} while (time_after_eq(end_time, jiffies));

	snd_printk("CS46xx secondary codec dont respond!\n");  
}
#endif

int __devinit snd_cs46xx_mixer(cs46xx_t *chip)
{
	snd_card_t *card = chip->card;
	ac97_bus_t bus;
	ac97_t ac97;
	snd_ctl_elem_id_t id;
	int err;
	unsigned int idx;

	/* detect primary codec */
	chip->nr_ac97_codecs = 0;
	snd_printdd("snd_cs46xx: detecting primary codec\n");
	memset(&bus, 0, sizeof(bus));
	bus.write = snd_cs46xx_ac97_write;
	bus.read = snd_cs46xx_ac97_read;
#ifdef CONFIG_SND_CS46XX_NEW_DSP
	bus.reset = snd_cs46xx_codec_reset;
#endif
	bus.private_data = chip;
	bus.private_free = snd_cs46xx_mixer_free_ac97_bus;
	if ((err = snd_ac97_bus(card, &bus, &chip->ac97_bus)) < 0)
		return err;

	memset(&ac97, 0, sizeof(ac97));
	ac97.private_data = chip;
	ac97.private_free = snd_cs46xx_mixer_free_ac97;
	chip->ac97[CS46XX_PRIMARY_CODEC_INDEX] = &ac97;

	snd_cs46xx_ac97_write(&ac97, AC97_MASTER, 0x8000);
	for (idx = 0; idx < 100; ++idx) {
		if (snd_cs46xx_ac97_read(&ac97, AC97_MASTER) == 0x8000)
			goto _ok;
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ/100);
	}
	chip->ac97[CS46XX_PRIMARY_CODEC_INDEX] = NULL;
	return -ENXIO;

 _ok:
	if ((err = snd_ac97_mixer(chip->ac97_bus, &ac97, &chip->ac97[CS46XX_PRIMARY_CODEC_INDEX])) < 0)
		return err;
	snd_printdd("snd_cs46xx: primary codec phase one\n");
	chip->nr_ac97_codecs = 1;

#ifdef CONFIG_SND_CS46XX_NEW_DSP
	snd_printdd("snd_cs46xx: detecting seconadry codec\n");
	/* try detect a secondary codec */
	memset(&ac97, 0, sizeof(ac97));    
	ac97.private_data = chip;
	ac97.private_free = snd_cs46xx_mixer_free_ac97;
	ac97.num = CS46XX_SECONDARY_CODEC_INDEX;

	snd_cs46xx_ac97_write(&ac97, AC97_RESET, 0);
	udelay(10);

	if (snd_cs46xx_ac97_read(&ac97, AC97_RESET) & 0x8000) {
		snd_printdd("snd_cs46xx: seconadry codec not present\n");
		goto _no_sec_codec;
	}

	chip->ac97[CS46XX_SECONDARY_CODEC_INDEX] = &ac97;
	snd_cs46xx_ac97_write(&ac97, AC97_MASTER, 0x8000);
	for (idx = 0; idx < 100; ++idx) {
		if (snd_cs46xx_ac97_read(&ac97, AC97_MASTER) == 0x8000) {
			goto _ok2;
		}
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ/100);
	}

 _no_sec_codec:
	snd_printdd("snd_cs46xx: secondary codec did not respond ...\n");

	chip->ac97[CS46XX_SECONDARY_CODEC_INDEX] = NULL;
	chip->nr_ac97_codecs = 1;
    
	/* well, one codec only ... */
	goto _end;
 _ok2:
	if ((err = snd_ac97_mixer(chip->ac97_bus, &ac97, &chip->ac97[CS46XX_SECONDARY_CODEC_INDEX])) < 0)
		return err;
	chip->nr_ac97_codecs = 2;

 _end:

#endif /* CONFIG_SND_CS46XX_NEW_DSP */

	/* add cs4630 mixer controls */
	for (idx = 0; idx < ARRAY_SIZE(snd_cs46xx_controls); idx++) {
		snd_kcontrol_t *kctl;
		kctl = snd_ctl_new1(&snd_cs46xx_controls[idx], chip);
		if ((err = snd_ctl_add(card, kctl)) < 0)
			return err;
	}

	/* get EAPD mixer switch (for voyetra hack) */
	memset(&id, 0, sizeof(id));
	id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	strcpy(id.name, "External Amplifier");
	chip->eapd_switch = snd_ctl_find_id(chip->card, &id);
    
#ifdef CONFIG_SND_CS46XX_NEW_DSP
	if (chip->nr_ac97_codecs == 1 && 
	    (snd_cs46xx_codec_read(chip, AC97_VENDOR_ID2,
				  CS46XX_PRIMARY_CODEC_INDEX) == 0x592b ||
	     snd_cs46xx_codec_read(chip, AC97_VENDOR_ID2,
				   CS46XX_PRIMARY_CODEC_INDEX) == 0x592d)) {
		/* set primary cs4294 codec into Extended Audio Mode */
		snd_printdd("setting EAM bit on cs4294 CODEC\n");
		snd_cs46xx_codec_write(chip, AC97_CSR_ACMODE, 0x200,
				       CS46XX_PRIMARY_CODEC_INDEX);
	}
	/* do soundcard specific mixer setup */
	if (chip->mixer_init) {
		snd_printdd ("calling chip->mixer_init(chip);\n");
		chip->mixer_init(chip);
	}
#endif

 	/* turn on amplifier */
	chip->amplifier_ctrl(chip, 1);
    
	return 0;
}

/*
 *  RawMIDI interface
 */

static void snd_cs46xx_midi_reset(cs46xx_t *chip)
{
	snd_cs46xx_pokeBA0(chip, BA0_MIDCR, MIDCR_MRST);
	udelay(100);
	snd_cs46xx_pokeBA0(chip, BA0_MIDCR, chip->midcr);
}

static int snd_cs46xx_midi_input_open(snd_rawmidi_substream_t * substream)
{
	unsigned long flags;
	cs46xx_t *chip = snd_magic_cast(cs46xx_t, substream->rmidi->private_data, return -ENXIO);

	chip->active_ctrl(chip, 1);
	spin_lock_irqsave(&chip->reg_lock, flags);
	chip->uartm |= CS46XX_MODE_INPUT;
	chip->midcr |= MIDCR_RXE;
	chip->midi_input = substream;
	if (!(chip->uartm & CS46XX_MODE_OUTPUT)) {
		snd_cs46xx_midi_reset(chip);
	} else {
		snd_cs46xx_pokeBA0(chip, BA0_MIDCR, chip->midcr);
	}
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return 0;
}

static int snd_cs46xx_midi_input_close(snd_rawmidi_substream_t * substream)
{
	unsigned long flags;
	cs46xx_t *chip = snd_magic_cast(cs46xx_t, substream->rmidi->private_data, return -ENXIO);

	spin_lock_irqsave(&chip->reg_lock, flags);
	chip->midcr &= ~(MIDCR_RXE | MIDCR_RIE);
	chip->midi_input = NULL;
	if (!(chip->uartm & CS46XX_MODE_OUTPUT)) {
		snd_cs46xx_midi_reset(chip);
	} else {
		snd_cs46xx_pokeBA0(chip, BA0_MIDCR, chip->midcr);
	}
	chip->uartm &= ~CS46XX_MODE_INPUT;
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	chip->active_ctrl(chip, -1);
	return 0;
}

static int snd_cs46xx_midi_output_open(snd_rawmidi_substream_t * substream)
{
	unsigned long flags;
	cs46xx_t *chip = snd_magic_cast(cs46xx_t, substream->rmidi->private_data, return -ENXIO);

	chip->active_ctrl(chip, 1);

	spin_lock_irqsave(&chip->reg_lock, flags);
	chip->uartm |= CS46XX_MODE_OUTPUT;
	chip->midcr |= MIDCR_TXE;
	chip->midi_output = substream;
	if (!(chip->uartm & CS46XX_MODE_INPUT)) {
		snd_cs46xx_midi_reset(chip);
	} else {
		snd_cs46xx_pokeBA0(chip, BA0_MIDCR, chip->midcr);
	}
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	return 0;
}

static int snd_cs46xx_midi_output_close(snd_rawmidi_substream_t * substream)
{
	unsigned long flags;
	cs46xx_t *chip = snd_magic_cast(cs46xx_t, substream->rmidi->private_data, return -ENXIO);

	spin_lock_irqsave(&chip->reg_lock, flags);
	chip->midcr &= ~(MIDCR_TXE | MIDCR_TIE);
	chip->midi_output = NULL;
	if (!(chip->uartm & CS46XX_MODE_INPUT)) {
		snd_cs46xx_midi_reset(chip);
	} else {
		snd_cs46xx_pokeBA0(chip, BA0_MIDCR, chip->midcr);
	}
	chip->uartm &= ~CS46XX_MODE_OUTPUT;
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	chip->active_ctrl(chip, -1);
	return 0;
}

static void snd_cs46xx_midi_input_trigger(snd_rawmidi_substream_t * substream, int up)
{
	unsigned long flags;
	cs46xx_t *chip = snd_magic_cast(cs46xx_t, substream->rmidi->private_data, return);

	spin_lock_irqsave(&chip->reg_lock, flags);
	if (up) {
		if ((chip->midcr & MIDCR_RIE) == 0) {
			chip->midcr |= MIDCR_RIE;
			snd_cs46xx_pokeBA0(chip, BA0_MIDCR, chip->midcr);
		}
	} else {
		if (chip->midcr & MIDCR_RIE) {
			chip->midcr &= ~MIDCR_RIE;
			snd_cs46xx_pokeBA0(chip, BA0_MIDCR, chip->midcr);
		}
	}
	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

static void snd_cs46xx_midi_output_trigger(snd_rawmidi_substream_t * substream, int up)
{
	unsigned long flags;
	cs46xx_t *chip = snd_magic_cast(cs46xx_t, substream->rmidi->private_data, return);
	unsigned char byte;

	spin_lock_irqsave(&chip->reg_lock, flags);
	if (up) {
		if ((chip->midcr & MIDCR_TIE) == 0) {
			chip->midcr |= MIDCR_TIE;
			/* fill UART FIFO buffer at first, and turn Tx interrupts only if necessary */
			while ((chip->midcr & MIDCR_TIE) &&
			       (snd_cs46xx_peekBA0(chip, BA0_MIDSR) & MIDSR_TBF) == 0) {
				if (snd_rawmidi_transmit(substream, &byte, 1) != 1) {
					chip->midcr &= ~MIDCR_TIE;
				} else {
					snd_cs46xx_pokeBA0(chip, BA0_MIDWP, byte);
				}
			}
			snd_cs46xx_pokeBA0(chip, BA0_MIDCR, chip->midcr);
		}
	} else {
		if (chip->midcr & MIDCR_TIE) {
			chip->midcr &= ~MIDCR_TIE;
			snd_cs46xx_pokeBA0(chip, BA0_MIDCR, chip->midcr);
		}
	}
	spin_unlock_irqrestore(&chip->reg_lock, flags);
}

static snd_rawmidi_ops_t snd_cs46xx_midi_output =
{
	.open =		snd_cs46xx_midi_output_open,
	.close =	snd_cs46xx_midi_output_close,
	.trigger =	snd_cs46xx_midi_output_trigger,
};

static snd_rawmidi_ops_t snd_cs46xx_midi_input =
{
	.open =		snd_cs46xx_midi_input_open,
	.close =	snd_cs46xx_midi_input_close,
	.trigger =	snd_cs46xx_midi_input_trigger,
};

int __devinit snd_cs46xx_midi(cs46xx_t *chip, int device, snd_rawmidi_t **rrawmidi)
{
	snd_rawmidi_t *rmidi;
	int err;

	if (rrawmidi)
		*rrawmidi = NULL;
	if ((err = snd_rawmidi_new(chip->card, "CS46XX", device, 1, 1, &rmidi)) < 0)
		return err;
	strcpy(rmidi->name, "CS46XX");
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &snd_cs46xx_midi_output);
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &snd_cs46xx_midi_input);
	rmidi->info_flags |= SNDRV_RAWMIDI_INFO_OUTPUT | SNDRV_RAWMIDI_INFO_INPUT | SNDRV_RAWMIDI_INFO_DUPLEX;
	rmidi->private_data = chip;
	chip->rmidi = rmidi;
	if (rrawmidi)
		*rrawmidi = NULL;
	return 0;
}


/*
 * gameport interface
 */

#if defined(CONFIG_GAMEPORT) || (defined(MODULE) && defined(CONFIG_GAMEPORT_MODULE))

typedef struct snd_cs46xx_gameport {
	struct gameport info;
	cs46xx_t *chip;
} cs46xx_gameport_t;

static void snd_cs46xx_gameport_trigger(struct gameport *gameport)
{
	cs46xx_gameport_t *gp = (cs46xx_gameport_t *)gameport;
	cs46xx_t *chip;
	snd_assert(gp, return);
	chip = snd_magic_cast(cs46xx_t, gp->chip, return);
	snd_cs46xx_pokeBA0(chip, BA0_JSPT, 0xFF);  //outb(gameport->io, 0xFF);
}

static unsigned char snd_cs46xx_gameport_read(struct gameport *gameport)
{
	cs46xx_gameport_t *gp = (cs46xx_gameport_t *)gameport;
	cs46xx_t *chip;
	snd_assert(gp, return 0);
	chip = snd_magic_cast(cs46xx_t, gp->chip, return 0);
	return snd_cs46xx_peekBA0(chip, BA0_JSPT); //inb(gameport->io);
}

static int snd_cs46xx_gameport_cooked_read(struct gameport *gameport, int *axes, int *buttons)
{
	cs46xx_gameport_t *gp = (cs46xx_gameport_t *)gameport;
	cs46xx_t *chip;
	unsigned js1, js2, jst;
	
	snd_assert(gp, return 0);
	chip = snd_magic_cast(cs46xx_t, gp->chip, return 0);

	js1 = snd_cs46xx_peekBA0(chip, BA0_JSC1);
	js2 = snd_cs46xx_peekBA0(chip, BA0_JSC2);
	jst = snd_cs46xx_peekBA0(chip, BA0_JSPT);
	
	*buttons = (~jst >> 4) & 0x0F; 
	
	axes[0] = ((js1 & JSC1_Y1V_MASK) >> JSC1_Y1V_SHIFT) & 0xFFFF;
	axes[1] = ((js1 & JSC1_X1V_MASK) >> JSC1_X1V_SHIFT) & 0xFFFF;
	axes[2] = ((js2 & JSC2_Y2V_MASK) >> JSC2_Y2V_SHIFT) & 0xFFFF;
	axes[3] = ((js2 & JSC2_X2V_MASK) >> JSC2_X2V_SHIFT) & 0xFFFF;

	for(jst=0;jst<4;++jst)
		if(axes[jst]==0xFFFF) axes[jst] = -1;
	return 0;
}

static int snd_cs46xx_gameport_open(struct gameport *gameport, int mode)
{
	switch (mode) {
	case GAMEPORT_MODE_COOKED:
		return 0;
	case GAMEPORT_MODE_RAW:
		return 0;
	default:
		return -1;
	}
	return 0;
}

void __devinit snd_cs46xx_gameport(cs46xx_t *chip)
{
	cs46xx_gameport_t *gp;
	gp = kmalloc(sizeof(*gp), GFP_KERNEL);
	if (! gp) {
		snd_printk("cannot allocate gameport area\n");
		return;
	}
	memset(gp, 0, sizeof(*gp));
	gp->info.open = snd_cs46xx_gameport_open;
	gp->info.read = snd_cs46xx_gameport_read;
	gp->info.trigger = snd_cs46xx_gameport_trigger;
	gp->info.cooked_read = snd_cs46xx_gameport_cooked_read;
	gp->chip = chip;
	chip->gameport = gp;

	snd_cs46xx_pokeBA0(chip, BA0_JSIO, 0xFF); // ?
	snd_cs46xx_pokeBA0(chip, BA0_JSCTL, JSCTL_SP_MEDIUM_SLOW);
	gameport_register_port(&gp->info);
}

#else

void __devinit snd_cs46xx_gameport(cs46xx_t *chip)
{
}

#endif /* CONFIG_GAMEPORT */

/*
 *  proc interface
 */

static long snd_cs46xx_io_read(snd_info_entry_t *entry, void *file_private_data,
			       struct file *file, char __user *buf,
			       unsigned long count, unsigned long pos)
{
	long size;
	snd_cs46xx_region_t *region = (snd_cs46xx_region_t *)entry->private_data;
	
	size = count;
	if (pos + (size_t)size > region->size)
		size = region->size - pos;
	if (size > 0) {
		if (copy_to_user_fromio(buf, region->remap_addr + pos, size))
			return -EFAULT;
	}
	return size;
}

static struct snd_info_entry_ops snd_cs46xx_proc_io_ops = {
	.read = snd_cs46xx_io_read,
};

static int __devinit snd_cs46xx_proc_init(snd_card_t * card, cs46xx_t *chip)
{
	snd_info_entry_t *entry;
	int idx;
	
	for (idx = 0; idx < 5; idx++) {
		snd_cs46xx_region_t *region = &chip->region.idx[idx];
		if (! snd_card_proc_new(card, region->name, &entry)) {
			entry->content = SNDRV_INFO_CONTENT_DATA;
			entry->private_data = chip;
			entry->c.ops = &snd_cs46xx_proc_io_ops;
			entry->size = region->size;
			entry->mode = S_IFREG | S_IRUSR;
		}
	}
#ifdef CONFIG_SND_CS46XX_NEW_DSP
	cs46xx_dsp_proc_init(card, chip);
#endif
	return 0;
}

static int snd_cs46xx_proc_done(cs46xx_t *chip)
{
#ifdef CONFIG_SND_CS46XX_NEW_DSP
	cs46xx_dsp_proc_done(chip);
#endif
	return 0;
}

/*
 * stop the h/w
 */
static void snd_cs46xx_hw_stop(cs46xx_t *chip)
{
	unsigned int tmp;

	tmp = snd_cs46xx_peek(chip, BA1_PFIE);
	tmp &= ~0x0000f03f;
	tmp |=  0x00000010;
	snd_cs46xx_poke(chip, BA1_PFIE, tmp);	/* playback interrupt disable */

	tmp = snd_cs46xx_peek(chip, BA1_CIE);
	tmp &= ~0x0000003f;
	tmp |=  0x00000011;
	snd_cs46xx_poke(chip, BA1_CIE, tmp);	/* capture interrupt disable */

	/*
         *  Stop playback DMA.
	 */
	tmp = snd_cs46xx_peek(chip, BA1_PCTL);
	snd_cs46xx_poke(chip, BA1_PCTL, tmp & 0x0000ffff);

	/*
         *  Stop capture DMA.
	 */
	tmp = snd_cs46xx_peek(chip, BA1_CCTL);
	snd_cs46xx_poke(chip, BA1_CCTL, tmp & 0xffff0000);

	/*
         *  Reset the processor.
         */
	snd_cs46xx_reset(chip);

	snd_cs46xx_proc_stop(chip);

	/*
	 *  Power down the PLL.
	 */
	snd_cs46xx_pokeBA0(chip, BA0_CLKCR1, 0);

	/*
	 *  Turn off the Processor by turning off the software clock enable flag in 
	 *  the clock control register.
	 */
	tmp = snd_cs46xx_peekBA0(chip, BA0_CLKCR1) & ~CLKCR1_SWCE;
	snd_cs46xx_pokeBA0(chip, BA0_CLKCR1, tmp);
}


static int snd_cs46xx_free(cs46xx_t *chip)
{
	int idx;

	snd_assert(chip != NULL, return -EINVAL);

	if (chip->active_ctrl)
		chip->active_ctrl(chip, 1);

#if defined(CONFIG_GAMEPORT) || (defined(MODULE) && defined(CONFIG_GAMEPORT_MODULE))
	if (chip->gameport) {
		gameport_unregister_port(&chip->gameport->info);
		kfree(chip->gameport);
	}
#endif

	if (chip->amplifier_ctrl)
		chip->amplifier_ctrl(chip, -chip->amplifier); /* force to off */
	
	snd_cs46xx_proc_done(chip);

	if (chip->region.idx[0].resource)
		snd_cs46xx_hw_stop(chip);

	for (idx = 0; idx < 5; idx++) {
		snd_cs46xx_region_t *region = &chip->region.idx[idx];
		if (region->remap_addr)
			iounmap((void *) region->remap_addr);
		if (region->resource) {
			release_resource(region->resource);
			kfree_nocheck(region->resource);
		}
	}
	if (chip->irq >= 0)
		free_irq(chip->irq, (void *)chip);

	if (chip->active_ctrl)
		chip->active_ctrl(chip, -chip->amplifier);
	
#ifdef CONFIG_SND_CS46XX_NEW_DSP
	if (chip->dsp_spos_instance) {
		cs46xx_dsp_spos_destroy(chip);
		chip->dsp_spos_instance = NULL;
	}
#endif
	
	snd_magic_kfree(chip);
	return 0;
}

static int snd_cs46xx_dev_free(snd_device_t *device)
{
	cs46xx_t *chip = snd_magic_cast(cs46xx_t, device->device_data, return -ENXIO);
	return snd_cs46xx_free(chip);
}

/*
 *  initialize chip
 */
static int snd_cs46xx_chip_init(cs46xx_t *chip)
{
	int timeout;

	/* 
	 *  First, blast the clock control register to zero so that the PLL starts
         *  out in a known state, and blast the master serial port control register
         *  to zero so that the serial ports also start out in a known state.
         */
        snd_cs46xx_pokeBA0(chip, BA0_CLKCR1, 0);
        snd_cs46xx_pokeBA0(chip, BA0_SERMC1, 0);

	/*
	 *  If we are in AC97 mode, then we must set the part to a host controlled
         *  AC-link.  Otherwise, we won't be able to bring up the link.
         */        
#ifdef CONFIG_SND_CS46XX_NEW_DSP
	snd_cs46xx_pokeBA0(chip, BA0_SERACC, SERACC_HSP | SERACC_CHIP_TYPE_2_0 | 
			   SERACC_TWO_CODECS);	/* 2.00 dual codecs */
	/* snd_cs46xx_pokeBA0(chip, BA0_SERACC, SERACC_HSP | SERACC_CHIP_TYPE_2_0); */ /* 2.00 codec */
#else
	snd_cs46xx_pokeBA0(chip, BA0_SERACC, SERACC_HSP | SERACC_CHIP_TYPE_1_03); /* 1.03 codec */
#endif

        /*
         *  Drive the ARST# pin low for a minimum of 1uS (as defined in the AC97
         *  spec) and then drive it high.  This is done for non AC97 modes since
         *  there might be logic external to the CS461x that uses the ARST# line
         *  for a reset.
         */
	snd_cs46xx_pokeBA0(chip, BA0_ACCTL, 0);
#ifdef CONFIG_SND_CS46XX_NEW_DSP
	snd_cs46xx_pokeBA0(chip, BA0_ACCTL2, 0);
#endif
	udelay(50);
	snd_cs46xx_pokeBA0(chip, BA0_ACCTL, ACCTL_RSTN);
#ifdef CONFIG_SND_CS46XX_NEW_DSP
	snd_cs46xx_pokeBA0(chip, BA0_ACCTL2, ACCTL_RSTN);
#endif
    
	/*
	 *  The first thing we do here is to enable sync generation.  As soon
	 *  as we start receiving bit clock, we'll start producing the SYNC
	 *  signal.
	 */
	snd_cs46xx_pokeBA0(chip, BA0_ACCTL, ACCTL_ESYN | ACCTL_RSTN);
#ifdef CONFIG_SND_CS46XX_NEW_DSP
	snd_cs46xx_pokeBA0(chip, BA0_ACCTL2, ACCTL_ESYN | ACCTL_RSTN);
#endif

	/*
	 *  Now wait for a short while to allow the AC97 part to start
	 *  generating bit clock (so we don't try to start the PLL without an
	 *  input clock).
	 */
	mdelay(10);

	/*
	 *  Set the serial port timing configuration, so that
	 *  the clock control circuit gets its clock from the correct place.
	 */
	snd_cs46xx_pokeBA0(chip, BA0_SERMC1, SERMC1_PTC_AC97);

	/*
	 *  Write the selected clock control setup to the hardware.  Do not turn on
	 *  SWCE yet (if requested), so that the devices clocked by the output of
	 *  PLL are not clocked until the PLL is stable.
	 */
	snd_cs46xx_pokeBA0(chip, BA0_PLLCC, PLLCC_LPF_1050_2780_KHZ | PLLCC_CDR_73_104_MHZ);
	snd_cs46xx_pokeBA0(chip, BA0_PLLM, 0x3a);
	snd_cs46xx_pokeBA0(chip, BA0_CLKCR2, CLKCR2_PDIVS_8);

	/*
	 *  Power up the PLL.
	 */
	snd_cs46xx_pokeBA0(chip, BA0_CLKCR1, CLKCR1_PLLP);

	/*
         *  Wait until the PLL has stabilized.
	 */
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(HZ/10); /* 100ms */

	/*
	 *  Turn on clocking of the core so that we can setup the serial ports.
	 */
	snd_cs46xx_pokeBA0(chip, BA0_CLKCR1, CLKCR1_PLLP | CLKCR1_SWCE);

	/*
	 * Enable FIFO  Host Bypass
	 */
	snd_cs46xx_pokeBA0(chip, BA0_SERBCF, SERBCF_HBP);

	/*
	 *  Fill the serial port FIFOs with silence.
	 */
	snd_cs46xx_clear_serial_FIFOs(chip);

	/*
	 *  Set the serial port FIFO pointer to the first sample in the FIFO.
	 */
	/* snd_cs46xx_pokeBA0(chip, BA0_SERBSP, 0); */

	/*
	 *  Write the serial port configuration to the part.  The master
	 *  enable bit is not set until all other values have been written.
	 */
	snd_cs46xx_pokeBA0(chip, BA0_SERC1, SERC1_SO1F_AC97 | SERC1_SO1EN);
	snd_cs46xx_pokeBA0(chip, BA0_SERC2, SERC2_SI1F_AC97 | SERC1_SO1EN);
	snd_cs46xx_pokeBA0(chip, BA0_SERMC1, SERMC1_PTC_AC97 | SERMC1_MSPE);


#ifdef CONFIG_SND_CS46XX_NEW_DSP
	snd_cs46xx_pokeBA0(chip, BA0_SERC7, SERC7_ASDI2EN);
	snd_cs46xx_pokeBA0(chip, BA0_SERC3, 0);
	snd_cs46xx_pokeBA0(chip, BA0_SERC4, 0);
	snd_cs46xx_pokeBA0(chip, BA0_SERC5, 0);
	snd_cs46xx_pokeBA0(chip, BA0_SERC6, 1);
#endif

	mdelay(5);


	/*
	 * Wait for the codec ready signal from the AC97 codec.
	 */
	timeout = 150;
	while (timeout-- > 0) {
		/*
		 *  Read the AC97 status register to see if we've seen a CODEC READY
		 *  signal from the AC97 codec.
		 */
		if (snd_cs46xx_peekBA0(chip, BA0_ACSTS) & ACSTS_CRDY)
			goto ok1;
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout((HZ+99)/100);
	}


	snd_printk("create - never read codec ready from AC'97\n");
	snd_printk("it is not probably bug, try to use CS4236 driver\n");
	return -EIO;
 ok1:
#ifdef CONFIG_SND_CS46XX_NEW_DSP
	{
		int count;
		for (count = 0; count < 150; count++) {
			/* First, we want to wait for a short time. */
			udelay(25);
        
			if (snd_cs46xx_peekBA0(chip, BA0_ACSTS2) & ACSTS_CRDY)
				break;
		}

		/*
		 *  Make sure CODEC is READY.
		 */
		if (!(snd_cs46xx_peekBA0(chip, BA0_ACSTS2) & ACSTS_CRDY))
			snd_printdd("cs46xx: never read card ready from secondary AC'97\n");
	}
#endif

	/*
	 *  Assert the vaid frame signal so that we can start sending commands
	 *  to the AC97 codec.
	 */
	snd_cs46xx_pokeBA0(chip, BA0_ACCTL, ACCTL_VFRM | ACCTL_ESYN | ACCTL_RSTN);
#ifdef CONFIG_SND_CS46XX_NEW_DSP
	snd_cs46xx_pokeBA0(chip, BA0_ACCTL2, ACCTL_VFRM | ACCTL_ESYN | ACCTL_RSTN);
#endif


	/*
	 *  Wait until we've sampled input slots 3 and 4 as valid, meaning that
	 *  the codec is pumping ADC data across the AC-link.
	 */
	timeout = 150;
	while (timeout-- > 0) {
		/*
		 *  Read the input slot valid register and see if input slots 3 and
		 *  4 are valid yet.
		 */
		if ((snd_cs46xx_peekBA0(chip, BA0_ACISV) & (ACISV_ISV3 | ACISV_ISV4)) == (ACISV_ISV3 | ACISV_ISV4))
			goto ok2;
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout((HZ+99)/100);
	}

#ifndef CONFIG_SND_CS46XX_NEW_DSP
	snd_printk("create - never read ISV3 & ISV4 from AC'97\n");
	return -EIO;
#else
	/* This may happen on a cold boot with a Terratec SiXPack 5.1.
	   Reloading the driver may help, if there's other soundcards 
	   with the same problem I would like to know. (Benny) */

	snd_printk("ERROR: snd-cs46xx: never read ISV3 & ISV4 from AC'97\n");
	snd_printk("       Try reloading the ALSA driver, if you find something\n");
        snd_printk("       broken or not working on your soundcard upon\n");
	snd_printk("       this message please report to alsa-devel@lists.sourceforge.net\n");

	return -EIO;
#endif
 ok2:

	/*
	 *  Now, assert valid frame and the slot 3 and 4 valid bits.  This will
	 *  commense the transfer of digital audio data to the AC97 codec.
	 */

	snd_cs46xx_pokeBA0(chip, BA0_ACOSV, ACOSV_SLV3 | ACOSV_SLV4);


	/*
	 *  Power down the DAC and ADC.  We will power them up (if) when we need
	 *  them.
	 */
	/* snd_cs46xx_pokeBA0(chip, BA0_AC97_POWERDOWN, 0x300); */

	/*
	 *  Turn off the Processor by turning off the software clock enable flag in 
	 *  the clock control register.
	 */
	/* tmp = snd_cs46xx_peekBA0(chip, BA0_CLKCR1) & ~CLKCR1_SWCE; */
	/* snd_cs46xx_pokeBA0(chip, BA0_CLKCR1, tmp); */

	return 0;
}

/*
 *  start and load DSP 
 */
int __devinit snd_cs46xx_start_dsp(cs46xx_t *chip)
{	
	unsigned int tmp;
	/*
	 *  Reset the processor.
	 */
	snd_cs46xx_reset(chip);
	/*
	 *  Download the image to the processor.
	 */
#ifdef CONFIG_SND_CS46XX_NEW_DSP
#if 0
	if (cs46xx_dsp_load_module(chip, &cwcemb80_module) < 0) {
		snd_printk(KERN_ERR "image download error\n");
		return -EIO;
	}
#endif

	if (cs46xx_dsp_load_module(chip, &cwc4630_module) < 0) {
		snd_printk(KERN_ERR "image download error [cwc4630]\n");
		return -EIO;
	}

	if (cs46xx_dsp_load_module(chip, &cwcasync_module) < 0) {
		snd_printk(KERN_ERR "image download error [cwcasync]\n");
		return -EIO;
	}

	if (cs46xx_dsp_load_module(chip, &cwcsnoop_module) < 0) {
		snd_printk(KERN_ERR "image download error [cwcsnoop]\n");
		return -EIO;
	}

	if (cs46xx_dsp_load_module(chip, &cwcbinhack_module) < 0) {
		snd_printk(KERN_ERR "image download error [cwcbinhack]\n");
		return -EIO;
	}

	if (cs46xx_dsp_load_module(chip, &cwcdma_module) < 0) {
		snd_printk(KERN_ERR "image download error [cwcdma]\n");
		return -EIO;
	}

	if (cs46xx_dsp_scb_and_task_init(chip) < 0)
		return -EIO;
#else
	/* old image */
	if (snd_cs46xx_download_image(chip) < 0) {
		snd_printk("image download error\n");
		return -EIO;
	}

	/*
         *  Stop playback DMA.
	 */
	tmp = snd_cs46xx_peek(chip, BA1_PCTL);
	chip->play_ctl = tmp & 0xffff0000;
	snd_cs46xx_poke(chip, BA1_PCTL, tmp & 0x0000ffff);
#endif

	/*
         *  Stop capture DMA.
	 */
	tmp = snd_cs46xx_peek(chip, BA1_CCTL);
	chip->capt.ctl = tmp & 0x0000ffff;
	snd_cs46xx_poke(chip, BA1_CCTL, tmp & 0xffff0000);

	mdelay(5);

	snd_cs46xx_set_play_sample_rate(chip, 8000);
	snd_cs46xx_set_capture_sample_rate(chip, 8000);

	snd_cs46xx_proc_start(chip);

	/*
	 *  Enable interrupts on the part.
	 */
	snd_cs46xx_pokeBA0(chip, BA0_HICR, HICR_IEV | HICR_CHGM);
        
	tmp = snd_cs46xx_peek(chip, BA1_PFIE);
	tmp &= ~0x0000f03f;
	snd_cs46xx_poke(chip, BA1_PFIE, tmp);	/* playback interrupt enable */

	tmp = snd_cs46xx_peek(chip, BA1_CIE);
	tmp &= ~0x0000003f;
	tmp |=  0x00000001;
	snd_cs46xx_poke(chip, BA1_CIE, tmp);	/* capture interrupt enable */
	
#ifndef CONFIG_SND_CS46XX_NEW_DSP
	/* set the attenuation to 0dB */ 
	snd_cs46xx_poke(chip, BA1_PVOL, 0x80008000);
	snd_cs46xx_poke(chip, BA1_CVOL, 0x80008000);
#endif

	return 0;
}


/*
 *	AMP control - null AMP
 */
 
static void amp_none(cs46xx_t *chip, int change)
{	
}

#ifdef CONFIG_SND_CS46XX_NEW_DSP
static int voyetra_setup_eapd_slot(cs46xx_t *chip)
{
	
	u32 idx, valid_slots,tmp,powerdown = 0;
	u16 modem_power,pin_config,logic_type;

	snd_printdd ("cs46xx: cs46xx_setup_eapd_slot()+\n");

	/*
	 *  See if the devices are powered down.  If so, we must power them up first
	 *  or they will not respond.
	 */
	tmp = snd_cs46xx_peekBA0(chip, BA0_CLKCR1);

	if (!(tmp & CLKCR1_SWCE)) {
		snd_cs46xx_pokeBA0(chip, BA0_CLKCR1, tmp | CLKCR1_SWCE);
		powerdown = 1;
	}

	/*
	 * Clear PRA.  The Bonzo chip will be used for GPIO not for modem
	 * stuff.
	 */
	if(chip->nr_ac97_codecs != 2) {
		snd_printk (KERN_ERR "cs46xx: cs46xx_setup_eapd_slot() - no secondary codec configured\n");
		return -EINVAL;
	}

	modem_power = snd_cs46xx_codec_read (chip, 
					     AC97_EXTENDED_MSTATUS,
					     CS46XX_SECONDARY_CODEC_INDEX);
	modem_power &=0xFEFF;

	snd_cs46xx_codec_write(chip, 
			       AC97_EXTENDED_MSTATUS, modem_power,
			       CS46XX_SECONDARY_CODEC_INDEX);

	/*
	 * Set GPIO pin's 7 and 8 so that they are configured for output.
	 */
	pin_config = snd_cs46xx_codec_read (chip, 
					    AC97_GPIO_CFG,
					    CS46XX_SECONDARY_CODEC_INDEX);
	pin_config &=0x27F;

	snd_cs46xx_codec_write(chip, 
			       AC97_GPIO_CFG, pin_config,
			       CS46XX_SECONDARY_CODEC_INDEX);
    
	/*
	 * Set GPIO pin's 7 and 8 so that they are compatible with CMOS logic.
	 */

	logic_type = snd_cs46xx_codec_read(chip, AC97_GPIO_POLARITY,
					   CS46XX_SECONDARY_CODEC_INDEX);
	logic_type &=0x27F; 

	snd_cs46xx_codec_write (chip, AC97_GPIO_POLARITY, logic_type,
				CS46XX_SECONDARY_CODEC_INDEX);

	valid_slots = snd_cs46xx_peekBA0(chip, BA0_ACOSV);
	valid_slots |= 0x200;
	snd_cs46xx_pokeBA0(chip, BA0_ACOSV, valid_slots);

	if ( cs46xx_wait_for_fifo(chip,1) ) {
	  snd_printdd("FIFO is busy\n");
	  
	  return -EINVAL;
	}

	/*
	 * Fill slots 12 with the correct value for the GPIO pins. 
	 */
	for(idx = 0x90; idx <= 0x9F; idx++) {
		/*
		 * Initialize the fifo so that bits 7 and 8 are on.
		 *
		 * Remember that the GPIO pins in bonzo are shifted by 4 bits to
		 * the left.  0x1800 corresponds to bits 7 and 8.
		 */
		snd_cs46xx_pokeBA0(chip, BA0_SERBWP, 0x1800);

		/*
		 * Wait for command to complete
		 */
		if ( cs46xx_wait_for_fifo(chip,200) ) {
			snd_printdd("failed waiting for FIFO at addr (%02X)\n",idx);

			return -EINVAL;
		}
            
		/*
		 * Write the serial port FIFO index.
		 */
		snd_cs46xx_pokeBA0(chip, BA0_SERBAD, idx);
      
		/*
		 * Tell the serial port to load the new value into the FIFO location.
		 */
		snd_cs46xx_pokeBA0(chip, BA0_SERBCM, SERBCM_WRC);
	}

	/* wait for last command to complete */
	cs46xx_wait_for_fifo(chip,200);

	/*
	 *  Now, if we powered up the devices, then power them back down again.
	 *  This is kinda ugly, but should never happen.
	 */
	if (powerdown)
		snd_cs46xx_pokeBA0(chip, BA0_CLKCR1, tmp);

	return 0;
}
#endif

/*
 *	Crystal EAPD mode
 */
 
static void amp_voyetra(cs46xx_t *chip, int change)
{
	/* Manage the EAPD bit on the Crystal 4297 
	   and the Analog AD1885 */
	   
#ifdef CONFIG_SND_CS46XX_NEW_DSP
	int old = chip->amplifier;
#endif
	int oval, val;
	
	chip->amplifier += change;
	oval = snd_cs46xx_codec_read(chip, AC97_POWERDOWN,
				     CS46XX_PRIMARY_CODEC_INDEX);
	val = oval;
	if (chip->amplifier) {
		/* Turn the EAPD amp on */
		val |= 0x8000;
	} else {
		/* Turn the EAPD amp off */
		val &= ~0x8000;
	}
	if (val != oval) {
		snd_cs46xx_codec_write(chip, AC97_POWERDOWN, val,
				       CS46XX_PRIMARY_CODEC_INDEX);
		if (chip->eapd_switch)
			snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &chip->eapd_switch->id);
	}

#ifdef CONFIG_SND_CS46XX_NEW_DSP
	if (chip->amplifier && !old) {
		voyetra_setup_eapd_slot(chip);
	}
#endif
}

static void hercules_init(cs46xx_t *chip) 
{
	/* default: AMP off, and SPDIF input optical */
	snd_cs46xx_pokeBA0(chip, BA0_EGPIODR, EGPIODR_GPOE0);
	snd_cs46xx_pokeBA0(chip, BA0_EGPIOPTR, EGPIODR_GPOE0);
}


/*
 *	Game Theatre XP card - EGPIO[2] is used to enable the external amp.
 */ 
static void amp_hercules(cs46xx_t *chip, int change)
{
	int old = chip->amplifier;
	int val1 = snd_cs46xx_peekBA0(chip, BA0_EGPIODR);
	int val2 = snd_cs46xx_peekBA0(chip, BA0_EGPIOPTR);

	chip->amplifier += change;
	if (chip->amplifier && !old) {
		snd_printdd ("Hercules amplifier ON\n");

		snd_cs46xx_pokeBA0(chip, BA0_EGPIODR, 
				   EGPIODR_GPOE2 | val1);     /* enable EGPIO2 output */
		snd_cs46xx_pokeBA0(chip, BA0_EGPIOPTR, 
				   EGPIOPTR_GPPT2 | val2);   /* open-drain on output */
	} else if (old && !chip->amplifier) {
		snd_printdd ("Hercules amplifier OFF\n");
		snd_cs46xx_pokeBA0(chip, BA0_EGPIODR,  val1 & ~EGPIODR_GPOE2); /* disable */
		snd_cs46xx_pokeBA0(chip, BA0_EGPIOPTR, val2 & ~EGPIOPTR_GPPT2); /* disable */
	}
}

static void voyetra_mixer_init (cs46xx_t *chip)
{
	snd_printdd ("initializing Voyetra mixer\n");

	/* Enable SPDIF out */
	snd_cs46xx_pokeBA0(chip, BA0_EGPIODR, EGPIODR_GPOE0);
	snd_cs46xx_pokeBA0(chip, BA0_EGPIOPTR, EGPIODR_GPOE0);
}

static void hercules_mixer_init (cs46xx_t *chip)
{
#ifdef CONFIG_SND_CS46XX_NEW_DSP
	unsigned int idx;
	int err;
	snd_card_t *card = chip->card;
#endif

	/* set EGPIO to default */
	hercules_init(chip);

	snd_printdd ("initializing Hercules mixer\n");

#ifdef CONFIG_SND_CS46XX_NEW_DSP
	for (idx = 0 ; idx < ARRAY_SIZE(snd_hercules_controls); idx++) {
		snd_kcontrol_t *kctl;

		kctl = snd_ctl_new1(&snd_hercules_controls[idx], chip);
		if ((err = snd_ctl_add(card, kctl)) < 0) {
			printk (KERN_ERR "cs46xx: failed to initialize Hercules mixer (%d)\n",err);
			break;
		}
	}
#endif
}


#if 0
/*
 *	Untested
 */
 
static void amp_voyetra_4294(cs46xx_t *chip, int change)
{
	chip->amplifier += change;

	if (chip->amplifier) {
		/* Switch the GPIO pins 7 and 8 to open drain */
		snd_cs46xx_codec_write(chip, 0x4C,
				       snd_cs46xx_codec_read(chip, 0x4C) & 0xFE7F);
		snd_cs46xx_codec_write(chip, 0x4E,
				       snd_cs46xx_codec_read(chip, 0x4E) | 0x0180);
		/* Now wake the AMP (this might be backwards) */
		snd_cs46xx_codec_write(chip, 0x54,
				       snd_cs46xx_codec_read(chip, 0x54) & ~0x0180);
	} else {
		snd_cs46xx_codec_write(chip, 0x54,
				       snd_cs46xx_codec_read(chip, 0x54) | 0x0180);
	}
}
#endif


/*
 * piix4 pci ids
 */
#ifndef PCI_VENDOR_ID_INTEL
#define PCI_VENDOR_ID_INTEL 0x8086
#endif /* PCI_VENDOR_ID_INTEL */

#ifndef PCI_DEVICE_ID_INTEL_82371AB_3
#define PCI_DEVICE_ID_INTEL_82371AB_3 0x7113
#endif /* PCI_DEVICE_ID_INTEL_82371AB_3 */

/*
 *	Handle the CLKRUN on a thinkpad. We must disable CLKRUN support
 *	whenever we need to beat on the chip.
 *
 *	The original idea and code for this hack comes from David Kaiser at
 *	Linuxcare. Perhaps one day Crystal will document their chips well
 *	enough to make them useful.
 */
 
static void clkrun_hack(cs46xx_t *chip, int change)
{
	u16 control, nval;
	
	if (chip->acpi_dev == NULL)
		return;

	chip->amplifier += change;
	
	/* Read ACPI port */	
	nval = control = inw(chip->acpi_port + 0x10);

	/* Flip CLKRUN off while running */
	if (! chip->amplifier)
		nval |= 0x2000;
	else
		nval &= ~0x2000;
	if (nval != control)
		outw(nval, chip->acpi_port + 0x10);
}

	
/*
 * detect intel piix4
 */
static void clkrun_init(cs46xx_t *chip)
{
	u8 pp;

	chip->acpi_dev = pci_find_device(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82371AB_3, NULL);
	if (chip->acpi_dev == NULL)
		return;		/* Not a thinkpad thats for sure */

	/* Find the control port */		
	pci_read_config_byte(chip->acpi_dev, 0x41, &pp);
	chip->acpi_port = pp << 8;
}


/*
 * Card subid table
 */
 
struct cs_card_type
{
	u16 vendor;
	u16 id;
	char *name;
	void (*init)(cs46xx_t *);
	void (*amp)(cs46xx_t *, int);
	void (*active)(cs46xx_t *, int);
	void (*mixer_init)(cs46xx_t *);
};

static struct cs_card_type __devinitdata cards[] = {
	{
		.vendor = 0x1489,
		.id = 0x7001,
		.name = "Genius Soundmaker 128 value",
		/* nothing special */
	},
	{
		.vendor = 0x5053,
		.id = 0x3357,
		.name = "Voyetra",
		.amp = amp_voyetra,
		.mixer_init = voyetra_mixer_init,
	},
	{
		.vendor = 0x1071,
		.id = 0x6003,
		.name = "Mitac MI6020/21",
		.amp = amp_voyetra,
	},
	{
		.vendor = 0x14AF,
		.id = 0x0050,
		.name = "Hercules Game Theatre XP",
		.amp = amp_hercules,
		.mixer_init = hercules_mixer_init,
	},
	{
		.vendor = 0x1681,
		.id = 0x0050,
		.name = "Hercules Game Theatre XP",
		.amp = amp_hercules,
		.mixer_init = hercules_mixer_init,
	},
	{
		.vendor = 0x1681,
		.id = 0x0051,
		.name = "Hercules Game Theatre XP",
		.amp = amp_hercules,
		.mixer_init = hercules_mixer_init,

	},
	{
		.vendor = 0x1681,
		.id = 0x0052,
		.name = "Hercules Game Theatre XP",
		.amp = amp_hercules,
		.mixer_init = hercules_mixer_init,
	},
	{
		.vendor = 0x1681,
		.id = 0x0053,
		.name = "Hercules Game Theatre XP",
		.amp = amp_hercules,
		.mixer_init = hercules_mixer_init,
	},
	{
		.vendor = 0x1681,
		.id = 0x0054,
		.name = "Hercules Game Theatre XP",
		.amp = amp_hercules,
		.mixer_init = hercules_mixer_init,
	},
	/* Teratec */
	{
		.vendor = 0x153b,
		.id = 0x1136,
		.name = "Terratec SiXPack 5.1",
	},
	/* Not sure if the 570 needs the clkrun hack */
	{
		.vendor = PCI_VENDOR_ID_IBM,
		.id = 0x0132,
		.name = "Thinkpad 570",
		.init = clkrun_init,
		.active = clkrun_hack,
	},
	{
		.vendor = PCI_VENDOR_ID_IBM,
		.id = 0x0153,
		.name = "Thinkpad 600X/A20/T20",
		.init = clkrun_init,
		.active = clkrun_hack,
	},
	{
		.vendor = PCI_VENDOR_ID_IBM,
		.id = 0x1010,
		.name = "Thinkpad 600E (unsupported)",
	},
	{} /* terminator */
};


/*
 * APM support
 */
#ifdef CONFIG_PM
static int snd_cs46xx_suspend(snd_card_t *card, unsigned int state)
{
	cs46xx_t *chip = snd_magic_cast(cs46xx_t, card->pm_private_data, return -EINVAL);
	int amp_saved;

	snd_pcm_suspend_all(chip->pcm);
	// chip->ac97_powerdown = snd_cs46xx_codec_read(chip, AC97_POWER_CONTROL);
	// chip->ac97_general_purpose = snd_cs46xx_codec_read(chip, BA0_AC97_GENERAL_PURPOSE);

	snd_ac97_suspend(chip->ac97[CS46XX_PRIMARY_CODEC_INDEX]);
	if (chip->ac97[CS46XX_SECONDARY_CODEC_INDEX])
		snd_ac97_suspend(chip->ac97[CS46XX_SECONDARY_CODEC_INDEX]);

	amp_saved = chip->amplifier;
	/* turn off amp */
	chip->amplifier_ctrl(chip, -chip->amplifier);
	snd_cs46xx_hw_stop(chip);
	/* disable CLKRUN */
	chip->active_ctrl(chip, -chip->amplifier);
	chip->amplifier = amp_saved; /* restore the status */
	snd_power_change_state(card, SNDRV_CTL_POWER_D3hot);
	return 0;
}

static int snd_cs46xx_resume(snd_card_t *card, unsigned int state)
{
	cs46xx_t *chip = snd_magic_cast(cs46xx_t, card->pm_private_data, return -EINVAL);
	int amp_saved;

	pci_enable_device(chip->pci);
	amp_saved = chip->amplifier;
	chip->amplifier = 0;
	chip->active_ctrl(chip, 1); /* force to on */

	snd_cs46xx_chip_init(chip);

#if 0
	snd_cs46xx_codec_write(chip, BA0_AC97_GENERAL_PURPOSE, 
			       chip->ac97_general_purpose);
	snd_cs46xx_codec_write(chip, AC97_POWER_CONTROL, 
			       chip->ac97_powerdown);
	mdelay(10);
	snd_cs46xx_codec_write(chip, BA0_AC97_POWERDOWN,
			       chip->ac97_powerdown);
	mdelay(5);
#endif

	snd_ac97_resume(chip->ac97[CS46XX_PRIMARY_CODEC_INDEX]);
	if (chip->ac97[CS46XX_SECONDARY_CODEC_INDEX])
		snd_ac97_resume(chip->ac97[CS46XX_SECONDARY_CODEC_INDEX]);

	if (amp_saved)
		chip->amplifier_ctrl(chip, 1); /* turn amp on */
	else
		chip->active_ctrl(chip, -1); /* disable CLKRUN */
	chip->amplifier = amp_saved;
	snd_power_change_state(card, SNDRV_CTL_POWER_D0);
	return 0;
}
#endif /* CONFIG_PM */


/*
 */

int __devinit snd_cs46xx_create(snd_card_t * card,
		      struct pci_dev * pci,
		      int external_amp, int thinkpad,
		      cs46xx_t ** rchip)
{
	cs46xx_t *chip;
	int err, idx;
	snd_cs46xx_region_t *region;
	struct cs_card_type *cp;
	u16 ss_card, ss_vendor;
	static snd_device_ops_t ops = {
		.dev_free =	snd_cs46xx_dev_free,
	};
	
	*rchip = NULL;

	/* enable PCI device */
	if ((err = pci_enable_device(pci)) < 0)
		return err;

	chip = snd_magic_kcalloc(cs46xx_t, 0, GFP_KERNEL);
	if (chip == NULL)
		return -ENOMEM;
	spin_lock_init(&chip->reg_lock);
#ifdef CONFIG_SND_CS46XX_NEW_DSP
	init_MUTEX(&chip->spos_mutex);
#endif
	chip->card = card;
	chip->pci = pci;
	chip->irq = -1;
	chip->ba0_addr = pci_resource_start(pci, 0);
	chip->ba1_addr = pci_resource_start(pci, 1);
	if (chip->ba0_addr == 0 || chip->ba0_addr == (unsigned long)~0 ||
	    chip->ba1_addr == 0 || chip->ba1_addr == (unsigned long)~0) {
	    	snd_printk("wrong address(es) - ba0 = 0x%lx, ba1 = 0x%lx\n", chip->ba0_addr, chip->ba1_addr);
	    	snd_cs46xx_free(chip);
	    	return -ENOMEM;
	}

	region = &chip->region.name.ba0;
	strcpy(region->name, "CS46xx_BA0");
	region->base = chip->ba0_addr;
	region->size = CS46XX_BA0_SIZE;

	region = &chip->region.name.data0;
	strcpy(region->name, "CS46xx_BA1_data0");
	region->base = chip->ba1_addr + BA1_SP_DMEM0;
	region->size = CS46XX_BA1_DATA0_SIZE;

	region = &chip->region.name.data1;
	strcpy(region->name, "CS46xx_BA1_data1");
	region->base = chip->ba1_addr + BA1_SP_DMEM1;
	region->size = CS46XX_BA1_DATA1_SIZE;

	region = &chip->region.name.pmem;
	strcpy(region->name, "CS46xx_BA1_pmem");
	region->base = chip->ba1_addr + BA1_SP_PMEM;
	region->size = CS46XX_BA1_PRG_SIZE;

	region = &chip->region.name.reg;
	strcpy(region->name, "CS46xx_BA1_reg");
	region->base = chip->ba1_addr + BA1_SP_REG;
	region->size = CS46XX_BA1_REG_SIZE;

	memset(&chip->dma_dev, 0, sizeof(chip->dma_dev));
	chip->dma_dev.type = SNDRV_DMA_TYPE_DEV;
	chip->dma_dev.dev = snd_dma_pci_data(pci);

	/* set up amp and clkrun hack */
	pci_read_config_word(pci, PCI_SUBSYSTEM_VENDOR_ID, &ss_vendor);
	pci_read_config_word(pci, PCI_SUBSYSTEM_ID, &ss_card);

	for (cp = &cards[0]; cp->name; cp++) {
		if (cp->vendor == ss_vendor && cp->id == ss_card) {
			snd_printdd ("hack for %s enabled\n", cp->name);

			chip->amplifier_ctrl = cp->amp;
			chip->active_ctrl = cp->active;
			chip->mixer_init = cp->mixer_init;

			if (cp->init)
				cp->init(chip);
			break;
		}
	}

	if (external_amp) {
		snd_printk("Crystal EAPD support forced on.\n");
		chip->amplifier_ctrl = amp_voyetra;
	}

	if (thinkpad) {
		snd_printk("Activating CLKRUN hack for Thinkpad.\n");
		chip->active_ctrl = clkrun_hack;
		clkrun_init(chip);
	}
	
	if (chip->amplifier_ctrl == NULL)
		chip->amplifier_ctrl = amp_none;
	if (chip->active_ctrl == NULL)
		chip->active_ctrl = amp_none;

	chip->active_ctrl(chip, 1); /* enable CLKRUN */

	pci_set_master(pci);

	for (idx = 0; idx < 5; idx++) {
		region = &chip->region.idx[idx];
		if ((region->resource = request_mem_region(region->base, region->size, region->name)) == NULL) {
			snd_printk("unable to request memory region 0x%lx-0x%lx\n", region->base, region->base + region->size - 1);
			snd_cs46xx_free(chip);
			return -EBUSY;
		}
		region->remap_addr = (unsigned long) ioremap_nocache(region->base, region->size);
		if (region->remap_addr == 0) {
			snd_printk("%s ioremap problem\n", region->name);
			snd_cs46xx_free(chip);
			return -ENOMEM;
		}
	}

	if (request_irq(pci->irq, snd_cs46xx_interrupt, SA_INTERRUPT|SA_SHIRQ, "CS46XX", (void *) chip)) {
		snd_printk("unable to grab IRQ %d\n", pci->irq);
		snd_cs46xx_free(chip);
		return -EBUSY;
	}
	chip->irq = pci->irq;

#ifdef CONFIG_SND_CS46XX_NEW_DSP
	chip->dsp_spos_instance = cs46xx_dsp_spos_create(chip);
	if (chip->dsp_spos_instance == NULL) {
		snd_cs46xx_free(chip);
		return -ENOMEM;
	}
#endif

	err = snd_cs46xx_chip_init(chip);
	if (err < 0) {
		snd_cs46xx_free(chip);
		return err;
	}

	snd_cs46xx_proc_init(card, chip);

	snd_card_set_pm_callback(card, snd_cs46xx_suspend, snd_cs46xx_resume, chip);

	if ((err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, chip, &ops)) < 0) {
		snd_cs46xx_free(chip);
		return err;
	}
	
	chip->active_ctrl(chip, -1); /* disable CLKRUN */

	snd_card_set_dev(card, &pci->dev);

	*rchip = chip;
	return 0;
}
