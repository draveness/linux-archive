/* Raytheon wireless LAN PCMCIA card driver for Linux 
   A  PCMCIA client driver for the Raylink wireless network card
   Written by Corey Thomas
*/

#ifndef RAYLINK_H

struct beacon_rx {
    struct mac_header mac;
    UCHAR timestamp[8];
    UCHAR beacon_intvl[2];
    UCHAR capability[2];
    UCHAR elements[sizeof(struct essid_element) 
                  + sizeof(struct rates_element)
                  + sizeof(struct freq_hop_element) 
                  + sizeof(struct japan_call_sign_element)
                  + sizeof(struct tim_element)];
};

/* Return values for get_free{,_tx}_ccs */
#define ECCSFULL  (-1)
#define ECCSBUSY  (-2)
#define ECARDGONE (-3)

typedef struct ray_dev_t {
    int card_status;
    int authentication_state;
    dev_node_t  node;
    window_handle_t amem_handle;   /* handle to window for attribute memory  */
    window_handle_t rmem_handle;   /* handle to window for rx buffer on card */
    UCHAR *sram;                   /* pointer to beginning of shared RAM     */
    UCHAR *amem;                   /* pointer to attribute mem window        */
    UCHAR *rmem;                   /* pointer to receive buffer window       */
    dev_link_t *finder;            /* pointer back to dev_link_t for card    */
    struct timer_list timer;
    long tx_ccs_lock;
    long ccs_lock;
    int   dl_param_ccs;
    union {
        struct b4_startup_params b4;
        struct b5_startup_params b5;
    } sparm;
    int timeout_flag;
    UCHAR supported_rates[8];
    UCHAR japan_call_sign[12];
    struct startup_res_6 startup_res;
    int num_multi;
    /* Network parameters from start/join */
    UCHAR bss_id[6];
    UCHAR auth_id[6];
    UCHAR net_default_tx_rate;
    UCHAR encryption;
    struct net_device_stats stats;

    UCHAR net_type;
    UCHAR sta_type;
    UCHAR fw_ver;
    UCHAR fw_bld;
    UCHAR fw_var;
    UCHAR ASIC_version;
    UCHAR assoc_id[2];
    UCHAR tib_length;
    UCHAR last_rsl;
    int beacon_rxed;
    struct beacon_rx last_bcn;
#ifdef WIRELESS_EXT
    iw_stats	wstats;		/* Wireless specific stats */
#endif
#ifdef WIRELESS_SPY
    int		spy_number;		/* Number of addresses to spy */
    mac_addr	spy_address[IW_MAX_SPY + 1];	/* The addresses to spy */
    iw_qual	spy_stat[IW_MAX_SPY + 1];	/* Statistics gathered */
#endif	/* WIRELESS_SPY */

} ray_dev_t;
/*****************************************************************************/

#endif /* RAYLINK_H */
