/*
 *
 * BRIEF MODULE DESCRIPTION
 *      The Descriptor Based DMA channel manager that first appeared
 *	on the Au1550.  I started with dma.c, but I think all that is
 *	left is this initial comment :-)
 *
 * Copyright 2004 Embedded Edge, LLC
 *	dan@embeddededge.com
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 *  THIS  SOFTWARE  IS PROVIDED   ``AS  IS'' AND   ANY  EXPRESS OR IMPLIED
 *  WARRANTIES,   INCLUDING, BUT NOT  LIMITED  TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
 *  NO  EVENT  SHALL   THE AUTHOR  BE    LIABLE FOR ANY   DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *  NOT LIMITED   TO, PROCUREMENT OF  SUBSTITUTE GOODS  OR SERVICES; LOSS OF
 *  USE, DATA,  OR PROFITS; OR  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 *  ANY THEORY OF LIABILITY, WHETHER IN  CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 *  THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the  GNU General Public License along
 *  with this program; if not, write  to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <asm/mach-au1x00/au1000.h>
#include <asm/mach-au1x00/au1xxx_dbdma.h>
#include <asm/system.h>

/*
 * The Descriptor Based DMA supports up to 16 channels.
 *
 * There are 32 devices defined. We keep an internal structure
 * of devices using these channels, along with additional
 * information.
 *
 * We allocate the descriptors and allow access to them through various
 * functions.  The drivers allocate the data buffers and assign them
 * to the descriptors.
 */
static spinlock_t au1xxx_dbdma_spin_lock = SPIN_LOCK_UNLOCKED;

/* I couldn't find a macro that did this......
*/
#define ALIGN_ADDR(x, a)	((((u32)(x)) + (a-1)) & ~(a-1))

static volatile dbdma_global_t *dbdma_gptr = (dbdma_global_t *)DDMA_GLOBAL_BASE;
static int dbdma_initialized;
static void au1xxx_dbdma_init(void);

typedef struct dbdma_device_table {
	u32		dev_flags;
	u32		dev_tsize;
	u32		dev_devwidth;
	u32		dev_physaddr;		/* If FIFO */
	u32		dev_intlevel;
	u32		dev_intpolarity;
} dbdev_tab_t;

typedef struct dbdma_chan_config {
	u32			chan_flags;
	u32			chan_index;
	dbdev_tab_t		*chan_src;
	dbdev_tab_t		*chan_dest;
	au1x_dma_chan_t		*chan_ptr;
	au1x_ddma_desc_t	*chan_desc_base;
	au1x_ddma_desc_t	*get_ptr, *put_ptr, *cur_ptr;
	void			*chan_callparam;
	void (*chan_callback)(int, void *, struct pt_regs *);
} chan_tab_t;

#define	DEV_FLAGS_INUSE		(1 << 0)
#define	DEV_FLAGS_ANYUSE	(1 << 1)
#define DEV_FLAGS_OUT		(1 << 2)
#define DEV_FLAGS_IN		(1 << 3)

static dbdev_tab_t dbdev_tab[] = {
	/* UARTS */
	{ DEV_FLAGS_OUT, 0, 8, 0x11100004, 0, 0 },
	{ DEV_FLAGS_IN, 0, 8, 0x11100000, 0, 0 },
	{ DEV_FLAGS_OUT, 0, 8, 0x11400004, 0, 0 },
	{ DEV_FLAGS_IN, 0, 8, 0x11400000, 0, 0 },

	/* EXT DMA */
	{ 0, 0, 0, 0x00000000, 0, 0 },
	{ 0, 0, 0, 0x00000000, 0, 0 },
	{ 0, 0, 0, 0x00000000, 0, 0 },
	{ 0, 0, 0, 0x00000000, 0, 0 },

	/* USB DEV */
	{ DEV_FLAGS_IN, 4, 8, 0x10200000, 0, 0 },
	{ DEV_FLAGS_OUT, 4, 8, 0x10200004, 0, 0 },
	{ DEV_FLAGS_OUT, 4, 8, 0x10200008, 0, 0 },
	{ DEV_FLAGS_OUT, 4, 8, 0x1020000c, 0, 0 },
	{ DEV_FLAGS_IN, 4, 8, 0x10200010, 0, 0 },
	{ DEV_FLAGS_IN, 4, 8, 0x10200014, 0, 0 },

	/* PSC 0 */
	{ DEV_FLAGS_OUT, 0, 0, 0x11a0001c, 0, 0 },
	{ DEV_FLAGS_IN, 0, 0, 0x11a0001c, 0, 0 },

	/* PSC 1 */
	{ DEV_FLAGS_OUT, 0, 0, 0x11b0001c, 0, 0 },
	{ DEV_FLAGS_IN, 0, 0, 0x11b0001c, 0, 0 },

	/* PSC 2 */
	{ DEV_FLAGS_OUT, 0, 0, 0x10a0001c, 0, 0 },
	{ DEV_FLAGS_IN, 0, 0, 0x10a0001c, 0, 0 },

	/* PSC 3 */
	{ DEV_FLAGS_OUT, 0, 0, 0x10b0001c, 0, 0 },
	{ DEV_FLAGS_IN, 0, 0, 0x10b0001c, 0, 0 },

	{ 0, 0, 0, 0x00000000, 0, 0 },	/* PCI */
	{ 0, 0, 0, 0x00000000, 0, 0 },	/* NAND */

	/* MAC 0 */
	{ DEV_FLAGS_IN, 0, 0, 0x00000000, 0, 0 },
	{ DEV_FLAGS_OUT, 0, 0, 0x00000000, 0, 0 },

	/* MAC 1 */
	{ DEV_FLAGS_IN, 0, 0, 0x00000000, 0, 0 },
	{ DEV_FLAGS_OUT, 0, 0, 0x00000000, 0, 0 },

	/* reserved */
	{ DEV_FLAGS_INUSE, 0, 0, 0x00000000, 0, 0 },
	{ DEV_FLAGS_INUSE, 0, 0, 0x00000000, 0, 0 },

	/* Memory */
	{ DEV_FLAGS_ANYUSE, 0, 0, 0x00000000, 0, 0 },	/* throttle */
	{ DEV_FLAGS_ANYUSE, 0, 0, 0x00000000, 0, 0 },	/* always */
};

static chan_tab_t *chan_tab_ptr[NUM_DBDMA_CHANS];

/* Allocate a channel and return a non-zero descriptor if successful.
*/
u32
au1xxx_dbdma_chan_alloc(u32 srcid, u32 destid,
       void (*callback)(int, void *, struct pt_regs *), void *callparam)
{
	unsigned long   flags;
	u32		used, chan, rv;
	u32		dcp;
	int		i;
	dbdev_tab_t	*stp, *dtp;
	chan_tab_t	*ctp;
	volatile au1x_dma_chan_t *cp;

	/* We do the intialization on the first channel allocation.
	 * We have to wait because of the interrupt handler initialization
	 * which can't be done successfully during board set up.
	 */
	if (!dbdma_initialized)
		au1xxx_dbdma_init();
	dbdma_initialized = 1;

	if ((srcid > DSCR_NDEV_IDS) || (destid > DSCR_NDEV_IDS))
		return 0;

	stp = &dbdev_tab[srcid];
	dtp = &dbdev_tab[destid];
	used = 0;
	rv = 0;

	/* Check to see if we can get both channels.
	*/
	spin_lock_irqsave(&au1xxx_dbdma_spin_lock, flags);
	if (!(stp->dev_flags & DEV_FLAGS_INUSE) ||
	     (stp->dev_flags & DEV_FLAGS_ANYUSE)) {
	     	/* Got source */
		stp->dev_flags |= DEV_FLAGS_INUSE;
		if (!(dtp->dev_flags & DEV_FLAGS_INUSE) ||
		     (dtp->dev_flags & DEV_FLAGS_ANYUSE)) {
			/* Got destination */
			dtp->dev_flags |= DEV_FLAGS_INUSE;
		}
		else {
			/* Can't get dest.  Release src.
			*/
			stp->dev_flags &= ~DEV_FLAGS_INUSE;
			used++;
		}
	}
	else {
		used++;
	}
	spin_unlock_irqrestore(&au1xxx_dbdma_spin_lock, flags);

	if (!used) {
		/* Let's see if we can allocate a channel for it.
		*/
		ctp = NULL;
		chan = 0;
		spin_lock_irqsave(&au1xxx_dbdma_spin_lock, flags);
		for (i=0; i<NUM_DBDMA_CHANS; i++) {
			if (chan_tab_ptr[i] == NULL) {
				/* If kmalloc fails, it is caught below same
				 * as a channel not available.
				 */
				ctp = (chan_tab_t *)kmalloc(sizeof(chan_tab_t), GFP_KERNEL);
				chan_tab_ptr[i] = ctp;
				ctp->chan_index = chan = i;
				break;
			}
		}
		spin_unlock_irqrestore(&au1xxx_dbdma_spin_lock, flags);

		if (ctp != NULL) {
			memset(ctp, 0, sizeof(chan_tab_t));
			dcp = DDMA_CHANNEL_BASE;
			dcp += (0x0100 * chan);
			ctp->chan_ptr = (au1x_dma_chan_t *)dcp;
			cp = (volatile au1x_dma_chan_t *)dcp;
			ctp->chan_src = stp;
			ctp->chan_dest = dtp;
			ctp->chan_callback = callback;
			ctp->chan_callparam = callparam;

			/* Initialize channel configuration.
			*/
			i = 0;
			if (stp->dev_intlevel)
				i |= DDMA_CFG_SED;
			if (stp->dev_intpolarity)
				i |= DDMA_CFG_SP;
			if (dtp->dev_intlevel)
				i |= DDMA_CFG_DED;
			if (dtp->dev_intpolarity)
				i |= DDMA_CFG_DP;
			cp->ddma_cfg = i;
			au_sync();

			/* Return a non-zero value that can be used to
			 * find the channel information in subsequent
			 * operations.
			 */
			rv = (u32)(&chan_tab_ptr[chan]);
		}
		else {
			/* Release devices.
			*/
			stp->dev_flags &= ~DEV_FLAGS_INUSE;
			dtp->dev_flags &= ~DEV_FLAGS_INUSE;
		}
	}
	return rv;
}

/* Set the device width if source or destination is a FIFO.
 * Should be 8, 16, or 32 bits.
 */
u32
au1xxx_dbdma_set_devwidth(u32 chanid, int bits)
{
	u32		rv;
	chan_tab_t	*ctp;
	dbdev_tab_t	*stp, *dtp;

	ctp = *((chan_tab_t **)chanid);
	stp = ctp->chan_src;
	dtp = ctp->chan_dest;
	rv = 0;

	if (stp->dev_flags & DEV_FLAGS_IN) {	/* Source in fifo */
		rv = stp->dev_devwidth;
		stp->dev_devwidth = bits;
	}
	if (dtp->dev_flags & DEV_FLAGS_OUT) {	/* Destination out fifo */
		rv = dtp->dev_devwidth;
		dtp->dev_devwidth = bits;
	}

	return rv;
}

/* Allocate a descriptor ring, initializing as much as possible.
*/
u32
au1xxx_dbdma_ring_alloc(u32 chanid, int entries)
{
	int			i;
	u32			desc_base, srcid, destid;
	u32			cmd0, cmd1, src1, dest1;
	u32			src0, dest0;
	chan_tab_t		*ctp;
	dbdev_tab_t		*stp, *dtp;
	au1x_ddma_desc_t	*dp;

	/* I guess we could check this to be within the
	 * range of the table......
	 */
	ctp = *((chan_tab_t **)chanid);
	stp = ctp->chan_src;
	dtp = ctp->chan_dest;

	/* The descriptors must be 32-byte aligned.  There is a
	 * possibility the allocation will give us such an address,
	 * and if we try that first we are likely to not waste larger
	 * slabs of memory.
	 */
	desc_base = (u32)kmalloc(entries * sizeof(au1x_ddma_desc_t), GFP_KERNEL);
	if (desc_base == 0)
		return 0;

	if (desc_base & 0x1f) {
		/* Lost....do it again, allocate extra, and round
		 * the address base.
		 */
		kfree((const void *)desc_base);
		i = entries * sizeof(au1x_ddma_desc_t);
		i += (sizeof(au1x_ddma_desc_t) - 1);
		if ((desc_base = (u32)kmalloc(i, GFP_KERNEL)) == 0)
			return 0;

		desc_base = ALIGN_ADDR(desc_base, sizeof(au1x_ddma_desc_t));
	}
	dp = (au1x_ddma_desc_t *)desc_base;

	/* Keep track of the base descriptor.
	*/
	ctp->chan_desc_base = dp;

	/* Initialize the rings with as much information as we know.
	 */
	srcid = stp - dbdev_tab;	/* Index is channel device ID */
	destid = dtp - dbdev_tab;

	cmd0 = cmd1 = src1 = dest1 = 0;
	src0 = dest0 = 0;

	cmd0 |= DSCR_CMD0_SID(srcid);
	cmd0 |= DSCR_CMD0_DID(destid);
	cmd0 |= DSCR_CMD0_IE | DSCR_CMD0_CV;
	cmd0 |= DSCR_CMD0_ST(DSCR_CMD0_ST_CURRENT);

	switch (stp->dev_devwidth) {
	case 8:
		cmd0 |= DSCR_CMD0_SW(DSCR_CMD0_BYTE);
		break;
	case 16:
		cmd0 |= DSCR_CMD0_SW(DSCR_CMD0_HALFWORD);
		break;
	case 32:
	default:
		cmd0 |= DSCR_CMD0_SW(DSCR_CMD0_WORD);
		break;
	}

	switch (dtp->dev_devwidth) {
	case 8:
		cmd0 |= DSCR_CMD0_DW(DSCR_CMD0_BYTE);
		break;
	case 16:
		cmd0 |= DSCR_CMD0_DW(DSCR_CMD0_HALFWORD);
		break;
	case 32:
	default:
		cmd0 |= DSCR_CMD0_DW(DSCR_CMD0_WORD);
		break;
	}

	/* If the device is marked as an in/out FIFO, ensure it is
	 * set non-coherent.
	 */
	if (stp->dev_flags & DEV_FLAGS_IN)
		cmd0 |= DSCR_CMD0_SN;		/* Source in fifo */
	if (dtp->dev_flags & DEV_FLAGS_OUT)
		cmd0 |= DSCR_CMD0_DN;		/* Destination out fifo */

	/* Set up source1.  For now, assume no stride and increment.
	 * A channel attribute update can change this later.
	 */
	switch (stp->dev_tsize) {
	case 1:
		src1 |= DSCR_SRC1_STS(DSCR_xTS_SIZE1);
		break;
	case 2:
		src1 |= DSCR_SRC1_STS(DSCR_xTS_SIZE2);
		break;
	case 4:
		src1 |= DSCR_SRC1_STS(DSCR_xTS_SIZE4);
		break;
	case 8:
	default:
		src1 |= DSCR_SRC1_STS(DSCR_xTS_SIZE8);
		break;
	}

	/* If source input is fifo, set static address.
	*/
	if (stp->dev_flags & DEV_FLAGS_IN) {
		src0 = stp->dev_physaddr;
		src1 |= DSCR_SRC1_SAM(DSCR_xAM_STATIC);
	}

	/* Set up dest1.  For now, assume no stride and increment.
	 * A channel attribute update can change this later.
	 */
	switch (dtp->dev_tsize) {
	case 1:
		dest1 |= DSCR_DEST1_DTS(DSCR_xTS_SIZE1);
		break;
	case 2:
		dest1 |= DSCR_DEST1_DTS(DSCR_xTS_SIZE2);
		break;
	case 4:
		dest1 |= DSCR_DEST1_DTS(DSCR_xTS_SIZE4);
		break;
	case 8:
	default:
		dest1 |= DSCR_DEST1_DTS(DSCR_xTS_SIZE8);
		break;
	}

	/* If destination output is fifo, set static address.
	*/
	if (dtp->dev_flags & DEV_FLAGS_OUT) {
		dest0 = dtp->dev_physaddr;
		dest1 |= DSCR_DEST1_DAM(DSCR_xAM_STATIC);
	}

	for (i=0; i<entries; i++) {
		dp->dscr_cmd0 = cmd0;
		dp->dscr_cmd1 = cmd1;
		dp->dscr_source0 = src0;
		dp->dscr_source1 = src1;
		dp->dscr_dest0 = dest0;
		dp->dscr_dest1 = dest1;
		dp->dscr_stat = 0;
		dp->dscr_nxtptr = DSCR_NXTPTR(virt_to_phys(dp + 1));
		dp++;
	}

	/* Make last descrptor point to the first.
	*/
	dp--;
	dp->dscr_nxtptr = DSCR_NXTPTR(virt_to_phys(ctp->chan_desc_base));
	ctp->get_ptr = ctp->put_ptr = ctp->cur_ptr = ctp->chan_desc_base;

	return (u32)(ctp->chan_desc_base);
}

/* Put a source buffer into the DMA ring.
 * This updates the source pointer and byte count.  Normally used
 * for memory to fifo transfers.
 */
u32
au1xxx_dbdma_put_source(u32 chanid, void *buf, int nbytes)
{
	chan_tab_t		*ctp;
	au1x_ddma_desc_t	*dp;

	/* I guess we could check this to be within the
	 * range of the table......
	 */
	ctp = *((chan_tab_t **)chanid);

	/* We should have multiple callers for a particular channel,
	 * an interrupt doesn't affect this pointer nor the descriptor,
	 * so no locking should be needed.
	 */
	dp = ctp->put_ptr;

	/* If the descriptor is valid, we are way ahead of the DMA
	 * engine, so just return an error condition.
	 */
	if (dp->dscr_cmd0 & DSCR_CMD0_V) {
		return 0;
	}

	/* Load up buffer address and byte count.
	*/
	dp->dscr_source0 = virt_to_phys(buf);
	dp->dscr_cmd1 = nbytes;
	dp->dscr_cmd0 |= DSCR_CMD0_V;	/* Let it rip */
	ctp->chan_ptr->ddma_dbell = 0xffffffff;	/* Make it go */

	/* Get next descriptor pointer.
	*/
	ctp->put_ptr = phys_to_virt(DSCR_GET_NXTPTR(dp->dscr_nxtptr));

	/* return something not zero.
	*/
	return nbytes;
}

/* Put a destination buffer into the DMA ring.
 * This updates the destination pointer and byte count.  Normally used
 * to place an empty buffer into the ring for fifo to memory transfers.
 */
u32
au1xxx_dbdma_put_dest(u32 chanid, void *buf, int nbytes)
{
	chan_tab_t		*ctp;
	au1x_ddma_desc_t	*dp;

	/* I guess we could check this to be within the
	 * range of the table......
	 */
	ctp = *((chan_tab_t **)chanid);

	/* We should have multiple callers for a particular channel,
	 * an interrupt doesn't affect this pointer nor the descriptor,
	 * so no locking should be needed.
	 */
	dp = ctp->put_ptr;

	/* If the descriptor is valid, we are way ahead of the DMA
	 * engine, so just return an error condition.
	 */
	if (dp->dscr_cmd0 & DSCR_CMD0_V)
		return 0;

	/* Load up buffer address and byte count.
	*/
	dp->dscr_dest0 = virt_to_phys(buf);
	dp->dscr_cmd1 = nbytes;
	dp->dscr_cmd0 |= DSCR_CMD0_V;	/* Let it rip */

	/* Get next descriptor pointer.
	*/
	ctp->put_ptr = phys_to_virt(DSCR_GET_NXTPTR(dp->dscr_nxtptr));

	/* return something not zero.
	*/
	return nbytes;
}

/* Get a destination buffer into the DMA ring.
 * Normally used to get a full buffer from the ring during fifo
 * to memory transfers.  This does not set the valid bit, you will
 * have to put another destination buffer to keep the DMA going.
 */
u32
au1xxx_dbdma_get_dest(u32 chanid, void **buf, int *nbytes)
{
	chan_tab_t		*ctp;
	au1x_ddma_desc_t	*dp;
	u32			rv;

	/* I guess we could check this to be within the
	 * range of the table......
	 */
	ctp = *((chan_tab_t **)chanid);

	/* We should have multiple callers for a particular channel,
	 * an interrupt doesn't affect this pointer nor the descriptor,
	 * so no locking should be needed.
	 */
	dp = ctp->get_ptr;

	/* If the descriptor is valid, we are way ahead of the DMA
	 * engine, so just return an error condition.
	 */
	if (dp->dscr_cmd0 & DSCR_CMD0_V)
		return 0;

	/* Return buffer address and byte count.
	*/
	*buf = (void *)(phys_to_virt(dp->dscr_dest0));
	*nbytes = dp->dscr_cmd1;
	rv = dp->dscr_stat;

	/* Get next descriptor pointer.
	*/
	ctp->get_ptr = phys_to_virt(DSCR_GET_NXTPTR(dp->dscr_nxtptr));

	/* return something not zero.
	*/
	return rv;
}

void
au1xxx_dbdma_stop(u32 chanid)
{
	chan_tab_t	*ctp;
	volatile au1x_dma_chan_t *cp;
	int halt_timeout = 0;

	ctp = *((chan_tab_t **)chanid);

	cp = ctp->chan_ptr;
	cp->ddma_cfg &= ~DDMA_CFG_EN;	/* Disable channel */
	au_sync();
	while (!(cp->ddma_stat & DDMA_STAT_H)) {
		udelay(1);
		halt_timeout++;
		if (halt_timeout > 100) {
			printk("warning: DMA channel won't halt\n");
			break;
		}
	}
	/* clear current desc valid and doorbell */
	cp->ddma_stat |= (DDMA_STAT_DB | DDMA_STAT_V);
	au_sync();
}

/* Start using the current descriptor pointer.  If the dbdma encounters
 * a not valid descriptor, it will stop.  In this case, we can just
 * continue by adding a buffer to the list and starting again.
 */
void
au1xxx_dbdma_start(u32 chanid)
{
	chan_tab_t	*ctp;
	volatile au1x_dma_chan_t *cp;

	ctp = *((chan_tab_t **)chanid);

	cp = ctp->chan_ptr;
	cp->ddma_desptr = virt_to_phys(ctp->cur_ptr);
	cp->ddma_cfg |= DDMA_CFG_EN;	/* Enable channel */
	au_sync();
	cp->ddma_dbell = 0xffffffff;	/* Make it go */
	au_sync();
}

void
au1xxx_dbdma_reset(u32 chanid)
{
	chan_tab_t		*ctp;
	au1x_ddma_desc_t	*dp;

	au1xxx_dbdma_stop(chanid);

	ctp = *((chan_tab_t **)chanid);
	ctp->get_ptr = ctp->put_ptr = ctp->cur_ptr = ctp->chan_desc_base;

	/* Run through the descriptors and reset the valid indicator.
	*/
	dp = ctp->chan_desc_base;

	do {
		dp->dscr_cmd0 &= ~DSCR_CMD0_V;
		dp = phys_to_virt(DSCR_GET_NXTPTR(dp->dscr_nxtptr));
	} while (dp != ctp->chan_desc_base);
}

u32
au1xxx_get_dma_residue(u32 chanid)
{
	chan_tab_t	*ctp;
	volatile au1x_dma_chan_t *cp;
	u32		rv;

	ctp = *((chan_tab_t **)chanid);
	cp = ctp->chan_ptr;

	/* This is only valid if the channel is stopped.
	*/
	rv = cp->ddma_bytecnt;
	au_sync();

	return rv;
}

void
au1xxx_dbdma_chan_free(u32 chanid)
{
	chan_tab_t	*ctp;
	dbdev_tab_t	*stp, *dtp;

	ctp = *((chan_tab_t **)chanid);
	stp = ctp->chan_src;
	dtp = ctp->chan_dest;

	au1xxx_dbdma_stop(chanid);

	if (ctp->chan_desc_base != NULL)
		kfree(ctp->chan_desc_base);

	stp->dev_flags &= ~DEV_FLAGS_INUSE;
	dtp->dev_flags &= ~DEV_FLAGS_INUSE;
	chan_tab_ptr[ctp->chan_index] = NULL;

	kfree(ctp);
}

static void
dbdma_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	u32	intstat;
	u32	chan_index;
	chan_tab_t		*ctp;
	au1x_ddma_desc_t	*dp;
	volatile au1x_dma_chan_t *cp;

	intstat = dbdma_gptr->ddma_intstat;
	au_sync();
	chan_index = au_ffs(intstat) - 1;

	ctp = chan_tab_ptr[chan_index];
	cp = ctp->chan_ptr;
	dp = ctp->cur_ptr;

	/* Reset interrupt.
	*/
	cp->ddma_irq = 0;
	au_sync();

	if (ctp->chan_callback)
		(ctp->chan_callback)(irq, ctp->chan_callparam, regs);

	ctp->cur_ptr = phys_to_virt(DSCR_GET_NXTPTR(dp->dscr_nxtptr));

}

static void
au1xxx_dbdma_init(void)
{
	dbdma_gptr->ddma_config = 0;
	dbdma_gptr->ddma_throttle = 0;
	dbdma_gptr->ddma_inten = 0xffff;
	au_sync();

	if (request_irq(AU1550_DDMA_INT, dbdma_interrupt, SA_INTERRUPT,
			"Au1xxx dbdma", (void *)dbdma_gptr))
		printk("Can't get 1550 dbdma irq");
}

void
au1xxx_dbdma_dump(u32 chanid)
{
	chan_tab_t		*ctp;
	au1x_ddma_desc_t	*dp;
	dbdev_tab_t		*stp, *dtp;
	volatile au1x_dma_chan_t *cp;

	ctp = *((chan_tab_t **)chanid);
	stp = ctp->chan_src;
	dtp = ctp->chan_dest;
	cp = ctp->chan_ptr;

	printk("Chan %x, stp %x (dev %d)  dtp %x (dev %d) \n",
		(u32)ctp, (u32)stp, stp - dbdev_tab, (u32)dtp, dtp - dbdev_tab);
	printk("desc base %x, get %x, put %x, cur %x\n",
		(u32)(ctp->chan_desc_base), (u32)(ctp->get_ptr),
		(u32)(ctp->put_ptr), (u32)(ctp->cur_ptr));

	printk("dbdma chan %x\n", (u32)cp);
	printk("cfg %08x, desptr %08x, statptr %08x\n",
		cp->ddma_cfg, cp->ddma_desptr, cp->ddma_statptr);
	printk("dbell %08x, irq %08x, stat %08x, bytecnt %08x\n",
		cp->ddma_dbell, cp->ddma_irq, cp->ddma_stat, cp->ddma_bytecnt);


	/* Run through the descriptors
	*/
	dp = ctp->chan_desc_base;

	do {
		printk("dp %08x, cmd0 %08x, cmd1 %08x\n",
			(u32)dp, dp->dscr_cmd0, dp->dscr_cmd1);
		printk("src0 %08x, src1 %08x, dest0 %08x\n",
			dp->dscr_source0, dp->dscr_source1, dp->dscr_dest0);
		printk("dest1 %08x, stat %08x, nxtptr %08x\n",
			dp->dscr_dest1, dp->dscr_stat, dp->dscr_nxtptr);
		dp = phys_to_virt(DSCR_GET_NXTPTR(dp->dscr_nxtptr));
	} while (dp != ctp->chan_desc_base);
}
