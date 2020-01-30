/*
 * Device driver for the SYMBIOS/LSILOGIC 53C8XX and 53C1010 family 
 * of PCI-SCSI IO processors.
 *
 * Copyright (C) 1999-2001  Gerard Roudier <groudier@free.fr>
 *
 * This driver is derived from the Linux sym53c8xx driver.
 * Copyright (C) 1998-2000  Gerard Roudier
 *
 * The sym53c8xx driver is derived from the ncr53c8xx driver that had been 
 * a port of the FreeBSD ncr driver to Linux-1.2.13.
 *
 * The original ncr driver has been written for 386bsd and FreeBSD by
 *         Wolfgang Stanglmeier        <wolf@cologne.de>
 *         Stefan Esser                <se@mi.Uni-Koeln.de>
 * Copyright (C) 1994  Wolfgang Stanglmeier
 *
 * Other major contributions:
 *
 * NVRAM detection and reading.
 * Copyright (C) 1997 Richard Waltham <dormouse@farsrobt.demon.co.uk>
 *
 *-----------------------------------------------------------------------------
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * Where this Software is combined with software released under the terms of 
 * the GNU Public License ("GPL") and the terms of the GPL would require the 
 * combined work to also be released under the terms of the GPL, the terms
 * and conditions of this License will apply in addition to those of the
 * GPL with the exception of any terms or conditions of this License that
 * conflict with, or are expressly prohibited by, the GPL.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define SYM_VERSION "2.1.18j"
#define SYM_DRIVER_NAME	"sym-" SYM_VERSION

#include "sym_glue.h"
#include "sym_nvram.h"

#if 0
#define SYM_DEBUG_GENERIC_SUPPORT
#endif

/*
 *  Needed function prototypes.
 */
static void sym_int_ma (hcb_p np);
static void sym_int_sir (hcb_p np);
static ccb_p sym_alloc_ccb(hcb_p np);
static ccb_p sym_ccb_from_dsa(hcb_p np, u32 dsa);
static void sym_alloc_lcb_tags (hcb_p np, u_char tn, u_char ln);
static void sym_complete_error (hcb_p np, ccb_p cp);
static void sym_complete_ok (hcb_p np, ccb_p cp);
static int sym_compute_residual(hcb_p np, ccb_p cp);

/*
 *  Returns the name of this driver.
 */
char *sym_driver_name(void)
{
	return SYM_DRIVER_NAME;
}
/*
 *  Print a buffer in hexadecimal format.
 */
static void sym_printb_hex (u_char *p, int n)
{
	while (n-- > 0)
		printf (" %x", *p++);
}

/*
 *  Same with a label at beginning and .\n at end.
 */
static void sym_printl_hex (char *label, u_char *p, int n)
{
	printf ("%s", label);
	sym_printb_hex (p, n);
	printf (".\n");
}

/*
 *  Print something which allows to retrieve the controler type, 
 *  unit, target, lun concerned by a kernel message.
 */
static void sym_print_target (hcb_p np, int target)
{
	printf ("%s:%d:", sym_name(np), target);
}

static void sym_print_lun(hcb_p np, int target, int lun)
{
	printf ("%s:%d:%d:", sym_name(np), target, lun);
}

/*
 *  Print out the content of a SCSI message.
 */
static int sym_show_msg (u_char * msg)
{
	u_char i;
	printf ("%x",*msg);
	if (*msg==M_EXTENDED) {
		for (i=1;i<8;i++) {
			if (i-1>msg[1]) break;
			printf ("-%x",msg[i]);
		};
		return (i+1);
	} else if ((*msg & 0xf0) == 0x20) {
		printf ("-%x",msg[1]);
		return (2);
	};
	return (1);
}

static void sym_print_msg (ccb_p cp, char *label, u_char *msg)
{
	PRINT_ADDR(cp);
	if (label)
		printf ("%s: ", label);

	(void) sym_show_msg (msg);
	printf (".\n");
}

static void sym_print_nego_msg (hcb_p np, int target, char *label, u_char *msg)
{
	PRINT_TARGET(np, target);
	if (label)
		printf ("%s: ", label);

	(void) sym_show_msg (msg);
	printf (".\n");
}

/*
 *  Print something that tells about extended errors.
 */
void sym_print_xerr(ccb_p cp, int x_status)
{
	if (x_status & XE_PARITY_ERR) {
		PRINT_ADDR(cp);
		printf ("unrecovered SCSI parity error.\n");
	}
	if (x_status & XE_EXTRA_DATA) {
		PRINT_ADDR(cp);
		printf ("extraneous data discarded.\n");
	}
	if (x_status & XE_BAD_PHASE) {
		PRINT_ADDR(cp);
		printf ("illegal scsi phase (4/5).\n");
	}
	if (x_status & XE_SODL_UNRUN) {
		PRINT_ADDR(cp);
		printf ("ODD transfer in DATA OUT phase.\n");
	}
	if (x_status & XE_SWIDE_OVRUN) {
		PRINT_ADDR(cp);
		printf ("ODD transfer in DATA IN phase.\n");
	}
}

/*
 *  Return a string for SCSI BUS mode.
 */
static char *sym_scsi_bus_mode(int mode)
{
	switch(mode) {
	case SMODE_HVD:	return "HVD";
	case SMODE_SE:	return "SE";
	case SMODE_LVD: return "LVD";
	}
	return "??";
}

/*
 *  Soft reset the chip.
 *
 *  Raising SRST when the chip is running may cause 
 *  problems on dual function chips (see below).
 *  On the other hand, LVD devices need some delay 
 *  to settle and report actual BUS mode in STEST4.
 */
static void sym_chip_reset (hcb_p np)
{
	OUTB (nc_istat, SRST);
	UDELAY (10);
	OUTB (nc_istat, 0);
	UDELAY(2000);	/* For BUS MODE to settle */
}

/*
 *  Really soft reset the chip.:)
 *
 *  Some 896 and 876 chip revisions may hang-up if we set 
 *  the SRST (soft reset) bit at the wrong time when SCRIPTS 
 *  are running.
 *  So, we need to abort the current operation prior to 
 *  soft resetting the chip.
 */
static void sym_soft_reset (hcb_p np)
{
	u_char istat = 0;
	int i;

	if (!(np->features & FE_ISTAT1) || !(INB (nc_istat1) & SCRUN))
		goto do_chip_reset;

	OUTB (nc_istat, CABRT);
	for (i = 100000 ; i ; --i) {
		istat = INB (nc_istat);
		if (istat & SIP) {
			INW (nc_sist);
		}
		else if (istat & DIP) {
			if (INB (nc_dstat) & ABRT)
				break;
		}
		UDELAY(5);
	}
	OUTB (nc_istat, 0);
	if (!i)
		printf("%s: unable to abort current chip operation, "
		       "ISTAT=0x%02x.\n", sym_name(np), istat);
do_chip_reset:
	sym_chip_reset (np);
}

/*
 *  Start reset process.
 *
 *  The interrupt handler will reinitialize the chip.
 */
static void sym_start_reset(hcb_p np)
{
	(void) sym_reset_scsi_bus(np, 1);
}
 
int sym_reset_scsi_bus(hcb_p np, int enab_int)
{
	u32 term;
	int retv = 0;

	sym_soft_reset(np);	/* Soft reset the chip */
	if (enab_int)
		OUTW (nc_sien, RST);
	/*
	 *  Enable Tolerant, reset IRQD if present and 
	 *  properly set IRQ mode, prior to resetting the bus.
	 */
	OUTB (nc_stest3, TE);
	OUTB (nc_dcntl, (np->rv_dcntl & IRQM));
	OUTB (nc_scntl1, CRST);
	UDELAY (200);

	if (!SYM_SETUP_SCSI_BUS_CHECK)
		goto out;
	/*
	 *  Check for no terminators or SCSI bus shorts to ground.
	 *  Read SCSI data bus, data parity bits and control signals.
	 *  We are expecting RESET to be TRUE and other signals to be 
	 *  FALSE.
	 */
	term =	INB(nc_sstat0);
	term =	((term & 2) << 7) + ((term & 1) << 17);	/* rst sdp0 */
	term |= ((INB(nc_sstat2) & 0x01) << 26) |	/* sdp1     */
		((INW(nc_sbdl) & 0xff)   << 9)  |	/* d7-0     */
		((INW(nc_sbdl) & 0xff00) << 10) |	/* d15-8    */
		INB(nc_sbcl);	/* req ack bsy sel atn msg cd io    */

	if (!np->maxwide)
		term &= 0x3ffff;

	if (term != (2<<7)) {
		printf("%s: suspicious SCSI data while resetting the BUS.\n",
			sym_name(np));
		printf("%s: %sdp0,d7-0,rst,req,ack,bsy,sel,atn,msg,c/d,i/o = "
			"0x%lx, expecting 0x%lx\n",
			sym_name(np),
			(np->features & FE_WIDE) ? "dp1,d15-8," : "",
			(u_long)term, (u_long)(2<<7));
		if (SYM_SETUP_SCSI_BUS_CHECK == 1)
			retv = 1;
	}
out:
	OUTB (nc_scntl1, 0);
	/* MDELAY(100); */
	return retv;
}

/*
 *  Select SCSI clock frequency
 */
static void sym_selectclock(hcb_p np, u_char scntl3)
{
	/*
	 *  If multiplier not present or not selected, leave here.
	 */
	if (np->multiplier <= 1) {
		OUTB(nc_scntl3,	scntl3);
		return;
	}

	if (sym_verbose >= 2)
		printf ("%s: enabling clock multiplier\n", sym_name(np));

	OUTB(nc_stest1, DBLEN);	   /* Enable clock multiplier		  */
	/*
	 *  Wait for the LCKFRQ bit to be set if supported by the chip.
	 *  Otherwise wait 50 micro-seconds (at least).
	 */
	if (np->features & FE_LCKFRQ) {
		int i = 20;
		while (!(INB(nc_stest4) & LCKFRQ) && --i > 0)
			UDELAY (20);
		if (!i)
			printf("%s: the chip cannot lock the frequency\n",
				sym_name(np));
	} else
		UDELAY ((50+10));
	OUTB(nc_stest3, HSC);		/* Halt the scsi clock		*/
	OUTB(nc_scntl3,	scntl3);
	OUTB(nc_stest1, (DBLEN|DBLSEL));/* Select clock multiplier	*/
	OUTB(nc_stest3, 0x00);		/* Restart scsi clock 		*/
}


/*
 *  Determine the chip's clock frequency.
 *
 *  This is essential for the negotiation of the synchronous 
 *  transfer rate.
 *
 *  Note: we have to return the correct value.
 *  THERE IS NO SAFE DEFAULT VALUE.
 *
 *  Most NCR/SYMBIOS boards are delivered with a 40 Mhz clock.
 *  53C860 and 53C875 rev. 1 support fast20 transfers but 
 *  do not have a clock doubler and so are provided with a 
 *  80 MHz clock. All other fast20 boards incorporate a doubler 
 *  and so should be delivered with a 40 MHz clock.
 *  The recent fast40 chips (895/896/895A/1010) use a 40 Mhz base 
 *  clock and provide a clock quadrupler (160 Mhz).
 */

/*
 *  calculate SCSI clock frequency (in KHz)
 */
static unsigned getfreq (hcb_p np, int gen)
{
	unsigned int ms = 0;
	unsigned int f;

	/*
	 * Measure GEN timer delay in order 
	 * to calculate SCSI clock frequency
	 *
	 * This code will never execute too
	 * many loop iterations (if DELAY is 
	 * reasonably correct). It could get
	 * too low a delay (too high a freq.)
	 * if the CPU is slow executing the 
	 * loop for some reason (an NMI, for
	 * example). For this reason we will
	 * if multiple measurements are to be 
	 * performed trust the higher delay 
	 * (lower frequency returned).
	 */
	OUTW (nc_sien , 0);	/* mask all scsi interrupts */
	(void) INW (nc_sist);	/* clear pending scsi interrupt */
	OUTB (nc_dien , 0);	/* mask all dma interrupts */
	(void) INW (nc_sist);	/* another one, just to be sure :) */
	/*
	 * The C1010-33 core does not report GEN in SIST,
	 * if this interrupt is masked in SIEN.
	 * I don't know yet if the C1010-66 behaves the same way.
	 */
	if (np->features & FE_C10) {
		OUTW (nc_sien, GEN);
		OUTB (nc_istat1, SIRQD);
	}
	OUTB (nc_scntl3, 4);	/* set pre-scaler to divide by 3 */
	OUTB (nc_stime1, 0);	/* disable general purpose timer */
	OUTB (nc_stime1, gen);	/* set to nominal delay of 1<<gen * 125us */
	while (!(INW(nc_sist) & GEN) && ms++ < 100000)
		UDELAY (1000/4);/* count in 1/4 of ms */
	OUTB (nc_stime1, 0);	/* disable general purpose timer */
	/*
	 * Undo C1010-33 specific settings.
	 */
	if (np->features & FE_C10) {
		OUTW (nc_sien, 0);
		OUTB (nc_istat1, 0);
	}
 	/*
 	 * set prescaler to divide by whatever 0 means
 	 * 0 ought to choose divide by 2, but appears
 	 * to set divide by 3.5 mode in my 53c810 ...
 	 */
 	OUTB (nc_scntl3, 0);

  	/*
 	 * adjust for prescaler, and convert into KHz 
  	 */
	f = ms ? ((1 << gen) * (4340*4)) / ms : 0;

	/*
	 * The C1010-33 result is biased by a factor 
	 * of 2/3 compared to earlier chips.
	 */
	if (np->features & FE_C10)
		f = (f * 2) / 3;

	if (sym_verbose >= 2)
		printf ("%s: Delay (GEN=%d): %u msec, %u KHz\n",
			sym_name(np), gen, ms/4, f);

	return f;
}

static unsigned sym_getfreq (hcb_p np)
{
	u_int f1, f2;
	int gen = 8;

	(void) getfreq (np, gen);	/* throw away first result */
	f1 = getfreq (np, gen);
	f2 = getfreq (np, gen);
	if (f1 > f2) f1 = f2;		/* trust lower result	*/
	return f1;
}

/*
 *  Get/probe chip SCSI clock frequency
 */
static void sym_getclock (hcb_p np, int mult)
{
	unsigned char scntl3 = np->sv_scntl3;
	unsigned char stest1 = np->sv_stest1;
	unsigned f1;

	np->multiplier = 1;
	f1 = 40000;
	/*
	 *  True with 875/895/896/895A with clock multiplier selected
	 */
	if (mult > 1 && (stest1 & (DBLEN+DBLSEL)) == DBLEN+DBLSEL) {
		if (sym_verbose >= 2)
			printf ("%s: clock multiplier found\n", sym_name(np));
		np->multiplier = mult;
	}

	/*
	 *  If multiplier not found or scntl3 not 7,5,3,
	 *  reset chip and get frequency from general purpose timer.
	 *  Otherwise trust scntl3 BIOS setting.
	 */
	if (np->multiplier != mult || (scntl3 & 7) < 3 || !(scntl3 & 1)) {
		OUTB (nc_stest1, 0);		/* make sure doubler is OFF */
		f1 = sym_getfreq (np);

		if (sym_verbose)
			printf ("%s: chip clock is %uKHz\n", sym_name(np), f1);

		if	(f1 <	45000)		f1 =  40000;
		else if (f1 <	55000)		f1 =  50000;
		else				f1 =  80000;

		if (f1 < 80000 && mult > 1) {
			if (sym_verbose >= 2)
				printf ("%s: clock multiplier assumed\n",
					sym_name(np));
			np->multiplier	= mult;
		}
	} else {
		if	((scntl3 & 7) == 3)	f1 =  40000;
		else if	((scntl3 & 7) == 5)	f1 =  80000;
		else 				f1 = 160000;

		f1 /= np->multiplier;
	}

	/*
	 *  Compute controller synchronous parameters.
	 */
	f1		*= np->multiplier;
	np->clock_khz	= f1;
}

/*
 *  Get/probe PCI clock frequency
 */
static int sym_getpciclock (hcb_p np)
{
	int f = 0;

	/*
	 *  For now, we only need to know about the actual 
	 *  PCI BUS clock frequency for C1010-66 chips.
	 */
#if 1
	if (np->features & FE_66MHZ) {
#else
	if (1) {
#endif
		OUTB (nc_stest1, SCLK);	/* Use the PCI clock as SCSI clock */
		f = (int) sym_getfreq (np);
		OUTB (nc_stest1, 0);
	}
	np->pciclk_khz = f;

	return f;
}

/*
 *  SYMBIOS chip clock divisor table.
 *
 *  Divisors are multiplied by 10,000,000 in order to make 
 *  calculations more simple.
 */
#define _5M 5000000
static u32 div_10M[] = {2*_5M, 3*_5M, 4*_5M, 6*_5M, 8*_5M, 12*_5M, 16*_5M};

/*
 *  Get clock factor and sync divisor for a given 
 *  synchronous factor period.
 */
static int 
sym_getsync(hcb_p np, u_char dt, u_char sfac, u_char *divp, u_char *fakp)
{
	u32	clk = np->clock_khz;	/* SCSI clock frequency in kHz	*/
	int	div = np->clock_divn;	/* Number of divisors supported	*/
	u32	fak;			/* Sync factor in sxfer		*/
	u32	per;			/* Period in tenths of ns	*/
	u32	kpc;			/* (per * clk)			*/
	int	ret;

	/*
	 *  Compute the synchronous period in tenths of nano-seconds
	 */
	if (dt && sfac <= 9)	per = 125;
	else if	(sfac <= 10)	per = 250;
	else if	(sfac == 11)	per = 303;
	else if	(sfac == 12)	per = 500;
	else			per = 40 * sfac;
	ret = per;

	kpc = per * clk;
	if (dt)
		kpc <<= 1;

	/*
	 *  For earliest C10 revision 0, we cannot use extra 
	 *  clocks for the setting of the SCSI clocking.
	 *  Note that this limits the lowest sync data transfer 
	 *  to 5 Mega-transfers per second and may result in
	 *  using higher clock divisors.
	 */
#if 1
	if ((np->features & (FE_C10|FE_U3EN)) == FE_C10) {
		/*
		 *  Look for the lowest clock divisor that allows an 
		 *  output speed not faster than the period.
		 */
		while (div > 0) {
			--div;
			if (kpc > (div_10M[div] << 2)) {
				++div;
				break;
			}
		}
		fak = 0;			/* No extra clocks */
		if (div == np->clock_divn) {	/* Are we too fast ? */
			ret = -1;
		}
		*divp = div;
		*fakp = fak;
		return ret;
	}
#endif

	/*
	 *  Look for the greatest clock divisor that allows an 
	 *  input speed faster than the period.
	 */
	while (div-- > 0)
		if (kpc >= (div_10M[div] << 2)) break;

	/*
	 *  Calculate the lowest clock factor that allows an output 
	 *  speed not faster than the period, and the max output speed.
	 *  If fak >= 1 we will set both XCLKH_ST and XCLKH_DT.
	 *  If fak >= 2 we will also set XCLKS_ST and XCLKS_DT.
	 */
	if (dt) {
		fak = (kpc - 1) / (div_10M[div] << 1) + 1 - 2;
		/* ret = ((2+fak)*div_10M[div])/np->clock_khz; */
	} else {
		fak = (kpc - 1) / div_10M[div] + 1 - 4;
		/* ret = ((4+fak)*div_10M[div])/np->clock_khz; */
	}

	/*
	 *  Check against our hardware limits, or bugs :).
	 */
	if (fak > 2) {
		fak = 2;
		ret = -1;
	}

	/*
	 *  Compute and return sync parameters.
	 */
	*divp = div;
	*fakp = fak;

	return ret;
}

/*
 *  SYMBIOS chips allow burst lengths of 2, 4, 8, 16, 32, 64,
 *  128 transfers. All chips support at least 16 transfers 
 *  bursts. The 825A, 875 and 895 chips support bursts of up 
 *  to 128 transfers and the 895A and 896 support bursts of up
 *  to 64 transfers. All other chips support up to 16 
 *  transfers bursts.
 *
 *  For PCI 32 bit data transfers each transfer is a DWORD.
 *  It is a QUADWORD (8 bytes) for PCI 64 bit data transfers.
 *
 *  We use log base 2 (burst length) as internal code, with 
 *  value 0 meaning "burst disabled".
 */

/*
 *  Burst length from burst code.
 */
#define burst_length(bc) (!(bc))? 0 : 1 << (bc)

/*
 *  Burst code from io register bits.
 */
#define burst_code(dmode, ctest4, ctest5) \
	(ctest4) & 0x80? 0 : (((dmode) & 0xc0) >> 6) + ((ctest5) & 0x04) + 1

/*
 *  Set initial io register bits from burst code.
 */
static __inline void sym_init_burst(hcb_p np, u_char bc)
{
	np->rv_ctest4	&= ~0x80;
	np->rv_dmode	&= ~(0x3 << 6);
	np->rv_ctest5	&= ~0x4;

	if (!bc) {
		np->rv_ctest4	|= 0x80;
	}
	else {
		--bc;
		np->rv_dmode	|= ((bc & 0x3) << 6);
		np->rv_ctest5	|= (bc & 0x4);
	}
}


/*
 * Print out the list of targets that have some flag disabled by user.
 */
static void sym_print_targets_flag(hcb_p np, int mask, char *msg)
{
	int cnt;
	int i;

	for (cnt = 0, i = 0 ; i < SYM_CONF_MAX_TARGET ; i++) {
		if (i == np->myaddr)
			continue;
		if (np->target[i].usrflags & mask) {
			if (!cnt++)
				printf("%s: %s disabled for targets",
					sym_name(np), msg);
			printf(" %d", i);
		}
	}
	if (cnt)
		printf(".\n");
}

/*
 *  Save initial settings of some IO registers.
 *  Assumed to have been set by BIOS.
 *  We cannot reset the chip prior to reading the 
 *  IO registers, since informations will be lost.
 *  Since the SCRIPTS processor may be running, this 
 *  is not safe on paper, but it seems to work quite 
 *  well. :)
 */
static void sym_save_initial_setting (hcb_p np)
{
	np->sv_scntl0	= INB(nc_scntl0) & 0x0a;
	np->sv_scntl3	= INB(nc_scntl3) & 0x07;
	np->sv_dmode	= INB(nc_dmode)  & 0xce;
	np->sv_dcntl	= INB(nc_dcntl)  & 0xa8;
	np->sv_ctest3	= INB(nc_ctest3) & 0x01;
	np->sv_ctest4	= INB(nc_ctest4) & 0x80;
	np->sv_gpcntl	= INB(nc_gpcntl);
	np->sv_stest1	= INB(nc_stest1);
	np->sv_stest2	= INB(nc_stest2) & 0x20;
	np->sv_stest4	= INB(nc_stest4);
	if (np->features & FE_C10) {	/* Always large DMA fifo + ultra3 */
		np->sv_scntl4	= INB(nc_scntl4);
		np->sv_ctest5	= INB(nc_ctest5) & 0x04;
	}
	else
		np->sv_ctest5	= INB(nc_ctest5) & 0x24;
}

#ifdef CONFIG_PARISC
static u32 parisc_setup_hcb(hcb_p np, u32 period)
{
	unsigned long pdc_period;
	char scsi_mode;
	struct hardware_path hwpath;

	/* Host firmware (PDC) keeps a table for crippling SCSI capabilities.
	 * Many newer machines export one channel of 53c896 chip
	 * as SE, 50-pin HD.  Also used for Multi-initiator SCSI clusters
	 * to set the SCSI Initiator ID.
	 */
	get_pci_node_path(np->s.device, &hwpath);
	if (!pdc_get_initiator(&hwpath, &np->myaddr, &pdc_period,
				&np->maxwide, &scsi_mode))
		return period;

	if (scsi_mode >= 0) {
		/* C3000 PDC reports period/mode */
		SYM_SETUP_SCSI_DIFF = 0;
		switch(scsi_mode) {
		case 0:	np->scsi_mode = SMODE_SE; break;
		case 1:	np->scsi_mode = SMODE_HVD; break;
		case 2:	np->scsi_mode = SMODE_LVD; break;
		default:	break;
		}
	}

	return (u32) pdc_period;
}
#else
static inline int parisc_setup_hcb(hcb_p np, u32 period) { return period; }
#endif
/*
 *  Prepare io register values used by sym_start_up() 
 *  according to selected and supported features.
 */
static int sym_prepare_setting(hcb_p np, struct sym_nvram *nvram)
{
	u_char	burst_max;
	u32	period;
	int i;

	/*
	 *  Wide ?
	 */
	np->maxwide	= (np->features & FE_WIDE)? 1 : 0;

	/*
	 *  Guess the frequency of the chip's clock.
	 */
	if	(np->features & (FE_ULTRA3 | FE_ULTRA2))
		np->clock_khz = 160000;
	else if	(np->features & FE_ULTRA)
		np->clock_khz = 80000;
	else
		np->clock_khz = 40000;

	/*
	 *  Get the clock multiplier factor.
 	 */
	if	(np->features & FE_QUAD)
		np->multiplier	= 4;
	else if	(np->features & FE_DBLR)
		np->multiplier	= 2;
	else
		np->multiplier	= 1;

	/*
	 *  Measure SCSI clock frequency for chips 
	 *  it may vary from assumed one.
	 */
	if (np->features & FE_VARCLK)
		sym_getclock(np, np->multiplier);

	/*
	 * Divisor to be used for async (timer pre-scaler).
	 */
	i = np->clock_divn - 1;
	while (--i >= 0) {
		if (10ul * SYM_CONF_MIN_ASYNC * np->clock_khz > div_10M[i]) {
			++i;
			break;
		}
	}
	np->rv_scntl3 = i+1;

	/*
	 * The C1010 uses hardwired divisors for async.
	 * So, we just throw away, the async. divisor.:-)
	 */
	if (np->features & FE_C10)
		np->rv_scntl3 = 0;

	/*
	 * Minimum synchronous period factor supported by the chip.
	 * Btw, 'period' is in tenths of nanoseconds.
	 */
	period = (4 * div_10M[0] + np->clock_khz - 1) / np->clock_khz;

	period = parisc_setup_hcb(np, period);

	if	(period <= 250)		np->minsync = 10;
	else if	(period <= 303)		np->minsync = 11;
	else if	(period <= 500)		np->minsync = 12;
	else				np->minsync = (period + 40 - 1) / 40;

	/*
	 * Check against chip SCSI standard support (SCSI-2,ULTRA,ULTRA2).
	 */
	if	(np->minsync < 25 &&
		 !(np->features & (FE_ULTRA|FE_ULTRA2|FE_ULTRA3)))
		np->minsync = 25;
	else if	(np->minsync < 12 &&
		 !(np->features & (FE_ULTRA2|FE_ULTRA3)))
		np->minsync = 12;

	/*
	 * Maximum synchronous period factor supported by the chip.
	 */
	period = (11 * div_10M[np->clock_divn - 1]) / (4 * np->clock_khz);
	np->maxsync = period > 2540 ? 254 : period / 10;

	/*
	 * If chip is a C1010, guess the sync limits in DT mode.
	 */
	if ((np->features & (FE_C10|FE_ULTRA3)) == (FE_C10|FE_ULTRA3)) {
		if (np->clock_khz == 160000) {
			np->minsync_dt = 9;
			np->maxsync_dt = 50;
			np->maxoffs_dt = nvram->type ? 62 : 31;
		}
	}
	
	/*
	 *  64 bit addressing  (895A/896/1010) ?
	 */
	if (np->features & FE_DAC) {
#if   SYM_CONF_DMA_ADDRESSING_MODE == 0
		np->rv_ccntl1	|= (DDAC);
#elif SYM_CONF_DMA_ADDRESSING_MODE == 1
		if (!np->use_dac)
			np->rv_ccntl1	|= (DDAC);
		else
			np->rv_ccntl1	|= (XTIMOD | EXTIBMV);
#elif SYM_CONF_DMA_ADDRESSING_MODE == 2
		if (!np->use_dac)
			np->rv_ccntl1	|= (DDAC);
		else
			np->rv_ccntl1	|= (0 | EXTIBMV);
#endif
	}

	/*
	 *  Phase mismatch handled by SCRIPTS (895A/896/1010) ?
  	 */
	if (np->features & FE_NOPM)
		np->rv_ccntl0	|= (ENPMJ);

 	/*
	 *  C1010-33 Errata: Part Number:609-039638 (rev. 1) is fixed.
	 *  In dual channel mode, contention occurs if internal cycles
	 *  are used. Disable internal cycles.
	 */
	if (np->device_id == PCI_ID_LSI53C1010_33 &&
	    np->revision_id < 0x1)
		np->rv_ccntl0	|=  DILS;

	/*
	 *  Select burst length (dwords)
	 */
	burst_max	= SYM_SETUP_BURST_ORDER;
	if (burst_max == 255)
		burst_max = burst_code(np->sv_dmode, np->sv_ctest4,
				       np->sv_ctest5);
	if (burst_max > 7)
		burst_max = 7;
	if (burst_max > np->maxburst)
		burst_max = np->maxburst;

	/*
	 *  DEL 352 - 53C810 Rev x11 - Part Number 609-0392140 - ITEM 2.
	 *  This chip and the 860 Rev 1 may wrongly use PCI cache line 
	 *  based transactions on LOAD/STORE instructions. So we have 
	 *  to prevent these chips from using such PCI transactions in 
	 *  this driver. The generic ncr driver that does not use 
	 *  LOAD/STORE instructions does not need this work-around.
	 */
	if ((np->device_id == PCI_ID_SYM53C810 &&
	     np->revision_id >= 0x10 && np->revision_id <= 0x11) ||
	    (np->device_id == PCI_ID_SYM53C860 &&
	     np->revision_id <= 0x1))
		np->features &= ~(FE_WRIE|FE_ERL|FE_ERMP);

	/*
	 *  Select all supported special features.
	 *  If we are using on-board RAM for scripts, prefetch (PFEN) 
	 *  does not help, but burst op fetch (BOF) does.
	 *  Disabling PFEN makes sure BOF will be used.
	 */
	if (np->features & FE_ERL)
		np->rv_dmode	|= ERL;		/* Enable Read Line */
	if (np->features & FE_BOF)
		np->rv_dmode	|= BOF;		/* Burst Opcode Fetch */
	if (np->features & FE_ERMP)
		np->rv_dmode	|= ERMP;	/* Enable Read Multiple */
#if 1
	if ((np->features & FE_PFEN) && !np->ram_ba)
#else
	if (np->features & FE_PFEN)
#endif
		np->rv_dcntl	|= PFEN;	/* Prefetch Enable */
	if (np->features & FE_CLSE)
		np->rv_dcntl	|= CLSE;	/* Cache Line Size Enable */
	if (np->features & FE_WRIE)
		np->rv_ctest3	|= WRIE;	/* Write and Invalidate */
	if (np->features & FE_DFS)
		np->rv_ctest5	|= DFS;		/* Dma Fifo Size */

	/*
	 *  Select some other
	 */
	if (SYM_SETUP_PCI_PARITY)
		np->rv_ctest4	|= MPEE; /* Master parity checking */
	if (SYM_SETUP_SCSI_PARITY)
		np->rv_scntl0	|= 0x0a; /*  full arb., ena parity, par->ATN  */

	/*
	 *  Get parity checking, host ID and verbose mode from NVRAM
	 */
	np->myaddr = 255;
	sym_nvram_setup_host (np, nvram);

	/*
	 *  Get SCSI addr of host adapter (set by bios?).
	 */
	if (np->myaddr == 255) {
		np->myaddr = INB(nc_scid) & 0x07;
		if (!np->myaddr)
			np->myaddr = SYM_SETUP_HOST_ID;
	}

	/*
	 *  Prepare initial io register bits for burst length
	 */
	sym_init_burst(np, burst_max);

	/*
	 *  Set SCSI BUS mode.
	 *  - LVD capable chips (895/895A/896/1010) report the 
	 *    current BUS mode through the STEST4 IO register.
	 *  - For previous generation chips (825/825A/875), 
	 *    user has to tell us how to check against HVD, 
	 *    since a 100% safe algorithm is not possible.
	 */
	np->scsi_mode = SMODE_SE;
	if (np->features & (FE_ULTRA2|FE_ULTRA3))
		np->scsi_mode = (np->sv_stest4 & SMODE);
	else if	(np->features & FE_DIFF) {
		if (SYM_SETUP_SCSI_DIFF == 1) {
			if (np->sv_scntl3) {
				if (np->sv_stest2 & 0x20)
					np->scsi_mode = SMODE_HVD;
			}
			else if (nvram->type == SYM_SYMBIOS_NVRAM) {
				if (!(INB(nc_gpreg) & 0x08))
					np->scsi_mode = SMODE_HVD;
			}
		}
		else if	(SYM_SETUP_SCSI_DIFF == 2)
			np->scsi_mode = SMODE_HVD;
	}
	if (np->scsi_mode == SMODE_HVD)
		np->rv_stest2 |= 0x20;

	/*
	 *  Set LED support from SCRIPTS.
	 *  Ignore this feature for boards known to use a 
	 *  specific GPIO wiring and for the 895A, 896 
	 *  and 1010 that drive the LED directly.
	 */
	if ((SYM_SETUP_SCSI_LED || 
	     (nvram->type == SYM_SYMBIOS_NVRAM ||
	      (nvram->type == SYM_TEKRAM_NVRAM &&
	       np->device_id == PCI_ID_SYM53C895))) &&
	    !(np->features & FE_LEDC) && !(np->sv_gpcntl & 0x01))
		np->features |= FE_LED0;

	/*
	 *  Set irq mode.
	 */
	switch(SYM_SETUP_IRQ_MODE & 3) {
	case 2:
		np->rv_dcntl	|= IRQM;
		break;
	case 1:
		np->rv_dcntl	|= (np->sv_dcntl & IRQM);
		break;
	default:
		break;
	}

	/*
	 *  Configure targets according to driver setup.
	 *  If NVRAM present get targets setup from NVRAM.
	 */
	for (i = 0 ; i < SYM_CONF_MAX_TARGET ; i++) {
		tcb_p tp = &np->target[i];

		tp->tinfo.user.scsi_version = tp->tinfo.curr.scsi_version= 2;
		tp->tinfo.user.spi_version  = tp->tinfo.curr.spi_version = 2;
		tp->tinfo.user.period = np->minsync;
		tp->tinfo.user.offset = np->maxoffs;
		tp->tinfo.user.width  = np->maxwide ? BUS_16_BIT : BUS_8_BIT;
		tp->usrflags |= (SYM_DISC_ENABLED | SYM_TAGS_ENABLED);
		tp->usrtags = SYM_SETUP_MAX_TAG;

		sym_nvram_setup_target (np, i, nvram);

		/*
		 * Some single-ended devices may crash on receiving a
		 * PPR negotiation attempt.  Only try PPR if we're in
		 * LVD mode.
		 */
		if (np->features & FE_ULTRA3) {
			tp->tinfo.user.options |= PPR_OPT_DT;
			tp->tinfo.user.period = np->minsync_dt;
			tp->tinfo.user.offset = np->maxoffs_dt;
			tp->tinfo.user.spi_version = 3;
		}

		if (!tp->usrtags)
			tp->usrflags &= ~SYM_TAGS_ENABLED;
	}

	/*
	 *  Let user know about the settings.
	 */
	i = nvram->type;
	printf("%s: %s NVRAM, ID %d, Fast-%d, %s, %s\n", sym_name(np),
		i  == SYM_SYMBIOS_NVRAM ? "Symbios" :
		(i == SYM_TEKRAM_NVRAM  ? "Tekram" : "No"),
		np->myaddr,
		(np->features & FE_ULTRA3) ? 80 : 
		(np->features & FE_ULTRA2) ? 40 : 
		(np->features & FE_ULTRA)  ? 20 : 10,
		sym_scsi_bus_mode(np->scsi_mode),
		(np->rv_scntl0 & 0xa)	? "parity checking" : "NO parity");
	/*
	 *  Tell him more on demand.
	 */
	if (sym_verbose) {
		printf("%s: %s IRQ line driver%s\n",
			sym_name(np),
			np->rv_dcntl & IRQM ? "totem pole" : "open drain",
			np->ram_ba ? ", using on-chip SRAM" : "");
		printf("%s: using %s firmware.\n", sym_name(np), np->fw_name);
		if (np->features & FE_NOPM)
			printf("%s: handling phase mismatch from SCRIPTS.\n", 
			       sym_name(np));
	}
	/*
	 *  And still more.
	 */
	if (sym_verbose >= 2) {
		printf ("%s: initial SCNTL3/DMODE/DCNTL/CTEST3/4/5 = "
			"(hex) %02x/%02x/%02x/%02x/%02x/%02x\n",
			sym_name(np), np->sv_scntl3, np->sv_dmode, np->sv_dcntl,
			np->sv_ctest3, np->sv_ctest4, np->sv_ctest5);

		printf ("%s: final   SCNTL3/DMODE/DCNTL/CTEST3/4/5 = "
			"(hex) %02x/%02x/%02x/%02x/%02x/%02x\n",
			sym_name(np), np->rv_scntl3, np->rv_dmode, np->rv_dcntl,
			np->rv_ctest3, np->rv_ctest4, np->rv_ctest5);
	}
	/*
	 *  Let user be aware of targets that have some disable flags set.
	 */
	sym_print_targets_flag(np, SYM_SCAN_BOOT_DISABLED, "SCAN AT BOOT");
	if (sym_verbose)
		sym_print_targets_flag(np, SYM_SCAN_LUNS_DISABLED,
				       "SCAN FOR LUNS");

	return 0;
}

/*
 *  Test the pci bus snoop logic :-(
 *
 *  Has to be called with interrupts disabled.
 */
#ifndef SYM_CONF_IOMAPPED
static int sym_regtest (hcb_p np)
{
	register volatile u32 data;
	/*
	 *  chip registers may NOT be cached.
	 *  write 0xffffffff to a read only register area,
	 *  and try to read it back.
	 */
	data = 0xffffffff;
	OUTL_OFF(offsetof(struct sym_reg, nc_dstat), data);
	data = INL_OFF(offsetof(struct sym_reg, nc_dstat));
#if 1
	if (data == 0xffffffff) {
#else
	if ((data & 0xe2f0fffd) != 0x02000080) {
#endif
		printf ("CACHE TEST FAILED: reg dstat-sstat2 readback %x.\n",
			(unsigned) data);
		return (0x10);
	};
	return (0);
}
#endif

static int sym_snooptest (hcb_p np)
{
	u32	sym_rd, sym_wr, sym_bk, host_rd, host_wr, pc, dstat;
	int	i, err=0;
#ifndef SYM_CONF_IOMAPPED
	err |= sym_regtest (np);
	if (err) return (err);
#endif
restart_test:
	/*
	 *  Enable Master Parity Checking as we intend 
	 *  to enable it for normal operations.
	 */
	OUTB (nc_ctest4, (np->rv_ctest4 & MPEE));
	/*
	 *  init
	 */
	pc  = SCRIPTZ_BA (np, snooptest);
	host_wr = 1;
	sym_wr  = 2;
	/*
	 *  Set memory and register.
	 */
	np->scratch = cpu_to_scr(host_wr);
	OUTL (nc_temp, sym_wr);
	/*
	 *  Start script (exchange values)
	 */
	OUTL (nc_dsa, np->hcb_ba);
	OUTL_DSP (pc);
	/*
	 *  Wait 'til done (with timeout)
	 */
	for (i=0; i<SYM_SNOOP_TIMEOUT; i++)
		if (INB(nc_istat) & (INTF|SIP|DIP))
			break;
	if (i>=SYM_SNOOP_TIMEOUT) {
		printf ("CACHE TEST FAILED: timeout.\n");
		return (0x20);
	};
	/*
	 *  Check for fatal DMA errors.
	 */
	dstat = INB (nc_dstat);
#if 1	/* Band aiding for broken hardwares that fail PCI parity */
	if ((dstat & MDPE) && (np->rv_ctest4 & MPEE)) {
		printf ("%s: PCI DATA PARITY ERROR DETECTED - "
			"DISABLING MASTER DATA PARITY CHECKING.\n",
			sym_name(np));
		np->rv_ctest4 &= ~MPEE;
		goto restart_test;
	}
#endif
	if (dstat & (MDPE|BF|IID)) {
		printf ("CACHE TEST FAILED: DMA error (dstat=0x%02x).", dstat);
		return (0x80);
	}
	/*
	 *  Save termination position.
	 */
	pc = INL (nc_dsp);
	/*
	 *  Read memory and register.
	 */
	host_rd = scr_to_cpu(np->scratch);
	sym_rd  = INL (nc_scratcha);
	sym_bk  = INL (nc_temp);
	/*
	 *  Check termination position.
	 */
	if (pc != SCRIPTZ_BA (np, snoopend)+8) {
		printf ("CACHE TEST FAILED: script execution failed.\n");
		printf ("start=%08lx, pc=%08lx, end=%08lx\n", 
			(u_long) SCRIPTZ_BA (np, snooptest), (u_long) pc,
			(u_long) SCRIPTZ_BA (np, snoopend) +8);
		return (0x40);
	};
	/*
	 *  Show results.
	 */
	if (host_wr != sym_rd) {
		printf ("CACHE TEST FAILED: host wrote %d, chip read %d.\n",
			(int) host_wr, (int) sym_rd);
		err |= 1;
	};
	if (host_rd != sym_wr) {
		printf ("CACHE TEST FAILED: chip wrote %d, host read %d.\n",
			(int) sym_wr, (int) host_rd);
		err |= 2;
	};
	if (sym_bk != sym_wr) {
		printf ("CACHE TEST FAILED: chip wrote %d, read back %d.\n",
			(int) sym_wr, (int) sym_bk);
		err |= 4;
	};

	return (err);
}

/*
 *  log message for real hard errors
 *
 *  sym0 targ 0?: ERROR (ds:si) (so-si-sd) (sx/s3/s4) @ name (dsp:dbc).
 *  	      reg: r0 r1 r2 r3 r4 r5 r6 ..... rf.
 *
 *  exception register:
 *  	ds:	dstat
 *  	si:	sist
 *
 *  SCSI bus lines:
 *  	so:	control lines as driven by chip.
 *  	si:	control lines as seen by chip.
 *  	sd:	scsi data lines as seen by chip.
 *
 *  wide/fastmode:
 *  	sx:	sxfer  (see the manual)
 *  	s3:	scntl3 (see the manual)
 *  	s4:	scntl4 (see the manual)
 *
 *  current script command:
 *  	dsp:	script address (relative to start of script).
 *  	dbc:	first word of script command.
 *
 *  First 24 register of the chip:
 *  	r0..rf
 */
static void sym_log_hard_error(hcb_p np, u_short sist, u_char dstat)
{
	u32	dsp;
	int	script_ofs;
	int	script_size;
	char	*script_name;
	u_char	*script_base;
	int	i;

	dsp	= INL (nc_dsp);

	if	(dsp > np->scripta_ba &&
		 dsp <= np->scripta_ba + np->scripta_sz) {
		script_ofs	= dsp - np->scripta_ba;
		script_size	= np->scripta_sz;
		script_base	= (u_char *) np->scripta0;
		script_name	= "scripta";
	}
	else if (np->scriptb_ba < dsp && 
		 dsp <= np->scriptb_ba + np->scriptb_sz) {
		script_ofs	= dsp - np->scriptb_ba;
		script_size	= np->scriptb_sz;
		script_base	= (u_char *) np->scriptb0;
		script_name	= "scriptb";
	} else {
		script_ofs	= dsp;
		script_size	= 0;
		script_base	= NULL;
		script_name	= "mem";
	}

	printf ("%s:%d: ERROR (%x:%x) (%x-%x-%x) (%x/%x/%x) @ (%s %x:%08x).\n",
		sym_name (np), (unsigned)INB (nc_sdid)&0x0f, dstat, sist,
		(unsigned)INB (nc_socl),   (unsigned)INB (nc_sbcl),
		(unsigned)INB (nc_sbdl),   (unsigned)INB (nc_sxfer),
		(unsigned)INB (nc_scntl3),
		(np->features & FE_C10) ?  (unsigned)INB (nc_scntl4) : 0,
		script_name, script_ofs,   (unsigned)INL (nc_dbc));

	if (((script_ofs & 3) == 0) &&
	    (unsigned)script_ofs < script_size) {
		printf ("%s: script cmd = %08x\n", sym_name(np),
			scr_to_cpu((int) *(u32 *)(script_base + script_ofs)));
	}

        printf ("%s: regdump:", sym_name(np));
        for (i=0; i<24;i++)
            printf (" %02x", (unsigned)INB_OFF(i));
        printf (".\n");

	/*
	 *  PCI BUS error.
	 */
	if (dstat & (MDPE|BF))
		sym_log_bus_error(np);
}

static struct sym_pci_chip sym_pci_dev_table[] = {
 {PCI_ID_SYM53C810, 0x0f, "810", 4, 8, 4, 64,
 FE_ERL}
 ,
#ifdef SYM_DEBUG_GENERIC_SUPPORT
 {PCI_ID_SYM53C810, 0xff, "810a", 4,  8, 4, 1,
 FE_BOF}
 ,
#else
 {PCI_ID_SYM53C810, 0xff, "810a", 4,  8, 4, 1,
 FE_CACHE_SET|FE_LDSTR|FE_PFEN|FE_BOF}
 ,
#endif
 {PCI_ID_SYM53C815, 0xff, "815", 4,  8, 4, 64,
 FE_BOF|FE_ERL}
 ,
 {PCI_ID_SYM53C825, 0x0f, "825", 6,  8, 4, 64,
 FE_WIDE|FE_BOF|FE_ERL|FE_DIFF}
 ,
 {PCI_ID_SYM53C825, 0xff, "825a", 6,  8, 4, 2,
 FE_WIDE|FE_CACHE0_SET|FE_BOF|FE_DFS|FE_LDSTR|FE_PFEN|FE_RAM|FE_DIFF}
 ,
 {PCI_ID_SYM53C860, 0xff, "860", 4,  8, 5, 1,
 FE_ULTRA|FE_CACHE_SET|FE_BOF|FE_LDSTR|FE_PFEN}
 ,
 {PCI_ID_SYM53C875, 0x01, "875", 6, 16, 5, 2,
 FE_WIDE|FE_ULTRA|FE_CACHE0_SET|FE_BOF|FE_DFS|FE_LDSTR|FE_PFEN|
 FE_RAM|FE_DIFF|FE_VARCLK}
 ,
 {PCI_ID_SYM53C875, 0xff, "875", 6, 16, 5, 2,
 FE_WIDE|FE_ULTRA|FE_DBLR|FE_CACHE0_SET|FE_BOF|FE_DFS|FE_LDSTR|FE_PFEN|
 FE_RAM|FE_DIFF|FE_VARCLK}
 ,
 {PCI_ID_SYM53C875_2, 0xff, "875", 6, 16, 5, 2,
 FE_WIDE|FE_ULTRA|FE_DBLR|FE_CACHE0_SET|FE_BOF|FE_DFS|FE_LDSTR|FE_PFEN|
 FE_RAM|FE_DIFF|FE_VARCLK}
 ,
 {PCI_ID_SYM53C885, 0xff, "885", 6, 16, 5, 2,
 FE_WIDE|FE_ULTRA|FE_DBLR|FE_CACHE0_SET|FE_BOF|FE_DFS|FE_LDSTR|FE_PFEN|
 FE_RAM|FE_DIFF|FE_VARCLK}
 ,
#ifdef SYM_DEBUG_GENERIC_SUPPORT
 {PCI_ID_SYM53C895, 0xff, "895", 6, 31, 7, 2,
 FE_WIDE|FE_ULTRA2|FE_QUAD|FE_CACHE_SET|FE_BOF|FE_DFS|
 FE_RAM|FE_LCKFRQ}
 ,
#else
 {PCI_ID_SYM53C895, 0xff, "895", 6, 31, 7, 2,
 FE_WIDE|FE_ULTRA2|FE_QUAD|FE_CACHE_SET|FE_BOF|FE_DFS|FE_LDSTR|FE_PFEN|
 FE_RAM|FE_LCKFRQ}
 ,
#endif
 {PCI_ID_SYM53C896, 0xff, "896", 6, 31, 7, 4,
 FE_WIDE|FE_ULTRA2|FE_QUAD|FE_CACHE_SET|FE_BOF|FE_DFS|FE_LDSTR|FE_PFEN|
 FE_RAM|FE_RAM8K|FE_64BIT|FE_DAC|FE_IO256|FE_NOPM|FE_LEDC|FE_LCKFRQ}
 ,
 {PCI_ID_SYM53C895A, 0xff, "895a", 6, 31, 7, 4,
 FE_WIDE|FE_ULTRA2|FE_QUAD|FE_CACHE_SET|FE_BOF|FE_DFS|FE_LDSTR|FE_PFEN|
 FE_RAM|FE_RAM8K|FE_DAC|FE_IO256|FE_NOPM|FE_LEDC|FE_LCKFRQ}
 ,
 {PCI_ID_SYM53C875A, 0xff, "875a", 6, 31, 7, 4,
 FE_WIDE|FE_ULTRA|FE_QUAD|FE_CACHE_SET|FE_BOF|FE_DFS|FE_LDSTR|FE_PFEN|
 FE_RAM|FE_DAC|FE_IO256|FE_NOPM|FE_LEDC|FE_LCKFRQ}
 ,
 {PCI_ID_LSI53C1010_33, 0x00, "1010-33", 6, 31, 7, 8,
 FE_WIDE|FE_ULTRA3|FE_QUAD|FE_CACHE_SET|FE_BOF|FE_DFBC|FE_LDSTR|FE_PFEN|
 FE_RAM|FE_RAM8K|FE_64BIT|FE_DAC|FE_IO256|FE_NOPM|FE_LEDC|FE_CRC|
 FE_C10}
 ,
 {PCI_ID_LSI53C1010_33, 0xff, "1010-33", 6, 31, 7, 8,
 FE_WIDE|FE_ULTRA3|FE_QUAD|FE_CACHE_SET|FE_BOF|FE_DFBC|FE_LDSTR|FE_PFEN|
 FE_RAM|FE_RAM8K|FE_64BIT|FE_DAC|FE_IO256|FE_NOPM|FE_LEDC|FE_CRC|
 FE_C10|FE_U3EN}
 ,
 {PCI_ID_LSI53C1010_66, 0xff, "1010-66", 6, 31, 7, 8,
 FE_WIDE|FE_ULTRA3|FE_QUAD|FE_CACHE_SET|FE_BOF|FE_DFBC|FE_LDSTR|FE_PFEN|
 FE_RAM|FE_RAM8K|FE_64BIT|FE_DAC|FE_IO256|FE_NOPM|FE_LEDC|FE_66MHZ|FE_CRC|
 FE_C10|FE_U3EN}
 ,
 {PCI_ID_LSI53C1510D, 0xff, "1510d", 6, 31, 7, 4,
 FE_WIDE|FE_ULTRA2|FE_QUAD|FE_CACHE_SET|FE_BOF|FE_DFS|FE_LDSTR|FE_PFEN|
 FE_RAM|FE_IO256|FE_LEDC}
};

#define sym_pci_num_devs \
	(sizeof(sym_pci_dev_table) / sizeof(sym_pci_dev_table[0]))

/*
 *  Look up the chip table.
 *
 *  Return a pointer to the chip entry if found, 
 *  zero otherwise.
 */
struct sym_pci_chip *
sym_lookup_pci_chip_table (u_short device_id, u_char revision)
{
	struct	sym_pci_chip *chip;
	int	i;

	for (i = 0; i < sym_pci_num_devs; i++) {
		chip = &sym_pci_dev_table[i];
		if (device_id != chip->device_id)
			continue;
		if (revision > chip->revision_id)
			continue;
		return chip;
	}

	return NULL;
}

#if SYM_CONF_DMA_ADDRESSING_MODE == 2
/*
 *  Lookup the 64 bit DMA segments map.
 *  This is only used if the direct mapping 
 *  has been unsuccessful.
 */
int sym_lookup_dmap(hcb_p np, u32 h, int s)
{
	int i;

	if (!np->use_dac)
		goto weird;

	/* Look up existing mappings */
	for (i = SYM_DMAP_SIZE-1; i > 0; i--) {
		if (h == np->dmap_bah[i])
			return i;
	}
	/* If direct mapping is free, get it */
	if (!np->dmap_bah[s])
		goto new;
	/* Collision -> lookup free mappings */
	for (s = SYM_DMAP_SIZE-1; s > 0; s--) {
		if (!np->dmap_bah[s])
			goto new;
	}
weird:
	panic("sym: ran out of 64 bit DMA segment registers");
	return -1;
new:
	np->dmap_bah[s] = h;
	np->dmap_dirty = 1;
	return s;
}

/*
 *  Update IO registers scratch C..R so they will be 
 *  in sync. with queued CCB expectations.
 */
static void sym_update_dmap_regs(hcb_p np)
{
	int o, i;

	if (!np->dmap_dirty)
		return;
	o = offsetof(struct sym_reg, nc_scrx[0]);
	for (i = 0; i < SYM_DMAP_SIZE; i++) {
		OUTL_OFF(o, np->dmap_bah[i]);
		o += 4;
	}
	np->dmap_dirty = 0;
}
#endif

/*
 *  Prepare the next negotiation message if needed.
 *
 *  Fill in the part of message buffer that contains the 
 *  negotiation and the nego_status field of the CCB.
 *  Returns the size of the message in bytes.
 */
static int sym_prepare_nego(hcb_p np, ccb_p cp, int nego, u_char *msgptr)
{
	tcb_p tp = &np->target[cp->target];
	int msglen = 0;

	/*
	 *  Early C1010 chips need a work-around for DT 
	 *  data transfer to work.
	 */
	if (!(np->features & FE_U3EN))
		tp->tinfo.goal.options = 0;
	/*
	 *  negotiate using PPR ?
	 */
	if (tp->tinfo.goal.options & PPR_OPT_MASK)
		nego = NS_PPR;
	/*
	 *  negotiate wide transfers ?
	 */
	else if (tp->tinfo.curr.width != tp->tinfo.goal.width)
		nego = NS_WIDE;
	/*
	 *  negotiate synchronous transfers?
	 */
	else if (tp->tinfo.curr.period != tp->tinfo.goal.period ||
		 tp->tinfo.curr.offset != tp->tinfo.goal.offset)
		nego = NS_SYNC;

	switch (nego) {
	case NS_SYNC:
		msgptr[msglen++] = M_EXTENDED;
		msgptr[msglen++] = 3;
		msgptr[msglen++] = M_X_SYNC_REQ;
		msgptr[msglen++] = tp->tinfo.goal.period;
		msgptr[msglen++] = tp->tinfo.goal.offset;
		break;
	case NS_WIDE:
		msgptr[msglen++] = M_EXTENDED;
		msgptr[msglen++] = 2;
		msgptr[msglen++] = M_X_WIDE_REQ;
		msgptr[msglen++] = tp->tinfo.goal.width;
		break;
	case NS_PPR:
		msgptr[msglen++] = M_EXTENDED;
		msgptr[msglen++] = 6;
		msgptr[msglen++] = M_X_PPR_REQ;
		msgptr[msglen++] = tp->tinfo.goal.period;
		msgptr[msglen++] = 0;
		msgptr[msglen++] = tp->tinfo.goal.offset;
		msgptr[msglen++] = tp->tinfo.goal.width;
		msgptr[msglen++] = tp->tinfo.goal.options & PPR_OPT_DT;
		break;
	};

	cp->nego_status = nego;

	if (nego) {
		tp->nego_cp = cp; /* Keep track a nego will be performed */
		if (DEBUG_FLAGS & DEBUG_NEGO) {
			sym_print_nego_msg(np, cp->target, 
					  nego == NS_SYNC ? "sync msgout" :
					  nego == NS_WIDE ? "wide msgout" :
					  "ppr msgout", msgptr);
		};
	};

	return msglen;
}

/*
 *  Insert a job into the start queue.
 */
void sym_put_start_queue(hcb_p np, ccb_p cp)
{
	u_short	qidx;

#ifdef SYM_CONF_IARB_SUPPORT
	/*
	 *  If the previously queued CCB is not yet done, 
	 *  set the IARB hint. The SCRIPTS will go with IARB 
	 *  for this job when starting the previous one.
	 *  We leave devices a chance to win arbitration by 
	 *  not using more than 'iarb_max' consecutive 
	 *  immediate arbitrations.
	 */
	if (np->last_cp && np->iarb_count < np->iarb_max) {
		np->last_cp->host_flags |= HF_HINT_IARB;
		++np->iarb_count;
	}
	else
		np->iarb_count = 0;
	np->last_cp = cp;
#endif

#if   SYM_CONF_DMA_ADDRESSING_MODE == 2
	/*
	 *  Make SCRIPTS aware of the 64 bit DMA 
	 *  segment registers not being up-to-date.
	 */
	if (np->dmap_dirty)
		cp->host_xflags |= HX_DMAP_DIRTY;
#endif

	/*
	 *  Optionnaly, set the IO timeout condition.
	 */
#ifdef	SYM_OPT_HANDLE_IO_TIMEOUT
	sym_timeout_ccb(np, cp, sym_cam_timeout(cp->cam_ccb));
#endif

	/*
	 *  Insert first the idle task and then our job.
	 *  The MBs should ensure proper ordering.
	 */
	qidx = np->squeueput + 2;
	if (qidx >= MAX_QUEUE*2) qidx = 0;

	np->squeue [qidx]	   = cpu_to_scr(np->idletask_ba);
	MEMORY_WRITE_BARRIER();
	np->squeue [np->squeueput] = cpu_to_scr(cp->ccb_ba);

	np->squeueput = qidx;

	if (DEBUG_FLAGS & DEBUG_QUEUE)
		printf ("%s: queuepos=%d.\n", sym_name (np), np->squeueput);

	/*
	 *  Script processor may be waiting for reselect.
	 *  Wake it up.
	 */
	MEMORY_WRITE_BARRIER();
	OUTB (nc_istat, SIGP|np->istat_sem);
}

#ifdef SYM_OPT_HANDLE_DEVICE_QUEUEING
/*
 *  Start next ready-to-start CCBs.
 */
void sym_start_next_ccbs(hcb_p np, lcb_p lp, int maxn)
{
	SYM_QUEHEAD *qp;
	ccb_p cp;

	/* 
	 *  Paranoia, as usual. :-)
	 */
	assert(!lp->started_tags || !lp->started_no_tag);

	/*
	 *  Try to start as many commands as asked by caller.
	 *  Prevent from having both tagged and untagged 
	 *  commands queued to the device at the same time.
	 */
	while (maxn--) {
		qp = sym_remque_head(&lp->waiting_ccbq);
		if (!qp)
			break;
		cp = sym_que_entry(qp, struct sym_ccb, link2_ccbq);
		if (cp->tag != NO_TAG) {
			if (lp->started_no_tag ||
			    lp->started_tags >= lp->started_max) {
				sym_insque_head(qp, &lp->waiting_ccbq);
				break;
			}
			lp->itlq_tbl[cp->tag] = cpu_to_scr(cp->ccb_ba);
			lp->head.resel_sa =
				cpu_to_scr(SCRIPTA_BA (np, resel_tag));
			++lp->started_tags;
		} else {
			if (lp->started_no_tag || lp->started_tags) {
				sym_insque_head(qp, &lp->waiting_ccbq);
				break;
			}
			lp->head.itl_task_sa = cpu_to_scr(cp->ccb_ba);
			lp->head.resel_sa =
			      cpu_to_scr(SCRIPTA_BA (np, resel_no_tag));
			++lp->started_no_tag;
		}
		cp->started = 1;
		sym_insque_tail(qp, &lp->started_ccbq);
		sym_put_start_queue(np, cp);
	}
}
#endif /* SYM_OPT_HANDLE_DEVICE_QUEUEING */

/*
 *  The chip may have completed jobs. Look at the DONE QUEUE.
 *
 *  On paper, memory read barriers may be needed here to 
 *  prevent out of order LOADs by the CPU from having 
 *  prefetched stale data prior to DMA having occurred.
 */
static int sym_wakeup_done (hcb_p np)
{
	ccb_p cp;
	int i, n;
	u32 dsa;

	n = 0;
	i = np->dqueueget;

	/* MEMORY_READ_BARRIER(); */
	while (1) {
		dsa = scr_to_cpu(np->dqueue[i]);
		if (!dsa)
			break;
		np->dqueue[i] = 0;
		if ((i = i+2) >= MAX_QUEUE*2)
			i = 0;

		cp = sym_ccb_from_dsa(np, dsa);
		if (cp) {
			MEMORY_READ_BARRIER();
			sym_complete_ok (np, cp);
			++n;
		}
		else
			printf ("%s: bad DSA (%x) in done queue.\n",
				sym_name(np), (u_int) dsa);
	}
	np->dqueueget = i;

	return n;
}

/*
 *  Complete all active CCBs with error.
 *  Used on CHIP/SCSI RESET.
 */
static void sym_flush_busy_queue (hcb_p np, int cam_status)
{
	/*
	 *  Move all active CCBs to the COMP queue 
	 *  and flush this queue.
	 */
	sym_que_splice(&np->busy_ccbq, &np->comp_ccbq);
	sym_que_init(&np->busy_ccbq);
	sym_flush_comp_queue(np, cam_status);
}

/*
 *  Start chip.
 *
 *  'reason' means:
 *     0: initialisation.
 *     1: SCSI BUS RESET delivered or received.
 *     2: SCSI BUS MODE changed.
 */
void sym_start_up (hcb_p np, int reason)
{
 	int	i;
	u32	phys;

 	/*
	 *  Reset chip if asked, otherwise just clear fifos.
 	 */
	if (reason == 1)
		sym_soft_reset(np);
	else {
		OUTB (nc_stest3, TE|CSF);
		OUTONB (nc_ctest3, CLF);
	}
 
	/*
	 *  Clear Start Queue
	 */
	phys = np->squeue_ba;
	for (i = 0; i < MAX_QUEUE*2; i += 2) {
		np->squeue[i]   = cpu_to_scr(np->idletask_ba);
		np->squeue[i+1] = cpu_to_scr(phys + (i+2)*4);
	}
	np->squeue[MAX_QUEUE*2-1] = cpu_to_scr(phys);

	/*
	 *  Start at first entry.
	 */
	np->squeueput = 0;

	/*
	 *  Clear Done Queue
	 */
	phys = np->dqueue_ba;
	for (i = 0; i < MAX_QUEUE*2; i += 2) {
		np->dqueue[i]   = 0;
		np->dqueue[i+1] = cpu_to_scr(phys + (i+2)*4);
	}
	np->dqueue[MAX_QUEUE*2-1] = cpu_to_scr(phys);

	/*
	 *  Start at first entry.
	 */
	np->dqueueget = 0;

	/*
	 *  Install patches in scripts.
	 *  This also let point to first position the start 
	 *  and done queue pointers used from SCRIPTS.
	 */
	np->fw_patch(np);

	/*
	 *  Wakeup all pending jobs.
	 */
	sym_flush_busy_queue(np, CAM_SCSI_BUS_RESET);

	/*
	 *  Init chip.
	 */
	OUTB (nc_istat,  0x00   );	/*  Remove Reset, abort */
	UDELAY (2000);	/* The 895 needs time for the bus mode to settle */

	OUTB (nc_scntl0, np->rv_scntl0 | 0xc0);
					/*  full arb., ena parity, par->ATN  */
	OUTB (nc_scntl1, 0x00);		/*  odd parity, and remove CRST!! */

	sym_selectclock(np, np->rv_scntl3);	/* Select SCSI clock */

	OUTB (nc_scid  , RRE|np->myaddr);	/* Adapter SCSI address */
	OUTW (nc_respid, 1ul<<np->myaddr);	/* Id to respond to */
	OUTB (nc_istat , SIGP	);		/*  Signal Process */
	OUTB (nc_dmode , np->rv_dmode);		/* Burst length, dma mode */
	OUTB (nc_ctest5, np->rv_ctest5);	/* Large fifo + large burst */

	OUTB (nc_dcntl , NOCOM|np->rv_dcntl);	/* Protect SFBR */
	OUTB (nc_ctest3, np->rv_ctest3);	/* Write and invalidate */
	OUTB (nc_ctest4, np->rv_ctest4);	/* Master parity checking */

	/* Extended Sreq/Sack filtering not supported on the C10 */
	if (np->features & FE_C10)
		OUTB (nc_stest2, np->rv_stest2);
	else
		OUTB (nc_stest2, EXT|np->rv_stest2);

	OUTB (nc_stest3, TE);			/* TolerANT enable */
	OUTB (nc_stime0, 0x0c);			/* HTH disabled  STO 0.25 sec */

	/*
	 *  For now, disable AIP generation on C1010-66.
	 */
	if (np->device_id == PCI_ID_LSI53C1010_66)
		OUTB (nc_aipcntl1, DISAIP);

	/*
	 *  C10101 rev. 0 errata.
	 *  Errant SGE's when in narrow. Write bits 4 & 5 of
	 *  STEST1 register to disable SGE. We probably should do 
	 *  that from SCRIPTS for each selection/reselection, but 
	 *  I just don't want. :)
	 */
	if (np->device_id == PCI_ID_LSI53C1010_33 &&
	    np->revision_id < 1)
		OUTB (nc_stest1, INB(nc_stest1) | 0x30);

	/*
	 *  DEL 441 - 53C876 Rev 5 - Part Number 609-0392787/2788 - ITEM 2.
	 *  Disable overlapped arbitration for some dual function devices, 
	 *  regardless revision id (kind of post-chip-design feature. ;-))
	 */
	if (np->device_id == PCI_ID_SYM53C875)
		OUTB (nc_ctest0, (1<<5));
	else if (np->device_id == PCI_ID_SYM53C896)
		np->rv_ccntl0 |= DPR;

	/*
	 *  Write CCNTL0/CCNTL1 for chips capable of 64 bit addressing 
	 *  and/or hardware phase mismatch, since only such chips 
	 *  seem to support those IO registers.
	 */
	if (np->features & (FE_DAC|FE_NOPM)) {
		OUTB (nc_ccntl0, np->rv_ccntl0);
		OUTB (nc_ccntl1, np->rv_ccntl1);
	}

#if	SYM_CONF_DMA_ADDRESSING_MODE == 2
	/*
	 *  Set up scratch C and DRS IO registers to map the 32 bit 
	 *  DMA address range our data structures are located in.
	 */
	if (np->use_dac) {
		np->dmap_bah[0] = 0;	/* ??? */
		OUTL (nc_scrx[0], np->dmap_bah[0]);
		OUTL (nc_drs, np->dmap_bah[0]);
	}
#endif

	/*
	 *  If phase mismatch handled by scripts (895A/896/1010),
	 *  set PM jump addresses.
	 */
	if (np->features & FE_NOPM) {
		OUTL (nc_pmjad1, SCRIPTB_BA (np, pm_handle));
		OUTL (nc_pmjad2, SCRIPTB_BA (np, pm_handle));
	}

	/*
	 *    Enable GPIO0 pin for writing if LED support from SCRIPTS.
	 *    Also set GPIO5 and clear GPIO6 if hardware LED control.
	 */
	if (np->features & FE_LED0)
		OUTB(nc_gpcntl, INB(nc_gpcntl) & ~0x01);
	else if (np->features & FE_LEDC)
		OUTB(nc_gpcntl, (INB(nc_gpcntl) & ~0x41) | 0x20);

	/*
	 *      enable ints
	 */
	OUTW (nc_sien , STO|HTH|MA|SGE|UDC|RST|PAR);
	OUTB (nc_dien , MDPE|BF|SSI|SIR|IID);

	/*
	 *  For 895/6 enable SBMC interrupt and save current SCSI bus mode.
	 *  Try to eat the spurious SBMC interrupt that may occur when 
	 *  we reset the chip but not the SCSI BUS (at initialization).
	 */
	if (np->features & (FE_ULTRA2|FE_ULTRA3)) {
		OUTONW (nc_sien, SBMC);
		if (reason == 0) {
			MDELAY(100);
			INW (nc_sist);
		}
		np->scsi_mode = INB (nc_stest4) & SMODE;
	}

	/*
	 *  Fill in target structure.
	 *  Reinitialize usrsync.
	 *  Reinitialize usrwide.
	 *  Prepare sync negotiation according to actual SCSI bus mode.
	 */
	for (i=0;i<SYM_CONF_MAX_TARGET;i++) {
		tcb_p tp = &np->target[i];

		tp->to_reset  = 0;
		tp->head.sval = 0;
		tp->head.wval = np->rv_scntl3;
		tp->head.uval = 0;

		tp->tinfo.curr.period = 0;
		tp->tinfo.curr.offset = 0;
		tp->tinfo.curr.width  = BUS_8_BIT;
		tp->tinfo.curr.options = 0;
	}

	/*
	 *  Download SCSI SCRIPTS to on-chip RAM if present,
	 *  and start script processor.
	 *  We do the download preferently from the CPU.
	 *  For platforms that may not support PCI memory mapping,
	 *  we use simple SCRIPTS that performs MEMORY MOVEs.
	 */
	if (np->ram_ba) {
		if (sym_verbose >= 2)
			printf ("%s: Downloading SCSI SCRIPTS.\n",
				sym_name(np));
		if (np->ram_ws == 8192) {
			OUTRAM_OFF(4096, np->scriptb0, np->scriptb_sz);
			phys =  scr_to_cpu(np->scr_ram_seg);
			OUTL (nc_mmws, phys);
			OUTL (nc_mmrs, phys);
			OUTL (nc_sfs,  phys);
			phys = SCRIPTB_BA (np, start64);
		}
		else
			phys = SCRIPTA_BA (np, init);
		OUTRAM_OFF(0, np->scripta0, np->scripta_sz);
	}
	else
		phys = SCRIPTA_BA (np, init);

	np->istat_sem = 0;

	OUTL (nc_dsa, np->hcb_ba);
	OUTL_DSP (phys);

	/*
	 *  Notify the XPT about the RESET condition.
	 */
	if (reason != 0)
		sym_xpt_async_bus_reset(np);
}

/*
 *  Switch trans mode for current job and it's target.
 */
static void sym_settrans(hcb_p np, int target, u_char dt, u_char ofs,
			 u_char per, u_char wide, u_char div, u_char fak)
{
	SYM_QUEHEAD *qp;
	u_char sval, wval, uval;
	tcb_p tp = &np->target[target];

	assert(target == (INB (nc_sdid) & 0x0f));

	sval = tp->head.sval;
	wval = tp->head.wval;
	uval = tp->head.uval;

#if 0
	printf("XXXX sval=%x wval=%x uval=%x (%x)\n", 
		sval, wval, uval, np->rv_scntl3);
#endif
	/*
	 *  Set the offset.
	 */
	if (!(np->features & FE_C10))
		sval = (sval & ~0x1f) | ofs;
	else
		sval = (sval & ~0x3f) | ofs;

	/*
	 *  Set the sync divisor and extra clock factor.
	 */
	if (ofs != 0) {
		wval = (wval & ~0x70) | ((div+1) << 4);
		if (!(np->features & FE_C10))
			sval = (sval & ~0xe0) | (fak << 5);
		else {
			uval = uval & ~(XCLKH_ST|XCLKH_DT|XCLKS_ST|XCLKS_DT);
			if (fak >= 1) uval |= (XCLKH_ST|XCLKH_DT);
			if (fak >= 2) uval |= (XCLKS_ST|XCLKS_DT);
		}
	}

	/*
	 *  Set the bus width.
	 */
	wval = wval & ~EWS;
	if (wide != 0)
		wval |= EWS;

	/*
	 *  Set misc. ultra enable bits.
	 */
	if (np->features & FE_C10) {
		uval = uval & ~(U3EN|AIPCKEN);
		if (dt)	{
			assert(np->features & FE_U3EN);
			uval |= U3EN;
		}
	}
	else {
		wval = wval & ~ULTRA;
		if (per <= 12)	wval |= ULTRA;
	}

	/*
	 *   Stop there if sync parameters are unchanged.
	 */
	if (tp->head.sval == sval && 
	    tp->head.wval == wval &&
	    tp->head.uval == uval)
		return;
	tp->head.sval = sval;
	tp->head.wval = wval;
	tp->head.uval = uval;

	/*
	 *  Disable extended Sreq/Sack filtering if per < 50.
	 *  Not supported on the C1010.
	 */
	if (per < 50 && !(np->features & FE_C10))
		OUTOFFB (nc_stest2, EXT);

	/*
	 *  set actual value and sync_status
	 */
	OUTB (nc_sxfer,  tp->head.sval);
	OUTB (nc_scntl3, tp->head.wval);

	if (np->features & FE_C10) {
		OUTB (nc_scntl4, tp->head.uval);
	}

	/*
	 *  patch ALL busy ccbs of this target.
	 */
	FOR_EACH_QUEUED_ELEMENT(&np->busy_ccbq, qp) {
		ccb_p cp;
		cp = sym_que_entry(qp, struct sym_ccb, link_ccbq);
		if (cp->target != target)
			continue;
		cp->phys.select.sel_scntl3 = tp->head.wval;
		cp->phys.select.sel_sxfer  = tp->head.sval;
		if (np->features & FE_C10) {
			cp->phys.select.sel_scntl4 = tp->head.uval;
		}
	}
}

/*
 *  We received a WDTR.
 *  Let everything be aware of the changes.
 */
static void sym_setwide(hcb_p np, int target, u_char wide)
{
	tcb_p tp = &np->target[target];

	sym_settrans(np, target, 0, 0, 0, wide, 0, 0);

	tp->tinfo.goal.width = tp->tinfo.curr.width = wide;
	tp->tinfo.curr.offset = 0;
	tp->tinfo.curr.period = 0;
	tp->tinfo.curr.options = 0;

	sym_xpt_async_nego_wide(np, target);
}

/*
 *  We received a SDTR.
 *  Let everything be aware of the changes.
 */
static void
sym_setsync(hcb_p np, int target,
            u_char ofs, u_char per, u_char div, u_char fak)
{
	tcb_p tp = &np->target[target];
	u_char wide = (tp->head.wval & EWS) ? BUS_16_BIT : BUS_8_BIT;

	sym_settrans(np, target, 0, ofs, per, wide, div, fak);

	tp->tinfo.curr.period  = per;
	tp->tinfo.curr.offset  = ofs;
	tp->tinfo.curr.options = 0;

	if (!(tp->tinfo.goal.options & PPR_OPT_MASK)) {
		tp->tinfo.goal.period	= per;
		tp->tinfo.goal.offset	= ofs;
		tp->tinfo.goal.options	= 0;
	}

	sym_xpt_async_nego_sync(np, target);
}

/*
 *  We received a PPR.
 *  Let everything be aware of the changes.
 */
static void 
sym_setpprot(hcb_p np, int target, u_char dt, u_char ofs,
             u_char per, u_char wide, u_char div, u_char fak)
{
	tcb_p tp = &np->target[target];

	sym_settrans(np, target, dt, ofs, per, wide, div, fak);

	tp->tinfo.goal.width	= tp->tinfo.curr.width  = wide;
	tp->tinfo.goal.period	= tp->tinfo.curr.period = per;
	tp->tinfo.goal.offset	= tp->tinfo.curr.offset = ofs;
	tp->tinfo.goal.options	= tp->tinfo.curr.options = dt;

	sym_xpt_async_nego_ppr(np, target);
}

/*
 *  generic recovery from scsi interrupt
 *
 *  The doc says that when the chip gets an SCSI interrupt,
 *  it tries to stop in an orderly fashion, by completing 
 *  an instruction fetch that had started or by flushing 
 *  the DMA fifo for a write to memory that was executing.
 *  Such a fashion is not enough to know if the instruction 
 *  that was just before the current DSP value has been 
 *  executed or not.
 *
 *  There are some small SCRIPTS sections that deal with 
 *  the start queue and the done queue that may break any 
 *  assomption from the C code if we are interrupted 
 *  inside, so we reset if this happens. Btw, since these 
 *  SCRIPTS sections are executed while the SCRIPTS hasn't 
 *  started SCSI operations, it is very unlikely to happen.
 *
 *  All the driver data structures are supposed to be 
 *  allocated from the same 4 GB memory window, so there 
 *  is a 1 to 1 relationship between DSA and driver data 
 *  structures. Since we are careful :) to invalidate the 
 *  DSA when we complete a command or when the SCRIPTS 
 *  pushes a DSA into a queue, we can trust it when it 
 *  points to a CCB.
 */
static void sym_recover_scsi_int (hcb_p np, u_char hsts)
{
	u32	dsp	= INL (nc_dsp);
	u32	dsa	= INL (nc_dsa);
	ccb_p cp	= sym_ccb_from_dsa(np, dsa);

	/*
	 *  If we haven't been interrupted inside the SCRIPTS 
	 *  critical pathes, we can safely restart the SCRIPTS 
	 *  and trust the DSA value if it matches a CCB.
	 */
	if ((!(dsp > SCRIPTA_BA (np, getjob_begin) &&
	       dsp < SCRIPTA_BA (np, getjob_end) + 1)) &&
	    (!(dsp > SCRIPTA_BA (np, ungetjob) &&
	       dsp < SCRIPTA_BA (np, reselect) + 1)) &&
	    (!(dsp > SCRIPTB_BA (np, sel_for_abort) &&
	       dsp < SCRIPTB_BA (np, sel_for_abort_1) + 1)) &&
	    (!(dsp > SCRIPTA_BA (np, done) &&
	       dsp < SCRIPTA_BA (np, done_end) + 1))) {
		OUTB (nc_ctest3, np->rv_ctest3 | CLF);	/* clear dma fifo  */
		OUTB (nc_stest3, TE|CSF);		/* clear scsi fifo */
		/*
		 *  If we have a CCB, let the SCRIPTS call us back for 
		 *  the handling of the error with SCRATCHA filled with 
		 *  STARTPOS. This way, we will be able to freeze the 
		 *  device queue and requeue awaiting IOs.
		 */
		if (cp) {
			cp->host_status = hsts;
			OUTL_DSP (SCRIPTA_BA (np, complete_error));
		}
		/*
		 *  Otherwise just restart the SCRIPTS.
		 */
		else {
			OUTL (nc_dsa, 0xffffff);
			OUTL_DSP (SCRIPTA_BA (np, start));
		}
	}
	else
		goto reset_all;

	return;

reset_all:
	sym_start_reset(np);
}

/*
 *  chip exception handler for selection timeout
 */
static void sym_int_sto (hcb_p np)
{
	u32 dsp	= INL (nc_dsp);

	if (DEBUG_FLAGS & DEBUG_TINY) printf ("T");

	if (dsp == SCRIPTA_BA (np, wf_sel_done) + 8)
		sym_recover_scsi_int(np, HS_SEL_TIMEOUT);
	else
		sym_start_reset(np);
}

/*
 *  chip exception handler for unexpected disconnect
 */
static void sym_int_udc (hcb_p np)
{
	printf ("%s: unexpected disconnect\n", sym_name(np));
	sym_recover_scsi_int(np, HS_UNEXPECTED);
}

/*
 *  chip exception handler for SCSI bus mode change
 *
 *  spi2-r12 11.2.3 says a transceiver mode change must 
 *  generate a reset event and a device that detects a reset 
 *  event shall initiate a hard reset. It says also that a
 *  device that detects a mode change shall set data transfer 
 *  mode to eight bit asynchronous, etc...
 *  So, just reinitializing all except chip should be enough.
 */
static void sym_int_sbmc (hcb_p np)
{
	u_char scsi_mode = INB (nc_stest4) & SMODE;

	/*
	 *  Notify user.
	 */
	printf("%s: SCSI BUS mode change from %s to %s.\n", sym_name(np),
		sym_scsi_bus_mode(np->scsi_mode), sym_scsi_bus_mode(scsi_mode));

	/*
	 *  Should suspend command processing for a few seconds and 
	 *  reinitialize all except the chip.
	 */
	sym_start_up (np, 2);
}

/*
 *  chip exception handler for SCSI parity error.
 *
 *  When the chip detects a SCSI parity error and is 
 *  currently executing a (CH)MOV instruction, it does 
 *  not interrupt immediately, but tries to finish the 
 *  transfer of the current scatter entry before 
 *  interrupting. The following situations may occur:
 *
 *  - The complete scatter entry has been transferred 
 *    without the device having changed phase.
 *    The chip will then interrupt with the DSP pointing 
 *    to the instruction that follows the MOV.
 *
 *  - A phase mismatch occurs before the MOV finished 
 *    and phase errors are to be handled by the C code.
 *    The chip will then interrupt with both PAR and MA 
 *    conditions set.
 *
 *  - A phase mismatch occurs before the MOV finished and 
 *    phase errors are to be handled by SCRIPTS.
 *    The chip will load the DSP with the phase mismatch 
 *    JUMP address and interrupt the host processor.
 */
static void sym_int_par (hcb_p np, u_short sist)
{
	u_char	hsts	= INB (HS_PRT);
	u32	dsp	= INL (nc_dsp);
	u32	dbc	= INL (nc_dbc);
	u32	dsa	= INL (nc_dsa);
	u_char	sbcl	= INB (nc_sbcl);
	u_char	cmd	= dbc >> 24;
	int phase	= cmd & 7;
	ccb_p	cp	= sym_ccb_from_dsa(np, dsa);

	printf("%s: SCSI parity error detected: SCR1=%d DBC=%x SBCL=%x\n",
		sym_name(np), hsts, dbc, sbcl);

	/*
	 *  Check that the chip is connected to the SCSI BUS.
	 */
	if (!(INB (nc_scntl1) & ISCON)) {
		sym_recover_scsi_int(np, HS_UNEXPECTED);
		return;
	}

	/*
	 *  If the nexus is not clearly identified, reset the bus.
	 *  We will try to do better later.
	 */
	if (!cp)
		goto reset_all;

	/*
	 *  Check instruction was a MOV, direction was INPUT and 
	 *  ATN is asserted.
	 */
	if ((cmd & 0xc0) || !(phase & 1) || !(sbcl & 0x8))
		goto reset_all;

	/*
	 *  Keep track of the parity error.
	 */
	OUTONB (HF_PRT, HF_EXT_ERR);
	cp->xerr_status |= XE_PARITY_ERR;

	/*
	 *  Prepare the message to send to the device.
	 */
	np->msgout[0] = (phase == 7) ? M_PARITY : M_ID_ERROR;

	/*
	 *  If the old phase was DATA IN phase, we have to deal with
	 *  the 3 situations described above.
	 *  For other input phases (MSG IN and STATUS), the device 
	 *  must resend the whole thing that failed parity checking 
	 *  or signal error. So, jumping to dispatcher should be OK.
	 */
	if (phase == 1 || phase == 5) {
		/* Phase mismatch handled by SCRIPTS */
		if (dsp == SCRIPTB_BA (np, pm_handle))
			OUTL_DSP (dsp);
		/* Phase mismatch handled by the C code */
		else if (sist & MA)
			sym_int_ma (np);
		/* No phase mismatch occurred */
		else {
			sym_set_script_dp (np, cp, dsp);
			OUTL_DSP (SCRIPTA_BA (np, dispatch));
		}
	}
	else if (phase == 7)	/* We definitely cannot handle parity errors */
#if 1				/* in message-in phase due to the relection  */
		goto reset_all; /* path and various message anticipations.   */
#else
		OUTL_DSP (SCRIPTA_BA (np, clrack));
#endif
	else
		OUTL_DSP (SCRIPTA_BA (np, dispatch));
	return;

reset_all:
	sym_start_reset(np);
	return;
}

/*
 *  chip exception handler for phase errors.
 *
 *  We have to construct a new transfer descriptor,
 *  to transfer the rest of the current block.
 */
static void sym_int_ma (hcb_p np)
{
	u32	dbc;
	u32	rest;
	u32	dsp;
	u32	dsa;
	u32	nxtdsp;
	u32	*vdsp;
	u32	oadr, olen;
	u32	*tblp;
        u32	newcmd;
	u_int	delta;
	u_char	cmd;
	u_char	hflags, hflags0;
	struct	sym_pmc *pm;
	ccb_p	cp;

	dsp	= INL (nc_dsp);
	dbc	= INL (nc_dbc);
	dsa	= INL (nc_dsa);

	cmd	= dbc >> 24;
	rest	= dbc & 0xffffff;
	delta	= 0;

	/*
	 *  locate matching cp if any.
	 */
	cp = sym_ccb_from_dsa(np, dsa);

	/*
	 *  Donnot take into account dma fifo and various buffers in 
	 *  INPUT phase since the chip flushes everything before 
	 *  raising the MA interrupt for interrupted INPUT phases.
	 *  For DATA IN phase, we will check for the SWIDE later.
	 */
	if ((cmd & 7) != 1 && (cmd & 7) != 5) {
		u_char ss0, ss2;

		if (np->features & FE_DFBC)
			delta = INW (nc_dfbc);
		else {
			u32 dfifo;

			/*
			 * Read DFIFO, CTEST[4-6] using 1 PCI bus ownership.
			 */
			dfifo = INL(nc_dfifo);

			/*
			 *  Calculate remaining bytes in DMA fifo.
			 *  (CTEST5 = dfifo >> 16)
			 */
			if (dfifo & (DFS << 16))
				delta = ((((dfifo >> 8) & 0x300) |
				          (dfifo & 0xff)) - rest) & 0x3ff;
			else
				delta = ((dfifo & 0xff) - rest) & 0x7f;
		}

		/*
		 *  The data in the dma fifo has not been transfered to
		 *  the target -> add the amount to the rest
		 *  and clear the data.
		 *  Check the sstat2 register in case of wide transfer.
		 */
		rest += delta;
		ss0  = INB (nc_sstat0);
		if (ss0 & OLF) rest++;
		if (!(np->features & FE_C10))
			if (ss0 & ORF) rest++;
		if (cp && (cp->phys.select.sel_scntl3 & EWS)) {
			ss2 = INB (nc_sstat2);
			if (ss2 & OLF1) rest++;
			if (!(np->features & FE_C10))
				if (ss2 & ORF1) rest++;
		};

		/*
		 *  Clear fifos.
		 */
		OUTB (nc_ctest3, np->rv_ctest3 | CLF);	/* dma fifo  */
		OUTB (nc_stest3, TE|CSF);		/* scsi fifo */
	}

	/*
	 *  log the information
	 */
	if (DEBUG_FLAGS & (DEBUG_TINY|DEBUG_PHASE))
		printf ("P%x%x RL=%d D=%d ", cmd&7, INB(nc_sbcl)&7,
			(unsigned) rest, (unsigned) delta);

	/*
	 *  try to find the interrupted script command,
	 *  and the address at which to continue.
	 */
	vdsp	= NULL;
	nxtdsp	= 0;
	if	(dsp >  np->scripta_ba &&
		 dsp <= np->scripta_ba + np->scripta_sz) {
		vdsp = (u32 *)((char*)np->scripta0 + (dsp-np->scripta_ba-8));
		nxtdsp = dsp;
	}
	else if	(dsp >  np->scriptb_ba &&
		 dsp <= np->scriptb_ba + np->scriptb_sz) {
		vdsp = (u32 *)((char*)np->scriptb0 + (dsp-np->scriptb_ba-8));
		nxtdsp = dsp;
	}

	/*
	 *  log the information
	 */
	if (DEBUG_FLAGS & DEBUG_PHASE) {
		printf ("\nCP=%p DSP=%x NXT=%x VDSP=%p CMD=%x ",
			cp, (unsigned)dsp, (unsigned)nxtdsp, vdsp, cmd);
	};

	if (!vdsp) {
		printf ("%s: interrupted SCRIPT address not found.\n", 
			sym_name (np));
		goto reset_all;
	}

	if (!cp) {
		printf ("%s: SCSI phase error fixup: CCB already dequeued.\n", 
			sym_name (np));
		goto reset_all;
	}

	/*
	 *  get old startaddress and old length.
	 */
	oadr = scr_to_cpu(vdsp[1]);

	if (cmd & 0x10) {	/* Table indirect */
		tblp = (u32 *) ((char*) &cp->phys + oadr);
		olen = scr_to_cpu(tblp[0]);
		oadr = scr_to_cpu(tblp[1]);
	} else {
		tblp = (u32 *) 0;
		olen = scr_to_cpu(vdsp[0]) & 0xffffff;
	};

	if (DEBUG_FLAGS & DEBUG_PHASE) {
		printf ("OCMD=%x\nTBLP=%p OLEN=%x OADR=%x\n",
			(unsigned) (scr_to_cpu(vdsp[0]) >> 24),
			tblp,
			(unsigned) olen,
			(unsigned) oadr);
	};

	/*
	 *  check cmd against assumed interrupted script command.
	 *  If dt data phase, the MOVE instruction hasn't bit 4 of 
	 *  the phase.
	 */
	if (((cmd & 2) ? cmd : (cmd & ~4)) != (scr_to_cpu(vdsp[0]) >> 24)) {
		PRINT_ADDR(cp);
		printf ("internal error: cmd=%02x != %02x=(vdsp[0] >> 24)\n",
			(unsigned)cmd, (unsigned)scr_to_cpu(vdsp[0]) >> 24);

		goto reset_all;
	};

	/*
	 *  if old phase not dataphase, leave here.
	 */
	if (cmd & 2) {
		PRINT_ADDR(cp);
		printf ("phase change %x-%x %d@%08x resid=%d.\n",
			cmd&7, INB(nc_sbcl)&7, (unsigned)olen,
			(unsigned)oadr, (unsigned)rest);
		goto unexpected_phase;
	};

	/*
	 *  Choose the correct PM save area.
	 *
	 *  Look at the PM_SAVE SCRIPT if you want to understand 
	 *  this stuff. The equivalent code is implemented in 
	 *  SCRIPTS for the 895A, 896 and 1010 that are able to 
	 *  handle PM from the SCRIPTS processor.
	 */
	hflags0 = INB (HF_PRT);
	hflags = hflags0;

	if (hflags & (HF_IN_PM0 | HF_IN_PM1 | HF_DP_SAVED)) {
		if (hflags & HF_IN_PM0)
			nxtdsp = scr_to_cpu(cp->phys.pm0.ret);
		else if	(hflags & HF_IN_PM1)
			nxtdsp = scr_to_cpu(cp->phys.pm1.ret);

		if (hflags & HF_DP_SAVED)
			hflags ^= HF_ACT_PM;
	}

	if (!(hflags & HF_ACT_PM)) {
		pm = &cp->phys.pm0;
		newcmd = SCRIPTA_BA (np, pm0_data);
	}
	else {
		pm = &cp->phys.pm1;
		newcmd = SCRIPTA_BA (np, pm1_data);
	}

	hflags &= ~(HF_IN_PM0 | HF_IN_PM1 | HF_DP_SAVED);
	if (hflags != hflags0)
		OUTB (HF_PRT, hflags);

	/*
	 *  fillin the phase mismatch context
	 */
	pm->sg.addr = cpu_to_scr(oadr + olen - rest);
	pm->sg.size = cpu_to_scr(rest);
	pm->ret     = cpu_to_scr(nxtdsp);

	/*
	 *  If we have a SWIDE,
	 *  - prepare the address to write the SWIDE from SCRIPTS,
	 *  - compute the SCRIPTS address to restart from,
	 *  - move current data pointer context by one byte.
	 */
	nxtdsp = SCRIPTA_BA (np, dispatch);
	if ((cmd & 7) == 1 && cp && (cp->phys.select.sel_scntl3 & EWS) &&
	    (INB (nc_scntl2) & WSR)) {
		u32 tmp;

		/*
		 *  Set up the table indirect for the MOVE
		 *  of the residual byte and adjust the data 
		 *  pointer context.
		 */
		tmp = scr_to_cpu(pm->sg.addr);
		cp->phys.wresid.addr = cpu_to_scr(tmp);
		pm->sg.addr = cpu_to_scr(tmp + 1);
		tmp = scr_to_cpu(pm->sg.size);
		cp->phys.wresid.size = cpu_to_scr((tmp&0xff000000) | 1);
		pm->sg.size = cpu_to_scr(tmp - 1);

		/*
		 *  If only the residual byte is to be moved, 
		 *  no PM context is needed.
		 */
		if ((tmp&0xffffff) == 1)
			newcmd = pm->ret;

		/*
		 *  Prepare the address of SCRIPTS that will 
		 *  move the residual byte to memory.
		 */
		nxtdsp = SCRIPTB_BA (np, wsr_ma_helper);
	}

	if (DEBUG_FLAGS & DEBUG_PHASE) {
		PRINT_ADDR(cp);
		printf ("PM %x %x %x / %x %x %x.\n",
			hflags0, hflags, newcmd,
			(unsigned)scr_to_cpu(pm->sg.addr),
			(unsigned)scr_to_cpu(pm->sg.size),
			(unsigned)scr_to_cpu(pm->ret));
	}

	/*
	 *  Restart the SCRIPTS processor.
	 */
	sym_set_script_dp (np, cp, newcmd);
	OUTL_DSP (nxtdsp);
	return;

	/*
	 *  Unexpected phase changes that occurs when the current phase 
	 *  is not a DATA IN or DATA OUT phase are due to error conditions.
	 *  Such event may only happen when the SCRIPTS is using a 
	 *  multibyte SCSI MOVE.
	 *
	 *  Phase change		Some possible cause
	 *
	 *  COMMAND  --> MSG IN	SCSI parity error detected by target.
	 *  COMMAND  --> STATUS	Bad command or refused by target.
	 *  MSG OUT  --> MSG IN     Message rejected by target.
	 *  MSG OUT  --> COMMAND    Bogus target that discards extended
	 *  			negotiation messages.
	 *
	 *  The code below does not care of the new phase and so 
	 *  trusts the target. Why to annoy it ?
	 *  If the interrupted phase is COMMAND phase, we restart at
	 *  dispatcher.
	 *  If a target does not get all the messages after selection, 
	 *  the code assumes blindly that the target discards extended 
	 *  messages and clears the negotiation status.
	 *  If the target does not want all our response to negotiation,
	 *  we force a SIR_NEGO_PROTO interrupt (it is a hack that avoids 
	 *  bloat for such a should_not_happen situation).
	 *  In all other situation, we reset the BUS.
	 *  Are these assumptions reasonnable ? (Wait and see ...)
	 */
unexpected_phase:
	dsp -= 8;
	nxtdsp = 0;

	switch (cmd & 7) {
	case 2:	/* COMMAND phase */
		nxtdsp = SCRIPTA_BA (np, dispatch);
		break;
#if 0
	case 3:	/* STATUS  phase */
		nxtdsp = SCRIPTA_BA (np, dispatch);
		break;
#endif
	case 6:	/* MSG OUT phase */
		/*
		 *  If the device may want to use untagged when we want 
		 *  tagged, we prepare an IDENTIFY without disc. granted, 
		 *  since we will not be able to handle reselect.
		 *  Otherwise, we just don't care.
		 */
		if	(dsp == SCRIPTA_BA (np, send_ident)) {
			if (cp->tag != NO_TAG && olen - rest <= 3) {
				cp->host_status = HS_BUSY;
				np->msgout[0] = M_IDENTIFY | cp->lun;
				nxtdsp = SCRIPTB_BA (np, ident_break_atn);
			}
			else
				nxtdsp = SCRIPTB_BA (np, ident_break);
		}
		else if	(dsp == SCRIPTB_BA (np, send_wdtr) ||
			 dsp == SCRIPTB_BA (np, send_sdtr) ||
			 dsp == SCRIPTB_BA (np, send_ppr)) {
			nxtdsp = SCRIPTB_BA (np, nego_bad_phase);
		}
		break;
#if 0
	case 7:	/* MSG IN  phase */
		nxtdsp = SCRIPTA_BA (np, clrack);
		break;
#endif
	}

	if (nxtdsp) {
		OUTL_DSP (nxtdsp);
		return;
	}

reset_all:
	sym_start_reset(np);
}

/*
 *  chip interrupt handler
 *
 *  In normal situations, interrupt conditions occur one at 
 *  a time. But when something bad happens on the SCSI BUS, 
 *  the chip may raise several interrupt flags before 
 *  stopping and interrupting the CPU. The additionnal 
 *  interrupt flags are stacked in some extra registers 
 *  after the SIP and/or DIP flag has been raised in the 
 *  ISTAT. After the CPU has read the interrupt condition 
 *  flag from SIST or DSTAT, the chip unstacks the other 
 *  interrupt flags and sets the corresponding bits in 
 *  SIST or DSTAT. Since the chip starts stacking once the 
 *  SIP or DIP flag is set, there is a small window of time 
 *  where the stacking does not occur.
 *
 *  Typically, multiple interrupt conditions may happen in 
 *  the following situations:
 *
 *  - SCSI parity error + Phase mismatch  (PAR|MA)
 *    When an parity error is detected in input phase 
 *    and the device switches to msg-in phase inside a 
 *    block MOV.
 *  - SCSI parity error + Unexpected disconnect (PAR|UDC)
 *    When a stupid device does not want to handle the 
 *    recovery of an SCSI parity error.
 *  - Some combinations of STO, PAR, UDC, ...
 *    When using non compliant SCSI stuff, when user is 
 *    doing non compliant hot tampering on the BUS, when 
 *    something really bad happens to a device, etc ...
 *
 *  The heuristic suggested by SYMBIOS to handle 
 *  multiple interrupts is to try unstacking all 
 *  interrupts conditions and to handle them on some 
 *  priority based on error severity.
 *  This will work when the unstacking has been 
 *  successful, but we cannot be 100 % sure of that, 
 *  since the CPU may have been faster to unstack than 
 *  the chip is able to stack. Hmmm ... But it seems that 
 *  such a situation is very unlikely to happen.
 *
 *  If this happen, for example STO caught by the CPU 
 *  then UDC happenning before the CPU have restarted 
 *  the SCRIPTS, the driver may wrongly complete the 
 *  same command on UDC, since the SCRIPTS didn't restart 
 *  and the DSA still points to the same command.
 *  We avoid this situation by setting the DSA to an 
 *  invalid value when the CCB is completed and before 
 *  restarting the SCRIPTS.
 *
 *  Another issue is that we need some section of our 
 *  recovery procedures to be somehow uninterruptible but 
 *  the SCRIPTS processor does not provides such a 
 *  feature. For this reason, we handle recovery preferently 
 *  from the C code and check against some SCRIPTS critical 
 *  sections from the C code.
 *
 *  Hopefully, the interrupt handling of the driver is now 
 *  able to resist to weird BUS error conditions, but donnot 
 *  ask me for any guarantee that it will never fail. :-)
 *  Use at your own decision and risk.
 */

void sym_interrupt (hcb_p np)
{
	u_char	istat, istatc;
	u_char	dstat;
	u_short	sist;

	/*
	 *  interrupt on the fly ?
	 *  (SCRIPTS may still be running)
	 *
	 *  A `dummy read' is needed to ensure that the 
	 *  clear of the INTF flag reaches the device 
	 *  and that posted writes are flushed to memory
	 *  before the scanning of the DONE queue.
	 *  Note that SCRIPTS also (dummy) read to memory 
	 *  prior to deliver the INTF interrupt condition.
	 */
	istat = INB (nc_istat);
	if (istat & INTF) {
		OUTB (nc_istat, (istat & SIGP) | INTF | np->istat_sem);
		istat = INB (nc_istat);		/* DUMMY READ */
		if (DEBUG_FLAGS & DEBUG_TINY) printf ("F ");
		(void)sym_wakeup_done (np);
	};

	if (!(istat & (SIP|DIP)))
		return;

#if 0	/* We should never get this one */
	if (istat & CABRT)
		OUTB (nc_istat, CABRT);
#endif

	/*
	 *  PAR and MA interrupts may occur at the same time,
	 *  and we need to know of both in order to handle 
	 *  this situation properly. We try to unstack SCSI 
	 *  interrupts for that reason. BTW, I dislike a LOT 
	 *  such a loop inside the interrupt routine.
	 *  Even if DMA interrupt stacking is very unlikely to 
	 *  happen, we also try unstacking these ones, since 
	 *  this has no performance impact.
	 */
	sist	= 0;
	dstat	= 0;
	istatc	= istat;
	do {
		if (istatc & SIP)
			sist  |= INW (nc_sist);
		if (istatc & DIP)
			dstat |= INB (nc_dstat);
		istatc = INB (nc_istat);
		istat |= istatc;
	} while (istatc & (SIP|DIP));

	if (DEBUG_FLAGS & DEBUG_TINY)
		printf ("<%d|%x:%x|%x:%x>",
			(int)INB(nc_scr0),
			dstat,sist,
			(unsigned)INL(nc_dsp),
			(unsigned)INL(nc_dbc));
	/*
	 *  On paper, a memory read barrier may be needed here to 
	 *  prevent out of order LOADs by the CPU from having 
	 *  prefetched stale data prior to DMA having occurred.
	 *  And since we are paranoid ... :)
	 */
	MEMORY_READ_BARRIER();

	/*
	 *  First, interrupts we want to service cleanly.
	 *
	 *  Phase mismatch (MA) is the most frequent interrupt 
	 *  for chip earlier than the 896 and so we have to service 
	 *  it as quickly as possible.
	 *  A SCSI parity error (PAR) may be combined with a phase 
	 *  mismatch condition (MA).
	 *  Programmed interrupts (SIR) are used to call the C code 
	 *  from SCRIPTS.
	 *  The single step interrupt (SSI) is not used in this 
	 *  driver.
	 */
	if (!(sist  & (STO|GEN|HTH|SGE|UDC|SBMC|RST)) &&
	    !(dstat & (MDPE|BF|ABRT|IID))) {
		if	(sist & PAR)	sym_int_par (np, sist);
		else if (sist & MA)	sym_int_ma (np);
		else if (dstat & SIR)	sym_int_sir (np);
		else if (dstat & SSI)	OUTONB_STD ();
		else			goto unknown_int;
		return;
	};

	/*
	 *  Now, interrupts that donnot happen in normal 
	 *  situations and that we may need to recover from.
	 *
	 *  On SCSI RESET (RST), we reset everything.
	 *  On SCSI BUS MODE CHANGE (SBMC), we complete all 
	 *  active CCBs with RESET status, prepare all devices 
	 *  for negotiating again and restart the SCRIPTS.
	 *  On STO and UDC, we complete the CCB with the corres- 
	 *  ponding status and restart the SCRIPTS.
	 */
	if (sist & RST) {
		printf("%s: SCSI BUS reset detected.\n", sym_name(np));
		sym_start_up (np, 1);
		return;
	};

	OUTB (nc_ctest3, np->rv_ctest3 | CLF);	/* clear dma fifo  */
	OUTB (nc_stest3, TE|CSF);		/* clear scsi fifo */

	if (!(sist  & (GEN|HTH|SGE)) &&
	    !(dstat & (MDPE|BF|ABRT|IID))) {
		if	(sist & SBMC)	sym_int_sbmc (np);
		else if (sist & STO)	sym_int_sto (np);
		else if (sist & UDC)	sym_int_udc (np);
		else			goto unknown_int;
		return;
	};

	/*
	 *  Now, interrupts we are not able to recover cleanly.
	 *
	 *  Log message for hard errors.
	 *  Reset everything.
	 */

	sym_log_hard_error(np, sist, dstat);

	if ((sist & (GEN|HTH|SGE)) ||
		(dstat & (MDPE|BF|ABRT|IID))) {
		sym_start_reset(np);
		return;
	};

unknown_int:
	/*
	 *  We just miss the cause of the interrupt. :(
	 *  Print a message. The timeout will do the real work.
	 */
	printf(	"%s: unknown interrupt(s) ignored, "
		"ISTAT=0x%x DSTAT=0x%x SIST=0x%x\n",
		sym_name(np), istat, dstat, sist);
}

/*
 *  Dequeue from the START queue all CCBs that match 
 *  a given target/lun/task condition (-1 means all),
 *  and move them from the BUSY queue to the COMP queue 
 *  with CAM_REQUEUE_REQ status condition.
 *  This function is used during error handling/recovery.
 *  It is called with SCRIPTS not running.
 */
static int 
sym_dequeue_from_squeue(hcb_p np, int i, int target, int lun, int task)
{
	int j;
	ccb_p cp;

	/*
	 *  Make sure the starting index is within range.
	 */
	assert((i >= 0) && (i < 2*MAX_QUEUE));

	/*
	 *  Walk until end of START queue and dequeue every job 
	 *  that matches the target/lun/task condition.
	 */
	j = i;
	while (i != np->squeueput) {
		cp = sym_ccb_from_dsa(np, scr_to_cpu(np->squeue[i]));
		assert(cp);
#ifdef SYM_CONF_IARB_SUPPORT
		/* Forget hints for IARB, they may be no longer relevant */
		cp->host_flags &= ~HF_HINT_IARB;
#endif
		if ((target == -1 || cp->target == target) &&
		    (lun    == -1 || cp->lun    == lun)    &&
		    (task   == -1 || cp->tag    == task)) {
			sym_set_cam_status(cp->cam_ccb, CAM_REQUEUE_REQ);
			sym_remque(&cp->link_ccbq);
			sym_insque_tail(&cp->link_ccbq, &np->comp_ccbq);
		}
		else {
			if (i != j)
				np->squeue[j] = np->squeue[i];
			if ((j += 2) >= MAX_QUEUE*2) j = 0;
		}
		if ((i += 2) >= MAX_QUEUE*2) i = 0;
	}
	if (i != j)		/* Copy back the idle task if needed */
		np->squeue[j] = np->squeue[i];
	np->squeueput = j;	/* Update our current start queue pointer */

	return (i - j) / 2;
}

/*
 *  Complete all CCBs queued to the COMP queue.
 *
 *  These CCBs are assumed:
 *  - Not to be referenced either by devices or 
 *    SCRIPTS-related queues and datas.
 *  - To have to be completed with an error condition 
 *    or requeued.
 *
 *  The device queue freeze count is incremented 
 *  for each CCB that does not prevent this.
 *  This function is called when all CCBs involved 
 *  in error handling/recovery have been reaped.
 */
void sym_flush_comp_queue(hcb_p np, int cam_status)
{
	SYM_QUEHEAD *qp;
	ccb_p cp;

	while ((qp = sym_remque_head(&np->comp_ccbq)) != 0) {
		cam_ccb_p ccb;
		cp = sym_que_entry(qp, struct sym_ccb, link_ccbq);
		sym_insque_tail(&cp->link_ccbq, &np->busy_ccbq);
		/* Leave quiet CCBs waiting for resources */
		if (cp->host_status == HS_WAIT)
			continue;
		ccb = cp->cam_ccb;
		if (cam_status)
			sym_set_cam_status(ccb, cam_status);
#ifdef SYM_OPT_HANDLE_DEVICE_QUEUEING
		if (sym_get_cam_status(ccb) == CAM_REQUEUE_REQ) {
			tcb_p tp = &np->target[cp->target];
			lcb_p lp = sym_lp(np, tp, cp->lun);
			if (lp) {
				sym_remque(&cp->link2_ccbq);
				sym_insque_tail(&cp->link2_ccbq,
				                &lp->waiting_ccbq);
				if (cp->started) {
					if (cp->tag != NO_TAG)
						--lp->started_tags;
					else
						--lp->started_no_tag;
				}
			}
			cp->started = 0;
			continue;
		}
#endif
		sym_free_ccb(np, cp);
		sym_freeze_cam_ccb(ccb);
		sym_xpt_done(np, ccb);
	}
}

/*
 *  chip handler for bad SCSI status condition
 *
 *  In case of bad SCSI status, we unqueue all the tasks 
 *  currently queued to the controller but not yet started 
 *  and then restart the SCRIPTS processor immediately.
 *
 *  QUEUE FULL and BUSY conditions are handled the same way.
 *  Basically all the not yet started tasks are requeued in 
 *  device queue and the queue is frozen until a completion.
 *
 *  For CHECK CONDITION and COMMAND TERMINATED status, we use 
 *  the CCB of the failed command to prepare a REQUEST SENSE 
 *  SCSI command and queue it to the controller queue.
 *
 *  SCRATCHA is assumed to have been loaded with STARTPOS 
 *  before the SCRIPTS called the C code.
 */
static void sym_sir_bad_scsi_status(hcb_p np, int num, ccb_p cp)
{
	tcb_p tp	= &np->target[cp->target];
	u32		startp;
	u_char		s_status = cp->ssss_status;
	u_char		h_flags  = cp->host_flags;
	int		msglen;
	int		nego;
	int		i;

	/*
	 *  Compute the index of the next job to start from SCRIPTS.
	 */
	i = (INL (nc_scratcha) - np->squeue_ba) / 4;

	/*
	 *  The last CCB queued used for IARB hint may be 
	 *  no longer relevant. Forget it.
	 */
#ifdef SYM_CONF_IARB_SUPPORT
	if (np->last_cp)
		np->last_cp = 0;
#endif

	/*
	 *  Now deal with the SCSI status.
	 */
	switch(s_status) {
	case S_BUSY:
	case S_QUEUE_FULL:
		if (sym_verbose >= 2) {
			PRINT_ADDR(cp);
			printf ("%s\n",
			        s_status == S_BUSY ? "BUSY" : "QUEUE FULL\n");
		}
	default:	/* S_INT, S_INT_COND_MET, S_CONFLICT */
		sym_complete_error (np, cp);
		break;
	case S_TERMINATED:
	case S_CHECK_COND:
		/*
		 *  If we get an SCSI error when requesting sense, give up.
		 */
		if (h_flags & HF_SENSE) {
			sym_complete_error (np, cp);
			break;
		}

		/*
		 *  Dequeue all queued CCBs for that device not yet started,
		 *  and restart the SCRIPTS processor immediately.
		 */
		(void) sym_dequeue_from_squeue(np, i, cp->target, cp->lun, -1);
		OUTL_DSP (SCRIPTA_BA (np, start));

 		/*
		 *  Save some info of the actual IO.
		 *  Compute the data residual.
		 */
		cp->sv_scsi_status = cp->ssss_status;
		cp->sv_xerr_status = cp->xerr_status;
		cp->sv_resid = sym_compute_residual(np, cp);

		/*
		 *  Prepare all needed data structures for 
		 *  requesting sense data.
		 */

		/*
		 *  identify message
		 */
		cp->scsi_smsg2[0] = M_IDENTIFY | cp->lun;
		msglen = 1;

		/*
		 *  If we are currently using anything different from 
		 *  async. 8 bit data transfers with that target,
		 *  start a negotiation, since the device may want 
		 *  to report us a UNIT ATTENTION condition due to 
		 *  a cause we currently ignore, and we donnot want 
		 *  to be stuck with WIDE and/or SYNC data transfer.
		 *
		 *  cp->nego_status is filled by sym_prepare_nego().
		 */
		cp->nego_status = 0;
		nego = 0;
		if	(tp->tinfo.curr.options & PPR_OPT_MASK)
			nego = NS_PPR;
		else if	(tp->tinfo.curr.width != BUS_8_BIT)
			nego = NS_WIDE;
		else if (tp->tinfo.curr.offset != 0)
			nego = NS_SYNC;
		if (nego)
			msglen +=
			sym_prepare_nego (np,cp, nego, &cp->scsi_smsg2[msglen]);
		/*
		 *  Message table indirect structure.
		 */
		cp->phys.smsg.addr	= cpu_to_scr(CCB_BA (cp, scsi_smsg2));
		cp->phys.smsg.size	= cpu_to_scr(msglen);

		/*
		 *  sense command
		 */
		cp->phys.cmd.addr	= cpu_to_scr(CCB_BA (cp, sensecmd));
		cp->phys.cmd.size	= cpu_to_scr(6);

		/*
		 *  patch requested size into sense command
		 */
		cp->sensecmd[0]		= REQUEST_SENSE;
		cp->sensecmd[1]		= 0;
		if (tp->tinfo.curr.scsi_version <= 2 && cp->lun <= 7)
			cp->sensecmd[1]	= cp->lun << 5;
		cp->sensecmd[4]		= SYM_SNS_BBUF_LEN;
		cp->data_len		= SYM_SNS_BBUF_LEN;

		/*
		 *  sense data
		 */
		bzero(cp->sns_bbuf, SYM_SNS_BBUF_LEN);
		cp->phys.sense.addr	= cpu_to_scr(vtobus(cp->sns_bbuf));
		cp->phys.sense.size	= cpu_to_scr(SYM_SNS_BBUF_LEN);

		/*
		 *  requeue the command.
		 */
		startp = SCRIPTB_BA (np, sdata_in);

		cp->phys.head.savep	= cpu_to_scr(startp);
		cp->phys.head.lastp	= cpu_to_scr(startp);
		cp->startp		= cpu_to_scr(startp);
		cp->goalp		= cpu_to_scr(startp + 16);

		cp->host_xflags = 0;
		cp->host_status	= cp->nego_status ? HS_NEGOTIATE : HS_BUSY;
		cp->ssss_status = S_ILLEGAL;
		cp->host_flags	= (HF_SENSE|HF_DATA_IN);
		cp->xerr_status = 0;
		cp->extra_bytes = 0;

		cp->phys.head.go.start = cpu_to_scr(SCRIPTA_BA (np, select));

		/*
		 *  Requeue the command.
		 */
		sym_put_start_queue(np, cp);

		/*
		 *  Give back to upper layer everything we have dequeued.
		 */
		sym_flush_comp_queue(np, 0);
		break;
	}
}

/*
 *  After a device has accepted some management message 
 *  as BUS DEVICE RESET, ABORT TASK, etc ..., or when 
 *  a device signals a UNIT ATTENTION condition, some 
 *  tasks are thrown away by the device. We are required 
 *  to reflect that on our tasks list since the device 
 *  will never complete these tasks.
 *
 *  This function move from the BUSY queue to the COMP 
 *  queue all disconnected CCBs for a given target that 
 *  match the following criteria:
 *  - lun=-1  means any logical UNIT otherwise a given one.
 *  - task=-1 means any task, otherwise a given one.
 */
int sym_clear_tasks(hcb_p np, int cam_status, int target, int lun, int task)
{
	SYM_QUEHEAD qtmp, *qp;
	int i = 0;
	ccb_p cp;

	/*
	 *  Move the entire BUSY queue to our temporary queue.
	 */
	sym_que_init(&qtmp);
	sym_que_splice(&np->busy_ccbq, &qtmp);
	sym_que_init(&np->busy_ccbq);

	/*
	 *  Put all CCBs that matches our criteria into 
	 *  the COMP queue and put back other ones into 
	 *  the BUSY queue.
	 */
	while ((qp = sym_remque_head(&qtmp)) != 0) {
		cam_ccb_p ccb;
		cp = sym_que_entry(qp, struct sym_ccb, link_ccbq);
		ccb = cp->cam_ccb;
		if (cp->host_status != HS_DISCONNECT ||
		    cp->target != target	     ||
		    (lun  != -1 && cp->lun != lun)   ||
		    (task != -1 && 
			(cp->tag != NO_TAG && cp->scsi_smsg[2] != task))) {
			sym_insque_tail(&cp->link_ccbq, &np->busy_ccbq);
			continue;
		}
		sym_insque_tail(&cp->link_ccbq, &np->comp_ccbq);

		/* Preserve the software timeout condition */
		if (sym_get_cam_status(ccb) != CAM_CMD_TIMEOUT)
			sym_set_cam_status(ccb, cam_status);
		++i;
#if 0
printf("XXXX TASK @%p CLEARED\n", cp);
#endif
	}
	return i;
}

/*
 *  chip handler for TASKS recovery
 *
 *  We cannot safely abort a command, while the SCRIPTS 
 *  processor is running, since we just would be in race 
 *  with it.
 *
 *  As long as we have tasks to abort, we keep the SEM 
 *  bit set in the ISTAT. When this bit is set, the 
 *  SCRIPTS processor interrupts (SIR_SCRIPT_STOPPED) 
 *  each time it enters the scheduler.
 *
 *  If we have to reset a target, clear tasks of a unit,
 *  or to perform the abort of a disconnected job, we 
 *  restart the SCRIPTS for selecting the target. Once 
 *  selected, the SCRIPTS interrupts (SIR_TARGET_SELECTED).
 *  If it loses arbitration, the SCRIPTS will interrupt again 
 *  the next time it will enter its scheduler, and so on ...
 *
 *  On SIR_TARGET_SELECTED, we scan for the more 
 *  appropriate thing to do:
 *
 *  - If nothing, we just sent a M_ABORT message to the 
 *    target to get rid of the useless SCSI bus ownership.
 *    According to the specs, no tasks shall be affected.
 *  - If the target is to be reset, we send it a M_RESET 
 *    message.
 *  - If a logical UNIT is to be cleared , we send the 
 *    IDENTIFY(lun) + M_ABORT.
 *  - If an untagged task is to be aborted, we send the 
 *    IDENTIFY(lun) + M_ABORT.
 *  - If a tagged task is to be aborted, we send the 
 *    IDENTIFY(lun) + task attributes + M_ABORT_TAG.
 *
 *  Once our 'kiss of death' :) message has been accepted 
 *  by the target, the SCRIPTS interrupts again 
 *  (SIR_ABORT_SENT). On this interrupt, we complete 
 *  all the CCBs that should have been aborted by the 
 *  target according to our message.
 */
static void sym_sir_task_recovery(hcb_p np, int num)
{
	SYM_QUEHEAD *qp;
	ccb_p cp;
	tcb_p tp;
	int target=-1, lun=-1, task;
	int i, k;

	switch(num) {
	/*
	 *  The SCRIPTS processor stopped before starting
	 *  the next command in order to allow us to perform 
	 *  some task recovery.
	 */
	case SIR_SCRIPT_STOPPED:
		/*
		 *  Do we have any target to reset or unit to clear ?
		 */
		for (i = 0 ; i < SYM_CONF_MAX_TARGET ; i++) {
			tp = &np->target[i];
			if (tp->to_reset || 
			    (tp->lun0p && tp->lun0p->to_clear)) {
				target = i;
				break;
			}
			if (!tp->lunmp)
				continue;
			for (k = 1 ; k < SYM_CONF_MAX_LUN ; k++) {
				if (tp->lunmp[k] && tp->lunmp[k]->to_clear) {
					target	= i;
					break;
				}
			}
			if (target != -1)
				break;
		}

		/*
		 *  If not, walk the busy queue for any 
		 *  disconnected CCB to be aborted.
		 */
		if (target == -1) {
			FOR_EACH_QUEUED_ELEMENT(&np->busy_ccbq, qp) {
				cp = sym_que_entry(qp,struct sym_ccb,link_ccbq);
				if (cp->host_status != HS_DISCONNECT)
					continue;
				if (cp->to_abort) {
					target = cp->target;
					break;
				}
			}
		}

		/*
		 *  If some target is to be selected, 
		 *  prepare and start the selection.
		 */
		if (target != -1) {
			tp = &np->target[target];
			np->abrt_sel.sel_id	= target;
			np->abrt_sel.sel_scntl3 = tp->head.wval;
			np->abrt_sel.sel_sxfer  = tp->head.sval;
			OUTL(nc_dsa, np->hcb_ba);
			OUTL_DSP (SCRIPTB_BA (np, sel_for_abort));
			return;
		}

		/*
		 *  Now look for a CCB to abort that haven't started yet.
		 *  Btw, the SCRIPTS processor is still stopped, so 
		 *  we are not in race.
		 */
		i = 0;
		cp = NULL;
		FOR_EACH_QUEUED_ELEMENT(&np->busy_ccbq, qp) {
			cp = sym_que_entry(qp, struct sym_ccb, link_ccbq);
			if (cp->host_status != HS_BUSY &&
			    cp->host_status != HS_NEGOTIATE)
				continue;
			if (!cp->to_abort)
				continue;
#ifdef SYM_CONF_IARB_SUPPORT
			/*
			 *    If we are using IMMEDIATE ARBITRATION, we donnot 
			 *    want to cancel the last queued CCB, since the 
			 *    SCRIPTS may have anticipated the selection.
			 */
			if (cp == np->last_cp) {
				cp->to_abort = 0;
				continue;
			}
#endif
			i = 1;	/* Means we have found some */
			break;
		}
		if (!i) {
			/*
			 *  We are done, so we donnot need 
			 *  to synchronize with the SCRIPTS anylonger.
			 *  Remove the SEM flag from the ISTAT.
			 */
			np->istat_sem = 0;
			OUTB (nc_istat, SIGP);
			break;
		}
		/*
		 *  Compute index of next position in the start 
		 *  queue the SCRIPTS intends to start and dequeue 
		 *  all CCBs for that device that haven't been started.
		 */
		i = (INL (nc_scratcha) - np->squeue_ba) / 4;
		i = sym_dequeue_from_squeue(np, i, cp->target, cp->lun, -1);

		/*
		 *  Make sure at least our IO to abort has been dequeued.
		 */
#ifndef SYM_OPT_HANDLE_DEVICE_QUEUEING
		assert(i && sym_get_cam_status(cp->cam_ccb) == CAM_REQUEUE_REQ);
#else
		sym_remque(&cp->link_ccbq);
		sym_insque_tail(&cp->link_ccbq, &np->comp_ccbq);
#endif
		/*
		 *  Keep track in cam status of the reason of the abort.
		 */
		if (cp->to_abort == 2)
			sym_set_cam_status(cp->cam_ccb, CAM_CMD_TIMEOUT);
		else
			sym_set_cam_status(cp->cam_ccb, CAM_REQ_ABORTED);

		/*
		 *  Complete with error everything that we have dequeued.
	 	 */
		sym_flush_comp_queue(np, 0);
		break;
	/*
	 *  The SCRIPTS processor has selected a target 
	 *  we may have some manual recovery to perform for.
	 */
	case SIR_TARGET_SELECTED:
		target = (INB (nc_sdid) & 0xf);
		tp = &np->target[target];

		np->abrt_tbl.addr = cpu_to_scr(vtobus(np->abrt_msg));

		/*
		 *  If the target is to be reset, prepare a 
		 *  M_RESET message and clear the to_reset flag 
		 *  since we donnot expect this operation to fail.
		 */
		if (tp->to_reset) {
			np->abrt_msg[0] = M_RESET;
			np->abrt_tbl.size = 1;
			tp->to_reset = 0;
			break;
		}

		/*
		 *  Otherwise, look for some logical unit to be cleared.
		 */
		if (tp->lun0p && tp->lun0p->to_clear)
			lun = 0;
		else if (tp->lunmp) {
			for (k = 1 ; k < SYM_CONF_MAX_LUN ; k++) {
				if (tp->lunmp[k] && tp->lunmp[k]->to_clear) {
					lun = k;
					break;
				}
			}
		}

		/*
		 *  If a logical unit is to be cleared, prepare 
		 *  an IDENTIFY(lun) + ABORT MESSAGE.
		 */
		if (lun != -1) {
			lcb_p lp = sym_lp(np, tp, lun);
			lp->to_clear = 0; /* We donnot expect to fail here */
			np->abrt_msg[0] = M_IDENTIFY | lun;
			np->abrt_msg[1] = M_ABORT;
			np->abrt_tbl.size = 2;
			break;
		}

		/*
		 *  Otherwise, look for some disconnected job to 
		 *  abort for this target.
		 */
		i = 0;
		cp = NULL;
		FOR_EACH_QUEUED_ELEMENT(&np->busy_ccbq, qp) {
			cp = sym_que_entry(qp, struct sym_ccb, link_ccbq);
			if (cp->host_status != HS_DISCONNECT)
				continue;
			if (cp->target != target)
				continue;
			if (!cp->to_abort)
				continue;
			i = 1;	/* Means we have some */
			break;
		}

		/*
		 *  If we have none, probably since the device has 
		 *  completed the command before we won abitration,
		 *  send a M_ABORT message without IDENTIFY.
		 *  According to the specs, the device must just 
		 *  disconnect the BUS and not abort any task.
		 */
		if (!i) {
			np->abrt_msg[0] = M_ABORT;
			np->abrt_tbl.size = 1;
			break;
		}

		/*
		 *  We have some task to abort.
		 *  Set the IDENTIFY(lun)
		 */
		np->abrt_msg[0] = M_IDENTIFY | cp->lun;

		/*
		 *  If we want to abort an untagged command, we 
		 *  will send a IDENTIFY + M_ABORT.
		 *  Otherwise (tagged command), we will send 
		 *  a IDENTITFY + task attributes + ABORT TAG.
		 */
		if (cp->tag == NO_TAG) {
			np->abrt_msg[1] = M_ABORT;
			np->abrt_tbl.size = 2;
		}
		else {
			np->abrt_msg[1] = cp->scsi_smsg[1];
			np->abrt_msg[2] = cp->scsi_smsg[2];
			np->abrt_msg[3] = M_ABORT_TAG;
			np->abrt_tbl.size = 4;
		}
		/*
		 *  Keep track of software timeout condition, since the 
		 *  peripheral driver may not count retries on abort 
		 *  conditions not due to timeout.
		 */
		if (cp->to_abort == 2)
			sym_set_cam_status(cp->cam_ccb, CAM_CMD_TIMEOUT);
		cp->to_abort = 0; /* We donnot expect to fail here */
		break;

	/*
	 *  The target has accepted our message and switched 
	 *  to BUS FREE phase as we expected.
	 */
	case SIR_ABORT_SENT:
		target = (INB (nc_sdid) & 0xf);
		tp = &np->target[target];
		
		/*
		**  If we didn't abort anything, leave here.
		*/
		if (np->abrt_msg[0] == M_ABORT)
			break;

		/*
		 *  If we sent a M_RESET, then a hardware reset has 
		 *  been performed by the target.
		 *  - Reset everything to async 8 bit
		 *  - Tell ourself to negotiate next time :-)
		 *  - Prepare to clear all disconnected CCBs for 
		 *    this target from our task list (lun=task=-1)
		 */
		lun = -1;
		task = -1;
		if (np->abrt_msg[0] == M_RESET) {
			tp->head.sval = 0;
			tp->head.wval = np->rv_scntl3;
			tp->head.uval = 0;
			tp->tinfo.curr.period = 0;
			tp->tinfo.curr.offset = 0;
			tp->tinfo.curr.width  = BUS_8_BIT;
			tp->tinfo.curr.options = 0;
		}

		/*
		 *  Otherwise, check for the LUN and TASK(s) 
		 *  concerned by the cancelation.
		 *  If it is not ABORT_TAG then it is CLEAR_QUEUE 
		 *  or an ABORT message :-)
		 */
		else {
			lun = np->abrt_msg[0] & 0x3f;
			if (np->abrt_msg[1] == M_ABORT_TAG)
				task = np->abrt_msg[2];
		}

		/*
		 *  Complete all the CCBs the device should have 
		 *  aborted due to our 'kiss of death' message.
		 */
		i = (INL (nc_scratcha) - np->squeue_ba) / 4;
		(void) sym_dequeue_from_squeue(np, i, target, lun, -1);
		(void) sym_clear_tasks(np, CAM_REQ_ABORTED, target, lun, task);
		sym_flush_comp_queue(np, 0);

 		/*
		 *  If we sent a BDR, make upper layer aware of that.
 		 */
		if (np->abrt_msg[0] == M_RESET)
			sym_xpt_async_sent_bdr(np, target);
		break;
	}

	/*
	 *  Print to the log the message we intend to send.
	 */
	if (num == SIR_TARGET_SELECTED) {
		PRINT_TARGET(np, target);
		sym_printl_hex("control msgout:", np->abrt_msg,
			      np->abrt_tbl.size);
		np->abrt_tbl.size = cpu_to_scr(np->abrt_tbl.size);
	}

	/*
	 *  Let the SCRIPTS processor continue.
	 */
	OUTONB_STD ();
}

/*
 *  Gerard's alchemy:) that deals with with the data 
 *  pointer for both MDP and the residual calculation.
 *
 *  I didn't want to bloat the code by more than 200 
 *  lignes for the handling of both MDP and the residual.
 *  This has been achieved by using a data pointer 
 *  representation consisting in an index in the data 
 *  array (dp_sg) and a negative offset (dp_ofs) that 
 *  have the following meaning:
 *
 *  - dp_sg = SYM_CONF_MAX_SG
 *    we are at the end of the data script.
 *  - dp_sg < SYM_CONF_MAX_SG
 *    dp_sg points to the next entry of the scatter array 
 *    we want to transfer.
 *  - dp_ofs < 0
 *    dp_ofs represents the residual of bytes of the 
 *    previous entry scatter entry we will send first.
 *  - dp_ofs = 0
 *    no residual to send first.
 *
 *  The function sym_evaluate_dp() accepts an arbitray 
 *  offset (basically from the MDP message) and returns 
 *  the corresponding values of dp_sg and dp_ofs.
 */

static int sym_evaluate_dp(hcb_p np, ccb_p cp, u32 scr, int *ofs)
{
	u32	dp_scr;
	int	dp_ofs, dp_sg, dp_sgmin;
	int	tmp;
	struct sym_pmc *pm;

	/*
	 *  Compute the resulted data pointer in term of a script 
	 *  address within some DATA script and a signed byte offset.
	 */
	dp_scr = scr;
	dp_ofs = *ofs;
	if	(dp_scr == SCRIPTA_BA (np, pm0_data))
		pm = &cp->phys.pm0;
	else if (dp_scr == SCRIPTA_BA (np, pm1_data))
		pm = &cp->phys.pm1;
	else
		pm = NULL;

	if (pm) {
		dp_scr  = scr_to_cpu(pm->ret);
		dp_ofs -= scr_to_cpu(pm->sg.size);
	}

	/*
	 *  If we are auto-sensing, then we are done.
	 */
	if (cp->host_flags & HF_SENSE) {
		*ofs = dp_ofs;
		return 0;
	}

	/*
	 *  Deduce the index of the sg entry.
	 *  Keep track of the index of the first valid entry.
	 *  If result is dp_sg = SYM_CONF_MAX_SG, then we are at the 
	 *  end of the data.
	 */
	tmp = scr_to_cpu(sym_goalp(cp));
	dp_sg = SYM_CONF_MAX_SG;
	if (dp_scr != tmp)
		dp_sg -= (tmp - 8 - (int)dp_scr) / (2*4);
	dp_sgmin = SYM_CONF_MAX_SG - cp->segments;

	/*
	 *  Move to the sg entry the data pointer belongs to.
	 *
	 *  If we are inside the data area, we expect result to be:
	 *
	 *  Either,
	 *      dp_ofs = 0 and dp_sg is the index of the sg entry
	 *      the data pointer belongs to (or the end of the data)
	 *  Or,
	 *      dp_ofs < 0 and dp_sg is the index of the sg entry 
	 *      the data pointer belongs to + 1.
	 */
	if (dp_ofs < 0) {
		int n;
		while (dp_sg > dp_sgmin) {
			--dp_sg;
			tmp = scr_to_cpu(cp->phys.data[dp_sg].size);
			n = dp_ofs + (tmp & 0xffffff);
			if (n > 0) {
				++dp_sg;
				break;
			}
			dp_ofs = n;
		}
	}
	else if (dp_ofs > 0) {
		while (dp_sg < SYM_CONF_MAX_SG) {
			tmp = scr_to_cpu(cp->phys.data[dp_sg].size);
			dp_ofs -= (tmp & 0xffffff);
			++dp_sg;
			if (dp_ofs <= 0)
				break;
		}
	}

	/*
	 *  Make sure the data pointer is inside the data area.
	 *  If not, return some error.
	 */
	if	(dp_sg < dp_sgmin || (dp_sg == dp_sgmin && dp_ofs < 0))
		goto out_err;
	else if	(dp_sg > SYM_CONF_MAX_SG ||
		 (dp_sg == SYM_CONF_MAX_SG && dp_ofs > 0))
		goto out_err;

	/*
	 *  Save the extreme pointer if needed.
	 */
	if (dp_sg > cp->ext_sg ||
            (dp_sg == cp->ext_sg && dp_ofs > cp->ext_ofs)) {
		cp->ext_sg  = dp_sg;
		cp->ext_ofs = dp_ofs;
	}

	/*
	 *  Return data.
	 */
	*ofs = dp_ofs;
	return dp_sg;

out_err:
	return -1;
}

/*
 *  chip handler for MODIFY DATA POINTER MESSAGE
 *
 *  We also call this function on IGNORE WIDE RESIDUE 
 *  messages that do not match a SWIDE full condition.
 *  Btw, we assume in that situation that such a message 
 *  is equivalent to a MODIFY DATA POINTER (offset=-1).
 */

static void sym_modify_dp(hcb_p np, tcb_p tp, ccb_p cp, int ofs)
{
	int dp_ofs	= ofs;
	u32	dp_scr	= sym_get_script_dp (np, cp);
	u32	dp_ret;
	u32	tmp;
	u_char	hflags;
	int	dp_sg;
	struct	sym_pmc *pm;

	/*
	 *  Not supported for auto-sense.
	 */
	if (cp->host_flags & HF_SENSE)
		goto out_reject;

	/*
	 *  Apply our alchemy:) (see comments in sym_evaluate_dp()), 
	 *  to the resulted data pointer.
	 */
	dp_sg = sym_evaluate_dp(np, cp, dp_scr, &dp_ofs);
	if (dp_sg < 0)
		goto out_reject;

	/*
	 *  And our alchemy:) allows to easily calculate the data 
	 *  script address we want to return for the next data phase.
	 */
	dp_ret = cpu_to_scr(sym_goalp(cp));
	dp_ret = dp_ret - 8 - (SYM_CONF_MAX_SG - dp_sg) * (2*4);

	/*
	 *  If offset / scatter entry is zero we donnot need 
	 *  a context for the new current data pointer.
	 */
	if (dp_ofs == 0) {
		dp_scr = dp_ret;
		goto out_ok;
	}

	/*
	 *  Get a context for the new current data pointer.
	 */
	hflags = INB (HF_PRT);

	if (hflags & HF_DP_SAVED)
		hflags ^= HF_ACT_PM;

	if (!(hflags & HF_ACT_PM)) {
		pm  = &cp->phys.pm0;
		dp_scr = SCRIPTA_BA (np, pm0_data);
	}
	else {
		pm = &cp->phys.pm1;
		dp_scr = SCRIPTA_BA (np, pm1_data);
	}

	hflags &= ~(HF_DP_SAVED);

	OUTB (HF_PRT, hflags);

	/*
	 *  Set up the new current data pointer.
	 *  ofs < 0 there, and for the next data phase, we 
	 *  want to transfer part of the data of the sg entry 
	 *  corresponding to index dp_sg-1 prior to returning 
	 *  to the main data script.
	 */
	pm->ret = cpu_to_scr(dp_ret);
	tmp  = scr_to_cpu(cp->phys.data[dp_sg-1].addr);
	tmp += scr_to_cpu(cp->phys.data[dp_sg-1].size) + dp_ofs;
	pm->sg.addr = cpu_to_scr(tmp);
	pm->sg.size = cpu_to_scr(-dp_ofs);

out_ok:
	sym_set_script_dp (np, cp, dp_scr);
	OUTL_DSP (SCRIPTA_BA (np, clrack));
	return;

out_reject:
	OUTL_DSP (SCRIPTB_BA (np, msg_bad));
}


/*
 *  chip calculation of the data residual.
 *
 *  As I used to say, the requirement of data residual 
 *  in SCSI is broken, useless and cannot be achieved 
 *  without huge complexity.
 *  But most OSes and even the official CAM require it.
 *  When stupidity happens to be so widely spread inside 
 *  a community, it gets hard to convince.
 *
 *  Anyway, I don't care, since I am not going to use 
 *  any software that considers this data residual as 
 *  a relevant information. :)
 */

int sym_compute_residual(hcb_p np, ccb_p cp)
{
	int dp_sg, dp_sgmin, resid = 0;
	int dp_ofs = 0;

	/*
	 *  Check for some data lost or just thrown away.
	 *  We are not required to be quite accurate in this 
	 *  situation. Btw, if we are odd for output and the 
	 *  device claims some more data, it may well happen 
	 *  than our residual be zero. :-)
	 */
	if (cp->xerr_status & (XE_EXTRA_DATA|XE_SODL_UNRUN|XE_SWIDE_OVRUN)) {
		if (cp->xerr_status & XE_EXTRA_DATA)
			resid -= cp->extra_bytes;
		if (cp->xerr_status & XE_SODL_UNRUN)
			++resid;
		if (cp->xerr_status & XE_SWIDE_OVRUN)
			--resid;
	}

	/*
	 *  If all data has been transferred,
	 *  there is no residual.
	 */
	if (cp->phys.head.lastp == sym_goalp(cp))
		return resid;

	/*
	 *  If no data transfer occurs, or if the data
	 *  pointer is weird, return full residual.
	 */
	if (cp->startp == cp->phys.head.lastp ||
	    sym_evaluate_dp(np, cp, scr_to_cpu(cp->phys.head.lastp),
			    &dp_ofs) < 0) {
		return cp->data_len;
	}

	/*
	 *  If we were auto-sensing, then we are done.
	 */
	if (cp->host_flags & HF_SENSE) {
		return -dp_ofs;
	}

	/*
	 *  We are now full comfortable in the computation 
	 *  of the data residual (2's complement).
	 */
	dp_sgmin = SYM_CONF_MAX_SG - cp->segments;
	resid = -cp->ext_ofs;
	for (dp_sg = cp->ext_sg; dp_sg < SYM_CONF_MAX_SG; ++dp_sg) {
		u_int tmp = scr_to_cpu(cp->phys.data[dp_sg].size);
		resid += (tmp & 0xffffff);
	}

	/*
	 *  Hopefully, the result is not too wrong.
	 */
	return resid;
}

/*
 *  Negotiation for WIDE and SYNCHRONOUS DATA TRANSFER.
 *
 *  When we try to negotiate, we append the negotiation message
 *  to the identify and (maybe) simple tag message.
 *  The host status field is set to HS_NEGOTIATE to mark this
 *  situation.
 *
 *  If the target doesn't answer this message immediately
 *  (as required by the standard), the SIR_NEGO_FAILED interrupt
 *  will be raised eventually.
 *  The handler removes the HS_NEGOTIATE status, and sets the
 *  negotiated value to the default (async / nowide).
 *
 *  If we receive a matching answer immediately, we check it
 *  for validity, and set the values.
 *
 *  If we receive a Reject message immediately, we assume the
 *  negotiation has failed, and fall back to standard values.
 *
 *  If we receive a negotiation message while not in HS_NEGOTIATE
 *  state, it's a target initiated negotiation. We prepare a
 *  (hopefully) valid answer, set our parameters, and send back 
 *  this answer to the target.
 *
 *  If the target doesn't fetch the answer (no message out phase),
 *  we assume the negotiation has failed, and fall back to default
 *  settings (SIR_NEGO_PROTO interrupt).
 *
 *  When we set the values, we adjust them in all ccbs belonging 
 *  to this target, in the controller's register, and in the "phys"
 *  field of the controller's struct sym_hcb.
 */

/*
 *  chip handler for SYNCHRONOUS DATA TRANSFER REQUEST (SDTR) message.
 */
static int  
sym_sync_nego_check(hcb_p np, int req, int target)
{
	tcb_p tp = &np->target[target];
	u_char	chg, ofs, per, fak, div;

	if (DEBUG_FLAGS & DEBUG_NEGO) {
		sym_print_nego_msg(np, target, "sync msgin", np->msgin);
	};

	/*
	 *  Get requested values.
	 */
	chg = 0;
	per = np->msgin[3];
	ofs = np->msgin[4];

	/*
	 *  Check values against our limits.
	 */
	if (ofs) {
		if (ofs > np->maxoffs)
			{chg = 1; ofs = np->maxoffs;}
		if (req) {
			if (ofs > tp->tinfo.user.offset)
				{chg = 1; ofs = tp->tinfo.user.offset;}
		}
	}

	if (ofs) {
		if (per < np->minsync)
			{chg = 1; per = np->minsync;}
		if (req) {
			if (per < tp->tinfo.user.period)
				{chg = 1; per = tp->tinfo.user.period;}
		}
	}

	/*
	 *  Get new chip synchronous parameters value.
	 */
	div = fak = 0;
	if (ofs && sym_getsync(np, 0, per, &div, &fak) < 0)
		goto reject_it;

	if (DEBUG_FLAGS & DEBUG_NEGO) {
		PRINT_TARGET(np, target);
		printf ("sdtr: ofs=%d per=%d div=%d fak=%d chg=%d.\n",
			ofs, per, div, fak, chg);
	}

	/*
	 *  If it was an answer we want to change, 
	 *  then it isn't acceptable. Reject it.
	 */
	if (!req && chg)
		goto reject_it;

	/*
	 *  Apply new values.
	 */
	sym_setsync (np, target, ofs, per, div, fak);

	/*
	 *  It was an answer. We are done.
	 */
	if (!req)
		return 0;

	/*
	 *  It was a request. Prepare an answer message.
	 */
	np->msgout[0] = M_EXTENDED;
	np->msgout[1] = 3;
	np->msgout[2] = M_X_SYNC_REQ;
	np->msgout[3] = per;
	np->msgout[4] = ofs;

	if (DEBUG_FLAGS & DEBUG_NEGO) {
		sym_print_nego_msg(np, target, "sync msgout", np->msgout);
	}

	np->msgin [0] = M_NOOP;

	return 0;

reject_it:
	sym_setsync (np, target, 0, 0, 0, 0);
	return -1;
}

static void sym_sync_nego(hcb_p np, tcb_p tp, ccb_p cp)
{
	int req = 1;
	int result;

	/*
	 *  Request or answer ?
	 */
	if (INB (HS_PRT) == HS_NEGOTIATE) {
		OUTB (HS_PRT, HS_BUSY);
		if (cp->nego_status && cp->nego_status != NS_SYNC)
			goto reject_it;
		req = 0;
	}

	/*
	 *  Check and apply new values.
	 */
	result = sym_sync_nego_check(np, req, cp->target);
	if (result)	/* Not acceptable, reject it */
		goto reject_it;
	if (req) {	/* Was a request, send response. */
		cp->nego_status = NS_SYNC;
		OUTL_DSP (SCRIPTB_BA (np, sdtr_resp));
	}
	else		/* Was a response, we are done. */
		OUTL_DSP (SCRIPTA_BA (np, clrack));
	return;

reject_it:
	OUTL_DSP (SCRIPTB_BA (np, msg_bad));
}

/*
 *  chip handler for PARALLEL PROTOCOL REQUEST (PPR) message.
 */
static int 
sym_ppr_nego_check(hcb_p np, int req, int target)
{
	tcb_p tp = &np->target[target];
	u_char	chg, ofs, per, fak, dt, div, wide;

	if (DEBUG_FLAGS & DEBUG_NEGO) {
		sym_print_nego_msg(np, target, "ppr msgin", np->msgin);
	};

	/*
	 *  Get requested values.
	 */
	chg  = 0;
	per  = np->msgin[3];
	ofs  = np->msgin[5];
	wide = np->msgin[6];
	dt   = np->msgin[7] & PPR_OPT_DT;

	/*
	 *  Check values against our limits.
	 */
	if (wide > np->maxwide) {
		chg = 1;
		wide = np->maxwide;
	}
	if (!wide || !(np->features & FE_ULTRA3))
		dt &= ~PPR_OPT_DT;
	if (req) {
		if (wide > tp->tinfo.user.width)
			{chg = 1; wide = tp->tinfo.user.width;}
	}

	if (!(np->features & FE_U3EN))	/* Broken U3EN bit not supported */
		dt &= ~PPR_OPT_DT;

	if (dt != (np->msgin[7] & PPR_OPT_MASK)) chg = 1;

	if (ofs) {
		if (dt) {
			if (ofs > np->maxoffs_dt)
				{chg = 1; ofs = np->maxoffs_dt;}
		}
		else if (ofs > np->maxoffs)
			{chg = 1; ofs = np->maxoffs;}
		if (req) {
			if (ofs > tp->tinfo.user.offset)
				{chg = 1; ofs = tp->tinfo.user.offset;}
		}
	}

	if (ofs) {
		if (dt) {
			if (per < np->minsync_dt)
				{chg = 1; per = np->minsync_dt;}
		}
		else if (per < np->minsync)
			{chg = 1; per = np->minsync;}
		if (req) {
			if (per < tp->tinfo.user.period)
				{chg = 1; per = tp->tinfo.user.period;}
		}
	}

	/*
	 *  Get new chip synchronous parameters value.
	 */
	div = fak = 0;
	if (ofs && sym_getsync(np, dt, per, &div, &fak) < 0)
		goto reject_it;

	/*
	 *  If it was an answer we want to change, 
	 *  then it isn't acceptable. Reject it.
	 */
	if (!req && chg)
		goto reject_it;

	/*
	 *  Apply new values.
	 */
	sym_setpprot (np, target, dt, ofs, per, wide, div, fak);

	/*
	 *  It was an answer. We are done.
	 */
	if (!req)
		return 0;

	/*
	 *  It was a request. Prepare an answer message.
	 */
	np->msgout[0] = M_EXTENDED;
	np->msgout[1] = 6;
	np->msgout[2] = M_X_PPR_REQ;
	np->msgout[3] = per;
	np->msgout[4] = 0;
	np->msgout[5] = ofs;
	np->msgout[6] = wide;
	np->msgout[7] = dt;

	if (DEBUG_FLAGS & DEBUG_NEGO) {
		sym_print_nego_msg(np, target, "ppr msgout", np->msgout);
	}

	np->msgin [0] = M_NOOP;

	return 0;

reject_it:
	sym_setpprot (np, target, 0, 0, 0, 0, 0, 0);
	/*
	 *  If it is a device response that should result in  
	 *  ST, we may want to try a legacy negotiation later.
	 */
	if (!req && !dt) {
		tp->tinfo.goal.options = 0;
		tp->tinfo.goal.width   = wide;
		tp->tinfo.goal.period  = per;
		tp->tinfo.goal.offset  = ofs;
	}
	return -1;
}

static void sym_ppr_nego(hcb_p np, tcb_p tp, ccb_p cp)
{
	int req = 1;
	int result;

	/*
	 *  Request or answer ?
	 */
	if (INB (HS_PRT) == HS_NEGOTIATE) {
		OUTB (HS_PRT, HS_BUSY);
		if (cp->nego_status && cp->nego_status != NS_PPR)
			goto reject_it;
		req = 0;
	}

	/*
	 *  Check and apply new values.
	 */
	result = sym_ppr_nego_check(np, req, cp->target);
	if (result)	/* Not acceptable, reject it */
		goto reject_it;
	if (req) {	/* Was a request, send response. */
		cp->nego_status = NS_PPR;
		OUTL_DSP (SCRIPTB_BA (np, ppr_resp));
	}
	else		/* Was a response, we are done. */
		OUTL_DSP (SCRIPTA_BA (np, clrack));
	return;

reject_it:
	OUTL_DSP (SCRIPTB_BA (np, msg_bad));
}

/*
 *  chip handler for WIDE DATA TRANSFER REQUEST (WDTR) message.
 */
static int  
sym_wide_nego_check(hcb_p np, int req, int target)
{
	tcb_p tp = &np->target[target];
	u_char	chg, wide;

	if (DEBUG_FLAGS & DEBUG_NEGO) {
		sym_print_nego_msg(np, target, "wide msgin", np->msgin);
	};

	/*
	 *  Get requested values.
	 */
	chg  = 0;
	wide = np->msgin[3];

	/*
	 *  Check values against our limits.
	 */
	if (wide > np->maxwide) {
		chg = 1;
		wide = np->maxwide;
	}
	if (req) {
		if (wide > tp->tinfo.user.width)
			{chg = 1; wide = tp->tinfo.user.width;}
	}

	if (DEBUG_FLAGS & DEBUG_NEGO) {
		PRINT_TARGET(np, target);
		printf ("wdtr: wide=%d chg=%d.\n", wide, chg);
	}

	/*
	 *  If it was an answer we want to change, 
	 *  then it isn't acceptable. Reject it.
	 */
	if (!req && chg)
		goto reject_it;

	/*
	 *  Apply new values.
	 */
	sym_setwide (np, target, wide);

	/*
	 *  It was an answer. We are done.
	 */
	if (!req)
		return 0;

	/*
	 *  It was a request. Prepare an answer message.
	 */
	np->msgout[0] = M_EXTENDED;
	np->msgout[1] = 2;
	np->msgout[2] = M_X_WIDE_REQ;
	np->msgout[3] = wide;

	np->msgin [0] = M_NOOP;

	if (DEBUG_FLAGS & DEBUG_NEGO) {
		sym_print_nego_msg(np, target, "wide msgout", np->msgout);
	}

	return 0;

reject_it:
	return -1;
}

static void sym_wide_nego(hcb_p np, tcb_p tp, ccb_p cp)
{
	int req = 1;
	int result;

	/*
	 *  Request or answer ?
	 */
	if (INB (HS_PRT) == HS_NEGOTIATE) {
		OUTB (HS_PRT, HS_BUSY);
		if (cp->nego_status && cp->nego_status != NS_WIDE)
			goto reject_it;
		req = 0;
	}

	/*
	 *  Check and apply new values.
	 */
	result = sym_wide_nego_check(np, req, cp->target);
	if (result)	/* Not acceptable, reject it */
		goto reject_it;
	if (req) {	/* Was a request, send response. */
		cp->nego_status = NS_WIDE;
		OUTL_DSP (SCRIPTB_BA (np, wdtr_resp));
	}
	else {		/* Was a response. */
		/*
		 * Negotiate for SYNC immediately after WIDE response.
		 * This allows to negotiate for both WIDE and SYNC on 
		 * a single SCSI command (Suggested by Justin Gibbs).
		 */
		if (tp->tinfo.goal.offset) {
			np->msgout[0] = M_EXTENDED;
			np->msgout[1] = 3;
			np->msgout[2] = M_X_SYNC_REQ;
			np->msgout[3] = tp->tinfo.goal.period;
			np->msgout[4] = tp->tinfo.goal.offset;

			if (DEBUG_FLAGS & DEBUG_NEGO) {
				sym_print_nego_msg(np, cp->target,
				                   "sync msgout", np->msgout);
			}

			cp->nego_status = NS_SYNC;
			OUTB (HS_PRT, HS_NEGOTIATE);
			OUTL_DSP (SCRIPTB_BA (np, sdtr_resp));
			return;
		}
		else
			OUTL_DSP (SCRIPTA_BA (np, clrack));
	};

	return;

reject_it:
	OUTL_DSP (SCRIPTB_BA (np, msg_bad));
}

/*
 *  Reset DT, SYNC or WIDE to default settings.
 *
 *  Called when a negotiation does not succeed either 
 *  on rejection or on protocol error.
 *
 *  A target that understands a PPR message should never 
 *  reject it, and messing with it is very unlikely.
 *  So, if a PPR makes problems, we may just want to 
 *  try a legacy negotiation later.
 */
static void sym_nego_default(hcb_p np, tcb_p tp, ccb_p cp)
{
	switch (cp->nego_status) {
	case NS_PPR:
#if 0
		sym_setpprot (np, cp->target, 0, 0, 0, 0, 0, 0);
#else
		tp->tinfo.goal.options = 0;
		if (tp->tinfo.goal.period < np->minsync)
			tp->tinfo.goal.period = np->minsync;
		if (tp->tinfo.goal.offset > np->maxoffs)
			tp->tinfo.goal.offset = np->maxoffs;
#endif
		break;
	case NS_SYNC:
		sym_setsync (np, cp->target, 0, 0, 0, 0);
		break;
	case NS_WIDE:
		sym_setwide (np, cp->target, 0);
		break;
	};
	np->msgin [0] = M_NOOP;
	np->msgout[0] = M_NOOP;
	cp->nego_status = 0;
}

/*
 *  chip handler for MESSAGE REJECT received in response to 
 *  PPR, WIDE or SYNCHRONOUS negotiation.
 */
static void sym_nego_rejected(hcb_p np, tcb_p tp, ccb_p cp)
{
	sym_nego_default(np, tp, cp);
	OUTB (HS_PRT, HS_BUSY);
}

/*
 *  chip exception handler for programmed interrupts.
 */
static void sym_int_sir (hcb_p np)
{
	u_char	num	= INB (nc_dsps);
	u32	dsa	= INL (nc_dsa);
	ccb_p	cp	= sym_ccb_from_dsa(np, dsa);
	u_char	target	= INB (nc_sdid) & 0x0f;
	tcb_p	tp	= &np->target[target];
	int	tmp;

	if (DEBUG_FLAGS & DEBUG_TINY) printf ("I#%d", num);

	switch (num) {
#if   SYM_CONF_DMA_ADDRESSING_MODE == 2
	/*
	 *  SCRIPTS tell us that we may have to update 
	 *  64 bit DMA segment registers.
	 */
	case SIR_DMAP_DIRTY:
		sym_update_dmap_regs(np);
		goto out;
#endif
	/*
	 *  Command has been completed with error condition 
	 *  or has been auto-sensed.
	 */
	case SIR_COMPLETE_ERROR:
		sym_complete_error(np, cp);
		return;
	/*
	 *  The C code is currently trying to recover from something.
	 *  Typically, user want to abort some command.
	 */
	case SIR_SCRIPT_STOPPED:
	case SIR_TARGET_SELECTED:
	case SIR_ABORT_SENT:
		sym_sir_task_recovery(np, num);
		return;
	/*
	 *  The device didn't go to MSG OUT phase after having 
	 *  been selected with ATN. We donnot want to handle 
	 *  that.
	 */
	case SIR_SEL_ATN_NO_MSG_OUT:
		printf ("%s:%d: No MSG OUT phase after selection with ATN.\n",
			sym_name (np), target);
		goto out_stuck;
	/*
	 *  The device didn't switch to MSG IN phase after 
	 *  having reseleted the initiator.
	 */
	case SIR_RESEL_NO_MSG_IN:
		printf ("%s:%d: No MSG IN phase after reselection.\n",
			sym_name (np), target);
		goto out_stuck;
	/*
	 *  After reselection, the device sent a message that wasn't 
	 *  an IDENTIFY.
	 */
	case SIR_RESEL_NO_IDENTIFY:
		printf ("%s:%d: No IDENTIFY after reselection.\n",
			sym_name (np), target);
		goto out_stuck;
	/*
	 *  The device reselected a LUN we donnot know about.
	 */
	case SIR_RESEL_BAD_LUN:
		np->msgout[0] = M_RESET;
		goto out;
	/*
	 *  The device reselected for an untagged nexus and we 
	 *  haven't any.
	 */
	case SIR_RESEL_BAD_I_T_L:
		np->msgout[0] = M_ABORT;
		goto out;
	/*
	 *  The device reselected for a tagged nexus that we donnot 
	 *  have.
	 */
	case SIR_RESEL_BAD_I_T_L_Q:
		np->msgout[0] = M_ABORT_TAG;
		goto out;
	/*
	 *  The SCRIPTS let us know that the device has grabbed 
	 *  our message and will abort the job.
	 */
	case SIR_RESEL_ABORTED:
		np->lastmsg = np->msgout[0];
		np->msgout[0] = M_NOOP;
		printf ("%s:%d: message %x sent on bad reselection.\n",
			sym_name (np), target, np->lastmsg);
		goto out;
	/*
	 *  The SCRIPTS let us know that a message has been 
	 *  successfully sent to the device.
	 */
	case SIR_MSG_OUT_DONE:
		np->lastmsg = np->msgout[0];
		np->msgout[0] = M_NOOP;
		/* Should we really care of that */
		if (np->lastmsg == M_PARITY || np->lastmsg == M_ID_ERROR) {
			if (cp) {
				cp->xerr_status &= ~XE_PARITY_ERR;
				if (!cp->xerr_status)
					OUTOFFB (HF_PRT, HF_EXT_ERR);
			}
		}
		goto out;
	/*
	 *  The device didn't send a GOOD SCSI status.
	 *  We may have some work to do prior to allow 
	 *  the SCRIPTS processor to continue.
	 */
	case SIR_BAD_SCSI_STATUS:
		if (!cp)
			goto out;
		sym_sir_bad_scsi_status(np, num, cp);
		return;
	/*
	 *  We are asked by the SCRIPTS to prepare a 
	 *  REJECT message.
	 */
	case SIR_REJECT_TO_SEND:
		sym_print_msg(cp, "M_REJECT to send for ", np->msgin);
		np->msgout[0] = M_REJECT;
		goto out;
	/*
	 *  We have been ODD at the end of a DATA IN 
	 *  transfer and the device didn't send a 
	 *  IGNORE WIDE RESIDUE message.
	 *  It is a data overrun condition.
	 */
	case SIR_SWIDE_OVERRUN:
		if (cp) {
			OUTONB (HF_PRT, HF_EXT_ERR);
			cp->xerr_status |= XE_SWIDE_OVRUN;
		}
		goto out;
	/*
	 *  We have been ODD at the end of a DATA OUT 
	 *  transfer.
	 *  It is a data underrun condition.
	 */
	case SIR_SODL_UNDERRUN:
		if (cp) {
			OUTONB (HF_PRT, HF_EXT_ERR);
			cp->xerr_status |= XE_SODL_UNRUN;
		}
		goto out;
	/*
	 *  The device wants us to tranfer more data than 
	 *  expected or in the wrong direction.
	 *  The number of extra bytes is in scratcha.
	 *  It is a data overrun condition.
	 */
	case SIR_DATA_OVERRUN:
		if (cp) {
			OUTONB (HF_PRT, HF_EXT_ERR);
			cp->xerr_status |= XE_EXTRA_DATA;
			cp->extra_bytes += INL (nc_scratcha);
		}
		goto out;
	/*
	 *  The device switched to an illegal phase (4/5).
	 */
	case SIR_BAD_PHASE:
		if (cp) {
			OUTONB (HF_PRT, HF_EXT_ERR);
			cp->xerr_status |= XE_BAD_PHASE;
		}
		goto out;
	/*
	 *  We received a message.
	 */
	case SIR_MSG_RECEIVED:
		if (!cp)
			goto out_stuck;
		switch (np->msgin [0]) {
		/*
		 *  We received an extended message.
		 *  We handle MODIFY DATA POINTER, SDTR, WDTR 
		 *  and reject all other extended messages.
		 */
		case M_EXTENDED:
			switch (np->msgin [2]) {
			case M_X_MODIFY_DP:
				if (DEBUG_FLAGS & DEBUG_POINTER)
					sym_print_msg(cp,"modify DP",np->msgin);
				tmp = (np->msgin[3]<<24) + (np->msgin[4]<<16) + 
				      (np->msgin[5]<<8)  + (np->msgin[6]);
				sym_modify_dp(np, tp, cp, tmp);
				return;
			case M_X_SYNC_REQ:
				sym_sync_nego(np, tp, cp);
				return;
			case M_X_PPR_REQ:
				sym_ppr_nego(np, tp, cp);
				return;
			case M_X_WIDE_REQ:
				sym_wide_nego(np, tp, cp);
				return;
			default:
				goto out_reject;
			}
			break;
		/*
		 *  We received a 1/2 byte message not handled from SCRIPTS.
		 *  We are only expecting MESSAGE REJECT and IGNORE WIDE 
		 *  RESIDUE messages that haven't been anticipated by 
		 *  SCRIPTS on SWIDE full condition. Unanticipated IGNORE 
		 *  WIDE RESIDUE messages are aliased as MODIFY DP (-1).
		 */
		case M_IGN_RESIDUE:
			if (DEBUG_FLAGS & DEBUG_POINTER)
				sym_print_msg(cp,"ign wide residue", np->msgin);
			if (cp->host_flags & HF_SENSE)
				OUTL_DSP (SCRIPTA_BA (np, clrack));
			else
				sym_modify_dp(np, tp, cp, -1);
			return;
		case M_REJECT:
			if (INB (HS_PRT) == HS_NEGOTIATE)
				sym_nego_rejected(np, tp, cp);
			else {
				PRINT_ADDR(cp);
				printf ("M_REJECT received (%x:%x).\n",
					scr_to_cpu(np->lastmsg), np->msgout[0]);
			}
			goto out_clrack;
			break;
		default:
			goto out_reject;
		}
		break;
	/*
	 *  We received an unknown message.
	 *  Ignore all MSG IN phases and reject it.
	 */
	case SIR_MSG_WEIRD:
		sym_print_msg(cp, "WEIRD message received", np->msgin);
		OUTL_DSP (SCRIPTB_BA (np, msg_weird));
		return;
	/*
	 *  Negotiation failed.
	 *  Target does not send us the reply.
	 *  Remove the HS_NEGOTIATE status.
	 */
	case SIR_NEGO_FAILED:
		OUTB (HS_PRT, HS_BUSY);
	/*
	 *  Negotiation failed.
	 *  Target does not want answer message.
	 */
	case SIR_NEGO_PROTO:
		sym_nego_default(np, tp, cp);
		goto out;
	};

out:
	OUTONB_STD ();
	return;
out_reject:
	OUTL_DSP (SCRIPTB_BA (np, msg_bad));
	return;
out_clrack:
	OUTL_DSP (SCRIPTA_BA (np, clrack));
	return;
out_stuck:
	return;
}

/*
 *  Acquire a control block
 */
ccb_p sym_get_ccb (hcb_p np, u_char tn, u_char ln, u_char tag_order)
{
	tcb_p tp = &np->target[tn];
	lcb_p lp = sym_lp(np, tp, ln);
	u_short tag = NO_TAG;
	SYM_QUEHEAD *qp;
	ccb_p cp = (ccb_p) 0;

	/*
	 *  Look for a free CCB
	 */
	if (sym_que_empty(&np->free_ccbq))
		(void) sym_alloc_ccb(np);
	qp = sym_remque_head(&np->free_ccbq);
	if (!qp)
		goto out;
	cp = sym_que_entry(qp, struct sym_ccb, link_ccbq);

#ifndef SYM_OPT_HANDLE_DEVICE_QUEUEING
	/*
	 *  If the LCB is not yet available and the LUN
	 *  has been probed ok, try to allocate the LCB.
	 */
	if (!lp && sym_is_bit(tp->lun_map, ln)) {
		lp = sym_alloc_lcb(np, tn, ln);
		if (!lp)
			goto out_free;
	}
#endif

	/*
	 *  If the LCB is not available here, then the 
	 *  logical unit is not yet discovered. For those 
	 *  ones only accept 1 SCSI IO per logical unit, 
	 *  since we cannot allow disconnections.
	 */
	if (!lp) {
		if (!sym_is_bit(tp->busy0_map, ln))
			sym_set_bit(tp->busy0_map, ln);
		else
			goto out_free;
	} else {
		/*
		 *  If we have been asked for a tagged command.
		 */
		if (tag_order) {
			/*
			 *  Debugging purpose.
			 */
#ifndef SYM_OPT_HANDLE_DEVICE_QUEUEING
			assert(lp->busy_itl == 0);
#endif
			/*
			 *  Allocate resources for tags if not yet.
			 */
			if (!lp->cb_tags) {
				sym_alloc_lcb_tags(np, tn, ln);
				if (!lp->cb_tags)
					goto out_free;
			}
			/*
			 *  Get a tag for this SCSI IO and set up
			 *  the CCB bus address for reselection, 
			 *  and count it for this LUN.
			 *  Toggle reselect path to tagged.
			 */
			if (lp->busy_itlq < SYM_CONF_MAX_TASK) {
				tag = lp->cb_tags[lp->ia_tag];
				if (++lp->ia_tag == SYM_CONF_MAX_TASK)
					lp->ia_tag = 0;
				++lp->busy_itlq;
#ifndef SYM_OPT_HANDLE_DEVICE_QUEUEING
				lp->itlq_tbl[tag] = cpu_to_scr(cp->ccb_ba);
				lp->head.resel_sa =
					cpu_to_scr(SCRIPTA_BA (np, resel_tag));
#endif
#ifdef SYM_OPT_LIMIT_COMMAND_REORDERING
				cp->tags_si = lp->tags_si;
				++lp->tags_sum[cp->tags_si];
				++lp->tags_since;
#endif
			}
			else
				goto out_free;
		}
		/*
		 *  This command will not be tagged.
		 *  If we already have either a tagged or untagged 
		 *  one, refuse to overlap this untagged one.
		 */
		else {
			/*
			 *  Debugging purpose.
			 */
#ifndef SYM_OPT_HANDLE_DEVICE_QUEUEING
			assert(lp->busy_itl == 0 && lp->busy_itlq == 0);
#endif
			/*
			 *  Count this nexus for this LUN.
			 *  Set up the CCB bus address for reselection.
			 *  Toggle reselect path to untagged.
			 */
			++lp->busy_itl;
#ifndef SYM_OPT_HANDLE_DEVICE_QUEUEING
			if (lp->busy_itl == 1) {
				lp->head.itl_task_sa = cpu_to_scr(cp->ccb_ba);
				lp->head.resel_sa =
				      cpu_to_scr(SCRIPTA_BA (np, resel_no_tag));
			}
			else
				goto out_free;
#endif
		}
	}
	/*
	 *  Put the CCB into the busy queue.
	 */
	sym_insque_tail(&cp->link_ccbq, &np->busy_ccbq);
#ifdef SYM_OPT_HANDLE_DEVICE_QUEUEING
	if (lp) {
		sym_remque(&cp->link2_ccbq);
		sym_insque_tail(&cp->link2_ccbq, &lp->waiting_ccbq);
	}

#endif
	/*
	 *  Remember all informations needed to free this CCB.
	 */
	cp->to_abort = 0;
	cp->tag	   = tag;
	cp->order  = tag_order;
	cp->target = tn;
	cp->lun    = ln;

	if (DEBUG_FLAGS & DEBUG_TAGS) {
		PRINT_LUN(np, tn, ln);
		printf ("ccb @%p using tag %d.\n", cp, tag);
	}

out:
	return cp;
out_free:
	sym_insque_head(&cp->link_ccbq, &np->free_ccbq);
	return (ccb_p) 0;
}

/*
 *  Release one control block
 */
void sym_free_ccb (hcb_p np, ccb_p cp)
{
	tcb_p tp = &np->target[cp->target];
	lcb_p lp = sym_lp(np, tp, cp->lun);

	if (DEBUG_FLAGS & DEBUG_TAGS) {
		PRINT_LUN(np, cp->target, cp->lun);
		printf ("ccb @%p freeing tag %d.\n", cp, cp->tag);
	}

	/*
	 *  If LCB available,
	 */
	if (lp) {
		/*
		 *  If tagged, release the tag, set the relect path 
		 */
		if (cp->tag != NO_TAG) {
#ifdef SYM_OPT_LIMIT_COMMAND_REORDERING
			--lp->tags_sum[cp->tags_si];
#endif
			/*
			 *  Free the tag value.
			 */
			lp->cb_tags[lp->if_tag] = cp->tag;
			if (++lp->if_tag == SYM_CONF_MAX_TASK)
				lp->if_tag = 0;
			/*
			 *  Make the reselect path invalid, 
			 *  and uncount this CCB.
			 */
			lp->itlq_tbl[cp->tag] = cpu_to_scr(np->bad_itlq_ba);
			--lp->busy_itlq;
		} else {	/* Untagged */
			/*
			 *  Make the reselect path invalid, 
			 *  and uncount this CCB.
			 */
			lp->head.itl_task_sa = cpu_to_scr(np->bad_itl_ba);
			--lp->busy_itl;
		}
		/*
		 *  If no JOB active, make the LUN reselect path invalid.
		 */
		if (lp->busy_itlq == 0 && lp->busy_itl == 0)
			lp->head.resel_sa =
				cpu_to_scr(SCRIPTB_BA (np, resel_bad_lun));
	}
	/*
	 *  Otherwise, we only accept 1 IO per LUN.
	 *  Clear the bit that keeps track of this IO.
	 */
	else
		sym_clr_bit(tp->busy0_map, cp->lun);

	/*
	 *  We donnot queue more than 1 ccb per target 
	 *  with negotiation at any time. If this ccb was 
	 *  used for negotiation, clear this info in the tcb.
	 */
	if (cp == tp->nego_cp)
		tp->nego_cp = NULL;

#ifdef SYM_CONF_IARB_SUPPORT
	/*
	 *  If we just complete the last queued CCB,
	 *  clear this info that is no longer relevant.
	 */
	if (cp == np->last_cp)
		np->last_cp = 0;
#endif

	/*
	 *  Unmap user data from DMA map if needed.
	 */
	sym_data_dmamap_unload(np, cp);

	/*
	 *  Make this CCB available.
	 */
	cp->cam_ccb = NULL;
	cp->host_status = HS_IDLE;
	sym_remque(&cp->link_ccbq);
	sym_insque_head(&cp->link_ccbq, &np->free_ccbq);

#ifdef	SYM_OPT_HANDLE_IO_TIMEOUT
	/*
	 *  Cancel any pending timeout condition.
	 */
	sym_untimeout_ccb(np, cp);
#endif

#ifdef SYM_OPT_HANDLE_DEVICE_QUEUEING
	if (lp) {
		sym_remque(&cp->link2_ccbq);
		sym_insque_tail(&cp->link2_ccbq, &np->dummy_ccbq);
		if (cp->started) {
			if (cp->tag != NO_TAG)
				--lp->started_tags;
			else
				--lp->started_no_tag;
		}
	}
	cp->started = 0;
#endif
}

/*
 *  Allocate a CCB from memory and initialize its fixed part.
 */
static ccb_p sym_alloc_ccb(hcb_p np)
{
	ccb_p cp = NULL;
	int hcode;

	/*
	 *  Prevent from allocating more CCBs than we can 
	 *  queue to the controller.
	 */
	if (np->actccbs >= SYM_CONF_MAX_START)
		return NULL;

	/*
	 *  Allocate memory for this CCB.
	 */
	cp = sym_calloc_dma(sizeof(struct sym_ccb), "CCB");
	if (!cp)
		goto out_free;

	/*
	 *  Allocate a bounce buffer for sense data.
	 */
	cp->sns_bbuf = sym_calloc_dma(SYM_SNS_BBUF_LEN, "SNS_BBUF");
	if (!cp->sns_bbuf)
		goto out_free;

	/*
	 *  Allocate a map for the DMA of user data.
	 */
	if (sym_data_dmamap_create(np, cp))
		goto out_free;

	/*
	 *  Count it.
	 */
	np->actccbs++;

	/*
	 *  Compute the bus address of this ccb.
	 */
	cp->ccb_ba = vtobus(cp);

	/*
	 *  Insert this ccb into the hashed list.
	 */
	hcode = CCB_HASH_CODE(cp->ccb_ba);
	cp->link_ccbh = np->ccbh[hcode];
	np->ccbh[hcode] = cp;

	/*
	 *  Initialyze the start and restart actions.
	 */
	cp->phys.head.go.start   = cpu_to_scr(SCRIPTA_BA (np, idle));
	cp->phys.head.go.restart = cpu_to_scr(SCRIPTB_BA (np, bad_i_t_l));

 	/*
	 *  Initilialyze some other fields.
	 */
	cp->phys.smsg_ext.addr = cpu_to_scr(HCB_BA(np, msgin[2]));

	/*
	 *  Chain into free ccb queue.
	 */
	sym_insque_head(&cp->link_ccbq, &np->free_ccbq);

	/*
	 *  Chain into optionnal lists.
	 */
#ifdef	SYM_OPT_HANDLE_IO_TIMEOUT
	sym_insque_head(&cp->tmo_linkq, &np->tmo0_ccbq);
#endif
#ifdef SYM_OPT_HANDLE_DEVICE_QUEUEING
	sym_insque_head(&cp->link2_ccbq, &np->dummy_ccbq);
#endif
	return cp;
out_free:
	if (cp) {
		if (cp->sns_bbuf)
			sym_mfree_dma(cp->sns_bbuf,SYM_SNS_BBUF_LEN,"SNS_BBUF");
		sym_mfree_dma(cp, sizeof(*cp), "CCB");
	}
	return NULL;
}

/*
 *  Look up a CCB from a DSA value.
 */
static ccb_p sym_ccb_from_dsa(hcb_p np, u32 dsa)
{
	int hcode;
	ccb_p cp;

	hcode = CCB_HASH_CODE(dsa);
	cp = np->ccbh[hcode];
	while (cp) {
		if (cp->ccb_ba == dsa)
			break;
		cp = cp->link_ccbh;
	}

	return cp;
}

/*
 *  Target control block initialisation.
 *  Nothing important to do at the moment.
 */
static void sym_init_tcb (hcb_p np, u_char tn)
{
#if 0	/*  Hmmm... this checking looks paranoid. */
	/*
	 *  Check some alignments required by the chip.
	 */	
	assert (((offsetof(struct sym_reg, nc_sxfer) ^
		offsetof(struct sym_tcb, head.sval)) &3) == 0);
	assert (((offsetof(struct sym_reg, nc_scntl3) ^
		offsetof(struct sym_tcb, head.wval)) &3) == 0);
#endif
}

/*
 *  Lun control block allocation and initialization.
 */
lcb_p sym_alloc_lcb (hcb_p np, u_char tn, u_char ln)
{
	tcb_p tp = &np->target[tn];
	lcb_p lp = sym_lp(np, tp, ln);

	/*
	 *  Already done, just return.
	 */
	if (lp)
		return lp;

	/*
	 *  Donnot allow LUN control block 
	 *  allocation for not probed LUNs.
	 */
	if (!sym_is_bit(tp->lun_map, ln))
		return NULL;

	/*
	 *  Initialize the target control block if not yet.
	 */
	sym_init_tcb (np, tn);

	/*
	 *  Allocate the LCB bus address array.
	 *  Compute the bus address of this table.
	 */
	if (ln && !tp->luntbl) {
		int i;

		tp->luntbl = sym_calloc_dma(256, "LUNTBL");
		if (!tp->luntbl)
			goto fail;
		for (i = 0 ; i < 64 ; i++)
			tp->luntbl[i] = cpu_to_scr(vtobus(&np->badlun_sa));
		tp->head.luntbl_sa = cpu_to_scr(vtobus(tp->luntbl));
	}

	/*
	 *  Allocate the table of pointers for LUN(s) > 0, if needed.
	 */
	if (ln && !tp->lunmp) {
		tp->lunmp = sym_calloc(SYM_CONF_MAX_LUN * sizeof(lcb_p),
				   "LUNMP");
		if (!tp->lunmp)
			goto fail;
	}

	/*
	 *  Allocate the lcb.
	 *  Make it available to the chip.
	 */
	lp = sym_calloc_dma(sizeof(struct sym_lcb), "LCB");
	if (!lp)
		goto fail;
	if (ln) {
		tp->lunmp[ln] = lp;
		tp->luntbl[ln] = cpu_to_scr(vtobus(lp));
	}
	else {
		tp->lun0p = lp;
		tp->head.lun0_sa = cpu_to_scr(vtobus(lp));
	}

	/*
	 *  Let the itl task point to error handling.
	 */
	lp->head.itl_task_sa = cpu_to_scr(np->bad_itl_ba);

	/*
	 *  Set the reselect pattern to our default. :)
	 */
	lp->head.resel_sa = cpu_to_scr(SCRIPTB_BA (np, resel_bad_lun));

	/*
	 *  Set user capabilities.
	 */
	lp->user_flags = tp->usrflags & (SYM_DISC_ENABLED | SYM_TAGS_ENABLED);

#ifdef SYM_OPT_HANDLE_DEVICE_QUEUEING
	/*
	 *  Initialize device queueing.
	 */
	sym_que_init(&lp->waiting_ccbq);
	sym_que_init(&lp->started_ccbq);
	lp->started_max   = SYM_CONF_MAX_TASK;
	lp->started_limit = SYM_CONF_MAX_TASK;
#endif
	/*
	 *  If we are busy, count the IO.
	 */
	if (sym_is_bit(tp->busy0_map, ln)) {
		lp->busy_itl = 1;
		sym_clr_bit(tp->busy0_map, ln);
	}
fail:
	return lp;
}

/*
 *  Allocate LCB resources for tagged command queuing.
 */
static void sym_alloc_lcb_tags (hcb_p np, u_char tn, u_char ln)
{
	tcb_p tp = &np->target[tn];
	lcb_p lp = sym_lp(np, tp, ln);
	int i;

	/*
	 *  If LCB not available, try to allocate it.
	 */
	if (!lp && !(lp = sym_alloc_lcb(np, tn, ln)))
		goto fail;

	/*
	 *  Allocate the task table and and the tag allocation 
	 *  circular buffer. We want both or none.
	 */
	lp->itlq_tbl = sym_calloc_dma(SYM_CONF_MAX_TASK*4, "ITLQ_TBL");
	if (!lp->itlq_tbl)
		goto fail;
	lp->cb_tags = sym_calloc(SYM_CONF_MAX_TASK, "CB_TAGS");
	if (!lp->cb_tags) {
		sym_mfree_dma(lp->itlq_tbl, SYM_CONF_MAX_TASK*4, "ITLQ_TBL");
		lp->itlq_tbl = NULL;
		goto fail;
	}

	/*
	 *  Initialize the task table with invalid entries.
	 */
	for (i = 0 ; i < SYM_CONF_MAX_TASK ; i++)
		lp->itlq_tbl[i] = cpu_to_scr(np->notask_ba);

	/*
	 *  Fill up the tag buffer with tag numbers.
	 */
	for (i = 0 ; i < SYM_CONF_MAX_TASK ; i++)
		lp->cb_tags[i] = i;

	/*
	 *  Make the task table available to SCRIPTS, 
	 *  And accept tagged commands now.
	 */
	lp->head.itlq_tbl_sa = cpu_to_scr(vtobus(lp->itlq_tbl));

	return;
fail:
	return;
}

/*
 *  Queue a SCSI IO to the controller.
 */
int sym_queue_scsiio(hcb_p np, cam_scsiio_p csio, ccb_p cp)
{
	tcb_p	tp;
	lcb_p	lp;
	u_char	idmsg, *msgptr;
	u_int   msglen;

	/*
	 *  Keep track of the IO in our CCB.
	 */
	cp->cam_ccb = (cam_ccb_p) csio;

	/*
	 *  Retreive the target descriptor.
	 */
	tp = &np->target[cp->target];

	/*
	 *  Retreive the lun descriptor.
	 */
	lp = sym_lp(np, tp, cp->lun);

	/*
	 *  Build the IDENTIFY message.
	 */
	idmsg = M_IDENTIFY | cp->lun;
	if (cp->tag != NO_TAG || (lp && (lp->curr_flags & SYM_DISC_ENABLED)))
		idmsg |= 0x40;

	msgptr = cp->scsi_smsg;
	msglen = 0;
	msgptr[msglen++] = idmsg;

	/*
	 *  Build the tag message if present.
	 */
	if (cp->tag != NO_TAG) {
		u_char order = cp->order;

		switch(order) {
		case M_ORDERED_TAG:
			break;
		case M_HEAD_TAG:
			break;
		default:
			order = M_SIMPLE_TAG;
		}
#ifdef SYM_OPT_LIMIT_COMMAND_REORDERING
		/*
		 *  Avoid too much reordering of SCSI commands.
		 *  The algorithm tries to prevent completion of any 
		 *  tagged command from being delayed against more 
		 *  than 3 times the max number of queued commands.
		 */
		if (lp && lp->tags_since > 3*SYM_CONF_MAX_TAG) {
			lp->tags_si = !(lp->tags_si);
			if (lp->tags_sum[lp->tags_si]) {
				order = M_ORDERED_TAG;
				if ((DEBUG_FLAGS & DEBUG_TAGS)||sym_verbose>1) {
					PRINT_ADDR(cp);
					printf("ordered tag forced.\n");
				}
			}
			lp->tags_since = 0;
		}
#endif
		msgptr[msglen++] = order;

		/*
		 *  For less than 128 tags, actual tags are numbered 
		 *  1,3,5,..2*MAXTAGS+1,since we may have to deal 
		 *  with devices that have problems with #TAG 0 or too 
		 *  great #TAG numbers. For more tags (up to 256), 
		 *  we use directly our tag number.
		 */
#if SYM_CONF_MAX_TASK > (512/4)
		msgptr[msglen++] = cp->tag;
#else
		msgptr[msglen++] = (cp->tag << 1) + 1;
#endif
	}

	/*
	 *  Build a negotiation message if needed.
	 *  (nego_status is filled by sym_prepare_nego())
	 */
	cp->nego_status = 0;
	if (tp->tinfo.curr.width   != tp->tinfo.goal.width  ||
	    tp->tinfo.curr.period  != tp->tinfo.goal.period ||
	    tp->tinfo.curr.offset  != tp->tinfo.goal.offset ||
	    tp->tinfo.curr.options != tp->tinfo.goal.options) {
		if (!tp->nego_cp && lp)
			msglen += sym_prepare_nego(np, cp, 0, msgptr + msglen);
	}

	/*
	 *  Startqueue
	 */
	cp->phys.head.go.start   = cpu_to_scr(SCRIPTA_BA (np, select));
	cp->phys.head.go.restart = cpu_to_scr(SCRIPTA_BA (np, resel_dsa));

	/*
	 *  select
	 */
	cp->phys.select.sel_id		= cp->target;
	cp->phys.select.sel_scntl3	= tp->head.wval;
	cp->phys.select.sel_sxfer	= tp->head.sval;
	cp->phys.select.sel_scntl4	= tp->head.uval;

	/*
	 *  message
	 */
	cp->phys.smsg.addr	= cpu_to_scr(CCB_BA (cp, scsi_smsg));
	cp->phys.smsg.size	= cpu_to_scr(msglen);

	/*
	 *  status
	 */
	cp->host_xflags		= 0;
	cp->host_status		= cp->nego_status ? HS_NEGOTIATE : HS_BUSY;
	cp->ssss_status		= S_ILLEGAL;
	cp->xerr_status		= 0;
	cp->host_flags		= 0;
	cp->extra_bytes		= 0;

	/*
	 *  extreme data pointer.
	 *  shall be positive, so -1 is lower than lowest.:)
	 */
	cp->ext_sg  = -1;
	cp->ext_ofs = 0;

	/*
	 *  Build the CDB and DATA descriptor block 
	 *  and start the IO.
	 */
	return sym_setup_data_and_start(np, csio, cp);
}

/*
 *  Reset a SCSI target (all LUNs of this target).
 */
int sym_reset_scsi_target(hcb_p np, int target)
{
	tcb_p tp;

	if (target == np->myaddr || (u_int)target >= SYM_CONF_MAX_TARGET)
		return -1;

	tp = &np->target[target];
	tp->to_reset = 1;

	np->istat_sem = SEM;
	OUTB (nc_istat, SIGP|SEM);

	return 0;
}

/*
 *  Abort a SCSI IO.
 */
int sym_abort_ccb(hcb_p np, ccb_p cp, int timed_out)
{
	/*
	 *  Check that the IO is active.
	 */
	if (!cp || !cp->host_status || cp->host_status == HS_WAIT)
		return -1;

	/*
	 *  If a previous abort didn't succeed in time,
	 *  perform a BUS reset.
	 */
	if (cp->to_abort) {
		sym_reset_scsi_bus(np, 1);
		return 0;
	}

	/*
	 *  Mark the CCB for abort and allow time for.
	 */
	cp->to_abort = timed_out ? 2 : 1;

	/*
	 *  Tell the SCRIPTS processor to stop and synchronize with us.
	 */
	np->istat_sem = SEM;
	OUTB (nc_istat, SIGP|SEM);
	return 0;
}

int sym_abort_scsiio(hcb_p np, cam_ccb_p ccb, int timed_out)
{
	ccb_p cp;
	SYM_QUEHEAD *qp;

	/*
	 *  Look up our CCB control block.
	 */
	cp = NULL;
	FOR_EACH_QUEUED_ELEMENT(&np->busy_ccbq, qp) {
		ccb_p cp2 = sym_que_entry(qp, struct sym_ccb, link_ccbq);
		if (cp2->cam_ccb == ccb) {
			cp = cp2;
			break;
		}
	}

	return sym_abort_ccb(np, cp, timed_out);
}

/*
 *  Complete execution of a SCSI command with extented 
 *  error, SCSI status error, or having been auto-sensed.
 *
 *  The SCRIPTS processor is not running there, so we 
 *  can safely access IO registers and remove JOBs from  
 *  the START queue.
 *  SCRATCHA is assumed to have been loaded with STARTPOS 
 *  before the SCRIPTS called the C code.
 */
void sym_complete_error (hcb_p np, ccb_p cp)
{
	tcb_p tp;
	lcb_p lp;
	int resid;
	int i;

	/*
	 *  Paranoid check. :)
	 */
	if (!cp || !cp->cam_ccb)
		return;

	if (DEBUG_FLAGS & (DEBUG_TINY|DEBUG_RESULT)) {
		printf ("CCB=%lx STAT=%x/%x/%x DEV=%d/%d\n", (unsigned long)cp,
			cp->host_status, cp->ssss_status, cp->host_flags,
			cp->target, cp->lun);
		MDELAY(100);
	}

	/*
	 *  Get target and lun pointers.
	 */
	tp = &np->target[cp->target];
	lp = sym_lp(np, tp, cp->lun);

	/*
	 *  Check for extended errors.
	 */
	if (cp->xerr_status) {
		if (sym_verbose)
			sym_print_xerr(cp, cp->xerr_status);
		if (cp->host_status == HS_COMPLETE)
			cp->host_status = HS_COMP_ERR;
	}

	/*
	 *  Calculate the residual.
	 */
	resid = sym_compute_residual(np, cp);

	if (!SYM_SETUP_RESIDUAL_SUPPORT) {/* If user does not want residuals */
		resid  = 0;		 /* throw them away. :)		    */
		cp->sv_resid = 0;
	}
#ifdef DEBUG_2_0_X
if (resid)
	printf("XXXX RESID= %d - 0x%x\n", resid, resid);
#endif

	/*
	 *  Dequeue all queued CCBs for that device 
	 *  not yet started by SCRIPTS.
	 */
	i = (INL (nc_scratcha) - np->squeue_ba) / 4;
	i = sym_dequeue_from_squeue(np, i, cp->target, cp->lun, -1);

	/*
	 *  Restart the SCRIPTS processor.
	 */
	OUTL_DSP (SCRIPTA_BA (np, start));

#ifdef SYM_OPT_HANDLE_DEVICE_QUEUEING
	if (cp->host_status == HS_COMPLETE &&
	    cp->ssss_status == S_QUEUE_FULL) {
		if (!lp || lp->started_tags - i < 2)
			goto weirdness;
		/*
		 *  Decrease queue depth as needed.
		 */
		lp->started_max = lp->started_tags - i - 1;
		lp->num_sgood = 0;

		if (sym_verbose >= 2) {
			PRINT_LUN(np, cp->target, cp->lun);
			printf(" queue depth is now %d\n", lp->started_max);
		}

		/*
		 *  Repair the CCB.
		 */
		cp->host_status = HS_BUSY;
		cp->ssss_status = S_ILLEGAL;

		/*
		 *  Let's requeue it to device.
		 */
		sym_set_cam_status(cp->cam_ccb, CAM_REQUEUE_REQ);
		goto finish;
	}
weirdness:
#endif
	/*
	 *  Synchronize DMA map if needed.
	 */
	sym_data_dmamap_postsync(np, cp);

	/*
	 *  Build result in CAM ccb.
	 */
	sym_set_cam_result_error(np, cp, resid);

#ifdef SYM_OPT_HANDLE_DEVICE_QUEUEING
finish:
#endif
	/*
	 *  Add this one to the COMP queue.
	 */
	sym_remque(&cp->link_ccbq);
	sym_insque_head(&cp->link_ccbq, &np->comp_ccbq);

	/*
	 *  Complete all those commands with either error 
	 *  or requeue condition.
	 */
	sym_flush_comp_queue(np, 0);

#ifdef SYM_OPT_HANDLE_DEVICE_QUEUEING
	/*
	 *  Donnot start more than 1 command after an error.
	 */
	if (lp)
		sym_start_next_ccbs(np, lp, 1);
#endif
}

/*
 *  Complete execution of a successful SCSI command.
 *
 *  Only successful commands go to the DONE queue, 
 *  since we need to have the SCRIPTS processor 
 *  stopped on any error condition.
 *  The SCRIPTS processor is running while we are 
 *  completing successful commands.
 */
void sym_complete_ok (hcb_p np, ccb_p cp)
{
	tcb_p tp;
	lcb_p lp;
	cam_ccb_p ccb;
	int resid;

	/*
	 *  Paranoid check. :)
	 */
	if (!cp || !cp->cam_ccb)
		return;
	assert (cp->host_status == HS_COMPLETE);

	/*
	 *  Get user command.
	 */
	ccb = cp->cam_ccb;

	/*
	 *  Get target and lun pointers.
	 */
	tp = &np->target[cp->target];
	lp = sym_lp(np, tp, cp->lun);

	/*
	 *  Assume device discovered on first success.
	 */
	if (!lp)
		sym_set_bit(tp->lun_map, cp->lun);

	/*
	 *  If all data have been transferred, given than no
	 *  extended error did occur, there is no residual.
	 */
	resid = 0;
	if (cp->phys.head.lastp != sym_goalp(cp))
		resid = sym_compute_residual(np, cp);

	/*
	 *  Wrong transfer residuals may be worse than just always 
	 *  returning zero. User can disable this feature from 
	 *  sym_conf.h. Residual support is enabled by default.
	 */
	if (!SYM_SETUP_RESIDUAL_SUPPORT)
		resid  = 0;
#ifdef DEBUG_2_0_X
if (resid)
	printf("XXXX RESID= %d - 0x%x\n", resid, resid);
#endif

	/*
	 *  Synchronize DMA map if needed.
	 */
	sym_data_dmamap_postsync(np, cp);

	/*
	 *  Build result in CAM ccb.
	 */
	sym_set_cam_result_ok(np, cp, resid);

#ifdef	SYM_OPT_SNIFF_INQUIRY
	/*
	 *  On standard INQUIRY response (EVPD and CmDt 
	 *  not set), sniff out device capabilities.
	 */
	if (cp->cdb_buf[0] == INQUIRY && !(cp->cdb_buf[1] & 0x3))
		sym_sniff_inquiry(np, cp->cam_ccb, resid);
#endif

#ifdef SYM_OPT_HANDLE_DEVICE_QUEUEING
	/*
	 *  If max number of started ccbs had been reduced,
	 *  increase it if 200 good status received.
	 */
	if (lp && lp->started_max < lp->started_limit) {
		++lp->num_sgood;
		if (lp->num_sgood >= 200) {
			lp->num_sgood = 0;
			++lp->started_max;
			if (sym_verbose >= 2) {
				PRINT_LUN(np, cp->target, cp->lun);
				printf(" queue depth is now %d\n",
				       lp->started_max);
			}
		}
	}
#endif

	/*
	 *  Free our CCB.
	 */
	sym_free_ccb (np, cp);

#ifdef SYM_OPT_HANDLE_DEVICE_QUEUEING
	/*
	 *  Requeue a couple of awaiting scsi commands.
	 */
	if (lp && !sym_que_empty(&lp->waiting_ccbq))
		sym_start_next_ccbs(np, lp, 2);
#endif
	/*
	 *  Complete the command.
	 */
	sym_xpt_done(np, ccb);
}

/*
 *  Soft-attach the controller.
 */
#ifdef SYM_OPT_NVRAM_PRE_READ
int sym_hcb_attach(hcb_p np, struct sym_fw *fw, struct sym_nvram *nvram)
#else
int sym_hcb_attach(hcb_p np, struct sym_fw *fw)
#endif
{
#ifndef SYM_OPT_NVRAM_PRE_READ
	struct sym_nvram nvram_buf, *nvram = &nvram_buf;
#endif
	int i;

	/*
	 *  Get some info about the firmware.
	 */
	np->scripta_sz	 = fw->a_size;
	np->scriptb_sz	 = fw->b_size;
	np->scriptz_sz	 = fw->z_size;
	np->fw_setup	 = fw->setup;
	np->fw_patch	 = fw->patch;
	np->fw_name	 = fw->name;

	/*
	 *  Save setting of some IO registers, so we will 
	 *  be able to probe specific implementations.
	 */
	sym_save_initial_setting (np);

	/*
	 *  Reset the chip now, since it has been reported 
	 *  that SCSI clock calibration may not work properly 
	 *  if the chip is currently active.
	 */
	sym_chip_reset (np);

	/*
	 *  Try to read the user set-up.
	 */
#ifndef SYM_OPT_NVRAM_PRE_READ
	(void) sym_read_nvram(np, nvram);
#endif

	/*
	 *  Prepare controller and devices settings, according 
	 *  to chip features, user set-up and driver set-up.
	 */
	(void) sym_prepare_setting(np, nvram);

	/*
	 *  Check the PCI clock frequency.
	 *  Must be performed after prepare_setting since it destroys 
	 *  STEST1 that is used to probe for the clock doubler.
	 */
	i = sym_getpciclock(np);
	if (i > 37000 && !(np->features & FE_66MHZ))
		printf("%s: PCI BUS clock seems too high: %u KHz.\n",
			sym_name(np), i);

	/*
	 *  Allocate the start queue.
	 */
	np->squeue = (u32 *) sym_calloc_dma(sizeof(u32)*(MAX_QUEUE*2),"SQUEUE");
	if (!np->squeue)
		goto attach_failed;
	np->squeue_ba = vtobus(np->squeue);

	/*
	 *  Allocate the done queue.
	 */
	np->dqueue = (u32 *) sym_calloc_dma(sizeof(u32)*(MAX_QUEUE*2),"DQUEUE");
	if (!np->dqueue)
		goto attach_failed;
	np->dqueue_ba = vtobus(np->dqueue);

	/*
	 *  Allocate the target bus address array.
	 */
	np->targtbl = (u32 *) sym_calloc_dma(256, "TARGTBL");
	if (!np->targtbl)
		goto attach_failed;
	np->targtbl_ba = vtobus(np->targtbl);

	/*
	 *  Allocate SCRIPTS areas.
	 */
	np->scripta0 = sym_calloc_dma(np->scripta_sz, "SCRIPTA0");
	np->scriptb0 = sym_calloc_dma(np->scriptb_sz, "SCRIPTB0");
	np->scriptz0 = sym_calloc_dma(np->scriptz_sz, "SCRIPTZ0");
	if (!np->scripta0 || !np->scriptb0 || !np->scriptz0)
		goto attach_failed;

	/*
	 *  Allocate the array of lists of CCBs hashed by DSA.
	 */
	np->ccbh = sym_calloc(sizeof(ccb_p *)*CCB_HASH_SIZE, "CCBH");
	if (!np->ccbh)
		goto attach_failed;

	/*
	 *  Initialyze the CCB free and busy queues.
	 */
	sym_que_init(&np->free_ccbq);
	sym_que_init(&np->busy_ccbq);
	sym_que_init(&np->comp_ccbq);

	/*
	 *  Initializations for optional handling 
	 *  of IO timeouts and device queueing.
	 */
#ifdef	SYM_OPT_HANDLE_IO_TIMEOUT
	sym_que_init(&np->tmo0_ccbq);
	np->tmo_ccbq =
		sym_calloc(2*SYM_CONF_TIMEOUT_ORDER_MAX*sizeof(SYM_QUEHEAD),
			   "TMO_CCBQ");
	for (i = 0 ; i < 2*SYM_CONF_TIMEOUT_ORDER_MAX ; i++)
		sym_que_init(&np->tmo_ccbq[i]);
#endif
#ifdef SYM_OPT_HANDLE_DEVICE_QUEUEING
	sym_que_init(&np->dummy_ccbq);
#endif
	/*
	 *  Allocate some CCB. We need at least ONE.
	 */
	if (!sym_alloc_ccb(np))
		goto attach_failed;

	/*
	 *  Calculate BUS addresses where we are going 
	 *  to load the SCRIPTS.
	 */
	np->scripta_ba	= vtobus(np->scripta0);
	np->scriptb_ba	= vtobus(np->scriptb0);
	np->scriptz_ba	= vtobus(np->scriptz0);

	if (np->ram_ba) {
		np->scripta_ba	= np->ram_ba;
		if (np->features & FE_RAM8K) {
			np->ram_ws = 8192;
			np->scriptb_ba = np->scripta_ba + 4096;
#if 0	/* May get useful for 64 BIT PCI addressing */
			np->scr_ram_seg = cpu_to_scr(np->scripta_ba >> 32);
#endif
		}
		else
			np->ram_ws = 4096;
	}

	/*
	 *  Copy scripts to controller instance.
	 */
	memcpy(np->scripta0, fw->a_base, np->scripta_sz);
	memcpy(np->scriptb0, fw->b_base, np->scriptb_sz);
	memcpy(np->scriptz0, fw->z_base, np->scriptz_sz);

	/*
	 *  Setup variable parts in scripts and compute
	 *  scripts bus addresses used from the C code.
	 */
	np->fw_setup(np, fw);

	/*
	 *  Bind SCRIPTS with physical addresses usable by the 
	 *  SCRIPTS processor (as seen from the BUS = BUS addresses).
	 */
	sym_fw_bind_script(np, (u32 *) np->scripta0, np->scripta_sz);
	sym_fw_bind_script(np, (u32 *) np->scriptb0, np->scriptb_sz);
	sym_fw_bind_script(np, (u32 *) np->scriptz0, np->scriptz_sz);

#ifdef SYM_CONF_IARB_SUPPORT
	/*
	 *    If user wants IARB to be set when we win arbitration 
	 *    and have other jobs, compute the max number of consecutive 
	 *    settings of IARB hints before we leave devices a chance to 
	 *    arbitrate for reselection.
	 */
#ifdef	SYM_SETUP_IARB_MAX
	np->iarb_max = SYM_SETUP_IARB_MAX;
#else
	np->iarb_max = 4;
#endif
#endif

	/*
	 *  Prepare the idle and invalid task actions.
	 */
	np->idletask.start	= cpu_to_scr(SCRIPTA_BA (np, idle));
	np->idletask.restart	= cpu_to_scr(SCRIPTB_BA (np, bad_i_t_l));
	np->idletask_ba		= vtobus(&np->idletask);

	np->notask.start	= cpu_to_scr(SCRIPTA_BA (np, idle));
	np->notask.restart	= cpu_to_scr(SCRIPTB_BA (np, bad_i_t_l));
	np->notask_ba		= vtobus(&np->notask);

	np->bad_itl.start	= cpu_to_scr(SCRIPTA_BA (np, idle));
	np->bad_itl.restart	= cpu_to_scr(SCRIPTB_BA (np, bad_i_t_l));
	np->bad_itl_ba		= vtobus(&np->bad_itl);

	np->bad_itlq.start	= cpu_to_scr(SCRIPTA_BA (np, idle));
	np->bad_itlq.restart	= cpu_to_scr(SCRIPTB_BA (np,bad_i_t_l_q));
	np->bad_itlq_ba		= vtobus(&np->bad_itlq);

	/*
	 *  Allocate and prepare the lun JUMP table that is used 
	 *  for a target prior the probing of devices (bad lun table).
	 *  A private table will be allocated for the target on the 
	 *  first INQUIRY response received.
	 */
	np->badluntbl = sym_calloc_dma(256, "BADLUNTBL");
	if (!np->badluntbl)
		goto attach_failed;

	np->badlun_sa = cpu_to_scr(SCRIPTB_BA (np, resel_bad_lun));
	for (i = 0 ; i < 64 ; i++)	/* 64 luns/target, no less */
		np->badluntbl[i] = cpu_to_scr(vtobus(&np->badlun_sa));

	/*
	 *  Prepare the bus address array that contains the bus 
	 *  address of each target control block.
	 *  For now, assume all logical units are wrong. :)
	 */
	for (i = 0 ; i < SYM_CONF_MAX_TARGET ; i++) {
		np->targtbl[i] = cpu_to_scr(vtobus(&np->target[i]));
		np->target[i].head.luntbl_sa =
				cpu_to_scr(vtobus(np->badluntbl));
		np->target[i].head.lun0_sa =
				cpu_to_scr(vtobus(&np->badlun_sa));
	}

	/*
	 *  Now check the cache handling of the pci chipset.
	 */
	if (sym_snooptest (np)) {
		printf("%s: CACHE INCORRECTLY CONFIGURED.\n", sym_name(np));
		goto attach_failed;
	};

	/*
	 *  Sigh! we are done.
	 */
	return 0;

attach_failed:
	return -ENXIO;
}

/*
 *  Free everything that has been allocated for this device.
 */
void sym_hcb_free(hcb_p np)
{
	SYM_QUEHEAD *qp;
	ccb_p cp;
	tcb_p tp;
	lcb_p lp;
	int target, lun;

	if (np->scriptz0)
		sym_mfree_dma(np->scriptz0, np->scriptz_sz, "SCRIPTZ0");
	if (np->scriptb0)
		sym_mfree_dma(np->scriptb0, np->scriptb_sz, "SCRIPTB0");
	if (np->scripta0)
		sym_mfree_dma(np->scripta0, np->scripta_sz, "SCRIPTA0");
#ifdef	SYM_OPT_HANDLE_IO_TIMEOUT
	if (np->tmo_ccbq)
		sym_mfree(np->tmo_ccbq,
			  2*SYM_CONF_TIMEOUT_ORDER_MAX*sizeof(SYM_QUEHEAD),
			  "TMO_CCBQ");
#endif
	if (np->squeue)
		sym_mfree_dma(np->squeue, sizeof(u32)*(MAX_QUEUE*2), "SQUEUE");
	if (np->dqueue)
		sym_mfree_dma(np->dqueue, sizeof(u32)*(MAX_QUEUE*2), "DQUEUE");

	if (np->actccbs) {
		while ((qp = sym_remque_head(&np->free_ccbq)) != 0) {
			cp = sym_que_entry(qp, struct sym_ccb, link_ccbq);
			sym_data_dmamap_destroy(np, cp);
			sym_mfree_dma(cp->sns_bbuf, SYM_SNS_BBUF_LEN,
				      "SNS_BBUF");
			sym_mfree_dma(cp, sizeof(*cp), "CCB");
		}
	}
	if (np->ccbh)
		sym_mfree(np->ccbh, sizeof(ccb_p *)*CCB_HASH_SIZE, "CCBH");

	if (np->badluntbl)
		sym_mfree_dma(np->badluntbl, 256,"BADLUNTBL");

	for (target = 0; target < SYM_CONF_MAX_TARGET ; target++) {
		tp = &np->target[target];
		for (lun = 0 ; lun < SYM_CONF_MAX_LUN ; lun++) {
			lp = sym_lp(np, tp, lun);
			if (!lp)
				continue;
			if (lp->itlq_tbl)
				sym_mfree_dma(lp->itlq_tbl, SYM_CONF_MAX_TASK*4,
				       "ITLQ_TBL");
			if (lp->cb_tags)
				sym_mfree(lp->cb_tags, SYM_CONF_MAX_TASK,
				       "CB_TAGS");
			sym_mfree_dma(lp, sizeof(*lp), "LCB");
		}
#if SYM_CONF_MAX_LUN > 1
		if (tp->lunmp)
			sym_mfree(tp->lunmp, SYM_CONF_MAX_LUN*sizeof(lcb_p),
			       "LUNMP");
#endif 
	}
	if (np->targtbl)
		sym_mfree_dma(np->targtbl, 256, "TARGTBL");
}
