/*
    I2C functions
    Copyright (C) 2003-2004  Kevin Thayer <nufan_wfk at yahoo.com>
    Copyright (C) 2005-2007  Hans Verkuil <hverkuil@xs4all.nl>

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
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

int ivtv_cx25840(struct ivtv *itv, unsigned int cmd, void *arg);
int ivtv_saa7115(struct ivtv *itv, unsigned int cmd, void *arg);
int ivtv_saa7127(struct ivtv *itv, unsigned int cmd, void *arg);
int ivtv_saa717x(struct ivtv *itv, unsigned int cmd, void *arg);
int ivtv_upd64031a(struct ivtv *itv, unsigned int cmd, void *arg);
int ivtv_upd64083(struct ivtv *itv, unsigned int cmd, void *arg);

int ivtv_i2c_hw_addr(struct ivtv *itv, u32 hw);
int ivtv_i2c_hw(struct ivtv *itv, u32 hw, unsigned int cmd, void *arg);
int ivtv_i2c_id(struct ivtv *itv, u32 id, unsigned int cmd, void *arg);
int ivtv_call_i2c_client(struct ivtv *itv, int addr, unsigned int cmd, void *arg);
void ivtv_call_i2c_clients(struct ivtv *itv, unsigned int cmd, void *arg);

/* init + register i2c algo-bit adapter */
int __devinit init_ivtv_i2c(struct ivtv *itv);
void __devexit exit_ivtv_i2c(struct ivtv *itv);
