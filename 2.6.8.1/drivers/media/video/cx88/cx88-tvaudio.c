/*
    cx88x-audio.c - Conexant CX23880/23881 audio downstream driver driver

     (c) 2001 Michael Eskin, Tom Zakrajsek [Windows version]
     (c) 2002 Yurij Sysoev <yurij@naturesoft.net>
     (c) 2003 Gerd Knorr <kraxel@bytesex.org>

    -----------------------------------------------------------------------

    Lot of voodoo here.  Even the data sheet doesn't help to
    understand what is going on here, the documentation for the audio
    part of the cx2388x chip is *very* bad.

    Some of this comes from party done linux driver sources I got from
    [undocumented].

    Some comes from the dscaler sources, one of the dscaler driver guy works
    for Conexant ...
    
    -----------------------------------------------------------------------
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/poll.h>
#include <linux/pci.h>
#include <linux/signal.h>
#include <linux/ioport.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/vmalloc.h>
#include <linux/init.h>
#include <linux/smp_lock.h>

#include "cx88.h"

static unsigned int audio_debug = 1;
MODULE_PARM(audio_debug,"i");
MODULE_PARM_DESC(audio_debug,"enable debug messages [audio]");

#define dprintk(fmt, arg...)	if (audio_debug) \
	printk(KERN_DEBUG "%s: " fmt, dev->name , ## arg)

/* ----------------------------------------------------------- */

static char *aud_ctl_names[64] =
{
	[ EN_BTSC_FORCE_MONO       ] = "BTSC_FORCE_MONO",
	[ EN_BTSC_FORCE_STEREO     ] = "BTSC_FORCE_STEREO",
	[ EN_BTSC_FORCE_SAP        ] = "BTSC_FORCE_SAP",
	[ EN_BTSC_AUTO_STEREO      ] = "BTSC_AUTO_STEREO",
	[ EN_BTSC_AUTO_SAP         ] = "BTSC_AUTO_SAP",
	[ EN_A2_FORCE_MONO1        ] = "A2_FORCE_MONO1",
	[ EN_A2_FORCE_MONO2        ] = "A2_FORCE_MONO2",
	[ EN_A2_FORCE_STEREO       ] = "A2_FORCE_STEREO",
	[ EN_A2_AUTO_MONO2         ] = "A2_AUTO_MONO2",
	[ EN_A2_AUTO_STEREO        ] = "A2_AUTO_STEREO",
	[ EN_EIAJ_FORCE_MONO1      ] = "EIAJ_FORCE_MONO1",
	[ EN_EIAJ_FORCE_MONO2      ] = "EIAJ_FORCE_MONO2",
	[ EN_EIAJ_FORCE_STEREO     ] = "EIAJ_FORCE_STEREO",
	[ EN_EIAJ_AUTO_MONO2       ] = "EIAJ_AUTO_MONO2",
	[ EN_EIAJ_AUTO_STEREO      ] = "EIAJ_AUTO_STEREO",
	[ EN_NICAM_FORCE_MONO1     ] = "NICAM_FORCE_MONO1",
	[ EN_NICAM_FORCE_MONO2     ] = "NICAM_FORCE_MONO2",
	[ EN_NICAM_FORCE_STEREO    ] = "NICAM_FORCE_STEREO",
	[ EN_NICAM_AUTO_MONO2      ] = "NICAM_AUTO_MONO2",
	[ EN_NICAM_AUTO_STEREO     ] = "NICAM_AUTO_STEREO",
	[ EN_FMRADIO_FORCE_MONO    ] = "FMRADIO_FORCE_MONO",
	[ EN_FMRADIO_FORCE_STEREO  ] = "FMRADIO_FORCE_STEREO",
	[ EN_FMRADIO_AUTO_STEREO   ] = "FMRADIO_AUTO_STEREO",
};

struct rlist {
	u32 reg;
	u32 val;
};

static void set_audio_registers(struct cx8800_dev *dev,
				const struct rlist *l)
{
	int i;

	for (i = 0; l[i].reg; i++) {
		switch (l[i].reg) {
		case AUD_PDF_DDS_CNST_BYTE2:
		case AUD_PDF_DDS_CNST_BYTE1:
		case AUD_PDF_DDS_CNST_BYTE0:
		case AUD_QAM_MODE:
		case AUD_PHACC_FREQ_8MSB:
		case AUD_PHACC_FREQ_8LSB:
			cx_writeb(l[i].reg, l[i].val);
			break;
		default:
			cx_write(l[i].reg, l[i].val);
			break;
		}
	}
}

static void set_audio_start(struct cx8800_dev *dev,
			    u32 mode, u32 ctl)
{
	// mute
	cx_write(AUD_VOL_CTL,       (1 << 6));

	//  increase level of input by 12dB
	cx_write(AUD_AFE_12DB_EN,   0x0001);

	// start programming
	cx_write(AUD_CTL,           0x0000);
	cx_write(AUD_INIT,          mode);
	cx_write(AUD_INIT_LD,       0x0001);
	cx_write(AUD_SOFT_RESET,    0x0001);

	cx_write(AUD_CTL,           ctl);
}

static void set_audio_finish(struct cx8800_dev *dev)
{
	u32 volume;

	// finish programming
	cx_write(AUD_SOFT_RESET, 0x0000);

	// start audio processing
	cx_set(AUD_CTL, EN_DAC_ENABLE);

	// unmute
	volume = cx_sread(SHADOW_AUD_VOL_CTL);
	cx_swrite(SHADOW_AUD_VOL_CTL, AUD_VOL_CTL, volume);
}

/* ----------------------------------------------------------- */

static void set_audio_standard_BTSC(struct cx8800_dev *dev, unsigned int sap)
{
	static const struct rlist btsc[] = {
		/* from dscaler */
		{ AUD_OUT1_SEL,                0x00000013 },
		{ AUD_OUT1_SHIFT,              0x00000000 },
		{ AUD_POLY0_DDS_CONSTANT,      0x0012010c },
		{ AUD_DMD_RA_DDS,              0x00c3e7aa },
		{ AUD_DBX_IN_GAIN,             0x00004734 },
		{ AUD_DBX_WBE_GAIN,            0x00004640 },
		{ AUD_DBX_SE_GAIN,             0x00008d31 },
		{ AUD_DCOC_0_SRC,              0x0000001a },
		{ AUD_IIR1_4_SEL,              0x00000021 },
		{ AUD_DCOC_PASS_IN,            0x00000003 },
		{ AUD_DCOC_0_SHIFT_IN0,        0x0000000a },
		{ AUD_DCOC_0_SHIFT_IN1,        0x00000008 },
		{ AUD_DCOC_1_SHIFT_IN0,        0x0000000a },
		{ AUD_DCOC_1_SHIFT_IN1,        0x00000008 },
		{ AUD_DN0_FREQ,                0x0000283b },
		{ AUD_DN2_SRC_SEL,             0x00000008 },
		{ AUD_DN2_FREQ,                0x00003000 },
		{ AUD_DN2_AFC,                 0x00000002 },
		{ AUD_DN2_SHFT,                0x00000000 },
		{ AUD_IIR2_2_SEL,              0x00000020 },
		{ AUD_IIR2_2_SHIFT,            0x00000000 },
		{ AUD_IIR2_3_SEL,              0x0000001f },
		{ AUD_IIR2_3_SHIFT,            0x00000000 },
		{ AUD_CRDC1_SRC_SEL,           0x000003ce },
		{ AUD_CRDC1_SHIFT,             0x00000000 },
		{ AUD_CORDIC_SHIFT_1,          0x00000007 },
		{ AUD_DCOC_1_SRC,              0x0000001b },
		{ AUD_DCOC1_SHIFT,             0x00000000 },
		{ AUD_RDSI_SEL,                0x00000008 },
		{ AUD_RDSQ_SEL,                0x00000008 },
		{ AUD_RDSI_SHIFT,              0x00000000 },
		{ AUD_RDSQ_SHIFT,              0x00000000 },
		{ AUD_POLYPH80SCALEFAC,        0x00000003 },
                { /* end of list */ },
	};
	static const struct rlist btsc_sap[] = {
		{ AUD_DBX_IN_GAIN,             0x00007200 },
		{ AUD_DBX_WBE_GAIN,            0x00006200 },
		{ AUD_DBX_SE_GAIN,             0x00006200 },
		{ AUD_IIR1_1_SEL,              0x00000000 },
		{ AUD_IIR1_3_SEL,              0x00000001 },
		{ AUD_DN1_SRC_SEL,             0x00000007 },
		{ AUD_IIR1_4_SHIFT,            0x00000006 },
		{ AUD_IIR2_1_SHIFT,            0x00000000 },
		{ AUD_IIR2_2_SHIFT,            0x00000000 },
		{ AUD_IIR3_0_SHIFT,            0x00000000 },
		{ AUD_IIR3_1_SHIFT,            0x00000000 },
		{ AUD_IIR3_0_SEL,              0x0000000d },
		{ AUD_IIR3_1_SEL,              0x0000000e },
		{ AUD_DEEMPH1_SRC_SEL,         0x00000014 },
		{ AUD_DEEMPH1_SHIFT,           0x00000000 },
		{ AUD_DEEMPH1_G0,              0x00004000 },
		{ AUD_DEEMPH1_A0,              0x00000000 },
		{ AUD_DEEMPH1_B0,              0x00000000 },
		{ AUD_DEEMPH1_A1,              0x00000000 },
		{ AUD_DEEMPH1_B1,              0x00000000 },
		{ AUD_OUT0_SEL,                0x0000003f },
		{ AUD_OUT1_SEL,                0x0000003f },
		{ AUD_DN1_AFC,                 0x00000002 },
		{ AUD_DCOC_0_SHIFT_IN0,        0x0000000a },
		{ AUD_DCOC_0_SHIFT_IN1,        0x00000008 },
		{ AUD_DCOC_1_SHIFT_IN0,        0x0000000a },
		{ AUD_DCOC_1_SHIFT_IN1,        0x00000008 },
		{ AUD_IIR1_0_SEL,              0x0000001d },
		{ AUD_IIR1_2_SEL,              0x0000001e },
		{ AUD_IIR2_1_SEL,              0x00000002 },
		{ AUD_IIR2_2_SEL,              0x00000004 },
		{ AUD_IIR3_2_SEL,              0x0000000f },
		{ AUD_DCOC2_SHIFT,             0x00000001 },
		{ AUD_IIR3_2_SHIFT,            0x00000001 },
		{ AUD_DEEMPH0_SRC_SEL,         0x00000014 },
		{ AUD_CORDIC_SHIFT_1,          0x00000006 },
		{ AUD_POLY0_DDS_CONSTANT,      0x000e4db2 },
		{ AUD_DMD_RA_DDS,              0x00f696e6 },
		{ AUD_IIR2_3_SEL,              0x00000025 },
		{ AUD_IIR1_4_SEL,              0x00000021 },
		{ AUD_DN1_FREQ,                0x0000c965 },
		{ AUD_DCOC_PASS_IN,            0x00000003 },
		{ AUD_DCOC_0_SRC,              0x0000001a },
		{ AUD_DCOC_1_SRC,              0x0000001b },
		{ AUD_DCOC1_SHIFT,             0x00000000 },
		{ AUD_RDSI_SEL,                0x00000009 },
		{ AUD_RDSQ_SEL,                0x00000009 },
		{ AUD_RDSI_SHIFT,              0x00000000 },
		{ AUD_RDSQ_SHIFT,              0x00000000 },
		{ AUD_POLYPH80SCALEFAC,        0x00000003 },
                { /* end of list */ },
	};

	// dscaler: exactly taken from driver,
	// dscaler: don't know why to set EN_FMRADIO_EN_RDS
	if (sap) {
		dprintk("%s SAP (status: unknown)\n",__FUNCTION__);
		set_audio_start(dev, 0x0001,
				EN_FMRADIO_EN_RDS | EN_BTSC_FORCE_SAP);
		set_audio_registers(dev, btsc_sap);
	} else {
		dprintk("%s (status: known-good)\n",__FUNCTION__);
		set_audio_start(dev, 0x0001,
				EN_FMRADIO_EN_RDS | EN_BTSC_AUTO_STEREO);
		set_audio_registers(dev, btsc);
	}
	set_audio_finish(dev);
}

static void set_audio_standard_NICAM(struct cx8800_dev *dev)
{
	static const struct rlist nicam_common[] = {
		/* from dscaler */
    		{ AUD_RATE_ADJ1,           0x00000010 },
    		{ AUD_RATE_ADJ2,           0x00000040 },
    		{ AUD_RATE_ADJ3,           0x00000100 },
    		{ AUD_RATE_ADJ4,           0x00000400 },
    		{ AUD_RATE_ADJ5,           0x00001000 },
    //		{ AUD_DMD_RA_DDS,          0x00c0d5ce },

		// Deemphasis 1:
		{ AUD_DEEMPHGAIN_R,        0x000023c2 },
		{ AUD_DEEMPHNUMER1_R,      0x0002a7bc },
		{ AUD_DEEMPHNUMER2_R,      0x0003023e },
		{ AUD_DEEMPHDENOM1_R,      0x0000f3d0 },
		{ AUD_DEEMPHDENOM2_R,      0x00000000 },

#if 0
		// Deemphasis 2: (other tv norm?)
		{ AUD_DEEMPHGAIN_R,        0x0000c600 },
		{ AUD_DEEMPHNUMER1_R,      0x00066738 },
		{ AUD_DEEMPHNUMER2_R,      0x00066739 },
		{ AUD_DEEMPHDENOM1_R,      0x0001e88c },
		{ AUD_DEEMPHDENOM2_R,      0x0001e88c },
#endif

		{ AUD_DEEMPHDENOM2_R,      0x00000000 },
		{ AUD_ERRLOGPERIOD_R,      0x00000fff },
		{ AUD_ERRINTRPTTHSHLD1_R,  0x000003ff },
		{ AUD_ERRINTRPTTHSHLD2_R,  0x000000ff },
		{ AUD_ERRINTRPTTHSHLD3_R,  0x0000003f },
		{ AUD_POLYPH80SCALEFAC,    0x00000003 },

		// setup QAM registers
		{ AUD_PDF_DDS_CNST_BYTE2,  0x06 },
		{ AUD_PDF_DDS_CNST_BYTE1,  0x82 },
		{ AUD_PDF_DDS_CNST_BYTE0,  0x16 },
		{ AUD_QAM_MODE,            0x05 },

                { /* end of list */ },
        };
	static const struct rlist nicam_pal_i[] = {
		{ AUD_PDF_DDS_CNST_BYTE0,  0x12 },
		{ AUD_PHACC_FREQ_8MSB,     0x3a },
		{ AUD_PHACC_FREQ_8LSB,     0x93 },

                { /* end of list */ },
	};
	static const struct rlist nicam_default[] = {
		{ AUD_PDF_DDS_CNST_BYTE0,  0x16 },
		{ AUD_PHACC_FREQ_8MSB,     0x34 },
		{ AUD_PHACC_FREQ_8LSB,     0x4c },

                { /* end of list */ },
	};

        set_audio_start(dev, 0x0010,
			EN_DMTRX_LR | EN_DMTRX_BYPASS | EN_NICAM_AUTO_STEREO);
        set_audio_registers(dev, nicam_common);
	switch (dev->tvaudio) {
	case WW_NICAM_I:
		dprintk("%s PAL-I NICAM (status: unknown)\n",__FUNCTION__);
		set_audio_registers(dev, nicam_pal_i);
	case WW_NICAM_BGDKL:
		dprintk("%s PAL NICAM (status: unknown)\n",__FUNCTION__);
		set_audio_registers(dev, nicam_default);
		break;
	};
        set_audio_finish(dev);
}

static void set_audio_standard_NICAM_L(struct cx8800_dev *dev)
{
	/* This is officially wierd.. register dumps indicate windows
	 * uses audio mode 4.. A2. Let's operate and find out. */

	static const struct rlist nicam_l[] = {
		// setup QAM registers
		{ AUD_PDF_DDS_CNST_BYTE2,	   0x48 },
		{ AUD_PDF_DDS_CNST_BYTE1,          0x3d },
		{ AUD_PDF_DDS_CNST_BYTE0,          0xf5 },
		{ AUD_QAM_MODE,                    0x00 },
		{ AUD_PHACC_FREQ_8MSB,             0x3a },
		{ AUD_PHACC_FREQ_8LSB,             0x4a },

		{ AUD_POLY0_DDS_CONSTANT,          0x000e4db2 },
		{ AUD_IIR1_0_SEL,                  0x00000000 },
		{ AUD_IIR1_1_SEL,                  0x00000002 },
		{ AUD_IIR1_2_SEL,                  0x00000023 },
		{ AUD_IIR1_3_SEL,                  0x00000004 },
		{ AUD_IIR1_4_SEL,                  0x00000005 },
		{ AUD_IIR1_5_SEL,                  0x00000007 },
		{ AUD_IIR1_0_SHIFT,                0x00000007 },
		{ AUD_IIR1_1_SHIFT,                0x00000000 },
		{ AUD_IIR1_2_SHIFT,                0x00000000 },
		{ AUD_IIR1_3_SHIFT,                0x00000007 },
		{ AUD_IIR1_4_SHIFT,                0x00000007 },
		{ AUD_IIR1_5_SHIFT,                0x00000007 },
		{ AUD_IIR2_0_SEL,                  0x00000002 },
		{ AUD_IIR2_1_SEL,                  0x00000003 },
		{ AUD_IIR2_2_SEL,                  0x00000004 },
		{ AUD_IIR2_3_SEL,                  0x00000005 },
		{ AUD_IIR3_0_SEL,                  0x00000007 },
		{ AUD_IIR3_1_SEL,                  0x00000023 },
		{ AUD_IIR3_2_SEL,                  0x00000016 },
		{ AUD_IIR4_0_SHIFT,                0x00000000 },
		{ AUD_IIR4_1_SHIFT,                0x00000000 },
		{ AUD_IIR3_2_SHIFT,                0x00000002 },
		{ AUD_IIR4_0_SEL,                  0x0000001d },
		{ AUD_IIR4_1_SEL,                  0x00000019 },
		{ AUD_IIR4_2_SEL,                  0x00000008 },
		{ AUD_IIR4_0_SHIFT,                0x00000000 },
		{ AUD_IIR4_1_SHIFT,                0x00000007 },
		{ AUD_IIR4_2_SHIFT,                0x00000007 },
		{ AUD_IIR4_0_CA0,                  0x0003e57e },
		{ AUD_IIR4_0_CA1,                  0x00005e11 },
		{ AUD_IIR4_0_CA2,                  0x0003a7cf },
		{ AUD_IIR4_0_CB0,                  0x00002368 },
		{ AUD_IIR4_0_CB1,                  0x0003bf1b },
		{ AUD_IIR4_1_CA0,                  0x00006349 },
		{ AUD_IIR4_1_CA1,                  0x00006f27 },
		{ AUD_IIR4_1_CA2,                  0x0000e7a3 },
		{ AUD_IIR4_1_CB0,                  0x00005653 },
		{ AUD_IIR4_1_CB1,                  0x0000cf97 },
		{ AUD_IIR4_2_CA0,                  0x00006349 },
		{ AUD_IIR4_2_CA1,                  0x00006f27 },
		{ AUD_IIR4_2_CA2,                  0x0000e7a3 },
		{ AUD_IIR4_2_CB0,                  0x00005653 },
		{ AUD_IIR4_2_CB1,                  0x0000cf97 },
		{ AUD_HP_MD_IIR4_1,                0x00000001 },
		{ AUD_HP_PROG_IIR4_1,              0x0000001a },
		{ AUD_DN0_FREQ,                    0x00000000 },
		{ AUD_DN1_FREQ,                    0x00003318 },
		{ AUD_DN1_SRC_SEL,                 0x00000017 },
		{ AUD_DN1_SHFT,                    0x00000007 },
		{ AUD_DN1_AFC,                     0x00000000 },
		{ AUD_DN1_FREQ_SHIFT,              0x00000000 },
		{ AUD_DN2_FREQ,                    0x00003551 },
		{ AUD_DN2_SRC_SEL,                 0x00000001 },
		{ AUD_DN2_SHFT,                    0x00000000 },
		{ AUD_DN2_AFC,                     0x00000002 },
		{ AUD_DN2_FREQ_SHIFT,              0x00000000 },
		{ AUD_PDET_SRC,                    0x00000014 },
		{ AUD_PDET_SHIFT,                  0x00000000 },
		{ AUD_DEEMPH0_SRC_SEL,             0x00000011 },
		{ AUD_DEEMPH1_SRC_SEL,             0x00000011 },
		{ AUD_DEEMPH0_SHIFT,               0x00000000 },
		{ AUD_DEEMPH1_SHIFT,               0x00000000 },
		{ AUD_DEEMPH0_G0,                  0x00007000 },
		{ AUD_DEEMPH0_A0,                  0x00000000 },
		{ AUD_DEEMPH0_B0,                  0x00000000 },
		{ AUD_DEEMPH0_A1,                  0x00000000 },
		{ AUD_DEEMPH0_B1,                  0x00000000 },
		{ AUD_DEEMPH1_G0,                  0x00007000 },
		{ AUD_DEEMPH1_A0,                  0x00000000 },
		{ AUD_DEEMPH1_B0,                  0x00000000 },
		{ AUD_DEEMPH1_A1,                  0x00000000 },
		{ AUD_DEEMPH1_B1,                  0x00000000 },
		{ AUD_DMD_RA_DDS,                  0x00f5c285 },
		{ AUD_RATE_ADJ1,                   0x00000100 },
		{ AUD_RATE_ADJ2,                   0x00000200 },
		{ AUD_RATE_ADJ3,                   0x00000300 },
		{ AUD_RATE_ADJ4,                   0x00000400 },
		{ AUD_RATE_ADJ5,                   0x00000500 },
		{ AUD_C2_UP_THR,                   0x00005400 },
		{ AUD_C2_LO_THR,                   0x00003000 },
		{ AUD_C1_UP_THR,                   0x00007000 },
		{ AUD_C2_LO_THR,                   0x00005400 },
		{ AUD_CTL,                         0x0000100c },
		{ AUD_DCOC_0_SRC,                  0x00000021 },
		{ AUD_DCOC_1_SRC,                  0x00000003 },
		{ AUD_DCOC1_SHIFT,                 0x00000000 },
		{ AUD_DCOC_1_SHIFT_IN0,            0x0000000a },
		{ AUD_DCOC_1_SHIFT_IN1,            0x00000008 },
		{ AUD_DCOC_PASS_IN,                0x00000000 },
		{ AUD_DCOC_2_SRC,                  0x0000001b },
		{ AUD_IIR4_0_SEL,                  0x0000001d },
		{ AUD_POLY0_DDS_CONSTANT,          0x000e4db2 },
		{ AUD_PHASE_FIX_CTL,               0x00000000 },
		{ AUD_CORDIC_SHIFT_1,              0x00000007 },
		{ AUD_PLL_EN,                      0x00000000 },
		{ AUD_PLL_PRESCALE,                0x00000002 },
		{ AUD_PLL_INT,                     0x0000001e },
		{ AUD_OUT1_SHIFT,                  0x00000000 },

		{ /* end of list */ },
	};

	dprintk("%s (status: unknown)\n",__FUNCTION__);
        set_audio_start(dev, 0x0004,
			0 /* FIXME */);
	set_audio_registers(dev, nicam_l);
        set_audio_finish(dev);
}

static void set_audio_standard_A2(struct cx8800_dev *dev)
{
	/* from dscaler cvs */
	static const struct rlist a2_common[] = {
		{ AUD_PDF_DDS_CNST_BYTE2,     0x06 },
		{ AUD_PDF_DDS_CNST_BYTE1,     0x82 },
		{ AUD_PDF_DDS_CNST_BYTE0,     0x12 },
		{ AUD_QAM_MODE,		      0x05 },
		{ AUD_PHACC_FREQ_8MSB,	      0x34 },
		{ AUD_PHACC_FREQ_8LSB,	      0x4c },

		{ AUD_RATE_ADJ1,	0x00001000 },
		{ AUD_RATE_ADJ2,	0x00002000 },
		{ AUD_RATE_ADJ3,	0x00003000 },
		{ AUD_RATE_ADJ4,	0x00004000 },
		{ AUD_RATE_ADJ5,	0x00005000 },
		{ AUD_THR_FR,		0x00000000 },
		{ AAGC_HYST,		0x0000001a },
		{ AUD_PILOT_BQD_1_K0,	0x0000755b },
		{ AUD_PILOT_BQD_1_K1,	0x00551340 },
		{ AUD_PILOT_BQD_1_K2,	0x006d30be },
		{ AUD_PILOT_BQD_1_K3,	0xffd394af },
		{ AUD_PILOT_BQD_1_K4,	0x00400000 },
		{ AUD_PILOT_BQD_2_K0,	0x00040000 },
		{ AUD_PILOT_BQD_2_K1,	0x002a4841 },
		{ AUD_PILOT_BQD_2_K2,	0x00400000 },
		{ AUD_PILOT_BQD_2_K3,	0x00000000 },
		{ AUD_PILOT_BQD_2_K4,	0x00000000 },
		{ AUD_MODE_CHG_TIMER,	0x00000040 },
		{ AUD_START_TIMER,	0x00000200 },
		{ AUD_AFE_12DB_EN,	0x00000000 },
		{ AUD_CORDIC_SHIFT_0,	0x00000007 },
		{ AUD_CORDIC_SHIFT_1,	0x00000007 },
		{ AUD_DEEMPH0_G0,	0x00000380 },
		{ AUD_DEEMPH1_G0,	0x00000380 },
		{ AUD_DCOC_0_SRC,	0x0000001a },
		{ AUD_DCOC0_SHIFT,	0x00000000 },
		{ AUD_DCOC_0_SHIFT_IN0,	0x0000000a },
		{ AUD_DCOC_0_SHIFT_IN1,	0x00000008 },
		{ AUD_DCOC_PASS_IN,	0x00000003 },
		{ AUD_IIR3_0_SEL,	0x00000021 },
		{ AUD_DN2_AFC,		0x00000002 },
		{ AUD_DCOC_1_SRC,	0x0000001b },
		{ AUD_DCOC1_SHIFT,	0x00000000 },
		{ AUD_DCOC_1_SHIFT_IN0,	0x0000000a },
		{ AUD_DCOC_1_SHIFT_IN1,	0x00000008 },
		{ AUD_IIR3_1_SEL,	0x00000023 },
		{ AUD_RDSI_SEL,		0x00000017 },
		{ AUD_RDSI_SHIFT,	0x00000000 },
		{ AUD_RDSQ_SEL,		0x00000017 },
		{ AUD_RDSQ_SHIFT,	0x00000000 },
		{ AUD_POLYPH80SCALEFAC,	0x00000001 },

		{ /* end of list */ },
	};

	static const struct rlist a2_table1[] = {
		// PAL-BG
		{ AUD_DMD_RA_DDS,	0x002a73bd },
		{ AUD_C1_UP_THR,	0x00007000 },
		{ AUD_C1_LO_THR,	0x00005400 },
		{ AUD_C2_UP_THR,	0x00005400 },
		{ AUD_C2_LO_THR,	0x00003000 },
		{ /* end of list */ },
	};
	static const struct rlist a2_table2[] = {
		// PAL-DK
		{ AUD_DMD_RA_DDS,	0x002a73bd },
		{ AUD_C1_UP_THR,	0x00007000 },
		{ AUD_C1_LO_THR,	0x00005400 },
		{ AUD_C2_UP_THR,	0x00005400 },
		{ AUD_C2_LO_THR,	0x00003000 },
		{ AUD_DN0_FREQ,		0x00003a1c },
		{ AUD_DN2_FREQ,		0x0000d2e0 },
		{ /* end of list */ },
	};
	static const struct rlist a2_table3[] = {
		// unknown, probably NTSC-M
		{ AUD_DMD_RA_DDS,	0x002a2873 },
		{ AUD_C1_UP_THR,	0x00003c00 },
		{ AUD_C1_LO_THR,	0x00003000 },
		{ AUD_C2_UP_THR,	0x00006000 },
		{ AUD_C2_LO_THR,	0x00003c00 },
		{ AUD_DN0_FREQ,		0x00002836 },
		{ AUD_DN1_FREQ,		0x00003418 },
		{ AUD_DN2_FREQ,		0x000029c7 },
		{ AUD_POLY0_DDS_CONSTANT, 0x000a7540 },
		{ /* end of list */ },
	};

	set_audio_start(dev, 0x0004, EN_DMTRX_SUMDIFF | EN_A2_AUTO_STEREO);
	set_audio_registers(dev, a2_common);
	switch (dev->tvaudio) {
	case WW_A2_BG:
		dprintk("%s PAL-BG A2 (status: known-good)\n",__FUNCTION__);
		set_audio_registers(dev, a2_table1);
		break;
	case WW_A2_DK:
		dprintk("%s PAL-DK A2 (status: known-good)\n",__FUNCTION__);
		set_audio_registers(dev, a2_table2);
		break;
	case WW_A2_M:
		dprintk("%s NTSC-M A2 (status: unknown)\n",__FUNCTION__);
		set_audio_registers(dev, a2_table3);
		break;
	};
	set_audio_finish(dev);
}

static void set_audio_standard_EIAJ(struct cx8800_dev *dev)
{
	static const struct rlist eiaj[] = {
		/* TODO: eiaj register settings are not there yet ... */

		{ /* end of list */ },
	};
	dprintk("%s (status: unknown)\n",__FUNCTION__);

	set_audio_start(dev, 0x0002, EN_EIAJ_AUTO_STEREO);
	set_audio_registers(dev, eiaj);
	set_audio_finish(dev);
}

static void set_audio_standard_FM(struct cx8800_dev *dev)
{
#if 0 /* FIXME */
	switch (dev->audio_properties.FM_deemphasis)
	{
		case WW_FM_DEEMPH_50:
			//Set De-emphasis filter coefficients for 50 usec
			cx_write(AUD_DEEMPH0_G0, 0x0C45);
			cx_write(AUD_DEEMPH0_A0, 0x6262);
			cx_write(AUD_DEEMPH0_B0, 0x1C29);
			cx_write(AUD_DEEMPH0_A1, 0x3FC66);
			cx_write(AUD_DEEMPH0_B1, 0x399A);

			cx_write(AUD_DEEMPH1_G0, 0x0D80);
			cx_write(AUD_DEEMPH1_A0, 0x6262);
			cx_write(AUD_DEEMPH1_B0, 0x1C29);
			cx_write(AUD_DEEMPH1_A1, 0x3FC66);
			cx_write(AUD_DEEMPH1_B1, 0x399A);
			
			break;

		case WW_FM_DEEMPH_75:
			//Set De-emphasis filter coefficients for 75 usec
			cx_write(AUD_DEEMPH0_G0, 0x91B );
			cx_write(AUD_DEEMPH0_A0, 0x6B68);
			cx_write(AUD_DEEMPH0_B0, 0x11EC);
			cx_write(AUD_DEEMPH0_A1, 0x3FC66);
			cx_write(AUD_DEEMPH0_B1, 0x399A);

			cx_write(AUD_DEEMPH1_G0, 0xAA0 );
			cx_write(AUD_DEEMPH1_A0, 0x6B68);
			cx_write(AUD_DEEMPH1_B0, 0x11EC);
			cx_write(AUD_DEEMPH1_A1, 0x3FC66);
			cx_write(AUD_DEEMPH1_B1, 0x399A);

			break;
	}
#endif

	dprintk("%s (status: unknown)\n",__FUNCTION__);
	set_audio_start(dev, 0x0020, EN_FMRADIO_AUTO_STEREO);

	// AB: 10/2/01: this register is not being reset appropriately on occasion.
	cx_write(AUD_POLYPH80SCALEFAC,3);

	set_audio_finish(dev);
}

/* ----------------------------------------------------------- */

void cx88_set_tvaudio(struct cx8800_dev *dev)
{
	switch (dev->tvaudio) {
	case WW_BTSC:
		set_audio_standard_BTSC(dev,0);
		break;
	case WW_NICAM_I:
	case WW_NICAM_BGDKL:
		set_audio_standard_NICAM(dev);
		break;
	case WW_A2_BG:
	case WW_A2_DK:
	case WW_A2_M:
		set_audio_standard_A2(dev);
		break;
	case WW_EIAJ:
		set_audio_standard_EIAJ(dev);
		break;
	case WW_FM:
		set_audio_standard_FM(dev);
		break;
	case WW_SYSTEM_L_AM:
		set_audio_standard_NICAM_L(dev);
		break;
	case WW_NONE:
	default:
		printk("%s: unknown tv audio mode [%d]\n",
		       dev->name, dev->tvaudio);
		break;
	}
	return;
}

void cx88_get_stereo(struct cx8800_dev *dev, struct v4l2_tuner *t)
{
	static char *m[] = {"stereo", "dual mono", "mono", "sap"};
	static char *p[] = {"no pilot", "pilot c1", "pilot c2", "?"};
	u32 reg,mode,pilot;

	reg   = cx_read(AUD_STATUS);
	mode  = reg & 0x03;
	pilot = (reg >> 2) & 0x03;
	dprintk("AUD_STATUS: 0x%x [%s/%s] ctl=%s\n",
		reg, m[mode], p[pilot],
		aud_ctl_names[cx_read(AUD_CTL) & 63]);

	t->capability = V4L2_TUNER_CAP_STEREO | V4L2_TUNER_CAP_SAP |
		V4L2_TUNER_CAP_LANG1 | V4L2_TUNER_CAP_LANG2;
	t->rxsubchans = V4L2_TUNER_SUB_MONO;
	t->audmode    = V4L2_TUNER_MODE_MONO;

	switch (dev->tvaudio) {
	case WW_A2_BG:
	case WW_A2_DK:
	case WW_A2_M:
 		if (1 == pilot) {
			/* stereo */
			t->rxsubchans = V4L2_TUNER_SUB_MONO | V4L2_TUNER_SUB_STEREO;
			if (0 == mode)
				t->audmode = V4L2_TUNER_MODE_STEREO;
		}
 		if (2 == pilot) {
			/* dual language -- FIXME */
			t->rxsubchans = V4L2_TUNER_SUB_LANG1 | V4L2_TUNER_SUB_LANG2;
			t->audmode = V4L2_TUNER_MODE_LANG1;
		}
		break;
	case WW_NICAM_BGDKL:
		if (0 == mode)
			t->audmode = V4L2_TUNER_MODE_STEREO;
		break;
	default:
		t->rxsubchans = V4L2_TUNER_SUB_MONO;
		t->audmode    = V4L2_TUNER_MODE_MONO;
		break;
	}
	return;
}

void cx88_set_stereo(struct cx8800_dev *dev, u32 mode)
{
	u32 ctl  = UNSET;
	u32 mask = UNSET;

	switch (dev->tvaudio) {
	case WW_A2_BG:
	case WW_A2_DK:
	case WW_A2_M:
		switch (mode) {
		case V4L2_TUNER_MODE_MONO:   
		case V4L2_TUNER_MODE_LANG1:
			ctl  = EN_A2_FORCE_MONO1;
			mask = 0x3f;
			break;
		case V4L2_TUNER_MODE_LANG2:
			ctl  = EN_A2_AUTO_MONO2;
			mask = 0x3f;
			break;
		case V4L2_TUNER_MODE_STEREO:
			ctl  = EN_A2_AUTO_STEREO | EN_DMTRX_SUMR;
			mask = 0x8bf;
			break;
		}
		break;
	case WW_NICAM_BGDKL:
		switch (mode) {
		case V4L2_TUNER_MODE_MONO:   
			ctl  = EN_NICAM_FORCE_MONO1;
			mask = 0x3f;
			break;
		case V4L2_TUNER_MODE_LANG1:
			ctl  = EN_NICAM_AUTO_MONO2;
			mask = 0x3f;
			break;
		case V4L2_TUNER_MODE_STEREO:
			ctl  = EN_NICAM_FORCE_STEREO | EN_DMTRX_LR;
			mask = 0x93f;
			break;
		}
		break;	
	case WW_FM:
		switch (mode) {
		case V4L2_TUNER_MODE_MONO:   
			ctl  = EN_FMRADIO_FORCE_MONO;
			mask = 0x3f;
			break;
		case V4L2_TUNER_MODE_STEREO:
			ctl  = EN_FMRADIO_AUTO_STEREO;
			mask = 0x3f;
			break;
		}
		break;	
	}

	if (UNSET != ctl) {
		cx_write(AUD_SOFT_RESET, 0x0001);
		cx_andor(AUD_CTL, mask,  ctl);
		cx_write(AUD_SOFT_RESET, 0x0000);
		dprintk("cx88_set_stereo: mask 0x%x, ctl 0x%x "
			"[status=0x%x,ctl=0x%x,vol=0x%x]\n",
			mask, ctl, cx_read(AUD_STATUS),
			cx_read(AUD_CTL), cx_sread(SHADOW_AUD_VOL_CTL));
	}
	return;
}

/* just monitor the audio status for now ... */
int cx88_audio_thread(void *data)
{
	struct cx8800_dev *dev = data;
	struct v4l2_tuner t;

	daemonize("msp3400");
	allow_signal(SIGTERM);
	dprintk("cx88: tvaudio thread started\n");

	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ*3);
		if (signal_pending(current))
			break;
		if (dev->shutdown)
			break;

		memset(&t,0,sizeof(t));
		cx88_get_stereo(dev,&t);
	}

	dprintk("cx88: tvaudio thread exiting\n");
        complete_and_exit(&dev->texit, 0);
}

/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
