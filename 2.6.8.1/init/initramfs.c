#include <linux/init.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/syscalls.h>

static __initdata char *message;
static void __init error(char *x)
{
	if (!message)
		message = x;
}

static void __init *malloc(size_t size)
{
	return kmalloc(size, GFP_KERNEL);
}

static void __init free(void *where)
{
	kfree(where);
}

/* link hash */

static struct hash {
	int ino, minor, major;
	struct hash *next;
	char *name;
} *head[32];

static inline int hash(int major, int minor, int ino)
{
	unsigned long tmp = ino + minor + (major << 3);
	tmp += tmp >> 5;
	return tmp & 31;
}

static char __init *find_link(int major, int minor, int ino, char *name)
{
	struct hash **p, *q;
	for (p = head + hash(major, minor, ino); *p; p = &(*p)->next) {
		if ((*p)->ino != ino)
			continue;
		if ((*p)->minor != minor)
			continue;
		if ((*p)->major != major)
			continue;
		return (*p)->name;
	}
	q = (struct hash *)malloc(sizeof(struct hash));
	if (!q)
		panic("can't allocate link hash entry");
	q->ino = ino;
	q->minor = minor;
	q->major = major;
	q->name = name;
	q->next = NULL;
	*p = q;
	return NULL;
}

static void __init free_hash(void)
{
	struct hash **p, *q;
	for (p = head; p < head + 32; p++) {
		while (*p) {
			q = *p;
			*p = q->next;
			free(q);
		}
	}
}

/* cpio header parsing */

static __initdata unsigned long ino, major, minor, nlink;
static __initdata mode_t mode;
static __initdata unsigned long body_len, name_len;
static __initdata uid_t uid;
static __initdata gid_t gid;
static __initdata unsigned rdev;

static void __init parse_header(char *s)
{
	unsigned long parsed[12];
	char buf[9];
	int i;

	buf[8] = '\0';
	for (i = 0, s += 6; i < 12; i++, s += 8) {
		memcpy(buf, s, 8);
		parsed[i] = simple_strtoul(buf, NULL, 16);
	}
	ino = parsed[0];
	mode = parsed[1];
	uid = parsed[2];
	gid = parsed[3];
	nlink = parsed[4];
	body_len = parsed[6];
	major = parsed[7];
	minor = parsed[8];
	rdev = new_encode_dev(MKDEV(parsed[9], parsed[10]));
	name_len = parsed[11];
}

/* FSM */

static __initdata enum state {
	Start,
	Collect,
	GotHeader,
	SkipIt,
	GotName,
	CopyFile,
	GotSymlink,
	Reset
} state, next_state;

static __initdata char *victim;
static __initdata unsigned count;
static __initdata loff_t this_header, next_header;

static __initdata int dry_run;

static inline void eat(unsigned n)
{
	victim += n;
	this_header += n;
	count -= n;
}

#define N_ALIGN(len) ((((len) + 1) & ~3) + 2)

static __initdata char *collected;
static __initdata int remains;
static __initdata char *collect;

static void __init read_into(char *buf, unsigned size, enum state next)
{
	if (count >= size) {
		collected = victim;
		eat(size);
		state = next;
	} else {
		collect = collected = buf;
		remains = size;
		next_state = next;
		state = Collect;
	}
}

static __initdata char *header_buf, *symlink_buf, *name_buf;

static int __init do_start(void)
{
	read_into(header_buf, 110, GotHeader);
	return 0;
}

static int __init do_collect(void)
{
	unsigned n = remains;
	if (count < n)
		n = count;
	memcpy(collect, victim, n);
	eat(n);
	collect += n;
	if ((remains -= n) != 0)
		return 1;
	state = next_state;
	return 0;
}

static int __init do_header(void)
{
	if (memcmp(collected, "070701", 6)) {
		error("no cpio magic");
		return 1;
	}
	parse_header(collected);
	next_header = this_header + N_ALIGN(name_len) + body_len;
	next_header = (next_header + 3) & ~3;
	if (dry_run) {
		read_into(name_buf, N_ALIGN(name_len), GotName);
		return 0;
	}
	state = SkipIt;
	if (name_len <= 0 || name_len > PATH_MAX)
		return 0;
	if (S_ISLNK(mode)) {
		if (body_len > PATH_MAX)
			return 0;
		collect = collected = symlink_buf;
		remains = N_ALIGN(name_len) + body_len;
		next_state = GotSymlink;
		state = Collect;
		return 0;
	}
	if (S_ISREG(mode) || !body_len)
		read_into(name_buf, N_ALIGN(name_len), GotName);
	return 0;
}

static int __init do_skip(void)
{
	if (this_header + count < next_header) {
		eat(count);
		return 1;
	} else {
		eat(next_header - this_header);
		state = next_state;
		return 0;
	}
}

static int __init do_reset(void)
{
	while(count && *victim == '\0')
		eat(1);
	if (count && (this_header & 3))
		error("broken padding");
	return 1;
}

static int __init maybe_link(void)
{
	if (nlink >= 2) {
		char *old = find_link(major, minor, ino, collected);
		if (old)
			return (sys_link(old, collected) < 0) ? -1 : 1;
	}
	return 0;
}

static __initdata int wfd;

static int __init do_name(void)
{
	state = SkipIt;
	next_state = Start;
	if (strcmp(collected, "TRAILER!!!") == 0) {
		free_hash();
		next_state = Reset;
		return 0;
	}
	if (dry_run)
		return 0;
	if (S_ISREG(mode)) {
		if (maybe_link() >= 0) {
			wfd = sys_open(collected, O_WRONLY|O_CREAT, mode);
			if (wfd >= 0) {
				sys_fchown(wfd, uid, gid);
				sys_fchmod(wfd, mode);
				state = CopyFile;
			}
		}
	} else if (S_ISDIR(mode)) {
		sys_mkdir(collected, mode);
		sys_chown(collected, uid, gid);
		sys_chmod(collected, mode);
	} else if (S_ISBLK(mode) || S_ISCHR(mode) ||
		   S_ISFIFO(mode) || S_ISSOCK(mode)) {
		if (maybe_link() == 0) {
			sys_mknod(collected, mode, rdev);
			sys_chown(collected, uid, gid);
			sys_chmod(collected, mode);
		}
	}
	return 0;
}

static int __init do_copy(void)
{
	if (count >= body_len) {
		sys_write(wfd, victim, body_len);
		sys_close(wfd);
		eat(body_len);
		state = SkipIt;
		return 0;
	} else {
		sys_write(wfd, victim, count);
		body_len -= count;
		eat(count);
		return 1;
	}
}

static int __init do_symlink(void)
{
	collected[N_ALIGN(name_len) + body_len] = '\0';
	sys_symlink(collected + N_ALIGN(name_len), collected);
	sys_lchown(collected, uid, gid);
	state = SkipIt;
	next_state = Start;
	return 0;
}

static __initdata int (*actions[])(void) = {
	[Start]		= do_start,
	[Collect]	= do_collect,
	[GotHeader]	= do_header,
	[SkipIt]	= do_skip,
	[GotName]	= do_name,
	[CopyFile]	= do_copy,
	[GotSymlink]	= do_symlink,
	[Reset]		= do_reset,
};

static int __init write_buffer(char *buf, unsigned len)
{
	count = len;
	victim = buf;

	while (!actions[state]())
		;
	return len - count;
}

static void __init flush_buffer(char *buf, unsigned len)
{
	int written;
	if (message)
		return;
	while ((written = write_buffer(buf, len)) < len && !message) {
		char c = buf[written];
		if (c == '0') {
			buf += written;
			len -= written;
			state = Start;
		} else
			error("junk in compressed archive");
	}
}

/*
 * gzip declarations
 */

#define OF(args)  args

#ifndef memzero
#define memzero(s, n)     memset ((s), 0, (n))
#endif

typedef unsigned char  uch;
typedef unsigned short ush;
typedef unsigned long  ulg;

#define WSIZE 0x8000    /* window size--must be a power of two, and */
			/*  at least 32K for zip's deflate method */

static uch *inbuf;
static uch *window;

static unsigned insize;  /* valid bytes in inbuf */
static unsigned inptr;   /* index of next byte to be processed in inbuf */
static unsigned outcnt;  /* bytes in output buffer */
static long bytes_out;

#define get_byte()  (inptr < insize ? inbuf[inptr++] : -1)
		
/* Diagnostic functions (stubbed out) */
#define Assert(cond,msg)
#define Trace(x)
#define Tracev(x)
#define Tracevv(x)
#define Tracec(c,x)
#define Tracecv(c,x)

#define STATIC static

static void flush_window(void);
static void error(char *m);
static void gzip_mark(void **);
static void gzip_release(void **);

#include "../lib/inflate.c"

static void __init gzip_mark(void **ptr)
{
}

static void __init gzip_release(void **ptr)
{
}

/* ===========================================================================
 * Write the output window window[0..outcnt-1] and update crc and bytes_out.
 * (Used for the decompressed data only.)
 */
static void __init flush_window(void)
{
	ulg c = crc;         /* temporary variable */
	unsigned n;
	uch *in, ch;

	flush_buffer(window, outcnt);
	in = window;
	for (n = 0; n < outcnt; n++) {
		ch = *in++;
		c = crc_32_tab[((int)c ^ ch) & 0xff] ^ (c >> 8);
	}
	crc = c;
	bytes_out += (ulg)outcnt;
	outcnt = 0;
}

char * __init unpack_to_rootfs(char *buf, unsigned len, int check_only)
{
	int written;
	dry_run = check_only;
	header_buf = malloc(110);
	symlink_buf = malloc(PATH_MAX + N_ALIGN(PATH_MAX) + 1);
	name_buf = malloc(N_ALIGN(PATH_MAX));
	window = malloc(WSIZE);
	if (!window || !header_buf || !symlink_buf || !name_buf)
		panic("can't allocate buffers");
	state = Start;
	this_header = 0;
	message = NULL;
	while (!message && len) {
		loff_t saved_offset = this_header;
		if (*buf == '0' && !(this_header & 3)) {
			state = Start;
			written = write_buffer(buf, len);
			buf += written;
			len -= written;
			continue;
		}
		if (!*buf) {
			buf++;
			len--;
			this_header++;
			continue;
		}
		this_header = 0;
		insize = len;
		inbuf = buf;
		inptr = 0;
		outcnt = 0;		/* bytes in output buffer */
		bytes_out = 0;
		crc = (ulg)0xffffffffL; /* shift register contents */
		makecrc();
		if (gunzip())
			message = "ungzip failed";
		if (state != Reset)
			error("junk in gzipped archive");
		this_header = saved_offset + inptr;
		buf += inptr;
		len -= inptr;
	}
	free(window);
	free(name_buf);
	free(symlink_buf);
	free(header_buf);
	return message;
}

extern char __initramfs_start, __initramfs_end;
#ifdef CONFIG_BLK_DEV_INITRD
#include <linux/initrd.h>
#endif

void __init populate_rootfs(void)
{
	char *err = unpack_to_rootfs(&__initramfs_start,
			 &__initramfs_end - &__initramfs_start, 0);
	if (err)
		panic(err);
#ifdef CONFIG_BLK_DEV_INITRD
	if (initrd_start) {
		int fd;
		printk(KERN_INFO "checking if image is initramfs...");
		err = unpack_to_rootfs((char *)initrd_start,
			initrd_end - initrd_start, 1);
		if (!err) {
			printk(" it is\n");
			unpack_to_rootfs((char *)initrd_start,
				initrd_end - initrd_start, 0);
			free_initrd_mem(initrd_start, initrd_end);
			return;
		}
		printk("it isn't (%s); looks like an initrd\n", err);
		fd = sys_open("/initrd.image", O_WRONLY|O_CREAT, 700);
		if (fd >= 0) {
			sys_write(fd, (char *)initrd_start,
					initrd_end - initrd_start);
			sys_close(fd);
			free_initrd_mem(initrd_start, initrd_end);
		}
	}
#endif
}
