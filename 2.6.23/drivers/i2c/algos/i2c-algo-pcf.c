/* ------------------------------------------------------------------------- */
/* i2c-algo-pcf.c i2c driver algorithms for PCF8584 adapters		     */
/* ------------------------------------------------------------------------- */
/*   Copyright (C) 1995-1997 Simon G. Vogl
                   1998-2000 Hans Berglund

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
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.		     */
/* ------------------------------------------------------------------------- */

/* With some changes from Ky�sti M�lkki <kmalkki@cc.hut.fi> and 
   Frodo Looijaard <frodol@dds.nl> ,and also from Martin Bailey
   <mbailey@littlefeet-inc.com> */

/* Partially rewriten by Oleg I. Vdovikin <vdovikin@jscc.ru> to handle multiple
   messages, proper stop/repstart signaling during receive,
   added detect code */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-pcf.h>
#include "i2c-algo-pcf.h"


#define DEB2(x) if (i2c_debug>=2) x
#define DEB3(x) if (i2c_debug>=3) x /* print several statistical values*/
#define DEBPROTO(x) if (i2c_debug>=9) x;
 	/* debug the protocol by showing transferred bits */
#define DEF_TIMEOUT 16

/* module parameters:
 */
static int i2c_debug;

/* --- setting states on the bus with the right timing: ---------------	*/

#define set_pcf(adap, ctl, val) adap->setpcf(adap->data, ctl, val)
#define get_pcf(adap, ctl) adap->getpcf(adap->data, ctl)
#define get_own(adap) adap->getown(adap->data)
#define get_clock(adap) adap->getclock(adap->data)
#define i2c_outb(adap, val) adap->setpcf(adap->data, 0, val)
#define i2c_inb(adap) adap->getpcf(adap->data, 0)

/* --- other auxiliary functions --------------------------------------	*/

static void i2c_start(struct i2c_algo_pcf_data *adap) 
{
	DEBPROTO(printk("S "));
	set_pcf(adap, 1, I2C_PCF_START);
}

static void i2c_repstart(struct i2c_algo_pcf_data *adap) 
{
	DEBPROTO(printk(" Sr "));
	set_pcf(adap, 1, I2C_PCF_REPSTART);
}


static void i2c_stop(struct i2c_algo_pcf_data *adap) 
{
	DEBPROTO(printk("P\n"));
	set_pcf(adap, 1, I2C_PCF_STOP);
}

static int wait_for_bb(struct i2c_algo_pcf_data *adap) {

	int timeout = DEF_TIMEOUT;
	int status;

	status = get_pcf(adap, 1);
#ifndef STUB_I2C
	while (timeout-- && !(status & I2C_PCF_BB)) {
		udelay(100); /* wait for 100 us */
		status = get_pcf(adap, 1);
	}
#endif
	if (timeout <= 0) {
		printk(KERN_ERR "Timeout waiting for Bus Busy\n");
	}
	
	return (timeout<=0);
}


static int wait_for_pin(struct i2c_algo_pcf_data *adap, int *status) {

	int timeout = DEF_TIMEOUT;

	*status = get_pcf(adap, 1);
#ifndef STUB_I2C
	while (timeout-- && (*status & I2C_PCF_PIN)) {
		adap->waitforpin();
		*status = get_pcf(adap, 1);
	}
	if (*status & I2C_PCF_LAB) {
		DEB2(printk(KERN_INFO 
			"i2c-algo-pcf.o: lost arbitration (CSR 0x%02x)\n",
			 *status));
		/* Cleanup from LAB-- reset and enable ESO.
		 * This resets the PCF8584; since we've lost the bus, no
		 * further attempts should be made by callers to clean up 
		 * (no i2c_stop() etc.)
		 */
		set_pcf(adap, 1, I2C_PCF_PIN);
		set_pcf(adap, 1, I2C_PCF_ESO);
		/* TODO: we should pause for a time period sufficient for any
		 * running I2C transaction to complete-- the arbitration
		 * logic won't work properly until the next START is seen.
		 */
		DEB2(printk(KERN_INFO 
			"i2c-algo-pcf.o: reset LAB condition (CSR 0x%02x)\n", 
			get_pcf(adap,1)));
		return(-EINTR);
	}
#endif
	if (timeout <= 0)
		return(-1);
	else
		return(0);
}

/* 
 * This should perform the 'PCF8584 initialization sequence' as described
 * in the Philips IC12 data book (1995, Aug 29).
 * There should be a 30 clock cycle wait after reset, I assume this
 * has been fulfilled.
 * There should be a delay at the end equal to the longest I2C message
 * to synchronize the BB-bit (in multimaster systems). How long is
 * this? I assume 1 second is always long enough.
 *
 * vdovikin: added detect code for PCF8584
 */
static int pcf_init_8584 (struct i2c_algo_pcf_data *adap)
{
	unsigned char temp;

	DEB3(printk(KERN_DEBUG "i2c-algo-pcf.o: PCF state 0x%02x\n", get_pcf(adap, 1)));

	/* S1=0x80: S0 selected, serial interface off */
	set_pcf(adap, 1, I2C_PCF_PIN);
	/* check to see S1 now used as R/W ctrl -
	   PCF8584 does that when ESO is zero */
	if (((temp = get_pcf(adap, 1)) & 0x7f) != (0)) {
		DEB2(printk(KERN_ERR "i2c-algo-pcf.o: PCF detection failed -- can't select S0 (0x%02x).\n", temp));
		return -ENXIO; /* definetly not PCF8584 */
	}

	/* load own address in S0, effective address is (own << 1)	*/
	i2c_outb(adap, get_own(adap));
	/* check it's really written */
	if ((temp = i2c_inb(adap)) != get_own(adap)) {
		DEB2(printk(KERN_ERR "i2c-algo-pcf.o: PCF detection failed -- can't set S0 (0x%02x).\n", temp));
		return -ENXIO;
	}

	/* S1=0xA0, next byte in S2					*/
	set_pcf(adap, 1, I2C_PCF_PIN | I2C_PCF_ES1);
	/* check to see S2 now selected */
	if (((temp = get_pcf(adap, 1)) & 0x7f) != I2C_PCF_ES1) {
		DEB2(printk(KERN_ERR "i2c-algo-pcf.o: PCF detection failed -- can't select S2 (0x%02x).\n", temp));
		return -ENXIO;
	}

	/* load clock register S2					*/
	i2c_outb(adap, get_clock(adap));
	/* check it's really written, the only 5 lowest bits does matter */
	if (((temp = i2c_inb(adap)) & 0x1f) != get_clock(adap)) {
		DEB2(printk(KERN_ERR "i2c-algo-pcf.o: PCF detection failed -- can't set S2 (0x%02x).\n", temp));
		return -ENXIO;
	}

	/* Enable serial interface, idle, S0 selected			*/
	set_pcf(adap, 1, I2C_PCF_IDLE);

	/* check to see PCF is really idled and we can access status register */
	if ((temp = get_pcf(adap, 1)) != (I2C_PCF_PIN | I2C_PCF_BB)) {
		DEB2(printk(KERN_ERR "i2c-algo-pcf.o: PCF detection failed -- can't select S1` (0x%02x).\n", temp));
		return -ENXIO;
	}
	
	printk(KERN_DEBUG "i2c-algo-pcf.o: deteted and initialized PCF8584.\n");

	return 0;
}


/* ----- Utility functions
 */

static inline int try_address(struct i2c_algo_pcf_data *adap,
		       unsigned char addr, int retries)
{
	int i, status, ret = -1;
	int wfp;
	for (i=0;i<retries;i++) {
		i2c_outb(adap, addr);
		i2c_start(adap);
		status = get_pcf(adap, 1);
		if ((wfp = wait_for_pin(adap, &status)) >= 0) {
			if ((status & I2C_PCF_LRB) == 0) { 
				i2c_stop(adap);
				break;	/* success! */
			}
		}
		if (wfp == -EINTR) {
			/* arbitration lost */
			udelay(adap->udelay);
			return -EINTR;
		}
		i2c_stop(adap);
		udelay(adap->udelay);
	}
	DEB2(if (i) printk(KERN_DEBUG "i2c-algo-pcf.o: needed %d retries for %d\n",i,
	                   addr));
	return ret;
}


static int pcf_sendbytes(struct i2c_adapter *i2c_adap, const char *buf,
                         int count, int last)
{
	struct i2c_algo_pcf_data *adap = i2c_adap->algo_data;
	int wrcount, status, timeout;
    
	for (wrcount=0; wrcount<count; ++wrcount) {
		DEB2(dev_dbg(&i2c_adap->dev, "i2c_write: writing %2.2X\n",
				buf[wrcount]&0xff));
		i2c_outb(adap, buf[wrcount]);
		timeout = wait_for_pin(adap, &status);
		if (timeout) {
			if (timeout == -EINTR) {
				/* arbitration lost */
				return -EINTR;
			}
			i2c_stop(adap);
			dev_err(&i2c_adap->dev, "i2c_write: error - timeout.\n");
			return -EREMOTEIO; /* got a better one ?? */
		}
#ifndef STUB_I2C
		if (status & I2C_PCF_LRB) {
			i2c_stop(adap);
			dev_err(&i2c_adap->dev, "i2c_write: error - no ack.\n");
			return -EREMOTEIO; /* got a better one ?? */
		}
#endif
	}
	if (last) {
		i2c_stop(adap);
	}
	else {
		i2c_repstart(adap);
	}

	return (wrcount);
}


static int pcf_readbytes(struct i2c_adapter *i2c_adap, char *buf,
                         int count, int last)
{
	int i, status;
	struct i2c_algo_pcf_data *adap = i2c_adap->algo_data;
	int wfp;

	/* increment number of bytes to read by one -- read dummy byte */
	for (i = 0; i <= count; i++) {

		if ((wfp = wait_for_pin(adap, &status))) {
			if (wfp == -EINTR) {
				/* arbitration lost */
				return -EINTR;
			}
			i2c_stop(adap);
			dev_err(&i2c_adap->dev, "pcf_readbytes timed out.\n");
			return (-1);
		}

#ifndef STUB_I2C
		if ((status & I2C_PCF_LRB) && (i != count)) {
			i2c_stop(adap);
			dev_err(&i2c_adap->dev, "i2c_read: i2c_inb, No ack.\n");
			return (-1);
		}
#endif
		
		if (i == count - 1) {
			set_pcf(adap, 1, I2C_PCF_ESO);
		} else 
		if (i == count) {
			if (last) {
				i2c_stop(adap);
			} else {
				i2c_repstart(adap);
			}
		};

		if (i) {
			buf[i - 1] = i2c_inb(adap);
		} else {
			i2c_inb(adap); /* dummy read */
		}
	}

	return (i - 1);
}


static inline int pcf_doAddress(struct i2c_algo_pcf_data *adap,
                                struct i2c_msg *msg, int retries) 
{
	unsigned short flags = msg->flags;
	unsigned char addr;
	int ret;
	if ( (flags & I2C_M_TEN)  ) { 
		/* a ten bit address */
		addr = 0xf0 | (( msg->addr >> 7) & 0x03);
		DEB2(printk(KERN_DEBUG "addr0: %d\n",addr));
		/* try extended address code...*/
		ret = try_address(adap, addr, retries);
		if (ret!=1) {
			printk(KERN_ERR "died at extended address code.\n");
			return -EREMOTEIO;
		}
		/* the remaining 8 bit address */
		i2c_outb(adap,msg->addr & 0x7f);
/* Status check comes here */
		if (ret != 1) {
			printk(KERN_ERR "died at 2nd address code.\n");
			return -EREMOTEIO;
		}
		if ( flags & I2C_M_RD ) {
			i2c_repstart(adap);
			/* okay, now switch into reading mode */
			addr |= 0x01;
			ret = try_address(adap, addr, retries);
			if (ret!=1) {
				printk(KERN_ERR "died at extended address code.\n");
				return -EREMOTEIO;
			}
		}
	} else {		/* normal 7bit address	*/
		addr = ( msg->addr << 1 );
		if (flags & I2C_M_RD )
			addr |= 1;
		if (flags & I2C_M_REV_DIR_ADDR )
			addr ^= 1;
		i2c_outb(adap, addr);
	}
	return 0;
}

static int pcf_xfer(struct i2c_adapter *i2c_adap,
		    struct i2c_msg *msgs, 
		    int num)
{
	struct i2c_algo_pcf_data *adap = i2c_adap->algo_data;
	struct i2c_msg *pmsg;
	int i;
	int ret=0, timeout, status;
    

	/* Check for bus busy */
	timeout = wait_for_bb(adap);
	if (timeout) {
		DEB2(printk(KERN_ERR "i2c-algo-pcf.o: "
		            "Timeout waiting for BB in pcf_xfer\n");)
		return -EIO;
	}
	
	for (i = 0;ret >= 0 && i < num; i++) {
		pmsg = &msgs[i];

		DEB2(printk(KERN_DEBUG "i2c-algo-pcf.o: Doing %s %d bytes to 0x%02x - %d of %d messages\n",
		     pmsg->flags & I2C_M_RD ? "read" : "write",
                     pmsg->len, pmsg->addr, i + 1, num);)
    
		ret = pcf_doAddress(adap, pmsg, i2c_adap->retries);

		/* Send START */
		if (i == 0) {
			i2c_start(adap); 
		}
    
		/* Wait for PIN (pending interrupt NOT) */
		timeout = wait_for_pin(adap, &status);
		if (timeout) {
			if (timeout == -EINTR) {
				/* arbitration lost */
				return (-EINTR);
			}
			i2c_stop(adap);
			DEB2(printk(KERN_ERR "i2c-algo-pcf.o: Timeout waiting "
				    "for PIN(1) in pcf_xfer\n");)
			return (-EREMOTEIO);
		}
    
#ifndef STUB_I2C
		/* Check LRB (last rcvd bit - slave ack) */
		if (status & I2C_PCF_LRB) {
			i2c_stop(adap);
			DEB2(printk(KERN_ERR "i2c-algo-pcf.o: No LRB(1) in pcf_xfer\n");)
			return (-EREMOTEIO);
		}
#endif
    
		DEB3(printk(KERN_DEBUG "i2c-algo-pcf.o: Msg %d, addr=0x%x, flags=0x%x, len=%d\n",
			    i, msgs[i].addr, msgs[i].flags, msgs[i].len);)
    
		/* Read */
		if (pmsg->flags & I2C_M_RD) {
			/* read bytes into buffer*/
			ret = pcf_readbytes(i2c_adap, pmsg->buf, pmsg->len,
                                            (i + 1 == num));
        
			if (ret != pmsg->len) {
				DEB2(printk(KERN_DEBUG "i2c-algo-pcf.o: fail: "
					    "only read %d bytes.\n",ret));
			} else {
				DEB2(printk(KERN_DEBUG "i2c-algo-pcf.o: read %d bytes.\n",ret));
			}
		} else { /* Write */
			ret = pcf_sendbytes(i2c_adap, pmsg->buf, pmsg->len,
                                            (i + 1 == num));
        
			if (ret != pmsg->len) {
				DEB2(printk(KERN_DEBUG "i2c-algo-pcf.o: fail: "
					    "only wrote %d bytes.\n",ret));
			} else {
				DEB2(printk(KERN_DEBUG "i2c-algo-pcf.o: wrote %d bytes.\n",ret));
			}
		}
	}

	return (i);
}

static u32 pcf_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL | 
	       I2C_FUNC_10BIT_ADDR | I2C_FUNC_PROTOCOL_MANGLING; 
}

/* -----exported algorithm data: -------------------------------------	*/

static const struct i2c_algorithm pcf_algo = {
	.master_xfer	= pcf_xfer,
	.functionality	= pcf_func,
};

/* 
 * registering functions to load algorithms at runtime 
 */
int i2c_pcf_add_bus(struct i2c_adapter *adap)
{
	struct i2c_algo_pcf_data *pcf_adap = adap->algo_data;
	int rval;

	DEB2(dev_dbg(&adap->dev, "hw routines registered.\n"));

	/* register new adapter to i2c module... */
	adap->algo = &pcf_algo;

	adap->timeout = 100;		/* default values, should	*/
	adap->retries = 3;		/* be replaced by defines	*/

	if ((rval = pcf_init_8584(pcf_adap)))
		return rval;

	rval = i2c_add_adapter(adap);

	return rval;
}
EXPORT_SYMBOL(i2c_pcf_add_bus);

MODULE_AUTHOR("Hans Berglund <hb@spacetec.no>");
MODULE_DESCRIPTION("I2C-Bus PCF8584 algorithm");
MODULE_LICENSE("GPL");

module_param(i2c_debug, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(i2c_debug,
        "debug level - 0 off; 1 normal; 2,3 more verbose; 9 pcf-protocol");
