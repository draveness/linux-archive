/*
 * Mostly platform independent upcall operations to Venus:
 *  -- upcalls
 *  -- upcall routines
 *
 * Linux 2.0 version
 * Copyright (C) 1996 Peter J. Braam <braam@maths.ox.ac.uk>, 
 * Michael Callahan <callahan@maths.ox.ac.uk> 
 * 
 * Redone for Linux 2.1
 * Copyright (C) 1997 Carnegie Mellon University
 *
 * Carnegie Mellon University encourages users of this code to contribute
 * improvements to the Coda project. Contact Peter Braam <coda@cs.cmu.edu>.
 */

#include <asm/system.h>
#include <asm/signal.h>
#include <linux/signal.h>

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/time.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <asm/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/vfs.h>

#include <linux/coda.h>
#include <linux/coda_linux.h>
#include <linux/coda_psdev.h>
#include <linux/coda_fs_i.h>
#include <linux/coda_cache.h>
#include <linux/coda_proc.h> 

#define upc_alloc() kmalloc(sizeof(struct upc_req), GFP_KERNEL)
#define upc_free(r) kfree(r)

static int coda_upcall(struct coda_sb_info *mntinfo, int inSize, int *outSize, 
		       union inputArgs *buffer);

static void *alloc_upcall(int opcode, int size)
{
	union inputArgs *inp;

	CODA_ALLOC(inp, union inputArgs *, size);
        if (!inp)
		return ERR_PTR(-ENOMEM);

        inp->ih.opcode = opcode;
	inp->ih.pid = current->pid;
	inp->ih.pgid = process_group(current);
#ifdef CODA_FS_OLD_API
	memset(&inp->ih.cred, 0, sizeof(struct coda_cred));
	inp->ih.cred.cr_fsuid = current->fsuid;
#else
	inp->ih.uid = current->fsuid;
#endif
	return (void*)inp;
}

#define UPARG(op)\
do {\
	inp = (union inputArgs *)alloc_upcall(op, insize); \
        if (IS_ERR(inp)) { return PTR_ERR(inp); }\
        outp = (union outputArgs *)(inp); \
        outsize = insize; \
} while (0)

#define INSIZE(tag) sizeof(struct coda_ ## tag ## _in)
#define OUTSIZE(tag) sizeof(struct coda_ ## tag ## _out)
#define SIZE(tag)  max_t(unsigned int, INSIZE(tag), OUTSIZE(tag))


/* the upcalls */
int venus_rootfid(struct super_block *sb, struct CodaFid *fidp)
{
        union inputArgs *inp;
        union outputArgs *outp;
        int insize, outsize, error;

        insize = SIZE(root);
        UPARG(CODA_ROOT);

	error = coda_upcall(coda_sbp(sb), insize, &outsize, inp);
	
	if (error) {
	        printk("coda_get_rootfid: error %d\n", error);
	} else {
		*fidp = outp->coda_root.VFid;
	}

	CODA_FREE(inp, insize);
	return error;
}

int venus_getattr(struct super_block *sb, struct CodaFid *fid, 
		     struct coda_vattr *attr) 
{
        union inputArgs *inp;
        union outputArgs *outp;
        int insize, outsize, error;

        insize = SIZE(getattr); 
	UPARG(CODA_GETATTR);
        inp->coda_getattr.VFid = *fid;

        error = coda_upcall(coda_sbp(sb), insize, &outsize, inp);
	
	*attr = outp->coda_getattr.attr;

	CODA_FREE(inp, insize);
        return error;
}

int venus_setattr(struct super_block *sb, struct CodaFid *fid, 
		  struct coda_vattr *vattr)
{
        union inputArgs *inp;
        union outputArgs *outp;
        int insize, outsize, error;
	
	insize = SIZE(setattr);
	UPARG(CODA_SETATTR);

        inp->coda_setattr.VFid = *fid;
	inp->coda_setattr.attr = *vattr;

        error = coda_upcall(coda_sbp(sb), insize, &outsize, inp);

        CODA_FREE(inp, insize);
        return error;
}

int venus_lookup(struct super_block *sb, struct CodaFid *fid, 
		    const char *name, int length, int * type, 
		    struct CodaFid *resfid)
{
        union inputArgs *inp;
        union outputArgs *outp;
        int insize, outsize, error;
	int offset;

	offset = INSIZE(lookup);
        insize = max_t(unsigned int, offset + length +1, OUTSIZE(lookup));
	UPARG(CODA_LOOKUP);

        inp->coda_lookup.VFid = *fid;
	inp->coda_lookup.name = offset;
	inp->coda_lookup.flags = CLU_CASE_SENSITIVE;
        /* send Venus a null terminated string */
        memcpy((char *)(inp) + offset, name, length);
        *((char *)inp + offset + length) = '\0';

        error = coda_upcall(coda_sbp(sb), insize, &outsize, inp);

	*resfid = outp->coda_lookup.VFid;
	*type = outp->coda_lookup.vtype;

	CODA_FREE(inp, insize);
	return error;
}

int venus_store(struct super_block *sb, struct CodaFid *fid, int flags,
                vuid_t uid)
{
        union inputArgs *inp;
        union outputArgs *outp;
        int insize, outsize, error;
#ifdef CODA_FS_OLD_API
	struct coda_cred cred = { 0, };
	cred.cr_fsuid = uid;
#endif
	
	insize = SIZE(store);
	UPARG(CODA_STORE);
	
#ifdef CODA_FS_OLD_API
	memcpy(&(inp->ih.cred), &cred, sizeof(cred));
#else
	inp->ih.uid = uid;
#endif
	
        inp->coda_store.VFid = *fid;
        inp->coda_store.flags = flags;

        error = coda_upcall(coda_sbp(sb), insize, &outsize, inp);

	CODA_FREE(inp, insize);
        return error;
}

int venus_release(struct super_block *sb, struct CodaFid *fid, int flags)
{
        union inputArgs *inp;
        union outputArgs *outp;
        int insize, outsize, error;
	
	insize = SIZE(release);
	UPARG(CODA_RELEASE);
	
	inp->coda_release.VFid = *fid;
	inp->coda_release.flags = flags;

	error = coda_upcall(coda_sbp(sb), insize, &outsize, inp);

	CODA_FREE(inp, insize);
	return error;
}

int venus_close(struct super_block *sb, struct CodaFid *fid, int flags,
                vuid_t uid)
{
	union inputArgs *inp;
	union outputArgs *outp;
	int insize, outsize, error;
#ifdef CODA_FS_OLD_API
	struct coda_cred cred = { 0, };
	cred.cr_fsuid = uid;
#endif
	
	insize = SIZE(release);
	UPARG(CODA_CLOSE);
	
#ifdef CODA_FS_OLD_API
	memcpy(&(inp->ih.cred), &cred, sizeof(cred));
#else
	inp->ih.uid = uid;
#endif
	
        inp->coda_close.VFid = *fid;
        inp->coda_close.flags = flags;

        error = coda_upcall(coda_sbp(sb), insize, &outsize, inp);

	CODA_FREE(inp, insize);
        return error;
}

int venus_open(struct super_block *sb, struct CodaFid *fid,
		  int flags, struct file **fh)
{
        union inputArgs *inp;
        union outputArgs *outp;
        int insize, outsize, error;
       
	insize = SIZE(open_by_fd);
	UPARG(CODA_OPEN_BY_FD);

        inp->coda_open.VFid = *fid;
        inp->coda_open.flags = flags;

        error = coda_upcall(coda_sbp(sb), insize, &outsize, inp);

	*fh = outp->coda_open_by_fd.fh;

	CODA_FREE(inp, insize);
	return error;
}	

int venus_mkdir(struct super_block *sb, struct CodaFid *dirfid, 
		   const char *name, int length, 
		   struct CodaFid *newfid, struct coda_vattr *attrs)
{
        union inputArgs *inp;
        union outputArgs *outp;
        int insize, outsize, error;
        int offset;

	offset = INSIZE(mkdir);
	insize = max_t(unsigned int, offset + length + 1, OUTSIZE(mkdir));
	UPARG(CODA_MKDIR);

        inp->coda_mkdir.VFid = *dirfid;
        inp->coda_mkdir.attr = *attrs;
	inp->coda_mkdir.name = offset;
        /* Venus must get null terminated string */
        memcpy((char *)(inp) + offset, name, length);
        *((char *)inp + offset + length) = '\0';
        
        error = coda_upcall(coda_sbp(sb), insize, &outsize, inp);

	*attrs = outp->coda_mkdir.attr;
	*newfid = outp->coda_mkdir.VFid;

	CODA_FREE(inp, insize);
	return error;        
}


int venus_rename(struct super_block *sb, struct CodaFid *old_fid, 
		 struct CodaFid *new_fid, size_t old_length, 
		 size_t new_length, const char *old_name, 
		 const char *new_name)
{
	union inputArgs *inp;
        union outputArgs *outp;
        int insize, outsize, error; 
	int offset, s;
	
	offset = INSIZE(rename);
	insize = max_t(unsigned int, offset + new_length + old_length + 8,
		     OUTSIZE(rename)); 
 	UPARG(CODA_RENAME);

        inp->coda_rename.sourceFid = *old_fid;
        inp->coda_rename.destFid =  *new_fid;
        inp->coda_rename.srcname = offset;

        /* Venus must receive an null terminated string */
        s = ( old_length & ~0x3) +4; /* round up to word boundary */
        memcpy((char *)(inp) + offset, old_name, old_length);
        *((char *)inp + offset + old_length) = '\0';

        /* another null terminated string for Venus */
        offset += s;
        inp->coda_rename.destname = offset;
        s = ( new_length & ~0x3) +4; /* round up to word boundary */
        memcpy((char *)(inp) + offset, new_name, new_length);
        *((char *)inp + offset + new_length) = '\0';

        error = coda_upcall(coda_sbp(sb), insize, &outsize, inp);

	CODA_FREE(inp, insize);
	return error;
}

int venus_create(struct super_block *sb, struct CodaFid *dirfid, 
		 const char *name, int length, int excl, int mode, dev_t rdev,
		 struct CodaFid *newfid, struct coda_vattr *attrs) 
{
        union inputArgs *inp;
        union outputArgs *outp;
        int insize, outsize, error;
        int offset;

        offset = INSIZE(create);
	insize = max_t(unsigned int, offset + length + 1, OUTSIZE(create));
	UPARG(CODA_CREATE);

        inp->coda_create.VFid = *dirfid;
        inp->coda_create.attr.va_mode = mode;
        inp->coda_create.attr.va_rdev = huge_encode_dev(rdev);
	inp->coda_create.excl = excl;
        inp->coda_create.mode = mode;
        inp->coda_create.name = offset;

        /* Venus must get null terminated string */
        memcpy((char *)(inp) + offset, name, length);
        *((char *)inp + offset + length) = '\0';
                
        error = coda_upcall(coda_sbp(sb), insize, &outsize, inp);

	*attrs = outp->coda_create.attr;
	*newfid = outp->coda_create.VFid;

	CODA_FREE(inp, insize);
	return error;        
}

int venus_rmdir(struct super_block *sb, struct CodaFid *dirfid, 
		    const char *name, int length)
{
        union inputArgs *inp;
        union outputArgs *outp;
        int insize, outsize, error;
        int offset;

        offset = INSIZE(rmdir);
	insize = max_t(unsigned int, offset + length + 1, OUTSIZE(rmdir));
	UPARG(CODA_RMDIR);

        inp->coda_rmdir.VFid = *dirfid;
        inp->coda_rmdir.name = offset;
        memcpy((char *)(inp) + offset, name, length);
	*((char *)inp + offset + length) = '\0';
        
        error = coda_upcall(coda_sbp(sb), insize, &outsize, inp);

	CODA_FREE(inp, insize);
	return error;
}

int venus_remove(struct super_block *sb, struct CodaFid *dirfid, 
		    const char *name, int length)
{
        union inputArgs *inp;
        union outputArgs *outp;
        int error=0, insize, outsize, offset;

        offset = INSIZE(remove);
	insize = max_t(unsigned int, offset + length + 1, OUTSIZE(remove));
	UPARG(CODA_REMOVE);

        inp->coda_remove.VFid = *dirfid;
        inp->coda_remove.name = offset;
        memcpy((char *)(inp) + offset, name, length);
	*((char *)inp + offset + length) = '\0';
        
        error = coda_upcall(coda_sbp(sb), insize, &outsize, inp);

	CODA_FREE(inp, insize);
	return error;
}

int venus_readlink(struct super_block *sb, struct CodaFid *fid, 
		      char *buffer, int *length)
{ 
        union inputArgs *inp;
        union outputArgs *outp;
        int insize, outsize, error;
        int retlen;
        char *result;
        
	insize = max_t(unsigned int,
		     INSIZE(readlink), OUTSIZE(readlink)+ *length + 1);
	UPARG(CODA_READLINK);

        inp->coda_readlink.VFid = *fid;
    
        error =  coda_upcall(coda_sbp(sb), insize, &outsize, inp);
	
	if (! error) {
                retlen = outp->coda_readlink.count;
		if ( retlen > *length )
		        retlen = *length;
		*length = retlen;
		result =  (char *)outp + (long)outp->coda_readlink.data;
		memcpy(buffer, result, retlen);
		*(buffer + retlen) = '\0';
	}
        
        CODA_FREE(inp, insize);
        return error;
}



int venus_link(struct super_block *sb, struct CodaFid *fid, 
		  struct CodaFid *dirfid, const char *name, int len )
{
        union inputArgs *inp;
        union outputArgs *outp;
        int insize, outsize, error;
        int offset;

	offset = INSIZE(link);
	insize = max_t(unsigned int, offset  + len + 1, OUTSIZE(link));
        UPARG(CODA_LINK);

        inp->coda_link.sourceFid = *fid;
        inp->coda_link.destFid = *dirfid;
        inp->coda_link.tname = offset;

        /* make sure strings are null terminated */
        memcpy((char *)(inp) + offset, name, len);
        *((char *)inp + offset + len) = '\0';
        
        error = coda_upcall(coda_sbp(sb), insize, &outsize, inp);

	CODA_FREE(inp, insize);
        return error;
}

int venus_symlink(struct super_block *sb, struct CodaFid *fid,
		     const char *name, int len,
		     const char *symname, int symlen)
{
        union inputArgs *inp;
        union outputArgs *outp;
        int insize, outsize, error;
        int offset, s;

        offset = INSIZE(symlink);
	insize = max_t(unsigned int, offset + len + symlen + 8, OUTSIZE(symlink));
	UPARG(CODA_SYMLINK);
        
        /*        inp->coda_symlink.attr = *tva; XXXXXX */ 
        inp->coda_symlink.VFid = *fid;

	/* Round up to word boundary and null terminate */
        inp->coda_symlink.srcname = offset;
        s = ( symlen  & ~0x3 ) + 4; 
        memcpy((char *)(inp) + offset, symname, symlen);
        *((char *)inp + offset + symlen) = '\0';
        
	/* Round up to word boundary and null terminate */
        offset += s;
        inp->coda_symlink.tname = offset;
        s = (len & ~0x3) + 4;
        memcpy((char *)(inp) + offset, name, len);
        *((char *)inp + offset + len) = '\0';

	error = coda_upcall(coda_sbp(sb), insize, &outsize, inp);

	CODA_FREE(inp, insize);
        return error;
}

int venus_fsync(struct super_block *sb, struct CodaFid *fid)
{
        union inputArgs *inp;
        union outputArgs *outp; 
	int insize, outsize, error;
	
	insize=SIZE(fsync);
	UPARG(CODA_FSYNC);

        inp->coda_fsync.VFid = *fid;
        error = coda_upcall(coda_sbp(sb), sizeof(union inputArgs), 
                            &outsize, inp);

	CODA_FREE(inp, insize);
	return error;
}

int venus_access(struct super_block *sb, struct CodaFid *fid, int mask)
{
        union inputArgs *inp;
        union outputArgs *outp; 
	int insize, outsize, error;

	insize = SIZE(access);
	UPARG(CODA_ACCESS);

        inp->coda_access.VFid = *fid;
        inp->coda_access.flags = mask;

	error = coda_upcall(coda_sbp(sb), insize, &outsize, inp);

	CODA_FREE(inp, insize);
	return error;
}


int venus_pioctl(struct super_block *sb, struct CodaFid *fid,
		 unsigned int cmd, struct PioctlData *data)
{
        union inputArgs *inp;
        union outputArgs *outp;  
	int insize, outsize, error;
	int iocsize;

	insize = VC_MAXMSGSIZE;
	UPARG(CODA_IOCTL);

        /* build packet for Venus */
        if (data->vi.in_size > VC_MAXDATASIZE) {
		error = -EINVAL;
		goto exit;
        }

        inp->coda_ioctl.VFid = *fid;
    
        /* the cmd field was mutated by increasing its size field to
         * reflect the path and follow args. We need to subtract that
         * out before sending the command to Venus.  */
        inp->coda_ioctl.cmd = (cmd & ~(PIOCPARM_MASK << 16));	
        iocsize = ((cmd >> 16) & PIOCPARM_MASK) - sizeof(char *) - sizeof(int);
        inp->coda_ioctl.cmd |= (iocsize & PIOCPARM_MASK) <<	16;	
    
        /* in->coda_ioctl.rwflag = flag; */
        inp->coda_ioctl.len = data->vi.in_size;
        inp->coda_ioctl.data = (char *)(INSIZE(ioctl));
     
        /* get the data out of user space */
        if ( copy_from_user((char*)inp + (long)inp->coda_ioctl.data,
			    data->vi.in, data->vi.in_size) ) {
		error = -EINVAL;
	        goto exit;
	}

        error = coda_upcall(coda_sbp(sb), SIZE(ioctl) + data->vi.in_size,
                            &outsize, inp);
        
        if (error) {
	        printk("coda_pioctl: Venus returns: %d for %s\n", 
		       error, coda_f2s(fid));
		goto exit; 
	}
        
	/* Copy out the OUT buffer. */
        if (outp->coda_ioctl.len > data->vi.out_size) {
		error = -EINVAL;
        } else {
		if (copy_to_user(data->vi.out, 
				 (char *)outp + (long)outp->coda_ioctl.data, 
				 data->vi.out_size)) {
			error = -EFAULT;
			goto exit;
		}
        }

 exit:
	CODA_FREE(inp, insize);
	return error;
}

int venus_statfs(struct super_block *sb, struct kstatfs *sfs) 
{ 
        union inputArgs *inp;
        union outputArgs *outp;
        int insize, outsize, error;
        
	insize = max_t(unsigned int, INSIZE(statfs), OUTSIZE(statfs));
	UPARG(CODA_STATFS);

        error = coda_upcall(coda_sbp(sb), insize, &outsize, inp);
	
        if (!error) {
		sfs->f_blocks = outp->coda_statfs.stat.f_blocks;
		sfs->f_bfree  = outp->coda_statfs.stat.f_bfree;
		sfs->f_bavail = outp->coda_statfs.stat.f_bavail;
		sfs->f_files  = outp->coda_statfs.stat.f_files;
		sfs->f_ffree  = outp->coda_statfs.stat.f_ffree;
	} else {
		printk("coda_statfs: Venus returns: %d\n", error);
	}

        CODA_FREE(inp, insize);
        return error;
}

/*
 * coda_upcall and coda_downcall routines.
 * 
 */

static inline void coda_waitfor_upcall(struct upc_req *vmp,
				       struct venus_comm *vcommp)
{
	DECLARE_WAITQUEUE(wait, current);

	vmp->uc_posttime = jiffies;

	add_wait_queue(&vmp->uc_sleep, &wait);
	for (;;) {
		if ( !coda_hard && vmp->uc_opcode != CODA_CLOSE ) 
			set_current_state(TASK_INTERRUPTIBLE);
		else
			set_current_state(TASK_UNINTERRUPTIBLE);

                /* venus died */
                if ( !vcommp->vc_inuse )
                        break;

		/* got a reply */
		if ( vmp->uc_flags & ( REQ_WRITE | REQ_ABORT ) )
			break;

		if ( !coda_hard && vmp->uc_opcode != CODA_CLOSE && signal_pending(current) ) {
			/* if this process really wants to die, let it go */
			if ( sigismember(&(current->pending.signal), SIGKILL) ||
			     sigismember(&(current->pending.signal), SIGINT) )
				break;
			/* signal is present: after timeout always return 
			   really smart idea, probably useless ... */
			if ( jiffies - vmp->uc_posttime > coda_timeout * HZ )
				break; 
		}
		schedule();
	}
	remove_wait_queue(&vmp->uc_sleep, &wait);
	set_current_state(TASK_RUNNING);

	return;
}


/* 
 * coda_upcall will return an error in the case of 
 * failed communication with Venus _or_ will peek at Venus
 * reply and return Venus' error.
 *
 * As venus has 2 types of errors, normal errors (positive) and internal
 * errors (negative), normal errors are negated, while internal errors
 * are all mapped to -EINTR, while showing a nice warning message. (jh)
 * 
 */
static int coda_upcall(struct coda_sb_info *sbi, 
		int inSize, int *outSize, 
		union inputArgs *buffer) 
{
	struct venus_comm *vcommp;
	union outputArgs *out;
	struct upc_req *req;
	int error = 0;

	vcommp = sbi->sbi_vcomm;
	if ( !vcommp->vc_inuse ) {
		printk("No pseudo device in upcall comms at %p\n", vcommp);
                return -ENXIO;
	}

	/* Format the request message. */
	req = upc_alloc();
	if (!req) {
		printk("Failed to allocate upc_req structure\n");
		return -ENOMEM;
	}
	req->uc_data = (void *)buffer;
	req->uc_flags = 0;
	req->uc_inSize = inSize;
	req->uc_outSize = *outSize ? *outSize : inSize;
	req->uc_opcode = ((union inputArgs *)buffer)->ih.opcode;
	req->uc_unique = ++vcommp->vc_seq;
	init_waitqueue_head(&req->uc_sleep);
	
	/* Fill in the common input args. */
	((union inputArgs *)buffer)->ih.unique = req->uc_unique;

	/* Append msg to pending queue and poke Venus. */
	list_add(&(req->uc_chain), vcommp->vc_pending.prev);
        
	wake_up_interruptible(&vcommp->vc_waitq);
	/* We can be interrupted while we wait for Venus to process
	 * our request.  If the interrupt occurs before Venus has read
	 * the request, we dequeue and return. If it occurs after the
	 * read but before the reply, we dequeue, send a signal
	 * message, and return. If it occurs after the reply we ignore
	 * it. In no case do we want to restart the syscall.  If it
	 * was interrupted by a venus shutdown (psdev_close), return
	 * ENODEV.  */

	/* Go to sleep.  Wake up on signals only after the timeout. */
	coda_waitfor_upcall(req, vcommp);

	if (vcommp->vc_inuse) {      /* i.e. Venus is still alive */
	    /* Op went through, interrupt or not... */
	    if (req->uc_flags & REQ_WRITE) {
		out = (union outputArgs *)req->uc_data;
		/* here we map positive Venus errors to kernel errors */
		error = -out->oh.result;
		*outSize = req->uc_outSize;
		goto exit;
	    }
	    if ( !(req->uc_flags & REQ_READ) && signal_pending(current)) { 
		/* Interrupted before venus read it. */
		list_del(&(req->uc_chain));
		/* perhaps the best way to convince the app to
		   give up? */
		error = -EINTR;
		goto exit;
	    } 
	    if ( (req->uc_flags & REQ_READ) && signal_pending(current) ) {
		    /* interrupted after Venus did its read, send signal */
		    union inputArgs *sig_inputArgs;
		    struct upc_req *sig_req;
		    
		    list_del(&(req->uc_chain));
		    error = -ENOMEM;
		    sig_req = upc_alloc();
		    if (!sig_req) goto exit;

		    CODA_ALLOC((sig_req->uc_data), char *, sizeof(struct coda_in_hdr));
		    if (!sig_req->uc_data) {
			upc_free(sig_req);
			goto exit;
		    }
		    
		    error = -EINTR;
		    sig_inputArgs = (union inputArgs *)sig_req->uc_data;
		    sig_inputArgs->ih.opcode = CODA_SIGNAL;
		    sig_inputArgs->ih.unique = req->uc_unique;
		    
		    sig_req->uc_flags = REQ_ASYNC;
		    sig_req->uc_opcode = sig_inputArgs->ih.opcode;
		    sig_req->uc_unique = sig_inputArgs->ih.unique;
		    sig_req->uc_inSize = sizeof(struct coda_in_hdr);
		    sig_req->uc_outSize = sizeof(struct coda_in_hdr);
		    
		    /* insert at head of queue! */
		    list_add(&(sig_req->uc_chain), &vcommp->vc_pending);
		    wake_up_interruptible(&vcommp->vc_waitq);
	    } else {
		    printk("Coda: Strange interruption..\n");
		    error = -EINTR;
	    }
	} else {	/* If venus died i.e. !VC_OPEN(vcommp) */
	        printk("coda_upcall: Venus dead on (op,un) (%d.%d) flags %d\n",
		       req->uc_opcode, req->uc_unique, req->uc_flags);
		error = -ENODEV;
	}

 exit:
	upc_free(req);
	return error;
}

/*  
    The statements below are part of the Coda opportunistic
    programming -- taken from the Mach/BSD kernel code for Coda. 
    You don't get correct semantics by stating what needs to be
    done without guaranteeing the invariants needed for it to happen.
    When will be have time to find out what exactly is going on?  (pjb)
*/


/* 
 * There are 7 cases where cache invalidations occur.  The semantics
 *  of each is listed here:
 *
 * CODA_FLUSH     -- flush all entries from the name cache and the cnode cache.
 * CODA_PURGEUSER -- flush all entries from the name cache for a specific user
 *                  This call is a result of token expiration.
 *
 * The next arise as the result of callbacks on a file or directory.
 * CODA_ZAPFILE   -- flush the cached attributes for a file.

 * CODA_ZAPDIR    -- flush the attributes for the dir and
 *                  force a new lookup for all the children
                    of this dir.

 *
 * The next is a result of Venus detecting an inconsistent file.
 * CODA_PURGEFID  -- flush the attribute for the file
 *                  purge it and its children from the dcache
 *
 * The last  allows Venus to replace local fids with global ones
 * during reintegration.
 *
 * CODA_REPLACE -- replace one CodaFid with another throughout the name cache */

int coda_downcall(int opcode, union outputArgs * out, struct super_block *sb)
{
	/* Handle invalidation requests. */
          if ( !sb || !sb->s_root || !sb->s_root->d_inode)
		  return 0; 

	  switch (opcode) {

	  case CODA_FLUSH : {
		   coda_cache_clear_all(sb);
		   shrink_dcache_sb(sb);
		   coda_flag_inode(sb->s_root->d_inode, C_FLUSH);
		   return(0);
	  }

	  case CODA_PURGEUSER : {
		   coda_cache_clear_all(sb);
		   return(0);
	  }

	  case CODA_ZAPDIR : {
	          struct inode *inode;
		  struct CodaFid *fid = &out->coda_zapdir.CodaFid;

		  inode = coda_fid_to_inode(fid, sb);
		  if (inode) {
			  coda_flag_inode_children(inode, C_PURGE);
	                  coda_flag_inode(inode, C_VATTR);
			  iput(inode);
		  }
		  
		  return(0);
	  }

	  case CODA_ZAPFILE : {
	          struct inode *inode;
		  struct CodaFid *fid = &out->coda_zapfile.CodaFid;
		  inode = coda_fid_to_inode(fid, sb);
		  if ( inode ) {
	                  coda_flag_inode(inode, C_VATTR);
			  iput(inode);
		  }
		  return 0;
	  }

	  case CODA_PURGEFID : {
	          struct inode *inode;
		  struct CodaFid *fid = &out->coda_purgefid.CodaFid;
		  inode = coda_fid_to_inode(fid, sb);
		  if ( inode ) { 
			coda_flag_inode_children(inode, C_PURGE);

			/* catch the dentries later if some are still busy */
			coda_flag_inode(inode, C_PURGE);
			d_prune_aliases(inode);

			iput(inode);
		  }
		  return 0;
	  }

	  case CODA_REPLACE : {
	          struct inode *inode;
		  struct CodaFid *oldfid = &out->coda_replace.OldFid;
		  struct CodaFid *newfid = &out->coda_replace.NewFid;
		  inode = coda_fid_to_inode(oldfid, sb);
		  if ( inode ) { 
			  coda_replace_fid(inode, oldfid, newfid);
			  iput(inode);
		  }
		  return 0;
	  }
	  }
	  return 0;
}

