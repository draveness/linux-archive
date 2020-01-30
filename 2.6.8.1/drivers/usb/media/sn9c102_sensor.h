/***************************************************************************
 * API for image sensors connected to the SN9C10[12] PC Camera Controllers *
 *                                                                         *
 * Copyright (C) 2004 by Luca Risolia <luca.risolia@studio.unibo.it>       *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify    *
 * it under the terms of the GNU General Public License as published by    *
 * the Free Software Foundation; either version 2 of the License, or       *
 * (at your option) any later version.                                     *
 *                                                                         *
 * This program is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License       *
 * along with this program; if not, write to the Free Software             *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.               *
 ***************************************************************************/

#ifndef _SN9C102_SENSOR_H_
#define _SN9C102_SENSOR_H_

#include <linux/usb.h>
#include <linux/videodev.h>
#include <linux/device.h>
#include <linux/stddef.h>
#include <linux/errno.h>
#include <asm/types.h>

struct sn9c102_device;
struct sn9c102_sensor;

/*****************************************************************************/

/* OVERVIEW.
   This is a small interface that allows you to add support for any CCD/CMOS
   image sensors connected to the SN9C10X bridges. The entire API is documented
   below. In the most general case, to support a sensor there are three steps
   you have to follow:
   1) define the main "sn9c102_sensor" structure by setting the basic fields;
   2) write a probing function to be called by the core module when the USB
      camera is recognized, then add both the USB ids and the name of that
      function to the two corresponding tables SENSOR_TABLE and ID_TABLE (see
      below);
   3) implement the methods that you want/need (and fill the rest of the main
      structure accordingly).
   "sn9c102_pas106b.c" is an example of all this stuff. Remember that you do
   NOT need to touch the source code of the core module for the things to work
   properly, unless you find bugs or flaws in it. Finally, do not forget to
   read the V4L2 API for completeness. */

/*****************************************************************************/
 
/* Probing functions: on success, you must attach the sensor to the camera
   by calling sn9c102_attach_sensor() provided below.
   To enable the I2C communication, you might need to perform a really basic
   initialization of the SN9C10X chip by using the write function declared 
   ahead.
   Functions must return 0 on success, the appropriate error otherwise. */
extern int sn9c102_probe_pas106b(struct sn9c102_device* cam);
extern int sn9c102_probe_tas5110c1b(struct sn9c102_device* cam);
extern int sn9c102_probe_tas5130d1b(struct sn9c102_device* cam);

/* Add the above entries to this table. Be sure to add the entry in the right
   place, since, on failure, the next probing routine is called according to 
   the order of the list below, from top to bottom */
#define SN9C102_SENSOR_TABLE                                                  \
static int (*sn9c102_sensor_table[])(struct sn9c102_device*) = {              \
	&sn9c102_probe_pas106b, /* strong detection based on SENSOR vid/pid */\
	&sn9c102_probe_tas5110c1b, /* detection based on USB pid/vid */       \
	&sn9c102_probe_tas5130d1b, /* detection based on USB pid/vid */       \
	NULL,                                                                 \
};

/* Attach a probed sensor to the camera. */
extern void 
sn9c102_attach_sensor(struct sn9c102_device* cam,
                      struct sn9c102_sensor* sensor);

/* Each SN9C10X camera has proper PID/VID identifiers. Add them here in case.*/
#define SN9C102_ID_TABLE                                                      \
static const struct usb_device_id sn9c102_id_table[] = {                      \
	{ USB_DEVICE(0xc45, 0x6001), },                                       \
	{ USB_DEVICE(0xc45, 0x6005), }, /* TAS5110C1B */                      \
	{ USB_DEVICE(0xc45, 0x6009), }, /* PAS106B */                         \
	{ USB_DEVICE(0xc45, 0x600d), }, /* PAS106B */                         \
	{ USB_DEVICE(0xc45, 0x6024), },                                       \
	{ USB_DEVICE(0xc45, 0x6025), }, /* TAS5130D1B Maybe also TAS5110C1B */\
	{ USB_DEVICE(0xc45, 0x6028), }, /* Maybe PAS202B */                   \
	{ USB_DEVICE(0xc45, 0x6029), },                                       \
	{ USB_DEVICE(0xc45, 0x602a), }, /* Maybe HV7131[D|E1] */              \
	{ USB_DEVICE(0xc45, 0x602c), }, /* Maybe OV7620 */                    \
	{ USB_DEVICE(0xc45, 0x6030), }, /* Maybe MI03 */                      \
	{ USB_DEVICE(0xc45, 0x8001), },                                       \
	{ }                                                                   \
};

/*****************************************************************************/

/* Read/write routines: they always return -1 on error, 0 or the read value
   otherwise. NOTE that a real read operation is not supported by the SN9C10X
   chip for some of its registers. To work around this problem, a pseudo-read
   call is provided instead: it returns the last successfully written value 
   on the register (0 if it has never been written), the usual -1 on error. */

/* The "try" I2C I/O versions are used when probing the sensor */
extern int sn9c102_i2c_try_write(struct sn9c102_device*,struct sn9c102_sensor*,
                                 u8 address, u8 value);
extern int sn9c102_i2c_try_read(struct sn9c102_device*,struct sn9c102_sensor*,
                                u8 address);

/* This must be used if and only if the sensor doesn't implement the standard
   I2C protocol, like the TASC sensors. There a number of good reasons why you
   must use the single-byte versions of this function: do not abuse. It writes
   n bytes, from data0 to datan, (registers 0x09 - 0x09+n of SN9C10X chip) */
extern int sn9c102_i2c_try_raw_write(struct sn9c102_device* cam,
                                     struct sn9c102_sensor* sensor, u8 n, 
                                     u8 data0, u8 data1, u8 data2, u8 data3,
                                     u8 data4, u8 data5);

/* To be used after the sensor struct has been attached to the camera struct */
extern int sn9c102_i2c_write(struct sn9c102_device*, u8 address, u8 value);
extern int sn9c102_i2c_read(struct sn9c102_device*, u8 address);

/* I/O on registers in the bridge. Could be used by the sensor methods too */
extern int sn9c102_write_reg(struct sn9c102_device*, u8 value, u16 index);
extern int sn9c102_pread_reg(struct sn9c102_device*, u16 index);

/* NOTE: there are no debugging functions here. To uniform the output you must
   use the dev_info()/dev_warn()/dev_err() macros defined in device.h, already
   included here, the argument being the struct device 'dev' of the sensor
   structure. Do NOT use these macros before the sensor is attached or the
   kernel will crash! However you should not need to notify the user about
   common errors or other messages, since this is done by the master module. */

/*****************************************************************************/

enum sn9c102_i2c_frequency { /* sensors may support both the frequencies */
	SN9C102_I2C_100KHZ = 0x01,
	SN9C102_I2C_400KHZ = 0x02,
};

enum sn9c102_i2c_interface {
	SN9C102_I2C_2WIRES,
	SN9C102_I2C_3WIRES,
};

struct sn9c102_sensor {
	char name[32], /* sensor name */
	     maintainer[64]; /* name of the mantainer <email> */

	/* These sensor capabilities must be provided if the SN9C10X controller
	   needs to communicate through the sensor serial interface by using
	   at least one of the i2c functions available */
	enum sn9c102_i2c_frequency frequency;
	enum sn9c102_i2c_interface interface;

	/* These identifiers must be provided if the image sensor implements
	   the standard I2C protocol. TASC sensors don't, although they have a
	   serial interface: so this is a case where the "raw" I2C version
	   could be helpful. */
	u8 slave_read_id, slave_write_id; /* reg. 0x09 */

	/* NOTE: Where not noted,most of the functions below are not mandatory.
	         Set to null if you do not implement them. If implemented,
	         they must return 0 on success, the proper error otherwise. */

	int (*init)(struct sn9c102_device* cam);
	/* This function is called after the sensor has been attached. 
	   It should be used to initialize the sensor only, but may also
	   configure part of the SN9C10X chip if necessary. You don't need to
	   setup picture settings like brightness, contrast, etc.. here, if
	   the corrisponding controls are implemented (see below), since 
	   they are adjusted in the core driver by calling the set_ctrl()
	   method after init(), where the arguments are the default values
	   specified in the v4l2_queryctrl list of supported controls;
	   Same suggestions apply for other settings, _if_ the corresponding
	   methods are present; if not, the initialization must configure the
	   sensor according to the default configuration structures below. */

	struct v4l2_queryctrl qctrl[V4L2_CID_LASTP1-V4L2_CID_BASE];
	/* Optional list of default controls, defined as indicated in the 
	   V4L2 API. Menu type controls are not handled by this interface. */

	int (*get_ctrl)(struct sn9c102_device* cam, struct v4l2_control* ctrl);
	int (*set_ctrl)(struct sn9c102_device* cam,
	                const struct v4l2_control* ctrl);
	/* You must implement at least the set_ctrl method if you have defined
	   the list above. The returned value must follow the V4L2
	   specifications for the VIDIOC_G|C_CTRL ioctls. V4L2_CID_H|VCENTER
	   are not supported by this driver, so do not implement them. Also,
	   passed values are NOT checked to see if they are out of bounds. */

	struct v4l2_cropcap cropcap;
	/* Think the image sensor as a grid of R,G,B monochromatic pixels
	   disposed according to a particular Bayer pattern, which describes
	   the complete array of pixels, from (0,0) to (xmax, ymax). We will
	   use this coordinate system from now on. It is assumed the sensor
	   chip can be programmed to capture/transmit a subsection of that
	   array of pixels: we will call this subsection "active window".
	   It is not always true that the largest achievable active window can
	   cover the whole array of pixels. The V4L2 API defines another
	   area called "source rectangle", which, in turn, is a subrectangle of
	   the active window. The SN9C10X chip is always programmed to read the
	   source rectangle.
	   The bounds of both the active window and the source rectangle are
	   specified in the cropcap substructures 'bounds' and 'defrect'.
	   By default, the source rectangle should cover the largest possible
	   area. Again, it is not always true that the largest source rectangle
	   can cover the entire active window, although it is a rare case for 
	   the hardware we have. The bounds of the source rectangle _must_ be
	   multiple of 16 and must use the same coordinate system as indicated
	   before; their centers shall align initially.
	   If necessary, the sensor chip must be initialized during init() to
	   set the bounds of the active sensor window; however, by default, it
	   usually covers the largest achievable area (maxwidth x maxheight)
	   of pixels, so no particular initialization is needed, if you have
	   defined the correct default bounds in the structures.
	   See the V4L2 API for further details.
	   NOTE: once you have defined the bounds of the active window
	         (struct cropcap.bounds) you must not change them.anymore.
	   Only 'bounds' and 'defrect' fields are mandatory, other fields
	   will be ignored. */

	int (*set_crop)(struct sn9c102_device* cam,
	                const struct v4l2_rect* rect);
	/* To be called on VIDIOC_C_SETCROP. The core module always calls a
	   default routine which configures the appropriate SN9C10X regs (also
	   scaling), but you may need to override/adjust specific stuff.
	   'rect' contains width and height values that are multiple of 16: in
	   case you override the default function, you always have to program
	   the chip to match those values; on error return the corresponding
	   error code without rolling back.
	   NOTE: in case, you must program the SN9C10X chip to get rid of 
	         blank pixels or blank lines at the _start_ of each line or
	         frame after each HSYNC or VSYNC, so that the image starts with
	         real RGB data (see regs 0x12,0x13) (having set H_SIZE and,
	         V_SIZE you don't have to care about blank pixels or blank
	         lines at the end of each line or frame). */

	struct v4l2_pix_format pix_format;
	/* What you have to define here are: initial 'width' and 'height' of
	   the target rectangle, the bayer 'pixelformat' and 'priv' which we'll
	   be used to indicate the number of bits per pixel, 8 or 9. 
	   Nothing more.
	   NOTE 1: both 'width' and 'height' _must_ be either 1/1 or 1/2 or 1/4
	           of cropcap.defrect.width and cropcap.defrect.height. I
	           suggest 1/1.
	   NOTE 2: as said above, you have to program the SN9C10X chip to get
	           rid of any blank pixels, so that the output of the sensor
	           matches the RGB bayer sequence (i.e. BGBGBG...GRGRGR). */

	const struct device* dev;
	/* This is the argument for dev_err(), dev_info() and dev_warn(). It
	   is used for debugging purposes. You must not access the struct
	   before the sensor is attached. */

	const struct usb_device* usbdev;
	/* Points to the usb_device struct after the sensor is attached.
	   Do not touch unless you know what you are doing. */

	/* Do NOT write to the data below, it's READ ONLY. It is used by the
	   core module to store successfully updated values of the above
	   settings, for rollbacks..etc..in case of errors during atomic I/O */
	struct v4l2_queryctrl _qctrl[V4L2_CID_LASTP1-V4L2_CID_BASE];
	struct v4l2_rect _rect;
};

#endif /* _SN9C102_SENSOR_H_ */
