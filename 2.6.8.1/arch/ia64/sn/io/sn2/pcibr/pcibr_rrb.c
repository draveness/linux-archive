/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2001-2003 Silicon Graphics, Inc. All rights reserved.
 */

#include <linux/types.h>
#include <asm/sn/sgi.h>
#include <asm/sn/pci/pciio.h>
#include <asm/sn/pci/pcibr.h>
#include <asm/sn/pci/pcibr_private.h>
#include <asm/sn/pci/pci_defs.h>

void		pcibr_rrb_alloc_init(pcibr_soft_t, int, int, int);
void		pcibr_rrb_alloc_more(pcibr_soft_t, int, int, int);

int		pcibr_wrb_flush(vertex_hdl_t);
int		pcibr_rrb_alloc(vertex_hdl_t, int *, int *);
int		pcibr_rrb_check(vertex_hdl_t, int *, int *, int *, int *);
int		pcibr_alloc_all_rrbs(vertex_hdl_t, int, int, int, int, 
			     	     int, int, int, int, int);
void		pcibr_rrb_flush(vertex_hdl_t);
int		pcibr_slot_initial_rrb_alloc(vertex_hdl_t,pciio_slot_t);

void            pcibr_rrb_debug(char *, pcibr_soft_t);


/*
 * RRB Management
 *
 * All the do_pcibr_rrb_ routines manipulate the Read Response Buffer (rrb)
 * registers within the Bridge.	 Two 32 registers (b_rrb_map[2] also known
 * as the b_even_resp & b_odd_resp registers) are used to allocate the 16
 * rrbs to devices.  The b_even_resp register represents even num devices,
 * and b_odd_resp represent odd number devices.	 Each rrb is represented by
 * 4-bits within a register.
 *   BRIDGE & XBRIDGE:	1 enable bit, 1 virtual channel bit, 2 device bits
 *   PIC:		1 enable bit, 2 virtual channel bits, 1 device bit
 * PIC has 4 devices per bus, and 4 virtual channels (1 normal & 3 virtual)
 * per device.	BRIDGE & XBRIDGE have 8 devices per bus and 2 virtual
 * channels (1 normal & 1 virtual) per device.	See the BRIDGE and PIC ASIC
 * Programmers Reference guides for more information.
 */ 
 
#define RRB_MASK (0xf)			/* mask a single rrb within reg */
#define RRB_SIZE (4)			/* sizeof rrb within reg (bits) */
 
#define RRB_ENABLE_BIT		      (0x8)  /* [BRIDGE | PIC]_RRB_EN */
#define NUM_PDEV_BITS		      (1)
#define NUMBER_VCHANNELS	      (4)
#define SLOT_2_PDEV(slot)		((slot) >> 1)
#define SLOT_2_RRB_REG(slot)  		((slot) & 0x1)

#define RRB_VALID(rrb)		      (0x00010000 << (rrb))
#define RRB_INUSE(rrb)		      (0x00000001 << (rrb))
#define RRB_CLEAR(rrb)		      (0x00000001 << (rrb))
 
/* validate that the slot and virtual channel are valid */
#define VALIDATE_SLOT_n_VCHAN(s, v) \
    (((((s) != PCIIO_SLOT_NONE) && ((s) <= (pciio_slot_t)3)) && \
      (((v) >= 0) && ((v) <= 3))) ? 1 : 0)
 
/*  
 * Count how many RRBs are marked valid for the specified PCI slot
 * and virtual channel.	 Return the count.
 */ 
static int
do_pcibr_rrb_count_valid(pcibr_soft_t pcibr_soft,
			 pciio_slot_t slot,
			 int vchan)
{
    uint64_t tmp;
    uint16_t enable_bit, vchan_bits, pdev_bits, rrb_bits;
    int rrb_index, cnt=0;

    if (!VALIDATE_SLOT_n_VCHAN(slot, vchan)) {
	printk(KERN_WARNING "do_pcibr_rrb_count_valid() invalid slot/vchan [%d/%d]\n", slot, vchan);
	return 0;
    }
    
    enable_bit = RRB_ENABLE_BIT;
    vchan_bits = vchan << NUM_PDEV_BITS;
    pdev_bits = SLOT_2_PDEV(slot);
    rrb_bits = enable_bit | vchan_bits | pdev_bits;
    
    tmp = pcireg_rrb_get(pcibr_soft, SLOT_2_RRB_REG(slot));
    
    for (rrb_index = 0; rrb_index < 8; rrb_index++) {
	if ((tmp & RRB_MASK) == rrb_bits)
	    cnt++;
	tmp = (tmp >> RRB_SIZE);
    }
    return cnt;
}
 
 
/*  
 * Count how many RRBs are available to be allocated to the specified
 * slot.  Return the count.
 */ 
static int
do_pcibr_rrb_count_avail(pcibr_soft_t pcibr_soft,
			 pciio_slot_t slot)
{
    uint64_t tmp;
    uint16_t enable_bit;
    int rrb_index, cnt=0;
    
    if (!VALIDATE_SLOT_n_VCHAN(slot, 0)) {
	printk(KERN_WARNING "do_pcibr_rrb_count_avail() invalid slot/vchan");
	return 0;
    }
    
    enable_bit = RRB_ENABLE_BIT;
    
    tmp = pcireg_rrb_get(pcibr_soft, SLOT_2_RRB_REG(slot));
    
    for (rrb_index = 0; rrb_index < 8; rrb_index++) {
	if ((tmp & enable_bit) != enable_bit)
	    cnt++;
	tmp = (tmp >> RRB_SIZE);
    }
    return cnt;
}
 
 
/*  
 * Allocate some additional RRBs for the specified slot and the specified
 * virtual channel.  Returns -1 if there were insufficient free RRBs to
 * satisfy the request, or 0 if the request was fulfilled.
 *
 * Note that if a request can be partially filled, it will be, even if
 * we return failure.
 */ 
static int
do_pcibr_rrb_alloc(pcibr_soft_t pcibr_soft,
		   pciio_slot_t slot,
		   int vchan,
		   int more)
{
    uint64_t reg, tmp = 0;
    uint16_t enable_bit, vchan_bits, pdev_bits, rrb_bits;
    int rrb_index;
    
    if (!VALIDATE_SLOT_n_VCHAN(slot, vchan)) {
	printk(KERN_WARNING "do_pcibr_rrb_alloc() invalid slot/vchan");
	return -1;
    }
    
    enable_bit = RRB_ENABLE_BIT;
    vchan_bits = vchan << NUM_PDEV_BITS;
    pdev_bits = SLOT_2_PDEV(slot);
    rrb_bits = enable_bit | vchan_bits | pdev_bits;
    
    reg = tmp = pcireg_rrb_get(pcibr_soft, SLOT_2_RRB_REG(slot));
    
    for (rrb_index = 0; ((rrb_index < 8) && (more > 0)); rrb_index++) {
	if ((tmp & enable_bit) != enable_bit) {
	    /* clear the rrb and OR in the new rrb into 'reg' */
	    reg = reg & ~(RRB_MASK << (RRB_SIZE * rrb_index));
	    reg = reg | (rrb_bits << (RRB_SIZE * rrb_index));
	    more--;
	}
	tmp = (tmp >> RRB_SIZE);
    }
    
    pcireg_rrb_set(pcibr_soft, SLOT_2_RRB_REG(slot), reg);
    return (more ? -1 : 0);
}
 
/*
 * Wait for the the specified rrb to have no outstanding XIO pkts
 * and for all data to be drained.  Mark the rrb as no longer being 
 * valid.
 */
static void
do_pcibr_rrb_clear(pcibr_soft_t pcibr_soft, int rrb)
{
    uint64_t             status;

    /* bridge_lock must be held;  this RRB must be disabled. */

    /* wait until RRB has no outstanduing XIO packets. */
    status = pcireg_rrb_status_get(pcibr_soft);
    while (status & RRB_INUSE(rrb)) {
	status = pcireg_rrb_status_get(pcibr_soft);
    }

    /* if the RRB has data, drain it. */
    if (status & RRB_VALID(rrb)) {
	pcireg_rrb_clear_set(pcibr_soft, RRB_CLEAR(rrb));

	/* wait until RRB is no longer valid. */
	status = pcireg_rrb_status_get(pcibr_soft);
	while (status & RRB_VALID(rrb)) {
	    status = pcireg_rrb_status_get(pcibr_soft);
	}
    }
}

 
/*  
 * Release some of the RRBs that have been allocated for the specified
 * slot. Returns zero for success, or negative if it was unable to free
 * that many RRBs.
 *
 * Note that if a request can be partially fulfilled, it will be, even
 * if we return failure.
 */ 
static int
do_pcibr_rrb_free(pcibr_soft_t pcibr_soft,
		  pciio_slot_t slot,
		  int vchan,
		  int less)
{
    uint64_t reg, tmp = 0, clr = 0;
    uint16_t enable_bit, vchan_bits, pdev_bits, rrb_bits;
    int rrb_index;
    
    if (!VALIDATE_SLOT_n_VCHAN(slot, vchan)) {
	printk(KERN_WARNING "do_pcibr_rrb_free() invalid slot/vchan");
	return -1;
    }
    
    enable_bit = RRB_ENABLE_BIT;
    vchan_bits = vchan << NUM_PDEV_BITS;
    pdev_bits = SLOT_2_PDEV(slot);
    rrb_bits = enable_bit | vchan_bits | pdev_bits;
    
    reg = tmp = pcireg_rrb_get(pcibr_soft, SLOT_2_RRB_REG(slot));
    
    for (rrb_index = 0; ((rrb_index < 8) && (less > 0)); rrb_index++) {
	if ((tmp & RRB_MASK) == rrb_bits) {
	   /*
	    * the old do_pcibr_rrb_free() code only clears the enable bit
	    * but I say we should clear the whole rrb (ie):
	    *	  reg = reg & ~(RRB_MASK << (RRB_SIZE * rrb_index));
	    * But to be compatible with old code we'll only clear enable.
	    */
	    reg = reg & ~(RRB_ENABLE_BIT << (RRB_SIZE * rrb_index));
	    clr = clr | (enable_bit << (RRB_SIZE * rrb_index));
	    less--;
	}
	tmp = (tmp >> RRB_SIZE);
    }
    
    pcireg_rrb_set(pcibr_soft, SLOT_2_RRB_REG(slot), reg);
    
    /* call do_pcibr_rrb_clear() for all the rrbs we've freed */
    for (rrb_index = 0; rrb_index < 8; rrb_index++) {
	int evn_odd = SLOT_2_RRB_REG(slot);
	if (clr & (enable_bit << (RRB_SIZE * rrb_index)))
	    do_pcibr_rrb_clear(pcibr_soft, (2 * rrb_index) + evn_odd);
    }
    
    return (less ? -1 : 0);
}
 
/* 
 * Flush the specified rrb by calling do_pcibr_rrb_clear().  This
 * routine is just a wrapper to make sure the rrb is disabled 
 * before calling do_pcibr_rrb_clear().
 */
static void
do_pcibr_rrb_flush(pcibr_soft_t pcibr_soft, int rrbn)
{
    uint64_t	rrbv;
    int		shft = (RRB_SIZE * (rrbn >> 1));
    uint64_t	ebit = RRB_ENABLE_BIT << shft;

    rrbv = pcireg_rrb_get(pcibr_soft, (rrbn & 1));
    if (rrbv & ebit) {
	pcireg_rrb_set(pcibr_soft, (rrbn & 1), (rrbv & ~ebit));
    }

    do_pcibr_rrb_clear(pcibr_soft, rrbn);

    if (rrbv & ebit) {
	pcireg_rrb_set(pcibr_soft, (rrbn & 1), rrbv);
    }
}

/*  
 * free all the rrbs (both the normal and virtual channels) for the
 * specified slot.
 */ 
void
do_pcibr_rrb_free_all(pcibr_soft_t pcibr_soft,
		      pciio_slot_t slot)
{
    int vchan;
    int vchan_total = NUMBER_VCHANNELS;
    
    /* pretend we own all 8 rrbs and just ignore the return value */
    for (vchan = 0; vchan < vchan_total; vchan++) {
	    do_pcibr_rrb_free(pcibr_soft, slot, vchan, 8);
	    pcibr_soft->bs_rrb_valid[slot][vchan] = 0;
    }
}


/*
 * Initialize a slot with a given number of RRBs.  (this routine
 * will also give back RRBs if the slot has more than we want).
 */
void
pcibr_rrb_alloc_init(pcibr_soft_t pcibr_soft,
		     int slot,
		     int vchan,
		     int init_rrbs)
{
    int			 had = pcibr_soft->bs_rrb_valid[slot][vchan];
    int			 have = had;
    int			 added = 0;

    for (added = 0; have < init_rrbs; ++added, ++have) {
	if (pcibr_soft->bs_rrb_res[slot] > 0)
	    pcibr_soft->bs_rrb_res[slot]--;
	else if (pcibr_soft->bs_rrb_avail[slot & 1] > 0)
	    pcibr_soft->bs_rrb_avail[slot & 1]--;
	else
	    break;
	if (do_pcibr_rrb_alloc(pcibr_soft, slot, vchan, 1) < 0)
	    break;

	pcibr_soft->bs_rrb_valid[slot][vchan]++;
    }

    /* Free any extra RRBs that the slot may have allocated to it */
    while (have > init_rrbs) {
	pcibr_soft->bs_rrb_avail[slot & 1]++;
	pcibr_soft->bs_rrb_valid[slot][vchan]--;
	do_pcibr_rrb_free(pcibr_soft, slot, vchan, 1);
	added--;
	have--;
    }

    PCIBR_DEBUG_ALWAYS((PCIBR_DEBUG_RRB, pcibr_soft->bs_vhdl,
		"pcibr_rrb_alloc_init: had %d, added/removed %d, "
		"(of requested %d) RRBs "
		"to slot %d, vchan %d\n", had, added, init_rrbs,
		PCIBR_DEVICE_TO_SLOT(pcibr_soft, slot), vchan));

    pcibr_rrb_debug("pcibr_rrb_alloc_init", pcibr_soft);
}


/*
 * Allocate more RRBs to a given slot (if the RRBs are available).
 */
void
pcibr_rrb_alloc_more(pcibr_soft_t pcibr_soft,
		     int slot,
		     int vchan, 
		     int more_rrbs)
{
    int			 added;

    for (added = 0; added < more_rrbs; ++added) {
	if (pcibr_soft->bs_rrb_res[slot] > 0)
	    pcibr_soft->bs_rrb_res[slot]--;
	else if (pcibr_soft->bs_rrb_avail[slot & 1] > 0)
	    pcibr_soft->bs_rrb_avail[slot & 1]--;
	else
	    break;
	if (do_pcibr_rrb_alloc(pcibr_soft, slot, vchan, 1) < 0)
	    break;

	pcibr_soft->bs_rrb_valid[slot][vchan]++;
    }

    PCIBR_DEBUG_ALWAYS((PCIBR_DEBUG_RRB, pcibr_soft->bs_vhdl,
		"pcibr_rrb_alloc_more: added %d (of %d requested) RRBs "
		"to slot %d, vchan %d\n", added, more_rrbs, 
		PCIBR_DEVICE_TO_SLOT(pcibr_soft, slot), vchan));

    pcibr_rrb_debug("pcibr_rrb_alloc_more", pcibr_soft);
}


/*
 * Flush all the rrb's assigned to the specified connection point.
 */
void
pcibr_rrb_flush(vertex_hdl_t pconn_vhdl)
{
    pciio_info_t  pciio_info = pciio_info_get(pconn_vhdl);
    pcibr_soft_t  pcibr_soft = (pcibr_soft_t)pciio_info_mfast_get(pciio_info);
    pciio_slot_t  slot = PCIBR_INFO_SLOT_GET_INT(pciio_info);

    uint64_t tmp;
    uint16_t enable_bit, pdev_bits, rrb_bits, rrb_mask;
    int rrb_index;
    unsigned long s;

    enable_bit = RRB_ENABLE_BIT;
    pdev_bits = SLOT_2_PDEV(slot);
    rrb_bits = enable_bit | pdev_bits;
    rrb_mask = enable_bit | ((NUM_PDEV_BITS << 1) - 1);

    tmp = pcireg_rrb_get(pcibr_soft, SLOT_2_RRB_REG(slot));

    s = pcibr_lock(pcibr_soft);
    for (rrb_index = 0; rrb_index < 8; rrb_index++) {
	int evn_odd = SLOT_2_RRB_REG(slot);
	if ((tmp & rrb_mask) == rrb_bits)
	    do_pcibr_rrb_flush(pcibr_soft, (2 * rrb_index) + evn_odd);
	tmp = (tmp >> RRB_SIZE);
    }
    pcibr_unlock(pcibr_soft, s);
}


/*
 * Device driver interface to flush the write buffers for a specified
 * device hanging off the bridge.
 */
int
pcibr_wrb_flush(vertex_hdl_t pconn_vhdl)
{
    pciio_info_t            pciio_info = pciio_info_get(pconn_vhdl);
    pciio_slot_t            pciio_slot = PCIBR_INFO_SLOT_GET_INT(pciio_info);
    pcibr_soft_t            pcibr_soft = (pcibr_soft_t) pciio_info_mfast_get(pciio_info);

    pcireg_wrb_flush_get(pcibr_soft, pciio_slot);

    return 0;
}

/*
 * Device driver interface to request RRBs for a specified device
 * hanging off a Bridge.  The driver requests the total number of
 * RRBs it would like for the normal channel (vchan0) and for the
 * "virtual channel" (vchan1).  The actual number allocated to each
 * channel is returned.
 *
 * If we cannot allocate at least one RRB to a channel that needs
 * at least one, return -1 (failure).  Otherwise, satisfy the request
 * as best we can and return 0.
 */
int
pcibr_rrb_alloc(vertex_hdl_t pconn_vhdl,
		int *count_vchan0,
		int *count_vchan1)
{
    pciio_info_t            pciio_info = pciio_info_get(pconn_vhdl);
    pciio_slot_t            pciio_slot = PCIBR_INFO_SLOT_GET_INT(pciio_info);
    pcibr_soft_t            pcibr_soft = (pcibr_soft_t) pciio_info_mfast_get(pciio_info);
    int                     desired_vchan0;
    int                     desired_vchan1;
    int                     orig_vchan0;
    int                     orig_vchan1;
    int                     delta_vchan0;
    int                     delta_vchan1;
    int                     final_vchan0;
    int                     final_vchan1;
    int                     avail_rrbs;
    int                     res_rrbs;
    int			    vchan_total;
    int			    vchan;
    unsigned long                s;
    int                     error;

    /*
     * TBD: temper request with admin info about RRB allocation,
     * and according to demand from other devices on this Bridge.
     *
     * One way of doing this would be to allocate two RRBs
     * for each device on the bus, before any drivers start
     * asking for extras. This has the weakness that one
     * driver might not give back an "extra" RRB until after
     * another driver has already failed to get one that
     * it wanted.
     */

    s = pcibr_lock(pcibr_soft);

    vchan_total = NUMBER_VCHANNELS;

    /* Save the boot-time RRB configuration for this slot */
    if (pcibr_soft->bs_rrb_valid_dflt[pciio_slot][VCHAN0] < 0) {
	for (vchan = 0; vchan < vchan_total; vchan++) 
	    pcibr_soft->bs_rrb_valid_dflt[pciio_slot][vchan] =
		    pcibr_soft->bs_rrb_valid[pciio_slot][vchan];
        pcibr_soft->bs_rrb_res_dflt[pciio_slot] =
                pcibr_soft->bs_rrb_res[pciio_slot];
                  
    }

    /* How many RRBs do we own? */
    orig_vchan0 = pcibr_soft->bs_rrb_valid[pciio_slot][VCHAN0];
    orig_vchan1 = pcibr_soft->bs_rrb_valid[pciio_slot][VCHAN1];

    /* How many RRBs do we want? */
    desired_vchan0 = count_vchan0 ? *count_vchan0 : orig_vchan0;
    desired_vchan1 = count_vchan1 ? *count_vchan1 : orig_vchan1;

    /* How many RRBs are free? */
    avail_rrbs = pcibr_soft->bs_rrb_avail[pciio_slot & 1]
	+ pcibr_soft->bs_rrb_res[pciio_slot];

    /* Figure desired deltas */
    delta_vchan0 = desired_vchan0 - orig_vchan0;
    delta_vchan1 = desired_vchan1 - orig_vchan1;

    /* Trim back deltas to something
     * that we can actually meet, by
     * decreasing the ending allocation
     * for whichever channel wants
     * more RRBs. If both want the same
     * number, cut the second channel.
     * NOTE: do not change the allocation for
     * a channel that was passed as NULL.
     */
    while ((delta_vchan0 + delta_vchan1) > avail_rrbs) {
	if (count_vchan0 &&
	    (!count_vchan1 ||
	     ((orig_vchan0 + delta_vchan0) >
	      (orig_vchan1 + delta_vchan1))))
	    delta_vchan0--;
	else
	    delta_vchan1--;
    }

    /* Figure final RRB allocations
     */
    final_vchan0 = orig_vchan0 + delta_vchan0;
    final_vchan1 = orig_vchan1 + delta_vchan1;

    /* If either channel wants RRBs but our actions
     * would leave it with none, declare an error,
     * but DO NOT change any RRB allocations.
     */
    if ((desired_vchan0 && !final_vchan0) ||
	(desired_vchan1 && !final_vchan1)) {

	error = -1;

    } else {

	/* Commit the allocations: free, then alloc.
	 */
	if (delta_vchan0 < 0)
	    do_pcibr_rrb_free(pcibr_soft, pciio_slot, VCHAN0, -delta_vchan0);
	if (delta_vchan1 < 0)
	    do_pcibr_rrb_free(pcibr_soft, pciio_slot, VCHAN1, -delta_vchan1);

	if (delta_vchan0 > 0)
	    do_pcibr_rrb_alloc(pcibr_soft, pciio_slot, VCHAN0, delta_vchan0);
	if (delta_vchan1 > 0)
	    do_pcibr_rrb_alloc(pcibr_soft, pciio_slot, VCHAN1, delta_vchan1);

	/* Return final values to caller.
	 */
	if (count_vchan0)
	    *count_vchan0 = final_vchan0;
	if (count_vchan1)
	    *count_vchan1 = final_vchan1;

	/* prevent automatic changes to this slot's RRBs
	 */
	pcibr_soft->bs_rrb_fixed |= 1 << pciio_slot;

	/* Track the actual allocations, release
	 * any further reservations, and update the
	 * number of available RRBs.
	 */

	pcibr_soft->bs_rrb_valid[pciio_slot][VCHAN0] = final_vchan0;
	pcibr_soft->bs_rrb_valid[pciio_slot][VCHAN1] = final_vchan1;
	pcibr_soft->bs_rrb_avail[pciio_slot & 1] =
	    pcibr_soft->bs_rrb_avail[pciio_slot & 1]
	    + pcibr_soft->bs_rrb_res[pciio_slot]
	    - delta_vchan0
	    - delta_vchan1;
	pcibr_soft->bs_rrb_res[pciio_slot] = 0;

        /*
         * Reserve enough RRBs so this slot's RRB configuration can be
         * reset to its boot-time default following a hot-plug shut-down
         */
	res_rrbs = (pcibr_soft->bs_rrb_res_dflt[pciio_slot] -
		    pcibr_soft->bs_rrb_res[pciio_slot]);
	for (vchan = 0; vchan < vchan_total; vchan++) {
	    res_rrbs += (pcibr_soft->bs_rrb_valid_dflt[pciio_slot][vchan] -
			 pcibr_soft->bs_rrb_valid[pciio_slot][vchan]);
	}

	if (res_rrbs > 0) {
            pcibr_soft->bs_rrb_res[pciio_slot] = res_rrbs;
            pcibr_soft->bs_rrb_avail[pciio_slot & 1] =
                pcibr_soft->bs_rrb_avail[pciio_slot & 1]
                - res_rrbs;
        }
 
	pcibr_rrb_debug("pcibr_rrb_alloc", pcibr_soft);

	error = 0;
    }

    pcibr_unlock(pcibr_soft, s);

    return error;
}

/*
 * Device driver interface to check the current state
 * of the RRB allocations.
 *
 *   pconn_vhdl is your PCI connection point (specifies which
 *      PCI bus and which slot).
 *
 *   count_vchan0 points to where to return the number of RRBs
 *      assigned to the primary DMA channel, used by all DMA
 *      that does not explicitly ask for the alternate virtual
 *      channel.
 *
 *   count_vchan1 points to where to return the number of RRBs
 *      assigned to the secondary DMA channel, used when
 *      PCIBR_VCHAN1 and PCIIO_DMA_A64 are specified.
 *
 *   count_reserved points to where to return the number of RRBs
 *      that have been automatically reserved for your device at
 *      startup, but which have not been assigned to a
 *      channel. RRBs must be assigned to a channel to be used;
 *      this can be done either with an explicit pcibr_rrb_alloc
 *      call, or automatically by the infrastructure when a DMA
 *      translation is constructed. Any call to pcibr_rrb_alloc
 *      will release any unassigned reserved RRBs back to the
 *      free pool.
 *
 *   count_pool points to where to return the number of RRBs
 *      that are currently unassigned and unreserved. This
 *      number can (and will) change as other drivers make calls
 *      to pcibr_rrb_alloc, or automatically allocate RRBs for
 *      DMA beyond their initial reservation.
 *
 * NULL may be passed for any of the return value pointers
 * the caller is not interested in.
 *
 * The return value is "0" if all went well, or "-1" if
 * there is a problem. Additionally, if the wrong vertex
 * is passed in, one of the subsidiary support functions
 * could panic with a "bad pciio fingerprint."
 */

int
pcibr_rrb_check(vertex_hdl_t pconn_vhdl,
		int *count_vchan0,
		int *count_vchan1,
		int *count_reserved,
		int *count_pool)
{
    pciio_info_t            pciio_info;
    pciio_slot_t            pciio_slot;
    pcibr_soft_t            pcibr_soft;
    unsigned long                s;
    int                     error = -1;

    if ((pciio_info = pciio_info_get(pconn_vhdl)) &&
	(pcibr_soft = (pcibr_soft_t) pciio_info_mfast_get(pciio_info)) &&
	((pciio_slot = PCIBR_INFO_SLOT_GET_INT(pciio_info)) < PCIBR_NUM_SLOTS(pcibr_soft))) {

	s = pcibr_lock(pcibr_soft);

	if (count_vchan0)
	    *count_vchan0 =
		pcibr_soft->bs_rrb_valid[pciio_slot][VCHAN0];

	if (count_vchan1)
	    *count_vchan1 =
		pcibr_soft->bs_rrb_valid[pciio_slot][VCHAN1];

	if (count_reserved)
	    *count_reserved =
		pcibr_soft->bs_rrb_res[pciio_slot];

	if (count_pool)
	    *count_pool =
		pcibr_soft->bs_rrb_avail[pciio_slot & 1];

	error = 0;

	pcibr_unlock(pcibr_soft, s);
    }
    return error;
}

/*
 * pcibr_slot_initial_rrb_alloc
 *	Allocate a default number of rrbs for this slot on 
 * 	the two channels.  This is dictated by the rrb allocation
 * 	strategy routine defined per platform.
 */

int
pcibr_slot_initial_rrb_alloc(vertex_hdl_t pcibr_vhdl,
			     pciio_slot_t slot)
{
    pcibr_soft_t	 pcibr_soft;
    pcibr_info_h	 pcibr_infoh;
    pcibr_info_t	 pcibr_info;
    int 		 vchan_total;
    int			 vchan;
    int                  chan[4];

    pcibr_soft = pcibr_soft_get(pcibr_vhdl);

    if (!pcibr_soft)
	return -EINVAL;

    if (!PCIBR_VALID_SLOT(pcibr_soft, slot))
	return -EINVAL;

    /* How many RRBs are on this slot? */
    vchan_total = NUMBER_VCHANNELS;
    for (vchan = 0; vchan < vchan_total; vchan++) 
        chan[vchan] = do_pcibr_rrb_count_valid(pcibr_soft, slot, vchan);

    PCIBR_DEBUG_ALWAYS((PCIBR_DEBUG_RRB, pcibr_vhdl,
	    "pcibr_slot_initial_rrb_alloc: slot %d started with %d+%d+%d+%d\n",
	    PCIBR_DEVICE_TO_SLOT(pcibr_soft, slot), 
	    chan[VCHAN0], chan[VCHAN1], chan[VCHAN2], chan[VCHAN3]));

    /* Do we really need any?
     */
    pcibr_infoh = pcibr_soft->bs_slot[slot].bss_infos;
    pcibr_info = pcibr_infoh[0];
    /*
     * PIC BRINGUP WAR (PV# 856866, 859504, 861476, 861478):
     * Don't free RRBs we allocated to device[2|3]--vchan3 as
     * a WAR to those PVs mentioned above.  In pcibr_attach2
     * we allocate RRB0,8,1,9 to device[2|3]--vchan3.
     */
    if (PCIBR_WAR_ENABLED(PV856866, pcibr_soft) && 
			(slot == 2 || slot == 3) &&
        		(pcibr_info->f_vendor == PCIIO_VENDOR_ID_NONE) &&
        		!pcibr_soft->bs_slot[slot].has_host) {

	for (vchan = 0; vchan < 2; vchan++) {
	    do_pcibr_rrb_free(pcibr_soft, slot, vchan, 8);
	    pcibr_soft->bs_rrb_valid[slot][vchan] = 0;
	}

        pcibr_soft->bs_rrb_valid[slot][3] = chan[3];

        return -ENODEV;
    }

    if ((pcibr_info->f_vendor == PCIIO_VENDOR_ID_NONE) &&
	!pcibr_soft->bs_slot[slot].has_host) {
	do_pcibr_rrb_free_all(pcibr_soft, slot);
        
	/* Reserve RRBs for this empty slot for hot-plug */
	for (vchan = 0; vchan < vchan_total; vchan++) 
	    pcibr_soft->bs_rrb_valid[slot][vchan] = 0;

	return -ENODEV;
    }

    for (vchan = 0; vchan < vchan_total; vchan++)
        pcibr_soft->bs_rrb_valid[slot][vchan] = chan[vchan];

    return 0;
}


/*
 * pcibr_initial_rrb
 *      Assign an equal total number of RRBs to all candidate slots, 
 *      where the total is the sum of the number of RRBs assigned to
 *      the normal channel, the number of RRBs assigned to the virtual
 *      channels, and the number of RRBs assigned as reserved. 
 *
 *      A candidate slot is any existing (populated or empty) slot.
 *      Empty SN1 slots need RRBs to support hot-plug operations.
 */

int
pcibr_initial_rrb(vertex_hdl_t pcibr_vhdl,
			     pciio_slot_t first, pciio_slot_t last)
{
    pcibr_soft_t            pcibr_soft = pcibr_soft_get(pcibr_vhdl);
    pciio_slot_t            slot;
    int			    rrb_total;
    int			    vchan_total;
    int			    vchan;
    int                     have[2][3];
    int                     res[2];
    int                     eo;

    have[0][0] = have[0][1] = have[0][2] = 0;
    have[1][0] = have[1][1] = have[1][2] = 0;
    res[0] = res[1] = 0;

    vchan_total = NUMBER_VCHANNELS;

    for (slot = pcibr_soft->bs_min_slot; 
			slot < PCIBR_NUM_SLOTS(pcibr_soft); ++slot) {
        /* Initial RRB management; give back RRBs in all non-existent slots */
        pcibr_slot_initial_rrb_alloc(pcibr_vhdl, slot);

        /* Base calculations only on existing slots */
        if ((slot >= first) && (slot <= last)) {
	    rrb_total = 0;
	    for (vchan = 0; vchan < vchan_total; vchan++) 
		rrb_total += pcibr_soft->bs_rrb_valid[slot][vchan];

            if (rrb_total < 3)
                have[slot & 1][rrb_total]++;
        }
    }

    /* Initialize even/odd slot available RRB counts */
    pcibr_soft->bs_rrb_avail[0] = do_pcibr_rrb_count_avail(pcibr_soft, 0);
    pcibr_soft->bs_rrb_avail[1] = do_pcibr_rrb_count_avail(pcibr_soft, 1);

    /*
     * Calculate reserved RRBs for slots based on current RRB usage
     */
    for (eo = 0; eo < 2; eo++) {
        if ((3 * have[eo][0] + 2 * have[eo][1] + have[eo][2]) <= pcibr_soft->bs_rrb_avail[eo])
            res[eo] = 3;
        else if ((2 * have[eo][0] + have[eo][1]) <= pcibr_soft->bs_rrb_avail[eo])
            res[eo] = 2;
        else if (have[eo][0] <= pcibr_soft->bs_rrb_avail[eo])
            res[eo] = 1;
        else
            res[eo] = 0;

    }

    /* Assign reserved RRBs to existing slots */
    for (slot = first; slot <= last; ++slot) {
        int                     r;

	if (pcibr_soft->bs_unused_slot & (1 << slot))
	    continue;

	rrb_total = 0;
	for (vchan = 0; vchan < vchan_total; vchan++)
		rrb_total += pcibr_soft->bs_rrb_valid[slot][vchan];

        r = res[slot & 1] - (rrb_total);

        if (r > 0) {
            pcibr_soft->bs_rrb_res[slot] = r;
            pcibr_soft->bs_rrb_avail[slot & 1] -= r;
        }
    }

    pcibr_rrb_debug("pcibr_initial_rrb", pcibr_soft);

    return 0;

}

/*
 * Dump the pcibr_soft_t RRB state variable
 */
void
pcibr_rrb_debug(char *calling_func, pcibr_soft_t pcibr_soft)
{
    pciio_slot_t slot;
    
    if (pcibr_debug_mask & PCIBR_DEBUG_RRB) {
        PCIBR_DEBUG_ALWAYS((PCIBR_DEBUG_RRB, pcibr_soft->bs_vhdl,
                    "%s: rrbs available, even=%d, odd=%d\n", calling_func,
                    pcibr_soft->bs_rrb_avail[0], pcibr_soft->bs_rrb_avail[1]));

        PCIBR_DEBUG_ALWAYS((PCIBR_DEBUG_RRB, pcibr_soft->bs_vhdl,
                    "\tslot\tvchan0\tvchan1\tvchan2\tvchan3\treserved\n"));

        for (slot=0; slot < PCIBR_NUM_SLOTS(pcibr_soft); slot++) {
            PCIBR_DEBUG_ALWAYS((PCIBR_DEBUG_RRB, pcibr_soft->bs_vhdl,
		    "\t %d\t  %d\t  %d\t  %d\t  %d\t  %d\n",
		    PCIBR_DEVICE_TO_SLOT(pcibr_soft, slot),
		    0xFFF & pcibr_soft->bs_rrb_valid[slot][VCHAN0],
		    0xFFF & pcibr_soft->bs_rrb_valid[slot][VCHAN1],
		    0xFFF & pcibr_soft->bs_rrb_valid[slot][VCHAN2],
		    0xFFF & pcibr_soft->bs_rrb_valid[slot][VCHAN3],
		    pcibr_soft->bs_rrb_res[slot]));
        }
    }
}
