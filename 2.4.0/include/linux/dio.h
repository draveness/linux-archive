/* header file for DIO boards for the HP300 architecture.
 * Maybe this should handle DIO-II later?
 * The general structure of this is vaguely based on how
 * the Amiga port handles Zorro boards.
 * Copyright (C) Peter Maydell 05/1998 <pmaydell@chiark.greenend.org.uk>
 *
 * The board IDs are from the NetBSD kernel, which for once provided
 * helpful comments...
 *
 * This goes with arch/m68k/hp300/dio.c
 */

#ifndef _LINUX_DIO_H
#define _LINUX_DIO_H

/* The DIO boards in a system are distinguished by 'select codes' which 
 * range from 0-63 (DIO) and 132-255 (DIO-II). 
 * The DIO board with select code sc is located at physical address 
 *     0x600000 + sc * 0x10000
 * So DIO cards cover [0x600000-0x800000); the areas [0x200000-0x400000) and
 * [0x800000-0x1000000) are for additional space required by things
 * like framebuffers. [0x400000-0x600000) is for miscellaneous internal I/O.
 * On Linux, this is currently all mapped into the virtual address space
 * at 0xf0000000 on bootup.
 * DIO-II boards are at 0x1000000 + (sc - 132) * 0x400000
 * which is address range [0x1000000-0x20000000) -- too big to map completely,
 * so currently we just don't handle DIO-II boards.  It wouldn't be hard to 
 * do with ioremap() though.
 */
#ifdef __KERNEL__
/* DIO/DIO-II boards all have the following 8bit registers.
 * These are offsets from the base of the device.
 */
#define DIO_IDOFF     0x01                        /* primary device ID */
#define DIO_IPLOFF    0x03                        /* interrupt priority level */
#define DIO_SECIDOFF  0x15                        /* secondary device ID */
#define DIOII_SIZEOFF 0x101                       /* device size, DIO-II only */

/* The internal HPIB device is special; this is its physaddr; its select code is 7. 
 * The reason why we have to treat it specially is because apparently it's broken:
 * the device ID isn't consistent/reliable. *sigh*
 */
#define DIO_IHPIBADDR 0x47800
#define DIO_IHPIBSCODE 7

/* If we don't have the internal HPIB defined, then treat select code 7 like
 * any other. If we *do* have internal HPIB, then we just have to assume that
 * select code 7 is the internal HPIB regardless of the ID register :-<
 */
#define CONFIG_IHPIB /* hack hack : not yet a proper config option */
#ifdef CONFIG_IHPIB
#define DIO_ISIHPIB(scode) ((scode) == DIO_IHPIBSCODE)
#else
#define DIO_ISIHPIB(scode) 0
#endif

#define DIO_VIRADDRBASE 0xf0000000                /* vir addr where IOspace is mapped */

#define DIO_BASE                0x600000        /* start of DIO space */
#define DIO_END                 0x1000000       /* end of DIO space */
#define DIO_DEVSIZE             0x10000         /* size of a DIO device */

#define DIOII_BASE              0x01000000      /* start of DIO-II space */
#define DIOII_END               0x20000000      /* end of DIO-II space */
#define DIOII_DEVSIZE           0x00400000      /* size of a DIO-II device */

/* Highest valid select code. If we add DIO-II support this should become
 * 256 for everything except HP320, which only has DIO.
 */
#define DIO_SCMAX 32                             
#define DIOII_SCBASE 132 /* lowest DIO-II select code */
#define DIO_SCINHOLE(scode) (((scode) >= 32) && ((scode) < DIOII_SCBASE))

/* macros to read device IDs, given base address */
#define DIO_ID(baseaddr) readb((baseaddr) + DIO_IDOFF)
#define DIO_SECID(baseaddr) readb((baseaddr) + DIO_SECIDOFF)

/* extract the interrupt level */
#define DIO_IPL(baseaddr) (((readb((baseaddr) + DIO_IPLOFF) >> 4) & 0x03) + 3)

/* find the size of a DIO-II board's address space.
 * DIO boards are all fixed length.
 */
#define DIOII_SIZE(baseaddr) ((readb((baseaddr) + DIOII_SIZEOFF) + 1) * 0x100000)

/* general purpose macro for both DIO and DIO-II */
#define DIO_SIZE(scode, base) (DIO_ISDIOII((scode)) ? DIOII_SIZE((base)) : DIO_DEVSIZE)

/* The hardware has primary and secondary IDs; we encode these in a single
 * int as PRIMARY ID & (SECONDARY ID << 8).
 * In practice this is only important for framebuffers,
 * and everybody else just sets ID fields equal to the DIO_ID_FOO value.
 */
#define DIO_ENCODE_ID(pr,sec) ((((int)sec & 0xff) << 8) & ((int)pr & 0xff))
/* macro to determine whether a given primary ID requires a secondary ID byte */
#define DIO_NEEDSSECID(id) ((id) == DIO_ID_FBUFFER)

/* Now a whole slew of macros giving device IDs and descriptive strings: */
#define DIO_ID_DCA0     0x02 /* 98644A serial */
#define DIO_DESC_DCA0 "98644A DCA0 serial"
#define DIO_ID_DCA0REM  0x82 /* 98644A serial */
#define DIO_DESC_DCA0REM "98644A DCA0REM serial"
#define DIO_ID_DCA1     0x42 /* 98644A serial */
#define DIO_DESC_DCA1 "98644A DCA1 serial"
#define DIO_ID_DCA1REM  0xc2 /* 98644A serial */
#define DIO_DESC_DCA1REM "98644A DCA1REM serial"
#define DIO_ID_DCM      0x05 /* 98642A serial MUX */
#define DIO_DESC_DCM "98642A DCM serial MUX"
#define DIO_ID_DCMREM   0x85 /* 98642A serial MUX */
#define DIO_DESC_DCMREM "98642A DCMREM serial MUX"
#define DIO_ID_LAN      0x15 /* 98643A LAN */
#define DIO_DESC_LAN "98643A LAN"
#define DIO_ID_FHPIB    0x08 /* 98625A/98625B fast HP-IB */
#define DIO_DESC_FHPIB "98625A/98625B fast HPIB"
#define DIO_ID_NHPIB    0x80 /* 98624A HP-IB (normal ie slow) */
#define DIO_DESC_NHPIB "98624A HPIB"
#define DIO_ID_IHPIB    0x00 /* internal HPIB (not its real ID, it hasn't got one! */
#define DIO_DESC_IHPIB "internal HPIB"
#define DIO_ID_SCSI0    0x07 /* 98625A SCSI */
#define DIO_DESC_SCSI0 "98625A SCSI0"
#define DIO_ID_SCSI1    0x27 /* ditto */
#define DIO_DESC_SCSI1 "98625A SCSI1"
#define DIO_ID_SCSI2    0x47 /* ditto */
#define DIO_DESC_SCSI2 "98625A SCSI2"
#define DIO_ID_SCSI3    0x67 /* ditto */
#define DIO_DESC_SCSI3 "98625A SCSI3"
#define DIO_ID_FBUFFER  0x39 /* framebuffer: flavour is distinguished by secondary ID */
#define DIO_DESC_FBUFFER "bitmapped display"
/* the NetBSD kernel source is a bit unsure as to what these next IDs actually do :-> */
#define DIO_ID_MISC0    0x03 /* 98622A */
#define DIO_DESC_MISC0 "98622A"
#define DIO_ID_MISC1    0x04 /* 98623A */
#define DIO_DESC_MISC1 "98623A"
#define DIO_ID_PARALLEL 0x06 /* internal parallel */
#define DIO_DESC_PARALLEL "internal parallel"
#define DIO_ID_MISC2    0x09 /* 98287A keyboard */
#define DIO_DESC_MISC2 "98287A keyboard"
#define DIO_ID_MISC3    0x0a /* HP98635A FP accelerator */
#define DIO_DESC_MISC3 "HP98635A FP accelerator"
#define DIO_ID_MISC4    0x0b /* timer */
#define DIO_DESC_MISC4 "timer"
#define DIO_ID_MISC5    0x12 /* 98640A */
#define DIO_DESC_MISC5 "98640A"
#define DIO_ID_MISC6    0x16 /* 98659A */
#define DIO_DESC_MISC6 "98659A"
#define DIO_ID_MISC7    0x19 /* 237 display */
#define DIO_DESC_MISC7 "237 display"
#define DIO_ID_MISC8    0x1a /* quad-wide card */
#define DIO_DESC_MISC8 "quad-wide card"
#define DIO_ID_MISC9    0x1b /* 98253A */
#define DIO_DESC_MISC9 "98253A"
#define DIO_ID_MISC10   0x1c /* 98627A */
#define DIO_DESC_MISC10 "98253A"
#define DIO_ID_MISC11   0x1d /* 98633A */
#define DIO_DESC_MISC11 "98633A"
#define DIO_ID_MISC12   0x1e /* 98259A */
#define DIO_DESC_MISC12 "98259A"
#define DIO_ID_MISC13   0x1f /* 8741 */
#define DIO_DESC_MISC13 "8741"
#define DIO_ID_VME      0x31 /* 98577A VME adapter */
#define DIO_DESC_VME "98577A VME adapter"
#define DIO_ID_DCL      0x34 /* 98628A serial */
#define DIO_DESC_DCL "98628A DCL serial"
#define DIO_ID_DCLREM   0xb4 /* 98628A serial */
#define DIO_DESC_DCLREM "98628A DCLREM serial"
/* These are the secondary IDs for the framebuffers */
#define DIO_ID2_GATORBOX    0x01 /* 98700/98710 "gatorbox" */
#define DIO_DESC2_GATORBOX       "98700/98710 \"gatorbox\" display"
#define DIO_ID2_TOPCAT      0x02 /* 98544/98545/98547 "topcat" */
#define DIO_DESC2_TOPCAT         "98544/98545/98547 \"topcat\" display"
#define DIO_ID2_RENAISSANCE 0x04 /* 98720/98721 "renaissance" */
#define DIO_DESC2_RENAISSANCE    "98720/98721 \"renaissance\" display"
#define DIO_ID2_LRCATSEYE   0x05 /* lowres "catseye" */
#define DIO_DESC2_LRCATSEYE      "low-res catseye display"
#define DIO_ID2_HRCCATSEYE  0x06 /* highres colour "catseye" */
#define DIO_DESC2_HRCCATSEYE     "high-res color catseye display"
#define DIO_ID2_HRMCATSEYE  0x07 /* highres mono "catseye" */
#define DIO_DESC2_HRMCATSEYE     "high-res mono catseye display"
#define DIO_ID2_DAVINCI     0x08 /* 98730/98731 "davinci" */
#define DIO_DESC2_DAVINCI        "98730/98731 \"davinci\" display"
#define DIO_ID2_XXXCATSEYE  0x09 /* "catseye" */
#define DIO_DESC2_XXXCATSEYE     "catseye display"
#define DIO_ID2_HYPERION    0x0e /* A1096A "hyperion" */
#define DIO_DESC2_HYPERION       "A1096A \"hyperion\" display"
#define DIO_ID2_XGENESIS    0x0b /* "x-genesis"; no NetBSD support */
#define DIO_DESC2_XGENESIS       "\"x-genesis\" display"
#define DIO_ID2_TIGER       0x0c /* "tiger"; no NetBSD support */
#define DIO_DESC2_TIGER          "\"tiger\" display"
#define DIO_ID2_YGENESIS    0x0d /* "y-genesis"; no NetBSD support */
#define DIO_DESC2_YGENESIS       "\"y-genesis\" display"
/* if you add new IDs then you should tell dio.c about them so it can
 * identify them...
 */

extern void dio_init(void);
extern int dio_find(int deviceid);
extern void *dio_scodetoviraddr(int scode);
extern int dio_scodetoipl(int scode);
extern void dio_config_board(int scode);
extern void dio_unconfig_board(int scode);


#endif /* __KERNEL__ */
#endif /* ndef _LINUX_DIO_H */
