/*
 *  setup.c, Setup for the CASIO CASSIOPEIA E-11/15/55/65.
 *
 *  Copyright (C) 2002-2004  Yoichi Yuasa <yuasa@hh.iij4u.or.jp>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <linux/config.h>
#include <linux/ioport.h>

#include <asm/io.h>
#include <asm/vr41xx/e55.h>

const char *get_system_type(void)
{
	return "CASIO CASSIOPEIA E-11/15/55/65";
}

static int casio_e55_setup(void)
{
	set_io_port_base(IO_PORT_BASE);
	ioport_resource.start = IO_PORT_RESOURCE_START;
	ioport_resource.end = IO_PORT_RESOURCE_END;

#ifdef CONFIG_SERIAL_8250
	vr41xx_select_siu_interface(SIU_RS232C, IRDA_NONE);
	vr41xx_siu_init();
#endif

	return 0;
}

early_initcall(casio_e55_setup);
