/*
  File: linux/posix_acl.h

  (C) 2002 Andreas Gruenbacher, <a.gruenbacher@computer.org>
*/


#ifndef __LINUX_POSIX_ACL_H
#define __LINUX_POSIX_ACL_H

#include <linux/slab.h>

#define ACL_UNDEFINED_ID	(-1)

/* a_type field in acl_user_posix_entry_t */
#define ACL_TYPE_ACCESS		(0x8000)
#define ACL_TYPE_DEFAULT	(0x4000)

/* e_tag entry in struct posix_acl_entry */
#define ACL_USER_OBJ		(0x01)
#define ACL_USER		(0x02)
#define ACL_GROUP_OBJ		(0x04)
#define ACL_GROUP		(0x08)
#define ACL_MASK		(0x10)
#define ACL_OTHER		(0x20)

/* permissions in the e_perm field */
#define ACL_READ		(0x04)
#define ACL_WRITE		(0x02)
#define ACL_EXECUTE		(0x01)
//#define ACL_ADD		(0x08)
//#define ACL_DELETE		(0x10)

struct posix_acl_entry {
	short			e_tag;
	unsigned short		e_perm;
	unsigned int		e_id;
};

struct posix_acl {
	atomic_t		a_refcount;
	unsigned int		a_count;
	struct posix_acl_entry	a_entries[0];
};

#define FOREACH_ACL_ENTRY(pa, acl, pe) \
	for(pa=(acl)->a_entries, pe=pa+(acl)->a_count; pa<pe; pa++)


/*
 * Duplicate an ACL handle.
 */
static inline struct posix_acl *
posix_acl_dup(struct posix_acl *acl)
{
	if (acl)
		atomic_inc(&acl->a_refcount);
	return acl;
}

/*
 * Free an ACL handle.
 */
static inline void
posix_acl_release(struct posix_acl *acl)
{
	if (acl && atomic_dec_and_test(&acl->a_refcount))
		kfree(acl);
}


/* posix_acl.c */

extern struct posix_acl *posix_acl_alloc(int, int);
extern struct posix_acl *posix_acl_clone(const struct posix_acl *, int);
extern int posix_acl_valid(const struct posix_acl *);
extern int posix_acl_permission(struct inode *, const struct posix_acl *, int);
extern struct posix_acl *posix_acl_from_mode(mode_t, int);
extern int posix_acl_equiv_mode(const struct posix_acl *, mode_t *);
extern int posix_acl_create_masq(struct posix_acl *, mode_t *);
extern int posix_acl_chmod_masq(struct posix_acl *, mode_t);
extern int posix_acl_masq_nfs_mode(struct posix_acl *, mode_t *);

extern struct posix_acl *get_posix_acl(struct inode *, int);
extern int set_posix_acl(struct inode *, int, struct posix_acl *);

#endif  /* __LINUX_POSIX_ACL_H */
