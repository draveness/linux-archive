#include <linux/init.h>
#include <linux/posix_acl.h>
#include <linux/xattr_acl.h>

#define REISERFS_ACL_VERSION	0x0001

typedef struct {
	__u16		e_tag;
	__u16		e_perm;
	__u32		e_id;
} reiserfs_acl_entry;

typedef struct {
	__u16		e_tag;
	__u16		e_perm;
} reiserfs_acl_entry_short;

typedef struct {
	__u32		a_version;
} reiserfs_acl_header;

static inline size_t reiserfs_acl_size(int count)
{
	if (count <= 4) {
		return sizeof(reiserfs_acl_header) +
		       count * sizeof(reiserfs_acl_entry_short);
	} else {
		return sizeof(reiserfs_acl_header) +
		       4 * sizeof(reiserfs_acl_entry_short) +
		       (count - 4) * sizeof(reiserfs_acl_entry);
	}
}

static inline int reiserfs_acl_count(size_t size)
{
	ssize_t s;
	size -= sizeof(reiserfs_acl_header);
	s = size - 4 * sizeof(reiserfs_acl_entry_short);
	if (s < 0) {
		if (size % sizeof(reiserfs_acl_entry_short))
			return -1;
		return size / sizeof(reiserfs_acl_entry_short);
	} else {
		if (s % sizeof(reiserfs_acl_entry))
			return -1;
		return s / sizeof(reiserfs_acl_entry) + 4;
	}
}


#ifdef CONFIG_REISERFS_FS_POSIX_ACL
struct posix_acl * reiserfs_get_acl(struct inode *inode, int type);
int reiserfs_set_acl(struct inode *inode, int type, struct posix_acl *acl);
int reiserfs_acl_chmod (struct inode *inode);
int reiserfs_inherit_default_acl (struct inode *dir, struct dentry *dentry, struct inode *inode);
int reiserfs_cache_default_acl (struct inode *dir);
extern int reiserfs_xattr_posix_acl_init (void) __init;
extern int reiserfs_xattr_posix_acl_exit (void);
extern struct reiserfs_xattr_handler posix_acl_default_handler;
extern struct reiserfs_xattr_handler posix_acl_access_handler;
#else

#define reiserfs_set_acl NULL
#define reiserfs_get_acl NULL
#define reiserfs_cache_default_acl(inode) 0

static inline int
reiserfs_xattr_posix_acl_init (void)
{
    return 0;
}

static inline int
reiserfs_xattr_posix_acl_exit (void)
{
    return 0;
}

static inline int
reiserfs_acl_chmod (struct inode *inode)
{
    return 0;
}

static inline int
reiserfs_inherit_default_acl (const struct inode *dir, struct dentry *dentry, struct inode *inode)
{
    return 0;
}

#endif
