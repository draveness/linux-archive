/*
 * device driver for philips saa7134 based TV cards
 * i2c interface support
 *
 * (c) 2001,02 Gerd Knorr <kraxel@bytesex.org> [SuSE Labs]
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include "saa7134-reg.h"
#include "saa7134.h"

/* ----------------------------------------------------------- */

static unsigned int i2c_debug = 0;
MODULE_PARM(i2c_debug,"i");
MODULE_PARM_DESC(i2c_debug,"enable debug messages [i2c]");

static unsigned int i2c_scan = 0;
MODULE_PARM(i2c_scan,"i");
MODULE_PARM_DESC(i2c_scan,"scan i2c bus at insmod time");

#define d1printk if (1 == i2c_debug) printk
#define d2printk if (2 == i2c_debug) printk

#define I2C_WAIT_DELAY  32
#define I2C_WAIT_RETRY  16

/* ----------------------------------------------------------- */

static char *str_i2c_status[] = {
	"IDLE", "DONE_STOP", "BUSY", "TO_SCL", "TO_ARB", "DONE_WRITE",
	"DONE_READ", "DONE_WRITE_TO", "DONE_READ_TO", "NO_DEVICE",
	"NO_ACKN", "BUS_ERR", "ARB_LOST", "SEQ_ERR", "ST_ERR", "SW_ERR"
};

enum i2c_status {
	IDLE          = 0,  // no I2C command pending
	DONE_STOP     = 1,  // I2C command done and STOP executed
	BUSY          = 2,  // executing I2C command
	TO_SCL        = 3,  // executing I2C command, time out on clock stretching
	TO_ARB        = 4,  // time out on arbitration trial, still trying
	DONE_WRITE    = 5,  // I2C command done and awaiting next write command
	DONE_READ     = 6,  // I2C command done and awaiting next read command
	DONE_WRITE_TO = 7,  // see 5, and time out on status echo
	DONE_READ_TO  = 8,  // see 6, and time out on status echo
	NO_DEVICE     = 9,  // no acknowledge on device slave address
	NO_ACKN       = 10, // no acknowledge after data byte transfer
	BUS_ERR       = 11, // bus error
	ARB_LOST      = 12, // arbitration lost during transfer
	SEQ_ERR       = 13, // erroneous programming sequence
	ST_ERR        = 14, // wrong status echoing
	SW_ERR        = 15  // software error
};

static char *str_i2c_attr[] = {
	"NOP", "STOP", "CONTINUE", "START"
};

enum i2c_attr {
	NOP           = 0,  // no operation on I2C bus
	STOP          = 1,  // stop condition, no associated byte transfer
	CONTINUE      = 2,  // continue with byte transfer
	START         = 3   // start condition with byte transfer
};

static inline enum i2c_status i2c_get_status(struct saa7134_dev *dev)
{
	enum i2c_status status;
	
	status = saa_readb(SAA7134_I2C_ATTR_STATUS) & 0x0f;
	d2printk(KERN_DEBUG "%s: i2c stat <= %s\n",dev->name,
		 str_i2c_status[status]);
	return status;
}

static inline void i2c_set_status(struct saa7134_dev *dev,
				  enum i2c_status status)
{
	d2printk(KERN_DEBUG "%s: i2c stat => %s\n",dev->name,
		 str_i2c_status[status]);
	saa_andorb(SAA7134_I2C_ATTR_STATUS,0x0f,status);
}

static inline void i2c_set_attr(struct saa7134_dev *dev, enum i2c_attr attr)
{
	d2printk(KERN_DEBUG "%s: i2c attr => %s\n",dev->name,
		 str_i2c_attr[attr]);
	saa_andorb(SAA7134_I2C_ATTR_STATUS,0xc0,attr << 6);
}

static inline int i2c_is_error(enum i2c_status status)
{
	switch (status) {
	case NO_DEVICE:
	case NO_ACKN:
	case BUS_ERR:
	case ARB_LOST:
	case SEQ_ERR:
	case ST_ERR:
		return TRUE;
	default:
		return FALSE;
	}
}

static inline int i2c_is_idle(enum i2c_status status)
{
	switch (status) {
	case IDLE:
	case DONE_STOP:
		return TRUE;
	default:
		return FALSE;
	}
}

static inline int i2c_is_busy(enum i2c_status status)
{
	switch (status) {
	case BUSY:
		return TRUE;
	default:
		return FALSE;
	}
}

static int i2c_is_busy_wait(struct saa7134_dev *dev)
{
	enum i2c_status status;
	int count;

	for (count = 0; count < I2C_WAIT_RETRY; count++) {
		status = i2c_get_status(dev);
		if (!i2c_is_busy(status))
			break;
		saa_wait(I2C_WAIT_DELAY);
	}
	if (I2C_WAIT_RETRY == count)
		return FALSE;
	return TRUE;
}

static int i2c_reset(struct saa7134_dev *dev)
{
	enum i2c_status status;
	int count;

	d2printk(KERN_DEBUG "%s: i2c reset\n",dev->name);
	status = i2c_get_status(dev);
	if (!i2c_is_error(status))
		return TRUE;
	i2c_set_status(dev,status);

	for (count = 0; count < I2C_WAIT_RETRY; count++) {
		status = i2c_get_status(dev);
		if (!i2c_is_error(status))
			break;
		udelay(I2C_WAIT_DELAY);
	}
	if (I2C_WAIT_RETRY == count)
		return FALSE;

	if (!i2c_is_idle(status))
		return FALSE;
	
	i2c_set_attr(dev,NOP);
	return TRUE;
}

static inline int i2c_send_byte(struct saa7134_dev *dev,
				enum i2c_attr attr,
				unsigned char data)
{
	enum i2c_status status;
	__u32 dword;

#if 0
	i2c_set_attr(dev,attr);
	saa_writeb(SAA7134_I2C_DATA, data);
#else
	/* have to write both attr + data in one 32bit word */
	dword  = saa_readl(SAA7134_I2C_ATTR_STATUS >> 2);
	dword &= 0x0f;
	dword |= (attr << 6);
	dword |= ((__u32)data << 8);
	dword |= 0x00 << 16;
	dword |= 0xf0 << 24;
	saa_writel(SAA7134_I2C_ATTR_STATUS >> 2, dword);
#endif
	d2printk(KERN_DEBUG "%s: i2c data => 0x%x\n",dev->name,data);
	
	if (!i2c_is_busy_wait(dev))
		return -EIO;
	status = i2c_get_status(dev);
	if (i2c_is_error(status))
		return -EIO;
	return 0;
}

static inline int i2c_recv_byte(struct saa7134_dev *dev)
{
	enum i2c_status status;
	unsigned char data;
	
	i2c_set_attr(dev,CONTINUE);
	if (!i2c_is_busy_wait(dev))
		return -EIO;
	status = i2c_get_status(dev);
	if (i2c_is_error(status))
		return -EIO;
	data = saa_readb(SAA7134_I2C_DATA);
	d2printk(KERN_DEBUG "%s: i2c data <= 0x%x\n",dev->name,data);
	return data;
}

static int saa7134_i2c_xfer(struct i2c_adapter *i2c_adap,
			    struct i2c_msg msgs[], int num)
{
	struct saa7134_dev *dev = i2c_adap->algo_data;
	enum i2c_status status;
	unsigned char data;
	int addr,rc,i,byte;

  	status = i2c_get_status(dev);
	if (!i2c_is_idle(status))
		if (!i2c_reset(dev))
			return -EIO;

	d1printk(KERN_DEBUG "%s: i2c xfer:",dev->name);
	for (i = 0; i < num; i++) {
		if (!(msgs[i].flags & I2C_M_NOSTART) || 0 == i) {
			/* send address */
			addr  = msgs[i].addr << 1;
			if (msgs[i].flags & I2C_M_RD)
				addr |= 1;
			d1printk(" < %02x", addr);
			rc = i2c_send_byte(dev,START,addr);
			if (rc < 0)
				 goto err;
		}
		if (msgs[i].flags & I2C_M_RD) {
			/* read bytes */
			for (byte = 0; byte < msgs[i].len; byte++) {
				d1printk(" =");
				rc = i2c_recv_byte(dev);
				if (rc < 0)
					goto err;
				d1printk("%02x", rc);
				msgs[i].buf[byte] = rc;
			}
		} else {
			/* write bytes */
			for (byte = 0; byte < msgs[i].len; byte++) {
				data = msgs[i].buf[byte];
				d1printk(" %02x", data);
				rc = i2c_send_byte(dev,CONTINUE,data);
				if (rc < 0)
					goto err;
			}
		}
	}
	d1printk(" >");
	i2c_set_attr(dev,STOP);
	rc = -EIO;
	if (!i2c_is_busy_wait(dev))
		goto err;
  	status = i2c_get_status(dev);
	if (i2c_is_error(status))
		goto err;

	d1printk("\n");
	return num;
 err:
	if (1 == i2c_debug) {
		status = i2c_get_status(dev);
		printk(" ERROR: %s\n",str_i2c_status[status]);
	}
	return rc;
}

/* ----------------------------------------------------------- */

static int algo_control(struct i2c_adapter *adapter, 
			unsigned int cmd, unsigned long arg)
{
	return 0;
}

static u32 functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_SMBUS_EMUL;
}

#ifndef I2C_PEC
static void inc_use(struct i2c_adapter *adap)
{
	MOD_INC_USE_COUNT;
}

static void dec_use(struct i2c_adapter *adap)
{
	MOD_DEC_USE_COUNT;
}
#endif

static int attach_inform(struct i2c_client *client)
{
        struct saa7134_dev *dev = client->adapter->algo_data;
	int tuner = dev->tuner_type;

	saa7134_i2c_call_clients(dev,TUNER_SET_TYPE,&tuner);
        return 0;
}

static struct i2c_algorithm saa7134_algo = {
	.name          = "saa7134",
	.id            = I2C_ALGO_SAA7134,
	.master_xfer   = saa7134_i2c_xfer,
	.algo_control  = algo_control,
	.functionality = functionality,
};

static struct i2c_adapter saa7134_adap_template = {
#ifdef I2C_PEC
	.owner         = THIS_MODULE,
#else
	.inc_use       = inc_use,
	.dec_use       = dec_use,
#endif
#ifdef I2C_CLASS_TV_ANALOG
	.class         = I2C_CLASS_TV_ANALOG,
#endif
	I2C_DEVNAME("saa7134"),
	.id            = I2C_ALGO_SAA7134,
	.algo          = &saa7134_algo,
	.client_register = attach_inform,
};

static struct i2c_client saa7134_client_template = {
	I2C_DEVNAME("saa7134 internal"),
        .id        = -1,
};

/* ----------------------------------------------------------- */

static int
saa7134_i2c_eeprom(struct saa7134_dev *dev, unsigned char *eedata, int len)
{
	unsigned char buf;
	int i,err;

	dev->i2c_client.addr = 0xa0 >> 1;
	buf = 0;
	if (1 != (err = i2c_master_send(&dev->i2c_client,&buf,1))) {
		printk(KERN_INFO "%s: Huh, no eeprom present (err=%d)?\n",
		       dev->name,err);
		return -1;
	}
	if (len != (err = i2c_master_recv(&dev->i2c_client,eedata,len))) {
		printk(KERN_WARNING "%s: i2c eeprom read error (err=%d)\n",
		       dev->name,err);
		return -1;
	}
	for (i = 0; i < len; i++) {
		if (0 == (i % 16))
			printk(KERN_INFO "%s: i2c eeprom %02x:",dev->name,i);
		printk(" %02x",eedata[i]);
		if (15 == (i % 16))
			printk("\n");
	}
	return 0;
}

static int
saa7134_i2c_scan(struct saa7134_dev *dev)
{
	unsigned char buf;
	int i,rc;

	for (i = 0; i < 256; i+= 2) {
		dev->i2c_client.addr = i >> 1;
		rc = i2c_master_recv(&dev->i2c_client,&buf,0);
		if (rc < 0)
			continue;
		printk("%s: i2c scan: found device @ %x%s\n",
		       dev->name, i, (i == 0xa0) ? " [eeprom]" : "");
	}
	return 0;
}

void saa7134_i2c_call_clients(struct saa7134_dev *dev,
			      unsigned int cmd, void *arg)
{
	BUG_ON(NULL == dev->i2c_adap.algo_data);
	i2c_clients_command(&dev->i2c_adap, cmd, arg);
}

int saa7134_i2c_register(struct saa7134_dev *dev)
{
	dev->i2c_adap = saa7134_adap_template;
	dev->i2c_adap.dev.parent = &dev->pci->dev;
	strcpy(dev->i2c_adap.name,dev->name);
	dev->i2c_adap.algo_data = dev;
	i2c_add_adapter(&dev->i2c_adap);
	
	dev->i2c_client = saa7134_client_template;
	dev->i2c_client.adapter = &dev->i2c_adap;
	
	saa7134_i2c_eeprom(dev,dev->eedata,sizeof(dev->eedata));
	if (i2c_scan)
		saa7134_i2c_scan(dev);
	return 0;
}

int saa7134_i2c_unregister(struct saa7134_dev *dev)
{
	i2c_del_adapter(&dev->i2c_adap);
	return 0;
}

/* ----------------------------------------------------------- */
/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
