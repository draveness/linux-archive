/*
 * linux/net/sunrpc/sunrpc_syms.c
 *
 * Symbols exported by the sunrpc module.
 *
 * Copyright (C) 1997 Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/config.h>
#include <linux/module.h>

#include <linux/types.h>
#include <linux/socket.h>
#include <linux/sched.h>
#include <linux/uio.h>
#include <linux/unistd.h>
#include <linux/init.h>

#include <linux/sunrpc/sched.h>
#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svcsock.h>
#include <linux/sunrpc/auth.h>
#include <linux/workqueue.h>
#include <linux/sunrpc/rpc_pipe_fs.h>


/* RPC scheduler */
EXPORT_SYMBOL(rpc_execute);
EXPORT_SYMBOL(rpc_init_task);
EXPORT_SYMBOL(rpc_sleep_on);
EXPORT_SYMBOL(rpc_wake_up_next);
EXPORT_SYMBOL(rpc_wake_up_task);
EXPORT_SYMBOL(rpc_new_child);
EXPORT_SYMBOL(rpc_run_child);
EXPORT_SYMBOL(rpciod_down);
EXPORT_SYMBOL(rpciod_up);
EXPORT_SYMBOL(rpc_new_task);
EXPORT_SYMBOL(rpc_wake_up_status);
EXPORT_SYMBOL(rpc_release_task);

/* RPC client functions */
EXPORT_SYMBOL(rpc_create_client);
EXPORT_SYMBOL(rpc_clone_client);
EXPORT_SYMBOL(rpc_destroy_client);
EXPORT_SYMBOL(rpc_shutdown_client);
EXPORT_SYMBOL(rpc_release_client);
EXPORT_SYMBOL(rpc_killall_tasks);
EXPORT_SYMBOL(rpc_call_sync);
EXPORT_SYMBOL(rpc_call_async);
EXPORT_SYMBOL(rpc_call_setup);
EXPORT_SYMBOL(rpc_clnt_sigmask);
EXPORT_SYMBOL(rpc_clnt_sigunmask);
EXPORT_SYMBOL(rpc_delay);
EXPORT_SYMBOL(rpc_restart_call);
EXPORT_SYMBOL(rpc_setbufsize);
EXPORT_SYMBOL(rpc_unlink);
EXPORT_SYMBOL(rpc_wake_up);
EXPORT_SYMBOL(rpc_queue_upcall);
EXPORT_SYMBOL(rpc_mkpipe);

/* Client transport */
EXPORT_SYMBOL(xprt_create_proto);
EXPORT_SYMBOL(xprt_destroy);
EXPORT_SYMBOL(xprt_set_timeout);
EXPORT_SYMBOL(xprt_udp_slot_table_entries);
EXPORT_SYMBOL(xprt_tcp_slot_table_entries);

/* Client credential cache */
EXPORT_SYMBOL(rpcauth_register);
EXPORT_SYMBOL(rpcauth_unregister);
EXPORT_SYMBOL(rpcauth_create);
EXPORT_SYMBOL(rpcauth_lookupcred);
EXPORT_SYMBOL(rpcauth_lookup_credcache);
EXPORT_SYMBOL(rpcauth_free_credcache);
EXPORT_SYMBOL(rpcauth_init_credcache);
EXPORT_SYMBOL(put_rpccred);

/* RPC server stuff */
EXPORT_SYMBOL(svc_create);
EXPORT_SYMBOL(svc_create_thread);
EXPORT_SYMBOL(svc_exit_thread);
EXPORT_SYMBOL(svc_destroy);
EXPORT_SYMBOL(svc_drop);
EXPORT_SYMBOL(svc_process);
EXPORT_SYMBOL(svc_recv);
EXPORT_SYMBOL(svc_wake_up);
EXPORT_SYMBOL(svc_makesock);
EXPORT_SYMBOL(svc_reserve);
EXPORT_SYMBOL(svc_auth_register);
EXPORT_SYMBOL(auth_domain_lookup);

/* RPC statistics */
#ifdef CONFIG_PROC_FS
EXPORT_SYMBOL(rpc_proc_register);
EXPORT_SYMBOL(rpc_proc_unregister);
EXPORT_SYMBOL(svc_proc_register);
EXPORT_SYMBOL(svc_proc_unregister);
EXPORT_SYMBOL(svc_seq_show);
#endif

/* caching... */
EXPORT_SYMBOL(auth_domain_find);
EXPORT_SYMBOL(auth_domain_put);
EXPORT_SYMBOL(auth_unix_add_addr);
EXPORT_SYMBOL(auth_unix_forget_old);
EXPORT_SYMBOL(auth_unix_lookup);
EXPORT_SYMBOL(cache_check);
EXPORT_SYMBOL(cache_clean);
EXPORT_SYMBOL(cache_flush);
EXPORT_SYMBOL(cache_purge);
EXPORT_SYMBOL(cache_fresh);
EXPORT_SYMBOL(cache_init);
EXPORT_SYMBOL(cache_register);
EXPORT_SYMBOL(cache_unregister);
EXPORT_SYMBOL(qword_add);
EXPORT_SYMBOL(qword_addhex);
EXPORT_SYMBOL(qword_get);
EXPORT_SYMBOL(svcauth_unix_purge);
EXPORT_SYMBOL(unix_domain_find);

/* Generic XDR */
EXPORT_SYMBOL(xdr_encode_string);
EXPORT_SYMBOL(xdr_decode_string);
EXPORT_SYMBOL(xdr_decode_string_inplace);
EXPORT_SYMBOL(xdr_decode_netobj);
EXPORT_SYMBOL(xdr_encode_netobj);
EXPORT_SYMBOL(xdr_encode_pages);
EXPORT_SYMBOL(xdr_inline_pages);
EXPORT_SYMBOL(xdr_shift_buf);
EXPORT_SYMBOL(xdr_buf_from_iov);
EXPORT_SYMBOL(xdr_buf_subsegment);
EXPORT_SYMBOL(xdr_buf_read_netobj);
EXPORT_SYMBOL(read_bytes_from_xdr_buf);

/* Debugging symbols */
#ifdef RPC_DEBUG
EXPORT_SYMBOL(rpc_debug);
EXPORT_SYMBOL(nfs_debug);
EXPORT_SYMBOL(nfsd_debug);
EXPORT_SYMBOL(nlm_debug);
#endif

extern int register_rpc_pipefs(void);
extern void unregister_rpc_pipefs(void);

static int __init
init_sunrpc(void)
{
	int err = register_rpc_pipefs();
	if (err)
		goto out;
	err = rpc_init_mempool() != 0;
	if (err)
		goto out;
#ifdef RPC_DEBUG
	rpc_register_sysctl();
#endif
#ifdef CONFIG_PROC_FS
	rpc_proc_init();
#endif
	cache_register(&auth_domain_cache);
	cache_register(&ip_map_cache);
out:
	return err;
}

static void __exit
cleanup_sunrpc(void)
{
	unregister_rpc_pipefs();
	rpc_destroy_mempool();
	cache_unregister(&auth_domain_cache);
	cache_unregister(&ip_map_cache);
#ifdef RPC_DEBUG
	rpc_unregister_sysctl();
#endif
#ifdef CONFIG_PROC_FS
	rpc_proc_exit();
#endif
}
MODULE_LICENSE("GPL");
module_init(init_sunrpc);
module_exit(cleanup_sunrpc);
