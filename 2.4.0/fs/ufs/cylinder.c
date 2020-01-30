/*
 *  linux/fs/ufs/cylinder.c
 *
 * Copyright (C) 1998
 * Daniel Pirkl <daniel.pirkl@email.cz>
 * Charles University, Faculty of Mathematics and Physics
 *
 *  ext2 - inode (block) bitmap caching inspired
 */

#include <linux/fs.h>
#include <linux/ufs_fs.h>
#include <linux/sched.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/locks.h>

#include <asm/bitops.h>
#include <asm/byteorder.h>

#include "swab.h"
#include "util.h"

#undef UFS_CYLINDER_DEBUG

#ifdef UFS_CYLINDER_DEBUG
#define UFSD(x) printk("(%s, %d), %s:", __FILE__, __LINE__, __FUNCTION__); printk x;
#else
#define UFSD(x)
#endif


/*
 * Read cylinder group into cache. The memory space for ufs_cg_private_info
 * structure is already allocated during ufs_read_super.
 */
static void ufs_read_cylinder (struct super_block * sb,
	unsigned cgno, unsigned bitmap_nr)
{
	struct ufs_sb_private_info * uspi;
	struct ufs_cg_private_info * ucpi;
	struct ufs_cylinder_group * ucg;
	unsigned i, j;
	unsigned swab;

	UFSD(("ENTER, cgno %u, bitmap_nr %u\n", cgno, bitmap_nr))
	swab = sb->u.ufs_sb.s_swab;	
	uspi = sb->u.ufs_sb.s_uspi;
	ucpi = sb->u.ufs_sb.s_ucpi[bitmap_nr];
	ucg = (struct ufs_cylinder_group *)sb->u.ufs_sb.s_ucg[cgno]->b_data;

	UCPI_UBH->fragment = ufs_cgcmin(cgno);
	UCPI_UBH->count = uspi->s_cgsize >> sb->s_blocksize_bits;
	/*
	 * We have already the first fragment of cylinder group block in buffer
	 */
	UCPI_UBH->bh[0] = sb->u.ufs_sb.s_ucg[cgno];
	for (i = 1; i < UCPI_UBH->count; i++)
		if (!(UCPI_UBH->bh[i] = bread (sb->s_dev, UCPI_UBH->fragment + i, sb->s_blocksize)))
			goto failed;
	sb->u.ufs_sb.s_cgno[bitmap_nr] = cgno;
			
	ucpi->c_cgx = SWAB32(ucg->cg_cgx);
	ucpi->c_ncyl = SWAB16(ucg->cg_ncyl);
	ucpi->c_niblk = SWAB16(ucg->cg_niblk);
	ucpi->c_ndblk = SWAB32(ucg->cg_ndblk);
	ucpi->c_rotor = SWAB32(ucg->cg_rotor);
	ucpi->c_frotor = SWAB32(ucg->cg_frotor);
	ucpi->c_irotor = SWAB32(ucg->cg_irotor);
	ucpi->c_btotoff = SWAB32(ucg->cg_btotoff);
	ucpi->c_boff = SWAB32(ucg->cg_boff);
	ucpi->c_iusedoff = SWAB32(ucg->cg_iusedoff);
	ucpi->c_freeoff = SWAB32(ucg->cg_freeoff);
	ucpi->c_nextfreeoff = SWAB32(ucg->cg_nextfreeoff);
	ucpi->c_clustersumoff = SWAB32(ucg->cg_u.cg_44.cg_clustersumoff);
	ucpi->c_clusteroff = SWAB32(ucg->cg_u.cg_44.cg_clusteroff);
	ucpi->c_nclusterblks = SWAB32(ucg->cg_u.cg_44.cg_nclusterblks);
	UFSD(("EXIT\n"))
	return;	
	
failed:
	for (j = 1; j < i; j++)
		brelse (sb->u.ufs_sb.s_ucg[j]);
	sb->u.ufs_sb.s_cgno[bitmap_nr] = UFS_CGNO_EMPTY;
	ufs_error (sb, "ufs_read_cylinder", "can't read cylinder group block %u", cgno);
}

/*
 * Remove cylinder group from cache, doesn't release memory
 * allocated for cylinder group (this is done at ufs_put_super only).
 */
void ufs_put_cylinder (struct super_block * sb, unsigned bitmap_nr)
{
	struct ufs_sb_private_info * uspi; 
	struct ufs_cg_private_info * ucpi;
	struct ufs_cylinder_group * ucg;
	unsigned i;
	unsigned swab;

	UFSD(("ENTER, bitmap_nr %u\n", bitmap_nr))

	swab = sb->u.ufs_sb.s_swab;
	uspi = sb->u.ufs_sb.s_uspi;
	if (sb->u.ufs_sb.s_cgno[bitmap_nr] == UFS_CGNO_EMPTY) {
		UFSD(("EXIT\n"))
		return;
	}
	ucpi = sb->u.ufs_sb.s_ucpi[bitmap_nr];
	ucg = ubh_get_ucg(UCPI_UBH);

	if (uspi->s_ncg > UFS_MAX_GROUP_LOADED && bitmap_nr >= sb->u.ufs_sb.s_cg_loaded) {
		ufs_panic (sb, "ufs_put_cylinder", "internal error");
		return;
	}
	/*
	 * rotor is not so important data, so we put it to disk 
	 * at the end of working with cylinder
	 */
	ucg->cg_rotor = SWAB32(ucpi->c_rotor);
	ucg->cg_frotor = SWAB32(ucpi->c_frotor);
	ucg->cg_irotor = SWAB32(ucpi->c_irotor);
	ubh_mark_buffer_dirty (UCPI_UBH);
	for (i = 1; i < UCPI_UBH->count; i++) {
		brelse (UCPI_UBH->bh[i]);
	}

	sb->u.ufs_sb.s_cgno[bitmap_nr] = UFS_CGNO_EMPTY;
	UFSD(("EXIT\n"))
}

/*
 * Find cylinder group in cache and return it as pointer.
 * If cylinder group is not in cache, we will load it from disk.
 *
 * The cache is managed by LRU algorithm. 
 */
struct ufs_cg_private_info * ufs_load_cylinder (
	struct super_block * sb, unsigned cgno)
{
	struct ufs_sb_private_info * uspi;
	struct ufs_cg_private_info * ucpi;
	unsigned cg, i, j;

	UFSD(("ENTER, cgno %u\n", cgno))

	uspi = sb->u.ufs_sb.s_uspi;
	if (cgno >= uspi->s_ncg) {
		ufs_panic (sb, "ufs_load_cylinder", "internal error, high number of cg");
		return NULL;
	}
	/*
	 * Cylinder group number cg it in cache and it was last used
	 */
	if (sb->u.ufs_sb.s_cgno[0] == cgno) {
		UFSD(("EXIT\n"))
		return sb->u.ufs_sb.s_ucpi[0];
	}
	/*
	 * Number of cylinder groups is not higher than UFS_MAX_GROUP_LOADED
	 */
	if (uspi->s_ncg <= UFS_MAX_GROUP_LOADED) {
		if (sb->u.ufs_sb.s_cgno[cgno] != UFS_CGNO_EMPTY) {
			if (sb->u.ufs_sb.s_cgno[cgno] != cgno) {
				ufs_panic (sb, "ufs_load_cylinder", "internal error, wrong number of cg in cache");
				UFSD(("EXIT (FAILED)\n"))
				return NULL;
			}
			else {
				UFSD(("EXIT\n"))
				return sb->u.ufs_sb.s_ucpi[cgno];
			}
		} else {
			ufs_read_cylinder (sb, cgno, cgno);
			UFSD(("EXIT\n"))
			return sb->u.ufs_sb.s_ucpi[cgno];
		}
	}
	/*
	 * Cylinder group number cg is in cache but it was not last used, 
	 * we will move to the first position
	 */
	for (i = 0; i < sb->u.ufs_sb.s_cg_loaded && sb->u.ufs_sb.s_cgno[i] != cgno; i++);
	if (i < sb->u.ufs_sb.s_cg_loaded && sb->u.ufs_sb.s_cgno[i] == cgno) {
		cg = sb->u.ufs_sb.s_cgno[i];
		ucpi = sb->u.ufs_sb.s_ucpi[i];
		for (j = i; j > 0; j--) {
			sb->u.ufs_sb.s_cgno[j] = sb->u.ufs_sb.s_cgno[j-1];
			sb->u.ufs_sb.s_ucpi[j] = sb->u.ufs_sb.s_ucpi[j-1];
		}
		sb->u.ufs_sb.s_cgno[0] = cg;
		sb->u.ufs_sb.s_ucpi[0] = ucpi;
	/*
	 * Cylinder group number cg is not in cache, we will read it from disk
	 * and put it to the first position
	 */
	} else {
		if (sb->u.ufs_sb.s_cg_loaded < UFS_MAX_GROUP_LOADED)
			sb->u.ufs_sb.s_cg_loaded++;
		else
			ufs_put_cylinder (sb, UFS_MAX_GROUP_LOADED-1);
		ucpi = sb->u.ufs_sb.s_ucpi[sb->u.ufs_sb.s_cg_loaded - 1];
		for (j = sb->u.ufs_sb.s_cg_loaded - 1; j > 0; j--) {
			sb->u.ufs_sb.s_cgno[j] = sb->u.ufs_sb.s_cgno[j-1];
			sb->u.ufs_sb.s_ucpi[j] = sb->u.ufs_sb.s_ucpi[j-1];
		}
		sb->u.ufs_sb.s_ucpi[0] = ucpi;
		ufs_read_cylinder (sb, cgno, 0);
	}
	UFSD(("EXIT\n"))
	return sb->u.ufs_sb.s_ucpi[0];
}
