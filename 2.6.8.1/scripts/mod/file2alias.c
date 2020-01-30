/* Simple code to turn various tables in an ELF file into alias definitions.
 * This deals with kernel datastructures where they should be
 * dealt with: in the kernel source.
 *
 * Copyright 2002-2003  Rusty Russell, IBM Corporation
 *           2003       Kai Germaschewski
 *           
 *
 * This software may be used and distributed according to the terms
 * of the GNU General Public License, incorporated herein by reference.
 */

#include "modpost.h"

/* We use the ELF typedefs, since we can't rely on stdint.h being present. */

#if KERNEL_ELFCLASS == ELFCLASS32
typedef Elf32_Addr     kernel_ulong_t;
#else
typedef Elf64_Addr     kernel_ulong_t;
#endif

typedef Elf32_Word     __u32;
typedef Elf32_Half     __u16;
typedef unsigned char  __u8;

/* Big exception to the "don't include kernel headers into userspace, which
 * even potentially has different endianness and word sizes, since 
 * we handle those differences explicitly below */
#include "../../include/linux/mod_devicetable.h"

#define ADD(str, sep, cond, field)                              \
do {                                                            \
        strcat(str, sep);                                       \
        if (cond)                                               \
                sprintf(str + strlen(str),                      \
                        sizeof(field) == 1 ? "%02X" :           \
                        sizeof(field) == 2 ? "%04X" :           \
                        sizeof(field) == 4 ? "%08X" : "",       \
                        field);                                 \
        else                                                    \
                sprintf(str + strlen(str), "*");                \
} while(0)

/* Looks like "usb:vNpNdlNdhNdcNdscNdpNicNiscNipN" */
static int do_usb_entry(const char *filename,
			struct usb_device_id *id, char *alias)
{
	id->match_flags = TO_NATIVE(id->match_flags);
	id->idVendor = TO_NATIVE(id->idVendor);
	id->idProduct = TO_NATIVE(id->idProduct);
	id->bcdDevice_lo = TO_NATIVE(id->bcdDevice_lo);
	id->bcdDevice_hi = TO_NATIVE(id->bcdDevice_hi);

	/*
	 * Some modules (visor) have empty slots as placeholder for
	 * run-time specification that results in catch-all alias
	 */
	if (!(id->idVendor | id->bDeviceClass | id->bInterfaceClass))
		return 1;

	strcpy(alias, "usb:");
	ADD(alias, "v", id->match_flags&USB_DEVICE_ID_MATCH_VENDOR,
	    id->idVendor);
	ADD(alias, "p", id->match_flags&USB_DEVICE_ID_MATCH_PRODUCT,
	    id->idProduct);
	ADD(alias, "dl", id->match_flags&USB_DEVICE_ID_MATCH_DEV_LO,
	    id->bcdDevice_lo);
	ADD(alias, "dh", id->match_flags&USB_DEVICE_ID_MATCH_DEV_HI,
	    id->bcdDevice_hi);
	ADD(alias, "dc", id->match_flags&USB_DEVICE_ID_MATCH_DEV_CLASS,
	    id->bDeviceClass);
	ADD(alias, "dsc",
	    id->match_flags&USB_DEVICE_ID_MATCH_DEV_SUBCLASS,
	    id->bDeviceSubClass);
	ADD(alias, "dp",
	    id->match_flags&USB_DEVICE_ID_MATCH_DEV_PROTOCOL,
	    id->bDeviceProtocol);
	ADD(alias, "ic",
	    id->match_flags&USB_DEVICE_ID_MATCH_INT_CLASS,
	    id->bInterfaceClass);
	ADD(alias, "isc",
	    id->match_flags&USB_DEVICE_ID_MATCH_INT_SUBCLASS,
	    id->bInterfaceSubClass);
	ADD(alias, "ip",
	    id->match_flags&USB_DEVICE_ID_MATCH_INT_PROTOCOL,
	    id->bInterfaceProtocol);
	return 1;
}

/* Looks like: ieee1394:venNmoNspNverN */
static int do_ieee1394_entry(const char *filename,
			     struct ieee1394_device_id *id, char *alias)
{
	id->match_flags = TO_NATIVE(id->match_flags);
	id->vendor_id = TO_NATIVE(id->vendor_id);
	id->model_id = TO_NATIVE(id->model_id);
	id->specifier_id = TO_NATIVE(id->specifier_id);
	id->version = TO_NATIVE(id->version);

	strcpy(alias, "ieee1394:");
	ADD(alias, "ven", id->match_flags & IEEE1394_MATCH_VENDOR_ID,
	    id->vendor_id);
	ADD(alias, "mo", id->match_flags & IEEE1394_MATCH_MODEL_ID,
	    id->model_id);
	ADD(alias, "sp", id->match_flags & IEEE1394_MATCH_SPECIFIER_ID,
	    id->specifier_id);
	ADD(alias, "ver", id->match_flags & IEEE1394_MATCH_VERSION,
	    id->version);

	return 1;
}

/* Looks like: pci:vNdNsvNsdNbcNscNiN. */
static int do_pci_entry(const char *filename,
			struct pci_device_id *id, char *alias)
{
	/* Class field can be divided into these three. */
	unsigned char baseclass, subclass, interface,
		baseclass_mask, subclass_mask, interface_mask;

	id->vendor = TO_NATIVE(id->vendor);
	id->device = TO_NATIVE(id->device);
	id->subvendor = TO_NATIVE(id->subvendor);
	id->subdevice = TO_NATIVE(id->subdevice);
	id->class = TO_NATIVE(id->class);
	id->class_mask = TO_NATIVE(id->class_mask);

	strcpy(alias, "pci:");
	ADD(alias, "v", id->vendor != PCI_ANY_ID, id->vendor);
	ADD(alias, "d", id->device != PCI_ANY_ID, id->device);
	ADD(alias, "sv", id->subvendor != PCI_ANY_ID, id->subvendor);
	ADD(alias, "sd", id->subdevice != PCI_ANY_ID, id->subdevice);

	baseclass = (id->class) >> 16;
	baseclass_mask = (id->class_mask) >> 16;
	subclass = (id->class) >> 8;
	subclass_mask = (id->class_mask) >> 8;
	interface = id->class;
	interface_mask = id->class_mask;

	if ((baseclass_mask != 0 && baseclass_mask != 0xFF)
	    || (subclass_mask != 0 && subclass_mask != 0xFF)
	    || (interface_mask != 0 && interface_mask != 0xFF)) {
		fprintf(stderr,
			"*** Warning: Can't handle masks in %s:%04X\n",
			filename, id->class_mask);
		return 0;
	}

	ADD(alias, "bc", baseclass_mask == 0xFF, baseclass);
	ADD(alias, "sc", subclass_mask == 0xFF, subclass);
	ADD(alias, "i", interface_mask == 0xFF, interface);
	return 1;
}

/* looks like: "ccw:tNmNdtNdmN" */ 
static int do_ccw_entry(const char *filename,
			struct ccw_device_id *id, char *alias)
{
	id->match_flags = TO_NATIVE(id->match_flags);
	id->cu_type = TO_NATIVE(id->cu_type);
	id->cu_model = TO_NATIVE(id->cu_model);
	id->dev_type = TO_NATIVE(id->dev_type);
	id->dev_model = TO_NATIVE(id->dev_model);

	strcpy(alias, "ccw:");
	ADD(alias, "t", id->match_flags&CCW_DEVICE_ID_MATCH_CU_TYPE,
	    id->cu_type);
	ADD(alias, "m", id->match_flags&CCW_DEVICE_ID_MATCH_CU_MODEL,
	    id->cu_model);
	ADD(alias, "dt", id->match_flags&CCW_DEVICE_ID_MATCH_DEVICE_TYPE,
	    id->dev_type);
	ADD(alias, "dm", id->match_flags&CCW_DEVICE_ID_MATCH_DEVICE_TYPE,
	    id->dev_model);
	return 1;
}

/* looks like: "pnp:dD" */
static int do_pnp_entry(const char *filename,
			struct pnp_device_id *id, char *alias)
{
	sprintf(alias, "pnp:d%s", id->id);
	return 1;
}

/* looks like: "pnp:cCdD..." */
static int do_pnp_card_entry(const char *filename,
			struct pnp_card_device_id *id, char *alias)
{
	int i;

	sprintf(alias, "pnp:c%s", id->id);
	for (i = 0; i < PNP_MAX_DEVICES; i++) {
		if (! *id->devs[i].id)
			break;
		sprintf(alias + strlen(alias), "d%s", id->devs[i].id);
	}
	return 1;
}

/* Ignore any prefix, eg. v850 prepends _ */
static inline int sym_is(const char *symbol, const char *name)
{
	const char *match;

	match = strstr(symbol, name);
	if (!match)
		return 0;
	return match[strlen(symbol)] == '\0';
}

static void do_table(void *symval, unsigned long size,
		     unsigned long id_size,
		     void *function,
		     struct module *mod)
{
	unsigned int i;
	char alias[500];
	int (*do_entry)(const char *, void *entry, char *alias) = function;

	if (size % id_size || size < id_size) {
		fprintf(stderr, "*** Warning: %s ids %lu bad size "
			"(each on %lu)\n", mod->name, size, id_size);
	}
	/* Leave last one: it's the terminator. */
	size -= id_size;

	for (i = 0; i < size; i += id_size) {
		if (do_entry(mod->name, symval+i, alias)) {
			/* Always end in a wildcard, for future extension */
			if (alias[strlen(alias)-1] != '*')
				strcat(alias, "*");
			buf_printf(&mod->dev_table_buf,
				   "MODULE_ALIAS(\"%s\");\n", alias);
		}
	}
}

/* Create MODULE_ALIAS() statements.
 * At this time, we cannot write the actual output C source yet,
 * so we write into the mod->dev_table_buf buffer. */
void handle_moddevtable(struct module *mod, struct elf_info *info,
			Elf_Sym *sym, const char *symname)
{
	void *symval;

	/* We're looking for a section relative symbol */
	if (!sym->st_shndx || sym->st_shndx >= info->hdr->e_shnum)
		return;

	symval = (void *)info->hdr
		+ info->sechdrs[sym->st_shndx].sh_offset
		+ sym->st_value;

	if (sym_is(symname, "__mod_pci_device_table"))
		do_table(symval, sym->st_size, sizeof(struct pci_device_id),
			 do_pci_entry, mod);
	else if (sym_is(symname, "__mod_usb_device_table"))
		do_table(symval, sym->st_size, sizeof(struct usb_device_id),
			 do_usb_entry, mod);
	else if (sym_is(symname, "__mod_ieee1394_device_table"))
		do_table(symval, sym->st_size, sizeof(struct ieee1394_device_id),
			 do_ieee1394_entry, mod);
	else if (sym_is(symname, "__mod_ccw_device_table"))
		do_table(symval, sym->st_size, sizeof(struct ccw_device_id),
			 do_ccw_entry, mod);
	else if (sym_is(symname, "__mod_pnp_device_table"))
		do_table(symval, sym->st_size, sizeof(struct pnp_device_id),
			 do_pnp_entry, mod);
	else if (sym_is(symname, "__mod_pnp_card_device_table"))
		do_table(symval, sym->st_size, sizeof(struct pnp_card_device_id),
			 do_pnp_card_entry, mod);
}

/* Now add out buffered information to the generated C source */
void add_moddevtable(struct buffer *buf, struct module *mod)
{
	buf_printf(buf, "\n");
	buf_write(buf, mod->dev_table_buf.p, mod->dev_table_buf.pos);
	free(mod->dev_table_buf.p);
}
