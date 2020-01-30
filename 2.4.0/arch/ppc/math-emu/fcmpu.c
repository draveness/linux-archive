/* $Id: fcmpu.c,v 1.1 1999/08/23 18:59:28 cort Exp $
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <asm/uaccess.h>

#include "soft-fp.h"
#include "double.h"

int
fcmpu(u32 *ccr, int crfD, void *frA, void *frB)
{
	FP_DECL_D(A);
	FP_DECL_D(B);
	int code[4] = { (1 << 3), (1 << 1), (1 << 2), (1 << 0) };
	long cmp;

#ifdef DEBUG
	printk("%s: %p (%08x) %d %p %p\n", __FUNCTION__, ccr, *ccr, crfD, frA, frB);
#endif

	__FP_UNPACK_D(A, frA);
	__FP_UNPACK_D(B, frB);

#ifdef DEBUG
	printk("A: %ld %lu %lu %ld (%ld)\n", A_s, A_f1, A_f0, A_e, A_c);
	printk("B: %ld %lu %lu %ld (%ld)\n", B_s, B_f1, B_f0, B_e, B_c);
#endif

	FP_CMP_D(cmp, A, B, 2);
	cmp = code[(cmp + 1) & 3];

	__FPU_FPSCR &= ~(0x1f000);
	__FPU_FPSCR |= (cmp << 12);

	*ccr &= ~(15 << ((7 - crfD) << 2));
	*ccr |= (cmp << ((7 - crfD) << 2));

#ifdef DEBUG
	printk("CR: %08x\n", *ccr);
#endif

	return 0;
}
