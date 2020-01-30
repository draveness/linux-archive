#ifndef __USBAUDIO_H
#define __USBAUDIO_H
/*
 *   (Tentative) USB Audio Driver for ALSA
 *
 *   Copyright (c) 2002 by Takashi Iwai <tiwai@suse.de>
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
 */


/*
 */

#define USB_SUBCLASS_AUDIO_CONTROL	0x01
#define USB_SUBCLASS_AUDIO_STREAMING	0x02
#define USB_SUBCLASS_MIDI_STREAMING	0x03
#define USB_SUBCLASS_VENDOR_SPEC	0xff

#define CS_AUDIO_UNDEFINED		0x20
#define CS_AUDIO_DEVICE			0x21
#define CS_AUDIO_CONFIGURATION		0x22
#define CS_AUDIO_STRING			0x23
#define CS_AUDIO_INTERFACE		0x24
#define CS_AUDIO_ENDPOINT		0x25

#define HEADER				0x01
#define INPUT_TERMINAL			0x02
#define OUTPUT_TERMINAL			0x03
#define MIXER_UNIT			0x04
#define SELECTOR_UNIT			0x05
#define FEATURE_UNIT			0x06
#define PROCESSING_UNIT			0x07
#define EXTENSION_UNIT			0x08

#define AS_GENERAL			0x01
#define FORMAT_TYPE			0x02
#define FORMAT_SPECIFIC			0x03

#define EP_GENERAL			0x01

#define MS_GENERAL			0x01
#define MIDI_IN_JACK			0x02
#define MIDI_OUT_JACK			0x03

/* endpoint attributes */
#define EP_ATTR_MASK			0x0c
#define EP_ATTR_ASYNC			0x04
#define EP_ATTR_ADAPTIVE		0x08
#define EP_ATTR_SYNC			0x0c

/* cs endpoint attributes */
#define EP_CS_ATTR_SAMPLE_RATE		0x01
#define EP_CS_ATTR_PITCH_CONTROL	0x02
#define EP_CS_ATTR_FILL_MAX		0x80

/* Audio Class specific Request Codes */

#define SET_CUR    0x01
#define GET_CUR    0x81
#define SET_MIN    0x02
#define GET_MIN    0x82
#define SET_MAX    0x03
#define GET_MAX    0x83
#define SET_RES    0x04
#define GET_RES    0x84
#define SET_MEM    0x05
#define GET_MEM    0x85
#define GET_STAT   0xff

/* Terminal Control Selectors */

#define COPY_PROTECT_CONTROL       0x01

/* Endpoint Control Selectors */

#define SAMPLING_FREQ_CONTROL      0x01
#define PITCH_CONTROL              0x02

/* Format Types */
#define USB_FORMAT_TYPE_I	0x01
#define USB_FORMAT_TYPE_II	0x02
#define USB_FORMAT_TYPE_III	0x03

/* type I */
#define USB_AUDIO_FORMAT_PCM	0x01
#define USB_AUDIO_FORMAT_PCM8	0x02
#define USB_AUDIO_FORMAT_IEEE_FLOAT	0x03
#define USB_AUDIO_FORMAT_ALAW	0x04
#define USB_AUDIO_FORMAT_MU_LAW	0x05

/* type II */
#define USB_AUDIO_FORMAT_MPEG	0x1001
#define USB_AUDIO_FORMAT_AC3	0x1002

/* type III */
#define USB_AUDIO_FORMAT_IEC1937_AC3	0x2001
#define USB_AUDIO_FORMAT_IEC1937_MPEG1_LAYER1	0x2002
#define USB_AUDIO_FORMAT_IEC1937_MPEG2_NOEXT	0x2003
#define USB_AUDIO_FORMAT_IEC1937_MPEG2_EXT	0x2004
#define USB_AUDIO_FORMAT_IEC1937_MPEG2_LAYER1_LS	0x2005
#define USB_AUDIO_FORMAT_IEC1937_MPEG2_LAYER23_LS	0x2006


/* maximum number of endpoints per interface */
#define MIDI_MAX_ENDPOINTS 2

/*
 */

typedef struct snd_usb_audio snd_usb_audio_t;

struct snd_usb_audio {
	
	int index;
	struct usb_device *dev;
	snd_card_t *card;
	int shutdown;
	int num_interfaces;

	struct list_head pcm_list;	/* list of pcm streams */
	int pcm_devs;

	struct list_head midi_list;	/* list of midi interfaces */
	int next_midi_device;

	unsigned int ignore_ctl_error;	/* for mixer */
};  

/*
 * Information about devices with broken descriptors
 */

#define QUIRK_NO_INTERFACE		-2
#define QUIRK_ANY_INTERFACE		-1

#define QUIRK_MIDI_FIXED_ENDPOINT	0
#define QUIRK_MIDI_YAMAHA		1
#define QUIRK_MIDI_MIDIMAN		2
#define QUIRK_COMPOSITE			3
#define QUIRK_AUDIO_FIXED_ENDPOINT	4
#define QUIRK_AUDIO_STANDARD_INTERFACE	5
#define QUIRK_MIDI_STANDARD_INTERFACE	6
#define QUIRK_AUDIO_EDIROL_UA700	7

typedef struct snd_usb_audio_quirk snd_usb_audio_quirk_t;
typedef struct snd_usb_midi_endpoint_info snd_usb_midi_endpoint_info_t;

struct snd_usb_audio_quirk {
	const char *vendor_name;
	const char *product_name;
	int16_t ifnum;
	int16_t type;
	const void *data;
};

/* data for QUIRK_MIDI_FIXED_ENDPOINT */
struct snd_usb_midi_endpoint_info {
	int8_t out_ep, in_ep;	/* ep number, 0 autodetect */
	uint16_t out_cables;	/* bitmask */
	uint16_t in_cables;	/* bitmask */
};

/* for QUIRK_MIDI_YAMAHA, data is NULL */

/* for QUIRK_MIDI_MIDIMAN, data points to a snd_usb_midi_endpoint_info
 * structure (out_cables and in_cables only) */

/* for QUIRK_COMPOSITE, data points to an array of snd_usb_audio_quirk
 * structures, terminated with .ifnum = -1 */

/* for QUIRK_AUDIO_FIXED_ENDPOINT, data points to an audioformat structure */

/* for QUIRK_AUDIO/MIDI_STANDARD_INTERFACE, data is NULL */

/* for QUIRK_AUDIO_EDIROL_UA700, data is NULL */

/*
 */

#define combine_word(s)    ((*s) | ((unsigned int)(s)[1] << 8))
#define combine_triple(s)  (combine_word(s) | ((unsigned int)(s)[2] << 16))
#define combine_quad(s)    (combine_triple(s) | ((unsigned int)(s)[3] << 24))

unsigned int snd_usb_combine_bytes(unsigned char *bytes, int size);

void *snd_usb_find_desc(void *descstart, int desclen, void *after, u8 dtype);
void *snd_usb_find_csint_desc(void *descstart, int desclen, void *after, u8 dsubtype);

int snd_usb_ctl_msg(struct usb_device *dev, unsigned int pipe, __u8 request, __u8 requesttype, __u16 value, __u16 index, void *data, __u16 size, int timeout);

int snd_usb_create_mixer(snd_usb_audio_t *chip, int ctrlif);

int snd_usb_create_midi_interface(snd_usb_audio_t *chip, struct usb_interface *iface, const snd_usb_audio_quirk_t *quirk);
void snd_usbmidi_disconnect(struct list_head *p, struct usb_driver *driver);

/*
 * retrieve usb_interface descriptor from the host interface
 * (conditional for compatibility with the older API)
 */
#ifndef get_iface_desc
#define get_iface_desc(iface)	(&(iface)->desc)
#define get_endpoint(alt,ep)	(&(alt)->endpoint[ep].desc)
#define get_ep_desc(ep)		(&(ep)->desc)
#define get_cfg_desc(cfg)	(&(cfg)->desc)
#endif

#ifndef usb_pipe_needs_resubmit
#define usb_pipe_needs_resubmit(pipe) 1
#endif

#ifndef snd_usb_complete_callback
#define snd_usb_complete_callback(x) (x)
#endif

#ifndef snd_usb_get_speed
#define snd_usb_get_speed(dev) ((dev)->speed)
#endif

#endif /* __USBAUDIO_H */
