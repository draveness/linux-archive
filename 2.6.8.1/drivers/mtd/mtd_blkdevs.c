/*
 * $Id: mtd_blkdevs.c,v 1.22 2004/07/12 12:35:28 dwmw2 Exp $
 *
 * (C) 2003 David Woodhouse <dwmw2@infradead.org>
 *
 * Interface to Linux 2.5 block layer for MTD 'translation layers'.
 *
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/fs.h>
#include <linux/mtd/blktrans.h>
#include <linux/mtd/mtd.h>
#include <linux/blkdev.h>
#include <linux/blkpg.h>
#include <linux/spinlock.h>
#include <linux/hdreg.h>
#include <linux/init.h>
#include <asm/semaphore.h>
#include <asm/uaccess.h>
#include <linux/devfs_fs_kernel.h>

static LIST_HEAD(blktrans_majors);

extern struct semaphore mtd_table_mutex;
extern struct mtd_info *mtd_table[];

struct mtd_blkcore_priv {
	struct completion thread_dead;
	int exiting;
	wait_queue_head_t thread_wq;
	struct request_queue *rq;
	spinlock_t queue_lock;
};

static int do_blktrans_request(struct mtd_blktrans_ops *tr,
			       struct mtd_blktrans_dev *dev,
			       struct request *req)
{
	unsigned long block, nsect;
	char *buf;

	block = req->sector;
	nsect = req->current_nr_sectors;
	buf = req->buffer;

	if (!(req->flags & REQ_CMD))
		return 0;

	if (block + nsect > get_capacity(req->rq_disk))
		return 0;

	switch(rq_data_dir(req)) {
	case READ:
		for (; nsect > 0; nsect--, block++, buf += 512)
			if (tr->readsect(dev, block, buf))
				return 0;
		return 1;

	case WRITE:
		if (!tr->writesect)
			return 0;

		for (; nsect > 0; nsect--, block++, buf += 512)
			if (tr->writesect(dev, block, buf))
				return 0;
		return 1;

	default:
		printk(KERN_NOTICE "Unknown request %ld\n", rq_data_dir(req));
		return 0;
	}
}

static int mtd_blktrans_thread(void *arg)
{
	struct mtd_blktrans_ops *tr = arg;
	struct request_queue *rq = tr->blkcore_priv->rq;

	/* we might get involved when memory gets low, so use PF_MEMALLOC */
	current->flags |= PF_MEMALLOC;

	daemonize("%sd", tr->name);

	/* daemonize() doesn't do this for us since some kernel threads
	   actually want to deal with signals. We can't just call 
	   exit_sighand() since that'll cause an oops when we finally
	   do exit. */
	spin_lock_irq(&current->sighand->siglock);
	sigfillset(&current->blocked);
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);

	spin_lock_irq(rq->queue_lock);
		
	while (!tr->blkcore_priv->exiting) {
		struct request *req;
		struct mtd_blktrans_dev *dev;
		int res = 0;
		DECLARE_WAITQUEUE(wait, current);

		req = elv_next_request(rq);

		if (!req) {
			add_wait_queue(&tr->blkcore_priv->thread_wq, &wait);
			set_current_state(TASK_INTERRUPTIBLE);

			spin_unlock_irq(rq->queue_lock);

			schedule();
			remove_wait_queue(&tr->blkcore_priv->thread_wq, &wait);

			spin_lock_irq(rq->queue_lock);

			continue;
		}

		dev = req->rq_disk->private_data;
		tr = dev->tr;

		spin_unlock_irq(rq->queue_lock);

		down(&dev->sem);
		res = do_blktrans_request(tr, dev, req);
		up(&dev->sem);

		spin_lock_irq(rq->queue_lock);

		end_request(req, res);
	}
	spin_unlock_irq(rq->queue_lock);

	complete_and_exit(&tr->blkcore_priv->thread_dead, 0);
}

static void mtd_blktrans_request(struct request_queue *rq)
{
	struct mtd_blktrans_ops *tr = rq->queuedata;
	wake_up(&tr->blkcore_priv->thread_wq);
}


int blktrans_open(struct inode *i, struct file *f)
{
	struct mtd_blktrans_dev *dev;
	struct mtd_blktrans_ops *tr;
	int ret = -ENODEV;

	dev = i->i_bdev->bd_disk->private_data;
	tr = dev->tr;

	if (!try_module_get(dev->mtd->owner))
		goto out;

	if (!try_module_get(tr->owner))
		goto out_tr;

	/* FIXME: Locking. A hot pluggable device can go away 
	   (del_mtd_device can be called for it) without its module
	   being unloaded. */
	dev->mtd->usecount++;

	ret = 0;
	if (tr->open && (ret = tr->open(dev))) {
		dev->mtd->usecount--;
		module_put(dev->mtd->owner);
	out_tr:
		module_put(tr->owner);
	}
 out:
	return ret;
}

int blktrans_release(struct inode *i, struct file *f)
{
	struct mtd_blktrans_dev *dev;
	struct mtd_blktrans_ops *tr;
	int ret = 0;

	dev = i->i_bdev->bd_disk->private_data;
	tr = dev->tr;

	if (tr->release)
		ret = tr->release(dev);

	if (!ret) {
		dev->mtd->usecount--;
		module_put(dev->mtd->owner);
		module_put(tr->owner);
	}

	return ret;
}


static int blktrans_ioctl(struct inode *inode, struct file *file, 
			      unsigned int cmd, unsigned long arg)
{
	struct mtd_blktrans_dev *dev = inode->i_bdev->bd_disk->private_data;
	struct mtd_blktrans_ops *tr = dev->tr;

	switch (cmd) {
	case BLKFLSBUF:
		if (tr->flush)
			return tr->flush(dev);
		/* The core code did the work, we had nothing to do. */
		return 0;

	case HDIO_GETGEO:
		if (tr->getgeo) {
			struct hd_geometry g;
			int ret;

			memset(&g, 0, sizeof(g));
			ret = tr->getgeo(dev, &g);
			if (ret)
				return ret;

			g.start = get_start_sect(inode->i_bdev);
			if (copy_to_user((void __user *)arg, &g, sizeof(g)))
				return -EFAULT;
			return 0;
		} /* else */
	default:
		return -ENOTTY;
	}
}

struct block_device_operations mtd_blktrans_ops = {
	.owner		= THIS_MODULE,
	.open		= blktrans_open,
	.release	= blktrans_release,
	.ioctl		= blktrans_ioctl,
};

int add_mtd_blktrans_dev(struct mtd_blktrans_dev *new)
{
	struct mtd_blktrans_ops *tr = new->tr;
	struct list_head *this;
	int last_devnum = -1;
	struct gendisk *gd;

	if (!down_trylock(&mtd_table_mutex)) {
		up(&mtd_table_mutex);
		BUG();
	}

	list_for_each(this, &tr->devs) {
		struct mtd_blktrans_dev *d = list_entry(this, struct mtd_blktrans_dev, list);
		if (new->devnum == -1) {
			/* Use first free number */
			if (d->devnum != last_devnum+1) {
				/* Found a free devnum. Plug it in here */
				new->devnum = last_devnum+1;
				list_add_tail(&new->list, &d->list);
				goto added;
			}
		} else if (d->devnum == new->devnum) {
			/* Required number taken */
			return -EBUSY;
		} else if (d->devnum > new->devnum) {
			/* Required number was free */
			list_add_tail(&new->list, &d->list);
			goto added;
		} 
		last_devnum = d->devnum;
	}
	if (new->devnum == -1)
		new->devnum = last_devnum+1;

	if ((new->devnum << tr->part_bits) > 256) {
		return -EBUSY;
	}

	init_MUTEX(&new->sem);
	list_add_tail(&new->list, &tr->devs);
 added:
	if (!tr->writesect)
		new->readonly = 1;

	gd = alloc_disk(1 << tr->part_bits);
	if (!gd) {
		list_del(&new->list);
		return -ENOMEM;
	}
	gd->major = tr->major;
	gd->first_minor = (new->devnum) << tr->part_bits;
	gd->fops = &mtd_blktrans_ops;
	
	snprintf(gd->disk_name, sizeof(gd->disk_name),
		 "%s%c", tr->name, (tr->part_bits?'a':'0') + new->devnum);
	snprintf(gd->devfs_name, sizeof(gd->devfs_name),
		 "%s/%c", tr->name, (tr->part_bits?'a':'0') + new->devnum);

	/* 2.5 has capacity in units of 512 bytes while still
	   having BLOCK_SIZE_BITS set to 10. Just to keep us amused. */
	set_capacity(gd, (new->size * new->blksize) >> 9);

	gd->private_data = new;
	new->blkcore_priv = gd;
	gd->queue = tr->blkcore_priv->rq;

	if (new->readonly)
		set_disk_ro(gd, 1);

	add_disk(gd);
	
	return 0;
}

int del_mtd_blktrans_dev(struct mtd_blktrans_dev *old)
{
	if (!down_trylock(&mtd_table_mutex)) {
		up(&mtd_table_mutex);
		BUG();
	}

	list_del(&old->list);

	del_gendisk(old->blkcore_priv);
	put_disk(old->blkcore_priv);
		
	return 0;
}

void blktrans_notify_remove(struct mtd_info *mtd)
{
	struct list_head *this, *this2, *next;

	list_for_each(this, &blktrans_majors) {
		struct mtd_blktrans_ops *tr = list_entry(this, struct mtd_blktrans_ops, list);

		list_for_each_safe(this2, next, &tr->devs) {
			struct mtd_blktrans_dev *dev = list_entry(this2, struct mtd_blktrans_dev, list);

			if (dev->mtd == mtd)
				tr->remove_dev(dev);
		}
	}
}

void blktrans_notify_add(struct mtd_info *mtd)
{
	struct list_head *this;

	if (mtd->type == MTD_ABSENT)
		return;

	list_for_each(this, &blktrans_majors) {
		struct mtd_blktrans_ops *tr = list_entry(this, struct mtd_blktrans_ops, list);

		tr->add_mtd(tr, mtd);
	}

}

static struct mtd_notifier blktrans_notifier = {
	.add = blktrans_notify_add,
	.remove = blktrans_notify_remove,
};
      
int register_mtd_blktrans(struct mtd_blktrans_ops *tr)
{
	int ret, i;

	/* Register the notifier if/when the first device type is 
	   registered, to prevent the link/init ordering from fucking
	   us over. */
	if (!blktrans_notifier.list.next)
		register_mtd_user(&blktrans_notifier);

	tr->blkcore_priv = kmalloc(sizeof(*tr->blkcore_priv), GFP_KERNEL);
	if (!tr->blkcore_priv)
		return -ENOMEM;

	memset(tr->blkcore_priv, 0, sizeof(*tr->blkcore_priv));

	down(&mtd_table_mutex);

	ret = register_blkdev(tr->major, tr->name);
	if (ret) {
		printk(KERN_WARNING "Unable to register %s block device on major %d: %d\n",
		       tr->name, tr->major, ret);
		kfree(tr->blkcore_priv);
		up(&mtd_table_mutex);
		return ret;
	}
	spin_lock_init(&tr->blkcore_priv->queue_lock);
	init_completion(&tr->blkcore_priv->thread_dead);
	init_waitqueue_head(&tr->blkcore_priv->thread_wq);

	tr->blkcore_priv->rq = blk_init_queue(mtd_blktrans_request, &tr->blkcore_priv->queue_lock);
	if (!tr->blkcore_priv->rq) {
		unregister_blkdev(tr->major, tr->name);
		kfree(tr->blkcore_priv);
		up(&mtd_table_mutex);
		return -ENOMEM;
	}

	tr->blkcore_priv->rq->queuedata = tr;

	ret = kernel_thread(mtd_blktrans_thread, tr, CLONE_KERNEL);
	if (ret < 0) {
		blk_cleanup_queue(tr->blkcore_priv->rq);
		unregister_blkdev(tr->major, tr->name);
		kfree(tr->blkcore_priv);
		up(&mtd_table_mutex);
		return ret;
	} 

	devfs_mk_dir(tr->name);

	INIT_LIST_HEAD(&tr->devs);
	list_add(&tr->list, &blktrans_majors);

	for (i=0; i<MAX_MTD_DEVICES; i++) {
		if (mtd_table[i] && mtd_table[i]->type != MTD_ABSENT)
			tr->add_mtd(tr, mtd_table[i]);
	}

	up(&mtd_table_mutex);

	return 0;
}

int deregister_mtd_blktrans(struct mtd_blktrans_ops *tr)
{
	struct list_head *this, *next;

	down(&mtd_table_mutex);

	/* Clean up the kernel thread */
	tr->blkcore_priv->exiting = 1;
	wake_up(&tr->blkcore_priv->thread_wq);
	wait_for_completion(&tr->blkcore_priv->thread_dead);

	/* Remove it from the list of active majors */
	list_del(&tr->list);

	list_for_each_safe(this, next, &tr->devs) {
		struct mtd_blktrans_dev *dev = list_entry(this, struct mtd_blktrans_dev, list);
		tr->remove_dev(dev);
	}

	devfs_remove(tr->name);
	blk_cleanup_queue(tr->blkcore_priv->rq);
	unregister_blkdev(tr->major, tr->name);

	up(&mtd_table_mutex);

	kfree(tr->blkcore_priv);

	if (!list_empty(&tr->devs))
		BUG();
	return 0;
}

static void __exit mtd_blktrans_exit(void)
{
	/* No race here -- if someone's currently in register_mtd_blktrans
	   we're screwed anyway. */
	if (blktrans_notifier.list.next)
		unregister_mtd_user(&blktrans_notifier);
}

module_exit(mtd_blktrans_exit);

EXPORT_SYMBOL_GPL(register_mtd_blktrans);
EXPORT_SYMBOL_GPL(deregister_mtd_blktrans);
EXPORT_SYMBOL_GPL(add_mtd_blktrans_dev);
EXPORT_SYMBOL_GPL(del_mtd_blktrans_dev);

MODULE_AUTHOR("David Woodhouse <dwmw2@infradead.org>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Common interface to block layer for MTD 'translation layers'");
