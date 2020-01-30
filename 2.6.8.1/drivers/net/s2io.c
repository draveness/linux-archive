/************************************************************************
 * s2io.c: A Linux PCI-X Ethernet driver for S2IO 10GbE Server NIC
 * Copyright(c) 2002-2005 S2IO Technologies

 * This software may be used and distributed according to the terms of
 * the GNU General Public License (GPL), incorporated herein by reference.
 * Drivers based on or derived from this code fall under the GPL and must
 * retain the authorship, copyright and license notice.  This file is not
 * a complete program and may only be used when the entire operating
 * system is licensed under the GPL.
 * See the file COPYING in this distribution for more information.
 *
 * Credits:
 * Jeff Garzik		: For pointing out the improper error condition 
 *			  check in the s2io_xmit routine and also some 
 * 			  issues in the Tx watch dog function. Also for
 *			  patiently answering all those innumerable 
 *			  questions regaring the 2.6 porting issues.
 * Stephen Hemminger	: Providing proper 2.6 porting mechanism for some
 *			  macros available only in 2.6 Kernel.
 * Francois Romieu	: For pointing out all code part that were 
 *			  deprecated and also styling related comments.
 * Grant Grundler	: For helping me get rid of some Architecture 
 *			  dependent code.
 * Christopher Hellwig	: Some more 2.6 specific issues in the driver.
 *			  	
 * The module loadable parameters that are supported by the driver and a brief
 * explaination of all the variables.
 * ring_num : This can be used to program the number of receive rings used 
 * in the driver.  					
 * frame_len: This is an array of size 8. Using this we can set the maximum 
 * size of the received frame that can be steered into the corrsponding 
 * receive ring.
 * ring_len: This defines the number of descriptors each ring can have. This 
 * is also an array of size 8.
 * fifo_num: This defines the number of Tx FIFOs thats used int the driver.
 * fifo_len: This too is an array of 8. Each element defines the number of 
 * Tx descriptors that can be associated with each corresponding FIFO.
 * latency_timer: This input is programmed into the Latency timer register
 * in PCI Configuration space.
 ************************************************************************/

#include<linux/config.h>
#include<linux/module.h>
#include<linux/types.h>
#include<linux/errno.h>
#include<linux/ioport.h>
#include<linux/pci.h>
#include<linux/kernel.h>
#include<linux/netdevice.h>
#include<linux/etherdevice.h>
#include<linux/skbuff.h>
#include<linux/init.h>
#include<linux/delay.h>
#include<linux/stddef.h>
#include<linux/ioctl.h>
#include<linux/timex.h>
#include<linux/sched.h>
#include<linux/ethtool.h>
#include<asm/system.h>
#include<asm/uaccess.h>
#include<linux/version.h>
#include<asm/io.h>
#include<linux/workqueue.h>

/* local include */
#include "s2io.h"
#include "s2io-regs.h"

/* S2io Driver name & version. */
static char s2io_driver_name[] = "s2io";
static char s2io_driver_version[] = "Version 1.0";

#define LINK_IS_UP(val64) (!(val64 & (ADAPTER_STATUS_RMAC_REMOTE_FAULT | \
				      ADAPTER_STATUS_RMAC_LOCAL_FAULT)))
#define TASKLET_IN_USE test_and_set_bit(0, \
				(unsigned long *)(&sp->tasklet_status))
#define PANIC	1
#define LOW	2
static inline int rx_buffer_level(nic_t * sp, int rxb_size, int ring)
{
	int level = 0;
	if ((sp->pkt_cnt[ring] - rxb_size) > 128) {
		level = LOW;
		if (rxb_size < sp->pkt_cnt[ring] / 8)
			level = PANIC;
	}

	return level;
}

/* Ethtool related variables and Macros. */
static char s2io_gstrings[][ETH_GSTRING_LEN] = {
	"Register test\t(offline)",
	"Eeprom test\t(offline)",
	"Link test\t(online)",
	"RLDRAM test\t(offline)",
	"BIST Test\t(offline)"
};

static char ethtool_stats_keys[][ETH_GSTRING_LEN] = {
	"tmac_frms",
	"tmac_data_octets",
	"tmac_drop_frms",
	"tmac_mcst_frms",
	"tmac_bcst_frms",
	"tmac_pause_ctrl_frms",
	"tmac_any_err_frms",
	"tmac_vld_ip_octets",
	"tmac_vld_ip",
	"tmac_drop_ip",
	"tmac_icmp",
	"tmac_rst_tcp",
	"tmac_tcp",
	"tmac_udp",
	"rmac_vld_frms",
	"rmac_data_octets",
	"rmac_fcs_err_frms",
	"rmac_drop_frms",
	"rmac_vld_mcst_frms",
	"rmac_vld_bcst_frms",
	"rmac_in_rng_len_err_frms",
	"rmac_long_frms",
	"rmac_pause_ctrl_frms",
	"rmac_discarded_frms",
	"rmac_usized_frms",
	"rmac_osized_frms",
	"rmac_frag_frms",
	"rmac_jabber_frms",
	"rmac_ip",
	"rmac_ip_octets",
	"rmac_hdr_err_ip",
	"rmac_drop_ip",
	"rmac_icmp",
	"rmac_tcp",
	"rmac_udp",
	"rmac_err_drp_udp",
	"rmac_pause_cnt",
	"rmac_accepted_ip",
	"rmac_err_tcp",
};

#define S2IO_STAT_LEN sizeof(ethtool_stats_keys)/ ETH_GSTRING_LEN
#define S2IO_STAT_STRINGS_LEN S2IO_STAT_LEN * ETH_GSTRING_LEN

#define S2IO_TEST_LEN	sizeof(s2io_gstrings) / ETH_GSTRING_LEN
#define S2IO_STRINGS_LEN	S2IO_TEST_LEN * ETH_GSTRING_LEN


/* Constants to be programmed into the Xena's registers to configure
 * the XAUI.
 */

#define SWITCH_SIGN	0xA5A5A5A5A5A5A5A5ULL
#define	END_SIGN	0x0

static u64 default_mdio_cfg[] = {
	/* Reset PMA PLL */
	0xC001010000000000ULL, 0xC0010100000000E0ULL,
	0xC0010100008000E4ULL,
	/* Remove Reset from PMA PLL */
	0xC001010000000000ULL, 0xC0010100000000E0ULL,
	0xC0010100000000E4ULL,
	END_SIGN
};

static u64 default_dtx_cfg[] = {
	0x8000051500000000ULL, 0x80000515000000E0ULL,
	0x80000515D93500E4ULL, 0x8001051500000000ULL,
	0x80010515000000E0ULL, 0x80010515001E00E4ULL,
	0x8002051500000000ULL, 0x80020515000000E0ULL,
	0x80020515F21000E4ULL,
	/* Set PADLOOPBACKN */
	0x8002051500000000ULL, 0x80020515000000E0ULL,
	0x80020515B20000E4ULL, 0x8003051500000000ULL,
	0x80030515000000E0ULL, 0x80030515B20000E4ULL,
	0x8004051500000000ULL, 0x80040515000000E0ULL,
	0x80040515B20000E4ULL, 0x8005051500000000ULL,
	0x80050515000000E0ULL, 0x80050515B20000E4ULL,
	SWITCH_SIGN,
	/* Remove PADLOOPBACKN */
	0x8002051500000000ULL, 0x80020515000000E0ULL,
	0x80020515F20000E4ULL, 0x8003051500000000ULL,
	0x80030515000000E0ULL, 0x80030515F20000E4ULL,
	0x8004051500000000ULL, 0x80040515000000E0ULL,
	0x80040515F20000E4ULL, 0x8005051500000000ULL,
	0x80050515000000E0ULL, 0x80050515F20000E4ULL,
	END_SIGN
};

/* Constants for Fixing the MacAddress problem seen mostly on
 * Alpha machines.
 */
static u64 fix_mac[] = {
	0x0060000000000000ULL, 0x0060600000000000ULL,
	0x0040600000000000ULL, 0x0000600000000000ULL,
	0x0020600000000000ULL, 0x0060600000000000ULL,
	0x0020600000000000ULL, 0x0060600000000000ULL,
	0x0020600000000000ULL, 0x0060600000000000ULL,
	0x0020600000000000ULL, 0x0060600000000000ULL,
	0x0020600000000000ULL, 0x0060600000000000ULL,
	0x0020600000000000ULL, 0x0060600000000000ULL,
	0x0020600000000000ULL, 0x0060600000000000ULL,
	0x0020600000000000ULL, 0x0060600000000000ULL,
	0x0020600000000000ULL, 0x0060600000000000ULL,
	0x0020600000000000ULL, 0x0060600000000000ULL,
	0x0020600000000000ULL, 0x0000600000000000ULL,
	0x0040600000000000ULL, 0x0060600000000000ULL,
	END_SIGN
};


/* Module Loadable parameters. */
static u32 ring_num;
static u32 frame_len[MAX_RX_RINGS];
static u32 ring_len[MAX_RX_RINGS];
static u32 fifo_num;
static u32 fifo_len[MAX_TX_FIFOS];
static u32 rx_prio;
static u32 tx_prio;
static u8 latency_timer = 0;

/* 
 * S2IO device table.
 * This table lists all the devices that this driver supports. 
 */
static struct pci_device_id s2io_tbl[] __devinitdata = {
	{PCI_VENDOR_ID_S2IO, PCI_DEVICE_ID_S2IO_WIN,
	 PCI_ANY_ID, PCI_ANY_ID},
	{PCI_VENDOR_ID_S2IO, PCI_DEVICE_ID_S2IO_UNI,
	 PCI_ANY_ID, PCI_ANY_ID},
	{0,}
};

MODULE_DEVICE_TABLE(pci, s2io_tbl);

static struct pci_driver s2io_driver = {
      name:"S2IO",
      id_table:s2io_tbl,
      probe:s2io_init_nic,
      remove:__devexit_p(s2io_rem_nic),
};

/*  
 *  Input Arguments: 
 *  Device private variable.
 *  Return Value: 
 *  SUCCESS on success and an appropriate -ve value on failure.
 *  Description: 
 *  The function allocates the all memory areas shared 
 *  between the NIC and the driver. This includes Tx descriptors, 
 *  Rx descriptors and the statistics block.
 */
static int initSharedMem(struct s2io_nic *nic)
{
	u32 size;
	void *tmp_v_addr, *tmp_v_addr_next;
	dma_addr_t tmp_p_addr, tmp_p_addr_next;
	RxD_block_t *pre_rxd_blk = NULL;
	int i, j, blk_cnt;
	struct net_device *dev = nic->dev;

	mac_info_t *mac_control;
	struct config_param *config;

	mac_control = &nic->mac_control;
	config = &nic->config;


	/* Allocation and initialization of TXDLs in FIOFs */
	size = 0;
	for (i = 0; i < config->TxFIFONum; i++) {
		size += config->TxCfg[i].FifoLen;
	}
	if (size > MAX_AVAILABLE_TXDS) {
		DBG_PRINT(ERR_DBG, "%s: Total number of Tx FIFOs ",
			  dev->name);
		DBG_PRINT(ERR_DBG, "exceeds the maximum value ");
		DBG_PRINT(ERR_DBG, "that can be used\n");
		return FAILURE;
	}
	size *= (sizeof(TxD_t) * config->MaxTxDs);

	mac_control->txd_list_mem = pci_alloc_consistent
	    (nic->pdev, size, &mac_control->txd_list_mem_phy);
	if (!mac_control->txd_list_mem) {
		return -ENOMEM;
	}
	mac_control->txd_list_mem_sz = size;

	tmp_v_addr = mac_control->txd_list_mem;
	tmp_p_addr = mac_control->txd_list_mem_phy;
	memset(tmp_v_addr, 0, size);

	DBG_PRINT(INIT_DBG, "%s:List Mem PHY: 0x%llx\n", dev->name,
		  (unsigned long long) tmp_p_addr);

	for (i = 0; i < config->TxFIFONum; i++) {
		mac_control->txdl_start_phy[i] = tmp_p_addr;
		mac_control->txdl_start[i] = (TxD_t *) tmp_v_addr;
		mac_control->tx_curr_put_info[i].offset = 0;
		mac_control->tx_curr_put_info[i].fifo_len =
		    config->TxCfg[i].FifoLen - 1;
		mac_control->tx_curr_get_info[i].offset = 0;
		mac_control->tx_curr_get_info[i].fifo_len =
		    config->TxCfg[i].FifoLen - 1;

		tmp_p_addr +=
		    (config->TxCfg[i].FifoLen * (sizeof(TxD_t)) *
		     config->MaxTxDs);
		tmp_v_addr +=
		    (config->TxCfg[i].FifoLen * (sizeof(TxD_t)) *
		     config->MaxTxDs);
	}

	/* Allocation and initialization of RXDs in Rings */
	size = 0;
	for (i = 0; i < config->RxRingNum; i++) {
		if (config->RxCfg[i].NumRxd % (MAX_RXDS_PER_BLOCK + 1)) {
			DBG_PRINT(ERR_DBG, "%s: RxD count of ", dev->name);
			DBG_PRINT(ERR_DBG, "Ring%d is not a multiple of ",
				  i);
			DBG_PRINT(ERR_DBG, "RxDs per Block");
			return FAILURE;
		}
		size += config->RxCfg[i].NumRxd;
		nic->block_count[i] =
		    config->RxCfg[i].NumRxd / (MAX_RXDS_PER_BLOCK + 1);
		nic->pkt_cnt[i] =
		    config->RxCfg[i].NumRxd - nic->block_count[i];
	}
	size = (size * (sizeof(RxD_t)));
	mac_control->rxd_ring_mem_sz = size;

	for (i = 0; i < config->RxRingNum; i++) {
		mac_control->rx_curr_get_info[i].block_index = 0;
		mac_control->rx_curr_get_info[i].offset = 0;
		mac_control->rx_curr_get_info[i].ring_len =
		    config->RxCfg[i].NumRxd - 1;
		mac_control->rx_curr_put_info[i].block_index = 0;
		mac_control->rx_curr_put_info[i].offset = 0;
		mac_control->rx_curr_put_info[i].ring_len =
		    config->RxCfg[i].NumRxd - 1;
		blk_cnt =
		    config->RxCfg[i].NumRxd / (MAX_RXDS_PER_BLOCK + 1);
		/*  Allocating all the Rx blocks */
		for (j = 0; j < blk_cnt; j++) {
			size = (MAX_RXDS_PER_BLOCK + 1) * (sizeof(RxD_t));
			tmp_v_addr = pci_alloc_consistent(nic->pdev, size,
							  &tmp_p_addr);
			if (tmp_v_addr == NULL) {
				/* In case of failure, freeSharedMem() 
				 * is called, which should free any 
				 * memory that was alloced till the 
				 * failure happened.
				 */
				nic->rx_blocks[i][j].block_virt_addr =
				    tmp_v_addr;
				return -ENOMEM;
			}
			memset(tmp_v_addr, 0, size);
			nic->rx_blocks[i][j].block_virt_addr = tmp_v_addr;
			nic->rx_blocks[i][j].block_dma_addr = tmp_p_addr;
		}
		/* Interlinking all Rx Blocks */
		for (j = 0; j < blk_cnt; j++) {
			tmp_v_addr = nic->rx_blocks[i][j].block_virt_addr;
			tmp_v_addr_next =
			    nic->rx_blocks[i][(j + 1) %
					      blk_cnt].block_virt_addr;
			tmp_p_addr = nic->rx_blocks[i][j].block_dma_addr;
			tmp_p_addr_next =
			    nic->rx_blocks[i][(j + 1) %
					      blk_cnt].block_dma_addr;

			pre_rxd_blk = (RxD_block_t *) tmp_v_addr;
			pre_rxd_blk->reserved_1 = END_OF_BLOCK;	/* last RxD 
								 * marker.
								 */
			pre_rxd_blk->reserved_2_pNext_RxD_block =
			    (unsigned long) tmp_v_addr_next;
			pre_rxd_blk->pNext_RxD_Blk_physical =
			    (u64) tmp_p_addr_next;
		}
	}

	/* Allocation and initialization of Statistics block */
	size = sizeof(StatInfo_t);
	mac_control->stats_mem = pci_alloc_consistent
	    (nic->pdev, size, &mac_control->stats_mem_phy);

	if (!mac_control->stats_mem) {
		/* In case of failure, freeSharedMem() is called, which 
		 * should free any memory that was alloced till the 
		 * failure happened.
		 */
		return -ENOMEM;
	}
	mac_control->stats_mem_sz = size;

	tmp_v_addr = mac_control->stats_mem;
	mac_control->StatsInfo = (StatInfo_t *) tmp_v_addr;
	memset(tmp_v_addr, 0, size);

	DBG_PRINT(INIT_DBG, "%s:Ring Mem PHY: 0x%llx\n", dev->name,
		  (unsigned long long) tmp_p_addr);

	return SUCCESS;
}

/*  
 *  Input Arguments: 
 *  Device peivate variable.
 *  Return Value: 
 *  NONE
 *  Description: 
 *  This function is to free all memory locations allocated by
 *  the initSharedMem() function and return it to the kernel.
 */
static void freeSharedMem(struct s2io_nic *nic)
{
	int i, j, blk_cnt, size;
	void *tmp_v_addr;
	dma_addr_t tmp_p_addr;
	mac_info_t *mac_control;
	struct config_param *config;


	if (!nic)
		return;

	mac_control = &nic->mac_control;
	config = &nic->config;

	if (mac_control->txd_list_mem) {
		pci_free_consistent(nic->pdev,
				    mac_control->txd_list_mem_sz,
				    mac_control->txd_list_mem,
				    mac_control->txd_list_mem_phy);
	}

	size = (MAX_RXDS_PER_BLOCK + 1) * (sizeof(RxD_t));
	for (i = 0; i < config->RxRingNum; i++) {
		blk_cnt = nic->block_count[i];
		for (j = 0; j < blk_cnt; j++) {
			tmp_v_addr = nic->rx_blocks[i][j].block_virt_addr;
			tmp_p_addr = nic->rx_blocks[i][j].block_dma_addr;
			if (tmp_v_addr == NULL)
				break;
			pci_free_consistent(nic->pdev, size,
					    tmp_v_addr, tmp_p_addr);
		}
	}

	if (mac_control->stats_mem) {
		pci_free_consistent(nic->pdev,
				    mac_control->stats_mem_sz,
				    mac_control->stats_mem,
				    mac_control->stats_mem_phy);
	}
}

/*  
 *  Input Arguments: 
 *  device peivate variable
 *  Return Value: 
 *  SUCCESS on success and '-1' on failure (endian settings incorrect).
 *  Description: 
 *  The function sequentially configures every block 
 *  of the H/W from their reset values. 
 */
static int initNic(struct s2io_nic *nic)
{
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) nic->bar0;
	struct net_device *dev = nic->dev;
	register u64 val64 = 0;
	void *add;
	u32 time;
	int i, j;
	mac_info_t *mac_control;
	struct config_param *config;
	int mdio_cnt = 0, dtx_cnt = 0;
	unsigned long long print_var, mem_share;

	mac_control = &nic->mac_control;
	config = &nic->config;

	/*  Set proper endian settings and verify the same by 
	 *  reading the PIF Feed-back register.
	 */
#ifdef  __BIG_ENDIAN
	/* The device by default set to a big endian format, so 
	 * a big endian driver need not set anything.
	 */
	writeq(0xffffffffffffffffULL, &bar0->swapper_ctrl);
	val64 = (SWAPPER_CTRL_PIF_R_FE |
		 SWAPPER_CTRL_PIF_R_SE |
		 SWAPPER_CTRL_PIF_W_FE |
		 SWAPPER_CTRL_PIF_W_SE |
		 SWAPPER_CTRL_TXP_FE |
		 SWAPPER_CTRL_TXP_SE |
		 SWAPPER_CTRL_TXD_R_FE |
		 SWAPPER_CTRL_TXD_W_FE |
		 SWAPPER_CTRL_TXF_R_FE |
		 SWAPPER_CTRL_RXD_R_FE |
		 SWAPPER_CTRL_RXD_W_FE |
		 SWAPPER_CTRL_RXF_W_FE |
		 SWAPPER_CTRL_XMSI_FE |
		 SWAPPER_CTRL_XMSI_SE |
		 SWAPPER_CTRL_STATS_FE | SWAPPER_CTRL_STATS_SE);
	writeq(val64, &bar0->swapper_ctrl);
#else
	/* Initially we enable all bits to make it accessible by 
	 * the driver, then we selectively enable only those bits 
	 * that we want to set.
	 */
	writeq(0xffffffffffffffffULL, &bar0->swapper_ctrl);
	val64 = (SWAPPER_CTRL_PIF_R_FE |
		 SWAPPER_CTRL_PIF_R_SE |
		 SWAPPER_CTRL_PIF_W_FE |
		 SWAPPER_CTRL_PIF_W_SE |
		 SWAPPER_CTRL_TXP_FE |
		 SWAPPER_CTRL_TXP_SE |
		 SWAPPER_CTRL_TXD_R_FE |
		 SWAPPER_CTRL_TXD_R_SE |
		 SWAPPER_CTRL_TXD_W_FE |
		 SWAPPER_CTRL_TXD_W_SE |
		 SWAPPER_CTRL_TXF_R_FE |
		 SWAPPER_CTRL_RXD_R_FE |
		 SWAPPER_CTRL_RXD_R_SE |
		 SWAPPER_CTRL_RXD_W_FE |
		 SWAPPER_CTRL_RXD_W_SE |
		 SWAPPER_CTRL_RXF_W_FE |
		 SWAPPER_CTRL_XMSI_FE |
		 SWAPPER_CTRL_XMSI_SE |
		 SWAPPER_CTRL_STATS_FE | SWAPPER_CTRL_STATS_SE);
	writeq(val64, &bar0->swapper_ctrl);
#endif

	/* Verifying if endian settings are accurate by reading 
	 * a feedback register.
	 */
	val64 = readq(&bar0->pif_rd_swapper_fb);
	if (val64 != 0x0123456789ABCDEFULL) {
		/* Endian settings are incorrect, calls for another dekko. */
		print_var = (unsigned long long) val64;
		DBG_PRINT(INIT_DBG, "%s: Endian settings are wrong",
			  dev->name);
		DBG_PRINT(ERR_DBG, ", feedback read %llx\n", print_var);

		return FAILURE;
	}

	/* Remove XGXS from reset state */
	val64 = 0;
	writeq(val64, &bar0->sw_reset);
	val64 = readq(&bar0->sw_reset);
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(HZ / 2);

	/*  Enable Receiving broadcasts */
	val64 = readq(&bar0->mac_cfg);
	val64 |= MAC_RMAC_BCAST_ENABLE;
	writeq(RMAC_CFG_KEY(0x4C0D), &bar0->rmac_cfg_key);
	writeq(val64, &bar0->mac_cfg);

	/* Read registers in all blocks */
	val64 = readq(&bar0->mac_int_mask);
	val64 = readq(&bar0->mc_int_mask);
	val64 = readq(&bar0->xgxs_int_mask);

	/*  Set MTU */
	val64 = dev->mtu;
	writeq(vBIT(val64, 2, 14), &bar0->rmac_max_pyld_len);

	/* Configuring the XAUI Interface of Xena. 
	 *****************************************
	 * To Configure the Xena's XAUI, one has to write a series 
	 * of 64 bit values into two registers in a particular 
	 * sequence. Hence a macro 'SWITCH_SIGN' has been defined 
	 * which will be defined in the array of configuration values 
	 * (default_dtx_cfg & default_mdio_cfg) at appropriate places 
	 * to switch writing from one regsiter to another. We continue 
	 * writing these values until we encounter the 'END_SIGN' macro.
	 * For example, After making a series of 21 writes into 
	 * dtx_control register the 'SWITCH_SIGN' appears and hence we 
	 * start writing into mdio_control until we encounter END_SIGN.
	 */
	while (1) {
	      dtx_cfg:
		while (default_dtx_cfg[dtx_cnt] != END_SIGN) {
			if (default_dtx_cfg[dtx_cnt] == SWITCH_SIGN) {
				dtx_cnt++;
				goto mdio_cfg;
			}
			writeq(default_dtx_cfg[dtx_cnt],
			       &bar0->dtx_control);
			val64 = readq(&bar0->dtx_control);
			dtx_cnt++;
		}
	      mdio_cfg:
		while (default_mdio_cfg[mdio_cnt] != END_SIGN) {
			if (default_mdio_cfg[mdio_cnt] == SWITCH_SIGN) {
				mdio_cnt++;
				goto dtx_cfg;
			}
			writeq(default_mdio_cfg[mdio_cnt],
			       &bar0->mdio_control);
			val64 = readq(&bar0->mdio_control);
			mdio_cnt++;
		}
		if ((default_dtx_cfg[dtx_cnt] == END_SIGN) &&
		    (default_mdio_cfg[mdio_cnt] == END_SIGN)) {
			break;
		} else {
			goto dtx_cfg;
		}
	}

	/*  Tx DMA Initialization */
	val64 = 0;
	writeq(val64, &bar0->tx_fifo_partition_0);
	writeq(val64, &bar0->tx_fifo_partition_1);
	writeq(val64, &bar0->tx_fifo_partition_2);
	writeq(val64, &bar0->tx_fifo_partition_3);


	for (i = 0, j = 0; i < config->TxFIFONum; i++) {
		val64 |=
		    vBIT(config->TxCfg[i].FifoLen - 1, ((i * 32) + 19),
			 13) | vBIT(config->TxCfg[i].FifoPriority,
				    ((i * 32) + 5), 3);

		if (i == (config->TxFIFONum - 1)) {
			if (i % 2 == 0)
				i++;
		}

		switch (i) {
		case 1:
			writeq(val64, &bar0->tx_fifo_partition_0);
			val64 = 0;
			break;
		case 3:
			writeq(val64, &bar0->tx_fifo_partition_1);
			val64 = 0;
			break;
		case 5:
			writeq(val64, &bar0->tx_fifo_partition_2);
			val64 = 0;
			break;
		case 7:
			writeq(val64, &bar0->tx_fifo_partition_3);
			break;
		}
	}

	/* Enable Tx FIFO partition 0. */
	val64 = readq(&bar0->tx_fifo_partition_0);
	val64 |= BIT(0);	/* To enable the FIFO partition. */
	writeq(val64, &bar0->tx_fifo_partition_0);

	val64 = readq(&bar0->tx_fifo_partition_0);
	DBG_PRINT(INIT_DBG, "Fifo partition at: 0x%p is: 0x%llx\n",
		  &bar0->tx_fifo_partition_0, (unsigned long long) val64);

	/* 
	 * Initialization of Tx_PA_CONFIG register to ignore packet 
	 * integrity checking.
	 */
	val64 = readq(&bar0->tx_pa_cfg);
	val64 |= TX_PA_CFG_IGNORE_FRM_ERR | TX_PA_CFG_IGNORE_SNAP_OUI |
	    TX_PA_CFG_IGNORE_LLC_CTRL | TX_PA_CFG_IGNORE_L2_ERR;
	writeq(val64, &bar0->tx_pa_cfg);

	/* Rx DMA intialization. */
	val64 = 0;
	for (i = 0; i < config->RxRingNum; i++) {
		val64 |=
		    vBIT(config->RxCfg[i].RingPriority, (5 + (i * 8)), 3);
	}
	writeq(val64, &bar0->rx_queue_priority);

	/* Allocating equal share of memory to all the configured 
	 * Rings.
	 */
	val64 = 0;
	for (i = 0; i < config->RxRingNum; i++) {
		switch (i) {
		case 0:
			mem_share = (64 / config->RxRingNum +
				     64 % config->RxRingNum);
			val64 |= RX_QUEUE_CFG_Q0_SZ(mem_share);
			continue;
		case 1:
			mem_share = (64 / config->RxRingNum);
			val64 |= RX_QUEUE_CFG_Q1_SZ(mem_share);
			continue;
		case 2:
			mem_share = (64 / config->RxRingNum);
			val64 |= RX_QUEUE_CFG_Q2_SZ(mem_share);
			continue;
		case 3:
			mem_share = (64 / config->RxRingNum);
			val64 |= RX_QUEUE_CFG_Q3_SZ(mem_share);
			continue;
		case 4:
			mem_share = (64 / config->RxRingNum);
			val64 |= RX_QUEUE_CFG_Q4_SZ(mem_share);
			continue;
		case 5:
			mem_share = (64 / config->RxRingNum);
			val64 |= RX_QUEUE_CFG_Q5_SZ(mem_share);
			continue;
		case 6:
			mem_share = (64 / config->RxRingNum);
			val64 |= RX_QUEUE_CFG_Q6_SZ(mem_share);
			continue;
		case 7:
			mem_share = (64 / config->RxRingNum);
			val64 |= RX_QUEUE_CFG_Q7_SZ(mem_share);
			continue;
		}
	}
	writeq(val64, &bar0->rx_queue_cfg);

	/* Initializing the Tx round robin registers to 0.
	 * Filling Tx and Rx round robin registers as per the 
	 * number of FIFOs and Rings is still TODO.
	 */
	writeq(0, &bar0->tx_w_round_robin_0);
	writeq(0, &bar0->tx_w_round_robin_1);
	writeq(0, &bar0->tx_w_round_robin_2);
	writeq(0, &bar0->tx_w_round_robin_3);
	writeq(0, &bar0->tx_w_round_robin_4);

	/* Disable Rx steering. Hard coding all packets be steered to
	 * Queue 0 for now. 
	 * TODO*/
	if (rx_prio) {
		u64 def = 0x8000000000000000ULL, tmp;
		for (i = 0; i < MAX_RX_RINGS; i++) {
			tmp = (u64) (def >> (i % config->RxRingNum));
			val64 |= (u64) (tmp >> (i * 8));
		}
		writeq(val64, &bar0->rts_qos_steering);
	} else {
		val64 = 0x8080808080808080ULL;
		writeq(val64, &bar0->rts_qos_steering);
	}

	/* UDP Fix */
	val64 = 0;
	for (i = 1; i < 8; i++)
		writeq(val64, &bar0->rts_frm_len_n[i]);

	/* Set rts_frm_len register for fifo 0 */
	writeq(MAC_RTS_FRM_LEN_SET(dev->mtu + 22),
	       &bar0->rts_frm_len_n[0]);

	/* Enable statistics */
	writeq(mac_control->stats_mem_phy, &bar0->stat_addr);
	val64 = SET_UPDT_PERIOD(8) | STAT_CFG_STAT_RO | STAT_CFG_STAT_EN;
	writeq(val64, &bar0->stat_cfg);

	/* Initializing the sampling rate for the device to calculate the
	 * bandwidth utilization.
	 */
	val64 = MAC_TX_LINK_UTIL_VAL(0x5) | MAC_RX_LINK_UTIL_VAL(0x5);
	writeq(val64, &bar0->mac_link_util);


	/* Initializing the Transmit and Receive Traffic Interrupt 
	 * Scheme.
	 */
	/* TTI Initialization */
	val64 = TTI_DATA1_MEM_TX_TIMER_VAL(0xFFF) |
	    TTI_DATA1_MEM_TX_URNG_A(0xA) | TTI_DATA1_MEM_TX_URNG_B(0x10) |
	    TTI_DATA1_MEM_TX_URNG_C(0x30) | TTI_DATA1_MEM_TX_TIMER_AC_EN;
	writeq(val64, &bar0->tti_data1_mem);

	val64 =
	    TTI_DATA2_MEM_TX_UFC_A(0x10) | TTI_DATA2_MEM_TX_UFC_B(0x20) |
	    TTI_DATA2_MEM_TX_UFC_C(0x40) | TTI_DATA2_MEM_TX_UFC_D(0x80);
	writeq(val64, &bar0->tti_data2_mem);

	val64 = TTI_CMD_MEM_WE | TTI_CMD_MEM_STROBE_NEW_CMD;
	writeq(val64, &bar0->tti_command_mem);

	/* Once the operation completes, the Strobe bit of the command
	 * register will be reset. We poll for this particular condition
	 * We wait for a maximum of 500ms for the operation to complete,
	 * if it's not complete by then we return error.
	 */
	time = 0;
	while (TRUE) {
		val64 = readq(&bar0->tti_command_mem);
		if (!(val64 & TTI_CMD_MEM_STROBE_NEW_CMD)) {
			break;
		}
		if (time > 10) {
			DBG_PRINT(ERR_DBG, "%s: TTI init Failed\n",
				  dev->name);
			return -1;
		}
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ / 20);
		time++;
	}

	/* RTI Initialization */
	val64 = RTI_DATA1_MEM_RX_TIMER_VAL(0xFFF) |
	    RTI_DATA1_MEM_RX_URNG_A(0xA) | RTI_DATA1_MEM_RX_URNG_B(0x10) |
	    RTI_DATA1_MEM_RX_URNG_C(0x30) | RTI_DATA1_MEM_RX_TIMER_AC_EN;
	writeq(val64, &bar0->rti_data1_mem);

	val64 = RTI_DATA2_MEM_RX_UFC_A(0x1) | RTI_DATA2_MEM_RX_UFC_B(0x2) |
	    RTI_DATA2_MEM_RX_UFC_C(0x40) | RTI_DATA2_MEM_RX_UFC_D(0x80);
	writeq(val64, &bar0->rti_data2_mem);

	val64 = RTI_CMD_MEM_WE | RTI_CMD_MEM_STROBE_NEW_CMD;
	writeq(val64, &bar0->rti_command_mem);

	/* Once the operation completes, the Strobe bit of the command
	 * register will be reset. We poll for this particular condition
	 * We wait for a maximum of 500ms for the operation to complete,
	 * if it's not complete by then we return error.
	 */
	time = 0;
	while (TRUE) {
		val64 = readq(&bar0->rti_command_mem);
		if (!(val64 & TTI_CMD_MEM_STROBE_NEW_CMD)) {
			break;
		}
		if (time > 10) {
			DBG_PRINT(ERR_DBG, "%s: RTI init Failed\n",
				  dev->name);
			return -1;
		}
		time++;
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ / 20);
	}

	/* Initializing proper values as Pause threshold into all 
	 * the 8 Queues on Rx side.
	 */
	writeq(0xffbbffbbffbbffbbULL, &bar0->mc_pause_thresh_q0q3);
	writeq(0xffbbffbbffbbffbbULL, &bar0->mc_pause_thresh_q4q7);

	/* Disable RMAC PAD STRIPPING */
	add = (void *) &bar0->mac_cfg;
	val64 = readq(&bar0->mac_cfg);
	val64 &= ~(MAC_CFG_RMAC_STRIP_PAD);
	writeq(RMAC_CFG_KEY(0x4C0D), &bar0->rmac_cfg_key);
	writel((u32) (val64), add);
	writeq(RMAC_CFG_KEY(0x4C0D), &bar0->rmac_cfg_key);
	writel((u32) (val64 >> 32), (add + 4));
	val64 = readq(&bar0->mac_cfg);

	return SUCCESS;
}

/*  
 *  Input Arguments: 
 *  device private variable,
 *  A mask indicating which Intr block must be modified and,
 *  A flag indicating whether to enable or disable the Intrs.
 *  Return Value: 
 *  NONE.
 *  Description: 
 *  This function will either disable or enable the interrupts 
 *  depending on the flag argument. The mask argument can be used to 
 *  enable/disable any Intr block. 
 */
static void en_dis_able_NicIntrs(struct s2io_nic *nic, u16 mask, int flag)
{
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) nic->bar0;
	register u64 val64 = 0, temp64 = 0;

	/*  Top level interrupt classification */
	/*  PIC Interrupts */
	if ((mask & (TX_PIC_INTR | RX_PIC_INTR))) {
		/*  Enable PIC Intrs in the general intr mask register */
		val64 = TXPIC_INT_M | PIC_RX_INT_M;
		if (flag == ENABLE_INTRS) {
			temp64 = readq(&bar0->general_int_mask);
			temp64 &= ~((u64) val64);
			writeq(temp64, &bar0->general_int_mask);
			/*  Disabled all PCIX, Flash, MDIO, IIC and GPIO
			 *  interrupts for now. 
			 * TODO */
			writeq(DISABLE_ALL_INTRS, &bar0->pic_int_mask);
			/*  No MSI Support is available presently, so TTI and
			 * RTI interrupts are also disabled.
			 */
		} else if (flag == DISABLE_INTRS) {
			/*  Disable PIC Intrs in the general intr mask register 
			 */
			writeq(DISABLE_ALL_INTRS, &bar0->pic_int_mask);
			temp64 = readq(&bar0->general_int_mask);
			val64 |= temp64;
			writeq(val64, &bar0->general_int_mask);
		}
	}

	/*  DMA Interrupts */
	/*  Enabling/Disabling Tx DMA interrupts */
	if (mask & TX_DMA_INTR) {
		/*  Enable TxDMA Intrs in the general intr mask register */
		val64 = TXDMA_INT_M;
		if (flag == ENABLE_INTRS) {
			temp64 = readq(&bar0->general_int_mask);
			temp64 &= ~((u64) val64);
			writeq(temp64, &bar0->general_int_mask);
			/* Disable all interrupts other than PFC interrupt in 
			 * DMA level.
			 */
			val64 = DISABLE_ALL_INTRS & (~TXDMA_PFC_INT_M);
			writeq(val64, &bar0->txdma_int_mask);
			/* Enable only the MISC error 1 interrupt in PFC block 
			 */
			val64 = DISABLE_ALL_INTRS & (~PFC_MISC_ERR_1);
			writeq(val64, &bar0->pfc_err_mask);
		} else if (flag == DISABLE_INTRS) {
			/*  Disable TxDMA Intrs in the general intr mask 
			 *  register */
			writeq(DISABLE_ALL_INTRS, &bar0->txdma_int_mask);
			writeq(DISABLE_ALL_INTRS, &bar0->pfc_err_mask);
			temp64 = readq(&bar0->general_int_mask);
			val64 |= temp64;
			writeq(val64, &bar0->general_int_mask);
		}
	}

	/*  Enabling/Disabling Rx DMA interrupts */
	if (mask & RX_DMA_INTR) {
		/*  Enable RxDMA Intrs in the general intr mask register */
		val64 = RXDMA_INT_M;
		if (flag == ENABLE_INTRS) {
			temp64 = readq(&bar0->general_int_mask);
			temp64 &= ~((u64) val64);
			writeq(temp64, &bar0->general_int_mask);
			/* All RxDMA block interrupts are disabled for now 
			 * TODO */
			writeq(DISABLE_ALL_INTRS, &bar0->rxdma_int_mask);
		} else if (flag == DISABLE_INTRS) {
			/*  Disable RxDMA Intrs in the general intr mask 
			 *  register */
			writeq(DISABLE_ALL_INTRS, &bar0->rxdma_int_mask);
			temp64 = readq(&bar0->general_int_mask);
			val64 |= temp64;
			writeq(val64, &bar0->general_int_mask);
		}
	}

	/*  MAC Interrupts */
	/*  Enabling/Disabling MAC interrupts */
	if (mask & (TX_MAC_INTR | RX_MAC_INTR)) {
		val64 = TXMAC_INT_M | RXMAC_INT_M;
		if (flag == ENABLE_INTRS) {
			temp64 = readq(&bar0->general_int_mask);
			temp64 &= ~((u64) val64);
			writeq(temp64, &bar0->general_int_mask);
			/* All MAC block error interrupts are disabled for now 
			 * except the link status change interrupt.
			 * TODO*/
			val64 = MAC_INT_STATUS_RMAC_INT;
			temp64 = readq(&bar0->mac_int_mask);
			temp64 &= ~((u64) val64);
			writeq(temp64, &bar0->mac_int_mask);

			val64 = readq(&bar0->mac_rmac_err_mask);
			val64 &= ~((u64) RMAC_LINK_STATE_CHANGE_INT);
			writeq(val64, &bar0->mac_rmac_err_mask);
		} else if (flag == DISABLE_INTRS) {
			/*  Disable MAC Intrs in the general intr mask register 
			 */
			writeq(DISABLE_ALL_INTRS, &bar0->mac_int_mask);
			writeq(DISABLE_ALL_INTRS,
			       &bar0->mac_rmac_err_mask);

			temp64 = readq(&bar0->general_int_mask);
			val64 |= temp64;
			writeq(val64, &bar0->general_int_mask);
		}
	}

	/*  XGXS Interrupts */
	if (mask & (TX_XGXS_INTR | RX_XGXS_INTR)) {
		val64 = TXXGXS_INT_M | RXXGXS_INT_M;
		if (flag == ENABLE_INTRS) {
			temp64 = readq(&bar0->general_int_mask);
			temp64 &= ~((u64) val64);
			writeq(temp64, &bar0->general_int_mask);
			/* All XGXS block error interrupts are disabled for now
			 *  TODO */
			writeq(DISABLE_ALL_INTRS, &bar0->xgxs_int_mask);
		} else if (flag == DISABLE_INTRS) {
			/*  Disable MC Intrs in the general intr mask register 
			 */
			writeq(DISABLE_ALL_INTRS, &bar0->xgxs_int_mask);
			temp64 = readq(&bar0->general_int_mask);
			val64 |= temp64;
			writeq(val64, &bar0->general_int_mask);
		}
	}

	/*  Memory Controller(MC) interrupts */
	if (mask & MC_INTR) {
		val64 = MC_INT_M;
		if (flag == ENABLE_INTRS) {
			temp64 = readq(&bar0->general_int_mask);
			temp64 &= ~((u64) val64);
			writeq(temp64, &bar0->general_int_mask);
			/* All MC block error interrupts are disabled for now
			 * TODO */
			writeq(DISABLE_ALL_INTRS, &bar0->mc_int_mask);
		} else if (flag == DISABLE_INTRS) {
			/*  Disable MC Intrs in the general intr mask register
			 */
			writeq(DISABLE_ALL_INTRS, &bar0->mc_int_mask);
			temp64 = readq(&bar0->general_int_mask);
			val64 |= temp64;
			writeq(val64, &bar0->general_int_mask);
		}
	}


	/*  Tx traffic interrupts */
	if (mask & TX_TRAFFIC_INTR) {
		val64 = TXTRAFFIC_INT_M;
		if (flag == ENABLE_INTRS) {
			temp64 = readq(&bar0->general_int_mask);
			temp64 &= ~((u64) val64);
			writeq(temp64, &bar0->general_int_mask);
			/* Enable all the Tx side interrupts */
			writeq(0x0, &bar0->tx_traffic_mask);	/* '0' Enables 
								 * all 64 TX 
								 * interrupt 
								 * levels.
								 */
		} else if (flag == DISABLE_INTRS) {
			/*  Disable Tx Traffic Intrs in the general intr mask 
			 *  register.
			 */
			writeq(DISABLE_ALL_INTRS, &bar0->tx_traffic_mask);
			temp64 = readq(&bar0->general_int_mask);
			val64 |= temp64;
			writeq(val64, &bar0->general_int_mask);
		}
	}

	/*  Rx traffic interrupts */
	if (mask & RX_TRAFFIC_INTR) {
		val64 = RXTRAFFIC_INT_M;
		if (flag == ENABLE_INTRS) {
			temp64 = readq(&bar0->general_int_mask);
			temp64 &= ~((u64) val64);
			writeq(temp64, &bar0->general_int_mask);
			writeq(0x0, &bar0->rx_traffic_mask);	/* '0' Enables 
								 * all 8 RX 
								 * interrupt 
								 * levels.
								 */
		} else if (flag == DISABLE_INTRS) {
			/*  Disable Rx Traffic Intrs in the general intr mask 
			 *  register.
			 */
			writeq(DISABLE_ALL_INTRS, &bar0->rx_traffic_mask);
			temp64 = readq(&bar0->general_int_mask);
			val64 |= temp64;
			writeq(val64, &bar0->general_int_mask);
		}
	}
}

/*  
 *  Input Arguments: 
 *   val64 - Value read from adapter status register.
 *   flag - indicates if the adapter enable bit was ever written once before.
 *  Return Value: 
 *   void.
 *  Description: 
 *   Returns whether the H/W is ready to go or not. Depending on whether 
 *   adapter enable bit was written or not the comparison differs and the 
 *   calling function passes the input argument flag to indicate this.
 */
static int verify_xena_quiescence(u64 val64, int flag)
{
	int ret = 0;
	u64 tmp64 = ~((u64) val64);

	if (!
	    (tmp64 &
	     (ADAPTER_STATUS_TDMA_READY | ADAPTER_STATUS_RDMA_READY |
	      ADAPTER_STATUS_PFC_READY | ADAPTER_STATUS_TMAC_BUF_EMPTY |
	      ADAPTER_STATUS_PIC_QUIESCENT | ADAPTER_STATUS_MC_DRAM_READY |
	      ADAPTER_STATUS_MC_QUEUES_READY | ADAPTER_STATUS_M_PLL_LOCK |
	      ADAPTER_STATUS_P_PLL_LOCK))) {
		if (flag == FALSE) {
			if (!(val64 & ADAPTER_STATUS_RMAC_PCC_IDLE) &&
			    ((val64 & ADAPTER_STATUS_RC_PRC_QUIESCENT) ==
			     ADAPTER_STATUS_RC_PRC_QUIESCENT)) {

				ret = 1;

			}
		} else {
			if (((val64 & ADAPTER_STATUS_RMAC_PCC_IDLE) ==
			     ADAPTER_STATUS_RMAC_PCC_IDLE) &&
			    (!(val64 & ADAPTER_STATUS_RC_PRC_QUIESCENT) ||
			     ((val64 & ADAPTER_STATUS_RC_PRC_QUIESCENT) ==
			      ADAPTER_STATUS_RC_PRC_QUIESCENT))) {

				ret = 1;

			}
		}
	}

	return ret;
}

/* 
 * New procedure to clear mac address reading  problems on Alpha platforms
 *
 */
void FixMacAddress(nic_t * sp)
{
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) sp->bar0;
	u64 val64;
	int i = 0;

	while (fix_mac[i] != END_SIGN) {
		writeq(fix_mac[i++], &bar0->gpio_control);
		val64 = readq(&bar0->gpio_control);
	}
}

/*  
 *  Input Arguments: 
 *  device private variable.
 *  Return Value: 
 *  SUCCESS on success and -1 on failure.
 *  Description: 
 *  This function actually turns the device on. Before this 
 *  function is called, all Registers are configured from their reset states 
 *  and shared memory is allocated but the NIC is still quiescent. On 
 *  calling this function, the device interrupts are cleared and the NIC is
 *  literally switched on by writing into the adapter control register.
 */
static int startNic(struct s2io_nic *nic)
{
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) nic->bar0;
	struct net_device *dev = nic->dev;
	register u64 val64 = 0;
	u16 interruptible, i;
	u16 subid;
	mac_info_t *mac_control;
	struct config_param *config;

	mac_control = &nic->mac_control;
	config = &nic->config;

	/*  PRC Initialization and configuration */
	for (i = 0; i < config->RxRingNum; i++) {
		writeq((u64) nic->rx_blocks[i][0].block_dma_addr,
		       &bar0->prc_rxd0_n[i]);

		val64 = readq(&bar0->prc_ctrl_n[i]);
		val64 |= PRC_CTRL_RC_ENABLED;
		writeq(val64, &bar0->prc_ctrl_n[i]);
	}

	/* Enabling MC-RLDRAM. After enabling the device, we timeout
	 * for around 100ms, which is approximately the time required
	 * for the device to be ready for operation.
	 */
	val64 = readq(&bar0->mc_rldram_mrs);
	val64 |= MC_RLDRAM_QUEUE_SIZE_ENABLE | MC_RLDRAM_MRS_ENABLE;
	writeq(val64, &bar0->mc_rldram_mrs);
	val64 = readq(&bar0->mc_rldram_mrs);

	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(HZ / 10);	/* Delay by around 100 ms. */

	/* Enabling ECC Protection. */
	val64 = readq(&bar0->adapter_control);
	val64 &= ~ADAPTER_ECC_EN;
	writeq(val64, &bar0->adapter_control);

	/* Clearing any possible Link state change interrupts that 
	 * could have popped up just before Enabling the card.
	 */
	val64 = readq(&bar0->mac_rmac_err_reg);
	if (val64)
		writeq(val64, &bar0->mac_rmac_err_reg);

	/* Verify if the device is ready to be enabled, if so enable 
	 * it.
	 */
	val64 = readq(&bar0->adapter_status);
	if (!verify_xena_quiescence(val64, nic->device_enabled_once)) {
		DBG_PRINT(ERR_DBG, "%s: device is not ready, ", dev->name);
		DBG_PRINT(ERR_DBG, "Adapter status reads: 0x%llx\n",
			  (unsigned long long) val64);
		return FAILURE;
	}

	/*  Enable select interrupts */
	interruptible = TX_TRAFFIC_INTR | RX_TRAFFIC_INTR | TX_MAC_INTR |
	    RX_MAC_INTR;
	en_dis_able_NicIntrs(nic, interruptible, ENABLE_INTRS);

	/* With some switches, link might be already up at this point.
	 * Because of this weird behavior, when we enable laser, 
	 * we may not get link. We need to handle this. We cannot 
	 * figure out which switch is misbehaving. So we are forced to 
	 * make a global change. 
	 */

	/* Enabling Laser. */
	val64 = readq(&bar0->adapter_control);
	val64 |= ADAPTER_EOI_TX_ON;
	writeq(val64, &bar0->adapter_control);

	/* SXE-002: Initialize link and activity LED */
	subid = nic->pdev->subsystem_device;
	if ((subid & 0xFF) >= 0x07) {
		val64 = readq(&bar0->gpio_control);
		val64 |= 0x0000800000000000ULL;
		writeq(val64, &bar0->gpio_control);
		val64 = 0x0411040400000000ULL;
		writeq(val64, (void *) ((u8 *) bar0 + 0x2700));
	}

	/* 
	 * Here we are performing soft reset on XGXS to 
	 * force link down. Since link is already up, we will get
	 * link state change interrupt after this reset
	 */
	writeq(0x8007051500000000ULL, &bar0->dtx_control);
	val64 = readq(&bar0->dtx_control);
	writeq(0x80070515000000E0ULL, &bar0->dtx_control);
	val64 = readq(&bar0->dtx_control);
	writeq(0x80070515001F00E4ULL, &bar0->dtx_control);
	val64 = readq(&bar0->dtx_control);

	return SUCCESS;
}

/*  
 *  Input Arguments: 
 *   nic - device private variable.
 *  Return Value: 
 *   void.
 *  Description: 
 *   Free all queued Tx buffers.
 */
void freeTxBuffers(struct s2io_nic *nic)
{
	struct net_device *dev = nic->dev;
	struct sk_buff *skb;
	TxD_t *txdp;
	int i, j;
#if DEBUG_ON
	int cnt = 0;
#endif
	mac_info_t *mac_control;
	struct config_param *config;

	mac_control = &nic->mac_control;
	config = &nic->config;

	for (i = 0; i < config->TxFIFONum; i++) {
		for (j = 0; j < config->TxCfg[i].FifoLen - 1; j++) {
			txdp = mac_control->txdl_start[i] +
			    (config->MaxTxDs * j);

			if (!(txdp->Control_1 & TXD_LIST_OWN_XENA)) {
				/* If owned by host, ignore */
				continue;
			}
			skb =
			    (struct sk_buff *) ((unsigned long) txdp->
						Host_Control);
			if (skb == NULL) {
				DBG_PRINT(ERR_DBG, "%s: NULL skb ",
					  dev->name);
				DBG_PRINT(ERR_DBG, "in Tx Int\n");
				return;
			}
#if DEBUG_ON
			cnt++;
#endif
			dev_kfree_skb(skb);
			memset(txdp, 0, sizeof(TxD_t));
		}
#if DEBUG_ON
		DBG_PRINT(INTR_DBG,
			  "%s:forcibly freeing %d skbs on FIFO%d\n",
			  dev->name, cnt, i);
#endif
	}
}

/*  
 *  Input Arguments: 
 *   nic - device private variable.
 *  Return Value: 
 *   void.
 *  Description: 
 *   This function does exactly the opposite of what the startNic() 
 *   function does. This function is called to stop 
 *   the device.
 */
static void stopNic(struct s2io_nic *nic)
{
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) nic->bar0;
	register u64 val64 = 0;
	u16 interruptible, i;
	mac_info_t *mac_control;
	struct config_param *config;

	mac_control = &nic->mac_control;
	config = &nic->config;

/*  Disable all interrupts */
	interruptible = TX_TRAFFIC_INTR | RX_TRAFFIC_INTR | TX_MAC_INTR |
	    RX_MAC_INTR;
	en_dis_able_NicIntrs(nic, interruptible, DISABLE_INTRS);

/*  Disable PRCs */
	for (i = 0; i < config->RxRingNum; i++) {
		val64 = readq(&bar0->prc_ctrl_n[i]);
		val64 &= ~((u64) PRC_CTRL_RC_ENABLED);
		writeq(val64, &bar0->prc_ctrl_n[i]);
	}
}

/*  
 *  Input Arguments: 
 *  device private variable
 *  Return Value: 
 *  SUCCESS on success or an appropriate -ve value on failure.
 *  Description: 
 *  The function allocates Rx side skbs and puts the physical
 *  address of these buffers into the RxD buffer pointers, so that the NIC
 *  can DMA the received frame into these locations.
 *  The NIC supports 3 receive modes, viz
 *  1. single buffer,
 *  2. three buffer and
 *  3. Five buffer modes.
 *  Each mode defines how many fragments the received frame will be split 
 *  up into by the NIC. The frame is split into L3 header, L4 Header, 
 *  L4 payload in three buffer mode and in 5 buffer mode, L4 payload itself 
 *  is split into 3 fragments. As of now only single buffer mode is supported.
 */
int fill_rx_buffers(struct s2io_nic *nic, int ring_no)
{
	struct net_device *dev = nic->dev;
	struct sk_buff *skb;
	RxD_t *rxdp;
	int off, off1, size, block_no, block_no1;
	int offset, offset1;
	u32 alloc_tab = 0;
	u32 alloc_cnt = nic->pkt_cnt[ring_no] -
	    atomic_read(&nic->rx_bufs_left[ring_no]);
	mac_info_t *mac_control;
	struct config_param *config;

	mac_control = &nic->mac_control;
	config = &nic->config;

	if (frame_len[ring_no]) {
		if (frame_len[ring_no] > dev->mtu)
			dev->mtu = frame_len[ring_no];
		size = frame_len[ring_no] + HEADER_ETHERNET_II_802_3_SIZE +
		    HEADER_802_2_SIZE + HEADER_SNAP_SIZE;
	} else {
		size = dev->mtu + HEADER_ETHERNET_II_802_3_SIZE +
		    HEADER_802_2_SIZE + HEADER_SNAP_SIZE;
	}

	while (alloc_tab < alloc_cnt) {
		block_no = mac_control->rx_curr_put_info[ring_no].
		    block_index;
		block_no1 = mac_control->rx_curr_get_info[ring_no].
		    block_index;
		off = mac_control->rx_curr_put_info[ring_no].offset;
		off1 = mac_control->rx_curr_get_info[ring_no].offset;
		offset = block_no * (MAX_RXDS_PER_BLOCK + 1) + off;
		offset1 = block_no1 * (MAX_RXDS_PER_BLOCK + 1) + off1;

		rxdp = nic->rx_blocks[ring_no][block_no].
		    block_virt_addr + off;
		if ((offset == offset1) && (rxdp->Host_Control)) {
			DBG_PRINT(INTR_DBG, "%s: Get and Put", dev->name);
			DBG_PRINT(INTR_DBG, " info equated\n");
			goto end;
		}

		if (rxdp->Control_1 == END_OF_BLOCK) {
			mac_control->rx_curr_put_info[ring_no].
			    block_index++;
			mac_control->rx_curr_put_info[ring_no].
			    block_index %= nic->block_count[ring_no];
			block_no = mac_control->rx_curr_put_info
			    [ring_no].block_index;
			off++;
			off %= (MAX_RXDS_PER_BLOCK + 1);
			mac_control->rx_curr_put_info[ring_no].offset =
			    off;
			/*rxdp = nic->rx_blocks[ring_no][block_no].
			   block_virt_addr + off; */
			rxdp = (RxD_t *) ((unsigned long) rxdp->Control_2);
			DBG_PRINT(INTR_DBG, "%s: Next block at: %p\n",
				  dev->name, rxdp);
		}

		if (rxdp->Control_1 & RXD_OWN_XENA) {
			mac_control->rx_curr_put_info[ring_no].
			    offset = off;
			goto end;
		}

		skb = dev_alloc_skb(size + NET_IP_ALIGN);
		if (!skb) {
			DBG_PRINT(ERR_DBG, "%s: Out of ", dev->name);
			DBG_PRINT(ERR_DBG, "memory to allocate SKBs\n");
			return -ENOMEM;
		}
		skb_reserve(skb, NET_IP_ALIGN);
		memset(rxdp, 0, sizeof(RxD_t));
		rxdp->Buffer0_ptr = pci_map_single
		    (nic->pdev, skb->data, size, PCI_DMA_FROMDEVICE);
		rxdp->Control_2 &= (~MASK_BUFFER0_SIZE);
		rxdp->Control_2 |= SET_BUFFER0_SIZE(size);
		rxdp->Host_Control = (unsigned long) (skb);
		rxdp->Control_1 |= RXD_OWN_XENA;
		off++;
		off %= (MAX_RXDS_PER_BLOCK + 1);
		mac_control->rx_curr_put_info[ring_no].offset = off;
		atomic_inc(&nic->rx_bufs_left[ring_no]);
		alloc_tab++;
	}

      end:
	return SUCCESS;
}

/*  
 *  Input Arguments: 
 *  device private variable.
 *  Return Value: 
 *  NONE.
 *  Description: 
 *  This function will free all Rx buffers allocated by host.
 */
static void freeRxBuffers(struct s2io_nic *sp)
{
	struct net_device *dev = sp->dev;
	int i, j, blk = 0, off, buf_cnt = 0;
	RxD_t *rxdp;
	struct sk_buff *skb;
	mac_info_t *mac_control;
	struct config_param *config;

	mac_control = &sp->mac_control;
	config = &sp->config;

	for (i = 0; i < config->RxRingNum; i++) {
		for (j = 0, blk = 0; j < config->RxCfg[i].NumRxd; j++) {
			off = j % (MAX_RXDS_PER_BLOCK + 1);
			rxdp = sp->rx_blocks[i][blk].block_virt_addr + off;

			if (rxdp->Control_1 == END_OF_BLOCK) {
				rxdp =
				    (RxD_t *) ((unsigned long) rxdp->
					       Control_2);
				j++;
				blk++;
			}

			skb =
			    (struct sk_buff *) ((unsigned long) rxdp->
						Host_Control);
			if (skb) {
				pci_unmap_single(sp->pdev, (dma_addr_t)
						 rxdp->Buffer0_ptr,
						 dev->mtu +
						 HEADER_ETHERNET_II_802_3_SIZE
						 + HEADER_802_2_SIZE +
						 HEADER_SNAP_SIZE,
						 PCI_DMA_FROMDEVICE);
				dev_kfree_skb(skb);
				atomic_dec(&sp->rx_bufs_left[i]);
				buf_cnt++;
			}
			memset(rxdp, 0, sizeof(RxD_t));
		}
		mac_control->rx_curr_put_info[i].block_index = 0;
		mac_control->rx_curr_get_info[i].block_index = 0;
		mac_control->rx_curr_put_info[i].offset = 0;
		mac_control->rx_curr_get_info[i].offset = 0;
		atomic_set(&sp->rx_bufs_left[i], 0);
		DBG_PRINT(INIT_DBG, "%s:Freed 0x%x Rx Buffers on ring%d\n",
			  dev->name, buf_cnt, i);
	}
}

/*
 *  Input Argument: 
 *   dev - pointer to the device structure.
 *   budget - The number of packets that were budgeted to be processed during
 *   one pass through the 'Poll" function.
 *  Return value:
 *   0 on success and 1 if there are No Rx packets to be processed.
 *  Description:
 *   Comes into picture only if NAPI support has been incorporated. It does
 *   the same thing that rxIntrHandler does, but not in a interrupt context
 *   also It will process only a given number of packets.
 */
#ifdef CONFIG_S2IO_NAPI
static int s2io_poll(struct net_device *dev, int *budget)
{
	nic_t *nic = dev->priv;
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) nic->bar0;
	int pkts_to_process = *budget, pkt_cnt = 0;
	register u64 val64 = 0;
	rx_curr_get_info_t offset_info;
	int i, block_no;
	u16 val16, cksum;
	struct sk_buff *skb;
	RxD_t *rxdp;
	mac_info_t *mac_control;
	struct config_param *config;

	mac_control = &nic->mac_control;
	config = &nic->config;

	if (pkts_to_process > dev->quota)
		pkts_to_process = dev->quota;

	val64 = readq(&bar0->rx_traffic_int);
	writeq(val64, &bar0->rx_traffic_int);

	for (i = 0; i < config->RxRingNum; i++) {
		if (--pkts_to_process < 0) {
			goto no_rx;
		}
		offset_info = mac_control->rx_curr_get_info[i];
		block_no = offset_info.block_index;
		rxdp = nic->rx_blocks[i][block_no].block_virt_addr +
		    offset_info.offset;
		while (!(rxdp->Control_1 & RXD_OWN_XENA)) {
			if (rxdp->Control_1 == END_OF_BLOCK) {
				rxdp =
				    (RxD_t *) ((unsigned long) rxdp->
					       Control_2);
				offset_info.offset++;
				offset_info.offset %=
				    (MAX_RXDS_PER_BLOCK + 1);
				block_no++;
				block_no %= nic->block_count[i];
				mac_control->rx_curr_get_info[i].
				    offset = offset_info.offset;
				mac_control->rx_curr_get_info[i].
				    block_index = block_no;
				continue;
			}
			skb =
			    (struct sk_buff *) ((unsigned long) rxdp->
						Host_Control);
			if (skb == NULL) {
				DBG_PRINT(ERR_DBG, "%s: The skb is ",
					  dev->name);
				DBG_PRINT(ERR_DBG, "Null in Rx Intr\n");
				return 0;
			}
			val64 = RXD_GET_BUFFER0_SIZE(rxdp->Control_2);
			val16 = (u16) (val64 >> 48);
			cksum = RXD_GET_L4_CKSUM(rxdp->Control_1);
			pci_unmap_single(nic->pdev, (dma_addr_t)
					 rxdp->Buffer0_ptr,
					 dev->mtu +
					 HEADER_ETHERNET_II_802_3_SIZE +
					 HEADER_802_2_SIZE +
					 HEADER_SNAP_SIZE,
					 PCI_DMA_FROMDEVICE);
			rxOsmHandler(nic, val16, rxdp, i);
			pkt_cnt++;
			offset_info.offset++;
			offset_info.offset %= (MAX_RXDS_PER_BLOCK + 1);
			rxdp =
			    nic->rx_blocks[i][block_no].block_virt_addr +
			    offset_info.offset;
			mac_control->rx_curr_get_info[i].offset =
			    offset_info.offset;
		}
	}
	if (!pkt_cnt)
		pkt_cnt = 1;

	for (i = 0; i < config->RxRingNum; i++)
		fill_rx_buffers(nic, i);

	dev->quota -= pkt_cnt;
	*budget -= pkt_cnt;
	netif_rx_complete(dev);

/* Re enable the Rx interrupts. */
	en_dis_able_NicIntrs(nic, RX_TRAFFIC_INTR, ENABLE_INTRS);
	return 0;

      no_rx:
	for (i = 0; i < config->RxRingNum; i++)
		fill_rx_buffers(nic, i);
	dev->quota -= pkt_cnt;
	*budget -= pkt_cnt;
	return 1;
}
#else
/*  
 *  Input Arguments: 
 *  device private variable.
 *  Return Value: 
 *  NONE.
 *  Description: 
 * If the interrupt is because of a received frame or if the 
 *  receive ring contains fresh as yet un-processed frames, this function is
 *  called. It picks out the RxD at which place the last Rx processing had 
 *  stopped and sends the skb to the OSM's Rx handler and then increments 
 *  the offset.
 */
static void rxIntrHandler(struct s2io_nic *nic)
{
	struct net_device *dev = (struct net_device *) nic->dev;
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) nic->bar0;
	rx_curr_get_info_t offset_info;
	RxD_t *rxdp;
	struct sk_buff *skb;
	u16 val16, cksum;
	register u64 val64 = 0;
	int i, block_no;
	mac_info_t *mac_control;
	struct config_param *config;

	mac_control = &nic->mac_control;
	config = &nic->config;

#if DEBUG_ON
	nic->rxint_cnt++;
#endif

/* rx_traffic_int reg is an R1 register, hence we read and write back 
 * the samevalue in the register to clear it.
 */
	val64 = readq(&bar0->rx_traffic_int);
	writeq(val64, &bar0->rx_traffic_int);

	for (i = 0; i < config->RxRingNum; i++) {
		offset_info = mac_control->rx_curr_get_info[i];
		block_no = offset_info.block_index;
		rxdp = nic->rx_blocks[i][block_no].block_virt_addr +
		    offset_info.offset;
		while (!(rxdp->Control_1 & RXD_OWN_XENA)) {
			if (rxdp->Control_1 == END_OF_BLOCK) {
				rxdp = (RxD_t *) ((unsigned long)
						  rxdp->Control_2);
				offset_info.offset++;
				offset_info.offset %=
				    (MAX_RXDS_PER_BLOCK + 1);
				block_no++;
				block_no %= nic->block_count[i];
				mac_control->rx_curr_get_info[i].
				    offset = offset_info.offset;
				mac_control->rx_curr_get_info[i].
				    block_index = block_no;
				continue;
			}
			skb = (struct sk_buff *) ((unsigned long)
						  rxdp->Host_Control);
			if (skb == NULL) {
				DBG_PRINT(ERR_DBG, "%s: The skb is ",
					  dev->name);
				DBG_PRINT(ERR_DBG, "Null in Rx Intr\n");
				return;
			}
			val64 = RXD_GET_BUFFER0_SIZE(rxdp->Control_2);
			val16 = (u16) (val64 >> 48);
			cksum = RXD_GET_L4_CKSUM(rxdp->Control_1);
			pci_unmap_single(nic->pdev, (dma_addr_t)
					 rxdp->Buffer0_ptr,
					 dev->mtu +
					 HEADER_ETHERNET_II_802_3_SIZE +
					 HEADER_802_2_SIZE +
					 HEADER_SNAP_SIZE,
					 PCI_DMA_FROMDEVICE);
			rxOsmHandler(nic, val16, rxdp, i);
			offset_info.offset++;
			offset_info.offset %= (MAX_RXDS_PER_BLOCK + 1);
			rxdp =
			    nic->rx_blocks[i][block_no].block_virt_addr +
			    offset_info.offset;
			mac_control->rx_curr_get_info[i].offset =
			    offset_info.offset;
		}
	}
}
#endif

/*  
 *  Input Arguments: 
 *  device private variable
 *  Return Value: 
 *  NONE
 *  Description: 
 *  If an interrupt was raised to indicate DMA complete of the 
 *  Tx packet, this function is called. It identifies the last TxD whose buffer
 *  was freed and frees all skbs whose data have already DMA'ed into the NICs
 *  internal memory.
 */
static void txIntrHandler(struct s2io_nic *nic)
{
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) nic->bar0;
	struct net_device *dev = (struct net_device *) nic->dev;
	tx_curr_get_info_t offset_info, offset_info1;
	struct sk_buff *skb;
	TxD_t *txdlp;
	register u64 val64 = 0;
	int i;
	u16 j, frg_cnt;
	mac_info_t *mac_control;
	struct config_param *config;
#if DEBUG_ON
	int cnt = 0;
	nic->txint_cnt++;
#endif

	mac_control = &nic->mac_control;
	config = &nic->config;

	/* tx_traffic_int reg is an R1 register, hence we read and write 
	 * back the samevalue in the register to clear it.
	 */
	val64 = readq(&bar0->tx_traffic_int);
	writeq(val64, &bar0->tx_traffic_int);

	for (i = 0; i < config->TxFIFONum; i++) {
		offset_info = mac_control->tx_curr_get_info[i];
		offset_info1 = mac_control->tx_curr_put_info[i];
		txdlp = mac_control->txdl_start[i] +
		    (config->MaxTxDs * offset_info.offset);
		while ((!(txdlp->Control_1 & TXD_LIST_OWN_XENA)) &&
		       (offset_info.offset != offset_info1.offset) &&
		       (txdlp->Host_Control)) {
			/* Check for TxD errors */
			if (txdlp->Control_1 & TXD_T_CODE) {
				unsigned long long err;
				err = txdlp->Control_1 & TXD_T_CODE;
				DBG_PRINT(ERR_DBG, "***TxD error %llx\n",
					  err);
			}

			skb = (struct sk_buff *) ((unsigned long)
						  txdlp->Host_Control);
			if (skb == NULL) {
				DBG_PRINT(ERR_DBG, "%s: Null skb ",
					  dev->name);
				DBG_PRINT(ERR_DBG, "in Tx Free Intr\n");
				return;
			}
			nic->tx_pkt_count++;

			frg_cnt = skb_shinfo(skb)->nr_frags;

			/*  For unfragmented skb */
			pci_unmap_single(nic->pdev, (dma_addr_t)
					 txdlp->Buffer_Pointer,
					 skb->len - skb->data_len,
					 PCI_DMA_TODEVICE);
			if (frg_cnt) {
				TxD_t *temp = txdlp;
				txdlp++;
				for (j = 0; j < frg_cnt; j++, txdlp++) {
					skb_frag_t *frag =
					    &skb_shinfo(skb)->frags[j];
					pci_unmap_page(nic->pdev,
						       (dma_addr_t)
						       txdlp->
						       Buffer_Pointer,
						       frag->size,
						       PCI_DMA_TODEVICE);
				}
				txdlp = temp;
			}
			memset(txdlp, 0,
			       (sizeof(TxD_t) * config->MaxTxDs));

			/* Updating the statistics block */
			nic->stats.tx_packets++;
			nic->stats.tx_bytes += skb->len;
#if DEBUG_ON
			nic->txpkt_bytes += skb->len;
			cnt++;
#endif
			dev_kfree_skb_irq(skb);

			offset_info.offset++;
			offset_info.offset %= offset_info.fifo_len + 1;
			txdlp = mac_control->txdl_start[i] +
			    (config->MaxTxDs * offset_info.offset);
			mac_control->tx_curr_get_info[i].offset =
			    offset_info.offset;
		}
#if DEBUG_ON
		DBG_PRINT(INTR_DBG, "%s: freed %d Tx Pkts\n", dev->name,
			  cnt);
#endif
	}

	spin_lock(&nic->tx_lock);
	if (netif_queue_stopped(dev))
		netif_wake_queue(dev);
	spin_unlock(&nic->tx_lock);
}

/*  
 *  Input Arguments: 
 *  device private variable
 *  Return Value: 
 *  NONE
 *  Description: 
 *  If the interrupt was neither because of Rx packet or Tx 
 *  complete, this function is called. If the interrupt was to indicate a loss
 *  of link, the OSM link status handler is invoked for any other alarm 
 *  interrupt the block that raised the interrupt is displayed and a H/W reset 
 *  is issued.
 */
static void alarmIntrHandler(struct s2io_nic *nic)
{
	struct net_device *dev = (struct net_device *) nic->dev;
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) nic->bar0;
	register u64 val64 = 0, err_reg = 0;


	/* Handling link status change error Intr */
	err_reg = readq(&bar0->mac_rmac_err_reg);
	if (err_reg & RMAC_LINK_STATE_CHANGE_INT) {
		schedule_work(&nic->set_link_task);
	}

	/* Handling SERR errors by stopping device Xmit queue and forcing 
	 * a H/W reset.
	 */
	val64 = readq(&bar0->serr_source);
	if (val64 & SERR_SOURCE_ANY) {
		DBG_PRINT(ERR_DBG, "%s: Device indicates ", dev->name);
		DBG_PRINT(ERR_DBG, "serious error!!\n");
		netif_stop_queue(dev);
	}
/* Other type of interrupts are not being handled now,  TODO*/
}

/*
 *  Input Argument: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  Return value:
 *   SUCCESS on success and FAILURE on failure.
 *  Description:
 *   Function that waits for a command to Write into RMAC ADDR DATA registers 
 *   to be completed and returns either success or error depending on whether 
 *   the command was complete or not. 
 */
int waitForCmdComplete(nic_t * sp)
{
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) sp->bar0;
	int ret = FAILURE, cnt = 0;
	u64 val64;

	while (TRUE) {
		val64 =
		    RMAC_ADDR_CMD_MEM_RD | RMAC_ADDR_CMD_MEM_STROBE_NEW_CMD
		    | RMAC_ADDR_CMD_MEM_OFFSET(0);
		writeq(val64, &bar0->rmac_addr_cmd_mem);
		val64 = readq(&bar0->rmac_addr_cmd_mem);
		if (!val64) {
			ret = SUCCESS;
			break;
		}
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ / 20);
		if (cnt++ > 10)
			break;
	}

	return ret;
}

/*
 *  Input Argument: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  Return value:
 *   void.
 *  Description:
 *   Function to Reset the card. This function then also restores the previously
 *   saved PCI configuration space registers as the card reset also resets the
 *   Configration space.
 */
void s2io_reset(nic_t * sp)
{
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) sp->bar0;
	u64 val64;
	u16 subid;

	val64 = SW_RESET_ALL;
	writeq(val64, &bar0->sw_reset);

	/* At this stage, if the PCI write is indeed completed, the 
	 * card is reset and so is the PCI Config space of the device. 
	 * So a read cannot be issued at this stage on any of the 
	 * registers to ensure the write into "sw_reset" register
	 * has gone through.
	 * Question: Is there any system call that will explicitly force
	 * all the write commands still pending on the bus to be pushed
	 * through?
	 * As of now I'am just giving a 250ms delay and hoping that the
	 * PCI write to sw_reset register is done by this time.
	 */
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(HZ / 4);

	/* Restore the PCI state saved during initializarion. */
	pci_restore_state(sp->pdev, sp->config_space);
	s2io_init_pci(sp);

	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(HZ / 4);

	/* SXE-002: Configure link and activity LED to turn it off */
	subid = sp->pdev->subsystem_device;
	if ((subid & 0xFF) >= 0x07) {
		val64 = readq(&bar0->gpio_control);
		val64 |= 0x0000800000000000ULL;
		writeq(val64, &bar0->gpio_control);
		val64 = 0x0411040400000000ULL;
		writeq(val64, (void *) ((u8 *) bar0 + 0x2700));
	}

	sp->device_enabled_once = FALSE;
}

/*
 *  Input Argument: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  Return value:
 *  SUCCESS on success and FAILURE on failure.
 *  Description:
 * Function to set the swapper control on the card correctly depending on the
 * 'endianness' of the system.
 */
int s2io_set_swapper(nic_t * sp)
{
	struct net_device *dev = sp->dev;
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) sp->bar0;
	u64 val64;

/*  Set proper endian settings and verify the same by reading the PIF 
 *  Feed-back register.
 */
#ifdef  __BIG_ENDIAN
/* The device by default set to a big endian format, so a big endian 
 * driver need not set anything.
 */
	writeq(0xffffffffffffffffULL, &bar0->swapper_ctrl);
	val64 = (SWAPPER_CTRL_PIF_R_FE |
		 SWAPPER_CTRL_PIF_R_SE |
		 SWAPPER_CTRL_PIF_W_FE |
		 SWAPPER_CTRL_PIF_W_SE |
		 SWAPPER_CTRL_TXP_FE |
		 SWAPPER_CTRL_TXP_SE |
		 SWAPPER_CTRL_TXD_R_FE |
		 SWAPPER_CTRL_TXD_W_FE |
		 SWAPPER_CTRL_TXF_R_FE |
		 SWAPPER_CTRL_RXD_R_FE |
		 SWAPPER_CTRL_RXD_W_FE |
		 SWAPPER_CTRL_RXF_W_FE |
		 SWAPPER_CTRL_XMSI_FE |
		 SWAPPER_CTRL_XMSI_SE |
		 SWAPPER_CTRL_STATS_FE | SWAPPER_CTRL_STATS_SE);
	writeq(val64, &bar0->swapper_ctrl);
#else
/* Initially we enable all bits to make it accessible by the driver,
 * then we selectively enable only those bits that we want to set.
 */
	writeq(0xffffffffffffffffULL, &bar0->swapper_ctrl);
	val64 = (SWAPPER_CTRL_PIF_R_FE |
		 SWAPPER_CTRL_PIF_R_SE |
		 SWAPPER_CTRL_PIF_W_FE |
		 SWAPPER_CTRL_PIF_W_SE |
		 SWAPPER_CTRL_TXP_FE |
		 SWAPPER_CTRL_TXP_SE |
		 SWAPPER_CTRL_TXD_R_FE |
		 SWAPPER_CTRL_TXD_R_SE |
		 SWAPPER_CTRL_TXD_W_FE |
		 SWAPPER_CTRL_TXD_W_SE |
		 SWAPPER_CTRL_TXF_R_FE |
		 SWAPPER_CTRL_RXD_R_FE |
		 SWAPPER_CTRL_RXD_R_SE |
		 SWAPPER_CTRL_RXD_W_FE |
		 SWAPPER_CTRL_RXD_W_SE |
		 SWAPPER_CTRL_RXF_W_FE |
		 SWAPPER_CTRL_XMSI_FE |
		 SWAPPER_CTRL_XMSI_SE |
		 SWAPPER_CTRL_STATS_FE | SWAPPER_CTRL_STATS_SE);
	writeq(val64, &bar0->swapper_ctrl);
#endif

/*  Verifying if endian settings are accurate by reading a feedback
 *  register.
 */
	val64 = readq(&bar0->pif_rd_swapper_fb);
	if (val64 != 0x0123456789ABCDEFULL) {
		/* Endian settings are incorrect, calls for another dekko. */
		DBG_PRINT(ERR_DBG, "%s: Endian settings are wrong, ",
			  dev->name);
		DBG_PRINT(ERR_DBG, "feedback read %llx\n",
			  (unsigned long long) val64);
		return FAILURE;
	}

	return SUCCESS;
}

/* ********************************************************* *
 * Functions defined below concern the OS part of the driver *
 * ********************************************************* */

/*
 *  Input Argument: 
 *  dev - pointer to the device structure.
 *  Return value:
 *  '0' on success and an appropriate (-)ve integer as defined in errno.h
 *   file on failure.
 *  Description:
 *  This function is the open entry point of the driver. It mainly calls a
 *  function to allocate Rx buffers and inserts them into the buffer
 *  descriptors and then enables the Rx part of the NIC. 
 */
int s2io_open(struct net_device *dev)
{
	nic_t *sp = dev->priv;
	int i, ret = 0, err = 0;
	mac_info_t *mac_control;
	struct config_param *config;


/* Make sure you have link off by default every time Nic is initialized*/
	netif_carrier_off(dev);
	sp->last_link_state = LINK_DOWN;

/*  Initialize the H/W I/O registers */
	if (initNic(sp) != 0) {
		DBG_PRINT(ERR_DBG, "%s: H/W initialization failed\n",
			  dev->name);
		return -ENODEV;
	}

/*  After proper initialization of H/W, register ISR */
	err =
	    request_irq((int) sp->irq, s2io_isr, SA_SHIRQ, sp->name, dev);
	if (err) {
		s2io_reset(sp);
		DBG_PRINT(ERR_DBG, "%s: ISR registration failed\n",
			  dev->name);
		return err;
	}
	if (s2io_set_mac_addr(dev, dev->dev_addr) == FAILURE) {
		DBG_PRINT(ERR_DBG, "Set Mac Address Failed\n");
		s2io_reset(sp);
		return -ENODEV;
	}


/*  Setting its receive mode */
	s2io_set_multicast(dev);

/*  Initializing the Rx buffers. For now we are considering only 1 Rx ring
 * and initializing buffers into 1016 RxDs or 8 Rx blocks
 */
	mac_control = &sp->mac_control;
	config = &sp->config;

	for (i = 0; i < config->RxRingNum; i++) {
		if ((ret = fill_rx_buffers(sp, i))) {
			DBG_PRINT(ERR_DBG, "%s: Out of memory in Open\n",
				  dev->name);
			s2io_reset(sp);
			free_irq(dev->irq, dev);
			freeRxBuffers(sp);
			return -ENOMEM;
		}
		DBG_PRINT(INFO_DBG, "Buf in ring:%d is %d:\n", i,
			  atomic_read(&sp->rx_bufs_left[i]));
	}

/*  Enable tasklet for the device */
	tasklet_init(&sp->task, s2io_tasklet, (unsigned long) dev);

/*  Enable Rx Traffic and interrupts on the NIC */
	if (startNic(sp)) {
		DBG_PRINT(ERR_DBG, "%s: Starting NIC failed\n", dev->name);
		tasklet_kill(&sp->task);
		s2io_reset(sp);
		free_irq(dev->irq, dev);
		freeRxBuffers(sp);
		return -ENODEV;
	}

	sp->device_close_flag = FALSE;	/* Device is up and running. */
	netif_start_queue(dev);

	return 0;
}

/*
 *  Input Argument/s: 
 *  dev - device pointer.
 *  Return value:
 *  '0' on success and an appropriate (-)ve integer as defined in errno.h
 *  file on failure.
 *  Description:
 *  This is the stop entry point of the driver. It needs to undo exactly
 *  whatever was done by the open entry point, thus it's usually referred to
 *  as the close function. Among other things this function mainly stops the
 *  Rx side of the NIC and frees all the Rx buffers in the Rx rings.
 */
int s2io_close(struct net_device *dev)
{
	nic_t *sp = dev->priv;
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) sp->bar0;
	register u64 val64 = 0;
	u16 cnt = 0;

	spin_lock(&sp->isr_lock);
	netif_stop_queue(dev);

/* disable Tx and Rx traffic on the NIC */
	stopNic(sp);

	spin_unlock(&sp->isr_lock);

/* If the device tasklet is running, wait till its done before killing it */
	while (atomic_read(&(sp->tasklet_status))) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ / 10);
	}
	tasklet_kill(&sp->task);

/* Check if the device is Quiescent and then Reset the NIC */
	do {
		val64 = readq(&bar0->adapter_status);
		if (verify_xena_quiescence(val64, sp->device_enabled_once)) {
			break;
		}

		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ / 20);
		cnt++;
		if (cnt == 10) {
			DBG_PRINT(ERR_DBG,
				  "s2io_close:Device not Quiescent ");
			DBG_PRINT(ERR_DBG, "adaper status reads 0x%llx\n",
				  (unsigned long long) val64);
			break;
		}
	} while (1);
	s2io_reset(sp);

/*  Free the Registered IRQ */
	free_irq(dev->irq, dev);

/* Free all Tx Buffers waiting for transmission */
	freeTxBuffers(sp);

/*  Free all Rx buffers allocated by host */
	freeRxBuffers(sp);

	sp->device_close_flag = TRUE;	/* Device is shut down. */

	return 0;
}

/*
 *  Input Argument/s: 
 *  skb - the socket buffer containing the Tx data.
 *  dev - device pointer.
 *  Return value:
 *  '0' on success & 1 on failure. 
 *  NOTE: when device cant queue the pkt, just the trans_start variable will
 *  not be upadted.
 *  Description:
 *  This function is the Tx entry point of the driver. S2IO NIC supports
 *  certain protocol assist features on Tx side, namely  CSO, S/G, LSO.
 */
int s2io_xmit(struct sk_buff *skb, struct net_device *dev)
{
	nic_t *sp = dev->priv;
	u16 off, txd_len, frg_cnt, frg_len, i, queue, off1, queue_len;
	register u64 val64;
	TxD_t *txdp;
	TxFIFO_element_t *tx_fifo;
	unsigned long flags;
#ifdef NETIF_F_TSO
	int mss;
#endif
	mac_info_t *mac_control;
	struct config_param *config;

	mac_control = &sp->mac_control;
	config = &sp->config;

	DBG_PRINT(TX_DBG, "%s: In S2IO Tx routine\n", dev->name);

	spin_lock_irqsave(&sp->tx_lock, flags);
	queue = 0;
	/* Multi FIFO Tx is disabled for now. */
	if (!queue && tx_prio) {
		u8 x = (skb->data)[5];
		queue = x % config->TxFIFONum;
	}


	off = (u16) mac_control->tx_curr_put_info[queue].offset;
	off1 = (u16) mac_control->tx_curr_get_info[queue].offset;
	txd_len = mac_control->txdl_len;
	txdp = mac_control->txdl_start[queue] + (config->MaxTxDs * off);

	queue_len = mac_control->tx_curr_put_info[queue].fifo_len + 1;
	/* Avoid "put" pointer going beyond "get" pointer */
	if (txdp->Host_Control || (((off + 1) % queue_len) == off1)) {
		DBG_PRINT(ERR_DBG, "Error in xmit, No free TXDs.\n");
		netif_stop_queue(dev);
		dev_kfree_skb(skb);
		spin_unlock_irqrestore(&sp->tx_lock, flags);
		return 0;
	}

#ifdef NETIF_F_TSO
	mss = skb_shinfo(skb)->tso_size;
	if (mss) {
		txdp->Control_1 |= TXD_TCP_LSO_EN;
		txdp->Control_1 |= TXD_TCP_LSO_MSS(mss);
	}
#endif

	frg_cnt = skb_shinfo(skb)->nr_frags;
	frg_len = skb->len - skb->data_len;

	txdp->Host_Control = (unsigned long) skb;
	txdp->Buffer_Pointer = pci_map_single
	    (sp->pdev, skb->data, frg_len, PCI_DMA_TODEVICE);
	if (skb->ip_summed == CHECKSUM_HW) {
		txdp->Control_2 |=
		    (TXD_TX_CKO_IPV4_EN | TXD_TX_CKO_TCP_EN |
		     TXD_TX_CKO_UDP_EN);
	}

	txdp->Control_2 |= config->TxIntrType;

	txdp->Control_1 |= (TXD_BUFFER0_SIZE(frg_len) |
			    TXD_GATHER_CODE_FIRST);
	txdp->Control_1 |= TXD_LIST_OWN_XENA;

	/* For fragmented SKB. */
	for (i = 0; i < frg_cnt; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		txdp++;
		txdp->Buffer_Pointer = (u64) pci_map_page
		    (sp->pdev, frag->page, frag->page_offset,
		     frag->size, PCI_DMA_TODEVICE);
		txdp->Control_1 |= TXD_BUFFER0_SIZE(frag->size);
	}
	txdp->Control_1 |= TXD_GATHER_CODE_LAST;

	tx_fifo = mac_control->tx_FIFO_start[queue];
	val64 = (mac_control->txdl_start_phy[queue] +
		 (sizeof(TxD_t) * txd_len * off));
	writeq(val64, &tx_fifo->TxDL_Pointer);

	val64 = (TX_FIFO_LAST_TXD_NUM(frg_cnt) | TX_FIFO_FIRST_LIST |
		 TX_FIFO_LAST_LIST);
#ifdef NETIF_F_TSO
	if (mss)
		val64 |= TX_FIFO_SPECIAL_FUNC;
#endif
	writeq(val64, &tx_fifo->List_Control);

	off++;
	off %= mac_control->tx_curr_put_info[queue].fifo_len + 1;
	mac_control->tx_curr_put_info[queue].offset = off;

	/* Avoid "put" pointer going beyond "get" pointer */
	if (((off + 1) % queue_len) == off1) {
		DBG_PRINT(TX_DBG, 
		  "No free TxDs for xmit, Put: 0x%x Get:0x%x\n",
		  off, off1);
		netif_stop_queue(dev);
	}

	dev->trans_start = jiffies;
	spin_unlock_irqrestore(&sp->tx_lock, flags);

	return 0;
}

/*
 *  Input Argument/s: 
 *  irq: the irq of the device.
 *  dev_id: a void pointer to the dev structure of the NIC.
 *  ptregs: pointer to the registers pushed on the stack.
 *  Return value:
 *  void.
 *  Description:
 *  This function is the ISR handler of the device. It identifies the reason 
 *  for the interrupt and calls the relevant service routines.
 *  As a contongency measure, this ISR allocates the recv buffers, if their 
 *  numbers are below the panic value which is presently set to 25% of the
 *  original number of rcv buffers allocated.
 */

static irqreturn_t s2io_isr(int irq, void *dev_id, struct pt_regs *regs)
{
	struct net_device *dev = (struct net_device *) dev_id;
	nic_t *sp = dev->priv;
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) sp->bar0;
	u64 reason = 0, general_mask = 0;
	mac_info_t *mac_control;
	struct config_param *config;

	mac_control = &sp->mac_control;
	config = &sp->config;

	spin_lock(&sp->isr_lock);

	/* Identify the cause for interrupt and call the appropriate
	 * interrupt handler. Causes for the interrupt could be;
	 * 1. Rx of packet.
	 * 2. Tx complete.
	 * 3. Link down.
	 * 4. Error in any functional blocks of the NIC. 
	 */
	reason = readq(&bar0->general_int_status);

	if (!reason) {
		/* The interrupt was not raised by Xena. */
		spin_unlock(&sp->isr_lock);
		return IRQ_NONE;
	}
	/* Mask the interrupts on the NIC */
	general_mask = readq(&bar0->general_int_mask);
	writeq(0xFFFFFFFFFFFFFFFFULL, &bar0->general_int_mask);

#if DEBUG_ON
	sp->int_cnt++;
#endif

	/* If Intr is because of Tx Traffic */
	if (reason & GEN_INTR_TXTRAFFIC) {
		txIntrHandler(sp);
	}

	/* If Intr is because of an error */
	if (reason & (GEN_ERROR_INTR))
		alarmIntrHandler(sp);

#ifdef CONFIG_S2IO_NAPI
	if (reason & GEN_INTR_RXTRAFFIC) {
		if (netif_rx_schedule_prep(dev)) {
			en_dis_able_NicIntrs(sp, RX_TRAFFIC_INTR,
					     DISABLE_INTRS);
			/* We retake the snap shot of the general interrupt 
			 * register.
			 */
			general_mask = readq(&bar0->general_int_mask);
			__netif_rx_schedule(dev);
		}
	}
#else
	/* If Intr is because of Rx Traffic */
	if (reason & GEN_INTR_RXTRAFFIC) {
		rxIntrHandler(sp);
	}
#endif

/* If the Rx buffer count is below the panic threshold then reallocate the
 * buffers from the interrupt handler itself, else schedule a tasklet to 
 * reallocate the buffers.
 */
#if 1
	{
	int i;

	for (i = 0; i < config->RxRingNum; i++) {
		int rxb_size = atomic_read(&sp->rx_bufs_left[i]);
		int level = rx_buffer_level(sp, rxb_size, i);

		if ((level == PANIC) && (!TASKLET_IN_USE)) {
			int ret;

			DBG_PRINT(ERR_DBG, "%s: Rx BD hit ", dev->name);
			DBG_PRINT(ERR_DBG, "PANIC levels\n");
			if ((ret = fill_rx_buffers(sp, i)) == -ENOMEM) {
				DBG_PRINT(ERR_DBG, "%s:Out of memory",
					  dev->name);
				DBG_PRINT(ERR_DBG, " in ISR!!\n");
				writeq(general_mask,
				       &bar0->general_int_mask);
				spin_unlock(&sp->isr_lock);
				return IRQ_HANDLED;
			}
			clear_bit(0,
				  (unsigned long *) (&sp->tasklet_status));
		} else if ((level == LOW)
			   && (!atomic_read(&sp->tasklet_status))) {
			tasklet_schedule(&sp->task);
		}

	}

	}
#else
	tasklet_schedule(&sp->task);
#endif

	/* Unmask all the previously enabled interrupts on the NIC */
	writeq(general_mask, &bar0->general_int_mask);

	spin_unlock(&sp->isr_lock);
	return IRQ_HANDLED;
}

/*
 *  Input Argument/s: 
 *  dev - pointer to the device structure.
 *  Return value:
 *  pointer to the updated net_device_stats structure.
 *  Description:
 *  This function updates the device statistics structure in the s2io_nic 
 *  structure and returns a pointer to the same.
 */
struct net_device_stats *s2io_get_stats(struct net_device *dev)
{
	nic_t *sp = dev->priv;
	mac_info_t *mac_control;
	struct config_param *config;

	mac_control = &sp->mac_control;
	config = &sp->config;

	sp->stats.tx_errors = mac_control->StatsInfo->tmac_any_err_frms;
	sp->stats.rx_errors = mac_control->StatsInfo->rmac_drop_frms;
	sp->stats.multicast = mac_control->StatsInfo->rmac_vld_mcst_frms;
	sp->stats.rx_length_errors =
	    mac_control->StatsInfo->rmac_long_frms;

	return (&sp->stats);
}

/*
 *  Input Argument/s: 
 *  dev - pointer to the device structure
 *  Return value:
 *  void.
 *  Description:
 *  This function is a driver entry point which gets called by the kernel 
 *  whenever multicast addresses must be enabled/disabled. This also gets 
 *  called to set/reset promiscuous mode. Depending on the deivce flag, we
 *  determine, if multicast address must be enabled or if promiscuous mode
 *  is to be disabled etc.
 */
static void s2io_set_multicast(struct net_device *dev)
{
	int i, j, prev_cnt;
	struct dev_mc_list *mclist;
	nic_t *sp = dev->priv;
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) sp->bar0;
	u64 val64 = 0, multi_mac = 0x010203040506ULL, mask =
	    0xfeffffffffffULL;
	u64 dis_addr = 0xffffffffffffULL, mac_addr = 0;
	void *add;

	if ((dev->flags & IFF_ALLMULTI) && (!sp->m_cast_flg)) {
		/*  Enable all Multicast addresses */
		writeq(RMAC_ADDR_DATA0_MEM_ADDR(multi_mac),
		       &bar0->rmac_addr_data0_mem);
		writeq(RMAC_ADDR_DATA1_MEM_MASK(mask),
		       &bar0->rmac_addr_data1_mem);
		val64 = RMAC_ADDR_CMD_MEM_WE |
		    RMAC_ADDR_CMD_MEM_STROBE_NEW_CMD |
		    RMAC_ADDR_CMD_MEM_OFFSET(MAC_MC_ALL_MC_ADDR_OFFSET);
		writeq(val64, &bar0->rmac_addr_cmd_mem);
		/* Wait till command completes */
		waitForCmdComplete(sp);

		sp->m_cast_flg = 1;
		sp->all_multi_pos = MAC_MC_ALL_MC_ADDR_OFFSET;
	} else if ((dev->flags & IFF_ALLMULTI) && (sp->m_cast_flg)) {
		/*  Disable all Multicast addresses */
		writeq(RMAC_ADDR_DATA0_MEM_ADDR(dis_addr),
		       &bar0->rmac_addr_data0_mem);
		val64 = RMAC_ADDR_CMD_MEM_WE |
		    RMAC_ADDR_CMD_MEM_STROBE_NEW_CMD |
		    RMAC_ADDR_CMD_MEM_OFFSET(sp->all_multi_pos);
		writeq(val64, &bar0->rmac_addr_cmd_mem);
		/* Wait till command completes */
		waitForCmdComplete(sp);

		sp->m_cast_flg = 0;
		sp->all_multi_pos = 0;
	}

	if ((dev->flags & IFF_PROMISC) && (!sp->promisc_flg)) {
		/*  Put the NIC into promiscuous mode */
		add = (void *) &bar0->mac_cfg;
		val64 = readq(&bar0->mac_cfg);
		val64 |= MAC_CFG_RMAC_PROM_ENABLE;

		writeq(RMAC_CFG_KEY(0x4C0D), &bar0->rmac_cfg_key);
		writel((u32) val64, add);
		writeq(RMAC_CFG_KEY(0x4C0D), &bar0->rmac_cfg_key);
		writel((u32) (val64 >> 32), (add + 4));

		val64 = readq(&bar0->mac_cfg);
		sp->promisc_flg = 1;
		DBG_PRINT(ERR_DBG, "%s: entered promiscuous mode\n",
			  dev->name);
	} else if (!(dev->flags & IFF_PROMISC) && (sp->promisc_flg)) {
		/*  Remove the NIC from promiscuous mode */
		add = (void *) &bar0->mac_cfg;
		val64 = readq(&bar0->mac_cfg);
		val64 &= ~MAC_CFG_RMAC_PROM_ENABLE;

		writeq(RMAC_CFG_KEY(0x4C0D), &bar0->rmac_cfg_key);
		writel((u32) val64, add);
		writeq(RMAC_CFG_KEY(0x4C0D), &bar0->rmac_cfg_key);
		writel((u32) (val64 >> 32), (add + 4));

		val64 = readq(&bar0->mac_cfg);
		sp->promisc_flg = 0;
		DBG_PRINT(ERR_DBG, "%s: left promiscuous mode\n",
			  dev->name);
	}

	/*  Update individual M_CAST address list */
	if ((!sp->m_cast_flg) && dev->mc_count) {
		if (dev->mc_count >
		    (MAX_ADDRS_SUPPORTED - MAC_MC_ADDR_START_OFFSET - 1)) {
			DBG_PRINT(ERR_DBG, "%s: No more Rx filters ",
				  dev->name);
			DBG_PRINT(ERR_DBG, "can be added, please enable ");
			DBG_PRINT(ERR_DBG, "ALL_MULTI instead\n");
			return;
		}

		prev_cnt = sp->mc_addr_count;
		sp->mc_addr_count = dev->mc_count;

		/* Clear out the previous list of Mc in the H/W. */
		for (i = 0; i < prev_cnt; i++) {
			writeq(RMAC_ADDR_DATA0_MEM_ADDR(dis_addr),
			       &bar0->rmac_addr_data0_mem);
			val64 = RMAC_ADDR_CMD_MEM_WE |
			    RMAC_ADDR_CMD_MEM_STROBE_NEW_CMD |
			    RMAC_ADDR_CMD_MEM_OFFSET
			    (MAC_MC_ADDR_START_OFFSET + i);
			writeq(val64, &bar0->rmac_addr_cmd_mem);

			/* Wait for command completes */
			if (waitForCmdComplete(sp)) {
				DBG_PRINT(ERR_DBG, "%s: Adding ",
					  dev->name);
				DBG_PRINT(ERR_DBG, "Multicasts failed\n");
				return;
			}
		}

		/* Create the new Rx filter list and update the same in H/W. */
		for (i = 0, mclist = dev->mc_list; i < dev->mc_count;
		     i++, mclist = mclist->next) {
			memcpy(sp->usr_addrs[i].addr, mclist->dmi_addr,
			       ETH_ALEN);
			for (j = 0; j < ETH_ALEN; j++) {
				mac_addr |= mclist->dmi_addr[j];
				mac_addr <<= 8;
			}
			writeq(RMAC_ADDR_DATA0_MEM_ADDR(mac_addr),
			       &bar0->rmac_addr_data0_mem);

			val64 = RMAC_ADDR_CMD_MEM_WE |
			    RMAC_ADDR_CMD_MEM_STROBE_NEW_CMD |
			    RMAC_ADDR_CMD_MEM_OFFSET
			    (i + MAC_MC_ADDR_START_OFFSET);
			writeq(val64, &bar0->rmac_addr_cmd_mem);

			/* Wait for command completes */
			if (waitForCmdComplete(sp)) {
				DBG_PRINT(ERR_DBG, "%s: Adding ",
					  dev->name);
				DBG_PRINT(ERR_DBG, "Multicasts failed\n");
				return;
			}
		}
	}
}

/*
 *  Input Argument/s: 
 *  dev - pointer to the device structure.
 *  new_mac - a uchar pointer to the new mac address which is to be set.
 *  Return value:
 *  SUCCESS on success and an appropriate (-)ve integer as defined in errno.h
 *  file on failure.
 *  Description:
 *  This procedure will program the Xframe to receive frames with new
 *  Mac Address
 */
int s2io_set_mac_addr(struct net_device *dev, u8 * addr)
{
	nic_t *sp = dev->priv;
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) sp->bar0;
	register u64 val64, mac_addr = 0;
	int i;

	/* 
	 * Set the new MAC address as the new unicast filter and reflect this
	 * change on the device address registered with the OS. It will be
	 * at offset 0. 
	 */
	for (i = 0; i < ETH_ALEN; i++) {
		mac_addr <<= 8;
		mac_addr |= addr[i];
	}

	writeq(RMAC_ADDR_DATA0_MEM_ADDR(mac_addr),
	       &bar0->rmac_addr_data0_mem);

	val64 =
	    RMAC_ADDR_CMD_MEM_WE | RMAC_ADDR_CMD_MEM_STROBE_NEW_CMD |
	    RMAC_ADDR_CMD_MEM_OFFSET(0);
	writeq(val64, &bar0->rmac_addr_cmd_mem);
	/* Wait till command completes */
	if (waitForCmdComplete(sp)) {
		DBG_PRINT(ERR_DBG, "%s: set_mac_addr failed\n", dev->name);
		return FAILURE;
	}

	return SUCCESS;
}

/*
 * Input Argument/s: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  info - pointer to the structure with parameters given by ethtool to set
 *  link information.
 * Return value:
 *  0 on success.
 * Description:
 *  The function sets different link parameters provided by the user onto 
 *  the NIC.
 */
static int s2io_ethtool_sset(struct net_device *dev,
			     struct ethtool_cmd *info)
{
	nic_t *sp = dev->priv;
	if ((info->autoneg == AUTONEG_ENABLE) ||
	    (info->speed != SPEED_10000) || (info->duplex != DUPLEX_FULL))
		return -EINVAL;
	else {
		s2io_close(sp->dev);
		s2io_open(sp->dev);
	}

	return 0;
}

/*
 * Input Argument/s: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  info - pointer to the structure with parameters given by ethtool to return
 *  link information.
 * Return value:
 *  void
 * Description:
 *  Returns link specefic information like speed, duplex etc.. to ethtool.
 */
int s2io_ethtool_gset(struct net_device *dev, struct ethtool_cmd *info)
{
	nic_t *sp = dev->priv;
	info->supported = (SUPPORTED_10000baseT_Full | SUPPORTED_FIBRE);
	info->advertising = (SUPPORTED_10000baseT_Full | SUPPORTED_FIBRE);
	info->port = PORT_FIBRE;
	/* info->transceiver?? TODO */

	if (netif_carrier_ok(sp->dev)) {
		info->speed = 10000;
		info->duplex = DUPLEX_FULL;
	} else {
		info->speed = -1;
		info->duplex = -1;
	}

	info->autoneg = AUTONEG_DISABLE;
	return 0;
}

/*
 * Input Argument/s: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  info - pointer to the structure with parameters given by ethtool to return
 *  driver information.
 * Return value:
 *  void
 * Description:
 *  Returns driver specefic information like name, version etc.. to ethtool.
 */
static void s2io_ethtool_gdrvinfo(struct net_device *dev,
				  struct ethtool_drvinfo *info)
{
	nic_t *sp = dev->priv;

	strncpy(info->driver, s2io_driver_name, sizeof(s2io_driver_name));
	strncpy(info->version, s2io_driver_version,
		sizeof(s2io_driver_version));
	strncpy(info->fw_version, "", 32);
	strncpy(info->bus_info, sp->pdev->slot_name, 32);
	info->regdump_len = XENA_REG_SPACE;
	info->eedump_len = XENA_EEPROM_SPACE;
	info->testinfo_len = S2IO_TEST_LEN;
	info->n_stats = S2IO_STAT_LEN;
}

/*
 * Input Argument/s: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  regs - pointer to the structure with parameters given by ethtool for 
 *  dumping the registers.
 *  reg_space - The input argumnet into which all the registers are dumped.
 * Return value:
 *  void
 * Description:
 *  Dumps the entire register space of xFrame NIC into the user given buffer 
 *  area.
 */
static void s2io_ethtool_gregs(struct net_device *dev,
			       struct ethtool_regs *regs, void *space)
{
	int i;
	u64 reg;
	u8 *reg_space = (u8 *) space;
	nic_t *sp = dev->priv;

	regs->len = XENA_REG_SPACE;
	regs->version = sp->pdev->subsystem_device;

	for (i = 0; i < regs->len; i += 8) {
		reg = readq((void *) (sp->bar0 + i));
		memcpy((reg_space + i), &reg, 8);
	}
}

/*
 * Input Argument/s: 
 *  data - address of the private member of the device structure, which 
 *  is a pointer to the s2io_nic structure, provided as an u32.
 * Return value:
 *  void
 * Description:
 *  This is actually the timer function that alternates the adapter LED bit
 *  of the adapter control bit to set/reset every time on invocation.
 *  The timer is set for 1/2 a second, hence tha NIC blinks once every second.
 */
static void s2io_phy_id(unsigned long data)
{
	nic_t *sp = (nic_t *) data;
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) sp->bar0;
	u64 val64 = 0;
	u16 subid;

	subid = sp->pdev->subsystem_device;
	if ((subid & 0xFF) >= 0x07) {
		val64 = readq(&bar0->gpio_control);
		val64 ^= GPIO_CTRL_GPIO_0;
		writeq(val64, &bar0->gpio_control);
	} else {
		val64 = readq(&bar0->adapter_control);
		val64 ^= ADAPTER_LED_ON;
		writeq(val64, &bar0->adapter_control);
	}

	mod_timer(&sp->id_timer, jiffies + HZ / 2);
}

/*
 * Input Argument/s: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  id - pointer to the structure with identification parameters given by 
 *  ethtool.
 * Return value:
 *  int , returns '0' on success
 * Description:
 *  Used to physically identify the NIC on the system. The Link LED will blink
 *  for a time specified by the user for identification.
 *  NOTE: The Link has to be Up to be able to blink the LED. Hence 
 *  identification is possible only if it's link is up.
 */
static int s2io_ethtool_idnic(struct net_device *dev, u32 data)
{
	u64 val64 = 0;
	nic_t *sp = dev->priv;
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) sp->bar0;
	u16 subid;

	subid = sp->pdev->subsystem_device;
	if ((subid & 0xFF) < 0x07) {
		val64 = readq(&bar0->adapter_control);
		if (!(val64 & ADAPTER_CNTL_EN)) {
			printk(KERN_ERR
			       "Adapter Link down, cannot blink LED\n");
			return -EFAULT;
		}
	}
	if (sp->id_timer.function == NULL) {
		init_timer(&sp->id_timer);
		sp->id_timer.function = s2io_phy_id;
		sp->id_timer.data = (unsigned long) sp;
	}
	mod_timer(&sp->id_timer, jiffies);
	set_current_state(TASK_INTERRUPTIBLE);
	if (data)
		schedule_timeout(data * HZ);
	else
		schedule_timeout(MAX_SCHEDULE_TIMEOUT);
	del_timer_sync(&sp->id_timer);

	return 0;
}

/*
 * Input Argument/s: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  ep - pointer to the structure with pause parameters given by ethtool.
 * Return value:
 *  void
 * Description:
 *  Returns the Pause frame generation and reception capability of the NIC.
 */
static void s2io_ethtool_getpause_data(struct net_device *dev,
				       struct ethtool_pauseparam *ep)
{
	u64 val64;
	nic_t *sp = dev->priv;
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) sp->bar0;

	val64 = readq(&bar0->rmac_pause_cfg);
	if (val64 & RMAC_PAUSE_GEN_ENABLE)
		ep->tx_pause = TRUE;
	if (val64 & RMAC_PAUSE_RX_ENABLE)
		ep->rx_pause = TRUE;
	ep->autoneg = FALSE;
}

/*
 * Input Argument/s: 
 * sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 * ep - pointer to the structure with pause parameters given by ethtool.
 * Return value:
 * int, returns '0' on Success
 * Description:
 * It can be used to set or reset Pause frame generation or reception support 
 * of the NIC.
 */
int s2io_ethtool_setpause_data(struct net_device *dev,
			       struct ethtool_pauseparam *ep)
{
	u64 val64;
	nic_t *sp = dev->priv;
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) sp->bar0;

	val64 = readq(&bar0->rmac_pause_cfg);
	if (ep->tx_pause)
		val64 |= RMAC_PAUSE_GEN_ENABLE;
	else
		val64 &= ~RMAC_PAUSE_GEN_ENABLE;
	if (ep->rx_pause)
		val64 |= RMAC_PAUSE_RX_ENABLE;
	else
		val64 &= ~RMAC_PAUSE_RX_ENABLE;
	writeq(val64, &bar0->rmac_pause_cfg);
	return 0;
}

/*
 * Input Argument/s: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  off - offset at which the data must be written
 * Return value:
 *  -1 on failure and the value read from the Eeprom if successful.
 * Description:
 *  Will read 4 bytes of data from the user given offset and return the 
 *  read data.
 * NOTE: Will allow to read only part of the EEPROM visible through the
 * 	 I2C bus.
 */
#define S2IO_DEV_ID		5
static u32 readEeprom(nic_t * sp, int off)
{
	u32 data = -1, exit_cnt = 0;
	u64 val64;
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) sp->bar0;

	val64 = I2C_CONTROL_DEV_ID(S2IO_DEV_ID) | I2C_CONTROL_ADDR(off) |
	    I2C_CONTROL_BYTE_CNT(0x3) | I2C_CONTROL_READ |
	    I2C_CONTROL_CNTL_START;
	writeq(val64, &bar0->i2c_control);

	while (exit_cnt < 5) {
		val64 = readq(&bar0->i2c_control);
		if (I2C_CONTROL_CNTL_END(val64)) {
			data = I2C_CONTROL_GET_DATA(val64);
			break;
		}
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ / 20);
		exit_cnt++;
	}

	return data;
}

/*
 * Input Argument/s: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  off - offset at which the data must be written
 *  data - The data that is to be written
 *  cnt - Number of bytes of the data that are actually to be written into 
 *  the Eeprom. (max of 3)
 * Return value:
 *  '0' on success, -1 on failure.
 * Description:
 *  Actually writes the relevant part of the data value into the Eeprom
 *  through the I2C bus.
 */
static int writeEeprom(nic_t * sp, int off, u32 data, int cnt)
{
	int exit_cnt = 0, ret = -1;
	u64 val64;
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) sp->bar0;

	val64 = I2C_CONTROL_DEV_ID(S2IO_DEV_ID) | I2C_CONTROL_ADDR(off) |
	    I2C_CONTROL_BYTE_CNT(cnt) | I2C_CONTROL_SET_DATA(data) |
	    I2C_CONTROL_CNTL_START;
	writeq(val64, &bar0->i2c_control);

	while (exit_cnt < 5) {
		val64 = readq(&bar0->i2c_control);
		if (I2C_CONTROL_CNTL_END(val64)) {
			if (!(val64 & I2C_CONTROL_NACK))
				ret = 0;
			break;
		}
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ / 20);
		exit_cnt++;
	}

	return ret;
}

/* 
 * A helper function used to invert the 4 byte u32 data field
 * byte by byte. This will be used by the Read Eeprom function
 * for display purposes.
 */
u32 inv(u32 data)
{
	static u32 ret = 0;

	if (data) {
		u8 c = data;
		ret = ((ret << 8) + c);
		data >>= 8;
		inv(data);
	}

	return ret;
}

/*
 * Input Argument/s: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  eeprom - pointer to the user level structure provided by ethtool, 
 *   containing all relevant information.
 *  data_buf - user defined value to be written into Eeprom.
 * Return value:
 *  int  '0' on success
 * Description:
 *  Reads the values stored in the Eeprom at given offset for a given length.
 *  Stores these values int the input argument data buffer 'data_buf' and
 *  returns these to the caller (ethtool.)
 */
int s2io_ethtool_geeprom(struct net_device *dev,
			 struct ethtool_eeprom *eeprom, u8 * data_buf)
{
	u32 data, i, valid;
	nic_t *sp = dev->priv;

	eeprom->magic = sp->pdev->vendor | (sp->pdev->device << 16);

	if ((eeprom->offset + eeprom->len) > (XENA_EEPROM_SPACE))
		eeprom->len = XENA_EEPROM_SPACE - eeprom->offset;

	for (i = 0; i < eeprom->len; i += 4) {
		data = readEeprom(sp, eeprom->offset + i);
		if (data < 0) {
			DBG_PRINT(ERR_DBG, "Read of EEPROM failed\n");
			return -EFAULT;
		}
		valid = inv(data);
		memcpy((data_buf + i), &valid, 4);
	}
	return 0;
}

/*
 * Input Argument/s: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  eeprom - pointer to the user level structure provided by ethtool, 
 *   containing all relevant information.
 *  data_buf - user defined value to be written into Eeprom.
 * Return value:
 *  '0' on success, -EFAULT on failure.
 * Description:
 *  Tries to write the user provided value in the Eeprom, at the offset
 *  given by the user.
 */
static int s2io_ethtool_seeprom(struct net_device *dev,
				struct ethtool_eeprom *eeprom,
				u8 * data_buf)
{
	int len = eeprom->len, cnt = 0;
	u32 valid = 0, data;
	nic_t *sp = dev->priv;

	if (eeprom->magic != (sp->pdev->vendor | (sp->pdev->device << 16))) {
		DBG_PRINT(ERR_DBG,
			  "ETHTOOL_WRITE_EEPROM Err: Magic value ");
		DBG_PRINT(ERR_DBG, "is wrong, Its not 0x%x\n",
			  eeprom->magic);
		return -EFAULT;
	}

	while (len) {
		data = (u32) data_buf[cnt] & 0x000000FF;
		if (data) {
			valid = (u32) (data << 24);
		} else
			valid = data;

		if (writeEeprom(sp, (eeprom->offset + cnt), valid, 0)) {
			DBG_PRINT(ERR_DBG,
				  "ETHTOOL_WRITE_EEPROM Err: Cannot ");
			DBG_PRINT(ERR_DBG,
				  "write into the specified offset\n");
			return -EFAULT;
		}
		cnt++;
		len--;
	}

	return 0;
}

/*
 * Input Argument/s: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  data - variable that returns the result of each of the test conducted by 
 *  	the driver.
 * Return value:
 *  '0' on success.
 * Description:
 *  Read and write into all clock domains. The NIC has 3 clock domains,
 *  see that registers in all the three regions are accessible.
 */
static int s2io_registerTest(nic_t * sp, uint64_t * data)
{
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) sp->bar0;
	u64 val64 = 0;
	int fail = 0;

	val64 = readq(&bar0->pcc_enable);
	if (val64 != 0xff00000000000000ULL) {
		fail = 1;
		DBG_PRINT(INFO_DBG, "Read Test level 1 fails\n");
	}

	val64 = readq(&bar0->rmac_pause_cfg);
	if (val64 != 0xc000ffff00000000ULL) {
		fail = 1;
		DBG_PRINT(INFO_DBG, "Read Test level 2 fails\n");
	}

	val64 = readq(&bar0->rx_queue_cfg);
	if (val64 != 0x0808080808080808ULL) {
		fail = 1;
		DBG_PRINT(INFO_DBG, "Read Test level 3 fails\n");
	}

	val64 = readq(&bar0->xgxs_efifo_cfg);
	if (val64 != 0x000000001923141EULL) {
		fail = 1;
		DBG_PRINT(INFO_DBG, "Read Test level 4 fails\n");
	}

	val64 = 0x5A5A5A5A5A5A5A5AULL;
	writeq(val64, &bar0->xmsi_data);
	val64 = readq(&bar0->xmsi_data);
	if (val64 != 0x5A5A5A5A5A5A5A5AULL) {
		fail = 1;
		DBG_PRINT(ERR_DBG, "Write Test level 1 fails\n");
	}

	val64 = 0xA5A5A5A5A5A5A5A5ULL;
	writeq(val64, &bar0->xmsi_data);
	val64 = readq(&bar0->xmsi_data);
	if (val64 != 0xA5A5A5A5A5A5A5A5ULL) {
		fail = 1;
		DBG_PRINT(ERR_DBG, "Write Test level 2 fails\n");
	}

	*data = fail;
	return 0;
}

/*
 * Input Argument/s: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  data - variable that returns the result of each of the test conducted by 
 *  	the driver.
 * Return value:
 *  '0' on success.
 * Description:
 *  Verify that EEPROM in the xena can be programmed using I2C_CONTROL 
 *  register.
 */
static int s2io_eepromTest(nic_t * sp, uint64_t * data)
{
	int fail = 0, ret_data;

	/* Test Write Error at offset 0 */
	if (!writeEeprom(sp, 0, 0, 3))
		fail = 1;

	/* Test Write at offset 4f0 */
	if (writeEeprom(sp, 0x4F0, 0x01234567, 3))
		fail = 1;
	if ((ret_data = readEeprom(sp, 0x4f0)) < 0)
		fail = 1;

	if (ret_data != 0x01234567)
		fail = 1;

	/* Reset the EEPROM data go FFFF */
	writeEeprom(sp, 0x4F0, 0xFFFFFFFF, 3);

	/* Test Write Request Error at offset 0x7c */
	if (!writeEeprom(sp, 0x07C, 0, 3))
		fail = 1;

	/* Test Write Request at offset 0x7fc */
	if (writeEeprom(sp, 0x7FC, 0x01234567, 3))
		fail = 1;
	if ((ret_data = readEeprom(sp, 0x7FC)) < 0)
		fail = 1;

	if (ret_data != 0x01234567)
		fail = 1;

	/* Reset the EEPROM data go FFFF */
	writeEeprom(sp, 0x7FC, 0xFFFFFFFF, 3);

	/* Test Write Error at offset 0x80 */
	if (!writeEeprom(sp, 0x080, 0, 3))
		fail = 1;

	/* Test Write Error at offset 0xfc */
	if (!writeEeprom(sp, 0x0FC, 0, 3))
		fail = 1;

	/* Test Write Error at offset 0x100 */
	if (!writeEeprom(sp, 0x100, 0, 3))
		fail = 1;

	/* Test Write Error at offset 4ec */
	if (!writeEeprom(sp, 0x4EC, 0, 3))
		fail = 1;

	*data = fail;
	return 0;
}

/*
 * Input Argument/s: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  data - variable that returns the result of each of the test conducted by 
 *  	the driver.
 * Return value:
 *  '0' on success and -1 on failure.
 * Description:
 *  This invokes the MemBist test of the card. We give around
 *  2 secs time for the Test to complete. If it's still not complete
 *  within this peiod, we consider that the test failed. 
 */
static int s2io_bistTest(nic_t * sp, uint64_t * data)
{
	u8 bist = 0;
	int cnt = 0, ret = -1;

	pci_read_config_byte(sp->pdev, PCI_BIST, &bist);
	bist |= PCI_BIST_START;
	pci_write_config_word(sp->pdev, PCI_BIST, bist);

	while (cnt < 20) {
		pci_read_config_byte(sp->pdev, PCI_BIST, &bist);
		if (!(bist & PCI_BIST_START)) {
			*data = (bist & PCI_BIST_CODE_MASK);
			ret = 0;
			break;
		}
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ / 10);
		cnt++;
	}

	return ret;
}

/*
 * Input Argument/s: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  data - variable that returns the result of each of the test conducted by 
 *  	the driver.
 * Return value:
 *  '0' on success.
 * Description:
 *  The function verifies the link state of the NIC and updates the input 
 *  argument 'data' appropriately.
 */
static int s2io_linkTest(nic_t * sp, uint64_t * data)
{
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) sp->bar0;
	u64 val64;

	val64 = readq(&bar0->adapter_status);
	if (val64 & ADAPTER_STATUS_RMAC_LOCAL_FAULT)
		*data = 1;

	return 0;
}

/*
 * Input Argument/s: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  data - variable that returns the result of each of the test conducted by 
 *  	the driver.
 * Return value:
 *  '0' on success.
 * Description:
 *  This is one of the offline test that tests the read and write 
 *  access to the RldRam chip on the NIC.
 */
static int s2io_rldramTest(nic_t * sp, uint64_t * data)
{
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) sp->bar0;
	u64 val64;
	int cnt, iteration = 0, test_pass = 0;

	val64 = readq(&bar0->adapter_control);
	val64 &= ~ADAPTER_ECC_EN;
	writeq(val64, &bar0->adapter_control);

	val64 = readq(&bar0->mc_rldram_test_ctrl);
	val64 |= MC_RLDRAM_TEST_MODE;
	writeq(val64, &bar0->mc_rldram_test_ctrl);

	val64 = readq(&bar0->mc_rldram_mrs);
	val64 |= MC_RLDRAM_QUEUE_SIZE_ENABLE;
	writeq(val64, &bar0->mc_rldram_mrs);

	val64 |= MC_RLDRAM_MRS_ENABLE;
	writeq(val64, &bar0->mc_rldram_mrs);

	while (iteration < 2) {
		val64 = 0x55555555aaaa0000ULL;
		if (iteration == 1) {
			val64 ^= 0xFFFFFFFFFFFF0000ULL;
		}
		writeq(val64, &bar0->mc_rldram_test_d0);

		val64 = 0xaaaa5a5555550000ULL;
		if (iteration == 1) {
			val64 ^= 0xFFFFFFFFFFFF0000ULL;
		}
		writeq(val64, &bar0->mc_rldram_test_d1);

		val64 = 0x55aaaaaaaa5a0000ULL;
		if (iteration == 1) {
			val64 ^= 0xFFFFFFFFFFFF0000ULL;
		}
		writeq(val64, &bar0->mc_rldram_test_d2);

		val64 = (u64) (0x0000003fffff0000ULL);
		writeq(val64, &bar0->mc_rldram_test_add);


		val64 = MC_RLDRAM_TEST_MODE;
		writeq(val64, &bar0->mc_rldram_test_ctrl);

		val64 |=
		    MC_RLDRAM_TEST_MODE | MC_RLDRAM_TEST_WRITE |
		    MC_RLDRAM_TEST_GO;
		writeq(val64, &bar0->mc_rldram_test_ctrl);

		for (cnt = 0; cnt < 5; cnt++) {
			val64 = readq(&bar0->mc_rldram_test_ctrl);
			if (val64 & MC_RLDRAM_TEST_DONE)
				break;
			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_timeout(HZ / 5);
		}

		if (cnt == 5)
			break;

		val64 = MC_RLDRAM_TEST_MODE;
		writeq(val64, &bar0->mc_rldram_test_ctrl);

		val64 |= MC_RLDRAM_TEST_MODE | MC_RLDRAM_TEST_GO;
		writeq(val64, &bar0->mc_rldram_test_ctrl);

		for (cnt = 0; cnt < 5; cnt++) {
			val64 = readq(&bar0->mc_rldram_test_ctrl);
			if (val64 & MC_RLDRAM_TEST_DONE)
				break;
			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_timeout(HZ / 2);
		}

		if (cnt == 5)
			break;

		val64 = readq(&bar0->mc_rldram_test_ctrl);
		if (val64 & MC_RLDRAM_TEST_PASS)
			test_pass = 1;

		iteration++;
	}

	if (!test_pass)
		*data = 1;
	else
		*data = 0;

	return 0;
}

/*
 * Input Argument/s: 
 *  sp - private member of the device structure, which is a pointer to the 
 *   	s2io_nic structure.
 *  ethtest - pointer to a ethtool command specific structure that will be
 *  	returned to the user.
 *  data - variable that returns the result of each of the test conducted by 
 *  	the driver.
 * Return value:
 *  SUCCESS on success and an appropriate -1 on failure.
 * Description:
 *  This function conducts 6 tests ( 4 offline and 2 online) to determine
 *  	the health of the card.
 */
static void s2io_ethtool_test(struct net_device *dev,
			      struct ethtool_test *ethtest,
			      uint64_t * data)
{
	nic_t *sp = dev->priv;
	int orig_state = netif_running(sp->dev);

	if (ethtest->flags == ETH_TEST_FL_OFFLINE) {
		/* Offline Tests. */
		if (orig_state) {
			s2io_close(sp->dev);
			s2io_set_swapper(sp);
		} else
			s2io_set_swapper(sp);

		if (s2io_registerTest(sp, &data[0]))
			ethtest->flags |= ETH_TEST_FL_FAILED;

		s2io_reset(sp);
		s2io_set_swapper(sp);

		if (s2io_rldramTest(sp, &data[3]))
			ethtest->flags |= ETH_TEST_FL_FAILED;

		s2io_reset(sp);
		s2io_set_swapper(sp);

		if (s2io_eepromTest(sp, &data[1]))
			ethtest->flags |= ETH_TEST_FL_FAILED;

		if (s2io_bistTest(sp, &data[4]))
			ethtest->flags |= ETH_TEST_FL_FAILED;

		if (orig_state)
			s2io_open(sp->dev);

		data[2] = 0;
	} else {
		/* Online Tests. */
		if (!orig_state) {
			DBG_PRINT(ERR_DBG,
				  "%s: is not up, cannot run test\n",
				  dev->name);
			data[0] = -1;
			data[1] = -1;
			data[2] = -1;
			data[3] = -1;
			data[4] = -1;
		}

		if (s2io_linkTest(sp, &data[2]))
			ethtest->flags |= ETH_TEST_FL_FAILED;

		data[0] = 0;
		data[1] = 0;
		data[3] = 0;
		data[4] = 0;
	}
}

static void s2io_get_ethtool_stats(struct net_device *dev,
				   struct ethtool_stats *estats,
				   u64 * tmp_stats)
{
	int i = 0;
	nic_t *sp = dev->priv;
	StatInfo_t *stat_info = sp->mac_control.StatsInfo;

	tmp_stats[i++] = stat_info->tmac_frms;
	tmp_stats[i++] = stat_info->tmac_data_octets;
	tmp_stats[i++] = stat_info->tmac_drop_frms;
	tmp_stats[i++] = stat_info->tmac_mcst_frms;
	tmp_stats[i++] = stat_info->tmac_bcst_frms;
	tmp_stats[i++] = stat_info->tmac_pause_ctrl_frms;
	tmp_stats[i++] = stat_info->tmac_any_err_frms;
	tmp_stats[i++] = stat_info->tmac_vld_ip_octets;
	tmp_stats[i++] = stat_info->tmac_vld_ip;
	tmp_stats[i++] = stat_info->tmac_drop_ip;
	tmp_stats[i++] = stat_info->tmac_icmp;
	tmp_stats[i++] = stat_info->tmac_rst_tcp;
	tmp_stats[i++] = stat_info->tmac_tcp;
	tmp_stats[i++] = stat_info->tmac_udp;
	tmp_stats[i++] = stat_info->rmac_vld_frms;
	tmp_stats[i++] = stat_info->rmac_data_octets;
	tmp_stats[i++] = stat_info->rmac_fcs_err_frms;
	tmp_stats[i++] = stat_info->rmac_drop_frms;
	tmp_stats[i++] = stat_info->rmac_vld_mcst_frms;
	tmp_stats[i++] = stat_info->rmac_vld_bcst_frms;
	tmp_stats[i++] = stat_info->rmac_in_rng_len_err_frms;
	tmp_stats[i++] = stat_info->rmac_long_frms;
	tmp_stats[i++] = stat_info->rmac_pause_ctrl_frms;
	tmp_stats[i++] = stat_info->rmac_discarded_frms;
	tmp_stats[i++] = stat_info->rmac_usized_frms;
	tmp_stats[i++] = stat_info->rmac_osized_frms;
	tmp_stats[i++] = stat_info->rmac_frag_frms;
	tmp_stats[i++] = stat_info->rmac_jabber_frms;
	tmp_stats[i++] = stat_info->rmac_ip;
	tmp_stats[i++] = stat_info->rmac_ip_octets;
	tmp_stats[i++] = stat_info->rmac_hdr_err_ip;
	tmp_stats[i++] = stat_info->rmac_drop_ip;
	tmp_stats[i++] = stat_info->rmac_icmp;
	tmp_stats[i++] = stat_info->rmac_tcp;
	tmp_stats[i++] = stat_info->rmac_udp;
	tmp_stats[i++] = stat_info->rmac_err_drp_udp;
	tmp_stats[i++] = stat_info->rmac_pause_cnt;
	tmp_stats[i++] = stat_info->rmac_accepted_ip;
	tmp_stats[i++] = stat_info->rmac_err_tcp;
}

int s2io_ethtool_get_regs_len(struct net_device *dev)
{
	return (XENA_REG_SPACE);
}


u32 s2io_ethtool_get_rx_csum(struct net_device * dev)
{
	nic_t *sp = dev->priv;

	return (sp->rx_csum);
}
int s2io_ethtool_set_rx_csum(struct net_device *dev, u32 data)
{
	nic_t *sp = dev->priv;

	if (data)
		sp->rx_csum = 1;
	else
		sp->rx_csum = 0;

	return 0;
}
int s2io_get_eeprom_len(struct net_device *dev)
{
	return (XENA_EEPROM_SPACE);
}

int s2io_ethtool_self_test_count(struct net_device *dev)
{
	return (S2IO_TEST_LEN);
}
void s2io_ethtool_get_strings(struct net_device *dev,
			      u32 stringset, u8 * data)
{
	switch (stringset) {
	case ETH_SS_TEST:
		memcpy(data, s2io_gstrings, S2IO_STRINGS_LEN);
		break;
	case ETH_SS_STATS:
		memcpy(data, &ethtool_stats_keys,
		       sizeof(ethtool_stats_keys));
	}
}
static int s2io_ethtool_get_stats_count(struct net_device *dev)
{
	return (S2IO_STAT_LEN);
}

static struct ethtool_ops netdev_ethtool_ops = {
	.get_settings = s2io_ethtool_gset,
	.set_settings = s2io_ethtool_sset,
	.get_drvinfo = s2io_ethtool_gdrvinfo,
	.get_regs_len = s2io_ethtool_get_regs_len,
	.get_regs = s2io_ethtool_gregs,
	.get_link = ethtool_op_get_link,
	.get_eeprom_len = s2io_get_eeprom_len,
	.get_eeprom = s2io_ethtool_geeprom,
	.set_eeprom = s2io_ethtool_seeprom,
	.get_pauseparam = s2io_ethtool_getpause_data,
	.set_pauseparam = s2io_ethtool_setpause_data,
	.get_rx_csum = s2io_ethtool_get_rx_csum,
	.set_rx_csum = s2io_ethtool_set_rx_csum,
	.get_tx_csum = ethtool_op_get_tx_csum,
	.set_tx_csum = ethtool_op_set_tx_csum,
	.get_sg = ethtool_op_get_sg,
	.set_sg = ethtool_op_set_sg,
#ifdef NETIF_F_TSO
	.get_tso = ethtool_op_get_tso,
	.set_tso = ethtool_op_set_tso,
#endif
	.self_test_count = s2io_ethtool_self_test_count,
	.self_test = s2io_ethtool_test,
	.get_strings = s2io_ethtool_get_strings,
	.phys_id = s2io_ethtool_idnic,
	.get_stats_count = s2io_ethtool_get_stats_count,
	.get_ethtool_stats = s2io_get_ethtool_stats
};

/*
 *  Input Argument/s: 
 *  dev -   Device pointer.
 *  ifr -   An IOCTL specefic structure, that can contain a pointer to
 *      a proprietary structure used to pass information to the driver.
 *  cmd -   This is used to distinguish between the different commands that
 *      can be passed to the IOCTL functions.
 *  Return value:
 *  '0' on success and an appropriate (-)ve integer as defined in errno.h
 *  file on failure.
 *  Description:
 *  This function has support for ethtool, adding multiple MAC addresses on 
 *  the NIC and some DBG commands for the util tool.
 */
int s2io_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	return -EOPNOTSUPP;
}

/*
 *  Input Argument/s: 
 *   dev - device pointer.
 *   new_mtu - the new MTU size for the device.
 *  Return value:
 *   '0' on success and an appropriate (-)ve integer as defined in errno.h
 *   file on failure.
 *  Description:
 *   A driver entry point to change MTU size for the device. Before changing
 *   the MTU the device must be stopped.
 */
int s2io_change_mtu(struct net_device *dev, int new_mtu)
{
	nic_t *sp = dev->priv;
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) sp->bar0;
	register u64 val64;

	if (netif_running(dev)) {
		DBG_PRINT(ERR_DBG, "%s: Must be stopped to ", dev->name);
		DBG_PRINT(ERR_DBG, "change its MTU \n");
		return -EBUSY;
	}

	if ((new_mtu < MIN_MTU) || (new_mtu > S2IO_JUMBO_SIZE)) {
		DBG_PRINT(ERR_DBG, "%s: MTU size is invalid.\n",
			  dev->name);
		return -EPERM;
	}

/* Set the new MTU into the PYLD register of the NIC */
	val64 = new_mtu;
	writeq(vBIT(val64, 2, 14), &bar0->rmac_max_pyld_len);

	dev->mtu = new_mtu;

	return 0;
}

/*
 *  Input Argument/s: 
 *  dev_adr - address of the device structure in dma_addr_t format.
 *  Return value:
 *  void.
 *  Description:
 *  This is the tasklet or the bottom half of the ISR. This is
 *  an extension of the ISR which is scheduled by the scheduler to be run 
 *  when the load on the CPU is low. All low priority tasks of the ISR can
 *  be pushed into the tasklet. For now the tasklet is used only to 
 *  replenish the Rx buffers in the Rx buffer descriptors.
 */
static void s2io_tasklet(unsigned long dev_addr)
{
	struct net_device *dev = (struct net_device *) dev_addr;
	nic_t *sp = dev->priv;
	int i, ret;
	mac_info_t *mac_control;
	struct config_param *config;

	mac_control = &sp->mac_control;
	config = &sp->config;

	if (!TASKLET_IN_USE) {
		for (i = 0; i < config->RxRingNum; i++) {
			ret = fill_rx_buffers(sp, i);
			if (ret == -ENOMEM) {
				DBG_PRINT(ERR_DBG, "%s: Out of ",
					  dev->name);
				DBG_PRINT(ERR_DBG, "memory in tasklet\n");
				return;
			} else if (ret == -EFILL) {
				DBG_PRINT(ERR_DBG,
					  "%s: Rx Ring %d is full\n",
					  dev->name, i);
				return;
			}
		}
		clear_bit(0, (unsigned long *) (&sp->tasklet_status));
	}
}


/*
 * Description:
 * 
 */
static void s2io_set_link(unsigned long data)
{
	nic_t *nic = (nic_t *) data;
	struct net_device *dev = nic->dev;
	XENA_dev_config_t *bar0 = (XENA_dev_config_t *) nic->bar0;
	register u64 val64, err_reg;

	/* Allow a small delay for the NICs self initiated 
	 * cleanup to complete.
	 */
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(HZ / 10);

	val64 = readq(&bar0->adapter_status);
	if (verify_xena_quiescence(val64, nic->device_enabled_once)) {
		/* Acknowledge interrupt and clear the R1 register */
		err_reg = readq(&bar0->mac_rmac_err_reg);
		writeq(err_reg, &bar0->mac_rmac_err_reg);

		if (LINK_IS_UP(val64)) {
			val64 = readq(&bar0->adapter_control);
			val64 |= ADAPTER_CNTL_EN;
			writeq(val64, &bar0->adapter_control);
			val64 |= ADAPTER_LED_ON;
			writeq(val64, &bar0->adapter_control);
			val64 = readq(&bar0->adapter_status);
			if (!LINK_IS_UP(val64)) {
				DBG_PRINT(ERR_DBG, "%s:", dev->name);
				DBG_PRINT(ERR_DBG, " Link down");
				DBG_PRINT(ERR_DBG, "after ");
				DBG_PRINT(ERR_DBG, "enabling ");
				DBG_PRINT(ERR_DBG, "device \n");
			}
			if (nic->device_enabled_once == FALSE) {
				nic->device_enabled_once = TRUE;
			}
			s2io_link(nic, LINK_UP);
		} else {
			s2io_link(nic, LINK_DOWN);
		}
	} else {		/* NIC is not Quiescent. */
		DBG_PRINT(ERR_DBG, "%s: Error: ", dev->name);
		DBG_PRINT(ERR_DBG, "device is not Quiescent\n");
		netif_stop_queue(dev);
	}
}

/*
 * Description:
 * This function is scheduled to be run by the s2io_tx_watchdog
 * function after 0.5 secs to reset the NIC. The idea is to reduce 
 * the run time of the watch dog routine which is run holding a
 * spin lock.
 */
static void s2io_restart_nic(unsigned long data)
{
	struct net_device *dev = (struct net_device *) data;
	nic_t *sp = dev->priv;

	s2io_close(dev);
	sp->device_close_flag = TRUE;
	s2io_open(dev);
	DBG_PRINT(ERR_DBG,
		  "%s: was reset by Tx watchdog timer.\n", dev->name);
}

/*
 *  Input Argument/s: 
 *  dev - device pointer.
 *  Return value:
 *  void
 *  Description:
 *  This function is triggered if the Tx Queue is stopped
 *  for a pre-defined amount of time when the Interface is still up.
 *  If the Interface is jammed in such a situation, the hardware is
 *  reset (by s2io_close) and restarted again (by s2io_open) to
 *  overcome any problem that might have been caused in the hardware.
 */
static void s2io_tx_watchdog(struct net_device *dev)
{
	nic_t *sp = dev->priv;

	if (netif_carrier_ok(dev)) {
		schedule_work(&sp->rst_timer_task);
	}
}

/*
 *  Input Argument/s: 
 *   sp - private member of the device structure, which is a pointer to the 
 *   s2io_nic structure.
 *   skb - the socket buffer pointer.
 *   len - length of the packet
 *   cksum - FCS checksum of the frame.
 *  ring_no - the ring from which this RxD was extracted.
 *  Return value:
 *   SUCCESS on success and -1 on failure.
 *  Description: 
 *   This function is called by the Tx interrupt serivce routine to perform 
 *   some OS related operations on the SKB before passing it to the upper
 *   layers. It mainly checks if the checksum is OK, if so adds it to the
 *   SKBs cksum variable, increments the Rx packet count and passes the SKB
 *   to the upper layer. If the checksum is wrong, it increments the Rx
 *   packet error count, frees the SKB and returns error.
 */
static int rxOsmHandler(nic_t * sp, u16 len, RxD_t * rxdp, int ring_no)
{
	struct net_device *dev = (struct net_device *) sp->dev;
	struct sk_buff *skb =
	    (struct sk_buff *) ((unsigned long) rxdp->Host_Control);
	u16 l3_csum, l4_csum;

	l3_csum = RXD_GET_L3_CKSUM(rxdp->Control_1);
	if ((rxdp->Control_1 & TCP_OR_UDP_FRAME) && (sp->rx_csum)) {
		l4_csum = RXD_GET_L4_CKSUM(rxdp->Control_1);
		if ((l3_csum == L3_CKSUM_OK) && (l4_csum == L4_CKSUM_OK)) {
			/* NIC verifies if the Checksum of the received
			 * frame is Ok or not and accordingly returns
			 * a flag in the RxD.
			 */
			skb->ip_summed = CHECKSUM_UNNECESSARY;
		} else {
			/* 
			 * Packet with erroneous checksum, let the 
			 * upper layers deal with it.
			 */
			skb->ip_summed = CHECKSUM_NONE;
		}
	} else {
		skb->ip_summed = CHECKSUM_NONE;
	}

	skb->dev = dev;
	skb_put(skb, len);
	skb->protocol = eth_type_trans(skb, dev);

#ifdef CONFIG_S2IO_NAPI
	netif_receive_skb(skb);
#else
	netif_rx(skb);
#endif

	dev->last_rx = jiffies;
#if DEBUG_ON
	sp->rxpkt_cnt++;
#endif
	sp->rx_pkt_count++;
	sp->stats.rx_packets++;
	sp->stats.rx_bytes += len;
	sp->rxpkt_bytes += len;

	atomic_dec(&sp->rx_bufs_left[ring_no]);
	rxdp->Host_Control = 0;
	return SUCCESS;
}

int check_for_txSpace(nic_t * sp)
{
	u32 put_off, get_off, queue_len;
	int ret = TRUE, i;

	for (i = 0; i < sp->config.TxFIFONum; i++) {
		queue_len = sp->mac_control.tx_curr_put_info[i].fifo_len
		    + 1;
		put_off = sp->mac_control.tx_curr_put_info[i].offset;
		get_off = sp->mac_control.tx_curr_get_info[i].offset;
		if (((put_off + 1) % queue_len) == get_off) {
			ret = FALSE;
			break;
		}
	}

	return ret;
}

/*
*  Input Argument/s: 
*   sp - private member of the device structure, which is a pointer to the 
*   s2io_nic structure.
*   link - inidicates whether link is UP/DOWN.
*  Return value:
*   void.
*  Description:
*   This function stops/starts the Tx queue depending on whether the link
*   status of the NIC is is down or up. This is called by the Alarm interrupt 
*  handler whenever a link change interrupt comes up. 
*/
void s2io_link(nic_t * sp, int link)
{
	struct net_device *dev = (struct net_device *) sp->dev;

	if (link != sp->last_link_state) {
		if (link == LINK_DOWN) {
			DBG_PRINT(ERR_DBG, "%s: Link down\n", dev->name);
			netif_carrier_off(dev);
			netif_stop_queue(dev);
		} else {
			DBG_PRINT(ERR_DBG, "%s: Link Up\n", dev->name);
			netif_carrier_on(dev);
			if (check_for_txSpace(sp) == TRUE) {
				/* Don't wake the queue, if we know there
				 * are no free TxDs available.
				 */
				netif_wake_queue(dev);
			}
		}
	}
	sp->last_link_state = link;
}

/*
*  Input Argument/s: 
*   pdev - structure containing the PCI related information of the device.
*  Return value:
*   returns the revision ID of the device.
*  Description:
*   Function to identify the Revision ID of xena.
*/
int get_xena_rev_id(struct pci_dev *pdev)
{
	u8 id = 0;
	int ret;
	ret = pci_read_config_byte(pdev, PCI_REVISION_ID, (u8 *) & id);
	return id;
}

/*
*  Input Argument/s: 
*   sp - private member of the device structure, which is a pointer to the 
*   s2io_nic structure.
*  Return value:
*   void
*  Description:
*   This function initializes a few of the PCI and PCI-X configuration registers
*   with recommended values.
*/
static void s2io_init_pci(nic_t * sp)
{
	u16 pci_cmd = 0;

/* Enable Data Parity Error Recovery in PCI-X command register. */
	pci_read_config_word(sp->pdev, PCIX_COMMAND_REGISTER,
			     &(sp->pcix_cmd));
	pci_write_config_word(sp->pdev, PCIX_COMMAND_REGISTER,
			      (sp->pcix_cmd | 1));
	pci_read_config_word(sp->pdev, PCIX_COMMAND_REGISTER,
			     &(sp->pcix_cmd));

/* Set the PErr Response bit in PCI command register. */
	pci_read_config_word(sp->pdev, PCI_COMMAND, &pci_cmd);
	pci_write_config_word(sp->pdev, PCI_COMMAND,
			      (pci_cmd | PCI_COMMAND_PARITY));
	pci_read_config_word(sp->pdev, PCI_COMMAND, &pci_cmd);

/* Set user specified value in Latency Timer */
	if (latency_timer) {
		pci_write_config_byte(sp->pdev, PCI_LATENCY_TIMER,
				      latency_timer);
		pci_read_config_byte(sp->pdev, PCI_LATENCY_TIMER,
				     &latency_timer);
	}

/* Set MMRB count to 4096 in PCI-X Command register. */
	pci_write_config_word(sp->pdev, PCIX_COMMAND_REGISTER,
			      (sp->pcix_cmd | 0x0C));
	pci_read_config_word(sp->pdev, PCIX_COMMAND_REGISTER,
			     &(sp->pcix_cmd));

/* Setting Maximum outstanding splits to two for now. */
	sp->pcix_cmd &= 0xFF1F;

	sp->pcix_cmd |=
	    XENA_MAX_OUTSTANDING_SPLITS(XENA_TWO_SPLIT_TRANSACTION);
	pci_write_config_word(sp->pdev, PCIX_COMMAND_REGISTER,
			      sp->pcix_cmd);
	pci_read_config_word(sp->pdev, PCIX_COMMAND_REGISTER,
			     &(sp->pcix_cmd));

}

MODULE_AUTHOR("Raghavendra Koushik <raghavendra.koushik@s2io.com>");
MODULE_LICENSE("GPL");
MODULE_PARM(ring_num, "1-" __MODULE_STRING(1) "i");
MODULE_PARM(frame_len, "1-" __MODULE_STRING(8) "i");
MODULE_PARM(ring_len, "1-" __MODULE_STRING(8) "i");
MODULE_PARM(fifo_num, "1-" __MODULE_STRING(1) "i");
MODULE_PARM(fifo_len, "1-" __MODULE_STRING(8) "i");
MODULE_PARM(rx_prio, "1-" __MODULE_STRING(1) "i");
MODULE_PARM(tx_prio, "1-" __MODULE_STRING(1) "i");
MODULE_PARM(latency_timer, "1-" __MODULE_STRING(1) "i");

/*
*  Input Argument/s: 
*   pdev - structure containing the PCI related information of the device.
*   pre -  the List of PCI devices supported by the driver listed in s2io_tbl.
*  Return value:
*   returns '0' on success and negative on failure.
*  Description:
*  The function initializes an adapter identified by the pci_dec structure.
*  All OS related initialization including memory and device structure and 
*  initlaization of the device private variable is done. Also the swapper 
*  control register is initialized to enable read and write into the I/O 
*  registers of the device.
*  
*/
static int __devinit
s2io_init_nic(struct pci_dev *pdev, const struct pci_device_id *pre)
{
	nic_t *sp;
	struct net_device *dev;
	char *dev_name = "S2IO 10GE NIC";
	int i, j, ret;
	int dma_flag = FALSE;
	u32 mac_up, mac_down;
	u64 val64 = 0, tmp64 = 0;
	XENA_dev_config_t *bar0 = NULL;
	u16 subid;
	mac_info_t *mac_control;
	struct config_param *config;


	if ((ret = pci_enable_device(pdev))) {
		DBG_PRINT(ERR_DBG,
			  "s2io_init_nic: pci_enable_device failed\n");
		return ret;
	}

	if (!pci_set_dma_mask(pdev, 0xffffffffffffffffULL)) {
		DBG_PRINT(INIT_DBG, "s2io_init_nic: Using 64bit DMA\n");
		dma_flag = TRUE;
		if (pci_set_consistent_dma_mask
		    (pdev, 0xffffffffffffffffULL)) {
			DBG_PRINT(ERR_DBG,
				  "Unable to obtain 64bit DMA for \
					consistent allocations\n");
			pci_disable_device(pdev);
			return -ENOMEM;
		}
	} else if (!pci_set_dma_mask(pdev, 0xffffffffUL)) {
		DBG_PRINT(INIT_DBG, "s2io_init_nic: Using 32bit DMA\n");
	} else {
		pci_disable_device(pdev);
		return -ENOMEM;
	}

	if (pci_request_regions(pdev, s2io_driver_name)) {
		DBG_PRINT(ERR_DBG, "Request Regions failed\n"),
		    pci_disable_device(pdev);
		return -ENODEV;
	}

	dev = alloc_etherdev(sizeof(nic_t));
	if (dev == NULL) {
		DBG_PRINT(ERR_DBG, "Device allocation failed\n");
		pci_disable_device(pdev);
		pci_release_regions(pdev);
		return -ENODEV;
	}

	pci_set_master(pdev);
	pci_set_drvdata(pdev, dev);
	SET_MODULE_OWNER(dev);
	SET_NETDEV_DEV(dev, &pdev->dev);

	/*  Private member variable initialized to s2io NIC structure */
	sp = dev->priv;
	memset(sp, 0, sizeof(nic_t));
	sp->dev = dev;
	sp->pdev = pdev;
	sp->vendor_id = pdev->vendor;
	sp->device_id = pdev->device;
	sp->high_dma_flag = dma_flag;
	sp->irq = pdev->irq;
	sp->device_enabled_once = FALSE;
	strcpy(sp->name, dev_name);

	/* Initialize some PCI/PCI-X fields of the NIC. */
	s2io_init_pci(sp);

	/* Setting the device configuration parameters.
	 * Most of these parameters can be specified by the user during 
	 * module insertion as they are module loadable parameters. If 
	 * these parameters are not not specified during load time, they 
	 * are initialized with default values.
	 */
	mac_control = &sp->mac_control;
	config = &sp->config;

	/* Tx side parameters. */
	config->TxFIFONum = fifo_num ? fifo_num : 1;

	if (!fifo_len[0] && (fifo_num > 1)) {
		printk(KERN_ERR "Fifo Lens not specified for all FIFOs\n");
		goto init_failed;
	}

	if (fifo_len[0]) {
		int cnt;

		for (cnt = 0; fifo_len[cnt]; cnt++);
		if (fifo_num) {
			if (cnt < fifo_num) {
				printk(KERN_ERR
				       "Fifo Lens not specified for ");
				printk(KERN_ERR "all FIFOs\n");
				goto init_failed;
			}
		}
		for (cnt = 0; cnt < config->TxFIFONum; cnt++) {
			config->TxCfg[cnt].FifoLen = fifo_len[cnt];
			config->TxCfg[cnt].FifoPriority = cnt;
		}
	} else {
		config->TxCfg[0].FifoLen = DEFAULT_FIFO_LEN;
		config->TxCfg[0].FifoPriority = 0;
	}

	config->TxIntrType = TXD_INT_TYPE_UTILZ;
	for (i = 0; i < config->TxFIFONum; i++) {
		if (config->TxCfg[i].FifoLen < 65) {
			config->TxIntrType = TXD_INT_TYPE_PER_LIST;
			break;
		}
	}

	config->TxCfg[0].fNoSnoop = (NO_SNOOP_TXD | NO_SNOOP_TXD_BUFFER);
	config->MaxTxDs = MAX_SKB_FRAGS;
	config->TxFlow = TRUE;

	/* Rx side parameters. */
	config->RxRingNum = ring_num ? ring_num : 1;

	if (ring_len[0]) {
		int cnt;
		for (cnt = 0; cnt < config->RxRingNum; cnt++) {
			config->RxCfg[cnt].NumRxd = ring_len[cnt];
			config->RxCfg[cnt].RingPriority = cnt;
		}
	} else {
		int id;
		if ((id = get_xena_rev_id(pdev)) == 1) {
			config->RxCfg[0].NumRxd = LARGE_RXD_CNT;

		} else {
			config->RxCfg[0].NumRxd = SMALL_RXD_CNT;
		}
		config->RxCfg[0].RingPriority = 0;
	}
	config->RxCfg[0].RingOrg = RING_ORG_BUFF1;
	config->RxCfg[0].RxdThresh = DEFAULT_RXD_THRESHOLD;
	config->RxCfg[0].fNoSnoop = (NO_SNOOP_RXD | NO_SNOOP_RXD_BUFFER);
	config->RxCfg[0].RxD_BackOff_Interval = TBD;
	config->RxFlow = TRUE;

	/* Miscellaneous parameters. */
	config->RxVLANEnable = TRUE;
	config->MTU = MAX_MTU_VLAN;
	config->JumboEnable = FALSE;

	/*  Setting Mac Control parameters */
	mac_control->txdl_len = MAX_SKB_FRAGS;
	mac_control->rmac_pause_time = 0;

	/* Initialize Ring buffer parameters. */
	for (i = 0; i < config->RxRingNum; i++)
		atomic_set(&sp->rx_bufs_left[i], 0);

	/*  initialize the shared memory used by the NIC and the host */
	if (initSharedMem(sp)) {
		DBG_PRINT(ERR_DBG, "%s: Memory allocation failed\n",
			  dev->name);
		goto mem_alloc_failed;
	}

	sp->bar0 = (caddr_t) ioremap(pci_resource_start(pdev, 0),
				     pci_resource_len(pdev, 0));
	if (!sp->bar0) {
		DBG_PRINT(ERR_DBG, "%s: S2IO: cannot remap io mem1\n",
			  dev->name);
		goto bar0_remap_failed;
	}

	sp->bar1 = (caddr_t) ioremap(pci_resource_start(pdev, 2),
				     pci_resource_len(pdev, 2));
	if (!sp->bar1) {
		DBG_PRINT(ERR_DBG, "%s: S2IO: cannot remap io mem2\n",
			  dev->name);
		goto bar1_remap_failed;
	}

	dev->irq = pdev->irq;
	dev->base_addr = (unsigned long) sp->bar0;

	/* Initializing the BAR1 address as the start of the FIFO pointer. */
	for (j = 0; j < MAX_TX_FIFOS; j++) {
		mac_control->tx_FIFO_start[j] = (TxFIFO_element_t *)
		    (sp->bar1 + (j * 0x00020000));
	}

	/*  Driver entry points */
	dev->open = &s2io_open;
	dev->stop = &s2io_close;
	dev->hard_start_xmit = &s2io_xmit;
	dev->get_stats = &s2io_get_stats;
	dev->set_multicast_list = &s2io_set_multicast;
	dev->do_ioctl = &s2io_ioctl;
	dev->change_mtu = &s2io_change_mtu;
	SET_ETHTOOL_OPS(dev, &netdev_ethtool_ops);

	/*
	 * will use eth_mac_addr() for  dev->set_mac_address
	 * mac address will be set every time dev->open() is called
	 */
#ifdef CONFIG_S2IO_NAPI
	dev->poll = s2io_poll;
	dev->weight = 128;	/* For now. */
#endif

	dev->features |= NETIF_F_SG | NETIF_F_IP_CSUM;
	if (sp->high_dma_flag == TRUE)
		dev->features |= NETIF_F_HIGHDMA;
#ifdef NETIF_F_TSO
	dev->features |= NETIF_F_TSO;
#endif

	dev->tx_timeout = &s2io_tx_watchdog;
	dev->watchdog_timeo = WATCH_DOG_TIMEOUT;
	INIT_WORK(&sp->rst_timer_task,
		  (void (*)(void *)) s2io_restart_nic, dev);
	INIT_WORK(&sp->set_link_task,
		  (void (*)(void *)) s2io_set_link, sp);

	if (register_netdev(dev)) {
		DBG_PRINT(ERR_DBG, "Device registration failed\n");
		goto register_failed;
	}

	pci_save_state(sp->pdev, sp->config_space);

	/* Setting swapper control on the NIC, for proper reset operation */
	if (s2io_set_swapper(sp)) {
		DBG_PRINT(ERR_DBG, "%s:swapper settings are wrong\n",
			  dev->name);
		goto set_swap_failed;
	}

	/* Fix for all "FFs" MAC address problems observed on Alpha platforms */
	FixMacAddress(sp);
	s2io_reset(sp);

	/* Setting swapper control on the NIC, so the MAC address can be read.
	 */
	if (s2io_set_swapper(sp)) {
		DBG_PRINT(ERR_DBG,
			  "%s: S2IO: swapper settings are wrong\n",
			  dev->name);
		goto set_swap_failed;
	}

	/*  MAC address initialization.
	 *  For now only one mac address will be read and used.
	 */
	bar0 = (XENA_dev_config_t *) sp->bar0;
	val64 = RMAC_ADDR_CMD_MEM_RD | RMAC_ADDR_CMD_MEM_STROBE_NEW_CMD |
	    RMAC_ADDR_CMD_MEM_OFFSET(0 + MAC_MAC_ADDR_START_OFFSET);
	writeq(val64, &bar0->rmac_addr_cmd_mem);
	waitForCmdComplete(sp);

	tmp64 = readq(&bar0->rmac_addr_data0_mem);
	mac_down = (u32) tmp64;
	mac_up = (u32) (tmp64 >> 32);

	memset(sp->defMacAddr[0].mac_addr, 0, sizeof(ETH_ALEN));

	sp->defMacAddr[0].mac_addr[3] = (u8) (mac_up);
	sp->defMacAddr[0].mac_addr[2] = (u8) (mac_up >> 8);
	sp->defMacAddr[0].mac_addr[1] = (u8) (mac_up >> 16);
	sp->defMacAddr[0].mac_addr[0] = (u8) (mac_up >> 24);
	sp->defMacAddr[0].mac_addr[5] = (u8) (mac_down >> 16);
	sp->defMacAddr[0].mac_addr[4] = (u8) (mac_down >> 24);

	DBG_PRINT(INIT_DBG,
		  "DEFAULT MAC ADDR:0x%02x-%02x-%02x-%02x-%02x-%02x\n",
		  sp->defMacAddr[0].mac_addr[0],
		  sp->defMacAddr[0].mac_addr[1],
		  sp->defMacAddr[0].mac_addr[2],
		  sp->defMacAddr[0].mac_addr[3],
		  sp->defMacAddr[0].mac_addr[4],
		  sp->defMacAddr[0].mac_addr[5]);

	/*  Set the factory defined MAC address initially   */
	dev->addr_len = ETH_ALEN;
	memcpy(dev->dev_addr, sp->defMacAddr, ETH_ALEN);

	/*  Initialize the tasklet status flag */
	atomic_set(&(sp->tasklet_status), 0);


	/* Initialize spinlocks */
	spin_lock_init(&sp->isr_lock);
	spin_lock_init(&sp->tx_lock);

	/* SXE-002: Configure link and activity LED to init state 
	 * on driver load. 
	 */
	subid = sp->pdev->subsystem_device;
	if ((subid & 0xFF) >= 0x07) {
		val64 = readq(&bar0->gpio_control);
		val64 |= 0x0000800000000000ULL;
		writeq(val64, &bar0->gpio_control);
		val64 = 0x0411040400000000ULL;
		writeq(val64, (u64 *) ((u8 *) bar0 + 0x2700));
		val64 = readq(&bar0->gpio_control);
	}

	/* Make Link state as off at this point, when the Link change 
	 * interrupt comes the state will be automatically changed to 
	 * the right state.
	 */
	netif_carrier_off(dev);
	sp->last_link_state = LINK_DOWN;

	sp->rx_csum = 1;	/* Rx chksum verify enabled by default */

	return 0;

      set_swap_failed:
	unregister_netdev(dev);
      register_failed:
	iounmap(sp->bar1);
      bar1_remap_failed:
	iounmap(sp->bar0);
      bar0_remap_failed:
      mem_alloc_failed:
	freeSharedMem(sp);
      init_failed:
	pci_disable_device(pdev);
	pci_release_regions(pdev);
	pci_set_drvdata(pdev, NULL);
	free_netdev(dev);

	return -ENODEV;
}

/*
*  Input Argument/s: 
*   pdev - structure containing the PCI related information of the device.
*  Return value:
*  void
*  Description:
*  This function is called by the Pci subsystem to release a PCI device 
*  and free up all resource held up by the device. This could be in response 
*  to a Hot plug event or when the driver is to be removed from memory.
*/
static void __devexit s2io_rem_nic(struct pci_dev *pdev)
{
	struct net_device *dev =
	    (struct net_device *) pci_get_drvdata(pdev);
	nic_t *sp;

	if (dev == NULL) {
		DBG_PRINT(ERR_DBG, "Driver Data is NULL!!\n");
		return;
	}
	sp = dev->priv;
	freeSharedMem(sp);
	iounmap(sp->bar0);
	iounmap(sp->bar1);
	pci_disable_device(pdev);
	pci_release_regions(pdev);
	pci_set_drvdata(pdev, NULL);

	unregister_netdev(dev);

	free_netdev(dev);
}

int __init s2io_starter(void)
{
	return pci_module_init(&s2io_driver);
}

void s2io_closer(void)
{
	pci_unregister_driver(&s2io_driver);
	DBG_PRINT(INIT_DBG, "cleanup done\n");
}

module_init(s2io_starter);
module_exit(s2io_closer);
