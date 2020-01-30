/*
 *  Digital Audio (PCM) abstract layer
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
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
#include <asm/io.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/info.h>
#include <sound/initval.h>

static int preallocate_dma = 1;
module_param(preallocate_dma, int, 0444);
MODULE_PARM_DESC(preallocate_dma, "Preallocate DMA memory when the PCM devices are initialized.");
MODULE_PARM_SYNTAX(preallocate_dma, SNDRV_BOOLEAN_TRUE_DESC);

static int maximum_substreams = 4;
module_param(maximum_substreams, int, 0444);
MODULE_PARM_DESC(maximum_substreams, "Maximum substreams with preallocated DMA memory.");
MODULE_PARM_SYNTAX(maximum_substreams, SNDRV_BOOLEAN_TRUE_DESC);

const static size_t snd_minimum_buffer = 16384;


/*
 * try to allocate as the large pages as possible.
 * stores the resultant memory size in *res_size.
 *
 * the minimum size is snd_minimum_buffer.  it should be power of 2.
 */
static int preallocate_pcm_pages(snd_pcm_substream_t *substream, size_t size)
{
	struct snd_dma_buffer *dmab = &substream->dma_buffer;
	int err;

	snd_assert(size > 0, return -EINVAL);

	/* already reserved? */
	if (snd_dma_get_reserved(&substream->dma_device, dmab) > 0) {
		if (dmab->bytes >= size)
			return 0; /* yes */
		/* no, reset the reserved block */
		/* if we can find bigger pages below, this block will be
		 * automatically removed in snd_dma_set_reserved().
		 */
		snd_dma_free_reserved(&substream->dma_device);
		dmab->bytes = 0;
	}

	do {
		if ((err = snd_dma_alloc_pages(&substream->dma_device, size, dmab)) < 0) {
			if (err != -ENOMEM)
				return err; /* fatal error */
		} else {
			/* remember this one */
			snd_dma_set_reserved(&substream->dma_device, dmab);
			return 0;
		}
		size >>= 1;
	} while (size >= snd_minimum_buffer);
	dmab->bytes = 0; /* tell error */
	return 0;
}

/*
 * release the preallocated buffer if not yet done.
 */
static void snd_pcm_lib_preallocate_dma_free(snd_pcm_substream_t *substream)
{
	if (substream->dma_buffer.area == NULL)
		return;
	snd_dma_free_reserved(&substream->dma_device);
	substream->dma_buffer.area = NULL;
}

/**
 * snd_pcm_lib_preallocate_free - release the preallocated buffer of the specified substream.
 * @substream: the pcm substream instance
 *
 * Releases the pre-allocated buffer of the given substream.
 *
 * Returns zero if successful, or a negative error code on failure.
 */
int snd_pcm_lib_preallocate_free(snd_pcm_substream_t *substream)
{
	snd_pcm_lib_preallocate_dma_free(substream);
	if (substream->proc_prealloc_entry) {
		snd_info_unregister(substream->proc_prealloc_entry);
		substream->proc_prealloc_entry = NULL;
	}
	substream->dma_device.type = SNDRV_DMA_TYPE_UNKNOWN;
	return 0;
}

/**
 * snd_pcm_lib_preallocate_free_for_all - release all pre-allocated buffers on the pcm
 * @pcm: the pcm instance
 *
 * Releases all the pre-allocated buffers on the given pcm.
 *
 * Returns zero if successful, or a negative error code on failure.
 */
int snd_pcm_lib_preallocate_free_for_all(snd_pcm_t *pcm)
{
	snd_pcm_substream_t *substream;
	int stream;

	for (stream = 0; stream < 2; stream++)
		for (substream = pcm->streams[stream].substream; substream; substream = substream->next)
			snd_pcm_lib_preallocate_free(substream);
	return 0;
}

/*
 * read callback for prealloc proc file
 *
 * prints the current allocated size in kB.
 */
static void snd_pcm_lib_preallocate_proc_read(snd_info_entry_t *entry,
					      snd_info_buffer_t *buffer)
{
	snd_pcm_substream_t *substream = (snd_pcm_substream_t *)entry->private_data;
	snd_iprintf(buffer, "%lu\n", (unsigned long) substream->dma_buffer.bytes / 1024);
}

/*
 * write callback for prealloc proc file
 *
 * accepts the preallocation size in kB.
 */
static void snd_pcm_lib_preallocate_proc_write(snd_info_entry_t *entry,
					       snd_info_buffer_t *buffer)
{
	snd_pcm_substream_t *substream = (snd_pcm_substream_t *)entry->private_data;
	char line[64], str[64];
	size_t size;
	struct snd_dma_buffer new_dmab;

	if (substream->runtime) {
		buffer->error = -EBUSY;
		return;
	}
	if (!snd_info_get_line(buffer, line, sizeof(line))) {
		snd_info_get_str(str, line, sizeof(str));
		size = simple_strtoul(str, NULL, 10) * 1024;
		if ((size != 0 && size < 8192) || size > substream->dma_max) {
			buffer->error = -EINVAL;
			return;
		}
		if (substream->dma_buffer.bytes == size)
			return;
		memset(&new_dmab, 0, sizeof(new_dmab));
		if (size > 0) {

			if (snd_dma_alloc_pages(&substream->dma_device, size, &new_dmab) < 0) {
				buffer->error = -ENOMEM;
				return;
			}
			substream->buffer_bytes_max = size;
			snd_dma_free_reserved(&substream->dma_device);
		} else {
			substream->buffer_bytes_max = UINT_MAX;
		}
		snd_dma_set_reserved(&substream->dma_device, &new_dmab);
		substream->dma_buffer = new_dmab;
	} else {
		buffer->error = -EINVAL;
	}
}

/*
 * pre-allocate the buffer and create a proc file for the substream
 */
static int snd_pcm_lib_preallocate_pages1(snd_pcm_substream_t *substream,
					  size_t size, size_t max)
{
	snd_info_entry_t *entry;

	memset(&substream->dma_buffer, 0, sizeof(substream->dma_buffer));
	if (size > 0 && preallocate_dma && substream->number < maximum_substreams)
		preallocate_pcm_pages(substream, size);

	if (substream->dma_buffer.bytes > 0)
		substream->buffer_bytes_max = substream->dma_buffer.bytes;
	substream->dma_max = max;
	if ((entry = snd_info_create_card_entry(substream->pcm->card, "prealloc", substream->proc_root)) != NULL) {
		entry->c.text.read_size = 64;
		entry->c.text.read = snd_pcm_lib_preallocate_proc_read;
		entry->c.text.write_size = 64;
		entry->c.text.write = snd_pcm_lib_preallocate_proc_write;
		entry->private_data = substream;
		if (snd_info_register(entry) < 0) {
			snd_info_free_entry(entry);
			entry = NULL;
		}
	}
	substream->proc_prealloc_entry = entry;
	return 0;
}


/*
 * set up the unique pcm id
 */
static inline void setup_pcm_id(snd_pcm_substream_t *subs)
{
	if (! subs->dma_device.id) {
		subs->dma_device.id = subs->pcm->device << 16 |
			subs->stream << 8 | (subs->number + 1);
		if (subs->dma_device.type == SNDRV_DMA_TYPE_CONTINUOUS ||
		    subs->dma_device.dev == NULL)
			subs->dma_device.id |= (subs->pcm->card->number + 1) << 24;
	}
}

/**
 * snd_pcm_lib_preallocate_pages - pre-allocation for the given DMA type
 * @substream: the pcm substream instance
 * @type: DMA type (SNDRV_DMA_TYPE_*)
 * @data: DMA type dependant data
 * @size: the requested pre-allocation size in bytes
 * @max: the max. allowed pre-allocation size
 *
 * Do pre-allocation for the given DMA type.
 *
 * Returns zero if successful, or a negative error code on failure.
 */
int snd_pcm_lib_preallocate_pages(snd_pcm_substream_t *substream,
				  int type, struct device *data,
				  size_t size, size_t max)
{
	substream->dma_device.type = type;
	substream->dma_device.dev = data;
	setup_pcm_id(substream);
	return snd_pcm_lib_preallocate_pages1(substream, size, max);
}

/**
 * snd_pcm_lib_preallocate_pages_for_all - pre-allocation for continous memory type (all substreams)
 * @substream: the pcm substream instance
 * @type: DMA type (SNDRV_DMA_TYPE_*)
 * @data: DMA type dependant data
 * @size: the requested pre-allocation size in bytes
 * @max: the max. allowed pre-allocation size
 *
 * Do pre-allocation to all substreams of the given pcm for the
 * specified DMA type.
 *
 * Returns zero if successful, or a negative error code on failure.
 */
int snd_pcm_lib_preallocate_pages_for_all(snd_pcm_t *pcm,
					  int type, void *data,
					  size_t size, size_t max)
{
	snd_pcm_substream_t *substream;
	int stream, err;

	for (stream = 0; stream < 2; stream++)
		for (substream = pcm->streams[stream].substream; substream; substream = substream->next)
			if ((err = snd_pcm_lib_preallocate_pages(substream, type, data, size, max)) < 0)
				return err;
	return 0;
}

/**
 * snd_pcm_sgbuf_ops_page - get the page struct at the given offset
 * @substream: the pcm substream instance
 * @offset: the buffer offset
 *
 * Returns the page struct at the given buffer offset.
 * Used as the page callback of PCM ops.
 */
struct page *snd_pcm_sgbuf_ops_page(snd_pcm_substream_t *substream, unsigned long offset)
{
	struct snd_sg_buf *sgbuf = snd_pcm_substream_sgbuf(substream);

	unsigned int idx = offset >> PAGE_SHIFT;
	if (idx >= (unsigned int)sgbuf->pages)
		return NULL;
	return sgbuf->page_table[idx];
}

/**
 * snd_pcm_lib_malloc_pages - allocate the DMA buffer
 * @substream: the substream to allocate the DMA buffer to
 * @size: the requested buffer size in bytes
 *
 * Allocates the DMA buffer on the BUS type given by
 * snd_pcm_lib_preallocate_xxx_pages().
 *
 * Returns 1 if the buffer is changed, 0 if not changed, or a negative
 * code on failure.
 */
int snd_pcm_lib_malloc_pages(snd_pcm_substream_t *substream, size_t size)
{
	snd_pcm_runtime_t *runtime;
	struct snd_dma_buffer dmab;

	snd_assert(substream->dma_device.type != SNDRV_DMA_TYPE_UNKNOWN, return -EINVAL);
	snd_assert(substream != NULL, return -EINVAL);
	runtime = substream->runtime;
	snd_assert(runtime != NULL, return -EINVAL);	

	if (runtime->dma_area != NULL) {
		/* perphaps, we might free the large DMA memory region
		   to save some space here, but the actual solution
		   costs us less time */
		if (runtime->dma_bytes >= size)
			return 0;	/* ok, do not change */
		snd_pcm_lib_free_pages(substream);
	}
	if (substream->dma_buffer.area != NULL && substream->dma_buffer.bytes >= size) {
		dmab = substream->dma_buffer; /* use the pre-allocated buffer */
	} else {
		memset(&dmab, 0, sizeof(dmab)); /* allocate a new buffer */
		if (snd_dma_alloc_pages(&substream->dma_device, size, &dmab) < 0)
			return -ENOMEM;
	}
	runtime->dma_area = dmab.area;
	runtime->dma_addr = dmab.addr;
	runtime->dma_private = dmab.private_data;
	runtime->dma_bytes = size;
	return 1;			/* area was changed */
}

/**
 * snd_pcm_lib_free_pages - release the allocated DMA buffer.
 * @substream: the substream to release the DMA buffer
 *
 * Releases the DMA buffer allocated via snd_pcm_lib_malloc_pages().
 *
 * Returns zero if successful, or a negative error code on failure.
 */
int snd_pcm_lib_free_pages(snd_pcm_substream_t *substream)
{
	snd_pcm_runtime_t *runtime;

	snd_assert(substream != NULL, return -EINVAL);
	runtime = substream->runtime;
	snd_assert(runtime != NULL, return -EINVAL);
	if (runtime->dma_area == NULL)
		return 0;
	if (runtime->dma_area != substream->dma_buffer.area) {
		/* it's a newly allocated buffer.  release it now. */
		struct snd_dma_buffer dmab;
		memset(&dmab, 0, sizeof(dmab));
		dmab.area = runtime->dma_area;
		dmab.addr = runtime->dma_addr;
		dmab.bytes = runtime->dma_bytes;
		dmab.private_data = runtime->dma_private;
		snd_dma_free_pages(&substream->dma_device, &dmab);
	}
	runtime->dma_area = NULL;
	runtime->dma_addr = 0UL;
	runtime->dma_bytes = 0;
	runtime->dma_private = NULL;
	return 0;
}

#ifndef MODULE

/* format is: snd-pcm=preallocate_dma,maximum_substreams */

static int __init alsa_pcm_setup(char *str)
{
	(void)(get_option(&str,&preallocate_dma) == 2 &&
	       get_option(&str,&maximum_substreams) == 2);
	return 1;
}

__setup("snd-pcm=", alsa_pcm_setup);

#endif /* ifndef MODULE */
