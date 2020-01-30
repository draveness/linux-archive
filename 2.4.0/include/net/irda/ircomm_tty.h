/*********************************************************************
 *                
 * Filename:      ircomm_tty.h
 * Version:       
 * Description:   
 * Status:        Experimental.
 * Author:        Dag Brattli <dagb@cs.uit.no>
 * Created at:    Sun Jun  6 23:24:22 1999
 * Modified at:   Fri Jan 28 13:16:57 2000
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

#ifndef IRCOMM_TTY_H
#define IRCOMM_TTY_H

#include <linux/serial.h>
#include <linux/termios.h>
#include <linux/timer.h>

#include <net/irda/irias_object.h>
#include <net/irda/ircomm_core.h>
#include <net/irda/ircomm_param.h>

#define IRCOMM_TTY_PORTS 32
#define IRCOMM_TTY_MAGIC 0x3432
#define IRCOMM_TTY_MAJOR 161
#define IRCOMM_TTY_MINOR 0

/*
 * IrCOMM TTY driver state
 */
struct ircomm_tty_cb {
	irda_queue_t queue;            /* Must be first */
	magic_t magic;

	int state;                /* Connect state */

	struct tty_struct *tty;
	struct ircomm_cb *ircomm; /* IrCOMM layer instance */

	struct sk_buff *tx_skb;   /* Transmit buffer */
	struct sk_buff *ctrl_skb; /* Control data buffer */

	/* Parameters */
	struct ircomm_params settings;

	__u8 service_type;        /* The service that we support */
	int client;               /* True if we are a client */
	LOCAL_FLOW flow;          /* IrTTP flow status */

	int line;
	__u32 flags;

	__u8 dlsap_sel;
	__u8 slsap_sel;

	__u32 saddr;
	__u32 daddr;

	__u32 max_data_size;   /* Max data we can transmit in one packet */
	__u32 max_header_size; /* The amount of header space we must reserve */

	struct iriap_cb *iriap; /* Instance used for querying remote IAS */
	struct ias_object* obj;
	int skey;
	int ckey;

	struct termios	  normal_termios;
	struct termios	  callout_termios;

	wait_queue_head_t open_wait;
	wait_queue_head_t close_wait;
	struct timer_list watchdog_timer;
	struct tq_struct  tqueue;

        unsigned short    close_delay;
        unsigned short    closing_wait; /* time to wait before closing */

	long session;           /* Session of opening process */
	long pgrp;		/* pgrp of opening process */
	int  open_count;
	int  blocked_open;	/* # of blocked opens */
};

void ircomm_tty_start(struct tty_struct *tty);
void ircomm_tty_stop(struct tty_struct *tty);
void ircomm_tty_check_modem_status(struct ircomm_tty_cb *self);

extern void ircomm_tty_change_speed(struct ircomm_tty_cb *self);
extern int ircomm_tty_ioctl(struct tty_struct *tty, struct file *file, 
			    unsigned int cmd, unsigned long arg);
extern void ircomm_tty_set_termios(struct tty_struct *tty, 
				   struct termios *old_termios);
extern hashbin_t *ircomm_tty;

#endif







