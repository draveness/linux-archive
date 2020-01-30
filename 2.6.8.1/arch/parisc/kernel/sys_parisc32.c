/*
 * sys_parisc32.c: Conversion between 32bit and 64bit native syscalls.
 *
 * Copyright (C) 2000-2001 Hewlett Packard Company
 * Copyright (C) 2000 John Marvin
 * Copyright (C) 2001 Matthew Wilcox
 *
 * These routines maintain argument size conversion between 32bit and 64bit
 * environment. Based heavily on sys_ia32.c and sys_sparc32.c.
 */

#include <linux/config.h>
#include <linux/compat.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/fs.h> 
#include <linux/mm.h> 
#include <linux/file.h> 
#include <linux/signal.h>
#include <linux/resource.h>
#include <linux/times.h>
#include <linux/utsname.h>
#include <linux/time.h>
#include <linux/timex.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/sem.h>
#include <linux/msg.h>
#include <linux/shm.h>
#include <linux/slab.h>
#include <linux/uio.h>
#include <linux/nfs_fs.h>
#include <linux/ncp_fs.h>
#include <linux/sunrpc/svc.h>
#include <linux/nfsd/nfsd.h>
#include <linux/nfsd/cache.h>
#include <linux/nfsd/xdr.h>
#include <linux/nfsd/syscall.h>
#include <linux/poll.h>
#include <linux/personality.h>
#include <linux/stat.h>
#include <linux/highmem.h>
#include <linux/highuid.h>
#include <linux/mman.h>
#include <linux/binfmts.h>
#include <linux/namei.h>
#include <linux/vfs.h>
#include <linux/ptrace.h>
#include <linux/swap.h>
#include <linux/syscalls.h>

#include <asm/types.h>
#include <asm/uaccess.h>
#include <asm/semaphore.h>
#include <asm/mmu_context.h>

#include "sys32.h"

#undef DEBUG

#ifdef DEBUG
#define DBG(x)	printk x
#else
#define DBG(x)
#endif

/*
 * sys32_execve() executes a new program.
 */

asmlinkage int sys32_execve(struct pt_regs *regs)
{
	int error;
	char *filename;

	DBG(("sys32_execve(%p) r26 = 0x%lx\n", regs, regs->gr[26]));
	filename = getname((char *) regs->gr[26]);
	error = PTR_ERR(filename);
	if (IS_ERR(filename))
		goto out;
	error = compat_do_execve(filename, compat_ptr(regs->gr[25]),
				 compat_ptr(regs->gr[24]), regs);
	if (error == 0)
		current->ptrace &= ~PT_DTRACE;
	putname(filename);
out:

	return error;
}

asmlinkage long sys32_unimplemented(int r26, int r25, int r24, int r23,
	int r22, int r21, int r20)
{
    printk(KERN_ERR "%s(%d): Unimplemented 32 on 64 syscall #%d!\n", 
    	current->comm, current->pid, r20);
    return -ENOSYS;
}

#ifdef CONFIG_SYSCTL

struct __sysctl_args32 {
	u32 name;
	int nlen;
	u32 oldval;
	u32 oldlenp;
	u32 newval;
	u32 newlen;
	u32 __unused[4];
};

asmlinkage long sys32_sysctl(struct __sysctl_args32 *args)
{
	struct __sysctl_args32 tmp;
	int error;
	unsigned int oldlen32;
	size_t oldlen, *oldlenp = NULL;
	unsigned long addr = (((long)&args->__unused[0]) + 7) & ~7;
	extern int do_sysctl(int *name, int nlen, void *oldval, size_t *oldlenp,
	       void *newval, size_t newlen);

	DBG(("sysctl32(%p)\n", args));

	if (copy_from_user(&tmp, args, sizeof(tmp)))
		return -EFAULT;

	if (tmp.oldval && tmp.oldlenp) {
		/* Duh, this is ugly and might not work if sysctl_args
		   is in read-only memory, but do_sysctl does indirectly
		   a lot of uaccess in both directions and we'd have to
		   basically copy the whole sysctl.c here, and
		   glibc's __sysctl uses rw memory for the structure
		   anyway.  */
		/* a possibly better hack than this, which will avoid the
		 * problem if the struct is read only, is to push the
		 * 'oldlen' value out to the user's stack instead. -PB
		 */
		if (get_user(oldlen32, (u32 *)(u64)tmp.oldlenp))
			return -EFAULT;
		oldlen = oldlen32;
		if (put_user(oldlen, (size_t *)addr))
			return -EFAULT;
		oldlenp = (size_t *)addr;
	}

	lock_kernel();
	error = do_sysctl((int *)(u64)tmp.name, tmp.nlen, (void *)(u64)tmp.oldval,
			  oldlenp, (void *)(u64)tmp.newval, tmp.newlen);
	unlock_kernel();
	if (oldlenp) {
		if (!error) {
			if (get_user(oldlen, (size_t *)addr)) {
				error = -EFAULT;
			} else {
				oldlen32 = oldlen;
				if (put_user(oldlen32, (u32 *)(u64)tmp.oldlenp))
					error = -EFAULT;
			}
		}
		if (copy_to_user(args->__unused, tmp.__unused, sizeof(tmp.__unused)))
			error = -EFAULT;
	}
	return error;
}

#else /* CONFIG_SYSCTL */

asmlinkage long sys32_sysctl(struct __sysctl_args *args)
{
	return -ENOSYS;
}
#endif /* CONFIG_SYSCTL */

asmlinkage long sys32_sched_rr_get_interval(pid_t pid,
	struct compat_timespec *interval)
{
	struct timespec t;
	int ret;
	
	KERNEL_SYSCALL(ret, sys_sched_rr_get_interval, pid, &t);
	if (put_compat_timespec(&t, interval))
		return -EFAULT;
	return ret;
}

static int
put_compat_timeval(struct compat_timeval *u, struct timeval *t)
{
	struct compat_timeval t32;
	t32.tv_sec = t->tv_sec;
	t32.tv_usec = t->tv_usec;
	return copy_to_user(u, &t32, sizeof t32);
}

static inline long get_ts32(struct timespec *o, struct compat_timeval *i)
{
	long usec;

	if (__get_user(o->tv_sec, &i->tv_sec))
		return -EFAULT;
	if (__get_user(usec, &i->tv_usec))
		return -EFAULT;
	o->tv_nsec = usec * 1000;
	return 0;
}

asmlinkage long sys32_time(compat_time_t *tloc)
{
	struct timeval tv;
	compat_time_t now32;

	do_gettimeofday(&tv);
	now32 = tv.tv_sec;

	if (tloc)
		if (put_user(now32, tloc))
			now32 = -EFAULT;

	return now32;
}

asmlinkage int
sys32_gettimeofday(struct compat_timeval *tv, struct timezone *tz)
{
    extern void do_gettimeofday(struct timeval *tv);

    if (tv) {
	    struct timeval ktv;
	    do_gettimeofday(&ktv);
	    if (put_compat_timeval(tv, &ktv))
		    return -EFAULT;
    }
    if (tz) {
	    extern struct timezone sys_tz;
	    if (copy_to_user(tz, &sys_tz, sizeof(sys_tz)))
		    return -EFAULT;
    }
    return 0;
}

asmlinkage 
int sys32_settimeofday(struct compat_timeval *tv, struct timezone *tz)
{
	struct timespec kts;
	struct timezone ktz;

 	if (tv) {
		if (get_ts32(&kts, tv))
			return -EFAULT;
	}
	if (tz) {
		if (copy_from_user(&ktz, tz, sizeof(ktz)))
			return -EFAULT;
	}

	return do_sys_settimeofday(tv ? &kts : NULL, tz ? &ktz : NULL);
}

int cp_compat_stat(struct kstat *stat, struct compat_stat *statbuf)
{
	int err;

	if (stat->size > MAX_NON_LFS || !new_valid_dev(stat->dev) ||
	    !new_valid_dev(stat->rdev))
		return -EOVERFLOW;

	err  = put_user(new_encode_dev(stat->dev), &statbuf->st_dev);
	err |= put_user(stat->ino, &statbuf->st_ino);
	err |= put_user(stat->mode, &statbuf->st_mode);
	err |= put_user(stat->nlink, &statbuf->st_nlink);
	err |= put_user(0, &statbuf->st_reserved1);
	err |= put_user(0, &statbuf->st_reserved2);
	err |= put_user(new_encode_dev(stat->rdev), &statbuf->st_rdev);
	err |= put_user(stat->size, &statbuf->st_size);
	err |= put_user(stat->atime.tv_sec, &statbuf->st_atime);
	err |= put_user(stat->atime.tv_nsec, &statbuf->st_atime_nsec);
	err |= put_user(stat->mtime.tv_sec, &statbuf->st_mtime);
	err |= put_user(stat->mtime.tv_nsec, &statbuf->st_mtime_nsec);
	err |= put_user(stat->ctime.tv_sec, &statbuf->st_ctime);
	err |= put_user(stat->ctime.tv_nsec, &statbuf->st_ctime_nsec);
	err |= put_user(stat->blksize, &statbuf->st_blksize);
	err |= put_user(stat->blocks, &statbuf->st_blocks);
	err |= put_user(0, &statbuf->__unused1);
	err |= put_user(0, &statbuf->__unused2);
	err |= put_user(0, &statbuf->__unused3);
	err |= put_user(0, &statbuf->__unused4);
	err |= put_user(0, &statbuf->__unused5);
	err |= put_user(0, &statbuf->st_fstype); /* not avail */
	err |= put_user(0, &statbuf->st_realdev); /* not avail */
	err |= put_user(0, &statbuf->st_basemode); /* not avail */
	err |= put_user(0, &statbuf->st_spareshort);
	err |= put_user(stat->uid, &statbuf->st_uid);
	err |= put_user(stat->gid, &statbuf->st_gid);
	err |= put_user(0, &statbuf->st_spare4[0]);
	err |= put_user(0, &statbuf->st_spare4[1]);
	err |= put_user(0, &statbuf->st_spare4[2]);

	return err;
}

struct linux32_dirent {
	u32		d_ino;
	compat_off_t	d_off;
	u16		d_reclen;
	char		d_name[1];
};

struct old_linux32_dirent {
	u32	d_ino;
	u32	d_offset;
	u16	d_namlen;
	char	d_name[1];
};

struct getdents32_callback {
	struct linux32_dirent * current_dir;
	struct linux32_dirent * previous;
	int count;
	int error;
};

struct readdir32_callback {
	struct old_linux32_dirent * dirent;
	int count;
};

#define ROUND_UP(x,a)	((__typeof__(x))(((unsigned long)(x) + ((a) - 1)) & ~((a) - 1)))
#define NAME_OFFSET(de) ((int) ((de)->d_name - (char *) (de)))
static int
filldir32 (void *__buf, const char *name, int namlen, loff_t offset, ino_t ino,
	   unsigned int d_type)
{
	struct linux32_dirent * dirent;
	struct getdents32_callback * buf = (struct getdents32_callback *) __buf;
	int reclen = ROUND_UP(NAME_OFFSET(dirent) + namlen + 1, 4);

	buf->error = -EINVAL;	/* only used if we fail.. */
	if (reclen > buf->count)
		return -EINVAL;
	dirent = buf->previous;
	if (dirent)
		put_user(offset, &dirent->d_off);
	dirent = buf->current_dir;
	buf->previous = dirent;
	put_user(ino, &dirent->d_ino);
	put_user(reclen, &dirent->d_reclen);
	copy_to_user(dirent->d_name, name, namlen);
	put_user(0, dirent->d_name + namlen);
	dirent = (struct linux32_dirent *)((char *)dirent + reclen);
	buf->current_dir = dirent;
	buf->count -= reclen;
	return 0;
}

asmlinkage long
sys32_getdents (unsigned int fd, void * dirent, unsigned int count)
{
	struct file * file;
	struct linux32_dirent * lastdirent;
	struct getdents32_callback buf;
	int error;

	error = -EBADF;
	file = fget(fd);
	if (!file)
		goto out;

	buf.current_dir = (struct linux32_dirent *) dirent;
	buf.previous = NULL;
	buf.count = count;
	buf.error = 0;

	error = vfs_readdir(file, filldir32, &buf);
	if (error < 0)
		goto out_putf;
	error = buf.error;
	lastdirent = buf.previous;
	if (lastdirent) {
		put_user(file->f_pos, &lastdirent->d_off);
		error = count - buf.count;
	}

out_putf:
	fput(file);
out:
	return error;
}

static int
fillonedir32 (void * __buf, const char * name, int namlen, loff_t offset, ino_t ino,
	      unsigned int d_type)
{
	struct readdir32_callback * buf = (struct readdir32_callback *) __buf;
	struct old_linux32_dirent * dirent;

	if (buf->count)
		return -EINVAL;
	buf->count++;
	dirent = buf->dirent;
	put_user(ino, &dirent->d_ino);
	put_user(offset, &dirent->d_offset);
	put_user(namlen, &dirent->d_namlen);
	copy_to_user(dirent->d_name, name, namlen);
	put_user(0, dirent->d_name + namlen);
	return 0;
}

asmlinkage long
sys32_readdir (unsigned int fd, void * dirent, unsigned int count)
{
	int error;
	struct file * file;
	struct readdir32_callback buf;

	error = -EBADF;
	file = fget(fd);
	if (!file)
		goto out;

	buf.count = 0;
	buf.dirent = dirent;

	error = vfs_readdir(file, fillonedir32, &buf);
	if (error >= 0)
		error = buf.count;
	fput(file);
out:
	return error;
}

/*** copied from mips64 ***/
/*
 * Ooo, nasty.  We need here to frob 32-bit unsigned longs to
 * 64-bit unsigned longs.
 */

static inline int
get_fd_set32(unsigned long n, u32 *ufdset, unsigned long *fdset)
{
	n = (n + 8*sizeof(u32) - 1) / (8*sizeof(u32));
	if (ufdset) {
		unsigned long odd;

		if (verify_area(VERIFY_WRITE, ufdset, n*sizeof(u32)))
			return -EFAULT;

		odd = n & 1UL;
		n &= ~1UL;
		while (n) {
			unsigned long h, l;
			__get_user(l, ufdset);
			__get_user(h, ufdset+1);
			ufdset += 2;
			*fdset++ = h << 32 | l;
			n -= 2;
		}
		if (odd)
			__get_user(*fdset, ufdset);
	} else {
		/* Tricky, must clear full unsigned long in the
		 * kernel fdset at the end, this makes sure that
		 * actually happens.
		 */
		memset(fdset, 0, ((n + 1) & ~1)*sizeof(u32));
	}
	return 0;
}

static inline void
set_fd_set32(unsigned long n, u32 *ufdset, unsigned long *fdset)
{
	unsigned long odd;
	n = (n + 8*sizeof(u32) - 1) / (8*sizeof(u32));

	if (!ufdset)
		return;

	odd = n & 1UL;
	n &= ~1UL;
	while (n) {
		unsigned long h, l;
		l = *fdset++;
		h = l >> 32;
		__put_user(l, ufdset);
		__put_user(h, ufdset+1);
		ufdset += 2;
		n -= 2;
	}
	if (odd)
		__put_user(*fdset, ufdset);
}

struct msgbuf32 {
    int mtype;
    char mtext[1];
};

asmlinkage long sys32_msgsnd(int msqid,
				struct msgbuf32 *umsgp32,
				size_t msgsz, int msgflg)
{
	struct msgbuf *mb;
	struct msgbuf32 mb32;
	int err;

	if ((mb = kmalloc(msgsz + sizeof *mb + 4, GFP_KERNEL)) == NULL)
		return -ENOMEM;

	err = get_user(mb32.mtype, &umsgp32->mtype);
	mb->mtype = mb32.mtype;
	err |= copy_from_user(mb->mtext, &umsgp32->mtext, msgsz);

	if (err)
		err = -EFAULT;
	else
		KERNEL_SYSCALL(err, sys_msgsnd, msqid, mb, msgsz, msgflg);

	kfree(mb);
	return err;
}

asmlinkage long sys32_msgrcv(int msqid,
				struct msgbuf32 *umsgp32,
				size_t msgsz, long msgtyp, int msgflg)
{
	struct msgbuf *mb;
	struct msgbuf32 mb32;
	int err, len;

	if ((mb = kmalloc(msgsz + sizeof *mb + 4, GFP_KERNEL)) == NULL)
		return -ENOMEM;

	KERNEL_SYSCALL(err, sys_msgrcv, msqid, mb, msgsz, msgtyp, msgflg);

	if (err >= 0) {
		len = err;
		mb32.mtype = mb->mtype;
		err = put_user(mb32.mtype, &umsgp32->mtype);
		err |= copy_to_user(&umsgp32->mtext, mb->mtext, len);
		if (err)
			err = -EFAULT;
		else
			err = len;
	}

	kfree(mb);
	return err;
}

asmlinkage int sys32_sendfile(int out_fd, int in_fd, compat_off_t *offset, s32 count)
{
        mm_segment_t old_fs = get_fs();
        int ret;
        off_t of;

        if (offset && get_user(of, offset))
                return -EFAULT;

        set_fs(KERNEL_DS);
        ret = sys_sendfile(out_fd, in_fd, offset ? &of : NULL, count);
        set_fs(old_fs);

        if (offset && put_user(of, offset))
                return -EFAULT;

        return ret;
}

typedef long __kernel_loff_t32;		/* move this to asm/posix_types.h? */

asmlinkage int sys32_sendfile64(int out_fd, int in_fd, __kernel_loff_t32 *offset, s32 count)
{
	mm_segment_t old_fs = get_fs();
	int ret;
	loff_t lof;
	
	if (offset && get_user(lof, offset))
		return -EFAULT;
		
	set_fs(KERNEL_DS);
	ret = sys_sendfile64(out_fd, in_fd, offset ? &lof : NULL, count);
	set_fs(old_fs);
	
	if (offset && put_user(lof, offset))
		return -EFAULT;
		
	return ret;
}


struct timex32 {
	unsigned int modes;	/* mode selector */
	int offset;		/* time offset (usec) */
	int freq;		/* frequency offset (scaled ppm) */
	int maxerror;		/* maximum error (usec) */
	int esterror;		/* estimated error (usec) */
	int status;		/* clock command/status */
	int constant;		/* pll time constant */
	int precision;		/* clock precision (usec) (read only) */
	int tolerance;		/* clock frequency tolerance (ppm)
				 * (read only)
				 */
	struct compat_timeval time;	/* (read only) */
	int tick;		/* (modified) usecs between clock ticks */

	int ppsfreq;           /* pps frequency (scaled ppm) (ro) */
	int jitter;            /* pps jitter (us) (ro) */
	int shift;              /* interval duration (s) (shift) (ro) */
	int stabil;            /* pps stability (scaled ppm) (ro) */
	int jitcnt;            /* jitter limit exceeded (ro) */
	int calcnt;            /* calibration intervals (ro) */
	int errcnt;            /* calibration errors (ro) */
	int stbcnt;            /* stability limit exceeded (ro) */

	int  :32; int  :32; int  :32; int  :32;
	int  :32; int  :32; int  :32; int  :32;
	int  :32; int  :32; int  :32; int  :32;
};

asmlinkage long sys32_adjtimex(struct timex32 *txc_p32)
{
	struct timex txc;
	struct timex32 t32;
	int ret;
	extern int do_adjtimex(struct timex *txc);

	if(copy_from_user(&t32, txc_p32, sizeof(struct timex32)))
		return -EFAULT;
#undef CP
#define CP(x) txc.x = t32.x
	CP(modes); CP(offset); CP(freq); CP(maxerror); CP(esterror);
	CP(status); CP(constant); CP(precision); CP(tolerance);
	CP(time.tv_sec); CP(time.tv_usec); CP(tick); CP(ppsfreq); CP(jitter);
	CP(shift); CP(stabil); CP(jitcnt); CP(calcnt); CP(errcnt);
	CP(stbcnt);
	ret = do_adjtimex(&txc);
#undef CP
#define CP(x) t32.x = txc.x
	CP(modes); CP(offset); CP(freq); CP(maxerror); CP(esterror);
	CP(status); CP(constant); CP(precision); CP(tolerance);
	CP(time.tv_sec); CP(time.tv_usec); CP(tick); CP(ppsfreq); CP(jitter);
	CP(shift); CP(stabil); CP(jitcnt); CP(calcnt); CP(errcnt);
	CP(stbcnt);
	return copy_to_user(txc_p32, &t32, sizeof(struct timex32)) ? -EFAULT : ret;
}


struct sysinfo32 {
	s32 uptime;
	u32 loads[3];
	u32 totalram;
	u32 freeram;
	u32 sharedram;
	u32 bufferram;
	u32 totalswap;
	u32 freeswap;
	unsigned short procs;
	u32 totalhigh;
	u32 freehigh;
	u32 mem_unit;
	char _f[12];
};

/* We used to call sys_sysinfo and translate the result.  But sys_sysinfo
 * undoes the good work done elsewhere, and rather than undoing the
 * damage, I decided to just duplicate the code from sys_sysinfo here.
 */

asmlinkage int sys32_sysinfo(struct sysinfo32 *info)
{
	struct sysinfo val;
	int err;
	unsigned long seq;

	/* We don't need a memset here because we copy the
	 * struct to userspace once element at a time.
	 */

	do {
		seq = read_seqbegin(&xtime_lock);
		val.uptime = jiffies / HZ;

		val.loads[0] = avenrun[0] << (SI_LOAD_SHIFT - FSHIFT);
		val.loads[1] = avenrun[1] << (SI_LOAD_SHIFT - FSHIFT);
		val.loads[2] = avenrun[2] << (SI_LOAD_SHIFT - FSHIFT);

		val.procs = nr_threads;
	} while (read_seqretry(&xtime_lock, seq));


	si_meminfo(&val);
	si_swapinfo(&val);
	
	err = put_user (val.uptime, &info->uptime);
	err |= __put_user (val.loads[0], &info->loads[0]);
	err |= __put_user (val.loads[1], &info->loads[1]);
	err |= __put_user (val.loads[2], &info->loads[2]);
	err |= __put_user (val.totalram, &info->totalram);
	err |= __put_user (val.freeram, &info->freeram);
	err |= __put_user (val.sharedram, &info->sharedram);
	err |= __put_user (val.bufferram, &info->bufferram);
	err |= __put_user (val.totalswap, &info->totalswap);
	err |= __put_user (val.freeswap, &info->freeswap);
	err |= __put_user (val.procs, &info->procs);
	err |= __put_user (val.totalhigh, &info->totalhigh);
	err |= __put_user (val.freehigh, &info->freehigh);
	err |= __put_user (val.mem_unit, &info->mem_unit);
	return err ? -EFAULT : 0;
}


/* lseek() needs a wrapper because 'offset' can be negative, but the top
 * half of the argument has been zeroed by syscall.S.
 */

asmlinkage int sys32_lseek(unsigned int fd, int offset, unsigned int origin)
{
	return sys_lseek(fd, offset, origin);
}

asmlinkage long sys32_semctl(int semid, int semnum, int cmd, union semun arg)
{
        union semun u;
	
        if (cmd == SETVAL) {
                /* Ugh.  arg is a union of int,ptr,ptr,ptr, so is 8 bytes.
                 * The int should be in the first 4, but our argument
                 * frobbing has left it in the last 4.
                 */
                u.val = *((int *)&arg + 1);
                return sys_semctl (semid, semnum, cmd, u);
	}
	return sys_semctl (semid, semnum, cmd, arg);
}

long sys32_lookup_dcookie(u32 cookie_high, u32 cookie_low, char *buf,
			  size_t len)
{
	return sys_lookup_dcookie((u64)cookie_high << 32 | cookie_low,
				  buf, len);
}
