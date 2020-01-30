/*
 * arch/ppc/syslib/mpc52xx_common.c
 *
 * Common code for the boards based on Freescale MPC52xx embedded CPU.
 *
 * 
 * Maintainer : Sylvain Munaut <tnt@246tNt.com>
 *
 * Support for other bootloaders than UBoot by Dale Farnsworth 
 * <dfarnsworth@mvista.com>
 * 
 * Copyright (C) 2004 Sylvain Munaut <tnt@246tNt.com>
 * Copyright (C) 2003 Montavista Software, Inc
 * 
 * This file is licensed under the terms of the GNU General Public License
 * version 2. This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#include <linux/config.h>

#include <asm/time.h>
#include <asm/mpc52xx.h>
#include <asm/mpc52xx_psc.h>
#include <asm/ocp.h>
#include <asm/ppcboot.h>

extern bd_t __res;

static int core_mult[] = {		/* CPU Frequency multiplier, taken    */
	0,  0,  0,  10, 20, 20, 25, 45,	/* from the datasheet used to compute */
	30, 55, 40, 50, 0,  60, 35, 0,	/* CPU frequency from XLB freq and    */
	30, 25, 65, 10, 70, 20, 75, 45,	/* external jumper config             */
	0,  55, 40, 50, 80, 60, 35, 0
};

void
mpc52xx_restart(char *cmd)
{
	struct mpc52xx_gpt* gpt0 = (struct mpc52xx_gpt*) MPC52xx_GPTx(0);
	
	local_irq_disable();
	
	/* Turn on the watchdog and wait for it to expire. It effectively
	  does a reset */
	if (gpt0 != NULL) {
		out_be32(&gpt0->count, 0x000000ff);
		out_be32(&gpt0->mode, 0x00009004);
	} else
		printk(KERN_ERR "mpc52xx_restart: Unable to ioremap GPT0 registers, -> looping ...");

	while (1);
}

void
mpc52xx_halt(void)
{
	local_irq_disable();

	while (1);
}

void
mpc52xx_power_off(void)
{
	/* By default we don't have any way of shut down.
	   If a specific board wants to, it can set the power down
	   code to any hardware implementation dependent code */
	mpc52xx_halt();
}


void __init
mpc52xx_set_bat(void)
{
	/* Set BAT 2 to map the 0xf0000000 area */
	/* This mapping is used during mpc52xx_progress,
	 * mpc52xx_find_end_of_memory, and UARTs/GPIO access for debug
	 */
	mb();
	mtspr(DBAT2U, 0xf0001ffe);
	mtspr(DBAT2L, 0xf000002a);
	mb();
}

void __init
mpc52xx_map_io(void)
{
	/* Here we only map the MBAR */
	io_block_mapping(
		MPC52xx_MBAR_VIRT, MPC52xx_MBAR, MPC52xx_MBAR_SIZE, _PAGE_IO);
}


#ifdef CONFIG_SERIAL_TEXT_DEBUG
#ifdef MPC52xx_PF_CONSOLE_PORT
#define MPC52xx_CONSOLE MPC52xx_PSCx(MPC52xx_PF_CONSOLE_PORT)
#else
#error "mpc52xx PSC for console not selected"
#endif

void
mpc52xx_progress(char *s, unsigned short hex)
{
	struct mpc52xx_psc *psc = (struct mpc52xx_psc *)MPC52xx_CONSOLE;
	char c;

		/* Don't we need to disable serial interrupts ? */
	
	while ((c = *s++) != 0) {
		if (c == '\n') {
			while (!(in_be16(&psc->mpc52xx_psc_status) &
			         MPC52xx_PSC_SR_TXRDY)) ;
			out_8(&psc->mpc52xx_psc_buffer_8, '\r');
		}
		while (!(in_be16(&psc->mpc52xx_psc_status) &
		         MPC52xx_PSC_SR_TXRDY)) ;
		out_8(&psc->mpc52xx_psc_buffer_8, c);
	}
}

#endif  /* CONFIG_SERIAL_TEXT_DEBUG */


unsigned long __init
mpc52xx_find_end_of_memory(void)
{
	u32 ramsize = __res.bi_memsize;

	/*
	 * if bootloader passed a memsize, just use it
	 * else get size from sdram config registers
	 */
	if (ramsize == 0) {
		struct mpc52xx_mmap_ctl *mmap_ctl;
		u32 sdram_config_0, sdram_config_1;

		/* Temp BAT2 mapping active when this is called ! */
		mmap_ctl = (struct mpc52xx_mmap_ctl*) MPC52xx_MMAP_CTL;
			
		sdram_config_0 = in_be32(&mmap_ctl->sdram0);
		sdram_config_1 = in_be32(&mmap_ctl->sdram1);

		if ((sdram_config_0 & 0x1f) >= 0x13)
			ramsize = 1 << ((sdram_config_0 & 0xf) + 17);

		if (((sdram_config_1 & 0x1f) >= 0x13) &&
				((sdram_config_1 & 0xfff00000) == ramsize))
			ramsize += 1 << ((sdram_config_1 & 0xf) + 17);

		iounmap(mmap_ctl);
	}
	
	return ramsize;
}

void __init
mpc52xx_calibrate_decr(void)
{
	int current_time, previous_time;
	int tbl_start, tbl_end;
	unsigned int xlbfreq, cpufreq, ipbfreq, pcifreq, divisor;

	xlbfreq = __res.bi_busfreq;
	/* if bootloader didn't pass bus frequencies, calculate them */
	if (xlbfreq == 0) {
		/* Get RTC & Clock manager modules */
		struct mpc52xx_rtc *rtc;
		struct mpc52xx_cdm *cdm;
		
		rtc = (struct mpc52xx_rtc*)
			ioremap(MPC52xx_RTC, sizeof(struct mpc52xx_rtc));
		cdm = (struct mpc52xx_cdm*)
			ioremap(MPC52xx_CDM, sizeof(struct mpc52xx_cdm));

		if ((rtc==NULL) || (cdm==NULL))
			panic("Can't ioremap RTC/CDM while computing bus freq");

		/* Count bus clock during 1/64 sec */
		out_be32(&rtc->dividers, 0x8f1f0000);	/* Set RTC 64x faster */
		previous_time = in_be32(&rtc->time);
		while ((current_time = in_be32(&rtc->time)) == previous_time) ;
		tbl_start = get_tbl();
		previous_time = current_time;
		while ((current_time = in_be32(&rtc->time)) == previous_time) ;
		tbl_end = get_tbl();
		out_be32(&rtc->dividers, 0xffff0000);	/* Restore RTC */

		/* Compute all frequency from that & CDM settings */
		xlbfreq = (tbl_end - tbl_start) << 8;
		cpufreq = (xlbfreq * core_mult[in_be32(&cdm->rstcfg)&0x1f])/10;
		ipbfreq = (in_8(&cdm->ipb_clk_sel) & 1) ?
					xlbfreq / 2 : xlbfreq;
		switch (in_8(&cdm->pci_clk_sel) & 3) {
		case 0:
			pcifreq = ipbfreq;
			break;
		case 1:
			pcifreq = ipbfreq / 2;
			break;
		default:
			pcifreq = xlbfreq / 4;
			break;
		}
		__res.bi_busfreq = xlbfreq;
		__res.bi_intfreq = cpufreq;
		__res.bi_ipbfreq = ipbfreq;
		__res.bi_pcifreq = pcifreq;
	
		/* Release mapping */
		iounmap((void*)rtc);
		iounmap((void*)cdm);
	}

	divisor = 4;

	tb_ticks_per_jiffy = xlbfreq / HZ / divisor;
	tb_to_us = mulhwu_scale_factor(xlbfreq / divisor, 1000000);
}


void __init
mpc52xx_add_board_devices(struct ocp_def board_ocp[]) {
	while (board_ocp->vendor != OCP_VENDOR_INVALID)
		if(ocp_add_one_device(board_ocp++))
			printk("mpc5200-ocp: Failed to add board device !\n");
}

