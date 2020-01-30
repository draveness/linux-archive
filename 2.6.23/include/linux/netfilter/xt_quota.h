#ifndef _XT_QUOTA_H
#define _XT_QUOTA_H

enum xt_quota_flags {
	XT_QUOTA_INVERT		= 0x1,
};
#define XT_QUOTA_MASK		0x1

struct xt_quota_info {
	u_int32_t		flags;
	u_int32_t		pad;
	aligned_u64		quota;
	struct xt_quota_info	*master;
};

#endif /* _XT_QUOTA_H */
