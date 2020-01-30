/*
 *  
 *  Copyright (C) 2002 Intersil Americas Inc.
 *            (C) 2003,2004 Aurelien Alleaume <slts@free.fr>
 *            (C) 2003 Herbert Valerio Riedel <hvr@gnu.org>
 *            (C) 2003 Luis R. Rodriguez <mcgrof@ruslug.rutgers.edu>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/if_arp.h>
#include <linux/pci.h>

#include <asm/uaccess.h>

#include "prismcompat.h"
#include "isl_ioctl.h"
#include "islpci_mgt.h"
#include "isl_oid.h"		/* additional types and defs for isl38xx fw */
#include "oid_mgt.h"

#include <net/iw_handler.h>	/* New driver API */

static int init_mode = CARD_DEFAULT_IW_MODE;
static int init_channel = CARD_DEFAULT_CHANNEL;
static int init_wep = CARD_DEFAULT_WEP;
static int init_filter = CARD_DEFAULT_FILTER;
static int init_authen = CARD_DEFAULT_AUTHEN;
static int init_dot1x = CARD_DEFAULT_DOT1X;
static int init_conformance = CARD_DEFAULT_CONFORMANCE;
static int init_mlme = CARD_DEFAULT_MLME_MODE;

module_param(init_mode, int, 0);
MODULE_PARM_DESC(init_mode,
		 "Set card mode:\n0: Auto\n1: Ad-Hoc\n2: Managed Client (Default)\n3: Master / Access Point\n4: Repeater (Not supported yet)\n5: Secondary (Not supported yet)\n6: Monitor");

module_param(init_channel, int, 0);
MODULE_PARM_DESC(init_channel,
		 "Check `iwpriv ethx channel` for available channels");

module_param(init_wep, int, 0);
module_param(init_filter, int, 0);

module_param(init_authen, int, 0);
MODULE_PARM_DESC(init_authen,
		 "Authentication method. Can be of seven types:\n0 0x0000: None\n1 0x0001: DOT11_AUTH_OS (Default)\n2 0x0002: DOT11_AUTH_SK\n3 0x0003: DOT11_AUTH_BOTH");

module_param(init_dot1x, int, 0);
MODULE_PARM_DESC(init_dot1x,
		 "\n0: None/not set	(Default)\n1: DOT11_DOT1X_AUTHENABLED\n2: DOT11_DOT1X_KEYTXENABLED");

module_param(init_mlme, int, 0);
MODULE_PARM_DESC(init_mlme,
		 "Sets the MAC layer management entity (MLME) mode of operation,\n0: DOT11_MLME_AUTO (Default)\n1: DOT11_MLME_INTERMEDIATE\n2: DOT11_MLME_EXTENDED");

/**
 * prism54_mib_mode_helper - MIB change mode helper function
 * @mib: the &struct islpci_mib object to modify
 * @iw_mode: new mode (%IW_MODE_*)
 * 
 *  This is a helper function, hence it does not lock. Make sure
 *  caller deals with locking *if* necessary. This function sets the 
 *  mode-dependent mib values and does the mapping of the Linux 
 *  Wireless API modes to Device firmware modes. It also checks for 
 *  correct valid Linux wireless modes. 
 */
int
prism54_mib_mode_helper(islpci_private *priv, u32 iw_mode)
{
	u32 config = INL_CONFIG_MANUALRUN;
	u32 mode, bsstype;

	/* For now, just catch early the Repeater and Secondary modes here */
	if (iw_mode == IW_MODE_REPEAT || iw_mode == IW_MODE_SECOND) {
		printk(KERN_DEBUG
		       "%s(): Sorry, Repeater mode and Secondary mode "
		       "are not yet supported by this driver.\n", __FUNCTION__);
		return -EINVAL;
	}

	priv->iw_mode = iw_mode;

	switch (iw_mode) {
	case IW_MODE_AUTO:
		mode = INL_MODE_CLIENT;
		bsstype = DOT11_BSSTYPE_ANY;
		break;
	case IW_MODE_ADHOC:
		mode = INL_MODE_CLIENT;
		bsstype = DOT11_BSSTYPE_IBSS;
		break;
	case IW_MODE_INFRA:
		mode = INL_MODE_CLIENT;
		bsstype = DOT11_BSSTYPE_INFRA;
		break;
	case IW_MODE_MASTER:
		mode = INL_MODE_AP;
		bsstype = DOT11_BSSTYPE_INFRA;
		break;
	case IW_MODE_MONITOR:
		mode = INL_MODE_PROMISCUOUS;
		bsstype = DOT11_BSSTYPE_ANY;
		config |= INL_CONFIG_RXANNEX;
		break;
	default:
		return -EINVAL;
	}

	if (init_wds)
		config |= INL_CONFIG_WDS;
	mgt_set(priv, DOT11_OID_BSSTYPE, &bsstype);
	mgt_set(priv, OID_INL_CONFIG, &config);
	mgt_set(priv, OID_INL_MODE, &mode);

	return 0;
}

/**
 * prism54_mib_init - fill MIB cache with defaults
 *
 *  this function initializes the struct given as @mib with defaults,
 *  of which many are retrieved from the global module parameter
 *  variables.  
 */

void
prism54_mib_init(islpci_private *priv)
{
	u32 t;
	struct obj_buffer psm_buffer = {
		.size = PSM_BUFFER_SIZE,
		.addr = priv->device_psm_buffer
	};

	mgt_set(priv, DOT11_OID_CHANNEL, &init_channel);
	mgt_set(priv, DOT11_OID_AUTHENABLE, &init_authen);
	mgt_set(priv, DOT11_OID_PRIVACYINVOKED, &init_wep);

	mgt_set(priv, DOT11_OID_PSMBUFFER, &psm_buffer);
	mgt_set(priv, DOT11_OID_EXUNENCRYPTED, &init_filter);
	mgt_set(priv, DOT11_OID_DOT1XENABLE, &init_dot1x);
	mgt_set(priv, DOT11_OID_MLMEAUTOLEVEL, &init_mlme);
	mgt_set(priv, OID_INL_DOT11D_CONFORMANCE, &init_conformance);

	t = 127;
	mgt_set(priv, OID_INL_OUTPUTPOWER, &t);

	/* Important: we are setting a default wireless mode and we are 
	 * forcing a valid one, so prism54_mib_mode_helper should just set
	 * mib values depending on what the wireless mode given is. No need
	 * for it save old values */
	if (init_mode > IW_MODE_MONITOR || init_mode < IW_MODE_AUTO) {
		printk(KERN_DEBUG "%s(): You passed a non-valid init_mode. "
		       "Using default mode\n", __FUNCTION__);
		init_mode = CARD_DEFAULT_IW_MODE;
	}
	/* This sets all of the mode-dependent values */
	prism54_mib_mode_helper(priv, init_mode);
}

/* this will be executed outside of atomic context thanks to
 * schedule_work(), thus we can as well use sleeping semaphore
 * locking */
void
prism54_update_stats(islpci_private *priv)
{
	char *data;
	int j;
	struct obj_bss bss, *bss2;
	union oid_res_t r;

	if (down_interruptible(&priv->stats_sem))
		return;

/* Noise floor.
 * I'm not sure if the unit is dBm.
 * Note : If we are not connected, this value seems to be irrelevant. */

	mgt_get_request(priv, DOT11_OID_NOISEFLOOR, 0, NULL, &r);
	priv->local_iwstatistics.qual.noise = r.u;

/* Get the rssi of the link. To do this we need to retrieve a bss. */

	/* First get the MAC address of the AP we are associated with. */
	mgt_get_request(priv, DOT11_OID_BSSID, 0, NULL, &r);
	data = r.ptr;

	/* copy this MAC to the bss */
	memcpy(bss.address, data, 6);
	kfree(data);

	/* now ask for the corresponding bss */
	j = mgt_get_request(priv, DOT11_OID_BSSFIND, 0, (void *) &bss, &r);
	bss2 = r.ptr;
	/* report the rssi and use it to calculate
	 *  link quality through a signal-noise
	 *  ratio */
	priv->local_iwstatistics.qual.level = bss2->rssi;
	priv->local_iwstatistics.qual.qual =
	    bss2->rssi - priv->iwstatistics.qual.noise;

	kfree(bss2);

	/* report that the stats are new */
	priv->local_iwstatistics.qual.updated = 0x7;

/* Rx : unable to decrypt the MPDU */
	mgt_get_request(priv, DOT11_OID_PRIVRXFAILED, 0, NULL, &r);
	priv->local_iwstatistics.discard.code = r.u;

/* Tx : Max MAC retries num reached */
	mgt_get_request(priv, DOT11_OID_MPDUTXFAILED, 0, NULL, &r);
	priv->local_iwstatistics.discard.retries = r.u;

	up(&priv->stats_sem);

	return;
}

struct iw_statistics *
prism54_get_wireless_stats(struct net_device *ndev)
{
	islpci_private *priv = netdev_priv(ndev);

	/* If the stats are being updated return old data */
	if (down_trylock(&priv->stats_sem) == 0) {
		memcpy(&priv->iwstatistics, &priv->local_iwstatistics,
		       sizeof (struct iw_statistics));
		/* They won't be marked updated for the next time */
		priv->local_iwstatistics.qual.updated = 0;
		up(&priv->stats_sem);
	} else
		priv->iwstatistics.qual.updated = 0;

	/* Update our wireless stats, but do not schedule to often 
	 * (max 1 HZ) */
	if ((priv->stats_timestamp == 0) ||
	    time_after(jiffies, priv->stats_timestamp + 1 * HZ)) {
		schedule_work(&priv->stats_work);
		priv->stats_timestamp = jiffies;
	}

	return &priv->iwstatistics;
}

static int
prism54_commit(struct net_device *ndev, struct iw_request_info *info,
	       char *cwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);

	/* simply re-set the last set SSID, this should commit most stuff */

	/* Commit in Monitor mode is not necessary, also setting essid
	 * in Monitor mode does not make sense and isn't allowed for this
	 * device's firmware */
	if (priv->iw_mode != IW_MODE_MONITOR)
		return mgt_set_request(priv, DOT11_OID_SSID, 0, NULL);
	return 0;
}

static int
prism54_get_name(struct net_device *ndev, struct iw_request_info *info,
		 char *cwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	char *capabilities;
	union oid_res_t r;
	int rvalue;

	if (islpci_get_state(priv) < PRV_STATE_INIT) {
		strncpy(cwrq, "NOT READY!", IFNAMSIZ);
		return 0;
	}
	rvalue = mgt_get_request(priv, OID_INL_PHYCAPABILITIES, 0, NULL, &r);

	switch (r.u) {
	case INL_PHYCAP_5000MHZ:
		capabilities = "IEEE 802.11a/b/g";
		break;
	case INL_PHYCAP_FAA:
		capabilities = "IEEE 802.11b/g - FAA Support";
		break;
	case INL_PHYCAP_2400MHZ:
	default:
		capabilities = "IEEE 802.11b/g";	/* Default */
		break;
	}
	strncpy(cwrq, capabilities, IFNAMSIZ);
	return rvalue;
}

static int
prism54_set_freq(struct net_device *ndev, struct iw_request_info *info,
		 struct iw_freq *fwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	int rvalue;
	u32 c;

	if (fwrq->m < 1000)
		/* we have a channel number */
		c = fwrq->m;
	else
		c = (fwrq->e == 1) ? channel_of_freq(fwrq->m / 100000) : 0;

	rvalue = c ? mgt_set_request(priv, DOT11_OID_CHANNEL, 0, &c) : -EINVAL;

	/* Call commit handler */
	return (rvalue ? rvalue : -EINPROGRESS);
}

static int
prism54_get_freq(struct net_device *ndev, struct iw_request_info *info,
		 struct iw_freq *fwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	union oid_res_t r;
	int rvalue;

	rvalue = mgt_get_request(priv, DOT11_OID_CHANNEL, 0, NULL, &r);

	fwrq->m = r.u;
	fwrq->e = 0;

	return rvalue;
}

static int
prism54_set_mode(struct net_device *ndev, struct iw_request_info *info,
		 __u32 * uwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	u32 mlmeautolevel = CARD_DEFAULT_MLME_MODE;

	/* Let's see if the user passed a valid Linux Wireless mode */
	if (*uwrq > IW_MODE_MONITOR || *uwrq < IW_MODE_AUTO) {
		printk(KERN_DEBUG
		       "%s: %s() You passed a non-valid init_mode.\n",
		       priv->ndev->name, __FUNCTION__);
		return -EINVAL;
	}

	down_write(&priv->mib_sem);

	if (prism54_mib_mode_helper(priv, *uwrq)) {
		up_write(&priv->mib_sem);
		return -EOPNOTSUPP;
	}

	/* the ACL code needs an intermediate mlmeautolevel. The wpa stuff an
	 * extended one.
	 */
	if ((*uwrq == IW_MODE_MASTER) && (priv->acl.policy != MAC_POLICY_OPEN))
		mlmeautolevel = DOT11_MLME_INTERMEDIATE;
	if (priv->wpa)
		mlmeautolevel = DOT11_MLME_EXTENDED;

	mgt_set(priv, DOT11_OID_MLMEAUTOLEVEL, &mlmeautolevel);

	mgt_commit(priv);
	priv->ndev->type = (priv->iw_mode == IW_MODE_MONITOR)
	    ? priv->monitor_type : ARPHRD_ETHER;
	up_write(&priv->mib_sem);

	return 0;
}

/* Use mib cache */
static int
prism54_get_mode(struct net_device *ndev, struct iw_request_info *info,
		 __u32 * uwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);

	BUG_ON((priv->iw_mode < IW_MODE_AUTO) || (priv->iw_mode >
						  IW_MODE_MONITOR));
	*uwrq = priv->iw_mode;

	return 0;
}

/* we use DOT11_OID_EDTHRESHOLD. From what I guess the card will not try to
 * emit data if (sensitivity > rssi - noise) (in dBm).
 * prism54_set_sens does not seem to work.
 */

static int
prism54_set_sens(struct net_device *ndev, struct iw_request_info *info,
		 struct iw_param *vwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	u32 sens;

	/* by default  the card sets this to 20. */
	sens = vwrq->disabled ? 20 : vwrq->value;

	return mgt_set_request(priv, DOT11_OID_EDTHRESHOLD, 0, &sens);
}

static int
prism54_get_sens(struct net_device *ndev, struct iw_request_info *info,
		 struct iw_param *vwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	union oid_res_t r;
	int rvalue;

	rvalue = mgt_get_request(priv, DOT11_OID_EDTHRESHOLD, 0, NULL, &r);

	vwrq->value = r.u;
	vwrq->disabled = (vwrq->value == 0);
	vwrq->fixed = 1;

	return rvalue;
}

static int
prism54_get_range(struct net_device *ndev, struct iw_request_info *info,
		  struct iw_point *dwrq, char *extra)
{
	struct iw_range *range = (struct iw_range *) extra;
	islpci_private *priv = netdev_priv(ndev);
	u8 *data;
	int i, m, rvalue;
	struct obj_frequencies *freq;
	union oid_res_t r;

	memset(range, 0, sizeof (struct iw_range));
	dwrq->length = sizeof (struct iw_range);

	/* set the wireless extension version number */
	range->we_version_source = SUPPORTED_WIRELESS_EXT;
	range->we_version_compiled = WIRELESS_EXT;

	/* Now the encoding capabilities */
	range->num_encoding_sizes = 3;
	/* 64(40) bits WEP */
	range->encoding_size[0] = 5;
	/* 128(104) bits WEP */
	range->encoding_size[1] = 13;
	/* 256 bits for WPA-PSK */
	range->encoding_size[2] = 32;
	/* 4 keys are allowed */
	range->max_encoding_tokens = 4;

	/* we don't know the quality range... */
	range->max_qual.level = 0;
	range->max_qual.noise = 0;
	range->max_qual.qual = 0;
	/* these value describe an average quality. Needs more tweaking... */
	range->avg_qual.level = -80;	/* -80 dBm */
	range->avg_qual.noise = 0;	/* don't know what to put here */
	range->avg_qual.qual = 0;

	range->sensitivity = 200;

	/* retry limit capabilities */
	range->retry_capa = IW_RETRY_LIMIT | IW_RETRY_LIFETIME;
	range->retry_flags = IW_RETRY_LIMIT;
	range->r_time_flags = IW_RETRY_LIFETIME;

	/* I don't know the range. Put stupid things here */
	range->min_retry = 1;
	range->max_retry = 65535;
	range->min_r_time = 1024;
	range->max_r_time = 65535 * 1024;

	/* txpower is supported in dBm's */
	range->txpower_capa = IW_TXPOW_DBM;

	if (islpci_get_state(priv) < PRV_STATE_INIT)
		return 0;

	/* Request the device for the supported frequencies
	 * not really relevant since some devices will report the 5 GHz band
	 * frequencies even if they don't support them.
	 */
	rvalue =
	    mgt_get_request(priv, DOT11_OID_SUPPORTEDFREQUENCIES, 0, NULL, &r);
	freq = r.ptr;

	range->num_channels = freq->nr;
	range->num_frequency = freq->nr;

	m = min(IW_MAX_FREQUENCIES, (int) freq->nr);
	for (i = 0; i < m; i++) {
		range->freq[i].m = freq->mhz[i];
		range->freq[i].e = 6;
		range->freq[i].i = channel_of_freq(freq->mhz[i]);
	}
	kfree(freq);

	rvalue |= mgt_get_request(priv, DOT11_OID_SUPPORTEDRATES, 0, NULL, &r);
	data = r.ptr;

	/* We got an array of char. It is NULL terminated. */
	i = 0;
	while ((i < IW_MAX_BITRATES) && (*data != 0)) {
		/*       the result must be in bps. The card gives us 500Kbps */
		range->bitrate[i] = *data * 500000;
		i++;
		data++;
	}
	range->num_bitrates = i;
	kfree(r.ptr);

	return rvalue;
}

/* Set AP address*/

static int
prism54_set_wap(struct net_device *ndev, struct iw_request_info *info,
		struct sockaddr *awrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	char bssid[6];
	int rvalue;

	if (awrq->sa_family != ARPHRD_ETHER)
		return -EINVAL;

	/* prepare the structure for the set object */
	memcpy(&bssid[0], awrq->sa_data, 6);

	/* set the bssid -- does this make sense when in AP mode? */
	rvalue = mgt_set_request(priv, DOT11_OID_BSSID, 0, &bssid);

	return (rvalue ? rvalue : -EINPROGRESS);	/* Call commit handler */
}

/* get AP address*/

static int
prism54_get_wap(struct net_device *ndev, struct iw_request_info *info,
		struct sockaddr *awrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	union oid_res_t r;
	int rvalue;

	rvalue = mgt_get_request(priv, DOT11_OID_BSSID, 0, NULL, &r);
	memcpy(awrq->sa_data, r.ptr, 6);
	awrq->sa_family = ARPHRD_ETHER;
	kfree(r.ptr);

	return rvalue;
}

static int
prism54_set_scan(struct net_device *dev, struct iw_request_info *info,
		 struct iw_param *vwrq, char *extra)
{
	/* hehe the device does this automagicaly */
	return 0;
}

/* a little helper that will translate our data into a card independent
 * format that the Wireless Tools will understand. This was inspired by
 * the "Aironet driver for 4500 and 4800 series cards" (GPL)
 */

static char *
prism54_translate_bss(struct net_device *ndev, char *current_ev,
		      char *end_buf, struct obj_bss *bss, char noise)
{
	struct iw_event iwe;	/* Temporary buffer */
	short cap;
	islpci_private *priv = netdev_priv(ndev);

	/* The first entry must be the MAC address */
	memcpy(iwe.u.ap_addr.sa_data, bss->address, 6);
	iwe.u.ap_addr.sa_family = ARPHRD_ETHER;
	iwe.cmd = SIOCGIWAP;
	current_ev =
	    iwe_stream_add_event(current_ev, end_buf, &iwe, IW_EV_ADDR_LEN);

	/* The following entries will be displayed in the same order we give them */

	/* The ESSID. */
	iwe.u.data.length = bss->ssid.length;
	iwe.u.data.flags = 1;
	iwe.cmd = SIOCGIWESSID;
	current_ev = iwe_stream_add_point(current_ev, end_buf,
					  &iwe, bss->ssid.octets);

	/* Capabilities */
#define CAP_ESS 0x01
#define CAP_IBSS 0x02
#define CAP_CRYPT 0x10

	/* Mode */
	cap = bss->capinfo;
	iwe.u.mode = 0;
	if (cap & CAP_ESS)
		iwe.u.mode = IW_MODE_MASTER;
	else if (cap & CAP_IBSS)
		iwe.u.mode = IW_MODE_ADHOC;
	iwe.cmd = SIOCGIWMODE;
	if (iwe.u.mode)
		current_ev =
		    iwe_stream_add_event(current_ev, end_buf, &iwe,
					 IW_EV_UINT_LEN);

	/* Encryption capability */
	if (cap & CAP_CRYPT)
		iwe.u.data.flags = IW_ENCODE_ENABLED | IW_ENCODE_NOKEY;
	else
		iwe.u.data.flags = IW_ENCODE_DISABLED;
	iwe.u.data.length = 0;
	iwe.cmd = SIOCGIWENCODE;
	current_ev = iwe_stream_add_point(current_ev, end_buf, &iwe, NULL);

	/* Add frequency. (short) bss->channel is the frequency in MHz */
	iwe.u.freq.m = channel_of_freq(bss->channel);
	iwe.u.freq.e = 0;
	iwe.cmd = SIOCGIWFREQ;
	current_ev =
	    iwe_stream_add_event(current_ev, end_buf, &iwe, IW_EV_FREQ_LEN);

	/* Add quality statistics */
	iwe.u.qual.level = bss->rssi;
	iwe.u.qual.noise = noise;
	/* do a simple SNR for quality */
	iwe.u.qual.qual = bss->rssi - noise;
	iwe.cmd = IWEVQUAL;
	current_ev =
	    iwe_stream_add_event(current_ev, end_buf, &iwe, IW_EV_QUAL_LEN);

	if (priv->wpa) {
		u8 wpa_ie[MAX_WPA_IE_LEN];
		char *buf, *p;
		size_t wpa_ie_len;
		int i;

		wpa_ie_len = prism54_wpa_ie_get(priv, bss->address, wpa_ie);
		if (wpa_ie_len > 0 &&
		    (buf = kmalloc(wpa_ie_len * 2 + 10, GFP_ATOMIC))) {
			p = buf;
			p += sprintf(p, "wpa_ie=");
			for (i = 0; i < wpa_ie_len; i++) {
				p += sprintf(p, "%02x", wpa_ie[i]);
			}
			memset(&iwe, 0, sizeof (iwe));
			iwe.cmd = IWEVCUSTOM;
			iwe.u.data.length = strlen(buf);
			current_ev = iwe_stream_add_point(current_ev, end_buf,
							  &iwe, buf);
			kfree(buf);
		}
	}
	return current_ev;
}

int
prism54_get_scan(struct net_device *ndev, struct iw_request_info *info,
		 struct iw_point *dwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	int i, rvalue;
	struct obj_bsslist *bsslist;
	u32 noise = 0;
	char *current_ev = extra;
	union oid_res_t r;

	if (islpci_get_state(priv) < PRV_STATE_INIT) {
		/* device is not ready, fail gently */
		dwrq->length = 0;
		return 0;
	}

	/* first get the noise value. We will use it to report the link quality */
	rvalue = mgt_get_request(priv, DOT11_OID_NOISEFLOOR, 0, NULL, &r);
	noise = r.u;

	/* Ask the device for a list of known bss. We can report at most
	 * IW_MAX_AP=64 to the range struct. But the device won't repport anything
	 * if you change the value of IWMAX_BSS=24.
	 */
	rvalue |= mgt_get_request(priv, DOT11_OID_BSSLIST, 0, NULL, &r);
	bsslist = r.ptr;

	/* ok now, scan the list and translate its info */
	for (i = 0; i < min(IW_MAX_AP, (int) bsslist->nr); i++)
		current_ev = prism54_translate_bss(ndev, current_ev,
						   extra + IW_SCAN_MAX_DATA,
						   &(bsslist->bsslist[i]),
						   noise);
	kfree(bsslist);
	dwrq->length = (current_ev - extra);
	dwrq->flags = 0;	/* todo */

	return rvalue;
}

static int
prism54_set_essid(struct net_device *ndev, struct iw_request_info *info,
		  struct iw_point *dwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	struct obj_ssid essid;

	memset(essid.octets, 0, 33);

	/* Check if we were asked for `any' */
	if (dwrq->flags && dwrq->length) {
		if (dwrq->length > min(33, IW_ESSID_MAX_SIZE + 1))
			return -E2BIG;
		essid.length = dwrq->length - 1;
		memcpy(essid.octets, extra, dwrq->length);
	} else
		essid.length = 0;

	if (priv->iw_mode != IW_MODE_MONITOR)
		return mgt_set_request(priv, DOT11_OID_SSID, 0, &essid);

	/* If in monitor mode, just save to mib */
	mgt_set(priv, DOT11_OID_SSID, &essid);
	return 0;

}

static int
prism54_get_essid(struct net_device *ndev, struct iw_request_info *info,
		  struct iw_point *dwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	struct obj_ssid *essid;
	union oid_res_t r;
	int rvalue;

	rvalue = mgt_get_request(priv, DOT11_OID_SSID, 0, NULL, &r);
	essid = r.ptr;

	if (essid->length) {
		dwrq->flags = 1;	/* set ESSID to ON for Wireless Extensions */
		/* if it is to big, trunk it */
		dwrq->length = min(IW_ESSID_MAX_SIZE, essid->length + 1);
	} else {
		dwrq->flags = 0;
		dwrq->length = 0;
	}
	essid->octets[essid->length] = '\0';
	memcpy(extra, essid->octets, dwrq->length);
	kfree(essid);

	return rvalue;
}

/* Provides no functionality, just completes the ioctl. In essence this is a 
 * just a cosmetic ioctl.
 */
static int
prism54_set_nick(struct net_device *ndev, struct iw_request_info *info,
		 struct iw_point *dwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);

	if (dwrq->length > IW_ESSID_MAX_SIZE)
		return -E2BIG;

	down_write(&priv->mib_sem);
	memset(priv->nickname, 0, sizeof (priv->nickname));
	memcpy(priv->nickname, extra, dwrq->length);
	up_write(&priv->mib_sem);

	return 0;
}

static int
prism54_get_nick(struct net_device *ndev, struct iw_request_info *info,
		 struct iw_point *dwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);

	dwrq->length = 0;

	down_read(&priv->mib_sem);
	dwrq->length = strlen(priv->nickname) + 1;
	memcpy(extra, priv->nickname, dwrq->length);
	up_read(&priv->mib_sem);

	return 0;
}

/* Set the allowed Bitrates */

static int
prism54_set_rate(struct net_device *ndev,
		 struct iw_request_info *info,
		 struct iw_param *vwrq, char *extra)
{

	islpci_private *priv = netdev_priv(ndev);
	u32 rate, profile;
	char *data;
	int ret, i;
	union oid_res_t r;

	if (vwrq->value == -1) {
		/* auto mode. No limit. */
		profile = 1;
		return mgt_set_request(priv, DOT11_OID_PROFILES, 0, &profile);
	}

	ret = mgt_get_request(priv, DOT11_OID_SUPPORTEDRATES, 0, NULL, &r);
	if (ret) {
		kfree(r.ptr);
		return ret;
	}

	rate = (u32) (vwrq->value / 500000);
	data = r.ptr;
	i = 0;

	while (data[i]) {
		if (rate && (data[i] == rate)) {
			break;
		}
		if (vwrq->value == i) {
			break;
		}
		data[i] |= 0x80;
		i++;
	}

	if (!data[i]) {
		kfree(r.ptr);
		return -EINVAL;
	}

	data[i] |= 0x80;
	data[i + 1] = 0;

	/* Now, check if we want a fixed or auto value */
	if (vwrq->fixed) {
		data[0] = data[i];
		data[1] = 0;
	}

/*
	i = 0;
	printk("prism54 rate: ");
	while(data[i]) {
		printk("%u ", data[i]);
		i++;
	}
	printk("0\n");
*/
	profile = -1;
	ret = mgt_set_request(priv, DOT11_OID_PROFILES, 0, &profile);
	ret |= mgt_set_request(priv, DOT11_OID_EXTENDEDRATES, 0, data);
	ret |= mgt_set_request(priv, DOT11_OID_RATES, 0, data);

	kfree(r.ptr);

	return ret;
}

/* Get the current bit rate */
static int
prism54_get_rate(struct net_device *ndev,
		 struct iw_request_info *info,
		 struct iw_param *vwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	int rvalue;
	char *data;
	union oid_res_t r;

	/* Get the current bit rate */
	if ((rvalue = mgt_get_request(priv, GEN_OID_LINKSTATE, 0, NULL, &r)))
		return rvalue;
	vwrq->value = r.u * 500000;

	/* request the device for the enabled rates */
	rvalue = mgt_get_request(priv, DOT11_OID_RATES, 0, NULL, &r);
	if (rvalue) {
		kfree(r.ptr);
		return rvalue;
	}
	data = r.ptr;
	vwrq->fixed = (data[0] != 0) && (data[1] == 0);
	kfree(r.ptr);

	return 0;
}

static int
prism54_set_rts(struct net_device *ndev, struct iw_request_info *info,
		struct iw_param *vwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);

	return mgt_set_request(priv, DOT11_OID_RTSTHRESH, 0, &vwrq->value);
}

static int
prism54_get_rts(struct net_device *ndev, struct iw_request_info *info,
		struct iw_param *vwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	union oid_res_t r;
	int rvalue;

	/* get the rts threshold */
	rvalue = mgt_get_request(priv, DOT11_OID_RTSTHRESH, 0, NULL, &r);
	vwrq->value = r.u;

	return rvalue;
}

static int
prism54_set_frag(struct net_device *ndev, struct iw_request_info *info,
		 struct iw_param *vwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);

	return mgt_set_request(priv, DOT11_OID_FRAGTHRESH, 0, &vwrq->value);
}

static int
prism54_get_frag(struct net_device *ndev, struct iw_request_info *info,
		 struct iw_param *vwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	union oid_res_t r;
	int rvalue;

	rvalue = mgt_get_request(priv, DOT11_OID_FRAGTHRESH, 0, NULL, &r);
	vwrq->value = r.u;

	return rvalue;
}

/* Here we have (min,max) = max retries for (small frames, big frames). Where
 * big frame <=>  bigger than the rts threshold
 * small frame <=>  smaller than the rts threshold
 * This is not really the behavior expected by the wireless tool but it seems
 * to be a common behavior in other drivers.
 */

static int
prism54_set_retry(struct net_device *ndev, struct iw_request_info *info,
		  struct iw_param *vwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	u32 slimit = 0, llimit = 0;	/* short and long limit */
	u32 lifetime = 0;
	int rvalue = 0;

	if (vwrq->disabled)
		/* we cannot disable this feature */
		return -EINVAL;

	if (vwrq->flags & IW_RETRY_LIMIT) {
		if (vwrq->flags & IW_RETRY_MIN)
			slimit = vwrq->value;
		else if (vwrq->flags & IW_RETRY_MAX)
			llimit = vwrq->value;
		else {
			/* we are asked to set both */
			slimit = vwrq->value;
			llimit = vwrq->value;
		}
	}
	if (vwrq->flags & IW_RETRY_LIFETIME)
		/* Wireless tools use us unit while the device uses 1024 us unit */
		lifetime = vwrq->value / 1024;

	/* now set what is requested */
	if (slimit)
		rvalue =
		    mgt_set_request(priv, DOT11_OID_SHORTRETRIES, 0, &slimit);
	if (llimit)
		rvalue |=
		    mgt_set_request(priv, DOT11_OID_LONGRETRIES, 0, &llimit);
	if (lifetime)
		rvalue |=
		    mgt_set_request(priv, DOT11_OID_MAXTXLIFETIME, 0,
				    &lifetime);
	return rvalue;
}

static int
prism54_get_retry(struct net_device *ndev, struct iw_request_info *info,
		  struct iw_param *vwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	union oid_res_t r;
	int rvalue = 0;
	vwrq->disabled = 0;	/* It cannot be disabled */

	if ((vwrq->flags & IW_RETRY_TYPE) == IW_RETRY_LIFETIME) {
		/* we are asked for the life time */
		rvalue =
		    mgt_get_request(priv, DOT11_OID_MAXTXLIFETIME, 0, NULL, &r);
		vwrq->value = r.u * 1024;
		vwrq->flags = IW_RETRY_LIFETIME;
	} else if ((vwrq->flags & IW_RETRY_MAX)) {
		/* we are asked for the long retry limit */
		rvalue |=
		    mgt_get_request(priv, DOT11_OID_LONGRETRIES, 0, NULL, &r);
		vwrq->value = r.u;
		vwrq->flags = IW_RETRY_LIMIT | IW_RETRY_MAX;
	} else {
		/* default. get the  short retry limit */
		rvalue |=
		    mgt_get_request(priv, DOT11_OID_SHORTRETRIES, 0, NULL, &r);
		vwrq->value = r.u;
		vwrq->flags = IW_RETRY_LIMIT | IW_RETRY_MIN;
	}

	return rvalue;
}

static int
prism54_set_encode(struct net_device *ndev, struct iw_request_info *info,
		   struct iw_point *dwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	int rvalue = 0, force = 0;
	int authen = DOT11_AUTH_OS, invoke = 0, exunencrypt = 0;
	union oid_res_t r;

	/* with the new API, it's impossible to get a NULL pointer.
	 * New version of iwconfig set the IW_ENCODE_NOKEY flag
	 * when no key is given, but older versions don't. */

	if (dwrq->length > 0) {
		/* we have a key to set */
		int index = (dwrq->flags & IW_ENCODE_INDEX) - 1;
		int current_index;
		struct obj_key key = { DOT11_PRIV_WEP, 0, "" };

		/* get the current key index */
		rvalue = mgt_get_request(priv, DOT11_OID_DEFKEYID, 0, NULL, &r);
		current_index = r.u;
		/* Verify that the key is not marked as invalid */
		if (!(dwrq->flags & IW_ENCODE_NOKEY)) {
			key.length = dwrq->length > sizeof (key.key) ?
			    sizeof (key.key) : dwrq->length;
			memcpy(key.key, extra, key.length);
			if (key.length == 32)
				/* we want WPA-PSK */
				key.type = DOT11_PRIV_TKIP;
			if ((index < 0) || (index > 3))
				/* no index provided use the current one */
				index = current_index;

			/* now send the key to the card  */
			rvalue |=
			    mgt_set_request(priv, DOT11_OID_DEFKEYX, index,
					    &key);
		}
		/*
		 * If a valid key is set, encryption should be enabled 
		 * (user may turn it off later).
		 * This is also how "iwconfig ethX key on" works
		 */
		if ((index == current_index) && (key.length > 0))
			force = 1;
	} else {
		int index = (dwrq->flags & IW_ENCODE_INDEX) - 1;
		if ((index >= 0) && (index <= 3)) {
			/* we want to set the key index */
			rvalue |=
			    mgt_set_request(priv, DOT11_OID_DEFKEYID, 0,
					    &index);
		} else {
			if (!dwrq->flags & IW_ENCODE_MODE) {
				/* we cannot do anything. Complain. */
				return -EINVAL;
			}
		}
	}
	/* now read the flags */
	if (dwrq->flags & IW_ENCODE_DISABLED) {
		/* Encoding disabled, 
		 * authen = DOT11_AUTH_OS;
		 * invoke = 0;
		 * exunencrypt = 0; */
	}
	if (dwrq->flags & IW_ENCODE_OPEN)
		/* Encode but accept non-encoded packets. No auth */
		invoke = 1;
	if ((dwrq->flags & IW_ENCODE_RESTRICTED) || force) {
		/* Refuse non-encoded packets. Auth */
		authen = DOT11_AUTH_BOTH;
		invoke = 1;
		exunencrypt = 1;
	}
	/* do the change if requested  */
	if ((dwrq->flags & IW_ENCODE_MODE) || force) {
		rvalue |=
		    mgt_set_request(priv, DOT11_OID_AUTHENABLE, 0, &authen);
		rvalue |=
		    mgt_set_request(priv, DOT11_OID_PRIVACYINVOKED, 0, &invoke);
		rvalue |=
		    mgt_set_request(priv, DOT11_OID_EXUNENCRYPTED, 0,
				    &exunencrypt);
	}
	return rvalue;
}

static int
prism54_get_encode(struct net_device *ndev, struct iw_request_info *info,
		   struct iw_point *dwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	struct obj_key *key;
	u32 devindex, index = (dwrq->flags & IW_ENCODE_INDEX) - 1;
	u32 authen = 0, invoke = 0, exunencrypt = 0;
	int rvalue;
	union oid_res_t r;

	/* first get the flags */
	rvalue = mgt_get_request(priv, DOT11_OID_AUTHENABLE, 0, NULL, &r);
	authen = r.u;
	rvalue |= mgt_get_request(priv, DOT11_OID_PRIVACYINVOKED, 0, NULL, &r);
	invoke = r.u;
	rvalue |= mgt_get_request(priv, DOT11_OID_EXUNENCRYPTED, 0, NULL, &r);
	exunencrypt = r.u;

	if (invoke && (authen == DOT11_AUTH_BOTH) && exunencrypt)
		dwrq->flags = IW_ENCODE_RESTRICTED;
	else if ((authen == DOT11_AUTH_OS) && !exunencrypt) {
		if (invoke)
			dwrq->flags = IW_ENCODE_OPEN;
		else
			dwrq->flags = IW_ENCODE_DISABLED;
	} else
		/* The card should not work in this state */
		dwrq->flags = 0;

	/* get the current device key index */
	rvalue |= mgt_get_request(priv, DOT11_OID_DEFKEYID, 0, NULL, &r);
	devindex = r.u;
	/* Now get the key, return it */
	if ((index < 0) || (index > 3))
		/* no index provided, use the current one */
		index = devindex;
	rvalue |= mgt_get_request(priv, DOT11_OID_DEFKEYX, index, NULL, &r);
	key = r.ptr;
	dwrq->length = key->length;
	memcpy(extra, key->key, dwrq->length);
	kfree(key);
	/* return the used key index */
	dwrq->flags |= devindex + 1;

	return rvalue;
}

static int
prism54_get_txpower(struct net_device *ndev, struct iw_request_info *info,
		    struct iw_param *vwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	union oid_res_t r;
	int rvalue;

	rvalue = mgt_get_request(priv, OID_INL_OUTPUTPOWER, 0, NULL, &r);
	/* intersil firmware operates in 0.25 dBm (1/4 dBm) */
	vwrq->value = (s32) r.u / 4;
	vwrq->fixed = 1;
	/* radio is not turned of
	 * btw: how is possible to turn off only the radio 
	 */
	vwrq->disabled = 0;

	return rvalue;
}

static int
prism54_set_txpower(struct net_device *ndev, struct iw_request_info *info,
		    struct iw_param *vwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	s32 u = vwrq->value;

	/* intersil firmware operates in 0.25 dBm (1/4) */
	u *= 4;
	if (vwrq->disabled) {
		/* don't know how to disable radio */
		printk(KERN_DEBUG
		       "%s: %s() disabling radio is not yet supported.\n",
		       priv->ndev->name, __FUNCTION__);
		return -ENOTSUPP;
	} else if (vwrq->fixed)
		/* currently only fixed value is supported */
		return mgt_set_request(priv, OID_INL_OUTPUTPOWER, 0, &u);
	else {
		printk(KERN_DEBUG
		       "%s: %s() auto power will be implemented later.\n",
		       priv->ndev->name, __FUNCTION__);
		return -ENOTSUPP;
	}
}

static int
prism54_reset(struct net_device *ndev, struct iw_request_info *info,
	      __u32 * uwrq, char *extra)
{
	islpci_reset(netdev_priv(ndev), 0);

	return 0;
}

static int
prism54_get_oid(struct net_device *ndev, struct iw_request_info *info,
		struct iw_point *dwrq, char *extra)
{
	union oid_res_t r;
	int rvalue;
	enum oid_num_t n = dwrq->flags;

	rvalue = mgt_get_request((islpci_private *) ndev->priv, n, 0, NULL, &r);
	dwrq->length = mgt_response_to_str(n, &r, extra);
	if ((isl_oid[n].flags & OID_FLAG_TYPE) != OID_TYPE_U32)
		kfree(r.ptr);
	return rvalue;
}

static int
prism54_set_u32(struct net_device *ndev, struct iw_request_info *info,
		__u32 * uwrq, char *extra)
{
	u32 oid = uwrq[0], u = uwrq[1];

	return mgt_set_request((islpci_private *) ndev->priv, oid, 0, &u);
}

static int
prism54_set_raw(struct net_device *ndev, struct iw_request_info *info,
		struct iw_point *dwrq, char *extra)
{
	u32 oid = dwrq->flags;

	return mgt_set_request((islpci_private *) ndev->priv, oid, 0, extra);
}

void
prism54_acl_init(struct islpci_acl *acl)
{
	sema_init(&acl->sem, 1);
	INIT_LIST_HEAD(&acl->mac_list);
	acl->size = 0;
	acl->policy = MAC_POLICY_OPEN;
}

static void
prism54_clear_mac(struct islpci_acl *acl)
{
	struct list_head *ptr, *next;
	struct mac_entry *entry;

	if (down_interruptible(&acl->sem))
		return;

	if (acl->size == 0) {
		up(&acl->sem);
		return;
	}

	for (ptr = acl->mac_list.next, next = ptr->next;
	     ptr != &acl->mac_list; ptr = next, next = ptr->next) {
		entry = list_entry(ptr, struct mac_entry, _list);
		list_del(ptr);
		kfree(entry);
	}
	acl->size = 0;
	up(&acl->sem);
}

void
prism54_acl_clean(struct islpci_acl *acl)
{
	prism54_clear_mac(acl);
}

static int
prism54_add_mac(struct net_device *ndev, struct iw_request_info *info,
		struct sockaddr *awrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	struct islpci_acl *acl = &priv->acl;
	struct mac_entry *entry;
	struct sockaddr *addr = (struct sockaddr *) extra;

	if (addr->sa_family != ARPHRD_ETHER)
		return -EOPNOTSUPP;

	entry = kmalloc(sizeof (struct mac_entry), GFP_KERNEL);
	if (entry == NULL)
		return -ENOMEM;

	memcpy(entry->addr, addr->sa_data, ETH_ALEN);

	if (down_interruptible(&acl->sem)) {
		kfree(entry);
		return -ERESTARTSYS;
	}
	list_add_tail(&entry->_list, &acl->mac_list);
	acl->size++;
	up(&acl->sem);

	return 0;
}

static int
prism54_del_mac(struct net_device *ndev, struct iw_request_info *info,
		struct sockaddr *awrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	struct islpci_acl *acl = &priv->acl;
	struct mac_entry *entry;
	struct list_head *ptr;
	struct sockaddr *addr = (struct sockaddr *) extra;

	if (addr->sa_family != ARPHRD_ETHER)
		return -EOPNOTSUPP;

	if (down_interruptible(&acl->sem))
		return -ERESTARTSYS;
	for (ptr = acl->mac_list.next; ptr != &acl->mac_list; ptr = ptr->next) {
		entry = list_entry(ptr, struct mac_entry, _list);

		if (memcmp(entry->addr, addr->sa_data, ETH_ALEN) == 0) {
			list_del(ptr);
			acl->size--;
			kfree(entry);
			up(&acl->sem);
			return 0;
		}
	}
	up(&acl->sem);
	return -EINVAL;
}

static int
prism54_get_mac(struct net_device *ndev, struct iw_request_info *info,
		struct iw_point *dwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	struct islpci_acl *acl = &priv->acl;
	struct mac_entry *entry;
	struct list_head *ptr;
	struct sockaddr *dst = (struct sockaddr *) extra;

	dwrq->length = 0;

	if (down_interruptible(&acl->sem))
		return -ERESTARTSYS;

	for (ptr = acl->mac_list.next; ptr != &acl->mac_list; ptr = ptr->next) {
		entry = list_entry(ptr, struct mac_entry, _list);

		memcpy(dst->sa_data, entry->addr, ETH_ALEN);
		dst->sa_family = ARPHRD_ETHER;
		dwrq->length++;
		dst++;
	}
	up(&acl->sem);
	return 0;
}

/* Setting policy also clears the MAC acl, even if we don't change the defaut
 * policy
 */

static int
prism54_set_policy(struct net_device *ndev, struct iw_request_info *info,
		   __u32 * uwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	struct islpci_acl *acl = &priv->acl;
	u32 mlmeautolevel;

	prism54_clear_mac(acl);

	if ((*uwrq < MAC_POLICY_OPEN) || (*uwrq > MAC_POLICY_REJECT))
		return -EINVAL;

	down_write(&priv->mib_sem);

	acl->policy = *uwrq;

	/* the ACL code needs an intermediate mlmeautolevel */
	if ((priv->iw_mode == IW_MODE_MASTER) &&
	    (acl->policy != MAC_POLICY_OPEN))
		mlmeautolevel = DOT11_MLME_INTERMEDIATE;
	else
		mlmeautolevel = CARD_DEFAULT_MLME_MODE;
	if (priv->wpa)
		mlmeautolevel = DOT11_MLME_EXTENDED;
	mgt_set(priv, DOT11_OID_MLMEAUTOLEVEL, &mlmeautolevel);
	/* restart the card with our new policy */
	mgt_commit(priv);
	up_write(&priv->mib_sem);

	return 0;
}

static int
prism54_get_policy(struct net_device *ndev, struct iw_request_info *info,
		   __u32 * uwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	struct islpci_acl *acl = &priv->acl;

	*uwrq = acl->policy;

	return 0;
}

/* Return 1 only if client should be accepted. */

static int
prism54_mac_accept(struct islpci_acl *acl, char *mac)
{
	struct list_head *ptr;
	struct mac_entry *entry;
	int res = 0;

	if (down_interruptible(&acl->sem))
		return -ERESTARTSYS;

	if (acl->policy == MAC_POLICY_OPEN) {
		up(&acl->sem);
		return 1;
	}

	for (ptr = acl->mac_list.next; ptr != &acl->mac_list; ptr = ptr->next) {
		entry = list_entry(ptr, struct mac_entry, _list);
		if (memcmp(entry->addr, mac, ETH_ALEN) == 0) {
			res = 1;
			break;
		}
	}
	res = (acl->policy == MAC_POLICY_ACCEPT) ? !res : res;
	up(&acl->sem);

	return res;
}

static int
prism54_kick_all(struct net_device *ndev, struct iw_request_info *info,
		 struct iw_point *dwrq, char *extra)
{
	struct obj_mlme *mlme;
	int rvalue;

	mlme = kmalloc(sizeof (struct obj_mlme), GFP_KERNEL);
	if (mlme == NULL)
		return -ENOMEM;

	/* Tell the card to kick every client */
	mlme->id = 0;
	rvalue =
	    mgt_set_request(netdev_priv(ndev), DOT11_OID_DISASSOCIATE, 0, mlme);
	kfree(mlme);

	return rvalue;
}

static int
prism54_kick_mac(struct net_device *ndev, struct iw_request_info *info,
		 struct sockaddr *awrq, char *extra)
{
	struct obj_mlme *mlme;
	struct sockaddr *addr = (struct sockaddr *) extra;
	int rvalue;

	if (addr->sa_family != ARPHRD_ETHER)
		return -EOPNOTSUPP;

	mlme = kmalloc(sizeof (struct obj_mlme), GFP_KERNEL);
	if (mlme == NULL)
		return -ENOMEM;

	/* Tell the card to only kick the corresponding bastard */
	memcpy(mlme->address, addr->sa_data, ETH_ALEN);
	mlme->id = -1;
	rvalue =
	    mgt_set_request(netdev_priv(ndev), DOT11_OID_DISASSOCIATE, 0, mlme);

	kfree(mlme);

	return rvalue;
}

/* Translate a TRAP oid into a wireless event. Called in islpci_mgt_receive. */

static void
format_event(islpci_private *priv, char *dest, const char *str,
	     const struct obj_mlme *mlme, u16 *length, int error)
{
	const u8 *a = mlme->address;
	int n = snprintf(dest, IW_CUSTOM_MAX,
			 "%s %s %2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X %s (%2.2X)",
			 str,
			 ((priv->iw_mode == IW_MODE_MASTER) ? "from" : "to"),
			 a[0], a[1], a[2], a[3], a[4], a[5],
			 (error ? (mlme->code ? " : REJECTED " : " : ACCEPTED ")
			  : ""), mlme->code);
	BUG_ON(n > IW_CUSTOM_MAX);
	*length = n;
}

static void
send_formatted_event(islpci_private *priv, const char *str,
		     const struct obj_mlme *mlme, int error)
{
	union iwreq_data wrqu;

	wrqu.data.pointer = kmalloc(IW_CUSTOM_MAX, GFP_KERNEL);
	if (!wrqu.data.pointer)
		return;
	wrqu.data.length = 0;
	format_event(priv, wrqu.data.pointer, str, mlme, &wrqu.data.length,
		     error);
	wireless_send_event(priv->ndev, IWEVCUSTOM, &wrqu, wrqu.data.pointer);
	kfree(wrqu.data.pointer);
}

static void
send_simple_event(islpci_private *priv, const char *str)
{
	union iwreq_data wrqu;
	int n = strlen(str);

	wrqu.data.pointer = kmalloc(IW_CUSTOM_MAX, GFP_KERNEL);
	if (!wrqu.data.pointer)
		return;
	BUG_ON(n > IW_CUSTOM_MAX);
	wrqu.data.length = n;
	strcpy(wrqu.data.pointer, str);
	wireless_send_event(priv->ndev, IWEVCUSTOM, &wrqu, wrqu.data.pointer);
	kfree(wrqu.data.pointer);
}

static void
link_changed(struct net_device *ndev, u32 bitrate)
{
	islpci_private *priv = netdev_priv(ndev);

	if (bitrate) {
		if (priv->iw_mode == IW_MODE_INFRA) {
			union iwreq_data uwrq;
			prism54_get_wap(ndev, NULL, (struct sockaddr *) &uwrq,
					NULL);
			wireless_send_event(ndev, SIOCGIWAP, &uwrq, NULL);
		} else
			send_simple_event(netdev_priv(ndev),
					  "Link established");
	} else
		send_simple_event(netdev_priv(ndev), "Link lost");
}

/* Beacon/ProbeResp payload header */
struct ieee80211_beacon_phdr {
	u8 timestamp[8];
	u16 beacon_int;
	u16 capab_info;
} __attribute__ ((packed));

#define WLAN_EID_GENERIC 0xdd
static u8 wpa_oid[4] = { 0x00, 0x50, 0xf2, 1 };

#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"

void
prism54_wpa_ie_add(islpci_private *priv, u8 *bssid,
		   u8 *wpa_ie, size_t wpa_ie_len)
{
	struct list_head *ptr;
	struct islpci_bss_wpa_ie *bss = NULL;

	if (wpa_ie_len > MAX_WPA_IE_LEN)
		wpa_ie_len = MAX_WPA_IE_LEN;

	if (down_interruptible(&priv->wpa_sem))
		return;

	/* try to use existing entry */
	list_for_each(ptr, &priv->bss_wpa_list) {
		bss = list_entry(ptr, struct islpci_bss_wpa_ie, list);
		if (memcmp(bss->bssid, bssid, ETH_ALEN) == 0) {
			list_move(&bss->list, &priv->bss_wpa_list);
			break;
		}
		bss = NULL;
	}

	if (bss == NULL) {
		/* add a new BSS entry; if max number of entries is already
		 * reached, replace the least recently updated */
		if (priv->num_bss_wpa >= MAX_BSS_WPA_IE_COUNT) {
			bss = list_entry(priv->bss_wpa_list.prev,
					 struct islpci_bss_wpa_ie, list);
			list_del(&bss->list);
		} else {
			bss = kmalloc(sizeof (*bss), GFP_ATOMIC);
			if (bss != NULL) {
				priv->num_bss_wpa++;
				memset(bss, 0, sizeof (*bss));
			}
		}
		if (bss != NULL) {
			memcpy(bss->bssid, bssid, ETH_ALEN);
			list_add(&bss->list, &priv->bss_wpa_list);
		}
	}

	if (bss != NULL) {
		memcpy(bss->wpa_ie, wpa_ie, wpa_ie_len);
		bss->wpa_ie_len = wpa_ie_len;
		bss->last_update = jiffies;
	} else {
		printk(KERN_DEBUG "Failed to add BSS WPA entry for " MACSTR
		       "\n", MAC2STR(bssid));
	}

	/* expire old entries from WPA list */
	while (priv->num_bss_wpa > 0) {
		bss = list_entry(priv->bss_wpa_list.prev,
				 struct islpci_bss_wpa_ie, list);
		if (!time_after(jiffies, bss->last_update + 60 * HZ))
			break;

		list_del(&bss->list);
		priv->num_bss_wpa--;
		kfree(bss);
	}

	up(&priv->wpa_sem);
}

size_t
prism54_wpa_ie_get(islpci_private *priv, u8 *bssid, u8 *wpa_ie)
{
	struct list_head *ptr;
	struct islpci_bss_wpa_ie *bss = NULL;
	size_t len = 0;

	if (down_interruptible(&priv->wpa_sem))
		return 0;

	list_for_each(ptr, &priv->bss_wpa_list) {
		bss = list_entry(ptr, struct islpci_bss_wpa_ie, list);
		if (memcmp(bss->bssid, bssid, ETH_ALEN) == 0)
			break;
		bss = NULL;
	}
	if (bss) {
		len = bss->wpa_ie_len;
		memcpy(wpa_ie, bss->wpa_ie, len);
	}
	up(&priv->wpa_sem);

	return len;
}

void
prism54_wpa_ie_init(islpci_private *priv)
{
	INIT_LIST_HEAD(&priv->bss_wpa_list);
	sema_init(&priv->wpa_sem, 1);
}

void
prism54_wpa_ie_clean(islpci_private *priv)
{
	struct list_head *ptr, *n;

	list_for_each_safe(ptr, n, &priv->bss_wpa_list) {
		struct islpci_bss_wpa_ie *bss;
		bss = list_entry(ptr, struct islpci_bss_wpa_ie, list);
		kfree(bss);
	}
}

static void
prism54_process_bss_data(islpci_private *priv, u32 oid, u8 *addr,
			 u8 *payload, size_t len)
{
	struct ieee80211_beacon_phdr *hdr;
	u8 *pos, *end;

	if (!priv->wpa)
		return;

	hdr = (struct ieee80211_beacon_phdr *) payload;
	pos = (u8 *) (hdr + 1);
	end = payload + len;
	while (pos < end) {
		if (pos + 2 + pos[1] > end) {
			printk(KERN_DEBUG "Parsing Beacon/ProbeResp failed "
			       "for " MACSTR "\n", MAC2STR(addr));
			return;
		}
		if (pos[0] == WLAN_EID_GENERIC && pos[1] >= 4 &&
		    memcmp(pos + 2, wpa_oid, 4) == 0) {
			prism54_wpa_ie_add(priv, addr, pos, pos[1] + 2);
			return;
		}
		pos += 2 + pos[1];
	}
}

static void
handle_request(islpci_private *priv, struct obj_mlme *mlme, enum oid_num_t oid)
{
	if (((mlme->state == DOT11_STATE_AUTHING) ||
	     (mlme->state == DOT11_STATE_ASSOCING))
	    && mgt_mlme_answer(priv)) {
		/* Someone is requesting auth and we must respond. Just send back
		 * the trap with error code set accordingly.
		 */
		mlme->code = prism54_mac_accept(&priv->acl,
						mlme->address) ? 0 : 1;
		mgt_set_request(priv, oid, 0, mlme);
	}
}

int
prism54_process_trap_helper(islpci_private *priv, enum oid_num_t oid,
			    char *data)
{
	struct obj_mlme *mlme = (struct obj_mlme *) data;
	size_t len;
	u8 *payload, *pos = (u8 *) (mlme + 1);

	len = pos[0] | (pos[1] << 8);	/* little endian data length */
	payload = pos + 2;

	/* I think all trapable objects are listed here.
	 * Some oids have a EX version. The difference is that they are emitted
	 * in DOT11_MLME_EXTENDED mode (set with DOT11_OID_MLMEAUTOLEVEL)
	 * with more info.
	 * The few events already defined by the wireless tools are not really
	 * suited. We use the more flexible custom event facility.
	 */

	/* I fear prism54_process_bss_data won't work with big endian data */
	if ((oid == DOT11_OID_BEACON) || (oid == DOT11_OID_PROBE))
		prism54_process_bss_data(priv, oid, mlme->address,
					 payload, len);

	mgt_le_to_cpu(isl_oid[oid].flags & OID_FLAG_TYPE, (void *) mlme);

	switch (oid) {

	case GEN_OID_LINKSTATE:
		link_changed(priv->ndev, (u32) *data);
		break;

	case DOT11_OID_MICFAILURE:
		send_simple_event(priv, "Mic failure");
		break;

	case DOT11_OID_DEAUTHENTICATE:
		send_formatted_event(priv, "DeAuthenticate request", mlme, 0);
		break;

	case DOT11_OID_AUTHENTICATE:
		handle_request(priv, mlme, oid);
		send_formatted_event(priv, "Authenticate request", mlme, 1);
		break;

	case DOT11_OID_DISASSOCIATE:
		send_formatted_event(priv, "Disassociate request", mlme, 0);
		break;

	case DOT11_OID_ASSOCIATE:
		handle_request(priv, mlme, oid);
		send_formatted_event(priv, "Associate request", mlme, 1);
		break;

	case DOT11_OID_REASSOCIATE:
		handle_request(priv, mlme, oid);
		send_formatted_event(priv, "ReAssociate request", mlme, 1);
		break;

	case DOT11_OID_BEACON:
		send_formatted_event(priv,
				     "Received a beacon from an unkown AP",
				     mlme, 0);
		break;

	case DOT11_OID_PROBE:
		/* we received a probe from a client. */
		send_formatted_event(priv, "Received a probe from client", mlme,
				     0);
		break;

		/* Note : "mlme" is actually a "struct obj_mlmeex *" here, but this
		 * is backward compatible layout-wise with "struct obj_mlme".
		 */

	case DOT11_OID_DEAUTHENTICATEEX:
		send_formatted_event(priv, "DeAuthenticate request", mlme, 0);
		break;

	case DOT11_OID_AUTHENTICATEEX:
		handle_request(priv, mlme, oid);
		send_formatted_event(priv, "Authenticate request", mlme, 1);
		break;

	case DOT11_OID_DISASSOCIATEEX:
		send_formatted_event(priv, "Disassociate request", mlme, 0);
		break;

	case DOT11_OID_ASSOCIATEEX:
		handle_request(priv, mlme, oid);
		send_formatted_event(priv, "Associate request", mlme, 1);
		break;

	case DOT11_OID_REASSOCIATEEX:
		handle_request(priv, mlme, oid);
		send_formatted_event(priv, "Reassociate request", mlme, 1);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

/*
 * Process a device trap.  This is called via schedule_work(), outside of
 * interrupt context, no locks held.
 */
void
prism54_process_trap(void *data)
{
	struct islpci_mgmtframe *frame = data;
	struct net_device *ndev = frame->ndev;
	enum oid_num_t n = mgt_oidtonum(frame->header->oid);

	if (n != OID_NUM_LAST)
		prism54_process_trap_helper(netdev_priv(ndev), n, frame->data);
	islpci_mgt_release(frame);
}

int
prism54_set_mac_address(struct net_device *ndev, void *addr)
{
	islpci_private *priv = netdev_priv(ndev);
	int ret;

	if (ndev->addr_len != 6)
		return -EINVAL;
	ret = mgt_set_request(priv, GEN_OID_MACADDRESS, 0,
			      &((struct sockaddr *) addr)->sa_data);
	if (!ret)
		memcpy(priv->ndev->dev_addr,
		       &((struct sockaddr *) addr)->sa_data, 6);

	return ret;
}

int
prism54_set_wpa(struct net_device *ndev, struct iw_request_info *info,
		__u32 * uwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);

	down_write(&priv->mib_sem);

	priv->wpa = *uwrq;
	if (priv->wpa) {
		u32 l = DOT11_MLME_EXTENDED;
		mgt_set(priv, DOT11_OID_MLMEAUTOLEVEL, &l);
	}
	/* restart the card with new level. Needed ? */
	mgt_commit(priv);
	up_write(&priv->mib_sem);

	return 0;
}

int
prism54_get_wpa(struct net_device *ndev, struct iw_request_info *info,
		__u32 * uwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	*uwrq = priv->wpa;
	return 0;
}

int
prism54_set_prismhdr(struct net_device *ndev, struct iw_request_info *info,
		     __u32 * uwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	priv->monitor_type =
	    (*uwrq ? ARPHRD_IEEE80211_PRISM : ARPHRD_IEEE80211);
	if (priv->iw_mode == IW_MODE_MONITOR)
		priv->ndev->type = priv->monitor_type;

	return 0;
}

int
prism54_get_prismhdr(struct net_device *ndev, struct iw_request_info *info,
		     __u32 * uwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	*uwrq = (priv->monitor_type == ARPHRD_IEEE80211_PRISM);
	return 0;
}

int
prism54_debug_oid(struct net_device *ndev, struct iw_request_info *info,
		  __u32 * uwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);

	priv->priv_oid = *uwrq;
	printk("%s: oid 0x%08X\n", ndev->name, *uwrq);

	return 0;
}

int
prism54_debug_get_oid(struct net_device *ndev, struct iw_request_info *info,
		      struct iw_point *data, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	struct islpci_mgmtframe *response = NULL;
	int ret = -EIO;

	printk("%s: get_oid 0x%08X\n", ndev->name, priv->priv_oid);
	data->length = 0;

	if (islpci_get_state(priv) >= PRV_STATE_INIT) {
		ret =
		    islpci_mgt_transaction(priv->ndev, PIMFOR_OP_GET,
					   priv->priv_oid, extra, 256,
					   &response);
		printk("%s: ret: %i\n", ndev->name, ret);
		if (ret || !response
		    || response->header->operation == PIMFOR_OP_ERROR) {
			if (response) {
				islpci_mgt_release(response);
			}
			printk("%s: EIO\n", ndev->name);
			ret = -EIO;
		}
		if (!ret) {
			data->length = response->header->length;
			memcpy(extra, response->data, data->length);
			islpci_mgt_release(response);
			printk("%s: len: %i\n", ndev->name, data->length);
		}
	}

	return ret;
}

int
prism54_debug_set_oid(struct net_device *ndev, struct iw_request_info *info,
		      struct iw_point *data, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	struct islpci_mgmtframe *response = NULL;
	int ret = 0, response_op = PIMFOR_OP_ERROR;

	printk("%s: set_oid 0x%08X\tlen: %d\n", ndev->name, priv->priv_oid,
	       data->length);

	if (islpci_get_state(priv) >= PRV_STATE_INIT) {
		ret =
		    islpci_mgt_transaction(priv->ndev, PIMFOR_OP_SET,
					   priv->priv_oid, extra, data->length,
					   &response);
		printk("%s: ret: %i\n", ndev->name, ret);
		if (ret || !response
		    || response->header->operation == PIMFOR_OP_ERROR) {
			if (response) {
				islpci_mgt_release(response);
			}
			printk("%s: EIO\n", ndev->name);
			ret = -EIO;
		}
		if (!ret) {
			response_op = response->header->operation;
			printk("%s: response_op: %i\n", ndev->name,
			       response_op);
			islpci_mgt_release(response);
		}
	}

	return (ret ? ret : -EINPROGRESS);
}

static int
prism54_set_spy(struct net_device *ndev,
		struct iw_request_info *info,
		union iwreq_data *uwrq, char *extra)
{
	islpci_private *priv = netdev_priv(ndev);
	u32 u, oid = OID_INL_CONFIG;

	down_write(&priv->mib_sem);
	mgt_get(priv, OID_INL_CONFIG, &u);

	if ((uwrq->data.length == 0) && (priv->spy_data.spy_number > 0))
		/* disable spy */
		u &= ~INL_CONFIG_RXANNEX;
	else if ((uwrq->data.length > 0) && (priv->spy_data.spy_number == 0))
		/* enable spy */
		u |= INL_CONFIG_RXANNEX;

	mgt_set(priv, OID_INL_CONFIG, &u);
	mgt_commit_list(priv, &oid, 1);
	up_write(&priv->mib_sem);

	return iw_handler_set_spy(ndev, info, uwrq, extra);
}

static const iw_handler prism54_handler[] = {
	(iw_handler) prism54_commit,	/* SIOCSIWCOMMIT */
	(iw_handler) prism54_get_name,	/* SIOCGIWNAME */
	(iw_handler) NULL,	/* SIOCSIWNWID */
	(iw_handler) NULL,	/* SIOCGIWNWID */
	(iw_handler) prism54_set_freq,	/* SIOCSIWFREQ */
	(iw_handler) prism54_get_freq,	/* SIOCGIWFREQ */
	(iw_handler) prism54_set_mode,	/* SIOCSIWMODE */
	(iw_handler) prism54_get_mode,	/* SIOCGIWMODE */
	(iw_handler) prism54_set_sens,	/* SIOCSIWSENS */
	(iw_handler) prism54_get_sens,	/* SIOCGIWSENS */
	(iw_handler) NULL,	/* SIOCSIWRANGE */
	(iw_handler) prism54_get_range,	/* SIOCGIWRANGE */
	(iw_handler) NULL,	/* SIOCSIWPRIV */
	(iw_handler) NULL,	/* SIOCGIWPRIV */
	(iw_handler) NULL,	/* SIOCSIWSTATS */
	(iw_handler) NULL,	/* SIOCGIWSTATS */
	prism54_set_spy,	/* SIOCSIWSPY */
	iw_handler_get_spy,	/* SIOCGIWSPY */
	iw_handler_set_thrspy,	/* SIOCSIWTHRSPY */
	iw_handler_get_thrspy,	/* SIOCGIWTHRSPY */
	(iw_handler) prism54_set_wap,	/* SIOCSIWAP */
	(iw_handler) prism54_get_wap,	/* SIOCGIWAP */
	(iw_handler) NULL,	/* -- hole -- */
	(iw_handler) NULL,	/* SIOCGIWAPLIST depreciated */
	(iw_handler) prism54_set_scan,	/* SIOCSIWSCAN */
	(iw_handler) prism54_get_scan,	/* SIOCGIWSCAN */
	(iw_handler) prism54_set_essid,	/* SIOCSIWESSID */
	(iw_handler) prism54_get_essid,	/* SIOCGIWESSID */
	(iw_handler) prism54_set_nick,	/* SIOCSIWNICKN */
	(iw_handler) prism54_get_nick,	/* SIOCGIWNICKN */
	(iw_handler) NULL,	/* -- hole -- */
	(iw_handler) NULL,	/* -- hole -- */
	(iw_handler) prism54_set_rate,	/* SIOCSIWRATE */
	(iw_handler) prism54_get_rate,	/* SIOCGIWRATE */
	(iw_handler) prism54_set_rts,	/* SIOCSIWRTS */
	(iw_handler) prism54_get_rts,	/* SIOCGIWRTS */
	(iw_handler) prism54_set_frag,	/* SIOCSIWFRAG */
	(iw_handler) prism54_get_frag,	/* SIOCGIWFRAG */
	(iw_handler) prism54_set_txpower,	/* SIOCSIWTXPOW */
	(iw_handler) prism54_get_txpower,	/* SIOCGIWTXPOW */
	(iw_handler) prism54_set_retry,	/* SIOCSIWRETRY */
	(iw_handler) prism54_get_retry,	/* SIOCGIWRETRY */
	(iw_handler) prism54_set_encode,	/* SIOCSIWENCODE */
	(iw_handler) prism54_get_encode,	/* SIOCGIWENCODE */
	(iw_handler) NULL,	/* SIOCSIWPOWER */
	(iw_handler) NULL,	/* SIOCGIWPOWER */
};

/* The low order bit identify a SET (0) or a GET (1) ioctl.  */

#define PRISM54_RESET		SIOCIWFIRSTPRIV
#define PRISM54_GET_POLICY	SIOCIWFIRSTPRIV+1
#define PRISM54_SET_POLICY	SIOCIWFIRSTPRIV+2
#define PRISM54_GET_MAC		SIOCIWFIRSTPRIV+3
#define PRISM54_ADD_MAC		SIOCIWFIRSTPRIV+4

#define PRISM54_DEL_MAC		SIOCIWFIRSTPRIV+6

#define PRISM54_KICK_MAC	SIOCIWFIRSTPRIV+8

#define PRISM54_KICK_ALL	SIOCIWFIRSTPRIV+10

#define PRISM54_GET_WPA		SIOCIWFIRSTPRIV+11
#define PRISM54_SET_WPA		SIOCIWFIRSTPRIV+12

#define PRISM54_DBG_OID		SIOCIWFIRSTPRIV+14
#define PRISM54_DBG_GET_OID	SIOCIWFIRSTPRIV+15
#define PRISM54_DBG_SET_OID	SIOCIWFIRSTPRIV+16

#define PRISM54_GET_OID		SIOCIWFIRSTPRIV+17
#define PRISM54_SET_OID_U32	SIOCIWFIRSTPRIV+18
#define	PRISM54_SET_OID_STR	SIOCIWFIRSTPRIV+20
#define	PRISM54_SET_OID_ADDR	SIOCIWFIRSTPRIV+22

#define PRISM54_GET_PRISMHDR	SIOCIWFIRSTPRIV+23
#define PRISM54_SET_PRISMHDR	SIOCIWFIRSTPRIV+24

#define IWPRIV_SET_U32(n,x)	{ n, IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1, 0, "s_"x }
#define IWPRIV_SET_SSID(n,x)	{ n, IW_PRIV_TYPE_CHAR | IW_PRIV_SIZE_FIXED | 1, 0, "s_"x }
#define IWPRIV_SET_ADDR(n,x)	{ n, IW_PRIV_TYPE_ADDR | IW_PRIV_SIZE_FIXED | 1, 0, "s_"x }
#define IWPRIV_GET(n,x)	{ n, 0, IW_PRIV_TYPE_CHAR | IW_PRIV_SIZE_FIXED | PRIV_STR_SIZE, "g_"x }

#define IWPRIV_U32(n,x)		IWPRIV_SET_U32(n,x), IWPRIV_GET(n,x)
#define IWPRIV_SSID(n,x)	IWPRIV_SET_SSID(n,x), IWPRIV_GET(n,x)
#define IWPRIV_ADDR(n,x)	IWPRIV_SET_ADDR(n,x), IWPRIV_GET(n,x)

/* Note : limited to 128 private ioctls (wireless tools 26) */

static const struct iw_priv_args prism54_private_args[] = {
/*{ cmd, set_args, get_args, name } */
	{PRISM54_RESET, 0, 0, "reset"},
	{PRISM54_GET_PRISMHDR, 0, IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
	 "get_prismhdr"},
	{PRISM54_SET_PRISMHDR, IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1, 0,
	 "set_prismhdr"},
	{PRISM54_GET_POLICY, 0, IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
	 "getPolicy"},
	{PRISM54_SET_POLICY, IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1, 0,
	 "setPolicy"},
	{PRISM54_GET_MAC, 0, IW_PRIV_TYPE_ADDR | 64, "getMac"},
	{PRISM54_ADD_MAC, IW_PRIV_TYPE_ADDR | IW_PRIV_SIZE_FIXED | 1, 0,
	 "addMac"},
	{PRISM54_DEL_MAC, IW_PRIV_TYPE_ADDR | IW_PRIV_SIZE_FIXED | 1, 0,
	 "delMac"},
	{PRISM54_KICK_MAC, IW_PRIV_TYPE_ADDR | IW_PRIV_SIZE_FIXED | 1, 0,
	 "kickMac"},
	{PRISM54_KICK_ALL, 0, 0, "kickAll"},
	{PRISM54_GET_WPA, 0, IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
	 "get_wpa"},
	{PRISM54_SET_WPA, IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1, 0,
	 "set_wpa"},
	{PRISM54_DBG_OID, IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1, 0,
	 "dbg_oid"},
	{PRISM54_DBG_GET_OID, 0, IW_PRIV_TYPE_BYTE | 256, "dbg_get_oid"},
	{PRISM54_DBG_SET_OID, IW_PRIV_TYPE_BYTE | 256, 0, "dbg_set_oid"},
	/* --- sub-ioctls handlers --- */
	{PRISM54_GET_OID,
	 0, IW_PRIV_TYPE_CHAR | IW_PRIV_SIZE_FIXED | PRIV_STR_SIZE, ""},
	{PRISM54_SET_OID_U32,
	 IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1, 0, ""},
	{PRISM54_SET_OID_STR,
	 IW_PRIV_TYPE_CHAR | IW_PRIV_SIZE_FIXED | 1, 0, ""},
	{PRISM54_SET_OID_ADDR,
	 IW_PRIV_TYPE_ADDR | IW_PRIV_SIZE_FIXED | 1, 0, ""},
	/* --- sub-ioctls definitions --- */
	IWPRIV_ADDR(GEN_OID_MACADDRESS, "addr"),
	IWPRIV_GET(GEN_OID_LINKSTATE, "linkstate"),
	IWPRIV_U32(DOT11_OID_BSSTYPE, "bsstype"),
	IWPRIV_ADDR(DOT11_OID_BSSID, "bssid"),
	IWPRIV_U32(DOT11_OID_STATE, "state"),
	IWPRIV_U32(DOT11_OID_AID, "aid"),

	IWPRIV_SSID(DOT11_OID_SSIDOVERRIDE, "ssidoverride"),

	IWPRIV_U32(DOT11_OID_MEDIUMLIMIT, "medlimit"),
	IWPRIV_U32(DOT11_OID_BEACONPERIOD, "beacon"),
	IWPRIV_U32(DOT11_OID_DTIMPERIOD, "dtimperiod"),

	IWPRIV_U32(DOT11_OID_AUTHENABLE, "authenable"),
	IWPRIV_U32(DOT11_OID_PRIVACYINVOKED, "privinvok"),
	IWPRIV_U32(DOT11_OID_EXUNENCRYPTED, "exunencrypt"),

	IWPRIV_U32(DOT11_OID_REKEYTHRESHOLD, "rekeythresh"),

	IWPRIV_U32(DOT11_OID_MAXTXLIFETIME, "maxtxlife"),
	IWPRIV_U32(DOT11_OID_MAXRXLIFETIME, "maxrxlife"),
	IWPRIV_U32(DOT11_OID_ALOFT_FIXEDRATE, "fixedrate"),
	IWPRIV_U32(DOT11_OID_MAXFRAMEBURST, "frameburst"),
	IWPRIV_U32(DOT11_OID_PSM, "psm"),

	IWPRIV_U32(DOT11_OID_BRIDGELOCAL, "bridge"),
	IWPRIV_U32(DOT11_OID_CLIENTS, "clients"),
	IWPRIV_U32(DOT11_OID_CLIENTSASSOCIATED, "clientassoc"),
	IWPRIV_U32(DOT11_OID_DOT1XENABLE, "dot1xenable"),
	IWPRIV_U32(DOT11_OID_ANTENNARX, "rxant"),
	IWPRIV_U32(DOT11_OID_ANTENNATX, "txant"),
	IWPRIV_U32(DOT11_OID_ANTENNADIVERSITY, "antdivers"),
	IWPRIV_U32(DOT11_OID_EDTHRESHOLD, "edthresh"),
	IWPRIV_U32(DOT11_OID_PREAMBLESETTINGS, "preamble"),
	IWPRIV_GET(DOT11_OID_RATES, "rates"),
	IWPRIV_U32(DOT11_OID_OUTPUTPOWER, ".11outpower"),
	IWPRIV_GET(DOT11_OID_SUPPORTEDRATES, "supprates"),
	IWPRIV_GET(DOT11_OID_SUPPORTEDFREQUENCIES, "suppfreq"),

	IWPRIV_U32(DOT11_OID_NOISEFLOOR, "noisefloor"),
	IWPRIV_GET(DOT11_OID_FREQUENCYACTIVITY, "freqactivity"),
	IWPRIV_U32(DOT11_OID_NONERPPROTECTION, "nonerpprotec"),
	IWPRIV_U32(DOT11_OID_PROFILES, "profile"),
	IWPRIV_GET(DOT11_OID_EXTENDEDRATES, "extrates"),
	IWPRIV_U32(DOT11_OID_MLMEAUTOLEVEL, "mlmelevel"),

	IWPRIV_GET(DOT11_OID_BSSS, "bsss"),
	IWPRIV_GET(DOT11_OID_BSSLIST, "bsslist"),
	IWPRIV_U32(OID_INL_MODE, "mode"),
	IWPRIV_U32(OID_INL_CONFIG, "config"),
	IWPRIV_U32(OID_INL_DOT11D_CONFORMANCE, ".11dconform"),
	IWPRIV_GET(OID_INL_PHYCAPABILITIES, "phycapa"),
	IWPRIV_U32(OID_INL_OUTPUTPOWER, "outpower"),
};

static const iw_handler prism54_private_handler[] = {
	(iw_handler) prism54_reset,
	(iw_handler) prism54_get_policy,
	(iw_handler) prism54_set_policy,
	(iw_handler) prism54_get_mac,
	(iw_handler) prism54_add_mac,
	(iw_handler) NULL,
	(iw_handler) prism54_del_mac,
	(iw_handler) NULL,
	(iw_handler) prism54_kick_mac,
	(iw_handler) NULL,
	(iw_handler) prism54_kick_all,
	(iw_handler) prism54_get_wpa,
	(iw_handler) prism54_set_wpa,
	(iw_handler) NULL,
	(iw_handler) prism54_debug_oid,
	(iw_handler) prism54_debug_get_oid,
	(iw_handler) prism54_debug_set_oid,
	(iw_handler) prism54_get_oid,
	(iw_handler) prism54_set_u32,
	(iw_handler) NULL,
	(iw_handler) prism54_set_raw,
	(iw_handler) NULL,
	(iw_handler) prism54_set_raw,
	(iw_handler) prism54_get_prismhdr,
	(iw_handler) prism54_set_prismhdr,
};

const struct iw_handler_def prism54_handler_def = {
	.num_standard = sizeof (prism54_handler) / sizeof (iw_handler),
	.num_private = sizeof (prism54_private_handler) / sizeof (iw_handler),
	.num_private_args =
	    sizeof (prism54_private_args) / sizeof (struct iw_priv_args),
	.standard = (iw_handler *) prism54_handler,
	.private = (iw_handler *) prism54_private_handler,
	.private_args = (struct iw_priv_args *) prism54_private_args,
	.spy_offset = offsetof(islpci_private, spy_data),
};

/* For ioctls that don't work with the new API */

int
prism54_ioctl(struct net_device *ndev, struct ifreq *rq, int cmd)
{

	return -EOPNOTSUPP;
}
