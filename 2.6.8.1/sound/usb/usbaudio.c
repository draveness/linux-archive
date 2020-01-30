/*
 *   (Tentative) USB Audio Driver for ALSA
 *
 *   Main and PCM part
 *
 *   Copyright (c) 2002 by Takashi Iwai <tiwai@suse.de>
 *
 *   Many codes borrowed from audio.c by 
 *	    Alan Cox (alan@lxorguk.ukuu.org.uk)
 *	    Thomas Sailer (sailer@ife.ee.ethz.ch)
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
 *
 *  NOTES:
 *
 *   - async unlink should be used for avoiding the sleep inside lock.
 *     2.4.22 usb-uhci seems buggy for async unlinking and results in
 *     oops.  in such a cse, pass async_unlink=0 option.
 *   - the linked URBs would be preferred but not used so far because of
 *     the instability of unlinking.
 *   - type II is not supported properly.  there is no device which supports
 *     this type *correctly*.  SB extigy looks as if it supports, but it's
 *     indeed an AC3 stream packed in SPDIF frames (i.e. no real AC3 stream).
 */


#include <sound/driver.h>
#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/usb.h>
#include <linux/moduleparam.h>
#include <sound/core.h>
#include <sound/info.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>

#include "usbaudio.h"


MODULE_AUTHOR("Takashi Iwai <tiwai@suse.de>");
MODULE_DESCRIPTION("USB Audio");
MODULE_LICENSE("GPL");
MODULE_CLASSES("{sound}");
MODULE_DEVICES("{{Generic,USB Audio}}");


static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;	/* Index 0-MAX */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;	/* ID for this card */
static int enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;	/* Enable this card */
static int vid[SNDRV_CARDS] = { [0 ... (SNDRV_CARDS-1)] = -1 }; /* Vendor ID for this card */
static int pid[SNDRV_CARDS] = { [0 ... (SNDRV_CARDS-1)] = -1 }; /* Product ID for this card */
static int nrpacks = 4;		/* max. number of packets per urb */
static int async_unlink = 1;
static int boot_devs;

module_param_array(index, int, boot_devs, 0444);
MODULE_PARM_DESC(index, "Index value for the USB audio adapter.");
MODULE_PARM_SYNTAX(index, SNDRV_INDEX_DESC);
module_param_array(id, charp, boot_devs, 0444);
MODULE_PARM_DESC(id, "ID string for the USB audio adapter.");
MODULE_PARM_SYNTAX(id, SNDRV_ID_DESC);
module_param_array(enable, bool, boot_devs, 0444);
MODULE_PARM_DESC(enable, "Enable USB audio adapter.");
MODULE_PARM_SYNTAX(enable, SNDRV_ENABLE_DESC);
module_param_array(vid, int, boot_devs, 0444);
MODULE_PARM_DESC(vid, "Vendor ID for the USB audio device.");
MODULE_PARM_SYNTAX(vid, SNDRV_ENABLED ",allows:{{-1,0xffff}},base:16");
module_param_array(pid, int, boot_devs, 0444);
MODULE_PARM_DESC(pid, "Product ID for the USB audio device.");
MODULE_PARM_SYNTAX(pid, SNDRV_ENABLED ",allows:{{-1,0xffff}},base:16");
module_param(nrpacks, int, 0444);
MODULE_PARM_DESC(nrpacks, "Max. number of packets per URB.");
MODULE_PARM_SYNTAX(nrpacks, SNDRV_ENABLED ",allows:{{1,10}}");
module_param(async_unlink, bool, 0444);
MODULE_PARM_DESC(async_unlink, "Use async unlink mode.");
MODULE_PARM_SYNTAX(async_unlink, SNDRV_BOOLEAN_TRUE_DESC);


/*
 * debug the h/w constraints
 */
/* #define HW_CONST_DEBUG */


/*
 *
 */

#define MAX_PACKS	10	
#define MAX_PACKS_HS	(MAX_PACKS * 8)	/* in high speed mode */
#define MAX_URBS	5	/* max. 20ms long packets */
#define SYNC_URBS	2	/* always two urbs for sync */
#define MIN_PACKS_URB	1	/* minimum 1 packet per urb */

typedef struct snd_usb_substream snd_usb_substream_t;
typedef struct snd_usb_stream snd_usb_stream_t;
typedef struct snd_urb_ctx snd_urb_ctx_t;

struct audioformat {
	struct list_head list;
	snd_pcm_format_t format;	/* format type */
	unsigned int channels;		/* # channels */
	unsigned int fmt_type;		/* USB audio format type (1-3) */
	unsigned int frame_size;	/* samples per frame for non-audio */
	int iface;			/* interface number */
	unsigned char altsetting;	/* corresponding alternate setting */
	unsigned char altset_idx;	/* array index of altenate setting */
	unsigned char attributes;	/* corresponding attributes of cs endpoint */
	unsigned char endpoint;		/* endpoint */
	unsigned char ep_attr;		/* endpoint attributes */
	unsigned int maxpacksize;	/* max. packet size */
	unsigned int rates;		/* rate bitmasks */
	unsigned int rate_min, rate_max;	/* min/max rates */
	unsigned int nr_rates;		/* number of rate table entries */
	unsigned int *rate_table;	/* rate table */
};

struct snd_urb_ctx {
	struct urb *urb;
	snd_usb_substream_t *subs;
	int index;	/* index for urb array */
	int packets;	/* number of packets per urb */
	int transfer;	/* transferred size */
	char *buf;	/* buffer for capture */
};

struct snd_urb_ops {
	int (*prepare)(snd_usb_substream_t *subs, snd_pcm_runtime_t *runtime, struct urb *u);
	int (*retire)(snd_usb_substream_t *subs, snd_pcm_runtime_t *runtime, struct urb *u);
	int (*prepare_sync)(snd_usb_substream_t *subs, snd_pcm_runtime_t *runtime, struct urb *u);
	int (*retire_sync)(snd_usb_substream_t *subs, snd_pcm_runtime_t *runtime, struct urb *u);
};

struct snd_usb_substream {
	snd_usb_stream_t *stream;
	struct usb_device *dev;
	snd_pcm_substream_t *pcm_substream;
	int direction;	/* playback or capture */
	int interface;	/* current interface */
	int endpoint;	/* assigned endpoint */
	struct audioformat *cur_audiofmt;	/* current audioformat pointer (for hw_params callback) */
	unsigned int cur_rate;		/* current rate (for hw_params callback) */
	unsigned int period_bytes;	/* current period bytes (for hw_params callback) */
	unsigned int format;     /* USB data format */
	unsigned int datapipe;   /* the data i/o pipe */
	unsigned int syncpipe;   /* 1 - async out or adaptive in */
	unsigned int syncinterval;  /* P for adaptive mode, 0 otherwise */
	unsigned int freqn;      /* nominal sampling rate in fs/fps in Q16.16 format */
	unsigned int freqm;      /* momentary sampling rate in fs/fps in Q16.16 format */
	unsigned int freqmax;    /* maximum sampling rate, used for buffer management */
	unsigned int phase;      /* phase accumulator */
	unsigned int maxpacksize;	/* max packet size in bytes */
	unsigned int maxframesize;	/* max packet size in frames */
	unsigned int curpacksize;	/* current packet size in bytes (for capture) */
	unsigned int curframesize;	/* current packet size in frames (for capture) */
	unsigned int fill_max: 1;	/* fill max packet size always */
	unsigned int fmt_type;		/* USB audio format type (1-3) */

	unsigned int running: 1;	/* running status */

	unsigned int hwptr;			/* free frame position in the buffer (only for playback) */
	unsigned int hwptr_done;			/* processed frame position in the buffer */
	unsigned int transfer_sched;		/* scheduled frames since last period (for playback) */
	unsigned int transfer_done;		/* processed frames since last period update */
	unsigned long active_mask;	/* bitmask of active urbs */
	unsigned long unlink_mask;	/* bitmask of unlinked urbs */

	unsigned int nurbs;			/* # urbs */
	snd_urb_ctx_t dataurb[MAX_URBS];	/* data urb table */
	snd_urb_ctx_t syncurb[SYNC_URBS];	/* sync urb table */
	char syncbuf[SYNC_URBS * MAX_PACKS * 4]; /* sync buffer; it's so small - let's get static */
	char *tmpbuf;			/* temporary buffer for playback */

	u64 formats;			/* format bitmasks (all or'ed) */
	unsigned int num_formats;		/* number of supported audio formats (list) */
	struct list_head fmt_list;	/* format list */
	spinlock_t lock;

	struct snd_urb_ops ops;		/* callbacks (must be filled at init) */
};


struct snd_usb_stream {
	snd_usb_audio_t *chip;
	snd_pcm_t *pcm;
	int pcm_index;
	unsigned int fmt_type;		/* USB audio format type (1-3) */
	snd_usb_substream_t substream[2];
	struct list_head list;
};

#define chip_t snd_usb_stream_t


/*
 * we keep the snd_usb_audio_t instances by ourselves for merging
 * the all interfaces on the same card as one sound device.
 */

static DECLARE_MUTEX(register_mutex);
static snd_usb_audio_t *usb_chip[SNDRV_CARDS];


/*
 * convert a sampling rate into our full speed format (fs/1000 in Q16.16)
 * this will overflow at approx 524 kHz
 */
inline static unsigned get_usb_full_speed_rate(unsigned int rate)
{
	return ((rate << 13) + 62) / 125;
}

/*
 * convert a sampling rate into USB high speed format (fs/8000 in Q16.16)
 * this will overflow at approx 4 MHz
 */
inline static unsigned get_usb_high_speed_rate(unsigned int rate)
{
	return ((rate << 10) + 62) / 125;
}

/* convert our full speed USB rate into sampling rate in Hz */
inline static unsigned get_full_speed_hz(unsigned int usb_rate)
{
	return (usb_rate * 125 + (1 << 12)) >> 13;
}

/* convert our high speed USB rate into sampling rate in Hz */
inline static unsigned get_high_speed_hz(unsigned int usb_rate)
{
	return (usb_rate * 125 + (1 << 9)) >> 10;
}


/*
 * prepare urb for full speed capture sync pipe
 *
 * fill the length and offset of each urb descriptor.
 * the fixed 10.14 frequency is passed through the pipe.
 */
static int prepare_capture_sync_urb(snd_usb_substream_t *subs,
				    snd_pcm_runtime_t *runtime,
				    struct urb *urb)
{
	unsigned char *cp = urb->transfer_buffer;
	snd_urb_ctx_t *ctx = (snd_urb_ctx_t *)urb->context;
	int i, offs;

	urb->number_of_packets = ctx->packets;
	urb->dev = ctx->subs->dev; /* we need to set this at each time */
	for (i = offs = 0; i < urb->number_of_packets; i++, offs += 4, cp += 4) {
		urb->iso_frame_desc[i].length = 3;
		urb->iso_frame_desc[i].offset = offs;
		cp[0] = subs->freqn >> 2;
		cp[1] = subs->freqn >> 10;
		cp[2] = subs->freqn >> 18;
	}
	return 0;
}

/*
 * prepare urb for high speed capture sync pipe
 *
 * fill the length and offset of each urb descriptor.
 * the fixed 12.13 frequency is passed as 16.16 through the pipe.
 */
static int prepare_capture_sync_urb_hs(snd_usb_substream_t *subs,
				       snd_pcm_runtime_t *runtime,
				       struct urb *urb)
{
	unsigned char *cp = urb->transfer_buffer;
	snd_urb_ctx_t *ctx = (snd_urb_ctx_t *)urb->context;
	int i, offs;

	urb->number_of_packets = ctx->packets;
	urb->dev = ctx->subs->dev; /* we need to set this at each time */
	for (i = offs = 0; i < urb->number_of_packets; i++, offs += 4, cp += 4) {
		urb->iso_frame_desc[i].length = 4;
		urb->iso_frame_desc[i].offset = offs;
		cp[0] = subs->freqn;
		cp[1] = subs->freqn >> 8;
		cp[2] = subs->freqn >> 16;
		cp[3] = subs->freqn >> 24;
	}
	return 0;
}

/*
 * process after capture sync complete
 * - nothing to do
 */
static int retire_capture_sync_urb(snd_usb_substream_t *subs,
				   snd_pcm_runtime_t *runtime,
				   struct urb *urb)
{
	return 0;
}

/*
 * prepare urb for capture data pipe
 *
 * fill the offset and length of each descriptor.
 *
 * we use a temporary buffer to write the captured data.
 * since the length of written data is determined by host, we cannot
 * write onto the pcm buffer directly...  the data is thus copied
 * later at complete callback to the global buffer.
 */
static int prepare_capture_urb(snd_usb_substream_t *subs,
			       snd_pcm_runtime_t *runtime,
			       struct urb *urb)
{
	int i, offs;
	unsigned long flags;
	snd_urb_ctx_t *ctx = (snd_urb_ctx_t *)urb->context;

	offs = 0;
	urb->dev = ctx->subs->dev; /* we need to set this at each time */
	urb->number_of_packets = 0;
	spin_lock_irqsave(&subs->lock, flags);
	for (i = 0; i < ctx->packets; i++) {
		urb->iso_frame_desc[i].offset = offs;
		urb->iso_frame_desc[i].length = subs->curpacksize;
		offs += subs->curpacksize;
		urb->number_of_packets++;
		subs->transfer_sched += subs->curframesize;
		if (subs->transfer_sched >= runtime->period_size) {
			subs->transfer_sched -= runtime->period_size;
			break;
		}
	}
	spin_unlock_irqrestore(&subs->lock, flags);
	urb->transfer_buffer = ctx->buf;
	urb->transfer_buffer_length = offs;
#if 0 // for check
	if (! urb->bandwidth) {
		int bustime;
		bustime = usb_check_bandwidth(urb->dev, urb);
		if (bustime < 0) 
			return bustime;
		printk("urb %d: bandwidth = %d (packets = %d)\n", ctx->index, bustime, urb->number_of_packets);
		usb_claim_bandwidth(urb->dev, urb, bustime, 1);
	}
#endif // for check
	return 0;
}

/*
 * process after capture complete
 *
 * copy the data from each desctiptor to the pcm buffer, and
 * update the current position.
 */
static int retire_capture_urb(snd_usb_substream_t *subs,
			      snd_pcm_runtime_t *runtime,
			      struct urb *urb)
{
	unsigned long flags;
	unsigned char *cp;
	int i;
	unsigned int stride, len, oldptr;

	stride = runtime->frame_bits >> 3;

	for (i = 0; i < urb->number_of_packets; i++) {
		cp = (unsigned char *)urb->transfer_buffer + urb->iso_frame_desc[i].offset;
		if (urb->iso_frame_desc[i].status) {
			snd_printd(KERN_ERR "frame %d active: %d\n", i, urb->iso_frame_desc[i].status);
			// continue;
		}
		len = urb->iso_frame_desc[i].actual_length / stride;
		if (! len)
			continue;
		/* update the current pointer */
		spin_lock_irqsave(&subs->lock, flags);
		oldptr = subs->hwptr_done;
		subs->hwptr_done += len;
		if (subs->hwptr_done >= runtime->buffer_size)
			subs->hwptr_done -= runtime->buffer_size;
		subs->transfer_done += len;
		spin_unlock_irqrestore(&subs->lock, flags);
		/* copy a data chunk */
		if (oldptr + len > runtime->buffer_size) {
			unsigned int cnt = runtime->buffer_size - oldptr;
			unsigned int blen = cnt * stride;
			memcpy(runtime->dma_area + oldptr * stride, cp, blen);
			memcpy(runtime->dma_area, cp + blen, len * stride - blen);
		} else {
			memcpy(runtime->dma_area + oldptr * stride, cp, len * stride);
		}
		/* update the pointer, call callback if necessary */
		spin_lock_irqsave(&subs->lock, flags);
		if (subs->transfer_done >= runtime->period_size) {
			subs->transfer_done -= runtime->period_size;
			spin_unlock_irqrestore(&subs->lock, flags);
			snd_pcm_period_elapsed(subs->pcm_substream);
		} else
			spin_unlock_irqrestore(&subs->lock, flags);
	}
	return 0;
}


/*
 * prepare urb for full speed playback sync pipe
 *
 * set up the offset and length to receive the current frequency.
 */

static int prepare_playback_sync_urb(snd_usb_substream_t *subs,
				     snd_pcm_runtime_t *runtime,
				     struct urb *urb)
{
	int i, offs;
	snd_urb_ctx_t *ctx = (snd_urb_ctx_t *)urb->context;

	urb->number_of_packets = ctx->packets;
	urb->dev = ctx->subs->dev; /* we need to set this at each time */
	for (i = offs = 0; i < urb->number_of_packets; i++, offs += 4) {
		urb->iso_frame_desc[i].length = 3;
		urb->iso_frame_desc[i].offset = offs;
	}
	return 0;
}

/*
 * prepare urb for high speed playback sync pipe
 *
 * set up the offset and length to receive the current frequency.
 */

static int prepare_playback_sync_urb_hs(snd_usb_substream_t *subs,
					snd_pcm_runtime_t *runtime,
					struct urb *urb)
{
	int i, offs;
	snd_urb_ctx_t *ctx = (snd_urb_ctx_t *)urb->context;

	urb->number_of_packets = ctx->packets;
	urb->dev = ctx->subs->dev; /* we need to set this at each time */
	for (i = offs = 0; i < urb->number_of_packets; i++, offs += 4) {
		urb->iso_frame_desc[i].length = 4;
		urb->iso_frame_desc[i].offset = offs;
	}
	return 0;
}

/*
 * process after full speed playback sync complete
 *
 * retrieve the current 10.14 frequency from pipe, and set it.
 * the value is referred in prepare_playback_urb().
 */
static int retire_playback_sync_urb(snd_usb_substream_t *subs,
				    snd_pcm_runtime_t *runtime,
				    struct urb *urb)
{
	int i;
	unsigned int f, found;
	unsigned char *cp = urb->transfer_buffer;
	unsigned long flags;

	found = 0;
	for (i = 0; i < urb->number_of_packets; i++, cp += 4) {
		if (urb->iso_frame_desc[i].status ||
		    urb->iso_frame_desc[i].actual_length < 3)
			continue;
		f = combine_triple(cp) << 2;
#if 0
		if (f < subs->freqn - (subs->freqn>>3) || f > subs->freqmax) {
			snd_printd(KERN_WARNING "requested frequency %d (%u,%03uHz) out of range (current nominal %d (%u,%03uHz))\n",
				   f, f >> 14, (f & ((1 << 14) - 1) * 1000) / ((1 << 14) - 1),
				   subs->freqn, subs->freqn >> 14, (subs->freqn & ((1 << 14) - 1) * 1000) / ((1 << 14) - 1));
			continue;
		}
#endif
		found = f;
	}
	if (found) {
		spin_lock_irqsave(&subs->lock, flags);
		subs->freqm = found;
		spin_unlock_irqrestore(&subs->lock, flags);
	}

	return 0;
}

/*
 * process after high speed playback sync complete
 *
 * retrieve the current 12.13 frequency from pipe, and set it.
 * the value is referred in prepare_playback_urb().
 */
static int retire_playback_sync_urb_hs(snd_usb_substream_t *subs,
				       snd_pcm_runtime_t *runtime,
				       struct urb *urb)
{
	int i;
	unsigned int found;
	unsigned char *cp = urb->transfer_buffer;
	unsigned long flags;

	found = 0;
	for (i = 0; i < urb->number_of_packets; i++, cp += 4) {
		if (urb->iso_frame_desc[i].status ||
		    urb->iso_frame_desc[i].actual_length < 4)
			continue;
		found = combine_quad(cp) & 0x0fffffff;
	}
	if (found) {
		spin_lock_irqsave(&subs->lock, flags);
		subs->freqm = found;
		spin_unlock_irqrestore(&subs->lock, flags);
	}

	return 0;
}

/*
 * prepare urb for playback data pipe
 *
 * we copy the data directly from the pcm buffer.
 * the current position to be copied is held in hwptr field.
 * since a urb can handle only a single linear buffer, if the total
 * transferred area overflows the buffer boundary, we cannot send
 * it directly from the buffer.  thus the data is once copied to
 * a temporary buffer and urb points to that.
 */
static int prepare_playback_urb(snd_usb_substream_t *subs,
				snd_pcm_runtime_t *runtime,
				struct urb *urb)
{
	int i, stride, offs;
	unsigned int counts;
	unsigned long flags;
	snd_urb_ctx_t *ctx = (snd_urb_ctx_t *)urb->context;

	stride = runtime->frame_bits >> 3;

	offs = 0;
	urb->dev = ctx->subs->dev; /* we need to set this at each time */
	urb->number_of_packets = 0;
	spin_lock_irqsave(&subs->lock, flags);
	for (i = 0; i < ctx->packets; i++) {
		/* calculate the size of a packet */
		if (subs->fill_max)
			counts = subs->maxframesize; /* fixed */
		else {
			subs->phase = (subs->phase & 0xffff) + subs->freqm;
			counts = subs->phase >> 16;
			if (counts > subs->maxframesize)
				counts = subs->maxframesize;
		}
		/* set up descriptor */
		urb->iso_frame_desc[i].offset = offs * stride;
		urb->iso_frame_desc[i].length = counts * stride;
		offs += counts;
		urb->number_of_packets++;
		subs->transfer_sched += counts;
		if (subs->transfer_sched >= runtime->period_size) {
			subs->transfer_sched -= runtime->period_size;
			if (subs->fmt_type == USB_FORMAT_TYPE_II) {
				if (subs->transfer_sched > 0) {
					/* FIXME: fill-max mode is not supported yet */
					offs -= subs->transfer_sched;
					counts -= subs->transfer_sched;
					urb->iso_frame_desc[i].length = counts * stride;
					subs->transfer_sched = 0;
				}
				i++;
				if (i < ctx->packets) {
					/* add a transfer delimiter */
					urb->iso_frame_desc[i].offset = offs * stride;
					urb->iso_frame_desc[i].length = 0;
					urb->number_of_packets++;
				}
			}
			break;
 		}
	}
	if (subs->hwptr + offs > runtime->buffer_size) {
		/* err, the transferred area goes over buffer boundary.
		 * copy the data to the temp buffer.
		 */
		int len;
		len = runtime->buffer_size - subs->hwptr;
		urb->transfer_buffer = subs->tmpbuf;
		memcpy(subs->tmpbuf, runtime->dma_area + subs->hwptr * stride, len * stride);
		memcpy(subs->tmpbuf + len * stride, runtime->dma_area, (offs - len) * stride);
		subs->hwptr += offs;
		subs->hwptr -= runtime->buffer_size;
	} else {
		/* set the buffer pointer */
		urb->transfer_buffer = runtime->dma_area + subs->hwptr * stride;
		subs->hwptr += offs;
	}
	spin_unlock_irqrestore(&subs->lock, flags);
	urb->transfer_buffer_length = offs * stride;
	ctx->transfer = offs;

	return 0;
}

/*
 * process after playback data complete
 *
 * update the current position and call callback if a period is processed.
 */
static int retire_playback_urb(snd_usb_substream_t *subs,
			       snd_pcm_runtime_t *runtime,
			       struct urb *urb)
{
	unsigned long flags;
	snd_urb_ctx_t *ctx = (snd_urb_ctx_t *)urb->context;

	spin_lock_irqsave(&subs->lock, flags);
	subs->transfer_done += ctx->transfer;
	subs->hwptr_done += ctx->transfer;
	ctx->transfer = 0;
	if (subs->hwptr_done >= runtime->buffer_size)
		subs->hwptr_done -= runtime->buffer_size;
	if (subs->transfer_done >= runtime->period_size) {
		subs->transfer_done -= runtime->period_size;
		spin_unlock_irqrestore(&subs->lock, flags);
		snd_pcm_period_elapsed(subs->pcm_substream);
	} else
		spin_unlock_irqrestore(&subs->lock, flags);
	return 0;
}


/*
 */
static struct snd_urb_ops audio_urb_ops[2] = {
	{
		.prepare =	prepare_playback_urb,
		.retire =	retire_playback_urb,
		.prepare_sync =	prepare_playback_sync_urb,
		.retire_sync =	retire_playback_sync_urb,
	},
	{
		.prepare =	prepare_capture_urb,
		.retire =	retire_capture_urb,
		.prepare_sync =	prepare_capture_sync_urb,
		.retire_sync =	retire_capture_sync_urb,
	},
};

static struct snd_urb_ops audio_urb_ops_high_speed[2] = {
	{
		.prepare =	prepare_playback_urb,
		.retire =	retire_playback_urb,
		.prepare_sync =	prepare_playback_sync_urb_hs,
		.retire_sync =	retire_playback_sync_urb_hs,
	},
	{
		.prepare =	prepare_capture_urb,
		.retire =	retire_capture_urb,
		.prepare_sync =	prepare_capture_sync_urb_hs,
		.retire_sync =	retire_capture_sync_urb,
	},
};

/*
 * complete callback from data urb
 */
static void snd_complete_urb(struct urb *urb, struct pt_regs *regs)
{
	snd_urb_ctx_t *ctx = (snd_urb_ctx_t *)urb->context;
	snd_usb_substream_t *subs = ctx->subs;
	snd_pcm_substream_t *substream = ctx->subs->pcm_substream;
	int err = 0;

	if ((subs->running && subs->ops.retire(subs, substream->runtime, urb)) ||
	    ! subs->running || /* can be stopped during retire callback */
	    (err = subs->ops.prepare(subs, substream->runtime, urb)) < 0 ||
	    (err = usb_submit_urb(urb, GFP_ATOMIC)) < 0) {
		clear_bit(ctx->index, &subs->active_mask);
		if (err < 0) {
			snd_printd(KERN_ERR "cannot submit urb (err = %d)\n", err);
			snd_pcm_stop(substream, SNDRV_PCM_STATE_XRUN);
		}
	}
}


/*
 * complete callback from sync urb
 */
static void snd_complete_sync_urb(struct urb *urb, struct pt_regs *regs)
{
	snd_urb_ctx_t *ctx = (snd_urb_ctx_t *)urb->context;
	snd_usb_substream_t *subs = ctx->subs;
	snd_pcm_substream_t *substream = ctx->subs->pcm_substream;
	int err = 0;

	if ((subs->running && subs->ops.retire_sync(subs, substream->runtime, urb)) ||
	    ! subs->running || /* can be stopped during retire callback */
	    (err = subs->ops.prepare_sync(subs, substream->runtime, urb)) < 0 ||
	    (err = usb_submit_urb(urb, GFP_ATOMIC)) < 0) {
		clear_bit(ctx->index + 16, &subs->active_mask);
		if (err < 0) {
			snd_printd(KERN_ERR "cannot submit sync urb (err = %d)\n", err);
			snd_pcm_stop(substream, SNDRV_PCM_STATE_XRUN);
		}
	}
}


/*
 * unlink active urbs.
 */
static int deactivate_urbs(snd_usb_substream_t *subs, int force, int can_sleep)
{
	unsigned int i;
	int async;

	subs->running = 0;

	if (!force && subs->stream->chip->shutdown) /* to be sure... */
		return 0;

	async = !can_sleep && async_unlink;

	if (! async && in_interrupt())
		return 0;

	for (i = 0; i < subs->nurbs; i++) {
		if (test_bit(i, &subs->active_mask)) {
			if (! test_and_set_bit(i, &subs->unlink_mask)) {
				struct urb *u = subs->dataurb[i].urb;
				if (async)
					u->transfer_flags |= URB_ASYNC_UNLINK;
				else
					u->transfer_flags &= ~URB_ASYNC_UNLINK;
				usb_unlink_urb(u);
			}
		}
	}
	if (subs->syncpipe) {
		for (i = 0; i < SYNC_URBS; i++) {
			if (test_bit(i+16, &subs->active_mask)) {
 				if (! test_and_set_bit(i+16, &subs->unlink_mask)) {
					struct urb *u = subs->syncurb[i].urb;
					if (async)
						u->transfer_flags |= URB_ASYNC_UNLINK;
					else
						u->transfer_flags &= ~URB_ASYNC_UNLINK;
					usb_unlink_urb(u);
				}
			}
		}
	}
	return 0;
}


/*
 * set up and start data/sync urbs
 */
static int start_urbs(snd_usb_substream_t *subs, snd_pcm_runtime_t *runtime)
{
	unsigned int i;
	int err;

	for (i = 0; i < subs->nurbs; i++) {
		snd_assert(subs->dataurb[i].urb, return -EINVAL);
		if (subs->ops.prepare(subs, runtime, subs->dataurb[i].urb) < 0) {
			snd_printk(KERN_ERR "cannot prepare datapipe for urb %d\n", i);
			goto __error;
		}
	}
	if (subs->syncpipe) {
		for (i = 0; i < SYNC_URBS; i++) {
			snd_assert(subs->syncurb[i].urb, return -EINVAL);
			if (subs->ops.prepare_sync(subs, runtime, subs->syncurb[i].urb) < 0) {
				snd_printk(KERN_ERR "cannot prepare syncpipe for urb %d\n", i);
				goto __error;
			}
		}
	}

	subs->active_mask = 0;
	subs->unlink_mask = 0;
	subs->running = 1;
	for (i = 0; i < subs->nurbs; i++) {
		if ((err = usb_submit_urb(subs->dataurb[i].urb, GFP_ATOMIC)) < 0) {
			snd_printk(KERN_ERR "cannot submit datapipe for urb %d, err = %d\n", i, err);
			goto __error;
		}
		set_bit(i, &subs->active_mask);
	}
	if (subs->syncpipe) {
		for (i = 0; i < SYNC_URBS; i++) {
			if ((err = usb_submit_urb(subs->syncurb[i].urb, GFP_ATOMIC)) < 0) {
				snd_printk(KERN_ERR "cannot submit syncpipe for urb %d, err = %d\n", i, err);
				goto __error;
			}
			set_bit(i + 16, &subs->active_mask);
		}
	}
	return 0;

 __error:
	// snd_pcm_stop(subs->pcm_substream, SNDRV_PCM_STATE_XRUN);
	deactivate_urbs(subs, 0, 0);
	return -EPIPE;
}


/* 
 *  wait until all urbs are processed.
 */
static int wait_clear_urbs(snd_usb_substream_t *subs)
{
	int timeout = HZ;
	unsigned int i;
	int alive;

	do {
		alive = 0;
		for (i = 0; i < subs->nurbs; i++) {
			if (test_bit(i, &subs->active_mask))
				alive++;
		}
		if (subs->syncpipe) {
			for (i = 0; i < SYNC_URBS; i++) {
				if (test_bit(i + 16, &subs->active_mask))
					alive++;
			}
		}
		if (! alive)
			break;
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(1);
	} while (--timeout > 0);
	if (alive)
		snd_printk(KERN_ERR "timeout: still %d active urbs..\n", alive);
	return 0;
}


/*
 * return the current pcm pointer.  just return the hwptr_done value.
 */
static snd_pcm_uframes_t snd_usb_pcm_pointer(snd_pcm_substream_t *substream)
{
	snd_usb_substream_t *subs = (snd_usb_substream_t *)substream->runtime->private_data;
	return subs->hwptr_done;
}


/*
 * start/stop substream
 */
static int snd_usb_pcm_trigger(snd_pcm_substream_t *substream, int cmd)
{
	snd_usb_substream_t *subs = (snd_usb_substream_t *)substream->runtime->private_data;
	int err;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		err = start_urbs(subs, substream->runtime);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		err = deactivate_urbs(subs, 0, 0);
		break;
	default:
		err = -EINVAL;
		break;
	}
	return err < 0 ? err : 0;
}


/*
 * release a urb data
 */
static void release_urb_ctx(snd_urb_ctx_t *u)
{
	if (u->urb) {
		usb_free_urb(u->urb);
		u->urb = NULL;
	}
	if (u->buf) {
		kfree(u->buf);
		u->buf = NULL;
	}
}

/*
 * release a substream
 */
static void release_substream_urbs(snd_usb_substream_t *subs, int force)
{
	int i;

	/* stop urbs (to be sure) */
	deactivate_urbs(subs, force, 1);
	wait_clear_urbs(subs);

	for (i = 0; i < MAX_URBS; i++)
		release_urb_ctx(&subs->dataurb[i]);
	for (i = 0; i < SYNC_URBS; i++)
		release_urb_ctx(&subs->syncurb[i]);
	if (subs->tmpbuf) {
		kfree(subs->tmpbuf);
		subs->tmpbuf = NULL;
	}
	subs->nurbs = 0;
}

/*
 * initialize a substream for plaback/capture
 */
static int init_substream_urbs(snd_usb_substream_t *subs, unsigned int period_bytes,
			       unsigned int rate, unsigned int frame_bits)
{
	unsigned int maxsize, n, i;
	int is_playback = subs->direction == SNDRV_PCM_STREAM_PLAYBACK;
	unsigned int npacks[MAX_URBS], urb_packs, total_packs;

	/* calculate the frequency in 16.16 format */
	if (snd_usb_get_speed(subs->dev) == USB_SPEED_FULL)
		subs->freqn = get_usb_full_speed_rate(rate);
	else
		subs->freqn = get_usb_high_speed_rate(rate);
	subs->freqm = subs->freqn;
	subs->freqmax = subs->freqn + (subs->freqn >> 2); /* max. allowed frequency */
	subs->phase = 0;

	/* calculate the max. size of packet */
	maxsize = ((subs->freqmax + 0xffff) * (frame_bits >> 3)) >> 16;
	if (subs->maxpacksize && maxsize > subs->maxpacksize) {
		//snd_printd(KERN_DEBUG "maxsize %d is greater than defined size %d\n",
		//	   maxsize, subs->maxpacksize);
		maxsize = subs->maxpacksize;
	}

	if (subs->fill_max)
		subs->curpacksize = subs->maxpacksize;
	else
		subs->curpacksize = maxsize;

	if (snd_usb_get_speed(subs->dev) == USB_SPEED_FULL)
		urb_packs = nrpacks;
	else
		urb_packs = nrpacks * 8;

	/* allocate a temporary buffer for playback */
	if (is_playback) {
		subs->tmpbuf = kmalloc(maxsize * urb_packs, GFP_KERNEL);
		if (! subs->tmpbuf) {
			snd_printk(KERN_ERR "cannot malloc tmpbuf\n");
			return -ENOMEM;
		}
	}

	/* decide how many packets to be used */
	total_packs = (period_bytes + maxsize - 1) / maxsize;
	if (total_packs < 2 * MIN_PACKS_URB)
		total_packs = 2 * MIN_PACKS_URB;
	subs->nurbs = (total_packs + urb_packs - 1) / urb_packs;
	if (subs->nurbs > MAX_URBS) {
		/* too much... */
		subs->nurbs = MAX_URBS;
		total_packs = MAX_URBS * urb_packs;
	}
	n = total_packs;
	for (i = 0; i < subs->nurbs; i++) {
		npacks[i] = n > urb_packs ? urb_packs : n;
		n -= urb_packs;
	}
	if (subs->nurbs <= 1) {
		/* too little - we need at least two packets
		 * to ensure contiguous playback/capture
		 */
		subs->nurbs = 2;
		npacks[0] = (total_packs + 1) / 2;
		npacks[1] = total_packs - npacks[0];
	} else if (npacks[subs->nurbs-1] < MIN_PACKS_URB) {
		/* the last packet is too small.. */
		if (subs->nurbs > 2) {
			/* merge to the first one */
			npacks[0] += npacks[subs->nurbs - 1];
			subs->nurbs--;
		} else {
			/* divide to two */
			subs->nurbs = 2;
			npacks[0] = (total_packs + 1) / 2;
			npacks[1] = total_packs - npacks[0];
		}
	}

	/* allocate and initialize data urbs */
	for (i = 0; i < subs->nurbs; i++) {
		snd_urb_ctx_t *u = &subs->dataurb[i];
		u->index = i;
		u->subs = subs;
		u->transfer = 0;
		u->packets = npacks[i];
		if (subs->fmt_type == USB_FORMAT_TYPE_II)
			u->packets++; /* for transfer delimiter */
		if (! is_playback) {
			/* allocate a capture buffer per urb */
			u->buf = kmalloc(maxsize * u->packets, GFP_KERNEL);
			if (! u->buf) {
				release_substream_urbs(subs, 0);
				return -ENOMEM;
			}
		}
		u->urb = usb_alloc_urb(u->packets, GFP_KERNEL);
		if (! u->urb) {
			release_substream_urbs(subs, 0);
			return -ENOMEM;
		}
		u->urb->dev = subs->dev;
		u->urb->pipe = subs->datapipe;
		u->urb->transfer_flags = URB_ISO_ASAP;
		u->urb->number_of_packets = u->packets;
		u->urb->interval = 1;
		u->urb->context = u;
		u->urb->complete = snd_usb_complete_callback(snd_complete_urb);
	}

	if (subs->syncpipe) {
		/* allocate and initialize sync urbs */
		for (i = 0; i < SYNC_URBS; i++) {
			snd_urb_ctx_t *u = &subs->syncurb[i];
			u->index = i;
			u->subs = subs;
			u->packets = nrpacks;
			u->urb = usb_alloc_urb(u->packets, GFP_KERNEL);
			if (! u->urb) {
				release_substream_urbs(subs, 0);
				return -ENOMEM;
			}
			u->urb->transfer_buffer = subs->syncbuf + i * nrpacks * 4;
			u->urb->transfer_buffer_length = nrpacks * 4;
			u->urb->dev = subs->dev;
			u->urb->pipe = subs->syncpipe;
			u->urb->transfer_flags = URB_ISO_ASAP;
			u->urb->number_of_packets = u->packets;
			if (snd_usb_get_speed(subs->dev) == USB_SPEED_HIGH)
				u->urb->interval = 8;
			else
				u->urb->interval = 1;
			u->urb->context = u;
			u->urb->complete = snd_usb_complete_callback(snd_complete_sync_urb);
		}
	}
	return 0;
}


/*
 * find a matching audio format
 */
static struct audioformat *find_format(snd_usb_substream_t *subs, unsigned int format,
				       unsigned int rate, unsigned int channels)
{
	struct list_head *p;
	struct audioformat *found = NULL;
	int cur_attr = 0, attr;

	list_for_each(p, &subs->fmt_list) {
		struct audioformat *fp;
		fp = list_entry(p, struct audioformat, list);
		if (fp->format != format || fp->channels != channels)
			continue;
		if (rate < fp->rate_min || rate > fp->rate_max)
			continue;
		if (! (fp->rates & SNDRV_PCM_RATE_CONTINUOUS)) {
			unsigned int i;
			for (i = 0; i < fp->nr_rates; i++)
				if (fp->rate_table[i] == rate)
					break;
			if (i >= fp->nr_rates)
				continue;
		}
		attr = fp->ep_attr & EP_ATTR_MASK;
		if (! found) {
			found = fp;
			cur_attr = attr;
			continue;
		}
		/* avoid async out and adaptive in if the other method
		 * supports the same format.
		 * this is a workaround for the case like
		 * M-audio audiophile USB.
		 */
		if (attr != cur_attr) {
			if ((attr == EP_ATTR_ASYNC &&
			     subs->direction == SNDRV_PCM_STREAM_PLAYBACK) ||
			    (attr == EP_ATTR_ADAPTIVE &&
			     subs->direction == SNDRV_PCM_STREAM_CAPTURE))
				continue;
			if ((cur_attr == EP_ATTR_ASYNC &&
			     subs->direction == SNDRV_PCM_STREAM_PLAYBACK) ||
			    (cur_attr == EP_ATTR_ADAPTIVE &&
			     subs->direction == SNDRV_PCM_STREAM_CAPTURE)) {
				found = fp;
				cur_attr = attr;
				continue;
			}
		}
		/* find the format with the largest max. packet size */
		if (fp->maxpacksize > found->maxpacksize) {
			found = fp;
			cur_attr = attr;
		}
	}
	return found;
}


/*
 * initialize the picth control and sample rate
 */
static int init_usb_pitch(struct usb_device *dev, int iface,
			  struct usb_host_interface *alts,
			  struct audioformat *fmt)
{
	unsigned int ep;
	unsigned char data[1];
	int err;

	ep = get_endpoint(alts, 0)->bEndpointAddress;
	/* if endpoint has pitch control, enable it */
	if (fmt->attributes & EP_CS_ATTR_PITCH_CONTROL) {
		data[0] = 1;
		if ((err = snd_usb_ctl_msg(dev, usb_sndctrlpipe(dev, 0), SET_CUR,
					   USB_TYPE_CLASS|USB_RECIP_ENDPOINT|USB_DIR_OUT, 
					   PITCH_CONTROL << 8, ep, data, 1, HZ)) < 0) {
			snd_printk(KERN_ERR "%d:%d:%d: cannot set enable PITCH\n",
				   dev->devnum, iface, ep);
			return err;
		}
	}
	return 0;
}

static int init_usb_sample_rate(struct usb_device *dev, int iface,
				struct usb_host_interface *alts,
				struct audioformat *fmt, int rate)
{
	unsigned int ep;
	unsigned char data[3];
	int err;

	ep = get_endpoint(alts, 0)->bEndpointAddress;
	/* if endpoint has sampling rate control, set it */
	if (fmt->attributes & EP_CS_ATTR_SAMPLE_RATE) {
		int crate;
		data[0] = rate;
		data[1] = rate >> 8;
		data[2] = rate >> 16;
		if ((err = snd_usb_ctl_msg(dev, usb_sndctrlpipe(dev, 0), SET_CUR,
					   USB_TYPE_CLASS|USB_RECIP_ENDPOINT|USB_DIR_OUT, 
					   SAMPLING_FREQ_CONTROL << 8, ep, data, 3, HZ)) < 0) {
			snd_printk(KERN_ERR "%d:%d:%d: cannot set freq %d to ep 0x%x\n",
				   dev->devnum, iface, fmt->altsetting, rate, ep);
			return err;
		}
		if ((err = snd_usb_ctl_msg(dev, usb_rcvctrlpipe(dev, 0), GET_CUR,
					   USB_TYPE_CLASS|USB_RECIP_ENDPOINT|USB_DIR_IN,
					   SAMPLING_FREQ_CONTROL << 8, ep, data, 3, HZ)) < 0) {
			snd_printk(KERN_ERR "%d:%d:%d: cannot get freq at ep 0x%x\n",
				   dev->devnum, iface, fmt->altsetting, ep);
			return err;
		}
		crate = data[0] | (data[1] << 8) | (data[2] << 16);
		if (crate != rate) {
			snd_printd(KERN_WARNING "current rate %d is different from the runtime rate %d\n", crate, rate);
			// runtime->rate = crate;
		}
	}
	return 0;
}

/*
 * find a matching format and set up the interface
 */
static int set_format(snd_usb_substream_t *subs, struct audioformat *fmt)
{
	struct usb_device *dev = subs->dev;
	struct usb_host_interface *alts;
	struct usb_interface_descriptor *altsd;
	struct usb_interface *iface;
	unsigned int ep, attr;
	int is_playback = subs->direction == SNDRV_PCM_STREAM_PLAYBACK;
	int err;

	iface = usb_ifnum_to_if(dev, fmt->iface);
	snd_assert(iface, return -EINVAL);
	alts = &iface->altsetting[fmt->altset_idx];
	altsd = get_iface_desc(alts);
	snd_assert(altsd->bAlternateSetting == fmt->altsetting, return -EINVAL);

	if (fmt == subs->cur_audiofmt)
		return 0;

	/* close the old interface */
	if (subs->interface >= 0 && subs->interface != fmt->iface) {
		usb_set_interface(subs->dev, subs->interface, 0);
		subs->interface = -1;
		subs->format = 0;
	}

	/* set interface */
	if (subs->interface != fmt->iface || subs->format != fmt->altset_idx) {
		if (usb_set_interface(dev, fmt->iface, fmt->altsetting) < 0) {
			snd_printk(KERN_ERR "%d:%d:%d: usb_set_interface failed\n",
				   dev->devnum, fmt->iface, fmt->altsetting);
			return -EIO;
		}
		snd_printdd(KERN_INFO "setting usb interface %d:%d\n", fmt->iface, fmt->altsetting);
		subs->interface = fmt->iface;
		subs->format = fmt->altset_idx;
	}

	/* create a data pipe */
	ep = fmt->endpoint & USB_ENDPOINT_NUMBER_MASK;
	if (is_playback)
		subs->datapipe = usb_sndisocpipe(dev, ep);
	else
		subs->datapipe = usb_rcvisocpipe(dev, ep);
	subs->syncpipe = subs->syncinterval = 0;
	subs->maxpacksize = fmt->maxpacksize;
	subs->fill_max = 0;

	/* we need a sync pipe in async OUT or adaptive IN mode */
	/* check the number of EP, since some devices have broken
	 * descriptors which fool us.  if it has only one EP,
	 * assume it as adaptive-out or sync-in.
	 */
	attr = fmt->ep_attr & EP_ATTR_MASK;
	if (((is_playback && attr == EP_ATTR_ASYNC) ||
	     (! is_playback && attr == EP_ATTR_ADAPTIVE)) &&
	    altsd->bNumEndpoints >= 2) {
		/* check sync-pipe endpoint */
		/* ... and check descriptor size before accessing bSynchAddress
		   because there is a version of the SB Audigy 2 NX firmware lacking
		   the audio fields in the endpoint descriptors */
		if ((get_endpoint(alts, 1)->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) != 0x01 ||
		    (get_endpoint(alts, 1)->bLength >= USB_DT_ENDPOINT_AUDIO_SIZE &&
		     get_endpoint(alts, 1)->bSynchAddress != 0)) {
			snd_printk(KERN_ERR "%d:%d:%d : invalid synch pipe\n",
				   dev->devnum, fmt->iface, fmt->altsetting);
			return -EINVAL;
		}
		ep = get_endpoint(alts, 1)->bEndpointAddress;
		if (get_endpoint(alts, 0)->bLength >= USB_DT_ENDPOINT_AUDIO_SIZE &&
		    (( is_playback && ep != (unsigned int)(get_endpoint(alts, 0)->bSynchAddress | USB_DIR_IN)) ||
		     (!is_playback && ep != (unsigned int)(get_endpoint(alts, 0)->bSynchAddress & ~USB_DIR_IN)))) {
			snd_printk(KERN_ERR "%d:%d:%d : invalid synch pipe\n",
				   dev->devnum, fmt->iface, fmt->altsetting);
			return -EINVAL;
		}
		ep &= USB_ENDPOINT_NUMBER_MASK;
		if (is_playback)
			subs->syncpipe = usb_rcvisocpipe(dev, ep);
		else
			subs->syncpipe = usb_sndisocpipe(dev, ep);
		subs->syncinterval = get_endpoint(alts, 1)->bRefresh;
	}

	/* always fill max packet size */
	if (fmt->attributes & EP_CS_ATTR_FILL_MAX)
		subs->fill_max = 1;

	if ((err = init_usb_pitch(dev, subs->interface, alts, fmt)) < 0)
		return err;

	subs->cur_audiofmt = fmt;

#if 0
	printk("setting done: format = %d, rate = %d, channels = %d\n",
	       fmt->format, fmt->rate, fmt->channels);
	printk("  datapipe = 0x%0x, syncpipe = 0x%0x\n",
	       subs->datapipe, subs->syncpipe);
#endif

	return 0;
}

/*
 * hw_params callback
 *
 * allocate a buffer and set the given audio format.
 *
 * so far we use a physically linear buffer although packetize transfer
 * doesn't need a continuous area.
 * if sg buffer is supported on the later version of alsa, we'll follow
 * that.
 */
static int snd_usb_hw_params(snd_pcm_substream_t *substream,
			     snd_pcm_hw_params_t *hw_params)
{
	snd_usb_substream_t *subs = (snd_usb_substream_t *)substream->runtime->private_data;
	struct audioformat *fmt;
	unsigned int channels, rate, format;
	int ret, changed;

	ret = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
	if (ret < 0)
		return ret;
	
	format = params_format(hw_params);
	rate = params_rate(hw_params);
	channels = params_channels(hw_params);
	fmt = find_format(subs, format, rate, channels);
	if (! fmt) {
		snd_printd(KERN_DEBUG "cannot set format: format = %s, rate = %d, channels = %d\n",
			   snd_pcm_format_name(format), rate, channels);
		return -EINVAL;
	}

	changed = subs->cur_audiofmt != fmt ||
		subs->period_bytes != params_period_bytes(hw_params) ||
		subs->cur_rate != rate;
	if ((ret = set_format(subs, fmt)) < 0)
		return ret;

	if (subs->cur_rate != rate) {
		struct usb_host_interface *alts;
		struct usb_interface *iface;
		iface = usb_ifnum_to_if(subs->dev, fmt->iface);
		alts = &iface->altsetting[fmt->altset_idx];
		ret = init_usb_sample_rate(subs->dev, subs->interface, alts, fmt, rate);
		if (ret < 0)
			return ret;
		subs->cur_rate = rate;
	}

	if (changed) {
		/* format changed */
		release_substream_urbs(subs, 0);
		/* influenced: period_bytes, channels, rate, format, */
		ret = init_substream_urbs(subs, params_period_bytes(hw_params),
					  params_rate(hw_params),
					  snd_pcm_format_physical_width(params_format(hw_params)) * params_channels(hw_params));
	}

	return ret;
}

/*
 * hw_free callback
 *
 * reset the audio format and release the buffer
 */
static int snd_usb_hw_free(snd_pcm_substream_t *substream)
{
	snd_usb_substream_t *subs = (snd_usb_substream_t *)substream->runtime->private_data;

	subs->cur_audiofmt = NULL;
	subs->cur_rate = 0;
	subs->period_bytes = 0;
	release_substream_urbs(subs, 0);
	return snd_pcm_lib_free_pages(substream);
}

/*
 * prepare callback
 *
 * only a few subtle things...
 */
static int snd_usb_pcm_prepare(snd_pcm_substream_t *substream)
{
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_usb_substream_t *subs = (snd_usb_substream_t *)runtime->private_data;

	if (! subs->cur_audiofmt) {
		snd_printk(KERN_ERR "usbaudio: no format is specified!\n");
		return -ENXIO;
	}

	/* some unit conversions in runtime */
	subs->maxframesize = bytes_to_frames(runtime, subs->maxpacksize);
	subs->curframesize = bytes_to_frames(runtime, subs->curpacksize);

	/* reset the pointer */
	subs->hwptr = 0;
	subs->hwptr_done = 0;
	subs->transfer_sched = 0;
	subs->transfer_done = 0;
	subs->phase = 0;

	/* clear urbs (to be sure) */
	deactivate_urbs(subs, 0, 1);
	wait_clear_urbs(subs);

	return 0;
}

static snd_pcm_hardware_t snd_usb_playback =
{
	.info =			(SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_BLOCK_TRANSFER |
				 SNDRV_PCM_INFO_MMAP_VALID),
	.buffer_bytes_max =	(128*1024),
	.period_bytes_min =	64,
	.period_bytes_max =	(128*1024),
	.periods_min =		2,
	.periods_max =		1024,
};

static snd_pcm_hardware_t snd_usb_capture =
{
	.info =			(SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_BLOCK_TRANSFER |
				 SNDRV_PCM_INFO_MMAP_VALID),
	.buffer_bytes_max =	(128*1024),
	.period_bytes_min =	64,
	.period_bytes_max =	(128*1024),
	.periods_min =		2,
	.periods_max =		1024,
};

/*
 * h/w constraints
 */

#ifdef HW_CONST_DEBUG
#define hwc_debug(fmt, args...) printk(KERN_DEBUG fmt, ##args)
#else
#define hwc_debug(fmt, args...) /**/
#endif

static int hw_check_valid_format(snd_pcm_hw_params_t *params, struct audioformat *fp)
{
	snd_interval_t *it = hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	snd_interval_t *ct = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	snd_mask_t *fmts = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	/* check the format */
	if (! snd_mask_test(fmts, fp->format)) {
		hwc_debug("   > check: no supported format %d\n", fp->format);
		return 0;
	}
	/* check the channels */
	if (fp->channels < ct->min || fp->channels > ct->max) {
		hwc_debug("   > check: no valid channels %d (%d/%d)\n", fp->channels, ct->min, ct->max);
		return 0;
	}
	/* check the rate is within the range */
	if (fp->rate_min > it->max || (fp->rate_min == it->max && it->openmax)) {
		hwc_debug("   > check: rate_min %d > max %d\n", fp->rate_min, it->max);
		return 0;
	}
	if (fp->rate_max < it->min || (fp->rate_max == it->min && it->openmin)) {
		hwc_debug("   > check: rate_max %d < min %d\n", fp->rate_max, it->min);
		return 0;
	}
	return 1;
}

static int hw_rule_rate(snd_pcm_hw_params_t *params,
			snd_pcm_hw_rule_t *rule)
{
	snd_usb_substream_t *subs = rule->private;
	struct list_head *p;
	snd_interval_t *it = hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	unsigned int rmin, rmax;
	int changed;
	
	hwc_debug("hw_rule_rate: (%d,%d)\n", it->min, it->max);
	changed = 0;
	rmin = rmax = 0;
	list_for_each(p, &subs->fmt_list) {
		struct audioformat *fp;
		fp = list_entry(p, struct audioformat, list);
		if (! hw_check_valid_format(params, fp))
			continue;
		if (changed++) {
			if (rmin > fp->rate_min)
				rmin = fp->rate_min;
			if (rmax < fp->rate_max)
				rmax = fp->rate_max;
		} else {
			rmin = fp->rate_min;
			rmax = fp->rate_max;
		}
	}

	if (! changed) {
		hwc_debug("  --> get empty\n");
		it->empty = 1;
		return -EINVAL;
	}

	changed = 0;
	if (it->min < rmin) {
		it->min = rmin;
		it->openmin = 0;
		changed = 1;
	}
	if (it->max > rmax) {
		it->max = rmax;
		it->openmax = 0;
		changed = 1;
	}
	if (snd_interval_checkempty(it)) {
		it->empty = 1;
		return -EINVAL;
	}
	hwc_debug("  --> (%d, %d) (changed = %d)\n", it->min, it->max, changed);
	return changed;
}


static int hw_rule_channels(snd_pcm_hw_params_t *params,
			    snd_pcm_hw_rule_t *rule)
{
	snd_usb_substream_t *subs = rule->private;
	struct list_head *p;
	snd_interval_t *it = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	unsigned int rmin, rmax;
	int changed;
	
	hwc_debug("hw_rule_channels: (%d,%d)\n", it->min, it->max);
	changed = 0;
	rmin = rmax = 0;
	list_for_each(p, &subs->fmt_list) {
		struct audioformat *fp;
		fp = list_entry(p, struct audioformat, list);
		if (! hw_check_valid_format(params, fp))
			continue;
		if (changed++) {
			if (rmin > fp->channels)
				rmin = fp->channels;
			if (rmax < fp->channels)
				rmax = fp->channels;
		} else {
			rmin = fp->channels;
			rmax = fp->channels;
		}
	}

	if (! changed) {
		hwc_debug("  --> get empty\n");
		it->empty = 1;
		return -EINVAL;
	}

	changed = 0;
	if (it->min < rmin) {
		it->min = rmin;
		it->openmin = 0;
		changed = 1;
	}
	if (it->max > rmax) {
		it->max = rmax;
		it->openmax = 0;
		changed = 1;
	}
	if (snd_interval_checkempty(it)) {
		it->empty = 1;
		return -EINVAL;
	}
	hwc_debug("  --> (%d, %d) (changed = %d)\n", it->min, it->max, changed);
	return changed;
}

static int hw_rule_format(snd_pcm_hw_params_t *params,
			  snd_pcm_hw_rule_t *rule)
{
	snd_usb_substream_t *subs = rule->private;
	struct list_head *p;
	snd_mask_t *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);
	u64 fbits;
	u32 oldbits[2];
	int changed;
	
	hwc_debug("hw_rule_format: %x:%x\n", fmt->bits[0], fmt->bits[1]);
	fbits = 0;
	list_for_each(p, &subs->fmt_list) {
		struct audioformat *fp;
		fp = list_entry(p, struct audioformat, list);
		if (! hw_check_valid_format(params, fp))
			continue;
		fbits |= (1ULL << fp->format);
	}

	oldbits[0] = fmt->bits[0];
	oldbits[1] = fmt->bits[1];
	fmt->bits[0] &= (u32)fbits;
	fmt->bits[1] &= (u32)(fbits >> 32);
	if (! fmt->bits[0] && ! fmt->bits[1]) {
		hwc_debug("  --> get empty\n");
		return -EINVAL;
	}
	changed = (oldbits[0] != fmt->bits[0] || oldbits[1] != fmt->bits[1]);
	hwc_debug("  --> %x:%x (changed = %d)\n", fmt->bits[0], fmt->bits[1], changed);
	return changed;
}

/*
 * check whether the registered audio formats need special hw-constraints
 */
static int check_hw_params_convention(snd_usb_substream_t *subs)
{
	int i;
	u32 channels[64];
	u32 rates[64];
	u32 cmaster, rmaster;
	u32 rate_min = 0, rate_max = 0;
	struct list_head *p;

	memset(channels, 0, sizeof(channels));
	memset(rates, 0, sizeof(rates));

	list_for_each(p, &subs->fmt_list) {
		struct audioformat *f;
		f = list_entry(p, struct audioformat, list);
		/* unconventional channels? */
		if (f->channels > 32)
			return 1;
		/* continuous rate min/max matches? */
		if (f->rates & SNDRV_PCM_RATE_CONTINUOUS) {
			if (rate_min && f->rate_min != rate_min)
				return 1;
			if (rate_max && f->rate_max != rate_max)
				return 1;
			rate_min = f->rate_min;
			rate_max = f->rate_max;
		}
		/* combination of continuous rates and fixed rates? */
		if (rates[f->format] & SNDRV_PCM_RATE_CONTINUOUS) {
			if (f->rates != rates[f->format])
				return 1;
		}
		if (f->rates & SNDRV_PCM_RATE_CONTINUOUS) {
			if (rates[f->format] && rates[f->format] != f->rates)
				return 1;
		}
		channels[f->format] |= (1 << f->channels);
		rates[f->format] |= f->rates;
	}
	/* check whether channels and rates match for all formats */
	cmaster = rmaster = 0;
	for (i = 0; i < 64; i++) {
		if (cmaster != channels[i] && cmaster && channels[i])
			return 1;
		if (rmaster != rates[i] && rmaster && rates[i])
			return 1;
		if (channels[i])
			cmaster = channels[i];
		if (rates[i])
			rmaster = rates[i];
	}
	/* check whether channels match for all distinct rates */
	memset(channels, 0, sizeof(channels));
	list_for_each(p, &subs->fmt_list) {
		struct audioformat *f;
		f = list_entry(p, struct audioformat, list);
		if (f->rates & SNDRV_PCM_RATE_CONTINUOUS)
			continue;
		for (i = 0; i < 32; i++) {
			if (f->rates & (1 << i))
				channels[i] |= (1 << f->channels);
		}
	}
	cmaster = 0;
	for (i = 0; i < 32; i++) {
		if (cmaster != channels[i] && cmaster && channels[i])
			return 1;
		if (channels[i])
			cmaster = channels[i];
	}
	return 0;
}


/*
 * set up the runtime hardware information.
 */

static int setup_hw_info(snd_pcm_runtime_t *runtime, snd_usb_substream_t *subs)
{
	struct list_head *p;
	int err;

	runtime->hw.formats = subs->formats;

	runtime->hw.rate_min = 0x7fffffff;
	runtime->hw.rate_max = 0;
	runtime->hw.channels_min = 256;
	runtime->hw.channels_max = 0;
	runtime->hw.rates = 0;
	/* check min/max rates and channels */
	list_for_each(p, &subs->fmt_list) {
		struct audioformat *fp;
		fp = list_entry(p, struct audioformat, list);
		runtime->hw.rates |= fp->rates;
		if (runtime->hw.rate_min > fp->rate_min)
			runtime->hw.rate_min = fp->rate_min;
		if (runtime->hw.rate_max < fp->rate_max)
			runtime->hw.rate_max = fp->rate_max;
		if (runtime->hw.channels_min > fp->channels)
			runtime->hw.channels_min = fp->channels;
		if (runtime->hw.channels_max < fp->channels)
			runtime->hw.channels_max = fp->channels;
		if (fp->fmt_type == USB_FORMAT_TYPE_II && fp->frame_size > 0) {
			/* FIXME: there might be more than one audio formats... */
			runtime->hw.period_bytes_min = runtime->hw.period_bytes_max =
				fp->frame_size;
		}
	}

	/* set the period time minimum 1ms */
	snd_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_PERIOD_TIME,
				     1000 * MIN_PACKS_URB,
				     /*(nrpacks * MAX_URBS) * 1000*/ UINT_MAX);

	if (check_hw_params_convention(subs)) {
		hwc_debug("setting extra hw constraints...\n");
		if ((err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_RATE, 
					       hw_rule_rate, subs,
					       SNDRV_PCM_HW_PARAM_FORMAT,
					       SNDRV_PCM_HW_PARAM_CHANNELS,
					       -1)) < 0)
			return err;
		if ((err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS, 
					       hw_rule_channels, subs,
					       SNDRV_PCM_HW_PARAM_FORMAT,
					       SNDRV_PCM_HW_PARAM_RATE,
					       -1)) < 0)
			return err;
		if ((err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_FORMAT,
					       hw_rule_format, subs,
					       SNDRV_PCM_HW_PARAM_RATE,
					       SNDRV_PCM_HW_PARAM_CHANNELS,
					       -1)) < 0)
			return err;
	}
	return 0;
}

static int snd_usb_pcm_open(snd_pcm_substream_t *substream, int direction,
			    snd_pcm_hardware_t *hw)
{
	snd_usb_stream_t *as = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_usb_substream_t *subs = &as->substream[direction];

	subs->interface = -1;
	subs->format = 0;
	runtime->hw = *hw;
	runtime->private_data = subs;
	subs->pcm_substream = substream;
	return setup_hw_info(runtime, subs);
}

static int snd_usb_pcm_close(snd_pcm_substream_t *substream, int direction)
{
	snd_usb_stream_t *as = snd_pcm_substream_chip(substream);
	snd_usb_substream_t *subs = &as->substream[direction];

	if (subs->interface >= 0) {
		usb_set_interface(subs->dev, subs->interface, 0);
		subs->interface = -1;
	}
	subs->pcm_substream = NULL;
	return 0;
}

static int snd_usb_playback_open(snd_pcm_substream_t *substream)
{
	return snd_usb_pcm_open(substream, SNDRV_PCM_STREAM_PLAYBACK, &snd_usb_playback);
}

static int snd_usb_playback_close(snd_pcm_substream_t *substream)
{
	return snd_usb_pcm_close(substream, SNDRV_PCM_STREAM_PLAYBACK);
}

static int snd_usb_capture_open(snd_pcm_substream_t *substream)
{
	return snd_usb_pcm_open(substream, SNDRV_PCM_STREAM_CAPTURE, &snd_usb_capture);
}

static int snd_usb_capture_close(snd_pcm_substream_t *substream)
{
	return snd_usb_pcm_close(substream, SNDRV_PCM_STREAM_CAPTURE);
}

static snd_pcm_ops_t snd_usb_playback_ops = {
	.open =		snd_usb_playback_open,
	.close =	snd_usb_playback_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_usb_hw_params,
	.hw_free =	snd_usb_hw_free,
	.prepare =	snd_usb_pcm_prepare,
	.trigger =	snd_usb_pcm_trigger,
	.pointer =	snd_usb_pcm_pointer,
};

static snd_pcm_ops_t snd_usb_capture_ops = {
	.open =		snd_usb_capture_open,
	.close =	snd_usb_capture_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_usb_hw_params,
	.hw_free =	snd_usb_hw_free,
	.prepare =	snd_usb_pcm_prepare,
	.trigger =	snd_usb_pcm_trigger,
	.pointer =	snd_usb_pcm_pointer,
};



/*
 * helper functions
 */

/*
 * combine bytes and get an integer value
 */
unsigned int snd_usb_combine_bytes(unsigned char *bytes, int size)
{
	switch (size) {
	case 1:  return *bytes;
	case 2:  return combine_word(bytes);
	case 3:  return combine_triple(bytes);
	case 4:  return combine_quad(bytes);
	default: return 0;
	}
}

/*
 * parse descriptor buffer and return the pointer starting the given
 * descriptor type.
 */
void *snd_usb_find_desc(void *descstart, int desclen, void *after, u8 dtype)
{
	u8 *p, *end, *next;

	p = descstart;
	end = p + desclen;
	for (; p < end;) {
		if (p[0] < 2)
			return NULL;
		next = p + p[0];
		if (next > end)
			return NULL;
		if (p[1] == dtype && (!after || (void *)p > after)) {
			return p;
		}
		p = next;
	}
	return NULL;
}

/*
 * find a class-specified interface descriptor with the given subtype.
 */
void *snd_usb_find_csint_desc(void *buffer, int buflen, void *after, u8 dsubtype)
{
	unsigned char *p = after;

	while ((p = snd_usb_find_desc(buffer, buflen, p,
				      USB_DT_CS_INTERFACE)) != NULL) {
		if (p[0] >= 3 && p[2] == dsubtype)
			return p;
	}
	return NULL;
}

/*
 * Wrapper for usb_control_msg().
 * Allocates a temp buffer to prevent dmaing from/to the stack.
 */
int snd_usb_ctl_msg(struct usb_device *dev, unsigned int pipe, __u8 request,
		    __u8 requesttype, __u16 value, __u16 index, void *data,
		    __u16 size, int timeout)
{
	int err;
	void *buf = NULL;

	if (size > 0) {
		buf = kmalloc(size, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		memcpy(buf, data, size);
	}
	err = usb_control_msg(dev, pipe, request, requesttype,
			      value, index, buf, size, timeout);
	if (size > 0) {
		memcpy(data, buf, size);
		kfree(buf);
	}
	return err;
}


/*
 * entry point for linux usb interface
 */

static int usb_audio_probe(struct usb_interface *intf,
			   const struct usb_device_id *id);
static void usb_audio_disconnect(struct usb_interface *intf);

static struct usb_device_id usb_audio_ids [] = {
#include "usbquirks.h"
    { .match_flags = (USB_DEVICE_ID_MATCH_INT_CLASS | USB_DEVICE_ID_MATCH_INT_SUBCLASS),
      .bInterfaceClass = USB_CLASS_AUDIO,
      .bInterfaceSubClass = USB_SUBCLASS_AUDIO_CONTROL },
    { }						/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, usb_audio_ids);

static struct usb_driver usb_audio_driver = {
	.owner =	THIS_MODULE,
	.name =		"snd-usb-audio",
	.probe =	usb_audio_probe,
	.disconnect =	usb_audio_disconnect,
	.id_table =	usb_audio_ids,
};


/*
 * proc interface for list the supported pcm formats
 */
static void proc_dump_substream_formats(snd_usb_substream_t *subs, snd_info_buffer_t *buffer)
{
	struct list_head *p;
	static char *sync_types[4] = {
		"NONE", "ASYNC", "ADAPTIVE", "SYNC"
	};

	list_for_each(p, &subs->fmt_list) {
		struct audioformat *fp;
		fp = list_entry(p, struct audioformat, list);
		snd_iprintf(buffer, "  Interface %d\n", fp->iface);
		snd_iprintf(buffer, "    Altset %d\n", fp->altsetting);
		snd_iprintf(buffer, "    Format: %s\n", snd_pcm_format_name(fp->format));
		snd_iprintf(buffer, "    Channels: %d\n", fp->channels);
		snd_iprintf(buffer, "    Endpoint: %d %s (%s)\n",
			    fp->endpoint & USB_ENDPOINT_NUMBER_MASK,
			    fp->endpoint & USB_DIR_IN ? "IN" : "OUT",
			    sync_types[(fp->ep_attr & EP_ATTR_MASK) >> 2]);
		if (fp->rates & SNDRV_PCM_RATE_CONTINUOUS) {
			snd_iprintf(buffer, "    Rates: %d - %d (continuous)\n",
				    fp->rate_min, fp->rate_max);
		} else {
			unsigned int i;
			snd_iprintf(buffer, "    Rates: ");
			for (i = 0; i < fp->nr_rates; i++) {
				if (i > 0)
					snd_iprintf(buffer, ", ");
				snd_iprintf(buffer, "%d", fp->rate_table[i]);
			}
			snd_iprintf(buffer, "\n");
		}
		// snd_iprintf(buffer, "    Max Packet Size = %d\n", fp->maxpacksize);
		// snd_iprintf(buffer, "    EP Attribute = 0x%x\n", fp->attributes);
	}
}

static void proc_dump_substream_status(snd_usb_substream_t *subs, snd_info_buffer_t *buffer)
{
	if (subs->running) {
		unsigned int i;
		snd_iprintf(buffer, "  Status: Running\n");
		snd_iprintf(buffer, "    Interface = %d\n", subs->interface);
		snd_iprintf(buffer, "    Altset = %d\n", subs->format);
		snd_iprintf(buffer, "    URBs = %d [ ", subs->nurbs);
		for (i = 0; i < subs->nurbs; i++)
			snd_iprintf(buffer, "%d ", subs->dataurb[i].packets);
		snd_iprintf(buffer, "]\n");
		snd_iprintf(buffer, "    Packet Size = %d\n", subs->curpacksize);
		snd_iprintf(buffer, "    Momentary freq = %u Hz\n",
			    snd_usb_get_speed(subs->dev) == USB_SPEED_FULL
			    ? get_full_speed_hz(subs->freqm)
			    : get_high_speed_hz(subs->freqm));
	} else {
		snd_iprintf(buffer, "  Status: Stop\n");
	}
}

static void proc_pcm_format_read(snd_info_entry_t *entry, snd_info_buffer_t *buffer)
{
	snd_usb_stream_t *stream = snd_magic_cast(snd_usb_stream_t, entry->private_data, return);
	
	snd_iprintf(buffer, "%s : %s\n", stream->chip->card->longname, stream->pcm->name);

	if (stream->substream[SNDRV_PCM_STREAM_PLAYBACK].num_formats) {
		snd_iprintf(buffer, "\nPlayback:\n");
		proc_dump_substream_status(&stream->substream[SNDRV_PCM_STREAM_PLAYBACK], buffer);
		proc_dump_substream_formats(&stream->substream[SNDRV_PCM_STREAM_PLAYBACK], buffer);
	}
	if (stream->substream[SNDRV_PCM_STREAM_CAPTURE].num_formats) {
		snd_iprintf(buffer, "\nCapture:\n");
		proc_dump_substream_status(&stream->substream[SNDRV_PCM_STREAM_CAPTURE], buffer);
		proc_dump_substream_formats(&stream->substream[SNDRV_PCM_STREAM_CAPTURE], buffer);
	}
}

static void proc_pcm_format_add(snd_usb_stream_t *stream)
{
	snd_info_entry_t *entry;
	char name[32];
	snd_card_t *card = stream->chip->card;

	sprintf(name, "stream%d", stream->pcm_index);
	if (! snd_card_proc_new(card, name, &entry))
		snd_info_set_text_ops(entry, stream, 1024, proc_pcm_format_read);
}


/*
 * initialize the substream instance.
 */

static void init_substream(snd_usb_stream_t *as, int stream, struct audioformat *fp)
{
	snd_usb_substream_t *subs = &as->substream[stream];

	INIT_LIST_HEAD(&subs->fmt_list);
	spin_lock_init(&subs->lock);

	subs->stream = as;
	subs->direction = stream;
	subs->dev = as->chip->dev;
	if (snd_usb_get_speed(subs->dev) == USB_SPEED_FULL)
		subs->ops = audio_urb_ops[stream];
	else
		subs->ops = audio_urb_ops_high_speed[stream];
	snd_pcm_lib_preallocate_pages(as->pcm->streams[stream].substream,
				      SNDRV_DMA_TYPE_CONTINUOUS,
				      snd_dma_continuous_data(GFP_KERNEL),
				      64 * 1024, 128 * 1024);
	snd_pcm_set_ops(as->pcm, stream,
			stream == SNDRV_PCM_STREAM_PLAYBACK ?
			&snd_usb_playback_ops : &snd_usb_capture_ops);

	list_add_tail(&fp->list, &subs->fmt_list);
	subs->formats |= 1ULL << fp->format;
	subs->endpoint = fp->endpoint;
	subs->num_formats++;
	subs->fmt_type = fp->fmt_type;
}


/*
 * free a substream
 */
static void free_substream(snd_usb_substream_t *subs)
{
	struct list_head *p, *n;

	if (! subs->num_formats)
		return; /* not initialized */
	list_for_each_safe(p, n, &subs->fmt_list) {
		struct audioformat *fp = list_entry(p, struct audioformat, list);
		if (fp->rate_table)
			kfree(fp->rate_table);
		kfree(fp);
	}
}


/*
 * free a usb stream instance
 */
static void snd_usb_audio_stream_free(snd_usb_stream_t *stream)
{
	free_substream(&stream->substream[0]);
	free_substream(&stream->substream[1]);
	list_del(&stream->list);
	snd_magic_kfree(stream);
}

static void snd_usb_audio_pcm_free(snd_pcm_t *pcm)
{
	snd_usb_stream_t *stream = pcm->private_data;
	if (stream) {
		stream->pcm = NULL;
		snd_pcm_lib_preallocate_free_for_all(pcm);
		snd_usb_audio_stream_free(stream);
	}
}


/*
 * add this endpoint to the chip instance.
 * if a stream with the same endpoint already exists, append to it.
 * if not, create a new pcm stream.
 */
static int add_audio_endpoint(snd_usb_audio_t *chip, int stream, struct audioformat *fp)
{
	struct list_head *p;
	snd_usb_stream_t *as;
	snd_usb_substream_t *subs;
	snd_pcm_t *pcm;
	int err;

	list_for_each(p, &chip->pcm_list) {
		as = list_entry(p, snd_usb_stream_t, list);
		if (as->fmt_type != fp->fmt_type)
			continue;
		subs = &as->substream[stream];
		if (! subs->endpoint)
			continue;
		if (subs->endpoint == fp->endpoint) {
			list_add_tail(&fp->list, &subs->fmt_list);
			subs->num_formats++;
			subs->formats |= 1ULL << fp->format;
			return 0;
		}
	}
	/* look for an empty stream */
	list_for_each(p, &chip->pcm_list) {
		as = list_entry(p, snd_usb_stream_t, list);
		if (as->fmt_type != fp->fmt_type)
			continue;
		subs = &as->substream[stream];
		if (subs->endpoint)
			continue;
		err = snd_pcm_new_stream(as->pcm, stream, 1);
		if (err < 0)
			return err;
		init_substream(as, stream, fp);
		return 0;
	}

	/* create a new pcm */
	as = snd_magic_kmalloc(snd_usb_stream_t, 0, GFP_KERNEL);
	if (! as)
		return -ENOMEM;
	memset(as, 0, sizeof(*as));
	as->pcm_index = chip->pcm_devs;
	as->chip = chip;
	as->fmt_type = fp->fmt_type;
	err = snd_pcm_new(chip->card, "USB Audio", chip->pcm_devs,
			  stream == SNDRV_PCM_STREAM_PLAYBACK ? 1 : 0,
			  stream == SNDRV_PCM_STREAM_PLAYBACK ? 0 : 1,
			  &pcm);
	if (err < 0) {
		snd_magic_kfree(as);
		return err;
	}
	as->pcm = pcm;
	pcm->private_data = as;
	pcm->private_free = snd_usb_audio_pcm_free;
	pcm->info_flags = SNDRV_PCM_INFO_NONATOMIC_OPS;
	if (chip->pcm_devs > 0)
		sprintf(pcm->name, "USB Audio #%d", chip->pcm_devs);
	else
		strcpy(pcm->name, "USB Audio");

	init_substream(as, stream, fp);

	list_add(&as->list, &chip->pcm_list);
	chip->pcm_devs++;

	proc_pcm_format_add(as);

	return 0;
}


/*
 * parse the audio format type I descriptor
 * and returns the corresponding pcm format
 *
 * @dev: usb device
 * @fp: audioformat record
 * @format: the format tag (wFormatTag)
 * @fmt: the format type descriptor
 */
static int parse_audio_format_i_type(struct usb_device *dev, struct audioformat *fp,
				     int format, unsigned char *fmt)
{
	int pcm_format;
	int sample_width, sample_bytes;

	/* FIXME: correct endianess and sign? */
	pcm_format = -1;
	sample_width = fmt[6];
	sample_bytes = fmt[5];
	switch (format) {
	case 0: /* some devices don't define this correctly... */
		snd_printdd(KERN_INFO "%d:%u:%d : format type 0 is detected, processed as PCM\n",
			    dev->devnum, fp->iface, fp->altsetting);
		/* fall-through */
	case USB_AUDIO_FORMAT_PCM:
		if (sample_width > sample_bytes * 8) {
			snd_printk(KERN_INFO "%d:%u:%d : sample bitwidth %d in over sample bytes %d\n",
				   dev->devnum, fp->iface, fp->altsetting,
				   sample_width, sample_bytes);
		}
		/* check the format byte size */
		switch (fmt[5]) {
		case 1:
			pcm_format = SNDRV_PCM_FORMAT_S8;
			break;
		case 2:
			/* M-Audio audiophile USB workaround */
			if (dev->descriptor.idVendor == 0x0763 &&
			    dev->descriptor.idProduct == 0x2003)
				pcm_format = SNDRV_PCM_FORMAT_S16_BE; /* grrr, big endian!! */
			else
				pcm_format = SNDRV_PCM_FORMAT_S16_LE;
			break;
		case 3:
			/* M-Audio audiophile USB workaround */
			if (dev->descriptor.idVendor == 0x0763 &&
			    dev->descriptor.idProduct == 0x2003)
				pcm_format = SNDRV_PCM_FORMAT_S24_3BE; /* grrr, big endian!! */
			else
				pcm_format = SNDRV_PCM_FORMAT_S24_3LE;
			break;
		case 4:
			pcm_format = SNDRV_PCM_FORMAT_S32_LE;
			break;
		default:
			snd_printk(KERN_INFO "%d:%u:%d : unsupported sample bitwidth %d in %d bytes\n",
				   dev->devnum, fp->iface, fp->altsetting, sample_width, sample_bytes);
			break;
		}
		break;
	case USB_AUDIO_FORMAT_PCM8:
		/* Dallas DS4201 workaround */
		if (dev->descriptor.idVendor == 0x04fa && dev->descriptor.idProduct == 0x4201)
			pcm_format = SNDRV_PCM_FORMAT_S8;
		else
			pcm_format = SNDRV_PCM_FORMAT_U8;
		break;
	case USB_AUDIO_FORMAT_IEEE_FLOAT:
		pcm_format = SNDRV_PCM_FORMAT_FLOAT_LE;
		break;
	case USB_AUDIO_FORMAT_ALAW:
		pcm_format = SNDRV_PCM_FORMAT_A_LAW;
		break;
	case USB_AUDIO_FORMAT_MU_LAW:
		pcm_format = SNDRV_PCM_FORMAT_MU_LAW;
		break;
	default:
		snd_printk(KERN_INFO "%d:%u:%d : unsupported format type %d\n",
			   dev->devnum, fp->iface, fp->altsetting, format);
		break;
	}
	return pcm_format;
}


/*
 * parse the format descriptor and stores the possible sample rates
 * on the audioformat table.
 *
 * @dev: usb device
 * @fp: audioformat record
 * @fmt: the format descriptor
 * @offset: the start offset of descriptor pointing the rate type
 *          (7 for type I and II, 8 for type II)
 */
static int parse_audio_format_rates(struct usb_device *dev, struct audioformat *fp,
				    unsigned char *fmt, int offset)
{
	int nr_rates = fmt[offset];
	if (fmt[0] < offset + 1 + 3 * (nr_rates ? nr_rates : 2)) {
		snd_printk(KERN_ERR "%d:%u:%d : invalid FORMAT_TYPE desc\n", 
				   dev->devnum, fp->iface, fp->altsetting);
		return -1;
	}

	if (nr_rates) {
		/*
		 * build the rate table and bitmap flags
		 */
		int r, idx, c;
		/* this table corresponds to the SNDRV_PCM_RATE_XXX bit */
		static unsigned int conv_rates[] = {
			5512, 8000, 11025, 16000, 22050, 32000, 44100, 48000,
			64000, 88200, 96000, 176400, 192000
		};
		fp->rate_table = kmalloc(sizeof(int) * nr_rates, GFP_KERNEL);
		if (fp->rate_table == NULL) {
			snd_printk(KERN_ERR "cannot malloc\n");
			return -1;
		}

		fp->nr_rates = nr_rates;
		fp->rate_min = fp->rate_max = combine_triple(&fmt[8]);
		for (r = 0, idx = offset + 1; r < nr_rates; r++, idx += 3) {
			unsigned int rate = fp->rate_table[r] = combine_triple(&fmt[idx]);
			if (rate < fp->rate_min)
				fp->rate_min = rate;
			else if (rate > fp->rate_max)
				fp->rate_max = rate;
			for (c = 0; c < (int)ARRAY_SIZE(conv_rates); c++) {
				if (rate == conv_rates[c]) {
					fp->rates |= (1 << c);
					break;
				}
			}
		}
	} else {
		/* continuous rates */
		fp->rates = SNDRV_PCM_RATE_CONTINUOUS;
		fp->rate_min = combine_triple(&fmt[offset + 1]);
		fp->rate_max = combine_triple(&fmt[offset + 4]);
	}
	return 0;
}

/*
 * parse the format type I and III descriptors
 */
static int parse_audio_format_i(struct usb_device *dev, struct audioformat *fp,
				int format, unsigned char *fmt)
{
	int pcm_format;

	if (fmt[3] == USB_FORMAT_TYPE_III) {
		/* FIXME: the format type is really IECxxx
		 *        but we give normal PCM format to get the existing
		 *        apps working...
		 */
		pcm_format = SNDRV_PCM_FORMAT_S16_LE;
	} else {
		pcm_format = parse_audio_format_i_type(dev, fp, format, fmt);
		if (pcm_format < 0)
			return -1;
	}
	fp->format = pcm_format;
	fp->channels = fmt[4];
	if (fp->channels < 1) {
		snd_printk(KERN_ERR "%d:%u:%d : invalid channels %d\n",
			   dev->devnum, fp->iface, fp->altsetting, fp->channels);
		return -1;
	}
	return parse_audio_format_rates(dev, fp, fmt, 7);
}

/*
 * prase the format type II descriptor
 */
static int parse_audio_format_ii(struct usb_device *dev, struct audioformat *fp,
				 int format, unsigned char *fmt)
{
	int brate, framesize;
	switch (format) {
	case USB_AUDIO_FORMAT_AC3:
		/* FIXME: there is no AC3 format defined yet */
		// fp->format = SNDRV_PCM_FORMAT_AC3;
		fp->format = SNDRV_PCM_FORMAT_U8; /* temporarily hack to receive byte streams */
		break;
	case USB_AUDIO_FORMAT_MPEG:
		fp->format = SNDRV_PCM_FORMAT_MPEG;
		break;
	default:
		snd_printd(KERN_INFO "%d:%u:%d : unknown format tag 0x%x is detected.  processed as MPEG.\n",
			   dev->devnum, fp->iface, fp->altsetting, format);
		fp->format = SNDRV_PCM_FORMAT_MPEG;
		break;
	}
	fp->channels = 1;
	brate = combine_word(&fmt[4]); 	/* fmt[4,5] : wMaxBitRate (in kbps) */
	framesize = combine_word(&fmt[6]); /* fmt[6,7]: wSamplesPerFrame */
	snd_printd(KERN_INFO "found format II with max.bitrate = %d, frame size=%d\n", brate, framesize);
	fp->frame_size = framesize;
	return parse_audio_format_rates(dev, fp, fmt, 8); /* fmt[8..] sample rates */
}

static int parse_audio_format(struct usb_device *dev, struct audioformat *fp,
			      int format, unsigned char *fmt, int stream)
{
	int err;

	switch (fmt[3]) {
	case USB_FORMAT_TYPE_I:
	case USB_FORMAT_TYPE_III:
		err = parse_audio_format_i(dev, fp, format, fmt);
		break;
	case USB_FORMAT_TYPE_II:
		err = parse_audio_format_ii(dev, fp, format, fmt);
		break;
	default:
		snd_printd(KERN_INFO "%d:%u:%d : format type %d is not supported yet\n",
			   dev->devnum, fp->iface, fp->altsetting, fmt[3]);
		return -1;
	}
	fp->fmt_type = fmt[3];
	if (err < 0)
		return err;
#if 1
	/* FIXME: temporary hack for extigy */
	/* extigy apparently supports sample rates other than 48k
	 * but not in ordinary way.  so we enable only 48k atm.
	 */
	if (dev->descriptor.idVendor == 0x041e && dev->descriptor.idProduct == 0x3000) {
		if (fmt[3] == USB_FORMAT_TYPE_I &&
		    stream == SNDRV_PCM_STREAM_PLAYBACK &&
		    fp->rates != SNDRV_PCM_RATE_48000)
			return -1; /* use 48k only */
	}
#endif
	return 0;
}	

static int parse_audio_endpoints(snd_usb_audio_t *chip, int iface_no)
{
	struct usb_device *dev;
	struct usb_interface *iface;
	struct usb_host_interface *alts;
	struct usb_interface_descriptor *altsd;
	int i, altno, err, stream;
	int format;
	struct audioformat *fp;
	unsigned char *fmt, *csep;

	dev = chip->dev;

	/* parse the interface's altsettings */
	iface = usb_ifnum_to_if(dev, iface_no);
	for (i = 0; i < iface->num_altsetting; i++) {
		alts = &iface->altsetting[i];
		altsd = get_iface_desc(alts);
		/* skip invalid one */
		if ((altsd->bInterfaceClass != USB_CLASS_AUDIO &&
		     altsd->bInterfaceClass != USB_CLASS_VENDOR_SPEC) ||
		    (altsd->bInterfaceSubClass != USB_SUBCLASS_AUDIO_STREAMING &&
		     altsd->bInterfaceSubClass != USB_SUBCLASS_VENDOR_SPEC) ||
		    altsd->bNumEndpoints < 1 ||
		    get_endpoint(alts, 0)->wMaxPacketSize == 0)
			continue;
		/* must be isochronous */
		if ((get_endpoint(alts, 0)->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) !=
		    USB_ENDPOINT_XFER_ISOC)
			continue;
		/* check direction */
		stream = (get_endpoint(alts, 0)->bEndpointAddress & USB_DIR_IN) ?
			SNDRV_PCM_STREAM_CAPTURE : SNDRV_PCM_STREAM_PLAYBACK;
		altno = altsd->bAlternateSetting;

		/* get audio formats */
		fmt = snd_usb_find_csint_desc(alts->extra, alts->extralen, NULL, AS_GENERAL);
		if (!fmt) {
			snd_printk(KERN_ERR "%d:%u:%d : AS_GENERAL descriptor not found\n",
				   dev->devnum, iface_no, altno);
			continue;
		}

		if (fmt[0] < 7) {
			snd_printk(KERN_ERR "%d:%u:%d : invalid AS_GENERAL desc\n", 
				   dev->devnum, iface_no, altno);
			continue;
		}

		format = (fmt[6] << 8) | fmt[5]; /* remember the format value */
			
		/* get format type */
		fmt = snd_usb_find_csint_desc(alts->extra, alts->extralen, NULL, FORMAT_TYPE);
		if (!fmt) {
			snd_printk(KERN_ERR "%d:%u:%d : no FORMAT_TYPE desc\n", 
				   dev->devnum, iface_no, altno);
			continue;
		}
		if (fmt[0] < 8) {
			snd_printk(KERN_ERR "%d:%u:%d : invalid FORMAT_TYPE desc\n", 
				   dev->devnum, iface_no, altno);
			continue;
		}

		csep = snd_usb_find_desc(alts->endpoint[0].extra, alts->endpoint[0].extralen, NULL, USB_DT_CS_ENDPOINT);
		/* Creamware Noah has this descriptor after the 2nd endpoint */
		if (!csep && altsd->bNumEndpoints >= 2)
			csep = snd_usb_find_desc(alts->endpoint[1].extra, alts->endpoint[1].extralen, NULL, USB_DT_CS_ENDPOINT);
		if (!csep || csep[0] < 7 || csep[2] != EP_GENERAL) {
			snd_printk(KERN_ERR "%d:%u:%d : no or invalid class specific endpoint descriptor\n", 
				   dev->devnum, iface_no, altno);
			continue;
		}

		fp = kmalloc(sizeof(*fp), GFP_KERNEL);
		if (! fp) {
			snd_printk(KERN_ERR "cannot malloc\n");
			return -ENOMEM;
		}

		memset(fp, 0, sizeof(*fp));
		fp->iface = iface_no;
		fp->altsetting = altno;
		fp->altset_idx = i;
		fp->endpoint = get_endpoint(alts, 0)->bEndpointAddress;
		fp->ep_attr = get_endpoint(alts, 0)->bmAttributes;
		/* FIXME: decode wMaxPacketSize of high bandwith endpoints */
		fp->maxpacksize = get_endpoint(alts, 0)->wMaxPacketSize;
		fp->attributes = csep[3];

		/* some quirks for attributes here */

		/* workaround for AudioTrak Optoplay */
		if (dev->descriptor.idVendor == 0x0a92 &&
		    dev->descriptor.idProduct == 0x0053) {
			/* Optoplay sets the sample rate attribute although
			 * it seems not supporting it in fact.
			 */
			fp->attributes &= ~EP_CS_ATTR_SAMPLE_RATE;
		}

		/* workaround for M-Audio Audiophile USB */
		if (dev->descriptor.idVendor == 0x0763 &&
		    dev->descriptor.idProduct == 0x2003) {
			/* doesn't set the sample rate attribute, but supports it */
			fp->attributes |= EP_CS_ATTR_SAMPLE_RATE;
		}

		/*
		 * plantronics headset and Griffin iMic have set adaptive-in
		 * although it's really not...
		 */
		if ((dev->descriptor.idVendor == 0x047f &&
		     dev->descriptor.idProduct == 0x0ca1) ||
		    /* Griffin iMic (note that there is an older model 77d:223) */
		    (dev->descriptor.idVendor == 0x077d &&
		     dev->descriptor.idProduct == 0x07af)) {
			fp->ep_attr &= ~EP_ATTR_MASK;
			if (stream == SNDRV_PCM_STREAM_PLAYBACK)
				fp->ep_attr |= EP_ATTR_ADAPTIVE;
			else
				fp->ep_attr |= EP_ATTR_SYNC;
		}

		/* ok, let's parse further... */
		if (parse_audio_format(dev, fp, format, fmt, stream) < 0) {
			if (fp->rate_table)
				kfree(fp->rate_table);
			kfree(fp);
			continue;
		}

		snd_printdd(KERN_INFO "%d:%u:%d: add audio endpoint 0x%x\n", dev->devnum, iface_no, i, fp->endpoint);
		err = add_audio_endpoint(chip, stream, fp);
		if (err < 0) {
			if (fp->rate_table)
				kfree(fp->rate_table);
			kfree(fp);
			return err;
		}
		/* try to set the interface... */
		usb_set_interface(chip->dev, iface_no, altno);
		init_usb_pitch(chip->dev, iface_no, alts, fp);
		init_usb_sample_rate(chip->dev, iface_no, alts, fp, fp->rate_max);
	}
	return 0;
}


/*
 * disconnect streams
 * called from snd_usb_audio_disconnect()
 */
static void snd_usb_stream_disconnect(struct list_head *head, struct usb_driver *driver)
{
	int idx;
	snd_usb_stream_t *as;
	snd_usb_substream_t *subs;

	as = list_entry(head, snd_usb_stream_t, list);
	for (idx = 0; idx < 2; idx++) {
		subs = &as->substream[idx];
		if (!subs->num_formats)
			return;
		release_substream_urbs(subs, 1);
		subs->interface = -1;
	}
}

/*
 * parse audio control descriptor and create pcm/midi streams
 */
static int snd_usb_create_streams(snd_usb_audio_t *chip, int ctrlif)
{
	struct usb_device *dev = chip->dev;
	struct usb_host_interface *host_iface;
	struct usb_interface *iface;
	unsigned char *p1;
	int i, j;

	/* find audiocontrol interface */
	host_iface = &usb_ifnum_to_if(dev, ctrlif)->altsetting[0];
	if (!(p1 = snd_usb_find_csint_desc(host_iface->extra, host_iface->extralen, NULL, HEADER))) {
		snd_printk(KERN_ERR "cannot find HEADER\n");
		return -EINVAL;
	}
	if (! p1[7] || p1[0] < 8 + p1[7]) {
		snd_printk(KERN_ERR "invalid HEADER\n");
		return -EINVAL;
	}

	/*
	 * parse all USB audio streaming interfaces
	 */
	for (i = 0; i < p1[7]; i++) {
		struct usb_host_interface *alts;
		struct usb_interface_descriptor *altsd;
		j = p1[8 + i];
		iface = usb_ifnum_to_if(dev, j);
		if (!iface) {
			snd_printk(KERN_ERR "%d:%u:%d : does not exist\n",
				   dev->devnum, ctrlif, j);
			continue;
		}
		if (usb_interface_claimed(iface)) {
			snd_printdd(KERN_INFO "%d:%d:%d: skipping, already claimed\n", dev->devnum, ctrlif, j);
			continue;
		}
		alts = &iface->altsetting[0];
		altsd = get_iface_desc(alts);
		if ((altsd->bInterfaceClass == USB_CLASS_AUDIO ||
		     altsd->bInterfaceClass == USB_CLASS_VENDOR_SPEC) &&
		    altsd->bInterfaceSubClass == USB_SUBCLASS_MIDI_STREAMING) {
			if (snd_usb_create_midi_interface(chip, iface, NULL) < 0) {
				snd_printk(KERN_ERR "%d:%u:%d: cannot create sequencer device\n", dev->devnum, ctrlif, j);
				continue;
			}
			usb_driver_claim_interface(&usb_audio_driver, iface, (void *)-1L);
			continue;
		}
		if ((altsd->bInterfaceClass != USB_CLASS_AUDIO &&
		     altsd->bInterfaceClass != USB_CLASS_VENDOR_SPEC) ||
		    altsd->bInterfaceSubClass != USB_SUBCLASS_AUDIO_STREAMING) {
			snd_printdd(KERN_ERR "%d:%u:%d: skipping non-supported interface %d\n", dev->devnum, ctrlif, j, altsd->bInterfaceClass);
			/* skip non-supported classes */
			continue;
		}
		if (! parse_audio_endpoints(chip, j)) {
			usb_set_interface(dev, j, 0); /* reset the current interface */
			usb_driver_claim_interface(&usb_audio_driver, iface, (void *)-1L);
		}
	}

	return 0;
}

/*
 * create a stream for an endpoint/altsetting without proper descriptors
 */
static int create_fixed_stream_quirk(snd_usb_audio_t *chip,
				     struct usb_interface *iface,
				     const snd_usb_audio_quirk_t *quirk)
{
	struct audioformat *fp;
	struct usb_host_interface *alts;
	int stream, err;
	int *rate_table = NULL;

	fp = kmalloc(sizeof(*fp), GFP_KERNEL);
	if (! fp) {
		snd_printk(KERN_ERR "cannot malloc\n");
		return -ENOMEM;
	}
	memcpy(fp, quirk->data, sizeof(*fp));
	if (fp->nr_rates > 0) {
		rate_table = kmalloc(sizeof(int) * fp->nr_rates, GFP_KERNEL);
		if (!rate_table) {
			kfree(fp);
			return -ENOMEM;
		}
		memcpy(rate_table, fp->rate_table, sizeof(int) * fp->nr_rates);
		fp->rate_table = rate_table;
	}

	stream = (fp->endpoint & USB_DIR_IN)
		? SNDRV_PCM_STREAM_CAPTURE : SNDRV_PCM_STREAM_PLAYBACK;
	err = add_audio_endpoint(chip, stream, fp);
	if (err < 0) {
		kfree(fp);
		if (rate_table)
			kfree(rate_table);
		return err;
	}
	if (fp->iface != get_iface_desc(&iface->altsetting[0])->bInterfaceNumber ||
	    fp->altset_idx >= iface->num_altsetting) {
		kfree(fp);
		if (rate_table)
			kfree(rate_table);
		return -EINVAL;
	}
	alts = &iface->altsetting[fp->altset_idx];
	usb_set_interface(chip->dev, fp->iface, 0);
	init_usb_pitch(chip->dev, fp->iface, alts, fp);
	init_usb_sample_rate(chip->dev, fp->iface, alts, fp, fp->rate_max);
	return 0;
}

/*
 * create a stream for an interface with proper descriptors
 */
static int create_standard_interface_quirk(snd_usb_audio_t *chip,
					   struct usb_interface *iface,
					   const snd_usb_audio_quirk_t *quirk)
{
	struct usb_host_interface *alts;
	struct usb_interface_descriptor *altsd;
	int err;

	alts = &iface->altsetting[0];
	altsd = get_iface_desc(alts);
	switch (quirk->type) {
	case QUIRK_AUDIO_STANDARD_INTERFACE:
		err = parse_audio_endpoints(chip, altsd->bInterfaceNumber);
		if (!err)
			usb_set_interface(chip->dev, altsd->bInterfaceNumber, 0); /* reset the current interface */
		break;
	case QUIRK_MIDI_STANDARD_INTERFACE:
		err = snd_usb_create_midi_interface(chip, iface, NULL);
		break;
	default:
		snd_printd(KERN_ERR "invalid quirk type %d\n", quirk->type);
		return -ENXIO;
	}
	if (err < 0) {
		snd_printk(KERN_ERR "cannot setup if %d: error %d\n",
			   altsd->bInterfaceNumber, err);
		return err;
	}
	return 0;
}

/*
 * Create a stream for an Edirol UA-700 interface.  The only way
 * to detect the sample rate is by looking at wMaxPacketSize.
 */
static int create_ua700_quirk(snd_usb_audio_t *chip, struct usb_interface *iface)
{
	static const struct audioformat ua700_format = {
		.format = SNDRV_PCM_FORMAT_S24_3LE,
		.channels = 2,
		.fmt_type = USB_FORMAT_TYPE_I,
		.altsetting = 1,
		.altset_idx = 1,
		.rates = SNDRV_PCM_RATE_CONTINUOUS,
	};
	struct usb_host_interface *alts;
	struct usb_interface_descriptor *altsd;
	struct audioformat *fp;
	int stream, err;

	/* both PCM and MIDI interfaces have 2 altsettings */
	if (iface->num_altsetting != 2)
		return -ENXIO;
	alts = &iface->altsetting[1];
	altsd = get_iface_desc(alts);

	if (altsd->bNumEndpoints == 2) {
		static const snd_usb_midi_endpoint_info_t ep = {
			.out_cables = 0x0003,
			.in_cables  = 0x0003
		};
		static const snd_usb_audio_quirk_t quirk = {
			.type = QUIRK_MIDI_FIXED_ENDPOINT,
			.data = &ep
		};
		return snd_usb_create_midi_interface(chip, iface, &quirk);
	}

	if (altsd->bNumEndpoints != 1)
		return -ENXIO;

	fp = kmalloc(sizeof(*fp), GFP_KERNEL);
	if (!fp)
		return -ENOMEM;
	memcpy(fp, &ua700_format, sizeof(*fp));

	fp->iface = altsd->bInterfaceNumber;
	fp->endpoint = get_endpoint(alts, 0)->bEndpointAddress;
	fp->ep_attr = get_endpoint(alts, 0)->bmAttributes;
	fp->maxpacksize = get_endpoint(alts, 0)->wMaxPacketSize;

	switch (fp->maxpacksize) {
	case 0x120:
		fp->rate_max = fp->rate_min = 44100;
		break;
	case 0x138:
		fp->rate_max = fp->rate_min = 48000;
		break;
	case 0x258:
		fp->rate_max = fp->rate_min = 96000;
		break;
	default:
		snd_printk(KERN_ERR "unknown sample rate\n");
		kfree(fp);
		return -ENXIO;
	}

	stream = (fp->endpoint & USB_DIR_IN)
		? SNDRV_PCM_STREAM_CAPTURE : SNDRV_PCM_STREAM_PLAYBACK;
	err = add_audio_endpoint(chip, stream, fp);
	if (err < 0) {
		kfree(fp);
		return err;
	}
	usb_set_interface(chip->dev, fp->iface, 0);
	return 0;
}

static int snd_usb_create_quirk(snd_usb_audio_t *chip,
				struct usb_interface *iface,
				const snd_usb_audio_quirk_t *quirk);

/*
 * handle the quirks for the contained interfaces
 */
static int create_composite_quirk(snd_usb_audio_t *chip,
				  struct usb_interface *iface,
				  const snd_usb_audio_quirk_t *quirk)
{
	int probed_ifnum = get_iface_desc(iface->altsetting)->bInterfaceNumber;
	int err;

	for (quirk = quirk->data; quirk->ifnum >= 0; ++quirk) {
		iface = usb_ifnum_to_if(chip->dev, quirk->ifnum);
		if (!iface)
			continue;
		if (quirk->ifnum != probed_ifnum &&
		    usb_interface_claimed(iface))
			continue;
		err = snd_usb_create_quirk(chip, iface, quirk);
		if (err < 0)
			return err;
		if (quirk->ifnum != probed_ifnum)
			usb_driver_claim_interface(&usb_audio_driver, iface, (void *)-1L);
	}
	return 0;
}


/*
 * boot quirks
 */

#define EXTIGY_FIRMWARE_SIZE_OLD 794
#define EXTIGY_FIRMWARE_SIZE_NEW 483

static int snd_usb_extigy_boot_quirk(struct usb_device *dev, struct usb_interface *intf)
{
	struct usb_host_config *config = dev->actconfig;
	int err;

	if (get_cfg_desc(config)->wTotalLength == EXTIGY_FIRMWARE_SIZE_OLD ||
	    get_cfg_desc(config)->wTotalLength == EXTIGY_FIRMWARE_SIZE_NEW) {
		snd_printdd("sending Extigy boot sequence...\n");
		/* Send message to force it to reconnect with full interface. */
		err = snd_usb_ctl_msg(dev, usb_sndctrlpipe(dev,0),
				      0x10, 0x43, 0x0001, 0x000a, NULL, 0, HZ);
		if (err < 0) snd_printdd("error sending boot message: %d\n", err);
		err = usb_get_descriptor(dev, USB_DT_DEVICE, 0,
				&dev->descriptor, sizeof(dev->descriptor));
		config = dev->actconfig;
		if (err < 0) snd_printdd("error usb_get_descriptor: %d\n", err);
		err = usb_reset_configuration(dev);
		if (err < 0) snd_printdd("error usb_reset_configuration: %d\n", err);
		snd_printdd("extigy_boot: new boot length = %d\n", get_cfg_desc(config)->wTotalLength);
		return -ENODEV; /* quit this anyway */
	}
	return 0;
}


/*
 * audio-interface quirks
 *
 * returns zero if no standard audio/MIDI parsing is needed.
 * returns a postive value if standard audio/midi interfaces are parsed
 * after this.
 * returns a negative value at error.
 */
static int snd_usb_create_quirk(snd_usb_audio_t *chip,
				struct usb_interface *iface,
				const snd_usb_audio_quirk_t *quirk)
{
	switch (quirk->type) {
	case QUIRK_MIDI_FIXED_ENDPOINT:
	case QUIRK_MIDI_YAMAHA:
	case QUIRK_MIDI_MIDIMAN:
		return snd_usb_create_midi_interface(chip, iface, quirk);
	case QUIRK_COMPOSITE:
		return create_composite_quirk(chip, iface, quirk);
	case QUIRK_AUDIO_FIXED_ENDPOINT:
		return create_fixed_stream_quirk(chip, iface, quirk);
	case QUIRK_AUDIO_STANDARD_INTERFACE:
	case QUIRK_MIDI_STANDARD_INTERFACE:
		return create_standard_interface_quirk(chip, iface, quirk);
	case QUIRK_AUDIO_EDIROL_UA700:
		return create_ua700_quirk(chip, iface);
	default:
		snd_printd(KERN_ERR "invalid quirk type %d\n", quirk->type);
		return -ENXIO;
	}
}


/*
 * common proc files to show the usb device info
 */
static void proc_audio_usbbus_read(snd_info_entry_t *entry, snd_info_buffer_t *buffer)
{
	snd_usb_audio_t *chip = snd_magic_cast(snd_usb_audio_t, entry->private_data, return);
	if (! chip->shutdown)
		snd_iprintf(buffer, "%03d/%03d\n", chip->dev->bus->busnum, chip->dev->devnum);
}

static void proc_audio_usbid_read(snd_info_entry_t *entry, snd_info_buffer_t *buffer)
{
	snd_usb_audio_t *chip = snd_magic_cast(snd_usb_audio_t, entry->private_data, return);
	if (! chip->shutdown)
		snd_iprintf(buffer, "%04x:%04x\n", chip->dev->descriptor.idVendor, chip->dev->descriptor.idProduct);
}

static void snd_usb_audio_create_proc(snd_usb_audio_t *chip)
{
	snd_info_entry_t *entry;
	if (! snd_card_proc_new(chip->card, "usbbus", &entry))
		snd_info_set_text_ops(entry, chip, 1024, proc_audio_usbbus_read);
	if (! snd_card_proc_new(chip->card, "usbid", &entry))
		snd_info_set_text_ops(entry, chip, 1024, proc_audio_usbid_read);
}

/*
 * free the chip instance
 *
 * here we have to do not much, since pcm and controls are already freed
 *
 */

static int snd_usb_audio_free(snd_usb_audio_t *chip)
{
	snd_magic_kfree(chip);
	return 0;
}

static int snd_usb_audio_dev_free(snd_device_t *device)
{
	snd_usb_audio_t *chip = snd_magic_cast(snd_usb_audio_t, device->device_data, return -ENXIO);
	return snd_usb_audio_free(chip);
}


/*
 * create a chip instance and set its names.
 */
static int snd_usb_audio_create(struct usb_device *dev, int idx,
				const snd_usb_audio_quirk_t *quirk,
				snd_usb_audio_t **rchip)
{
	snd_card_t *card;
	snd_usb_audio_t *chip;
	int err, len;
	char component[14];
	static snd_device_ops_t ops = {
		.dev_free =	snd_usb_audio_dev_free,
	};
	
	*rchip = NULL;

	if (snd_usb_get_speed(dev) != USB_SPEED_FULL &&
	    snd_usb_get_speed(dev) != USB_SPEED_HIGH) {
		snd_printk(KERN_ERR "unknown device speed %d\n", snd_usb_get_speed(dev));
		return -ENXIO;
	}

	card = snd_card_new(index[idx], id[idx], THIS_MODULE, 0);
	if (card == NULL) {
		snd_printk(KERN_ERR "cannot create card instance %d\n", idx);
		return -ENOMEM;
	}

	chip = snd_magic_kcalloc(snd_usb_audio_t, 0, GFP_KERNEL);
	if (! chip) {
		snd_card_free(card);
		return -ENOMEM;
	}

	chip->index = idx;
	chip->dev = dev;
	chip->card = card;
	INIT_LIST_HEAD(&chip->pcm_list);
	INIT_LIST_HEAD(&chip->midi_list);

	if ((err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, chip, &ops)) < 0) {
		snd_usb_audio_free(chip);
		snd_card_free(card);
		return err;
	}

	strcpy(card->driver, "USB-Audio");
	sprintf(component, "USB%04x:%04x",
		dev->descriptor.idVendor, dev->descriptor.idProduct);
	snd_component_add(card, component);

	/* retrieve the device string as shortname */
 	if (quirk && quirk->product_name) {
		strlcpy(card->shortname, quirk->product_name, sizeof(card->shortname));
	} else {
		if (!dev->descriptor.iProduct ||
		    usb_string(dev, dev->descriptor.iProduct,
      			       card->shortname, sizeof(card->shortname)) <= 0) {
			/* no name available from anywhere, so use ID */
			sprintf(card->shortname, "USB Device %#04x:%#04x",
				dev->descriptor.idVendor, dev->descriptor.idProduct);
		}
	}

	/* retrieve the vendor and device strings as longname */
	if (quirk && quirk->vendor_name) {
		len = strlcpy(card->longname, quirk->vendor_name, sizeof(card->longname));
	} else {
		if (dev->descriptor.iManufacturer)
			len = usb_string(dev, dev->descriptor.iManufacturer,
					 card->longname, sizeof(card->longname));
		else
			len = 0;
		/* we don't really care if there isn't any vendor string */
	}
	if (len > 0)
		strlcat(card->longname, " ", sizeof(card->longname));

	strlcat(card->longname, card->shortname, sizeof(card->longname));

	len = strlcat(card->longname, " at ", sizeof(card->longname));

	if (len < sizeof(card->longname))
		usb_make_path(dev, card->longname + len, sizeof(card->longname) - len);

	strlcat(card->longname,
		snd_usb_get_speed(dev) == USB_SPEED_FULL ? ", full speed" : ", high speed",
		sizeof(card->longname));

	snd_usb_audio_create_proc(chip);

	snd_card_set_dev(card, &dev->dev);

	*rchip = chip;
	return 0;
}


/*
 * probe the active usb device
 *
 * note that this can be called multiple times per a device, when it
 * includes multiple audio control interfaces.
 *
 * thus we check the usb device pointer and creates the card instance
 * only at the first time.  the successive calls of this function will
 * append the pcm interface to the corresponding card.
 */
static void *snd_usb_audio_probe(struct usb_device *dev,
				 struct usb_interface *intf,
				 const struct usb_device_id *usb_id)
{
	struct usb_host_config *config = dev->actconfig;
	const snd_usb_audio_quirk_t *quirk = (const snd_usb_audio_quirk_t *)usb_id->driver_info;
	int i, err;
	snd_usb_audio_t *chip;
	struct usb_host_interface *alts;
	int ifnum;

	alts = &intf->altsetting[0];
	ifnum = get_iface_desc(alts)->bInterfaceNumber;

	if (quirk && quirk->ifnum >= 0 && ifnum != quirk->ifnum)
		goto __err_val;

	/* SB Extigy needs special boot-up sequence */
	/* if more models come, this will go to the quirk list. */
	if (dev->descriptor.idVendor == 0x041e && dev->descriptor.idProduct == 0x3000) {
		if (snd_usb_extigy_boot_quirk(dev, intf) < 0)
			goto __err_val;
		config = dev->actconfig;
	}

	/*
	 * found a config.  now register to ALSA
	 */

	/* check whether it's already registered */
	chip = NULL;
	down(&register_mutex);
	for (i = 0; i < SNDRV_CARDS; i++) {
		if (usb_chip[i] && usb_chip[i]->dev == dev) {
			if (usb_chip[i]->shutdown) {
				snd_printk(KERN_ERR "USB device is in the shutdown state, cannot create a card instance\n");
				goto __error;
			}
			chip = usb_chip[i];
			break;
		}
	}
	if (! chip) {
		/* it's a fresh one.
		 * now look for an empty slot and create a new card instance
		 */
		/* first, set the current configuration for this device */
		if (usb_reset_configuration(dev) < 0) {
			snd_printk(KERN_ERR "cannot reset configuration (value 0x%x)\n", get_cfg_desc(config)->bConfigurationValue);
			goto __error;
		}
		for (i = 0; i < SNDRV_CARDS; i++)
			if (enable[i] && ! usb_chip[i] &&
			    (vid[i] == -1 || vid[i] == dev->descriptor.idVendor) &&
			    (pid[i] == -1 || pid[i] == dev->descriptor.idProduct)) {
				if (snd_usb_audio_create(dev, i, quirk, &chip) < 0) {
					goto __error;
				}
				break;
			}
		if (! chip) {
			snd_printk(KERN_ERR "no available usb audio device\n");
			goto __error;
		}
	}

	err = 1; /* continue */
	if (quirk && quirk->ifnum != QUIRK_NO_INTERFACE) {
		/* need some special handlings */
		if ((err = snd_usb_create_quirk(chip, intf, quirk)) < 0)
			goto __error;
	}

	if (err > 0) {
		/* create normal USB audio interfaces */
		if (snd_usb_create_streams(chip, ifnum) < 0 ||
		    snd_usb_create_mixer(chip, ifnum) < 0) {
			goto __error;
		}
	}

	/* we are allowed to call snd_card_register() many times */
	if (snd_card_register(chip->card) < 0) {
		goto __error;
	}

	usb_chip[chip->index] = chip;
	chip->num_interfaces++;
	up(&register_mutex);
	return chip;

 __error:
	if (chip && !chip->num_interfaces)
		snd_card_free(chip->card);
	up(&register_mutex);
 __err_val:
	return NULL;
}

/*
 * we need to take care of counter, since disconnection can be called also
 * many times as well as usb_audio_probe(). 
 */
static void snd_usb_audio_disconnect(struct usb_device *dev, void *ptr)
{
	snd_usb_audio_t *chip;
	snd_card_t *card;
	struct list_head *p;

	if (ptr == (void *)-1L)
		return;

	chip = snd_magic_cast(snd_usb_audio_t, ptr, return);
	card = chip->card;
	down(&register_mutex);
	chip->shutdown = 1;
	chip->num_interfaces--;
	if (chip->num_interfaces <= 0) {
		snd_card_disconnect(card);
		/* release the pcm resources */
		list_for_each(p, &chip->pcm_list) {
			snd_usb_stream_disconnect(p, &usb_audio_driver);
		}
		/* release the midi resources */
		list_for_each(p, &chip->midi_list) {
			snd_usbmidi_disconnect(p, &usb_audio_driver);
		}
		usb_chip[chip->index] = NULL;
		up(&register_mutex);
		snd_card_free_in_thread(card);
	} else {
		up(&register_mutex);
	}
}

/*
 * new 2.5 USB kernel API
 */
static int usb_audio_probe(struct usb_interface *intf,
			   const struct usb_device_id *id)
{
	void *chip;
	chip = snd_usb_audio_probe(interface_to_usbdev(intf), intf, id);
	if (chip) {
		dev_set_drvdata(&intf->dev, chip);
		return 0;
	} else
		return -EIO;
}

static void usb_audio_disconnect(struct usb_interface *intf)
{
	snd_usb_audio_disconnect(interface_to_usbdev(intf),
				 dev_get_drvdata(&intf->dev));
}


static int __init snd_usb_audio_init(void)
{
	if (nrpacks < MIN_PACKS_URB || nrpacks > MAX_PACKS) {
		printk(KERN_WARNING "invalid nrpacks value.\n");
		return -EINVAL;
	}
	usb_register(&usb_audio_driver);
	return 0;
}


static void __exit snd_usb_audio_cleanup(void)
{
	usb_deregister(&usb_audio_driver);
}

module_init(snd_usb_audio_init);
module_exit(snd_usb_audio_cleanup);
