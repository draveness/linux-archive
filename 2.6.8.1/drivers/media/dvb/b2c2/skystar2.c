/*
 * skystar2.c - driver for the Technisat SkyStar2 PCI DVB card
 *              based on the FlexCopII by B2C2,Inc.
 *
 * Copyright (C) 2003  Vadim Catana, skystar@moldova.cc
 *
 * FIX: DISEQC Tone Burst in flexcop_diseqc_ioctl()
 * FIX: FULL soft DiSEqC for skystar2 (FlexCopII rev 130) VP310 equipped 
 *     Vincenzo Di Massa, hawk.it at tiscalinet.it
 * 	
 * Converted to Linux coding style
 * Misc reorganization, polishing, restyling
 *     Roberto Ragusa, r.ragusa at libero.it
 *       
 * Added hardware filtering support, 
 *     Niklas Peinecke, peinecke at gdv.uni-hannover.de
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/init.h>

#include <asm/io.h>

#include "dvb_i2c.h"
#include "dvb_frontend.h"

#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>
#include "dvb_demux.h"
#include "dmxdev.h"
#include "dvb_filter.h"
#include "dvbdev.h"
#include "demux.h"
#include "dvb_net.h"

#include "dvb_functions.h"

static int debug = 0;
#define dprintk(x...)	do { if (debug>=1) printk(x); } while (0)
#define ddprintk(x...)	do { if (debug>=2) printk(x); } while (0)
static int enable_hw_filters = 2;

#define SIZE_OF_BUF_DMA1	0x3ac00
#define SIZE_OF_BUF_DMA2	0x758

#define MAX_N_HW_FILTERS	(6+32)
#define N_PID_SLOTS		256

struct dmaq {
	u32 bus_addr;
	u32 head;
	u32 tail;
	u32 buffer_size;
	u8 *buffer;
};


struct adapter {
	struct pci_dev *pdev;

	u8 card_revision;
	u32 b2c2_revision;
	u32 pid_filter_max;
	u32 mac_filter_max;
	u32 irq;
	unsigned long io_mem;
	unsigned long io_port;
	u8 mac_addr[8];
	u32 dw_sram_type;

	struct dvb_adapter *dvb_adapter;
	struct dvb_demux demux;
	struct dmxdev dmxdev;
	struct dmx_frontend hw_frontend;
	struct dmx_frontend mem_frontend;
	struct dvb_i2c_bus *i2c_bus;
	struct dvb_net dvbnet;

	struct semaphore i2c_sem;

	struct dmaq dmaq1;
	struct dmaq dmaq2;

	u32 dma_ctrl;
	u32 dma_status;

	int capturing;

	spinlock_t lock;

	int useable_hw_filters;
	u16 hw_pids[MAX_N_HW_FILTERS];
	u16 pid_list[N_PID_SLOTS];
	int pid_rc[N_PID_SLOTS];	// ref counters for the pids
	int pid_count;
	int whole_bandwidth_count;
	u32 mac_filter;
};

#define write_reg_dw(adapter,reg,value) writel(value, adapter->io_mem + reg)
#define read_reg_dw(adapter,reg) readl(adapter->io_mem + reg)

static void write_reg_bitfield(struct adapter *adapter, u32 reg, u32 zeromask, u32 orvalue)
{
	u32 tmp;

	tmp = read_reg_dw(adapter, reg);
	tmp = (tmp & ~zeromask) | orvalue;
	write_reg_dw(adapter, reg, tmp);
}

/* i2c functions */
static int i2c_main_write_for_flex2(struct adapter *adapter, u32 command, u8 *buf, int retries)
{
	int i;
	u32 value;

	write_reg_dw(adapter, 0x100, 0);
	write_reg_dw(adapter, 0x100, command);

	for (i = 0; i < retries; i++) {
		value = read_reg_dw(adapter, 0x100);

		if ((value & 0x40000000) == 0) {
			if ((value & 0x81000000) == 0x80000000) {
				if (buf != 0)
					*buf = (value >> 0x10) & 0xff;

				return 1;
			}
		} else {
			write_reg_dw(adapter, 0x100, 0);
			write_reg_dw(adapter, 0x100, command);
		}
	}

	return 0;
}

/* device = 0x10000000 for tuner, 0x20000000 for eeprom */
static void i2c_main_setup(u32 device, u32 chip_addr, u8 op, u8 addr, u32 value, u32 len, u32 *command)
{
	*command = device | ((len - 1) << 26) | (value << 16) | (addr << 8) | chip_addr;

	if (op != 0)
		*command = *command | 0x03000000;
	else
		*command = *command | 0x01000000;
}

static int flex_i2c_read4(struct adapter *adapter, u32 device, u32 chip_addr, u16 addr, u8 *buf, u8 len)
{
	u32 command;
	u32 value;

	int result, i;

	i2c_main_setup(device, chip_addr, 1, addr, 0, len, &command);

	result = i2c_main_write_for_flex2(adapter, command, buf, 100000);

	if ((result & 0xff) != 0) {
		if (len > 1) {
			value = read_reg_dw(adapter, 0x104);

			for (i = 1; i < len; i++) {
				buf[i] = value & 0xff;
				value = value >> 8;
			}
		}
	}

	return result;
}

static int flex_i2c_write4(struct adapter *adapter, u32 device, u32 chip_addr, u32 addr, u8 *buf, u8 len)
{
	u32 command;
	u32 value;
	int i;

	if (len > 1) {
		value = 0;

		for (i = len; i > 1; i--) {
			value = value << 8;
			value = value | buf[i - 1];
		}

		write_reg_dw(adapter, 0x104, value);
	}

	i2c_main_setup(device, chip_addr, 0, addr, buf[0], len, &command);

	return i2c_main_write_for_flex2(adapter, command, NULL, 100000);
}

static void fixchipaddr(u32 device, u32 bus, u32 addr, u32 *ret)
{
	if (device == 0x20000000)
		*ret = bus | ((addr >> 8) & 3);

	*ret = bus;
}

static u32 flex_i2c_read(struct adapter *adapter, u32 device, u32 bus, u32 addr, u8 *buf, u32 len)
{
	u32 chipaddr;
	u32 bytes_to_transfer;
	u8 *start;

	ddprintk("%s:\n", __FUNCTION__);

	start = buf;

	while (len != 0) {
		bytes_to_transfer = len;

		if (bytes_to_transfer > 4)
			bytes_to_transfer = 4;

		fixchipaddr(device, bus, addr, &chipaddr);

		if (flex_i2c_read4(adapter, device, chipaddr, addr, buf, bytes_to_transfer) == 0)
			return buf - start;

		buf = buf + bytes_to_transfer;
		addr = addr + bytes_to_transfer;
		len = len - bytes_to_transfer;
	};

	return buf - start;
}

static u32 flex_i2c_write(struct adapter *adapter, u32 device, u32 bus, u32 addr, u8 *buf, u32 len)
{
	u32 chipaddr;
	u32 bytes_to_transfer;
	u8 *start;

	ddprintk("%s:\n", __FUNCTION__);

	start = buf;

	while (len != 0) {
		bytes_to_transfer = len;

		if (bytes_to_transfer > 4)
			bytes_to_transfer = 4;

		fixchipaddr(device, bus, addr, &chipaddr);

		if (flex_i2c_write4(adapter, device, chipaddr, addr, buf, bytes_to_transfer) == 0)
			return buf - start;

		buf = buf + bytes_to_transfer;
		addr = addr + bytes_to_transfer;
		len = len - bytes_to_transfer;
	}

	return buf - start;
}

static int master_xfer(struct dvb_i2c_bus *i2c, const struct i2c_msg *msgs, int num)
{
	struct adapter *tmp = i2c->data;
	int i, ret = 0;

	if (down_interruptible(&tmp->i2c_sem))
		return -ERESTARTSYS;

	ddprintk("%s: %d messages to transfer\n", __FUNCTION__, num);

		for (i = 0; i < num; i++) {
		ddprintk("message %d: flags=0x%x, addr=0x%x, buf=0x%x, len=%d \n", i,
			 msgs[i].flags, msgs[i].addr, msgs[i].buf[0], msgs[i].len);
	
		/* allow only the mt312 and stv0299 frontends to access the bus */
		if ((msgs[i].addr != 0x0e) && (msgs[i].addr != 0x68) && (msgs[i].addr != 0x61)) {
		up(&tmp->i2c_sem);

		return -EREMOTEIO;
	}
	}

	// read command
	if ((num == 2) && (msgs[0].flags == 0) && (msgs[1].flags == I2C_M_RD) && (msgs[0].buf != NULL) && (msgs[1].buf != NULL)) {

		ret = flex_i2c_read(tmp, 0x10000000, msgs[0].addr, msgs[0].buf[0], msgs[1].buf, msgs[1].len);

		up(&tmp->i2c_sem);

		if (ret != msgs[1].len) {
			printk("%s: read error !\n", __FUNCTION__);

			for (i = 0; i < 2; i++) {
				printk("message %d: flags=0x%x, addr=0x%x, buf=0x%x, len=%d \n", i,
				       msgs[i].flags, msgs[i].addr, msgs[i].buf[0], msgs[i].len);
		}

			return -EREMOTEIO;
		}

		return num;
	}
	// write command
	for (i = 0; i < num; i++) {

		if ((msgs[i].flags != 0) || (msgs[i].buf == NULL) || (msgs[i].len < 2))
			return -EINVAL;

		ret = flex_i2c_write(tmp, 0x10000000, msgs[i].addr, msgs[i].buf[0], &msgs[i].buf[1], msgs[i].len - 1);

		up(&tmp->i2c_sem);

		if (ret != msgs[0].len - 1) {
			printk("%s: write error %i !\n", __FUNCTION__, ret);

			printk("message %d: flags=0x%x, addr=0x%x, buf[0]=0x%x, len=%d \n", i,
			       msgs[i].flags, msgs[i].addr, msgs[i].buf[0], msgs[i].len);

			return -EREMOTEIO;
		}

		return num;
	}

	printk("%s: unknown command format !\n", __FUNCTION__);

	return -EINVAL;
}

/* SRAM (Skystar2 rev2.3 has one "ISSI IS61LV256" chip on board,
   but it seems that FlexCopII can work with more than one chip) */
static void sram_set_net_dest(struct adapter *adapter, u8 dest)
{
	u32 tmp;

	udelay(1000);

	tmp = (read_reg_dw(adapter, 0x714) & 0xfffffffc) | (dest & 3);

	udelay(1000);

	write_reg_dw(adapter, 0x714, tmp);
	write_reg_dw(adapter, 0x714, tmp);

	udelay(1000);

	/* return value is never used? */
/*	return tmp; */
}

static void sram_set_cai_dest(struct adapter *adapter, u8 dest)
{
	u32 tmp;

	udelay(1000);

	tmp = (read_reg_dw(adapter, 0x714) & 0xfffffff3) | ((dest & 3) << 2);

	udelay(1000);
	udelay(1000);

	write_reg_dw(adapter, 0x714, tmp);
	write_reg_dw(adapter, 0x714, tmp);

	udelay(1000);

	/* return value is never used? */
/*	return tmp; */
}

static void sram_set_cao_dest(struct adapter *adapter, u8 dest)
{
	u32 tmp;

	udelay(1000);

	tmp = (read_reg_dw(adapter, 0x714) & 0xffffffcf) | ((dest & 3) << 4);

	udelay(1000);
	udelay(1000);

	write_reg_dw(adapter, 0x714, tmp);
	write_reg_dw(adapter, 0x714, tmp);

	udelay(1000);

	/* return value is never used? */
/*	return tmp; */
}

static void sram_set_media_dest(struct adapter *adapter, u8 dest)
{
	u32 tmp;

	udelay(1000);

	tmp = (read_reg_dw(adapter, 0x714) & 0xffffff3f) | ((dest & 3) << 6);

	udelay(1000);
	udelay(1000);

	write_reg_dw(adapter, 0x714, tmp);
	write_reg_dw(adapter, 0x714, tmp);

	udelay(1000);

	/* return value is never used? */
/*	return tmp; */
}

/* SRAM memory is accessed through a buffer register in the FlexCop
   chip (0x700). This register has the following structure:
    bits 0-14  : address
    bit  15    : read/write flag
    bits 16-23 : 8-bit word to write
    bits 24-27 : = 4
    bits 28-29 : memory bank selector
    bit  31    : busy flag
*/
static void flex_sram_write(struct adapter *adapter, u32 bank, u32 addr, u8 *buf, u32 len)
{
	int i, retries;
	u32 command;

	for (i = 0; i < len; i++) {
		command = bank | addr | 0x04000000 | (*buf << 0x10);

		retries = 2;

		while (((read_reg_dw(adapter, 0x700) & 0x80000000) != 0) && (retries > 0)) {
			mdelay(1);
			retries--;
		};

		if (retries == 0)
			printk("%s: SRAM timeout\n", __FUNCTION__);

		write_reg_dw(adapter, 0x700, command);

		buf++;
		addr++;
	}
}

static void flex_sram_read(struct adapter *adapter, u32 bank, u32 addr, u8 *buf, u32 len)
{
	int i, retries;
	u32 command, value;

	for (i = 0; i < len; i++) {
		command = bank | addr | 0x04008000;

		retries = 10000;

		while (((read_reg_dw(adapter, 0x700) & 0x80000000) != 0) && (retries > 0)) {
			mdelay(1);
			retries--;
		};

		if (retries == 0)
			printk("%s: SRAM timeout\n", __FUNCTION__);

		write_reg_dw(adapter, 0x700, command);

		retries = 10000;

		while (((read_reg_dw(adapter, 0x700) & 0x80000000) != 0) && (retries > 0)) {
			mdelay(1);
			retries--;
		};

		if (retries == 0)
			printk("%s: SRAM timeout\n", __FUNCTION__);

		value = read_reg_dw(adapter, 0x700) >> 0x10;

		*buf = (value & 0xff);

		addr++;
		buf++;
	}
}

static void sram_write_chunk(struct adapter *adapter, u32 addr, u8 *buf, u16 len)
{
	u32 bank;

	bank = 0;

	if (adapter->dw_sram_type == 0x20000) {
		bank = (addr & 0x18000) << 0x0d;
	}

	if (adapter->dw_sram_type == 0x00000) {
		if ((addr >> 0x0f) == 0)
			bank = 0x20000000;
		else
			bank = 0x10000000;
	}

	flex_sram_write(adapter, bank, addr & 0x7fff, buf, len);
}

static void sram_read_chunk(struct adapter *adapter, u32 addr, u8 *buf, u16 len)
{
	u32 bank;

	bank = 0;

	if (adapter->dw_sram_type == 0x20000) {
		bank = (addr & 0x18000) << 0x0d;
	}

	if (adapter->dw_sram_type == 0x00000) {
		if ((addr >> 0x0f) == 0)
			bank = 0x20000000;
		else
			bank = 0x10000000;
	}

	flex_sram_read(adapter, bank, addr & 0x7fff, buf, len);
}

static void sram_read(struct adapter *adapter, u32 addr, u8 *buf, u32 len)
{
	u32 length;

	while (len != 0) {
		length = len;

		// check if the address range belongs to the same 
		// 32K memory chip. If not, the data is read from 
		// one chip at a time.
		if ((addr >> 0x0f) != ((addr + len - 1) >> 0x0f)) {
			length = (((addr >> 0x0f) + 1) << 0x0f) - addr;
		}

		sram_read_chunk(adapter, addr, buf, length);

		addr = addr + length;
		buf = buf + length;
		len = len - length;
	}
}

static void sram_write(struct adapter *adapter, u32 addr, u8 *buf, u32 len)
{
	u32 length;

	while (len != 0) {
		length = len;

		// check if the address range belongs to the same 
		// 32K memory chip. If not, the data is written to
		// one chip at a time.
		if ((addr >> 0x0f) != ((addr + len - 1) >> 0x0f)) {
			length = (((addr >> 0x0f) + 1) << 0x0f) - addr;
		}

		sram_write_chunk(adapter, addr, buf, length);

		addr = addr + length;
		buf = buf + length;
		len = len - length;
	}
}

static void sram_set_size(struct adapter *adapter, u32 mask)
{
	write_reg_dw(adapter, 0x71c, (mask | (~0x30000 & read_reg_dw(adapter, 0x71c))));
}

static void sram_init(struct adapter *adapter)
{
	u32 tmp;

	tmp = read_reg_dw(adapter, 0x71c);

	write_reg_dw(adapter, 0x71c, 1);

	if (read_reg_dw(adapter, 0x71c) != 0) {
		write_reg_dw(adapter, 0x71c, tmp);

		adapter->dw_sram_type = tmp & 0x30000;

		ddprintk("%s: dw_sram_type = %x\n", __FUNCTION__, adapter->dw_sram_type);

	} else {

		adapter->dw_sram_type = 0x10000;

		ddprintk("%s: dw_sram_type = %x\n", __FUNCTION__, adapter->dw_sram_type);
	}

	/* return value is never used? */
/*	return adapter->dw_sram_type; */
}

static int sram_test_location(struct adapter *adapter, u32 mask, u32 addr)
{
	u8 tmp1, tmp2;

	dprintk("%s: mask = %x, addr = %x\n", __FUNCTION__, mask, addr);

	sram_set_size(adapter, mask);
	sram_init(adapter);

	tmp2 = 0xa5;
	tmp1 = 0x4f;

	sram_write(adapter, addr, &tmp2, 1);
	sram_write(adapter, addr + 4, &tmp1, 1);

	tmp2 = 0;

	mdelay(20);

	sram_read(adapter, addr, &tmp2, 1);
	sram_read(adapter, addr, &tmp2, 1);

	dprintk("%s: wrote 0xa5, read 0x%2x\n", __FUNCTION__, tmp2);

	if (tmp2 != 0xa5)
		return 0;

	tmp2 = 0x5a;
	tmp1 = 0xf4;

	sram_write(adapter, addr, &tmp2, 1);
	sram_write(adapter, addr + 4, &tmp1, 1);

	tmp2 = 0;

	mdelay(20);

	sram_read(adapter, addr, &tmp2, 1);
	sram_read(adapter, addr, &tmp2, 1);

	dprintk("%s: wrote 0x5a, read 0x%2x\n", __FUNCTION__, tmp2);

	if (tmp2 != 0x5a)
		return 0;

	return 1;
}

static u32 sram_length(struct adapter *adapter)
{
	if (adapter->dw_sram_type == 0x10000)
		return 32768;	//  32K
	if (adapter->dw_sram_type == 0x00000)
		return 65536;	//  64K        
	if (adapter->dw_sram_type == 0x20000)
		return 131072;	// 128K

	return 32768;		// 32K
}

/* FlexcopII can work with 32K, 64K or 128K of external SRAM memory.
    - for 128K there are 4x32K chips at bank 0,1,2,3.
    - for  64K there are 2x32K chips at bank 1,2.
    - for  32K there is one 32K chip at bank 0.

   FlexCop works only with one bank at a time. The bank is selected
   by bits 28-29 of the 0x700 register.
  
   bank 0 covers addresses 0x00000-0x07fff
   bank 1 covers addresses 0x08000-0x0ffff
   bank 2 covers addresses 0x10000-0x17fff
   bank 3 covers addresses 0x18000-0x1ffff
*/
static int sram_detect_for_flex2(struct adapter *adapter)
{
	u32 tmp, tmp2, tmp3;

	dprintk("%s:\n", __FUNCTION__);

	tmp = read_reg_dw(adapter, 0x208);
	write_reg_dw(adapter, 0x208, 0);

	tmp2 = read_reg_dw(adapter, 0x71c);

	dprintk("%s: tmp2 = %x\n", __FUNCTION__, tmp2);

	write_reg_dw(adapter, 0x71c, 1);

	tmp3 = read_reg_dw(adapter, 0x71c);

	dprintk("%s: tmp3 = %x\n", __FUNCTION__, tmp3);

	write_reg_dw(adapter, 0x71c, tmp2);

	// check for internal SRAM ???
	tmp3--;
	if (tmp3 != 0) {
		sram_set_size(adapter, 0x10000);
		sram_init(adapter);
		write_reg_dw(adapter, 0x208, tmp);

		dprintk("%s: sram size = 32K\n", __FUNCTION__);

		return 32;
	}

	if (sram_test_location(adapter, 0x20000, 0x18000) != 0) {
		sram_set_size(adapter, 0x20000);
		sram_init(adapter);
		write_reg_dw(adapter, 0x208, tmp);

		dprintk("%s: sram size = 128K\n", __FUNCTION__);

		return 128;
	}

	if (sram_test_location(adapter, 0x00000, 0x10000) != 0) {
		sram_set_size(adapter, 0x00000);
		sram_init(adapter);
		write_reg_dw(adapter, 0x208, tmp);

		dprintk("%s: sram size = 64K\n", __FUNCTION__);

		return 64;
	}

	if (sram_test_location(adapter, 0x10000, 0x00000) != 0) {
		sram_set_size(adapter, 0x10000);
		sram_init(adapter);
		write_reg_dw(adapter, 0x208, tmp);

		dprintk("%s: sram size = 32K\n", __FUNCTION__);

		return 32;
	}

	sram_set_size(adapter, 0x10000);
	sram_init(adapter);
	write_reg_dw(adapter, 0x208, tmp);

	dprintk("%s: SRAM detection failed. Set to 32K \n", __FUNCTION__);

	return 0;
}

static void sll_detect_sram_size(struct adapter *adapter)
{
	sram_detect_for_flex2(adapter);
}

/* EEPROM (Skystar2 has one "24LC08B" chip on board) */
/*
static int eeprom_write(struct adapter *adapter, u16 addr, u8 *buf, u16 len)
{
	return flex_i2c_write(adapter, 0x20000000, 0x50, addr, buf, len);
}
*/

static int eeprom_read(struct adapter *adapter, u16 addr, u8 *buf, u16 len)
{
	return flex_i2c_read(adapter, 0x20000000, 0x50, addr, buf, len);
}

u8 calc_lrc(u8 *buf, int len)
{
	int i;
	u8 sum;

	sum = 0;

	for (i = 0; i < len; i++)
		sum = sum ^ buf[i];

	return sum;
}

static int eeprom_lrc_read(struct adapter *adapter, u32 addr, u32 len, u8 *buf, int retries)
{
	int i;

	for (i = 0; i < retries; i++) {
		if (eeprom_read(adapter, addr, buf, len) == len) {
			if (calc_lrc(buf, len - 1) == buf[len - 1])
				return 1;
		}
	}

	return 0;
}

/*
static int eeprom_lrc_write(struct adapter *adapter, u32 addr, u32 len, u8 *wbuf, u8 *rbuf, int retries)
{
	int i;

	for (i = 0; i < retries; i++) {
		if (eeprom_write(adapter, addr, wbuf, len) == len) {
			if (eeprom_lrc_read(adapter, addr, len, rbuf, retries) == 1)
				return 1;
		}
	}

	return 0;
}
*/


/* These functions could be used to unlock SkyStar2 cards. */

/*
static int eeprom_writeKey(struct adapter *adapter, u8 *key, u32 len)
{
	u8 rbuf[20];
	u8 wbuf[20];

	if (len != 16)
		return 0;

	memcpy(wbuf, key, len);

	wbuf[16] = 0;
	wbuf[17] = 0;
	wbuf[18] = 0;
	wbuf[19] = calc_lrc(wbuf, 19);

	return eeprom_lrc_write(adapter, 0x3e4, 20, wbuf, rbuf, 4);
}

static int eeprom_readKey(struct adapter *adapter, u8 *key, u32 len)
{
	u8 buf[20];

	if (len != 16)
		return 0;

	if (eeprom_lrc_read(adapter, 0x3e4, 20, buf, 4) == 0)
		return 0;

	memcpy(key, buf, len);

	return 1;
}
*/

static int eeprom_get_mac_addr(struct adapter *adapter, char type, u8 *mac)
{
	u8 tmp[8];

	if (eeprom_lrc_read(adapter, 0x3f8, 8, tmp, 4) != 0) {
		if (type != 0) {
			mac[0] = tmp[0];
			mac[1] = tmp[1];
			mac[2] = tmp[2];
			mac[3] = 0xfe;
			mac[4] = 0xff;
			mac[5] = tmp[3];
			mac[6] = tmp[4];
			mac[7] = tmp[5];

		} else {

			mac[0] = tmp[0];
			mac[1] = tmp[1];
			mac[2] = tmp[2];
			mac[3] = tmp[3];
			mac[4] = tmp[4];
			mac[5] = tmp[5];
		}

		return 1;

	} else {

		if (type == 0) {
			memset(mac, 0, 6);

		} else {

			memset(mac, 0, 8);
		}

		return 0;
	}
}

/*
static char eeprom_set_mac_addr(struct adapter *adapter, char type, u8 *mac)
{
	u8 tmp[8];

	if (type != 0) {
		tmp[0] = mac[0];
		tmp[1] = mac[1];
		tmp[2] = mac[2];
		tmp[3] = mac[5];
		tmp[4] = mac[6];
		tmp[5] = mac[7];

	} else {

		tmp[0] = mac[0];
		tmp[1] = mac[1];
		tmp[2] = mac[2];
		tmp[3] = mac[3];
		tmp[4] = mac[4];
		tmp[5] = mac[5];
	}

	tmp[6] = 0;
	tmp[7] = calc_lrc(tmp, 7);

	if (eeprom_write(adapter, 0x3f8, tmp, 8) == 8)
		return 1;

	return 0;
}
*/

/* PID filter */

/* every flexcop has 6 "lower" hw PID filters     */
/* these are enabled by setting bits 0-5 of 0x208 */
/* for the 32 additional filters we have to select one */
/* of them through 0x310 and modify through 0x314 */
/* op: 0=disable, 1=enable */
static void filter_enable_hw_filter(struct adapter *adapter, int id, u8 op)
{
	dprintk("%s: id=%d op=%d\n", __FUNCTION__, id, op);
	if (id <= 5) {
		u32 mask = (0x00000001 << id);
		write_reg_bitfield(adapter, 0x208, mask, op ? mask : 0);
	} else {
		/* select */
		write_reg_bitfield(adapter, 0x310, 0x1f, (id - 6) & 0x1f);
		/* modify */
		write_reg_bitfield(adapter, 0x314, 0x00006000, op ? 0x00004000 : 0);
	}
}

/* this sets the PID that should pass the specified filter */
static void pid_set_hw_pid(struct adapter *adapter, int id, u16 pid)
{
	dprintk("%s: id=%d  pid=%d\n", __FUNCTION__, id, pid);
	if (id <= 5) {
		u32 adr = 0x300 + ((id & 6) << 1);
		int shift = (id & 1) ? 16 : 0;
		dprintk("%s: id=%d  addr=%x %c  pid=%d\n", __FUNCTION__, id, adr, (id & 1) ? 'h' : 'l', pid);
		write_reg_bitfield(adapter, adr, (0x7fff) << shift, (pid & 0x1fff) << shift);
	} else {
		/* select */
		write_reg_bitfield(adapter, 0x310, 0x1f, (id - 6) & 0x1f);
		/* modify */
		write_reg_bitfield(adapter, 0x314, 0x1fff, pid & 0x1fff);
	}
}


/*
static void filter_enable_null_filter(struct adapter *adapter, u32 op)
{
	dprintk("%s: op=%x\n", __FUNCTION__, op);

	write_reg_bitfield(adapter, 0x208, 0x00000040, op?0x00000040:0);
}
*/

static void filter_enable_mask_filter(struct adapter *adapter, u32 op)
{
	dprintk("%s: op=%x\n", __FUNCTION__, op);

	write_reg_bitfield(adapter, 0x208, 0x00000080, op ? 0x00000080 : 0);
}


static void ctrl_enable_mac(struct adapter *adapter, u32 op)
{
	write_reg_bitfield(adapter, 0x208, 0x00004000, op ? 0x00004000 : 0);
}

static int ca_set_mac_dst_addr_filter(struct adapter *adapter, u8 *mac)
{
	u32 tmp1, tmp2;

	tmp1 = (mac[3] << 0x18) | (mac[2] << 0x10) | (mac[1] << 0x08) | mac[0];
	tmp2 = (mac[5] << 0x08) | mac[4];

	write_reg_dw(adapter, 0x418, tmp1);
	write_reg_dw(adapter, 0x41c, tmp2);

	return 0;
}

/*
static void set_ignore_mac_filter(struct adapter *adapter, u8 op)
{
	if (op != 0) {
		write_reg_bitfield(adapter, 0x208, 0x00004000, 0);
		adapter->mac_filter = 1;
	} else {
		if (adapter->mac_filter != 0) {
			adapter->mac_filter = 0;
			write_reg_bitfield(adapter, 0x208, 0x00004000, 0x00004000);
		}
	}
}
*/

/*
static void check_null_filter_enable(struct adapter *adapter)
{
	filter_enable_null_filter(adapter, 1);
	filter_enable_mask_filter(adapter, 1);
}
*/

static void pid_set_group_pid(struct adapter *adapter, u16 pid)
{
	u32 value;

	dprintk("%s: pid=%x\n", __FUNCTION__, pid);
	value = (pid & 0x3fff) | (read_reg_dw(adapter, 0x30c) & 0xffff0000);
	write_reg_dw(adapter, 0x30c, value);
}

static void pid_set_group_mask(struct adapter *adapter, u16 pid)
{
	u32 value;

	dprintk("%s: pid=%x\n", __FUNCTION__, pid);
	value = ((pid & 0x3fff) << 0x10) | (read_reg_dw(adapter, 0x30c) & 0xffff);
	write_reg_dw(adapter, 0x30c, value);
}

/*
static int pid_get_group_pid(struct adapter *adapter)
{
	return read_reg_dw(adapter, 0x30c) & 0x00001fff;
}

static int pid_get_group_mask(struct adapter *adapter)
{
	return (read_reg_dw(adapter, 0x30c) >> 0x10)& 0x00001fff;
}
*/

/*
static void reset_hardware_pid_filter(struct adapter *adapter)
{
	pid_set_stream1_pid(adapter, 0x1fff);

	pid_set_stream2_pid(adapter, 0x1fff);
	filter_enable_stream2_filter(adapter, 0);

	pid_set_pcr_pid(adapter, 0x1fff);
	filter_enable_pcr_filter(adapter, 0);

	pid_set_pmt_pid(adapter, 0x1fff);
	filter_enable_pmt_filter(adapter, 0);

	pid_set_ecm_pid(adapter, 0x1fff);
	filter_enable_ecm_filter(adapter, 0);

	pid_set_emm_pid(adapter, 0x1fff);
	filter_enable_emm_filter(adapter, 0);
}
*/

static void init_pids(struct adapter *adapter)
{
	int i;

	adapter->pid_count = 0;
	adapter->whole_bandwidth_count = 0;
	for (i = 0; i < adapter->useable_hw_filters; i++) {
		dprintk("%s: setting filter %d to 0x1fff\n", __FUNCTION__, i);
		adapter->hw_pids[i] = 0x1fff;
		pid_set_hw_pid(adapter, i, 0x1fff);
}

	pid_set_group_pid(adapter, 0);
	pid_set_group_mask(adapter, 0x1fe0);
}

static void open_whole_bandwidth(struct adapter *adapter)
{
	dprintk("%s:\n", __FUNCTION__);
	pid_set_group_pid(adapter, 0);
	pid_set_group_mask(adapter, 0);
/*
	filter_enable_mask_filter(adapter, 1);
*/
}

static void close_whole_bandwidth(struct adapter *adapter)
{
	dprintk("%s:\n", __FUNCTION__);
	pid_set_group_pid(adapter, 0);
	pid_set_group_mask(adapter, 0x1fe0);
/*
	filter_enable_mask_filter(adapter, 1);
*/
}

static void whole_bandwidth_inc(struct adapter *adapter)
{
	if (adapter->whole_bandwidth_count++ == 0)
		open_whole_bandwidth(adapter);
}

static void whole_bandwidth_dec(struct adapter *adapter)
{
	if (--adapter->whole_bandwidth_count <= 0)
		close_whole_bandwidth(adapter);
}

/* The specified PID has to be let through the
   hw filters.
   We try to allocate an hardware filter and open whole
   bandwidth when allocation is impossible.
   All pids<=0x1f pass through the group filter.
   Returns 1 on success, -1 on error */
static int add_hw_pid(struct adapter *adapter, u16 pid)
{
	int i;

	dprintk("%s: pid=%d\n", __FUNCTION__, pid);

	if (pid <= 0x1f)
		return 1;

	/* we can't use a filter for 0x2000, so no search */
	if (pid != 0x2000) {
		/* find an unused hardware filter */
		for (i = 0; i < adapter->useable_hw_filters; i++) {
			dprintk("%s: pid=%d searching slot=%d\n", __FUNCTION__, pid, i);
			if (adapter->hw_pids[i] == 0x1fff) {
				dprintk("%s: pid=%d slot=%d\n", __FUNCTION__, pid, i);
				adapter->hw_pids[i] = pid;
				pid_set_hw_pid(adapter, i, pid);
				filter_enable_hw_filter(adapter, i, 1);
		return 1;
	}
	}
	}
	/* if we have not used a filter, this pid depends on whole bandwidth */
	dprintk("%s: pid=%d whole_bandwidth\n", __FUNCTION__, pid);
	whole_bandwidth_inc(adapter);
		return 1;
	}

/* returns -1 if the pid was not present in the filters */
static int remove_hw_pid(struct adapter *adapter, u16 pid)
{
	int i;

	dprintk("%s: pid=%d\n", __FUNCTION__, pid);

	if (pid <= 0x1f)
		return 1;

	/* we can't use a filter for 0x2000, so no search */
	if (pid != 0x2000) {
		for (i = 0; i < adapter->useable_hw_filters; i++) {
			dprintk("%s: pid=%d searching slot=%d\n", __FUNCTION__, pid, i);
			if (adapter->hw_pids[i] == pid) {	// find the pid slot
				dprintk("%s: pid=%d slot=%d\n", __FUNCTION__, pid, i);
				adapter->hw_pids[i] = 0x1fff;
				pid_set_hw_pid(adapter, i, 0x1fff);
				filter_enable_hw_filter(adapter, i, 0);
		return 1;
	}
	}
	}
	/* if we have not used a filter, this pid depended on whole bandwith */
	dprintk("%s: pid=%d whole_bandwidth\n", __FUNCTION__, pid);
	whole_bandwidth_dec(adapter);
		return 1;
	}

/* Adds a PID to the filters.
   Adding a pid more than once is possible, we keep reference counts.
   Whole stream available through pid==0x2000.
   Returns 1 on success, -1 on error */
static int add_pid(struct adapter *adapter, u16 pid)
{
	int i;

	dprintk("%s: pid=%d\n", __FUNCTION__, pid);

	if (pid > 0x1ffe && pid != 0x2000)
		return -1;

	// check if the pid is already present
	for (i = 0; i < adapter->pid_count; i++)
		if (adapter->pid_list[i] == pid) {
			adapter->pid_rc[i]++;	// increment ref counter
		return 1;
		}

	if (adapter->pid_count == N_PID_SLOTS)
		return -1;	// no more pids can be added
	adapter->pid_list[adapter->pid_count] = pid;	// register pid
	adapter->pid_rc[adapter->pid_count] = 1;
	adapter->pid_count++;
	// hardware setting
	add_hw_pid(adapter, pid);

			return 1;
		}

/* Removes a PID from the filters. */
static int remove_pid(struct adapter *adapter, u16 pid)
{
	int i;

	dprintk("%s: pid=%d\n", __FUNCTION__, pid);

	if (pid > 0x1ffe && pid != 0x2000)
		return -1;

	// check if the pid is present (it must be!)
	for (i = 0; i < adapter->pid_count; i++) {
		if (adapter->pid_list[i] == pid) {
			adapter->pid_rc[i]--;
			if (adapter->pid_rc[i] <= 0) {
				// remove from the list
				adapter->pid_count--;
				adapter->pid_list[i]=adapter->pid_list[adapter->pid_count];
				adapter->pid_rc[i] = adapter->pid_rc[adapter->pid_count];
				// hardware setting
				remove_hw_pid(adapter, pid);
			}
			return 1;
		}
	}

	return -1;
}


/* dma & irq */
static void ctrl_enable_smc(struct adapter *adapter, u32 op)
{
	write_reg_bitfield(adapter, 0x208, 0x00000800, op ? 0x00000800 : 0);
}

static void dma_enable_disable_irq(struct adapter *adapter, u32 flag1, u32 flag2, u32 flag3)
{
	adapter->dma_ctrl = adapter->dma_ctrl & 0x000f0000;

	if (flag1 == 0) {
		if (flag2 == 0)
			adapter->dma_ctrl = adapter->dma_ctrl & ~0x00010000;
		else
			adapter->dma_ctrl = adapter->dma_ctrl | 0x00010000;

		if (flag3 == 0)
			adapter->dma_ctrl = adapter->dma_ctrl & ~0x00020000;
		else
			adapter->dma_ctrl = adapter->dma_ctrl | 0x00020000;

	} else {

		if (flag2 == 0)
			adapter->dma_ctrl = adapter->dma_ctrl & ~0x00040000;
		else
			adapter->dma_ctrl = adapter->dma_ctrl | 0x00040000;

		if (flag3 == 0)
			adapter->dma_ctrl = adapter->dma_ctrl & ~0x00080000;
		else
			adapter->dma_ctrl = adapter->dma_ctrl | 0x00080000;
	}
}

static void irq_dma_enable_disable_irq(struct adapter *adapter, u32 op)
{
	u32 value;

	value = read_reg_dw(adapter, 0x208) & 0xfff0ffff;

	if (op != 0)
		value = value | (adapter->dma_ctrl & 0x000f0000);

	write_reg_dw(adapter, 0x208, value);
}

/* FlexCopII has 2 dma channels. DMA1 is used to transfer TS data to
   system memory.

   The DMA1 buffer is divided in 2 subbuffers of equal size.
   FlexCopII will transfer TS data to one subbuffer, signal an interrupt
   when the subbuffer is full and continue fillig the second subbuffer.

   For DMA1:
       subbuffer size in 32-bit words is stored in the first 24 bits of
       register 0x004. The last 8 bits of register 0x004 contain the number
       of subbuffers.
       
       the first 30 bits of register 0x000 contain the address of the first
       subbuffer. The last 2 bits contain 0, when dma1 is disabled and 1,
       when dma1 is enabled.

       the first 30 bits of register 0x00c contain the address of the second
       subbuffer. the last 2 bits contain 1.

       register 0x008 will contain the address of the subbuffer that was filled
       with TS data, when FlexCopII will generate an interrupt.

   For DMA2:
       subbuffer size in 32-bit words is stored in the first 24 bits of
       register 0x014. The last 8 bits of register 0x014 contain the number
       of subbuffers.
       
       the first 30 bits of register 0x010 contain the address of the first
       subbuffer.  The last 2 bits contain 0, when dma1 is disabled and 1,
       when dma1 is enabled.

       the first 30 bits of register 0x01c contain the address of the second
       subbuffer. the last 2 bits contain 1.

       register 0x018 contains the address of the subbuffer that was filled
       with TS data, when FlexCopII generates an interrupt.
*/
static int dma_init_dma(struct adapter *adapter, u32 dma_channel)
{
	u32 subbuffers, subbufsize, subbuf0, subbuf1;

	if (dma_channel == 0) {
		dprintk("%s: Initializing DMA1 channel\n", __FUNCTION__);

		subbuffers = 2;

		subbufsize = (((adapter->dmaq1.buffer_size / 2) / 4) << 8) | subbuffers;

		subbuf0 = adapter->dmaq1.bus_addr & 0xfffffffc;

		subbuf1 = ((adapter->dmaq1.bus_addr + adapter->dmaq1.buffer_size / 2) & 0xfffffffc) | 1;

		dprintk("%s: first subbuffer address = 0x%x\n", __FUNCTION__, subbuf0);
		udelay(1000);
		write_reg_dw(adapter, 0x000, subbuf0);

		dprintk("%s: subbuffer size = 0x%x\n", __FUNCTION__, (subbufsize >> 8) * 4);
		udelay(1000);
		write_reg_dw(adapter, 0x004, subbufsize);

		dprintk("%s: second subbuffer address = 0x%x\n", __FUNCTION__, subbuf1);
		udelay(1000);
		write_reg_dw(adapter, 0x00c, subbuf1);

		dprintk("%s: counter = 0x%x\n", __FUNCTION__, adapter->dmaq1.bus_addr & 0xfffffffc);
		write_reg_dw(adapter, 0x008, adapter->dmaq1.bus_addr & 0xfffffffc);
		udelay(1000);

		if (subbuffers == 0)
			dma_enable_disable_irq(adapter, 0, 1, 0);
		else
			dma_enable_disable_irq(adapter, 0, 1, 1);

		irq_dma_enable_disable_irq(adapter, 1);

		sram_set_media_dest(adapter, 1);
		sram_set_net_dest(adapter, 1);
		sram_set_cai_dest(adapter, 2);
		sram_set_cao_dest(adapter, 2);
	}

	if (dma_channel == 1) {
		dprintk("%s: Initializing DMA2 channel\n", __FUNCTION__);

		subbuffers = 2;

		subbufsize = (((adapter->dmaq2.buffer_size / 2) / 4) << 8) | subbuffers;

		subbuf0 = adapter->dmaq2.bus_addr & 0xfffffffc;

		subbuf1 = ((adapter->dmaq2.bus_addr + adapter->dmaq2.buffer_size / 2) & 0xfffffffc) | 1;

		dprintk("%s: first subbuffer address = 0x%x\n", __FUNCTION__, subbuf0);
		udelay(1000);
		write_reg_dw(adapter, 0x010, subbuf0);

		dprintk("%s: subbuffer size = 0x%x\n", __FUNCTION__, (subbufsize >> 8) * 4);
		udelay(1000);
		write_reg_dw(adapter, 0x014, subbufsize);

		dprintk("%s: second buffer address = 0x%x\n", __FUNCTION__, subbuf1);
		udelay(1000);
		write_reg_dw(adapter, 0x01c, subbuf1);

		sram_set_cai_dest(adapter, 2);
	}

	return 0;
}

static void ctrl_enable_receive_data(struct adapter *adapter, u32 op)
{
	if (op == 0) {
		write_reg_bitfield(adapter, 0x208, 0x00008000, 0);
		adapter->dma_status = adapter->dma_status & ~0x00000004;
	} else {
		write_reg_bitfield(adapter, 0x208, 0x00008000, 0x00008000);
		adapter->dma_status = adapter->dma_status | 0x00000004;
	}
}

/* bit 0 of dma_mask is set to 1 if dma1 channel has to be enabled/disabled
   bit 1 of dma_mask is set to 1 if dma2 channel has to be enabled/disabled
*/
static void dma_start_stop(struct adapter *adapter, u32 dma_mask, int start_stop)
{
	u32 dma_enable, dma1_enable, dma2_enable;

	dprintk("%s: dma_mask=%x\n", __FUNCTION__, dma_mask);

	if (start_stop == 1) {
		dprintk("%s: starting dma\n", __FUNCTION__);

		dma1_enable = 0;
		dma2_enable = 0;

		if (((dma_mask & 1) != 0) && ((adapter->dma_status & 1) == 0) && (adapter->dmaq1.bus_addr != 0)) {
			adapter->dma_status = adapter->dma_status | 1;
			dma1_enable = 1;
		}

		if (((dma_mask & 2) != 0) && ((adapter->dma_status & 2) == 0) && (adapter->dmaq2.bus_addr != 0)) {
			adapter->dma_status = adapter->dma_status | 2;
			dma2_enable = 1;
		}
		// enable dma1 and dma2
		if ((dma1_enable == 1) && (dma2_enable == 1)) {
			write_reg_dw(adapter, 0x000, adapter->dmaq1.bus_addr | 1);
			write_reg_dw(adapter, 0x00c, (adapter->dmaq1.bus_addr + adapter->dmaq1.buffer_size / 2) | 1);
			write_reg_dw(adapter, 0x010, adapter->dmaq2.bus_addr | 1);

			ctrl_enable_receive_data(adapter, 1);

			return;
		}
		// enable dma1
		if ((dma1_enable == 1) && (dma2_enable == 0)) {
			write_reg_dw(adapter, 0x000, adapter->dmaq1.bus_addr | 1);
			write_reg_dw(adapter, 0x00c, (adapter->dmaq1.bus_addr + adapter->dmaq1.buffer_size / 2) | 1);

			ctrl_enable_receive_data(adapter, 1);

			return;
		}
		// enable dma2
		if ((dma1_enable == 0) && (dma2_enable == 1)) {
			write_reg_dw(adapter, 0x010, adapter->dmaq2.bus_addr | 1);

			ctrl_enable_receive_data(adapter, 1);

			return;
		}
		// start dma
		if ((dma1_enable == 0) && (dma2_enable == 0)) {
			ctrl_enable_receive_data(adapter, 1);

			return;
		}

	} else {

		dprintk("%s: stopping dma\n", __FUNCTION__);

		dma_enable = adapter->dma_status & 0x00000003;

		if (((dma_mask & 1) != 0) && ((adapter->dma_status & 1) != 0)) {
			dma_enable = dma_enable & 0xfffffffe;
		}

		if (((dma_mask & 2) != 0) && ((adapter->dma_status & 2) != 0)) {
			dma_enable = dma_enable & 0xfffffffd;
		}
		//stop dma
		if ((dma_enable == 0) && ((adapter->dma_status & 4) != 0)) {
			ctrl_enable_receive_data(adapter, 0);

			udelay(3000);
		}
		//disable dma1
		if (((dma_mask & 1) != 0) && ((adapter->dma_status & 1) != 0) && (adapter->dmaq1.bus_addr != 0)) {
			write_reg_dw(adapter, 0x000, adapter->dmaq1.bus_addr);
			write_reg_dw(adapter, 0x00c, (adapter->dmaq1.bus_addr + adapter->dmaq1.buffer_size / 2) | 1);

			adapter->dma_status = adapter->dma_status & ~0x00000001;
		}
		//disable dma2
		if (((dma_mask & 2) != 0) && ((adapter->dma_status & 2) != 0) && (adapter->dmaq2.bus_addr != 0)) {
			write_reg_dw(adapter, 0x010, adapter->dmaq2.bus_addr);

			adapter->dma_status = adapter->dma_status & ~0x00000002;
		}
	}
}

static void open_stream(struct adapter *adapter, u16 pid)
{
	u32 dma_mask;

	++adapter->capturing;

	filter_enable_mask_filter(adapter, 1);

	add_pid(adapter, pid);

	dprintk("%s: adapter->dma_status=%x\n", __FUNCTION__, adapter->dma_status);

	if ((adapter->dma_status & 7) != 7) {
		dma_mask = 0;

		if (((adapter->dma_status & 0x10000000) != 0) && ((adapter->dma_status & 1) == 0)) {
			dma_mask = dma_mask | 1;

			adapter->dmaq1.head = 0;
			adapter->dmaq1.tail = 0;

			memset(adapter->dmaq1.buffer, 0, adapter->dmaq1.buffer_size);
		}

		if (((adapter->dma_status & 0x20000000) != 0) && ((adapter->dma_status & 2) == 0)) {
			dma_mask = dma_mask | 2;

			adapter->dmaq2.head = 0;
			adapter->dmaq2.tail = 0;
		}

		if (dma_mask != 0) {
			irq_dma_enable_disable_irq(adapter, 1);

			dma_start_stop(adapter, dma_mask, 1);
		}
	}
}

static void close_stream(struct adapter *adapter, u16 pid)
{
	if (adapter->capturing > 0)
		--adapter->capturing;

	dprintk("%s: dma_status=%x\n", __FUNCTION__, adapter->dma_status);

	if (adapter->capturing == 0) {
		u32 dma_mask = 0;

	if ((adapter->dma_status & 1) != 0)
		dma_mask = dma_mask | 0x00000001;
	if ((adapter->dma_status & 2) != 0)
		dma_mask = dma_mask | 0x00000002;

	if (dma_mask != 0) {
			dma_start_stop(adapter, dma_mask, 0);
	}
	}
	remove_pid(adapter, pid);
}

static void interrupt_service_dma1(struct adapter *adapter)
{
	struct dvb_demux *dvbdmx = &adapter->demux;

	int n_cur_dma_counter;
	u32 n_num_bytes_parsed;
	u32 n_num_new_bytes_transferred;
	u32 dw_default_packet_size = 188;
	u8 gb_tmp_buffer[188];
	u8 *pb_dma_buf_cur_pos;

	n_cur_dma_counter = readl(adapter->io_mem + 0x008) - adapter->dmaq1.bus_addr;
	n_cur_dma_counter = (n_cur_dma_counter / dw_default_packet_size) * dw_default_packet_size;

	if ((n_cur_dma_counter < 0) || (n_cur_dma_counter > adapter->dmaq1.buffer_size)) {
		dprintk("%s: dma counter outside dma buffer\n", __FUNCTION__);
		return;
	}

	adapter->dmaq1.head = n_cur_dma_counter;

	if (adapter->dmaq1.tail <= n_cur_dma_counter) {
		n_num_new_bytes_transferred = n_cur_dma_counter - adapter->dmaq1.tail;

	} else {

		n_num_new_bytes_transferred = (adapter->dmaq1.buffer_size - adapter->dmaq1.tail) + n_cur_dma_counter;
	}

	ddprintk("%s: n_cur_dma_counter = %d\n", __FUNCTION__, n_cur_dma_counter);
	ddprintk("%s: dmaq1.tail        = %d\n", __FUNCTION__, adapter->dmaq1.tail);
	ddprintk("%s: bytes_transferred = %d\n", __FUNCTION__, n_num_new_bytes_transferred);

	if (n_num_new_bytes_transferred < dw_default_packet_size)
		return;

	n_num_bytes_parsed = 0;

	while (n_num_bytes_parsed < n_num_new_bytes_transferred) {
		pb_dma_buf_cur_pos = adapter->dmaq1.buffer + adapter->dmaq1.tail;

		if (adapter->dmaq1.buffer + adapter->dmaq1.buffer_size < adapter->dmaq1.buffer + adapter->dmaq1.tail + 188) {
			memcpy(gb_tmp_buffer, adapter->dmaq1.buffer + adapter->dmaq1.tail,
			       adapter->dmaq1.buffer_size - adapter->dmaq1.tail);
			memcpy(gb_tmp_buffer + (adapter->dmaq1.buffer_size - adapter->dmaq1.tail), adapter->dmaq1.buffer,
			       (188 - (adapter->dmaq1.buffer_size - adapter->dmaq1.tail)));

			pb_dma_buf_cur_pos = gb_tmp_buffer;
		}

		if (adapter->capturing != 0) {
			dvb_dmx_swfilter_packets(dvbdmx, pb_dma_buf_cur_pos, dw_default_packet_size / 188);
		}

		n_num_bytes_parsed = n_num_bytes_parsed + dw_default_packet_size;

		adapter->dmaq1.tail = adapter->dmaq1.tail + dw_default_packet_size;

		if (adapter->dmaq1.tail >= adapter->dmaq1.buffer_size)
			adapter->dmaq1.tail = adapter->dmaq1.tail - adapter->dmaq1.buffer_size;
	};
}

static void interrupt_service_dma2(struct adapter *adapter)
{
	printk("%s:\n", __FUNCTION__);
}

static irqreturn_t isr(int irq, void *dev_id, struct pt_regs *regs)
{
	struct adapter *tmp = dev_id;

	u32 value;

	ddprintk("%s:\n", __FUNCTION__);

	spin_lock_irq(&tmp->lock);

	if (0 == ((value = read_reg_dw(tmp, 0x20c)) & 0x0f)) {
		spin_unlock_irq(&tmp->lock);
		return IRQ_NONE;
	}
	
	while (value != 0) {
		if ((value & 0x03) != 0)
			interrupt_service_dma1(tmp);
		if ((value & 0x0c) != 0)
			interrupt_service_dma2(tmp);
		value = read_reg_dw(tmp, 0x20c) & 0x0f;
	}

	spin_unlock_irq(&tmp->lock);
	return IRQ_HANDLED;
}

static void init_dma_queue(struct adapter *adapter)
{
	dma_addr_t dma_addr;

	if (adapter->dmaq1.buffer != 0)
		return;

	adapter->dmaq1.head = 0;
	adapter->dmaq1.tail = 0;
	adapter->dmaq1.buffer = NULL;

	adapter->dmaq1.buffer = pci_alloc_consistent(adapter->pdev, SIZE_OF_BUF_DMA1 + 0x80, &dma_addr);

	if (adapter->dmaq1.buffer != 0) {
		memset(adapter->dmaq1.buffer, 0, SIZE_OF_BUF_DMA1);

		adapter->dmaq1.bus_addr = dma_addr;
		adapter->dmaq1.buffer_size = SIZE_OF_BUF_DMA1;

		dma_init_dma(adapter, 0);

		adapter->dma_status = adapter->dma_status | 0x10000000;

		ddprintk("%s: allocated dma buffer at 0x%p, length=%d\n", __FUNCTION__, adapter->dmaq1.buffer, SIZE_OF_BUF_DMA1);

	} else {

		adapter->dma_status = adapter->dma_status & ~0x10000000;
	}

	if (adapter->dmaq2.buffer != 0)
		return;

	adapter->dmaq2.head = 0;
	adapter->dmaq2.tail = 0;
	adapter->dmaq2.buffer = NULL;

	adapter->dmaq2.buffer = pci_alloc_consistent(adapter->pdev, SIZE_OF_BUF_DMA2 + 0x80, &dma_addr);

	if (adapter->dmaq2.buffer != 0) {
		memset(adapter->dmaq2.buffer, 0, SIZE_OF_BUF_DMA2);

		adapter->dmaq2.bus_addr = dma_addr;
		adapter->dmaq2.buffer_size = SIZE_OF_BUF_DMA2;

		dma_init_dma(adapter, 1);

		adapter->dma_status = adapter->dma_status | 0x20000000;

		ddprintk("%s: allocated dma buffer at 0x%p, length=%d\n", __FUNCTION__, adapter->dmaq2.buffer, (int) SIZE_OF_BUF_DMA2);

	} else {

		adapter->dma_status = adapter->dma_status & ~0x20000000;
	}
}

static void free_dma_queue(struct adapter *adapter)
{
	if (adapter->dmaq1.buffer != 0) {
		pci_free_consistent(adapter->pdev, SIZE_OF_BUF_DMA1 + 0x80, adapter->dmaq1.buffer, adapter->dmaq1.bus_addr);

		adapter->dmaq1.bus_addr = 0;
		adapter->dmaq1.head = 0;
		adapter->dmaq1.tail = 0;
		adapter->dmaq1.buffer_size = 0;
		adapter->dmaq1.buffer = NULL;
	}

	if (adapter->dmaq2.buffer != 0) {
		pci_free_consistent(adapter->pdev, SIZE_OF_BUF_DMA2 + 0x80, adapter->dmaq2.buffer, adapter->dmaq2.bus_addr);

		adapter->dmaq2.bus_addr = 0;
		adapter->dmaq2.head = 0;
		adapter->dmaq2.tail = 0;
		adapter->dmaq2.buffer_size = 0;
		adapter->dmaq2.buffer = NULL;
	}
}

static void free_adapter_object(struct adapter *adapter)
{
	dprintk("%s:\n", __FUNCTION__);

	close_stream(adapter, 0);

	if (adapter->irq != 0)
		free_irq(adapter->irq, adapter);

	free_dma_queue(adapter);

	if (adapter->io_mem != 0)
		iounmap((void *) adapter->io_mem);

	if (adapter != 0)
		kfree(adapter);
}

static struct pci_driver skystar2_pci_driver;

static int claim_adapter(struct adapter *adapter)
{
	struct pci_dev *pdev = adapter->pdev;

	u16 var;

	if (!request_region(pci_resource_start(pdev, 1), pci_resource_len(pdev, 1), skystar2_pci_driver.name))
		return -EBUSY;

	if (!request_mem_region(pci_resource_start(pdev, 0), pci_resource_len(pdev, 0), skystar2_pci_driver.name))
		return -EBUSY;

	pci_read_config_byte(pdev, PCI_CLASS_REVISION, &adapter->card_revision);

	dprintk("%s: card revision %x \n", __FUNCTION__, adapter->card_revision);

	if (pci_enable_device(pdev))
		return -EIO;

	pci_read_config_word(pdev, 4, &var);

	if ((var & 4) == 0)
		pci_set_master(pdev);

	adapter->io_port = pdev->resource[1].start;

	adapter->io_mem = (unsigned long) ioremap(pdev->resource[0].start, 0x800);

	if (adapter->io_mem == 0) {
		dprintk("%s: can not map io memory\n", __FUNCTION__);

		return 2;
	}

	dprintk("%s: io memory maped at %lx\n", __FUNCTION__, adapter->io_mem);

	return 1;
}

/*
static int sll_reset_flexcop(struct adapter *adapter)
{
	write_reg_dw(adapter, 0x208, 0);
	write_reg_dw(adapter, 0x210, 0xb2ff);

	return 0;
}
*/

static void decide_how_many_hw_filters(struct adapter *adapter)
{
	int hw_filters;
	int mod_option_hw_filters;

	// FlexCop IIb & III have 6+32 hw filters    
	// FlexCop II has 6 hw filters, every other should have at least 6
	switch (adapter->b2c2_revision) {
	case 0x82:		/* II */
		hw_filters = 6;
		break;
	case 0xc3:		/* IIB */
		hw_filters = 6 + 32;
		break;
	case 0xc0:		/* III */
		hw_filters = 6 + 32;
		break;
	default:
		hw_filters = 6;
		break;
	}
	printk("%s: the chip has %i hardware filters", __FILE__, hw_filters);

	mod_option_hw_filters = 0;
	if (enable_hw_filters >= 1)
		mod_option_hw_filters += 6;
	if (enable_hw_filters >= 2)
		mod_option_hw_filters += 32;

	if (mod_option_hw_filters >= hw_filters) {
		adapter->useable_hw_filters = hw_filters;
	} else {
		adapter->useable_hw_filters = mod_option_hw_filters;
		printk(", but only %d will be used because of module option", mod_option_hw_filters);
	}
	printk("\n");
	dprintk("%s: useable_hardware_filters set to %i\n", __FILE__, adapter->useable_hw_filters);
}

static int driver_initialize(struct pci_dev *pdev)
{
	struct adapter *adapter;
	u32 tmp;

	if (!(adapter = kmalloc(sizeof(struct adapter), GFP_KERNEL))) {
		dprintk("%s: out of memory!\n", __FUNCTION__);

		return -ENOMEM;
	}

	memset(adapter, 0, sizeof(struct adapter));

	pci_set_drvdata(pdev,adapter);

	adapter->pdev = pdev;
	adapter->irq = pdev->irq;

	if ((claim_adapter(adapter)) != 1) {
		free_adapter_object(adapter);

		return -ENODEV;
	}

	irq_dma_enable_disable_irq(adapter, 0);

	if (request_irq(pdev->irq, isr, 0x4000000, "Skystar2", adapter) != 0) {
		dprintk("%s: unable to allocate irq=%d !\n", __FUNCTION__, pdev->irq);

		free_adapter_object(adapter);

		return -ENODEV;
	}

	read_reg_dw(adapter, 0x208);
	write_reg_dw(adapter, 0x208, 0);
	write_reg_dw(adapter, 0x210, 0xb2ff);
	write_reg_dw(adapter, 0x208, 0x40);

	init_dma_queue(adapter);

	if ((adapter->dma_status & 0x30000000) == 0) {
		free_adapter_object(adapter);

		return -ENODEV;
	}

	adapter->b2c2_revision = (read_reg_dw(adapter, 0x204) >> 0x18);

	switch (adapter->b2c2_revision) {
	case 0x82:
		printk("%s: FlexCopII(rev.130) chip found\n", __FILE__);
		break;
	case 0xc3:
		printk("%s: FlexCopIIB(rev.195) chip found\n", __FILE__);
		break;
	case 0xc0:
		printk("%s: FlexCopIII(rev.192) chip found\n", __FILE__);
		break;
	default:
		printk("%s: The revision of the FlexCop chip on your card is %d\n", __FILE__, adapter->b2c2_revision);
		printk("%s: This driver works only with FlexCopII(rev.130), FlexCopIIB(rev.195) and FlexCopIII(rev.192).\n", __FILE__);
		free_adapter_object(adapter);
		pci_set_drvdata(pdev, NULL);
		release_region(pci_resource_start(pdev, 1), pci_resource_len(pdev, 1));
		release_mem_region(pci_resource_start(pdev, 0), pci_resource_len(pdev, 0));
			return -ENODEV;
		}

	decide_how_many_hw_filters(adapter);

	init_pids(adapter);

	tmp = read_reg_dw(adapter, 0x204);

	write_reg_dw(adapter, 0x204, 0);
	mdelay(20);

	write_reg_dw(adapter, 0x204, tmp);
	mdelay(10);

	tmp = read_reg_dw(adapter, 0x308);
	write_reg_dw(adapter, 0x308, 0x4000 | tmp);

	adapter->dw_sram_type = 0x10000;

	sll_detect_sram_size(adapter);

	dprintk("%s sram length = %d, sram type= %x\n", __FUNCTION__, sram_length(adapter), adapter->dw_sram_type);

	sram_set_media_dest(adapter, 1);
	sram_set_net_dest(adapter, 1);

	ctrl_enable_smc(adapter, 0);

	sram_set_cai_dest(adapter, 2);
	sram_set_cao_dest(adapter, 2);

	dma_enable_disable_irq(adapter, 1, 0, 0);

	if (eeprom_get_mac_addr(adapter, 0, adapter->mac_addr) != 0) {
		printk("%s MAC address = %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x \n", __FUNCTION__, adapter->mac_addr[0],
		       adapter->mac_addr[1], adapter->mac_addr[2], adapter->mac_addr[3], adapter->mac_addr[4], adapter->mac_addr[5],
		       adapter->mac_addr[6], adapter->mac_addr[7]
		    );

		ca_set_mac_dst_addr_filter(adapter, adapter->mac_addr);
		ctrl_enable_mac(adapter, 1);
	}

	adapter->lock = SPIN_LOCK_UNLOCKED;

	return 0;
}

static void driver_halt(struct pci_dev *pdev)
{
	struct adapter *adapter;

	adapter = pci_get_drvdata(pdev);

	irq_dma_enable_disable_irq(adapter, 0);

	ctrl_enable_receive_data(adapter, 0);

	free_adapter_object(adapter);

	pci_set_drvdata(pdev, NULL);

	release_region(pci_resource_start(pdev, 1), pci_resource_len(pdev, 1));

	release_mem_region(pci_resource_start(pdev, 0), pci_resource_len(pdev, 0));
}

static int dvb_start_feed(struct dvb_demux_feed *dvbdmxfeed)
{
	struct dvb_demux *dvbdmx = dvbdmxfeed->demux;
	struct adapter *adapter = (struct adapter *) dvbdmx->priv;

	dprintk("%s: PID=%d, type=%d\n", __FUNCTION__, dvbdmxfeed->pid, dvbdmxfeed->type);

	open_stream(adapter, dvbdmxfeed->pid);

	return 0;
}

static int dvb_stop_feed(struct dvb_demux_feed *dvbdmxfeed)
{
	struct dvb_demux *dvbdmx = dvbdmxfeed->demux;
	struct adapter *adapter = (struct adapter *) dvbdmx->priv;

	dprintk("%s: PID=%d, type=%d\n", __FUNCTION__, dvbdmxfeed->pid, dvbdmxfeed->type);

	close_stream(adapter, dvbdmxfeed->pid);

	return 0;
}

/* lnb control */
static void set_tuner_tone(struct adapter *adapter, u8 tone)
{
	u16 wz_half_period_for_45_mhz[] = { 0x01ff, 0x0154, 0x00ff, 0x00cc };
	u16 ax;

	dprintk("%s: %u\n", __FUNCTION__, tone);

	switch (tone) {
	case 1:
		ax = wz_half_period_for_45_mhz[0];
		break;
	case 2:
		ax = wz_half_period_for_45_mhz[1];
		break;
	case 3:
		ax = wz_half_period_for_45_mhz[2];
		break;
	case 4:
		ax = wz_half_period_for_45_mhz[3];
		break;

	default:
		ax = 0;
	}

	if (ax != 0) {
		write_reg_dw(adapter, 0x200, ((ax << 0x0f) + (ax & 0x7fff)) | 0x40000000);

	} else {

		write_reg_dw(adapter, 0x200, 0x40ff8000);
	}
}

static void set_tuner_polarity(struct adapter *adapter, u8 polarity)
{
	u32 var;

	dprintk("%s : polarity = %u \n", __FUNCTION__, polarity);

	var = read_reg_dw(adapter, 0x204);

	if (polarity == 0) {
		dprintk("%s: LNB power off\n", __FUNCTION__);
		var = var | 1;
	};

	if (polarity == 1) {
		var = var & ~1;
		var = var & ~4;
	};

	if (polarity == 2) {
		var = var & ~1;
		var = var | 4;
	}

	write_reg_dw(adapter, 0x204, var);
}

static void diseqc_send_bit(struct adapter *adapter, int data)
{
	set_tuner_tone(adapter, 1);
	udelay(data ? 500 : 1000);
	set_tuner_tone(adapter, 0);
	udelay(data ? 1000 : 500);
}


static void diseqc_send_byte(struct adapter *adapter, int data)
		{
	int i, par = 1, d;

	for (i = 7; i >= 0; i--) {
		d = (data >> i) & 1;
		par ^= d;
		diseqc_send_bit(adapter, d);
	}

	diseqc_send_bit(adapter, par);
		}


static int send_diseqc_msg(struct adapter *adapter, int len, u8 *msg, unsigned long burst)
{
	int i;

	set_tuner_tone(adapter, 0);
	mdelay(16);

	for (i = 0; i < len; i++)
		diseqc_send_byte(adapter, msg[i]);

	mdelay(16);

	if (burst != -1) {
		if (burst)
			diseqc_send_byte(adapter, 0xff);
		else {
			set_tuner_tone(adapter, 1);
			udelay(12500);
			set_tuner_tone(adapter, 0);
		}
		dvb_delay(20);
	}

	return 0;
}


int soft_diseqc(struct adapter *adapter, unsigned int cmd, void *arg)
{
	switch (cmd) {
	case FE_SET_TONE:
		switch ((fe_sec_tone_mode_t) arg) {
		case SEC_TONE_ON:
			set_tuner_tone(adapter, 1);
			break;
		case SEC_TONE_OFF:
			set_tuner_tone(adapter, 0);
				break;
			default:
				return -EINVAL;
			};
		break;

	case FE_DISEQC_SEND_MASTER_CMD:
		{
			struct dvb_diseqc_master_cmd *cmd = arg;

			send_diseqc_msg(adapter, cmd->msg_len, cmd->msg, 0);
			break;
		}

	case FE_DISEQC_SEND_BURST:
		send_diseqc_msg(adapter, 0, NULL, (unsigned long) arg);
		break;

	default:
		return -EOPNOTSUPP;
	};

	return 0;
}

static int flexcop_diseqc_ioctl(struct dvb_frontend *fe, unsigned int cmd, void *arg)
		{
	struct adapter *adapter = fe->before_after_data;

	struct dvb_frontend_info info;

	fe->ioctl(fe, FE_GET_INFO, &info);

	// we must use different DiSEqC hw

	if (strcmp(info.name, "Zarlink MT312") == 0) {
		//VP310 using mt312 driver for tuning only: diseqc not wired
		//use FCII instead
		if (!soft_diseqc(adapter, cmd, arg))
			return 0;
	}

	switch (cmd) {
	case FE_SLEEP:
		{
			dprintk("%s: FE_SLEEP\n", __FUNCTION__);

			set_tuner_polarity(adapter, 0);

			// return -EOPNOTSUPP, to make DVB core also send "FE_SLEEP" command to frontend.
			return -EOPNOTSUPP;
		}

	case FE_SET_VOLTAGE:
		{
			dprintk("%s: FE_SET_VOLTAGE\n", __FUNCTION__);

			switch ((fe_sec_voltage_t) arg) {
			case SEC_VOLTAGE_13:

				dprintk("%s: SEC_VOLTAGE_13, %x\n", __FUNCTION__, SEC_VOLTAGE_13);

				set_tuner_polarity(adapter, 1);

				return 0;

			case SEC_VOLTAGE_18:

				dprintk("%s: SEC_VOLTAGE_18, %x\n", __FUNCTION__, SEC_VOLTAGE_18);

				set_tuner_polarity(adapter, 2);

				return 0;

			default:

				return -EINVAL;
			};
		}


	default:

		return -EOPNOTSUPP;
	};

	return 0;
}

static int skystar2_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct adapter *adapter;
	struct dvb_adapter *dvb_adapter;
	struct dvb_demux *dvbdemux;

	int ret;

	if (pdev == NULL)
		return -ENODEV;

	if (driver_initialize(pdev) != 0)
		return -ENODEV;

	dvb_register_adapter(&dvb_adapter, skystar2_pci_driver.name, THIS_MODULE);

	if (dvb_adapter == NULL) {
		printk("%s: Error registering DVB adapter\n", __FUNCTION__);

		driver_halt(pdev);

		return -ENODEV;
	}

	adapter = (struct adapter *) pci_get_drvdata(pdev);

	adapter->dvb_adapter = dvb_adapter;

	init_MUTEX(&adapter->i2c_sem);

	adapter->i2c_bus = dvb_register_i2c_bus(master_xfer, adapter, adapter->dvb_adapter, 0);

	if (!adapter->i2c_bus)
		return -ENOMEM;

	dvb_add_frontend_ioctls(adapter->dvb_adapter, flexcop_diseqc_ioctl, NULL, adapter);

	dvbdemux = &adapter->demux;

	dvbdemux->priv = (void *) adapter;
	dvbdemux->filternum = N_PID_SLOTS;
	dvbdemux->feednum = N_PID_SLOTS;
	dvbdemux->start_feed = dvb_start_feed;
	dvbdemux->stop_feed = dvb_stop_feed;
	dvbdemux->write_to_decoder = NULL;
	dvbdemux->dmx.capabilities = (DMX_TS_FILTERING | DMX_SECTION_FILTERING | DMX_MEMORY_BASED_FILTERING);

	dvb_dmx_init(&adapter->demux);

	adapter->hw_frontend.source = DMX_FRONTEND_0;

	adapter->dmxdev.filternum = N_PID_SLOTS;
	adapter->dmxdev.demux = &dvbdemux->dmx;
	adapter->dmxdev.capabilities = 0;

	dvb_dmxdev_init(&adapter->dmxdev, adapter->dvb_adapter);

	ret = dvbdemux->dmx.add_frontend(&dvbdemux->dmx, &adapter->hw_frontend);
	if (ret < 0)
		return ret;

	adapter->mem_frontend.source = DMX_MEMORY_FE;

	ret = dvbdemux->dmx.add_frontend(&dvbdemux->dmx, &adapter->mem_frontend);
	if (ret < 0)
		return ret;

	ret = dvbdemux->dmx.connect_frontend(&dvbdemux->dmx, &adapter->hw_frontend);
	if (ret < 0)
		return ret;

	dvb_net_init(adapter->dvb_adapter, &adapter->dvbnet, &dvbdemux->dmx);
	return 0;
}

static void skystar2_remove(struct pci_dev *pdev)
{
	struct adapter *adapter;
	struct dvb_demux *dvbdemux;

	if (pdev == NULL)
		return;

	adapter = pci_get_drvdata(pdev);

	if (adapter != NULL) {
		dvb_net_release(&adapter->dvbnet);
		dvbdemux = &adapter->demux;

		dvbdemux->dmx.close(&dvbdemux->dmx);
		dvbdemux->dmx.remove_frontend(&dvbdemux->dmx, &adapter->hw_frontend);
		dvbdemux->dmx.remove_frontend(&dvbdemux->dmx, &adapter->mem_frontend);

		dvb_dmxdev_release(&adapter->dmxdev);
		dvb_dmx_release(&adapter->demux);

		if (adapter->dvb_adapter != NULL) {
			dvb_remove_frontend_ioctls(adapter->dvb_adapter, flexcop_diseqc_ioctl, NULL);

			if (adapter->i2c_bus != NULL)
				dvb_unregister_i2c_bus(master_xfer, adapter->i2c_bus->adapter, adapter->i2c_bus->id);

			dvb_unregister_adapter(adapter->dvb_adapter);
		}
		driver_halt(pdev);
	}
}

static struct pci_device_id skystar2_pci_tbl[] = {
	{0x000013d0, 0x00002103, 0xffffffff, 0xffffffff, 0x00000000, 0x00000000, 0x00000000},
	{0x000013d0, 0x00002200, 0xffffffff, 0xffffffff, 0x00000000, 0x00000000, 0x00000000},	//FCIII
	{0,},
};

MODULE_DEVICE_TABLE(pci, skystar2_pci_tbl);

static struct pci_driver skystar2_pci_driver = {
	.name = "Technisat SkyStar2 driver",
	.id_table = skystar2_pci_tbl,
	.probe = skystar2_probe,
	.remove = skystar2_remove,
};

static int skystar2_init(void)
{
	return pci_module_init(&skystar2_pci_driver);
}

static void skystar2_cleanup(void)
{
	pci_unregister_driver(&skystar2_pci_driver);
}

module_init(skystar2_init);
module_exit(skystar2_cleanup);

MODULE_PARM(debug, "i");
MODULE_PARM_DESC(debug, "enable verbose debug messages: supported values: 1 and 2");
MODULE_PARM(enable_hw_filters, "i");
MODULE_PARM_DESC(enable_hw_filters, "enable hardware filters: supported values: 0 (none), 1, 2");

MODULE_DESCRIPTION("Technisat SkyStar2 DVB PCI Driver");
MODULE_LICENSE("GPL");
