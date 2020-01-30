/* Driver for Microtech DPCM-USB CompactFlash/SmartMedia reader
 *
 * $Id: dpcm.c,v 1.3 2000/08/25 00:13:51 mdharm Exp $
 *
 * DPCM driver v0.1:
 *
 * First release
 *
 * Current development and maintenance by:
 *   (c) 2000 Brian Webb (webbb@earthlink.net)
 *
 * This device contains both a CompactFlash card reader, which
 * usest the Control/Bulk w/o Interrupt protocol and
 * a SmartMedia card reader that uses the same protocol
 * as the SDDR09.
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

#include <linux/config.h>
#include "transport.h"
#include "protocol.h"
#include "usb.h"
#include "debug.h"
#include "dpcm.h"
#include "sddr09.h"

/*
 * Transport for the Microtech DPCM-USB
 *
 */
int dpcm_transport(Scsi_Cmnd *srb, struct us_data *us)
{
  int ret;

  if(srb == NULL)
    return USB_STOR_TRANSPORT_ERROR;

  US_DEBUGP("dpcm_transport: LUN=%d\n", srb->lun);

  switch(srb->lun) {
  case 0:

    /*
     * LUN 0 corresponds to the CompactFlash card reader.
     */
    return usb_stor_CB_transport(srb, us);

#ifdef CONFIG_USB_STORAGE_SDDR09
  case 1:

    /*
     * LUN 1 corresponds to the SmartMedia card reader.
     */

    /*
     * Set the LUN to 0 (just in case).
     */
    srb->lun = 0; us->srb->lun = 0;
    ret = sddr09_transport(srb, us);
    srb->lun = 1; us->srb->lun = 1;

    return ret;
#endif

  default:
    US_DEBUGP("dpcm_transport: Invalid LUN %d\n", srb->lun);
    return USB_STOR_TRANSPORT_ERROR;
  }
}
