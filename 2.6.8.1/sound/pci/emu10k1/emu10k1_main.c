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
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <sound/core.h>
#include <sound/emu10k1.h>

#if 0
MODULE_AUTHOR("Jaroslav Kysela <perex@suse.cz>, Creative Labs, Inc.");
MODULE_DESCRIPTION("Routines for control of EMU10K1 chips");
MODULE_LICENSE("GPL");
#endif

/*************************************************************************
 * EMU10K1 init / done
 *************************************************************************/

void snd_emu10k1_voice_init(emu10k1_t * emu, int ch)
{
	snd_emu10k1_ptr_write(emu, DCYSUSV, ch, 0);
	snd_emu10k1_ptr_write(emu, IP, ch, 0);
	snd_emu10k1_ptr_write(emu, VTFT, ch, 0xffff);
	snd_emu10k1_ptr_write(emu, CVCF, ch, 0xffff);
	snd_emu10k1_ptr_write(emu, PTRX, ch, 0);
	snd_emu10k1_ptr_write(emu, CPF, ch, 0);
	snd_emu10k1_ptr_write(emu, CCR, ch, 0);

	snd_emu10k1_ptr_write(emu, PSST, ch, 0);
	snd_emu10k1_ptr_write(emu, DSL, ch, 0x10);
	snd_emu10k1_ptr_write(emu, CCCA, ch, 0);
	snd_emu10k1_ptr_write(emu, Z1, ch, 0);
	snd_emu10k1_ptr_write(emu, Z2, ch, 0);
	snd_emu10k1_ptr_write(emu, FXRT, ch, 0x32100000);

	snd_emu10k1_ptr_write(emu, ATKHLDM, ch, 0);
	snd_emu10k1_ptr_write(emu, DCYSUSM, ch, 0);
	snd_emu10k1_ptr_write(emu, IFATN, ch, 0xffff);
	snd_emu10k1_ptr_write(emu, PEFE, ch, 0);
	snd_emu10k1_ptr_write(emu, FMMOD, ch, 0);
	snd_emu10k1_ptr_write(emu, TREMFRQ, ch, 24);	/* 1 Hz */
	snd_emu10k1_ptr_write(emu, FM2FRQ2, ch, 24);	/* 1 Hz */
	snd_emu10k1_ptr_write(emu, TEMPENV, ch, 0);

	/*** these are last so OFF prevents writing ***/
	snd_emu10k1_ptr_write(emu, LFOVAL2, ch, 0);
	snd_emu10k1_ptr_write(emu, LFOVAL1, ch, 0);
	snd_emu10k1_ptr_write(emu, ATKHLDV, ch, 0);
	snd_emu10k1_ptr_write(emu, ENVVOL, ch, 0);
	snd_emu10k1_ptr_write(emu, ENVVAL, ch, 0);

	/* Audigy extra stuffs */
	if (emu->audigy) {
		snd_emu10k1_ptr_write(emu, 0x4c, ch, 0); /* ?? */
		snd_emu10k1_ptr_write(emu, 0x4d, ch, 0); /* ?? */
		snd_emu10k1_ptr_write(emu, 0x4e, ch, 0); /* ?? */
		snd_emu10k1_ptr_write(emu, 0x4f, ch, 0); /* ?? */
		snd_emu10k1_ptr_write(emu, A_FXRT1, ch, 0x03020100);
		snd_emu10k1_ptr_write(emu, A_FXRT2, ch, 0x3f3f3f3f);
		snd_emu10k1_ptr_write(emu, A_SENDAMOUNTS, ch, 0);
	}
}

static int __devinit snd_emu10k1_init(emu10k1_t * emu, int enable_ir)
{
	int ch, idx, err;
	unsigned int silent_page;

	emu->fx8010.itram_size = (16 * 1024)/2;
	emu->fx8010.etram_pages.area = NULL;
	emu->fx8010.etram_pages.bytes = 0;

	/* disable audio and lock cache */
	outl(HCFG_LOCKSOUNDCACHE | HCFG_LOCKTANKCACHE_MASK | HCFG_MUTEBUTTONENABLE, emu->port + HCFG);

	/* reset recording buffers */
	snd_emu10k1_ptr_write(emu, MICBS, 0, ADCBS_BUFSIZE_NONE);
	snd_emu10k1_ptr_write(emu, MICBA, 0, 0);
	snd_emu10k1_ptr_write(emu, FXBS, 0, ADCBS_BUFSIZE_NONE);
	snd_emu10k1_ptr_write(emu, FXBA, 0, 0);
	snd_emu10k1_ptr_write(emu, ADCBS, 0, ADCBS_BUFSIZE_NONE);
	snd_emu10k1_ptr_write(emu, ADCBA, 0, 0);

	/* disable channel interrupt */
	outl(0, emu->port + INTE);
	snd_emu10k1_ptr_write(emu, CLIEL, 0, 0);
	snd_emu10k1_ptr_write(emu, CLIEH, 0, 0);
	snd_emu10k1_ptr_write(emu, SOLEL, 0, 0);
	snd_emu10k1_ptr_write(emu, SOLEH, 0, 0);

	if (emu->audigy){
		snd_emu10k1_ptr_write(emu, 0x5e, 0, 0xf00); /* ?? */
		snd_emu10k1_ptr_write(emu, 0x5f, 0, 0x3); /* ?? */
	}

	/* init envelope engine */
	for (ch = 0; ch < NUM_G; ch++) {
		emu->voices[ch].emu = emu;
		emu->voices[ch].number = ch;
		snd_emu10k1_voice_init(emu, ch);
	}

	/*
	 *  Init to 0x02109204 :
	 *  Clock accuracy    = 0     (1000ppm)
	 *  Sample Rate       = 2     (48kHz)
	 *  Audio Channel     = 1     (Left of 2)
	 *  Source Number     = 0     (Unspecified)
	 *  Generation Status = 1     (Original for Cat Code 12)
	 *  Cat Code          = 12    (Digital Signal Mixer)
	 *  Mode              = 0     (Mode 0)
	 *  Emphasis          = 0     (None)
	 *  CP                = 1     (Copyright unasserted)
	 *  AN                = 0     (Audio data)
	 *  P                 = 0     (Consumer)
	 */
	snd_emu10k1_ptr_write(emu, SPCS0, 0,
			emu->spdif_bits[0] =
			SPCS_CLKACCY_1000PPM | SPCS_SAMPLERATE_48 |
			SPCS_CHANNELNUM_LEFT | SPCS_SOURCENUM_UNSPEC |
			SPCS_GENERATIONSTATUS | 0x00001200 |
			0x00000000 | SPCS_EMPHASIS_NONE | SPCS_COPYRIGHT);
	snd_emu10k1_ptr_write(emu, SPCS1, 0,
			emu->spdif_bits[1] =
			SPCS_CLKACCY_1000PPM | SPCS_SAMPLERATE_48 |
			SPCS_CHANNELNUM_LEFT | SPCS_SOURCENUM_UNSPEC |
			SPCS_GENERATIONSTATUS | 0x00001200 |
			0x00000000 | SPCS_EMPHASIS_NONE | SPCS_COPYRIGHT);
	snd_emu10k1_ptr_write(emu, SPCS2, 0,
			emu->spdif_bits[2] =
			SPCS_CLKACCY_1000PPM | SPCS_SAMPLERATE_48 |
			SPCS_CHANNELNUM_LEFT | SPCS_SOURCENUM_UNSPEC |
			SPCS_GENERATIONSTATUS | 0x00001200 |
			0x00000000 | SPCS_EMPHASIS_NONE | SPCS_COPYRIGHT);

	if (emu->audigy && emu->revision == 4) { /* audigy2 */
		/* Hacks for Alice3 to work independent of haP16V driver */
		u32 tmp;

		//Setup SRCMulti_I2S SamplingRate
		tmp = snd_emu10k1_ptr_read(emu, A_SPDIF_SAMPLERATE, 0);
		tmp &= 0xfffff1ff;
		tmp |= (0x2<<9);
		snd_emu10k1_ptr_write(emu, A_SPDIF_SAMPLERATE, 0, tmp);

		/* Setup SRCSel (Enable Spdif,I2S SRCMulti) */
		outl(0x600000, emu->port + 0x20);
		outl(0x14, emu->port + 0x24);

		/* Setup SRCMulti Input Audio Enable */
		outl(0x6E0000, emu->port + 0x20);
		outl(0xFF00FF00, emu->port + 0x24);
	}

	/*
	 *  Clear page with silence & setup all pointers to this page
	 */
	memset(emu->silent_page.area, 0, PAGE_SIZE);
	silent_page = emu->silent_page.addr << 1;
	for (idx = 0; idx < MAXPAGES; idx++)
		((u32 *)emu->ptb_pages.area)[idx] = cpu_to_le32(silent_page | idx);
	snd_emu10k1_ptr_write(emu, PTB, 0, emu->ptb_pages.addr);
	snd_emu10k1_ptr_write(emu, TCB, 0, 0);	/* taken from original driver */
	snd_emu10k1_ptr_write(emu, TCBS, 0, 4);	/* taken from original driver */

	silent_page = (emu->silent_page.addr << 1) | MAP_PTI_MASK;
	for (ch = 0; ch < NUM_G; ch++) {
		snd_emu10k1_ptr_write(emu, MAPA, ch, silent_page);
		snd_emu10k1_ptr_write(emu, MAPB, ch, silent_page);
	}

	/*
	 *  Hokay, setup HCFG
	 *   Mute Disable Audio = 0
	 *   Lock Tank Memory = 1
	 *   Lock Sound Memory = 0
	 *   Auto Mute = 1
	 */
	if (emu->audigy) {
		if (emu->revision == 4) /* audigy2 */
			outl(HCFG_AUDIOENABLE |
			     HCFG_AC3ENABLE_CDSPDIF |
			     HCFG_AC3ENABLE_GPSPDIF |
			     HCFG_AUTOMUTE | HCFG_JOYENABLE, emu->port + HCFG);
		else
			outl(HCFG_AUTOMUTE | HCFG_JOYENABLE, emu->port + HCFG);
	} else if (emu->model == 0x20 ||
	    emu->model == 0xc400 ||
	    (emu->model == 0x21 && emu->revision < 6))
		outl(HCFG_LOCKTANKCACHE_MASK | HCFG_AUTOMUTE, emu->port + HCFG);
	else
		// With on-chip joystick
		outl(HCFG_LOCKTANKCACHE_MASK | HCFG_AUTOMUTE | HCFG_JOYENABLE, emu->port + HCFG);

	if (enable_ir) {	/* enable IR for SB Live */
		if (emu->audigy) {
			unsigned int reg = inl(emu->port + A_IOCFG);
			outl(reg | A_IOCFG_GPOUT2, emu->port + A_IOCFG);
			udelay(500);
			outl(reg | A_IOCFG_GPOUT1 | A_IOCFG_GPOUT2, emu->port + A_IOCFG);
			udelay(100);
			outl(reg, emu->port + A_IOCFG);
		} else {
			unsigned int reg = inl(emu->port + HCFG);
			outl(reg | HCFG_GPOUT2, emu->port + HCFG);
			udelay(500);
			outl(reg | HCFG_GPOUT1 | HCFG_GPOUT2, emu->port + HCFG);
			udelay(100);
			outl(reg, emu->port + HCFG);
 		}
	}
	
	if (emu->audigy) {	/* enable analog output */
		unsigned int reg = inl(emu->port + A_IOCFG);
		outl(reg | A_IOCFG_GPOUT0, emu->port + A_IOCFG);
	}

	/*
	 *  Initialize the effect engine
	 */
	if ((err = snd_emu10k1_init_efx(emu)) < 0)
		return err;

	/*
	 *  Enable the audio bit
	 */
	outl(inl(emu->port + HCFG) | HCFG_AUDIOENABLE, emu->port + HCFG);

	/* Enable analog/digital outs on audigy */
	if (emu->audigy) {
		outl(inl(emu->port + A_IOCFG) & ~0x44, emu->port + A_IOCFG);
 
		if (emu->revision == 4) { /* audigy2 */
			/* Unmute Analog now.  Set GPO6 to 1 for Apollo.
			 * This has to be done after init ALice3 I2SOut beyond 48KHz.
			 * So, sequence is important. */
			outl(inl(emu->port + A_IOCFG) | 0x0040, emu->port + A_IOCFG);
		} else {
			/* Disable routing from AC97 line out to Front speakers */
			outl(inl(emu->port + A_IOCFG) | 0x0080, emu->port + A_IOCFG);
		}
	}
	
#if 0
	{
	unsigned int tmp;
	/* FIXME: the following routine disables LiveDrive-II !! */
	// TOSLink detection
	emu->tos_link = 0;
	tmp = inl(emu->port + HCFG);
	if (tmp & (HCFG_GPINPUT0 | HCFG_GPINPUT1)) {
		outl(tmp|0x800, emu->port + HCFG);
		udelay(50);
		if (tmp != (inl(emu->port + HCFG) & ~0x800)) {
			emu->tos_link = 1;
			outl(tmp, emu->port + HCFG);
		}
	}
	}
#endif

	snd_emu10k1_intr_enable(emu, INTE_PCIERRORENABLE);

	emu->reserved_page = (emu10k1_memblk_t *)snd_emu10k1_synth_alloc(emu, 4096);
	if (emu->reserved_page)
		emu->reserved_page->map_locked = 1;
	
	return 0;
}

static int snd_emu10k1_done(emu10k1_t * emu)
{
	int ch;

	outl(0, emu->port + INTE);

	/*
	 *  Shutdown the chip
	 */
	for (ch = 0; ch < NUM_G; ch++)
		snd_emu10k1_ptr_write(emu, DCYSUSV, ch, 0);
	for (ch = 0; ch < NUM_G; ch++) {
		snd_emu10k1_ptr_write(emu, VTFT, ch, 0);
		snd_emu10k1_ptr_write(emu, CVCF, ch, 0);
		snd_emu10k1_ptr_write(emu, PTRX, ch, 0);
		snd_emu10k1_ptr_write(emu, CPF, ch, 0);
	}

	/* reset recording buffers */
	snd_emu10k1_ptr_write(emu, MICBS, 0, 0);
	snd_emu10k1_ptr_write(emu, MICBA, 0, 0);
	snd_emu10k1_ptr_write(emu, FXBS, 0, 0);
	snd_emu10k1_ptr_write(emu, FXBA, 0, 0);
	snd_emu10k1_ptr_write(emu, FXWC, 0, 0);
	snd_emu10k1_ptr_write(emu, ADCBS, 0, ADCBS_BUFSIZE_NONE);
	snd_emu10k1_ptr_write(emu, ADCBA, 0, 0);
	snd_emu10k1_ptr_write(emu, TCBS, 0, TCBS_BUFFSIZE_16K);
	snd_emu10k1_ptr_write(emu, TCB, 0, 0);
	if (emu->audigy)
		snd_emu10k1_ptr_write(emu, A_DBG, 0, A_DBG_SINGLE_STEP);
	else
		snd_emu10k1_ptr_write(emu, DBG, 0, 0x8000);

	/* disable channel interrupt */
	snd_emu10k1_ptr_write(emu, CLIEL, 0, 0);
	snd_emu10k1_ptr_write(emu, CLIEH, 0, 0);
	snd_emu10k1_ptr_write(emu, SOLEL, 0, 0);
	snd_emu10k1_ptr_write(emu, SOLEH, 0, 0);

	/* remove reserved page */
	if (emu->reserved_page != NULL) {
		snd_emu10k1_synth_free(emu, (snd_util_memblk_t *)emu->reserved_page);
		emu->reserved_page = NULL;
	}

	/* disable audio and lock cache */
	outl(HCFG_LOCKSOUNDCACHE | HCFG_LOCKTANKCACHE_MASK | HCFG_MUTEBUTTONENABLE, emu->port + HCFG);
	snd_emu10k1_ptr_write(emu, PTB, 0, 0);

	snd_emu10k1_free_efx(emu);

	return 0;
}

/*************************************************************************
 * ECARD functional implementation
 *************************************************************************/

/* In A1 Silicon, these bits are in the HC register */
#define HOOKN_BIT		(1L << 12)
#define HANDN_BIT		(1L << 11)
#define PULSEN_BIT		(1L << 10)

#define EC_GDI1			(1 << 13)
#define EC_GDI0			(1 << 14)

#define EC_NUM_CONTROL_BITS	20

#define EC_AC3_DATA_SELN	0x0001L
#define EC_EE_DATA_SEL		0x0002L
#define EC_EE_CNTRL_SELN	0x0004L
#define EC_EECLK		0x0008L
#define EC_EECS			0x0010L
#define EC_EESDO		0x0020L
#define EC_TRIM_CSN		0x0040L
#define EC_TRIM_SCLK		0x0080L
#define EC_TRIM_SDATA		0x0100L
#define EC_TRIM_MUTEN		0x0200L
#define EC_ADCCAL		0x0400L
#define EC_ADCRSTN		0x0800L
#define EC_DACCAL		0x1000L
#define EC_DACMUTEN		0x2000L
#define EC_LEDN			0x4000L

#define EC_SPDIF0_SEL_SHIFT	15
#define EC_SPDIF1_SEL_SHIFT	17
#define EC_SPDIF0_SEL_MASK	(0x3L << EC_SPDIF0_SEL_SHIFT)
#define EC_SPDIF1_SEL_MASK	(0x7L << EC_SPDIF1_SEL_SHIFT)
#define EC_SPDIF0_SELECT(_x)	(((_x) << EC_SPDIF0_SEL_SHIFT) & EC_SPDIF0_SEL_MASK)
#define EC_SPDIF1_SELECT(_x)	(((_x) << EC_SPDIF1_SEL_SHIFT) & EC_SPDIF1_SEL_MASK)
#define EC_CURRENT_PROM_VERSION 0x01	/* Self-explanatory.  This should
					 * be incremented any time the EEPROM's
					 * format is changed.  */

#define EC_EEPROM_SIZE		0x40	/* ECARD EEPROM has 64 16-bit words */

/* Addresses for special values stored in to EEPROM */
#define EC_PROM_VERSION_ADDR	0x20	/* Address of the current prom version */
#define EC_BOARDREV0_ADDR	0x21	/* LSW of board rev */
#define EC_BOARDREV1_ADDR	0x22	/* MSW of board rev */

#define EC_LAST_PROMFILE_ADDR	0x2f

#define EC_SERIALNUM_ADDR	0x30	/* First word of serial number.  The 
					 * can be up to 30 characters in length
					 * and is stored as a NULL-terminated
					 * ASCII string.  Any unused bytes must be
					 * filled with zeros */
#define EC_CHECKSUM_ADDR	0x3f	/* Location at which checksum is stored */


/* Most of this stuff is pretty self-evident.  According to the hardware 
 * dudes, we need to leave the ADCCAL bit low in order to avoid a DC 
 * offset problem.  Weird.
 */
#define EC_RAW_RUN_MODE		(EC_DACMUTEN | EC_ADCRSTN | EC_TRIM_MUTEN | \
				 EC_TRIM_CSN)


#define EC_DEFAULT_ADC_GAIN	0xC4C4
#define EC_DEFAULT_SPDIF0_SEL	0x0
#define EC_DEFAULT_SPDIF1_SEL	0x4

/**************************************************************************
 * @func Clock bits into the Ecard's control latch.  The Ecard uses a
 *  control latch will is loaded bit-serially by toggling the Modem control
 *  lines from function 2 on the E8010.  This function hides these details
 *  and presents the illusion that we are actually writing to a distinct
 *  register.
 */

static void snd_emu10k1_ecard_write(emu10k1_t * emu, unsigned int value)
{
	unsigned short count;
	unsigned int data;
	unsigned long hc_port;
	unsigned int hc_value;

	hc_port = emu->port + HCFG;
	hc_value = inl(hc_port) & ~(HOOKN_BIT | HANDN_BIT | PULSEN_BIT);
	outl(hc_value, hc_port);

	for (count = 0; count < EC_NUM_CONTROL_BITS; count++) {

		/* Set up the value */
		data = ((value & 0x1) ? PULSEN_BIT : 0);
		value >>= 1;

		outl(hc_value | data, hc_port);

		/* Clock the shift register */
		outl(hc_value | data | HANDN_BIT, hc_port);
		outl(hc_value | data, hc_port);
	}

	/* Latch the bits */
	outl(hc_value | HOOKN_BIT, hc_port);
	outl(hc_value, hc_port);
}

/**************************************************************************
 * @func Set the gain of the ECARD's CS3310 Trim/gain controller.  The
 * trim value consists of a 16bit value which is composed of two
 * 8 bit gain/trim values, one for the left channel and one for the
 * right channel.  The following table maps from the Gain/Attenuation
 * value in decibels into the corresponding bit pattern for a single
 * channel.
 */

static void snd_emu10k1_ecard_setadcgain(emu10k1_t * emu,
					 unsigned short gain)
{
	unsigned int bit;

	/* Enable writing to the TRIM registers */
	snd_emu10k1_ecard_write(emu, emu->ecard_ctrl & ~EC_TRIM_CSN);

	/* Do it again to insure that we meet hold time requirements */
	snd_emu10k1_ecard_write(emu, emu->ecard_ctrl & ~EC_TRIM_CSN);

	for (bit = (1 << 15); bit; bit >>= 1) {
		unsigned int value;
		
		value = emu->ecard_ctrl & ~(EC_TRIM_CSN | EC_TRIM_SDATA);

		if (gain & bit)
			value |= EC_TRIM_SDATA;

		/* Clock the bit */
		snd_emu10k1_ecard_write(emu, value);
		snd_emu10k1_ecard_write(emu, value | EC_TRIM_SCLK);
		snd_emu10k1_ecard_write(emu, value);
	}

	snd_emu10k1_ecard_write(emu, emu->ecard_ctrl);
}

static int __devinit snd_emu10k1_ecard_init(emu10k1_t * emu)
{
	unsigned int hc_value;

	/* Set up the initial settings */
	emu->ecard_ctrl = EC_RAW_RUN_MODE |
			  EC_SPDIF0_SELECT(EC_DEFAULT_SPDIF0_SEL) |
			  EC_SPDIF1_SELECT(EC_DEFAULT_SPDIF1_SEL);

	/* Step 0: Set the codec type in the hardware control register 
	 * and enable audio output */
	hc_value = inl(emu->port + HCFG);
	outl(hc_value | HCFG_AUDIOENABLE | HCFG_CODECFORMAT_I2S, emu->port + HCFG);
	inl(emu->port + HCFG);

	/* Step 1: Turn off the led and deassert TRIM_CS */
	snd_emu10k1_ecard_write(emu, EC_ADCCAL | EC_LEDN | EC_TRIM_CSN);

	/* Step 2: Calibrate the ADC and DAC */
	snd_emu10k1_ecard_write(emu, EC_DACCAL | EC_LEDN | EC_TRIM_CSN);

	/* Step 3: Wait for awhile;   XXX We can't get away with this
	 * under a real operating system; we'll need to block and wait that
	 * way. */
	snd_emu10k1_wait(emu, 48000);

	/* Step 4: Switch off the DAC and ADC calibration.  Note
	 * That ADC_CAL is actually an inverted signal, so we assert
	 * it here to stop calibration.  */
	snd_emu10k1_ecard_write(emu, EC_ADCCAL | EC_LEDN | EC_TRIM_CSN);

	/* Step 4: Switch into run mode */
	snd_emu10k1_ecard_write(emu, emu->ecard_ctrl);

	/* Step 5: Set the analog input gain */
	snd_emu10k1_ecard_setadcgain(emu, EC_DEFAULT_ADC_GAIN);

	return 0;
}

/*
 *  Create the EMU10K1 instance
 */

static int snd_emu10k1_free(emu10k1_t *emu)
{
	if (emu->res_port != NULL) {	/* avoid access to already used hardware */
	       	snd_emu10k1_fx8010_tram_setup(emu, 0);
		snd_emu10k1_done(emu);
       	}
	if (emu->memhdr)
		snd_util_memhdr_free(emu->memhdr);
	if (emu->silent_page.area)
		snd_dma_free_pages(&emu->dma_dev, &emu->silent_page);
	if (emu->ptb_pages.area)
		snd_dma_free_pages(&emu->dma_dev, &emu->ptb_pages);
	if (emu->page_ptr_table)
		vfree(emu->page_ptr_table);
	if (emu->page_addr_table)
		vfree(emu->page_addr_table);
	if (emu->res_port) {
		release_resource(emu->res_port);
		kfree_nocheck(emu->res_port);
	}
	if (emu->irq >= 0)
		free_irq(emu->irq, (void *)emu);
	snd_magic_kfree(emu);
	return 0;
}

static int snd_emu10k1_dev_free(snd_device_t *device)
{
	emu10k1_t *emu = snd_magic_cast(emu10k1_t, device->device_data, return -ENXIO);
	return snd_emu10k1_free(emu);
}

int __devinit snd_emu10k1_create(snd_card_t * card,
		       struct pci_dev * pci,
		       unsigned short extin_mask,
		       unsigned short extout_mask,
		       long max_cache_bytes,
		       int enable_ir,
		       emu10k1_t ** remu)
{
	emu10k1_t *emu;
	int err;
	int is_audigy;
	static snd_device_ops_t ops = {
		.dev_free =	snd_emu10k1_dev_free,
	};
	
	*remu = NULL;

	// is_audigy = (int)pci->driver_data;
	is_audigy = (pci->device == 0x0004);

	/* enable PCI device */
	if ((err = pci_enable_device(pci)) < 0)
		return err;

	emu = snd_magic_kcalloc(emu10k1_t, 0, GFP_KERNEL);
	if (emu == NULL)
		return -ENOMEM;
	/* set the DMA transfer mask */
	emu->dma_mask = is_audigy ? AUDIGY_DMA_MASK : EMU10K1_DMA_MASK;
	if (pci_set_dma_mask(pci, emu->dma_mask) < 0 ||
	    pci_set_consistent_dma_mask(pci, emu->dma_mask) < 0) {
		snd_printk(KERN_ERR "architecture does not support PCI busmaster DMA with mask 0x%lx\n", emu->dma_mask);
		snd_magic_kfree(emu);
		return -ENXIO;
	}
	emu->card = card;
	spin_lock_init(&emu->reg_lock);
	spin_lock_init(&emu->emu_lock);
	spin_lock_init(&emu->voice_lock);
	spin_lock_init(&emu->synth_lock);
	spin_lock_init(&emu->memblk_lock);
	init_MUTEX(&emu->ptb_lock);
	init_MUTEX(&emu->fx8010.lock);
	INIT_LIST_HEAD(&emu->mapped_link_head);
	INIT_LIST_HEAD(&emu->mapped_order_link_head);
	emu->pci = pci;
	emu->irq = -1;
	emu->synth = NULL;
	emu->get_synth_voice = NULL;
	emu->port = pci_resource_start(pci, 0);

	emu->audigy = is_audigy;
	if (is_audigy)
		emu->gpr_base = A_FXGPREGBASE;
	else
		emu->gpr_base = FXGPREGBASE;

	if ((emu->res_port = request_region(emu->port, 0x20, "EMU10K1")) == NULL) {
		snd_emu10k1_free(emu);
		return -EBUSY;
	}

	if (request_irq(pci->irq, snd_emu10k1_interrupt, SA_INTERRUPT|SA_SHIRQ, "EMU10K1", (void *)emu)) {
		snd_emu10k1_free(emu);
		return -EBUSY;
	}
	emu->irq = pci->irq;

	memset(&emu->dma_dev, 0, sizeof(emu->dma_dev));
	emu->dma_dev.type = SNDRV_DMA_TYPE_DEV;
	emu->dma_dev.dev = snd_dma_pci_data(pci);

	emu->max_cache_pages = max_cache_bytes >> PAGE_SHIFT;
	if (snd_dma_alloc_pages(&emu->dma_dev, 32 * 1024, &emu->ptb_pages) < 0) {
		snd_emu10k1_free(emu);
		return -ENOMEM;
	}

	emu->page_ptr_table = (void **)vmalloc(emu->max_cache_pages * sizeof(void*));
	emu->page_addr_table = (unsigned long*)vmalloc(emu->max_cache_pages * sizeof(unsigned long));
	if (emu->page_ptr_table == NULL || emu->page_addr_table == NULL) {
		snd_emu10k1_free(emu);
		return -ENOMEM;
	}

	if (snd_dma_alloc_pages(&emu->dma_dev, EMUPAGESIZE, &emu->silent_page) < 0) {
		snd_emu10k1_free(emu);
		return -ENOMEM;
	}
	emu->memhdr = snd_util_memhdr_new(emu->max_cache_pages * PAGE_SIZE);
	if (emu->memhdr == NULL) {
		snd_emu10k1_free(emu);
		return -ENOMEM;
	}
	emu->memhdr->block_extra_size = sizeof(emu10k1_memblk_t) - sizeof(snd_util_memblk_t);

	pci_set_master(pci);
	/* read revision & serial */
	pci_read_config_byte(pci, PCI_REVISION_ID, (char *)&emu->revision);
	pci_read_config_dword(pci, PCI_SUBSYSTEM_VENDOR_ID, &emu->serial);
	pci_read_config_word(pci, PCI_SUBSYSTEM_ID, &emu->model);
	emu->card_type = EMU10K1_CARD_CREATIVE;
	if (emu->serial == 0x40011102) {
		emu->card_type = EMU10K1_CARD_EMUAPS;
		emu->APS = 1;
		emu->no_ac97 = 1; /* APS has no AC97 chip */
	}
	else if (emu->revision == 4 && emu->serial == 0x10051102) {
		/* Audigy 2 EX has apparently no effective AC97 controls
		 * (for both input and output), so we skip the AC97 detections
		 */
		snd_printdd(KERN_INFO "Audigy2 EX is detected. skpping ac97.\n");
		emu->no_ac97 = 1;
	}
	
	emu->fx8010.fxbus_mask = 0x303f;
	if (extin_mask == 0)
		extin_mask = 0x3fcf;
	if (extout_mask == 0)
		extout_mask = 0x7fff;
	emu->fx8010.extin_mask = extin_mask;
	emu->fx8010.extout_mask = extout_mask;

	if (emu->APS) {
		if ((err = snd_emu10k1_ecard_init(emu)) < 0) {
			snd_emu10k1_free(emu);
			return err;
		}
	} else {
		/* 5.1: Enable the additional AC97 Slots. If the emu10k1 version
			does not support this, it shouldn't do any harm */
		snd_emu10k1_ptr_write(emu, AC97SLOT, 0, AC97SLOT_CNTR|AC97SLOT_LFE);
	}

	if ((err = snd_emu10k1_init(emu, enable_ir)) < 0) {
		snd_emu10k1_free(emu);
		return err;
	}

	if ((err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, emu, &ops)) < 0) {
		snd_emu10k1_free(emu);
		return err;
	}

	snd_emu10k1_proc_init(emu);

	snd_card_set_dev(card, &pci->dev);
	*remu = emu;
	return 0;
}

/* memory.c */
EXPORT_SYMBOL(snd_emu10k1_synth_alloc);
EXPORT_SYMBOL(snd_emu10k1_synth_free);
EXPORT_SYMBOL(snd_emu10k1_synth_bzero);
EXPORT_SYMBOL(snd_emu10k1_synth_copy_from_user);
EXPORT_SYMBOL(snd_emu10k1_memblk_map);
/* voice.c */
EXPORT_SYMBOL(snd_emu10k1_voice_alloc);
EXPORT_SYMBOL(snd_emu10k1_voice_free);
/* io.c */
EXPORT_SYMBOL(snd_emu10k1_ptr_read);
EXPORT_SYMBOL(snd_emu10k1_ptr_write);
