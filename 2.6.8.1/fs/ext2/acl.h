/*
  File: fs/ext2/acl.h

  (C) 2001 Andreas Gruenbacher, <a.gruenbacher@computer.org>
*/

#include <linux/xattr_acl.h>

#define EXT2_ACL_VERSION	0x0001
#define EXT2_ACL_MAX_ENTRIES	32

typedef struct {
	__u16		e_tag;
	__u16		e_perm;
	__u32		e_id;
} ext2_acl_entry;

typedef struct {
	__u16		e_tag;
	__u16		e_perm;
} ext2_acl_entry_short;

typedef struct {
	__u32		a_version;
} ext2_acl_header;

static inline size_t ext2_acl_size(int count)
{
	if (count <= 4) {
		return sizeof(ext2_acl_header) +
		       count * sizeof(ext2_acl_entry_short);
	} else {
		return sizeof(ext2_acl_header) +
		       4 * sizeof(ext2_acl_entry_short) +
		       (count - 4) * sizeof(ext2_acl_entry);
	}
}

static inline int ext2_acl_count(size_t size)
{
	ssize_t s;
	size -= sizeof(ext2_acl_header);
	s = size - 4 * sizeof(ext2_acl_entry_short);
	if (s < 0) {
		if (size % sizeof(ext2_acl_entry_short))
			return -1;
		return size / sizeof(ext2_acl_entry_short);
	} else {
		if (s % sizeof(ext2_acl_entry))
			return -1;
		return s / sizeof(ext2_acl_entry) + 4;
	}
}

#ifdef CONFIG_EXT2_FS_POSIX_ACL

/* Value for inode->u.ext2_i.i_acl and inode->u.ext2_i.i_default_acl
   if the ACL has not been cached */
#define EXT2_ACL_NOT_CACHED ((void *)-1)

/* acl.c */
extern int ext2_permission (struct inode *, int, struct nameidata *);
extern int ext2_acl_chmod (struct inode *);
extern int ext2_init_acl (struct inode *, struct inode *);

extern int init_ext2_acl(void);
extern void exit_ext2_acl(void);

#else
#include <linux/sched.h>
#define ext2_permission NULL
#define ext2_get_acl	NULL
#define ext2_set_acl	NULL

static inline int
ext2_acl_chmod (struct inode *inode)
{
	return 0;
}

static inline int ext2_init_acl (struct inode *inode, struct inode *dir)
{
	inode->i_mode &= ~current->fs->umask;
	return 0;
}
#endif

