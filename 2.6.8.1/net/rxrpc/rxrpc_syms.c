/* rxrpc_syms.c: exported Rx RPC layer interface symbols
 *
 * Copyright (C) 2002 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/config.h>
#include <linux/module.h>

#include <rxrpc/transport.h>
#include <rxrpc/connection.h>
#include <rxrpc/call.h>
#include <rxrpc/krxiod.h>

/* call.c */
EXPORT_SYMBOL(rxrpc_call_rcv_timeout);
EXPORT_SYMBOL(rxrpc_call_acks_timeout);
EXPORT_SYMBOL(rxrpc_call_dfr_ack_timeout);
EXPORT_SYMBOL(rxrpc_call_max_resend);
EXPORT_SYMBOL(rxrpc_call_states);
EXPORT_SYMBOL(rxrpc_call_error_states);

EXPORT_SYMBOL(rxrpc_create_call);
EXPORT_SYMBOL(rxrpc_incoming_call);
EXPORT_SYMBOL(rxrpc_put_call);
EXPORT_SYMBOL(rxrpc_call_abort);
EXPORT_SYMBOL(rxrpc_call_read_data);
EXPORT_SYMBOL(rxrpc_call_write_data);
EXPORT_SYMBOL(rxrpc_call_flush);

/* connection.c */
EXPORT_SYMBOL(rxrpc_create_connection);
EXPORT_SYMBOL(rxrpc_put_connection);

/* sysctl.c */
EXPORT_SYMBOL(rxrpc_ktrace);
EXPORT_SYMBOL(rxrpc_kdebug);
EXPORT_SYMBOL(rxrpc_kproto);
EXPORT_SYMBOL(rxrpc_knet);

/* transport.c */
EXPORT_SYMBOL(rxrpc_create_transport);
EXPORT_SYMBOL(rxrpc_clear_transport);
EXPORT_SYMBOL(rxrpc_put_transport);
EXPORT_SYMBOL(rxrpc_add_service);
EXPORT_SYMBOL(rxrpc_del_service);
