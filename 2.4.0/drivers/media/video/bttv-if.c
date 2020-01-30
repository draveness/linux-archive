/*
    bttv-if.c  --  interfaces to other kernel modules
	all the i2c code is here
	also the gpio interface exported by bttv (used by lirc)

    bttv - Bt848 frame grabber driver

    Copyright (C) 1996,97,98 Ralph  Metzler (rjkm@thp.uni-koeln.de)
                           & Marcus Metzler (mocm@thp.uni-koeln.de)
    (c) 1999,2000 Gerd Knorr <kraxel@goldbach.in-berlin.de>

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

#define __NO_VERSION__ 1

#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>

#include <asm/io.h>

#include "bttvp.h"
#include "tuner.h"


EXPORT_SYMBOL(bttv_get_cardinfo);
EXPORT_SYMBOL(bttv_get_id);
EXPORT_SYMBOL(bttv_gpio_enable);
EXPORT_SYMBOL(bttv_read_gpio);
EXPORT_SYMBOL(bttv_write_gpio);
EXPORT_SYMBOL(bttv_get_gpio_queue);

/* ----------------------------------------------------------------------- */
/* Exported functions - for other modules which want to access the         */
/*                      gpio ports (IR for example)                        */
/*                      see bttv.h for comments                            */

int bttv_get_cardinfo(unsigned int card, int *type, int *cardid)
{
	if (card >= bttv_num) {
		return -1;
	}
	*type   = bttvs[card].type;
	*cardid = bttvs[card].cardid;
	return 0;
}

int bttv_get_id(unsigned int card)
{
	printk("bttv_get_id is obsolete, use bttv_get_cardinfo instead\n");
	if (card >= bttv_num) {
		return -1;
	}
	return bttvs[card].type;
}

int bttv_gpio_enable(unsigned int card, unsigned long mask, unsigned long data)
{
	struct bttv *btv;

	if (card >= bttv_num) {
		return -EINVAL;
	}
	
	btv = &bttvs[card];
	btaor(data, ~mask, BT848_GPIO_OUT_EN);
	if (bttv_gpio)
		bttv_gpio_tracking(btv,"extern enable");
	return 0;
}

int bttv_read_gpio(unsigned int card, unsigned long *data)
{
	struct bttv *btv;
	
	if (card >= bttv_num) {
		return -EINVAL;
	}

	btv = &bttvs[card];

	if(btv->shutdown) {
		return -ENODEV;
	}

/* prior setting BT848_GPIO_REG_INP is (probably) not needed 
   because we set direct input on init */
	*data = btread(BT848_GPIO_DATA);
	return 0;
}

int bttv_write_gpio(unsigned int card, unsigned long mask, unsigned long data)
{
	struct bttv *btv;
	
	if (card >= bttv_num) {
		return -EINVAL;
	}

	btv = &bttvs[card];

/* prior setting BT848_GPIO_REG_INP is (probably) not needed 
   because direct input is set on init */
	btaor(data & mask, ~mask, BT848_GPIO_DATA);
	if (bttv_gpio)
		bttv_gpio_tracking(btv,"extern write");
	return 0;
}

wait_queue_head_t* bttv_get_gpio_queue(unsigned int card)
{
	struct bttv *btv;

	if (card >= bttv_num) {
		return NULL;
	}

	btv = &bttvs[card];
	if (bttvs[card].shutdown) {
		return NULL;
	}
	return &btv->gpioq;
}


/* ----------------------------------------------------------------------- */
/* I2C functions                                                           */

void bttv_bit_setscl(void *data, int state)
{
	struct bttv *btv = (struct bttv*)data;

	if (state)
		btv->i2c_state |= 0x02;
	else
		btv->i2c_state &= ~0x02;
	btwrite(btv->i2c_state, BT848_I2C);
	btread(BT848_I2C);
}

void bttv_bit_setsda(void *data, int state)
{
	struct bttv *btv = (struct bttv*)data;

	if (state)
		btv->i2c_state |= 0x01;
	else
		btv->i2c_state &= ~0x01;
	btwrite(btv->i2c_state, BT848_I2C);
	btread(BT848_I2C);
}

static int bttv_bit_getscl(void *data)
{
	struct bttv *btv = (struct bttv*)data;
	int state;
	
	state = btread(BT848_I2C) & 0x02 ? 1 : 0;
	return state;
}

static int bttv_bit_getsda(void *data)
{
	struct bttv *btv = (struct bttv*)data;
	int state;

	state = btread(BT848_I2C) & 0x01;
	return state;
}

static void bttv_inc_use(struct i2c_adapter *adap)
{
	MOD_INC_USE_COUNT;
}

static void bttv_dec_use(struct i2c_adapter *adap)
{
	MOD_DEC_USE_COUNT;
}

static int attach_inform(struct i2c_client *client)
{
        struct bttv *btv = (struct bttv*)client->adapter->data;
	int i;

	for (i = 0; i < I2C_CLIENTS_MAX; i++) {
		if (btv->i2c_clients[i] == NULL) {
			btv->i2c_clients[i] = client;
			break;
		}
	}
	if (btv->tuner_type != -1)
		bttv_call_i2c_clients(btv,TUNER_SET_TYPE,&btv->tuner_type);
        if (bttv_verbose)
		printk("bttv%d: i2c attach [%s]\n",btv->nr,client->name);
        return 0;
}

static int detach_inform(struct i2c_client *client)
{
        struct bttv *btv = (struct bttv*)client->adapter->data;
	int i;

        if (bttv_verbose)
		printk("bttv%d: i2c detach [%s]\n",btv->nr,client->name);
	for (i = 0; i < I2C_CLIENTS_MAX; i++) {
		if (btv->i2c_clients[i] == client) {
			btv->i2c_clients[i] = NULL;
			break;
		}
	}
        return 0;
}

void bttv_call_i2c_clients(struct bttv *btv, unsigned int cmd, void *arg)
{
	int i;
	
	for (i = 0; i < I2C_CLIENTS_MAX; i++) {
		if (NULL == btv->i2c_clients[i])
			continue;
		if (NULL == btv->i2c_clients[i]->driver->command)
			continue;
		btv->i2c_clients[i]->driver->command(
			btv->i2c_clients[i],cmd,arg);
	}
}

struct i2c_algo_bit_data bttv_i2c_algo_template = {
	setsda:  bttv_bit_setsda,
	setscl:  bttv_bit_setscl,
	getsda:  bttv_bit_getsda,
	getscl:  bttv_bit_getscl,
	udelay:  40,
	mdelay:  10,
	timeout: 200,
};

struct i2c_adapter bttv_i2c_adap_template = {
	name:              "bt848",
	id:                I2C_HW_B_BT848,
	inc_use:           bttv_inc_use,
	dec_use:           bttv_dec_use,
	client_register:   attach_inform,
	client_unregister: detach_inform,
};

struct i2c_client bttv_i2c_client_template = {
        name: "bttv internal use only",
        id:   -1,
};


/* read I2C */
int bttv_I2CRead(struct bttv *btv, unsigned char addr, char *probe_for) 
{
        unsigned char buffer = 0;

	if (0 != btv->i2c_rc)
		return -1;
	if (bttv_verbose && NULL != probe_for)
		printk(KERN_INFO "bttv%d: i2c: checking for %s @ 0x%02x... ",
		       btv->nr,probe_for,addr);
        btv->i2c_client.addr = addr >> 1;
        if (1 != i2c_master_recv(&btv->i2c_client, &buffer, 1)) {
		if (NULL != probe_for) {
			if (bttv_verbose)
				printk("not found\n");
		} else
			printk(KERN_WARNING "bttv%d: i2c read 0x%x: error\n",
			       btv->nr,addr);
                return -1;
	}
	if (bttv_verbose && NULL != probe_for)
		printk("found\n");
        return buffer;
}

/* write I2C */
int bttv_I2CWrite(struct bttv *btv, unsigned char addr, unsigned char b1,
                    unsigned char b2, int both)
{
        unsigned char buffer[2];
        int bytes = both ? 2 : 1;

	if (0 != btv->i2c_rc)
		return -1;
        btv->i2c_client.addr = addr >> 1;
        buffer[0] = b1;
        buffer[1] = b2;
        if (bytes != i2c_master_send(&btv->i2c_client, buffer, bytes))
		return -1;
        return 0;
}

/* read EEPROM content */
void __devinit bttv_readee(struct bttv *btv, unsigned char *eedata, int addr)
{
	int i;
        
	if (bttv_I2CWrite(btv, addr, 0, -1, 0)<0) {
		printk(KERN_WARNING "bttv: readee error\n");
		return;
	}
	btv->i2c_client.addr = addr >> 1;
	for (i=0; i<256; i+=16) {
		if (16 != i2c_master_recv(&btv->i2c_client,eedata+i,16)) {
			printk(KERN_WARNING "bttv: readee error\n");
			break;
		}
	}
}

/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
