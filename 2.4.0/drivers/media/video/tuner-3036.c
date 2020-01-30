/*
 * Driver for Philips SAB3036 "CITAC" tuner control chip.
 *
 * Author: Phil Blundell <philb@gnu.org>
 *
 * The SAB3036 is just about different enough from the chips that
 * tuner.c copes with to make it not worth the effort to crowbar
 * the support into that file.  So instead we have a separate driver. 
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/malloc.h>
#include <linux/version.h>
#include <linux/init.h>

#include <linux/i2c.h>
#include <linux/videodev.h>

#include "tuner.h"

static int debug;	/* insmod parameter */
static int this_adap;

static struct i2c_client client_template;

/* Addresses to scan */
static unsigned short normal_i2c[] = {I2C_CLIENT_END};
static unsigned short normal_i2c_range[] = {0x60, 0x61, I2C_CLIENT_END};
static unsigned short probe[2]        = { I2C_CLIENT_END, I2C_CLIENT_END };
static unsigned short probe_range[2]  = { I2C_CLIENT_END, I2C_CLIENT_END };
static unsigned short ignore[2]       = { I2C_CLIENT_END, I2C_CLIENT_END };
static unsigned short ignore_range[2] = { I2C_CLIENT_END, I2C_CLIENT_END };
static unsigned short force[2]        = { I2C_CLIENT_END, I2C_CLIENT_END };

static struct i2c_client_address_data addr_data = {
	normal_i2c, normal_i2c_range, 
	probe, probe_range, 
	ignore, ignore_range, 
	force
};

/* ---------------------------------------------------------------------- */

static unsigned char
tuner_getstatus (struct i2c_client *c)
{
	unsigned char byte;
	if (i2c_master_recv(c, &byte, 1) != 1)
		printk(KERN_ERR "tuner-3036: I/O error.\n");
	return byte;
}

#define TUNER_FL        0x80

static int 
tuner_islocked (struct i2c_client *c)
{
        return (tuner_getstatus(c) & TUNER_FL);
}

/* ---------------------------------------------------------------------- */

static void 
set_tv_freq(struct i2c_client *c, int freq)
{
	u16 div = ((freq * 20) / 16);
	unsigned long give_up = jiffies + HZ;
	unsigned char buffer[2];

	if (debug)
		printk(KERN_DEBUG "tuner: setting frequency %dMHz, divisor %x\n", freq / 16, div);
	
	/* Select high tuning current */
	buffer[0] = 0x29;
	buffer[1] = 0x3e;

	if (i2c_master_send(c, buffer, 2) != 2)
		printk("tuner: i2c i/o error 1\n");
	
	buffer[0] = 0x80 | ((div>>8) & 0x7f);
	buffer[1] = div & 0xff;

	if (i2c_master_send(c, buffer, 2) != 2)
		printk("tuner: i2c i/o error 2\n");
	
	while (!tuner_islocked(c) && time_before(jiffies, give_up))
		schedule();
	       
	if (!tuner_islocked(c))
		printk(KERN_WARNING "tuner: failed to achieve PLL lock\n");
	
	/* Select low tuning current and engage AFC */
	buffer[0] = 0x29;
	buffer[1] = 0xb2;

	if (i2c_master_send(c, buffer, 2) != 2)
		printk("tuner: i2c i/o error 3\n");

	if (debug)
		printk(KERN_DEBUG "tuner: status %02x\n", tuner_getstatus(c));
}

/* ---------------------------------------------------------------------- */

static int 
tuner_attach(struct i2c_adapter *adap, int addr,
	     unsigned short flags, int kind)
{
	static unsigned char buffer[] = { 0x29, 0x32, 0x2a, 0, 0x2b, 0 };

	struct i2c_client *client;

	if (this_adap > 0)
		return -1;
	this_adap++;
	
        client_template.adapter = adap;
        client_template.addr = addr;

	client = kmalloc(sizeof(struct i2c_client), GFP_KERNEL);
        if (client == NULL)
                return -ENOMEM;
        memcpy(client, &client_template, sizeof(struct i2c_client));

	printk("tuner: SAB3036 found, status %02x\n", tuner_getstatus(client));

        i2c_attach_client(client);
	MOD_INC_USE_COUNT;

	if (i2c_master_send(client, buffer, 2) != 2)
		printk("tuner: i2c i/o error 1\n");
	if (i2c_master_send(client, buffer+2, 2) != 2)
		printk("tuner: i2c i/o error 2\n");
	if (i2c_master_send(client, buffer+4, 2) != 2)
		printk("tuner: i2c i/o error 3\n");
	return 0;
}

static int 
tuner_detach(struct i2c_client *c)
{
	MOD_DEC_USE_COUNT;
	return 0;
}

static int 
tuner_command(struct i2c_client *client, unsigned int cmd, void *arg)
{
	int *iarg = (int*)arg;

	switch (cmd) 
	{
		case TUNER_SET_TVFREQ:
			set_tv_freq(client, *iarg);
			break;
	    
		default:
			return -EINVAL;
	}
	return 0;
}

static int 
tuner_probe(struct i2c_adapter *adap)
{
	this_adap = 0;
	if (adap->id == (I2C_ALGO_BIT | I2C_HW_B_LP))
		return i2c_probe(adap, &addr_data, tuner_attach);
	return 0;
}

/* ----------------------------------------------------------------------- */

static struct i2c_driver 
i2c_driver_tuner = 
{
	"sab3036",		/* name       */
	I2C_DRIVERID_SAB3036,	/* ID         */
        I2C_DF_NOTIFY,
	tuner_probe,
	tuner_detach,
	tuner_command
};

static struct i2c_client client_template =
{
        "SAB3036",		/* name       */
        -1,
        0,
        0,
        NULL,
        &i2c_driver_tuner
};

EXPORT_NO_SYMBOLS;

int __init
tuner3036_init(void)
{
	i2c_add_driver(&i2c_driver_tuner);
	return 0;
}

void __exit
tuner3036_exit(void)
{
	i2c_del_driver(&i2c_driver_tuner);
}

MODULE_DESCRIPTION("SAB3036 tuner driver");
MODULE_AUTHOR("Philip Blundell <philb@gnu.org>");
MODULE_PARM(debug,"i");
MODULE_PARM_DESC(debug,"Enable debugging output");

module_init(tuner3036_init);
module_exit(tuner3036_exit);
