/*
 * linux/fs/ext4/acl.c
 *
 * Copyright (C) 2001-2003 Andreas Gruenbacher, <agruen@suse.de>
 */

#include <linux/init.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/ext4_jbd2.h>
#include <linux/ext4_fs.h>
#include "xattr.h"
#include "acl.h"

/*
 * Convert from filesystem to in-memory representation.
 */
static struct posix_acl *
ext4_acl_from_disk(const void *value, size_t size)
{
	const char *end = (char *)value + size;
	int n, count;
	struct posix_acl *acl;

	if (!value)
		return NULL;
	if (size < sizeof(ext4_acl_header))
		 return ERR_PTR(-EINVAL);
	if (((ext4_acl_header *)value)->a_version !=
	    cpu_to_le32(EXT4_ACL_VERSION))
		return ERR_PTR(-EINVAL);
	value = (char *)value + sizeof(ext4_acl_header);
	count = ext4_acl_count(size);
	if (count < 0)
		return ERR_PTR(-EINVAL);
	if (count == 0)
		return NULL;
	acl = posix_acl_alloc(count, GFP_KERNEL);
	if (!acl)
		return ERR_PTR(-ENOMEM);
	for (n=0; n < count; n++) {
		ext4_acl_entry *entry =
			(ext4_acl_entry *)value;
		if ((char *)value + sizeof(ext4_acl_entry_short) > end)
			goto fail;
		acl->a_entries[n].e_tag  = le16_to_cpu(entry->e_tag);
		acl->a_entries[n].e_perm = le16_to_cpu(entry->e_perm);
		switch(acl->a_entries[n].e_tag) {
			case ACL_USER_OBJ:
			case ACL_GROUP_OBJ:
			case ACL_MASK:
			case ACL_OTHER:
				value = (char *)value +
					sizeof(ext4_acl_entry_short);
				acl->a_entries[n].e_id = ACL_UNDEFINED_ID;
				break;

			case ACL_USER:
			case ACL_GROUP:
				value = (char *)value + sizeof(ext4_acl_entry);
				if ((char *)value > end)
					goto fail;
				acl->a_entries[n].e_id =
					le32_to_cpu(entry->e_id);
				break;

			default:
				goto fail;
		}
	}
	if (value != end)
		goto fail;
	return acl;

fail:
	posix_acl_release(acl);
	return ERR_PTR(-EINVAL);
}

/*
 * Convert from in-memory to filesystem representation.
 */
static void *
ext4_acl_to_disk(const struct posix_acl *acl, size_t *size)
{
	ext4_acl_header *ext_acl;
	char *e;
	size_t n;

	*size = ext4_acl_size(acl->a_count);
	ext_acl = kmalloc(sizeof(ext4_acl_header) + acl->a_count *
			sizeof(ext4_acl_entry), GFP_KERNEL);
	if (!ext_acl)
		return ERR_PTR(-ENOMEM);
	ext_acl->a_version = cpu_to_le32(EXT4_ACL_VERSION);
	e = (char *)ext_acl + sizeof(ext4_acl_header);
	for (n=0; n < acl->a_count; n++) {
		ext4_acl_entry *entry = (ext4_acl_entry *)e;
		entry->e_tag  = cpu_to_le16(acl->a_entries[n].e_tag);
		entry->e_perm = cpu_to_le16(acl->a_entries[n].e_perm);
		switch(acl->a_entries[n].e_tag) {
			case ACL_USER:
			case ACL_GROUP:
				entry->e_id =
					cpu_to_le32(acl->a_entries[n].e_id);
				e += sizeof(ext4_acl_entry);
				break;

			case ACL_USER_OBJ:
			case ACL_GROUP_OBJ:
			case ACL_MASK:
			case ACL_OTHER:
				e += sizeof(ext4_acl_entry_short);
				break;

			default:
				goto fail;
		}
	}
	return (char *)ext_acl;

fail:
	kfree(ext_acl);
	return ERR_PTR(-EINVAL);
}

static inline struct posix_acl *
ext4_iget_acl(struct inode *inode, struct posix_acl **i_acl)
{
	struct posix_acl *acl = EXT4_ACL_NOT_CACHED;

	spin_lock(&inode->i_lock);
	if (*i_acl != EXT4_ACL_NOT_CACHED)
		acl = posix_acl_dup(*i_acl);
	spin_unlock(&inode->i_lock);

	return acl;
}

static inline void
ext4_iset_acl(struct inode *inode, struct posix_acl **i_acl,
		struct posix_acl *acl)
{
	spin_lock(&inode->i_lock);
	if (*i_acl != EXT4_ACL_NOT_CACHED)
		posix_acl_release(*i_acl);
	*i_acl = posix_acl_dup(acl);
	spin_unlock(&inode->i_lock);
}

/*
 * Inode operation get_posix_acl().
 *
 * inode->i_mutex: don't care
 */
static struct posix_acl *
ext4_get_acl(struct inode *inode, int type)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	int name_index;
	char *value = NULL;
	struct posix_acl *acl;
	int retval;

	if (!test_opt(inode->i_sb, POSIX_ACL))
		return NULL;

	switch(type) {
		case ACL_TYPE_ACCESS:
			acl = ext4_iget_acl(inode, &ei->i_acl);
			if (acl != EXT4_ACL_NOT_CACHED)
				return acl;
			name_index = EXT4_XATTR_INDEX_POSIX_ACL_ACCESS;
			break;

		case ACL_TYPE_DEFAULT:
			acl = ext4_iget_acl(inode, &ei->i_default_acl);
			if (acl != EXT4_ACL_NOT_CACHED)
				return acl;
			name_index = EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT;
			break;

		default:
			return ERR_PTR(-EINVAL);
	}
	retval = ext4_xattr_get(inode, name_index, "", NULL, 0);
	if (retval > 0) {
		value = kmalloc(retval, GFP_KERNEL);
		if (!value)
			return ERR_PTR(-ENOMEM);
		retval = ext4_xattr_get(inode, name_index, "", value, retval);
	}
	if (retval > 0)
		acl = ext4_acl_from_disk(value, retval);
	else if (retval == -ENODATA || retval == -ENOSYS)
		acl = NULL;
	else
		acl = ERR_PTR(retval);
	kfree(value);

	if (!IS_ERR(acl)) {
		switch(type) {
			case ACL_TYPE_ACCESS:
				ext4_iset_acl(inode, &ei->i_acl, acl);
				break;

			case ACL_TYPE_DEFAULT:
				ext4_iset_acl(inode, &ei->i_default_acl, acl);
				break;
		}
	}
	return acl;
}

/*
 * Set the access or default ACL of an inode.
 *
 * inode->i_mutex: down unless called from ext4_new_inode
 */
static int
ext4_set_acl(handle_t *handle, struct inode *inode, int type,
	     struct posix_acl *acl)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	int name_index;
	void *value = NULL;
	size_t size = 0;
	int error;

	if (S_ISLNK(inode->i_mode))
		return -EOPNOTSUPP;

	switch(type) {
		case ACL_TYPE_ACCESS:
			name_index = EXT4_XATTR_INDEX_POSIX_ACL_ACCESS;
			if (acl) {
				mode_t mode = inode->i_mode;
				error = posix_acl_equiv_mode(acl, &mode);
				if (error < 0)
					return error;
				else {
					inode->i_mode = mode;
					ext4_mark_inode_dirty(handle, inode);
					if (error == 0)
						acl = NULL;
				}
			}
			break;

		case ACL_TYPE_DEFAULT:
			name_index = EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT;
			if (!S_ISDIR(inode->i_mode))
				return acl ? -EACCES : 0;
			break;

		default:
			return -EINVAL;
	}
	if (acl) {
		value = ext4_acl_to_disk(acl, &size);
		if (IS_ERR(value))
			return (int)PTR_ERR(value);
	}

	error = ext4_xattr_set_handle(handle, inode, name_index, "",
				      value, size, 0);

	kfree(value);
	if (!error) {
		switch(type) {
			case ACL_TYPE_ACCESS:
				ext4_iset_acl(inode, &ei->i_acl, acl);
				break;

			case ACL_TYPE_DEFAULT:
				ext4_iset_acl(inode, &ei->i_default_acl, acl);
				break;
		}
	}
	return error;
}

static int
ext4_check_acl(struct inode *inode, int mask)
{
	struct posix_acl *acl = ext4_get_acl(inode, ACL_TYPE_ACCESS);

	if (IS_ERR(acl))
		return PTR_ERR(acl);
	if (acl) {
		int error = posix_acl_permission(inode, acl, mask);
		posix_acl_release(acl);
		return error;
	}

	return -EAGAIN;
}

int
ext4_permission(struct inode *inode, int mask, struct nameidata *nd)
{
	return generic_permission(inode, mask, ext4_check_acl);
}

/*
 * Initialize the ACLs of a new inode. Called from ext4_new_inode.
 *
 * dir->i_mutex: down
 * inode->i_mutex: up (access to inode is still exclusive)
 */
int
ext4_init_acl(handle_t *handle, struct inode *inode, struct inode *dir)
{
	struct posix_acl *acl = NULL;
	int error = 0;

	if (!S_ISLNK(inode->i_mode)) {
		if (test_opt(dir->i_sb, POSIX_ACL)) {
			acl = ext4_get_acl(dir, ACL_TYPE_DEFAULT);
			if (IS_ERR(acl))
				return PTR_ERR(acl);
		}
		if (!acl)
			inode->i_mode &= ~current->fs->umask;
	}
	if (test_opt(inode->i_sb, POSIX_ACL) && acl) {
		struct posix_acl *clone;
		mode_t mode;

		if (S_ISDIR(inode->i_mode)) {
			error = ext4_set_acl(handle, inode,
					     ACL_TYPE_DEFAULT, acl);
			if (error)
				goto cleanup;
		}
		clone = posix_acl_clone(acl, GFP_KERNEL);
		error = -ENOMEM;
		if (!clone)
			goto cleanup;

		mode = inode->i_mode;
		error = posix_acl_create_masq(clone, &mode);
		if (error >= 0) {
			inode->i_mode = mode;
			if (error > 0) {
				/* This is an extended ACL */
				error = ext4_set_acl(handle, inode,
						     ACL_TYPE_ACCESS, clone);
			}
		}
		posix_acl_release(clone);
	}
cleanup:
	posix_acl_release(acl);
	return error;
}

/*
 * Does chmod for an inode that may have an Access Control List. The
 * inode->i_mode field must be updated to the desired value by the caller
 * before calling this function.
 * Returns 0 on success, or a negative error number.
 *
 * We change the ACL rather than storing some ACL entries in the file
 * mode permission bits (which would be more efficient), because that
 * would break once additional permissions (like  ACL_APPEND, ACL_DELETE
 * for directories) are added. There are no more bits available in the
 * file mode.
 *
 * inode->i_mutex: down
 */
int
ext4_acl_chmod(struct inode *inode)
{
	struct posix_acl *acl, *clone;
	int error;

	if (S_ISLNK(inode->i_mode))
		return -EOPNOTSUPP;
	if (!test_opt(inode->i_sb, POSIX_ACL))
		return 0;
	acl = ext4_get_acl(inode, ACL_TYPE_ACCESS);
	if (IS_ERR(acl) || !acl)
		return PTR_ERR(acl);
	clone = posix_acl_clone(acl, GFP_KERNEL);
	posix_acl_release(acl);
	if (!clone)
		return -ENOMEM;
	error = posix_acl_chmod_masq(clone, inode->i_mode);
	if (!error) {
		handle_t *handle;
		int retries = 0;

	retry:
		handle = ext4_journal_start(inode,
				EXT4_DATA_TRANS_BLOCKS(inode->i_sb));
		if (IS_ERR(handle)) {
			error = PTR_ERR(handle);
			ext4_std_error(inode->i_sb, error);
			goto out;
		}
		error = ext4_set_acl(handle, inode, ACL_TYPE_ACCESS, clone);
		ext4_journal_stop(handle);
		if (error == -ENOSPC &&
		    ext4_should_retry_alloc(inode->i_sb, &retries))
			goto retry;
	}
out:
	posix_acl_release(clone);
	return error;
}

/*
 * Extended attribute handlers
 */
static size_t
ext4_xattr_list_acl_access(struct inode *inode, char *list, size_t list_len,
			   const char *name, size_t name_len)
{
	const size_t size = sizeof(POSIX_ACL_XATTR_ACCESS);

	if (!test_opt(inode->i_sb, POSIX_ACL))
		return 0;
	if (list && size <= list_len)
		memcpy(list, POSIX_ACL_XATTR_ACCESS, size);
	return size;
}

static size_t
ext4_xattr_list_acl_default(struct inode *inode, char *list, size_t list_len,
			    const char *name, size_t name_len)
{
	const size_t size = sizeof(POSIX_ACL_XATTR_DEFAULT);

	if (!test_opt(inode->i_sb, POSIX_ACL))
		return 0;
	if (list && size <= list_len)
		memcpy(list, POSIX_ACL_XATTR_DEFAULT, size);
	return size;
}

static int
ext4_xattr_get_acl(struct inode *inode, int type, void *buffer, size_t size)
{
	struct posix_acl *acl;
	int error;

	if (!test_opt(inode->i_sb, POSIX_ACL))
		return -EOPNOTSUPP;

	acl = ext4_get_acl(inode, type);
	if (IS_ERR(acl))
		return PTR_ERR(acl);
	if (acl == NULL)
		return -ENODATA;
	error = posix_acl_to_xattr(acl, buffer, size);
	posix_acl_release(acl);

	return error;
}

static int
ext4_xattr_get_acl_access(struct inode *inode, const char *name,
			  void *buffer, size_t size)
{
	if (strcmp(name, "") != 0)
		return -EINVAL;
	return ext4_xattr_get_acl(inode, ACL_TYPE_ACCESS, buffer, size);
}

static int
ext4_xattr_get_acl_default(struct inode *inode, const char *name,
			   void *buffer, size_t size)
{
	if (strcmp(name, "") != 0)
		return -EINVAL;
	return ext4_xattr_get_acl(inode, ACL_TYPE_DEFAULT, buffer, size);
}

static int
ext4_xattr_set_acl(struct inode *inode, int type, const void *value,
		   size_t size)
{
	handle_t *handle;
	struct posix_acl *acl;
	int error, retries = 0;

	if (!test_opt(inode->i_sb, POSIX_ACL))
		return -EOPNOTSUPP;
	if (!is_owner_or_cap(inode))
		return -EPERM;

	if (value) {
		acl = posix_acl_from_xattr(value, size);
		if (IS_ERR(acl))
			return PTR_ERR(acl);
		else if (acl) {
			error = posix_acl_valid(acl);
			if (error)
				goto release_and_out;
		}
	} else
		acl = NULL;

retry:
	handle = ext4_journal_start(inode, EXT4_DATA_TRANS_BLOCKS(inode->i_sb));
	if (IS_ERR(handle))
		return PTR_ERR(handle);
	error = ext4_set_acl(handle, inode, type, acl);
	ext4_journal_stop(handle);
	if (error == -ENOSPC && ext4_should_retry_alloc(inode->i_sb, &retries))
		goto retry;

release_and_out:
	posix_acl_release(acl);
	return error;
}

static int
ext4_xattr_set_acl_access(struct inode *inode, const char *name,
			  const void *value, size_t size, int flags)
{
	if (strcmp(name, "") != 0)
		return -EINVAL;
	return ext4_xattr_set_acl(inode, ACL_TYPE_ACCESS, value, size);
}

static int
ext4_xattr_set_acl_default(struct inode *inode, const char *name,
			   const void *value, size_t size, int flags)
{
	if (strcmp(name, "") != 0)
		return -EINVAL;
	return ext4_xattr_set_acl(inode, ACL_TYPE_DEFAULT, value, size);
}

struct xattr_handler ext4_xattr_acl_access_handler = {
	.prefix	= POSIX_ACL_XATTR_ACCESS,
	.list	= ext4_xattr_list_acl_access,
	.get	= ext4_xattr_get_acl_access,
	.set	= ext4_xattr_set_acl_access,
};

struct xattr_handler ext4_xattr_acl_default_handler = {
	.prefix	= POSIX_ACL_XATTR_DEFAULT,
	.list	= ext4_xattr_list_acl_default,
	.get	= ext4_xattr_get_acl_default,
	.set	= ext4_xattr_set_acl_default,
};
