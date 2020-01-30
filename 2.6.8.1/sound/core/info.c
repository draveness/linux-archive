/*
 *  Information interface for ALSA driver
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
#include <linux/version.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/time.h>
#include <linux/smp_lock.h>
#include <sound/core.h>
#include <sound/minors.h>
#include <sound/info.h>
#include <sound/version.h>
#include <linux/proc_fs.h>
#include <linux/devfs_fs_kernel.h>
#include <stdarg.h>

/*
 *
 */

int snd_info_check_reserved_words(const char *str)
{
	static char *reserved[] =
	{
		"version",
		"meminfo",
		"memdebug",
		"detect",
		"devices",
		"oss",
		"cards",
		"timers",
		"synth",
		"pcm",
		"seq",
		NULL
	};
	char **xstr = reserved;

	while (*xstr) {
		if (!strcmp(*xstr, str))
			return 0;
		xstr++;
	}
	if (!strncmp(str, "card", 4))
		return 0;
	return 1;
}

#ifdef CONFIG_PROC_FS

static DECLARE_MUTEX(info_mutex);

typedef struct _snd_info_private_data {
	snd_info_buffer_t *rbuffer;
	snd_info_buffer_t *wbuffer;
	snd_info_entry_t *entry;
	void *file_private_data;
} snd_info_private_data_t;

static int snd_info_version_init(void);
static int snd_info_version_done(void);


/**
 * snd_iprintf - printf on the procfs buffer
 * @buffer: the procfs buffer
 * @fmt: the printf format
 *
 * Outputs the string on the procfs buffer just like printf().
 *
 * Returns the size of output string.
 */
int snd_iprintf(snd_info_buffer_t * buffer, char *fmt,...)
{
	va_list args;
	int res;
	char sbuffer[512];

	if (buffer->stop || buffer->error)
		return 0;
	va_start(args, fmt);
	res = vscnprintf(sbuffer, sizeof(sbuffer), fmt, args);
	va_end(args);
	if (buffer->size + res >= buffer->len) {
		buffer->stop = 1;
		return 0;
	}
	strcpy(buffer->curr, sbuffer);
	buffer->curr += res;
	buffer->size += res;
	return res;
}

/*

 */

static struct proc_dir_entry *snd_proc_root = NULL;
snd_info_entry_t *snd_seq_root = NULL;
#ifdef CONFIG_SND_OSSEMUL
snd_info_entry_t *snd_oss_root = NULL;
#endif

static inline void snd_info_entry_prepare(struct proc_dir_entry *de)
{
	de->owner = THIS_MODULE;
}

void snd_remove_proc_entry(struct proc_dir_entry *parent,
			   struct proc_dir_entry *de)
{
	if (de)
		remove_proc_entry(de->name, parent);
}

static loff_t snd_info_entry_llseek(struct file *file, loff_t offset, int orig)
{
	snd_info_private_data_t *data;
	struct snd_info_entry *entry;
	loff_t ret;

	data = snd_magic_cast(snd_info_private_data_t, file->private_data, return -ENXIO);
	entry = data->entry;
	lock_kernel();
	switch (entry->content) {
	case SNDRV_INFO_CONTENT_TEXT:
		switch (orig) {
		case 0:	/* SEEK_SET */
			file->f_pos = offset;
			ret = file->f_pos;
			goto out;
		case 1:	/* SEEK_CUR */
			file->f_pos += offset;
			ret = file->f_pos;
			goto out;
		case 2:	/* SEEK_END */
		default:
			ret = -EINVAL;
			goto out;
		}
		break;
	case SNDRV_INFO_CONTENT_DATA:
		if (entry->c.ops->llseek) {
			ret = entry->c.ops->llseek(entry,
						    data->file_private_data,
						    file, offset, orig);
			goto out;
		}
		break;
	}
	ret = -ENXIO;
out:
	unlock_kernel();
	return ret;
}

static ssize_t snd_info_entry_read(struct file *file, char __user *buffer,
				   size_t count, loff_t * offset)
{
	snd_info_private_data_t *data;
	struct snd_info_entry *entry;
	snd_info_buffer_t *buf;
	size_t size = 0;
	loff_t pos;

	data = snd_magic_cast(snd_info_private_data_t, file->private_data, return -ENXIO);
	snd_assert(data != NULL, return -ENXIO);
	pos = *offset;
	if (pos < 0 || (long) pos != pos || (ssize_t) count < 0)
		return -EIO;
	if ((unsigned long) pos + (unsigned long) count < (unsigned long) pos)
		return -EIO;
	entry = data->entry;
	switch (entry->content) {
	case SNDRV_INFO_CONTENT_TEXT:
		buf = data->rbuffer;
		if (buf == NULL)
			return -EIO;
		if (pos >= buf->size)
			return 0;
		size = buf->size - pos;
		size = min(count, size);
		if (copy_to_user(buffer, buf->buffer + pos, size))
			return -EFAULT;
		break;
	case SNDRV_INFO_CONTENT_DATA:
		if (entry->c.ops->read)
			size = entry->c.ops->read(entry,
						  data->file_private_data,
						  file, buffer, count, pos);
		break;
	}
	if ((ssize_t) size > 0)
		*offset = pos + size;
	return size;
}

static ssize_t snd_info_entry_write(struct file *file, const char __user *buffer,
				    size_t count, loff_t * offset)
{
	snd_info_private_data_t *data;
	struct snd_info_entry *entry;
	snd_info_buffer_t *buf;
	size_t size = 0;
	loff_t pos;

	data = snd_magic_cast(snd_info_private_data_t, file->private_data, return -ENXIO);
	snd_assert(data != NULL, return -ENXIO);
	entry = data->entry;
	pos = *offset;
	if (pos < 0 || (long) pos != pos || (ssize_t) count < 0)
		return -EIO;
	if ((unsigned long) pos + (unsigned long) count < (unsigned long) pos)
		return -EIO;
	switch (entry->content) {
	case SNDRV_INFO_CONTENT_TEXT:
		buf = data->wbuffer;
		if (buf == NULL)
			return -EIO;
		if (pos >= buf->len)
			return -ENOMEM;
		size = buf->len - pos;
		size = min(count, size);
		if (copy_from_user(buf->buffer + pos, buffer, size))
			return -EFAULT;
		if ((long)buf->size < pos + size)
			buf->size = pos + size;
		break;
	case SNDRV_INFO_CONTENT_DATA:
		if (entry->c.ops->write)
			size = entry->c.ops->write(entry,
						   data->file_private_data,
						   file, buffer, count, pos);
		break;
	}
	if ((ssize_t) size > 0)
		*offset = pos + size;
	return size;
}

static int snd_info_entry_open(struct inode *inode, struct file *file)
{
	snd_info_entry_t *entry;
	snd_info_private_data_t *data;
	snd_info_buffer_t *buffer;
	struct proc_dir_entry *p;
	int mode, err;

	down(&info_mutex);
	p = PDE(inode);
	entry = p == NULL ? NULL : (snd_info_entry_t *)p->data;
	if (entry == NULL || entry->disconnected) {
		up(&info_mutex);
		return -ENODEV;
	}
	if (!try_module_get(entry->module)) {
		err = -EFAULT;
		goto __error1;
	}
	mode = file->f_flags & O_ACCMODE;
	if (mode == O_RDONLY || mode == O_RDWR) {
		if ((entry->content == SNDRV_INFO_CONTENT_TEXT &&
		     !entry->c.text.read_size) ||
		    (entry->content == SNDRV_INFO_CONTENT_DATA &&
		     entry->c.ops->read == NULL)) {
		    	err = -ENODEV;
		    	goto __error;
		}
	}
	if (mode == O_WRONLY || mode == O_RDWR) {
		if ((entry->content == SNDRV_INFO_CONTENT_TEXT &&
		     !entry->c.text.write_size) ||
		    (entry->content == SNDRV_INFO_CONTENT_DATA &&
		     entry->c.ops->write == NULL)) {
		    	err = -ENODEV;
		    	goto __error;
		}
	}
	data = snd_magic_kcalloc(snd_info_private_data_t, 0, GFP_KERNEL);
	if (data == NULL) {
		err = -ENOMEM;
		goto __error;
	}
	data->entry = entry;
	switch (entry->content) {
	case SNDRV_INFO_CONTENT_TEXT:
		if (mode == O_RDONLY || mode == O_RDWR) {
			buffer = (snd_info_buffer_t *)
				 	snd_kcalloc(sizeof(snd_info_buffer_t), GFP_KERNEL);
			if (buffer == NULL) {
				snd_magic_kfree(data);
				err = -ENOMEM;
				goto __error;
			}
			buffer->len = (entry->c.text.read_size +
				      (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);
			buffer->buffer = vmalloc(buffer->len);
			if (buffer->buffer == NULL) {
				kfree(buffer);
				snd_magic_kfree(data);
				err = -ENOMEM;
				goto __error;
			}
			buffer->curr = buffer->buffer;
			data->rbuffer = buffer;
		}
		if (mode == O_WRONLY || mode == O_RDWR) {
			buffer = (snd_info_buffer_t *)
					snd_kcalloc(sizeof(snd_info_buffer_t), GFP_KERNEL);
			if (buffer == NULL) {
				if (mode == O_RDWR) {
					vfree(data->rbuffer->buffer);
					kfree(data->rbuffer);
				}
				snd_magic_kfree(data);
				err = -ENOMEM;
				goto __error;
			}
			buffer->len = (entry->c.text.write_size +
				      (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);
			buffer->buffer = vmalloc(buffer->len);
			if (buffer->buffer == NULL) {
				if (mode == O_RDWR) {
					vfree(data->rbuffer->buffer);
					kfree(data->rbuffer);
				}
				kfree(buffer);
				snd_magic_kfree(data);
				err = -ENOMEM;
				goto __error;
			}
			buffer->curr = buffer->buffer;
			data->wbuffer = buffer;
		}
		break;
	case SNDRV_INFO_CONTENT_DATA:	/* data */
		if (entry->c.ops->open) {
			if ((err = entry->c.ops->open(entry, mode,
						      &data->file_private_data)) < 0) {
				snd_magic_kfree(data);
				goto __error;
			}
		}
		break;
	}
	file->private_data = data;
	up(&info_mutex);
	if (entry->content == SNDRV_INFO_CONTENT_TEXT &&
	    (mode == O_RDONLY || mode == O_RDWR)) {
		if (entry->c.text.read) {
			down(&entry->access);
			entry->c.text.read(entry, data->rbuffer);
			up(&entry->access);
		}
	}
	return 0;

      __error:
	module_put(entry->module);
      __error1:
	up(&info_mutex);
	return err;
}

static int snd_info_entry_release(struct inode *inode, struct file *file)
{
	snd_info_entry_t *entry;
	snd_info_private_data_t *data;
	int mode;

	mode = file->f_flags & O_ACCMODE;
	data = snd_magic_cast(snd_info_private_data_t, file->private_data, return -ENXIO);
	entry = data->entry;
	switch (entry->content) {
	case SNDRV_INFO_CONTENT_TEXT:
		if (mode == O_RDONLY || mode == O_RDWR) {
			vfree(data->rbuffer->buffer);
			kfree(data->rbuffer);
		}
		if (mode == O_WRONLY || mode == O_RDWR) {
			if (entry->c.text.write) {
				entry->c.text.write(entry, data->wbuffer);
				if (data->wbuffer->error) {
					snd_printk(KERN_WARNING "data write error to %s (%i)\n",
						entry->name,
						data->wbuffer->error);
				}
			}
			vfree(data->wbuffer->buffer);
			kfree(data->wbuffer);
		}
		break;
	case SNDRV_INFO_CONTENT_DATA:
		if (entry->c.ops->release)
			entry->c.ops->release(entry, mode,
					      data->file_private_data);
		break;
	}
	module_put(entry->module);
	snd_magic_kfree(data);
	return 0;
}

static unsigned int snd_info_entry_poll(struct file *file, poll_table * wait)
{
	snd_info_private_data_t *data;
	struct snd_info_entry *entry;
	unsigned int mask;

	data = snd_magic_cast(snd_info_private_data_t, file->private_data, return -ENXIO);
	if (data == NULL)
		return 0;
	entry = data->entry;
	mask = 0;
	switch (entry->content) {
	case SNDRV_INFO_CONTENT_DATA:
		if (entry->c.ops->poll)
			return entry->c.ops->poll(entry,
						  data->file_private_data,
						  file, wait);
		if (entry->c.ops->read)
			mask |= POLLIN | POLLRDNORM;
		if (entry->c.ops->write)
			mask |= POLLOUT | POLLWRNORM;
		break;
	}
	return mask;
}

static int snd_info_entry_ioctl(struct inode *inode, struct file *file,
				unsigned int cmd, unsigned long arg)
{
	snd_info_private_data_t *data;
	struct snd_info_entry *entry;

	data = snd_magic_cast(snd_info_private_data_t, file->private_data, return -ENXIO);
	if (data == NULL)
		return 0;
	entry = data->entry;
	switch (entry->content) {
	case SNDRV_INFO_CONTENT_DATA:
		if (entry->c.ops->ioctl)
			return entry->c.ops->ioctl(entry,
						   data->file_private_data,
						   file, cmd, arg);
		break;
	}
	return -ENOTTY;
}

static int snd_info_entry_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file->f_dentry->d_inode;
	snd_info_private_data_t *data;
	struct snd_info_entry *entry;

	data = snd_magic_cast(snd_info_private_data_t, file->private_data, return -ENXIO);
	if (data == NULL)
		return 0;
	entry = data->entry;
	switch (entry->content) {
	case SNDRV_INFO_CONTENT_DATA:
		if (entry->c.ops->mmap)
			return entry->c.ops->mmap(entry,
						  data->file_private_data,
						  inode, file, vma);
		break;
	}
	return -ENXIO;
}

static struct file_operations snd_info_entry_operations =
{
	.owner =	THIS_MODULE,
	.llseek =	snd_info_entry_llseek,
	.read =		snd_info_entry_read,
	.write =	snd_info_entry_write,
	.poll =		snd_info_entry_poll,
	.ioctl =	snd_info_entry_ioctl,
	.mmap =		snd_info_entry_mmap,
	.open =		snd_info_entry_open,
	.release =	snd_info_entry_release,
};

/**
 * snd_create_proc_entry - create a procfs entry
 * @name: the name of the proc file
 * @mode: the file permission bits, S_Ixxx
 * @parent: the parent proc-directory entry
 *
 * Creates a new proc file entry with the given name and permission
 * on the given directory.
 *
 * Returns the pointer of new instance or NULL on failure.
 */
struct proc_dir_entry *snd_create_proc_entry(const char *name, mode_t mode,
					     struct proc_dir_entry *parent)
{
	struct proc_dir_entry *p;
	p = create_proc_entry(name, mode, parent);
	if (p)
		snd_info_entry_prepare(p);
	return p;
}

int __init snd_info_init(void)
{
	struct proc_dir_entry *p;

	p = snd_create_proc_entry("asound", S_IFDIR | S_IRUGO | S_IXUGO, &proc_root);
	if (p == NULL)
		return -ENOMEM;
	snd_proc_root = p;
#ifdef CONFIG_SND_OSSEMUL
	{
		snd_info_entry_t *entry;
		if ((entry = snd_info_create_module_entry(THIS_MODULE, "oss", NULL)) == NULL)
			return -ENOMEM;
		entry->mode = S_IFDIR | S_IRUGO | S_IXUGO;
		if (snd_info_register(entry) < 0) {
			snd_info_free_entry(entry);
			return -ENOMEM;
		}
		snd_oss_root = entry;
	}
#endif
#if defined(CONFIG_SND_SEQUENCER) || defined(CONFIG_SND_SEQUENCER_MODULE)
	{
		snd_info_entry_t *entry;
		if ((entry = snd_info_create_module_entry(THIS_MODULE, "seq", NULL)) == NULL)
			return -ENOMEM;
		entry->mode = S_IFDIR | S_IRUGO | S_IXUGO;
		if (snd_info_register(entry) < 0) {
			snd_info_free_entry(entry);
			return -ENOMEM;
		}
		snd_seq_root = entry;
	}
#endif
	snd_info_version_init();
	snd_memory_info_init();
	snd_minor_info_init();
	snd_minor_info_oss_init();
	snd_card_info_init();
	return 0;
}

int __exit snd_info_done(void)
{
	snd_card_info_done();
	snd_minor_info_oss_done();
	snd_minor_info_done();
	snd_memory_info_done();
	snd_info_version_done();
	if (snd_proc_root) {
#if defined(CONFIG_SND_SEQUENCER) || defined(CONFIG_SND_SEQUENCER_MODULE)
		if (snd_seq_root)
			snd_info_unregister(snd_seq_root);
#endif
#ifdef CONFIG_SND_OSSEMUL
		if (snd_oss_root)
			snd_info_unregister(snd_oss_root);
#endif
		snd_remove_proc_entry(&proc_root, snd_proc_root);
	}
	return 0;
}

/*

 */


/*
 * create a card proc file
 * called from init.c
 */
int snd_info_card_create(snd_card_t * card)
{
	char str[8];
	snd_info_entry_t *entry;

	snd_assert(card != NULL, return -ENXIO);

	sprintf(str, "card%i", card->number);
	if ((entry = snd_info_create_module_entry(card->module, str, NULL)) == NULL)
		return -ENOMEM;
	entry->mode = S_IFDIR | S_IRUGO | S_IXUGO;
	if (snd_info_register(entry) < 0) {
		snd_info_free_entry(entry);
		return -ENOMEM;
	}
	card->proc_root = entry;
	return 0;
}

/*
 * register the card proc file
 * called from init.c
 */
int snd_info_card_register(snd_card_t * card)
{
	struct proc_dir_entry *p;

	snd_assert(card != NULL, return -ENXIO);

	if (!strcmp(card->id, card->proc_root->name))
		return 0;

	p = proc_symlink(card->id, snd_proc_root, card->proc_root->name);
	if (p == NULL)
		return -ENOMEM;
	card->proc_root_link = p;
	return 0;
}

/*
 * de-register the card proc file
 * called from init.c
 */
int snd_info_card_free(snd_card_t * card)
{
	snd_assert(card != NULL, return -ENXIO);
	if (card->proc_root_link) {
		snd_remove_proc_entry(snd_proc_root, card->proc_root_link);
		card->proc_root_link = NULL;
	}
	if (card->proc_root) {
		snd_info_unregister(card->proc_root);
		card->proc_root = NULL;
	}
	return 0;
}


/**
 * snd_info_get_line - read one line from the procfs buffer
 * @buffer: the procfs buffer
 * @line: the buffer to store
 * @len: the max. buffer size - 1
 *
 * Reads one line from the buffer and stores the string.
 *
 * Returns zero if successful, or 1 if error or EOF.
 */
int snd_info_get_line(snd_info_buffer_t * buffer, char *line, int len)
{
	int c = -1;

	if (len <= 0 || buffer->stop || buffer->error)
		return 1;
	while (--len > 0) {
		c = *buffer->curr++;
		if (c == '\n') {
			if ((buffer->curr - buffer->buffer) >= (long)buffer->size) {
				buffer->stop = 1;
			}
			break;
		}
		*line++ = c;
		if ((buffer->curr - buffer->buffer) >= (long)buffer->size) {
			buffer->stop = 1;
			break;
		}
	}
	while (c != '\n' && !buffer->stop) {
		c = *buffer->curr++;
		if ((buffer->curr - buffer->buffer) >= (long)buffer->size) {
			buffer->stop = 1;
		}
	}
	*line = '\0';
	return 0;
}

/**
 * snd_info_get_line - parse a string token
 * @dest: the buffer to store the string token
 * @src: the original string
 * @len: the max. length of token - 1
 *
 * Parses the original string and copy a token to the given
 * string buffer.
 *
 * Returns the updated pointer of the original string so that
 * it can be used for the next call.
 */
char *snd_info_get_str(char *dest, char *src, int len)
{
	int c;

	while (*src == ' ' || *src == '\t')
		src++;
	if (*src == '"' || *src == '\'') {
		c = *src++;
		while (--len > 0 && *src && *src != c) {
			*dest++ = *src++;
		}
		if (*src == c)
			src++;
	} else {
		while (--len > 0 && *src && *src != ' ' && *src != '\t') {
			*dest++ = *src++;
		}
	}
	*dest = 0;
	while (*src == ' ' || *src == '\t')
		src++;
	return src;
}

/**
 * snd_info_create_entry - create an info entry
 * @name: the proc file name
 *
 * Creates an info entry with the given file name and initializes as
 * the default state.
 *
 * Usually called from other functions such as
 * snd_info_create_card_entry().
 *
 * Returns the pointer of the new instance, or NULL on failure.
 */
static snd_info_entry_t *snd_info_create_entry(const char *name)
{
	snd_info_entry_t *entry;
	entry = snd_magic_kcalloc(snd_info_entry_t, 0, GFP_KERNEL);
	if (entry == NULL)
		return NULL;
	entry->name = snd_kmalloc_strdup(name, GFP_KERNEL);
	if (entry->name == NULL) {
		snd_magic_kfree(entry);
		return NULL;
	}
	entry->mode = S_IFREG | S_IRUGO;
	entry->content = SNDRV_INFO_CONTENT_TEXT;
	init_MUTEX(&entry->access);
	return entry;
}

/**
 * snd_info_create_module_entry - create an info entry for the given module
 * @module: the module pointer
 * @name: the file name
 * @parent: the parent directory
 *
 * Creates a new info entry and assigns it to the given module.
 *
 * Returns the pointer of the new instance, or NULL on failure.
 */
snd_info_entry_t *snd_info_create_module_entry(struct module * module,
					       const char *name,
					       snd_info_entry_t *parent)
{
	snd_info_entry_t *entry = snd_info_create_entry(name);
	if (entry) {
		entry->module = module;
		entry->parent = parent;
	}
	return entry;
}

/**
 * snd_info_create_card_entry - create an info entry for the given card
 * @card: the card instance
 * @name: the file name
 * @parent: the parent directory
 *
 * Creates a new info entry and assigns it to the given card.
 *
 * Returns the pointer of the new instance, or NULL on failure.
 */
snd_info_entry_t *snd_info_create_card_entry(snd_card_t * card,
					     const char *name,
					     snd_info_entry_t * parent)
{
	snd_info_entry_t *entry = snd_info_create_entry(name);
	if (entry) {
		entry->module = card->module;
		entry->card = card;
		entry->parent = parent;
	}
	return entry;
}

static int snd_info_dev_free_entry(snd_device_t *device)
{
	snd_info_entry_t *entry = snd_magic_cast(snd_info_entry_t, device->device_data, return -ENXIO);
	snd_info_free_entry(entry);
	return 0;
}

static int snd_info_dev_register_entry(snd_device_t *device)
{
	snd_info_entry_t *entry = snd_magic_cast(snd_info_entry_t, device->device_data, return -ENXIO);
	return snd_info_register(entry);
}

static int snd_info_dev_disconnect_entry(snd_device_t *device)
{
	snd_info_entry_t *entry = snd_magic_cast(snd_info_entry_t, device->device_data, return -ENXIO);
	entry->disconnected = 1;
	return 0;
}

static int snd_info_dev_unregister_entry(snd_device_t *device)
{
	snd_info_entry_t *entry = snd_magic_cast(snd_info_entry_t, device->device_data, return -ENXIO);
	return snd_info_unregister(entry);
}

/**
 * snd_card_proc_new - create an info entry for the given card
 * @card: the card instance
 * @name: the file name
 * @entryp: the pointer to store the new info entry
 *
 * Creates a new info entry and assigns it to the given card.
 * Unlike snd_info_create_card_entry(), this function registers the
 * info entry as an ALSA device component, so that it can be
 * unregistered/released without explicit call.
 * Also, you don't have to register this entry via snd_info_register(),
 * since this will be registered by snd_card_register() automatically.
 *
 * The parent is assumed as card->proc_root.
 *
 * For releasing this entry, use snd_device_free() instead of
 * snd_info_free_entry(). 
 *
 * Returns zero if successful, or a negative error code on failure.
 */
int snd_card_proc_new(snd_card_t *card, const char *name,
		      snd_info_entry_t **entryp)
{
	static snd_device_ops_t ops = {
		.dev_free = snd_info_dev_free_entry,
		.dev_register =	snd_info_dev_register_entry,
		.dev_disconnect = snd_info_dev_disconnect_entry,
		.dev_unregister = snd_info_dev_unregister_entry
	};
	snd_info_entry_t *entry;
	int err;

	entry = snd_info_create_card_entry(card, name, card->proc_root);
	if (! entry)
		return -ENOMEM;
	if ((err = snd_device_new(card, SNDRV_DEV_INFO, entry, &ops)) < 0) {
		snd_info_free_entry(entry);
		return err;
	}
	if (entryp)
		*entryp = entry;
	return 0;
}

/**
 * snd_info_free_entry - release the info entry
 * @entry: the info entry
 *
 * Releases the info entry.  Don't call this after registered.
 */
void snd_info_free_entry(snd_info_entry_t * entry)
{
	if (entry == NULL)
		return;
	if (entry->name)
		kfree((char *)entry->name);
	if (entry->private_free)
		entry->private_free(entry);
	snd_magic_kfree(entry);
}

/**
 * snd_info_register - register the info entry
 * @entry: the info entry
 *
 * Registers the proc info entry.
 *
 * Returns zero if successful, or a negative error code on failure.
 */
int snd_info_register(snd_info_entry_t * entry)
{
	struct proc_dir_entry *root, *p = NULL;

	snd_assert(entry != NULL, return -ENXIO);
	root = entry->parent == NULL ? snd_proc_root : entry->parent->p;
	down(&info_mutex);
	p = snd_create_proc_entry(entry->name, entry->mode, root);
	if (!p) {
		up(&info_mutex);
		return -ENOMEM;
	}
	p->owner = entry->module;
	if (!S_ISDIR(entry->mode))
		p->proc_fops = &snd_info_entry_operations;
	p->size = entry->size;
	p->data = entry;
	entry->p = p;
	up(&info_mutex);
	return 0;
}

/**
 * snd_info_unregister - de-register the info entry
 * @entry: the info entry
 *
 * De-registers the info entry and releases the instance.
 *
 * Returns zero if successful, or a negative error code on failure.
 */
int snd_info_unregister(snd_info_entry_t * entry)
{
	struct proc_dir_entry *root;

	snd_assert(entry != NULL && entry->p != NULL, return -ENXIO);
	root = entry->parent == NULL ? snd_proc_root : entry->parent->p;
	snd_assert(root, return -ENXIO);
	down(&info_mutex);
	snd_remove_proc_entry(root, entry->p);
	up(&info_mutex);
	snd_info_free_entry(entry);
	return 0;
}

/*

 */

static snd_info_entry_t *snd_info_version_entry = NULL;

static void snd_info_version_read(snd_info_entry_t *entry, snd_info_buffer_t * buffer)
{
	static char *kernel_version = UTS_RELEASE;

	snd_iprintf(buffer,
		    "Advanced Linux Sound Architecture Driver Version " CONFIG_SND_VERSION CONFIG_SND_DATE ".\n"
		    "Compiled on " __DATE__ " for kernel %s"
#ifdef CONFIG_SMP
		    " (SMP)"
#endif
#ifdef MODVERSIONS
		    " with versioned symbols"
#endif
		    ".\n", kernel_version);
}

static int __init snd_info_version_init(void)
{
	snd_info_entry_t *entry;

	entry = snd_info_create_module_entry(THIS_MODULE, "version", NULL);
	if (entry == NULL)
		return -ENOMEM;
	entry->c.text.read_size = 256;
	entry->c.text.read = snd_info_version_read;
	if (snd_info_register(entry) < 0) {
		snd_info_free_entry(entry);
		return -ENOMEM;
	}
	snd_info_version_entry = entry;
	return 0;
}

static int __exit snd_info_version_done(void)
{
	if (snd_info_version_entry)
		snd_info_unregister(snd_info_version_entry);
	return 0;
}

#endif /* CONFIG_PROC_FS */
