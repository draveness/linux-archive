/*
 *	AX.25 release 037
 *
 *	This code REQUIRES 2.1.15 or higher/ NET3.038
 *
 *	This module:
 *		This module is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 *	Most of this code is based on the SDL diagrams published in the 7th
 *	ARRL Computer Networking Conference papers. The diagrams have mistakes
 *	in them, but are mostly correct. Before you modify the code could you
 *	read the SDL diagrams as the code is not obvious and probably very
 *	easy to break;
 *
 *	History
 *	AX.25 036	Jonathan(G4KLX)	Cloned from ax25_out.c and ax25_subr.c.
 *			Joerg(DL1BKE)	Changed ax25_ds_enquiry_response(),
 *					fixed ax25_dama_on() and ax25_dama_off().
 *	AX.25 037	Jonathan(G4KLX)	New timer architecture.
 */

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <net/ax25.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/interrupt.h>

void ax25_ds_nr_error_recovery(ax25_cb *ax25)
{
	ax25_ds_establish_data_link(ax25);
}

/*
 *	dl1bke 960114: transmit I frames on DAMA poll
 */
void ax25_ds_enquiry_response(ax25_cb *ax25)
{
	ax25_cb *ax25o;

	/* Please note that neither DK4EG�s nor DG2FEF�s
	 * DAMA spec mention the following behaviour as seen
	 * with TheFirmware:
	 *
	 * 	DB0ACH->DL1BKE <RR C P R0> [DAMA]
	 *	DL1BKE->DB0ACH <I NR=0 NS=0>
	 *	DL1BKE-7->DB0PRA-6 DB0ACH <I C S3 R5>
	 *	DL1BKE->DB0ACH <RR R F R0>
	 *
	 * The Flexnet DAMA Master implementation apparently
	 * insists on the "proper" AX.25 behaviour:
	 *
	 * 	DB0ACH->DL1BKE <RR C P R0> [DAMA]
	 *	DL1BKE->DB0ACH <RR R F R0>
	 *	DL1BKE->DB0ACH <I NR=0 NS=0>
	 *	DL1BKE-7->DB0PRA-6 DB0ACH <I C S3 R5>
	 *
	 * Flexnet refuses to send us *any* I frame if we send
	 * a REJ in case AX25_COND_REJECT is set. It is superfluous in 
	 * this mode anyway (a RR or RNR invokes the retransmission).
	 * Is this a Flexnet bug?
	 */

	ax25_std_enquiry_response(ax25);

	if (!(ax25->condition & AX25_COND_PEER_RX_BUSY)) {
		ax25_requeue_frames(ax25);
		ax25_kick(ax25);
	}

	if (ax25->state == AX25_STATE_1 || ax25->state == AX25_STATE_2 || skb_peek(&ax25->ack_queue) != NULL)
		ax25_ds_t1_timeout(ax25);
	else
		ax25->n2count = 0;

	ax25_start_t3timer(ax25);
	ax25_ds_set_timer(ax25->ax25_dev);

	for (ax25o = ax25_list; ax25o != NULL; ax25o = ax25o->next) {
		if (ax25o == ax25)
			continue;

		if (ax25o->ax25_dev != ax25->ax25_dev)
			continue;

		if (ax25o->state == AX25_STATE_1 || ax25o->state == AX25_STATE_2) {
			ax25_ds_t1_timeout(ax25o);
			continue;
		}

		if (!(ax25o->condition & AX25_COND_PEER_RX_BUSY) && ax25o->state == AX25_STATE_3) {
			ax25_requeue_frames(ax25o);
			ax25_kick(ax25o);
		}

		if (ax25o->state == AX25_STATE_1 || ax25o->state == AX25_STATE_2 || skb_peek(&ax25o->ack_queue) != NULL)
			ax25_ds_t1_timeout(ax25o);

		/* do not start T3 for listening sockets (tnx DD8NE) */

		if (ax25o->state != AX25_STATE_0)
			ax25_start_t3timer(ax25o);
	}
}

void ax25_ds_establish_data_link(ax25_cb *ax25)
{
	ax25->condition &= AX25_COND_DAMA_MODE;
	ax25->n2count    = 0;
	ax25_calculate_t1(ax25);
	ax25_start_t1timer(ax25);
	ax25_stop_t2timer(ax25);
	ax25_start_t3timer(ax25);
}

/*
 *	:::FIXME:::
 *	This is a kludge. Not all drivers recognize kiss commands. 
 *	We need a driver level  request to switch duplex mode, that does 
 *	either SCC changing, PI config or KISS as required. Currently
 *	this request isn't reliable.
 */
static void ax25_kiss_cmd(ax25_dev *ax25_dev, unsigned char cmd, unsigned char param)
{
	struct sk_buff *skb;
	unsigned char *p;

	if (ax25_dev->dev == NULL)
		return;

	if ((skb = alloc_skb(2, GFP_ATOMIC)) == NULL)
		return;

	skb->nh.raw = skb->data;
	p = skb_put(skb, 2);

	*p++ = cmd;
	*p++ = param;

	skb->dev      = ax25_dev->dev;
	skb->protocol = htons(ETH_P_AX25);

	dev_queue_xmit(skb);
}

/*
 *	A nasty problem arises if we count the number of DAMA connections
 *	wrong, especially when connections on the device already existed
 *	and our network node (or the sysop) decides to turn on DAMA Master
 *	mode. We thus flag the 'real' slave connections with 
 *	ax25->dama_slave=1 and look on every disconnect if still slave
 *	connections exist.
 */
static int ax25_check_dama_slave(ax25_dev *ax25_dev)
{
	ax25_cb *ax25;

	for (ax25 = ax25_list; ax25 != NULL ; ax25 = ax25->next)
		if (ax25->ax25_dev == ax25_dev && (ax25->condition & AX25_COND_DAMA_MODE) && ax25->state > AX25_STATE_1)
			return 1;

	return 0;
}

void ax25_dev_dama_on(ax25_dev *ax25_dev)
{
	if (ax25_dev == NULL)
		return;

	if (ax25_dev->dama.slave == 0)
		ax25_kiss_cmd(ax25_dev, 5, 1);

	ax25_dev->dama.slave = 1;
	ax25_ds_set_timer(ax25_dev);
}

void ax25_dev_dama_off(ax25_dev *ax25_dev)
{
	if (ax25_dev == NULL)
		return;

	if (ax25_dev->dama.slave && !ax25_check_dama_slave(ax25_dev)) {
		ax25_kiss_cmd(ax25_dev, 5, 0);
		ax25_dev->dama.slave = 0;
		ax25_ds_del_timer(ax25_dev);
	}
}

void ax25_dama_on(ax25_cb *ax25)
{
	ax25_dev_dama_on(ax25->ax25_dev);
	ax25->condition |= AX25_COND_DAMA_MODE;
}

void ax25_dama_off(ax25_cb *ax25)
{
	ax25->condition &= ~AX25_COND_DAMA_MODE;
	ax25_dev_dama_off(ax25->ax25_dev);
}

