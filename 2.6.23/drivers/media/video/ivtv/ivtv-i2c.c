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

/*
    This file includes an i2c implementation that was reverse engineered
    from the Hauppauge windows driver.  Older ivtv versions used i2c-algo-bit,
    which whilst fine under most circumstances, had trouble with the Zilog
    CPU on the PVR-150 which handles IR functions (occasional inability to
    communicate with the chip until it was reset) and also with the i2c
    bus being completely unreachable when multiple PVR cards were present.

    The implementation is very similar to i2c-algo-bit, but there are enough
    subtle differences that the two are hard to merge.  The general strategy
    employed by i2c-algo-bit is to use udelay() to implement the timing
    when putting out bits on the scl/sda lines.  The general strategy taken
    here is to poll the lines for state changes (see ivtv_waitscl and
    ivtv_waitsda).  In addition there are small delays at various locations
    which poll the SCL line 5 times (ivtv_scldelay).  I would guess that
    since this is memory mapped I/O that the length of those delays is tied
    to the PCI bus clock.  There is some extra code to do with recovery
    and retries.  Since it is not known what causes the actual i2c problems
    in the first place, the only goal if one was to attempt to use
    i2c-algo-bit would be to try to make it follow the same code path.
    This would be a lot of work, and I'm also not convinced that it would
    provide a generic benefit to i2c-algo-bit.  Therefore consider this
    an engineering solution -- not pretty, but it works.

    Some more general comments about what we are doing:

    The i2c bus is a 2 wire serial bus, with clock (SCL) and data (SDA)
    lines.  To communicate on the bus (as a master, we don't act as a slave),
    we first initiate a start condition (ivtv_start).  We then write the
    address of the device that we want to communicate with, along with a flag
    that indicates whether this is a read or a write.  The slave then issues
    an ACK signal (ivtv_ack), which tells us that it is ready for reading /
    writing.  We then proceed with reading or writing (ivtv_read/ivtv_write),
    and finally issue a stop condition (ivtv_stop) to make the bus available
    to other masters.

    There is an additional form of transaction where a write may be
    immediately followed by a read.  In this case, there is no intervening
    stop condition.  (Only the msp3400 chip uses this method of data transfer).
 */

#include "ivtv-driver.h"
#include "ivtv-cards.h"
#include "ivtv-gpio.h"
#include "ivtv-i2c.h"

#include <media/ir-kbd-i2c.h>

/* i2c implementation for cx23415/6 chip, ivtv project.
 * Author: Kevin Thayer (nufan_wfk at yahoo.com)
 */
/* i2c stuff */
#define IVTV_REG_I2C_SETSCL_OFFSET 0x7000
#define IVTV_REG_I2C_SETSDA_OFFSET 0x7004
#define IVTV_REG_I2C_GETSCL_OFFSET 0x7008
#define IVTV_REG_I2C_GETSDA_OFFSET 0x700c

#ifndef I2C_ADAP_CLASS_TV_ANALOG
#define I2C_ADAP_CLASS_TV_ANALOG I2C_CLASS_TV_ANALOG
#endif /* I2C_ADAP_CLASS_TV_ANALOG */

#define IVTV_CS53L32A_I2C_ADDR		0x11
#define IVTV_CX25840_I2C_ADDR 		0x44
#define IVTV_SAA7115_I2C_ADDR 		0x21
#define IVTV_SAA7127_I2C_ADDR 		0x44
#define IVTV_SAA717x_I2C_ADDR 		0x21
#define IVTV_MSP3400_I2C_ADDR 		0x40
#define IVTV_HAUPPAUGE_I2C_ADDR 	0x50
#define IVTV_WM8739_I2C_ADDR 		0x1a
#define IVTV_WM8775_I2C_ADDR		0x1b
#define IVTV_TEA5767_I2C_ADDR		0x60
#define IVTV_UPD64031A_I2C_ADDR 	0x12
#define IVTV_UPD64083_I2C_ADDR 		0x5c
#define IVTV_TDA985X_I2C_ADDR      	0x5b

/* This array should match the IVTV_HW_ defines */
static const u8 hw_driverids[] = {
	I2C_DRIVERID_CX25840,
	I2C_DRIVERID_SAA711X,
	I2C_DRIVERID_SAA7127,
	I2C_DRIVERID_MSP3400,
	I2C_DRIVERID_TUNER,
	I2C_DRIVERID_WM8775,
	I2C_DRIVERID_CS53L32A,
	I2C_DRIVERID_TVEEPROM,
	I2C_DRIVERID_SAA711X,
	I2C_DRIVERID_TVAUDIO,
	I2C_DRIVERID_UPD64031A,
	I2C_DRIVERID_UPD64083,
	I2C_DRIVERID_SAA717X,
	I2C_DRIVERID_WM8739,
	0 		/* IVTV_HW_GPIO dummy driver ID */
};

/* This array should match the IVTV_HW_ defines */
static const char * const hw_drivernames[] = {
	"cx2584x",
	"saa7115",
	"saa7127",
	"msp3400",
	"tuner",
	"wm8775",
	"cs53l32a",
	"tveeprom",
	"saa7114",
	"tvaudio",
	"upd64031a",
	"upd64083",
	"saa717x",
	"wm8739",
	"gpio",
};

static int attach_inform(struct i2c_client *client)
{
	struct ivtv *itv = (struct ivtv *)i2c_get_adapdata(client->adapter);
	int i;

	IVTV_DEBUG_I2C("i2c client attach\n");
	for (i = 0; i < I2C_CLIENTS_MAX; i++) {
		if (itv->i2c_clients[i] == NULL) {
			itv->i2c_clients[i] = client;
			break;
		}
	}
	if (i == I2C_CLIENTS_MAX) {
		IVTV_ERR("Insufficient room for new I2C client\n");
	}
	return 0;
}

static int detach_inform(struct i2c_client *client)
{
	int i;
	struct ivtv *itv = (struct ivtv *)i2c_get_adapdata(client->adapter);

	IVTV_DEBUG_I2C("i2c client detach\n");
	for (i = 0; i < I2C_CLIENTS_MAX; i++) {
		if (itv->i2c_clients[i] == client) {
			itv->i2c_clients[i] = NULL;
			break;
		}
	}
	IVTV_DEBUG_I2C("i2c detach [client=%s,%s]\n",
		   client->name, (i < I2C_CLIENTS_MAX) ? "ok" : "failed");

	return 0;
}

/* Set the serial clock line to the desired state */
static void ivtv_setscl(struct ivtv *itv, int state)
{
	/* write them out */
	/* write bits are inverted */
	write_reg(~state, IVTV_REG_I2C_SETSCL_OFFSET);
}

/* Set the serial data line to the desired state */
static void ivtv_setsda(struct ivtv *itv, int state)
{
	/* write them out */
	/* write bits are inverted */
	write_reg(~state & 1, IVTV_REG_I2C_SETSDA_OFFSET);
}

/* Read the serial clock line */
static int ivtv_getscl(struct ivtv *itv)
{
	return read_reg(IVTV_REG_I2C_GETSCL_OFFSET) & 1;
}

/* Read the serial data line */
static int ivtv_getsda(struct ivtv *itv)
{
	return read_reg(IVTV_REG_I2C_GETSDA_OFFSET) & 1;
}

/* Implement a short delay by polling the serial clock line */
static void ivtv_scldelay(struct ivtv *itv)
{
	int i;

	for (i = 0; i < 5; ++i)
		ivtv_getscl(itv);
}

/* Wait for the serial clock line to become set to a specific value */
static int ivtv_waitscl(struct ivtv *itv, int val)
{
	int i;

	ivtv_scldelay(itv);
	for (i = 0; i < 1000; ++i) {
		if (ivtv_getscl(itv) == val)
			return 1;
	}
	return 0;
}

/* Wait for the serial data line to become set to a specific value */
static int ivtv_waitsda(struct ivtv *itv, int val)
{
	int i;

	ivtv_scldelay(itv);
	for (i = 0; i < 1000; ++i) {
		if (ivtv_getsda(itv) == val)
			return 1;
	}
	return 0;
}

/* Wait for the slave to issue an ACK */
static int ivtv_ack(struct ivtv *itv)
{
	int ret = 0;

	if (ivtv_getscl(itv) == 1) {
		IVTV_DEBUG_HI_I2C("SCL was high starting an ack\n");
		ivtv_setscl(itv, 0);
		if (!ivtv_waitscl(itv, 0)) {
			IVTV_DEBUG_I2C("Could not set SCL low starting an ack\n");
			return -EREMOTEIO;
		}
	}
	ivtv_setsda(itv, 1);
	ivtv_scldelay(itv);
	ivtv_setscl(itv, 1);
	if (!ivtv_waitsda(itv, 0)) {
		IVTV_DEBUG_I2C("Slave did not ack\n");
		ret = -EREMOTEIO;
	}
	ivtv_setscl(itv, 0);
	if (!ivtv_waitscl(itv, 0)) {
		IVTV_DEBUG_I2C("Failed to set SCL low after ACK\n");
		ret = -EREMOTEIO;
	}
	return ret;
}

/* Write a single byte to the i2c bus and wait for the slave to ACK */
static int ivtv_sendbyte(struct ivtv *itv, unsigned char byte)
{
	int i, bit;

	IVTV_DEBUG_HI_I2C("write %x\n",byte);
	for (i = 0; i < 8; ++i, byte<<=1) {
		ivtv_setscl(itv, 0);
		if (!ivtv_waitscl(itv, 0)) {
			IVTV_DEBUG_I2C("Error setting SCL low\n");
			return -EREMOTEIO;
		}
		bit = (byte>>7)&1;
		ivtv_setsda(itv, bit);
		if (!ivtv_waitsda(itv, bit)) {
			IVTV_DEBUG_I2C("Error setting SDA\n");
			return -EREMOTEIO;
		}
		ivtv_setscl(itv, 1);
		if (!ivtv_waitscl(itv, 1)) {
			IVTV_DEBUG_I2C("Slave not ready for bit\n");
			return -EREMOTEIO;
		}
	}
	ivtv_setscl(itv, 0);
	if (!ivtv_waitscl(itv, 0)) {
		IVTV_DEBUG_I2C("Error setting SCL low\n");
		return -EREMOTEIO;
	}
	return ivtv_ack(itv);
}

/* Read a byte from the i2c bus and send a NACK if applicable (i.e. for the
   final byte) */
static int ivtv_readbyte(struct ivtv *itv, unsigned char *byte, int nack)
{
	int i;

	*byte = 0;

	ivtv_setsda(itv, 1);
	ivtv_scldelay(itv);
	for (i = 0; i < 8; ++i) {
		ivtv_setscl(itv, 0);
		ivtv_scldelay(itv);
		ivtv_setscl(itv, 1);
		if (!ivtv_waitscl(itv, 1)) {
			IVTV_DEBUG_I2C("Error setting SCL high\n");
			return -EREMOTEIO;
		}
		*byte = ((*byte)<<1)|ivtv_getsda(itv);
	}
	ivtv_setscl(itv, 0);
	ivtv_scldelay(itv);
	ivtv_setsda(itv, nack);
	ivtv_scldelay(itv);
	ivtv_setscl(itv, 1);
	ivtv_scldelay(itv);
	ivtv_setscl(itv, 0);
	ivtv_scldelay(itv);
	IVTV_DEBUG_HI_I2C("read %x\n",*byte);
	return 0;
}

/* Issue a start condition on the i2c bus to alert slaves to prepare for
   an address write */
static int ivtv_start(struct ivtv *itv)
{
	int sda;

	sda = ivtv_getsda(itv);
	if (sda != 1) {
		IVTV_DEBUG_HI_I2C("SDA was low at start\n");
		ivtv_setsda(itv, 1);
		if (!ivtv_waitsda(itv, 1)) {
			IVTV_DEBUG_I2C("SDA stuck low\n");
			return -EREMOTEIO;
		}
	}
	if (ivtv_getscl(itv) != 1) {
		ivtv_setscl(itv, 1);
		if (!ivtv_waitscl(itv, 1)) {
			IVTV_DEBUG_I2C("SCL stuck low at start\n");
			return -EREMOTEIO;
		}
	}
	ivtv_setsda(itv, 0);
	ivtv_scldelay(itv);
	return 0;
}

/* Issue a stop condition on the i2c bus to release it */
static int ivtv_stop(struct ivtv *itv)
{
	int i;

	if (ivtv_getscl(itv) != 0) {
		IVTV_DEBUG_HI_I2C("SCL not low when stopping\n");
		ivtv_setscl(itv, 0);
		if (!ivtv_waitscl(itv, 0)) {
			IVTV_DEBUG_I2C("SCL could not be set low\n");
		}
	}
	ivtv_setsda(itv, 0);
	ivtv_scldelay(itv);
	ivtv_setscl(itv, 1);
	if (!ivtv_waitscl(itv, 1)) {
		IVTV_DEBUG_I2C("SCL could not be set high\n");
		return -EREMOTEIO;
	}
	ivtv_scldelay(itv);
	ivtv_setsda(itv, 1);
	if (!ivtv_waitsda(itv, 1)) {
		IVTV_DEBUG_I2C("resetting I2C\n");
		for (i = 0; i < 16; ++i) {
			ivtv_setscl(itv, 0);
			ivtv_scldelay(itv);
			ivtv_setscl(itv, 1);
			ivtv_scldelay(itv);
			ivtv_setsda(itv, 1);
		}
		ivtv_waitsda(itv, 1);
		return -EREMOTEIO;
	}
	return 0;
}

/* Write a message to the given i2c slave.  do_stop may be 0 to prevent
   issuing the i2c stop condition (when following with a read) */
static int ivtv_write(struct ivtv *itv, unsigned char addr, unsigned char *data, u32 len, int do_stop)
{
	int retry, ret = -EREMOTEIO;
	u32 i;

	for (retry = 0; ret != 0 && retry < 8; ++retry) {
		ret = ivtv_start(itv);

		if (ret == 0) {
			ret = ivtv_sendbyte(itv, addr<<1);
			for (i = 0; ret == 0 && i < len; ++i)
				ret = ivtv_sendbyte(itv, data[i]);
		}
		if (ret != 0 || do_stop) {
			ivtv_stop(itv);
		}
	}
	if (ret)
		IVTV_DEBUG_I2C("i2c write to %x failed\n", addr);
	return ret;
}

/* Read data from the given i2c slave.  A stop condition is always issued. */
static int ivtv_read(struct ivtv *itv, unsigned char addr, unsigned char *data, u32 len)
{
	int retry, ret = -EREMOTEIO;
	u32 i;

	for (retry = 0; ret != 0 && retry < 8; ++retry) {
		ret = ivtv_start(itv);
		if (ret == 0)
			ret = ivtv_sendbyte(itv, (addr << 1) | 1);
		for (i = 0; ret == 0 && i < len; ++i) {
			ret = ivtv_readbyte(itv, &data[i], i == len - 1);
		}
		ivtv_stop(itv);
	}
	if (ret)
		IVTV_DEBUG_I2C("i2c read from %x failed\n", addr);
	return ret;
}

/* Kernel i2c transfer implementation.  Takes a number of messages to be read
   or written.  If a read follows a write, this will occur without an
   intervening stop condition */
static int ivtv_xfer(struct i2c_adapter *i2c_adap, struct i2c_msg *msgs, int num)
{
	struct ivtv *itv = i2c_get_adapdata(i2c_adap);
	int retval;
	int i;

	mutex_lock(&itv->i2c_bus_lock);
	for (i = retval = 0; retval == 0 && i < num; i++) {
		if (msgs[i].flags & I2C_M_RD)
			retval = ivtv_read(itv, msgs[i].addr, msgs[i].buf, msgs[i].len);
		else {
			/* if followed by a read, don't stop */
			int stop = !(i + 1 < num && msgs[i + 1].flags == I2C_M_RD);

			retval = ivtv_write(itv, msgs[i].addr, msgs[i].buf, msgs[i].len, stop);
		}
	}
	mutex_unlock(&itv->i2c_bus_lock);
	return retval ? retval : num;
}

/* Kernel i2c capabilities */
static u32 ivtv_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static struct i2c_algorithm ivtv_algo = {
	.master_xfer   = ivtv_xfer,
	.functionality = ivtv_functionality,
};

/* template for our-bit banger */
static struct i2c_adapter ivtv_i2c_adap_hw_template = {
	.name = "ivtv i2c driver",
	.id = I2C_HW_B_CX2341X,
	.algo = &ivtv_algo,
	.algo_data = NULL,			/* filled from template */
	.client_register = attach_inform,
	.client_unregister = detach_inform,
	.owner = THIS_MODULE,
#ifdef I2C_ADAP_CLASS_TV_ANALOG
	.class = I2C_ADAP_CLASS_TV_ANALOG,
#endif
};

static void ivtv_setscl_old(void *data, int state)
{
	struct ivtv *itv = (struct ivtv *)data;

	if (state)
		itv->i2c_state |= 0x01;
	else
		itv->i2c_state &= ~0x01;

	/* write them out */
	/* write bits are inverted */
	write_reg(~itv->i2c_state, IVTV_REG_I2C_SETSCL_OFFSET);
}

static void ivtv_setsda_old(void *data, int state)
{
	struct ivtv *itv = (struct ivtv *)data;

	if (state)
		itv->i2c_state |= 0x01;
	else
		itv->i2c_state &= ~0x01;

	/* write them out */
	/* write bits are inverted */
	write_reg(~itv->i2c_state, IVTV_REG_I2C_SETSDA_OFFSET);
}

static int ivtv_getscl_old(void *data)
{
	struct ivtv *itv = (struct ivtv *)data;

	return read_reg(IVTV_REG_I2C_GETSCL_OFFSET) & 1;
}

static int ivtv_getsda_old(void *data)
{
	struct ivtv *itv = (struct ivtv *)data;

	return read_reg(IVTV_REG_I2C_GETSDA_OFFSET) & 1;
}

/* template for i2c-bit-algo */
static struct i2c_adapter ivtv_i2c_adap_template = {
	.name = "ivtv i2c driver",
	.id = I2C_HW_B_CX2341X,  	/* algo-bit is OR'd with this */
	.algo = NULL,                   /* set by i2c-algo-bit */
	.algo_data = NULL,              /* filled from template */
	.client_register = attach_inform,
	.client_unregister = detach_inform,
	.owner = THIS_MODULE,
#ifdef I2C_ADAP_CLASS_TV_ANALOG
	.class = I2C_ADAP_CLASS_TV_ANALOG,
#endif
};

static struct i2c_algo_bit_data ivtv_i2c_algo_template = {
	NULL,                   /* ?? */
	ivtv_setsda_old,        /* setsda function */
	ivtv_setscl_old,        /* " */
	ivtv_getsda_old,        /* " */
	ivtv_getscl_old,        /* " */
	10,                     /* udelay */
	200                     /* timeout */
};

static struct i2c_client ivtv_i2c_client_template = {
	.name = "ivtv internal",
};

int ivtv_call_i2c_client(struct ivtv *itv, int addr, unsigned int cmd, void *arg)
{
	struct i2c_client *client;
	int retval;
	int i;

	IVTV_DEBUG_I2C("call_i2c_client addr=%02x\n", addr);
	for (i = 0; i < I2C_CLIENTS_MAX; i++) {
		client = itv->i2c_clients[i];
		if (client == NULL) {
			continue;
		}
		if (client->driver->command == NULL) {
			continue;
		}
		if (addr == client->addr) {
			retval = client->driver->command(client, cmd, arg);
			return retval;
		}
	}
	if (cmd != VIDIOC_G_CHIP_IDENT)
		IVTV_ERR("i2c addr 0x%02x not found for command 0x%x\n", addr, cmd);
	return -ENODEV;
}

/* Find the i2c device based on the driver ID and return
   its i2c address or -ENODEV if no matching device was found. */
static int ivtv_i2c_id_addr(struct ivtv *itv, u32 id)
{
	struct i2c_client *client;
	int retval = -ENODEV;
	int i;

	for (i = 0; i < I2C_CLIENTS_MAX; i++) {
		client = itv->i2c_clients[i];
		if (client == NULL)
			continue;
		if (id == client->driver->id) {
			retval = client->addr;
			break;
		}
	}
	return retval;
}

/* Find the i2c device name matching the DRIVERID */
static const char *ivtv_i2c_id_name(u32 id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(hw_driverids); i++)
		if (hw_driverids[i] == id)
			return hw_drivernames[i];
	return "unknown device";
}

/* Find the i2c device name matching the IVTV_HW_ flag */
static const char *ivtv_i2c_hw_name(u32 hw)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(hw_driverids); i++)
		if (1 << i == hw)
			return hw_drivernames[i];
	return "unknown device";
}

/* Find the i2c device matching the IVTV_HW_ flag and return
   its i2c address or -ENODEV if no matching device was found. */
int ivtv_i2c_hw_addr(struct ivtv *itv, u32 hw)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(hw_driverids); i++)
		if (1 << i == hw)
			return ivtv_i2c_id_addr(itv, hw_driverids[i]);
	return -ENODEV;
}

/* Calls i2c device based on IVTV_HW_ flag. If hw == 0, then do nothing.
   If hw == IVTV_HW_GPIO then call the gpio handler. */
int ivtv_i2c_hw(struct ivtv *itv, u32 hw, unsigned int cmd, void *arg)
{
	int addr;

	if (hw == IVTV_HW_GPIO)
		return ivtv_gpio(itv, cmd, arg);
	if (hw == 0)
		return 0;

	addr = ivtv_i2c_hw_addr(itv, hw);
	if (addr < 0) {
		IVTV_ERR("i2c hardware 0x%08x (%s) not found for command 0x%x\n",
			       hw, ivtv_i2c_hw_name(hw), cmd);
		return addr;
	}
	return ivtv_call_i2c_client(itv, addr, cmd, arg);
}

/* Calls i2c device based on I2C driver ID. */
int ivtv_i2c_id(struct ivtv *itv, u32 id, unsigned int cmd, void *arg)
{
	int addr;

	addr = ivtv_i2c_id_addr(itv, id);
	if (addr < 0) {
		if (cmd != VIDIOC_G_CHIP_IDENT)
			IVTV_ERR("i2c ID 0x%08x (%s) not found for command 0x%x\n",
				id, ivtv_i2c_id_name(id), cmd);
		return addr;
	}
	return ivtv_call_i2c_client(itv, addr, cmd, arg);
}

int ivtv_cx25840(struct ivtv *itv, unsigned int cmd, void *arg)
{
	return ivtv_call_i2c_client(itv, IVTV_CX25840_I2C_ADDR, cmd, arg);
}

int ivtv_saa7115(struct ivtv *itv, unsigned int cmd, void *arg)
{
	return ivtv_call_i2c_client(itv, IVTV_SAA7115_I2C_ADDR, cmd, arg);
}

int ivtv_saa7127(struct ivtv *itv, unsigned int cmd, void *arg)
{
	return ivtv_call_i2c_client(itv, IVTV_SAA7127_I2C_ADDR, cmd, arg);
}

int ivtv_saa717x(struct ivtv *itv, unsigned int cmd, void *arg)
{
	return ivtv_call_i2c_client(itv, IVTV_SAA717x_I2C_ADDR, cmd, arg);
}

int ivtv_upd64031a(struct ivtv *itv, unsigned int cmd, void *arg)
{
	return ivtv_call_i2c_client(itv, IVTV_UPD64031A_I2C_ADDR, cmd, arg);
}

int ivtv_upd64083(struct ivtv *itv, unsigned int cmd, void *arg)
{
	return ivtv_call_i2c_client(itv, IVTV_UPD64083_I2C_ADDR, cmd, arg);
}

/* broadcast cmd for all I2C clients and for the gpio subsystem */
void ivtv_call_i2c_clients(struct ivtv *itv, unsigned int cmd, void *arg)
{
	if (itv->i2c_adap.algo == NULL) {
		IVTV_ERR("Adapter is not set");
		return;
	}
	i2c_clients_command(&itv->i2c_adap, cmd, arg);
	if (itv->hw_flags & IVTV_HW_GPIO)
		ivtv_gpio(itv, cmd, arg);
}

/* init + register i2c algo-bit adapter */
int __devinit init_ivtv_i2c(struct ivtv *itv)
{
	IVTV_DEBUG_I2C("i2c init\n");

	if (itv->options.newi2c > 0) {
		memcpy(&itv->i2c_adap, &ivtv_i2c_adap_hw_template,
		       sizeof(struct i2c_adapter));
	} else {
		memcpy(&itv->i2c_adap, &ivtv_i2c_adap_template,
		       sizeof(struct i2c_adapter));
		memcpy(&itv->i2c_algo, &ivtv_i2c_algo_template,
		       sizeof(struct i2c_algo_bit_data));
		itv->i2c_algo.data = itv;
		itv->i2c_adap.algo_data = &itv->i2c_algo;
	}

	sprintf(itv->i2c_adap.name + strlen(itv->i2c_adap.name), " #%d",
		itv->num);
	i2c_set_adapdata(&itv->i2c_adap, itv);

	memcpy(&itv->i2c_client, &ivtv_i2c_client_template,
	       sizeof(struct i2c_client));
	itv->i2c_client.adapter = &itv->i2c_adap;
	itv->i2c_adap.dev.parent = &itv->dev->dev;

	IVTV_DEBUG_I2C("setting scl and sda to 1\n");
	ivtv_setscl(itv, 1);
	ivtv_setsda(itv, 1);

	if (itv->options.newi2c > 0)
		return i2c_add_adapter(&itv->i2c_adap);
	else
		return i2c_bit_add_bus(&itv->i2c_adap);
}

void __devexit exit_ivtv_i2c(struct ivtv *itv)
{
	IVTV_DEBUG_I2C("i2c exit\n");

	i2c_del_adapter(&itv->i2c_adap);
}
