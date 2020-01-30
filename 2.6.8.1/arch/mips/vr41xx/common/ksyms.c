/*
 *   ksyms.c, Export NEC VR4100 series specific functions needed for loadable modules.
 *
 *  Copyright (C) 2003  Yoichi Yuasa <yuasa@hh.iij4u.or.jp>
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
#include <linux/module.h>

#include <asm/vr41xx/vr41xx.h>

EXPORT_SYMBOL(vr41xx_get_vtclock_frequency);
EXPORT_SYMBOL(vr41xx_get_tclock_frequency);

EXPORT_SYMBOL(vr41xx_set_rtclong1_cycle);
EXPORT_SYMBOL(vr41xx_read_rtclong1_counter);
EXPORT_SYMBOL(vr41xx_set_rtclong2_cycle);
EXPORT_SYMBOL(vr41xx_read_rtclong2_counter);
EXPORT_SYMBOL(vr41xx_set_tclock_cycle);
EXPORT_SYMBOL(vr41xx_read_tclock_counter);
