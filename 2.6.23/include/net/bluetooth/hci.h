/* 
   BlueZ - Bluetooth protocol stack for Linux
   Copyright (C) 2000-2001 Qualcomm Incorporated

   Written 2000,2001 by Maxim Krasnyansky <maxk@qualcomm.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 2 as
   published by the Free Software Foundation;

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS.
   IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) AND AUTHOR(S) BE LIABLE FOR ANY
   CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES 
   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN 
   ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF 
   OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

   ALL LIABILITY, INCLUDING LIABILITY FOR INFRINGEMENT OF ANY PATENTS, 
   COPYRIGHTS, TRADEMARKS OR OTHER RIGHTS, RELATING TO USE OF THIS 
   SOFTWARE IS DISCLAIMED.
*/

#ifndef __HCI_H
#define __HCI_H

#define HCI_MAX_ACL_SIZE	1024
#define HCI_MAX_SCO_SIZE	255
#define HCI_MAX_EVENT_SIZE	260
#define HCI_MAX_FRAME_SIZE	(HCI_MAX_ACL_SIZE + 4)

/* HCI dev events */
#define HCI_DEV_REG			1
#define HCI_DEV_UNREG			2
#define HCI_DEV_UP			3
#define HCI_DEV_DOWN			4
#define HCI_DEV_SUSPEND			5
#define HCI_DEV_RESUME			6

/* HCI notify events */
#define HCI_NOTIFY_CONN_ADD		1
#define HCI_NOTIFY_CONN_DEL		2
#define HCI_NOTIFY_VOICE_SETTING	3

/* HCI device types */
#define HCI_VIRTUAL	0
#define HCI_USB		1
#define HCI_PCCARD	2
#define HCI_UART	3
#define HCI_RS232	4
#define HCI_PCI		5
#define HCI_SDIO	6

/* HCI device quirks */
enum {
	HCI_QUIRK_RESET_ON_INIT,
	HCI_QUIRK_RAW_DEVICE,
	HCI_QUIRK_FIXUP_BUFFER_SIZE
};

/* HCI device flags */
enum {
	HCI_UP,
	HCI_INIT,
	HCI_RUNNING,

	HCI_PSCAN,
	HCI_ISCAN,
	HCI_AUTH,
	HCI_ENCRYPT,
	HCI_INQUIRY,

	HCI_RAW,

	HCI_SECMGR
};

/* HCI ioctl defines */
#define HCIDEVUP	_IOW('H', 201, int)
#define HCIDEVDOWN	_IOW('H', 202, int)
#define HCIDEVRESET	_IOW('H', 203, int)
#define HCIDEVRESTAT	_IOW('H', 204, int)

#define HCIGETDEVLIST	_IOR('H', 210, int)
#define HCIGETDEVINFO	_IOR('H', 211, int)
#define HCIGETCONNLIST	_IOR('H', 212, int)
#define HCIGETCONNINFO	_IOR('H', 213, int)

#define HCISETRAW	_IOW('H', 220, int)
#define HCISETSCAN	_IOW('H', 221, int)
#define HCISETAUTH	_IOW('H', 222, int)
#define HCISETENCRYPT	_IOW('H', 223, int)
#define HCISETPTYPE	_IOW('H', 224, int)
#define HCISETLINKPOL	_IOW('H', 225, int)
#define HCISETLINKMODE	_IOW('H', 226, int)
#define HCISETACLMTU	_IOW('H', 227, int)
#define HCISETSCOMTU	_IOW('H', 228, int)

#define HCISETSECMGR	_IOW('H', 230, int)

#define HCIINQUIRY	_IOR('H', 240, int)

/* HCI timeouts */
#define HCI_CONNECT_TIMEOUT	(40000)	/* 40 seconds */
#define HCI_DISCONN_TIMEOUT	(2000)	/* 2 seconds */
#define HCI_IDLE_TIMEOUT	(6000)	/* 6 seconds */
#define HCI_INIT_TIMEOUT	(10000)	/* 10 seconds */

/* HCI data types */
#define HCI_COMMAND_PKT		0x01
#define HCI_ACLDATA_PKT		0x02
#define HCI_SCODATA_PKT		0x03
#define HCI_EVENT_PKT		0x04
#define HCI_VENDOR_PKT		0xff

/* HCI packet types */
#define HCI_DM1		0x0008
#define HCI_DM3		0x0400
#define HCI_DM5		0x4000
#define HCI_DH1		0x0010
#define HCI_DH3		0x0800
#define HCI_DH5		0x8000

#define HCI_HV1		0x0020
#define HCI_HV2		0x0040
#define HCI_HV3		0x0080

#define SCO_PTYPE_MASK	(HCI_HV1 | HCI_HV2 | HCI_HV3)
#define ACL_PTYPE_MASK	(~SCO_PTYPE_MASK)

/* eSCO packet types */
#define ESCO_HV1	0x0001
#define ESCO_HV2	0x0002
#define ESCO_HV3	0x0004
#define ESCO_EV3	0x0008
#define ESCO_EV4	0x0010
#define ESCO_EV5	0x0020

/* ACL flags */
#define ACL_CONT		0x01
#define ACL_START		0x02
#define ACL_ACTIVE_BCAST	0x04
#define ACL_PICO_BCAST		0x08

/* Baseband links */
#define SCO_LINK	0x00
#define ACL_LINK	0x01
#define ESCO_LINK	0x02

/* LMP features */
#define LMP_3SLOT	0x01
#define LMP_5SLOT	0x02
#define LMP_ENCRYPT	0x04
#define LMP_SOFFSET	0x08
#define LMP_TACCURACY	0x10
#define LMP_RSWITCH	0x20
#define LMP_HOLD	0x40
#define LMP_SNIFF	0x80

#define LMP_PARK	0x01
#define LMP_RSSI	0x02
#define LMP_QUALITY	0x04
#define LMP_SCO		0x08
#define LMP_HV2		0x10
#define LMP_HV3		0x20
#define LMP_ULAW	0x40
#define LMP_ALAW	0x80

#define LMP_CVSD	0x01
#define LMP_PSCHEME	0x02
#define LMP_PCONTROL	0x04

#define LMP_ESCO	0x80

#define LMP_EV4		0x01
#define LMP_EV5		0x02

#define LMP_SNIFF_SUBR	0x02

/* Connection modes */
#define HCI_CM_ACTIVE	0x0000
#define HCI_CM_HOLD	0x0001
#define HCI_CM_SNIFF	0x0002
#define HCI_CM_PARK	0x0003

/* Link policies */
#define HCI_LP_RSWITCH	0x0001
#define HCI_LP_HOLD	0x0002
#define HCI_LP_SNIFF	0x0004
#define HCI_LP_PARK	0x0008

/* Link modes */
#define HCI_LM_ACCEPT	0x8000
#define HCI_LM_MASTER	0x0001
#define HCI_LM_AUTH	0x0002
#define HCI_LM_ENCRYPT	0x0004
#define HCI_LM_TRUSTED	0x0008
#define HCI_LM_RELIABLE	0x0010
#define HCI_LM_SECURE	0x0020

/* -----  HCI Commands ---- */
/* OGF & OCF values */

/* Informational Parameters */
#define OGF_INFO_PARAM	0x04

#define OCF_READ_LOCAL_VERSION	0x0001
struct hci_rp_read_loc_version {
	__u8     status;
	__u8     hci_ver;
	__le16   hci_rev;
	__u8     lmp_ver;
	__le16   manufacturer;
	__le16   lmp_subver;
} __attribute__ ((packed));

#define OCF_READ_LOCAL_FEATURES	0x0003
struct hci_rp_read_local_features {
	__u8 status;
	__u8 features[8];
} __attribute__ ((packed));

#define OCF_READ_BUFFER_SIZE	0x0005
struct hci_rp_read_buffer_size {
	__u8     status;
	__le16   acl_mtu;
	__u8     sco_mtu;
	__le16   acl_max_pkt;
	__le16   sco_max_pkt;
} __attribute__ ((packed));

#define OCF_READ_BD_ADDR	0x0009
struct hci_rp_read_bd_addr {
	__u8     status;
	bdaddr_t bdaddr;
} __attribute__ ((packed));

/* Host Controller and Baseband */
#define OGF_HOST_CTL	0x03
#define OCF_RESET		0x0003
#define OCF_READ_AUTH_ENABLE	0x001F
#define OCF_WRITE_AUTH_ENABLE	0x0020
	#define AUTH_DISABLED		0x00
	#define AUTH_ENABLED		0x01

#define OCF_READ_ENCRYPT_MODE	0x0021
#define OCF_WRITE_ENCRYPT_MODE	0x0022
	#define ENCRYPT_DISABLED	0x00
	#define ENCRYPT_P2P		0x01
	#define ENCRYPT_BOTH		0x02

#define OCF_WRITE_CA_TIMEOUT  	0x0016	
#define OCF_WRITE_PG_TIMEOUT  	0x0018

#define OCF_WRITE_SCAN_ENABLE 	0x001A
	#define SCAN_DISABLED		0x00
	#define SCAN_INQUIRY		0x01
	#define SCAN_PAGE		0x02

#define OCF_SET_EVENT_FLT	0x0005
struct hci_cp_set_event_flt {
	__u8     flt_type;
	__u8     cond_type;
	__u8     condition[0];
} __attribute__ ((packed));

/* Filter types */
#define HCI_FLT_CLEAR_ALL	0x00
#define HCI_FLT_INQ_RESULT	0x01
#define HCI_FLT_CONN_SETUP	0x02

/* CONN_SETUP Condition types */
#define HCI_CONN_SETUP_ALLOW_ALL	0x00
#define HCI_CONN_SETUP_ALLOW_CLASS	0x01
#define HCI_CONN_SETUP_ALLOW_BDADDR	0x02

/* CONN_SETUP Conditions */
#define HCI_CONN_SETUP_AUTO_OFF	0x01
#define HCI_CONN_SETUP_AUTO_ON	0x02

#define OCF_READ_CLASS_OF_DEV	0x0023
struct hci_rp_read_dev_class {
	__u8     status;
	__u8     dev_class[3];
} __attribute__ ((packed));

#define OCF_WRITE_CLASS_OF_DEV	0x0024
struct hci_cp_write_dev_class {
	__u8     dev_class[3];
} __attribute__ ((packed));

#define OCF_READ_VOICE_SETTING	0x0025
struct hci_rp_read_voice_setting {
	__u8     status;
	__le16   voice_setting;
} __attribute__ ((packed));

#define OCF_WRITE_VOICE_SETTING	0x0026
struct hci_cp_write_voice_setting {
	__le16   voice_setting;
} __attribute__ ((packed));

#define OCF_HOST_BUFFER_SIZE	0x0033
struct hci_cp_host_buffer_size {
	__le16   acl_mtu;
	__u8     sco_mtu;
	__le16   acl_max_pkt;
	__le16   sco_max_pkt;
} __attribute__ ((packed));

/* Link Control */
#define OGF_LINK_CTL	0x01 

#define OCF_CREATE_CONN		0x0005
struct hci_cp_create_conn {
	bdaddr_t bdaddr;
	__le16   pkt_type;
	__u8     pscan_rep_mode;
	__u8     pscan_mode;
	__le16   clock_offset;
	__u8     role_switch;
} __attribute__ ((packed));

#define OCF_CREATE_CONN_CANCEL	0x0008
struct hci_cp_create_conn_cancel {
	bdaddr_t bdaddr;
} __attribute__ ((packed));

#define OCF_ACCEPT_CONN_REQ	0x0009
struct hci_cp_accept_conn_req {
	bdaddr_t bdaddr;
	__u8     role;
} __attribute__ ((packed));

#define OCF_REJECT_CONN_REQ	0x000a
struct hci_cp_reject_conn_req {
	bdaddr_t bdaddr;
	__u8     reason;
} __attribute__ ((packed));

#define OCF_DISCONNECT	0x0006
struct hci_cp_disconnect {
	__le16   handle;
	__u8     reason;
} __attribute__ ((packed));

#define OCF_ADD_SCO	0x0007
struct hci_cp_add_sco {
	__le16   handle;
	__le16   pkt_type;
} __attribute__ ((packed));

#define OCF_INQUIRY		0x0001
struct hci_cp_inquiry {
	__u8     lap[3];
	__u8     length;
	__u8     num_rsp;
} __attribute__ ((packed));

#define OCF_INQUIRY_CANCEL	0x0002

#define OCF_EXIT_PERIODIC_INQ	0x0004

#define OCF_LINK_KEY_REPLY	0x000B
struct hci_cp_link_key_reply {
	bdaddr_t bdaddr;
	__u8     link_key[16];
} __attribute__ ((packed));

#define OCF_LINK_KEY_NEG_REPLY	0x000C
struct hci_cp_link_key_neg_reply {
	bdaddr_t bdaddr;
} __attribute__ ((packed));

#define OCF_PIN_CODE_REPLY	0x000D
struct hci_cp_pin_code_reply {
	bdaddr_t bdaddr;
	__u8     pin_len;
	__u8     pin_code[16];
} __attribute__ ((packed));

#define OCF_PIN_CODE_NEG_REPLY	0x000E
struct hci_cp_pin_code_neg_reply {
	bdaddr_t bdaddr;
} __attribute__ ((packed));

#define OCF_CHANGE_CONN_PTYPE	0x000F
struct hci_cp_change_conn_ptype {
	__le16   handle;
	__le16   pkt_type;
} __attribute__ ((packed));

#define OCF_AUTH_REQUESTED	0x0011
struct hci_cp_auth_requested {
	__le16   handle;
} __attribute__ ((packed));

#define OCF_SET_CONN_ENCRYPT	0x0013
struct hci_cp_set_conn_encrypt {
	__le16   handle;
	__u8     encrypt;
} __attribute__ ((packed));

#define OCF_CHANGE_CONN_LINK_KEY 0x0015
struct hci_cp_change_conn_link_key {
	__le16   handle;
} __attribute__ ((packed));

#define OCF_READ_REMOTE_FEATURES 0x001B
struct hci_cp_read_remote_features {
	__le16   handle;
} __attribute__ ((packed));

#define OCF_READ_REMOTE_VERSION 0x001D
struct hci_cp_read_remote_version {
	__le16   handle;
} __attribute__ ((packed));

/* Link Policy */
#define OGF_LINK_POLICY	0x02   

#define OCF_SNIFF_MODE		0x0003
struct hci_cp_sniff_mode {
	__le16   handle;
	__le16   max_interval;
	__le16   min_interval;
	__le16   attempt;
	__le16   timeout;
} __attribute__ ((packed));

#define OCF_EXIT_SNIFF_MODE	0x0004
struct hci_cp_exit_sniff_mode {
	__le16   handle;
} __attribute__ ((packed));

#define OCF_ROLE_DISCOVERY	0x0009
struct hci_cp_role_discovery {
	__le16   handle;
} __attribute__ ((packed));
struct hci_rp_role_discovery {
	__u8     status;
	__le16   handle;
	__u8     role;
} __attribute__ ((packed));

#define OCF_READ_LINK_POLICY	0x000C
struct hci_cp_read_link_policy {
	__le16   handle;
} __attribute__ ((packed));
struct hci_rp_read_link_policy {
	__u8     status;
	__le16   handle;
	__le16   policy;
} __attribute__ ((packed));

#define OCF_SWITCH_ROLE		0x000B
struct hci_cp_switch_role {
	bdaddr_t bdaddr;
	__u8     role;
} __attribute__ ((packed));

#define OCF_WRITE_LINK_POLICY	0x000D
struct hci_cp_write_link_policy {
	__le16   handle;
	__le16   policy;
} __attribute__ ((packed));
struct hci_rp_write_link_policy {
	__u8     status;
	__le16   handle;
} __attribute__ ((packed));

#define OCF_SNIFF_SUBRATE	0x0011
struct hci_cp_sniff_subrate {
	__le16   handle;
	__le16   max_latency;
	__le16   min_remote_timeout;
	__le16   min_local_timeout;
} __attribute__ ((packed));

/* Status params */
#define OGF_STATUS_PARAM	0x05

/* Testing commands */
#define OGF_TESTING_CMD		0x3E

/* Vendor specific commands */
#define OGF_VENDOR_CMD		0x3F

/* ---- HCI Events ---- */
#define HCI_EV_INQUIRY_COMPLETE	0x01

#define HCI_EV_INQUIRY_RESULT	0x02
struct inquiry_info {
	bdaddr_t bdaddr;
	__u8     pscan_rep_mode;
	__u8     pscan_period_mode;
	__u8     pscan_mode;
	__u8     dev_class[3];
	__le16   clock_offset;
} __attribute__ ((packed));

#define HCI_EV_INQUIRY_RESULT_WITH_RSSI	0x22
struct inquiry_info_with_rssi {
	bdaddr_t bdaddr;
	__u8     pscan_rep_mode;
	__u8     pscan_period_mode;
	__u8     dev_class[3];
	__le16   clock_offset;
	__s8     rssi;
} __attribute__ ((packed));
struct inquiry_info_with_rssi_and_pscan_mode {
	bdaddr_t bdaddr;
	__u8     pscan_rep_mode;
	__u8     pscan_period_mode;
	__u8     pscan_mode;
	__u8     dev_class[3];
	__le16   clock_offset;
	__s8     rssi;
} __attribute__ ((packed));

#define HCI_EV_EXTENDED_INQUIRY_RESULT	0x2F
struct extended_inquiry_info {
	bdaddr_t bdaddr;
	__u8     pscan_rep_mode;
	__u8     pscan_period_mode;
	__u8     dev_class[3];
	__le16   clock_offset;
	__s8     rssi;
	__u8     data[240];
} __attribute__ ((packed));

#define HCI_EV_CONN_COMPLETE 	0x03
struct hci_ev_conn_complete {
	__u8     status;
	__le16   handle;
	bdaddr_t bdaddr;
	__u8     link_type;
	__u8     encr_mode;
} __attribute__ ((packed));

#define HCI_EV_CONN_REQUEST	0x04
struct hci_ev_conn_request {
	bdaddr_t bdaddr;
	__u8     dev_class[3];
	__u8     link_type;
} __attribute__ ((packed));

#define HCI_EV_DISCONN_COMPLETE	0x05
struct hci_ev_disconn_complete {
	__u8     status;
	__le16   handle;
	__u8     reason;
} __attribute__ ((packed));

#define HCI_EV_AUTH_COMPLETE	0x06
struct hci_ev_auth_complete {
	__u8     status;
	__le16   handle;
} __attribute__ ((packed));

#define HCI_EV_ENCRYPT_CHANGE	0x08
struct hci_ev_encrypt_change {
	__u8     status;
	__le16   handle;
	__u8     encrypt;
} __attribute__ ((packed));

#define HCI_EV_CHANGE_CONN_LINK_KEY_COMPLETE	0x09
struct hci_ev_change_conn_link_key_complete {
	__u8     status;
	__le16   handle;
} __attribute__ ((packed));

#define HCI_EV_QOS_SETUP_COMPLETE	0x0D
struct hci_qos {
	__u8     service_type;
	__u32    token_rate;
	__u32    peak_bandwidth;
	__u32    latency;
	__u32    delay_variation;
} __attribute__ ((packed));
struct hci_ev_qos_setup_complete {
	__u8     status;
	__le16   handle;
	struct   hci_qos qos;
} __attribute__ ((packed));

#define HCI_EV_CMD_COMPLETE 	0x0E
struct hci_ev_cmd_complete {
	__u8     ncmd;
	__le16   opcode;
} __attribute__ ((packed));

#define HCI_EV_CMD_STATUS 	0x0F
struct hci_ev_cmd_status {
	__u8     status;
	__u8     ncmd;
	__le16   opcode;
} __attribute__ ((packed));

#define HCI_EV_NUM_COMP_PKTS	0x13
struct hci_ev_num_comp_pkts {
	__u8     num_hndl;
	/* variable length part */
} __attribute__ ((packed));

#define HCI_EV_ROLE_CHANGE	0x12
struct hci_ev_role_change {
	__u8     status;
	bdaddr_t bdaddr;
	__u8     role;
} __attribute__ ((packed));

#define HCI_EV_MODE_CHANGE	0x14
struct hci_ev_mode_change {
	__u8     status;
	__le16   handle;
	__u8     mode;
	__le16   interval;
} __attribute__ ((packed));

#define HCI_EV_PIN_CODE_REQ	0x16
struct hci_ev_pin_code_req {
	bdaddr_t bdaddr;
} __attribute__ ((packed));

#define HCI_EV_LINK_KEY_REQ	0x17
struct hci_ev_link_key_req {
	bdaddr_t bdaddr;
} __attribute__ ((packed));

#define HCI_EV_LINK_KEY_NOTIFY	0x18
struct hci_ev_link_key_notify {
	bdaddr_t bdaddr;
	__u8	 link_key[16];
	__u8	 key_type;
} __attribute__ ((packed));

#define HCI_EV_REMOTE_FEATURES	0x0B
struct hci_ev_remote_features {
	__u8     status;
	__le16   handle;
	__u8     features[8];
} __attribute__ ((packed));

#define HCI_EV_REMOTE_VERSION	0x0C
struct hci_ev_remote_version {
	__u8     status;
	__le16   handle;
	__u8     lmp_ver;
	__le16   manufacturer;
	__le16   lmp_subver;
} __attribute__ ((packed));

#define HCI_EV_CLOCK_OFFSET	0x01C
struct hci_ev_clock_offset {
	__u8     status;
	__le16   handle;
	__le16   clock_offset;
} __attribute__ ((packed));

#define HCI_EV_PSCAN_REP_MODE	0x20
struct hci_ev_pscan_rep_mode {
	bdaddr_t bdaddr;
	__u8     pscan_rep_mode;
} __attribute__ ((packed));

#define HCI_EV_SNIFF_SUBRATE	0x2E
struct hci_ev_sniff_subrate {
	__u8     status;
	__le16   handle;
	__le16   max_tx_latency;
	__le16   max_rx_latency;
	__le16   max_remote_timeout;
	__le16   max_local_timeout;
} __attribute__ ((packed));

/* Internal events generated by Bluetooth stack */
#define HCI_EV_STACK_INTERNAL	0xFD
struct hci_ev_stack_internal {
	__u16    type;
	__u8     data[0];
} __attribute__ ((packed));

#define HCI_EV_SI_DEVICE  	0x01
struct hci_ev_si_device {
	__u16    event;
	__u16    dev_id;
} __attribute__ ((packed));

#define HCI_EV_SI_SECURITY	0x02
struct hci_ev_si_security {
	__u16    event;
	__u16    proto;
	__u16    subproto;
	__u8     incoming;
} __attribute__ ((packed));

/* ---- HCI Packet structures ---- */
#define HCI_COMMAND_HDR_SIZE 3
#define HCI_EVENT_HDR_SIZE   2
#define HCI_ACL_HDR_SIZE     4
#define HCI_SCO_HDR_SIZE     3

struct hci_command_hdr {
	__le16 	opcode;		/* OCF & OGF */
	__u8 	plen;
} __attribute__ ((packed));

struct hci_event_hdr {
	__u8 	evt;
	__u8 	plen;
} __attribute__ ((packed));

struct hci_acl_hdr {
	__le16 	handle;		/* Handle & Flags(PB, BC) */
	__le16 	dlen;
} __attribute__ ((packed));

struct hci_sco_hdr {
	__le16 	handle;
	__u8 	dlen;
} __attribute__ ((packed));

#ifdef __KERNEL__
#include <linux/skbuff.h>
static inline struct hci_event_hdr *hci_event_hdr(const struct sk_buff *skb)
{
	return (struct hci_event_hdr *)skb->data;
}

static inline struct hci_acl_hdr *hci_acl_hdr(const struct sk_buff *skb)
{
	return (struct hci_acl_hdr *)skb->data;
}

static inline struct hci_sco_hdr *hci_sco_hdr(const struct sk_buff *skb)
{
	return (struct hci_sco_hdr *)skb->data;
}
#endif

/* Command opcode pack/unpack */
#define hci_opcode_pack(ogf, ocf)	(__u16) ((ocf & 0x03ff)|(ogf << 10))
#define hci_opcode_ogf(op)		(op >> 10)
#define hci_opcode_ocf(op)		(op & 0x03ff)

/* ACL handle and flags pack/unpack */
#define hci_handle_pack(h, f)	(__u16) ((h & 0x0fff)|(f << 12))
#define hci_handle(h)		(h & 0x0fff)
#define hci_flags(h)		(h >> 12)

/* ---- HCI Sockets ---- */

/* Socket options */
#define HCI_DATA_DIR	1
#define HCI_FILTER	2
#define HCI_TIME_STAMP	3

/* CMSG flags */
#define HCI_CMSG_DIR	0x0001
#define HCI_CMSG_TSTAMP	0x0002

struct sockaddr_hci {
	sa_family_t    hci_family;
	unsigned short hci_dev;
};
#define HCI_DEV_NONE	0xffff

struct hci_filter {
	unsigned long type_mask;
	unsigned long event_mask[2];
	__le16   opcode;
};

struct hci_ufilter {
	__u32   type_mask;
	__u32   event_mask[2];
	__le16   opcode;
};

#define HCI_FLT_TYPE_BITS	31
#define HCI_FLT_EVENT_BITS	63
#define HCI_FLT_OGF_BITS	63
#define HCI_FLT_OCF_BITS	127

/* ---- HCI Ioctl requests structures ---- */
struct hci_dev_stats {
	__u32 err_rx;
	__u32 err_tx;
	__u32 cmd_tx;
	__u32 evt_rx;
	__u32 acl_tx;
	__u32 acl_rx;
	__u32 sco_tx;
	__u32 sco_rx;
	__u32 byte_rx;
	__u32 byte_tx;
};

struct hci_dev_info {
	__u16 dev_id;
	char  name[8];

	bdaddr_t bdaddr;

	__u32 flags;
	__u8  type;

	__u8  features[8];

	__u32 pkt_type;
	__u32 link_policy;
	__u32 link_mode;

	__u16 acl_mtu;
	__u16 acl_pkts;
	__u16 sco_mtu;
	__u16 sco_pkts;

	struct hci_dev_stats stat;
};

struct hci_conn_info {
	__u16    handle;
	bdaddr_t bdaddr;
	__u8	 type;
	__u8	 out;
	__u16	 state;
	__u32	 link_mode;
};

struct hci_dev_req {
	__u16 dev_id;
	__u32 dev_opt;
};

struct hci_dev_list_req {
	__u16  dev_num;
	struct hci_dev_req dev_req[0];	/* hci_dev_req structures */
};

struct hci_conn_list_req {
	__u16  dev_id;
	__u16  conn_num;
	struct hci_conn_info conn_info[0];
};

struct hci_conn_info_req {
	bdaddr_t bdaddr;
	__u8     type;
	struct   hci_conn_info conn_info[0];
};

struct hci_inquiry_req {
	__u16 dev_id;
	__u16 flags;
	__u8  lap[3];
	__u8  length;
	__u8  num_rsp;
};
#define IREQ_CACHE_FLUSH 0x0001

#endif /* __HCI_H */
