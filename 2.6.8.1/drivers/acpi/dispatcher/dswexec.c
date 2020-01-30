/******************************************************************************
 *
 * Module Name: dswexec - Dispatcher method execution callbacks;
 *                        dispatch to interpreter.
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
#include <acpi/amlcode.h>
#include <acpi/acdispat.h>
#include <acpi/acinterp.h>
#include <acpi/acnamesp.h>
#include <acpi/acdebug.h>
#include <acpi/acdisasm.h>


#define _COMPONENT          ACPI_DISPATCHER
	 ACPI_MODULE_NAME    ("dswexec")

/*
 * Dispatch table for opcode classes
 */
static ACPI_EXECUTE_OP      acpi_gbl_op_type_dispatch [] = {
			 acpi_ex_opcode_1A_0T_0R,
			 acpi_ex_opcode_1A_0T_1R,
			 acpi_ex_opcode_1A_1T_0R,
			 acpi_ex_opcode_1A_1T_1R,
			 acpi_ex_opcode_2A_0T_0R,
			 acpi_ex_opcode_2A_0T_1R,
			 acpi_ex_opcode_2A_1T_1R,
			 acpi_ex_opcode_2A_2T_1R,
			 acpi_ex_opcode_3A_0T_0R,
			 acpi_ex_opcode_3A_1T_1R,
			 acpi_ex_opcode_6A_0T_1R};

/*****************************************************************************
 *
 * FUNCTION:    acpi_ds_get_predicate_value
 *
 * PARAMETERS:  walk_state      - Current state of the parse tree walk
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Get the result of a predicate evaluation
 *
 ****************************************************************************/

acpi_status
acpi_ds_get_predicate_value (
	struct acpi_walk_state          *walk_state,
	union acpi_operand_object       *result_obj) {
	acpi_status                     status = AE_OK;
	union acpi_operand_object       *obj_desc;


	ACPI_FUNCTION_TRACE_PTR ("ds_get_predicate_value", walk_state);


	walk_state->control_state->common.state = 0;

	if (result_obj) {
		status = acpi_ds_result_pop (&obj_desc, walk_state);
		if (ACPI_FAILURE (status)) {
			ACPI_DEBUG_PRINT ((ACPI_DB_ERROR,
				"Could not get result from predicate evaluation, %s\n",
				acpi_format_exception (status)));

			return_ACPI_STATUS (status);
		}
	}
	else {
		status = acpi_ds_create_operand (walk_state, walk_state->op, 0);
		if (ACPI_FAILURE (status)) {
			return_ACPI_STATUS (status);
		}

		status = acpi_ex_resolve_to_value (&walk_state->operands [0], walk_state);
		if (ACPI_FAILURE (status)) {
			return_ACPI_STATUS (status);
		}

		obj_desc = walk_state->operands [0];
	}

	if (!obj_desc) {
		ACPI_DEBUG_PRINT ((ACPI_DB_ERROR, "No predicate obj_desc=%p State=%p\n",
			obj_desc, walk_state));

		return_ACPI_STATUS (AE_AML_NO_OPERAND);
	}

	/*
	 * Result of predicate evaluation currently must
	 * be a number
	 */
	if (ACPI_GET_OBJECT_TYPE (obj_desc) != ACPI_TYPE_INTEGER) {
		ACPI_DEBUG_PRINT ((ACPI_DB_ERROR,
			"Bad predicate (not a number) obj_desc=%p State=%p Type=%X\n",
			obj_desc, walk_state, ACPI_GET_OBJECT_TYPE (obj_desc)));

		status = AE_AML_OPERAND_TYPE;
		goto cleanup;
	}

	/* Truncate the predicate to 32-bits if necessary */

	acpi_ex_truncate_for32bit_table (obj_desc);

	/*
	 * Save the result of the predicate evaluation on
	 * the control stack
	 */
	if (obj_desc->integer.value) {
		walk_state->control_state->common.value = TRUE;
	}
	else {
		/*
		 * Predicate is FALSE, we will just toss the
		 * rest of the package
		 */
		walk_state->control_state->common.value = FALSE;
		status = AE_CTRL_FALSE;
	}


cleanup:

	ACPI_DEBUG_PRINT ((ACPI_DB_EXEC, "Completed a predicate eval=%X Op=%p\n",
		walk_state->control_state->common.value, walk_state->op));

	 /* Break to debugger to display result */

	ACPI_DEBUGGER_EXEC (acpi_db_display_result_object (obj_desc, walk_state));

	/*
	 * Delete the predicate result object (we know that
	 * we don't need it anymore)
	 */
	acpi_ut_remove_reference (obj_desc);

	walk_state->control_state->common.state = ACPI_CONTROL_NORMAL;
	return_ACPI_STATUS (status);
}


/*****************************************************************************
 *
 * FUNCTION:    acpi_ds_exec_begin_op
 *
 * PARAMETERS:  walk_state      - Current state of the parse tree walk
 *              out_op          - Return op if a new one is created
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Descending callback used during the execution of control
 *              methods.  This is where most operators and operands are
 *              dispatched to the interpreter.
 *
 ****************************************************************************/

acpi_status
acpi_ds_exec_begin_op (
	struct acpi_walk_state          *walk_state,
	union acpi_parse_object         **out_op)
{
	union acpi_parse_object         *op;
	acpi_status                     status = AE_OK;
	u32                             opcode_class;


	ACPI_FUNCTION_TRACE_PTR ("ds_exec_begin_op", walk_state);


	op = walk_state->op;
	if (!op) {
		status = acpi_ds_load2_begin_op (walk_state, out_op);
		if (ACPI_FAILURE (status)) {
			return_ACPI_STATUS (status);
		}

		op = *out_op;
		walk_state->op = op;
		walk_state->opcode = op->common.aml_opcode;
		walk_state->op_info = acpi_ps_get_opcode_info (op->common.aml_opcode);

		if (acpi_ns_opens_scope (walk_state->op_info->object_type)) {
			ACPI_DEBUG_PRINT ((ACPI_DB_DISPATCH, "(%s) Popping scope for Op %p\n",
				acpi_ut_get_type_name (walk_state->op_info->object_type), op));

			status = acpi_ds_scope_stack_pop (walk_state);
			if (ACPI_FAILURE (status)) {
				return_ACPI_STATUS (status);
			}
		}
	}

	if (op == walk_state->origin) {
		if (out_op) {
			*out_op = op;
		}

		return_ACPI_STATUS (AE_OK);
	}

	/*
	 * If the previous opcode was a conditional, this opcode
	 * must be the beginning of the associated predicate.
	 * Save this knowledge in the current scope descriptor
	 */
	if ((walk_state->control_state) &&
		(walk_state->control_state->common.state ==
			ACPI_CONTROL_CONDITIONAL_EXECUTING)) {
		ACPI_DEBUG_PRINT ((ACPI_DB_EXEC, "Exec predicate Op=%p State=%p\n",
				  op, walk_state));

		walk_state->control_state->common.state = ACPI_CONTROL_PREDICATE_EXECUTING;

		/* Save start of predicate */

		walk_state->control_state->control.predicate_op = op;
	}


	opcode_class = walk_state->op_info->class;

	/* We want to send namepaths to the load code */

	if (op->common.aml_opcode == AML_INT_NAMEPATH_OP) {
		opcode_class = AML_CLASS_NAMED_OBJECT;
	}

	/*
	 * Handle the opcode based upon the opcode type
	 */
	switch (opcode_class) {
	case AML_CLASS_CONTROL:

		status = acpi_ds_result_stack_push (walk_state);
		if (ACPI_FAILURE (status)) {
			return_ACPI_STATUS (status);
		}

		status = acpi_ds_exec_begin_control_op (walk_state, op);
		break;


	case AML_CLASS_NAMED_OBJECT:

		if (walk_state->walk_type == ACPI_WALK_METHOD) {
			/*
			 * Found a named object declaration during method
			 * execution;  we must enter this object into the
			 * namespace.  The created object is temporary and
			 * will be deleted upon completion of the execution
			 * of this method.
			 */
			status = acpi_ds_load2_begin_op (walk_state, NULL);
		}

		if (op->common.aml_opcode == AML_REGION_OP) {
			status = acpi_ds_result_stack_push (walk_state);
		}
		break;


	case AML_CLASS_EXECUTE:
	case AML_CLASS_CREATE:

		/* most operators with arguments */
		/* Start a new result/operand state */

		status = acpi_ds_result_stack_push (walk_state);
		break;


	default:
		break;
	}

	/* Nothing to do here during method execution */

	return_ACPI_STATUS (status);
}


/*****************************************************************************
 *
 * FUNCTION:    acpi_ds_exec_end_op
 *
 * PARAMETERS:  walk_state      - Current state of the parse tree walk
 *              Op              - Op that has been just been completed in the
 *                                walk;  Arguments have now been evaluated.
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Ascending callback used during the execution of control
 *              methods.  The only thing we really need to do here is to
 *              notice the beginning of IF, ELSE, and WHILE blocks.
 *
 ****************************************************************************/

acpi_status
acpi_ds_exec_end_op (
	struct acpi_walk_state          *walk_state)
{
	union acpi_parse_object         *op;
	acpi_status                     status = AE_OK;
	u32                             op_type;
	u32                             op_class;
	union acpi_parse_object         *next_op;
	union acpi_parse_object         *first_arg;


	ACPI_FUNCTION_TRACE_PTR ("ds_exec_end_op", walk_state);


	op      = walk_state->op;
	op_type = walk_state->op_info->type;
	op_class = walk_state->op_info->class;

	if (op_class == AML_CLASS_UNKNOWN) {
		ACPI_DEBUG_PRINT ((ACPI_DB_ERROR, "Unknown opcode %X\n", op->common.aml_opcode));
		return_ACPI_STATUS (AE_NOT_IMPLEMENTED);
	}

	first_arg = op->common.value.arg;

	/* Init the walk state */

	walk_state->num_operands = 0;
	walk_state->return_desc = NULL;
	walk_state->result_obj = NULL;

	/* Call debugger for single step support (DEBUG build only) */

	ACPI_DEBUGGER_EXEC (status = acpi_db_single_step (walk_state, op, op_class));
	ACPI_DEBUGGER_EXEC (if (ACPI_FAILURE (status)) {return_ACPI_STATUS (status);});

	/* Decode the Opcode Class */

	switch (op_class) {
	case AML_CLASS_ARGUMENT:    /* constants, literals, etc. -- do nothing */
		break;


	case AML_CLASS_EXECUTE:     /* most operators with arguments */

		/* Build resolved operand stack */

		status = acpi_ds_create_operands (walk_state, first_arg);
		if (ACPI_FAILURE (status)) {
			goto cleanup;
		}

		/* Done with this result state (Now that operand stack is built) */

		status = acpi_ds_result_stack_pop (walk_state);
		if (ACPI_FAILURE (status)) {
			goto cleanup;
		}

		/* Resolve all operands */

		status = acpi_ex_resolve_operands (walk_state->opcode,
				  &(walk_state->operands [walk_state->num_operands -1]),
				  walk_state);
		if (ACPI_SUCCESS (status)) {
			ACPI_DUMP_OPERANDS (ACPI_WALK_OPERANDS, ACPI_IMODE_EXECUTE,
					  acpi_ps_get_opcode_name (walk_state->opcode),
					  walk_state->num_operands, "after ex_resolve_operands");

			/*
			 * Dispatch the request to the appropriate interpreter handler
			 * routine.  There is one routine per opcode "type" based upon the
			 * number of opcode arguments and return type.
			 */
			status = acpi_gbl_op_type_dispatch [op_type] (walk_state);
		}
		else {
			/*
			 * Treat constructs of the form "Store(local_x,local_x)" as noops when the
			 * Local is uninitialized.
			 */
			if  ((status == AE_AML_UNINITIALIZED_LOCAL) &&
				(walk_state->opcode == AML_STORE_OP) &&
				(walk_state->operands[0]->common.type == ACPI_TYPE_LOCAL_REFERENCE) &&
				(walk_state->operands[1]->common.type == ACPI_TYPE_LOCAL_REFERENCE) &&
				(walk_state->operands[0]->reference.opcode ==
				 walk_state->operands[1]->reference.opcode)) {
				status = AE_OK;
			}
			else {
				ACPI_DEBUG_PRINT ((ACPI_DB_ERROR,
					"[%s]: Could not resolve operands, %s\n",
					acpi_ps_get_opcode_name (walk_state->opcode),
					acpi_format_exception (status)));
			}
		}

		/* Always delete the argument objects and clear the operand stack */

		acpi_ds_clear_operands (walk_state);

		/*
		 * If a result object was returned from above, push it on the
		 * current result stack
		 */
		if (ACPI_SUCCESS (status) &&
			walk_state->result_obj) {
			status = acpi_ds_result_push (walk_state->result_obj, walk_state);
		}

		break;


	default:

		switch (op_type) {
		case AML_TYPE_CONTROL:    /* Type 1 opcode, IF/ELSE/WHILE/NOOP */

			/* 1 Operand, 0 external_result, 0 internal_result */

			status = acpi_ds_exec_end_control_op (walk_state, op);
			if (ACPI_FAILURE (status)) {
				break;
			}

			status = acpi_ds_result_stack_pop (walk_state);
			break;


		case AML_TYPE_METHOD_CALL:

			ACPI_DEBUG_PRINT ((ACPI_DB_DISPATCH, "Method invocation, Op=%p\n", op));

			/*
			 * (AML_METHODCALL) Op->Value->Arg->Node contains
			 * the method Node pointer
			 */
			/* next_op points to the op that holds the method name */

			next_op = first_arg;

			/* next_op points to first argument op */

			next_op = next_op->common.next;

			/*
			 * Get the method's arguments and put them on the operand stack
			 */
			status = acpi_ds_create_operands (walk_state, next_op);
			if (ACPI_FAILURE (status)) {
				break;
			}

			/*
			 * Since the operands will be passed to another control method,
			 * we must resolve all local references here (Local variables,
			 * arguments to *this* method, etc.)
			 */
			status = acpi_ds_resolve_operands (walk_state);
			if (ACPI_FAILURE (status)) {
				/* On error, clear all resolved operands */

				acpi_ds_clear_operands (walk_state);
				break;
			}

			/*
			 * Tell the walk loop to preempt this running method and
			 * execute the new method
			 */
			status = AE_CTRL_TRANSFER;

			/*
			 * Return now; we don't want to disturb anything,
			 * especially the operand count!
			 */
			return_ACPI_STATUS (status);


		case AML_TYPE_CREATE_FIELD:

			ACPI_DEBUG_PRINT ((ACPI_DB_EXEC,
				"Executing create_field Buffer/Index Op=%p\n", op));

			status = acpi_ds_load2_end_op (walk_state);
			if (ACPI_FAILURE (status)) {
				break;
			}

			status = acpi_ds_eval_buffer_field_operands (walk_state, op);
			break;


		case AML_TYPE_CREATE_OBJECT:

			ACPI_DEBUG_PRINT ((ACPI_DB_EXEC,
				"Executing create_object (Buffer/Package) Op=%p\n", op));

			switch (op->common.parent->common.aml_opcode) {
			case AML_NAME_OP:

				/*
				 * Put the Node on the object stack (Contains the ACPI Name of
				 * this object)
				 */
				walk_state->operands[0] = (void *) op->common.parent->common.node;
				walk_state->num_operands = 1;

				status = acpi_ds_create_node (walk_state, op->common.parent->common.node, op->common.parent);
				if (ACPI_FAILURE (status)) {
					break;
				}

				/* Fall through */
				/*lint -fallthrough */

			case AML_INT_EVAL_SUBTREE_OP:

				status = acpi_ds_eval_data_object_operands (walk_state, op,
						  acpi_ns_get_attached_object (op->common.parent->common.node));
				break;

			default:

				status = acpi_ds_eval_data_object_operands (walk_state, op, NULL);
				break;
			}

			/*
			 * If a result object was returned from above, push it on the
			 * current result stack
			 */
			if (ACPI_SUCCESS (status) &&
				walk_state->result_obj) {
				status = acpi_ds_result_push (walk_state->result_obj, walk_state);
			}
			break;


		case AML_TYPE_NAMED_FIELD:
		case AML_TYPE_NAMED_COMPLEX:
		case AML_TYPE_NAMED_SIMPLE:
		case AML_TYPE_NAMED_NO_OBJ:

			status = acpi_ds_load2_end_op (walk_state);
			if (ACPI_FAILURE (status)) {
				break;
			}

			if (op->common.aml_opcode == AML_REGION_OP) {
				ACPI_DEBUG_PRINT ((ACPI_DB_EXEC,
					"Executing op_region Address/Length Op=%p\n", op));

				status = acpi_ds_eval_region_operands (walk_state, op);
				if (ACPI_FAILURE (status)) {
					break;
				}

				status = acpi_ds_result_stack_pop (walk_state);
			}

			break;


		case AML_TYPE_UNDEFINED:

			ACPI_DEBUG_PRINT ((ACPI_DB_ERROR, "Undefined opcode type Op=%p\n", op));
			return_ACPI_STATUS (AE_NOT_IMPLEMENTED);


		case AML_TYPE_BOGUS:

			ACPI_DEBUG_PRINT ((ACPI_DB_DISPATCH,
				"Internal opcode=%X type Op=%p\n",
				walk_state->opcode, op));
			break;


		default:

			ACPI_DEBUG_PRINT ((ACPI_DB_ERROR,
				"Unimplemented opcode, class=%X type=%X Opcode=%X Op=%p\n",
				op_class, op_type, op->common.aml_opcode, op));

			status = AE_NOT_IMPLEMENTED;
			break;
		}
	}

	/*
	 * ACPI 2.0 support for 64-bit integers: Truncate numeric
	 * result value if we are executing from a 32-bit ACPI table
	 */
	acpi_ex_truncate_for32bit_table (walk_state->result_obj);

	/*
	 * Check if we just completed the evaluation of a
	 * conditional predicate
	 */

	if ((walk_state->control_state) &&
		(walk_state->control_state->common.state ==
			ACPI_CONTROL_PREDICATE_EXECUTING) &&
		(walk_state->control_state->control.predicate_op == op)) {
		status = acpi_ds_get_predicate_value (walk_state, walk_state->result_obj);
		walk_state->result_obj = NULL;
	}


cleanup:
	if (walk_state->result_obj) {
		/* Break to debugger to display result */

		ACPI_DEBUGGER_EXEC (acpi_db_display_result_object (walk_state->result_obj, walk_state));

		/*
		 * Delete the result op if and only if:
		 * Parent will not use the result -- such as any
		 * non-nested type2 op in a method (parent will be method)
		 */
		acpi_ds_delete_result_if_not_used (op, walk_state->result_obj, walk_state);
	}

#ifdef _UNDER_DEVELOPMENT

	if (walk_state->parser_state.aml == walk_state->parser_state.aml_end) {
		acpi_db_method_end (walk_state);
	}
#endif

	/* Always clear the object stack */

	walk_state->num_operands = 0;

#ifdef ACPI_DISASSEMBLER

	/* On error, display method locals/args */

	if (ACPI_FAILURE (status)) {
		acpi_dm_dump_method_info (status, walk_state, op);
	}
#endif

	return_ACPI_STATUS (status);
}


