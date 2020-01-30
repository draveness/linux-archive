/*
 * include/asm-mips/vr41xx/vr41xx.h
 *
 * Include file for NEC VR4100 series.
 *
 * Copyright (C) 1999 Michael Klar
 * Copyright (C) 2001, 2002 Paul Mundt
 * Copyright (C) 2002 MontaVista Software, Inc.
 * Copyright (C) 2002 TimeSys Corp.
 * Copyright (C) 2003-2004 Yoichi Yuasa <yuasa@hh.iij4u.or.jp>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */
#ifndef __NEC_VR41XX_H
#define __NEC_VR41XX_H

#include <linux/interrupt.h>

/*
 * CPU Revision
 */
/* VR4122 0x00000c70-0x00000c72 */
#define PRID_VR4122_REV1_0	0x00000c70
#define PRID_VR4122_REV2_0	0x00000c70
#define PRID_VR4122_REV2_1	0x00000c70
#define PRID_VR4122_REV3_0	0x00000c71
#define PRID_VR4122_REV3_1	0x00000c72

/* VR4181A 0x00000c73-0x00000c7f */
#define PRID_VR4181A_REV1_0	0x00000c73
#define PRID_VR4181A_REV1_1	0x00000c74

/* VR4131 0x00000c80-0x00000c83 */
#define PRID_VR4131_REV1_2	0x00000c80
#define PRID_VR4131_REV2_0	0x00000c81
#define PRID_VR4131_REV2_1	0x00000c82
#define PRID_VR4131_REV2_2	0x00000c83

/* VR4133 0x00000c84- */
#define PRID_VR4133		0x00000c84

/*
 * Bus Control Uint
 */
extern unsigned long vr41xx_get_vtclock_frequency(void);
extern unsigned long vr41xx_get_tclock_frequency(void);

/*
 * Clock Mask Unit
 */
typedef enum {
	PIU_CLOCK,
	SIU_CLOCK,
	AIU_CLOCK,
	KIU_CLOCK,
	FIR_CLOCK,
	DSIU_CLOCK,
	CSI_CLOCK,
	PCIU_CLOCK,
	HSP_CLOCK,
	PCI_CLOCK,
	CEU_CLOCK,
	ETHER0_CLOCK,
	ETHER1_CLOCK
} vr41xx_clock_t;

extern void vr41xx_supply_clock(vr41xx_clock_t clock);
extern void vr41xx_mask_clock(vr41xx_clock_t clock);

/*
 * Interrupt Control Unit
 */
/* CPU core Interrupt Numbers */
#define MIPS_CPU_IRQ_BASE	0
#define MIPS_CPU_IRQ(x)		(MIPS_CPU_IRQ_BASE + (x))
#define MIPS_SOFTINT0_IRQ	MIPS_CPU_IRQ(0)
#define MIPS_SOFTINT1_IRQ	MIPS_CPU_IRQ(1)
#define INT0_CASCADE_IRQ	MIPS_CPU_IRQ(2)
#define INT1_CASCADE_IRQ	MIPS_CPU_IRQ(3)
#define INT2_CASCADE_IRQ	MIPS_CPU_IRQ(4)
#define INT3_CASCADE_IRQ	MIPS_CPU_IRQ(5)
#define INT4_CASCADE_IRQ	MIPS_CPU_IRQ(6)
#define MIPS_COUNTER_IRQ	MIPS_CPU_IRQ(7)

/* SYINT1 Interrupt Numbers */
#define SYSINT1_IRQ_BASE	8
#define SYSINT1_IRQ(x)		(SYSINT1_IRQ_BASE + (x))
#define BATTRY_IRQ		SYSINT1_IRQ(0)
#define POWER_IRQ		SYSINT1_IRQ(1)
#define RTCLONG1_IRQ		SYSINT1_IRQ(2)
#define ELAPSEDTIME_IRQ		SYSINT1_IRQ(3)
/* RFU */
#define PIU_IRQ			SYSINT1_IRQ(5)
#define AIU_IRQ			SYSINT1_IRQ(6)
#define KIU_IRQ			SYSINT1_IRQ(7)
#define GIUINT_CASCADE_IRQ	SYSINT1_IRQ(8)
#define SIU_IRQ			SYSINT1_IRQ(9)
#define BUSERR_IRQ		SYSINT1_IRQ(10)
#define SOFTINT_IRQ		SYSINT1_IRQ(11)
#define CLKRUN_IRQ		SYSINT1_IRQ(12)
#define DOZEPIU_IRQ		SYSINT1_IRQ(13)
#define SYSINT1_IRQ_LAST	DOZEPIU_IRQ

/* SYSINT2 Interrupt Numbers */
#define SYSINT2_IRQ_BASE	24
#define SYSINT2_IRQ(x)		(SYSINT2_IRQ_BASE + (x))
#define RTCLONG2_IRQ		SYSINT2_IRQ(0)
#define LED_IRQ			SYSINT2_IRQ(1)
#define HSP_IRQ			SYSINT2_IRQ(2)
#define TCLOCK_IRQ		SYSINT2_IRQ(3)
#define FIR_IRQ			SYSINT2_IRQ(4)
#define CEU_IRQ			SYSINT2_IRQ(4)	/* same number as FIR_IRQ */
#define DSIU_IRQ		SYSINT2_IRQ(5)
#define PCI_IRQ			SYSINT2_IRQ(6)
#define SCU_IRQ			SYSINT2_IRQ(7)
#define CSI_IRQ			SYSINT2_IRQ(8)
#define BCU_IRQ			SYSINT2_IRQ(9)
#define ETHERNET_IRQ		SYSINT2_IRQ(10)
#define SYSINT2_IRQ_LAST	ETHERNET_IRQ

/* GIU Interrupt Numbers */
#define GIU_IRQ_BASE		40
#define GIU_IRQ(x)		(GIU_IRQ_BASE + (x))	/* IRQ 40-71 */
#define GIU_IRQ_LAST		GIU_IRQ(31)
#define GIU_IRQ_TO_PIN(x)	((x) - GIU_IRQ_BASE)	/* Pin 0-31 */

extern int vr41xx_set_intassign(unsigned int irq, unsigned char intassign);
extern int vr41xx_cascade_irq(unsigned int irq, int (*get_irq_number)(int irq));

#define PIUINT_COMMAND		0x0040
#define PIUINT_DATA		0x0020
#define PIUINT_PAGE1		0x0010
#define PIUINT_PAGE0		0x0008
#define PIUINT_DATALOST		0x0004
#define PIUINT_STATUSCHANGE	0x0001

extern void vr41xx_enable_piuint(uint16_t mask);
extern void vr41xx_disable_piuint(uint16_t mask);

#define AIUINT_INPUT_DMAEND	0x0800
#define AIUINT_INPUT_DMAHALT	0x0400
#define AIUINT_INPUT_DATALOST	0x0200
#define AIUINT_INPUT_DATA	0x0100
#define AIUINT_OUTPUT_DMAEND	0x0008
#define AIUINT_OUTPUT_DMAHALT	0x0004
#define AIUINT_OUTPUT_NODATA	0x0002

extern void vr41xx_enable_aiuint(uint16_t mask);
extern void vr41xx_disable_aiuint(uint16_t mask);

#define KIUINT_DATALOST		0x0004
#define KIUINT_DATAREADY	0x0002
#define KIUINT_SCAN		0x0001

extern void vr41xx_enable_kiuint(uint16_t mask);
extern void vr41xx_disable_kiuint(uint16_t mask);

#define DSIUINT_CTS		0x0800
#define DSIUINT_RXERR		0x0400
#define DSIUINT_RX		0x0200
#define DSIUINT_TX		0x0100
#define DSIUINT_ALL		0x0f00

extern void vr41xx_enable_dsiuint(uint16_t mask);
extern void vr41xx_disable_dsiuint(uint16_t mask);

#define FIRINT_UNIT		0x0010
#define FIRINT_RX_DMAEND	0x0008
#define FIRINT_RX_DMAHALT	0x0004
#define FIRINT_TX_DMAEND	0x0002
#define FIRINT_TX_DMAHALT	0x0001

extern void vr41xx_enable_firint(uint16_t mask);
extern void vr41xx_disable_firint(uint16_t mask);

extern void vr41xx_enable_pciint(void);
extern void vr41xx_disable_pciint(void);

extern void vr41xx_enable_scuint(void);
extern void vr41xx_disable_scuint(void);

#define CSIINT_TX_DMAEND	0x0040
#define CSIINT_TX_DMAHALT	0x0020
#define CSIINT_TX_DATA		0x0010
#define CSIINT_TX_FIFOEMPTY	0x0008
#define CSIINT_RX_DMAEND	0x0004
#define CSIINT_RX_DMAHALT	0x0002
#define CSIINT_RX_FIFOEMPTY	0x0001

extern void vr41xx_enable_csiint(uint16_t mask);
extern void vr41xx_disable_csiint(uint16_t mask);

extern void vr41xx_enable_bcuint(void);
extern void vr41xx_disable_bcuint(void);

/*
 * Power Management Unit
 */

/*
 * RTC
 */
extern void vr41xx_set_rtclong1_cycle(uint32_t cycles);
extern uint32_t vr41xx_read_rtclong1_counter(void);

extern void vr41xx_set_rtclong2_cycle(uint32_t cycles);
extern uint32_t vr41xx_read_rtclong2_counter(void);

extern void vr41xx_set_tclock_cycle(uint32_t cycles);
extern uint32_t vr41xx_read_tclock_counter(void);

/*
 * General-Purpose I/O Unit
 */
enum {
	TRIGGER_LEVEL,
	TRIGGER_EDGE,
	TRIGGER_EDGE_FALLING,
	TRIGGER_EDGE_RISING
};

enum {
	SIGNAL_THROUGH,
	SIGNAL_HOLD
};

extern void vr41xx_set_irq_trigger(int pin, int trigger, int hold);

enum {
	LEVEL_LOW,
	LEVEL_HIGH
};

extern void vr41xx_set_irq_level(int pin, int level);

enum {
	PIO_INPUT,
	PIO_OUTPUT
};

enum {
	DATA_LOW,
	DATA_HIGH
};

/*
 * Serial Interface Unit
 */
extern void vr41xx_siu_init(void);
extern int vr41xx_serial_ports;

/* SIU interfaces */
typedef enum {
	SIU_RS232C,
	SIU_IRDA
} siu_interface_t;

/* IrDA interfaces */
typedef enum {
	IRDA_NONE,
	IRDA_SHARP,
	IRDA_TEMIC,
	IRDA_HP
} irda_module_t;

extern void vr41xx_select_siu_interface(siu_interface_t interface,
                                        irda_module_t module);

/*
 * Debug Serial Interface Unit
 */
extern void vr41xx_dsiu_init(void);

/*
 * PCI Control Unit
 */
#define PCI_MASTER_ADDRESS_MASK	0x7fffffffU

struct pci_master_address_conversion {
	uint32_t bus_base_address;
	uint32_t address_mask;
	uint32_t pci_base_address;
};

struct pci_target_address_conversion {
	uint32_t address_mask;
	uint32_t bus_base_address;
};

typedef enum {
	CANNOT_LOCK_FROM_DEVICE,
	CAN_LOCK_FROM_DEVICE,
} pci_exclusive_access_t;

struct pci_mailbox_address {
	uint32_t base_address;
};

struct pci_target_address_window {
	uint32_t base_address;
};

typedef enum {
	PCI_ARBITRATION_MODE_FAIR,
	PCI_ARBITRATION_MODE_ALTERNATE_0,
	PCI_ARBITRATION_MODE_ALTERNATE_B,
} pci_arbiter_priority_control_t;

typedef enum {
	PCI_TAKE_AWAY_GNT_DISABLE,
	PCI_TAKE_AWAY_GNT_ENABLE,
} pci_take_away_gnt_mode_t;

struct pci_controller_unit_setup {
	struct pci_master_address_conversion *master_memory1;
	struct pci_master_address_conversion *master_memory2;

	struct pci_target_address_conversion *target_memory1;
	struct pci_target_address_conversion *target_memory2;

	struct pci_master_address_conversion *master_io;

	pci_exclusive_access_t exclusive_access;

	uint32_t pci_clock_max;
	uint8_t wait_time_limit_from_irdy_to_trdy;	/* Only VR4122 is supported */

	struct pci_mailbox_address *mailbox;
	struct pci_target_address_window *target_window1;
	struct pci_target_address_window *target_window2;

	uint8_t master_latency_timer;
	uint8_t retry_limit;

	pci_arbiter_priority_control_t arbiter_priority_control;
	pci_take_away_gnt_mode_t take_away_gnt_mode;

	struct resource *mem_resource;
	struct resource *io_resource;
};

extern void vr41xx_pciu_setup(struct pci_controller_unit_setup *setup);

#endif /* __NEC_VR41XX_H */
