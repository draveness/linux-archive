/*
 * V9FS definitions.
 *
 *  Copyright (C) 2004 by Eric Van Hensbergen <ericvh@gmail.com>
 *  Copyright (C) 2002 by Ron Minnich <rminnich@lanl.gov>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to:
 *  Free Software Foundation
 *  51 Franklin Street, Fifth Floor
 *  Boston, MA  02111-1301  USA
 *
 */

/*
  * Session structure provides information for an opened session
  *
  */

struct v9fs_session_info {
	/* options */
	unsigned int maxdata;
	unsigned char extended;	/* set to 1 if we are using UNIX extensions */
	unsigned char nodev;	/* set to 1 if no disable device mapping */
	unsigned short port;	/* port to connect to */
	unsigned short debug;	/* debug level */
	unsigned short proto;	/* protocol to use */
	unsigned int afid;	/* authentication fid */
	unsigned int rfdno;	/* read file descriptor number */
	unsigned int wfdno;	/* write file descriptor number */
	unsigned int cache;	/* cache mode */

	char *name;		/* user name to mount as */
	char *remotename;	/* name of remote hierarchy being mounted */
	unsigned int uid;	/* default uid/muid for legacy support */
	unsigned int gid;	/* default gid for legacy support */

	struct p9_client *clnt;	/* 9p client */
	struct dentry *debugfs_dir;
};

/* possible values of ->proto */
enum {
	PROTO_TCP,
	PROTO_UNIX,
	PROTO_FD,
	PROTO_PCI,
};

/* possible values of ->cache */
/* eventually support loose, tight, time, session, default always none */
enum {
	CACHE_NONE,		/* default */
	CACHE_LOOSE,		/* no consistency */
};

extern struct dentry *v9fs_debugfs_root;

struct p9_fid *v9fs_session_init(struct v9fs_session_info *, const char *,
									char *);
void v9fs_session_close(struct v9fs_session_info *v9ses);
void v9fs_session_cancel(struct v9fs_session_info *v9ses);

#define V9FS_MAGIC 0x01021997

/* other default globals */
#define V9FS_PORT		564
#define V9FS_DEFUSER	"nobody"
#define V9FS_DEFANAME	""

static inline struct v9fs_session_info *v9fs_inode2v9ses(struct inode *inode)
{
	return (inode->i_sb->s_fs_info);
}
