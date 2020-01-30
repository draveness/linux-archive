/* $Id: dma.c,v 1.7 1994/12/28 03:35:33 root Exp root $
 * linux/kernel/dma.c: A DMA channel allocator. Inspired by linux/kernel/irq.c.
 *
 * Written by Hennus Bergman, 1992.
 *
 * 1994/12/26: Changes by Alex Nash to fix a minor bug in /proc/dma.
 *   In the previous version the reported device could end up being wrong,
 *   if a device requested a DMA channel that was already in use.
 *   [It also happened to remove the sizeof(char *) == sizeof(int)
 *   assumption introduced because of those /proc/dma patches. -- Hennus]
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <asm/dma.h>
#include <asm/system.h>

 

/* A note on resource allocation:
 *
 * All drivers needing DMA channels, should allocate and release them
 * through the public routines `request_dma()' and `free_dma()'.
 *
 * In order to avoid problems, all processes should allocate resources in
 * the same sequence and release them in the reverse order.
 *
 * So, when allocating DMAs and IRQs, first allocate the IRQ, then the DMA.
 * When releasing them, first release the DMA, then release the IRQ.
 * If you don't, you may cause allocation requests to fail unnecessarily.
 * This doesn't really matter now, but it will once we get real semaphores
 * in the kernel.
 */


spinlock_t dma_spin_lock = SPIN_LOCK_UNLOCKED;

/*
 *	If our port doesn't define this it has no PC like DMA
 */

#ifdef MAX_DMA_CHANNELS


/* Channel n is busy iff dma_chan_busy[n].lock != 0.
 * DMA0 used to be reserved for DRAM refresh, but apparently not any more...
 * DMA4 is reserved for cascading.
 */

struct dma_chan {
	int  lock;
	const char *device_id;
};

static struct dma_chan dma_chan_busy[MAX_DMA_CHANNELS] = {
	{ 0, 0 },
	{ 0, 0 },
	{ 0, 0 },
	{ 0, 0 },
	{ 1, "cascade" },
	{ 0, 0 },
	{ 0, 0 },
	{ 0, 0 }
};

int get_dma_list(char *buf)
{
	int i, len = 0;

	for (i = 0 ; i < MAX_DMA_CHANNELS ; i++) {
		if (dma_chan_busy[i].lock) {
		    len += sprintf(buf+len, "%2d: %s\n",
				   i,
				   dma_chan_busy[i].device_id);
		}
	}
	return len;
} /* get_dma_list */


int request_dma(unsigned int dmanr, const char * device_id)
{
	if (dmanr >= MAX_DMA_CHANNELS)
		return -EINVAL;

	if (xchg(&dma_chan_busy[dmanr].lock, 1) != 0)
		return -EBUSY;

	dma_chan_busy[dmanr].device_id = device_id;

	/* old flag was 0, now contains 1 to indicate busy */
	return 0;
} /* request_dma */


void free_dma(unsigned int dmanr)
{
	if (dmanr >= MAX_DMA_CHANNELS) {
		printk("Trying to free DMA%d\n", dmanr);
		return;
	}

	if (xchg(&dma_chan_busy[dmanr].lock, 0) == 0) {
		printk("Trying to free free DMA%d\n", dmanr);
		return;
	}	

} /* free_dma */

#else

int request_dma(unsigned int dmanr, const char *device_id)
{
	return -EINVAL;
}

int free_dma(unsigned int dmanr)
{
	return -EINVAL;
}

int get_dma_list(char *buf)
{	
	strcpy(buf, "No DMA\n");
	return 7;
}
#endif
