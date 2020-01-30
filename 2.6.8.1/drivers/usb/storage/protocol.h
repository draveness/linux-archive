/* Driver for USB Mass Storage compliant devices
 * Protocol Functions Header File
 *
 * $Id: protocol.h,v 1.4 2001/02/13 07:10:03 mdharm Exp $
 *
 * Current development and maintenance by:
 *   (c) 1999, 2000 Matthew Dharm (mdharm-usb@one-eyed-alien.net)
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

#ifndef _PROTOCOL_H_
#define _PROTOCOL_H_

#include <linux/blkdev.h>
#include "scsi.h"
#include "usb.h"

/* Sub Classes */

#define US_SC_RBC	0x01		/* Typically, flash devices */
#define US_SC_8020	0x02		/* CD-ROM */
#define US_SC_QIC	0x03		/* QIC-157 Tapes */
#define US_SC_UFI	0x04		/* Floppy */
#define US_SC_8070	0x05		/* Removable media */
#define US_SC_SCSI	0x06		/* Transparent */
#define US_SC_ISD200    0x07		/* ISD200 ATA */
#define US_SC_MIN	US_SC_RBC
#define US_SC_MAX	US_SC_ISD200

#define US_SC_DEVICE	0xff		/* Use device's value */

/* Protocol handling routines */
extern void usb_stor_ATAPI_command(Scsi_Cmnd*, struct us_data*);
extern void usb_stor_qic157_command(Scsi_Cmnd*, struct us_data*);
extern void usb_stor_ufi_command(Scsi_Cmnd*, struct us_data*);
extern void usb_stor_transparent_scsi_command(Scsi_Cmnd*, struct us_data*);

/* Scsi_Cmnd transfer buffer access utilities */
enum xfer_buf_dir	{TO_XFER_BUF, FROM_XFER_BUF};

extern unsigned int usb_stor_access_xfer_buf(unsigned char *buffer,
	unsigned int buflen, Scsi_Cmnd *srb, unsigned int *index,
	unsigned int *offset, enum xfer_buf_dir dir);

extern void usb_stor_set_xfer_buf(unsigned char *buffer,
	unsigned int buflen, Scsi_Cmnd *srb);
#endif
