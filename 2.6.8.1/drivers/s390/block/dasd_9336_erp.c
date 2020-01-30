/* 
 * File...........: linux/drivers/s390/block/dasd_9336_erp.c
 * Author(s)......: Holger Smolinski <Holger.Smolinski@de.ibm.com>
 * Bugreports.to..: <Linux390@de.ibm.com>
 * (C) IBM Corporation, IBM Deutschland Entwicklung GmbH, 2000
 *
 * $Revision: 1.8 $
 */

#define PRINTK_HEADER "dasd_erp(9336)"

#include "dasd_int.h"


/*
 * DASD_9336_ERP_EXAMINE 
 *
 * DESCRIPTION
 *   Checks only for fatal/no/recover error. 
 *   A detailed examination of the sense data is done later outside
 *   the interrupt handler.
 *
 *   The logic is based on the 'IBM 3880 Storage Control Reference' manual
 *   'Chapter 7. 9336 Sense Data'.
 *
 * RETURN VALUES
 *   dasd_era_none	no error 
 *   dasd_era_fatal	for all fatal (unrecoverable errors)
 *   dasd_era_recover	for all others.
 */
dasd_era_t
dasd_9336_erp_examine(struct dasd_ccw_req * cqr, struct irb * irb)
{
	/* check for successful execution first */
	if (irb->scsw.cstat == 0x00 &&
	    irb->scsw.dstat == (DEV_STAT_CHN_END | DEV_STAT_DEV_END))
		return dasd_era_none;

	/* examine the 24 byte sense data */
	return dasd_era_recover;

}				/* END dasd_9336_erp_examine */

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-indent-level: 4 
 * c-brace-imaginary-offset: 0
 * c-brace-offset: -4
 * c-argdecl-indent: 4
 * c-label-offset: -4
 * c-continued-statement-offset: 4
 * c-continued-brace-offset: 0
 * indent-tabs-mode: 1
 * tab-width: 8
 * End:
 */
