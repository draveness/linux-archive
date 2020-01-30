/*
 *
 * device driver for philips saa7134 based TV cards
 * card-specific stuff.
 *
 * (c) 2001-04 Gerd Knorr <kraxel@bytesex.org> [SuSE Labs]
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/init.h>
#include <linux/module.h>

#include "saa7134-reg.h"
#include "saa7134.h"
#include <media/v4l2-common.h>

/* commly used strings */
static char name_mute[]    = "mute";
static char name_radio[]   = "Radio";
static char name_tv[]      = "Television";
static char name_tv_mono[] = "TV (mono only)";
static char name_comp1[]   = "Composite1";
static char name_comp2[]   = "Composite2";
static char name_comp3[]   = "Composite3";
static char name_comp4[]   = "Composite4";
static char name_svideo[]  = "S-Video";

/* ------------------------------------------------------------------ */
/* board config info                                                  */

struct saa7134_board saa7134_boards[] = {
	[SAA7134_BOARD_UNKNOWN] = {
		.name		= "UNKNOWN/GENERIC",
		.audio_clock	= 0x00187de7,
		.tuner_type	= TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,

		.inputs         = {{
			.name = "default",
			.vmux = 0,
			.amux = LINE1,
		}},
	},
	[SAA7134_BOARD_PROTEUS_PRO] = {
		/* /me */
		.name		= "Proteus Pro [philips reference design]",
		.audio_clock	= 0x00187de7,
		.tuner_type	= TUNER_PHILIPS_PAL,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,

		.inputs         = {{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE1,
		},{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_tv_mono,
			.vmux = 1,
			.amux = LINE2,
			.tv   = 1,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
		},
	},
	[SAA7134_BOARD_FLYVIDEO3000] = {
		/* "Marco d'Itri" <md@Linux.IT> */
		.name		= "LifeView FlyVIDEO3000",
		.audio_clock	= 0x00200000,
		.tuner_type	= TUNER_PHILIPS_PAL,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,

		.gpiomask       = 0xe000,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.gpio = 0x8000,
			.tv   = 1,
		},{
			.name = name_tv_mono,
			.vmux = 1,
			.amux = LINE2,
			.gpio = 0x0000,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE2,
			.gpio = 0x4000,
		},{
			.name = name_comp2,
			.vmux = 3,
			.amux = LINE2,
			.gpio = 0x4000,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
			.gpio = 0x4000,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
			.gpio = 0x2000,
		},
		.mute = {
			.name = name_mute,
			.amux = TV,
			.gpio = 0x8000,
		},
	},
	[SAA7134_BOARD_FLYVIDEO2000] = {
		/* "TC Wan" <tcwan@cs.usm.my> */
		.name           = "LifeView/Typhoon FlyVIDEO2000",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_LG_PAL_NEW_TAPC,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,

		.gpiomask       = 0xe000,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = LINE2,
			.gpio = 0x0000,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE2,
			.gpio = 0x4000,
		},{
			.name = name_comp2,
			.vmux = 3,
			.amux = LINE2,
			.gpio = 0x4000,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
			.gpio = 0x4000,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
			.gpio = 0x2000,
		},
		.mute = {
			.name = name_mute,
			.amux = LINE2,
			.gpio = 0x8000,
		},
	},
	[SAA7134_BOARD_FLYTVPLATINUM_MINI] = {
		/* "Arnaud Quette" <aquette@free.fr> */
		.name           = "LifeView FlyTV Platinum Mini",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,

		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,     /* Composite signal on S-Video input */
			.vmux = 0,
			.amux = LINE2,
		},{
			.name = name_comp2,	/* Composite input */
			.vmux = 3,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
		}},
	},
	[SAA7134_BOARD_FLYTVPLATINUM_FM] = {
		/* LifeView FlyTV Platinum FM (LR214WF) */
		/* "Peter Missel <peter.missel@onlinehome.de> */
		.name           = "LifeView FlyTV Platinum FM / Gold",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,

		.gpiomask       = 0x1E000,	/* Set GP16 and unused 15,14,13 to Output */
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.gpio = 0x10000,	/* GP16=1 selects TV input */
			.tv   = 1,
		},{
/*			.name = name_tv_mono,
			.vmux = 1,
			.amux = LINE2,
			.gpio = 0x0000,
			.tv   = 1,
		},{
*/			.name = name_comp1,	/* Composite signal on S-Video input */
			.vmux = 0,
			.amux = LINE2,
/*			.gpio = 0x4000,         */
		},{
			.name = name_comp2,	/* Composite input */
			.vmux = 3,
			.amux = LINE2,
/*			.gpio = 0x4000,         */
		},{
			.name = name_svideo,	/* S-Video signal on S-Video input */
			.vmux = 8,
			.amux = LINE2,
/*			.gpio = 0x4000,         */
		}},
		.radio = {
			.name = name_radio,
			.amux = TV,
			.gpio = 0x00000,	/* GP16=0 selects FM radio antenna */
		},
		.mute = {
			.name = name_mute,
			.amux = TV,
			.gpio = 0x10000,
		},
	},
	[SAA7134_BOARD_EMPRESS] = {
		/* "Gert Vervoort" <gert.vervoort@philips.com> */
		.name		= "EMPRESS",
		.audio_clock	= 0x00187de7,
		.tuner_type	= TUNER_PHILIPS_PAL,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,

		.inputs         = {{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE1,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		},{
			.name = name_tv,
			.vmux = 1,
			.amux = LINE2,
			.tv   = 1,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
		},
		.mpeg      = SAA7134_MPEG_EMPRESS,
		.video_out = CCIR656,
	},
	[SAA7134_BOARD_MONSTERTV] = {
		/* "K.Ohta" <alpha292@bremen.or.jp> */
		.name           = "SKNet Monster TV",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_NTSC_M,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,

		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE1,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
		},
	},
	[SAA7134_BOARD_MD9717] = {
		.name		= "Tevion MD 9717",
		.audio_clock	= 0x00200000,
		.tuner_type	= TUNER_PHILIPS_PAL,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			/* workaround for problems with normal TV sound */
			.name = name_tv_mono,
			.vmux = 1,
			.amux = LINE2,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 2,
			.amux = LINE1,
		},{
			.name = name_comp2,
			.vmux = 3,
			.amux = LINE1,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
		},
	},
	[SAA7134_BOARD_TVSTATION_RDS] = {
		/* Typhoon TV Tuner RDS: Art.Nr. 50694 */
		.name		= "KNC One TV-Station RDS / Typhoon TV Tuner RDS",
		.audio_clock	= 0x00200000,
		.tuner_type	= TUNER_PHILIPS_FM1216ME_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_tv_mono,
			.vmux = 1,
			.amux   = LINE2,
			.tv   = 1,
		},{

			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE1,
		},{

			.name = "CVid over SVid",
			.vmux = 0,
			.amux = LINE1,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
		},
	},
	[SAA7134_BOARD_TVSTATION_DVR] = {
		.name		= "KNC One TV-Station DVR",
		.audio_clock	= 0x00200000,
		.tuner_type	= TUNER_PHILIPS_FM1216ME_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf	= TDA9887_PRESENT,
		.gpiomask	= 0x820000,
		.inputs		= {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
			.gpio = 0x20000,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
			.gpio = 0x20000,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE1,
			.gpio = 0x20000,
		}},
		.radio		= {
			.name = name_radio,
			.amux = LINE2,
			.gpio = 0x20000,
		},
		.mpeg           = SAA7134_MPEG_EMPRESS,
		.video_out	= CCIR656,
	},
	[SAA7134_BOARD_CINERGY400] = {
		.name           = "Terratec Cinergy 400 TV",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_PHILIPS_PAL,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 4,
			.amux = LINE1,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		},{
			.name = name_comp2, /* CVideo over SVideo Connector */
			.vmux = 0,
			.amux = LINE1,
		}}
	},
	[SAA7134_BOARD_MD5044] = {
		.name           = "Medion 5044",
		.audio_clock    = 0x00187de7, /* was: 0x00200000, */
		.tuner_type     = TUNER_PHILIPS_FM1216ME_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			/* workaround for problems with normal TV sound */
			.name = name_tv_mono,
			.vmux = 1,
			.amux = LINE2,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE2,
		},{
			.name = name_comp2,
			.vmux = 3,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
		},
	},
	[SAA7134_BOARD_KWORLD] = {
		.name           = "Kworld/KuroutoShikou SAA7130-TVPCI",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_NTSC_M,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE1,
		},{
			.name = name_tv,
			.vmux = 1,
			.amux = LINE2,
			.tv   = 1,
		}},
	},
	[SAA7134_BOARD_CINERGY600] = {
		.name           = "Terratec Cinergy 600 TV",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_PHILIPS_PAL,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 4,
			.amux = LINE1,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		},{
			.name = name_comp2, /* CVideo over SVideo Connector */
			.vmux = 0,
			.amux = LINE1,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
		},
	},
	[SAA7134_BOARD_MD7134] = {
		.name           = "Medion 7134",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_FMD1216ME_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs = {{
			.name   = name_tv,
			.vmux   = 1,
			.amux   = TV,
			.tv     = 1,
		},{
			.name   = name_comp1,
			.vmux   = 0,
			.amux   = LINE1,
		},{
			.name   = name_svideo,
			.vmux   = 8,
			.amux   = LINE1,
		}},
		.radio = {
			.name   = name_radio,
			.amux   = LINE2,
		},
	},
	[SAA7134_BOARD_TYPHOON_90031] = {
		/* aka Typhoon "TV+Radio", Art.Nr 90031 */
		/* Tom Zoerner <tomzo at users sourceforge net> */
		.name           = "Typhoon TV+Radio 90031",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_PHILIPS_PAL,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.inputs         = {{
			.name   = name_tv,
			.vmux   = 1,
			.amux   = TV,
			.tv     = 1,
		},{
			.name   = name_comp1,
			.vmux   = 3,
			.amux   = LINE1,
		},{
			.name   = name_svideo,
			.vmux   = 8,
			.amux   = LINE1,
		}},
		.radio = {
			.name   = name_radio,
			.amux   = LINE2,
		},
	},
	[SAA7134_BOARD_ELSA] = {
		.name           = "ELSA EX-VISION 300TV",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_HITACHI_NTSC,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		},{
			.name   = name_comp1,
			.vmux   = 0,
			.amux   = LINE1,
		},{
			.name = name_tv,
			.vmux = 4,
			.amux = LINE2,
			.tv   = 1,
		}},
	},
	[SAA7134_BOARD_ELSA_500TV] = {
		.name           = "ELSA EX-VISION 500TV",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_HITACHI_NTSC,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_svideo,
			.vmux = 7,
			.amux = LINE1,
		},{
			.name = name_tv,
			.vmux = 8,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_tv_mono,
			.vmux = 8,
			.amux = LINE2,
			.tv   = 1,
		}},
	},
	[SAA7134_BOARD_ELSA_700TV] = {
		.name           = "ELSA EX-VISION 700TV",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_HITACHI_NTSC,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_tv,
			.vmux = 4,
			.amux = LINE2,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 6,
			.amux = LINE1,
		},{
			.name = name_svideo,
			.vmux = 7,
			.amux = LINE1,
		}},
		.mute           = {
			.name = name_mute,
			.amux = TV,
		},
	},
	[SAA7134_BOARD_ASUSTeK_TVFM7134] = {
		.name           = "ASUS TV-FM 7134",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_FM1216ME_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 4,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 6,
			.amux = LINE2,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE1,
		},
	},
	[SAA7134_BOARD_ASUSTeK_TVFM7135] = {
		.name           = "ASUS TV-FM 7135",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.gpiomask       = 0x200000,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.gpio = 0x0000,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 4,
			.amux = LINE2,
			.gpio = 0x0000,
		},{
			.name = name_svideo,
			.vmux = 6,
			.amux = LINE2,
			.gpio = 0x0000,
		}},
		.radio = {
			.name = name_radio,
			.amux = TV,
			.gpio = 0x200000,
		},
		.mute  = {
			.name = name_mute,
			.gpio = 0x0000,
		},

	},
	[SAA7134_BOARD_VA1000POWER] = {
		.name           = "AOPEN VA1000 POWER",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_NTSC,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE1,
		},{
			.name = name_tv,
			.vmux = 1,
			.amux = LINE2,
			.tv   = 1,
		}},
	},
	[SAA7134_BOARD_10MOONSTVMASTER] = {
		/* "lilicheng" <llc@linuxfans.org> */
		.name           = "10MOONS PCI TV CAPTURE CARD",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_LG_PAL_NEW_TAPC,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.gpiomask       = 0xe000,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = LINE2,
			.gpio = 0x0000,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE2,
			.gpio = 0x4000,
		},{
			.name = name_comp2,
			.vmux = 3,
			.amux = LINE2,
			.gpio = 0x4000,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
			.gpio = 0x4000,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
			.gpio = 0x2000,
		},
		.mute = {
			.name = name_mute,
			.amux = LINE2,
			.gpio = 0x8000,
		},
	},
	[SAA7134_BOARD_BMK_MPEX_NOTUNER] = {
		/* "Andrew de Quincey" <adq@lidskialf.net> */
		.name		= "BMK MPEX No Tuner",
		.audio_clock	= 0x200000,
		.tuner_type	= TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_comp1,
			.vmux = 4,
			.amux = LINE1,
		},{
			.name = name_comp2,
			.vmux = 3,
			.amux = LINE1,
		},{
			.name = name_comp3,
			.vmux = 0,
			.amux = LINE1,
		},{
			.name = name_comp4,
			.vmux = 1,
			.amux = LINE1,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		}},
		.mpeg      = SAA7134_MPEG_EMPRESS,
		.video_out = CCIR656,
	},
	[SAA7134_BOARD_VIDEOMATE_TV] = {
		.name           = "Compro VideoMate TV",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_NTSC_M,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE1,
		},{
			.name = name_tv,
			.vmux = 1,
			.amux = LINE2,
			.tv   = 1,
		}},
	},
	[SAA7134_BOARD_VIDEOMATE_TV_GOLD_PLUS] = {
		.name           = "Compro VideoMate TV Gold+",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_NTSC_M,
		.gpiomask       = 0x800c0000,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
			.gpio = 0x06c00012,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE1,
			.gpio = 0x0ac20012,
		},{
			.name = name_tv,
			.vmux = 1,
			.amux = LINE2,
			.gpio = 0x08c20012,
			.tv   = 1,
		}},				/* radio and probably mute is missing */
	},
	[SAA7134_BOARD_CRONOS_PLUS] = {
		/*
		gpio pins:
			0  .. 3   BASE_ID
			4  .. 7   PROTECT_ID
			8  .. 11  USER_OUT
			12 .. 13  USER_IN
			14 .. 15  VIDIN_SEL
		*/
		.name           = "Matrox CronosPlus",
		.tuner_type     = TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.gpiomask       = 0xcf00,
		.inputs         = {{
			.name = name_comp1,
			.vmux = 0,
			.gpio = 2 << 14,
		},{
			.name = name_comp2,
			.vmux = 0,
			.gpio = 1 << 14,
		},{
			.name = name_comp3,
			.vmux = 0,
			.gpio = 0 << 14,
		},{
			.name = name_comp4,
			.vmux = 0,
			.gpio = 3 << 14,
		},{
			.name = name_svideo,
			.vmux = 8,
			.gpio = 2 << 14,
		}},
	},
	[SAA7134_BOARD_MD2819] = {
		.name           = "AverMedia M156 / Medion 2819",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_FM1216ME_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE2,
		},{
			.name = name_comp2,
			.vmux = 3,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
		},
	},
	[SAA7134_BOARD_BMK_MPEX_TUNER] = {
		/* "Greg Wickham <greg.wickham@grangenet.net> */
		.name           = "BMK MPEX Tuner",
		.audio_clock    = 0x200000,
		.tuner_type     = TUNER_PHILIPS_PAL,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_comp1,
			.vmux = 1,
			.amux = LINE1,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		},{
			.name = name_tv,
			.vmux = 3,
			.amux = TV,
			.tv   = 1,
		}},
		.mpeg      = SAA7134_MPEG_EMPRESS,
		.video_out = CCIR656,
	},
	[SAA7134_BOARD_ASUSTEK_TVFM7133] = {
		.name           = "ASUS TV-FM 7133",
		.audio_clock    = 0x00187de7,
		/* probably wrong, the 7133 one is the NTSC version ...
		* .tuner_type  = TUNER_PHILIPS_FM1236_MK3 */
		.tuner_type     = TUNER_LG_NTSC_NEW_TAPC,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,

		},{
			.name = name_comp1,
			.vmux = 4,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 6,
			.amux = LINE2,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE1,
		},
	},
	[SAA7134_BOARD_PINNACLE_PCTV_STEREO] = {
		.name           = "Pinnacle PCTV Stereo (saa7134)",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_MT2032,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT | TDA9887_INTERCARRIER | TDA9887_PORT2_INACTIVE,
		.inputs         = {{
			.name = name_tv,
			.vmux = 3,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE2,
		},{
			.name = name_comp2,
			.vmux = 1,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
		}},
	},
	[SAA7134_BOARD_MANLI_MTV002] = {
		/* Ognjen Nastic <ognjen@logosoft.ba> */
		.name           = "Manli MuchTV M-TV002/Behold TV 403 FM",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_PHILIPS_PAL,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		},{
			.name   = name_comp1,
			.vmux   = 1,
			.amux   = LINE1,
		},{
			.name = name_tv,
			.vmux = 3,
			.amux = LINE2,
			.tv   = 1,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
		},
	},
	[SAA7134_BOARD_MANLI_MTV001] = {
		/* Ognjen Nastic <ognjen@logosoft.ba> UNTESTED */
		.name           = "Manli MuchTV M-TV001/Behold TV 401",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_PHILIPS_PAL,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		},{
			.name = name_comp1,
			.vmux = 1,
			.amux = LINE1,
		},{
			.name = name_tv,
			.vmux = 3,
			.amux = LINE2,
			.tv   = 1,
		}},
		.mute = {
			.name = name_mute,
			.amux = LINE1,
		},
	},
	[SAA7134_BOARD_TG3000TV] = {
		/* TransGear 3000TV */
		.name           = "Nagase Sangyo TransGear 3000TV",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_NTSC_M,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = LINE2,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
		}},
	},
	[SAA7134_BOARD_ECS_TVP3XP] = {
		.name           = "Elitegroup ECS TVP3XP FM1216 Tuner Card(PAL-BG,FM) ",
		.audio_clock    = 0x187de7,  /* xtal 32.1 MHz */
		.tuner_type     = TUNER_PHILIPS_PAL,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name   = name_tv,
			.vmux   = 1,
			.amux   = TV,
			.tv     = 1,
		},{
			.name   = name_tv_mono,
			.vmux   = 1,
			.amux   = LINE2,
			.tv     = 1,
		},{
			.name   = name_comp1,
			.vmux   = 3,
			.amux   = LINE1,
		},{
			.name   = name_svideo,
			.vmux   = 8,
			.amux   = LINE1,
		},{
			.name   = "CVid over SVid",
			.vmux   = 0,
			.amux   = LINE1,
		}},
		.radio = {
			.name   = name_radio,
			.amux   = LINE2,
		},
	},
	[SAA7134_BOARD_ECS_TVP3XP_4CB5] = {
		.name           = "Elitegroup ECS TVP3XP FM1236 Tuner Card (NTSC,FM)",
		.audio_clock    = 0x187de7,
		.tuner_type     = TUNER_PHILIPS_NTSC,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name   = name_tv,
			.vmux   = 1,
			.amux   = TV,
			.tv     = 1,
		},{
			.name   = name_tv_mono,
			.vmux   = 1,
			.amux   = LINE2,
			.tv     = 1,
		},{
			.name   = name_comp1,
			.vmux   = 3,
			.amux   = LINE1,
		},{
			.name   = name_svideo,
			.vmux   = 8,
			.amux   = LINE1,
		},{
			.name   = "CVid over SVid",
			.vmux   = 0,
			.amux   = LINE1,
		}},
		.radio = {
			.name   = name_radio,
			.amux   = LINE2,
		},
	},
    [SAA7134_BOARD_ECS_TVP3XP_4CB6] = {
		/* Barry Scott <barry.scott@onelan.co.uk> */
		.name		= "Elitegroup ECS TVP3XP FM1246 Tuner Card (PAL,FM)",
		.audio_clock    = 0x187de7,
		.tuner_type     = TUNER_PHILIPS_PAL_I,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name   = name_tv,
			.vmux   = 1,
			.amux   = TV,
			.tv     = 1,
		},{
			.name   = name_tv_mono,
			.vmux   = 1,
			.amux   = LINE2,
			.tv     = 1,
		},{
			.name   = name_comp1,
			.vmux   = 3,
			.amux   = LINE1,
		},{
			.name   = name_svideo,
			.vmux   = 8,
			.amux   = LINE1,
		},{
			.name   = "CVid over SVid",
			.vmux   = 0,
			.amux   = LINE1,
		}},
		.radio = {
			.name   = name_radio,
			.amux   = LINE2,
		},
	},
	[SAA7134_BOARD_AVACSSMARTTV] = {
		/* Roman Pszonczenko <romka@kolos.math.uni.lodz.pl> */
		.name           = "AVACS SmartTV",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_PAL,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_tv_mono,
			.vmux = 1,
			.amux = LINE2,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE2,
		},{
			.name = name_comp2,
			.vmux = 3,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
			.gpio = 0x200000,
		},
	},
	[SAA7134_BOARD_AVERMEDIA_DVD_EZMAKER] = {
		/* Michael Smith <msmith@cbnco.com> */
		.name           = "AVerMedia DVD EZMaker",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_comp1,
			.vmux = 3,
		},{
			.name = name_svideo,
			.vmux = 8,
		}},
	},
	[SAA7134_BOARD_NOVAC_PRIMETV7133] = {
		/* toshii@netbsd.org */
		.name           = "Noval Prime TV 7133",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_ALPS_TSBH1_NTSC,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_comp1,
			.vmux = 3,
		},{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_svideo,
			.vmux = 8,
		}},
	},
	[SAA7134_BOARD_AVERMEDIA_STUDIO_305] = {
		.name           = "AverMedia AverTV Studio 305",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_FM1256_IH3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = LINE2,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE2,
		},{
			.name = name_comp2,
			.vmux = 3,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
		},
		.mute = {
			.name = name_mute,
			.amux = LINE1,
		},
	},
	[SAA7134_BOARD_UPMOST_PURPLE_TV] = {
		.name           = "UPMOST PURPLE TV",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_FM1236_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.inputs         = {{
			.name = name_tv,
			.vmux = 7,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_svideo,
			.vmux = 7,
			.amux = LINE1,
		}},
	},
	[SAA7134_BOARD_ITEMS_MTV005] = {
		/* Norman Jonas <normanjonas@arcor.de> */
		.name           = "Items MuchTV Plus / IT-005",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_PAL,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_tv,
			.vmux = 3,
			.amux = TV,
			.tv   = 1,
		},{
			.name   = name_comp1,
			.vmux   = 1,
			.amux   = LINE1,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
		},
	},
	[SAA7134_BOARD_CINERGY200] = {
		.name           = "Terratec Cinergy 200 TV",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_PHILIPS_PAL,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = LINE2,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 4,
			.amux = LINE1,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		},{
			.name = name_comp2, /* CVideo over SVideo Connector */
			.vmux = 0,
			.amux = LINE1,
		}},
		.mute = {
			.name = name_mute,
			.amux = LINE2,
		},
	},
	[SAA7134_BOARD_VIDEOMATE_TV_PVR] = {
		/* Alain St-Denis <alain@topaze.homeip.net> */
		.name           = "Compro VideoMate TV PVR/FM",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_NTSC_M,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.gpiomask	= 0x808c0080,
		.inputs         = {{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
			.gpio = 0x00080,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE1,
			.gpio = 0x00080,
		},{
			.name = name_tv,
			.vmux = 1,
			.amux = LINE2_LEFT,
			.tv   = 1,
			.gpio = 0x00080,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
			.gpio = 0x80000,
		},
		.mute = {
			.name = name_mute,
			.amux = LINE2,
			.gpio = 0x40000,
		},
	},
	[SAA7134_BOARD_SABRENT_SBTTVFM] = {
		/* Michael Rodriguez-Torrent <mrtorrent@asu.edu> */
		.name           = "Sabrent SBT-TVFM (saa7130)",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_NTSC_M,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_comp1,
			.vmux = 1,
			.amux = LINE1,
		},{
			.name = name_tv,
			.vmux = 3,
			.amux = LINE2,
			.tv   = 1,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		}},
		.radio = {
			.name   = name_radio,
			.amux   = LINE2,
		},
	},
	[SAA7134_BOARD_ZOLID_XPERT_TV7134] = {
		/* Helge Jensen <helge.jensen@slog.dk> */
		.name           = ":Zolid Xpert TV7134",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_NTSC,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE1,
		},{
			.name = name_tv,
			.vmux = 1,
			.amux = LINE2,
			.tv   = 1,
		}},
	},
	[SAA7134_BOARD_EMPIRE_PCI_TV_RADIO_LE] = {
		/* "Matteo Az" <matte.az@nospam.libero.it> ;-) */
		.name           = "Empire PCI TV-Radio LE",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_PAL,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.gpiomask       = 0x4000,
		.inputs         = {{
			.name = name_tv_mono,
			.vmux = 1,
			.amux = LINE2,
			.gpio = 0x8000,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE1,
			.gpio = 0x8000,
		},{
			.name = name_svideo,
			.vmux = 6,
			.amux = LINE1,
			.gpio = 0x8000,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE1,
			.gpio = 0x8000,
		},
		.mute = {
			.name = name_mute,
			.amux = TV,
			.gpio =0x8000,
		}
	},
	[SAA7134_BOARD_AVERMEDIA_STUDIO_307] = {
		/*
		Nickolay V. Shmyrev <nshmyrev@yandex.ru>
		Lots of thanks to Andrey Zolotarev <zolotarev_andrey@mail.ru>
		*/
		.name           = "Avermedia AVerTV Studio 307",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_FM1256_IH3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.gpiomask       = 0x03,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
			.gpio = 0x00,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE1,
			.gpio = 0x02,
		},{
			.name = name_comp2,
			.vmux = 3,
			.amux = LINE1,
			.gpio = 0x02,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
			.gpio = 0x02,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE1,
			.gpio = 0x01,
		},
		.mute  = {
			.name = name_mute,
			.amux = LINE1,
			.gpio = 0x00,
		},
	},
	[SAA7134_BOARD_AVERMEDIA_GO_007_FM] = {
		.name           = "Avermedia AVerTV GO 007 FM",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.gpiomask       = 0x00300003,
		/* .gpiomask       = 0x8c240003, */
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
			.gpio = 0x01,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE1,
			.gpio = 0x02,
		},{
			.name = name_svideo,
			.vmux = 6,
			.amux = LINE1,
			.gpio = 0x02,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE1,
			.gpio = 0x00300001,
		},
		.mute = {
			.name = name_mute,
			.amux = TV,
			.gpio = 0x01,
		},
	},
	[SAA7134_BOARD_AVERMEDIA_CARDBUS] = {
		/* Kees.Blom@cwi.nl */
		.name           = "AVerMedia Cardbus TV/Radio (E500)",
		.audio_clock    = 0x187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE1,
		},
	},
	[SAA7134_BOARD_CINERGY400_CARDBUS] = {
		.name           = "Terratec Cinergy 400 mobile",
		.audio_clock    = 0x187de7,
		.tuner_type     = TUNER_ALPS_TSBE5_PAL,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_tv_mono,
			.vmux = 1,
			.amux = LINE2,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE1,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		}},
	},
	[SAA7134_BOARD_CINERGY600_MK3] = {
		.name           = "Terratec Cinergy 600 TV MK3",
		.audio_clock    = 0x00200000,
		.tuner_type	= TUNER_PHILIPS_FM1216ME_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 4,
			.amux = LINE1,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		},{
			.name = name_comp2, /* CVideo over SVideo Connector */
			.vmux = 0,
			.amux = LINE1,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
		},
	},
	[SAA7134_BOARD_VIDEOMATE_GOLD_PLUS] = {
		/* Dylan Walkden <dylan_walkden@hotmail.com> */
		.name		= "Compro VideoMate Gold+ Pal",
		.audio_clock	= 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_PAL,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.gpiomask	= 0x1ce780,
		.inputs		= {{
			.name = name_svideo,
			.vmux = 0,		/* CVideo over SVideo Connector - ok? */
			.amux = LINE1,
			.gpio = 0x008080,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE1,
			.gpio = 0x008080,
		},{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
			.gpio = 0x008080,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
			.gpio = 0x80000,
		},
		.mute = {
			.name = name_mute,
			.amux = LINE2,
			.gpio = 0x0c8000,
		},
	},
	[SAA7134_BOARD_PINNACLE_300I_DVBT_PAL] = {
		.name           = "Pinnacle PCTV 300i DVB-T + PAL",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_MT2032,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT | TDA9887_INTERCARRIER | TDA9887_PORT2_INACTIVE,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs         = {{
			.name = name_tv,
			.vmux = 3,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE2,
		},{
			.name = name_comp2,
			.vmux = 1,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
		}},
	},
	[SAA7134_BOARD_PROVIDEO_PV952] = {
		/* andreas.kretschmer@web.de */
		.name		= "ProVideo PV952",
		.audio_clock	= 0x00187de7,
		.tuner_type	= TUNER_PHILIPS_FM1216ME_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.inputs         = {{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE1,
		},{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_tv_mono,
			.vmux = 1,
			.amux = LINE2,
			.tv   = 1,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
		},
	},
	[SAA7134_BOARD_AVERMEDIA_305] = {
		/* much like the "studio" version but without radio
		* and another tuner (sirspiritus@yandex.ru) */
		.name           = "AverMedia AverTV/305",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_FQ1216ME,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = LINE2,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE2,
		},{
			.name = name_comp2,
			.vmux = 3,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
		}},
		.mute = {
			.name = name_mute,
			.amux = LINE1,
		},
	},
	[SAA7134_BOARD_FLYDVBTDUO] = {
		/* LifeView FlyDVB-T DUO */
		/* "Nico Sabbi <nsabbi@tiscali.it>  Hartmut Hackmann hartmut.hackmann@t-online.de*/
		.name           = "LifeView FlyDVB-T DUO / MSI TV@nywhere Duo",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.gpiomask	= 0x00200000,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.gpio = 0x200000,	/* GPIO21=High for TV input */
			.tv   = 1,
		},{
			.name = name_comp1,	/* Composite signal on S-Video input */
			.vmux = 0,
			.amux = LINE2,
		},{
			.name = name_comp2,	/* Composite input */
			.vmux = 3,
			.amux = LINE2,
		},{
			.name = name_svideo,	/* S-Video signal on S-Video input */
			.vmux = 8,
			.amux = LINE2,
		}},
		.radio = {
			.name = name_radio,
			.amux = TV,
			.gpio = 0x000000,	/* GPIO21=Low for FM radio antenna */
		},
	},
	[SAA7134_BOARD_PHILIPS_TOUGH] = {
		.name           = "Philips TOUGH DVB-T reference design",
		.tuner_type	= TUNER_ABSENT,
		.audio_clock    = 0x00187de7,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs = {{
			.name   = name_comp1,
			.vmux   = 0,
			.amux   = LINE1,
		},{
			.name   = name_svideo,
			.vmux   = 8,
			.amux   = LINE1,
		}},
	},
	[SAA7134_BOARD_AVERMEDIA_307] = {
		/*
		Davydov Vladimir <vladimir@iqmedia.com>
		*/
		.name           = "Avermedia AVerTV 307",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_FQ1216ME,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE1,
		},{
			.name = name_comp2,
			.vmux = 3,
			.amux = LINE1,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		}},
	},
	[SAA7134_BOARD_ADS_INSTANT_TV] = {
		.name           = "ADS Tech Instant TV (saa7135)",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
		}},
	},
	[SAA7134_BOARD_KWORLD_VSTREAM_XPERT] = {
		.name           = "Kworld/Tevion V-Stream Xpert TV PVR7134",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_PAL_I,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.gpiomask	= 0x0700,
		.inputs = {{
			.name   = name_tv,
			.vmux   = 1,
			.amux   = TV,
			.tv     = 1,
			.gpio   = 0x000,
		},{
			.name   = name_comp1,
			.vmux   = 3,
			.amux   = LINE1,
			.gpio   = 0x200,		/* gpio by DScaler */
		},{
			.name   = name_svideo,
			.vmux   = 0,
			.amux   = LINE1,
			.gpio   = 0x200,
		}},
		.radio = {
			.name   = name_radio,
			.amux   = LINE1,
			.gpio   = 0x100,
		},
		.mute  = {
			.name = name_mute,
			.amux = TV,
			.gpio = 0x000,
		},
	},
	[SAA7134_BOARD_FLYDVBT_DUO_CARDBUS] = {
		.name		= "LifeView/Typhoon/Genius FlyDVB-T Duo Cardbus",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.mpeg           = SAA7134_MPEG_DVB,
		.gpiomask	= 0x00200000,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.gpio = 0x200000,	/* GPIO21=High for TV input */
			.tv   = 1,
		},{
			.name = name_svideo,	/* S-Video signal on S-Video input */
			.vmux = 8,
			.amux = LINE2,
		},{
			.name = name_comp1,	/* Composite signal on S-Video input */
			.vmux = 0,
			.amux = LINE2,
		},{
			.name = name_comp2,	/* Composite input */
			.vmux = 3,
			.amux = LINE2,
		}},
		.radio = {
			.name = name_radio,
			.amux = TV,
			.gpio = 0x000000,	/* GPIO21=Low for FM radio antenna */
		},
	},
	[SAA7134_BOARD_VIDEOMATE_TV_GOLD_PLUSII] = {
		.name           = "Compro VideoMate TV Gold+II",
		.audio_clock    = 0x002187de7,
		.tuner_type     = TUNER_LG_PAL_NEW_TAPC,
		.radio_type     = TUNER_TEA5767,
		.tuner_addr     = 0x63,
		.radio_addr     = 0x60,
		.gpiomask       = 0x8c1880,
		.inputs         = {{
			.name = name_svideo,
			.vmux = 0,
			.amux = LINE1,
			.gpio = 0x800800,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE1,
			.gpio = 0x801000,
		},{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
			.gpio = 0x800000,
		}},
		.radio = {
			.name = name_radio,
			.amux = TV,
			.gpio = 0x880000,
		},
		.mute = {
			.name = name_mute,
			.amux = LINE2,
			.gpio = 0x840000,
		},
	},
	[SAA7134_BOARD_KWORLD_XPERT] = {
		/*
		FIXME:
		- Remote control doesn't initialize properly.
		- Audio volume starts muted,
		then gradually increases after channel change.
		- Overlay scaling problems (application error?)
		- Composite S-Video untested.
		From: Konrad Rzepecki <hannibal@megapolis.pl>
		*/
		.name           = "Kworld Xpert TV PVR7134",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_TENA_9533_DI,
		.radio_type     = TUNER_TEA5767,
		.tuner_addr	= 0x61,
		.radio_addr	= 0x60,
		.gpiomask	= 0x0700,
		.inputs = {{
			.name   = name_tv,
			.vmux   = 1,
			.amux   = TV,
			.tv     = 1,
			.gpio   = 0x000,
		},{
			.name   = name_comp1,
			.vmux   = 3,
			.amux   = LINE1,
			.gpio   = 0x200,		/* gpio by DScaler */
		},{
			.name   = name_svideo,
			.vmux   = 0,
			.amux   = LINE1,
			.gpio   = 0x200,
		}},
		.radio = {
			.name   = name_radio,
			.amux   = LINE1,
			.gpio   = 0x100,
		},
		.mute = {
			.name = name_mute,
			.amux = TV,
			.gpio = 0x000,
		},
	},
	[SAA7134_BOARD_FLYTV_DIGIMATRIX] = {
		.name		= "FlyTV mini Asus Digimatrix",
		.audio_clock	= 0x00200000,
		.tuner_type	= TUNER_LG_TALN,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_tv_mono,
			.vmux = 1,
			.amux = LINE2,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE2,
		},{
			.name = name_comp2,
			.vmux = 3,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
		}},
		.radio = {
			.name = name_radio,		/* radio unconfirmed */
			.amux = LINE2,
		},
	},
	[SAA7134_BOARD_KWORLD_TERMINATOR] = {
		/* Kworld V-Stream Studio TV Terminator */
		/* "James Webb <jrwebb@qwest.net> */
		.name           = "V-Stream Studio TV Terminator",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr     = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.gpiomask       = 1 << 21,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.gpio = 0x0000000,
			.tv   = 1,
		},{
			.name = name_comp1,     /* Composite input */
			.vmux = 3,
			.amux = LINE2,
			.gpio = 0x0000000,
		},{
			.name = name_svideo,    /* S-Video input */
			.vmux = 8,
			.amux = LINE2,
			.gpio = 0x0000000,
		}},
		.radio = {
			.name = name_radio,
			.amux = TV,
			.gpio = 0x0200000,
		},
	},
	[SAA7134_BOARD_YUAN_TUN900] = {
		/* FIXME:
		 * S-Video and composite sources untested.
		 * Radio not working.
		 * Remote control not yet implemented.
		 * From : codemaster@webgeeks.be */
		.name           = "Yuan TUN-900 (saa7135)",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr= ADDR_UNSET,
		.radio_addr= ADDR_UNSET,
		.gpiomask       = 0x00010003,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
			.gpio = 0x01,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE2,
			.gpio = 0x02,
		},{
			.name = name_svideo,
			.vmux = 6,
			.amux = LINE2,
			.gpio = 0x02,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE1,
			.gpio = 0x00010003,
		},
		.mute = {
			.name = name_mute,
			.amux = TV,
			.gpio = 0x01,
		},
	},
	[SAA7134_BOARD_BEHOLD_409FM] = {
		/* <http://tuner.beholder.ru>, Sergey <skiv@orel.ru> */
		.name           = "Beholder BeholdTV 409 FM",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_FM1216ME_MK3,
		.radio_type     = UNSET,
		.tuner_addr     = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.inputs         = {{
			  .name = name_tv,
			  .vmux = 3,
			  .amux = TV,
			  .tv   = 1,
		},{
			  .name = name_comp1,
			  .vmux = 1,
			  .amux = LINE1,
		},{
			  .name = name_svideo,
			  .vmux = 8,
			  .amux = LINE1,
		}},
		.radio = {
			  .name = name_radio,
			  .amux = LINE2,
		},
	},
	[SAA7134_BOARD_GOTVIEW_7135] = {
		/* Mike Baikov <mike@baikov.com> */
		/* Andrey Cvetcov <ays14@yandex.ru> */
		.name            = "GoTView 7135 PCI",
		.audio_clock     = 0x00187de7,
		.tuner_type      = TUNER_PHILIPS_FM1216ME_MK3,
		.radio_type      = UNSET,
		.tuner_addr      = ADDR_UNSET,
		.radio_addr      = ADDR_UNSET,
		.tda9887_conf    = TDA9887_PRESENT,
		.gpiomask        = 0x00200003,
		.inputs          = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
			.gpio = 0x00200003,
		},{
			.name = name_tv_mono,
			.vmux = 1,
			.amux = LINE2,
			.gpio = 0x00200003,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE1,
			.gpio = 0x00200003,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
			.gpio = 0x00200003,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
			.gpio = 0x00200003,
		},
		.mute = {
			.name = name_mute,
			.amux = TV,
			.gpio = 0x00200003,
		},
	},
	[SAA7134_BOARD_PHILIPS_EUROPA] = {
		.name           = "Philips EUROPA V3 reference design",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TD1316,
		.radio_type     = UNSET,
		.tuner_addr	= 0x61,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT | TDA9887_PORT1_ACTIVE,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs = {{
			.name   = name_tv,
			.vmux   = 3,
			.amux   = TV,
			.tv     = 1,
		},{
			.name   = name_comp1,
			.vmux   = 0,
			.amux   = LINE2,
		},{
			.name   = name_svideo,
			.vmux   = 8,
			.amux   = LINE2,
		}},
	},
	[SAA7134_BOARD_VIDEOMATE_DVBT_300] = {
		.name           = "Compro Videomate DVB-T300",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TD1316,
		.radio_type     = UNSET,
		.tuner_addr	= 0x61,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT | TDA9887_PORT1_ACTIVE,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs = {{
			.name   = name_tv,
			.vmux   = 3,
			.amux   = TV,
			.tv     = 1,
		},{
			.name   = name_comp1,
			.vmux   = 1,
			.amux   = LINE2,
		},{
			.name   = name_svideo,
			.vmux   = 8,
			.amux   = LINE2,
		}},
	},
	[SAA7134_BOARD_VIDEOMATE_DVBT_200] = {
		.name           = "Compro Videomate DVB-T200",
		.tuner_type	= TUNER_ABSENT,
		.audio_clock    = 0x00187de7,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs = {{
			.name   = name_comp1,
			.vmux   = 0,
			.amux   = LINE1,
		},{
			.name   = name_svideo,
			.vmux   = 8,
			.amux   = LINE1,
		}},
	},
	[SAA7134_BOARD_RTD_VFG7350] = {
		.name		= "RTD Embedded Technologies VFG7350",
		.audio_clock	= 0x00200000,
		.tuner_type	= TUNER_ABSENT,
		.radio_type	= UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs		= {{
			.name   = "Composite 0",
			.vmux   = 0,
			.amux   = LINE1,
		},{
			.name   = "Composite 1",
			.vmux   = 1,
			.amux   = LINE2,
		},{
			.name   = "Composite 2",
			.vmux   = 2,
			.amux   = LINE1,
		},{
			.name   = "Composite 3",
			.vmux   = 3,
			.amux   = LINE2,
		},{
			.name   = "S-Video 0",
			.vmux   = 8,
			.amux   = LINE1,
		},{
			.name   = "S-Video 1",
			.vmux   = 9,
			.amux   = LINE2,
		}},
		.mpeg           = SAA7134_MPEG_EMPRESS,
		.video_out      = CCIR656,
		.vid_port_opts  = ( SET_T_CODE_POLARITY_NON_INVERTED |
				    SET_CLOCK_NOT_DELAYED |
				    SET_CLOCK_INVERTED |
				    SET_VSYNC_OFF ),
	},
	[SAA7134_BOARD_RTD_VFG7330] = {
		.name		= "RTD Embedded Technologies VFG7330",
		.audio_clock	= 0x00200000,
		.tuner_type	= TUNER_ABSENT,
		.radio_type	= UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs		= {{
			.name   = "Composite 0",
			.vmux   = 0,
			.amux   = LINE1,
		},{
			.name   = "Composite 1",
			.vmux   = 1,
			.amux   = LINE2,
		},{
			.name   = "Composite 2",
			.vmux   = 2,
			.amux   = LINE1,
		},{
			.name   = "Composite 3",
			.vmux   = 3,
			.amux   = LINE2,
		},{
			.name   = "S-Video 0",
			.vmux   = 8,
			.amux   = LINE1,
		},{
			.name   = "S-Video 1",
			.vmux   = 9,
			.amux   = LINE2,
		}},
	},
	[SAA7134_BOARD_FLYTVPLATINUM_MINI2] = {
		.name           = "LifeView FlyTV Platinum Mini2",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,

		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,     /* Composite signal on S-Video input */
			.vmux = 0,
			.amux = LINE2,
		},{
			.name = name_comp2,	/* Composite input */
			.vmux = 3,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
		}},
	},
	[SAA7134_BOARD_AVERMEDIA_AVERTVHD_A180] = {
		/* Michael Krufky <mkrufky@m1k.net>
		 * Uses Alps Electric TDHU2, containing NXT2004 ATSC Decoder
		 * AFAIK, there is no analog demod, thus,
		 * no support for analog television.
		 */
		.name           = "AVerMedia AVerTVHD MCE A180",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr     = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs         = {{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
		}},
	},
	[SAA7134_BOARD_MONSTERTV_MOBILE] = {
		.name           = "SKNet MonsterTV Mobile",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,

		.inputs         = {{
			  .name = name_tv,
			  .vmux = 1,
			  .amux = TV,
			  .tv   = 1,
		},{
			  .name = name_comp1,
			  .vmux = 3,
			  .amux = LINE1,
		},{
			  .name = name_svideo,
			  .vmux = 6,
			  .amux = LINE1,
		}},
	},
	[SAA7134_BOARD_PINNACLE_PCTV_110i] = {
	       .name           = "Pinnacle PCTV 40i/50i/110i (saa7133)",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr     = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.gpiomask       = 0x080200000,
		.inputs         = {{
			  .name = name_tv,
			  .vmux = 4,
			  .amux = TV,
			  .tv   = 1,
		},{
			  .name = name_comp1,
			  .vmux = 1,
			 .amux = LINE2,
	       },{
			 .name = name_comp2,
			 .vmux = 0,
			  .amux = LINE2,
		},{
			  .name = name_svideo,
			  .vmux = 8,
			  .amux = LINE2,
		}},
		.radio = {
			  .name = name_radio,
			  .amux = LINE1,
		},
	},
	[SAA7134_BOARD_ASUSTeK_P7131_DUAL] = {
		.name           = "ASUSTeK P7131 Dual",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.gpiomask	= 1 << 21,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
			.gpio = 0x0000000,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE2,
			.gpio = 0x0200000,
		},{
			.name = name_comp2,
			.vmux = 0,
			.amux = LINE2,
			.gpio = 0x0200000,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
			.gpio = 0x0200000,
		}},
		.radio = {
			.name = name_radio,
			.amux = TV,
			.gpio = 0x0200000,
		},
	},
	[SAA7134_BOARD_SEDNA_PC_TV_CARDBUS] = {
		/* Paul Tom Zalac <pzalac@gmail.com> */
		/* Pavel Mihaylov <bin@bash.info> */
		.name           = "Sedna/MuchTV PC TV Cardbus TV/Radio (ITO25 Rev:2B)",
				/* Sedna/MuchTV (OEM) Cardbus TV Tuner */
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr     = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.gpiomask       = 0xe880c0,
		.inputs         = {{
			.name = name_tv,
			.vmux = 3,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 1,
			.amux = LINE1,
		},{
			.name = name_svideo,
			.vmux = 6,
			.amux = LINE1,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
		},
	},
	[SAA7134_BOARD_ASUSTEK_DIGIMATRIX_TV] = {
		/* "Cyril Lacoux (Yack)" <clacoux@ifeelgood.org> */
		.name           = "ASUS Digimatrix TV",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_PHILIPS_FQ1216ME,
		.tda9887_conf   = TDA9887_PRESENT,
		.radio_type     = UNSET,
		.tuner_addr     = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE1,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		}},
	},
	[SAA7134_BOARD_PHILIPS_TIGER] = {
		.name           = "Philips Tiger reference design",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tuner_config   = 0,
		.mpeg           = SAA7134_MPEG_DVB,
		.gpiomask       = 0x0200000,
		.inputs = {{
			.name   = name_tv,
			.vmux   = 1,
			.amux   = TV,
			.tv     = 1,
		},{
			.name   = name_comp1,
			.vmux   = 3,
			.amux   = LINE1,
		},{
			.name   = name_svideo,
			.vmux   = 8,
			.amux   = LINE1,
		}},
		.radio = {
			.name   = name_radio,
			.amux   = TV,
			.gpio   = 0x0200000,
		},
	},
	[SAA7134_BOARD_MSI_TVATANYWHERE_PLUS] = {
		.name           = "MSI TV@Anywhere plus",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.gpiomask       = 1 << 21,
		.inputs = {{
			.name   = name_tv,
			.vmux   = 1,
			.amux   = TV,
			.tv     = 1,
		},{
			.name   = name_comp1,
			.vmux   = 3,
			.amux   = LINE2,	/* unconfirmed, taken from Philips driver */
		},{
			.name   = name_comp2,
			.vmux   = 0,		/* untested, Composite over S-Video */
			.amux   = LINE2,
		},{
			.name   = name_svideo,
			.vmux   = 8,
			.amux   = LINE2,
		}},
		.radio = {
			.name   = name_radio,
			.amux   = TV,
			.gpio   = 0x0200000,
		},
	},
	[SAA7134_BOARD_CINERGY250PCI] = {
		/* remote-control does not work. The signal about a
		   key press comes in via gpio, but the key code
		   doesn't. Neither does it have an i2c remote control
		   interface. */
		.name           = "Terratec Cinergy 250 PCI TV",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.gpiomask       = 0x80200000,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_svideo,  /* NOT tested */
			.vmux = 8,
			.amux = LINE1,
		}},
		.radio = {
			.name   = name_radio,
			.amux   = TV,
			.gpio   = 0x0200000,
		},
	},
	[SAA7134_BOARD_FLYDVB_TRIO] = {
		/* LifeView LR319 FlyDVB Trio */
		/* Peter Missel <peter.missel@onlinehome.de> */
		.name           = "LifeView FlyDVB Trio",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.gpiomask	= 0x00200000,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs         = {{
			.name = name_tv,	/* Analog broadcast/cable TV */
			.vmux = 1,
			.amux = TV,
			.gpio = 0x200000,	/* GPIO21=High for TV input */
			.tv   = 1,
		},{
			.name = name_svideo,	/* S-Video signal on S-Video input */
			.vmux = 8,
			.amux = LINE2,
		},{
			.name = name_comp1,	/* Composite signal on S-Video input */
			.vmux = 0,
			.amux = LINE2,
		},{
			.name = name_comp2,	/* Composite input */
			.vmux = 3,
			.amux = LINE2,
		}},
		.radio = {
			.name = name_radio,
			.amux = TV,
			.gpio = 0x000000,	/* GPIO21=Low for FM radio antenna */
		},
	},
	[SAA7134_BOARD_AVERMEDIA_777] = {
		.name           = "AverTV DVB-T 777",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs = {{
			.name   = name_comp1,
			.vmux   = 1,
			.amux   = LINE1,
		},{
			.name   = name_svideo,
			.vmux   = 8,
			.amux   = LINE1,
		}},
	},
	[SAA7134_BOARD_FLYDVBT_LR301] = {
		/* LifeView FlyDVB-T */
		/* Giampiero Giancipoli <gianci@libero.it> */
		.name           = "LifeView FlyDVB-T / Genius VideoWonder DVB-T",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs         = {{
			.name = name_comp1,	/* Composite input */
			.vmux = 3,
			.amux = LINE2,
		},{
			.name = name_svideo,	/* S-Video signal on S-Video input */
			.vmux = 8,
			.amux = LINE2,
		}},
	},
	[SAA7134_BOARD_ADS_DUO_CARDBUS_PTV331] = {
		.name           = "ADS Instant TV Duo Cardbus PTV331",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.mpeg           = SAA7134_MPEG_DVB,
		.gpiomask       = 0x00600000, /* Bit 21 0=Radio, Bit 22 0=TV */
		.inputs = {{
			.name   = name_tv,
			.vmux   = 1,
			.amux   = TV,
			.tv     = 1,
			.gpio   = 0x00200000,
		}},
	},
	[SAA7134_BOARD_TEVION_DVBT_220RF] = {
		.name           = "Tevion/KWorld DVB-T 220RF",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr     = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs = {{
			.name   = name_tv,
			.vmux   = 1,
			.amux   = TV,
			.tv     = 1,
		},{
			.name   = name_comp1,
			.vmux   = 3,
			.amux   = LINE1,
		},{
			.name   = name_svideo,
			.vmux   = 0,
			.amux   = LINE1,
		}},
		.radio = {
			.name   = name_radio,
			.amux   = LINE1,
		},
	},
	[SAA7134_BOARD_KWORLD_DVBT_210] = {
		.name           = "KWorld DVB-T 210",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.mpeg           = SAA7134_MPEG_DVB,
		.gpiomask       = 1 << 21,
		.inputs = {{
			.name   = name_tv,
			.vmux   = 1,
			.amux   = TV,
			.tv     = 1,
		},{
			.name   = name_comp1,
			.vmux   = 3,
			.amux   = LINE1,
		},{
			.name   = name_svideo,
			.vmux   = 8,
			.amux   = LINE1,
		}},
		.radio = {
			.name   = name_radio,
			.amux   = TV,
			.gpio   = 0x0200000,
		},
	},
	[SAA7134_BOARD_KWORLD_ATSC110] = {
		.name           = "Kworld ATSC110",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TUV1236D,
		.radio_type     = UNSET,
		.tuner_addr     = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
		}},
	},
	[SAA7134_BOARD_AVERMEDIA_A169_B] = {
		/* AVerMedia A169  */
		/* Rickard Osser <ricky@osser.se>  */
		/* This card has two saa7134 chips on it,
		   but only one of them is currently working. */
		.name		= "AVerMedia A169 B",
		.audio_clock    = 0x02187de7,
		.tuner_type	= TUNER_LG_TALN,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.gpiomask       = 0x0a60000,
	},
	[SAA7134_BOARD_AVERMEDIA_A169_B1] = {
		/* AVerMedia A169 */
		/* Rickard Osser <ricky@osser.se> */
		.name		= "AVerMedia A169 B1",
		.audio_clock    = 0x02187de7,
		.tuner_type	= TUNER_LG_TALN,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.gpiomask       = 0xca60000,
		.inputs         = {{
			.name = name_tv,
			.vmux = 4,
			.amux = TV,
			.tv   = 1,
			.gpio = 0x04a61000,
		},{
			.name = name_comp2,  /*  Composite SVIDEO (B/W if signal is carried with SVIDEO) */
			.vmux = 1,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 9,           /* 9 is correct as S-VIDEO1 according to a169.inf! */
			.amux = LINE1,
		}},
	},
	[SAA7134_BOARD_MD7134_BRIDGE_2] = {
		/* This card has two saa7134 chips on it,
		   but only one of them is currently working.
		   The programming for the primary decoder is
		   in SAA7134_BOARD_MD7134 */
		.name           = "Medion 7134 Bridge #2",
		.audio_clock    = 0x00187de7,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
	},
	[SAA7134_BOARD_FLYDVBT_HYBRID_CARDBUS] = {
		.name		= "LifeView FlyDVB-T Hybrid Cardbus",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.mpeg           = SAA7134_MPEG_DVB,
		.gpiomask       = 0x00600000, /* Bit 21 0=Radio, Bit 22 0=TV */
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.gpio = 0x200000,	/* GPIO21=High for TV input */
			.tv   = 1,
		},{
			.name = name_svideo,	/* S-Video signal on S-Video input */
			.vmux = 8,
			.amux = LINE2,
		},{
			.name = name_comp1,	/* Composite signal on S-Video input */
			.vmux = 0,
			.amux = LINE2,
		},{
			.name = name_comp2,	/* Composite input */
			.vmux = 3,
			.amux = LINE2,
		}},
		.radio = {
			.name = name_radio,
			.amux = TV,
			.gpio = 0x000000,	/* GPIO21=Low for FM radio antenna */
		},
	},
	[SAA7134_BOARD_FLYVIDEO3000_NTSC] = {
		/* "Zac Bowling" <zac@zacbowling.com> */
		.name           = "LifeView FlyVIDEO3000 (NTSC)",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_PHILIPS_NTSC,
		.radio_type     = UNSET,
		.tuner_addr     = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,

		.gpiomask       = 0xe000,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.gpio = 0x8000,
			.tv   = 1,
		},{
			.name = name_tv_mono,
			.vmux = 1,
			.amux = LINE2,
			.gpio = 0x0000,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE2,
			.gpio = 0x4000,
		},{
			.name = name_comp2,
			.vmux = 3,
			.amux = LINE2,
			.gpio = 0x4000,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
			.gpio = 0x4000,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
			.gpio = 0x2000,
		},
			.mute = {
			.name = name_mute,
			.amux = TV,
			.gpio = 0x8000,
		},
	},
	[SAA7134_BOARD_MEDION_MD8800_QUADRO] = {
		.name           = "Medion Md8800 Quadro",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr     = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs = {{
			.name   = name_tv,
			.vmux   = 1,
			.amux   = TV,
			.tv     = 1,
		},{
			.name   = name_comp1,
			.vmux   = 0,
			.amux   = LINE2,
		},{
			.name   = name_svideo,
			.vmux   = 8,
			.amux   = LINE2,
		}},
	},
	[SAA7134_BOARD_FLYDVBS_LR300] = {
		/* LifeView FlyDVB-s */
		/* Igor M. Liplianin <liplianin@tut.by> */
		.name           = "LifeView FlyDVB-S /Acorp TV134DS",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs         = {{
			.name = name_comp1,	/* Composite input */
			.vmux = 3,
			.amux = LINE1,
		},{
			.name = name_svideo,	/* S-Video signal on S-Video input */
			.vmux = 8,
			.amux = LINE1,
		}},
	},
	[SAA7134_BOARD_PROTEUS_2309] = {
		.name           = "Proteus Pro 2309",
		.audio_clock    = 0x00187de7,
		.tuner_type	= TUNER_PHILIPS_FM1216ME_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = LINE2,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE2,
		},{
			.name = name_comp2,
			.vmux = 3,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
		}},
		.mute = {
			.name = name_mute,
			.amux = LINE1,
		},
	},
	[SAA7134_BOARD_AVERMEDIA_A16AR] = {
		/* Petr Baudis <pasky@ucw.cz> */
		.name           = "AVerMedia TV Hybrid A16AR",
		.audio_clock    = 0x187de7,
		.tuner_type     = TUNER_PHILIPS_TD1316, /* untested */
		.radio_type     = TUNER_TEA5767, /* untested */
		.tuner_addr     = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE1,
		},
	},
	[SAA7134_BOARD_ASUS_EUROPA2_HYBRID] = {
		.name           = "Asus Europa2 OEM",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_FMD1216ME_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT| TDA9887_PORT1_ACTIVE | TDA9887_PORT2_ACTIVE,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs = {{
			.name   = name_tv,
			.vmux   = 3,
			.amux   = TV,
			.tv     = 1,
		},{
			.name   = name_comp1,
			.vmux   = 4,
			.amux   = LINE2,
		},{
			.name   = name_svideo,
			.vmux   = 8,
			.amux   = LINE2,
		}},
		.radio = {
			.name   = name_radio,
			.amux   = LINE1,
		},
	},
	[SAA7134_BOARD_PINNACLE_PCTV_310i] = {
		.name           = "Pinnacle PCTV 310i",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr     = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.tuner_config   = 1,
		.mpeg           = SAA7134_MPEG_DVB,
		.gpiomask       = 0x000200000,
		.inputs         = {{
			.name = name_tv,
			.vmux = 4,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 1,
			.amux = LINE2,
		},{
			.name = name_comp2,
			.vmux = 0,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
		}},
		.radio = {
			.name = name_radio,
			.amux   = TV,
			.gpio   = 0x0200000,
		},
	},
	[SAA7134_BOARD_AVERMEDIA_STUDIO_507] = {
		/* Mikhail Fedotov <mo_fedotov@mail.ru> */
		.name           = "Avermedia AVerTV Studio 507",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_FM1256_IH3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.gpiomask       = 0x03,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
			.gpio = 0x00,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE2,
			.gpio = 0x00,
		},{
			.name = name_comp2,
			.vmux = 3,
			.amux = LINE2,
			.gpio = 0x00,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
			.gpio = 0x00,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
			.gpio = 0x01,
		},
		.mute  = {
			.name = name_mute,
			.amux = LINE1,
			.gpio = 0x00,
		},
	},
	[SAA7134_BOARD_VIDEOMATE_DVBT_200A] = {
		/* Francis Barber <fedora@barber-family.id.au> */
		.name           = "Compro Videomate DVB-T200A",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT | TDA9887_PORT1_ACTIVE,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs = {{
			.name   = name_tv,
			.vmux   = 3,
			.amux   = TV,
			.tv     = 1,
		},{
			.name   = name_comp1,
			.vmux   = 1,
			.amux   = LINE2,
		},{
			.name   = name_svideo,
			.vmux   = 8,
			.amux   = LINE2,
		}},
	},
	[SAA7134_BOARD_HAUPPAUGE_HVR1110] = {
		/* Thomas Genty <tomlohave@gmail.com> */
		.name           = "Hauppauge WinTV-HVR1110 DVB-T/Hybrid",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr     = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name   = name_comp1,
			.vmux   = 3,
			.amux   = LINE2, /* FIXME: audio doesn't work on svideo/composite */
		},{
			.name   = name_svideo,
			.vmux   = 8,
			.amux   = LINE2, /* FIXME: audio doesn't work on svideo/composite */
		}},
		.radio = {
			.name = name_radio,
			.amux   = TV,
		},
	},
	[SAA7134_BOARD_CINERGY_HT_PCMCIA] = {
		.name           = "Terratec Cinergy HT PCMCIA",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs = {{
			.name   = name_tv,
			.vmux   = 1,
			.amux   = TV,
			.tv     = 1,
		},{
			.name   = name_comp1,
			.vmux   = 0,
			.amux   = LINE1,
		},{
			.name   = name_svideo,
			.vmux   = 6,
			.amux   = LINE1,
		}},
	},
	[SAA7134_BOARD_ENCORE_ENLTV] = {
	/* Steven Walter <stevenrwalter@gmail.com>
	   Juan Pablo Sormani <sorman@gmail.com> */
		.name           = "Encore ENLTV",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_TNF_5335MF,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = 3,
			.tv   = 1,
		},{
			.name = name_tv_mono,
			.vmux = 7,
			.amux = 4,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = 2,
		},{
			.name = name_svideo,
			.vmux = 0,
			.amux = 2,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
/*			.gpio = 0x00300001,*/
			.gpio = 0x20000,

		},
		.mute = {
			.name = name_mute,
			.amux = 0,
		},
	},
	[SAA7134_BOARD_ENCORE_ENLTV_FM] = {
  /*	Juan Pablo Sormani <sorman@gmail.com> */
		.name           = "Encore ENLTV-FM",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_PHILIPS_ATSC,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = 3,
			.tv   = 1,
		},{
			.name = name_tv_mono,
			.vmux = 7,
			.amux = 4,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = 2,
		},{
			.name = name_svideo,
			.vmux = 0,
			.amux = 2,
		}},
		.radio = {
			.name = name_radio,
			.amux = LINE2,
			.gpio = 0x20000,

		},
		.mute = {
			.name = name_mute,
			.amux = 0,
		},
	},
	[SAA7134_BOARD_CINERGY_HT_PCI] = {
		.name           = "Terratec Cinergy HT PCI",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs = {{
			.name   = name_tv,
			.vmux   = 1,
			.amux   = TV,
			.tv     = 1,
		},{
			.name   = name_comp1,
			.vmux   = 0,
			.amux   = LINE1,
		},{
			.name   = name_svideo,
			.vmux   = 6,
			.amux   = LINE1,
		}},
	},
	[SAA7134_BOARD_PHILIPS_TIGER_S] = {
		.name           = "Philips Tiger - S Reference design",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tuner_config   = 2,
		.mpeg           = SAA7134_MPEG_DVB,
		.gpiomask       = 0x0200000,
		.inputs = {{
			.name   = name_tv,
			.vmux   = 1,
			.amux   = TV,
			.tv     = 1,
		},{
			.name   = name_comp1,
			.vmux   = 3,
			.amux   = LINE1,
		},{
			.name   = name_svideo,
			.vmux   = 8,
			.amux   = LINE1,
		}},
		.radio = {
			.name   = name_radio,
			.amux   = TV,
			.gpio   = 0x0200000,
		},
	},
	[SAA7134_BOARD_AVERMEDIA_M102] = {
		.name           = "Avermedia M102",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.gpiomask       = 1<<21,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE2,
		},{
			.name = name_svideo,
			.vmux = 6,
			.amux = LINE2,
		}},
	},
	[SAA7134_BOARD_ASUS_P7131_4871] = {
		.name           = "ASUS P7131 4871",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tuner_config   = 2,
		.mpeg           = SAA7134_MPEG_DVB,
		.gpiomask       = 0x0200000,
		.inputs = {{
			.name   = name_tv,
			.vmux   = 1,
			.amux   = TV,
			.tv     = 1,
			.gpio   = 0x0200000,
		}},
	},
	[SAA7134_BOARD_ASUSTeK_P7131_HYBRID_LNA] = {
		.name           = "ASUSTeK P7131 Hybrid",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tuner_config   = 2,
		.gpiomask	= 1 << 21,
		.mpeg           = SAA7134_MPEG_DVB,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
			.gpio = 0x0000000,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE2,
			.gpio = 0x0200000,
		},{
			.name = name_comp2,
			.vmux = 0,
			.amux = LINE2,
			.gpio = 0x0200000,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE2,
			.gpio = 0x0200000,
		}},
		.radio = {
			.name = name_radio,
			.amux = TV,
			.gpio = 0x0200000,
		},
	},
	[SAA7134_BOARD_SABRENT_TV_PCB05] = {
		.name           = "Sabrent PCMCIA TV-PCB05",
		.audio_clock    = 0x00187de7,
		.tuner_type     = TUNER_PHILIPS_TDA8290,
		.radio_type     = UNSET,
		.tuner_addr     = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = TV,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE1,
		},{
			.name = name_comp2,
			.vmux = 0,
			.amux = LINE1,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
		}},
		.mute = {
			.name = name_mute,
			.amux = TV,
		},
	},
	[SAA7134_BOARD_10MOONSTVMASTER3] = {
		/* Tony Wan <aloha_cn@hotmail.com> */
		.name           = "10MOONS TM300 TV Card",
		.audio_clock    = 0x00200000,
		.tuner_type     = TUNER_LG_PAL_NEW_TAPC,
		.radio_type     = UNSET,
		.tuner_addr     = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.gpiomask       = 0x7000,
		.inputs         = {{
			.name = name_tv,
			.vmux = 1,
			.amux = LINE2,
			.gpio = 0x0000,
			.tv   = 1,
		},{
			.name = name_comp1,
			.vmux = 3,
			.amux = LINE1,
			.gpio = 0x2000,
		},{
			.name = name_svideo,
			.vmux = 8,
			.amux = LINE1,
			.gpio = 0x2000,
		}},
		.mute = {
			.name = name_mute,
			.amux = LINE2,
			.gpio = 0x3000,
		},
	},
};

const unsigned int saa7134_bcount = ARRAY_SIZE(saa7134_boards);

/* ------------------------------------------------------------------ */
/* PCI ids + subsystem IDs                                            */

struct pci_device_id saa7134_pci_tbl[] = {
	{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = PCI_VENDOR_ID_PHILIPS,
		.subdevice    = 0x2001,
		.driver_data  = SAA7134_BOARD_PROTEUS_PRO,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = PCI_VENDOR_ID_PHILIPS,
		.subdevice    = 0x2001,
		.driver_data  = SAA7134_BOARD_PROTEUS_PRO,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = PCI_VENDOR_ID_PHILIPS,
		.subdevice    = 0x6752,
		.driver_data  = SAA7134_BOARD_EMPRESS,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x1131,
		.subdevice    = 0x4e85,
		.driver_data  = SAA7134_BOARD_MONSTERTV,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x153b,
		.subdevice    = 0x1142,
		.driver_data  = SAA7134_BOARD_CINERGY400,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x153b,
		.subdevice    = 0x1143,
		.driver_data  = SAA7134_BOARD_CINERGY600,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x153b,
		.subdevice    = 0x1158,
		.driver_data  = SAA7134_BOARD_CINERGY600_MK3,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x153b,
		.subdevice    = 0x1162,
		.driver_data  = SAA7134_BOARD_CINERGY400_CARDBUS,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x5169,
		.subdevice    = 0x0138,
		.driver_data  = SAA7134_BOARD_FLYVIDEO3000_NTSC,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x5168,
		.subdevice    = 0x0138,
		.driver_data  = SAA7134_BOARD_FLYVIDEO3000,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x4e42,				/* "Typhoon PCI Capture TV Card" Art.No. 50673 */
		.subdevice    = 0x0138,
		.driver_data  = SAA7134_BOARD_FLYVIDEO3000,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = 0x5168,
		.subdevice    = 0x0138,
		.driver_data  = SAA7134_BOARD_FLYVIDEO2000,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = 0x4e42,		/* Typhoon */
		.subdevice    = 0x0138,		/* LifeView FlyTV Prime30 OEM */
		.driver_data  = SAA7134_BOARD_FLYVIDEO2000,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x5168,
		.subdevice    = 0x0212, /* minipci, LR212 */
		.driver_data  = SAA7134_BOARD_FLYTVPLATINUM_MINI,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x14c0,
		.subdevice    = 0x1212, /* minipci, LR1212 */
		.driver_data  = SAA7134_BOARD_FLYTVPLATINUM_MINI2,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x4e42,
		.subdevice    = 0x0212, /* OEM minipci, LR212 */
		.driver_data  = SAA7134_BOARD_FLYTVPLATINUM_MINI,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x5168,	/* Animation Technologies (LifeView) */
		.subdevice    = 0x0214, /* Standard PCI, LR214 Rev E and earlier (SAA7135) */
		.driver_data  = SAA7134_BOARD_FLYTVPLATINUM_FM,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x5168,	/* Animation Technologies (LifeView) */
		.subdevice    = 0x5214, /* Standard PCI, LR214 Rev F onwards (SAA7131) */
		.driver_data  = SAA7134_BOARD_FLYTVPLATINUM_FM,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1489, /* KYE */
		.subdevice    = 0x0214, /* Genius VideoWonder ProTV */
		.driver_data  = SAA7134_BOARD_FLYTVPLATINUM_FM, /* is an LR214WF actually */
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x16be,
		.subdevice    = 0x0003,
		.driver_data  = SAA7134_BOARD_MD7134,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = 0x1048,
		.subdevice    = 0x226b,
		.driver_data  = SAA7134_BOARD_ELSA,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = 0x1048,
		.subdevice    = 0x226a,
		.driver_data  = SAA7134_BOARD_ELSA_500TV,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = 0x1048,
		.subdevice    = 0x226c,
		.driver_data  = SAA7134_BOARD_ELSA_700TV,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = PCI_VENDOR_ID_ASUSTEK,
		.subdevice    = 0x4842,
		.driver_data  = SAA7134_BOARD_ASUSTeK_TVFM7134,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = PCI_VENDOR_ID_ASUSTEK,
		.subdevice    = 0x4845,
		.driver_data  = SAA7134_BOARD_ASUSTeK_TVFM7135,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = PCI_VENDOR_ID_ASUSTEK,
		.subdevice    = 0x4830,
		.driver_data  = SAA7134_BOARD_ASUSTeK_TVFM7134,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = PCI_VENDOR_ID_ASUSTEK,
		.subdevice    = 0x4843,
		.driver_data  = SAA7134_BOARD_ASUSTEK_TVFM7133,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = PCI_VENDOR_ID_ASUSTEK,
		.subdevice    = 0x4840,
		.driver_data  = SAA7134_BOARD_ASUSTeK_TVFM7134,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = PCI_VENDOR_ID_PHILIPS,
		.subdevice    = 0xfe01,
		.driver_data  = SAA7134_BOARD_TVSTATION_RDS,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x1894,
		.subdevice    = 0xfe01,
		.driver_data  = SAA7134_BOARD_TVSTATION_RDS,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x1894,
		.subdevice    = 0xa006,
		.driver_data  = SAA7134_BOARD_TVSTATION_DVR,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x1131,
		.subdevice    = 0x7133,
		.driver_data  = SAA7134_BOARD_VA1000POWER,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = PCI_VENDOR_ID_PHILIPS,
		.subdevice    = 0x2001,
		.driver_data  = SAA7134_BOARD_10MOONSTVMASTER,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x185b,
		.subdevice    = 0xc100,
		.driver_data  = SAA7134_BOARD_VIDEOMATE_TV,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x185b,
		.subdevice    = 0xc100,
		.driver_data  = SAA7134_BOARD_VIDEOMATE_TV_GOLD_PLUS,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = PCI_VENDOR_ID_MATROX,
		.subdevice    = 0x48d0,
		.driver_data  = SAA7134_BOARD_CRONOS_PLUS,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x1461, /* Avermedia Technologies Inc */
		.subdevice    = 0xa70b,
		.driver_data  = SAA7134_BOARD_MD2819,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = 0x1461, /* Avermedia Technologies Inc */
		.subdevice    = 0x2115,
		.driver_data  = SAA7134_BOARD_AVERMEDIA_STUDIO_305,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = 0x1461, /* Avermedia Technologies Inc */
		.subdevice    = 0x2108,
		.driver_data  = SAA7134_BOARD_AVERMEDIA_305,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = 0x1461, /* Avermedia Technologies Inc */
		.subdevice    = 0x10ff,
		.driver_data  = SAA7134_BOARD_AVERMEDIA_DVD_EZMAKER,
	},{
		/* AVerMedia CardBus */
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x1461, /* Avermedia Technologies Inc */
		.subdevice    = 0xd6ee,
		.driver_data  = SAA7134_BOARD_AVERMEDIA_CARDBUS,
	},{
		/* TransGear 3000TV */
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = 0x1461, /* Avermedia Technologies Inc */
		.subdevice    = 0x050c,
		.driver_data  = SAA7134_BOARD_TG3000TV,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x11bd,
		.subdevice    = 0x002b,
		.driver_data  = SAA7134_BOARD_PINNACLE_PCTV_STEREO,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x11bd,
		.subdevice    = 0x002d,
		.driver_data  = SAA7134_BOARD_PINNACLE_300I_DVBT_PAL,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x1019,
		.subdevice    = 0x4cb4,
		.driver_data  = SAA7134_BOARD_ECS_TVP3XP,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1019,
		.subdevice    = 0x4cb5,
		.driver_data  = SAA7134_BOARD_ECS_TVP3XP_4CB5,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x1019,
		.subdevice    = 0x4cb6,
		.driver_data  = SAA7134_BOARD_ECS_TVP3XP_4CB6,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x12ab,
		.subdevice    = 0x0800,
		.driver_data  = SAA7134_BOARD_UPMOST_PURPLE_TV,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = 0x153b,
		.subdevice    = 0x1152,
		.driver_data  = SAA7134_BOARD_CINERGY200,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = 0x185b,
		.subdevice    = 0xc100,
		.driver_data  = SAA7134_BOARD_VIDEOMATE_TV_PVR,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x1461, /* Avermedia Technologies Inc */
		.subdevice    = 0x9715,
		.driver_data  = SAA7134_BOARD_AVERMEDIA_STUDIO_307,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x1461, /* Avermedia Technologies Inc */
		.subdevice    = 0xa70a,
		.driver_data  = SAA7134_BOARD_AVERMEDIA_307,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x185b,
		.subdevice    = 0xc200,
		.driver_data  = SAA7134_BOARD_VIDEOMATE_GOLD_PLUS,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x1540,
		.subdevice    = 0x9524,
		.driver_data  = SAA7134_BOARD_PROVIDEO_PV952,

	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x5168,
		.subdevice    = 0x0502,                /* Cardbus version */
		.driver_data  = SAA7134_BOARD_FLYDVBT_DUO_CARDBUS,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x5168,
		.subdevice    = 0x0306,                /* PCI version */
		.driver_data  = SAA7134_BOARD_FLYDVBTDUO,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1461, /* Avermedia Technologies Inc */
		.subdevice    = 0xf31f,
		.driver_data  = SAA7134_BOARD_AVERMEDIA_GO_007_FM,

	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = PCI_VENDOR_ID_PHILIPS,
		.subdevice    = 0x2004,
		.driver_data  = SAA7134_BOARD_PHILIPS_TOUGH,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1421,
		.subdevice    = 0x0350,		/* PCI version */
		.driver_data  = SAA7134_BOARD_ADS_INSTANT_TV,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1421,
		.subdevice    = 0x0351,		/* PCI version, new revision */
		.driver_data  = SAA7134_BOARD_ADS_INSTANT_TV,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1421,
		.subdevice    = 0x0370,		/* cardbus version */
		.driver_data  = SAA7134_BOARD_ADS_INSTANT_TV,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1421,
		.subdevice    = 0x1370,        /* cardbus version */
		.driver_data  = SAA7134_BOARD_ADS_INSTANT_TV,

	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x4e42,		/* Typhoon */
		.subdevice    = 0x0502,		/* LifeView LR502 OEM */
		.driver_data  = SAA7134_BOARD_FLYDVBT_DUO_CARDBUS,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1043,
		.subdevice    = 0x0210,		/* mini pci NTSC version */
		.driver_data  = SAA7134_BOARD_FLYTV_DIGIMATRIX,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x1043,
		.subdevice    = 0x0210,		/* mini pci PAL/SECAM version */
		.driver_data  = SAA7134_BOARD_ASUSTEK_DIGIMATRIX_TV,

	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x0000, /* It shouldn't break anything, since subdevice id seems unique */
		.subdevice    = 0x4091,
		.driver_data  = SAA7134_BOARD_BEHOLD_409FM,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x5456, /* GoTView */
		.subdevice    = 0x7135,
		.driver_data  = SAA7134_BOARD_GOTVIEW_7135,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = PCI_VENDOR_ID_PHILIPS,
		.subdevice    = 0x2004,
		.driver_data  = SAA7134_BOARD_PHILIPS_EUROPA,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x185b,
		.subdevice    = 0xc900,
		.driver_data  = SAA7134_BOARD_VIDEOMATE_DVBT_300,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = 0x185b,
		.subdevice    = 0xc901,
		.driver_data  = SAA7134_BOARD_VIDEOMATE_DVBT_200,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1435,
		.subdevice    = 0x7350,
		.driver_data  = SAA7134_BOARD_RTD_VFG7350,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1435,
		.subdevice    = 0x7330,
		.driver_data  = SAA7134_BOARD_RTD_VFG7330,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1461,
		.subdevice    = 0x1044,
		.driver_data  = SAA7134_BOARD_AVERMEDIA_AVERTVHD_A180,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1131,
		.subdevice    = 0x4ee9,
		.driver_data  = SAA7134_BOARD_MONSTERTV_MOBILE,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x11bd,
		.subdevice    = 0x002e,
		.driver_data  = SAA7134_BOARD_PINNACLE_PCTV_110i,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1043,
		.subdevice    = 0x4862,
		.driver_data  = SAA7134_BOARD_ASUSTeK_P7131_DUAL,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = PCI_VENDOR_ID_PHILIPS,
		.subdevice    = 0x2018,
		.driver_data  = SAA7134_BOARD_PHILIPS_TIGER,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1462,
		.subdevice    = 0x6231,
		.driver_data  = SAA7134_BOARD_MSI_TVATANYWHERE_PLUS,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x153b,
		.subdevice    = 0x1160,
		.driver_data  = SAA7134_BOARD_CINERGY250PCI,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,	/* SAA 7131E */
		.subvendor    = 0x5168,
		.subdevice    = 0x0319,
		.driver_data  = SAA7134_BOARD_FLYDVB_TRIO,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x1461,
		.subdevice    = 0x2c05,
		.driver_data  = SAA7134_BOARD_AVERMEDIA_777,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x5168,
		.subdevice    = 0x0301,
		.driver_data  = SAA7134_BOARD_FLYDVBT_LR301,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x0331,
		.subdevice    = 0x1421,
		.driver_data  = SAA7134_BOARD_ADS_DUO_CARDBUS_PTV331,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x17de,
		.subdevice    = 0x7201,
		.driver_data  = SAA7134_BOARD_TEVION_DVBT_220RF,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x17de,
		.subdevice    = 0x7250,
		.driver_data  = SAA7134_BOARD_KWORLD_DVBT_210,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133, /* SAA7135HL */
		.subvendor    = 0x17de,
		.subdevice    = 0x7350,
		.driver_data  = SAA7134_BOARD_KWORLD_ATSC110,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x1461,
		.subdevice    = 0x7360,
		.driver_data  = SAA7134_BOARD_AVERMEDIA_A169_B,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x1461,
		.subdevice    = 0x6360,
		.driver_data  = SAA7134_BOARD_AVERMEDIA_A169_B1,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x16be,
		.subdevice    = 0x0005,
		.driver_data  = SAA7134_BOARD_MD7134_BRIDGE_2,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x5168,
		.subdevice    = 0x0300,
		.driver_data  = SAA7134_BOARD_FLYDVBS_LR300,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x4e42,
		.subdevice    = 0x0300,/* LR300 */
		.driver_data  = SAA7134_BOARD_FLYDVBS_LR300,
	},{
		.vendor = PCI_VENDOR_ID_PHILIPS,
		.device = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor = 0x1489,
		.subdevice = 0x0301,
		.driver_data = SAA7134_BOARD_FLYDVBT_LR301,
	},{
		.vendor = PCI_VENDOR_ID_PHILIPS,
		.device = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor = 0x5168, /* Animation Technologies (LifeView) */
		.subdevice = 0x0304,
		.driver_data = SAA7134_BOARD_FLYTVPLATINUM_FM,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x5168,
		.subdevice    = 0x3306,
		.driver_data  = SAA7134_BOARD_FLYDVBT_HYBRID_CARDBUS,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x5168,
		.subdevice    = 0x3502,  /* whats the difference to 0x3306 ?*/
		.driver_data  = SAA7134_BOARD_FLYDVBT_HYBRID_CARDBUS,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x16be,
		.subdevice    = 0x0007,
		.driver_data  = SAA7134_BOARD_MEDION_MD8800_QUADRO,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x16be,
		.subdevice    = 0x0008,
		.driver_data  = SAA7134_BOARD_MEDION_MD8800_QUADRO,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1461,
		.subdevice    = 0x2c05,
		.driver_data  = SAA7134_BOARD_AVERMEDIA_777,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1489,
		.subdevice    = 0x0502,                /* Cardbus version */
		.driver_data  = SAA7134_BOARD_FLYDVBT_DUO_CARDBUS,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = 0x0919, /* Philips Proteus PRO 2309 */
		.subdevice    = 0x2003,
		.driver_data  = SAA7134_BOARD_PROTEUS_2309,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x1461,
		.subdevice    = 0x2c00,
		.driver_data  = SAA7134_BOARD_AVERMEDIA_A16AR,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x1043,
		.subdevice    = 0x4860,
		.driver_data  = SAA7134_BOARD_ASUS_EUROPA2_HYBRID,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x11bd,
		.subdevice    = 0x002f,
		.driver_data  = SAA7134_BOARD_PINNACLE_PCTV_310i,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1461, /* Avermedia Technologies Inc */
		.subdevice    = 0x9715,
		.driver_data  = SAA7134_BOARD_AVERMEDIA_STUDIO_507,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1043,
		.subdevice    = 0x4876,
		.driver_data  = SAA7134_BOARD_ASUSTeK_P7131_HYBRID_LNA,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x0070,
		.subdevice    = 0x6701,
		.driver_data  = SAA7134_BOARD_HAUPPAUGE_HVR1110,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x153b,
		.subdevice    = 0x1172,
		.driver_data  = SAA7134_BOARD_CINERGY_HT_PCMCIA,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = PCI_VENDOR_ID_PHILIPS,
		.subdevice    = 0x2342,
		.driver_data  = SAA7134_BOARD_ENCORE_ENLTV,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = 0x1131,
		.subdevice    = 0x2341,
		.driver_data  = SAA7134_BOARD_ENCORE_ENLTV,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = 0x3016,
		.subdevice    = 0x2344,
		.driver_data  = SAA7134_BOARD_ENCORE_ENLTV,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = 0x1131,
		.subdevice    = 0x230f,
		.driver_data  = SAA7134_BOARD_ENCORE_ENLTV_FM,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x153b,
		.subdevice    = 0x1175,
		.driver_data  = SAA7134_BOARD_CINERGY_HT_PCI,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1461, /* Avermedia Technologies Inc */
		.subdevice    = 0xf31e,
		.driver_data  = SAA7134_BOARD_AVERMEDIA_M102,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x4E42,         /* MSI */
		.subdevice    = 0x0306,         /* TV@nywhere DUO */
		.driver_data  = SAA7134_BOARD_FLYDVBTDUO,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1043,
		.subdevice    = 0x4871,
		.driver_data  = SAA7134_BOARD_ASUS_P7131_4871,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = 0x1043,
		.subdevice    = 0x4857,
		.driver_data  = SAA7134_BOARD_ASUSTeK_P7131_DUAL,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = 0x0919, /* SinoVideo PCI 2309 Proteus (7134) */
		.subdevice    = 0x2003, /* OEM cardbus */
		.driver_data  = SAA7134_BOARD_SABRENT_TV_PCB05,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = PCI_VENDOR_ID_PHILIPS,
		.subdevice    = 0x2304,
		.driver_data  = SAA7134_BOARD_10MOONSTVMASTER3,
	},{
		/* --- boards without eeprom + subsystem ID --- */
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = PCI_VENDOR_ID_PHILIPS,
		.subdevice    = 0,
		.driver_data  = SAA7134_BOARD_NOAUTO,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = PCI_VENDOR_ID_PHILIPS,
		.subdevice    = 0,
		.driver_data  = SAA7134_BOARD_NOAUTO,
	},{
		/* --- default catch --- */
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7130,
		.subvendor    = PCI_ANY_ID,
		.subdevice    = PCI_ANY_ID,
		.driver_data  = SAA7134_BOARD_UNKNOWN,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7133,
		.subvendor    = PCI_ANY_ID,
		.subdevice    = PCI_ANY_ID,
		.driver_data  = SAA7134_BOARD_UNKNOWN,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7134,
		.subvendor    = PCI_ANY_ID,
		.subdevice    = PCI_ANY_ID,
		.driver_data  = SAA7134_BOARD_UNKNOWN,
	},{
		.vendor       = PCI_VENDOR_ID_PHILIPS,
		.device       = PCI_DEVICE_ID_PHILIPS_SAA7135,
		.subvendor    = PCI_ANY_ID,
		.subdevice    = PCI_ANY_ID,
		.driver_data  = SAA7134_BOARD_UNKNOWN,
	},{
		/* --- end of list --- */
	}
};
MODULE_DEVICE_TABLE(pci, saa7134_pci_tbl);

/* ----------------------------------------------------------- */
/* flyvideo tweaks                                             */


static void board_flyvideo(struct saa7134_dev *dev)
{
	printk("%s: there are different flyvideo cards with different tuners\n"
	       "%s: out there, you might have to use the tuner=<nr> insmod\n"
	       "%s: option to override the default value.\n",
	       dev->name, dev->name, dev->name);
}

/* ----------------------------------------------------------- */

int saa7134_board_init1(struct saa7134_dev *dev)
{
	/* Always print gpio, often manufacturers encode tuner type and other info. */
	saa_writel(SAA7134_GPIO_GPMODE0 >> 2, 0);
	dev->gpio_value = saa_readl(SAA7134_GPIO_GPSTATUS0 >> 2);
	printk(KERN_INFO "%s: board init: gpio is %x\n", dev->name, dev->gpio_value);

	switch (dev->board) {
	case SAA7134_BOARD_FLYVIDEO2000:
	case SAA7134_BOARD_FLYVIDEO3000:
	case SAA7134_BOARD_FLYVIDEO3000_NTSC:
		dev->has_remote = SAA7134_REMOTE_GPIO;
		board_flyvideo(dev);
		break;
	case SAA7134_BOARD_FLYTVPLATINUM_MINI2:
	case SAA7134_BOARD_FLYTVPLATINUM_FM:
	case SAA7134_BOARD_CINERGY400:
	case SAA7134_BOARD_CINERGY600:
	case SAA7134_BOARD_CINERGY600_MK3:
	case SAA7134_BOARD_ECS_TVP3XP:
	case SAA7134_BOARD_ECS_TVP3XP_4CB5:
	case SAA7134_BOARD_ECS_TVP3XP_4CB6:
	case SAA7134_BOARD_MD2819:
	case SAA7134_BOARD_KWORLD_VSTREAM_XPERT:
	case SAA7134_BOARD_KWORLD_XPERT:
	case SAA7134_BOARD_AVERMEDIA_STUDIO_305:
	case SAA7134_BOARD_AVERMEDIA_305:
	case SAA7134_BOARD_AVERMEDIA_STUDIO_307:
	case SAA7134_BOARD_AVERMEDIA_307:
	case SAA7134_BOARD_AVERMEDIA_STUDIO_507:
	case SAA7134_BOARD_AVERMEDIA_GO_007_FM:
	case SAA7134_BOARD_AVERMEDIA_777:
/*      case SAA7134_BOARD_SABRENT_SBTTVFM:  */ /* not finished yet */
	case SAA7134_BOARD_VIDEOMATE_TV_PVR:
	case SAA7134_BOARD_VIDEOMATE_GOLD_PLUS:
	case SAA7134_BOARD_VIDEOMATE_TV_GOLD_PLUSII:
	case SAA7134_BOARD_VIDEOMATE_DVBT_300:
	case SAA7134_BOARD_VIDEOMATE_DVBT_200:
	case SAA7134_BOARD_VIDEOMATE_DVBT_200A:
	case SAA7134_BOARD_MANLI_MTV001:
	case SAA7134_BOARD_MANLI_MTV002:
	case SAA7134_BOARD_BEHOLD_409FM:
	case SAA7134_BOARD_AVACSSMARTTV:
	case SAA7134_BOARD_GOTVIEW_7135:
	case SAA7134_BOARD_KWORLD_TERMINATOR:
	case SAA7134_BOARD_SEDNA_PC_TV_CARDBUS:
	case SAA7134_BOARD_FLYDVBT_LR301:
	case SAA7134_BOARD_ASUSTeK_P7131_DUAL:
	case SAA7134_BOARD_ASUSTeK_P7131_HYBRID_LNA:
	case SAA7134_BOARD_FLYDVBTDUO:
	case SAA7134_BOARD_PROTEUS_2309:
	case SAA7134_BOARD_AVERMEDIA_A16AR:
	case SAA7134_BOARD_ENCORE_ENLTV:
	case SAA7134_BOARD_ENCORE_ENLTV_FM:
	case SAA7134_BOARD_10MOONSTVMASTER3:
		dev->has_remote = SAA7134_REMOTE_GPIO;
		break;
	case SAA7134_BOARD_FLYDVBS_LR300:
		saa_writeb(SAA7134_GPIO_GPMODE3, 0x80);
		saa_writeb(SAA7134_GPIO_GPSTATUS2, 0x40);
		dev->has_remote = SAA7134_REMOTE_GPIO;
		break;
	case SAA7134_BOARD_MD5044:
		printk("%s: seems there are two different versions of the MD5044\n"
		       "%s: (with the same ID) out there.  If sound doesn't work for\n"
		       "%s: you try the audio_clock_override=0x200000 insmod option.\n",
		       dev->name,dev->name,dev->name);
		break;
	case SAA7134_BOARD_CINERGY400_CARDBUS:
		/* power-up tuner chip */
		saa_andorl(SAA7134_GPIO_GPMODE0 >> 2,   0x00040000, 0x00040000);
		saa_andorl(SAA7134_GPIO_GPSTATUS0 >> 2, 0x00040000, 0x00000000);
		break;
	case SAA7134_BOARD_PINNACLE_300I_DVBT_PAL:
		/* this turns the remote control chip off to work around a bug in it */
		saa_writeb(SAA7134_GPIO_GPMODE1, 0x80);
		saa_writeb(SAA7134_GPIO_GPSTATUS1, 0x80);
		break;
	case SAA7134_BOARD_MONSTERTV_MOBILE:
		/* power-up tuner chip */
		saa_andorl(SAA7134_GPIO_GPMODE0 >> 2,   0x00040000, 0x00040000);
		saa_andorl(SAA7134_GPIO_GPSTATUS0 >> 2, 0x00040000, 0x00000004);
		break;
	case SAA7134_BOARD_FLYDVBT_DUO_CARDBUS:
		/* turn the fan on */
		saa_writeb(SAA7134_GPIO_GPMODE3, 0x08);
		saa_writeb(SAA7134_GPIO_GPSTATUS3, 0x06);
		break;
	case SAA7134_BOARD_ADS_DUO_CARDBUS_PTV331:
	case SAA7134_BOARD_FLYDVBT_HYBRID_CARDBUS:
		saa_andorl(SAA7134_GPIO_GPMODE0 >> 2, 0x08000000, 0x08000000);
		saa_andorl(SAA7134_GPIO_GPSTATUS0 >> 2, 0x08000000, 0x00000000);
		break;
	case SAA7134_BOARD_AVERMEDIA_CARDBUS:
		/* power-up tuner chip */
		saa_andorl(SAA7134_GPIO_GPMODE0 >> 2,   0xffffffff, 0xffffffff);
		saa_andorl(SAA7134_GPIO_GPSTATUS0 >> 2, 0xffffffff, 0xffffffff);
		msleep(1);
		break;
	case SAA7134_BOARD_RTD_VFG7350:

		/*
		 * Make sure Production Test Register at offset 0x1D1 is cleared
		 * to take chip out of test mode.  Clearing bit 4 (TST_EN_AOUT)
		 * prevents pin 105 from remaining low; keeping pin 105 low
		 * continually resets the SAA6752 chip.
		 */

		saa_writeb (SAA7134_PRODUCTION_TEST_MODE, 0x00);
		break;
	/* i2c remotes */
	case SAA7134_BOARD_PINNACLE_PCTV_110i:
	case SAA7134_BOARD_PINNACLE_PCTV_310i:
	case SAA7134_BOARD_UPMOST_PURPLE_TV:
	case SAA7134_BOARD_HAUPPAUGE_HVR1110:
		dev->has_remote = SAA7134_REMOTE_I2C;
		break;
	case SAA7134_BOARD_AVERMEDIA_A169_B:
	case SAA7134_BOARD_MD7134_BRIDGE_2:
		printk("%s: %s: dual saa713x broadcast decoders\n"
		       "%s: Sorry, none of the inputs to this chip are supported yet.\n"
		       "%s: Dual decoder functionality is disabled for now, use the other chip.\n",
		       dev->name,card(dev).name,dev->name,dev->name);
		break;
	case SAA7134_BOARD_AVERMEDIA_M102:
		/* enable tuner */
		saa_andorl(SAA7134_GPIO_GPMODE0 >> 2,   0x8c040007, 0x8c040007);
		saa_andorl(SAA7134_GPIO_GPSTATUS0 >> 2, 0x0c0007cd, 0x0c0007cd);
		break;
	}
	return 0;
}

/* stuff which needs working i2c */
int saa7134_board_init2(struct saa7134_dev *dev)
{
	unsigned char buf;
	int board;
	struct tuner_setup tun_setup;
	tun_setup.config = 0;
	tun_setup.tuner_callback = saa7134_tuner_callback;

	switch (dev->board) {
	case SAA7134_BOARD_BMK_MPEX_NOTUNER:
	case SAA7134_BOARD_BMK_MPEX_TUNER:
		dev->i2c_client.addr = 0x60;
		board = (i2c_master_recv(&dev->i2c_client,&buf,0) < 0)
			? SAA7134_BOARD_BMK_MPEX_NOTUNER
			: SAA7134_BOARD_BMK_MPEX_TUNER;
		if (board == dev->board)
			break;
		dev->board = board;
		printk("%s: board type fixup: %s\n", dev->name,
		saa7134_boards[dev->board].name);
		dev->tuner_type = saa7134_boards[dev->board].tuner_type;

		if (TUNER_ABSENT != dev->tuner_type) {
				tun_setup.mode_mask = T_RADIO | T_ANALOG_TV | T_DIGITAL_TV;
				tun_setup.type = dev->tuner_type;
				tun_setup.addr = ADDR_UNSET;

				saa7134_i2c_call_clients (dev, TUNER_SET_TYPE_ADDR, &tun_setup);
		}
		break;
	case SAA7134_BOARD_MD7134:
		{
		u8 subaddr;
		u8 data[3];
		int ret, tuner_t;

		struct i2c_msg msg[] = {{.addr=0x50, .flags=0, .buf=&subaddr, .len = 1},
					{.addr=0x50, .flags=I2C_M_RD, .buf=data, .len = 3}};
		subaddr= 0x14;
		tuner_t = 0;
		ret = i2c_transfer(&dev->i2c_adap, msg, 2);
		if (ret != 2) {
			printk(KERN_ERR "EEPROM read failure\n");
		} else if ((data[0] != 0) && (data[0] != 0xff)) {
			/* old config structure */
			subaddr = data[0] + 2;
			msg[1].len = 2;
			i2c_transfer(&dev->i2c_adap, msg, 2);
			tuner_t = (data[0] << 8) + data[1];
			switch (tuner_t){
			case 0x0103:
				dev->tuner_type = TUNER_PHILIPS_PAL;
				break;
			case 0x010C:
				dev->tuner_type = TUNER_PHILIPS_FM1216ME_MK3;
				break;
			default:
				printk(KERN_ERR "%s Cant determine tuner type %x from EEPROM\n", dev->name, tuner_t);
			}
		} else if ((data[1] != 0) && (data[1] != 0xff)) {
			/* new config structure */
			subaddr = data[1] + 1;
			msg[1].len = 1;
			i2c_transfer(&dev->i2c_adap, msg, 2);
			subaddr = data[0] + 1;
			msg[1].len = 2;
			i2c_transfer(&dev->i2c_adap, msg, 2);
			tuner_t = (data[1] << 8) + data[0];
			switch (tuner_t) {
			case 0x0005:
				dev->tuner_type = TUNER_PHILIPS_FM1216ME_MK3;
				break;
			case 0x001d:
				dev->tuner_type = TUNER_PHILIPS_FMD1216ME_MK3;
					printk(KERN_INFO "%s Board has DVB-T\n", dev->name);
				break;
			default:
				printk(KERN_ERR "%s Cant determine tuner type %x from EEPROM\n", dev->name, tuner_t);
			}
		} else {
			printk(KERN_ERR "%s unexpected config structure\n", dev->name);
		}

		printk(KERN_INFO "%s Tuner type is %d\n", dev->name, dev->tuner_type);
		if (dev->tuner_type == TUNER_PHILIPS_FMD1216ME_MK3) {
			dev->tda9887_conf = TDA9887_PRESENT | TDA9887_PORT1_ACTIVE | TDA9887_PORT2_ACTIVE;
			saa7134_i2c_call_clients(dev,TDA9887_SET_CONFIG, &dev->tda9887_conf);
		}

		tun_setup.mode_mask = T_RADIO | T_ANALOG_TV | T_DIGITAL_TV;
		tun_setup.type = dev->tuner_type;
		tun_setup.addr = ADDR_UNSET;

		saa7134_i2c_call_clients (dev, TUNER_SET_TYPE_ADDR,&tun_setup);
		}
		break;
	case SAA7134_BOARD_PHILIPS_EUROPA:
	case SAA7134_BOARD_VIDEOMATE_DVBT_300:
	case SAA7134_BOARD_ASUS_EUROPA2_HYBRID:
		/* The Philips EUROPA based hybrid boards have the tuner connected through
		 * the channel decoder. We have to make it transparent to find it
		 */
		{
		u8 data[] = { 0x07, 0x02};
		struct i2c_msg msg = {.addr=0x08, .flags=0, .buf=data, .len = sizeof(data)};
		i2c_transfer(&dev->i2c_adap, &msg, 1);

		tun_setup.mode_mask = T_ANALOG_TV | T_DIGITAL_TV;
		tun_setup.type = dev->tuner_type;
		tun_setup.addr = dev->tuner_addr;

		saa7134_i2c_call_clients (dev, TUNER_SET_TYPE_ADDR,&tun_setup);
		}
		break;
	case SAA7134_BOARD_PHILIPS_TIGER:
	case SAA7134_BOARD_PHILIPS_TIGER_S:
		{
		u8 data[] = { 0x3c, 0x33, 0x60};
		struct i2c_msg msg = {.addr=0x08, .flags=0, .buf=data, .len = sizeof(data)};
		if(dev->autodetected && (dev->eedata[0x49] == 0x50)) {
			dev->board = SAA7134_BOARD_PHILIPS_TIGER_S;
			printk(KERN_INFO "%s: Reconfigured board as %s\n",
				dev->name, saa7134_boards[dev->board].name);
		}
		if(dev->board == SAA7134_BOARD_PHILIPS_TIGER_S) {
			tun_setup.mode_mask = T_ANALOG_TV | T_DIGITAL_TV;
			tun_setup.type = TUNER_PHILIPS_TDA8290;
			tun_setup.addr = 0x4b;
			tun_setup.config = 2;

			saa7134_i2c_call_clients (dev, TUNER_SET_TYPE_ADDR,&tun_setup);
			data[2] = 0x68;
		}
		i2c_transfer(&dev->i2c_adap, &msg, 1);
		}
		break;
	case SAA7134_BOARD_PINNACLE_PCTV_310i:
	case SAA7134_BOARD_KWORLD_DVBT_210:
	case SAA7134_BOARD_TEVION_DVBT_220RF:
	case SAA7134_BOARD_ASUSTeK_P7131_DUAL:
	case SAA7134_BOARD_ASUSTeK_P7131_HYBRID_LNA:
	case SAA7134_BOARD_MEDION_MD8800_QUADRO:
	case SAA7134_BOARD_HAUPPAUGE_HVR1110:
		/* this is a hybrid board, initialize to analog mode
		 * and configure firmware eeprom address
		 */
		{
		u8 data[] = { 0x3c, 0x33, 0x60};
		struct i2c_msg msg = {.addr=0x08, .flags=0, .buf=data, .len = sizeof(data)};
		i2c_transfer(&dev->i2c_adap, &msg, 1);
		}
		break;
	case SAA7134_BOARD_FLYDVB_TRIO:
		{
		u8 data[] = { 0x3c, 0x33, 0x62};
		struct i2c_msg msg = {.addr=0x09, .flags=0, .buf=data, .len = sizeof(data)};
		i2c_transfer(&dev->i2c_adap, &msg, 1);
		}
		break;
	case SAA7134_BOARD_ADS_DUO_CARDBUS_PTV331:
	case SAA7134_BOARD_FLYDVBT_HYBRID_CARDBUS:
		/* initialize analog mode  */
		{
		u8 data[] = { 0x3c, 0x33, 0x6a};
		struct i2c_msg msg = {.addr=0x08, .flags=0, .buf=data, .len = sizeof(data)};
		i2c_transfer(&dev->i2c_adap, &msg, 1);
		}
		break;
	case SAA7134_BOARD_CINERGY_HT_PCMCIA:
	case SAA7134_BOARD_CINERGY_HT_PCI:
		/* initialize analog mode */
		{
		u8 data[] = { 0x3c, 0x33, 0x68};
		struct i2c_msg msg = {.addr=0x08, .flags=0, .buf=data, .len = sizeof(data)};
		i2c_transfer(&dev->i2c_adap, &msg, 1);
		}
		break;
	case SAA7134_BOARD_KWORLD_ATSC110:
		{
			/* enable tuner */
			int i;
			static const u8 buffer [] = { 0x10,0x12,0x13,0x04,0x16,0x00,0x14,0x04,0x017,0x00 };
			dev->i2c_client.addr = 0x0a;
			for (i = 0; i < 5; i++)
				if (2 != i2c_master_send(&dev->i2c_client,&buffer[i*2],2))
					printk(KERN_WARNING "%s: Unable to enable tuner(%i).\n",
					       dev->name, i);
		}
		break;
	case SAA7134_BOARD_VIDEOMATE_DVBT_200:
	case SAA7134_BOARD_VIDEOMATE_DVBT_200A:
		/* The T200 and the T200A share the same pci id.  Consequently,
		 * we are going to query eeprom to try to find out which one we
		 * are actually looking at. */

		/* Don't do this if the board was specifically selected with an
		 * insmod option or if we have the default configuration T200*/
		if(!dev->autodetected || (dev->eedata[0x41] == 0xd0))
			break;
		if(dev->eedata[0x41] == 0x02) {
			/* Reconfigure board  as T200A */
			dev->board = SAA7134_BOARD_VIDEOMATE_DVBT_200A;
			dev->tuner_type   = saa7134_boards[dev->board].tuner_type;
			dev->tda9887_conf = saa7134_boards[dev->board].tda9887_conf;
			printk(KERN_INFO "%s: Reconfigured board as %s\n",
				dev->name, saa7134_boards[dev->board].name);
		} else {
			printk(KERN_WARNING "%s: Unexpected tuner type info: %x in eeprom\n",
				dev->name, dev->eedata[0x41]);
			break;
		}
		break;
	}
	return 0;
}

/* ----------------------------------------------------------- */
/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
