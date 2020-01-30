/*********************************************************************
 *                
 * Filename:      ircomm_param.c
 * Version:       1.0
 * Description:   Parameter handling for the IrCOMM protocol
 * Status:        Experimental.
 * Author:        Dag Brattli <dagb@cs.uit.no>
 * Created at:    Mon Jun  7 10:25:11 1999
 * Modified at:   Sun Jan 30 14:32:03 2000
 * Modified by:   Dag Brattli <dagb@cs.uit.no>
 * 
 *     Copyright (c) 1999-2000 Dag Brattli, All Rights Reserved.
 *     
 *     This program is free software; you can redistribute it and/or 
 *     modify it under the terms of the GNU General Public License as 
 *     published by the Free Software Foundation; either version 2 of 
 *     the License, or (at your option) any later version.
 * 
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *     GNU General Public License for more details.
 * 
 *     You should have received a copy of the GNU General Public License 
 *     along with this program; if not, write to the Free Software 
 *     Foundation, Inc., 59 Temple Place, Suite 330, Boston, 
 *     MA 02111-1307 USA
 *     
 ********************************************************************/

#include <linux/sched.h>
#include <linux/interrupt.h>

#include <net/irda/irda.h>
#include <net/irda/parameters.h>

#include <net/irda/ircomm_core.h>
#include <net/irda/ircomm_tty_attach.h>
#include <net/irda/ircomm_tty.h>

#include <net/irda/ircomm_param.h>

static int ircomm_param_service_type(void *instance, irda_param_t *param, 
				     int get);
static int ircomm_param_port_type(void *instance, irda_param_t *param, 
				  int get);
static int ircomm_param_port_name(void *instance, irda_param_t *param, 
				  int get);
static int ircomm_param_service_type(void *instance, irda_param_t *param, 
				     int get);
static int ircomm_param_data_rate(void *instance, irda_param_t *param, 
				  int get);
static int ircomm_param_data_format(void *instance, irda_param_t *param, 
				    int get);
static int ircomm_param_flow_control(void *instance, irda_param_t *param, 
				     int get);
static int ircomm_param_xon_xoff(void *instance, irda_param_t *param, int get);
static int ircomm_param_enq_ack(void *instance, irda_param_t *param, int get);
static int ircomm_param_line_status(void *instance, irda_param_t *param, 
				    int get);
static int ircomm_param_dte(void *instance, irda_param_t *param, int get);
static int ircomm_param_dce(void *instance, irda_param_t *param, int get);
static int ircomm_param_poll(void *instance, irda_param_t *param, int get);

static pi_minor_info_t pi_minor_call_table_common[] = {
	{ ircomm_param_service_type, PV_INT_8_BITS },
	{ ircomm_param_port_type,    PV_INT_8_BITS },
	{ ircomm_param_port_name,    PV_STRING }
};
static pi_minor_info_t pi_minor_call_table_non_raw[] = {
	{ ircomm_param_data_rate,    PV_INT_32_BITS | PV_BIG_ENDIAN },
	{ ircomm_param_data_format,  PV_INT_8_BITS },
	{ ircomm_param_flow_control, PV_INT_8_BITS },
	{ ircomm_param_xon_xoff,     PV_INT_16_BITS },
	{ ircomm_param_enq_ack,      PV_INT_16_BITS },
	{ ircomm_param_line_status,  PV_INT_8_BITS }
};
static pi_minor_info_t pi_minor_call_table_9_wire[] = {
	{ ircomm_param_dte,          PV_INT_8_BITS },
	{ ircomm_param_dce,          PV_INT_8_BITS },
	{ ircomm_param_poll,         PV_NO_VALUE },
};

static pi_major_info_t pi_major_call_table[] = {
	{ pi_minor_call_table_common,  3 },
	{ pi_minor_call_table_non_raw, 6 },
 	{ pi_minor_call_table_9_wire,  3 }
/* 	{ pi_minor_call_table_centronics }  */
};

pi_param_info_t ircomm_param_info = { pi_major_call_table, 3, 0x0f, 4 };

/*
 * Function ircomm_param_flush (self)
 *
 *    Flush (send) out all queued parameters
 *
 */
int ircomm_param_flush(struct ircomm_tty_cb *self)
{
	if (self->ctrl_skb) {
		ircomm_control_request(self->ircomm, self->ctrl_skb);
		self->ctrl_skb = NULL;	
	}
	return 0;
}

/*
 * Function ircomm_param_request (self, pi, flush)
 *
 *    Queue a parameter for the control channel
 *
 */
int ircomm_param_request(struct ircomm_tty_cb *self, __u8 pi, int flush)
{
	struct tty_struct *tty;
	unsigned long flags;
	struct sk_buff *skb;
	int count;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);

	tty = self->tty;
	if (!tty)
		return 0;

	/* Make sure we don't send parameters for raw mode */
	if (self->service_type == IRCOMM_3_WIRE_RAW)
		return 0;

	save_flags(flags);
	cli();

	skb = self->ctrl_skb;	
	if (!skb) {
		skb = dev_alloc_skb(256);
		if (!skb) {
			restore_flags(flags);
			return -ENOMEM;
		}
		
		skb_reserve(skb, self->max_header_size);
		self->ctrl_skb = skb;
	}
	/* 
	 * Inserting is a little bit tricky since we don't know how much
	 * room we will need. But this should hopefully work OK 
	 */
	count = irda_param_insert(self, pi, skb->tail, skb_tailroom(skb),
				  &ircomm_param_info);
	if (count < 0) {
		WARNING(__FUNCTION__ "(), no room for parameter!\n");
		restore_flags(flags);
		return -1;
	}
	skb_put(skb, count);

	restore_flags(flags);

	IRDA_DEBUG(2, __FUNCTION__ "(), skb->len=%d\n", skb->len);

	if (flush) {
		/* ircomm_tty_do_softint will take care of the rest */
		queue_task(&self->tqueue, &tq_immediate);
		mark_bh(IMMEDIATE_BH);
	}

	return count;
}

/*
 * Function ircomm_param_service_type (self, buf, len)
 *
 *    Handle service type, this function will both be called after the LM-IAS
 *    query and then the remote device sends its initial paramters
 *
 */
static int ircomm_param_service_type(void *instance, irda_param_t *param, 
				     int get)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;
	__u8 service_type = param->pv.b; /* We know it's a one byte integer */

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);

	if (get) {
		param->pv.b = self->settings.service_type;
		return 0;
	}

	/* Find all common service types */
	service_type &= self->service_type;
	if (!service_type) {
		IRDA_DEBUG(2, __FUNCTION__
			   "(), No common service type to use!\n");
		return -1;
	}
	IRDA_DEBUG(0, __FUNCTION__ "(), services in common=%02x\n",
		   service_type);

	/*
	 * Now choose a preferred service type of those available
	 */
	if (service_type & IRCOMM_CENTRONICS)
		self->settings.service_type = IRCOMM_CENTRONICS;
	else if (service_type & IRCOMM_9_WIRE)
		self->settings.service_type = IRCOMM_9_WIRE;
	else if (service_type & IRCOMM_3_WIRE)
		self->settings.service_type = IRCOMM_3_WIRE;
	else if (service_type & IRCOMM_3_WIRE_RAW)
		self->settings.service_type = IRCOMM_3_WIRE_RAW;

	IRDA_DEBUG(0, __FUNCTION__ "(), resulting service type=0x%02x\n", 
		   self->settings.service_type);

	/* 
	 * Now the line is ready for some communication. Check if we are a
         * server, and send over some initial parameters 
	 */
	if (!self->client && (self->settings.service_type != IRCOMM_3_WIRE_RAW))
	{
		/* Init connection */
		ircomm_tty_send_initial_parameters(self);
		ircomm_tty_link_established(self);
	}

	return 0;
}

/*
 * Function ircomm_param_port_type (self, param)
 *
 *    The port type parameter tells if the devices are serial or parallel.
 *    Since we only advertise serial service, this parameter should only
 *    be equal to IRCOMM_SERIAL.
 */
static int ircomm_param_port_type(void *instance, irda_param_t *param, int get)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);
	
	if (get)
		param->pv.b = IRCOMM_SERIAL;
	else {
		self->settings.port_type = param->pv.b;

		IRDA_DEBUG(0, __FUNCTION__ "(), port type=%d\n", 
			   self->settings.port_type);
	}
	return 0;
}

/*
 * Function ircomm_param_port_name (self, param)
 *
 *    Exchange port name
 *
 */
static int ircomm_param_port_name(void *instance, irda_param_t *param, int get)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;
	
	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);

	if (get) {
		IRDA_DEBUG(0, __FUNCTION__ "(), not imp!\n");
	} else {
		IRDA_DEBUG(0, __FUNCTION__ "(), port-name=%s\n", param->pv.c);
		strncpy(self->settings.port_name, param->pv.c, 32);
	}

	return 0;
}

/*
 * Function ircomm_param_data_rate (self, param)
 *
 *    Exchange data rate to be used in this settings
 *
 */
static int ircomm_param_data_rate(void *instance, irda_param_t *param, int get)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;
	
	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);

	if (get)
		param->pv.i = self->settings.data_rate;
	else
		self->settings.data_rate = param->pv.i;
	
	IRDA_DEBUG(2, __FUNCTION__ "(), data rate = %d\n", param->pv.i);

	return 0;
}

/*
 * Function ircomm_param_data_format (self, param)
 *
 *    Exchange data format to be used in this settings
 *
 */
static int ircomm_param_data_format(void *instance, irda_param_t *param, 
				    int get)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);

	if (get)
		param->pv.b = self->settings.data_format;
	else
		self->settings.data_format = param->pv.b;
	
	return 0;
}

/*
 * Function ircomm_param_flow_control (self, param)
 *
 *    Exchange flow control settings to be used in this settings
 *
 */
static int ircomm_param_flow_control(void *instance, irda_param_t *param, 
				     int get)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);
	
	if (get)
		param->pv.b = self->settings.flow_control;
	else
		self->settings.flow_control = param->pv.b;

	IRDA_DEBUG(1, __FUNCTION__ "(), flow control = 0x%02x\n", param->pv.b);

	return 0;
}

/*
 * Function ircomm_param_xon_xoff (self, param)
 *
 *    Exchange XON/XOFF characters
 *
 */
static int ircomm_param_xon_xoff(void *instance, irda_param_t *param, int get)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);
	
	if (get) {
		param->pv.s = self->settings.xonxoff[0];
		param->pv.s |= self->settings.xonxoff[1] << 8;
	} else {
		self->settings.xonxoff[0] = param->pv.s & 0xff;
		self->settings.xonxoff[1] = param->pv.s >> 8;
	}

	IRDA_DEBUG(0, __FUNCTION__ "(), XON/XOFF = 0x%02x,0x%02x\n", 
		   param->pv.s & 0xff, param->pv.s >> 8);

	return 0;
}

/*
 * Function ircomm_param_enq_ack (self, param)
 *
 *    Exchange ENQ/ACK characters
 *
 */
static int ircomm_param_enq_ack(void *instance, irda_param_t *param, int get)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);
	
	if (get) {
		param->pv.s = self->settings.enqack[0];
		param->pv.s |= self->settings.enqack[1] << 8;
	} else {
		self->settings.enqack[0] = param->pv.s & 0xff;
		self->settings.enqack[1] = param->pv.s >> 8;
	}

	IRDA_DEBUG(0, __FUNCTION__ "(), ENQ/ACK = 0x%02x,0x%02x\n",
		   param->pv.s & 0xff, param->pv.s >> 8);

	return 0;
}

/*
 * Function ircomm_param_line_status (self, param)
 *
 *    
 *
 */
static int ircomm_param_line_status(void *instance, irda_param_t *param, 
				    int get)
{
	IRDA_DEBUG(2, __FUNCTION__ "(), not impl.\n");

	return 0;
}

/*
 * Function ircomm_param_dte (instance, param)
 *
 *    If we get here, there must be some sort of null-modem connection, and
 *    we are probably working in server mode as well.
 */
static int ircomm_param_dte(void *instance, irda_param_t *param, int get)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;
	__u8 dte;

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);

	if (get)
		param->pv.b = self->settings.dte;
	else {
		dte = param->pv.b;
		
		if (dte & IRCOMM_DELTA_DTR)
			self->settings.dce |= (IRCOMM_DELTA_DSR|
					      IRCOMM_DELTA_RI |
					      IRCOMM_DELTA_CD);
		if (dte & IRCOMM_DTR)
			self->settings.dce |= (IRCOMM_DSR|
					      IRCOMM_RI |
					      IRCOMM_CD);
		
		if (dte & IRCOMM_DELTA_RTS)
			self->settings.dce |= IRCOMM_DELTA_CTS;
		if (dte & IRCOMM_RTS)
			self->settings.dce |= IRCOMM_CTS;

		/* Take appropriate actions */
		ircomm_tty_check_modem_status(self);

		/* Null modem cable emulator */
		self->settings.null_modem = TRUE;
	}

	return 0;
}

/*
 * Function ircomm_param_dce (instance, param)
 *
 *    
 *
 */
static int ircomm_param_dce(void *instance, irda_param_t *param, int get)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;
	__u8 dce;

	IRDA_DEBUG(1, __FUNCTION__ "(), dce = 0x%02x\n", param->pv.b);

	dce = param->pv.b;

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);

	self->settings.dce = dce;

	/* Check if any of the settings have changed */
	if (dce & 0x0f) {
		if (dce & IRCOMM_DELTA_CTS) {
			IRDA_DEBUG(2, __FUNCTION__ "(), CTS \n");
		}
	}

	ircomm_tty_check_modem_status(self);

	return 0;
}

/*
 * Function ircomm_param_poll (instance, param)
 *
 *    Called when the peer device is polling for the line settings
 *
 */
static int ircomm_param_poll(void *instance, irda_param_t *param, int get)
{
	struct ircomm_tty_cb *self = (struct ircomm_tty_cb *) instance;

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == IRCOMM_TTY_MAGIC, return -1;);

	/* Poll parameters are always of lenght 0 (just a signal) */
	if (!get) {
		/* Respond with DTE line settings */
		ircomm_param_request(self, IRCOMM_DTE, TRUE);
	}
	return 0;
}





