
/*
    card-als100.c - driver for Avance Logic ALS100 based soundcards.
    Copyright (C) 1999-2000 by Massimo Piccioni <dafastidio@libero.it>

    Thanks to Pierfrancesco 'qM2' Passerini.

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
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
*/

#include <sound/driver.h>
#include <linux/init.h>
#include <linux/wait.h>
#include <linux/time.h>
#include <linux/pnp.h>
#include <linux/moduleparam.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/mpu401.h>
#include <sound/opl3.h>
#include <sound/sb.h>

#define chip_t sb_t

#define PFX "als100: "

MODULE_AUTHOR("Massimo Piccioni <dafastidio@libero.it>");
MODULE_DESCRIPTION("Avance Logic ALS1X0");
MODULE_LICENSE("GPL");
MODULE_CLASSES("{sound}");
MODULE_DEVICES("{{Avance Logic,ALS100 - PRO16PNP},"
	        "{Avance Logic,ALS110},"
	        "{Avance Logic,ALS120},"
	        "{Avance Logic,ALS200},"
	        "{3D Melody,MF1000},"
	        "{Digimate,3D Sound},"
	        "{Avance Logic,ALS120},"
	        "{RTL,RTL3000}}");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;	/* Index 0-MAX */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;	/* ID for this card */
static int enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_ISAPNP; /* Enable this card */
static long port[SNDRV_CARDS] = SNDRV_DEFAULT_PORT;	/* PnP setup */
static long mpu_port[SNDRV_CARDS] = SNDRV_DEFAULT_PORT;	/* PnP setup */
static long fm_port[SNDRV_CARDS] = SNDRV_DEFAULT_PORT;	/* PnP setup */
static int irq[SNDRV_CARDS] = SNDRV_DEFAULT_IRQ;	/* PnP setup */
static int mpu_irq[SNDRV_CARDS] = SNDRV_DEFAULT_IRQ;	/* PnP setup */
static int dma8[SNDRV_CARDS] = SNDRV_DEFAULT_DMA;	/* PnP setup */
static int dma16[SNDRV_CARDS] = SNDRV_DEFAULT_DMA;	/* PnP setup */
static int boot_devs;

module_param_array(index, int, boot_devs, 0444);
MODULE_PARM_DESC(index, "Index value for als100 based soundcard.");
MODULE_PARM_SYNTAX(index, SNDRV_INDEX_DESC);
module_param_array(id, charp, boot_devs, 0444);
MODULE_PARM_DESC(id, "ID string for als100 based soundcard.");
MODULE_PARM_SYNTAX(id, SNDRV_ID_DESC);
module_param_array(enable, bool, boot_devs, 0444);
MODULE_PARM_DESC(enable, "Enable als100 based soundcard.");
MODULE_PARM_SYNTAX(enable, SNDRV_ENABLE_DESC);
module_param_array(port, long, boot_devs, 0444);
MODULE_PARM_DESC(port, "Port # for als100 driver.");
MODULE_PARM_SYNTAX(port, SNDRV_PORT12_DESC);
module_param_array(mpu_port, long, boot_devs, 0444);
MODULE_PARM_DESC(mpu_port, "MPU-401 port # for als100 driver.");
MODULE_PARM_SYNTAX(mpu_port, SNDRV_PORT12_DESC);
module_param_array(fm_port, long, boot_devs, 0444);
MODULE_PARM_DESC(fm_port, "FM port # for als100 driver.");
MODULE_PARM_SYNTAX(fm_port, SNDRV_PORT12_DESC);
module_param_array(irq, int, boot_devs, 0444);
MODULE_PARM_DESC(irq, "IRQ # for als100 driver.");
MODULE_PARM_SYNTAX(irq, SNDRV_IRQ_DESC);
module_param_array(mpu_irq, int, boot_devs, 0444);
MODULE_PARM_DESC(mpu_irq, "MPU-401 IRQ # for als100 driver.");
MODULE_PARM_SYNTAX(mpu_irq, SNDRV_IRQ_DESC);
module_param_array(dma8, int, boot_devs, 0444);
MODULE_PARM_DESC(dma8, "8-bit DMA # for als100 driver.");
MODULE_PARM_SYNTAX(dma8, SNDRV_DMA8_DESC);
module_param_array(dma16, int, boot_devs, 0444);
MODULE_PARM_DESC(dma16, "16-bit DMA # for als100 driver.");
MODULE_PARM_SYNTAX(dma16, SNDRV_DMA16_DESC);

struct snd_card_als100 {
	int dev_no;
	struct pnp_dev *dev;
	struct pnp_dev *devmpu;
	struct pnp_dev *devopl;
};

static struct pnp_card_device_id snd_als100_pnpids[] = {
	/* ALS100 - PRO16PNP */
	{ .id = "ALS0001", .devs = { { "@@@0001" }, { "@X@0001" }, { "@H@0001" } } },
	/* ALS110 - MF1000 - Digimate 3D Sound */
	{ .id = "ALS0110", .devs = { { "@@@1001" }, { "@X@1001" }, { "@H@1001" } } },
	/* ALS120 */
	{ .id = "ALS0120", .devs = { { "@@@2001" }, { "@X@2001" }, { "@H@2001" } } },
	/* ALS200 */
	{ .id = "ALS0200", .devs = { { "@@@0020" }, { "@X@0020" }, { "@H@0001" } } },
	/* RTL3000 */
	{ .id = "RTL3000", .devs = { { "@@@2001" }, { "@X@2001" }, { "@H@2001" } } },
	{ .id = "", } /* end */
};

MODULE_DEVICE_TABLE(pnp_card, snd_als100_pnpids);

#define DRIVER_NAME	"snd-card-als100"

static int __devinit snd_card_als100_pnp(int dev, struct snd_card_als100 *acard,
					 struct pnp_card_link *card,
					 const struct pnp_card_device_id *id)
{
	struct pnp_dev *pdev;
	struct pnp_resource_table *cfg = kmalloc(sizeof(*cfg), GFP_KERNEL);
	int err;

	if (!cfg)
		return -ENOMEM;
	acard->dev = pnp_request_card_device(card, id->devs[0].id, NULL);
	if (acard->dev == NULL) {
		kfree(cfg);
		return -ENODEV;
	}
	acard->devmpu = pnp_request_card_device(card, id->devs[1].id, acard->dev);
	acard->devopl = pnp_request_card_device(card, id->devs[2].id, acard->devmpu);

	pdev = acard->dev;

	pnp_init_resource_table(cfg);

	/* override resources */
	if (port[dev] != SNDRV_AUTO_PORT)
		pnp_resource_change(&cfg->port_resource[0], port[dev], 16);
	if (dma8[dev] != SNDRV_AUTO_DMA)
		pnp_resource_change(&cfg->dma_resource[0], dma8[dev], 1);
	if (dma16[dev] != SNDRV_AUTO_DMA)
		pnp_resource_change(&cfg->dma_resource[1], dma16[dev], 1);
	if (irq[dev] != SNDRV_AUTO_IRQ)
		pnp_resource_change(&cfg->irq_resource[0], irq[dev], 1);
	if (pnp_manual_config_dev(pdev, cfg, 0) < 0)
		snd_printk(KERN_ERR PFX "AUDIO the requested resources are invalid, using auto config\n");
	err = pnp_activate_dev(pdev);
	if (err < 0) {
		snd_printk(KERN_ERR PFX "AUDIO pnp configure failure\n");
		kfree(cfg);
		return err;
	}
	port[dev] = pnp_port_start(pdev, 0);
	dma8[dev] = pnp_dma(pdev, 1);
	dma16[dev] = pnp_dma(pdev, 0);
	irq[dev] = pnp_irq(pdev, 0);

	pdev = acard->devmpu;
	if (pdev != NULL) {
		pnp_init_resource_table(cfg);
		if (mpu_port[dev] != SNDRV_AUTO_PORT)
			pnp_resource_change(&cfg->port_resource[0], mpu_port[dev], 2);
		if (mpu_irq[dev] != SNDRV_AUTO_IRQ)
			pnp_resource_change(&cfg->irq_resource[0], mpu_irq[dev], 1);
		if ((pnp_manual_config_dev(pdev, cfg, 0)) < 0)
			snd_printk(KERN_ERR PFX "MPU401 the requested resources are invalid, using auto config\n");
		err = pnp_activate_dev(pdev);
		if (err < 0)
			goto __mpu_error;
		mpu_port[dev] = pnp_port_start(pdev, 0);
		mpu_irq[dev] = pnp_irq(pdev, 0);
	} else {
	     __mpu_error:
	     	if (pdev) {
		     	pnp_release_card_device(pdev);
	     		snd_printk(KERN_ERR PFX "MPU401 pnp configure failure, skipping\n");
	     	}
	     	acard->devmpu = NULL;
	     	mpu_port[dev] = -1;
	}

	pdev = acard->devopl;
	if (pdev != NULL) {
		pnp_init_resource_table(cfg);
		if (fm_port[dev] != SNDRV_AUTO_PORT)
			pnp_resource_change(&cfg->port_resource[0], fm_port[dev], 4);
		if ((pnp_manual_config_dev(pdev, cfg, 0)) < 0)
			snd_printk(KERN_ERR PFX "OPL3 the requested resources are invalid, using auto config\n");
		err = pnp_activate_dev(pdev);
		if (err < 0)
			goto __fm_error;
		fm_port[dev] = pnp_port_start(pdev, 0);
	} else {
	      __fm_error:
	     	if (pdev) {
		     	pnp_release_card_device(pdev);
	     		snd_printk(KERN_ERR PFX "OPL3 pnp configure failure, skipping\n");
	     	}
	     	acard->devopl = NULL;
	     	fm_port[dev] = -1;
	}

	kfree(cfg);
	return 0;
}

static int __init snd_card_als100_probe(int dev,
					struct pnp_card_link *pcard,
					const struct pnp_card_device_id *pid)
{
	int error;
	sb_t *chip;
	snd_card_t *card;
	struct snd_card_als100 *acard;
	opl3_t *opl3;

	if ((card = snd_card_new(index[dev], id[dev], THIS_MODULE,
				 sizeof(struct snd_card_als100))) == NULL)
		return -ENOMEM;
	acard = (struct snd_card_als100 *)card->private_data;

	if ((error = snd_card_als100_pnp(dev, acard, pcard, pid))) {
		snd_card_free(card);
		return error;
	}
	snd_card_set_dev(card, &pcard->card->dev);

	if ((error = snd_sbdsp_create(card, port[dev],
				      irq[dev],
				      snd_sb16dsp_interrupt,
				      dma8[dev],
				      dma16[dev],
				      SB_HW_ALS100, &chip)) < 0) {
		snd_card_free(card);
		return error;
	}

	strcpy(card->driver, "ALS100");
	strcpy(card->shortname, "Avance Logic ALS100");
	sprintf(card->longname, "%s, %s at 0x%lx, irq %d, dma %d&%d",
		card->shortname, chip->name, chip->port,
		irq[dev], dma8[dev], dma16[dev]);

	if ((error = snd_sb16dsp_pcm(chip, 0, NULL)) < 0) {
		snd_card_free(card);
		return error;
	}

	if ((error = snd_sbmixer_new(chip)) < 0) {
		snd_card_free(card);
		return error;
	}

	if (mpu_port[dev] > 0 && mpu_port[dev] != SNDRV_AUTO_PORT) {
		if (snd_mpu401_uart_new(card, 0, MPU401_HW_ALS100,
					mpu_port[dev], 0, 
					mpu_irq[dev], SA_INTERRUPT,
					NULL) < 0)
			snd_printk(KERN_ERR PFX "no MPU-401 device at 0x%lx\n", mpu_port[dev]);
	}

	if (fm_port[dev] > 0 && fm_port[dev] != SNDRV_AUTO_PORT) {
		if (snd_opl3_create(card,
				    fm_port[dev], fm_port[dev] + 2,
				    OPL3_HW_AUTO, 0, &opl3) < 0) {
			snd_printk(KERN_ERR PFX "no OPL device at 0x%lx-0x%lx\n",
				   fm_port[dev], fm_port[dev] + 2);
		} else {
			if ((error = snd_opl3_timer_new(opl3, 0, 1)) < 0) {
				snd_card_free(card);
				return error;
			}
			if ((error = snd_opl3_hwdep_new(opl3, 0, 1, NULL)) < 0) {
				snd_card_free(card);
				return error;
			}
		}
	}

	if ((error = snd_card_register(card)) < 0) {
		snd_card_free(card);
		return error;
	}
	pnp_set_card_drvdata(pcard, card);
	return 0;
}

static int __devinit snd_als100_pnp_detect(struct pnp_card_link *card,
					   const struct pnp_card_device_id *id)
{
	static int dev;
	int res;

	for ( ; dev < SNDRV_CARDS; dev++) {
		if (!enable[dev])
			continue;
		res = snd_card_als100_probe(dev, card, id);
		if (res < 0)
			return res;
		dev++;
		return 0;
	}
	return -ENODEV;
}

static void __devexit snd_als100_pnp_remove(struct pnp_card_link * pcard)
{
	snd_card_t *card = (snd_card_t *) pnp_get_card_drvdata(pcard);

	snd_card_disconnect(card);
	snd_card_free_in_thread(card);
}

static struct pnp_card_driver als100_pnpc_driver = {
	.flags          = PNP_DRIVER_RES_DISABLE,
        .name           = "als100",
        .id_table       = snd_als100_pnpids,
        .probe          = snd_als100_pnp_detect,
        .remove         = __devexit_p(snd_als100_pnp_remove),
};

static int __init alsa_card_als100_init(void)
{
	int cards = 0;

	cards += pnp_register_card_driver(&als100_pnpc_driver);
#ifdef MODULE
	if (!cards) {
		pnp_unregister_card_driver(&als100_pnpc_driver);
		snd_printk(KERN_ERR "no ALS100 based soundcards found\n");
	}
#endif
	return cards ? 0 : -ENODEV;
}

static void __exit alsa_card_als100_exit(void)
{
	pnp_unregister_card_driver(&als100_pnpc_driver);
}

module_init(alsa_card_als100_init)
module_exit(alsa_card_als100_exit)
