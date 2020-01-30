/*
 *  ALSA card-level driver for Turtle Beach Wavefront cards 
 *                                              (Maui,Tropez,Tropez+)
 *
 *  Copyright (c) 1997-1999 by Paul Barton-Davis <pbd@op.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <sound/driver.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/pnp.h>
#include <linux/moduleparam.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/opl3.h>
#include <sound/snd_wavefront.h>

#define chip_t cs4231_t

MODULE_AUTHOR("Paul Barton-Davis <pbd@op.net>");
MODULE_DESCRIPTION("Turtle Beach Wavefront");
MODULE_LICENSE("GPL");
MODULE_CLASSES("{sound}");
MODULE_DEVICES("{{Turtle Beach,Maui/Tropez/Tropez+}}");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;	    /* Index 0-MAX */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;	    /* ID for this card */
static int enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE;	    /* Enable this card */
static int isapnp[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = 1};
static long cs4232_pcm_port[SNDRV_CARDS] = SNDRV_DEFAULT_PORT;	/* PnP setup */
static int cs4232_pcm_irq[SNDRV_CARDS] = SNDRV_DEFAULT_IRQ; /* 5,7,9,11,12,15 */
static long cs4232_mpu_port[SNDRV_CARDS] = SNDRV_DEFAULT_PORT; /* PnP setup */
static int cs4232_mpu_irq[SNDRV_CARDS] = SNDRV_DEFAULT_IRQ; /* 9,11,12,15 */
static long ics2115_port[SNDRV_CARDS] = SNDRV_DEFAULT_PORT; /* PnP setup */
static int ics2115_irq[SNDRV_CARDS] = SNDRV_DEFAULT_IRQ;    /* 2,9,11,12,15 */
static long fm_port[SNDRV_CARDS] = SNDRV_DEFAULT_PORT;	    /* PnP setup */
static int dma1[SNDRV_CARDS] = SNDRV_DEFAULT_DMA;	    /* 0,1,3,5,6,7 */
static int dma2[SNDRV_CARDS] = SNDRV_DEFAULT_DMA;	    /* 0,1,3,5,6,7 */
static int use_cs4232_midi[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = 0}; 
static int boot_devs;

module_param_array(index, int, boot_devs, 0444);
MODULE_PARM_DESC(index, "Index value for WaveFront soundcard.");
MODULE_PARM_SYNTAX(index, SNDRV_INDEX_DESC);
module_param_array(id, charp, boot_devs, 0444);
MODULE_PARM_DESC(id, "ID string for WaveFront soundcard.");
MODULE_PARM_SYNTAX(id, SNDRV_ID_DESC);
module_param_array(enable, bool, boot_devs, 0444);
MODULE_PARM_DESC(enable, "Enable WaveFront soundcard.");
MODULE_PARM_SYNTAX(enable, SNDRV_ENABLE_DESC);
#ifdef CONFIG_PNP
module_param_array(isapnp, bool, boot_devs, 0444);
MODULE_PARM_DESC(isapnp, "ISA PnP detection for WaveFront soundcards.");
MODULE_PARM_SYNTAX(isapnp, SNDRV_ISAPNP_DESC);
#endif
module_param_array(cs4232_pcm_port, long, boot_devs, 0444);
MODULE_PARM_DESC(cs4232_pcm_port, "Port # for CS4232 PCM interface.");
MODULE_PARM_SYNTAX(cs4232_pcm_port, SNDRV_PORT12_DESC);
module_param_array(cs4232_pcm_irq, int, boot_devs, 0444);
MODULE_PARM_DESC(cs4232_pcm_irq, "IRQ # for CS4232 PCM interface.");
MODULE_PARM_SYNTAX(cs4232_pcm_irq, SNDRV_ENABLED ",allows:{{5},{7},{9},{11},{12},{15}},dialog:list");
module_param_array(dma1, int, boot_devs, 0444);
MODULE_PARM_DESC(dma1, "DMA1 # for CS4232 PCM interface.");
MODULE_PARM_SYNTAX(dma1, SNDRV_DMA_DESC);
module_param_array(dma2, int, boot_devs, 0444);
MODULE_PARM_DESC(dma2, "DMA2 # for CS4232 PCM interface.");
MODULE_PARM_SYNTAX(dma2, SNDRV_DMA_DESC);
module_param_array(cs4232_mpu_port, long, boot_devs, 0444);
MODULE_PARM_DESC(cs4232_mpu_port, "port # for CS4232 MPU-401 interface.");
MODULE_PARM_SYNTAX(cs4232_mpu_port, SNDRV_PORT12_DESC);
module_param_array(cs4232_mpu_irq, int, boot_devs, 0444);
MODULE_PARM_DESC(cs4232_mpu_irq, "IRQ # for CS4232 MPU-401 interface.");
MODULE_PARM_SYNTAX(cs4232_mpu_irq, SNDRV_ENABLED ",allows:{{9},{11},{12},{15}},dialog:list");
module_param_array(ics2115_irq, int, boot_devs, 0444);
MODULE_PARM_DESC(ics2115_irq, "IRQ # for ICS2115.");
MODULE_PARM_SYNTAX(ics2115_irq, SNDRV_ENABLED ",allows:{{9},{11},{12},{15}},dialog:list");
module_param_array(ics2115_port, long, boot_devs, 0444);
MODULE_PARM_DESC(ics2115_port, "Port # for ICS2115.");
MODULE_PARM_SYNTAX(ics2115_port, SNDRV_PORT12_DESC);
module_param_array(fm_port, long, boot_devs, 0444);
MODULE_PARM_DESC(fm_port, "FM port #.");
MODULE_PARM_SYNTAX(fm_port, SNDRV_PORT12_DESC);
module_param_array(use_cs4232_midi, bool, boot_devs, 0444);
MODULE_PARM_DESC(use_cs4232_midi, "Use CS4232 MPU-401 interface (inaccessibly located inside your computer)");
MODULE_PARM_SYNTAX(use_cs4232_midi, SNDRV_ENABLED "," SNDRV_BOOLEAN_FALSE_DESC);

static snd_card_t *snd_wavefront_legacy[SNDRV_CARDS] = SNDRV_DEFAULT_PTR;

#ifdef CONFIG_PNP

static struct pnp_card_device_id snd_wavefront_pnpids[] = {
	/* Tropez */
	{ .id = "CSC7532", .devs = { { "CSC0000" }, { "CSC0010" }, { "PnPb006" }, { "CSC0004" } } },
	/* Tropez+ */
	{ .id = "CSC7632", .devs = { { "CSC0000" }, { "CSC0010" }, { "PnPb006" }, { "CSC0004" } } },
	{ .id = "" }
};

MODULE_DEVICE_TABLE(pnp_card, snd_wavefront_pnpids);

static int __devinit
snd_wavefront_pnp (int dev, snd_wavefront_card_t *acard, struct pnp_card_link *card,
		   const struct pnp_card_device_id *id)
{
	struct pnp_dev *pdev;
	struct pnp_resource_table *cfg = kmalloc(sizeof(*cfg), GFP_KERNEL);
	int err;

	if (!cfg)
		return -ENOMEM;

	/* Check for each logical device. */

	/* CS4232 chip (aka "windows sound system") is logical device 0 */

	acard->wss = pnp_request_card_device(card, id->devs[0].id, NULL);
	if (acard->wss == NULL) {
		kfree(cfg);
		return -EBUSY;
	}

	/* there is a game port at logical device 1, but we ignore it completely */

	/* the control interface is logical device 2, but we ignore it
	   completely. in fact, nobody even seems to know what it
	   does.
	*/

	/* Only configure the CS4232 MIDI interface if its been
	   specifically requested. It is logical device 3.
	*/

	if (use_cs4232_midi[dev]) {
		acard->mpu = pnp_request_card_device(card, id->devs[2].id, NULL);
		if (acard->mpu == NULL) {
			kfree(cfg);
			return -EBUSY;
		}
	}

	/* The ICS2115 synth is logical device 4 */

	acard->synth = pnp_request_card_device(card, id->devs[3].id, NULL);
	if (acard->synth == NULL) {
		kfree(cfg);
		return -EBUSY;
	}

	/* PCM/FM initialization */

	pdev = acard->wss;

	pnp_init_resource_table(cfg);

	/* An interesting note from the Tropez+ FAQ:

	   Q. [Ports] Why is the base address of the WSS I/O ports off by 4?

	   A. WSS I/O requires a block of 8 I/O addresses ("ports"). Of these, the first
	   4 are used to identify and configure the board. With the advent of PnP,
	   these first 4 addresses have become obsolete, and software applications
	   only use the last 4 addresses to control the codec chip. Therefore, the
	   base address setting "skips past" the 4 unused addresses.

	*/

	if (cs4232_pcm_port[dev] != SNDRV_AUTO_PORT)
		pnp_resource_change(&cfg->port_resource[0], cs4232_pcm_port[dev], 4);
	if (fm_port[dev] != SNDRV_AUTO_PORT)
		pnp_resource_change(&cfg->port_resource[1], fm_port[dev], 4);
	if (dma1[dev] != SNDRV_AUTO_DMA)
		pnp_resource_change(&cfg->dma_resource[0], dma1[dev], 1);
	if (dma2[dev] != SNDRV_AUTO_DMA)
		pnp_resource_change(&cfg->dma_resource[1], dma2[dev], 1);
	if (cs4232_pcm_irq[dev] != SNDRV_AUTO_IRQ)
		pnp_resource_change(&cfg->irq_resource[0], cs4232_pcm_irq[dev], 1);

	if (pnp_manual_config_dev(pdev, cfg, 0) < 0)
		snd_printk(KERN_ERR "PnP WSS the requested resources are invalid, using auto config\n");
	err = pnp_activate_dev(pdev);
	if (err < 0) {
		snd_printk(KERN_ERR "PnP WSS pnp configure failure\n");
		kfree(cfg);
		return err;
	}

	cs4232_pcm_port[dev] = pnp_port_start(pdev, 0);
	fm_port[dev] = pnp_port_start(pdev, 1);
	dma1[dev] = pnp_dma(pdev, 0);
	dma2[dev] = pnp_dma(pdev, 1);
	cs4232_pcm_irq[dev] = pnp_irq(pdev, 0);

	/* Synth initialization */

	pdev = acard->synth;
	
	pnp_init_resource_table(cfg);

	if (ics2115_port[dev] != SNDRV_AUTO_PORT) {
		pnp_resource_change(&cfg->port_resource[0], ics2115_port[dev], 16);
	}
		
	if (ics2115_port[dev] != SNDRV_AUTO_IRQ) {
		pnp_resource_change(&cfg->irq_resource[0], ics2115_irq[dev], 1);
	}

	if (pnp_manual_config_dev(pdev, cfg, 0) < 0)
		snd_printk(KERN_ERR "PnP ICS2115 the requested resources are invalid, using auto config\n");
	err = pnp_activate_dev(pdev);
	if (err < 0) {
		snd_printk(KERN_ERR "PnP ICS2115 pnp configure failure\n");
		kfree(cfg);
		return err;
	}

	ics2115_port[dev] = pnp_port_start(pdev, 0);
	ics2115_irq[dev] = pnp_irq(pdev, 0);

	/* CS4232 MPU initialization. Configure this only if
	   explicitly requested, since its physically inaccessible and
	   consumes another IRQ.
	*/

	if (use_cs4232_midi[dev]) {

		pdev = acard->mpu;

		pnp_init_resource_table(cfg);

		if (cs4232_mpu_port[dev] != SNDRV_AUTO_PORT)
			pnp_resource_change(&cfg->port_resource[0], cs4232_mpu_port[dev], 2);
		if (cs4232_mpu_irq[dev] != SNDRV_AUTO_IRQ)
			pnp_resource_change(&cfg->port_resource[0], cs4232_mpu_irq[dev], 1);

		if (pnp_manual_config_dev(pdev, cfg, 0) < 0)
			snd_printk(KERN_ERR "PnP MPU401 the requested resources are invalid, using auto config\n");
		err = pnp_activate_dev(pdev);
		if (err < 0) {
			snd_printk(KERN_ERR "PnP MPU401 pnp configure failure\n");
			cs4232_mpu_port[dev] = SNDRV_AUTO_PORT;
		} else {
			cs4232_mpu_port[dev] = pnp_port_start(pdev, 0);
			cs4232_mpu_irq[dev] = pnp_irq(pdev, 0);
		}

		snd_printk ("CS4232 MPU: port=0x%lx, irq=%i\n", 
			    cs4232_mpu_port[dev], 
			    cs4232_mpu_irq[dev]);
	}

	snd_printdd ("CS4232: pcm port=0x%lx, fm port=0x%lx, dma1=%i, dma2=%i, irq=%i\nICS2115: port=0x%lx, irq=%i\n", 
		    cs4232_pcm_port[dev], 
		    fm_port[dev],
		    dma1[dev], 
		    dma2[dev], 
		    cs4232_pcm_irq[dev],
		    ics2115_port[dev], 
		    ics2115_irq[dev]);
	
	kfree(cfg);
	return 0;
}

#endif /* CONFIG_PNP */

static irqreturn_t snd_wavefront_ics2115_interrupt(int irq, 
					    void *dev_id, 
					    struct pt_regs *regs)
{
	snd_wavefront_card_t *acard;

	acard = (snd_wavefront_card_t *) dev_id;

	if (acard == NULL) 
		return IRQ_NONE;

	if (acard->wavefront.interrupts_are_midi) {
		snd_wavefront_midi_interrupt (acard);
	} else {
		snd_wavefront_internal_interrupt (acard);
	}
	return IRQ_HANDLED;
}

snd_hwdep_t * __devinit
snd_wavefront_new_synth (snd_card_t *card,
			 int hw_dev,
			 snd_wavefront_card_t *acard)
{
	snd_hwdep_t *wavefront_synth;

	if (snd_wavefront_detect (acard) < 0) {
		return NULL;
	}

	if (snd_wavefront_start (&acard->wavefront) < 0) {
		return NULL;
	}

	if (snd_hwdep_new(card, "WaveFront", hw_dev, &wavefront_synth) < 0)
		return NULL;
	strcpy (wavefront_synth->name, 
		"WaveFront (ICS2115) wavetable synthesizer");
	wavefront_synth->ops.open = snd_wavefront_synth_open;
	wavefront_synth->ops.release = snd_wavefront_synth_release;
	wavefront_synth->ops.ioctl = snd_wavefront_synth_ioctl;

	return wavefront_synth;
}

snd_hwdep_t * __devinit
snd_wavefront_new_fx (snd_card_t *card,
		      int hw_dev,
		      snd_wavefront_card_t *acard,
		      unsigned long port)

{
	snd_hwdep_t *fx_processor;

	if (snd_wavefront_fx_start (&acard->wavefront)) {
		snd_printk ("cannot initialize YSS225 FX processor");
		return NULL;
	}

	if (snd_hwdep_new (card, "YSS225", hw_dev, &fx_processor) < 0)
		return NULL;
	sprintf (fx_processor->name, "YSS225 FX Processor at 0x%lx", port);
	fx_processor->ops.open = snd_wavefront_fx_open;
	fx_processor->ops.release = snd_wavefront_fx_release;
	fx_processor->ops.ioctl = snd_wavefront_fx_ioctl;
	
	return fx_processor;
}

static snd_wavefront_mpu_id internal_id = internal_mpu;
static snd_wavefront_mpu_id external_id = external_mpu;

snd_rawmidi_t * __devinit
snd_wavefront_new_midi (snd_card_t *card,
			int midi_dev,
			snd_wavefront_card_t *acard,
			unsigned long port,
			snd_wavefront_mpu_id mpu)

{
	snd_rawmidi_t *rmidi;
	static int first = 1;

	if (first) {
		first = 0;
		acard->wavefront.midi.base = port;
		if (snd_wavefront_midi_start (acard)) {
			snd_printk ("cannot initialize MIDI interface\n");
			return NULL;
		}
	}

	if (snd_rawmidi_new (card, "WaveFront MIDI", midi_dev, 1, 1, &rmidi) < 0)
		return NULL;

	if (mpu == internal_mpu) {
		strcpy(rmidi->name, "WaveFront MIDI (Internal)");
		rmidi->private_data = &internal_id;
	} else {
		strcpy(rmidi->name, "WaveFront MIDI (External)");
		rmidi->private_data = &external_id;
	}

	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &snd_wavefront_midi_output);
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &snd_wavefront_midi_input);

	rmidi->info_flags |= SNDRV_RAWMIDI_INFO_OUTPUT |
	                     SNDRV_RAWMIDI_INFO_INPUT |
	                     SNDRV_RAWMIDI_INFO_DUPLEX;

	return rmidi;
}

static void
snd_wavefront_free(snd_card_t *card)
{
	snd_wavefront_card_t *acard = (snd_wavefront_card_t *)card->private_data;
	
	if (acard) {
		if (acard->wavefront.res_base != NULL) {
			release_resource(acard->wavefront.res_base);
			kfree_nocheck(acard->wavefront.res_base);
		}
		if (acard->wavefront.irq > 0)
			free_irq(acard->wavefront.irq, (void *)acard);
	}
}

static int __devinit
snd_wavefront_probe (int dev, struct pnp_card_link *pcard,
		     const struct pnp_card_device_id *pid)
{
	snd_card_t *card;
	snd_wavefront_card_t *acard;
	cs4231_t *chip;
	snd_hwdep_t *wavefront_synth;
	snd_rawmidi_t *ics2115_internal_rmidi = NULL;
	snd_rawmidi_t *ics2115_external_rmidi = NULL;
	snd_hwdep_t *fx_processor;
	int hw_dev = 0, midi_dev = 0, err;

#ifdef CONFIG_PNP
	if (!isapnp[dev]) {
#endif
		if (cs4232_pcm_port[dev] == SNDRV_AUTO_PORT) {
			snd_printk("specify CS4232 port\n");
			return -EINVAL;
		}
		if (ics2115_port[dev] == SNDRV_AUTO_PORT) {
			snd_printk("specify ICS2115 port\n");
			return -ENODEV;
		}
#ifdef CONFIG_PNP
	}
#endif
	card = snd_card_new (index[dev], 
			     id[dev],
			     THIS_MODULE,
			     sizeof(snd_wavefront_card_t));

	if (card == NULL) {
		return -ENOMEM;
	}
	acard = (snd_wavefront_card_t *)card->private_data;
	acard->wavefront.irq = -1;
	spin_lock_init(&acard->wavefront.irq_lock);
	init_waitqueue_head(&acard->wavefront.interrupt_sleeper);
	spin_lock_init(&acard->wavefront.midi.open);
	spin_lock_init(&acard->wavefront.midi.virtual);
	card->private_free = snd_wavefront_free;

#ifdef CONFIG_PNP
	if (isapnp[dev]) {
		if (snd_wavefront_pnp (dev, acard, pcard, pid) < 0) {
			if (cs4232_pcm_port[dev] == SNDRV_AUTO_PORT) {
				snd_printk ("isapnp detection failed\n");
				snd_card_free (card);
				return -ENODEV;
			}
		}
		snd_card_set_dev(card, &pcard->card->dev);
	}
#endif /* CONFIG_PNP */

	/* --------- PCM --------------- */

	if ((err = snd_cs4231_create (card,
				      cs4232_pcm_port[dev],
				      -1,
				      cs4232_pcm_irq[dev],
				      dma1[dev],
				      dma2[dev],
				      CS4231_HW_DETECT, 0, &chip)) < 0) {
		snd_card_free(card);
		snd_printk ("can't allocate CS4231 device\n");
		return err;
	}

	if ((err = snd_cs4231_pcm (chip, 0, NULL)) < 0) {
		snd_card_free(card);
		return err;
	}
	if ((err = snd_cs4231_timer (chip, 0, NULL)) < 0) {
		snd_card_free(card);
		return err;
	}

	/* ---------- OPL3 synth --------- */

	if (fm_port[dev] > 0 && fm_port[dev] != SNDRV_AUTO_PORT) {
		opl3_t *opl3;

	        if ((err = snd_opl3_create(card,
					   fm_port[dev],
					   fm_port[dev] + 2,
					   OPL3_HW_OPL3_CS,
					   0, &opl3)) < 0) {
			snd_printk ("can't allocate or detect OPL3 synth\n");
			snd_card_free(card);
			return err;
		}

		if ((err = snd_opl3_hwdep_new(opl3, hw_dev, 1, NULL)) < 0) {
			snd_card_free(card);
			return err;
		}
		hw_dev++;
	}

	/* ------- ICS2115 Wavetable synth ------- */

	if ((acard->wavefront.res_base = request_region(ics2115_port[dev], 16, "ICS2115")) == NULL) {
		snd_printk("unable to grab ICS2115 i/o region 0x%lx-0x%lx\n", ics2115_port[dev], ics2115_port[dev] + 16 - 1);
		snd_card_free(card);
		return -EBUSY;
	}
	if (request_irq(ics2115_irq[dev], snd_wavefront_ics2115_interrupt, SA_INTERRUPT, "ICS2115", (void *)acard)) {
		snd_printk("unable to use ICS2115 IRQ %d\n", ics2115_irq[dev]);
		snd_card_free(card);
		return -EBUSY;
	}
	
	acard->wavefront.irq = ics2115_irq[dev];
	acard->wavefront.base = ics2115_port[dev];

	if ((wavefront_synth = snd_wavefront_new_synth (card, hw_dev, acard)) == NULL) {
		snd_printk ("can't create WaveFront synth device\n");
		snd_card_free(card);
		return -ENOMEM;
	}

	strcpy (wavefront_synth->name, "ICS2115 Wavetable MIDI Synthesizer");
	wavefront_synth->iface = SNDRV_HWDEP_IFACE_ICS2115;
	hw_dev++;

	/* --------- Mixer ------------ */

	if ((err = snd_cs4231_mixer(chip)) < 0) {
		snd_printk ("can't allocate mixer device\n");
		snd_card_free(card);
		return err;
	}

	/* -------- CS4232 MPU-401 interface -------- */

	if (cs4232_mpu_port[dev] > 0 && cs4232_mpu_port[dev] != SNDRV_AUTO_PORT) {
		if ((err = snd_mpu401_uart_new(card, midi_dev, MPU401_HW_CS4232,
					       cs4232_mpu_port[dev], 0,
					       cs4232_mpu_irq[dev],
					       SA_INTERRUPT,
					       NULL)) < 0) {
			snd_printk ("can't allocate CS4232 MPU-401 device\n");
			snd_card_free(card);
			return err;
		}
		midi_dev++;
	}

	/* ------ ICS2115 internal MIDI ------------ */

	if (ics2115_port[dev] > 0 && ics2115_port[dev] != SNDRV_AUTO_PORT) {
		ics2115_internal_rmidi = 
			snd_wavefront_new_midi (card, 
						midi_dev,
						acard,
						ics2115_port[dev],
						internal_mpu);
		if (ics2115_internal_rmidi == NULL) {
			snd_printk ("can't setup ICS2115 internal MIDI device\n");
			snd_card_free(card);
			return -ENOMEM;
		}
		midi_dev++;
	}

	/* ------ ICS2115 external MIDI ------------ */

	if (ics2115_port[dev] > 0 && ics2115_port[dev] != SNDRV_AUTO_PORT) {
		ics2115_external_rmidi = 
			snd_wavefront_new_midi (card, 
						midi_dev,
						acard,
						ics2115_port[dev],
						external_mpu);
		if (ics2115_external_rmidi == NULL) {
			snd_printk ("can't setup ICS2115 external MIDI device\n");
			snd_card_free(card);
			return -ENOMEM;
		}
		midi_dev++;
	}

	/* FX processor for Tropez+ */

	if (acard->wavefront.has_fx) {
		fx_processor = snd_wavefront_new_fx (card,
						     hw_dev,
						     acard,
						     ics2115_port[dev]);
		if (fx_processor == NULL) {
			snd_printk ("can't setup FX device\n");
			snd_card_free(card);
			return -ENOMEM;
		}

		hw_dev++;

		strcpy(card->driver, "Tropez+");
		strcpy(card->shortname, "Turtle Beach Tropez+");
	} else {
		/* Need a way to distinguish between Maui and Tropez */
		strcpy(card->driver, "WaveFront");
		strcpy(card->shortname, "Turtle Beach WaveFront");
	}

	/* ----- Register the card --------- */

	/* Not safe to include "Turtle Beach" in longname, due to 
	   length restrictions
	*/

	sprintf(card->longname, "%s PCM 0x%lx irq %d dma %d",
		card->driver,
		chip->port,
		cs4232_pcm_irq[dev],
		dma1[dev]);

	if (dma2[dev] >= 0 && dma2[dev] < 8)
		sprintf(card->longname + strlen(card->longname), "&%d", dma2[dev]);

	if (cs4232_mpu_port[dev] > 0 && cs4232_mpu_port[dev] != SNDRV_AUTO_PORT) {
		sprintf (card->longname + strlen (card->longname), 
			 " MPU-401 0x%lx irq %d",
			 cs4232_mpu_port[dev],
			 cs4232_mpu_irq[dev]);
	}

	sprintf (card->longname + strlen (card->longname), 
		 " SYNTH 0x%lx irq %d",
		 ics2115_port[dev],
		 ics2115_irq[dev]);

	if ((err = snd_card_register(card)) < 0) {
		snd_card_free(card);
		return err;
	}
	if (pcard)
		pnp_set_card_drvdata(pcard, card);
	else
		snd_wavefront_legacy[dev] = card;
	return 0;
}	

#ifdef CONFIG_PNP

static int __devinit snd_wavefront_pnp_detect(struct pnp_card_link *card,
                                              const struct pnp_card_device_id *id)
{
        static int dev;
        int res;

        for ( ; dev < SNDRV_CARDS; dev++) {
                if (!enable[dev] || !isapnp[dev])
                        continue;
                res = snd_wavefront_probe(dev, card, id);
                if (res < 0)
                        return res;
                dev++;
                return 0;
        }

        return -ENODEV;
}

static void __devexit snd_wavefront_pnp_remove(struct pnp_card_link * pcard)
{
	snd_card_t *card = (snd_card_t *) pnp_get_card_drvdata(pcard);

	snd_card_disconnect(card);
	snd_card_free_in_thread(card);
}

static struct pnp_card_driver wavefront_pnpc_driver = {
	.flags		= PNP_DRIVER_RES_DISABLE,
	.name		= "wavefront",
	.id_table	= snd_wavefront_pnpids,
	.probe		= snd_wavefront_pnp_detect,
	.remove		= __devexit_p(snd_wavefront_pnp_remove),
};

#endif /* CONFIG_PNP */

static int __init alsa_card_wavefront_init(void)
{
	int cards = 0;
	int dev;
	for (dev = 0; dev < SNDRV_CARDS; dev++) {
		if (!enable[dev])
			continue;
#ifdef CONFIG_PNP
		if (isapnp[dev])
			continue;
#endif
		if (snd_wavefront_probe(dev, NULL, NULL) >= 0)
			cards++;
	}
#ifdef CONFIG_PNP
	cards += pnp_register_card_driver(&wavefront_pnpc_driver);
#endif
	if (!cards) {
#ifdef CONFIG_PNP
		pnp_unregister_card_driver(&wavefront_pnpc_driver);
#endif
#ifdef MODULE
		printk (KERN_ERR "No WaveFront cards found or devices busy\n");
#endif
		return -ENODEV;
	}
	return 0;
}

static void __exit alsa_card_wavefront_exit(void)
{
	int idx;

#ifdef CONFIG_PNP
	pnp_unregister_card_driver(&wavefront_pnpc_driver);
#endif
	for (idx = 0; idx < SNDRV_CARDS; idx++)
		snd_card_free(snd_wavefront_legacy[idx]);
}

module_init(alsa_card_wavefront_init)
module_exit(alsa_card_wavefront_exit)
