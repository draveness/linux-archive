/*
 *  fs/partitions/sgi.c
 *
 *  Code extracted from drivers/block/genhd.c
 */

#include "check.h"
#include "sgi.h"

struct sgi_disklabel {
	s32 magic_mushroom;		/* Big fat spliff... */
	s16 root_part_num;		/* Root partition number */
	s16 swap_part_num;		/* Swap partition number */
	s8 boot_file[16];		/* Name of boot file for ARCS */
	u8 _unused0[48];		/* Device parameter useless crapola.. */
	struct sgi_volume {
		s8 name[8];		/* Name of volume */
		s32 block_num;		/* Logical block number */
		s32 num_bytes;		/* How big, in bytes */
	} volume[15];
	struct sgi_partition {
		u32 num_blocks;		/* Size in logical blocks */
		u32 first_block;	/* First logical block */
		s32 type;		/* Type of this partition */
	} partitions[16];
	s32 csum;			/* Disk label checksum */
	s32 _unused1;			/* Padding */
};

int sgi_partition(struct parsed_partitions *state, struct block_device *bdev)
{
	int i, csum, magic;
	int slot = 1;
	unsigned int *ui, start, blocks, cs;
	Sector sect;
	struct sgi_disklabel *label;
	struct sgi_partition *p;
	char b[BDEVNAME_SIZE];

	label = (struct sgi_disklabel *) read_dev_sector(bdev, 0, &sect);
	if (!label)
		return -1;
	p = &label->partitions[0];
	magic = label->magic_mushroom;
	if(be32_to_cpu(magic) != SGI_LABEL_MAGIC) {
		/*printk("Dev %s SGI disklabel: bad magic %08x\n",
		       bdevname(bdev, b), magic);*/
		put_dev_sector(sect);
		return 0;
	}
	ui = ((unsigned int *) (label + 1)) - 1;
	for(csum = 0; ui >= ((unsigned int *) label);) {
		cs = *ui--;
		csum += be32_to_cpu(cs);
	}
	if(csum) {
		printk(KERN_WARNING "Dev %s SGI disklabel: csum bad, label corrupted\n",
		       bdevname(bdev, b));
		put_dev_sector(sect);
		return 0;
	}
	/* All SGI disk labels have 16 partitions, disks under Linux only
	 * have 15 minor's.  Luckily there are always a few zero length
	 * partitions which we don't care about so we never overflow the
	 * current_minor.
	 */
	for(i = 0; i < 16; i++, p++) {
		blocks = be32_to_cpu(p->num_blocks);
		start  = be32_to_cpu(p->first_block);
		if (blocks)
			put_partition(state, slot++, start, blocks);
	}
	printk("\n");
	put_dev_sector(sect);
	return 1;
}
