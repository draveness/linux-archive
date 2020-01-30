/* 
   saa7185 - Philips SAA7185B video encoder driver version 0.0.3

   Copyright (C) 1998 Dave Perks <dperks@ibm.net>

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
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/malloc.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/signal.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/page.h>
#include <linux/sched.h>
#include <asm/segment.h>
#include <linux/types.h>
#include <linux/wrapper.h>

#include <linux/videodev.h>
#include <linux/version.h>
#include <asm/uaccess.h>

#include <linux/i2c-old.h>
#include <linux/video_encoder.h>

#define DEBUG(x)   x		/* Debug driver */

/* ----------------------------------------------------------------------- */

struct saa7185 {
	struct i2c_bus *bus;
	int addr;
	unsigned char reg[128];

	int norm;
	int enable;
	int bright;
	int contrast;
	int hue;
	int sat;
};

#define   I2C_SAA7185        0x88

#define I2C_DELAY   10

/* ----------------------------------------------------------------------- */

static int saa7185_write(struct saa7185 *dev, unsigned char subaddr, unsigned char data)
{
	int ack;

	LOCK_I2C_BUS(dev->bus);

	i2c_start(dev->bus);
	i2c_sendbyte(dev->bus, dev->addr, I2C_DELAY);
	i2c_sendbyte(dev->bus, subaddr, I2C_DELAY);
	ack = i2c_sendbyte(dev->bus, data, I2C_DELAY);
	dev->reg[subaddr] = data;
	i2c_stop(dev->bus);
	UNLOCK_I2C_BUS(dev->bus);
	return ack;
}

static int saa7185_write_block(struct saa7185 *dev, unsigned const char *data, unsigned int len)
{
	int ack = 0;
	unsigned subaddr;

	while (len > 1) {
		LOCK_I2C_BUS(dev->bus);
		i2c_start(dev->bus);
		i2c_sendbyte(dev->bus, dev->addr, I2C_DELAY);
		ack = i2c_sendbyte(dev->bus, (subaddr = *data++), I2C_DELAY);
		ack = i2c_sendbyte(dev->bus, (dev->reg[subaddr] = *data++), I2C_DELAY);
		len -= 2;
		while (len > 1 && *data == ++subaddr) {
			data++;
			ack = i2c_sendbyte(dev->bus, (dev->reg[subaddr] = *data++), I2C_DELAY);
			len -= 2;
		}
		i2c_stop(dev->bus);
		UNLOCK_I2C_BUS(dev->bus);
	}
	return ack;
}

/* ----------------------------------------------------------------------- */

static const unsigned char init_common[] =
{
	0x3a, 0x0f,		/* CBENB=0, V656=0, VY2C=1, YUV2C=1, MY2C=1, MUV2C=1 */

	0x42, 0x6b,		/* OVLY0=107 */
	0x43, 0x00,		/* OVLU0=0     white */
	0x44, 0x00,		/* OVLV0=0   */
	0x45, 0x22,		/* OVLY1=34  */
	0x46, 0xac,		/* OVLU1=172   yellow */
	0x47, 0x0e,		/* OVLV1=14  */
	0x48, 0x03,		/* OVLY2=3   */
	0x49, 0x1d,		/* OVLU2=29    cyan */
	0x4a, 0xac,		/* OVLV2=172 */
	0x4b, 0xf0,		/* OVLY3=240 */
	0x4c, 0xc8,		/* OVLU3=200   green */
	0x4d, 0xb9,		/* OVLV3=185 */
	0x4e, 0xd4,		/* OVLY4=212 */
	0x4f, 0x38,		/* OVLU4=56    magenta */
	0x50, 0x47,		/* OVLV4=71  */
	0x51, 0xc1,		/* OVLY5=193 */
	0x52, 0xe3,		/* OVLU5=227   red */
	0x53, 0x54,		/* OVLV5=84  */
	0x54, 0xa3,		/* OVLY6=163 */
	0x55, 0x54,		/* OVLU6=84    blue */
	0x56, 0xf2,		/* OVLV6=242 */
	0x57, 0x90,		/* OVLY7=144 */
	0x58, 0x00,		/* OVLU7=0     black */
	0x59, 0x00,		/* OVLV7=0   */

	0x5a, 0x00,		/* CHPS=0    */
	0x5b, 0x76,		/* GAINU=118 */
	0x5c, 0xa5,		/* GAINV=165 */
	0x5d, 0x3c,		/* BLCKL=60  */
	0x5e, 0x3a,		/* BLNNL=58  */
	0x5f, 0x3a,		/* CCRS=0, BLNVB=58 */
	0x60, 0x00,		/* NULL      */

/* 0x61 - 0x66 set according to norm */

	0x67, 0x00,		/* 0 : caption 1st byte odd  field */
	0x68, 0x00,		/* 0 : caption 2nd byte odd  field */
	0x69, 0x00,		/* 0 : caption 1st byte even field */
	0x6a, 0x00,		/* 0 : caption 2nd byte even field */

	0x6b, 0x91,		/* MODIN=2, PCREF=0, SCCLN=17 */
	0x6c, 0x20,		/* SRCV1=0, TRCV2=1, ORCV1=0, PRCV1=0, CBLF=0, ORCV2=0, PRCV2=0 */
	0x6d, 0x00,		/* SRCM1=0, CCEN=0 */

	0x6e, 0x0e,		/* HTRIG=0x00e, approx. centered, at least for PAL */
	0x6f, 0x00,		/* HTRIG upper bits */
	0x70, 0x20,		/* PHRES=0, SBLN=1, VTRIG=0 */

/* The following should not be needed */

	0x71, 0x15,		/* BMRQ=0x115 */
	0x72, 0x90,		/* EMRQ=0x690 */
	0x73, 0x61,		/* EMRQ=0x690, BMRQ=0x115 */
	0x74, 0x00,		/* NULL       */
	0x75, 0x00,		/* NULL       */
	0x76, 0x00,		/* NULL       */
	0x77, 0x15,		/* BRCV=0x115 */
	0x78, 0x90,		/* ERCV=0x690 */
	0x79, 0x61,		/* ERCV=0x690, BRCV=0x115 */

/* Field length controls */

	0x7a, 0x70,		/* FLC=0 */

/* The following should not be needed if SBLN = 1 */

	0x7b, 0x16,		/* FAL=22 */
	0x7c, 0x35,		/* LAL=244 */
	0x7d, 0x20,		/* LAL=244, FAL=22 */
};

static const unsigned char init_pal[] =
{
	0x61, 0x1e,		/* FISE=0, PAL=1, SCBW=1, RTCE=1, YGS=1, INPI=0, DOWN=0 */
	0x62, 0xc8,		/* DECTYP=1, BSTA=72 */
	0x63, 0xcb,		/* FSC0 */
	0x64, 0x8a,		/* FSC1 */
	0x65, 0x09,		/* FSC2 */
	0x66, 0x2a,		/* FSC3 */
};

static const unsigned char init_ntsc[] =
{
	0x61, 0x1d,		/* FISE=1, PAL=0, SCBW=1, RTCE=1, YGS=1, INPI=0, DOWN=0 */
	0x62, 0xe6,		/* DECTYP=1, BSTA=102 */
	0x63, 0x1f,		/* FSC0 */
	0x64, 0x7c,		/* FSC1 */
	0x65, 0xf0,		/* FSC2 */
	0x66, 0x21,		/* FSC3 */
};

static int saa7185_attach(struct i2c_device *device)
{
	int i;
	struct saa7185 *encoder;

	device->data = encoder = kmalloc(sizeof(struct saa7185), GFP_KERNEL);
	if (encoder == NULL) {
		return -ENOMEM;
	}
	MOD_INC_USE_COUNT;

	memset(encoder, 0, sizeof(struct saa7185));
	strcpy(device->name, "saa7185");
	encoder->bus = device->bus;
	encoder->addr = device->addr;
	encoder->norm = VIDEO_MODE_NTSC;
	encoder->enable = 1;

	i = saa7185_write_block(encoder, init_common, sizeof(init_common));
	if (i >= 0) {
		i = saa7185_write_block(encoder, init_ntsc, sizeof(init_ntsc));
	}
	if (i < 0) {
		printk(KERN_ERR "%s_attach: init error %d\n", device->name, i);
	}
	return 0;
}


static int saa7185_detach(struct i2c_device *device)
{
	kfree(device->data);
	MOD_DEC_USE_COUNT;
	return 0;
}

static int saa7185_command(struct i2c_device *device, unsigned int cmd, void *arg)
{
	struct saa7185 *encoder = device->data;

	switch (cmd) {

	case ENCODER_GET_CAPABILITIES:
		{
			struct video_encoder_capability *cap = arg;

			cap->flags
			    = VIDEO_ENCODER_PAL
			    | VIDEO_ENCODER_NTSC
			    | VIDEO_ENCODER_SECAM
			    | VIDEO_ENCODER_CCIR;
			cap->inputs = 1;
			cap->outputs = 1;
		}
		break;

	case ENCODER_SET_NORM:
		{
			int *iarg = arg;

			switch (*iarg) {

			case VIDEO_MODE_NTSC:
				saa7185_write_block(encoder, init_ntsc, sizeof(init_ntsc));
				break;

			case VIDEO_MODE_PAL:
				saa7185_write_block(encoder, init_pal, sizeof(init_pal));
				break;

			case VIDEO_MODE_SECAM:
			default:
				return -EINVAL;

			}
			encoder->norm = *iarg;
		}
		break;

	case ENCODER_SET_INPUT:
		{
			int *iarg = arg;

#if 0
			/* not much choice of inputs */
			if (*iarg != 0) {
				return -EINVAL;
			}
#else
			/* RJ: *iarg = 0: input is from SA7111
			   *iarg = 1: input is from ZR36060 */

			switch (*iarg) {

			case 0:
				/* Switch RTCE to 1 */
				saa7185_write(encoder, 0x61, (encoder->reg[0x61] & 0xf7) | 0x08);
				break;

			case 1:
				/* Switch RTCE to 0 */
				saa7185_write(encoder, 0x61, (encoder->reg[0x61] & 0xf7) | 0x00);
				break;

			default:
				return -EINVAL;

			}
#endif
		}
		break;

	case ENCODER_SET_OUTPUT:
		{
			int *iarg = arg;

			/* not much choice of outputs */
			if (*iarg != 0) {
				return -EINVAL;
			}
		}
		break;

	case ENCODER_ENABLE_OUTPUT:
		{
			int *iarg = arg;

			encoder->enable = !!*iarg;
			saa7185_write(encoder, 0x61, (encoder->reg[0x61] & 0xbf) | (encoder->enable ? 0x00 : 0x40));
		}
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

/* ----------------------------------------------------------------------- */

struct i2c_driver i2c_driver_saa7185 =
{
	"saa7185",		/* name */
	I2C_DRIVERID_VIDEOENCODER,	/* ID */
	I2C_SAA7185, I2C_SAA7185 + 1,

	saa7185_attach,
	saa7185_detach,
	saa7185_command
};

EXPORT_NO_SYMBOLS;

#ifdef MODULE
int init_module(void)
#else
int saa7185_init(void)
#endif
{
	return i2c_register_driver(&i2c_driver_saa7185);
}



#ifdef MODULE

void cleanup_module(void)
{
	i2c_unregister_driver(&i2c_driver_saa7185);
}

#endif
