/*
 * Universal Interface for Intel High Definition Audio Codec
 *
 * Copyright (c) 2004 Takashi Iwai <tiwai@suse.de>
 *
 *
 *  This driver is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This driver is distributed in the hope that it will be useful,
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
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/mutex.h>
#include <sound/core.h>
#include "hda_codec.h"
#include <sound/asoundef.h>
#include <sound/tlv.h>
#include <sound/initval.h>
#include "hda_local.h"


/*
 * vendor / preset table
 */

struct hda_vendor_id {
	unsigned int id;
	const char *name;
};

/* codec vendor labels */
static struct hda_vendor_id hda_vendor_ids[] = {
	{ 0x10ec, "Realtek" },
	{ 0x1057, "Motorola" },
	{ 0x1106, "VIA" },
	{ 0x11d4, "Analog Devices" },
	{ 0x13f6, "C-Media" },
	{ 0x14f1, "Conexant" },
	{ 0x434d, "C-Media" },
	{ 0x8384, "SigmaTel" },
	{} /* terminator */
};

/* codec presets */
#include "hda_patch.h"


/**
 * snd_hda_codec_read - send a command and get the response
 * @codec: the HDA codec
 * @nid: NID to send the command
 * @direct: direct flag
 * @verb: the verb to send
 * @parm: the parameter for the verb
 *
 * Send a single command and read the corresponding response.
 *
 * Returns the obtained response value, or -1 for an error.
 */
unsigned int snd_hda_codec_read(struct hda_codec *codec, hda_nid_t nid,
				int direct,
				unsigned int verb, unsigned int parm)
{
	unsigned int res;
	mutex_lock(&codec->bus->cmd_mutex);
	if (!codec->bus->ops.command(codec, nid, direct, verb, parm))
		res = codec->bus->ops.get_response(codec);
	else
		res = (unsigned int)-1;
	mutex_unlock(&codec->bus->cmd_mutex);
	return res;
}

/**
 * snd_hda_codec_write - send a single command without waiting for response
 * @codec: the HDA codec
 * @nid: NID to send the command
 * @direct: direct flag
 * @verb: the verb to send
 * @parm: the parameter for the verb
 *
 * Send a single command without waiting for response.
 *
 * Returns 0 if successful, or a negative error code.
 */
int snd_hda_codec_write(struct hda_codec *codec, hda_nid_t nid, int direct,
			 unsigned int verb, unsigned int parm)
{
	int err;
	mutex_lock(&codec->bus->cmd_mutex);
	err = codec->bus->ops.command(codec, nid, direct, verb, parm);
	mutex_unlock(&codec->bus->cmd_mutex);
	return err;
}

/**
 * snd_hda_sequence_write - sequence writes
 * @codec: the HDA codec
 * @seq: VERB array to send
 *
 * Send the commands sequentially from the given array.
 * The array must be terminated with NID=0.
 */
void snd_hda_sequence_write(struct hda_codec *codec, const struct hda_verb *seq)
{
	for (; seq->nid; seq++)
		snd_hda_codec_write(codec, seq->nid, 0, seq->verb, seq->param);
}

/**
 * snd_hda_get_sub_nodes - get the range of sub nodes
 * @codec: the HDA codec
 * @nid: NID to parse
 * @start_id: the pointer to store the start NID
 *
 * Parse the NID and store the start NID of its sub-nodes.
 * Returns the number of sub-nodes.
 */
int snd_hda_get_sub_nodes(struct hda_codec *codec, hda_nid_t nid,
			  hda_nid_t *start_id)
{
	unsigned int parm;

	parm = snd_hda_param_read(codec, nid, AC_PAR_NODE_COUNT);
	*start_id = (parm >> 16) & 0x7fff;
	return (int)(parm & 0x7fff);
}

/**
 * snd_hda_get_connections - get connection list
 * @codec: the HDA codec
 * @nid: NID to parse
 * @conn_list: connection list array
 * @max_conns: max. number of connections to store
 *
 * Parses the connection list of the given widget and stores the list
 * of NIDs.
 *
 * Returns the number of connections, or a negative error code.
 */
int snd_hda_get_connections(struct hda_codec *codec, hda_nid_t nid,
			    hda_nid_t *conn_list, int max_conns)
{
	unsigned int parm;
	int i, conn_len, conns;
	unsigned int shift, num_elems, mask;
	hda_nid_t prev_nid;

	snd_assert(conn_list && max_conns > 0, return -EINVAL);

	parm = snd_hda_param_read(codec, nid, AC_PAR_CONNLIST_LEN);
	if (parm & AC_CLIST_LONG) {
		/* long form */
		shift = 16;
		num_elems = 2;
	} else {
		/* short form */
		shift = 8;
		num_elems = 4;
	}
	conn_len = parm & AC_CLIST_LENGTH;
	mask = (1 << (shift-1)) - 1;

	if (!conn_len)
		return 0; /* no connection */

	if (conn_len == 1) {
		/* single connection */
		parm = snd_hda_codec_read(codec, nid, 0,
					  AC_VERB_GET_CONNECT_LIST, 0);
		conn_list[0] = parm & mask;
		return 1;
	}

	/* multi connection */
	conns = 0;
	prev_nid = 0;
	for (i = 0; i < conn_len; i++) {
		int range_val;
		hda_nid_t val, n;

		if (i % num_elems == 0)
			parm = snd_hda_codec_read(codec, nid, 0,
						  AC_VERB_GET_CONNECT_LIST, i);
		range_val = !!(parm & (1 << (shift-1))); /* ranges */
		val = parm & mask;
		parm >>= shift;
		if (range_val) {
			/* ranges between the previous and this one */
			if (!prev_nid || prev_nid >= val) {
				snd_printk(KERN_WARNING "hda_codec: "
					   "invalid dep_range_val %x:%x\n",
					   prev_nid, val);
				continue;
			}
			for (n = prev_nid + 1; n <= val; n++) {
				if (conns >= max_conns) {
					snd_printk(KERN_ERR
						   "Too many connections\n");
					return -EINVAL;
				}
				conn_list[conns++] = n;
			}
		} else {
			if (conns >= max_conns) {
				snd_printk(KERN_ERR "Too many connections\n");
				return -EINVAL;
			}
			conn_list[conns++] = val;
		}
		prev_nid = val;
	}
	return conns;
}


/**
 * snd_hda_queue_unsol_event - add an unsolicited event to queue
 * @bus: the BUS
 * @res: unsolicited event (lower 32bit of RIRB entry)
 * @res_ex: codec addr and flags (upper 32bit or RIRB entry)
 *
 * Adds the given event to the queue.  The events are processed in
 * the workqueue asynchronously.  Call this function in the interrupt
 * hanlder when RIRB receives an unsolicited event.
 *
 * Returns 0 if successful, or a negative error code.
 */
int snd_hda_queue_unsol_event(struct hda_bus *bus, u32 res, u32 res_ex)
{
	struct hda_bus_unsolicited *unsol;
	unsigned int wp;

	unsol = bus->unsol;
	if (!unsol)
		return 0;

	wp = (unsol->wp + 1) % HDA_UNSOL_QUEUE_SIZE;
	unsol->wp = wp;

	wp <<= 1;
	unsol->queue[wp] = res;
	unsol->queue[wp + 1] = res_ex;

	schedule_work(&unsol->work);

	return 0;
}

/*
 * process queueud unsolicited events
 */
static void process_unsol_events(struct work_struct *work)
{
	struct hda_bus_unsolicited *unsol =
		container_of(work, struct hda_bus_unsolicited, work);
	struct hda_bus *bus = unsol->bus;
	struct hda_codec *codec;
	unsigned int rp, caddr, res;

	while (unsol->rp != unsol->wp) {
		rp = (unsol->rp + 1) % HDA_UNSOL_QUEUE_SIZE;
		unsol->rp = rp;
		rp <<= 1;
		res = unsol->queue[rp];
		caddr = unsol->queue[rp + 1];
		if (!(caddr & (1 << 4))) /* no unsolicited event? */
			continue;
		codec = bus->caddr_tbl[caddr & 0x0f];
		if (codec && codec->patch_ops.unsol_event)
			codec->patch_ops.unsol_event(codec, res);
	}
}

/*
 * initialize unsolicited queue
 */
static int __devinit init_unsol_queue(struct hda_bus *bus)
{
	struct hda_bus_unsolicited *unsol;

	if (bus->unsol) /* already initialized */
		return 0;

	unsol = kzalloc(sizeof(*unsol), GFP_KERNEL);
	if (!unsol) {
		snd_printk(KERN_ERR "hda_codec: "
			   "can't allocate unsolicited queue\n");
		return -ENOMEM;
	}
	INIT_WORK(&unsol->work, process_unsol_events);
	unsol->bus = bus;
	bus->unsol = unsol;
	return 0;
}

/*
 * destructor
 */
static void snd_hda_codec_free(struct hda_codec *codec);

static int snd_hda_bus_free(struct hda_bus *bus)
{
	struct hda_codec *codec, *n;

	if (!bus)
		return 0;
	if (bus->unsol) {
		flush_scheduled_work();
		kfree(bus->unsol);
	}
	list_for_each_entry_safe(codec, n, &bus->codec_list, list) {
		snd_hda_codec_free(codec);
	}
	if (bus->ops.private_free)
		bus->ops.private_free(bus);
	kfree(bus);
	return 0;
}

static int snd_hda_bus_dev_free(struct snd_device *device)
{
	struct hda_bus *bus = device->device_data;
	return snd_hda_bus_free(bus);
}

/**
 * snd_hda_bus_new - create a HDA bus
 * @card: the card entry
 * @temp: the template for hda_bus information
 * @busp: the pointer to store the created bus instance
 *
 * Returns 0 if successful, or a negative error code.
 */
int __devinit snd_hda_bus_new(struct snd_card *card,
			      const struct hda_bus_template *temp,
			      struct hda_bus **busp)
{
	struct hda_bus *bus;
	int err;
	static struct snd_device_ops dev_ops = {
		.dev_free = snd_hda_bus_dev_free,
	};

	snd_assert(temp, return -EINVAL);
	snd_assert(temp->ops.command && temp->ops.get_response, return -EINVAL);

	if (busp)
		*busp = NULL;

	bus = kzalloc(sizeof(*bus), GFP_KERNEL);
	if (bus == NULL) {
		snd_printk(KERN_ERR "can't allocate struct hda_bus\n");
		return -ENOMEM;
	}

	bus->card = card;
	bus->private_data = temp->private_data;
	bus->pci = temp->pci;
	bus->modelname = temp->modelname;
	bus->ops = temp->ops;

	mutex_init(&bus->cmd_mutex);
	INIT_LIST_HEAD(&bus->codec_list);

	err = snd_device_new(card, SNDRV_DEV_BUS, bus, &dev_ops);
	if (err < 0) {
		snd_hda_bus_free(bus);
		return err;
	}
	if (busp)
		*busp = bus;
	return 0;
}

/*
 * find a matching codec preset
 */
static const struct hda_codec_preset __devinit *
find_codec_preset(struct hda_codec *codec)
{
	const struct hda_codec_preset **tbl, *preset;

	if (codec->bus->modelname && !strcmp(codec->bus->modelname, "generic"))
		return NULL; /* use the generic parser */

	for (tbl = hda_preset_tables; *tbl; tbl++) {
		for (preset = *tbl; preset->id; preset++) {
			u32 mask = preset->mask;
			if (!mask)
				mask = ~0;
			if (preset->id == (codec->vendor_id & mask) &&
			    (!preset->rev ||
			     preset->rev == codec->revision_id))
				return preset;
		}
	}
	return NULL;
}

/*
 * snd_hda_get_codec_name - store the codec name
 */
void snd_hda_get_codec_name(struct hda_codec *codec,
			    char *name, int namelen)
{
	const struct hda_vendor_id *c;
	const char *vendor = NULL;
	u16 vendor_id = codec->vendor_id >> 16;
	char tmp[16];

	for (c = hda_vendor_ids; c->id; c++) {
		if (c->id == vendor_id) {
			vendor = c->name;
			break;
		}
	}
	if (!vendor) {
		sprintf(tmp, "Generic %04x", vendor_id);
		vendor = tmp;
	}
	if (codec->preset && codec->preset->name)
		snprintf(name, namelen, "%s %s", vendor, codec->preset->name);
	else
		snprintf(name, namelen, "%s ID %x", vendor,
			 codec->vendor_id & 0xffff);
}

/*
 * look for an AFG and MFG nodes
 */
static void __devinit setup_fg_nodes(struct hda_codec *codec)
{
	int i, total_nodes;
	hda_nid_t nid;

	total_nodes = snd_hda_get_sub_nodes(codec, AC_NODE_ROOT, &nid);
	for (i = 0; i < total_nodes; i++, nid++) {
		unsigned int func;
		func = snd_hda_param_read(codec, nid, AC_PAR_FUNCTION_TYPE);
		switch (func & 0xff) {
		case AC_GRP_AUDIO_FUNCTION:
			codec->afg = nid;
			break;
		case AC_GRP_MODEM_FUNCTION:
			codec->mfg = nid;
			break;
		default:
			break;
		}
	}
}

/*
 * read widget caps for each widget and store in cache
 */
static int read_widget_caps(struct hda_codec *codec, hda_nid_t fg_node)
{
	int i;
	hda_nid_t nid;

	codec->num_nodes = snd_hda_get_sub_nodes(codec, fg_node,
						 &codec->start_nid);
	codec->wcaps = kmalloc(codec->num_nodes * 4, GFP_KERNEL);
	if (!codec->wcaps)
		return -ENOMEM;
	nid = codec->start_nid;
	for (i = 0; i < codec->num_nodes; i++, nid++)
		codec->wcaps[i] = snd_hda_param_read(codec, nid,
						     AC_PAR_AUDIO_WIDGET_CAP);
	return 0;
}


/*
 * codec destructor
 */
static void snd_hda_codec_free(struct hda_codec *codec)
{
	if (!codec)
		return;
	list_del(&codec->list);
	codec->bus->caddr_tbl[codec->addr] = NULL;
	if (codec->patch_ops.free)
		codec->patch_ops.free(codec);
	kfree(codec->amp_info);
	kfree(codec->wcaps);
	kfree(codec);
}

static void init_amp_hash(struct hda_codec *codec);

/**
 * snd_hda_codec_new - create a HDA codec
 * @bus: the bus to assign
 * @codec_addr: the codec address
 * @codecp: the pointer to store the generated codec
 *
 * Returns 0 if successful, or a negative error code.
 */
int __devinit snd_hda_codec_new(struct hda_bus *bus, unsigned int codec_addr,
				struct hda_codec **codecp)
{
	struct hda_codec *codec;
	char component[13];
	int err;

	snd_assert(bus, return -EINVAL);
	snd_assert(codec_addr <= HDA_MAX_CODEC_ADDRESS, return -EINVAL);

	if (bus->caddr_tbl[codec_addr]) {
		snd_printk(KERN_ERR "hda_codec: "
			   "address 0x%x is already occupied\n", codec_addr);
		return -EBUSY;
	}

	codec = kzalloc(sizeof(*codec), GFP_KERNEL);
	if (codec == NULL) {
		snd_printk(KERN_ERR "can't allocate struct hda_codec\n");
		return -ENOMEM;
	}

	codec->bus = bus;
	codec->addr = codec_addr;
	mutex_init(&codec->spdif_mutex);
	init_amp_hash(codec);

	list_add_tail(&codec->list, &bus->codec_list);
	bus->caddr_tbl[codec_addr] = codec;

	codec->vendor_id = snd_hda_param_read(codec, AC_NODE_ROOT,
					      AC_PAR_VENDOR_ID);
	if (codec->vendor_id == -1)
		/* read again, hopefully the access method was corrected
		 * in the last read...
		 */
		codec->vendor_id = snd_hda_param_read(codec, AC_NODE_ROOT,
						      AC_PAR_VENDOR_ID);
	codec->subsystem_id = snd_hda_param_read(codec, AC_NODE_ROOT,
						 AC_PAR_SUBSYSTEM_ID);
	codec->revision_id = snd_hda_param_read(codec, AC_NODE_ROOT,
						AC_PAR_REV_ID);

	setup_fg_nodes(codec);
	if (!codec->afg && !codec->mfg) {
		snd_printdd("hda_codec: no AFG or MFG node found\n");
		snd_hda_codec_free(codec);
		return -ENODEV;
	}

	if (read_widget_caps(codec, codec->afg ? codec->afg : codec->mfg) < 0) {
		snd_printk(KERN_ERR "hda_codec: cannot malloc\n");
		snd_hda_codec_free(codec);
		return -ENOMEM;
	}

	if (!codec->subsystem_id) {
		hda_nid_t nid = codec->afg ? codec->afg : codec->mfg;
		codec->subsystem_id =
			snd_hda_codec_read(codec, nid, 0,
					   AC_VERB_GET_SUBSYSTEM_ID, 0);
	}

	codec->preset = find_codec_preset(codec);
	/* audio codec should override the mixer name */
	if (codec->afg || !*bus->card->mixername)
		snd_hda_get_codec_name(codec, bus->card->mixername,
				       sizeof(bus->card->mixername));

	if (codec->preset && codec->preset->patch)
		err = codec->preset->patch(codec);
	else
		err = snd_hda_parse_generic_codec(codec);
	if (err < 0) {
		snd_hda_codec_free(codec);
		return err;
	}

	if (codec->patch_ops.unsol_event)
		init_unsol_queue(bus);

	snd_hda_codec_proc_new(codec);

	sprintf(component, "HDA:%08x", codec->vendor_id);
	snd_component_add(codec->bus->card, component);

	if (codecp)
		*codecp = codec;
	return 0;
}

/**
 * snd_hda_codec_setup_stream - set up the codec for streaming
 * @codec: the CODEC to set up
 * @nid: the NID to set up
 * @stream_tag: stream tag to pass, it's between 0x1 and 0xf.
 * @channel_id: channel id to pass, zero based.
 * @format: stream format.
 */
void snd_hda_codec_setup_stream(struct hda_codec *codec, hda_nid_t nid,
				u32 stream_tag,
				int channel_id, int format)
{
	if (!nid)
		return;

	snd_printdd("hda_codec_setup_stream: "
		    "NID=0x%x, stream=0x%x, channel=%d, format=0x%x\n",
		    nid, stream_tag, channel_id, format);
	snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_CHANNEL_STREAMID,
			    (stream_tag << 4) | channel_id);
	msleep(1);
	snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_STREAM_FORMAT, format);
}

/*
 * amp access functions
 */

/* FIXME: more better hash key? */
#define HDA_HASH_KEY(nid,dir,idx) (u32)((nid) + ((idx) << 16) + ((dir) << 24))
#define INFO_AMP_CAPS	(1<<0)
#define INFO_AMP_VOL(ch)	(1 << (1 + (ch)))

/* initialize the hash table */
static void __devinit init_amp_hash(struct hda_codec *codec)
{
	memset(codec->amp_hash, 0xff, sizeof(codec->amp_hash));
	codec->num_amp_entries = 0;
	codec->amp_info_size = 0;
	codec->amp_info = NULL;
}

/* query the hash.  allocate an entry if not found. */
static struct hda_amp_info *get_alloc_amp_hash(struct hda_codec *codec, u32 key)
{
	u16 idx = key % (u16)ARRAY_SIZE(codec->amp_hash);
	u16 cur = codec->amp_hash[idx];
	struct hda_amp_info *info;

	while (cur != 0xffff) {
		info = &codec->amp_info[cur];
		if (info->key == key)
			return info;
		cur = info->next;
	}

	/* add a new hash entry */
	if (codec->num_amp_entries >= codec->amp_info_size) {
		/* reallocate the array */
		int new_size = codec->amp_info_size + 64;
		struct hda_amp_info *new_info;
		new_info = kcalloc(new_size, sizeof(struct hda_amp_info),
				   GFP_KERNEL);
		if (!new_info) {
			snd_printk(KERN_ERR "hda_codec: "
				   "can't malloc amp_info\n");
			return NULL;
		}
		if (codec->amp_info) {
			memcpy(new_info, codec->amp_info,
			       codec->amp_info_size *
			       sizeof(struct hda_amp_info));
			kfree(codec->amp_info);
		}
		codec->amp_info_size = new_size;
		codec->amp_info = new_info;
	}
	cur = codec->num_amp_entries++;
	info = &codec->amp_info[cur];
	info->key = key;
	info->status = 0; /* not initialized yet */
	info->next = codec->amp_hash[idx];
	codec->amp_hash[idx] = cur;

	return info;
}

/*
 * query AMP capabilities for the given widget and direction
 */
static u32 query_amp_caps(struct hda_codec *codec, hda_nid_t nid, int direction)
{
	struct hda_amp_info *info;

	info = get_alloc_amp_hash(codec, HDA_HASH_KEY(nid, direction, 0));
	if (!info)
		return 0;
	if (!(info->status & INFO_AMP_CAPS)) {
		if (!(get_wcaps(codec, nid) & AC_WCAP_AMP_OVRD))
			nid = codec->afg;
		info->amp_caps = snd_hda_param_read(codec, nid,
						    direction == HDA_OUTPUT ?
						    AC_PAR_AMP_OUT_CAP :
						    AC_PAR_AMP_IN_CAP);
		if (info->amp_caps)
			info->status |= INFO_AMP_CAPS;
	}
	return info->amp_caps;
}

int snd_hda_override_amp_caps(struct hda_codec *codec, hda_nid_t nid, int dir,
			      unsigned int caps)
{
	struct hda_amp_info *info;

	info = get_alloc_amp_hash(codec, HDA_HASH_KEY(nid, dir, 0));
	if (!info)
		return -EINVAL;
	info->amp_caps = caps;
	info->status |= INFO_AMP_CAPS;
	return 0;
}

/*
 * read the current volume to info
 * if the cache exists, read the cache value.
 */
static unsigned int get_vol_mute(struct hda_codec *codec,
				 struct hda_amp_info *info, hda_nid_t nid,
				 int ch, int direction, int index)
{
	u32 val, parm;

	if (info->status & INFO_AMP_VOL(ch))
		return info->vol[ch];

	parm = ch ? AC_AMP_GET_RIGHT : AC_AMP_GET_LEFT;
	parm |= direction == HDA_OUTPUT ? AC_AMP_GET_OUTPUT : AC_AMP_GET_INPUT;
	parm |= index;
	val = snd_hda_codec_read(codec, nid, 0,
				 AC_VERB_GET_AMP_GAIN_MUTE, parm);
	info->vol[ch] = val & 0xff;
	info->status |= INFO_AMP_VOL(ch);
	return info->vol[ch];
}

/*
 * write the current volume in info to the h/w and update the cache
 */
static void put_vol_mute(struct hda_codec *codec, struct hda_amp_info *info,
			 hda_nid_t nid, int ch, int direction, int index,
			 int val)
{
	u32 parm;

	parm = ch ? AC_AMP_SET_RIGHT : AC_AMP_SET_LEFT;
	parm |= direction == HDA_OUTPUT ? AC_AMP_SET_OUTPUT : AC_AMP_SET_INPUT;
	parm |= index << AC_AMP_SET_INDEX_SHIFT;
	parm |= val;
	snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_AMP_GAIN_MUTE, parm);
	info->vol[ch] = val;
}

/*
 * read AMP value.  The volume is between 0 to 0x7f, 0x80 = mute bit.
 */
int snd_hda_codec_amp_read(struct hda_codec *codec, hda_nid_t nid, int ch,
			   int direction, int index)
{
	struct hda_amp_info *info;
	info = get_alloc_amp_hash(codec, HDA_HASH_KEY(nid, direction, index));
	if (!info)
		return 0;
	return get_vol_mute(codec, info, nid, ch, direction, index);
}

/*
 * update the AMP value, mask = bit mask to set, val = the value
 */
int snd_hda_codec_amp_update(struct hda_codec *codec, hda_nid_t nid, int ch,
			     int direction, int idx, int mask, int val)
{
	struct hda_amp_info *info;

	info = get_alloc_amp_hash(codec, HDA_HASH_KEY(nid, direction, idx));
	if (!info)
		return 0;
	val &= mask;
	val |= get_vol_mute(codec, info, nid, ch, direction, idx) & ~mask;
	if (info->vol[ch] == val && !codec->in_resume)
		return 0;
	put_vol_mute(codec, info, nid, ch, direction, idx, val);
	return 1;
}


/*
 * AMP control callbacks
 */
/* retrieve parameters from private_value */
#define get_amp_nid(kc)		((kc)->private_value & 0xffff)
#define get_amp_channels(kc)	(((kc)->private_value >> 16) & 0x3)
#define get_amp_direction(kc)	(((kc)->private_value >> 18) & 0x1)
#define get_amp_index(kc)	(((kc)->private_value >> 19) & 0xf)

/* volume */
int snd_hda_mixer_amp_volume_info(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_info *uinfo)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	u16 nid = get_amp_nid(kcontrol);
	u8 chs = get_amp_channels(kcontrol);
	int dir = get_amp_direction(kcontrol);
	u32 caps;

	caps = query_amp_caps(codec, nid, dir);
	/* num steps */
	caps = (caps & AC_AMPCAP_NUM_STEPS) >> AC_AMPCAP_NUM_STEPS_SHIFT;
	if (!caps) {
		printk(KERN_WARNING "hda_codec: "
		       "num_steps = 0 for NID=0x%x\n", nid);
		return -EINVAL;
	}
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = chs == 3 ? 2 : 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = caps;
	return 0;
}

int snd_hda_mixer_amp_volume_get(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	hda_nid_t nid = get_amp_nid(kcontrol);
	int chs = get_amp_channels(kcontrol);
	int dir = get_amp_direction(kcontrol);
	int idx = get_amp_index(kcontrol);
	long *valp = ucontrol->value.integer.value;

	if (chs & 1)
		*valp++ = snd_hda_codec_amp_read(codec, nid, 0, dir, idx) & 0x7f;
	if (chs & 2)
		*valp = snd_hda_codec_amp_read(codec, nid, 1, dir, idx) & 0x7f;
	return 0;
}

int snd_hda_mixer_amp_volume_put(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	hda_nid_t nid = get_amp_nid(kcontrol);
	int chs = get_amp_channels(kcontrol);
	int dir = get_amp_direction(kcontrol);
	int idx = get_amp_index(kcontrol);
	long *valp = ucontrol->value.integer.value;
	int change = 0;

	if (chs & 1) {
		change = snd_hda_codec_amp_update(codec, nid, 0, dir, idx,
						  0x7f, *valp);
		valp++;
	}
	if (chs & 2)
		change |= snd_hda_codec_amp_update(codec, nid, 1, dir, idx,
						   0x7f, *valp);
	return change;
}

int snd_hda_mixer_amp_tlv(struct snd_kcontrol *kcontrol, int op_flag,
			  unsigned int size, unsigned int __user *_tlv)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	hda_nid_t nid = get_amp_nid(kcontrol);
	int dir = get_amp_direction(kcontrol);
	u32 caps, val1, val2;

	if (size < 4 * sizeof(unsigned int))
		return -ENOMEM;
	caps = query_amp_caps(codec, nid, dir);
	val2 = (caps & AC_AMPCAP_STEP_SIZE) >> AC_AMPCAP_STEP_SIZE_SHIFT;
	val2 = (val2 + 1) * 25;
	val1 = -((caps & AC_AMPCAP_OFFSET) >> AC_AMPCAP_OFFSET_SHIFT);
	val1 = ((int)val1) * ((int)val2);
	if (put_user(SNDRV_CTL_TLVT_DB_SCALE, _tlv))
		return -EFAULT;
	if (put_user(2 * sizeof(unsigned int), _tlv + 1))
		return -EFAULT;
	if (put_user(val1, _tlv + 2))
		return -EFAULT;
	if (put_user(val2, _tlv + 3))
		return -EFAULT;
	return 0;
}

/* switch */
int snd_hda_mixer_amp_switch_info(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_info *uinfo)
{
	int chs = get_amp_channels(kcontrol);

	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = chs == 3 ? 2 : 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

int snd_hda_mixer_amp_switch_get(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	hda_nid_t nid = get_amp_nid(kcontrol);
	int chs = get_amp_channels(kcontrol);
	int dir = get_amp_direction(kcontrol);
	int idx = get_amp_index(kcontrol);
	long *valp = ucontrol->value.integer.value;

	if (chs & 1)
		*valp++ = (snd_hda_codec_amp_read(codec, nid, 0, dir, idx) &
			   0x80) ? 0 : 1;
	if (chs & 2)
		*valp = (snd_hda_codec_amp_read(codec, nid, 1, dir, idx) &
			 0x80) ? 0 : 1;
	return 0;
}

int snd_hda_mixer_amp_switch_put(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	hda_nid_t nid = get_amp_nid(kcontrol);
	int chs = get_amp_channels(kcontrol);
	int dir = get_amp_direction(kcontrol);
	int idx = get_amp_index(kcontrol);
	long *valp = ucontrol->value.integer.value;
	int change = 0;

	if (chs & 1) {
		change = snd_hda_codec_amp_update(codec, nid, 0, dir, idx,
						  0x80, *valp ? 0 : 0x80);
		valp++;
	}
	if (chs & 2)
		change |= snd_hda_codec_amp_update(codec, nid, 1, dir, idx,
						   0x80, *valp ? 0 : 0x80);
	
	return change;
}

/*
 * bound volume controls
 *
 * bind multiple volumes (# indices, from 0)
 */

#define AMP_VAL_IDX_SHIFT	19
#define AMP_VAL_IDX_MASK	(0x0f<<19)

int snd_hda_mixer_bind_switch_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	unsigned long pval;
	int err;

	mutex_lock(&codec->spdif_mutex); /* reuse spdif_mutex */
	pval = kcontrol->private_value;
	kcontrol->private_value = pval & ~AMP_VAL_IDX_MASK; /* index 0 */
	err = snd_hda_mixer_amp_switch_get(kcontrol, ucontrol);
	kcontrol->private_value = pval;
	mutex_unlock(&codec->spdif_mutex);
	return err;
}

int snd_hda_mixer_bind_switch_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	unsigned long pval;
	int i, indices, err = 0, change = 0;

	mutex_lock(&codec->spdif_mutex); /* reuse spdif_mutex */
	pval = kcontrol->private_value;
	indices = (pval & AMP_VAL_IDX_MASK) >> AMP_VAL_IDX_SHIFT;
	for (i = 0; i < indices; i++) {
		kcontrol->private_value = (pval & ~AMP_VAL_IDX_MASK) |
			(i << AMP_VAL_IDX_SHIFT);
		err = snd_hda_mixer_amp_switch_put(kcontrol, ucontrol);
		if (err < 0)
			break;
		change |= err;
	}
	kcontrol->private_value = pval;
	mutex_unlock(&codec->spdif_mutex);
	return err < 0 ? err : change;
}

/*
 * SPDIF out controls
 */

static int snd_hda_spdif_mask_info(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_IEC958;
	uinfo->count = 1;
	return 0;
}

static int snd_hda_spdif_cmask_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.iec958.status[0] = IEC958_AES0_PROFESSIONAL |
					   IEC958_AES0_NONAUDIO |
					   IEC958_AES0_CON_EMPHASIS_5015 |
					   IEC958_AES0_CON_NOT_COPYRIGHT;
	ucontrol->value.iec958.status[1] = IEC958_AES1_CON_CATEGORY |
					   IEC958_AES1_CON_ORIGINAL;
	return 0;
}

static int snd_hda_spdif_pmask_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.iec958.status[0] = IEC958_AES0_PROFESSIONAL |
					   IEC958_AES0_NONAUDIO |
					   IEC958_AES0_PRO_EMPHASIS_5015;
	return 0;
}

static int snd_hda_spdif_default_get(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);

	ucontrol->value.iec958.status[0] = codec->spdif_status & 0xff;
	ucontrol->value.iec958.status[1] = (codec->spdif_status >> 8) & 0xff;
	ucontrol->value.iec958.status[2] = (codec->spdif_status >> 16) & 0xff;
	ucontrol->value.iec958.status[3] = (codec->spdif_status >> 24) & 0xff;

	return 0;
}

/* convert from SPDIF status bits to HDA SPDIF bits
 * bit 0 (DigEn) is always set zero (to be filled later)
 */
static unsigned short convert_from_spdif_status(unsigned int sbits)
{
	unsigned short val = 0;

	if (sbits & IEC958_AES0_PROFESSIONAL)
		val |= AC_DIG1_PROFESSIONAL;
	if (sbits & IEC958_AES0_NONAUDIO)
		val |= AC_DIG1_NONAUDIO;
	if (sbits & IEC958_AES0_PROFESSIONAL) {
		if ((sbits & IEC958_AES0_PRO_EMPHASIS) ==
		    IEC958_AES0_PRO_EMPHASIS_5015)
			val |= AC_DIG1_EMPHASIS;
	} else {
		if ((sbits & IEC958_AES0_CON_EMPHASIS) ==
		    IEC958_AES0_CON_EMPHASIS_5015)
			val |= AC_DIG1_EMPHASIS;
		if (!(sbits & IEC958_AES0_CON_NOT_COPYRIGHT))
			val |= AC_DIG1_COPYRIGHT;
		if (sbits & (IEC958_AES1_CON_ORIGINAL << 8))
			val |= AC_DIG1_LEVEL;
		val |= sbits & (IEC958_AES1_CON_CATEGORY << 8);
	}
	return val;
}

/* convert to SPDIF status bits from HDA SPDIF bits
 */
static unsigned int convert_to_spdif_status(unsigned short val)
{
	unsigned int sbits = 0;

	if (val & AC_DIG1_NONAUDIO)
		sbits |= IEC958_AES0_NONAUDIO;
	if (val & AC_DIG1_PROFESSIONAL)
		sbits |= IEC958_AES0_PROFESSIONAL;
	if (sbits & IEC958_AES0_PROFESSIONAL) {
		if (sbits & AC_DIG1_EMPHASIS)
			sbits |= IEC958_AES0_PRO_EMPHASIS_5015;
	} else {
		if (val & AC_DIG1_EMPHASIS)
			sbits |= IEC958_AES0_CON_EMPHASIS_5015;
		if (!(val & AC_DIG1_COPYRIGHT))
			sbits |= IEC958_AES0_CON_NOT_COPYRIGHT;
		if (val & AC_DIG1_LEVEL)
			sbits |= (IEC958_AES1_CON_ORIGINAL << 8);
		sbits |= val & (0x7f << 8);
	}
	return sbits;
}

static int snd_hda_spdif_default_put(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	hda_nid_t nid = kcontrol->private_value;
	unsigned short val;
	int change;

	mutex_lock(&codec->spdif_mutex);
	codec->spdif_status = ucontrol->value.iec958.status[0] |
		((unsigned int)ucontrol->value.iec958.status[1] << 8) |
		((unsigned int)ucontrol->value.iec958.status[2] << 16) |
		((unsigned int)ucontrol->value.iec958.status[3] << 24);
	val = convert_from_spdif_status(codec->spdif_status);
	val |= codec->spdif_ctls & 1;
	change = codec->spdif_ctls != val;
	codec->spdif_ctls = val;

	if (change || codec->in_resume) {
		snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_DIGI_CONVERT_1,
				    val & 0xff);
		snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_DIGI_CONVERT_2,
				    val >> 8);
	}

	mutex_unlock(&codec->spdif_mutex);
	return change;
}

static int snd_hda_spdif_out_switch_info(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int snd_hda_spdif_out_switch_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = codec->spdif_ctls & AC_DIG1_ENABLE;
	return 0;
}

static int snd_hda_spdif_out_switch_put(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	hda_nid_t nid = kcontrol->private_value;
	unsigned short val;
	int change;

	mutex_lock(&codec->spdif_mutex);
	val = codec->spdif_ctls & ~AC_DIG1_ENABLE;
	if (ucontrol->value.integer.value[0])
		val |= AC_DIG1_ENABLE;
	change = codec->spdif_ctls != val;
	if (change || codec->in_resume) {
		codec->spdif_ctls = val;
		snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_DIGI_CONVERT_1,
				    val & 0xff);
		/* unmute amp switch (if any) */
		if ((get_wcaps(codec, nid) & AC_WCAP_OUT_AMP) &&
		    (val & AC_DIG1_ENABLE))
			snd_hda_codec_write(codec, nid, 0,
					    AC_VERB_SET_AMP_GAIN_MUTE,
					    AC_AMP_SET_RIGHT | AC_AMP_SET_LEFT |
					    AC_AMP_SET_OUTPUT);
	}
	mutex_unlock(&codec->spdif_mutex);
	return change;
}

static struct snd_kcontrol_new dig_mixes[] = {
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = SNDRV_CTL_NAME_IEC958("",PLAYBACK,CON_MASK),
		.info = snd_hda_spdif_mask_info,
		.get = snd_hda_spdif_cmask_get,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = SNDRV_CTL_NAME_IEC958("",PLAYBACK,PRO_MASK),
		.info = snd_hda_spdif_mask_info,
		.get = snd_hda_spdif_pmask_get,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = SNDRV_CTL_NAME_IEC958("",PLAYBACK,DEFAULT),
		.info = snd_hda_spdif_mask_info,
		.get = snd_hda_spdif_default_get,
		.put = snd_hda_spdif_default_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = SNDRV_CTL_NAME_IEC958("",PLAYBACK,SWITCH),
		.info = snd_hda_spdif_out_switch_info,
		.get = snd_hda_spdif_out_switch_get,
		.put = snd_hda_spdif_out_switch_put,
	},
	{ } /* end */
};

/**
 * snd_hda_create_spdif_out_ctls - create Output SPDIF-related controls
 * @codec: the HDA codec
 * @nid: audio out widget NID
 *
 * Creates controls related with the SPDIF output.
 * Called from each patch supporting the SPDIF out.
 *
 * Returns 0 if successful, or a negative error code.
 */
int __devinit snd_hda_create_spdif_out_ctls(struct hda_codec *codec,
					    hda_nid_t nid)
{
	int err;
	struct snd_kcontrol *kctl;
	struct snd_kcontrol_new *dig_mix;

	for (dig_mix = dig_mixes; dig_mix->name; dig_mix++) {
		kctl = snd_ctl_new1(dig_mix, codec);
		kctl->private_value = nid;
		err = snd_ctl_add(codec->bus->card, kctl);
		if (err < 0)
			return err;
	}
	codec->spdif_ctls =
		snd_hda_codec_read(codec, nid, 0, AC_VERB_GET_DIGI_CONVERT, 0);
	codec->spdif_status = convert_to_spdif_status(codec->spdif_ctls);
	return 0;
}

/*
 * SPDIF input
 */

#define snd_hda_spdif_in_switch_info	snd_hda_spdif_out_switch_info

static int snd_hda_spdif_in_switch_get(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = codec->spdif_in_enable;
	return 0;
}

static int snd_hda_spdif_in_switch_put(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	hda_nid_t nid = kcontrol->private_value;
	unsigned int val = !!ucontrol->value.integer.value[0];
	int change;

	mutex_lock(&codec->spdif_mutex);
	change = codec->spdif_in_enable != val;
	if (change || codec->in_resume) {
		codec->spdif_in_enable = val;
		snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_DIGI_CONVERT_1,
				    val);
	}
	mutex_unlock(&codec->spdif_mutex);
	return change;
}

static int snd_hda_spdif_in_status_get(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	hda_nid_t nid = kcontrol->private_value;
	unsigned short val;
	unsigned int sbits;

	val = snd_hda_codec_read(codec, nid, 0, AC_VERB_GET_DIGI_CONVERT, 0);
	sbits = convert_to_spdif_status(val);
	ucontrol->value.iec958.status[0] = sbits;
	ucontrol->value.iec958.status[1] = sbits >> 8;
	ucontrol->value.iec958.status[2] = sbits >> 16;
	ucontrol->value.iec958.status[3] = sbits >> 24;
	return 0;
}

static struct snd_kcontrol_new dig_in_ctls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = SNDRV_CTL_NAME_IEC958("",CAPTURE,SWITCH),
		.info = snd_hda_spdif_in_switch_info,
		.get = snd_hda_spdif_in_switch_get,
		.put = snd_hda_spdif_in_switch_put,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = SNDRV_CTL_NAME_IEC958("",CAPTURE,DEFAULT),
		.info = snd_hda_spdif_mask_info,
		.get = snd_hda_spdif_in_status_get,
	},
	{ } /* end */
};

/**
 * snd_hda_create_spdif_in_ctls - create Input SPDIF-related controls
 * @codec: the HDA codec
 * @nid: audio in widget NID
 *
 * Creates controls related with the SPDIF input.
 * Called from each patch supporting the SPDIF in.
 *
 * Returns 0 if successful, or a negative error code.
 */
int __devinit snd_hda_create_spdif_in_ctls(struct hda_codec *codec,
					   hda_nid_t nid)
{
	int err;
	struct snd_kcontrol *kctl;
	struct snd_kcontrol_new *dig_mix;

	for (dig_mix = dig_in_ctls; dig_mix->name; dig_mix++) {
		kctl = snd_ctl_new1(dig_mix, codec);
		kctl->private_value = nid;
		err = snd_ctl_add(codec->bus->card, kctl);
		if (err < 0)
			return err;
	}
	codec->spdif_in_enable =
		snd_hda_codec_read(codec, nid, 0, AC_VERB_GET_DIGI_CONVERT, 0) &
		AC_DIG1_ENABLE;
	return 0;
}


/*
 * set power state of the codec
 */
static void hda_set_power_state(struct hda_codec *codec, hda_nid_t fg,
				unsigned int power_state)
{
	hda_nid_t nid, nid_start;
	int nodes;

	snd_hda_codec_write(codec, fg, 0, AC_VERB_SET_POWER_STATE,
			    power_state);

	nodes = snd_hda_get_sub_nodes(codec, fg, &nid_start);
	for (nid = nid_start; nid < nodes + nid_start; nid++) {
		if (get_wcaps(codec, nid) & AC_WCAP_POWER)
			snd_hda_codec_write(codec, nid, 0,
					    AC_VERB_SET_POWER_STATE,
					    power_state);
	}

	if (power_state == AC_PWRST_D0)
		msleep(10);
}


/**
 * snd_hda_build_controls - build mixer controls
 * @bus: the BUS
 *
 * Creates mixer controls for each codec included in the bus.
 *
 * Returns 0 if successful, otherwise a negative error code.
 */
int __devinit snd_hda_build_controls(struct hda_bus *bus)
{
	struct hda_codec *codec;

	/* build controls */
	list_for_each_entry(codec, &bus->codec_list, list) {
		int err;
		if (!codec->patch_ops.build_controls)
			continue;
		err = codec->patch_ops.build_controls(codec);
		if (err < 0)
			return err;
	}

	/* initialize */
	list_for_each_entry(codec, &bus->codec_list, list) {
		int err;
		hda_set_power_state(codec,
				    codec->afg ? codec->afg : codec->mfg,
				    AC_PWRST_D0);
		if (!codec->patch_ops.init)
			continue;
		err = codec->patch_ops.init(codec);
		if (err < 0)
			return err;
	}
	return 0;
}

/*
 * stream formats
 */
struct hda_rate_tbl {
	unsigned int hz;
	unsigned int alsa_bits;
	unsigned int hda_fmt;
};

static struct hda_rate_tbl rate_bits[] = {
	/* rate in Hz, ALSA rate bitmask, HDA format value */

	/* autodetected value used in snd_hda_query_supported_pcm */
	{ 8000, SNDRV_PCM_RATE_8000, 0x0500 }, /* 1/6 x 48 */
	{ 11025, SNDRV_PCM_RATE_11025, 0x4300 }, /* 1/4 x 44 */
	{ 16000, SNDRV_PCM_RATE_16000, 0x0200 }, /* 1/3 x 48 */
	{ 22050, SNDRV_PCM_RATE_22050, 0x4100 }, /* 1/2 x 44 */
	{ 32000, SNDRV_PCM_RATE_32000, 0x0a00 }, /* 2/3 x 48 */
	{ 44100, SNDRV_PCM_RATE_44100, 0x4000 }, /* 44 */
	{ 48000, SNDRV_PCM_RATE_48000, 0x0000 }, /* 48 */
	{ 88200, SNDRV_PCM_RATE_88200, 0x4800 }, /* 2 x 44 */
	{ 96000, SNDRV_PCM_RATE_96000, 0x0800 }, /* 2 x 48 */
	{ 176400, SNDRV_PCM_RATE_176400, 0x5800 },/* 4 x 44 */
	{ 192000, SNDRV_PCM_RATE_192000, 0x1800 }, /* 4 x 48 */
#define AC_PAR_PCM_RATE_BITS	11
	/* up to bits 10, 384kHZ isn't supported properly */

	/* not autodetected value */
	{ 9600, SNDRV_PCM_RATE_KNOT, 0x0400 }, /* 1/5 x 48 */

	{ 0 } /* terminator */
};

/**
 * snd_hda_calc_stream_format - calculate format bitset
 * @rate: the sample rate
 * @channels: the number of channels
 * @format: the PCM format (SNDRV_PCM_FORMAT_XXX)
 * @maxbps: the max. bps
 *
 * Calculate the format bitset from the given rate, channels and th PCM format.
 *
 * Return zero if invalid.
 */
unsigned int snd_hda_calc_stream_format(unsigned int rate,
					unsigned int channels,
					unsigned int format,
					unsigned int maxbps)
{
	int i;
	unsigned int val = 0;

	for (i = 0; rate_bits[i].hz; i++)
		if (rate_bits[i].hz == rate) {
			val = rate_bits[i].hda_fmt;
			break;
		}
	if (!rate_bits[i].hz) {
		snd_printdd("invalid rate %d\n", rate);
		return 0;
	}

	if (channels == 0 || channels > 8) {
		snd_printdd("invalid channels %d\n", channels);
		return 0;
	}
	val |= channels - 1;

	switch (snd_pcm_format_width(format)) {
	case 8:  val |= 0x00; break;
	case 16: val |= 0x10; break;
	case 20:
	case 24:
	case 32:
		if (maxbps >= 32)
			val |= 0x40;
		else if (maxbps >= 24)
			val |= 0x30;
		else
			val |= 0x20;
		break;
	default:
		snd_printdd("invalid format width %d\n",
			    snd_pcm_format_width(format));
		return 0;
	}

	return val;
}

/**
 * snd_hda_query_supported_pcm - query the supported PCM rates and formats
 * @codec: the HDA codec
 * @nid: NID to query
 * @ratesp: the pointer to store the detected rate bitflags
 * @formatsp: the pointer to store the detected formats
 * @bpsp: the pointer to store the detected format widths
 *
 * Queries the supported PCM rates and formats.  The NULL @ratesp, @formatsp
 * or @bsps argument is ignored.
 *
 * Returns 0 if successful, otherwise a negative error code.
 */
int snd_hda_query_supported_pcm(struct hda_codec *codec, hda_nid_t nid,
				u32 *ratesp, u64 *formatsp, unsigned int *bpsp)
{
	int i;
	unsigned int val, streams;

	val = 0;
	if (nid != codec->afg &&
	    (get_wcaps(codec, nid) & AC_WCAP_FORMAT_OVRD)) {
		val = snd_hda_param_read(codec, nid, AC_PAR_PCM);
		if (val == -1)
			return -EIO;
	}
	if (!val)
		val = snd_hda_param_read(codec, codec->afg, AC_PAR_PCM);

	if (ratesp) {
		u32 rates = 0;
		for (i = 0; i < AC_PAR_PCM_RATE_BITS; i++) {
			if (val & (1 << i))
				rates |= rate_bits[i].alsa_bits;
		}
		*ratesp = rates;
	}

	if (formatsp || bpsp) {
		u64 formats = 0;
		unsigned int bps;
		unsigned int wcaps;

		wcaps = get_wcaps(codec, nid);
		streams = snd_hda_param_read(codec, nid, AC_PAR_STREAM);
		if (streams == -1)
			return -EIO;
		if (!streams) {
			streams = snd_hda_param_read(codec, codec->afg,
						     AC_PAR_STREAM);
			if (streams == -1)
				return -EIO;
		}

		bps = 0;
		if (streams & AC_SUPFMT_PCM) {
			if (val & AC_SUPPCM_BITS_8) {
				formats |= SNDRV_PCM_FMTBIT_U8;
				bps = 8;
			}
			if (val & AC_SUPPCM_BITS_16) {
				formats |= SNDRV_PCM_FMTBIT_S16_LE;
				bps = 16;
			}
			if (wcaps & AC_WCAP_DIGITAL) {
				if (val & AC_SUPPCM_BITS_32)
					formats |= SNDRV_PCM_FMTBIT_IEC958_SUBFRAME_LE;
				if (val & (AC_SUPPCM_BITS_20|AC_SUPPCM_BITS_24))
					formats |= SNDRV_PCM_FMTBIT_S32_LE;
				if (val & AC_SUPPCM_BITS_24)
					bps = 24;
				else if (val & AC_SUPPCM_BITS_20)
					bps = 20;
			} else if (val & (AC_SUPPCM_BITS_20|AC_SUPPCM_BITS_24|
					  AC_SUPPCM_BITS_32)) {
				formats |= SNDRV_PCM_FMTBIT_S32_LE;
				if (val & AC_SUPPCM_BITS_32)
					bps = 32;
				else if (val & AC_SUPPCM_BITS_24)
					bps = 24;
				else if (val & AC_SUPPCM_BITS_20)
					bps = 20;
			}
		}
		else if (streams == AC_SUPFMT_FLOAT32) {
			/* should be exclusive */
			formats |= SNDRV_PCM_FMTBIT_FLOAT_LE;
			bps = 32;
		} else if (streams == AC_SUPFMT_AC3) {
			/* should be exclusive */
			/* temporary hack: we have still no proper support
			 * for the direct AC3 stream...
			 */
			formats |= SNDRV_PCM_FMTBIT_U8;
			bps = 8;
		}
		if (formatsp)
			*formatsp = formats;
		if (bpsp)
			*bpsp = bps;
	}

	return 0;
}

/**
 * snd_hda_is_supported_format - check whether the given node supports
 * the format val
 *
 * Returns 1 if supported, 0 if not.
 */
int snd_hda_is_supported_format(struct hda_codec *codec, hda_nid_t nid,
				unsigned int format)
{
	int i;
	unsigned int val = 0, rate, stream;

	if (nid != codec->afg &&
	    (get_wcaps(codec, nid) & AC_WCAP_FORMAT_OVRD)) {
		val = snd_hda_param_read(codec, nid, AC_PAR_PCM);
		if (val == -1)
			return 0;
	}
	if (!val) {
		val = snd_hda_param_read(codec, codec->afg, AC_PAR_PCM);
		if (val == -1)
			return 0;
	}

	rate = format & 0xff00;
	for (i = 0; i < AC_PAR_PCM_RATE_BITS; i++)
		if (rate_bits[i].hda_fmt == rate) {
			if (val & (1 << i))
				break;
			return 0;
		}
	if (i >= AC_PAR_PCM_RATE_BITS)
		return 0;

	stream = snd_hda_param_read(codec, nid, AC_PAR_STREAM);
	if (stream == -1)
		return 0;
	if (!stream && nid != codec->afg)
		stream = snd_hda_param_read(codec, codec->afg, AC_PAR_STREAM);
	if (!stream || stream == -1)
		return 0;

	if (stream & AC_SUPFMT_PCM) {
		switch (format & 0xf0) {
		case 0x00:
			if (!(val & AC_SUPPCM_BITS_8))
				return 0;
			break;
		case 0x10:
			if (!(val & AC_SUPPCM_BITS_16))
				return 0;
			break;
		case 0x20:
			if (!(val & AC_SUPPCM_BITS_20))
				return 0;
			break;
		case 0x30:
			if (!(val & AC_SUPPCM_BITS_24))
				return 0;
			break;
		case 0x40:
			if (!(val & AC_SUPPCM_BITS_32))
				return 0;
			break;
		default:
			return 0;
		}
	} else {
		/* FIXME: check for float32 and AC3? */
	}

	return 1;
}

/*
 * PCM stuff
 */
static int hda_pcm_default_open_close(struct hda_pcm_stream *hinfo,
				      struct hda_codec *codec,
				      struct snd_pcm_substream *substream)
{
	return 0;
}

static int hda_pcm_default_prepare(struct hda_pcm_stream *hinfo,
				   struct hda_codec *codec,
				   unsigned int stream_tag,
				   unsigned int format,
				   struct snd_pcm_substream *substream)
{
	snd_hda_codec_setup_stream(codec, hinfo->nid, stream_tag, 0, format);
	return 0;
}

static int hda_pcm_default_cleanup(struct hda_pcm_stream *hinfo,
				   struct hda_codec *codec,
				   struct snd_pcm_substream *substream)
{
	snd_hda_codec_setup_stream(codec, hinfo->nid, 0, 0, 0);
	return 0;
}

static int __devinit set_pcm_default_values(struct hda_codec *codec,
					    struct hda_pcm_stream *info)
{
	/* query support PCM information from the given NID */
	if (info->nid && (!info->rates || !info->formats)) {
		snd_hda_query_supported_pcm(codec, info->nid,
				info->rates ? NULL : &info->rates,
				info->formats ? NULL : &info->formats,
				info->maxbps ? NULL : &info->maxbps);
	}
	if (info->ops.open == NULL)
		info->ops.open = hda_pcm_default_open_close;
	if (info->ops.close == NULL)
		info->ops.close = hda_pcm_default_open_close;
	if (info->ops.prepare == NULL) {
		snd_assert(info->nid, return -EINVAL);
		info->ops.prepare = hda_pcm_default_prepare;
	}
	if (info->ops.cleanup == NULL) {
		snd_assert(info->nid, return -EINVAL);
		info->ops.cleanup = hda_pcm_default_cleanup;
	}
	return 0;
}

/**
 * snd_hda_build_pcms - build PCM information
 * @bus: the BUS
 *
 * Create PCM information for each codec included in the bus.
 *
 * The build_pcms codec patch is requested to set up codec->num_pcms and
 * codec->pcm_info properly.  The array is referred by the top-level driver
 * to create its PCM instances.
 * The allocated codec->pcm_info should be released in codec->patch_ops.free
 * callback.
 *
 * At least, substreams, channels_min and channels_max must be filled for
 * each stream.  substreams = 0 indicates that the stream doesn't exist.
 * When rates and/or formats are zero, the supported values are queried
 * from the given nid.  The nid is used also by the default ops.prepare
 * and ops.cleanup callbacks.
 *
 * The driver needs to call ops.open in its open callback.  Similarly,
 * ops.close is supposed to be called in the close callback.
 * ops.prepare should be called in the prepare or hw_params callback
 * with the proper parameters for set up.
 * ops.cleanup should be called in hw_free for clean up of streams.
 *
 * This function returns 0 if successfull, or a negative error code.
 */
int __devinit snd_hda_build_pcms(struct hda_bus *bus)
{
	struct hda_codec *codec;

	list_for_each_entry(codec, &bus->codec_list, list) {
		unsigned int pcm, s;
		int err;
		if (!codec->patch_ops.build_pcms)
			continue;
		err = codec->patch_ops.build_pcms(codec);
		if (err < 0)
			return err;
		for (pcm = 0; pcm < codec->num_pcms; pcm++) {
			for (s = 0; s < 2; s++) {
				struct hda_pcm_stream *info;
				info = &codec->pcm_info[pcm].stream[s];
				if (!info->substreams)
					continue;
				err = set_pcm_default_values(codec, info);
				if (err < 0)
					return err;
			}
		}
	}
	return 0;
}

/**
 * snd_hda_check_board_config - compare the current codec with the config table
 * @codec: the HDA codec
 * @num_configs: number of config enums
 * @models: array of model name strings
 * @tbl: configuration table, terminated by null entries
 *
 * Compares the modelname or PCI subsystem id of the current codec with the
 * given configuration table.  If a matching entry is found, returns its
 * config value (supposed to be 0 or positive).
 *
 * If no entries are matching, the function returns a negative value.
 */
int __devinit snd_hda_check_board_config(struct hda_codec *codec,
					 int num_configs, const char **models,
					 const struct snd_pci_quirk *tbl)
{
	if (codec->bus->modelname && models) {
		int i;
		for (i = 0; i < num_configs; i++) {
			if (models[i] &&
			    !strcmp(codec->bus->modelname, models[i])) {
				snd_printd(KERN_INFO "hda_codec: model '%s' is "
					   "selected\n", models[i]);
				return i;
			}
		}
	}

	if (!codec->bus->pci || !tbl)
		return -1;

	tbl = snd_pci_quirk_lookup(codec->bus->pci, tbl);
	if (!tbl)
		return -1;
	if (tbl->value >= 0 && tbl->value < num_configs) {
#ifdef CONFIG_SND_DEBUG_DETECT
		char tmp[10];
		const char *model = NULL;
		if (models)
			model = models[tbl->value];
		if (!model) {
			sprintf(tmp, "#%d", tbl->value);
			model = tmp;
		}
		snd_printdd(KERN_INFO "hda_codec: model '%s' is selected "
			    "for config %x:%x (%s)\n",
			    model, tbl->subvendor, tbl->subdevice,
			    (tbl->name ? tbl->name : "Unknown device"));
#endif
		return tbl->value;
	}
	return -1;
}

/**
 * snd_hda_add_new_ctls - create controls from the array
 * @codec: the HDA codec
 * @knew: the array of struct snd_kcontrol_new
 *
 * This helper function creates and add new controls in the given array.
 * The array must be terminated with an empty entry as terminator.
 *
 * Returns 0 if successful, or a negative error code.
 */
int __devinit snd_hda_add_new_ctls(struct hda_codec *codec,
				   struct snd_kcontrol_new *knew)
{
	int err;

	for (; knew->name; knew++) {
		struct snd_kcontrol *kctl;
		kctl = snd_ctl_new1(knew, codec);
		if (!kctl)
			return -ENOMEM;
		err = snd_ctl_add(codec->bus->card, kctl);
		if (err < 0) {
			if (!codec->addr)
				return err;
			kctl = snd_ctl_new1(knew, codec);
			if (!kctl)
				return -ENOMEM;
			kctl->id.device = codec->addr;
			err = snd_ctl_add(codec->bus->card, kctl);
			if (err < 0)
				return err;
		}
	}
	return 0;
}


/*
 * Channel mode helper
 */
int snd_hda_ch_mode_info(struct hda_codec *codec,
			 struct snd_ctl_elem_info *uinfo,
			 const struct hda_channel_mode *chmode,
			 int num_chmodes)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = num_chmodes;
	if (uinfo->value.enumerated.item >= num_chmodes)
		uinfo->value.enumerated.item = num_chmodes - 1;
	sprintf(uinfo->value.enumerated.name, "%dch",
		chmode[uinfo->value.enumerated.item].channels);
	return 0;
}

int snd_hda_ch_mode_get(struct hda_codec *codec,
			struct snd_ctl_elem_value *ucontrol,
			const struct hda_channel_mode *chmode,
			int num_chmodes,
			int max_channels)
{
	int i;

	for (i = 0; i < num_chmodes; i++) {
		if (max_channels == chmode[i].channels) {
			ucontrol->value.enumerated.item[0] = i;
			break;
		}
	}
	return 0;
}

int snd_hda_ch_mode_put(struct hda_codec *codec,
			struct snd_ctl_elem_value *ucontrol,
			const struct hda_channel_mode *chmode,
			int num_chmodes,
			int *max_channelsp)
{
	unsigned int mode;

	mode = ucontrol->value.enumerated.item[0];
	snd_assert(mode < num_chmodes, return -EINVAL);
	if (*max_channelsp == chmode[mode].channels && !codec->in_resume)
		return 0;
	/* change the current channel setting */
	*max_channelsp = chmode[mode].channels;
	if (chmode[mode].sequence)
		snd_hda_sequence_write(codec, chmode[mode].sequence);
	return 1;
}

/*
 * input MUX helper
 */
int snd_hda_input_mux_info(const struct hda_input_mux *imux,
			   struct snd_ctl_elem_info *uinfo)
{
	unsigned int index;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = imux->num_items;
	index = uinfo->value.enumerated.item;
	if (index >= imux->num_items)
		index = imux->num_items - 1;
	strcpy(uinfo->value.enumerated.name, imux->items[index].label);
	return 0;
}

int snd_hda_input_mux_put(struct hda_codec *codec,
			  const struct hda_input_mux *imux,
			  struct snd_ctl_elem_value *ucontrol,
			  hda_nid_t nid,
			  unsigned int *cur_val)
{
	unsigned int idx;

	idx = ucontrol->value.enumerated.item[0];
	if (idx >= imux->num_items)
		idx = imux->num_items - 1;
	if (*cur_val == idx && !codec->in_resume)
		return 0;
	snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_CONNECT_SEL,
			    imux->items[idx].index);
	*cur_val = idx;
	return 1;
}


/*
 * Multi-channel / digital-out PCM helper functions
 */

/* setup SPDIF output stream */
static void setup_dig_out_stream(struct hda_codec *codec, hda_nid_t nid,
				 unsigned int stream_tag, unsigned int format)
{
	/* turn off SPDIF once; otherwise the IEC958 bits won't be updated */
	if (codec->spdif_ctls & AC_DIG1_ENABLE)
		snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_DIGI_CONVERT_1,
				    codec->spdif_ctls & ~AC_DIG1_ENABLE & 0xff);
	snd_hda_codec_setup_stream(codec, nid, stream_tag, 0, format);
	/* turn on again (if needed) */
	if (codec->spdif_ctls & AC_DIG1_ENABLE)
		snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_DIGI_CONVERT_1,
				    codec->spdif_ctls & 0xff);
}

/*
 * open the digital out in the exclusive mode
 */
int snd_hda_multi_out_dig_open(struct hda_codec *codec,
			       struct hda_multi_out *mout)
{
	mutex_lock(&codec->spdif_mutex);
	if (mout->dig_out_used == HDA_DIG_ANALOG_DUP)
		/* already opened as analog dup; reset it once */
		snd_hda_codec_setup_stream(codec, mout->dig_out_nid, 0, 0, 0);
	mout->dig_out_used = HDA_DIG_EXCLUSIVE;
	mutex_unlock(&codec->spdif_mutex);
	return 0;
}

int snd_hda_multi_out_dig_prepare(struct hda_codec *codec,
				  struct hda_multi_out *mout,
				  unsigned int stream_tag,
				  unsigned int format,
				  struct snd_pcm_substream *substream)
{
	mutex_lock(&codec->spdif_mutex);
	setup_dig_out_stream(codec, mout->dig_out_nid, stream_tag, format);
	mutex_unlock(&codec->spdif_mutex);
	return 0;
}

/*
 * release the digital out
 */
int snd_hda_multi_out_dig_close(struct hda_codec *codec,
				struct hda_multi_out *mout)
{
	mutex_lock(&codec->spdif_mutex);
	mout->dig_out_used = 0;
	mutex_unlock(&codec->spdif_mutex);
	return 0;
}

/*
 * set up more restrictions for analog out
 */
int snd_hda_multi_out_analog_open(struct hda_codec *codec,
				  struct hda_multi_out *mout,
				  struct snd_pcm_substream *substream)
{
	substream->runtime->hw.channels_max = mout->max_channels;
	return snd_pcm_hw_constraint_step(substream->runtime, 0,
					  SNDRV_PCM_HW_PARAM_CHANNELS, 2);
}

/*
 * set up the i/o for analog out
 * when the digital out is available, copy the front out to digital out, too.
 */
int snd_hda_multi_out_analog_prepare(struct hda_codec *codec,
				     struct hda_multi_out *mout,
				     unsigned int stream_tag,
				     unsigned int format,
				     struct snd_pcm_substream *substream)
{
	hda_nid_t *nids = mout->dac_nids;
	int chs = substream->runtime->channels;
	int i;

	mutex_lock(&codec->spdif_mutex);
	if (mout->dig_out_nid && mout->dig_out_used != HDA_DIG_EXCLUSIVE) {
		if (chs == 2 &&
		    snd_hda_is_supported_format(codec, mout->dig_out_nid,
						format) &&
		    !(codec->spdif_status & IEC958_AES0_NONAUDIO)) {
			mout->dig_out_used = HDA_DIG_ANALOG_DUP;
			setup_dig_out_stream(codec, mout->dig_out_nid,
					     stream_tag, format);
		} else {
			mout->dig_out_used = 0;
			snd_hda_codec_setup_stream(codec, mout->dig_out_nid,
						   0, 0, 0);
		}
	}
	mutex_unlock(&codec->spdif_mutex);

	/* front */
	snd_hda_codec_setup_stream(codec, nids[HDA_FRONT], stream_tag,
				   0, format);
	if (mout->hp_nid && mout->hp_nid != nids[HDA_FRONT])
		/* headphone out will just decode front left/right (stereo) */
		snd_hda_codec_setup_stream(codec, mout->hp_nid, stream_tag,
					   0, format);
	/* extra outputs copied from front */
	for (i = 0; i < ARRAY_SIZE(mout->extra_out_nid); i++)
		if (mout->extra_out_nid[i])
			snd_hda_codec_setup_stream(codec,
						   mout->extra_out_nid[i],
						   stream_tag, 0, format);

	/* surrounds */
	for (i = 1; i < mout->num_dacs; i++) {
		if (chs >= (i + 1) * 2) /* independent out */
			snd_hda_codec_setup_stream(codec, nids[i], stream_tag,
						   i * 2, format);
		else /* copy front */
			snd_hda_codec_setup_stream(codec, nids[i], stream_tag,
						   0, format);
	}
	return 0;
}

/*
 * clean up the setting for analog out
 */
int snd_hda_multi_out_analog_cleanup(struct hda_codec *codec,
				     struct hda_multi_out *mout)
{
	hda_nid_t *nids = mout->dac_nids;
	int i;

	for (i = 0; i < mout->num_dacs; i++)
		snd_hda_codec_setup_stream(codec, nids[i], 0, 0, 0);
	if (mout->hp_nid)
		snd_hda_codec_setup_stream(codec, mout->hp_nid, 0, 0, 0);
	for (i = 0; i < ARRAY_SIZE(mout->extra_out_nid); i++)
		if (mout->extra_out_nid[i])
			snd_hda_codec_setup_stream(codec,
						   mout->extra_out_nid[i],
						   0, 0, 0);
	mutex_lock(&codec->spdif_mutex);
	if (mout->dig_out_nid && mout->dig_out_used == HDA_DIG_ANALOG_DUP) {
		snd_hda_codec_setup_stream(codec, mout->dig_out_nid, 0, 0, 0);
		mout->dig_out_used = 0;
	}
	mutex_unlock(&codec->spdif_mutex);
	return 0;
}

/*
 * Helper for automatic ping configuration
 */

static int __devinit is_in_nid_list(hda_nid_t nid, hda_nid_t *list)
{
	for (; *list; list++)
		if (*list == nid)
			return 1;
	return 0;
}


/*
 * Sort an associated group of pins according to their sequence numbers.
 */
static void sort_pins_by_sequence(hda_nid_t * pins, short * sequences,
				  int num_pins)
{
	int i, j;
	short seq;
	hda_nid_t nid;
	
	for (i = 0; i < num_pins; i++) {
		for (j = i + 1; j < num_pins; j++) {
			if (sequences[i] > sequences[j]) {
				seq = sequences[i];
				sequences[i] = sequences[j];
				sequences[j] = seq;
				nid = pins[i];
				pins[i] = pins[j];
				pins[j] = nid;
			}
		}
	}
}


/*
 * Parse all pin widgets and store the useful pin nids to cfg
 *
 * The number of line-outs or any primary output is stored in line_outs,
 * and the corresponding output pins are assigned to line_out_pins[],
 * in the order of front, rear, CLFE, side, ...
 *
 * If more extra outputs (speaker and headphone) are found, the pins are
 * assisnged to hp_pins[] and speaker_pins[], respectively.  If no line-out jack
 * is detected, one of speaker of HP pins is assigned as the primary
 * output, i.e. to line_out_pins[0].  So, line_outs is always positive
 * if any analog output exists.
 * 
 * The analog input pins are assigned to input_pins array.
 * The digital input/output pins are assigned to dig_in_pin and dig_out_pin,
 * respectively.
 */
int __devinit snd_hda_parse_pin_def_config(struct hda_codec *codec,
					   struct auto_pin_cfg *cfg,
					   hda_nid_t *ignore_nids)
{
	hda_nid_t nid, nid_start;
	int nodes;
	short seq, assoc_line_out, assoc_speaker;
	short sequences_line_out[ARRAY_SIZE(cfg->line_out_pins)];
	short sequences_speaker[ARRAY_SIZE(cfg->speaker_pins)];

	memset(cfg, 0, sizeof(*cfg));

	memset(sequences_line_out, 0, sizeof(sequences_line_out));
	memset(sequences_speaker, 0, sizeof(sequences_speaker));
	assoc_line_out = assoc_speaker = 0;

	nodes = snd_hda_get_sub_nodes(codec, codec->afg, &nid_start);
	for (nid = nid_start; nid < nodes + nid_start; nid++) {
		unsigned int wid_caps = get_wcaps(codec, nid);
		unsigned int wid_type =
			(wid_caps & AC_WCAP_TYPE) >> AC_WCAP_TYPE_SHIFT;
		unsigned int def_conf;
		short assoc, loc;

		/* read all default configuration for pin complex */
		if (wid_type != AC_WID_PIN)
			continue;
		/* ignore the given nids (e.g. pc-beep returns error) */
		if (ignore_nids && is_in_nid_list(nid, ignore_nids))
			continue;

		def_conf = snd_hda_codec_read(codec, nid, 0,
					      AC_VERB_GET_CONFIG_DEFAULT, 0);
		if (get_defcfg_connect(def_conf) == AC_JACK_PORT_NONE)
			continue;
		loc = get_defcfg_location(def_conf);
		switch (get_defcfg_device(def_conf)) {
		case AC_JACK_LINE_OUT:
			seq = get_defcfg_sequence(def_conf);
			assoc = get_defcfg_association(def_conf);
			if (!assoc)
				continue;
			if (!assoc_line_out)
				assoc_line_out = assoc;
			else if (assoc_line_out != assoc)
				continue;
			if (cfg->line_outs >= ARRAY_SIZE(cfg->line_out_pins))
				continue;
			cfg->line_out_pins[cfg->line_outs] = nid;
			sequences_line_out[cfg->line_outs] = seq;
			cfg->line_outs++;
			break;
		case AC_JACK_SPEAKER:
			seq = get_defcfg_sequence(def_conf);
			assoc = get_defcfg_association(def_conf);
			if (! assoc)
				continue;
			if (! assoc_speaker)
				assoc_speaker = assoc;
			else if (assoc_speaker != assoc)
				continue;
			if (cfg->speaker_outs >= ARRAY_SIZE(cfg->speaker_pins))
				continue;
			cfg->speaker_pins[cfg->speaker_outs] = nid;
			sequences_speaker[cfg->speaker_outs] = seq;
			cfg->speaker_outs++;
			break;
		case AC_JACK_HP_OUT:
			if (cfg->hp_outs >= ARRAY_SIZE(cfg->hp_pins))
				continue;
			cfg->hp_pins[cfg->hp_outs] = nid;
			cfg->hp_outs++;
			break;
		case AC_JACK_MIC_IN: {
			int preferred, alt;
			if (loc == AC_JACK_LOC_FRONT) {
				preferred = AUTO_PIN_FRONT_MIC;
				alt = AUTO_PIN_MIC;
			} else {
				preferred = AUTO_PIN_MIC;
				alt = AUTO_PIN_FRONT_MIC;
			}
			if (!cfg->input_pins[preferred])
				cfg->input_pins[preferred] = nid;
			else if (!cfg->input_pins[alt])
				cfg->input_pins[alt] = nid;
			break;
		}
		case AC_JACK_LINE_IN:
			if (loc == AC_JACK_LOC_FRONT)
				cfg->input_pins[AUTO_PIN_FRONT_LINE] = nid;
			else
				cfg->input_pins[AUTO_PIN_LINE] = nid;
			break;
		case AC_JACK_CD:
			cfg->input_pins[AUTO_PIN_CD] = nid;
			break;
		case AC_JACK_AUX:
			cfg->input_pins[AUTO_PIN_AUX] = nid;
			break;
		case AC_JACK_SPDIF_OUT:
			cfg->dig_out_pin = nid;
			break;
		case AC_JACK_SPDIF_IN:
			cfg->dig_in_pin = nid;
			break;
		}
	}

	/* sort by sequence */
	sort_pins_by_sequence(cfg->line_out_pins, sequences_line_out,
			      cfg->line_outs);
	sort_pins_by_sequence(cfg->speaker_pins, sequences_speaker,
			      cfg->speaker_outs);
	
	/*
	 * FIX-UP: if no line-outs are detected, try to use speaker or HP pin
	 * as a primary output
	 */
	if (!cfg->line_outs) {
		if (cfg->speaker_outs) {
			cfg->line_outs = cfg->speaker_outs;
			memcpy(cfg->line_out_pins, cfg->speaker_pins,
			       sizeof(cfg->speaker_pins));
			cfg->speaker_outs = 0;
			memset(cfg->speaker_pins, 0, sizeof(cfg->speaker_pins));
			cfg->line_out_type = AUTO_PIN_SPEAKER_OUT;
		} else if (cfg->hp_outs) {
			cfg->line_outs = cfg->hp_outs;
			memcpy(cfg->line_out_pins, cfg->hp_pins,
			       sizeof(cfg->hp_pins));
			cfg->hp_outs = 0;
			memset(cfg->hp_pins, 0, sizeof(cfg->hp_pins));
			cfg->line_out_type = AUTO_PIN_HP_OUT;
		}
	}

	/* Reorder the surround channels
	 * ALSA sequence is front/surr/clfe/side
	 * HDA sequence is:
	 *    4-ch: front/surr  =>  OK as it is
	 *    6-ch: front/clfe/surr
	 *    8-ch: front/clfe/rear/side|fc
	 */
	switch (cfg->line_outs) {
	case 3:
	case 4:
		nid = cfg->line_out_pins[1];
		cfg->line_out_pins[1] = cfg->line_out_pins[2];
		cfg->line_out_pins[2] = nid;
		break;
	}

	/*
	 * debug prints of the parsed results
	 */
	snd_printd("autoconfig: line_outs=%d (0x%x/0x%x/0x%x/0x%x/0x%x)\n",
		   cfg->line_outs, cfg->line_out_pins[0], cfg->line_out_pins[1],
		   cfg->line_out_pins[2], cfg->line_out_pins[3],
		   cfg->line_out_pins[4]);
	snd_printd("   speaker_outs=%d (0x%x/0x%x/0x%x/0x%x/0x%x)\n",
		   cfg->speaker_outs, cfg->speaker_pins[0],
		   cfg->speaker_pins[1], cfg->speaker_pins[2],
		   cfg->speaker_pins[3], cfg->speaker_pins[4]);
	snd_printd("   hp_outs=%d (0x%x/0x%x/0x%x/0x%x/0x%x)\n",
		   cfg->hp_outs, cfg->hp_pins[0],
		   cfg->hp_pins[1], cfg->hp_pins[2],
		   cfg->hp_pins[3], cfg->hp_pins[4]);
	snd_printd("   inputs: mic=0x%x, fmic=0x%x, line=0x%x, fline=0x%x,"
		   " cd=0x%x, aux=0x%x\n",
		   cfg->input_pins[AUTO_PIN_MIC],
		   cfg->input_pins[AUTO_PIN_FRONT_MIC],
		   cfg->input_pins[AUTO_PIN_LINE],
		   cfg->input_pins[AUTO_PIN_FRONT_LINE],
		   cfg->input_pins[AUTO_PIN_CD],
		   cfg->input_pins[AUTO_PIN_AUX]);

	return 0;
}

/* labels for input pins */
const char *auto_pin_cfg_labels[AUTO_PIN_LAST] = {
	"Mic", "Front Mic", "Line", "Front Line", "CD", "Aux"
};


#ifdef CONFIG_PM
/*
 * power management
 */

/**
 * snd_hda_suspend - suspend the codecs
 * @bus: the HDA bus
 * @state: suspsend state
 *
 * Returns 0 if successful.
 */
int snd_hda_suspend(struct hda_bus *bus, pm_message_t state)
{
	struct hda_codec *codec;

	/* FIXME: should handle power widget capabilities */
	list_for_each_entry(codec, &bus->codec_list, list) {
		if (codec->patch_ops.suspend)
			codec->patch_ops.suspend(codec, state);
		hda_set_power_state(codec,
				    codec->afg ? codec->afg : codec->mfg,
				    AC_PWRST_D3);
	}
	return 0;
}

/**
 * snd_hda_resume - resume the codecs
 * @bus: the HDA bus
 * @state: resume state
 *
 * Returns 0 if successful.
 */
int snd_hda_resume(struct hda_bus *bus)
{
	struct hda_codec *codec;

	list_for_each_entry(codec, &bus->codec_list, list) {
		hda_set_power_state(codec,
				    codec->afg ? codec->afg : codec->mfg,
				    AC_PWRST_D0);
		if (codec->patch_ops.resume)
			codec->patch_ops.resume(codec);
	}
	return 0;
}

/**
 * snd_hda_resume_ctls - resume controls in the new control list
 * @codec: the HDA codec
 * @knew: the array of struct snd_kcontrol_new
 *
 * This function resumes the mixer controls in the struct snd_kcontrol_new array,
 * originally for snd_hda_add_new_ctls().
 * The array must be terminated with an empty entry as terminator.
 */
int snd_hda_resume_ctls(struct hda_codec *codec, struct snd_kcontrol_new *knew)
{
	struct snd_ctl_elem_value *val;

	val = kmalloc(sizeof(*val), GFP_KERNEL);
	if (!val)
		return -ENOMEM;
	codec->in_resume = 1;
	for (; knew->name; knew++) {
		int i, count;
		count = knew->count ? knew->count : 1;
		for (i = 0; i < count; i++) {
			memset(val, 0, sizeof(*val));
			val->id.iface = knew->iface;
			val->id.device = knew->device;
			val->id.subdevice = knew->subdevice;
			strcpy(val->id.name, knew->name);
			val->id.index = knew->index ? knew->index : i;
			/* Assume that get callback reads only from cache,
			 * not accessing to the real hardware
			 */
			if (snd_ctl_elem_read(codec->bus->card, val) < 0)
				continue;
			snd_ctl_elem_write(codec->bus->card, NULL, val);
		}
	}
	codec->in_resume = 0;
	kfree(val);
	return 0;
}

/**
 * snd_hda_resume_spdif_out - resume the digital out
 * @codec: the HDA codec
 */
int snd_hda_resume_spdif_out(struct hda_codec *codec)
{
	return snd_hda_resume_ctls(codec, dig_mixes);
}

/**
 * snd_hda_resume_spdif_in - resume the digital in
 * @codec: the HDA codec
 */
int snd_hda_resume_spdif_in(struct hda_codec *codec)
{
	return snd_hda_resume_ctls(codec, dig_in_ctls);
}
#endif
