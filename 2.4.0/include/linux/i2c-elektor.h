/* ------------------------------------------------------------------------- */
/* i2c-elektor.c i2c-hw access for PCF8584 style isa bus adaptes             */
/* ------------------------------------------------------------------------- */
/*   Copyright (C) 1995-97 Simon G. Vogl
                   1998-99 Hans Berglund

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.                */
/* ------------------------------------------------------------------------- */

/* With some changes from Ky�sti M�lkki <kmalkki@cc.hut.fi> and even
   Frodo Looijaard <frodol@dds.nl> */

/* $Id: i2c-elektor.h,v 1.4 2000/01/18 23:54:07 frodo Exp $ */

#ifndef I2C_PCF_ELEKTOR_H
#define I2C_PCF_ELEKTOR_H 1

/*
 * This struct contains the hw-dependent functions of PCF8584 adapters to 
 * manipulate the registers, and to init any hw-specific features. 
 */

struct i2c_pcf_isa {
	int pi_base;
	int pi_irq;
	int pi_clock;
	int pi_own;
};


#endif /* PCF_ELEKTOR_H */
