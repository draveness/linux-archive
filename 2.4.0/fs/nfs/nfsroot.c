/*
 *  $Id: nfsroot.c,v 1.45 1998/03/07 10:44:46 mj Exp $
 *
 *  Copyright (C) 1995, 1996  Gero Kuhlmann <gero@gkminix.han.de>
 *
 *  Allow an NFS filesystem to be mounted as root. The way this works is:
 *     (1) Use the IP autoconfig mechanism to set local IP addresses and routes.
 *     (2) Handle RPC negotiation with the system which replied to RARP or
 *         was reported as a boot server by BOOTP or manually.
 *     (3) The actual mounting is done later, when init() is running.
 *
 *
 *	Changes:
 *
 *	Alan Cox	:	Removed get_address name clash with FPU.
 *	Alan Cox	:	Reformatted a bit.
 *	Gero Kuhlmann	:	Code cleanup
 *	Michael Rausch  :	Fixed recognition of an incoming RARP answer.
 *	Martin Mares	: (2.0)	Auto-configuration via BOOTP supported.
 *	Martin Mares	:	Manual selection of interface & BOOTP/RARP.
 *	Martin Mares	:	Using network routes instead of host routes,
 *				allowing the default configuration to be used
 *				for normal operation of the host.
 *	Martin Mares	:	Randomized timer with exponential backoff
 *				installed to minimize network congestion.
 *	Martin Mares	:	Code cleanup.
 *	Martin Mares	: (2.1)	BOOTP and RARP made configuration options.
 *	Martin Mares	:	Server hostname generation fixed.
 *	Gerd Knorr	:	Fixed wired inode handling
 *	Martin Mares	: (2.2)	"0.0.0.0" addresses from command line ignored.
 *	Martin Mares	:	RARP replies not tested for server address.
 *	Gero Kuhlmann	: (2.3) Some bug fixes and code cleanup again (please
 *				send me your new patches _before_ bothering
 *				Linus so that I don' always have to cleanup
 *				_afterwards_ - thanks)
 *	Gero Kuhlmann	:	Last changes of Martin Mares undone.
 *	Gero Kuhlmann	: 	RARP replies are tested for specified server
 *				again. However, it's now possible to have
 *				different RARP and NFS servers.
 *	Gero Kuhlmann	:	"0.0.0.0" addresses from command line are
 *				now mapped to INADDR_NONE.
 *	Gero Kuhlmann	:	Fixed a bug which prevented BOOTP path name
 *				from being used (thanks to Leo Spiekman)
 *	Andy Walker	:	Allow to specify the NFS server in nfs_root
 *				without giving a path name
 *	Swen Th�mmler	:	Allow to specify the NFS options in nfs_root
 *				without giving a path name. Fix BOOTP request
 *				for domainname (domainname is NIS domain, not
 *				DNS domain!). Skip dummy devices for BOOTP.
 *	Jacek Zapala	:	Fixed a bug which prevented server-ip address
 *				from nfsroot parameter from being used.
 *	Olaf Kirch	:	Adapted to new NFS code.
 *	Jakub Jelinek	:	Free used code segment.
 *	Marko Kohtala	:	Fixed some bugs.
 *	Martin Mares	:	Debug message cleanup
 *	Martin Mares	:	Changed to use the new generic IP layer autoconfig
 *				code. BOOTP and RARP moved there.
 *	Martin Mares	:	Default path now contains host name instead of
 *				host IP address (but host name defaults to IP
 *				address anyway).
 *	Martin Mares	:	Use root_server_addr appropriately during setup.
 *	Martin Mares	:	Rewrote parameter parsing, now hopefully giving
 *				correct overriding.
 *	Trond Myklebust :	Add in preliminary support for NFSv3 and TCP.
 *				Fix bug in root_nfs_addr(). nfs_data.namlen
 *				is NOT for the length of the hostname.
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/sunrpc/clnt.h>
#include <linux/nfs.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_mount.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/major.h>
#include <linux/utsname.h>
#include <net/ipconfig.h>

/* Define this to allow debugging output */
#undef NFSROOT_DEBUG
#define NFSDBG_FACILITY NFSDBG_ROOT

/* Default path we try to mount. "%s" gets replaced by our IP address */
#define NFS_ROOT		"/tftpboot/%s"

/* Parameters passed from the kernel command line */
static char nfs_root_name[256] __initdata = "";

/* Address of NFS server */
static __u32 servaddr __initdata = 0;

/* Name of directory to mount */
static char nfs_path[NFS_MAXPATHLEN] __initdata = { 0, };

/* NFS-related data */
static struct nfs_mount_data nfs_data __initdata = { 0, };/* NFS mount info */
static int nfs_port __initdata = 0;		/* Port to connect to for NFS */
static int mount_port __initdata = 0;		/* Mount daemon port number */


/***************************************************************************

			     Parsing of options

 ***************************************************************************/

/*
 *  The following integer options are recognized
 */
static struct nfs_int_opts {
	char *name;
	int  *val;
} root_int_opts[] __initdata = {
	{ "port",	&nfs_port },
	{ "rsize",	&nfs_data.rsize },
	{ "wsize",	&nfs_data.wsize },
	{ "timeo",	&nfs_data.timeo },
	{ "retrans",	&nfs_data.retrans },
	{ "acregmin",	&nfs_data.acregmin },
	{ "acregmax",	&nfs_data.acregmax },
	{ "acdirmin",	&nfs_data.acdirmin },
	{ "acdirmax",	&nfs_data.acdirmax },
	{ NULL,		NULL }
};


/*
 *  And now the flag options
 */
static struct nfs_bool_opts {
	char *name;
	int  and_mask;
	int  or_mask;
} root_bool_opts[] __initdata = {
	{ "soft",	~NFS_MOUNT_SOFT,	NFS_MOUNT_SOFT },
	{ "hard",	~NFS_MOUNT_SOFT,	0 },
	{ "intr",	~NFS_MOUNT_INTR,	NFS_MOUNT_INTR },
	{ "nointr",	~NFS_MOUNT_INTR,	0 },
	{ "posix",	~NFS_MOUNT_POSIX,	NFS_MOUNT_POSIX },
	{ "noposix",	~NFS_MOUNT_POSIX,	0 },
	{ "cto",	~NFS_MOUNT_NOCTO,	0 },
	{ "nocto",	~NFS_MOUNT_NOCTO,	NFS_MOUNT_NOCTO },
	{ "ac",		~NFS_MOUNT_NOAC,	0 },
	{ "noac",	~NFS_MOUNT_NOAC,	NFS_MOUNT_NOAC },
	{ "lock",	~NFS_MOUNT_NONLM,	0 },
	{ "nolock",	~NFS_MOUNT_NONLM,	NFS_MOUNT_NONLM },
#ifdef CONFIG_NFS_V3
	{ "v2",		~NFS_MOUNT_VER3,	0 },
	{ "v3",		~NFS_MOUNT_VER3,	NFS_MOUNT_VER3 },
#endif
	{ "udp",	~NFS_MOUNT_TCP,		0 },
	{ "tcp",	~NFS_MOUNT_TCP,		NFS_MOUNT_TCP },
	{ "broken_suid",~NFS_MOUNT_BROKEN_SUID,	NFS_MOUNT_BROKEN_SUID },
	{ NULL,		0,			0 }
};


/*
 *  Extract IP address from the parameter string if needed. Note that we
 *  need to have root_server_addr set _before_ IPConfig gets called as it
 *  can override it.
 */
static void __init root_nfs_parse_addr(char *name)
{
	int octets = 0;
	char *cp, *cq;

	cp = cq = name;
	while (octets < 4) {
		while (*cp >= '0' && *cp <= '9')
			cp++;
		if (cp == cq || cp - cq > 3)
			break;
		if (*cp == '.' || octets == 3)
			octets++;
		if (octets < 4)
			cp++;
		cq = cp;
	}
	if (octets == 4 && (*cp == ':' || *cp == '\0')) {
		if (*cp == ':')
			*cp++ = '\0';
		root_server_addr = in_aton(name);
		strcpy(name, cp);
	}
}


/*
 *  Parse option string.
 */
static void __init root_nfs_parse(char *name, char *buf)
{
	char *options, *val, *cp;

	if ((options = strchr(name, ','))) {
		*options++ = 0;
		cp = strtok(options, ",");
		while (cp) {
			if ((val = strchr(cp, '='))) {
				struct nfs_int_opts *opts = root_int_opts;
				*val++ = '\0';
				while (opts->name && strcmp(opts->name, cp))
					opts++;
				if (opts->name)
					*(opts->val) = (int) simple_strtoul(val, NULL, 10);
			} else {
				struct nfs_bool_opts *opts = root_bool_opts;
				while (opts->name && strcmp(opts->name, cp))
					opts++;
				if (opts->name) {
					nfs_data.flags &= opts->and_mask;
					nfs_data.flags |= opts->or_mask;
				}
			}
			cp = strtok(NULL, ",");
		}
	}
	if (name[0] && strcmp(name, "default")) {
		strncpy(buf, name, NFS_MAXPATHLEN-1);
		buf[NFS_MAXPATHLEN-1] = 0;
	}
}


/*
 *  Prepare the NFS data structure and parse all options.
 */
static int __init root_nfs_name(char *name)
{
	char buf[NFS_MAXPATHLEN];
	char *cp;

	/* Set some default values */
	memset(&nfs_data, 0, sizeof(nfs_data));
	nfs_port          = -1;
	nfs_data.version  = NFS_MOUNT_VERSION;
	nfs_data.flags    = NFS_MOUNT_NONLM;	/* No lockd in nfs root yet */
	nfs_data.rsize    = NFS_DEF_FILE_IO_BUFFER_SIZE;
	nfs_data.wsize    = NFS_DEF_FILE_IO_BUFFER_SIZE;
	nfs_data.bsize	  = 0;
	nfs_data.timeo    = 7;
	nfs_data.retrans  = 3;
	nfs_data.acregmin = 3;
	nfs_data.acregmax = 60;
	nfs_data.acdirmin = 30;
	nfs_data.acdirmax = 60;
	strcpy(buf, NFS_ROOT);

	/* Process options received from the remote server */
	root_nfs_parse(root_server_path, buf);

	/* Override them by options set on kernel command-line */
	root_nfs_parse(name, buf);

	cp = system_utsname.nodename;
	if (strlen(buf) + strlen(cp) > NFS_MAXPATHLEN) {
		printk(KERN_ERR "Root-NFS: Pathname for remote directory too long.\n");
		return -1;
	}
	sprintf(nfs_path, buf, cp);

	return 1;
}


/*
 *  Get NFS server address.
 */
static int __init root_nfs_addr(void)
{
	if ((servaddr = root_server_addr) == INADDR_NONE) {
		printk(KERN_ERR "Root-NFS: No NFS server available, giving up.\n");
		return -1;
	}

	strncpy(nfs_data.hostname, in_ntoa(servaddr), sizeof(nfs_data.hostname)-1);
	return 0;
}

/*
 *  Tell the user what's going on.
 */
#ifdef NFSROOT_DEBUG
static void __init root_nfs_print(void)
{
	printk(KERN_NOTICE "Root-NFS: Mounting %s on server %s as root\n",
		nfs_path, nfs_data.hostname);
	printk(KERN_NOTICE "Root-NFS:     rsize = %d, wsize = %d, timeo = %d, retrans = %d\n",
		nfs_data.rsize, nfs_data.wsize, nfs_data.timeo, nfs_data.retrans);
	printk(KERN_NOTICE "Root-NFS:     acreg (min,max) = (%d,%d), acdir (min,max) = (%d,%d)\n",
		nfs_data.acregmin, nfs_data.acregmax,
		nfs_data.acdirmin, nfs_data.acdirmax);
	printk(KERN_NOTICE "Root-NFS:     nfsd port = %d, mountd port = %d, flags = %08x\n",
		nfs_port, mount_port, nfs_data.flags);
}
#endif


int __init root_nfs_init(void)
{
#ifdef NFSROOT_DEBUG
	nfs_debug |= NFSDBG_ROOT;
#endif

	/*
	 * Decode the root directory path name and NFS options from
	 * the kernel command line. This has to go here in order to
	 * be able to use the client IP address for the remote root
	 * directory (necessary for pure RARP booting).
	 */
	if (root_nfs_name(nfs_root_name) < 0 ||
	    root_nfs_addr() < 0)
		return -1;

#ifdef NFSROOT_DEBUG
	root_nfs_print();
#endif

	return 0;
}


/*
 *  Parse NFS server and directory information passed on the kernel
 *  command line.
 */
int __init nfs_root_setup(char *line)
{
	ROOT_DEV = MKDEV(UNNAMED_MAJOR, 255);
	if (line[0] == '/' || line[0] == ',' || (line[0] >= '0' && line[0] <= '9')) {
		strncpy(nfs_root_name, line, sizeof(nfs_root_name));
		nfs_root_name[sizeof(nfs_root_name)-1] = '\0';
	} else {
		int n = strlen(line) + strlen(NFS_ROOT);
		if (n >= sizeof(nfs_root_name))
			line[sizeof(nfs_root_name) - strlen(NFS_ROOT) - 1] = '\0';
		sprintf(nfs_root_name, NFS_ROOT, line);
	}
	root_nfs_parse_addr(nfs_root_name);
	return 1;
}

__setup("nfsroot=", nfs_root_setup);

/***************************************************************************

	       Routines to actually mount the root directory

 ***************************************************************************/

/*
 *  Construct sockaddr_in from address and port number.
 */
static inline void
set_sockaddr(struct sockaddr_in *sin, __u32 addr, __u16 port)
{
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = addr;
	sin->sin_port = port;
}

/*
 *  Query server portmapper for the port of a daemon program.
 */
static int __init root_nfs_getport(int program, int version, int proto)
{
	struct sockaddr_in sin;

	printk(KERN_NOTICE "Looking up port of RPC %d/%d on %s\n",
		program, version, in_ntoa(servaddr));
	set_sockaddr(&sin, servaddr, 0);
	return rpc_getport_external(&sin, program, version, proto);
}


/*
 *  Use portmapper to find mountd and nfsd port numbers if not overriden
 *  by the user. Use defaults if portmapper is not available.
 *  XXX: Is there any nfs server with no portmapper?
 */
static int __init root_nfs_ports(void)
{
	int port;
	int nfsd_ver, mountd_ver;
	int nfsd_port, mountd_port;
	int proto;

	if (nfs_data.flags & NFS_MOUNT_VER3) {
		nfsd_ver = NFS3_VERSION;
		mountd_ver = NFS_MNT3_VERSION;
		nfsd_port = NFS_PORT;
		mountd_port = NFS_MNT_PORT;
	} else {
		nfsd_ver = NFS2_VERSION;
		mountd_ver = NFS_MNT_VERSION;
		nfsd_port = NFS_PORT;
		mountd_port = NFS_MNT_PORT;
	}

	proto = (nfs_data.flags & NFS_MOUNT_TCP) ? IPPROTO_TCP : IPPROTO_UDP;

	if (nfs_port < 0) {
		if ((port = root_nfs_getport(NFS_PROGRAM, nfsd_ver, proto)) < 0) {
			printk(KERN_ERR "Root-NFS: Unable to get nfsd port "
					"number from server, using default\n");
			port = nfsd_port;
		}
		nfs_port = htons(port);
		dprintk("Root-NFS: Portmapper on server returned %d "
			"as nfsd port\n", port);
	}

	if ((port = root_nfs_getport(NFS_MNT_PROGRAM, nfsd_ver, proto)) < 0) {
		printk(KERN_ERR "Root-NFS: Unable to get mountd port "
				"number from server, using default\n");
		port = mountd_port;
	}
	mount_port = htons(port);
	dprintk("Root-NFS: mountd port is %d\n", port);

	return 0;
}


/*
 *  Get a file handle from the server for the directory which is to be
 *  mounted.
 */
static int __init root_nfs_get_handle(void)
{
	struct sockaddr_in sin;
	int status;

	set_sockaddr(&sin, servaddr, mount_port);
	if (nfs_data.flags & NFS_MOUNT_VER3)
		status = nfs3_mount(&sin, nfs_path, &nfs_data.root);
	else
		status = nfs_mount(&sin, nfs_path, &nfs_data.root);
	if (status < 0)
		printk(KERN_ERR "Root-NFS: Server returned error %d "
				"while mounting %s\n", status, nfs_path);

	return status;
}

/*
 *  Get the NFS port numbers and file handle, and return the prepared 'data'
 *  argument for ->read_super() if everything went OK. Return NULL otherwise.
 */
void * __init nfs_root_data(void)
{
	if (root_nfs_init() < 0
	 || root_nfs_ports() < 0
	 || root_nfs_get_handle() < 0)
		return NULL;
	set_sockaddr((struct sockaddr_in *) &nfs_data.addr, servaddr, nfs_port);
	return (void*)&nfs_data;
}
