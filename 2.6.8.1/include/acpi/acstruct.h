/******************************************************************************
 *
 * Name: acstruct.h - Internal structs
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

#ifndef __ACSTRUCT_H__
#define __ACSTRUCT_H__


/*****************************************************************************
 *
 * Tree walking typedefs and structs
 *
 ****************************************************************************/


/*
 * Walk state - current state of a parse tree walk.  Used for both a leisurely stroll through
 * the tree (for whatever reason), and for control method execution.
 */

#define ACPI_NEXT_OP_DOWNWARD       1
#define ACPI_NEXT_OP_UPWARD         2

#define ACPI_WALK_NON_METHOD        0
#define ACPI_WALK_METHOD            1
#define ACPI_WALK_METHOD_RESTART    2
#define ACPI_WALK_CONST_REQUIRED    3
#define ACPI_WALK_CONST_OPTIONAL    4

struct acpi_walk_state
{
	u8                                  data_type;                          /* To differentiate various internal objs MUST BE FIRST!*/\
	acpi_owner_id                       owner_id;                           /* Owner of objects created during the walk */
	u8                                  last_predicate;                     /* Result of last predicate */
	u8                                  current_result;                     /* */
	u8                                  next_op_info;                       /* Info about next_op */
	u8                                  num_operands;                       /* Stack pointer for Operands[] array */
	u8                                  return_used;
	u8                                  walk_type;
	u16                                 opcode;                             /* Current AML opcode */
	u8                                  scope_depth;
	u8                                  reserved1;
	u32                                 arg_count;                          /* push for fixed or var args */
	u32                                 aml_offset;
	u32                                 arg_types;
	u32                                 method_breakpoint;                  /* For single stepping */
	u32                                 user_breakpoint;                    /* User AML breakpoint */
	u32                                 parse_flags;
	u32                                 prev_arg_types;

	u8                                  *aml_last_while;
	struct acpi_namespace_node          arguments[ACPI_METHOD_NUM_ARGS];    /* Control method arguments */
	union acpi_operand_object           **caller_return_desc;
	union acpi_generic_state            *control_state;                     /* List of control states (nested IFs) */
	struct acpi_namespace_node          *deferred_node;                      /* Used when executing deferred opcodes */
	struct acpi_namespace_node          local_variables[ACPI_METHOD_NUM_LOCALS];    /* Control method locals */
	struct acpi_namespace_node          *method_call_node;                  /* Called method Node*/
	union acpi_parse_object             *method_call_op;                    /* method_call Op if running a method */
	union acpi_operand_object           *method_desc;                       /* Method descriptor if running a method */
	struct acpi_namespace_node          *method_node;                       /* Method node if running a method. */
	union acpi_parse_object             *op;                                /* Current parser op */
	union acpi_operand_object           *operands[ACPI_OBJ_NUM_OPERANDS+1]; /* Operands passed to the interpreter (+1 for NULL terminator) */
	const struct acpi_opcode_info       *op_info;                           /* Info on current opcode */
	union acpi_parse_object             *origin;                            /* Start of walk [Obsolete] */
	union acpi_operand_object           **params;
	struct acpi_parse_state             parser_state;                       /* Current state of parser */
	union acpi_operand_object           *result_obj;
	union acpi_generic_state            *results;                           /* Stack of accumulated results */
	union acpi_operand_object           *return_desc;                       /* Return object, if any */
	union acpi_generic_state            *scope_info;                        /* Stack of nested scopes */

	union acpi_parse_object             *prev_op;                           /* Last op that was processed */
	union acpi_parse_object             *next_op;                           /* next op to be processed */
	acpi_parse_downwards                descending_callback;
	acpi_parse_upwards                  ascending_callback;
	struct acpi_thread_state            *thread;
	struct acpi_walk_state              *next;                              /* Next walk_state in list */
};


/* Info used by acpi_ps_init_objects */

struct acpi_init_walk_info
{
	u16                             method_count;
	u16                             device_count;
	u16                             op_region_count;
	u16                             field_count;
	u16                             buffer_count;
	u16                             package_count;
	u16                             op_region_init;
	u16                             field_init;
	u16                             buffer_init;
	u16                             package_init;
	u16                             object_count;
	struct acpi_table_desc          *table_desc;
};


/* Info used by acpi_ns_initialize_devices */

struct acpi_device_walk_info
{
	u16                             device_count;
	u16                             num_STA;
	u16                             num_INI;
	struct acpi_table_desc          *table_desc;
};


/* TBD: [Restructure] Merge with struct above */

struct acpi_walk_info
{
	u32                             debug_level;
	u32                             owner_id;
	u8                              display_type;
};

/* Display Types */

#define ACPI_DISPLAY_SUMMARY    0
#define ACPI_DISPLAY_OBJECTS    1

struct acpi_get_devices_info
{
	acpi_walk_callback              user_function;
	void                            *context;
	char                            *hid;
};


union acpi_aml_operands
{
	union acpi_operand_object           *operands[7];

	struct
	{
		struct acpi_object_integer      *type;
		struct acpi_object_integer      *code;
		struct acpi_object_integer      *argument;

	} fatal;

	struct
	{
		union acpi_operand_object       *source;
		struct acpi_object_integer      *index;
		union acpi_operand_object       *target;

	} index;

	struct
	{
		union acpi_operand_object       *source;
		struct acpi_object_integer      *index;
		struct acpi_object_integer      *length;
		union acpi_operand_object       *target;

	} mid;
};


#endif
