/*
 * video1394.h - driver for OHCI 1394 boards
 * Copyright (C)1999,2000 Sebastien Rougeaux <sebastien.rougeaux@anu.edu.au>
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
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef _VIDEO_1394_H
#define _VIDEO_1394_H

#define VIDEO1394_DRIVER_NAME "video1394"

#define VIDEO1394_MAX_SIZE 0x4000000

enum {
	VIDEO1394_BUFFER_FREE = 0,
	VIDEO1394_BUFFER_QUEUED,
	VIDEO1394_BUFFER_READY
};

enum {
	VIDEO1394_LISTEN_CHANNEL = 0,
	VIDEO1394_UNLISTEN_CHANNEL,
	VIDEO1394_LISTEN_QUEUE_BUFFER,
	VIDEO1394_LISTEN_WAIT_BUFFER,
	VIDEO1394_TALK_CHANNEL,
	VIDEO1394_UNTALK_CHANNEL,
	VIDEO1394_TALK_QUEUE_BUFFER,
	VIDEO1394_TALK_WAIT_BUFFER
};

#define VIDEO1394_SYNC_FRAMES         0x00000001
#define VIDEO1394_INCLUDE_ISO_HEADERS 0x00000002

struct video1394_mmap {
	int channel;
	int sync_tag;
	int nb_buffers;
	int buf_size;
	int packet_size;
	int fps;
	int flags;
};

struct video1394_wait {
	int channel;
	int buffer;
};


#endif
