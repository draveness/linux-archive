/*
 *  PCM Plug-In shared (kernel/library) code
 *  Copyright (c) 1999 by Jaroslav Kysela <perex@suse.cz>
 *  Copyright (c) 2000 by Abramo Bagnara <abramo@alsa-project.org>
 *
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Library General Public License as
 *   published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Library General Public License for more details.
 *
 *   You should have received a copy of the GNU Library General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */
  
#if 0
#define PLUGIN_DEBUG
#endif

#include <sound/driver.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/vmalloc.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include "pcm_plugin.h"

#define snd_pcm_plug_first(plug) ((plug)->runtime->oss.plugin_first)
#define snd_pcm_plug_last(plug) ((plug)->runtime->oss.plugin_last)

static int snd_pcm_plugin_src_channels_mask(snd_pcm_plugin_t *plugin,
					    bitset_t *dst_vmask,
					    bitset_t **src_vmask)
{
	bitset_t *vmask = plugin->src_vmask;
	bitset_copy(vmask, dst_vmask, plugin->src_format.channels);
	*src_vmask = vmask;
	return 0;
}

static int snd_pcm_plugin_dst_channels_mask(snd_pcm_plugin_t *plugin,
					    bitset_t *src_vmask,
					    bitset_t **dst_vmask)
{
	bitset_t *vmask = plugin->dst_vmask;
	bitset_copy(vmask, src_vmask, plugin->dst_format.channels);
	*dst_vmask = vmask;
	return 0;
}

/*
 *  because some cards might have rates "very close", we ignore
 *  all "resampling" requests within +-5%
 */
static int rate_match(unsigned int src_rate, unsigned int dst_rate)
{
	unsigned int low = (src_rate * 95) / 100;
	unsigned int high = (src_rate * 105) / 100;
	return dst_rate >= low && dst_rate <= high;
}

static int snd_pcm_plugin_alloc(snd_pcm_plugin_t *plugin, snd_pcm_uframes_t frames)
{
	snd_pcm_plugin_format_t *format;
	ssize_t width;
	size_t size;
	unsigned int channel;
	snd_pcm_plugin_channel_t *c;

	if (plugin->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		format = &plugin->src_format;
	} else {
		format = &plugin->dst_format;
	}
	if ((width = snd_pcm_format_physical_width(format->format)) < 0)
		return width;
	size = frames * format->channels * width;
	snd_assert((size % 8) == 0, return -ENXIO);
	size /= 8;
	if (plugin->buf_frames < frames) {
		if (plugin->buf)
			vfree(plugin->buf);
		plugin->buf = vmalloc(size);
		plugin->buf_frames = frames;
	}
	if (!plugin->buf) {
		plugin->buf_frames = 0;
		return -ENOMEM;
	}
	c = plugin->buf_channels;
	if (plugin->access == SNDRV_PCM_ACCESS_RW_INTERLEAVED) {
		for (channel = 0; channel < format->channels; channel++, c++) {
			c->frames = frames;
			c->enabled = 1;
			c->wanted = 0;
			c->area.addr = plugin->buf;
			c->area.first = channel * width;
			c->area.step = format->channels * width;
		}
	} else if (plugin->access == SNDRV_PCM_ACCESS_RW_NONINTERLEAVED) {
		snd_assert((size % format->channels) == 0,);
		size /= format->channels;
		for (channel = 0; channel < format->channels; channel++, c++) {
			c->frames = frames;
			c->enabled = 1;
			c->wanted = 0;
			c->area.addr = plugin->buf + (channel * size);
			c->area.first = 0;
			c->area.step = width;
		}
	} else
		return -EINVAL;
	return 0;
}

int snd_pcm_plug_alloc(snd_pcm_plug_t *plug, snd_pcm_uframes_t frames)
{
	int err;
	snd_assert(snd_pcm_plug_first(plug) != NULL, return -ENXIO);
	if (snd_pcm_plug_stream(plug) == SNDRV_PCM_STREAM_PLAYBACK) {
		snd_pcm_plugin_t *plugin = snd_pcm_plug_first(plug);
		while (plugin->next) {
			if (plugin->dst_frames)
				frames = plugin->dst_frames(plugin, frames);
			snd_assert(frames > 0, return -ENXIO);
			plugin = plugin->next;
			err = snd_pcm_plugin_alloc(plugin, frames);
			if (err < 0)
				return err;
		}
	} else {
		snd_pcm_plugin_t *plugin = snd_pcm_plug_last(plug);
		while (plugin->prev) {
			if (plugin->src_frames)
				frames = plugin->src_frames(plugin, frames);
			snd_assert(frames > 0, return -ENXIO);
			plugin = plugin->prev;
			err = snd_pcm_plugin_alloc(plugin, frames);
			if (err < 0)
				return err;
		}
	}
	return 0;
}


snd_pcm_sframes_t snd_pcm_plugin_client_channels(snd_pcm_plugin_t *plugin,
				       snd_pcm_uframes_t frames,
				       snd_pcm_plugin_channel_t **channels)
{
	*channels = plugin->buf_channels;
	return frames;
}

int snd_pcm_plugin_build(snd_pcm_plug_t *plug,
			 const char *name,
			 snd_pcm_plugin_format_t *src_format,
			 snd_pcm_plugin_format_t *dst_format,
			 size_t extra,
			 snd_pcm_plugin_t **ret)
{
	snd_pcm_plugin_t *plugin;
	unsigned int channels;
	
	snd_assert(plug != NULL, return -ENXIO);
	snd_assert(src_format != NULL && dst_format != NULL, return -ENXIO);
	plugin = (snd_pcm_plugin_t *)snd_kcalloc(sizeof(*plugin) + extra, GFP_KERNEL);
	if (plugin == NULL)
		return -ENOMEM;
	plugin->name = name;
	plugin->plug = plug;
	plugin->stream = snd_pcm_plug_stream(plug);
	plugin->access = SNDRV_PCM_ACCESS_RW_INTERLEAVED;
	plugin->src_format = *src_format;
	plugin->src_width = snd_pcm_format_physical_width(src_format->format);
	snd_assert(plugin->src_width > 0, );
	plugin->dst_format = *dst_format;
	plugin->dst_width = snd_pcm_format_physical_width(dst_format->format);
	snd_assert(plugin->dst_width > 0, );
	if (plugin->stream == SNDRV_PCM_STREAM_PLAYBACK)
		channels = src_format->channels;
	else
		channels = dst_format->channels;
	plugin->buf_channels = snd_kcalloc(channels * sizeof(*plugin->buf_channels), GFP_KERNEL);
	if (plugin->buf_channels == NULL) {
		snd_pcm_plugin_free(plugin);
		return -ENOMEM;
	}
	plugin->src_vmask = bitset_alloc(src_format->channels);
	if (plugin->src_vmask == NULL) {
		snd_pcm_plugin_free(plugin);
		return -ENOMEM;
	}
	plugin->dst_vmask = bitset_alloc(dst_format->channels);
	if (plugin->dst_vmask == NULL) {
		snd_pcm_plugin_free(plugin);
		return -ENOMEM;
	}
	plugin->client_channels = snd_pcm_plugin_client_channels;
	plugin->src_channels_mask = snd_pcm_plugin_src_channels_mask;
	plugin->dst_channels_mask = snd_pcm_plugin_dst_channels_mask;
	*ret = plugin;
	return 0;
}

int snd_pcm_plugin_free(snd_pcm_plugin_t *plugin)
{
	if (! plugin)
		return 0;
	if (plugin->private_free)
		plugin->private_free(plugin);
	if (plugin->buf_channels)
		kfree(plugin->buf_channels);
	if (plugin->buf)
		vfree(plugin->buf);
	if (plugin->src_vmask)
		kfree(plugin->src_vmask);
	if (plugin->dst_vmask)
		kfree(plugin->dst_vmask);
	kfree(plugin);
	return 0;
}

snd_pcm_sframes_t snd_pcm_plug_client_size(snd_pcm_plug_t *plug, snd_pcm_uframes_t drv_frames)
{
	snd_pcm_plugin_t *plugin, *plugin_prev, *plugin_next;
	int stream = snd_pcm_plug_stream(plug);

	snd_assert(plug != NULL, return -ENXIO);
	if (drv_frames == 0)
		return 0;
	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		plugin = snd_pcm_plug_last(plug);
		while (plugin && drv_frames > 0) {
			plugin_prev = plugin->prev;
			if (plugin->src_frames)
				drv_frames = plugin->src_frames(plugin, drv_frames);
			plugin = plugin_prev;
		}
	} else if (stream == SNDRV_PCM_STREAM_CAPTURE) {
		plugin = snd_pcm_plug_first(plug);
		while (plugin && drv_frames > 0) {
			plugin_next = plugin->next;
			if (plugin->dst_frames)
				drv_frames = plugin->dst_frames(plugin, drv_frames);
			plugin = plugin_next;
		}
	} else
		snd_BUG();
	return drv_frames;
}

snd_pcm_sframes_t snd_pcm_plug_slave_size(snd_pcm_plug_t *plug, snd_pcm_uframes_t clt_frames)
{
	snd_pcm_plugin_t *plugin, *plugin_prev, *plugin_next;
	snd_pcm_sframes_t frames;
	int stream = snd_pcm_plug_stream(plug);
	
	snd_assert(plug != NULL, return -ENXIO);
	if (clt_frames == 0)
		return 0;
	frames = clt_frames;
	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		plugin = snd_pcm_plug_first(plug);
		while (plugin && frames > 0) {
			plugin_next = plugin->next;
			if (plugin->dst_frames) {
				frames = plugin->dst_frames(plugin, frames);
				if (frames < 0)
					return frames;
			}
			plugin = plugin_next;
		}
	} else if (stream == SNDRV_PCM_STREAM_CAPTURE) {
		plugin = snd_pcm_plug_last(plug);
		while (plugin) {
			plugin_prev = plugin->prev;
			if (plugin->src_frames) {
				frames = plugin->src_frames(plugin, frames);
				if (frames < 0)
					return frames;
			}
			plugin = plugin_prev;
		}
	} else
		snd_BUG();
	return frames;
}

static int snd_pcm_plug_formats(snd_mask_t *mask, int format)
{
	snd_mask_t formats = *mask;
	u64 linfmts = (SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S8 |
		       SNDRV_PCM_FMTBIT_U16_LE | SNDRV_PCM_FMTBIT_S16_LE |
		       SNDRV_PCM_FMTBIT_U16_BE | SNDRV_PCM_FMTBIT_S16_BE |
		       SNDRV_PCM_FMTBIT_U24_LE | SNDRV_PCM_FMTBIT_S24_LE |
		       SNDRV_PCM_FMTBIT_U24_BE | SNDRV_PCM_FMTBIT_S24_BE |
		       SNDRV_PCM_FMTBIT_U32_LE | SNDRV_PCM_FMTBIT_S32_LE |
		       SNDRV_PCM_FMTBIT_U32_BE | SNDRV_PCM_FMTBIT_S32_BE);
	snd_mask_set(&formats, SNDRV_PCM_FORMAT_MU_LAW);
	
	if (formats.bits[0] & (u32)linfmts)
		formats.bits[0] |= (u32)linfmts;
	if (formats.bits[1] & (u32)(linfmts >> 32))
		formats.bits[1] |= (u32)(linfmts >> 32);
	return snd_mask_test(&formats, format);
}

static int preferred_formats[] = {
	SNDRV_PCM_FORMAT_S16_LE,
	SNDRV_PCM_FORMAT_S16_BE,
	SNDRV_PCM_FORMAT_U16_LE,
	SNDRV_PCM_FORMAT_U16_BE,
	SNDRV_PCM_FORMAT_S24_LE,
	SNDRV_PCM_FORMAT_S24_BE,
	SNDRV_PCM_FORMAT_U24_LE,
	SNDRV_PCM_FORMAT_U24_BE,
	SNDRV_PCM_FORMAT_S32_LE,
	SNDRV_PCM_FORMAT_S32_BE,
	SNDRV_PCM_FORMAT_U32_LE,
	SNDRV_PCM_FORMAT_U32_BE,
	SNDRV_PCM_FORMAT_S8,
	SNDRV_PCM_FORMAT_U8
};

int snd_pcm_plug_slave_format(int format, snd_mask_t *format_mask)
{
	if (snd_mask_test(format_mask, format))
		return format;
	if (! snd_pcm_plug_formats(format_mask, format))
		return -EINVAL;
	if (snd_pcm_format_linear(format)) {
		int width = snd_pcm_format_width(format);
		int unsignd = snd_pcm_format_unsigned(format);
		int big = snd_pcm_format_big_endian(format);
		int format1;
		int wid, width1=width;
		int dwidth1 = 8;
		for (wid = 0; wid < 4; ++wid) {
			int end, big1 = big;
			for (end = 0; end < 2; ++end) {
				int sgn, unsignd1 = unsignd;
				for (sgn = 0; sgn < 2; ++sgn) {
					format1 = snd_pcm_build_linear_format(width1, unsignd1, big1);
					if (format1 >= 0 &&
					    snd_mask_test(format_mask, format1))
						goto _found;
					unsignd1 = !unsignd1;
				}
				big1 = !big1;
			}
			if (width1 == 32) {
				dwidth1 = -dwidth1;
				width1 = width;
			}
			width1 += dwidth1;
		}
		return -EINVAL;
	_found:
		return format1;
	} else {
		unsigned int i;
		switch (format) {
		case SNDRV_PCM_FORMAT_MU_LAW:
			for (i = 0; i < sizeof(preferred_formats) / sizeof(preferred_formats[0]); ++i) {
				int format1 = preferred_formats[i];
				if (snd_mask_test(format_mask, format1))
					return format1;
			}
		default:
			return -EINVAL;
		}
	}
}

int snd_pcm_plug_format_plugins(snd_pcm_plug_t *plug,
				snd_pcm_hw_params_t *params,
				snd_pcm_hw_params_t *slave_params)
{
	snd_pcm_plugin_format_t tmpformat;
	snd_pcm_plugin_format_t dstformat;
	snd_pcm_plugin_format_t srcformat;
	int src_access, dst_access;
	snd_pcm_plugin_t *plugin = NULL;
	int err, first;
	int stream = snd_pcm_plug_stream(plug);
	int slave_interleaved = (params_channels(slave_params) == 1 ||
				 params_access(slave_params) == SNDRV_PCM_ACCESS_RW_INTERLEAVED);

	switch (stream) {
	case SNDRV_PCM_STREAM_PLAYBACK:
		dstformat.format = params_format(slave_params);
		dstformat.rate = params_rate(slave_params);
		dstformat.channels = params_channels(slave_params);
		srcformat.format = params_format(params);
		srcformat.rate = params_rate(params);
		srcformat.channels = params_channels(params);
		src_access = SNDRV_PCM_ACCESS_RW_INTERLEAVED;
		dst_access = (slave_interleaved ? SNDRV_PCM_ACCESS_RW_INTERLEAVED :
						  SNDRV_PCM_ACCESS_RW_NONINTERLEAVED);
		break;
	case SNDRV_PCM_STREAM_CAPTURE:
		dstformat.format = params_format(params);
		dstformat.rate = params_rate(params);
		dstformat.channels = params_channels(params);
		srcformat.format = params_format(slave_params);
		srcformat.rate = params_rate(slave_params);
		srcformat.channels = params_channels(slave_params);
		src_access = (slave_interleaved ? SNDRV_PCM_ACCESS_RW_INTERLEAVED :
						  SNDRV_PCM_ACCESS_RW_NONINTERLEAVED);
		dst_access = SNDRV_PCM_ACCESS_RW_INTERLEAVED;
		break;
	default:
		snd_BUG();
		return -EINVAL;
	}
	tmpformat = srcformat;
		
	pdprintf("srcformat: format=%i, rate=%i, channels=%i\n", 
		 srcformat.format,
		 srcformat.rate,
		 srcformat.channels);
	pdprintf("dstformat: format=%i, rate=%i, channels=%i\n", 
		 dstformat.format,
		 dstformat.rate,
		 dstformat.channels);

	/* Format change (linearization) */
	if ((srcformat.format != dstformat.format ||
	     !rate_match(srcformat.rate, dstformat.rate) ||
	     srcformat.channels != dstformat.channels) &&
	    !snd_pcm_format_linear(srcformat.format)) {
		if (snd_pcm_format_linear(dstformat.format))
			tmpformat.format = dstformat.format;
		else
			tmpformat.format = SNDRV_PCM_FORMAT_S16;
		first = plugin == NULL;
		switch (srcformat.format) {
		case SNDRV_PCM_FORMAT_MU_LAW:
			err = snd_pcm_plugin_build_mulaw(plug,
							 &srcformat, &tmpformat,
							 &plugin);
			break;
		default:
			return -EINVAL;
		}
		pdprintf("format change: src=%i, dst=%i returns %i\n", srcformat.format, tmpformat.format, err);
		if (err < 0)
			return err;
		err = snd_pcm_plugin_append(plugin);
		if (err < 0) {
			snd_pcm_plugin_free(plugin);
			return err;
		}
		srcformat = tmpformat;
		src_access = dst_access;
	}

	/* channels reduction */
	if (srcformat.channels > dstformat.channels) {
		int sv = srcformat.channels;
		int dv = dstformat.channels;
		route_ttable_entry_t *ttable = snd_kcalloc(dv*sv*sizeof(*ttable), GFP_KERNEL);
		if (ttable == NULL)
			return -ENOMEM;
#if 1
		if (sv == 2 && dv == 1) {
			ttable[0] = HALF;
			ttable[1] = HALF;
		} else
#endif
		{
			int v;
			for (v = 0; v < dv; ++v)
				ttable[v * sv + v] = FULL;
		}
		tmpformat.channels = dstformat.channels;
		if (rate_match(srcformat.rate, dstformat.rate) &&
		    snd_pcm_format_linear(dstformat.format))
			tmpformat.format = dstformat.format;
		err = snd_pcm_plugin_build_route(plug,
						 &srcformat, &tmpformat,
						 ttable, &plugin);
		kfree(ttable);
		pdprintf("channels reduction: src=%i, dst=%i returns %i\n", srcformat.channels, tmpformat.channels, err);
		if (err < 0) {
			snd_pcm_plugin_free(plugin);
			return err;
		}
		err = snd_pcm_plugin_append(plugin);
		if (err < 0) {
			snd_pcm_plugin_free(plugin);
			return err;
		}
		srcformat = tmpformat;
		src_access = dst_access;
	}

	/* rate resampling */
	if (!rate_match(srcformat.rate, dstformat.rate)) {
		tmpformat.rate = dstformat.rate;
		if (srcformat.channels == dstformat.channels &&
		    snd_pcm_format_linear(dstformat.format))
			tmpformat.format = dstformat.format;
        	err = snd_pcm_plugin_build_rate(plug,
        					&srcformat, &tmpformat,
						&plugin);
		pdprintf("rate down resampling: src=%i, dst=%i returns %i\n", srcformat.rate, tmpformat.rate, err);
		if (err < 0) {
			snd_pcm_plugin_free(plugin);
			return err;
		}      					    
		err = snd_pcm_plugin_append(plugin);
		if (err < 0) {
			snd_pcm_plugin_free(plugin);
			return err;
		}
		srcformat = tmpformat;
		src_access = dst_access;
        }

	/* channels extension  */
	if (srcformat.channels < dstformat.channels) {
		int sv = srcformat.channels;
		int dv = dstformat.channels;
		route_ttable_entry_t *ttable = snd_kcalloc(dv * sv * sizeof(*ttable), GFP_KERNEL);
		if (ttable == NULL)
			return -ENOMEM;
#if 0
		{
			int v;
			for (v = 0; v < sv; ++v)
				ttable[v * sv + v] = FULL;
		}
#else
		{
			/* Playback is spreaded on all channels */
			int vd, vs;
			for (vd = 0, vs = 0; vd < dv; ++vd) {
				ttable[vd * sv + vs] = FULL;
				vs++;
				if (vs == sv)
					vs = 0;
			}
		}
#endif
		tmpformat.channels = dstformat.channels;
		if (snd_pcm_format_linear(dstformat.format))
			tmpformat.format = dstformat.format;
		err = snd_pcm_plugin_build_route(plug,
						 &srcformat, &tmpformat,
						 ttable, &plugin);
		kfree(ttable);
		pdprintf("channels extension: src=%i, dst=%i returns %i\n", srcformat.channels, tmpformat.channels, err);
		if (err < 0) {
			snd_pcm_plugin_free(plugin);
			return err;
		}      					    
		err = snd_pcm_plugin_append(plugin);
		if (err < 0) {
			snd_pcm_plugin_free(plugin);
			return err;
		}
		srcformat = tmpformat;
		src_access = dst_access;
	}

	/* format change */
	if (srcformat.format != dstformat.format) {
		tmpformat.format = dstformat.format;
		if (tmpformat.format == SNDRV_PCM_FORMAT_MU_LAW) {
			err = snd_pcm_plugin_build_mulaw(plug,
							 &srcformat, &tmpformat,
							 &plugin);
		}
		else if (snd_pcm_format_linear(srcformat.format) &&
			 snd_pcm_format_linear(tmpformat.format)) {
			err = snd_pcm_plugin_build_linear(plug,
							  &srcformat, &tmpformat,
							  &plugin);
		}
		else
			return -EINVAL;
		pdprintf("format change: src=%i, dst=%i returns %i\n", srcformat.format, tmpformat.format, err);
		if (err < 0)
			return err;
		err = snd_pcm_plugin_append(plugin);
		if (err < 0) {
			snd_pcm_plugin_free(plugin);
			return err;
		}
		srcformat = tmpformat;
		src_access = dst_access;
	}

	/* de-interleave */
	if (src_access != dst_access) {
		err = snd_pcm_plugin_build_copy(plug,
						&srcformat,
						&tmpformat,
						&plugin);
		pdprintf("interleave change (copy: returns %i)\n", err);
		if (err < 0)
			return err;
		err = snd_pcm_plugin_append(plugin);
		if (err < 0) {
			snd_pcm_plugin_free(plugin);
			return err;
		}
	}

	return 0;
}

snd_pcm_sframes_t snd_pcm_plug_client_channels_buf(snd_pcm_plug_t *plug,
					 char *buf,
					 snd_pcm_uframes_t count,
					 snd_pcm_plugin_channel_t **channels)
{
	snd_pcm_plugin_t *plugin;
	snd_pcm_plugin_channel_t *v;
	snd_pcm_plugin_format_t *format;
	int width, nchannels, channel;
	int stream = snd_pcm_plug_stream(plug);

	snd_assert(buf != NULL, return -ENXIO);
	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		plugin = snd_pcm_plug_first(plug);
		format = &plugin->src_format;
	} else {
		plugin = snd_pcm_plug_last(plug);
		format = &plugin->dst_format;
	}
	v = plugin->buf_channels;
	*channels = v;
	if ((width = snd_pcm_format_physical_width(format->format)) < 0)
		return width;
	nchannels = format->channels;
	snd_assert(plugin->access == SNDRV_PCM_ACCESS_RW_INTERLEAVED || format->channels <= 1, return -ENXIO);
	for (channel = 0; channel < nchannels; channel++, v++) {
		v->frames = count;
		v->enabled = 1;
		v->wanted = (stream == SNDRV_PCM_STREAM_CAPTURE);
		v->area.addr = buf;
		v->area.first = channel * width;
		v->area.step = nchannels * width;
	}
	return count;
}

int snd_pcm_plug_playback_channels_mask(snd_pcm_plug_t *plug,
					bitset_t *client_vmask)
{
	snd_pcm_plugin_t *plugin = snd_pcm_plug_last(plug);
	if (plugin == NULL) {
		return 0;
	} else {
		int schannels = plugin->dst_format.channels;
		bitset_t bs[bitset_size(schannels)];
		bitset_t *srcmask;
		bitset_t *dstmask = bs;
		int err;
		bitset_one(dstmask, schannels);
		if (plugin == NULL) {
			bitset_and(client_vmask, dstmask, schannels);
			return 0;
		}
		while (1) {
			err = plugin->src_channels_mask(plugin, dstmask, &srcmask);
			if (err < 0)
				return err;
			dstmask = srcmask;
			if (plugin->prev == NULL)
				break;
			plugin = plugin->prev;
		}
		bitset_and(client_vmask, dstmask, plugin->src_format.channels);
		return 0;
	}
}

int snd_pcm_plug_capture_channels_mask(snd_pcm_plug_t *plug,
				       bitset_t *client_vmask)
{
	snd_pcm_plugin_t *plugin = snd_pcm_plug_first(plug);
	if (plugin == NULL) {
		return 0;
	} else {
		int schannels = plugin->src_format.channels;
		bitset_t bs[bitset_size(schannels)];
		bitset_t *srcmask = bs;
		bitset_t *dstmask;
		int err;
		bitset_one(srcmask, schannels);
		while (1) {
			err = plugin->dst_channels_mask(plugin, srcmask, &dstmask);
			if (err < 0)
				return err;
			srcmask = dstmask;
			if (plugin->next == NULL)
				break;
			plugin = plugin->next;
		}
		bitset_and(client_vmask, srcmask, plugin->dst_format.channels);
		return 0;
	}
}

static int snd_pcm_plug_playback_disable_useless_channels(snd_pcm_plug_t *plug,
							  snd_pcm_plugin_channel_t *src_channels)
{
	snd_pcm_plugin_t *plugin = snd_pcm_plug_first(plug);
	unsigned int nchannels = plugin->src_format.channels;
	bitset_t bs[bitset_size(nchannels)];
	bitset_t *srcmask = bs;
	int err;
	unsigned int channel;
	for (channel = 0; channel < nchannels; channel++) {
		if (src_channels[channel].enabled)
			bitset_set(srcmask, channel);
		else
			bitset_reset(srcmask, channel);
	}
	err = snd_pcm_plug_playback_channels_mask(plug, srcmask);
	if (err < 0)
		return err;
	for (channel = 0; channel < nchannels; channel++) {
		if (!bitset_get(srcmask, channel))
			src_channels[channel].enabled = 0;
	}
	return 0;
}

static int snd_pcm_plug_capture_disable_useless_channels(snd_pcm_plug_t *plug,
							 snd_pcm_plugin_channel_t *src_channels,
							 snd_pcm_plugin_channel_t *client_channels)
{
	snd_pcm_plugin_t *plugin = snd_pcm_plug_last(plug);
	unsigned int nchannels = plugin->dst_format.channels;
	bitset_t bs[bitset_size(nchannels)];
	bitset_t *dstmask = bs;
	bitset_t *srcmask;
	int err;
	unsigned int channel;
	for (channel = 0; channel < nchannels; channel++) {
		if (client_channels[channel].enabled)
			bitset_set(dstmask, channel);
		else
			bitset_reset(dstmask, channel);
	}
	while (plugin) {
		err = plugin->src_channels_mask(plugin, dstmask, &srcmask);
		if (err < 0)
			return err;
		dstmask = srcmask;
		plugin = plugin->prev;
	}
	plugin = snd_pcm_plug_first(plug);
	nchannels = plugin->src_format.channels;
	for (channel = 0; channel < nchannels; channel++) {
		if (!bitset_get(dstmask, channel))
			src_channels[channel].enabled = 0;
	}
	return 0;
}

snd_pcm_sframes_t snd_pcm_plug_write_transfer(snd_pcm_plug_t *plug, snd_pcm_plugin_channel_t *src_channels, snd_pcm_uframes_t size)
{
	snd_pcm_plugin_t *plugin, *next;
	snd_pcm_plugin_channel_t *dst_channels;
	int err;
	snd_pcm_sframes_t frames = size;

	if ((err = snd_pcm_plug_playback_disable_useless_channels(plug, src_channels)) < 0)
		return err;
	
	plugin = snd_pcm_plug_first(plug);
	while (plugin && frames > 0) {
		if ((next = plugin->next) != NULL) {
			snd_pcm_sframes_t frames1 = frames;
			if (plugin->dst_frames)
				frames1 = plugin->dst_frames(plugin, frames);
			if ((err = next->client_channels(next, frames1, &dst_channels)) < 0) {
				return err;
			}
			if (err != frames1) {
				frames = err;
				if (plugin->src_frames)
					frames = plugin->src_frames(plugin, frames1);
			}
		} else
			dst_channels = NULL;
		pdprintf("write plugin: %s, %li\n", plugin->name, frames);
		if ((frames = plugin->transfer(plugin, src_channels, dst_channels, frames)) < 0)
			return frames;
		src_channels = dst_channels;
		plugin = next;
	}
	return snd_pcm_plug_client_size(plug, frames);
}

snd_pcm_sframes_t snd_pcm_plug_read_transfer(snd_pcm_plug_t *plug, snd_pcm_plugin_channel_t *dst_channels_final, snd_pcm_uframes_t size)
{
	snd_pcm_plugin_t *plugin, *next;
	snd_pcm_plugin_channel_t *src_channels, *dst_channels;
	snd_pcm_sframes_t frames = size;
	int err;

	frames = snd_pcm_plug_slave_size(plug, frames);
	if (frames < 0)
		return frames;

	src_channels = NULL;
	plugin = snd_pcm_plug_first(plug);
	while (plugin && frames > 0) {
		if ((next = plugin->next) != NULL) {
			if ((err = plugin->client_channels(plugin, frames, &dst_channels)) < 0) {
				return err;
			}
			frames = err;
			if (!plugin->prev) {
				if ((err = snd_pcm_plug_capture_disable_useless_channels(plug, dst_channels, dst_channels_final)) < 0)
					return err;
			}
		} else {
			dst_channels = dst_channels_final;
		}
		pdprintf("read plugin: %s, %li\n", plugin->name, frames);
		if ((frames = plugin->transfer(plugin, src_channels, dst_channels, frames)) < 0)
			return frames;
		plugin = next;
		src_channels = dst_channels;
	}
	return frames;
}

int snd_pcm_area_silence(const snd_pcm_channel_area_t *dst_area, size_t dst_offset,
			 size_t samples, int format)
{
	/* FIXME: sub byte resolution and odd dst_offset */
	char *dst;
	unsigned int dst_step;
	int width;
	u_int64_t silence;
	if (!dst_area->addr)
		return 0;
	dst = dst_area->addr + (dst_area->first + dst_area->step * dst_offset) / 8;
	width = snd_pcm_format_physical_width(format);
	silence = snd_pcm_format_silence_64(format);
	if (dst_area->step == (unsigned int) width) {
		size_t dwords = samples * width / 64;
		u_int64_t *dst64 = (u_int64_t *)dst;

		samples -= dwords * 64 / width;
		while (dwords-- > 0)
			*dst64++ = silence;
		if (samples == 0)
			return 0;
		dst = (char *)dst64;
	}
	dst_step = dst_area->step / 8;
	switch (width) {
	case 4: {
		u_int8_t s0 = silence & 0xf0;
		u_int8_t s1 = silence & 0x0f;
		int dstbit = dst_area->first % 8;
		int dstbit_step = dst_area->step % 8;
		while (samples-- > 0) {
			if (dstbit) {
				*dst &= 0xf0;
				*dst |= s1;
			} else {
				*dst &= 0x0f;
				*dst |= s0;
			}
			dst += dst_step;
			dstbit += dstbit_step;
			if (dstbit == 8) {
				dst++;
				dstbit = 0;
			}
		}
		break;
	}
	case 8: {
		u_int8_t sil = silence;
		while (samples-- > 0) {
			*dst = sil;
			dst += dst_step;
		}
		break;
	}
	case 16: {
		u_int16_t sil = silence;
		while (samples-- > 0) {
			*(u_int16_t*)dst = sil;
			dst += dst_step;
		}
		break;
	}
	case 32: {
		u_int32_t sil = silence;
		while (samples-- > 0) {
			*(u_int32_t*)dst = sil;
			dst += dst_step;
		}
		break;
	}
	case 64: {
		while (samples-- > 0) {
			*(u_int64_t*)dst = silence;
			dst += dst_step;
		}
		break;
	}
	default:
		snd_BUG();
	}
	return 0;
}

int snd_pcm_area_copy(const snd_pcm_channel_area_t *src_area, size_t src_offset,
		      const snd_pcm_channel_area_t *dst_area, size_t dst_offset,
		      size_t samples, int format)
{
	/* FIXME: sub byte resolution and odd dst_offset */
	char *src, *dst;
	int width;
	int src_step, dst_step;
	src = src_area->addr + (src_area->first + src_area->step * src_offset) / 8;
	if (!src_area->addr)
		return snd_pcm_area_silence(dst_area, dst_offset, samples, format);
	dst = dst_area->addr + (dst_area->first + dst_area->step * dst_offset) / 8;
	if (!dst_area->addr)
		return 0;
	width = snd_pcm_format_physical_width(format);
	if (src_area->step == (unsigned int) width &&
	    dst_area->step == (unsigned int) width) {
		size_t bytes = samples * width / 8;
		samples -= bytes * 8 / width;
		memcpy(dst, src, bytes);
		if (samples == 0)
			return 0;
	}
	src_step = src_area->step / 8;
	dst_step = dst_area->step / 8;
	switch (width) {
	case 4: {
		int srcbit = src_area->first % 8;
		int srcbit_step = src_area->step % 8;
		int dstbit = dst_area->first % 8;
		int dstbit_step = dst_area->step % 8;
		while (samples-- > 0) {
			unsigned char srcval;
			if (srcbit)
				srcval = *src & 0x0f;
			else
				srcval = *src & 0xf0;
			if (dstbit)
				*dst &= 0xf0;
			else
				*dst &= 0x0f;
			*dst |= srcval;
			src += src_step;
			srcbit += srcbit_step;
			if (srcbit == 8) {
				src++;
				srcbit = 0;
			}
			dst += dst_step;
			dstbit += dstbit_step;
			if (dstbit == 8) {
				dst++;
				dstbit = 0;
			}
		}
		break;
	}
	case 8: {
		while (samples-- > 0) {
			*dst = *src;
			src += src_step;
			dst += dst_step;
		}
		break;
	}
	case 16: {
		while (samples-- > 0) {
			*(u_int16_t*)dst = *(u_int16_t*)src;
			src += src_step;
			dst += dst_step;
		}
		break;
	}
	case 32: {
		while (samples-- > 0) {
			*(u_int32_t*)dst = *(u_int32_t*)src;
			src += src_step;
			dst += dst_step;
		}
		break;
	}
	case 64: {
		while (samples-- > 0) {
			*(u_int64_t*)dst = *(u_int64_t*)src;
			src += src_step;
			dst += dst_step;
		}
		break;
	}
	default:
		snd_BUG();
	}
	return 0;
}
