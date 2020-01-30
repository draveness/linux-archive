#ifndef _NFS_FS_I
#define _NFS_FS_I

#include <asm/types.h>
#include <linux/list.h>
#include <linux/nfs.h>

/*
 * NFS lock info
 */
struct nfs_lock_info {
	u32		state;
	u32		flags;
	struct nlm_host	*host;
};

/*
 * Lock flag values
 */
#define NFS_LCK_GRANTED		0x0001		/* lock has been granted */
#define NFS_LCK_RECLAIM		0x0002		/* lock marked for reclaiming */

#endif
