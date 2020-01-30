/*
 * originally based on the dummy device.
 *
 * Copyright 1999, Thomas Davis, tadavis@lbl.gov.
 * Licensed under the GPL. Based on dummy.c, and eql.c devices.
 *
 * bonding.c: an Ethernet Bonding driver
 *
 * This is useful to talk to a Cisco EtherChannel compatible equipment:
 *	Cisco 5500
 *	Sun Trunking (Solaris)
 *	Alteon AceDirector Trunks
 *	Linux Bonding
 *	and probably many L2 switches ...
 *
 * How it works:
 *    ifconfig bond0 ipaddress netmask up
 *      will setup a network device, with an ip address.  No mac address
 *	will be assigned at this time.  The hw mac address will come from
 *	the first slave bonded to the channel.  All slaves will then use
 *	this hw mac address.
 *
 *    ifconfig bond0 down
 *         will release all slaves, marking them as down.
 *
 *    ifenslave bond0 eth0
 *	will attach eth0 to bond0 as a slave.  eth0 hw mac address will either
 *	a: be used as initial mac address
 *	b: if a hw mac address already is there, eth0's hw mac address
 *	   will then be set from bond0.
 *
 * v0.1 - first working version.
 * v0.2 - changed stats to be calculated by summing slaves stats.
 *
 * Changes:
 * Arnaldo Carvalho de Melo <acme@conectiva.com.br>
 * - fix leaks on failure at bond_init
 *
 * 2000/09/30 - Willy Tarreau <willy at meta-x.org>
 *     - added trivial code to release a slave device.
 *     - fixed security bug (CAP_NET_ADMIN not checked)
 *     - implemented MII link monitoring to disable dead links :
 *       All MII capable slaves are checked every <miimon> milliseconds
 *       (100 ms seems good). This value can be changed by passing it to
 *       insmod. A value of zero disables the monitoring (default).
 *     - fixed an infinite loop in bond_xmit_roundrobin() when there's no
 *       good slave.
 *     - made the code hopefully SMP safe
 *
 * 2000/10/03 - Willy Tarreau <willy at meta-x.org>
 *     - optimized slave lists based on relevant suggestions from Thomas Davis
 *     - implemented active-backup method to obtain HA with two switches:
 *       stay as long as possible on the same active interface, while we
 *       also monitor the backup one (MII link status) because we want to know
 *       if we are able to switch at any time. ( pass "mode=1" to insmod )
 *     - lots of stress testings because we need it to be more robust than the
 *       wires ! :->
 *
 * 2000/10/09 - Willy Tarreau <willy at meta-x.org>
 *     - added up and down delays after link state change.
 *     - optimized the slaves chaining so that when we run forward, we never
 *       repass through the bond itself, but we can find it by searching
 *       backwards. Renders the deletion more difficult, but accelerates the
 *       scan.
 *     - smarter enslaving and releasing.
 *     - finer and more robust SMP locking
 *
 * 2000/10/17 - Willy Tarreau <willy at meta-x.org>
 *     - fixed two potential SMP race conditions
 *
 * 2000/10/18 - Willy Tarreau <willy at meta-x.org>
 *     - small fixes to the monitoring FSM in case of zero delays
 * 2000/11/01 - Willy Tarreau <willy at meta-x.org>
 *     - fixed first slave not automatically used in trunk mode.
 * 2000/11/10 : spelling of "EtherChannel" corrected.
 * 2000/11/13 : fixed a race condition in case of concurrent accesses to ioctl().
 * 2000/12/16 : fixed improper usage of rtnl_exlock_nowait().
 *
 * 2001/1/3 - Chad N. Tindel <ctindel at ieee dot org>
 *     - The bonding driver now simulates MII status monitoring, just like
 *       a normal network device.  It will show that the link is down iff
 *       every slave in the bond shows that their links are down.  If at least
 *       one slave is up, the bond's MII status will appear as up.
 *
 * 2001/2/7 - Chad N. Tindel <ctindel at ieee dot org>
 *     - Applications can now query the bond from user space to get
 *       information which may be useful.  They do this by calling
 *       the BOND_INFO_QUERY ioctl.  Once the app knows how many slaves
 *       are in the bond, it can call the BOND_SLAVE_INFO_QUERY ioctl to
 *       get slave specific information (# link failures, etc).  See
 *       <linux/if_bonding.h> for more details.  The structs of interest
 *       are ifbond and ifslave.
 *
 * 2001/4/5 - Chad N. Tindel <ctindel at ieee dot org>
 *     - Ported to 2.4 Kernel
 *
 * 2001/5/2 - Jeffrey E. Mast <jeff at mastfamily dot com>
 *     - When a device is detached from a bond, the slave device is no longer
 *       left thinking that is has a master.
 *
 * 2001/5/16 - Jeffrey E. Mast <jeff at mastfamily dot com>
 *     - memset did not appropriately initialized the bond rw_locks. Used
 *       rwlock_init to initialize to unlocked state to prevent deadlock when
 *       first attempting a lock
 *     - Called SET_MODULE_OWNER for bond device
 *
 * 2001/5/17 - Tim Anderson <tsa at mvista.com>
 *     - 2 paths for releasing for slave release; 1 through ioctl
 *       and 2) through close. Both paths need to release the same way.
 *     - the free slave in bond release is changing slave status before
 *       the free. The netdev_set_master() is intended to change slave state
 *       so it should not be done as part of the release process.
 *     - Simple rule for slave state at release: only the active in A/B and
 *       only one in the trunked case.
 *
 * 2001/6/01 - Tim Anderson <tsa at mvista.com>
 *     - Now call dev_close when releasing a slave so it doesn't screw up
 *       out routing table.
 *
 * 2001/6/01 - Chad N. Tindel <ctindel at ieee dot org>
 *     - Added /proc support for getting bond and slave information.
 *       Information is in /proc/net/<bond device>/info.
 *     - Changed the locking when calling bond_close to prevent deadlock.
 *
 * 2001/8/05 - Janice Girouard <girouard at us.ibm.com>
 *     - correct problem where refcnt of slave is not incremented in bond_ioctl
 *       so the system hangs when halting.
 *     - correct locking problem when unable to malloc in bond_enslave.
 *     - adding bond_xmit_xor logic.
 *     - adding multiple bond device support.
 *
 * 2001/8/13 - Erik Habbinga <erik_habbinga at hp dot com>
 *     - correct locking problem with rtnl_exlock_nowait
 *
 * 2001/8/23 - Janice Girouard <girouard at us.ibm.com>
 *     - bzero initial dev_bonds, to correct oops
 *     - convert SIOCDEVPRIVATE to new MII ioctl calls
 *
 * 2001/9/13 - Takao Indoh <indou dot takao at jp dot fujitsu dot com>
 *     - Add the BOND_CHANGE_ACTIVE ioctl implementation
 *
 * 2001/9/14 - Mark Huth <mhuth at mvista dot com>
 *     - Change MII_LINK_READY to not check for end of auto-negotiation,
 *       but only for an up link.
 *
 * 2001/9/20 - Chad N. Tindel <ctindel at ieee dot org>
 *     - Add the device field to bonding_t.  Previously the net_device
 *       corresponding to a bond wasn't available from the bonding_t
 *       structure.
 *
 * 2001/9/25 - Janice Girouard <girouard at us.ibm.com>
 *     - add arp_monitor for active backup mode
 *
 * 2001/10/23 - Takao Indoh <indou dot takao at jp dot fujitsu dot com>
 *     - Various memory leak fixes
 *
 * 2001/11/5 - Mark Huth <mark dot huth at mvista dot com>
 *     - Don't take rtnl lock in bond_mii_monitor as it deadlocks under
 *       certain hotswap conditions.
 *       Note:  this same change may be required in bond_arp_monitor ???
 *     - Remove possibility of calling bond_sethwaddr with NULL slave_dev ptr
 *     - Handle hot swap ethernet interface deregistration events to remove
 *       kernel oops following hot swap of enslaved interface
 *
 * 2002/1/2 - Chad N. Tindel <ctindel at ieee dot org>
 *     - Restore original slave flags at release time.
 *
 * 2002/02/18 - Erik Habbinga <erik_habbinga at hp dot com>
 *     - bond_release(): calling kfree on our_slave after call to
 *       bond_restore_slave_flags, not before
 *     - bond_enslave(): saving slave flags into original_flags before
 *       call to netdev_set_master, so the IFF_SLAVE flag doesn't end
 *       up in original_flags
 *
 * 2002/04/05 - Mark Smith <mark.smith at comdev dot cc> and
 *              Steve Mead <steve.mead at comdev dot cc>
 *     - Port Gleb Natapov's multicast support patchs from 2.4.12
 *       to 2.4.18 adding support for multicast.
 *
 * 2002/06/10 - Tony Cureington <tony.cureington * hp_com>
 *     - corrected uninitialized pointer (ifr.ifr_data) in bond_check_dev_link;
 *       actually changed function to use MIIPHY, then MIIREG, and finally
 *       ETHTOOL to determine the link status
 *     - fixed bad ifr_data pointer assignments in bond_ioctl
 *     - corrected mode 1 being reported as active-backup in bond_get_info;
 *       also added text to distinguish type of load balancing (rr or xor)
 *     - change arp_ip_target module param from "1-12s" (array of 12 ptrs)
 *       to "s" (a single ptr)
 *
 * 2002/08/30 - Jay Vosburgh <fubar at us dot ibm dot com>
 *     - Removed acquisition of xmit_lock in set_multicast_list; caused
 *       deadlock on SMP (lock is held by caller).
 *     - Revamped SIOCGMIIPHY, SIOCGMIIREG portion of bond_check_dev_link().
 *
 * 2002/09/18 - Jay Vosburgh <fubar at us dot ibm dot com>
 *     - Fixed up bond_check_dev_link() (and callers): removed some magic
 *	 numbers, banished local MII_ defines, wrapped ioctl calls to
 *	 prevent EFAULT errors
 *
 * 2002/9/30 - Jay Vosburgh <fubar at us dot ibm dot com>
 *     - make sure the ip target matches the arp_target before saving the
 *	 hw address.
 *
 * 2002/9/30 - Dan Eisner <eisner at 2robots dot com>
 *     - make sure my_ip is set before taking down the link, since
 *	 not all switches respond if the source ip is not set.
 *
 * 2002/10/8 - Janice Girouard <girouard at us dot ibm dot com>
 *     - read in the local ip address when enslaving a device
 *     - add primary support
 *     - make sure 2*arp_interval has passed when a new device
 *       is brought on-line before taking it down.
 *
 * 2002/09/11 - Philippe De Muyter <phdm at macqel dot be>
 *     - Added bond_xmit_broadcast logic.
 *     - Added bond_mode() support function.
 *
 * 2002/10/26 - Laurent Deniel <laurent.deniel at free.fr>
 *     - allow to register multicast addresses only on active slave
 *       (useful in active-backup mode)
 *     - add multicast module parameter
 *     - fix deletion of multicast groups after unloading module
 *
 * 2002/11/06 - Kameshwara Rayaprolu <kameshwara.rao * wipro_com>
 *     - Changes to prevent panic from closing the device twice; if we close
 *       the device in bond_release, we must set the original_flags to down
 *       so it won't be closed again by the network layer.
 *
 * 2002/11/07 - Tony Cureington <tony.cureington * hp_com>
 *     - Fix arp_target_hw_addr memory leak
 *     - Created activebackup_arp_monitor function to handle arp monitoring
 *       in active backup mode - the bond_arp_monitor had several problems...
 *       such as allowing slaves to tx arps sequentially without any delay
 *       for a response
 *     - Renamed bond_arp_monitor to loadbalance_arp_monitor and re-wrote
 *       this function to just handle arp monitoring in load-balancing mode;
 *       it is a lot more compact now
 *     - Changes to ensure one and only one slave transmits in active-backup
 *       mode
 *     - Robustesize parameters; warn users about bad combinations of
 *       parameters; also if miimon is specified and a network driver does
 *       not support MII or ETHTOOL, inform the user of this
 *     - Changes to support link_failure_count when in arp monitoring mode
 *     - Fix up/down delay reported in /proc
 *     - Added version; log version; make version available from "modinfo -d"
 *     - Fixed problem in bond_check_dev_link - if the first IOCTL (SIOCGMIIPH)
 *	 failed, the ETHTOOL ioctl never got a chance
 *
 * 2002/11/16 - Laurent Deniel <laurent.deniel at free.fr>
 *     - fix multicast handling in activebackup_arp_monitor
 *     - remove one unnecessary and confusing curr_active_slave == slave test
 *	 in activebackup_arp_monitor
 *
 *  2002/11/17 - Laurent Deniel <laurent.deniel at free.fr>
 *     - fix bond_slave_info_query when slave_id = num_slaves
 *
 *  2002/11/19 - Janice Girouard <girouard at us dot ibm dot com>
 *     - correct ifr_data reference.  Update ifr_data reference
 *       to mii_ioctl_data struct values to avoid confusion.
 *
 *  2002/11/22 - Bert Barbe <bert.barbe at oracle dot com>
 *      - Add support for multiple arp_ip_target
 *
 *  2002/12/13 - Jay Vosburgh <fubar at us dot ibm dot com>
 *	- Changed to allow text strings for mode and multicast, e.g.,
 *	  insmod bonding mode=active-backup.  The numbers still work.
 *	  One change: an invalid choice will cause module load failure,
 *	  rather than the previous behavior of just picking one.
 *	- Minor cleanups; got rid of dup ctype stuff, atoi function
 *
 * 2003/02/07 - Jay Vosburgh <fubar at us dot ibm dot com>
 *	- Added use_carrier module parameter that causes miimon to
 *	  use netif_carrier_ok() test instead of MII/ETHTOOL ioctls.
 *	- Minor cleanups; consolidated ioctl calls to one function.
 *
 * 2003/02/07 - Tony Cureington <tony.cureington * hp_com>
 *	- Fix bond_mii_monitor() logic error that could result in
 *	  bonding round-robin mode ignoring links after failover/recovery
 *
 * 2003/03/17 - Jay Vosburgh <fubar at us dot ibm dot com>
 *	- kmalloc fix (GFP_KERNEL to GFP_ATOMIC) reported by
 *	  Shmulik dot Hen at intel.com.
 *	- Based on discussion on mailing list, changed use of
 *	  update_slave_cnt(), created wrapper functions for adding/removing
 *	  slaves, changed bond_xmit_xor() to check slave_cnt instead of
 *	  checking slave and slave->dev (which only worked by accident).
 *	- Misc code cleanup: get arp_send() prototype from header file,
 *	  add max_bonds to bonding.txt.
 *
 * 2003/03/18 - Tsippy Mendelson <tsippy.mendelson at intel dot com> and
 *		Shmulik Hen <shmulik.hen at intel dot com>
 *	- Make sure only bond_attach_slave() and bond_detach_slave() can
 *	  manipulate the slave list, including slave_cnt, even when in
 *	  bond_release_all().
 *	- Fixed hang in bond_release() with traffic running:
 *	  netdev_set_master() must not be called from within the bond lock.
 *
 * 2003/03/18 - Tsippy Mendelson <tsippy.mendelson at intel dot com> and
 *		Shmulik Hen <shmulik.hen at intel dot com>
 *	- Fixed hang in bond_enslave() with traffic running:
 *	  netdev_set_master() must not be called from within the bond lock.
 *
 * 2003/03/18 - Amir Noam <amir.noam at intel dot com>
 *	- Added support for getting slave's speed and duplex via ethtool.
 *	  Needed for 802.3ad and other future modes.
 *
 * 2003/03/18 - Tsippy Mendelson <tsippy.mendelson at intel dot com> and
 *		Shmulik Hen <shmulik.hen at intel dot com>
 *	- Enable support of modes that need to use the unique mac address of
 *	  each slave.
 *	  * bond_enslave(): Moved setting the slave's mac address, and
 *	    openning it, from the application to the driver. This breaks
 *	    backward comaptibility with old versions of ifenslave that open
 *	     the slave before enalsving it !!!.
 *	  * bond_release(): The driver also takes care of closing the slave
 *	    and restoring its original mac address.
 *	- Removed the code that restores all base driver's flags.
 *	  Flags are automatically restored once all undo stages are done
 *	  properly.
 *	- Block possibility of enslaving before the master is up. This
 *	  prevents putting the system in an unstable state.
 *
 * 2003/03/18 - Amir Noam <amir.noam at intel dot com>,
 *		Tsippy Mendelson <tsippy.mendelson at intel dot com> and
 *		Shmulik Hen <shmulik.hen at intel dot com>
 *	- Added support for IEEE 802.3ad Dynamic link aggregation mode.
 *
 * 2003/05/01 - Amir Noam <amir.noam at intel dot com>
 *	- Added ABI version control to restore compatibility between
 *	  new/old ifenslave and new/old bonding.
 *
 * 2003/05/01 - Shmulik Hen <shmulik.hen at intel dot com>
 *	- Fixed bug in bond_release_all(): save old value of curr_active_slave
 *	  before setting it to NULL.
 *	- Changed driver versioning scheme to include version number instead
 *	  of release date (that is already in another field). There are 3
 *	  fields X.Y.Z where:
 *		X - Major version - big behavior changes
 *		Y - Minor version - addition of features
 *		Z - Extra version - minor changes and bug fixes
 *	  The current version is 1.0.0 as a base line.
 *
 * 2003/05/01 - Tsippy Mendelson <tsippy.mendelson at intel dot com> and
 *		Amir Noam <amir.noam at intel dot com>
 *	- Added support for lacp_rate module param.
 *	- Code beautification and style changes (mainly in comments).
 *	  new version - 1.0.1
 *
 * 2003/05/01 - Shmulik Hen <shmulik.hen at intel dot com>
 *	- Based on discussion on mailing list, changed locking scheme
 *	  to use lock/unlock or lock_bh/unlock_bh appropriately instead
 *	  of lock_irqsave/unlock_irqrestore. The new scheme helps exposing
 *	  hidden bugs and solves system hangs that occurred due to the fact
 *	  that holding lock_irqsave doesn't prevent softirqs from running.
 *	  This also increases total throughput since interrupts are not
 *	  blocked on each transmitted packets or monitor timeout.
 *	  new version - 2.0.0
 *
 * 2003/05/01 - Shmulik Hen <shmulik.hen at intel dot com>
 *	- Added support for Transmit load balancing mode.
 *	- Concentrate all assignments of curr_active_slave to a single point
 *	  so specific modes can take actions when the primary adapter is
 *	  changed.
 *	- Take the updelay parameter into consideration during bond_enslave
 *	  since some adapters loose their link during setting the device.
 *	- Renamed bond_3ad_link_status_changed() to
 *	  bond_3ad_handle_link_change() for compatibility with TLB.
 *	  new version - 2.1.0
 *
 * 2003/05/01 - Tsippy Mendelson <tsippy.mendelson at intel dot com>
 *	- Added support for Adaptive load balancing mode which is
 *	  equivalent to Transmit load balancing + Receive load balancing.
 *	  new version - 2.2.0
 *
 * 2003/05/15 - Jay Vosburgh <fubar at us dot ibm dot com>
 *	- Applied fix to activebackup_arp_monitor posted to bonding-devel
 *	  by Tony Cureington <tony.cureington * hp_com>.  Fixes ARP
 *	  monitor endless failover bug.  Version to 2.2.10
 *
 * 2003/05/20 - Amir Noam <amir.noam at intel dot com>
 *	- Fixed bug in ABI version control - Don't commit to a specific
 *	  ABI version if receiving unsupported ioctl commands.
 *
 * 2003/05/22 - Jay Vosburgh <fubar at us dot ibm dot com>
 *	- Fix ifenslave -c causing bond to loose existing routes;
 *	  added bond_set_mac_address() that doesn't require the
 *	  bond to be down.
 *	- In conjunction with fix for ifenslave -c, in
 *	  bond_change_active(), changing to the already active slave
 *	  is no longer an error (it successfully does nothing).
 *
 * 2003/06/30 - Amir Noam <amir.noam at intel dot com>
 * 	- Fixed bond_change_active() for ALB/TLB modes.
 *	  Version to 2.2.14.
 *
 * 2003/07/29 - Amir Noam <amir.noam at intel dot com>
 * 	- Fixed ARP monitoring bug.
 *	  Version to 2.2.15.
 *
 * 2003/07/31 - Willy Tarreau <willy at ods dot org>
 * 	- Fixed kernel panic when using ARP monitoring without
 *	  setting bond's IP address.
 *	  Version to 2.2.16.
 *
 * 2003/08/06 - Amir Noam <amir.noam at intel dot com>
 * 	- Back port from 2.6: use alloc_netdev(); fix /proc handling;
 *	  made stats a part of bond struct so no need to allocate
 *	  and free it separately; use standard list operations instead
 *	  of pre-allocated array of bonds.
 *	  Version to 2.3.0.
 *
 * 2003/08/07 - Jay Vosburgh <fubar at us dot ibm dot com>,
 *	       Amir Noam <amir.noam at intel dot com> and
 *	       Shmulik Hen <shmulik.hen at intel dot com>
 *	- Propagating master's settings: Distinguish between modes that
 *	  use a primary slave from those that don't, and propagate settings
 *	  accordingly; Consolidate change_active opeartions and add
 *	  reselect_active and find_best opeartions; Decouple promiscuous
 *	  handling from the multicast mode setting; Add support for changing
 *	  HW address and MTU with proper unwind; Consolidate procfs code,
 *	  add CHANGENAME handler; Enhance netdev notification handling.
 *	  Version to 2.4.0.
 *
 * 2003/09/15 - Stephen Hemminger <shemminger at osdl dot org>,
 *	       Amir Noam <amir.noam at intel dot com>
 *	- Convert /proc to seq_file interface.
 *	  Change /proc/net/bondX/info to /proc/net/bonding/bondX.
 *	  Set version to 2.4.1.
 *
 * 2003/11/20 - Amir Noam <amir.noam at intel dot com>
 *	- Fix /proc creation/destruction.
 *
 * 2003/12/01 - Shmulik Hen <shmulik.hen at intel dot com>
 *	- Massive cleanup - Set version to 2.5.0
 *	  Code changes:
 *	  o Consolidate format of prints and debug prints.
 *	  o Remove bonding_t/slave_t typedefs and consolidate all casts.
 *	  o Remove dead code and unnecessary checks.
 *	  o Consolidate starting/stopping timers.
 *	  o Consolidate handling of primary module param throughout the code.
 *	  o Removed multicast module param support - all settings are done
 *	    according to mode.
 *	  o Slave list iteration - bond is no longer part of the list,
 *	    added cyclic list iteration macros.
 *	  o Consolidate error handling in all xmit functions.
 *	  Style changes:
 *	  o Consolidate function naming and declarations.
 *	  o Consolidate function params and local variables names.
 *	  o Consolidate return values.
 *	  o Consolidate curly braces.
 *	  o Consolidate conditionals format.
 *	  o Change struct member names and types.
 *	  o Chomp trailing spaces, remove empty lines, fix indentations.
 *	  o Re-organize code according to context.
 *
 * 2003/12/30 - Amir Noam <amir.noam at intel dot com>
 *	- Fixed: Cannot remove and re-enslave the original active slave.
 *	- Fixed: Releasing the original active slave causes mac address
 *		 duplication.
 *	- Add support for slaves that use ethtool_ops.
 *	  Set version to 2.5.3.
 *
 * 2004/01/05 - Amir Noam <amir.noam at intel dot com>
 *	- Save bonding parameters per bond instead of using the global values.
 *	  Set version to 2.5.4.
 *
 * 2004/01/14 - Shmulik Hen <shmulik.hen at intel dot com>
 *	- Enhance VLAN support:
 *	  * Add support for VLAN hardware acceleration capable slaves.
 *	  * Add capability to tag self generated packets in ALB/TLB modes.
 *	  Set version to 2.6.0.
 */

//#define BONDING_DEBUG 1

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/socket.h>
#include <linux/ctype.h>
#include <linux/inet.h>
#include <asm/system.h>
#include <asm/bitops.h>
#include <asm/io.h>
#include <asm/dma.h>
#include <asm/uaccess.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <linux/rtnetlink.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/smp.h>
#include <linux/if_ether.h>
#include <net/arp.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/if_vlan.h>
#include <linux/if_bonding.h>
#include "bonding.h"
#include "bond_3ad.h"
#include "bond_alb.h"

/*---------------------------- Module parameters ----------------------------*/

/* monitor all links that often (in milliseconds). <=0 disables monitoring */
#define BOND_LINK_MON_INTERV	0
#define BOND_LINK_ARP_INTERV	0

static int max_bonds	= BOND_DEFAULT_MAX_BONDS;
static int miimon	= BOND_LINK_MON_INTERV;
static int updelay	= 0;
static int downdelay	= 0;
static int use_carrier	= 1;
static char *mode	= NULL;
static char *primary	= NULL;
static char *lacp_rate	= NULL;
static int arp_interval = BOND_LINK_ARP_INTERV;
static char *arp_ip_target[BOND_MAX_ARP_TARGETS] = { NULL, };

MODULE_PARM(max_bonds, "i");
MODULE_PARM_DESC(max_bonds, "Max number of bonded devices");
MODULE_PARM(miimon, "i");
MODULE_PARM_DESC(miimon, "Link check interval in milliseconds");
MODULE_PARM(updelay, "i");
MODULE_PARM_DESC(updelay, "Delay before considering link up, in milliseconds");
MODULE_PARM(downdelay, "i");
MODULE_PARM_DESC(downdelay, "Delay before considering link down, in milliseconds");
MODULE_PARM(use_carrier, "i");
MODULE_PARM_DESC(use_carrier, "Use netif_carrier_ok (vs MII ioctls) in miimon; 0 for off, 1 for on (default)");
MODULE_PARM(mode, "s");
MODULE_PARM_DESC(mode, "Mode of operation : 0 for round robin, 1 for active-backup, 2 for xor");
MODULE_PARM(primary, "s");
MODULE_PARM_DESC(primary, "Primary network device to use");
MODULE_PARM(lacp_rate, "s");
MODULE_PARM_DESC(lacp_rate, "LACPDU tx rate to request from 802.3ad partner (slow/fast)");
MODULE_PARM(arp_interval, "i");
MODULE_PARM_DESC(arp_interval, "arp interval in milliseconds");
MODULE_PARM(arp_ip_target, "1-" __MODULE_STRING(BOND_MAX_ARP_TARGETS) "s");
MODULE_PARM_DESC(arp_ip_target, "arp targets in n.n.n.n form");

/*----------------------------- Global variables ----------------------------*/

static const char *version =
	DRV_DESCRIPTION ": v" DRV_VERSION " (" DRV_RELDATE ")\n";

static LIST_HEAD(bond_dev_list);

#ifdef CONFIG_PROC_FS
static struct proc_dir_entry *bond_proc_dir = NULL;
#endif

static u32 arp_target[BOND_MAX_ARP_TARGETS] = { 0, } ;
static int arp_ip_count	= 0;
static u32 my_ip	= 0;
static int bond_mode	= BOND_MODE_ROUNDROBIN;
static int lacp_fast	= 0;
static int app_abi_ver	= 0;
static int orig_app_abi_ver = -1; /* This is used to save the first ABI version
				   * we receive from the application. Once set,
				   * it won't be changed, and the module will
				   * refuse to enslave/release interfaces if the
				   * command comes from an application using
				   * another ABI version.
				   */

struct bond_parm_tbl {
	char *modename;
	int mode;
};

static struct bond_parm_tbl bond_lacp_tbl[] = {
{	"slow",		AD_LACP_SLOW},
{	"fast",		AD_LACP_FAST},
{	NULL,		-1},
};

static struct bond_parm_tbl bond_mode_tbl[] = {
{	"balance-rr",		BOND_MODE_ROUNDROBIN},
{	"active-backup",	BOND_MODE_ACTIVEBACKUP},
{	"balance-xor",		BOND_MODE_XOR},
{	"broadcast",		BOND_MODE_BROADCAST},
{	"802.3ad",		BOND_MODE_8023AD},
{	"balance-tlb",		BOND_MODE_TLB},
{	"balance-alb",		BOND_MODE_ALB},
{	NULL,			-1},
};

/*-------------------------- Forward declarations ---------------------------*/

static inline void bond_set_mode_ops(struct net_device *bond_dev, int mode);

/*---------------------------- General routines -----------------------------*/

static const char *bond_mode_name(int mode)
{
	switch (mode) {
	case BOND_MODE_ROUNDROBIN :
		return "load balancing (round-robin)";
	case BOND_MODE_ACTIVEBACKUP :
		return "fault-tolerance (active-backup)";
	case BOND_MODE_XOR :
		return "load balancing (xor)";
	case BOND_MODE_BROADCAST :
		return "fault-tolerance (broadcast)";
	case BOND_MODE_8023AD:
		return "IEEE 802.3ad Dynamic link aggregation";
	case BOND_MODE_TLB:
		return "transmit load balancing";
	case BOND_MODE_ALB:
		return "adaptive load balancing";
	default:
		return "unknown";
	}
}

/*---------------------------------- VLAN -----------------------------------*/

/**
 * bond_add_vlan - add a new vlan id on bond
 * @bond: bond that got the notification
 * @vlan_id: the vlan id to add
 *
 * Returns -ENOMEM if allocation failed.
 */
static int bond_add_vlan(struct bonding *bond, unsigned short vlan_id)
{
	struct vlan_entry *vlan;

	dprintk("bond: %s, vlan id %d\n",
		(bond ? bond->dev->name: "None"), vlan_id);

	vlan = kmalloc(sizeof(struct vlan_entry), GFP_KERNEL);
	if (!vlan) {
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&vlan->vlan_list);
	vlan->vlan_id = vlan_id;

	write_lock_bh(&bond->lock);

	list_add_tail(&vlan->vlan_list, &bond->vlan_list);

	write_unlock_bh(&bond->lock);

	dprintk("added VLAN ID %d on bond %s\n", vlan_id, bond->dev->name);

	return 0;
}

/**
 * bond_del_vlan - delete a vlan id from bond
 * @bond: bond that got the notification
 * @vlan_id: the vlan id to delete
 *
 * returns -ENODEV if @vlan_id was not found in @bond.
 */
static int bond_del_vlan(struct bonding *bond, unsigned short vlan_id)
{
	struct vlan_entry *vlan, *next;
	int res = -ENODEV;

	dprintk("bond: %s, vlan id %d\n", bond->dev->name, vlan_id);

	write_lock_bh(&bond->lock);

	list_for_each_entry_safe(vlan, next, &bond->vlan_list, vlan_list) {
		if (vlan->vlan_id == vlan_id) {
			list_del(&vlan->vlan_list);

			if ((bond->params.mode == BOND_MODE_TLB) ||
			    (bond->params.mode == BOND_MODE_ALB)) {
				bond_alb_clear_vlan(bond, vlan_id);
			}

			dprintk("removed VLAN ID %d from bond %s\n", vlan_id,
				bond->dev->name);

			kfree(vlan);

			if (list_empty(&bond->vlan_list) &&
			    (bond->slave_cnt == 0)) {
				/* Last VLAN removed and no slaves, so
				 * restore block on adding VLANs. This will
				 * be removed once new slaves that are not
				 * VLAN challenged will be added.
				 */
				bond->dev->features |= NETIF_F_VLAN_CHALLENGED;
			}

			res = 0;
			goto out;
		}
	}

	dprintk("couldn't find VLAN ID %d in bond %s\n", vlan_id,
		bond->dev->name);

out:
	write_unlock_bh(&bond->lock);
	return res;
}

/**
 * bond_has_challenged_slaves
 * @bond: the bond we're working on
 *
 * Searches the slave list. Returns 1 if a vlan challenged slave
 * was found, 0 otherwise.
 *
 * Assumes bond->lock is held.
 */
static int bond_has_challenged_slaves(struct bonding *bond)
{
	struct slave *slave;
	int i;

	bond_for_each_slave(bond, slave, i) {
		if (slave->dev->features & NETIF_F_VLAN_CHALLENGED) {
			dprintk("found VLAN challenged slave - %s\n",
				slave->dev->name);
			return 1;
		}
	}

	dprintk("no VLAN challenged slaves found\n");
	return 0;
}

/**
 * bond_next_vlan - safely skip to the next item in the vlans list.
 * @bond: the bond we're working on
 * @curr: item we're advancing from
 *
 * Returns %NULL if list is empty, bond->next_vlan if @curr is %NULL,
 * or @curr->next otherwise (even if it is @curr itself again).
 * 
 * Caller must hold bond->lock
 */
struct vlan_entry *bond_next_vlan(struct bonding *bond, struct vlan_entry *curr)
{
	struct vlan_entry *next, *last;

	if (list_empty(&bond->vlan_list)) {
		return NULL;
	}

	if (!curr) {
		next = list_entry(bond->vlan_list.next,
				  struct vlan_entry, vlan_list);
	} else {
		last = list_entry(bond->vlan_list.prev,
				  struct vlan_entry, vlan_list);
		if (last == curr) {
			next = list_entry(bond->vlan_list.next,
					  struct vlan_entry, vlan_list);
		} else {
			next = list_entry(curr->vlan_list.next,
					  struct vlan_entry, vlan_list);
		}
	}

	return next;
}

/**
 * bond_dev_queue_xmit - Prepare skb for xmit.
 * 
 * @bond: bond device that got this skb for tx.
 * @skb: hw accel VLAN tagged skb to transmit
 * @slave_dev: slave that is supposed to xmit this skbuff
 * 
 * When the bond gets an skb to tarnsmit that is
 * already hardware accelerated VLAN tagged, and it
 * needs to relay this skb to a slave that is not
 * hw accel capable, the skb needs to be "unaccelerated",
 * i.e. strip the hwaccel tag and re-insert it as part
 * of the payload.
 * 
 * Assumption - once a VLAN device is created over the bond device, all
 * packets are going to be hardware accelerated VLAN tagged since the IP
 * binding is done over the VLAN device
 */
int bond_dev_queue_xmit(struct bonding *bond, struct sk_buff *skb, struct net_device *slave_dev)
{
	unsigned short vlan_id;
	int res;

	if (!list_empty(&bond->vlan_list) &&
	    !(slave_dev->features & NETIF_F_HW_VLAN_TX)) {
		res = vlan_get_tag(skb, &vlan_id);
		if (res) {
			return -EINVAL;
		}

		skb->dev = slave_dev;
		skb = vlan_put_tag(skb, vlan_id);
		if (!skb) {
			/* vlan_put_tag() frees the skb in case of error,
			 * so return success here so the calling functions
			 * won't attempt to free is again.
			 */
			return 0;
		}
	} else {
		skb->dev = slave_dev;
	}

	skb->priority = 1;
	dev_queue_xmit(skb);

	return 0;
}

/*
 * In the following 3 functions, bond_vlan_rx_register(), bond_vlan_rx_add_vid
 * and bond_vlan_rx_kill_vid, We don't protect the slave list iteration with a
 * lock because:
 * a. This operation is performed in IOCTL context,
 * b. The operation is protected by the RTNL semaphore in the 8021q code,
 * c. Holding a lock with BH disabled while directly calling a base driver
 *    entry point is generally a BAD idea.
 * 
 * The design of synchronization/protection for this operation in the 8021q
 * module is good for one or more VLAN devices over a single physical device
 * and cannot be extended for a teaming solution like bonding, so there is a
 * potential race condition here where a net device from the vlan group might
 * be referenced (either by a base driver or the 8021q code) while it is being
 * removed from the system. However, it turns out we're not making matters
 * worse, and if it works for regular VLAN usage it will work here too.
*/

/**
 * bond_vlan_rx_register - Propagates registration to slaves
 * @bond_dev: bonding net device that got called
 * @grp: vlan group being registered
 */
static void bond_vlan_rx_register(struct net_device *bond_dev, struct vlan_group *grp)
{
	struct bonding *bond = bond_dev->priv;
	struct slave *slave;
	int i;

	bond->vlgrp = grp;

	bond_for_each_slave(bond, slave, i) {
		struct net_device *slave_dev = slave->dev;

		if ((slave_dev->features & NETIF_F_HW_VLAN_RX) &&
		    slave_dev->vlan_rx_register) {
			slave_dev->vlan_rx_register(slave_dev, grp);
		}
	}
}

/**
 * bond_vlan_rx_add_vid - Propagates adding an id to slaves
 * @bond_dev: bonding net device that got called
 * @vid: vlan id being added
 */
static void bond_vlan_rx_add_vid(struct net_device *bond_dev, uint16_t vid)
{
	struct bonding *bond = bond_dev->priv;
	struct slave *slave;
	int i, res;

	bond_for_each_slave(bond, slave, i) {
		struct net_device *slave_dev = slave->dev;

		if ((slave_dev->features & NETIF_F_HW_VLAN_FILTER) &&
		    slave_dev->vlan_rx_add_vid) {
			slave_dev->vlan_rx_add_vid(slave_dev, vid);
		}
	}

	res = bond_add_vlan(bond, vid);
	if (res) {
		printk(KERN_ERR DRV_NAME
		       ": %s: Failed to add vlan id %d\n",
		       bond_dev->name, vid);
	}
}

/**
 * bond_vlan_rx_kill_vid - Propagates deleting an id to slaves
 * @bond_dev: bonding net device that got called
 * @vid: vlan id being removed
 */
static void bond_vlan_rx_kill_vid(struct net_device *bond_dev, uint16_t vid)
{
	struct bonding *bond = bond_dev->priv;
	struct slave *slave;
	struct net_device *vlan_dev;
	int i, res;

	bond_for_each_slave(bond, slave, i) {
		struct net_device *slave_dev = slave->dev;

		if ((slave_dev->features & NETIF_F_HW_VLAN_FILTER) &&
		    slave_dev->vlan_rx_kill_vid) {
			/* Save and then restore vlan_dev in the grp array,
			 * since the slave's driver might clear it.
			 */
			vlan_dev = bond->vlgrp->vlan_devices[vid];
			slave_dev->vlan_rx_kill_vid(slave_dev, vid);
			bond->vlgrp->vlan_devices[vid] = vlan_dev;
		}
	}

	res = bond_del_vlan(bond, vid);
	if (res) {
		printk(KERN_ERR DRV_NAME
		       ": %s: Failed to remove vlan id %d\n",
		       bond_dev->name, vid);
	}
}

static void bond_add_vlans_on_slave(struct bonding *bond, struct net_device *slave_dev)
{
	struct vlan_entry *vlan;

	write_lock_bh(&bond->lock);

	if (list_empty(&bond->vlan_list)) {
		goto out;
	}

	if ((slave_dev->features & NETIF_F_HW_VLAN_RX) &&
	    slave_dev->vlan_rx_register) {
		slave_dev->vlan_rx_register(slave_dev, bond->vlgrp);
	}

	if (!(slave_dev->features & NETIF_F_HW_VLAN_FILTER) ||
	    !(slave_dev->vlan_rx_add_vid)) {
		goto out;
	}

	list_for_each_entry(vlan, &bond->vlan_list, vlan_list) {
		slave_dev->vlan_rx_add_vid(slave_dev, vlan->vlan_id);
	}

out:
	write_unlock_bh(&bond->lock);
}

static void bond_del_vlans_from_slave(struct bonding *bond, struct net_device *slave_dev)
{
	struct vlan_entry *vlan;
	struct net_device *vlan_dev;

	write_lock_bh(&bond->lock);

	if (list_empty(&bond->vlan_list)) {
		goto out;
	}

	if (!(slave_dev->features & NETIF_F_HW_VLAN_FILTER) ||
	    !(slave_dev->vlan_rx_kill_vid)) {
		goto unreg;
	}

	list_for_each_entry(vlan, &bond->vlan_list, vlan_list) {
		/* Save and then restore vlan_dev in the grp array,
		 * since the slave's driver might clear it.
		 */
		vlan_dev = bond->vlgrp->vlan_devices[vlan->vlan_id];
		slave_dev->vlan_rx_kill_vid(slave_dev, vlan->vlan_id);
		bond->vlgrp->vlan_devices[vlan->vlan_id] = vlan_dev;
	}

unreg:
	if ((slave_dev->features & NETIF_F_HW_VLAN_RX) &&
	    slave_dev->vlan_rx_register) {
		slave_dev->vlan_rx_register(slave_dev, NULL);
	}

out:
	write_unlock_bh(&bond->lock);
}

/*------------------------------- Link status -------------------------------*/

/*
 * Get link speed and duplex from the slave's base driver
 * using ethtool. If for some reason the call fails or the
 * values are invalid, fake speed and duplex to 100/Full
 * and return error.
 */
static int bond_update_speed_duplex(struct slave *slave)
{
	struct net_device *slave_dev = slave->dev;
	static int (* ioctl)(struct net_device *, struct ifreq *, int);
	struct ifreq ifr;
	struct ethtool_cmd etool;

	/* Fake speed and duplex */
	slave->speed = SPEED_100;
	slave->duplex = DUPLEX_FULL;

	if (slave_dev->ethtool_ops) {
		u32 res;

		if (!slave_dev->ethtool_ops->get_settings) {
			return -1;
		}

		res = slave_dev->ethtool_ops->get_settings(slave_dev, &etool);
		if (res < 0) {
			return -1;
		}

		goto verify;
	}

	ioctl = slave_dev->do_ioctl;
	strncpy(ifr.ifr_name, slave_dev->name, IFNAMSIZ);
	etool.cmd = ETHTOOL_GSET;
	ifr.ifr_data = (char*)&etool;
	if (!ioctl || (IOCTL(slave_dev, &ifr, SIOCETHTOOL) < 0)) {
		return -1;
	}

verify:
	switch (etool.speed) {
	case SPEED_10:
	case SPEED_100:
	case SPEED_1000:
		break;
	default:
		return -1;
	}

	switch (etool.duplex) {
	case DUPLEX_FULL:
	case DUPLEX_HALF:
		break;
	default:
		return -1;
	}

	slave->speed = etool.speed;
	slave->duplex = etool.duplex;

	return 0;
}

/*
 * if <dev> supports MII link status reporting, check its link status.
 *
 * We either do MII/ETHTOOL ioctls, or check netif_carrier_ok(),
 * depening upon the setting of the use_carrier parameter.
 *
 * Return either BMSR_LSTATUS, meaning that the link is up (or we
 * can't tell and just pretend it is), or 0, meaning that the link is
 * down.
 *
 * If reporting is non-zero, instead of faking link up, return -1 if
 * both ETHTOOL and MII ioctls fail (meaning the device does not
 * support them).  If use_carrier is set, return whatever it says.
 * It'd be nice if there was a good way to tell if a driver supports
 * netif_carrier, but there really isn't.
 */
static int bond_check_dev_link(struct bonding *bond, struct net_device *slave_dev, int reporting)
{
	static int (* ioctl)(struct net_device *, struct ifreq *, int);
	struct ifreq ifr;
	struct mii_ioctl_data *mii;
	struct ethtool_value etool;

	if (bond->params.use_carrier) {
		return netif_carrier_ok(slave_dev) ? BMSR_LSTATUS : 0;
	}

	ioctl = slave_dev->do_ioctl;
	if (ioctl) {
		/* TODO: set pointer to correct ioctl on a per team member */
		/*       bases to make this more efficient. that is, once  */
		/*       we determine the correct ioctl, we will always    */
		/*       call it and not the others for that team          */
		/*       member.                                           */

		/*
		 * We cannot assume that SIOCGMIIPHY will also read a
		 * register; not all network drivers (e.g., e100)
		 * support that.
		 */

		/* Yes, the mii is overlaid on the ifreq.ifr_ifru */
		strncpy(ifr.ifr_name, slave_dev->name, IFNAMSIZ);
		mii = if_mii(&ifr);
		if (IOCTL(slave_dev, &ifr, SIOCGMIIPHY) == 0) {
			mii->reg_num = MII_BMSR;
			if (IOCTL(slave_dev, &ifr, SIOCGMIIREG) == 0) {
				return (mii->val_out & BMSR_LSTATUS);
			}
		}
	}

	/* try SIOCETHTOOL ioctl, some drivers cache ETHTOOL_GLINK */
	/* for a period of time so we attempt to get link status   */
	/* from it last if the above MII ioctls fail...            */
	if (slave_dev->ethtool_ops) {
		if (slave_dev->ethtool_ops->get_link) {
			u32 link;

			link = slave_dev->ethtool_ops->get_link(slave_dev);

			return link ? BMSR_LSTATUS : 0;
		}
	}

	if (ioctl) {
		strncpy(ifr.ifr_name, slave_dev->name, IFNAMSIZ);
		etool.cmd = ETHTOOL_GLINK;
		ifr.ifr_data = (char*)&etool;
		if (IOCTL(slave_dev, &ifr, SIOCETHTOOL) == 0) {
			if (etool.data == 1) {
				return BMSR_LSTATUS;
			} else {
				dprintk("SIOCETHTOOL shows link down\n");
				return 0;
			}
		}
	}

	/*
	 * If reporting, report that either there's no dev->do_ioctl,
	 * or both SIOCGMIIREG and SIOCETHTOOL failed (meaning that we
	 * cannot report link status).  If not reporting, pretend
	 * we're ok.
	 */
	return (reporting ? -1 : BMSR_LSTATUS);
}

/*----------------------------- Multicast list ------------------------------*/

/*
 * Returns 0 if dmi1 and dmi2 are the same, non-0 otherwise
 */
static inline int bond_is_dmi_same(struct dev_mc_list *dmi1, struct dev_mc_list *dmi2)
{
	return memcmp(dmi1->dmi_addr, dmi2->dmi_addr, dmi1->dmi_addrlen) == 0 &&
			dmi1->dmi_addrlen == dmi2->dmi_addrlen;
}

/*
 * returns dmi entry if found, NULL otherwise
 */
static struct dev_mc_list *bond_mc_list_find_dmi(struct dev_mc_list *dmi, struct dev_mc_list *mc_list)
{
	struct dev_mc_list *idmi;

	for (idmi = mc_list; idmi; idmi = idmi->next) {
		if (bond_is_dmi_same(dmi, idmi)) {
			return idmi;
		}
	}

	return NULL;
}

/*
 * Push the promiscuity flag down to appropriate slaves
 */
static void bond_set_promiscuity(struct bonding *bond, int inc)
{
	if (USES_PRIMARY(bond->params.mode)) {
		/* write lock already acquired */
		if (bond->curr_active_slave) {
			dev_set_promiscuity(bond->curr_active_slave->dev, inc);
		}
	} else {
		struct slave *slave;
		int i;
		bond_for_each_slave(bond, slave, i) {
			dev_set_promiscuity(slave->dev, inc);
		}
	}
}

/*
 * Push the allmulti flag down to all slaves
 */
static void bond_set_allmulti(struct bonding *bond, int inc)
{
	if (USES_PRIMARY(bond->params.mode)) {
		/* write lock already acquired */
		if (bond->curr_active_slave) {
			dev_set_allmulti(bond->curr_active_slave->dev, inc);
		}
	} else {
		struct slave *slave;
		int i;
		bond_for_each_slave(bond, slave, i) {
			dev_set_allmulti(slave->dev, inc);
		}
	}
}

/*
 * Add a Multicast address to slaves
 * according to mode
 */
static void bond_mc_add(struct bonding *bond, void *addr, int alen)
{
	if (USES_PRIMARY(bond->params.mode)) {
		/* write lock already acquired */
		if (bond->curr_active_slave) {
			dev_mc_add(bond->curr_active_slave->dev, addr, alen, 0);
		}
	} else {
		struct slave *slave;
		int i;
		bond_for_each_slave(bond, slave, i) {
			dev_mc_add(slave->dev, addr, alen, 0);
		}
	}
}

/*
 * Remove a multicast address from slave
 * according to mode
 */
static void bond_mc_delete(struct bonding *bond, void *addr, int alen)
{
	if (USES_PRIMARY(bond->params.mode)) {
		/* write lock already acquired */
		if (bond->curr_active_slave) {
			dev_mc_delete(bond->curr_active_slave->dev, addr, alen, 0);
		}
	} else {
		struct slave *slave;
		int i;
		bond_for_each_slave(bond, slave, i) {
			dev_mc_delete(slave->dev, addr, alen, 0);
		}
	}
}

/*
 * Totally destroys the mc_list in bond
 */
static void bond_mc_list_destroy(struct bonding *bond)
{
	struct dev_mc_list *dmi;

	dmi = bond->mc_list;
	while (dmi) {
		bond->mc_list = dmi->next;
		kfree(dmi);
		dmi = bond->mc_list;
	}
}

/*
 * Copy all the Multicast addresses from src to the bonding device dst
 */
static int bond_mc_list_copy(struct dev_mc_list *mc_list, struct bonding *bond, int gpf_flag)
{
	struct dev_mc_list *dmi, *new_dmi;

	for (dmi = mc_list; dmi; dmi = dmi->next) {
		new_dmi = kmalloc(sizeof(struct dev_mc_list), gpf_flag);

		if (!new_dmi) {
			/* FIXME: Potential memory leak !!! */
			return -ENOMEM;
		}

		new_dmi->next = bond->mc_list;
		bond->mc_list = new_dmi;
		new_dmi->dmi_addrlen = dmi->dmi_addrlen;
		memcpy(new_dmi->dmi_addr, dmi->dmi_addr, dmi->dmi_addrlen);
		new_dmi->dmi_users = dmi->dmi_users;
		new_dmi->dmi_gusers = dmi->dmi_gusers;
	}

	return 0;
}

/*
 * flush all members of flush->mc_list from device dev->mc_list
 */
static void bond_mc_list_flush(struct net_device *bond_dev, struct net_device *slave_dev)
{
	struct bonding *bond = bond_dev->priv;
	struct dev_mc_list *dmi;

	for (dmi = bond_dev->mc_list; dmi; dmi = dmi->next) {
		dev_mc_delete(slave_dev, dmi->dmi_addr, dmi->dmi_addrlen, 0);
	}

	if (bond->params.mode == BOND_MODE_8023AD) {
		/* del lacpdu mc addr from mc list */
		u8 lacpdu_multicast[ETH_ALEN] = MULTICAST_LACPDU_ADDR;

		dev_mc_delete(slave_dev, lacpdu_multicast, ETH_ALEN, 0);
	}
}

/*--------------------------- Active slave change ---------------------------*/

/*
 * Update the mc list and multicast-related flags for the new and
 * old active slaves (if any) according to the multicast mode, and
 * promiscuous flags unconditionally.
 */
static void bond_mc_swap(struct bonding *bond, struct slave *new_active, struct slave *old_active)
{
	struct dev_mc_list *dmi;

	if (!USES_PRIMARY(bond->params.mode)) {
		/* nothing to do -  mc list is already up-to-date on
		 * all slaves
		 */
		return;
	}

	if (old_active) {
		if (bond->dev->flags & IFF_PROMISC) {
			dev_set_promiscuity(old_active->dev, -1);
		}

		if (bond->dev->flags & IFF_ALLMULTI) {
			dev_set_allmulti(old_active->dev, -1);
		}

		for (dmi = bond->dev->mc_list; dmi; dmi = dmi->next) {
			dev_mc_delete(old_active->dev, dmi->dmi_addr, dmi->dmi_addrlen, 0);
		}
	}

	if (new_active) {
		if (bond->dev->flags & IFF_PROMISC) {
			dev_set_promiscuity(new_active->dev, 1);
		}

		if (bond->dev->flags & IFF_ALLMULTI) {
			dev_set_allmulti(new_active->dev, 1);
		}

		for (dmi = bond->dev->mc_list; dmi; dmi = dmi->next) {
			dev_mc_add(new_active->dev, dmi->dmi_addr, dmi->dmi_addrlen, 0);
		}
	}
}

/**
 * find_best_interface - select the best available slave to be the active one
 * @bond: our bonding struct
 *
 * Warning: Caller must hold curr_slave_lock for writing.
 */
static struct slave *bond_find_best_slave(struct bonding *bond)
{
	struct slave *new_active, *old_active;
	struct slave *bestslave = NULL;
	int mintime = bond->params.updelay;
	int i;

	new_active = old_active = bond->curr_active_slave;

	if (!new_active) { /* there were no active slaves left */
		if (bond->slave_cnt > 0) {  /* found one slave */
			new_active = bond->first_slave;
		} else {
			return NULL; /* still no slave, return NULL */
		}
	}

	/* first try the primary link; if arping, a link must tx/rx traffic
	 * before it can be considered the curr_active_slave - also, we would skip
	 * slaves between the curr_active_slave and primary_slave that may be up
	 * and able to arp
	 */
	if ((bond->primary_slave) &&
	    (!bond->params.arp_interval) &&
	    (IS_UP(bond->primary_slave->dev))) {
		new_active = bond->primary_slave;
	}

	/* remember where to stop iterating over the slaves */
	old_active = new_active;

	bond_for_each_slave_from(bond, new_active, i, old_active) {
		if (IS_UP(new_active->dev)) {
			if (new_active->link == BOND_LINK_UP) {
				return new_active;
			} else if (new_active->link == BOND_LINK_BACK) {
				/* link up, but waiting for stabilization */
				if (new_active->delay < mintime) {
					mintime = new_active->delay;
					bestslave = new_active;
				}
			}
		}
	}

	return bestslave;
}

/**
 * change_active_interface - change the active slave into the specified one
 * @bond: our bonding struct
 * @new: the new slave to make the active one
 *
 * Set the new slave to the bond's settings and unset them on the old
 * curr_active_slave.
 * Setting include flags, mc-list, promiscuity, allmulti, etc.
 *
 * If @new's link state is %BOND_LINK_BACK we'll set it to %BOND_LINK_UP,
 * because it is apparently the best available slave we have, even though its
 * updelay hasn't timed out yet.
 *
 * Warning: Caller must hold curr_slave_lock for writing.
 */
static void bond_change_active_slave(struct bonding *bond, struct slave *new_active)
{
	struct slave *old_active = bond->curr_active_slave;

	if (old_active == new_active) {
		return;
	}

	if (new_active) {
		if (new_active->link == BOND_LINK_BACK) {
			if (USES_PRIMARY(bond->params.mode)) {
				printk(KERN_INFO DRV_NAME
				       ": %s: making interface %s the new "
				       "active one %d ms earlier.\n",
				       bond->dev->name, new_active->dev->name,
				       (bond->params.updelay - new_active->delay) * bond->params.miimon);
			}

			new_active->delay = 0;
			new_active->link = BOND_LINK_UP;
			new_active->jiffies = jiffies;

			if (bond->params.mode == BOND_MODE_8023AD) {
				bond_3ad_handle_link_change(new_active, BOND_LINK_UP);
			}

			if ((bond->params.mode == BOND_MODE_TLB) ||
			    (bond->params.mode == BOND_MODE_ALB)) {
				bond_alb_handle_link_change(bond, new_active, BOND_LINK_UP);
			}
		} else {
			if (USES_PRIMARY(bond->params.mode)) {
				printk(KERN_INFO DRV_NAME
				       ": %s: making interface %s the new "
				       "active one.\n",
				       bond->dev->name, new_active->dev->name);
			}
		}
	}

	if (bond->params.mode == BOND_MODE_ACTIVEBACKUP) {
		if (old_active) {
			bond_set_slave_inactive_flags(old_active);
		}

		if (new_active) {
			bond_set_slave_active_flags(new_active);
		}
	}

	if (USES_PRIMARY(bond->params.mode)) {
		bond_mc_swap(bond, new_active, old_active);
	}

	if ((bond->params.mode == BOND_MODE_TLB) ||
	    (bond->params.mode == BOND_MODE_ALB)) {
		bond_alb_handle_active_change(bond, new_active);
	} else {
		bond->curr_active_slave = new_active;
	}
}

/**
 * bond_select_active_slave - select a new active slave, if needed
 * @bond: our bonding struct
 *
 * This functions shoud be called when one of the following occurs:
 * - The old curr_active_slave has been released or lost its link.
 * - The primary_slave has got its link back.
 * - A slave has got its link back and there's no old curr_active_slave.
 *
 * Warning: Caller must hold curr_slave_lock for writing.
 */
static void bond_select_active_slave(struct bonding *bond)
{
	struct slave *best_slave;

	best_slave = bond_find_best_slave(bond);
	if (best_slave != bond->curr_active_slave) {
		bond_change_active_slave(bond, best_slave);
	}
}

/*--------------------------- slave list handling ---------------------------*/

/*
 * This function attaches the slave to the end of list.
 *
 * bond->lock held for writing by caller.
 */
static void bond_attach_slave(struct bonding *bond, struct slave *new_slave)
{
	if (bond->first_slave == NULL) { /* attaching the first slave */
		new_slave->next = new_slave;
		new_slave->prev = new_slave;
		bond->first_slave = new_slave;
	} else {
		new_slave->next = bond->first_slave;
		new_slave->prev = bond->first_slave->prev;
		new_slave->next->prev = new_slave;
		new_slave->prev->next = new_slave;
	}

	bond->slave_cnt++;
}

/*
 * This function detaches the slave from the list.
 * WARNING: no check is made to verify if the slave effectively
 * belongs to <bond>.
 * Nothing is freed on return, structures are just unchained.
 * If any slave pointer in bond was pointing to <slave>,
 * it should be changed by the calling function.
 *
 * bond->lock held for writing by caller.
 */
static void bond_detach_slave(struct bonding *bond, struct slave *slave)
{
	if (slave->next) {
		slave->next->prev = slave->prev;
	}

	if (slave->prev) {
		slave->prev->next = slave->next;
	}

	if (bond->first_slave == slave) { /* slave is the first slave */
		if (bond->slave_cnt > 1) { /* there are more slave */
			bond->first_slave = slave->next;
		} else {
			bond->first_slave = NULL; /* slave was the last one */
		}
	}

	slave->next = NULL;
	slave->prev = NULL;
	bond->slave_cnt--;
}

/*---------------------------------- IOCTL ----------------------------------*/

static int bond_sethwaddr(struct net_device *bond_dev, struct net_device *slave_dev)
{
	dprintk("bond_dev=%p\n", bond_dev);
	dprintk("slave_dev=%p\n", slave_dev);
	dprintk("slave_dev->addr_len=%d\n", slave_dev->addr_len);
	memcpy(bond_dev->dev_addr, slave_dev->dev_addr, slave_dev->addr_len);
	return 0;
}

/* enslave device <slave> to bond device <master> */
static int bond_enslave(struct net_device *bond_dev, struct net_device *slave_dev)
{
	struct bonding *bond = bond_dev->priv;
	struct slave *new_slave = NULL;
	struct dev_mc_list *dmi;
	struct sockaddr addr;
	int link_reporting;
	int old_features = bond_dev->features;
	int res = 0;

	if (slave_dev->do_ioctl == NULL) {
		printk(KERN_WARNING DRV_NAME
		       ": Warning : no link monitoring support for %s\n",
		       slave_dev->name);
	}

	/* bond must be initialized by bond_open() before enslaving */
	if (!(bond_dev->flags & IFF_UP)) {
		dprintk("Error, master_dev is not up\n");
		return -EPERM;
	}

	/* already enslaved */
	if (slave_dev->flags & IFF_SLAVE) {
		dprintk("Error, Device was already enslaved\n");
		return -EBUSY;
	}

	/* vlan challenged mutual exclusion */
	/* no need to lock since we're protected by rtnl_lock */
	if (slave_dev->features & NETIF_F_VLAN_CHALLENGED) {
		dprintk("%s: NETIF_F_VLAN_CHALLENGED\n", slave_dev->name);
		if (!list_empty(&bond->vlan_list)) {
			printk(KERN_ERR DRV_NAME
			       ": Error: cannot enslave VLAN "
			       "challenged slave %s on VLAN enabled "
			       "bond %s\n", slave_dev->name,
			       bond_dev->name);
			return -EPERM;
		} else {
			printk(KERN_WARNING DRV_NAME
			       ": Warning: enslaved VLAN challenged "
			       "slave %s. Adding VLANs will be blocked as "
			       "long as %s is part of bond %s\n",
			       slave_dev->name, slave_dev->name,
			       bond_dev->name);
			bond_dev->features |= NETIF_F_VLAN_CHALLENGED;
		}
	} else {
		dprintk("%s: ! NETIF_F_VLAN_CHALLENGED\n", slave_dev->name);
		if (bond->slave_cnt == 0) {
			/* First slave, and it is not VLAN challenged,
			 * so remove the block of adding VLANs over the bond.
			 */
			bond_dev->features &= ~NETIF_F_VLAN_CHALLENGED;
		}
	}

	if (app_abi_ver >= 1) {
		/* The application is using an ABI, which requires the
		 * slave interface to be closed.
		 */
		if ((slave_dev->flags & IFF_UP)) {
			printk(KERN_ERR DRV_NAME
			       ": Error: %s is up\n",
			       slave_dev->name);
			res = -EPERM;
			goto err_undo_flags;
		}

		if (slave_dev->set_mac_address == NULL) {
			printk(KERN_ERR DRV_NAME
			       ": Error: The slave device you specified does "
			       "not support setting the MAC address.\n");
			printk(KERN_ERR
			       "Your kernel likely does not support slave "
			       "devices.\n");

			res = -EOPNOTSUPP;
			goto err_undo_flags;
		}
	} else {
		/* The application is not using an ABI, which requires the
		 * slave interface to be open.
		 */
		if (!(slave_dev->flags & IFF_UP)) {
			printk(KERN_ERR DRV_NAME
			       ": Error: %s is not running\n",
			       slave_dev->name);
			res = -EINVAL;
			goto err_undo_flags;
		}

		if ((bond->params.mode == BOND_MODE_8023AD) ||
		    (bond->params.mode == BOND_MODE_TLB)    ||
		    (bond->params.mode == BOND_MODE_ALB)) {
			printk(KERN_ERR DRV_NAME
			       ": Error: to use %s mode, you must upgrade "
			       "ifenslave.\n",
			       bond_mode_name(bond->params.mode));
			res = -EOPNOTSUPP;
			goto err_undo_flags;
		}
	}

	new_slave = kmalloc(sizeof(struct slave), GFP_KERNEL);
	if (!new_slave) {
		res = -ENOMEM;
		goto err_undo_flags;
	}

	memset(new_slave, 0, sizeof(struct slave));

	/* save slave's original flags before calling
	 * netdev_set_master and dev_open
	 */
	new_slave->original_flags = slave_dev->flags;

	if (app_abi_ver >= 1) {
		/* save slave's original ("permanent") mac address for
		 * modes that needs it, and for restoring it upon release,
		 * and then set it to the master's address
		 */
		memcpy(new_slave->perm_hwaddr, slave_dev->dev_addr, ETH_ALEN);

		/* set slave to master's mac address
		 * The application already set the master's
		 * mac address to that of the first slave
		 */
		memcpy(addr.sa_data, bond_dev->dev_addr, bond_dev->addr_len);
		addr.sa_family = slave_dev->type;
		res = slave_dev->set_mac_address(slave_dev, &addr);
		if (res) {
			dprintk("Error %d calling set_mac_address\n", res);
			goto err_free;
		}

		/* open the slave since the application closed it */
		res = dev_open(slave_dev);
		if (res) {
			dprintk("Openning slave %s failed\n", slave_dev->name);
			goto err_restore_mac;
		}
	}

	res = netdev_set_master(slave_dev, bond_dev);
	if (res) {
		dprintk("Error %d calling netdev_set_master\n", res);
		if (app_abi_ver < 1) {
			goto err_free;
		} else {
			goto err_close;
		}
	}

	new_slave->dev = slave_dev;

	if ((bond->params.mode == BOND_MODE_TLB) ||
	    (bond->params.mode == BOND_MODE_ALB)) {
		/* bond_alb_init_slave() must be called before all other stages since
		 * it might fail and we do not want to have to undo everything
		 */
		res = bond_alb_init_slave(bond, new_slave);
		if (res) {
			goto err_unset_master;
		}
	}

	/* If the mode USES_PRIMARY, then the new slave gets the
	 * master's promisc (and mc) settings only if it becomes the
	 * curr_active_slave, and that is taken care of later when calling
	 * bond_change_active()
	 */
	if (!USES_PRIMARY(bond->params.mode)) {
		/* set promiscuity level to new slave */
		if (bond_dev->flags & IFF_PROMISC) {
			dev_set_promiscuity(slave_dev, 1);
		}

		/* set allmulti level to new slave */
		if (bond_dev->flags & IFF_ALLMULTI) {
			dev_set_allmulti(slave_dev, 1);
		}

		/* upload master's mc_list to new slave */
		for (dmi = bond_dev->mc_list; dmi; dmi = dmi->next) {
			dev_mc_add (slave_dev, dmi->dmi_addr, dmi->dmi_addrlen, 0);
		}
	}

	if (bond->params.mode == BOND_MODE_8023AD) {
		/* add lacpdu mc addr to mc list */
		u8 lacpdu_multicast[ETH_ALEN] = MULTICAST_LACPDU_ADDR;

		dev_mc_add(slave_dev, lacpdu_multicast, ETH_ALEN, 0);
	}

	bond_add_vlans_on_slave(bond, slave_dev);

	write_lock_bh(&bond->lock);

	bond_attach_slave(bond, new_slave);

	new_slave->delay = 0;
	new_slave->link_failure_count = 0;

	if (bond->params.miimon && !bond->params.use_carrier) {
		link_reporting = bond_check_dev_link(bond, slave_dev, 1);

		if ((link_reporting == -1) && !bond->params.arp_interval) {
			/*
			 * miimon is set but a bonded network driver
			 * does not support ETHTOOL/MII and
			 * arp_interval is not set.  Note: if
			 * use_carrier is enabled, we will never go
			 * here (because netif_carrier is always
			 * supported); thus, we don't need to change
			 * the messages for netif_carrier.
			 */
			printk(KERN_WARNING DRV_NAME
			       ": Warning: MII and ETHTOOL support not "
			       "available for interface %s, and "
			       "arp_interval/arp_ip_target module parameters "
			       "not specified, thus bonding will not detect "
			       "link failures! see bonding.txt for details.\n",
			       slave_dev->name);
		} else if (link_reporting == -1) {
			/* unable get link status using mii/ethtool */
			printk(KERN_WARNING DRV_NAME
			       ": Warning: can't get link status from "
			       "interface %s; the network driver associated "
			       "with this interface does not support MII or "
			       "ETHTOOL link status reporting, thus miimon "
			       "has no effect on this interface.\n",
			       slave_dev->name);
		}
	}

	/* check for initial state */
	if (!bond->params.miimon ||
	    (bond_check_dev_link(bond, slave_dev, 0) == BMSR_LSTATUS)) {
		if (bond->params.updelay) {
			dprintk("Initial state of slave_dev is "
				"BOND_LINK_BACK\n");
			new_slave->link  = BOND_LINK_BACK;
			new_slave->delay = bond->params.updelay;
		} else {
			dprintk("Initial state of slave_dev is "
				"BOND_LINK_UP\n");
			new_slave->link  = BOND_LINK_UP;
		}
		new_slave->jiffies = jiffies;
	} else {
		dprintk("Initial state of slave_dev is "
			"BOND_LINK_DOWN\n");
		new_slave->link  = BOND_LINK_DOWN;
	}

	if (bond_update_speed_duplex(new_slave) &&
	    (new_slave->link != BOND_LINK_DOWN)) {
		printk(KERN_WARNING DRV_NAME
		       ": Warning: failed to get speed/duplex from %s, speed "
		       "forced to 100Mbps, duplex forced to Full.\n",
		       new_slave->dev->name);

		if (bond->params.mode == BOND_MODE_8023AD) {
			printk(KERN_WARNING
			       "Operation of 802.3ad mode requires ETHTOOL "
			       "support in base driver for proper aggregator "
			       "selection.\n");
		}
	}

	if (USES_PRIMARY(bond->params.mode) && bond->params.primary[0]) {
		/* if there is a primary slave, remember it */
		if (strcmp(bond->params.primary, new_slave->dev->name) == 0) {
			bond->primary_slave = new_slave;
		}
	}

	switch (bond->params.mode) {
	case BOND_MODE_ACTIVEBACKUP:
		/* if we're in active-backup mode, we need one and only one active
		 * interface. The backup interfaces will have their NOARP flag set
		 * because we need them to be completely deaf and not to respond to
		 * any ARP request on the network to avoid fooling a switch. Thus,
		 * since we guarantee that curr_active_slave always point to the last
		 * usable interface, we just have to verify this interface's flag.
		 */
		if (((!bond->curr_active_slave) ||
		     (bond->curr_active_slave->dev->flags & IFF_NOARP)) &&
		    (new_slave->link != BOND_LINK_DOWN)) {
			dprintk("This is the first active slave\n");
			/* first slave or no active slave yet, and this link
			   is OK, so make this interface the active one */
			bond_change_active_slave(bond, new_slave);
		} else {
			dprintk("This is just a backup slave\n");
			bond_set_slave_inactive_flags(new_slave);
		}
		break;
	case BOND_MODE_8023AD:
		/* in 802.3ad mode, the internal mechanism
		 * will activate the slaves in the selected
		 * aggregator
		 */
		bond_set_slave_inactive_flags(new_slave);
		/* if this is the first slave */
		if (bond->slave_cnt == 1) {
			SLAVE_AD_INFO(new_slave).id = 1;
			/* Initialize AD with the number of times that the AD timer is called in 1 second
			 * can be called only after the mac address of the bond is set
			 */
			bond_3ad_initialize(bond, 1000/AD_TIMER_INTERVAL,
					    bond->params.lacp_fast);
		} else {
			SLAVE_AD_INFO(new_slave).id =
				SLAVE_AD_INFO(new_slave->prev).id + 1;
		}

		bond_3ad_bind_slave(new_slave);
		break;
	case BOND_MODE_TLB:
	case BOND_MODE_ALB:
		new_slave->state = BOND_STATE_ACTIVE;
		if ((!bond->curr_active_slave) &&
		    (new_slave->link != BOND_LINK_DOWN)) {
			/* first slave or no active slave yet, and this link
			 * is OK, so make this interface the active one
			 */
			bond_change_active_slave(bond, new_slave);
		}
		break;
	default:
		dprintk("This slave is always active in trunk mode\n");

		/* always active in trunk mode */
		new_slave->state = BOND_STATE_ACTIVE;

		/* In trunking mode there is little meaning to curr_active_slave
		 * anyway (it holds no special properties of the bond device),
		 * so we can change it without calling change_active_interface()
		 */
		if (!bond->curr_active_slave) {
			bond->curr_active_slave = new_slave;
		}
		break;
	} /* switch(bond_mode) */

	write_unlock_bh(&bond->lock);

	if (app_abi_ver < 1) {
		/*
		 * !!! This is to support old versions of ifenslave.
		 * We can remove this in 2.5 because our ifenslave takes
		 * care of this for us.
		 * We check to see if the master has a mac address yet.
		 * If not, we'll give it the mac address of our slave device.
		 */
		int ndx = 0;

		for (ndx = 0; ndx < bond_dev->addr_len; ndx++) {
			dprintk("Checking ndx=%d of bond_dev->dev_addr\n",
				ndx);
			if (bond_dev->dev_addr[ndx] != 0) {
				dprintk("Found non-zero byte at ndx=%d\n",
					ndx);
				break;
			}
		}

		if (ndx == bond_dev->addr_len) {
			/*
			 * We got all the way through the address and it was
			 * all 0's.
			 */
			dprintk("%s doesn't have a MAC address yet.  \n",
				bond_dev->name);
			dprintk("Going to give assign it from %s.\n",
				slave_dev->name);
			bond_sethwaddr(bond_dev, slave_dev);
		}
	}

	printk(KERN_INFO DRV_NAME
	       ": %s: enslaving %s as a%s interface with a%s link.\n",
	       bond_dev->name, slave_dev->name,
	       new_slave->state == BOND_STATE_ACTIVE ? "n active" : " backup",
	       new_slave->link != BOND_LINK_DOWN ? "n up" : " down");

	/* enslave is successful */
	return 0;

/* Undo stages on error */
err_unset_master:
	netdev_set_master(slave_dev, NULL);

err_close:
	dev_close(slave_dev);

err_restore_mac:
	memcpy(addr.sa_data, new_slave->perm_hwaddr, ETH_ALEN);
	addr.sa_family = slave_dev->type;
	slave_dev->set_mac_address(slave_dev, &addr);

err_free:
	kfree(new_slave);

err_undo_flags:
	bond_dev->features = old_features;

	return res;
}

/*
 * Try to release the slave device <slave> from the bond device <master>
 * It is legal to access curr_active_slave without a lock because all the function
 * is write-locked.
 *
 * The rules for slave state should be:
 *   for Active/Backup:
 *     Active stays on all backups go down
 *   for Bonded connections:
 *     The first up interface should be left on and all others downed.
 */
static int bond_release(struct net_device *bond_dev, struct net_device *slave_dev)
{
	struct bonding *bond = bond_dev->priv;
	struct slave *slave, *oldcurrent;
	struct sockaddr addr;
	int mac_addr_differ;

	/* slave is not a slave or master is not master of this slave */
	if (!(slave_dev->flags & IFF_SLAVE) ||
	    (slave_dev->master != bond_dev)) {
		printk(KERN_ERR DRV_NAME
		       ": Error: %s: cannot release %s.\n",
		       bond_dev->name, slave_dev->name);
		return -EINVAL;
	}

	write_lock_bh(&bond->lock);

	slave = bond_get_slave_by_dev(bond, slave_dev);
	if (!slave) {
		/* not a slave of this bond */
		printk(KERN_INFO DRV_NAME
		       ": %s: %s not enslaved\n",
		       bond_dev->name, slave_dev->name);
		return -EINVAL;
	}

	mac_addr_differ = memcmp(bond_dev->dev_addr,
				 slave->perm_hwaddr,
				 ETH_ALEN);
	if (!mac_addr_differ && (bond->slave_cnt > 1)) {
		printk(KERN_WARNING DRV_NAME
		       ": Warning: the permanent HWaddr of %s "
		       "- %02X:%02X:%02X:%02X:%02X:%02X - is "
		       "still in use by %s. Set the HWaddr of "
		       "%s to a different address to avoid "
		       "conflicts.\n",
		       slave_dev->name,
		       slave->perm_hwaddr[0],
		       slave->perm_hwaddr[1],
		       slave->perm_hwaddr[2],
		       slave->perm_hwaddr[3],
		       slave->perm_hwaddr[4],
		       slave->perm_hwaddr[5],
		       bond_dev->name,
		       slave_dev->name);
	}

	/* Inform AD package of unbinding of slave. */
	if (bond->params.mode == BOND_MODE_8023AD) {
		/* must be called before the slave is
		 * detached from the list
		 */
		bond_3ad_unbind_slave(slave);
	}

	printk(KERN_INFO DRV_NAME
	       ": %s: releasing %s interface %s\n",
	       bond_dev->name,
	       (slave->state == BOND_STATE_ACTIVE)
	       ? "active" : "backup",
	       slave_dev->name);

	oldcurrent = bond->curr_active_slave;

	bond->current_arp_slave = NULL;

	/* release the slave from its bond */
	bond_detach_slave(bond, slave);

	if (bond->primary_slave == slave) {
		bond->primary_slave = NULL;
	}

	if (oldcurrent == slave) {
		bond_change_active_slave(bond, NULL);
	}

	if ((bond->params.mode == BOND_MODE_TLB) ||
	    (bond->params.mode == BOND_MODE_ALB)) {
		/* Must be called only after the slave has been
		 * detached from the list and the curr_active_slave
		 * has been cleared (if our_slave == old_current),
		 * but before a new active slave is selected.
		 */
		bond_alb_deinit_slave(bond, slave);
	}

	if (oldcurrent == slave) {
		bond_select_active_slave(bond);

		if (!bond->curr_active_slave) {
			printk(KERN_INFO DRV_NAME
			       ": %s: now running without any active "
			       "interface !\n",
			       bond_dev->name);
		}
	}

	if (bond->slave_cnt == 0) {
		/* if the last slave was removed, zero the mac address
		 * of the master so it will be set by the application
		 * to the mac address of the first slave
		 */
		memset(bond_dev->dev_addr, 0, bond_dev->addr_len);

		if (list_empty(&bond->vlan_list)) {
			bond_dev->features |= NETIF_F_VLAN_CHALLENGED;
		} else {
			printk(KERN_WARNING DRV_NAME
			       ": Warning: clearing HW address of %s while it "
			       "still has VLANs.\n",
			       bond_dev->name);
			printk(KERN_WARNING DRV_NAME
			       ": When re-adding slaves, make sure the bond's "
			       "HW address matches its VLANs'.\n");
		}
	} else if ((bond_dev->features & NETIF_F_VLAN_CHALLENGED) &&
		   !bond_has_challenged_slaves(bond)) {
		printk(KERN_INFO DRV_NAME
		       ": last VLAN challenged slave %s "
		       "left bond %s. VLAN blocking is removed\n",
		       slave_dev->name, bond_dev->name);
		bond_dev->features &= ~NETIF_F_VLAN_CHALLENGED;
	}

	write_unlock_bh(&bond->lock);

	bond_del_vlans_from_slave(bond, slave_dev);

	/* If the mode USES_PRIMARY, then we should only remove its
	 * promisc and mc settings if it was the curr_active_slave, but that was
	 * already taken care of above when we detached the slave
	 */
	if (!USES_PRIMARY(bond->params.mode)) {
		/* unset promiscuity level from slave */
		if (bond_dev->flags & IFF_PROMISC) {
			dev_set_promiscuity(slave_dev, -1);
		}

		/* unset allmulti level from slave */
		if (bond_dev->flags & IFF_ALLMULTI) {
			dev_set_allmulti(slave_dev, -1);
		}

		/* flush master's mc_list from slave */
		bond_mc_list_flush(bond_dev, slave_dev);
	}

	netdev_set_master(slave_dev, NULL);

	/* close slave before restoring its mac address */
	dev_close(slave_dev);

	if (app_abi_ver >= 1) {
		/* restore original ("permanent") mac address */
		memcpy(addr.sa_data, slave->perm_hwaddr, ETH_ALEN);
		addr.sa_family = slave_dev->type;
		slave_dev->set_mac_address(slave_dev, &addr);
	}

	/* restore the original state of the
	 * IFF_NOARP flag that might have been
	 * set by bond_set_slave_inactive_flags()
	 */
	if ((slave->original_flags & IFF_NOARP) == 0) {
		slave_dev->flags &= ~IFF_NOARP;
	}

	kfree(slave);

	return 0;  /* deletion OK */
}

/*
 * This function releases all slaves.
 */
static int bond_release_all(struct net_device *bond_dev)
{
	struct bonding *bond = bond_dev->priv;
	struct slave *slave;
	struct net_device *slave_dev;
	struct sockaddr addr;

	write_lock_bh(&bond->lock);

	if (bond->slave_cnt == 0) {
		goto out;
	}

	bond->current_arp_slave = NULL;
	bond->primary_slave = NULL;
	bond_change_active_slave(bond, NULL);

	while ((slave = bond->first_slave) != NULL) {
		/* Inform AD package of unbinding of slave
		 * before slave is detached from the list.
		 */
		if (bond->params.mode == BOND_MODE_8023AD) {
			bond_3ad_unbind_slave(slave);
		}

		slave_dev = slave->dev;
		bond_detach_slave(bond, slave);

		if ((bond->params.mode == BOND_MODE_TLB) ||
		    (bond->params.mode == BOND_MODE_ALB)) {
			/* must be called only after the slave
			 * has been detached from the list
			 */
			bond_alb_deinit_slave(bond, slave);
		}

		/* now that the slave is detached, unlock and perform
		 * all the undo steps that should not be called from
		 * within a lock.
		 */
		write_unlock_bh(&bond->lock);

		bond_del_vlans_from_slave(bond, slave_dev);

		/* If the mode USES_PRIMARY, then we should only remove its
		 * promisc and mc settings if it was the curr_active_slave, but that was
		 * already taken care of above when we detached the slave
		 */
		if (!USES_PRIMARY(bond->params.mode)) {
			/* unset promiscuity level from slave */
			if (bond_dev->flags & IFF_PROMISC) {
				dev_set_promiscuity(slave_dev, -1);
			}

			/* unset allmulti level from slave */
			if (bond_dev->flags & IFF_ALLMULTI) {
				dev_set_allmulti(slave_dev, -1);
			}

			/* flush master's mc_list from slave */
			bond_mc_list_flush(bond_dev, slave_dev);
		}

		netdev_set_master(slave_dev, NULL);

		/* close slave before restoring its mac address */
		dev_close(slave_dev);

		if (app_abi_ver >= 1) {
			/* restore original ("permanent") mac address*/
			memcpy(addr.sa_data, slave->perm_hwaddr, ETH_ALEN);
			addr.sa_family = slave_dev->type;
			slave_dev->set_mac_address(slave_dev, &addr);
		}

		/* restore the original state of the IFF_NOARP flag that might have
		 * been set by bond_set_slave_inactive_flags()
		 */
		if ((slave->original_flags & IFF_NOARP) == 0) {
			slave_dev->flags &= ~IFF_NOARP;
		}

		kfree(slave);

		/* re-acquire the lock before getting the next slave */
		write_lock_bh(&bond->lock);
	}

	/* zero the mac address of the master so it will be
	 * set by the application to the mac address of the
	 * first slave
	 */
	memset(bond_dev->dev_addr, 0, bond_dev->addr_len);

	if (list_empty(&bond->vlan_list)) {
		bond_dev->features |= NETIF_F_VLAN_CHALLENGED;
	} else {
		printk(KERN_WARNING DRV_NAME
		       ": Warning: clearing HW address of %s while it "
		       "still has VLANs.\n",
		       bond_dev->name);
		printk(KERN_WARNING DRV_NAME
		       ": When re-adding slaves, make sure the bond's "
		       "HW address matches its VLANs'.\n");
	}

	printk(KERN_INFO DRV_NAME
	       ": %s: released all slaves\n",
	       bond_dev->name);

out:
	write_unlock_bh(&bond->lock);

	return 0;
}

/*
 * This function changes the active slave to slave <slave_dev>.
 * It returns -EINVAL in the following cases.
 *  - <slave_dev> is not found in the list.
 *  - There is not active slave now.
 *  - <slave_dev> is already active.
 *  - The link state of <slave_dev> is not BOND_LINK_UP.
 *  - <slave_dev> is not running.
 * In these cases, this fuction does nothing.
 * In the other cases, currnt_slave pointer is changed and 0 is returned.
 */
static int bond_ioctl_change_active(struct net_device *bond_dev, struct net_device *slave_dev)
{
	struct bonding *bond = bond_dev->priv;
	struct slave *old_active = NULL;
	struct slave *new_active = NULL;
	int res = 0;

	if (!USES_PRIMARY(bond->params.mode)) {
		return -EINVAL;
	}

	/* Verify that master_dev is indeed the master of slave_dev */
	if (!(slave_dev->flags & IFF_SLAVE) ||
	    (slave_dev->master != bond_dev)) {
		return -EINVAL;
	}

	write_lock_bh(&bond->lock);

	old_active = bond->curr_active_slave;
	new_active = bond_get_slave_by_dev(bond, slave_dev);

	/*
	 * Changing to the current active: do nothing; return success.
	 */
	if (new_active && (new_active == old_active)) {
		write_unlock_bh(&bond->lock);
		return 0;
	}

	if ((new_active) &&
	    (old_active) &&
	    (new_active->link == BOND_LINK_UP) &&
	    IS_UP(new_active->dev)) {
		bond_change_active_slave(bond, new_active);
	} else {
		res = -EINVAL;
	}

	write_unlock_bh(&bond->lock);

	return res;
}

static int bond_ethtool_ioctl(struct net_device *bond_dev, struct ifreq *ifr)
{
	struct ethtool_drvinfo info;
	void __user *addr = ifr->ifr_data;
	uint32_t cmd;

	if (get_user(cmd, (uint32_t __user *)addr)) {
		return -EFAULT;
	}

	switch (cmd) {
	case ETHTOOL_GDRVINFO:
		if (copy_from_user(&info, addr, sizeof(info))) {
			return -EFAULT;
		}

		if (strcmp(info.driver, "ifenslave") == 0) {
			int new_abi_ver;
			char *endptr;

			new_abi_ver = simple_strtoul(info.fw_version,
						     &endptr, 0);
			if (*endptr) {
				printk(KERN_ERR DRV_NAME
				       ": Error: got invalid ABI "
				       "version from application\n");

				return -EINVAL;
			}

			if (orig_app_abi_ver == -1) {
				orig_app_abi_ver  = new_abi_ver;
			}

			app_abi_ver = new_abi_ver;
		}

		strncpy(info.driver,  DRV_NAME, 32);
		strncpy(info.version, DRV_VERSION, 32);
		snprintf(info.fw_version, 32, "%d", BOND_ABI_VERSION);

		if (copy_to_user(addr, &info, sizeof(info))) {
			return -EFAULT;
		}

		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int bond_info_query(struct net_device *bond_dev, struct ifbond *info)
{
	struct bonding *bond = bond_dev->priv;

	info->bond_mode = bond->params.mode;
	info->miimon = bond->params.miimon;

	read_lock_bh(&bond->lock);
	info->num_slaves = bond->slave_cnt;
	read_unlock_bh(&bond->lock);

	return 0;
}

static int bond_slave_info_query(struct net_device *bond_dev, struct ifslave *info)
{
	struct bonding *bond = bond_dev->priv;
	struct slave *slave;
	int i, found = 0;

	if (info->slave_id < 0) {
		return -ENODEV;
	}

	read_lock_bh(&bond->lock);

	bond_for_each_slave(bond, slave, i) {
		if (i == (int)info->slave_id) {
			found = 1;
			break;
		}
	}

	read_unlock_bh(&bond->lock);

	if (found) {
		strcpy(info->slave_name, slave->dev->name);
		info->link = slave->link;
		info->state = slave->state;
		info->link_failure_count = slave->link_failure_count;
	} else {
		return -ENODEV;
	}

	return 0;
}

/*-------------------------------- Monitoring -------------------------------*/

/* this function is called regularly to monitor each slave's link. */
static void bond_mii_monitor(struct net_device *bond_dev)
{
	struct bonding *bond = bond_dev->priv;
	struct slave *slave, *oldcurrent;
	int do_failover = 0;
	int delta_in_ticks;
	int i;

	read_lock(&bond->lock);

	delta_in_ticks = (bond->params.miimon * HZ) / 1000;

	if (bond->kill_timers) {
		goto out;
	}

	if (bond->slave_cnt == 0) {
		goto re_arm;
	}

	/* we will try to read the link status of each of our slaves, and
	 * set their IFF_RUNNING flag appropriately. For each slave not
	 * supporting MII status, we won't do anything so that a user-space
	 * program could monitor the link itself if needed.
	 */

	read_lock(&bond->curr_slave_lock);
	oldcurrent = bond->curr_active_slave;
	read_unlock(&bond->curr_slave_lock);

	bond_for_each_slave(bond, slave, i) {
		struct net_device *slave_dev = slave->dev;
		int link_state;
		u16 old_speed = slave->speed;
		u8 old_duplex = slave->duplex;

		link_state = bond_check_dev_link(bond, slave_dev, 0);

		switch (slave->link) {
		case BOND_LINK_UP:	/* the link was up */
			if (link_state == BMSR_LSTATUS) {
				/* link stays up, nothing more to do */
				break;
			} else { /* link going down */
				slave->link  = BOND_LINK_FAIL;
				slave->delay = bond->params.downdelay;

				if (slave->link_failure_count < UINT_MAX) {
					slave->link_failure_count++;
				}

				if (bond->params.downdelay) {
					printk(KERN_INFO DRV_NAME
					       ": %s: link status down for %s "
					       "interface %s, disabling it in "
					       "%d ms.\n",
					       bond_dev->name,
					       IS_UP(slave_dev)
					       ? ((bond->params.mode == BOND_MODE_ACTIVEBACKUP)
						  ? ((slave == oldcurrent)
						     ? "active " : "backup ")
						  : "")
					       : "idle ",
					       slave_dev->name,
					       bond->params.downdelay * bond->params.miimon);
				}
			}
			/* no break ! fall through the BOND_LINK_FAIL test to
			   ensure proper action to be taken
			*/
		case BOND_LINK_FAIL:	/* the link has just gone down */
			if (link_state != BMSR_LSTATUS) {
				/* link stays down */
				if (slave->delay <= 0) {
					/* link down for too long time */
					slave->link = BOND_LINK_DOWN;

					/* in active/backup mode, we must
					 * completely disable this interface
					 */
					if ((bond->params.mode == BOND_MODE_ACTIVEBACKUP) ||
					    (bond->params.mode == BOND_MODE_8023AD)) {
						bond_set_slave_inactive_flags(slave);
					}

					printk(KERN_INFO DRV_NAME
					       ": %s: link status definitely "
					       "down for interface %s, "
					       "disabling it\n",
					       bond_dev->name,
					       slave_dev->name);

					/* notify ad that the link status has changed */
					if (bond->params.mode == BOND_MODE_8023AD) {
						bond_3ad_handle_link_change(slave, BOND_LINK_DOWN);
					}

					if ((bond->params.mode == BOND_MODE_TLB) ||
					    (bond->params.mode == BOND_MODE_ALB)) {
						bond_alb_handle_link_change(bond, slave, BOND_LINK_DOWN);
					}

					if (slave == oldcurrent) {
						do_failover = 1;
					}
				} else {
					slave->delay--;
				}
			} else {
				/* link up again */
				slave->link  = BOND_LINK_UP;
				slave->jiffies = jiffies;
				printk(KERN_INFO DRV_NAME
				       ": %s: link status up again after %d "
				       "ms for interface %s.\n",
				       bond_dev->name,
				       (bond->params.downdelay - slave->delay) * bond->params.miimon,
				       slave_dev->name);
			}
			break;
		case BOND_LINK_DOWN:	/* the link was down */
			if (link_state != BMSR_LSTATUS) {
				/* the link stays down, nothing more to do */
				break;
			} else {	/* link going up */
				slave->link  = BOND_LINK_BACK;
				slave->delay = bond->params.updelay;

				if (bond->params.updelay) {
					/* if updelay == 0, no need to
					   advertise about a 0 ms delay */
					printk(KERN_INFO DRV_NAME
					       ": %s: link status up for "
					       "interface %s, enabling it "
					       "in %d ms.\n",
					       bond_dev->name,
					       slave_dev->name,
					       bond->params.updelay * bond->params.miimon);
				}
			}
			/* no break ! fall through the BOND_LINK_BACK state in
			   case there's something to do.
			*/
		case BOND_LINK_BACK:	/* the link has just come back */
			if (link_state != BMSR_LSTATUS) {
				/* link down again */
				slave->link  = BOND_LINK_DOWN;

				printk(KERN_INFO DRV_NAME
				       ": %s: link status down again after %d "
				       "ms for interface %s.\n",
				       bond_dev->name,
				       (bond->params.updelay - slave->delay) * bond->params.miimon,
				       slave_dev->name);
			} else {
				/* link stays up */
				if (slave->delay == 0) {
					/* now the link has been up for long time enough */
					slave->link = BOND_LINK_UP;
					slave->jiffies = jiffies;

					if (bond->params.mode == BOND_MODE_8023AD) {
						/* prevent it from being the active one */
						slave->state = BOND_STATE_BACKUP;
					} else if (bond->params.mode != BOND_MODE_ACTIVEBACKUP) {
						/* make it immediately active */
						slave->state = BOND_STATE_ACTIVE;
					} else if (slave != bond->primary_slave) {
						/* prevent it from being the active one */
						slave->state = BOND_STATE_BACKUP;
					}

					printk(KERN_INFO DRV_NAME
					       ": %s: link status definitely "
					       "up for interface %s.\n",
					       bond_dev->name,
					       slave_dev->name);

					/* notify ad that the link status has changed */
					if (bond->params.mode == BOND_MODE_8023AD) {
						bond_3ad_handle_link_change(slave, BOND_LINK_UP);
					}

					if ((bond->params.mode == BOND_MODE_TLB) ||
					    (bond->params.mode == BOND_MODE_ALB)) {
						bond_alb_handle_link_change(bond, slave, BOND_LINK_UP);
					}

					if ((!oldcurrent) ||
					    (slave == bond->primary_slave)) {
						do_failover = 1;
					}
				} else {
					slave->delay--;
				}
			}
			break;
		default:
			/* Should not happen */
			printk(KERN_ERR "bonding: Error: %s  Illegal value (link=%d)\n",
			       slave->dev->name, slave->link);
			goto out;
		} /* end of switch (slave->link) */

		bond_update_speed_duplex(slave);

		if (bond->params.mode == BOND_MODE_8023AD) {
			if (old_speed != slave->speed) {
				bond_3ad_adapter_speed_changed(slave);
			}

			if (old_duplex != slave->duplex) {
				bond_3ad_adapter_duplex_changed(slave);
			}
		}

	} /* end of for */

	if (do_failover) {
		write_lock(&bond->curr_slave_lock);

		bond_select_active_slave(bond);

		if (oldcurrent && !bond->curr_active_slave) {
			printk(KERN_INFO DRV_NAME
			       ": %s: now running without any active "
			       "interface !\n",
			       bond_dev->name);
		}

		write_unlock(&bond->curr_slave_lock);
	}

re_arm:
	if (bond->params.miimon) {
		mod_timer(&bond->mii_timer, jiffies + delta_in_ticks);
	}
out:
	read_unlock(&bond->lock);
}

static void bond_arp_send_all(struct bonding *bond, struct slave *slave)
{
	int i;
	u32 *targets = bond->params.arp_targets;

	for (i = 0; (i < BOND_MAX_ARP_TARGETS) && targets[i]; i++) {
		arp_send(ARPOP_REQUEST, ETH_P_ARP, targets[i], slave->dev,
			 my_ip, NULL, slave->dev->dev_addr,
			 NULL);
	}
}

/*
 * this function is called regularly to monitor each slave's link
 * ensuring that traffic is being sent and received when arp monitoring
 * is used in load-balancing mode. if the adapter has been dormant, then an
 * arp is transmitted to generate traffic. see activebackup_arp_monitor for
 * arp monitoring in active backup mode.
 */
static void bond_loadbalance_arp_mon(struct net_device *bond_dev)
{
	struct bonding *bond = bond_dev->priv;
	struct slave *slave, *oldcurrent;
	int do_failover = 0;
	int delta_in_ticks;
	int i;

	read_lock(&bond->lock);

	delta_in_ticks = (bond->params.arp_interval * HZ) / 1000;

	if (bond->kill_timers) {
		goto out;
	}

	if (bond->slave_cnt == 0) {
		goto re_arm;
	}

	read_lock(&bond->curr_slave_lock);
	oldcurrent = bond->curr_active_slave;
	read_unlock(&bond->curr_slave_lock);

	/* see if any of the previous devices are up now (i.e. they have
	 * xmt and rcv traffic). the curr_active_slave does not come into
	 * the picture unless it is null. also, slave->jiffies is not needed
	 * here because we send an arp on each slave and give a slave as
	 * long as it needs to get the tx/rx within the delta.
	 * TODO: what about up/down delay in arp mode? it wasn't here before
	 *       so it can wait
	 */
	bond_for_each_slave(bond, slave, i) {
		if (slave->link != BOND_LINK_UP) {
			if (((jiffies - slave->dev->trans_start) <= delta_in_ticks) &&
			    ((jiffies - slave->dev->last_rx) <= delta_in_ticks)) {

				slave->link  = BOND_LINK_UP;
				slave->state = BOND_STATE_ACTIVE;

				/* primary_slave has no meaning in round-robin
				 * mode. the window of a slave being up and
				 * curr_active_slave being null after enslaving
				 * is closed.
				 */
				if (!oldcurrent) {
					printk(KERN_INFO DRV_NAME
					       ": %s: link status definitely "
					       "up for interface %s, ",
					       bond_dev->name,
					       slave->dev->name);
					do_failover = 1;
				} else {
					printk(KERN_INFO DRV_NAME
					       ": %s: interface %s is now up\n",
					       bond_dev->name,
					       slave->dev->name);
				}
			}
		} else {
			/* slave->link == BOND_LINK_UP */

			/* not all switches will respond to an arp request
			 * when the source ip is 0, so don't take the link down
			 * if we don't know our ip yet
			 */
			if (((jiffies - slave->dev->trans_start) >= (2*delta_in_ticks)) ||
			    (((jiffies - slave->dev->last_rx) >= (2*delta_in_ticks)) &&
			     my_ip)) {

				slave->link  = BOND_LINK_DOWN;
				slave->state = BOND_STATE_BACKUP;

				if (slave->link_failure_count < UINT_MAX) {
					slave->link_failure_count++;
				}

				printk(KERN_INFO DRV_NAME
				       ": %s: interface %s is now down.\n",
				       bond_dev->name,
				       slave->dev->name);

				if (slave == oldcurrent) {
					do_failover = 1;
				}
			}
		}

		/* note: if switch is in round-robin mode, all links
		 * must tx arp to ensure all links rx an arp - otherwise
		 * links may oscillate or not come up at all; if switch is
		 * in something like xor mode, there is nothing we can
		 * do - all replies will be rx'ed on same link causing slaves
		 * to be unstable during low/no traffic periods
		 */
		if (IS_UP(slave->dev)) {
			bond_arp_send_all(bond, slave);
		}
	}

	if (do_failover) {
		write_lock(&bond->curr_slave_lock);

		bond_select_active_slave(bond);

		if (oldcurrent && !bond->curr_active_slave) {
			printk(KERN_INFO DRV_NAME
			       ": %s: now running without any active "
			       "interface !\n",
			       bond_dev->name);
		}

		write_unlock(&bond->curr_slave_lock);
	}

re_arm:
	if (bond->params.arp_interval) {
		mod_timer(&bond->arp_timer, jiffies + delta_in_ticks);
	}
out:
	read_unlock(&bond->lock);
}

/*
 * When using arp monitoring in active-backup mode, this function is
 * called to determine if any backup slaves have went down or a new
 * current slave needs to be found.
 * The backup slaves never generate traffic, they are considered up by merely
 * receiving traffic. If the current slave goes down, each backup slave will
 * be given the opportunity to tx/rx an arp before being taken down - this
 * prevents all slaves from being taken down due to the current slave not
 * sending any traffic for the backups to receive. The arps are not necessarily
 * necessary, any tx and rx traffic will keep the current slave up. While any
 * rx traffic will keep the backup slaves up, the current slave is responsible
 * for generating traffic to keep them up regardless of any other traffic they
 * may have received.
 * see loadbalance_arp_monitor for arp monitoring in load balancing mode
 */
static void bond_activebackup_arp_mon(struct net_device *bond_dev)
{
	struct bonding *bond = bond_dev->priv;
	struct slave *slave;
	int delta_in_ticks;
	int i;

	read_lock(&bond->lock);

	delta_in_ticks = (bond->params.arp_interval * HZ) / 1000;

	if (bond->kill_timers) {
		goto out;
	}

	if (bond->slave_cnt == 0) {
		goto re_arm;
	}

	/* determine if any slave has come up or any backup slave has
	 * gone down
	 * TODO: what about up/down delay in arp mode? it wasn't here before
	 *       so it can wait
	 */
	bond_for_each_slave(bond, slave, i) {
		if (slave->link != BOND_LINK_UP) {
			if ((jiffies - slave->dev->last_rx) <= delta_in_ticks) {

				slave->link = BOND_LINK_UP;

				write_lock(&bond->curr_slave_lock);

				if ((!bond->curr_active_slave) &&
				    ((jiffies - slave->dev->trans_start) <= delta_in_ticks)) {
					bond_change_active_slave(bond, slave);
					bond->current_arp_slave = NULL;
				} else if (bond->curr_active_slave != slave) {
					/* this slave has just come up but we
					 * already have a current slave; this
					 * can also happen if bond_enslave adds
					 * a new slave that is up while we are
					 * searching for a new slave
					 */
					bond_set_slave_inactive_flags(slave);
					bond->current_arp_slave = NULL;
				}

				if (slave == bond->curr_active_slave) {
					printk(KERN_INFO DRV_NAME
					       ": %s: %s is up and now the "
					       "active interface\n",
					       bond_dev->name,
					       slave->dev->name);
				} else {
					printk(KERN_INFO DRV_NAME
					       ": %s: backup interface %s is "
					       "now up\n",
					       bond_dev->name,
					       slave->dev->name);
				}

				write_unlock(&bond->curr_slave_lock);
			}
		} else {
			read_lock(&bond->curr_slave_lock);

			if ((slave != bond->curr_active_slave) &&
			    (!bond->current_arp_slave) &&
			    (((jiffies - slave->dev->last_rx) >= 3*delta_in_ticks) &&
			     my_ip)) {
				/* a backup slave has gone down; three times
				 * the delta allows the current slave to be
				 * taken out before the backup slave.
				 * note: a non-null current_arp_slave indicates
				 * the curr_active_slave went down and we are
				 * searching for a new one; under this
				 * condition we only take the curr_active_slave
				 * down - this gives each slave a chance to
				 * tx/rx traffic before being taken out
				 */

				read_unlock(&bond->curr_slave_lock);

				slave->link  = BOND_LINK_DOWN;

				if (slave->link_failure_count < UINT_MAX) {
					slave->link_failure_count++;
				}

				bond_set_slave_inactive_flags(slave);

				printk(KERN_INFO DRV_NAME
				       ": %s: backup interface %s is now down\n",
				       bond_dev->name,
				       slave->dev->name);
			} else {
				read_unlock(&bond->curr_slave_lock);
			}
		}
	}

	read_lock(&bond->curr_slave_lock);
	slave = bond->curr_active_slave;
	read_unlock(&bond->curr_slave_lock);

	if (slave) {
		/* if we have sent traffic in the past 2*arp_intervals but
		 * haven't xmit and rx traffic in that time interval, select
		 * a different slave. slave->jiffies is only updated when
		 * a slave first becomes the curr_active_slave - not necessarily
		 * after every arp; this ensures the slave has a full 2*delta
		 * before being taken out. if a primary is being used, check
		 * if it is up and needs to take over as the curr_active_slave
		 */
		if ((((jiffies - slave->dev->trans_start) >= (2*delta_in_ticks)) ||
		     (((jiffies - slave->dev->last_rx) >= (2*delta_in_ticks)) &&
		      my_ip)) &&
		    ((jiffies - slave->jiffies) >= 2*delta_in_ticks)) {

			slave->link  = BOND_LINK_DOWN;

			if (slave->link_failure_count < UINT_MAX) {
				slave->link_failure_count++;
			}

			printk(KERN_INFO DRV_NAME
			       ": %s: link status down for active interface "
			       "%s, disabling it\n",
			       bond_dev->name,
			       slave->dev->name);

			write_lock(&bond->curr_slave_lock);

			bond_select_active_slave(bond);
			slave = bond->curr_active_slave;

			write_unlock(&bond->curr_slave_lock);

			bond->current_arp_slave = slave;

			if (slave) {
				slave->jiffies = jiffies;
			}
		} else if ((bond->primary_slave) &&
			   (bond->primary_slave != slave) &&
			   (bond->primary_slave->link == BOND_LINK_UP)) {
			/* at this point, slave is the curr_active_slave */
			printk(KERN_INFO DRV_NAME
			       ": %s: changing from interface %s to primary "
			       "interface %s\n",
			       bond_dev->name,
			       slave->dev->name,
			       bond->primary_slave->dev->name);

			/* primary is up so switch to it */
			write_lock(&bond->curr_slave_lock);
			bond_change_active_slave(bond, bond->primary_slave);
			write_unlock(&bond->curr_slave_lock);

			slave = bond->primary_slave;
			slave->jiffies = jiffies;
		} else {
			bond->current_arp_slave = NULL;
		}

		/* the current slave must tx an arp to ensure backup slaves
		 * rx traffic
		 */
		if (slave && my_ip) {
			bond_arp_send_all(bond, slave);
		}
	}

	/* if we don't have a curr_active_slave, search for the next available
	 * backup slave from the current_arp_slave and make it the candidate
	 * for becoming the curr_active_slave
	 */
	if (!slave) {
		if (!bond->current_arp_slave) {
			bond->current_arp_slave = bond->first_slave;
		}

		if (bond->current_arp_slave) {
			bond_set_slave_inactive_flags(bond->current_arp_slave);

			/* search for next candidate */
			bond_for_each_slave_from(bond, slave, i, bond->current_arp_slave) {
				if (IS_UP(slave->dev)) {
					slave->link = BOND_LINK_BACK;
					bond_set_slave_active_flags(slave);
					bond_arp_send_all(bond, slave);
					slave->jiffies = jiffies;
					bond->current_arp_slave = slave;
					break;
				}

				/* if the link state is up at this point, we
				 * mark it down - this can happen if we have
				 * simultaneous link failures and
				 * reselect_active_interface doesn't make this
				 * one the current slave so it is still marked
				 * up when it is actually down
				 */
				if (slave->link == BOND_LINK_UP) {
					slave->link  = BOND_LINK_DOWN;
					if (slave->link_failure_count < UINT_MAX) {
						slave->link_failure_count++;
					}

					bond_set_slave_inactive_flags(slave);

					printk(KERN_INFO DRV_NAME
					       ": %s: backup interface %s is "
					       "now down.\n",
					       bond_dev->name,
					       slave->dev->name);
				}
			}
		}
	}

re_arm:
	if (bond->params.arp_interval) {
		mod_timer(&bond->arp_timer, jiffies + delta_in_ticks);
	}
out:
	read_unlock(&bond->lock);
}

/*------------------------------ proc/seq_file-------------------------------*/

#ifdef CONFIG_PROC_FS

#define SEQ_START_TOKEN ((void *)1)

static void *bond_info_seq_start(struct seq_file *seq, loff_t *pos)
{
	struct bonding *bond = seq->private;
	loff_t off = 0;
	struct slave *slave;
	int i;

	/* make sure the bond won't be taken away */
	read_lock(&dev_base_lock);
	read_lock_bh(&bond->lock);

	if (*pos == 0) {
		return SEQ_START_TOKEN;
	}

	bond_for_each_slave(bond, slave, i) {
		if (++off == *pos) {
			return slave;
		}
	}

	return NULL;
}

static void *bond_info_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct bonding *bond = seq->private;
	struct slave *slave = v;

	++*pos;
	if (v == SEQ_START_TOKEN) {
		return bond->first_slave;
	}

	slave = slave->next;

	return (slave == bond->first_slave) ? NULL : slave;
}

static void bond_info_seq_stop(struct seq_file *seq, void *v)
{
	struct bonding *bond = seq->private;

	read_unlock_bh(&bond->lock);
	read_unlock(&dev_base_lock);
}

static void bond_info_show_master(struct seq_file *seq)
{
	struct bonding *bond = seq->private;
	struct slave *curr;

	read_lock(&bond->curr_slave_lock);
	curr = bond->curr_active_slave;
	read_unlock(&bond->curr_slave_lock);

	seq_printf(seq, "Bonding Mode: %s\n",
		   bond_mode_name(bond->params.mode));

	if (USES_PRIMARY(bond->params.mode)) {
		seq_printf(seq, "Primary Slave: %s\n",
			   (bond->params.primary[0]) ?
			   	bond->params.primary : "None");

		seq_printf(seq, "Currently Active Slave: %s\n",
			   (curr) ? curr->dev->name : "None");
	}

	seq_printf(seq, "MII Status: %s\n", (curr) ? "up" : "down");
	seq_printf(seq, "MII Polling Interval (ms): %d\n", bond->params.miimon);
	seq_printf(seq, "Up Delay (ms): %d\n",
		   bond->params.updelay * bond->params.miimon);
	seq_printf(seq, "Down Delay (ms): %d\n",
		   bond->params.downdelay * bond->params.miimon);

	if (bond->params.mode == BOND_MODE_8023AD) {
		struct ad_info ad_info;

		seq_puts(seq, "\n802.3ad info\n");
		seq_printf(seq, "LACP rate: %s\n",
			   (bond->params.lacp_fast) ? "fast" : "slow");

		if (bond_3ad_get_active_agg_info(bond, &ad_info)) {
			seq_printf(seq, "bond %s has no active aggregator\n",
				   bond->dev->name);
		} else {
			seq_printf(seq, "Active Aggregator Info:\n");

			seq_printf(seq, "\tAggregator ID: %d\n",
				   ad_info.aggregator_id);
			seq_printf(seq, "\tNumber of ports: %d\n",
				   ad_info.ports);
			seq_printf(seq, "\tActor Key: %d\n",
				   ad_info.actor_key);
			seq_printf(seq, "\tPartner Key: %d\n",
				   ad_info.partner_key);
			seq_printf(seq, "\tPartner Mac Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
				   ad_info.partner_system[0],
				   ad_info.partner_system[1],
				   ad_info.partner_system[2],
				   ad_info.partner_system[3],
				   ad_info.partner_system[4],
				   ad_info.partner_system[5]);
		}
	}
}

static void bond_info_show_slave(struct seq_file *seq, const struct slave *slave)
{
	struct bonding *bond = seq->private;

	seq_printf(seq, "\nSlave Interface: %s\n", slave->dev->name);
	seq_printf(seq, "MII Status: %s\n",
		   (slave->link == BOND_LINK_UP) ?  "up" : "down");
	seq_printf(seq, "Link Failure Count: %d\n",
		   slave->link_failure_count);

	if (app_abi_ver >= 1) {
		seq_printf(seq,
			   "Permanent HW addr: %02x:%02x:%02x:%02x:%02x:%02x\n",
			   slave->perm_hwaddr[0],
			   slave->perm_hwaddr[1],
			   slave->perm_hwaddr[2],
			   slave->perm_hwaddr[3],
			   slave->perm_hwaddr[4],
			   slave->perm_hwaddr[5]);
	}

	if (bond->params.mode == BOND_MODE_8023AD) {
		const struct aggregator *agg
			= SLAVE_AD_INFO(slave).port.aggregator;

		if (agg) {
			seq_printf(seq, "Aggregator ID: %d\n",
				   agg->aggregator_identifier);
		} else {
			seq_puts(seq, "Aggregator ID: N/A\n");
		}
	}
}

static int bond_info_seq_show(struct seq_file *seq, void *v)
{
	if (v == SEQ_START_TOKEN) {
		seq_printf(seq, "%s\n", version);
		bond_info_show_master(seq);
	} else {
		bond_info_show_slave(seq, v);
	}

	return 0;
}

static struct seq_operations bond_info_seq_ops = {
	.start = bond_info_seq_start,
	.next  = bond_info_seq_next,
	.stop  = bond_info_seq_stop,
	.show  = bond_info_seq_show,
};

static int bond_info_open(struct inode *inode, struct file *file)
{
	struct seq_file *seq;
	struct proc_dir_entry *proc;
	int res;

	res = seq_open(file, &bond_info_seq_ops);
	if (!res) {
		/* recover the pointer buried in proc_dir_entry data */
		seq = file->private_data;
		proc = PDE(inode);
		seq->private = proc->data;
	}

	return res;
}

static struct file_operations bond_info_fops = {
	.owner   = THIS_MODULE,
	.open    = bond_info_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release,
};

static int bond_create_proc_entry(struct bonding *bond)
{
	struct net_device *bond_dev = bond->dev;

	if (bond_proc_dir) {
		bond->proc_entry = create_proc_entry(bond_dev->name,
						     S_IRUGO,
						     bond_proc_dir);
		if (bond->proc_entry == NULL) {
			printk(KERN_WARNING DRV_NAME
			       ": Warning: Cannot create /proc/net/%s/%s\n",
			       DRV_NAME, bond_dev->name);
		} else {
			bond->proc_entry->data = bond;
			bond->proc_entry->proc_fops = &bond_info_fops;
			bond->proc_entry->owner = THIS_MODULE;
			memcpy(bond->proc_file_name, bond_dev->name, IFNAMSIZ);
		}
	}

	return 0;
}

static void bond_remove_proc_entry(struct bonding *bond)
{
	if (bond_proc_dir && bond->proc_entry) {
		remove_proc_entry(bond->proc_file_name, bond_proc_dir);
		memset(bond->proc_file_name, 0, IFNAMSIZ);
		bond->proc_entry = NULL;
	}
}

/* Create the bonding directory under /proc/net, if doesn't exist yet.
 * Caller must hold rtnl_lock.
 */
static void bond_create_proc_dir(void)
{
	int len = strlen(DRV_NAME);

	for (bond_proc_dir = proc_net->subdir; bond_proc_dir;
	     bond_proc_dir = bond_proc_dir->next) {
		if ((bond_proc_dir->namelen == len) &&
		    !memcmp(bond_proc_dir->name, DRV_NAME, len)) {
			break;
		}
	}

	if (!bond_proc_dir) {
		bond_proc_dir = proc_mkdir(DRV_NAME, proc_net);
		if (bond_proc_dir) {
			bond_proc_dir->owner = THIS_MODULE;
		} else {
			printk(KERN_WARNING DRV_NAME
				": Warning: cannot create /proc/net/%s\n",
				DRV_NAME);
		}
	}
}

/* Destroy the bonding directory under /proc/net, if empty.
 * Caller must hold rtnl_lock.
 */
static void bond_destroy_proc_dir(void)
{
	struct proc_dir_entry *de;

	if (!bond_proc_dir) {
		return;
	}

	/* verify that the /proc dir is empty */
	for (de = bond_proc_dir->subdir; de; de = de->next) {
		/* ignore . and .. */
		if (*(de->name) != '.') {
			break;
		}
	}

	if (de) {
		if (bond_proc_dir->owner == THIS_MODULE) {
			bond_proc_dir->owner = NULL;
		}
	} else {
		remove_proc_entry(DRV_NAME, proc_net);
		bond_proc_dir = NULL;
	}
}
#endif /* CONFIG_PROC_FS */

/*-------------------------- netdev event handling --------------------------*/

/*
 * Change device name
 */
static int bond_event_changename(struct bonding *bond)
{
#ifdef CONFIG_PROC_FS
	bond_remove_proc_entry(bond);
	bond_create_proc_entry(bond);
#endif

	return NOTIFY_DONE;
}

static int bond_master_netdev_event(unsigned long event, struct net_device *bond_dev)
{
	struct bonding *event_bond = bond_dev->priv;

	switch (event) {
	case NETDEV_CHANGENAME:
		return bond_event_changename(event_bond);
	case NETDEV_UNREGISTER:
		/*
		 * TODO: remove a bond from the list?
		 */
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static int bond_slave_netdev_event(unsigned long event, struct net_device *slave_dev)
{
	struct net_device *bond_dev = slave_dev->master;

	switch (event) {
	case NETDEV_UNREGISTER:
		if (bond_dev) {
			bond_release(bond_dev, slave_dev);
		}
		break;
	case NETDEV_CHANGE:
		/*
		 * TODO: is this what we get if somebody
		 * sets up a hierarchical bond, then rmmod's
		 * one of the slave bonding devices?
		 */
		break;
	case NETDEV_DOWN:
		/*
		 * ... Or is it this?
		 */
		break;
	case NETDEV_CHANGEMTU:
		/*
		 * TODO: Should slaves be allowed to
		 * independently alter their MTU?  For
		 * an active-backup bond, slaves need
		 * not be the same type of device, so
		 * MTUs may vary.  For other modes,
		 * slaves arguably should have the
		 * same MTUs. To do this, we'd need to
		 * take over the slave's change_mtu
		 * function for the duration of their
		 * servitude.
		 */
		break;
	case NETDEV_CHANGENAME:
		/*
		 * TODO: handle changing the primary's name
		 */
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

/*
 * bond_netdev_event: handle netdev notifier chain events.
 *
 * This function receives events for the netdev chain.  The caller (an
 * ioctl handler calling notifier_call_chain) holds the necessary
 * locks for us to safely manipulate the slave devices (RTNL lock,
 * dev_probe_lock).
 */
static int bond_netdev_event(struct notifier_block *this, unsigned long event, void *ptr)
{
	struct net_device *event_dev = (struct net_device *)ptr;

	dprintk("event_dev: %s, event: %lx\n",
		(event_dev ? event_dev->name : "None"),
		event);

	if (event_dev->flags & IFF_MASTER) {
		dprintk("IFF_MASTER\n");
		return bond_master_netdev_event(event, event_dev);
	}

	if (event_dev->flags & IFF_SLAVE) {
		dprintk("IFF_SLAVE\n");
		return bond_slave_netdev_event(event, event_dev);
	}

	return NOTIFY_DONE;
}

static struct notifier_block bond_netdev_notifier = {
	.notifier_call = bond_netdev_event,
};

/*-------------------------- Packet type handling ---------------------------*/

/* register to receive lacpdus on a bond */
static void bond_register_lacpdu(struct bonding *bond)
{
	struct packet_type *pk_type = &(BOND_AD_INFO(bond).ad_pkt_type);

	/* initialize packet type */
	pk_type->type = PKT_TYPE_LACPDU;
	pk_type->dev = bond->dev;
	pk_type->func = bond_3ad_lacpdu_recv;

	dev_add_pack(pk_type);
}

/* unregister to receive lacpdus on a bond */
static void bond_unregister_lacpdu(struct bonding *bond)
{
	dev_remove_pack(&(BOND_AD_INFO(bond).ad_pkt_type));
}

/*-------------------------- Device entry points ----------------------------*/

static int bond_open(struct net_device *bond_dev)
{
	struct bonding *bond = bond_dev->priv;
	struct timer_list *mii_timer = &bond->mii_timer;
	struct timer_list *arp_timer = &bond->arp_timer;

	bond->kill_timers = 0;

	if ((bond->params.mode == BOND_MODE_TLB) ||
	    (bond->params.mode == BOND_MODE_ALB)) {
		struct timer_list *alb_timer = &(BOND_ALB_INFO(bond).alb_timer);

		/* bond_alb_initialize must be called before the timer
		 * is started.
		 */
		if (bond_alb_initialize(bond, (bond->params.mode == BOND_MODE_ALB))) {
			/* something went wrong - fail the open operation */
			return -1;
		}

		init_timer(alb_timer);
		alb_timer->expires  = jiffies + 1;
		alb_timer->data     = (unsigned long)bond;
		alb_timer->function = (void *)&bond_alb_monitor;
		add_timer(alb_timer);
	}

	if (bond->params.miimon) {  /* link check interval, in milliseconds. */
		init_timer(mii_timer);
		mii_timer->expires  = jiffies + 1;
		mii_timer->data     = (unsigned long)bond_dev;
		mii_timer->function = (void *)&bond_mii_monitor;
		add_timer(mii_timer);
	}

	if (bond->params.arp_interval) {  /* arp interval, in milliseconds. */
		init_timer(arp_timer);
		arp_timer->expires  = jiffies + 1;
		arp_timer->data     = (unsigned long)bond_dev;
		if (bond->params.mode == BOND_MODE_ACTIVEBACKUP) {
			arp_timer->function = (void *)&bond_activebackup_arp_mon;
		} else {
			arp_timer->function = (void *)&bond_loadbalance_arp_mon;
		}
		add_timer(arp_timer);
	}

	if (bond->params.mode == BOND_MODE_8023AD) {
		struct timer_list *ad_timer = &(BOND_AD_INFO(bond).ad_timer);
		init_timer(ad_timer);
		ad_timer->expires  = jiffies + 1;
		ad_timer->data     = (unsigned long)bond;
		ad_timer->function = (void *)&bond_3ad_state_machine_handler;
		add_timer(ad_timer);

		/* register to receive LACPDUs */
		bond_register_lacpdu(bond);
	}

	return 0;
}

static int bond_close(struct net_device *bond_dev)
{
	struct bonding *bond = bond_dev->priv;

	write_lock_bh(&bond->lock);

	bond_mc_list_destroy(bond);

	if (bond->params.mode == BOND_MODE_8023AD) {
		/* Unregister the receive of LACPDUs */
		bond_unregister_lacpdu(bond);
	}

	/* signal timers not to re-arm */
	bond->kill_timers = 1;

	write_unlock_bh(&bond->lock);

	/* del_timer_sync must run without holding the bond->lock
	 * because a running timer might be trying to hold it too
	 */

	if (bond->params.miimon) {  /* link check interval, in milliseconds. */
		del_timer_sync(&bond->mii_timer);
	}

	if (bond->params.arp_interval) {  /* arp interval, in milliseconds. */
		del_timer_sync(&bond->arp_timer);
	}

	switch (bond->params.mode) {
	case BOND_MODE_8023AD:
		del_timer_sync(&(BOND_AD_INFO(bond).ad_timer));
		break;
	case BOND_MODE_TLB:
	case BOND_MODE_ALB:
		del_timer_sync(&(BOND_ALB_INFO(bond).alb_timer));
		break;
	default:
		break;
	}

	/* Release the bonded slaves */
	bond_release_all(bond_dev);

	if ((bond->params.mode == BOND_MODE_TLB) ||
	    (bond->params.mode == BOND_MODE_ALB)) {
		/* Must be called only after all
		 * slaves have been released
		 */
		bond_alb_deinitialize(bond);
	}

	return 0;
}

static struct net_device_stats *bond_get_stats(struct net_device *bond_dev)
{
	struct bonding *bond = bond_dev->priv;
	struct net_device_stats *stats = &(bond->stats), *sstats;
	struct slave *slave;
	int i;

	memset(stats, 0, sizeof(struct net_device_stats));

	read_lock_bh(&bond->lock);

	bond_for_each_slave(bond, slave, i) {
		sstats = slave->dev->get_stats(slave->dev);

		stats->rx_packets += sstats->rx_packets;
		stats->rx_bytes += sstats->rx_bytes;
		stats->rx_errors += sstats->rx_errors;
		stats->rx_dropped += sstats->rx_dropped;

		stats->tx_packets += sstats->tx_packets;
		stats->tx_bytes += sstats->tx_bytes;
		stats->tx_errors += sstats->tx_errors;
		stats->tx_dropped += sstats->tx_dropped;

		stats->multicast += sstats->multicast;
		stats->collisions += sstats->collisions;

		stats->rx_length_errors += sstats->rx_length_errors;
		stats->rx_over_errors += sstats->rx_over_errors;
		stats->rx_crc_errors += sstats->rx_crc_errors;
		stats->rx_frame_errors += sstats->rx_frame_errors;
		stats->rx_fifo_errors += sstats->rx_fifo_errors;
		stats->rx_missed_errors += sstats->rx_missed_errors;

		stats->tx_aborted_errors += sstats->tx_aborted_errors;
		stats->tx_carrier_errors += sstats->tx_carrier_errors;
		stats->tx_fifo_errors += sstats->tx_fifo_errors;
		stats->tx_heartbeat_errors += sstats->tx_heartbeat_errors;
		stats->tx_window_errors += sstats->tx_window_errors;
	}

	read_unlock_bh(&bond->lock);

	return stats;
}

static int bond_do_ioctl(struct net_device *bond_dev, struct ifreq *ifr, int cmd)
{
	struct net_device *slave_dev = NULL;
	struct ifbond k_binfo;
	struct ifbond __user *u_binfo = NULL;
	struct ifslave k_sinfo;
	struct ifslave __user *u_sinfo = NULL;
	struct mii_ioctl_data *mii = NULL;
	int prev_abi_ver = orig_app_abi_ver;
	int res = 0;

	dprintk("bond_ioctl: master=%s, cmd=%d\n",
		bond_dev->name, cmd);

	switch (cmd) {
	case SIOCETHTOOL:
		return bond_ethtool_ioctl(bond_dev, ifr);
	case SIOCGMIIPHY:
		mii = if_mii(ifr);
		if (!mii) {
			return -EINVAL;
		}
		mii->phy_id = 0;
		/* Fall Through */
	case SIOCGMIIREG:
		/*
		 * We do this again just in case we were called by SIOCGMIIREG
		 * instead of SIOCGMIIPHY.
		 */
		mii = if_mii(ifr);
		if (!mii) {
			return -EINVAL;
		}

		if (mii->reg_num == 1) {
			struct bonding *bond = bond_dev->priv;
			mii->val_out = 0;
			read_lock_bh(&bond->lock);
			read_lock(&bond->curr_slave_lock);
			if (bond->curr_active_slave) {
				mii->val_out = BMSR_LSTATUS;
			}
			read_unlock(&bond->curr_slave_lock);
			read_unlock_bh(&bond->lock);
		}

		return 0;
	case BOND_INFO_QUERY_OLD:
	case SIOCBONDINFOQUERY:
		u_binfo = (struct ifbond __user *)ifr->ifr_data;

		if (copy_from_user(&k_binfo, u_binfo, sizeof(ifbond))) {
			return -EFAULT;
		}

		res = bond_info_query(bond_dev, &k_binfo);
		if (res == 0) {
			if (copy_to_user(u_binfo, &k_binfo, sizeof(ifbond))) {
				return -EFAULT;
			}
		}

		return res;
	case BOND_SLAVE_INFO_QUERY_OLD:
	case SIOCBONDSLAVEINFOQUERY:
		u_sinfo = (struct ifslave __user *)ifr->ifr_data;

		if (copy_from_user(&k_sinfo, u_sinfo, sizeof(ifslave))) {
			return -EFAULT;
		}

		res = bond_slave_info_query(bond_dev, &k_sinfo);
		if (res == 0) {
			if (copy_to_user(u_sinfo, &k_sinfo, sizeof(ifslave))) {
				return -EFAULT;
			}
		}

		return res;
	default:
		/* Go on */
		break;
	}

	if (!capable(CAP_NET_ADMIN)) {
		return -EPERM;
	}

	if (orig_app_abi_ver == -1) {
		/* no orig_app_abi_ver was provided yet, so we'll use the
		 * current one from now on, even if it's 0
		 */
		orig_app_abi_ver = app_abi_ver;

	} else if (orig_app_abi_ver != app_abi_ver) {
		printk(KERN_ERR DRV_NAME
		       ": Error: already using ifenslave ABI version %d; to "
		       "upgrade ifenslave to version %d, you must first "
		       "reload bonding.\n",
		       orig_app_abi_ver, app_abi_ver);
		return -EINVAL;
	}

	slave_dev = dev_get_by_name(ifr->ifr_slave);

	dprintk("slave_dev=%p: \n", slave_dev);

	if (!slave_dev) {
		res = -ENODEV;
	} else {
		dprintk("slave_dev->name=%s: \n", slave_dev->name);
		switch (cmd) {
		case BOND_ENSLAVE_OLD:
		case SIOCBONDENSLAVE:
			res = bond_enslave(bond_dev, slave_dev);
			break;
		case BOND_RELEASE_OLD:
		case SIOCBONDRELEASE:
			res = bond_release(bond_dev, slave_dev);
			break;
		case BOND_SETHWADDR_OLD:
		case SIOCBONDSETHWADDR:
			res = bond_sethwaddr(bond_dev, slave_dev);
			break;
		case BOND_CHANGE_ACTIVE_OLD:
		case SIOCBONDCHANGEACTIVE:
			res = bond_ioctl_change_active(bond_dev, slave_dev);
			break;
		default:
			res = -EOPNOTSUPP;
		}

		dev_put(slave_dev);
	}

	if (res < 0) {
		/* The ioctl failed, so there's no point in changing the
		 * orig_app_abi_ver. We'll restore it's value just in case
		 * we've changed it earlier in this function.
		 */
		orig_app_abi_ver = prev_abi_ver;
	}

	return res;
}

static void bond_set_multicast_list(struct net_device *bond_dev)
{
	struct bonding *bond = bond_dev->priv;
	struct dev_mc_list *dmi;

	write_lock_bh(&bond->lock);

	/*
	 * Do promisc before checking multicast_mode
	 */
	if ((bond_dev->flags & IFF_PROMISC) && !(bond->flags & IFF_PROMISC)) {
		bond_set_promiscuity(bond, 1);
	}

	if (!(bond_dev->flags & IFF_PROMISC) && (bond->flags & IFF_PROMISC)) {
		bond_set_promiscuity(bond, -1);
	}

	/* set allmulti flag to slaves */
	if ((bond_dev->flags & IFF_ALLMULTI) && !(bond->flags & IFF_ALLMULTI)) {
		bond_set_allmulti(bond, 1);
	}

	if (!(bond_dev->flags & IFF_ALLMULTI) && (bond->flags & IFF_ALLMULTI)) {
		bond_set_allmulti(bond, -1);
	}

	bond->flags = bond_dev->flags;

	/* looking for addresses to add to slaves' mc list */
	for (dmi = bond_dev->mc_list; dmi; dmi = dmi->next) {
		if (!bond_mc_list_find_dmi(dmi, bond->mc_list)) {
			bond_mc_add(bond, dmi->dmi_addr, dmi->dmi_addrlen);
		}
	}

	/* looking for addresses to delete from slaves' list */
	for (dmi = bond->mc_list; dmi; dmi = dmi->next) {
		if (!bond_mc_list_find_dmi(dmi, bond_dev->mc_list)) {
			bond_mc_delete(bond, dmi->dmi_addr, dmi->dmi_addrlen);
		}
	}

	/* save master's multicast list */
	bond_mc_list_destroy(bond);
	bond_mc_list_copy(bond_dev->mc_list, bond, GFP_ATOMIC);

	write_unlock_bh(&bond->lock);
}

/*
 * Change the MTU of all of a master's slaves to match the master
 */
static int bond_change_mtu(struct net_device *bond_dev, int new_mtu)
{
	struct bonding *bond = bond_dev->priv;
	struct slave *slave, *stop_at;
	int res = 0;
	int i;

	dprintk("bond=%p, name=%s, new_mtu=%d\n", bond,
		(bond_dev ? bond_dev->name : "None"), new_mtu);

	/* Can't hold bond->lock with bh disabled here since
	 * some base drivers panic. On the other hand we can't
	 * hold bond->lock without bh disabled because we'll
	 * deadlock. The only solution is to rely on the fact
	 * that we're under rtnl_lock here, and the slaves
	 * list won't change. This doesn't solve the problem
	 * of setting the slave's MTU while it is
	 * transmitting, but the assumption is that the base
	 * driver can handle that.
	 *
	 * TODO: figure out a way to safely iterate the slaves
	 * list, but without holding a lock around the actual
	 * call to the base driver.
	 */

	bond_for_each_slave(bond, slave, i) {
		dprintk("s %p s->p %p c_m %p\n", slave,
			slave->prev, slave->dev->change_mtu);
		if (slave->dev->change_mtu) {
			res = slave->dev->change_mtu(slave->dev, new_mtu);
		} else {
			slave->dev->mtu = new_mtu;
			res = 0;
		}

		if (res) {
			/* If we failed to set the slave's mtu to the new value
			 * we must abort the operation even in ACTIVE_BACKUP
			 * mode, because if we allow the backup slaves to have
			 * different mtu values than the active slave we'll
			 * need to change their mtu when doing a failover. That
			 * means changing their mtu from timer context, which
			 * is probably not a good idea.
			 */
			dprintk("err %d %s\n", res, slave->dev->name);
			goto unwind;
		}
	}

	bond_dev->mtu = new_mtu;

	return 0;

unwind:
	/* unwind from head to the slave that failed */
	stop_at = slave;
	bond_for_each_slave_from_to(bond, slave, i, bond->first_slave, stop_at) {
		int tmp_res;

		if (slave->dev->change_mtu) {
			tmp_res = slave->dev->change_mtu(slave->dev, bond_dev->mtu);
			if (tmp_res) {
				dprintk("unwind err %d dev %s\n", tmp_res,
					slave->dev->name);
			}
		} else {
			slave->dev->mtu = bond_dev->mtu;
		}
	}

	return res;
}

/*
 * Change HW address
 *
 * Note that many devices must be down to change the HW address, and
 * downing the master releases all slaves.  We can make bonds full of
 * bonding devices to test this, however.
 */
static int bond_set_mac_address(struct net_device *bond_dev, void *addr)
{
	struct bonding *bond = bond_dev->priv;
	struct sockaddr *sa = addr, tmp_sa;
	struct slave *slave, *stop_at;
	int res = 0;
	int i;

	dprintk("bond=%p, name=%s\n", bond, (bond_dev ? bond_dev->name : "None"));

	if (!is_valid_ether_addr(sa->sa_data)) {
		return -EADDRNOTAVAIL;
	}

	/* Can't hold bond->lock with bh disabled here since
	 * some base drivers panic. On the other hand we can't
	 * hold bond->lock without bh disabled because we'll
	 * deadlock. The only solution is to rely on the fact
	 * that we're under rtnl_lock here, and the slaves
	 * list won't change. This doesn't solve the problem
	 * of setting the slave's hw address while it is
	 * transmitting, but the assumption is that the base
	 * driver can handle that.
	 *
	 * TODO: figure out a way to safely iterate the slaves
	 * list, but without holding a lock around the actual
	 * call to the base driver.
	 */

	bond_for_each_slave(bond, slave, i) {
		dprintk("slave %p %s\n", slave, slave->dev->name);

		if (slave->dev->set_mac_address == NULL) {
			res = -EOPNOTSUPP;
			dprintk("EOPNOTSUPP %s\n", slave->dev->name);
			goto unwind;
		}

		res = slave->dev->set_mac_address(slave->dev, addr);
		if (res) {
			/* TODO: consider downing the slave
			 * and retry ?
			 * User should expect communications
			 * breakage anyway until ARP finish
			 * updating, so...
			 */
			dprintk("err %d %s\n", res, slave->dev->name);
			goto unwind;
		}
	}

	/* success */
	memcpy(bond_dev->dev_addr, sa->sa_data, bond_dev->addr_len);
	return 0;

unwind:
	memcpy(tmp_sa.sa_data, bond_dev->dev_addr, bond_dev->addr_len);
	tmp_sa.sa_family = bond_dev->type;

	/* unwind from head to the slave that failed */
	stop_at = slave;
	bond_for_each_slave_from_to(bond, slave, i, bond->first_slave, stop_at) {
		int tmp_res;

		tmp_res = slave->dev->set_mac_address(slave->dev, &tmp_sa);
		if (tmp_res) {
			dprintk("unwind err %d dev %s\n", tmp_res,
				slave->dev->name);
		}
	}

	return res;
}

static int bond_xmit_roundrobin(struct sk_buff *skb, struct net_device *bond_dev)
{
	struct bonding *bond = bond_dev->priv;
	struct slave *slave, *start_at;
	int i;
	int res = 1;

	read_lock(&bond->lock);

	if (!BOND_IS_OK(bond)) {
		goto out;
	}

	read_lock(&bond->curr_slave_lock);
	slave = start_at = bond->curr_active_slave;
	read_unlock(&bond->curr_slave_lock);

	if (!slave) {
		goto out;
	}

	bond_for_each_slave_from(bond, slave, i, start_at) {
		if (IS_UP(slave->dev) &&
		    (slave->link == BOND_LINK_UP) &&
		    (slave->state == BOND_STATE_ACTIVE)) {
			res = bond_dev_queue_xmit(bond, skb, slave->dev);

			write_lock(&bond->curr_slave_lock);
			bond->curr_active_slave = slave->next;
			write_unlock(&bond->curr_slave_lock);

			break;
		}
	}


out:
	if (res) {
		/* no suitable interface, frame not sent */
		dev_kfree_skb(skb);
	}
	read_unlock(&bond->lock);
	return 0;
}

/*
 * in active-backup mode, we know that bond->curr_active_slave is always valid if
 * the bond has a usable interface.
 */
static int bond_xmit_activebackup(struct sk_buff *skb, struct net_device *bond_dev)
{
	struct bonding *bond = bond_dev->priv;
	int res = 1;

	/* if we are sending arp packets, try to at least
	   identify our own ip address */
	if (bond->params.arp_interval && !my_ip &&
		(skb->protocol == __constant_htons(ETH_P_ARP))) {
		char *the_ip = (char *)skb->data +
				sizeof(struct ethhdr) +
				sizeof(struct arphdr) +
				ETH_ALEN;
		memcpy(&my_ip, the_ip, 4);
	}

	read_lock(&bond->lock);
	read_lock(&bond->curr_slave_lock);

	if (!BOND_IS_OK(bond)) {
		goto out;
	}

	if (bond->curr_active_slave) { /* one usable interface */
		res = bond_dev_queue_xmit(bond, skb, bond->curr_active_slave->dev);
	}

out:
	if (res) {
		/* no suitable interface, frame not sent */
		dev_kfree_skb(skb);
	}
	read_unlock(&bond->curr_slave_lock);
	read_unlock(&bond->lock);
	return 0;
}

/*
 * in XOR mode, we determine the output device by performing xor on
 * the source and destination hw adresses.  If this device is not
 * enabled, find the next slave following this xor slave.
 */
static int bond_xmit_xor(struct sk_buff *skb, struct net_device *bond_dev)
{
	struct bonding *bond = bond_dev->priv;
	struct ethhdr *data = (struct ethhdr *)skb->data;
	struct slave *slave, *start_at;
	int slave_no;
	int i;
	int res = 1;

	read_lock(&bond->lock);

	if (!BOND_IS_OK(bond)) {
		goto out;
	}

	slave_no = (data->h_dest[5]^bond_dev->dev_addr[5]) % bond->slave_cnt;

	bond_for_each_slave(bond, slave, i) {
		slave_no--;
		if (slave_no < 0) {
			break;
		}
	}

	start_at = slave;

	bond_for_each_slave_from(bond, slave, i, start_at) {
		if (IS_UP(slave->dev) &&
		    (slave->link == BOND_LINK_UP) &&
		    (slave->state == BOND_STATE_ACTIVE)) {
			res = bond_dev_queue_xmit(bond, skb, slave->dev);
			break;
		}
	}

out:
	if (res) {
		/* no suitable interface, frame not sent */
		dev_kfree_skb(skb);
	}
	read_unlock(&bond->lock);
	return 0;
}

/*
 * in broadcast mode, we send everything to all usable interfaces.
 */
static int bond_xmit_broadcast(struct sk_buff *skb, struct net_device *bond_dev)
{
	struct bonding *bond = bond_dev->priv;
	struct slave *slave, *start_at;
	struct net_device *tx_dev = NULL;
	int i;
	int res = 1;

	read_lock(&bond->lock);

	if (!BOND_IS_OK(bond)) {
		goto out;
	}

	read_lock(&bond->curr_slave_lock);
	start_at = bond->curr_active_slave;
	read_unlock(&bond->curr_slave_lock);

	if (!start_at) {
		goto out;
	}

	bond_for_each_slave_from(bond, slave, i, start_at) {
		if (IS_UP(slave->dev) &&
		    (slave->link == BOND_LINK_UP) &&
		    (slave->state == BOND_STATE_ACTIVE)) {
			if (tx_dev) {
				struct sk_buff *skb2 = skb_clone(skb, GFP_ATOMIC);
				if (!skb2) {
					printk(KERN_ERR DRV_NAME
					       ": Error: bond_xmit_broadcast(): "
					       "skb_clone() failed\n");
					continue;
				}

				res = bond_dev_queue_xmit(bond, skb2, tx_dev);
				if (res) {
					dev_kfree_skb(skb2);
					continue;
				}
			}
			tx_dev = slave->dev;
		}
	}

	if (tx_dev) {
		res = bond_dev_queue_xmit(bond, skb, tx_dev);
	}

out:
	if (res) {
		/* no suitable interface, frame not sent */
		dev_kfree_skb(skb);
	}
	/* frame sent to all suitable interfaces */
	read_unlock(&bond->lock);
	return 0;
}

/*------------------------- Device initialization ---------------------------*/

/*
 * set bond mode specific net device operations
 */
static inline void bond_set_mode_ops(struct net_device *bond_dev, int mode)
{
	switch (mode) {
	case BOND_MODE_ROUNDROBIN:
		bond_dev->hard_start_xmit = bond_xmit_roundrobin;
		break;
	case BOND_MODE_ACTIVEBACKUP:
		bond_dev->hard_start_xmit = bond_xmit_activebackup;
		break;
	case BOND_MODE_XOR:
		bond_dev->hard_start_xmit = bond_xmit_xor;
		break;
	case BOND_MODE_BROADCAST:
		bond_dev->hard_start_xmit = bond_xmit_broadcast;
		break;
	case BOND_MODE_8023AD:
		bond_dev->hard_start_xmit = bond_3ad_xmit_xor;
		break;
	case BOND_MODE_TLB:
	case BOND_MODE_ALB:
		bond_dev->hard_start_xmit = bond_alb_xmit;
		bond_dev->set_mac_address = bond_alb_set_mac_address;
		break;
	default:
		/* Should never happen, mode already checked */
		printk(KERN_ERR DRV_NAME
		       ": Error: Unknown bonding mode %d\n",
		       mode);
		break;
	}
}

/*
 * Does not allocate but creates a /proc entry.
 * Allowed to fail.
 */
static int __init bond_init(struct net_device *bond_dev, struct bond_params *params)
{
	struct bonding *bond = bond_dev->priv;

	dprintk("Begin bond_init for %s\n", bond_dev->name);

	/* initialize rwlocks */
	rwlock_init(&bond->lock);
	rwlock_init(&bond->curr_slave_lock);

	bond->params = *params; /* copy params struct */

	/* Initialize pointers */
	bond->first_slave = NULL;
	bond->curr_active_slave = NULL;
	bond->current_arp_slave = NULL;
	bond->primary_slave = NULL;
	bond->dev = bond_dev;
	INIT_LIST_HEAD(&bond->vlan_list);

	/* Initialize the device entry points */
	bond_dev->open = bond_open;
	bond_dev->stop = bond_close;
	bond_dev->get_stats = bond_get_stats;
	bond_dev->do_ioctl = bond_do_ioctl;
	bond_dev->set_multicast_list = bond_set_multicast_list;
	bond_dev->change_mtu = bond_change_mtu;
	bond_dev->set_mac_address = bond_set_mac_address;

	bond_set_mode_ops(bond_dev, bond->params.mode);

	bond_dev->destructor = free_netdev;

	/* Initialize the device options */
	bond_dev->tx_queue_len = 0;
	bond_dev->flags |= IFF_MASTER|IFF_MULTICAST;

	/* At first, we block adding VLANs. That's the only way to
	 * prevent problems that occur when adding VLANs over an
	 * empty bond. The block will be removed once non-challenged
	 * slaves are enslaved.
	 */
	bond_dev->features |= NETIF_F_VLAN_CHALLENGED;

	/* By default, we declare the bond to be fully
	 * VLAN hardware accelerated capable. Special
	 * care is taken in the various xmit functions
	 * when there are slaves that are not hw accel
	 * capable
	 */
	bond_dev->vlan_rx_register = bond_vlan_rx_register;
	bond_dev->vlan_rx_add_vid  = bond_vlan_rx_add_vid;
	bond_dev->vlan_rx_kill_vid = bond_vlan_rx_kill_vid;
	bond_dev->features |= (NETIF_F_HW_VLAN_TX |
			       NETIF_F_HW_VLAN_RX |
			       NETIF_F_HW_VLAN_FILTER);

#ifdef CONFIG_PROC_FS
	bond_create_proc_entry(bond);
#endif

	list_add_tail(&bond->bond_list, &bond_dev_list);

	return 0;
}

/* De-initialize device specific data.
 * Caller must hold rtnl_lock.
 */
static inline void bond_deinit(struct net_device *bond_dev)
{
	struct bonding *bond = bond_dev->priv;

	list_del(&bond->bond_list);

#ifdef CONFIG_PROC_FS
	bond_remove_proc_entry(bond);
#endif
}

/* Unregister and free all bond devices.
 * Caller must hold rtnl_lock.
 */
static void bond_free_all(void)
{
	struct bonding *bond, *nxt;

	list_for_each_entry_safe(bond, nxt, &bond_dev_list, bond_list) {
		struct net_device *bond_dev = bond->dev;

		unregister_netdevice(bond_dev);
		bond_deinit(bond_dev);
	}

#ifdef CONFIG_PROC_FS
	bond_destroy_proc_dir();
#endif
}

/*------------------------- Module initialization ---------------------------*/

/*
 * Convert string input module parms.  Accept either the
 * number of the mode or its string name.
 */
static inline int bond_parse_parm(char *mode_arg, struct bond_parm_tbl *tbl)
{
	int i;

	for (i = 0; tbl[i].modename; i++) {
		if ((isdigit(*mode_arg) &&
		     tbl[i].mode == simple_strtol(mode_arg, NULL, 0)) ||
		    (strncmp(mode_arg, tbl[i].modename,
			     strlen(tbl[i].modename)) == 0)) {
			return tbl[i].mode;
		}
	}

	return -1;
}

static int bond_check_params(struct bond_params *params)
{
	/*
	 * Convert string parameters.
	 */
	if (mode) {
		bond_mode = bond_parse_parm(mode, bond_mode_tbl);
		if (bond_mode == -1) {
			printk(KERN_ERR DRV_NAME
			       ": Error: Invalid bonding mode \"%s\"\n",
			       mode == NULL ? "NULL" : mode);
			return -EINVAL;
		}
	}

	if (lacp_rate) {
		if (bond_mode != BOND_MODE_8023AD) {
			printk(KERN_INFO DRV_NAME
			       ": lacp_rate param is irrelevant in mode %s\n",
			       bond_mode_name(bond_mode));
		} else {
			lacp_fast = bond_parse_parm(lacp_rate, bond_lacp_tbl);
			if (lacp_fast == -1) {
				printk(KERN_ERR DRV_NAME
				       ": Error: Invalid lacp rate \"%s\"\n",
				       lacp_rate == NULL ? "NULL" : lacp_rate);
				return -EINVAL;
			}
		}
	}

	if (max_bonds < 1 || max_bonds > INT_MAX) {
		printk(KERN_WARNING DRV_NAME
		       ": Warning: max_bonds (%d) not in range %d-%d, so it "
		       "was reset to BOND_DEFAULT_MAX_BONDS (%d)",
		       max_bonds, 1, INT_MAX, BOND_DEFAULT_MAX_BONDS);
		max_bonds = BOND_DEFAULT_MAX_BONDS;
	}

	if (miimon < 0) {
		printk(KERN_WARNING DRV_NAME
		       ": Warning: miimon module parameter (%d), "
		       "not in range 0-%d, so it was reset to %d\n",
		       miimon, INT_MAX, BOND_LINK_MON_INTERV);
		miimon = BOND_LINK_MON_INTERV;
	}

	if (updelay < 0) {
		printk(KERN_WARNING DRV_NAME
		       ": Warning: updelay module parameter (%d), "
		       "not in range 0-%d, so it was reset to 0\n",
		       updelay, INT_MAX);
		updelay = 0;
	}

	if (downdelay < 0) {
		printk(KERN_WARNING DRV_NAME
		       ": Warning: downdelay module parameter (%d), "
		       "not in range 0-%d, so it was reset to 0\n",
		       downdelay, INT_MAX);
		downdelay = 0;
	}

	if ((use_carrier != 0) && (use_carrier != 1)) {
		printk(KERN_WARNING DRV_NAME
		       ": Warning: use_carrier module parameter (%d), "
		       "not of valid value (0/1), so it was set to 1\n",
		       use_carrier);
		use_carrier = 1;
	}

	/* reset values for 802.3ad */
	if (bond_mode == BOND_MODE_8023AD) {
		if (!miimon) {
			printk(KERN_WARNING DRV_NAME
			       ": Warning: miimon must be specified, "
			       "otherwise bonding will not detect link "
			       "failure, speed and duplex which are "
			       "essential for 802.3ad operation\n");
			printk(KERN_WARNING "Forcing miimon to 100msec\n");
			miimon = 100;
		}
	}

	/* reset values for TLB/ALB */
	if ((bond_mode == BOND_MODE_TLB) ||
	    (bond_mode == BOND_MODE_ALB)) {
		if (!miimon) {
			printk(KERN_WARNING DRV_NAME
			       ": Warning: miimon must be specified, "
			       "otherwise bonding will not detect link "
			       "failure and link speed which are essential "
			       "for TLB/ALB load balancing\n");
			printk(KERN_WARNING "Forcing miimon to 100msec\n");
			miimon = 100;
		}
	}

	if (bond_mode == BOND_MODE_ALB) {
		printk(KERN_NOTICE DRV_NAME
		       ": In ALB mode you might experience client "
		       "disconnections upon reconnection of a link if the "
		       "bonding module updelay parameter (%d msec) is "
		       "incompatible with the forwarding delay time of the "
		       "switch\n",
		       updelay);
	}

	if (!miimon) {
		if (updelay || downdelay) {
			/* just warn the user the up/down delay will have
			 * no effect since miimon is zero...
			 */
			printk(KERN_WARNING DRV_NAME
			       ": Warning: miimon module parameter not set "
			       "and updelay (%d) or downdelay (%d) module "
			       "parameter is set; updelay and downdelay have "
			       "no effect unless miimon is set\n",
			       updelay, downdelay);
		}
	} else {
		/* don't allow arp monitoring */
		if (arp_interval) {
			printk(KERN_WARNING DRV_NAME
			       ": Warning: miimon (%d) and arp_interval (%d) "
			       "can't be used simultaneously, disabling ARP "
			       "monitoring\n",
			       miimon, arp_interval);
			arp_interval = 0;
		}

		if ((updelay % miimon) != 0) {
			printk(KERN_WARNING DRV_NAME
			       ": Warning: updelay (%d) is not a multiple "
			       "of miimon (%d), updelay rounded to %d ms\n",
			       updelay, miimon, (updelay / miimon) * miimon);
		}

		updelay /= miimon;

		if ((downdelay % miimon) != 0) {
			printk(KERN_WARNING DRV_NAME
			       ": Warning: downdelay (%d) is not a multiple "
			       "of miimon (%d), downdelay rounded to %d ms\n",
			       downdelay, miimon,
			       (downdelay / miimon) * miimon);
		}

		downdelay /= miimon;
	}

	if (arp_interval < 0) {
		printk(KERN_WARNING DRV_NAME
		       ": Warning: arp_interval module parameter (%d) "
		       ", not in range 0-%d, so it was reset to %d\n",
		       arp_interval, INT_MAX, BOND_LINK_ARP_INTERV);
		arp_interval = BOND_LINK_ARP_INTERV;
	}

	for (arp_ip_count = 0;
	     (arp_ip_count < BOND_MAX_ARP_TARGETS) && arp_ip_target[arp_ip_count];
	     arp_ip_count++) {
		/* not complete check, but should be good enough to
		   catch mistakes */
		if (!isdigit(arp_ip_target[arp_ip_count][0])) {
			printk(KERN_WARNING DRV_NAME
			       ": Warning: bad arp_ip_target module parameter "
			       "(%s), ARP monitoring will not be performed\n",
			       arp_ip_target[arp_ip_count]);
			arp_interval = 0;
		} else {
			u32 ip = in_aton(arp_ip_target[arp_ip_count]);
			arp_target[arp_ip_count] = ip;
		}
	}

	if (arp_interval && !arp_ip_count) {
		/* don't allow arping if no arp_ip_target given... */
		printk(KERN_WARNING DRV_NAME
		       ": Warning: arp_interval module parameter (%d) "
		       "specified without providing an arp_ip_target "
		       "parameter, arp_interval was reset to 0\n",
		       arp_interval);
		arp_interval = 0;
	}

	if (miimon) {
		printk(KERN_INFO DRV_NAME
		       ": MII link monitoring set to %d ms\n",
		       miimon);
	} else if (arp_interval) {
		int i;

		printk(KERN_INFO DRV_NAME
		       ": ARP monitoring set to %d ms with %d target(s):",
		       arp_interval, arp_ip_count);

		for (i = 0; i < arp_ip_count; i++)
			printk (" %s", arp_ip_target[i]);

		printk("\n");

	} else {
		/* miimon and arp_interval not set, we need one so things
		 * work as expected, see bonding.txt for details
		 */
		printk(KERN_WARNING DRV_NAME
		       ": Warning: either miimon or arp_interval and "
		       "arp_ip_target module parameters must be specified, "
		       "otherwise bonding will not detect link failures! see "
		       "bonding.txt for details.\n");
	}

	if (primary && !USES_PRIMARY(bond_mode)) {
		/* currently, using a primary only makes sense
		 * in active backup, TLB or ALB modes
		 */
		printk(KERN_WARNING DRV_NAME
		       ": Warning: %s primary device specified but has no "
		       "effect in %s mode\n",
		       primary, bond_mode_name(bond_mode));
		primary = NULL;
	}

	/* fill params struct with the proper values */
	params->mode = bond_mode;
	params->miimon = miimon;
	params->arp_interval = arp_interval;
	params->updelay = updelay;
	params->downdelay = downdelay;
	params->use_carrier = use_carrier;
	params->lacp_fast = lacp_fast;
	params->primary[0] = 0;

	if (primary) {
		strncpy(params->primary, primary, IFNAMSIZ);
		params->primary[IFNAMSIZ - 1] = 0;
	}

	memcpy(params->arp_targets, arp_target, sizeof(arp_target));

	return 0;
}

static int __init bonding_init(void)
{
	struct bond_params params;
	int i;
	int res;

	printk(KERN_INFO "%s", version);

	res = bond_check_params(&params);
	if (res) {
		return res;
	}

	rtnl_lock();

#ifdef CONFIG_PROC_FS
	bond_create_proc_dir();
#endif

	for (i = 0; i < max_bonds; i++) {
		struct net_device *bond_dev;

		bond_dev = alloc_netdev(sizeof(struct bonding), "", ether_setup);
		if (!bond_dev) {
			res = -ENOMEM;
			goto out_err;
		}

		res = dev_alloc_name(bond_dev, "bond%d");
		if (res < 0) {
			free_netdev(bond_dev);
			goto out_err;
		}

		/* bond_init() must be called after dev_alloc_name() (for the
		 * /proc files), but before register_netdevice(), because we
		 * need to set function pointers.
		 */
		res = bond_init(bond_dev, &params);
		if (res < 0) {
			free_netdev(bond_dev);
			goto out_err;
		}

		SET_MODULE_OWNER(bond_dev);

		res = register_netdevice(bond_dev);
		if (res < 0) {
			bond_deinit(bond_dev);
			free_netdev(bond_dev);
			goto out_err;
		}
	}

	rtnl_unlock();
	register_netdevice_notifier(&bond_netdev_notifier);

	return 0;

out_err:
	/* free and unregister all bonds that were successfully added */
	bond_free_all();

	rtnl_unlock();

	return res;
}

static void __exit bonding_exit(void)
{
	unregister_netdevice_notifier(&bond_netdev_notifier);

	rtnl_lock();
	bond_free_all();
	rtnl_unlock();
}

module_init(bonding_init);
module_exit(bonding_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(DRV_DESCRIPTION ", v" DRV_VERSION);
MODULE_AUTHOR("Thomas Davis, tadavis@lbl.gov and many others");
MODULE_SUPPORTED_DEVICE("most ethernet devices");

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */

