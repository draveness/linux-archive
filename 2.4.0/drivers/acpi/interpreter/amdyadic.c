/******************************************************************************
 *
 * Module Name: amdyadic - ACPI AML (p-code) execution for dyadic operators
 *              $Revision: 68 $
 *
 *****************************************************************************/

/*
 *  Copyright (C) 2000 R. Byron Moore
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include "acpi.h"
#include "acparser.h"
#include "acnamesp.h"
#include "acinterp.h"
#include "acevents.h"
#include "amlcode.h"
#include "acdispat.h"


#define _COMPONENT          INTERPRETER
	 MODULE_NAME         ("amdyadic")


/*****************************************************************************
 *
 * FUNCTION:    Acpi_aml_exec_dyadic1
 *
 * PARAMETERS:  Opcode              - The opcode to be executed
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Execute Type 1 dyadic operator with numeric operands:
 *              Notify_op
 *
 * ALLOCATION:  Deletes both operands
 *
 ****************************************************************************/

ACPI_STATUS
acpi_aml_exec_dyadic1 (
	u16                     opcode,
	ACPI_WALK_STATE         *walk_state)
{
	ACPI_OPERAND_OBJECT     *obj_desc = NULL;
	ACPI_OPERAND_OBJECT     *val_desc = NULL;
	ACPI_NAMESPACE_NODE     *node;
	ACPI_STATUS             status = AE_OK;


	/* Resolve all operands */

	status = acpi_aml_resolve_operands (opcode, WALK_OPERANDS, walk_state);
	/* Get the operands */

	status |= acpi_ds_obj_stack_pop_object (&val_desc, walk_state);
	status |= acpi_ds_obj_stack_pop_object (&obj_desc, walk_state);
	if (ACPI_FAILURE (status)) {
		/* Invalid parameters on object stack  */

		goto cleanup;
	}


	/* Examine the opcode */

	switch (opcode)
	{

	/* Def_notify  :=  Notify_op   Notify_object   Notify_value */

	case AML_NOTIFY_OP:

		/* The Obj_desc is actually an Node */

		node = (ACPI_NAMESPACE_NODE *) obj_desc;
		obj_desc = NULL;

		/* Object must be a device or thermal zone */

		if (node && val_desc) {
			switch (node->type)
			{
			case ACPI_TYPE_DEVICE:
			case ACPI_TYPE_THERMAL:

				/*
				 * Requires that Device and Thermal_zone be compatible
				 * mappings
				 */

				/* Dispatch the notify to the appropriate handler */

				acpi_ev_notify_dispatch (node, (u32) val_desc->number.value);
				break;

			default:
				status = AE_AML_OPERAND_TYPE;
			}
		}
		break;

	default:

		REPORT_ERROR (("Acpi_aml_exec_dyadic1: Unknown dyadic opcode %X\n",
			opcode));
		status = AE_AML_BAD_OPCODE;
	}


cleanup:

	/* Always delete both operands */

	acpi_cm_remove_reference (val_desc);
	acpi_cm_remove_reference (obj_desc);


	return (status);
}


/*****************************************************************************
 *
 * FUNCTION:    Acpi_aml_exec_dyadic2_r
 *
 * PARAMETERS:  Opcode              - The opcode to be executed
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Execute Type 2 dyadic operator with numeric operands and
 *              one or two result operands.
 *
 * ALLOCATION:  Deletes one operand descriptor -- other remains on stack
 *
 ****************************************************************************/

ACPI_STATUS
acpi_aml_exec_dyadic2_r (
	u16                     opcode,
	ACPI_WALK_STATE         *walk_state,
	ACPI_OPERAND_OBJECT     **return_desc)
{
	ACPI_OPERAND_OBJECT     *obj_desc   = NULL;
	ACPI_OPERAND_OBJECT     *obj_desc2  = NULL;
	ACPI_OPERAND_OBJECT     *res_desc   = NULL;
	ACPI_OPERAND_OBJECT     *res_desc2  = NULL;
	ACPI_OPERAND_OBJECT     *ret_desc   = NULL;
	ACPI_OPERAND_OBJECT     *ret_desc2  = NULL;
	ACPI_STATUS             status      = AE_OK;
	u32                     num_operands = 3;
	NATIVE_CHAR             *new_buf;


	/* Resolve all operands */

	status = acpi_aml_resolve_operands (opcode, WALK_OPERANDS, walk_state);
	/* Get all operands */

	if (AML_DIVIDE_OP == opcode) {
		num_operands = 4;
		status |= acpi_ds_obj_stack_pop_object (&res_desc2, walk_state);
	}

	status |= acpi_ds_obj_stack_pop_object (&res_desc, walk_state);
	status |= acpi_ds_obj_stack_pop_object (&obj_desc2, walk_state);
	status |= acpi_ds_obj_stack_pop_object (&obj_desc, walk_state);
	if (ACPI_FAILURE (status)) {
		goto cleanup;
	}


	/* Create an internal return object if necessary */

	switch (opcode)
	{
	case AML_ADD_OP:
	case AML_BIT_AND_OP:
	case AML_BIT_NAND_OP:
	case AML_BIT_OR_OP:
	case AML_BIT_NOR_OP:
	case AML_BIT_XOR_OP:
	case AML_DIVIDE_OP:
	case AML_MULTIPLY_OP:
	case AML_SHIFT_LEFT_OP:
	case AML_SHIFT_RIGHT_OP:
	case AML_SUBTRACT_OP:

		ret_desc = acpi_cm_create_internal_object (ACPI_TYPE_NUMBER);
		if (!ret_desc) {
			status = AE_NO_MEMORY;
			goto cleanup;
		}

		break;
	}


	/*
	 * Execute the opcode
	 */

	switch (opcode)
	{

	/* Def_add :=  Add_op  Operand1    Operand2    Result  */

	case AML_ADD_OP:

		ret_desc->number.value = obj_desc->number.value +
				 obj_desc2->number.value;
		break;


	/* Def_and :=  And_op  Operand1    Operand2    Result  */

	case AML_BIT_AND_OP:

		ret_desc->number.value = obj_desc->number.value &
				 obj_desc2->number.value;
		break;


	/* Def_nAnd := NAnd_op Operand1    Operand2    Result  */

	case AML_BIT_NAND_OP:

		ret_desc->number.value = ~(obj_desc->number.value &
				   obj_desc2->number.value);
		break;


	/* Def_or  :=  Or_op   Operand1    Operand2    Result  */

	case AML_BIT_OR_OP:

		ret_desc->number.value = obj_desc->number.value |
				 obj_desc2->number.value;
		break;


	/* Def_nOr :=  NOr_op  Operand1    Operand2    Result  */

	case AML_BIT_NOR_OP:

		ret_desc->number.value = ~(obj_desc->number.value |
				   obj_desc2->number.value);
		break;


	/* Def_xOr :=  XOr_op  Operand1    Operand2    Result  */

	case AML_BIT_XOR_OP:

		ret_desc->number.value = obj_desc->number.value ^
				 obj_desc2->number.value;
		break;


	/* Def_divide  :=  Divide_op Dividend Divisor Remainder Quotient   */

	case AML_DIVIDE_OP:

		if (!obj_desc2->number.value) {
			REPORT_ERROR
				(("Aml_exec_dyadic2_r/Divide_op: Divide by zero\n"));

			status = AE_AML_DIVIDE_BY_ZERO;
			goto cleanup;
		}

		ret_desc2 = acpi_cm_create_internal_object (ACPI_TYPE_NUMBER);
		if (!ret_desc2) {
			status = AE_NO_MEMORY;
			goto cleanup;
		}

		/* Remainder (modulo) */

		ret_desc->number.value  = ACPI_MODULO (obj_desc->number.value,
				  obj_desc2->number.value);

		/* Result (what we used to call the quotient) */

		ret_desc2->number.value = ACPI_DIVIDE (obj_desc->number.value,
				  obj_desc2->number.value);
		break;


	/* Def_multiply := Multiply_op Operand1    Operand2    Result  */

	case AML_MULTIPLY_OP:

		ret_desc->number.value = obj_desc->number.value *
				 obj_desc2->number.value;
		break;


	/* Def_shift_left  :=  Shift_left_op Operand Shift_count Result */

	case AML_SHIFT_LEFT_OP:

		ret_desc->number.value = obj_desc->number.value <<
				 obj_desc2->number.value;
		break;


	/* Def_shift_right :=  Shift_right_op  Operand Shift_count Result  */

	case AML_SHIFT_RIGHT_OP:

		ret_desc->number.value = obj_desc->number.value >>
				 obj_desc2->number.value;
		break;


	/* Def_subtract := Subtract_op Operand1    Operand2    Result  */

	case AML_SUBTRACT_OP:

		ret_desc->number.value = obj_desc->number.value -
				 obj_desc2->number.value;
		break;


	/* Def_concat  :=  Concat_op   Data1   Data2   Result  */

	case AML_CONCAT_OP:

		if (obj_desc2->common.type != obj_desc->common.type) {
			status = AE_AML_OPERAND_TYPE;
			goto cleanup;
		}

		/* Both operands are now known to be the same */

		if (ACPI_TYPE_STRING == obj_desc->common.type) {
			ret_desc = acpi_cm_create_internal_object (ACPI_TYPE_STRING);
			if (!ret_desc) {
				status = AE_NO_MEMORY;
				goto cleanup;
			}

			/* Operand1 is string  */

			new_buf = acpi_cm_allocate (obj_desc->string.length +
					  obj_desc2->string.length + 1);
			if (!new_buf) {
				REPORT_ERROR
					(("Aml_exec_dyadic2_r/Concat_op: String allocation failure\n"));
				status = AE_NO_MEMORY;
				goto cleanup;
			}

			STRCPY (new_buf, obj_desc->string.pointer);
			STRCPY (new_buf + obj_desc->string.length,
					  obj_desc2->string.pointer);

			/* Point the return object to the new string */

			ret_desc->string.pointer = new_buf;
			ret_desc->string.length = obj_desc->string.length +=
					  obj_desc2->string.length;
		}

		else {
			/* Operand1 is not a string ==> must be a buffer */

			ret_desc = acpi_cm_create_internal_object (ACPI_TYPE_BUFFER);
			if (!ret_desc) {
				status = AE_NO_MEMORY;
				goto cleanup;
			}

			new_buf = acpi_cm_allocate (obj_desc->buffer.length +
					  obj_desc2->buffer.length);
			if (!new_buf) {
				REPORT_ERROR
					(("Aml_exec_dyadic2_r/Concat_op: Buffer allocation failure\n"));
				status = AE_NO_MEMORY;
				goto cleanup;
			}

			MEMCPY (new_buf, obj_desc->buffer.pointer,
					  obj_desc->buffer.length);
			MEMCPY (new_buf + obj_desc->buffer.length, obj_desc2->buffer.pointer,
					  obj_desc2->buffer.length);

			/*
			 * Point the return object to the new buffer
			 */

			ret_desc->buffer.pointer    = (u8 *) new_buf;
			ret_desc->buffer.length     = obj_desc->buffer.length +
					 obj_desc2->buffer.length;
		}
		break;


	default:

		REPORT_ERROR (("Acpi_aml_exec_dyadic2_r: Unknown dyadic opcode %X\n", opcode));
		status = AE_AML_BAD_OPCODE;
		goto cleanup;
	}


	/*
	 * Store the result of the operation (which is now in Obj_desc) into
	 * the result descriptor, or the location pointed to by the result
	 * descriptor (Res_desc).
	 */

	status = acpi_aml_exec_store (ret_desc, res_desc, walk_state);
	if (ACPI_FAILURE (status)) {
		goto cleanup;
	}

	if (AML_DIVIDE_OP == opcode) {
		status = acpi_aml_exec_store (ret_desc2, res_desc2, walk_state);

		/*
		 * Since the remainder is not returned, remove a reference to
		 * the object we created earlier
		 */

		acpi_cm_remove_reference (ret_desc2);
	}


cleanup:

	/* Always delete the operands */

	acpi_cm_remove_reference (obj_desc);
	acpi_cm_remove_reference (obj_desc2);


	/* Delete return object on error */

	if (ACPI_FAILURE (status)) {
		/* On failure, delete the result ops */

		acpi_cm_remove_reference (res_desc);
		acpi_cm_remove_reference (res_desc2);

		if (ret_desc) {
			/* And delete the internal return object */

			acpi_cm_remove_reference (ret_desc);
			ret_desc = NULL;
		}
	}

	/* Set the return object and exit */

	*return_desc = ret_desc;
	return (status);
}


/*****************************************************************************
 *
 * FUNCTION:    Acpi_aml_exec_dyadic2_s
 *
 * PARAMETERS:  Opcode              - The opcode to be executed
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Execute Type 2 dyadic synchronization operator
 *
 * ALLOCATION:  Deletes one operand descriptor -- other remains on stack
 *
 ****************************************************************************/

ACPI_STATUS
acpi_aml_exec_dyadic2_s (
	u16                     opcode,
	ACPI_WALK_STATE         *walk_state,
	ACPI_OPERAND_OBJECT     **return_desc)
{
	ACPI_OPERAND_OBJECT     *obj_desc;
	ACPI_OPERAND_OBJECT     *time_desc;
	ACPI_OPERAND_OBJECT     *ret_desc = NULL;
	ACPI_STATUS             status;


	/* Resolve all operands */

	status = acpi_aml_resolve_operands (opcode, WALK_OPERANDS, walk_state);
	/* Get all operands */

	status |= acpi_ds_obj_stack_pop_object (&time_desc, walk_state);
	status |= acpi_ds_obj_stack_pop_object (&obj_desc, walk_state);
	if (ACPI_FAILURE (status)) {
		/* Invalid parameters on object stack  */

		goto cleanup;
	}


	/* Create the internal return object */

	ret_desc = acpi_cm_create_internal_object (ACPI_TYPE_NUMBER);
	if (!ret_desc) {
		status = AE_NO_MEMORY;
		goto cleanup;
	}

	/* Default return value is FALSE, operation did not time out */

	ret_desc->number.value = 0;


	/* Examine the opcode */

	switch (opcode)
	{

	/* Def_acquire :=  Acquire_op  Mutex_object Timeout */

	case AML_ACQUIRE_OP:

		status = acpi_aml_system_acquire_mutex (time_desc, obj_desc);
		break;


	/* Def_wait := Wait_op Acpi_event_object Timeout */

	case AML_WAIT_OP:

		status = acpi_aml_system_wait_event (time_desc, obj_desc);
		break;


	default:

		REPORT_ERROR (("Acpi_aml_exec_dyadic2_s: Unknown dyadic synchronization opcode %X\n", opcode));
		status = AE_AML_BAD_OPCODE;
		goto cleanup;
	}


	/*
	 * Return a boolean indicating if operation timed out
	 * (TRUE) or not (FALSE)
	 */

	if (status == AE_TIME) {
		ret_desc->number.value = ACPI_INTEGER_MAX;  /* TRUE, op timed out */
		status = AE_OK;
	}


cleanup:

	/* Delete params */

	acpi_cm_remove_reference (time_desc);
	acpi_cm_remove_reference (obj_desc);

	/* Delete return object on error */

	if (ACPI_FAILURE (status) &&
		(ret_desc))
	{
		acpi_cm_remove_reference (ret_desc);
		ret_desc = NULL;
	}


	/* Set the return object and exit */

	*return_desc = ret_desc;
	return (status);
}


/*****************************************************************************
 *
 * FUNCTION:    Acpi_aml_exec_dyadic2
 *
 * PARAMETERS:  Opcode              - The opcode to be executed
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Execute Type 2 dyadic operator with numeric operands and
 *              no result operands
 *
 * ALLOCATION:  Deletes one operand descriptor -- other remains on stack
 *              containing result value
 *
 ****************************************************************************/

ACPI_STATUS
acpi_aml_exec_dyadic2 (
	u16                     opcode,
	ACPI_WALK_STATE         *walk_state,
	ACPI_OPERAND_OBJECT     **return_desc)
{
	ACPI_OPERAND_OBJECT     *obj_desc;
	ACPI_OPERAND_OBJECT     *obj_desc2;
	ACPI_OPERAND_OBJECT     *ret_desc = NULL;
	ACPI_STATUS             status;
	u8                      lboolean;


	/* Resolve all operands */

	status = acpi_aml_resolve_operands (opcode, WALK_OPERANDS, walk_state);
	/* Get all operands */

	status |= acpi_ds_obj_stack_pop_object (&obj_desc2, walk_state);
	status |= acpi_ds_obj_stack_pop_object (&obj_desc, walk_state);
	if (ACPI_FAILURE (status)) {
		/* Invalid parameters on object stack  */

		goto cleanup;
	}


	/* Create the internal return object */

	ret_desc = acpi_cm_create_internal_object (ACPI_TYPE_NUMBER);
	if (!ret_desc) {
		status = AE_NO_MEMORY;
		goto cleanup;
	}

	/*
	 * Execute the Opcode
	 */

	lboolean = FALSE;
	switch (opcode)
	{

	/* Def_lAnd := LAnd_op Operand1    Operand2    */

	case AML_LAND_OP:

		lboolean = (u8) (obj_desc->number.value &&
				  obj_desc2->number.value);
		break;


	/* Def_lEqual  :=  LEqual_op   Operand1    Operand2    */

	case AML_LEQUAL_OP:

		lboolean = (u8) (obj_desc->number.value ==
				  obj_desc2->number.value);
		break;


	/* Def_lGreater := LGreater_op Operand1    Operand2    */

	case AML_LGREATER_OP:

		lboolean = (u8) (obj_desc->number.value >
				  obj_desc2->number.value);
		break;


	/* Def_lLess   :=  LLess_op Operand1   Operand2    */

	case AML_LLESS_OP:

		lboolean = (u8) (obj_desc->number.value <
				  obj_desc2->number.value);
		break;


	/* Def_lOr :=  LOr_op  Operand1    Operand2    */

	case AML_LOR_OP:

		lboolean = (u8) (obj_desc->number.value ||
				  obj_desc2->number.value);
		break;


	default:

		REPORT_ERROR (("Acpi_aml_exec_dyadic2: Unknown dyadic opcode %X\n", opcode));
		status = AE_AML_BAD_OPCODE;
		goto cleanup;
		break;
	}


	/* Set return value to logical TRUE (all ones) or FALSE (zero) */

	if (lboolean) {
		ret_desc->number.value = ACPI_INTEGER_MAX;
	}
	else {
		ret_desc->number.value = 0;
	}


cleanup:

	/* Always delete operands */

	acpi_cm_remove_reference (obj_desc);
	acpi_cm_remove_reference (obj_desc2);


	/* Delete return object on error */

	if (ACPI_FAILURE (status) &&
		(ret_desc))
	{
		acpi_cm_remove_reference (ret_desc);
		ret_desc = NULL;
	}


	/* Set the return object and exit */

	*return_desc = ret_desc;
	return (status);
}


