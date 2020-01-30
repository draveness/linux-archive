/*
	drivers/net/tulip/eeprom.c

	Maintained by Jeff Garzik <jgarzik@mandrakesoft.com>
	Copyright 2000  The Linux Kernel Team
	Written/copyright 1994-1999 by Donald Becker.

	This software may be used and distributed according to the terms
	of the GNU Public License, incorporated herein by reference.

	Please refer to Documentation/networking/tulip.txt for more
	information on this driver.

*/

#include "tulip.h"
#include <linux/init.h>
#include <asm/unaligned.h>



/* Serial EEPROM section. */
/* The main routine to parse the very complicated SROM structure.
   Search www.digital.com for "21X4 SROM" to get details.
   This code is very complex, and will require changes to support
   additional cards, so I'll be verbose about what is going on.
   */

/* Known cards that have old-style EEPROMs. */
static struct eeprom_fixup eeprom_fixups[] __devinitdata = {
  {"Asante", 0, 0, 0x94, {0x1e00, 0x0000, 0x0800, 0x0100, 0x018c,
			  0x0000, 0x0000, 0xe078, 0x0001, 0x0050, 0x0018 }},
  {"SMC9332DST", 0, 0, 0xC0, { 0x1e00, 0x0000, 0x0800, 0x041f,
			   0x0000, 0x009E, /* 10baseT */
			   0x0004, 0x009E, /* 10baseT-FD */
			   0x0903, 0x006D, /* 100baseTx */
			   0x0905, 0x006D, /* 100baseTx-FD */ }},
  {"Cogent EM100", 0, 0, 0x92, { 0x1e00, 0x0000, 0x0800, 0x063f,
				 0x0107, 0x8021, /* 100baseFx */
				 0x0108, 0x8021, /* 100baseFx-FD */
				 0x0100, 0x009E, /* 10baseT */
				 0x0104, 0x009E, /* 10baseT-FD */
				 0x0103, 0x006D, /* 100baseTx */
				 0x0105, 0x006D, /* 100baseTx-FD */ }},
  {"Maxtech NX-110", 0, 0, 0xE8, { 0x1e00, 0x0000, 0x0800, 0x0513,
				   0x1001, 0x009E, /* 10base2, CSR12 0x10*/
				   0x0000, 0x009E, /* 10baseT */
				   0x0004, 0x009E, /* 10baseT-FD */
				   0x0303, 0x006D, /* 100baseTx, CSR12 0x03 */
				   0x0305, 0x006D, /* 100baseTx-FD CSR12 0x03 */}},
  {"Accton EN1207", 0, 0, 0xE8, { 0x1e00, 0x0000, 0x0800, 0x051F,
				  0x1B01, 0x0000, /* 10base2,   CSR12 0x1B */
				  0x0B00, 0x009E, /* 10baseT,   CSR12 0x0B */
				  0x0B04, 0x009E, /* 10baseT-FD,CSR12 0x0B */
				  0x1B03, 0x006D, /* 100baseTx, CSR12 0x1B */
				  0x1B05, 0x006D, /* 100baseTx-FD CSR12 0x1B */
   }},
  {"NetWinder", 0x00, 0x10, 0x57,
	/* Default media = MII
	 * MII block, reset sequence (3) = 0x0821 0x0000 0x0001, capabilities 0x01e1
	 */
	{ 0x1e00, 0x0000, 0x000b, 0x8f01, 0x0103, 0x0300, 0x0821, 0x000, 0x0001, 0x0000, 0x01e1 }
  },
  {0, 0, 0, 0, {}}};


static const char *block_name[] __devinitdata = {
	"21140 non-MII",
	"21140 MII PHY",
	"21142 Serial PHY",
	"21142 MII PHY",
	"21143 SYM PHY",
	"21143 reset method"
};


void __devinit tulip_parse_eeprom(struct net_device *dev)
{
	/* The last media info list parsed, for multiport boards.  */
	static struct mediatable *last_mediatable = NULL;
	static unsigned char *last_ee_data = NULL;
	static int controller_index = 0;
	struct tulip_private *tp = (struct tulip_private *)dev->priv;
	unsigned char *ee_data = tp->eeprom;
	int i;

	tp->mtable = 0;
	/* Detect an old-style (SA only) EEPROM layout:
	   memcmp(eedata, eedata+16, 8). */
	for (i = 0; i < 8; i ++)
		if (ee_data[i] != ee_data[16+i])
			break;
	if (i >= 8) {
		if (ee_data[0] == 0xff) {
			if (last_mediatable) {
				controller_index++;
				printk(KERN_INFO "%s:  Controller %d of multiport board.\n",
					   dev->name, controller_index);
				tp->mtable = last_mediatable;
				ee_data = last_ee_data;
				goto subsequent_board;
			} else
				printk(KERN_INFO "%s:  Missing EEPROM, this interface may "
					   "not work correctly!\n",
			   dev->name);
			return;
		}
	  /* Do a fix-up based on the vendor half of the station address prefix. */
	  for (i = 0; eeprom_fixups[i].name; i++) {
		if (dev->dev_addr[0] == eeprom_fixups[i].addr0
			&&  dev->dev_addr[1] == eeprom_fixups[i].addr1
			&&  dev->dev_addr[2] == eeprom_fixups[i].addr2) {
		  if (dev->dev_addr[2] == 0xE8  &&  ee_data[0x1a] == 0x55)
			  i++;			/* An Accton EN1207, not an outlaw Maxtech. */
		  memcpy(ee_data + 26, eeprom_fixups[i].newtable,
				 sizeof(eeprom_fixups[i].newtable));
		  printk(KERN_INFO "%s: Old format EEPROM on '%s' board.  Using"
				 " substitute media control info.\n",
				 dev->name, eeprom_fixups[i].name);
		  break;
		}
	  }
	  if (eeprom_fixups[i].name == NULL) { /* No fixup found. */
		  printk(KERN_INFO "%s: Old style EEPROM with no media selection "
				 "information.\n",
			   dev->name);
		return;
	  }
	}

	controller_index = 0;
	if (ee_data[19] > 1) {		/* Multiport board. */
		last_ee_data = ee_data;
	}
subsequent_board:

	if (ee_data[27] == 0) {		/* No valid media table. */
	} else if (tp->chip_id == DC21041) {
		unsigned char *p = (void *)ee_data + ee_data[27 + controller_index*3];
		int media = get_u16(p);
		int count = p[2];
		p += 3;

		printk(KERN_INFO "%s: 21041 Media table, default media %4.4x (%s).\n",
			   dev->name, media,
			   media & 0x0800 ? "Autosense" : medianame[media & 15]);
		for (i = 0; i < count; i++) {
			unsigned char media_code = *p++;
			if (media_code & 0x40)
				p += 6;
			printk(KERN_INFO "%s:  21041 media #%d, %s.\n",
				   dev->name, media_code & 15, medianame[media_code & 15]);
		}
	} else {
		unsigned char *p = (void *)ee_data + ee_data[27];
		unsigned char csr12dir = 0;
		int count, new_advertise = 0;
		struct mediatable *mtable;
		u16 media = get_u16(p);

		p += 2;
		if (tp->flags & CSR12_IN_SROM)
			csr12dir = *p++;
		count = *p++;

	        /* there is no phy information, don't even try to build mtable */
	        if (count == 0) {
			DPRINTK("no phy info, aborting mtable build\n");
		        return;
		}

		mtable = (struct mediatable *)
			kmalloc(sizeof(struct mediatable) + count*sizeof(struct medialeaf),
					GFP_KERNEL);
		if (mtable == NULL)
			return;				/* Horrible, impossible failure. */
		last_mediatable = tp->mtable = mtable;
		mtable->defaultmedia = media;
		mtable->leafcount = count;
		mtable->csr12dir = csr12dir;
		mtable->has_nonmii = mtable->has_mii = mtable->has_reset = 0;
		mtable->csr15dir = mtable->csr15val = 0;

		printk(KERN_INFO "%s:  EEPROM default media type %s.\n", dev->name,
			   media & 0x0800 ? "Autosense" : medianame[media & 15]);
		for (i = 0; i < count; i++) {
			struct medialeaf *leaf = &mtable->mleaf[i];

			if ((p[0] & 0x80) == 0) { /* 21140 Compact block. */
				leaf->type = 0;
				leaf->media = p[0] & 0x3f;
				leaf->leafdata = p;
				if ((p[2] & 0x61) == 0x01)	/* Bogus, but Znyx boards do it. */
					mtable->has_mii = 1;
				p += 4;
			} else {
				leaf->type = p[1];
				if (p[1] == 0x05) {
					mtable->has_reset = i;
					leaf->media = p[2] & 0x0f;
				} else if (tp->chip_id == DM910X && p[1] == 0x80) {
					/* Hack to ignore Davicom delay period block */
					mtable->leafcount--;
					count--;
					i--;
					leaf->leafdata = p + 2;
					p += (p[0] & 0x3f) + 1;
					continue;
				} else if (p[1] & 1) {
					mtable->has_mii = 1;
					leaf->media = 11;
				} else {
					mtable->has_nonmii = 1;
					leaf->media = p[2] & 0x0f;
					/* Davicom's media number for 100BaseTX is strange */
					if (tp->chip_id == DM910X && leaf->media == 1)
						leaf->media = 3;
					switch (leaf->media) {
					case 0: new_advertise |= 0x0020; break;
					case 4: new_advertise |= 0x0040; break;
					case 3: new_advertise |= 0x0080; break;
					case 5: new_advertise |= 0x0100; break;
					case 6: new_advertise |= 0x0200; break;
					}
					if (p[1] == 2  &&  leaf->media == 0) {
						if (p[2] & 0x40) {
							u32 base15 = get_unaligned((u16*)&p[7]);
							mtable->csr15dir =
								(get_unaligned((u16*)&p[9])<<16) + base15;
							mtable->csr15val =
								(get_unaligned((u16*)&p[11])<<16) + base15;
						} else {
							mtable->csr15dir = get_unaligned((u16*)&p[3])<<16;
							mtable->csr15val = get_unaligned((u16*)&p[5])<<16;
						}
					}
				}
				leaf->leafdata = p + 2;
				p += (p[0] & 0x3f) + 1;
			}
			if (tulip_debug > 1  &&  leaf->media == 11) {
				unsigned char *bp = leaf->leafdata;
				printk(KERN_INFO "%s:  MII interface PHY %d, setup/reset "
					   "sequences %d/%d long, capabilities %2.2x %2.2x.\n",
					   dev->name, bp[0], bp[1], bp[2 + bp[1]*2],
					   bp[5 + bp[2 + bp[1]*2]*2], bp[4 + bp[2 + bp[1]*2]*2]);
			}
			printk(KERN_INFO "%s:  Index #%d - Media %s (#%d) described "
				   "by a %s (%d) block.\n",
				   dev->name, i, medianame[leaf->media], leaf->media,
				   leaf->type >= ARRAY_SIZE(block_name) ? "UNKNOWN" :
				   block_name[leaf->type], leaf->type);
		}
		if (new_advertise)
			tp->to_advertise = new_advertise;
	}
}
/* Reading a serial EEPROM is a "bit" grungy, but we work our way through:->.*/

/* Note: this routine returns extra data bits for size detection. */
int __devinit tulip_read_eeprom(long ioaddr, int location, int addr_len)
{
	int i;
	unsigned retval = 0;
	long ee_addr = ioaddr + CSR9;
	int read_cmd = location | (EE_READ_CMD << addr_len);

	outl(EE_ENB & ~EE_CS, ee_addr);
	outl(EE_ENB, ee_addr);

	/* Shift the read command bits out. */
	for (i = 4 + addr_len; i >= 0; i--) {
		short dataval = (read_cmd & (1 << i)) ? EE_DATA_WRITE : 0;
		outl(EE_ENB | dataval, ee_addr);
		eeprom_delay();
		outl(EE_ENB | dataval | EE_SHIFT_CLK, ee_addr);
		eeprom_delay();
		retval = (retval << 1) | ((inl(ee_addr) & EE_DATA_READ) ? 1 : 0);
	}
	outl(EE_ENB, ee_addr);

	for (i = 16; i > 0; i--) {
		outl(EE_ENB | EE_SHIFT_CLK, ee_addr);
		eeprom_delay();
		retval = (retval << 1) | ((inl(ee_addr) & EE_DATA_READ) ? 1 : 0);
		outl(EE_ENB, ee_addr);
		eeprom_delay();
	}

	/* Terminate the EEPROM access. */
	outl(EE_ENB & ~EE_CS, ee_addr);
	return retval;
}

