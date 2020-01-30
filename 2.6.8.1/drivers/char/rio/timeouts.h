
/****************************************************************************
 *******                                                              *******
 *******                     T I M E O U T S
 *******                                                              *******
 ****************************************************************************

 Author  : Ian Nandhra
 Date    :

 *
 *  (C) 1990 - 2000 Specialix International Ltd., Byfleet, Surrey, UK.
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

 Version : 0.01


                            Mods
 ----------------------------------------------------------------------------
  Date     By                Description
 ----------------------------------------------------------------------------

 ***************************************************************************/

#ifndef lint
#ifdef SCCS_LABELS
static char *_rio_defaults_h_sccs = "@(#)timeouts.h	1.3" ;
#endif
#endif

#define MILLISECOND           (int) (1000/64)   /* 15.625 low ticks */
#define SECOND                (int) 15625       /* Low priority ticks */

#define TX_TIMEOUT          (int) (200 * MILLISECOND)


/*********** end of file ***********/

