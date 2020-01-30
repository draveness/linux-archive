#ifndef __ALPHA_CIA__H__
#define __ALPHA_CIA__H__

/* Define to experiment with fitting everything into one 512MB HAE window.  */
#define CIA_ONE_HAE_WINDOW 1

#include <linux/config.h>
#include <linux/types.h>
#include <asm/compiler.h>

/*
 * CIA is the internal name for the 21171 chipset which provides
 * memory controller and PCI access for the 21164 chip based systems.
 * Also supported here is the 21172 (CIA-2) and 21174 (PYXIS).
 *
 * The lineage is a bit confused, since the 21174 was reportedly started
 * from the 21171 Pass 1 mask, and so is missing bug fixes that appear
 * in 21171 Pass 2 and 21172, but it also contains additional features.
 *
 * This file is based on:
 *
 * DECchip 21171 Core Logic Chipset
 * Technical Reference Manual
 *
 * EC-QE18B-TE
 *
 * david.rusling@reo.mts.dec.com Initial Version.
 *
 */

/*
 * CIA ADDRESS BIT DEFINITIONS
 *
 *  3333 3333 3322 2222 2222 1111 1111 11
 *  9876 5432 1098 7654 3210 9876 5432 1098 7654 3210
 *  ---- ---- ---- ---- ---- ---- ---- ---- ---- ----
 *  1                                             000
 *  ---- ---- ---- ---- ---- ---- ---- ---- ---- ----
 *  |                                             |\|
 *  |                               Byte Enable --+ |
 *  |                             Transfer Length --+
 *  +-- IO space, not cached
 *
 *   Byte      Transfer
 *   Enable    Length    Transfer  Byte    Address
 *   adr<6:5>  adr<4:3>  Length    Enable  Adder
 *   ---------------------------------------------
 *      00        00      Byte      1110   0x000
 *      01        00      Byte      1101   0x020
 *      10        00      Byte      1011   0x040
 *      11        00      Byte      0111   0x060
 *
 *      00        01      Word      1100   0x008
 *      01        01      Word      1001   0x028 <= Not supported in this code.
 *      10        01      Word      0011   0x048
 *
 *      00        10      Tribyte   1000   0x010
 *      01        10      Tribyte   0001   0x030
 *
 *      10        11      Longword  0000   0x058
 *
 *      Note that byte enables are asserted low.
 *
 */

#define CIA_MEM_R1_MASK 0x1fffffff  /* SPARSE Mem region 1 mask is 29 bits */
#define CIA_MEM_R2_MASK 0x07ffffff  /* SPARSE Mem region 2 mask is 27 bits */
#define CIA_MEM_R3_MASK 0x03ffffff  /* SPARSE Mem region 3 mask is 26 bits */

/*
 * 21171-CA Control and Status Registers
 */
#define CIA_IOC_CIA_REV			(IDENT_ADDR + 0x8740000080UL)
#  define CIA_REV_MASK			0xff
#define CIA_IOC_PCI_LAT			(IDENT_ADDR + 0x87400000C0UL)
#define CIA_IOC_CIA_CTRL		(IDENT_ADDR + 0x8740000100UL)
#  define CIA_CTRL_PCI_EN		(1 << 0)
#  define CIA_CTRL_PCI_LOCK_EN		(1 << 1)
#  define CIA_CTRL_PCI_LOOP_EN		(1 << 2)
#  define CIA_CTRL_FST_BB_EN		(1 << 3)
#  define CIA_CTRL_PCI_MST_EN		(1 << 4)
#  define CIA_CTRL_PCI_MEM_EN		(1 << 5)
#  define CIA_CTRL_PCI_REQ64_EN		(1 << 6)
#  define CIA_CTRL_PCI_ACK64_EN		(1 << 7)
#  define CIA_CTRL_ADDR_PE_EN		(1 << 8)
#  define CIA_CTRL_PERR_EN		(1 << 9)
#  define CIA_CTRL_FILL_ERR_EN		(1 << 10)
#  define CIA_CTRL_MCHK_ERR_EN		(1 << 11)
#  define CIA_CTRL_ECC_CHK_EN		(1 << 12)
#  define CIA_CTRL_ASSERT_IDLE_BC	(1 << 13)
#  define CIA_CTRL_COM_IDLE_BC		(1 << 14)
#  define CIA_CTRL_CSR_IOA_BYPASS	(1 << 15)
#  define CIA_CTRL_IO_FLUSHREQ_EN	(1 << 16)
#  define CIA_CTRL_CPU_FLUSHREQ_EN	(1 << 17)
#  define CIA_CTRL_ARB_CPU_EN		(1 << 18)
#  define CIA_CTRL_EN_ARB_LINK		(1 << 19)
#  define CIA_CTRL_RD_TYPE_SHIFT	20
#  define CIA_CTRL_RL_TYPE_SHIFT	24
#  define CIA_CTRL_RM_TYPE_SHIFT	28
#  define CIA_CTRL_EN_DMA_RD_PERF	(1 << 31)
#define CIA_IOC_CIA_CNFG		(IDENT_ADDR + 0x8740000140UL)
#  define CIA_CNFG_IOA_BWEN		(1 << 0)
#  define CIA_CNFG_PCI_MWEN		(1 << 4)
#  define CIA_CNFG_PCI_DWEN		(1 << 5)
#  define CIA_CNFG_PCI_WLEN		(1 << 8)
#define CIA_IOC_FLASH_CTRL		(IDENT_ADDR + 0x8740000200UL)
#define CIA_IOC_HAE_MEM			(IDENT_ADDR + 0x8740000400UL)
#define CIA_IOC_HAE_IO			(IDENT_ADDR + 0x8740000440UL)
#define CIA_IOC_CFG			(IDENT_ADDR + 0x8740000480UL)
#define CIA_IOC_CACK_EN			(IDENT_ADDR + 0x8740000600UL)
#  define CIA_CACK_EN_LOCK_EN		(1 << 0)
#  define CIA_CACK_EN_MB_EN		(1 << 1)
#  define CIA_CACK_EN_SET_DIRTY_EN	(1 << 2)
#  define CIA_CACK_EN_BC_VICTIM_EN	(1 << 3)


/*
 * 21171-CA Diagnostic Registers
 */
#define CIA_IOC_CIA_DIAG		(IDENT_ADDR + 0x8740002000UL)
#define CIA_IOC_DIAG_CHECK		(IDENT_ADDR + 0x8740003000UL)

/*
 * 21171-CA Performance Monitor registers
 */
#define CIA_IOC_PERF_MONITOR		(IDENT_ADDR + 0x8740004000UL)
#define CIA_IOC_PERF_CONTROL		(IDENT_ADDR + 0x8740004040UL)

/*
 * 21171-CA Error registers
 */
#define CIA_IOC_CPU_ERR0		(IDENT_ADDR + 0x8740008000UL)
#define CIA_IOC_CPU_ERR1		(IDENT_ADDR + 0x8740008040UL)
#define CIA_IOC_CIA_ERR			(IDENT_ADDR + 0x8740008200UL)
#  define CIA_ERR_COR_ERR		(1 << 0)
#  define CIA_ERR_UN_COR_ERR		(1 << 1)
#  define CIA_ERR_CPU_PE		(1 << 2)
#  define CIA_ERR_MEM_NEM		(1 << 3)
#  define CIA_ERR_PCI_SERR		(1 << 4)
#  define CIA_ERR_PERR			(1 << 5)
#  define CIA_ERR_PCI_ADDR_PE		(1 << 6)
#  define CIA_ERR_RCVD_MAS_ABT		(1 << 7)
#  define CIA_ERR_RCVD_TAR_ABT		(1 << 8)
#  define CIA_ERR_PA_PTE_INV		(1 << 9)
#  define CIA_ERR_FROM_WRT_ERR		(1 << 10)
#  define CIA_ERR_IOA_TIMEOUT		(1 << 11)
#  define CIA_ERR_LOST_CORR_ERR		(1 << 16)
#  define CIA_ERR_LOST_UN_CORR_ERR	(1 << 17)
#  define CIA_ERR_LOST_CPU_PE		(1 << 18)
#  define CIA_ERR_LOST_MEM_NEM		(1 << 19)
#  define CIA_ERR_LOST_PERR		(1 << 21)
#  define CIA_ERR_LOST_PCI_ADDR_PE	(1 << 22)
#  define CIA_ERR_LOST_RCVD_MAS_ABT	(1 << 23)
#  define CIA_ERR_LOST_RCVD_TAR_ABT	(1 << 24)
#  define CIA_ERR_LOST_PA_PTE_INV	(1 << 25)
#  define CIA_ERR_LOST_FROM_WRT_ERR	(1 << 26)
#  define CIA_ERR_LOST_IOA_TIMEOUT	(1 << 27)
#  define CIA_ERR_VALID			(1 << 31)
#define CIA_IOC_CIA_STAT		(IDENT_ADDR + 0x8740008240UL)
#define CIA_IOC_ERR_MASK		(IDENT_ADDR + 0x8740008280UL)
#define CIA_IOC_CIA_SYN			(IDENT_ADDR + 0x8740008300UL)
#define CIA_IOC_MEM_ERR0		(IDENT_ADDR + 0x8740008400UL)
#define CIA_IOC_MEM_ERR1		(IDENT_ADDR + 0x8740008440UL)
#define CIA_IOC_PCI_ERR0		(IDENT_ADDR + 0x8740008800UL)
#define CIA_IOC_PCI_ERR1		(IDENT_ADDR + 0x8740008840UL)
#define CIA_IOC_PCI_ERR3		(IDENT_ADDR + 0x8740008880UL)

/*
 * 21171-CA System configuration registers
 */
#define CIA_IOC_MCR			(IDENT_ADDR + 0x8750000000UL)
#define CIA_IOC_MBA0			(IDENT_ADDR + 0x8750000600UL)
#define CIA_IOC_MBA2			(IDENT_ADDR + 0x8750000680UL)
#define CIA_IOC_MBA4			(IDENT_ADDR + 0x8750000700UL)
#define CIA_IOC_MBA6			(IDENT_ADDR + 0x8750000780UL)
#define CIA_IOC_MBA8			(IDENT_ADDR + 0x8750000800UL)
#define CIA_IOC_MBAA			(IDENT_ADDR + 0x8750000880UL)
#define CIA_IOC_MBAC			(IDENT_ADDR + 0x8750000900UL)
#define CIA_IOC_MBAE			(IDENT_ADDR + 0x8750000980UL)
#define CIA_IOC_TMG0			(IDENT_ADDR + 0x8750000B00UL)
#define CIA_IOC_TMG1			(IDENT_ADDR + 0x8750000B40UL)
#define CIA_IOC_TMG2			(IDENT_ADDR + 0x8750000B80UL)

/*
 * 2117A-CA PCI Address and Scatter-Gather Registers.
 */
#define CIA_IOC_PCI_TBIA		(IDENT_ADDR + 0x8760000100UL)

#define CIA_IOC_PCI_W0_BASE		(IDENT_ADDR + 0x8760000400UL)
#define CIA_IOC_PCI_W0_MASK		(IDENT_ADDR + 0x8760000440UL)
#define CIA_IOC_PCI_T0_BASE		(IDENT_ADDR + 0x8760000480UL)

#define CIA_IOC_PCI_W1_BASE		(IDENT_ADDR + 0x8760000500UL)
#define CIA_IOC_PCI_W1_MASK		(IDENT_ADDR + 0x8760000540UL)
#define CIA_IOC_PCI_T1_BASE		(IDENT_ADDR + 0x8760000580UL)

#define CIA_IOC_PCI_W2_BASE		(IDENT_ADDR + 0x8760000600UL)
#define CIA_IOC_PCI_W2_MASK		(IDENT_ADDR + 0x8760000640UL)
#define CIA_IOC_PCI_T2_BASE		(IDENT_ADDR + 0x8760000680UL)

#define CIA_IOC_PCI_W3_BASE		(IDENT_ADDR + 0x8760000700UL)
#define CIA_IOC_PCI_W3_MASK		(IDENT_ADDR + 0x8760000740UL)
#define CIA_IOC_PCI_T3_BASE		(IDENT_ADDR + 0x8760000780UL)

#define CIA_IOC_PCI_W_DAC		(IDENT_ADDR + 0x87600007C0UL)

/*
 * 2117A-CA Address Translation Registers.
 */

/* 8 tag registers, the first 4 of which are lockable.  */
#define CIA_IOC_TB_TAGn(n) \
	(IDENT_ADDR + 0x8760000800UL + (n)*0x40)

/* 4 page registers per tag register.  */
#define CIA_IOC_TBn_PAGEm(n,m) \
	(IDENT_ADDR + 0x8760001000UL + (n)*0x100 + (m)*0x40)

/*
 * Memory spaces:
 */
#define CIA_IACK_SC			(IDENT_ADDR + 0x8720000000UL)
#define CIA_CONF			(IDENT_ADDR + 0x8700000000UL)
#define CIA_IO				(IDENT_ADDR + 0x8580000000UL)
#define CIA_SPARSE_MEM			(IDENT_ADDR + 0x8000000000UL)
#define CIA_SPARSE_MEM_R2		(IDENT_ADDR + 0x8400000000UL)
#define CIA_SPARSE_MEM_R3		(IDENT_ADDR + 0x8500000000UL)
#define CIA_DENSE_MEM		        (IDENT_ADDR + 0x8600000000UL)
#define CIA_BW_MEM			(IDENT_ADDR + 0x8800000000UL)
#define CIA_BW_IO			(IDENT_ADDR + 0x8900000000UL)
#define CIA_BW_CFG_0			(IDENT_ADDR + 0x8a00000000UL)
#define CIA_BW_CFG_1			(IDENT_ADDR + 0x8b00000000UL)

/*
 * ALCOR's GRU ASIC registers
 */
#define GRU_INT_REQ			(IDENT_ADDR + 0x8780000000UL)
#define GRU_INT_MASK			(IDENT_ADDR + 0x8780000040UL)
#define GRU_INT_EDGE			(IDENT_ADDR + 0x8780000080UL)
#define GRU_INT_HILO			(IDENT_ADDR + 0x87800000C0UL)
#define GRU_INT_CLEAR			(IDENT_ADDR + 0x8780000100UL)

#define GRU_CACHE_CNFG			(IDENT_ADDR + 0x8780000200UL)
#define GRU_SCR				(IDENT_ADDR + 0x8780000300UL)
#define GRU_LED				(IDENT_ADDR + 0x8780000800UL)
#define GRU_RESET			(IDENT_ADDR + 0x8780000900UL)

#define ALCOR_GRU_INT_REQ_BITS		0x800fffffUL
#define XLT_GRU_INT_REQ_BITS		0x80003fffUL
#define GRU_INT_REQ_BITS		(alpha_mv.sys.cia.gru_int_req_bits+0)

/*
 * PYXIS interrupt control registers
 */
#define PYXIS_INT_REQ			(IDENT_ADDR + 0x87A0000000UL)
#define PYXIS_INT_MASK			(IDENT_ADDR + 0x87A0000040UL)
#define PYXIS_INT_HILO			(IDENT_ADDR + 0x87A00000C0UL)
#define PYXIS_INT_ROUTE			(IDENT_ADDR + 0x87A0000140UL)
#define PYXIS_GPO			(IDENT_ADDR + 0x87A0000180UL)
#define PYXIS_INT_CNFG			(IDENT_ADDR + 0x87A00001C0UL)
#define PYXIS_RT_COUNT			(IDENT_ADDR + 0x87A0000200UL)
#define PYXIS_INT_TIME			(IDENT_ADDR + 0x87A0000240UL)
#define PYXIS_IIC_CTRL			(IDENT_ADDR + 0x87A00002C0UL)
#define PYXIS_RESET			(IDENT_ADDR + 0x8780000900UL)

/*
 * Data structure for handling CIA machine checks.
 */

/* System-specific info.  */
struct el_CIA_sysdata_mcheck {
	unsigned long	cpu_err0;
	unsigned long	cpu_err1;
	unsigned long	cia_err;
	unsigned long	cia_stat;
	unsigned long	err_mask;
	unsigned long	cia_syn;
	unsigned long	mem_err0;
	unsigned long	mem_err1;
	unsigned long	pci_err0;
	unsigned long	pci_err1;
	unsigned long	pci_err2;
};


#ifdef __KERNEL__

#ifndef __EXTERN_INLINE
#define __EXTERN_INLINE extern inline
#define __IO_EXTERN_INLINE
#endif

/*
 * I/O functions:
 *
 * CIA (the 2117x PCI/memory support chipset for the EV5 (21164)
 * series of processors uses a sparse address mapping scheme to
 * get at PCI memory and I/O.
 */

#define vucp	volatile unsigned char *
#define vusp	volatile unsigned short *
#define vip	volatile int *
#define vuip	volatile unsigned int *
#define vulp	volatile unsigned long *

__EXTERN_INLINE unsigned int cia_inb(unsigned long addr)
{
	long result;
	result = *(vip) ((addr << 5) + CIA_IO + 0x00);
	return __kernel_extbl(result, addr & 3);
}

__EXTERN_INLINE void cia_outb(unsigned char b, unsigned long addr)
{
	unsigned long w = __kernel_insbl(b, addr & 3);
	*(vuip) ((addr << 5) + CIA_IO + 0x00) = w;
	mb();
}

__EXTERN_INLINE unsigned int cia_inw(unsigned long addr)
{
	long result;
	result = *(vip) ((addr << 5) + CIA_IO + 0x08);
	return __kernel_extwl(result, addr & 3);
}

__EXTERN_INLINE void cia_outw(unsigned short b, unsigned long addr)
{
	unsigned long w = __kernel_inswl(b, addr & 3);
	*(vuip) ((addr << 5) + CIA_IO + 0x08) = w;
	mb();
}

__EXTERN_INLINE unsigned int cia_inl(unsigned long addr)
{
	return *(vuip) ((addr << 5) + CIA_IO + 0x18);
}

__EXTERN_INLINE void cia_outl(unsigned int b, unsigned long addr)
{
	*(vuip) ((addr << 5) + CIA_IO + 0x18) = b;
	mb();
}

__EXTERN_INLINE unsigned int cia_bwx_inb(unsigned long addr)
{
	/* ??? I wish I could get rid of this.  But there's no ioremap
	   equivalent for I/O space.  PCI I/O can be forced into the
	   CIA BWX I/O region, but that doesn't take care of legacy
	   ISA crap.  */

	return __kernel_ldbu(*(vucp)(addr+CIA_BW_IO));
}

__EXTERN_INLINE void cia_bwx_outb(unsigned char b, unsigned long addr)
{
	__kernel_stb(b, *(vucp)(addr+CIA_BW_IO));
	mb();
}

__EXTERN_INLINE unsigned int cia_bwx_inw(unsigned long addr)
{
	return __kernel_ldwu(*(vusp)(addr+CIA_BW_IO));
}

__EXTERN_INLINE void cia_bwx_outw(unsigned short b, unsigned long addr)
{
	__kernel_stw(b, *(vusp)(addr+CIA_BW_IO));
	mb();
}

__EXTERN_INLINE unsigned int cia_bwx_inl(unsigned long addr)
{
	return *(vuip)(addr+CIA_BW_IO);
}

__EXTERN_INLINE void cia_bwx_outl(unsigned int b, unsigned long addr)
{
	*(vuip)(addr+CIA_BW_IO) = b;
	mb();
}


/*
 * Memory functions.  64-bit and 32-bit accesses are done through
 * dense memory space, everything else through sparse space.
 *
 * For reading and writing 8 and 16 bit quantities we need to
 * go through one of the three sparse address mapping regions
 * and use the HAE_MEM CSR to provide some bits of the address.
 * The following few routines use only sparse address region 1
 * which gives 1Gbyte of accessible space which relates exactly
 * to the amount of PCI memory mapping *into* system address space.
 * See p 6-17 of the specification but it looks something like this:
 *
 * 21164 Address:
 *
 *          3         2         1
 * 9876543210987654321098765432109876543210
 * 1ZZZZ0.PCI.QW.Address............BBLL
 *
 * ZZ = SBZ
 * BB = Byte offset
 * LL = Transfer length
 *
 * PCI Address:
 *
 * 3         2         1
 * 10987654321098765432109876543210
 * HHH....PCI.QW.Address........ 00
 *
 * HHH = 31:29 HAE_MEM CSR
 *
 */

__EXTERN_INLINE unsigned long cia_readb(unsigned long addr)
{
	unsigned long result;

	addr &= CIA_MEM_R1_MASK;
	result = *(vip) ((addr << 5) + CIA_SPARSE_MEM + 0x00);
	return __kernel_extbl(result, addr & 3);
}

__EXTERN_INLINE unsigned long cia_readw(unsigned long addr)
{
	unsigned long result;

	addr &= CIA_MEM_R1_MASK;
	result = *(vip) ((addr << 5) + CIA_SPARSE_MEM + 0x08);
	return __kernel_extwl(result, addr & 3);
}

__EXTERN_INLINE void cia_writeb(unsigned char b, unsigned long addr)
{
	unsigned long w;

	addr &= CIA_MEM_R1_MASK;
	w = __kernel_insbl(b, addr & 3);
	*(vuip) ((addr << 5) + CIA_SPARSE_MEM + 0x00) = w;
}

__EXTERN_INLINE void cia_writew(unsigned short b, unsigned long addr)
{
	unsigned long w;

	addr &= CIA_MEM_R1_MASK;
	w = __kernel_inswl(b, addr & 3);
	*(vuip) ((addr << 5) + CIA_SPARSE_MEM + 0x08) = w;
}

__EXTERN_INLINE unsigned long cia_readl(unsigned long addr)
{
	return *(vuip)addr;
}

__EXTERN_INLINE unsigned long cia_readq(unsigned long addr)
{
	return *(vulp)addr;
}

__EXTERN_INLINE void cia_writel(unsigned int b, unsigned long addr)
{
	*(vuip)addr = b;
}

__EXTERN_INLINE void cia_writeq(unsigned long b, unsigned long addr)
{
	*(vulp)addr = b;
}

__EXTERN_INLINE unsigned long cia_ioremap(unsigned long addr)
{
	return addr + CIA_DENSE_MEM;
}

__EXTERN_INLINE unsigned long cia_bwx_readb(unsigned long addr)
{
	return __kernel_ldbu(*(vucp)addr);
}

__EXTERN_INLINE unsigned long cia_bwx_readw(unsigned long addr)
{
	return __kernel_ldwu(*(vusp)addr);
}

__EXTERN_INLINE unsigned long cia_bwx_readl(unsigned long addr)
{
	return *(vuip)addr;
}

__EXTERN_INLINE unsigned long cia_bwx_readq(unsigned long addr)
{
	return *(vulp)addr;
}

__EXTERN_INLINE void cia_bwx_writeb(unsigned char b, unsigned long addr)
{
	__kernel_stb(b, *(vucp)addr);
}

__EXTERN_INLINE void cia_bwx_writew(unsigned short b, unsigned long addr)
{
	__kernel_stw(b, *(vusp)addr);
}

__EXTERN_INLINE void cia_bwx_writel(unsigned int b, unsigned long addr)
{
	*(vuip)addr = b;
}

__EXTERN_INLINE void cia_bwx_writeq(unsigned long b, unsigned long addr)
{
	*(vulp)addr = b;
}

__EXTERN_INLINE unsigned long cia_bwx_ioremap(unsigned long addr)
{
	return addr + CIA_BW_MEM;
}

__EXTERN_INLINE int cia_is_ioaddr(unsigned long addr)
{
	return addr >= IDENT_ADDR + 0x8000000000UL;
}

#undef vucp
#undef vusp
#undef vip
#undef vuip
#undef vulp

#ifdef __WANT_IO_DEF

#ifdef CONFIG_ALPHA_PYXIS
# define __inb(p)		cia_bwx_inb((unsigned long)(p))
# define __inw(p)		cia_bwx_inw((unsigned long)(p))
# define __inl(p)		cia_bwx_inl((unsigned long)(p))
# define __outb(x,p)		cia_bwx_outb((x),(unsigned long)(p))
# define __outw(x,p)		cia_bwx_outw((x),(unsigned long)(p))
# define __outl(x,p)		cia_bwx_outl((x),(unsigned long)(p))
# define __readb(a)		cia_bwx_readb((unsigned long)(a))
# define __readw(a)		cia_bwx_readw((unsigned long)(a))
# define __readl(a)		cia_bwx_readl((unsigned long)(a))
# define __readq(a)		cia_bwx_readq((unsigned long)(a))
# define __writeb(x,a)		cia_bwx_writeb((x),(unsigned long)(a))
# define __writew(x,a)		cia_bwx_writew((x),(unsigned long)(a))
# define __writel(x,a)		cia_bwx_writel((x),(unsigned long)(a))
# define __writeq(x,a)		cia_bwx_writeq((x),(unsigned long)(a))
# define __ioremap(a)		cia_bwx_ioremap((unsigned long)(a))
# define inb(p)			__inb(p)
# define inw(p)			__inw(p)
# define inl(p)			__inl(p)
# define outb(x,p)		__outb((x),(p))
# define outw(x,p)		__outw((x),(p))
# define outl(x,p)		__outl((x),(p))
# define __raw_readb(a)		__readb(a)
# define __raw_readw(a)		__readw(a)
# define __raw_readl(a)		__readl(a)
# define __raw_readq(a)		__readq(a)
# define __raw_writeb(x,a)	__writeb((x),(a))
# define __raw_writew(x,a)	__writew((x),(a))
# define __raw_writel(x,a)	__writel((x),(a))
# define __raw_writeq(x,a)	__writeq((x),(a))
#else
# define __inb(p)		cia_inb((unsigned long)(p))
# define __inw(p)		cia_inw((unsigned long)(p))
# define __inl(p)		cia_inl((unsigned long)(p))
# define __outb(x,p)		cia_outb((x),(unsigned long)(p))
# define __outw(x,p)		cia_outw((x),(unsigned long)(p))
# define __outl(x,p)		cia_outl((x),(unsigned long)(p))
# define __readb(a)		cia_readb((unsigned long)(a))
# define __readw(a)		cia_readw((unsigned long)(a))
# define __readl(a)		cia_readl((unsigned long)(a))
# define __readq(a)		cia_readq((unsigned long)(a))
# define __writeb(x,a)		cia_writeb((x),(unsigned long)(a))
# define __writew(x,a)		cia_writew((x),(unsigned long)(a))
# define __writel(x,a)		cia_writel((x),(unsigned long)(a))
# define __writeq(x,a)		cia_writeq((x),(unsigned long)(a))
# define __ioremap(a)		cia_ioremap((unsigned long)(a))
# define __raw_readl(a)		__readl(a)
# define __raw_readq(a)		__readq(a)
# define __raw_writel(v,a)	__writel((v),(a))
# define __raw_writeq(v,a)	__writeq((v),(a))
#endif /* PYXIS */

#define __is_ioaddr(a)		cia_is_ioaddr((unsigned long)(a))

#endif /* __WANT_IO_DEF */

#ifdef __IO_EXTERN_INLINE
#undef __EXTERN_INLINE
#undef __IO_EXTERN_INLINE
#endif

#endif /* __KERNEL__ */

#endif /* __ALPHA_CIA__H__ */
