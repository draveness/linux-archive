/*arch/powerpc/platforms/8xx/mpc885ads_setup.c
 *
 * Platform setup for the Freescale mpc885ads board
 *
 * Vitaly Bordug <vbordug@ru.mvista.com>
 *
 * Copyright 2005 MontaVista Software Inc.
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2. This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/param.h>
#include <linux/string.h>
#include <linux/ioport.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/root_dev.h>

#include <linux/fs_enet_pd.h>
#include <linux/fs_uart_pd.h>
#include <linux/fsl_devices.h>
#include <linux/mii.h>

#include <asm/delay.h>
#include <asm/io.h>
#include <asm/machdep.h>
#include <asm/page.h>
#include <asm/processor.h>
#include <asm/system.h>
#include <asm/time.h>
#include <asm/ppcboot.h>
#include <asm/mpc8xx.h>
#include <asm/8xx_immap.h>
#include <asm/commproc.h>
#include <asm/fs_pd.h>
#include <asm/prom.h>

extern void cpm_reset(void);
extern void mpc8xx_show_cpuinfo(struct seq_file *);
extern void mpc8xx_restart(char *cmd);
extern void mpc8xx_calibrate_decr(void);
extern int mpc8xx_set_rtc_time(struct rtc_time *tm);
extern void mpc8xx_get_rtc_time(struct rtc_time *tm);
extern void m8xx_pic_init(void);
extern unsigned int mpc8xx_get_irq(void);

static void init_smc1_uart_ioports(struct fs_uart_platform_info *fpi);
static void init_smc2_uart_ioports(struct fs_uart_platform_info *fpi);
static void init_scc3_ioports(struct fs_platform_info *ptr);

#ifdef CONFIG_PCMCIA_M8XX
static void pcmcia_hw_setup(int slot, int enable)
{
	unsigned *bcsr_io;

	bcsr_io = ioremap(BCSR1, sizeof(unsigned long));
	if (enable)
		clrbits32(bcsr_io, BCSR1_PCCEN);
	else
		setbits32(bcsr_io, BCSR1_PCCEN);

	iounmap(bcsr_io);
}

static int pcmcia_set_voltage(int slot, int vcc, int vpp)
{
	u32 reg = 0;
	unsigned *bcsr_io;

	bcsr_io = ioremap(BCSR1, sizeof(unsigned long));

	switch (vcc) {
	case 0:
		break;
	case 33:
		reg |= BCSR1_PCCVCC0;
		break;
	case 50:
		reg |= BCSR1_PCCVCC1;
		break;
	default:
		return 1;
	}

	switch (vpp) {
	case 0:
		break;
	case 33:
	case 50:
		if (vcc == vpp)
			reg |= BCSR1_PCCVPP1;
		else
			return 1;
		break;
	case 120:
		if ((vcc == 33) || (vcc == 50))
			reg |= BCSR1_PCCVPP0;
		else
			return 1;
	default:
		return 1;
	}

	/* first, turn off all power */
	clrbits32(bcsr_io, 0x00610000);

	/* enable new powersettings */
	setbits32(bcsr_io, reg);

	iounmap(bcsr_io);
	return 0;
}
#endif

void __init mpc885ads_board_setup(void)
{
	cpm8xx_t *cp;
	unsigned int *bcsr_io;
	u8 tmpval8;

#ifdef CONFIG_FS_ENET
	iop8xx_t *io_port;
#endif

	bcsr_io = ioremap(BCSR1, sizeof(unsigned long));
	cp = (cpm8xx_t *) immr_map(im_cpm);

	if (bcsr_io == NULL) {
		printk(KERN_CRIT "Could not remap BCSR\n");
		return;
	}
#ifdef CONFIG_SERIAL_CPM_SMC1
	clrbits32(bcsr_io, BCSR1_RS232EN_1);
	clrbits32(&cp->cp_simode, 0xe0000000 >> 17);	/* brg1 */
	tmpval8 = in_8(&(cp->cp_smc[0].smc_smcm)) | (SMCM_RX | SMCM_TX);
	out_8(&(cp->cp_smc[0].smc_smcm), tmpval8);
	clrbits16(&cp->cp_smc[0].smc_smcmr, SMCMR_REN | SMCMR_TEN);	/* brg1 */
#else
	setbits32(bcsr_io, BCSR1_RS232EN_1);
	out_be16(&cp->cp_smc[0].smc_smcmr, 0);
	out_8(&cp->cp_smc[0].smc_smce, 0);
#endif

#ifdef CONFIG_SERIAL_CPM_SMC2
	clrbits32(bcsr_io, BCSR1_RS232EN_2);
	clrbits32(&cp->cp_simode, 0xe0000000 >> 1);
	setbits32(&cp->cp_simode, 0x20000000 >> 1);	/* brg2 */
	tmpval8 = in_8(&(cp->cp_smc[1].smc_smcm)) | (SMCM_RX | SMCM_TX);
	out_8(&(cp->cp_smc[1].smc_smcm), tmpval8);
	clrbits16(&cp->cp_smc[1].smc_smcmr, SMCMR_REN | SMCMR_TEN);

	init_smc2_uart_ioports(0);
#else
	setbits32(bcsr_io, BCSR1_RS232EN_2);
	out_be16(&cp->cp_smc[1].smc_smcmr, 0);
	out_8(&cp->cp_smc[1].smc_smce, 0);
#endif
	immr_unmap(cp);
	iounmap(bcsr_io);

#ifdef CONFIG_FS_ENET
	/* use MDC for MII (common) */
	io_port = (iop8xx_t *) immr_map(im_ioport);
	setbits16(&io_port->iop_pdpar, 0x0080);
	clrbits16(&io_port->iop_pddir, 0x0080);

	bcsr_io = ioremap(BCSR5, sizeof(unsigned long));
	clrbits32(bcsr_io, BCSR5_MII1_EN);
	clrbits32(bcsr_io, BCSR5_MII1_RST);
#ifndef CONFIG_FC_ENET_HAS_SCC
	clrbits32(bcsr_io, BCSR5_MII2_EN);
	clrbits32(bcsr_io, BCSR5_MII2_RST);

#endif
	iounmap(bcsr_io);
	immr_unmap(io_port);

#endif

#ifdef CONFIG_PCMCIA_M8XX
	/*Set up board specific hook-ups */
	m8xx_pcmcia_ops.hw_ctrl = pcmcia_hw_setup;
	m8xx_pcmcia_ops.voltage_set = pcmcia_set_voltage;
#endif
}

static void init_fec1_ioports(struct fs_platform_info *ptr)
{
	cpm8xx_t *cp = (cpm8xx_t *) immr_map(im_cpm);
	iop8xx_t *io_port = (iop8xx_t *) immr_map(im_ioport);

	/* configure FEC1 pins  */
	setbits16(&io_port->iop_papar, 0xf830);
	setbits16(&io_port->iop_padir, 0x0830);
	clrbits16(&io_port->iop_padir, 0xf000);

	setbits32(&cp->cp_pbpar, 0x00001001);
	clrbits32(&cp->cp_pbdir, 0x00001001);

	setbits16(&io_port->iop_pcpar, 0x000c);
	clrbits16(&io_port->iop_pcdir, 0x000c);

	setbits32(&cp->cp_pepar, 0x00000003);
	setbits32(&cp->cp_pedir, 0x00000003);
	clrbits32(&cp->cp_peso, 0x00000003);
	clrbits32(&cp->cp_cptr, 0x00000100);

	immr_unmap(io_port);
	immr_unmap(cp);
}

static void init_fec2_ioports(struct fs_platform_info *ptr)
{
	cpm8xx_t *cp = (cpm8xx_t *) immr_map(im_cpm);
	iop8xx_t *io_port = (iop8xx_t *) immr_map(im_ioport);

	/* configure FEC2 pins */
	setbits32(&cp->cp_pepar, 0x0003fffc);
	setbits32(&cp->cp_pedir, 0x0003fffc);
	clrbits32(&cp->cp_peso, 0x000087fc);
	setbits32(&cp->cp_peso, 0x00037800);
	clrbits32(&cp->cp_cptr, 0x00000080);

	immr_unmap(io_port);
	immr_unmap(cp);
}

void init_fec_ioports(struct fs_platform_info *fpi)
{
	int fec_no = fs_get_fec_index(fpi->fs_no);

	switch (fec_no) {
	case 0:
		init_fec1_ioports(fpi);
		break;
	case 1:
		init_fec2_ioports(fpi);
		break;
	default:
		printk(KERN_ERR "init_fec_ioports: invalid FEC number\n");
		return;
	}
}

static void init_scc3_ioports(struct fs_platform_info *fpi)
{
	unsigned *bcsr_io;
	iop8xx_t *io_port;
	cpm8xx_t *cp;

	bcsr_io = ioremap(BCSR_ADDR, BCSR_SIZE);
	io_port = (iop8xx_t *) immr_map(im_ioport);
	cp = (cpm8xx_t *) immr_map(im_cpm);

	if (bcsr_io == NULL) {
		printk(KERN_CRIT "Could not remap BCSR\n");
		return;
	}

	/* Enable the PHY.
	 */
	clrbits32(bcsr_io + 4, BCSR4_ETH10_RST);
	udelay(1000);
	setbits32(bcsr_io + 4, BCSR4_ETH10_RST);
	/* Configure port A pins for Txd and Rxd.
	 */
	setbits16(&io_port->iop_papar, PA_ENET_RXD | PA_ENET_TXD);
	clrbits16(&io_port->iop_padir, PA_ENET_RXD | PA_ENET_TXD);

	/* Configure port C pins to enable CLSN and RENA.
	 */
	clrbits16(&io_port->iop_pcpar, PC_ENET_CLSN | PC_ENET_RENA);
	clrbits16(&io_port->iop_pcdir, PC_ENET_CLSN | PC_ENET_RENA);
	setbits16(&io_port->iop_pcso, PC_ENET_CLSN | PC_ENET_RENA);

	/* Configure port E for TCLK and RCLK.
	 */
	setbits32(&cp->cp_pepar, PE_ENET_TCLK | PE_ENET_RCLK);
	clrbits32(&cp->cp_pepar, PE_ENET_TENA);
	clrbits32(&cp->cp_pedir, PE_ENET_TCLK | PE_ENET_RCLK | PE_ENET_TENA);
	clrbits32(&cp->cp_peso, PE_ENET_TCLK | PE_ENET_RCLK);
	setbits32(&cp->cp_peso, PE_ENET_TENA);

	/* Configure Serial Interface clock routing.
	 * First, clear all SCC bits to zero, then set the ones we want.
	 */
	clrbits32(&cp->cp_sicr, SICR_ENET_MASK);
	setbits32(&cp->cp_sicr, SICR_ENET_CLKRT);

	/* Disable Rx and Tx. SMC1 sshould be stopped if SCC3 eternet are used.
	 */
	clrbits16(&cp->cp_smc[0].smc_smcmr, SMCMR_REN | SMCMR_TEN);
	/* On the MPC885ADS SCC ethernet PHY is initialized in the full duplex mode
	 * by H/W setting after reset. SCC ethernet controller support only half duplex.
	 * This discrepancy of modes causes a lot of carrier lost errors.
	 */

	/* In the original SCC enet driver the following code is placed at
	   the end of the initialization */
	setbits32(&cp->cp_pepar, PE_ENET_TENA);
	clrbits32(&cp->cp_pedir, PE_ENET_TENA);
	setbits32(&cp->cp_peso, PE_ENET_TENA);

	setbits32(bcsr_io + 4, BCSR1_ETHEN);
	iounmap(bcsr_io);
	immr_unmap(io_port);
	immr_unmap(cp);
}

void init_scc_ioports(struct fs_platform_info *fpi)
{
	int scc_no = fs_get_scc_index(fpi->fs_no);

	switch (scc_no) {
	case 2:
		init_scc3_ioports(fpi);
		break;
	default:
		printk(KERN_ERR "init_scc_ioports: invalid SCC number\n");
		return;
	}
}

static void init_smc1_uart_ioports(struct fs_uart_platform_info *ptr)
{
	unsigned *bcsr_io;
	cpm8xx_t *cp;

	cp = (cpm8xx_t *) immr_map(im_cpm);
	setbits32(&cp->cp_pepar, 0x000000c0);
	clrbits32(&cp->cp_pedir, 0x000000c0);
	clrbits32(&cp->cp_peso, 0x00000040);
	setbits32(&cp->cp_peso, 0x00000080);
	immr_unmap(cp);

	bcsr_io = ioremap(BCSR1, sizeof(unsigned long));

	if (bcsr_io == NULL) {
		printk(KERN_CRIT "Could not remap BCSR1\n");
		return;
	}
	clrbits32(bcsr_io, BCSR1_RS232EN_1);
	iounmap(bcsr_io);
}

static void init_smc2_uart_ioports(struct fs_uart_platform_info *fpi)
{
	unsigned *bcsr_io;
	cpm8xx_t *cp;

	cp = (cpm8xx_t *) immr_map(im_cpm);
	setbits32(&cp->cp_pepar, 0x00000c00);
	clrbits32(&cp->cp_pedir, 0x00000c00);
	clrbits32(&cp->cp_peso, 0x00000400);
	setbits32(&cp->cp_peso, 0x00000800);
	immr_unmap(cp);

	bcsr_io = ioremap(BCSR1, sizeof(unsigned long));

	if (bcsr_io == NULL) {
		printk(KERN_CRIT "Could not remap BCSR1\n");
		return;
	}
	clrbits32(bcsr_io, BCSR1_RS232EN_2);
	iounmap(bcsr_io);
}

void init_smc_ioports(struct fs_uart_platform_info *data)
{
	int smc_no = fs_uart_id_fsid2smc(data->fs_no);

	switch (smc_no) {
	case 0:
		init_smc1_uart_ioports(data);
		data->brg = data->clk_rx;
		break;
	case 1:
		init_smc2_uart_ioports(data);
		data->brg = data->clk_rx;
		break;
	default:
		printk(KERN_ERR "init_scc_ioports: invalid SCC number\n");
		return;
	}
}

int platform_device_skip(const char *model, int id)
{
#ifdef CONFIG_MPC8xx_SECOND_ETH_SCC3
	const char *dev = "FEC";
	int n = 2;
#else
	const char *dev = "SCC";
	int n = 3;
#endif

	if (!strcmp(model, dev) && n == id)
		return 1;

	return 0;
}

static void __init mpc885ads_setup_arch(void)
{
	struct device_node *cpu;

	cpu = of_find_node_by_type(NULL, "cpu");
	if (cpu != 0) {
		const unsigned int *fp;

		fp = of_get_property(cpu, "clock-frequency", NULL);
		if (fp != 0)
			loops_per_jiffy = *fp / HZ;
		else
			loops_per_jiffy = 50000000 / HZ;
		of_node_put(cpu);
	}

	cpm_reset();

	mpc885ads_board_setup();

	ROOT_DEV = Root_NFS;
}

static int __init mpc885ads_probe(void)
{
	char *model = of_get_flat_dt_prop(of_get_flat_dt_root(),
					  "model", NULL);
	if (model == NULL)
		return 0;
	if (strcmp(model, "MPC885ADS"))
		return 0;

	return 1;
}

define_machine(mpc885_ads)
{
.name = "MPC885 ADS",.probe = mpc885ads_probe,.setup_arch =
	    mpc885ads_setup_arch,.init_IRQ =
	    m8xx_pic_init,.show_cpuinfo = mpc8xx_show_cpuinfo,.get_irq =
	    mpc8xx_get_irq,.restart = mpc8xx_restart,.calibrate_decr =
	    mpc8xx_calibrate_decr,.set_rtc_time =
	    mpc8xx_set_rtc_time,.get_rtc_time = mpc8xx_get_rtc_time,};
