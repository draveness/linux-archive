/* $Id: isdn_concap.c,v 1.8 2000/05/11 22:29:20 kai Exp $
 
 * Linux ISDN subsystem, protocol encapsulation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

/* Stuff to support the concap_proto by isdn4linux. isdn4linux - specific
 * stuff goes here. Stuff that depends only on the concap protocol goes to
 * another -- protocol specific -- source file.
 *
 */


#include <linux/isdn.h>
#include "isdn_x25iface.h"
#include "isdn_net.h"
#include <linux/concap.h>
#include "isdn_concap.h"


/* The following set of device service operations are for encapsulation
   protocols that require for reliable datalink semantics. That means:

   - before any data is to be submitted the connection must explicitly
     be set up.
   - after the successful set up of the connection is signalled the
     connection is considered to be reliably up.

   Auto-dialing ist not compatible with this requirements. Thus, auto-dialing 
   is completely bypassed.

   It might be possible to implement a (non standardized) datalink protocol
   that provides a reliable data link service while using some auto dialing
   mechanism. Such a protocol would need an auxiliary channel (i.e. user-user-
   signaling on the D-channel) while the B-channel is down.
   */


int isdn_concap_dl_data_req(struct concap_proto *concap, struct sk_buff *skb)
{
	struct net_device *ndev = concap -> net_dev;
	isdn_net_dev *nd = ((isdn_net_local *) ndev->priv)->netdev;
	isdn_net_local *lp = isdn_net_get_locked_lp(nd);

	IX25DEBUG( "isdn_concap_dl_data_req: %s \n", concap->net_dev->name);
	if (!lp) {
		IX25DEBUG( "isdn_concap_dl_data_req: %s : isdn_net_send_skb returned %d\n", concap -> net_dev -> name, 1);
		return 1;
	}
	lp->huptimer = 0;
	isdn_net_writebuf_skb(lp, skb);
	spin_unlock_bh(&lp->xmit_lock);
	IX25DEBUG( "isdn_concap_dl_data_req: %s : isdn_net_send_skb returned %d\n", concap -> net_dev -> name, 0);
	return 0;
}


int isdn_concap_dl_connect_req(struct concap_proto *concap)
{
	struct net_device *ndev = concap -> net_dev;
	isdn_net_local *lp = (isdn_net_local *) ndev->priv;
	int ret;
	IX25DEBUG( "isdn_concap_dl_connect_req: %s \n", ndev -> name);

	/* dial ... */
	ret = isdn_net_dial_req( lp );
	if ( ret ) IX25DEBUG("dialing failed\n");
	return ret;
}

int isdn_concap_dl_disconn_req(struct concap_proto *concap)
{
	IX25DEBUG( "isdn_concap_dl_disconn_req: %s \n", concap -> net_dev -> name);

	isdn_net_hangup( concap -> net_dev );
	return 0;
}

struct concap_device_ops isdn_concap_reliable_dl_dops = {
	&isdn_concap_dl_data_req,
	&isdn_concap_dl_connect_req,
	&isdn_concap_dl_disconn_req
};

struct concap_device_ops isdn_concap_demand_dial_dops = {
	NULL, /* set this first entry to something like &isdn_net_start_xmit,
		 but the entry part of the current isdn_net_start_xmit must be
		 separated first. */
	/* no connection control for demand dial semantics */
	NULL,
	NULL,
};

/* The following should better go into a dedicated source file such that
   this sourcefile does not need to include any protocol specific header
   files. For now:
   */
struct concap_proto * isdn_concap_new( int encap )
{
	switch ( encap ) {
	case ISDN_NET_ENCAP_X25IFACE:
		return isdn_x25iface_proto_new();
	}
	return NULL;
}
