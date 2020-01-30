/*
 *
 * Hardware accelerated Matrox Millennium I, II, Mystique, G100, G200 and G400
 *
 * (c) 1998,1999,2000 Petr Vandrovec <vandrove@vc.cvut.cz>
 *
 */
#ifndef __MATROXFB_H__
#define __MATROXFB_H__

/* general, but fairly heavy, debugging */
#undef MATROXFB_DEBUG

/* heavy debugging: */
/* -- logs putc[s], so everytime a char is displayed, it's logged */
#undef MATROXFB_DEBUG_HEAVY

/* This one _could_ cause infinite loops */
/* It _does_ cause lots and lots of messages during idle loops */
#undef MATROXFB_DEBUG_LOOP

/* Debug register calls, too? */
#undef MATROXFB_DEBUG_REG

/* Guard accelerator accesses with spin_lock_irqsave... */
#undef MATROXFB_USE_SPINLOCKS

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/malloc.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/console.h>
#include <linux/selection.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/pci.h>
#include <linux/spinlock.h>

#include <asm/io.h>
#include <asm/unaligned.h>
#ifdef CONFIG_MTRR
#include <asm/mtrr.h>
#endif

#include <video/fbcon.h>
#include <video/fbcon-cfb4.h>
#include <video/fbcon-cfb8.h>
#include <video/fbcon-cfb16.h>
#include <video/fbcon-cfb24.h>
#include <video/fbcon-cfb32.h>

#if defined(CONFIG_FB_COMPAT_XPMAC)
#include <asm/vc_ioctl.h>
#endif
#if defined(CONFIG_PPC)
#include <asm/prom.h>
#include <asm/pci-bridge.h>
#include <video/macmodes.h>
#endif

/* always compile support for 32MB... It cost almost nothing */
#define CONFIG_FB_MATROX_32MB

#define FBCON_HAS_VGATEXT

#ifdef MATROXFB_DEBUG

#define DEBUG
#define DBG(x)		printk(KERN_DEBUG "matroxfb: %s\n", (x));

#ifdef MATROXFB_DEBUG_HEAVY
#define DBG_HEAVY(x)	DBG(x)
#else /* MATROXFB_DEBUG_HEAVY */
#define DBG_HEAVY(x)	/* DBG_HEAVY */
#endif /* MATROXFB_DEBUG_HEAVY */

#ifdef MATROXFB_DEBUG_LOOP
#define DBG_LOOP(x)	DBG(x)
#else /* MATROXFB_DEBUG_LOOP */
#define DBG_LOOP(x)	/* DBG_LOOP */
#endif /* MATROXFB_DEBUG_LOOP */

#ifdef MATROXFB_DEBUG_REG
#define DBG_REG(x)	DBG(x)
#else /* MATROXFB_DEBUG_REG */
#define DBG_REG(x)	/* DBG_REG */
#endif /* MATROXFB_DEBUG_REG */

#else /* MATROXFB_DEBUG */

#define DBG(x)		/* DBG */
#define DBG_HEAVY(x)	/* DBG_HEAVY */
#define DBG_REG(x)	/* DBG_REG */
#define DBG_LOOP(x)	/* DBG_LOOP */

#endif /* MATROXFB_DEBUG */

#ifndef __i386__
#ifndef ioremap_nocache
#define ioremap_nocache(X,Y) ioremap(X,Y)
#endif
#endif

#if defined(__alpha__) || defined(__m68k__)
#define READx_WORKS
#define MEMCPYTOIO_WORKS
#else
#define READx_FAILS
/* recheck __ppc__, maybe that __ppc__ needs MEMCPYTOIO_WRITEL */
/* I benchmarked PII/350MHz with G200... MEMCPY, MEMCPYTOIO and WRITEL are on same speed ( <2% diff) */
/* so that means that G200 speed (or AGP speed?) is our limit... I do not have benchmark to test, how */
/* much of PCI bandwidth is used during transfers... */
#if defined(__i386__)
#define MEMCPYTOIO_MEMCPY
#else
#define MEMCPYTOIO_WRITEL
#endif
#endif

#ifdef __sparc__
#error "Sorry, I have no idea how to do this on sparc... There is mapioaddr... With bus_type parameter..."
#endif

#if defined(__m68k__)
#define MAP_BUSTOVIRT
#else
#define MAP_IOREMAP
#endif

#ifdef DEBUG
#define dprintk(X...)	printk(X)
#else
#define dprintk(X...)
#endif

#ifndef PCI_SS_VENDOR_ID_SIEMENS_NIXDORF
#define PCI_SS_VENDOR_ID_SIEMENS_NIXDORF	0x110A
#endif
#ifndef PCI_SS_VENDOR_ID_MATROX
#define PCI_SS_VENDOR_ID_MATROX		PCI_VENDOR_ID_MATROX
#endif
#ifndef PCI_DEVICE_ID_MATROX_G200_PCI
#define PCI_DEVICE_ID_MATROX_G200_PCI	0x0520
#endif
#ifndef PCI_DEVICE_ID_MATROX_G200_AGP
#define PCI_DEVICE_ID_MATROX_G200_AGP	0x0521
#endif
#ifndef PCI_DEVICE_ID_MATROX_G100
#define PCI_DEVICE_ID_MATROX_G100	0x1000
#endif
#ifndef PCI_DEVICE_ID_MATROX_G100_AGP
#define PCI_DEVICE_ID_MATROX_G100_AGP	0x1001
#endif
#ifndef PCI_DEVICE_ID_MATROX_G400_AGP
#define PCI_DEVICE_ID_MATROX_G400_AGP	0x0525
#endif

#ifndef PCI_SS_ID_MATROX_PRODUCTIVA_G100_AGP
#define PCI_SS_ID_MATROX_GENERIC		0xFF00
#define PCI_SS_ID_MATROX_PRODUCTIVA_G100_AGP	0xFF01
#define PCI_SS_ID_MATROX_MYSTIQUE_G200_AGP	0xFF02
#define PCI_SS_ID_MATROX_MILLENIUM_G200_AGP	0xFF03
#define PCI_SS_ID_MATROX_MARVEL_G200_AGP	0xFF04
#define PCI_SS_ID_MATROX_MGA_G100_PCI		0xFF05
#define PCI_SS_ID_MATROX_MGA_G100_AGP		0x1001
#define PCI_SS_ID_MATROX_MILLENNIUM_G400_MAX_AGP	0x2179
#define PCI_SS_ID_SIEMENS_MGA_G100_AGP		0x001E /* 30 */
#define PCI_SS_ID_SIEMENS_MGA_G200_AGP		0x0032 /* 50 */
#endif

#define MX_VISUAL_TRUECOLOR	FB_VISUAL_DIRECTCOLOR
#define MX_VISUAL_DIRECTCOLOR	FB_VISUAL_TRUECOLOR
#define MX_VISUAL_PSEUDOCOLOR	FB_VISUAL_PSEUDOCOLOR

#define CNVT_TOHW(val,width) ((((val)<<(width))+0x7FFF-(val))>>16)

/* G100, G200 and Mystique have (almost) same DAC */
#undef NEED_DAC1064
#if defined(CONFIG_FB_MATROX_MYSTIQUE) || defined(CONFIG_FB_MATROX_G100)
#define NEED_DAC1064 1
#endif

typedef struct {
	u_int8_t*	vaddr;
} vaddr_t;

#ifdef READx_WORKS
static inline unsigned int mga_readb(vaddr_t va, unsigned int offs) {
	return readb(va.vaddr + offs);
}

static inline unsigned int mga_readw(vaddr_t va, unsigned int offs) {
	return readw(va.vaddr + offs);
}

static inline u_int32_t mga_readl(vaddr_t va, unsigned int offs) {
	return readl(va.vaddr + offs);
}

static inline void mga_writeb(vaddr_t va, unsigned int offs, u_int8_t value) {
	writeb(value, va.vaddr + offs);
}

static inline void mga_writew(vaddr_t va, unsigned int offs, u_int16_t value) {
	writew(value, va.vaddr + offs);
}

static inline void mga_writel(vaddr_t va, unsigned int offs, u_int32_t value) {
	writel(value, va.vaddr + offs);
}
#else
static inline unsigned int mga_readb(vaddr_t va, unsigned int offs) {
	return *(volatile u_int8_t*)(va.vaddr + offs);
}

static inline unsigned int mga_readw(vaddr_t va, unsigned int offs) {
	return *(volatile u_int16_t*)(va.vaddr + offs);
}

static inline u_int32_t mga_readl(vaddr_t va, unsigned int offs) {
	return *(volatile u_int32_t*)(va.vaddr + offs);
}

static inline void mga_writeb(vaddr_t va, unsigned int offs, u_int8_t value) {
	*(volatile u_int8_t*)(va.vaddr + offs) = value;
}

static inline void mga_writew(vaddr_t va, unsigned int offs, u_int16_t value) {
	*(volatile u_int16_t*)(va.vaddr + offs) = value;
}

static inline void mga_writel(vaddr_t va, unsigned int offs, u_int32_t value) {
	*(volatile u_int32_t*)(va.vaddr + offs) = value;
}
#endif

static inline void mga_memcpy_toio(vaddr_t va, unsigned int offs, const void* src, int len) {
#ifdef MEMCPYTOIO_WORKS
	memcpy_toio(va.vaddr + offs, src, len);
#elif defined(MEMCPYTOIO_WRITEL)
#define srcd ((const u_int32_t*)src)
	if (offs & 3) {
		while (len >= 4) {
			mga_writel(va, offs, get_unaligned(srcd++));
			offs += 4;
			len -= 4;
		}
	} else {
		while (len >= 4) {
			mga_writel(va, offs, *srcd++);
			offs += 4;
			len -= 4;
		}
	}
#undef srcd
	if (len) {
		u_int32_t tmp;

		memcpy(&tmp, src, len);
		mga_writel(va, offs, tmp);
	}
#elif defined(MEMCPYTOIO_MEMCPY)
	memcpy(va.vaddr + offs, src, len);
#else
#error "Sorry, do not know how to write block of data to device"
#endif
}

static inline void vaddr_add(vaddr_t* va, unsigned long offs) {
	va->vaddr += offs;
}

static inline void* vaddr_va(vaddr_t va) {
	return va.vaddr;
}

#define MGA_IOREMAP_NORMAL	0
#define MGA_IOREMAP_NOCACHE	1

#define MGA_IOREMAP_FB		MGA_IOREMAP_NOCACHE
#define MGA_IOREMAP_MMIO	MGA_IOREMAP_NOCACHE
static inline int mga_ioremap(unsigned long phys, unsigned long size, int flags, vaddr_t* virt) {
#ifdef MAP_IOREMAP
	if (flags & MGA_IOREMAP_NOCACHE)
		virt->vaddr = ioremap_nocache(phys, size);
	else
		virt->vaddr = ioremap(phys, size);
#else
#ifdef MAP_BUSTOVIRT
	virt->vaddr = bus_to_virt(phys);
#else
#error "Your architecture does not have neither ioremap nor bus_to_virt... Giving up"
#endif
#endif
	return (virt->vaddr == 0); /* 0, !0... 0, error_code in future */
}

static inline void mga_iounmap(vaddr_t va) {
#ifdef MAP_IOREMAP
	iounmap(va.vaddr);
#endif
}

struct my_timming {
	unsigned int pixclock;
	unsigned int HDisplay;
	unsigned int HSyncStart;
	unsigned int HSyncEnd;
	unsigned int HTotal;
	unsigned int VDisplay;
	unsigned int VSyncStart;
	unsigned int VSyncEnd;
	unsigned int VTotal;
	unsigned int sync;
	int	     dblscan;
	int	     interlaced;
	unsigned int delay;	/* CRTC delay */
};

struct matrox_pll_features {
	unsigned int	vco_freq_min;
	unsigned int	ref_freq;
	unsigned int	feed_div_min;
	unsigned int	feed_div_max;
	unsigned int	in_div_min;
	unsigned int	in_div_max;
	unsigned int	post_shift_max;
};

struct matroxfb_par
{
	unsigned int	final_bppShift;
	unsigned int	cmap_len;
	struct {
		unsigned int bytes;
		unsigned int pixels;
		unsigned int chunks;
		      } ydstorg;
	void		(*putc)(u_int32_t, u_int32_t, struct display*, int, int, int);
	void		(*putcs)(u_int32_t, u_int32_t, struct display*, const unsigned short*, int, int, int);
};

struct matrox_fb_info;

struct matrox_DAC1064_features {
	u_int8_t	xvrefctrl;
	u_int8_t	xmiscctrl;
	unsigned int	cursorimage;
};

struct matrox_accel_features {
	int		has_cacheflush;
};

/* current hardware status */
struct mavenregs {
	u_int8_t regs[256];
	int	 mode;
	int	 vlines;
	int	 xtal;
	int	 fv;

	u_int16_t htotal;
	u_int16_t hcorr;
};

struct matrox_hw_state {
	u_int32_t	MXoptionReg;
	unsigned char	DACclk[6];
	unsigned char	DACreg[80];
	unsigned char	MiscOutReg;
	unsigned char	DACpal[768];
	unsigned char	CRTC[25];
	unsigned char	CRTCEXT[9];
	unsigned char	SEQ[5];
	/* unused for MGA mode, but who knows... */
	unsigned char	GCTL[9];
	/* unused for MGA mode, but who knows... */
	unsigned char	ATTR[21];

	/* TVOut only */
	struct mavenregs	maven;

	/* CRTC2 only */
	/* u_int32_t	TBD */
};

struct matrox_accel_data {
#ifdef CONFIG_FB_MATROX_MILLENIUM
	unsigned char	ramdac_rev;
#endif
	u_int32_t	m_dwg_rect;
	u_int32_t	m_opmode;
};

struct matrox_altout {
	int		(*compute)(void* altout_dev, struct my_timming* input, struct matrox_hw_state* state);
	int		(*program)(void* altout_dev, const struct matrox_hw_state* state);
	int		(*start)(void* altout_dev);
	void		(*incuse)(void* altout_dev);
	void		(*decuse)(void* altout_dev);
	int		(*setmode)(void* altout_dev, u_int32_t mode);
	int		(*getmode)(void* altout_dev, u_int32_t* mode);
};

struct matrox_switch;
struct matroxfb_driver;

struct matrox_fb_info {
	/* fb_info must be first */
	struct fb_info		fbcon;

	struct list_head	next_fb;

	int			dead;
	unsigned int		usecount;

	struct matroxfb_par	curr;
	struct matrox_hw_state	hw1;
	struct matrox_hw_state	hw2;
	struct matrox_hw_state*	newhw;
	struct matrox_hw_state*	currenthw;

	struct matrox_accel_data accel;

	struct pci_dev*		pcidev;

	struct {
		u_int32_t	all;
		u_int32_t	ph;
		u_int32_t	sh;
			      } output;
	struct matrox_altout*	primout;
	struct {
	struct fb_info*		info;
	struct rw_semaphore	lock;
			      } crtc2;
	struct {
	struct matrox_altout*	output;
	void*			device;
	struct rw_semaphore	lock;
			      } altout;

#define MATROXFB_MAX_FB_DRIVERS		5
	struct matroxfb_driver* (drivers[MATROXFB_MAX_FB_DRIVERS]);
	void*			(drivers_data[MATROXFB_MAX_FB_DRIVERS]);
	unsigned int		drivers_count;

	struct {
	unsigned long	base;	/* physical */
	vaddr_t		vbase;	/* CPU view */
	unsigned int	len;
	unsigned int	len_usable;
	unsigned int	len_maximum;
		      } video;

	struct {
	unsigned long	base;	/* physical */
	vaddr_t		vbase;	/* CPU view */
	unsigned int	len;
		      } mmio;

	unsigned int	max_pixel_clock;

	struct matrox_switch*	hw_switch;
	int		currcon;
	struct display*	currcon_display;

	struct {
		struct matrox_pll_features pll;
		struct matrox_DAC1064_features DAC1064;
		struct matrox_accel_features accel;
			      } features;
	struct {
		spinlock_t	DAC;
		spinlock_t	accel;
			      } lock;

	int			interleave;
	int			millenium;
	int			milleniumII;
	struct {
		int		cfb4;
		const int*	vxres;
		int		cross4MB;
		int		text;
		int		plnwt;
			      } capable;
	struct {
		unsigned int	size;
		unsigned int	mgabase;
		vaddr_t		vbase;
			      } fastfont;
#ifdef CONFIG_MTRR
	struct {
		int		vram;
		int		vram_valid;
			      } mtrr;
#endif
	struct {
		int		precise_width;
		int		mga_24bpp_fix;
		int		novga;
		int		nobios;
		int		nopciretry;
		int		noinit;
		int		inverse;
		int		hwcursor;
		int		blink;
		int		sgram;
#ifdef CONFIG_FB_MATROX_32MB
		int		support32MB;
#endif

		int		accelerator;
		int		text_type_aux;
		int		video64bits;
		int		crtc2;
		int		maven_capable;
		unsigned int	vgastep;
		unsigned int	textmode;
		unsigned int	textstep;
		unsigned int	textvram;	/* character cells */
		unsigned int	ydstorg;	/* offset in bytes from video start to usable memory */
						/* 0 except for 6MB Millenium */
		int		memtype;
		int		g450dac;
			      } devflags;
	struct display_switch	dispsw;
	struct {
		int		x;
		int		y;
		unsigned int	w;
		unsigned int	u;
		unsigned int	d;
		unsigned int	type;
		int		state;
		int		redraw;
		struct timer_list timer;
			      } cursor;
	struct { unsigned red, green, blue, transp; } palette[256];
#if defined(CONFIG_FB_COMPAT_XPMAC)
	char	matrox_name[32];
#endif
/* These ifdefs must be last! They differ for module & non-module compiles */
#if defined(FBCON_HAS_CFB16) || defined(FBCON_HAS_CFB24) || defined(FBCON_HAS_CFB32)
	union {
#ifdef FBCON_HAS_CFB16
		u_int16_t	cfb16[16];
#endif
#ifdef FBCON_HAS_CFB24
		u_int32_t	cfb24[16];
#endif
#ifdef FBCON_HAS_CFB32
		u_int32_t	cfb32[16];
#endif
	} cmap;
#endif
};

#ifdef CONFIG_FB_MATROX_MULTIHEAD
#define ACCESS_FBINFO2(info, x) (info->x)
#define ACCESS_FBINFO(x) ACCESS_FBINFO2(minfo,x)

#define MINFO minfo

#define WPMINFO2 struct matrox_fb_info* minfo
#define WPMINFO  WPMINFO2 ,
#define CPMINFO2 const struct matrox_fb_info* minfo
#define CPMINFO	 CPMINFO2 ,
#define PMINFO2  minfo
#define PMINFO   PMINFO2 ,

static inline struct matrox_fb_info* mxinfo(const struct display* p) {
	return (struct matrox_fb_info*)p->fb_info;
}

#define PMXINFO(p)	   mxinfo(p),
#define MINFO_FROM(x)	   struct matrox_fb_info* minfo = x
#define MINFO_FROM_DISP(x) MINFO_FROM(mxinfo(x))

#else

extern struct matrox_fb_info matroxfb_global_mxinfo;
struct display global_disp;

#define ACCESS_FBINFO(x) (matroxfb_global_mxinfo.x)
#define ACCESS_FBINFO2(info, x) (matroxfb_global_mxinfo.x)

#define MINFO (&matroxfb_global_mxinfo)

#define WPMINFO2 void
#define WPMINFO
#define CPMINFO2 void
#define CPMINFO
#define PMINFO2
#define PMINFO

#if 0
static inline struct matrox_fb_info* mxinfo(const struct display* p) {
	return &matroxfb_global_mxinfo;
}
#endif

#define PMXINFO(p)
#define MINFO_FROM(x)
#define MINFO_FROM_DISP(x)

#endif

struct matrox_switch {
	int	(*preinit)(WPMINFO struct matrox_hw_state*);
	void	(*reset)(WPMINFO struct matrox_hw_state*);
	int	(*init)(CPMINFO struct matrox_hw_state*, struct my_timming*, struct display*);
	void	(*restore)(WPMINFO struct matrox_hw_state*, struct matrox_hw_state*, struct display*);
	int	(*selhwcursor)(WPMINFO struct display*);
};

struct matroxfb_driver {
	struct list_head	node;
	char*			name;
	void*			(*probe)(struct matrox_fb_info* info);
	void			(*remove)(struct matrox_fb_info* info, void* data);
};

int matroxfb_register_driver(struct matroxfb_driver* drv);
void matroxfb_unregister_driver(struct matroxfb_driver* drv);

#define PCI_OPTION_REG	0x40
#define PCI_MGA_INDEX	0x44
#define PCI_MGA_DATA	0x48

#define M_DWGCTL	0x1C00
#define M_MACCESS	0x1C04
#define M_CTLWTST	0x1C08

#define M_PLNWT		0x1C1C

#define M_BCOL		0x1C20
#define M_FCOL		0x1C24

#define M_SGN		0x1C58
#define M_LEN		0x1C5C
#define M_AR0		0x1C60
#define M_AR1		0x1C64
#define M_AR2		0x1C68
#define M_AR3		0x1C6C
#define M_AR4		0x1C70
#define M_AR5		0x1C74
#define M_AR6		0x1C78

#define M_CXBNDRY	0x1C80
#define M_FXBNDRY	0x1C84
#define M_YDSTLEN	0x1C88
#define M_PITCH		0x1C8C
#define M_YDST		0x1C90
#define M_YDSTORG	0x1C94
#define M_YTOP		0x1C98
#define M_YBOT		0x1C9C

/* mystique only */
#define M_CACHEFLUSH	0x1FFF

#define M_EXEC		0x0100

#define M_DWG_TRAP	0x04
#define M_DWG_BITBLT	0x08
#define M_DWG_ILOAD	0x09

#define M_DWG_LINEAR	0x0080
#define M_DWG_SOLID	0x0800
#define M_DWG_ARZERO	0x1000
#define M_DWG_SGNZERO	0x2000
#define M_DWG_SHIFTZERO	0x4000

#define M_DWG_REPLACE	0x000C0000
#define M_DWG_REPLACE2	(M_DWG_REPLACE | 0x40)
#define M_DWG_XOR	0x00060010

#define M_DWG_BFCOL	0x04000000
#define M_DWG_BMONOWF	0x08000000

#define M_DWG_TRANSC	0x40000000

#define M_FIFOSTATUS	0x1E10
#define M_STATUS	0x1E14

#define M_IEN		0x1E1C

#define M_VCOUNT	0x1E20

#define M_RESET		0x1E40
#define M_MEMRDBK	0x1E44

#define M_AGP2PLL	0x1E4C

#define M_OPMODE	0x1E54
#define     M_OPMODE_DMA_GEN_WRITE	0x00
#define     M_OPMODE_DMA_BLIT		0x04
#define     M_OPMODE_DMA_VECTOR_WRITE	0x08
#define     M_OPMODE_DMA_LE		0x0000		/* little endian - no transformation */
#define     M_OPMODE_DMA_BE_8BPP	0x0000
#define     M_OPMODE_DMA_BE_16BPP	0x0100
#define     M_OPMODE_DMA_BE_32BPP	0x0200
#define     M_OPMODE_DIR_LE		0x000000	/* little endian - no transformation */
#define     M_OPMODE_DIR_BE_8BPP	0x000000
#define     M_OPMODE_DIR_BE_16BPP	0x010000
#define     M_OPMODE_DIR_BE_32BPP	0x020000

#define M_ATTR_INDEX	0x1FC0
#define M_ATTR_DATA	0x1FC1

#define M_MISC_REG	0x1FC2
#define M_3C2_RD	0x1FC2

#define M_SEQ_INDEX	0x1FC4
#define M_SEQ_DATA	0x1FC5

#define M_MISC_REG_READ	0x1FCC

#define M_GRAPHICS_INDEX 0x1FCE
#define M_GRAPHICS_DATA	0x1FCF

#define M_CRTC_INDEX	0x1FD4

#define M_ATTR_RESET	0x1FDA
#define M_3DA_WR	0x1FDA
#define M_INSTS1	0x1FDA

#define M_EXTVGA_INDEX	0x1FDE
#define M_EXTVGA_DATA	0x1FDF

/* G200 only */
#define M_SRCORG	0x2CB4

#define M_RAMDAC_BASE	0x3C00

/* fortunately, same on TVP3026 and MGA1064 */
#define M_DAC_REG	(M_RAMDAC_BASE+0)
#define M_DAC_VAL	(M_RAMDAC_BASE+1)
#define M_PALETTE_MASK	(M_RAMDAC_BASE+2)

#define M_X_INDEX	0x00
#define M_X_DATAREG	0x0A

#define DAC_XGENIOCTRL		0x2A
#define DAC_XGENIODATA		0x2B

#ifdef __LITTLE_ENDIAN
#define MX_OPTION_BSWAP		0x00000000

#define M_OPMODE_4BPP	(M_OPMODE_DMA_LE | M_OPMODE_DIR_LE | M_OPMODE_DMA_BLIT)
#define M_OPMODE_8BPP	(M_OPMODE_DMA_LE | M_OPMODE_DIR_LE | M_OPMODE_DMA_BLIT)
#define M_OPMODE_16BPP	(M_OPMODE_DMA_LE | M_OPMODE_DIR_LE | M_OPMODE_DMA_BLIT)
#define M_OPMODE_24BPP	(M_OPMODE_DMA_LE | M_OPMODE_DIR_LE | M_OPMODE_DMA_BLIT)
#define M_OPMODE_32BPP	(M_OPMODE_DMA_LE | M_OPMODE_DIR_LE | M_OPMODE_DMA_BLIT)
#else
#ifdef __BIG_ENDIAN
#define MX_OPTION_BSWAP		0x80000000

#define M_OPMODE_4BPP	(M_OPMODE_DMA_LE | M_OPMODE_DIR_LE | M_OPMODE_DMA_BLIT)	/* TODO */
#define M_OPMODE_8BPP	(M_OPMODE_DMA_BE_8BPP  | M_OPMODE_DIR_BE_8BPP  | M_OPMODE_DMA_BLIT)
#define M_OPMODE_16BPP	(M_OPMODE_DMA_BE_16BPP | M_OPMODE_DIR_BE_16BPP | M_OPMODE_DMA_BLIT)
#define M_OPMODE_24BPP	(M_OPMODE_DMA_BE_8BPP | M_OPMODE_DIR_BE_8BPP | M_OPMODE_DMA_BLIT)	/* TODO, ?32 */
#define M_OPMODE_32BPP	(M_OPMODE_DMA_BE_32BPP | M_OPMODE_DIR_BE_32BPP | M_OPMODE_DMA_BLIT)
#else
#error "Byte ordering have to be defined. Cannot continue."
#endif
#endif

#define mga_inb(addr)	mga_readb(ACCESS_FBINFO(mmio.vbase), (addr))
#define mga_inl(addr)	mga_readl(ACCESS_FBINFO(mmio.vbase), (addr))
#define mga_outb(addr,val) mga_writeb(ACCESS_FBINFO(mmio.vbase), (addr), (val))
#define mga_outw(addr,val) mga_writew(ACCESS_FBINFO(mmio.vbase), (addr), (val))
#define mga_outl(addr,val) mga_writel(ACCESS_FBINFO(mmio.vbase), (addr), (val))
#define mga_readr(port,idx) (mga_outb((port),(idx)), mga_inb((port)+1))
#ifdef __LITTLE_ENDIAN
#define mga_setr(addr,port,val) mga_outw(addr, ((val)<<8) | (port))
#else
#define mga_setr(addr,port,val) do { mga_outb(addr, port); mga_outb((addr)+1, val); } while (0)
#endif

#ifdef __LITTLE_ENDIAN
#define mga_fifo(n)	do {} while (mga_inb(M_FIFOSTATUS) < (n))
#else
#define mga_fifo(n)	do {} while ((mga_inl(M_FIFOSTATUS) & 0xFF) < (n))
#endif

#define WaitTillIdle()	do {} while (mga_inl(M_STATUS) & 0x10000)

/* code speedup */
#ifdef CONFIG_FB_MATROX_MILLENIUM
#define isInterleave(x)	 (x->interleave)
#define isMillenium(x)	 (x->millenium)
#define isMilleniumII(x) (x->milleniumII)
#else
#define isInterleave(x)  (0)
#define isMillenium(x)	 (0)
#define isMilleniumII(x) (0)
#endif

#define matroxfb_DAC_lock()                   spin_lock(&ACCESS_FBINFO(lock.DAC))
#define matroxfb_DAC_unlock()                 spin_unlock(&ACCESS_FBINFO(lock.DAC))
#define matroxfb_DAC_lock_irqsave(flags)      spin_lock_irqsave(&ACCESS_FBINFO(lock.DAC),flags)
#define matroxfb_DAC_unlock_irqrestore(flags) spin_unlock_irqrestore(&ACCESS_FBINFO(lock.DAC),flags)
extern void matroxfb_DAC_out(CPMINFO int reg, int val);
extern int matroxfb_DAC_in(CPMINFO int reg);
extern struct list_head matroxfb_list;
extern void matroxfb_var2my(struct fb_var_screeninfo* fvsi, struct my_timming* mt);

#ifdef MATROXFB_USE_SPINLOCKS
#define CRITBEGIN  spin_lock_irqsave(&ACCESS_FBINFO(lock.accel), critflags);
#define CRITEND	   spin_unlock_irqrestore(&ACCESS_FBINFO(lock.accel), critflags);
#define CRITFLAGS  unsigned long critflags;
#else
#define CRITBEGIN
#define CRITEND
#define CRITFLAGS
#endif

#endif	/* __MATROXFB_H__ */
