/*
    lm78.c - Part of lm_sensors, Linux kernel modules for hardware
             monitoring
    Copyright (c) 1998, 1999  Frodo Looijaard <frodol@dds.nl> 
    Copyright (c) 2007        Jean Delvare <khali@linux-fr.org>

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

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/ioport.h>
#include <linux/hwmon.h>
#include <linux/hwmon-vid.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <asm/io.h>

/* ISA device, if found */
static struct platform_device *pdev;

/* Addresses to scan */
static unsigned short normal_i2c[] = { 0x20, 0x21, 0x22, 0x23, 0x24,
					0x25, 0x26, 0x27, 0x28, 0x29,
					0x2a, 0x2b, 0x2c, 0x2d, 0x2e,
					0x2f, I2C_CLIENT_END };
static unsigned short isa_address = 0x290;

/* Insmod parameters */
I2C_CLIENT_INSMOD_2(lm78, lm79);

/* Many LM78 constants specified below */

/* Length of ISA address segment */
#define LM78_EXTENT 8

/* Where are the ISA address/data registers relative to the base address */
#define LM78_ADDR_REG_OFFSET 5
#define LM78_DATA_REG_OFFSET 6

/* The LM78 registers */
#define LM78_REG_IN_MAX(nr) (0x2b + (nr) * 2)
#define LM78_REG_IN_MIN(nr) (0x2c + (nr) * 2)
#define LM78_REG_IN(nr) (0x20 + (nr))

#define LM78_REG_FAN_MIN(nr) (0x3b + (nr))
#define LM78_REG_FAN(nr) (0x28 + (nr))

#define LM78_REG_TEMP 0x27
#define LM78_REG_TEMP_OVER 0x39
#define LM78_REG_TEMP_HYST 0x3a

#define LM78_REG_ALARM1 0x41
#define LM78_REG_ALARM2 0x42

#define LM78_REG_VID_FANDIV 0x47

#define LM78_REG_CONFIG 0x40
#define LM78_REG_CHIPID 0x49
#define LM78_REG_I2C_ADDR 0x48


/* Conversions. Rounding and limit checking is only done on the TO_REG 
   variants. */

/* IN: mV, (0V to 4.08V)
   REG: 16mV/bit */
static inline u8 IN_TO_REG(unsigned long val)
{
	unsigned long nval = SENSORS_LIMIT(val, 0, 4080);
	return (nval + 8) / 16;
}
#define IN_FROM_REG(val) ((val) *  16)

static inline u8 FAN_TO_REG(long rpm, int div)
{
	if (rpm <= 0)
		return 255;
	return SENSORS_LIMIT((1350000 + rpm * div / 2) / (rpm * div), 1, 254);
}

static inline int FAN_FROM_REG(u8 val, int div)
{
	return val==0 ? -1 : val==255 ? 0 : 1350000/(val*div);
}

/* TEMP: mC (-128C to +127C)
   REG: 1C/bit, two's complement */
static inline s8 TEMP_TO_REG(int val)
{
	int nval = SENSORS_LIMIT(val, -128000, 127000) ;
	return nval<0 ? (nval-500)/1000 : (nval+500)/1000;
}

static inline int TEMP_FROM_REG(s8 val)
{
	return val * 1000;
}

#define DIV_FROM_REG(val) (1 << (val))

/* There are some complications in a module like this. First off, LM78 chips
   may be both present on the SMBus and the ISA bus, and we have to handle
   those cases separately at some places. Second, there might be several
   LM78 chips available (well, actually, that is probably never done; but
   it is a clean illustration of how to handle a case like that). Finally,
   a specific chip may be attached to *both* ISA and SMBus, and we would
   not like to detect it double. Fortunately, in the case of the LM78 at
   least, a register tells us what SMBus address we are on, so that helps
   a bit - except if there could be more than one SMBus. Groan. No solution
   for this yet. */

/* For ISA chips, we abuse the i2c_client addr and name fields. We also use
   the driver field to differentiate between I2C and ISA chips. */
struct lm78_data {
	struct i2c_client client;
	struct class_device *class_dev;
	struct mutex lock;
	enum chips type;

	struct mutex update_lock;
	char valid;		/* !=0 if following fields are valid */
	unsigned long last_updated;	/* In jiffies */

	u8 in[7];		/* Register value */
	u8 in_max[7];		/* Register value */
	u8 in_min[7];		/* Register value */
	u8 fan[3];		/* Register value */
	u8 fan_min[3];		/* Register value */
	s8 temp;		/* Register value */
	s8 temp_over;		/* Register value */
	s8 temp_hyst;		/* Register value */
	u8 fan_div[3];		/* Register encoding, shifted right */
	u8 vid;			/* Register encoding, combined */
	u16 alarms;		/* Register encoding, combined */
};


static int lm78_attach_adapter(struct i2c_adapter *adapter);
static int lm78_detect(struct i2c_adapter *adapter, int address, int kind);
static int lm78_detach_client(struct i2c_client *client);

static int __devinit lm78_isa_probe(struct platform_device *pdev);
static int __devexit lm78_isa_remove(struct platform_device *pdev);

static int lm78_read_value(struct lm78_data *data, u8 reg);
static int lm78_write_value(struct lm78_data *data, u8 reg, u8 value);
static struct lm78_data *lm78_update_device(struct device *dev);
static void lm78_init_device(struct lm78_data *data);


static struct i2c_driver lm78_driver = {
	.driver = {
		.name	= "lm78",
	},
	.id		= I2C_DRIVERID_LM78,
	.attach_adapter	= lm78_attach_adapter,
	.detach_client	= lm78_detach_client,
};

static struct platform_driver lm78_isa_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.name	= "lm78",
	},
	.probe		= lm78_isa_probe,
	.remove		= lm78_isa_remove,
};


/* 7 Voltages */
static ssize_t show_in(struct device *dev, struct device_attribute *da,
		       char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct lm78_data *data = lm78_update_device(dev);
	return sprintf(buf, "%d\n", IN_FROM_REG(data->in[attr->index]));
}

static ssize_t show_in_min(struct device *dev, struct device_attribute *da,
			   char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct lm78_data *data = lm78_update_device(dev);
	return sprintf(buf, "%d\n", IN_FROM_REG(data->in_min[attr->index]));
}

static ssize_t show_in_max(struct device *dev, struct device_attribute *da,
			   char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct lm78_data *data = lm78_update_device(dev);
	return sprintf(buf, "%d\n", IN_FROM_REG(data->in_max[attr->index]));
}

static ssize_t set_in_min(struct device *dev, struct device_attribute *da,
			  const char *buf, size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct lm78_data *data = dev_get_drvdata(dev);
	unsigned long val = simple_strtoul(buf, NULL, 10);
	int nr = attr->index;

	mutex_lock(&data->update_lock);
	data->in_min[nr] = IN_TO_REG(val);
	lm78_write_value(data, LM78_REG_IN_MIN(nr), data->in_min[nr]);
	mutex_unlock(&data->update_lock);
	return count;
}

static ssize_t set_in_max(struct device *dev, struct device_attribute *da,
			  const char *buf, size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct lm78_data *data = dev_get_drvdata(dev);
	unsigned long val = simple_strtoul(buf, NULL, 10);
	int nr = attr->index;

	mutex_lock(&data->update_lock);
	data->in_max[nr] = IN_TO_REG(val);
	lm78_write_value(data, LM78_REG_IN_MAX(nr), data->in_max[nr]);
	mutex_unlock(&data->update_lock);
	return count;
}
	
#define show_in_offset(offset)					\
static SENSOR_DEVICE_ATTR(in##offset##_input, S_IRUGO,		\
		show_in, NULL, offset);				\
static SENSOR_DEVICE_ATTR(in##offset##_min, S_IRUGO | S_IWUSR,	\
		show_in_min, set_in_min, offset);		\
static SENSOR_DEVICE_ATTR(in##offset##_max, S_IRUGO | S_IWUSR,	\
		show_in_max, set_in_max, offset);

show_in_offset(0);
show_in_offset(1);
show_in_offset(2);
show_in_offset(3);
show_in_offset(4);
show_in_offset(5);
show_in_offset(6);

/* Temperature */
static ssize_t show_temp(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct lm78_data *data = lm78_update_device(dev);
	return sprintf(buf, "%d\n", TEMP_FROM_REG(data->temp));
}

static ssize_t show_temp_over(struct device *dev, struct device_attribute *da,
			      char *buf)
{
	struct lm78_data *data = lm78_update_device(dev);
	return sprintf(buf, "%d\n", TEMP_FROM_REG(data->temp_over));
}

static ssize_t set_temp_over(struct device *dev, struct device_attribute *da,
			     const char *buf, size_t count)
{
	struct lm78_data *data = dev_get_drvdata(dev);
	long val = simple_strtol(buf, NULL, 10);

	mutex_lock(&data->update_lock);
	data->temp_over = TEMP_TO_REG(val);
	lm78_write_value(data, LM78_REG_TEMP_OVER, data->temp_over);
	mutex_unlock(&data->update_lock);
	return count;
}

static ssize_t show_temp_hyst(struct device *dev, struct device_attribute *da,
			      char *buf)
{
	struct lm78_data *data = lm78_update_device(dev);
	return sprintf(buf, "%d\n", TEMP_FROM_REG(data->temp_hyst));
}

static ssize_t set_temp_hyst(struct device *dev, struct device_attribute *da,
			     const char *buf, size_t count)
{
	struct lm78_data *data = dev_get_drvdata(dev);
	long val = simple_strtol(buf, NULL, 10);

	mutex_lock(&data->update_lock);
	data->temp_hyst = TEMP_TO_REG(val);
	lm78_write_value(data, LM78_REG_TEMP_HYST, data->temp_hyst);
	mutex_unlock(&data->update_lock);
	return count;
}

static DEVICE_ATTR(temp1_input, S_IRUGO, show_temp, NULL);
static DEVICE_ATTR(temp1_max, S_IRUGO | S_IWUSR,
		show_temp_over, set_temp_over);
static DEVICE_ATTR(temp1_max_hyst, S_IRUGO | S_IWUSR,
		show_temp_hyst, set_temp_hyst);

/* 3 Fans */
static ssize_t show_fan(struct device *dev, struct device_attribute *da,
			char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct lm78_data *data = lm78_update_device(dev);
	int nr = attr->index;
	return sprintf(buf, "%d\n", FAN_FROM_REG(data->fan[nr],
		DIV_FROM_REG(data->fan_div[nr])) );
}

static ssize_t show_fan_min(struct device *dev, struct device_attribute *da,
			    char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct lm78_data *data = lm78_update_device(dev);
	int nr = attr->index;
	return sprintf(buf,"%d\n", FAN_FROM_REG(data->fan_min[nr],
		DIV_FROM_REG(data->fan_div[nr])) );
}

static ssize_t set_fan_min(struct device *dev, struct device_attribute *da,
			   const char *buf, size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct lm78_data *data = dev_get_drvdata(dev);
	int nr = attr->index;
	unsigned long val = simple_strtoul(buf, NULL, 10);

	mutex_lock(&data->update_lock);
	data->fan_min[nr] = FAN_TO_REG(val, DIV_FROM_REG(data->fan_div[nr]));
	lm78_write_value(data, LM78_REG_FAN_MIN(nr), data->fan_min[nr]);
	mutex_unlock(&data->update_lock);
	return count;
}

static ssize_t show_fan_div(struct device *dev, struct device_attribute *da,
			    char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct lm78_data *data = lm78_update_device(dev);
	return sprintf(buf, "%d\n", DIV_FROM_REG(data->fan_div[attr->index]));
}

/* Note: we save and restore the fan minimum here, because its value is
   determined in part by the fan divisor.  This follows the principle of
   least surprise; the user doesn't expect the fan minimum to change just
   because the divisor changed. */
static ssize_t set_fan_div(struct device *dev, struct device_attribute *da,
			   const char *buf, size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct lm78_data *data = dev_get_drvdata(dev);
	int nr = attr->index;
	unsigned long val = simple_strtoul(buf, NULL, 10);
	unsigned long min;
	u8 reg;

	mutex_lock(&data->update_lock);
	min = FAN_FROM_REG(data->fan_min[nr],
			   DIV_FROM_REG(data->fan_div[nr]));

	switch (val) {
	case 1: data->fan_div[nr] = 0; break;
	case 2: data->fan_div[nr] = 1; break;
	case 4: data->fan_div[nr] = 2; break;
	case 8: data->fan_div[nr] = 3; break;
	default:
		dev_err(dev, "fan_div value %ld not "
			"supported. Choose one of 1, 2, 4 or 8!\n", val);
		mutex_unlock(&data->update_lock);
		return -EINVAL;
	}

	reg = lm78_read_value(data, LM78_REG_VID_FANDIV);
	switch (nr) {
	case 0:
		reg = (reg & 0xcf) | (data->fan_div[nr] << 4);
		break;
	case 1:
		reg = (reg & 0x3f) | (data->fan_div[nr] << 6);
		break;
	}
	lm78_write_value(data, LM78_REG_VID_FANDIV, reg);

	data->fan_min[nr] =
		FAN_TO_REG(min, DIV_FROM_REG(data->fan_div[nr]));
	lm78_write_value(data, LM78_REG_FAN_MIN(nr), data->fan_min[nr]);
	mutex_unlock(&data->update_lock);

	return count;
}

#define show_fan_offset(offset)				\
static SENSOR_DEVICE_ATTR(fan##offset##_input, S_IRUGO,		\
		show_fan, NULL, offset - 1);			\
static SENSOR_DEVICE_ATTR(fan##offset##_min, S_IRUGO | S_IWUSR,	\
		show_fan_min, set_fan_min, offset - 1);

show_fan_offset(1);
show_fan_offset(2);
show_fan_offset(3);

/* Fan 3 divisor is locked in H/W */
static SENSOR_DEVICE_ATTR(fan1_div, S_IRUGO | S_IWUSR,
		show_fan_div, set_fan_div, 0);
static SENSOR_DEVICE_ATTR(fan2_div, S_IRUGO | S_IWUSR,
		show_fan_div, set_fan_div, 1);
static SENSOR_DEVICE_ATTR(fan3_div, S_IRUGO, show_fan_div, NULL, 2);

/* VID */
static ssize_t show_vid(struct device *dev, struct device_attribute *da,
			char *buf)
{
	struct lm78_data *data = lm78_update_device(dev);
	return sprintf(buf, "%d\n", vid_from_reg(data->vid, 82));
}
static DEVICE_ATTR(cpu0_vid, S_IRUGO, show_vid, NULL);

/* Alarms */
static ssize_t show_alarms(struct device *dev, struct device_attribute *da,
			   char *buf)
{
	struct lm78_data *data = lm78_update_device(dev);
	return sprintf(buf, "%u\n", data->alarms);
}
static DEVICE_ATTR(alarms, S_IRUGO, show_alarms, NULL);

/* This function is called when:
     * lm78_driver is inserted (when this module is loaded), for each
       available adapter
     * when a new adapter is inserted (and lm78_driver is still present) */
static int lm78_attach_adapter(struct i2c_adapter *adapter)
{
	if (!(adapter->class & I2C_CLASS_HWMON))
		return 0;
	return i2c_probe(adapter, &addr_data, lm78_detect);
}

static struct attribute *lm78_attributes[] = {
	&sensor_dev_attr_in0_input.dev_attr.attr,
	&sensor_dev_attr_in0_min.dev_attr.attr,
	&sensor_dev_attr_in0_max.dev_attr.attr,
	&sensor_dev_attr_in1_input.dev_attr.attr,
	&sensor_dev_attr_in1_min.dev_attr.attr,
	&sensor_dev_attr_in1_max.dev_attr.attr,
	&sensor_dev_attr_in2_input.dev_attr.attr,
	&sensor_dev_attr_in2_min.dev_attr.attr,
	&sensor_dev_attr_in2_max.dev_attr.attr,
	&sensor_dev_attr_in3_input.dev_attr.attr,
	&sensor_dev_attr_in3_min.dev_attr.attr,
	&sensor_dev_attr_in3_max.dev_attr.attr,
	&sensor_dev_attr_in4_input.dev_attr.attr,
	&sensor_dev_attr_in4_min.dev_attr.attr,
	&sensor_dev_attr_in4_max.dev_attr.attr,
	&sensor_dev_attr_in5_input.dev_attr.attr,
	&sensor_dev_attr_in5_min.dev_attr.attr,
	&sensor_dev_attr_in5_max.dev_attr.attr,
	&sensor_dev_attr_in6_input.dev_attr.attr,
	&sensor_dev_attr_in6_min.dev_attr.attr,
	&sensor_dev_attr_in6_max.dev_attr.attr,
	&dev_attr_temp1_input.attr,
	&dev_attr_temp1_max.attr,
	&dev_attr_temp1_max_hyst.attr,
	&sensor_dev_attr_fan1_input.dev_attr.attr,
	&sensor_dev_attr_fan1_min.dev_attr.attr,
	&sensor_dev_attr_fan1_div.dev_attr.attr,
	&sensor_dev_attr_fan2_input.dev_attr.attr,
	&sensor_dev_attr_fan2_min.dev_attr.attr,
	&sensor_dev_attr_fan2_div.dev_attr.attr,
	&sensor_dev_attr_fan3_input.dev_attr.attr,
	&sensor_dev_attr_fan3_min.dev_attr.attr,
	&sensor_dev_attr_fan3_div.dev_attr.attr,
	&dev_attr_alarms.attr,
	&dev_attr_cpu0_vid.attr,

	NULL
};

static const struct attribute_group lm78_group = {
	.attrs = lm78_attributes,
};

/* I2C devices get this name attribute automatically, but for ISA devices
   we must create it by ourselves. */
static ssize_t show_name(struct device *dev, struct device_attribute
			 *devattr, char *buf)
{
	struct lm78_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%s\n", data->client.name);
}
static DEVICE_ATTR(name, S_IRUGO, show_name, NULL);

/* This function is called by i2c_probe */
static int lm78_detect(struct i2c_adapter *adapter, int address, int kind)
{
	int i, err;
	struct i2c_client *new_client;
	struct lm78_data *data;
	const char *client_name = "";

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
		err = -ENODEV;
		goto ERROR1;
	}

	/* OK. For now, we presume we have a valid client. We now create the
	   client structure, even though we cannot fill it completely yet.
	   But it allows us to access lm78_{read,write}_value. */

	if (!(data = kzalloc(sizeof(struct lm78_data), GFP_KERNEL))) {
		err = -ENOMEM;
		goto ERROR1;
	}

	new_client = &data->client;
	i2c_set_clientdata(new_client, data);
	new_client->addr = address;
	new_client->adapter = adapter;
	new_client->driver = &lm78_driver;

	/* Now, we do the remaining detection. */
	if (kind < 0) {
		if (lm78_read_value(data, LM78_REG_CONFIG) & 0x80) {
			err = -ENODEV;
			goto ERROR2;
		}
		if (lm78_read_value(data, LM78_REG_I2C_ADDR) !=
		    address) {
			err = -ENODEV;
			goto ERROR2;
		}
	}

	/* Determine the chip type. */
	if (kind <= 0) {
		i = lm78_read_value(data, LM78_REG_CHIPID);
		if (i == 0x00 || i == 0x20	/* LM78 */
		 || i == 0x40)			/* LM78-J */
			kind = lm78;
		else if ((i & 0xfe) == 0xc0)
			kind = lm79;
		else {
			if (kind == 0)
				dev_warn(&adapter->dev, "Ignoring 'force' "
					"parameter for unknown chip at "
					"adapter %d, address 0x%02x\n",
					i2c_adapter_id(adapter), address);
			err = -ENODEV;
			goto ERROR2;
		}
	}

	if (kind == lm78) {
		client_name = "lm78";
	} else if (kind == lm79) {
		client_name = "lm79";
	}

	/* Fill in the remaining client fields and put into the global list */
	strlcpy(new_client->name, client_name, I2C_NAME_SIZE);
	data->type = kind;

	/* Tell the I2C layer a new client has arrived */
	if ((err = i2c_attach_client(new_client)))
		goto ERROR2;

	/* Initialize the LM78 chip */
	lm78_init_device(data);

	/* Register sysfs hooks */
	if ((err = sysfs_create_group(&new_client->dev.kobj, &lm78_group)))
		goto ERROR3;

	data->class_dev = hwmon_device_register(&new_client->dev);
	if (IS_ERR(data->class_dev)) {
		err = PTR_ERR(data->class_dev);
		goto ERROR4;
	}

	return 0;

ERROR4:
	sysfs_remove_group(&new_client->dev.kobj, &lm78_group);
ERROR3:
	i2c_detach_client(new_client);
ERROR2:
	kfree(data);
ERROR1:
	return err;
}

static int lm78_detach_client(struct i2c_client *client)
{
	struct lm78_data *data = i2c_get_clientdata(client);
	int err;

	hwmon_device_unregister(data->class_dev);
	sysfs_remove_group(&client->dev.kobj, &lm78_group);

	if ((err = i2c_detach_client(client)))
		return err;

	kfree(data);

	return 0;
}

static int __devinit lm78_isa_probe(struct platform_device *pdev)
{
	int err;
	struct lm78_data *data;
	struct resource *res;
	const char *name;

	/* Reserve the ISA region */
	res = platform_get_resource(pdev, IORESOURCE_IO, 0);
	if (!request_region(res->start, LM78_EXTENT, "lm78")) {
		err = -EBUSY;
		goto exit;
	}

	if (!(data = kzalloc(sizeof(struct lm78_data), GFP_KERNEL))) {
		err = -ENOMEM;
		goto exit_release_region;
	}
	mutex_init(&data->lock);
	data->client.addr = res->start;
	i2c_set_clientdata(&data->client, data);
	platform_set_drvdata(pdev, data);

	if (lm78_read_value(data, LM78_REG_CHIPID) & 0x80) {
		data->type = lm79;
		name = "lm79";
	} else {
		data->type = lm78;
		name = "lm78";
	}
	strlcpy(data->client.name, name, I2C_NAME_SIZE);

	/* Initialize the LM78 chip */
	lm78_init_device(data);

	/* Register sysfs hooks */
	if ((err = sysfs_create_group(&pdev->dev.kobj, &lm78_group))
	 || (err = device_create_file(&pdev->dev, &dev_attr_name)))
		goto exit_remove_files;

	data->class_dev = hwmon_device_register(&pdev->dev);
	if (IS_ERR(data->class_dev)) {
		err = PTR_ERR(data->class_dev);
		goto exit_remove_files;
	}

	return 0;

 exit_remove_files:
	sysfs_remove_group(&pdev->dev.kobj, &lm78_group);
	device_remove_file(&pdev->dev, &dev_attr_name);
	kfree(data);
 exit_release_region:
	release_region(res->start, LM78_EXTENT);
 exit:
	return err;
}

static int __devexit lm78_isa_remove(struct platform_device *pdev)
{
	struct lm78_data *data = platform_get_drvdata(pdev);

	hwmon_device_unregister(data->class_dev);
	sysfs_remove_group(&pdev->dev.kobj, &lm78_group);
	device_remove_file(&pdev->dev, &dev_attr_name);
	release_region(data->client.addr, LM78_EXTENT);
	kfree(data);

	return 0;
}

/* The SMBus locks itself, but ISA access must be locked explicitly! 
   We don't want to lock the whole ISA bus, so we lock each client
   separately.
   We ignore the LM78 BUSY flag at this moment - it could lead to deadlocks,
   would slow down the LM78 access and should not be necessary.  */
static int lm78_read_value(struct lm78_data *data, u8 reg)
{
	struct i2c_client *client = &data->client;

	if (!client->driver) { /* ISA device */
		int res;
		mutex_lock(&data->lock);
		outb_p(reg, client->addr + LM78_ADDR_REG_OFFSET);
		res = inb_p(client->addr + LM78_DATA_REG_OFFSET);
		mutex_unlock(&data->lock);
		return res;
	} else
		return i2c_smbus_read_byte_data(client, reg);
}

/* The SMBus locks itself, but ISA access muse be locked explicitly! 
   We don't want to lock the whole ISA bus, so we lock each client
   separately.
   We ignore the LM78 BUSY flag at this moment - it could lead to deadlocks,
   would slow down the LM78 access and should not be necessary. 
   There are some ugly typecasts here, but the good new is - they should
   nowhere else be necessary! */
static int lm78_write_value(struct lm78_data *data, u8 reg, u8 value)
{
	struct i2c_client *client = &data->client;

	if (!client->driver) { /* ISA device */
		mutex_lock(&data->lock);
		outb_p(reg, client->addr + LM78_ADDR_REG_OFFSET);
		outb_p(value, client->addr + LM78_DATA_REG_OFFSET);
		mutex_unlock(&data->lock);
		return 0;
	} else
		return i2c_smbus_write_byte_data(client, reg, value);
}

static void lm78_init_device(struct lm78_data *data)
{
	u8 config;
	int i;

	/* Start monitoring */
	config = lm78_read_value(data, LM78_REG_CONFIG);
	if ((config & 0x09) != 0x01)
		lm78_write_value(data, LM78_REG_CONFIG,
				 (config & 0xf7) | 0x01);

	/* A few vars need to be filled upon startup */
	for (i = 0; i < 3; i++) {
		data->fan_min[i] = lm78_read_value(data,
					LM78_REG_FAN_MIN(i));
	}

	mutex_init(&data->update_lock);
}

static struct lm78_data *lm78_update_device(struct device *dev)
{
	struct lm78_data *data = dev_get_drvdata(dev);
	int i;

	mutex_lock(&data->update_lock);

	if (time_after(jiffies, data->last_updated + HZ + HZ / 2)
	    || !data->valid) {

		dev_dbg(dev, "Starting lm78 update\n");

		for (i = 0; i <= 6; i++) {
			data->in[i] =
			    lm78_read_value(data, LM78_REG_IN(i));
			data->in_min[i] =
			    lm78_read_value(data, LM78_REG_IN_MIN(i));
			data->in_max[i] =
			    lm78_read_value(data, LM78_REG_IN_MAX(i));
		}
		for (i = 0; i < 3; i++) {
			data->fan[i] =
			    lm78_read_value(data, LM78_REG_FAN(i));
			data->fan_min[i] =
			    lm78_read_value(data, LM78_REG_FAN_MIN(i));
		}
		data->temp = lm78_read_value(data, LM78_REG_TEMP);
		data->temp_over =
		    lm78_read_value(data, LM78_REG_TEMP_OVER);
		data->temp_hyst =
		    lm78_read_value(data, LM78_REG_TEMP_HYST);
		i = lm78_read_value(data, LM78_REG_VID_FANDIV);
		data->vid = i & 0x0f;
		if (data->type == lm79)
			data->vid |=
			    (lm78_read_value(data, LM78_REG_CHIPID) &
			     0x01) << 4;
		else
			data->vid |= 0x10;
		data->fan_div[0] = (i >> 4) & 0x03;
		data->fan_div[1] = i >> 6;
		data->alarms = lm78_read_value(data, LM78_REG_ALARM1) +
		    (lm78_read_value(data, LM78_REG_ALARM2) << 8);
		data->last_updated = jiffies;
		data->valid = 1;

		data->fan_div[2] = 1;
	}

	mutex_unlock(&data->update_lock);

	return data;
}

/* return 1 if a supported chip is found, 0 otherwise */
static int __init lm78_isa_found(unsigned short address)
{
	int val, save, found = 0;

	if (!request_region(address, LM78_EXTENT, "lm78"))
		return 0;

#define REALLY_SLOW_IO
	/* We need the timeouts for at least some LM78-like
	   chips. But only if we read 'undefined' registers. */
	val = inb_p(address + 1);
	if (inb_p(address + 2) != val
	 || inb_p(address + 3) != val
	 || inb_p(address + 7) != val)
		goto release;
#undef REALLY_SLOW_IO

	/* We should be able to change the 7 LSB of the address port. The
	   MSB (busy flag) should be clear initially, set after the write. */
	save = inb_p(address + LM78_ADDR_REG_OFFSET);
	if (save & 0x80)
		goto release;
	val = ~save & 0x7f;
	outb_p(val, address + LM78_ADDR_REG_OFFSET);
	if (inb_p(address + LM78_ADDR_REG_OFFSET) != (val | 0x80)) {
		outb_p(save, address + LM78_ADDR_REG_OFFSET);
		goto release;
	}

	/* We found a device, now see if it could be an LM78 */
	outb_p(LM78_REG_CONFIG, address + LM78_ADDR_REG_OFFSET);
	val = inb_p(address + LM78_DATA_REG_OFFSET);
	if (val & 0x80)
		goto release;
	outb_p(LM78_REG_I2C_ADDR, address + LM78_ADDR_REG_OFFSET);
	val = inb_p(address + LM78_DATA_REG_OFFSET);
	if (val < 0x03 || val > 0x77)	/* Not a valid I2C address */
		goto release;

	/* The busy flag should be clear again */
	if (inb_p(address + LM78_ADDR_REG_OFFSET) & 0x80)
		goto release;

	/* Explicitly prevent the misdetection of Winbond chips */
	outb_p(0x4f, address + LM78_ADDR_REG_OFFSET);
	val = inb_p(address + LM78_DATA_REG_OFFSET);
	if (val == 0xa3 || val == 0x5c)
		goto release;

	/* Explicitly prevent the misdetection of ITE chips */
	outb_p(0x58, address + LM78_ADDR_REG_OFFSET);
	val = inb_p(address + LM78_DATA_REG_OFFSET);
	if (val == 0x90)
		goto release;

	/* Determine the chip type */
	outb_p(LM78_REG_CHIPID, address + LM78_ADDR_REG_OFFSET);
	val = inb_p(address + LM78_DATA_REG_OFFSET);
	if (val == 0x00 || val == 0x20	/* LM78 */
	 || val == 0x40			/* LM78-J */
	 || (val & 0xfe) == 0xc0)	/* LM79 */
		found = 1;

	if (found)
		pr_info("lm78: Found an %s chip at %#x\n",
			val & 0x80 ? "LM79" : "LM78", (int)address);

 release:
	release_region(address, LM78_EXTENT);
	return found;
}

static int __init lm78_isa_device_add(unsigned short address)
{
	struct resource res = {
		.start	= address,
		.end	= address + LM78_EXTENT - 1,
		.name	= "lm78",
		.flags	= IORESOURCE_IO,
	};
	int err;

	pdev = platform_device_alloc("lm78", address);
	if (!pdev) {
		err = -ENOMEM;
		printk(KERN_ERR "lm78: Device allocation failed\n");
		goto exit;
	}

	err = platform_device_add_resources(pdev, &res, 1);
	if (err) {
		printk(KERN_ERR "lm78: Device resource addition failed "
		       "(%d)\n", err);
		goto exit_device_put;
	}

	err = platform_device_add(pdev);
	if (err) {
		printk(KERN_ERR "lm78: Device addition failed (%d)\n",
		       err);
		goto exit_device_put;
	}

	return 0;

 exit_device_put:
	platform_device_put(pdev);
 exit:
	pdev = NULL;
	return err;
}

static int __init sm_lm78_init(void)
{
	int res;

	res = i2c_add_driver(&lm78_driver);
	if (res)
		goto exit;

	if (lm78_isa_found(isa_address)) {
		res = platform_driver_register(&lm78_isa_driver);
		if (res)
			goto exit_unreg_i2c_driver;

		/* Sets global pdev as a side effect */
		res = lm78_isa_device_add(isa_address);
		if (res)
			goto exit_unreg_isa_driver;
	}

	return 0;

 exit_unreg_isa_driver:
	platform_driver_unregister(&lm78_isa_driver);
 exit_unreg_i2c_driver:
	i2c_del_driver(&lm78_driver);
 exit:
	return res;
}

static void __exit sm_lm78_exit(void)
{
	if (pdev) {
		platform_device_unregister(pdev);
		platform_driver_unregister(&lm78_isa_driver);
	}
	i2c_del_driver(&lm78_driver);
}



MODULE_AUTHOR("Frodo Looijaard <frodol@dds.nl>");
MODULE_DESCRIPTION("LM78/LM79 driver");
MODULE_LICENSE("GPL");

module_init(sm_lm78_init);
module_exit(sm_lm78_exit);
