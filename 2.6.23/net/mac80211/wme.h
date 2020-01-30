/*
 * IEEE 802.11 driver (80211.o) - QoS datatypes
 * Copyright 2004, Instant802 Networks, Inc.
 * Copyright 2005, Devicescape Software, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _WME_H
#define _WME_H

#include <linux/netdevice.h>
#include "ieee80211_i.h"

#define QOS_CONTROL_LEN 2

#define QOS_CONTROL_ACK_POLICY_NORMAL 0
#define QOS_CONTROL_ACK_POLICY_NOACK 1

#define QOS_CONTROL_TID_MASK 0x0f
#define QOS_CONTROL_ACK_POLICY_SHIFT 5

#define QOS_CONTROL_TAG1D_MASK 0x07

ieee80211_txrx_result
ieee80211_rx_h_parse_qos(struct ieee80211_txrx_data *rx);

ieee80211_txrx_result
ieee80211_rx_h_remove_qos_control(struct ieee80211_txrx_data *rx);

#ifdef CONFIG_NET_SCHED
void ieee80211_install_qdisc(struct net_device *dev);
int ieee80211_qdisc_installed(struct net_device *dev);

int ieee80211_wme_register(void);
void ieee80211_wme_unregister(void);
#else
static inline void ieee80211_install_qdisc(struct net_device *dev)
{
}
static inline int ieee80211_qdisc_installed(struct net_device *dev)
{
	return 0;
}

static inline int ieee80211_wme_register(void)
{
	return 0;
}
static inline void ieee80211_wme_unregister(void)
{
}
#endif /* CONFIG_NET_SCHED */

#endif /* _WME_H */
