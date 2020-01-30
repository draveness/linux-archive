/*
 *  Midi Sequencer interface routines.
 *
 *  Copyright (C) 1999 Steve Ratcliffe
 *  Copyright (c) 1999-2000 Takashi Iwai <tiwai@suse.de>
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
 */

#include "emux_voice.h"
#include <linux/slab.h>


/* Prototypes for static functions */
static void free_port(void *private);
static void snd_emux_init_port(snd_emux_port_t *p);
static int snd_emux_use(void *private_data, snd_seq_port_subscribe_t *info);
static int snd_emux_unuse(void *private_data, snd_seq_port_subscribe_t *info);
static int get_client(snd_card_t *card, int index, char *name);

/*
 * MIDI emulation operators
 */
static snd_midi_op_t emux_ops = {
	snd_emux_note_on,
	snd_emux_note_off,
	snd_emux_key_press,
	snd_emux_terminate_note,
	snd_emux_control,
	snd_emux_nrpn,
	snd_emux_sysex,
};


/*
 * number of MIDI channels
 */
#define MIDI_CHANNELS		16

/*
 * type flags for MIDI sequencer port
 */
#define DEFAULT_MIDI_TYPE	(SNDRV_SEQ_PORT_TYPE_MIDI_GENERIC |\
				 SNDRV_SEQ_PORT_TYPE_MIDI_GM |\
				 SNDRV_SEQ_PORT_TYPE_MIDI_GS |\
				 SNDRV_SEQ_PORT_TYPE_MIDI_XG |\
				 SNDRV_SEQ_PORT_TYPE_DIRECT_SAMPLE)

/*
 * Initialise the EMUX Synth by creating a client and registering
 * a series of ports.
 * Each of the ports will contain the 16 midi channels.  Applications
 * can connect to these ports to play midi data.
 */
int
snd_emux_init_seq(snd_emux_t *emu, snd_card_t *card, int index)
{
	int  i;
	snd_seq_port_callback_t pinfo;
	char tmpname[64];

	sprintf(tmpname, "%s WaveTable", emu->name);
	emu->client = get_client(card, index, tmpname);
	if (emu->client < 0) {
		snd_printk("can't create client\n");
		return -ENODEV;
	}

	if (emu->num_ports < 0) {
		snd_printk("seqports must be greater than zero\n");
		emu->num_ports = 1;
	} else if (emu->num_ports >= SNDRV_EMUX_MAX_PORTS) {
		snd_printk("too many ports."
			   "limited max. ports %d\n", SNDRV_EMUX_MAX_PORTS);
		emu->num_ports = SNDRV_EMUX_MAX_PORTS;
	}

	memset(&pinfo, 0, sizeof(pinfo));
	pinfo.owner = THIS_MODULE;
	pinfo.use = snd_emux_use;
	pinfo.unuse = snd_emux_unuse;
	pinfo.event_input = snd_emux_event_input;

	for (i = 0; i < emu->num_ports; i++) {
		snd_emux_port_t *p;

		sprintf(tmpname, "%s Port %d", emu->name, i);
		p = snd_emux_create_port(emu, tmpname, MIDI_CHANNELS,
					 0, &pinfo);
		if (p == NULL) {
			snd_printk("can't create port\n");
			return -ENOMEM;
		}

		p->port_mode =  SNDRV_EMUX_PORT_MODE_MIDI;
		snd_emux_init_port(p);
		emu->ports[i] = p->chset.port;
		emu->portptrs[i] = p;
	}

	return 0;
}


/*
 * Detach from the ports that were set up for this synthesizer and
 * destroy the kernel client.
 */
void
snd_emux_detach_seq(snd_emux_t *emu)
{
	if (emu->voices)
		snd_emux_terminate_all(emu);
		
	down(&emu->register_mutex);
	if (emu->client >= 0) {
		snd_seq_delete_kernel_client(emu->client);
		emu->client = -1;
	}
	up(&emu->register_mutex);
}


/*
 * create a sequencer port and channel_set
 */

snd_emux_port_t *
snd_emux_create_port(snd_emux_t *emu, char *name,
			int max_channels, int oss_port,
			snd_seq_port_callback_t *callback)
{
	snd_emux_port_t *p;
	int i, type, cap;

	/* Allocate structures for this channel */
	if ((p = snd_magic_kcalloc(snd_emux_port_t, 0, GFP_KERNEL)) == NULL) {
		snd_printk("no memory\n");
		return NULL;
	}
	p->chset.channels = snd_kcalloc(max_channels * sizeof(snd_midi_channel_t), GFP_KERNEL);
	if (p->chset.channels == NULL) {
		snd_printk("no memory\n");
		snd_magic_kfree(p);
		return NULL;
	}
	for (i = 0; i < max_channels; i++)
		p->chset.channels[i].number = i;
	p->chset.private_data = p;
	p->chset.max_channels = max_channels;
	p->emu = emu;
	p->chset.client = emu->client;
#ifdef SNDRV_EMUX_USE_RAW_EFFECT
	snd_emux_create_effect(p);
#endif
	callback->private_free = free_port;
	callback->private_data = p;

	cap = SNDRV_SEQ_PORT_CAP_WRITE;
	if (oss_port) {
		type = SNDRV_SEQ_PORT_TYPE_SPECIFIC;
	} else {
		type = DEFAULT_MIDI_TYPE;
		cap |= SNDRV_SEQ_PORT_CAP_SUBS_WRITE;
	}

	p->chset.port = snd_seq_event_port_attach(emu->client, callback,
						  cap, type, max_channels,
						  emu->max_voices, name);

	return p;
}


/*
 * release memory block for port
 */
static void
free_port(void *private_data)
{
	snd_emux_port_t *p;

	p = snd_magic_cast(snd_emux_port_t, private_data, return);
	if (p) {
#ifdef SNDRV_EMUX_USE_RAW_EFFECT
		snd_emux_delete_effect(p);
#endif
		if (p->chset.channels)
			kfree(p->chset.channels);
		snd_magic_kfree(p);
	}
}


#define DEFAULT_DRUM_FLAGS	(1<<9)

/*
 * initialize the port specific parameters
 */
static void
snd_emux_init_port(snd_emux_port_t *p)
{
	p->drum_flags = DEFAULT_DRUM_FLAGS;
	p->volume_atten = 0;

	snd_emux_reset_port(p);
}


/*
 * reset port
 */
void
snd_emux_reset_port(snd_emux_port_t *port)
{
	int i;

	/* stop all sounds */
	snd_emux_sounds_off_all(port);

	snd_midi_channel_set_clear(&port->chset);

#ifdef SNDRV_EMUX_USE_RAW_EFFECT
	snd_emux_clear_effect(port);
#endif

	/* set port specific control parameters */
	port->ctrls[EMUX_MD_DEF_BANK] = 0;
	port->ctrls[EMUX_MD_DEF_DRUM] = 0;
	port->ctrls[EMUX_MD_REALTIME_PAN] = 1;

	for (i = 0; i < port->chset.max_channels; i++) {
		snd_midi_channel_t *chan = port->chset.channels + i;
		chan->drum_channel = ((port->drum_flags >> i) & 1) ? 1 : 0;
	}
}


/*
 * input sequencer event
 */
int
snd_emux_event_input(snd_seq_event_t *ev, int direct, void *private_data,
		     int atomic, int hop)
{
	snd_emux_port_t *port;

	port = snd_magic_cast(snd_emux_port_t, private_data, return -EINVAL);
	snd_assert(port != NULL && ev != NULL, return -EINVAL);

	snd_midi_process_event(&emux_ops, ev, &port->chset);

	return 0;
}


/*
 * increment usage count
 */
int
snd_emux_inc_count(snd_emux_t *emu)
{
	emu->used++;
	if (!try_module_get(emu->ops.owner))
		goto __error;
	if (!try_module_get(emu->card->module)) {
		module_put(emu->ops.owner);
	      __error:
		emu->used--;
		return 0;
	}
	return 1;
}


/*
 * decrease usage count
 */
void
snd_emux_dec_count(snd_emux_t *emu)
{
	module_put(emu->card->module);
	emu->used--;
	if (emu->used <= 0)
		snd_emux_terminate_all(emu);
	module_put(emu->ops.owner);
}


/*
 * Routine that is called upon a first use of a particular port
 */
static int
snd_emux_use(void *private_data, snd_seq_port_subscribe_t *info)
{
	snd_emux_port_t *p;
	snd_emux_t *emu;

	p = snd_magic_cast(snd_emux_port_t, private_data, return -EINVAL);
	snd_assert(p != NULL, return -EINVAL);
	emu = p->emu;
	snd_assert(emu != NULL, return -EINVAL);

	down(&emu->register_mutex);
	snd_emux_init_port(p);
	snd_emux_inc_count(emu);
	up(&emu->register_mutex);
	return 0;
}

/*
 * Routine that is called upon the last unuse() of a particular port.
 */
static int
snd_emux_unuse(void *private_data, snd_seq_port_subscribe_t *info)
{
	snd_emux_port_t *p;
	snd_emux_t *emu;

	p = snd_magic_cast(snd_emux_port_t, private_data, return -EINVAL);
	snd_assert(p != NULL, return -EINVAL);
	emu = p->emu;
	snd_assert(emu != NULL, return -EINVAL);

	down(&emu->register_mutex);
	snd_emux_sounds_off_all(p);
	snd_emux_dec_count(emu);
	up(&emu->register_mutex);
	return 0;
}


/*
 * Create a sequencer client
 */
static int
get_client(snd_card_t *card, int index, char *name)
{
	snd_seq_client_callback_t callbacks;
	snd_seq_client_info_t cinfo;
	int client;

	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.private_data = NULL;
	callbacks.allow_input = 1;
	callbacks.allow_output = 1;

	/* Find a free client, start from 1 as the MPU expects to use 0 */
	client = snd_seq_create_kernel_client(card, index, &callbacks);
	if (client < 0)
		return client;

	memset(&cinfo, 0, sizeof(cinfo));
	cinfo.client = client;
	cinfo.type = KERNEL_CLIENT;
	strcpy(cinfo.name, name);
	snd_seq_kernel_client_ctl(client, SNDRV_SEQ_IOCTL_SET_CLIENT_INFO, &cinfo);

	return client;
}


/*
 * attach virtual rawmidi devices
 */
int snd_emux_init_virmidi(snd_emux_t *emu, snd_card_t *card)
{
	int i;

	emu->vmidi = NULL;
	if (emu->midi_ports <= 0)
		return 0;

	emu->vmidi = snd_kcalloc(sizeof(snd_rawmidi_t*) * emu->midi_ports, GFP_KERNEL);
	if (emu->vmidi == NULL)
		return -ENOMEM;

	for (i = 0; i < emu->midi_ports; i++) {
		snd_rawmidi_t *rmidi;
		snd_virmidi_dev_t *rdev;
		if (snd_virmidi_new(card, emu->midi_devidx + i, &rmidi) < 0)
			goto __error;
		rdev = snd_magic_cast(snd_virmidi_dev_t, rmidi->private_data, continue);
		sprintf(rmidi->name, "%s Synth MIDI", emu->name);
		rdev->seq_mode = SNDRV_VIRMIDI_SEQ_ATTACH;
		rdev->client = emu->client;
		rdev->port = emu->ports[i];
		if (snd_device_register(card, rmidi) < 0) {
			snd_device_free(card, rmidi);
			goto __error;
		}
		emu->vmidi[i] = rmidi;
		//snd_printk("virmidi %d ok\n", i);
	}
	return 0;

__error:
	//snd_printk("error init..\n");
	snd_emux_delete_virmidi(emu);
	return -ENOMEM;
}

int snd_emux_delete_virmidi(snd_emux_t *emu)
{
	int i;

	if (emu->vmidi == NULL)
		return 0;

	for (i = 0; i < emu->midi_ports; i++) {
		if (emu->vmidi[i])
			snd_device_free(emu->card, emu->vmidi[i]);
	}
	kfree(emu->vmidi);
	emu->vmidi = NULL;
	return 0;
}
