/*
    saa7146.o - driver for generic saa7146-based hardware

    Copyright (C) 1998-2003 Michael Hunold <michael@mihu.de>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <media/saa7146.h>

/* global variables */
struct list_head saa7146_devices;
struct semaphore saa7146_devices_lock;

static int initialized = 0;
int saa7146_num = 0;

unsigned int saa7146_debug = 0;

MODULE_PARM(saa7146_debug,"i");
MODULE_PARM_DESC(saa7146_debug, "debug level (default: 0)");

#if 0
static void dump_registers(struct saa7146_dev* dev)
{
	int i = 0;

	INFO((" @ %li jiffies:\n",jiffies));
	for(i = 0; i <= 0x148; i+=4) {
		printk("0x%03x: 0x%08x\n",i,saa7146_read(dev,i));
	}
}
#endif

/****************************************************************************
 * gpio and debi helper functions
 ****************************************************************************/

/* write "data" to the gpio-pin "pin" */
void saa7146_set_gpio(struct saa7146_dev *dev, u8 pin, u8 data)
{
	u32 value = 0;

	/* sanity check */
	if(pin > 3)
		return;

	/* read old register contents */
	value = saa7146_read(dev, GPIO_CTRL );
	
	value &= ~(0xff << (8*pin));
	value |= (data << (8*pin));

	saa7146_write(dev, GPIO_CTRL, value);
}

/* This DEBI code is based on the saa7146 Stradis driver by Nathan Laredo */
int saa7146_wait_for_debi_done(struct saa7146_dev *dev)
{
	unsigned long start;

	/* wait for registers to be programmed */
	start = jiffies;
	while (1) {
                if (saa7146_read(dev, MC2) & 2)
                        break;
		if (time_after(jiffies, start + HZ/20)) {
			DEB_S(("timed out while waiting for registers getting programmed\n"));
			return -ETIMEDOUT;
		}
	}

	/* wait for transfer to complete */
	start = jiffies;
	while (1) {
		if (!(saa7146_read(dev, PSR) & SPCI_DEBI_S))
			break;
		saa7146_read(dev, MC2);
		if (time_after(jiffies, start + HZ/4)) {
			DEB_S(("timed out while waiting for transfer completion\n"));
			return -ETIMEDOUT;
		}
	}

	return 0;
}

/****************************************************************************
 * general helper functions
 ****************************************************************************/

/* this is videobuf_vmalloc_to_sg() from video-buf.c 
   make sure virt has been allocated with vmalloc_32(), otherwise the BUG()
   may be triggered on highmem machines */
static struct scatterlist* vmalloc_to_sg(unsigned char *virt, int nr_pages)
{
	struct scatterlist *sglist;
	struct page *pg;
	int i;

	sglist = kmalloc(sizeof(struct scatterlist)*nr_pages, GFP_KERNEL);
	if (NULL == sglist)
		return NULL;
	memset(sglist,0,sizeof(struct scatterlist)*nr_pages);
	for (i = 0; i < nr_pages; i++, virt += PAGE_SIZE) {
		pg = vmalloc_to_page(virt);
		if (NULL == pg)
			goto err;
		if (PageHighMem(pg))
			BUG();
		sglist[i].page   = pg;
		sglist[i].length = PAGE_SIZE;
	}
	return sglist;
	
 err:
	kfree(sglist);
	return NULL;
}

/********************************************************************************/
/* common page table functions */

#define SAA7146_PGTABLE_SIZE 4096

char *saa7146_vmalloc_build_pgtable(struct pci_dev *pci, long length, struct saa7146_pgtable *pt)
{
	int pages = (length+PAGE_SIZE-1)/PAGE_SIZE;
	char *mem = vmalloc_32(length);
	int slen = 0;

	if (NULL == mem) {
		return NULL;
	}

	if (!(pt->slist = vmalloc_to_sg(mem, pages))) {
		vfree(mem);
		return NULL;
	}

	if (saa7146_pgtable_alloc(pci, pt)) {
		kfree(pt->slist);
		pt->slist = NULL;
		vfree(mem);
		return NULL;
	}
	
	slen = pci_map_sg(pci,pt->slist,pages,PCI_DMA_FROMDEVICE);
	if (0 != saa7146_pgtable_build_single(pci, pt, pt->slist, slen)) {
		return NULL;
	}

	return mem;
}

void saa7146_pgtable_free(struct pci_dev *pci, struct saa7146_pgtable *pt)
{
	if (NULL == pt->cpu)
		return;
	pci_free_consistent(pci, pt->size, pt->cpu, pt->dma);
	pt->cpu = NULL;
	if (NULL != pt->slist) {
		kfree(pt->slist);
		pt->slist = NULL;
	}
}

int saa7146_pgtable_alloc(struct pci_dev *pci, struct saa7146_pgtable *pt)
{
        u32          *cpu;
        dma_addr_t   dma_addr;

	cpu = pci_alloc_consistent(pci, SAA7146_PGTABLE_SIZE, &dma_addr);
	if (NULL == cpu) {
		return -ENOMEM;
	}
	pt->size = SAA7146_PGTABLE_SIZE;
	pt->cpu  = cpu;
	pt->dma  = dma_addr;

	return 0;
}

int saa7146_pgtable_build_single(struct pci_dev *pci, struct saa7146_pgtable *pt,
	struct scatterlist *list, int sglen  )
{
	u32   *ptr, fill;
	int nr_pages = 0;
	int   i,p;

	BUG_ON( 0 == sglen);

	if (list->offset > PAGE_SIZE) {
		DEB_D(("offset > PAGE_SIZE. this should not happen."));
		return -EINVAL;
	}
	
	/* if we have a user buffer, the first page may not be
	   aligned to a page boundary. */
	pt->offset = list->offset;

	ptr = pt->cpu;
	for (i = 0; i < sglen; i++, list++) {
/*
		printk("i:%d, adr:0x%08x, len:%d, offset:%d\n", i,sg_dma_address(list), sg_dma_len(list), list->offset);
*/
		for (p = 0; p * 4096 < list->length; p++, ptr++) {
			*ptr = sg_dma_address(list) + p * 4096;
			nr_pages++;
		}
	}


	/* safety; fill the page table up with the last valid page */
	fill = *(ptr-1);
	for(i=nr_pages;i<1024;i++) {
		*ptr++ = fill;
	}

/*
	ptr = pt->cpu;
	printk("offset: %d\n",pt->offset);
	for(i=0;i<5;i++) {
		printk("ptr1 %d: 0x%08x\n",i,ptr[i]);
	}
*/
	return 0;
}

/********************************************************************************/
/* gpio functions */

void saa7146_setgpio(struct saa7146_dev *dev, int port, u32 data)
{
	u32 val = 0;

        val=saa7146_read(dev,GPIO_CTRL);
        val&=~(0xff << (8*(port)));
        val|=(data)<<(8*(port));
        saa7146_write(dev, GPIO_CTRL, val);
}

/********************************************************************************/
/* interrupt handler */

static irqreturn_t interrupt_hw(int irq, void *dev_id, struct pt_regs *regs)
{
	struct saa7146_dev *dev = (struct saa7146_dev*)dev_id;
	u32 isr = 0;

	/* read out the interrupt status register */
	isr = saa7146_read(dev, ISR);

	/* is this our interrupt? */
	if ( 0 == isr ) {
		/* nope, some other device */
		return IRQ_NONE;
	}

	saa7146_write(dev, ISR, isr);

	if( 0 != (dev->ext)) {
		if( 0 != (dev->ext->irq_mask & isr )) {
			if( 0 != dev->ext->irq_func ) {
				dev->ext->irq_func(dev, &isr);
			}
			isr &= ~dev->ext->irq_mask;
		}
	}
	if (0 != (isr & (MASK_27))) {
		DEB_INT(("irq: RPS0 (0x%08x).\n",isr));
		if( 0 != dev->vv_data && 0 != dev->vv_callback) {
			dev->vv_callback(dev,isr);
		}
		isr &= ~MASK_27;
	}
	if (0 != (isr & (MASK_28))) {
		if( 0 != dev->vv_data && 0 != dev->vv_callback) {
			dev->vv_callback(dev,isr);
		}
		isr &= ~MASK_28;
	}
	if (0 != (isr & (MASK_16|MASK_17))) {
		u32 status = saa7146_read(dev, I2C_STATUS);
		if( (0x3 == (status & 0x3)) || (0 == (status & 0x1)) ) {
			IER_DISABLE(dev, MASK_16|MASK_17);
			/* only wake up if we expect something */
			if( 0 != dev->i2c_op ) {
				u32 psr = (saa7146_read(dev, PSR) >> 16) & 0x2;
				u32 ssr = (saa7146_read(dev, SSR) >> 17) & 0x1f;
				DEB_I2C(("irq: i2c, status: 0x%08x, psr:0x%02x, ssr:0x%02x).\n",status,psr,ssr));
				dev->i2c_op = 0;
				wake_up(&dev->i2c_wq);
			} else {
				DEB_I2C(("unexpected irq: i2c, status: 0x%08x, isr %#x\n",status, isr));
			}
		} else {
			DEB_I2C(("unhandled irq: i2c, status: 0x%08x, isr %#x\n",status, isr));
		}
		isr &= ~(MASK_16|MASK_17);
	}
	if( 0 != isr ) {
		ERR(("warning: interrupt enabled, but not handled properly.(0x%08x)\n",isr));
		ERR(("disabling interrupt source(s)!\n"));
		IER_DISABLE(dev,isr);
	}
	return IRQ_HANDLED;
}

/*********************************************************************************/
/* configuration-functions                                                       */

static int saa7146_init_one(struct pci_dev *pci, const struct pci_device_id *ent)
{
	unsigned long adr = 0, len = 0;
	struct saa7146_dev* dev = kmalloc (sizeof(struct saa7146_dev),GFP_KERNEL);
	
	struct saa7146_pci_extension_data *pci_ext = (struct saa7146_pci_extension_data *)ent->driver_data;
	struct saa7146_extension* ext = pci_ext->ext;
	int err = 0;
	
	if (!(dev = kmalloc (sizeof(struct saa7146_dev),GFP_KERNEL))) {
		ERR(("out of memory.\n"));
		return -ENOMEM;
	}

	/* clear out mem for sure */
	memset(dev, 0x0, sizeof(struct saa7146_dev));

	DEB_EE(("pci:%p\n",pci));

	if (pci_enable_device(pci)) {
		ERR(("pci_enable_device() failed.\n"));
		err = -EIO;
		goto pci_error;
	}

	/* enable bus-mastering */
	pci_set_master(pci);

	dev->pci = pci;
	/* get chip-revision; this is needed to enable bug-fixes */
	if( 0 > pci_read_config_dword(dev->pci, PCI_CLASS_REVISION, &dev->revision)) {
		ERR(("pci_read_config_dword() failed.\n"));
		err = -ENODEV;
		goto pci_error;
	}
	dev->revision &= 0xf;

	/* remap the memory from virtual to physical adress */
	adr = pci_resource_start(pci,0);
	len = pci_resource_len(pci,0);

	if (!request_mem_region(pci_resource_start(pci,0), pci_resource_len(pci,0), "saa7146")) {
		ERR(("request_mem_region() failed.\n"));
		err = -ENODEV;
		goto pci_error;
	}

	if (!(dev->mem = ioremap(adr,len))) {
		ERR(("ioremap() failed.\n"));
		err = -ENODEV;
		goto ioremap_error;
	}

	/* we don't do a master reset here anymore, it screws up
	   some boards that don't have an i2c-eeprom for configuration
	   values */
/*
	saa7146_write(dev, MC1, MASK_31);
*/

	/* disable all irqs */
	saa7146_write(dev, IER, 0);

	/* shut down all dma transfers */
	saa7146_write(dev, MC1, 0x00ff0000);

	/* clear out any rps-signals pending */
	saa7146_write(dev, MC2, 0xf8000000);

	/* request an interrupt for the saa7146 */
	if (request_irq(dev->pci->irq, interrupt_hw, SA_SHIRQ | SA_INTERRUPT,
			dev->name, dev))
	{
		ERR(("request_irq() failed.\n"));
		err = -ENODEV;
		goto irq_error;
	}

	/* get memory for various stuff */
	dev->d_rps0.cpu_addr = pci_alloc_consistent(dev->pci, SAA7146_RPS_MEM, &dev->d_rps0.dma_handle);	
	if( NULL == dev->d_rps0.cpu_addr ) {
		err = -ENOMEM;
		goto kmalloc_error_1;
	}
	memset(dev->d_rps0.cpu_addr, 0x0, SAA7146_RPS_MEM);

	dev->d_rps1.cpu_addr = pci_alloc_consistent(dev->pci, SAA7146_RPS_MEM, &dev->d_rps1.dma_handle);	
	if( NULL == dev->d_rps1.cpu_addr ) {
		err = -ENOMEM;
		goto kmalloc_error_2;
	}
	memset(dev->d_rps1.cpu_addr, 0x0, SAA7146_RPS_MEM);

	dev->d_i2c.cpu_addr = pci_alloc_consistent(dev->pci, SAA7146_RPS_MEM, &dev->d_i2c.dma_handle);	
	if( NULL == dev->d_i2c.cpu_addr ) {
		err = -ENOMEM;
		goto kmalloc_error_3;
	}
	memset(dev->d_i2c.cpu_addr, 0x0, SAA7146_RPS_MEM);

	/* the rest + print status message */

	/* create a nice device name */
	sprintf(&dev->name[0], "saa7146 (%d)",saa7146_num);

	INFO(("found saa7146 @ mem %p (revision %d, irq %d) (0x%04x,0x%04x).\n", dev->mem, dev->revision,dev->pci->irq,dev->pci->subsystem_vendor,dev->pci->subsystem_device));
	dev->ext = ext;

	pci_set_drvdata(pci,dev);

        init_MUTEX(&dev->lock);
	dev->int_slock = SPIN_LOCK_UNLOCKED;
	dev->slock = SPIN_LOCK_UNLOCKED;

	init_MUTEX(&dev->i2c_lock);

	dev->module = THIS_MODULE;
	init_waitqueue_head(&dev->i2c_wq);

	/* set some sane pci arbitrition values */
	saa7146_write(dev, PCI_BT_V1, 0x1c00101f); 

	if( 0 != ext->probe) {
		if( 0 != ext->probe(dev) ) {
			DEB_D(("ext->probe() failed for %p. skipping device.\n",dev));
			err = -ENODEV;
			goto probe_error;
		}
	}

	if( 0 != ext->attach(dev,pci_ext) ) {
		DEB_D(("ext->attach() failed for %p. skipping device.\n",dev));
			err = -ENODEV;
			goto attach_error;
	}

	INIT_LIST_HEAD(&dev->item);
	list_add_tail(&dev->item,&saa7146_devices);
	saa7146_num++;

	err = 0;
	goto out;
attach_error:
probe_error:
	pci_set_drvdata(pci,NULL);
	pci_free_consistent(dev->pci, SAA7146_RPS_MEM, dev->d_i2c.cpu_addr, dev->d_i2c.dma_handle);
kmalloc_error_3:
	pci_free_consistent(dev->pci, SAA7146_RPS_MEM, dev->d_rps1.cpu_addr, dev->d_rps1.dma_handle);
kmalloc_error_2:
	pci_free_consistent(dev->pci, SAA7146_RPS_MEM, dev->d_rps0.cpu_addr, dev->d_rps0.dma_handle);
kmalloc_error_1:
	free_irq(dev->pci->irq, (void *)dev);
irq_error:
	iounmap(dev->mem);
ioremap_error:
        release_mem_region(adr,len);
pci_error:
	kfree(dev);
out:
	return err;
}

static void saa7146_remove_one(struct pci_dev *pdev)
{
	struct saa7146_dev* dev = (struct saa7146_dev*) pci_get_drvdata(pdev);
	DEB_EE(("dev:%p\n",dev));

	dev->ext->detach(dev);

	/* shut down all video dma transfers */
	saa7146_write(dev, MC1, 0x00ff0000);

	/* disable all irqs, release irq-routine */
	saa7146_write(dev, IER, 0);

	free_irq(dev->pci->irq, (void *)dev);

	/* free kernel memory */
	pci_free_consistent(dev->pci, SAA7146_RPS_MEM, dev->d_i2c.cpu_addr, dev->d_i2c.dma_handle);
	pci_free_consistent(dev->pci, SAA7146_RPS_MEM, dev->d_rps1.cpu_addr, dev->d_rps1.dma_handle);
	pci_free_consistent(dev->pci, SAA7146_RPS_MEM, dev->d_rps0.cpu_addr, dev->d_rps0.dma_handle);

	iounmap(dev->mem);
	release_mem_region(pci_resource_start(dev->pci,0), pci_resource_len(dev->pci,0));

	list_del(&dev->item);
	kfree(dev);

	saa7146_num--;
}

/*********************************************************************************/
/* extension handling functions                                                  */

int saa7146_register_extension(struct saa7146_extension* ext)
{
	DEB_EE(("ext:%p\n",ext));

	if( 0 == initialized ) {
		INIT_LIST_HEAD(&saa7146_devices);
		init_MUTEX(&saa7146_devices_lock);
		initialized = 1;
	}

	ext->driver.name = ext->name;
	ext->driver.id_table = ext->pci_tbl;
	ext->driver.probe = saa7146_init_one;
	ext->driver.remove = saa7146_remove_one;

	printk("saa7146: register extension '%s'.\n",ext->name);
	return pci_module_init(&ext->driver);
}

int saa7146_unregister_extension(struct saa7146_extension* ext)
{
	DEB_EE(("ext:%p\n",ext));
	printk("saa7146: unregister extension '%s'.\n",ext->name);
	pci_unregister_driver(&ext->driver);
	return 0;
}

static int __init saa7146_init_module(void)
{
	if( 0 == initialized ) {
		INIT_LIST_HEAD(&saa7146_devices);
		init_MUTEX(&saa7146_devices_lock);
		initialized = 1;
	}
	return 0;
}

static void __exit saa7146_cleanup_module(void)
{
}

module_init(saa7146_init_module);
module_exit(saa7146_cleanup_module);

EXPORT_SYMBOL_GPL(saa7146_register_extension);
EXPORT_SYMBOL_GPL(saa7146_unregister_extension);

/* misc functions used by extension modules */
EXPORT_SYMBOL_GPL(saa7146_pgtable_alloc);
EXPORT_SYMBOL_GPL(saa7146_pgtable_free);
EXPORT_SYMBOL_GPL(saa7146_pgtable_build_single);
EXPORT_SYMBOL_GPL(saa7146_vmalloc_build_pgtable);
EXPORT_SYMBOL_GPL(saa7146_wait_for_debi_done);

EXPORT_SYMBOL_GPL(saa7146_setgpio);

EXPORT_SYMBOL_GPL(saa7146_i2c_transfer);
EXPORT_SYMBOL_GPL(saa7146_i2c_adapter_prepare);

EXPORT_SYMBOL_GPL(saa7146_debug);
EXPORT_SYMBOL_GPL(saa7146_devices);
EXPORT_SYMBOL_GPL(saa7146_devices_lock);

MODULE_AUTHOR("Michael Hunold <michael@mihu.de>");
MODULE_DESCRIPTION("driver for generic saa7146-based hardware");
MODULE_LICENSE("GPL");
