/*
 * An i2c driver for the Xicor/Intersil X1205 RTC
 * Copyright 2004 Karen Spearel
 * Copyright 2005 Alessandro Zummo
 *
 * please send all reports to:
 * 	Karen Spearel <kas111 at gmail dot com>
 *	Alessandro Zummo <a.zummo@towertech.it>
 *
 * based on a lot of other RTC drivers.
 *
 * Information and datasheet:
 * http://www.intersil.com/cda/deviceinfo/0,1477,X1205,00.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/i2c.h>
#include <linux/bcd.h>
#include <linux/rtc.h>
#include <linux/delay.h>

#define DRV_VERSION "1.0.7"

/* Addresses to scan: none. This chip is located at
 * 0x6f and uses a two bytes register addressing.
 * Two bytes need to be written to read a single register,
 * while most other chips just require one and take the second
 * one as the data to be written. To prevent corrupting
 * unknown chips, the user must explicitly set the probe parameter.
 */

static unsigned short normal_i2c[] = { I2C_CLIENT_END };

/* Insmod parameters */
I2C_CLIENT_INSMOD;

/* offsets into CCR area */

#define CCR_SEC			0
#define CCR_MIN			1
#define CCR_HOUR		2
#define CCR_MDAY		3
#define CCR_MONTH		4
#define CCR_YEAR		5
#define CCR_WDAY		6
#define CCR_Y2K			7

#define X1205_REG_SR		0x3F	/* status register */
#define X1205_REG_Y2K		0x37
#define X1205_REG_DW		0x36
#define X1205_REG_YR		0x35
#define X1205_REG_MO		0x34
#define X1205_REG_DT		0x33
#define X1205_REG_HR		0x32
#define X1205_REG_MN		0x31
#define X1205_REG_SC		0x30
#define X1205_REG_DTR		0x13
#define X1205_REG_ATR		0x12
#define X1205_REG_INT		0x11
#define X1205_REG_0		0x10
#define X1205_REG_Y2K1		0x0F
#define X1205_REG_DWA1		0x0E
#define X1205_REG_YRA1		0x0D
#define X1205_REG_MOA1		0x0C
#define X1205_REG_DTA1		0x0B
#define X1205_REG_HRA1		0x0A
#define X1205_REG_MNA1		0x09
#define X1205_REG_SCA1		0x08
#define X1205_REG_Y2K0		0x07
#define X1205_REG_DWA0		0x06
#define X1205_REG_YRA0		0x05
#define X1205_REG_MOA0		0x04
#define X1205_REG_DTA0		0x03
#define X1205_REG_HRA0		0x02
#define X1205_REG_MNA0		0x01
#define X1205_REG_SCA0		0x00

#define X1205_CCR_BASE		0x30	/* Base address of CCR */
#define X1205_ALM0_BASE		0x00	/* Base address of ALARM0 */

#define X1205_SR_RTCF		0x01	/* Clock failure */
#define X1205_SR_WEL		0x02	/* Write Enable Latch */
#define X1205_SR_RWEL		0x04	/* Register Write Enable */

#define X1205_DTR_DTR0		0x01
#define X1205_DTR_DTR1		0x02
#define X1205_DTR_DTR2		0x04

#define X1205_HR_MIL		0x80	/* Set in ccr.hour for 24 hr mode */

/* Prototypes */
static int x1205_attach(struct i2c_adapter *adapter);
static int x1205_detach(struct i2c_client *client);
static int x1205_probe(struct i2c_adapter *adapter, int address, int kind);

static struct i2c_driver x1205_driver = {
	.driver		= {
		.name	= "x1205",
	},
	.id		= I2C_DRIVERID_X1205,
	.attach_adapter = &x1205_attach,
	.detach_client	= &x1205_detach,
};

/*
 * In the routines that deal directly with the x1205 hardware, we use
 * rtc_time -- month 0-11, hour 0-23, yr = calendar year-epoch
 * Epoch is initialized as 2000. Time is set to UTC.
 */
static int x1205_get_datetime(struct i2c_client *client, struct rtc_time *tm,
				unsigned char reg_base)
{
	unsigned char dt_addr[2] = { 0, reg_base };

	unsigned char buf[8];

	struct i2c_msg msgs[] = {
		{ client->addr, 0, 2, dt_addr },	/* setup read ptr */
		{ client->addr, I2C_M_RD, 8, buf },	/* read date */
	};

	/* read date registers */
	if ((i2c_transfer(client->adapter, &msgs[0], 2)) != 2) {
		dev_err(&client->dev, "%s: read error\n", __FUNCTION__);
		return -EIO;
	}

	dev_dbg(&client->dev,
		"%s: raw read data - sec=%02x, min=%02x, hr=%02x, "
		"mday=%02x, mon=%02x, year=%02x, wday=%02x, y2k=%02x\n",
		__FUNCTION__,
		buf[0], buf[1], buf[2], buf[3],
		buf[4], buf[5], buf[6], buf[7]);

	tm->tm_sec = BCD2BIN(buf[CCR_SEC]);
	tm->tm_min = BCD2BIN(buf[CCR_MIN]);
	tm->tm_hour = BCD2BIN(buf[CCR_HOUR] & 0x3F); /* hr is 0-23 */
	tm->tm_mday = BCD2BIN(buf[CCR_MDAY]);
	tm->tm_mon = BCD2BIN(buf[CCR_MONTH]) - 1; /* mon is 0-11 */
	tm->tm_year = BCD2BIN(buf[CCR_YEAR])
			+ (BCD2BIN(buf[CCR_Y2K]) * 100) - 1900;
	tm->tm_wday = buf[CCR_WDAY];

	dev_dbg(&client->dev, "%s: tm is secs=%d, mins=%d, hours=%d, "
		"mday=%d, mon=%d, year=%d, wday=%d\n",
		__FUNCTION__,
		tm->tm_sec, tm->tm_min, tm->tm_hour,
		tm->tm_mday, tm->tm_mon, tm->tm_year, tm->tm_wday);

	return 0;
}

static int x1205_get_status(struct i2c_client *client, unsigned char *sr)
{
	static unsigned char sr_addr[2] = { 0, X1205_REG_SR };

	struct i2c_msg msgs[] = {
		{ client->addr, 0, 2, sr_addr },	/* setup read ptr */
		{ client->addr, I2C_M_RD, 1, sr },	/* read status */
	};

	/* read status register */
	if ((i2c_transfer(client->adapter, &msgs[0], 2)) != 2) {
		dev_err(&client->dev, "%s: read error\n", __FUNCTION__);
		return -EIO;
	}

	return 0;
}

static int x1205_set_datetime(struct i2c_client *client, struct rtc_time *tm,
				int datetoo, u8 reg_base)
{
	int i, xfer;
	unsigned char buf[8];

	static const unsigned char wel[3] = { 0, X1205_REG_SR,
						X1205_SR_WEL };

	static const unsigned char rwel[3] = { 0, X1205_REG_SR,
						X1205_SR_WEL | X1205_SR_RWEL };

	static const unsigned char diswe[3] = { 0, X1205_REG_SR, 0 };

	dev_dbg(&client->dev,
		"%s: secs=%d, mins=%d, hours=%d\n",
		__FUNCTION__,
		tm->tm_sec, tm->tm_min, tm->tm_hour);

	buf[CCR_SEC] = BIN2BCD(tm->tm_sec);
	buf[CCR_MIN] = BIN2BCD(tm->tm_min);

	/* set hour and 24hr bit */
	buf[CCR_HOUR] = BIN2BCD(tm->tm_hour) | X1205_HR_MIL;

	/* should we also set the date? */
	if (datetoo) {
		dev_dbg(&client->dev,
			"%s: mday=%d, mon=%d, year=%d, wday=%d\n",
			__FUNCTION__,
			tm->tm_mday, tm->tm_mon, tm->tm_year, tm->tm_wday);

		buf[CCR_MDAY] = BIN2BCD(tm->tm_mday);

		/* month, 1 - 12 */
		buf[CCR_MONTH] = BIN2BCD(tm->tm_mon + 1);

		/* year, since the rtc epoch*/
		buf[CCR_YEAR] = BIN2BCD(tm->tm_year % 100);
		buf[CCR_WDAY] = tm->tm_wday & 0x07;
		buf[CCR_Y2K] = BIN2BCD(tm->tm_year / 100);
	}

	/* this sequence is required to unlock the chip */
	if ((xfer = i2c_master_send(client, wel, 3)) != 3) {
		dev_err(&client->dev, "%s: wel - %d\n", __FUNCTION__, xfer);
		return -EIO;
	}

	if ((xfer = i2c_master_send(client, rwel, 3)) != 3) {
		dev_err(&client->dev, "%s: rwel - %d\n", __FUNCTION__, xfer);
		return -EIO;
	}

	/* write register's data */
	for (i = 0; i < (datetoo ? 8 : 3); i++) {
		unsigned char rdata[3] = { 0, reg_base + i, buf[i] };

		xfer = i2c_master_send(client, rdata, 3);
		if (xfer != 3) {
			dev_err(&client->dev,
				"%s: xfer=%d addr=%02x, data=%02x\n",
				__FUNCTION__,
				 xfer, rdata[1], rdata[2]);
			return -EIO;
		}
	};

	/* disable further writes */
	if ((xfer = i2c_master_send(client, diswe, 3)) != 3) {
		dev_err(&client->dev, "%s: diswe - %d\n", __FUNCTION__, xfer);
		return -EIO;
	}

	return 0;
}

static int x1205_fix_osc(struct i2c_client *client)
{
	int err;
	struct rtc_time tm;

	tm.tm_hour = tm.tm_min = tm.tm_sec = 0;

	if ((err = x1205_set_datetime(client, &tm, 0, X1205_CCR_BASE)) < 0)
		dev_err(&client->dev,
			"unable to restart the oscillator\n");

	return err;
}

static int x1205_get_dtrim(struct i2c_client *client, int *trim)
{
	unsigned char dtr;
	static unsigned char dtr_addr[2] = { 0, X1205_REG_DTR };

	struct i2c_msg msgs[] = {
		{ client->addr, 0, 2, dtr_addr },	/* setup read ptr */
		{ client->addr, I2C_M_RD, 1, &dtr }, 	/* read dtr */
	};

	/* read dtr register */
	if ((i2c_transfer(client->adapter, &msgs[0], 2)) != 2) {
		dev_err(&client->dev, "%s: read error\n", __FUNCTION__);
		return -EIO;
	}

	dev_dbg(&client->dev, "%s: raw dtr=%x\n", __FUNCTION__, dtr);

	*trim = 0;

	if (dtr & X1205_DTR_DTR0)
		*trim += 20;

	if (dtr & X1205_DTR_DTR1)
		*trim += 10;

	if (dtr & X1205_DTR_DTR2)
		*trim = -*trim;

	return 0;
}

static int x1205_get_atrim(struct i2c_client *client, int *trim)
{
	s8 atr;
	static unsigned char atr_addr[2] = { 0, X1205_REG_ATR };

	struct i2c_msg msgs[] = {
		{ client->addr, 0, 2, atr_addr },	/* setup read ptr */
		{ client->addr, I2C_M_RD, 1, &atr }, 	/* read atr */
	};

	/* read atr register */
	if ((i2c_transfer(client->adapter, &msgs[0], 2)) != 2) {
		dev_err(&client->dev, "%s: read error\n", __FUNCTION__);
		return -EIO;
	}

	dev_dbg(&client->dev, "%s: raw atr=%x\n", __FUNCTION__, atr);

	/* atr is a two's complement value on 6 bits,
	 * perform sign extension. The formula is
	 * Catr = (atr * 0.25pF) + 11.00pF.
	 */
	if (atr & 0x20)
		atr |= 0xC0;

	dev_dbg(&client->dev, "%s: raw atr=%x (%d)\n", __FUNCTION__, atr, atr);

	*trim = (atr * 250) + 11000;

	dev_dbg(&client->dev, "%s: real=%d\n", __FUNCTION__, *trim);

	return 0;
}

struct x1205_limit
{
	unsigned char reg, mask, min, max;
};

static int x1205_validate_client(struct i2c_client *client)
{
	int i, xfer;

	/* Probe array. We will read the register at the specified
	 * address and check if the given bits are zero.
	 */
	static const unsigned char probe_zero_pattern[] = {
		/* register, mask */
		X1205_REG_SR,	0x18,
		X1205_REG_DTR,	0xF8,
		X1205_REG_ATR,	0xC0,
		X1205_REG_INT,	0x18,
		X1205_REG_0,	0xFF,
	};

	static const struct x1205_limit probe_limits_pattern[] = {
		/* register, mask, min, max */
		{ X1205_REG_Y2K,	0xFF,	19,	20	},
		{ X1205_REG_DW,		0xFF,	0,	6	},
		{ X1205_REG_YR,		0xFF,	0,	99	},
		{ X1205_REG_MO,		0xFF,	0,	12	},
		{ X1205_REG_DT,		0xFF,	0,	31	},
		{ X1205_REG_HR,		0x7F,	0,	23	},
		{ X1205_REG_MN,		0xFF,	0,	59	},
		{ X1205_REG_SC,		0xFF,	0,	59	},
		{ X1205_REG_Y2K1,	0xFF,	19,	20	},
		{ X1205_REG_Y2K0,	0xFF,	19,	20	},
	};

	/* check that registers have bits a 0 where expected */
	for (i = 0; i < ARRAY_SIZE(probe_zero_pattern); i += 2) {
		unsigned char buf;

		unsigned char addr[2] = { 0, probe_zero_pattern[i] };

		struct i2c_msg msgs[2] = {
			{ client->addr, 0, 2, addr },
			{ client->addr, I2C_M_RD, 1, &buf },
		};

		if ((xfer = i2c_transfer(client->adapter, msgs, 2)) != 2) {
			dev_err(&client->dev,
				"%s: could not read register %x\n",
				__FUNCTION__, probe_zero_pattern[i]);

			return -EIO;
		}

		if ((buf & probe_zero_pattern[i+1]) != 0) {
			dev_err(&client->dev,
				"%s: register=%02x, zero pattern=%d, value=%x\n",
				__FUNCTION__, probe_zero_pattern[i], i, buf);

			return -ENODEV;
		}
	}

	/* check limits (only registers with bcd values) */
	for (i = 0; i < ARRAY_SIZE(probe_limits_pattern); i++) {
		unsigned char reg, value;

		unsigned char addr[2] = { 0, probe_limits_pattern[i].reg };

		struct i2c_msg msgs[2] = {
			{ client->addr, 0, 2, addr },
			{ client->addr, I2C_M_RD, 1, &reg },
		};

		if ((xfer = i2c_transfer(client->adapter, msgs, 2)) != 2) {
			dev_err(&client->dev,
				"%s: could not read register %x\n",
				__FUNCTION__, probe_limits_pattern[i].reg);

			return -EIO;
		}

		value = BCD2BIN(reg & probe_limits_pattern[i].mask);

		if (value > probe_limits_pattern[i].max ||
			value < probe_limits_pattern[i].min) {
			dev_dbg(&client->dev,
				"%s: register=%x, lim pattern=%d, value=%d\n",
				__FUNCTION__, probe_limits_pattern[i].reg,
				i, value);

			return -ENODEV;
		}
	}

	return 0;
}

static int x1205_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	return x1205_get_datetime(to_i2c_client(dev),
		&alrm->time, X1205_ALM0_BASE);
}

static int x1205_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	return x1205_set_datetime(to_i2c_client(dev),
		&alrm->time, 1, X1205_ALM0_BASE);
}

static int x1205_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	return x1205_get_datetime(to_i2c_client(dev),
		tm, X1205_CCR_BASE);
}

static int x1205_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	return x1205_set_datetime(to_i2c_client(dev),
		tm, 1, X1205_CCR_BASE);
}

static int x1205_rtc_proc(struct device *dev, struct seq_file *seq)
{
	int err, dtrim, atrim;

	if ((err = x1205_get_dtrim(to_i2c_client(dev), &dtrim)) == 0)
		seq_printf(seq, "digital_trim\t: %d ppm\n", dtrim);

	if ((err = x1205_get_atrim(to_i2c_client(dev), &atrim)) == 0)
		seq_printf(seq, "analog_trim\t: %d.%02d pF\n",
			atrim / 1000, atrim % 1000);
	return 0;
}

static const struct rtc_class_ops x1205_rtc_ops = {
	.proc		= x1205_rtc_proc,
	.read_time	= x1205_rtc_read_time,
	.set_time	= x1205_rtc_set_time,
	.read_alarm	= x1205_rtc_read_alarm,
	.set_alarm	= x1205_rtc_set_alarm,
};

static ssize_t x1205_sysfs_show_atrim(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int err, atrim;

	err = x1205_get_atrim(to_i2c_client(dev), &atrim);
	if (err)
		return err;

	return sprintf(buf, "%d.%02d pF\n", atrim / 1000, atrim % 1000);
}
static DEVICE_ATTR(atrim, S_IRUGO, x1205_sysfs_show_atrim, NULL);

static ssize_t x1205_sysfs_show_dtrim(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int err, dtrim;

	err = x1205_get_dtrim(to_i2c_client(dev), &dtrim);
	if (err)
		return err;

	return sprintf(buf, "%d ppm\n", dtrim);
}
static DEVICE_ATTR(dtrim, S_IRUGO, x1205_sysfs_show_dtrim, NULL);

static int x1205_attach(struct i2c_adapter *adapter)
{
	return i2c_probe(adapter, &addr_data, x1205_probe);
}

static int x1205_probe(struct i2c_adapter *adapter, int address, int kind)
{
	int err = 0;
	unsigned char sr;
	struct i2c_client *client;
	struct rtc_device *rtc;

	dev_dbg(&adapter->dev, "%s\n", __FUNCTION__);

	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C)) {
		err = -ENODEV;
		goto exit;
	}

	if (!(client = kzalloc(sizeof(struct i2c_client), GFP_KERNEL))) {
		err = -ENOMEM;
		goto exit;
	}

	/* I2C client */
	client->addr = address;
	client->driver = &x1205_driver;
	client->adapter	= adapter;

	strlcpy(client->name, x1205_driver.driver.name, I2C_NAME_SIZE);

	/* Verify the chip is really an X1205 */
	if (kind < 0) {
		if (x1205_validate_client(client) < 0) {
			err = -ENODEV;
			goto exit_kfree;
		}
	}

	/* Inform the i2c layer */
	if ((err = i2c_attach_client(client)))
		goto exit_kfree;

	dev_info(&client->dev, "chip found, driver version " DRV_VERSION "\n");

	rtc = rtc_device_register(x1205_driver.driver.name, &client->dev,
				&x1205_rtc_ops, THIS_MODULE);

	if (IS_ERR(rtc)) {
		err = PTR_ERR(rtc);
		goto exit_detach;
	}

	i2c_set_clientdata(client, rtc);

	/* Check for power failures and eventualy enable the osc */
	if ((err = x1205_get_status(client, &sr)) == 0) {
		if (sr & X1205_SR_RTCF) {
			dev_err(&client->dev,
				"power failure detected, "
				"please set the clock\n");
			udelay(50);
			x1205_fix_osc(client);
		}
	}
	else
		dev_err(&client->dev, "couldn't read status\n");

	err = device_create_file(&client->dev, &dev_attr_atrim);
	if (err) goto exit_devreg;
	err = device_create_file(&client->dev, &dev_attr_dtrim);
	if (err) goto exit_atrim;

	return 0;

exit_atrim:
	device_remove_file(&client->dev, &dev_attr_atrim);

exit_devreg:
	rtc_device_unregister(rtc);

exit_detach:
	i2c_detach_client(client);

exit_kfree:
	kfree(client);

exit:
	return err;
}

static int x1205_detach(struct i2c_client *client)
{
	int err;
	struct rtc_device *rtc = i2c_get_clientdata(client);

 	if (rtc)
		rtc_device_unregister(rtc);

	if ((err = i2c_detach_client(client)))
		return err;

	kfree(client);

	return 0;
}

static int __init x1205_init(void)
{
	return i2c_add_driver(&x1205_driver);
}

static void __exit x1205_exit(void)
{
	i2c_del_driver(&x1205_driver);
}

MODULE_AUTHOR(
	"Karen Spearel <kas111 at gmail dot com>, "
	"Alessandro Zummo <a.zummo@towertech.it>");
MODULE_DESCRIPTION("Xicor/Intersil X1205 RTC driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);

module_init(x1205_init);
module_exit(x1205_exit);
