/* $Id: fctiwz.c,v 1.1 1999/08/23 18:59:31 cort Exp $
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <asm/uaccess.h>

#include "soft-fp.h"
#include "double.h"

int
fctiwz(u32 *frD, void *frB)
{
	FP_DECL_D(B);
	u32 fpscr;
	unsigned int r;

	fpscr = __FPU_FPSCR;
	__FPU_FPSCR &= ~(3);
	__FPU_FPSCR |= FP_RND_ZERO;

	__FP_UNPACK_D(B, frB);
	FP_TO_INT_D(r, B, 32, 1);
	frD[1] = r;

	__FPU_FPSCR = fpscr;

#ifdef DEBUG
	printk("%s: D %p, B %p: ", __FUNCTION__, frD, frB);
	dump_double(frD);
	printk("\n");
#endif

	return 0;
}
