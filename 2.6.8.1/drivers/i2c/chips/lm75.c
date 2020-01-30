/*
    lm75.c - Part of lm_sensors, Linux kernel modules for hardware
             monitoring
    Copyright (c) 1998, 1999  Frodo Looijaard <frodol@dds.nl>

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
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <linux/config.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/i2c-sensor.h>
#include "lm75.h"


/* Addresses to scan */
static unsigned short normal_i2c[] = { I2C_CLIENT_END };
static unsigned short normal_i2c_range[] = { 0x48, 0x4f, I2C_CLIENT_END };
static unsigned int normal_isa[] = { I2C_CLIENT_ISA_END };
static unsigned int normal_isa_range[] = { I2C_CLIENT_ISA_END };

/* Insmod parameters */
SENSORS_INSMOD_1(lm75);

/* Many LM75 constants specified below */

/* The LM75 registers */
#define LM75_REG_TEMP		0x00
#define LM75_REG_CONF		0x01
#define LM75_REG_TEMP_HYST	0x02
#define LM75_REG_TEMP_OS	0x03

/* Each client has this additional data */
struct lm75_data {
	struct i2c_client	client;
	struct semaphore	update_lock;
	char			valid;		/* !=0 if following fields are valid */
	unsigned long		last_updated;	/* In jiffies */
	u16			temp_input;	/* Register values */
	u16			temp_max;
	u16			temp_hyst;
};

static int lm75_attach_adapter(struct i2c_adapter *adapter);
static int lm75_detect(struct i2c_adapter *adapter, int address, int kind);
static void lm75_init_client(struct i2c_client *client);
static int lm75_detach_client(struct i2c_client *client);
static int lm75_read_value(struct i2c_client *client, u8 reg);
static int lm75_write_value(struct i2c_client *client, u8 reg, u16 value);
static struct lm75_data *lm75_update_device(struct device *dev);


/* This is the driver that will be inserted */
static struct i2c_driver lm75_driver = {
	.owner		= THIS_MODULE,
	.name		= "lm75",
	.id		= I2C_DRIVERID_LM75,
	.flags		= I2C_DF_NOTIFY,
	.attach_adapter	= lm75_attach_adapter,
	.detach_client	= lm75_detach_client,
};

static int lm75_id = 0;

#define show(value)	\
static ssize_t show_##value(struct device *dev, char *buf)		\
{									\
	struct lm75_data *data = lm75_update_device(dev);		\
	return sprintf(buf, "%d\n", LM75_TEMP_FROM_REG(data->value));	\
}
show(temp_max);
show(temp_hyst);
show(temp_input);

#define set(value, reg)	\
static ssize_t set_##value(struct device *dev, const char *buf, size_t count)	\
{								\
	struct i2c_client *client = to_i2c_client(dev);		\
	struct lm75_data *data = i2c_get_clientdata(client);	\
	int temp = simple_strtoul(buf, NULL, 10);		\
	data->value = LM75_TEMP_TO_REG(temp);			\
	lm75_write_value(client, reg, data->value);		\
	return count;						\
}
set(temp_max, LM75_REG_TEMP_OS);
set(temp_hyst, LM75_REG_TEMP_HYST);

static DEVICE_ATTR(temp1_max, S_IWUSR | S_IRUGO, show_temp_max, set_temp_max);
static DEVICE_ATTR(temp1_max_hyst, S_IWUSR | S_IRUGO, show_temp_hyst, set_temp_hyst);
static DEVICE_ATTR(temp1_input, S_IRUGO, show_temp_input, NULL);

static int lm75_attach_adapter(struct i2c_adapter *adapter)
{
	if (!(adapter->class & I2C_CLASS_HWMON))
		return 0;
	return i2c_detect(adapter, &addr_data, lm75_detect);
}

/* This function is called by i2c_detect */
static int lm75_detect(struct i2c_adapter *adapter, int address, int kind)
{
	int i;
	struct i2c_client *new_client;
	struct lm75_data *data;
	int err = 0;
	const char *name = "";

	/* Make sure we aren't probing the ISA bus!! This is just a safety check
	   at this moment; i2c_detect really won't call us. */
#ifdef DEBUG
	if (i2c_is_isa_adapter(adapter)) {
		dev_dbg(&adapter->dev,
			"lm75_detect called for an ISA bus adapter?!?\n");
		goto exit;
	}
#endif

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA |
				     I2C_FUNC_SMBUS_WORD_DATA))
		goto exit;

	/* OK. For now, we presume we have a valid client. We now create the
	   client structure, even though we cannot fill it completely yet.
	   But it allows us to access lm75_{read,write}_value. */
	if (!(data = kmalloc(sizeof(struct lm75_data), GFP_KERNEL))) {
		err = -ENOMEM;
		goto exit;
	}
	memset(data, 0, sizeof(struct lm75_data));

	new_client = &data->client;
	i2c_set_clientdata(new_client, data);
	new_client->addr = address;
	new_client->adapter = adapter;
	new_client->driver = &lm75_driver;
	new_client->flags = 0;

	/* Now, we do the remaining detection. There is no identification-
	   dedicated register so we have to rely on several tricks:
	   unused bits, registers cycling over 8-address boundaries,
	   addresses 0x04-0x07 returning the last read value.
	   The cycling+unused addresses combination is not tested,
	   since it would significantly slow the detection down and would
	   hardly add any value. */
	if (kind < 0) {
		int cur, conf, hyst, os;

		/* Unused addresses */
		cur = i2c_smbus_read_word_data(new_client, 0);
		conf = i2c_smbus_read_byte_data(new_client, 1);
		hyst = i2c_smbus_read_word_data(new_client, 2);
		if (i2c_smbus_read_word_data(new_client, 4) != hyst
		 || i2c_smbus_read_word_data(new_client, 5) != hyst
		 || i2c_smbus_read_word_data(new_client, 6) != hyst
		 || i2c_smbus_read_word_data(new_client, 7) != hyst)
		 	goto exit_free;
		os = i2c_smbus_read_word_data(new_client, 3);
		if (i2c_smbus_read_word_data(new_client, 4) != os
		 || i2c_smbus_read_word_data(new_client, 5) != os
		 || i2c_smbus_read_word_data(new_client, 6) != os
		 || i2c_smbus_read_word_data(new_client, 7) != os)
		 	goto exit_free;

		/* Unused bits */
		if (conf & 0xe0)
		 	goto exit_free;

		/* Addresses cycling */
		for (i = 8; i < 0xff; i += 8)
			if (i2c_smbus_read_byte_data(new_client, i + 1) != conf
			 || i2c_smbus_read_word_data(new_client, i + 2) != hyst
			 || i2c_smbus_read_word_data(new_client, i + 3) != os)
				goto exit_free;
	}

	/* Determine the chip type - only one kind supported! */
	if (kind <= 0)
		kind = lm75;

	if (kind == lm75) {
		name = "lm75";
	}

	/* Fill in the remaining client fields and put it into the global list */
	strlcpy(new_client->name, name, I2C_NAME_SIZE);

	new_client->id = lm75_id++;
	data->valid = 0;
	init_MUTEX(&data->update_lock);

	/* Tell the I2C layer a new client has arrived */
	if ((err = i2c_attach_client(new_client)))
		goto exit_free;

	/* Initialize the LM75 chip */
	lm75_init_client(new_client);
	
	/* Register sysfs hooks */
	device_create_file(&new_client->dev, &dev_attr_temp1_max);
	device_create_file(&new_client->dev, &dev_attr_temp1_max_hyst);
	device_create_file(&new_client->dev, &dev_attr_temp1_input);

	return 0;

exit_free:
	kfree(data);
exit:
	return err;
}

static int lm75_detach_client(struct i2c_client *client)
{
	i2c_detach_client(client);
	kfree(i2c_get_clientdata(client));
	return 0;
}

/* All registers are word-sized, except for the configuration register.
   LM75 uses a high-byte first convention, which is exactly opposite to
   the usual practice. */
static int lm75_read_value(struct i2c_client *client, u8 reg)
{
	if (reg == LM75_REG_CONF)
		return i2c_smbus_read_byte_data(client, reg);
	else
		return swab16(i2c_smbus_read_word_data(client, reg));
}

/* All registers are word-sized, except for the configuration register.
   LM75 uses a high-byte first convention, which is exactly opposite to
   the usual practice. */
static int lm75_write_value(struct i2c_client *client, u8 reg, u16 value)
{
	if (reg == LM75_REG_CONF)
		return i2c_smbus_write_byte_data(client, reg, value);
	else
		return i2c_smbus_write_word_data(client, reg, swab16(value));
}

static void lm75_init_client(struct i2c_client *client)
{
	/* Initialize the LM75 chip */
	lm75_write_value(client, LM75_REG_CONF, 0);
}

static struct lm75_data *lm75_update_device(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lm75_data *data = i2c_get_clientdata(client);

	down(&data->update_lock);

	if ((jiffies - data->last_updated > HZ + HZ / 2) ||
	    (jiffies < data->last_updated) || !data->valid) {
		dev_dbg(&client->dev, "Starting lm75 update\n");

		data->temp_input = lm75_read_value(client, LM75_REG_TEMP);
		data->temp_max = lm75_read_value(client, LM75_REG_TEMP_OS);
		data->temp_hyst = lm75_read_value(client, LM75_REG_TEMP_HYST);
		data->last_updated = jiffies;
		data->valid = 1;
	}

	up(&data->update_lock);

	return data;
}

static int __init sensors_lm75_init(void)
{
	return i2c_add_driver(&lm75_driver);
}

static void __exit sensors_lm75_exit(void)
{
	i2c_del_driver(&lm75_driver);
}

MODULE_AUTHOR("Frodo Looijaard <frodol@dds.nl>");
MODULE_DESCRIPTION("LM75 driver");
MODULE_LICENSE("GPL");

module_init(sensors_lm75_init);
module_exit(sensors_lm75_exit);
