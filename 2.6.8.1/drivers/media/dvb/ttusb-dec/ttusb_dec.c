/*
 * TTUSB DEC Driver
 *
 * Copyright (C) 2003-2004 Alex Woods <linux-dvb@giblets.org>
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <asm/semaphore.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/interrupt.h>
#include <linux/firmware.h>
#if defined(CONFIG_CRC32) || defined(CONFIG_CRC32_MODULE)
#include <linux/crc32.h>
#else
#warning "CRC checking of firmware not available"
#endif
#include <linux/init.h>

#include "dmxdev.h"
#include "dvb_demux.h"
#include "dvb_i2c.h"
#include "dvb_filter.h"
#include "dvb_frontend.h"
#include "dvb_net.h"

static int debug = 0;
static int output_pva = 0;

#define dprintk	if (debug) printk

#define DRIVER_NAME		"TechnoTrend/Hauppauge DEC USB"

#define COMMAND_PIPE		0x03
#define RESULT_PIPE		0x84
#define IN_PIPE			0x88
#define OUT_PIPE		0x07

#define COMMAND_PACKET_SIZE	0x3c
#define ARM_PACKET_SIZE		0x1000

#define ISO_BUF_COUNT		0x04
#define FRAMES_PER_ISO_BUF	0x04
#define ISO_FRAME_SIZE		0x03FF

#define	MAX_PVA_LENGTH		6144

#define LOF_HI			10600000
#define LOF_LO			9750000

enum ttusb_dec_model {
	TTUSB_DEC2000T,
	TTUSB_DEC2540T,
	TTUSB_DEC3000S
};

enum ttusb_dec_packet_type {
	TTUSB_DEC_PACKET_PVA,
	TTUSB_DEC_PACKET_SECTION,
	TTUSB_DEC_PACKET_EMPTY
};

enum ttusb_dec_interface {
	TTUSB_DEC_INTERFACE_INITIAL,
	TTUSB_DEC_INTERFACE_IN,
	TTUSB_DEC_INTERFACE_OUT
};

struct ttusb_dec {
	enum ttusb_dec_model		model;
	char				*model_name;
	char				*firmware_name;
	int				can_playback;

	/* DVB bits */
	struct dvb_adapter		*adapter;
	struct dmxdev			dmxdev;
	struct dvb_demux		demux;
	struct dmx_frontend		frontend;
	struct dvb_i2c_bus		i2c_bus;
	struct dvb_net			dvb_net;
	struct dvb_frontend_info	*frontend_info;
	int (*frontend_ioctl) (struct dvb_frontend *, unsigned int, void *);

	u16			pid[DMX_PES_OTHER];
	int			hi_band;
	int			voltage;

	/* USB bits */
	struct usb_device	*udev;
	u8			trans_count;
	unsigned int		command_pipe;
	unsigned int		result_pipe;
	unsigned int			in_pipe;
	unsigned int			out_pipe;
	enum ttusb_dec_interface	interface;
	struct semaphore	usb_sem;

	void			*iso_buffer;
	dma_addr_t		iso_dma_handle;
	struct urb		*iso_urb[ISO_BUF_COUNT];
	int			iso_stream_count;
	struct semaphore	iso_sem;

	u8				packet[MAX_PVA_LENGTH + 4];
	enum ttusb_dec_packet_type	packet_type;
	int				packet_state;
	int				packet_length;
	int				packet_payload_length;
	u16				next_packet_id;

	int				pva_stream_count;
	int				filter_stream_count;

	struct dvb_filter_pes2ts	a_pes2ts;
	struct dvb_filter_pes2ts	v_pes2ts;

	u8			v_pes[16 + MAX_PVA_LENGTH];
	int			v_pes_length;
	int			v_pes_postbytes;

	struct list_head	urb_frame_list;
	struct tasklet_struct	urb_tasklet;
	spinlock_t		urb_frame_list_lock;

	struct dvb_demux_filter	*audio_filter;
	struct dvb_demux_filter	*video_filter;
	struct list_head	filter_info_list;
	spinlock_t		filter_info_list_lock;

	int			active; /* Loaded successfully */
};

struct urb_frame {
	u8			data[ISO_FRAME_SIZE];
	int			length;
	struct list_head	urb_frame_list;
};

struct filter_info {
	u8			stream_id;
	struct dvb_demux_filter	*filter;
	struct list_head	filter_info_list;
};

static struct dvb_frontend_info dec2000t_frontend_info = {
	.name			= "TechnoTrend/Hauppauge DEC2000-t Frontend",
	.type			= FE_OFDM,
	.frequency_min		= 51000000,
	.frequency_max		= 858000000,
	.frequency_stepsize	= 62500,
	.caps =	FE_CAN_FEC_1_2 | FE_CAN_FEC_2_3 | FE_CAN_FEC_3_4 |
		FE_CAN_FEC_5_6 | FE_CAN_FEC_7_8 | FE_CAN_FEC_AUTO |
		FE_CAN_QAM_16 | FE_CAN_QAM_64 | FE_CAN_QAM_AUTO |
		FE_CAN_TRANSMISSION_MODE_AUTO | FE_CAN_GUARD_INTERVAL_AUTO |
		FE_CAN_HIERARCHY_AUTO,
};

static struct dvb_frontend_info dec3000s_frontend_info = {
	.name			= "TechnoTrend/Hauppauge DEC3000-s Frontend",
	.type			= FE_QPSK,
	.frequency_min		= 950000,
	.frequency_max		= 2150000,
	.frequency_stepsize	= 125,
	.caps =	FE_CAN_FEC_1_2 | FE_CAN_FEC_2_3 | FE_CAN_FEC_3_4 |
		FE_CAN_FEC_5_6 | FE_CAN_FEC_7_8 | FE_CAN_FEC_AUTO |
		FE_CAN_QAM_16 | FE_CAN_QAM_64 | FE_CAN_QAM_AUTO |
		FE_CAN_TRANSMISSION_MODE_AUTO | FE_CAN_GUARD_INTERVAL_AUTO |
		FE_CAN_HIERARCHY_AUTO,
};

static void ttusb_dec_set_model(struct ttusb_dec *dec,
				enum ttusb_dec_model model);

static u16 crc16(u16 crc, const u8 *buf, size_t len)
{
	u16 tmp;

	while (len--) {
		crc ^= *buf++;
		crc ^= (u8)crc >> 4;
		tmp = (u8)crc;
		crc ^= (tmp ^ (tmp << 1)) << 4;
	}
	return crc;
}

static int ttusb_dec_send_command(struct ttusb_dec *dec, const u8 command,
				  int param_length, const u8 params[],
				  int *result_length, u8 cmd_result[])
{
	int result, actual_len, i;
	u8 *b;

	dprintk("%s\n", __FUNCTION__);
	
	b = kmalloc(COMMAND_PACKET_SIZE + 4, GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	if ((result = down_interruptible(&dec->usb_sem))) {
		kfree(b);
		printk("%s: Failed to down usb semaphore.\n", __FUNCTION__);
		return result;
	}

	b[0] = 0xaa;
	b[1] = ++dec->trans_count;
	b[2] = command;
	b[3] = param_length;

	if (params)
		memcpy(&b[4], params, param_length);

	if (debug) {
		printk("%s: command: ", __FUNCTION__);
		for (i = 0; i < param_length + 4; i++)
			printk("0x%02X ", b[i]);
		printk("\n");
	}

	result = usb_bulk_msg(dec->udev, dec->command_pipe, b,
			      COMMAND_PACKET_SIZE + 4, &actual_len, HZ);

	if (result) {
		printk("%s: command bulk message failed: error %d\n",
		       __FUNCTION__, result);
		up(&dec->usb_sem);
		kfree(b);
		return result;
	}

	result = usb_bulk_msg(dec->udev, dec->result_pipe, b,
			      COMMAND_PACKET_SIZE + 4, &actual_len, HZ);

	if (result) {
		printk("%s: result bulk message failed: error %d\n",
		       __FUNCTION__, result);
		up(&dec->usb_sem);
		kfree(b);
		return result;
	} else {
		if (debug) {
			printk("%s: result: ", __FUNCTION__);
			for (i = 0; i < actual_len; i++)
				printk("0x%02X ", b[i]);
			printk("\n");
		}

		if (result_length)
			*result_length = b[3];
		if (cmd_result && b[3] > 0)
			memcpy(cmd_result, &b[4], b[3]);

		up(&dec->usb_sem);

		kfree(b);
		return 0;
	}
}

static int ttusb_dec_get_stb_state (struct ttusb_dec *dec, unsigned int *mode,
				    unsigned int *model, unsigned int *version)
{
	u8 c[COMMAND_PACKET_SIZE];
	int c_length;
	int result;
	unsigned int tmp;

	dprintk("%s\n", __FUNCTION__);

	result = ttusb_dec_send_command(dec, 0x08, 0, NULL, &c_length, c);
	if (result)
		return result;

	if (c_length >= 0x0c) {
		if (mode != NULL) {
			memcpy(&tmp, c, 4);
			*mode = ntohl(tmp);
		}
		if (model != NULL) {
			memcpy(&tmp, &c[4], 4);
			*model = ntohl(tmp);
		}
		if (version != NULL) {
			memcpy(&tmp, &c[8], 4);
			*version = ntohl(tmp);
		}
		return 0;
	} else {
		return -1;
	}
}

static int ttusb_dec_audio_pes2ts_cb(void *priv, unsigned char *data)
{
	struct ttusb_dec *dec = (struct ttusb_dec *)priv;

	dec->audio_filter->feed->cb.ts(data, 188, NULL, 0,
				       &dec->audio_filter->feed->feed.ts,
				       DMX_OK);

	return 0;
}

static int ttusb_dec_video_pes2ts_cb(void *priv, unsigned char *data)
{
	struct ttusb_dec *dec = (struct ttusb_dec *)priv;

	dec->video_filter->feed->cb.ts(data, 188, NULL, 0,
				       &dec->video_filter->feed->feed.ts,
				       DMX_OK);

	return 0;
}

static void ttusb_dec_set_pids(struct ttusb_dec *dec)
{
	u8 b[] = { 0x00, 0x00, 0x00, 0x00,
		   0x00, 0x00, 0xff, 0xff,
		   0xff, 0xff, 0xff, 0xff };

	u16 pcr = htons(dec->pid[DMX_PES_PCR]);
	u16 audio = htons(dec->pid[DMX_PES_AUDIO]);
	u16 video = htons(dec->pid[DMX_PES_VIDEO]);

	dprintk("%s\n", __FUNCTION__);

	memcpy(&b[0], &pcr, 2);
	memcpy(&b[2], &audio, 2);
	memcpy(&b[4], &video, 2);

	ttusb_dec_send_command(dec, 0x50, sizeof(b), b, NULL, NULL);

		dvb_filter_pes2ts_init(&dec->a_pes2ts, dec->pid[DMX_PES_AUDIO],
			       ttusb_dec_audio_pes2ts_cb, dec);
		dvb_filter_pes2ts_init(&dec->v_pes2ts, dec->pid[DMX_PES_VIDEO],
			       ttusb_dec_video_pes2ts_cb, dec);
	dec->v_pes_length = 0;
	dec->v_pes_postbytes = 0;
}

static void ttusb_dec_process_pva(struct ttusb_dec *dec, u8 *pva, int length)
{
	if (length < 8) {
		printk("%s: packet too short - discarding\n", __FUNCTION__);
		return;
	}

	if (length > 8 + MAX_PVA_LENGTH) {
		printk("%s: packet too long - discarding\n", __FUNCTION__);
		return;
	}

	switch (pva[2]) {

	case 0x01: {		/* VideoStream */
		int prebytes = pva[5] & 0x03;
		int postbytes = (pva[5] & 0x0c) >> 2;
			u16 v_pes_payload_length;

		if (output_pva) {
			dec->video_filter->feed->cb.ts(pva, length, NULL, 0,
				&dec->video_filter->feed->feed.ts, DMX_OK);
			return;
		}

			if (dec->v_pes_postbytes > 0 &&
			    dec->v_pes_postbytes == prebytes) {
				memcpy(&dec->v_pes[dec->v_pes_length],
			       &pva[12], prebytes);

				dvb_filter_pes2ts(&dec->v_pes2ts, dec->v_pes,
					  dec->v_pes_length + prebytes, 1);
			}

		if (pva[5] & 0x10) {
				dec->v_pes[7] = 0x80;
				dec->v_pes[8] = 0x05;

			dec->v_pes[9] = 0x21 | ((pva[8] & 0xc0) >> 5);
			dec->v_pes[10] = ((pva[8] & 0x3f) << 2) |
					 ((pva[9] & 0xc0) >> 6);
				dec->v_pes[11] = 0x01 |
					 ((pva[9] & 0x3f) << 2) |
					 ((pva[10] & 0x80) >> 6);
			dec->v_pes[12] = ((pva[10] & 0x7f) << 1) |
					 ((pva[11] & 0xc0) >> 7);
			dec->v_pes[13] = 0x01 | ((pva[11] & 0x7f) << 1);

			memcpy(&dec->v_pes[14], &pva[12 + prebytes],
			       length - 12 - prebytes);
			dec->v_pes_length = 14 + length - 12 - prebytes;
			} else {
				dec->v_pes[7] = 0x00;
				dec->v_pes[8] = 0x00;

			memcpy(&dec->v_pes[9], &pva[8], length - 8);
			dec->v_pes_length = 9 + length - 8;
			}

			dec->v_pes_postbytes = postbytes;

			if (dec->v_pes[9 + dec->v_pes[8]] == 0x00 &&
			    dec->v_pes[10 + dec->v_pes[8]] == 0x00 &&
			    dec->v_pes[11 + dec->v_pes[8]] == 0x01)
				dec->v_pes[6] = 0x84;
			else
				dec->v_pes[6] = 0x80;

			v_pes_payload_length = htons(dec->v_pes_length - 6 +
						     postbytes);
			memcpy(&dec->v_pes[4], &v_pes_payload_length, 2);

			if (postbytes == 0)
				dvb_filter_pes2ts(&dec->v_pes2ts, dec->v_pes,
						  dec->v_pes_length, 1);

			break;
		}

	case 0x02:		/* MainAudioStream */
		if (output_pva) {
			dec->audio_filter->feed->cb.ts(pva, length, NULL, 0,
				&dec->audio_filter->feed->feed.ts, DMX_OK);
			return;
		}

		dvb_filter_pes2ts(&dec->a_pes2ts, &pva[8], length - 8,
				  pva[5] & 0x10);
		break;

	default:
		printk("%s: unknown PVA type: %02x.\n", __FUNCTION__,
		       pva[2]);
		break;
	}
}

static void ttusb_dec_process_filter(struct ttusb_dec *dec, u8 *packet,
				     int length)
{
	struct list_head *item;
	struct filter_info *finfo;
	struct dvb_demux_filter *filter = NULL;
	unsigned long flags;
	u8 sid;

	sid = packet[1];
	spin_lock_irqsave(&dec->filter_info_list_lock, flags);
	for (item = dec->filter_info_list.next; item != &dec->filter_info_list;
	     item = item->next) {
		finfo = list_entry(item, struct filter_info, filter_info_list);
		if (finfo->stream_id == sid) {
			filter = finfo->filter;
			break;
		}
	}
	spin_unlock_irqrestore(&dec->filter_info_list_lock, flags);

	if (filter)
		filter->feed->cb.sec(&packet[2], length - 2, NULL, 0,
				     &filter->filter, DMX_OK);
}

static void ttusb_dec_process_packet(struct ttusb_dec *dec)
{
	int i;
	u16 csum = 0;
	u16 packet_id;

	if (dec->packet_length % 2) {
		printk("%s: odd sized packet - discarding\n", __FUNCTION__);
		return;
	}

	for (i = 0; i < dec->packet_length; i += 2)
		csum ^= ((dec->packet[i] << 8) + dec->packet[i + 1]);

	if (csum) {
		printk("%s: checksum failed - discarding\n", __FUNCTION__);
		return;
	}

	packet_id = dec->packet[dec->packet_length - 4] << 8;
	packet_id += dec->packet[dec->packet_length - 3];

	if ((packet_id != dec->next_packet_id) && dec->next_packet_id) {
		printk("%s: warning: lost packets between %u and %u\n",
		       __FUNCTION__, dec->next_packet_id - 1, packet_id);
	}

	if (packet_id == 0xffff)
		dec->next_packet_id = 0x8000;
	else
		dec->next_packet_id = packet_id + 1;

	switch (dec->packet_type) {
	case TTUSB_DEC_PACKET_PVA:
		if (dec->pva_stream_count)
			ttusb_dec_process_pva(dec, dec->packet,
						 dec->packet_payload_length);
		break;

	case TTUSB_DEC_PACKET_SECTION:
		if (dec->filter_stream_count)
			ttusb_dec_process_filter(dec, dec->packet,
						 dec->packet_payload_length);
		break;

	case TTUSB_DEC_PACKET_EMPTY:
		break;
	}
}

static void swap_bytes(u8 *b, int length)
{
	u8 c;

	length -= length % 2;
	for (; length; b += 2, length -= 2) {
		c = *b;
		*b = *(b + 1);
		*(b + 1) = c;
	}
}

static void ttusb_dec_process_urb_frame(struct ttusb_dec * dec, u8 * b,
					int length)
{
	swap_bytes(b, length);

	while (length) {
		switch (dec->packet_state) {

		case 0:
		case 1:
		case 2:
			if (*b++ == 0xaa)
				dec->packet_state++;
			else
				dec->packet_state = 0;

			length--;
			break;

		case 3:
			if (*b == 0x00) {
				dec->packet_state++;
				dec->packet_length = 0;
			} else if (*b != 0xaa) {
				dec->packet_state = 0;
			}

			b++;
			length--;
			break;

		case 4:
			dec->packet[dec->packet_length++] = *b++;

			if (dec->packet_length == 2) {
				if (dec->packet[0] == 'A' &&
				    dec->packet[1] == 'V') {
					dec->packet_type =
						TTUSB_DEC_PACKET_PVA;
					dec->packet_state++;
				} else if (dec->packet[0] == 'S') {
					dec->packet_type =
						TTUSB_DEC_PACKET_SECTION;
					dec->packet_state++;
				} else if (dec->packet[0] == 0x00) {
					dec->packet_type =
						TTUSB_DEC_PACKET_EMPTY;
					dec->packet_payload_length = 2;
					dec->packet_state = 7;
			} else {
					printk("%s: unknown packet type: "
					       "%02x%02x\n", __FUNCTION__,
					       dec->packet[0], dec->packet[1]);
					dec->packet_state = 0;
				}
			}

			length--;
			break;

		case 5:
			dec->packet[dec->packet_length++] = *b++;

			if (dec->packet_type == TTUSB_DEC_PACKET_PVA &&
			    dec->packet_length == 8) {
				dec->packet_state++;
				dec->packet_payload_length = 8 +
					(dec->packet[6] << 8) +
					dec->packet[7];
			} else if (dec->packet_type ==
					TTUSB_DEC_PACKET_SECTION &&
				   dec->packet_length == 5) {
				dec->packet_state++;
				dec->packet_payload_length = 5 +
					((dec->packet[3] & 0x0f) << 8) +
					dec->packet[4];
			}

			length--;
			break;

		case 6: {
			int remainder = dec->packet_payload_length -
					dec->packet_length;

				if (length >= remainder) {
				memcpy(dec->packet + dec->packet_length,
					       b, remainder);
				dec->packet_length += remainder;
					b += remainder;
					length -= remainder;
				dec->packet_state++;
				} else {
				memcpy(&dec->packet[dec->packet_length],
					       b, length);
				dec->packet_length += length;
					length = 0;
				}

				break;
			}

		case 7: {
			int tail = 4;

			dec->packet[dec->packet_length++] = *b++;

			if (dec->packet_type == TTUSB_DEC_PACKET_SECTION &&
			    dec->packet_payload_length % 2)
				tail++;

			if (dec->packet_length ==
			    dec->packet_payload_length + tail) {
				ttusb_dec_process_packet(dec);
				dec->packet_state = 0;
			}

			length--;
			break;
		}

		default:
			printk("%s: illegal packet state encountered.\n",
			       __FUNCTION__);
			dec->packet_state = 0;
		}
	}
}

static void ttusb_dec_process_urb_frame_list(unsigned long data)
{
	struct ttusb_dec *dec = (struct ttusb_dec *)data;
	struct list_head *item;
	struct urb_frame *frame;
	unsigned long flags;

	while (1) {
		spin_lock_irqsave(&dec->urb_frame_list_lock, flags);
		if ((item = dec->urb_frame_list.next) != &dec->urb_frame_list) {
			frame = list_entry(item, struct urb_frame,
					   urb_frame_list);
			list_del(&frame->urb_frame_list);
		} else {
			spin_unlock_irqrestore(&dec->urb_frame_list_lock,
					       flags);
			return;
		}
		spin_unlock_irqrestore(&dec->urb_frame_list_lock, flags);

		ttusb_dec_process_urb_frame(dec, frame->data, frame->length);
		kfree(frame);
	}
}

static void ttusb_dec_process_urb(struct urb *urb, struct pt_regs *ptregs)
{
	struct ttusb_dec *dec = urb->context;

	if (!urb->status) {
		int i;

		for (i = 0; i < FRAMES_PER_ISO_BUF; i++) {
			struct usb_iso_packet_descriptor *d;

			u8 *b;
			int length;
			struct urb_frame *frame;

			d = &urb->iso_frame_desc[i];
			b = urb->transfer_buffer + d->offset;
			length = d->actual_length;

			if ((frame = kmalloc(sizeof(struct urb_frame),
					     GFP_ATOMIC))) {
				unsigned long flags;

				memcpy(frame->data, b, length);
				frame->length = length;

				spin_lock_irqsave(&dec->urb_frame_list_lock,
						     flags);
				list_add_tail(&frame->urb_frame_list,
					      &dec->urb_frame_list);
				spin_unlock_irqrestore(&dec->urb_frame_list_lock,
						       flags);

				tasklet_schedule(&dec->urb_tasklet);
			}
		}
	} else {
		 /* -ENOENT is expected when unlinking urbs */
		if (urb->status != -ENOENT)
			dprintk("%s: urb error: %d\n", __FUNCTION__,
				urb->status);
	}

	if (dec->iso_stream_count)
		usb_submit_urb(urb, GFP_ATOMIC);
}

static void ttusb_dec_setup_urbs(struct ttusb_dec *dec)
{
	int i, j, buffer_offset = 0;

	dprintk("%s\n", __FUNCTION__);

	for (i = 0; i < ISO_BUF_COUNT; i++) {
		int frame_offset = 0;
		struct urb *urb = dec->iso_urb[i];

		urb->dev = dec->udev;
		urb->context = dec;
		urb->complete = ttusb_dec_process_urb;
		urb->pipe = dec->in_pipe;
		urb->transfer_flags = URB_ISO_ASAP;
		urb->interval = 1;
		urb->number_of_packets = FRAMES_PER_ISO_BUF;
		urb->transfer_buffer_length = ISO_FRAME_SIZE *
					      FRAMES_PER_ISO_BUF;
		urb->transfer_buffer = dec->iso_buffer + buffer_offset;
		buffer_offset += ISO_FRAME_SIZE * FRAMES_PER_ISO_BUF;

		for (j = 0; j < FRAMES_PER_ISO_BUF; j++) {
			urb->iso_frame_desc[j].offset = frame_offset;
			urb->iso_frame_desc[j].length = ISO_FRAME_SIZE;
			frame_offset += ISO_FRAME_SIZE;
		}
	}
}

static void ttusb_dec_stop_iso_xfer(struct ttusb_dec *dec)
{
	int i;

	dprintk("%s\n", __FUNCTION__);

	if (down_interruptible(&dec->iso_sem))
		return;

	dec->iso_stream_count--;

	if (!dec->iso_stream_count) {
		for (i = 0; i < ISO_BUF_COUNT; i++)
			usb_unlink_urb(dec->iso_urb[i]);
	}

	up(&dec->iso_sem);
}

/* Setting the interface of the DEC tends to take down the USB communications
 * for a short period, so it's important not to call this function just before
 * trying to talk to it.
 */
static int ttusb_dec_set_interface(struct ttusb_dec *dec,
				   enum ttusb_dec_interface interface)
{
	int result = 0;
	u8 b[] = { 0x05 };

	if (interface != dec->interface) {
		switch (interface) {
		case TTUSB_DEC_INTERFACE_INITIAL:
			result = usb_set_interface(dec->udev, 0, 0);
			break;
		case TTUSB_DEC_INTERFACE_IN:
			result = ttusb_dec_send_command(dec, 0x80, sizeof(b),
							b, NULL, NULL);
			if (result)
				return result;
			result = usb_set_interface(dec->udev, 0, 7);
			break;
		case TTUSB_DEC_INTERFACE_OUT:
			result = usb_set_interface(dec->udev, 0, 1);
			break;
		}

		if (result)
			return result;

		dec->interface = interface;
	}

	return 0;
}

static int ttusb_dec_start_iso_xfer(struct ttusb_dec *dec)
{
	int i, result;

	dprintk("%s\n", __FUNCTION__);

	if (down_interruptible(&dec->iso_sem))
		return -EAGAIN;

	if (!dec->iso_stream_count) {
		ttusb_dec_setup_urbs(dec);

		dec->packet_state = 0;
		dec->v_pes_postbytes = 0;
		dec->next_packet_id = 0;

		for (i = 0; i < ISO_BUF_COUNT; i++) {
			if ((result = usb_submit_urb(dec->iso_urb[i],
						     GFP_ATOMIC))) {
				printk("%s: failed urb submission %d: "
				       "error %d\n", __FUNCTION__, i, result);

				while (i) {
					usb_unlink_urb(dec->iso_urb[i - 1]);
					i--;
				}

				up(&dec->iso_sem);
				return result;
			}
		}
	}

	dec->iso_stream_count++;

	up(&dec->iso_sem);

	return 0;
}

static int ttusb_dec_start_ts_feed(struct dvb_demux_feed *dvbdmxfeed)
{
	struct dvb_demux *dvbdmx = dvbdmxfeed->demux;
	struct ttusb_dec *dec = dvbdmx->priv;
	u8 b0[] = { 0x05 };
	int result = 0;

	dprintk("%s\n", __FUNCTION__);

	dprintk("  ts_type:");

	if (dvbdmxfeed->ts_type & TS_DECODER)
		dprintk(" TS_DECODER");

	if (dvbdmxfeed->ts_type & TS_PACKET)
		dprintk(" TS_PACKET");

	if (dvbdmxfeed->ts_type & TS_PAYLOAD_ONLY)
		dprintk(" TS_PAYLOAD_ONLY");

	dprintk("\n");

	switch (dvbdmxfeed->pes_type) {

	case DMX_TS_PES_VIDEO:
		dprintk("  pes_type: DMX_TS_PES_VIDEO\n");
		dec->pid[DMX_PES_PCR] = dvbdmxfeed->pid;
		dec->pid[DMX_PES_VIDEO] = dvbdmxfeed->pid;
		dec->video_filter = dvbdmxfeed->filter;
		ttusb_dec_set_pids(dec);
		break;

	case DMX_TS_PES_AUDIO:
		dprintk("  pes_type: DMX_TS_PES_AUDIO\n");
		dec->pid[DMX_PES_AUDIO] = dvbdmxfeed->pid;
		dec->audio_filter = dvbdmxfeed->filter;
		ttusb_dec_set_pids(dec);
		break;

	case DMX_TS_PES_TELETEXT:
		dec->pid[DMX_PES_TELETEXT] = dvbdmxfeed->pid;
		dprintk("  pes_type: DMX_TS_PES_TELETEXT\n");
		break;

	case DMX_TS_PES_PCR:
		dprintk("  pes_type: DMX_TS_PES_PCR\n");
		dec->pid[DMX_PES_PCR] = dvbdmxfeed->pid;
		ttusb_dec_set_pids(dec);
		break;

	case DMX_TS_PES_OTHER:
		dprintk("  pes_type: DMX_TS_PES_OTHER\n");
		break;

	default:
		dprintk("  pes_type: unknown (%d)\n", dvbdmxfeed->pes_type);
		return -EINVAL;

	}

	result = ttusb_dec_send_command(dec, 0x80, sizeof(b0), b0, NULL, NULL);
	if (result)
		return result;

	dec->pva_stream_count++;
	return ttusb_dec_start_iso_xfer(dec);
}

static int ttusb_dec_start_sec_feed(struct dvb_demux_feed *dvbdmxfeed)
{
	struct ttusb_dec *dec = dvbdmxfeed->demux->priv;
	u8 b0[] = { 0x00, 0x00, 0x00, 0x01,
		    0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00,
		    0x00, 0xff, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00,
		    0x00 };
	u16 pid;
	u8 c[COMMAND_PACKET_SIZE];
	int c_length;
	int result;
	struct filter_info *finfo;
	unsigned long flags;
	u8 x = 1;

	dprintk("%s\n", __FUNCTION__);

	pid = htons(dvbdmxfeed->pid);
	memcpy(&b0[0], &pid, 2);
	memcpy(&b0[4], &x, 1);
	memcpy(&b0[5], &dvbdmxfeed->filter->filter.filter_value[0], 1);

	result = ttusb_dec_send_command(dec, 0x60, sizeof(b0), b0,
					&c_length, c);

	if (!result) {
		if (c_length == 2) {
			if (!(finfo = kmalloc(sizeof(struct filter_info),
					      GFP_ATOMIC)))
				return -ENOMEM;

			finfo->stream_id = c[1];
			finfo->filter = dvbdmxfeed->filter;

			spin_lock_irqsave(&dec->filter_info_list_lock, flags);
			list_add_tail(&finfo->filter_info_list,
				      &dec->filter_info_list);
			spin_unlock_irqrestore(&dec->filter_info_list_lock,
					       flags);

			dvbdmxfeed->priv = finfo;

			dec->filter_stream_count++;
			return ttusb_dec_start_iso_xfer(dec);
		}

		return -EAGAIN;
	} else
		return result;
}

static int ttusb_dec_start_feed(struct dvb_demux_feed *dvbdmxfeed)
{
	struct dvb_demux *dvbdmx = dvbdmxfeed->demux;

	dprintk("%s\n", __FUNCTION__);

	if (!dvbdmx->dmx.frontend)
		return -EINVAL;

	dprintk("  pid: 0x%04X\n", dvbdmxfeed->pid);

	switch (dvbdmxfeed->type) {

	case DMX_TYPE_TS:
		return ttusb_dec_start_ts_feed(dvbdmxfeed);
		break;

	case DMX_TYPE_SEC:
		return ttusb_dec_start_sec_feed(dvbdmxfeed);
		break;

	default:
		dprintk("  type: unknown (%d)\n", dvbdmxfeed->type);
		return -EINVAL;

	}
}

static int ttusb_dec_stop_ts_feed(struct dvb_demux_feed *dvbdmxfeed)
{
	struct ttusb_dec *dec = dvbdmxfeed->demux->priv;
	u8 b0[] = { 0x00 };

	ttusb_dec_send_command(dec, 0x81, sizeof(b0), b0, NULL, NULL);

	dec->pva_stream_count--;

	ttusb_dec_stop_iso_xfer(dec);

	return 0;
}

static int ttusb_dec_stop_sec_feed(struct dvb_demux_feed *dvbdmxfeed)
{
	struct ttusb_dec *dec = dvbdmxfeed->demux->priv;
	u8 b0[] = { 0x00, 0x00 };
	struct filter_info *finfo = (struct filter_info *)dvbdmxfeed->priv;
	unsigned long flags;

	b0[1] = finfo->stream_id;
	spin_lock_irqsave(&dec->filter_info_list_lock, flags);
	list_del(&finfo->filter_info_list);
	spin_unlock_irqrestore(&dec->filter_info_list_lock, flags);
	kfree(finfo);
	ttusb_dec_send_command(dec, 0x62, sizeof(b0), b0, NULL, NULL);

	dec->filter_stream_count--;

	ttusb_dec_stop_iso_xfer(dec);

	return 0;
}

static int ttusb_dec_stop_feed(struct dvb_demux_feed *dvbdmxfeed)
{
	dprintk("%s\n", __FUNCTION__);

	switch (dvbdmxfeed->type) {
	case DMX_TYPE_TS:
		return ttusb_dec_stop_ts_feed(dvbdmxfeed);
		break;

	case DMX_TYPE_SEC:
		return ttusb_dec_stop_sec_feed(dvbdmxfeed);
		break;
	}

	return 0;
}

static void ttusb_dec_free_iso_urbs(struct ttusb_dec *dec)
{
	int i;

	dprintk("%s\n", __FUNCTION__);

	for (i = 0; i < ISO_BUF_COUNT; i++)
		if (dec->iso_urb[i])
			usb_free_urb(dec->iso_urb[i]);

	pci_free_consistent(NULL,
			    ISO_FRAME_SIZE * (FRAMES_PER_ISO_BUF *
					      ISO_BUF_COUNT),
			    dec->iso_buffer, dec->iso_dma_handle);
}

static int ttusb_dec_alloc_iso_urbs(struct ttusb_dec *dec)
{
	int i;

	dprintk("%s\n", __FUNCTION__);

	dec->iso_buffer = pci_alloc_consistent(NULL,
					       ISO_FRAME_SIZE *
					       (FRAMES_PER_ISO_BUF *
						ISO_BUF_COUNT),
				 	       &dec->iso_dma_handle);

	memset(dec->iso_buffer, 0,
	       sizeof(ISO_FRAME_SIZE * (FRAMES_PER_ISO_BUF * ISO_BUF_COUNT)));

	for (i = 0; i < ISO_BUF_COUNT; i++) {
		struct urb *urb;

		if (!(urb = usb_alloc_urb(FRAMES_PER_ISO_BUF, GFP_ATOMIC))) {
			ttusb_dec_free_iso_urbs(dec);
			return -ENOMEM;
		}

		dec->iso_urb[i] = urb;
	}

	ttusb_dec_setup_urbs(dec);

	return 0;
}

static void ttusb_dec_init_tasklet(struct ttusb_dec *dec)
{
	dec->urb_frame_list_lock = SPIN_LOCK_UNLOCKED;
	INIT_LIST_HEAD(&dec->urb_frame_list);
	tasklet_init(&dec->urb_tasklet, ttusb_dec_process_urb_frame_list,
		     (unsigned long)dec);
}

static void ttusb_dec_init_v_pes(struct ttusb_dec *dec)
{
	dprintk("%s\n", __FUNCTION__);

	dec->v_pes[0] = 0x00;
	dec->v_pes[1] = 0x00;
	dec->v_pes[2] = 0x01;
	dec->v_pes[3] = 0xe0;
}

static void ttusb_dec_init_usb(struct ttusb_dec *dec)
{
	dprintk("%s\n", __FUNCTION__);

	sema_init(&dec->usb_sem, 1);
	sema_init(&dec->iso_sem, 1);

	dec->command_pipe = usb_sndbulkpipe(dec->udev, COMMAND_PIPE);
	dec->result_pipe = usb_rcvbulkpipe(dec->udev, RESULT_PIPE);
	dec->in_pipe = usb_rcvisocpipe(dec->udev, IN_PIPE);
	dec->out_pipe = usb_sndisocpipe(dec->udev, OUT_PIPE);

	ttusb_dec_alloc_iso_urbs(dec);
}

static int ttusb_dec_boot_dsp(struct ttusb_dec *dec)
{
	int i, j, actual_len, result, size, trans_count;
	u8 b0[] = { 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00,
		    0x61, 0x00 };
	u8 b1[] = { 0x61 };
	u8 *b;
	char idstring[21];
	u8 *firmware = NULL;
	size_t firmware_size = 0;
	u16 firmware_csum = 0;
	u16 firmware_csum_ns;
	u32 firmware_size_nl;
#if defined(CONFIG_CRC32) || defined(CONFIG_CRC32_MODULE)
	u32 crc32_csum, crc32_check, tmp;
#endif
	const struct firmware *fw_entry = NULL;
	dprintk("%s\n", __FUNCTION__);

	if (request_firmware(&fw_entry, dec->firmware_name, &dec->udev->dev)) {
		printk(KERN_ERR "%s: Firmware (%s) unavailable.\n",
		       __FUNCTION__, dec->firmware_name);
		return 1;
	}

	firmware = fw_entry->data;
	firmware_size = fw_entry->size;

	if (firmware_size < 60) {
		printk("%s: firmware size too small for DSP code (%u < 60).\n",
			__FUNCTION__, firmware_size);
		return -1;
	}

	/* a 32 bit checksum over the first 56 bytes of the DSP Code is stored
	   at offset 56 of file, so use it to check if the firmware file is
	   valid. */
#if defined(CONFIG_CRC32) || defined(CONFIG_CRC32_MODULE)
	crc32_csum = crc32(~0L, firmware, 56) ^ ~0L;
	memcpy(&tmp, &firmware[56], 4);
	crc32_check = htonl(tmp);
	if (crc32_csum != crc32_check) {
		printk("%s: crc32 check of DSP code failed (calculated "
		       "0x%08x != 0x%08x in file), file invalid.\n",
			__FUNCTION__, crc32_csum, crc32_check);
		return -1;
	}
#endif
	memcpy(idstring, &firmware[36], 20);
	idstring[20] = '\0';
	printk(KERN_INFO "ttusb_dec: found DSP code \"%s\".\n", idstring);

	firmware_size_nl = htonl(firmware_size);
	memcpy(b0, &firmware_size_nl, 4);
	firmware_csum = crc16(~0, firmware, firmware_size) ^ ~0;
	firmware_csum_ns = htons(firmware_csum);
	memcpy(&b0[6], &firmware_csum_ns, 2);

	result = ttusb_dec_send_command(dec, 0x41, sizeof(b0), b0, NULL, NULL);

	if (result)
		return result;

	trans_count = 0;
	j = 0;

	b = kmalloc(ARM_PACKET_SIZE, GFP_KERNEL);
	if (b == NULL)
		return -ENOMEM;

	for (i = 0; i < firmware_size; i += COMMAND_PACKET_SIZE) {
		size = firmware_size - i;
		if (size > COMMAND_PACKET_SIZE)
			size = COMMAND_PACKET_SIZE;

		b[j + 0] = 0xaa;
		b[j + 1] = trans_count++;
		b[j + 2] = 0xf0;
		b[j + 3] = size;
		memcpy(&b[j + 4], &firmware[i], size);

		j += COMMAND_PACKET_SIZE + 4;

		if (j >= ARM_PACKET_SIZE) {
			result = usb_bulk_msg(dec->udev, dec->command_pipe, b,
					      ARM_PACKET_SIZE, &actual_len,
					      HZ / 10);
			j = 0;
		} else if (size < COMMAND_PACKET_SIZE) {
			result = usb_bulk_msg(dec->udev, dec->command_pipe, b,
					      j - COMMAND_PACKET_SIZE + size,
					      &actual_len, HZ / 10);
		}
	}

	result = ttusb_dec_send_command(dec, 0x43, sizeof(b1), b1, NULL, NULL);

	kfree(b);

	return result;
}

static int ttusb_dec_init_stb(struct ttusb_dec *dec)
{
	int result;
	unsigned int mode, model, version;

	dprintk("%s\n", __FUNCTION__);

	result = ttusb_dec_get_stb_state(dec, &mode, &model, &version);

	if (!result) {
		if (!mode) {
			if (version == 0xABCDEFAB)
				printk(KERN_INFO "ttusb_dec: no version "
				       "info in Firmware\n");
			else
				printk(KERN_INFO "ttusb_dec: Firmware "
				       "%x.%02x%c%c\n",
				       version >> 24, (version >> 16) & 0xff,
				       (version >> 8) & 0xff, version & 0xff);

			result = ttusb_dec_boot_dsp(dec);
			if (result)
				return result;
		else
				return 1;
		} else {
			/* We can't trust the USB IDs that some firmwares
			   give the box */
			switch (model) {
			case 0x00070008:
			case 0x0007000c:
				ttusb_dec_set_model(dec, TTUSB_DEC3000S);
				break;
			case 0x00070009:
				ttusb_dec_set_model(dec, TTUSB_DEC2000T);
				break;
			case 0x00070011:
				ttusb_dec_set_model(dec, TTUSB_DEC2540T);
				break;
			default:
				printk(KERN_ERR "%s: unknown model returned "
				       "by firmware (%08x) - please report\n",
				       __FUNCTION__, model);
				return -1;
				break;
			}

			if (version >= 0x01770000)
				dec->can_playback = 1;

			return 0;
	}
	}
	else
		return result;
}

static int ttusb_dec_init_dvb(struct ttusb_dec *dec)
{
	int result;

	dprintk("%s\n", __FUNCTION__);

	if ((result = dvb_register_adapter(&dec->adapter,
					   dec->model_name, THIS_MODULE)) < 0) {
		printk("%s: dvb_register_adapter failed: error %d\n",
		       __FUNCTION__, result);

		return result;
	}

	dec->demux.dmx.capabilities = DMX_TS_FILTERING | DMX_SECTION_FILTERING;

	dec->demux.priv = (void *)dec;
	dec->demux.filternum = 31;
	dec->demux.feednum = 31;
	dec->demux.start_feed = ttusb_dec_start_feed;
	dec->demux.stop_feed = ttusb_dec_stop_feed;
	dec->demux.write_to_decoder = NULL;

	if ((result = dvb_dmx_init(&dec->demux)) < 0) {
		printk("%s: dvb_dmx_init failed: error %d\n", __FUNCTION__,
		       result);

		dvb_unregister_adapter(dec->adapter);

		return result;
	}

	dec->dmxdev.filternum = 32;
	dec->dmxdev.demux = &dec->demux.dmx;
	dec->dmxdev.capabilities = 0;

	if ((result = dvb_dmxdev_init(&dec->dmxdev, dec->adapter)) < 0) {
		printk("%s: dvb_dmxdev_init failed: error %d\n",
		       __FUNCTION__, result);

		dvb_dmx_release(&dec->demux);
		dvb_unregister_adapter(dec->adapter);

		return result;
	}

	dec->frontend.source = DMX_FRONTEND_0;

	if ((result = dec->demux.dmx.add_frontend(&dec->demux.dmx,
						  &dec->frontend)) < 0) {
		printk("%s: dvb_dmx_init failed: error %d\n", __FUNCTION__,
		       result);

		dvb_dmxdev_release(&dec->dmxdev);
		dvb_dmx_release(&dec->demux);
		dvb_unregister_adapter(dec->adapter);

		return result;
	}

	if ((result = dec->demux.dmx.connect_frontend(&dec->demux.dmx,
						      &dec->frontend)) < 0) {
		printk("%s: dvb_dmx_init failed: error %d\n", __FUNCTION__,
		       result);

		dec->demux.dmx.remove_frontend(&dec->demux.dmx, &dec->frontend);
		dvb_dmxdev_release(&dec->dmxdev);
		dvb_dmx_release(&dec->demux);
		dvb_unregister_adapter(dec->adapter);

		return result;
	}

	dvb_net_init(dec->adapter, &dec->dvb_net, &dec->demux.dmx);

	return 0;
}

static void ttusb_dec_exit_dvb(struct ttusb_dec *dec)
{
	dprintk("%s\n", __FUNCTION__);

	dvb_net_release(&dec->dvb_net);
	dec->demux.dmx.close(&dec->demux.dmx);
	dec->demux.dmx.remove_frontend(&dec->demux.dmx, &dec->frontend);
	dvb_dmxdev_release(&dec->dmxdev);
	dvb_dmx_release(&dec->demux);
	dvb_unregister_adapter(dec->adapter);
}

static void ttusb_dec_exit_usb(struct ttusb_dec *dec)
{
	int i;

	dprintk("%s\n", __FUNCTION__);

	dec->iso_stream_count = 0;

	for (i = 0; i < ISO_BUF_COUNT; i++)
		usb_unlink_urb(dec->iso_urb[i]);

	ttusb_dec_free_iso_urbs(dec);
}

static void ttusb_dec_exit_tasklet(struct ttusb_dec *dec)
{
	struct list_head *item;
	struct urb_frame *frame;

	tasklet_kill(&dec->urb_tasklet);

	while ((item = dec->urb_frame_list.next) != &dec->urb_frame_list) {
		frame = list_entry(item, struct urb_frame, urb_frame_list);
		list_del(&frame->urb_frame_list);
		kfree(frame);
	}
}

static int ttusb_dec_2000t_frontend_ioctl(struct dvb_frontend *fe, unsigned int cmd,
				  void *arg)
{
	struct ttusb_dec *dec = fe->data;

	dprintk("%s\n", __FUNCTION__);

	switch (cmd) {

	case FE_GET_INFO:
		dprintk("%s: FE_GET_INFO\n", __FUNCTION__);
		memcpy(arg, dec->frontend_info,
		       sizeof (struct dvb_frontend_info));
		break;

	case FE_READ_STATUS: {
			fe_status_t *status = (fe_status_t *)arg;
			dprintk("%s: FE_READ_STATUS\n", __FUNCTION__);
			*status = FE_HAS_SIGNAL | FE_HAS_VITERBI |
				  FE_HAS_SYNC | FE_HAS_CARRIER | FE_HAS_LOCK;
			break;
		}

	case FE_READ_BER: {
			u32 *ber = (u32 *)arg;
			dprintk("%s: FE_READ_BER\n", __FUNCTION__);
			*ber = 0;
			return -ENOSYS;
			break;
		}

	case FE_READ_SIGNAL_STRENGTH: {
			dprintk("%s: FE_READ_SIGNAL_STRENGTH\n", __FUNCTION__);
			*(s32 *)arg = 0xFF;
			return -ENOSYS;
			break;
		}

	case FE_READ_SNR:
		dprintk("%s: FE_READ_SNR\n", __FUNCTION__);
		*(s32 *)arg = 0;
		return -ENOSYS;
		break;

	case FE_READ_UNCORRECTED_BLOCKS:
		dprintk("%s: FE_READ_UNCORRECTED_BLOCKS\n", __FUNCTION__);
		*(u32 *)arg = 0;
		return -ENOSYS;
		break;

	case FE_SET_FRONTEND: {
			struct dvb_frontend_parameters *p =
				(struct dvb_frontend_parameters *)arg;
		u8 b[] = { 0x00, 0x00, 0x00, 0x03,
			   0x00, 0x00, 0x00, 0x00,
			   0x00, 0x00, 0x00, 0x01,
			   0x00, 0x00, 0x00, 0xff,
			   0x00, 0x00, 0x00, 0xff };
			u32 freq;

			dprintk("%s: FE_SET_FRONTEND\n", __FUNCTION__);

			dprintk("            frequency->%d\n", p->frequency);
			dprintk("            symbol_rate->%d\n",
				p->u.qam.symbol_rate);
			dprintk("            inversion->%d\n", p->inversion);

			freq = htonl(p->frequency / 1000);
			memcpy(&b[4], &freq, sizeof (u32));
			ttusb_dec_send_command(dec, 0x71, sizeof(b), b, NULL, NULL);

			break;
		}

	case FE_GET_FRONTEND:
		dprintk("%s: FE_GET_FRONTEND\n", __FUNCTION__);
		break;

	case FE_SLEEP:
		dprintk("%s: FE_SLEEP\n", __FUNCTION__);
		return -ENOSYS;
		break;

	case FE_INIT:
		dprintk("%s: FE_INIT\n", __FUNCTION__);
		break;

	default:
		dprintk("%s: unknown IOCTL (0x%X)\n", __FUNCTION__, cmd);
		return -EINVAL;

	}

	return 0;
}

static int ttusb_dec_3000s_frontend_ioctl(struct dvb_frontend *fe,
					  unsigned int cmd, void *arg)
{
	struct ttusb_dec *dec = fe->data;

	dprintk("%s\n", __FUNCTION__);

	switch (cmd) {

	case FE_GET_INFO:
		dprintk("%s: FE_GET_INFO\n", __FUNCTION__);
		memcpy(arg, dec->frontend_info,
		       sizeof (struct dvb_frontend_info));
		break;

	case FE_READ_STATUS: {
			fe_status_t *status = (fe_status_t *)arg;
			dprintk("%s: FE_READ_STATUS\n", __FUNCTION__);
			*status = FE_HAS_SIGNAL | FE_HAS_VITERBI |
				  FE_HAS_SYNC | FE_HAS_CARRIER | FE_HAS_LOCK;
			break;
		}

	case FE_READ_BER: {
			u32 *ber = (u32 *)arg;
			dprintk("%s: FE_READ_BER\n", __FUNCTION__);
			*ber = 0;
			return -ENOSYS;
			break;
		}

	case FE_READ_SIGNAL_STRENGTH: {
			dprintk("%s: FE_READ_SIGNAL_STRENGTH\n", __FUNCTION__);
			*(s32 *)arg = 0xFF;
			return -ENOSYS;
			break;
		}

	case FE_READ_SNR:
		dprintk("%s: FE_READ_SNR\n", __FUNCTION__);
		*(s32 *)arg = 0;
		return -ENOSYS;
		break;

	case FE_READ_UNCORRECTED_BLOCKS:
		dprintk("%s: FE_READ_UNCORRECTED_BLOCKS\n", __FUNCTION__);
		*(u32 *)arg = 0;
		return -ENOSYS;
		break;

	case FE_SET_FRONTEND: {
			struct dvb_frontend_parameters *p =
				(struct dvb_frontend_parameters *)arg;
		u8 b[] = { 0x00, 0x00, 0x00, 0x01,
			   0x00, 0x00, 0x00, 0x00,
			   0x00, 0x00, 0x00, 0x01,
			   0x00, 0x00, 0x00, 0x00,
			   0x00, 0x00, 0x00, 0x00,
			   0x00, 0x00, 0x00, 0x00,
			   0x00, 0x00, 0x00, 0x00,
			   0x00, 0x00, 0x00, 0x00,
			   0x00, 0x00, 0x00, 0x00,
			   0x00, 0x00, 0x00, 0x00 };
			u32 freq;
			u32 sym_rate;
			u32 band;
		u32 lnb_voltage;

			dprintk("%s: FE_SET_FRONTEND\n", __FUNCTION__);

			dprintk("            frequency->%d\n", p->frequency);
			dprintk("            symbol_rate->%d\n",
				p->u.qam.symbol_rate);
			dprintk("            inversion->%d\n", p->inversion);

		freq = htonl(p->frequency * 1000 +
		       (dec->hi_band ? LOF_HI : LOF_LO));
			memcpy(&b[4], &freq, sizeof(u32));
			sym_rate = htonl(p->u.qam.symbol_rate);
			memcpy(&b[12], &sym_rate, sizeof(u32));
			band = htonl(dec->hi_band ? LOF_HI : LOF_LO);
			memcpy(&b[24], &band, sizeof(u32));
		lnb_voltage = htonl(dec->voltage);
		memcpy(&b[28], &lnb_voltage, sizeof(u32));

			ttusb_dec_send_command(dec, 0x71, sizeof(b), b, NULL, NULL);

			break;
		}

	case FE_GET_FRONTEND:
		dprintk("%s: FE_GET_FRONTEND\n", __FUNCTION__);
		break;

	case FE_SLEEP:
		dprintk("%s: FE_SLEEP\n", __FUNCTION__);
		return -ENOSYS;
		break;

	case FE_INIT:
		dprintk("%s: FE_INIT\n", __FUNCTION__);
		break;

	case FE_DISEQC_SEND_MASTER_CMD:
		dprintk("%s: FE_DISEQC_SEND_MASTER_CMD\n", __FUNCTION__);
		break;

	case FE_DISEQC_SEND_BURST:
		dprintk("%s: FE_DISEQC_SEND_BURST\n", __FUNCTION__);
		break;

	case FE_SET_TONE: {
			fe_sec_tone_mode_t tone = (fe_sec_tone_mode_t)arg;
			dprintk("%s: FE_SET_TONE\n", __FUNCTION__);
			dec->hi_band = (SEC_TONE_ON == tone);
			break;
		}

	case FE_SET_VOLTAGE:
		dprintk("%s: FE_SET_VOLTAGE\n", __FUNCTION__);
		switch ((fe_sec_voltage_t) arg) {
		case SEC_VOLTAGE_13:
			dec->voltage = 13;
			break;
		case SEC_VOLTAGE_18:
			dec->voltage = 18;
			break;
		default:
			return -EINVAL;
			break;
		}
		break;

	default:
		dprintk("%s: unknown IOCTL (0x%X)\n", __FUNCTION__, cmd);
		return -EINVAL;

	}

	return 0;
}

static void ttusb_dec_init_frontend(struct ttusb_dec *dec)
{
	dec->i2c_bus.adapter = dec->adapter;

	dvb_register_frontend(dec->frontend_ioctl, &dec->i2c_bus, (void *)dec,
			      dec->frontend_info);
}

static void ttusb_dec_exit_frontend(struct ttusb_dec *dec)
{
	dvb_unregister_frontend(dec->frontend_ioctl, &dec->i2c_bus);
}

static void ttusb_dec_init_filters(struct ttusb_dec *dec)
{
	INIT_LIST_HEAD(&dec->filter_info_list);
	dec->filter_info_list_lock = SPIN_LOCK_UNLOCKED;
}

static void ttusb_dec_exit_filters(struct ttusb_dec *dec)
{
	struct list_head *item;
	struct filter_info *finfo;

	while ((item = dec->filter_info_list.next) != &dec->filter_info_list) {
		finfo = list_entry(item, struct filter_info, filter_info_list);
		list_del(&finfo->filter_info_list);
		kfree(finfo);
	}
}

static int ttusb_dec_probe(struct usb_interface *intf,
			   const struct usb_device_id *id)
{
	struct usb_device *udev;
	struct ttusb_dec *dec;

	dprintk("%s\n", __FUNCTION__);

	udev = interface_to_usbdev(intf);

	if (!(dec = kmalloc(sizeof(struct ttusb_dec), GFP_KERNEL))) {
		printk("%s: couldn't allocate memory.\n", __FUNCTION__);
		return -ENOMEM;
	}

	usb_set_intfdata(intf, (void *)dec);

	memset(dec, 0, sizeof(struct ttusb_dec));

	switch (id->idProduct) {
		case 0x1006:
		ttusb_dec_set_model(dec, TTUSB_DEC3000S);
			break;

		case 0x1008:
		ttusb_dec_set_model(dec, TTUSB_DEC2000T);
		break;

	case 0x1009:
		ttusb_dec_set_model(dec, TTUSB_DEC2540T);
			break;
	}

	dec->udev = udev;

	ttusb_dec_init_usb(dec);
	if (ttusb_dec_init_stb(dec)) {
		ttusb_dec_exit_usb(dec);
		return 0;
	}
	ttusb_dec_init_dvb(dec);
	ttusb_dec_init_frontend(dec);
	ttusb_dec_init_v_pes(dec);
	ttusb_dec_init_filters(dec);
	ttusb_dec_init_tasklet(dec);

	dec->active = 1;

	ttusb_dec_set_interface(dec, TTUSB_DEC_INTERFACE_IN);

	return 0;
}

static void ttusb_dec_disconnect(struct usb_interface *intf)
{
	struct ttusb_dec *dec = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);

	dprintk("%s\n", __FUNCTION__);

	if (dec->active) {
	ttusb_dec_exit_tasklet(dec);
		ttusb_dec_exit_filters(dec);
	ttusb_dec_exit_usb(dec);
		ttusb_dec_exit_frontend(dec);
	ttusb_dec_exit_dvb(dec);
	}

	kfree(dec);
}

static void ttusb_dec_set_model(struct ttusb_dec *dec,
				enum ttusb_dec_model model)
{
	dec->model = model;

	switch (model) {
	case TTUSB_DEC2000T:
		dec->model_name = "DEC2000-t";
		dec->firmware_name = "dvb-ttusb-dec-2000t.fw";
		dec->frontend_info = &dec2000t_frontend_info;
		dec->frontend_ioctl = ttusb_dec_2000t_frontend_ioctl;
		break;

	case TTUSB_DEC2540T:
		dec->model_name = "DEC2540-t";
		dec->firmware_name = "dvb-ttusb-dec-2540t.fw";
		dec->frontend_info = &dec2000t_frontend_info;
		dec->frontend_ioctl = ttusb_dec_2000t_frontend_ioctl;
		break;

	case TTUSB_DEC3000S:
		dec->model_name = "DEC3000-s";
		dec->firmware_name = "dvb-ttusb-dec-3000s.fw";
		dec->frontend_info = &dec3000s_frontend_info;
		dec->frontend_ioctl = ttusb_dec_3000s_frontend_ioctl;
		break;
	}
}

static struct usb_device_id ttusb_dec_table[] = {
	{USB_DEVICE(0x0b48, 0x1006)},	/* DEC3000-s */
	/*{USB_DEVICE(0x0b48, 0x1007)},	   Unconfirmed */
	{USB_DEVICE(0x0b48, 0x1008)},	/* DEC2000-t */
	{USB_DEVICE(0x0b48, 0x1009)},	/* DEC2540-t */
	{}
};

static struct usb_driver ttusb_dec_driver = {
	.name		= "ttusb-dec",
      .probe		= ttusb_dec_probe,
      .disconnect	= ttusb_dec_disconnect,
      .id_table		= ttusb_dec_table,
};

static int __init ttusb_dec_init(void)
{
	int result;

	if ((result = usb_register(&ttusb_dec_driver)) < 0) {
		printk("%s: initialisation failed: error %d.\n", __FUNCTION__,
		       result);
		return result;
	}

	return 0;
}

static void __exit ttusb_dec_exit(void)
{
	usb_deregister(&ttusb_dec_driver);
}

module_init(ttusb_dec_init);
module_exit(ttusb_dec_exit);

MODULE_AUTHOR("Alex Woods <linux-dvb@giblets.org>");
MODULE_DESCRIPTION(DRIVER_NAME);
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(usb, ttusb_dec_table);

MODULE_PARM(debug, "i");
MODULE_PARM_DESC(debug, "Debug level");
MODULE_PARM(output_pva, "i");
MODULE_PARM_DESC(output_pva, "Output PVA from dvr device");
