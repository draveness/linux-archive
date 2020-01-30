/*
 * file.c - operations for regular (text) files.
 */

#include <linux/module.h>
#include <linux/dnotify.h>
#include <linux/kobject.h>
#include <asm/uaccess.h>

#include "sysfs.h"

static struct file_operations sysfs_file_operations;

static int init_file(struct inode * inode)
{
	inode->i_size = PAGE_SIZE;
	inode->i_fop = &sysfs_file_operations;
	return 0;
}

#define to_subsys(k) container_of(k,struct subsystem,kset.kobj)
#define to_sattr(a) container_of(a,struct subsys_attribute,attr)

/**
 * Subsystem file operations.
 * These operations allow subsystems to have files that can be 
 * read/written. 
 */
static ssize_t 
subsys_attr_show(struct kobject * kobj, struct attribute * attr, char * page)
{
	struct subsystem * s = to_subsys(kobj);
	struct subsys_attribute * sattr = to_sattr(attr);
	ssize_t ret = 0;

	if (sattr->show)
		ret = sattr->show(s,page);
	return ret;
}

static ssize_t 
subsys_attr_store(struct kobject * kobj, struct attribute * attr, 
		  const char * page, size_t count)
{
	struct subsystem * s = to_subsys(kobj);
	struct subsys_attribute * sattr = to_sattr(attr);
	ssize_t ret = 0;

	if (sattr->store)
		ret = sattr->store(s,page,count);
	return ret;
}

static struct sysfs_ops subsys_sysfs_ops = {
	.show	= subsys_attr_show,
	.store	= subsys_attr_store,
};


struct sysfs_buffer {
	size_t			count;
	loff_t			pos;
	char			* page;
	struct sysfs_ops	* ops;
};


/**
 *	fill_read_buffer - allocate and fill buffer from object.
 *	@file:		file pointer.
 *	@buffer:	data buffer for file.
 *
 *	Allocate @buffer->page, if it hasn't been already, then call the
 *	kobject's show() method to fill the buffer with this attribute's 
 *	data. 
 *	This is called only once, on the file's first read. 
 */
static int fill_read_buffer(struct file * file, struct sysfs_buffer * buffer)
{
	struct attribute * attr = file->f_dentry->d_fsdata;
	struct kobject * kobj = file->f_dentry->d_parent->d_fsdata;
	struct sysfs_ops * ops = buffer->ops;
	int ret = 0;
	ssize_t count;

	if (!buffer->page)
		buffer->page = (char *) get_zeroed_page(GFP_KERNEL);
	if (!buffer->page)
		return -ENOMEM;

	count = ops->show(kobj,attr,buffer->page);
	BUG_ON(count > (ssize_t)PAGE_SIZE);
	if (count >= 0)
		buffer->count = count;
	else
		ret = count;
	return ret;
}


/**
 *	flush_read_buffer - push buffer to userspace.
 *	@buffer:	data buffer for file.
 *	@userbuf:	user-passed buffer.
 *	@count:		number of bytes requested.
 *	@ppos:		file position.
 *
 *	Copy the buffer we filled in fill_read_buffer() to userspace.
 *	This is done at the reader's leisure, copying and advancing 
 *	the amount they specify each time.
 *	This may be called continuously until the buffer is empty.
 */
static int flush_read_buffer(struct sysfs_buffer * buffer, char __user * buf,
			     size_t count, loff_t * ppos)
{
	int error;

	if (count > (buffer->count - *ppos))
		count = buffer->count - *ppos;

	error = copy_to_user(buf,buffer->page + *ppos,count);
	if (!error)
		*ppos += count;
	return error ? -EFAULT : count;
}

/**
 *	sysfs_read_file - read an attribute. 
 *	@file:	file pointer.
 *	@buf:	buffer to fill.
 *	@count:	number of bytes to read.
 *	@ppos:	starting offset in file.
 *
 *	Userspace wants to read an attribute file. The attribute descriptor
 *	is in the file's ->d_fsdata. The target object is in the directory's
 *	->d_fsdata.
 *
 *	We call fill_read_buffer() to allocate and fill the buffer from the
 *	object's show() method exactly once (if the read is happening from
 *	the beginning of the file). That should fill the entire buffer with
 *	all the data the object has to offer for that attribute.
 *	We then call flush_read_buffer() to copy the buffer to userspace
 *	in the increments specified.
 */

static ssize_t
sysfs_read_file(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct sysfs_buffer * buffer = file->private_data;
	ssize_t retval = 0;

	if (!*ppos) {
		if ((retval = fill_read_buffer(file,buffer)))
			return retval;
	}
	pr_debug("%s: count = %d, ppos = %lld, buf = %s\n",
		 __FUNCTION__,count,*ppos,buffer->page);
	return flush_read_buffer(buffer,buf,count,ppos);
}


/**
 *	fill_write_buffer - copy buffer from userspace.
 *	@buffer:	data buffer for file.
 *	@userbuf:	data from user.
 *	@count:		number of bytes in @userbuf.
 *
 *	Allocate @buffer->page if it hasn't been already, then
 *	copy the user-supplied buffer into it.
 */

static int 
fill_write_buffer(struct sysfs_buffer * buffer, const char __user * buf, size_t count)
{
	int error;

	if (!buffer->page)
		buffer->page = (char *)get_zeroed_page(GFP_KERNEL);
	if (!buffer->page)
		return -ENOMEM;

	if (count >= PAGE_SIZE)
		count = PAGE_SIZE - 1;
	error = copy_from_user(buffer->page,buf,count);
	return error ? -EFAULT : count;
}


/**
 *	flush_write_buffer - push buffer to kobject.
 *	@file:		file pointer.
 *	@buffer:	data buffer for file.
 *
 *	Get the correct pointers for the kobject and the attribute we're
 *	dealing with, then call the store() method for the attribute, 
 *	passing the buffer that we acquired in fill_write_buffer().
 */

static int 
flush_write_buffer(struct file * file, struct sysfs_buffer * buffer, size_t count)
{
	struct attribute * attr = file->f_dentry->d_fsdata;
	struct kobject * kobj = file->f_dentry->d_parent->d_fsdata;
	struct sysfs_ops * ops = buffer->ops;

	return ops->store(kobj,attr,buffer->page,count);
}


/**
 *	sysfs_write_file - write an attribute.
 *	@file:	file pointer
 *	@buf:	data to write
 *	@count:	number of bytes
 *	@ppos:	starting offset
 *
 *	Similar to sysfs_read_file(), though working in the opposite direction.
 *	We allocate and fill the data from the user in fill_write_buffer(),
 *	then push it to the kobject in flush_write_buffer().
 *	There is no easy way for us to know if userspace is only doing a partial
 *	write, so we don't support them. We expect the entire buffer to come
 *	on the first write. 
 *	Hint: if you're writing a value, first read the file, modify only the
 *	the value you're changing, then write entire buffer back. 
 */

static ssize_t
sysfs_write_file(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct sysfs_buffer * buffer = file->private_data;

	count = fill_write_buffer(buffer,buf,count);
	if (count > 0)
		count = flush_write_buffer(file,buffer,count);
	if (count > 0)
		*ppos += count;
	return count;
}

static int check_perm(struct inode * inode, struct file * file)
{
	struct kobject *kobj = sysfs_get_kobject(file->f_dentry->d_parent);
	struct attribute * attr = file->f_dentry->d_fsdata;
	struct sysfs_buffer * buffer;
	struct sysfs_ops * ops = NULL;
	int error = 0;

	if (!kobj || !attr)
		goto Einval;

	/* Grab the module reference for this attribute if we have one */
	if (!try_module_get(attr->owner)) {
		error = -ENODEV;
		goto Done;
	}

	/* if the kobject has no ktype, then we assume that it is a subsystem
	 * itself, and use ops for it.
	 */
	if (kobj->kset && kobj->kset->ktype)
		ops = kobj->kset->ktype->sysfs_ops;
	else if (kobj->ktype)
		ops = kobj->ktype->sysfs_ops;
	else
		ops = &subsys_sysfs_ops;

	/* No sysfs operations, either from having no subsystem,
	 * or the subsystem have no operations.
	 */
	if (!ops)
		goto Eaccess;

	/* File needs write support.
	 * The inode's perms must say it's ok, 
	 * and we must have a store method.
	 */
	if (file->f_mode & FMODE_WRITE) {

		if (!(inode->i_mode & S_IWUGO) || !ops->store)
			goto Eaccess;

	}

	/* File needs read support.
	 * The inode's perms must say it's ok, and we there
	 * must be a show method for it.
	 */
	if (file->f_mode & FMODE_READ) {
		if (!(inode->i_mode & S_IRUGO) || !ops->show)
			goto Eaccess;
	}

	/* No error? Great, allocate a buffer for the file, and store it
	 * it in file->private_data for easy access.
	 */
	buffer = kmalloc(sizeof(struct sysfs_buffer),GFP_KERNEL);
	if (buffer) {
		memset(buffer,0,sizeof(struct sysfs_buffer));
		buffer->ops = ops;
		file->private_data = buffer;
	} else
		error = -ENOMEM;
	goto Done;

 Einval:
	error = -EINVAL;
	goto Done;
 Eaccess:
	error = -EACCES;
	module_put(attr->owner);
 Done:
	if (error && kobj)
		kobject_put(kobj);
	return error;
}

static int sysfs_open_file(struct inode * inode, struct file * filp)
{
	return check_perm(inode,filp);
}

static int sysfs_release(struct inode * inode, struct file * filp)
{
	struct kobject * kobj = filp->f_dentry->d_parent->d_fsdata;
	struct attribute * attr = filp->f_dentry->d_fsdata;
	struct sysfs_buffer * buffer = filp->private_data;

	if (kobj) 
		kobject_put(kobj);
	module_put(attr->owner);

	if (buffer) {
		if (buffer->page)
			free_page((unsigned long)buffer->page);
		kfree(buffer);
	}
	return 0;
}

static struct file_operations sysfs_file_operations = {
	.read		= sysfs_read_file,
	.write		= sysfs_write_file,
	.llseek		= generic_file_llseek,
	.open		= sysfs_open_file,
	.release	= sysfs_release,
};


int sysfs_add_file(struct dentry * dir, const struct attribute * attr)
{
	struct dentry * dentry;
	int error;

	down(&dir->d_inode->i_sem);
	dentry = sysfs_get_dentry(dir,attr->name);
	if (!IS_ERR(dentry)) {
		error = sysfs_create(dentry,
				     (attr->mode & S_IALLUGO) | S_IFREG,
				     init_file);
		if (!error)
			dentry->d_fsdata = (void *)attr;
		dput(dentry);
	} else
		error = PTR_ERR(dentry);
	up(&dir->d_inode->i_sem);
	return error;
}


/**
 *	sysfs_create_file - create an attribute file for an object.
 *	@kobj:	object we're creating for. 
 *	@attr:	atrribute descriptor.
 */

int sysfs_create_file(struct kobject * kobj, const struct attribute * attr)
{
	if (kobj && attr)
		return sysfs_add_file(kobj->dentry,attr);
	return -EINVAL;
}


/**
 * sysfs_update_file - update the modified timestamp on an object attribute.
 * @kobj: object we're acting for.
 * @attr: attribute descriptor.
 *
 * Also call dnotify for the dentry, which lots of userspace programs
 * use.
 */
int sysfs_update_file(struct kobject * kobj, const struct attribute * attr)
{
	struct dentry * dir = kobj->dentry;
	struct dentry * victim;
	int res = -ENOENT;

	down(&dir->d_inode->i_sem);
	victim = sysfs_get_dentry(dir, attr->name);
	if (!IS_ERR(victim)) {
		/* make sure dentry is really there */
		if (victim->d_inode && 
		    (victim->d_parent->d_inode == dir->d_inode)) {
			victim->d_inode->i_mtime = CURRENT_TIME;
			dnotify_parent(victim, DN_MODIFY);

			/**
			 * Drop reference from initial sysfs_get_dentry().
			 */
			dput(victim);
			res = 0;
		}
		
		/**
		 * Drop the reference acquired from sysfs_get_dentry() above.
		 */
		dput(victim);
	}
	up(&dir->d_inode->i_sem);

	return res;
}


/**
 *	sysfs_remove_file - remove an object attribute.
 *	@kobj:	object we're acting for.
 *	@attr:	attribute descriptor.
 *
 *	Hash the attribute name and kill the victim.
 */

void sysfs_remove_file(struct kobject * kobj, const struct attribute * attr)
{
	sysfs_hash_and_remove(kobj->dentry,attr->name);
}


EXPORT_SYMBOL(sysfs_create_file);
EXPORT_SYMBOL(sysfs_remove_file);
EXPORT_SYMBOL(sysfs_update_file);

