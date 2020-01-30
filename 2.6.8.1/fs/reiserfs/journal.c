/*
** Write ahead logging implementation copyright Chris Mason 2000
**
** The background commits make this code very interelated, and 
** overly complex.  I need to rethink things a bit....The major players:
**
** journal_begin -- call with the number of blocks you expect to log.  
**                  If the current transaction is too
** 		    old, it will block until the current transaction is 
** 		    finished, and then start a new one.
**		    Usually, your transaction will get joined in with 
**                  previous ones for speed.
**
** journal_join  -- same as journal_begin, but won't block on the current 
**                  transaction regardless of age.  Don't ever call
**                  this.  Ever.  There are only two places it should be 
**                  called from, and they are both inside this file.
**
** journal_mark_dirty -- adds blocks into this transaction.  clears any flags 
**                       that might make them get sent to disk
**                       and then marks them BH_JDirty.  Puts the buffer head 
**                       into the current transaction hash.  
**
** journal_end -- if the current transaction is batchable, it does nothing
**                   otherwise, it could do an async/synchronous commit, or
**                   a full flush of all log and real blocks in the 
**                   transaction.
**
** flush_old_commits -- if the current transaction is too old, it is ended and 
**                      commit blocks are sent to disk.  Forces commit blocks 
**                      to disk for all backgrounded commits that have been 
**                      around too long.
**		     -- Note, if you call this as an immediate flush from 
**		        from within kupdate, it will ignore the immediate flag
*/

#include <linux/config.h>
#include <asm/uaccess.h>
#include <asm/system.h>

#include <linux/time.h>
#include <asm/semaphore.h>

#include <linux/vmalloc.h>
#include <linux/reiserfs_fs.h>

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/smp_lock.h>
#include <linux/suspend.h>
#include <linux/buffer_head.h>
#include <linux/workqueue.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>


/* gets a struct reiserfs_journal_list * from a list head */
#define JOURNAL_LIST_ENTRY(h) (list_entry((h), struct reiserfs_journal_list, \
                               j_list))
#define JOURNAL_WORK_ENTRY(h) (list_entry((h), struct reiserfs_journal_list, \
                               j_working_list))

/* the number of mounted filesystems.  This is used to decide when to
** start and kill the commit workqueue
*/
static int reiserfs_mounted_fs_count;

static struct workqueue_struct *commit_wq;

#define JOURNAL_TRANS_HALF 1018   /* must be correct to keep the desc and commit
				     structs at 4k */
#define BUFNR 64 /*read ahead */

/* cnode stat bits.  Move these into reiserfs_fs.h */

#define BLOCK_FREED 2		/* this block was freed, and can't be written.  */
#define BLOCK_FREED_HOLDER 3    /* this block was freed during this transaction, and can't be written */

#define BLOCK_NEEDS_FLUSH 4	/* used in flush_journal_list */
#define BLOCK_DIRTIED 5


/* journal list state bits */
#define LIST_TOUCHED 1
#define LIST_DIRTY   2
#define LIST_COMMIT_PENDING  4		/* someone will commit this list */

/* flags for do_journal_end */
#define FLUSH_ALL   1		/* flush commit and real blocks */
#define COMMIT_NOW  2		/* end and commit this transaction */
#define WAIT        4		/* wait for the log blocks to hit the disk*/

/* state bits for the journal */
#define WRITERS_BLOCKED 1      /* set when new writers not allowed */
#define WRITERS_QUEUED 2       /* set when log is full due to too many
				* writers
				*/

static int do_journal_end(struct reiserfs_transaction_handle *,struct super_block *,unsigned long nblocks,int flags) ;
static int flush_journal_list(struct super_block *s, struct reiserfs_journal_list *jl, int flushall) ;
static int flush_commit_list(struct super_block *s, struct reiserfs_journal_list *jl, int flushall)  ;
static int can_dirty(struct reiserfs_journal_cnode *cn) ;
static int journal_join(struct reiserfs_transaction_handle *th, struct super_block *p_s_sb, unsigned long nblocks);
static int release_journal_dev( struct super_block *super,
				struct reiserfs_journal *journal );
static int dirty_one_transaction(struct super_block *s,
                                 struct reiserfs_journal_list *jl);
static void flush_async_commits(void *p);

static void init_journal_hash(struct super_block *p_s_sb) {
  memset(SB_JOURNAL(p_s_sb)->j_hash_table, 0, JOURNAL_HASH_SIZE * sizeof(struct reiserfs_journal_cnode *)) ;
}

/*
** clears BH_Dirty and sticks the buffer on the clean list.  Called because I can't allow refile_buffer to
** make schedule happen after I've freed a block.  Look at remove_from_transaction and journal_mark_freed for
** more details.
*/
static int reiserfs_clean_and_file_buffer(struct buffer_head *bh) {
  if (bh) {
    clear_buffer_dirty(bh);
    clear_bit(BH_JTest, &bh->b_state);
  }
  return 0 ;
}

static struct reiserfs_bitmap_node *
allocate_bitmap_node(struct super_block *p_s_sb) {
  struct reiserfs_bitmap_node *bn ;
  static int id;

  bn = reiserfs_kmalloc(sizeof(struct reiserfs_bitmap_node), GFP_NOFS, p_s_sb) ;
  if (!bn) {
    return NULL ;
  }
  bn->data = reiserfs_kmalloc(p_s_sb->s_blocksize, GFP_NOFS, p_s_sb) ;
  if (!bn->data) {
    reiserfs_kfree(bn, sizeof(struct reiserfs_bitmap_node), p_s_sb) ;
    return NULL ;
  }
  bn->id = id++ ;
  memset(bn->data, 0, p_s_sb->s_blocksize) ;
  INIT_LIST_HEAD(&bn->list) ;
  return bn ;
}

static struct reiserfs_bitmap_node *
get_bitmap_node(struct super_block *p_s_sb) {
  struct reiserfs_bitmap_node *bn = NULL;
  struct list_head *entry = SB_JOURNAL(p_s_sb)->j_bitmap_nodes.next ;

  SB_JOURNAL(p_s_sb)->j_used_bitmap_nodes++ ;
repeat:

  if(entry != &SB_JOURNAL(p_s_sb)->j_bitmap_nodes) {
    bn = list_entry(entry, struct reiserfs_bitmap_node, list) ;
    list_del(entry) ;
    memset(bn->data, 0, p_s_sb->s_blocksize) ;
    SB_JOURNAL(p_s_sb)->j_free_bitmap_nodes-- ;
    return bn ;
  }
  bn = allocate_bitmap_node(p_s_sb) ;
  if (!bn) {
    yield();
    goto repeat ;
  }
  return bn ;
}
static inline void free_bitmap_node(struct super_block *p_s_sb,
                                    struct reiserfs_bitmap_node *bn) {
  SB_JOURNAL(p_s_sb)->j_used_bitmap_nodes-- ;
  if (SB_JOURNAL(p_s_sb)->j_free_bitmap_nodes > REISERFS_MAX_BITMAP_NODES) {
    reiserfs_kfree(bn->data, p_s_sb->s_blocksize, p_s_sb) ;
    reiserfs_kfree(bn, sizeof(struct reiserfs_bitmap_node), p_s_sb) ;
  } else {
    list_add(&bn->list, &SB_JOURNAL(p_s_sb)->j_bitmap_nodes) ;
    SB_JOURNAL(p_s_sb)->j_free_bitmap_nodes++ ;
  }
}

static void allocate_bitmap_nodes(struct super_block *p_s_sb) {
  int i ;
  struct reiserfs_bitmap_node *bn = NULL ;
  for (i = 0 ; i < REISERFS_MIN_BITMAP_NODES ; i++) {
    bn = allocate_bitmap_node(p_s_sb) ;
    if (bn) {
      list_add(&bn->list, &SB_JOURNAL(p_s_sb)->j_bitmap_nodes) ;
      SB_JOURNAL(p_s_sb)->j_free_bitmap_nodes++ ;
    } else {
      break ; // this is ok, we'll try again when more are needed 
    }
  }
}

static int set_bit_in_list_bitmap(struct super_block *p_s_sb, int block,
                                  struct reiserfs_list_bitmap *jb) {
  int bmap_nr = block / (p_s_sb->s_blocksize << 3) ;
  int bit_nr = block % (p_s_sb->s_blocksize << 3) ;

  if (!jb->bitmaps[bmap_nr]) {
    jb->bitmaps[bmap_nr] = get_bitmap_node(p_s_sb) ;
  }
  set_bit(bit_nr, (unsigned long *)jb->bitmaps[bmap_nr]->data) ;
  return 0 ;
}

static void cleanup_bitmap_list(struct super_block *p_s_sb,
                                struct reiserfs_list_bitmap *jb) {
  int i;
  if (jb->bitmaps == NULL)
    return;

  for (i = 0 ; i < SB_BMAP_NR(p_s_sb) ; i++) {
    if (jb->bitmaps[i]) {
      free_bitmap_node(p_s_sb, jb->bitmaps[i]) ;
      jb->bitmaps[i] = NULL ;
    }
  }
}

/*
** only call this on FS unmount.
*/
static int free_list_bitmaps(struct super_block *p_s_sb,
                             struct reiserfs_list_bitmap *jb_array) {
  int i ;
  struct reiserfs_list_bitmap *jb ;
  for (i = 0 ; i < JOURNAL_NUM_BITMAPS ; i++) {
    jb = jb_array + i ;
    jb->journal_list = NULL ;
    cleanup_bitmap_list(p_s_sb, jb) ;
    vfree(jb->bitmaps) ;
    jb->bitmaps = NULL ;
  }
  return 0;
}

static int free_bitmap_nodes(struct super_block *p_s_sb) {
  struct list_head *next = SB_JOURNAL(p_s_sb)->j_bitmap_nodes.next ;
  struct reiserfs_bitmap_node *bn ;

  while(next != &SB_JOURNAL(p_s_sb)->j_bitmap_nodes) {
    bn = list_entry(next, struct reiserfs_bitmap_node, list) ;
    list_del(next) ;
    reiserfs_kfree(bn->data, p_s_sb->s_blocksize, p_s_sb) ;
    reiserfs_kfree(bn, sizeof(struct reiserfs_bitmap_node), p_s_sb) ;
    next = SB_JOURNAL(p_s_sb)->j_bitmap_nodes.next ;
    SB_JOURNAL(p_s_sb)->j_free_bitmap_nodes-- ;
  }

  return 0 ;
}

/*
** get memory for JOURNAL_NUM_BITMAPS worth of bitmaps. 
** jb_array is the array to be filled in.
*/
int reiserfs_allocate_list_bitmaps(struct super_block *p_s_sb,
                                   struct reiserfs_list_bitmap *jb_array,
				   int bmap_nr) {
  int i ;
  int failed = 0 ;
  struct reiserfs_list_bitmap *jb ;
  int mem = bmap_nr * sizeof(struct reiserfs_bitmap_node *) ;

  for (i = 0 ; i < JOURNAL_NUM_BITMAPS ; i++) {
    jb = jb_array + i ;
    jb->journal_list = NULL ;
    jb->bitmaps = vmalloc( mem ) ;
    if (!jb->bitmaps) {
      reiserfs_warning(p_s_sb, "clm-2000, unable to allocate bitmaps for journal lists") ;
      failed = 1;   
      break ;
    }
    memset(jb->bitmaps, 0, mem) ;
  }
  if (failed) {
    free_list_bitmaps(p_s_sb, jb_array) ;
    return -1 ;
  }
  return 0 ;
}

/*
** find an available list bitmap.  If you can't find one, flush a commit list 
** and try again
*/
static struct reiserfs_list_bitmap *
get_list_bitmap(struct super_block *p_s_sb, struct reiserfs_journal_list *jl) {
  int i,j ; 
  struct reiserfs_list_bitmap *jb = NULL ;

  for (j = 0 ; j < (JOURNAL_NUM_BITMAPS * 3) ; j++) {
    i = SB_JOURNAL(p_s_sb)->j_list_bitmap_index ;
    SB_JOURNAL(p_s_sb)->j_list_bitmap_index = (i + 1) % JOURNAL_NUM_BITMAPS ;
    jb = SB_JOURNAL(p_s_sb)->j_list_bitmap + i ;
    if (SB_JOURNAL(p_s_sb)->j_list_bitmap[i].journal_list) {
      flush_commit_list(p_s_sb, SB_JOURNAL(p_s_sb)->j_list_bitmap[i].journal_list, 1) ;
      if (!SB_JOURNAL(p_s_sb)->j_list_bitmap[i].journal_list) {
	break ;
      }
    } else {
      break ;
    }
  }
  if (jb->journal_list) { /* double check to make sure if flushed correctly */
    return NULL ;
  }
  jb->journal_list = jl ;
  return jb ;
}

/* 
** allocates a new chunk of X nodes, and links them all together as a list.
** Uses the cnode->next and cnode->prev pointers
** returns NULL on failure
*/
static struct reiserfs_journal_cnode *allocate_cnodes(int num_cnodes) {
  struct reiserfs_journal_cnode *head ;
  int i ;
  if (num_cnodes <= 0) {
    return NULL ;
  }
  head = vmalloc(num_cnodes * sizeof(struct reiserfs_journal_cnode)) ;
  if (!head) {
    return NULL ;
  }
  memset(head, 0, num_cnodes * sizeof(struct reiserfs_journal_cnode)) ;
  head[0].prev = NULL ;
  head[0].next = head + 1 ;
  for (i = 1 ; i < num_cnodes; i++) {
    head[i].prev = head + (i - 1) ;
    head[i].next = head + (i + 1) ; /* if last one, overwrite it after the if */
  }
  head[num_cnodes -1].next = NULL ;
  return head ;
}

/*
** pulls a cnode off the free list, or returns NULL on failure 
*/
static struct reiserfs_journal_cnode *get_cnode(struct super_block *p_s_sb) {
  struct reiserfs_journal_cnode *cn ;

  reiserfs_check_lock_depth(p_s_sb, "get_cnode") ;

  if (SB_JOURNAL(p_s_sb)->j_cnode_free <= 0) {
    return NULL ;
  }
  SB_JOURNAL(p_s_sb)->j_cnode_used++ ;
  SB_JOURNAL(p_s_sb)->j_cnode_free-- ;
  cn = SB_JOURNAL(p_s_sb)->j_cnode_free_list ;
  if (!cn) {
    return cn ;
  }
  if (cn->next) {
    cn->next->prev = NULL ;
  }
  SB_JOURNAL(p_s_sb)->j_cnode_free_list = cn->next ;
  memset(cn, 0, sizeof(struct reiserfs_journal_cnode)) ;
  return cn ;
}

/*
** returns a cnode to the free list 
*/
static void free_cnode(struct super_block *p_s_sb, struct reiserfs_journal_cnode *cn) {

  reiserfs_check_lock_depth(p_s_sb, "free_cnode") ;

  SB_JOURNAL(p_s_sb)->j_cnode_used-- ;
  SB_JOURNAL(p_s_sb)->j_cnode_free++ ;
  /* memset(cn, 0, sizeof(struct reiserfs_journal_cnode)) ; */
  cn->next = SB_JOURNAL(p_s_sb)->j_cnode_free_list ;
  if (SB_JOURNAL(p_s_sb)->j_cnode_free_list) {
    SB_JOURNAL(p_s_sb)->j_cnode_free_list->prev = cn ;
  }
  cn->prev = NULL ; /* not needed with the memset, but I might kill the memset, and forget to do this */
  SB_JOURNAL(p_s_sb)->j_cnode_free_list = cn ;
}

static int clear_prepared_bits(struct buffer_head *bh) {
  clear_bit(BH_JPrepared, &bh->b_state) ;
  clear_bit(BH_JRestore_dirty, &bh->b_state) ;
  return 0 ;
}

/* buffer is in current transaction */
inline int buffer_journaled(const struct buffer_head *bh) {
  if (bh)
    return test_bit(BH_JDirty, &bh->b_state) ;
  else
    return 0 ;
}

/* disk block was taken off free list before being in a finished transation, or written to disk
** journal_new blocks can be reused immediately, for any purpose
*/ 
inline int buffer_journal_new(const struct buffer_head *bh) {
  if (bh) 
    return test_bit(BH_JNew, &bh->b_state) ;
  else
    return 0 ;
}

inline int mark_buffer_journal_new(struct buffer_head *bh) {
  if (bh) {
    set_bit(BH_JNew, &bh->b_state) ;
  }
  return 0 ;
}

inline int mark_buffer_not_journaled(struct buffer_head *bh) {
  if (bh) 
    clear_bit(BH_JDirty, &bh->b_state) ;
  return 0 ;
}

/* utility function to force a BUG if it is called without the big
** kernel lock held.  caller is the string printed just before calling BUG()
*/
void reiserfs_check_lock_depth(struct super_block *sb, char *caller) {
#ifdef CONFIG_SMP
  if (current->lock_depth < 0) {
    reiserfs_panic (sb, "%s called without kernel lock held", caller) ;
  }
#else
  ;
#endif
}

/* return a cnode with same dev, block number and size in table, or null if not found */
static inline struct reiserfs_journal_cnode *
get_journal_hash_dev(struct super_block *sb,
		     struct reiserfs_journal_cnode **table,
		     long bl)
{
  struct reiserfs_journal_cnode *cn ;
  cn = journal_hash(table, sb, bl) ;
  while(cn) {
    if (cn->blocknr == bl && cn->sb == sb)
      return cn ;
    cn = cn->hnext ;
  }
  return (struct reiserfs_journal_cnode *)0 ;
}

/* returns a cnode with same size, block number and dev as bh in the current transaction hash.  NULL if not found */
static inline struct reiserfs_journal_cnode *get_journal_hash(struct super_block *p_s_sb, struct buffer_head *bh) {
  struct reiserfs_journal_cnode *cn ;
  if (bh) {
    cn =  get_journal_hash_dev(p_s_sb, SB_JOURNAL(p_s_sb)->j_hash_table, bh->b_blocknr);
  }
  else {
    return (struct reiserfs_journal_cnode *)0 ;
  }
  return cn ;
}

/*
** this actually means 'can this block be reallocated yet?'.  If you set search_all, a block can only be allocated
** if it is not in the current transaction, was not freed by the current transaction, and has no chance of ever
** being overwritten by a replay after crashing.
**
** If you don't set search_all, a block can only be allocated if it is not in the current transaction.  Since deleting
** a block removes it from the current transaction, this case should never happen.  If you don't set search_all, make
** sure you never write the block without logging it.
**
** next_zero_bit is a suggestion about the next block to try for find_forward.
** when bl is rejected because it is set in a journal list bitmap, we search
** for the next zero bit in the bitmap that rejected bl.  Then, we return that
** through next_zero_bit for find_forward to try.
**
** Just because we return something in next_zero_bit does not mean we won't
** reject it on the next call to reiserfs_in_journal
**
*/
int reiserfs_in_journal(struct super_block *p_s_sb,
                        int bmap_nr, int bit_nr, int search_all, 
			b_blocknr_t *next_zero_bit) {
  struct reiserfs_journal_cnode *cn ;
  struct reiserfs_list_bitmap *jb ;
  int i ;
  unsigned long bl;

  *next_zero_bit = 0 ; /* always start this at zero. */

  PROC_INFO_INC( p_s_sb, journal.in_journal );
  /* If we aren't doing a search_all, this is a metablock, and it will be logged before use.
  ** if we crash before the transaction that freed it commits,  this transaction won't
  ** have committed either, and the block will never be written
  */
  if (search_all) {
    for (i = 0 ; i < JOURNAL_NUM_BITMAPS ; i++) {
      PROC_INFO_INC( p_s_sb, journal.in_journal_bitmap );
      jb = SB_JOURNAL(p_s_sb)->j_list_bitmap + i ;
      if (jb->journal_list && jb->bitmaps[bmap_nr] &&
          test_bit(bit_nr, (unsigned long *)jb->bitmaps[bmap_nr]->data)) {
	*next_zero_bit = find_next_zero_bit((unsigned long *)
	                             (jb->bitmaps[bmap_nr]->data),
	                             p_s_sb->s_blocksize << 3, bit_nr+1) ; 
	return 1 ;
      }
    }
  }

  bl = bmap_nr * (p_s_sb->s_blocksize << 3) + bit_nr;
  /* is it in any old transactions? */
  if (search_all && (cn = get_journal_hash_dev(p_s_sb, SB_JOURNAL(p_s_sb)->j_list_hash_table, bl))) {
    return 1; 
  }

  /* is it in the current transaction.  This should never happen */
  if ((cn = get_journal_hash_dev(p_s_sb, SB_JOURNAL(p_s_sb)->j_hash_table, bl))) {
    BUG();
    return 1; 
  }

  PROC_INFO_INC( p_s_sb, journal.in_journal_reusable );
  /* safe for reuse */
  return 0 ;
}

/* insert cn into table
*/
inline void insert_journal_hash(struct reiserfs_journal_cnode **table, struct reiserfs_journal_cnode *cn) {
  struct reiserfs_journal_cnode *cn_orig ;

  cn_orig = journal_hash(table, cn->sb, cn->blocknr) ;
  cn->hnext = cn_orig ;
  cn->hprev = NULL ;
  if (cn_orig) {
    cn_orig->hprev = cn ;
  }
  journal_hash(table, cn->sb, cn->blocknr) =  cn ;
}

/* lock the current transaction */
inline static void lock_journal(struct super_block *p_s_sb) {
    PROC_INFO_INC( p_s_sb, journal.lock_journal );
    down(&SB_JOURNAL(p_s_sb)->j_lock);
}

/* unlock the current transaction */
inline static void unlock_journal(struct super_block *p_s_sb) {
    up(&SB_JOURNAL(p_s_sb)->j_lock);
}

static inline void get_journal_list(struct reiserfs_journal_list *jl)
{
    jl->j_refcount++;
}

static inline void put_journal_list(struct super_block *s,
                                   struct reiserfs_journal_list *jl)
{
    if (jl->j_refcount < 1) {
        reiserfs_panic (s, "trans id %lu, refcount at %d", jl->j_trans_id,
	                                         jl->j_refcount);
    }
    if (--jl->j_refcount == 0)
        reiserfs_kfree(jl, sizeof(struct reiserfs_journal_list), s);
}

/*
** this used to be much more involved, and I'm keeping it just in case things get ugly again.
** it gets called by flush_commit_list, and cleans up any data stored about blocks freed during a
** transaction.
*/
static void cleanup_freed_for_journal_list(struct super_block *p_s_sb, struct reiserfs_journal_list *jl) {

  struct reiserfs_list_bitmap *jb = jl->j_list_bitmap ;
  if (jb) {
    cleanup_bitmap_list(p_s_sb, jb) ;
  }
  jl->j_list_bitmap->journal_list = NULL ;
  jl->j_list_bitmap = NULL ;
}

static int journal_list_still_alive(struct super_block *s,
                                    unsigned long trans_id)
{
    struct list_head *entry = &SB_JOURNAL(s)->j_journal_list;
    struct reiserfs_journal_list *jl;

    if (!list_empty(entry)) {
        jl = JOURNAL_LIST_ENTRY(entry->next);
	if (jl->j_trans_id <= trans_id) {
	    return 1;
	}
    }
    return 0;
}

static void reiserfs_end_buffer_io_sync(struct buffer_head *bh, int uptodate) {
    char b[BDEVNAME_SIZE];

    if (buffer_journaled(bh)) {
        reiserfs_warning(NULL, "clm-2084: pinned buffer %lu:%s sent to disk",
	                 bh->b_blocknr, bdevname(bh->b_bdev, b)) ;
    }
    if (uptodate)
    	set_buffer_uptodate(bh) ;
    else
    	clear_buffer_uptodate(bh) ;
    unlock_buffer(bh) ;
    put_bh(bh) ;
}

static void reiserfs_end_ordered_io(struct buffer_head *bh, int uptodate) {
    if (uptodate)
    	set_buffer_uptodate(bh) ;
    else
    	clear_buffer_uptodate(bh) ;
    unlock_buffer(bh) ;
    put_bh(bh) ;
}

static void submit_logged_buffer(struct buffer_head *bh) {
    get_bh(bh) ;
    bh->b_end_io = reiserfs_end_buffer_io_sync ;
    mark_buffer_notjournal_new(bh) ;
    clear_buffer_dirty(bh) ;
    if (!test_and_clear_bit(BH_JTest, &bh->b_state))
        BUG();
    if (!buffer_uptodate(bh))
        BUG();
    submit_bh(WRITE, bh) ;
}

static void submit_ordered_buffer(struct buffer_head *bh) {
    get_bh(bh) ;
    bh->b_end_io = reiserfs_end_ordered_io;
    clear_buffer_dirty(bh) ;
    if (!buffer_uptodate(bh))
        BUG();
    submit_bh(WRITE, bh) ;
}

#define CHUNK_SIZE 32
struct buffer_chunk {
    struct buffer_head *bh[CHUNK_SIZE];
    int nr;
};

static void write_chunk(struct buffer_chunk *chunk) {
    int i;
    for (i = 0; i < chunk->nr ; i++) {
	submit_logged_buffer(chunk->bh[i]) ;
    }
    chunk->nr = 0;
}

static void write_ordered_chunk(struct buffer_chunk *chunk) {
    int i;
    for (i = 0; i < chunk->nr ; i++) {
	submit_ordered_buffer(chunk->bh[i]) ;
    }
    chunk->nr = 0;
}

static int add_to_chunk(struct buffer_chunk *chunk, struct buffer_head *bh,
			 spinlock_t *lock,
			 void (fn)(struct buffer_chunk *))
{
    int ret = 0;
    if (chunk->nr >= CHUNK_SIZE)
        BUG();
    chunk->bh[chunk->nr++] = bh;
    if (chunk->nr >= CHUNK_SIZE) {
	ret = 1;
        if (lock)
	    spin_unlock(lock);
        fn(chunk);
        if (lock)
	    spin_lock(lock);
    }
    return ret;
}


atomic_t nr_reiserfs_jh = ATOMIC_INIT(0);
static struct reiserfs_jh *alloc_jh(void) {
    struct reiserfs_jh *jh;
    while(1) {
	jh = kmalloc(sizeof(*jh), GFP_NOFS);
	if (jh) {
	    atomic_inc(&nr_reiserfs_jh);
	    return jh;
	}
        yield();
    }
}

/*
 * we want to free the jh when the buffer has been written
 * and waited on
 */
void reiserfs_free_jh(struct buffer_head *bh) {
    struct reiserfs_jh *jh;

    jh = bh->b_private;
    if (jh) {
	bh->b_private = NULL;
	jh->bh = NULL;
	list_del_init(&jh->list);
	kfree(jh);
	if (atomic_read(&nr_reiserfs_jh) <= 0)
	    BUG();
	atomic_dec(&nr_reiserfs_jh);
	put_bh(bh);
    }
}

static inline int __add_jh(struct reiserfs_journal *j, struct buffer_head *bh,
                           int tail)
{
    struct reiserfs_jh *jh;

    if (bh->b_private) {
	spin_lock(&j->j_dirty_buffers_lock);
	if (!bh->b_private) {
	    spin_unlock(&j->j_dirty_buffers_lock);
	    goto no_jh;
	}
        jh = bh->b_private;
	list_del_init(&jh->list);
    } else {
no_jh:
	get_bh(bh);
	jh = alloc_jh();
	spin_lock(&j->j_dirty_buffers_lock);
	/* buffer must be locked for __add_jh, should be able to have
	 * two adds at the same time
	 */
	if (bh->b_private)
	    BUG();
	jh->bh = bh;
	bh->b_private = jh;
    }
    jh->jl = j->j_current_jl;
    if (tail)
	list_add_tail(&jh->list, &jh->jl->j_tail_bh_list);
    else {
	list_add_tail(&jh->list, &jh->jl->j_bh_list);
    }
    spin_unlock(&j->j_dirty_buffers_lock);
    return 0;
}

int reiserfs_add_tail_list(struct inode *inode, struct buffer_head *bh) {
    return __add_jh(SB_JOURNAL(inode->i_sb), bh, 1);
}
int reiserfs_add_ordered_list(struct inode *inode, struct buffer_head *bh) {
    return __add_jh(SB_JOURNAL(inode->i_sb), bh, 0);
}

#define JH_ENTRY(l) list_entry((l), struct reiserfs_jh, list)
static int write_ordered_buffers(spinlock_t *lock,
				 struct reiserfs_journal *j,
                                 struct reiserfs_journal_list *jl,
				 struct list_head *list)
{
    struct buffer_head *bh;
    struct reiserfs_jh *jh;
    int ret = 0;
    struct buffer_chunk chunk;
    struct list_head tmp;
    INIT_LIST_HEAD(&tmp);

    chunk.nr = 0;
    spin_lock(lock);
    while(!list_empty(list)) {
        jh = JH_ENTRY(list->next);
	bh = jh->bh;
	get_bh(bh);
	if (test_set_buffer_locked(bh)) {
	    if (!buffer_dirty(bh)) {
		list_del_init(&jh->list);
		list_add(&jh->list, &tmp);
		goto loop_next;
	    }
	    spin_unlock(lock);
	    if (chunk.nr)
		write_ordered_chunk(&chunk);
	    wait_on_buffer(bh);
	    cond_resched();
	    spin_lock(lock);
	    goto loop_next;
	}
	if (buffer_dirty(bh)) {
	    list_del_init(&jh->list);
	    list_add(&jh->list, &tmp);
	    add_to_chunk(&chunk, bh, lock, write_ordered_chunk);
	} else {
	    reiserfs_free_jh(bh);
	    unlock_buffer(bh);
	}
loop_next:
	put_bh(bh);
	cond_resched_lock(lock);
    }
    if (chunk.nr) {
	spin_unlock(lock);
        write_ordered_chunk(&chunk);
	spin_lock(lock);
    }
    while(!list_empty(&tmp)) {
        jh = JH_ENTRY(tmp.prev);
	bh = jh->bh;
	get_bh(bh);
	reiserfs_free_jh(bh);

	if (buffer_locked(bh)) {
	    spin_unlock(lock);
	    wait_on_buffer(bh);
	    spin_lock(lock);
	}
	if (!buffer_uptodate(bh))
	    ret = -EIO;
	put_bh(bh);
	cond_resched_lock(lock);
    }
    spin_unlock(lock);
    return ret;
}

static int flush_older_commits(struct super_block *s, struct reiserfs_journal_list *jl) {
    struct reiserfs_journal_list *other_jl;
    struct reiserfs_journal_list *first_jl;
    struct list_head *entry;
    unsigned long trans_id = jl->j_trans_id;
    unsigned long other_trans_id;
    unsigned long first_trans_id;

find_first:
    /*
     * first we walk backwards to find the oldest uncommitted transation
     */
    first_jl = jl;
    entry = jl->j_list.prev;
    while(1) {
	other_jl = JOURNAL_LIST_ENTRY(entry);
	if (entry == &SB_JOURNAL(s)->j_journal_list ||
	    atomic_read(&other_jl->j_older_commits_done))
	    break;

        first_jl = other_jl;
	entry = other_jl->j_list.prev;
    }

    /* if we didn't find any older uncommitted transactions, return now */
    if (first_jl == jl) {
        return 0;
    }

    first_trans_id = first_jl->j_trans_id;

    entry = &first_jl->j_list;
    while(1) {
	other_jl = JOURNAL_LIST_ENTRY(entry);
	other_trans_id = other_jl->j_trans_id;

	if (other_trans_id < trans_id) {
	    if (atomic_read(&other_jl->j_commit_left) != 0) {
		flush_commit_list(s, other_jl, 0);

		/* list we were called with is gone, return */
		if (!journal_list_still_alive(s, trans_id))
		    return 1;

		/* the one we just flushed is gone, this means all
		 * older lists are also gone, so first_jl is no longer
		 * valid either.  Go back to the beginning.
		 */
		if (!journal_list_still_alive(s, other_trans_id)) {
		    goto find_first;
		}
	    }
	    entry = entry->next;
	    if (entry == &SB_JOURNAL(s)->j_journal_list)
		return 0;
	} else {
	    return 0;
	}
    }
    return 0;
}
int reiserfs_async_progress_wait(struct super_block *s) {
    DEFINE_WAIT(wait);
    struct reiserfs_journal *j = SB_JOURNAL(s);
    if (atomic_read(&j->j_async_throttle))
    	blk_congestion_wait(WRITE, HZ/10);
    return 0;
}

/*
** if this journal list still has commit blocks unflushed, send them to disk.
**
** log areas must be flushed in order (transaction 2 can't commit before transaction 1)
** Before the commit block can by written, every other log block must be safely on disk
**
*/
static int flush_commit_list(struct super_block *s, struct reiserfs_journal_list *jl, int flushall) {
  int i;
  int bn ;
  struct buffer_head *tbh = NULL ;
  unsigned long trans_id = jl->j_trans_id;

  reiserfs_check_lock_depth(s, "flush_commit_list") ;

  if (atomic_read(&jl->j_older_commits_done)) {
    return 0 ;
  }

  /* before we can put our commit blocks on disk, we have to make sure everyone older than
  ** us is on disk too
  */
  if (jl->j_len <= 0)
    BUG();
  if (trans_id == SB_JOURNAL(s)->j_trans_id)
    BUG();

  get_journal_list(jl);
  if (flushall) {
    if (flush_older_commits(s, jl) == 1) {
      /* list disappeared during flush_older_commits.  return */
      goto put_jl;
    }
  }

  /* make sure nobody is trying to flush this one at the same time */
  down(&jl->j_commit_lock);
  if (!journal_list_still_alive(s, trans_id)) {
    up(&jl->j_commit_lock);
    goto put_jl;
  }
  if (jl->j_trans_id == 0)
    BUG();

  /* this commit is done, exit */
  if (atomic_read(&(jl->j_commit_left)) <= 0) {
    if (flushall) {
      atomic_set(&(jl->j_older_commits_done), 1) ;
    }
    up(&jl->j_commit_lock);
    goto put_jl;
  }

  if (!list_empty(&jl->j_bh_list)) {
      unlock_kernel();
      write_ordered_buffers(&SB_JOURNAL(s)->j_dirty_buffers_lock,
                            SB_JOURNAL(s), jl, &jl->j_bh_list);
      lock_kernel();
  }
  if (!list_empty(&jl->j_bh_list))
      BUG();
  /*
   * for the description block and all the log blocks, submit any buffers
   * that haven't already reached the disk
   */
  atomic_inc(&SB_JOURNAL(s)->j_async_throttle);
  for (i = 0 ; i < (jl->j_len + 1) ; i++) {
    bn = SB_ONDISK_JOURNAL_1st_BLOCK(s) + (jl->j_start+i) %
         SB_ONDISK_JOURNAL_SIZE(s);
    tbh = journal_find_get_block(s, bn) ;
    if (buffer_dirty(tbh))
	ll_rw_block(WRITE, 1, &tbh) ;
    put_bh(tbh) ;
  }
  atomic_dec(&SB_JOURNAL(s)->j_async_throttle);

  /* wait on everything written so far before writing the commit */
  for (i = 0 ;  i < (jl->j_len + 1) ; i++) {
    bn = SB_ONDISK_JOURNAL_1st_BLOCK(s) +
	 (jl->j_start + i) % SB_ONDISK_JOURNAL_SIZE(s) ;
    tbh = journal_find_get_block(s, bn) ;
    wait_on_buffer(tbh) ;
    // since we're using ll_rw_blk above, it might have skipped over
    // a locked buffer.  Double check here
    //
    if (buffer_dirty(tbh))
      sync_dirty_buffer(tbh);
    if (!buffer_uptodate(tbh)) {
      reiserfs_panic(s, "journal-601, buffer write failed\n") ;
    }
    put_bh(tbh) ; /* once for journal_find_get_block */
    put_bh(tbh) ;    /* once due to original getblk in do_journal_end */
    atomic_dec(&(jl->j_commit_left)) ;
  }

  if (atomic_read(&(jl->j_commit_left)) != 1)
    BUG();

  if (buffer_dirty(jl->j_commit_bh))
    BUG();
  mark_buffer_dirty(jl->j_commit_bh) ;
  sync_dirty_buffer(jl->j_commit_bh) ;
  if (!buffer_uptodate(jl->j_commit_bh)) {
    reiserfs_panic(s, "journal-615: buffer write failed\n") ;
  }
  bforget(jl->j_commit_bh) ;
  if (SB_JOURNAL(s)->j_last_commit_id != 0 &&
     (jl->j_trans_id - SB_JOURNAL(s)->j_last_commit_id) != 1) {
      reiserfs_warning(s, "clm-2200: last commit %lu, current %lu",
                       SB_JOURNAL(s)->j_last_commit_id,
		       jl->j_trans_id);
  }
  SB_JOURNAL(s)->j_last_commit_id = jl->j_trans_id;

  /* now, every commit block is on the disk.  It is safe to allow blocks freed during this transaction to be reallocated */
  cleanup_freed_for_journal_list(s, jl) ;

  /* mark the metadata dirty */
  dirty_one_transaction(s, jl);
  atomic_dec(&(jl->j_commit_left)) ;

  if (flushall) {
    atomic_set(&(jl->j_older_commits_done), 1) ;
  }
  up(&jl->j_commit_lock);
put_jl:
  put_journal_list(s, jl);
  return 0 ;
}

/*
** flush_journal_list frequently needs to find a newer transaction for a given block.  This does that, or 
** returns NULL if it can't find anything 
*/
static struct reiserfs_journal_list *find_newer_jl_for_cn(struct reiserfs_journal_cnode *cn) {
  struct super_block *sb = cn->sb;
  b_blocknr_t blocknr = cn->blocknr ;

  cn = cn->hprev ;
  while(cn) {
    if (cn->sb == sb && cn->blocknr == blocknr && cn->jlist) {
      return cn->jlist ;
    }
    cn = cn->hprev ;
  }
  return NULL ;
}

void remove_journal_hash(struct super_block *, struct reiserfs_journal_cnode **,
struct reiserfs_journal_list *, unsigned long, int);

/*
** once all the real blocks have been flushed, it is safe to remove them from the
** journal list for this transaction.  Aside from freeing the cnode, this also allows the
** block to be reallocated for data blocks if it had been deleted.
*/
static void remove_all_from_journal_list(struct super_block *p_s_sb, struct reiserfs_journal_list *jl, int debug) {
  struct reiserfs_journal_cnode *cn, *last ;
  cn = jl->j_realblock ;

  /* which is better, to lock once around the whole loop, or
  ** to lock for each call to remove_journal_hash?
  */
  while(cn) {
    if (cn->blocknr != 0) {
      if (debug) {
       reiserfs_warning (p_s_sb, "block %u, bh is %d, state %ld", cn->blocknr,
                         cn->bh ? 1: 0, cn->state) ;
      }
      cn->state = 0 ;
      remove_journal_hash(p_s_sb, SB_JOURNAL(p_s_sb)->j_list_hash_table, jl, cn->blocknr, 1) ;
    }
    last = cn ;
    cn = cn->next ;
    free_cnode(p_s_sb, last) ;
  }
  jl->j_realblock = NULL ;
}

/*
** if this timestamp is greater than the timestamp we wrote last to the header block, write it to the header block.
** once this is done, I can safely say the log area for this transaction won't ever be replayed, and I can start
** releasing blocks in this transaction for reuse as data blocks.
** called by flush_journal_list, before it calls remove_all_from_journal_list
**
*/
static int _update_journal_header_block(struct super_block *p_s_sb, unsigned long offset, unsigned long trans_id) {
  struct reiserfs_journal_header *jh ;
  if (trans_id >= SB_JOURNAL(p_s_sb)->j_last_flush_trans_id) {
    if (buffer_locked((SB_JOURNAL(p_s_sb)->j_header_bh)))  {
      wait_on_buffer((SB_JOURNAL(p_s_sb)->j_header_bh)) ;
      if (!buffer_uptodate(SB_JOURNAL(p_s_sb)->j_header_bh)) {
        reiserfs_panic(p_s_sb, "journal-699: buffer write failed\n") ;
      }
    }
    SB_JOURNAL(p_s_sb)->j_last_flush_trans_id = trans_id ;
    SB_JOURNAL(p_s_sb)->j_first_unflushed_offset = offset ;
    jh = (struct reiserfs_journal_header *)(SB_JOURNAL(p_s_sb)->j_header_bh->b_data) ;
    jh->j_last_flush_trans_id = cpu_to_le32(trans_id) ;
    jh->j_first_unflushed_offset = cpu_to_le32(offset) ;
    jh->j_mount_id = cpu_to_le32(SB_JOURNAL(p_s_sb)->j_mount_id) ;
    set_buffer_dirty(SB_JOURNAL(p_s_sb)->j_header_bh) ;
    sync_dirty_buffer(SB_JOURNAL(p_s_sb)->j_header_bh) ;
    if (!buffer_uptodate(SB_JOURNAL(p_s_sb)->j_header_bh)) {
      reiserfs_warning (p_s_sb, "journal-837: IO error during journal replay");
      return -EIO ;
    }
  }
  return 0 ;
}

static int update_journal_header_block(struct super_block *p_s_sb, 
                                       unsigned long offset, 
				       unsigned long trans_id) {
    if (_update_journal_header_block(p_s_sb, offset, trans_id)) {
	reiserfs_panic(p_s_sb, "journal-712: buffer write failed\n") ;
    }
    return 0 ;
}
/* 
** flush any and all journal lists older than you are 
** can only be called from flush_journal_list
*/
static int flush_older_journal_lists(struct super_block *p_s_sb,
                                     struct reiserfs_journal_list *jl)
{
    struct list_head *entry;
    struct reiserfs_journal_list *other_jl ;
    unsigned long trans_id = jl->j_trans_id;

    /* we know we are the only ones flushing things, no extra race
     * protection is required.
     */
restart:
    entry = SB_JOURNAL(p_s_sb)->j_journal_list.next;
    other_jl = JOURNAL_LIST_ENTRY(entry);
    if (other_jl->j_trans_id < trans_id) {
	/* do not flush all */
	flush_journal_list(p_s_sb, other_jl, 0) ;

	/* other_jl is now deleted from the list */
	goto restart;
    }
    return 0 ;
}

static void del_from_work_list(struct super_block *s,
                               struct reiserfs_journal_list *jl) {
    if (!list_empty(&jl->j_working_list)) {
	list_del_init(&jl->j_working_list);
	SB_JOURNAL(s)->j_num_work_lists--;
    }
}

/* flush a journal list, both commit and real blocks
**
** always set flushall to 1, unless you are calling from inside
** flush_journal_list
**
** IMPORTANT.  This can only be called while there are no journal writers, 
** and the journal is locked.  That means it can only be called from 
** do_journal_end, or by journal_release
*/
static int flush_journal_list(struct super_block *s, 
                              struct reiserfs_journal_list *jl, int flushall) {
  struct reiserfs_journal_list *pjl ;
  struct reiserfs_journal_cnode *cn, *last ;
  int count ;
  int was_jwait = 0 ;
  int was_dirty = 0 ;
  struct buffer_head *saved_bh ; 
  unsigned long j_len_saved = jl->j_len ;

  if (j_len_saved <= 0) {
    BUG();
  }

  if (atomic_read(&SB_JOURNAL(s)->j_wcount) != 0) {
    reiserfs_warning(s, "clm-2048: flush_journal_list called with wcount %d",
                      atomic_read(&SB_JOURNAL(s)->j_wcount)) ;
  }
  if (jl->j_trans_id == 0)
    BUG();

  /* if flushall == 0, the lock is already held */
  if (flushall) {
      down(&SB_JOURNAL(s)->j_flush_sem);
  } else if (!down_trylock(&SB_JOURNAL(s)->j_flush_sem)) {
      BUG();
  }

  count = 0 ;
  if (j_len_saved > SB_JOURNAL_TRANS_MAX(s)) {
    reiserfs_panic(s, "journal-715: flush_journal_list, length is %lu, trans id %lu\n", j_len_saved, jl->j_trans_id);
    return 0 ;
  }

  /* if all the work is already done, get out of here */
  if (atomic_read(&(jl->j_nonzerolen)) <= 0 && 
      atomic_read(&(jl->j_commit_left)) <= 0) {
    goto flush_older_and_return ;
  } 

  /* start by putting the commit list on disk.  This will also flush 
  ** the commit lists of any olders transactions
  */
  flush_commit_list(s, jl, 1) ;

  if (!(jl->j_state & LIST_DIRTY))
      BUG();

  /* are we done now? */
  if (atomic_read(&(jl->j_nonzerolen)) <= 0 && 
      atomic_read(&(jl->j_commit_left)) <= 0) {
    goto flush_older_and_return ;
  }

  /* loop through each cnode, see if we need to write it, 
  ** or wait on a more recent transaction, or just ignore it 
  */
  if (atomic_read(&(SB_JOURNAL(s)->j_wcount)) != 0) {
    reiserfs_panic(s, "journal-844: panic journal list is flushing, wcount is not 0\n") ;
  }
  cn = jl->j_realblock ;
  while(cn) {
    was_jwait = 0 ;
    was_dirty = 0 ;
    saved_bh = NULL ;
    /* blocknr of 0 is no longer in the hash, ignore it */
    if (cn->blocknr == 0) {
      goto free_cnode ;
    }
    pjl = find_newer_jl_for_cn(cn) ;
    /* the order is important here.  We check pjl to make sure we
    ** don't clear BH_JDirty_wait if we aren't the one writing this
    ** block to disk
    */
    if (!pjl && cn->bh) {
      saved_bh = cn->bh ;

      /* we do this to make sure nobody releases the buffer while 
      ** we are working with it 
      */
      get_bh(saved_bh) ;

      if (buffer_journal_dirty(saved_bh)) {
	if (!can_dirty(cn))
	  BUG();
        was_jwait = 1 ;
        was_dirty = 1 ;
      } else if (can_dirty(cn)) {
        /* everything with !pjl && jwait should be writable */
	BUG();
      }
    }

    /* if someone has this block in a newer transaction, just make
    ** sure they are commited, and don't try writing it to disk
    */
    if (pjl) {
      if (atomic_read(&pjl->j_commit_left))
        flush_commit_list(s, pjl, 1) ;
      goto free_cnode ;
    }

    /* bh == NULL when the block got to disk on its own, OR, 
    ** the block got freed in a future transaction 
    */
    if (saved_bh == NULL) {
      goto free_cnode ;
    }

    /* this should never happen.  kupdate_one_transaction has this list
    ** locked while it works, so we should never see a buffer here that
    ** is not marked JDirty_wait
    */
    if ((!was_jwait) && !buffer_locked(saved_bh)) {
	reiserfs_warning (s, "journal-813: BAD! buffer %llu %cdirty %cjwait, "
			  "not in a newer tranasction",
			  (unsigned long long)saved_bh->b_blocknr,
			  was_dirty ? ' ' : '!', was_jwait ? ' ' : '!') ;
    }
    if (was_dirty) { 
      /* we inc again because saved_bh gets decremented at free_cnode */
      get_bh(saved_bh) ;
      set_bit(BLOCK_NEEDS_FLUSH, &cn->state) ;
      lock_buffer(saved_bh);
      if (cn->blocknr != saved_bh->b_blocknr)
        BUG();
      if (buffer_dirty(saved_bh))
        submit_logged_buffer(saved_bh) ;
      else
        unlock_buffer(saved_bh);
      count++ ;
    } else {
      reiserfs_warning (s, "clm-2082: Unable to flush buffer %llu in %s",
                        (unsigned long long)saved_bh->b_blocknr, __FUNCTION__);
    }
free_cnode:
    last = cn ;
    cn = cn->next ;
    if (saved_bh) {
      /* we incremented this to keep others from taking the buffer head away */
      put_bh(saved_bh) ;
      if (atomic_read(&(saved_bh->b_count)) < 0) {
        reiserfs_warning (s, "journal-945: saved_bh->b_count < 0");
      }
    }
  }
  if (count > 0) {
    cn = jl->j_realblock ;
    while(cn) {
      if (test_bit(BLOCK_NEEDS_FLUSH, &cn->state)) {
	if (!cn->bh) {
	  reiserfs_panic(s, "journal-1011: cn->bh is NULL\n") ;
	}
	wait_on_buffer(cn->bh) ;
	if (!cn->bh) {
	  reiserfs_panic(s, "journal-1012: cn->bh is NULL\n") ;
	}
	if (!buffer_uptodate(cn->bh)) {
	  reiserfs_panic(s, "journal-949: buffer write failed\n") ;
	}
	/* note, we must clear the JDirty_wait bit after the up to date
	** check, otherwise we race against our flushpage routine
	*/
	if (!test_and_clear_bit(BH_JDirty_wait, &cn->bh->b_state))
	    BUG();

        /* undo the inc from journal_mark_dirty */
	put_bh(cn->bh) ;
        brelse(cn->bh) ;
      }
      cn = cn->next ;
    }
  }

flush_older_and_return:
  /* before we can update the journal header block, we _must_ flush all 
  ** real blocks from all older transactions to disk.  This is because
  ** once the header block is updated, this transaction will not be
  ** replayed after a crash
  */
  if (flushall) {
    flush_older_journal_lists(s, jl);
  } 
  
  /* before we can remove everything from the hash tables for this 
  ** transaction, we must make sure it can never be replayed
  **
  ** since we are only called from do_journal_end, we know for sure there
  ** are no allocations going on while we are flushing journal lists.  So,
  ** we only need to update the journal header block for the last list
  ** being flushed
  */
  if (flushall) {
    update_journal_header_block(s, (jl->j_start + jl->j_len + 2) % SB_ONDISK_JOURNAL_SIZE(s), jl->j_trans_id) ;
  }
  remove_all_from_journal_list(s, jl, 0) ;
  list_del(&jl->j_list);
  SB_JOURNAL(s)->j_num_lists--;
  del_from_work_list(s, jl);

  if (SB_JOURNAL(s)->j_last_flush_id != 0 &&
     (jl->j_trans_id - SB_JOURNAL(s)->j_last_flush_id) != 1) {
      reiserfs_warning(s, "clm-2201: last flush %lu, current %lu",
                       SB_JOURNAL(s)->j_last_flush_id,
		       jl->j_trans_id);
  }
  SB_JOURNAL(s)->j_last_flush_id = jl->j_trans_id;

  /* not strictly required since we are freeing the list, but it should
   * help find code using dead lists later on
   */
  jl->j_len = 0 ;
  atomic_set(&(jl->j_nonzerolen), 0) ;
  jl->j_start = 0 ;
  jl->j_realblock = NULL ;
  jl->j_commit_bh = NULL ;
  jl->j_trans_id = 0 ;
  jl->j_state = 0;
  put_journal_list(s, jl);
  if (flushall)
    up(&SB_JOURNAL(s)->j_flush_sem);
  return 0 ;
} 

static int write_one_transaction(struct super_block *s,
                                 struct reiserfs_journal_list *jl,
				 struct buffer_chunk *chunk)
{
    struct reiserfs_journal_cnode *cn;
    int ret = 0 ;

    jl->j_state |= LIST_TOUCHED;
    del_from_work_list(s, jl);
    if (jl->j_len == 0 || atomic_read(&jl->j_nonzerolen) == 0) {
        return 0;
    }

    cn = jl->j_realblock ;
    while(cn) {
        /* if the blocknr == 0, this has been cleared from the hash,
        ** skip it
        */
        if (cn->blocknr == 0) {
            goto next ;
        }
        if (cn->bh && can_dirty(cn) && buffer_dirty(cn->bh)) {
	    struct buffer_head *tmp_bh;
	    /* we can race against journal_mark_freed when we try
	     * to lock_buffer(cn->bh), so we have to inc the buffer
	     * count, and recheck things after locking
	     */
	    tmp_bh = cn->bh;
	    get_bh(tmp_bh);
	    lock_buffer(tmp_bh);
	    if (cn->bh && can_dirty(cn) && buffer_dirty(tmp_bh)) {
		if (!buffer_journal_dirty(tmp_bh) ||
		    reiserfs_buffer_prepared(tmp_bh))
		    BUG();
		add_to_chunk(chunk, tmp_bh, NULL, write_chunk);
		ret++;
	    } else {
		/* note, cn->bh might be null now */
		unlock_buffer(tmp_bh);
	    }
	    put_bh(tmp_bh);
        }
next:
        cn = cn->next ;
	cond_resched();
    }
    return ret ;
}

/* used by flush_commit_list */
static int dirty_one_transaction(struct super_block *s,
                                 struct reiserfs_journal_list *jl)
{
    struct reiserfs_journal_cnode *cn;
    struct reiserfs_journal_list *pjl;
    int ret = 0 ;

    jl->j_state |= LIST_DIRTY;
    cn = jl->j_realblock ;
    while(cn) {
        /* look for a more recent transaction that logged this
        ** buffer.  Only the most recent transaction with a buffer in
        ** it is allowed to send that buffer to disk
        */
	pjl = find_newer_jl_for_cn(cn) ;
        if (!pjl && cn->blocknr && cn->bh && buffer_journal_dirty(cn->bh))
	{
	    if (!can_dirty(cn))
	        BUG();
	    /* if the buffer is prepared, it will either be logged
	     * or restored.  If restored, we need to make sure
	     * it actually gets marked dirty
	     */
	    mark_buffer_notjournal_new(cn->bh) ;
	    if (test_bit(BH_JPrepared, &cn->bh->b_state)) {
	        set_bit(BH_JRestore_dirty, &cn->bh->b_state);
	    } else {
	        set_bit(BH_JTest, &cn->bh->b_state);
	        mark_buffer_dirty(cn->bh);
	    }
        } 
        cn = cn->next ;
    }
    return ret ;
}

static int kupdate_transactions(struct super_block *s,
                                   struct reiserfs_journal_list *jl,
				   struct reiserfs_journal_list **next_jl,
				   unsigned long *next_trans_id,
				   int num_blocks,
				   int num_trans) {
    int ret = 0;
    int written = 0 ;
    int transactions_flushed = 0;
    unsigned long orig_trans_id = jl->j_trans_id;
    struct buffer_chunk chunk;
    struct list_head *entry;
    chunk.nr = 0;

    down(&SB_JOURNAL(s)->j_flush_sem);
    if (!journal_list_still_alive(s, orig_trans_id)) {
	goto done;
    }

    /* we've got j_flush_sem held, nobody is going to delete any
     * of these lists out from underneath us
     */
    while((num_trans && transactions_flushed < num_trans) ||
          (!num_trans && written < num_blocks)) {

	if (jl->j_len == 0 || (jl->j_state & LIST_TOUCHED) ||
	    atomic_read(&jl->j_commit_left))
	{
	    del_from_work_list(s, jl);
	    break;
	}
	ret = write_one_transaction(s, jl, &chunk);

	if (ret < 0)
	    goto done;
	transactions_flushed++;
	written += ret;
	entry = jl->j_list.next;

	/* did we wrap? */
	if (entry == &SB_JOURNAL(s)->j_journal_list) {
	    break;
        }
	jl = JOURNAL_LIST_ENTRY(entry);

	/* don't bother with older transactions */
	if (jl->j_trans_id <= orig_trans_id)
	    break;
    }
    if (chunk.nr) {
        write_chunk(&chunk);
    }

done:
    up(&SB_JOURNAL(s)->j_flush_sem);
    return ret;
}

/* for o_sync and fsync heavy applications, they tend to use
** all the journa list slots with tiny transactions.  These
** trigger lots and lots of calls to update the header block, which
** adds seeks and slows things down.
**
** This function tries to clear out a large chunk of the journal lists
** at once, which makes everything faster since only the newest journal
** list updates the header block
*/
static int flush_used_journal_lists(struct super_block *s,
                                    struct reiserfs_journal_list *jl) {
    unsigned long len = 0;
    unsigned long cur_len;
    int ret;
    int i;
    int limit = 256;
    struct reiserfs_journal_list *tjl;
    struct reiserfs_journal_list *flush_jl;
    unsigned long trans_id;

    flush_jl = tjl = jl;

    /* in data logging mode, try harder to flush a lot of blocks */
    if (reiserfs_data_log(s))
	limit = 1024;
    /* flush for 256 transactions or limit blocks, whichever comes first */
    for(i = 0 ; i < 256 && len < limit ; i++) {
	if (atomic_read(&tjl->j_commit_left) ||
	    tjl->j_trans_id < jl->j_trans_id) {
	    break;
	}
	cur_len = atomic_read(&tjl->j_nonzerolen);
	if (cur_len > 0) {
	    tjl->j_state &= ~LIST_TOUCHED;
	}
	len += cur_len;
	flush_jl = tjl;
	if (tjl->j_list.next == &SB_JOURNAL(s)->j_journal_list)
	    break;
	tjl = JOURNAL_LIST_ENTRY(tjl->j_list.next);
    }
    /* try to find a group of blocks we can flush across all the
    ** transactions, but only bother if we've actually spanned
    ** across multiple lists
    */
    if (flush_jl != jl) {
        ret = kupdate_transactions(s, jl, &tjl, &trans_id, len, i);
    }
    flush_journal_list(s, flush_jl, 1);
    return 0;
}

/*
** removes any nodes in table with name block and dev as bh.
** only touchs the hnext and hprev pointers.
*/
void remove_journal_hash(struct super_block *sb,
			struct reiserfs_journal_cnode **table,
			struct reiserfs_journal_list *jl,
			unsigned long block, int remove_freed)
{
  struct reiserfs_journal_cnode *cur ;
  struct reiserfs_journal_cnode **head ;

  head= &(journal_hash(table, sb, block)) ;
  if (!head) {
    return ;
  }
  cur = *head ;
  while(cur) {
    if (cur->blocknr == block && cur->sb == sb && (jl == NULL || jl == cur->jlist) && 
        (!test_bit(BLOCK_FREED, &cur->state) || remove_freed)) {
      if (cur->hnext) {
        cur->hnext->hprev = cur->hprev ;
      }
      if (cur->hprev) {
	cur->hprev->hnext = cur->hnext ;
      } else {
	*head = cur->hnext ;
      }
      cur->blocknr = 0 ;
      cur->sb = NULL ;
      cur->state = 0 ;
      if (cur->bh && cur->jlist) /* anybody who clears the cur->bh will also dec the nonzerolen */
	atomic_dec(&(cur->jlist->j_nonzerolen)) ;
      cur->bh = NULL ;
      cur->jlist = NULL ;
    } 
    cur = cur->hnext ;
  }
}

static void free_journal_ram(struct super_block *p_s_sb) {
  reiserfs_kfree(SB_JOURNAL(p_s_sb)->j_current_jl,
                 sizeof(struct reiserfs_journal_list), p_s_sb);
  SB_JOURNAL(p_s_sb)->j_num_lists--;

  vfree(SB_JOURNAL(p_s_sb)->j_cnode_free_orig) ;
  free_list_bitmaps(p_s_sb, SB_JOURNAL(p_s_sb)->j_list_bitmap) ;
  free_bitmap_nodes(p_s_sb) ; /* must be after free_list_bitmaps */
  if (SB_JOURNAL(p_s_sb)->j_header_bh) {
    brelse(SB_JOURNAL(p_s_sb)->j_header_bh) ;
  }
  /* j_header_bh is on the journal dev, make sure not to release the journal
   * dev until we brelse j_header_bh
   */
  release_journal_dev(p_s_sb, SB_JOURNAL(p_s_sb));
  vfree(SB_JOURNAL(p_s_sb)) ;
}

/*
** call on unmount.  Only set error to 1 if you haven't made your way out
** of read_super() yet.  Any other caller must keep error at 0.
*/
static int do_journal_release(struct reiserfs_transaction_handle *th, struct super_block *p_s_sb, int error) {
  struct reiserfs_transaction_handle myth ;

  /* we only want to flush out transactions if we were called with error == 0
  */
  if (!error && !(p_s_sb->s_flags & MS_RDONLY)) {
    /* end the current trans */
    do_journal_end(th, p_s_sb,10, FLUSH_ALL) ;

    /* make sure something gets logged to force our way into the flush code */
    journal_join(&myth, p_s_sb, 1) ;
    reiserfs_prepare_for_journal(p_s_sb, SB_BUFFER_WITH_SB(p_s_sb), 1) ;
    journal_mark_dirty(&myth, p_s_sb, SB_BUFFER_WITH_SB(p_s_sb)) ;
    do_journal_end(&myth, p_s_sb,1, FLUSH_ALL) ;
  }

  reiserfs_mounted_fs_count-- ;
  /* wait for all commits to finish */
  cancel_delayed_work(&SB_JOURNAL(p_s_sb)->j_work);
  flush_workqueue(commit_wq);
  if (!reiserfs_mounted_fs_count) {
    destroy_workqueue(commit_wq);
    commit_wq = NULL;
  }

  free_journal_ram(p_s_sb) ;

  return 0 ;
}

/*
** call on unmount.  flush all journal trans, release all alloc'd ram
*/
int journal_release(struct reiserfs_transaction_handle *th, struct super_block *p_s_sb) {
  return do_journal_release(th, p_s_sb, 0) ;
}
/*
** only call from an error condition inside reiserfs_read_super!
*/
int journal_release_error(struct reiserfs_transaction_handle *th, struct super_block *p_s_sb) {
  return do_journal_release(th, p_s_sb, 1) ;
}

/* compares description block with commit block.  returns 1 if they differ, 0 if they are the same */
static int journal_compare_desc_commit(struct super_block *p_s_sb, struct reiserfs_journal_desc *desc, 
			               struct reiserfs_journal_commit *commit) {
  if (get_commit_trans_id (commit) != get_desc_trans_id (desc) || 
      get_commit_trans_len (commit) != get_desc_trans_len (desc) || 
      get_commit_trans_len (commit) > SB_JOURNAL_TRANS_MAX(p_s_sb) || 
      get_commit_trans_len (commit) <= 0 
  ) {
    return 1 ;
  }
  return 0 ;
}
/* returns 0 if it did not find a description block  
** returns -1 if it found a corrupt commit block
** returns 1 if both desc and commit were valid 
*/
static int journal_transaction_is_valid(struct super_block *p_s_sb, struct buffer_head *d_bh, unsigned long *oldest_invalid_trans_id, unsigned long *newest_mount_id) {
  struct reiserfs_journal_desc *desc ;
  struct reiserfs_journal_commit *commit ;
  struct buffer_head *c_bh ;
  unsigned long offset ;

  if (!d_bh)
      return 0 ;

  desc = (struct reiserfs_journal_desc *)d_bh->b_data ;
  if (get_desc_trans_len(desc) > 0 && !memcmp(get_journal_desc_magic (d_bh), JOURNAL_DESC_MAGIC, 8)) {
    if (oldest_invalid_trans_id && *oldest_invalid_trans_id && get_desc_trans_id(desc) > *oldest_invalid_trans_id) {
      reiserfs_debug(p_s_sb, REISERFS_DEBUG_CODE, "journal-986: transaction "
	              "is valid returning because trans_id %d is greater than "
		      "oldest_invalid %lu", get_desc_trans_id(desc),
		       *oldest_invalid_trans_id);
      return 0 ;
    }
    if (newest_mount_id && *newest_mount_id > get_desc_mount_id (desc)) {
      reiserfs_debug(p_s_sb, REISERFS_DEBUG_CODE, "journal-1087: transaction "
                     "is valid returning because mount_id %d is less than "
		     "newest_mount_id %lu", get_desc_mount_id (desc),
		     *newest_mount_id) ;
      return -1 ;
    }
    if ( get_desc_trans_len(desc) > SB_JOURNAL_TRANS_MAX(p_s_sb) ) {
      reiserfs_warning(p_s_sb, "journal-2018: Bad transaction length %d encountered, ignoring transaction", get_desc_trans_len(desc));
      return -1 ;
    }
    offset = d_bh->b_blocknr - SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb) ;

    /* ok, we have a journal description block, lets see if the transaction was valid */
    c_bh = journal_bread(p_s_sb, SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb) +
		 ((offset + get_desc_trans_len(desc) + 1) % SB_ONDISK_JOURNAL_SIZE(p_s_sb))) ;
    if (!c_bh)
      return 0 ;
    commit = (struct reiserfs_journal_commit *)c_bh->b_data ;
    if (journal_compare_desc_commit(p_s_sb, desc, commit)) {
      reiserfs_debug(p_s_sb, REISERFS_DEBUG_CODE, 
                     "journal_transaction_is_valid, commit offset %ld had bad "
		     "time %d or length %d",
		     c_bh->b_blocknr -  SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb),
		     get_commit_trans_id (commit), 
		     get_commit_trans_len(commit));
      brelse(c_bh) ;
      if (oldest_invalid_trans_id) {
	*oldest_invalid_trans_id = get_desc_trans_id(desc) ;
	reiserfs_debug(p_s_sb, REISERFS_DEBUG_CODE, "journal-1004: "
		       "transaction_is_valid setting oldest invalid trans_id "
		       "to %d", get_desc_trans_id(desc)) ;
      }
      return -1; 
    }
    brelse(c_bh) ;
    reiserfs_debug(p_s_sb, REISERFS_DEBUG_CODE, "journal-1006: found valid "
                   "transaction start offset %llu, len %d id %d",
		   d_bh->b_blocknr - SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb), 
		   get_desc_trans_len(desc), get_desc_trans_id(desc)) ;
    return 1 ;
  } else {
    return 0 ;
  }
}

static void brelse_array(struct buffer_head **heads, int num) {
  int i ;
  for (i = 0 ; i < num ; i++) {
    brelse(heads[i]) ;
  }
}

/*
** given the start, and values for the oldest acceptable transactions,
** this either reads in a replays a transaction, or returns because the transaction
** is invalid, or too old.
*/
static int journal_read_transaction(struct super_block *p_s_sb, unsigned long cur_dblock, unsigned long oldest_start, 
				    unsigned long oldest_trans_id, unsigned long newest_mount_id) {
  struct reiserfs_journal_desc *desc ;
  struct reiserfs_journal_commit *commit ;
  unsigned long trans_id = 0 ;
  struct buffer_head *c_bh ;
  struct buffer_head *d_bh ;
  struct buffer_head **log_blocks = NULL ;
  struct buffer_head **real_blocks = NULL ;
  unsigned long trans_offset ;
  int i;
  int trans_half;

  d_bh = journal_bread(p_s_sb, cur_dblock) ;
  if (!d_bh)
    return 1 ;
  desc = (struct reiserfs_journal_desc *)d_bh->b_data ;
  trans_offset = d_bh->b_blocknr - SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb) ;
  reiserfs_debug(p_s_sb, REISERFS_DEBUG_CODE, "journal-1037: "
                 "journal_read_transaction, offset %llu, len %d mount_id %d",
		 d_bh->b_blocknr - SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb), 
		 get_desc_trans_len(desc), get_desc_mount_id(desc)) ;
  if (get_desc_trans_id(desc) < oldest_trans_id) {
    reiserfs_debug(p_s_sb, REISERFS_DEBUG_CODE, "journal-1039: "
                   "journal_read_trans skipping because %lu is too old",
		   cur_dblock - SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb)) ;
    brelse(d_bh) ;
    return 1 ;
  }
  if (get_desc_mount_id(desc) != newest_mount_id) {
    reiserfs_debug(p_s_sb, REISERFS_DEBUG_CODE, "journal-1146: "
                   "journal_read_trans skipping because %d is != "
		   "newest_mount_id %lu", get_desc_mount_id(desc),
		    newest_mount_id) ;
    brelse(d_bh) ;
    return 1 ;
  }
  c_bh = journal_bread(p_s_sb, SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb) +
		((trans_offset + get_desc_trans_len(desc) + 1) % 
		 SB_ONDISK_JOURNAL_SIZE(p_s_sb))) ;
  if (!c_bh) {
    brelse(d_bh) ;
    return 1 ;
  }
  commit = (struct reiserfs_journal_commit *)c_bh->b_data ;
  if (journal_compare_desc_commit(p_s_sb, desc, commit)) {
    reiserfs_debug(p_s_sb, REISERFS_DEBUG_CODE, "journal_read_transaction, "
                   "commit offset %llu had bad time %d or length %d",
		   c_bh->b_blocknr -  SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb), 
		   get_commit_trans_id(commit), get_commit_trans_len(commit));
    brelse(c_bh) ;
    brelse(d_bh) ;
    return 1; 
  }
  trans_id = get_desc_trans_id(desc) ;
  /* now we know we've got a good transaction, and it was inside the valid time ranges */
  log_blocks = reiserfs_kmalloc(get_desc_trans_len(desc) * sizeof(struct buffer_head *), GFP_NOFS, p_s_sb) ;
  real_blocks = reiserfs_kmalloc(get_desc_trans_len(desc) * sizeof(struct buffer_head *), GFP_NOFS, p_s_sb) ;
  if (!log_blocks  || !real_blocks) {
    brelse(c_bh) ;
    brelse(d_bh) ;
    reiserfs_kfree(log_blocks, get_desc_trans_len(desc) * sizeof(struct buffer_head *), p_s_sb) ;
    reiserfs_kfree(real_blocks, get_desc_trans_len(desc) * sizeof(struct buffer_head *), p_s_sb) ;
    reiserfs_warning(p_s_sb, "journal-1169: kmalloc failed, unable to mount FS") ;
    return -1 ;
  }
  /* get all the buffer heads */
  trans_half = journal_trans_half (p_s_sb->s_blocksize) ;
  for(i = 0 ; i < get_desc_trans_len(desc) ; i++) {
    log_blocks[i] =  journal_getblk(p_s_sb,  SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb) + (trans_offset + 1 + i) % SB_ONDISK_JOURNAL_SIZE(p_s_sb));
    if (i < trans_half) {
      real_blocks[i] = sb_getblk(p_s_sb, le32_to_cpu(desc->j_realblock[i])) ;
    } else {
      real_blocks[i] = sb_getblk(p_s_sb, le32_to_cpu(commit->j_realblock[i - trans_half])) ;
    }
    if ( real_blocks[i]->b_blocknr > SB_BLOCK_COUNT(p_s_sb) ) {
      reiserfs_warning(p_s_sb, "journal-1207: REPLAY FAILURE fsck required! Block to replay is outside of filesystem");
      goto abort_replay;
    }
    /* make sure we don't try to replay onto log or reserved area */
    if (is_block_in_log_or_reserved_area(p_s_sb, real_blocks[i]->b_blocknr)) {
      reiserfs_warning(p_s_sb, "journal-1204: REPLAY FAILURE fsck required! Trying to replay onto a log block") ;
abort_replay:
      brelse_array(log_blocks, i) ;
      brelse_array(real_blocks, i) ;
      brelse(c_bh) ;
      brelse(d_bh) ;
      reiserfs_kfree(log_blocks, get_desc_trans_len(desc) * sizeof(struct buffer_head *), p_s_sb) ;
      reiserfs_kfree(real_blocks, get_desc_trans_len(desc) * sizeof(struct buffer_head *), p_s_sb) ;
      return -1 ;
    }
  }
  /* read in the log blocks, memcpy to the corresponding real block */
  ll_rw_block(READ, get_desc_trans_len(desc), log_blocks) ;
  for (i = 0 ; i < get_desc_trans_len(desc) ; i++) {
    wait_on_buffer(log_blocks[i]) ;
    if (!buffer_uptodate(log_blocks[i])) {
      reiserfs_warning(p_s_sb, "journal-1212: REPLAY FAILURE fsck required! buffer write failed") ;
      brelse_array(log_blocks + i, get_desc_trans_len(desc) - i) ;
      brelse_array(real_blocks, get_desc_trans_len(desc)) ;
      brelse(c_bh) ;
      brelse(d_bh) ;
      reiserfs_kfree(log_blocks, get_desc_trans_len(desc) * sizeof(struct buffer_head *), p_s_sb) ;
      reiserfs_kfree(real_blocks, get_desc_trans_len(desc) * sizeof(struct buffer_head *), p_s_sb) ;
      return -1 ;
    }
    memcpy(real_blocks[i]->b_data, log_blocks[i]->b_data, real_blocks[i]->b_size) ;
    set_buffer_uptodate(real_blocks[i]) ;
    brelse(log_blocks[i]) ;
  }
  /* flush out the real blocks */
  for (i = 0 ; i < get_desc_trans_len(desc) ; i++) {
    set_buffer_dirty(real_blocks[i]) ;
    ll_rw_block(WRITE, 1, real_blocks + i) ;
  }
  for (i = 0 ; i < get_desc_trans_len(desc) ; i++) {
    wait_on_buffer(real_blocks[i]) ; 
    if (!buffer_uptodate(real_blocks[i])) {
      reiserfs_warning(p_s_sb, "journal-1226: REPLAY FAILURE, fsck required! buffer write failed") ;
      brelse_array(real_blocks + i, get_desc_trans_len(desc) - i) ;
      brelse(c_bh) ;
      brelse(d_bh) ;
      reiserfs_kfree(log_blocks, get_desc_trans_len(desc) * sizeof(struct buffer_head *), p_s_sb) ;
      reiserfs_kfree(real_blocks, get_desc_trans_len(desc) * sizeof(struct buffer_head *), p_s_sb) ;
      return -1 ;
    }
    brelse(real_blocks[i]) ;
  }
  cur_dblock =  SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb) + ((trans_offset + get_desc_trans_len(desc) + 2) % SB_ONDISK_JOURNAL_SIZE(p_s_sb)) ;
  reiserfs_debug(p_s_sb, REISERFS_DEBUG_CODE, "journal-1095: setting journal "
                 "start to offset %ld",
		 cur_dblock -  SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb)) ;
  
  /* init starting values for the first transaction, in case this is the last transaction to be replayed. */
  SB_JOURNAL(p_s_sb)->j_start = cur_dblock - SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb) ;
  SB_JOURNAL(p_s_sb)->j_last_flush_trans_id = trans_id ;
  SB_JOURNAL(p_s_sb)->j_trans_id = trans_id + 1;
  brelse(c_bh) ;
  brelse(d_bh) ;
  reiserfs_kfree(log_blocks, le32_to_cpu(desc->j_len) * sizeof(struct buffer_head *), p_s_sb) ;
  reiserfs_kfree(real_blocks, le32_to_cpu(desc->j_len) * sizeof(struct buffer_head *), p_s_sb) ;
  return 0 ;
}

/* This function reads blocks starting from block and to max_block of bufsize
   size (but no more than BUFNR blocks at a time). This proved to improve
   mounting speed on self-rebuilding raid5 arrays at least.
   Right now it is only used from journal code. But later we might use it
   from other places.
   Note: Do not use journal_getblk/sb_getblk functions here! */
struct buffer_head * reiserfs_breada (struct block_device *dev, int block, int bufsize,
			    unsigned int max_block)
{
	struct buffer_head * bhlist[BUFNR];
	unsigned int blocks = BUFNR;
	struct buffer_head * bh;
	int i, j;
	
	bh = __getblk (dev, block, bufsize );
	if (buffer_uptodate (bh))
		return (bh);   
		
	if (block + BUFNR > max_block) {
		blocks = max_block - block;
	}
	bhlist[0] = bh;
	j = 1;
	for (i = 1; i < blocks; i++) {
		bh = __getblk (dev, block + i, bufsize);
		if (buffer_uptodate (bh)) {
			brelse (bh);
			break;
		}
		else bhlist[j++] = bh;
	}
	ll_rw_block (READ, j, bhlist);
	for(i = 1; i < j; i++) 
		brelse (bhlist[i]);
	bh = bhlist[0];
	wait_on_buffer (bh);
	if (buffer_uptodate (bh))
		return bh;
	brelse (bh);
	return NULL;
}

/*
** read and replay the log
** on a clean unmount, the journal header's next unflushed pointer will be to an invalid
** transaction.  This tests that before finding all the transactions in the log, which makes normal mount times fast.
**
** After a crash, this starts with the next unflushed transaction, and replays until it finds one too old, or invalid.
**
** On exit, it sets things up so the first transaction will work correctly.
*/
static int journal_read(struct super_block *p_s_sb) {
  struct reiserfs_journal_desc *desc ;
  unsigned long oldest_trans_id = 0;
  unsigned long oldest_invalid_trans_id = 0 ;
  time_t start ;
  unsigned long oldest_start = 0;
  unsigned long cur_dblock = 0 ;
  unsigned long newest_mount_id = 9 ;
  struct buffer_head *d_bh ;
  struct reiserfs_journal_header *jh ;
  int valid_journal_header = 0 ;
  int replay_count = 0 ;
  int continue_replay = 1 ;
  int ret ;
  char b[BDEVNAME_SIZE];

  cur_dblock = SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb) ;
  reiserfs_info (p_s_sb, "checking transaction log (%s)\n",
	 bdevname(SB_JOURNAL(p_s_sb)->j_dev_bd, b));
  start = get_seconds();

  /* step 1, read in the journal header block.  Check the transaction it says 
  ** is the first unflushed, and if that transaction is not valid, 
  ** replay is done
  */
  SB_JOURNAL(p_s_sb)->j_header_bh = journal_bread(p_s_sb,
					   SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb) + 
					   SB_ONDISK_JOURNAL_SIZE(p_s_sb));
  if (!SB_JOURNAL(p_s_sb)->j_header_bh) {
    return 1 ;
  }
  jh = (struct reiserfs_journal_header *)(SB_JOURNAL(p_s_sb)->j_header_bh->b_data) ;
  if (le32_to_cpu(jh->j_first_unflushed_offset) >= 0 && 
      le32_to_cpu(jh->j_first_unflushed_offset) < SB_ONDISK_JOURNAL_SIZE(p_s_sb) && 
      le32_to_cpu(jh->j_last_flush_trans_id) > 0) {
    oldest_start = SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb) + 
                       le32_to_cpu(jh->j_first_unflushed_offset) ;
    oldest_trans_id = le32_to_cpu(jh->j_last_flush_trans_id) + 1;
    newest_mount_id = le32_to_cpu(jh->j_mount_id);
    reiserfs_debug(p_s_sb, REISERFS_DEBUG_CODE, "journal-1153: found in "
                   "header: first_unflushed_offset %d, last_flushed_trans_id "
		   "%lu", le32_to_cpu(jh->j_first_unflushed_offset),
		   le32_to_cpu(jh->j_last_flush_trans_id)) ;
    valid_journal_header = 1 ;

    /* now, we try to read the first unflushed offset.  If it is not valid, 
    ** there is nothing more we can do, and it makes no sense to read 
    ** through the whole log.
    */
    d_bh = journal_bread(p_s_sb, SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb) + le32_to_cpu(jh->j_first_unflushed_offset)) ;
    ret = journal_transaction_is_valid(p_s_sb, d_bh, NULL, NULL) ;
    if (!ret) {
      continue_replay = 0 ;
    }
    brelse(d_bh) ;
    goto start_log_replay;
  }

  if (continue_replay && bdev_read_only(p_s_sb->s_bdev)) {
    reiserfs_warning (p_s_sb,
		      "clm-2076: device is readonly, unable to replay log") ;
    return -1 ;
  }

  /* ok, there are transactions that need to be replayed.  start with the first log block, find
  ** all the valid transactions, and pick out the oldest.
  */
  while(continue_replay && cur_dblock < (SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb) + SB_ONDISK_JOURNAL_SIZE(p_s_sb))) {
    /* Note that it is required for blocksize of primary fs device and journal
       device to be the same */
    d_bh = reiserfs_breada(SB_JOURNAL(p_s_sb)->j_dev_bd, cur_dblock, p_s_sb->s_blocksize,
			   SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb) + SB_ONDISK_JOURNAL_SIZE(p_s_sb)) ;
    ret = journal_transaction_is_valid(p_s_sb, d_bh, &oldest_invalid_trans_id, &newest_mount_id) ;
    if (ret == 1) {
      desc = (struct reiserfs_journal_desc *)d_bh->b_data ;
      if (oldest_start == 0) { /* init all oldest_ values */
        oldest_trans_id = get_desc_trans_id(desc) ;
	oldest_start = d_bh->b_blocknr ;
	newest_mount_id = get_desc_mount_id(desc) ;
	reiserfs_debug(p_s_sb, REISERFS_DEBUG_CODE, "journal-1179: Setting "
	               "oldest_start to offset %llu, trans_id %lu",
		       oldest_start - SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb), 
		       oldest_trans_id) ;
      } else if (oldest_trans_id > get_desc_trans_id(desc)) { 
        /* one we just read was older */
        oldest_trans_id = get_desc_trans_id(desc) ;
	oldest_start = d_bh->b_blocknr ;
	reiserfs_debug(p_s_sb, REISERFS_DEBUG_CODE, "journal-1180: Resetting "
	               "oldest_start to offset %lu, trans_id %lu",
			oldest_start - SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb), 
			oldest_trans_id) ;
      }
      if (newest_mount_id < get_desc_mount_id(desc)) {
        newest_mount_id = get_desc_mount_id(desc) ;
	reiserfs_debug(p_s_sb, REISERFS_DEBUG_CODE, "journal-1299: Setting "
	              "newest_mount_id to %d", get_desc_mount_id(desc));
      }
      cur_dblock += get_desc_trans_len(desc) + 2 ;
    } else {
      cur_dblock++ ;
    }
    brelse(d_bh) ;
  }

start_log_replay:
  cur_dblock = oldest_start ;
  if (oldest_trans_id)  {
    reiserfs_debug(p_s_sb, REISERFS_DEBUG_CODE, "journal-1206: Starting replay "
                   "from offset %llu, trans_id %lu",
		   cur_dblock - SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb), 
		   oldest_trans_id) ;

  }
  replay_count = 0 ;
  while(continue_replay && oldest_trans_id > 0) {
    ret = journal_read_transaction(p_s_sb, cur_dblock, oldest_start, oldest_trans_id, newest_mount_id) ;
    if (ret < 0) {
      return ret ;
    } else if (ret != 0) {
      break ;
    }
    cur_dblock = SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb) + SB_JOURNAL(p_s_sb)->j_start ;
    replay_count++ ;
   if (cur_dblock == oldest_start)
        break;
  }

  if (oldest_trans_id == 0) {
    reiserfs_debug(p_s_sb, REISERFS_DEBUG_CODE, "journal-1225: No valid "
                   "transactions found") ;
  }
  /* j_start does not get set correctly if we don't replay any transactions.
  ** if we had a valid journal_header, set j_start to the first unflushed transaction value,
  ** copy the trans_id from the header
  */
  if (valid_journal_header && replay_count == 0) { 
    SB_JOURNAL(p_s_sb)->j_start = le32_to_cpu(jh->j_first_unflushed_offset) ;
    SB_JOURNAL(p_s_sb)->j_trans_id = le32_to_cpu(jh->j_last_flush_trans_id) + 1;
    SB_JOURNAL(p_s_sb)->j_last_flush_trans_id = le32_to_cpu(jh->j_last_flush_trans_id) ;
    SB_JOURNAL(p_s_sb)->j_mount_id = le32_to_cpu(jh->j_mount_id) + 1;
  } else {
    SB_JOURNAL(p_s_sb)->j_mount_id = newest_mount_id + 1 ;
  }
  reiserfs_debug(p_s_sb, REISERFS_DEBUG_CODE, "journal-1299: Setting "
                 "newest_mount_id to %lu", SB_JOURNAL(p_s_sb)->j_mount_id) ;
  SB_JOURNAL(p_s_sb)->j_first_unflushed_offset = SB_JOURNAL(p_s_sb)->j_start ; 
  if (replay_count > 0) {
    reiserfs_info (p_s_sb, "replayed %d transactions in %lu seconds\n",
		   replay_count, get_seconds() - start) ;
  }
  if (!bdev_read_only(p_s_sb->s_bdev) && 
       _update_journal_header_block(p_s_sb, SB_JOURNAL(p_s_sb)->j_start, 
                                   SB_JOURNAL(p_s_sb)->j_last_flush_trans_id))
  {
      /* replay failed, caller must call free_journal_ram and abort
      ** the mount
      */
      return -1 ;
  }
  return 0 ;
}

static struct reiserfs_journal_list *alloc_journal_list(struct super_block *s)
{
    struct reiserfs_journal_list *jl;
retry:
    jl = reiserfs_kmalloc(sizeof(struct reiserfs_journal_list), GFP_NOFS, s);
    if (!jl) {
	yield();
	goto retry;
    }
    memset(jl, 0, sizeof(*jl));
    INIT_LIST_HEAD(&jl->j_list);
    INIT_LIST_HEAD(&jl->j_working_list);
    INIT_LIST_HEAD(&jl->j_tail_bh_list);
    INIT_LIST_HEAD(&jl->j_bh_list);
    sema_init(&jl->j_commit_lock, 1);
    SB_JOURNAL(s)->j_num_lists++;
    get_journal_list(jl);
    return jl;
}

static void journal_list_init(struct super_block *p_s_sb) {
    SB_JOURNAL(p_s_sb)->j_current_jl = alloc_journal_list(p_s_sb);
}

static int release_journal_dev( struct super_block *super,
				struct reiserfs_journal *journal )
{
    int result;
    
    result = 0;

    if( journal -> j_dev_file != NULL ) {
	result = filp_close( journal -> j_dev_file, NULL );
	journal -> j_dev_file = NULL;
	journal -> j_dev_bd = NULL;
    } else if( journal -> j_dev_bd != NULL ) {
	result = blkdev_put( journal -> j_dev_bd );
	journal -> j_dev_bd = NULL;
    }

    if( result != 0 ) {
	reiserfs_warning(super, "sh-457: release_journal_dev: Cannot release journal device: %i", result );
    }
    return result;
}

static int journal_init_dev( struct super_block *super, 
			     struct reiserfs_journal *journal, 
			     const char *jdev_name )
{
	int result;
	dev_t jdev;
	int blkdev_mode = FMODE_READ | FMODE_WRITE;
	char b[BDEVNAME_SIZE];

	result = 0;

	journal -> j_dev_bd = NULL;
	journal -> j_dev_file = NULL;
	jdev = SB_ONDISK_JOURNAL_DEVICE( super ) ?
		new_decode_dev(SB_ONDISK_JOURNAL_DEVICE(super)) : super->s_dev;	

	if (bdev_read_only(super->s_bdev))
	    blkdev_mode = FMODE_READ;

	/* there is no "jdev" option and journal is on separate device */
	if( ( !jdev_name || !jdev_name[ 0 ] ) ) {
		journal->j_dev_bd = open_by_devnum(jdev, blkdev_mode);
		if (IS_ERR(journal->j_dev_bd)) {
			result = PTR_ERR(journal->j_dev_bd);
			journal->j_dev_bd = NULL;
			reiserfs_warning (super, "sh-458: journal_init_dev: "
					  "cannot init journal device '%s': %i",
					  __bdevname(jdev, b), result );
			return result;
		} else if (jdev != super->s_dev)
			set_blocksize(journal->j_dev_bd, super->s_blocksize);
		return 0;
	}

	journal -> j_dev_file = filp_open( jdev_name, 0, 0 );
	if( !IS_ERR( journal -> j_dev_file ) ) {
		struct inode *jdev_inode = journal->j_dev_file->f_mapping->host;
		if( !S_ISBLK( jdev_inode -> i_mode ) ) {
			reiserfs_warning  (super, "journal_init_dev: '%s' is "
					   "not a block device", jdev_name );
			result = -ENOTBLK;
		} else  {
			/* ok */
			journal->j_dev_bd = I_BDEV(jdev_inode);
			set_blocksize(journal->j_dev_bd, super->s_blocksize);
		}
	} else {
		result = PTR_ERR( journal -> j_dev_file );
		journal -> j_dev_file = NULL;
		reiserfs_warning (super,
				  "journal_init_dev: Cannot open '%s': %i",
				  jdev_name, result );
	}
	if( result != 0 ) {
		release_journal_dev( super, journal );
	}
	reiserfs_info(super, "journal_init_dev: journal device: %s\n",
		bdevname(journal->j_dev_bd, b));
	return result;
}

/*
** must be called once on fs mount.  calls journal_read for you
*/
int journal_init(struct super_block *p_s_sb, const char * j_dev_name, int old_format, unsigned int commit_max_age) {
    int num_cnodes = SB_ONDISK_JOURNAL_SIZE(p_s_sb) * 2 ;
    struct buffer_head *bhjh;
    struct reiserfs_super_block * rs;
    struct reiserfs_journal_header *jh;
    struct reiserfs_journal *journal;
    struct reiserfs_journal_list *jl;
    char b[BDEVNAME_SIZE];

    journal = SB_JOURNAL(p_s_sb) = vmalloc(sizeof (struct reiserfs_journal)) ;
    if (!journal) {
	reiserfs_warning (p_s_sb, "journal-1256: unable to get memory for journal structure") ;
	return 1 ;
    }
    memset(journal, 0, sizeof(struct reiserfs_journal)) ;
    INIT_LIST_HEAD(&SB_JOURNAL(p_s_sb)->j_bitmap_nodes) ;
    INIT_LIST_HEAD (&SB_JOURNAL(p_s_sb)->j_prealloc_list);
    INIT_LIST_HEAD(&SB_JOURNAL(p_s_sb)->j_working_list);
    INIT_LIST_HEAD(&SB_JOURNAL(p_s_sb)->j_journal_list);
    if (reiserfs_allocate_list_bitmaps(p_s_sb,
				       SB_JOURNAL(p_s_sb)->j_list_bitmap,
 				       SB_BMAP_NR(p_s_sb)))
	goto free_and_return ;
    allocate_bitmap_nodes(p_s_sb) ;

    /* reserved for journal area support */
    SB_JOURNAL_1st_RESERVED_BLOCK(p_s_sb) = (old_format ?
					     REISERFS_OLD_DISK_OFFSET_IN_BYTES / p_s_sb->s_blocksize +
					     SB_BMAP_NR(p_s_sb) + 1 :
					     REISERFS_DISK_OFFSET_IN_BYTES / p_s_sb->s_blocksize + 2); 
    
    /* Sanity check to see is the standard journal fitting withing first bitmap
       (actual for small blocksizes) */
    if ( !SB_ONDISK_JOURNAL_DEVICE( p_s_sb ) &&
         (SB_JOURNAL_1st_RESERVED_BLOCK(p_s_sb) + SB_ONDISK_JOURNAL_SIZE(p_s_sb) > p_s_sb->s_blocksize * 8) ) {
	reiserfs_warning (p_s_sb, "journal-1393: journal does not fit for area "
			  "addressed by first of bitmap blocks. It starts at "
			  "%u and its size is %u. Block size %ld",
			  SB_JOURNAL_1st_RESERVED_BLOCK(p_s_sb),
			  SB_ONDISK_JOURNAL_SIZE(p_s_sb), p_s_sb->s_blocksize);
	goto free_and_return;
    }

    if( journal_init_dev( p_s_sb, journal, j_dev_name ) != 0 ) {
      reiserfs_warning (p_s_sb, "sh-462: unable to initialize jornal device");
      goto free_and_return;
    }

     rs = SB_DISK_SUPER_BLOCK(p_s_sb);
     
     /* read journal header */
     bhjh = journal_bread(p_s_sb,
		   SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb) + SB_ONDISK_JOURNAL_SIZE(p_s_sb));
     if (!bhjh) {
	 reiserfs_warning (p_s_sb, "sh-459: unable to read journal header");
	 goto free_and_return;
     }
     jh = (struct reiserfs_journal_header *)(bhjh->b_data);
     
     /* make sure that journal matches to the super block */
     if (is_reiserfs_jr(rs) && (jh->jh_journal.jp_journal_magic != sb_jp_journal_magic(rs))) {
	 reiserfs_warning (p_s_sb, "sh-460: journal header magic %x "
			   "(device %s) does not match to magic found in super "
			   "block %x",
			   jh->jh_journal.jp_journal_magic,
			   bdevname( SB_JOURNAL(p_s_sb)->j_dev_bd, b),
			   sb_jp_journal_magic(rs));
	 brelse (bhjh);
	 goto free_and_return;
  }
     
  SB_JOURNAL_TRANS_MAX(p_s_sb)      = le32_to_cpu (jh->jh_journal.jp_journal_trans_max);
  SB_JOURNAL_MAX_BATCH(p_s_sb)      = le32_to_cpu (jh->jh_journal.jp_journal_max_batch);
  SB_JOURNAL_MAX_COMMIT_AGE(p_s_sb) = le32_to_cpu (jh->jh_journal.jp_journal_max_commit_age);
  SB_JOURNAL_MAX_TRANS_AGE(p_s_sb)  = JOURNAL_MAX_TRANS_AGE;

  if (SB_JOURNAL_TRANS_MAX(p_s_sb)) {
    /* make sure these parameters are available, assign it if they are not */
    __u32 initial = SB_JOURNAL_TRANS_MAX(p_s_sb);
    __u32 ratio = 1;
    
    if (p_s_sb->s_blocksize < 4096)
      ratio = 4096 / p_s_sb->s_blocksize;
    
    if (SB_ONDISK_JOURNAL_SIZE(p_s_sb)/SB_JOURNAL_TRANS_MAX(p_s_sb) < JOURNAL_MIN_RATIO)
      SB_JOURNAL_TRANS_MAX(p_s_sb) = SB_ONDISK_JOURNAL_SIZE(p_s_sb) / JOURNAL_MIN_RATIO;
    if (SB_JOURNAL_TRANS_MAX(p_s_sb) > JOURNAL_TRANS_MAX_DEFAULT / ratio)
      SB_JOURNAL_TRANS_MAX(p_s_sb) = JOURNAL_TRANS_MAX_DEFAULT / ratio;
    if (SB_JOURNAL_TRANS_MAX(p_s_sb) < JOURNAL_TRANS_MIN_DEFAULT / ratio)
      SB_JOURNAL_TRANS_MAX(p_s_sb) = JOURNAL_TRANS_MIN_DEFAULT / ratio;
    
    if (SB_JOURNAL_TRANS_MAX(p_s_sb) != initial)
      reiserfs_warning (p_s_sb, "sh-461: journal_init: wrong transaction max size (%u). Changed to %u",
	      initial, SB_JOURNAL_TRANS_MAX(p_s_sb));

    SB_JOURNAL_MAX_BATCH(p_s_sb) = SB_JOURNAL_TRANS_MAX(p_s_sb)*
      JOURNAL_MAX_BATCH_DEFAULT/JOURNAL_TRANS_MAX_DEFAULT;
  }  
  
  if (!SB_JOURNAL_TRANS_MAX(p_s_sb)) {
    /*we have the file system was created by old version of mkreiserfs 
      so this field contains zero value */
    SB_JOURNAL_TRANS_MAX(p_s_sb)      = JOURNAL_TRANS_MAX_DEFAULT ;
    SB_JOURNAL_MAX_BATCH(p_s_sb)      = JOURNAL_MAX_BATCH_DEFAULT ;  
    SB_JOURNAL_MAX_COMMIT_AGE(p_s_sb) = JOURNAL_MAX_COMMIT_AGE ;
    
    /* for blocksize >= 4096 - max transaction size is 1024. For block size < 4096
       trans max size is decreased proportionally */
    if (p_s_sb->s_blocksize < 4096) {
      SB_JOURNAL_TRANS_MAX(p_s_sb) /= (4096 / p_s_sb->s_blocksize) ;
      SB_JOURNAL_MAX_BATCH(p_s_sb) = (SB_JOURNAL_TRANS_MAX(p_s_sb)) * 9 / 10 ;
    }
  }

  SB_JOURNAL_DEFAULT_MAX_COMMIT_AGE(p_s_sb) = SB_JOURNAL_MAX_COMMIT_AGE(p_s_sb);

  if (commit_max_age != 0) {
      SB_JOURNAL_MAX_COMMIT_AGE(p_s_sb) = commit_max_age;
      SB_JOURNAL_MAX_TRANS_AGE(p_s_sb) = commit_max_age;
  }

  reiserfs_info (p_s_sb, "journal params: device %s, size %u, "
		 "journal first block %u, max trans len %u, max batch %u, "
		 "max commit age %u, max trans age %u\n",
		 bdevname( SB_JOURNAL(p_s_sb)->j_dev_bd, b),
		 SB_ONDISK_JOURNAL_SIZE(p_s_sb),
		 SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb),
		 SB_JOURNAL_TRANS_MAX(p_s_sb),
		 SB_JOURNAL_MAX_BATCH(p_s_sb),
		 SB_JOURNAL_MAX_COMMIT_AGE(p_s_sb),
		 SB_JOURNAL_MAX_TRANS_AGE(p_s_sb));

  brelse (bhjh);
     
  SB_JOURNAL(p_s_sb)->j_list_bitmap_index = 0 ;
  journal_list_init(p_s_sb) ;

  memset(SB_JOURNAL(p_s_sb)->j_list_hash_table, 0, JOURNAL_HASH_SIZE * sizeof(struct reiserfs_journal_cnode *)) ;

  INIT_LIST_HEAD(&SB_JOURNAL(p_s_sb)->j_dirty_buffers) ;
  spin_lock_init(&SB_JOURNAL(p_s_sb)->j_dirty_buffers_lock) ;

  SB_JOURNAL(p_s_sb)->j_start = 0 ;
  SB_JOURNAL(p_s_sb)->j_len = 0 ;
  SB_JOURNAL(p_s_sb)->j_len_alloc = 0 ;
  atomic_set(&(SB_JOURNAL(p_s_sb)->j_wcount), 0) ;
  atomic_set(&(SB_JOURNAL(p_s_sb)->j_async_throttle), 0) ;
  SB_JOURNAL(p_s_sb)->j_bcount = 0 ;	  
  SB_JOURNAL(p_s_sb)->j_trans_start_time = 0 ;	  
  SB_JOURNAL(p_s_sb)->j_last = NULL ;	  
  SB_JOURNAL(p_s_sb)->j_first = NULL ;     
  init_waitqueue_head(&(SB_JOURNAL(p_s_sb)->j_join_wait)) ;
  sema_init(&SB_JOURNAL(p_s_sb)->j_lock, 1);
  sema_init(&SB_JOURNAL(p_s_sb)->j_flush_sem, 1);

  SB_JOURNAL(p_s_sb)->j_trans_id = 10 ;  
  SB_JOURNAL(p_s_sb)->j_mount_id = 10 ; 
  SB_JOURNAL(p_s_sb)->j_state = 0 ;
  atomic_set(&(SB_JOURNAL(p_s_sb)->j_jlock), 0) ;
  SB_JOURNAL(p_s_sb)->j_cnode_free_list = allocate_cnodes(num_cnodes) ;
  SB_JOURNAL(p_s_sb)->j_cnode_free_orig = SB_JOURNAL(p_s_sb)->j_cnode_free_list ;
  SB_JOURNAL(p_s_sb)->j_cnode_free = SB_JOURNAL(p_s_sb)->j_cnode_free_list ? num_cnodes : 0 ;
  SB_JOURNAL(p_s_sb)->j_cnode_used = 0 ;
  SB_JOURNAL(p_s_sb)->j_must_wait = 0 ;

  init_journal_hash(p_s_sb) ;
  jl = SB_JOURNAL(p_s_sb)->j_current_jl;
  jl->j_list_bitmap = get_list_bitmap(p_s_sb, jl);
  if (!jl->j_list_bitmap) {
    reiserfs_warning(p_s_sb, "journal-2005, get_list_bitmap failed for journal list 0") ;
    goto free_and_return;
  }
  if (journal_read(p_s_sb) < 0) {
    reiserfs_warning(p_s_sb, "Replay Failure, unable to mount") ;
    goto free_and_return;
  }

  reiserfs_mounted_fs_count++ ;
  if (reiserfs_mounted_fs_count <= 1)
    commit_wq = create_workqueue("reiserfs");

  INIT_WORK(&journal->j_work, flush_async_commits, p_s_sb);
  return 0 ;
free_and_return:
  free_journal_ram(p_s_sb);
  return 1;
}

/*
** test for a polite end of the current transaction.  Used by file_write, and should
** be used by delete to make sure they don't write more than can fit inside a single
** transaction
*/
int journal_transaction_should_end(struct reiserfs_transaction_handle *th, int new_alloc) {
  time_t now = get_seconds() ;
  /* cannot restart while nested */
  if (th->t_refcount > 1)
    return 0 ;
  if ( SB_JOURNAL(th->t_super)->j_must_wait > 0 ||
       (SB_JOURNAL(th->t_super)->j_len_alloc + new_alloc) >= SB_JOURNAL_MAX_BATCH(th->t_super) || 
       atomic_read(&(SB_JOURNAL(th->t_super)->j_jlock)) ||
      (now - SB_JOURNAL(th->t_super)->j_trans_start_time) > SB_JOURNAL_MAX_TRANS_AGE(th->t_super) ||
       SB_JOURNAL(th->t_super)->j_cnode_free < (SB_JOURNAL_TRANS_MAX(th->t_super) * 3)) { 
    return 1 ;
  }
  return 0 ;
}

/* this must be called inside a transaction, and requires the 
** kernel_lock to be held
*/
void reiserfs_block_writes(struct reiserfs_transaction_handle *th) {
    struct super_block *s = th->t_super ;
    SB_JOURNAL(s)->j_must_wait = 1 ;
    set_bit(WRITERS_BLOCKED, &SB_JOURNAL(s)->j_state) ;
    return ;
}

/* this must be called without a transaction started, and does not
** require BKL
*/
void reiserfs_allow_writes(struct super_block *s) {
    clear_bit(WRITERS_BLOCKED, &SB_JOURNAL(s)->j_state) ;
    wake_up(&SB_JOURNAL(s)->j_join_wait) ;
}

/* this must be called without a transaction started, and does not
** require BKL
*/
void reiserfs_wait_on_write_block(struct super_block *s) {
    wait_event(SB_JOURNAL(s)->j_join_wait, 
               !test_bit(WRITERS_BLOCKED, &SB_JOURNAL(s)->j_state)) ;
}

static void queue_log_writer(struct super_block *s) {
    wait_queue_t wait;
    set_bit(WRITERS_QUEUED, &SB_JOURNAL(s)->j_state);

    /*
     * we don't want to use wait_event here because
     * we only want to wait once.
     */
    init_waitqueue_entry(&wait, current);
    add_wait_queue(&SB_JOURNAL(s)->j_join_wait, &wait);
    set_current_state(TASK_UNINTERRUPTIBLE);
    if (test_bit(WRITERS_QUEUED, &SB_JOURNAL(s)->j_state))
        schedule();
    current->state = TASK_RUNNING;
    remove_wait_queue(&SB_JOURNAL(s)->j_join_wait, &wait);
}

static void wake_queued_writers(struct super_block *s) {
    if (test_and_clear_bit(WRITERS_QUEUED, &SB_JOURNAL(s)->j_state))
        wake_up(&SB_JOURNAL(s)->j_join_wait);
}

static void let_transaction_grow(struct super_block *sb,
                                 unsigned long trans_id)
{
    unsigned long bcount = SB_JOURNAL(sb)->j_bcount;
    while(1) {
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(1);
	SB_JOURNAL(sb)->j_current_jl->j_state |= LIST_COMMIT_PENDING;
        while ((atomic_read(&SB_JOURNAL(sb)->j_wcount) > 0 ||
	        atomic_read(&SB_JOURNAL(sb)->j_jlock)) &&
	       SB_JOURNAL(sb)->j_trans_id == trans_id) {
	    queue_log_writer(sb);
	}
	if (SB_JOURNAL(sb)->j_trans_id != trans_id)
	    break;
	if (bcount == SB_JOURNAL(sb)->j_bcount)
	    break;
	bcount = SB_JOURNAL(sb)->j_bcount;
    }
}

/* join == true if you must join an existing transaction.
** join == false if you can deal with waiting for others to finish
**
** this will block until the transaction is joinable.  send the number of blocks you
** expect to use in nblocks.
*/
static int do_journal_begin_r(struct reiserfs_transaction_handle *th, struct super_block * p_s_sb,unsigned long nblocks,int join) {
  time_t now = get_seconds() ;
  int old_trans_id  ;
  struct reiserfs_journal *journal = SB_JOURNAL(p_s_sb);
  struct reiserfs_transaction_handle myth;
  int sched_count = 0;

  reiserfs_check_lock_depth(p_s_sb, "journal_begin") ;
  RFALSE( p_s_sb->s_flags & MS_RDONLY, 
	  "clm-2078: calling journal_begin on readonly FS") ;

  PROC_INFO_INC( p_s_sb, journal.journal_being );
  /* set here for journal_join */
  th->t_refcount = 1;
  th->t_super = p_s_sb ;

relock:
  lock_journal(p_s_sb) ;
  journal->j_bcount++;

  if (test_bit(WRITERS_BLOCKED, &journal->j_state)) {
    unlock_journal(p_s_sb) ;
    reiserfs_wait_on_write_block(p_s_sb) ;
    PROC_INFO_INC( p_s_sb, journal.journal_relock_writers );
    goto relock ;
  }
  now = get_seconds();

  /* if there is no room in the journal OR
  ** if this transaction is too old, and we weren't called joinable, wait for it to finish before beginning 
  ** we don't sleep if there aren't other writers
  */

  if ( (!join && journal->j_must_wait > 0) ||
     ( !join && (journal->j_len_alloc + nblocks + 2) >= SB_JOURNAL_MAX_BATCH(p_s_sb)) ||
     (!join && atomic_read(&journal->j_wcount) > 0 && journal->j_trans_start_time > 0 &&
      (now - journal->j_trans_start_time) > SB_JOURNAL_MAX_TRANS_AGE(p_s_sb)) ||
     (!join && atomic_read(&journal->j_jlock)) ||
     (!join && journal->j_cnode_free < (SB_JOURNAL_TRANS_MAX(p_s_sb) * 3))) {

    old_trans_id = journal->j_trans_id;
    unlock_journal(p_s_sb) ; /* allow others to finish this transaction */

    if (!join && (journal->j_len_alloc + nblocks + 2) >=
        SB_JOURNAL_MAX_BATCH(p_s_sb) &&
	((journal->j_len + nblocks + 2) * 100) < (journal->j_len_alloc * 75))
    {
	if (atomic_read(&journal->j_wcount) > 10) {
	    sched_count++;
	    queue_log_writer(p_s_sb);
	    goto relock;
	}
    }
    /* don't mess with joining the transaction if all we have to do is
     * wait for someone else to do a commit
     */
    if (atomic_read(&journal->j_jlock)) {
	while (journal->j_trans_id == old_trans_id &&
	       atomic_read(&journal->j_jlock)) {
	    queue_log_writer(p_s_sb);
        }
	goto relock;
    }
    journal_join(&myth, p_s_sb, 1) ;

    /* someone might have ended the transaction while we joined */
    if (old_trans_id != SB_JOURNAL(p_s_sb)->j_trans_id) {
        do_journal_end(&myth, p_s_sb, 1, 0) ;
    } else {
        do_journal_end(&myth, p_s_sb, 1, COMMIT_NOW) ;
    }

    PROC_INFO_INC( p_s_sb, journal.journal_relock_wcount );
    goto relock ;
  }
  /* we are the first writer, set trans_id */
  if (journal->j_trans_start_time == 0) {
    journal->j_trans_start_time = get_seconds();
  }
  atomic_inc(&(journal->j_wcount)) ;
  journal->j_len_alloc += nblocks ;
  th->t_blocks_logged = 0 ;
  th->t_blocks_allocated = nblocks ;
  th->t_trans_id = journal->j_trans_id ;
  unlock_journal(p_s_sb) ;
  return 0 ;
}

struct reiserfs_transaction_handle *
reiserfs_persistent_transaction(struct super_block *s, int nblocks) {
    int ret ;
    struct reiserfs_transaction_handle *th ;

    /* if we're nesting into an existing transaction.  It will be
    ** persistent on its own
    */
    if (reiserfs_transaction_running(s)) {
        th = current->journal_info ;
	th->t_refcount++ ;
	if (th->t_refcount < 2) {
	    BUG() ;
	}
	return th ;
    }
    th = reiserfs_kmalloc(sizeof(struct reiserfs_transaction_handle), GFP_NOFS, s) ;
    if (!th)
       return NULL;
    ret = journal_begin(th, s, nblocks) ;
    if (ret) {
	reiserfs_kfree(th, sizeof(struct reiserfs_transaction_handle), s) ;
        return NULL;
    }
    return th ;
}

int
reiserfs_end_persistent_transaction(struct reiserfs_transaction_handle *th) {
    struct super_block *s = th->t_super;
    int ret;
    ret = journal_end(th, th->t_super, th->t_blocks_allocated);
    if (th->t_refcount == 0)
	reiserfs_kfree(th, sizeof(struct reiserfs_transaction_handle), s) ;
    return ret;
}

static int journal_join(struct reiserfs_transaction_handle *th, struct super_block *p_s_sb, unsigned long nblocks) {
  struct reiserfs_transaction_handle *cur_th = current->journal_info;

  /* this keeps do_journal_end from NULLing out the current->journal_info
  ** pointer
  */
  th->t_handle_save = cur_th ;
  if (cur_th && cur_th->t_refcount > 1) {
      BUG() ;
  }
  return do_journal_begin_r(th, p_s_sb, nblocks, 1) ;
}

int journal_begin(struct reiserfs_transaction_handle *th, struct super_block  * p_s_sb, unsigned long nblocks) {
    struct reiserfs_transaction_handle *cur_th = current->journal_info ;
    int ret ;

    th->t_handle_save = NULL ;
    if (cur_th) {
	/* we are nesting into the current transaction */
	if (cur_th->t_super == p_s_sb) {
	      cur_th->t_refcount++ ;
	      memcpy(th, cur_th, sizeof(*th));
	      if (th->t_refcount <= 1)
		      reiserfs_warning (p_s_sb, "BAD: refcount <= 1, but journal_info != 0");
	      return 0;
	} else {
	    /* we've ended up with a handle from a different filesystem.
	    ** save it and restore on journal_end.  This should never
	    ** really happen...
	    */
	    reiserfs_warning(p_s_sb, "clm-2100: nesting info a different FS") ;
	    th->t_handle_save = current->journal_info ;
	    current->journal_info = th;
	}
    } else {
	current->journal_info = th;
    }
    ret = do_journal_begin_r(th, p_s_sb, nblocks, 0) ;
    if (current->journal_info != th)
        BUG() ;
    return ret ;
}

/*
** puts bh into the current transaction.  If it was already there, reorders removes the
** old pointers from the hash, and puts new ones in (to make sure replay happen in the right order).
**
** if it was dirty, cleans and files onto the clean list.  I can't let it be dirty again until the
** transaction is committed.
** 
** if j_len, is bigger than j_len_alloc, it pushes j_len_alloc to 10 + j_len.
*/
int journal_mark_dirty(struct reiserfs_transaction_handle *th, struct super_block *p_s_sb, struct buffer_head *bh) {
  struct reiserfs_journal_cnode *cn = NULL;
  int count_already_incd = 0 ;
  int prepared = 0 ;

  PROC_INFO_INC( p_s_sb, journal.mark_dirty );
  if (th->t_trans_id != SB_JOURNAL(p_s_sb)->j_trans_id) {
    reiserfs_panic(th->t_super, "journal-1577: handle trans id %ld != current trans id %ld\n", 
                   th->t_trans_id, SB_JOURNAL(p_s_sb)->j_trans_id);
  }
  p_s_sb->s_dirt = 1;

  prepared = test_and_clear_bit(BH_JPrepared, &bh->b_state) ;
  clear_bit(BH_JRestore_dirty, &bh->b_state);
  /* already in this transaction, we are done */
  if (buffer_journaled(bh)) {
    PROC_INFO_INC( p_s_sb, journal.mark_dirty_already );
    return 0 ;
  }

  /* this must be turned into a panic instead of a warning.  We can't allow
  ** a dirty or journal_dirty or locked buffer to be logged, as some changes
  ** could get to disk too early.  NOT GOOD.
  */
  if (!prepared || buffer_dirty(bh)) {
    reiserfs_warning (p_s_sb, "journal-1777: buffer %llu bad state "
		      "%cPREPARED %cLOCKED %cDIRTY %cJDIRTY_WAIT",
		      (unsigned long long)bh->b_blocknr, prepared ? ' ' : '!',
			    buffer_locked(bh) ? ' ' : '!',
			    buffer_dirty(bh) ? ' ' : '!',
			    buffer_journal_dirty(bh) ? ' ' : '!') ;
  }

  if (atomic_read(&(SB_JOURNAL(p_s_sb)->j_wcount)) <= 0) {
    reiserfs_warning (p_s_sb, "journal-1409: journal_mark_dirty returning because j_wcount was %d", atomic_read(&(SB_JOURNAL(p_s_sb)->j_wcount))) ;
    return 1 ;
  }
  /* this error means I've screwed up, and we've overflowed the transaction.  
  ** Nothing can be done here, except make the FS readonly or panic.
  */ 
  if (SB_JOURNAL(p_s_sb)->j_len >= SB_JOURNAL_TRANS_MAX(p_s_sb)) { 
    reiserfs_panic(th->t_super, "journal-1413: journal_mark_dirty: j_len (%lu) is too big\n", SB_JOURNAL(p_s_sb)->j_len) ;
  }

  if (buffer_journal_dirty(bh)) {
    count_already_incd = 1 ;
    PROC_INFO_INC( p_s_sb, journal.mark_dirty_notjournal );
    mark_buffer_notjournal_dirty(bh) ;
  }

  if (SB_JOURNAL(p_s_sb)->j_len > SB_JOURNAL(p_s_sb)->j_len_alloc) {
    SB_JOURNAL(p_s_sb)->j_len_alloc = SB_JOURNAL(p_s_sb)->j_len + JOURNAL_PER_BALANCE_CNT ;
  }

  set_bit(BH_JDirty, &bh->b_state) ;

  /* now put this guy on the end */
  if (!cn) {
    cn = get_cnode(p_s_sb) ;
    if (!cn) {
      reiserfs_panic(p_s_sb, "get_cnode failed!\n"); 
    }

    if (th->t_blocks_logged == th->t_blocks_allocated) {
      th->t_blocks_allocated += JOURNAL_PER_BALANCE_CNT ;
      SB_JOURNAL(p_s_sb)->j_len_alloc += JOURNAL_PER_BALANCE_CNT ;
    }
    th->t_blocks_logged++ ;
    SB_JOURNAL(p_s_sb)->j_len++ ;

    cn->bh = bh ;
    cn->blocknr = bh->b_blocknr ;
    cn->sb = p_s_sb;
    cn->jlist = NULL ;
    insert_journal_hash(SB_JOURNAL(p_s_sb)->j_hash_table, cn) ;
    if (!count_already_incd) {
      get_bh(bh) ;
    }
  }
  cn->next = NULL ;
  cn->prev = SB_JOURNAL(p_s_sb)->j_last ;
  cn->bh = bh ;
  if (SB_JOURNAL(p_s_sb)->j_last) {
    SB_JOURNAL(p_s_sb)->j_last->next = cn ;
    SB_JOURNAL(p_s_sb)->j_last = cn ;
  } else {
    SB_JOURNAL(p_s_sb)->j_first = cn ;
    SB_JOURNAL(p_s_sb)->j_last = cn ;
  }
  return 0 ;
}

int journal_end(struct reiserfs_transaction_handle *th, struct super_block *p_s_sb, unsigned long nblocks) {
  if (!current->journal_info && th->t_refcount > 1)
    reiserfs_warning (p_s_sb, "REISER-NESTING: th NULL, refcount %d",
                      th->t_refcount);

  th->t_refcount--;
  if (th->t_refcount > 0) {
    struct reiserfs_transaction_handle *cur_th = current->journal_info ;

    /* we aren't allowed to close a nested transaction on a different
    ** filesystem from the one in the task struct
    */
    if (cur_th->t_super != th->t_super)
      BUG() ;

    if (th != cur_th) {
      memcpy(current->journal_info, th, sizeof(*th));
      th->t_trans_id = 0;
    }
    return 0;
  } else {
    return do_journal_end(th, p_s_sb, nblocks, 0) ;
  }
}

/* removes from the current transaction, relsing and descrementing any counters.  
** also files the removed buffer directly onto the clean list
**
** called by journal_mark_freed when a block has been deleted
**
** returns 1 if it cleaned and relsed the buffer. 0 otherwise
*/
static int remove_from_transaction(struct super_block *p_s_sb, b_blocknr_t blocknr, int already_cleaned) {
  struct buffer_head *bh ;
  struct reiserfs_journal_cnode *cn ;
  int ret = 0;

  cn = get_journal_hash_dev(p_s_sb, SB_JOURNAL(p_s_sb)->j_hash_table, blocknr) ;
  if (!cn || !cn->bh) {
    return ret ;
  }
  bh = cn->bh ;
  if (cn->prev) {
    cn->prev->next = cn->next ;
  }
  if (cn->next) {
    cn->next->prev = cn->prev ;
  }
  if (cn == SB_JOURNAL(p_s_sb)->j_first) {
    SB_JOURNAL(p_s_sb)->j_first = cn->next ;  
  }
  if (cn == SB_JOURNAL(p_s_sb)->j_last) {
    SB_JOURNAL(p_s_sb)->j_last = cn->prev ;
  }
  if (bh)
	remove_journal_hash(p_s_sb, SB_JOURNAL(p_s_sb)->j_hash_table, NULL, bh->b_blocknr, 0) ; 
  mark_buffer_not_journaled(bh) ; /* don't log this one */

  if (!already_cleaned) {
    mark_buffer_notjournal_dirty(bh) ; 
    put_bh(bh) ;
    if (atomic_read(&(bh->b_count)) < 0) {
      reiserfs_warning (p_s_sb, "journal-1752: remove from trans, b_count < 0");
    }
    ret = 1 ;
  }
  SB_JOURNAL(p_s_sb)->j_len-- ;
  SB_JOURNAL(p_s_sb)->j_len_alloc-- ;
  free_cnode(p_s_sb, cn) ;
  return ret ;
}

/*
** for any cnode in a journal list, it can only be dirtied of all the
** transactions that include it are commited to disk.
** this checks through each transaction, and returns 1 if you are allowed to dirty,
** and 0 if you aren't
**
** it is called by dirty_journal_list, which is called after flush_commit_list has gotten all the log
** blocks for a given transaction on disk
**
*/
static int can_dirty(struct reiserfs_journal_cnode *cn) {
  struct super_block *sb = cn->sb;
  b_blocknr_t blocknr = cn->blocknr  ;
  struct reiserfs_journal_cnode *cur = cn->hprev ;
  int can_dirty = 1 ;
  
  /* first test hprev.  These are all newer than cn, so any node here
  ** with the same block number and dev means this node can't be sent
  ** to disk right now.
  */
  while(cur && can_dirty) {
    if (cur->jlist && cur->bh && cur->blocknr && cur->sb == sb && 
        cur->blocknr == blocknr) {
      can_dirty = 0 ;
    }
    cur = cur->hprev ;
  }
  /* then test hnext.  These are all older than cn.  As long as they
  ** are committed to the log, it is safe to write cn to disk
  */
  cur = cn->hnext ;
  while(cur && can_dirty) {
    if (cur->jlist && cur->jlist->j_len > 0 && 
        atomic_read(&(cur->jlist->j_commit_left)) > 0 && cur->bh && 
        cur->blocknr && cur->sb == sb && cur->blocknr == blocknr) {
      can_dirty = 0 ;
    }
    cur = cur->hnext ;
  }
  return can_dirty ;
}

/* syncs the commit blocks, but does not force the real buffers to disk
** will wait until the current transaction is done/commited before returning 
*/
int journal_end_sync(struct reiserfs_transaction_handle *th, struct super_block *p_s_sb, unsigned long nblocks) {

  /* you can sync while nested, very, very bad */
  if (th->t_refcount > 1) {
    BUG() ;
  }
  if (SB_JOURNAL(p_s_sb)->j_len == 0) {
    reiserfs_prepare_for_journal(p_s_sb, SB_BUFFER_WITH_SB(p_s_sb), 1) ;
    journal_mark_dirty(th, p_s_sb, SB_BUFFER_WITH_SB(p_s_sb)) ;
  }
  return do_journal_end(th, p_s_sb, nblocks, COMMIT_NOW | WAIT) ;
}

/*
** writeback the pending async commits to disk
*/
static void flush_async_commits(void *p) {
  struct super_block *p_s_sb = p;
  struct reiserfs_journal_list *jl;
  struct list_head *entry;

  lock_kernel();
  if (!list_empty(&SB_JOURNAL(p_s_sb)->j_journal_list)) {
      /* last entry is the youngest, commit it and you get everything */
      entry = SB_JOURNAL(p_s_sb)->j_journal_list.prev;
      jl = JOURNAL_LIST_ENTRY(entry);
      flush_commit_list(p_s_sb, jl, 1);
  }
  unlock_kernel();
  /*
   * this is a little racey, but there's no harm in missing
   * the filemap_fdata_write
   */
  if (!atomic_read(&SB_JOURNAL(p_s_sb)->j_async_throttle)) {
      atomic_inc(&SB_JOURNAL(p_s_sb)->j_async_throttle);
      filemap_fdatawrite(p_s_sb->s_bdev->bd_inode->i_mapping);
      atomic_dec(&SB_JOURNAL(p_s_sb)->j_async_throttle);
  }
}

/*
** flushes any old transactions to disk
** ends the current transaction if it is too old
*/
int reiserfs_flush_old_commits(struct super_block *p_s_sb) {
    time_t now ;
    struct reiserfs_transaction_handle th ;

    now = get_seconds();
    /* safety check so we don't flush while we are replaying the log during
     * mount
     */
    if (list_empty(&SB_JOURNAL(p_s_sb)->j_journal_list)) {
	return 0  ;
    }

    /* check the current transaction.  If there are no writers, and it is
     * too old, finish it, and force the commit blocks to disk
     */
    if (atomic_read(&(SB_JOURNAL(p_s_sb)->j_wcount)) <= 0 &&
        SB_JOURNAL(p_s_sb)->j_trans_start_time > 0 &&
        SB_JOURNAL(p_s_sb)->j_len > 0 &&
        (now - SB_JOURNAL(p_s_sb)->j_trans_start_time) >
	SB_JOURNAL_MAX_TRANS_AGE(p_s_sb))
    {
	journal_join(&th, p_s_sb, 1) ;
	reiserfs_prepare_for_journal(p_s_sb, SB_BUFFER_WITH_SB(p_s_sb), 1) ;
	journal_mark_dirty(&th, p_s_sb, SB_BUFFER_WITH_SB(p_s_sb)) ;

	/* we're only being called from kreiserfsd, it makes no sense to do
	** an async commit so that kreiserfsd can do it later
	*/
	do_journal_end(&th, p_s_sb,1, COMMIT_NOW | WAIT) ;
    }
    return p_s_sb->s_dirt;
}

/*
** returns 0 if do_journal_end should return right away, returns 1 if do_journal_end should finish the commit
** 
** if the current transaction is too old, but still has writers, this will wait on j_join_wait until all 
** the writers are done.  By the time it wakes up, the transaction it was called has already ended, so it just
** flushes the commit list and returns 0.
**
** Won't batch when flush or commit_now is set.  Also won't batch when others are waiting on j_join_wait.
** 
** Note, we can't allow the journal_end to proceed while there are still writers in the log.
*/
static int check_journal_end(struct reiserfs_transaction_handle *th, struct super_block  * p_s_sb, 
                             unsigned long nblocks, int flags) {

  time_t now ;
  int flush = flags & FLUSH_ALL ;
  int commit_now = flags & COMMIT_NOW ;
  int wait_on_commit = flags & WAIT ;
  struct reiserfs_journal_list *jl;

  if (th->t_trans_id != SB_JOURNAL(p_s_sb)->j_trans_id) {
    reiserfs_panic(th->t_super, "journal-1577: handle trans id %ld != current trans id %ld\n", 
                   th->t_trans_id, SB_JOURNAL(p_s_sb)->j_trans_id);
  }

  SB_JOURNAL(p_s_sb)->j_len_alloc -= (th->t_blocks_allocated - th->t_blocks_logged) ;
  if (atomic_read(&(SB_JOURNAL(p_s_sb)->j_wcount)) > 0) { /* <= 0 is allowed.  unmounting might not call begin */
    atomic_dec(&(SB_JOURNAL(p_s_sb)->j_wcount)) ;
  }

  /* BUG, deal with case where j_len is 0, but people previously freed blocks need to be released 
  ** will be dealt with by next transaction that actually writes something, but should be taken
  ** care of in this trans
  */
  if (SB_JOURNAL(p_s_sb)->j_len == 0) {
    BUG();
  }
  /* if wcount > 0, and we are called to with flush or commit_now,
  ** we wait on j_join_wait.  We will wake up when the last writer has
  ** finished the transaction, and started it on its way to the disk.
  ** Then, we flush the commit or journal list, and just return 0 
  ** because the rest of journal end was already done for this transaction.
  */
  if (atomic_read(&(SB_JOURNAL(p_s_sb)->j_wcount)) > 0) {
    if (flush || commit_now) {
      unsigned trans_id ;

      jl = SB_JOURNAL(p_s_sb)->j_current_jl;
      trans_id = jl->j_trans_id;
      if (wait_on_commit)
        jl->j_state |= LIST_COMMIT_PENDING;
      atomic_set(&(SB_JOURNAL(p_s_sb)->j_jlock), 1) ;
      if (flush) {
        SB_JOURNAL(p_s_sb)->j_next_full_flush = 1 ;
      }
      unlock_journal(p_s_sb) ;

      /* sleep while the current transaction is still j_jlocked */
      while(SB_JOURNAL(p_s_sb)->j_trans_id == trans_id) {
	if (atomic_read(&SB_JOURNAL(p_s_sb)->j_jlock)) {
	    queue_log_writer(p_s_sb);
        } else {
	    lock_journal(p_s_sb);
	    if (SB_JOURNAL(p_s_sb)->j_trans_id == trans_id) {
	        atomic_set(&(SB_JOURNAL(p_s_sb)->j_jlock), 1) ;
	    }
	    unlock_journal(p_s_sb);
	}
      }
      if (SB_JOURNAL(p_s_sb)->j_trans_id == trans_id) {
          BUG();
      }
      if (commit_now && journal_list_still_alive(p_s_sb, trans_id) &&
          wait_on_commit)
      {
	  flush_commit_list(p_s_sb, jl, 1) ;
      }
      return 0 ;
    } 
    unlock_journal(p_s_sb) ;
    return 0 ;
  }

  /* deal with old transactions where we are the last writers */
  now = get_seconds();
  if ((now - SB_JOURNAL(p_s_sb)->j_trans_start_time) > SB_JOURNAL_MAX_TRANS_AGE(p_s_sb)) {
    commit_now = 1 ;
    SB_JOURNAL(p_s_sb)->j_next_async_flush = 1 ;
  }
  /* don't batch when someone is waiting on j_join_wait */
  /* don't batch when syncing the commit or flushing the whole trans */
  if (!(SB_JOURNAL(p_s_sb)->j_must_wait > 0) && !(atomic_read(&(SB_JOURNAL(p_s_sb)->j_jlock))) && !flush && !commit_now && 
      (SB_JOURNAL(p_s_sb)->j_len < SB_JOURNAL_MAX_BATCH(p_s_sb))  && 
      SB_JOURNAL(p_s_sb)->j_len_alloc < SB_JOURNAL_MAX_BATCH(p_s_sb) && SB_JOURNAL(p_s_sb)->j_cnode_free > (SB_JOURNAL_TRANS_MAX(p_s_sb) * 3)) {
    SB_JOURNAL(p_s_sb)->j_bcount++ ;
    unlock_journal(p_s_sb) ;
    return 0 ;
  }

  if (SB_JOURNAL(p_s_sb)->j_start > SB_ONDISK_JOURNAL_SIZE(p_s_sb)) {
    reiserfs_panic(p_s_sb, "journal-003: journal_end: j_start (%ld) is too high\n", SB_JOURNAL(p_s_sb)->j_start) ;
  }
  return 1 ;
}

/*
** Does all the work that makes deleting blocks safe.
** when deleting a block mark BH_JNew, just remove it from the current transaction, clean it's buffer_head and move on.
** 
** otherwise:
** set a bit for the block in the journal bitmap.  That will prevent it from being allocated for unformatted nodes
** before this transaction has finished.
**
** mark any cnodes for this block as BLOCK_FREED, and clear their bh pointers.  That will prevent any old transactions with
** this block from trying to flush to the real location.  Since we aren't removing the cnode from the journal_list_hash,
** the block can't be reallocated yet.
**
** Then remove it from the current transaction, decrementing any counters and filing it on the clean list.
*/
int journal_mark_freed(struct reiserfs_transaction_handle *th, struct super_block *p_s_sb, b_blocknr_t blocknr) {
  struct reiserfs_journal_cnode *cn = NULL ;
  struct buffer_head *bh = NULL ;
  struct reiserfs_list_bitmap *jb = NULL ;
  int cleaned = 0 ;

  cn = get_journal_hash_dev(p_s_sb, SB_JOURNAL(p_s_sb)->j_hash_table, blocknr);
  if (cn && cn->bh) {
      bh = cn->bh ;
      get_bh(bh) ;
  }
  /* if it is journal new, we just remove it from this transaction */
  if (bh && buffer_journal_new(bh)) {
    mark_buffer_notjournal_new(bh) ;
    clear_prepared_bits(bh) ;
    reiserfs_clean_and_file_buffer(bh) ;
    cleaned = remove_from_transaction(p_s_sb, blocknr, cleaned) ;
  } else {
    /* set the bit for this block in the journal bitmap for this transaction */
    jb = SB_JOURNAL(p_s_sb)->j_current_jl->j_list_bitmap;
    if (!jb) {
      reiserfs_panic(p_s_sb, "journal-1702: journal_mark_freed, journal_list_bitmap is NULL\n") ;
    }
    set_bit_in_list_bitmap(p_s_sb, blocknr, jb) ;

    /* Note, the entire while loop is not allowed to schedule.  */

    if (bh) {
      clear_prepared_bits(bh) ;
      reiserfs_clean_and_file_buffer(bh) ;
    }
    cleaned = remove_from_transaction(p_s_sb, blocknr, cleaned) ;

    /* find all older transactions with this block, make sure they don't try to write it out */
    cn = get_journal_hash_dev(p_s_sb,SB_JOURNAL(p_s_sb)->j_list_hash_table,  blocknr) ;
    while (cn) {
      if (p_s_sb == cn->sb && blocknr == cn->blocknr) {
	set_bit(BLOCK_FREED, &cn->state) ;
	if (cn->bh) {
	  if (!cleaned) {
	    /* remove_from_transaction will brelse the buffer if it was 
	    ** in the current trans
	    */
	    mark_buffer_notjournal_dirty(cn->bh) ;
	    cleaned = 1 ;
	    put_bh(cn->bh) ;
	    if (atomic_read(&(cn->bh->b_count)) < 0) {
	      reiserfs_warning (p_s_sb, "journal-2138: cn->bh->b_count < 0");
	    }
	  }
	  if (cn->jlist) { /* since we are clearing the bh, we MUST dec nonzerolen */
	    atomic_dec(&(cn->jlist->j_nonzerolen)) ;
	  }
	  cn->bh = NULL ; 
	} 
      }
      cn = cn->hnext ;
    }
  }

  if (bh) {
    put_bh(bh) ; /* get_hash grabs the buffer */
    if (atomic_read(&(bh->b_count)) < 0) {
      reiserfs_warning (p_s_sb, "journal-2165: bh->b_count < 0");
    }
  }
  return 0 ;
}

void reiserfs_update_inode_transaction(struct inode *inode) {
  REISERFS_I(inode)->i_jl = SB_JOURNAL(inode->i_sb)->j_current_jl;
  REISERFS_I(inode)->i_trans_id = SB_JOURNAL(inode->i_sb)->j_trans_id ;
}

static void __commit_trans_jl(struct inode *inode, unsigned long id,
                                 struct reiserfs_journal_list *jl)
{
    struct reiserfs_transaction_handle th ;
    struct super_block *sb = inode->i_sb ;

    /* is it from the current transaction, or from an unknown transaction? */
    if (id == SB_JOURNAL(sb)->j_trans_id) {
	jl = SB_JOURNAL(sb)->j_current_jl;
	/* try to let other writers come in and grow this transaction */
	let_transaction_grow(sb, id);
	if (SB_JOURNAL(sb)->j_trans_id != id) {
	    goto flush_commit_only;
	}

	journal_begin(&th, sb, 1) ;

	/* someone might have ended this transaction while we joined */
	if (SB_JOURNAL(sb)->j_trans_id != id) {
	    reiserfs_prepare_for_journal(sb, SB_BUFFER_WITH_SB(sb), 1) ;
	    journal_mark_dirty(&th, sb, SB_BUFFER_WITH_SB(sb)) ;
	    journal_end(&th, sb, 1) ;
	    goto flush_commit_only;
	}

	journal_end_sync(&th, sb, 1) ;

    } else {
	/* this gets tricky, we have to make sure the journal list in
	 * the inode still exists.  We know the list is still around
	 * if we've got a larger transaction id than the oldest list
	 */
flush_commit_only:
	if (journal_list_still_alive(inode->i_sb, id)) {
	    flush_commit_list(sb, jl, 1) ;
	}
    }
    /* otherwise the list is gone, and long since committed */
}

void reiserfs_commit_for_inode(struct inode *inode) {
    unsigned long id = REISERFS_I(inode)->i_trans_id;
    struct reiserfs_journal_list *jl = REISERFS_I(inode)->i_jl;

    /* for the whole inode, assume unset id means it was
     * changed in the current transaction.  More conservative
     */
    if (!id || !jl) {
	reiserfs_update_inode_transaction(inode) ;
	id = REISERFS_I(inode)->i_trans_id;
	/* jl will be updated in __commit_trans_jl */
    }

    __commit_trans_jl(inode, id, jl);
}

void reiserfs_restore_prepared_buffer(struct super_block *p_s_sb, 
                                      struct buffer_head *bh) {
    PROC_INFO_INC( p_s_sb, journal.restore_prepared );
    if (!bh) {
	return ;
    }
    if (test_and_clear_bit(BH_JRestore_dirty, &bh->b_state) &&
	buffer_journal_dirty(bh)) {
	struct reiserfs_journal_cnode *cn;
	cn = get_journal_hash_dev(p_s_sb,
	                          SB_JOURNAL(p_s_sb)->j_list_hash_table,
				  bh->b_blocknr);
	if (cn && can_dirty(cn)) {
	    set_bit(BH_JTest, &bh->b_state);
	    mark_buffer_dirty(bh);
        }
    }
    clear_bit(BH_JPrepared, &bh->b_state) ;
}

extern struct tree_balance *cur_tb ;
/*
** before we can change a metadata block, we have to make sure it won't
** be written to disk while we are altering it.  So, we must:
** clean it
** wait on it.
** 
*/
int reiserfs_prepare_for_journal(struct super_block *p_s_sb,
                                  struct buffer_head *bh, int wait) {
  PROC_INFO_INC( p_s_sb, journal.prepare );

    if (test_set_buffer_locked(bh)) {
	if (!wait)
	    return 0;
	lock_buffer(bh);
    }
    set_bit(BH_JPrepared, &bh->b_state);
    if (test_clear_buffer_dirty(bh) && buffer_journal_dirty(bh))  {
	clear_bit(BH_JTest, &bh->b_state);
	set_bit(BH_JRestore_dirty, &bh->b_state);
    }
    unlock_buffer(bh);
    return 1;
}

static void flush_old_journal_lists(struct super_block *s) {
    struct reiserfs_journal_list *jl;
    struct list_head *entry;
    time_t now = get_seconds();

    while(!list_empty(&SB_JOURNAL(s)->j_journal_list)) {
        entry = SB_JOURNAL(s)->j_journal_list.next;
	jl = JOURNAL_LIST_ENTRY(entry);
	/* this check should always be run, to send old lists to disk */
	if (jl->j_timestamp < (now - (JOURNAL_MAX_TRANS_AGE * 4))) {
	    flush_used_journal_lists(s, jl);
	} else {
	    break;
	}
    }
}

/* 
** long and ugly.  If flush, will not return until all commit
** blocks and all real buffers in the trans are on disk.
** If no_async, won't return until all commit blocks are on disk.
**
** keep reading, there are comments as you go along
*/
static int do_journal_end(struct reiserfs_transaction_handle *th, struct super_block  * p_s_sb, unsigned long nblocks, 
		          int flags) {
  struct reiserfs_journal_cnode *cn, *next, *jl_cn; 
  struct reiserfs_journal_cnode *last_cn = NULL;
  struct reiserfs_journal_desc *desc ; 
  struct reiserfs_journal_commit *commit ; 
  struct buffer_head *c_bh ; /* commit bh */
  struct buffer_head *d_bh ; /* desc bh */
  int cur_write_start = 0 ; /* start index of current log write */
  int old_start ;
  int i ;
  int flush = flags & FLUSH_ALL ;
  int wait_on_commit = flags & WAIT ;
  struct reiserfs_journal_list *jl, *temp_jl;
  struct list_head *entry, *safe;
  unsigned long jindex;
  unsigned long commit_trans_id;
  int trans_half;

  if (th->t_refcount > 1)
    BUG() ;

  current->journal_info = th->t_handle_save;
  reiserfs_check_lock_depth(p_s_sb, "journal end");
  if (SB_JOURNAL(p_s_sb)->j_len == 0) {
      reiserfs_prepare_for_journal(p_s_sb, SB_BUFFER_WITH_SB(p_s_sb), 1) ;
      journal_mark_dirty(th, p_s_sb, SB_BUFFER_WITH_SB(p_s_sb)) ;
  }

  lock_journal(p_s_sb) ;
  if (SB_JOURNAL(p_s_sb)->j_next_full_flush) {
    flags |= FLUSH_ALL ;
    flush = 1 ;
  }
  if (SB_JOURNAL(p_s_sb)->j_next_async_flush) {
    flags |= COMMIT_NOW | WAIT;
    wait_on_commit = 1;
  }

  /* check_journal_end locks the journal, and unlocks if it does not return 1 
  ** it tells us if we should continue with the journal_end, or just return
  */
  if (!check_journal_end(th, p_s_sb, nblocks, flags)) {
    p_s_sb->s_dirt = 1;
    wake_queued_writers(p_s_sb);
    reiserfs_async_progress_wait(p_s_sb);
    goto out ;
  }

  /* check_journal_end might set these, check again */
  if (SB_JOURNAL(p_s_sb)->j_next_full_flush) {
    flush = 1 ;
  }

  /*
  ** j must wait means we have to flush the log blocks, and the real blocks for
  ** this transaction
  */
  if (SB_JOURNAL(p_s_sb)->j_must_wait > 0) {
    flush = 1 ;
  }

#ifdef REISERFS_PREALLOCATE
  /* quota ops might need to nest, setup the journal_info pointer for them */
  current->journal_info = th ;
  reiserfs_discard_all_prealloc(th); /* it should not involve new blocks into
				      * the transaction */
  current->journal_info = th->t_handle_save ;
#endif
  
  /* setup description block */
  d_bh = journal_getblk(p_s_sb, SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb) + SB_JOURNAL(p_s_sb)->j_start) ; 
  set_buffer_uptodate(d_bh);
  desc = (struct reiserfs_journal_desc *)(d_bh)->b_data ;
  memset(d_bh->b_data, 0, d_bh->b_size) ;
  memcpy(get_journal_desc_magic (d_bh), JOURNAL_DESC_MAGIC, 8) ;
  set_desc_trans_id(desc, SB_JOURNAL(p_s_sb)->j_trans_id) ;

  /* setup commit block.  Don't write (keep it clean too) this one until after everyone else is written */
  c_bh =  journal_getblk(p_s_sb, SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb) + 
		 ((SB_JOURNAL(p_s_sb)->j_start + SB_JOURNAL(p_s_sb)->j_len + 1) % SB_ONDISK_JOURNAL_SIZE(p_s_sb))) ;
  commit = (struct reiserfs_journal_commit *)c_bh->b_data ;
  memset(c_bh->b_data, 0, c_bh->b_size) ;
  set_commit_trans_id(commit, SB_JOURNAL(p_s_sb)->j_trans_id) ;
  set_buffer_uptodate(c_bh) ;

  /* init this journal list */
  jl = SB_JOURNAL(p_s_sb)->j_current_jl;

  /* we lock the commit before doing anything because
   * we want to make sure nobody tries to run flush_commit_list until
   * the new transaction is fully setup, and we've already flushed the
   * ordered bh list
   */
  down(&jl->j_commit_lock);

  /* save the transaction id in case we need to commit it later */
  commit_trans_id = jl->j_trans_id;

  atomic_set(&jl->j_older_commits_done, 0) ;
  jl->j_trans_id = SB_JOURNAL(p_s_sb)->j_trans_id ;
  jl->j_timestamp = SB_JOURNAL(p_s_sb)->j_trans_start_time ;
  jl->j_commit_bh = c_bh ;
  jl->j_start = SB_JOURNAL(p_s_sb)->j_start ;
  jl->j_len = SB_JOURNAL(p_s_sb)->j_len ;
  atomic_set(&jl->j_nonzerolen, SB_JOURNAL(p_s_sb)->j_len) ;
  atomic_set(&jl->j_commit_left, SB_JOURNAL(p_s_sb)->j_len + 2);
  jl->j_realblock = NULL ;

  /* The ENTIRE FOR LOOP MUST not cause schedule to occur.
  **  for each real block, add it to the journal list hash,
  ** copy into real block index array in the commit or desc block
  */
  trans_half = journal_trans_half(p_s_sb->s_blocksize);
  for (i = 0, cn = SB_JOURNAL(p_s_sb)->j_first ; cn ; cn = cn->next, i++) {
    if (test_bit(BH_JDirty, &cn->bh->b_state) ) {
      jl_cn = get_cnode(p_s_sb) ;
      if (!jl_cn) {
        reiserfs_panic(p_s_sb, "journal-1676, get_cnode returned NULL\n") ;
      }
      if (i == 0) {
        jl->j_realblock = jl_cn ;
      }
      jl_cn->prev = last_cn ;
      jl_cn->next = NULL ;
      if (last_cn) {
        last_cn->next = jl_cn ;
      }
      last_cn = jl_cn ;
      /* make sure the block we are trying to log is not a block 
         of journal or reserved area */

      if (is_block_in_log_or_reserved_area(p_s_sb, cn->bh->b_blocknr)) {
        reiserfs_panic(p_s_sb, "journal-2332: Trying to log block %lu, which is a log block\n", cn->bh->b_blocknr) ;
      }
      jl_cn->blocknr = cn->bh->b_blocknr ; 
      jl_cn->state = 0 ;
      jl_cn->sb = p_s_sb;
      jl_cn->bh = cn->bh ;
      jl_cn->jlist = jl;
      insert_journal_hash(SB_JOURNAL(p_s_sb)->j_list_hash_table, jl_cn) ; 
      if (i < trans_half) {
	desc->j_realblock[i] = cpu_to_le32(cn->bh->b_blocknr) ;
      } else {
	commit->j_realblock[i - trans_half] = cpu_to_le32(cn->bh->b_blocknr) ;
      }
    } else {
      i-- ;
    }
  }
  set_desc_trans_len(desc, SB_JOURNAL(p_s_sb)->j_len) ;
  set_desc_mount_id(desc, SB_JOURNAL(p_s_sb)->j_mount_id) ;
  set_desc_trans_id(desc, SB_JOURNAL(p_s_sb)->j_trans_id) ;
  set_commit_trans_len(commit, SB_JOURNAL(p_s_sb)->j_len);

  /* special check in case all buffers in the journal were marked for not logging */
  if (SB_JOURNAL(p_s_sb)->j_len == 0) {
    BUG();
  }

  /* we're about to dirty all the log blocks, mark the description block
   * dirty now too.  Don't mark the commit block dirty until all the
   * others are on disk
   */
  mark_buffer_dirty(d_bh);

  /* first data block is j_start + 1, so add one to cur_write_start wherever you use it */
  cur_write_start = SB_JOURNAL(p_s_sb)->j_start ;
  cn = SB_JOURNAL(p_s_sb)->j_first ;
  jindex = 1 ; /* start at one so we don't get the desc again */
  while(cn) {
    clear_bit(BH_JNew, &(cn->bh->b_state)) ;
    /* copy all the real blocks into log area.  dirty log blocks */
    if (test_bit(BH_JDirty, &cn->bh->b_state)) {
      struct buffer_head *tmp_bh ;
      char *addr;
      struct page *page;
      tmp_bh =  journal_getblk(p_s_sb, SB_ONDISK_JOURNAL_1st_BLOCK(p_s_sb) + 
		       ((cur_write_start + jindex) % SB_ONDISK_JOURNAL_SIZE(p_s_sb))) ;
      set_buffer_uptodate(tmp_bh);
      page = cn->bh->b_page;
      addr = kmap(page);
      memcpy(tmp_bh->b_data, addr + offset_in_page(cn->bh->b_data),
             cn->bh->b_size);
      kunmap(page);
      mark_buffer_dirty(tmp_bh);
      jindex++ ;
      set_bit(BH_JDirty_wait, &(cn->bh->b_state)) ; 
      clear_bit(BH_JDirty, &(cn->bh->b_state)) ;
    } else {
      /* JDirty cleared sometime during transaction.  don't log this one */
      reiserfs_warning(p_s_sb, "journal-2048: do_journal_end: BAD, buffer in journal hash, but not JDirty!") ;
      brelse(cn->bh) ;
    }
    next = cn->next ;
    free_cnode(p_s_sb, cn) ;
    cn = next ;
    cond_resched();
  }

  /* we are done  with both the c_bh and d_bh, but
  ** c_bh must be written after all other commit blocks,
  ** so we dirty/relse c_bh in flush_commit_list, with commit_left <= 1.
  */

  SB_JOURNAL(p_s_sb)->j_current_jl = alloc_journal_list(p_s_sb);

  /* now it is safe to insert this transaction on the main list */
  list_add_tail(&jl->j_list, &SB_JOURNAL(p_s_sb)->j_journal_list);
  list_add_tail(&jl->j_working_list, &SB_JOURNAL(p_s_sb)->j_working_list);
  SB_JOURNAL(p_s_sb)->j_num_work_lists++;

  /* reset journal values for the next transaction */
  old_start = SB_JOURNAL(p_s_sb)->j_start ;
  SB_JOURNAL(p_s_sb)->j_start = (SB_JOURNAL(p_s_sb)->j_start + SB_JOURNAL(p_s_sb)->j_len + 2) % SB_ONDISK_JOURNAL_SIZE(p_s_sb);
  atomic_set(&(SB_JOURNAL(p_s_sb)->j_wcount), 0) ;
  SB_JOURNAL(p_s_sb)->j_bcount = 0 ;
  SB_JOURNAL(p_s_sb)->j_last = NULL ;
  SB_JOURNAL(p_s_sb)->j_first = NULL ;
  SB_JOURNAL(p_s_sb)->j_len = 0 ;
  SB_JOURNAL(p_s_sb)->j_trans_start_time = 0 ;
  SB_JOURNAL(p_s_sb)->j_trans_id++ ;
  SB_JOURNAL(p_s_sb)->j_current_jl->j_trans_id = SB_JOURNAL(p_s_sb)->j_trans_id;
  SB_JOURNAL(p_s_sb)->j_must_wait = 0 ;
  SB_JOURNAL(p_s_sb)->j_len_alloc = 0 ;
  SB_JOURNAL(p_s_sb)->j_next_full_flush = 0 ;
  SB_JOURNAL(p_s_sb)->j_next_async_flush = 0 ;
  init_journal_hash(p_s_sb) ; 

  // make sure reiserfs_add_jh sees the new current_jl before we
  // write out the tails
  smp_mb();

  /* tail conversion targets have to hit the disk before we end the
   * transaction.  Otherwise a later transaction might repack the tail
   * before this transaction commits, leaving the data block unflushed and
   * clean, if we crash before the later transaction commits, the data block
   * is lost.
   */
  if (!list_empty(&jl->j_tail_bh_list)) {
      unlock_kernel();
      write_ordered_buffers(&SB_JOURNAL(p_s_sb)->j_dirty_buffers_lock,
			    SB_JOURNAL(p_s_sb), jl, &jl->j_tail_bh_list);
      lock_kernel();
  }
  if (!list_empty(&jl->j_tail_bh_list))
      BUG();
  up(&jl->j_commit_lock);

  /* honor the flush wishes from the caller, simple commits can
  ** be done outside the journal lock, they are done below
  **
  ** if we don't flush the commit list right now, we put it into
  ** the work queue so the people waiting on the async progress work
  ** queue don't wait for this proc to flush journal lists and such.
  */
  if (flush) {
    flush_commit_list(p_s_sb, jl, 1) ;
    flush_journal_list(p_s_sb, jl, 1) ;
  } else if (!(jl->j_state & LIST_COMMIT_PENDING))
    queue_delayed_work(commit_wq, &SB_JOURNAL(p_s_sb)->j_work, HZ/10);


  /* if the next transaction has any chance of wrapping, flush 
  ** transactions that might get overwritten.  If any journal lists are very 
  ** old flush them as well.  
  */
first_jl:
  list_for_each_safe(entry, safe, &SB_JOURNAL(p_s_sb)->j_journal_list) {
    temp_jl = JOURNAL_LIST_ENTRY(entry);
    if (SB_JOURNAL(p_s_sb)->j_start <= temp_jl->j_start) {
      if ((SB_JOURNAL(p_s_sb)->j_start + SB_JOURNAL_TRANS_MAX(p_s_sb) + 1) >=
          temp_jl->j_start)
      {
	flush_used_journal_lists(p_s_sb, temp_jl);
	goto first_jl;
      } else if ((SB_JOURNAL(p_s_sb)->j_start +
                  SB_JOURNAL_TRANS_MAX(p_s_sb) + 1) <
		  SB_ONDISK_JOURNAL_SIZE(p_s_sb))
      {
          /* if we don't cross into the next transaction and we don't
	   * wrap, there is no way we can overlap any later transactions
	   * break now
	   */
	  break;
      }
    } else if ((SB_JOURNAL(p_s_sb)->j_start +
                SB_JOURNAL_TRANS_MAX(p_s_sb) + 1) >
		SB_ONDISK_JOURNAL_SIZE(p_s_sb))
    {
      if (((SB_JOURNAL(p_s_sb)->j_start + SB_JOURNAL_TRANS_MAX(p_s_sb) + 1) %
            SB_ONDISK_JOURNAL_SIZE(p_s_sb)) >= temp_jl->j_start)
      {
	flush_used_journal_lists(p_s_sb, temp_jl);
	goto first_jl;
      } else {
	  /* we don't overlap anything from out start to the end of the
	   * log, and our wrapped portion doesn't overlap anything at
	   * the start of the log.  We can break
	   */
	  break;
      }
    }
  }
  flush_old_journal_lists(p_s_sb);

  SB_JOURNAL(p_s_sb)->j_current_jl->j_list_bitmap = get_list_bitmap(p_s_sb, SB_JOURNAL(p_s_sb)->j_current_jl) ;

  if (!(SB_JOURNAL(p_s_sb)->j_current_jl->j_list_bitmap)) {
    reiserfs_panic(p_s_sb, "journal-1996: do_journal_end, could not get a list bitmap\n") ;
  }

  atomic_set(&(SB_JOURNAL(p_s_sb)->j_jlock), 0) ;
  unlock_journal(p_s_sb) ;
  /* wake up any body waiting to join. */
  clear_bit(WRITERS_QUEUED, &SB_JOURNAL(p_s_sb)->j_state);
  wake_up(&(SB_JOURNAL(p_s_sb)->j_join_wait)) ;

  if (!flush && wait_on_commit &&
      journal_list_still_alive(p_s_sb, commit_trans_id)) {
	  flush_commit_list(p_s_sb, jl, 1) ;
  }
out:
  reiserfs_check_lock_depth(p_s_sb, "journal end2");
  th->t_trans_id = 0;
  return 0 ;
}
