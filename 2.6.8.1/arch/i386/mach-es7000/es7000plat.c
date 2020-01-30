/*
 * Written by: Garry Forsgren, Unisys Corporation
 *             Natalie Protasevich, Unisys Corporation
 * This file contains the code to configure and interface
 * with Unisys ES7000 series hardware system manager.
 *
 * Copyright (c) 2003 Unisys Corporation.  All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston MA 02111-1307, USA.
 *
 * Contact information: Unisys Corporation, Township Line & Union Meeting
 * Roads-A, Unisys Way, Blue Bell, Pennsylvania, 19424, or:
 *
 * http://www.unisys.com
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/init.h>
#include <linux/acpi.h>
#include <asm/io.h>
#include <asm/nmi.h>
#include <asm/smp.h>
#include <asm/apicdef.h>
#include "es7000.h"

/*
 * ES7000 Globals
 */

volatile unsigned long	*psai = NULL;
struct mip_reg		*mip_reg;
struct mip_reg		*host_reg;
int 			mip_port;
unsigned long		mip_addr, host_addr;

static int __init
es7000_rename_gsi(int ioapic, int gsi)
{
	if (ioapic)
		return gsi;
	else {
		if (gsi == 0)
			return 13;
		if (gsi == 1)
			return 16;
		if (gsi == 4)
			return 17;
		if (gsi == 6)
			return 18;
		if (gsi == 7)
			return 19;
		if (gsi == 8)
			return 20;
		return gsi;
        }
}

/*
 * Parse the OEM Table
 */

int __init
parse_unisys_oem (char *oemptr, int oem_entries)
{
	int                     i;
	int 			success = 0;
	unsigned char           type, size;
	unsigned long           val;
	char                    *tp = NULL;
	struct psai             *psaip = NULL;
	struct mip_reg_info 	*mi;
	struct mip_reg		*host, *mip;

	tp = oemptr;

	tp += 8;

	for (i=0; i <= oem_entries; i++) {
		type = *tp++;
		size = *tp++;
		tp -= 2;
		switch (type) {
		case MIP_REG:
			mi = (struct mip_reg_info *)tp;
			val = MIP_RD_LO(mi->host_reg);
			host_addr = val;
			host = (struct mip_reg *)val;
			host_reg = __va(host);
			val = MIP_RD_LO(mi->mip_reg);
			mip_port = MIP_PORT(mi->mip_info);
			mip_addr = val;
			mip = (struct mip_reg *)val;
			mip_reg = __va(mip);
			Dprintk("es7000_mipcfg: host_reg = 0x%lx \n",
				(unsigned long)host_reg);
			Dprintk("es7000_mipcfg: mip_reg = 0x%lx \n",
				(unsigned long)mip_reg);
			success++;
			break;
		case MIP_PSAI_REG:
			psaip = (struct psai *)tp;
			if (tp != NULL) {
				if (psaip->addr)
					psai = __va(psaip->addr);
				else
					psai = NULL;
				success++;
			}
			break;
		default:
			break;
		}
		if (i == 6) break;
		tp += size;
	}

	if (success < 2) {
		es7000_plat = 0;
	} else {
		printk("\nEnabling ES7000 specific features...\n");
		es7000_plat = 1;
		platform_rename_gsi = es7000_rename_gsi;
	}
	return es7000_plat;
}

int __init
find_unisys_acpi_oem_table(unsigned long *oem_addr, int *length)
{
	struct acpi_table_rsdp		*rsdp = NULL;
	unsigned long			rsdp_phys = 0;
	struct acpi_table_header 	*header = NULL;
	int				i;
	struct acpi_table_sdt		sdt;

	rsdp_phys = acpi_find_rsdp();
	rsdp = __va(rsdp_phys);
	if (rsdp->rsdt_address) {
		struct acpi_table_rsdt	*mapped_rsdt = NULL;
		sdt.pa = rsdp->rsdt_address;

		header = (struct acpi_table_header *)
			__acpi_map_table(sdt.pa, sizeof(struct acpi_table_header));
		if (!header)
			return -ENODEV;

		sdt.count = (header->length - sizeof(struct acpi_table_header)) >> 3;
		mapped_rsdt = (struct acpi_table_rsdt *)
			__acpi_map_table(sdt.pa, header->length);
		if (!mapped_rsdt)
			return -ENODEV;

		header = &mapped_rsdt->header;

		for (i = 0; i < sdt.count; i++)
			sdt.entry[i].pa = (unsigned long) mapped_rsdt->entry[i];
	};
	for (i = 0; i < sdt.count; i++) {

		header = (struct acpi_table_header *)
			__acpi_map_table(sdt.entry[i].pa,
				sizeof(struct acpi_table_header));
		if (!header)
			continue;
		if (!strncmp((char *) &header->signature, "OEM1", 4)) {
			if (!strncmp((char *) &header->oem_id, "UNISYS", 6)) {
				void *addr;
				struct oem_table *t;
				acpi_table_print(header, sdt.entry[i].pa);
				t = (struct oem_table *) __acpi_map_table(sdt.entry[i].pa, header->length);
				addr = (void *) __acpi_map_table(t->OEMTableAddr, t->OEMTableSize);
				*length = header->length;
				*oem_addr = (unsigned long) addr;
				return 0;
			}
		}
	}
	printk("ES7000: did not find Unisys ACPI OEM table!\n");
	return -1;
}

static void
es7000_spin(int n)
{
	int i = 0;

	while (i++ < n)
		rep_nop();
}

static int __init
es7000_mip_write(struct mip_reg *mip_reg)
{
	int			status = 0;
	int			spin;

	spin = MIP_SPIN;
	while (((unsigned long long)host_reg->off_38 &
		(unsigned long long)MIP_VALID) != 0) {
			if (--spin <= 0) {
				printk("es7000_mip_write: Timeout waiting for Host Valid Flag");
				return -1;
			}
		es7000_spin(MIP_SPIN);
	}

	memcpy(host_reg, mip_reg, sizeof(struct mip_reg));
	outb(1, mip_port);

	spin = MIP_SPIN;

	while (((unsigned long long)mip_reg->off_38 &
		(unsigned long long)MIP_VALID) == 0) {
		if (--spin <= 0) {
			printk("es7000_mip_write: Timeout waiting for MIP Valid Flag");
			return -1;
		}
		es7000_spin(MIP_SPIN);
	}

	status = ((unsigned long long)mip_reg->off_0 &
		(unsigned long long)0xffff0000000000) >> 48;
	mip_reg->off_38 = ((unsigned long long)mip_reg->off_38 &
		(unsigned long long)~MIP_VALID);
	return status;
}

int
es7000_start_cpu(int cpu, unsigned long eip)
{
	unsigned long vect = 0, psaival = 0;

	if (psai == NULL)
		return -1;

	vect = ((unsigned long)__pa(eip)/0x1000) << 16;
	psaival = (0x1000000 | vect | cpu);

	while (*psai & 0x1000000)
                ;

	*psai = psaival;

	return 0;

}

int
es7000_stop_cpu(int cpu)
{
	int startup;

	if (psai == NULL)
		return -1;

	startup= (0x1000000 | cpu);

	while ((*psai & 0xff00ffff) != startup)
		;

	startup = (*psai & 0xff0000) >> 16;
	*psai &= 0xffffff;

	return 0;

}

void __init
es7000_sw_apic()
{
	if (es7000_plat) {
		int mip_status;
		struct mip_reg es7000_mip_reg;

		printk("ES7000: Enabling APIC mode.\n");
        	memset(&es7000_mip_reg, 0, sizeof(struct mip_reg));
        	es7000_mip_reg.off_0 = MIP_SW_APIC;
        	es7000_mip_reg.off_38 = (MIP_VALID);
        	while ((mip_status = es7000_mip_write(&es7000_mip_reg)) != 0)
              		printk("es7000_sw_apic: command failed, status = %x\n",
				mip_status);
		return;
	}
}
