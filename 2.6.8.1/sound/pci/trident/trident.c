/*
 *  Driver for Trident 4DWave DX/NX & SiS SI7018 Audio PCI soundcard
 *
 *  Driver was originated by Trident <audio@tridentmicro.com>
 *  			     Fri Feb 19 15:55:28 MST 1999
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <sound/driver.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/time.h>
#include <linux/moduleparam.h>
#include <sound/core.h>
#include <sound/trident.h>
#include <sound/initval.h>

MODULE_AUTHOR("Jaroslav Kysela <perex@suse.cz>, <audio@tridentmicro.com>");
MODULE_DESCRIPTION("Trident 4D-WaveDX/NX & SiS SI7018");
MODULE_LICENSE("GPL");
MODULE_CLASSES("{sound}");
MODULE_DEVICES("{{Trident,4DWave DX},"
		"{Trident,4DWave NX},"
		"{SiS,SI7018 PCI Audio},"
		"{Best Union,Miss Melody 4DWave PCI},"
		"{HIS,4DWave PCI},"
		"{Warpspeed,ONSpeed 4DWave PCI},"
		"{Aztech Systems,PCI 64-Q3D},"
		"{Addonics,SV 750},"
		"{CHIC,True Sound 4Dwave},"
		"{Shark,Predator4D-PCI},"
		"{Jaton,SonicWave 4D},"
		"{Hoontech,SoundTrack Digital 4DWave NX}}");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;	/* Index 0-MAX */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;	/* ID for this card */
static int enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;	/* Enable this card */
static int pcm_channels[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = 32};
static int wavetable_size[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = 8192};
static int boot_devs;

module_param_array(index, int, boot_devs, 0444);
MODULE_PARM_DESC(index, "Index value for Trident 4DWave PCI soundcard.");
MODULE_PARM_SYNTAX(index, SNDRV_INDEX_DESC);
module_param_array(id, charp, boot_devs, 0444);
MODULE_PARM_DESC(id, "ID string for Trident 4DWave PCI soundcard.");
MODULE_PARM_SYNTAX(id, SNDRV_ID_DESC);
module_param_array(enable, bool, boot_devs, 0444);
MODULE_PARM_DESC(enable, "Enable Trident 4DWave PCI soundcard.");
MODULE_PARM_SYNTAX(enable, SNDRV_ENABLE_DESC);
module_param_array(pcm_channels, int, boot_devs, 0444);
MODULE_PARM_DESC(pcm_channels, "Number of hardware channels assigned for PCM.");
MODULE_PARM_SYNTAX(pcm_channels, SNDRV_ENABLED ",default:32,allows:{{1,32}}");
module_param_array(wavetable_size, int, boot_devs, 0444);
MODULE_PARM_DESC(wavetable_size, "Maximum memory size in kB for wavetable synth.");
MODULE_PARM_SYNTAX(wavetable_size, SNDRV_ENABLED ",default:8192,skill:advanced");

static struct pci_device_id snd_trident_ids[] = {
	{ 0x1023, 0x2000, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0, },	/* Trident 4DWave DX PCI Audio */
	{ 0x1023, 0x2001, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0, },	/* Trident 4DWave NX PCI Audio */
	{ 0x1039, 0x7018, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0, },	/* SiS SI7018 PCI Audio */
	{ 0, }
};

MODULE_DEVICE_TABLE(pci, snd_trident_ids);

static int __devinit snd_trident_probe(struct pci_dev *pci,
				       const struct pci_device_id *pci_id)
{
	static int dev;
	snd_card_t *card;
	trident_t *trident;
	const char *str;
	int err, pcm_dev = 0;

	if (dev >= SNDRV_CARDS)
		return -ENODEV;
	if (!enable[dev]) {
		dev++;
		return -ENOENT;
	}

	card = snd_card_new(index[dev], id[dev], THIS_MODULE, 0);
	if (card == NULL)
		return -ENOMEM;

	if ((err = snd_trident_create(card, pci,
				      pcm_channels[dev],
				      ((pci->vendor << 16) | pci->device) == TRIDENT_DEVICE_ID_SI7018 ? 1 : 2,
				      wavetable_size[dev],
				      &trident)) < 0) {
		snd_card_free(card);
		return err;
	}

	switch (trident->device) {
	case TRIDENT_DEVICE_ID_DX:
		str = "TRID4DWAVEDX";
		break;
	case TRIDENT_DEVICE_ID_NX:
		str = "TRID4DWAVENX";
		break;
	case TRIDENT_DEVICE_ID_SI7018:
		str = "SI7018";
		break;
	default:
		str = "Unknown";
	}
	strcpy(card->driver, str);
	if (trident->device == TRIDENT_DEVICE_ID_SI7018) {
		strcpy(card->shortname, "SiS ");
	} else {
		strcpy(card->shortname, "Trident ");
	}
	strcat(card->shortname, card->driver);
	sprintf(card->longname, "%s PCI Audio at 0x%lx, irq %d",
		card->shortname, trident->port, trident->irq);

	if ((err = snd_trident_pcm(trident, pcm_dev++, NULL)) < 0) {
		snd_card_free(card);
		return err;
	}
	switch (trident->device) {
	case TRIDENT_DEVICE_ID_DX:
	case TRIDENT_DEVICE_ID_NX:
		if ((err = snd_trident_foldback_pcm(trident, pcm_dev++, NULL)) < 0) {
			snd_card_free(card);
			return err;
		}
		break;
	}
	if (trident->device == TRIDENT_DEVICE_ID_NX || trident->device == TRIDENT_DEVICE_ID_SI7018) {
		if ((err = snd_trident_spdif_pcm(trident, pcm_dev++, NULL)) < 0) {
			snd_card_free(card);
			return err;
		}
	}
	if ((err = snd_mpu401_uart_new(card, 0, MPU401_HW_TRID4DWAVE,
				       trident->midi_port, 1,
				       trident->irq, 0, &trident->rmidi)) < 0) {
		snd_card_free(card);
		return err;
	}

#if defined(CONFIG_SND_SEQUENCER) || (defined(MODULE) && defined(CONFIG_SND_SEQUENCER_MODULE))
	if ((err = snd_trident_attach_synthesizer(trident)) < 0) {
		snd_card_free(card);
		return err;
	}
#endif

	snd_trident_gameport(trident);

	if ((err = snd_card_register(card)) < 0) {
		snd_card_free(card);
		return err;
	}
	pci_set_drvdata(pci, card);
	dev++;
	return 0;
}

static void __devexit snd_trident_remove(struct pci_dev *pci)
{
	snd_card_free(pci_get_drvdata(pci));
	pci_set_drvdata(pci, NULL);
}

static struct pci_driver driver = {
	.name = "Trident4DWaveAudio",
	.id_table = snd_trident_ids,
	.probe = snd_trident_probe,
	.remove = __devexit_p(snd_trident_remove),
	SND_PCI_PM_CALLBACKS
};

static int __init alsa_card_trident_init(void)
{
	return pci_module_init(&driver);
}

static void __exit alsa_card_trident_exit(void)
{
	pci_unregister_driver(&driver);
}

module_init(alsa_card_trident_init)
module_exit(alsa_card_trident_exit)
