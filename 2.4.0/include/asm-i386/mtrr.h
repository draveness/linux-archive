/*  Generic MTRR (Memory Type Range Register) ioctls.

    Copyright (C) 1997-1999  Richard Gooch

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    Richard Gooch may be reached by email at  rgooch@atnf.csiro.au
    The postal address is:
      Richard Gooch, c/o ATNF, P. O. Box 76, Epping, N.S.W., 2121, Australia.
*/
#ifndef _LINUX_MTRR_H
#define _LINUX_MTRR_H

#include <linux/config.h>
#include <linux/ioctl.h>

#define	MTRR_IOCTL_BASE	'M'

struct mtrr_sentry
{
    unsigned long base;    /*  Base address     */
    unsigned long size;    /*  Size of region   */
    unsigned int type;     /*  Type of region   */
};

struct mtrr_gentry
{
    unsigned int regnum;   /*  Register number  */
    unsigned long base;    /*  Base address     */
    unsigned long size;    /*  Size of region   */
    unsigned int type;     /*  Type of region   */
};

/*  These are the various ioctls  */
#define MTRRIOC_ADD_ENTRY        _IOW(MTRR_IOCTL_BASE,  0, struct mtrr_sentry)
#define MTRRIOC_SET_ENTRY        _IOW(MTRR_IOCTL_BASE,  1, struct mtrr_sentry)
#define MTRRIOC_DEL_ENTRY        _IOW(MTRR_IOCTL_BASE,  2, struct mtrr_sentry)
#define MTRRIOC_GET_ENTRY        _IOWR(MTRR_IOCTL_BASE, 3, struct mtrr_gentry)
#define MTRRIOC_KILL_ENTRY       _IOW(MTRR_IOCTL_BASE,  4, struct mtrr_sentry)
#define MTRRIOC_ADD_PAGE_ENTRY   _IOW(MTRR_IOCTL_BASE,  5, struct mtrr_sentry)
#define MTRRIOC_SET_PAGE_ENTRY   _IOW(MTRR_IOCTL_BASE,  6, struct mtrr_sentry)
#define MTRRIOC_DEL_PAGE_ENTRY   _IOW(MTRR_IOCTL_BASE,  7, struct mtrr_sentry)
#define MTRRIOC_GET_PAGE_ENTRY   _IOWR(MTRR_IOCTL_BASE, 8, struct mtrr_gentry)
#define MTRRIOC_KILL_PAGE_ENTRY  _IOW(MTRR_IOCTL_BASE,  9, struct mtrr_sentry)

/*  These are the region types  */
#define MTRR_TYPE_UNCACHABLE 0
#define MTRR_TYPE_WRCOMB     1
/*#define MTRR_TYPE_         2*/
/*#define MTRR_TYPE_         3*/
#define MTRR_TYPE_WRTHROUGH  4
#define MTRR_TYPE_WRPROT     5
#define MTRR_TYPE_WRBACK     6
#define MTRR_NUM_TYPES       7

#ifdef MTRR_NEED_STRINGS
static char *mtrr_strings[MTRR_NUM_TYPES] =
{
    "uncachable",               /* 0 */
    "write-combining",          /* 1 */
    "?",                        /* 2 */
    "?",                        /* 3 */
    "write-through",            /* 4 */
    "write-protect",            /* 5 */
    "write-back",               /* 6 */
};
#endif

#ifdef __KERNEL__

/*  The following functions are for use by other drivers  */
# ifdef CONFIG_MTRR
extern int mtrr_add (unsigned long base, unsigned long size,
		     unsigned int type, char increment);
extern int mtrr_add_page (unsigned long base, unsigned long size,
		     unsigned int type, char increment);
extern int mtrr_del (int reg, unsigned long base, unsigned long size);
extern int mtrr_del_page (int reg, unsigned long base, unsigned long size);
#  else
static __inline__ int mtrr_add (unsigned long base, unsigned long size,
				unsigned int type, char increment)
{
    return -ENODEV;
}
static __inline__ int mtrr_add_page (unsigned long base, unsigned long size,
				unsigned int type, char increment)
{
    return -ENODEV;
}
static __inline__ int mtrr_del (int reg, unsigned long base,
				unsigned long size)
{
    return -ENODEV;
}
static __inline__ int mtrr_del_page (int reg, unsigned long base,
				unsigned long size)
{
    return -ENODEV;
}
#  endif

/*  The following functions are for initialisation: don't use them!  */
extern int mtrr_init (void);
#  if defined(CONFIG_SMP) && defined(CONFIG_MTRR)
extern void mtrr_init_boot_cpu (void);
extern void mtrr_init_secondary_cpu (void);
#  endif

#endif

#endif  /*  _LINUX_MTRR_H  */
