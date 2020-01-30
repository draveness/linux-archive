/*
    it87.c - Part of lm_sensors, Linux kernel modules for hardware
             monitoring.

    Supports: IT8705F  Super I/O chip w/LPC interface
              IT8712F  Super I/O chip w/LPC interface & SMbus
              Sis950   A clone of the IT8705F

    Copyright (C) 2001 Chris Gauthron <chrisg@0-in.com> 
    Largely inspired by lm78.c of the same package

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

/*
    djg@pdp8.net David Gesswein 7/18/01
    Modified to fix bug with not all alarms enabled.
    Added ability to read battery voltage and select temperature sensor
    type at module load time.
*/

#include <linux/config.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/i2c-sensor.h>
#include <asm/io.h>


/* Addresses to scan */
static unsigned short normal_i2c[] = { I2C_CLIENT_END };
static unsigned short normal_i2c_range[] = { 0x20, 0x2f, I2C_CLIENT_END };
static unsigned int normal_isa[] = { 0x0290, I2C_CLIENT_ISA_END };
static unsigned int normal_isa_range[] = { I2C_CLIENT_ISA_END };

/* Insmod parameters */
SENSORS_INSMOD_1(it87);

#define	REG	0x2e	/* The register to read/write */
#define	DEV	0x07	/* Register: Logical device select */
#define	VAL	0x2f	/* The value to read/write */
#define PME	0x04	/* The device with the fan registers in it */
#define	DEVID	0x20	/* Register: Device ID */

static inline void
superio_outb(int reg, int val)
{
	outb(reg, REG);
	outb(val, VAL);
}

static inline int
superio_inb(int reg)
{
	outb(reg, REG);
	return inb(VAL);
}

static inline void
superio_select(void)
{
	outb(DEV, REG);
	outb(PME, VAL);
}

static inline void
superio_enter(void)
{
	outb(0x87, REG);
	outb(0x01, REG);
	outb(0x55, REG);
	outb(0x55, REG);
}

static inline void
superio_exit(void)
{
	outb(0x02, REG);
	outb(0x02, VAL);
}

/* just IT8712F for now - this should be extended to support the other
   chips as well */
#define IT8712F_DEVID 0x8712
#define IT87_ACT_REG  0x30
#define IT87_BASE_REG 0x60

/* Update battery voltage after every reading if true */
static int update_vbat;

/* Reset the registers on init if true */
static int reset;

/* Many IT87 constants specified below */

/* Length of ISA address segment */
#define IT87_EXTENT 8

/* Where are the ISA address/data registers relative to the base address */
#define IT87_ADDR_REG_OFFSET 5
#define IT87_DATA_REG_OFFSET 6

/*----- The IT87 registers -----*/

#define IT87_REG_CONFIG        0x00

#define IT87_REG_ALARM1        0x01
#define IT87_REG_ALARM2        0x02
#define IT87_REG_ALARM3        0x03

#define IT87_REG_VID           0x0a
#define IT87_REG_FAN_DIV       0x0b

/* Monitors: 9 voltage (0 to 7, battery), 3 temp (1 to 3), 3 fan (1 to 3) */

#define IT87_REG_FAN(nr)       (0x0d + (nr))
#define IT87_REG_FAN_MIN(nr)   (0x10 + (nr))
#define IT87_REG_FAN_MAIN_CTRL 0x13

#define IT87_REG_VIN(nr)       (0x20 + (nr))
#define IT87_REG_TEMP(nr)      (0x29 + (nr))

#define IT87_REG_VIN_MAX(nr)   (0x30 + (nr) * 2)
#define IT87_REG_VIN_MIN(nr)   (0x31 + (nr) * 2)
#define IT87_REG_TEMP_HIGH(nr) (0x40 + (nr) * 2)
#define IT87_REG_TEMP_LOW(nr)  (0x41 + (nr) * 2)

#define IT87_REG_I2C_ADDR      0x48

#define IT87_REG_VIN_ENABLE    0x50
#define IT87_REG_TEMP_ENABLE   0x51

#define IT87_REG_CHIPID        0x58

#define IN_TO_REG(val)  (SENSORS_LIMIT((((val) + 8)/16),0,255))
#define IN_FROM_REG(val) ((val) * 16)

static inline u8 FAN_TO_REG(long rpm, int div)
{
	if (rpm == 0)
		return 255;
	rpm = SENSORS_LIMIT(rpm, 1, 1000000);
	return SENSORS_LIMIT((1350000 + rpm * div / 2) / (rpm * div), 1,
			     254);
}

#define FAN_FROM_REG(val,div) ((val)==0?-1:(val)==255?0:1350000/((val)*(div)))

#define TEMP_TO_REG(val) (SENSORS_LIMIT(((val)<0?(((val)-500)/1000):\
					((val)+500)/1000),-128,127))
#define TEMP_FROM_REG(val) (((val)>0x80?(val)-0x100:(val))*1000)

#define VID_FROM_REG(val) ((val)==0x1f?0:(val)>=0x10?510-(val)*10:\
				205-(val)*5)
#define ALARMS_FROM_REG(val) (val)

static int DIV_TO_REG(int val)
{
	int answer = 0;
	while ((val >>= 1) != 0)
		answer++;
	return answer;
}
#define DIV_FROM_REG(val) (1 << (val))


/* For each registered IT87, we need to keep some data in memory. That
   data is pointed to by it87_list[NR]->data. The structure itself is
   dynamically allocated, at the same time when a new it87 client is
   allocated. */
struct it87_data {
	struct i2c_client client;
	struct semaphore lock;
	enum chips type;

	struct semaphore update_lock;
	char valid;		/* !=0 if following fields are valid */
	unsigned long last_updated;	/* In jiffies */

	u8 in[9];		/* Register value */
	u8 in_max[9];		/* Register value */
	u8 in_min[9];		/* Register value */
	u8 fan[3];		/* Register value */
	u8 fan_min[3];		/* Register value */
	u8 temp[3];		/* Register value */
	u8 temp_high[3];	/* Register value */
	u8 temp_low[3];		/* Register value */
	u8 sensor;		/* Register value */
	u8 fan_div[3];		/* Register encoding, shifted right */
	u8 vid;			/* Register encoding, combined */
	u32 alarms;		/* Register encoding, combined */
};


static int it87_attach_adapter(struct i2c_adapter *adapter);
static int it87_find(int *address);
static int it87_detect(struct i2c_adapter *adapter, int address, int kind);
static int it87_detach_client(struct i2c_client *client);

static int it87_read_value(struct i2c_client *client, u8 register);
static int it87_write_value(struct i2c_client *client, u8 register,
			u8 value);
static struct it87_data *it87_update_device(struct device *dev);
static void it87_init_client(struct i2c_client *client, struct it87_data *data);


static struct i2c_driver it87_driver = {
	.owner		= THIS_MODULE,
	.name		= "it87",
	.id		= I2C_DRIVERID_IT87,
	.flags		= I2C_DF_NOTIFY,
	.attach_adapter	= it87_attach_adapter,
	.detach_client	= it87_detach_client,
};

static int it87_id = 0;

static ssize_t show_in(struct device *dev, char *buf, int nr)
{
	struct it87_data *data = it87_update_device(dev);
	return sprintf(buf, "%d\n", IN_FROM_REG(data->in[nr]));
}

static ssize_t show_in_min(struct device *dev, char *buf, int nr)
{
	struct it87_data *data = it87_update_device(dev);
	return sprintf(buf, "%d\n", IN_FROM_REG(data->in_min[nr]));
}

static ssize_t show_in_max(struct device *dev, char *buf, int nr)
{
	struct it87_data *data = it87_update_device(dev);
	return sprintf(buf, "%d\n", IN_FROM_REG(data->in_max[nr]));
}

static ssize_t set_in_min(struct device *dev, const char *buf, 
		size_t count, int nr)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct it87_data *data = i2c_get_clientdata(client);
	unsigned long val = simple_strtoul(buf, NULL, 10);
	data->in_min[nr] = IN_TO_REG(val);
	it87_write_value(client, IT87_REG_VIN_MIN(nr), 
			data->in_min[nr]);
	return count;
}
static ssize_t set_in_max(struct device *dev, const char *buf, 
		size_t count, int nr)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct it87_data *data = i2c_get_clientdata(client);
	unsigned long val = simple_strtoul(buf, NULL, 10);
	data->in_max[nr] = IN_TO_REG(val);
	it87_write_value(client, IT87_REG_VIN_MAX(nr), 
			data->in_max[nr]);
	return count;
}

#define show_in_offset(offset)					\
static ssize_t							\
	show_in##offset (struct device *dev, char *buf)		\
{								\
	return show_in(dev, buf, 0x##offset);			\
}								\
static DEVICE_ATTR(in##offset##_input, S_IRUGO, show_in##offset, NULL);

#define limit_in_offset(offset)					\
static ssize_t							\
	show_in##offset##_min (struct device *dev, char *buf)	\
{								\
	return show_in_min(dev, buf, 0x##offset);		\
}								\
static ssize_t							\
	show_in##offset##_max (struct device *dev, char *buf)	\
{								\
	return show_in_max(dev, buf, 0x##offset);		\
}								\
static ssize_t set_in##offset##_min (struct device *dev, 	\
		const char *buf, size_t count) 			\
{								\
	return set_in_min(dev, buf, count, 0x##offset);		\
}								\
static ssize_t set_in##offset##_max (struct device *dev,	\
			const char *buf, size_t count)		\
{								\
	return set_in_max(dev, buf, count, 0x##offset);		\
}								\
static DEVICE_ATTR(in##offset##_min, S_IRUGO | S_IWUSR, 	\
		show_in##offset##_min, set_in##offset##_min);	\
static DEVICE_ATTR(in##offset##_max, S_IRUGO | S_IWUSR, 	\
		show_in##offset##_max, set_in##offset##_max);

show_in_offset(0);
limit_in_offset(0);
show_in_offset(1);
limit_in_offset(1);
show_in_offset(2);
limit_in_offset(2);
show_in_offset(3);
limit_in_offset(3);
show_in_offset(4);
limit_in_offset(4);
show_in_offset(5);
limit_in_offset(5);
show_in_offset(6);
limit_in_offset(6);
show_in_offset(7);
limit_in_offset(7);
show_in_offset(8);

/* 3 temperatures */
static ssize_t show_temp(struct device *dev, char *buf, int nr)
{
	struct it87_data *data = it87_update_device(dev);
	return sprintf(buf, "%d\n", TEMP_FROM_REG(data->temp[nr]));
}
static ssize_t show_temp_max(struct device *dev, char *buf, int nr)
{
	struct it87_data *data = it87_update_device(dev);
	return sprintf(buf, "%d\n", TEMP_FROM_REG(data->temp_high[nr]));
}
static ssize_t show_temp_min(struct device *dev, char *buf, int nr)
{
	struct it87_data *data = it87_update_device(dev);
	return sprintf(buf, "%d\n", TEMP_FROM_REG(data->temp_low[nr]));
}
static ssize_t set_temp_max(struct device *dev, const char *buf, 
		size_t count, int nr)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct it87_data *data = i2c_get_clientdata(client);
	int val = simple_strtol(buf, NULL, 10);
	data->temp_high[nr] = TEMP_TO_REG(val);
	it87_write_value(client, IT87_REG_TEMP_HIGH(nr), data->temp_high[nr]);
	return count;
}
static ssize_t set_temp_min(struct device *dev, const char *buf, 
		size_t count, int nr)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct it87_data *data = i2c_get_clientdata(client);
	int val = simple_strtol(buf, NULL, 10);
	data->temp_low[nr] = TEMP_TO_REG(val);
	it87_write_value(client, IT87_REG_TEMP_LOW(nr), data->temp_low[nr]);
	return count;
}
#define show_temp_offset(offset)					\
static ssize_t show_temp_##offset (struct device *dev, char *buf)	\
{									\
	return show_temp(dev, buf, 0x##offset - 1);			\
}									\
static ssize_t								\
show_temp_##offset##_max (struct device *dev, char *buf)		\
{									\
	return show_temp_max(dev, buf, 0x##offset - 1);			\
}									\
static ssize_t								\
show_temp_##offset##_min (struct device *dev, char *buf)		\
{									\
	return show_temp_min(dev, buf, 0x##offset - 1);			\
}									\
static ssize_t set_temp_##offset##_max (struct device *dev, 		\
		const char *buf, size_t count) 				\
{									\
	return set_temp_max(dev, buf, count, 0x##offset - 1);		\
}									\
static ssize_t set_temp_##offset##_min (struct device *dev, 		\
		const char *buf, size_t count) 				\
{									\
	return set_temp_min(dev, buf, count, 0x##offset - 1);		\
}									\
static DEVICE_ATTR(temp##offset##_input, S_IRUGO, show_temp_##offset, NULL); \
static DEVICE_ATTR(temp##offset##_max, S_IRUGO | S_IWUSR, 		\
		show_temp_##offset##_max, set_temp_##offset##_max); 	\
static DEVICE_ATTR(temp##offset##_min, S_IRUGO | S_IWUSR, 		\
		show_temp_##offset##_min, set_temp_##offset##_min);	

show_temp_offset(1);
show_temp_offset(2);
show_temp_offset(3);

static ssize_t show_sensor(struct device *dev, char *buf, int nr)
{
	struct it87_data *data = it87_update_device(dev);
	if (data->sensor & (1 << nr))
		return sprintf(buf, "3\n");  /* thermal diode */
	if (data->sensor & (8 << nr))
		return sprintf(buf, "2\n");  /* thermistor */
	return sprintf(buf, "0\n");      /* disabled */
}
static ssize_t set_sensor(struct device *dev, const char *buf, 
		size_t count, int nr)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct it87_data *data = i2c_get_clientdata(client);
	int val = simple_strtol(buf, NULL, 10);

	data->sensor &= ~(1 << nr);
	data->sensor &= ~(8 << nr);
	/* 3 = thermal diode; 2 = thermistor; 0 = disabled */
	if (val == 3)
	    data->sensor |= 1 << nr;
	else if (val == 2)
	    data->sensor |= 8 << nr;
	else if (val != 0)
		return -EINVAL;
	it87_write_value(client, IT87_REG_TEMP_ENABLE, data->sensor);
	return count;
}
#define show_sensor_offset(offset)					\
static ssize_t show_sensor_##offset (struct device *dev, char *buf)	\
{									\
	return show_sensor(dev, buf, 0x##offset - 1);			\
}									\
static ssize_t set_sensor_##offset (struct device *dev, 		\
		const char *buf, size_t count) 				\
{									\
	return set_sensor(dev, buf, count, 0x##offset - 1);		\
}									\
static DEVICE_ATTR(temp##offset##_type, S_IRUGO | S_IWUSR, 		\
		show_sensor_##offset, set_sensor_##offset);

show_sensor_offset(1);
show_sensor_offset(2);
show_sensor_offset(3);

/* 3 Fans */
static ssize_t show_fan(struct device *dev, char *buf, int nr)
{
	struct it87_data *data = it87_update_device(dev);
	return sprintf(buf,"%d\n", FAN_FROM_REG(data->fan[nr], 
				DIV_FROM_REG(data->fan_div[nr])) );
}
static ssize_t show_fan_min(struct device *dev, char *buf, int nr)
{
	struct it87_data *data = it87_update_device(dev);
	return sprintf(buf,"%d\n",
		FAN_FROM_REG(data->fan_min[nr], DIV_FROM_REG(data->fan_div[nr])) );
}
static ssize_t show_fan_div(struct device *dev, char *buf, int nr)
{
	struct it87_data *data = it87_update_device(dev);
	return sprintf(buf,"%d\n", DIV_FROM_REG(data->fan_div[nr]) );
}
static ssize_t set_fan_min(struct device *dev, const char *buf, 
		size_t count, int nr)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct it87_data *data = i2c_get_clientdata(client);
	int val = simple_strtol(buf, NULL, 10);
	data->fan_min[nr] = FAN_TO_REG(val, DIV_FROM_REG(data->fan_div[nr]));
	it87_write_value(client, IT87_REG_FAN_MIN(nr), data->fan_min[nr]);
	return count;
}
static ssize_t set_fan_div(struct device *dev, const char *buf, 
		size_t count, int nr)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct it87_data *data = i2c_get_clientdata(client);
	int val = simple_strtol(buf, NULL, 10);
	int i, min[3];
	u8 old = it87_read_value(client, IT87_REG_FAN_DIV);

	for (i = 0; i < 3; i++)
		min[i] = FAN_FROM_REG(data->fan_min[i], DIV_FROM_REG(data->fan_div[i]));

	switch (nr) {
	case 0:
	case 1:
		data->fan_div[nr] = DIV_TO_REG(val);
		break;
	case 2:
		if (val < 8)
			data->fan_div[nr] = 1;
		else
			data->fan_div[nr] = 3;
	}
	val = old & 0x100;
	val |= (data->fan_div[0] & 0x07);
	val |= (data->fan_div[1] & 0x07) << 3;
	if (data->fan_div[2] == 3)
		val |= 0x1 << 6;
	it87_write_value(client, IT87_REG_FAN_DIV, val);

	for (i = 0; i < 3; i++) {
		data->fan_min[i]=FAN_TO_REG(min[i], DIV_FROM_REG(data->fan_div[i]));
		it87_write_value(client, IT87_REG_FAN_MIN(i), data->fan_min[i]);
	}
	return count;
}

#define show_fan_offset(offset)						\
static ssize_t show_fan_##offset (struct device *dev, char *buf)	\
{									\
	return show_fan(dev, buf, 0x##offset - 1);			\
}									\
static ssize_t show_fan_##offset##_min (struct device *dev, char *buf)	\
{									\
	return show_fan_min(dev, buf, 0x##offset - 1);			\
}									\
static ssize_t show_fan_##offset##_div (struct device *dev, char *buf)	\
{									\
	return show_fan_div(dev, buf, 0x##offset - 1);			\
}									\
static ssize_t set_fan_##offset##_min (struct device *dev, 		\
	const char *buf, size_t count) 					\
{									\
	return set_fan_min(dev, buf, count, 0x##offset - 1);		\
}									\
static ssize_t set_fan_##offset##_div (struct device *dev, 		\
		const char *buf, size_t count) 				\
{									\
	return set_fan_div(dev, buf, count, 0x##offset - 1);		\
}									\
static DEVICE_ATTR(fan##offset##_input, S_IRUGO, show_fan_##offset, NULL); \
static DEVICE_ATTR(fan##offset##_min, S_IRUGO | S_IWUSR, 		\
		show_fan_##offset##_min, set_fan_##offset##_min); 	\
static DEVICE_ATTR(fan##offset##_div, S_IRUGO | S_IWUSR, 		\
		show_fan_##offset##_div, set_fan_##offset##_div);

show_fan_offset(1);
show_fan_offset(2);
show_fan_offset(3);

/* Alarms */
static ssize_t show_alarms(struct device *dev, char *buf)
{
	struct it87_data *data = it87_update_device(dev);
	return sprintf(buf,"%d\n", ALARMS_FROM_REG(data->alarms));
}
static DEVICE_ATTR(alarms, S_IRUGO | S_IWUSR, show_alarms, NULL);

/* This function is called when:
     * it87_driver is inserted (when this module is loaded), for each
       available adapter
     * when a new adapter is inserted (and it87_driver is still present) */
static int it87_attach_adapter(struct i2c_adapter *adapter)
{
	if (!(adapter->class & I2C_CLASS_HWMON))
		return 0;
	return i2c_detect(adapter, &addr_data, it87_detect);
}

/* SuperIO detection - will change normal_isa[0] if a chip is found */
static int it87_find(int *address)
{
	u16 val;

	superio_enter();
	val = (superio_inb(DEVID) << 8) |
	       superio_inb(DEVID + 1);
	if (val != IT8712F_DEVID) {
		superio_exit();
		return -ENODEV;
	}

	superio_select();
	val = (superio_inb(IT87_BASE_REG) << 8) |
	       superio_inb(IT87_BASE_REG + 1);
	superio_exit();
	*address = val & ~(IT87_EXTENT - 1);
	if (*address == 0) {
		return -ENODEV;
	}
	return 0;
}

/* This function is called by i2c_detect */
int it87_detect(struct i2c_adapter *adapter, int address, int kind)
{
	int i;
	struct i2c_client *new_client;
	struct it87_data *data;
	int err = 0;
	const char *name = "";
	int is_isa = i2c_is_isa_adapter(adapter);

	if (!is_isa && 
	    !i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		goto ERROR0;

	/* Reserve the ISA region */
	if (is_isa)
		if (!request_region(address, IT87_EXTENT, name))
			goto ERROR0;

	/* Probe whether there is anything available on this address. Already
	   done for SMBus clients */
	if (kind < 0) {
		if (is_isa) {

#define REALLY_SLOW_IO
			/* We need the timeouts for at least some IT87-like chips. But only
			   if we read 'undefined' registers. */
			i = inb_p(address + 1);
			if (inb_p(address + 2) != i
			 || inb_p(address + 3) != i
			 || inb_p(address + 7) != i) {
		 		err = -ENODEV;
				goto ERROR1;
			}
#undef REALLY_SLOW_IO

			/* Let's just hope nothing breaks here */
			i = inb_p(address + 5) & 0x7f;
			outb_p(~i & 0x7f, address + 5);
			if ((inb_p(address + 5) & 0x7f) != (~i & 0x7f)) {
				outb_p(i, address + 5);
				err = -ENODEV;
				goto ERROR1;
			}
		}
	}

	/* OK. For now, we presume we have a valid client. We now create the
	   client structure, even though we cannot fill it completely yet.
	   But it allows us to access it87_{read,write}_value. */

	if (!(data = kmalloc(sizeof(struct it87_data), GFP_KERNEL))) {
		err = -ENOMEM;
		goto ERROR1;
	}
	memset(data, 0, sizeof(struct it87_data));

	new_client = &data->client;
	if (is_isa)
		init_MUTEX(&data->lock);
	i2c_set_clientdata(new_client, data);
	new_client->addr = address;
	new_client->adapter = adapter;
	new_client->driver = &it87_driver;
	new_client->flags = 0;

	/* Now, we do the remaining detection. */

	if (kind < 0) {
		if ((it87_read_value(new_client, IT87_REG_CONFIG) & 0x80)
		  || (!is_isa
		   && it87_read_value(new_client, IT87_REG_I2C_ADDR) != address)) {
		   	err = -ENODEV;
			goto ERROR2;
		}
	}

	/* Determine the chip type. */
	if (kind <= 0) {
		i = it87_read_value(new_client, IT87_REG_CHIPID);
		if (i == 0x90) {
			kind = it87;
		}
		else {
			if (kind == 0)
				dev_info(&adapter->dev, 
					"Ignoring 'force' parameter for unknown chip at "
					"adapter %d, address 0x%02x\n",
					i2c_adapter_id(adapter), address);
			err = -ENODEV;
			goto ERROR2;
		}
	}

	if (kind == it87) {
		name = "it87";
	}

	/* Fill in the remaining client fields and put it into the global list */
	strlcpy(new_client->name, name, I2C_NAME_SIZE);

	data->type = kind;

	new_client->id = it87_id++;
	data->valid = 0;
	init_MUTEX(&data->update_lock);

	/* Tell the I2C layer a new client has arrived */
	if ((err = i2c_attach_client(new_client)))
		goto ERROR2;

	/* Initialize the IT87 chip */
	it87_init_client(new_client, data);

	/* Register sysfs hooks */
	device_create_file(&new_client->dev, &dev_attr_in0_input);
	device_create_file(&new_client->dev, &dev_attr_in1_input);
	device_create_file(&new_client->dev, &dev_attr_in2_input);
	device_create_file(&new_client->dev, &dev_attr_in3_input);
	device_create_file(&new_client->dev, &dev_attr_in4_input);
	device_create_file(&new_client->dev, &dev_attr_in5_input);
	device_create_file(&new_client->dev, &dev_attr_in6_input);
	device_create_file(&new_client->dev, &dev_attr_in7_input);
	device_create_file(&new_client->dev, &dev_attr_in8_input);
	device_create_file(&new_client->dev, &dev_attr_in0_min);
	device_create_file(&new_client->dev, &dev_attr_in1_min);
	device_create_file(&new_client->dev, &dev_attr_in2_min);
	device_create_file(&new_client->dev, &dev_attr_in3_min);
	device_create_file(&new_client->dev, &dev_attr_in4_min);
	device_create_file(&new_client->dev, &dev_attr_in5_min);
	device_create_file(&new_client->dev, &dev_attr_in6_min);
	device_create_file(&new_client->dev, &dev_attr_in7_min);
	device_create_file(&new_client->dev, &dev_attr_in0_max);
	device_create_file(&new_client->dev, &dev_attr_in1_max);
	device_create_file(&new_client->dev, &dev_attr_in2_max);
	device_create_file(&new_client->dev, &dev_attr_in3_max);
	device_create_file(&new_client->dev, &dev_attr_in4_max);
	device_create_file(&new_client->dev, &dev_attr_in5_max);
	device_create_file(&new_client->dev, &dev_attr_in6_max);
	device_create_file(&new_client->dev, &dev_attr_in7_max);
	device_create_file(&new_client->dev, &dev_attr_temp1_input);
	device_create_file(&new_client->dev, &dev_attr_temp2_input);
	device_create_file(&new_client->dev, &dev_attr_temp3_input);
	device_create_file(&new_client->dev, &dev_attr_temp1_max);
	device_create_file(&new_client->dev, &dev_attr_temp2_max);
	device_create_file(&new_client->dev, &dev_attr_temp3_max);
	device_create_file(&new_client->dev, &dev_attr_temp1_min);
	device_create_file(&new_client->dev, &dev_attr_temp2_min);
	device_create_file(&new_client->dev, &dev_attr_temp3_min);
	device_create_file(&new_client->dev, &dev_attr_temp1_type);
	device_create_file(&new_client->dev, &dev_attr_temp2_type);
	device_create_file(&new_client->dev, &dev_attr_temp3_type);
	device_create_file(&new_client->dev, &dev_attr_fan1_input);
	device_create_file(&new_client->dev, &dev_attr_fan2_input);
	device_create_file(&new_client->dev, &dev_attr_fan3_input);
	device_create_file(&new_client->dev, &dev_attr_fan1_min);
	device_create_file(&new_client->dev, &dev_attr_fan2_min);
	device_create_file(&new_client->dev, &dev_attr_fan3_min);
	device_create_file(&new_client->dev, &dev_attr_fan1_div);
	device_create_file(&new_client->dev, &dev_attr_fan2_div);
	device_create_file(&new_client->dev, &dev_attr_fan3_div);
	device_create_file(&new_client->dev, &dev_attr_alarms);

	return 0;

ERROR2:
	kfree(data);
ERROR1:
	if (is_isa)
		release_region(address, IT87_EXTENT);
ERROR0:
	return err;
}

static int it87_detach_client(struct i2c_client *client)
{
	int err;

	if ((err = i2c_detach_client(client))) {
		dev_err(&client->dev,
			"Client deregistration failed, client not detached.\n");
		return err;
	}

	if(i2c_is_isa_client(client))
		release_region(client->addr, IT87_EXTENT);
	kfree(i2c_get_clientdata(client));

	return 0;
}

/* The SMBus locks itself, but ISA access must be locked explicitely! 
   We don't want to lock the whole ISA bus, so we lock each client
   separately.
   We ignore the IT87 BUSY flag at this moment - it could lead to deadlocks,
   would slow down the IT87 access and should not be necessary. */
static int it87_read_value(struct i2c_client *client, u8 reg)
{
	struct it87_data *data = i2c_get_clientdata(client);

	int res;
	if (i2c_is_isa_client(client)) {
		down(&data->lock);
		outb_p(reg, client->addr + IT87_ADDR_REG_OFFSET);
		res = inb_p(client->addr + IT87_DATA_REG_OFFSET);
		up(&data->lock);
		return res;
	} else
		return i2c_smbus_read_byte_data(client, reg);
}

/* The SMBus locks itself, but ISA access muse be locked explicitely! 
   We don't want to lock the whole ISA bus, so we lock each client
   separately.
   We ignore the IT87 BUSY flag at this moment - it could lead to deadlocks,
   would slow down the IT87 access and should not be necessary. */
static int it87_write_value(struct i2c_client *client, u8 reg, u8 value)
{
	struct it87_data *data = i2c_get_clientdata(client);

	if (i2c_is_isa_client(client)) {
		down(&data->lock);
		outb_p(reg, client->addr + IT87_ADDR_REG_OFFSET);
		outb_p(value, client->addr + IT87_DATA_REG_OFFSET);
		up(&data->lock);
		return 0;
	} else
		return i2c_smbus_write_byte_data(client, reg, value);
}

/* Called when we have found a new IT87. */
static void it87_init_client(struct i2c_client *client, struct it87_data *data)
{
	int tmp;

	if (reset) {
		/* Reset all except Watchdog values and last conversion values
		   This sets fan-divs to 2, among others */
		it87_write_value(client, IT87_REG_CONFIG, 0x80);
	}

	/* Check if temperature channnels are reset manually or by some reason */
	tmp = it87_read_value(client, IT87_REG_TEMP_ENABLE);
	if ((tmp & 0x3f) == 0) {
		/* Temp1,Temp3=thermistor; Temp2=thermal diode */
		tmp = (tmp & 0xc0) | 0x2a;
		it87_write_value(client, IT87_REG_TEMP_ENABLE, tmp);
	}
	data->sensor = tmp;

	/* Check if voltage monitors are reset manually or by some reason */
	tmp = it87_read_value(client, IT87_REG_VIN_ENABLE);
	if ((tmp & 0xff) == 0) {
		/* Enable all voltage monitors */
		it87_write_value(client, IT87_REG_VIN_ENABLE, 0xff);
	}

	/* Check if tachometers are reset manually or by some reason */
	tmp = it87_read_value(client, IT87_REG_FAN_MAIN_CTRL);
	if ((tmp & 0x70) == 0) {
		/* Enable all fan tachometers */
		tmp = (tmp & 0x8f) | 0x70;
		it87_write_value(client, IT87_REG_FAN_MAIN_CTRL, tmp);
	}

	/* Start monitoring */
	it87_write_value(client, IT87_REG_CONFIG,
			 (it87_read_value(client, IT87_REG_CONFIG) & 0x36)
			 | (update_vbat ? 0x41 : 0x01));
}

static struct it87_data *it87_update_device(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct it87_data *data = i2c_get_clientdata(client);
	int i;

	down(&data->update_lock);

	if ((jiffies - data->last_updated > HZ + HZ / 2) ||
	    (jiffies < data->last_updated) || !data->valid) {

		if (update_vbat) {
			/* Cleared after each update, so reenable.  Value
		 	  returned by this read will be previous value */	
			it87_write_value(client, IT87_REG_CONFIG,
			   it87_read_value(client, IT87_REG_CONFIG) | 0x40);
		}
		for (i = 0; i <= 7; i++) {
			data->in[i] =
			    it87_read_value(client, IT87_REG_VIN(i));
			data->in_min[i] =
			    it87_read_value(client, IT87_REG_VIN_MIN(i));
			data->in_max[i] =
			    it87_read_value(client, IT87_REG_VIN_MAX(i));
		}
		data->in[8] =
		    it87_read_value(client, IT87_REG_VIN(8));
		/* Temperature sensor doesn't have limit registers, set
		   to min and max value */
		data->in_min[8] = 0;
		data->in_max[8] = 255;

		for (i = 0; i < 3; i++) {
			data->fan[i] =
			    it87_read_value(client, IT87_REG_FAN(i));
			data->fan_min[i] =
			    it87_read_value(client, IT87_REG_FAN_MIN(i));
		}
		for (i = 0; i < 3; i++) {
			data->temp[i] =
			    it87_read_value(client, IT87_REG_TEMP(i));
			data->temp_high[i] =
			    it87_read_value(client, IT87_REG_TEMP_HIGH(i));
			data->temp_low[i] =
			    it87_read_value(client, IT87_REG_TEMP_LOW(i));
		}

		/* The 8705 does not have VID capability */
		data->vid = 0x1f;

		i = it87_read_value(client, IT87_REG_FAN_DIV);
		data->fan_div[0] = i & 0x07;
		data->fan_div[1] = (i >> 3) & 0x07;
		data->fan_div[2] = (i & 0x40) ? 3 : 1;

		data->alarms =
			it87_read_value(client, IT87_REG_ALARM1) |
			(it87_read_value(client, IT87_REG_ALARM2) << 8) |
			(it87_read_value(client, IT87_REG_ALARM3) << 16);

		data->sensor = it87_read_value(client, IT87_REG_TEMP_ENABLE);

		data->last_updated = jiffies;
		data->valid = 1;
	}

	up(&data->update_lock);

	return data;
}

static int __init sm_it87_init(void)
{
	int addr;

	if (!it87_find(&addr)) {
		normal_isa[0] = addr;
	}
	return i2c_add_driver(&it87_driver);
}

static void __exit sm_it87_exit(void)
{
	i2c_del_driver(&it87_driver);
}


MODULE_AUTHOR("Chris Gauthron <chrisg@0-in.com>");
MODULE_DESCRIPTION("IT8705F, IT8712F, Sis950 driver");
MODULE_PARM(update_vbat, "i");
MODULE_PARM_DESC(update_vbat, "Update vbat if set else return powerup value");
MODULE_PARM(reset, "i");
MODULE_PARM_DESC(reset, "Reset the chip's registers, default no");
MODULE_LICENSE("GPL");

module_init(sm_it87_init);
module_exit(sm_it87_exit);
