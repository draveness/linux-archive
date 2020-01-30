/******************************************************************************
 *
 * Module Name: utglobal - Global variables for the ACPI subsystem
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

#define DEFINE_ACPI_GLOBALS

#include <acpi/acpi.h>
#include <acpi/acnamesp.h>

#define _COMPONENT          ACPI_UTILITIES
	 ACPI_MODULE_NAME    ("utglobal")


/******************************************************************************
 *
 * FUNCTION:    acpi_format_exception
 *
 * PARAMETERS:  Status       - The acpi_status code to be formatted
 *
 * RETURN:      A string containing the exception  text
 *
 * DESCRIPTION: This function translates an ACPI exception into an ASCII string.
 *
 ******************************************************************************/

const char *
acpi_format_exception (
	acpi_status                     status)
{
	const char                      *exception = "UNKNOWN_STATUS_CODE";
	acpi_status                     sub_status;


	ACPI_FUNCTION_NAME ("format_exception");


	sub_status = (status & ~AE_CODE_MASK);

	switch (status & AE_CODE_MASK) {
	case AE_CODE_ENVIRONMENTAL:

		if (sub_status <= AE_CODE_ENV_MAX) {
			exception = acpi_gbl_exception_names_env [sub_status];
			break;
		}
		goto unknown;

	case AE_CODE_PROGRAMMER:

		if (sub_status <= AE_CODE_PGM_MAX) {
			exception = acpi_gbl_exception_names_pgm [sub_status -1];
			break;
		}
		goto unknown;

	case AE_CODE_ACPI_TABLES:

		if (sub_status <= AE_CODE_TBL_MAX) {
			exception = acpi_gbl_exception_names_tbl [sub_status -1];
			break;
		}
		goto unknown;

	case AE_CODE_AML:

		if (sub_status <= AE_CODE_AML_MAX) {
			exception = acpi_gbl_exception_names_aml [sub_status -1];
			break;
		}
		goto unknown;

	case AE_CODE_CONTROL:

		if (sub_status <= AE_CODE_CTRL_MAX) {
			exception = acpi_gbl_exception_names_ctrl [sub_status -1];
			break;
		}
		goto unknown;

	default:
		goto unknown;
	}


	return ((const char *) exception);

unknown:

	ACPI_DEBUG_PRINT ((ACPI_DB_ERROR, "Unknown exception code: 0x%8.8X\n", status));
	return ((const char *) exception);
}


/******************************************************************************
 *
 * Static global variable initialization.
 *
 ******************************************************************************/

/*
 * We want the debug switches statically initialized so they
 * are already set when the debugger is entered.
 */

/* Debug switch - level and trace mask */

#ifdef ACPI_DEBUG_OUTPUT
u32                                 acpi_dbg_level = ACPI_DEBUG_DEFAULT;
#else
u32                                 acpi_dbg_level = ACPI_NORMAL_DEFAULT;
#endif

/* Debug switch - layer (component) mask */

u32                                 acpi_dbg_layer = ACPI_COMPONENT_DEFAULT;
u32                                 acpi_gbl_nesting_level = 0;


/* Debugger globals */

u8                                  acpi_gbl_db_terminate_threads = FALSE;
u8                                  acpi_gbl_abort_method = FALSE;
u8                                  acpi_gbl_method_executing = FALSE;

/* System flags */

u32                                 acpi_gbl_startup_flags = 0;

/* System starts uninitialized */

u8                                  acpi_gbl_shutdown = TRUE;

const u8                            acpi_gbl_decode_to8bit [8] = {1,2,4,8,16,32,64,128};

const char                          *acpi_gbl_sleep_state_names[ACPI_S_STATE_COUNT] = {
			  "\\_S0_",
			  "\\_S1_",
			  "\\_S2_",
			  "\\_S3_",
			  "\\_S4_",
			  "\\_S5_"};

const char                          *acpi_gbl_highest_dstate_names[4] = {
					   "_S1D",
					   "_S2D",
					   "_S3D",
					   "_S4D"};

/* Strings supported by the _OSI predefined (internal) method */

const char                          *acpi_gbl_valid_osi_strings[ACPI_NUM_OSI_STRINGS] = {
							 "Linux",
							 "Windows 2000",
							 "Windows 2001",
							 "Windows 2001.1"};


/******************************************************************************
 *
 * Namespace globals
 *
 ******************************************************************************/


/*
 * Predefined ACPI Names (Built-in to the Interpreter)
 *
 * NOTES:
 * 1) _SB_ is defined to be a device to allow \_SB_._INI to be run
 *    during the initialization sequence.
 */
const struct acpi_predefined_names      acpi_gbl_pre_defined_names[] =
{ {"_GPE",    ACPI_TYPE_LOCAL_SCOPE,      NULL},
	{"_PR_",    ACPI_TYPE_LOCAL_SCOPE,      NULL},
	{"_SB_",    ACPI_TYPE_DEVICE,           NULL},
	{"_SI_",    ACPI_TYPE_LOCAL_SCOPE,      NULL},
	{"_TZ_",    ACPI_TYPE_LOCAL_SCOPE,      NULL},
	{"_REV",    ACPI_TYPE_INTEGER,          "2"},
	{"_OS_",    ACPI_TYPE_STRING,           ACPI_OS_NAME},
	{"_GL_",    ACPI_TYPE_MUTEX,            "0"},

#if !defined (ACPI_NO_METHOD_EXECUTION) || defined (ACPI_CONSTANT_EVAL_ONLY)
	{"_OSI",    ACPI_TYPE_METHOD,           "1"},
#endif
	{NULL,      ACPI_TYPE_ANY,              NULL}              /* Table terminator */
};


/*
 * Properties of the ACPI Object Types, both internal and external.
 * The table is indexed by values of acpi_object_type
 */
const u8                                acpi_gbl_ns_properties[] =
{
	ACPI_NS_NORMAL,                     /* 00 Any              */
	ACPI_NS_NORMAL,                     /* 01 Number           */
	ACPI_NS_NORMAL,                     /* 02 String           */
	ACPI_NS_NORMAL,                     /* 03 Buffer           */
	ACPI_NS_NORMAL,                     /* 04 Package          */
	ACPI_NS_NORMAL,                     /* 05 field_unit       */
	ACPI_NS_NEWSCOPE,                   /* 06 Device           */
	ACPI_NS_NORMAL,                     /* 07 Event            */
	ACPI_NS_NEWSCOPE,                   /* 08 Method           */
	ACPI_NS_NORMAL,                     /* 09 Mutex            */
	ACPI_NS_NORMAL,                     /* 10 Region           */
	ACPI_NS_NEWSCOPE,                   /* 11 Power            */
	ACPI_NS_NEWSCOPE,                   /* 12 Processor        */
	ACPI_NS_NEWSCOPE,                   /* 13 Thermal          */
	ACPI_NS_NORMAL,                     /* 14 buffer_field     */
	ACPI_NS_NORMAL,                     /* 15 ddb_handle       */
	ACPI_NS_NORMAL,                     /* 16 Debug Object     */
	ACPI_NS_NORMAL,                     /* 17 def_field        */
	ACPI_NS_NORMAL,                     /* 18 bank_field       */
	ACPI_NS_NORMAL,                     /* 19 index_field      */
	ACPI_NS_NORMAL,                     /* 20 Reference        */
	ACPI_NS_NORMAL,                     /* 21 Alias            */
	ACPI_NS_NORMAL,                     /* 22 method_alias     */
	ACPI_NS_NORMAL,                     /* 23 Notify           */
	ACPI_NS_NORMAL,                     /* 24 Address Handler  */
	ACPI_NS_NEWSCOPE | ACPI_NS_LOCAL,   /* 25 Resource Desc    */
	ACPI_NS_NEWSCOPE | ACPI_NS_LOCAL,   /* 26 Resource Field   */
	ACPI_NS_NEWSCOPE,                   /* 27 Scope            */
	ACPI_NS_NORMAL,                     /* 28 Extra            */
	ACPI_NS_NORMAL,                     /* 29 Data             */
	ACPI_NS_NORMAL                      /* 30 Invalid          */
};


/* Hex to ASCII conversion table */

static const char                   acpi_gbl_hex_to_ascii[] =
			  {'0','1','2','3','4','5','6','7',
					 '8','9','A','B','C','D','E','F'};

/*****************************************************************************
 *
 * FUNCTION:    acpi_ut_hex_to_ascii_char
 *
 * PARAMETERS:  Integer             - Contains the hex digit
 *              Position            - bit position of the digit within the
 *                                    integer
 *
 * RETURN:      Ascii character
 *
 * DESCRIPTION: Convert a hex digit to an ascii character
 *
 ****************************************************************************/

char
acpi_ut_hex_to_ascii_char (
	acpi_integer                    integer,
	u32                             position)
{

	return (acpi_gbl_hex_to_ascii[(integer >> position) & 0xF]);
}


/******************************************************************************
 *
 * Table name globals
 *
 * NOTE: This table includes ONLY the ACPI tables that the subsystem consumes.
 * it is NOT an exhaustive list of all possible ACPI tables.  All ACPI tables
 * that are not used by the subsystem are simply ignored.
 *
 * Do NOT add any table to this list that is not consumed directly by this
 * subsystem.
 *
 ******************************************************************************/

struct acpi_table_list              acpi_gbl_table_lists[NUM_ACPI_TABLE_TYPES];

struct acpi_table_support           acpi_gbl_table_data[NUM_ACPI_TABLE_TYPES] =
{
	/***********    Name,   Signature, Global typed pointer     Signature size,      Type                  How many allowed?,    Contains valid AML? */

	/* RSDP 0 */ {RSDP_NAME, RSDP_SIG, NULL,                    sizeof (RSDP_SIG)-1, ACPI_TABLE_ROOT     | ACPI_TABLE_SINGLE},
	/* DSDT 1 */ {DSDT_SIG,  DSDT_SIG, (void *) &acpi_gbl_DSDT, sizeof (DSDT_SIG)-1, ACPI_TABLE_SECONDARY| ACPI_TABLE_SINGLE   | ACPI_TABLE_EXECUTABLE},
	/* FADT 2 */ {FADT_SIG,  FADT_SIG, (void *) &acpi_gbl_FADT, sizeof (FADT_SIG)-1, ACPI_TABLE_PRIMARY  | ACPI_TABLE_SINGLE},
	/* FACS 3 */ {FACS_SIG,  FACS_SIG, (void *) &acpi_gbl_FACS, sizeof (FACS_SIG)-1, ACPI_TABLE_SECONDARY| ACPI_TABLE_SINGLE},
	/* PSDT 4 */ {PSDT_SIG,  PSDT_SIG, NULL,                    sizeof (PSDT_SIG)-1, ACPI_TABLE_PRIMARY  | ACPI_TABLE_MULTIPLE | ACPI_TABLE_EXECUTABLE},
	/* SSDT 5 */ {SSDT_SIG,  SSDT_SIG, NULL,                    sizeof (SSDT_SIG)-1, ACPI_TABLE_PRIMARY  | ACPI_TABLE_MULTIPLE | ACPI_TABLE_EXECUTABLE},
	/* XSDT 6 */ {XSDT_SIG,  XSDT_SIG, NULL,                    sizeof (RSDT_SIG)-1, ACPI_TABLE_ROOT     | ACPI_TABLE_SINGLE},
};


/******************************************************************************
 *
 * Event and Hardware globals
 *
 ******************************************************************************/

struct acpi_bit_register_info       acpi_gbl_bit_register_info[ACPI_NUM_BITREG] =
{
	/* Name                                     Parent Register             Register Bit Position                   Register Bit Mask       */

	/* ACPI_BITREG_TIMER_STATUS         */   {ACPI_REGISTER_PM1_STATUS,   ACPI_BITPOSITION_TIMER_STATUS,          ACPI_BITMASK_TIMER_STATUS},
	/* ACPI_BITREG_BUS_MASTER_STATUS    */   {ACPI_REGISTER_PM1_STATUS,   ACPI_BITPOSITION_BUS_MASTER_STATUS,     ACPI_BITMASK_BUS_MASTER_STATUS},
	/* ACPI_BITREG_GLOBAL_LOCK_STATUS   */   {ACPI_REGISTER_PM1_STATUS,   ACPI_BITPOSITION_GLOBAL_LOCK_STATUS,    ACPI_BITMASK_GLOBAL_LOCK_STATUS},
	/* ACPI_BITREG_POWER_BUTTON_STATUS  */   {ACPI_REGISTER_PM1_STATUS,   ACPI_BITPOSITION_POWER_BUTTON_STATUS,   ACPI_BITMASK_POWER_BUTTON_STATUS},
	/* ACPI_BITREG_SLEEP_BUTTON_STATUS  */   {ACPI_REGISTER_PM1_STATUS,   ACPI_BITPOSITION_SLEEP_BUTTON_STATUS,   ACPI_BITMASK_SLEEP_BUTTON_STATUS},
	/* ACPI_BITREG_RT_CLOCK_STATUS      */   {ACPI_REGISTER_PM1_STATUS,   ACPI_BITPOSITION_RT_CLOCK_STATUS,       ACPI_BITMASK_RT_CLOCK_STATUS},
	/* ACPI_BITREG_WAKE_STATUS          */   {ACPI_REGISTER_PM1_STATUS,   ACPI_BITPOSITION_WAKE_STATUS,           ACPI_BITMASK_WAKE_STATUS},

	/* ACPI_BITREG_TIMER_ENABLE         */   {ACPI_REGISTER_PM1_ENABLE,   ACPI_BITPOSITION_TIMER_ENABLE,          ACPI_BITMASK_TIMER_ENABLE},
	/* ACPI_BITREG_GLOBAL_LOCK_ENABLE   */   {ACPI_REGISTER_PM1_ENABLE,   ACPI_BITPOSITION_GLOBAL_LOCK_ENABLE,    ACPI_BITMASK_GLOBAL_LOCK_ENABLE},
	/* ACPI_BITREG_POWER_BUTTON_ENABLE  */   {ACPI_REGISTER_PM1_ENABLE,   ACPI_BITPOSITION_POWER_BUTTON_ENABLE,   ACPI_BITMASK_POWER_BUTTON_ENABLE},
	/* ACPI_BITREG_SLEEP_BUTTON_ENABLE  */   {ACPI_REGISTER_PM1_ENABLE,   ACPI_BITPOSITION_SLEEP_BUTTON_ENABLE,   ACPI_BITMASK_SLEEP_BUTTON_ENABLE},
	/* ACPI_BITREG_RT_CLOCK_ENABLE      */   {ACPI_REGISTER_PM1_ENABLE,   ACPI_BITPOSITION_RT_CLOCK_ENABLE,       ACPI_BITMASK_RT_CLOCK_ENABLE},
	/* ACPI_BITREG_WAKE_ENABLE          */   {ACPI_REGISTER_PM1_ENABLE,   0,                                      0},

	/* ACPI_BITREG_SCI_ENABLE           */   {ACPI_REGISTER_PM1_CONTROL,  ACPI_BITPOSITION_SCI_ENABLE,            ACPI_BITMASK_SCI_ENABLE},
	/* ACPI_BITREG_BUS_MASTER_RLD       */   {ACPI_REGISTER_PM1_CONTROL,  ACPI_BITPOSITION_BUS_MASTER_RLD,        ACPI_BITMASK_BUS_MASTER_RLD},
	/* ACPI_BITREG_GLOBAL_LOCK_RELEASE  */   {ACPI_REGISTER_PM1_CONTROL,  ACPI_BITPOSITION_GLOBAL_LOCK_RELEASE,   ACPI_BITMASK_GLOBAL_LOCK_RELEASE},
	/* ACPI_BITREG_SLEEP_TYPE_A         */   {ACPI_REGISTER_PM1_CONTROL,  ACPI_BITPOSITION_SLEEP_TYPE_X,          ACPI_BITMASK_SLEEP_TYPE_X},
	/* ACPI_BITREG_SLEEP_TYPE_B         */   {ACPI_REGISTER_PM1_CONTROL,  ACPI_BITPOSITION_SLEEP_TYPE_X,          ACPI_BITMASK_SLEEP_TYPE_X},
	/* ACPI_BITREG_SLEEP_ENABLE         */   {ACPI_REGISTER_PM1_CONTROL,  ACPI_BITPOSITION_SLEEP_ENABLE,          ACPI_BITMASK_SLEEP_ENABLE},

	/* ACPI_BITREG_ARB_DIS              */   {ACPI_REGISTER_PM2_CONTROL,  ACPI_BITPOSITION_ARB_DISABLE,           ACPI_BITMASK_ARB_DISABLE}
};


struct acpi_fixed_event_info        acpi_gbl_fixed_event_info[ACPI_NUM_FIXED_EVENTS] =
{
	/* ACPI_EVENT_PMTIMER       */  {ACPI_BITREG_TIMER_STATUS,          ACPI_BITREG_TIMER_ENABLE,        ACPI_BITMASK_TIMER_STATUS,          ACPI_BITMASK_TIMER_ENABLE},
	/* ACPI_EVENT_GLOBAL        */  {ACPI_BITREG_GLOBAL_LOCK_STATUS,    ACPI_BITREG_GLOBAL_LOCK_ENABLE,  ACPI_BITMASK_GLOBAL_LOCK_STATUS,    ACPI_BITMASK_GLOBAL_LOCK_ENABLE},
	/* ACPI_EVENT_POWER_BUTTON  */  {ACPI_BITREG_POWER_BUTTON_STATUS,   ACPI_BITREG_POWER_BUTTON_ENABLE, ACPI_BITMASK_POWER_BUTTON_STATUS,   ACPI_BITMASK_POWER_BUTTON_ENABLE},
	/* ACPI_EVENT_SLEEP_BUTTON  */  {ACPI_BITREG_SLEEP_BUTTON_STATUS,   ACPI_BITREG_SLEEP_BUTTON_ENABLE, ACPI_BITMASK_SLEEP_BUTTON_STATUS,   ACPI_BITMASK_SLEEP_BUTTON_ENABLE},
	/* ACPI_EVENT_RTC           */  {ACPI_BITREG_RT_CLOCK_STATUS,       ACPI_BITREG_RT_CLOCK_ENABLE,     ACPI_BITMASK_RT_CLOCK_STATUS,       ACPI_BITMASK_RT_CLOCK_ENABLE},
};

/*****************************************************************************
 *
 * FUNCTION:    acpi_ut_get_region_name
 *
 * PARAMETERS:  None.
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Translate a Space ID into a name string (Debug only)
 *
 ****************************************************************************/

/* Region type decoding */

const char                *acpi_gbl_region_types[ACPI_NUM_PREDEFINED_REGIONS] =
{
/*! [Begin] no source code translation (keep these ASL Keywords as-is) */
	"SystemMemory",
	"SystemIO",
	"PCI_Config",
	"EmbeddedControl",
	"SMBus",
	"CMOS",
	"PCIBARTarget",
	"DataTable"
/*! [End] no source code translation !*/
};


char *
acpi_ut_get_region_name (
	u8                              space_id)
{

	if (space_id >= ACPI_USER_REGION_BEGIN)
	{
		return ("user_defined_region");
	}

	else if (space_id >= ACPI_NUM_PREDEFINED_REGIONS)
	{
		return ("invalid_space_id");
	}

	return ((char *) acpi_gbl_region_types[space_id]);
}


/*****************************************************************************
 *
 * FUNCTION:    acpi_ut_get_event_name
 *
 * PARAMETERS:  None.
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Translate a Event ID into a name string (Debug only)
 *
 ****************************************************************************/

/* Event type decoding */

static const char                *acpi_gbl_event_types[ACPI_NUM_FIXED_EVENTS] =
{
	"PM_Timer",
	"global_lock",
	"power_button",
	"sleep_button",
	"real_time_clock",
};


char *
acpi_ut_get_event_name (
	u32                             event_id)
{

	if (event_id > ACPI_EVENT_MAX)
	{
		return ("invalid_event_iD");
	}

	return ((char *) acpi_gbl_event_types[event_id]);
}


/*****************************************************************************
 *
 * FUNCTION:    acpi_ut_get_type_name
 *
 * PARAMETERS:  None.
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Translate a Type ID into a name string (Debug only)
 *
 ****************************************************************************/

/*
 * Elements of acpi_gbl_ns_type_names below must match
 * one-to-one with values of acpi_object_type
 *
 * The type ACPI_TYPE_ANY (Untyped) is used as a "don't care" when searching; when
 * stored in a table it really means that we have thus far seen no evidence to
 * indicate what type is actually going to be stored for this entry.
 */
static const char                   acpi_gbl_bad_type[] = "UNDEFINED";
#define TYPE_NAME_LENGTH    12                           /* Maximum length of each string */

static const char                   *acpi_gbl_ns_type_names[] = /* printable names of ACPI types */
{
	/* 00 */ "Untyped",
	/* 01 */ "Integer",
	/* 02 */ "String",
	/* 03 */ "Buffer",
	/* 04 */ "Package",
	/* 05 */ "field_unit",
	/* 06 */ "Device",
	/* 07 */ "Event",
	/* 08 */ "Method",
	/* 09 */ "Mutex",
	/* 10 */ "Region",
	/* 11 */ "Power",
	/* 12 */ "Processor",
	/* 13 */ "Thermal",
	/* 14 */ "buffer_field",
	/* 15 */ "ddb_handle",
	/* 16 */ "debug_object",
	/* 17 */ "region_field",
	/* 18 */ "bank_field",
	/* 19 */ "index_field",
	/* 20 */ "Reference",
	/* 21 */ "Alias",
	/* 22 */ "method_alias",
	/* 23 */ "Notify",
	/* 24 */ "addr_handler",
	/* 25 */ "resource_desc",
	/* 26 */ "resource_fld",
	/* 27 */ "Scope",
	/* 28 */ "Extra",
	/* 29 */ "Data",
	/* 30 */ "Invalid"
};


char *
acpi_ut_get_type_name (
	acpi_object_type                type)
{

	if (type > ACPI_TYPE_INVALID)
	{
		return ((char *) acpi_gbl_bad_type);
	}

	return ((char *) acpi_gbl_ns_type_names[type]);
}


char *
acpi_ut_get_object_type_name (
	union acpi_operand_object       *obj_desc)
{

	if (!obj_desc)
	{
		return ("[NULL Object Descriptor]");
	}

	return (acpi_ut_get_type_name (ACPI_GET_OBJECT_TYPE (obj_desc)));
}


/*****************************************************************************
 *
 * FUNCTION:    acpi_ut_get_node_name
 *
 * PARAMETERS:  Object               - A namespace node
 *
 * RETURN:      Pointer to a string
 *
 * DESCRIPTION: Validate the node and return the node's ACPI name.
 *
 ****************************************************************************/

char *
acpi_ut_get_node_name (
	void                            *object)
{
	struct acpi_namespace_node      *node = (struct acpi_namespace_node *) object;


	if (!object)
	{
		return ("NULL NODE");
	}

	if (object == ACPI_ROOT_OBJECT)
	{
		node = acpi_gbl_root_node;
	}

	if (node->descriptor != ACPI_DESC_TYPE_NAMED)
	{
		return ("****");
	}

	if (!acpi_ut_valid_acpi_name (* (u32 *) node->name.ascii))
	{
		return ("----");
	}

	return (node->name.ascii);
}


/*****************************************************************************
 *
 * FUNCTION:    acpi_ut_get_descriptor_name
 *
 * PARAMETERS:  Object               - An ACPI object
 *
 * RETURN:      Pointer to a string
 *
 * DESCRIPTION: Validate object and return the descriptor type
 *
 ****************************************************************************/

static const char                   *acpi_gbl_desc_type_names[] = /* printable names of descriptor types */
{
	/* 00 */ "Invalid",
	/* 01 */ "Cached",
	/* 02 */ "State-Generic",
	/* 03 */ "State-Update",
	/* 04 */ "State-Package",
	/* 05 */ "State-Control",
	/* 06 */ "State-root_parse_scope",
	/* 07 */ "State-parse_scope",
	/* 08 */ "State-walk_scope",
	/* 09 */ "State-Result",
	/* 10 */ "State-Notify",
	/* 11 */ "State-Thread",
	/* 12 */ "Walk",
	/* 13 */ "Parser",
	/* 14 */ "Operand",
	/* 15 */ "Node"
};


char *
acpi_ut_get_descriptor_name (
	void                            *object)
{

	if (!object)
	{
		return ("NULL OBJECT");
	}

	if (ACPI_GET_DESCRIPTOR_TYPE (object) > ACPI_DESC_TYPE_MAX)
	{
		return ((char *) acpi_gbl_bad_type);
	}

	return ((char *) acpi_gbl_desc_type_names[ACPI_GET_DESCRIPTOR_TYPE (object)]);

}


#if defined(ACPI_DEBUG_OUTPUT) || defined(ACPI_DEBUGGER)
/*
 * Strings and procedures used for debug only
 */

/*****************************************************************************
 *
 * FUNCTION:    acpi_ut_get_mutex_name
 *
 * PARAMETERS:  None.
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Translate a mutex ID into a name string (Debug only)
 *
 ****************************************************************************/

char *
acpi_ut_get_mutex_name (
	u32                             mutex_id)
{

	if (mutex_id > MAX_MUTEX)
	{
		return ("Invalid Mutex ID");
	}

	return (acpi_gbl_mutex_names[mutex_id]);
}

#endif


/*****************************************************************************
 *
 * FUNCTION:    acpi_ut_valid_object_type
 *
 * PARAMETERS:  Type            - Object type to be validated
 *
 * RETURN:      TRUE if valid object type
 *
 * DESCRIPTION: Validate an object type
 *
 ****************************************************************************/

u8
acpi_ut_valid_object_type (
	acpi_object_type                type)
{

	if (type > ACPI_TYPE_LOCAL_MAX)
	{
		/* Note: Assumes all TYPEs are contiguous (external/local) */

		return (FALSE);
	}

	return (TRUE);
}


/****************************************************************************
 *
 * FUNCTION:    acpi_ut_allocate_owner_id
 *
 * PARAMETERS:  id_type         - Type of ID (method or table)
 *
 * DESCRIPTION: Allocate a table or method owner id
 *
 ***************************************************************************/

acpi_owner_id
acpi_ut_allocate_owner_id (
	u32                             id_type)
{
	acpi_owner_id                   owner_id = 0xFFFF;


	ACPI_FUNCTION_TRACE ("ut_allocate_owner_id");


	if (ACPI_FAILURE (acpi_ut_acquire_mutex (ACPI_MTX_CACHES)))
	{
		return (0);
	}

	switch (id_type)
	{
	case ACPI_OWNER_TYPE_TABLE:

		owner_id = acpi_gbl_next_table_owner_id;
		acpi_gbl_next_table_owner_id++;

		/* Check for wraparound */

		if (acpi_gbl_next_table_owner_id == ACPI_FIRST_METHOD_ID)
		{
			acpi_gbl_next_table_owner_id = ACPI_FIRST_TABLE_ID;
			ACPI_REPORT_WARNING (("Table owner ID wraparound\n"));
		}
		break;


	case ACPI_OWNER_TYPE_METHOD:

		owner_id = acpi_gbl_next_method_owner_id;
		acpi_gbl_next_method_owner_id++;

		if (acpi_gbl_next_method_owner_id == ACPI_FIRST_TABLE_ID)
		{
			/* Check for wraparound */

			acpi_gbl_next_method_owner_id = ACPI_FIRST_METHOD_ID;
		}
		break;

	default:
		break;
	}

	(void) acpi_ut_release_mutex (ACPI_MTX_CACHES);
	return_VALUE (owner_id);
}


/****************************************************************************
 *
 * FUNCTION:    acpi_ut_init_globals
 *
 * PARAMETERS:  none
 *
 * DESCRIPTION: Init library globals.  All globals that require specific
 *              initialization should be initialized here!
 *
 ***************************************************************************/

void
acpi_ut_init_globals (
	void)
{
	u32                             i;


	ACPI_FUNCTION_TRACE ("ut_init_globals");

	/* Runtime configuration */

	acpi_gbl_create_osi_method = TRUE;
	acpi_gbl_all_methods_serialized = FALSE;

	/* Memory allocation and cache lists */

	ACPI_MEMSET (acpi_gbl_memory_lists, 0, sizeof (struct acpi_memory_list) * ACPI_NUM_MEM_LISTS);

	acpi_gbl_memory_lists[ACPI_MEM_LIST_STATE].link_offset      = (u16) ACPI_PTR_DIFF (&(((union acpi_generic_state *) NULL)->common.next), NULL);
	acpi_gbl_memory_lists[ACPI_MEM_LIST_PSNODE].link_offset     = (u16) ACPI_PTR_DIFF (&(((union acpi_parse_object *) NULL)->common.next), NULL);
	acpi_gbl_memory_lists[ACPI_MEM_LIST_PSNODE_EXT].link_offset = (u16) ACPI_PTR_DIFF (&(((union acpi_parse_object *) NULL)->common.next), NULL);
	acpi_gbl_memory_lists[ACPI_MEM_LIST_OPERAND].link_offset    = (u16) ACPI_PTR_DIFF (&(((union acpi_operand_object *) NULL)->cache.next), NULL);
	acpi_gbl_memory_lists[ACPI_MEM_LIST_WALK].link_offset       = (u16) ACPI_PTR_DIFF (&(((struct acpi_walk_state *) NULL)->next), NULL);

	acpi_gbl_memory_lists[ACPI_MEM_LIST_NSNODE].object_size     = sizeof (struct acpi_namespace_node);
	acpi_gbl_memory_lists[ACPI_MEM_LIST_STATE].object_size      = sizeof (union acpi_generic_state);
	acpi_gbl_memory_lists[ACPI_MEM_LIST_PSNODE].object_size     = sizeof (struct acpi_parse_obj_common);
	acpi_gbl_memory_lists[ACPI_MEM_LIST_PSNODE_EXT].object_size = sizeof (struct acpi_parse_obj_named);
	acpi_gbl_memory_lists[ACPI_MEM_LIST_OPERAND].object_size    = sizeof (union acpi_operand_object);
	acpi_gbl_memory_lists[ACPI_MEM_LIST_WALK].object_size       = sizeof (struct acpi_walk_state);

	acpi_gbl_memory_lists[ACPI_MEM_LIST_STATE].max_cache_depth  = ACPI_MAX_STATE_CACHE_DEPTH;
	acpi_gbl_memory_lists[ACPI_MEM_LIST_PSNODE].max_cache_depth = ACPI_MAX_PARSE_CACHE_DEPTH;
	acpi_gbl_memory_lists[ACPI_MEM_LIST_PSNODE_EXT].max_cache_depth = ACPI_MAX_EXTPARSE_CACHE_DEPTH;
	acpi_gbl_memory_lists[ACPI_MEM_LIST_OPERAND].max_cache_depth = ACPI_MAX_OBJECT_CACHE_DEPTH;
	acpi_gbl_memory_lists[ACPI_MEM_LIST_WALK].max_cache_depth   = ACPI_MAX_WALK_CACHE_DEPTH;

	ACPI_MEM_TRACKING (acpi_gbl_memory_lists[ACPI_MEM_LIST_GLOBAL].list_name    = "Global Memory Allocation");
	ACPI_MEM_TRACKING (acpi_gbl_memory_lists[ACPI_MEM_LIST_NSNODE].list_name    = "Namespace Nodes");
	ACPI_MEM_TRACKING (acpi_gbl_memory_lists[ACPI_MEM_LIST_STATE].list_name     = "State Object Cache");
	ACPI_MEM_TRACKING (acpi_gbl_memory_lists[ACPI_MEM_LIST_PSNODE].list_name    = "Parse Node Cache");
	ACPI_MEM_TRACKING (acpi_gbl_memory_lists[ACPI_MEM_LIST_PSNODE_EXT].list_name = "Extended Parse Node Cache");
	ACPI_MEM_TRACKING (acpi_gbl_memory_lists[ACPI_MEM_LIST_OPERAND].list_name   = "Operand Object Cache");
	ACPI_MEM_TRACKING (acpi_gbl_memory_lists[ACPI_MEM_LIST_WALK].list_name      = "Tree Walk Node Cache");

	/* ACPI table structure */

	for (i = 0; i < NUM_ACPI_TABLE_TYPES; i++)
	{
		acpi_gbl_table_lists[i].next        = NULL;
		acpi_gbl_table_lists[i].count       = 0;
	}

	/* Mutex locked flags */

	for (i = 0; i < NUM_MUTEX; i++)
	{
		acpi_gbl_mutex_info[i].mutex        = NULL;
		acpi_gbl_mutex_info[i].owner_id     = ACPI_MUTEX_NOT_ACQUIRED;
		acpi_gbl_mutex_info[i].use_count    = 0;
	}

	/* GPE support */

	acpi_gbl_gpe_xrupt_list_head        = NULL;
	acpi_gbl_gpe_fadt_blocks[0]         = NULL;
	acpi_gbl_gpe_fadt_blocks[1]         = NULL;

	/* Global notify handlers */

	acpi_gbl_system_notify.handler      = NULL;
	acpi_gbl_device_notify.handler      = NULL;
	acpi_gbl_init_handler               = NULL;

	/* Global "typed" ACPI table pointers */

	acpi_gbl_RSDP                       = NULL;
	acpi_gbl_XSDT                       = NULL;
	acpi_gbl_FACS                       = NULL;
	acpi_gbl_FADT                       = NULL;
	acpi_gbl_DSDT                       = NULL;

	/* Global Lock support */

	acpi_gbl_global_lock_acquired       = FALSE;
	acpi_gbl_global_lock_thread_count   = 0;
	acpi_gbl_global_lock_handle         = 0;

	/* Miscellaneous variables */

	acpi_gbl_table_flags                = ACPI_PHYSICAL_POINTER;
	acpi_gbl_rsdp_original_location     = 0;
	acpi_gbl_cm_single_step             = FALSE;
	acpi_gbl_db_terminate_threads       = FALSE;
	acpi_gbl_shutdown                   = FALSE;
	acpi_gbl_ns_lookup_count            = 0;
	acpi_gbl_ps_find_count              = 0;
	acpi_gbl_acpi_hardware_present      = TRUE;
	acpi_gbl_next_table_owner_id        = ACPI_FIRST_TABLE_ID;
	acpi_gbl_next_method_owner_id       = ACPI_FIRST_METHOD_ID;
	acpi_gbl_debugger_configuration     = DEBUGGER_THREADING;
	acpi_gbl_db_output_flags            = ACPI_DB_CONSOLE_OUTPUT;

	/* Hardware oriented */

	acpi_gbl_events_initialized         = FALSE;

	/* Namespace */

	acpi_gbl_root_node                  = NULL;

	acpi_gbl_root_node_struct.name.integer = ACPI_ROOT_NAME;
	acpi_gbl_root_node_struct.descriptor = ACPI_DESC_TYPE_NAMED;
	acpi_gbl_root_node_struct.type      = ACPI_TYPE_DEVICE;
	acpi_gbl_root_node_struct.child     = NULL;
	acpi_gbl_root_node_struct.peer      = NULL;
	acpi_gbl_root_node_struct.object    = NULL;
	acpi_gbl_root_node_struct.flags     = ANOBJ_END_OF_PEER_LIST;


#ifdef ACPI_DEBUG_OUTPUT
	acpi_gbl_lowest_stack_pointer       = ACPI_SIZE_MAX;
#endif

	return_VOID;
}


