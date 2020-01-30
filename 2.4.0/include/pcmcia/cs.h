/*
 * cs.h 1.71 2000/08/29 00:54:20
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License
 * at http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and
 * limitations under the License. 
 *
 * The initial developer of the original code is David A. Hinds
 * <dahinds@users.sourceforge.net>.  Portions created by David A. Hinds
 * are Copyright (C) 1999 David A. Hinds.  All Rights Reserved.
 *
 * Alternatively, the contents of this file may be used under the
 * terms of the GNU Public License version 2 (the "GPL"), in which
 * case the provisions of the GPL are applicable instead of the
 * above.  If you wish to allow the use of your version of this file
 * only under the terms of the GPL and not to allow others to use
 * your version of this file under the MPL, indicate your decision by
 * deleting the provisions above and replace them with the notice and
 * other provisions required by the GPL.  If you do not delete the
 * provisions above, a recipient may use your version of this file
 * under either the MPL or the GPL.
 */

#ifndef _LINUX_CS_H
#define _LINUX_CS_H

/* For AccessConfigurationRegister */
typedef struct conf_reg_t {
    u_char	Function;
    u_int	Action;
    off_t	Offset;
    u_int	Value;
} conf_reg_t;

/* Actions */
#define CS_READ		1
#define CS_WRITE	2

/* for AdjustResourceInfo */
typedef struct adjust_t {
    u_int	Action;
    u_int	Resource;
    u_int	Attributes;
    union {
	struct memory {
	    u_long	Base;
	    u_long	Size;
	} memory;
	struct io {
	    ioaddr_t	BasePort;
	    ioaddr_t	NumPorts;
	    u_int	IOAddrLines;
	} io;
	struct irq {
	    u_int	IRQ;
	} irq;
    } resource;
} adjust_t;

/* Action field */
#define REMOVE_MANAGED_RESOURCE		1
#define ADD_MANAGED_RESOURCE		2
#define GET_FIRST_MANAGED_RESOURCE	3
#define GET_NEXT_MANAGED_RESOURCE	4
/* Resource field */
#define RES_MEMORY_RANGE		1
#define RES_IO_RANGE			2
#define RES_IRQ				3
/* Attribute field */
#define RES_IRQ_TYPE			0x03
#define RES_IRQ_TYPE_EXCLUSIVE		0
#define RES_IRQ_TYPE_TIME		1
#define RES_IRQ_TYPE_DYNAMIC		2
#define RES_IRQ_CSC			0x04
#define RES_SHARED			0x08
#define RES_RESERVED			0x10
#define RES_ALLOCATED			0x20
#define RES_REMOVED			0x40

typedef struct servinfo_t {
    char	Signature[2];
    u_int	Count;
    u_int	Revision;
    u_int	CSLevel;
    char	*VendorString;
} servinfo_t;

typedef struct event_callback_args_t {
    client_handle_t client_handle;
    void	*info;
    void	*mtdrequest;
    void	*buffer;
    void	*misc;
    void	*client_data;
    struct bus_operations *bus;
} event_callback_args_t;

/* for GetConfigurationInfo */
typedef struct config_info_t {
    u_char	Function;
    u_int	Attributes;
    u_int	Vcc, Vpp1, Vpp2;
    u_int	IntType;
    u_int	ConfigBase;
    u_char	Status, Pin, Copy, Option, ExtStatus;
    u_int	Present;
    u_int	CardValues;
    u_int	AssignedIRQ;
    u_int	IRQAttributes;
    ioaddr_t	BasePort1;
    ioaddr_t	NumPorts1;
    u_int	Attributes1;
    ioaddr_t	BasePort2;
    ioaddr_t	NumPorts2;
    u_int	Attributes2;
    u_int	IOAddrLines;
} config_info_t;

/* For CardValues field */
#define CV_OPTION_VALUE		0x01
#define CV_STATUS_VALUE		0x02
#define CV_PIN_REPLACEMENT	0x04
#define CV_COPY_VALUE		0x08
#define CV_EXT_STATUS		0x10

/* For GetFirst/NextClient */
typedef struct client_req_t {
    socket_t	Socket;
    u_int	Attributes;
} client_req_t;

#define CLIENT_THIS_SOCKET	0x01

/* For RegisterClient */
typedef struct client_reg_t {
    dev_info_t	*dev_info;
    u_int	Attributes;
    u_int	EventMask;
    int		(*event_handler)(event_t event, int priority,
				 event_callback_args_t *);
    event_callback_args_t event_callback_args;
    u_int	Version;
} client_reg_t;

/* ModifyConfiguration */
typedef struct modconf_t {
    u_int	Attributes;
    u_int	Vcc, Vpp1, Vpp2;
} modconf_t;

/* Attributes for ModifyConfiguration */
#define CONF_IRQ_CHANGE_VALID	0x100
#define CONF_VCC_CHANGE_VALID	0x200
#define CONF_VPP1_CHANGE_VALID	0x400
#define CONF_VPP2_CHANGE_VALID	0x800

/* For RequestConfiguration */
typedef struct config_req_t {
    u_int	Attributes;
    u_int	Vcc, Vpp1, Vpp2;
    u_int	IntType;
    u_int	ConfigBase;
    u_char	Status, Pin, Copy, ExtStatus;
    u_char	ConfigIndex;
    u_int	Present;
} config_req_t;

/* Attributes for RequestConfiguration */
#define CONF_ENABLE_IRQ		0x01
#define CONF_ENABLE_DMA		0x02
#define CONF_ENABLE_SPKR	0x04
#define CONF_VALID_CLIENT	0x100

/* IntType field */
#define INT_MEMORY		0x01
#define INT_MEMORY_AND_IO	0x02
#define INT_CARDBUS		0x04

/* For RequestIO and ReleaseIO */
typedef struct io_req_t {
    ioaddr_t	BasePort1;
    ioaddr_t	NumPorts1;
    u_int	Attributes1;
    ioaddr_t	BasePort2;
    ioaddr_t	NumPorts2;
    u_int	Attributes2;
    u_int	IOAddrLines;
} io_req_t;

/* Attributes for RequestIO and ReleaseIO */
#define IO_SHARED		0x01
#define IO_FIRST_SHARED		0x02
#define IO_FORCE_ALIAS_ACCESS	0x04
#define IO_DATA_PATH_WIDTH	0x18
#define IO_DATA_PATH_WIDTH_8	0x00
#define IO_DATA_PATH_WIDTH_16	0x08
#define IO_DATA_PATH_WIDTH_AUTO	0x10

/* For RequestIRQ and ReleaseIRQ */
typedef struct irq_req_t {
    u_int	Attributes;
    u_int	AssignedIRQ;
    u_int	IRQInfo1, IRQInfo2;
    void	*Handler;
    void	*Instance;
} irq_req_t;

/* Attributes for RequestIRQ and ReleaseIRQ */
#define IRQ_TYPE			0x03
#define IRQ_TYPE_EXCLUSIVE		0x00
#define IRQ_TYPE_TIME			0x01
#define IRQ_TYPE_DYNAMIC_SHARING	0x02
#define IRQ_FORCED_PULSE		0x04
#define IRQ_FIRST_SHARED		0x08
#define IRQ_HANDLE_PRESENT		0x10
#define IRQ_PULSE_ALLOCATED		0x100

/* Bits in IRQInfo1 field */
#define IRQ_MASK		0x0f
#define IRQ_NMI_ID		0x01
#define IRQ_IOCK_ID		0x02
#define IRQ_BERR_ID		0x04
#define IRQ_VEND_ID		0x08
#define IRQ_INFO2_VALID		0x10
#define IRQ_LEVEL_ID		0x20
#define IRQ_PULSE_ID		0x40
#define IRQ_SHARE_ID		0x80

typedef struct eventmask_t {
    u_int	Attributes;
    u_int	EventMask;
} eventmask_t;

#define CONF_EVENT_MASK_VALID	0x01

/* Configuration registers present */
#define PRESENT_OPTION		0x001
#define PRESENT_STATUS		0x002
#define PRESENT_PIN_REPLACE	0x004
#define PRESENT_COPY		0x008
#define PRESENT_EXT_STATUS	0x010
#define PRESENT_IOBASE_0	0x020
#define PRESENT_IOBASE_1	0x040
#define PRESENT_IOBASE_2	0x080
#define PRESENT_IOBASE_3	0x100
#define PRESENT_IOSIZE		0x200

/* For GetMemPage, MapMemPage */
typedef struct memreq_t {
    u_int	CardOffset;
    page_t	Page;
} memreq_t;

/* For ModifyWindow */
typedef struct modwin_t {
    u_int	Attributes;
    u_int	AccessSpeed;
} modwin_t;

/* For RequestWindow */
typedef struct win_req_t {
    u_int	Attributes;
    u_long	Base;
    u_int	Size;
    u_int	AccessSpeed;
} win_req_t;

/* Attributes for RequestWindow */
#define WIN_ADDR_SPACE		0x0001
#define WIN_ADDR_SPACE_MEM	0x0000
#define WIN_ADDR_SPACE_IO	0x0001
#define WIN_MEMORY_TYPE		0x0002
#define WIN_MEMORY_TYPE_CM	0x0000
#define WIN_MEMORY_TYPE_AM	0x0002
#define WIN_ENABLE		0x0004
#define WIN_DATA_WIDTH		0x0018
#define WIN_DATA_WIDTH_8	0x0000
#define WIN_DATA_WIDTH_16	0x0008
#define WIN_DATA_WIDTH_32	0x0010
#define WIN_PAGED		0x0020
#define WIN_SHARED		0x0040
#define WIN_FIRST_SHARED	0x0080
#define WIN_USE_WAIT		0x0100
#define WIN_STRICT_ALIGN	0x0200
#define WIN_MAP_BELOW_1MB	0x0400
#define WIN_PREFETCH		0x0800
#define WIN_CACHEABLE		0x1000
#define WIN_BAR_MASK		0xe000
#define WIN_BAR_SHIFT		13

/* Attributes for RegisterClient */
#define INFO_MASTER_CLIENT	0x01
#define INFO_IO_CLIENT		0x02
#define INFO_MTD_CLIENT		0x04
#define INFO_MEM_CLIENT		0x08
#define MAX_NUM_CLIENTS		3

#define INFO_CARD_SHARE		0x10
#define INFO_CARD_EXCL		0x20

typedef struct cs_status_t {
    u_char	Function;
    event_t 	CardState;
    event_t	SocketState;
} cs_status_t;

typedef struct error_info_t {
    int		func;
    int		retcode;
} error_info_t;

/* Special stuff for binding drivers to sockets */
typedef struct bind_req_t {
    socket_t	Socket;
    u_char	Function;
    dev_info_t	*dev_info;
} bind_req_t;

/* Flag to bind to all functions */
#define BIND_FN_ALL	0xff

typedef struct mtd_bind_t {
    socket_t	Socket;
    u_int	Attributes;
    u_int	CardOffset;
    dev_info_t	*dev_info;
} mtd_bind_t;

/* Events */
#define CS_EVENT_PRI_LOW		0
#define CS_EVENT_PRI_HIGH		1

#define CS_EVENT_WRITE_PROTECT		0x000001
#define CS_EVENT_CARD_LOCK		0x000002
#define CS_EVENT_CARD_INSERTION		0x000004
#define CS_EVENT_CARD_REMOVAL		0x000008
#define CS_EVENT_BATTERY_DEAD		0x000010
#define CS_EVENT_BATTERY_LOW		0x000020
#define CS_EVENT_READY_CHANGE		0x000040
#define CS_EVENT_CARD_DETECT		0x000080
#define CS_EVENT_RESET_REQUEST		0x000100
#define CS_EVENT_RESET_PHYSICAL		0x000200
#define CS_EVENT_CARD_RESET		0x000400
#define CS_EVENT_REGISTRATION_COMPLETE	0x000800
#define CS_EVENT_RESET_COMPLETE		0x001000
#define CS_EVENT_PM_SUSPEND		0x002000
#define CS_EVENT_PM_RESUME		0x004000
#define CS_EVENT_INSERTION_REQUEST	0x008000
#define CS_EVENT_EJECTION_REQUEST	0x010000
#define CS_EVENT_MTD_REQUEST		0x020000
#define CS_EVENT_ERASE_COMPLETE		0x040000
#define CS_EVENT_REQUEST_ATTENTION	0x080000
#define CS_EVENT_CB_DETECT		0x100000
#define CS_EVENT_3VCARD			0x200000
#define CS_EVENT_XVCARD			0x400000

/* Return codes */
#define CS_SUCCESS		0x00
#define CS_BAD_ADAPTER		0x01
#define CS_BAD_ATTRIBUTE	0x02
#define CS_BAD_BASE		0x03
#define CS_BAD_EDC		0x04
#define CS_BAD_IRQ		0x06
#define CS_BAD_OFFSET		0x07
#define CS_BAD_PAGE		0x08
#define CS_READ_FAILURE		0x09
#define CS_BAD_SIZE		0x0a
#define CS_BAD_SOCKET		0x0b
#define CS_BAD_TYPE		0x0d
#define CS_BAD_VCC		0x0e
#define CS_BAD_VPP		0x0f
#define CS_BAD_WINDOW		0x11
#define CS_WRITE_FAILURE	0x12
#define CS_NO_CARD		0x14
#define CS_UNSUPPORTED_FUNCTION	0x15
#define CS_UNSUPPORTED_MODE	0x16
#define CS_BAD_SPEED		0x17
#define CS_BUSY			0x18
#define CS_GENERAL_FAILURE	0x19
#define CS_WRITE_PROTECTED	0x1a
#define CS_BAD_ARG_LENGTH	0x1b
#define CS_BAD_ARGS		0x1c
#define CS_CONFIGURATION_LOCKED	0x1d
#define CS_IN_USE		0x1e
#define CS_NO_MORE_ITEMS	0x1f
#define CS_OUT_OF_RESOURCE	0x20
#define CS_BAD_HANDLE		0x21

#define CS_BAD_TUPLE		0x40

#ifdef __KERNEL__

/*
 *  The main Card Services entry point
 */

enum service {
    AccessConfigurationRegister, AddSocketServices,
    AdjustResourceInfo, CheckEraseQueue, CloseMemory, CopyMemory,
    DeregisterClient, DeregisterEraseQueue, GetCardServicesInfo,
    GetClientInfo, GetConfigurationInfo, GetEventMask,
    GetFirstClient, GetFirstPartion, GetFirstRegion, GetFirstTuple,
    GetNextClient, GetNextPartition, GetNextRegion, GetNextTuple,
    GetStatus, GetTupleData, MapLogSocket, MapLogWindow, MapMemPage,
    MapPhySocket, MapPhyWindow, ModifyConfiguration, ModifyWindow,
    OpenMemory, ParseTuple, ReadMemory, RegisterClient,
    RegisterEraseQueue, RegisterMTD, RegisterTimer,
    ReleaseConfiguration, ReleaseExclusive, ReleaseIO, ReleaseIRQ,
    ReleaseSocketMask, ReleaseWindow, ReplaceSocketServices,
    RequestConfiguration, RequestExclusive, RequestIO, RequestIRQ,
    RequestSocketMask, RequestWindow, ResetCard, ReturnSSEntry,
    SetEventMask, SetRegion, ValidateCIS, VendorSpecific,
    WriteMemory, BindDevice, BindMTD, ReportError,
    SuspendCard, ResumeCard, EjectCard, InsertCard, ReplaceCIS,
    GetFirstWindow, GetNextWindow, GetMemPage
};

#ifdef IN_CARD_SERVICES
extern int CardServices(int func, void *a1, void *a2, void *a3);
#else
extern int CardServices(int func, ...);
#endif

int pcmcia_access_configuration_register(client_handle_t handle, conf_reg_t *reg);
int pcmcia_bind_device(bind_req_t *req);
int pcmcia_bind_mtd(mtd_bind_t *req);
int pcmcia_deregister_client(client_handle_t handle);
int pcmcia_get_configuration_info(client_handle_t handle, config_info_t *config);
int pcmcia_get_card_services_info(servinfo_t *info);
int pcmcia_get_first_client(client_handle_t *handle, client_req_t *req);
int pcmcia_get_next_client(client_handle_t *handle, client_req_t *req);
int pcmcia_get_window(window_handle_t *handle, int idx, win_req_t *req);
int pcmcia_get_first_window(window_handle_t *win, win_req_t *req);
int pcmcia_get_next_window(window_handle_t *win, win_req_t *req);
int pcmcia_get_status(client_handle_t handle, cs_status_t *status);
int pcmcia_get_mem_page(window_handle_t win, memreq_t *req);
int pcmcia_map_mem_page(window_handle_t win, memreq_t *req);
int pcmcia_modify_configuration(client_handle_t handle, modconf_t *mod);
int pcmcia_modify_window(window_handle_t win, modwin_t *req);
int pcmcia_register_client(client_handle_t *handle, client_reg_t *req);
int pcmcia_release_configuration(client_handle_t handle);
int pcmcia_release_io(client_handle_t handle, io_req_t *req);
int pcmcia_release_irq(client_handle_t handle, irq_req_t *req);
int pcmcia_release_window(window_handle_t win);
int pcmcia_request_configuration(client_handle_t handle, config_req_t *req);
int pcmcia_request_io(client_handle_t handle, io_req_t *req);
int pcmcia_request_irq(client_handle_t handle, irq_req_t *req);
int pcmcia_request_window(client_handle_t *handle, win_req_t *req, window_handle_t *wh);
int pcmcia_reset_card(client_handle_t handle, client_req_t *req);
int pcmcia_suspend_card(client_handle_t handle, client_req_t *req);
int pcmcia_resume_card(client_handle_t handle, client_req_t *req);
int pcmcia_eject_card(client_handle_t handle, client_req_t *req);
int pcmcia_insert_card(client_handle_t handle, client_req_t *req);
int pcmcia_set_event_mask(client_handle_t handle, eventmask_t *mask);
int pcmcia_report_error(client_handle_t handle, error_info_t *err);
struct pci_bus *pcmcia_lookup_bus(client_handle_t handle);

/* rsrc_mgr.c */
int pcmcia_adjust_resource_info(client_handle_t handle, adjust_t *adj);

#endif /* __KERNEL__ */

#endif /* _LINUX_CS_H */
