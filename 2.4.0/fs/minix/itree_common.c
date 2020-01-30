/* Generic part */

typedef struct {
	block_t	*p;
	block_t	key;
	struct buffer_head *bh;
} Indirect;

static inline void add_chain(Indirect *p, struct buffer_head *bh, block_t *v)
{
	p->key = *(p->p = v);
	p->bh = bh;
}

static inline int verify_chain(Indirect *from, Indirect *to)
{
	while (from <= to && from->key == *from->p)
		from++;
	return (from > to);
}

static inline block_t *block_end(struct buffer_head *bh)
{
	return (block_t *)((char*)bh->b_data + BLOCK_SIZE);
}

static inline Indirect *get_branch(struct inode *inode,
					int depth,
					int *offsets,
					Indirect chain[DEPTH],
					int *err)
{
	kdev_t dev = inode->i_dev;
	Indirect *p = chain;
	struct buffer_head *bh;

	*err = 0;
	/* i_data is not going away, no lock needed */
	add_chain (chain, NULL, i_data(inode) + *offsets);
	if (!p->key)
		goto no_block;
	while (--depth) {
		bh = bread(dev, block_to_cpu(p->key), BLOCK_SIZE);
		if (!bh)
			goto failure;
		/* Reader: pointers */
		if (!verify_chain(chain, p))
			goto changed;
		add_chain(++p, bh, (block_t *)bh->b_data + *++offsets);
		/* Reader: end */
		if (!p->key)
			goto no_block;
	}
	return NULL;

changed:
	*err = -EAGAIN;
	goto no_block;
failure:
	*err = -EIO;
no_block:
	return p;
}

static int alloc_branch(struct inode *inode,
			     int num,
			     int *offsets,
			     Indirect *branch)
{
	int n = 0;
	int i;
	int parent = minix_new_block(inode);

	branch[0].key = cpu_to_block(parent);
	if (parent) for (n = 1; n < num; n++) {
		struct buffer_head *bh;
		/* Allocate the next block */
		int nr = minix_new_block(inode);
		if (!nr)
			break;
		branch[n].key = cpu_to_block(nr);
		bh = getblk(inode->i_dev, parent, BLOCK_SIZE);
		if (!buffer_uptodate(bh))
			wait_on_buffer(bh);
		memset(bh->b_data, 0, BLOCK_SIZE);
		branch[n].bh = bh;
		branch[n].p = (block_t*) bh->b_data + offsets[n];
		*branch[n].p = branch[n].key;
		mark_buffer_uptodate(bh, 1);
		mark_buffer_dirty(bh);
		parent = nr;
	}
	if (n == num)
		return 0;

	/* Allocation failed, free what we already allocated */
	for (i = 1; i < n; i++)
		bforget(branch[i].bh);
	for (i = 0; i < n; i++)
		minix_free_block(inode, block_to_cpu(branch[i].key));
	return -ENOSPC;
}

static inline int splice_branch(struct inode *inode,
				     Indirect chain[DEPTH],
				     Indirect *where,
				     int num)
{
	int i;

	/* Verify that place we are splicing to is still there and vacant */

	/* Writer: pointers */
	if (!verify_chain(chain, where-1) || *where->p)
		/* Writer: end */
		goto changed;

	/* That's it */

	*where->p = where->key;

	/* Writer: end */

	/* We are done with atomic stuff, now do the rest of housekeeping */

	inode->i_ctime = CURRENT_TIME;

	/* had we spliced it onto indirect block? */
	if (where->bh)
		mark_buffer_dirty(where->bh);

	mark_inode_dirty(inode);
	return 0;

changed:
	for (i = 1; i < num; i++)
		bforget(where[i].bh);
	for (i = 0; i < num; i++)
		minix_free_block(inode, block_to_cpu(where[i].key));
	return -EAGAIN;
}

static inline int get_block(struct inode * inode, long block,
			struct buffer_head *bh_result, int create)
{
	int err = -EIO;
	int offsets[DEPTH];
	Indirect chain[DEPTH];
	Indirect *partial;
	int left;
	int depth = block_to_path(inode, block, offsets);

	if (depth == 0)
		goto out;

	lock_kernel();
reread:
	partial = get_branch(inode, depth, offsets, chain, &err);

	/* Simplest case - block found, no allocation needed */
	if (!partial) {
got_it:
		bh_result->b_dev = inode->i_dev;
		bh_result->b_blocknr = block_to_cpu(chain[depth-1].key);
		bh_result->b_state |= (1UL << BH_Mapped);
		/* Clean up and exit */
		partial = chain+depth-1; /* the whole chain */
		goto cleanup;
	}

	/* Next simple case - plain lookup or failed read of indirect block */
	if (!create || err == -EIO) {
cleanup:
		while (partial > chain) {
			brelse(partial->bh);
			partial--;
		}
		unlock_kernel();
out:
		return err;
	}

	/*
	 * Indirect block might be removed by truncate while we were
	 * reading it. Handling of that case (forget what we've got and
	 * reread) is taken out of the main path.
	 */
	if (err == -EAGAIN)
		goto changed;

	left = (chain + depth) - partial;
	err = alloc_branch(inode, left, offsets+(partial-chain), partial);
	if (err)
		goto cleanup;

	if (splice_branch(inode, chain, partial, left) < 0)
		goto changed;

	bh_result->b_state |= (1UL << BH_New);
	goto got_it;

changed:
	while (partial > chain) {
		bforget(partial->bh);
		partial--;
	}
	goto reread;
}

static inline int all_zeroes(block_t *p, block_t *q)
{
	while (p < q)
		if (*p++)
			return 0;
	return 1;
}

static Indirect *find_shared(struct inode *inode,
				int depth,
				int offsets[DEPTH],
				Indirect chain[DEPTH],
				block_t *top)
{
	Indirect *partial, *p;
	int k, err;

	*top = 0;
	for (k = depth; k > 1 && !offsets[k-1]; k--)
		;
	partial = get_branch(inode, k, offsets, chain, &err);
	/* Writer: pointers */
	if (!partial)
		partial = chain + k-1;
	if (!partial->key && *partial->p)
		/* Writer: end */
		goto no_top;
	for (p=partial;p>chain && all_zeroes((block_t*)p->bh->b_data,p->p);p--)
		;
	if (p == chain + k - 1 && p > chain) {
		p->p--;
	} else {
		*top = *p->p;
		*p->p = 0;
	}
	/* Writer: end */

	while(partial > p)
	{
		brelse(partial->bh);
		partial--;
	}
no_top:
	return partial;
}

static inline void free_data(struct inode *inode, block_t *p, block_t *q)
{
	unsigned long nr;

	for ( ; p < q ; p++) {
		nr = block_to_cpu(*p);
		if (nr) {
			*p = 0;
			minix_free_block(inode, nr);
		}
	}
}

static void free_branches(struct inode *inode, block_t *p, block_t *q, int depth)
{
	struct buffer_head * bh;
	unsigned long nr;

	if (depth--) {
		for ( ; p < q ; p++) {
			nr = block_to_cpu(*p);
			if (!nr)
				continue;
			*p = 0;
			bh = bread (inode->i_dev, nr, BLOCK_SIZE);
			if (!bh)
				continue;
			free_branches(inode, (block_t*)bh->b_data,
				      block_end(bh), depth);
			bforget(bh);
			minix_free_block(inode, nr);
			mark_inode_dirty(inode);
		}
	} else
		free_data(inode, p, q);
}

static inline void truncate (struct inode * inode)
{
	block_t *idata = i_data(inode);
	int offsets[DEPTH];
	Indirect chain[DEPTH];
	Indirect *partial;
	block_t nr = 0;
	int n;
	int first_whole;
	long iblock;

	iblock = (inode->i_size + BLOCK_SIZE-1) >> 10;
	block_truncate_page(inode->i_mapping, inode->i_size, get_block);

	n = block_to_path(inode, iblock, offsets);
	if (!n)
		return;

	if (n == 1) {
		free_data(inode, idata+offsets[0], idata + DIRECT);
		first_whole = 0;
		goto do_indirects;
	}

	first_whole = offsets[0] + 1 - DIRECT;
	partial = find_shared(inode, n, offsets, chain, &nr);
	if (nr) {
		if (partial == chain)
			mark_inode_dirty(inode);
		else
			mark_buffer_dirty(partial->bh);
		free_branches(inode, &nr, &nr+1, (chain+n-1) - partial);
	}
	/* Clear the ends of indirect blocks on the shared branch */
	while (partial > chain) {
		free_branches(inode, partial->p + 1, block_end(partial->bh),
				(chain+n-1) - partial);
		mark_buffer_dirty(partial->bh);
		brelse (partial->bh);
		partial--;
	}
do_indirects:
	/* Kill the remaining (whole) subtrees */
	while (first_whole < DEPTH-1) {
		nr = idata[DIRECT+first_whole];
		if (nr) {
			idata[DIRECT+first_whole] = 0;
			mark_inode_dirty(inode);
			free_branches(inode, &nr, &nr+1, first_whole+1);
		}
		first_whole++;
	}
	inode->i_mtime = inode->i_ctime = CURRENT_TIME;
	mark_inode_dirty(inode);
}

static int sync_block (struct inode * inode, block_t block, int wait)
{
	struct buffer_head * bh;
	
	if (!block)
		return 0;
	bh = get_hash_table(inode->i_dev, block_to_cpu(block), BLOCK_SIZE);
	if (!bh)
		return 0;
	if (wait && buffer_req(bh) && !buffer_uptodate(bh)) {
		brelse(bh);
		return -1;
	}
	if (wait || !buffer_uptodate(bh) || !buffer_dirty(bh))
	{
		brelse(bh);
		return 0;
	}
	ll_rw_block(WRITE, 1, &bh);
	atomic_dec(&bh->b_count);
	return 0;
}

static int sync_indirect(struct inode *inode, block_t iblock, int depth,
			 int wait)
{
	struct buffer_head * ind_bh = NULL;
	int rc, err = 0;

	if (!iblock)
		return 0;

	rc = sync_block (inode, iblock, wait);
	if (rc)
		return rc;

	ind_bh = bread(inode->i_dev, block_to_cpu(iblock), BLOCK_SIZE);
	if (!ind_bh)
		return -1;

	if (--depth) {
		block_t *p = (block_t*)ind_bh->b_data;
		block_t *end = block_end(ind_bh);
		while (p < end) {
			rc = sync_indirect (inode, *p++, depth, wait);
			if (rc > 0)
				break;
			if (rc)
				err = rc;
		}
	}
	brelse(ind_bh);
	return err;
}

static inline int sync_file(struct inode * inode)
{
	int wait, err = 0, i;
	block_t *idata = i_data(inode);
	
	lock_kernel();
	err = generic_buffer_fdatasync(inode, 0, ~0UL);
	for (wait=0; wait<=1; wait++)
		for (i=1; i<DEPTH; i++)
			err |= sync_indirect(inode, idata[DIRECT+i-1], i, wait);
	err |= minix_sync_inode (inode);
	unlock_kernel();
	return (err < 0) ? -EIO : 0;
}
