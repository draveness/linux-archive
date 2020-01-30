/******************************************************************************
 *
 * Module Name: evxface - External interfaces for ACPI events
 *
 *****************************************************************************/

/*
 * Copyright (C) 2000 - 2004, R. Byron Moore
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    substantially similar to the "NO WARRANTY" disclaimer below
 *    ("Disclaimer") and any redistribution must be conditioned upon
 *    including a substantially similar Disclaimer requirement for further
 *    binary redistribution.
 * 3. Neither the names of the above-listed copyright holders nor the names
 *    of any contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGES.
 */


#include <acpi/acpi.h>
#include <acpi/acnamesp.h>
#include <acpi/acevents.h>
#include <acpi/acinterp.h>

#define _COMPONENT          ACPI_EVENTS
	 ACPI_MODULE_NAME    ("evxface")


/*******************************************************************************
 *
 * FUNCTION:    acpi_install_fixed_event_handler
 *
 * PARAMETERS:  Event           - Event type to enable.
 *              Handler         - Pointer to the handler function for the
 *                                event
 *              Context         - Value passed to the handler on each GPE
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Saves the pointer to the handler function and then enables the
 *              event.
 *
 ******************************************************************************/

acpi_status
acpi_install_fixed_event_handler (
	u32                             event,
	acpi_event_handler              handler,
	void                            *context)
{
	acpi_status                     status;


	ACPI_FUNCTION_TRACE ("acpi_install_fixed_event_handler");


	/* Parameter validation */

	if (event > ACPI_EVENT_MAX) {
		return_ACPI_STATUS (AE_BAD_PARAMETER);
	}

	status = acpi_ut_acquire_mutex (ACPI_MTX_EVENTS);
	if (ACPI_FAILURE (status)) {
		return_ACPI_STATUS (status);
	}

	/* Don't allow two handlers. */

	if (NULL != acpi_gbl_fixed_event_handlers[event].handler) {
		status = AE_ALREADY_EXISTS;
		goto cleanup;
	}

	/* Install the handler before enabling the event */

	acpi_gbl_fixed_event_handlers[event].handler = handler;
	acpi_gbl_fixed_event_handlers[event].context = context;

	status = acpi_enable_event (event, 0);
	if (ACPI_FAILURE (status)) {
		ACPI_DEBUG_PRINT ((ACPI_DB_WARN, "Could not enable fixed event.\n"));

		/* Remove the handler */

		acpi_gbl_fixed_event_handlers[event].handler = NULL;
		acpi_gbl_fixed_event_handlers[event].context = NULL;
	}
	else {
		ACPI_DEBUG_PRINT ((ACPI_DB_INFO,
			"Enabled fixed event %X, Handler=%p\n", event, handler));
	}


cleanup:
	(void) acpi_ut_release_mutex (ACPI_MTX_EVENTS);
	return_ACPI_STATUS (status);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_remove_fixed_event_handler
 *
 * PARAMETERS:  Event           - Event type to disable.
 *              Handler         - Address of the handler
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Disables the event and unregisters the event handler.
 *
 ******************************************************************************/

acpi_status
acpi_remove_fixed_event_handler (
	u32                             event,
	acpi_event_handler              handler)
{
	acpi_status                     status = AE_OK;


	ACPI_FUNCTION_TRACE ("acpi_remove_fixed_event_handler");


	/* Parameter validation */

	if (event > ACPI_EVENT_MAX) {
		return_ACPI_STATUS (AE_BAD_PARAMETER);
	}

	status = acpi_ut_acquire_mutex (ACPI_MTX_EVENTS);
	if (ACPI_FAILURE (status)) {
		return_ACPI_STATUS (status);
	}

	/* Disable the event before removing the handler */

	status = acpi_disable_event (event, 0);

	/* Always Remove the handler */

	acpi_gbl_fixed_event_handlers[event].handler = NULL;
	acpi_gbl_fixed_event_handlers[event].context = NULL;

	if (ACPI_FAILURE (status)) {
		ACPI_DEBUG_PRINT ((ACPI_DB_WARN,
			"Could not write to fixed event enable register.\n"));
	}
	else {
		ACPI_DEBUG_PRINT ((ACPI_DB_INFO, "Disabled fixed event %X.\n", event));
	}

	(void) acpi_ut_release_mutex (ACPI_MTX_EVENTS);
	return_ACPI_STATUS (status);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_install_notify_handler
 *
 * PARAMETERS:  Device          - The device for which notifies will be handled
 *              handler_type    - The type of handler:
 *                                  ACPI_SYSTEM_NOTIFY: system_handler (00-7f)
 *                                  ACPI_DEVICE_NOTIFY: driver_handler (80-ff)
 *              Handler         - Address of the handler
 *              Context         - Value passed to the handler on each GPE
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Install a handler for notifies on an ACPI device
 *
 ******************************************************************************/

acpi_status
acpi_install_notify_handler (
	acpi_handle                     device,
	u32                             handler_type,
	acpi_notify_handler             handler,
	void                            *context)
{
	union acpi_operand_object       *obj_desc;
	union acpi_operand_object       *notify_obj;
	struct acpi_namespace_node      *node;
	acpi_status                     status;


	ACPI_FUNCTION_TRACE ("acpi_install_notify_handler");


	/* Parameter validation */

	if ((!device)  ||
		(!handler) ||
		(handler_type > ACPI_MAX_NOTIFY_HANDLER_TYPE)) {
		return_ACPI_STATUS (AE_BAD_PARAMETER);
	}

	status = acpi_ut_acquire_mutex (ACPI_MTX_NAMESPACE);
	if (ACPI_FAILURE (status)) {
		return_ACPI_STATUS (status);
	}

	/* Convert and validate the device handle */

	node = acpi_ns_map_handle_to_node (device);
	if (!node) {
		status = AE_BAD_PARAMETER;
		goto unlock_and_exit;
	}

	/*
	 * Root Object:
	 * Registering a notify handler on the root object indicates that the
	 * caller wishes to receive notifications for all objects.  Note that
	 * only one <external> global handler can be regsitered (per notify type).
	 */
	if (device == ACPI_ROOT_OBJECT) {
		/* Make sure the handler is not already installed */

		if (((handler_type == ACPI_SYSTEM_NOTIFY) &&
				acpi_gbl_system_notify.handler)        ||
			((handler_type == ACPI_DEVICE_NOTIFY) &&
				acpi_gbl_device_notify.handler)) {
			status = AE_ALREADY_EXISTS;
			goto unlock_and_exit;
		}

		if (handler_type == ACPI_SYSTEM_NOTIFY) {
			acpi_gbl_system_notify.node  = node;
			acpi_gbl_system_notify.handler = handler;
			acpi_gbl_system_notify.context = context;
		}
		else /* ACPI_DEVICE_NOTIFY */ {
			acpi_gbl_device_notify.node  = node;
			acpi_gbl_device_notify.handler = handler;
			acpi_gbl_device_notify.context = context;
		}

		/* Global notify handler installed */
	}

	/*
	 * All Other Objects:
	 * Caller will only receive notifications specific to the target object.
	 * Note that only certain object types can receive notifications.
	 */
	else {
		/* Notifies allowed on this object? */

		if (!acpi_ev_is_notify_object (node)) {
			status = AE_TYPE;
			goto unlock_and_exit;
		}

		/* Check for an existing internal object */

		obj_desc = acpi_ns_get_attached_object (node);
		if (obj_desc) {
			/* Object exists - make sure there's no handler */

			if (((handler_type == ACPI_SYSTEM_NOTIFY) &&
					obj_desc->common_notify.system_notify) ||
				((handler_type == ACPI_DEVICE_NOTIFY) &&
					obj_desc->common_notify.device_notify)) {
				status = AE_ALREADY_EXISTS;
				goto unlock_and_exit;
			}
		}
		else {
			/* Create a new object */

			obj_desc = acpi_ut_create_internal_object (node->type);
			if (!obj_desc) {
				status = AE_NO_MEMORY;
				goto unlock_and_exit;
			}

			/* Attach new object to the Node */

			status = acpi_ns_attach_object (device, obj_desc, node->type);

			/* Remove local reference to the object */

			acpi_ut_remove_reference (obj_desc);

			if (ACPI_FAILURE (status)) {
				goto unlock_and_exit;
			}
		}

		/* Install the handler */

		notify_obj = acpi_ut_create_internal_object (ACPI_TYPE_LOCAL_NOTIFY);
		if (!notify_obj) {
			status = AE_NO_MEMORY;
			goto unlock_and_exit;
		}

		notify_obj->notify.node   = node;
		notify_obj->notify.handler = handler;
		notify_obj->notify.context = context;

		if (handler_type == ACPI_SYSTEM_NOTIFY) {
			obj_desc->common_notify.system_notify = notify_obj;
		}
		else /* ACPI_DEVICE_NOTIFY */ {
			obj_desc->common_notify.device_notify = notify_obj;
		}
	}


unlock_and_exit:
	(void) acpi_ut_release_mutex (ACPI_MTX_NAMESPACE);
	return_ACPI_STATUS (status);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_remove_notify_handler
 *
 * PARAMETERS:  Device          - The device for which notifies will be handled
 *              handler_type    - The type of handler:
 *                                  ACPI_SYSTEM_NOTIFY: system_handler (00-7f)
 *                                  ACPI_DEVICE_NOTIFY: driver_handler (80-ff)
 *              Handler         - Address of the handler
 * RETURN:      Status
 *
 * DESCRIPTION: Remove a handler for notifies on an ACPI device
 *
 ******************************************************************************/

acpi_status
acpi_remove_notify_handler (
	acpi_handle                     device,
	u32                             handler_type,
	acpi_notify_handler             handler)
{
	union acpi_operand_object       *notify_obj;
	union acpi_operand_object       *obj_desc;
	struct acpi_namespace_node      *node;
	acpi_status                     status;


	ACPI_FUNCTION_TRACE ("acpi_remove_notify_handler");


	/* Parameter validation */

	if ((!device)  ||
		(!handler) ||
		(handler_type > ACPI_MAX_NOTIFY_HANDLER_TYPE)) {
		return_ACPI_STATUS (AE_BAD_PARAMETER);
	}

	status = acpi_ut_acquire_mutex (ACPI_MTX_NAMESPACE);
	if (ACPI_FAILURE (status)) {
		return_ACPI_STATUS (status);
	}

	/* Convert and validate the device handle */

	node = acpi_ns_map_handle_to_node (device);
	if (!node) {
		status = AE_BAD_PARAMETER;
		goto unlock_and_exit;
	}

	/*
	 * Root Object
	 */
	if (device == ACPI_ROOT_OBJECT) {
		ACPI_DEBUG_PRINT ((ACPI_DB_INFO, "Removing notify handler for ROOT object.\n"));

		if (((handler_type == ACPI_SYSTEM_NOTIFY) &&
			  !acpi_gbl_system_notify.handler) ||
			((handler_type == ACPI_DEVICE_NOTIFY) &&
			  !acpi_gbl_device_notify.handler)) {
			status = AE_NOT_EXIST;
			goto unlock_and_exit;
		}

		/* Make sure all deferred tasks are completed */

		(void) acpi_ut_release_mutex (ACPI_MTX_NAMESPACE);
		acpi_os_wait_events_complete(NULL);
		status = acpi_ut_acquire_mutex (ACPI_MTX_NAMESPACE);
		if (ACPI_FAILURE (status)) {
			return_ACPI_STATUS (status);
 		}

		if (handler_type == ACPI_SYSTEM_NOTIFY) {
			acpi_gbl_system_notify.node  = NULL;
			acpi_gbl_system_notify.handler = NULL;
			acpi_gbl_system_notify.context = NULL;
		}
		else {
			acpi_gbl_device_notify.node  = NULL;
			acpi_gbl_device_notify.handler = NULL;
			acpi_gbl_device_notify.context = NULL;
		}
	}

	/*
	 * All Other Objects
	 */
	else {
		/* Notifies allowed on this object? */

		if (!acpi_ev_is_notify_object (node)) {
			status = AE_TYPE;
			goto unlock_and_exit;
		}

		/* Check for an existing internal object */

		obj_desc = acpi_ns_get_attached_object (node);
		if (!obj_desc) {
			status = AE_NOT_EXIST;
			goto unlock_and_exit;
		}

		/* Object exists - make sure there's an existing handler */

		if (handler_type == ACPI_SYSTEM_NOTIFY) {
			notify_obj = obj_desc->common_notify.system_notify;
		}
		else {
			notify_obj = obj_desc->common_notify.device_notify;
		}

		if ((!notify_obj) ||
			(notify_obj->notify.handler != handler)) {
			status = AE_BAD_PARAMETER;
			goto unlock_and_exit;
		}

		/* Make sure all deferred tasks are completed */

		(void) acpi_ut_release_mutex (ACPI_MTX_NAMESPACE);
		acpi_os_wait_events_complete(NULL);
		status = acpi_ut_acquire_mutex (ACPI_MTX_NAMESPACE);
		if (ACPI_FAILURE (status)) {
			return_ACPI_STATUS (status);
 		}

		/* Remove the handler */

		if (handler_type == ACPI_SYSTEM_NOTIFY) {
			obj_desc->common_notify.system_notify = NULL;
		}
		else {
			obj_desc->common_notify.device_notify = NULL;
		}

		acpi_ut_remove_reference (notify_obj);
	}


unlock_and_exit:
	(void) acpi_ut_release_mutex (ACPI_MTX_NAMESPACE);
	return_ACPI_STATUS (status);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_install_gpe_handler
 *
 * PARAMETERS:  gpe_number      - The GPE number within the GPE block
 *              gpe_block       - GPE block (NULL == FADT GPEs)
 *              Type            - Whether this GPE should be treated as an
 *                                edge- or level-triggered interrupt.
 *              Handler         - Address of the handler
 *              Context         - Value passed to the handler on each GPE
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Install a handler for a General Purpose Event.
 *
 ******************************************************************************/

acpi_status
acpi_install_gpe_handler (
	acpi_handle                     gpe_device,
	u32                             gpe_number,
	u32                             type,
	acpi_gpe_handler                handler,
	void                            *context)
{
	acpi_status                     status;
	struct acpi_gpe_event_info      *gpe_event_info;


	ACPI_FUNCTION_TRACE ("acpi_install_gpe_handler");


	/* Parameter validation */

	if (!handler) {
		return_ACPI_STATUS (AE_BAD_PARAMETER);
	}

	status = acpi_ut_acquire_mutex (ACPI_MTX_EVENTS);
	if (ACPI_FAILURE (status)) {
		return_ACPI_STATUS (status);
	}

	/* Ensure that we have a valid GPE number */

	gpe_event_info = acpi_ev_get_gpe_event_info (gpe_device, gpe_number);
	if (!gpe_event_info) {
		status = AE_BAD_PARAMETER;
		goto unlock_and_exit;
	}

	/* Make sure that there isn't a handler there already */

	if (gpe_event_info->handler) {
		status = AE_ALREADY_EXISTS;
		goto unlock_and_exit;
	}

	/* Install the handler */

	acpi_os_acquire_lock (acpi_gbl_gpe_lock, ACPI_NOT_ISR);
	gpe_event_info->handler = handler;
	gpe_event_info->context = context;
	gpe_event_info->flags = (u8) type;
	acpi_os_release_lock (acpi_gbl_gpe_lock, ACPI_NOT_ISR);

	/* Clear the GPE (of stale events), the enable it */

	status = acpi_hw_clear_gpe (gpe_event_info);
	if (ACPI_FAILURE (status)) {
		goto unlock_and_exit;
	}

	status = acpi_hw_enable_gpe (gpe_event_info);


unlock_and_exit:
	(void) acpi_ut_release_mutex (ACPI_MTX_EVENTS);
	return_ACPI_STATUS (status);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_remove_gpe_handler
 *
 * PARAMETERS:  gpe_number      - The event to remove a handler
 *              gpe_block       - GPE block (NULL == FADT GPEs)
 *              Handler         - Address of the handler
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Remove a handler for a General Purpose acpi_event.
 *
 ******************************************************************************/

acpi_status
acpi_remove_gpe_handler (
	acpi_handle                     gpe_device,
	u32                             gpe_number,
	acpi_gpe_handler                handler)
{
	acpi_status                     status;
	struct acpi_gpe_event_info      *gpe_event_info;


	ACPI_FUNCTION_TRACE ("acpi_remove_gpe_handler");


	/* Parameter validation */

	if (!handler) {
		return_ACPI_STATUS (AE_BAD_PARAMETER);
	}

	status = acpi_ut_acquire_mutex (ACPI_MTX_EVENTS);
	if (ACPI_FAILURE (status)) {
		return_ACPI_STATUS (status);
	}

	/* Ensure that we have a valid GPE number */

	gpe_event_info = acpi_ev_get_gpe_event_info (gpe_device, gpe_number);
	if (!gpe_event_info) {
		status = AE_BAD_PARAMETER;
		goto unlock_and_exit;
	}

	/* Disable the GPE before removing the handler */

	status = acpi_hw_disable_gpe (gpe_event_info);
	if (ACPI_FAILURE (status)) {
		goto unlock_and_exit;
	}

	/* Make sure that the installed handler is the same */

	if (gpe_event_info->handler != handler) {
		(void) acpi_hw_enable_gpe (gpe_event_info);
		status = AE_BAD_PARAMETER;
		goto unlock_and_exit;
	}

	/* Make sure all deferred tasks are completed */

	(void) acpi_ut_release_mutex (ACPI_MTX_EVENTS);
	acpi_os_wait_events_complete(NULL);
	status = acpi_ut_acquire_mutex (ACPI_MTX_EVENTS);
	if (ACPI_FAILURE (status)) {
		return_ACPI_STATUS (status);
 	}

	/* Remove the handler */

	acpi_os_acquire_lock (acpi_gbl_gpe_lock, ACPI_NOT_ISR);
	gpe_event_info->handler = NULL;
	gpe_event_info->context = NULL;
	acpi_os_release_lock (acpi_gbl_gpe_lock, ACPI_NOT_ISR);


unlock_and_exit:
	(void) acpi_ut_release_mutex (ACPI_MTX_EVENTS);
	return_ACPI_STATUS (status);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_acquire_global_lock
 *
 * PARAMETERS:  Timeout         - How long the caller is willing to wait
 *              out_handle      - A handle to the lock if acquired
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Acquire the ACPI Global Lock
 *
 ******************************************************************************/

acpi_status
acpi_acquire_global_lock (
	u16                             timeout,
	u32                             *handle)
{
	acpi_status                     status;


	if (!handle) {
		return (AE_BAD_PARAMETER);
	}

	status = acpi_ex_enter_interpreter ();
	if (ACPI_FAILURE (status)) {
		return (status);
	}

	status = acpi_ev_acquire_global_lock (timeout);
	acpi_ex_exit_interpreter ();

	if (ACPI_SUCCESS (status)) {
		acpi_gbl_global_lock_handle++;
		*handle = acpi_gbl_global_lock_handle;
	}

	return (status);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_release_global_lock
 *
 * PARAMETERS:  Handle      - Returned from acpi_acquire_global_lock
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Release the ACPI Global Lock
 *
 ******************************************************************************/

acpi_status
acpi_release_global_lock (
	u32                             handle)
{
	acpi_status                     status;


	if (handle != acpi_gbl_global_lock_handle) {
		return (AE_NOT_ACQUIRED);
	}

	status = acpi_ev_release_global_lock ();
	return (status);
}


