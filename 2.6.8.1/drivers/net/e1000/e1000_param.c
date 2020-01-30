/*******************************************************************************

  
  Copyright(c) 1999 - 2004 Intel Corporation. All rights reserved.
  
  This program is free software; you can redistribute it and/or modify it 
  under the terms of the GNU General Public License as published by the Free 
  Software Foundation; either version 2 of the License, or (at your option) 
  any later version.
  
  This program is distributed in the hope that it will be useful, but WITHOUT 
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for 
  more details.
  
  You should have received a copy of the GNU General Public License along with
  this program; if not, write to the Free Software Foundation, Inc., 59 
  Temple Place - Suite 330, Boston, MA  02111-1307, USA.
  
  The full GNU General Public License is included in this distribution in the
  file called LICENSE.
  
  Contact Information:
  Linux NICS <linux.nics@intel.com>
  Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497

*******************************************************************************/

#include "e1000.h"

/* This is the only thing that needs to be changed to adjust the
 * maximum number of ports that the driver can manage.
 */

#define E1000_MAX_NIC 32

#define OPTION_UNSET    -1
#define OPTION_DISABLED 0
#define OPTION_ENABLED  1

/* Module Parameters are always initialized to -1, so that the driver
 * can tell the difference between no user specified value or the
 * user asking for the default value.
 * The true default values are loaded in when e1000_check_options is called.
 *
 * This is a GCC extension to ANSI C.
 * See the item "Labeled Elements in Initializers" in the section
 * "Extensions to the C Language Family" of the GCC documentation.
 */

#define E1000_PARAM_INIT { [0 ... E1000_MAX_NIC] = OPTION_UNSET }

/* All parameters are treated the same, as an integer array of values.
 * This macro just reduces the need to repeat the same declaration code
 * over and over (plus this helps to avoid typo bugs).
 */

#define E1000_PARAM(X, S) \
static const int __devinitdata X[E1000_MAX_NIC + 1] = E1000_PARAM_INIT; \
MODULE_PARM(X, "1-" __MODULE_STRING(E1000_MAX_NIC) "i"); \
MODULE_PARM_DESC(X, S);

/* Transmit Descriptor Count
 *
 * Valid Range: 80-256 for 82542 and 82543 gigabit ethernet controllers
 * Valid Range: 80-4096 for 82544 and newer
 *
 * Default Value: 256
 */

E1000_PARAM(TxDescriptors, "Number of transmit descriptors");

/* Receive Descriptor Count
 *
 * Valid Range: 80-256 for 82542 and 82543 gigabit ethernet controllers
 * Valid Range: 80-4096 for 82544 and newer
 *
 * Default Value: 256
 */

E1000_PARAM(RxDescriptors, "Number of receive descriptors");

/* User Specified Speed Override
 *
 * Valid Range: 0, 10, 100, 1000
 *  - 0    - auto-negotiate at all supported speeds
 *  - 10   - only link at 10 Mbps
 *  - 100  - only link at 100 Mbps
 *  - 1000 - only link at 1000 Mbps
 *
 * Default Value: 0
 */

E1000_PARAM(Speed, "Speed setting");

/* User Specified Duplex Override
 *
 * Valid Range: 0-2
 *  - 0 - auto-negotiate for duplex
 *  - 1 - only link at half duplex
 *  - 2 - only link at full duplex
 *
 * Default Value: 0
 */

E1000_PARAM(Duplex, "Duplex setting");

/* Auto-negotiation Advertisement Override
 *
 * Valid Range: 0x01-0x0F, 0x20-0x2F (copper); 0x20 (fiber)
 *
 * The AutoNeg value is a bit mask describing which speed and duplex
 * combinations should be advertised during auto-negotiation.
 * The supported speed and duplex modes are listed below
 *
 * Bit           7     6     5      4      3     2     1      0
 * Speed (Mbps)  N/A   N/A   1000   N/A    100   100   10     10
 * Duplex                    Full          Full  Half  Full   Half
 *
 * Default Value: 0x2F (copper); 0x20 (fiber)
 */

E1000_PARAM(AutoNeg, "Advertised auto-negotiation setting");

/* User Specified Flow Control Override
 *
 * Valid Range: 0-3
 *  - 0 - No Flow Control
 *  - 1 - Rx only, respond to PAUSE frames but do not generate them
 *  - 2 - Tx only, generate PAUSE frames but ignore them on receive
 *  - 3 - Full Flow Control Support
 *
 * Default Value: Read flow control settings from the EEPROM
 */

E1000_PARAM(FlowControl, "Flow Control setting");

/* XsumRX - Receive Checksum Offload Enable/Disable
 *
 * Valid Range: 0, 1
 *  - 0 - disables all checksum offload
 *  - 1 - enables receive IP/TCP/UDP checksum offload
 *        on 82543 and newer -based NICs
 *
 * Default Value: 1
 */

E1000_PARAM(XsumRX, "Disable or enable Receive Checksum offload");

/* Transmit Interrupt Delay in units of 1.024 microseconds
 *
 * Valid Range: 0-65535
 *
 * Default Value: 64
 */

E1000_PARAM(TxIntDelay, "Transmit Interrupt Delay");

/* Transmit Absolute Interrupt Delay in units of 1.024 microseconds
 *
 * Valid Range: 0-65535
 *
 * Default Value: 0
 */

E1000_PARAM(TxAbsIntDelay, "Transmit Absolute Interrupt Delay");

/* Receive Interrupt Delay in units of 1.024 microseconds
 *
 * Valid Range: 0-65535
 *
 * Default Value: 0
 */

E1000_PARAM(RxIntDelay, "Receive Interrupt Delay");

/* Receive Absolute Interrupt Delay in units of 1.024 microseconds
 *
 * Valid Range: 0-65535
 *
 * Default Value: 128
 */

E1000_PARAM(RxAbsIntDelay, "Receive Absolute Interrupt Delay");

/* Interrupt Throttle Rate (interrupts/sec)
 *
 * Valid Range: 100-100000 (0=off, 1=dynamic)
 *
 * Default Value: 1
 */

E1000_PARAM(InterruptThrottleRate, "Interrupt Throttling Rate");

#define AUTONEG_ADV_DEFAULT  0x2F
#define AUTONEG_ADV_MASK     0x2F
#define FLOW_CONTROL_DEFAULT FLOW_CONTROL_FULL

#define DEFAULT_RDTR                   0
#define MAX_RXDELAY               0xFFFF
#define MIN_RXDELAY                    0

#define DEFAULT_RADV                 128
#define MAX_RXABSDELAY            0xFFFF
#define MIN_RXABSDELAY                 0

#define DEFAULT_TIDV                  64
#define MAX_TXDELAY               0xFFFF
#define MIN_TXDELAY                    0

#define DEFAULT_TADV                  64
#define MAX_TXABSDELAY            0xFFFF
#define MIN_TXABSDELAY                 0

#define DEFAULT_ITR                    1
#define MAX_ITR                   100000
#define MIN_ITR                      100

struct e1000_option {
	enum { enable_option, range_option, list_option } type;
	char *name;
	char *err;
	int  def;
	union {
		struct { /* range_option info */
			int min;
			int max;
		} r;
		struct { /* list_option info */
			int nr;
			struct e1000_opt_list { int i; char *str; } *p;
		} l;
	} arg;
};

static int __devinit
e1000_validate_option(int *value, struct e1000_option *opt,
	struct e1000_adapter *adapter)
{
	if(*value == OPTION_UNSET) {
		*value = opt->def;
		return 0;
	}

	switch (opt->type) {
	case enable_option:
		switch (*value) {
		case OPTION_ENABLED:
			DPRINTK(PROBE, INFO, "%s Enabled\n", opt->name);
			return 0;
		case OPTION_DISABLED:
			DPRINTK(PROBE, INFO, "%s Disabled\n", opt->name);
			return 0;
		}
		break;
	case range_option:
		if(*value >= opt->arg.r.min && *value <= opt->arg.r.max) {
			DPRINTK(PROBE, INFO,
				"%s set to %i\n", opt->name, *value);
			return 0;
		}
		break;
	case list_option: {
		int i;
		struct e1000_opt_list *ent;

		for(i = 0; i < opt->arg.l.nr; i++) {
			ent = &opt->arg.l.p[i];
			if(*value == ent->i) {
				if(ent->str[0] != '\0')
					DPRINTK(PROBE, INFO, "%s\n", ent->str);
				return 0;
			}
		}
	}
		break;
	default:
		BUG();
	}

	DPRINTK(PROBE, INFO, "Invalid %s specified (%i) %s\n",
	       opt->name, *value, opt->err);
	*value = opt->def;
	return -1;
}

static void e1000_check_fiber_options(struct e1000_adapter *adapter);
static void e1000_check_copper_options(struct e1000_adapter *adapter);

/**
 * e1000_check_options - Range Checking for Command Line Parameters
 * @adapter: board private structure
 *
 * This routine checks all command line parameters for valid user
 * input.  If an invalid value is given, or if no user specified
 * value exists, a default value is used.  The final value is stored
 * in a variable in the adapter structure.
 **/

void __devinit
e1000_check_options(struct e1000_adapter *adapter)
{
	int bd = adapter->bd_number;
	if(bd >= E1000_MAX_NIC) {
		DPRINTK(PROBE, NOTICE,
		       "Warning: no configuration for board #%i\n", bd);
		DPRINTK(PROBE, NOTICE, "Using defaults for all values\n");
		bd = E1000_MAX_NIC;
	}

	{ /* Transmit Descriptor Count */
		struct e1000_option opt = {
			.type = range_option,
			.name = "Transmit Descriptors",
			.err  = "using default of "
				__MODULE_STRING(E1000_DEFAULT_TXD),
			.def  = E1000_DEFAULT_TXD,
			.arg  = { .r = { .min = E1000_MIN_TXD }}
		};
		struct e1000_desc_ring *tx_ring = &adapter->tx_ring;
		e1000_mac_type mac_type = adapter->hw.mac_type;
		opt.arg.r.max = mac_type < e1000_82544 ?
			E1000_MAX_TXD : E1000_MAX_82544_TXD;

		tx_ring->count = TxDescriptors[bd];
		e1000_validate_option(&tx_ring->count, &opt, adapter);
		E1000_ROUNDUP(tx_ring->count, REQ_TX_DESCRIPTOR_MULTIPLE);
	}
	{ /* Receive Descriptor Count */
		struct e1000_option opt = {
			.type = range_option,
			.name = "Receive Descriptors",
			.err  = "using default of "
				__MODULE_STRING(E1000_DEFAULT_RXD),
			.def  = E1000_DEFAULT_RXD,
			.arg  = { .r = { .min = E1000_MIN_RXD }}
		};
		struct e1000_desc_ring *rx_ring = &adapter->rx_ring;
		e1000_mac_type mac_type = adapter->hw.mac_type;
		opt.arg.r.max = mac_type < e1000_82544 ? E1000_MAX_RXD :
			E1000_MAX_82544_RXD;

		rx_ring->count = RxDescriptors[bd];
		e1000_validate_option(&rx_ring->count, &opt, adapter);
		E1000_ROUNDUP(rx_ring->count, REQ_RX_DESCRIPTOR_MULTIPLE);
	}
	{ /* Checksum Offload Enable/Disable */
		struct e1000_option opt = {
			.type = enable_option,
			.name = "Checksum Offload",
			.err  = "defaulting to Enabled",
			.def  = OPTION_ENABLED
		};

		int rx_csum = XsumRX[bd];
		e1000_validate_option(&rx_csum, &opt, adapter);
		adapter->rx_csum = rx_csum;
	}
	{ /* Flow Control */

		struct e1000_opt_list fc_list[] =
			{{ e1000_fc_none,    "Flow Control Disabled" },
			 { e1000_fc_rx_pause,"Flow Control Receive Only" },
			 { e1000_fc_tx_pause,"Flow Control Transmit Only" },
			 { e1000_fc_full,    "Flow Control Enabled" },
			 { e1000_fc_default, "Flow Control Hardware Default" }};

		struct e1000_option opt = {
			.type = list_option,
			.name = "Flow Control",
			.err  = "reading default settings from EEPROM",
			.def  = e1000_fc_default,
			.arg  = { .l = { .nr = ARRAY_SIZE(fc_list),
					 .p = fc_list }}
		};

		int fc = FlowControl[bd];
		e1000_validate_option(&fc, &opt, adapter);
		adapter->hw.fc = adapter->hw.original_fc = fc;
	}
	{ /* Transmit Interrupt Delay */
		struct e1000_option opt = {
			.type = range_option,
			.name = "Transmit Interrupt Delay",
			.err  = "using default of " __MODULE_STRING(DEFAULT_TIDV),
			.def  = DEFAULT_TIDV,
			.arg  = { .r = { .min = MIN_TXDELAY,
					 .max = MAX_TXDELAY }}
		};

		adapter->tx_int_delay = TxIntDelay[bd];
		e1000_validate_option(&adapter->tx_int_delay, &opt, adapter);
	}
	{ /* Transmit Absolute Interrupt Delay */
		struct e1000_option opt = {
			.type = range_option,
			.name = "Transmit Absolute Interrupt Delay",
			.err  = "using default of " __MODULE_STRING(DEFAULT_TADV),
			.def  = DEFAULT_TADV,
			.arg  = { .r = { .min = MIN_TXABSDELAY,
					 .max = MAX_TXABSDELAY }}
		};

		adapter->tx_abs_int_delay = TxAbsIntDelay[bd];
		e1000_validate_option(&adapter->tx_abs_int_delay, &opt, adapter);
	}
	{ /* Receive Interrupt Delay */
		struct e1000_option opt = {
			.type = range_option,
			.name = "Receive Interrupt Delay",
			.err  = "using default of " __MODULE_STRING(DEFAULT_RDTR),
			.def  = DEFAULT_RDTR,
			.arg  = { .r = { .min = MIN_RXDELAY,
					 .max = MAX_RXDELAY }}
		};

		adapter->rx_int_delay = RxIntDelay[bd];
		e1000_validate_option(&adapter->rx_int_delay, &opt, adapter);
	}
	{ /* Receive Absolute Interrupt Delay */
		struct e1000_option opt = {
			.type = range_option,
			.name = "Receive Absolute Interrupt Delay",
			.err  = "using default of " __MODULE_STRING(DEFAULT_RADV),
			.def  = DEFAULT_RADV,
			.arg  = { .r = { .min = MIN_RXABSDELAY,
					 .max = MAX_RXABSDELAY }}
		};

		adapter->rx_abs_int_delay = RxAbsIntDelay[bd];
		e1000_validate_option(&adapter->rx_abs_int_delay, &opt, adapter);
	}
	{ /* Interrupt Throttling Rate */
		struct e1000_option opt = {
			.type = range_option,
			.name = "Interrupt Throttling Rate (ints/sec)",
			.err  = "using default of " __MODULE_STRING(DEFAULT_ITR),
			.def  = DEFAULT_ITR,
			.arg  = { .r = { .min = MIN_ITR,
					 .max = MAX_ITR }}
		};

		adapter->itr = InterruptThrottleRate[bd];
		switch(adapter->itr) {
		case -1:
			adapter->itr = 1;
			break;
		case 0:
			DPRINTK(PROBE, INFO, "%s turned off\n", opt.name);
			break;
		case 1:
			DPRINTK(PROBE, INFO,
				"%s set to dynamic mode\n", opt.name);
			break;
		default:
			e1000_validate_option(&adapter->itr, &opt, adapter);
			break;
		}
	}

	switch(adapter->hw.media_type) {
	case e1000_media_type_fiber:
	case e1000_media_type_internal_serdes:
		e1000_check_fiber_options(adapter);
		break;
	case e1000_media_type_copper:
		e1000_check_copper_options(adapter);
		break;
	default:
		BUG();
	}
}

/**
 * e1000_check_fiber_options - Range Checking for Link Options, Fiber Version
 * @adapter: board private structure
 *
 * Handles speed and duplex options on fiber adapters
 **/

static void __devinit
e1000_check_fiber_options(struct e1000_adapter *adapter)
{
	int bd = adapter->bd_number;
	bd = bd > E1000_MAX_NIC ? E1000_MAX_NIC : bd;

	if((Speed[bd] != OPTION_UNSET)) {
		DPRINTK(PROBE, INFO, "Speed not valid for fiber adapters, "
		       "parameter ignored\n");
	}
	if((Duplex[bd] != OPTION_UNSET)) {
		DPRINTK(PROBE, INFO, "Duplex not valid for fiber adapters, "
		       "parameter ignored\n");
	}
	if((AutoNeg[bd] != OPTION_UNSET) && (AutoNeg[bd] != 0x20)) {
		DPRINTK(PROBE, INFO, "AutoNeg other than Full/1000 is "
		       "not valid for fiber adapters, parameter ignored\n");
	}
}

/**
 * e1000_check_copper_options - Range Checking for Link Options, Copper Version
 * @adapter: board private structure
 *
 * Handles speed and duplex options on copper adapters
 **/

static void __devinit
e1000_check_copper_options(struct e1000_adapter *adapter)
{
	int speed, dplx;
	int bd = adapter->bd_number;
	bd = bd > E1000_MAX_NIC ? E1000_MAX_NIC : bd;

	{ /* Speed */
		struct e1000_opt_list speed_list[] = {{          0, "" },
						      {   SPEED_10, "" },
						      {  SPEED_100, "" },
						      { SPEED_1000, "" }};

		struct e1000_option opt = {
			.type = list_option,
			.name = "Speed",
			.err  = "parameter ignored",
			.def  = 0,
			.arg  = { .l = { .nr = ARRAY_SIZE(speed_list),
					 .p = speed_list }}
		};

		speed = Speed[bd];
		e1000_validate_option(&speed, &opt, adapter);
	}
	{ /* Duplex */
		struct e1000_opt_list dplx_list[] = {{           0, "" },
						     { HALF_DUPLEX, "" },
						     { FULL_DUPLEX, "" }};

		struct e1000_option opt = {
			.type = list_option,
			.name = "Duplex",
			.err  = "parameter ignored",
			.def  = 0,
			.arg  = { .l = { .nr = ARRAY_SIZE(dplx_list),
					 .p = dplx_list }}
		};

		dplx = Duplex[bd];
		e1000_validate_option(&dplx, &opt, adapter);
	}

	if(AutoNeg[bd] != OPTION_UNSET && (speed != 0 || dplx != 0)) {
		DPRINTK(PROBE, INFO,
		       "AutoNeg specified along with Speed or Duplex, "
		       "parameter ignored\n");
		adapter->hw.autoneg_advertised = AUTONEG_ADV_DEFAULT;
	} else { /* Autoneg */
		struct e1000_opt_list an_list[] =
			#define AA "AutoNeg advertising "
			{{ 0x01, AA "10/HD" },
			 { 0x02, AA "10/FD" },
			 { 0x03, AA "10/FD, 10/HD" },
			 { 0x04, AA "100/HD" },
			 { 0x05, AA "100/HD, 10/HD" },
			 { 0x06, AA "100/HD, 10/FD" },
			 { 0x07, AA "100/HD, 10/FD, 10/HD" },
			 { 0x08, AA "100/FD" },
			 { 0x09, AA "100/FD, 10/HD" },
			 { 0x0a, AA "100/FD, 10/FD" },
			 { 0x0b, AA "100/FD, 10/FD, 10/HD" },
			 { 0x0c, AA "100/FD, 100/HD" },
			 { 0x0d, AA "100/FD, 100/HD, 10/HD" },
			 { 0x0e, AA "100/FD, 100/HD, 10/FD" },
			 { 0x0f, AA "100/FD, 100/HD, 10/FD, 10/HD" },
			 { 0x20, AA "1000/FD" },
			 { 0x21, AA "1000/FD, 10/HD" },
			 { 0x22, AA "1000/FD, 10/FD" },
			 { 0x23, AA "1000/FD, 10/FD, 10/HD" },
			 { 0x24, AA "1000/FD, 100/HD" },
			 { 0x25, AA "1000/FD, 100/HD, 10/HD" },
			 { 0x26, AA "1000/FD, 100/HD, 10/FD" },
			 { 0x27, AA "1000/FD, 100/HD, 10/FD, 10/HD" },
			 { 0x28, AA "1000/FD, 100/FD" },
			 { 0x29, AA "1000/FD, 100/FD, 10/HD" },
			 { 0x2a, AA "1000/FD, 100/FD, 10/FD" },
			 { 0x2b, AA "1000/FD, 100/FD, 10/FD, 10/HD" },
			 { 0x2c, AA "1000/FD, 100/FD, 100/HD" },
			 { 0x2d, AA "1000/FD, 100/FD, 100/HD, 10/HD" },
			 { 0x2e, AA "1000/FD, 100/FD, 100/HD, 10/FD" },
			 { 0x2f, AA "1000/FD, 100/FD, 100/HD, 10/FD, 10/HD" }};

		struct e1000_option opt = {
			.type = list_option,
			.name = "AutoNeg",
			.err  = "parameter ignored",
			.def  = AUTONEG_ADV_DEFAULT,
			.arg  = { .l = { .nr = ARRAY_SIZE(an_list),
					 .p = an_list }}
		};

		int an = AutoNeg[bd];
		e1000_validate_option(&an, &opt, adapter);
		adapter->hw.autoneg_advertised = an;
	}

	switch (speed + dplx) {
	case 0:
		adapter->hw.autoneg = adapter->fc_autoneg = 1;
		if(Speed[bd] != OPTION_UNSET || Duplex[bd] != OPTION_UNSET)
			DPRINTK(PROBE, INFO,
			       "Speed and duplex autonegotiation enabled\n");
		break;
	case HALF_DUPLEX:
		DPRINTK(PROBE, INFO, "Half Duplex specified without Speed\n");
		DPRINTK(PROBE, INFO,
			"Using Autonegotiation at Half Duplex only\n");
		adapter->hw.autoneg = adapter->fc_autoneg = 1;
		adapter->hw.autoneg_advertised = ADVERTISE_10_HALF |
		                                 ADVERTISE_100_HALF;
		break;
	case FULL_DUPLEX:
		DPRINTK(PROBE, INFO, "Full Duplex specified without Speed\n");
		DPRINTK(PROBE, INFO,
			"Using Autonegotiation at Full Duplex only\n");
		adapter->hw.autoneg = adapter->fc_autoneg = 1;
		adapter->hw.autoneg_advertised = ADVERTISE_10_FULL |
		                                 ADVERTISE_100_FULL |
		                                 ADVERTISE_1000_FULL;
		break;
	case SPEED_10:
		DPRINTK(PROBE, INFO,
			"10 Mbps Speed specified without Duplex\n");
		DPRINTK(PROBE, INFO, "Using Autonegotiation at 10 Mbps only\n");
		adapter->hw.autoneg = adapter->fc_autoneg = 1;
		adapter->hw.autoneg_advertised = ADVERTISE_10_HALF |
		                                 ADVERTISE_10_FULL;
		break;
	case SPEED_10 + HALF_DUPLEX:
		DPRINTK(PROBE, INFO, "Forcing to 10 Mbps Half Duplex\n");
		adapter->hw.autoneg = adapter->fc_autoneg = 0;
		adapter->hw.forced_speed_duplex = e1000_10_half;
		adapter->hw.autoneg_advertised = 0;
		break;
	case SPEED_10 + FULL_DUPLEX:
		DPRINTK(PROBE, INFO, "Forcing to 10 Mbps Full Duplex\n");
		adapter->hw.autoneg = adapter->fc_autoneg = 0;
		adapter->hw.forced_speed_duplex = e1000_10_full;
		adapter->hw.autoneg_advertised = 0;
		break;
	case SPEED_100:
		DPRINTK(PROBE, INFO,
			"100 Mbps Speed specified without Duplex\n");
		DPRINTK(PROBE, INFO,
			"Using Autonegotiation at 100 Mbps only\n");
		adapter->hw.autoneg = adapter->fc_autoneg = 1;
		adapter->hw.autoneg_advertised = ADVERTISE_100_HALF |
		                                 ADVERTISE_100_FULL;
		break;
	case SPEED_100 + HALF_DUPLEX:
		DPRINTK(PROBE, INFO, "Forcing to 100 Mbps Half Duplex\n");
		adapter->hw.autoneg = adapter->fc_autoneg = 0;
		adapter->hw.forced_speed_duplex = e1000_100_half;
		adapter->hw.autoneg_advertised = 0;
		break;
	case SPEED_100 + FULL_DUPLEX:
		DPRINTK(PROBE, INFO, "Forcing to 100 Mbps Full Duplex\n");
		adapter->hw.autoneg = adapter->fc_autoneg = 0;
		adapter->hw.forced_speed_duplex = e1000_100_full;
		adapter->hw.autoneg_advertised = 0;
		break;
	case SPEED_1000:
		DPRINTK(PROBE, INFO,
			"1000 Mbps Speed specified without Duplex\n");
		DPRINTK(PROBE, INFO,
		       "Using Autonegotiation at 1000 Mbps Full Duplex only\n");
		adapter->hw.autoneg = adapter->fc_autoneg = 1;
		adapter->hw.autoneg_advertised = ADVERTISE_1000_FULL;
		break;
	case SPEED_1000 + HALF_DUPLEX:
		DPRINTK(PROBE, INFO,
			"Half Duplex is not supported at 1000 Mbps\n");
		DPRINTK(PROBE, INFO,
		       "Using Autonegotiation at 1000 Mbps Full Duplex only\n");
		adapter->hw.autoneg = adapter->fc_autoneg = 1;
		adapter->hw.autoneg_advertised = ADVERTISE_1000_FULL;
		break;
	case SPEED_1000 + FULL_DUPLEX:
		DPRINTK(PROBE, INFO,
		       "Using Autonegotiation at 1000 Mbps Full Duplex only\n");
		adapter->hw.autoneg = adapter->fc_autoneg = 1;
		adapter->hw.autoneg_advertised = ADVERTISE_1000_FULL;
		break;
	default:
		BUG();
	}

	/* Speed, AutoNeg and MDI/MDI-X must all play nice */
	if (e1000_validate_mdi_setting(&(adapter->hw)) < 0) {
		DPRINTK(PROBE, INFO,
		       "Speed, AutoNeg and MDI-X specifications are "
		       "incompatible. Setting MDI-X to a compatible value.\n");
	}
}

