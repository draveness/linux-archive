/*
 * ipmi_smi.h
 *
 * MontaVista IPMI system management interface
 *
 * Author: MontaVista Software, Inc.
 *         Corey Minyard <minyard@mvista.com>
 *         source@mvista.com
 *
 * Copyright 2002 MontaVista Software Inc.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or (at your
 *  option) any later version.
 *
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 *  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 *  TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *  USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef __LINUX_IPMI_SMI_H
#define __LINUX_IPMI_SMI_H

#include <linux/ipmi_msgdefs.h>
#include <linux/proc_fs.h>
#include <linux/module.h>

/* This files describes the interface for IPMI system management interface
   drivers to bind into the IPMI message handler. */

/* Structure for the low-level drivers. */
typedef struct ipmi_smi *ipmi_smi_t;

/*
 * Messages to/from the lower layer.  The smi interface will take one
 * of these to send. After the send has occurred and a response has
 * been received, it will report this same data structure back up to
 * the upper layer.  If an error occurs, it should fill in the
 * response with an error code in the completion code location. When
 * asynchronous data is received, one of these is allocated, the
 * data_size is set to zero and the response holds the data from the
 * get message or get event command that the interface initiated.
 * Note that it is the interfaces responsibility to detect
 * asynchronous data and messages and request them from the
 * interface.
 */
struct ipmi_smi_msg
{
	struct list_head link;

	long    msgid;
	void    *user_data;

	int           data_size;
	unsigned char data[IPMI_MAX_MSG_LENGTH];

	int           rsp_size;
	unsigned char rsp[IPMI_MAX_MSG_LENGTH];

	/* Will be called when the system is done with the message
           (presumably to free it). */
	void (*done)(struct ipmi_smi_msg *msg);
};

struct ipmi_smi_handlers
{
	struct module *owner;

	/* Called to enqueue an SMI message to be sent.  This
	   operation is not allowed to fail.  If an error occurs, it
	   should report back the error in a received message.  It may
	   do this in the current call context, since no write locks
	   are held when this is run.  If the priority is > 0, the
	   message will go into a high-priority queue and be sent
	   first.  Otherwise, it goes into a normal-priority queue. */
	void (*sender)(void                *send_info,
		       struct ipmi_smi_msg *msg,
		       int                 priority);

	/* Called by the upper layer to request that we try to get
	   events from the BMC we are attached to. */
	void (*request_events)(void *send_info);

	/* Called when the interface should go into "run to
	   completion" mode.  If this call sets the value to true, the
	   interface should make sure that all messages are flushed
	   out and that none are pending, and any new requests are run
	   to completion immediately. */
	void (*set_run_to_completion)(void *send_info, int run_to_completion);

	/* Called to poll for work to do.  This is so upper layers can
	   poll for operations during things like crash dumps. */
	void (*poll)(void *send_info);
};

/* Add a low-level interface to the IPMI driver. */
int ipmi_register_smi(struct ipmi_smi_handlers *handlers,
		      void                     *send_info,
		      unsigned char            version_major,
		      unsigned char            version_minor,
		      ipmi_smi_t               *intf);

/*
 * Remove a low-level interface from the IPMI driver.  This will
 * return an error if the interface is still in use by a user.
 */
int ipmi_unregister_smi(ipmi_smi_t intf);

/*
 * The lower layer reports received messages through this interface.
 * The data_size should be zero if this is an asyncronous message.  If
 * the lower layer gets an error sending a message, it should format
 * an error response in the message response.
 */
void ipmi_smi_msg_received(ipmi_smi_t          intf,
			   struct ipmi_smi_msg *msg);

/* The lower layer received a watchdog pre-timeout on interface. */
void ipmi_smi_watchdog_pretimeout(ipmi_smi_t intf);

struct ipmi_smi_msg *ipmi_alloc_smi_msg(void);
static inline void ipmi_free_smi_msg(struct ipmi_smi_msg *msg)
{
	msg->done(msg);
}

/* Allow the lower layer to add things to the proc filesystem
   directory for this interface.  Note that the entry will
   automatically be dstroyed when the interface is destroyed. */
int ipmi_smi_add_proc_entry(ipmi_smi_t smi, char *name,
			    read_proc_t *read_proc, write_proc_t *write_proc,
			    void *data, struct module *owner);

#endif /* __LINUX_IPMI_SMI_H */
