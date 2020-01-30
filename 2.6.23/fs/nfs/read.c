/*
 * linux/fs/nfs/read.c
 *
 * Block I/O for NFS
 *
 * Partial copy of Linus' read cache modifications to fs/nfs/file.c
 * modified for async RPC by okir@monad.swb.de
 */

#include <linux/time.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/sunrpc/clnt.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_page.h>
#include <linux/smp_lock.h>

#include <asm/system.h>

#include "internal.h"
#include "iostat.h"

#define NFSDBG_FACILITY		NFSDBG_PAGECACHE

static int nfs_pagein_multi(struct inode *, struct list_head *, unsigned int, size_t, int);
static int nfs_pagein_one(struct inode *, struct list_head *, unsigned int, size_t, int);
static const struct rpc_call_ops nfs_read_partial_ops;
static const struct rpc_call_ops nfs_read_full_ops;

static struct kmem_cache *nfs_rdata_cachep;
static mempool_t *nfs_rdata_mempool;

#define MIN_POOL_READ	(32)

struct nfs_read_data *nfs_readdata_alloc(unsigned int pagecount)
{
	struct nfs_read_data *p = mempool_alloc(nfs_rdata_mempool, GFP_NOFS);

	if (p) {
		memset(p, 0, sizeof(*p));
		INIT_LIST_HEAD(&p->pages);
		p->npages = pagecount;
		if (pagecount <= ARRAY_SIZE(p->page_array))
			p->pagevec = p->page_array;
		else {
			p->pagevec = kcalloc(pagecount, sizeof(struct page *), GFP_NOFS);
			if (!p->pagevec) {
				mempool_free(p, nfs_rdata_mempool);
				p = NULL;
			}
		}
	}
	return p;
}

static void nfs_readdata_rcu_free(struct rcu_head *head)
{
	struct nfs_read_data *p = container_of(head, struct nfs_read_data, task.u.tk_rcu);
	if (p && (p->pagevec != &p->page_array[0]))
		kfree(p->pagevec);
	mempool_free(p, nfs_rdata_mempool);
}

static void nfs_readdata_free(struct nfs_read_data *rdata)
{
	call_rcu_bh(&rdata->task.u.tk_rcu, nfs_readdata_rcu_free);
}

void nfs_readdata_release(void *data)
{
        nfs_readdata_free(data);
}

static
int nfs_return_empty_page(struct page *page)
{
	zero_user_page(page, 0, PAGE_CACHE_SIZE, KM_USER0);
	SetPageUptodate(page);
	unlock_page(page);
	return 0;
}

static void nfs_readpage_truncate_uninitialised_page(struct nfs_read_data *data)
{
	unsigned int remainder = data->args.count - data->res.count;
	unsigned int base = data->args.pgbase + data->res.count;
	unsigned int pglen;
	struct page **pages;

	if (data->res.eof == 0 || remainder == 0)
		return;
	/*
	 * Note: "remainder" can never be negative, since we check for
	 * 	this in the XDR code.
	 */
	pages = &data->args.pages[base >> PAGE_CACHE_SHIFT];
	base &= ~PAGE_CACHE_MASK;
	pglen = PAGE_CACHE_SIZE - base;
	for (;;) {
		if (remainder <= pglen) {
			zero_user_page(*pages, base, remainder, KM_USER0);
			break;
		}
		zero_user_page(*pages, base, pglen, KM_USER0);
		pages++;
		remainder -= pglen;
		pglen = PAGE_CACHE_SIZE;
		base = 0;
	}
}

static int nfs_readpage_async(struct nfs_open_context *ctx, struct inode *inode,
		struct page *page)
{
	LIST_HEAD(one_request);
	struct nfs_page	*new;
	unsigned int len;

	len = nfs_page_length(page);
	if (len == 0)
		return nfs_return_empty_page(page);
	new = nfs_create_request(ctx, inode, page, 0, len);
	if (IS_ERR(new)) {
		unlock_page(page);
		return PTR_ERR(new);
	}
	if (len < PAGE_CACHE_SIZE)
		zero_user_page(page, len, PAGE_CACHE_SIZE - len, KM_USER0);

	nfs_list_add_request(new, &one_request);
	if (NFS_SERVER(inode)->rsize < PAGE_CACHE_SIZE)
		nfs_pagein_multi(inode, &one_request, 1, len, 0);
	else
		nfs_pagein_one(inode, &one_request, 1, len, 0);
	return 0;
}

static void nfs_readpage_release(struct nfs_page *req)
{
	unlock_page(req->wb_page);

	dprintk("NFS: read done (%s/%Ld %d@%Ld)\n",
			req->wb_context->path.dentry->d_inode->i_sb->s_id,
			(long long)NFS_FILEID(req->wb_context->path.dentry->d_inode),
			req->wb_bytes,
			(long long)req_offset(req));
	nfs_clear_request(req);
	nfs_release_request(req);
}

/*
 * Set up the NFS read request struct
 */
static void nfs_read_rpcsetup(struct nfs_page *req, struct nfs_read_data *data,
		const struct rpc_call_ops *call_ops,
		unsigned int count, unsigned int offset)
{
	struct inode		*inode;
	int flags;

	data->req	  = req;
	data->inode	  = inode = req->wb_context->path.dentry->d_inode;
	data->cred	  = req->wb_context->cred;

	data->args.fh     = NFS_FH(inode);
	data->args.offset = req_offset(req) + offset;
	data->args.pgbase = req->wb_pgbase + offset;
	data->args.pages  = data->pagevec;
	data->args.count  = count;
	data->args.context = req->wb_context;

	data->res.fattr   = &data->fattr;
	data->res.count   = count;
	data->res.eof     = 0;
	nfs_fattr_init(&data->fattr);

	/* Set up the initial task struct. */
	flags = RPC_TASK_ASYNC | (IS_SWAPFILE(inode)? NFS_RPC_SWAPFLAGS : 0);
	rpc_init_task(&data->task, NFS_CLIENT(inode), flags, call_ops, data);
	NFS_PROTO(inode)->read_setup(data);

	data->task.tk_cookie = (unsigned long)inode;

	dprintk("NFS: %5u initiated read call (req %s/%Ld, %u bytes @ offset %Lu)\n",
			data->task.tk_pid,
			inode->i_sb->s_id,
			(long long)NFS_FILEID(inode),
			count,
			(unsigned long long)data->args.offset);
}

static void
nfs_async_read_error(struct list_head *head)
{
	struct nfs_page	*req;

	while (!list_empty(head)) {
		req = nfs_list_entry(head->next);
		nfs_list_remove_request(req);
		SetPageError(req->wb_page);
		nfs_readpage_release(req);
	}
}

/*
 * Start an async read operation
 */
static void nfs_execute_read(struct nfs_read_data *data)
{
	struct rpc_clnt *clnt = NFS_CLIENT(data->inode);
	sigset_t oldset;

	rpc_clnt_sigmask(clnt, &oldset);
	rpc_execute(&data->task);
	rpc_clnt_sigunmask(clnt, &oldset);
}

/*
 * Generate multiple requests to fill a single page.
 *
 * We optimize to reduce the number of read operations on the wire.  If we
 * detect that we're reading a page, or an area of a page, that is past the
 * end of file, we do not generate NFS read operations but just clear the
 * parts of the page that would have come back zero from the server anyway.
 *
 * We rely on the cached value of i_size to make this determination; another
 * client can fill pages on the server past our cached end-of-file, but we
 * won't see the new data until our attribute cache is updated.  This is more
 * or less conventional NFS client behavior.
 */
static int nfs_pagein_multi(struct inode *inode, struct list_head *head, unsigned int npages, size_t count, int flags)
{
	struct nfs_page *req = nfs_list_entry(head->next);
	struct page *page = req->wb_page;
	struct nfs_read_data *data;
	size_t rsize = NFS_SERVER(inode)->rsize, nbytes;
	unsigned int offset;
	int requests = 0;
	LIST_HEAD(list);

	nfs_list_remove_request(req);

	nbytes = count;
	do {
		size_t len = min(nbytes,rsize);

		data = nfs_readdata_alloc(1);
		if (!data)
			goto out_bad;
		INIT_LIST_HEAD(&data->pages);
		list_add(&data->pages, &list);
		requests++;
		nbytes -= len;
	} while(nbytes != 0);
	atomic_set(&req->wb_complete, requests);

	ClearPageError(page);
	offset = 0;
	nbytes = count;
	do {
		data = list_entry(list.next, struct nfs_read_data, pages);
		list_del_init(&data->pages);

		data->pagevec[0] = page;

		if (nbytes < rsize)
			rsize = nbytes;
		nfs_read_rpcsetup(req, data, &nfs_read_partial_ops,
				  rsize, offset);
		offset += rsize;
		nbytes -= rsize;
		nfs_execute_read(data);
	} while (nbytes != 0);

	return 0;

out_bad:
	while (!list_empty(&list)) {
		data = list_entry(list.next, struct nfs_read_data, pages);
		list_del(&data->pages);
		nfs_readdata_free(data);
	}
	SetPageError(page);
	nfs_readpage_release(req);
	return -ENOMEM;
}

static int nfs_pagein_one(struct inode *inode, struct list_head *head, unsigned int npages, size_t count, int flags)
{
	struct nfs_page		*req;
	struct page		**pages;
	struct nfs_read_data	*data;

	data = nfs_readdata_alloc(npages);
	if (!data)
		goto out_bad;

	INIT_LIST_HEAD(&data->pages);
	pages = data->pagevec;
	while (!list_empty(head)) {
		req = nfs_list_entry(head->next);
		nfs_list_remove_request(req);
		nfs_list_add_request(req, &data->pages);
		ClearPageError(req->wb_page);
		*pages++ = req->wb_page;
	}
	req = nfs_list_entry(data->pages.next);

	nfs_read_rpcsetup(req, data, &nfs_read_full_ops, count, 0);

	nfs_execute_read(data);
	return 0;
out_bad:
	nfs_async_read_error(head);
	return -ENOMEM;
}

/*
 * This is the callback from RPC telling us whether a reply was
 * received or some error occurred (timeout or socket shutdown).
 */
int nfs_readpage_result(struct rpc_task *task, struct nfs_read_data *data)
{
	int status;

	dprintk("NFS: %s: %5u, (status %d)\n", __FUNCTION__, task->tk_pid,
			task->tk_status);

	status = NFS_PROTO(data->inode)->read_done(task, data);
	if (status != 0)
		return status;

	nfs_add_stats(data->inode, NFSIOS_SERVERREADBYTES, data->res.count);

	if (task->tk_status == -ESTALE) {
		set_bit(NFS_INO_STALE, &NFS_FLAGS(data->inode));
		nfs_mark_for_revalidate(data->inode);
	}
	spin_lock(&data->inode->i_lock);
	NFS_I(data->inode)->cache_validity |= NFS_INO_INVALID_ATIME;
	spin_unlock(&data->inode->i_lock);
	return 0;
}

static int nfs_readpage_retry(struct rpc_task *task, struct nfs_read_data *data)
{
	struct nfs_readargs *argp = &data->args;
	struct nfs_readres *resp = &data->res;

	if (resp->eof || resp->count == argp->count)
		return 0;

	/* This is a short read! */
	nfs_inc_stats(data->inode, NFSIOS_SHORTREAD);
	/* Has the server at least made some progress? */
	if (resp->count == 0)
		return 0;

	/* Yes, so retry the read at the end of the data */
	argp->offset += resp->count;
	argp->pgbase += resp->count;
	argp->count -= resp->count;
	rpc_restart_call(task);
	return -EAGAIN;
}

/*
 * Handle a read reply that fills part of a page.
 */
static void nfs_readpage_result_partial(struct rpc_task *task, void *calldata)
{
	struct nfs_read_data *data = calldata;
	struct nfs_page *req = data->req;
	struct page *page = req->wb_page;
 
	if (nfs_readpage_result(task, data) != 0)
		return;

	if (likely(task->tk_status >= 0)) {
		nfs_readpage_truncate_uninitialised_page(data);
		if (nfs_readpage_retry(task, data) != 0)
			return;
	}
	if (unlikely(task->tk_status < 0))
		SetPageError(page);
	if (atomic_dec_and_test(&req->wb_complete)) {
		if (!PageError(page))
			SetPageUptodate(page);
		nfs_readpage_release(req);
	}
}

static const struct rpc_call_ops nfs_read_partial_ops = {
	.rpc_call_done = nfs_readpage_result_partial,
	.rpc_release = nfs_readdata_release,
};

static void nfs_readpage_set_pages_uptodate(struct nfs_read_data *data)
{
	unsigned int count = data->res.count;
	unsigned int base = data->args.pgbase;
	struct page **pages;

	if (data->res.eof)
		count = data->args.count;
	if (unlikely(count == 0))
		return;
	pages = &data->args.pages[base >> PAGE_CACHE_SHIFT];
	base &= ~PAGE_CACHE_MASK;
	count += base;
	for (;count >= PAGE_CACHE_SIZE; count -= PAGE_CACHE_SIZE, pages++)
		SetPageUptodate(*pages);
	if (count == 0)
		return;
	/* Was this a short read? */
	if (data->res.eof || data->res.count == data->args.count)
		SetPageUptodate(*pages);
}

/*
 * This is the callback from RPC telling us whether a reply was
 * received or some error occurred (timeout or socket shutdown).
 */
static void nfs_readpage_result_full(struct rpc_task *task, void *calldata)
{
	struct nfs_read_data *data = calldata;

	if (nfs_readpage_result(task, data) != 0)
		return;
	/*
	 * Note: nfs_readpage_retry may change the values of
	 * data->args. In the multi-page case, we therefore need
	 * to ensure that we call nfs_readpage_set_pages_uptodate()
	 * first.
	 */
	if (likely(task->tk_status >= 0)) {
		nfs_readpage_truncate_uninitialised_page(data);
		nfs_readpage_set_pages_uptodate(data);
		if (nfs_readpage_retry(task, data) != 0)
			return;
	}
	while (!list_empty(&data->pages)) {
		struct nfs_page *req = nfs_list_entry(data->pages.next);

		nfs_list_remove_request(req);
		nfs_readpage_release(req);
	}
}

static const struct rpc_call_ops nfs_read_full_ops = {
	.rpc_call_done = nfs_readpage_result_full,
	.rpc_release = nfs_readdata_release,
};

/*
 * Read a page over NFS.
 * We read the page synchronously in the following case:
 *  -	The error flag is set for this page. This happens only when a
 *	previous async read operation failed.
 */
int nfs_readpage(struct file *file, struct page *page)
{
	struct nfs_open_context *ctx;
	struct inode *inode = page->mapping->host;
	int		error;

	dprintk("NFS: nfs_readpage (%p %ld@%lu)\n",
		page, PAGE_CACHE_SIZE, page->index);
	nfs_inc_stats(inode, NFSIOS_VFSREADPAGE);
	nfs_add_stats(inode, NFSIOS_READPAGES, 1);

	/*
	 * Try to flush any pending writes to the file..
	 *
	 * NOTE! Because we own the page lock, there cannot
	 * be any new pending writes generated at this point
	 * for this page (other pages can be written to).
	 */
	error = nfs_wb_page(inode, page);
	if (error)
		goto out_unlock;
	if (PageUptodate(page))
		goto out_unlock;

	error = -ESTALE;
	if (NFS_STALE(inode))
		goto out_unlock;

	if (file == NULL) {
		error = -EBADF;
		ctx = nfs_find_open_context(inode, NULL, FMODE_READ);
		if (ctx == NULL)
			goto out_unlock;
	} else
		ctx = get_nfs_open_context((struct nfs_open_context *)
				file->private_data);

	error = nfs_readpage_async(ctx, inode, page);

	put_nfs_open_context(ctx);
	return error;
out_unlock:
	unlock_page(page);
	return error;
}

struct nfs_readdesc {
	struct nfs_pageio_descriptor *pgio;
	struct nfs_open_context *ctx;
};

static int
readpage_async_filler(void *data, struct page *page)
{
	struct nfs_readdesc *desc = (struct nfs_readdesc *)data;
	struct inode *inode = page->mapping->host;
	struct nfs_page *new;
	unsigned int len;
	int error;

	error = nfs_wb_page(inode, page);
	if (error)
		goto out_unlock;
	if (PageUptodate(page))
		goto out_unlock;

	len = nfs_page_length(page);
	if (len == 0)
		return nfs_return_empty_page(page);

	new = nfs_create_request(desc->ctx, inode, page, 0, len);
	if (IS_ERR(new))
		goto out_error;

	if (len < PAGE_CACHE_SIZE)
		zero_user_page(page, len, PAGE_CACHE_SIZE - len, KM_USER0);
	nfs_pageio_add_request(desc->pgio, new);
	return 0;
out_error:
	error = PTR_ERR(new);
	SetPageError(page);
out_unlock:
	unlock_page(page);
	return error;
}

int nfs_readpages(struct file *filp, struct address_space *mapping,
		struct list_head *pages, unsigned nr_pages)
{
	struct nfs_pageio_descriptor pgio;
	struct nfs_readdesc desc = {
		.pgio = &pgio,
	};
	struct inode *inode = mapping->host;
	struct nfs_server *server = NFS_SERVER(inode);
	size_t rsize = server->rsize;
	unsigned long npages;
	int ret = -ESTALE;

	dprintk("NFS: nfs_readpages (%s/%Ld %d)\n",
			inode->i_sb->s_id,
			(long long)NFS_FILEID(inode),
			nr_pages);
	nfs_inc_stats(inode, NFSIOS_VFSREADPAGES);

	if (NFS_STALE(inode))
		goto out;

	if (filp == NULL) {
		desc.ctx = nfs_find_open_context(inode, NULL, FMODE_READ);
		if (desc.ctx == NULL)
			return -EBADF;
	} else
		desc.ctx = get_nfs_open_context((struct nfs_open_context *)
				filp->private_data);
	if (rsize < PAGE_CACHE_SIZE)
		nfs_pageio_init(&pgio, inode, nfs_pagein_multi, rsize, 0);
	else
		nfs_pageio_init(&pgio, inode, nfs_pagein_one, rsize, 0);

	ret = read_cache_pages(mapping, pages, readpage_async_filler, &desc);

	nfs_pageio_complete(&pgio);
	npages = (pgio.pg_bytes_written + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;
	nfs_add_stats(inode, NFSIOS_READPAGES, npages);
	put_nfs_open_context(desc.ctx);
out:
	return ret;
}

int __init nfs_init_readpagecache(void)
{
	nfs_rdata_cachep = kmem_cache_create("nfs_read_data",
					     sizeof(struct nfs_read_data),
					     0, SLAB_HWCACHE_ALIGN,
					     NULL);
	if (nfs_rdata_cachep == NULL)
		return -ENOMEM;

	nfs_rdata_mempool = mempool_create_slab_pool(MIN_POOL_READ,
						     nfs_rdata_cachep);
	if (nfs_rdata_mempool == NULL)
		return -ENOMEM;

	return 0;
}

void nfs_destroy_readpagecache(void)
{
	mempool_destroy(nfs_rdata_mempool);
	kmem_cache_destroy(nfs_rdata_cachep);
}
