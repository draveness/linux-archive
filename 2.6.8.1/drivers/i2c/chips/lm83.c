/*
 * lm83.c - Part of lm_sensors, Linux kernel modules for hardware
 *          monitoring
 * Copyright (C) 2003  Jean Delvare <khali@linux-fr.org>
 *
 * Heavily inspired from the lm78, lm75 and adm1021 drivers. The LM83 is
 * a sensor chip made by National Semiconductor. It reports up to four
 * temperatures (its own plus up to three external ones) with a 1 deg
 * resolution and a 3-4 deg accuracy. Complete datasheet can be obtained
 * from National's website at:
 *   http://www.national.com/pf/LM/LM83.html
 * Since the datasheet omits to give the chip stepping code, I give it
 * here: 0x03 (at register 0xff).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/i2c-sensor.h>

/*
 * Addresses to scan
 * Address is selected using 2 three-level pins, resulting in 9 possible
 * addresses.
 */

static unsigned short normal_i2c[] = { I2C_CLIENT_END };
static unsigned short normal_i2c_range[] = { 0x18, 0x1a, 0x29, 0x2b,
	0x4c, 0x4e, I2C_CLIENT_END };
static unsigned int normal_isa[] = { I2C_CLIENT_ISA_END };
static unsigned int normal_isa_range[] = { I2C_CLIENT_ISA_END };

/*
 * Insmod parameters
 */

SENSORS_INSMOD_1(lm83);

/*
 * The LM83 registers
 * Manufacturer ID is 0x01 for National Semiconductor.
 */

#define LM83_REG_R_MAN_ID		0xFE
#define LM83_REG_R_CHIP_ID		0xFF
#define LM83_REG_R_CONFIG		0x03
#define LM83_REG_W_CONFIG		0x09
#define LM83_REG_R_STATUS1		0x02
#define LM83_REG_R_STATUS2		0x35
#define LM83_REG_R_LOCAL_TEMP		0x00
#define LM83_REG_R_LOCAL_HIGH		0x05
#define LM83_REG_W_LOCAL_HIGH		0x0B
#define LM83_REG_R_REMOTE1_TEMP		0x30
#define LM83_REG_R_REMOTE1_HIGH		0x38
#define LM83_REG_W_REMOTE1_HIGH		0x50
#define LM83_REG_R_REMOTE2_TEMP		0x01
#define LM83_REG_R_REMOTE2_HIGH		0x07
#define LM83_REG_W_REMOTE2_HIGH		0x0D
#define LM83_REG_R_REMOTE3_TEMP		0x31
#define LM83_REG_R_REMOTE3_HIGH		0x3A
#define LM83_REG_W_REMOTE3_HIGH		0x52
#define LM83_REG_R_TCRIT		0x42
#define LM83_REG_W_TCRIT		0x5A

/*
 * Conversions and various macros
 * The LM83 uses signed 8-bit values.
 */

#define TEMP_FROM_REG(val)	((val > 127 ? val-256 : val) * 1000)
#define TEMP_TO_REG(val)	((val < 0 ? val+256 : val) / 1000)

static const u8 LM83_REG_R_TEMP[] = {
	LM83_REG_R_LOCAL_TEMP,
	LM83_REG_R_REMOTE1_TEMP,
	LM83_REG_R_REMOTE2_TEMP,
	LM83_REG_R_REMOTE3_TEMP
};

static const u8 LM83_REG_R_HIGH[] = {
	LM83_REG_R_LOCAL_HIGH,
	LM83_REG_R_REMOTE1_HIGH,
	LM83_REG_R_REMOTE2_HIGH,
	LM83_REG_R_REMOTE3_HIGH
};

static const u8 LM83_REG_W_HIGH[] = {
	LM83_REG_W_LOCAL_HIGH,
	LM83_REG_W_REMOTE1_HIGH,
	LM83_REG_W_REMOTE2_HIGH,
	LM83_REG_W_REMOTE3_HIGH
};

/*
 * Functions declaration
 */

static int lm83_attach_adapter(struct i2c_adapter *adapter);
static int lm83_detect(struct i2c_adapter *adapter, int address, int kind);
static int lm83_detach_client(struct i2c_client *client);
static struct lm83_data *lm83_update_device(struct device *dev);

/*
 * Driver data (common to all clients)
 */
 
static struct i2c_driver lm83_driver = {
	.owner		= THIS_MODULE,
	.name		= "lm83",
	.id		= I2C_DRIVERID_LM83,
	.flags		= I2C_DF_NOTIFY,
	.attach_adapter	= lm83_attach_adapter,
	.detach_client	= lm83_detach_client,
};

/*
 * Client data (each client gets its own)
 */

struct lm83_data {
	struct i2c_client client;
	struct semaphore update_lock;
	char valid; /* zero until following fields are valid */
	unsigned long last_updated; /* in jiffies */

	/* registers values */
	u8 temp_input[4];
	u8 temp_high[4];
	u8 temp_crit;
	u16 alarms; /* bitvector, combined */
};

/*
 * Internal variables
 */

static int lm83_id = 0;

/*
 * Sysfs stuff
 */

#define show_temp(suffix, value) \
static ssize_t show_temp_##suffix(struct device *dev, char *buf) \
{ \
	struct lm83_data *data = lm83_update_device(dev); \
	return sprintf(buf, "%d\n", TEMP_FROM_REG(data->value)); \
}
show_temp(input1, temp_input[0]);
show_temp(input2, temp_input[1]);
show_temp(input3, temp_input[2]);
show_temp(input4, temp_input[3]);
show_temp(high1, temp_high[0]);
show_temp(high2, temp_high[1]);
show_temp(high3, temp_high[2]);
show_temp(high4, temp_high[3]);
show_temp(crit, temp_crit);

#define set_temp(suffix, value, reg) \
static ssize_t set_temp_##suffix(struct device *dev, const char *buf, \
	size_t count) \
{ \
	struct i2c_client *client = to_i2c_client(dev); \
	struct lm83_data *data = i2c_get_clientdata(client); \
	data->value = TEMP_TO_REG(simple_strtoul(buf, NULL, 10)); \
	i2c_smbus_write_byte_data(client, reg, data->value); \
	return count; \
}
set_temp(high1, temp_high[0], LM83_REG_W_LOCAL_HIGH);
set_temp(high2, temp_high[1], LM83_REG_W_REMOTE1_HIGH);
set_temp(high3, temp_high[2], LM83_REG_W_REMOTE2_HIGH);
set_temp(high4, temp_high[3], LM83_REG_W_REMOTE3_HIGH);
set_temp(crit, temp_crit, LM83_REG_W_TCRIT);

static ssize_t show_alarms(struct device *dev, char *buf)
{
	struct lm83_data *data = lm83_update_device(dev);
	return sprintf(buf, "%d\n", data->alarms);
}

static DEVICE_ATTR(temp1_input, S_IRUGO, show_temp_input1, NULL);
static DEVICE_ATTR(temp2_input, S_IRUGO, show_temp_input2, NULL);
static DEVICE_ATTR(temp3_input, S_IRUGO, show_temp_input3, NULL);
static DEVICE_ATTR(temp4_input, S_IRUGO, show_temp_input4, NULL);
static DEVICE_ATTR(temp1_max, S_IWUSR | S_IRUGO, show_temp_high1,
    set_temp_high1);
static DEVICE_ATTR(temp2_max, S_IWUSR | S_IRUGO, show_temp_high2,
    set_temp_high2);
static DEVICE_ATTR(temp3_max, S_IWUSR | S_IRUGO, show_temp_high3,
    set_temp_high3);
static DEVICE_ATTR(temp4_max, S_IWUSR | S_IRUGO, show_temp_high4,
    set_temp_high4);
static DEVICE_ATTR(temp_crit, S_IWUSR | S_IRUGO, show_temp_crit,
    set_temp_crit);
static DEVICE_ATTR(alarms, S_IRUGO, show_alarms, NULL);

/*
 * Real code
 */

static int lm83_attach_adapter(struct i2c_adapter *adapter)
{
	if (!(adapter->class & I2C_CLASS_HWMON))
		return 0;
	return i2c_detect(adapter, &addr_data, lm83_detect);
}

/*
 * The following function does more than just detection. If detection
 * succeeds, it also registers the new chip.
 */
static int lm83_detect(struct i2c_adapter *adapter, int address, int kind)
{
	struct i2c_client *new_client;
	struct lm83_data *data;
	int err = 0;
	const char *name = "";

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		goto exit;

	if (!(data = kmalloc(sizeof(struct lm83_data), GFP_KERNEL))) {
		err = -ENOMEM;
		goto exit;
	}
	memset(data, 0, sizeof(struct lm83_data));

	/* The common I2C client data is placed right after the
	 * LM83-specific data. */
	new_client = &data->client;
	i2c_set_clientdata(new_client, data);
	new_client->addr = address;
	new_client->adapter = adapter;
	new_client->driver = &lm83_driver;
	new_client->flags = 0;

	/* Now we do the detection and identification. A negative kind
	 * means that the driver was loaded with no force parameter
	 * (default), so we must both detect and identify the chip
	 * (actually there is only one possible kind of chip for now, LM83).
	 * A zero kind means that the driver was loaded with the force
	 * parameter, the detection step shall be skipped. A positive kind
	 * means that the driver was loaded with the force parameter and a
	 * given kind of chip is requested, so both the detection and the
	 * identification steps are skipped. */
	if (kind < 0) { /* detection */
		if (((i2c_smbus_read_byte_data(new_client, LM83_REG_R_STATUS1)
		    & 0xA8) != 0x00) ||
		    ((i2c_smbus_read_byte_data(new_client, LM83_REG_R_STATUS2)
		    & 0x48) != 0x00) ||
		    ((i2c_smbus_read_byte_data(new_client, LM83_REG_R_CONFIG)
		    & 0x41) != 0x00)) {
			dev_dbg(&adapter->dev,
			    "LM83 detection failed at 0x%02x.\n", address);
			goto exit_free;
		}
	}

	if (kind <= 0) { /* identification */
		u8 man_id, chip_id;

		man_id = i2c_smbus_read_byte_data(new_client,
		    LM83_REG_R_MAN_ID);
		chip_id = i2c_smbus_read_byte_data(new_client,
		    LM83_REG_R_CHIP_ID);

		if (man_id == 0x01) { /* National Semiconductor */
			if (chip_id == 0x03) {
				kind = lm83;
			}
		}

		if (kind <= 0) { /* identification failed */
			dev_info(&adapter->dev,
			    "Unsupported chip (man_id=0x%02X, "
			    "chip_id=0x%02X).\n", man_id, chip_id);
			goto exit_free;
		}
	}

	if (kind == lm83) {
		name = "lm83";
	}

	/* We can fill in the remaining client fields */
	strlcpy(new_client->name, name, I2C_NAME_SIZE);
	new_client->id = lm83_id++;
	data->valid = 0;
	init_MUTEX(&data->update_lock);

	/* Tell the I2C layer a new client has arrived */
	if ((err = i2c_attach_client(new_client)))
		goto exit_free;

	/*
	 * Initialize the LM83 chip
	 * (Nothing to do for this one.)
	 */

	/* Register sysfs hooks */
	device_create_file(&new_client->dev, &dev_attr_temp1_input);
	device_create_file(&new_client->dev, &dev_attr_temp2_input);
	device_create_file(&new_client->dev, &dev_attr_temp3_input);
	device_create_file(&new_client->dev, &dev_attr_temp4_input);
	device_create_file(&new_client->dev, &dev_attr_temp1_max);
	device_create_file(&new_client->dev, &dev_attr_temp2_max);
	device_create_file(&new_client->dev, &dev_attr_temp3_max);
	device_create_file(&new_client->dev, &dev_attr_temp4_max);
	device_create_file(&new_client->dev, &dev_attr_temp_crit);
	device_create_file(&new_client->dev, &dev_attr_alarms);

	return 0;

exit_free:
	kfree(data);
exit:
	return err;
}

static int lm83_detach_client(struct i2c_client *client)
{
	int err;

	if ((err = i2c_detach_client(client))) {
		dev_err(&client->dev,
		    "Client deregistration failed, client not detached.\n");
		return err;
	}

	kfree(i2c_get_clientdata(client));
	return 0;
}

static struct lm83_data *lm83_update_device(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lm83_data *data = i2c_get_clientdata(client);

	down(&data->update_lock);

	if ((jiffies - data->last_updated > HZ * 2) ||
	    (jiffies < data->last_updated) ||
	    !data->valid) {
		int nr;

		dev_dbg(&client->dev, "Updating lm83 data.\n");
		for (nr = 0; nr < 4 ; nr++) {
			data->temp_input[nr] =
			    i2c_smbus_read_byte_data(client,
			    LM83_REG_R_TEMP[nr]);
			data->temp_high[nr] =
			    i2c_smbus_read_byte_data(client,
			    LM83_REG_R_HIGH[nr]);
		}
		data->temp_crit =
		    i2c_smbus_read_byte_data(client, LM83_REG_R_TCRIT);
		data->alarms =
		    i2c_smbus_read_byte_data(client, LM83_REG_R_STATUS1)
		    + (i2c_smbus_read_byte_data(client, LM83_REG_R_STATUS2)
		    << 8);

		data->last_updated = jiffies;
		data->valid = 1;
	}

	up(&data->update_lock);

	return data;
}

static int __init sensors_lm83_init(void)
{
	return i2c_add_driver(&lm83_driver);
}

static void __exit sensors_lm83_exit(void)
{
	i2c_del_driver(&lm83_driver);
}

MODULE_AUTHOR("Jean Delvare <khali@linux-fr.org>");
MODULE_DESCRIPTION("LM83 driver");
MODULE_LICENSE("GPL");

module_init(sensors_lm83_init);
module_exit(sensors_lm83_exit);
