#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <asm/acpi.h>
#include <asm/io.h>
#include <linux/pm.h>
#include <asm/system.h>
#include <linux/dmi.h>
#include <linux/bootmem.h>


int es7000_plat = 0;

struct dmi_header
{
	u8	type;
	u8	length;
	u16	handle;
};

#undef DMI_DEBUG

#ifdef DMI_DEBUG
#define dmi_printk(x) printk x
#else
#define dmi_printk(x)
#endif

static char * __init dmi_string(struct dmi_header *dm, u8 s)
{
	u8 *bp=(u8 *)dm;
	bp+=dm->length;
	if(!s)
		return "";
	s--;
	while(s>0 && *bp)
	{
		bp+=strlen(bp);
		bp++;
		s--;
	}
	return bp;
}

/*
 *	We have to be cautious here. We have seen BIOSes with DMI pointers
 *	pointing to completely the wrong place for example
 */
 
static int __init dmi_table(u32 base, int len, int num, void (*decode)(struct dmi_header *))
{
	u8 *buf;
	struct dmi_header *dm;
	u8 *data;
	int i=0;
		
	buf = bt_ioremap(base, len);
	if(buf==NULL)
		return -1;

	data = buf;

	/*
 	 *	Stop when we see all the items the table claimed to have
 	 *	OR we run off the end of the table (also happens)
 	 */
 
	while(i<num && data-buf+sizeof(struct dmi_header)<=len)
	{
		dm=(struct dmi_header *)data;
		/*
		 *  We want to know the total length (formated area and strings)
		 *  before decoding to make sure we won't run off the table in
		 *  dmi_decode or dmi_string
		 */
		data+=dm->length;
		while(data-buf<len-1 && (data[0] || data[1]))
			data++;
		if(data-buf<len-1)
			decode(dm);
		data+=2;
		i++;
	}
	bt_iounmap(buf, len);
	return 0;
}


inline static int __init dmi_checksum(u8 *buf)
{
	u8 sum=0;
	int a;
	
	for(a=0; a<15; a++)
		sum+=buf[a];
	return (sum==0);
}

static int __init dmi_iterate(void (*decode)(struct dmi_header *))
{
	u8 buf[15];
	u32 fp=0xF0000;

	while (fp < 0xFFFFF)
	{
		isa_memcpy_fromio(buf, fp, 15);
		if(memcmp(buf, "_DMI_", 5)==0 && dmi_checksum(buf))
		{
			u16 num=buf[13]<<8|buf[12];
			u16 len=buf[7]<<8|buf[6];
			u32 base=buf[11]<<24|buf[10]<<16|buf[9]<<8|buf[8];

			/*
			 * DMI version 0.0 means that the real version is taken from
			 * the SMBIOS version, which we don't know at this point.
			 */
			if(buf[14]!=0)
				printk(KERN_INFO "DMI %d.%d present.\n",
					buf[14]>>4, buf[14]&0x0F);
			else
				printk(KERN_INFO "DMI present.\n");
			dmi_printk((KERN_INFO "%d structures occupying %d bytes.\n",
				num, len));
			dmi_printk((KERN_INFO "DMI table at 0x%08X.\n",
				base));
			if(dmi_table(base,len, num, decode)==0)
				return 0;
		}
		fp+=16;
	}
	return -1;
}

static char *dmi_ident[DMI_STRING_MAX];

/*
 *	Save a DMI string
 */
 
static void __init dmi_save_ident(struct dmi_header *dm, int slot, int string)
{
	char *d = (char*)dm;
	char *p = dmi_string(dm, d[string]);
	if(p==NULL || *p == 0)
		return;
	if (dmi_ident[slot])
		return;
	dmi_ident[slot] = alloc_bootmem(strlen(p)+1);
	if(dmi_ident[slot])
		strcpy(dmi_ident[slot], p);
	else
		printk(KERN_ERR "dmi_save_ident: out of memory.\n");
}

/*
 * Ugly compatibility crap.
 */
#define dmi_blacklist	dmi_system_id
#define NO_MATCH	{ DMI_NONE, NULL}
#define MATCH		DMI_MATCH

/*
 * Some machines, usually laptops, can't handle an enabled local APIC.
 * The symptoms include hangs or reboots when suspending or resuming,
 * attaching or detaching the power cord, or entering BIOS setup screens
 * through magic key sequences.
 */
static int __init local_apic_kills_bios(struct dmi_blacklist *d)
{
#ifdef CONFIG_X86_LOCAL_APIC
	extern int enable_local_apic;
	if (enable_local_apic == 0) {
		enable_local_apic = -1;
		printk(KERN_WARNING "%s with broken BIOS detected. "
		       "Refusing to enable the local APIC.\n",
		       d->ident);
	}
#endif
	return 0;
}


/*
 * Toshiba keyboard likes to repeat keys when they are not repeated.
 */

static __init int broken_toshiba_keyboard(struct dmi_blacklist *d)
{
	printk(KERN_WARNING "Toshiba with broken keyboard detected. If your keyboard sometimes generates 3 keypresses instead of one, see http://davyd.ucc.asn.au/projects/toshiba/README\n");
	return 0;
}


#ifdef CONFIG_ACPI_SLEEP
static __init int reset_videomode_after_s3(struct dmi_blacklist *d)
{
	/* See acpi_wakeup.S */
	extern long acpi_video_flags;
	acpi_video_flags |= 2;
	return 0;
}
#endif


#ifdef	CONFIG_ACPI_BOOT
extern int acpi_force;

static __init __attribute__((unused)) int dmi_disable_acpi(struct dmi_blacklist *d) 
{ 
	if (!acpi_force) { 
		printk(KERN_NOTICE "%s detected: acpi off\n",d->ident); 
		disable_acpi();
	} else { 
		printk(KERN_NOTICE 
		       "Warning: DMI blacklist says broken, but acpi forced\n"); 
	}
	return 0;
} 

/*
 * Limit ACPI to CPU enumeration for HT
 */
static __init __attribute__((unused)) int force_acpi_ht(struct dmi_blacklist *d) 
{ 
	if (!acpi_force) { 
		printk(KERN_NOTICE "%s detected: force use of acpi=ht\n", d->ident); 
		disable_acpi();
		acpi_ht = 1; 
	} else { 
		printk(KERN_NOTICE 
		       "Warning: acpi=force overrules DMI blacklist: acpi=ht\n"); 
	}
	return 0;
} 

/*
 * early nForce2 reference BIOS shipped with a
 * bogus ACPI IRQ0 -> pin2 interrupt override -- ignore it
 */
static __init int ignore_timer_override(struct dmi_blacklist *d)
{
	extern int acpi_skip_timer_override;
	printk(KERN_NOTICE "%s detected: BIOS IRQ0 pin2 override"
		" will be ignored\n", d->ident); 	

	acpi_skip_timer_override = 1;
	return 0;
}
#endif

#ifdef	CONFIG_ACPI_PCI
static __init int disable_acpi_irq(struct dmi_blacklist *d) 
{
	if (!acpi_force) {
		printk(KERN_NOTICE "%s detected: force use of acpi=noirq\n",
		       d->ident); 	
		acpi_noirq_set();
	}
	return 0;
}
static __init int disable_acpi_pci(struct dmi_blacklist *d) 
{
	if (!acpi_force) {
		printk(KERN_NOTICE "%s detected: force use of pci=noacpi\n",
		       d->ident); 	
		acpi_disable_pci();
	}
	return 0;
}  
#endif

/*
 *	Process the DMI blacklists
 */
 

/*
 *	This will be expanded over time to force things like the APM 
 *	interrupt mask settings according to the laptop
 */
 
static __initdata struct dmi_blacklist dmi_blacklist[]={

	/* Machines which have problems handling enabled local APICs */

	{ local_apic_kills_bios, "Dell Inspiron", {
			MATCH(DMI_SYS_VENDOR, "Dell Computer Corporation"),
			MATCH(DMI_PRODUCT_NAME, "Inspiron"),
			NO_MATCH, NO_MATCH
			} },

	{ local_apic_kills_bios, "Dell Latitude", {
			MATCH(DMI_SYS_VENDOR, "Dell Computer Corporation"),
			MATCH(DMI_PRODUCT_NAME, "Latitude"),
			NO_MATCH, NO_MATCH
			} },

	{ local_apic_kills_bios, "IBM Thinkpad T20", {
			MATCH(DMI_BOARD_VENDOR, "IBM"),
			MATCH(DMI_BOARD_NAME, "264741U"),
			NO_MATCH, NO_MATCH
			} },

	{ local_apic_kills_bios, "ASUS L3C", {
			MATCH(DMI_BOARD_VENDOR, "ASUSTeK Computer INC."),
			MATCH(DMI_BOARD_NAME, "P4_L3C"),
			NO_MATCH, NO_MATCH
			} },

	{ broken_toshiba_keyboard, "Toshiba Satellite 4030cdt", { /* Keyboard generates spurious repeats */
			MATCH(DMI_PRODUCT_NAME, "S4030CDT/4.3"),
			NO_MATCH, NO_MATCH, NO_MATCH
			} },
#ifdef CONFIG_ACPI_SLEEP
	{ reset_videomode_after_s3, "Toshiba Satellite 4030cdt", { /* Reset video mode after returning from ACPI S3 sleep */
			MATCH(DMI_PRODUCT_NAME, "S4030CDT/4.3"),
			NO_MATCH, NO_MATCH, NO_MATCH
			} },
#endif

#ifdef	CONFIG_ACPI_BOOT
	/*
	 * If your system is blacklisted here, but you find that acpi=force
	 * works for you, please contact acpi-devel@sourceforge.net
	 */

	/*
	 *	Boxes that need ACPI disabled
	 */

	{ dmi_disable_acpi, "IBM Thinkpad", {
			MATCH(DMI_BOARD_VENDOR, "IBM"),
			MATCH(DMI_BOARD_NAME, "2629H1G"),
			NO_MATCH, NO_MATCH }},

	/*
	 *	Boxes that need acpi=ht 
	 */

	{ force_acpi_ht, "FSC Primergy T850", {
			MATCH(DMI_SYS_VENDOR, "FUJITSU SIEMENS"),
			MATCH(DMI_PRODUCT_NAME, "PRIMERGY T850"),
			NO_MATCH, NO_MATCH }},

	{ force_acpi_ht, "DELL GX240", {
			MATCH(DMI_BOARD_VENDOR, "Dell Computer Corporation"),
			MATCH(DMI_BOARD_NAME, "OptiPlex GX240"),
			NO_MATCH, NO_MATCH }},

	{ force_acpi_ht, "HP VISUALIZE NT Workstation", {
			MATCH(DMI_BOARD_VENDOR, "Hewlett-Packard"),
			MATCH(DMI_PRODUCT_NAME, "HP VISUALIZE NT Workstation"),
			NO_MATCH, NO_MATCH }},

	{ force_acpi_ht, "Compaq ProLiant DL380 G2", {
			MATCH(DMI_SYS_VENDOR, "Compaq"),
			MATCH(DMI_PRODUCT_NAME, "ProLiant DL380 G2"),
			NO_MATCH, NO_MATCH }},

	{ force_acpi_ht, "Compaq ProLiant ML530 G2", {
			MATCH(DMI_SYS_VENDOR, "Compaq"),
			MATCH(DMI_PRODUCT_NAME, "ProLiant ML530 G2"),
			NO_MATCH, NO_MATCH }},

	{ force_acpi_ht, "Compaq ProLiant ML350 G3", {
			MATCH(DMI_SYS_VENDOR, "Compaq"),
			MATCH(DMI_PRODUCT_NAME, "ProLiant ML350 G3"),
			NO_MATCH, NO_MATCH }},

	{ force_acpi_ht, "Compaq Workstation W8000", {
			MATCH(DMI_SYS_VENDOR, "Compaq"),
			MATCH(DMI_PRODUCT_NAME, "Workstation W8000"),
			NO_MATCH, NO_MATCH }},

	{ force_acpi_ht, "ASUS P4B266", {
			MATCH(DMI_BOARD_VENDOR, "ASUSTeK Computer INC."),
			MATCH(DMI_BOARD_NAME, "P4B266"),
			NO_MATCH, NO_MATCH }},

	{ force_acpi_ht, "ASUS P2B-DS", {
			MATCH(DMI_BOARD_VENDOR, "ASUSTeK Computer INC."),
			MATCH(DMI_BOARD_NAME, "P2B-DS"),
			NO_MATCH, NO_MATCH }},

	{ force_acpi_ht, "ASUS CUR-DLS", {
			MATCH(DMI_BOARD_VENDOR, "ASUSTeK Computer INC."),
			MATCH(DMI_BOARD_NAME, "CUR-DLS"),
			NO_MATCH, NO_MATCH }},

	{ force_acpi_ht, "ABIT i440BX-W83977", {
			MATCH(DMI_BOARD_VENDOR, "ABIT <http://www.abit.com>"),
			MATCH(DMI_BOARD_NAME, "i440BX-W83977 (BP6)"),
			NO_MATCH, NO_MATCH }},

	{ force_acpi_ht, "IBM Bladecenter", {
			MATCH(DMI_BOARD_VENDOR, "IBM"),
			MATCH(DMI_BOARD_NAME, "IBM eServer BladeCenter HS20"),
			NO_MATCH, NO_MATCH }},

	{ force_acpi_ht, "IBM eServer xSeries 360", {
			MATCH(DMI_BOARD_VENDOR, "IBM"),
			MATCH(DMI_BOARD_NAME, "eServer xSeries 360"),
			NO_MATCH, NO_MATCH }},

	{ force_acpi_ht, "IBM eserver xSeries 330", {
			MATCH(DMI_BOARD_VENDOR, "IBM"),
			MATCH(DMI_BOARD_NAME, "eserver xSeries 330"),
			NO_MATCH, NO_MATCH }},

	{ force_acpi_ht, "IBM eserver xSeries 440", {
			MATCH(DMI_BOARD_VENDOR, "IBM"),
			MATCH(DMI_PRODUCT_NAME, "eserver xSeries 440"),
			NO_MATCH, NO_MATCH }},

	/*
	 * Systems with nForce2 BIOS timer override bug
	 * nVidia claims all nForce have timer on pin0,
	 * and applying this workaround is a NOP on fixed BIOS,
	 * so prospects are good for replacing these entries
	 * with something to key of chipset PCI-ID.
	 */
	{ ignore_timer_override, "Abit NF7-S v2", {
			MATCH(DMI_BOARD_VENDOR, "http://www.abit.com.tw/"),
			MATCH(DMI_BOARD_NAME, "NF7-S/NF7,NF7-V (nVidia-nForce2)"),
			MATCH(DMI_BIOS_VERSION, "6.00 PG"),
			MATCH(DMI_BIOS_DATE, "03/24/2004") }},

	{ ignore_timer_override, "Asus A7N8X v2", {
			MATCH(DMI_BOARD_VENDOR, "ASUSTeK Computer INC."),
			MATCH(DMI_BOARD_NAME, "A7N8X2.0"),
			MATCH(DMI_BIOS_VERSION, "ASUS A7N8X2.0 Deluxe ACPI BIOS Rev 1007"),
			MATCH(DMI_BIOS_DATE, "10/06/2003") }},

	{ ignore_timer_override, "Asus A7N8X-X", {
			MATCH(DMI_BOARD_VENDOR, "ASUSTeK Computer INC."),
			MATCH(DMI_BOARD_NAME, "A7N8X-X"),
			MATCH(DMI_BIOS_VERSION, "ASUS A7N8X-X ACPI BIOS Rev 1009"),
			MATCH(DMI_BIOS_DATE, "2/3/2004") }},

	{ ignore_timer_override, "MSI K7N2-Delta", {
			MATCH(DMI_BOARD_VENDOR, "MICRO-STAR INTERNATIONAL CO., LTD"),
			MATCH(DMI_BOARD_NAME, "MS-6570"),
			MATCH(DMI_BIOS_VERSION, "6.00 PG"),
			MATCH(DMI_BIOS_DATE, "03/29/2004") }},

	{ ignore_timer_override, "Shuttle SN41G2", {
			MATCH(DMI_BOARD_VENDOR, "Shuttle Inc"),
			MATCH(DMI_BOARD_NAME, "FN41"),
			MATCH(DMI_BIOS_VERSION, "6.00 PG"),
			MATCH(DMI_BIOS_DATE, "01/14/2004") }},

	{ ignore_timer_override, "Shuttle AN35N", {
			MATCH(DMI_BOARD_VENDOR, "Shuttle Inc"),
			MATCH(DMI_BOARD_NAME, "AN35"),
			MATCH(DMI_BIOS_VERSION, "6.00 PG"),
			MATCH(DMI_BIOS_DATE, "12/05/2003") }},
#endif	// CONFIG_ACPI_BOOT

#ifdef	CONFIG_ACPI_PCI
	/*
	 *	Boxes that need ACPI PCI IRQ routing disabled
	 */

	{ disable_acpi_irq, "ASUS A7V", {
			MATCH(DMI_BOARD_VENDOR, "ASUSTeK Computer INC"),
			MATCH(DMI_BOARD_NAME, "<A7V>"),
			/* newer BIOS, Revision 1011, does work */
			MATCH(DMI_BIOS_VERSION, "ASUS A7V ACPI BIOS Revision 1007"),
			NO_MATCH }},

	/*
	 *	Boxes that need ACPI PCI IRQ routing and PCI scan disabled
	 */
	{ disable_acpi_pci, "ASUS PR-DLS", {	/* _BBN 0 bug */
			MATCH(DMI_BOARD_VENDOR, "ASUSTeK Computer INC."),
			MATCH(DMI_BOARD_NAME, "PR-DLS"),
			MATCH(DMI_BIOS_VERSION, "ASUS PR-DLS ACPI BIOS Revision 1010"),
			MATCH(DMI_BIOS_DATE, "03/21/2003") }},

 	{ disable_acpi_pci, "Acer TravelMate 36x Laptop", {
 			MATCH(DMI_SYS_VENDOR, "Acer"),
 			MATCH(DMI_PRODUCT_NAME, "TravelMate 360"),
 			NO_MATCH, NO_MATCH
 			} },

#endif

	{ NULL, }
};
	
	
/*
 *	Walk the blacklist table running matching functions until someone 
 *	returns 1 or we hit the end.
 */
 

static __init void dmi_check_blacklist(void)
{
#ifdef	CONFIG_ACPI_BOOT
#define	ACPI_BLACKLIST_CUTOFF_YEAR	2001

	if (dmi_ident[DMI_BIOS_DATE]) { 
		char *s = strrchr(dmi_ident[DMI_BIOS_DATE], '/'); 
		if (s) { 
			int year, disable = 0;
			s++; 
			year = simple_strtoul(s,NULL,0); 
			if (year >= 1000) 
				disable = year < ACPI_BLACKLIST_CUTOFF_YEAR; 
			else if (year < 1 || (year > 90 && year <= 99))
				disable = 1; 
			if (disable && !acpi_force) { 
				printk(KERN_NOTICE "ACPI disabled because your bios is from %s and too old\n", s);
				printk(KERN_NOTICE "You can enable it with acpi=force\n");
				disable_acpi();
			} 
		}
	}
#endif
 	dmi_check_system(dmi_blacklist);
}

	

/*
 *	Process a DMI table entry. Right now all we care about are the BIOS
 *	and machine entries. For 2.5 we should pull the smbus controller info
 *	out of here.
 */

static void __init dmi_decode(struct dmi_header *dm)
{
#ifdef DMI_DEBUG
	u8 *data = (u8 *)dm;
#endif
	
	switch(dm->type)
	{
		case  0:
			dmi_printk(("BIOS Vendor: %s\n",
				dmi_string(dm, data[4])));
			dmi_save_ident(dm, DMI_BIOS_VENDOR, 4);
			dmi_printk(("BIOS Version: %s\n", 
				dmi_string(dm, data[5])));
			dmi_save_ident(dm, DMI_BIOS_VERSION, 5);
			dmi_printk(("BIOS Release: %s\n",
				dmi_string(dm, data[8])));
			dmi_save_ident(dm, DMI_BIOS_DATE, 8);
			break;
		case 1:
			dmi_printk(("System Vendor: %s\n",
				dmi_string(dm, data[4])));
			dmi_save_ident(dm, DMI_SYS_VENDOR, 4);
			dmi_printk(("Product Name: %s\n",
				dmi_string(dm, data[5])));
			dmi_save_ident(dm, DMI_PRODUCT_NAME, 5);
			dmi_printk(("Version: %s\n",
				dmi_string(dm, data[6])));
			dmi_save_ident(dm, DMI_PRODUCT_VERSION, 6);
			dmi_printk(("Serial Number: %s\n",
				dmi_string(dm, data[7])));
			break;
		case 2:
			dmi_printk(("Board Vendor: %s\n",
				dmi_string(dm, data[4])));
			dmi_save_ident(dm, DMI_BOARD_VENDOR, 4);
			dmi_printk(("Board Name: %s\n",
				dmi_string(dm, data[5])));
			dmi_save_ident(dm, DMI_BOARD_NAME, 5);
			dmi_printk(("Board Version: %s\n",
				dmi_string(dm, data[6])));
			dmi_save_ident(dm, DMI_BOARD_VERSION, 6);
			break;
	}
}

void __init dmi_scan_machine(void)
{
	int err = dmi_iterate(dmi_decode);
	if(err == 0)
		dmi_check_blacklist();
	else
		printk(KERN_INFO "DMI not present.\n");
}


/**
 *	dmi_check_system - check system DMI data
 *	@list: array of dmi_system_id structures to match against
 *
 *	Walk the blacklist table running matching functions until someone
 *	returns non zero or we hit the end. Callback function is called for
 *	each successfull match. Returns the number of matches.
 */
int dmi_check_system(struct dmi_system_id *list)
{
	int i, count = 0;
	struct dmi_system_id *d = list;

	while (d->ident) {
		for (i = 0; i < ARRAY_SIZE(d->matches); i++) {
			int s = d->matches[i].slot;
			if (s == DMI_NONE)
				continue;
			if (dmi_ident[s] && strstr(dmi_ident[s], d->matches[i].substr))
				continue;
			/* No match */
			goto fail;
		}
		if (d->callback && d->callback(d))
			break;
		count++;
fail:		d++;
	}

	return count;
}

EXPORT_SYMBOL(dmi_check_system);

/**
 *	dmi_get_system_info - return DMI data value
 *	@field: data index (see enum dmi_filed)
 *
 *	Returns one DMI data value, can be used to perform
 *	complex DMI data checks.
 */
char * dmi_get_system_info(int field)
{
	return dmi_ident[field];
}

EXPORT_SYMBOL(dmi_get_system_info);
