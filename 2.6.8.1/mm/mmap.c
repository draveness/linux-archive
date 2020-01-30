/*
 * mm/mmap.c
 *
 * Written by obz.
 *
 * Address space accounting code	<alan@redhat.com>
 */

#include <linux/slab.h>
#include <linux/shm.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/syscalls.h>
#include <linux/init.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/personality.h>
#include <linux/security.h>
#include <linux/hugetlb.h>
#include <linux/profile.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/mempolicy.h>
#include <linux/rmap.h>

#include <asm/uaccess.h>
#include <asm/cacheflush.h>
#include <asm/tlb.h>

/*
 * WARNING: the debugging will use recursive algorithms so never enable this
 * unless you know what you are doing.
 */
#undef DEBUG_MM_RB

/* description of effects of mapping type and prot in current implementation.
 * this is due to the limited x86 page protection hardware.  The expected
 * behavior is in parens:
 *
 * map_type	prot
 *		PROT_NONE	PROT_READ	PROT_WRITE	PROT_EXEC
 * MAP_SHARED	r: (no) no	r: (yes) yes	r: (no) yes	r: (no) yes
 *		w: (no) no	w: (no) no	w: (yes) yes	w: (no) no
 *		x: (no) no	x: (no) yes	x: (no) yes	x: (yes) yes
 *		
 * MAP_PRIVATE	r: (no) no	r: (yes) yes	r: (no) yes	r: (no) yes
 *		w: (no) no	w: (no) no	w: (copy) copy	w: (no) no
 *		x: (no) no	x: (no) yes	x: (no) yes	x: (yes) yes
 *
 */
pgprot_t protection_map[16] = {
	__P000, __P001, __P010, __P011, __P100, __P101, __P110, __P111,
	__S000, __S001, __S010, __S011, __S100, __S101, __S110, __S111
};

int sysctl_overcommit_memory = 0;	/* default is heuristic overcommit */
int sysctl_overcommit_ratio = 50;	/* default is 50% */
int sysctl_max_map_count = DEFAULT_MAX_MAP_COUNT;
atomic_t vm_committed_space = ATOMIC_INIT(0);

EXPORT_SYMBOL(sysctl_overcommit_memory);
EXPORT_SYMBOL(sysctl_overcommit_ratio);
EXPORT_SYMBOL(sysctl_max_map_count);
EXPORT_SYMBOL(vm_committed_space);

/*
 * Requires inode->i_mapping->i_mmap_lock
 */
static void __remove_shared_vm_struct(struct vm_area_struct *vma,
		struct file *file, struct address_space *mapping)
{
	if (vma->vm_flags & VM_DENYWRITE)
		atomic_inc(&file->f_dentry->d_inode->i_writecount);
	if (vma->vm_flags & VM_SHARED)
		mapping->i_mmap_writable--;

	flush_dcache_mmap_lock(mapping);
	if (unlikely(vma->vm_flags & VM_NONLINEAR))
		list_del_init(&vma->shared.vm_set.list);
	else
		vma_prio_tree_remove(vma, &mapping->i_mmap);
	flush_dcache_mmap_unlock(mapping);
}

/*
 * Remove one vm structure and free it.
 */
static void remove_vm_struct(struct vm_area_struct *vma)
{
	struct file *file = vma->vm_file;

	if (file) {
		struct address_space *mapping = file->f_mapping;
		spin_lock(&mapping->i_mmap_lock);
		__remove_shared_vm_struct(vma, file, mapping);
		spin_unlock(&mapping->i_mmap_lock);
	}
	if (vma->vm_ops && vma->vm_ops->close)
		vma->vm_ops->close(vma);
	if (file)
		fput(file);
	anon_vma_unlink(vma);
	mpol_free(vma_policy(vma));
	kmem_cache_free(vm_area_cachep, vma);
}

/*
 *  sys_brk() for the most part doesn't need the global kernel
 *  lock, except when an application is doing something nasty
 *  like trying to un-brk an area that has already been mapped
 *  to a regular file.  in this case, the unmapping will need
 *  to invoke file system routines that need the global lock.
 */
asmlinkage unsigned long sys_brk(unsigned long brk)
{
	unsigned long rlim, retval;
	unsigned long newbrk, oldbrk;
	struct mm_struct *mm = current->mm;

	down_write(&mm->mmap_sem);

	if (brk < mm->end_code)
		goto out;
	newbrk = PAGE_ALIGN(brk);
	oldbrk = PAGE_ALIGN(mm->brk);
	if (oldbrk == newbrk)
		goto set_brk;

	/* Always allow shrinking brk. */
	if (brk <= mm->brk) {
		if (!do_munmap(mm, newbrk, oldbrk-newbrk))
			goto set_brk;
		goto out;
	}

	/* Check against rlimit.. */
	rlim = current->rlim[RLIMIT_DATA].rlim_cur;
	if (rlim < RLIM_INFINITY && brk - mm->start_data > rlim)
		goto out;

	/* Check against existing mmap mappings. */
	if (find_vma_intersection(mm, oldbrk, newbrk+PAGE_SIZE))
		goto out;

	/* Ok, looks good - let it rip. */
	if (do_brk(oldbrk, newbrk-oldbrk) != oldbrk)
		goto out;
set_brk:
	mm->brk = brk;
out:
	retval = mm->brk;
	up_write(&mm->mmap_sem);
	return retval;
}

#ifdef DEBUG_MM_RB
static int browse_rb(struct rb_root *root)
{
	int i = 0, j;
	struct rb_node *nd, *pn = NULL;
	unsigned long prev = 0, pend = 0;

	for (nd = rb_first(root); nd; nd = rb_next(nd)) {
		struct vm_area_struct *vma;
		vma = rb_entry(nd, struct vm_area_struct, vm_rb);
		if (vma->vm_start < prev)
			printk("vm_start %lx prev %lx\n", vma->vm_start, prev), i = -1;
		if (vma->vm_start < pend)
			printk("vm_start %lx pend %lx\n", vma->vm_start, pend);
		if (vma->vm_start > vma->vm_end)
			printk("vm_end %lx < vm_start %lx\n", vma->vm_end, vma->vm_start);
		i++;
		pn = nd;
	}
	j = 0;
	for (nd = pn; nd; nd = rb_prev(nd)) {
		j++;
	}
	if (i != j)
		printk("backwards %d, forwards %d\n", j, i), i = 0;
	return i;
}

void validate_mm(struct mm_struct *mm)
{
	int bug = 0;
	int i = 0;
	struct vm_area_struct *tmp = mm->mmap;
	while (tmp) {
		tmp = tmp->vm_next;
		i++;
	}
	if (i != mm->map_count)
		printk("map_count %d vm_next %d\n", mm->map_count, i), bug = 1;
	i = browse_rb(&mm->mm_rb);
	if (i != mm->map_count)
		printk("map_count %d rb %d\n", mm->map_count, i), bug = 1;
	if (bug)
		BUG();
}
#else
#define validate_mm(mm) do { } while (0)
#endif

static struct vm_area_struct *
find_vma_prepare(struct mm_struct *mm, unsigned long addr,
		struct vm_area_struct **pprev, struct rb_node ***rb_link,
		struct rb_node ** rb_parent)
{
	struct vm_area_struct * vma;
	struct rb_node ** __rb_link, * __rb_parent, * rb_prev;

	__rb_link = &mm->mm_rb.rb_node;
	rb_prev = __rb_parent = NULL;
	vma = NULL;

	while (*__rb_link) {
		struct vm_area_struct *vma_tmp;

		__rb_parent = *__rb_link;
		vma_tmp = rb_entry(__rb_parent, struct vm_area_struct, vm_rb);

		if (vma_tmp->vm_end > addr) {
			vma = vma_tmp;
			if (vma_tmp->vm_start <= addr)
				return vma;
			__rb_link = &__rb_parent->rb_left;
		} else {
			rb_prev = __rb_parent;
			__rb_link = &__rb_parent->rb_right;
		}
	}

	*pprev = NULL;
	if (rb_prev)
		*pprev = rb_entry(rb_prev, struct vm_area_struct, vm_rb);
	*rb_link = __rb_link;
	*rb_parent = __rb_parent;
	return vma;
}

static inline void
__vma_link_list(struct mm_struct *mm, struct vm_area_struct *vma,
		struct vm_area_struct *prev, struct rb_node *rb_parent)
{
	if (prev) {
		vma->vm_next = prev->vm_next;
		prev->vm_next = vma;
	} else {
		mm->mmap = vma;
		if (rb_parent)
			vma->vm_next = rb_entry(rb_parent,
					struct vm_area_struct, vm_rb);
		else
			vma->vm_next = NULL;
	}
}

void __vma_link_rb(struct mm_struct *mm, struct vm_area_struct *vma,
		struct rb_node **rb_link, struct rb_node *rb_parent)
{
	rb_link_node(&vma->vm_rb, rb_parent, rb_link);
	rb_insert_color(&vma->vm_rb, &mm->mm_rb);
}

static inline void __vma_link_file(struct vm_area_struct *vma)
{
	struct file * file;

	file = vma->vm_file;
	if (file) {
		struct address_space *mapping = file->f_mapping;

		if (vma->vm_flags & VM_DENYWRITE)
			atomic_dec(&file->f_dentry->d_inode->i_writecount);
		if (vma->vm_flags & VM_SHARED)
			mapping->i_mmap_writable++;

		flush_dcache_mmap_lock(mapping);
		if (unlikely(vma->vm_flags & VM_NONLINEAR))
			list_add_tail(&vma->shared.vm_set.list,
					&mapping->i_mmap_nonlinear);
		else
			vma_prio_tree_insert(vma, &mapping->i_mmap);
		flush_dcache_mmap_unlock(mapping);
	}
}

static void
__vma_link(struct mm_struct *mm, struct vm_area_struct *vma,
	struct vm_area_struct *prev, struct rb_node **rb_link,
	struct rb_node *rb_parent)
{
	__vma_link_list(mm, vma, prev, rb_parent);
	__vma_link_rb(mm, vma, rb_link, rb_parent);
	__anon_vma_link(vma);
}

static void vma_link(struct mm_struct *mm, struct vm_area_struct *vma,
			struct vm_area_struct *prev, struct rb_node **rb_link,
			struct rb_node *rb_parent)
{
	struct address_space *mapping = NULL;

	if (vma->vm_file)
		mapping = vma->vm_file->f_mapping;

	if (mapping)
		spin_lock(&mapping->i_mmap_lock);
	anon_vma_lock(vma);

	__vma_link(mm, vma, prev, rb_link, rb_parent);
	__vma_link_file(vma);

	anon_vma_unlock(vma);
	if (mapping)
		spin_unlock(&mapping->i_mmap_lock);

	mm->map_count++;
	validate_mm(mm);
}

/*
 * Helper for vma_adjust in the split_vma insert case:
 * insert vm structure into list and rbtree and anon_vma,
 * but it has already been inserted into prio_tree earlier.
 */
static void
__insert_vm_struct(struct mm_struct * mm, struct vm_area_struct * vma)
{
	struct vm_area_struct * __vma, * prev;
	struct rb_node ** rb_link, * rb_parent;

	__vma = find_vma_prepare(mm, vma->vm_start,&prev, &rb_link, &rb_parent);
	if (__vma && __vma->vm_start < vma->vm_end)
		BUG();
	__vma_link(mm, vma, prev, rb_link, rb_parent);
	mm->map_count++;
}

static inline void
__vma_unlink(struct mm_struct *mm, struct vm_area_struct *vma,
		struct vm_area_struct *prev)
{
	prev->vm_next = vma->vm_next;
	rb_erase(&vma->vm_rb, &mm->mm_rb);
	if (mm->mmap_cache == vma)
		mm->mmap_cache = prev;
}

/*
 * We cannot adjust vm_start, vm_end, vm_pgoff fields of a vma that
 * is already present in an i_mmap tree without adjusting the tree.
 * The following helper function should be used when such adjustments
 * are necessary.  The "insert" vma (if any) is to be inserted
 * before we drop the necessary locks.
 */
void vma_adjust(struct vm_area_struct *vma, unsigned long start,
	unsigned long end, pgoff_t pgoff, struct vm_area_struct *insert)
{
	struct mm_struct *mm = vma->vm_mm;
	struct vm_area_struct *next = vma->vm_next;
	struct vm_area_struct *importer = NULL;
	struct address_space *mapping = NULL;
	struct prio_tree_root *root = NULL;
	struct file *file = vma->vm_file;
	struct anon_vma *anon_vma = NULL;
	long adjust_next = 0;
	int remove_next = 0;

	if (next && !insert) {
		if (end >= next->vm_end) {
			/*
			 * vma expands, overlapping all the next, and
			 * perhaps the one after too (mprotect case 6).
			 */
again:			remove_next = 1 + (end > next->vm_end);
			end = next->vm_end;
			anon_vma = next->anon_vma;
		} else if (end > next->vm_start) {
			/*
			 * vma expands, overlapping part of the next:
			 * mprotect case 5 shifting the boundary up.
			 */
			adjust_next = (end - next->vm_start) >> PAGE_SHIFT;
			anon_vma = next->anon_vma;
			importer = vma;
		} else if (end < vma->vm_end) {
			/*
			 * vma shrinks, and !insert tells it's not
			 * split_vma inserting another: so it must be
			 * mprotect case 4 shifting the boundary down.
			 */
			adjust_next = - ((vma->vm_end - end) >> PAGE_SHIFT);
			anon_vma = next->anon_vma;
			importer = next;
		}
	}

	if (file) {
		mapping = file->f_mapping;
		if (!(vma->vm_flags & VM_NONLINEAR))
			root = &mapping->i_mmap;
		spin_lock(&mapping->i_mmap_lock);
		if (insert) {
			/*
			 * Put into prio_tree now, so instantiated pages
			 * are visible to arm/parisc __flush_dcache_page
			 * throughout; but we cannot insert into address
			 * space until vma start or end is updated.
			 */
			__vma_link_file(insert);
		}
	}

	/*
	 * When changing only vma->vm_end, we don't really need
	 * anon_vma lock: but is that case worth optimizing out?
	 */
	if (vma->anon_vma)
		anon_vma = vma->anon_vma;
	if (anon_vma) {
		spin_lock(&anon_vma->lock);
		/*
		 * Easily overlooked: when mprotect shifts the boundary,
		 * make sure the expanding vma has anon_vma set if the
		 * shrinking vma had, to cover any anon pages imported.
		 */
		if (importer && !importer->anon_vma) {
			importer->anon_vma = anon_vma;
			__anon_vma_link(importer);
		}
	}

	if (root) {
		flush_dcache_mmap_lock(mapping);
		vma_prio_tree_remove(vma, root);
		if (adjust_next)
			vma_prio_tree_remove(next, root);
	}

	vma->vm_start = start;
	vma->vm_end = end;
	vma->vm_pgoff = pgoff;
	if (adjust_next) {
		next->vm_start += adjust_next << PAGE_SHIFT;
		next->vm_pgoff += adjust_next;
	}

	if (root) {
		if (adjust_next) {
			vma_prio_tree_init(next);
			vma_prio_tree_insert(next, root);
		}
		vma_prio_tree_init(vma);
		vma_prio_tree_insert(vma, root);
		flush_dcache_mmap_unlock(mapping);
	}

	if (remove_next) {
		/*
		 * vma_merge has merged next into vma, and needs
		 * us to remove next before dropping the locks.
		 */
		__vma_unlink(mm, next, vma);
		if (file)
			__remove_shared_vm_struct(next, file, mapping);
		if (next->anon_vma)
			__anon_vma_merge(vma, next);
	} else if (insert) {
		/*
		 * split_vma has split insert from vma, and needs
		 * us to insert it before dropping the locks
		 * (it may either follow vma or precede it).
		 */
		__insert_vm_struct(mm, insert);
	}

	if (anon_vma)
		spin_unlock(&anon_vma->lock);
	if (mapping)
		spin_unlock(&mapping->i_mmap_lock);

	if (remove_next) {
		if (file)
			fput(file);
		mm->map_count--;
		mpol_free(vma_policy(next));
		kmem_cache_free(vm_area_cachep, next);
		/*
		 * In mprotect's case 6 (see comments on vma_merge),
		 * we must remove another next too. It would clutter
		 * up the code too much to do both in one go.
		 */
		if (remove_next == 2) {
			next = vma->vm_next;
			goto again;
		}
	}

	validate_mm(mm);
}

/*
 * If the vma has a ->close operation then the driver probably needs to release
 * per-vma resources, so we don't attempt to merge those.
 */
#define VM_SPECIAL (VM_IO | VM_DONTCOPY | VM_DONTEXPAND | VM_RESERVED)

static inline int is_mergeable_vma(struct vm_area_struct *vma,
			struct file *file, unsigned long vm_flags)
{
	if (vma->vm_flags != vm_flags)
		return 0;
	if (vma->vm_file != file)
		return 0;
	if (vma->vm_ops && vma->vm_ops->close)
		return 0;
	return 1;
}

static inline int is_mergeable_anon_vma(struct anon_vma *anon_vma1,
					struct anon_vma *anon_vma2)
{
	return !anon_vma1 || !anon_vma2 || (anon_vma1 == anon_vma2);
}

/*
 * Return true if we can merge this (vm_flags,anon_vma,file,vm_pgoff)
 * in front of (at a lower virtual address and file offset than) the vma.
 *
 * We cannot merge two vmas if they have differently assigned (non-NULL)
 * anon_vmas, nor if same anon_vma is assigned but offsets incompatible.
 *
 * We don't check here for the merged mmap wrapping around the end of pagecache
 * indices (16TB on ia32) because do_mmap_pgoff() does not permit mmap's which
 * wrap, nor mmaps which cover the final page at index -1UL.
 */
static int
can_vma_merge_before(struct vm_area_struct *vma, unsigned long vm_flags,
	struct anon_vma *anon_vma, struct file *file, pgoff_t vm_pgoff)
{
	if (is_mergeable_vma(vma, file, vm_flags) &&
	    is_mergeable_anon_vma(anon_vma, vma->anon_vma)) {
		if (vma->vm_pgoff == vm_pgoff)
			return 1;
	}
	return 0;
}

/*
 * Return true if we can merge this (vm_flags,anon_vma,file,vm_pgoff)
 * beyond (at a higher virtual address and file offset than) the vma.
 *
 * We cannot merge two vmas if they have differently assigned (non-NULL)
 * anon_vmas, nor if same anon_vma is assigned but offsets incompatible.
 */
static int
can_vma_merge_after(struct vm_area_struct *vma, unsigned long vm_flags,
	struct anon_vma *anon_vma, struct file *file, pgoff_t vm_pgoff)
{
	if (is_mergeable_vma(vma, file, vm_flags) &&
	    is_mergeable_anon_vma(anon_vma, vma->anon_vma)) {
		pgoff_t vm_pglen;
		vm_pglen = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
		if (vma->vm_pgoff + vm_pglen == vm_pgoff)
			return 1;
	}
	return 0;
}

/*
 * Given a mapping request (addr,end,vm_flags,file,pgoff), figure out
 * whether that can be merged with its predecessor or its successor.
 * Or both (it neatly fills a hole).
 *
 * In most cases - when called for mmap, brk or mremap - [addr,end) is
 * certain not to be mapped by the time vma_merge is called; but when
 * called for mprotect, it is certain to be already mapped (either at
 * an offset within prev, or at the start of next), and the flags of
 * this area are about to be changed to vm_flags - and the no-change
 * case has already been eliminated.
 *
 * The following mprotect cases have to be considered, where AAAA is
 * the area passed down from mprotect_fixup, never extending beyond one
 * vma, PPPPPP is the prev vma specified, and NNNNNN the next vma after:
 *
 *     AAAA             AAAA                AAAA          AAAA
 *    PPPPPPNNNNNN    PPPPPPNNNNNN    PPPPPPNNNNNN    PPPPNNNNXXXX
 *    cannot merge    might become    might become    might become
 *                    PPNNNNNNNNNN    PPPPPPPPPPNN    PPPPPPPPPPPP 6 or
 *    mmap, brk or    case 4 below    case 5 below    PPPPPPPPXXXX 7 or
 *    mremap move:                                    PPPPNNNNNNNN 8
 *        AAAA
 *    PPPP    NNNN    PPPPPPPPPPPP    PPPPPPPPNNNN    PPPPNNNNNNNN
 *    might become    case 1 below    case 2 below    case 3 below
 *
 * Odd one out? Case 8, because it extends NNNN but needs flags of XXXX:
 * mprotect_fixup updates vm_flags & vm_page_prot on successful return.
 */
struct vm_area_struct *vma_merge(struct mm_struct *mm,
			struct vm_area_struct *prev, unsigned long addr,
			unsigned long end, unsigned long vm_flags,
		     	struct anon_vma *anon_vma, struct file *file,
			pgoff_t pgoff, struct mempolicy *policy)
{
	pgoff_t pglen = (end - addr) >> PAGE_SHIFT;
	struct vm_area_struct *area, *next;

	/*
	 * We later require that vma->vm_flags == vm_flags,
	 * so this tests vma->vm_flags & VM_SPECIAL, too.
	 */
	if (vm_flags & VM_SPECIAL)
		return NULL;

	if (prev)
		next = prev->vm_next;
	else
		next = mm->mmap;
	area = next;
	if (next && next->vm_end == end)		/* cases 6, 7, 8 */
		next = next->vm_next;

	/*
	 * Can it merge with the predecessor?
	 */
	if (prev && prev->vm_end == addr &&
  			mpol_equal(vma_policy(prev), policy) &&
			can_vma_merge_after(prev, vm_flags,
						anon_vma, file, pgoff)) {
		/*
		 * OK, it can.  Can we now merge in the successor as well?
		 */
		if (next && end == next->vm_start &&
				mpol_equal(policy, vma_policy(next)) &&
				can_vma_merge_before(next, vm_flags,
					anon_vma, file, pgoff+pglen) &&
				is_mergeable_anon_vma(prev->anon_vma,
						      next->anon_vma)) {
							/* cases 1, 6 */
			vma_adjust(prev, prev->vm_start,
				next->vm_end, prev->vm_pgoff, NULL);
		} else					/* cases 2, 5, 7 */
			vma_adjust(prev, prev->vm_start,
				end, prev->vm_pgoff, NULL);
		return prev;
	}

	/*
	 * Can this new request be merged in front of next?
	 */
	if (next && end == next->vm_start &&
 			mpol_equal(policy, vma_policy(next)) &&
			can_vma_merge_before(next, vm_flags,
					anon_vma, file, pgoff+pglen)) {
		if (prev && addr < prev->vm_end)	/* case 4 */
			vma_adjust(prev, prev->vm_start,
				addr, prev->vm_pgoff, NULL);
		else					/* cases 3, 8 */
			vma_adjust(area, addr, next->vm_end,
				next->vm_pgoff - pglen, NULL);
		return area;
	}

	return NULL;
}

/*
 * find_mergeable_anon_vma is used by anon_vma_prepare, to check
 * neighbouring vmas for a suitable anon_vma, before it goes off
 * to allocate a new anon_vma.  It checks because a repetitive
 * sequence of mprotects and faults may otherwise lead to distinct
 * anon_vmas being allocated, preventing vma merge in subsequent
 * mprotect.
 */
struct anon_vma *find_mergeable_anon_vma(struct vm_area_struct *vma)
{
	struct vm_area_struct *near;
	unsigned long vm_flags;

	near = vma->vm_next;
	if (!near)
		goto try_prev;

	/*
	 * Since only mprotect tries to remerge vmas, match flags
	 * which might be mprotected into each other later on.
	 * Neither mlock nor madvise tries to remerge at present,
	 * so leave their flags as obstructing a merge.
	 */
	vm_flags = vma->vm_flags & ~(VM_READ|VM_WRITE|VM_EXEC);
	vm_flags |= near->vm_flags & (VM_READ|VM_WRITE|VM_EXEC);

	if (near->anon_vma && vma->vm_end == near->vm_start &&
 			mpol_equal(vma_policy(vma), vma_policy(near)) &&
			can_vma_merge_before(near, vm_flags,
				NULL, vma->vm_file, vma->vm_pgoff +
				((vma->vm_end - vma->vm_start) >> PAGE_SHIFT)))
		return near->anon_vma;
try_prev:
	/*
	 * It is potentially slow to have to call find_vma_prev here.
	 * But it's only on the first write fault on the vma, not
	 * every time, and we could devise a way to avoid it later
	 * (e.g. stash info in next's anon_vma_node when assigning
	 * an anon_vma, or when trying vma_merge).  Another time.
	 */
	if (find_vma_prev(vma->vm_mm, vma->vm_start, &near) != vma)
		BUG();
	if (!near)
		goto none;

	vm_flags = vma->vm_flags & ~(VM_READ|VM_WRITE|VM_EXEC);
	vm_flags |= near->vm_flags & (VM_READ|VM_WRITE|VM_EXEC);

	if (near->anon_vma && near->vm_end == vma->vm_start &&
  			mpol_equal(vma_policy(near), vma_policy(vma)) &&
			can_vma_merge_after(near, vm_flags,
				NULL, vma->vm_file, vma->vm_pgoff))
		return near->anon_vma;
none:
	/*
	 * There's no absolute need to look only at touching neighbours:
	 * we could search further afield for "compatible" anon_vmas.
	 * But it would probably just be a waste of time searching,
	 * or lead to too many vmas hanging off the same anon_vma.
	 * We're trying to allow mprotect remerging later on,
	 * not trying to minimize memory used for anon_vmas.
	 */
	return NULL;
}

/*
 * The caller must hold down_write(current->mm->mmap_sem).
 */

unsigned long do_mmap_pgoff(struct file * file, unsigned long addr,
			unsigned long len, unsigned long prot,
			unsigned long flags, unsigned long pgoff)
{
	struct mm_struct * mm = current->mm;
	struct vm_area_struct * vma, * prev;
	struct inode *inode;
	unsigned int vm_flags;
	int correct_wcount = 0;
	int error;
	struct rb_node ** rb_link, * rb_parent;
	int accountable = 1;
	unsigned long charged = 0;

	/*
	 * Does the application expect PROT_READ to imply PROT_EXEC:
	 */
	if (unlikely((prot & PROT_READ) &&
			(current->personality & READ_IMPLIES_EXEC)))
		prot |= PROT_EXEC;

	if (file) {
		if (is_file_hugepages(file))
			accountable = 0;

		if (!file->f_op || !file->f_op->mmap)
			return -ENODEV;

		if ((prot & PROT_EXEC) &&
		    (file->f_vfsmnt->mnt_flags & MNT_NOEXEC))
			return -EPERM;
	}

	if (!len)
		return addr;

	/* Careful about overflows.. */
	len = PAGE_ALIGN(len);
	if (!len || len > TASK_SIZE)
		return -EINVAL;

	/* offset overflow? */
	if ((pgoff + (len >> PAGE_SHIFT)) < pgoff)
		return -EINVAL;

	/* Too many mappings? */
	if (mm->map_count > sysctl_max_map_count)
		return -ENOMEM;

	/* Obtain the address to map to. we verify (or select) it and ensure
	 * that it represents a valid section of the address space.
	 */
	addr = get_unmapped_area(file, addr, len, pgoff, flags);
	if (addr & ~PAGE_MASK)
		return addr;

	/* Do simple checking here so the lower-level routines won't have
	 * to. we assume access permissions have been handled by the open
	 * of the memory object, so we don't do any here.
	 */
	vm_flags = calc_vm_prot_bits(prot) | calc_vm_flag_bits(flags) |
			mm->def_flags | VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC;

	if (flags & MAP_LOCKED) {
		if (!capable(CAP_IPC_LOCK))
			return -EPERM;
		vm_flags |= VM_LOCKED;
	}
	/* mlock MCL_FUTURE? */
	if (vm_flags & VM_LOCKED) {
		unsigned long locked = mm->locked_vm << PAGE_SHIFT;
		locked += len;
		if (locked > current->rlim[RLIMIT_MEMLOCK].rlim_cur)
			return -EAGAIN;
	}

	inode = file ? file->f_dentry->d_inode : NULL;

	if (file) {
		switch (flags & MAP_TYPE) {
		case MAP_SHARED:
			if ((prot&PROT_WRITE) && !(file->f_mode&FMODE_WRITE))
				return -EACCES;

			/*
			 * Make sure we don't allow writing to an append-only
			 * file..
			 */
			if (IS_APPEND(inode) && (file->f_mode & FMODE_WRITE))
				return -EACCES;

			/*
			 * Make sure there are no mandatory locks on the file.
			 */
			if (locks_verify_locked(inode))
				return -EAGAIN;

			vm_flags |= VM_SHARED | VM_MAYSHARE;
			if (!(file->f_mode & FMODE_WRITE))
				vm_flags &= ~(VM_MAYWRITE | VM_SHARED);

			/* fall through */
		case MAP_PRIVATE:
			if (!(file->f_mode & FMODE_READ))
				return -EACCES;
			break;

		default:
			return -EINVAL;
		}
	} else {
		switch (flags & MAP_TYPE) {
		case MAP_SHARED:
			vm_flags |= VM_SHARED | VM_MAYSHARE;
			break;
		case MAP_PRIVATE:
			/*
			 * Set pgoff according to addr for anon_vma.
			 */
			pgoff = addr >> PAGE_SHIFT;
			break;
		default:
			return -EINVAL;
		}
	}

	error = security_file_mmap(file, prot, flags);
	if (error)
		return error;
		
	/* Clear old maps */
	error = -ENOMEM;
munmap_back:
	vma = find_vma_prepare(mm, addr, &prev, &rb_link, &rb_parent);
	if (vma && vma->vm_start < addr + len) {
		if (do_munmap(mm, addr, len))
			return -ENOMEM;
		goto munmap_back;
	}

	/* Check against address space limit. */
	if ((mm->total_vm << PAGE_SHIFT) + len
	    > current->rlim[RLIMIT_AS].rlim_cur)
		return -ENOMEM;

	if (accountable && (!(flags & MAP_NORESERVE) ||
			sysctl_overcommit_memory > 1)) {
		if (vm_flags & VM_SHARED) {
			/* Check memory availability in shmem_file_setup? */
			vm_flags |= VM_ACCOUNT;
		} else if (vm_flags & VM_WRITE) {
			/*
			 * Private writable mapping: check memory availability
			 */
			charged = len >> PAGE_SHIFT;
			if (security_vm_enough_memory(charged))
				return -ENOMEM;
			vm_flags |= VM_ACCOUNT;
		}
	}

	/*
	 * Can we just expand an old private anonymous mapping?
	 * The VM_SHARED test is necessary because shmem_zero_setup
	 * will create the file object for a shared anonymous map below.
	 */
	if (!file && !(vm_flags & VM_SHARED) &&
	    vma_merge(mm, prev, addr, addr + len, vm_flags,
					NULL, NULL, pgoff, NULL))
		goto out;

	/*
	 * Determine the object being mapped and call the appropriate
	 * specific mapper. the address has already been validated, but
	 * not unmapped, but the maps are removed from the list.
	 */
	vma = kmem_cache_alloc(vm_area_cachep, SLAB_KERNEL);
	if (!vma) {
		error = -ENOMEM;
		goto unacct_error;
	}
	memset(vma, 0, sizeof(*vma));

	vma->vm_mm = mm;
	vma->vm_start = addr;
	vma->vm_end = addr + len;
	vma->vm_flags = vm_flags;
	vma->vm_page_prot = protection_map[vm_flags & 0x0f];
	vma->vm_pgoff = pgoff;

	if (file) {
		error = -EINVAL;
		if (vm_flags & (VM_GROWSDOWN|VM_GROWSUP))
			goto free_vma;
		if (vm_flags & VM_DENYWRITE) {
			error = deny_write_access(file);
			if (error)
				goto free_vma;
			correct_wcount = 1;
		}
		vma->vm_file = file;
		get_file(file);
		error = file->f_op->mmap(file, vma);
		if (error)
			goto unmap_and_free_vma;
	} else if (vm_flags & VM_SHARED) {
		error = shmem_zero_setup(vma);
		if (error)
			goto free_vma;
	}

	/* We set VM_ACCOUNT in a shared mapping's vm_flags, to inform
	 * shmem_zero_setup (perhaps called through /dev/zero's ->mmap)
	 * that memory reservation must be checked; but that reservation
	 * belongs to shared memory object, not to vma: so now clear it.
	 */
	if ((vm_flags & (VM_SHARED|VM_ACCOUNT)) == (VM_SHARED|VM_ACCOUNT))
		vma->vm_flags &= ~VM_ACCOUNT;

	/* Can addr have changed??
	 *
	 * Answer: Yes, several device drivers can do it in their
	 *         f_op->mmap method. -DaveM
	 */
	addr = vma->vm_start;

	if (!file || !vma_merge(mm, prev, addr, vma->vm_end,
			vma->vm_flags, NULL, file, pgoff, vma_policy(vma))) {
		vma_link(mm, vma, prev, rb_link, rb_parent);
		if (correct_wcount)
			atomic_inc(&inode->i_writecount);
	} else {
		if (file) {
			if (correct_wcount)
				atomic_inc(&inode->i_writecount);
			fput(file);
		}
		mpol_free(vma_policy(vma));
		kmem_cache_free(vm_area_cachep, vma);
	}
out:	
	mm->total_vm += len >> PAGE_SHIFT;
	if (vm_flags & VM_LOCKED) {
		mm->locked_vm += len >> PAGE_SHIFT;
		make_pages_present(addr, addr + len);
	}
	if (flags & MAP_POPULATE) {
		up_write(&mm->mmap_sem);
		sys_remap_file_pages(addr, len, 0,
					pgoff, flags & MAP_NONBLOCK);
		down_write(&mm->mmap_sem);
	}
	return addr;

unmap_and_free_vma:
	if (correct_wcount)
		atomic_inc(&inode->i_writecount);
	vma->vm_file = NULL;
	fput(file);

	/* Undo any partial mapping done by a device driver. */
	zap_page_range(vma, vma->vm_start, vma->vm_end - vma->vm_start, NULL);
free_vma:
	kmem_cache_free(vm_area_cachep, vma);
unacct_error:
	if (charged)
		vm_unacct_memory(charged);
	return error;
}

EXPORT_SYMBOL(do_mmap_pgoff);

/* Get an address range which is currently unmapped.
 * For shmat() with addr=0.
 *
 * Ugly calling convention alert:
 * Return value with the low bits set means error value,
 * ie
 *	if (ret & ~PAGE_MASK)
 *		error = ret;
 *
 * This function "knows" that -ENOMEM has the bits set.
 */
#ifndef HAVE_ARCH_UNMAPPED_AREA
static inline unsigned long
arch_get_unmapped_area(struct file *filp, unsigned long addr,
		unsigned long len, unsigned long pgoff, unsigned long flags)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long start_addr;

	if (len > TASK_SIZE)
		return -ENOMEM;

	if (addr) {
		addr = PAGE_ALIGN(addr);
		vma = find_vma(mm, addr);
		if (TASK_SIZE - len >= addr &&
		    (!vma || addr + len <= vma->vm_start))
			return addr;
	}
	start_addr = addr = mm->free_area_cache;

full_search:
	for (vma = find_vma(mm, addr); ; vma = vma->vm_next) {
		/* At this point:  (!vma || addr < vma->vm_end). */
		if (TASK_SIZE - len < addr) {
			/*
			 * Start a new search - just in case we missed
			 * some holes.
			 */
			if (start_addr != TASK_UNMAPPED_BASE) {
				start_addr = addr = TASK_UNMAPPED_BASE;
				goto full_search;
			}
			return -ENOMEM;
		}
		if (!vma || addr + len <= vma->vm_start) {
			/*
			 * Remember the place where we stopped the search:
			 */
			mm->free_area_cache = addr + len;
			return addr;
		}
		addr = vma->vm_end;
	}
}
#else
extern unsigned long
arch_get_unmapped_area(struct file *, unsigned long, unsigned long,
			unsigned long, unsigned long);
#endif	

unsigned long
get_unmapped_area(struct file *file, unsigned long addr, unsigned long len,
		unsigned long pgoff, unsigned long flags)
{
	if (flags & MAP_FIXED) {
		unsigned long ret;

		if (addr > TASK_SIZE - len)
			return -ENOMEM;
		if (addr & ~PAGE_MASK)
			return -EINVAL;
		if (file && is_file_hugepages(file))  {
			/*
			 * Check if the given range is hugepage aligned, and
			 * can be made suitable for hugepages.
			 */
			ret = prepare_hugepage_range(addr, len);
		} else {
			/*
			 * Ensure that a normal request is not falling in a
			 * reserved hugepage range.  For some archs like IA-64,
			 * there is a separate region for hugepages.
			 */
			ret = is_hugepage_only_range(addr, len);
		}
		if (ret)
			return -EINVAL;
		return addr;
	}

	if (file && file->f_op && file->f_op->get_unmapped_area)
		return file->f_op->get_unmapped_area(file, addr, len,
						pgoff, flags);

	return arch_get_unmapped_area(file, addr, len, pgoff, flags);
}

EXPORT_SYMBOL(get_unmapped_area);

/* Look up the first VMA which satisfies  addr < vm_end,  NULL if none. */
struct vm_area_struct * find_vma(struct mm_struct * mm, unsigned long addr)
{
	struct vm_area_struct *vma = NULL;

	if (mm) {
		/* Check the cache first. */
		/* (Cache hit rate is typically around 35%.) */
		vma = mm->mmap_cache;
		if (!(vma && vma->vm_end > addr && vma->vm_start <= addr)) {
			struct rb_node * rb_node;

			rb_node = mm->mm_rb.rb_node;
			vma = NULL;

			while (rb_node) {
				struct vm_area_struct * vma_tmp;

				vma_tmp = rb_entry(rb_node,
						struct vm_area_struct, vm_rb);

				if (vma_tmp->vm_end > addr) {
					vma = vma_tmp;
					if (vma_tmp->vm_start <= addr)
						break;
					rb_node = rb_node->rb_left;
				} else
					rb_node = rb_node->rb_right;
			}
			if (vma)
				mm->mmap_cache = vma;
		}
	}
	return vma;
}

EXPORT_SYMBOL(find_vma);

/* Same as find_vma, but also return a pointer to the previous VMA in *pprev. */
struct vm_area_struct *
find_vma_prev(struct mm_struct *mm, unsigned long addr,
			struct vm_area_struct **pprev)
{
	struct vm_area_struct *vma = NULL, *prev = NULL;
	struct rb_node * rb_node;
	if (!mm)
		goto out;

	/* Guard against addr being lower than the first VMA */
	vma = mm->mmap;

	/* Go through the RB tree quickly. */
	rb_node = mm->mm_rb.rb_node;

	while (rb_node) {
		struct vm_area_struct *vma_tmp;
		vma_tmp = rb_entry(rb_node, struct vm_area_struct, vm_rb);

		if (addr < vma_tmp->vm_end) {
			rb_node = rb_node->rb_left;
		} else {
			prev = vma_tmp;
			if (!prev->vm_next || (addr < prev->vm_next->vm_end))
				break;
			rb_node = rb_node->rb_right;
		}
	}

out:
	*pprev = prev;
	return prev ? prev->vm_next : vma;
}

#ifdef CONFIG_STACK_GROWSUP
/*
 * vma is the first one with address > vma->vm_end.  Have to extend vma.
 */
int expand_stack(struct vm_area_struct * vma, unsigned long address)
{
	unsigned long grow;

	if (!(vma->vm_flags & VM_GROWSUP))
		return -EFAULT;

	/*
	 * We must make sure the anon_vma is allocated
	 * so that the anon_vma locking is not a noop.
	 */
	if (unlikely(anon_vma_prepare(vma)))
		return -ENOMEM;
	anon_vma_lock(vma);

	/*
	 * vma->vm_start/vm_end cannot change under us because the caller
	 * is required to hold the mmap_sem in read mode.  We need the
	 * anon_vma lock to serialize against concurrent expand_stacks.
	 */
	address += 4 + PAGE_SIZE - 1;
	address &= PAGE_MASK;
	grow = (address - vma->vm_end) >> PAGE_SHIFT;

	/* Overcommit.. */
	if (security_vm_enough_memory(grow)) {
		anon_vma_unlock(vma);
		return -ENOMEM;
	}
	
	if (address - vma->vm_start > current->rlim[RLIMIT_STACK].rlim_cur ||
			((vma->vm_mm->total_vm + grow) << PAGE_SHIFT) >
			current->rlim[RLIMIT_AS].rlim_cur) {
		anon_vma_unlock(vma);
		vm_unacct_memory(grow);
		return -ENOMEM;
	}
	vma->vm_end = address;
	vma->vm_mm->total_vm += grow;
	if (vma->vm_flags & VM_LOCKED)
		vma->vm_mm->locked_vm += grow;
	anon_vma_unlock(vma);
	return 0;
}

struct vm_area_struct *
find_extend_vma(struct mm_struct *mm, unsigned long addr)
{
	struct vm_area_struct *vma, *prev;

	addr &= PAGE_MASK;
	vma = find_vma_prev(mm, addr, &prev);
	if (vma && (vma->vm_start <= addr))
		return vma;
	if (!prev || expand_stack(prev, addr))
		return NULL;
	if (prev->vm_flags & VM_LOCKED) {
		make_pages_present(addr, prev->vm_end);
	}
	return prev;
}
#else
/*
 * vma is the first one with address < vma->vm_start.  Have to extend vma.
 */
int expand_stack(struct vm_area_struct *vma, unsigned long address)
{
	unsigned long grow;

	/*
	 * We must make sure the anon_vma is allocated
	 * so that the anon_vma locking is not a noop.
	 */
	if (unlikely(anon_vma_prepare(vma)))
		return -ENOMEM;
	anon_vma_lock(vma);

	/*
	 * vma->vm_start/vm_end cannot change under us because the caller
	 * is required to hold the mmap_sem in read mode.  We need the
	 * anon_vma lock to serialize against concurrent expand_stacks.
	 */
	address &= PAGE_MASK;
	grow = (vma->vm_start - address) >> PAGE_SHIFT;

	/* Overcommit.. */
	if (security_vm_enough_memory(grow)) {
		anon_vma_unlock(vma);
		return -ENOMEM;
	}
	
	if (vma->vm_end - address > current->rlim[RLIMIT_STACK].rlim_cur ||
			((vma->vm_mm->total_vm + grow) << PAGE_SHIFT) >
			current->rlim[RLIMIT_AS].rlim_cur) {
		anon_vma_unlock(vma);
		vm_unacct_memory(grow);
		return -ENOMEM;
	}
	vma->vm_start = address;
	vma->vm_pgoff -= grow;
	vma->vm_mm->total_vm += grow;
	if (vma->vm_flags & VM_LOCKED)
		vma->vm_mm->locked_vm += grow;
	anon_vma_unlock(vma);
	return 0;
}

struct vm_area_struct *
find_extend_vma(struct mm_struct * mm, unsigned long addr)
{
	struct vm_area_struct * vma;
	unsigned long start;

	addr &= PAGE_MASK;
	vma = find_vma(mm,addr);
	if (!vma)
		return NULL;
	if (vma->vm_start <= addr)
		return vma;
	if (!(vma->vm_flags & VM_GROWSDOWN))
		return NULL;
	start = vma->vm_start;
	if (expand_stack(vma, addr))
		return NULL;
	if (vma->vm_flags & VM_LOCKED) {
		make_pages_present(addr, start);
	}
	return vma;
}
#endif

/*
 * Try to free as many page directory entries as we can,
 * without having to work very hard at actually scanning
 * the page tables themselves.
 *
 * Right now we try to free page tables if we have a nice
 * PGDIR-aligned area that got free'd up. We could be more
 * granular if we want to, but this is fast and simple,
 * and covers the bad cases.
 *
 * "prev", if it exists, points to a vma before the one
 * we just free'd - but there's no telling how much before.
 */
static void free_pgtables(struct mmu_gather *tlb, struct vm_area_struct *prev,
	unsigned long start, unsigned long end)
{
	unsigned long first = start & PGDIR_MASK;
	unsigned long last = end + PGDIR_SIZE - 1;
	unsigned long start_index, end_index;
	struct mm_struct *mm = tlb->mm;

	if (!prev) {
		prev = mm->mmap;
		if (!prev)
			goto no_mmaps;
		if (prev->vm_end > start) {
			if (last > prev->vm_start)
				last = prev->vm_start;
			goto no_mmaps;
		}
	}
	for (;;) {
		struct vm_area_struct *next = prev->vm_next;

		if (next) {
			if (next->vm_start < start) {
				prev = next;
				continue;
			}
			if (last > next->vm_start)
				last = next->vm_start;
		}
		if (prev->vm_end > first)
			first = prev->vm_end + PGDIR_SIZE - 1;
		break;
	}
no_mmaps:
	if (last < first)	/* for arches with discontiguous pgd indices */
		return;
	/*
	 * If the PGD bits are not consecutive in the virtual address, the
	 * old method of shifting the VA >> by PGDIR_SHIFT doesn't work.
	 */
	start_index = pgd_index(first);
	if (start_index < FIRST_USER_PGD_NR)
		start_index = FIRST_USER_PGD_NR;
	end_index = pgd_index(last);
	if (end_index > start_index) {
		clear_page_tables(tlb, start_index, end_index - start_index);
		flush_tlb_pgtables(mm, first & PGDIR_MASK, last & PGDIR_MASK);
	}
}

/* Normal function to fix up a mapping
 * This function is the default for when an area has no specific
 * function.  This may be used as part of a more specific routine.
 *
 * By the time this function is called, the area struct has been
 * removed from the process mapping list.
 */
static void unmap_vma(struct mm_struct *mm, struct vm_area_struct *area)
{
	size_t len = area->vm_end - area->vm_start;

	area->vm_mm->total_vm -= len >> PAGE_SHIFT;
	if (area->vm_flags & VM_LOCKED)
		area->vm_mm->locked_vm -= len >> PAGE_SHIFT;
	/*
	 * Is this a new hole at the lowest possible address?
	 */
	if (area->vm_start >= TASK_UNMAPPED_BASE &&
				area->vm_start < area->vm_mm->free_area_cache)
	      area->vm_mm->free_area_cache = area->vm_start;

	remove_vm_struct(area);
}

/*
 * Update the VMA and inode share lists.
 *
 * Ok - we have the memory areas we should free on the 'free' list,
 * so release them, and do the vma updates.
 */
static void unmap_vma_list(struct mm_struct *mm,
	struct vm_area_struct *mpnt)
{
	do {
		struct vm_area_struct *next = mpnt->vm_next;
		unmap_vma(mm, mpnt);
		mpnt = next;
	} while (mpnt != NULL);
	validate_mm(mm);
}

/*
 * Get rid of page table information in the indicated region.
 *
 * Called with the page table lock held.
 */
static void unmap_region(struct mm_struct *mm,
	struct vm_area_struct *vma,
	struct vm_area_struct *prev,
	unsigned long start,
	unsigned long end)
{
	struct mmu_gather *tlb;
	unsigned long nr_accounted = 0;

	lru_add_drain();
	tlb = tlb_gather_mmu(mm, 0);
	unmap_vmas(&tlb, mm, vma, start, end, &nr_accounted, NULL);
	vm_unacct_memory(nr_accounted);

	if (is_hugepage_only_range(start, end - start))
		hugetlb_free_pgtables(tlb, prev, start, end);
	else
		free_pgtables(tlb, prev, start, end);
	tlb_finish_mmu(tlb, start, end);
}

/*
 * Create a list of vma's touched by the unmap, removing them from the mm's
 * vma list as we go..
 */
static void
detach_vmas_to_be_unmapped(struct mm_struct *mm, struct vm_area_struct *vma,
	struct vm_area_struct *prev, unsigned long end)
{
	struct vm_area_struct **insertion_point;
	struct vm_area_struct *tail_vma = NULL;

	insertion_point = (prev ? &prev->vm_next : &mm->mmap);
	do {
		rb_erase(&vma->vm_rb, &mm->mm_rb);
		mm->map_count--;
		tail_vma = vma;
		vma = vma->vm_next;
	} while (vma && vma->vm_start < end);
	*insertion_point = vma;
	tail_vma->vm_next = NULL;
	mm->mmap_cache = NULL;		/* Kill the cache. */
}

/*
 * Split a vma into two pieces at address 'addr', a new vma is allocated
 * either for the first part or the the tail.
 */
int split_vma(struct mm_struct * mm, struct vm_area_struct * vma,
	      unsigned long addr, int new_below)
{
	struct mempolicy *pol;
	struct vm_area_struct *new;

	if (mm->map_count >= sysctl_max_map_count)
		return -ENOMEM;

	new = kmem_cache_alloc(vm_area_cachep, SLAB_KERNEL);
	if (!new)
		return -ENOMEM;

	/* most fields are the same, copy all, and then fixup */
	*new = *vma;
	vma_prio_tree_init(new);

	if (new_below)
		new->vm_end = addr;
	else {
		new->vm_start = addr;
		new->vm_pgoff += ((addr - vma->vm_start) >> PAGE_SHIFT);
	}

	pol = mpol_copy(vma_policy(vma));
	if (IS_ERR(pol)) {
		kmem_cache_free(vm_area_cachep, new);
		return PTR_ERR(pol);
	}
	vma_set_policy(new, pol);

	if (new->vm_file)
		get_file(new->vm_file);

	if (new->vm_ops && new->vm_ops->open)
		new->vm_ops->open(new);

	if (new_below)
		vma_adjust(vma, addr, vma->vm_end, vma->vm_pgoff +
			((addr - new->vm_start) >> PAGE_SHIFT), new);
	else
		vma_adjust(vma, vma->vm_start, addr, vma->vm_pgoff, new);

	return 0;
}

/* Munmap is split into 2 main parts -- this part which finds
 * what needs doing, and the areas themselves, which do the
 * work.  This now handles partial unmappings.
 * Jeremy Fitzhardinge <jeremy@goop.org>
 */
int do_munmap(struct mm_struct *mm, unsigned long start, size_t len)
{
	unsigned long end;
	struct vm_area_struct *mpnt, *prev, *last;

	if ((start & ~PAGE_MASK) || start > TASK_SIZE || len > TASK_SIZE-start)
		return -EINVAL;

	if ((len = PAGE_ALIGN(len)) == 0)
		return -EINVAL;

	/* Find the first overlapping VMA */
	mpnt = find_vma_prev(mm, start, &prev);
	if (!mpnt)
		return 0;
	/* we have  start < mpnt->vm_end  */

	if (is_vm_hugetlb_page(mpnt)) {
		int ret = is_aligned_hugepage_range(start, len);

		if (ret)
			return ret;
	}

	/* if it doesn't overlap, we have nothing.. */
	end = start + len;
	if (mpnt->vm_start >= end)
		return 0;

	/* Something will probably happen, so notify. */
	if (mpnt->vm_file && (mpnt->vm_flags & VM_EXEC))
		profile_exec_unmap(mm);
 
	/*
	 * If we need to split any vma, do it now to save pain later.
	 *
	 * Note: mremap's move_vma VM_ACCOUNT handling assumes a partially
	 * unmapped vm_area_struct will remain in use: so lower split_vma
	 * places tmp vma above, and higher split_vma places tmp vma below.
	 */
	if (start > mpnt->vm_start) {
		if (split_vma(mm, mpnt, start, 0))
			return -ENOMEM;
		prev = mpnt;
	}

	/* Does it split the last one? */
	last = find_vma(mm, end);
	if (last && end > last->vm_start) {
		if (split_vma(mm, last, end, 1))
			return -ENOMEM;
	}
	mpnt = prev? prev->vm_next: mm->mmap;

	/*
	 * Remove the vma's, and unmap the actual pages
	 */
	detach_vmas_to_be_unmapped(mm, mpnt, prev, end);
	spin_lock(&mm->page_table_lock);
	unmap_region(mm, mpnt, prev, start, end);
	spin_unlock(&mm->page_table_lock);

	/* Fix up all other VM information */
	unmap_vma_list(mm, mpnt);

	return 0;
}

EXPORT_SYMBOL(do_munmap);

asmlinkage long sys_munmap(unsigned long addr, size_t len)
{
	int ret;
	struct mm_struct *mm = current->mm;

	down_write(&mm->mmap_sem);
	ret = do_munmap(mm, addr, len);
	up_write(&mm->mmap_sem);
	return ret;
}

/*
 *  this is really a simplified "do_mmap".  it only handles
 *  anonymous maps.  eventually we may be able to do some
 *  brk-specific accounting here.
 */
unsigned long do_brk(unsigned long addr, unsigned long len)
{
	struct mm_struct * mm = current->mm;
	struct vm_area_struct * vma, * prev;
	unsigned long flags;
	struct rb_node ** rb_link, * rb_parent;
	pgoff_t pgoff = addr >> PAGE_SHIFT;

	len = PAGE_ALIGN(len);
	if (!len)
		return addr;

	if ((addr + len) > TASK_SIZE || (addr + len) < addr)
		return -EINVAL;

	/*
	 * mlock MCL_FUTURE?
	 */
	if (mm->def_flags & VM_LOCKED) {
		unsigned long locked = mm->locked_vm << PAGE_SHIFT;
		locked += len;
		if (locked > current->rlim[RLIMIT_MEMLOCK].rlim_cur)
			return -EAGAIN;
	}

	/*
	 * Clear old maps.  this also does some error checking for us
	 */
 munmap_back:
	vma = find_vma_prepare(mm, addr, &prev, &rb_link, &rb_parent);
	if (vma && vma->vm_start < addr + len) {
		if (do_munmap(mm, addr, len))
			return -ENOMEM;
		goto munmap_back;
	}

	/* Check against address space limits *after* clearing old maps... */
	if ((mm->total_vm << PAGE_SHIFT) + len
	    > current->rlim[RLIMIT_AS].rlim_cur)
		return -ENOMEM;

	if (mm->map_count > sysctl_max_map_count)
		return -ENOMEM;

	if (security_vm_enough_memory(len >> PAGE_SHIFT))
		return -ENOMEM;

	flags = VM_DATA_DEFAULT_FLAGS | VM_ACCOUNT | mm->def_flags;

	/* Can we just expand an old private anonymous mapping? */
	if (vma_merge(mm, prev, addr, addr + len, flags,
					NULL, NULL, pgoff, NULL))
		goto out;

	/*
	 * create a vma struct for an anonymous mapping
	 */
	vma = kmem_cache_alloc(vm_area_cachep, SLAB_KERNEL);
	if (!vma) {
		vm_unacct_memory(len >> PAGE_SHIFT);
		return -ENOMEM;
	}
	memset(vma, 0, sizeof(*vma));

	vma->vm_mm = mm;
	vma->vm_start = addr;
	vma->vm_end = addr + len;
	vma->vm_pgoff = pgoff;
	vma->vm_flags = flags;
	vma->vm_page_prot = protection_map[flags & 0x0f];
	vma_link(mm, vma, prev, rb_link, rb_parent);
out:
	mm->total_vm += len >> PAGE_SHIFT;
	if (flags & VM_LOCKED) {
		mm->locked_vm += len >> PAGE_SHIFT;
		make_pages_present(addr, addr + len);
	}
	return addr;
}

EXPORT_SYMBOL(do_brk);

/* Release all mmaps. */
void exit_mmap(struct mm_struct *mm)
{
	struct mmu_gather *tlb;
	struct vm_area_struct *vma;
	unsigned long nr_accounted = 0;

	profile_exit_mmap(mm);
 
	lru_add_drain();

	spin_lock(&mm->page_table_lock);

	tlb = tlb_gather_mmu(mm, 1);
	flush_cache_mm(mm);
	/* Use ~0UL here to ensure all VMAs in the mm are unmapped */
	mm->map_count -= unmap_vmas(&tlb, mm, mm->mmap, 0,
					~0UL, &nr_accounted, NULL);
	vm_unacct_memory(nr_accounted);
	BUG_ON(mm->map_count);	/* This is just debugging */
	clear_page_tables(tlb, FIRST_USER_PGD_NR, USER_PTRS_PER_PGD);
	tlb_finish_mmu(tlb, 0, MM_VM_SIZE(mm));

	vma = mm->mmap;
	mm->mmap = mm->mmap_cache = NULL;
	mm->mm_rb = RB_ROOT;
	mm->rss = 0;
	mm->total_vm = 0;
	mm->locked_vm = 0;

	spin_unlock(&mm->page_table_lock);

	/*
	 * Walk the list again, actually closing and freeing it
	 * without holding any MM locks.
	 */
	while (vma) {
		struct vm_area_struct *next = vma->vm_next;
		remove_vm_struct(vma);
		vma = next;
	}
}

/* Insert vm structure into process list sorted by address
 * and into the inode's i_mmap tree.  If vm_file is non-NULL
 * then i_mmap_lock is taken here.
 */
void insert_vm_struct(struct mm_struct * mm, struct vm_area_struct * vma)
{
	struct vm_area_struct * __vma, * prev;
	struct rb_node ** rb_link, * rb_parent;

	/*
	 * The vm_pgoff of a purely anonymous vma should be irrelevant
	 * until its first write fault, when page's anon_vma and index
	 * are set.  But now set the vm_pgoff it will almost certainly
	 * end up with (unless mremap moves it elsewhere before that
	 * first wfault), so /proc/pid/maps tells a consistent story.
	 *
	 * By setting it to reflect the virtual start address of the
	 * vma, merges and splits can happen in a seamless way, just
	 * using the existing file pgoff checks and manipulations.
	 * Similarly in do_mmap_pgoff and in do_brk.
	 */
	if (!vma->vm_file) {
		BUG_ON(vma->anon_vma);
		vma->vm_pgoff = vma->vm_start >> PAGE_SHIFT;
	}
	__vma = find_vma_prepare(mm,vma->vm_start,&prev,&rb_link,&rb_parent);
	if (__vma && __vma->vm_start < vma->vm_end)
		BUG();
	vma_link(mm, vma, prev, rb_link, rb_parent);
}

/*
 * Copy the vma structure to a new location in the same mm,
 * prior to moving page table entries, to effect an mremap move.
 */
struct vm_area_struct *copy_vma(struct vm_area_struct **vmap,
	unsigned long addr, unsigned long len, pgoff_t pgoff)
{
	struct vm_area_struct *vma = *vmap;
	unsigned long vma_start = vma->vm_start;
	struct mm_struct *mm = vma->vm_mm;
	struct vm_area_struct *new_vma, *prev;
	struct rb_node **rb_link, *rb_parent;
	struct mempolicy *pol;

	/*
	 * If anonymous vma has not yet been faulted, update new pgoff
	 * to match new location, to increase its chance of merging.
	 */
	if (!vma->vm_file && !vma->anon_vma)
		pgoff = addr >> PAGE_SHIFT;

	find_vma_prepare(mm, addr, &prev, &rb_link, &rb_parent);
	new_vma = vma_merge(mm, prev, addr, addr + len, vma->vm_flags,
			vma->anon_vma, vma->vm_file, pgoff, vma_policy(vma));
	if (new_vma) {
		/*
		 * Source vma may have been merged into new_vma
		 */
		if (vma_start >= new_vma->vm_start &&
		    vma_start < new_vma->vm_end)
			*vmap = new_vma;
	} else {
		new_vma = kmem_cache_alloc(vm_area_cachep, SLAB_KERNEL);
		if (new_vma) {
			*new_vma = *vma;
			vma_prio_tree_init(new_vma);
			pol = mpol_copy(vma_policy(vma));
			if (IS_ERR(pol)) {
				kmem_cache_free(vm_area_cachep, new_vma);
				return NULL;
			}
			vma_set_policy(new_vma, pol);
			new_vma->vm_start = addr;
			new_vma->vm_end = addr + len;
			new_vma->vm_pgoff = pgoff;
			if (new_vma->vm_file)
				get_file(new_vma->vm_file);
			if (new_vma->vm_ops && new_vma->vm_ops->open)
				new_vma->vm_ops->open(new_vma);
			vma_link(mm, new_vma, prev, rb_link, rb_parent);
		}
	}
	return new_vma;
}
