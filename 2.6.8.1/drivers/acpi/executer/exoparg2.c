/******************************************************************************
 *
 * Module Name: exoparg2 - AML execution - opcodes with 2 arguments
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
#include <acpi/acparser.h>
#include <acpi/acinterp.h>
#include <acpi/acevents.h>
#include <acpi/amlcode.h>


#define _COMPONENT          ACPI_EXECUTER
	 ACPI_MODULE_NAME    ("exoparg2")


/*!
 * Naming convention for AML interpreter execution routines.
 *
 * The routines that begin execution of AML opcodes are named with a common
 * convention based upon the number of arguments, the number of target operands,
 * and whether or not a value is returned:
 *
 *      AcpiExOpcode_xA_yT_zR
 *
 * Where:
 *
 * xA - ARGUMENTS:    The number of arguments (input operands) that are
 *                    required for this opcode type (1 through 6 args).
 * yT - TARGETS:      The number of targets (output operands) that are required
 *                    for this opcode type (0, 1, or 2 targets).
 * zR - RETURN VALUE: Indicates whether this opcode type returns a value
 *                    as the function return (0 or 1).
 *
 * The AcpiExOpcode* functions are called via the Dispatcher component with
 * fully resolved operands.
!*/


/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_opcode_2A_0T_0R
 *
 * PARAMETERS:  walk_state          - Current walk state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Execute opcode with two arguments, no target, and no return
 *              value.
 *
 * ALLOCATION:  Deletes both operands
 *
 ******************************************************************************/

acpi_status
acpi_ex_opcode_2A_0T_0R (
	struct acpi_walk_state          *walk_state)
{
	union acpi_operand_object       **operand = &walk_state->operands[0];
	struct acpi_namespace_node      *node;
	acpi_status                     status = AE_OK;


	ACPI_FUNCTION_TRACE_STR ("ex_opcode_2A_0T_0R",
			acpi_ps_get_opcode_name (walk_state->opcode));


	/* Examine the opcode */

	switch (walk_state->opcode) {
	case AML_NOTIFY_OP:         /* Notify (notify_object, notify_value) */

		/* The first operand is a namespace node */

		node = (struct acpi_namespace_node *) operand[0];

		/* Notifies allowed on this object? */

		if (!acpi_ev_is_notify_object (node)) {
			ACPI_DEBUG_PRINT ((ACPI_DB_ERROR, "Unexpected notify object type [%s]\n",
					acpi_ut_get_type_name (node->type)));

			status = AE_AML_OPERAND_TYPE;
			break;
		}

		/*
		 * Dispatch the notify to the appropriate handler
		 * NOTE: the request is queued for execution after this method
		 * completes.  The notify handlers are NOT invoked synchronously
		 * from this thread -- because handlers may in turn run other
		 * control methods.
		 */
		status = acpi_ev_queue_notify_request (node,
				  (u32) operand[1]->integer.value);
		break;


	default:

		ACPI_REPORT_ERROR (("acpi_ex_opcode_2A_0T_0R: Unknown opcode %X\n",
				walk_state->opcode));
		status = AE_AML_BAD_OPCODE;
	}

	return_ACPI_STATUS (status);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_opcode_2A_2T_1R
 *
 * PARAMETERS:  walk_state          - Current walk state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Execute a dyadic operator (2 operands) with 2 output targets
 *              and one implicit return value.
 *
 ******************************************************************************/

acpi_status
acpi_ex_opcode_2A_2T_1R (
	struct acpi_walk_state          *walk_state)
{
	union acpi_operand_object       **operand = &walk_state->operands[0];
	union acpi_operand_object       *return_desc1 = NULL;
	union acpi_operand_object       *return_desc2 = NULL;
	acpi_status                     status;


	ACPI_FUNCTION_TRACE_STR ("ex_opcode_2A_2T_1R", acpi_ps_get_opcode_name (walk_state->opcode));


	/*
	 * Execute the opcode
	 */
	switch (walk_state->opcode) {
	case AML_DIVIDE_OP:             /* Divide (Dividend, Divisor, remainder_result quotient_result) */

		return_desc1 = acpi_ut_create_internal_object (ACPI_TYPE_INTEGER);
		if (!return_desc1) {
			status = AE_NO_MEMORY;
			goto cleanup;
		}

		return_desc2 = acpi_ut_create_internal_object (ACPI_TYPE_INTEGER);
		if (!return_desc2) {
			status = AE_NO_MEMORY;
			goto cleanup;
		}

		/* Quotient to return_desc1, remainder to return_desc2 */

		status = acpi_ut_divide (&operand[0]->integer.value, &operand[1]->integer.value,
				   &return_desc1->integer.value, &return_desc2->integer.value);
		if (ACPI_FAILURE (status)) {
			goto cleanup;
		}
		break;


	default:

		ACPI_REPORT_ERROR (("acpi_ex_opcode_2A_2T_1R: Unknown opcode %X\n",
				walk_state->opcode));
		status = AE_AML_BAD_OPCODE;
		goto cleanup;
	}


	/* Store the results to the target reference operands */

	status = acpi_ex_store (return_desc2, operand[2], walk_state);
	if (ACPI_FAILURE (status)) {
		goto cleanup;
	}

	status = acpi_ex_store (return_desc1, operand[3], walk_state);
	if (ACPI_FAILURE (status)) {
		goto cleanup;
	}

	/* Return the remainder */

	walk_state->result_obj = return_desc1;


cleanup:
	/*
	 * Since the remainder is not returned indirectly, remove a reference to
	 * it. Only the quotient is returned indirectly.
	 */
	acpi_ut_remove_reference (return_desc2);

	if (ACPI_FAILURE (status)) {
		/* Delete the return object */

		acpi_ut_remove_reference (return_desc1);
	}

	return_ACPI_STATUS (status);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_opcode_2A_1T_1R
 *
 * PARAMETERS:  walk_state          - Current walk state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Execute opcode with two arguments, one target, and a return
 *              value.
 *
 ******************************************************************************/

acpi_status
acpi_ex_opcode_2A_1T_1R (
	struct acpi_walk_state          *walk_state)
{
	union acpi_operand_object       **operand = &walk_state->operands[0];
	union acpi_operand_object       *return_desc = NULL;
	union acpi_operand_object       *temp_desc = NULL;
	u32                             index;
	acpi_status                     status = AE_OK;
	acpi_size                       length;


	ACPI_FUNCTION_TRACE_STR ("ex_opcode_2A_1T_1R", acpi_ps_get_opcode_name (walk_state->opcode));


	/*
	 * Execute the opcode
	 */
	if (walk_state->op_info->flags & AML_MATH) {
		/* All simple math opcodes (add, etc.) */

		return_desc = acpi_ut_create_internal_object (ACPI_TYPE_INTEGER);
		if (!return_desc) {
			status = AE_NO_MEMORY;
			goto cleanup;
		}

		return_desc->integer.value = acpi_ex_do_math_op (walk_state->opcode,
				  operand[0]->integer.value,
				  operand[1]->integer.value);
		goto store_result_to_target;
	}


	switch (walk_state->opcode) {
	case AML_MOD_OP:                /* Mod (Dividend, Divisor, remainder_result (ACPI 2.0) */

		return_desc = acpi_ut_create_internal_object (ACPI_TYPE_INTEGER);
		if (!return_desc) {
			status = AE_NO_MEMORY;
			goto cleanup;
		}

		/* return_desc will contain the remainder */

		status = acpi_ut_divide (&operand[0]->integer.value, &operand[1]->integer.value,
				  NULL, &return_desc->integer.value);
		break;


	case AML_CONCAT_OP:             /* Concatenate (Data1, Data2, Result) */

		/*
		 * Convert the second operand if necessary.  The first operand
		 * determines the type of the second operand, (See the Data Types
		 * section of the ACPI specification.)  Both object types are
		 * guaranteed to be either Integer/String/Buffer by the operand
		 * resolution mechanism above.
		 */
		switch (ACPI_GET_OBJECT_TYPE (operand[0])) {
		case ACPI_TYPE_INTEGER:
			status = acpi_ex_convert_to_integer (operand[1], &temp_desc, walk_state);
			break;

		case ACPI_TYPE_STRING:
			status = acpi_ex_convert_to_string (operand[1], &temp_desc, 16, ACPI_UINT32_MAX, walk_state);
			break;

		case ACPI_TYPE_BUFFER:
			status = acpi_ex_convert_to_buffer (operand[1], &temp_desc, walk_state);
			break;

		default:
			ACPI_REPORT_ERROR (("Concat - invalid obj type: %X\n",
					ACPI_GET_OBJECT_TYPE (operand[0])));
			status = AE_AML_INTERNAL;
		}

		if (ACPI_FAILURE (status)) {
			goto cleanup;
		}

		/*
		 * Both operands are now known to be the same object type
		 * (Both are Integer, String, or Buffer), and we can now perform the
		 * concatenation.
		 */
		status = acpi_ex_do_concatenate (operand[0], temp_desc, &return_desc, walk_state);
		if (temp_desc != operand[1]) {
			acpi_ut_remove_reference (temp_desc);
		}
		break;


	case AML_TO_STRING_OP:          /* to_string (Buffer, Length, Result) (ACPI 2.0) */

		/*
		 * Input object is guaranteed to be a buffer at this point (it may have
		 * been converted.)  Copy the raw buffer data to a new object of type String.
		 */

		/* Get the length of the new string */

		length = 0;
		if (operand[1]->integer.value == 0) {
			/* Handle optional length value */

			operand[1]->integer.value = ACPI_INTEGER_MAX;
		}

		while ((length < operand[0]->buffer.length) &&
			   (length < operand[1]->integer.value) &&
			   (operand[0]->buffer.pointer[length])) {
			length++;
		}

		if (length > ACPI_MAX_STRING_CONVERSION) {
			status = AE_AML_STRING_LIMIT;
			goto cleanup;
		}

		/* Create the internal return object */

		return_desc = acpi_ut_create_internal_object (ACPI_TYPE_STRING);
		if (!return_desc) {
			status = AE_NO_MEMORY;
			goto cleanup;
		}

		/* Allocate a new string buffer (Length + 1 for null terminator) */

		return_desc->string.pointer = ACPI_MEM_CALLOCATE (length + 1);
		if (!return_desc->string.pointer) {
			status = AE_NO_MEMORY;
			goto cleanup;
		}

		/* Copy the raw buffer data with no transform */

		ACPI_MEMCPY (return_desc->string.pointer, operand[0]->buffer.pointer, length);

		/* Set the string length */

		return_desc->string.length = (u32) length;
		break;


	case AML_CONCAT_RES_OP:         /* concatenate_res_template (Buffer, Buffer, Result) (ACPI 2.0) */

		status = acpi_ex_concat_template (operand[0], operand[1], &return_desc, walk_state);
		break;


	case AML_INDEX_OP:              /* Index (Source Index Result) */

		/* Create the internal return object */

		return_desc = acpi_ut_create_internal_object (ACPI_TYPE_LOCAL_REFERENCE);
		if (!return_desc) {
			status = AE_NO_MEMORY;
			goto cleanup;
		}

		index = (u32) operand[1]->integer.value;

		/*
		 * At this point, the Source operand is either a Package or a Buffer
		 */
		if (ACPI_GET_OBJECT_TYPE (operand[0]) == ACPI_TYPE_PACKAGE) {
			/* Object to be indexed is a Package */

			if (index >= operand[0]->package.count) {
				ACPI_DEBUG_PRINT ((ACPI_DB_ERROR, "Index value (%X) beyond package end (%X)\n",
					index, operand[0]->package.count));
				status = AE_AML_PACKAGE_LIMIT;
				goto cleanup;
			}

			return_desc->reference.target_type = ACPI_TYPE_PACKAGE;
			return_desc->reference.object    = operand[0];
			return_desc->reference.where     = &operand[0]->package.elements [index];
		}
		else {
			/* Object to be indexed is a Buffer */

			if (index >= operand[0]->buffer.length) {
				ACPI_DEBUG_PRINT ((ACPI_DB_ERROR, "Index value (%X) beyond end of buffer (%X)\n",
					index, operand[0]->buffer.length));
				status = AE_AML_BUFFER_LIMIT;
				goto cleanup;
			}

			return_desc->reference.target_type = ACPI_TYPE_BUFFER_FIELD;
			return_desc->reference.object    = operand[0];
		}

		/* Complete the Index reference object */

		return_desc->reference.opcode    = AML_INDEX_OP;
		return_desc->reference.offset    = index;

		/* Store the reference to the Target */

		status = acpi_ex_store (return_desc, operand[2], walk_state);

		/* Return the reference */

		walk_state->result_obj = return_desc;
		goto cleanup;


	default:

		ACPI_REPORT_ERROR (("acpi_ex_opcode_2A_1T_1R: Unknown opcode %X\n",
				walk_state->opcode));
		status = AE_AML_BAD_OPCODE;
		break;
	}


store_result_to_target:

	if (ACPI_SUCCESS (status)) {
		/*
		 * Store the result of the operation (which is now in return_desc) into
		 * the Target descriptor.
		 */
		status = acpi_ex_store (return_desc, operand[2], walk_state);
		if (ACPI_FAILURE (status)) {
			goto cleanup;
		}

		if (!walk_state->result_obj) {
			walk_state->result_obj = return_desc;
		}
	}


cleanup:

	/* Delete return object on error */

	if (ACPI_FAILURE (status)) {
		acpi_ut_remove_reference (return_desc);
	}

	return_ACPI_STATUS (status);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_opcode_2A_0T_1R
 *
 * PARAMETERS:  walk_state          - Current walk state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Execute opcode with 2 arguments, no target, and a return value
 *
 ******************************************************************************/

acpi_status
acpi_ex_opcode_2A_0T_1R (
	struct acpi_walk_state          *walk_state)
{
	union acpi_operand_object       **operand = &walk_state->operands[0];
	union acpi_operand_object       *return_desc = NULL;
	acpi_status                     status = AE_OK;
	u8                              logical_result = FALSE;


	ACPI_FUNCTION_TRACE_STR ("ex_opcode_2A_0T_1R", acpi_ps_get_opcode_name (walk_state->opcode));


	/* Create the internal return object */

	return_desc = acpi_ut_create_internal_object (ACPI_TYPE_INTEGER);
	if (!return_desc) {
		status = AE_NO_MEMORY;
		goto cleanup;
	}

	/*
	 * Execute the Opcode
	 */
	if (walk_state->op_info->flags & AML_LOGICAL) /* logical_op (Operand0, Operand1) */ {
		logical_result = acpi_ex_do_logical_op (walk_state->opcode,
				 operand[0]->integer.value,
				 operand[1]->integer.value);
		goto store_logical_result;
	}


	switch (walk_state->opcode) {
	case AML_ACQUIRE_OP:            /* Acquire (mutex_object, Timeout) */

		status = acpi_ex_acquire_mutex (operand[1], operand[0], walk_state);
		if (status == AE_TIME) {
			logical_result = TRUE;      /* TRUE = Acquire timed out */
			status = AE_OK;
		}
		break;


	case AML_WAIT_OP:               /* Wait (event_object, Timeout) */

		status = acpi_ex_system_wait_event (operand[1], operand[0]);
		if (status == AE_TIME) {
			logical_result = TRUE;      /* TRUE, Wait timed out */
			status = AE_OK;
		}
		break;


	default:

		ACPI_REPORT_ERROR (("acpi_ex_opcode_2A_0T_1R: Unknown opcode %X\n",
			walk_state->opcode));
		status = AE_AML_BAD_OPCODE;
		goto cleanup;
	}


store_logical_result:
	/*
	 * Set return value to according to logical_result. logical TRUE (all ones)
	 * Default is FALSE (zero)
	 */
	if (logical_result) {
		return_desc->integer.value = ACPI_INTEGER_MAX;
	}

	walk_state->result_obj = return_desc;


cleanup:

	/* Delete return object on error */

	if (ACPI_FAILURE (status)) {
		acpi_ut_remove_reference (return_desc);
	}

	return_ACPI_STATUS (status);
}


