/*
 *  linux/drivers/acorn/char/pcf8583.c
 *
 *  Copyright (C) 2000 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Driver for PCF8583 RTC & RAM chip
 */
#include <linux/i2c.h>
#include <linux/malloc.h>
#include <linux/string.h>
#include <linux/mc146818rtc.h>
#include <linux/init.h>

#include "pcf8583.h"

static struct i2c_driver pcf8583_driver;

static unsigned short ignore[] = { I2C_CLIENT_END };
static unsigned short normal_addr[] = { 0x50, 0x51, I2C_CLIENT_END };

static struct i2c_client_address_data addr_data = {
	force:			ignore,
	ignore:			ignore,
	ignore_range:		ignore,
	normal_i2c:		ignore,
	normal_i2c_range:	normal_addr,
	probe:			ignore,
	probe_range:		ignore
};

#define DAT(x) ((unsigned int)(x->data))

static int
pcf8583_attach(struct i2c_adapter *adap, int addr, unsigned short flags,
	       int kind)
{
	struct i2c_client *c;
	unsigned char buf[1], ad[1] = { 0 };
	struct i2c_msg msgs[2] = {
		{ addr, 0,        1, ad  },
		{ addr, I2C_M_RD, 1, buf }
	};

	c = kmalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	strcpy(c->name, "PCF8583");
	c->id		= pcf8583_driver.id;
	c->flags	= 0;
	c->addr		= addr;
	c->adapter	= adap;
	c->driver	= &pcf8583_driver;
	c->data		= NULL;

	if (i2c_transfer(c->adapter, msgs, 2) == 2)
		DAT(c) = buf[0];

	return i2c_attach_client(c);
}

static int
pcf8583_probe(struct i2c_adapter *adap)
{
	return i2c_probe(adap, &addr_data, pcf8583_attach);
}

static int
pcf8583_detach(struct i2c_client *client)
{
	i2c_detach_client(client);
	return 0;
}

static int
pcf8583_get_datetime(struct i2c_client *client, struct rtc_tm *dt)
{
	unsigned char buf[8], addr[1] = { 1 };
	struct i2c_msg msgs[2] = {
		{ client->addr, 0,        1, addr },
		{ client->addr, I2C_M_RD, 6, buf  }
	};
	int ret = -EIO;

	if (i2c_transfer(client->adapter, msgs, 2) == 2) {
		dt->year_off = buf[4] >> 6;
		dt->wday     = buf[5] >> 5;

		buf[4] &= 0x3f;
		buf[5] &= 0x1f;

		dt->cs       = BCD_TO_BIN(buf[0]);
		dt->secs     = BCD_TO_BIN(buf[1]);
		dt->mins     = BCD_TO_BIN(buf[2]);
		dt->hours    = BCD_TO_BIN(buf[3]);
		dt->mday     = BCD_TO_BIN(buf[4]);
		dt->mon      = BCD_TO_BIN(buf[5]);

		ret = 0;
	}

	return ret;
}

static int
pcf8583_set_datetime(struct i2c_client *client, struct rtc_tm *dt, int datetoo)
{
	unsigned char buf[8];
	int ret, len = 6;

	buf[0] = 0;
	buf[1] = DAT(client) | 0x80;
	buf[2] = BIN_TO_BCD(dt->cs);
	buf[3] = BIN_TO_BCD(dt->secs);
	buf[4] = BIN_TO_BCD(dt->mins);
	buf[5] = BIN_TO_BCD(dt->hours);

	if (datetoo) {
		len = 8;
		buf[6] = BIN_TO_BCD(dt->mday) | (dt->year_off << 6);
		buf[7] = BIN_TO_BCD(dt->mon)  | (dt->wday << 5);
	}

	ret = i2c_master_send(client, (char *)buf, len);

	buf[1] = DAT(client);
	i2c_master_send(client, (char *)buf, 2);

	return ret;
}

static int
pcf8583_get_ctrl(struct i2c_client *client, unsigned char *ctrl)
{
	*ctrl = DAT(client);
	return 0;
}

static int
pcf8583_set_ctrl(struct i2c_client *client, unsigned char *ctrl)
{
	unsigned char buf[2];

	buf[0] = 0;
	buf[1] = *ctrl;
	DAT(client) = *ctrl;

	return i2c_master_send(client, (char *)buf, 2);
}

static int
pcf8583_read_mem(struct i2c_client *client, struct mem *mem)
{
	unsigned char addr[1];
	struct i2c_msg msgs[2] = {
		{ client->addr, 0,        1, addr },
		{ client->addr, I2C_M_RD, 0, mem->data }
	};

	if (mem->loc < 8)
		return -EINVAL;

	addr[0] = mem->loc;
	msgs[1].len = mem->nr;

	return i2c_transfer(client->adapter, msgs, 2) == 2 ? 0 : -EIO;
}

static int
pcf8583_write_mem(struct i2c_client *client, struct mem *mem)
{
	unsigned char addr[1];
	struct i2c_msg msgs[2] = {
		{ client->addr, 0, 1, addr },
		{ client->addr, 0, 0, mem->data }
	};

	if (mem->loc < 8)
		return -EINVAL;

	addr[0] = mem->loc;
	msgs[1].len = mem->nr;

	return i2c_transfer(client->adapter, msgs, 2) == 2 ? 0 : -EIO;
}

static int
pcf8583_command(struct i2c_client *client, unsigned int cmd, void *arg)
{
	switch (cmd) {
	case RTC_GETDATETIME:
		return pcf8583_get_datetime(client, arg);
		
	case RTC_SETTIME:
		return pcf8583_set_datetime(client, arg, 0);

	case RTC_SETDATETIME:
		return pcf8583_set_datetime(client, arg, 1);

	case RTC_GETCTRL:
		return pcf8583_get_ctrl(client, arg);

	case RTC_SETCTRL:
		return pcf8583_set_ctrl(client, arg);

	case MEM_READ:
		return pcf8583_read_mem(client, arg);

	case MEM_WRITE:
		return pcf8583_write_mem(client, arg);

	default:
		return -EINVAL;
	}
}

static struct i2c_driver pcf8583_driver = {
	"PCF8583",
	I2C_DRIVERID_PCF8583,
	I2C_DF_NOTIFY,
	pcf8583_probe,
	pcf8583_detach,
	pcf8583_command
};

static __init int pcf8583_init(void)
{
	return i2c_add_driver(&pcf8583_driver);
}

__initcall(pcf8583_init);
