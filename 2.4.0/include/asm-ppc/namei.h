/* $Id: namei.h,v 1.4 1998/10/28 08:13:33 jj Exp $
 * linux/include/asm-ppc/namei.h
 * Adapted from linux/include/asm-alpha/namei.h
 *
 * Included from linux/fs/namei.c
 */

#ifdef __KERNEL__
#ifndef __PPC_NAMEI_H
#define __PPC_NAMEI_H

/* This dummy routine maybe changed to something useful
 * for /usr/gnemul/ emulation stuff.
 * Look at asm-sparc/namei.h for details.
 */

#define __emul_prefix() NULL

#endif /* __PPC_NAMEI_H */
#endif /* __KERNEL__ */
