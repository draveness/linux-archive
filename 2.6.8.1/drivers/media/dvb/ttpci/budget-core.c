/*
 * budget-core.c: driver for the SAA7146 based Budget DVB cards
 *
 * Compiled from various sources by Michael Hunold <michael@mihu.de>
 *
 * Copyright (C) 2002 Ralph Metzler <rjkm@metzlerbros.de>
 *
 * Copyright (C) 1999-2002 Ralph  Metzler
 *			 & Marcus Metzler for convergence integrated media GmbH
 *
 * 26feb2004 Support for FS Activy Card (Grundig tuner) by
 *	     Michael Dreher <michael@5dot1.de>,
 *	     Oliver Endriss <o.endriss@gmx.de>,
 *	     Andreas 'randy' Weinberger
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 * Or, point your browser to http://www.gnu.org/copyleft/gpl.html
 *
 *
 * the project's page is at http://www.linuxtv.org/dvb/
 */

#include "budget.h"
#include "ttpci-eeprom.h"

int budget_debug = 0;

/****************************************************************************
 * TT budget / WinTV Nova
 ****************************************************************************/

static int stop_ts_capture(struct budget *budget)
{
	DEB_EE(("budget: %p\n",budget));

        if (--budget->feeding)
                return budget->feeding;

        saa7146_write(budget->dev, MC1, MASK_20); // DMA3 off
	IER_DISABLE(budget->dev, MASK_10);
        return 0;
}


static int start_ts_capture (struct budget *budget)
{
        struct saa7146_dev *dev=budget->dev;

	DEB_EE(("budget: %p\n",budget));

        if (budget->feeding) 
                return ++budget->feeding;

      	saa7146_write(dev, MC1, MASK_20); // DMA3 off

        memset(budget->grabbing, 0x00, TS_HEIGHT*TS_WIDTH);

        saa7146_write(dev, PCI_BT_V1, 0x001c0000 |
            (saa7146_read(dev, PCI_BT_V1) & ~0x001f0000));

        budget->tsf=0xff;
        budget->ttbp=0;

	/*
	 *  Signal path on the Activy:
	 *
	 *  tuner -> SAA7146 port A -> SAA7146 BRS -> SAA7146 DMA3 -> memory
	 *
	 *  Since the tuner feeds 204 bytes packets into the SAA7146,
	 *  DMA3 is configured to strip the trailing 16 FEC bytes:
	 *	Pitch: 188, NumBytes3: 188, NumLines3: 1024
	 */

	if (budget->card->type == BUDGET_FS_ACTIVY) {
		saa7146_write(dev, DD1_INIT, 0x04000000);
		saa7146_write(dev, MC2, (MASK_09 | MASK_25));
		saa7146_write(dev, BRS_CTRL, 0x00000000);
	} else {
		if (budget->video_port == BUDGET_VIDEO_PORTA) {
			saa7146_write(dev, DD1_INIT, 0x06000200);
			saa7146_write(dev, MC2, (MASK_09 | MASK_25 | MASK_10 | MASK_26));
			saa7146_write(dev, BRS_CTRL, 0x00000000);
		} else {
        saa7146_write(dev, DD1_INIT, 0x02000600);
        saa7146_write(dev, MC2, (MASK_09 | MASK_25 | MASK_10 | MASK_26));
        saa7146_write(dev, BRS_CTRL, 0x60000000);	
		}
	}

      	saa7146_write(dev, MC2, (MASK_08 | MASK_24));
        mdelay(10);

        saa7146_write(dev, BASE_ODD3, 0);
        saa7146_write(dev, BASE_EVEN3, 0);
        saa7146_write(dev, PROT_ADDR3, TS_WIDTH*TS_HEIGHT);	
        saa7146_write(dev, BASE_PAGE3, budget->pt.dma |ME1|0x90);

	if (budget->card->type == BUDGET_FS_ACTIVY) {
		saa7146_write(dev, PITCH3, TS_WIDTH/2);
		saa7146_write(dev, NUM_LINE_BYTE3, ((TS_HEIGHT*2)<<16)|(TS_WIDTH/2));
	} else {
		saa7146_write(dev, PITCH3, TS_WIDTH);
        saa7146_write(dev, NUM_LINE_BYTE3, (TS_HEIGHT<<16)|TS_WIDTH);
	}

      	saa7146_write(dev, MC2, (MASK_04 | MASK_20));
     	saa7146_write(dev, MC1, (MASK_04 | MASK_20)); // DMA3 on

	IER_ENABLE(budget->dev, MASK_10); // VPE

        return ++budget->feeding;
}


static void vpeirq (unsigned long data)
{
        struct budget *budget = (struct budget*) data;
        u8 *mem = (u8 *)(budget->grabbing);
        u32 olddma = budget->ttbp;
        u32 newdma = saa7146_read(budget->dev, PCI_VDP3);

        /* nearest lower position divisible by 188 */
        newdma -= newdma % 188;

        if (newdma >= TS_BUFLEN)
                return;

	budget->ttbp = newdma;
	
	if(budget->feeding == 0 || newdma == olddma)
		return;

        if (newdma > olddma) { /* no wraparound, dump olddma..newdma */
                        dvb_dmx_swfilter_packets(&budget->demux, 
        	                mem+olddma, (newdma-olddma) / 188);
        } else { /* wraparound, dump olddma..buflen and 0..newdma */
	                dvb_dmx_swfilter_packets(&budget->demux,
        	                mem+olddma, (TS_BUFLEN-olddma) / 188);
                        dvb_dmx_swfilter_packets(&budget->demux,
                                mem, newdma / 188);
        }
}


/****************************************************************************
 * DVB API SECTION
 ****************************************************************************/

static int budget_start_feed(struct dvb_demux_feed *feed)
{
        struct dvb_demux *demux = feed->demux;
        struct budget *budget = (struct budget*) demux->priv;
	int status;

	DEB_EE(("budget: %p\n",budget));

        if (!demux->dmx.frontend)
                return -EINVAL;

   	spin_lock(&budget->feedlock);   
	status = start_ts_capture (budget);
   	spin_unlock(&budget->feedlock);
	return status;
}

static int budget_stop_feed(struct dvb_demux_feed *feed)
{
        struct dvb_demux *demux = feed->demux;
        struct budget *budget = (struct budget *) demux->priv;
	int status;

	DEB_EE(("budget: %p\n",budget));

   	spin_lock(&budget->feedlock);
	status = stop_ts_capture (budget);
   	spin_unlock(&budget->feedlock);
	return status;
}


static int budget_register(struct budget *budget)
{
        struct dvb_demux *dvbdemux=&budget->demux;
        int ret;

	DEB_EE(("budget: %p\n",budget));

        dvbdemux->priv = (void *) budget;

	dvbdemux->filternum = 256;
        dvbdemux->feednum = 256;
        dvbdemux->start_feed = budget_start_feed;
        dvbdemux->stop_feed = budget_stop_feed;
        dvbdemux->write_to_decoder = NULL;

        dvbdemux->dmx.capabilities = (DMX_TS_FILTERING | DMX_SECTION_FILTERING |
                                      DMX_MEMORY_BASED_FILTERING);

        dvb_dmx_init(&budget->demux);

        budget->dmxdev.filternum = 256;
        budget->dmxdev.demux = &dvbdemux->dmx;
        budget->dmxdev.capabilities = 0;

        dvb_dmxdev_init(&budget->dmxdev, budget->dvb_adapter);

        budget->hw_frontend.source = DMX_FRONTEND_0;

        ret = dvbdemux->dmx.add_frontend(&dvbdemux->dmx, &budget->hw_frontend);

        if (ret < 0)
                return ret;
        
        budget->mem_frontend.source = DMX_MEMORY_FE;
        ret=dvbdemux->dmx.add_frontend (&dvbdemux->dmx, 
                                        &budget->mem_frontend);
        if (ret<0)
                return ret;
        
        ret=dvbdemux->dmx.connect_frontend (&dvbdemux->dmx, 
                                            &budget->hw_frontend);
        if (ret < 0)
                return ret;

        dvb_net_init(budget->dvb_adapter, &budget->dvb_net, &dvbdemux->dmx);

	return 0;
}


static void budget_unregister(struct budget *budget)
{
        struct dvb_demux *dvbdemux=&budget->demux;

	DEB_EE(("budget: %p\n",budget));

	dvb_net_release(&budget->dvb_net);

	dvbdemux->dmx.close(&dvbdemux->dmx);
        dvbdemux->dmx.remove_frontend(&dvbdemux->dmx, &budget->hw_frontend);
        dvbdemux->dmx.remove_frontend(&dvbdemux->dmx, &budget->mem_frontend);

        dvb_dmxdev_release(&budget->dmxdev);
        dvb_dmx_release(&budget->demux);
}


static int master_xfer (struct dvb_i2c_bus *i2c, const struct i2c_msg msgs[], int num)
{
	struct saa7146_dev *dev = i2c->data;
	return saa7146_i2c_transfer(dev, msgs, num, 6);
}


int ttpci_budget_init (struct budget *budget,
		       struct saa7146_dev* dev,
		       struct saa7146_pci_extension_data *info)
{
	int length = TS_WIDTH*TS_HEIGHT;
	int ret = 0;
	struct budget_info *bi = info->ext_priv;

	memset(budget, 0, sizeof(struct budget));

	DEB_EE(("dev: %p, budget: %p\n", dev, budget));

	budget->card = bi;
	budget->dev = (struct saa7146_dev *) dev;

	dvb_register_adapter(&budget->dvb_adapter, budget->card->name, THIS_MODULE);

	/* set dd1 stream a & b */
      	saa7146_write(dev, DD1_STREAM_B, 0x00000000);
	saa7146_write(dev, MC2, (MASK_09 | MASK_25));
	saa7146_write(dev, MC2, (MASK_10 | MASK_26));
	saa7146_write(dev, DD1_INIT, 0x02000000);
	saa7146_write(dev, MC2, (MASK_09 | MASK_25 | MASK_10 | MASK_26));

       	if (bi->type != BUDGET_FS_ACTIVY)
		budget->video_port = BUDGET_VIDEO_PORTB;
	else
		budget->video_port = BUDGET_VIDEO_PORTA;
	spin_lock_init(&budget->feedlock);

	/* the Siemens DVB needs this if you want to have the i2c chips
           get recognized before the main driver is loaded */
	if (bi->type != BUDGET_FS_ACTIVY)
		saa7146_write(dev, GPIO_CTRL, 0x500000); /* GPIO 3 = 1 */
	
	saa7146_i2c_adapter_prepare(dev, NULL, 0, SAA7146_I2C_BUS_BIT_RATE_120);

	budget->i2c_bus = dvb_register_i2c_bus (master_xfer, dev,
						budget->dvb_adapter, 0);

	if (!budget->i2c_bus) {
		dvb_unregister_adapter (budget->dvb_adapter);
		return -ENOMEM;
	}

	ttpci_eeprom_parse_mac(budget->i2c_bus);

	if( NULL == (budget->grabbing = saa7146_vmalloc_build_pgtable(dev->pci,length,&budget->pt))) {
		ret = -ENOMEM;
		goto err;
	}

	saa7146_write(dev, PCI_BT_V1, 0x001c0000);
	/* upload all */
        saa7146_write(dev, GPIO_CTRL, 0x000000);

	tasklet_init (&budget->vpe_tasklet, vpeirq, (unsigned long) budget);

	/* frontend power on */
	if (bi->type == BUDGET_FS_ACTIVY)
		saa7146_setgpio(dev, 1, SAA7146_GPIO_OUTHI);
	else
		saa7146_setgpio(dev, 2, SAA7146_GPIO_OUTHI);

        if (budget_register(budget) == 0) {
		return 0;
	}
err:
	if (budget->grabbing)
		vfree(budget->grabbing);

	dvb_unregister_i2c_bus (master_xfer,budget->i2c_bus->adapter,
				budget->i2c_bus->id);

	dvb_unregister_adapter (budget->dvb_adapter);

	return ret;
}


int ttpci_budget_deinit (struct budget *budget)
{
	struct saa7146_dev *dev = budget->dev;

	DEB_EE(("budget: %p\n", budget));

	budget_unregister (budget);

	dvb_unregister_i2c_bus (master_xfer, budget->i2c_bus->adapter,
				budget->i2c_bus->id);

	dvb_unregister_adapter (budget->dvb_adapter);

	tasklet_kill (&budget->vpe_tasklet);

	saa7146_pgtable_free (dev->pci, &budget->pt);

	vfree (budget->grabbing);

	return 0;
}

void ttpci_budget_irq10_handler (struct saa7146_dev* dev, u32 *isr) 
{
	struct budget *budget = (struct budget*)dev->ext_priv;

	DEB_EE(("dev: %p, budget: %p\n",dev,budget));

	if (*isr & MASK_10)
		tasklet_schedule (&budget->vpe_tasklet);
}

void ttpci_budget_set_video_port(struct saa7146_dev* dev, int video_port)
{
	struct budget *budget = (struct budget*)dev->ext_priv;

	spin_lock(&budget->feedlock);
	budget->video_port = video_port;
	if (budget->feeding) {
		int oldfeeding = budget->feeding;
	   	budget->feeding = 1;
		stop_ts_capture(budget);
		start_ts_capture(budget);
	   	budget->feeding = oldfeeding;
	}
   	spin_unlock(&budget->feedlock);
}



EXPORT_SYMBOL_GPL(ttpci_budget_init);
EXPORT_SYMBOL_GPL(ttpci_budget_deinit);
EXPORT_SYMBOL_GPL(ttpci_budget_irq10_handler);
EXPORT_SYMBOL_GPL(ttpci_budget_set_video_port);
EXPORT_SYMBOL_GPL(budget_debug);

MODULE_PARM(budget_debug,"i");
MODULE_LICENSE("GPL");


