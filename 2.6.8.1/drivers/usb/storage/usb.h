/* Driver for USB Mass Storage compliant devices
 * Main Header File
 *
 * $Id: usb.h,v 1.21 2002/04/21 02:57:59 mdharm Exp $
 *
 * Current development and maintenance by:
 *   (c) 1999-2002 Matthew Dharm (mdharm-usb@one-eyed-alien.net)
 *
 * Initial work by:
 *   (c) 1999 Michael Gee (michael@linuxspecific.com)
 *
 * This driver is based on the 'USB Mass Storage Class' document. This
 * describes in detail the protocol used to communicate with such
 * devices.  Clearly, the designers had SCSI and ATAPI commands in
 * mind when they created this document.  The commands are all very
 * similar to commands in the SCSI-II and ATAPI specifications.
 *
 * It is important to note that in a number of cases this class
 * exhibits class-specific exemptions from the USB specification.
 * Notably the usage of NAK, STALL and ACK differs from the norm, in
 * that they are used to communicate wait, failed and OK on commands.
 *
 * Also, for certain devices, the interrupt endpoint is used to convey
 * status of a command.
 *
 * Please see http://www.one-eyed-alien.net/~mdharm/linux-usb for more
 * information about this driver.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef _USB_H_
#define _USB_H_

#include <linux/usb.h>
#include <linux/blkdev.h>
#include <linux/smp_lock.h>
#include <linux/completion.h>
#include "scsi.h"
#include <scsi/scsi_host.h>

struct us_data;

/*
 * Unusual device list definitions 
 */

struct us_unusual_dev {
	const char* vendorName;
	const char* productName;
	__u8  useProtocol;
	__u8  useTransport;
	int (*initFunction)(struct us_data *);
	unsigned int flags;
};

/* Flag definitions: these entries are static */
#define US_FL_SINGLE_LUN      0x00000001 /* allow access to only LUN 0	    */
#define US_FL_MODE_XLATE      0          /* [no longer used]                */
#define US_FL_NEED_OVERRIDE   0x00000004 /* unusual_devs entry is necessary */
#define US_FL_IGNORE_SER      0		 /* [no longer used]		    */
#define US_FL_SCM_MULT_TARG   0x00000020 /* supports multiple targets	    */
#define US_FL_FIX_INQUIRY     0x00000040 /* INQUIRY response needs faking   */
#define US_FL_FIX_CAPACITY    0x00000080 /* READ CAPACITY response too big  */

/* Dynamic flag definitions: used in set_bit() etc. */
#define US_FLIDX_URB_ACTIVE	18  /* 0x00040000  current_urb is in use  */
#define US_FLIDX_SG_ACTIVE	19  /* 0x00080000  current_sg is in use   */
#define US_FLIDX_ABORTING	20  /* 0x00100000  abort is in progress   */
#define US_FLIDX_DISCONNECTING	21  /* 0x00200000  disconnect in progress */
#define ABORTING_OR_DISCONNECTING	((1UL << US_FLIDX_ABORTING) | \
					 (1UL << US_FLIDX_DISCONNECTING))
#define US_FLIDX_RESETTING	22  /* 0x00400000  device reset in progress */


/* processing state machine states */
#define US_STATE_IDLE		1
#define US_STATE_RUNNING	2
#define US_STATE_RESETTING	3
#define US_STATE_ABORTING	4

#define USB_STOR_STRING_LEN 32

/*
 * We provide a DMA-mapped I/O buffer for use with small USB transfers.
 * It turns out that CB[I] needs a 12-byte buffer and Bulk-only needs a
 * 31-byte buffer.  But Freecom needs a 64-byte buffer, so that's the
 * size we'll allocate.
 */

#define US_IOBUF_SIZE		64	/* Size of the DMA-mapped I/O buffer */

typedef int (*trans_cmnd)(Scsi_Cmnd*, struct us_data*);
typedef int (*trans_reset)(struct us_data*);
typedef void (*proto_cmnd)(Scsi_Cmnd*, struct us_data*);
typedef void (*extra_data_destructor)(void *);	 /* extra data destructor   */

/* we allocate one of these for every device that we remember */
struct us_data {
	/* The device we're working with
	 * It's important to note:
	 *    (o) you must hold dev_semaphore to change pusb_dev
	 */
	struct semaphore	dev_semaphore;	 /* protect pusb_dev */
	struct usb_device	*pusb_dev;	 /* this usb_device */
	struct usb_interface	*pusb_intf;	 /* this interface */
	struct us_unusual_dev   *unusual_dev;	 /* device-filter entry     */
	unsigned long		flags;		 /* from filter initially */
	unsigned int		send_bulk_pipe;	 /* cached pipe values */
	unsigned int		recv_bulk_pipe;
	unsigned int		send_ctrl_pipe;
	unsigned int		recv_ctrl_pipe;
	unsigned int		recv_intr_pipe;

	/* information about the device */
	char			vendor[USB_STOR_STRING_LEN];
	char			product[USB_STOR_STRING_LEN];
	char			serial[USB_STOR_STRING_LEN];
	char			*transport_name;
	char			*protocol_name;
	u8			subclass;
	u8			protocol;
	u8			max_lun;

	u8			ifnum;		 /* interface number   */
	u8			ep_bInterval;	 /* interrupt interval */ 

	/* function pointers for this device */
	trans_cmnd		transport;	 /* transport function	   */
	trans_reset		transport_reset; /* transport device reset */
	proto_cmnd		proto_handler;	 /* protocol handler	   */

	/* SCSI interfaces */
	struct Scsi_Host	*host;		 /* our dummy host data */
	Scsi_Cmnd		*srb;		 /* current srb		*/

	/* thread information */
	int			pid;		 /* control thread	 */
	int			sm_state;	 /* what we are doing	 */

	/* control and bulk communications data */
	struct urb		*current_urb;	 /* USB requests	 */
	struct usb_ctrlrequest	*cr;		 /* control requests	 */
	struct usb_sg_request	current_sg;	 /* scatter-gather req.  */
	unsigned char		*iobuf;		 /* I/O buffer		 */
	dma_addr_t		cr_dma;		 /* buffer DMA addresses */
	dma_addr_t		iobuf_dma;

	/* mutual exclusion and synchronization structures */
	struct semaphore	sema;		 /* to sleep thread on   */
	struct completion	notify;		 /* thread begin/end	 */
	wait_queue_head_t	dev_reset_wait;  /* wait during reset    */

	/* subdriver information */
	void			*extra;		 /* Any extra data          */
	extra_data_destructor	extra_destructor;/* extra data destructor   */
};

/* The structure which defines our driver */
extern struct usb_driver usb_storage_driver;

/* Function to fill an inquiry response. See usb.c for details */
extern void fill_inquiry_response(struct us_data *us,
	unsigned char *data, unsigned int data_len);

/* The scsi_lock() and scsi_unlock() macros protect the sm_state and the
 * single queue element srb for write access */
#define scsi_unlock(host)	spin_unlock_irq(host->host_lock)
#define scsi_lock(host)		spin_lock_irq(host->host_lock)

#endif
