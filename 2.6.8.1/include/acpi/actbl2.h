/******************************************************************************
 *
 * Name: actbl2.h - ACPI Specification Revision 2.0 Tables
 *
 *****************************************************************************/

/*
 * Copyright (C) 2000 - 2004, R. Byron Moore
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    substantially similar to the "NO WARRANTY" disclaimer below
 *    ("Disclaimer") and any redistribution must be conditioned upon
 *    including a substantially similar Disclaimer requirement for further
 *    binary redistribution.
 * 3. Neither the names of the above-listed copyright holders nor the names
 *    of any contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGES.
 */

#ifndef __ACTBL2_H__
#define __ACTBL2_H__

/*
 * Prefered Power Management Profiles
 */
#define PM_UNSPECIFIED                  0
#define PM_DESKTOP                      1
#define PM_MOBILE                       2
#define PM_WORKSTATION                  3
#define PM_ENTERPRISE_SERVER            4
#define PM_SOHO_SERVER                  5
#define PM_APPLIANCE_PC                 6

/*
 * ACPI Boot Arch Flags
 */
#define BAF_LEGACY_DEVICES              0x0001
#define BAF_8042_KEYBOARD_CONTROLLER    0x0002

#define FADT2_REVISION_ID               3


#pragma pack(1)

/*
 * ACPI 2.0 Root System Description Table (RSDT)
 */
struct rsdt_descriptor_rev2
{
	ACPI_TABLE_HEADER_DEF                           /* ACPI common table header */
	u32                             table_offset_entry [1]; /* Array of pointers to  */
			 /* ACPI table headers */
};


/*
 * ACPI 2.0 Extended System Description Table (XSDT)
 */
struct xsdt_descriptor_rev2
{
	ACPI_TABLE_HEADER_DEF                           /* ACPI common table header */
	u64                             table_offset_entry [1]; /* Array of pointers to  */
			 /* ACPI table headers */
};


/*
 * ACPI 2.0 Firmware ACPI Control Structure (FACS)
 */
struct facs_descriptor_rev2
{
	char                            signature[4];           /* ACPI signature */
	u32                             length;                 /* Length of structure, in bytes */
	u32                             hardware_signature;     /* Hardware configuration signature */
	u32                             firmware_waking_vector; /* 32bit physical address of the Firmware Waking Vector. */
	u32                             global_lock;            /* Global Lock used to synchronize access to shared hardware resources */
	u32                             S4bios_f        : 1;    /* S4Bios_f - Indicates if S4BIOS support is present */
	u32                             reserved1       : 31;   /* Must be 0 */
	u64                             xfirmware_waking_vector; /* 64bit physical address of the Firmware Waking Vector. */
	u8                              version;                /* Version of this table */
	u8                              reserved3 [31];         /* Reserved - must be zero */
};


/*
 * ACPI 2.0 Generic Address Structure (GAS)
 */
struct acpi_generic_address
{
	u8                              address_space_id;       /* Address space where struct or register exists. */
	u8                              register_bit_width;     /* Size in bits of given register */
	u8                              register_bit_offset;    /* Bit offset within the register */
	u8                              reserved;               /* Must be 0 */
	u64                             address;                /* 64-bit address of struct or register */
};


/*
 * ACPI 2.0 Fixed ACPI Description Table (FADT)
 */
struct fadt_descriptor_rev2
{
	ACPI_TABLE_HEADER_DEF                       /* ACPI common table header */
	u32                             V1_firmware_ctrl;   /* 32-bit physical address of FACS */
	u32                             V1_dsdt;            /* 32-bit physical address of DSDT */
	u8                              reserved1;          /* System Interrupt Model isn't used in ACPI 2.0*/
	u8                              prefer_PM_profile;  /* Conveys preferred power management profile to OSPM. */
	u16                             sci_int;            /* System vector of SCI interrupt */
	u32                             smi_cmd;            /* Port address of SMI command port */
	u8                              acpi_enable;        /* Value to write to smi_cmd to enable ACPI */
	u8                              acpi_disable;       /* Value to write to smi_cmd to disable ACPI */
	u8                              S4bios_req;         /* Value to write to SMI CMD to enter S4BIOS state */
	u8                              pstate_cnt;         /* Processor performance state control*/
	u32                             V1_pm1a_evt_blk;    /* Port address of Power Mgt 1a acpi_event Reg Blk */
	u32                             V1_pm1b_evt_blk;    /* Port address of Power Mgt 1b acpi_event Reg Blk */
	u32                             V1_pm1a_cnt_blk;    /* Port address of Power Mgt 1a Control Reg Blk */
	u32                             V1_pm1b_cnt_blk;    /* Port address of Power Mgt 1b Control Reg Blk */
	u32                             V1_pm2_cnt_blk;     /* Port address of Power Mgt 2 Control Reg Blk */
	u32                             V1_pm_tmr_blk;      /* Port address of Power Mgt Timer Ctrl Reg Blk */
	u32                             V1_gpe0_blk;        /* Port addr of General Purpose acpi_event 0 Reg Blk */
	u32                             V1_gpe1_blk;        /* Port addr of General Purpose acpi_event 1 Reg Blk */
	u8                              pm1_evt_len;        /* Byte length of ports at pm1_x_evt_blk */
	u8                              pm1_cnt_len;        /* Byte length of ports at pm1_x_cnt_blk */
	u8                              pm2_cnt_len;        /* Byte Length of ports at pm2_cnt_blk */
	u8                              pm_tm_len;          /* Byte Length of ports at pm_tm_blk */
	u8                              gpe0_blk_len;       /* Byte Length of ports at gpe0_blk */
	u8                              gpe1_blk_len;       /* Byte Length of ports at gpe1_blk */
	u8                              gpe1_base;          /* Offset in gpe model where gpe1 events start */
	u8                              cst_cnt;            /* Support for the _CST object and C States change notification.*/
	u16                             plvl2_lat;          /* Worst case HW latency to enter/exit C2 state */
	u16                             plvl3_lat;          /* Worst case HW latency to enter/exit C3 state */
	u16                             flush_size;         /* Number of flush strides that need to be read */
	u16                             flush_stride;       /* Processor's memory cache line width, in bytes */
	u8                              duty_offset;        /* Processor's duty cycle index in processor's P_CNT reg*/
	u8                              duty_width;         /* Processor's duty cycle value bit width in P_CNT register.*/
	u8                              day_alrm;           /* Index to day-of-month alarm in RTC CMOS RAM */
	u8                              mon_alrm;           /* Index to month-of-year alarm in RTC CMOS RAM */
	u8                              century;            /* Index to century in RTC CMOS RAM */
	u16                             iapc_boot_arch;     /* IA-PC Boot Architecture Flags. See Table 5-10 for description*/
	u8                              reserved2;          /* Reserved */
	u32                             wb_invd     : 1;    /* The wbinvd instruction works properly */
	u32                             wb_invd_flush : 1;  /* The wbinvd flushes but does not invalidate */
	u32                             proc_c1     : 1;    /* All processors support C1 state */
	u32                             plvl2_up    : 1;    /* C2 state works on MP system */
	u32                             pwr_button  : 1;    /* Power button is handled as a generic feature */
	u32                             sleep_button : 1;   /* Sleep button is handled as a generic feature, or not present */
	u32                             fixed_rTC   : 1;    /* RTC wakeup stat not in fixed register space */
	u32                             rtcs4       : 1;    /* RTC wakeup stat not possible from S4 */
	u32                             tmr_val_ext : 1;    /* Indicates tmr_val is 32 bits 0=24-bits*/
	u32                             dock_cap    : 1;    /* Supports Docking */
	u32                             reset_reg_sup : 1;  /* Indicates system supports system reset via the FADT RESET_REG*/
	u32                             sealed_case : 1;    /* Indicates system has no internal expansion capabilities and case is sealed. */
	u32                             headless    : 1;    /* Indicates system does not have local video capabilities or local input devices.*/
	u32                             cpu_sw_sleep : 1;   /* Indicates to OSPM that a processor native instruction */
			   /* Must be executed after writing the SLP_TYPx register. */
	u32                             reserved6   : 18;   /* Reserved - must be zero */

	struct acpi_generic_address     reset_register;     /* Reset register address in GAS format */
	u8                              reset_value;        /* Value to write to the reset_register port to reset the system. */
	u8                              reserved7[3];       /* These three bytes must be zero */
	u64                             xfirmware_ctrl;     /* 64-bit physical address of FACS */
	u64                             Xdsdt;              /* 64-bit physical address of DSDT */
	struct acpi_generic_address     xpm1a_evt_blk;      /* Extended Power Mgt 1a acpi_event Reg Blk address */
	struct acpi_generic_address     xpm1b_evt_blk;      /* Extended Power Mgt 1b acpi_event Reg Blk address */
	struct acpi_generic_address     xpm1a_cnt_blk;      /* Extended Power Mgt 1a Control Reg Blk address */
	struct acpi_generic_address     xpm1b_cnt_blk;      /* Extended Power Mgt 1b Control Reg Blk address */
	struct acpi_generic_address     xpm2_cnt_blk;       /* Extended Power Mgt 2 Control Reg Blk address */
	struct acpi_generic_address     xpm_tmr_blk;        /* Extended Power Mgt Timer Ctrl Reg Blk address */
	struct acpi_generic_address     xgpe0_blk;          /* Extended General Purpose acpi_event 0 Reg Blk address */
	struct acpi_generic_address     xgpe1_blk;          /* Extended General Purpose acpi_event 1 Reg Blk address */
};


/* Embedded Controller */

struct ec_boot_resources
{
	ACPI_TABLE_HEADER_DEF
	struct acpi_generic_address     ec_control;         /* Address of EC command/status register */
	struct acpi_generic_address     ec_data;            /* Address of EC data register */
	u32                             uid;                /* Unique ID - must be same as the EC _UID method */
	u8                              gpe_bit;            /* The GPE for the EC */
	u8                              ec_id[1];           /* Full namepath of the EC in the ACPI namespace */
};


#pragma pack()

#endif /* __ACTBL2_H__ */

