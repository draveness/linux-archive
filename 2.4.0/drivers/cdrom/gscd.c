#define GSCD_VERSION "0.4a Oliver Raupach <raupach@nwfs1.rz.fh-hannover.de>"

/*
	linux/drivers/block/gscd.c - GoldStar R420 CDROM driver

        Copyright (C) 1995  Oliver Raupach <raupach@nwfs1.rz.fh-hannover.de>
        based upon pre-works by   Eberhard Moenkeberg <emoenke@gwdg.de>
        

        For all kind of other information about the GoldStar CDROM
        and this Linux device driver I installed a WWW-URL:
        http://linux.rz.fh-hannover.de/~raupach        


             If you are the editor of a Linux CD, you should
             enable gscd.c within your boot floppy kernel and
             send me one of your CDs for free.


        --------------------------------------------------------------------
	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2, or (at your option)
	any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
	
	--------------------------------------------------------------------
	
	9 November 1999 -- Make kernel-parameter implementation work with 2.3.x 
	                   Removed init_module & cleanup_module in favor of 
		   	   module_init & module_exit.
			   Torben Mathiasen <tmm@image.dk>

*/

/* These settings are for various debug-level. Leave they untouched ... */ 
#define  NO_GSCD_DEBUG 
#define  NO_IOCTL_DEBUG
#define  NO_MODULE_DEBUG
#define  NO_FUTURE_WORK
/*------------------------*/

#include <linux/module.h>

#include <linux/malloc.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/cdrom.h>
#include <linux/ioport.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/devfs_fs_kernel.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/uaccess.h>

#define MAJOR_NR GOLDSTAR_CDROM_MAJOR
#include <linux/blk.h>
#define gscd_port gscd /* for compatible parameter passing with "insmod" */
#include "gscd.h"

static int gscd_blocksizes[1] = {512};

static int gscdPresent            = 0;

static unsigned char gscd_buf[2048];    /* buffer for block size conversion */
static int   gscd_bn              = -1;
static short gscd_port            = GSCD_BASE_ADDR;
MODULE_PARM(gscd, "h");

/* Kommt spaeter vielleicht noch mal dran ...
 *    static DECLARE_WAIT_QUEUE_HEAD(gscd_waitq);
 */ 

static void gscd_transfer         (void);
static void gscd_read_cmd         (void);
static void gscd_hsg2msf          (long hsg, struct msf *msf);
static void gscd_bin2bcd          (unsigned char *p);

/* Schnittstellen zum Kern/FS */

static void do_gscd_request       (request_queue_t *);
static void __do_gscd_request     (unsigned long dummy);
static int  gscd_ioctl            (struct inode *, struct file *, unsigned int, unsigned long);
static int  gscd_open             (struct inode *, struct file *);
static int  gscd_release          (struct inode *, struct file *);
static int  check_gscd_med_chg    (kdev_t);

/*      GoldStar Funktionen    */

static void cc_Reset             (void);
static int  wait_drv_ready       (void);
static int  find_drives          (void);
static void cmd_out              (int, char *, char *, int);
static void cmd_status           (void);
static void cc_Ident             (char *);
static void cc_SetSpeed          (void);
static void init_cd_drive        (int);

static int  get_status           (void);
static void clear_Audio          (void);
static void cc_invalidate        (void);

/* some things for the next version */
#ifdef FUTURE_WORK
static void update_state          (void);
static long gscd_msf2hsg          (struct msf *mp);
static int  gscd_bcd2bin          (unsigned char bcd);
#endif

/*    common GoldStar Initialization    */

static int my_gscd_init (void);


/*      lo-level cmd-Funktionen    */

static void cmd_info_in          ( char *, int );
static void cmd_end              ( void );
static void cmd_read_b           ( char *, int, int );
static void cmd_read_w           ( char *, int, int );
static int  cmd_unit_alive       ( void );
static void cmd_write_cmd        ( char * );


/*      GoldStar Variablen     */

static int  curr_drv_state;
static int  drv_states[]       = {0,0,0,0,0,0,0,0};
static int  drv_mode;
static int  disk_state;
static int  speed;
static int  ndrives;

static unsigned char drv_num_read;
static unsigned char f_dsk_valid;
static unsigned char current_drive;
static unsigned char f_drv_ok;


static char f_AudioPlay;
static char f_AudioPause;
static int  AudioStart_m;
static int  AudioStart_f;
static int  AudioEnd_m;
static int  AudioEnd_f;
 
static struct timer_list gscd_timer;

static struct block_device_operations gscd_fops = {
	open:			gscd_open,
	release:		gscd_release,
	ioctl:			gscd_ioctl,
	check_media_change:	check_gscd_med_chg,
};

/* 
 * Checking if the media has been changed
 * (not yet implemented)
 */
static int check_gscd_med_chg (kdev_t full_dev)
{
   int target;


   target = MINOR(full_dev);

   if (target > 0) 
   {
      printk("GSCD: GoldStar CD-ROM request error: invalid device.\n");
      return 0;
   }

   #ifdef GSCD_DEBUG
   printk ("gscd: check_med_change\n");
   #endif
 
  return 0;
}


#ifndef MODULE
/* Using new interface for kernel-parameters */
  
static int __init gscd_setup (char *str)
{
  int ints[2];
  (void)get_options(str, ARRAY_SIZE(ints), ints);
  
  if (ints[0] > 0) 
  {
     gscd_port = ints[1];
  }
 return 1;
}

__setup("gscd=", gscd_setup);

#endif

static int gscd_ioctl (struct inode *ip, struct file *fp, unsigned int cmd, unsigned long arg)
{
unsigned char to_do[10];
unsigned char dummy;

                             
    switch (cmd)
    {
       case CDROMSTART:     /* Spin up the drive */
		/* Don't think we can do this.  Even if we could,
 		 * I think the drive times out and stops after a while
		 * anyway.  For now, ignore it.
		 */
            return 0;

       case CDROMRESUME:   /* keine Ahnung was das ist */
            return 0;


       case CDROMEJECT:
            cmd_status ();
            to_do[0] = CMD_TRAY_CTL;
            cmd_out (TYPE_INFO, (char *)&to_do, (char *)&dummy, 0);

            return 0;

       default:
            return -EINVAL;
    }

}


/*
 * Take care of the different block sizes between cdrom and Linux.
 * When Linux gets variable block sizes this will probably go away.
 */

static void gscd_transfer (void)
{
long offs;

	while (CURRENT -> nr_sectors > 0 && gscd_bn == CURRENT -> sector / 4)
	{
		offs = (CURRENT -> sector & 3) * 512;
		memcpy(CURRENT -> buffer, gscd_buf + offs, 512);
		CURRENT -> nr_sectors--;
		CURRENT -> sector++;
		CURRENT -> buffer += 512;
	}
}


/*
 * I/O request routine called from Linux kernel.
 */

static void do_gscd_request (request_queue_t * q)
{
  __do_gscd_request(0);
}

static void __do_gscd_request (unsigned long dummy)
{
unsigned int block,dev;
unsigned int nsect;

repeat:
	if (QUEUE_EMPTY || CURRENT->rq_status == RQ_INACTIVE)
		goto out;
	INIT_REQUEST;
	dev = MINOR(CURRENT->rq_dev);
	block = CURRENT->sector;
	nsect = CURRENT->nr_sectors;

	if (QUEUE_EMPTY || CURRENT -> sector == -1)
		goto out;

	if (CURRENT -> cmd != READ)
	{
		printk("GSCD: bad cmd %d\n", CURRENT -> cmd);
		end_request(0);
		goto repeat;
	}

	if (MINOR(CURRENT -> rq_dev) != 0)
	{
		printk("GSCD: this version supports only one device\n");
		end_request(0);
		goto repeat;
	}

	gscd_transfer();

	/* if we satisfied the request from the buffer, we're done. */

	if (CURRENT -> nr_sectors == 0)
	{
		end_request(1);
		goto repeat;
	}

#ifdef GSCD_DEBUG
        printk ("GSCD: dev %d, block %d, nsect %d\n", dev, block, nsect );
#endif

	gscd_read_cmd ();
out:
	return;
}



/*
 * Check the result of the set-mode command.  On success, send the
 * read-data command.
 */

static void
gscd_read_cmd (void)
{
long   block;
struct gscd_Play_msf gscdcmd;
char   cmd[] = { CMD_READ, 0x80, 0,0,0, 0,1 }; /* cmd mode M-S-F secth sectl */



        cmd_status ();
        if ( disk_state & (ST_NO_DISK | ST_DOOR_OPEN) )
        {
           printk ( "GSCD: no disk or door open\n" );
           end_request (0);
        }
        else
        {
           if ( disk_state & ST_INVALID )
           {
              printk ( "GSCD: disk invalid\n" );
              end_request (0);
           }
           else
           {
              gscd_bn = -1;		/* purge our buffer */
              block = CURRENT -> sector / 4;
              gscd_hsg2msf(block, &gscdcmd.start);	/* cvt to msf format */

              cmd[2] = gscdcmd.start.min;
              cmd[3] = gscdcmd.start.sec;
              cmd[4] = gscdcmd.start.frame;

#ifdef GSCD_DEBUG
              printk ("GSCD: read msf %d:%d:%d\n", cmd[2], cmd[3], cmd[4] ); 
#endif 
              cmd_out ( TYPE_DATA, (char *)&cmd, (char *)&gscd_buf[0], 1 );
        
              gscd_bn = CURRENT -> sector / 4;
              gscd_transfer();
              end_request(1);
	   }
	}
	SET_TIMER(__do_gscd_request, 1);
}


/*
 * Open the device special file.  Check that a disk is in.
 */

static int gscd_open (struct inode *ip, struct file *fp)
{
int   st;

#ifdef GSCD_DEBUG
printk ( "GSCD: open\n" );
#endif

	if (gscdPresent == 0)
		return -ENXIO;			/* no hardware */

	MOD_INC_USE_COUNT;

        get_status ();
        st = disk_state & (ST_NO_DISK | ST_DOOR_OPEN);
        if ( st )
        {
           printk ( "GSCD: no disk or door open\n" );
           MOD_DEC_USE_COUNT;
           return -ENXIO;
        }
                   
/*	if (updateToc() < 0)
		return -EIO;
*/

	return 0;
}


/*
 * On close, we flush all gscd blocks from the buffer cache.
 */

static int gscd_release (struct inode * inode, struct file * file)
{

#ifdef GSCD_DEBUG
printk ( "GSCD: release\n" );
#endif

	gscd_bn = -1;

	MOD_DEC_USE_COUNT;
	return 0;
}


int get_status (void)
{
int  status;

    cmd_status ();
    status = disk_state & (ST_x08 | ST_x04 | ST_INVALID | ST_x01);
     
    if ( status == (ST_x08 | ST_x04 | ST_INVALID | ST_x01) )
    {
       cc_invalidate ();
       return 1;
    }
    else
    {
       return 0;
    }
}


void cc_invalidate (void)
{
   drv_num_read  = 0xFF;
   f_dsk_valid   = 0xFF;
   current_drive = 0xFF;
   f_drv_ok      = 0xFF;

   clear_Audio ();
   
}   

void clear_Audio (void)
{

   f_AudioPlay = 0;
   f_AudioPause = 0;
   AudioStart_m = 0;
   AudioStart_f = 0;
   AudioEnd_m   = 0;
   AudioEnd_f   = 0;
   
}

/*
 *   waiting ?  
 */

int wait_drv_ready (void)
{
int found, read;

   do
   {  
       found = inb ( GSCDPORT(0) ); 
       found &= 0x0f;
       read  = inb ( GSCDPORT(0) );
       read  &= 0x0f;
   } while ( read != found );
   
#ifdef GSCD_DEBUG
printk ( "Wait for: %d\n", read );
#endif   
   
   return read;
}

void cc_Ident (char * respons)
{
char to_do [] = {CMD_IDENT, 0, 0};

    cmd_out (TYPE_INFO, (char *)&to_do, (char *)respons, (int)0x1E );

}

void cc_SetSpeed (void)
{
char to_do [] = {CMD_SETSPEED, 0, 0};
char dummy;

    if ( speed > 0 )
    {
       to_do[1] = speed & 0x0F;
       cmd_out (TYPE_INFO, (char *)&to_do, (char *)&dummy, 0);
    }
}


void cc_Reset (void)
{
char to_do [] = {CMD_RESET, 0};
char dummy;

   cmd_out (TYPE_INFO, (char *)&to_do, (char *)&dummy, 0);
}



void cmd_status (void)
{
char to_do [] = {CMD_STATUS, 0};
char dummy;

   cmd_out (TYPE_INFO, (char *)&to_do, (char *)&dummy, 0);

#ifdef GSCD_DEBUG
printk ("GSCD: Status: %d\n", disk_state );
#endif

}

void cmd_out ( int cmd_type, char * cmd, char * respo_buf, int respo_count )
{
int        result;


       result = wait_drv_ready ();
       if ( result != drv_mode )
       {
       unsigned long test_loops = 0xFFFF;
       int           i,dummy;

          outb ( curr_drv_state, GSCDPORT(0));
          
          /* LOCLOOP_170 */
          do
          {
             result = wait_drv_ready ();
             test_loops--;
          } while ( (result != drv_mode) && (test_loops > 0) );

          if ( result != drv_mode )
          {
             disk_state = ST_x08 | ST_x04 | ST_INVALID;
             return;
          }          
        
          /* ...and waiting */
          for ( i=1,dummy=1 ; i<0xFFFF ; i++ )
          {
             dummy *= i;
          }              
       }

       /* LOC_172 */    
       /* check the unit */
       /* and wake it up */
       if ( cmd_unit_alive () != 0x08 )
       {
          /* LOC_174 */
          /* game over for this unit */
          disk_state = ST_x08 | ST_x04 | ST_INVALID;
          return;
       }
       
       /* LOC_176 */
       #ifdef GSCD_DEBUG
        printk ("LOC_176 ");
       #endif       
       if ( drv_mode == 0x09 )
       {
          /* magic... */
          printk ("GSCD: magic ...\n");       
          outb ( result, GSCDPORT(2));
       }

       /* write the command to the drive */
       cmd_write_cmd (cmd);

       /* LOC_178 */
       for (;;)
       {
          result = wait_drv_ready ();
          if ( result != drv_mode )
          {
             /* LOC_179 */
             if ( result == 0x04 )                  /* Mode 4 */
             {
                /* LOC_205 */
                #ifdef GSCD_DEBUG
                printk ("LOC_205 ");	              
                #endif
                disk_state = inb ( GSCDPORT (2));
             
                do
                {
                   result = wait_drv_ready ();
                } while ( result != drv_mode );
                return;

             }
             else
             {
                if ( result == 0x06 )               /* Mode 6 */
                {
                   /* LOC_181 */
                   #ifdef GSCD_DEBUG
                    printk ("LOC_181 ");	              
                   #endif

                   if (cmd_type == TYPE_DATA)
                   {
                      /* read data */
                      /* LOC_184 */
                      if ( drv_mode == 9 )
                      {
                         /* read the data to the buffer (word) */
 
                         /* (*(cmd+1))?(CD_FRAMESIZE/2):(CD_FRAMESIZE_RAW/2) */
                         cmd_read_w ( respo_buf, respo_count, CD_FRAMESIZE/2 );
                         return;
                      } 
                      else
                      { 
                         /* read the data to the buffer (byte) */
                         
                         /* (*(cmd+1))?(CD_FRAMESIZE):(CD_FRAMESIZE_RAW)    */
                         cmd_read_b ( respo_buf, respo_count, CD_FRAMESIZE );
                         return;
                      }
                   }
                   else
                   { 
                      /* read the info to the buffer */
                      cmd_info_in ( respo_buf, respo_count ); 
                      return;
                   }                  

                   return;
                }
             }

          }
          else
          {
             disk_state = ST_x08 | ST_x04 | ST_INVALID;         
             return;
          }    
       } /* for (;;) */


#ifdef GSCD_DEBUG
printk ("\n");       
#endif    
}


static void cmd_write_cmd ( char *pstr )
{
int  i,j;

    /* LOC_177 */
    #ifdef GSCD_DEBUG
     printk ("LOC_177 ");       
    #endif
       
    /* calculate the number of parameter */
    j = *pstr & 0x0F;

    /* shift it out */
    for ( i=0 ; i<j ; i++ )
    {
       outb ( *pstr, GSCDPORT(2) );
       pstr++;
    }
}


static int cmd_unit_alive ( void )
{
int            result;
unsigned long  max_test_loops;


    /* LOC_172 */
    #ifdef GSCD_DEBUG
     printk ("LOC_172 ");       
    #endif

    outb ( curr_drv_state, GSCDPORT(0));       
    max_test_loops = 0xFFFF;
     
    do
    {
       result = wait_drv_ready ();
       max_test_loops--;
    } while ( (result != 0x08) && (max_test_loops > 0) );

    return result;
}       


static void cmd_info_in ( char *pb, int count )
{
int   result;
char  read;


        /* read info */
        /* LOC_182 */
        #ifdef GSCD_DEBUG
         printk ("LOC_182 ");				              
        #endif
       
        do
        {
           read = inb (GSCDPORT(2)); 
           if ( count > 0 )
           {
              *pb = read;
              pb++;
              count--;
           }
                         
           /* LOC_183 */
           do
           {
              result = wait_drv_ready ();
           } while ( result == 0x0E );
        } while ( result == 6 );                     

        cmd_end ();
        return;
}


static void cmd_read_b ( char *pb, int count, int size )
{
int  result;
int  i;
                     
                     
       /* LOC_188 */
       /* LOC_189 */
       #ifdef GSCD_DEBUG
        printk ("LOC_189 ");			              
       #endif

       do 
       {
          do
          {
             result = wait_drv_ready ();
          } while ( result != 6 || result == 0x0E );
                         
          if ( result != 6 )
          {
             cmd_end ();
             return;
          }                       
          
          #ifdef GSCD_DEBUG
           printk ("LOC_191 ");       
          #endif

          for ( i=0 ; i< size ; i++ )
          {
             *pb = inb (GSCDPORT(2));
             pb++;
          }
          count--;
       } while ( count > 0 );
       
       cmd_end ();
       return;
}


static void cmd_end (void)
{
int   result;


      /* LOC_204 */
      #ifdef GSCD_DEBUG
       printk ("LOC_204 ");				              
      #endif
      
      do
      {
         result = wait_drv_ready ();
         if ( result == drv_mode )
         {
            return;
         }
      } while ( result != 4 );

      /* LOC_205 */
      #ifdef GSCD_DEBUG
       printk ("LOC_205 ");			              
      #endif
       
      disk_state = inb ( GSCDPORT (2));
             
      do
      {
         result = wait_drv_ready ();
      } while ( result != drv_mode );
      return;

}


static void cmd_read_w ( char *pb, int count, int size )
{
int  result;
int  i;


    #ifdef GSCD_DEBUG
     printk ("LOC_185 ");			              
    #endif
 
    do 
    {
       /* LOC_185 */
       do
       {
          result = wait_drv_ready ();
       } while ( result != 6 || result == 0x0E );
                         
       if ( result != 6 )
       {
          cmd_end ();
          return;
       }                       
                         
       for ( i=0 ; i<size ; i++ )
       {
           /* na, hier muss ich noch mal drueber nachdenken */
           *pb = inw(GSCDPORT(2));
           pb++;
        }
        count--;
     } while ( count > 0 );
                    
     cmd_end ();
     return;
}

int __init find_drives (void)
{
int *pdrv;
int drvnum;
int subdrv;
int i;

    speed           = 0;
    pdrv            = (int *)&drv_states;
    curr_drv_state  = 0xFE;
    subdrv          = 0;
    drvnum          = 0;

    for ( i=0 ; i<8 ; i++ )
    { 
       subdrv++;
       cmd_status ();
       disk_state &= ST_x08 | ST_x04 | ST_INVALID | ST_x01;
       if ( disk_state != (ST_x08 | ST_x04 | ST_INVALID) )
       {
          /* LOC_240 */
          *pdrv = curr_drv_state;
          init_cd_drive (drvnum);
          pdrv++;
          drvnum++;
       }
       else
       {
          if ( subdrv < 2 )
          {
             continue;
          }
          else
          {
             subdrv = 0;
          }
       }   

/*       curr_drv_state<<1;         <-- das geht irgendwie nicht */ 
/* muss heissen:    curr_drv_state <<= 1; (ist ja Wert-Zuweisung) */
         curr_drv_state *= 2;
         curr_drv_state |= 1;
#ifdef GSCD_DEBUG
         printk ("DriveState: %d\n", curr_drv_state );
#endif
    } 

    ndrives = drvnum;
    return drvnum;
}    

void __init init_cd_drive ( int num )
{
char resp [50];
int  i;

   printk ("GSCD: init unit %d\n", num );
   cc_Ident ((char *)&resp);

   printk ("GSCD: identification: ");
   for ( i=0 ; i<0x1E; i++ )
   {
      printk ( "%c", resp[i] );
   }
   printk ("\n");
   
   cc_SetSpeed ();  

}    

#ifdef FUTURE_WORK
/* return_done */
static void update_state ( void )
{
unsigned int AX;


    if ( (disk_state & (ST_x08 | ST_x04 | ST_INVALID | ST_x01)) == 0 )
    {
       if ( disk_state == (ST_x08 | ST_x04 | ST_INVALID))
       {
          AX = ST_INVALID;
       }
  
       if ( (disk_state & (ST_x08 | ST_x04 | ST_INVALID | ST_x01)) == 0 )
       {
          invalidate ();
          f_drv_ok = 0;
       }
       
       AX |= 0x8000;
    }
    
    if ( disk_state & ST_PLAYING )
    {
       AX |= 0x200;
    }
    
    AX |= 0x100;
    /* pkt_esbx = AX; */

    disk_state = 0;
    
}
#endif

/* Init for the Module-Version */
int init_gscd(void)
{
long err;


     /* call the GoldStar-init */
     err = my_gscd_init ( );

     if ( err < 0 )
     {
         return err;
     }
     else
     {
        printk (KERN_INFO "Happy GoldStar !\n" );
        return 0;
     }    
}

void __exit exit_gscd(void)
{
   CLEAR_TIMER;

   devfs_unregister(devfs_find_handle(NULL, "gscd", 0, 0, DEVFS_SPECIAL_BLK,
				      0));
   if ((devfs_unregister_blkdev(MAJOR_NR, "gscd" ) == -EINVAL))
   {
      printk("What's that: can't unregister GoldStar-module\n" );
      return;
   }
   blk_cleanup_queue(BLK_DEFAULT_QUEUE(MAJOR_NR));
   release_region (gscd_port,4);
   printk(KERN_INFO "GoldStar-module released.\n" );
}

#ifdef MODULE
module_init(init_gscd);
#endif 
module_exit(exit_gscd);


/* Test for presence of drive and initialize it.  Called only at boot time. */
int __init gscd_init (void)
{
   return my_gscd_init ();
}


/* This is the common initialisation for the GoldStar drive. */
/* It is called at boot time AND for module init.           */
int __init my_gscd_init (void)
{
int i;
int result;

        printk (KERN_INFO "GSCD: version %s\n", GSCD_VERSION);
        printk (KERN_INFO "GSCD: Trying to detect a Goldstar R420 CD-ROM drive at 0x%X.\n", gscd_port);

        if (check_region(gscd_port, 4)) 
        {
	  printk("GSCD: Init failed, I/O port (%X) already in use.\n", gscd_port);
	  return -EIO;
	}
	  

	/* check for card */
	result = wait_drv_ready ();
        if ( result == 0x09 )
        {
           printk ("GSCD: DMA kann ich noch nicht!\n" );
           return -EIO;
        }
 
        if ( result == 0x0b )
        {
           drv_mode = result;
           i = find_drives ();
           if ( i == 0 )
           {
              printk ( "GSCD: GoldStar CD-ROM Drive is not found.\n" );
              return -EIO;
           }
        }

        if ( (result != 0x0b) && (result != 0x09) )
        {
              printk ("GSCD: GoldStar Interface Adapter does not exist or H/W error\n" );
              return -EIO;
        }          		            

        /* reset all drives */
        i = 0;
        while ( drv_states[i] != 0 )
        {
           curr_drv_state = drv_states[i];
           printk (KERN_INFO "GSCD: Reset unit %d ... ",i );
           cc_Reset ();
           printk ( "done\n" );
           i++;
        }

	if (devfs_register_blkdev(MAJOR_NR, "gscd", &gscd_fops) != 0)
	{
		printk("GSCD: Unable to get major %d for GoldStar CD-ROM\n",
		       MAJOR_NR);
		return -EIO;
	}
	devfs_register (NULL, "gscd", DEVFS_FL_DEFAULT, MAJOR_NR, 0,
			S_IFBLK | S_IRUGO | S_IWUGO, &gscd_fops, NULL);

	blk_init_queue(BLK_DEFAULT_QUEUE(MAJOR_NR), DEVICE_REQUEST);
	blksize_size[MAJOR_NR] = gscd_blocksizes;
	read_ahead[MAJOR_NR] = 4;
        
        disk_state = 0;
        gscdPresent = 1;

	request_region(gscd_port, 4, "gscd");
	register_disk(NULL, MKDEV(MAJOR_NR,0), 1, &gscd_fops, 0);

        printk (KERN_INFO "GSCD: GoldStar CD-ROM Drive found.\n" );
	return 0;
}

static void gscd_hsg2msf (long hsg, struct msf *msf)
{
	hsg += CD_MSF_OFFSET;
	msf -> min = hsg / (CD_FRAMES*CD_SECS);
	hsg %= CD_FRAMES*CD_SECS;
	msf -> sec = hsg / CD_FRAMES;
	msf -> frame = hsg % CD_FRAMES;

	gscd_bin2bcd(&msf -> min);		/* convert to BCD */
	gscd_bin2bcd(&msf -> sec);
	gscd_bin2bcd(&msf -> frame);
}
 

static void gscd_bin2bcd (unsigned char *p)
{
int u, t;

	u = *p % 10;
	t = *p / 10;
	*p = u | (t << 4);
}


#ifdef FUTURE_WORK
static long gscd_msf2hsg (struct msf *mp)
{
	return gscd_bcd2bin(mp -> frame)
		+ gscd_bcd2bin(mp -> sec) * CD_FRAMES
		+ gscd_bcd2bin(mp -> min) * CD_FRAMES * CD_SECS
		- CD_MSF_OFFSET;
}

static int gscd_bcd2bin (unsigned char bcd)
{
	return (bcd >> 4) * 10 + (bcd & 0xF);
}
#endif


