/*******************************************************************************

  
  Copyright(c) 1999 - 2004 Intel Corporation. All rights reserved.
  
  This program is free software; you can redistribute it and/or modify it 
  under the terms of the GNU General Public License as published by the Free 
  Software Foundation; either version 2 of the License, or (at your option) 
  any later version.
  
  This program is distributed in the hope that it will be useful, but WITHOUT 
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for 
  more details.
  
  You should have received a copy of the GNU General Public License along with
  this program; if not, write to the Free Software Foundation, Inc., 59 
  Temple Place - Suite 330, Boston, MA  02111-1307, USA.
  
  The full GNU General Public License is included in this distribution in the
  file called LICENSE.
  
  Contact Information:
  Linux NICS <linux.nics@intel.com>
  Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497

*******************************************************************************/

/* e1000_hw.h
 * Structures, enums, and macros for the MAC
 */

#ifndef _E1000_HW_H_
#define _E1000_HW_H_

#include "e1000_osdep.h"


/* Forward declarations of structures used by the shared code */
struct e1000_hw;
struct e1000_hw_stats;

/* Enumerated types specific to the e1000 hardware */
/* Media Access Controlers */
typedef enum {
    e1000_undefined = 0,
    e1000_82542_rev2_0,
    e1000_82542_rev2_1,
    e1000_82543,
    e1000_82544,
    e1000_82540,
    e1000_82545,
    e1000_82545_rev_3,
    e1000_82546,
    e1000_82546_rev_3,
    e1000_82541,
    e1000_82541_rev_2,
    e1000_82547,
    e1000_82547_rev_2,
    e1000_num_macs
} e1000_mac_type;

typedef enum {
    e1000_eeprom_uninitialized = 0,
    e1000_eeprom_spi,
    e1000_eeprom_microwire,
    e1000_num_eeprom_types
} e1000_eeprom_type;

/* Media Types */
typedef enum {
    e1000_media_type_copper = 0,
    e1000_media_type_fiber = 1,
    e1000_media_type_internal_serdes = 2,
    e1000_num_media_types
} e1000_media_type;

typedef enum {
    e1000_10_half = 0,
    e1000_10_full = 1,
    e1000_100_half = 2,
    e1000_100_full = 3
} e1000_speed_duplex_type;

/* Flow Control Settings */
typedef enum {
    e1000_fc_none = 0,
    e1000_fc_rx_pause = 1,
    e1000_fc_tx_pause = 2,
    e1000_fc_full = 3,
    e1000_fc_default = 0xFF
} e1000_fc_type;

/* PCI bus types */
typedef enum {
    e1000_bus_type_unknown = 0,
    e1000_bus_type_pci,
    e1000_bus_type_pcix,
    e1000_bus_type_reserved
} e1000_bus_type;

/* PCI bus speeds */
typedef enum {
    e1000_bus_speed_unknown = 0,
    e1000_bus_speed_33,
    e1000_bus_speed_66,
    e1000_bus_speed_100,
    e1000_bus_speed_120,
    e1000_bus_speed_133,
    e1000_bus_speed_reserved
} e1000_bus_speed;

/* PCI bus widths */
typedef enum {
    e1000_bus_width_unknown = 0,
    e1000_bus_width_32,
    e1000_bus_width_64,
    e1000_bus_width_reserved
} e1000_bus_width;

/* PHY status info structure and supporting enums */
typedef enum {
    e1000_cable_length_50 = 0,
    e1000_cable_length_50_80,
    e1000_cable_length_80_110,
    e1000_cable_length_110_140,
    e1000_cable_length_140,
    e1000_cable_length_undefined = 0xFF
} e1000_cable_length;

typedef enum {
    e1000_igp_cable_length_10  = 10,
    e1000_igp_cable_length_20  = 20,
    e1000_igp_cable_length_30  = 30,
    e1000_igp_cable_length_40  = 40,
    e1000_igp_cable_length_50  = 50,
    e1000_igp_cable_length_60  = 60,
    e1000_igp_cable_length_70  = 70,
    e1000_igp_cable_length_80  = 80,
    e1000_igp_cable_length_90  = 90,
    e1000_igp_cable_length_100 = 100,
    e1000_igp_cable_length_110 = 110,
    e1000_igp_cable_length_120 = 120,
    e1000_igp_cable_length_130 = 130,
    e1000_igp_cable_length_140 = 140,
    e1000_igp_cable_length_150 = 150,
    e1000_igp_cable_length_160 = 160,
    e1000_igp_cable_length_170 = 170,
    e1000_igp_cable_length_180 = 180
} e1000_igp_cable_length;

typedef enum {
    e1000_10bt_ext_dist_enable_normal = 0,
    e1000_10bt_ext_dist_enable_lower,
    e1000_10bt_ext_dist_enable_undefined = 0xFF
} e1000_10bt_ext_dist_enable;

typedef enum {
    e1000_rev_polarity_normal = 0,
    e1000_rev_polarity_reversed,
    e1000_rev_polarity_undefined = 0xFF
} e1000_rev_polarity;

typedef enum {
    e1000_downshift_normal = 0,
    e1000_downshift_activated,
    e1000_downshift_undefined = 0xFF
} e1000_downshift;

typedef enum {
    e1000_polarity_reversal_enabled = 0,
    e1000_polarity_reversal_disabled,
    e1000_polarity_reversal_undefined = 0xFF
} e1000_polarity_reversal;

typedef enum {
    e1000_auto_x_mode_manual_mdi = 0,
    e1000_auto_x_mode_manual_mdix,
    e1000_auto_x_mode_auto1,
    e1000_auto_x_mode_auto2,
    e1000_auto_x_mode_undefined = 0xFF
} e1000_auto_x_mode;

typedef enum {
    e1000_1000t_rx_status_not_ok = 0,
    e1000_1000t_rx_status_ok,
    e1000_1000t_rx_status_undefined = 0xFF
} e1000_1000t_rx_status;

typedef enum {
    e1000_phy_m88 = 0,
    e1000_phy_igp,
    e1000_phy_undefined = 0xFF
} e1000_phy_type;

typedef enum {
    e1000_ms_hw_default = 0,
    e1000_ms_force_master,
    e1000_ms_force_slave,
    e1000_ms_auto
} e1000_ms_type;

typedef enum {
    e1000_ffe_config_enabled = 0,
    e1000_ffe_config_active,
    e1000_ffe_config_blocked
} e1000_ffe_config;

typedef enum {
    e1000_dsp_config_disabled = 0,
    e1000_dsp_config_enabled,
    e1000_dsp_config_activated,
    e1000_dsp_config_undefined = 0xFF
} e1000_dsp_config;

struct e1000_phy_info {
    e1000_cable_length cable_length;
    e1000_10bt_ext_dist_enable extended_10bt_distance;
    e1000_rev_polarity cable_polarity;
    e1000_downshift downshift;
    e1000_polarity_reversal polarity_correction;
    e1000_auto_x_mode mdix_mode;
    e1000_1000t_rx_status local_rx;
    e1000_1000t_rx_status remote_rx;
};

struct e1000_phy_stats {
    uint32_t idle_errors;
    uint32_t receive_errors;
};

struct e1000_eeprom_info {
    e1000_eeprom_type type;
    uint16_t word_size;
    uint16_t opcode_bits;
    uint16_t address_bits;
    uint16_t delay_usec;
    uint16_t page_size;
};



/* Error Codes */
#define E1000_SUCCESS      0
#define E1000_ERR_EEPROM   1
#define E1000_ERR_PHY      2
#define E1000_ERR_CONFIG   3
#define E1000_ERR_PARAM    4
#define E1000_ERR_MAC_TYPE 5
#define E1000_ERR_PHY_TYPE 6

/* Function prototypes */
/* Initialization */
int32_t e1000_reset_hw(struct e1000_hw *hw);
int32_t e1000_init_hw(struct e1000_hw *hw);
int32_t e1000_set_mac_type(struct e1000_hw *hw);
void e1000_set_media_type(struct e1000_hw *hw);

/* Link Configuration */
int32_t e1000_setup_link(struct e1000_hw *hw);
int32_t e1000_phy_setup_autoneg(struct e1000_hw *hw);
void e1000_config_collision_dist(struct e1000_hw *hw);
int32_t e1000_config_fc_after_link_up(struct e1000_hw *hw);
int32_t e1000_check_for_link(struct e1000_hw *hw);
int32_t e1000_get_speed_and_duplex(struct e1000_hw *hw, uint16_t * speed, uint16_t * duplex);
int32_t e1000_wait_autoneg(struct e1000_hw *hw);
int32_t e1000_force_mac_fc(struct e1000_hw *hw);

/* PHY */
int32_t e1000_read_phy_reg(struct e1000_hw *hw, uint32_t reg_addr, uint16_t *phy_data);
int32_t e1000_write_phy_reg(struct e1000_hw *hw, uint32_t reg_addr, uint16_t data);
void e1000_phy_hw_reset(struct e1000_hw *hw);
int32_t e1000_phy_reset(struct e1000_hw *hw);
int32_t e1000_detect_gig_phy(struct e1000_hw *hw);
int32_t e1000_phy_get_info(struct e1000_hw *hw, struct e1000_phy_info *phy_info);
int32_t e1000_phy_m88_get_info(struct e1000_hw *hw, struct e1000_phy_info *phy_info);
int32_t e1000_phy_igp_get_info(struct e1000_hw *hw, struct e1000_phy_info *phy_info);
int32_t e1000_get_cable_length(struct e1000_hw *hw, uint16_t *min_length, uint16_t *max_length);
int32_t e1000_check_polarity(struct e1000_hw *hw, uint16_t *polarity);
int32_t e1000_check_downshift(struct e1000_hw *hw);
int32_t e1000_validate_mdi_setting(struct e1000_hw *hw);

/* EEPROM Functions */
void e1000_init_eeprom_params(struct e1000_hw *hw);
int32_t e1000_read_eeprom(struct e1000_hw *hw, uint16_t reg, uint16_t words, uint16_t *data);
int32_t e1000_validate_eeprom_checksum(struct e1000_hw *hw);
int32_t e1000_update_eeprom_checksum(struct e1000_hw *hw);
int32_t e1000_write_eeprom(struct e1000_hw *hw, uint16_t reg, uint16_t words, uint16_t *data);
int32_t e1000_read_part_num(struct e1000_hw *hw, uint32_t * part_num);
int32_t e1000_read_mac_addr(struct e1000_hw * hw);

/* Filters (multicast, vlan, receive) */
void e1000_init_rx_addrs(struct e1000_hw *hw);
void e1000_mc_addr_list_update(struct e1000_hw *hw, uint8_t * mc_addr_list, uint32_t mc_addr_count, uint32_t pad, uint32_t rar_used_count);
uint32_t e1000_hash_mc_addr(struct e1000_hw *hw, uint8_t * mc_addr);
void e1000_mta_set(struct e1000_hw *hw, uint32_t hash_value);
void e1000_rar_set(struct e1000_hw *hw, uint8_t * mc_addr, uint32_t rar_index);
void e1000_write_vfta(struct e1000_hw *hw, uint32_t offset, uint32_t value);
void e1000_clear_vfta(struct e1000_hw *hw);

/* LED functions */
int32_t e1000_setup_led(struct e1000_hw *hw);
int32_t e1000_cleanup_led(struct e1000_hw *hw);
int32_t e1000_led_on(struct e1000_hw *hw);
int32_t e1000_led_off(struct e1000_hw *hw);

/* Adaptive IFS Functions */

/* Everything else */
uint32_t e1000_enable_mng_pass_thru(struct e1000_hw *hw);
void e1000_clear_hw_cntrs(struct e1000_hw *hw);
void e1000_reset_adaptive(struct e1000_hw *hw);
void e1000_update_adaptive(struct e1000_hw *hw);
void e1000_tbi_adjust_stats(struct e1000_hw *hw, struct e1000_hw_stats *stats, uint32_t frame_len, uint8_t * mac_addr);
void e1000_get_bus_info(struct e1000_hw *hw);
void e1000_pci_set_mwi(struct e1000_hw *hw);
void e1000_pci_clear_mwi(struct e1000_hw *hw);
void e1000_read_pci_cfg(struct e1000_hw *hw, uint32_t reg, uint16_t * value);
void e1000_write_pci_cfg(struct e1000_hw *hw, uint32_t reg, uint16_t * value);
/* Port I/O is only supported on 82544 and newer */
uint32_t e1000_io_read(struct e1000_hw *hw, unsigned long port);
uint32_t e1000_read_reg_io(struct e1000_hw *hw, uint32_t offset);
void e1000_io_write(struct e1000_hw *hw, unsigned long port, uint32_t value);
void e1000_write_reg_io(struct e1000_hw *hw, uint32_t offset, uint32_t value);
int32_t e1000_config_dsp_after_link_change(struct e1000_hw *hw, boolean_t link_up);
int32_t e1000_set_d3_lplu_state(struct e1000_hw *hw, boolean_t active);

#define E1000_READ_REG_IO(a, reg) \
    e1000_read_reg_io((a), E1000_##reg)
#define E1000_WRITE_REG_IO(a, reg, val) \
    e1000_write_reg_io((a), E1000_##reg, val)

/* PCI Device IDs */
#define E1000_DEV_ID_82542               0x1000
#define E1000_DEV_ID_82543GC_FIBER       0x1001
#define E1000_DEV_ID_82543GC_COPPER      0x1004
#define E1000_DEV_ID_82544EI_COPPER      0x1008
#define E1000_DEV_ID_82544EI_FIBER       0x1009
#define E1000_DEV_ID_82544GC_COPPER      0x100C
#define E1000_DEV_ID_82544GC_LOM         0x100D
#define E1000_DEV_ID_82540EM             0x100E
#define E1000_DEV_ID_82540EM_LOM         0x1015
#define E1000_DEV_ID_82540EP_LOM         0x1016
#define E1000_DEV_ID_82540EP             0x1017
#define E1000_DEV_ID_82540EP_LP          0x101E
#define E1000_DEV_ID_82545EM_COPPER      0x100F
#define E1000_DEV_ID_82545EM_FIBER       0x1011
#define E1000_DEV_ID_82545GM_COPPER      0x1026
#define E1000_DEV_ID_82545GM_FIBER       0x1027
#define E1000_DEV_ID_82545GM_SERDES      0x1028
#define E1000_DEV_ID_82546EB_COPPER      0x1010
#define E1000_DEV_ID_82546EB_FIBER       0x1012
#define E1000_DEV_ID_82546EB_QUAD_COPPER 0x101D
#define E1000_DEV_ID_82541EI             0x1013
#define E1000_DEV_ID_82541EI_MOBILE      0x1018
#define E1000_DEV_ID_82541ER             0x1078
#define E1000_DEV_ID_82547GI             0x1075
#define E1000_DEV_ID_82541GI             0x1076
#define E1000_DEV_ID_82541GI_MOBILE      0x1077
#define E1000_DEV_ID_82546GB_COPPER      0x1079
#define E1000_DEV_ID_82546GB_FIBER       0x107A
#define E1000_DEV_ID_82546GB_SERDES      0x107B
#define E1000_DEV_ID_82547EI             0x1019

#define NODE_ADDRESS_SIZE 6
#define ETH_LENGTH_OF_ADDRESS 6

/* MAC decode size is 128K - This is the size of BAR0 */
#define MAC_DECODE_SIZE (128 * 1024)

#define E1000_82542_2_0_REV_ID 2
#define E1000_82542_2_1_REV_ID 3
#define E1000_REVISION_0       0
#define E1000_REVISION_1       1
#define E1000_REVISION_2       2

#define SPEED_10    10
#define SPEED_100   100
#define SPEED_1000  1000
#define HALF_DUPLEX 1
#define FULL_DUPLEX 2

/* The sizes (in bytes) of a ethernet packet */
#define ENET_HEADER_SIZE             14
#define MAXIMUM_ETHERNET_FRAME_SIZE  1518 /* With FCS */
#define MINIMUM_ETHERNET_FRAME_SIZE  64   /* With FCS */
#define ETHERNET_FCS_SIZE            4
#define MAXIMUM_ETHERNET_PACKET_SIZE \
    (MAXIMUM_ETHERNET_FRAME_SIZE - ETHERNET_FCS_SIZE)
#define MINIMUM_ETHERNET_PACKET_SIZE \
    (MINIMUM_ETHERNET_FRAME_SIZE - ETHERNET_FCS_SIZE)
#define CRC_LENGTH                   ETHERNET_FCS_SIZE
#define MAX_JUMBO_FRAME_SIZE         0x3F00


/* 802.1q VLAN Packet Sizes */
#define VLAN_TAG_SIZE                     4     /* 802.3ac tag (not DMAed) */

/* Ethertype field values */
#define ETHERNET_IEEE_VLAN_TYPE 0x8100  /* 802.3ac packet */
#define ETHERNET_IP_TYPE        0x0800  /* IP packets */
#define ETHERNET_ARP_TYPE       0x0806  /* Address Resolution Protocol (ARP) */

/* Packet Header defines */
#define IP_PROTOCOL_TCP    6
#define IP_PROTOCOL_UDP    0x11

/* This defines the bits that are set in the Interrupt Mask
 * Set/Read Register.  Each bit is documented below:
 *   o RXDMT0 = Receive Descriptor Minimum Threshold hit (ring 0)
 *   o RXSEQ  = Receive Sequence Error
 */
#define POLL_IMS_ENABLE_MASK ( \
    E1000_IMS_RXDMT0 |         \
    E1000_IMS_RXSEQ)

/* This defines the bits that are set in the Interrupt Mask
 * Set/Read Register.  Each bit is documented below:
 *   o RXT0   = Receiver Timer Interrupt (ring 0)
 *   o TXDW   = Transmit Descriptor Written Back
 *   o RXDMT0 = Receive Descriptor Minimum Threshold hit (ring 0)
 *   o RXSEQ  = Receive Sequence Error
 *   o LSC    = Link Status Change
 */
#define IMS_ENABLE_MASK ( \
    E1000_IMS_RXT0   |    \
    E1000_IMS_TXDW   |    \
    E1000_IMS_RXDMT0 |    \
    E1000_IMS_RXSEQ  |    \
    E1000_IMS_LSC)

/* Number of high/low register pairs in the RAR. The RAR (Receive Address
 * Registers) holds the directed and multicast addresses that we monitor. We
 * reserve one of these spots for our directed address, allowing us room for
 * E1000_RAR_ENTRIES - 1 multicast addresses.
 */
#define E1000_RAR_ENTRIES 15

#define MIN_NUMBER_OF_DESCRIPTORS 8
#define MAX_NUMBER_OF_DESCRIPTORS 0xFFF8

/* Receive Descriptor */
struct e1000_rx_desc {
    uint64_t buffer_addr; /* Address of the descriptor's data buffer */
    uint16_t length;     /* Length of data DMAed into data buffer */
    uint16_t csum;       /* Packet checksum */
    uint8_t status;      /* Descriptor status */
    uint8_t errors;      /* Descriptor Errors */
    uint16_t special;
};

/* Receive Decriptor bit definitions */
#define E1000_RXD_STAT_DD       0x01    /* Descriptor Done */
#define E1000_RXD_STAT_EOP      0x02    /* End of Packet */
#define E1000_RXD_STAT_IXSM     0x04    /* Ignore checksum */
#define E1000_RXD_STAT_VP       0x08    /* IEEE VLAN Packet */
#define E1000_RXD_STAT_TCPCS    0x20    /* TCP xsum calculated */
#define E1000_RXD_STAT_IPCS     0x40    /* IP xsum calculated */
#define E1000_RXD_STAT_PIF      0x80    /* passed in-exact filter */
#define E1000_RXD_ERR_CE        0x01    /* CRC Error */
#define E1000_RXD_ERR_SE        0x02    /* Symbol Error */
#define E1000_RXD_ERR_SEQ       0x04    /* Sequence Error */
#define E1000_RXD_ERR_CXE       0x10    /* Carrier Extension Error */
#define E1000_RXD_ERR_TCPE      0x20    /* TCP/UDP Checksum Error */
#define E1000_RXD_ERR_IPE       0x40    /* IP Checksum Error */
#define E1000_RXD_ERR_RXE       0x80    /* Rx Data Error */
#define E1000_RXD_SPC_VLAN_MASK 0x0FFF  /* VLAN ID is in lower 12 bits */
#define E1000_RXD_SPC_PRI_MASK  0xE000  /* Priority is in upper 3 bits */
#define E1000_RXD_SPC_PRI_SHIFT 0x000D  /* Priority is in upper 3 of 16 */
#define E1000_RXD_SPC_CFI_MASK  0x1000  /* CFI is bit 12 */
#define E1000_RXD_SPC_CFI_SHIFT 0x000C  /* CFI is bit 12 */

/* mask to determine if packets should be dropped due to frame errors */
#define E1000_RXD_ERR_FRAME_ERR_MASK ( \
    E1000_RXD_ERR_CE  |                \
    E1000_RXD_ERR_SE  |                \
    E1000_RXD_ERR_SEQ |                \
    E1000_RXD_ERR_CXE |                \
    E1000_RXD_ERR_RXE)

/* Transmit Descriptor */
struct e1000_tx_desc {
    uint64_t buffer_addr;       /* Address of the descriptor's data buffer */
    union {
        uint32_t data;
        struct {
            uint16_t length;    /* Data buffer length */
            uint8_t cso;        /* Checksum offset */
            uint8_t cmd;        /* Descriptor control */
        } flags;
    } lower;
    union {
        uint32_t data;
        struct {
            uint8_t status;     /* Descriptor status */
            uint8_t css;        /* Checksum start */
            uint16_t special;
        } fields;
    } upper;
};

/* Transmit Descriptor bit definitions */
#define E1000_TXD_DTYP_D     0x00100000 /* Data Descriptor */
#define E1000_TXD_DTYP_C     0x00000000 /* Context Descriptor */
#define E1000_TXD_POPTS_IXSM 0x01       /* Insert IP checksum */
#define E1000_TXD_POPTS_TXSM 0x02       /* Insert TCP/UDP checksum */
#define E1000_TXD_CMD_EOP    0x01000000 /* End of Packet */
#define E1000_TXD_CMD_IFCS   0x02000000 /* Insert FCS (Ethernet CRC) */
#define E1000_TXD_CMD_IC     0x04000000 /* Insert Checksum */
#define E1000_TXD_CMD_RS     0x08000000 /* Report Status */
#define E1000_TXD_CMD_RPS    0x10000000 /* Report Packet Sent */
#define E1000_TXD_CMD_DEXT   0x20000000 /* Descriptor extension (0 = legacy) */
#define E1000_TXD_CMD_VLE    0x40000000 /* Add VLAN tag */
#define E1000_TXD_CMD_IDE    0x80000000 /* Enable Tidv register */
#define E1000_TXD_STAT_DD    0x00000001 /* Descriptor Done */
#define E1000_TXD_STAT_EC    0x00000002 /* Excess Collisions */
#define E1000_TXD_STAT_LC    0x00000004 /* Late Collisions */
#define E1000_TXD_STAT_TU    0x00000008 /* Transmit underrun */
#define E1000_TXD_CMD_TCP    0x01000000 /* TCP packet */
#define E1000_TXD_CMD_IP     0x02000000 /* IP packet */
#define E1000_TXD_CMD_TSE    0x04000000 /* TCP Seg enable */
#define E1000_TXD_STAT_TC    0x00000004 /* Tx Underrun */

/* Offload Context Descriptor */
struct e1000_context_desc {
    union {
        uint32_t ip_config;
        struct {
            uint8_t ipcss;      /* IP checksum start */
            uint8_t ipcso;      /* IP checksum offset */
            uint16_t ipcse;     /* IP checksum end */
        } ip_fields;
    } lower_setup;
    union {
        uint32_t tcp_config;
        struct {
            uint8_t tucss;      /* TCP checksum start */
            uint8_t tucso;      /* TCP checksum offset */
            uint16_t tucse;     /* TCP checksum end */
        } tcp_fields;
    } upper_setup;
    uint32_t cmd_and_length;    /* */
    union {
        uint32_t data;
        struct {
            uint8_t status;     /* Descriptor status */
            uint8_t hdr_len;    /* Header length */
            uint16_t mss;       /* Maximum segment size */
        } fields;
    } tcp_seg_setup;
};

/* Offload data descriptor */
struct e1000_data_desc {
    uint64_t buffer_addr;       /* Address of the descriptor's buffer address */
    union {
        uint32_t data;
        struct {
            uint16_t length;    /* Data buffer length */
            uint8_t typ_len_ext;        /* */
            uint8_t cmd;        /* */
        } flags;
    } lower;
    union {
        uint32_t data;
        struct {
            uint8_t status;     /* Descriptor status */
            uint8_t popts;      /* Packet Options */
            uint16_t special;   /* */
        } fields;
    } upper;
};

/* Filters */
#define E1000_NUM_UNICAST          16   /* Unicast filter entries */
#define E1000_MC_TBL_SIZE          128  /* Multicast Filter Table (4096 bits) */
#define E1000_VLAN_FILTER_TBL_SIZE 128  /* VLAN Filter Table (4096 bits) */


/* Receive Address Register */
struct e1000_rar {
    volatile uint32_t low;      /* receive address low */
    volatile uint32_t high;     /* receive address high */
};

/* Number of entries in the Multicast Table Array (MTA). */
#define E1000_NUM_MTA_REGISTERS 128

/* IPv4 Address Table Entry */
struct e1000_ipv4_at_entry {
    volatile uint32_t ipv4_addr;        /* IP Address (RW) */
    volatile uint32_t reserved;
};

/* Four wakeup IP addresses are supported */
#define E1000_WAKEUP_IP_ADDRESS_COUNT_MAX 4
#define E1000_IP4AT_SIZE                  E1000_WAKEUP_IP_ADDRESS_COUNT_MAX
#define E1000_IP6AT_SIZE                  1

/* IPv6 Address Table Entry */
struct e1000_ipv6_at_entry {
    volatile uint8_t ipv6_addr[16];
};

/* Flexible Filter Length Table Entry */
struct e1000_fflt_entry {
    volatile uint32_t length;   /* Flexible Filter Length (RW) */
    volatile uint32_t reserved;
};

/* Flexible Filter Mask Table Entry */
struct e1000_ffmt_entry {
    volatile uint32_t mask;     /* Flexible Filter Mask (RW) */
    volatile uint32_t reserved;
};

/* Flexible Filter Value Table Entry */
struct e1000_ffvt_entry {
    volatile uint32_t value;    /* Flexible Filter Value (RW) */
    volatile uint32_t reserved;
};

/* Four Flexible Filters are supported */
#define E1000_FLEXIBLE_FILTER_COUNT_MAX 4

/* Each Flexible Filter is at most 128 (0x80) bytes in length */
#define E1000_FLEXIBLE_FILTER_SIZE_MAX  128

#define E1000_FFLT_SIZE E1000_FLEXIBLE_FILTER_COUNT_MAX
#define E1000_FFMT_SIZE E1000_FLEXIBLE_FILTER_SIZE_MAX
#define E1000_FFVT_SIZE E1000_FLEXIBLE_FILTER_SIZE_MAX

/* Register Set. (82543, 82544)
 *
 * Registers are defined to be 32 bits and  should be accessed as 32 bit values.
 * These registers are physically located on the NIC, but are mapped into the
 * host memory address space.
 *
 * RW - register is both readable and writable
 * RO - register is read only
 * WO - register is write only
 * R/clr - register is read only and is cleared when read
 * A - register array
 */
#define E1000_CTRL     0x00000  /* Device Control - RW */
#define E1000_CTRL_DUP 0x00004  /* Device Control Duplicate (Shadow) - RW */
#define E1000_STATUS   0x00008  /* Device Status - RO */
#define E1000_EECD     0x00010  /* EEPROM/Flash Control - RW */
#define E1000_EERD     0x00014  /* EEPROM Read - RW */
#define E1000_CTRL_EXT 0x00018  /* Extended Device Control - RW */
#define E1000_FLA      0x0001C  /* Flash Access - RW */
#define E1000_MDIC     0x00020  /* MDI Control - RW */
#define E1000_FCAL     0x00028  /* Flow Control Address Low - RW */
#define E1000_FCAH     0x0002C  /* Flow Control Address High -RW */
#define E1000_FCT      0x00030  /* Flow Control Type - RW */
#define E1000_VET      0x00038  /* VLAN Ether Type - RW */
#define E1000_ICR      0x000C0  /* Interrupt Cause Read - R/clr */
#define E1000_ITR      0x000C4  /* Interrupt Throttling Rate - RW */
#define E1000_ICS      0x000C8  /* Interrupt Cause Set - WO */
#define E1000_IMS      0x000D0  /* Interrupt Mask Set - RW */
#define E1000_IMC      0x000D8  /* Interrupt Mask Clear - WO */
#define E1000_RCTL     0x00100  /* RX Control - RW */
#define E1000_FCTTV    0x00170  /* Flow Control Transmit Timer Value - RW */
#define E1000_TXCW     0x00178  /* TX Configuration Word - RW */
#define E1000_RXCW     0x00180  /* RX Configuration Word - RO */
#define E1000_TCTL     0x00400  /* TX Control - RW */
#define E1000_TIPG     0x00410  /* TX Inter-packet gap -RW */
#define E1000_TBT      0x00448  /* TX Burst Timer - RW */
#define E1000_AIT      0x00458  /* Adaptive Interframe Spacing Throttle - RW */
#define E1000_LEDCTL   0x00E00  /* LED Control - RW */
#define E1000_PBA      0x01000  /* Packet Buffer Allocation - RW */
#define E1000_FCRTL    0x02160  /* Flow Control Receive Threshold Low - RW */
#define E1000_FCRTH    0x02168  /* Flow Control Receive Threshold High - RW */
#define E1000_RDBAL    0x02800  /* RX Descriptor Base Address Low - RW */
#define E1000_RDBAH    0x02804  /* RX Descriptor Base Address High - RW */
#define E1000_RDLEN    0x02808  /* RX Descriptor Length - RW */
#define E1000_RDH      0x02810  /* RX Descriptor Head - RW */
#define E1000_RDT      0x02818  /* RX Descriptor Tail - RW */
#define E1000_RDTR     0x02820  /* RX Delay Timer - RW */
#define E1000_RXDCTL   0x02828  /* RX Descriptor Control - RW */
#define E1000_RADV     0x0282C  /* RX Interrupt Absolute Delay Timer - RW */
#define E1000_RSRPD    0x02C00  /* RX Small Packet Detect - RW */
#define E1000_TXDMAC   0x03000  /* TX DMA Control - RW */
#define E1000_TDFH     0x03410  /* TX Data FIFO Head - RW */
#define E1000_TDFT     0x03418  /* TX Data FIFO Tail - RW */
#define E1000_TDFHS    0x03420  /* TX Data FIFO Head Saved - RW */
#define E1000_TDFTS    0x03428  /* TX Data FIFO Tail Saved - RW */
#define E1000_TDFPC    0x03430  /* TX Data FIFO Packet Count - RW */
#define E1000_TDBAL    0x03800  /* TX Descriptor Base Address Low - RW */
#define E1000_TDBAH    0x03804  /* TX Descriptor Base Address High - RW */
#define E1000_TDLEN    0x03808  /* TX Descriptor Length - RW */
#define E1000_TDH      0x03810  /* TX Descriptor Head - RW */
#define E1000_TDT      0x03818  /* TX Descripotr Tail - RW */
#define E1000_TIDV     0x03820  /* TX Interrupt Delay Value - RW */
#define E1000_TXDCTL   0x03828  /* TX Descriptor Control - RW */
#define E1000_TADV     0x0382C  /* TX Interrupt Absolute Delay Val - RW */
#define E1000_TSPMT    0x03830  /* TCP Segmentation PAD & Min Threshold - RW */
#define E1000_CRCERRS  0x04000  /* CRC Error Count - R/clr */
#define E1000_ALGNERRC 0x04004  /* Alignment Error Count - R/clr */
#define E1000_SYMERRS  0x04008  /* Symbol Error Count - R/clr */
#define E1000_RXERRC   0x0400C  /* Receive Error Count - R/clr */
#define E1000_MPC      0x04010  /* Missed Packet Count - R/clr */
#define E1000_SCC      0x04014  /* Single Collision Count - R/clr */
#define E1000_ECOL     0x04018  /* Excessive Collision Count - R/clr */
#define E1000_MCC      0x0401C  /* Multiple Collision Count - R/clr */
#define E1000_LATECOL  0x04020  /* Late Collision Count - R/clr */
#define E1000_COLC     0x04028  /* Collision Count - R/clr */
#define E1000_DC       0x04030  /* Defer Count - R/clr */
#define E1000_TNCRS    0x04034  /* TX-No CRS - R/clr */
#define E1000_SEC      0x04038  /* Sequence Error Count - R/clr */
#define E1000_CEXTERR  0x0403C  /* Carrier Extension Error Count - R/clr */
#define E1000_RLEC     0x04040  /* Receive Length Error Count - R/clr */
#define E1000_XONRXC   0x04048  /* XON RX Count - R/clr */
#define E1000_XONTXC   0x0404C  /* XON TX Count - R/clr */
#define E1000_XOFFRXC  0x04050  /* XOFF RX Count - R/clr */
#define E1000_XOFFTXC  0x04054  /* XOFF TX Count - R/clr */
#define E1000_FCRUC    0x04058  /* Flow Control RX Unsupported Count- R/clr */
#define E1000_PRC64    0x0405C  /* Packets RX (64 bytes) - R/clr */
#define E1000_PRC127   0x04060  /* Packets RX (65-127 bytes) - R/clr */
#define E1000_PRC255   0x04064  /* Packets RX (128-255 bytes) - R/clr */
#define E1000_PRC511   0x04068  /* Packets RX (255-511 bytes) - R/clr */
#define E1000_PRC1023  0x0406C  /* Packets RX (512-1023 bytes) - R/clr */
#define E1000_PRC1522  0x04070  /* Packets RX (1024-1522 bytes) - R/clr */
#define E1000_GPRC     0x04074  /* Good Packets RX Count - R/clr */
#define E1000_BPRC     0x04078  /* Broadcast Packets RX Count - R/clr */
#define E1000_MPRC     0x0407C  /* Multicast Packets RX Count - R/clr */
#define E1000_GPTC     0x04080  /* Good Packets TX Count - R/clr */
#define E1000_GORCL    0x04088  /* Good Octets RX Count Low - R/clr */
#define E1000_GORCH    0x0408C  /* Good Octets RX Count High - R/clr */
#define E1000_GOTCL    0x04090  /* Good Octets TX Count Low - R/clr */
#define E1000_GOTCH    0x04094  /* Good Octets TX Count High - R/clr */
#define E1000_RNBC     0x040A0  /* RX No Buffers Count - R/clr */
#define E1000_RUC      0x040A4  /* RX Undersize Count - R/clr */
#define E1000_RFC      0x040A8  /* RX Fragment Count - R/clr */
#define E1000_ROC      0x040AC  /* RX Oversize Count - R/clr */
#define E1000_RJC      0x040B0  /* RX Jabber Count - R/clr */
#define E1000_MGTPRC   0x040B4  /* Management Packets RX Count - R/clr */
#define E1000_MGTPDC   0x040B8  /* Management Packets Dropped Count - R/clr */
#define E1000_MGTPTC   0x040BC  /* Management Packets TX Count - R/clr */
#define E1000_TORL     0x040C0  /* Total Octets RX Low - R/clr */
#define E1000_TORH     0x040C4  /* Total Octets RX High - R/clr */
#define E1000_TOTL     0x040C8  /* Total Octets TX Low - R/clr */
#define E1000_TOTH     0x040CC  /* Total Octets TX High - R/clr */
#define E1000_TPR      0x040D0  /* Total Packets RX - R/clr */
#define E1000_TPT      0x040D4  /* Total Packets TX - R/clr */
#define E1000_PTC64    0x040D8  /* Packets TX (64 bytes) - R/clr */
#define E1000_PTC127   0x040DC  /* Packets TX (65-127 bytes) - R/clr */
#define E1000_PTC255   0x040E0  /* Packets TX (128-255 bytes) - R/clr */
#define E1000_PTC511   0x040E4  /* Packets TX (256-511 bytes) - R/clr */
#define E1000_PTC1023  0x040E8  /* Packets TX (512-1023 bytes) - R/clr */
#define E1000_PTC1522  0x040EC  /* Packets TX (1024-1522 Bytes) - R/clr */
#define E1000_MPTC     0x040F0  /* Multicast Packets TX Count - R/clr */
#define E1000_BPTC     0x040F4  /* Broadcast Packets TX Count - R/clr */
#define E1000_TSCTC    0x040F8  /* TCP Segmentation Context TX - R/clr */
#define E1000_TSCTFC   0x040FC  /* TCP Segmentation Context TX Fail - R/clr */
#define E1000_RXCSUM   0x05000  /* RX Checksum Control - RW */
#define E1000_MTA      0x05200  /* Multicast Table Array - RW Array */
#define E1000_RA       0x05400  /* Receive Address - RW Array */
#define E1000_VFTA     0x05600  /* VLAN Filter Table Array - RW Array */
#define E1000_WUC      0x05800  /* Wakeup Control - RW */
#define E1000_WUFC     0x05808  /* Wakeup Filter Control - RW */
#define E1000_WUS      0x05810  /* Wakeup Status - RO */
#define E1000_MANC     0x05820  /* Management Control - RW */
#define E1000_IPAV     0x05838  /* IP Address Valid - RW */
#define E1000_IP4AT    0x05840  /* IPv4 Address Table - RW Array */
#define E1000_IP6AT    0x05880  /* IPv6 Address Table - RW Array */
#define E1000_WUPL     0x05900  /* Wakeup Packet Length - RW */
#define E1000_WUPM     0x05A00  /* Wakeup Packet Memory - RO A */
#define E1000_FFLT     0x05F00  /* Flexible Filter Length Table - RW Array */
#define E1000_HOST_IF  0x08800  /* Host Interface */
#define E1000_FFMT     0x09000  /* Flexible Filter Mask Table - RW Array */
#define E1000_FFVT     0x09800  /* Flexible Filter Value Table - RW Array */

/* Register Set (82542)
 *
 * Some of the 82542 registers are located at different offsets than they are
 * in more current versions of the 8254x. Despite the difference in location,
 * the registers function in the same manner.
 */
#define E1000_82542_CTRL     E1000_CTRL
#define E1000_82542_CTRL_DUP E1000_CTRL_DUP
#define E1000_82542_STATUS   E1000_STATUS
#define E1000_82542_EECD     E1000_EECD
#define E1000_82542_EERD     E1000_EERD
#define E1000_82542_CTRL_EXT E1000_CTRL_EXT
#define E1000_82542_FLA      E1000_FLA
#define E1000_82542_MDIC     E1000_MDIC
#define E1000_82542_FCAL     E1000_FCAL
#define E1000_82542_FCAH     E1000_FCAH
#define E1000_82542_FCT      E1000_FCT
#define E1000_82542_VET      E1000_VET
#define E1000_82542_RA       0x00040
#define E1000_82542_ICR      E1000_ICR
#define E1000_82542_ITR      E1000_ITR
#define E1000_82542_ICS      E1000_ICS
#define E1000_82542_IMS      E1000_IMS
#define E1000_82542_IMC      E1000_IMC
#define E1000_82542_RCTL     E1000_RCTL
#define E1000_82542_RDTR     0x00108
#define E1000_82542_RDBAL    0x00110
#define E1000_82542_RDBAH    0x00114
#define E1000_82542_RDLEN    0x00118
#define E1000_82542_RDH      0x00120
#define E1000_82542_RDT      0x00128
#define E1000_82542_FCRTH    0x00160
#define E1000_82542_FCRTL    0x00168
#define E1000_82542_FCTTV    E1000_FCTTV
#define E1000_82542_TXCW     E1000_TXCW
#define E1000_82542_RXCW     E1000_RXCW
#define E1000_82542_MTA      0x00200
#define E1000_82542_TCTL     E1000_TCTL
#define E1000_82542_TIPG     E1000_TIPG
#define E1000_82542_TDBAL    0x00420
#define E1000_82542_TDBAH    0x00424
#define E1000_82542_TDLEN    0x00428
#define E1000_82542_TDH      0x00430
#define E1000_82542_TDT      0x00438
#define E1000_82542_TIDV     0x00440
#define E1000_82542_TBT      E1000_TBT
#define E1000_82542_AIT      E1000_AIT
#define E1000_82542_VFTA     0x00600
#define E1000_82542_LEDCTL   E1000_LEDCTL
#define E1000_82542_PBA      E1000_PBA
#define E1000_82542_RXDCTL   E1000_RXDCTL
#define E1000_82542_RADV     E1000_RADV
#define E1000_82542_RSRPD    E1000_RSRPD
#define E1000_82542_TXDMAC   E1000_TXDMAC
#define E1000_82542_TDFHS    E1000_TDFHS
#define E1000_82542_TDFTS    E1000_TDFTS
#define E1000_82542_TDFPC    E1000_TDFPC
#define E1000_82542_TXDCTL   E1000_TXDCTL
#define E1000_82542_TADV     E1000_TADV
#define E1000_82542_TSPMT    E1000_TSPMT
#define E1000_82542_CRCERRS  E1000_CRCERRS
#define E1000_82542_ALGNERRC E1000_ALGNERRC
#define E1000_82542_SYMERRS  E1000_SYMERRS
#define E1000_82542_RXERRC   E1000_RXERRC
#define E1000_82542_MPC      E1000_MPC
#define E1000_82542_SCC      E1000_SCC
#define E1000_82542_ECOL     E1000_ECOL
#define E1000_82542_MCC      E1000_MCC
#define E1000_82542_LATECOL  E1000_LATECOL
#define E1000_82542_COLC     E1000_COLC
#define E1000_82542_DC       E1000_DC
#define E1000_82542_TNCRS    E1000_TNCRS
#define E1000_82542_SEC      E1000_SEC
#define E1000_82542_CEXTERR  E1000_CEXTERR
#define E1000_82542_RLEC     E1000_RLEC
#define E1000_82542_XONRXC   E1000_XONRXC
#define E1000_82542_XONTXC   E1000_XONTXC
#define E1000_82542_XOFFRXC  E1000_XOFFRXC
#define E1000_82542_XOFFTXC  E1000_XOFFTXC
#define E1000_82542_FCRUC    E1000_FCRUC
#define E1000_82542_PRC64    E1000_PRC64
#define E1000_82542_PRC127   E1000_PRC127
#define E1000_82542_PRC255   E1000_PRC255
#define E1000_82542_PRC511   E1000_PRC511
#define E1000_82542_PRC1023  E1000_PRC1023
#define E1000_82542_PRC1522  E1000_PRC1522
#define E1000_82542_GPRC     E1000_GPRC
#define E1000_82542_BPRC     E1000_BPRC
#define E1000_82542_MPRC     E1000_MPRC
#define E1000_82542_GPTC     E1000_GPTC
#define E1000_82542_GORCL    E1000_GORCL
#define E1000_82542_GORCH    E1000_GORCH
#define E1000_82542_GOTCL    E1000_GOTCL
#define E1000_82542_GOTCH    E1000_GOTCH
#define E1000_82542_RNBC     E1000_RNBC
#define E1000_82542_RUC      E1000_RUC
#define E1000_82542_RFC      E1000_RFC
#define E1000_82542_ROC      E1000_ROC
#define E1000_82542_RJC      E1000_RJC
#define E1000_82542_MGTPRC   E1000_MGTPRC
#define E1000_82542_MGTPDC   E1000_MGTPDC
#define E1000_82542_MGTPTC   E1000_MGTPTC
#define E1000_82542_TORL     E1000_TORL
#define E1000_82542_TORH     E1000_TORH
#define E1000_82542_TOTL     E1000_TOTL
#define E1000_82542_TOTH     E1000_TOTH
#define E1000_82542_TPR      E1000_TPR
#define E1000_82542_TPT      E1000_TPT
#define E1000_82542_PTC64    E1000_PTC64
#define E1000_82542_PTC127   E1000_PTC127
#define E1000_82542_PTC255   E1000_PTC255
#define E1000_82542_PTC511   E1000_PTC511
#define E1000_82542_PTC1023  E1000_PTC1023
#define E1000_82542_PTC1522  E1000_PTC1522
#define E1000_82542_MPTC     E1000_MPTC
#define E1000_82542_BPTC     E1000_BPTC
#define E1000_82542_TSCTC    E1000_TSCTC
#define E1000_82542_TSCTFC   E1000_TSCTFC
#define E1000_82542_RXCSUM   E1000_RXCSUM
#define E1000_82542_WUC      E1000_WUC
#define E1000_82542_WUFC     E1000_WUFC
#define E1000_82542_WUS      E1000_WUS
#define E1000_82542_MANC     E1000_MANC
#define E1000_82542_IPAV     E1000_IPAV
#define E1000_82542_IP4AT    E1000_IP4AT
#define E1000_82542_IP6AT    E1000_IP6AT
#define E1000_82542_WUPL     E1000_WUPL
#define E1000_82542_WUPM     E1000_WUPM
#define E1000_82542_FFLT     E1000_FFLT
#define E1000_82542_TDFH     0x08010
#define E1000_82542_TDFT     0x08018
#define E1000_82542_FFMT     E1000_FFMT
#define E1000_82542_FFVT     E1000_FFVT
#define E1000_82542_HOST_IF  E1000_HOST_IF

/* Statistics counters collected by the MAC */
struct e1000_hw_stats {
    uint64_t crcerrs;
    uint64_t algnerrc;
    uint64_t symerrs;
    uint64_t rxerrc;
    uint64_t mpc;
    uint64_t scc;
    uint64_t ecol;
    uint64_t mcc;
    uint64_t latecol;
    uint64_t colc;
    uint64_t dc;
    uint64_t tncrs;
    uint64_t sec;
    uint64_t cexterr;
    uint64_t rlec;
    uint64_t xonrxc;
    uint64_t xontxc;
    uint64_t xoffrxc;
    uint64_t xofftxc;
    uint64_t fcruc;
    uint64_t prc64;
    uint64_t prc127;
    uint64_t prc255;
    uint64_t prc511;
    uint64_t prc1023;
    uint64_t prc1522;
    uint64_t gprc;
    uint64_t bprc;
    uint64_t mprc;
    uint64_t gptc;
    uint64_t gorcl;
    uint64_t gorch;
    uint64_t gotcl;
    uint64_t gotch;
    uint64_t rnbc;
    uint64_t ruc;
    uint64_t rfc;
    uint64_t roc;
    uint64_t rjc;
    uint64_t mgprc;
    uint64_t mgpdc;
    uint64_t mgptc;
    uint64_t torl;
    uint64_t torh;
    uint64_t totl;
    uint64_t toth;
    uint64_t tpr;
    uint64_t tpt;
    uint64_t ptc64;
    uint64_t ptc127;
    uint64_t ptc255;
    uint64_t ptc511;
    uint64_t ptc1023;
    uint64_t ptc1522;
    uint64_t mptc;
    uint64_t bptc;
    uint64_t tsctc;
    uint64_t tsctfc;
};

/* Structure containing variables used by the shared code (e1000_hw.c) */
struct e1000_hw {
    uint8_t *hw_addr;
    e1000_mac_type mac_type;
    e1000_phy_type phy_type;
    uint32_t phy_init_script;
    e1000_media_type media_type;
    void *back;
    e1000_fc_type fc;
    e1000_bus_speed bus_speed;
    e1000_bus_width bus_width;
    e1000_bus_type bus_type;
    struct e1000_eeprom_info eeprom;
    e1000_ms_type master_slave;
    e1000_ms_type original_master_slave;
    e1000_ffe_config ffe_config_state;
    uint32_t asf_firmware_present;
    unsigned long io_base;
    uint32_t phy_id;
    uint32_t phy_revision;
    uint32_t phy_addr;
    uint32_t original_fc;
    uint32_t txcw;
    uint32_t autoneg_failed;
    uint32_t max_frame_size;
    uint32_t min_frame_size;
    uint32_t mc_filter_type;
    uint32_t num_mc_addrs;
    uint32_t collision_delta;
    uint32_t tx_packet_delta;
    uint32_t ledctl_default;
    uint32_t ledctl_mode1;
    uint32_t ledctl_mode2;
    uint16_t phy_spd_default;
    uint16_t autoneg_advertised;
    uint16_t pci_cmd_word;
    uint16_t fc_high_water;
    uint16_t fc_low_water;
    uint16_t fc_pause_time;
    uint16_t current_ifs_val;
    uint16_t ifs_min_val;
    uint16_t ifs_max_val;
    uint16_t ifs_step_size;
    uint16_t ifs_ratio;
    uint16_t device_id;
    uint16_t vendor_id;
    uint16_t subsystem_id;
    uint16_t subsystem_vendor_id;
    uint8_t revision_id;
    uint8_t autoneg;
    uint8_t mdix;
    uint8_t forced_speed_duplex;
    uint8_t wait_autoneg_complete;
    uint8_t dma_fairness;
    uint8_t mac_addr[NODE_ADDRESS_SIZE];
    uint8_t perm_mac_addr[NODE_ADDRESS_SIZE];
    boolean_t disable_polarity_correction;
    boolean_t speed_downgraded;
    e1000_dsp_config dsp_config_state;
    boolean_t get_link_status;
    boolean_t serdes_link_down;
    boolean_t tbi_compatibility_en;
    boolean_t tbi_compatibility_on;
    boolean_t phy_reset_disable;
    boolean_t fc_send_xon;
    boolean_t fc_strict_ieee;
    boolean_t report_tx_early;
    boolean_t adaptive_ifs;
    boolean_t ifs_params_forced;
    boolean_t in_ifs_mode;
};


#define E1000_EEPROM_SWDPIN0   0x0001   /* SWDPIN 0 EEPROM Value */
#define E1000_EEPROM_LED_LOGIC 0x0020   /* Led Logic Word */

/* Register Bit Masks */
/* Device Control */
#define E1000_CTRL_FD       0x00000001  /* Full duplex.0=half; 1=full */
#define E1000_CTRL_BEM      0x00000002  /* Endian Mode.0=little,1=big */
#define E1000_CTRL_PRIOR    0x00000004  /* Priority on PCI. 0=rx,1=fair */
#define E1000_CTRL_LRST     0x00000008  /* Link reset. 0=normal,1=reset */
#define E1000_CTRL_TME      0x00000010  /* Test mode. 0=normal,1=test */
#define E1000_CTRL_SLE      0x00000020  /* Serial Link on 0=dis,1=en */
#define E1000_CTRL_ASDE     0x00000020  /* Auto-speed detect enable */
#define E1000_CTRL_SLU      0x00000040  /* Set link up (Force Link) */
#define E1000_CTRL_ILOS     0x00000080  /* Invert Loss-Of Signal */
#define E1000_CTRL_SPD_SEL  0x00000300  /* Speed Select Mask */
#define E1000_CTRL_SPD_10   0x00000000  /* Force 10Mb */
#define E1000_CTRL_SPD_100  0x00000100  /* Force 100Mb */
#define E1000_CTRL_SPD_1000 0x00000200  /* Force 1Gb */
#define E1000_CTRL_BEM32    0x00000400  /* Big Endian 32 mode */
#define E1000_CTRL_FRCSPD   0x00000800  /* Force Speed */
#define E1000_CTRL_FRCDPX   0x00001000  /* Force Duplex */
#define E1000_CTRL_SWDPIN0  0x00040000  /* SWDPIN 0 value */
#define E1000_CTRL_SWDPIN1  0x00080000  /* SWDPIN 1 value */
#define E1000_CTRL_SWDPIN2  0x00100000  /* SWDPIN 2 value */
#define E1000_CTRL_SWDPIN3  0x00200000  /* SWDPIN 3 value */
#define E1000_CTRL_SWDPIO0  0x00400000  /* SWDPIN 0 Input or output */
#define E1000_CTRL_SWDPIO1  0x00800000  /* SWDPIN 1 input or output */
#define E1000_CTRL_SWDPIO2  0x01000000  /* SWDPIN 2 input or output */
#define E1000_CTRL_SWDPIO3  0x02000000  /* SWDPIN 3 input or output */
#define E1000_CTRL_RST      0x04000000  /* Global reset */
#define E1000_CTRL_RFCE     0x08000000  /* Receive Flow Control enable */
#define E1000_CTRL_TFCE     0x10000000  /* Transmit flow control enable */
#define E1000_CTRL_RTE      0x20000000  /* Routing tag enable */
#define E1000_CTRL_VME      0x40000000  /* IEEE VLAN mode enable */
#define E1000_CTRL_PHY_RST  0x80000000  /* PHY Reset */

/* Device Status */
#define E1000_STATUS_FD         0x00000001      /* Full duplex.0=half,1=full */
#define E1000_STATUS_LU         0x00000002      /* Link up.0=no,1=link */
#define E1000_STATUS_FUNC_MASK  0x0000000C      /* PCI Function Mask */
#define E1000_STATUS_FUNC_0     0x00000000      /* Function 0 */
#define E1000_STATUS_FUNC_1     0x00000004      /* Function 1 */
#define E1000_STATUS_TXOFF      0x00000010      /* transmission paused */
#define E1000_STATUS_TBIMODE    0x00000020      /* TBI mode */
#define E1000_STATUS_SPEED_MASK 0x000000C0
#define E1000_STATUS_SPEED_10   0x00000000      /* Speed 10Mb/s */
#define E1000_STATUS_SPEED_100  0x00000040      /* Speed 100Mb/s */
#define E1000_STATUS_SPEED_1000 0x00000080      /* Speed 1000Mb/s */
#define E1000_STATUS_ASDV       0x00000300      /* Auto speed detect value */
#define E1000_STATUS_MTXCKOK    0x00000400      /* MTX clock running OK */
#define E1000_STATUS_PCI66      0x00000800      /* In 66Mhz slot */
#define E1000_STATUS_BUS64      0x00001000      /* In 64 bit slot */
#define E1000_STATUS_PCIX_MODE  0x00002000      /* PCI-X mode */
#define E1000_STATUS_PCIX_SPEED 0x0000C000      /* PCI-X bus speed */

/* Constants used to intrepret the masked PCI-X bus speed. */
#define E1000_STATUS_PCIX_SPEED_66  0x00000000 /* PCI-X bus speed  50-66 MHz */
#define E1000_STATUS_PCIX_SPEED_100 0x00004000 /* PCI-X bus speed  66-100 MHz */
#define E1000_STATUS_PCIX_SPEED_133 0x00008000 /* PCI-X bus speed 100-133 MHz */

/* EEPROM/Flash Control */
#define E1000_EECD_SK        0x00000001 /* EEPROM Clock */
#define E1000_EECD_CS        0x00000002 /* EEPROM Chip Select */
#define E1000_EECD_DI        0x00000004 /* EEPROM Data In */
#define E1000_EECD_DO        0x00000008 /* EEPROM Data Out */
#define E1000_EECD_FWE_MASK  0x00000030
#define E1000_EECD_FWE_DIS   0x00000010 /* Disable FLASH writes */
#define E1000_EECD_FWE_EN    0x00000020 /* Enable FLASH writes */
#define E1000_EECD_FWE_SHIFT 4
#define E1000_EECD_REQ       0x00000040 /* EEPROM Access Request */
#define E1000_EECD_GNT       0x00000080 /* EEPROM Access Grant */
#define E1000_EECD_PRES      0x00000100 /* EEPROM Present */
#define E1000_EECD_SIZE      0x00000200 /* EEPROM Size (0=64 word 1=256 word) */
#define E1000_EECD_ADDR_BITS 0x00000400 /* EEPROM Addressing bits based on type
                                         * (0-small, 1-large) */
#define E1000_EECD_TYPE      0x00002000 /* EEPROM Type (1-SPI, 0-Microwire) */
#ifndef E1000_EEPROM_GRANT_ATTEMPTS
#define E1000_EEPROM_GRANT_ATTEMPTS 1000 /* EEPROM # attempts to gain grant */
#endif

/* EEPROM Read */
#define E1000_EERD_START      0x00000001 /* Start Read */
#define E1000_EERD_DONE       0x00000010 /* Read Done */
#define E1000_EERD_ADDR_SHIFT 8
#define E1000_EERD_ADDR_MASK  0x0000FF00 /* Read Address */
#define E1000_EERD_DATA_SHIFT 16
#define E1000_EERD_DATA_MASK  0xFFFF0000 /* Read Data */

/* SPI EEPROM Status Register */
#define EEPROM_STATUS_RDY_SPI  0x01
#define EEPROM_STATUS_WEN_SPI  0x02
#define EEPROM_STATUS_BP0_SPI  0x04
#define EEPROM_STATUS_BP1_SPI  0x08
#define EEPROM_STATUS_WPEN_SPI 0x80

/* Extended Device Control */
#define E1000_CTRL_EXT_GPI0_EN   0x00000001 /* Maps SDP4 to GPI0 */
#define E1000_CTRL_EXT_GPI1_EN   0x00000002 /* Maps SDP5 to GPI1 */
#define E1000_CTRL_EXT_PHYINT_EN E1000_CTRL_EXT_GPI1_EN
#define E1000_CTRL_EXT_GPI2_EN   0x00000004 /* Maps SDP6 to GPI2 */
#define E1000_CTRL_EXT_GPI3_EN   0x00000008 /* Maps SDP7 to GPI3 */
#define E1000_CTRL_EXT_SDP4_DATA 0x00000010 /* Value of SW Defineable Pin 4 */
#define E1000_CTRL_EXT_SDP5_DATA 0x00000020 /* Value of SW Defineable Pin 5 */
#define E1000_CTRL_EXT_PHY_INT   E1000_CTRL_EXT_SDP5_DATA
#define E1000_CTRL_EXT_SDP6_DATA 0x00000040 /* Value of SW Defineable Pin 6 */
#define E1000_CTRL_EXT_SDP7_DATA 0x00000080 /* Value of SW Defineable Pin 7 */
#define E1000_CTRL_EXT_SDP4_DIR  0x00000100 /* Direction of SDP4 0=in 1=out */
#define E1000_CTRL_EXT_SDP5_DIR  0x00000200 /* Direction of SDP5 0=in 1=out */
#define E1000_CTRL_EXT_SDP6_DIR  0x00000400 /* Direction of SDP6 0=in 1=out */
#define E1000_CTRL_EXT_SDP7_DIR  0x00000800 /* Direction of SDP7 0=in 1=out */
#define E1000_CTRL_EXT_ASDCHK    0x00001000 /* Initiate an ASD sequence */
#define E1000_CTRL_EXT_EE_RST    0x00002000 /* Reinitialize from EEPROM */
#define E1000_CTRL_EXT_IPS       0x00004000 /* Invert Power State */
#define E1000_CTRL_EXT_SPD_BYPS  0x00008000 /* Speed Select Bypass */
#define E1000_CTRL_EXT_LINK_MODE_MASK 0x00C00000
#define E1000_CTRL_EXT_LINK_MODE_GMII 0x00000000
#define E1000_CTRL_EXT_LINK_MODE_TBI  0x00C00000
#define E1000_CTRL_EXT_WR_WMARK_MASK  0x03000000
#define E1000_CTRL_EXT_WR_WMARK_256   0x00000000
#define E1000_CTRL_EXT_WR_WMARK_320   0x01000000
#define E1000_CTRL_EXT_WR_WMARK_384   0x02000000
#define E1000_CTRL_EXT_WR_WMARK_448   0x03000000

/* MDI Control */
#define E1000_MDIC_DATA_MASK 0x0000FFFF
#define E1000_MDIC_REG_MASK  0x001F0000
#define E1000_MDIC_REG_SHIFT 16
#define E1000_MDIC_PHY_MASK  0x03E00000
#define E1000_MDIC_PHY_SHIFT 21
#define E1000_MDIC_OP_WRITE  0x04000000
#define E1000_MDIC_OP_READ   0x08000000
#define E1000_MDIC_READY     0x10000000
#define E1000_MDIC_INT_EN    0x20000000
#define E1000_MDIC_ERROR     0x40000000

/* LED Control */
#define E1000_LEDCTL_LED0_MODE_MASK       0x0000000F
#define E1000_LEDCTL_LED0_MODE_SHIFT      0
#define E1000_LEDCTL_LED0_IVRT            0x00000040
#define E1000_LEDCTL_LED0_BLINK           0x00000080
#define E1000_LEDCTL_LED1_MODE_MASK       0x00000F00
#define E1000_LEDCTL_LED1_MODE_SHIFT      8
#define E1000_LEDCTL_LED1_IVRT            0x00004000
#define E1000_LEDCTL_LED1_BLINK           0x00008000
#define E1000_LEDCTL_LED2_MODE_MASK       0x000F0000
#define E1000_LEDCTL_LED2_MODE_SHIFT      16
#define E1000_LEDCTL_LED2_IVRT            0x00400000
#define E1000_LEDCTL_LED2_BLINK           0x00800000
#define E1000_LEDCTL_LED3_MODE_MASK       0x0F000000
#define E1000_LEDCTL_LED3_MODE_SHIFT      24
#define E1000_LEDCTL_LED3_IVRT            0x40000000
#define E1000_LEDCTL_LED3_BLINK           0x80000000

#define E1000_LEDCTL_MODE_LINK_10_1000  0x0
#define E1000_LEDCTL_MODE_LINK_100_1000 0x1
#define E1000_LEDCTL_MODE_LINK_UP       0x2
#define E1000_LEDCTL_MODE_ACTIVITY      0x3
#define E1000_LEDCTL_MODE_LINK_ACTIVITY 0x4
#define E1000_LEDCTL_MODE_LINK_10       0x5
#define E1000_LEDCTL_MODE_LINK_100      0x6
#define E1000_LEDCTL_MODE_LINK_1000     0x7
#define E1000_LEDCTL_MODE_PCIX_MODE     0x8
#define E1000_LEDCTL_MODE_FULL_DUPLEX   0x9
#define E1000_LEDCTL_MODE_COLLISION     0xA
#define E1000_LEDCTL_MODE_BUS_SPEED     0xB
#define E1000_LEDCTL_MODE_BUS_SIZE      0xC
#define E1000_LEDCTL_MODE_PAUSED        0xD
#define E1000_LEDCTL_MODE_LED_ON        0xE
#define E1000_LEDCTL_MODE_LED_OFF       0xF

/* Receive Address */
#define E1000_RAH_AV  0x80000000        /* Receive descriptor valid */

/* Interrupt Cause Read */
#define E1000_ICR_TXDW          0x00000001 /* Transmit desc written back */
#define E1000_ICR_TXQE          0x00000002 /* Transmit Queue empty */
#define E1000_ICR_LSC           0x00000004 /* Link Status Change */
#define E1000_ICR_RXSEQ         0x00000008 /* rx sequence error */
#define E1000_ICR_RXDMT0        0x00000010 /* rx desc min. threshold (0) */
#define E1000_ICR_RXO           0x00000040 /* rx overrun */
#define E1000_ICR_RXT0          0x00000080 /* rx timer intr (ring 0) */
#define E1000_ICR_MDAC          0x00000200 /* MDIO access complete */
#define E1000_ICR_RXCFG         0x00000400 /* RX /c/ ordered set */
#define E1000_ICR_GPI_EN0       0x00000800 /* GP Int 0 */
#define E1000_ICR_GPI_EN1       0x00001000 /* GP Int 1 */
#define E1000_ICR_GPI_EN2       0x00002000 /* GP Int 2 */
#define E1000_ICR_GPI_EN3       0x00004000 /* GP Int 3 */
#define E1000_ICR_TXD_LOW       0x00008000
#define E1000_ICR_SRPD          0x00010000

/* Interrupt Cause Set */
#define E1000_ICS_TXDW      E1000_ICR_TXDW      /* Transmit desc written back */
#define E1000_ICS_TXQE      E1000_ICR_TXQE      /* Transmit Queue empty */
#define E1000_ICS_LSC       E1000_ICR_LSC       /* Link Status Change */
#define E1000_ICS_RXSEQ     E1000_ICR_RXSEQ     /* rx sequence error */
#define E1000_ICS_RXDMT0    E1000_ICR_RXDMT0    /* rx desc min. threshold */
#define E1000_ICS_RXO       E1000_ICR_RXO       /* rx overrun */
#define E1000_ICS_RXT0      E1000_ICR_RXT0      /* rx timer intr */
#define E1000_ICS_MDAC      E1000_ICR_MDAC      /* MDIO access complete */
#define E1000_ICS_RXCFG     E1000_ICR_RXCFG     /* RX /c/ ordered set */
#define E1000_ICS_GPI_EN0   E1000_ICR_GPI_EN0   /* GP Int 0 */
#define E1000_ICS_GPI_EN1   E1000_ICR_GPI_EN1   /* GP Int 1 */
#define E1000_ICS_GPI_EN2   E1000_ICR_GPI_EN2   /* GP Int 2 */
#define E1000_ICS_GPI_EN3   E1000_ICR_GPI_EN3   /* GP Int 3 */
#define E1000_ICS_TXD_LOW   E1000_ICR_TXD_LOW
#define E1000_ICS_SRPD      E1000_ICR_SRPD

/* Interrupt Mask Set */
#define E1000_IMS_TXDW      E1000_ICR_TXDW      /* Transmit desc written back */
#define E1000_IMS_TXQE      E1000_ICR_TXQE      /* Transmit Queue empty */
#define E1000_IMS_LSC       E1000_ICR_LSC       /* Link Status Change */
#define E1000_IMS_RXSEQ     E1000_ICR_RXSEQ     /* rx sequence error */
#define E1000_IMS_RXDMT0    E1000_ICR_RXDMT0    /* rx desc min. threshold */
#define E1000_IMS_RXO       E1000_ICR_RXO       /* rx overrun */
#define E1000_IMS_RXT0      E1000_ICR_RXT0      /* rx timer intr */
#define E1000_IMS_MDAC      E1000_ICR_MDAC      /* MDIO access complete */
#define E1000_IMS_RXCFG     E1000_ICR_RXCFG     /* RX /c/ ordered set */
#define E1000_IMS_GPI_EN0   E1000_ICR_GPI_EN0   /* GP Int 0 */
#define E1000_IMS_GPI_EN1   E1000_ICR_GPI_EN1   /* GP Int 1 */
#define E1000_IMS_GPI_EN2   E1000_ICR_GPI_EN2   /* GP Int 2 */
#define E1000_IMS_GPI_EN3   E1000_ICR_GPI_EN3   /* GP Int 3 */
#define E1000_IMS_TXD_LOW   E1000_ICR_TXD_LOW
#define E1000_IMS_SRPD      E1000_ICR_SRPD

/* Interrupt Mask Clear */
#define E1000_IMC_TXDW      E1000_ICR_TXDW      /* Transmit desc written back */
#define E1000_IMC_TXQE      E1000_ICR_TXQE      /* Transmit Queue empty */
#define E1000_IMC_LSC       E1000_ICR_LSC       /* Link Status Change */
#define E1000_IMC_RXSEQ     E1000_ICR_RXSEQ     /* rx sequence error */
#define E1000_IMC_RXDMT0    E1000_ICR_RXDMT0    /* rx desc min. threshold */
#define E1000_IMC_RXO       E1000_ICR_RXO       /* rx overrun */
#define E1000_IMC_RXT0      E1000_ICR_RXT0      /* rx timer intr */
#define E1000_IMC_MDAC      E1000_ICR_MDAC      /* MDIO access complete */
#define E1000_IMC_RXCFG     E1000_ICR_RXCFG     /* RX /c/ ordered set */
#define E1000_IMC_GPI_EN0   E1000_ICR_GPI_EN0   /* GP Int 0 */
#define E1000_IMC_GPI_EN1   E1000_ICR_GPI_EN1   /* GP Int 1 */
#define E1000_IMC_GPI_EN2   E1000_ICR_GPI_EN2   /* GP Int 2 */
#define E1000_IMC_GPI_EN3   E1000_ICR_GPI_EN3   /* GP Int 3 */
#define E1000_IMC_TXD_LOW   E1000_ICR_TXD_LOW
#define E1000_IMC_SRPD      E1000_ICR_SRPD

/* Receive Control */
#define E1000_RCTL_RST            0x00000001    /* Software reset */
#define E1000_RCTL_EN             0x00000002    /* enable */
#define E1000_RCTL_SBP            0x00000004    /* store bad packet */
#define E1000_RCTL_UPE            0x00000008    /* unicast promiscuous enable */
#define E1000_RCTL_MPE            0x00000010    /* multicast promiscuous enab */
#define E1000_RCTL_LPE            0x00000020    /* long packet enable */
#define E1000_RCTL_LBM_NO         0x00000000    /* no loopback mode */
#define E1000_RCTL_LBM_MAC        0x00000040    /* MAC loopback mode */
#define E1000_RCTL_LBM_SLP        0x00000080    /* serial link loopback mode */
#define E1000_RCTL_LBM_TCVR       0x000000C0    /* tcvr loopback mode */
#define E1000_RCTL_RDMTS_HALF     0x00000000    /* rx desc min threshold size */
#define E1000_RCTL_RDMTS_QUAT     0x00000100    /* rx desc min threshold size */
#define E1000_RCTL_RDMTS_EIGTH    0x00000200    /* rx desc min threshold size */
#define E1000_RCTL_MO_SHIFT       12            /* multicast offset shift */
#define E1000_RCTL_MO_0           0x00000000    /* multicast offset 11:0 */
#define E1000_RCTL_MO_1           0x00001000    /* multicast offset 12:1 */
#define E1000_RCTL_MO_2           0x00002000    /* multicast offset 13:2 */
#define E1000_RCTL_MO_3           0x00003000    /* multicast offset 15:4 */
#define E1000_RCTL_MDR            0x00004000    /* multicast desc ring 0 */
#define E1000_RCTL_BAM            0x00008000    /* broadcast enable */
/* these buffer sizes are valid if E1000_RCTL_BSEX is 0 */
#define E1000_RCTL_SZ_2048        0x00000000    /* rx buffer size 2048 */
#define E1000_RCTL_SZ_1024        0x00010000    /* rx buffer size 1024 */
#define E1000_RCTL_SZ_512         0x00020000    /* rx buffer size 512 */
#define E1000_RCTL_SZ_256         0x00030000    /* rx buffer size 256 */
/* these buffer sizes are valid if E1000_RCTL_BSEX is 1 */
#define E1000_RCTL_SZ_16384       0x00010000    /* rx buffer size 16384 */
#define E1000_RCTL_SZ_8192        0x00020000    /* rx buffer size 8192 */
#define E1000_RCTL_SZ_4096        0x00030000    /* rx buffer size 4096 */
#define E1000_RCTL_VFE            0x00040000    /* vlan filter enable */
#define E1000_RCTL_CFIEN          0x00080000    /* canonical form enable */
#define E1000_RCTL_CFI            0x00100000    /* canonical form indicator */
#define E1000_RCTL_DPF            0x00400000    /* discard pause frames */
#define E1000_RCTL_PMCF           0x00800000    /* pass MAC control frames */
#define E1000_RCTL_BSEX           0x02000000    /* Buffer size extension */
#define E1000_RCTL_SECRC          0x04000000    /* Strip Ethernet CRC */

/* Receive Descriptor */
#define E1000_RDT_DELAY 0x0000ffff      /* Delay timer (1=1024us) */
#define E1000_RDT_FPDB  0x80000000      /* Flush descriptor block */
#define E1000_RDLEN_LEN 0x0007ff80      /* descriptor length */
#define E1000_RDH_RDH   0x0000ffff      /* receive descriptor head */
#define E1000_RDT_RDT   0x0000ffff      /* receive descriptor tail */

/* Flow Control */
#define E1000_FCRTH_RTH  0x0000FFF8     /* Mask Bits[15:3] for RTH */
#define E1000_FCRTH_XFCE 0x80000000     /* External Flow Control Enable */
#define E1000_FCRTL_RTL  0x0000FFF8     /* Mask Bits[15:3] for RTL */
#define E1000_FCRTL_XONE 0x80000000     /* Enable XON frame transmission */

/* Receive Descriptor Control */
#define E1000_RXDCTL_PTHRESH 0x0000003F /* RXDCTL Prefetch Threshold */
#define E1000_RXDCTL_HTHRESH 0x00003F00 /* RXDCTL Host Threshold */
#define E1000_RXDCTL_WTHRESH 0x003F0000 /* RXDCTL Writeback Threshold */
#define E1000_RXDCTL_GRAN    0x01000000 /* RXDCTL Granularity */

/* Transmit Descriptor Control */
#define E1000_TXDCTL_PTHRESH 0x000000FF /* TXDCTL Prefetch Threshold */
#define E1000_TXDCTL_HTHRESH 0x0000FF00 /* TXDCTL Host Threshold */
#define E1000_TXDCTL_WTHRESH 0x00FF0000 /* TXDCTL Writeback Threshold */
#define E1000_TXDCTL_GRAN    0x01000000 /* TXDCTL Granularity */
#define E1000_TXDCTL_LWTHRESH 0xFE000000 /* TXDCTL Low Threshold */
#define E1000_TXDCTL_FULL_TX_DESC_WB 0x01010000 /* GRAN=1, WTHRESH=1 */

/* Transmit Configuration Word */
#define E1000_TXCW_FD         0x00000020        /* TXCW full duplex */
#define E1000_TXCW_HD         0x00000040        /* TXCW half duplex */
#define E1000_TXCW_PAUSE      0x00000080        /* TXCW sym pause request */
#define E1000_TXCW_ASM_DIR    0x00000100        /* TXCW astm pause direction */
#define E1000_TXCW_PAUSE_MASK 0x00000180        /* TXCW pause request mask */
#define E1000_TXCW_RF         0x00003000        /* TXCW remote fault */
#define E1000_TXCW_NP         0x00008000        /* TXCW next page */
#define E1000_TXCW_CW         0x0000ffff        /* TxConfigWord mask */
#define E1000_TXCW_TXC        0x40000000        /* Transmit Config control */
#define E1000_TXCW_ANE        0x80000000        /* Auto-neg enable */

/* Receive Configuration Word */
#define E1000_RXCW_CW    0x0000ffff     /* RxConfigWord mask */
#define E1000_RXCW_NC    0x04000000     /* Receive config no carrier */
#define E1000_RXCW_IV    0x08000000     /* Receive config invalid */
#define E1000_RXCW_CC    0x10000000     /* Receive config change */
#define E1000_RXCW_C     0x20000000     /* Receive config */
#define E1000_RXCW_SYNCH 0x40000000     /* Receive config synch */
#define E1000_RXCW_ANC   0x80000000     /* Auto-neg complete */

/* Transmit Control */
#define E1000_TCTL_RST    0x00000001    /* software reset */
#define E1000_TCTL_EN     0x00000002    /* enable tx */
#define E1000_TCTL_BCE    0x00000004    /* busy check enable */
#define E1000_TCTL_PSP    0x00000008    /* pad short packets */
#define E1000_TCTL_CT     0x00000ff0    /* collision threshold */
#define E1000_TCTL_COLD   0x003ff000    /* collision distance */
#define E1000_TCTL_SWXOFF 0x00400000    /* SW Xoff transmission */
#define E1000_TCTL_PBE    0x00800000    /* Packet Burst Enable */
#define E1000_TCTL_RTLC   0x01000000    /* Re-transmit on late collision */
#define E1000_TCTL_NRTU   0x02000000    /* No Re-transmit on underrun */

/* Receive Checksum Control */
#define E1000_RXCSUM_PCSS_MASK 0x000000FF   /* Packet Checksum Start */
#define E1000_RXCSUM_IPOFL     0x00000100   /* IPv4 checksum offload */
#define E1000_RXCSUM_TUOFL     0x00000200   /* TCP / UDP checksum offload */
#define E1000_RXCSUM_IPV6OFL   0x00000400   /* IPv6 checksum offload */

/* Definitions for power management and wakeup registers */
/* Wake Up Control */
#define E1000_WUC_APME       0x00000001 /* APM Enable */
#define E1000_WUC_PME_EN     0x00000002 /* PME Enable */
#define E1000_WUC_PME_STATUS 0x00000004 /* PME Status */
#define E1000_WUC_APMPME     0x00000008 /* Assert PME on APM Wakeup */
#define E1000_WUC_SPM        0x80000000 /* Enable SPM */

/* Wake Up Filter Control */
#define E1000_WUFC_LNKC 0x00000001 /* Link Status Change Wakeup Enable */
#define E1000_WUFC_MAG  0x00000002 /* Magic Packet Wakeup Enable */
#define E1000_WUFC_EX   0x00000004 /* Directed Exact Wakeup Enable */
#define E1000_WUFC_MC   0x00000008 /* Directed Multicast Wakeup Enable */
#define E1000_WUFC_BC   0x00000010 /* Broadcast Wakeup Enable */
#define E1000_WUFC_ARP  0x00000020 /* ARP Request Packet Wakeup Enable */
#define E1000_WUFC_IPV4 0x00000040 /* Directed IPv4 Packet Wakeup Enable */
#define E1000_WUFC_IPV6 0x00000080 /* Directed IPv6 Packet Wakeup Enable */
#define E1000_WUFC_FLX0 0x00010000 /* Flexible Filter 0 Enable */
#define E1000_WUFC_FLX1 0x00020000 /* Flexible Filter 1 Enable */
#define E1000_WUFC_FLX2 0x00040000 /* Flexible Filter 2 Enable */
#define E1000_WUFC_FLX3 0x00080000 /* Flexible Filter 3 Enable */
#define E1000_WUFC_ALL_FILTERS 0x000F00FF /* Mask for all wakeup filters */
#define E1000_WUFC_FLX_OFFSET 16       /* Offset to the Flexible Filters bits */
#define E1000_WUFC_FLX_FILTERS 0x000F0000 /* Mask for the 4 flexible filters */

/* Wake Up Status */
#define E1000_WUS_LNKC 0x00000001 /* Link Status Changed */
#define E1000_WUS_MAG  0x00000002 /* Magic Packet Received */
#define E1000_WUS_EX   0x00000004 /* Directed Exact Received */
#define E1000_WUS_MC   0x00000008 /* Directed Multicast Received */
#define E1000_WUS_BC   0x00000010 /* Broadcast Received */
#define E1000_WUS_ARP  0x00000020 /* ARP Request Packet Received */
#define E1000_WUS_IPV4 0x00000040 /* Directed IPv4 Packet Wakeup Received */
#define E1000_WUS_IPV6 0x00000080 /* Directed IPv6 Packet Wakeup Received */
#define E1000_WUS_FLX0 0x00010000 /* Flexible Filter 0 Match */
#define E1000_WUS_FLX1 0x00020000 /* Flexible Filter 1 Match */
#define E1000_WUS_FLX2 0x00040000 /* Flexible Filter 2 Match */
#define E1000_WUS_FLX3 0x00080000 /* Flexible Filter 3 Match */
#define E1000_WUS_FLX_FILTERS 0x000F0000 /* Mask for the 4 flexible filters */

/* Management Control */
#define E1000_MANC_SMBUS_EN      0x00000001 /* SMBus Enabled - RO */
#define E1000_MANC_ASF_EN        0x00000002 /* ASF Enabled - RO */
#define E1000_MANC_R_ON_FORCE    0x00000004 /* Reset on Force TCO - RO */
#define E1000_MANC_RMCP_EN       0x00000100 /* Enable RCMP 026Fh Filtering */
#define E1000_MANC_0298_EN       0x00000200 /* Enable RCMP 0298h Filtering */
#define E1000_MANC_IPV4_EN       0x00000400 /* Enable IPv4 */
#define E1000_MANC_IPV6_EN       0x00000800 /* Enable IPv6 */
#define E1000_MANC_SNAP_EN       0x00001000 /* Accept LLC/SNAP */
#define E1000_MANC_ARP_EN        0x00002000 /* Enable ARP Request Filtering */
#define E1000_MANC_NEIGHBOR_EN   0x00004000 /* Enable Neighbor Discovery
                                             * Filtering */
#define E1000_MANC_TCO_RESET     0x00010000 /* TCO Reset Occurred */
#define E1000_MANC_RCV_TCO_EN    0x00020000 /* Receive TCO Packets Enabled */
#define E1000_MANC_REPORT_STATUS 0x00040000 /* Status Reporting Enabled */
#define E1000_MANC_EN_MAC_ADDR_FILTER   0x00100000 /* Enable MAC address
                                                    * filtering */
#define E1000_MANC_EN_MNG2HOST   0x00200000 /* Enable MNG packets to host
                                             * memory */
#define E1000_MANC_SMB_REQ       0x01000000 /* SMBus Request */
#define E1000_MANC_SMB_GNT       0x02000000 /* SMBus Grant */
#define E1000_MANC_SMB_CLK_IN    0x04000000 /* SMBus Clock In */
#define E1000_MANC_SMB_DATA_IN   0x08000000 /* SMBus Data In */
#define E1000_MANC_SMB_DATA_OUT  0x10000000 /* SMBus Data Out */
#define E1000_MANC_SMB_CLK_OUT   0x20000000 /* SMBus Clock Out */

#define E1000_MANC_SMB_DATA_OUT_SHIFT  28 /* SMBus Data Out Shift */
#define E1000_MANC_SMB_CLK_OUT_SHIFT   29 /* SMBus Clock Out Shift */

/* Wake Up Packet Length */
#define E1000_WUPL_LENGTH_MASK 0x0FFF   /* Only the lower 12 bits are valid */

#define E1000_MDALIGN          4096

/* EEPROM Commands - Microwire */
#define EEPROM_READ_OPCODE_MICROWIRE  0x6  /* EEPROM read opcode */
#define EEPROM_WRITE_OPCODE_MICROWIRE 0x5  /* EEPROM write opcode */
#define EEPROM_ERASE_OPCODE_MICROWIRE 0x7  /* EEPROM erase opcode */
#define EEPROM_EWEN_OPCODE_MICROWIRE  0x13 /* EEPROM erase/write enable */
#define EEPROM_EWDS_OPCODE_MICROWIRE  0x10 /* EEPROM erast/write disable */

/* EEPROM Commands - SPI */
#define EEPROM_MAX_RETRY_SPI    5000 /* Max wait of 5ms, for RDY signal */
#define EEPROM_READ_OPCODE_SPI  0x3  /* EEPROM read opcode */
#define EEPROM_WRITE_OPCODE_SPI 0x2  /* EEPROM write opcode */
#define EEPROM_A8_OPCODE_SPI    0x8  /* opcode bit-3 = address bit-8 */
#define EEPROM_WREN_OPCODE_SPI  0x6  /* EEPROM set Write Enable latch */
#define EEPROM_WRDI_OPCODE_SPI  0x4  /* EEPROM reset Write Enable latch */
#define EEPROM_RDSR_OPCODE_SPI  0x5  /* EEPROM read Status register */
#define EEPROM_WRSR_OPCODE_SPI  0x1  /* EEPROM write Status register */

/* EEPROM Size definitions */
#define EEPROM_SIZE_16KB        0x1800
#define EEPROM_SIZE_8KB         0x1400
#define EEPROM_SIZE_4KB         0x1000
#define EEPROM_SIZE_2KB         0x0C00
#define EEPROM_SIZE_1KB         0x0800
#define EEPROM_SIZE_512B        0x0400
#define EEPROM_SIZE_128B        0x0000
#define EEPROM_SIZE_MASK        0x1C00

/* EEPROM Word Offsets */
#define EEPROM_COMPAT                 0x0003
#define EEPROM_ID_LED_SETTINGS        0x0004
#define EEPROM_SERDES_AMPLITUDE       0x0006 /* For SERDES output amplitude adjustment. */
#define EEPROM_PHY_CLASS_WORD         0x0007
#define EEPROM_INIT_CONTROL1_REG      0x000A
#define EEPROM_INIT_CONTROL2_REG      0x000F
#define EEPROM_INIT_CONTROL3_PORT_B   0x0014
#define EEPROM_INIT_CONTROL3_PORT_A   0x0024
#define EEPROM_CFG                    0x0012
#define EEPROM_FLASH_VERSION          0x0032
#define EEPROM_CHECKSUM_REG           0x003F

/* Word definitions for ID LED Settings */
#define ID_LED_RESERVED_0000 0x0000
#define ID_LED_RESERVED_FFFF 0xFFFF
#define ID_LED_DEFAULT       ((ID_LED_OFF1_ON2 << 12) | \
                              (ID_LED_OFF1_OFF2 << 8) | \
                              (ID_LED_DEF1_DEF2 << 4) | \
                              (ID_LED_DEF1_DEF2))
#define ID_LED_DEF1_DEF2     0x1
#define ID_LED_DEF1_ON2      0x2
#define ID_LED_DEF1_OFF2     0x3
#define ID_LED_ON1_DEF2      0x4
#define ID_LED_ON1_ON2       0x5
#define ID_LED_ON1_OFF2      0x6
#define ID_LED_OFF1_DEF2     0x7
#define ID_LED_OFF1_ON2      0x8
#define ID_LED_OFF1_OFF2     0x9

#define IGP_ACTIVITY_LED_MASK   0xFFFFF0FF
#define IGP_ACTIVITY_LED_ENABLE 0x0300
#define IGP_LED3_MODE           0x07000000


/* Mask bits for SERDES amplitude adjustment in Word 6 of the EEPROM */
#define EEPROM_SERDES_AMPLITUDE_MASK  0x000F

/* Mask bit for PHY class in Word 7 of the EEPROM */
#define EEPROM_PHY_CLASS_A   0x8000

/* Mask bits for fields in Word 0x0a of the EEPROM */
#define EEPROM_WORD0A_ILOS   0x0010
#define EEPROM_WORD0A_SWDPIO 0x01E0
#define EEPROM_WORD0A_LRST   0x0200
#define EEPROM_WORD0A_FD     0x0400
#define EEPROM_WORD0A_66MHZ  0x0800

/* Mask bits for fields in Word 0x0f of the EEPROM */
#define EEPROM_WORD0F_PAUSE_MASK 0x3000
#define EEPROM_WORD0F_PAUSE      0x1000
#define EEPROM_WORD0F_ASM_DIR    0x2000
#define EEPROM_WORD0F_ANE        0x0800
#define EEPROM_WORD0F_SWPDIO_EXT 0x00F0

/* For checksumming, the sum of all words in the EEPROM should equal 0xBABA. */
#define EEPROM_SUM 0xBABA

/* EEPROM Map defines (WORD OFFSETS)*/
#define EEPROM_NODE_ADDRESS_BYTE_0 0
#define EEPROM_PBA_BYTE_1          8

#define EEPROM_RESERVED_WORD          0xFFFF

/* EEPROM Map Sizes (Byte Counts) */
#define PBA_SIZE 4

/* Collision related configuration parameters */
#define E1000_COLLISION_THRESHOLD       15
#define E1000_CT_SHIFT                  4
#define E1000_COLLISION_DISTANCE        64
#define E1000_FDX_COLLISION_DISTANCE    E1000_COLLISION_DISTANCE
#define E1000_HDX_COLLISION_DISTANCE    E1000_COLLISION_DISTANCE
#define E1000_COLD_SHIFT                12

/* Number of Transmit and Receive Descriptors must be a multiple of 8 */
#define REQ_TX_DESCRIPTOR_MULTIPLE  8
#define REQ_RX_DESCRIPTOR_MULTIPLE  8

/* Default values for the transmit IPG register */
#define DEFAULT_82542_TIPG_IPGT        10
#define DEFAULT_82543_TIPG_IPGT_FIBER  9
#define DEFAULT_82543_TIPG_IPGT_COPPER 8

#define E1000_TIPG_IPGT_MASK  0x000003FF
#define E1000_TIPG_IPGR1_MASK 0x000FFC00
#define E1000_TIPG_IPGR2_MASK 0x3FF00000

#define DEFAULT_82542_TIPG_IPGR1 2
#define DEFAULT_82543_TIPG_IPGR1 8
#define E1000_TIPG_IPGR1_SHIFT  10

#define DEFAULT_82542_TIPG_IPGR2 10
#define DEFAULT_82543_TIPG_IPGR2 6
#define E1000_TIPG_IPGR2_SHIFT  20

#define E1000_TXDMAC_DPP 0x00000001

/* Adaptive IFS defines */
#define TX_THRESHOLD_START     8
#define TX_THRESHOLD_INCREMENT 10
#define TX_THRESHOLD_DECREMENT 1
#define TX_THRESHOLD_STOP      190
#define TX_THRESHOLD_DISABLE   0
#define TX_THRESHOLD_TIMER_MS  10000
#define MIN_NUM_XMITS          1000
#define IFS_MAX                80
#define IFS_STEP               10
#define IFS_MIN                40
#define IFS_RATIO              4

/* PBA constants */
#define E1000_PBA_16K 0x0010    /* 16KB, default TX allocation */
#define E1000_PBA_22K 0x0016
#define E1000_PBA_24K 0x0018
#define E1000_PBA_30K 0x001E
#define E1000_PBA_40K 0x0028
#define E1000_PBA_48K 0x0030    /* 48KB, default RX allocation */

/* Flow Control Constants */
#define FLOW_CONTROL_ADDRESS_LOW  0x00C28001
#define FLOW_CONTROL_ADDRESS_HIGH 0x00000100
#define FLOW_CONTROL_TYPE         0x8808

/* The historical defaults for the flow control values are given below. */
#define FC_DEFAULT_HI_THRESH        (0x8000)    /* 32KB */
#define FC_DEFAULT_LO_THRESH        (0x4000)    /* 16KB */
#define FC_DEFAULT_TX_TIMER         (0x100)     /* ~130 us */

/* PCIX Config space */
#define PCIX_COMMAND_REGISTER    0xE6
#define PCIX_STATUS_REGISTER_LO  0xE8
#define PCIX_STATUS_REGISTER_HI  0xEA

#define PCIX_COMMAND_MMRBC_MASK      0x000C
#define PCIX_COMMAND_MMRBC_SHIFT     0x2
#define PCIX_STATUS_HI_MMRBC_MASK    0x0060
#define PCIX_STATUS_HI_MMRBC_SHIFT   0x5
#define PCIX_STATUS_HI_MMRBC_4K      0x3
#define PCIX_STATUS_HI_MMRBC_2K      0x2


/* Number of bits required to shift right the "pause" bits from the
 * EEPROM (bits 13:12) to the "pause" (bits 8:7) field in the TXCW register.
 */
#define PAUSE_SHIFT 5

/* Number of bits required to shift left the "SWDPIO" bits from the
 * EEPROM (bits 8:5) to the "SWDPIO" (bits 25:22) field in the CTRL register.
 */
#define SWDPIO_SHIFT 17

/* Number of bits required to shift left the "SWDPIO_EXT" bits from the
 * EEPROM word F (bits 7:4) to the bits 11:8 of The Extended CTRL register.
 */
#define SWDPIO__EXT_SHIFT 4

/* Number of bits required to shift left the "ILOS" bit from the EEPROM
 * (bit 4) to the "ILOS" (bit 7) field in the CTRL register.
 */
#define ILOS_SHIFT  3


#define RECEIVE_BUFFER_ALIGN_SIZE  (256)

/* Number of milliseconds we wait for auto-negotiation to complete */
#define LINK_UP_TIMEOUT             500

#define E1000_TX_BUFFER_SIZE ((uint32_t)1514)

/* The carrier extension symbol, as received by the NIC. */
#define CARRIER_EXTENSION   0x0F

/* TBI_ACCEPT macro definition:
 *
 * This macro requires:
 *      adapter = a pointer to struct e1000_hw
 *      status = the 8 bit status field of the RX descriptor with EOP set
 *      error = the 8 bit error field of the RX descriptor with EOP set
 *      length = the sum of all the length fields of the RX descriptors that
 *               make up the current frame
 *      last_byte = the last byte of the frame DMAed by the hardware
 *      max_frame_length = the maximum frame length we want to accept.
 *      min_frame_length = the minimum frame length we want to accept.
 *
 * This macro is a conditional that should be used in the interrupt
 * handler's Rx processing routine when RxErrors have been detected.
 *
 * Typical use:
 *  ...
 *  if (TBI_ACCEPT) {
 *      accept_frame = TRUE;
 *      e1000_tbi_adjust_stats(adapter, MacAddress);
 *      frame_length--;
 *  } else {
 *      accept_frame = FALSE;
 *  }
 *  ...
 */

#define TBI_ACCEPT(adapter, status, errors, length, last_byte) \
    ((adapter)->tbi_compatibility_on && \
     (((errors) & E1000_RXD_ERR_FRAME_ERR_MASK) == E1000_RXD_ERR_CE) && \
     ((last_byte) == CARRIER_EXTENSION) && \
     (((status) & E1000_RXD_STAT_VP) ? \
          (((length) > ((adapter)->min_frame_size - VLAN_TAG_SIZE)) && \
           ((length) <= ((adapter)->max_frame_size + 1))) : \
          (((length) > (adapter)->min_frame_size) && \
           ((length) <= ((adapter)->max_frame_size + VLAN_TAG_SIZE + 1)))))


/* Structures, enums, and macros for the PHY */

/* Bit definitions for the Management Data IO (MDIO) and Management Data
 * Clock (MDC) pins in the Device Control Register.
 */
#define E1000_CTRL_PHY_RESET_DIR  E1000_CTRL_SWDPIO0
#define E1000_CTRL_PHY_RESET      E1000_CTRL_SWDPIN0
#define E1000_CTRL_MDIO_DIR       E1000_CTRL_SWDPIO2
#define E1000_CTRL_MDIO           E1000_CTRL_SWDPIN2
#define E1000_CTRL_MDC_DIR        E1000_CTRL_SWDPIO3
#define E1000_CTRL_MDC            E1000_CTRL_SWDPIN3
#define E1000_CTRL_PHY_RESET_DIR4 E1000_CTRL_EXT_SDP4_DIR
#define E1000_CTRL_PHY_RESET4     E1000_CTRL_EXT_SDP4_DATA

/* PHY 1000 MII Register/Bit Definitions */
/* PHY Registers defined by IEEE */
#define PHY_CTRL         0x00 /* Control Register */
#define PHY_STATUS       0x01 /* Status Regiser */
#define PHY_ID1          0x02 /* Phy Id Reg (word 1) */
#define PHY_ID2          0x03 /* Phy Id Reg (word 2) */
#define PHY_AUTONEG_ADV  0x04 /* Autoneg Advertisement */
#define PHY_LP_ABILITY   0x05 /* Link Partner Ability (Base Page) */
#define PHY_AUTONEG_EXP  0x06 /* Autoneg Expansion Reg */
#define PHY_NEXT_PAGE_TX 0x07 /* Next Page TX */
#define PHY_LP_NEXT_PAGE 0x08 /* Link Partner Next Page */
#define PHY_1000T_CTRL   0x09 /* 1000Base-T Control Reg */
#define PHY_1000T_STATUS 0x0A /* 1000Base-T Status Reg */
#define PHY_EXT_STATUS   0x0F /* Extended Status Reg */

/* M88E1000 Specific Registers */
#define M88E1000_PHY_SPEC_CTRL     0x10  /* PHY Specific Control Register */
#define M88E1000_PHY_SPEC_STATUS   0x11  /* PHY Specific Status Register */
#define M88E1000_INT_ENABLE        0x12  /* Interrupt Enable Register */
#define M88E1000_INT_STATUS        0x13  /* Interrupt Status Register */
#define M88E1000_EXT_PHY_SPEC_CTRL 0x14  /* Extended PHY Specific Control */
#define M88E1000_RX_ERR_CNTR       0x15  /* Receive Error Counter */

#define M88E1000_PHY_EXT_CTRL      0x1A  /* PHY extend control register */
#define M88E1000_PHY_PAGE_SELECT   0x1D  /* Reg 29 for page number setting */
#define M88E1000_PHY_GEN_CONTROL   0x1E  /* Its meaning depends on reg 29 */
#define M88E1000_PHY_VCO_REG_BIT8  0x100 /* Bits 8 & 11 are adjusted for */
#define M88E1000_PHY_VCO_REG_BIT11 0x800    /* improved BER performance */

#define IGP01E1000_IEEE_REGS_PAGE  0x0000
#define IGP01E1000_IEEE_RESTART_AUTONEG 0x3300
#define IGP01E1000_IEEE_FORCE_GIGA      0x0140

/* IGP01E1000 Specific Registers */
#define IGP01E1000_PHY_PORT_CONFIG 0x10 /* PHY Specific Port Config Register */
#define IGP01E1000_PHY_PORT_STATUS 0x11 /* PHY Specific Status Register */
#define IGP01E1000_PHY_PORT_CTRL   0x12 /* PHY Specific Control Register */
#define IGP01E1000_PHY_LINK_HEALTH 0x13 /* PHY Link Health Register */
#define IGP01E1000_GMII_FIFO       0x14 /* GMII FIFO Register */
#define IGP01E1000_PHY_CHANNEL_QUALITY 0x15 /* PHY Channel Quality Register */
#define IGP01E1000_PHY_PAGE_SELECT     0x1F /* PHY Page Select Core Register */

/* IGP01E1000 AGC Registers - stores the cable length values*/
#define IGP01E1000_PHY_AGC_A        0x1172
#define IGP01E1000_PHY_AGC_B        0x1272
#define IGP01E1000_PHY_AGC_C        0x1472
#define IGP01E1000_PHY_AGC_D        0x1872

/* IGP01E1000 DSP Reset Register */
#define IGP01E1000_PHY_DSP_RESET   0x1F33
#define IGP01E1000_PHY_DSP_SET     0x1F71
#define IGP01E1000_PHY_DSP_FFE     0x1F35

#define IGP01E1000_PHY_CHANNEL_NUM    4
#define IGP01E1000_PHY_AGC_PARAM_A    0x1171
#define IGP01E1000_PHY_AGC_PARAM_B    0x1271
#define IGP01E1000_PHY_AGC_PARAM_C    0x1471
#define IGP01E1000_PHY_AGC_PARAM_D    0x1871

#define IGP01E1000_PHY_EDAC_MU_INDEX        0xC000
#define IGP01E1000_PHY_EDAC_SIGN_EXT_9_BITS 0x8000

#define IGP01E1000_PHY_ANALOG_TX_STATE      0x2890
#define IGP01E1000_PHY_ANALOG_CLASS_A       0x2000
#define IGP01E1000_PHY_FORCE_ANALOG_ENABLE  0x0004
#define IGP01E1000_PHY_DSP_FFE_CM_CP        0x0069

#define IGP01E1000_PHY_DSP_FFE_DEFAULT      0x002A
/* IGP01E1000 PCS Initialization register - stores the polarity status when
 * speed = 1000 Mbps. */
#define IGP01E1000_PHY_PCS_INIT_REG  0x00B4
#define IGP01E1000_PHY_PCS_CTRL_REG  0x00B5

#define IGP01E1000_ANALOG_REGS_PAGE  0x20C0

#define MAX_PHY_REG_ADDRESS 0x1F        /* 5 bit address bus (0-0x1F) */
#define MAX_PHY_MULTI_PAGE_REG  0xF     /*Registers that are equal on all pages*/
/* PHY Control Register */
#define MII_CR_SPEED_SELECT_MSB 0x0040  /* bits 6,13: 10=1000, 01=100, 00=10 */
#define MII_CR_COLL_TEST_ENABLE 0x0080  /* Collision test enable */
#define MII_CR_FULL_DUPLEX      0x0100  /* FDX =1, half duplex =0 */
#define MII_CR_RESTART_AUTO_NEG 0x0200  /* Restart auto negotiation */
#define MII_CR_ISOLATE          0x0400  /* Isolate PHY from MII */
#define MII_CR_POWER_DOWN       0x0800  /* Power down */
#define MII_CR_AUTO_NEG_EN      0x1000  /* Auto Neg Enable */
#define MII_CR_SPEED_SELECT_LSB 0x2000  /* bits 6,13: 10=1000, 01=100, 00=10 */
#define MII_CR_LOOPBACK         0x4000  /* 0 = normal, 1 = loopback */
#define MII_CR_RESET            0x8000  /* 0 = normal, 1 = PHY reset */

/* PHY Status Register */
#define MII_SR_EXTENDED_CAPS     0x0001 /* Extended register capabilities */
#define MII_SR_JABBER_DETECT     0x0002 /* Jabber Detected */
#define MII_SR_LINK_STATUS       0x0004 /* Link Status 1 = link */
#define MII_SR_AUTONEG_CAPS      0x0008 /* Auto Neg Capable */
#define MII_SR_REMOTE_FAULT      0x0010 /* Remote Fault Detect */
#define MII_SR_AUTONEG_COMPLETE  0x0020 /* Auto Neg Complete */
#define MII_SR_PREAMBLE_SUPPRESS 0x0040 /* Preamble may be suppressed */
#define MII_SR_EXTENDED_STATUS   0x0100 /* Ext. status info in Reg 0x0F */
#define MII_SR_100T2_HD_CAPS     0x0200 /* 100T2 Half Duplex Capable */
#define MII_SR_100T2_FD_CAPS     0x0400 /* 100T2 Full Duplex Capable */
#define MII_SR_10T_HD_CAPS       0x0800 /* 10T   Half Duplex Capable */
#define MII_SR_10T_FD_CAPS       0x1000 /* 10T   Full Duplex Capable */
#define MII_SR_100X_HD_CAPS      0x2000 /* 100X  Half Duplex Capable */
#define MII_SR_100X_FD_CAPS      0x4000 /* 100X  Full Duplex Capable */
#define MII_SR_100T4_CAPS        0x8000 /* 100T4 Capable */

/* Autoneg Advertisement Register */
#define NWAY_AR_SELECTOR_FIELD 0x0001   /* indicates IEEE 802.3 CSMA/CD */
#define NWAY_AR_10T_HD_CAPS    0x0020   /* 10T   Half Duplex Capable */
#define NWAY_AR_10T_FD_CAPS    0x0040   /* 10T   Full Duplex Capable */
#define NWAY_AR_100TX_HD_CAPS  0x0080   /* 100TX Half Duplex Capable */
#define NWAY_AR_100TX_FD_CAPS  0x0100   /* 100TX Full Duplex Capable */
#define NWAY_AR_100T4_CAPS     0x0200   /* 100T4 Capable */
#define NWAY_AR_PAUSE          0x0400   /* Pause operation desired */
#define NWAY_AR_ASM_DIR        0x0800   /* Asymmetric Pause Direction bit */
#define NWAY_AR_REMOTE_FAULT   0x2000   /* Remote Fault detected */
#define NWAY_AR_NEXT_PAGE      0x8000   /* Next Page ability supported */

/* Link Partner Ability Register (Base Page) */
#define NWAY_LPAR_SELECTOR_FIELD 0x0000 /* LP protocol selector field */
#define NWAY_LPAR_10T_HD_CAPS    0x0020 /* LP is 10T   Half Duplex Capable */
#define NWAY_LPAR_10T_FD_CAPS    0x0040 /* LP is 10T   Full Duplex Capable */
#define NWAY_LPAR_100TX_HD_CAPS  0x0080 /* LP is 100TX Half Duplex Capable */
#define NWAY_LPAR_100TX_FD_CAPS  0x0100 /* LP is 100TX Full Duplex Capable */
#define NWAY_LPAR_100T4_CAPS     0x0200 /* LP is 100T4 Capable */
#define NWAY_LPAR_PAUSE          0x0400 /* LP Pause operation desired */
#define NWAY_LPAR_ASM_DIR        0x0800 /* LP Asymmetric Pause Direction bit */
#define NWAY_LPAR_REMOTE_FAULT   0x2000 /* LP has detected Remote Fault */
#define NWAY_LPAR_ACKNOWLEDGE    0x4000 /* LP has rx'd link code word */
#define NWAY_LPAR_NEXT_PAGE      0x8000 /* Next Page ability supported */

/* Autoneg Expansion Register */
#define NWAY_ER_LP_NWAY_CAPS      0x0001 /* LP has Auto Neg Capability */
#define NWAY_ER_PAGE_RXD          0x0002 /* LP is 10T   Half Duplex Capable */
#define NWAY_ER_NEXT_PAGE_CAPS    0x0004 /* LP is 10T   Full Duplex Capable */
#define NWAY_ER_LP_NEXT_PAGE_CAPS 0x0008 /* LP is 100TX Half Duplex Capable */
#define NWAY_ER_PAR_DETECT_FAULT  0x0010 /* LP is 100TX Full Duplex Capable */

/* Next Page TX Register */
#define NPTX_MSG_CODE_FIELD 0x0001 /* NP msg code or unformatted data */
#define NPTX_TOGGLE         0x0800 /* Toggles between exchanges
                                    * of different NP
                                    */
#define NPTX_ACKNOWLDGE2    0x1000 /* 1 = will comply with msg
                                    * 0 = cannot comply with msg
                                    */
#define NPTX_MSG_PAGE       0x2000 /* formatted(1)/unformatted(0) pg */
#define NPTX_NEXT_PAGE      0x8000 /* 1 = addition NP will follow
                                    * 0 = sending last NP
                                    */

/* Link Partner Next Page Register */
#define LP_RNPR_MSG_CODE_FIELD 0x0001 /* NP msg code or unformatted data */
#define LP_RNPR_TOGGLE         0x0800 /* Toggles between exchanges
                                       * of different NP
                                       */
#define LP_RNPR_ACKNOWLDGE2    0x1000 /* 1 = will comply with msg
                                       * 0 = cannot comply with msg
                                       */
#define LP_RNPR_MSG_PAGE       0x2000  /* formatted(1)/unformatted(0) pg */
#define LP_RNPR_ACKNOWLDGE     0x4000  /* 1 = ACK / 0 = NO ACK */
#define LP_RNPR_NEXT_PAGE      0x8000  /* 1 = addition NP will follow
                                        * 0 = sending last NP
                                        */

/* 1000BASE-T Control Register */
#define CR_1000T_ASYM_PAUSE      0x0080 /* Advertise asymmetric pause bit */
#define CR_1000T_HD_CAPS         0x0100 /* Advertise 1000T HD capability */
#define CR_1000T_FD_CAPS         0x0200 /* Advertise 1000T FD capability  */
#define CR_1000T_REPEATER_DTE    0x0400 /* 1=Repeater/switch device port */
                                        /* 0=DTE device */
#define CR_1000T_MS_VALUE        0x0800 /* 1=Configure PHY as Master */
                                        /* 0=Configure PHY as Slave */
#define CR_1000T_MS_ENABLE       0x1000 /* 1=Master/Slave manual config value */
                                        /* 0=Automatic Master/Slave config */
#define CR_1000T_TEST_MODE_NORMAL 0x0000 /* Normal Operation */
#define CR_1000T_TEST_MODE_1     0x2000 /* Transmit Waveform test */
#define CR_1000T_TEST_MODE_2     0x4000 /* Master Transmit Jitter test */
#define CR_1000T_TEST_MODE_3     0x6000 /* Slave Transmit Jitter test */
#define CR_1000T_TEST_MODE_4     0x8000 /* Transmitter Distortion test */

/* 1000BASE-T Status Register */
#define SR_1000T_IDLE_ERROR_CNT   0x00FF /* Num idle errors since last read */
#define SR_1000T_ASYM_PAUSE_DIR   0x0100 /* LP asymmetric pause direction bit */
#define SR_1000T_LP_HD_CAPS       0x0400 /* LP is 1000T HD capable */
#define SR_1000T_LP_FD_CAPS       0x0800 /* LP is 1000T FD capable */
#define SR_1000T_REMOTE_RX_STATUS 0x1000 /* Remote receiver OK */
#define SR_1000T_LOCAL_RX_STATUS  0x2000 /* Local receiver OK */
#define SR_1000T_MS_CONFIG_RES    0x4000 /* 1=Local TX is Master, 0=Slave */
#define SR_1000T_MS_CONFIG_FAULT  0x8000 /* Master/Slave config fault */
#define SR_1000T_REMOTE_RX_STATUS_SHIFT          12
#define SR_1000T_LOCAL_RX_STATUS_SHIFT           13
#define SR_1000T_PHY_EXCESSIVE_IDLE_ERR_COUNT    5
#define FFE_IDLE_ERR_COUNT_TIMEOUT_20            20
#define FFE_IDLE_ERR_COUNT_TIMEOUT_100           100

/* Extended Status Register */
#define IEEE_ESR_1000T_HD_CAPS 0x1000 /* 1000T HD capable */
#define IEEE_ESR_1000T_FD_CAPS 0x2000 /* 1000T FD capable */
#define IEEE_ESR_1000X_HD_CAPS 0x4000 /* 1000X HD capable */
#define IEEE_ESR_1000X_FD_CAPS 0x8000 /* 1000X FD capable */

#define PHY_TX_POLARITY_MASK   0x0100 /* register 10h bit 8 (polarity bit) */
#define PHY_TX_NORMAL_POLARITY 0      /* register 10h bit 8 (normal polarity) */

#define AUTO_POLARITY_DISABLE  0x0010 /* register 11h bit 4 */
                                      /* (0=enable, 1=disable) */

/* M88E1000 PHY Specific Control Register */
#define M88E1000_PSCR_JABBER_DISABLE    0x0001 /* 1=Jabber Function disabled */
#define M88E1000_PSCR_POLARITY_REVERSAL 0x0002 /* 1=Polarity Reversal enabled */
#define M88E1000_PSCR_SQE_TEST          0x0004 /* 1=SQE Test enabled */
#define M88E1000_PSCR_CLK125_DISABLE    0x0010 /* 1=CLK125 low,
                                                * 0=CLK125 toggling
                                                */
#define M88E1000_PSCR_MDI_MANUAL_MODE  0x0000  /* MDI Crossover Mode bits 6:5 */
                                               /* Manual MDI configuration */
#define M88E1000_PSCR_MDIX_MANUAL_MODE 0x0020  /* Manual MDIX configuration */
#define M88E1000_PSCR_AUTO_X_1000T     0x0040  /* 1000BASE-T: Auto crossover,
                                                *  100BASE-TX/10BASE-T:
                                                *  MDI Mode
                                                */
#define M88E1000_PSCR_AUTO_X_MODE      0x0060  /* Auto crossover enabled
                                                * all speeds.
                                                */
#define M88E1000_PSCR_10BT_EXT_DIST_ENABLE 0x0080
                                        /* 1=Enable Extended 10BASE-T distance
                                         * (Lower 10BASE-T RX Threshold)
                                         * 0=Normal 10BASE-T RX Threshold */
#define M88E1000_PSCR_MII_5BIT_ENABLE      0x0100
                                        /* 1=5-Bit interface in 100BASE-TX
                                         * 0=MII interface in 100BASE-TX */
#define M88E1000_PSCR_SCRAMBLER_DISABLE    0x0200 /* 1=Scrambler disable */
#define M88E1000_PSCR_FORCE_LINK_GOOD      0x0400 /* 1=Force link good */
#define M88E1000_PSCR_ASSERT_CRS_ON_TX     0x0800 /* 1=Assert CRS on Transmit */

#define M88E1000_PSCR_POLARITY_REVERSAL_SHIFT    1
#define M88E1000_PSCR_AUTO_X_MODE_SHIFT          5
#define M88E1000_PSCR_10BT_EXT_DIST_ENABLE_SHIFT 7

/* M88E1000 PHY Specific Status Register */
#define M88E1000_PSSR_JABBER             0x0001 /* 1=Jabber */
#define M88E1000_PSSR_REV_POLARITY       0x0002 /* 1=Polarity reversed */
#define M88E1000_PSSR_DOWNSHIFT          0x0020 /* 1=Downshifted */
#define M88E1000_PSSR_MDIX               0x0040 /* 1=MDIX; 0=MDI */
#define M88E1000_PSSR_CABLE_LENGTH       0x0380 /* 0=<50M;1=50-80M;2=80-110M;
                                            * 3=110-140M;4=>140M */
#define M88E1000_PSSR_LINK               0x0400 /* 1=Link up, 0=Link down */
#define M88E1000_PSSR_SPD_DPLX_RESOLVED  0x0800 /* 1=Speed & Duplex resolved */
#define M88E1000_PSSR_PAGE_RCVD          0x1000 /* 1=Page received */
#define M88E1000_PSSR_DPLX               0x2000 /* 1=Duplex 0=Half Duplex */
#define M88E1000_PSSR_SPEED              0xC000 /* Speed, bits 14:15 */
#define M88E1000_PSSR_10MBS              0x0000 /* 00=10Mbs */
#define M88E1000_PSSR_100MBS             0x4000 /* 01=100Mbs */
#define M88E1000_PSSR_1000MBS            0x8000 /* 10=1000Mbs */

#define M88E1000_PSSR_REV_POLARITY_SHIFT 1
#define M88E1000_PSSR_DOWNSHIFT_SHIFT    5
#define M88E1000_PSSR_MDIX_SHIFT         6
#define M88E1000_PSSR_CABLE_LENGTH_SHIFT 7

/* M88E1000 Extended PHY Specific Control Register */
#define M88E1000_EPSCR_FIBER_LOOPBACK 0x4000 /* 1=Fiber loopback */
#define M88E1000_EPSCR_DOWN_NO_IDLE   0x8000 /* 1=Lost lock detect enabled.
                                              * Will assert lost lock and bring
                                              * link down if idle not seen
                                              * within 1ms in 1000BASE-T
                                              */
/* Number of times we will attempt to autonegotiate before downshifting if we
 * are the master */
#define M88E1000_EPSCR_MASTER_DOWNSHIFT_MASK 0x0C00
#define M88E1000_EPSCR_MASTER_DOWNSHIFT_1X   0x0000
#define M88E1000_EPSCR_MASTER_DOWNSHIFT_2X   0x0400
#define M88E1000_EPSCR_MASTER_DOWNSHIFT_3X   0x0800
#define M88E1000_EPSCR_MASTER_DOWNSHIFT_4X   0x0C00
/* Number of times we will attempt to autonegotiate before downshifting if we
 * are the slave */
#define M88E1000_EPSCR_SLAVE_DOWNSHIFT_MASK  0x0300
#define M88E1000_EPSCR_SLAVE_DOWNSHIFT_DIS   0x0000
#define M88E1000_EPSCR_SLAVE_DOWNSHIFT_1X    0x0100
#define M88E1000_EPSCR_SLAVE_DOWNSHIFT_2X    0x0200
#define M88E1000_EPSCR_SLAVE_DOWNSHIFT_3X    0x0300
#define M88E1000_EPSCR_TX_CLK_2_5     0x0060 /* 2.5 MHz TX_CLK */
#define M88E1000_EPSCR_TX_CLK_25      0x0070 /* 25  MHz TX_CLK */
#define M88E1000_EPSCR_TX_CLK_0       0x0000 /* NO  TX_CLK */

/* IGP01E1000 Specific Port Config Register - R/W */
#define IGP01E1000_PSCFR_AUTO_MDIX_PAR_DETECT  0x0010
#define IGP01E1000_PSCFR_PRE_EN                0x0020
#define IGP01E1000_PSCFR_SMART_SPEED           0x0080
#define IGP01E1000_PSCFR_DISABLE_TPLOOPBACK    0x0100
#define IGP01E1000_PSCFR_DISABLE_JABBER        0x0400
#define IGP01E1000_PSCFR_DISABLE_TRANSMIT      0x2000

/* IGP01E1000 Specific Port Status Register - R/O */
#define IGP01E1000_PSSR_AUTONEG_FAILED         0x0001 /* RO LH SC */
#define IGP01E1000_PSSR_POLARITY_REVERSED      0x0002
#define IGP01E1000_PSSR_CABLE_LENGTH           0x007C
#define IGP01E1000_PSSR_FULL_DUPLEX            0x0200
#define IGP01E1000_PSSR_LINK_UP                0x0400
#define IGP01E1000_PSSR_MDIX                   0x0800
#define IGP01E1000_PSSR_SPEED_MASK             0xC000 /* speed bits mask */
#define IGP01E1000_PSSR_SPEED_10MBPS           0x4000
#define IGP01E1000_PSSR_SPEED_100MBPS          0x8000
#define IGP01E1000_PSSR_SPEED_1000MBPS         0xC000
#define IGP01E1000_PSSR_CABLE_LENGTH_SHIFT     0x0002 /* shift right 2 */
#define IGP01E1000_PSSR_MDIX_SHIFT             0x000B /* shift right 11 */

/* IGP01E1000 Specific Port Control Register - R/W */
#define IGP01E1000_PSCR_TP_LOOPBACK            0x0010
#define IGP01E1000_PSCR_CORRECT_NC_SCMBLR      0x0200
#define IGP01E1000_PSCR_TEN_CRS_SELECT         0x0400
#define IGP01E1000_PSCR_FLIP_CHIP              0x0800
#define IGP01E1000_PSCR_AUTO_MDIX              0x1000
#define IGP01E1000_PSCR_FORCE_MDI_MDIX         0x2000 /* 0-MDI, 1-MDIX */

/* IGP01E1000 Specific Port Link Health Register */
#define IGP01E1000_PLHR_SS_DOWNGRADE           0x8000
#define IGP01E1000_PLHR_GIG_SCRAMBLER_ERROR    0x4000
#define IGP01E1000_PLHR_MASTER_FAULT           0x2000
#define IGP01E1000_PLHR_MASTER_RESOLUTION      0x1000
#define IGP01E1000_PLHR_GIG_REM_RCVR_NOK       0x0800 /* LH */
#define IGP01E1000_PLHR_IDLE_ERROR_CNT_OFLOW   0x0400 /* LH */
#define IGP01E1000_PLHR_DATA_ERR_1             0x0200 /* LH */
#define IGP01E1000_PLHR_DATA_ERR_0             0x0100
#define IGP01E1000_PLHR_AUTONEG_FAULT          0x0040
#define IGP01E1000_PLHR_AUTONEG_ACTIVE         0x0010
#define IGP01E1000_PLHR_VALID_CHANNEL_D        0x0008
#define IGP01E1000_PLHR_VALID_CHANNEL_C        0x0004
#define IGP01E1000_PLHR_VALID_CHANNEL_B        0x0002
#define IGP01E1000_PLHR_VALID_CHANNEL_A        0x0001

/* IGP01E1000 Channel Quality Register */
#define IGP01E1000_MSE_CHANNEL_D        0x000F
#define IGP01E1000_MSE_CHANNEL_C        0x00F0
#define IGP01E1000_MSE_CHANNEL_B        0x0F00
#define IGP01E1000_MSE_CHANNEL_A        0xF000

/* IGP01E1000 DSP reset macros */
#define DSP_RESET_ENABLE     0x0
#define DSP_RESET_DISABLE    0x2
#define E1000_MAX_DSP_RESETS 10

/* IGP01E1000 AGC Registers */

#define IGP01E1000_AGC_LENGTH_SHIFT 7         /* Coarse - 13:11, Fine - 10:7 */

/* 7 bits (3 Coarse + 4 Fine) --> 128 optional values */
#define IGP01E1000_AGC_LENGTH_TABLE_SIZE 128

/* The precision of the length is +/- 10 meters */
#define IGP01E1000_AGC_RANGE    10

/* IGP01E1000 PCS Initialization register */
/* bits 3:6 in the PCS registers stores the channels polarity */
#define IGP01E1000_PHY_POLARITY_MASK    0x0078

/* IGP01E1000 GMII FIFO Register */
#define IGP01E1000_GMII_FLEX_SPD               0x10 /* Enable flexible speed
                                                     * on Link-Up */
#define IGP01E1000_GMII_SPD                    0x20 /* Enable SPD */

/* IGP01E1000 Analog Register */
#define IGP01E1000_ANALOG_SPARE_FUSE_STATUS       0x20D1
#define IGP01E1000_ANALOG_FUSE_STATUS             0x20D0
#define IGP01E1000_ANALOG_FUSE_CONTROL            0x20DC
#define IGP01E1000_ANALOG_FUSE_BYPASS             0x20DE

#define IGP01E1000_ANALOG_FUSE_POLY_MASK            0xF000
#define IGP01E1000_ANALOG_FUSE_FINE_MASK            0x0F80
#define IGP01E1000_ANALOG_FUSE_COARSE_MASK          0x0070
#define IGP01E1000_ANALOG_SPARE_FUSE_ENABLED        0x0100
#define IGP01E1000_ANALOG_FUSE_ENABLE_SW_CONTROL    0x0002

#define IGP01E1000_ANALOG_FUSE_COARSE_THRESH        0x0040
#define IGP01E1000_ANALOG_FUSE_COARSE_10            0x0010
#define IGP01E1000_ANALOG_FUSE_FINE_1               0x0080
#define IGP01E1000_ANALOG_FUSE_FINE_10              0x0500

/* Bit definitions for valid PHY IDs. */
#define M88E1000_E_PHY_ID  0x01410C50
#define M88E1000_I_PHY_ID  0x01410C30
#define M88E1011_I_PHY_ID  0x01410C20
#define IGP01E1000_I_PHY_ID  0x02A80380
#define M88E1000_12_PHY_ID M88E1000_E_PHY_ID
#define M88E1000_14_PHY_ID M88E1000_E_PHY_ID
#define M88E1011_I_REV_4   0x04

/* Miscellaneous PHY bit definitions. */
#define PHY_PREAMBLE        0xFFFFFFFF
#define PHY_SOF             0x01
#define PHY_OP_READ         0x02
#define PHY_OP_WRITE        0x01
#define PHY_TURNAROUND      0x02
#define PHY_PREAMBLE_SIZE   32
#define MII_CR_SPEED_1000   0x0040
#define MII_CR_SPEED_100    0x2000
#define MII_CR_SPEED_10     0x0000
#define E1000_PHY_ADDRESS   0x01
#define PHY_AUTO_NEG_TIME   45  /* 4.5 Seconds */
#define PHY_FORCE_TIME      20  /* 2.0 Seconds */
#define PHY_REVISION_MASK   0xFFFFFFF0
#define DEVICE_SPEED_MASK   0x00000300  /* Device Ctrl Reg Speed Mask */
#define REG4_SPEED_MASK     0x01E0
#define REG9_SPEED_MASK     0x0300
#define ADVERTISE_10_HALF   0x0001
#define ADVERTISE_10_FULL   0x0002
#define ADVERTISE_100_HALF  0x0004
#define ADVERTISE_100_FULL  0x0008
#define ADVERTISE_1000_HALF 0x0010
#define ADVERTISE_1000_FULL 0x0020
#define AUTONEG_ADVERTISE_SPEED_DEFAULT 0x002F  /* Everything but 1000-Half */
#define AUTONEG_ADVERTISE_10_100_ALL    0x000F /* All 10/100 speeds*/
#define AUTONEG_ADVERTISE_10_ALL        0x0003 /* 10Mbps Full & Half speeds*/

#endif /* _E1000_HW_H_ */
