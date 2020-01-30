/*
 *  Virtual Raw MIDI client on Sequencer
 *
 *  Copyright (c) 2000 by Takashi Iwai <tiwai@suse.de>,
 *                        Jaroslav Kysela <perex@perex.cz>
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

/*
 * Virtual Raw MIDI client
 *
 * The virtual rawmidi client is a sequencer client which associate
 * a rawmidi device file.  The created rawmidi device file can be
 * accessed as a normal raw midi, but its MIDI source and destination
 * are arbitrary.  For example, a user-client software synth connected
 * to this port can be used as a normal midi device as well.
 *
 * The virtual rawmidi device accepts also multiple opens.  Each file
 * has its own input buffer, so that no conflict would occur.  The drain
 * of input/output buffer acts only to the local buffer.
 *
 */

#include <sound/driver.h>
#include <linux/init.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/rawmidi.h>
#include <sound/info.h>
#include <sound/control.h>
#include <sound/minors.h>
#include <sound/seq_kernel.h>
#include <sound/seq_midi_event.h>
#include <sound/seq_virmidi.h>

MODULE_AUTHOR("Takashi Iwai <tiwai@suse.de>");
MODULE_DESCRIPTION("Virtual Raw MIDI client on Sequencer");
MODULE_LICENSE("GPL");

/*
 * initialize an event record
 */
static void snd_virmidi_init_event(snd_virmidi_t *vmidi, snd_seq_event_t *ev)
{
	memset(ev, 0, sizeof(*ev));
	ev->source.port = vmidi->port;
	switch (vmidi->seq_mode) {
	case SNDRV_VIRMIDI_SEQ_DISPATCH:
		ev->dest.client = SNDRV_SEQ_ADDRESS_SUBSCRIBERS;
		break;
	case SNDRV_VIRMIDI_SEQ_ATTACH:
		/* FIXME: source and destination are same - not good.. */
		ev->dest.client = vmidi->client;
		ev->dest.port = vmidi->port;
		break;
	}
	ev->type = SNDRV_SEQ_EVENT_NONE;
}

/*
 * decode input event and put to read buffer of each opened file
 */
static int snd_virmidi_dev_receive_event(snd_virmidi_dev_t *rdev, snd_seq_event_t *ev)
{
	snd_virmidi_t *vmidi;
	struct list_head *list;
	unsigned char msg[4];
	int len;

	read_lock(&rdev->filelist_lock);
	list_for_each(list, &rdev->filelist) {
		vmidi = list_entry(list, snd_virmidi_t, list);
		if (!vmidi->trigger)
			continue;
		if (ev->type == SNDRV_SEQ_EVENT_SYSEX) {
			if ((ev->flags & SNDRV_SEQ_EVENT_LENGTH_MASK) != SNDRV_SEQ_EVENT_LENGTH_VARIABLE)
				continue;
			snd_seq_dump_var_event(ev, (snd_seq_dump_func_t)snd_rawmidi_receive, vmidi->substream);
		} else {
			len = snd_midi_event_decode(vmidi->parser, msg, sizeof(msg), ev);
			if (len > 0)
				snd_rawmidi_receive(vmidi->substream, msg, len);
		}
	}
	read_unlock(&rdev->filelist_lock);

	return 0;
}

/*
 * receive an event from the remote virmidi port
 *
 * for rawmidi inputs, you can call this function from the event
 * handler of a remote port which is attached to the virmidi via
 * SNDRV_VIRMIDI_SEQ_ATTACH.
 */
/* exported */
int snd_virmidi_receive(snd_rawmidi_t *rmidi, snd_seq_event_t *ev)
{
	snd_virmidi_dev_t *rdev;

	rdev = snd_magic_cast(snd_virmidi_dev_t, rmidi->private_data, return -EINVAL);
	return snd_virmidi_dev_receive_event(rdev, ev);
}

/*
 * event handler of virmidi port
 */
static int snd_virmidi_event_input(snd_seq_event_t *ev, int direct,
				   void *private_data, int atomic, int hop)
{
	snd_virmidi_dev_t *rdev;

	rdev = snd_magic_cast(snd_virmidi_dev_t, private_data, return -EINVAL);
	if (!(rdev->flags & SNDRV_VIRMIDI_USE))
		return 0; /* ignored */
	return snd_virmidi_dev_receive_event(rdev, ev);
}

/*
 * trigger rawmidi stream for input
 */
static void snd_virmidi_input_trigger(snd_rawmidi_substream_t * substream, int up)
{
	snd_virmidi_t *vmidi = snd_magic_cast(snd_virmidi_t, substream->runtime->private_data, return);

	if (up) {
		vmidi->trigger = 1;
	} else {
		vmidi->trigger = 0;
	}
}

/*
 * trigger rawmidi stream for output
 */
static void snd_virmidi_output_trigger(snd_rawmidi_substream_t * substream, int up)
{
	snd_virmidi_t *vmidi = snd_magic_cast(snd_virmidi_t, substream->runtime->private_data, return);
	int count, res;
	unsigned char buf[32], *pbuf;

	if (up) {
		vmidi->trigger = 1;
		if (vmidi->seq_mode == SNDRV_VIRMIDI_SEQ_DISPATCH &&
		    !(vmidi->rdev->flags & SNDRV_VIRMIDI_SUBSCRIBE)) {
			snd_rawmidi_transmit_ack(substream, substream->runtime->buffer_size - substream->runtime->avail);
			return;		/* ignored */
		}
		if (vmidi->event.type != SNDRV_SEQ_EVENT_NONE) {
			if (snd_seq_kernel_client_dispatch(vmidi->client, &vmidi->event, 0, 0) < 0)
				return;
			vmidi->event.type = SNDRV_SEQ_EVENT_NONE;
		}
		while (1) {
			count = snd_rawmidi_transmit_peek(substream, buf, sizeof(buf));
			if (count <= 0)
				break;
			pbuf = buf;
			while (count > 0) {
				res = snd_midi_event_encode(vmidi->parser, pbuf, count, &vmidi->event);
				if (res < 0) {
					snd_midi_event_reset_encode(vmidi->parser);
					continue;
				}
				snd_rawmidi_transmit_ack(substream, res);
				pbuf += res;
				count -= res;
				if (vmidi->event.type != SNDRV_SEQ_EVENT_NONE) {
					if (snd_seq_kernel_client_dispatch(vmidi->client, &vmidi->event, 0, 0) < 0)
						return;
					vmidi->event.type = SNDRV_SEQ_EVENT_NONE;
				}
			}
		}
	} else {
		vmidi->trigger = 0;
	}
}

/*
 * open rawmidi handle for input
 */
static int snd_virmidi_input_open(snd_rawmidi_substream_t * substream)
{
	snd_virmidi_dev_t *rdev = snd_magic_cast(snd_virmidi_dev_t, substream->rmidi->private_data, return -EINVAL);
	snd_rawmidi_runtime_t *runtime = substream->runtime;
	snd_virmidi_t *vmidi;
	unsigned long flags;

	vmidi = snd_magic_kcalloc(snd_virmidi_t, 0, GFP_KERNEL);
	if (vmidi == NULL)
		return -ENOMEM;
	vmidi->substream = substream;
	if (snd_midi_event_new(0, &vmidi->parser) < 0) {
		snd_magic_kfree(vmidi);
		return -ENOMEM;
	}
	vmidi->seq_mode = rdev->seq_mode;
	vmidi->client = rdev->client;
	vmidi->port = rdev->port;	
	runtime->private_data = vmidi;
	write_lock_irqsave(&rdev->filelist_lock, flags);
	list_add_tail(&vmidi->list, &rdev->filelist);
	write_unlock_irqrestore(&rdev->filelist_lock, flags);
	vmidi->rdev = rdev;
	return 0;
}

/*
 * open rawmidi handle for output
 */
static int snd_virmidi_output_open(snd_rawmidi_substream_t * substream)
{
	snd_virmidi_dev_t *rdev = snd_magic_cast(snd_virmidi_dev_t, substream->rmidi->private_data, return -EINVAL);
	snd_rawmidi_runtime_t *runtime = substream->runtime;
	snd_virmidi_t *vmidi;

	vmidi = snd_magic_kcalloc(snd_virmidi_t, 0, GFP_KERNEL);
	if (vmidi == NULL)
		return -ENOMEM;
	vmidi->substream = substream;
	if (snd_midi_event_new(MAX_MIDI_EVENT_BUF, &vmidi->parser) < 0) {
		snd_magic_kfree(vmidi);
		return -ENOMEM;
	}
	vmidi->seq_mode = rdev->seq_mode;
	vmidi->client = rdev->client;
	vmidi->port = rdev->port;
	snd_virmidi_init_event(vmidi, &vmidi->event);
	vmidi->rdev = rdev;
	runtime->private_data = vmidi;
	return 0;
}

/*
 * close rawmidi handle for input
 */
static int snd_virmidi_input_close(snd_rawmidi_substream_t * substream)
{
	snd_virmidi_t *vmidi = snd_magic_cast(snd_virmidi_t, substream->runtime->private_data, return -EINVAL);
	snd_midi_event_free(vmidi->parser);
	list_del(&vmidi->list);
	substream->runtime->private_data = NULL;
	snd_magic_kfree(vmidi);
	return 0;
}

/*
 * close rawmidi handle for output
 */
static int snd_virmidi_output_close(snd_rawmidi_substream_t * substream)
{
	snd_virmidi_t *vmidi = snd_magic_cast(snd_virmidi_t, substream->runtime->private_data, return -EINVAL);
	snd_midi_event_free(vmidi->parser);
	substream->runtime->private_data = NULL;
	snd_magic_kfree(vmidi);
	return 0;
}

/*
 * subscribe callback - allow output to rawmidi device
 */
static int snd_virmidi_subscribe(void *private_data, snd_seq_port_subscribe_t *info)
{
	snd_virmidi_dev_t *rdev;

	rdev = snd_magic_cast(snd_virmidi_dev_t, private_data, return -EINVAL);
	if (!try_module_get(rdev->card->module))
		return -EFAULT;
	rdev->flags |= SNDRV_VIRMIDI_SUBSCRIBE;
	return 0;
}

/*
 * unsubscribe callback - disallow output to rawmidi device
 */
static int snd_virmidi_unsubscribe(void *private_data, snd_seq_port_subscribe_t *info)
{
	snd_virmidi_dev_t *rdev;

	rdev = snd_magic_cast(snd_virmidi_dev_t, private_data, return -EINVAL);
	rdev->flags &= ~SNDRV_VIRMIDI_SUBSCRIBE;
	module_put(rdev->card->module);
	return 0;
}


/*
 * use callback - allow input to rawmidi device
 */
static int snd_virmidi_use(void *private_data, snd_seq_port_subscribe_t *info)
{
	snd_virmidi_dev_t *rdev;

	rdev = snd_magic_cast(snd_virmidi_dev_t, private_data, return -EINVAL);
	if (!try_module_get(rdev->card->module))
		return -EFAULT;
	rdev->flags |= SNDRV_VIRMIDI_USE;
	return 0;
}

/*
 * unuse callback - disallow input to rawmidi device
 */
static int snd_virmidi_unuse(void *private_data, snd_seq_port_subscribe_t *info)
{
	snd_virmidi_dev_t *rdev;

	rdev = snd_magic_cast(snd_virmidi_dev_t, private_data, return -EINVAL);
	rdev->flags &= ~SNDRV_VIRMIDI_USE;
	module_put(rdev->card->module);
	return 0;
}


/*
 *  Register functions
 */

static snd_rawmidi_ops_t snd_virmidi_input_ops = {
	.open = snd_virmidi_input_open,
	.close = snd_virmidi_input_close,
	.trigger = snd_virmidi_input_trigger,
};

static snd_rawmidi_ops_t snd_virmidi_output_ops = {
	.open = snd_virmidi_output_open,
	.close = snd_virmidi_output_close,
	.trigger = snd_virmidi_output_trigger,
};

/*
 * create a sequencer client and a port
 */
static int snd_virmidi_dev_attach_seq(snd_virmidi_dev_t *rdev)
{
	int client;
	snd_seq_client_callback_t callbacks;
	snd_seq_port_callback_t pcallbacks;
	snd_seq_client_info_t info;
	snd_seq_port_info_t pinfo;
	int err;

	if (rdev->client >= 0)
		return 0;

	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.private_data = rdev;
	callbacks.allow_input = 1;
	callbacks.allow_output = 1;
	client = snd_seq_create_kernel_client(rdev->card, rdev->device, &callbacks);
	if (client < 0)
		return client;
	rdev->client = client;

	/* set client name */
	memset(&info, 0, sizeof(info));
	info.client = client;
	info.type = KERNEL_CLIENT;
	sprintf(info.name, "%s %d-%d", rdev->rmidi->name, rdev->card->number, rdev->device);
	snd_seq_kernel_client_ctl(client, SNDRV_SEQ_IOCTL_SET_CLIENT_INFO, &info);

	/* create a port */
	memset(&pinfo, 0, sizeof(pinfo));
	pinfo.addr.client = client;
	sprintf(pinfo.name, "VirMIDI %d-%d", rdev->card->number, rdev->device);
	/* set all capabilities */
	pinfo.capability |= SNDRV_SEQ_PORT_CAP_WRITE | SNDRV_SEQ_PORT_CAP_SYNC_WRITE | SNDRV_SEQ_PORT_CAP_SUBS_WRITE;
	pinfo.capability |= SNDRV_SEQ_PORT_CAP_READ | SNDRV_SEQ_PORT_CAP_SYNC_READ | SNDRV_SEQ_PORT_CAP_SUBS_READ;
	pinfo.capability |= SNDRV_SEQ_PORT_CAP_DUPLEX;
	pinfo.type = SNDRV_SEQ_PORT_TYPE_MIDI_GENERIC;
	pinfo.midi_channels = 16;
	memset(&pcallbacks, 0, sizeof(pcallbacks));
	pcallbacks.owner = THIS_MODULE;
	pcallbacks.private_data = rdev;
	pcallbacks.subscribe = snd_virmidi_subscribe;
	pcallbacks.unsubscribe = snd_virmidi_unsubscribe;
	pcallbacks.use = snd_virmidi_use;
	pcallbacks.unuse = snd_virmidi_unuse;
	pcallbacks.event_input = snd_virmidi_event_input;
	pinfo.kernel = &pcallbacks;
	err = snd_seq_kernel_client_ctl(client, SNDRV_SEQ_IOCTL_CREATE_PORT, &pinfo);
	if (err < 0) {
		snd_seq_delete_kernel_client(client);
		rdev->client = -1;
		return err;
	}

	rdev->port = pinfo.addr.port;
	return 0;	/* success */
}


/*
 * release the sequencer client
 */
static void snd_virmidi_dev_detach_seq(snd_virmidi_dev_t *rdev)
{
	if (rdev->client >= 0) {
		snd_seq_delete_kernel_client(rdev->client);
		rdev->client = -1;
	}
}

/*
 * register the device
 */
static int snd_virmidi_dev_register(snd_rawmidi_t *rmidi)
{
	snd_virmidi_dev_t *rdev = snd_magic_cast(snd_virmidi_dev_t, rmidi->private_data, return -ENXIO);
	int err;

	switch (rdev->seq_mode) {
	case SNDRV_VIRMIDI_SEQ_DISPATCH:
		err = snd_virmidi_dev_attach_seq(rdev);
		if (err < 0)
			return err;
		break;
	case SNDRV_VIRMIDI_SEQ_ATTACH:
		if (rdev->client == 0)
			return -EINVAL;
		/* should check presence of port more strictly.. */
		break;
	default:
		snd_printk(KERN_ERR "seq_mode is not set: %d\n", rdev->seq_mode);
		return -EINVAL;
	}
	return 0;
}


/*
 * unregister the device
 */
static int snd_virmidi_dev_unregister(snd_rawmidi_t *rmidi)
{
	snd_virmidi_dev_t *rdev = snd_magic_cast(snd_virmidi_dev_t, rmidi->private_data, return -ENXIO);

	if (rdev->seq_mode == SNDRV_VIRMIDI_SEQ_DISPATCH)
		snd_virmidi_dev_detach_seq(rdev);
	return 0;
}

/*
 *
 */
static snd_rawmidi_global_ops_t snd_virmidi_global_ops = {
	.dev_register = snd_virmidi_dev_register,
	.dev_unregister = snd_virmidi_dev_unregister,
};

/*
 * free device
 */
static void snd_virmidi_free(snd_rawmidi_t *rmidi)
{
	snd_virmidi_dev_t *rdev = snd_magic_cast(snd_virmidi_dev_t, rmidi->private_data, return);
	snd_magic_kfree(rdev);
}

/*
 * create a new device
 *
 */
/* exported */
int snd_virmidi_new(snd_card_t *card, int device, snd_rawmidi_t **rrmidi)
{
	snd_rawmidi_t *rmidi;
	snd_virmidi_dev_t *rdev;
	int err;
	
	*rrmidi = NULL;
	if ((err = snd_rawmidi_new(card, "VirMidi", device,
				   16,	/* may be configurable */
				   16,	/* may be configurable */
				   &rmidi)) < 0)
		return err;
	strcpy(rmidi->name, rmidi->id);
	rdev = snd_magic_kcalloc(snd_virmidi_dev_t, 0, GFP_KERNEL);
	if (rdev == NULL) {
		snd_device_free(card, rmidi);
		return -ENOMEM;
	}
	rdev->card = card;
	rdev->rmidi = rmidi;
	rdev->device = device;
	rdev->client = -1;
	rwlock_init(&rdev->filelist_lock);
	INIT_LIST_HEAD(&rdev->filelist);
	rdev->seq_mode = SNDRV_VIRMIDI_SEQ_DISPATCH;
	rmidi->private_data = rdev;
	rmidi->private_free = snd_virmidi_free;
	rmidi->ops = &snd_virmidi_global_ops;
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &snd_virmidi_input_ops);
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &snd_virmidi_output_ops);
	rmidi->info_flags = SNDRV_RAWMIDI_INFO_INPUT |
			    SNDRV_RAWMIDI_INFO_OUTPUT |
			    SNDRV_RAWMIDI_INFO_DUPLEX;
	*rrmidi = rmidi;
	return 0;
}

/*
 *  ENTRY functions
 */

static int __init alsa_virmidi_init(void)
{
	return 0;
}

static void __exit alsa_virmidi_exit(void)
{
}

module_init(alsa_virmidi_init)
module_exit(alsa_virmidi_exit)

EXPORT_SYMBOL(snd_virmidi_new);
EXPORT_SYMBOL(snd_virmidi_receive);
