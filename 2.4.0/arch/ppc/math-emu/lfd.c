/* $Id: lfd.c,v 1.1 1999/08/23 19:00:08 cort Exp $
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <asm/uaccess.h>

#include "sfp-machine.h"
#include "double.h"

int
lfd(void *frD, void *ea)
{
	if (copy_from_user(frD, ea, sizeof(double)))
		return -EFAULT;
#ifdef DEBUG
	printk("%s: D %p, ea %p: ", __FUNCTION__, frD, ea);
	dump_double(frD);
	printk("\n");
#endif
	return 0;
}
