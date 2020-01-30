/******************************************************************************
 *
 * Name: aclocal.h - Internal data types used across the ACPI subsystem
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

#ifndef __ACLOCAL_H__
#define __ACLOCAL_H__


#define ACPI_WAIT_FOREVER               0xFFFF  /* u16, as per ACPI spec */

typedef void *                                  acpi_mutex;
typedef u32                                     acpi_mutex_handle;


/* Total number of aml opcodes defined */

#define AML_NUM_OPCODES                 0x7E


/*****************************************************************************
 *
 * Mutex typedefs and structs
 *
 ****************************************************************************/


/*
 * Predefined handles for the mutex objects used within the subsystem
 * All mutex objects are automatically created by acpi_ut_mutex_initialize.
 *
 * The acquire/release ordering protocol is implied via this list.  Mutexes
 * with a lower value must be acquired before mutexes with a higher value.
 *
 * NOTE: any changes here must be reflected in the acpi_gbl_mutex_names table also!
 */

#define ACPI_MTX_EXECUTE                0
#define ACPI_MTX_INTERPRETER            1
#define ACPI_MTX_PARSER                 2
#define ACPI_MTX_DISPATCHER             3
#define ACPI_MTX_TABLES                 4
#define ACPI_MTX_OP_REGIONS             5
#define ACPI_MTX_NAMESPACE              6
#define ACPI_MTX_EVENTS                 7
#define ACPI_MTX_HARDWARE               8
#define ACPI_MTX_CACHES                 9
#define ACPI_MTX_MEMORY                 10
#define ACPI_MTX_DEBUG_CMD_COMPLETE     11
#define ACPI_MTX_DEBUG_CMD_READY        12

#define MAX_MUTEX                       12
#define NUM_MUTEX                       MAX_MUTEX+1


#if defined(ACPI_DEBUG_OUTPUT) || defined(ACPI_DEBUGGER)
#ifdef DEFINE_ACPI_GLOBALS

/* Names for the mutexes used in the subsystem */

static char                         *acpi_gbl_mutex_names[] =
{
	"ACPI_MTX_Execute",
	"ACPI_MTX_Interpreter",
	"ACPI_MTX_Parser",
	"ACPI_MTX_Dispatcher",
	"ACPI_MTX_Tables",
	"ACPI_MTX_op_regions",
	"ACPI_MTX_Namespace",
	"ACPI_MTX_Events",
	"ACPI_MTX_Hardware",
	"ACPI_MTX_Caches",
	"ACPI_MTX_Memory",
	"ACPI_MTX_debug_cmd_complete",
	"ACPI_MTX_debug_cmd_ready",
};

#endif
#endif


/* Table for the global mutexes */

struct acpi_mutex_info
{
	acpi_mutex                          mutex;
	u32                                 use_count;
	u32                                 owner_id;
};

/* This owner ID means that the mutex is not in use (unlocked) */

#define ACPI_MUTEX_NOT_ACQUIRED         (u32) (-1)


/* Lock flag parameter for various interfaces */

#define ACPI_MTX_DO_NOT_LOCK            0
#define ACPI_MTX_LOCK                   1


typedef u16                                     acpi_owner_id;
#define ACPI_OWNER_TYPE_TABLE           0x0
#define ACPI_OWNER_TYPE_METHOD          0x1
#define ACPI_FIRST_METHOD_ID            0x0001
#define ACPI_FIRST_TABLE_ID             0xF000


/* Field access granularities */

#define ACPI_FIELD_BYTE_GRANULARITY     1
#define ACPI_FIELD_WORD_GRANULARITY     2
#define ACPI_FIELD_DWORD_GRANULARITY    4
#define ACPI_FIELD_QWORD_GRANULARITY    8

/*****************************************************************************
 *
 * Namespace typedefs and structs
 *
 ****************************************************************************/


/* Operational modes of the AML interpreter/scanner */

typedef enum
{
	ACPI_IMODE_LOAD_PASS1               = 0x01,
	ACPI_IMODE_LOAD_PASS2               = 0x02,
	ACPI_IMODE_EXECUTE                  = 0x0E

} acpi_interpreter_mode;


/*
 * The Node describes a named object that appears in the AML
 * An acpi_node is used to store Nodes.
 *
 * data_type is used to differentiate between internal descriptors, and MUST
 * be the first byte in this structure.
 */

union acpi_name_union
{
	u32                                 integer;
	char                                ascii[4];
};

struct acpi_namespace_node
{
	u8                                  descriptor;     /* Used to differentiate object descriptor types */
	u8                                  type;           /* Type associated with this name */
	u16                                 owner_id;
	union acpi_name_union               name;           /* ACPI Name, always 4 chars per ACPI spec */


	union acpi_operand_object           *object;        /* Pointer to attached ACPI object (optional) */
	struct acpi_namespace_node          *child;         /* First child */
	struct acpi_namespace_node          *peer;          /* Next peer*/
	u16                                 reference_count; /* Current count of references and children */
	u8                                  flags;
};


#define ACPI_ENTRY_NOT_FOUND            NULL


/* Node flags */

#define ANOBJ_RESERVED                  0x01
#define ANOBJ_END_OF_PEER_LIST          0x02
#define ANOBJ_DATA_WIDTH_32             0x04     /* Parent table is 64-bits */
#define ANOBJ_METHOD_ARG                0x08
#define ANOBJ_METHOD_LOCAL              0x10
#define ANOBJ_METHOD_NO_RETVAL          0x20
#define ANOBJ_METHOD_SOME_NO_RETVAL     0x40

#define ANOBJ_IS_BIT_OFFSET             0x80


/*
 * ACPI Table Descriptor.  One per ACPI table
 */
struct acpi_table_desc
{
	struct acpi_table_desc          *prev;
	struct acpi_table_desc          *next;
	struct acpi_table_desc          *installed_desc;
	struct acpi_table_header        *pointer;
	u8                              *aml_start;
	u64                             physical_address;
	u32                             aml_length;
	acpi_size                       length;
	acpi_owner_id                   table_id;
	u8                              type;
	u8                              allocation;
	u8                              loaded_into_namespace;
};

struct acpi_table_list
{
	struct acpi_table_desc          *next;
	u32                             count;
};


struct acpi_find_context
{
	char                            *search_for;
	acpi_handle                     *list;
	u32                             *count;
};


struct acpi_ns_search_data
{
	struct acpi_namespace_node      *node;
};


/*
 * Predefined Namespace items
 */
struct acpi_predefined_names
{
	char                            *name;
	u8                              type;
	char                            *val;
};


/* Object types used during package copies */


#define ACPI_COPY_TYPE_SIMPLE           0
#define ACPI_COPY_TYPE_PACKAGE          1

/* Info structure used to convert external<->internal namestrings */

struct acpi_namestring_info
{
	char                            *external_name;
	char                            *next_external_char;
	char                            *internal_name;
	u32                             length;
	u32                             num_segments;
	u32                             num_carats;
	u8                              fully_qualified;
};


/* Field creation info */

struct acpi_create_field_info
{
	struct acpi_namespace_node      *region_node;
	struct acpi_namespace_node      *field_node;
	struct acpi_namespace_node      *register_node;
	struct acpi_namespace_node      *data_register_node;
	u32                             bank_value;
	u32                             field_bit_position;
	u32                             field_bit_length;
	u8                              field_flags;
	u8                              attribute;
	u8                              field_type;
};


/*****************************************************************************
 *
 * Event typedefs and structs
 *
 ****************************************************************************/

/* Information about a GPE, one per each GPE in an array */

struct acpi_gpe_event_info
{
	struct acpi_namespace_node              *method_node;   /* Method node for this GPE level */
	acpi_gpe_handler                        handler;        /* Address of handler, if any */
	void                                    *context;       /* Context to be passed to handler */
	struct acpi_gpe_register_info           *register_info; /* Backpointer to register info */
	u8                                      flags;          /* Level or Edge */
	u8                                      bit_mask;       /* This GPE within the register */
};

/* Information about a GPE register pair, one per each status/enable pair in an array */

struct acpi_gpe_register_info
{
	struct acpi_generic_address             status_address; /* Address of status reg */
	struct acpi_generic_address             enable_address; /* Address of enable reg */
	u8                                      status;         /* Current value of status reg */
	u8                                      enable;         /* Current value of enable reg */
	u8                                      wake_enable;    /* Mask of bits to keep enabled when sleeping */
	u8                                      base_gpe_number; /* Base GPE number for this register */
};

/*
 * Information about a GPE register block, one per each installed block --
 * GPE0, GPE1, and one per each installed GPE Block Device.
 */
struct acpi_gpe_block_info
{
	struct acpi_gpe_block_info              *previous;
	struct acpi_gpe_block_info              *next;
	struct acpi_gpe_xrupt_info              *xrupt_block;   /* Backpointer to interrupt block */
	struct acpi_gpe_register_info           *register_info; /* One per GPE register pair */
	struct acpi_gpe_event_info              *event_info;    /* One for each GPE */
	struct acpi_generic_address             block_address;  /* Base address of the block */
	u32                                     register_count; /* Number of register pairs in block */
	u8                                      block_base_number;/* Base GPE number for this block */
};

/* Information about GPE interrupt handlers, one per each interrupt level used for GPEs */

struct acpi_gpe_xrupt_info
{
	struct acpi_gpe_xrupt_info              *previous;
	struct acpi_gpe_xrupt_info              *next;
	struct acpi_gpe_block_info              *gpe_block_list_head; /* List of GPE blocks for this xrupt */
	u32                                     interrupt_level;    /* System interrupt level */
};


struct acpi_gpe_walk_info
{
	struct acpi_namespace_node              *gpe_device;
	struct acpi_gpe_block_info              *gpe_block;
};


typedef acpi_status (*ACPI_GPE_CALLBACK) (
	struct acpi_gpe_xrupt_info      *gpe_xrupt_info,
	struct acpi_gpe_block_info      *gpe_block);


/* Information about each particular fixed event */

struct acpi_fixed_event_handler
{
	acpi_event_handler              handler;        /* Address of handler. */
	void                            *context;       /* Context to be passed to handler */
};

struct acpi_fixed_event_info
{
	u8                              status_register_id;
	u8                              enable_register_id;
	u16                             status_bit_mask;
	u16                             enable_bit_mask;
};

/* Information used during field processing */

struct acpi_field_info
{
	u8                              skip_field;
	u8                              field_flag;
	u32                             pkg_length;
};


/*****************************************************************************
 *
 * Generic "state" object for stacks
 *
 ****************************************************************************/


#define ACPI_CONTROL_NORMAL                  0xC0
#define ACPI_CONTROL_CONDITIONAL_EXECUTING   0xC1
#define ACPI_CONTROL_PREDICATE_EXECUTING     0xC2
#define ACPI_CONTROL_PREDICATE_FALSE         0xC3
#define ACPI_CONTROL_PREDICATE_TRUE          0xC4


/* Forward declarations */
struct acpi_walk_state        ;
struct acpi_obj_mutex;
union acpi_parse_object        ;


#define ACPI_STATE_COMMON                  /* Two 32-bit fields and a pointer */\
	u8                                  data_type;          /* To differentiate various internal objs */\
	u8                                  flags;      \
	u16                                 value;      \
	u16                                 state;      \
	u16                                 reserved;   \
	void                                *next;      \

struct acpi_common_state
{
	ACPI_STATE_COMMON
};


/*
 * Update state - used to traverse complex objects such as packages
 */
struct acpi_update_state
{
	ACPI_STATE_COMMON
	union acpi_operand_object           *object;
};


/*
 * Pkg state - used to traverse nested package structures
 */
struct acpi_pkg_state
{
	ACPI_STATE_COMMON
	union acpi_operand_object           *source_object;
	union acpi_operand_object           *dest_object;
	struct acpi_walk_state              *walk_state;
	void                                *this_target_obj;
	u32                                 num_packages;
	u16                                 index;
};


/*
 * Control state - one per if/else and while constructs.
 * Allows nesting of these constructs
 */
struct acpi_control_state
{
	ACPI_STATE_COMMON
	union acpi_parse_object             *predicate_op;
	u8                                  *aml_predicate_start;   /* Start of if/while predicate */
	u8                                  *package_end;           /* End of if/while block */
	u16                                 opcode;
};


/*
 * Scope state - current scope during namespace lookups
 */
struct acpi_scope_state
{
	ACPI_STATE_COMMON
	struct acpi_namespace_node          *node;
};


struct acpi_pscope_state
{
	ACPI_STATE_COMMON
	union acpi_parse_object             *op;                    /* Current op being parsed */
	u8                                  *arg_end;               /* Current argument end */
	u8                                  *pkg_end;               /* Current package end */
	u32                                 arg_list;               /* Next argument to parse */
	u32                                 arg_count;              /* Number of fixed arguments */
};


/*
 * Thread state - one per thread across multiple walk states.  Multiple walk
 * states are created when there are nested control methods executing.
 */
struct acpi_thread_state
{
	ACPI_STATE_COMMON
	struct acpi_walk_state              *walk_state_list;       /* Head of list of walk_states for this thread */
	union acpi_operand_object           *acquired_mutex_list;   /* List of all currently acquired mutexes */
	u32                                 thread_id;              /* Running thread ID */
	u16                                 current_sync_level;     /* Mutex Sync (nested acquire) level */
};


/*
 * Result values - used to accumulate the results of nested
 * AML arguments
 */
struct acpi_result_values
{
	ACPI_STATE_COMMON
	union acpi_operand_object           *obj_desc [ACPI_OBJ_NUM_OPERANDS];
	u8                                  num_results;
	u8                                  last_insert;
};


typedef
acpi_status (*acpi_parse_downwards) (
	struct acpi_walk_state              *walk_state,
	union acpi_parse_object             **out_op);

typedef
acpi_status (*acpi_parse_upwards) (
	struct acpi_walk_state              *walk_state);


/*
 * Notify info - used to pass info to the deferred notify
 * handler/dispatcher.
 */
struct acpi_notify_info
{
	ACPI_STATE_COMMON
	struct acpi_namespace_node          *node;
	union acpi_operand_object           *handler_obj;
};


/* Generic state is union of structs above */

union acpi_generic_state
{
	struct acpi_common_state            common;
	struct acpi_control_state           control;
	struct acpi_update_state            update;
	struct acpi_scope_state             scope;
	struct acpi_pscope_state            parse_scope;
	struct acpi_pkg_state               pkg;
	struct acpi_thread_state            thread;
	struct acpi_result_values           results;
	struct acpi_notify_info             notify;
};


/*****************************************************************************
 *
 * Interpreter typedefs and structs
 *
 ****************************************************************************/

typedef
acpi_status (*ACPI_EXECUTE_OP) (
	struct acpi_walk_state              *walk_state);


/*****************************************************************************
 *
 * Parser typedefs and structs
 *
 ****************************************************************************/

/*
 * AML opcode, name, and argument layout
 */
struct acpi_opcode_info
{
#if defined(ACPI_DISASSEMBLER) || defined(ACPI_DEBUG_OUTPUT)
	char                                *name;          /* Opcode name (disassembler/debug only) */
#endif
	u32                                 parse_args;     /* Grammar/Parse time arguments */
	u32                                 runtime_args;   /* Interpret time arguments */
	u32                                 flags;          /* Misc flags */
	u8                                  object_type;    /* Corresponding internal object type */
	u8                                  class;          /* Opcode class */
	u8                                  type;           /* Opcode type */
};


union acpi_parse_value
{
	acpi_integer                        integer;        /* Integer constant (Up to 64 bits) */
	struct uint64_struct                integer64;      /* Structure overlay for 2 32-bit Dwords */
	u32                                 size;           /* bytelist or field size */
	char                                *string;        /* NULL terminated string */
	u8                                  *buffer;        /* buffer or string */
	char                                *name;          /* NULL terminated string */
	union acpi_parse_object             *arg;           /* arguments and contained ops */
};


#define ACPI_PARSE_COMMON \
	u8                                  data_type;      /* To differentiate various internal objs */\
	u8                                  flags;          /* Type of Op */\
	u16                                 aml_opcode;     /* AML opcode */\
	u32                                 aml_offset;     /* Offset of declaration in AML */\
	union acpi_parse_object             *parent;        /* Parent op */\
	union acpi_parse_object             *next;          /* Next op */\
	ACPI_DISASM_ONLY_MEMBERS (\
	u8                                  disasm_flags;   /* Used during AML disassembly */\
	u8                                  disasm_opcode;  /* Subtype used for disassembly */\
	char                                aml_op_name[16]) /* Op name (debug only) */\
			   /* NON-DEBUG members below: */\
	struct acpi_namespace_node          *node;          /* For use by interpreter */\
	union acpi_parse_value              value;          /* Value or args associated with the opcode */\


#define ACPI_DASM_BUFFER        0x00
#define ACPI_DASM_RESOURCE      0x01
#define ACPI_DASM_STRING        0x02
#define ACPI_DASM_UNICODE       0x03
#define ACPI_DASM_EISAID        0x04
#define ACPI_DASM_MATCHOP       0x05

/*
 * generic operation (for example:  If, While, Store)
 */
struct acpi_parse_obj_common
{
	ACPI_PARSE_COMMON
};


/*
 * Extended Op for named ops (Scope, Method, etc.), deferred ops (Methods and op_regions),
 * and bytelists.
 */
struct acpi_parse_obj_named
{
	ACPI_PARSE_COMMON
	u8                                  *path;
	u8                                  *data;          /* AML body or bytelist data */
	u32                                 length;         /* AML length */
	u32                                 name;           /* 4-byte name or zero if no name */
};


/* The parse node is the fundamental element of the parse tree */

struct acpi_parse_obj_asl
{
	ACPI_PARSE_COMMON
	union acpi_parse_object             *child;
	union acpi_parse_object             *parent_method;
	char                                *filename;
	char                                *external_name;
	char                                *namepath;
	char                                name_seg[4];
	u32                                 extra_value;
	u32                                 column;
	u32                                 line_number;
	u32                                 logical_line_number;
	u32                                 logical_byte_offset;
	u32                                 end_line;
	u32                                 end_logical_line;
	u32                                 acpi_btype;
	u32                                 aml_length;
	u32                                 aml_subtree_length;
	u32                                 final_aml_length;
	u32                                 final_aml_offset;
	u32                                 compile_flags;
	u16                                 parse_opcode;
	u8                                  aml_opcode_length;
	u8                                  aml_pkg_len_bytes;
	u8                                  extra;
	char                                parse_op_name[12];
};


union acpi_parse_object
{
	struct acpi_parse_obj_common        common;
	struct acpi_parse_obj_named         named;
	struct acpi_parse_obj_asl           asl;
};


/*
 * Parse state - one state per parser invocation and each control
 * method.
 */
struct acpi_parse_state
{
	u32                                 aml_size;
	u8                                  *aml_start;     /* First AML byte */
	u8                                  *aml;           /* Next AML byte */
	u8                                  *aml_end;       /* (last + 1) AML byte */
	u8                                  *pkg_start;     /* Current package begin */
	u8                                  *pkg_end;       /* Current package end */
	union acpi_parse_object             *start_op;      /* Root of parse tree */
	struct acpi_namespace_node          *start_node;
	union acpi_generic_state            *scope;         /* Current scope */
	union acpi_parse_object             *start_scope;
};


/* Parse object flags */

#define ACPI_PARSEOP_GENERIC                    0x01
#define ACPI_PARSEOP_NAMED                      0x02
#define ACPI_PARSEOP_DEFERRED                   0x04
#define ACPI_PARSEOP_BYTELIST                   0x08
#define ACPI_PARSEOP_IN_CACHE                   0x80

/* Parse object disasm_flags */

#define ACPI_PARSEOP_IGNORE                     0x01
#define ACPI_PARSEOP_PARAMLIST                  0x02
#define ACPI_PARSEOP_EMPTY_TERMLIST             0x04
#define ACPI_PARSEOP_SPECIAL                    0x10


/*****************************************************************************
 *
 * Hardware (ACPI registers) and PNP
 *
 ****************************************************************************/

#define PCI_ROOT_HID_STRING         "PNP0A03"

struct acpi_bit_register_info
{
	u8                                  parent_register;
	u8                                  bit_position;
	u16                                 access_bit_mask;
};


/*
 * Register IDs
 * These are the full ACPI registers
 */
#define ACPI_REGISTER_PM1_STATUS                0x01
#define ACPI_REGISTER_PM1_ENABLE                0x02
#define ACPI_REGISTER_PM1_CONTROL               0x03
#define ACPI_REGISTER_PM1A_CONTROL              0x04
#define ACPI_REGISTER_PM1B_CONTROL              0x05
#define ACPI_REGISTER_PM2_CONTROL               0x06
#define ACPI_REGISTER_PM_TIMER                  0x07
#define ACPI_REGISTER_PROCESSOR_BLOCK           0x08
#define ACPI_REGISTER_SMI_COMMAND_BLOCK         0x09


/* Masks used to access the bit_registers */

#define ACPI_BITMASK_TIMER_STATUS               0x0001
#define ACPI_BITMASK_BUS_MASTER_STATUS          0x0010
#define ACPI_BITMASK_GLOBAL_LOCK_STATUS         0x0020
#define ACPI_BITMASK_POWER_BUTTON_STATUS        0x0100
#define ACPI_BITMASK_SLEEP_BUTTON_STATUS        0x0200
#define ACPI_BITMASK_RT_CLOCK_STATUS            0x0400
#define ACPI_BITMASK_WAKE_STATUS                0x8000

#define ACPI_BITMASK_ALL_FIXED_STATUS           (ACPI_BITMASK_TIMER_STATUS          | \
			 ACPI_BITMASK_BUS_MASTER_STATUS     | \
			 ACPI_BITMASK_GLOBAL_LOCK_STATUS    | \
			 ACPI_BITMASK_POWER_BUTTON_STATUS   | \
			 ACPI_BITMASK_SLEEP_BUTTON_STATUS   | \
			 ACPI_BITMASK_RT_CLOCK_STATUS       | \
			 ACPI_BITMASK_WAKE_STATUS)

#define ACPI_BITMASK_TIMER_ENABLE               0x0001
#define ACPI_BITMASK_GLOBAL_LOCK_ENABLE         0x0020
#define ACPI_BITMASK_POWER_BUTTON_ENABLE        0x0100
#define ACPI_BITMASK_SLEEP_BUTTON_ENABLE        0x0200
#define ACPI_BITMASK_RT_CLOCK_ENABLE            0x0400

#define ACPI_BITMASK_SCI_ENABLE                 0x0001
#define ACPI_BITMASK_BUS_MASTER_RLD             0x0002
#define ACPI_BITMASK_GLOBAL_LOCK_RELEASE        0x0004
#define ACPI_BITMASK_SLEEP_TYPE_X               0x1C00
#define ACPI_BITMASK_SLEEP_ENABLE               0x2000

#define ACPI_BITMASK_ARB_DISABLE                0x0001


/* Raw bit position of each bit_register */

#define ACPI_BITPOSITION_TIMER_STATUS           0x00
#define ACPI_BITPOSITION_BUS_MASTER_STATUS      0x04
#define ACPI_BITPOSITION_GLOBAL_LOCK_STATUS     0x05
#define ACPI_BITPOSITION_POWER_BUTTON_STATUS    0x08
#define ACPI_BITPOSITION_SLEEP_BUTTON_STATUS    0x09
#define ACPI_BITPOSITION_RT_CLOCK_STATUS        0x0A
#define ACPI_BITPOSITION_WAKE_STATUS            0x0F

#define ACPI_BITPOSITION_TIMER_ENABLE           0x00
#define ACPI_BITPOSITION_GLOBAL_LOCK_ENABLE     0x05
#define ACPI_BITPOSITION_POWER_BUTTON_ENABLE    0x08
#define ACPI_BITPOSITION_SLEEP_BUTTON_ENABLE    0x09
#define ACPI_BITPOSITION_RT_CLOCK_ENABLE        0x0A

#define ACPI_BITPOSITION_SCI_ENABLE             0x00
#define ACPI_BITPOSITION_BUS_MASTER_RLD         0x01
#define ACPI_BITPOSITION_GLOBAL_LOCK_RELEASE    0x02
#define ACPI_BITPOSITION_SLEEP_TYPE_X           0x0A
#define ACPI_BITPOSITION_SLEEP_ENABLE           0x0D

#define ACPI_BITPOSITION_ARB_DISABLE            0x00


/*****************************************************************************
 *
 * Resource descriptors
 *
 ****************************************************************************/


/* resource_type values */

#define ACPI_RESOURCE_TYPE_MEMORY_RANGE         0
#define ACPI_RESOURCE_TYPE_IO_RANGE             1
#define ACPI_RESOURCE_TYPE_BUS_NUMBER_RANGE     2

/* Resource descriptor types and masks */

#define ACPI_RDESC_TYPE_LARGE                   0x80
#define ACPI_RDESC_TYPE_SMALL                   0x00

#define ACPI_RDESC_TYPE_MASK                    0x80
#define ACPI_RDESC_SMALL_MASK                   0x78 /* Only bits 6:3 contain the type */


/*
 * Small resource descriptor types
 * Note: The 3 length bits (2:0) must be zero
 */
#define ACPI_RDESC_TYPE_IRQ_FORMAT              0x20
#define ACPI_RDESC_TYPE_DMA_FORMAT              0x28
#define ACPI_RDESC_TYPE_START_DEPENDENT         0x30
#define ACPI_RDESC_TYPE_END_DEPENDENT           0x38
#define ACPI_RDESC_TYPE_IO_PORT                 0x40
#define ACPI_RDESC_TYPE_FIXED_IO_PORT           0x48
#define ACPI_RDESC_TYPE_SMALL_VENDOR            0x70
#define ACPI_RDESC_TYPE_END_TAG                 0x78

/*
 * Large resource descriptor types
 */

#define ACPI_RDESC_TYPE_MEMORY_24               0x81
#define ACPI_RDESC_TYPE_GENERAL_REGISTER        0x82
#define ACPI_RDESC_TYPE_LARGE_VENDOR            0x84
#define ACPI_RDESC_TYPE_MEMORY_32               0x85
#define ACPI_RDESC_TYPE_FIXED_MEMORY_32         0x86
#define ACPI_RDESC_TYPE_DWORD_ADDRESS_SPACE     0x87
#define ACPI_RDESC_TYPE_WORD_ADDRESS_SPACE      0x88
#define ACPI_RDESC_TYPE_EXTENDED_XRUPT          0x89
#define ACPI_RDESC_TYPE_QWORD_ADDRESS_SPACE     0x8A


/*****************************************************************************
 *
 * Miscellaneous
 *
 ****************************************************************************/

#define ACPI_ASCII_ZERO                      0x30


/*****************************************************************************
 *
 * Debugger
 *
 ****************************************************************************/

struct acpi_db_method_info
{
	acpi_handle                     thread_gate;
	char                            *name;
	char                            **args;
	u32                             flags;
	u32                             num_loops;
	char                            pathname[128];
};

struct acpi_integrity_info
{
	u32                         nodes;
	u32                         objects;
};


#define ACPI_DB_REDIRECTABLE_OUTPUT  0x01
#define ACPI_DB_CONSOLE_OUTPUT       0x02
#define ACPI_DB_DUPLICATE_OUTPUT     0x03


/*****************************************************************************
 *
 * Debug
 *
 ****************************************************************************/

struct acpi_debug_print_info
{
	u32                             component_id;
	char                            *proc_name;
	char                            *module_name;
};


/* Entry for a memory allocation (debug only) */

#define ACPI_MEM_MALLOC                      0
#define ACPI_MEM_CALLOC                      1
#define ACPI_MAX_MODULE_NAME                 16

#define ACPI_COMMON_DEBUG_MEM_HEADER \
	struct acpi_debug_mem_block         *previous; \
	struct acpi_debug_mem_block         *next; \
	u32                                 size; \
	u32                                 component; \
	u32                                 line; \
	char                                module[ACPI_MAX_MODULE_NAME]; \
	u8                                  alloc_type;

struct acpi_debug_mem_header
{
	ACPI_COMMON_DEBUG_MEM_HEADER
};

struct acpi_debug_mem_block
{
	ACPI_COMMON_DEBUG_MEM_HEADER
	u64                                 user_space;
};


#define ACPI_MEM_LIST_GLOBAL            0
#define ACPI_MEM_LIST_NSNODE            1

#define ACPI_MEM_LIST_FIRST_CACHE_LIST  2
#define ACPI_MEM_LIST_STATE             2
#define ACPI_MEM_LIST_PSNODE            3
#define ACPI_MEM_LIST_PSNODE_EXT        4
#define ACPI_MEM_LIST_OPERAND           5
#define ACPI_MEM_LIST_WALK              6
#define ACPI_MEM_LIST_MAX               6
#define ACPI_NUM_MEM_LISTS              7


struct acpi_memory_list
{
	void                                *list_head;
	u16                                 link_offset;
	u16                                 max_cache_depth;
	u16                                 cache_depth;
	u16                                 object_size;

#ifdef ACPI_DBG_TRACK_ALLOCATIONS

	/* Statistics for debug memory tracking only */

	u32                                 total_allocated;
	u32                                 total_freed;
	u32                                 current_total_size;
	u32                                 cache_requests;
	u32                                 cache_hits;
	char                                *list_name;
#endif
};


#endif /* __ACLOCAL_H__ */
