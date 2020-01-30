/*
 * cpia_pp CPiA Parallel Port driver
 *
 * Supports CPiA based parallel port Video Camera's.
 *
 * (C) Copyright 1999 Bas Huisman <bhuism@cs.utwente.nl>
 * (C) Copyright 1999-2000 Scott J. Bertin <sbertin@mindspring.com>,
 * (C) Copyright 1999-2000 Peter Pregler <Peter_Pregler@email.com>
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
#include <linux/version.h>

#include <linux/module.h>
#include <linux/init.h>

#include <linux/kernel.h>
#include <linux/parport.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/smp_lock.h>

#ifdef CONFIG_KMOD
#include <linux/kmod.h>
#endif

/* #define _CPIA_DEBUG_		define for verbose debug output */
#include "cpia.h"

static int cpia_pp_open(void *privdata);
static int cpia_pp_registerCallback(void *privdata, void (*cb) (void *cbdata),
                                    void *cbdata);
static int cpia_pp_transferCmd(void *privdata, u8 *command, u8 *data);
static int cpia_pp_streamStart(void *privdata);
static int cpia_pp_streamStop(void *privdata);
static int cpia_pp_streamRead(void *privdata, u8 *buffer, int noblock);
static int cpia_pp_close(void *privdata);

#define ABOUT "Parallel port driver for Vision CPiA based cameras"

/* IEEE 1284 Compatiblity Mode signal names 	*/
#define nStrobe		PARPORT_CONTROL_STROBE	  /* inverted */
#define nAutoFd		PARPORT_CONTROL_AUTOFD	  /* inverted */
#define nInit		PARPORT_CONTROL_INIT
#define nSelectIn	PARPORT_CONTROL_SELECT
#define IntrEnable	PARPORT_CONTROL_INTEN	  /* normally zero for no IRQ */
#define DirBit		PARPORT_CONTROL_DIRECTION /* 0 = Forward, 1 = Reverse	*/

#define nFault		PARPORT_STATUS_ERROR
#define Select		PARPORT_STATUS_SELECT
#define PError		PARPORT_STATUS_PAPEROUT
#define nAck		PARPORT_STATUS_ACK
#define Busy		PARPORT_STATUS_BUSY	  /* inverted */	

/* some more */
#define HostClk		nStrobe
#define HostAck		nAutoFd
#define nReverseRequest	nInit
#define Active_1284	nSelectIn
#define nPeriphRequest	nFault
#define XFlag		Select
#define nAckReverse	PError
#define PeriphClk	nAck
#define PeriphAck	Busy

/* these can be used to correct for the inversion on some bits */
#define STATUS_INVERSION_MASK	(Busy)
#define CONTROL_INVERSION_MASK	(nStrobe|nAutoFd|nSelectIn)

#define ECR_empty	0x01
#define ECR_full	0x02
#define ECR_serviceIntr 0x04
#define ECR_dmaEn	0x08
#define ECR_nErrIntrEn	0x10

#define ECR_mode_mask	0xE0
#define ECR_SPP_mode	0x00
#define ECR_PS2_mode	0x20
#define ECR_FIFO_mode	0x40
#define ECR_ECP_mode	0x60

#define ECP_FIFO_SIZE	16
#define DMA_BUFFER_SIZE               PAGE_SIZE
	/* for 16bit DMA make sure DMA_BUFFER_SIZE is 16 bit aligned */
#define PARPORT_CHUNK_SIZE	PAGE_SIZE/* >=2.3.x */
				/* we read this many bytes at once */

#define GetECRMasked(port,mask)	(parport_read_econtrol(port) & (mask))
#define GetStatus(port)		((parport_read_status(port)^STATUS_INVERSION_MASK)&(0xf8))
#define SetStatus(port,val)	parport_write_status(port,(val)^STATUS_INVERSION_MASK)
#define GetControl(port)	((parport_read_control(port)^CONTROL_INVERSION_MASK)&(0x3f))
#define SetControl(port,val)	parport_write_control(port,(val)^CONTROL_INVERSION_MASK)

#define GetStatusMasked(port,mask)	(GetStatus(port) & (mask))
#define GetControlMasked(port,mask)	(GetControl(port) & (mask))
#define SetControlMasked(port,mask)	SetControl(port,GetControl(port) | (mask));
#define ClearControlMasked(port,mask)	SetControl(port,GetControl(port)&~(mask));
#define FrobControlBit(port,mask,value)	SetControl(port,(GetControl(port)&~(mask))|((value)&(mask)));

#define PACKET_LENGTH 	8

/* Magic numbers for defining port-device mappings */
#define PPCPIA_PARPORT_UNSPEC -4
#define PPCPIA_PARPORT_AUTO -3
#define PPCPIA_PARPORT_OFF -2
#define PPCPIA_PARPORT_NONE -1

#ifdef MODULE
static int parport_nr[PARPORT_MAX] = {[0 ... PARPORT_MAX - 1] = PPCPIA_PARPORT_UNSPEC};
static char *parport[PARPORT_MAX] = {NULL,};

MODULE_AUTHOR("B. Huisman <bhuism@cs.utwente.nl> & Peter Pregler <Peter_Pregler@email.com>");
MODULE_DESCRIPTION("Parallel port driver for Vision CPiA based cameras");
MODULE_PARM(parport, "1-" __MODULE_STRING(PARPORT_MAX) "s");
MODULE_PARM_DESC(parport, "'auto' or a list of parallel port numbers. Just like lp.");
#else
static int parport_nr[PARPORT_MAX] __initdata =
	{[0 ... PARPORT_MAX - 1] = PPCPIA_PARPORT_UNSPEC};
static int parport_ptr = 0;
#endif

struct pp_cam_entry {
	struct pardevice *pdev;
	struct parport *port;
	struct tq_struct cb_task;
	int open_count;
	wait_queue_head_t wq_stream;
	/* image state flags */
	int image_ready;	/* we got an interrupt */
	int image_complete;	/* we have seen 4 EOI */

	int streaming; /* we are in streaming mode */
	int stream_irq;
};

static struct cpia_camera_ops cpia_pp_ops = 
{
	cpia_pp_open,
	cpia_pp_registerCallback,
	cpia_pp_transferCmd,
	cpia_pp_streamStart,
	cpia_pp_streamStop,
	cpia_pp_streamRead,
	cpia_pp_close,
	1
};

static struct cam_data *cam_list;

#ifdef _CPIA_DEBUG_
#define DEB_PORT(port) { \
u8 controll = GetControl(port); \
u8 statusss = GetStatus(port); \
DBG("nsel %c per %c naut %c nstrob %c nak %c busy %c nfaul %c sel %c init %c dir %c\n",\
((controll & nSelectIn)	? 'U' : 'D'), \
((statusss & PError)	? 'U' : 'D'), \
((controll & nAutoFd)	? 'U' : 'D'), \
((controll & nStrobe)	? 'U' : 'D'), \
((statusss & nAck)	? 'U' : 'D'), \
((statusss & Busy)	? 'U' : 'D'), \
((statusss & nFault)	? 'U' : 'D'), \
((statusss & Select)	? 'U' : 'D'), \
((controll & nInit)	? 'U' : 'D'), \
((controll & DirBit)	? 'R' : 'F')  \
); }
#else
#define DEB_PORT(port) {}
#endif

#define WHILE_OUT_TIMEOUT (HZ/10)
#define DMA_TIMEOUT 10*HZ

/* FIXME */
static void cpia_parport_enable_irq( struct parport *port ) {
	parport_enable_irq(port);
	mdelay(10);
	return;
}

static void cpia_parport_disable_irq( struct parport *port ) {
	parport_disable_irq(port);
	mdelay(10);
	return;
}

/****************************************************************************
 *
 *  EndTransferMode
 *
 ***************************************************************************/
static void EndTransferMode(struct pp_cam_entry *cam)
{
	parport_negotiate(cam->port, IEEE1284_MODE_COMPAT);
}

/****************************************************************************
 *
 *  ForwardSetup
 *
 ***************************************************************************/
static int ForwardSetup(struct pp_cam_entry *cam)
{
	int retry;
	
	/* After some commands the camera needs extra time before
	 * it will respond again, so we try up to 3 times */
	for(retry=0; retry<3; ++retry) {
		if(!parport_negotiate(cam->port, IEEE1284_MODE_ECP)) {
			break;
		}
	}
	if(retry == 3) {
		DBG("Unable to negotiate ECP mode\n");
		return -1;
	}
	return 0;
}

/****************************************************************************
 *
 *  ReverseSetup
 *
 ***************************************************************************/
static int ReverseSetup(struct pp_cam_entry *cam, int extensibility)
{
	int retry;
	int mode = IEEE1284_MODE_ECP;
	if(extensibility) mode = 8|3|IEEE1284_EXT_LINK;

	/* After some commands the camera needs extra time before
	 * it will respond again, so we try up to 3 times */
	for(retry=0; retry<3; ++retry) {
		if(!parport_negotiate(cam->port, mode)) {
			break;
		}
	}
	if(retry == 3) {
		if(extensibility)
			DBG("Unable to negotiate extensibility mode\n");
		else
			DBG("Unable to negotiate ECP mode\n");
		return -1;
	}
	if(extensibility) cam->port->ieee1284.mode = IEEE1284_MODE_ECP;
	return 0;
}

/****************************************************************************
 *
 *  WritePacket
 *
 ***************************************************************************/
static int WritePacket(struct pp_cam_entry *cam, const u8 *packet, size_t size)
{
	int retval=0;
	int size_written;

	if (packet == NULL) {
		return -EINVAL;
	}
	if (ForwardSetup(cam)) {
		DBG("Write failed in setup\n");
		return -EIO;
	}
	size_written = parport_write(cam->port, packet, size);
	if(size_written != size) {
		DBG("Write failed, wrote %d/%d\n", size_written, size);
		retval = -EIO;
	}
	EndTransferMode(cam);
	return retval;
}

/****************************************************************************
 *
 *  ReadPacket
 *
 ***************************************************************************/
static int ReadPacket(struct pp_cam_entry *cam, u8 *packet, size_t size)
{
	int retval=0;
	if (packet == NULL) {
		return -EINVAL;
	}
	if (ReverseSetup(cam, 0)) {
		return -EIO;
	}
	if(parport_read(cam->port, packet, size) != size) {
		retval = -EIO;
	}
	EndTransferMode(cam);
	return retval;
}

/****************************************************************************
 *
 *  cpia_pp_streamStart
 *
 ***************************************************************************/
static int cpia_pp_streamStart(void *privdata)
{
	struct pp_cam_entry *cam = privdata;
	DBG("\n");
	cam->streaming=1;
	cam->image_ready=0;
	//if (ReverseSetup(cam,1)) return -EIO;
	if(cam->stream_irq) cpia_parport_enable_irq(cam->port);
	return 0;
}

/****************************************************************************
 *
 *  cpia_pp_streamStop
 *
 ***************************************************************************/
static int cpia_pp_streamStop(void *privdata)
{
	struct pp_cam_entry *cam = privdata;

	DBG("\n");
	cam->streaming=0;
	cpia_parport_disable_irq(cam->port);
	//EndTransferMode(cam);

	return 0;
}

static int cpia_pp_read(struct parport *port, u8 *buffer, int len)
{
	int bytes_read, new_bytes;
	for(bytes_read=0; bytes_read<len; bytes_read += new_bytes) {
		new_bytes = parport_read(port, buffer+bytes_read,
			                 len-bytes_read);
		if(new_bytes < 0) break;
	}
	return bytes_read;
}

/****************************************************************************
 *
 *  cpia_pp_streamRead
 *
 ***************************************************************************/
static int cpia_pp_streamRead(void *privdata, u8 *buffer, int noblock)
{
	struct pp_cam_entry *cam = privdata;
	int read_bytes = 0;
	int i, endseen, block_size, new_bytes;

	if(cam == NULL) {
		DBG("Internal driver error: cam is NULL\n");
		return -EINVAL;
	}
	if(buffer == NULL) {
		DBG("Internal driver error: buffer is NULL\n");
		return -EINVAL;
	}
	//if(cam->streaming) DBG("%d / %d\n", cam->image_ready, noblock);
	if( cam->stream_irq ) {
		DBG("%d\n", cam->image_ready);
		cam->image_ready--;
	}
	cam->image_complete=0;
	if (0/*cam->streaming*/) {
		if(!cam->image_ready) {
			if(noblock) return -EWOULDBLOCK;
			interruptible_sleep_on(&cam->wq_stream);
			if( signal_pending(current) ) return -EINTR;
			DBG("%d\n", cam->image_ready);
		}
	} else {
		if (ReverseSetup(cam, 1)) {
			DBG("unable to ReverseSetup\n");
			return -EIO;
		}
	}
	endseen = 0;
	block_size = PARPORT_CHUNK_SIZE;
	while( !cam->image_complete ) {
		if(current->need_resched)  schedule();
		
		new_bytes = cpia_pp_read(cam->port, buffer, block_size );
		if( new_bytes <= 0 ) {
			break;
		}
		i=-1;
		while(++i<new_bytes && endseen<4) {
	        	if(*buffer==EOI) {
	                	endseen++;
	                } else {
	                	endseen=0;
	                }
			buffer++;
		}
		read_bytes += i;
		if( endseen==4 ) {
			cam->image_complete=1;
			break;
		}
		if( CPIA_MAX_IMAGE_SIZE-read_bytes <= PARPORT_CHUNK_SIZE ) {
			block_size=CPIA_MAX_IMAGE_SIZE-read_bytes;
		}
	}
	EndTransferMode(cam);
	return cam->image_complete ? read_bytes : -EIO;
}

/****************************************************************************
 *
 *  cpia_pp_transferCmd
 *
 ***************************************************************************/
static int cpia_pp_transferCmd(void *privdata, u8 *command, u8 *data)
{
	int err;
	int retval=0;
	int databytes;
	struct pp_cam_entry *cam = privdata;

	if(cam == NULL) {
		DBG("Internal driver error: cam is NULL\n");
		return -EINVAL;
	}
	if(command == NULL) {
		DBG("Internal driver error: command is NULL\n");
		return -EINVAL;
	}
	databytes = (((int)command[7])<<8) | command[6];
	if ((err = WritePacket(cam, command, PACKET_LENGTH)) < 0) {
		DBG("Error writing command\n");
		return err;
	}
	if(command[0] == DATA_IN) {
		u8 buffer[8];
		if(data == NULL) {
			DBG("Internal driver error: data is NULL\n");
			return -EINVAL;
		}
		if((err = ReadPacket(cam, buffer, 8)) < 0) {
			return err;
			DBG("Error reading command result\n");
		}
		memcpy(data, buffer, databytes);
	} else if(command[0] == DATA_OUT) {
		if(databytes > 0) {
			if(data == NULL) {
				DBG("Internal driver error: data is NULL\n");
				retval = -EINVAL;
			} else {
				if((err=WritePacket(cam, data, databytes)) < 0){
					DBG("Error writing command data\n");
					return err;
				}
			}
		}
	} else {
		DBG("Unexpected first byte of command: %x\n", command[0]);
		retval = -EINVAL;
	}
	return retval;
}

/****************************************************************************
 *
 *  cpia_pp_open
 *
 ***************************************************************************/
static int cpia_pp_open(void *privdata)
{
	struct pp_cam_entry *cam = (struct pp_cam_entry *)privdata;
	
	if (cam == NULL)
		return -EINVAL;
	
	if(cam->open_count == 0) {
		if (parport_claim(cam->pdev)) {
			DBG("failed to claim the port\n");
			return -EBUSY;
		}
		parport_negotiate(cam->port, IEEE1284_MODE_COMPAT);
		parport_data_forward(cam->port);
		parport_write_control(cam->port, PARPORT_CONTROL_SELECT);
		udelay(50);
		parport_write_control(cam->port,
		                      PARPORT_CONTROL_SELECT
		                      | PARPORT_CONTROL_INIT);
	}
	
	++cam->open_count;
	
#ifdef MODULE
	MOD_INC_USE_COUNT;
#endif
	return 0;
}

/****************************************************************************
 *
 *  cpia_pp_registerCallback
 *
 ***************************************************************************/
static int cpia_pp_registerCallback(void *privdata, void (*cb)(void *cbdata), void *cbdata)
{
	struct pp_cam_entry *cam = privdata;
	int retval = 0;
	
	if(cam->port->irq != PARPORT_IRQ_NONE) {
		cam->cb_task.routine = cb;
		cam->cb_task.data = cbdata;
	} else {
		retval = -1;
	}
	return retval;
}

/****************************************************************************
 *
 *  cpia_pp_close
 *
 ***************************************************************************/
static int cpia_pp_close(void *privdata)
{
	struct pp_cam_entry *cam = privdata;
#ifdef MODULE
	MOD_DEC_USE_COUNT;
#endif
	if (--cam->open_count == 0) {
		parport_release(cam->pdev);
	}
	return 0;
}

/****************************************************************************
 *
 *  cpia_pp_register
 *
 ***************************************************************************/
static int cpia_pp_register(struct parport *port)
{
	struct pardevice *pdev = NULL;
	struct pp_cam_entry *cam;
	struct cam_data *cpia;

	if (!(port->modes & PARPORT_MODE_ECP) &&
	    !(port->modes & PARPORT_MODE_TRISTATE)) {
		LOG("port is not ECP capable\n");
		return -ENXIO;
	}

	cam = kmalloc(sizeof(struct pp_cam_entry), GFP_KERNEL);
	if (cam == NULL) {
		LOG("failed to allocate camera structure\n");
		return -ENOMEM;
	}
	memset(cam,0,sizeof(struct pp_cam_entry));
	
	pdev = parport_register_device(port, "cpia_pp", NULL, NULL,
	                               NULL, 0, cam);

	if (!pdev) {
		LOG("failed to parport_register_device\n");
		kfree(cam);
		return -ENXIO;
	}

	cam->pdev = pdev;
	cam->port = port;
	init_waitqueue_head(&cam->wq_stream);

	cam->streaming = 0;
	cam->stream_irq = 0;

	if((cpia = cpia_register_camera(&cpia_pp_ops, cam)) == NULL) {
		LOG("failed to cpia_register_camera\n");
		parport_unregister_device(pdev);
		kfree(cam);
		return -ENXIO;
	}
	ADD_TO_LIST(cam_list, cpia);

	return 0;
}

static void cpia_pp_detach (struct parport *port)
{
	struct cam_data *cpia;

	for(cpia = cam_list; cpia != NULL; cpia = cpia->next) {
		struct pp_cam_entry *cam = cpia->lowlevel_data;
		if (cam && cam->port->number == port->number) {
			REMOVE_FROM_LIST(cpia);
			
			cpia_unregister_camera(cpia);
			
			if(cam->open_count > 0) {
				cpia_pp_close(cam);
			}

			parport_unregister_device(cam->pdev);
		
			kfree(cam);
			cpia->lowlevel_data = NULL;
			break;
		}
	}
}

static void cpia_pp_attach (struct parport *port)
{
	unsigned int i;

	switch (parport_nr[0])
	{
	case PPCPIA_PARPORT_UNSPEC:
	case PPCPIA_PARPORT_AUTO:
		if (port->probe_info[0].class != PARPORT_CLASS_MEDIA ||
		    port->probe_info[0].cmdset == NULL ||
		    strncmp(port->probe_info[0].cmdset, "CPIA_1", 6) != 0)
			return;

		cpia_pp_register(port);

		break;

	default:
		for (i = 0; i < PARPORT_MAX; ++i) {
			if (port->number == parport_nr[i]) {
				cpia_pp_register(port);
				break;
			}
		}
		break;
	}
}

static struct parport_driver cpia_pp_driver = {
	"cpia_pp",
	cpia_pp_attach,
	cpia_pp_detach,
	NULL
};

int cpia_pp_init(void)
{
	printk(KERN_INFO "%s v%d.%d.%d\n",ABOUT, 
	       CPIA_PP_MAJ_VER,CPIA_PP_MIN_VER,CPIA_PP_PATCH_VER);

	if(parport_nr[0] == PPCPIA_PARPORT_OFF) {
		printk("  disabled\n");
		return 0;
	}
	
	if (parport_register_driver (&cpia_pp_driver)) {
		LOG ("unable to register with parport\n");
		return -EIO;
	}

	return 0;
}

#ifdef MODULE
int init_module(void)
{
	if (parport[0]) {
		/* The user gave some parameters.  Let's see what they were. */
		if (!strncmp(parport[0], "auto", 4)) {
			parport_nr[0] = PPCPIA_PARPORT_AUTO;
		} else {
			int n;
			for (n = 0; n < PARPORT_MAX && parport[n]; n++) {
				if (!strncmp(parport[n], "none", 4)) {
					parport_nr[n] = PPCPIA_PARPORT_NONE;
				} else {
					char *ep;
					unsigned long r = simple_strtoul(parport[n], &ep, 0);
					if (ep != parport[n]) {
						parport_nr[n] = r;
					} else {
						LOG("bad port specifier `%s'\n", parport[n]);
						return -ENODEV;
					}
				}
			}
		}
	}
#if defined(CONFIG_KMOD) && defined(CONFIG_PNP_PARPORT_MODULE)
	if(parport_enumerate() && !parport_enumerate()->probe_info.model) {
		request_module("parport_probe");
	}
#endif
	return cpia_pp_init();
}

void cleanup_module(void)
{
	parport_unregister_driver (&cpia_pp_driver);
	return;
}

#else /* !MODULE */

static int __init cpia_pp_setup(char *str)
{
#if 0
	/* Is this only a 2.2ism? -jerdfelt */
	if (!str) {
		if (ints[0] == 0 || ints[1] == 0) {
			/* disable driver on "cpia_pp=" or "cpia_pp=0" */
			parport_nr[0] = PPCPIA_PARPORT_OFF;
		}
	} else
#endif
	if (!strncmp(str, "parport", 7)) {
		int n = simple_strtoul(str + 7, NULL, 10);
		if (parport_ptr < PARPORT_MAX) {
			parport_nr[parport_ptr++] = n;
		} else {
			LOG("too many ports, %s ignored.\n", str);
		}
	} else if (!strcmp(str, "auto")) {
		parport_nr[0] = PPCPIA_PARPORT_AUTO;
	} else if (!strcmp(str, "none")) {
		parport_nr[parport_ptr++] = PPCPIA_PARPORT_NONE;
	}

	return 0;
}

__setup("cpia_pp=", cpia_pp_setup);

#endif /* !MODULE */
