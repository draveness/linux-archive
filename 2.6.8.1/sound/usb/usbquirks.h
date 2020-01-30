/*
 * ALSA USB Audio Driver
 *
 * Copyright (c) 2002 by Takashi Iwai <tiwai@suse.de>,
 *                       Clemens Ladisch <clemens@ladisch.de>
 *
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
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

/*
 * The contents of this file are part of the driver's id_table.
 *
 * In a perfect world, this file would be empty.
 */

/*
 * Use this for devices where other interfaces are standard compliant,
 * to prevent the quirk being applied to those interfaces. (To work with
 * hotplugging, bDeviceClass must be set to USB_CLASS_PER_INTERFACE.)
 */
#define USB_DEVICE_VENDOR_SPEC(vend, prod) \
	.match_flags = USB_DEVICE_ID_MATCH_VENDOR | \
		       USB_DEVICE_ID_MATCH_PRODUCT | \
		       USB_DEVICE_ID_MATCH_INT_CLASS, \
	.idVendor = vend, \
	.idProduct = prod, \
	.bInterfaceClass = USB_CLASS_VENDOR_SPEC

/*
 * Yamaha devices
 */

#define YAMAHA_DEVICE(id, name) { \
	USB_DEVICE(0x0499, id), \
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) { \
		.vendor_name = "Yamaha", \
		.product_name = name, \
		.ifnum = QUIRK_ANY_INTERFACE, \
		.type = QUIRK_MIDI_YAMAHA \
	} \
}
#define YAMAHA_INTERFACE(id, intf, name) { \
	USB_DEVICE_VENDOR_SPEC(0x0499, id), \
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) { \
		.vendor_name = "Yamaha", \
		.product_name = name, \
		.ifnum = intf, \
		.type = QUIRK_MIDI_YAMAHA \
	} \
}
YAMAHA_DEVICE(0x1000, "UX256"),
YAMAHA_DEVICE(0x1001, "MU1000"),
YAMAHA_DEVICE(0x1002, "MU2000"),
YAMAHA_DEVICE(0x1003, "MU500"),
YAMAHA_INTERFACE(0x1004, 3, "UW500"),
YAMAHA_DEVICE(0x1005, "MOTIF6"),
YAMAHA_DEVICE(0x1006, "MOTIF7"),
YAMAHA_DEVICE(0x1007, "MOTIF8"),
YAMAHA_DEVICE(0x1008, "UX96"),
YAMAHA_DEVICE(0x1009, "UX16"),
YAMAHA_INTERFACE(0x100a, 3, "EOS BX"),
YAMAHA_DEVICE(0x100e, "S08"),
YAMAHA_DEVICE(0x100f, "CLP-150"),
YAMAHA_DEVICE(0x1010, "CLP-170"),
YAMAHA_DEVICE(0x1011, "P-250"),
YAMAHA_DEVICE(0x1012, "TYROS"),
YAMAHA_DEVICE(0x1013, "PF-500"),
YAMAHA_DEVICE(0x1014, "S90"),
YAMAHA_DEVICE(0x1015, "MOTIF-R"),
YAMAHA_DEVICE(0x1017, "CVP-204"),
YAMAHA_DEVICE(0x1018, "CVP-206"),
YAMAHA_DEVICE(0x1019, "CVP-208"),
YAMAHA_DEVICE(0x101a, "CVP-210"),
YAMAHA_DEVICE(0x101b, "PSR-1100"),
YAMAHA_DEVICE(0x101c, "PSR-2100"),
YAMAHA_DEVICE(0x101e, "PSR-K1"),
YAMAHA_DEVICE(0x1020, "EZ-250i"),
YAMAHA_DEVICE(0x1021, "MOTIF ES 6"),
YAMAHA_DEVICE(0x1022, "MOTIF ES 7"),
YAMAHA_DEVICE(0x1023, "MOTIF ES 8"),
YAMAHA_DEVICE(0x5000, "CS1D"),
YAMAHA_DEVICE(0x5001, "DSP1D"),
YAMAHA_DEVICE(0x5002, "DME32"),
YAMAHA_DEVICE(0x5003, "DM2000"),
YAMAHA_DEVICE(0x5004, "02R96"),
YAMAHA_DEVICE(0x5005, "ACU16-C"),
YAMAHA_DEVICE(0x5006, "NHB32-C"),
YAMAHA_DEVICE(0x5007, "DM1000"),
YAMAHA_DEVICE(0x5008, "01V96"),
#undef YAMAHA_DEVICE
#undef YAMAHA_INTERFACE

/*
 * Roland/RolandED/Edirol/BOSS devices
 */
{
	USB_DEVICE(0x0582, 0x0000),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "Roland",
		.product_name = "UA-100",
		.ifnum = QUIRK_ANY_INTERFACE,
		.type = QUIRK_COMPOSITE,
		.data = (const snd_usb_audio_quirk_t[]) {
			{
				.ifnum = 0,
				.type = QUIRK_AUDIO_FIXED_ENDPOINT,
				.data = & (const struct audioformat) {
					.format = SNDRV_PCM_FORMAT_S16_LE,
					.channels = 4,
					.iface = 0,
					.altsetting = 1,
					.altset_idx = 1,
					.attributes = 0,
					.endpoint = 0x01,
					.ep_attr = 0x09,
					.rates = SNDRV_PCM_RATE_CONTINUOUS,
					.rate_min = 44100,
					.rate_max = 44100,
				}
			},
			{
				.ifnum = 1,
				.type = QUIRK_AUDIO_FIXED_ENDPOINT,
				.data = & (const struct audioformat) {
					.format = SNDRV_PCM_FORMAT_S16_LE,
					.channels = 2,
					.iface = 1,
					.altsetting = 1,
					.altset_idx = 1,
					.attributes = EP_CS_ATTR_FILL_MAX,
					.endpoint = 0x81,
					.ep_attr = 0x05,
					.rates = SNDRV_PCM_RATE_CONTINUOUS,
					.rate_min = 44100,
					.rate_max = 44100,
				}
			},
			{
				.ifnum = 2,
				.type = QUIRK_MIDI_FIXED_ENDPOINT,
				.data = & (const snd_usb_midi_endpoint_info_t) {
					.out_cables = 0x0007,
					.in_cables  = 0x0007
				}
			},
			{
				.ifnum = -1
			}
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0002),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "EDIROL",
		.product_name = "UM-4",
		.ifnum = 2,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x000f,
			.in_cables  = 0x000f
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0003),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "Roland",
		.product_name = "SC-8850",
		.ifnum = 2,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x003f,
			.in_cables  = 0x003f
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0004),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "Roland",
		.product_name = "U-8",
		.ifnum = 2,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0005,
			.in_cables  = 0x0005
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0005),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "EDIROL",
		.product_name = "UM-2",
		.ifnum = 2,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0003,
			.in_cables  = 0x0003
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0007),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "Roland",
		.product_name = "SC-8820",
		.ifnum = 2,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0013,
			.in_cables  = 0x0013
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0008),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "Roland",
		.product_name = "PC-300",
		.ifnum = 2,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0001,
			.in_cables  = 0x0001
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0009),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "EDIROL",
		.product_name = "UM-1",
		.ifnum = 2,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0001,
			.in_cables  = 0x0001
		}
	}
},
{
	USB_DEVICE(0x0582, 0x000b),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "Roland",
		.product_name = "SK-500",
		.ifnum = 2,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0013,
			.in_cables  = 0x0013
		}
	}
},
{
	/* thanks to Emiliano Grilli <emillo@libero.it> for helping researching this data */
	USB_DEVICE(0x0582, 0x000c),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "Roland",
		.product_name = "SC-D70",
		.ifnum = QUIRK_ANY_INTERFACE,
		.type = QUIRK_COMPOSITE,
		.data = (const snd_usb_audio_quirk_t[]) {
			{
				.ifnum = 0,
				.type = QUIRK_AUDIO_FIXED_ENDPOINT,
				.data = & (const struct audioformat) {
					.format = SNDRV_PCM_FORMAT_S24_3LE,
					.channels = 2,
					.iface = 0,
					.altsetting = 1,
					.altset_idx = 1,
					.attributes = 0,
					.endpoint = 0x01,
					.ep_attr = 0x01,
					.rates = SNDRV_PCM_RATE_CONTINUOUS,
					.rate_min = 44100,
					.rate_max = 44100,
				}
			},
			{
				.ifnum = 1,
				.type = QUIRK_AUDIO_FIXED_ENDPOINT,
				.data = & (const struct audioformat) {
					.format = SNDRV_PCM_FORMAT_S24_3LE,
					.channels = 2,
					.iface = 1,
					.altsetting = 1,
					.altset_idx = 1,
					.attributes = 0,
					.endpoint = 0x81,
					.ep_attr = 0x01,
					.rates = SNDRV_PCM_RATE_CONTINUOUS,
					.rate_min = 44100,
					.rate_max = 44100,
				}
			},
			{
				.ifnum = 2,
				.type = QUIRK_MIDI_FIXED_ENDPOINT,
				.data = & (const snd_usb_midi_endpoint_info_t) {
					.out_cables = 0x0007,
					.in_cables  = 0x0007
				}
			},
			{
				.ifnum = -1
			}
		}
	}
},
{	/*
	 * This quirk is for the "Advanced Driver" mode of the Edirol UA-5.
	 * If the advanced mode switch at the back of the unit is off, the
	 * UA-5 has ID 0x0582/0x0011 and is standard compliant (no quirks),
	 * but offers only 16-bit PCM.
	 * In advanced mode, the UA-5 will output S24_3LE samples (two
	 * channels) at the rate indicated on the front switch, including
	 * the 96kHz sample rate.
	 */
	USB_DEVICE(0x0582, 0x0010),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "EDIROL",
		.product_name = "UA-5",
		.ifnum = QUIRK_ANY_INTERFACE,
		.type = QUIRK_COMPOSITE,
		.data = (const snd_usb_audio_quirk_t[]) {
			{
				.ifnum = 1,
				.type = QUIRK_AUDIO_STANDARD_INTERFACE
			},
			{
				.ifnum = 2,
				.type = QUIRK_AUDIO_STANDARD_INTERFACE
			},
			{
				.ifnum = -1
			}
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0012),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "Roland",
		.product_name = "XV-5050",
		.ifnum = 0,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0001,
			.in_cables  = 0x0001
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0014),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "EDIROL",
		.product_name = "UM-880",
		.ifnum = 0,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x01ff,
			.in_cables  = 0x01ff
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0016),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "EDIROL",
		.product_name = "SD-90",
		.ifnum = 2,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x000f,
			.in_cables  = 0x000f
		}
	}
},
{
	USB_DEVICE(0x0582, 0x001b),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "Roland",
		.product_name = "MMP-2",
		.ifnum = 2,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0001,
			.in_cables  = 0x0001
		}
	}
},
{
	USB_DEVICE(0x0582, 0x001d),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "Roland",
		.product_name = "V-SYNTH",
		.ifnum = 0,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0001,
			.in_cables  = 0x0001
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0023),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "EDIROL",
		.product_name = "UM-550",
		.ifnum = 0,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x003f,
			.in_cables  = 0x003f
		}
	}
},
{
	/*
	 * This quirk is for the "Advanced Driver" mode. If off, the UA-20
	 * has ID 0x0026 and is standard compliant, but has only 16-bit PCM
	 * and no MIDI.
	 */
	USB_DEVICE(0x0582, 0x0025),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "EDIROL",
		.product_name = "UA-20",
		.ifnum = QUIRK_ANY_INTERFACE,
		.type = QUIRK_COMPOSITE,
		.data = (const snd_usb_audio_quirk_t[]) {
			{
				.ifnum = 1,
				.type = QUIRK_AUDIO_STANDARD_INTERFACE
			},
			{
				.ifnum = 2,
				.type = QUIRK_AUDIO_STANDARD_INTERFACE
			},
			{
				.ifnum = 3,
				.type = QUIRK_MIDI_STANDARD_INTERFACE
			},
			{
				.ifnum = -1
			}
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0027),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "EDIROL",
		.product_name = "SD-20",
		.ifnum = 0,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0003,
			.in_cables  = 0x0007
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0029),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "EDIROL",
		.product_name = "SD-80",
		.ifnum = 0,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x000f,
			.in_cables  = 0x000f
		}
	}
},
{	/*
	 * This quirk is for the "Advanced" modes of the Edirol UA-700.
	 * If the sample format switch is not in an advanced setting, the
	 * UA-700 has ID 0x0582/0x002c and is standard compliant (no quirks),
	 * but offers only 16-bit PCM and no MIDI.
	 */
	USB_DEVICE_VENDOR_SPEC(0x0582, 0x002b),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "EDIROL",
		.product_name = "UA-700",
		.ifnum = QUIRK_ANY_INTERFACE,
		.type = QUIRK_COMPOSITE,
		.data = (const snd_usb_audio_quirk_t[]) {
			{
				.ifnum = 1,
				.type = QUIRK_AUDIO_EDIROL_UA700
			},
			{
				.ifnum = 2,
				.type = QUIRK_AUDIO_EDIROL_UA700
			},
			{
				.ifnum = 3,
				.type = QUIRK_AUDIO_EDIROL_UA700
			},
			{
				.ifnum = -1
			}
		}
	}
},
{
	USB_DEVICE(0x0582, 0x002d),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "Roland",
		.product_name = "XV-2020",
		.ifnum = 0,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0001,
			.in_cables  = 0x0001
		}
	}
},
{
	USB_DEVICE(0x0582, 0x002f),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "Roland",
		.product_name = "VariOS",
		.ifnum = 0,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0007,
			.in_cables  = 0x0007
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0033),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "EDIROL",
		.product_name = "PCR",
		.ifnum = 0,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0003,
			.in_cables  = 0x0007
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0037),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "Roland",
		.product_name = "Digital Piano",
		.ifnum = 0,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0001,
			.in_cables  = 0x0001
		}
	}
},
{
	USB_DEVICE_VENDOR_SPEC(0x0582, 0x003b),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "BOSS",
		.product_name = "GS-10",
		.ifnum = 3,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0003,
			.in_cables  = 0x0003
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0040),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "Roland",
		.product_name = "GI-20",
		.ifnum = 0,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0001,
			.in_cables  = 0x0001
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0044),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "Roland",
		.product_name = "UA-1000",
		.ifnum = QUIRK_ANY_INTERFACE,
		.type = QUIRK_COMPOSITE,
		.data = (const snd_usb_audio_quirk_t[]) {
			{
				.ifnum = 1,
				.type = QUIRK_AUDIO_FIXED_ENDPOINT,
				.data = & (const struct audioformat) {
					.format = SNDRV_PCM_FORMAT_S24,
					.channels = 12,
					.iface = 1,
					.altsetting = 1,
					.altset_idx = 1,
					.attributes = 0,
					.endpoint = 0x81,
					.ep_attr = 0x01,
					.rates = SNDRV_PCM_RATE_CONTINUOUS,
					.rate_min = 48000,
					.rate_max = 48000,
				}
			},
			{
				.ifnum = 2,
				.type = QUIRK_AUDIO_FIXED_ENDPOINT,
				.data = & (const struct audioformat) {
					.format = SNDRV_PCM_FORMAT_S24,
					.channels = 10,
					.iface = 2,
					.altsetting = 1,
					.altset_idx = 1,
					.attributes = 0,
					.endpoint = 0x02,
					.ep_attr = 0x01,
					.rates = SNDRV_PCM_RATE_CONTINUOUS,
					.rate_min = 48000,
					.rate_max = 48000,
				}
			},
			{
				.ifnum = 3,
				.type = QUIRK_MIDI_FIXED_ENDPOINT,
				.data = & (const snd_usb_midi_endpoint_info_t) {
					.out_cables = 0x0003,
					.in_cables  = 0x0003
				}
			},
			{
				.ifnum = -1
			}
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0048),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "EDIROL",
		.product_name = "UR-80",
		.ifnum = 0,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0003,
			.in_cables  = 0x0007
		}
	}
},
{
	USB_DEVICE(0x0582, 0x004d),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "EDIROL",
		.product_name = "PCR-A",
		.ifnum = 0,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0003,
			.in_cables  = 0x0007
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0065),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "EDIROL",
		.product_name = "PCR-1",
		.ifnum = 0,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0001,
			.in_cables  = 0x0003
		}
	}
},
{
	/*
	 * This quirk is for the "Advanced Driver" mode. If off, the UA-3FX
	 * is standard compliant, but has only 16-bit PCM.
	 */
	USB_DEVICE(0x0582, 0x0050),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "EDIROL",
		.product_name = "UA-3FX",
		.ifnum = QUIRK_ANY_INTERFACE,
		.type = QUIRK_COMPOSITE,
		.data = (const snd_usb_audio_quirk_t[]) {
			{
				.ifnum = 1,
				.type = QUIRK_AUDIO_STANDARD_INTERFACE
			},
			{
				.ifnum = 2,
				.type = QUIRK_AUDIO_STANDARD_INTERFACE
			},
			{
				.ifnum = -1
			}
		}
	}
},
{
	USB_DEVICE(0x0582, 0x0052),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "EDIROL",
		.product_name = "UM-1SX",
		.ifnum = 0,
		.type = QUIRK_MIDI_STANDARD_INTERFACE
	}
},

/* Midiman/M-Audio devices */
{
	USB_DEVICE_VENDOR_SPEC(0x0763, 0x1002),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "M-Audio",
		.product_name = "MidiSport 2x2",
		.ifnum = QUIRK_ANY_INTERFACE,
		.type = QUIRK_MIDI_MIDIMAN,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0003,
			.in_cables  = 0x0003
		}
	}
},
{
	USB_DEVICE_VENDOR_SPEC(0x0763, 0x1011),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "M-Audio",
		.product_name = "MidiSport 1x1",
		.ifnum = QUIRK_ANY_INTERFACE,
		.type = QUIRK_MIDI_MIDIMAN,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0001,
			.in_cables  = 0x0001
		}
	}
},
{
	USB_DEVICE_VENDOR_SPEC(0x0763, 0x1015),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "M-Audio",
		.product_name = "Keystation",
		.ifnum = QUIRK_ANY_INTERFACE,
		.type = QUIRK_MIDI_MIDIMAN,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0001,
			.in_cables  = 0x0001
		}
	}
},
{
	USB_DEVICE_VENDOR_SPEC(0x0763, 0x1021),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "M-Audio",
		.product_name = "MidiSport 4x4",
		.ifnum = QUIRK_ANY_INTERFACE,
		.type = QUIRK_MIDI_MIDIMAN,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x000f,
			.in_cables  = 0x000f
		}
	}
},
{
	/*
	 * For hardware revision 1.05; in the later revisions (1.10 and
	 * 1.21), 0x1031 is the ID for the device without firmware.
	 * Thanks to Olaf Giesbrecht <Olaf_Giesbrecht@yahoo.de>
	 */
	USB_DEVICE_VER(0x0763, 0x1031, 0x0100, 0x0109),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "M-Audio",
		.product_name = "MidiSport 8x8",
		.ifnum = QUIRK_ANY_INTERFACE,
		.type = QUIRK_MIDI_MIDIMAN,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x01ff,
			.in_cables  = 0x01ff
		}
	}
},
{
	USB_DEVICE_VENDOR_SPEC(0x0763, 0x1033),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "M-Audio",
		.product_name = "MidiSport 8x8",
		.ifnum = QUIRK_ANY_INTERFACE,
		.type = QUIRK_MIDI_MIDIMAN,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x01ff,
			.in_cables  = 0x01ff
		}
	}
},
{
	USB_DEVICE_VENDOR_SPEC(0x0763, 0x1041),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "M-Audio",
		.product_name = "MidiSport 2x4",
		.ifnum = QUIRK_ANY_INTERFACE,
		.type = QUIRK_MIDI_MIDIMAN,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x000f,
			.in_cables  = 0x0003
		}
	}
},
{
	USB_DEVICE_VENDOR_SPEC(0x0763, 0x2001),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "M-Audio",
		.product_name = "Quattro",
		.ifnum = 9,
		.type = QUIRK_MIDI_MIDIMAN,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0001,
			.in_cables  = 0x0001
		}
	}
},
{
	USB_DEVICE_VENDOR_SPEC(0x0763, 0x2003),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "M-Audio",
		.product_name = "AudioPhile",
		.ifnum = 6,
		.type = QUIRK_MIDI_MIDIMAN,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0001,
			.in_cables  = 0x0001
		}
	}
},
{
	USB_DEVICE_VENDOR_SPEC(0x0763, 0x2008),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "M-Audio",
		.product_name = "Ozone",
		.ifnum = 3,
		.type = QUIRK_MIDI_MIDIMAN,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0001,
			.in_cables  = 0x0001
		}
	}
},
{
	USB_DEVICE_VENDOR_SPEC(0x0763, 0x200d),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "M-Audio",
		.product_name = "OmniStudio",
		.ifnum = 9,
		.type = QUIRK_MIDI_MIDIMAN,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0001,
			.in_cables  = 0x0001
		}
	}
},

/* Mark of the Unicorn devices */
{
	/* thanks to Woodley Packard <sweaglesw@thibs.menloschool.org> */
	USB_DEVICE(0x07fd, 0x0001),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "MOTU",
		.product_name = "Fastlane",
		.ifnum = 1,
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = & (const snd_usb_midi_endpoint_info_t) {
			.out_cables = 0x0003,
			.in_cables  = 0x0003
		}
	}
},

{
	/* Creative Sound Blaster MP3+ */
	USB_DEVICE(0x041e, 0x3010),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "Creative Labs",
		.product_name = "Sound Blaster MP3+",
		.ifnum = QUIRK_NO_INTERFACE
	}
	
},

{
	USB_DEVICE_VENDOR_SPEC(0x0ccd, 0x0013),
	.driver_info = (unsigned long) & (const snd_usb_audio_quirk_t) {
		.vendor_name = "Terratec",
		.product_name = "PHASE 26",
		.ifnum = 3,
		.type = QUIRK_MIDI_STANDARD_INTERFACE
	}
},

#undef USB_DEVICE_VENDOR_SPEC
