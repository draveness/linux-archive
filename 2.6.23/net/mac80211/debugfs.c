/*
 * mac80211 debugfs for wireless PHYs
 *
 * Copyright 2007	Johannes Berg <johannes@sipsolutions.net>
 *
 * GPLv2
 *
 */

#include <linux/debugfs.h>
#include <linux/rtnetlink.h>
#include "ieee80211_i.h"
#include "ieee80211_rate.h"
#include "debugfs.h"

int mac80211_open_file_generic(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static const char *ieee80211_mode_str(int mode)
{
	switch (mode) {
	case MODE_IEEE80211A:
		return "IEEE 802.11a";
	case MODE_IEEE80211B:
		return "IEEE 802.11b";
	case MODE_IEEE80211G:
		return "IEEE 802.11g";
	case MODE_ATHEROS_TURBO:
		return "Atheros Turbo (5 GHz)";
	default:
		return "UNKNOWN";
	}
}

static ssize_t modes_read(struct file *file, char __user *userbuf,
			  size_t count, loff_t *ppos)
{
	struct ieee80211_local *local = file->private_data;
	struct ieee80211_hw_mode *mode;
	char buf[150], *p = buf;

	/* FIXME: locking! */
	list_for_each_entry(mode, &local->modes_list, list) {
		p += scnprintf(p, sizeof(buf)+buf-p,
			       "%s\n", ieee80211_mode_str(mode->mode));
	}

	return simple_read_from_buffer(userbuf, count, ppos, buf, p-buf);
}

static const struct file_operations modes_ops = {
	.read = modes_read,
	.open = mac80211_open_file_generic,
};

#define DEBUGFS_READONLY_FILE(name, buflen, fmt, value...)		\
static ssize_t name## _read(struct file *file, char __user *userbuf,	\
			    size_t count, loff_t *ppos)			\
{									\
	struct ieee80211_local *local = file->private_data;		\
	char buf[buflen];						\
	int res;							\
									\
	res = scnprintf(buf, buflen, fmt "\n", ##value);		\
	return simple_read_from_buffer(userbuf, count, ppos, buf, res);	\
}									\
									\
static const struct file_operations name## _ops = {			\
	.read = name## _read,						\
	.open = mac80211_open_file_generic,				\
};

#define DEBUGFS_ADD(name)						\
	local->debugfs.name = debugfs_create_file(#name, 0444, phyd,	\
						  local, &name## _ops);

#define DEBUGFS_DEL(name)						\
	debugfs_remove(local->debugfs.name);				\
	local->debugfs.name = NULL;


DEBUGFS_READONLY_FILE(channel, 20, "%d",
		      local->hw.conf.channel);
DEBUGFS_READONLY_FILE(frequency, 20, "%d",
		      local->hw.conf.freq);
DEBUGFS_READONLY_FILE(radar_detect, 20, "%d",
		      local->hw.conf.radar_detect);
DEBUGFS_READONLY_FILE(antenna_sel_tx, 20, "%d",
		      local->hw.conf.antenna_sel_tx);
DEBUGFS_READONLY_FILE(antenna_sel_rx, 20, "%d",
		      local->hw.conf.antenna_sel_rx);
DEBUGFS_READONLY_FILE(bridge_packets, 20, "%d",
		      local->bridge_packets);
DEBUGFS_READONLY_FILE(key_tx_rx_threshold, 20, "%d",
		      local->key_tx_rx_threshold);
DEBUGFS_READONLY_FILE(rts_threshold, 20, "%d",
		      local->rts_threshold);
DEBUGFS_READONLY_FILE(fragmentation_threshold, 20, "%d",
		      local->fragmentation_threshold);
DEBUGFS_READONLY_FILE(short_retry_limit, 20, "%d",
		      local->short_retry_limit);
DEBUGFS_READONLY_FILE(long_retry_limit, 20, "%d",
		      local->long_retry_limit);
DEBUGFS_READONLY_FILE(total_ps_buffered, 20, "%d",
		      local->total_ps_buffered);
DEBUGFS_READONLY_FILE(mode, 20, "%s",
		      ieee80211_mode_str(local->hw.conf.phymode));
DEBUGFS_READONLY_FILE(wep_iv, 20, "%#06x",
		      local->wep_iv & 0xffffff);
DEBUGFS_READONLY_FILE(tx_power_reduction, 20, "%d.%d dBm",
		      local->hw.conf.tx_power_reduction / 10,
		      local->hw.conf.tx_power_reduction % 10);
DEBUGFS_READONLY_FILE(rate_ctrl_alg, 100, "%s",
		      local->rate_ctrl ? local->rate_ctrl->ops->name : "<unset>");

/* statistics stuff */

static inline int rtnl_lock_local(struct ieee80211_local *local)
{
	rtnl_lock();
	if (unlikely(local->reg_state != IEEE80211_DEV_REGISTERED)) {
		rtnl_unlock();
		return -ENODEV;
	}
	return 0;
}

#define DEBUGFS_STATS_FILE(name, buflen, fmt, value...)			\
	DEBUGFS_READONLY_FILE(stats_ ##name, buflen, fmt, ##value)

static ssize_t format_devstat_counter(struct ieee80211_local *local,
	char __user *userbuf,
	size_t count, loff_t *ppos,
	int (*printvalue)(struct ieee80211_low_level_stats *stats, char *buf,
			  int buflen))
{
	struct ieee80211_low_level_stats stats;
	char buf[20];
	int res;

	if (!local->ops->get_stats)
		return -EOPNOTSUPP;

	res = rtnl_lock_local(local);
	if (res)
		return res;

	res = local->ops->get_stats(local_to_hw(local), &stats);
	rtnl_unlock();
	if (!res)
		res = printvalue(&stats, buf, sizeof(buf));
	return simple_read_from_buffer(userbuf, count, ppos, buf, res);
}

#define DEBUGFS_DEVSTATS_FILE(name)					\
static int print_devstats_##name(struct ieee80211_low_level_stats *stats,\
				 char *buf, int buflen)			\
{									\
	return scnprintf(buf, buflen, "%u\n", stats->name);		\
}									\
static ssize_t stats_ ##name## _read(struct file *file,			\
				     char __user *userbuf,		\
				     size_t count, loff_t *ppos)	\
{									\
	return format_devstat_counter(file->private_data,		\
				      userbuf,				\
				      count,				\
				      ppos,				\
				      print_devstats_##name);		\
}									\
									\
static const struct file_operations stats_ ##name## _ops = {		\
	.read = stats_ ##name## _read,					\
	.open = mac80211_open_file_generic,				\
};

#define DEBUGFS_STATS_ADD(name)						\
	local->debugfs.stats.name = debugfs_create_file(#name, 0444, statsd,\
		local, &stats_ ##name## _ops);

#define DEBUGFS_STATS_DEL(name)						\
	debugfs_remove(local->debugfs.stats.name);			\
	local->debugfs.stats.name = NULL;

DEBUGFS_STATS_FILE(transmitted_fragment_count, 20, "%u",
		   local->dot11TransmittedFragmentCount);
DEBUGFS_STATS_FILE(multicast_transmitted_frame_count, 20, "%u",
		   local->dot11MulticastTransmittedFrameCount);
DEBUGFS_STATS_FILE(failed_count, 20, "%u",
		   local->dot11FailedCount);
DEBUGFS_STATS_FILE(retry_count, 20, "%u",
		   local->dot11RetryCount);
DEBUGFS_STATS_FILE(multiple_retry_count, 20, "%u",
		   local->dot11MultipleRetryCount);
DEBUGFS_STATS_FILE(frame_duplicate_count, 20, "%u",
		   local->dot11FrameDuplicateCount);
DEBUGFS_STATS_FILE(received_fragment_count, 20, "%u",
		   local->dot11ReceivedFragmentCount);
DEBUGFS_STATS_FILE(multicast_received_frame_count, 20, "%u",
		   local->dot11MulticastReceivedFrameCount);
DEBUGFS_STATS_FILE(transmitted_frame_count, 20, "%u",
		   local->dot11TransmittedFrameCount);
DEBUGFS_STATS_FILE(wep_undecryptable_count, 20, "%u",
		   local->dot11WEPUndecryptableCount);
#ifdef CONFIG_MAC80211_DEBUG_COUNTERS
DEBUGFS_STATS_FILE(tx_handlers_drop, 20, "%u",
		   local->tx_handlers_drop);
DEBUGFS_STATS_FILE(tx_handlers_queued, 20, "%u",
		   local->tx_handlers_queued);
DEBUGFS_STATS_FILE(tx_handlers_drop_unencrypted, 20, "%u",
		   local->tx_handlers_drop_unencrypted);
DEBUGFS_STATS_FILE(tx_handlers_drop_fragment, 20, "%u",
		   local->tx_handlers_drop_fragment);
DEBUGFS_STATS_FILE(tx_handlers_drop_wep, 20, "%u",
		   local->tx_handlers_drop_wep);
DEBUGFS_STATS_FILE(tx_handlers_drop_not_assoc, 20, "%u",
		   local->tx_handlers_drop_not_assoc);
DEBUGFS_STATS_FILE(tx_handlers_drop_unauth_port, 20, "%u",
		   local->tx_handlers_drop_unauth_port);
DEBUGFS_STATS_FILE(rx_handlers_drop, 20, "%u",
		   local->rx_handlers_drop);
DEBUGFS_STATS_FILE(rx_handlers_queued, 20, "%u",
		   local->rx_handlers_queued);
DEBUGFS_STATS_FILE(rx_handlers_drop_nullfunc, 20, "%u",
		   local->rx_handlers_drop_nullfunc);
DEBUGFS_STATS_FILE(rx_handlers_drop_defrag, 20, "%u",
		   local->rx_handlers_drop_defrag);
DEBUGFS_STATS_FILE(rx_handlers_drop_short, 20, "%u",
		   local->rx_handlers_drop_short);
DEBUGFS_STATS_FILE(rx_handlers_drop_passive_scan, 20, "%u",
		   local->rx_handlers_drop_passive_scan);
DEBUGFS_STATS_FILE(tx_expand_skb_head, 20, "%u",
		   local->tx_expand_skb_head);
DEBUGFS_STATS_FILE(tx_expand_skb_head_cloned, 20, "%u",
		   local->tx_expand_skb_head_cloned);
DEBUGFS_STATS_FILE(rx_expand_skb_head, 20, "%u",
		   local->rx_expand_skb_head);
DEBUGFS_STATS_FILE(rx_expand_skb_head2, 20, "%u",
		   local->rx_expand_skb_head2);
DEBUGFS_STATS_FILE(rx_handlers_fragments, 20, "%u",
		   local->rx_handlers_fragments);
DEBUGFS_STATS_FILE(tx_status_drop, 20, "%u",
		   local->tx_status_drop);

static ssize_t stats_wme_rx_queue_read(struct file *file,
				       char __user *userbuf,
				       size_t count, loff_t *ppos)
{
	struct ieee80211_local *local = file->private_data;
	char buf[NUM_RX_DATA_QUEUES*15], *p = buf;
	int i;

	for (i = 0; i < NUM_RX_DATA_QUEUES; i++)
		p += scnprintf(p, sizeof(buf)+buf-p,
			       "%u\n", local->wme_rx_queue[i]);

	return simple_read_from_buffer(userbuf, count, ppos, buf, p-buf);
}

static const struct file_operations stats_wme_rx_queue_ops = {
	.read = stats_wme_rx_queue_read,
	.open = mac80211_open_file_generic,
};

static ssize_t stats_wme_tx_queue_read(struct file *file,
				       char __user *userbuf,
				       size_t count, loff_t *ppos)
{
	struct ieee80211_local *local = file->private_data;
	char buf[NUM_TX_DATA_QUEUES*15], *p = buf;
	int i;

	for (i = 0; i < NUM_TX_DATA_QUEUES; i++)
		p += scnprintf(p, sizeof(buf)+buf-p,
			       "%u\n", local->wme_tx_queue[i]);

	return simple_read_from_buffer(userbuf, count, ppos, buf, p-buf);
}

static const struct file_operations stats_wme_tx_queue_ops = {
	.read = stats_wme_tx_queue_read,
	.open = mac80211_open_file_generic,
};
#endif

DEBUGFS_DEVSTATS_FILE(dot11ACKFailureCount);
DEBUGFS_DEVSTATS_FILE(dot11RTSFailureCount);
DEBUGFS_DEVSTATS_FILE(dot11FCSErrorCount);
DEBUGFS_DEVSTATS_FILE(dot11RTSSuccessCount);


void debugfs_hw_add(struct ieee80211_local *local)
{
	struct dentry *phyd = local->hw.wiphy->debugfsdir;
	struct dentry *statsd;

	if (!phyd)
		return;

	local->debugfs.stations = debugfs_create_dir("stations", phyd);
	local->debugfs.keys = debugfs_create_dir("keys", phyd);

	DEBUGFS_ADD(channel);
	DEBUGFS_ADD(frequency);
	DEBUGFS_ADD(radar_detect);
	DEBUGFS_ADD(antenna_sel_tx);
	DEBUGFS_ADD(antenna_sel_rx);
	DEBUGFS_ADD(bridge_packets);
	DEBUGFS_ADD(key_tx_rx_threshold);
	DEBUGFS_ADD(rts_threshold);
	DEBUGFS_ADD(fragmentation_threshold);
	DEBUGFS_ADD(short_retry_limit);
	DEBUGFS_ADD(long_retry_limit);
	DEBUGFS_ADD(total_ps_buffered);
	DEBUGFS_ADD(mode);
	DEBUGFS_ADD(wep_iv);
	DEBUGFS_ADD(tx_power_reduction);
	DEBUGFS_ADD(modes);

	statsd = debugfs_create_dir("statistics", phyd);
	local->debugfs.statistics = statsd;

	/* if the dir failed, don't put all the other things into the root! */
	if (!statsd)
		return;

	DEBUGFS_STATS_ADD(transmitted_fragment_count);
	DEBUGFS_STATS_ADD(multicast_transmitted_frame_count);
	DEBUGFS_STATS_ADD(failed_count);
	DEBUGFS_STATS_ADD(retry_count);
	DEBUGFS_STATS_ADD(multiple_retry_count);
	DEBUGFS_STATS_ADD(frame_duplicate_count);
	DEBUGFS_STATS_ADD(received_fragment_count);
	DEBUGFS_STATS_ADD(multicast_received_frame_count);
	DEBUGFS_STATS_ADD(transmitted_frame_count);
	DEBUGFS_STATS_ADD(wep_undecryptable_count);
#ifdef CONFIG_MAC80211_DEBUG_COUNTERS
	DEBUGFS_STATS_ADD(tx_handlers_drop);
	DEBUGFS_STATS_ADD(tx_handlers_queued);
	DEBUGFS_STATS_ADD(tx_handlers_drop_unencrypted);
	DEBUGFS_STATS_ADD(tx_handlers_drop_fragment);
	DEBUGFS_STATS_ADD(tx_handlers_drop_wep);
	DEBUGFS_STATS_ADD(tx_handlers_drop_not_assoc);
	DEBUGFS_STATS_ADD(tx_handlers_drop_unauth_port);
	DEBUGFS_STATS_ADD(rx_handlers_drop);
	DEBUGFS_STATS_ADD(rx_handlers_queued);
	DEBUGFS_STATS_ADD(rx_handlers_drop_nullfunc);
	DEBUGFS_STATS_ADD(rx_handlers_drop_defrag);
	DEBUGFS_STATS_ADD(rx_handlers_drop_short);
	DEBUGFS_STATS_ADD(rx_handlers_drop_passive_scan);
	DEBUGFS_STATS_ADD(tx_expand_skb_head);
	DEBUGFS_STATS_ADD(tx_expand_skb_head_cloned);
	DEBUGFS_STATS_ADD(rx_expand_skb_head);
	DEBUGFS_STATS_ADD(rx_expand_skb_head2);
	DEBUGFS_STATS_ADD(rx_handlers_fragments);
	DEBUGFS_STATS_ADD(tx_status_drop);
	DEBUGFS_STATS_ADD(wme_tx_queue);
	DEBUGFS_STATS_ADD(wme_rx_queue);
#endif
	DEBUGFS_STATS_ADD(dot11ACKFailureCount);
	DEBUGFS_STATS_ADD(dot11RTSFailureCount);
	DEBUGFS_STATS_ADD(dot11FCSErrorCount);
	DEBUGFS_STATS_ADD(dot11RTSSuccessCount);
}

void debugfs_hw_del(struct ieee80211_local *local)
{
	DEBUGFS_DEL(channel);
	DEBUGFS_DEL(frequency);
	DEBUGFS_DEL(radar_detect);
	DEBUGFS_DEL(antenna_sel_tx);
	DEBUGFS_DEL(antenna_sel_rx);
	DEBUGFS_DEL(bridge_packets);
	DEBUGFS_DEL(key_tx_rx_threshold);
	DEBUGFS_DEL(rts_threshold);
	DEBUGFS_DEL(fragmentation_threshold);
	DEBUGFS_DEL(short_retry_limit);
	DEBUGFS_DEL(long_retry_limit);
	DEBUGFS_DEL(total_ps_buffered);
	DEBUGFS_DEL(mode);
	DEBUGFS_DEL(wep_iv);
	DEBUGFS_DEL(tx_power_reduction);
	DEBUGFS_DEL(modes);

	DEBUGFS_STATS_DEL(transmitted_fragment_count);
	DEBUGFS_STATS_DEL(multicast_transmitted_frame_count);
	DEBUGFS_STATS_DEL(failed_count);
	DEBUGFS_STATS_DEL(retry_count);
	DEBUGFS_STATS_DEL(multiple_retry_count);
	DEBUGFS_STATS_DEL(frame_duplicate_count);
	DEBUGFS_STATS_DEL(received_fragment_count);
	DEBUGFS_STATS_DEL(multicast_received_frame_count);
	DEBUGFS_STATS_DEL(transmitted_frame_count);
	DEBUGFS_STATS_DEL(wep_undecryptable_count);
	DEBUGFS_STATS_DEL(num_scans);
#ifdef CONFIG_MAC80211_DEBUG_COUNTERS
	DEBUGFS_STATS_DEL(tx_handlers_drop);
	DEBUGFS_STATS_DEL(tx_handlers_queued);
	DEBUGFS_STATS_DEL(tx_handlers_drop_unencrypted);
	DEBUGFS_STATS_DEL(tx_handlers_drop_fragment);
	DEBUGFS_STATS_DEL(tx_handlers_drop_wep);
	DEBUGFS_STATS_DEL(tx_handlers_drop_not_assoc);
	DEBUGFS_STATS_DEL(tx_handlers_drop_unauth_port);
	DEBUGFS_STATS_DEL(rx_handlers_drop);
	DEBUGFS_STATS_DEL(rx_handlers_queued);
	DEBUGFS_STATS_DEL(rx_handlers_drop_nullfunc);
	DEBUGFS_STATS_DEL(rx_handlers_drop_defrag);
	DEBUGFS_STATS_DEL(rx_handlers_drop_short);
	DEBUGFS_STATS_DEL(rx_handlers_drop_passive_scan);
	DEBUGFS_STATS_DEL(tx_expand_skb_head);
	DEBUGFS_STATS_DEL(tx_expand_skb_head_cloned);
	DEBUGFS_STATS_DEL(rx_expand_skb_head);
	DEBUGFS_STATS_DEL(rx_expand_skb_head2);
	DEBUGFS_STATS_DEL(rx_handlers_fragments);
	DEBUGFS_STATS_DEL(tx_status_drop);
	DEBUGFS_STATS_DEL(wme_tx_queue);
	DEBUGFS_STATS_DEL(wme_rx_queue);
#endif
	DEBUGFS_STATS_DEL(dot11ACKFailureCount);
	DEBUGFS_STATS_DEL(dot11RTSFailureCount);
	DEBUGFS_STATS_DEL(dot11FCSErrorCount);
	DEBUGFS_STATS_DEL(dot11RTSSuccessCount);

	debugfs_remove(local->debugfs.statistics);
	local->debugfs.statistics = NULL;
	debugfs_remove(local->debugfs.stations);
	local->debugfs.stations = NULL;
	debugfs_remove(local->debugfs.keys);
	local->debugfs.keys = NULL;
}
