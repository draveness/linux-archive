/*
 *   (c) 2003, 2004 Advanced Micro Devices, Inc.
 *  Your use of this code is subject to the terms and conditions of the
 *  GNU general public license version 2. See "COPYING" or
 *  http://www.gnu.org/licenses/gpl.html
 *
 *  Support : paul.devriendt@amd.com
 *
 *  Based on the powernow-k7.c module written by Dave Jones.
 *  (C) 2003 Dave Jones <davej@codemonkey.org.uk> on behalf of SuSE Labs
 *  (C) 2004 Dominik Brodowski <linux@brodo.de>
 *  (C) 2004 Pavel Machek <pavel@suse.cz>
 *  Licensed under the terms of the GNU GPL License version 2.
 *  Based upon datasheets & sample CPUs kindly provided by AMD.
 *
 *  Valuable input gratefully received from Dave Jones, Pavel Machek,
 *  Dominik Brodowski, and others.
 *  Processor information obtained from Chapter 9 (Power and Thermal Management)
 *  of the "BIOS and Kernel Developer's Guide for the AMD Athlon 64 and AMD
 *  Opteron Processors" available for download from www.amd.com
 */

#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <asm/msr.h>
#include <asm/io.h>
#include <asm/delay.h>

#if defined(CONFIG_ACPI_PROCESSOR) || defined(CONFIG_ACPI_PROCESSOR_MODULE)
#include <linux/acpi.h>
#include <acpi/processor.h>
#endif

#define PFX "powernow-k8: "
#define BFX PFX "BIOS error: "
#define VERSION "version 1.00.09b"
#include "powernow-k8.h"

/* serialize freq changes  */
static DECLARE_MUTEX(fidvid_sem);

static struct powernow_k8_data *powernow_data[NR_CPUS];

/* Return a frequency in MHz, given an input fid */
static u32 find_freq_from_fid(u32 fid)
{
	return 800 + (fid * 100);
}

/* Return a frequency in KHz, given an input fid */
static u32 find_khz_freq_from_fid(u32 fid)
{
	return 1000 * find_freq_from_fid(fid);
}

/* Return a voltage in miliVolts, given an input vid */
static u32 find_millivolts_from_vid(struct powernow_k8_data *data, u32 vid)
{
	return 1550-vid*25;
}

/* Return the vco fid for an input fid */
static u32 convert_fid_to_vco_fid(u32 fid)
{
	if (fid < HI_FID_TABLE_BOTTOM) {
		return 8 + (2 * fid);
	} else {
		return fid;
	}
}

/*
 * Return 1 if the pending bit is set. Unless we just instructed the processor
 * to transition to a new state, seeing this bit set is really bad news.
 */
static int pending_bit_stuck(void)
{
	u32 lo, hi;

	rdmsr(MSR_FIDVID_STATUS, lo, hi);
	return lo & MSR_S_LO_CHANGE_PENDING ? 1 : 0;
}

/*
 * Update the global current fid / vid values from the status msr.
 * Returns 1 on error.
 */
static int query_current_values_with_pending_wait(struct powernow_k8_data *data)
{
	u32 lo, hi;
	u32 i = 0;

	lo = MSR_S_LO_CHANGE_PENDING;
	while (lo & MSR_S_LO_CHANGE_PENDING) {
		if (i++ > 0x1000000) {
			printk(KERN_ERR PFX "detected change pending stuck\n");
			return 1;
		}
		rdmsr(MSR_FIDVID_STATUS, lo, hi);
	}

	data->currvid = hi & MSR_S_HI_CURRENT_VID;
	data->currfid = lo & MSR_S_LO_CURRENT_FID;

	return 0;
}

/* the isochronous relief time */
static void count_off_irt(struct powernow_k8_data *data)
{
	udelay((1 << data->irt) * 10);
	return;
}

/* the voltage stabalization time */
static void count_off_vst(struct powernow_k8_data *data)
{
	udelay(data->vstable * VST_UNITS_20US);
	return;
}

/* need to init the control msr to a safe value (for each cpu) */
static void fidvid_msr_init(void)
{
	u32 lo, hi;
	u8 fid, vid;

	rdmsr(MSR_FIDVID_STATUS, lo, hi);
	vid = hi & MSR_S_HI_CURRENT_VID;
	fid = lo & MSR_S_LO_CURRENT_FID;
	lo = fid | (vid << MSR_C_LO_VID_SHIFT);
	hi = MSR_C_HI_STP_GNT_BENIGN;
	dprintk(PFX "cpu%d, init lo 0x%x, hi 0x%x\n", smp_processor_id(), lo, hi);
	wrmsr(MSR_FIDVID_CTL, lo, hi);
}


/* write the new fid value along with the other control fields to the msr */
static int write_new_fid(struct powernow_k8_data *data, u32 fid)
{
	u32 lo;
	u32 savevid = data->currvid;

	if ((fid & INVALID_FID_MASK) || (data->currvid & INVALID_VID_MASK)) {
		printk(KERN_ERR PFX "internal error - overflow on fid write\n");
		return 1;
	}

	lo = fid | (data->currvid << MSR_C_LO_VID_SHIFT) | MSR_C_LO_INIT_FID_VID;

	dprintk(KERN_DEBUG PFX "writing fid 0x%x, lo 0x%x, hi 0x%x\n",
		fid, lo, data->plllock * PLL_LOCK_CONVERSION);

	wrmsr(MSR_FIDVID_CTL, lo, data->plllock * PLL_LOCK_CONVERSION);

	if (query_current_values_with_pending_wait(data))
		return 1;

	count_off_irt(data);

	if (savevid != data->currvid) {
		printk(KERN_ERR PFX "vid change on fid trans, old 0x%x, new 0x%x\n",
		       savevid, data->currvid);
		return 1;
	}

	if (fid != data->currfid) {
		printk(KERN_ERR PFX "fid trans failed, fid 0x%x, curr 0x%x\n", fid,
		        data->currfid);
		return 1;
	}

	return 0;
}

/* Write a new vid to the hardware */
static int write_new_vid(struct powernow_k8_data *data, u32 vid)
{
	u32 lo;
	u32 savefid = data->currfid;

	if ((data->currfid & INVALID_FID_MASK) || (vid & INVALID_VID_MASK)) {
		printk(KERN_ERR PFX "internal error - overflow on vid write\n");
		return 1;
	}

	lo = data->currfid | (vid << MSR_C_LO_VID_SHIFT) | MSR_C_LO_INIT_FID_VID;

	dprintk(KERN_DEBUG PFX "writing vid 0x%x, lo 0x%x, hi 0x%x\n",
		vid, lo, STOP_GRANT_5NS);

	wrmsr(MSR_FIDVID_CTL, lo, STOP_GRANT_5NS);

	if (query_current_values_with_pending_wait(data))
		return 1;

	if (savefid != data->currfid) {
		printk(KERN_ERR PFX "fid changed on vid trans, old 0x%x new 0x%x\n",
		       savefid, data->currfid);
		return 1;
	}

	if (vid != data->currvid) {
		printk(KERN_ERR PFX "vid trans failed, vid 0x%x, curr 0x%x\n", vid,
				data->currvid);
		return 1;
	}

	return 0;
}

/*
 * Reduce the vid by the max of step or reqvid.
 * Decreasing vid codes represent increasing voltages:
 * vid of 0 is 1.550V, vid of 0x1e is 0.800V, vid of 0x1f is off.
 */
static int decrease_vid_code_by_step(struct powernow_k8_data *data, u32 reqvid, u32 step)
{
	if ((data->currvid - reqvid) > step)
		reqvid = data->currvid - step;

	if (write_new_vid(data, reqvid))
		return 1;

	count_off_vst(data);

	return 0;
}

/* Change the fid and vid, by the 3 phases. */
static int transition_fid_vid(struct powernow_k8_data *data, u32 reqfid, u32 reqvid)
{
	if (core_voltage_pre_transition(data, reqvid))
		return 1;

	if (core_frequency_transition(data, reqfid))
		return 1;

	if (core_voltage_post_transition(data, reqvid))
		return 1;

	if (query_current_values_with_pending_wait(data))
		return 1;

	if ((reqfid != data->currfid) || (reqvid != data->currvid)) {
		printk(KERN_ERR PFX "failed (cpu%d): req 0x%x 0x%x, curr 0x%x 0x%x\n",
				smp_processor_id(),
				reqfid, reqvid, data->currfid, data->currvid);
		return 1;
	}

	dprintk(KERN_INFO PFX "transitioned (cpu%d): new fid 0x%x, vid 0x%x\n",
		smp_processor_id(), data->currfid, data->currvid);

	return 0;
}

/* Phase 1 - core voltage transition ... setup voltage */
static int core_voltage_pre_transition(struct powernow_k8_data *data, u32 reqvid)
{
	u32 rvosteps = data->rvo;
	u32 savefid = data->currfid;

	dprintk(KERN_DEBUG PFX
		"ph1 (cpu%d): start, currfid 0x%x, currvid 0x%x, reqvid 0x%x, rvo 0x%x\n",
		smp_processor_id(),
		data->currfid, data->currvid, reqvid, data->rvo);

	while (data->currvid > reqvid) {
		dprintk(KERN_DEBUG PFX "ph1: curr 0x%x, req vid 0x%x\n",
			data->currvid, reqvid);
		if (decrease_vid_code_by_step(data, reqvid, data->vidmvs))
			return 1;
	}

	while (rvosteps > 0) {
		if (data->currvid == 0) {
			rvosteps = 0;
		} else {
			dprintk(KERN_DEBUG PFX
				"ph1: changing vid for rvo, req 0x%x\n",
				data->currvid - 1);
			if (decrease_vid_code_by_step(data, data->currvid - 1, 1))
				return 1;
			rvosteps--;
		}
	}

	if (query_current_values_with_pending_wait(data))
		return 1;

	if (savefid != data->currfid) {
		printk(KERN_ERR PFX "ph1 err, currfid changed 0x%x\n", data->currfid);
		return 1;
	}

	dprintk(KERN_DEBUG PFX "ph1 complete, currfid 0x%x, currvid 0x%x\n",
		data->currfid, data->currvid);

	return 0;
}

/* Phase 2 - core frequency transition */
static int core_frequency_transition(struct powernow_k8_data *data, u32 reqfid)
{
	u32 vcoreqfid;
	u32 vcocurrfid;
	u32 vcofiddiff;
	u32 savevid = data->currvid;

	if ((reqfid < HI_FID_TABLE_BOTTOM) && (data->currfid < HI_FID_TABLE_BOTTOM)) {
		printk(KERN_ERR PFX "ph2: illegal lo-lo transition 0x%x 0x%x\n",
			reqfid, data->currfid);
		return 1;
	}

	if (data->currfid == reqfid) {
		printk(KERN_ERR PFX "ph2 null fid transition 0x%x\n", data->currfid);
		return 0;
	}

	dprintk(KERN_DEBUG PFX
		"ph2 (cpu%d): starting, currfid 0x%x, currvid 0x%x, reqfid 0x%x\n",
		smp_processor_id(),
		data->currfid, data->currvid, reqfid);

	vcoreqfid = convert_fid_to_vco_fid(reqfid);
	vcocurrfid = convert_fid_to_vco_fid(data->currfid);
	vcofiddiff = vcocurrfid > vcoreqfid ? vcocurrfid - vcoreqfid
	    : vcoreqfid - vcocurrfid;

	while (vcofiddiff > 2) {
		if (reqfid > data->currfid) {
			if (data->currfid > LO_FID_TABLE_TOP) {
				if (write_new_fid(data, data->currfid + 2)) {
					return 1;
				}
			} else {
				if (write_new_fid
				    (data, 2 + convert_fid_to_vco_fid(data->currfid))) {
					return 1;
				}
			}
		} else {
			if (write_new_fid(data, data->currfid - 2))
				return 1;
		}

		vcocurrfid = convert_fid_to_vco_fid(data->currfid);
		vcofiddiff = vcocurrfid > vcoreqfid ? vcocurrfid - vcoreqfid
		    : vcoreqfid - vcocurrfid;
	}

	if (write_new_fid(data, reqfid))
		return 1;

	if (query_current_values_with_pending_wait(data))
		return 1;

	if (data->currfid != reqfid) {
		printk(KERN_ERR PFX
			"ph2: mismatch, failed fid transition, curr 0x%x, req 0x%x\n",
			data->currfid, reqfid);
		return 1;
	}

	if (savevid != data->currvid) {
		printk(KERN_ERR PFX "ph2: vid changed, save 0x%x, curr 0x%x\n",
			savevid, data->currvid);
		return 1;
	}

	dprintk(KERN_DEBUG PFX "ph2 complete, currfid 0x%x, currvid 0x%x\n",
		data->currfid, data->currvid);

	return 0;
}

/* Phase 3 - core voltage transition flow ... jump to the final vid. */
static int core_voltage_post_transition(struct powernow_k8_data *data, u32 reqvid)
{
	u32 savefid = data->currfid;
	u32 savereqvid = reqvid;

	dprintk(KERN_DEBUG PFX "ph3 (cpu%d): starting, currfid 0x%x, currvid 0x%x\n",
		smp_processor_id(),
		data->currfid, data->currvid);

	if (reqvid != data->currvid) {
		if (write_new_vid(data, reqvid))
			return 1;

		if (savefid != data->currfid) {
			printk(KERN_ERR PFX
			       "ph3: bad fid change, save 0x%x, curr 0x%x\n",
			       savefid, data->currfid);
			return 1;
		}

		if (data->currvid != reqvid) {
			printk(KERN_ERR PFX
			       "ph3: failed vid transition\n, req 0x%x, curr 0x%x",
			       reqvid, data->currvid);
			return 1;
		}
	}

	if (query_current_values_with_pending_wait(data))
		return 1;

	if (savereqvid != data->currvid) {
		dprintk(KERN_ERR PFX "ph3 failed, currvid 0x%x\n", data->currvid);
		return 1;
	}

	if (savefid != data->currfid) {
		dprintk(KERN_ERR PFX "ph3 failed, currfid changed 0x%x\n",
			data->currfid);
		return 1;
	}

	dprintk(KERN_DEBUG PFX "ph3 complete, currfid 0x%x, currvid 0x%x\n",
		data->currfid, data->currvid);

	return 0;
}

static int check_supported_cpu(unsigned int cpu)
{
	cpumask_t oldmask = CPU_MASK_ALL;
	u32 eax, ebx, ecx, edx;
	unsigned int rc = 0;

	oldmask = current->cpus_allowed;
	set_cpus_allowed(current, cpumask_of_cpu(cpu));
	schedule();

	if (smp_processor_id() != cpu) {
		printk(KERN_ERR "limiting to cpu %u failed\n", cpu);
		goto out;
	}

	if (current_cpu_data.x86_vendor != X86_VENDOR_AMD)
		goto out;

	eax = cpuid_eax(CPUID_PROCESSOR_SIGNATURE);
	if (((eax & CPUID_USE_XFAM_XMOD) != CPUID_USE_XFAM_XMOD) ||
	    ((eax & CPUID_XFAM) != CPUID_XFAM_K8) ||
	    ((eax & CPUID_XMOD) > CPUID_XMOD_REV_E)) {
		printk(KERN_INFO PFX "Processor cpuid %x not supported\n", eax);
		goto out;
	}

	eax = cpuid_eax(CPUID_GET_MAX_CAPABILITIES);
	if (eax < CPUID_FREQ_VOLT_CAPABILITIES) {
		printk(KERN_INFO PFX
		       "No frequency change capabilities detected\n");
		goto out;
	}

	cpuid(CPUID_FREQ_VOLT_CAPABILITIES, &eax, &ebx, &ecx, &edx);
	if ((edx & P_STATE_TRANSITION_CAPABLE) != P_STATE_TRANSITION_CAPABLE) {
		printk(KERN_INFO PFX "Power state transitions not supported\n");
		goto out;
	}

	rc = 1;

out:
	set_cpus_allowed(current, oldmask);
	schedule();
	return rc;

}

static int check_pst_table(struct powernow_k8_data *data, struct pst_s *pst, u8 maxvid)
{
	unsigned int j;
	u8 lastfid = 0xff;

	for (j = 0; j < data->numps; j++) {
		if (pst[j].vid > LEAST_VID) {
			printk(KERN_ERR PFX "vid %d invalid : 0x%x\n", j, pst[j].vid);
			return -EINVAL;
		}
		if (pst[j].vid < data->rvo) {	/* vid + rvo >= 0 */
			printk(KERN_ERR BFX "0 vid exceeded with pstate %d\n", j);
			return -ENODEV;
		}
		if (pst[j].vid < maxvid + data->rvo) {	/* vid + rvo >= maxvid */
			printk(KERN_ERR BFX "maxvid exceeded with pstate %d\n", j);
			return -ENODEV;
		}
		if ((pst[j].fid > MAX_FID)
		    || (pst[j].fid & 1)
		    || (j && (pst[j].fid < HI_FID_TABLE_BOTTOM))) {
			/* Only first fid is allowed to be in "low" range */
			printk(KERN_ERR PFX "fid %d invalid : 0x%x\n", j, pst[j].fid);
			return -EINVAL;
		}
		if (pst[j].fid < lastfid)
			lastfid = pst[j].fid;
	}
	if (lastfid & 1) {
		printk(KERN_ERR PFX "lastfid invalid\n");
		return -EINVAL;
	}
	if (lastfid > LO_FID_TABLE_TOP)
		printk(KERN_INFO PFX  "first fid not from lo freq table\n");

	return 0;
}

static void print_basics(struct powernow_k8_data *data)
{
	int j;
	for (j = 0; j < data->numps; j++) {
		if (data->powernow_table[j].frequency != CPUFREQ_ENTRY_INVALID)
			printk(KERN_INFO PFX "   %d : fid 0x%x (%d MHz), vid 0x%x (%d mV)\n", j,
				data->powernow_table[j].index & 0xff,
				data->powernow_table[j].frequency/1000,
				data->powernow_table[j].index >> 8,
				find_millivolts_from_vid(data, data->powernow_table[j].index >> 8));
	}
	if (data->batps)
		printk(KERN_INFO PFX "Only %d pstates on battery\n", data->batps);
}

static int fill_powernow_table(struct powernow_k8_data *data, struct pst_s *pst, u8 maxvid)
{
	struct cpufreq_frequency_table *powernow_table;
	unsigned int j;

	if (data->batps) {    /* use ACPI support to get full speed on mains power */
		printk(KERN_WARNING PFX "Only %d pstates usable (use ACPI driver for full range\n", data->batps);
		data->numps = data->batps;
	}

	for ( j=1; j<data->numps; j++ ) {
		if (pst[j-1].fid >= pst[j].fid) {
			printk(KERN_ERR PFX "PST out of sequence\n");
			return -EINVAL;
		}
	}

	if (data->numps < 2) {
		printk(KERN_ERR PFX "no p states to transition\n");
		return -ENODEV;
	}

	if (check_pst_table(data, pst, maxvid))
		return -EINVAL;

	powernow_table = kmalloc((sizeof(struct cpufreq_frequency_table)
		* (data->numps + 1)), GFP_KERNEL);
	if (!powernow_table) {
		printk(KERN_ERR PFX "powernow_table memory alloc failure\n");
		return -ENOMEM;
	}

	for (j = 0; j < data->numps; j++) {
		powernow_table[j].index = pst[j].fid; /* lower 8 bits */
		powernow_table[j].index |= (pst[j].vid << 8); /* upper 8 bits */
		powernow_table[j].frequency = find_khz_freq_from_fid(pst[j].fid);
	}
	powernow_table[data->numps].frequency = CPUFREQ_TABLE_END;
	powernow_table[data->numps].index = 0;

	if (query_current_values_with_pending_wait(data)) {
		kfree(powernow_table);
		return -EIO;
	}

	dprintk(KERN_INFO PFX "cfid 0x%x, cvid 0x%x\n", data->currfid, data->currvid);
	data->powernow_table = powernow_table;
	print_basics(data);

	for (j = 0; j < data->numps; j++)
		if ((pst[j].fid==data->currfid) && (pst[j].vid==data->currvid))
			return 0;

	dprintk(KERN_ERR PFX "currfid/vid do not match PST, ignoring\n");
	return 0;
}

/* Find and validate the PSB/PST table in BIOS. */
static int find_psb_table(struct powernow_k8_data *data)
{
	struct psb_s *psb;
	unsigned int i;
	u32 mvs;
	u8 maxvid;

	for (i = 0xc0000; i < 0xffff0; i += 0x10) {
		/* Scan BIOS looking for the signature. */
		/* It can not be at ffff0 - it is too big. */

		psb = phys_to_virt(i);
		if (memcmp(psb, PSB_ID_STRING, PSB_ID_STRING_LEN) != 0)
			continue;

		dprintk(KERN_DEBUG PFX "found PSB header at 0x%p\n", psb);

		dprintk(KERN_DEBUG PFX "table vers: 0x%x\n", psb->tableversion);
		if (psb->tableversion != PSB_VERSION_1_4) {
			printk(KERN_INFO BFX "PSB table is not v1.4\n");
			return -ENODEV;
		}

		dprintk(KERN_DEBUG PFX "flags: 0x%x\n", psb->flags1);
		if (psb->flags1) {
			printk(KERN_ERR BFX "unknown flags\n");
			return -ENODEV;
		}

		data->vstable = psb->voltagestabilizationtime;
		dprintk(KERN_INFO PFX "voltage stabilization time: %d(*20us)\n", data->vstable);

		dprintk(KERN_DEBUG PFX "flags2: 0x%x\n", psb->flags2);
		data->rvo = psb->flags2 & 3;
		data->irt = ((psb->flags2) >> 2) & 3;
		mvs = ((psb->flags2) >> 4) & 3;
		data->vidmvs = 1 << mvs;
		data->batps = ((psb->flags2) >> 6) & 3;

		dprintk(KERN_INFO PFX "ramp voltage offset: %d\n", data->rvo);
		dprintk(KERN_INFO PFX "isochronous relief time: %d\n", data->irt);
		dprintk(KERN_INFO PFX "maximum voltage step: %d - 0x%x\n", mvs, data->vidmvs);

		dprintk(KERN_DEBUG PFX "numpst: 0x%x\n", psb->numpst);
		if (psb->numpst != 1) {
			printk(KERN_ERR BFX "numpst must be 1\n");
			return -ENODEV;
		}

		data->plllock = psb->plllocktime;
		dprintk(KERN_INFO PFX "plllocktime: 0x%x (units 1us)\n", psb->plllocktime);
		dprintk(KERN_INFO PFX "maxfid: 0x%x\n", psb->maxfid);
		dprintk(KERN_INFO PFX "maxvid: 0x%x\n", psb->maxvid);
		maxvid = psb->maxvid;

		data->numps = psb->numpstates;
		dprintk(KERN_INFO PFX "numpstates: 0x%x\n", data->numps);
		return fill_powernow_table(data, (struct pst_s *)(psb+1), maxvid);
	}
	/*
	 * If you see this message, complain to BIOS manufacturer. If
	 * he tells you "we do not support Linux" or some similar
	 * nonsense, remember that Windows 2000 uses the same legacy
	 * mechanism that the old Linux PSB driver uses. Tell them it
	 * is broken with Windows 2000.
	 *
	 * The reference to the AMD documentation is chapter 9 in the
	 * BIOS and Kernel Developer's Guide, which is available on
	 * www.amd.com
	 */
	printk(KERN_ERR PFX "BIOS error - no PSB\n");
	return -ENODEV;
}

#if defined(CONFIG_ACPI_PROCESSOR) || defined(CONFIG_ACPI_PROCESSOR_MODULE)
static void powernow_k8_acpi_pst_values(struct powernow_k8_data *data, unsigned int index)
{
	if (!data->acpi_data.state_count)
		return;

	data->irt = (data->acpi_data.states[index].control >> IRT_SHIFT) & IRT_MASK;
	data->rvo = (data->acpi_data.states[index].control >> RVO_SHIFT) & RVO_MASK;
	data->plllock = (data->acpi_data.states[index].control >> PLL_L_SHIFT) & PLL_L_MASK;
	data->vidmvs = 1 << ((data->acpi_data.states[index].control >> MVS_SHIFT) & MVS_MASK);
	data->vstable = (data->acpi_data.states[index].control >> VST_SHIFT) & VST_MASK;
}

static int powernow_k8_cpu_init_acpi(struct powernow_k8_data *data)
{
	int i;
	int cntlofreq = 0;
	struct cpufreq_frequency_table *powernow_table;

	if (acpi_processor_register_performance(&data->acpi_data, data->cpu)) {
		dprintk(KERN_DEBUG PFX "register performance failed\n");
		return -EIO;
	}

	/* verify the data contained in the ACPI structures */
	if (data->acpi_data.state_count <= 1) {
		dprintk(KERN_DEBUG PFX "No ACPI P-States\n");
		goto err_out;
	}

	if ((data->acpi_data.control_register.space_id != ACPI_ADR_SPACE_FIXED_HARDWARE) ||
		(data->acpi_data.status_register.space_id != ACPI_ADR_SPACE_FIXED_HARDWARE)) {
		dprintk(KERN_DEBUG PFX "Invalid control/status registers\n");
		goto err_out;
	}

	/* fill in data->powernow_table */
	powernow_table = kmalloc((sizeof(struct cpufreq_frequency_table)
		* (data->acpi_data.state_count + 1)), GFP_KERNEL);
	if (!powernow_table) {
		dprintk(KERN_ERR PFX "powernow_table memory alloc failure\n");
		goto err_out;
	}

	for (i = 0; i < data->acpi_data.state_count; i++) {
		u32 fid = data->acpi_data.states[i].control & FID_MASK;
		u32 vid = (data->acpi_data.states[i].control >> VID_SHIFT) & VID_MASK;

		dprintk(KERN_INFO PFX "   %d : fid 0x%x, vid 0x%x\n", i, fid, vid);

		powernow_table[i].index = fid; /* lower 8 bits */
		powernow_table[i].index |= (vid << 8); /* upper 8 bits */
		powernow_table[i].frequency = find_khz_freq_from_fid(fid);

		/* verify frequency is OK */
		if ((powernow_table[i].frequency > (MAX_FREQ * 1000)) ||
			(powernow_table[i].frequency < (MIN_FREQ * 1000))) {
			dprintk(KERN_INFO PFX "invalid freq %u kHz, ignoring\n", powernow_table[i].frequency);
			powernow_table[i].frequency = CPUFREQ_ENTRY_INVALID;
			continue;
		}

		/* verify voltage is OK - BIOSs are using "off" to indicate invalid */
		if (vid == 0x1f) {
			dprintk(KERN_INFO PFX "invalid vid %u, ignoring\n", vid);
			powernow_table[i].frequency = CPUFREQ_ENTRY_INVALID;
			continue;
		}

 		if (fid < HI_FID_TABLE_BOTTOM) {
 			if (cntlofreq) {
 				/* if both entries are the same, ignore this
 				 * one... 
 				 */
 				if ((powernow_table[i].frequency != powernow_table[cntlofreq].frequency) ||
 				    (powernow_table[i].index != powernow_table[cntlofreq].index)) {
 					printk(KERN_ERR PFX "Too many lo freq table entries\n");
 					goto err_out_mem;
 				}
				
 				dprintk(KERN_INFO PFX "double low frequency table entry, ignoring it.\n");
 				powernow_table[i].frequency = CPUFREQ_ENTRY_INVALID;
 				continue;
 			} else
 				cntlofreq = i;
		}

		if (powernow_table[i].frequency != (data->acpi_data.states[i].core_frequency * 1000)) {
			printk(KERN_INFO PFX "invalid freq entries %u kHz vs. %u kHz\n",
				powernow_table[i].frequency,
				(unsigned int) (data->acpi_data.states[i].core_frequency * 1000));
			powernow_table[i].frequency = CPUFREQ_ENTRY_INVALID;
			continue;
		}
	}

	powernow_table[data->acpi_data.state_count].frequency = CPUFREQ_TABLE_END;
	powernow_table[data->acpi_data.state_count].index = 0;
	data->powernow_table = powernow_table;

	/* fill in data */
	data->numps = data->acpi_data.state_count;
	print_basics(data);
	powernow_k8_acpi_pst_values(data, 0);
	return 0;

err_out_mem:
	kfree(powernow_table);

err_out:
	acpi_processor_unregister_performance(&data->acpi_data, data->cpu);

	/* data->acpi_data.state_count informs us at ->exit() whether ACPI was used */
	data->acpi_data.state_count = 0;

	return -ENODEV;
}

static void powernow_k8_cpu_exit_acpi(struct powernow_k8_data *data)
{
	if (data->acpi_data.state_count)
		acpi_processor_unregister_performance(&data->acpi_data, data->cpu);
}

#else
static int powernow_k8_cpu_init_acpi(struct powernow_k8_data *data) { return -ENODEV; }
static void powernow_k8_cpu_exit_acpi(struct powernow_k8_data *data) { return; }
static void powernow_k8_acpi_pst_values(struct powernow_k8_data *data, unsigned int index) { return; }
#endif /* CONFIG_X86_POWERNOW_K8_ACPI */

/* Take a frequency, and issue the fid/vid transition command */
static int transition_frequency(struct powernow_k8_data *data, unsigned int index)
{
	u32 fid;
	u32 vid;
	int res;
	struct cpufreq_freqs freqs;

	dprintk(KERN_DEBUG PFX "cpu %d transition to index %u\n",
		smp_processor_id(), index );

	/* fid are the lower 8 bits of the index we stored into
	 * the cpufreq frequency table in find_psb_table, vid are 
	 * the upper 8 bits.
	 */

	fid = data->powernow_table[index].index & 0xFF;
	vid = (data->powernow_table[index].index & 0xFF00) >> 8;

	dprintk(KERN_DEBUG PFX "table matched fid 0x%x, giving vid 0x%x\n",
		fid, vid);

	if (query_current_values_with_pending_wait(data))
		return 1;

	if ((data->currvid == vid) && (data->currfid == fid)) {
		dprintk(KERN_DEBUG PFX
			"target matches current values (fid 0x%x, vid 0x%x)\n",
			fid, vid);
		return 0;
	}

	if ((fid < HI_FID_TABLE_BOTTOM) && (data->currfid < HI_FID_TABLE_BOTTOM)) {
		printk(KERN_ERR PFX
		       "ignoring illegal change in lo freq table-%x to 0x%x\n",
		       data->currfid, fid);
		return 1;
	}

	dprintk(KERN_DEBUG PFX "cpu %d, changing to fid 0x%x, vid 0x%x\n",
				smp_processor_id(), fid, vid);

	freqs.cpu = data->cpu;

	freqs.old = find_khz_freq_from_fid(data->currfid);
	freqs.new = find_khz_freq_from_fid(fid);
	cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);

	down(&fidvid_sem);
	res = transition_fid_vid(data, fid, vid);
	up(&fidvid_sem);

	freqs.new = find_khz_freq_from_fid(data->currfid);
	cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);

	return res;
}

/* Driver entry point to switch to the target frequency */
static int powernowk8_target(struct cpufreq_policy *pol, unsigned targfreq, unsigned relation)
{
	cpumask_t oldmask = CPU_MASK_ALL;
	struct powernow_k8_data *data = powernow_data[pol->cpu];
	u32 checkfid = data->currfid;
	u32 checkvid = data->currvid;
	unsigned int newstate;
	int ret = -EIO;

	/* only run on specific CPU from here on */
	oldmask = current->cpus_allowed;
	set_cpus_allowed(current, cpumask_of_cpu(pol->cpu));
	schedule();

	if (smp_processor_id() != pol->cpu) {
		printk(KERN_ERR "limiting to cpu %u failed\n", pol->cpu);
		goto err_out;
	}

	if (pending_bit_stuck()) {
		printk(KERN_ERR PFX "failing targ, change pending bit set\n");
		goto err_out;
	}

	dprintk(KERN_DEBUG PFX "targ: cpu %d, %d kHz, min %d, max %d, relation %d\n",
		pol->cpu, targfreq, pol->min, pol->max, relation);

	if (query_current_values_with_pending_wait(data)) {
		ret = -EIO;
		goto err_out;
	}

	dprintk(KERN_DEBUG PFX "targ: curr fid 0x%x, vid 0x%x\n",
		data->currfid, data->currvid);

	if ((checkvid != data->currvid) || (checkfid != data->currfid)) {
		printk(KERN_ERR PFX
		       "error - out of sync, fid 0x%x 0x%x, vid 0x%x 0x%x\n",
		       checkfid, data->currfid, checkvid, data->currvid);
	}

	if (cpufreq_frequency_table_target(pol, data->powernow_table, targfreq, relation, &newstate))
		goto err_out;

	powernow_k8_acpi_pst_values(data, newstate);

	if (transition_frequency(data, newstate)) {
		printk(KERN_ERR PFX "transition frequency failed\n");
		ret = 1;
		goto err_out;
	}

	pol->cur = find_khz_freq_from_fid(data->currfid);
	ret = 0;

err_out:
	set_cpus_allowed(current, oldmask);
	schedule();

	return ret;
}

/* Driver entry point to verify the policy and range of frequencies */
static int powernowk8_verify(struct cpufreq_policy *pol)
{
	struct powernow_k8_data *data = powernow_data[pol->cpu];

	return cpufreq_frequency_table_verify(pol, data->powernow_table);
}

/* per CPU init entry point to the driver */
static int __init powernowk8_cpu_init(struct cpufreq_policy *pol)
{
	struct powernow_k8_data *data;
	cpumask_t oldmask = CPU_MASK_ALL;
	int rc;

	if (!check_supported_cpu(pol->cpu))
		return -ENODEV;

	data = kmalloc(sizeof(struct powernow_k8_data), GFP_KERNEL);
	if (!data) {
		printk(KERN_ERR PFX "unable to alloc powernow_k8_data");
		return -ENOMEM;
	}
	memset(data,0,sizeof(struct powernow_k8_data));

	data->cpu = pol->cpu;

	if (powernow_k8_cpu_init_acpi(data)) {
		/*
		 * Use the PSB BIOS structure. This is only availabe on
		 * an UP version, and is deprecated by AMD.
		 */

		if (pol->cpu != 0) {
			printk(KERN_ERR PFX "init not cpu 0\n");
			kfree(data);
			return -ENODEV;
		}
		if ((num_online_cpus() != 1) || (num_possible_cpus() != 1)) {
			printk(KERN_INFO PFX "MP systems not supported by PSB BIOS structure\n");
			kfree(data);
			return -ENODEV;
		}
		rc = find_psb_table(data);
		if (rc) {
			kfree(data);
			return -ENODEV;
		}
	}

	/* only run on specific CPU from here on */
	oldmask = current->cpus_allowed;
	set_cpus_allowed(current, cpumask_of_cpu(pol->cpu));
	schedule();

	if (smp_processor_id() != pol->cpu) {
		printk(KERN_ERR "limiting to cpu %u failed\n", pol->cpu);
		goto err_out;
	}

	if (pending_bit_stuck()) {
		printk(KERN_ERR PFX "failing init, change pending bit set\n");
		goto err_out;
	}

	if (query_current_values_with_pending_wait(data))
		goto err_out;

	fidvid_msr_init();

	/* run on any CPU again */
	set_cpus_allowed(current, oldmask);
	schedule();

	pol->governor = CPUFREQ_DEFAULT_GOVERNOR;

	/* Take a crude guess here. 
	 * That guess was in microseconds, so multiply with 1000 */
	pol->cpuinfo.transition_latency = (((data->rvo + 8) * data->vstable * VST_UNITS_20US)
	    + (3 * (1 << data->irt) * 10)) * 1000;

	pol->cur = find_khz_freq_from_fid(data->currfid);
	dprintk(KERN_DEBUG PFX "policy current frequency %d kHz\n", pol->cur);

	/* min/max the cpu is capable of */
	if (cpufreq_frequency_table_cpuinfo(pol, data->powernow_table)) {
		printk(KERN_ERR PFX "invalid powernow_table\n");
		kfree(data->powernow_table);
		kfree(data);
		return -EINVAL;
	}

	cpufreq_frequency_table_get_attr(data->powernow_table, pol->cpu);

	printk(KERN_INFO PFX "cpu_init done, current fid 0x%x, vid 0x%x\n",
	       data->currfid, data->currvid);

	powernow_data[pol->cpu] = data;

	return 0;

err_out:
	set_cpus_allowed(current, oldmask);
	schedule();

	kfree(data);
	return -ENODEV;
}

static int __exit powernowk8_cpu_exit (struct cpufreq_policy *pol)
{
	struct powernow_k8_data *data = powernow_data[pol->cpu];

	if (!data)
		return -EINVAL;

	powernow_k8_cpu_exit_acpi(data);

	cpufreq_frequency_table_put_attr(pol->cpu);

	kfree(data->powernow_table);
	kfree(data);

	return 0;
}

static unsigned int powernowk8_get (unsigned int cpu)
{
	struct powernow_k8_data *data = powernow_data[cpu];
	cpumask_t oldmask = current->cpus_allowed;
	unsigned int khz = 0;

	set_cpus_allowed(current, cpumask_of_cpu(cpu));
	if (smp_processor_id() != cpu) {
		printk(KERN_ERR PFX "limiting to CPU %d failed in powernowk8_get\n", cpu);
		set_cpus_allowed(current, oldmask);
		return 0;
	}
	preempt_disable();

	if (query_current_values_with_pending_wait(data))
		goto out;

	khz = find_khz_freq_from_fid(data->currfid);

 out:
	preempt_enable_no_resched();
	set_cpus_allowed(current, oldmask);

	return khz;
}

static struct freq_attr* powernow_k8_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	NULL,
};

static struct cpufreq_driver cpufreq_amd64_driver = {
	.verify = powernowk8_verify,
	.target = powernowk8_target,
	.init = powernowk8_cpu_init,
	.exit = powernowk8_cpu_exit,
	.get = powernowk8_get,
	.name = "powernow-k8",
	.owner = THIS_MODULE,
	.attr = powernow_k8_attr,
};

/* driver entry point for init */
static int __init powernowk8_init(void)
{
	unsigned int i, supported_cpus = 0;

	for (i=0; i<NR_CPUS; i++) {
		if (!cpu_online(i))
			continue;
		if (check_supported_cpu(i))
			supported_cpus++;
	}

	if (supported_cpus == num_online_cpus()) {
		printk(KERN_INFO PFX "Found %d AMD Athlon 64 / Opteron processors (" VERSION ")\n",
			supported_cpus);
		return cpufreq_register_driver(&cpufreq_amd64_driver);
	}

	return -ENODEV;
}

/* driver entry point for term */
static void __exit powernowk8_exit(void)
{
	dprintk(KERN_INFO PFX "exit\n");

	cpufreq_unregister_driver(&cpufreq_amd64_driver);
}

MODULE_AUTHOR("Paul Devriendt <paul.devriendt@amd.com>");
MODULE_DESCRIPTION("AMD Athlon 64 and Opteron processor frequency driver.");
MODULE_LICENSE("GPL");

late_initcall(powernowk8_init);
module_exit(powernowk8_exit);
