/* Driver for USB Mass Storage compliant devices
 *
 * $Id: protocol.c,v 1.14 2002/04/22 03:39:43 mdharm Exp $
 *
 * Current development and maintenance by:
 *   (c) 1999-2002 Matthew Dharm (mdharm-usb@one-eyed-alien.net)
 *
 * Developed with the assistance of:
 *   (c) 2000 David L. Brown, Jr. (usb-storage@davidb.org)
 *   (c) 2002 Alan Stern (stern@rowland.org)
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

#include <linux/highmem.h>
#include "protocol.h"
#include "usb.h"
#include "debug.h"
#include "scsiglue.h"
#include "transport.h"

/***********************************************************************
 * Helper routines
 ***********************************************************************/

/*
 * Fix-up the return data from an INQUIRY command to show 
 * ANSI SCSI rev 2 so we don't confuse the SCSI layers above us
 */
static void fix_inquiry_data(Scsi_Cmnd *srb)
{
	unsigned char databuf[3];
	unsigned int index, offset;

	/* verify that it's an INQUIRY command */
	if (srb->cmnd[0] != INQUIRY)
		return;

	index = offset = 0;
	if (usb_stor_access_xfer_buf(databuf, sizeof(databuf), srb,
			&index, &offset, FROM_XFER_BUF) != sizeof(databuf))
		return;

	if ((databuf[2] & 7) == 2)
		return;

	US_DEBUGP("Fixing INQUIRY data to show SCSI rev 2 - was %d\n",
		  databuf[2] & 7);

	/* Change the SCSI revision number */
	databuf[2] = (databuf[2] & ~7) | 2;

	index = offset = 0;
	usb_stor_access_xfer_buf(databuf, sizeof(databuf), srb,
			&index, &offset, TO_XFER_BUF);
}

/*
 * Fix-up the return data from a READ CAPACITY command. My Feiya reader
 * returns a value that is 1 too large.
 */
static void fix_read_capacity(Scsi_Cmnd *srb)
{
	unsigned int index, offset;
	u32 c;
	unsigned long capacity;

	/* verify that it's a READ CAPACITY command */
	if (srb->cmnd[0] != READ_CAPACITY)
		return;

	index = offset = 0;
	if (usb_stor_access_xfer_buf((unsigned char *) &c, 4, srb,
			&index, &offset, FROM_XFER_BUF) != 4)
		return;

	capacity = be32_to_cpu(c);
	US_DEBUGP("US: Fixing capacity: from %ld to %ld\n",
	       capacity+1, capacity);
	c = cpu_to_be32(capacity - 1);

	index = offset = 0;
	usb_stor_access_xfer_buf((unsigned char *) &c, 4, srb,
			&index, &offset, TO_XFER_BUF);
}

/***********************************************************************
 * Protocol routines
 ***********************************************************************/

void usb_stor_qic157_command(Scsi_Cmnd *srb, struct us_data *us)
{
	/* Pad the ATAPI command with zeros 
	 *
	 * NOTE: This only works because a Scsi_Cmnd struct field contains
	 * a unsigned char cmnd[16], so we know we have storage available
	 */
	for (; srb->cmd_len<12; srb->cmd_len++)
		srb->cmnd[srb->cmd_len] = 0;

	/* set command length to 12 bytes */
	srb->cmd_len = 12;

	/* send the command to the transport layer */
	usb_stor_invoke_transport(srb, us);
	if (srb->result == SAM_STAT_GOOD) {
		/* fix the INQUIRY data if necessary */
		fix_inquiry_data(srb);
	}
}

void usb_stor_ATAPI_command(Scsi_Cmnd *srb, struct us_data *us)
{
	/* Pad the ATAPI command with zeros 
	 *
	 * NOTE: This only works because a Scsi_Cmnd struct field contains
	 * a unsigned char cmnd[16], so we know we have storage available
	 */

	/* Pad the ATAPI command with zeros */
	for (; srb->cmd_len<12; srb->cmd_len++)
		srb->cmnd[srb->cmd_len] = 0;

	/* set command length to 12 bytes */
	srb->cmd_len = 12;

	/* send the command to the transport layer */
	usb_stor_invoke_transport(srb, us);

	if (srb->result == SAM_STAT_GOOD) {
		/* fix the INQUIRY data if necessary */
		fix_inquiry_data(srb);
	}
}


void usb_stor_ufi_command(Scsi_Cmnd *srb, struct us_data *us)
{
	/* fix some commands -- this is a form of mode translation
	 * UFI devices only accept 12 byte long commands 
	 *
	 * NOTE: This only works because a Scsi_Cmnd struct field contains
	 * a unsigned char cmnd[16], so we know we have storage available
	 */

	/* Pad the ATAPI command with zeros */
	for (; srb->cmd_len<12; srb->cmd_len++)
		srb->cmnd[srb->cmd_len] = 0;

	/* set command length to 12 bytes (this affects the transport layer) */
	srb->cmd_len = 12;

	/* XXX We should be constantly re-evaluating the need for these */

	/* determine the correct data length for these commands */
	switch (srb->cmnd[0]) {

		/* for INQUIRY, UFI devices only ever return 36 bytes */
	case INQUIRY:
		srb->cmnd[4] = 36;
		break;

		/* again, for MODE_SENSE_10, we get the minimum (8) */
	case MODE_SENSE_10:
		srb->cmnd[7] = 0;
		srb->cmnd[8] = 8;
		break;

		/* for REQUEST_SENSE, UFI devices only ever return 18 bytes */
	case REQUEST_SENSE:
		srb->cmnd[4] = 18;
		break;
	} /* end switch on cmnd[0] */

	/* send the command to the transport layer */
	usb_stor_invoke_transport(srb, us);

	if (srb->result == SAM_STAT_GOOD) {
		/* Fix the data for an INQUIRY, if necessary */
		fix_inquiry_data(srb);
	}
}

void usb_stor_transparent_scsi_command(Scsi_Cmnd *srb, struct us_data *us)
{
	/* send the command to the transport layer */
	usb_stor_invoke_transport(srb, us);

	if (srb->result == SAM_STAT_GOOD) {
		/* Fix the INQUIRY data if necessary */
		fix_inquiry_data(srb);

		/* Fix the READ CAPACITY result if necessary */
		if (us->flags & US_FL_FIX_CAPACITY)
			fix_read_capacity(srb);
	}
}

/***********************************************************************
 * Scatter-gather transfer buffer access routines
 ***********************************************************************/

/* Copy a buffer of length buflen to/from the srb's transfer buffer.
 * (Note: for scatter-gather transfers (srb->use_sg > 0), srb->request_buffer
 * points to a list of s-g entries and we ignore srb->request_bufflen.
 * For non-scatter-gather transfers, srb->request_buffer points to the
 * transfer buffer itself and srb->request_bufflen is the buffer's length.)
 * Update the *index and *offset variables so that the next copy will
 * pick up from where this one left off. */

unsigned int usb_stor_access_xfer_buf(unsigned char *buffer,
	unsigned int buflen, Scsi_Cmnd *srb, unsigned int *index,
	unsigned int *offset, enum xfer_buf_dir dir)
{
	unsigned int cnt;

	/* If not using scatter-gather, just transfer the data directly.
	 * Make certain it will fit in the available buffer space. */
	if (srb->use_sg == 0) {
		if (*offset >= srb->request_bufflen)
			return 0;
		cnt = min(buflen, srb->request_bufflen - *offset);
		if (dir == TO_XFER_BUF)
			memcpy((unsigned char *) srb->request_buffer + *offset,
					buffer, cnt);
		else
			memcpy(buffer, (unsigned char *) srb->request_buffer +
					*offset, cnt);
		*offset += cnt;

	/* Using scatter-gather.  We have to go through the list one entry
	 * at a time.  Each s-g entry contains some number of pages, and
	 * each page has to be kmap()'ed separately.  If the page is already
	 * in kernel-addressable memory then kmap() will return its address.
	 * If the page is not directly accessible -- such as a user buffer
	 * located in high memory -- then kmap() will map it to a temporary
	 * position in the kernel's virtual address space. */
	} else {
		struct scatterlist *sg =
				(struct scatterlist *) srb->request_buffer
				+ *index;

		/* This loop handles a single s-g list entry, which may
		 * include multiple pages.  Find the initial page structure
		 * and the starting offset within the page, and update
		 * the *offset and *index values for the next loop. */
		cnt = 0;
		while (cnt < buflen && *index < srb->use_sg) {
			struct page *page = sg->page +
					((sg->offset + *offset) >> PAGE_SHIFT);
			unsigned int poff =
					(sg->offset + *offset) & (PAGE_SIZE-1);
			unsigned int sglen = sg->length - *offset;

			if (sglen > buflen - cnt) {

				/* Transfer ends within this s-g entry */
				sglen = buflen - cnt;
				*offset += sglen;
			} else {

				/* Transfer continues to next s-g entry */
				*offset = 0;
				++*index;
				++sg;
			}

			/* Transfer the data for all the pages in this
			 * s-g entry.  For each page: call kmap(), do the
			 * transfer, and call kunmap() immediately after. */
			while (sglen > 0) {
				unsigned int plen = min(sglen, (unsigned int)
						PAGE_SIZE - poff);
				unsigned char *ptr = kmap(page);

				if (dir == TO_XFER_BUF)
					memcpy(ptr + poff, buffer + cnt, plen);
				else
					memcpy(buffer + cnt, ptr + poff, plen);
				kunmap(page);

				/* Start at the beginning of the next page */
				poff = 0;
				++page;
				cnt += plen;
				sglen -= plen;
			}
		}
	}

	/* Return the amount actually transferred */
	return cnt;
}

/* Store the contents of buffer into srb's transfer buffer and set the
 * SCSI residue. */
void usb_stor_set_xfer_buf(unsigned char *buffer,
	unsigned int buflen, Scsi_Cmnd *srb)
{
	unsigned int index = 0, offset = 0;

	usb_stor_access_xfer_buf(buffer, buflen, srb, &index, &offset,
			TO_XFER_BUF);
	if (buflen < srb->request_bufflen)
		srb->resid = srb->request_bufflen - buflen;
}
