/*
 * sysctl.h: General linux system control interface
 *
 * Begun 24 March 1995, Stephen Tweedie
 *
 ****************************************************************
 ****************************************************************
 **
 **  WARNING:  
 **  The values in this file are exported to user space via 
 **  the sysctl() binary interface.  Do *NOT* change the 
 **  numbering of any existing values here, and do not change
 **  any numbers within any one set of values.  If you have
 **  to redefine an existing interface, use a new number for it.
 **  The kernel will then return ENOTDIR to any application using
 **  the old binary interface.
 **
 **  --sct
 **
 ****************************************************************
 ****************************************************************
 */

#ifndef _LINUX_SYSCTL_H
#define _LINUX_SYSCTL_H

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/list.h>

struct file;

#define CTL_MAXNAME 10

struct __sysctl_args {
	int *name;
	int nlen;
	void *oldval;
	size_t *oldlenp;
	void *newval;
	size_t newlen;
	unsigned long __unused[4];
};

/* Define sysctl names first */

/* Top-level names: */

/* For internal pattern-matching use only: */
#ifdef __KERNEL__
#define CTL_ANY		-1	/* Matches any name */
#define CTL_NONE	0
#endif

enum
{
	CTL_KERN=1,		/* General kernel info and control */
	CTL_VM=2,		/* VM management */
	CTL_NET=3,		/* Networking */
	CTL_PROC=4,		/* Process info */
	CTL_FS=5,		/* Filesystems */
	CTL_DEBUG=6,		/* Debugging */
	CTL_DEV=7,		/* Devices */
	CTL_BUS=8		/* Buses */
};

/* CTL_BUS names: */
enum
{
	BUS_ISA=1		/* ISA */
};

/* CTL_KERN names: */
enum
{
	KERN_OSTYPE=1,		/* string: system version */
	KERN_OSRELEASE=2,	/* string: system release */
	KERN_OSREV=3,		/* int: system revision */
	KERN_VERSION=4,		/* string: compile time info */
	KERN_SECUREMASK=5,	/* struct: maximum rights mask */
	KERN_PROF=6,		/* table: profiling information */
	KERN_NODENAME=7,
	KERN_DOMAINNAME=8,

	KERN_CAP_BSET=14,	/* int: capability bounding set */
	KERN_PANIC=15,		/* int: panic timeout */
	KERN_REALROOTDEV=16,	/* real root device to mount after initrd */

	KERN_SPARC_REBOOT=21,	/* reboot command on Sparc */
	KERN_CTLALTDEL=22,	/* int: allow ctl-alt-del to reboot */
	KERN_PRINTK=23,		/* struct: control printk logging parameters */
	KERN_NAMETRANS=24,	/* Name translation */
	KERN_PPC_HTABRECLAIM=25, /* turn htab reclaimation on/off on PPC */
	KERN_PPC_ZEROPAGED=26,	/* turn idle page zeroing on/off on PPC */
	KERN_PPC_POWERSAVE_NAP=27, /* use nap mode for power saving */
	KERN_MODPROBE=28,
	KERN_SG_BIG_BUFF=29,
	KERN_ACCT=30,		/* BSD process accounting parameters */
	KERN_PPC_L2CR=31,	/* l2cr register on PPC */

	KERN_RTSIGNR=32,	/* Number of rt sigs queued */
	KERN_RTSIGMAX=33,	/* Max queuable */
	
	KERN_SHMMAX=34,         /* long: Maximum shared memory segment */
	KERN_MSGMAX=35,         /* int: Maximum size of a messege */
	KERN_MSGMNB=36,         /* int: Maximum message queue size */
	KERN_MSGPOOL=37,        /* int: Maximum system message pool size */
	KERN_SYSRQ=38,		/* int: Sysreq enable */
	KERN_MAX_THREADS=39,	/* int: Maximum nr of threads in the system */
 	KERN_RANDOM=40,		/* Random driver */
 	KERN_SHMALL=41,		/* int: Maximum size of shared memory */
 	KERN_MSGMNI=42,		/* int: msg queue identifiers */
 	KERN_SEM=43,		/* struct: sysv semaphore limits */
 	KERN_SPARC_STOP_A=44,	/* int: Sparc Stop-A enable */
 	KERN_SHMMNI=45,		/* int: shm array identifiers */
	KERN_OVERFLOWUID=46,	/* int: overflow UID */
	KERN_OVERFLOWGID=47,	/* int: overflow GID */
	KERN_SHMPATH=48,	/* string: path to shm fs */
	KERN_HOTPLUG=49,	/* string: path to hotplug policy agent */
};


/* CTL_VM names: */
enum
{
	VM_SWAPCTL=1,		/* struct: Set vm swapping control */
	VM_SWAPOUT=2,		/* int: Linear or sqrt() swapout for hogs */
	VM_FREEPG=3,		/* struct: Set free page thresholds */
	VM_BDFLUSH=4,		/* struct: Control buffer cache flushing */
	VM_OVERCOMMIT_MEMORY=5,	/* Turn off the virtual memory safety limit */
	VM_BUFFERMEM=6,		/* struct: Set buffer memory thresholds */
	VM_PAGECACHE=7,		/* struct: Set cache memory thresholds */
	VM_PAGERDAEMON=8,	/* struct: Control kswapd behaviour */
	VM_PGT_CACHE=9,		/* struct: Set page table cache parameters */
	VM_PAGE_CLUSTER=10	/* int: set number of pages to swap together */
};


/* CTL_NET names: */
enum
{
	NET_CORE=1,
	NET_ETHER=2,
	NET_802=3,
	NET_UNIX=4,
	NET_IPV4=5,
	NET_IPX=6,
	NET_ATALK=7,
	NET_NETROM=8,
	NET_AX25=9,
	NET_BRIDGE=10,
	NET_ROSE=11,
	NET_IPV6=12,
	NET_X25=13,
	NET_TR=14,
	NET_DECNET=15,
	NET_ECONET=16,
	NET_KHTTPD=17
};

/* /proc/sys/kernel/random */
enum
{
	RANDOM_POOLSIZE=1,
	RANDOM_ENTROPY_COUNT=2,
	RANDOM_READ_THRESH=3,
	RANDOM_WRITE_THRESH=4,
	RANDOM_BOOT_ID=5,
	RANDOM_UUID=6
};

/* /proc/sys/bus/isa */
enum
{
	BUS_ISA_MEM_BASE=1,
	BUS_ISA_PORT_BASE=2,
	BUS_ISA_PORT_SHIFT=3
};

/* /proc/sys/net/core */
enum
{
	NET_CORE_WMEM_MAX=1,
	NET_CORE_RMEM_MAX=2,
	NET_CORE_WMEM_DEFAULT=3,
	NET_CORE_RMEM_DEFAULT=4,
/* was	NET_CORE_DESTROY_DELAY */
	NET_CORE_MAX_BACKLOG=6,
	NET_CORE_FASTROUTE=7,
	NET_CORE_MSG_COST=8,
	NET_CORE_MSG_BURST=9,
	NET_CORE_OPTMEM_MAX=10,
	NET_CORE_HOT_LIST_LENGTH=11,
	NET_CORE_DIVERT_VERSION=12,
	NET_CORE_NO_CONG_THRESH=13,
	NET_CORE_NO_CONG=14,
	NET_CORE_LO_CONG=15,
	NET_CORE_MOD_CONG=16
};

/* /proc/sys/net/ethernet */

/* /proc/sys/net/802 */

/* /proc/sys/net/unix */

enum
{
	NET_UNIX_DESTROY_DELAY=1,
	NET_UNIX_DELETE_DELAY=2,
	NET_UNIX_MAX_DGRAM_QLEN=3,
};

/* /proc/sys/net/ipv4 */
enum
{
	/* v2.0 compatibile variables */
	NET_IPV4_FORWARD=8,
	NET_IPV4_DYNADDR=9,

	NET_IPV4_CONF=16,
	NET_IPV4_NEIGH=17,
	NET_IPV4_ROUTE=18,
	NET_IPV4_FIB_HASH=19,

	NET_IPV4_TCP_TIMESTAMPS=33,
	NET_IPV4_TCP_WINDOW_SCALING=34,
	NET_IPV4_TCP_SACK=35,
	NET_IPV4_TCP_RETRANS_COLLAPSE=36,
	NET_IPV4_DEFAULT_TTL=37,
	NET_IPV4_AUTOCONFIG=38,
	NET_IPV4_NO_PMTU_DISC=39,
	NET_IPV4_TCP_SYN_RETRIES=40,
	NET_IPV4_IPFRAG_HIGH_THRESH=41,
	NET_IPV4_IPFRAG_LOW_THRESH=42,
	NET_IPV4_IPFRAG_TIME=43,
	NET_IPV4_TCP_MAX_KA_PROBES=44,
	NET_IPV4_TCP_KEEPALIVE_TIME=45,
	NET_IPV4_TCP_KEEPALIVE_PROBES=46,
	NET_IPV4_TCP_RETRIES1=47,
	NET_IPV4_TCP_RETRIES2=48,
	NET_IPV4_TCP_FIN_TIMEOUT=49,
	NET_IPV4_IP_MASQ_DEBUG=50,
	NET_TCP_SYNCOOKIES=51,
	NET_TCP_STDURG=52,
	NET_TCP_RFC1337=53,
	NET_TCP_SYN_TAILDROP=54,
	NET_TCP_MAX_SYN_BACKLOG=55,
	NET_IPV4_LOCAL_PORT_RANGE=56,
	NET_IPV4_ICMP_ECHO_IGNORE_ALL=57,
	NET_IPV4_ICMP_ECHO_IGNORE_BROADCASTS=58,
	NET_IPV4_ICMP_SOURCEQUENCH_RATE=59,
	NET_IPV4_ICMP_DESTUNREACH_RATE=60,
	NET_IPV4_ICMP_TIMEEXCEED_RATE=61,
	NET_IPV4_ICMP_PARAMPROB_RATE=62,
	NET_IPV4_ICMP_ECHOREPLY_RATE=63,
	NET_IPV4_ICMP_IGNORE_BOGUS_ERROR_RESPONSES=64,
	NET_IPV4_IGMP_MAX_MEMBERSHIPS=65,
	NET_TCP_TW_RECYCLE=66,
	NET_IPV4_ALWAYS_DEFRAG=67,
	NET_IPV4_TCP_KEEPALIVE_INTVL=68,
	NET_IPV4_INET_PEER_THRESHOLD=69,
	NET_IPV4_INET_PEER_MINTTL=70,
	NET_IPV4_INET_PEER_MAXTTL=71,
	NET_IPV4_INET_PEER_GC_MINTIME=72,
	NET_IPV4_INET_PEER_GC_MAXTIME=73,
	NET_TCP_ORPHAN_RETRIES=74,
	NET_TCP_ABORT_ON_OVERFLOW=75,
	NET_TCP_SYNACK_RETRIES=76,
	NET_TCP_MAX_ORPHANS=77,
	NET_TCP_MAX_TW_BUCKETS=78,
	NET_TCP_FACK=79,
	NET_TCP_REORDERING=80,
	NET_TCP_ECN=81,
	NET_TCP_DSACK=82,
	NET_TCP_MEM=83,
	NET_TCP_WMEM=84,
	NET_TCP_RMEM=85,
	NET_TCP_APP_WIN=86,
	NET_TCP_ADV_WIN_SCALE=87,
	NET_IPV4_NONLOCAL_BIND=88,
};

enum {
	NET_IPV4_ROUTE_FLUSH=1,
	NET_IPV4_ROUTE_MIN_DELAY=2,
	NET_IPV4_ROUTE_MAX_DELAY=3,
	NET_IPV4_ROUTE_GC_THRESH=4,
	NET_IPV4_ROUTE_MAX_SIZE=5,
	NET_IPV4_ROUTE_GC_MIN_INTERVAL=6,
	NET_IPV4_ROUTE_GC_TIMEOUT=7,
	NET_IPV4_ROUTE_GC_INTERVAL=8,
	NET_IPV4_ROUTE_REDIRECT_LOAD=9,
	NET_IPV4_ROUTE_REDIRECT_NUMBER=10,
	NET_IPV4_ROUTE_REDIRECT_SILENCE=11,
	NET_IPV4_ROUTE_ERROR_COST=12,
	NET_IPV4_ROUTE_ERROR_BURST=13,
	NET_IPV4_ROUTE_GC_ELASTICITY=14,
	NET_IPV4_ROUTE_MTU_EXPIRES=15,
	NET_IPV4_ROUTE_MIN_PMTU=16,
	NET_IPV4_ROUTE_MIN_ADVMSS=17
};

enum
{
	NET_PROTO_CONF_ALL=-2,
	NET_PROTO_CONF_DEFAULT=-3

	/* And device ifindices ... */
};

enum
{
	NET_IPV4_CONF_FORWARDING=1,
	NET_IPV4_CONF_MC_FORWARDING=2,
	NET_IPV4_CONF_PROXY_ARP=3,
	NET_IPV4_CONF_ACCEPT_REDIRECTS=4,
	NET_IPV4_CONF_SECURE_REDIRECTS=5,
	NET_IPV4_CONF_SEND_REDIRECTS=6,
	NET_IPV4_CONF_SHARED_MEDIA=7,
	NET_IPV4_CONF_RP_FILTER=8,
	NET_IPV4_CONF_ACCEPT_SOURCE_ROUTE=9,
	NET_IPV4_CONF_BOOTP_RELAY=10,
	NET_IPV4_CONF_LOG_MARTIANS=11,
	NET_IPV4_CONF_TAG=12
};

/* /proc/sys/net/ipv6 */
enum {
	NET_IPV6_CONF=16,
	NET_IPV6_NEIGH=17,
	NET_IPV6_ROUTE=18
};

enum {
	NET_IPV6_ROUTE_FLUSH=1,
	NET_IPV6_ROUTE_GC_THRESH=2,
	NET_IPV6_ROUTE_MAX_SIZE=3,
	NET_IPV6_ROUTE_GC_MIN_INTERVAL=4,
	NET_IPV6_ROUTE_GC_TIMEOUT=5,
	NET_IPV6_ROUTE_GC_INTERVAL=6,
	NET_IPV6_ROUTE_GC_ELASTICITY=7,
	NET_IPV6_ROUTE_MTU_EXPIRES=8,
	NET_IPV6_ROUTE_MIN_ADVMSS=9
};

enum {
	NET_IPV6_FORWARDING=1,
	NET_IPV6_HOP_LIMIT=2,
	NET_IPV6_MTU=3,
	NET_IPV6_ACCEPT_RA=4,
	NET_IPV6_ACCEPT_REDIRECTS=5,
	NET_IPV6_AUTOCONF=6,
	NET_IPV6_DAD_TRANSMITS=7,
	NET_IPV6_RTR_SOLICITS=8,
	NET_IPV6_RTR_SOLICIT_INTERVAL=9,
	NET_IPV6_RTR_SOLICIT_DELAY=10
};

/* /proc/sys/net/<protocol>/neigh/<dev> */
enum {
	NET_NEIGH_MCAST_SOLICIT=1,
	NET_NEIGH_UCAST_SOLICIT=2,
	NET_NEIGH_APP_SOLICIT=3,
	NET_NEIGH_RETRANS_TIME=4,
	NET_NEIGH_REACHABLE_TIME=5,
	NET_NEIGH_DELAY_PROBE_TIME=6,
	NET_NEIGH_GC_STALE_TIME=7,
	NET_NEIGH_UNRES_QLEN=8,
	NET_NEIGH_PROXY_QLEN=9,
	NET_NEIGH_ANYCAST_DELAY=10,
	NET_NEIGH_PROXY_DELAY=11,
	NET_NEIGH_LOCKTIME=12,
	NET_NEIGH_GC_INTERVAL=13,
	NET_NEIGH_GC_THRESH1=14,
	NET_NEIGH_GC_THRESH2=15,
	NET_NEIGH_GC_THRESH3=16
};

/* /proc/sys/net/ipx */


/* /proc/sys/net/appletalk */
enum {
	NET_ATALK_AARP_EXPIRY_TIME=1,
	NET_ATALK_AARP_TICK_TIME=2,
	NET_ATALK_AARP_RETRANSMIT_LIMIT=3,
	NET_ATALK_AARP_RESOLVE_TIME=4
};


/* /proc/sys/net/netrom */
enum {
	NET_NETROM_DEFAULT_PATH_QUALITY=1,
	NET_NETROM_OBSOLESCENCE_COUNT_INITIALISER=2,
	NET_NETROM_NETWORK_TTL_INITIALISER=3,
	NET_NETROM_TRANSPORT_TIMEOUT=4,
	NET_NETROM_TRANSPORT_MAXIMUM_TRIES=5,
	NET_NETROM_TRANSPORT_ACKNOWLEDGE_DELAY=6,
	NET_NETROM_TRANSPORT_BUSY_DELAY=7,
	NET_NETROM_TRANSPORT_REQUESTED_WINDOW_SIZE=8,
	NET_NETROM_TRANSPORT_NO_ACTIVITY_TIMEOUT=9,
	NET_NETROM_ROUTING_CONTROL=10,
	NET_NETROM_LINK_FAILS_COUNT=11
};

/* /proc/sys/net/ax25 */
enum {
	NET_AX25_IP_DEFAULT_MODE=1,
	NET_AX25_DEFAULT_MODE=2,
	NET_AX25_BACKOFF_TYPE=3,
	NET_AX25_CONNECT_MODE=4,
	NET_AX25_STANDARD_WINDOW=5,
	NET_AX25_EXTENDED_WINDOW=6,
	NET_AX25_T1_TIMEOUT=7,
	NET_AX25_T2_TIMEOUT=8,
	NET_AX25_T3_TIMEOUT=9,
	NET_AX25_IDLE_TIMEOUT=10,
	NET_AX25_N2=11,
	NET_AX25_PACLEN=12,
	NET_AX25_PROTOCOL=13,
	NET_AX25_DAMA_SLAVE_TIMEOUT=14
};

/* /proc/sys/net/rose */
enum {
	NET_ROSE_RESTART_REQUEST_TIMEOUT=1,
	NET_ROSE_CALL_REQUEST_TIMEOUT=2,
	NET_ROSE_RESET_REQUEST_TIMEOUT=3,
	NET_ROSE_CLEAR_REQUEST_TIMEOUT=4,
	NET_ROSE_ACK_HOLD_BACK_TIMEOUT=5,
	NET_ROSE_ROUTING_CONTROL=6,
	NET_ROSE_LINK_FAIL_TIMEOUT=7,
	NET_ROSE_MAX_VCS=8,
	NET_ROSE_WINDOW_SIZE=9,
	NET_ROSE_NO_ACTIVITY_TIMEOUT=10
};

/* /proc/sys/net/x25 */
enum {
	NET_X25_RESTART_REQUEST_TIMEOUT=1,
	NET_X25_CALL_REQUEST_TIMEOUT=2,
	NET_X25_RESET_REQUEST_TIMEOUT=3,
	NET_X25_CLEAR_REQUEST_TIMEOUT=4,
	NET_X25_ACK_HOLD_BACK_TIMEOUT=5
};

/* /proc/sys/net/token-ring */
enum
{
	NET_TR_RIF_TIMEOUT=1
};

/* /proc/sys/net/decnet/ */
enum {
	NET_DECNET_NODE_TYPE = 1,
	NET_DECNET_NODE_ADDRESS = 2,
	NET_DECNET_NODE_NAME = 3,
	NET_DECNET_DEFAULT_DEVICE = 4,
	NET_DECNET_TIME_WAIT = 5,
	NET_DECNET_DN_COUNT = 6,
	NET_DECNET_DI_COUNT = 7,
	NET_DECNET_DR_COUNT = 8,
	NET_DECNET_DST_GC_INTERVAL = 9,
	NET_DECNET_CONF = 10,
	NET_DECNET_DEBUG_LEVEL = 255
};

/* /proc/sys/net/khttpd/ */
enum {
	NET_KHTTPD_DOCROOT	= 1,
	NET_KHTTPD_START	= 2,
	NET_KHTTPD_STOP		= 3,
	NET_KHTTPD_UNLOAD	= 4,
	NET_KHTTPD_CLIENTPORT	= 5,
	NET_KHTTPD_PERMREQ	= 6,
	NET_KHTTPD_PERMFORBID	= 7,
	NET_KHTTPD_LOGGING	= 8,
	NET_KHTTPD_SERVERPORT	= 9,
	NET_KHTTPD_DYNAMICSTRING= 10,
	NET_KHTTPD_SLOPPYMIME   = 11,
	NET_KHTTPD_THREADS	= 12,
	NET_KHTTPD_MAXCONNECT	= 13
};

/* /proc/sys/net/decnet/conf/<dev> */
enum {
	NET_DECNET_CONF_LOOPBACK = -2,
	NET_DECNET_CONF_DDCMP = -3,
	NET_DECNET_CONF_PPP = -4,
	NET_DECNET_CONF_X25 = -5,
	NET_DECNET_CONF_GRE = -6,
	NET_DECNET_CONF_ETHER = -7

	/* ... and ifindex of devices */
};

/* /proc/sys/net/decnet/conf/<dev>/ */
enum {
	NET_DECNET_CONF_DEV_PRIORITY = 1,
	NET_DECNET_CONF_DEV_T1 = 2,
	NET_DECNET_CONF_DEV_T2 = 3,
	NET_DECNET_CONF_DEV_T3 = 4,
	NET_DECNET_CONF_DEV_FORWARDING = 5,
	NET_DECNET_CONF_DEV_BLKSIZE = 6,
	NET_DECNET_CONF_DEV_STATE = 7
};

/* CTL_PROC names: */

/* CTL_FS names: */
enum
{
	FS_NRINODE=1,	/* int:current number of allocated inodes */
	FS_STATINODE=2,
	FS_MAXINODE=3,	/* int:maximum number of inodes that can be allocated */
	FS_NRDQUOT=4,	/* int:current number of allocated dquots */
	FS_MAXDQUOT=5,	/* int:maximum number of dquots that can be allocated */
	FS_NRFILE=6,	/* int:current number of allocated filedescriptors */
	FS_MAXFILE=7,	/* int:maximum number of filedescriptors that can be allocated */
	FS_DENTRY=8,
	FS_NRSUPER=9,	/* int:current number of allocated super_blocks */
	FS_MAXSUPER=10,	/* int:maximum number of super_blocks that can be allocated */
	FS_OVERFLOWUID=11,	/* int: overflow UID */
	FS_OVERFLOWGID=12,	/* int: overflow GID */
	FS_LEASES=13,	/* int: leases enabled */
	FS_DIR_NOTIFY=14,	/* int: directory notification enabled */
	FS_LEASE_TIME=15,	/* int: maximum time to wait for a lease break */
};

/* CTL_DEBUG names: */

/* CTL_DEV names: */
enum {
	DEV_CDROM=1,
	DEV_HWMON=2,
	DEV_PARPORT=3,
	DEV_RAID=4,
	DEV_MAC_HID=5
};

/* /proc/sys/dev/cdrom */
enum {
	DEV_CDROM_INFO=1,
	DEV_CDROM_AUTOCLOSE=2,
	DEV_CDROM_AUTOEJECT=3,
	DEV_CDROM_DEBUG=4,
	DEV_CDROM_LOCK=5,
	DEV_CDROM_CHECK_MEDIA=6
};

/* /proc/sys/dev/parport */
enum {
	DEV_PARPORT_DEFAULT=-3
};

/* /proc/sys/dev/raid */
enum {
	DEV_RAID_SPEED_LIMIT_MIN=1,
	DEV_RAID_SPEED_LIMIT_MAX=2
};

/* /proc/sys/dev/parport/default */
enum {
	DEV_PARPORT_DEFAULT_TIMESLICE=1,
	DEV_PARPORT_DEFAULT_SPINTIME=2
};

/* /proc/sys/dev/parport/parport n */
enum {
	DEV_PARPORT_SPINTIME=1,
	DEV_PARPORT_BASE_ADDR=2,
	DEV_PARPORT_IRQ=3,
	DEV_PARPORT_DMA=4,
	DEV_PARPORT_MODES=5,
	DEV_PARPORT_DEVICES=6,
	DEV_PARPORT_AUTOPROBE=16
};

/* /proc/sys/dev/parport/parport n/devices/ */
enum {
	DEV_PARPORT_DEVICES_ACTIVE=-3,
};

/* /proc/sys/dev/parport/parport n/devices/device n */
enum {
	DEV_PARPORT_DEVICE_TIMESLICE=1,
};

/* /proc/sys/dev/mac_hid */
enum {
	DEV_MAC_HID_KEYBOARD_SENDS_LINUX_KEYCODES=1,
	DEV_MAC_HID_KEYBOARD_LOCK_KEYCODES=2,
	DEV_MAC_HID_MOUSE_BUTTON_EMULATION=3,
	DEV_MAC_HID_MOUSE_BUTTON2_KEYCODE=4,
	DEV_MAC_HID_MOUSE_BUTTON3_KEYCODE=5,
	DEV_MAC_HID_ADB_MOUSE_SENDS_KEYCODES=6
};

#ifdef __KERNEL__

extern asmlinkage long sys_sysctl(struct __sysctl_args *);
extern void sysctl_init(void);

typedef struct ctl_table ctl_table;

typedef int ctl_handler (ctl_table *table, int *name, int nlen,
			 void *oldval, size_t *oldlenp,
			 void *newval, size_t newlen, 
			 void **context);

typedef int proc_handler (ctl_table *ctl, int write, struct file * filp,
			  void *buffer, size_t *lenp);

extern int proc_dostring(ctl_table *, int, struct file *,
			 void *, size_t *);
extern int proc_dointvec(ctl_table *, int, struct file *,
			 void *, size_t *);
extern int proc_dointvec_bset(ctl_table *, int, struct file *,
			      void *, size_t *);
extern int proc_dointvec_minmax(ctl_table *, int, struct file *,
				void *, size_t *);
extern int proc_dointvec_jiffies(ctl_table *, int, struct file *,
				 void *, size_t *);
extern int proc_doulongvec_minmax(ctl_table *, int, struct file *,
				  void *, size_t *);
extern int proc_doulongvec_ms_jiffies_minmax(ctl_table *table, int,
				      struct file *, void *, size_t *);

extern int do_sysctl (int *name, int nlen,
		      void *oldval, size_t *oldlenp,
		      void *newval, size_t newlen);

extern int do_sysctl_strategy (ctl_table *table, 
			       int *name, int nlen,
			       void *oldval, size_t *oldlenp,
			       void *newval, size_t newlen, void ** context);

extern ctl_handler sysctl_string;
extern ctl_handler sysctl_intvec;
extern ctl_handler sysctl_jiffies;


/*
 * Register a set of sysctl names by calling register_sysctl_table
 * with an initialised array of ctl_table's.  An entry with zero
 * ctl_name terminates the table.  table->de will be set up by the
 * registration and need not be initialised in advance.
 *
 * sysctl names can be mirrored automatically under /proc/sys.  The
 * procname supplied controls /proc naming.
 *
 * The table's mode will be honoured both for sys_sysctl(2) and
 * proc-fs access.
 *
 * Leaf nodes in the sysctl tree will be represented by a single file
 * under /proc; non-leaf nodes will be represented by directories.  A
 * null procname disables /proc mirroring at this node.
 * 
 * sysctl(2) can automatically manage read and write requests through
 * the sysctl table.  The data and maxlen fields of the ctl_table
 * struct enable minimal validation of the values being written to be
 * performed, and the mode field allows minimal authentication.
 * 
 * More sophisticated management can be enabled by the provision of a
 * strategy routine with the table entry.  This will be called before
 * any automatic read or write of the data is performed.
 * 
 * The strategy routine may return:
 * <0: Error occurred (error is passed to user process)
 * 0:  OK - proceed with automatic read or write.
 * >0: OK - read or write has been done by the strategy routine, so 
 *     return immediately.
 * 
 * There must be a proc_handler routine for any terminal nodes
 * mirrored under /proc/sys (non-terminals are handled by a built-in
 * directory handler).  Several default handlers are available to
 * cover common cases.
 */

/* A sysctl table is an array of struct ctl_table: */
struct ctl_table 
{
	int ctl_name;			/* Binary ID */
	const char *procname;		/* Text ID for /proc/sys, or zero */
	void *data;
	int maxlen;
	mode_t mode;
	ctl_table *child;
	proc_handler *proc_handler;	/* Callback for text formatting */
	ctl_handler *strategy;		/* Callback function for all r/w */
	struct proc_dir_entry *de;	/* /proc control block */
	void *extra1;
	void *extra2;
};

/* struct ctl_table_header is used to maintain dynamic lists of
   ctl_table trees. */
struct ctl_table_header
{
	ctl_table *ctl_table;
	struct list_head ctl_entry;
};

struct ctl_table_header * register_sysctl_table(ctl_table * table, 
						int insert_at_head);
void unregister_sysctl_table(struct ctl_table_header * table);

#else /* __KERNEL__ */

#endif /* __KERNEL__ */

#endif /* _LINUX_SYSCTL_H */
