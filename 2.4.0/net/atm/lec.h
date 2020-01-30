/*
 *
 * Lan Emulation client header file
 *
 * Marko Kiiskila carnil@cs.tut.fi
 *
 */

#ifndef _LEC_H_
#define _LEC_H_

#include <linux/config.h>
#include <linux/atmdev.h>
#include <linux/netdevice.h>
#include <linux/atmlec.h>

#if defined (CONFIG_BRIDGE) || defined(CONFIG_BRIDGE_MODULE)
#include <linux/if_bridge.h>
struct net_bridge_fdb_entry *(*br_fdb_get_hook)(struct net_bridge *br,
                                                unsigned char *addr);
void (*br_fdb_put_hook)(struct net_bridge_fdb_entry *ent);
#endif /* defined(CONFIG_BRIDGE) || defined(CONFIG_BRIDGE_MODULE) */

#define LEC_HEADER_LEN 16

struct lecdatahdr_8023 {
  unsigned short le_header;
  unsigned char h_dest[ETH_ALEN];
  unsigned char h_source[ETH_ALEN];
  unsigned short h_type;
};

struct lecdatahdr_8025 {
  unsigned short le_header;
  unsigned char ac_pad;
  unsigned char fc;
  unsigned char h_dest[ETH_ALEN];
  unsigned char h_source[ETH_ALEN];
};

/*
 * Operations that LANE2 capable device can do. Two first functions
 * are used to make the device do things. See spec 3.1.3 and 3.1.4.
 *
 * The third function is intented for the MPOA component sitting on
 * top of the LANE device. The MPOA component assigns it's own function
 * to (*associate_indicator)() and the LANE device will use that
 * function to tell about TLVs it sees floating through.
 *
 */
struct lane2_ops {
	int  (*resolve)(struct net_device *dev, u8 *dst_mac, int force,
                        u8 **tlvs, u32 *sizeoftlvs);
        int  (*associate_req)(struct net_device *dev, u8 *lan_dst,
                              u8 *tlvs, u32 sizeoftlvs);
	void (*associate_indicator)(struct net_device *dev, u8 *mac_addr,
                                    u8 *tlvs, u32 sizeoftlvs);
};

struct atm_lane_ops {
        int (*lecd_attach)(struct atm_vcc *vcc, int arg);
        int (*mcast_attach)(struct atm_vcc *vcc, int arg);
        int (*vcc_attach)(struct atm_vcc *vcc, void *arg);
        struct net_device **(*get_lecs)(void);
};

/*
 * ATM LAN Emulation supports both LLC & Dix Ethernet EtherType
 * frames. 
 * 1. Dix Ethernet EtherType frames encoded by placing EtherType
 *    field in h_type field. Data follows immediatelly after header.
 * 2. LLC Data frames whose total length, including LLC field and data,
 *    but not padding required to meet the minimum data frame length, 
 *    is less than 1536(0x0600) MUST be encoded by placing that length
 *    in the the h_type field. The LLC field follows header immediatelly.
 * 3. LLC data frames longer than this maximum MUST be encoded by placing
 *    the value 0 in the h_type field.
 *
 */

/* Hash table size */
#define LEC_ARP_TABLE_SIZE 16

struct lec_priv {
        struct net_device_stats stats;
        unsigned short lecid;      /* Lecid of this client */
        struct lec_arp_table *lec_arp_empty_ones;
        /* Used for storing VCC's that don't have a MAC address attached yet */
        struct lec_arp_table *lec_arp_tables[LEC_ARP_TABLE_SIZE];
        /* Actual LE ARP table */
        struct lec_arp_table *lec_no_forward;
        /* Used for storing VCC's (and forward packets from) which are to
           age out by not using them to forward packets. 
           This is because to some LE clients there will be 2 VCCs. Only
           one of them gets used. */
        struct lec_arp_table *mcast_fwds;
        /* With LANEv2 it is possible that BUS (or a special multicast server)
           establishes multiple Multicast Forward VCCs to us. This list
           collects all those VCCs. LANEv1 client has only one item in this
           list. These entries are not aged out. */
        atomic_t lec_arp_lock_var;
        struct atm_vcc *mcast_vcc; /* Default Multicast Send VCC */
        struct atm_vcc *lecd;
        struct timer_list lec_arp_timer;
        /* C10 */
        unsigned int maximum_unknown_frame_count;
/* Within the period of time defined by this variable, the client will send 
   no more than C10 frames to BUS for a given unicast destination. (C11) */
        unsigned long max_unknown_frame_time;
/* If no traffic has been sent in this vcc for this period of time,
   vcc will be torn down (C12)*/
        unsigned long vcc_timeout_period;
/* An LE Client MUST not retry an LE_ARP_REQUEST for a 
   given frame's LAN Destination more than maximum retry count times,
   after the first LEC_ARP_REQUEST (C13)*/
        unsigned short max_retry_count;
/* Max time the client will maintain an entry in its arp cache in
   absence of a verification of that relationship (C17)*/
        unsigned long aging_time;
/* Max time the client will maintain an entry in cache when
   topology change flag is true (C18) */
        unsigned long forward_delay_time;
/* Topology change flag  (C19)*/
        int topology_change;
/* Max time the client expects an LE_ARP_REQUEST/LE_ARP_RESPONSE
   cycle to take (C20)*/
        unsigned long arp_response_time;
/* Time limit ot wait to receive an LE_FLUSH_RESPONSE after the
   LE_FLUSH_REQUEST has been sent before taking recover action. (C21)*/
        unsigned long flush_timeout;
/* The time since sending a frame to the bus after which the
   LE Client may assume that the frame has been either discarded or
   delivered to the recipient (C22) */
        unsigned long path_switching_delay;

        u8 *tlvs;          /* LANE2: TLVs are new                */
        u32 sizeoftlvs;    /* The size of the tlv array in bytes */
        int lane_version;  /* LANE2                              */
	int itfnum;        /* e.g. 2 for lec2, 5 for lec5        */
        struct lane2_ops *lane2_ops; /* can be NULL for LANE v1  */
        int is_proxy;      /* bridge between ATM and Ethernet    */
        int is_trdev;      /* Device type, 0 = Ethernet, 1 = TokenRing */
};

int lecd_attach(struct atm_vcc *vcc, int arg);
int lec_vcc_attach(struct atm_vcc *vcc, void *arg);
int lec_mcast_attach(struct atm_vcc *vcc, int arg);
struct net_device **get_dev_lec(void);
int make_lec(struct atm_vcc *vcc);
int send_to_lecd(struct lec_priv *priv,
                 atmlec_msg_type type, unsigned char *mac_addr,
                 unsigned char *atm_addr, struct sk_buff *data);
void lec_push(struct atm_vcc *vcc, struct sk_buff *skb);

void atm_lane_init(void);
void atm_lane_init_ops(struct atm_lane_ops *ops);
#endif _LEC_H_

