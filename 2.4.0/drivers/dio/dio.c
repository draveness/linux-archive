/* Code to support devices on the DIO (and eventually DIO-II) bus
 * Copyright (C) 05/1998 Peter Maydell <pmaydell@chiark.greenend.org.uk>
 * 
 * This code has basically these routines at the moment:
 * int dio_find(u_int deviceid)
 *    Search the list of DIO devices and return the select code
 *    of the next unconfigured device found that matches the given device ID.
 *    Note that the deviceid parameter should be the encoded ID.
 *    This means that framebuffers should pass it as 
 *    DIO_ENCODE_ID(DIO_ID_FBUFFER,DIO_ID2_TOPCAT)
 *    (or whatever); everybody else just uses DIO_ID_FOOBAR.
 * void *dio_scodetoviraddr(int scode)
 *    Return the virtual address corresponding to the given select code.
 *    NB: DIO-II devices will have to be mapped in in this routine!
 * int dio_scodetoipl(int scode)
 *    Every DIO card has a fixed interrupt priority level. This function 
 *    returns it, whatever it is.
 * const char *dio_scodetoname(int scode)
 *    Return a character string describing this board [might be "" if 
 *    not CONFIG_DIO_CONSTANTS]
 * void dio_config_board(int scode)     mark board as configured in the list
 * void dio_unconfig_board(int scode)   mark board as no longer configured
 *
 * This file is based on the way the Amiga port handles Zorro II cards, 
 * although we aren't so complicated...
 */
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/dio.h>
#include <linux/malloc.h>                         /* kmalloc() */
#include <linux/init.h>
#include <asm/hwtest.h>                           /* hwreg_present() */
#include <asm/io.h>                               /* readb() */
/* not a real config option yet! */
#define CONFIG_DIO_CONSTANTS

#ifdef CONFIG_DIO_CONSTANTS
/* We associate each numeric ID with an appropriate descriptive string
 * using a constant array of these structs.
 * FIXME: we should be able to arrange to throw away most of the strings
 * using the initdata stuff. Then we wouldn't need to worry about 
 * carrying them around...
 * I think we do this by copying them into newly kmalloc()ed memory and 
 * marking the names[] array as .initdata ?
 */
struct dioname
{
        int id;
        const char *name;
};

/* useful macro */
#define DIONAME(x) { DIO_ID_##x, DIO_DESC_##x }
#define DIOFBNAME(x) { DIO_ENCODE_ID( DIO_ID_FBUFFER, DIO_ID2_##x), DIO_DESC2_##x }

static struct dioname names[] = 
{
        DIONAME(DCA0), DIONAME(DCA0REM), DIONAME(DCA1), DIONAME(DCA1REM),
        DIONAME(DCM), DIONAME(DCMREM),
        DIONAME(LAN),
        DIONAME(FHPIB), DIONAME(NHPIB), DIONAME(IHPIB),
        DIONAME(SCSI0), DIONAME(SCSI1), DIONAME(SCSI2), DIONAME(SCSI3),
        DIONAME(FBUFFER),
        DIONAME(PARALLEL), DIONAME(VME), DIONAME(DCL), DIONAME(DCLREM),
        DIONAME(MISC0), DIONAME(MISC1), DIONAME(MISC2), DIONAME(MISC3),
        DIONAME(MISC4), DIONAME(MISC5), DIONAME(MISC6), DIONAME(MISC7),
        DIONAME(MISC8), DIONAME(MISC9), DIONAME(MISC10), DIONAME(MISC11), 
        DIONAME(MISC12), DIONAME(MISC13),
        DIOFBNAME(GATORBOX), DIOFBNAME(TOPCAT), DIOFBNAME(RENAISSANCE),
        DIOFBNAME(LRCATSEYE), DIOFBNAME(HRCCATSEYE), DIOFBNAME(HRMCATSEYE),
        DIOFBNAME(DAVINCI), DIOFBNAME(XXXCATSEYE), DIOFBNAME(HYPERION),
        DIOFBNAME(XGENESIS), DIOFBNAME(TIGER), DIOFBNAME(YGENESIS)   
};

#undef DIONAME
#undef DIOFBNAME

#define NUMNAMES (sizeof(names) / sizeof(struct dioname))

static const char *unknowndioname 
        = "unknown DIO board -- please email <pmaydell@chiark.greenend.org.uk>!";

static const char *dio_getname(int id)
{
        /* return pointer to a constant string describing the board with given ID */
	unsigned int i;
        for (i = 0; i < NUMNAMES; i++)
                if (names[i].id == id) 
                        return names[i].name;
        
        return unknowndioname;
}

#else

static char dio_no_name[] = { 0 };
#define dio_getname(_id)	(dio_no_name)

#endif /* CONFIG_DIO_CONSTANTS */

/* We represent all the DIO boards in the system with a linked list of these structs. */
struct dioboard
{
        struct dioboard *next;                    /* link to next struct in list */
        int ipl;                                  /* IPL of this board */
        int configured;                           /* has this board been configured? */
        int scode;                                /* select code of this board */
        int id;                                   /* encoded ID */
        const char *name;
};

static struct dioboard *blist = NULL;

static int __init dio_find_slow(int deviceid)
{
	/* Called to find a DIO device before the full bus scan has run.  Basically
	   only used by the console driver.  */
	int scode;
	for (scode = 0; scode < DIO_SCMAX; scode++)
	{
		void *va;

                if (DIO_SCINHOLE(scode))
                        continue;
                
                va = dio_scodetoviraddr(scode);
                if (!va || !hwreg_present(va + DIO_IDOFF))
                        continue;             /* no board present at that select code */

		if (DIO_ID(va) == deviceid)
			return scode;
	}
	return 0;
}

int dio_find(int deviceid)
{
	if (blist) 
	{
		/* fast way */
		struct dioboard *b;
		for (b = blist; b; b = b->next)
			if (b->id == deviceid && b->configured == 0)
				return b->scode;
		return 0;
	}
	return dio_find_slow(deviceid);
}

/* This is the function that scans the DIO space and works out what
 * hardware is actually present.
 */
void __init dio_init(void)
{
        int scode;
        struct dioboard *b, *bprev = NULL;
   
        printk("Scanning for DIO devices...\n");
        
        for (scode = 0; scode < DIO_SCMAX; ++scode)
        {
                u_char prid, secid = 0;        /* primary, secondary ID bytes */
                u_char *va;
                
                if (DIO_SCINHOLE(scode))
                        continue;
                
                va = dio_scodetoviraddr(scode);
                if (!va || !hwreg_present(va + DIO_IDOFF))
                        continue;              /* no board present at that select code */

                /* Found a board, allocate it an entry in the list */
                b = kmalloc(sizeof(struct dioboard), GFP_KERNEL);
                
                /* read the ID byte(s) and encode if necessary. Note workaround 
                 * for broken internal HPIB devices...
                 */
                if (!DIO_ISIHPIB(scode))
                        prid = DIO_ID(va);
                else 
                        prid = DIO_ID_IHPIB;
                
                if (DIO_NEEDSSECID(prid))
                {
                        secid = DIO_SECID(va);
                        b->id = DIO_ENCODE_ID(prid, secid);
                }
                else
                        b->id = prid;
      
                b->configured = 0;
                b->scode = scode;
                b->ipl = DIO_IPL(va);
                b->name = dio_getname(b->id);
                printk("select code %3d: ID %02X", scode, prid);
                if (DIO_NEEDSSECID(b->id))
                        printk(":%02X", secid);
                printk(" %s\n", b->name);
                
                b->next = NULL;

                if (bprev)
                        bprev->next = b;
                else
                        blist = b;
                bprev = b;
        }
}

/* Bear in mind that this is called in the very early stages of initialisation
 * in order to get the virtual address of the serial port for the console...
 */
void *dio_scodetoviraddr(int scode)
{
        if (scode > DIOII_SCBASE)
        {
                printk("dio_scodetoviraddr: don't support DIO-II yet!\n");
                return 0;
        }
        else if (scode > DIO_SCMAX || scode < 0)
                return 0;
        else if (DIO_SCINHOLE(scode))
                return 0;
        else if (scode == DIO_IHPIBSCODE) /* this should really be #ifdef CONFIG_IHPIB */
                return (void*)DIO_IHPIBADDR;   /* or something similar... */
        
        return (void*)(DIO_VIRADDRBASE + DIO_BASE + scode * 0x10000);
}

int dio_scodetoipl(int scode)
{
        struct dioboard *b;
        for (b = blist; b; b = b->next)
                if (b->scode == scode) 
                        break;
        
        if (!b)
        {
                printk("dio_scodetoipl: bad select code %d\n", scode);
                return 0;
        }
        else
                return b->ipl;
}

const char *dio_scodetoname(int scode)
{
        struct dioboard *b;
        for (b = blist; b; b = b->next)
                if (b->scode == scode) 
                        break;
        
        if (!b)
        {
                printk("dio_scodetoname: bad select code %d\n", scode);
                return NULL;
        }
        else
                return b->name;
}

void dio_config_board(int scode)
{
        struct dioboard *b;
        for (b = blist; b; b = b->next)
                if (b->scode == scode)
                        break;
   
        if (!b) 
                printk("dio_config_board: bad select code %d\n", scode);
        else if (b->configured)
                printk("dio_config_board: board at select code %d already configured\n", scode);
        else
                b->configured = 1;
}

void dio_unconfig_board(int scode)
{
        struct dioboard *b;
        for (b = blist; b; b = b->next)
                if (b->scode == scode) 
                        break;
   
        if (!b) 
                printk("dio_unconfig_board: bad select code %d\n", scode);
        else if (!b->configured)
                printk("dio_unconfig_board: board at select code %d not configured\n", 
		       scode);
        else 
                b->configured = 0;
}
