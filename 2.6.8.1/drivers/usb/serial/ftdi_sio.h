/*
 * Definitions for the FTDI USB Single Port Serial Converter - 
 * known as FTDI_SIO (Serial Input/Output application of the chipset) 
 *
 * The example I have is known as the USC-1000 which is available from
 * http://www.dse.co.nz - cat no XH4214 It looks similar to this:
 * http://www.dansdata.com/usbser.htm but I can't be sure There are other
 * USC-1000s which don't look like my device though so beware!
 *
 * The device is based on the FTDI FT8U100AX chip. It has a DB25 on one side, 
 * USB on the other.
 *
 * Thanx to FTDI (http://www.ftdi.co.uk) for so kindly providing details
 * of the protocol required to talk to the device and ongoing assistence
 * during development.
 *
 * Bill Ryder - bryder@sgi.com formerly of Silicon Graphics, Inc.- wrote the 
 * FTDI_SIO implementation.
 *
 * Philipp G�hring - pg@futureware.at - added the Device ID of the USB relais
 * from Rudolf Gugler
 *
 */

#define FTDI_VID	0x0403	/* Vendor Id */
#define FTDI_SIO_PID	0x8372	/* Product Id SIO application of 8U100AX  */
#define FTDI_8U232AM_PID 0x6001 /* Similar device to SIO above */
#define FTDI_8U232AM_ALT_PID 0x6006 /* FTDI's alternate PID for above */
#define FTDI_RELAIS_PID	0xFA10  /* Relais device from Rudolf Gugler */
#define FTDI_NF_RIC_VID	0x0DCD	/* Vendor Id */
#define FTDI_NF_RIC_PID	0x0001	/* Product Id */

/* www.irtrans.de device */
#define FTDI_IRTRANS_PID 0xFC60 /* Product Id */

/* www.crystalfontz.com devices - thanx for providing free devices for evaluation ! */
/* they use the ftdi chipset for the USB interface and the vendor id is the same */
#define FTDI_XF_632_PID 0xFC08	/* 632: 16x2 Character Display */
#define FTDI_XF_634_PID 0xFC09	/* 634: 20x4 Character Display */
#define FTDI_XF_547_PID 0xFC0A	/* 547: Two line Display */
#define FTDI_XF_633_PID 0xFC0B	/* 633: 16x2 Character Display with Keys */
#define FTDI_XF_631_PID 0xFC0C	/* 631: 20x2 Character Display */
#define FTDI_XF_635_PID 0xFC0D	/* 635: 20x4 Character Display */
#define FTDI_XF_640_PID 0xFC0E	/* 640: Two line Display */
#define FTDI_XF_642_PID 0xFC0F	/* 642: Two line Display */

/* Video Networks Limited / Homechoice in the UK use an ftdi-based device for their 1Mb */
/* broadband internet service.  The following PID is exhibited by the usb device supplied */
/* (the VID is the standard ftdi vid (FTDI_VID) */
#define FTDI_VNHCPCUSB_D_PID 0xfe38 /* Product Id */

/*
 * The following are the values for the Matrix Orbital LCD displays,
 * which are the FT232BM ( similar to the 8U232AM )
 */
#define FTDI_MTXORB_0_PID      0xFA00  /* Matrix Orbital Product Id */
#define FTDI_MTXORB_1_PID      0xFA01  /* Matrix Orbital Product Id */
#define FTDI_MTXORB_2_PID      0xFA02  /* Matrix Orbital Product Id */
#define FTDI_MTXORB_3_PID      0xFA03  /* Matrix Orbital Product Id */
#define FTDI_MTXORB_4_PID      0xFA04  /* Matrix Orbital Product Id */
#define FTDI_MTXORB_5_PID      0xFA05  /* Matrix Orbital Product Id */
#define FTDI_MTXORB_6_PID      0xFA06  /* Matrix Orbital Product Id */

/* Interbiometrics USB I/O Board */
/* Developed for Interbiometrics by Rudolf Gugler */
#define INTERBIOMETRICS_VID              0x1209
#define INTERBIOMETRICS_IOBOARD_PID      0x1002
#define INTERBIOMETRICS_MINI_IOBOARD_PID 0x1006

/*
 * The following are the values for the Perle Systems
 * UltraPort USB serial converters
 */
#define FTDI_PERLE_ULTRAPORT_PID 0xF0C0	/* Perle UltraPort Product Id */

/*
 * The following are the values for the Sealevel SeaLINK+ adapters.
 * (Original list sent by Tuan Hoang.  Ian Abbott renamed the macros and
 * removed some PIDs that don't seem to match any existing products.)
 */
#define SEALEVEL_VID		0x0c52	/* Sealevel Vendor ID */
#define SEALEVEL_2101_PID	0x2101	/* SeaLINK+232 (2101/2105) */
#define SEALEVEL_2102_PID	0x2102	/* SeaLINK+485 (2102) */
#define SEALEVEL_2103_PID	0x2103	/* SeaLINK+232I (2103) */
#define SEALEVEL_2104_PID	0x2104	/* SeaLINK+485I (2104) */
#define SEALEVEL_2201_1_PID	0x2211	/* SeaPORT+2/232 (2201) Port 1 */
#define SEALEVEL_2201_2_PID	0x2221	/* SeaPORT+2/232 (2201) Port 2 */
#define SEALEVEL_2202_1_PID	0x2212	/* SeaPORT+2/485 (2202) Port 1 */
#define SEALEVEL_2202_2_PID	0x2222	/* SeaPORT+2/485 (2202) Port 2 */
#define SEALEVEL_2203_1_PID	0x2213	/* SeaPORT+2 (2203) Port 1 */
#define SEALEVEL_2203_2_PID	0x2223	/* SeaPORT+2 (2203) Port 2 */
#define SEALEVEL_2401_1_PID	0x2411	/* SeaPORT+4/232 (2401) Port 1 */
#define SEALEVEL_2401_2_PID	0x2421	/* SeaPORT+4/232 (2401) Port 2 */
#define SEALEVEL_2401_3_PID	0x2431	/* SeaPORT+4/232 (2401) Port 3 */
#define SEALEVEL_2401_4_PID	0x2441	/* SeaPORT+4/232 (2401) Port 4 */
#define SEALEVEL_2402_1_PID	0x2412	/* SeaPORT+4/485 (2402) Port 1 */
#define SEALEVEL_2402_2_PID	0x2422	/* SeaPORT+4/485 (2402) Port 2 */
#define SEALEVEL_2402_3_PID	0x2432	/* SeaPORT+4/485 (2402) Port 3 */
#define SEALEVEL_2402_4_PID	0x2442	/* SeaPORT+4/485 (2402) Port 4 */
#define SEALEVEL_2403_1_PID	0x2413	/* SeaPORT+4 (2403) Port 1 */
#define SEALEVEL_2403_2_PID	0x2423	/* SeaPORT+4 (2403) Port 2 */
#define SEALEVEL_2403_3_PID	0x2433	/* SeaPORT+4 (2403) Port 3 */
#define SEALEVEL_2403_4_PID	0x2443	/* SeaPORT+4 (2403) Port 4 */
#define SEALEVEL_2801_1_PID	0X2811	/* SeaLINK+8/232 (2801) Port 1 */
#define SEALEVEL_2801_2_PID	0X2821	/* SeaLINK+8/232 (2801) Port 2 */
#define SEALEVEL_2801_3_PID	0X2831	/* SeaLINK+8/232 (2801) Port 3 */
#define SEALEVEL_2801_4_PID	0X2841	/* SeaLINK+8/232 (2801) Port 4 */
#define SEALEVEL_2801_5_PID	0X2851	/* SeaLINK+8/232 (2801) Port 5 */
#define SEALEVEL_2801_6_PID	0X2861	/* SeaLINK+8/232 (2801) Port 6 */
#define SEALEVEL_2801_7_PID	0X2871	/* SeaLINK+8/232 (2801) Port 7 */
#define SEALEVEL_2801_8_PID	0X2881	/* SeaLINK+8/232 (2801) Port 8 */
#define SEALEVEL_2802_1_PID	0X2812	/* SeaLINK+8/485 (2802) Port 1 */
#define SEALEVEL_2802_2_PID	0X2822	/* SeaLINK+8/485 (2802) Port 2 */
#define SEALEVEL_2802_3_PID	0X2832	/* SeaLINK+8/485 (2802) Port 3 */
#define SEALEVEL_2802_4_PID	0X2842	/* SeaLINK+8/485 (2802) Port 4 */
#define SEALEVEL_2802_5_PID	0X2852	/* SeaLINK+8/485 (2802) Port 5 */
#define SEALEVEL_2802_6_PID	0X2862	/* SeaLINK+8/485 (2802) Port 6 */
#define SEALEVEL_2802_7_PID	0X2872	/* SeaLINK+8/485 (2802) Port 7 */
#define SEALEVEL_2802_8_PID	0X2882	/* SeaLINK+8/485 (2802) Port 8 */
#define SEALEVEL_2803_1_PID	0X2813	/* SeaLINK+8 (2803) Port 1 */
#define SEALEVEL_2803_2_PID	0X2823 	/* SeaLINK+8 (2803) Port 2 */
#define SEALEVEL_2803_3_PID	0X2833 	/* SeaLINK+8 (2803) Port 3 */
#define SEALEVEL_2803_4_PID	0X2843 	/* SeaLINK+8 (2803) Port 4 */
#define SEALEVEL_2803_5_PID	0X2853 	/* SeaLINK+8 (2803) Port 5 */
#define SEALEVEL_2803_6_PID	0X2863 	/* SeaLINK+8 (2803) Port 6 */
#define SEALEVEL_2803_7_PID	0X2873 	/* SeaLINK+8 (2803) Port 7 */
#define SEALEVEL_2803_8_PID	0X2883 	/* SeaLINK+8 (2803) Port 8 */

/*
 * DSS-20 Sync Station for Sony Ericsson P800
 */
 
#define FTDI_DSS20_PID          0xFC82  

/*
 * Home Electronics (www.home-electro.com) USB gadgets
 */
#define FTDI_HE_TIRA1_PID	0xFA78	/* Tira-1 IR tranceiver */

/* USB-UIRT - An infrared receiver and transmitter using the 8U232AM chip */
/* http://home.earthlink.net/~jrhees/USBUIRT/index.htm */
#define FTDI_USB_UIRT_PID	0xF850	/* Product Id */

/* ELV USB Module UO100 (PID sent by Stefan Frings) */
#define FTDI_ELV_UO100_PID	0xFB58	/* Product Id */

/*
 * Definitions for ID TECH (www.idt-net.com) devices
 */
#define IDTECH_VID		0x0ACD	/* ID TECH Vendor ID */
#define IDTECH_IDT1221U_PID	0x0300	/* IDT1221U USB to RS-232 adapter */

/*
 * Definitions for Omnidirectional Control Technology, Inc. devices
 */
#define OCT_VID			0x0B39	/* OCT vendor ID */
/* Note: OCT US101 is also rebadged as Dick Smith Electronics (NZ) XH6381 */
/* Also rebadged as SIIG Inc. model US2308 */
#define OCT_US101_PID		0x0421	/* OCT US101 USB to RS-232 */

/* an infrared receiver for user access control with IR tags */
#define FTDI_PIEGROUP_PID	0xF208	/* Product Id */

/*
 * Protego product ids
 */
#define PROTEGO_SPECIAL_1	0xFC70	/* special/unknown device */
#define PROTEGO_R2X0		0xFC71	/* R200-USB TRNG unit (R210, R220, and R230) */
#define PROTEGO_SPECIAL_3	0xFC72	/* special/unknown device */
#define PROTEGO_SPECIAL_4	0xFC73	/* special/unknown device */

/*
 * Gude Analog- und Digitalsysteme GmbH
 */
#define FTDI_GUDEADS_E808_PID    0xE808
#define FTDI_GUDEADS_E809_PID    0xE809
#define FTDI_GUDEADS_E80A_PID    0xE80A
#define FTDI_GUDEADS_E80B_PID    0xE80B
#define FTDI_GUDEADS_E80C_PID    0xE80C
#define FTDI_GUDEADS_E80D_PID    0xE80D
#define FTDI_GUDEADS_E80E_PID    0xE80E
#define FTDI_GUDEADS_E80F_PID    0xE80F
#define FTDI_GUDEADS_E888_PID    0xE888  /* Expert ISDN Control USB */
#define FTDI_GUDEADS_E889_PID    0xE889  /* USB RS-232 OptoBridge */
#define FTDI_GUDEADS_E88A_PID    0xE88A
#define FTDI_GUDEADS_E88B_PID    0xE88B
#define FTDI_GUDEADS_E88C_PID    0xE88C
#define FTDI_GUDEADS_E88D_PID    0xE88D
#define FTDI_GUDEADS_E88E_PID    0xE88E
#define FTDI_GUDEADS_E88F_PID    0xE88F

/*
 * Linx Technologies product ids
 */
#define LINX_SDMUSBQSS_PID	0xF448	/* Linx SDM-USB-QS-S */
#define LINX_MASTERDEVEL2_PID   0xF449   /* Linx Master Development 2.0 */
#define LINX_FUTURE_0_PID   0xF44A   /* Linx future device */
#define LINX_FUTURE_1_PID   0xF44B   /* Linx future device */
#define LINX_FUTURE_2_PID   0xF44C   /* Linx future device */

/* CCS Inc. ICDU/ICDU40 product ID - the FT232BM is used in an in-circuit-debugger */
/* unit for PIC16's/PIC18's */
#define FTDI_CCSICDU20_0_PID    0xF9D0     
#define FTDI_CCSICDU40_1_PID    0xF9D1     

/* Inside Accesso contactless reader (http://www.insidefr.com) */
#define INSIDE_ACCESSO		0xFAD0

/*
 * Intrepid Control Systems (http://www.intrepidcs.com/) ValueCAN and NeoVI
 */
#define INTREPID_VID		0x093C
#define INTREPID_VALUECAN_PID	0x0601
#define INTREPID_NEOVI_PID	0x0701

/*
 * Falcom Wireless Communications GmbH
 */
#define FALCOM_VID		0x0F94	/* Vendor Id */
#define FALCOM_TWIST_PID	0x0001	/* Falcom Twist USB GPRS modem */

/*
 * SUUNTO product ids
 */
#define FTDI_SUUNTO_SPORTS_PID	0xF680	/* Suunto Sports instrument */

/* Commands */
#define FTDI_SIO_RESET 		0 /* Reset the port */
#define FTDI_SIO_MODEM_CTRL 	1 /* Set the modem control register */
#define FTDI_SIO_SET_FLOW_CTRL	2 /* Set flow control register */
#define FTDI_SIO_SET_BAUD_RATE	3 /* Set baud rate */
#define FTDI_SIO_SET_DATA	4 /* Set the data characteristics of the port */
#define FTDI_SIO_GET_MODEM_STATUS	5 /* Retrieve current value of modern status register */
#define FTDI_SIO_SET_EVENT_CHAR	6 /* Set the event character */
#define FTDI_SIO_SET_ERROR_CHAR	7 /* Set the error character */

/* Port Identifier Table */
#define PIT_DEFAULT 		0 /* SIOA */
#define PIT_SIOA		1 /* SIOA */
/* The device this driver is tested with one has only one port */
#define PIT_SIOB		2 /* SIOB */
#define PIT_PARALLEL		3 /* Parallel */

/* FTDI_SIO_RESET */
#define FTDI_SIO_RESET_REQUEST FTDI_SIO_RESET
#define FTDI_SIO_RESET_REQUEST_TYPE 0x40
#define FTDI_SIO_RESET_SIO 0
#define FTDI_SIO_RESET_PURGE_RX 1
#define FTDI_SIO_RESET_PURGE_TX 2

/*
 * BmRequestType:  0100 0000B
 * bRequest:       FTDI_SIO_RESET
 * wValue:         Control Value 
 *                   0 = Reset SIO
 *                   1 = Purge RX buffer
 *                   2 = Purge TX buffer
 * wIndex:         Port
 * wLength:        0
 * Data:           None
 *
 * The Reset SIO command has this effect:
 *
 *    Sets flow control set to 'none'
 *    Event char = $0D
 *    Event trigger = disabled
 *    Purge RX buffer
 *    Purge TX buffer
 *    Clear DTR
 *    Clear RTS
 *    baud and data format not reset
 *
 * The Purge RX and TX buffer commands affect nothing except the buffers
 *
   */

/* FTDI_SIO_SET_BAUDRATE */
#define FTDI_SIO_SET_BAUDRATE_REQUEST_TYPE 0x40
#define FTDI_SIO_SET_BAUDRATE_REQUEST 3

/*
 * BmRequestType:  0100 0000B
 * bRequest:       FTDI_SIO_SET_BAUDRATE
 * wValue:         BaudDivisor value - see below
 * wIndex:         Port
 * wLength:        0
 * Data:           None
 * The BaudDivisor values are calculated as follows:
 * - BaseClock is either 12000000 or 48000000 depending on the device. FIXME: I wish
 *   I knew how to detect old chips to select proper base clock!
 * - BaudDivisor is a fixed point number encoded in a funny way.
 *   (--WRONG WAY OF THINKING--)
 *   BaudDivisor is a fixed point number encoded with following bit weighs:
 *   (-2)(-1)(13..0). It is a radical with a denominator of 4, so values
 *   end with 0.0 (00...), 0.25 (10...), 0.5 (01...), and 0.75 (11...).
 *   (--THE REALITY--)
 *   The both-bits-set has quite different meaning from 0.75 - the chip designers
 *   have decided it to mean 0.125 instead of 0.75.
 *   This info looked up in FTDI application note "FT8U232 DEVICES \ Data Rates
 *   and Flow Control Consideration for USB to RS232".
 * - BaudDivisor = (BaseClock / 16) / BaudRate, where the (=) operation should
 *   automagically re-encode the resulting value to take fractions into consideration.
 * As all values are integers, some bit twiddling is in order:
 *   BaudDivisor = (BaseClock / 16 / BaudRate) |
 *   (((BaseClock / 2 / BaudRate) & 4) ? 0x4000    // 0.5
 *    : ((BaseClock / 2 / BaudRate) & 2) ? 0x8000  // 0.25
 *    : ((BaseClock / 2 / BaudRate) & 1) ? 0xc000  // 0.125
 *    : 0)
 *
 * For the FT232BM, a 17th divisor bit was introduced to encode the multiples
 * of 0.125 missing from the FT8U232AM.  Bits 16 to 14 are coded as follows
 * (the first four codes are the same as for the FT8U232AM, where bit 16 is
 * always 0):
 *   000 - add .000 to divisor
 *   001 - add .500 to divisor
 *   010 - add .250 to divisor
 *   011 - add .125 to divisor
 *   100 - add .375 to divisor
 *   101 - add .625 to divisor
 *   110 - add .750 to divisor
 *   111 - add .875 to divisor
 * Bits 15 to 0 of the 17-bit divisor are placed in the urb value.  Bit 16 is 
 * placed in bit 0 of the urb index.
 *
 * Note that there are a couple of special cases to support the highest baud
 * rates.  If the calculated divisor value is 1, this needs to be replaced with
 * 0.  Additionally for the FT232BM, if the calculated divisor value is 0x4001
 * (1.5), this needs to be replaced with 0x0001 (1) (but this divisor value is
 * not supported by the FT8U232AM).
 */

typedef enum {
	SIO = 1,
	FT8U232AM = 2,
	FT232BM = 3,
} ftdi_chip_type_t;

typedef enum {
 ftdi_sio_b300 = 0, 
 ftdi_sio_b600 = 1, 
 ftdi_sio_b1200 = 2,
 ftdi_sio_b2400 = 3,
 ftdi_sio_b4800 = 4,
 ftdi_sio_b9600 = 5,
 ftdi_sio_b19200 = 6,
 ftdi_sio_b38400 = 7,
 ftdi_sio_b57600 = 8,
 ftdi_sio_b115200 = 9
} FTDI_SIO_baudrate_t ;

/*
 * The ftdi_8U232AM_xxMHz_byyy constants have been removed. The encoded divisor values
 * are calculated internally.
 */

#define FTDI_SIO_SET_DATA_REQUEST FTDI_SIO_SET_DATA
#define FTDI_SIO_SET_DATA_REQUEST_TYPE 0x40
#define FTDI_SIO_SET_DATA_PARITY_NONE (0x0 << 8 )
#define FTDI_SIO_SET_DATA_PARITY_ODD (0x1 << 8 )
#define FTDI_SIO_SET_DATA_PARITY_EVEN (0x2 << 8 )
#define FTDI_SIO_SET_DATA_PARITY_MARK (0x3 << 8 )
#define FTDI_SIO_SET_DATA_PARITY_SPACE (0x4 << 8 )
#define FTDI_SIO_SET_DATA_STOP_BITS_1 (0x0 << 11 )
#define FTDI_SIO_SET_DATA_STOP_BITS_15 (0x1 << 11 )
#define FTDI_SIO_SET_DATA_STOP_BITS_2 (0x2 << 11 )
#define FTDI_SIO_SET_BREAK (0x1 << 14)
/* FTDI_SIO_SET_DATA */

/*
 * BmRequestType:  0100 0000B 
 * bRequest:       FTDI_SIO_SET_DATA
 * wValue:         Data characteristics (see below)
 * wIndex:         Port
 * wLength:        0
 * Data:           No
 *
 * Data characteristics
 *
 *   B0..7   Number of data bits
 *   B8..10  Parity
 *           0 = None
 *           1 = Odd
 *           2 = Even
 *           3 = Mark
 *           4 = Space
 *   B11..13 Stop Bits
 *           0 = 1
 *           1 = 1.5
 *           2 = 2
 *   B14
 *           1 = TX ON (break)
 *           0 = TX OFF (normal state)
 *   B15 Reserved
 *
 */



/* FTDI_SIO_MODEM_CTRL */
#define FTDI_SIO_SET_MODEM_CTRL_REQUEST_TYPE 0x40
#define FTDI_SIO_SET_MODEM_CTRL_REQUEST FTDI_SIO_MODEM_CTRL

/* 
 * BmRequestType:   0100 0000B
 * bRequest:        FTDI_SIO_MODEM_CTRL
 * wValue:          ControlValue (see below)
 * wIndex:          Port
 * wLength:         0
 * Data:            None
 *
 * NOTE: If the device is in RTS/CTS flow control, the RTS set by this
 * command will be IGNORED without an error being returned
 * Also - you can not set DTR and RTS with one control message
 */

#define FTDI_SIO_SET_DTR_MASK 0x1
#define FTDI_SIO_SET_DTR_HIGH ( 1 | ( FTDI_SIO_SET_DTR_MASK  << 8))
#define FTDI_SIO_SET_DTR_LOW  ( 0 | ( FTDI_SIO_SET_DTR_MASK  << 8))
#define FTDI_SIO_SET_RTS_MASK 0x2
#define FTDI_SIO_SET_RTS_HIGH ( 2 | ( FTDI_SIO_SET_RTS_MASK << 8 ))
#define FTDI_SIO_SET_RTS_LOW ( 0 | ( FTDI_SIO_SET_RTS_MASK << 8 ))

/*
 * ControlValue
 * B0    DTR state
 *          0 = reset
 *          1 = set
 * B1    RTS state
 *          0 = reset
 *          1 = set
 * B2..7 Reserved
 * B8    DTR state enable
 *          0 = ignore
 *          1 = use DTR state
 * B9    RTS state enable
 *          0 = ignore
 *          1 = use RTS state
 * B10..15 Reserved
 */

/* FTDI_SIO_SET_FLOW_CTRL */
#define FTDI_SIO_SET_FLOW_CTRL_REQUEST_TYPE 0x40
#define FTDI_SIO_SET_FLOW_CTRL_REQUEST FTDI_SIO_SET_FLOW_CTRL
#define FTDI_SIO_DISABLE_FLOW_CTRL 0x0 
#define FTDI_SIO_RTS_CTS_HS (0x1 << 8)
#define FTDI_SIO_DTR_DSR_HS (0x2 << 8)
#define FTDI_SIO_XON_XOFF_HS (0x4 << 8)
/*
 *   BmRequestType:  0100 0000b
 *   bRequest:       FTDI_SIO_SET_FLOW_CTRL
 *   wValue:         Xoff/Xon
 *   wIndex:         Protocol/Port - hIndex is protocl / lIndex is port
 *   wLength:        0 
 *   Data:           None
 *
 * hIndex protocol is:
 *   B0 Output handshaking using RTS/CTS
 *       0 = disabled
 *       1 = enabled
 *   B1 Output handshaking using DTR/DSR
 *       0 = disabled
 *       1 = enabled
 *   B2 Xon/Xoff handshaking
 *       0 = disabled
 *       1 = enabled
 *
 * A value of zero in the hIndex field disables handshaking
 *
 * If Xon/Xoff handshaking is specified, the hValue field should contain the XOFF character 
 * and the lValue field contains the XON character.
 */  
 
/*
 * FTDI_SIO_SET_EVENT_CHAR 
 *
 * Set the special event character for the specified communications port.
 * If the device sees this character it will immediately return the
 * data read so far - rather than wait 40ms or until 62 bytes are read
 * which is what normally happens.
 */


#define  FTDI_SIO_SET_EVENT_CHAR_REQUEST FTDI_SIO_SET_EVENT_CHAR
#define  FTDI_SIO_SET_EVENT_CHAR_REQUEST_TYPE 0x40


/* 
 *  BmRequestType:   0100 0000b
 *  bRequest:        FTDI_SIO_SET_EVENT_CHAR
 *  wValue:          EventChar
 *  wIndex:          Port
 *  wLength:         0
 *  Data:            None
 *
 * wValue:
 *   B0..7   Event Character
 *   B8      Event Character Processing
 *             0 = disabled
 *             1 = enabled
 *   B9..15  Reserved
 *
 */
          
/* FTDI_SIO_SET_ERROR_CHAR */

/* Set the parity error replacement character for the specified communications port */

/* 
 *  BmRequestType:  0100 0000b
 *  bRequest:       FTDI_SIO_SET_EVENT_CHAR
 *  wValue:         Error Char
 *  wIndex:         Port
 *  wLength:        0
 *  Data:           None
 *
 *Error Char
 *  B0..7  Error Character
 *  B8     Error Character Processing
 *           0 = disabled
 *           1 = enabled
 *  B9..15 Reserved
 *
 */

/* FTDI_SIO_GET_MODEM_STATUS */
/* Retreive the current value of the modem status register */

#define FTDI_SIO_GET_MODEM_STATUS_REQUEST_TYPE 0xc0
#define FTDI_SIO_GET_MODEM_STATUS_REQUEST FTDI_SIO_GET_MODEM_STATUS
#define FTDI_SIO_CTS_MASK 0x10
#define FTDI_SIO_DSR_MASK 0x20
#define FTDI_SIO_RI_MASK  0x40
#define FTDI_SIO_RLSD_MASK 0x80
/* 
 *   BmRequestType:   1100 0000b
 *   bRequest:        FTDI_SIO_GET_MODEM_STATUS
 *   wValue:          zero
 *   wIndex:          Port
 *   wLength:         1
 *   Data:            Status
 * 
 * One byte of data is returned 
 * B0..3 0
 * B4    CTS
 *         0 = inactive
 *         1 = active
 * B5    DSR
 *         0 = inactive
 *         1 = active
 * B6    Ring Indicator (RI)
 *         0 = inactive
 *         1 = active
 * B7    Receive Line Signal Detect (RLSD)
 *         0 = inactive
 *         1 = active 
 */



/* Descriptors returned by the device 
 * 
 *  Device Descriptor
 * 
 * Offset	Field		Size	Value	Description
 * 0	bLength		1	0x12	Size of descriptor in bytes
 * 1	bDescriptorType	1	0x01	DEVICE Descriptor Type
 * 2	bcdUSB		2	0x0110	USB Spec Release Number
 * 4	bDeviceClass	1	0x00	Class Code
 * 5	bDeviceSubClass	1	0x00	SubClass Code
 * 6	bDeviceProtocol	1	0x00	Protocol Code
 * 7	bMaxPacketSize0 1	0x08	Maximum packet size for endpoint 0
 * 8	idVendor	2	0x0403	Vendor ID
 * 10	idProduct	2	0x8372	Product ID (FTDI_SIO_PID)
 * 12	bcdDevice	2	0x0001	Device release number
 * 14	iManufacturer	1	0x01	Index of man. string desc
 * 15	iProduct	1	0x02	Index of prod string desc
 * 16	iSerialNumber	1	0x02	Index of serial nmr string desc
 * 17	bNumConfigurations 1    0x01	Number of possible configurations
 * 
 * Configuration Descriptor
 * 
 * Offset	Field			Size	Value
 * 0	bLength			1	0x09	Size of descriptor in bytes
 * 1	bDescriptorType		1	0x02	CONFIGURATION Descriptor Type
 * 2	wTotalLength		2	0x0020	Total length of data
 * 4	bNumInterfaces		1	0x01	Number of interfaces supported
 * 5	bConfigurationValue	1	0x01	Argument for SetCOnfiguration() req
 * 6	iConfiguration		1	0x02	Index of config string descriptor
 * 7	bmAttributes		1	0x20	Config characteristics Remote Wakeup
 * 8	MaxPower		1	0x1E	Max power consumption
 * 
 * Interface Descriptor
 * 
 * Offset	Field			Size	Value
 * 0	bLength			1	0x09	Size of descriptor in bytes
 * 1	bDescriptorType		1	0x04	INTERFACE Descriptor Type
 * 2	bInterfaceNumber	1	0x00	Number of interface
 * 3	bAlternateSetting	1	0x00	Value used to select alternate
 * 4	bNumEndpoints		1	0x02	Number of endpoints
 * 5	bInterfaceClass		1	0xFF	Class Code
 * 6	bInterfaceSubClass	1	0xFF	Subclass Code
 * 7	bInterfaceProtocol	1	0xFF	Protocol Code
 * 8	iInterface		1	0x02	Index of interface string description
 * 
 * IN Endpoint Descriptor
 * 
 * Offset	Field			Size	Value
 * 0	bLength			1	0x07	Size of descriptor in bytes
 * 1	bDescriptorType		1	0x05	ENDPOINT descriptor type
 * 2	bEndpointAddress	1	0x82	Address of endpoint
 * 3	bmAttributes		1	0x02	Endpoint attributes - Bulk
 * 4	bNumEndpoints		2	0x0040	maximum packet size
 * 5	bInterval		1	0x00	Interval for polling endpoint
 * 
 * OUT Endpoint Descriptor
 * 
 * Offset	Field			Size	Value
 * 0	bLength			1	0x07	Size of descriptor in bytes
 * 1	bDescriptorType		1	0x05	ENDPOINT descriptor type
 * 2	bEndpointAddress	1	0x02	Address of endpoint
 * 3	bmAttributes		1	0x02	Endpoint attributes - Bulk
 * 4	bNumEndpoints		2	0x0040	maximum packet size
 * 5	bInterval		1	0x00	Interval for polling endpoint
 *     
 * DATA FORMAT
 * 
 * IN Endpoint
 * 
 * The device reserves the first two bytes of data on this endpoint to contain the current
 * values of the modem and line status registers. In the absence of data, the device 
 * generates a message consisting of these two status bytes every 40 ms
 * 
 * Byte 0: Modem Status
 * 
 * Offset	Description
 * B0	Reserved - must be 1
 * B1	Reserved - must be 0
 * B2	Reserved - must be 0
 * B3	Reserved - must be 0
 * B4	Clear to Send (CTS)
 * B5	Data Set Ready (DSR)
 * B6	Ring Indicator (RI)
 * B7	Receive Line Signal Detect (RLSD)
 * 
 * Byte 1: Line Status
 * 
 * Offset	Description
 * B0	Data Ready (DR)
 * B1	Overrun Error (OE)
 * B2	Parity Error (PE)
 * B3	Framing Error (FE)
 * B4	Break Interrupt (BI)
 * B5	Transmitter Holding Register (THRE)
 * B6	Transmitter Empty (TEMT)
 * B7	Error in RCVR FIFO
 * 
 */
#define FTDI_RS0_CTS	(1 << 4)
#define FTDI_RS0_DSR	(1 << 5)
#define FTDI_RS0_RI	(1 << 6)
#define FTDI_RS0_RLSD	(1 << 7)

#define FTDI_RS_DR  1
#define FTDI_RS_OE (1<<1)
#define FTDI_RS_PE (1<<2)
#define FTDI_RS_FE (1<<3)
#define FTDI_RS_BI (1<<4)
#define FTDI_RS_THRE (1<<5)
#define FTDI_RS_TEMT (1<<6)
#define FTDI_RS_FIFO  (1<<7)

/*
 * OUT Endpoint
 * 
 * This device reserves the first bytes of data on this endpoint contain the length
 * and port identifier of the message. For the FTDI USB Serial converter the port 
 * identifier is always 1.
 * 
 * Byte 0: Line Status
 * 
 * Offset	Description
 * B0	Reserved - must be 1
 * B1	Reserved - must be 0
 * B2..7	Length of message - (not including Byte 0)
 * 
 */

