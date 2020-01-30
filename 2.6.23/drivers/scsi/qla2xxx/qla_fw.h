/*
 * QLogic Fibre Channel HBA Driver
 * Copyright (c)  2003-2005 QLogic Corporation
 *
 * See LICENSE.qla2xxx for copyright and licensing details.
 */
#ifndef __QLA_FW_H
#define __QLA_FW_H

#define MBS_CHECKSUM_ERROR	0x4010
#define MBS_INVALID_PRODUCT_KEY	0x4020

/*
 * Firmware Options.
 */
#define FO1_ENABLE_PUREX	BIT_10
#define FO1_DISABLE_LED_CTRL	BIT_6
#define FO1_ENABLE_8016		BIT_0
#define FO2_ENABLE_SEL_CLASS2	BIT_5
#define FO3_NO_ABTS_ON_LINKDOWN	BIT_14
#define FO3_HOLD_STS_IOCB	BIT_12

/*
 * Port Database structure definition for ISP 24xx.
 */
#define PDO_FORCE_ADISC		BIT_1
#define PDO_FORCE_PLOGI		BIT_0


#define	PORT_DATABASE_24XX_SIZE		64
struct port_database_24xx {
	uint16_t flags;
#define PDF_TASK_RETRY_ID	BIT_14
#define PDF_FC_TAPE		BIT_7
#define PDF_ACK0_CAPABLE	BIT_6
#define PDF_FCP2_CONF		BIT_5
#define PDF_CLASS_2		BIT_4
#define PDF_HARD_ADDR		BIT_1

	uint8_t current_login_state;
	uint8_t last_login_state;
#define PDS_PLOGI_PENDING	0x03
#define PDS_PLOGI_COMPLETE	0x04
#define PDS_PRLI_PENDING	0x05
#define PDS_PRLI_COMPLETE	0x06
#define PDS_PORT_UNAVAILABLE	0x07
#define PDS_PRLO_PENDING	0x09
#define PDS_LOGO_PENDING	0x11
#define PDS_PRLI2_PENDING	0x12

	uint8_t hard_address[3];
	uint8_t reserved_1;

	uint8_t port_id[3];
	uint8_t sequence_id;

	uint16_t port_timer;

	uint16_t nport_handle;			/* N_PORT handle. */

	uint16_t receive_data_size;
	uint16_t reserved_2;

	uint8_t prli_svc_param_word_0[2];	/* Big endian */
						/* Bits 15-0 of word 0 */
	uint8_t prli_svc_param_word_3[2];	/* Big endian */
						/* Bits 15-0 of word 3 */

	uint8_t port_name[WWN_SIZE];
	uint8_t node_name[WWN_SIZE];

	uint8_t reserved_3[24];
};

struct vp_database_24xx {
	uint16_t vp_status;
	uint8_t  options;
	uint8_t  id;
	uint8_t  port_name[WWN_SIZE];
	uint8_t  node_name[WWN_SIZE];
	uint16_t port_id_low;
	uint16_t port_id_high;
};

struct nvram_24xx {
	/* NVRAM header. */
	uint8_t id[4];
	uint16_t nvram_version;
	uint16_t reserved_0;

	/* Firmware Initialization Control Block. */
	uint16_t version;
	uint16_t reserved_1;
	uint16_t frame_payload_size;
	uint16_t execution_throttle;
	uint16_t exchange_count;
	uint16_t hard_address;

	uint8_t port_name[WWN_SIZE];
	uint8_t node_name[WWN_SIZE];

	uint16_t login_retry_count;
	uint16_t link_down_on_nos;
	uint16_t interrupt_delay_timer;
	uint16_t login_timeout;

	uint32_t firmware_options_1;
	uint32_t firmware_options_2;
	uint32_t firmware_options_3;

	/* Offset 56. */

	/*
	 * BIT 0     = Control Enable
	 * BIT 1-15  =
	 *
	 * BIT 0-7   = Reserved
	 * BIT 8-10  = Output Swing 1G
	 * BIT 11-13 = Output Emphasis 1G
	 * BIT 14-15 = Reserved
	 *
	 * BIT 0-7   = Reserved
	 * BIT 8-10  = Output Swing 2G
	 * BIT 11-13 = Output Emphasis 2G
	 * BIT 14-15 = Reserved
	 *
	 * BIT 0-7   = Reserved
	 * BIT 8-10  = Output Swing 4G
	 * BIT 11-13 = Output Emphasis 4G
	 * BIT 14-15 = Reserved
	 */
	uint16_t seriallink_options[4];

	uint16_t reserved_2[16];

	/* Offset 96. */
	uint16_t reserved_3[16];

	/* PCIe table entries. */
	uint16_t reserved_4[16];

	/* Offset 160. */
	uint16_t reserved_5[16];

	/* Offset 192. */
	uint16_t reserved_6[16];

	/* Offset 224. */
	uint16_t reserved_7[16];

	/*
	 * BIT 0  = Enable spinup delay
	 * BIT 1  = Disable BIOS
	 * BIT 2  = Enable Memory Map BIOS
	 * BIT 3  = Enable Selectable Boot
	 * BIT 4  = Disable RISC code load
	 * BIT 5  = Disable Serdes
	 * BIT 6  =
	 * BIT 7  =
	 *
	 * BIT 8  =
	 * BIT 9  =
	 * BIT 10 = Enable lip full login
	 * BIT 11 = Enable target reset
	 * BIT 12 =
	 * BIT 13 =
	 * BIT 14 =
	 * BIT 15 = Enable alternate WWN
	 *
	 * BIT 16-31 =
	 */
	uint32_t host_p;

	uint8_t alternate_port_name[WWN_SIZE];
	uint8_t alternate_node_name[WWN_SIZE];

	uint8_t boot_port_name[WWN_SIZE];
	uint16_t boot_lun_number;
	uint16_t reserved_8;

	uint8_t alt1_boot_port_name[WWN_SIZE];
	uint16_t alt1_boot_lun_number;
	uint16_t reserved_9;

	uint8_t alt2_boot_port_name[WWN_SIZE];
	uint16_t alt2_boot_lun_number;
	uint16_t reserved_10;

	uint8_t alt3_boot_port_name[WWN_SIZE];
	uint16_t alt3_boot_lun_number;
	uint16_t reserved_11;

	/*
	 * BIT 0 = Selective Login
	 * BIT 1 = Alt-Boot Enable
	 * BIT 2 = Reserved
	 * BIT 3 = Boot Order List
	 * BIT 4 = Reserved
	 * BIT 5 = Selective LUN
	 * BIT 6 = Reserved
	 * BIT 7-31 =
	 */
	uint32_t efi_parameters;

	uint8_t reset_delay;
	uint8_t reserved_12;
	uint16_t reserved_13;

	uint16_t boot_id_number;
	uint16_t reserved_14;

	uint16_t max_luns_per_target;
	uint16_t reserved_15;

	uint16_t port_down_retry_count;
	uint16_t link_down_timeout;

	/* FCode parameters. */
	uint16_t fcode_parameter;

	uint16_t reserved_16[3];

	/* Offset 352. */
	uint8_t prev_drv_ver_major;
	uint8_t prev_drv_ver_submajob;
	uint8_t prev_drv_ver_minor;
	uint8_t prev_drv_ver_subminor;

	uint16_t prev_bios_ver_major;
	uint16_t prev_bios_ver_minor;

	uint16_t prev_efi_ver_major;
	uint16_t prev_efi_ver_minor;

	uint16_t prev_fw_ver_major;
	uint8_t prev_fw_ver_minor;
	uint8_t prev_fw_ver_subminor;

	uint16_t reserved_17[8];

	/* Offset 384. */
	uint16_t reserved_18[16];

	/* Offset 416. */
	uint16_t reserved_19[16];

	/* Offset 448. */
	uint16_t reserved_20[16];

	/* Offset 480. */
	uint8_t model_name[16];

	uint16_t reserved_21[2];

	/* Offset 500. */
	/* HW Parameter Block. */
	uint16_t pcie_table_sig;
	uint16_t pcie_table_offset;

	uint16_t subsystem_vendor_id;
	uint16_t subsystem_device_id;

	uint32_t checksum;
};

/*
 * ISP Initialization Control Block.
 * Little endian except where noted.
 */
#define	ICB_VERSION 1
struct init_cb_24xx {
	uint16_t version;
	uint16_t reserved_1;

	uint16_t frame_payload_size;
	uint16_t execution_throttle;
	uint16_t exchange_count;

	uint16_t hard_address;

	uint8_t port_name[WWN_SIZE];		/* Big endian. */
	uint8_t node_name[WWN_SIZE];		/* Big endian. */

	uint16_t response_q_inpointer;
	uint16_t request_q_outpointer;

	uint16_t login_retry_count;

	uint16_t prio_request_q_outpointer;

	uint16_t response_q_length;
	uint16_t request_q_length;

	uint16_t link_down_on_nos;		/* Milliseconds. */

	uint16_t prio_request_q_length;

	uint32_t request_q_address[2];
	uint32_t response_q_address[2];
	uint32_t prio_request_q_address[2];

	uint8_t reserved_2[8];

	uint16_t atio_q_inpointer;
	uint16_t atio_q_length;
	uint32_t atio_q_address[2];

	uint16_t interrupt_delay_timer;		/* 100us increments. */
	uint16_t login_timeout;

	/*
	 * BIT 0  = Enable Hard Loop Id
	 * BIT 1  = Enable Fairness
	 * BIT 2  = Enable Full-Duplex
	 * BIT 3  = Reserved
	 * BIT 4  = Enable Target Mode
	 * BIT 5  = Disable Initiator Mode
	 * BIT 6  = Reserved
	 * BIT 7  = Reserved
	 *
	 * BIT 8  = Reserved
	 * BIT 9  = Non Participating LIP
	 * BIT 10 = Descending Loop ID Search
	 * BIT 11 = Acquire Loop ID in LIPA
	 * BIT 12 = Reserved
	 * BIT 13 = Full Login after LIP
	 * BIT 14 = Node Name Option
	 * BIT 15-31 = Reserved
	 */
	uint32_t firmware_options_1;

	/*
	 * BIT 0  = Operation Mode bit 0
	 * BIT 1  = Operation Mode bit 1
	 * BIT 2  = Operation Mode bit 2
	 * BIT 3  = Operation Mode bit 3
	 * BIT 4  = Connection Options bit 0
	 * BIT 5  = Connection Options bit 1
	 * BIT 6  = Connection Options bit 2
	 * BIT 7  = Enable Non part on LIHA failure
	 *
	 * BIT 8  = Enable Class 2
	 * BIT 9  = Enable ACK0
	 * BIT 10 = Reserved
	 * BIT 11 = Enable FC-SP Security
	 * BIT 12 = FC Tape Enable
	 * BIT 13 = Reserved
	 * BIT 14 = Enable Target PRLI Control
	 * BIT 15-31 = Reserved
	 */
	uint32_t firmware_options_2;

	/*
	 * BIT 0  = Reserved
	 * BIT 1  = Soft ID only
	 * BIT 2  = Reserved
	 * BIT 3  = Reserved
	 * BIT 4  = FCP RSP Payload bit 0
	 * BIT 5  = FCP RSP Payload bit 1
	 * BIT 6  = Enable Receive Out-of-Order data frame handling
	 * BIT 7  = Disable Automatic PLOGI on Local Loop
	 *
	 * BIT 8  = Reserved
	 * BIT 9  = Enable Out-of-Order FCP_XFER_RDY relative offset handling
	 * BIT 10 = Reserved
	 * BIT 11 = Reserved
	 * BIT 12 = Reserved
	 * BIT 13 = Data Rate bit 0
	 * BIT 14 = Data Rate bit 1
	 * BIT 15 = Data Rate bit 2
	 * BIT 16 = Enable 75 ohm Termination Select
	 * BIT 17-31 = Reserved
	 */
	uint32_t firmware_options_3;

	uint8_t  reserved_3[24];
};

/*
 * ISP queue - command entry structure definition.
 */
#define COMMAND_TYPE_6	0x48		/* Command Type 6 entry */
struct cmd_type_6 {
	uint8_t entry_type;		/* Entry type. */
	uint8_t entry_count;		/* Entry count. */
	uint8_t sys_define;		/* System defined. */
	uint8_t entry_status;		/* Entry Status. */

	uint32_t handle;		/* System handle. */

	uint16_t nport_handle;		/* N_PORT handle. */
	uint16_t timeout;		/* Command timeout. */

	uint16_t dseg_count;		/* Data segment count. */

	uint16_t fcp_rsp_dsd_len;	/* FCP_RSP DSD length. */

	struct scsi_lun lun;		/* FCP LUN (BE). */

	uint16_t control_flags;		/* Control flags. */
#define CF_DATA_SEG_DESCR_ENABLE	BIT_2
#define CF_READ_DATA			BIT_1
#define CF_WRITE_DATA			BIT_0

	uint16_t fcp_cmnd_dseg_len;		/* Data segment length. */
	uint32_t fcp_cmnd_dseg_address[2];	/* Data segment address. */

	uint32_t fcp_rsp_dseg_address[2];	/* Data segment address. */

	uint32_t byte_count;		/* Total byte count. */

	uint8_t port_id[3];		/* PortID of destination port. */
	uint8_t vp_index;

	uint32_t fcp_data_dseg_address[2];	/* Data segment address. */
	uint16_t fcp_data_dseg_len;		/* Data segment length. */
	uint16_t reserved_1;			/* MUST be set to 0. */
};

#define COMMAND_TYPE_7	0x18		/* Command Type 7 entry */
struct cmd_type_7 {
	uint8_t entry_type;		/* Entry type. */
	uint8_t entry_count;		/* Entry count. */
	uint8_t sys_define;		/* System defined. */
	uint8_t entry_status;		/* Entry Status. */

	uint32_t handle;		/* System handle. */

	uint16_t nport_handle;		/* N_PORT handle. */
	uint16_t timeout;		/* Command timeout. */
#define FW_MAX_TIMEOUT		0x1999

	uint16_t dseg_count;		/* Data segment count. */
	uint16_t reserved_1;

	struct scsi_lun lun;		/* FCP LUN (BE). */

	uint16_t task_mgmt_flags;	/* Task management flags. */
#define TMF_CLEAR_ACA		BIT_14
#define TMF_TARGET_RESET	BIT_13
#define TMF_LUN_RESET		BIT_12
#define TMF_CLEAR_TASK_SET	BIT_10
#define TMF_ABORT_TASK_SET	BIT_9
#define TMF_DSD_LIST_ENABLE	BIT_2
#define TMF_READ_DATA		BIT_1
#define TMF_WRITE_DATA		BIT_0

	uint8_t task;
#define TSK_SIMPLE		0
#define TSK_HEAD_OF_QUEUE	1
#define TSK_ORDERED		2
#define TSK_ACA			4
#define TSK_UNTAGGED		5

	uint8_t crn;

	uint8_t fcp_cdb[MAX_CMDSZ]; 	/* SCSI command words. */
	uint32_t byte_count;		/* Total byte count. */

	uint8_t port_id[3];		/* PortID of destination port. */
	uint8_t vp_index;

	uint32_t dseg_0_address[2];	/* Data segment 0 address. */
	uint32_t dseg_0_len;		/* Data segment 0 length. */
};

/*
 * ISP queue - status entry structure definition.
 */
#define	STATUS_TYPE	0x03		/* Status entry. */
struct sts_entry_24xx {
	uint8_t entry_type;		/* Entry type. */
	uint8_t entry_count;		/* Entry count. */
	uint8_t sys_define;		/* System defined. */
	uint8_t entry_status;		/* Entry Status. */

	uint32_t handle;		/* System handle. */

	uint16_t comp_status;		/* Completion status. */
	uint16_t ox_id;			/* OX_ID used by the firmware. */

	uint32_t residual_len;		/* FW calc residual transfer length. */

	uint16_t reserved_1;
	uint16_t state_flags;		/* State flags. */
#define SF_TRANSFERRED_DATA	BIT_11
#define SF_FCP_RSP_DMA		BIT_0

	uint16_t reserved_2;
	uint16_t scsi_status;		/* SCSI status. */
#define SS_CONFIRMATION_REQ		BIT_12

	uint32_t rsp_residual_count;	/* FCP RSP residual count. */

	uint32_t sense_len;		/* FCP SENSE length. */
	uint32_t rsp_data_len;		/* FCP response data length. */

	uint8_t data[28];		/* FCP response/sense information. */
};

/*
 * Status entry completion status
 */
#define CS_DATA_REASSEMBLY_ERROR 0x11	/* Data Reassembly Error.. */
#define CS_ABTS_BY_TARGET	0x13	/* Target send ABTS to abort IOCB. */
#define CS_FW_RESOURCE		0x2C	/* Firmware Resource Unavailable. */
#define CS_TASK_MGMT_OVERRUN	0x30	/* Task management overrun (8+). */
#define CS_ABORT_BY_TARGET	0x47	/* Abort By Target. */

/*
 * ISP queue - marker entry structure definition.
 */
#define MARKER_TYPE	0x04		/* Marker entry. */
struct mrk_entry_24xx {
	uint8_t entry_type;		/* Entry type. */
	uint8_t entry_count;		/* Entry count. */
	uint8_t handle_count;		/* Handle count. */
	uint8_t entry_status;		/* Entry Status. */

	uint32_t handle;		/* System handle. */

	uint16_t nport_handle;		/* N_PORT handle. */

	uint8_t modifier;		/* Modifier (7-0). */
#define MK_SYNC_ID_LUN	0		/* Synchronize ID/LUN */
#define MK_SYNC_ID	1		/* Synchronize ID */
#define MK_SYNC_ALL	2		/* Synchronize all ID/LUN */
	uint8_t reserved_1;

	uint8_t reserved_2;
	uint8_t vp_index;

	uint16_t reserved_3;

	uint8_t lun[8];			/* FCP LUN (BE). */
	uint8_t reserved_4[40];
};

/*
 * ISP queue - CT Pass-Through entry structure definition.
 */
#define CT_IOCB_TYPE		0x29	/* CT Pass-Through IOCB entry */
struct ct_entry_24xx {
	uint8_t entry_type;		/* Entry type. */
	uint8_t entry_count;		/* Entry count. */
	uint8_t sys_define;		/* System Defined. */
	uint8_t entry_status;		/* Entry Status. */

	uint32_t handle;		/* System handle. */

	uint16_t comp_status;		/* Completion status. */

	uint16_t nport_handle;		/* N_PORT handle. */

	uint16_t cmd_dsd_count;

	uint8_t vp_index;
	uint8_t reserved_1;

	uint16_t timeout;		/* Command timeout. */
	uint16_t reserved_2;

	uint16_t rsp_dsd_count;

	uint8_t reserved_3[10];

	uint32_t rsp_byte_count;
	uint32_t cmd_byte_count;

	uint32_t dseg_0_address[2];	/* Data segment 0 address. */
	uint32_t dseg_0_len;		/* Data segment 0 length. */
	uint32_t dseg_1_address[2];	/* Data segment 1 address. */
	uint32_t dseg_1_len;		/* Data segment 1 length. */
};

/*
 * ISP queue - ELS Pass-Through entry structure definition.
 */
#define ELS_IOCB_TYPE		0x53	/* ELS Pass-Through IOCB entry */
struct els_entry_24xx {
	uint8_t entry_type;		/* Entry type. */
	uint8_t entry_count;		/* Entry count. */
	uint8_t sys_define;		/* System Defined. */
	uint8_t entry_status;		/* Entry Status. */

	uint32_t handle;		/* System handle. */

	uint16_t reserved_1;

	uint16_t nport_handle;		/* N_PORT handle. */

	uint16_t tx_dsd_count;

	uint8_t vp_index;
	uint8_t sof_type;
#define EST_SOFI3		(1 << 4)
#define EST_SOFI2		(3 << 4)

	uint32_t rx_xchg_address;	/* Receive exchange address. */
	uint16_t rx_dsd_count;

	uint8_t opcode;
	uint8_t reserved_2;

	uint8_t port_id[3];
	uint8_t reserved_3;

	uint16_t reserved_4;

	uint16_t control_flags;		/* Control flags. */
#define ECF_PAYLOAD_DESCR_MASK	(BIT_15|BIT_14|BIT_13)
#define EPD_ELS_COMMAND		(0 << 13)
#define EPD_ELS_ACC		(1 << 13)
#define EPD_ELS_RJT		(2 << 13)
#define EPD_RX_XCHG		(3 << 13)
#define ECF_CLR_PASSTHRU_PEND	BIT_12
#define ECF_INCL_FRAME_HDR	BIT_11

	uint32_t rx_byte_count;
	uint32_t tx_byte_count;

	uint32_t tx_address[2];		/* Data segment 0 address. */
	uint32_t tx_len;		/* Data segment 0 length. */
	uint32_t rx_address[2];		/* Data segment 1 address. */
	uint32_t rx_len;		/* Data segment 1 length. */
};

/*
 * ISP queue - Mailbox Command entry structure definition.
 */
#define MBX_IOCB_TYPE	0x39
struct mbx_entry_24xx {
	uint8_t entry_type;		/* Entry type. */
	uint8_t entry_count;		/* Entry count. */
	uint8_t handle_count;		/* Handle count. */
	uint8_t entry_status;		/* Entry Status. */

	uint32_t handle;		/* System handle. */

	uint16_t mbx[28];
};


#define LOGINOUT_PORT_IOCB_TYPE	0x52	/* Login/Logout Port entry. */
struct logio_entry_24xx {
	uint8_t entry_type;		/* Entry type. */
	uint8_t entry_count;		/* Entry count. */
	uint8_t sys_define;		/* System defined. */
	uint8_t entry_status;		/* Entry Status. */

	uint32_t handle;		/* System handle. */

	uint16_t comp_status;		/* Completion status. */
#define CS_LOGIO_ERROR		0x31	/* Login/Logout IOCB error. */

	uint16_t nport_handle;		/* N_PORT handle. */

	uint16_t control_flags;		/* Control flags. */
					/* Modifiers. */
#define LCF_INCLUDE_SNS		BIT_10	/* Include SNS (FFFFFC) during LOGO. */
#define LCF_FCP2_OVERRIDE	BIT_9	/* Set/Reset word 3 of PRLI. */
#define LCF_CLASS_2		BIT_8	/* Enable class 2 during PLOGI. */
#define LCF_FREE_NPORT		BIT_7	/* Release NPORT handle after LOGO. */
#define LCF_EXPL_LOGO		BIT_6	/* Perform an explicit LOGO. */
#define LCF_SKIP_PRLI		BIT_5	/* Skip PRLI after PLOGI. */
#define LCF_IMPL_LOGO_ALL	BIT_5	/* Implicit LOGO to all ports. */
#define LCF_COND_PLOGI		BIT_4	/* PLOGI only if not logged-in. */
#define LCF_IMPL_LOGO		BIT_4	/* Perform an implicit LOGO. */
#define LCF_IMPL_PRLO		BIT_4	/* Perform an implicit PRLO. */
					/* Commands. */
#define LCF_COMMAND_PLOGI	0x00	/* PLOGI. */
#define LCF_COMMAND_PRLI	0x01	/* PRLI. */
#define LCF_COMMAND_PDISC	0x02	/* PDISC. */
#define LCF_COMMAND_ADISC	0x03	/* ADISC. */
#define LCF_COMMAND_LOGO	0x08	/* LOGO. */
#define LCF_COMMAND_PRLO	0x09	/* PRLO. */
#define LCF_COMMAND_TPRLO	0x0A	/* TPRLO. */

	uint8_t vp_index;
	uint8_t reserved_1;

	uint8_t port_id[3];		/* PortID of destination port. */

	uint8_t rsp_size;		/* Response size in 32bit words. */

	uint32_t io_parameter[11];	/* General I/O parameters. */
#define LSC_SCODE_NOLINK	0x01
#define LSC_SCODE_NOIOCB	0x02
#define LSC_SCODE_NOXCB		0x03
#define LSC_SCODE_CMD_FAILED	0x04
#define LSC_SCODE_NOFABRIC	0x05
#define LSC_SCODE_FW_NOT_READY	0x07
#define LSC_SCODE_NOT_LOGGED_IN	0x09
#define LSC_SCODE_NOPCB		0x0A

#define LSC_SCODE_ELS_REJECT	0x18
#define LSC_SCODE_CMD_PARAM_ERR	0x19
#define LSC_SCODE_PORTID_USED	0x1A
#define LSC_SCODE_NPORT_USED	0x1B
#define LSC_SCODE_NONPORT	0x1C
#define LSC_SCODE_LOGGED_IN	0x1D
#define LSC_SCODE_NOFLOGI_ACC	0x1F
};

#define TSK_MGMT_IOCB_TYPE	0x14
struct tsk_mgmt_entry {
	uint8_t entry_type;		/* Entry type. */
	uint8_t entry_count;		/* Entry count. */
	uint8_t handle_count;		/* Handle count. */
	uint8_t entry_status;		/* Entry Status. */

	uint32_t handle;		/* System handle. */

	uint16_t nport_handle;		/* N_PORT handle. */

	uint16_t reserved_1;

	uint16_t delay;			/* Activity delay in seconds. */

	uint16_t timeout;		/* Command timeout. */

	uint8_t lun[8];			/* FCP LUN (BE). */

	uint32_t control_flags;		/* Control Flags. */
#define TCF_NOTMCMD_TO_TARGET	BIT_31
#define TCF_LUN_RESET		BIT_4
#define TCF_ABORT_TASK_SET	BIT_3
#define TCF_CLEAR_TASK_SET	BIT_2
#define TCF_TARGET_RESET	BIT_1
#define TCF_CLEAR_ACA		BIT_0

	uint8_t reserved_2[20];

	uint8_t port_id[3];		/* PortID of destination port. */
	uint8_t vp_index;

	uint8_t reserved_3[12];
};

#define ABORT_IOCB_TYPE	0x33
struct abort_entry_24xx {
	uint8_t entry_type;		/* Entry type. */
	uint8_t entry_count;		/* Entry count. */
	uint8_t handle_count;		/* Handle count. */
	uint8_t entry_status;		/* Entry Status. */

	uint32_t handle;		/* System handle. */

	uint16_t nport_handle;		/* N_PORT handle. */
					/* or Completion status. */

	uint16_t options;		/* Options. */
#define AOF_NO_ABTS		BIT_0	/* Do not send any ABTS. */

	uint32_t handle_to_abort;	/* System handle to abort. */

	uint8_t reserved_1[32];

	uint8_t port_id[3];		/* PortID of destination port. */
	uint8_t vp_index;

	uint8_t reserved_2[12];
};

/*
 * ISP I/O Register Set structure definitions.
 */
struct device_reg_24xx {
	uint32_t flash_addr;		/* Flash/NVRAM BIOS address. */
#define FARX_DATA_FLAG	BIT_31
#define FARX_ACCESS_FLASH_CONF	0x7FFD0000
#define FARX_ACCESS_FLASH_DATA	0x7FF00000
#define FARX_ACCESS_NVRAM_CONF	0x7FFF0000
#define FARX_ACCESS_NVRAM_DATA	0x7FFE0000

#define FA_NVRAM_FUNC0_ADDR	0x80
#define FA_NVRAM_FUNC1_ADDR	0x180

#define FA_NVRAM_VPD_SIZE	0x200
#define FA_NVRAM_VPD0_ADDR	0x00
#define FA_NVRAM_VPD1_ADDR	0x100
					/*
					 * RISC code begins at offset 512KB
					 * within flash. Consisting of two
					 * contiguous RISC code segments.
					 */
#define FA_RISC_CODE_ADDR	0x20000
#define FA_RISC_CODE_SEGMENTS	2

#define FA_FW_AREA_ADDR		0x40000
#define FA_VPD_NVRAM_ADDR	0x48000
#define FA_FEATURE_ADDR		0x4C000
#define FA_FLASH_DESCR_ADDR	0x50000
#define FA_HW_EVENT_ADDR	0x54000
#define FA_BOOT_LOG_ADDR	0x58000
#define FA_FW_DUMP0_ADDR	0x60000
#define FA_FW_DUMP1_ADDR	0x70000

	uint32_t flash_data;		/* Flash/NVRAM BIOS data. */

	uint32_t ctrl_status;		/* Control/Status. */
#define CSRX_FLASH_ACCESS_ERROR	BIT_18	/* Flash/NVRAM Access Error. */
#define CSRX_DMA_ACTIVE		BIT_17	/* DMA Active status. */
#define CSRX_DMA_SHUTDOWN	BIT_16	/* DMA Shutdown control status. */
#define CSRX_FUNCTION		BIT_15	/* Function number. */
					/* PCI-X Bus Mode. */
#define CSRX_PCIX_BUS_MODE_MASK	(BIT_11|BIT_10|BIT_9|BIT_8)
#define PBM_PCI_33MHZ		(0 << 8)
#define PBM_PCIX_M1_66MHZ	(1 << 8)
#define PBM_PCIX_M1_100MHZ	(2 << 8)
#define PBM_PCIX_M1_133MHZ	(3 << 8)
#define PBM_PCIX_M2_66MHZ	(5 << 8)
#define PBM_PCIX_M2_100MHZ	(6 << 8)
#define PBM_PCIX_M2_133MHZ	(7 << 8)
#define PBM_PCI_66MHZ		(8 << 8)
					/* Max Write Burst byte count. */
#define CSRX_MAX_WRT_BURST_MASK	(BIT_5|BIT_4)
#define MWB_512_BYTES		(0 << 4)
#define MWB_1024_BYTES		(1 << 4)
#define MWB_2048_BYTES		(2 << 4)
#define MWB_4096_BYTES		(3 << 4)

#define CSRX_64BIT_SLOT		BIT_2	/* PCI 64-Bit Bus Slot. */
#define CSRX_FLASH_ENABLE	BIT_1	/* Flash BIOS Read/Write enable. */
#define CSRX_ISP_SOFT_RESET	BIT_0	/* ISP soft reset. */

	uint32_t ictrl;			/* Interrupt control. */
#define ICRX_EN_RISC_INT	BIT_3	/* Enable RISC interrupts on PCI. */

	uint32_t istatus;		/* Interrupt status. */
#define ISRX_RISC_INT		BIT_3	/* RISC interrupt. */

	uint32_t unused_1[2];		/* Gap. */

					/* Request Queue. */
	uint32_t req_q_in;		/*  In-Pointer. */
	uint32_t req_q_out;		/*  Out-Pointer. */
					/* Response Queue. */
	uint32_t rsp_q_in;		/*  In-Pointer. */
	uint32_t rsp_q_out;		/*  Out-Pointer. */
					/* Priority Request Queue. */
	uint32_t preq_q_in;		/*  In-Pointer. */
	uint32_t preq_q_out;		/*  Out-Pointer. */

	uint32_t unused_2[2];		/* Gap. */

					/* ATIO Queue. */
	uint32_t atio_q_in;		/*  In-Pointer. */
	uint32_t atio_q_out;		/*  Out-Pointer. */

	uint32_t host_status;
#define HSRX_RISC_INT		BIT_15	/* RISC to Host interrupt. */
#define HSRX_RISC_PAUSED	BIT_8	/* RISC Paused. */

	uint32_t hccr;			/* Host command & control register. */
					/* HCCR statuses. */
#define HCCRX_HOST_INT		BIT_6	/* Host to RISC interrupt bit. */
#define HCCRX_RISC_RESET	BIT_5	/* RISC Reset mode bit. */
#define HCCRX_RISC_PAUSE	BIT_4	/* RISC Pause mode bit. */
					/* HCCR commands. */
					/* NOOP. */
#define HCCRX_NOOP		0x00000000
					/* Set RISC Reset. */
#define HCCRX_SET_RISC_RESET	0x10000000
					/* Clear RISC Reset. */
#define HCCRX_CLR_RISC_RESET	0x20000000
					/* Set RISC Pause. */
#define HCCRX_SET_RISC_PAUSE	0x30000000
					/* Releases RISC Pause. */
#define HCCRX_REL_RISC_PAUSE	0x40000000
					/* Set HOST to RISC interrupt. */
#define HCCRX_SET_HOST_INT	0x50000000
					/* Clear HOST to RISC interrupt. */
#define HCCRX_CLR_HOST_INT	0x60000000
					/* Clear RISC to PCI interrupt. */
#define HCCRX_CLR_RISC_INT	0xA0000000

	uint32_t gpiod;			/* GPIO Data register. */

					/* LED update mask. */
#define GPDX_LED_UPDATE_MASK	(BIT_20|BIT_19|BIT_18)
					/* Data update mask. */
#define GPDX_DATA_UPDATE_MASK	(BIT_17|BIT_16)
					/* Data update mask. */
#define GPDX_DATA_UPDATE_2_MASK	(BIT_28|BIT_27|BIT_26|BIT_17|BIT_16)
					/* LED control mask. */
#define GPDX_LED_COLOR_MASK	(BIT_4|BIT_3|BIT_2)
					/* LED bit values. Color names as
					 * referenced in fw spec.
					 */
#define GPDX_LED_YELLOW_ON	BIT_2
#define GPDX_LED_GREEN_ON	BIT_3
#define GPDX_LED_AMBER_ON	BIT_4
					/* Data in/out. */
#define GPDX_DATA_INOUT		(BIT_1|BIT_0)

	uint32_t gpioe;			/* GPIO Enable register. */
					/* Enable update mask. */
#define GPEX_ENABLE_UPDATE_MASK	(BIT_17|BIT_16)
					/* Enable update mask. */
#define GPEX_ENABLE_UPDATE_2_MASK (BIT_28|BIT_27|BIT_26|BIT_17|BIT_16)
					/* Enable. */
#define GPEX_ENABLE		(BIT_1|BIT_0)

	uint32_t iobase_addr;		/* I/O Bus Base Address register. */

	uint32_t unused_3[10];		/* Gap. */

	uint16_t mailbox0;
	uint16_t mailbox1;
	uint16_t mailbox2;
	uint16_t mailbox3;
	uint16_t mailbox4;
	uint16_t mailbox5;
	uint16_t mailbox6;
	uint16_t mailbox7;
	uint16_t mailbox8;
	uint16_t mailbox9;
	uint16_t mailbox10;
	uint16_t mailbox11;
	uint16_t mailbox12;
	uint16_t mailbox13;
	uint16_t mailbox14;
	uint16_t mailbox15;
	uint16_t mailbox16;
	uint16_t mailbox17;
	uint16_t mailbox18;
	uint16_t mailbox19;
	uint16_t mailbox20;
	uint16_t mailbox21;
	uint16_t mailbox22;
	uint16_t mailbox23;
	uint16_t mailbox24;
	uint16_t mailbox25;
	uint16_t mailbox26;
	uint16_t mailbox27;
	uint16_t mailbox28;
	uint16_t mailbox29;
	uint16_t mailbox30;
	uint16_t mailbox31;

	uint32_t iobase_window;
	uint32_t unused_4[8];		/* Gap. */
	uint32_t iobase_q;
	uint32_t unused_5[2];		/* Gap. */
	uint32_t iobase_select;
	uint32_t unused_6[2];		/* Gap. */
	uint32_t iobase_sdata;
};

/* MID Support ***************************************************************/

#define MAX_MID_VPS	125

struct mid_conf_entry_24xx {
	uint16_t reserved_1;

	/*
	 * BIT 0  = Enable Hard Loop Id
	 * BIT 1  = Acquire Loop ID in LIPA
	 * BIT 2  = ID not Acquired
	 * BIT 3  = Enable VP
	 * BIT 4  = Enable Initiator Mode
	 * BIT 5  = Disable Target Mode
	 * BIT 6-7 = Reserved
	 */
	uint8_t options;

	uint8_t hard_address;

	uint8_t port_name[WWN_SIZE];
	uint8_t node_name[WWN_SIZE];
};

struct mid_init_cb_24xx {
	struct init_cb_24xx init_cb;

	uint16_t count;
	uint16_t options;

	struct mid_conf_entry_24xx entries[MAX_MID_VPS];
};


struct mid_db_entry_24xx {
	uint16_t status;
#define MDBS_NON_PARTIC		BIT_3
#define MDBS_ID_ACQUIRED	BIT_1
#define MDBS_ENABLED		BIT_0

	uint8_t options;
	uint8_t hard_address;

	uint8_t port_name[WWN_SIZE];
	uint8_t node_name[WWN_SIZE];

	uint8_t port_id[3];
	uint8_t reserved_1;
};

struct mid_db_24xx {
	struct mid_db_entry_24xx entries[MAX_MID_VPS];
};

 /*
 * Virtual Fabric ID type definition.
 */
typedef struct vf_id {
        uint16_t id : 12;
        uint16_t priority : 4;
} vf_id_t;

/*
 * Virtual Fabric HopCt type definition.
 */
typedef struct vf_hopct {
        uint16_t reserved : 8;
        uint16_t hopct : 8;
} vf_hopct_t;

/*
 * Virtual Port Control IOCB
 */
#define VP_CTRL_IOCB_TYPE	0x30	/* Vitual Port Control entry. */
struct vp_ctrl_entry_24xx {
	uint8_t entry_type;		/* Entry type. */
	uint8_t entry_count;		/* Entry count. */
	uint8_t sys_define;		/* System defined. */
	uint8_t entry_status;		/* Entry Status. */

	uint32_t handle;		/* System handle. */

	uint16_t vp_idx_failed;

	uint16_t comp_status;		/* Completion status. */
#define CS_VCE_IOCB_ERROR       0x01    /* Error processing IOCB */
#define CS_VCE_ACQ_ID_ERROR	0x02	/* Error while acquireing ID. */
#define CS_VCE_BUSY		0x05	/* Firmware not ready to accept cmd. */

	uint16_t command;
#define VCE_COMMAND_ENABLE_VPS	0x00	/* Enable VPs. */
#define VCE_COMMAND_DISABLE_VPS	0x08	/* Disable VPs. */
#define VCE_COMMAND_DISABLE_VPS_REINIT	0x09 /* Disable VPs and reinit link. */
#define VCE_COMMAND_DISABLE_VPS_LOGO	0x0a /* Disable VPs and LOGO ports. */
#define VCE_COMMAND_DISABLE_VPS_LOGO_ALL        0x0b /* Disable VPs and LOGO ports. */

	uint16_t vp_count;

	uint8_t vp_idx_map[16];
	uint16_t flags;
	struct vf_id    id;
	uint16_t reserved_4;
	struct vf_hopct  hopct;
	uint8_t reserved_5[8];
};

/*
 * Modify Virtual Port Configuration IOCB
 */
#define VP_CONFIG_IOCB_TYPE	0x31	/* Vitual Port Config entry. */
struct vp_config_entry_24xx {
	uint8_t entry_type;		/* Entry type. */
	uint8_t entry_count;		/* Entry count. */
	uint8_t handle_count;
	uint8_t entry_status;		/* Entry Status. */

	uint32_t handle;		/* System handle. */

	uint16_t flags;
#define CS_VF_BIND_VPORTS_TO_VF         BIT_0
#define CS_VF_SET_QOS_OF_VPORTS         BIT_1
#define CS_VF_SET_HOPS_OF_VPORTS        BIT_2

	uint16_t comp_status;		/* Completion status. */
#define CS_VCT_STS_ERROR	0x01	/* Specified VPs were not disabled. */
#define CS_VCT_CNT_ERROR	0x02	/* Invalid VP count. */
#define CS_VCT_ERROR		0x03	/* Unknown error. */
#define CS_VCT_IDX_ERROR	0x02	/* Invalid VP index. */
#define CS_VCT_BUSY		0x05	/* Firmware not ready to accept cmd. */

	uint8_t command;
#define VCT_COMMAND_MOD_VPS     0x00    /* Modify VP configurations. */
#define VCT_COMMAND_MOD_ENABLE_VPS 0x01 /* Modify configuration & enable VPs. */

	uint8_t vp_count;

	uint8_t vp_index1;
	uint8_t vp_index2;

	uint8_t options_idx1;
	uint8_t hard_address_idx1;
	uint16_t reserved_vp1;
	uint8_t port_name_idx1[WWN_SIZE];
	uint8_t node_name_idx1[WWN_SIZE];

	uint8_t options_idx2;
	uint8_t hard_address_idx2;
	uint16_t reserved_vp2;
	uint8_t port_name_idx2[WWN_SIZE];
	uint8_t node_name_idx2[WWN_SIZE];
	struct vf_id    id;
	uint16_t reserved_4;
	struct vf_hopct  hopct;
	uint8_t reserved_5;
};

#define VP_RPT_ID_IOCB_TYPE	0x32	/* Report ID Acquisition entry. */
struct vp_rpt_id_entry_24xx {
	uint8_t entry_type;		/* Entry type. */
	uint8_t entry_count;		/* Entry count. */
	uint8_t sys_define;		/* System defined. */
	uint8_t entry_status;		/* Entry Status. */

	uint32_t handle;		/* System handle. */

	uint16_t vp_count;		/* Format 0 -- | VP setup | VP acq |. */
					/* Format 1 -- | VP count |. */
	uint16_t vp_idx;		/* Format 0 -- Reserved. */
					/* Format 1 -- VP status and index. */

	uint8_t port_id[3];
	uint8_t format;

	uint8_t vp_idx_map[16];

	uint8_t reserved_4[32];
};

#define VF_EVFP_IOCB_TYPE       0x26    /* Exchange Virtual Fabric Parameters entry. */
struct vf_evfp_entry_24xx {
        uint8_t entry_type;             /* Entry type. */
        uint8_t entry_count;            /* Entry count. */
        uint8_t sys_define;             /* System defined. */
        uint8_t entry_status;           /* Entry Status. */

        uint32_t handle;                /* System handle. */
        uint16_t comp_status;           /* Completion status. */
        uint16_t timeout;               /* timeout */
        uint16_t adim_tagging_mode;

        uint16_t vfport_id;
        uint32_t exch_addr;

        uint16_t nport_handle;          /* N_PORT handle. */
        uint16_t control_flags;
        uint32_t io_parameter_0;
        uint32_t io_parameter_1;
        uint32_t tx_address[2];         /* Data segment 0 address. */
        uint32_t tx_len;                /* Data segment 0 length. */
        uint32_t rx_address[2];         /* Data segment 1 address. */
        uint32_t rx_len;                /* Data segment 1 length. */
};

/* END MID Support ***********************************************************/
#endif
