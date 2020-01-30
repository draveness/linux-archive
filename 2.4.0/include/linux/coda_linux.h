/* 
 * Coda File System, Linux Kernel module
 * 
 * Original version, adapted from cfs_mach.c, (C) Carnegie Mellon University
 * Linux modifications (C) 1996, Peter J. Braam
 * Rewritten for Linux 2.1 (C) 1997 Carnegie Mellon University
 *
 * Carnegie Mellon University encourages users of this software to
 * contribute improvements to the Coda project.
 */

#ifndef _LINUX_CODA_FS
#define _LINUX_CODA_FS

#include <linux/kernel.h>
#include <linux/param.h>
#include <linux/sched.h> 
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/malloc.h>
#include <linux/wait.h>		
#include <linux/types.h>
#include <linux/fs.h>

/* operations */
extern struct inode_operations coda_dir_inode_operations;
extern struct inode_operations coda_file_inode_operations;
extern struct inode_operations coda_ioctl_inode_operations;

extern struct address_space_operations coda_file_aops;
extern struct address_space_operations coda_symlink_aops;

extern struct file_operations coda_dir_operations;
extern struct file_operations coda_file_operations;
extern struct file_operations coda_ioctl_operations;

/* operations shared over more than one file */
int coda_open(struct inode *i, struct file *f);
int coda_release(struct inode *i, struct file *f);
int coda_permission(struct inode *inode, int mask);
int coda_revalidate_inode(struct dentry *);
int coda_notify_change(struct dentry *, struct iattr *);

/* global variables */
extern int coda_debug;
extern int coda_print_entry;
extern int coda_access_cache;

/* this file:  heloers */
static __inline__ struct ViceFid *coda_i2f(struct inode *);
static __inline__ char *coda_i2s(struct inode *);
static __inline__ void coda_flag_inode(struct inode *, int flag);
char *coda_f2s(ViceFid *f);
char *coda_f2s2(ViceFid *f);
int coda_isroot(struct inode *i);
int coda_fid_is_volroot(struct ViceFid *);
int coda_fid_is_weird(struct ViceFid *fid);
int coda_iscontrol(const char *name, size_t length);

void coda_load_creds(struct coda_cred *cred);
void coda_vattr_to_iattr(struct inode *, struct coda_vattr *);
void coda_iattr_to_vattr(struct iattr *, struct coda_vattr *);
unsigned short coda_flags_to_cflags(unsigned short);
void print_vattr( struct coda_vattr *attr );
int coda_cred_ok(struct coda_cred *cred);
int coda_cred_eq(struct coda_cred *cred1, struct coda_cred *cred2);

/* cache.c */
void coda_purge_children(struct inode *, int);
void coda_purge_dentries(struct inode *);

/* sysctl.h */
void coda_sysctl_init(void);
void coda_sysctl_clean(void);


/* debugging masks */
#define D_SUPER     1   /* print results returned by Venus */ 
#define D_INODE     2   /* print entry and exit into procedure */
#define D_FILE      4   
#define D_CACHE     8   /* cache debugging */
#define D_MALLOC    16  /* print malloc, de-alloc information */
#define D_CNODE     32
#define D_UPCALL    64  /* up and downcall debugging */
#define D_PSDEV    128  
#define D_PIOCTL   256
#define D_SPECIAL  512
#define D_TIMING  1024
#define D_DOWNCALL 2048
 
#define CDEBUG(mask, format, a...)                                \
  do {                                                            \
  if (coda_debug & mask) {                                        \
    printk("(%s,l. %d): ",  __FUNCTION__, __LINE__);              \
    printk(format, ## a); }                                       \
} while (0)

#define ENTRY    \
    if(coda_print_entry) printk("Process %d entered %s\n",current->pid,__FUNCTION__)

#define EXIT    \
    if(coda_print_entry) printk("Process %d leaving %s\n",current->pid,__FUNCTION__)

#define CODA_ALLOC(ptr, cast, size)                                       \
do {                                                                      \
    if (size < PAGE_SIZE) {                                               \
        ptr = (cast)kmalloc((unsigned long) size, GFP_KERNEL);            \
        CDEBUG(D_MALLOC, "kmalloced: %lx at %p.\n", (long)size, ptr);     \
     }  else {                                                            \
        ptr = (cast)vmalloc((unsigned long) size);                        \
	CDEBUG(D_MALLOC, "vmalloced: %lx at %p .\n", (long)size, ptr);}   \
    if (ptr == 0) {                                                       \
        printk("kernel malloc returns 0 at %s:%d\n", __FILE__, __LINE__); \
    }                                                                     \
    else memset( ptr, 0, size );                                          \
} while (0)


#define CODA_FREE(ptr,size) do {if (size < PAGE_SIZE) { kfree((ptr)); CDEBUG(D_MALLOC, "kfreed: %lx at %p.\n", (long) size, ptr); } else { vfree((ptr)); CDEBUG(D_MALLOC, "vfreed: %lx at %p.\n", (long) size, ptr);} } while (0)

/* inode to cnode access functions */

static __inline__ struct ViceFid *coda_i2f(struct inode *inode)
{
	return &(inode->u.coda_i.c_fid);
}

static __inline__ char *coda_i2s(struct inode *inode)
{
	return coda_f2s(&(inode->u.coda_i.c_fid));
}

/* this will not zap the inode away */
static __inline__ void coda_flag_inode(struct inode *inode, int flag)
{
	inode->u.coda_i.c_flags |= flag;
}		

#define ITOC(inode) (&((inode)->u.coda_i))

#endif
