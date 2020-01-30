#ifndef _PSERIES_PLPAR_WRAPPERS_H
#define _PSERIES_PLPAR_WRAPPERS_H

#include <asm/hvcall.h>

static inline long poll_pending(void)
{
	return plpar_hcall_norets(H_POLL_PENDING);
}

static inline long prod_processor(void)
{
	return plpar_hcall_norets(H_PROD);
}

static inline long cede_processor(void)
{
	return plpar_hcall_norets(H_CEDE);
}

static inline long vpa_call(unsigned long flags, unsigned long cpu,
		unsigned long vpa)
{
	/* flags are in bits 16-18 (counting from most significant bit) */
	flags = flags << (63 - 18);

	return plpar_hcall_norets(H_REGISTER_VPA, flags, cpu, vpa);
}

static inline long unregister_vpa(unsigned long cpu, unsigned long vpa)
{
	return vpa_call(0x5, cpu, vpa);
}

static inline long register_vpa(unsigned long cpu, unsigned long vpa)
{
	return vpa_call(0x1, cpu, vpa);
}

static inline long unregister_slb_shadow(unsigned long cpu, unsigned long vpa)
{
	return vpa_call(0x7, cpu, vpa);
}

static inline long register_slb_shadow(unsigned long cpu, unsigned long vpa)
{
	return vpa_call(0x3, cpu, vpa);
}

extern void vpa_init(int cpu);

static inline long plpar_pte_enter(unsigned long flags,
		unsigned long hpte_group, unsigned long hpte_v,
		unsigned long hpte_r, unsigned long *slot)
{
	long rc;
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE];

	rc = plpar_hcall(H_ENTER, retbuf, flags, hpte_group, hpte_v, hpte_r);

	*slot = retbuf[0];

	return rc;
}

static inline long plpar_pte_remove(unsigned long flags, unsigned long ptex,
		unsigned long avpn, unsigned long *old_pteh_ret,
		unsigned long *old_ptel_ret)
{
	long rc;
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE];

	rc = plpar_hcall(H_REMOVE, retbuf, flags, ptex, avpn);

	*old_pteh_ret = retbuf[0];
	*old_ptel_ret = retbuf[1];

	return rc;
}

/* plpar_pte_remove_raw can be called in real mode. It calls plpar_hcall_raw */
static inline long plpar_pte_remove_raw(unsigned long flags, unsigned long ptex,
		unsigned long avpn, unsigned long *old_pteh_ret,
		unsigned long *old_ptel_ret)
{
	long rc;
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE];

	rc = plpar_hcall_raw(H_REMOVE, retbuf, flags, ptex, avpn);

	*old_pteh_ret = retbuf[0];
	*old_ptel_ret = retbuf[1];

	return rc;
}

static inline long plpar_pte_read(unsigned long flags, unsigned long ptex,
		unsigned long *old_pteh_ret, unsigned long *old_ptel_ret)
{
	long rc;
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE];

	rc = plpar_hcall(H_READ, retbuf, flags, ptex);

	*old_pteh_ret = retbuf[0];
	*old_ptel_ret = retbuf[1];

	return rc;
}

/* plpar_pte_read_raw can be called in real mode. It calls plpar_hcall_raw */
static inline long plpar_pte_read_raw(unsigned long flags, unsigned long ptex,
		unsigned long *old_pteh_ret, unsigned long *old_ptel_ret)
{
	long rc;
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE];

	rc = plpar_hcall_raw(H_READ, retbuf, flags, ptex);

	*old_pteh_ret = retbuf[0];
	*old_ptel_ret = retbuf[1];

	return rc;
}

static inline long plpar_pte_protect(unsigned long flags, unsigned long ptex,
		unsigned long avpn)
{
	return plpar_hcall_norets(H_PROTECT, flags, ptex, avpn);
}

static inline long plpar_tce_get(unsigned long liobn, unsigned long ioba,
		unsigned long *tce_ret)
{
	long rc;
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE];

	rc = plpar_hcall(H_GET_TCE, retbuf, liobn, ioba);

	*tce_ret = retbuf[0];

	return rc;
}

static inline long plpar_tce_put(unsigned long liobn, unsigned long ioba,
		unsigned long tceval)
{
	return plpar_hcall_norets(H_PUT_TCE, liobn, ioba, tceval);
}

static inline long plpar_tce_put_indirect(unsigned long liobn,
		unsigned long ioba, unsigned long page, unsigned long count)
{
	return plpar_hcall_norets(H_PUT_TCE_INDIRECT, liobn, ioba, page, count);
}

static inline long plpar_tce_stuff(unsigned long liobn, unsigned long ioba,
		unsigned long tceval, unsigned long count)
{
	return plpar_hcall_norets(H_STUFF_TCE, liobn, ioba, tceval, count);
}

static inline long plpar_get_term_char(unsigned long termno,
		unsigned long *len_ret, char *buf_ret)
{
	long rc;
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE];
	unsigned long *lbuf = (unsigned long *)buf_ret;	/* TODO: alignment? */

	rc = plpar_hcall(H_GET_TERM_CHAR, retbuf, termno);

	*len_ret = retbuf[0];
	lbuf[0] = retbuf[1];
	lbuf[1] = retbuf[2];

	return rc;
}

static inline long plpar_put_term_char(unsigned long termno, unsigned long len,
		const char *buffer)
{
	unsigned long *lbuf = (unsigned long *)buffer;	/* TODO: alignment? */
	return plpar_hcall_norets(H_PUT_TERM_CHAR, termno, len, lbuf[0],
			lbuf[1]);
}

static inline long plpar_eoi(unsigned long xirr)
{
	return plpar_hcall_norets(H_EOI, xirr);
}

static inline long plpar_cppr(unsigned long cppr)
{
	return plpar_hcall_norets(H_CPPR, cppr);
}

static inline long plpar_ipi(unsigned long servernum, unsigned long mfrr)
{
	return plpar_hcall_norets(H_IPI, servernum, mfrr);
}

static inline long plpar_xirr(unsigned long *xirr_ret)
{
	long rc;
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE];

	rc = plpar_hcall(H_XIRR, retbuf);

	*xirr_ret = retbuf[0];

	return rc;
}

#endif /* _PSERIES_PLPAR_WRAPPERS_H */
