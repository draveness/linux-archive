#ifndef _ASM_REQUIRED_FEATURES_H
#define _ASM_REQUIRED_FEATURES_H 1

/* Define minimum CPUID feature set for kernel These bits are checked
   really early to actually display a visible error message before the
   kernel dies.  Make sure to assign features to the proper mask!

   The real information is in arch/x86_64/Kconfig.cpu, this just converts
   the CONFIGs into a bitmask */

/* x86-64 baseline features */
#define NEED_FPU	(1<<(X86_FEATURE_FPU & 31))
#define NEED_PSE	(1<<(X86_FEATURE_PSE & 31))
#define NEED_MSR	(1<<(X86_FEATURE_MSR & 31))
#define NEED_PAE	(1<<(X86_FEATURE_PAE & 31))
#define NEED_CX8	(1<<(X86_FEATURE_CX8 & 31))
#define NEED_PGE	(1<<(X86_FEATURE_PGE & 31))
#define NEED_FXSR	(1<<(X86_FEATURE_FXSR & 31))
#define NEED_CMOV	(1<<(X86_FEATURE_CMOV & 31))
#define NEED_XMM	(1<<(X86_FEATURE_XMM & 31))
#define NEED_XMM2	(1<<(X86_FEATURE_XMM2 & 31))

#define REQUIRED_MASK0	(NEED_FPU|NEED_PSE|NEED_MSR|NEED_PAE|\
			 NEED_CX8|NEED_PGE|NEED_FXSR|NEED_CMOV|\
			 NEED_XMM|NEED_XMM2)
#define SSE_MASK	(NEED_XMM|NEED_XMM2)

/* x86-64 baseline features */
#define NEED_LM		(1<<(X86_FEATURE_LM & 31))

#ifdef CONFIG_X86_USE_3DNOW
# define NEED_3DNOW	(1<<(X86_FEATURE_3DNOW & 31))
#else
# define NEED_3DNOW	0
#endif

#define REQUIRED_MASK1	(NEED_LM|NEED_3DNOW)

#define REQUIRED_MASK2	0
#define REQUIRED_MASK3	0
#define REQUIRED_MASK4	0
#define REQUIRED_MASK5	0
#define REQUIRED_MASK6	0
#define REQUIRED_MASK7	0

#endif
