/*********************************************************************
 *                
 * Filename:      qos.h
 * Version:       1.0
 * Description:   Quality of Service definitions
 * Status:        Experimental.
 * Author:        Dag Brattli <dagb@cs.uit.no>
 * Created at:    Fri Sep 19 23:21:09 1997
 * Modified at:   Thu Dec  2 13:51:54 1999
 * Modified by:   Dag Brattli <dagb@cs.uit.no>
 * 
 *     Copyright (c) 1999 Dag Brattli, All Rights Reserved.
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

#ifndef IRDA_QOS_H
#define IRDA_QOS_H

#include <linux/config.h>
#include <linux/skbuff.h>

#include <net/irda/parameters.h>

#define PI_BAUD_RATE     0x01
#define PI_MAX_TURN_TIME 0x82
#define PI_DATA_SIZE     0x83
#define PI_WINDOW_SIZE   0x84
#define PI_ADD_BOFS      0x85
#define PI_MIN_TURN_TIME 0x86
#define PI_LINK_DISC     0x08
#define PI_COMPRESSION   0x07 /* Just a random pick */


#define IR_115200_MAX 0x3f

/* Baud rates (first byte) */
#define IR_2400     0x01
#define IR_9600     0x02
#define IR_19200    0x04
#define IR_38400    0x08
#define IR_57600    0x10
#define IR_115200   0x20
#define IR_576000   0x40
#define IR_1152000  0x80

/* Baud rates (second byte) */
#define IR_4000000  0x01
#define IR_16000000 0x02

/* Quality of Service information */
typedef struct {
	__u32 value;
	__u16 bits; /* LSB is first byte, MSB is second byte */
} qos_value_t;

struct qos_info {
	magic_t magic;

	qos_value_t baud_rate;       /* IR_11520O | ... */
	qos_value_t max_turn_time;
	qos_value_t data_size;
	qos_value_t window_size;
	qos_value_t additional_bofs;
	qos_value_t min_turn_time;
	qos_value_t link_disc_time;
	
	qos_value_t power;
#ifdef CONFIG_IRDA_COMPRESSION
	/* An experimental non IrDA field */
	qos_value_t compression;
#endif
};

extern int sysctl_max_baud_rate;
extern int sysctl_max_inactive_time;

extern __u32 baud_rates[];
extern __u32 data_sizes[];
extern __u32 min_turn_times[];
extern __u32 add_bofs[];
extern __u32 compressions[];

void irda_init_max_qos_capabilies(struct qos_info *qos);
void irda_qos_compute_intersection(struct qos_info *, struct qos_info *);

__u32 irlap_max_line_capacity(__u32 speed, __u32 max_turn_time);
__u32 irlap_requested_line_capacity(struct qos_info *qos);
__u32 irlap_min_turn_time_in_bytes(__u32 speed, __u32 min_turn_time);

int msb_index(__u16 byte);
void irda_qos_bits_to_value(struct qos_info *qos);

#endif

