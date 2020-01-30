#ifndef __QETH_H__
#define __QETH_H__

#include <linux/if.h>
#include <linux/if_arp.h>

#include <linux/if_tr.h>
#include <linux/trdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>

#include <net/ipv6.h>
#include <linux/in6.h>
#include <net/if_inet6.h>
#include <net/addrconf.h>


#include <asm/bitops.h>
#include <asm/debug.h>
#include <asm/qdio.h>
#include <asm/ccwdev.h>
#include <asm/ccwgroup.h>

#include "qeth_mpc.h"

#define VERSION_QETH_H 		"$Revision: 1.113 $"

#ifdef CONFIG_QETH_IPV6
#define QETH_VERSION_IPV6 	":IPv6"
#else
#define QETH_VERSION_IPV6 	""
#endif
#ifdef CONFIG_QETH_VLAN
#define QETH_VERSION_VLAN 	":VLAN"
#else
#define QETH_VERSION_VLAN 	""
#endif

/**
 * Debug Facility stuff
 */
#define QETH_DBF_SETUP_NAME "qeth_setup"
#define QETH_DBF_SETUP_LEN 8
#define QETH_DBF_SETUP_INDEX 3
#define QETH_DBF_SETUP_NR_AREAS 1
#define QETH_DBF_SETUP_LEVEL 3

#define QETH_DBF_MISC_NAME "qeth_misc"
#define QETH_DBF_MISC_LEN 128
#define QETH_DBF_MISC_INDEX 1
#define QETH_DBF_MISC_NR_AREAS 1
#define QETH_DBF_MISC_LEVEL 2

#define QETH_DBF_DATA_NAME "qeth_data"
#define QETH_DBF_DATA_LEN 96
#define QETH_DBF_DATA_INDEX 3
#define QETH_DBF_DATA_NR_AREAS 1
#define QETH_DBF_DATA_LEVEL 2

#define QETH_DBF_CONTROL_NAME "qeth_control"
#define QETH_DBF_CONTROL_LEN 256
#define QETH_DBF_CONTROL_INDEX 3
#define QETH_DBF_CONTROL_NR_AREAS 2
#define QETH_DBF_CONTROL_LEVEL 2

#define QETH_DBF_TRACE_NAME "qeth_trace"
#define QETH_DBF_TRACE_LEN 8
#define QETH_DBF_TRACE_INDEX 2
#define QETH_DBF_TRACE_NR_AREAS 2
#define QETH_DBF_TRACE_LEVEL 3

#define QETH_DBF_SENSE_NAME "qeth_sense"
#define QETH_DBF_SENSE_LEN 64
#define QETH_DBF_SENSE_INDEX 1
#define QETH_DBF_SENSE_NR_AREAS 1
#define QETH_DBF_SENSE_LEVEL 2

#define QETH_DBF_QERR_NAME "qeth_qerr"
#define QETH_DBF_QERR_LEN 8
#define QETH_DBF_QERR_INDEX 1
#define QETH_DBF_QERR_NR_AREAS 2
#define QETH_DBF_QERR_LEVEL 2

#define QETH_DBF_TEXT(name,level,text) \
	do { \
		debug_text_event(qeth_dbf_##name,level,text); \
	} while (0)

#define QETH_DBF_HEX(name,level,addr,len) \
	do { \
		debug_event(qeth_dbf_##name,level,(void*)(addr),len); \
	} while (0)

extern DEFINE_PER_CPU(char[256], qeth_dbf_txt_buf);

#define QETH_DBF_TEXT_(name,level,text...)				\
	do {								\
		char* dbf_txt_buf = get_cpu_var(qeth_dbf_txt_buf);	\
		sprintf(dbf_txt_buf, text);			  	\
		debug_text_event(qeth_dbf_##name,level,dbf_txt_buf);	\
		put_cpu_var(qeth_dbf_txt_buf);				\
	} while (0)

#define QETH_DBF_SPRINTF(name,level,text...) \
	do { \
		debug_sprintf_event(qeth_dbf_trace, level, ##text ); \
		debug_sprintf_event(qeth_dbf_trace, level, text ); \
	} while (0)

/**
 * some more debug stuff
 */
#define PRINTK_HEADER 	"qeth: "

#define HEXDUMP16(importance,header,ptr) \
PRINT_##importance(header "%02x %02x %02x %02x  %02x %02x %02x %02x  " \
		   "%02x %02x %02x %02x  %02x %02x %02x %02x\n", \
		   *(((char*)ptr)),*(((char*)ptr)+1),*(((char*)ptr)+2), \
		   *(((char*)ptr)+3),*(((char*)ptr)+4),*(((char*)ptr)+5), \
		   *(((char*)ptr)+6),*(((char*)ptr)+7),*(((char*)ptr)+8), \
		   *(((char*)ptr)+9),*(((char*)ptr)+10),*(((char*)ptr)+11), \
		   *(((char*)ptr)+12),*(((char*)ptr)+13), \
		   *(((char*)ptr)+14),*(((char*)ptr)+15)); \
PRINT_##importance(header "%02x %02x %02x %02x  %02x %02x %02x %02x  " \
		   "%02x %02x %02x %02x  %02x %02x %02x %02x\n", \
		   *(((char*)ptr)+16),*(((char*)ptr)+17), \
		   *(((char*)ptr)+18),*(((char*)ptr)+19), \
		   *(((char*)ptr)+20),*(((char*)ptr)+21), \
		   *(((char*)ptr)+22),*(((char*)ptr)+23), \
		   *(((char*)ptr)+24),*(((char*)ptr)+25), \
		   *(((char*)ptr)+26),*(((char*)ptr)+27), \
		   *(((char*)ptr)+28),*(((char*)ptr)+29), \
		   *(((char*)ptr)+30),*(((char*)ptr)+31));

static inline void
qeth_hex_dump(unsigned char *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (i && !(i % 16))
			printk("\n");
		printk("%02x ", *(buf + i));
	}
	printk("\n");
}

#define SENSE_COMMAND_REJECT_BYTE 0
#define SENSE_COMMAND_REJECT_FLAG 0x80
#define SENSE_RESETTING_EVENT_BYTE 1
#define SENSE_RESETTING_EVENT_FLAG 0x80

#define atomic_swap(a,b) xchg((int *)a.counter, b)

/*
 * Common IO related definitions
 */
extern struct device *qeth_root_dev;
extern struct ccw_driver qeth_ccw_driver;
extern struct ccwgroup_driver qeth_ccwgroup_driver;

#define CARD_RDEV(card) card->read.ccwdev
#define CARD_WDEV(card) card->write.ccwdev
#define CARD_DDEV(card) card->data.ccwdev
#define CARD_BUS_ID(card) card->gdev->dev.bus_id
#define CARD_RDEV_ID(card) card->read.ccwdev->dev.bus_id
#define CARD_WDEV_ID(card) card->write.ccwdev->dev.bus_id
#define CARD_DDEV_ID(card) card->data.ccwdev->dev.bus_id
#define CHANNEL_ID(channel) channel->ccwdev->dev.bus_id

#define CARD_FROM_CDEV(cdev) (struct qeth_card *) \
		((struct ccwgroup_device *)cdev->dev.driver_data)\
		->dev.driver_data;

/**
 * card stuff
 */
#ifdef CONFIG_QETH_PERF_STATS
struct qeth_perf_stats {
	unsigned int bufs_rec;
	unsigned int bufs_sent;

	unsigned int skbs_sent_pack;
	unsigned int bufs_sent_pack;

	unsigned int sc_dp_p;
	unsigned int sc_p_dp;
	/* qdio_input_handler: number of times called, time spent in */
	__u64 inbound_start_time;
	unsigned int inbound_cnt;
	unsigned int inbound_time;
	/* qeth_send_packet: number of times called, time spent in */
	__u64 outbound_start_time;
	unsigned int outbound_cnt;
	unsigned int outbound_time;
	/* qdio_output_handler: number of times called, time spent in */
	__u64 outbound_handler_start_time;
	unsigned int outbound_handler_cnt;
	unsigned int outbound_handler_time;
	/* number of calls to and time spent in do_QDIO for inbound queue */
	__u64 inbound_do_qdio_start_time;
	unsigned int inbound_do_qdio_cnt;
	unsigned int inbound_do_qdio_time;
	/* number of calls to and time spent in do_QDIO for outbound queues */
	__u64 outbound_do_qdio_start_time;
	unsigned int outbound_do_qdio_cnt;
	unsigned int outbound_do_qdio_time;
};
#endif /* CONFIG_QETH_PERF_STATS */

/* Routing stuff */
struct qeth_routing_info {
	enum qeth_routing_types type;
};

/* IPA stuff */
struct qeth_ipa_info {
	__u32 supported_funcs;
	__u32 enabled_funcs;
};

static inline int
qeth_is_ipa_supported(struct qeth_ipa_info *ipa, enum qeth_ipa_funcs func)
{
	return (ipa->supported_funcs & func);
}

static inline int
qeth_is_ipa_enabled(struct qeth_ipa_info *ipa, enum qeth_ipa_funcs func)
{
	return (ipa->supported_funcs & ipa->enabled_funcs & func);
}

#define qeth_adp_supported(c,f) \
	qeth_is_ipa_supported(&c->options.adp, f)
#define qeth_adp_enabled(c,f) \
	qeth_is_ipa_enabled(&c->options.adp, f)
#define qeth_is_supported(c,f) \
	qeth_is_ipa_supported(&c->options.ipa4, f)
#define qeth_is_enabled(c,f) \
	qeth_is_ipa_enabled(&c->options.ipa4, f)
#ifdef CONFIG_QETH_IPV6
#define qeth_is_supported6(c,f) \
	qeth_is_ipa_supported(&c->options.ipa6, f)
#define qeth_is_enabled6(c,f) \
	qeth_is_ipa_enabled(&c->options.ipa6, f)
#else /* CONFIG_QETH_IPV6 */
#define qeth_is_supported6(c,f) 0
#define qeth_is_enabled6(c,f) 0
#endif /* CONFIG_QETH_IPV6 */
#define qeth_is_ipafunc_supported(c,prot,f) \
	 (prot==QETH_PROT_IPV6)? qeth_is_supported6(c,f):qeth_is_supported(c,f)
#define qeth_is_ipafunc_enabled(c,prot,f) \
	 (prot==QETH_PROT_IPV6)? qeth_is_enabled6(c,f):qeth_is_enabled(c,f)


#define QETH_IDX_FUNC_LEVEL_OSAE_ENA_IPAT 0x0101
#define QETH_IDX_FUNC_LEVEL_OSAE_DIS_IPAT 0x0101
#define QETH_IDX_FUNC_LEVEL_IQD_ENA_IPAT 0x4108
#define QETH_IDX_FUNC_LEVEL_IQD_DIS_IPAT 0x5108

#define QETH_MODELLIST_ARRAY \
	{{0x1731,0x01,0x1732,0x01,QETH_CARD_TYPE_OSAE,1, \
	QETH_IDX_FUNC_LEVEL_OSAE_ENA_IPAT, \
	QETH_IDX_FUNC_LEVEL_OSAE_DIS_IPAT, \
	QETH_MAX_QUEUES,0}, \
	{0x1731,0x05,0x1732,0x05,QETH_CARD_TYPE_IQD,0, \
	QETH_IDX_FUNC_LEVEL_IQD_ENA_IPAT, \
	QETH_IDX_FUNC_LEVEL_IQD_DIS_IPAT, \
	QETH_MAX_QUEUES,0x103}, \
	{0,0,0,0,0,0,0,0,0}}

#define QETH_REAL_CARD		1
#define QETH_VLAN_CARD		2
#define QETH_BUFSIZE	 	4096

/**
 * some more defs
 */
#define IF_NAME_LEN	 	16
#define QETH_TX_TIMEOUT		100 * HZ
#define QETH_HEADER_SIZE	32
#define MAX_PORTNO 		15
#define QETH_FAKE_LL_LEN 	ETH_HLEN
#define QETH_FAKE_LL_V6_ADDR_POS 24

/*IPv6 address autoconfiguration stuff*/
#define UNIQUE_ID_IF_CREATE_ADDR_FAILED 0xfffe
#define UNIQUE_ID_NOT_BY_CARD 		0x10000

/*****************************************************************************/
/* QDIO queue and buffer handling                                            */
/*****************************************************************************/
#define QETH_MAX_QUEUES 4
#define QETH_IN_BUF_SIZE_DEFAULT 65536
#define QETH_IN_BUF_COUNT_DEFAULT 16
#define QETH_IN_BUF_COUNT_MIN 8
#define QETH_IN_BUF_COUNT_MAX 128
#define QETH_MAX_BUFFER_ELEMENTS(card) ((card)->qdio.in_buf_size >> 12)
#define QETH_IN_BUF_REQUEUE_THRESHOLD(card) \
		((card)->qdio.in_buf_pool.buf_count / 2)

/* buffers we have to be behind before we get a PCI */
#define QETH_PCI_THRESHOLD_A(card) ((card)->qdio.in_buf_pool.buf_count+1)
/*enqueued free buffers left before we get a PCI*/
#define QETH_PCI_THRESHOLD_B(card) 0
/*not used unless the microcode gets patched*/
#define QETH_PCI_TIMER_VALUE(card) 3

#define QETH_MIN_INPUT_THRESHOLD 1
#define QETH_MAX_INPUT_THRESHOLD 500
#define QETH_MIN_OUTPUT_THRESHOLD 1
#define QETH_MAX_OUTPUT_THRESHOLD 300

/* priority queing */
#define QETH_PRIOQ_DEFAULT QETH_NO_PRIO_QUEUEING
#define QETH_DEFAULT_QUEUE    2
#define QETH_NO_PRIO_QUEUEING 0
#define QETH_PRIO_Q_ING_PREC  1
#define QETH_PRIO_Q_ING_TOS   2
#define IP_TOS_LOWDELAY 0x10
#define IP_TOS_HIGHTHROUGHPUT 0x08
#define IP_TOS_HIGHRELIABILITY 0x04
#define IP_TOS_NOTIMPORTANT 0x02

/* Packing */
#define QETH_LOW_WATERMARK_PACK  2
#define QETH_HIGH_WATERMARK_PACK 5
#define QETH_WATERMARK_PACK_FUZZ 1

#define QETH_IP_HEADER_SIZE 40
/* VLAN defines */
#define QETH_EXT_HDR_VLAN_FRAME        0x01
#define QETH_EXT_HDR_TOKEN_ID          0x02
#define QETH_EXT_HDR_INCLUDE_VLAN_TAG  0x04

struct qeth_hdr {
	__u8  id;
	__u8  flags;
	__u16 inbound_checksum;
	__u32 token;
	__u16 length;
	__u8  vlan_prio;
	__u8  ext_flags;
	__u16 vlan_id;
	__u16 frame_offset;
	__u8  dest_addr[16];
} __attribute__ ((packed));

/* flags for qeth_hdr.flags */
#define QETH_HDR_PASSTHRU 0x10
#define QETH_HDR_IPV6     0x80
#define QETH_HDR_CAST_MASK 0x07
enum qeth_cast_flags {
	QETH_CAST_UNICAST   = 0x06,
	QETH_CAST_MULTICAST = 0x04,
	QETH_CAST_BROADCAST = 0x05,
	QETH_CAST_ANYCAST   = 0x07,
	QETH_CAST_NOCAST    = 0x00,
};

/* flags for qeth_hdr.ext_flags */
#define QETH_HDR_EXT_VLAN_FRAME      0x01
#define QETH_HDR_EXT_CSUM_HDR_REQ    0x10
#define QETH_HDR_EXT_CSUM_TRANSP_REQ 0x20
#define QETH_HDR_EXT_SRC_MAC_ADDR    0x08

static inline int
qeth_is_last_sbale(struct qdio_buffer_element *sbale)
{
	return (sbale->flags & SBAL_FLAGS_LAST_ENTRY);
}

enum qeth_qdio_buffer_states {
	/*
	 * inbound: read out by driver; owned by hardware in order to be filled
	 * outbound: owned by driver in order to be filled
	 */
	QETH_QDIO_BUF_EMPTY,
	/*
	 * inbound: filled by hardware; owned by driver in order to be read out
	 * outbound: filled by driver; owned by hardware in order to be sent
	 */
	QETH_QDIO_BUF_PRIMED,
};

enum qeth_qdio_info_states {
	QETH_QDIO_UNINITIALIZED,
	QETH_QDIO_ALLOCATED,
	QETH_QDIO_ESTABLISHED,
};

struct qeth_buffer_pool_entry {
	struct list_head list;
	struct list_head init_list;
	void *elements[QDIO_MAX_ELEMENTS_PER_BUFFER];
};

struct qeth_qdio_buffer_pool {
	struct list_head entry_list;
	int buf_count;
};

struct qeth_qdio_buffer {
	struct qdio_buffer *buffer;
	volatile enum qeth_qdio_buffer_states state;
	/* the buffer pool entry currently associated to this buffer */
	struct qeth_buffer_pool_entry *pool_entry;
};

struct qeth_qdio_q {
	struct qdio_buffer qdio_bufs[QDIO_MAX_BUFFERS_PER_Q];
	struct qeth_qdio_buffer bufs[QDIO_MAX_BUFFERS_PER_Q];
	/*
	 * buf_to_init means "buffer must be initialized by driver and must
	 * be made available for hardware" -> state is set to EMPTY
	 */
	volatile int next_buf_to_init;
} __attribute__ ((aligned(256)));

struct qeth_qdio_out_buffer {
	struct qdio_buffer *buffer;
	atomic_t state;
	volatile int next_element_to_fill;
	struct sk_buff_head skb_list;
};

struct qeth_card;

enum qeth_out_q_states {
       QETH_OUT_Q_UNLOCKED,
       QETH_OUT_Q_LOCKED,
       QETH_OUT_Q_LOCKED_FLUSH,
};

struct qeth_qdio_out_q {
	struct qdio_buffer qdio_bufs[QDIO_MAX_BUFFERS_PER_Q];
	struct qeth_qdio_out_buffer bufs[QDIO_MAX_BUFFERS_PER_Q];
	int queue_no;
	struct qeth_card *card;
	atomic_t state;
	volatile int do_pack;
	/*
	 * index of buffer to be filled by driver; state EMPTY or PACKING
	 */
	volatile int next_buf_to_fill;
	/*
	 * number of buffers that are currently filled (PRIMED)
	 * -> these buffers are hardware-owned
	 */
	atomic_t used_buffers;
	/* indicates whether PCI flag must be set (or if one is outstanding) */
	atomic_t set_pci_flags_count;
} __attribute__ ((aligned(256)));

struct qeth_qdio_info {
	volatile enum qeth_qdio_info_states state;
	/* input */
	struct qeth_qdio_q *in_q;
	struct qeth_qdio_buffer_pool in_buf_pool;
	struct qeth_qdio_buffer_pool init_pool;
	int in_buf_size;

	/* output */
	int no_out_queues;
	struct qeth_qdio_out_q **out_qs;

	/* priority queueing */
	int do_prio_queueing;
	int default_out_queue;
};

enum qeth_send_errors {
	QETH_SEND_ERROR_NONE,
	QETH_SEND_ERROR_LINK_FAILURE,
	QETH_SEND_ERROR_RETRY,
	QETH_SEND_ERROR_KICK_IT,
};

#define QETH_ETH_MAC_V4      0x0100 /* like v4 */
#define QETH_ETH_MAC_V6      0x3333 /* like v6 */
/* tr mc mac is longer, but that will be enough to detect mc frames */
#define QETH_TR_MAC_NC       0xc000 /* non-canonical */
#define QETH_TR_MAC_C        0x0300 /* canonical */

#define DEFAULT_ADD_HHLEN 0
#define MAX_ADD_HHLEN 1024

/**
 * buffer stuff for read channel
 */
#define QETH_CMD_BUFFER_NO	8

/**
 *  channel state machine
 */
enum qeth_channel_states {
	CH_STATE_UP,
	CH_STATE_DOWN,
	CH_STATE_ACTIVATING,
	CH_STATE_HALTED,
	CH_STATE_STOPPED,
};
/**
 * card state machine
 */
enum qeth_card_states {
	CARD_STATE_DOWN,
	CARD_STATE_HARDSETUP,
	CARD_STATE_SOFTSETUP,
	CARD_STATE_UP,
	CARD_STATE_RECOVER,
};

/**
 * Protocol versions
 */
enum qeth_prot_versions {
	QETH_PROT_SNA  = 0x0001,
	QETH_PROT_IPV4 = 0x0004,
	QETH_PROT_IPV6 = 0x0006,
};

enum qeth_ip_types {
	QETH_IP_TYPE_NORMAL,
	QETH_IP_TYPE_VIPA,
	QETH_IP_TYPE_RXIP,
};

enum qeth_cmd_buffer_state {
	BUF_STATE_FREE,
	BUF_STATE_LOCKED,
	BUF_STATE_PROCESSED,
};
/**
 * IP address and multicast list
 */
struct qeth_ipaddr {
	struct list_head entry;
	enum qeth_ip_types type;
	enum qeth_ipa_setdelip_flags set_flags;
	enum qeth_ipa_setdelip_flags del_flags;
	int is_multicast;
	volatile int users;
	enum qeth_prot_versions proto;
	unsigned char mac[OSA_ADDR_LEN];
	union {
		struct {
			unsigned int addr;
			unsigned int mask;
		} a4;
		struct {
			struct in6_addr addr;
			unsigned int pfxlen;
		} a6;
	} u;
};

struct qeth_ipato_entry {
	struct list_head entry;
	enum qeth_prot_versions proto;
	char addr[16];
	int mask_bits;
};

struct qeth_ipato {
	int enabled;
	int invert4;
	int invert6;
	struct list_head entries;
};

struct qeth_channel;

struct qeth_cmd_buffer {
	enum qeth_cmd_buffer_state state;
	struct qeth_channel *channel;
	unsigned char *data;
	int rc;
	void (*callback) (struct qeth_channel *, struct qeth_cmd_buffer *);
};


/**
 * definition of a qeth channel, used for read and write
 */
struct qeth_channel {
	enum qeth_channel_states state;
	struct ccw1 ccw;
	spinlock_t iob_lock;
	wait_queue_head_t wait_q;
	struct tasklet_struct irq_tasklet;
	struct ccw_device *ccwdev;
/*command buffer for control data*/
	struct qeth_cmd_buffer iob[QETH_CMD_BUFFER_NO];
	atomic_t irq_pending;
	volatile int io_buf_no;
	volatile int buf_no;
};

/**
 *  OSA card related definitions
 */
struct qeth_token {
	__u32 issuer_rm_w;
	__u32 issuer_rm_r;
	__u32 cm_filter_w;
	__u32 cm_filter_r;
	__u32 cm_connection_w;
	__u32 cm_connection_r;
	__u32 ulp_filter_w;
	__u32 ulp_filter_r;
	__u32 ulp_connection_w;
	__u32 ulp_connection_r;
};

struct qeth_seqno {
	__u32 trans_hdr;
	__u32 pdu_hdr;
	__u32 pdu_hdr_ack;
	__u16 ipa;
};

struct qeth_reply {
	struct list_head list;
	wait_queue_head_t wait_q;
	int (*callback)(struct qeth_card *,struct qeth_reply *,unsigned long);
 	u32 seqno;
	unsigned long offset;
	int received;
	int rc;
	void *param;
	struct qeth_card *card;
	atomic_t refcnt;
};

#define QETH_BROADCAST_WITH_ECHO    1
#define QETH_BROADCAST_WITHOUT_ECHO 2

struct qeth_card_info {
	char if_name[IF_NAME_LEN];
	unsigned short unit_addr2;
	unsigned short cula;
	unsigned short chpid;
	__u16 func_level;
	char mcl_level[QETH_MCL_LENGTH + 1];
	int guestlan;
	int portname_required;
	int portno;
	char portname[9];
	enum qeth_card_types type;
	enum qeth_link_types link_type;
	int is_multicast_different;
	int initial_mtu;
	int max_mtu;
	int broadcast_capable;
	int unique_id;
	__u32 csum_mask;
};

struct qeth_card_options {
	struct qeth_routing_info route4;
	struct qeth_ipa_info ipa4;
	struct qeth_ipa_info adp; /*Adapter parameters*/
#ifdef CONFIG_QETH_IPV6
	struct qeth_routing_info route6;
	struct qeth_ipa_info ipa6;
#endif /* QETH_IPV6 */
	enum qeth_checksum_types checksum_type;
	int broadcast_mode;
	int macaddr_mode;
	int fake_broadcast;
	int add_hhlen;
	int fake_ll;
};

/*
 * thread bits for qeth_card thread masks
 */
enum qeth_threads {
	QETH_SET_IP_THREAD  = 1,
	QETH_SET_MC_THREAD  = 2,
	QETH_RECOVER_THREAD = 4,
};

struct qeth_card {
	struct list_head list;
	enum qeth_card_states state;
	int lan_online;
	spinlock_t lock;
/*hardware and sysfs stuff*/
	struct ccwgroup_device *gdev;
	struct qeth_channel read;
	struct qeth_channel write;
	struct qeth_channel data;

	struct net_device *dev;
	struct net_device_stats stats;

	struct qeth_card_info info;
	struct qeth_token token;
	struct qeth_seqno seqno;
	struct qeth_card_options options;

	wait_queue_head_t wait_q;
#ifdef CONFIG_QETH_VLAN
	spinlock_t vlanlock;
	struct vlan_group *vlangrp;
#endif
	struct work_struct kernel_thread_starter;
	spinlock_t thread_mask_lock;
	volatile unsigned long thread_start_mask;
	volatile unsigned long thread_allowed_mask;
	volatile unsigned long thread_running_mask;
	spinlock_t ip_lock;
	struct list_head ip_list;
	struct list_head ip_tbd_list;
	struct qeth_ipato ipato;
	struct list_head cmd_waiter_list;
	/* QDIO buffer handling */
	struct qeth_qdio_info qdio;
#ifdef CONFIG_QETH_PERF_STATS
	struct qeth_perf_stats perf_stats;
#endif /* CONFIG_QETH_PERF_STATS */
	int use_hard_stop;
};

struct qeth_card_list_struct {
	struct list_head list;
	rwlock_t rwlock;
};

extern struct qeth_card_list_struct qeth_card_list;

/*notifier list */
struct qeth_notify_list_struct {
	struct list_head list;
	struct task_struct *task;
	int signum;
};
extern spinlock_t qeth_notify_lock;
extern struct list_head qeth_notify_list;

/*some helper functions*/

inline static __u8
qeth_get_ipa_adp_type(enum qeth_link_types link_type)
{
	switch (link_type) {
	case QETH_LINK_TYPE_HSTR:
		return 2;
	default:
		return 1;
	}
}

inline static int
qeth_get_hlen(__u8 link_type)
{
#ifdef CONFIG_QETH_IPV6
	switch (link_type) {
	case QETH_LINK_TYPE_HSTR:
	case QETH_LINK_TYPE_LANE_TR:
		return sizeof(struct qeth_hdr) + TR_HLEN;
	default:
#ifdef CONFIG_QETH_VLAN
		return sizeof(struct qeth_hdr) + VLAN_ETH_HLEN;
#else
		return sizeof(struct qeth_hdr) + ETH_HLEN;
#endif
	}
#else  /* CONFIG_QETH_IPV6 */
#ifdef CONFIG_QETH_VLAN
	return sizeof(struct qeth_hdr) + VLAN_HLEN;
#else
	return sizeof(struct qeth_hdr);
#endif
#endif /* CONFIG_QETH_IPV6 */
}

inline static unsigned short
qeth_get_netdev_flags(int cardtype)
{
	switch (cardtype) {
	case QETH_CARD_TYPE_IQD:
		return IFF_NOARP;
#ifdef CONFIG_QETH_IPV6
	default:
		return 0;
#else
	default:
		return IFF_NOARP;
#endif
	}
}

inline static int
qeth_get_initial_mtu_for_card(struct qeth_card * card)
{
	switch (card->info.type) {
	case QETH_CARD_TYPE_UNKNOWN:
		return 1500;
	case QETH_CARD_TYPE_IQD:
		return card->info.max_mtu;
	case QETH_CARD_TYPE_OSAE:
		switch (card->info.link_type) {
		case QETH_LINK_TYPE_HSTR:
		case QETH_LINK_TYPE_LANE_TR:
			return 2000;
		default:
			return 1492;
		}
	default:
		return 1500;
	}
}

inline static int
qeth_get_max_mtu_for_card(int cardtype)
{
	switch (cardtype) {
	case QETH_CARD_TYPE_UNKNOWN:
		return 61440;
	case QETH_CARD_TYPE_OSAE:
		return 61440;
	case QETH_CARD_TYPE_IQD:
		return 57344;
	default:
		return 1500;
	}
}

inline static int
qeth_get_mtu_out_of_mpc(int cardtype)
{
	switch (cardtype) {
	case QETH_CARD_TYPE_IQD:
		return 1;
	default:
		return 0;
	}
}

inline static int
qeth_get_mtu_outof_framesize(int framesize)
{
	switch (framesize) {
	case 0x4000:
		return 8192;
	case 0x6000:
		return 16384;
	case 0xa000:
		return 32768;
	case 0xffff:
		return 57344;
	default:
		return 0;
	}
}

inline static int
qeth_mtu_is_valid(struct qeth_card * card, int mtu)
{
	switch (card->info.type) {
	case QETH_CARD_TYPE_OSAE:
		return ((mtu >= 576) && (mtu <= 61440));
	case QETH_CARD_TYPE_IQD:
		return ((mtu >= 576) &&
			(mtu <= card->info.max_mtu + 4096 - 32));
	case QETH_CARD_TYPE_UNKNOWN:
	default:
		return 1;
	}
}

inline static int
qeth_get_arphdr_type(int cardtype, int linktype)
{
	switch (cardtype) {
	case QETH_CARD_TYPE_OSAE:
		switch (linktype) {
		case QETH_LINK_TYPE_LANE_TR:
		case QETH_LINK_TYPE_HSTR:
			return ARPHRD_IEEE802_TR;
		default:
			return ARPHRD_ETHER;
		}
	case QETH_CARD_TYPE_IQD:
	default:
		return ARPHRD_ETHER;
	}
}

#ifdef CONFIG_QETH_PERF_STATS
inline static int
qeth_get_micros(void)
{
	return (int) (get_clock() >> 12);
}
#endif

static inline int
qeth_get_qdio_q_format(struct qeth_card *card)
{
	switch (card->info.type) {
	case QETH_CARD_TYPE_IQD:
		return 2;
	default:
		return 0;
	}
}

static inline void
qeth_ipaddr4_to_string(const __u8 *addr, char *buf)
{
	sprintf(buf, "%i.%i.%i.%i", addr[0], addr[1], addr[2], addr[3]);
}

static inline int
qeth_string_to_ipaddr4(const char *buf, __u8 *addr)
{
	const char *start, *end;
	char abuf[4];
	char *tmp;
	int len;
	int i;

	start = buf;
	for (i = 0; i < 3; i++) {
		if (!(end = strchr(start, '.')))
			return -EINVAL;
		len = end - start;
		memset(abuf, 0, 4);
		strncpy(abuf, start, len);
		addr[i] = simple_strtoul(abuf, &tmp, 10);
		start = end + 1;
	}
	memset(abuf, 0, 4);
	strcpy(abuf, start);
	addr[3] = simple_strtoul(abuf, &tmp, 10);
	return 0;
}

static inline void
qeth_ipaddr6_to_string(const __u8 *addr, char *buf)
{
	sprintf(buf, "%02x%02x:%02x%02x:%02x%02x:%02x%02x"
		     ":%02x%02x:%02x%02x:%02x%02x:%02x%02x",
		     addr[0], addr[1], addr[2], addr[3],
		     addr[4], addr[5], addr[6], addr[7],
		     addr[8], addr[9], addr[10], addr[11],
		     addr[12], addr[13], addr[14], addr[15]);
}

static inline int
qeth_string_to_ipaddr6(const char *buf, __u8 *addr)
{
	const char *start, *end;
	u16 *tmp_addr;
	char abuf[5];
	char *tmp;
	int len;
	int i;

	tmp_addr = (u16 *)addr;
	start = buf;
	for (i = 0; i < 7; i++) {
		if (!(end = strchr(start, ':')))
			return -EINVAL;
		len = end - start;
		memset(abuf, 0, 5);
		strncpy(abuf, start, len);
		tmp_addr[i] = simple_strtoul(abuf, &tmp, 16);
		start = end + 1;
	}
	memset(abuf, 0, 5);
	strcpy(abuf, start);
	tmp_addr[7] = simple_strtoul(abuf, &tmp, 16);
	return 0;
}

static inline void
qeth_ipaddr_to_string(enum qeth_prot_versions proto, const __u8 *addr,
		      char *buf)
{
	if (proto == QETH_PROT_IPV4)
		return qeth_ipaddr4_to_string(addr, buf);
	else if (proto == QETH_PROT_IPV6)
		return qeth_ipaddr6_to_string(addr, buf);
}

static inline int
qeth_string_to_ipaddr(const char *buf, enum qeth_prot_versions proto,
		      __u8 *addr)
{
	if (proto == QETH_PROT_IPV4)
		return qeth_string_to_ipaddr4(buf, addr);
	else if (proto == QETH_PROT_IPV6)
		return qeth_string_to_ipaddr6(buf, addr);
	else
		return -EINVAL;
}

extern int
qeth_setrouting_v4(struct qeth_card *);
extern int
qeth_setrouting_v6(struct qeth_card *);

extern int
qeth_add_ipato_entry(struct qeth_card *, struct qeth_ipato_entry *);

extern void
qeth_del_ipato_entry(struct qeth_card *, enum qeth_prot_versions, u8 *, int);

extern int
qeth_add_vipa(struct qeth_card *, enum qeth_prot_versions, const u8 *);

extern void
qeth_del_vipa(struct qeth_card *, enum qeth_prot_versions, const u8 *);

extern int
qeth_add_rxip(struct qeth_card *, enum qeth_prot_versions, const u8 *);

extern void
qeth_del_rxip(struct qeth_card *, enum qeth_prot_versions, const u8 *);

extern int
qeth_notifier_register(struct task_struct *, int );

extern int
qeth_notifier_unregister(struct task_struct * );

extern void
qeth_schedule_recovery(struct qeth_card *);

extern int
qeth_realloc_buffer_pool(struct qeth_card *, int);
#endif /* __QETH_H__ */
