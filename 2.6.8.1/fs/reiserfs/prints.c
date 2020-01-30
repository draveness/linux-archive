/*
 * Copyright 2000 by Hans Reiser, licensing governed by reiserfs/README
 */

#include <linux/config.h>
#include <linux/time.h>
#include <linux/fs.h>
#include <linux/reiserfs_fs.h>
#include <linux/string.h>
#include <linux/buffer_head.h>

#include <stdarg.h>

static char error_buf[1024];
static char fmt_buf[1024];
static char off_buf[80];


static char * reiserfs_cpu_offset (struct cpu_key * key)
{
  if (cpu_key_k_type(key) == TYPE_DIRENTRY)
    sprintf (off_buf, "%Lu(%Lu)", 
	     (unsigned long long)GET_HASH_VALUE (cpu_key_k_offset (key)),
	     (unsigned long long)GET_GENERATION_NUMBER (cpu_key_k_offset (key)));
  else
    sprintf (off_buf, "0x%Lx", (unsigned long long)cpu_key_k_offset (key));
  return off_buf;
}


static char * le_offset (struct key * key)
{
  int version;

  version = le_key_version (key);
  if (le_key_k_type (version, key) == TYPE_DIRENTRY)
    sprintf (off_buf, "%Lu(%Lu)", 
	     (unsigned long long)GET_HASH_VALUE (le_key_k_offset (version, key)),
	     (unsigned long long)GET_GENERATION_NUMBER (le_key_k_offset (version, key)));
  else
    sprintf (off_buf, "0x%Lx", (unsigned long long)le_key_k_offset (version, key));
  return off_buf;
}


static char * cpu_type (struct cpu_key * key)
{
    if (cpu_key_k_type (key) == TYPE_STAT_DATA)
	return "SD";
    if (cpu_key_k_type (key) == TYPE_DIRENTRY)
	return "DIR";
    if (cpu_key_k_type (key) == TYPE_DIRECT)
	return "DIRECT";
    if (cpu_key_k_type (key) == TYPE_INDIRECT)
	return "IND";
    return "UNKNOWN";
}


static char * le_type (struct key * key)
{
    int version;
    
    version = le_key_version (key);

    if (le_key_k_type (version, key) == TYPE_STAT_DATA)
	return "SD";
    if (le_key_k_type (version, key) == TYPE_DIRENTRY)
	return "DIR";
    if (le_key_k_type (version, key) == TYPE_DIRECT)
	return "DIRECT";
    if (le_key_k_type (version, key) == TYPE_INDIRECT)
	return "IND";
    return "UNKNOWN";
}


/* %k */
static void sprintf_le_key (char * buf, struct key * key)
{
  if (key)
    sprintf (buf, "[%d %d %s %s]", le32_to_cpu (key->k_dir_id),
	     le32_to_cpu (key->k_objectid), le_offset (key), le_type (key));
  else
    sprintf (buf, "[NULL]");
}


/* %K */
static void sprintf_cpu_key (char * buf, struct cpu_key * key)
{
  if (key)
    sprintf (buf, "[%d %d %s %s]", key->on_disk_key.k_dir_id,
	     key->on_disk_key.k_objectid, reiserfs_cpu_offset (key),
             cpu_type (key));
  else
    sprintf (buf, "[NULL]");
}

static void sprintf_de_head( char *buf, struct reiserfs_de_head *deh )
{
    if( deh )
        sprintf( buf, "[offset=%d dir_id=%d objectid=%d location=%d state=%04x]", deh_offset(deh), deh_dir_id(deh),
                 deh_objectid(deh), deh_location(deh), deh_state(deh) );
    else
        sprintf( buf, "[NULL]" );

}

static void sprintf_item_head (char * buf, struct item_head * ih)
{
    if (ih) {
	strcpy (buf, (ih_version (ih) == KEY_FORMAT_3_6) ? "*3.6* " : "*3.5*");
	sprintf_le_key (buf + strlen (buf), &(ih->ih_key));
	sprintf (buf + strlen (buf), ", item_len %d, item_location %d, "
		 "free_space(entry_count) %d",
		 ih_item_len(ih), ih_location(ih), ih_free_space (ih));
    } else
	sprintf (buf, "[NULL]");
}


static void sprintf_direntry (char * buf, struct reiserfs_dir_entry * de)
{
  char name[20];

  memcpy (name, de->de_name, de->de_namelen > 19 ? 19 : de->de_namelen);
  name [de->de_namelen > 19 ? 19 : de->de_namelen] = 0;
  sprintf (buf, "\"%s\"==>[%d %d]", name, de->de_dir_id, de->de_objectid);
}


static void sprintf_block_head (char * buf, struct buffer_head * bh)
{
  sprintf (buf, "level=%d, nr_items=%d, free_space=%d rdkey ",
	   B_LEVEL (bh), B_NR_ITEMS (bh), B_FREE_SPACE (bh));
}


static void sprintf_buffer_head (char * buf, struct buffer_head * bh) 
{
  char b[BDEVNAME_SIZE];

  sprintf (buf, "dev %s, size %d, blocknr %llu, count %d, state 0x%lx, page %p, (%s, %s, %s)",
	   bdevname (bh->b_bdev, b), bh->b_size,
	   (unsigned long long)bh->b_blocknr,
	   atomic_read (&(bh->b_count)),
	   bh->b_state, bh->b_page,
	   buffer_uptodate (bh) ? "UPTODATE" : "!UPTODATE",
	   buffer_dirty (bh) ? "DIRTY" : "CLEAN",
	   buffer_locked (bh) ? "LOCKED" : "UNLOCKED");
}


static void sprintf_disk_child (char * buf, struct disk_child * dc)
{
  sprintf (buf, "[dc_number=%d, dc_size=%u]", dc_block_number(dc), dc_size(dc));
}


static char * is_there_reiserfs_struct (char * fmt, int * what, int * skip)
{
  char * k = fmt;

  *skip = 0;
  
  while ((k = strchr (k, '%')) != NULL)
  {
    if (k[1] == 'k' || k[1] == 'K' || k[1] == 'h' || k[1] == 't' ||
	      k[1] == 'z' || k[1] == 'b' || k[1] == 'y' || k[1] == 'a' ) {
      *what = k[1];
      break;
    }
    (*skip) ++;
    k ++;
  }
  return k;
}


/* debugging reiserfs we used to print out a lot of different
   variables, like keys, item headers, buffer heads etc. Values of
   most fields matter. So it took a long time just to write
   appropriative printk. With this reiserfs_warning you can use format
   specification for complex structures like you used to do with
   printfs for integers, doubles and pointers. For instance, to print
   out key structure you have to write just: 
   reiserfs_warning ("bad key %k", key); 
   instead of 
   printk ("bad key %lu %lu %lu %lu", key->k_dir_id, key->k_objectid, 
           key->k_offset, key->k_uniqueness); 
*/


static void
prepare_error_buf( const char *fmt, va_list args )
{
    char * fmt1 = fmt_buf;
    char * k;
    char * p = error_buf;
    int i, j, what, skip;

    strcpy (fmt1, fmt);

    while( (k = is_there_reiserfs_struct( fmt1, &what, &skip )) != NULL )
    {
        *k = 0;

        p += vsprintf (p, fmt1, args);

        for (i = 0; i < skip; i ++)
            j = va_arg (args, int);

        switch (what) {
        case 'k':
            sprintf_le_key (p, va_arg(args, struct key *));
            break;
        case 'K':
            sprintf_cpu_key (p, va_arg(args, struct cpu_key *));
            break;
        case 'h':
            sprintf_item_head (p, va_arg(args, struct item_head *));
            break;
        case 't':
            sprintf_direntry (p, va_arg(args, struct reiserfs_dir_entry *));
            break;
        case 'y':
            sprintf_disk_child (p, va_arg(args, struct disk_child *));
            break;
        case 'z':
            sprintf_block_head (p, va_arg(args, struct buffer_head *));
            break;
        case 'b':
            sprintf_buffer_head (p, va_arg(args, struct buffer_head *));
            break;
        case 'a':
            sprintf_de_head (p, va_arg(args, struct reiserfs_de_head *));
            break;
        }

        p += strlen (p);
        fmt1 = k + 2;
    }
    vsprintf (p, fmt1, args);

}


/* in addition to usual conversion specifiers this accepts reiserfs
   specific conversion specifiers: 
   %k to print little endian key, 
   %K to print cpu key, 
   %h to print item_head,
   %t to print directory entry 
   %z to print block head (arg must be struct buffer_head *
   %b to print buffer_head
*/

#define do_reiserfs_warning(fmt)\
{\
    va_list args;\
    va_start( args, fmt );\
    prepare_error_buf( fmt, args );\
    va_end( args );\
}

void reiserfs_warning (struct super_block *sb, const char * fmt, ...)
{
  do_reiserfs_warning(fmt);
  if (sb)
      printk (KERN_WARNING "ReiserFS: %s: warning: %s\n",
             reiserfs_bdevname (sb), error_buf);
  else
      printk (KERN_WARNING "ReiserFS: warning: %s\n", error_buf);
}

/* No newline.. reiserfs_info calls can be followed by printk's */
void reiserfs_info (struct super_block *sb, const char * fmt, ...)
{
  do_reiserfs_warning(fmt);
  if (sb)
      printk (KERN_NOTICE "ReiserFS: %s: %s",
             reiserfs_bdevname (sb), error_buf);
  else
      printk (KERN_NOTICE "ReiserFS: %s", error_buf);
}

/* No newline.. reiserfs_printk calls can be followed by printk's */
void reiserfs_printk (const char * fmt, ...)
{
  do_reiserfs_warning(fmt);
  printk (error_buf);
}

void reiserfs_debug (struct super_block *s, int level, const char * fmt, ...)
{
#ifdef CONFIG_REISERFS_CHECK
  do_reiserfs_warning(fmt);
  if (s)
      printk (KERN_DEBUG "ReiserFS: %s: %s\n",
             reiserfs_bdevname (s), error_buf);
  else
      printk (KERN_DEBUG "ReiserFS: %s\n", error_buf);
#endif
}

/* The format:

           maintainer-errorid: [function-name:] message

    where errorid is unique to the maintainer and function-name is
    optional, is recommended, so that anyone can easily find the bug
    with a simple grep for the short to type string
    maintainer-errorid.  Don't bother with reusing errorids, there are
    lots of numbers out there.

    Example: 
    
    reiserfs_panic(
	p_sb, "reiser-29: reiserfs_new_blocknrs: "
	"one of search_start or rn(%d) is equal to MAX_B_NUM,"
	"which means that we are optimizing location based on the bogus location of a temp buffer (%p).", 
	rn, bh
    );

    Regular panic()s sometimes clear the screen before the message can
    be read, thus the need for the while loop.  

    Numbering scheme for panic used by Vladimir and Anatoly( Hans completely ignores this scheme, and considers it
    pointless complexity):

    panics in reiserfs_fs.h have numbers from 1000 to 1999
    super.c				        2000 to 2999
    preserve.c (unused)			    3000 to 3999
    bitmap.c				    4000 to 4999
    stree.c				        5000 to 5999
    prints.c				    6000 to 6999
    namei.c                     7000 to 7999
    fix_nodes.c                 8000 to 8999
    dir.c                       9000 to 9999
	lbalance.c					10000 to 10999
	ibalance.c		11000 to 11999 not ready
	do_balan.c		12000 to 12999
	inode.c			13000 to 13999
	file.c			14000 to 14999
    objectid.c                       15000 - 15999
    buffer.c                         16000 - 16999
    symlink.c                        17000 - 17999

   .  */


#ifdef CONFIG_REISERFS_CHECK
extern struct tree_balance * cur_tb;
#endif

void reiserfs_panic (struct super_block * sb, const char * fmt, ...)
{
  do_reiserfs_warning(fmt);
  printk (KERN_EMERG "REISERFS: panic (device %s): %s\n",
          reiserfs_bdevname (sb), error_buf);
  BUG ();

  /* this is not actually called, but makes reiserfs_panic() "noreturn" */
  panic ("REISERFS: panic (device %s): %s\n",
	 reiserfs_bdevname (sb), error_buf);
}


void print_virtual_node (struct virtual_node * vn)
{
    int i;
    struct virtual_item * vi;

    printk ("VIRTUAL NODE CONTAINS %d items, has size %d,%s,%s, ITEM_POS=%d POS_IN_ITEM=%d MODE=\'%c\'\n",
	    vn->vn_nr_item, vn->vn_size,
	    (vn->vn_vi[0].vi_type & VI_TYPE_LEFT_MERGEABLE )? "left mergeable" : "", 
	    (vn->vn_vi[vn->vn_nr_item - 1].vi_type & VI_TYPE_RIGHT_MERGEABLE) ? "right mergeable" : "",
	    vn->vn_affected_item_num, vn->vn_pos_in_item, vn->vn_mode);
    
    vi = vn->vn_vi;
    for (i = 0; i < vn->vn_nr_item; i ++, vi ++)
	op_print_vi (vi);
	
}


void print_path (struct tree_balance * tb, struct path * path)
{
    int h = 0;
    struct buffer_head * bh;
    
    if (tb) {
	while (tb->insert_size[h]) {
	    bh = PATH_H_PBUFFER (path, h);
	    printk ("block %llu (level=%d), position %d\n", bh ? (unsigned long long)bh->b_blocknr : 0LL,
		    bh ? B_LEVEL (bh) : 0, PATH_H_POSITION (path, h));
	    h ++;
	}
  } else {
      int offset = path->path_length;
      struct buffer_head * bh;
      printk ("Offset    Bh     (b_blocknr, b_count) Position Nr_item\n");
      while ( offset > ILLEGAL_PATH_ELEMENT_OFFSET ) {
	  bh = PATH_OFFSET_PBUFFER (path, offset);
	  printk ("%6d %10p (%9llu, %7d) %8d %7d\n", offset, 
		  bh, bh ? (unsigned long long)bh->b_blocknr : 0LL, bh ? atomic_read (&(bh->b_count)) : 0,
		  PATH_OFFSET_POSITION (path, offset), bh ? B_NR_ITEMS (bh) : -1);
	  
	  offset --;
      }
  }

}


/* this prints internal nodes (4 keys/items in line) (dc_number,
   dc_size)[k_dirid, k_objectid, k_offset, k_uniqueness](dc_number,
   dc_size)...*/
static int print_internal (struct buffer_head * bh, int first, int last)
{
    struct key * key;
    struct disk_child * dc;
    int i;
    int from, to;
    
    if (!B_IS_KEYS_LEVEL (bh))
	return 1;

    check_internal (bh);
    
    if (first == -1) {
	from = 0;
	to = B_NR_ITEMS (bh);
    } else {
	from = first;
	to = last < B_NR_ITEMS (bh) ? last : B_NR_ITEMS (bh);
    }

    reiserfs_printk ("INTERNAL NODE (%ld) contains %z\n",  bh->b_blocknr, bh);
    
    dc = B_N_CHILD (bh, from);
    reiserfs_printk ("PTR %d: %y ", from, dc);
    
    for (i = from, key = B_N_PDELIM_KEY (bh, from), dc ++; i < to; i ++, key ++, dc ++) {
	reiserfs_printk ("KEY %d: %k PTR %d: %y ", i, key, i + 1, dc);
	if (i && i % 4 == 0)
	    printk ("\n");
    }
    printk ("\n");
    return 0;
}





static int print_leaf (struct buffer_head * bh, int print_mode, int first, int last)
{
    struct block_head * blkh;
    struct item_head * ih;
    int i, nr;
    int from, to;

    if (!B_IS_ITEMS_LEVEL (bh))
	return 1;

    check_leaf (bh);

    blkh = B_BLK_HEAD (bh);
    ih = B_N_PITEM_HEAD (bh,0);
    nr = blkh_nr_item(blkh);

    printk ("\n===================================================================\n");
    reiserfs_printk ("LEAF NODE (%ld) contains %z\n", bh->b_blocknr, bh);

    if (!(print_mode & PRINT_LEAF_ITEMS)) {
	reiserfs_printk ("FIRST ITEM_KEY: %k, LAST ITEM KEY: %k\n",
			  &(ih->ih_key), &((ih + nr - 1)->ih_key));
	return 0;
    }

    if (first < 0 || first > nr - 1) 
	from = 0;
    else 
	from = first;

    if (last < 0 || last > nr )
	to = nr;
    else
	to = last;

    ih += from;
    printk ("-------------------------------------------------------------------------------\n");
    printk ("|##|   type    |           key           | ilen | free_space | version | loc  |\n");
    for (i = from; i < to; i++, ih ++) {
	printk ("-------------------------------------------------------------------------------\n");
	reiserfs_printk ("|%2d| %h |\n", i, ih);
	if (print_mode & PRINT_LEAF_ITEMS)
	    op_print_item (ih, B_I_PITEM (bh, ih));
    }

    printk ("===================================================================\n");

    return 0;
}

char * reiserfs_hashname(int code)
{
    if ( code == YURA_HASH)
	return "rupasov";
    if ( code == TEA_HASH)
	return "tea";
    if ( code == R5_HASH)
	return "r5";

    return "unknown";
}

/* return 1 if this is not super block */
static int print_super_block (struct buffer_head * bh)
{
    struct reiserfs_super_block * rs = (struct reiserfs_super_block *)(bh->b_data);
    int skipped, data_blocks;
    char *version;
    char b[BDEVNAME_SIZE];

    if (is_reiserfs_3_5(rs)) {
        version = "3.5";
    } else if (is_reiserfs_3_6(rs)) {
        version = "3.6";
    } else if (is_reiserfs_jr(rs)) {
      version = ((sb_version(rs) == REISERFS_VERSION_2) ?
 		 "3.6" : "3.5");  
    } else {
	return 1;
    }

    printk ("%s\'s super block is in block %llu\n", bdevname (bh->b_bdev, b),
            (unsigned long long)bh->b_blocknr);
    printk ("Reiserfs version %s\n", version );
    printk ("Block count %u\n", sb_block_count(rs));
    printk ("Blocksize %d\n", sb_blocksize(rs));
    printk ("Free blocks %u\n", sb_free_blocks(rs));
    // FIXME: this would be confusing if
    // someone stores reiserfs super block in some data block ;)
//    skipped = (bh->b_blocknr * bh->b_size) / sb_blocksize(rs);
    skipped = bh->b_blocknr;
    data_blocks = sb_block_count(rs) - skipped - 1 - sb_bmap_nr(rs) -
	    (!is_reiserfs_jr(rs) ? sb_jp_journal_size(rs) + 1 : sb_reserved_for_journal(rs)) -	    
	    sb_free_blocks(rs);
    printk ("Busy blocks (skipped %d, bitmaps - %d, journal (or reserved) blocks - %d\n"
	    "1 super block, %d data blocks\n", 
	    skipped, sb_bmap_nr(rs), (!is_reiserfs_jr(rs) ? (sb_jp_journal_size(rs) + 1) :
				      sb_reserved_for_journal(rs)) , data_blocks);
    printk ("Root block %u\n", sb_root_block(rs));
    printk ("Journal block (first) %d\n", sb_jp_journal_1st_block(rs));
    printk ("Journal dev %d\n", sb_jp_journal_dev(rs));
    printk ("Journal orig size %d\n", sb_jp_journal_size(rs));
    printk ("FS state %d\n", sb_fs_state(rs));
    printk ("Hash function \"%s\"\n",
	    reiserfs_hashname(sb_hash_function_code(rs)));
    
    printk ("Tree height %d\n", sb_tree_height(rs));
    return 0;
}

static int print_desc_block (struct buffer_head * bh)
{
    struct reiserfs_journal_desc * desc;

    if (memcmp(get_journal_desc_magic (bh), JOURNAL_DESC_MAGIC, 8))
	return 1;

    desc = (struct reiserfs_journal_desc *)(bh->b_data);
    printk ("Desc block %llu (j_trans_id %d, j_mount_id %d, j_len %d)",
	    (unsigned long long)bh->b_blocknr, get_desc_trans_id (desc), get_desc_mount_id (desc),
	    get_desc_trans_len (desc));

    return 0;
}


void print_block (struct buffer_head * bh, ...)//int print_mode, int first, int last)
{
    va_list args;
    int mode, first, last;

    va_start (args, bh);

    if ( ! bh ) {
	printk("print_block: buffer is NULL\n");
	return;
    }

    mode = va_arg (args, int);
    first = va_arg (args, int);
    last = va_arg (args, int);
    if (print_leaf (bh, mode, first, last))
	if (print_internal (bh, first, last))
	    if (print_super_block (bh))
		if (print_desc_block (bh))
		    printk ("Block %llu contains unformatted data\n", (unsigned long long)bh->b_blocknr);
}



char print_tb_buf[2048];

/* this stores initial state of tree balance in the print_tb_buf */
void store_print_tb (struct tree_balance * tb)
{
    int h = 0;
    int i;
    struct buffer_head * tbSh, * tbFh;

    if (!tb)
	return;

    sprintf (print_tb_buf, "\n"
	     "BALANCING %d\n"
	     "MODE=%c, ITEM_POS=%d POS_IN_ITEM=%d\n" 
	     "=====================================================================\n"
	     "* h *    S    *    L    *    R    *   F   *   FL  *   FR  *  CFL  *  CFR  *\n",
	     REISERFS_SB(tb->tb_sb)->s_do_balance,
	     tb->tb_mode, PATH_LAST_POSITION (tb->tb_path), tb->tb_path->pos_in_item);
  
    for (h = 0; h < sizeof(tb->insert_size) / sizeof (tb->insert_size[0]); h ++) {
	if (PATH_H_PATH_OFFSET (tb->tb_path, h) <= tb->tb_path->path_length && 
	    PATH_H_PATH_OFFSET (tb->tb_path, h) > ILLEGAL_PATH_ELEMENT_OFFSET) {
	    tbSh = PATH_H_PBUFFER (tb->tb_path, h);
	    tbFh = PATH_H_PPARENT (tb->tb_path, h);
	} else {
	    tbSh = NULL;
	    tbFh = NULL;
	}
	sprintf (print_tb_buf + strlen (print_tb_buf),
		 "* %d * %3lld(%2d) * %3lld(%2d) * %3lld(%2d) * %5lld * %5lld * %5lld * %5lld * %5lld *\n",
		 h, 
		 (tbSh) ? (long long)(tbSh->b_blocknr):(-1LL),
		 (tbSh) ? atomic_read (&(tbSh->b_count)) : -1,
		 (tb->L[h]) ? (long long)(tb->L[h]->b_blocknr):(-1LL),
		 (tb->L[h]) ? atomic_read (&(tb->L[h]->b_count)) : -1,
		 (tb->R[h]) ? (long long)(tb->R[h]->b_blocknr):(-1LL),
		 (tb->R[h]) ? atomic_read (&(tb->R[h]->b_count)) : -1,
		 (tbFh) ? (long long)(tbFh->b_blocknr):(-1LL),
		 (tb->FL[h]) ? (long long)(tb->FL[h]->b_blocknr):(-1LL),
		 (tb->FR[h]) ? (long long)(tb->FR[h]->b_blocknr):(-1LL),
		 (tb->CFL[h]) ? (long long)(tb->CFL[h]->b_blocknr):(-1LL),
		 (tb->CFR[h]) ? (long long)(tb->CFR[h]->b_blocknr):(-1LL));
    }

    sprintf (print_tb_buf + strlen (print_tb_buf), 
	     "=====================================================================\n"
	     "* h * size * ln * lb * rn * rb * blkn * s0 * s1 * s1b * s2 * s2b * curb * lk * rk *\n"
	     "* 0 * %4d * %2d * %2d * %2d * %2d * %4d * %2d * %2d * %3d * %2d * %3d * %4d * %2d * %2d *\n",
	     tb->insert_size[0], tb->lnum[0], tb->lbytes, tb->rnum[0],tb->rbytes, tb->blknum[0], 
	     tb->s0num, tb->s1num,tb->s1bytes,  tb->s2num, tb->s2bytes, tb->cur_blknum, tb->lkey[0], tb->rkey[0]);

    /* this prints balance parameters for non-leaf levels */
    h = 0;
    do {
	h++;
	sprintf (print_tb_buf + strlen (print_tb_buf),
		 "* %d * %4d * %2d *    * %2d *    * %2d *\n",
		h, tb->insert_size[h], tb->lnum[h], tb->rnum[h], tb->blknum[h]);
    } while (tb->insert_size[h]);

    sprintf (print_tb_buf + strlen (print_tb_buf), 
	     "=====================================================================\n"
	     "FEB list: ");

    /* print FEB list (list of buffers in form (bh (b_blocknr, b_count), that will be used for new nodes) */
    h = 0;
    for (i = 0; i < sizeof (tb->FEB) / sizeof (tb->FEB[0]); i ++)
	sprintf (print_tb_buf + strlen (print_tb_buf),
		 "%p (%llu %d)%s", tb->FEB[i], tb->FEB[i] ? (unsigned long long)tb->FEB[i]->b_blocknr : 0ULL,
		 tb->FEB[i] ? atomic_read (&(tb->FEB[i]->b_count)) : 0, 
		 (i == sizeof (tb->FEB) / sizeof (tb->FEB[0]) - 1) ? "\n" : ", ");

    sprintf (print_tb_buf + strlen (print_tb_buf), 
	     "======================== the end ====================================\n");
}

void print_cur_tb (char * mes)
{
    printk ("%s\n%s", mes, print_tb_buf);
}

static void check_leaf_block_head (struct buffer_head * bh)
{
  struct block_head * blkh;
  int nr;

  blkh = B_BLK_HEAD (bh);
  nr = blkh_nr_item(blkh);
  if ( nr > (bh->b_size - BLKH_SIZE) / IH_SIZE)
    reiserfs_panic (NULL, "vs-6010: check_leaf_block_head: invalid item number %z", bh);
  if ( blkh_free_space(blkh) > 
      bh->b_size - BLKH_SIZE - IH_SIZE * nr )
    reiserfs_panic (NULL, "vs-6020: check_leaf_block_head: invalid free space %z", bh);
    
}

static void check_internal_block_head (struct buffer_head * bh)
{
    struct block_head * blkh;
    
    blkh = B_BLK_HEAD (bh);
    if (!(B_LEVEL (bh) > DISK_LEAF_NODE_LEVEL && B_LEVEL (bh) <= MAX_HEIGHT))
	reiserfs_panic (NULL, "vs-6025: check_internal_block_head: invalid level %z", bh);

    if (B_NR_ITEMS (bh) > (bh->b_size - BLKH_SIZE) / IH_SIZE)
	reiserfs_panic (NULL, "vs-6030: check_internal_block_head: invalid item number %z", bh);

    if (B_FREE_SPACE (bh) != 
	bh->b_size - BLKH_SIZE - KEY_SIZE * B_NR_ITEMS (bh) - DC_SIZE * (B_NR_ITEMS (bh) + 1))
	reiserfs_panic (NULL, "vs-6040: check_internal_block_head: invalid free space %z", bh);

}


void check_leaf (struct buffer_head * bh)
{
    int i;
    struct item_head * ih;

    if (!bh)
	return;
    check_leaf_block_head (bh);
    for (i = 0, ih = B_N_PITEM_HEAD (bh, 0); i < B_NR_ITEMS (bh); i ++, ih ++)
	op_check_item (ih, B_I_PITEM (bh, ih));
}


void check_internal (struct buffer_head * bh)
{
  if (!bh)
    return;
  check_internal_block_head (bh);
}


void print_statistics (struct super_block * s)
{

  /*
  printk ("reiserfs_put_super: session statistics: balances %d, fix_nodes %d, \
bmap with search %d, without %d, dir2ind %d, ind2dir %d\n",
	  REISERFS_SB(s)->s_do_balance, REISERFS_SB(s)->s_fix_nodes,
	  REISERFS_SB(s)->s_bmaps, REISERFS_SB(s)->s_bmaps_without_search,
	  REISERFS_SB(s)->s_direct2indirect, REISERFS_SB(s)->s_indirect2direct);
  */

}
