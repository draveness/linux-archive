/*
 *   ALSA driver for RME Hammerfall DSP audio interface(s)
 *
 *      Copyright (c) 2002  Paul Davis
 *                          Marcus Andersson
 *                          Thomas Charbonnel
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <sound/driver.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/moduleparam.h>

#include <sound/core.h>
#include <sound/control.h>
#include <sound/pcm.h>
#include <sound/info.h>
#include <sound/asoundef.h>
#include <sound/rawmidi.h>
#include <sound/hwdep.h>
#include <sound/initval.h>
#include <sound/hdsp.h>

#include <asm/byteorder.h>
#include <asm/current.h>
#include <asm/io.h>

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;	/* Index 0-MAX */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;	/* ID for this card */
static int enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;	/* Enable this card */
static int precise_ptr[SNDRV_CARDS] = { [0 ... (SNDRV_CARDS-1)] = 0 }; /* Enable precise pointer */
static int line_outs_monitor[SNDRV_CARDS] = { [0 ... (SNDRV_CARDS-1)] = 0}; /* Send all inputs/playback to line outs */
static int boot_devs;

module_param_array(index, int, boot_devs, 0444);
MODULE_PARM_DESC(index, "Index value for RME Hammerfall DSP interface.");
MODULE_PARM_SYNTAX(index, SNDRV_INDEX_DESC);
module_param_array(id, charp, boot_devs, 0444);
MODULE_PARM_DESC(id, "ID string for RME Hammerfall DSP interface.");
MODULE_PARM_SYNTAX(id, SNDRV_ID_DESC);
module_param_array(enable, bool, boot_devs, 0444);
MODULE_PARM_DESC(enable, "Enable/disable specific Hammerfall DSP soundcards.");
MODULE_PARM_SYNTAX(enable, SNDRV_ENABLE_DESC);
module_param_array(precise_ptr, bool, boot_devs, 0444);
MODULE_PARM_DESC(precise_ptr, "Enable precise pointer (doesn't work reliably).");
MODULE_PARM_SYNTAX(precise_ptr, SNDRV_ENABLED "," SNDRV_BOOLEAN_FALSE_DESC);
module_param_array(line_outs_monitor, bool, boot_devs, 0444);
MODULE_PARM_DESC(line_outs_monitor, "Send all input and playback streams to line outs by default.");
MODULE_PARM_SYNTAX(line_outs_monitor, SNDRV_ENABLED "," SNDRV_BOOLEAN_FALSE_DESC);
MODULE_AUTHOR("Paul Davis <paul@linuxaudiosystems.com>, Marcus Andersson, Thomas Charbonnel <thomas@undata.org>");
MODULE_DESCRIPTION("RME Hammerfall DSP");
MODULE_LICENSE("GPL");
MODULE_CLASSES("{sound}");
MODULE_DEVICES("{{RME Hammerfall-DSP},"
	        "{RME HDSP-9652},"
		"{RME HDSP-9632}}");

#define HDSP_MAX_CHANNELS        26
#define HDSP_MAX_DS_CHANNELS     14
#define HDSP_MAX_QS_CHANNELS     8
#define DIGIFACE_SS_CHANNELS     26
#define DIGIFACE_DS_CHANNELS     14
#define MULTIFACE_SS_CHANNELS    18
#define MULTIFACE_DS_CHANNELS    14
#define H9652_SS_CHANNELS        26
#define H9652_DS_CHANNELS        14
/* This does not include possible Analog Extension Boards
   AEBs are detected at card initialization
*/
#define H9632_SS_CHANNELS	 12
#define H9632_DS_CHANNELS	 8
#define H9632_QS_CHANNELS	 4

/* Write registers. These are defined as byte-offsets from the iobase value.
 */
#define HDSP_resetPointer               0
#define HDSP_outputBufferAddress	32
#define HDSP_inputBufferAddress		36
#define HDSP_controlRegister		64
#define HDSP_interruptConfirmation	96
#define HDSP_outputEnable	  	128
#define HDSP_control2Reg		256
#define HDSP_midiDataOut0  		352
#define HDSP_midiDataOut1  		356
#define HDSP_fifoData  			368
#define HDSP_inputEnable	 	384

/* Read registers. These are defined as byte-offsets from the iobase value
 */

#define HDSP_statusRegister    0
#define HDSP_timecode        128
#define HDSP_status2Register 192
#define HDSP_midiDataOut0    352
#define HDSP_midiDataOut1    356
#define HDSP_midiDataIn0     360
#define HDSP_midiDataIn1     364
#define HDSP_midiStatusOut0  384
#define HDSP_midiStatusOut1  388
#define HDSP_midiStatusIn0   392
#define HDSP_midiStatusIn1   396
#define HDSP_fifoStatus      400

/* the meters are regular i/o-mapped registers, but offset
   considerably from the rest. the peak registers are reset
   when read; the least-significant 4 bits are full-scale counters; 
   the actual peak value is in the most-significant 24 bits.
*/

#define HDSP_playbackPeakLevel  4096  /* 26 * 32 bit values */
#define HDSP_inputPeakLevel     4224  /* 26 * 32 bit values */
#define HDSP_outputPeakLevel    4352  /* (26+2) * 32 bit values */
#define HDSP_playbackRmsLevel   4612  /* 26 * 64 bit values */
#define HDSP_inputRmsLevel      4868  /* 26 * 64 bit values */


/* This is for H9652 cards
   Peak values are read downward from the base
   Rms values are read upward
   There are rms values for the outputs too
   26*3 values are read in ss mode
   14*3 in ds mode, with no gap between values
*/
#define HDSP_9652_peakBase	7164	
#define HDSP_9652_rmsBase	4096

/* c.f. the hdsp_9632_meters_t struct */
#define HDSP_9632_metersBase	4096

#define HDSP_IO_EXTENT     7168

/* control2 register bits */

#define HDSP_TMS                0x01
#define HDSP_TCK                0x02
#define HDSP_TDI                0x04
#define HDSP_JTAG               0x08
#define HDSP_PWDN               0x10
#define HDSP_PROGRAM	        0x020
#define HDSP_CONFIG_MODE_0	0x040
#define HDSP_CONFIG_MODE_1	0x080
#define HDSP_VERSION_BIT	0x100
#define HDSP_BIGENDIAN_MODE     0x200
#define HDSP_RD_MULTIPLE        0x400
#define HDSP_9652_ENABLE_MIXER  0x800
#define HDSP_TDO                0x10000000

#define HDSP_S_PROGRAM     	(HDSP_PROGRAM|HDSP_CONFIG_MODE_0)
#define HDSP_S_LOAD		(HDSP_PROGRAM|HDSP_CONFIG_MODE_1)

/* Control Register bits */

#define HDSP_Start                (1<<0)  /* start engine */
#define HDSP_Latency0             (1<<1)  /* buffer size = 2^n where n is defined by Latency{2,1,0} */
#define HDSP_Latency1             (1<<2)  /* [ see above ] */
#define HDSP_Latency2             (1<<3)  /* [ see above ] */
#define HDSP_ClockModeMaster      (1<<4)  /* 1=Master, 0=Slave/Autosync */
#define HDSP_AudioInterruptEnable (1<<5)  /* what do you think ? */
#define HDSP_Frequency0           (1<<6)  /* 0=44.1kHz/88.2kHz/176.4kHz 1=48kHz/96kHz/192kHz */
#define HDSP_Frequency1           (1<<7)  /* 0=32kHz/64kHz/128kHz */
#define HDSP_DoubleSpeed          (1<<8)  /* 0=normal speed, 1=double speed */
#define HDSP_SPDIFProfessional    (1<<9)  /* 0=consumer, 1=professional */
#define HDSP_SPDIFEmphasis        (1<<10) /* 0=none, 1=on */
#define HDSP_SPDIFNonAudio        (1<<11) /* 0=off, 1=on */
#define HDSP_SPDIFOpticalOut      (1<<12) /* 1=use 1st ADAT connector for SPDIF, 0=do not */
#define HDSP_SyncRef2             (1<<13) 
#define HDSP_SPDIFInputSelect0    (1<<14) 
#define HDSP_SPDIFInputSelect1    (1<<15) 
#define HDSP_SyncRef0             (1<<16) 
#define HDSP_SyncRef1             (1<<17)
#define HDSP_AnalogExtensionBoard (1<<18) /* For H9632 cards */ 
#define HDSP_XLRBreakoutCable     (1<<20) /* For H9632 cards */
#define HDSP_Midi0InterruptEnable (1<<22)
#define HDSP_Midi1InterruptEnable (1<<23)
#define HDSP_LineOut              (1<<24)
#define HDSP_ADGain0		  (1<<25) /* From here : H9632 specific */
#define HDSP_ADGain1		  (1<<26)
#define HDSP_DAGain0		  (1<<27)
#define HDSP_DAGain1		  (1<<28)
#define HDSP_PhoneGain0		  (1<<29)
#define HDSP_PhoneGain1		  (1<<30)
#define HDSP_QuadSpeed	  	  (1<<31)

#define HDSP_ADGainMask       (HDSP_ADGain0|HDSP_ADGain1)
#define HDSP_ADGainMinus10dBV  HDSP_ADGainMask
#define HDSP_ADGainPlus4dBu   (HDSP_ADGain0)
#define HDSP_ADGainLowGain     0

#define HDSP_DAGainMask         (HDSP_DAGain0|HDSP_DAGain1)
#define HDSP_DAGainHighGain      HDSP_DAGainMask
#define HDSP_DAGainPlus4dBu     (HDSP_DAGain0)
#define HDSP_DAGainMinus10dBV    0

#define HDSP_PhoneGainMask      (HDSP_PhoneGain0|HDSP_PhoneGain1)
#define HDSP_PhoneGain0dB        HDSP_PhoneGainMask
#define HDSP_PhoneGainMinus6dB  (HDSP_PhoneGain0)
#define HDSP_PhoneGainMinus12dB  0

#define HDSP_LatencyMask    (HDSP_Latency0|HDSP_Latency1|HDSP_Latency2)
#define HDSP_FrequencyMask  (HDSP_Frequency0|HDSP_Frequency1|HDSP_DoubleSpeed|HDSP_QuadSpeed)

#define HDSP_SPDIFInputMask    (HDSP_SPDIFInputSelect0|HDSP_SPDIFInputSelect1)
#define HDSP_SPDIFInputADAT1    0
#define HDSP_SPDIFInputCoaxial (HDSP_SPDIFInputSelect0)
#define HDSP_SPDIFInputCdrom   (HDSP_SPDIFInputSelect1)
#define HDSP_SPDIFInputAES     (HDSP_SPDIFInputSelect0|HDSP_SPDIFInputSelect1)

#define HDSP_SyncRefMask        (HDSP_SyncRef0|HDSP_SyncRef1|HDSP_SyncRef2)
#define HDSP_SyncRef_ADAT1       0
#define HDSP_SyncRef_ADAT2      (HDSP_SyncRef0)
#define HDSP_SyncRef_ADAT3      (HDSP_SyncRef1)
#define HDSP_SyncRef_SPDIF      (HDSP_SyncRef0|HDSP_SyncRef1)
#define HDSP_SyncRef_WORD       (HDSP_SyncRef2)
#define HDSP_SyncRef_ADAT_SYNC  (HDSP_SyncRef0|HDSP_SyncRef2)

/* Sample Clock Sources */

#define HDSP_CLOCK_SOURCE_AUTOSYNC           0
#define HDSP_CLOCK_SOURCE_INTERNAL_32KHZ     1
#define HDSP_CLOCK_SOURCE_INTERNAL_44_1KHZ   2
#define HDSP_CLOCK_SOURCE_INTERNAL_48KHZ     3
#define HDSP_CLOCK_SOURCE_INTERNAL_64KHZ     4
#define HDSP_CLOCK_SOURCE_INTERNAL_88_2KHZ   5
#define HDSP_CLOCK_SOURCE_INTERNAL_96KHZ     6
#define HDSP_CLOCK_SOURCE_INTERNAL_128KHZ    7
#define HDSP_CLOCK_SOURCE_INTERNAL_176_4KHZ  8
#define HDSP_CLOCK_SOURCE_INTERNAL_192KHZ    9

/* Preferred sync reference choices - used by "pref_sync_ref" control switch */

#define HDSP_SYNC_FROM_WORD      0
#define HDSP_SYNC_FROM_SPDIF     1
#define HDSP_SYNC_FROM_ADAT1     2
#define HDSP_SYNC_FROM_ADAT_SYNC 3
#define HDSP_SYNC_FROM_ADAT2     4
#define HDSP_SYNC_FROM_ADAT3     5

/* SyncCheck status */

#define HDSP_SYNC_CHECK_NO_LOCK 0
#define HDSP_SYNC_CHECK_LOCK    1
#define HDSP_SYNC_CHECK_SYNC	2

/* AutoSync references - used by "autosync_ref" control switch */

#define HDSP_AUTOSYNC_FROM_WORD      0
#define HDSP_AUTOSYNC_FROM_ADAT_SYNC 1
#define HDSP_AUTOSYNC_FROM_SPDIF     2
#define HDSP_AUTOSYNC_FROM_NONE	     3
#define HDSP_AUTOSYNC_FROM_ADAT1     4
#define HDSP_AUTOSYNC_FROM_ADAT2     5
#define HDSP_AUTOSYNC_FROM_ADAT3     6

/* Possible sources of S/PDIF input */

#define HDSP_SPDIFIN_OPTICAL  0	/* optical  (ADAT1) */
#define HDSP_SPDIFIN_COAXIAL  1	/* coaxial (RCA) */
#define HDSP_SPDIFIN_INTERNAL 2	/* internal (CDROM) */
#define HDSP_SPDIFIN_AES      3 /* xlr for H9632 (AES)*/

#define HDSP_Frequency32KHz    HDSP_Frequency0
#define HDSP_Frequency44_1KHz  HDSP_Frequency1
#define HDSP_Frequency48KHz    (HDSP_Frequency1|HDSP_Frequency0)
#define HDSP_Frequency64KHz    (HDSP_DoubleSpeed|HDSP_Frequency0)
#define HDSP_Frequency88_2KHz  (HDSP_DoubleSpeed|HDSP_Frequency1)
#define HDSP_Frequency96KHz    (HDSP_DoubleSpeed|HDSP_Frequency1|HDSP_Frequency0)
/* For H9632 cards */
#define HDSP_Frequency128KHz   (HDSP_QuadSpeed|HDSP_DoubleSpeed|HDSP_Frequency0)
#define HDSP_Frequency176_4KHz (HDSP_QuadSpeed|HDSP_DoubleSpeed|HDSP_Frequency1)
#define HDSP_Frequency192KHz   (HDSP_QuadSpeed|HDSP_DoubleSpeed|HDSP_Frequency1|HDSP_Frequency0)

#define hdsp_encode_latency(x)       (((x)<<1) & HDSP_LatencyMask)
#define hdsp_decode_latency(x)       (((x) & HDSP_LatencyMask)>>1)

#define hdsp_encode_spdif_in(x) (((x)&0x3)<<14)
#define hdsp_decode_spdif_in(x) (((x)>>14)&0x3)

/* Status Register bits */

#define HDSP_audioIRQPending    (1<<0)
#define HDSP_Lock2              (1<<1)     /* this is for Digiface and H9652 */
#define HDSP_spdifFrequency3	HDSP_Lock2 /* this is for H9632 only */
#define HDSP_Lock1              (1<<2)
#define HDSP_Lock0              (1<<3)
#define HDSP_SPDIFSync          (1<<4)
#define HDSP_TimecodeLock       (1<<5)
#define HDSP_BufferPositionMask 0x000FFC0 /* Bit 6..15 : h/w buffer pointer */
#define HDSP_Sync2              (1<<16)
#define HDSP_Sync1              (1<<17)
#define HDSP_Sync0              (1<<18)
#define HDSP_DoubleSpeedStatus  (1<<19)
#define HDSP_ConfigError        (1<<20)
#define HDSP_DllError           (1<<21)
#define HDSP_spdifFrequency0    (1<<22)
#define HDSP_spdifFrequency1    (1<<23)
#define HDSP_spdifFrequency2    (1<<24)
#define HDSP_SPDIFErrorFlag     (1<<25)
#define HDSP_BufferID           (1<<26)
#define HDSP_TimecodeSync       (1<<27)
#define HDSP_AEBO          	(1<<28) /* H9632 specific Analog Extension Boards */
#define HDSP_AEBI		(1<<29) /* 0 = present, 1 = absent */
#define HDSP_midi0IRQPending    (1<<30) 
#define HDSP_midi1IRQPending    (1<<31)

#define HDSP_spdifFrequencyMask    (HDSP_spdifFrequency0|HDSP_spdifFrequency1|HDSP_spdifFrequency2)

#define HDSP_spdifFrequency32KHz   (HDSP_spdifFrequency0)
#define HDSP_spdifFrequency44_1KHz (HDSP_spdifFrequency1)
#define HDSP_spdifFrequency48KHz   (HDSP_spdifFrequency0|HDSP_spdifFrequency1)

#define HDSP_spdifFrequency64KHz   (HDSP_spdifFrequency2)
#define HDSP_spdifFrequency88_2KHz (HDSP_spdifFrequency0|HDSP_spdifFrequency2)
#define HDSP_spdifFrequency96KHz   (HDSP_spdifFrequency2|HDSP_spdifFrequency1)

/* This is for H9632 cards */
#define HDSP_spdifFrequency128KHz   HDSP_spdifFrequencyMask
#define HDSP_spdifFrequency176_4KHz HDSP_spdifFrequency3
#define HDSP_spdifFrequency192KHz   (HDSP_spdifFrequency3|HDSP_spdifFrequency0)

/* Status2 Register bits */

#define HDSP_version0     (1<<0)
#define HDSP_version1     (1<<1)
#define HDSP_version2     (1<<2)
#define HDSP_wc_lock      (1<<3)
#define HDSP_wc_sync      (1<<4)
#define HDSP_inp_freq0    (1<<5)
#define HDSP_inp_freq1    (1<<6)
#define HDSP_inp_freq2    (1<<7)
#define HDSP_SelSyncRef0  (1<<8)
#define HDSP_SelSyncRef1  (1<<9)
#define HDSP_SelSyncRef2  (1<<10)

#define HDSP_wc_valid (HDSP_wc_lock|HDSP_wc_sync)

#define HDSP_systemFrequencyMask (HDSP_inp_freq0|HDSP_inp_freq1|HDSP_inp_freq2)
#define HDSP_systemFrequency32   (HDSP_inp_freq0)
#define HDSP_systemFrequency44_1 (HDSP_inp_freq1)
#define HDSP_systemFrequency48   (HDSP_inp_freq0|HDSP_inp_freq1)
#define HDSP_systemFrequency64   (HDSP_inp_freq2)
#define HDSP_systemFrequency88_2 (HDSP_inp_freq0|HDSP_inp_freq2)
#define HDSP_systemFrequency96   (HDSP_inp_freq1|HDSP_inp_freq2)
/* FIXME : more values for 9632 cards ? */

#define HDSP_SelSyncRefMask        (HDSP_SelSyncRef0|HDSP_SelSyncRef1|HDSP_SelSyncRef2)
#define HDSP_SelSyncRef_ADAT1      0
#define HDSP_SelSyncRef_ADAT2      (HDSP_SelSyncRef0)
#define HDSP_SelSyncRef_ADAT3      (HDSP_SelSyncRef1)
#define HDSP_SelSyncRef_SPDIF      (HDSP_SelSyncRef0|HDSP_SelSyncRef1)
#define HDSP_SelSyncRef_WORD       (HDSP_SelSyncRef2)
#define HDSP_SelSyncRef_ADAT_SYNC  (HDSP_SelSyncRef0|HDSP_SelSyncRef2)

/* Card state flags */

#define HDSP_InitializationComplete  (1<<0)
#define HDSP_FirmwareLoaded	     (1<<1)
#define HDSP_FirmwareCached	     (1<<2)

/* FIFO wait times, defined in terms of 1/10ths of msecs */

#define HDSP_LONG_WAIT	 5000
#define HDSP_SHORT_WAIT  30

#define UNITY_GAIN                       32768
#define MINUS_INFINITY_GAIN              0

#ifndef PCI_VENDOR_ID_XILINX
#define PCI_VENDOR_ID_XILINX		0x10ee
#endif
#ifndef PCI_DEVICE_ID_XILINX_HAMMERFALL_DSP
#define PCI_DEVICE_ID_XILINX_HAMMERFALL_DSP 0x3fc5
#endif

/* the size of a substream (1 mono data stream) */

#define HDSP_CHANNEL_BUFFER_SAMPLES  (16*1024)
#define HDSP_CHANNEL_BUFFER_BYTES    (4*HDSP_CHANNEL_BUFFER_SAMPLES)

/* the size of the area we need to allocate for DMA transfers. the
   size is the same regardless of the number of channels - the 
   Multiface still uses the same memory area.

   Note that we allocate 1 more channel than is apparently needed
   because the h/w seems to write 1 byte beyond the end of the last
   page. Sigh.
*/

#define HDSP_DMA_AREA_BYTES ((HDSP_MAX_CHANNELS+1) * HDSP_CHANNEL_BUFFER_BYTES)
#define HDSP_DMA_AREA_KILOBYTES (HDSP_DMA_AREA_BYTES/1024)

typedef struct _hdsp             hdsp_t;
typedef struct _hdsp_midi        hdsp_midi_t;
typedef struct _hdsp_9632_meters hdsp_9632_meters_t;

struct _hdsp_9632_meters {
    u32 input_peak[16];
    u32 playback_peak[16];
    u32 output_peak[16];
    u32 xxx_peak[16];
    u32 padding[64];
    u32 input_rms_low[16];
    u32 playback_rms_low[16];
    u32 output_rms_low[16];
    u32 xxx_rms_low[16];
    u32 input_rms_high[16];
    u32 playback_rms_high[16];
    u32 output_rms_high[16];
    u32 xxx_rms_high[16];
};

struct _hdsp_midi {
    hdsp_t                  *hdsp;
    int                      id;
    snd_rawmidi_t           *rmidi;
    snd_rawmidi_substream_t *input;
    snd_rawmidi_substream_t *output;
    char                     istimer; /* timer in use */
    struct timer_list	     timer;
    spinlock_t               lock;
    int			     pending;
};

struct _hdsp {
	spinlock_t            lock;
	snd_pcm_substream_t  *capture_substream;
	snd_pcm_substream_t  *playback_substream;
        hdsp_midi_t           midi[2];
	struct tasklet_struct midi_tasklet;
	int                   precise_ptr;
	u32                   control_register;	     /* cached value */
	u32                   control2_register;     /* cached value */
	u32                   creg_spdif;
	u32                   creg_spdif_stream;
	char                 *card_name;	     /* digiface/multiface */
	HDSP_IO_Type          io_type;               /* ditto, but for code use */
        unsigned short        firmware_rev;
	unsigned short	      state;		     /* stores state bits */
	u32		      firmware_cache[24413]; /* this helps recover from accidental iobox power failure */
	size_t                period_bytes; 	     /* guess what this is */
	unsigned char	      max_channels;
	unsigned char	      qs_in_channels;	     /* quad speed mode for H9632 */
	unsigned char         ds_in_channels;
	unsigned char         ss_in_channels;	    /* different for multiface/digiface */
	unsigned char	      qs_out_channels;	    
	unsigned char         ds_out_channels;
	unsigned char         ss_out_channels;
	void                 *capture_buffer_unaligned;	 /* original buffer addresses */
	void                 *playback_buffer_unaligned; /* original buffer addresses */
	unsigned char        *capture_buffer;	    /* suitably aligned address */
	unsigned char        *playback_buffer;	    /* suitably aligned address */
	dma_addr_t            capture_buffer_addr;
	dma_addr_t            playback_buffer_addr;
	pid_t                 capture_pid;
	pid_t                 playback_pid;
	int                   running;
        int                   passthru;              /* non-zero if doing pass-thru */
	int                   system_sample_rate;
	char                 *channel_map;
	int                   dev;
	int                   irq;
	unsigned long         port;
	struct resource      *res_port;
        unsigned long         iobase;
	snd_card_t           *card;
	snd_pcm_t            *pcm;
	snd_hwdep_t          *hwdep;
	struct pci_dev       *pci;
	snd_kcontrol_t       *spdif_ctl;
        unsigned short        mixer_matrix[HDSP_MATRIX_MIXER_SIZE];
};

/* These tables map the ALSA channels 1..N to the channels that we
   need to use in order to find the relevant channel buffer. RME
   refer to this kind of mapping as between "the ADAT channel and
   the DMA channel." We index it using the logical audio channel,
   and the value is the DMA channel (i.e. channel buffer number)
   where the data for that channel can be read/written from/to.
*/

static char channel_map_df_ss[HDSP_MAX_CHANNELS] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
	18, 19, 20, 21, 22, 23, 24, 25
};

static char channel_map_mf_ss[HDSP_MAX_CHANNELS] = { /* Multiface */
	/* Analog */
	0, 1, 2, 3, 4, 5, 6, 7, 
	/* ADAT 2 */
	16, 17, 18, 19, 20, 21, 22, 23, 
	/* SPDIF */
	24, 25,
	-1, -1, -1, -1, -1, -1, -1, -1
};

static char channel_map_ds[HDSP_MAX_CHANNELS] = {
	/* ADAT channels are remapped */
	1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23,
	/* channels 12 and 13 are S/PDIF */
	24, 25,
	/* others don't exist */
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

static char channel_map_H9632_ss[HDSP_MAX_CHANNELS] = {
	/* ADAT channels */
	0, 1, 2, 3, 4, 5, 6, 7,
	/* SPDIF */
	8, 9,
	/* Analog */
	10, 11, 
	/* AO4S-192 and AI4S-192 extension boards */
	12, 13, 14, 15,
	/* others don't exist */
	-1, -1, -1, -1, -1, -1, -1, -1, 
	-1, -1
};

static char channel_map_H9632_ds[HDSP_MAX_CHANNELS] = {
	/* ADAT */
	1, 3, 5, 7,
	/* SPDIF */
	8, 9,
	/* Analog */
	10, 11, 
	/* AO4S-192 and AI4S-192 extension boards */
	12, 13, 14, 15,
	/* others don't exist */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1
};

static char channel_map_H9632_qs[HDSP_MAX_CHANNELS] = {
	/* ADAT is disabled in this mode */
	/* SPDIF */
	8, 9,
	/* Analog */
	10, 11,
	/* AO4S-192 and AI4S-192 extension boards */
	12, 13, 14, 15,
	/* others don't exist */
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1
};

#define HDSP_PREALLOCATE_MEMORY	/* via module snd-hdsp_mem */

#ifdef HDSP_PREALLOCATE_MEMORY
static void *snd_hammerfall_get_buffer(struct pci_dev *pci, size_t size, dma_addr_t *addrp, int capture)
{
	struct snd_dma_device pdev;
	struct snd_dma_buffer dmbuf;

	memset(&pdev, 0, sizeof(pdev));
	pdev.type = SNDRV_DMA_TYPE_DEV;
	pdev.dev = snd_dma_pci_data(pci);
	pdev.id = capture;
	dmbuf.bytes = 0;
	if (! snd_dma_get_reserved(&pdev, &dmbuf)) {
		if (snd_dma_alloc_pages(&pdev, size, &dmbuf) < 0)
			return NULL;
		snd_dma_set_reserved(&pdev, &dmbuf);
	}
	*addrp = dmbuf.addr;
	return dmbuf.area;
}

static void snd_hammerfall_free_buffer(struct pci_dev *pci, size_t size, void *ptr, dma_addr_t addr, int capture)
{
	struct snd_dma_device pdev;

	memset(&pdev, 0, sizeof(pdev));
	pdev.type = SNDRV_DMA_TYPE_DEV;
	pdev.dev = snd_dma_pci_data(pci);
	pdev.id = capture;
	snd_dma_free_reserved(&pdev);
}

#else
static void *snd_hammerfall_get_buffer(struct pci_dev *pci, size_t size, dma_addr_t *addrp, int capture)
{
	return snd_malloc_pci_pages(pci, size, addrp);
}

static void snd_hammerfall_free_buffer(struct pci_dev *pci, size_t size, void *ptr, dma_addr_t addr, int capture)
{
	snd_free_pci_pages(pci, size, ptr, addr);
}
#endif

static struct pci_device_id snd_hdsp_ids[] = {
	{
		.vendor = PCI_VENDOR_ID_XILINX,
		.device = PCI_DEVICE_ID_XILINX_HAMMERFALL_DSP, 
		.subvendor = PCI_ANY_ID,
		.subdevice = PCI_ANY_ID,
	}, /* RME Hammerfall-DSP */
	{ 0, },
};

MODULE_DEVICE_TABLE(pci, snd_hdsp_ids);

/* prototypes */
static int __devinit snd_hdsp_create_alsa_devices(snd_card_t *card, hdsp_t *hdsp);
static int __devinit snd_hdsp_create_pcm(snd_card_t *card, hdsp_t *hdsp);
static inline int snd_hdsp_enable_io (hdsp_t *hdsp);
static inline void snd_hdsp_initialize_midi_flush (hdsp_t *hdsp);
static inline void snd_hdsp_initialize_channels (hdsp_t *hdsp);
static inline int hdsp_fifo_wait(hdsp_t *hdsp, int count, int timeout);
static int hdsp_autosync_ref(hdsp_t *hdsp);
static int snd_hdsp_set_defaults(hdsp_t *hdsp);
static inline void snd_hdsp_9652_enable_mixer (hdsp_t *hdsp);

static inline int hdsp_playback_to_output_key (hdsp_t *hdsp, int in, int out)
{
	switch (hdsp->firmware_rev) {
	case 0xa:
		return (64 * out) + (32 + (in));
	case 0x96:
	case 0x97:
		return (32 * out) + (16 + (in));
	default:
		return (52 * out) + (26 + (in));
	}
}

static inline int hdsp_input_to_output_key (hdsp_t *hdsp, int in, int out)
{
	switch (hdsp->firmware_rev) {
	case 0xa:
		return (64 * out) + in;
	case 0x96:
	case 0x97:
		return (32 * out) + in;
	default:
		return (52 * out) + in;
	}
}

static inline void hdsp_write(hdsp_t *hdsp, int reg, int val)
{
	writel(val, hdsp->iobase + reg);
}

static inline unsigned int hdsp_read(hdsp_t *hdsp, int reg)
{
	return readl (hdsp->iobase + reg);
}

static inline int hdsp_check_for_iobox (hdsp_t *hdsp)
{

	if (hdsp->io_type == H9652 || hdsp->io_type == H9632) return 0;
	if (hdsp_read (hdsp, HDSP_statusRegister) & HDSP_ConfigError) {
		snd_printk ("Hammerfall-DSP: no Digiface or Multiface connected!\n");
		hdsp->state &= ~HDSP_FirmwareLoaded;
		return -EIO;
	}
	return 0;

}

static int snd_hdsp_load_firmware_from_cache(hdsp_t *hdsp) {

	int i;
	unsigned long flags;

	if ((hdsp_read (hdsp, HDSP_statusRegister) & HDSP_DllError) != 0) {
		
		snd_printk ("loading firmware\n");

		hdsp_write (hdsp, HDSP_control2Reg, HDSP_S_PROGRAM);
		hdsp_write (hdsp, HDSP_fifoData, 0);
		
		if (hdsp_fifo_wait (hdsp, 0, HDSP_LONG_WAIT)) {
			snd_printk ("timeout waiting for download preparation\n");
			return -EIO;
		}
		
		hdsp_write (hdsp, HDSP_control2Reg, HDSP_S_LOAD);
		
		for (i = 0; i < 24413; ++i) {
			hdsp_write(hdsp, HDSP_fifoData, hdsp->firmware_cache[i]);
			if (hdsp_fifo_wait (hdsp, 127, HDSP_LONG_WAIT)) {
				snd_printk ("timeout during firmware loading\n");
				return -EIO;
			}
		}

		if ((1000 / HZ) < 3000) {
			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_timeout((3000 * HZ + 999) / 1000);
		} else {
			mdelay(3000);
		}
		
		if (hdsp_fifo_wait (hdsp, 0, HDSP_LONG_WAIT)) {
			snd_printk ("timeout at end of firmware loading\n");
		    	return -EIO;
		}

#ifdef SNDRV_BIG_ENDIAN
		hdsp->control2_register = HDSP_BIGENDIAN_MODE;
#else
		hdsp->control2_register = 0;
#endif
		hdsp_write (hdsp, HDSP_control2Reg, hdsp->control2_register);
		snd_printk ("finished firmware loading\n");
		
	}
	if (hdsp->state & HDSP_InitializationComplete) {
		snd_printk("firmware loaded from cache, restoring defaults\n");
		spin_lock_irqsave(&hdsp->lock, flags);
		snd_hdsp_set_defaults(hdsp);
		spin_unlock_irqrestore(&hdsp->lock, flags); 
	}
	
	hdsp->state |= HDSP_FirmwareLoaded;

	return 0;
}

static inline int hdsp_get_iobox_version (hdsp_t *hdsp)
{
	int err;
	
	if (hdsp_check_for_iobox (hdsp)) {
		return -EIO;
	}

	if ((err = snd_hdsp_enable_io(hdsp)) < 0) {
		return err;
	}
		
	if ((hdsp_read (hdsp, HDSP_statusRegister) & HDSP_DllError) != 0) {
	
		hdsp_write (hdsp, HDSP_control2Reg, HDSP_PROGRAM);
		hdsp_write (hdsp, HDSP_fifoData, 0);
		if (hdsp_fifo_wait (hdsp, 0, HDSP_SHORT_WAIT) < 0) {
			return -EIO;
		}

		hdsp_write (hdsp, HDSP_control2Reg, HDSP_S_LOAD);
		hdsp_write (hdsp, HDSP_fifoData, 0);

		if (hdsp_fifo_wait (hdsp, 0, HDSP_SHORT_WAIT)) {
			hdsp->io_type = Multiface;
			hdsp_write (hdsp, HDSP_control2Reg, HDSP_VERSION_BIT);
			hdsp_write (hdsp, HDSP_control2Reg, HDSP_S_LOAD);
			hdsp_fifo_wait (hdsp, 0, HDSP_SHORT_WAIT);
		} else {
			hdsp->io_type = Digiface;
		} 
	} else {
		/* firmware was already loaded, get iobox type */
		if (hdsp_read(hdsp, HDSP_status2Register) & HDSP_version1) {
			hdsp->io_type = Multiface;
		} else {
			hdsp->io_type = Digiface;
		}
	}
	return 0;
}


static inline int hdsp_check_for_firmware (hdsp_t *hdsp)
{
	if (hdsp->io_type == H9652 || hdsp->io_type == H9632) return 0;
	if ((hdsp_read (hdsp, HDSP_statusRegister) & HDSP_DllError) != 0) {
		snd_printk("firmware not present.\n");
		hdsp->state &= ~HDSP_FirmwareLoaded;
		return -EIO;
	}
	return 0;
}


static inline int hdsp_fifo_wait(hdsp_t *hdsp, int count, int timeout)
{    
	int i;

	/* the fifoStatus registers reports on how many words
	   are available in the command FIFO.
	*/
	
	for (i = 0; i < timeout; i++) {

		if ((int)(hdsp_read (hdsp, HDSP_fifoStatus) & 0xff) <= count)
			return 0;

		/* not very friendly, but we only do this during a firmware
		   load and changing the mixer, so we just put up with it.
		*/

		udelay (100);
	}

	snd_printk ("wait for FIFO status <= %d failed after %d iterations\n",
		    count, timeout);
	return -1;
}

static inline int hdsp_read_gain (hdsp_t *hdsp, unsigned int addr)
{
	if (addr >= HDSP_MATRIX_MIXER_SIZE) {
		return 0;
	}
	return hdsp->mixer_matrix[addr];
}

static inline int hdsp_write_gain(hdsp_t *hdsp, unsigned int addr, unsigned short data)
{
	unsigned int ad;

	if (addr >= HDSP_MATRIX_MIXER_SIZE)
		return -1;
	
	if (hdsp->io_type == H9652 || hdsp->io_type == H9632) {

		/* from martin bj�rnsen:
		   
		   "You can only write dwords to the
		   mixer memory which contain two
		   mixer values in the low and high
		   word. So if you want to change
		   value 0 you have to read value 1
		   from the cache and write both to
		   the first dword in the mixer
		   memory."
		*/

		if (hdsp->io_type == H9632 && addr >= 512) {
			return 0;
		}

		if (hdsp->io_type == H9652 && addr >= 1352) {
			return 0;
		}

		hdsp->mixer_matrix[addr] = data;

		
		/* `addr' addresses a 16-bit wide address, but
		   the address space accessed via hdsp_write
		   uses byte offsets. put another way, addr
		   varies from 0 to 1351, but to access the
		   corresponding memory location, we need
		   to access 0 to 2703 ...
		*/
		ad = addr/2;
	
		hdsp_write (hdsp, 4096 + (ad*4), 
			    (hdsp->mixer_matrix[(addr&0x7fe)+1] << 16) + 
			    hdsp->mixer_matrix[addr&0x7fe]);
		
		return 0;

	} else {

		ad = (addr << 16) + data;
		
		if (hdsp_fifo_wait(hdsp, 127, HDSP_LONG_WAIT)) {
			return -1;
		}

		hdsp_write (hdsp, HDSP_fifoData, ad);
		hdsp->mixer_matrix[addr] = data;

	}

	return 0;
}

static inline int snd_hdsp_use_is_exclusive(hdsp_t *hdsp)
{
	unsigned long flags;
	int ret = 1;

	spin_lock_irqsave(&hdsp->lock, flags);
	if ((hdsp->playback_pid != hdsp->capture_pid) &&
	    (hdsp->playback_pid >= 0) && (hdsp->capture_pid >= 0)) {
		ret = 0;
	}
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return ret;
}

static inline int hdsp_external_sample_rate (hdsp_t *hdsp)
{
	unsigned int status2 = hdsp_read(hdsp, HDSP_status2Register);
	unsigned int rate_bits = status2 & HDSP_systemFrequencyMask;

	switch (rate_bits) {
	case HDSP_systemFrequency32:   return 32000;
	case HDSP_systemFrequency44_1: return 44100;
	case HDSP_systemFrequency48:   return 48000;
	case HDSP_systemFrequency64:   return 64000;
	case HDSP_systemFrequency88_2: return 88200;
	case HDSP_systemFrequency96:   return 96000;
	default:
		return 0;
	}
}

static inline int hdsp_spdif_sample_rate(hdsp_t *hdsp)
{
	unsigned int status = hdsp_read(hdsp, HDSP_statusRegister);
	unsigned int rate_bits = (status & HDSP_spdifFrequencyMask);

	if (status & HDSP_SPDIFErrorFlag) {
		return 0;
	}
	
	switch (rate_bits) {
	case HDSP_spdifFrequency32KHz: return 32000;
	case HDSP_spdifFrequency44_1KHz: return 44100;
	case HDSP_spdifFrequency48KHz: return 48000;
	case HDSP_spdifFrequency64KHz: return 64000;
	case HDSP_spdifFrequency88_2KHz: return 88200;
	case HDSP_spdifFrequency96KHz: return 96000;
	case HDSP_spdifFrequency128KHz: 
		if (hdsp->io_type == H9632) return 128000;
		break;
	case HDSP_spdifFrequency176_4KHz: 
		if (hdsp->io_type == H9632) return 176400;
		break;
	case HDSP_spdifFrequency192KHz: 
		if (hdsp->io_type == H9632) return 192000;
		break;
	default:
		break;
	}
	snd_printk ("unknown spdif frequency status; bits = 0x%x, status = 0x%x\n", rate_bits, status);
	return 0;
}

static inline void hdsp_compute_period_size(hdsp_t *hdsp)
{
	hdsp->period_bytes = 1 << ((hdsp_decode_latency(hdsp->control_register) + 8));
}

static snd_pcm_uframes_t hdsp_hw_pointer(hdsp_t *hdsp)
{
	int position;

	position = hdsp_read(hdsp, HDSP_statusRegister);

	if (!hdsp->precise_ptr) {
		return (position & HDSP_BufferID) ? (hdsp->period_bytes / 4) : 0;
	}

	position &= HDSP_BufferPositionMask;
	position /= 4;
	position -= 32;
	position &= (HDSP_CHANNEL_BUFFER_SAMPLES-1);
	return position;
}

static inline void hdsp_reset_hw_pointer(hdsp_t *hdsp)
{
	hdsp_write (hdsp, HDSP_resetPointer, 0);
}

static inline void hdsp_start_audio(hdsp_t *s)
{
	s->control_register |= (HDSP_AudioInterruptEnable | HDSP_Start);
	hdsp_write(s, HDSP_controlRegister, s->control_register);
}

static inline void hdsp_stop_audio(hdsp_t *s)
{
	s->control_register &= ~(HDSP_Start | HDSP_AudioInterruptEnable);
	hdsp_write(s, HDSP_controlRegister, s->control_register);
}

static inline void hdsp_silence_playback(hdsp_t *hdsp)
{
	memset(hdsp->playback_buffer, 0, HDSP_DMA_AREA_BYTES);
}

static int hdsp_set_interrupt_interval(hdsp_t *s, unsigned int frames)
{
	int n;

	spin_lock_irq(&s->lock);

	frames >>= 7;
	n = 0;
	while (frames) {
		n++;
		frames >>= 1;
	}

	s->control_register &= ~HDSP_LatencyMask;
	s->control_register |= hdsp_encode_latency(n);

	hdsp_write(s, HDSP_controlRegister, s->control_register);

	hdsp_compute_period_size(s);

	spin_unlock_irq(&s->lock);

	return 0;
}

static int hdsp_set_rate(hdsp_t *hdsp, int rate, int called_internally)
{
	int reject_if_open = 0;
	int current_rate;
	int rate_bits;

	/* ASSUMPTION: hdsp->lock is either held, or
	   there is no need for it (e.g. during module
	   initialization).
	*/
	
	if (!(hdsp->control_register & HDSP_ClockModeMaster)) {	
		if (called_internally) {
			/* request from ctl or card initialization */
			snd_printk("device is not running as a clock master: cannot set sample rate.\n");
			return -1;
		} else {		
			/* hw_param request while in AutoSync mode */
			int external_freq = hdsp_external_sample_rate(hdsp);
			int spdif_freq = hdsp_spdif_sample_rate(hdsp);
		
			if ((spdif_freq == external_freq*2) && (hdsp_autosync_ref(hdsp) >= HDSP_AUTOSYNC_FROM_ADAT1)) {
				snd_printk("Detected ADAT in double speed mode\n");
			} else if (hdsp->io_type == H9632 && (spdif_freq == external_freq*4) && (hdsp_autosync_ref(hdsp) >= HDSP_AUTOSYNC_FROM_ADAT1)) {
				snd_printk("Detected ADAT in quad speed mode\n");			
			} else if (rate != external_freq) {
				snd_printk("No AutoSync source for requested rate\n");
				return -1;
			}		
		}	
	}

	current_rate = hdsp->system_sample_rate;

	/* Changing from a "single speed" to a "double speed" rate is
	   not allowed if any substreams are open. This is because
	   such a change causes a shift in the location of 
	   the DMA buffers and a reduction in the number of available
	   buffers. 

	   Note that a similar but essentially insoluble problem
	   exists for externally-driven rate changes. All we can do
	   is to flag rate changes in the read/write routines.  */

	if (rate > 96000 && hdsp->io_type != H9632) {
		return -EINVAL;
	}
	
	switch (rate) {
	case 32000:
		if (current_rate > 48000) {
			reject_if_open = 1;
		}
		rate_bits = HDSP_Frequency32KHz;
		break;
	case 44100:
		if (current_rate > 48000) {
			reject_if_open = 1;
		}
		rate_bits = HDSP_Frequency44_1KHz;
		break;
	case 48000:
		if (current_rate > 48000) {
			reject_if_open = 1;
		}
		rate_bits = HDSP_Frequency48KHz;
		break;
	case 64000:
		if (current_rate <= 48000 || current_rate > 96000) {
			reject_if_open = 1;
		}
		rate_bits = HDSP_Frequency64KHz;
		break;
	case 88200:
		if (current_rate <= 48000 || current_rate > 96000) {
			reject_if_open = 1;
		}
		rate_bits = HDSP_Frequency88_2KHz;
		break;
	case 96000:
		if (current_rate <= 48000 || current_rate > 96000) {
			reject_if_open = 1;
		}
		rate_bits = HDSP_Frequency96KHz;
		break;
	case 128000:
		if (current_rate < 128000) {
			reject_if_open = 1;
		}
		rate_bits = HDSP_Frequency128KHz;
		break;
	case 176400:
		if (current_rate < 128000) {
			reject_if_open = 1;
		}
		rate_bits = HDSP_Frequency176_4KHz;
		break;
	case 192000:
		if (current_rate < 128000) {
			reject_if_open = 1;
		}
		rate_bits = HDSP_Frequency192KHz;
		break;
	default:
		return -EINVAL;
	}

	if (reject_if_open && (hdsp->capture_pid >= 0 || hdsp->playback_pid >= 0)) {
		snd_printk ("cannot change speed mode (capture PID = %d, playback PID = %d)\n",
			    hdsp->capture_pid,
			    hdsp->playback_pid);
		return -EBUSY;
	}

	hdsp->control_register &= ~HDSP_FrequencyMask;
	hdsp->control_register |= rate_bits;
	hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);

	if (rate >= 128000) {
		hdsp->channel_map = channel_map_H9632_qs;
	} else if (rate > 48000) {
		if (hdsp->io_type == H9632) {
			hdsp->channel_map = channel_map_H9632_ds;
		} else {
			hdsp->channel_map = channel_map_ds;
		}
	} else {
		switch (hdsp->io_type) {
		case Multiface:
			hdsp->channel_map = channel_map_mf_ss;
			break;
		case Digiface:
		case H9652:
			hdsp->channel_map = channel_map_df_ss;
			break;
		case H9632:
			hdsp->channel_map = channel_map_H9632_ss;
			break;
		default:
			/* should never happen */
			break;
		}
	}
	
	hdsp->system_sample_rate = rate;

	return 0;
}

static void hdsp_set_thru(hdsp_t *hdsp, int channel, int enable)
{

	hdsp->passthru = 0;

	if (channel < 0) {

		int i;

		/* set thru for all channels */

		if (enable) {
			for (i = 0; i < hdsp->max_channels; i++) {
				hdsp_write_gain (hdsp, hdsp_input_to_output_key(hdsp,i,i), UNITY_GAIN);
			}
		} else {
			for (i = 0; i < hdsp->max_channels; i++) {
				hdsp_write_gain (hdsp, hdsp_input_to_output_key(hdsp,i,i), MINUS_INFINITY_GAIN);
			}
		}

	} else {
		int mapped_channel;

		snd_assert(channel < hdsp->max_channels, return);

		mapped_channel = hdsp->channel_map[channel];

		snd_assert(mapped_channel > -1, return);

		if (enable) {
			hdsp_write_gain (hdsp, hdsp_input_to_output_key(hdsp,mapped_channel,mapped_channel), UNITY_GAIN);
		} else {
			hdsp_write_gain (hdsp, hdsp_input_to_output_key(hdsp,mapped_channel,mapped_channel), MINUS_INFINITY_GAIN);
		}
	}
}

static int hdsp_set_passthru(hdsp_t *hdsp, int onoff)
{
	if (onoff) {
		hdsp_set_thru(hdsp, -1, 1);
		hdsp_reset_hw_pointer(hdsp);
		hdsp_silence_playback(hdsp);

		/* we don't want interrupts, so do a
		   custom version of hdsp_start_audio().
		*/

		hdsp->control_register |= (HDSP_Start|HDSP_AudioInterruptEnable|hdsp_encode_latency(7));

		hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
		hdsp->passthru = 1;
	} else {
		hdsp_set_thru(hdsp, -1, 0);
		hdsp_stop_audio(hdsp);		
		hdsp->passthru = 0;
	}

	return 0;
}

/*----------------------------------------------------------------------------
   MIDI
  ----------------------------------------------------------------------------*/

static inline unsigned char snd_hdsp_midi_read_byte (hdsp_t *hdsp, int id)
{
	/* the hardware already does the relevant bit-mask with 0xff */
	if (id) {
		return hdsp_read(hdsp, HDSP_midiDataIn1);
	} else {
		return hdsp_read(hdsp, HDSP_midiDataIn0);
	}
}

static inline void snd_hdsp_midi_write_byte (hdsp_t *hdsp, int id, int val)
{
	/* the hardware already does the relevant bit-mask with 0xff */
	if (id) {
		hdsp_write(hdsp, HDSP_midiDataOut1, val);
	} else {
		hdsp_write(hdsp, HDSP_midiDataOut0, val);
	}
}

static inline int snd_hdsp_midi_input_available (hdsp_t *hdsp, int id)
{
	if (id) {
		return (hdsp_read(hdsp, HDSP_midiStatusIn1) & 0xff);
	} else {
		return (hdsp_read(hdsp, HDSP_midiStatusIn0) & 0xff);
	}
}

static inline int snd_hdsp_midi_output_possible (hdsp_t *hdsp, int id)
{
	int fifo_bytes_used;

	if (id) {
		fifo_bytes_used = hdsp_read(hdsp, HDSP_midiStatusOut1) & 0xff;
	} else {
		fifo_bytes_used = hdsp_read(hdsp, HDSP_midiStatusOut0) & 0xff;
	}

	if (fifo_bytes_used < 128) {
		return  128 - fifo_bytes_used;
	} else {
		return 0;
	}
}

static inline void snd_hdsp_flush_midi_input (hdsp_t *hdsp, int id)
{
	while (snd_hdsp_midi_input_available (hdsp, id)) {
		snd_hdsp_midi_read_byte (hdsp, id);
	}
}

static int snd_hdsp_midi_output_write (hdsp_midi_t *hmidi)
{
	unsigned long flags;
	int n_pending;
	int to_write;
	int i;
	unsigned char buf[128];

	/* Output is not interrupt driven */
		
	spin_lock_irqsave (&hmidi->lock, flags);
	if (hmidi->output) {
		if (!snd_rawmidi_transmit_empty (hmidi->output)) {
			if ((n_pending = snd_hdsp_midi_output_possible (hmidi->hdsp, hmidi->id)) > 0) {
				if (n_pending > (int)sizeof (buf))
					n_pending = sizeof (buf);
				
				if ((to_write = snd_rawmidi_transmit (hmidi->output, buf, n_pending)) > 0) {
					for (i = 0; i < to_write; ++i) 
						snd_hdsp_midi_write_byte (hmidi->hdsp, hmidi->id, buf[i]);
				}
			}
		}
	}
	spin_unlock_irqrestore (&hmidi->lock, flags);
	return 0;
}

static int snd_hdsp_midi_input_read (hdsp_midi_t *hmidi)
{
	unsigned char buf[128]; /* this buffer is designed to match the MIDI input FIFO size */
	unsigned long flags;
	int n_pending;
	int i;

	spin_lock_irqsave (&hmidi->lock, flags);
	if ((n_pending = snd_hdsp_midi_input_available (hmidi->hdsp, hmidi->id)) > 0) {
		if (hmidi->input) {
			if (n_pending > (int)sizeof (buf)) {
				n_pending = sizeof (buf);
			}
			for (i = 0; i < n_pending; ++i) {
				buf[i] = snd_hdsp_midi_read_byte (hmidi->hdsp, hmidi->id);
			}
			if (n_pending) {
				snd_rawmidi_receive (hmidi->input, buf, n_pending);
			}
		} else {
			/* flush the MIDI input FIFO */
			while (--n_pending) {
				snd_hdsp_midi_read_byte (hmidi->hdsp, hmidi->id);
			}
		}
	}
	hmidi->pending = 0;
	if (hmidi->id) {
		hmidi->hdsp->control_register |= HDSP_Midi1InterruptEnable;
	} else {
		hmidi->hdsp->control_register |= HDSP_Midi0InterruptEnable;
	}
	hdsp_write(hmidi->hdsp, HDSP_controlRegister, hmidi->hdsp->control_register);
	spin_unlock_irqrestore (&hmidi->lock, flags);
	return snd_hdsp_midi_output_write (hmidi);
}

static void snd_hdsp_midi_input_trigger(snd_rawmidi_substream_t * substream, int up)
{
	hdsp_t *hdsp;
	hdsp_midi_t *hmidi;
	unsigned long flags;
	u32 ie;

	hmidi = (hdsp_midi_t *) substream->rmidi->private_data;
	hdsp = hmidi->hdsp;
	ie = hmidi->id ? HDSP_Midi1InterruptEnable : HDSP_Midi0InterruptEnable;
	spin_lock_irqsave (&hdsp->lock, flags);
	if (up) {
		if (!(hdsp->control_register & ie)) {
			snd_hdsp_flush_midi_input (hdsp, hmidi->id);
			hdsp->control_register |= ie;
		}
	} else {
		hdsp->control_register &= ~ie;
	}

	hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
	spin_unlock_irqrestore (&hdsp->lock, flags);
}

static void snd_hdsp_midi_output_timer(unsigned long data)
{
	hdsp_midi_t *hmidi = (hdsp_midi_t *) data;
	unsigned long flags;
	
	snd_hdsp_midi_output_write(hmidi);
	spin_lock_irqsave (&hmidi->lock, flags);

	/* this does not bump hmidi->istimer, because the
	   kernel automatically removed the timer when it
	   expired, and we are now adding it back, thus
	   leaving istimer wherever it was set before.  
	*/

	if (hmidi->istimer) {
		hmidi->timer.expires = 1 + jiffies;
		add_timer(&hmidi->timer);
	}

	spin_unlock_irqrestore (&hmidi->lock, flags);
}

static void snd_hdsp_midi_output_trigger(snd_rawmidi_substream_t * substream, int up)
{
	hdsp_midi_t *hmidi;
	unsigned long flags;

	hmidi = (hdsp_midi_t *) substream->rmidi->private_data;
	spin_lock_irqsave (&hmidi->lock, flags);
	if (up) {
		if (!hmidi->istimer) {
			init_timer(&hmidi->timer);
			hmidi->timer.function = snd_hdsp_midi_output_timer;
			hmidi->timer.data = (unsigned long) hmidi;
			hmidi->timer.expires = 1 + jiffies;
			add_timer(&hmidi->timer);
			hmidi->istimer++;
		}
	} else {
		if (hmidi->istimer && --hmidi->istimer <= 0) {
			del_timer (&hmidi->timer);
		}
	}
	spin_unlock_irqrestore (&hmidi->lock, flags);
	if (up)
		snd_hdsp_midi_output_write(hmidi);
}

static int snd_hdsp_midi_input_open(snd_rawmidi_substream_t * substream)
{
	hdsp_midi_t *hmidi;
	unsigned long flags;

	hmidi = (hdsp_midi_t *) substream->rmidi->private_data;
	spin_lock_irqsave (&hmidi->lock, flags);
	snd_hdsp_flush_midi_input (hmidi->hdsp, hmidi->id);
	hmidi->input = substream;
	spin_unlock_irqrestore (&hmidi->lock, flags);

	return 0;
}

static int snd_hdsp_midi_output_open(snd_rawmidi_substream_t * substream)
{
	hdsp_midi_t *hmidi;
	unsigned long flags;

	hmidi = (hdsp_midi_t *) substream->rmidi->private_data;
	spin_lock_irqsave (&hmidi->lock, flags);
	hmidi->output = substream;
	spin_unlock_irqrestore (&hmidi->lock, flags);

	return 0;
}

static int snd_hdsp_midi_input_close(snd_rawmidi_substream_t * substream)
{
	hdsp_midi_t *hmidi;
	unsigned long flags;

	snd_hdsp_midi_input_trigger (substream, 0);

	hmidi = (hdsp_midi_t *) substream->rmidi->private_data;
	spin_lock_irqsave (&hmidi->lock, flags);
	hmidi->input = NULL;
	spin_unlock_irqrestore (&hmidi->lock, flags);

	return 0;
}

static int snd_hdsp_midi_output_close(snd_rawmidi_substream_t * substream)
{
	hdsp_midi_t *hmidi;
	unsigned long flags;

	snd_hdsp_midi_output_trigger (substream, 0);

	hmidi = (hdsp_midi_t *) substream->rmidi->private_data;
	spin_lock_irqsave (&hmidi->lock, flags);
	hmidi->output = NULL;
	spin_unlock_irqrestore (&hmidi->lock, flags);

	return 0;
}

snd_rawmidi_ops_t snd_hdsp_midi_output =
{
	.open =		snd_hdsp_midi_output_open,
	.close =	snd_hdsp_midi_output_close,
	.trigger =	snd_hdsp_midi_output_trigger,
};

snd_rawmidi_ops_t snd_hdsp_midi_input =
{
	.open =		snd_hdsp_midi_input_open,
	.close =	snd_hdsp_midi_input_close,
	.trigger =	snd_hdsp_midi_input_trigger,
};

static int __devinit snd_hdsp_create_midi (snd_card_t *card, hdsp_t *hdsp, int id)
{
	char buf[32];

	hdsp->midi[id].id = id;
	hdsp->midi[id].rmidi = NULL;
	hdsp->midi[id].input = NULL;
	hdsp->midi[id].output = NULL;
	hdsp->midi[id].hdsp = hdsp;
	hdsp->midi[id].istimer = 0;
	hdsp->midi[id].pending = 0;
	spin_lock_init (&hdsp->midi[id].lock);

	sprintf (buf, "%s MIDI %d", card->shortname, id+1);
	if (snd_rawmidi_new (card, buf, id, 1, 1, &hdsp->midi[id].rmidi) < 0) {
		return -1;
	}

	sprintf (hdsp->midi[id].rmidi->name, "%s MIDI %d", card->id, id+1);
	hdsp->midi[id].rmidi->private_data = &hdsp->midi[id];

	snd_rawmidi_set_ops (hdsp->midi[id].rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &snd_hdsp_midi_output);
	snd_rawmidi_set_ops (hdsp->midi[id].rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &snd_hdsp_midi_input);

	hdsp->midi[id].rmidi->info_flags |= SNDRV_RAWMIDI_INFO_OUTPUT |
		SNDRV_RAWMIDI_INFO_INPUT |
		SNDRV_RAWMIDI_INFO_DUPLEX;

	return 0;
}

/*-----------------------------------------------------------------------------
  Control Interface
  ----------------------------------------------------------------------------*/

static u32 snd_hdsp_convert_from_aes(snd_aes_iec958_t *aes)
{
	u32 val = 0;
	val |= (aes->status[0] & IEC958_AES0_PROFESSIONAL) ? HDSP_SPDIFProfessional : 0;
	val |= (aes->status[0] & IEC958_AES0_NONAUDIO) ? HDSP_SPDIFNonAudio : 0;
	if (val & HDSP_SPDIFProfessional)
		val |= (aes->status[0] & IEC958_AES0_PRO_EMPHASIS_5015) ? HDSP_SPDIFEmphasis : 0;
	else
		val |= (aes->status[0] & IEC958_AES0_CON_EMPHASIS_5015) ? HDSP_SPDIFEmphasis : 0;
	return val;
}

static void snd_hdsp_convert_to_aes(snd_aes_iec958_t *aes, u32 val)
{
	aes->status[0] = ((val & HDSP_SPDIFProfessional) ? IEC958_AES0_PROFESSIONAL : 0) |
			 ((val & HDSP_SPDIFNonAudio) ? IEC958_AES0_NONAUDIO : 0);
	if (val & HDSP_SPDIFProfessional)
		aes->status[0] |= (val & HDSP_SPDIFEmphasis) ? IEC958_AES0_PRO_EMPHASIS_5015 : 0;
	else
		aes->status[0] |= (val & HDSP_SPDIFEmphasis) ? IEC958_AES0_CON_EMPHASIS_5015 : 0;
}

static int snd_hdsp_control_spdif_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_IEC958;
	uinfo->count = 1;
	return 0;
}

static int snd_hdsp_control_spdif_get(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	snd_hdsp_convert_to_aes(&ucontrol->value.iec958, hdsp->creg_spdif);
	return 0;
}

static int snd_hdsp_control_spdif_put(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int change;
	u32 val;
	
	val = snd_hdsp_convert_from_aes(&ucontrol->value.iec958);
	spin_lock_irqsave(&hdsp->lock, flags);
	change = val != hdsp->creg_spdif;
	hdsp->creg_spdif = val;
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return change;
}

static int snd_hdsp_control_spdif_stream_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_IEC958;
	uinfo->count = 1;
	return 0;
}

static int snd_hdsp_control_spdif_stream_get(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	snd_hdsp_convert_to_aes(&ucontrol->value.iec958, hdsp->creg_spdif_stream);
	return 0;
}

static int snd_hdsp_control_spdif_stream_put(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int change;
	u32 val;
	
	val = snd_hdsp_convert_from_aes(&ucontrol->value.iec958);
	spin_lock_irqsave(&hdsp->lock, flags);
	change = val != hdsp->creg_spdif_stream;
	hdsp->creg_spdif_stream = val;
	hdsp->control_register &= ~(HDSP_SPDIFProfessional | HDSP_SPDIFNonAudio | HDSP_SPDIFEmphasis);
	hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register |= val);
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return change;
}

static int snd_hdsp_control_spdif_mask_info(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_IEC958;
	uinfo->count = 1;
	return 0;
}

static int snd_hdsp_control_spdif_mask_get(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	ucontrol->value.iec958.status[0] = kcontrol->private_value;
	return 0;
}

#define HDSP_SPDIF_IN(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_PCM,  \
  .name = xname, \
  .index = xindex, \
  .info = snd_hdsp_info_spdif_in, \
  .get = snd_hdsp_get_spdif_in, \
  .put = snd_hdsp_put_spdif_in }

static unsigned int hdsp_spdif_in(hdsp_t *hdsp)
{
	return hdsp_decode_spdif_in(hdsp->control_register & HDSP_SPDIFInputMask);
}

static int hdsp_set_spdif_input(hdsp_t *hdsp, int in)
{
	hdsp->control_register &= ~HDSP_SPDIFInputMask;
	hdsp->control_register |= hdsp_encode_spdif_in(in);
	hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
	return 0;
}

static int snd_hdsp_info_spdif_in(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	static char *texts[4] = {"Optical", "Coaxial", "Internal", "AES"};
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = ((hdsp->io_type == H9632) ? 4 : 3);
	if (uinfo->value.enumerated.item > ((hdsp->io_type == H9632) ? 3 : 2))
		uinfo->value.enumerated.item = ((hdsp->io_type == H9632) ? 3 : 2);
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_hdsp_get_spdif_in(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	ucontrol->value.enumerated.item[0] = hdsp_spdif_in(hdsp);
	return 0;
}

static int snd_hdsp_put_spdif_in(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int change;
	unsigned int val;
	
	if (!snd_hdsp_use_is_exclusive(hdsp))
		return -EBUSY;
	val = ucontrol->value.enumerated.item[0] % ((hdsp->io_type == H9632) ? 4 : 3);
	spin_lock_irqsave(&hdsp->lock, flags);
	change = val != hdsp_spdif_in(hdsp);
	if (change)
		hdsp_set_spdif_input(hdsp, val);
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return change;
}

#define HDSP_SPDIF_OUT(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, .name = xname, .index = xindex, \
  .info = snd_hdsp_info_spdif_bits, \
  .get = snd_hdsp_get_spdif_out, .put = snd_hdsp_put_spdif_out }

static int hdsp_spdif_out(hdsp_t *hdsp)
{
	return (hdsp->control_register & HDSP_SPDIFOpticalOut) ? 1 : 0;
}

static int hdsp_set_spdif_output(hdsp_t *hdsp, int out)
{
	if (out) {
		hdsp->control_register |= HDSP_SPDIFOpticalOut;
	} else {
		hdsp->control_register &= ~HDSP_SPDIFOpticalOut;
	}
	hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
	return 0;
}

static int snd_hdsp_info_spdif_bits(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int snd_hdsp_get_spdif_out(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	ucontrol->value.integer.value[0] = hdsp_spdif_out(hdsp);
	return 0;
}

static int snd_hdsp_put_spdif_out(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int change;
	unsigned int val;
	
	if (!snd_hdsp_use_is_exclusive(hdsp))
		return -EBUSY;
	val = ucontrol->value.integer.value[0] & 1;
	spin_lock_irqsave(&hdsp->lock, flags);
	change = (int)val != hdsp_spdif_out(hdsp);
	hdsp_set_spdif_output(hdsp, val);
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return change;
}

#define HDSP_SPDIF_PROFESSIONAL(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, .name = xname, .index = xindex, \
  .info = snd_hdsp_info_spdif_bits, \
  .get = snd_hdsp_get_spdif_professional, .put = snd_hdsp_put_spdif_professional }

static int hdsp_spdif_professional(hdsp_t *hdsp)
{
	return (hdsp->control_register & HDSP_SPDIFProfessional) ? 1 : 0;
}

static int hdsp_set_spdif_professional(hdsp_t *hdsp, int val)
{
	if (val) {
		hdsp->control_register |= HDSP_SPDIFProfessional;
	} else {
		hdsp->control_register &= ~HDSP_SPDIFProfessional;
	}
	hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
	return 0;
}

static int snd_hdsp_get_spdif_professional(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	ucontrol->value.integer.value[0] = hdsp_spdif_professional(hdsp);
	return 0;
}

static int snd_hdsp_put_spdif_professional(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int change;
	unsigned int val;
	
	if (!snd_hdsp_use_is_exclusive(hdsp))
		return -EBUSY;
	val = ucontrol->value.integer.value[0] & 1;
	spin_lock_irqsave(&hdsp->lock, flags);
	change = (int)val != hdsp_spdif_professional(hdsp);
	hdsp_set_spdif_professional(hdsp, val);
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return change;
}

#define HDSP_SPDIF_EMPHASIS(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, .name = xname, .index = xindex, \
  .info = snd_hdsp_info_spdif_bits, \
  .get = snd_hdsp_get_spdif_emphasis, .put = snd_hdsp_put_spdif_emphasis }

static int hdsp_spdif_emphasis(hdsp_t *hdsp)
{
	return (hdsp->control_register & HDSP_SPDIFEmphasis) ? 1 : 0;
}

static int hdsp_set_spdif_emphasis(hdsp_t *hdsp, int val)
{
	if (val) {
		hdsp->control_register |= HDSP_SPDIFEmphasis;
	} else {
		hdsp->control_register &= ~HDSP_SPDIFEmphasis;
	}
	hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
	return 0;
}

static int snd_hdsp_get_spdif_emphasis(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	ucontrol->value.integer.value[0] = hdsp_spdif_emphasis(hdsp);
	return 0;
}

static int snd_hdsp_put_spdif_emphasis(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int change;
	unsigned int val;
	
	if (!snd_hdsp_use_is_exclusive(hdsp))
		return -EBUSY;
	val = ucontrol->value.integer.value[0] & 1;
	spin_lock_irqsave(&hdsp->lock, flags);
	change = (int)val != hdsp_spdif_emphasis(hdsp);
	hdsp_set_spdif_emphasis(hdsp, val);
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return change;
}

#define HDSP_SPDIF_NON_AUDIO(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, .name = xname, .index = xindex, \
  .info = snd_hdsp_info_spdif_bits, \
  .get = snd_hdsp_get_spdif_nonaudio, .put = snd_hdsp_put_spdif_nonaudio }

static int hdsp_spdif_nonaudio(hdsp_t *hdsp)
{
	return (hdsp->control_register & HDSP_SPDIFNonAudio) ? 1 : 0;
}

static int hdsp_set_spdif_nonaudio(hdsp_t *hdsp, int val)
{
	if (val) {
		hdsp->control_register |= HDSP_SPDIFNonAudio;
	} else {
		hdsp->control_register &= ~HDSP_SPDIFNonAudio;
	}
	hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
	return 0;
}

static int snd_hdsp_get_spdif_nonaudio(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	ucontrol->value.integer.value[0] = hdsp_spdif_nonaudio(hdsp);
	return 0;
}

static int snd_hdsp_put_spdif_nonaudio(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int change;
	unsigned int val;
	
	if (!snd_hdsp_use_is_exclusive(hdsp))
		return -EBUSY;
	val = ucontrol->value.integer.value[0] & 1;
	spin_lock_irqsave(&hdsp->lock, flags);
	change = (int)val != hdsp_spdif_nonaudio(hdsp);
	hdsp_set_spdif_nonaudio(hdsp, val);
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return change;
}

#define HDSP_SPDIF_SAMPLE_RATE(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, \
  .name = xname, \
  .index = xindex, \
  .access = SNDRV_CTL_ELEM_ACCESS_READ, \
  .info = snd_hdsp_info_spdif_sample_rate, \
  .get = snd_hdsp_get_spdif_sample_rate \
}

static int snd_hdsp_info_spdif_sample_rate(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	static char *texts[] = {"32000", "44100", "48000", "64000", "88200", "96000", "None", "128000", "176400", "192000"};
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = (hdsp->io_type == H9632) ? 10 : 7;
	if (uinfo->value.enumerated.item >= uinfo->value.enumerated.items)
		uinfo->value.enumerated.item = uinfo->value.enumerated.items - 1;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_hdsp_get_spdif_sample_rate(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	switch (hdsp_spdif_sample_rate(hdsp)) {
	case 32000:
		ucontrol->value.enumerated.item[0] = 0;
		break;
	case 44100:
		ucontrol->value.enumerated.item[0] = 1;
		break;
	case 48000:
		ucontrol->value.enumerated.item[0] = 2;
		break;
	case 64000:
		ucontrol->value.enumerated.item[0] = 3;
		break;
	case 88200:
		ucontrol->value.enumerated.item[0] = 4;
		break;
	case 96000:
		ucontrol->value.enumerated.item[0] = 5;
		break;
	case 128000:
		ucontrol->value.enumerated.item[0] = 7;
		break;
	case 176400:
		ucontrol->value.enumerated.item[0] = 8;
		break;
	case 192000:
		ucontrol->value.enumerated.item[0] = 9;
		break;
	default:
		ucontrol->value.enumerated.item[0] = 6;		
	}
	return 0;
}

#define HDSP_SYSTEM_SAMPLE_RATE(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, \
  .name = xname, \
  .index = xindex, \
  .access = SNDRV_CTL_ELEM_ACCESS_READ, \
  .info = snd_hdsp_info_system_sample_rate, \
  .get = snd_hdsp_get_system_sample_rate \
}

static int snd_hdsp_info_system_sample_rate(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	return 0;
}

static int snd_hdsp_get_system_sample_rate(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	ucontrol->value.enumerated.item[0] = hdsp->system_sample_rate;
	return 0;
}

#define HDSP_AUTOSYNC_SAMPLE_RATE(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_PCM, \
  .name = xname, \
  .index = xindex, \
  .access = SNDRV_CTL_ELEM_ACCESS_READ, \
  .info = snd_hdsp_info_autosync_sample_rate, \
  .get = snd_hdsp_get_autosync_sample_rate \
}

static int snd_hdsp_info_autosync_sample_rate(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	static char *texts[] = {"32000", "44100", "48000", "64000", "88200", "96000", "None", "128000", "176400", "192000"};	
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = (hdsp->io_type == H9632) ? 10 : 7 ;
	if (uinfo->value.enumerated.item >= uinfo->value.enumerated.items)
		uinfo->value.enumerated.item = uinfo->value.enumerated.items - 1;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_hdsp_get_autosync_sample_rate(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	switch (hdsp_external_sample_rate(hdsp)) {
	case 32000:
		ucontrol->value.enumerated.item[0] = 0;
		break;
	case 44100:
		ucontrol->value.enumerated.item[0] = 1;
		break;
	case 48000:
		ucontrol->value.enumerated.item[0] = 2;
		break;
	case 64000:
		ucontrol->value.enumerated.item[0] = 3;
		break;
	case 88200:
		ucontrol->value.enumerated.item[0] = 4;
		break;
	case 96000:
		ucontrol->value.enumerated.item[0] = 5;
		break;
	case 128000:
		ucontrol->value.enumerated.item[0] = 7;
		break;
	case 176400:
		ucontrol->value.enumerated.item[0] = 8;
		break;
	case 192000:
		ucontrol->value.enumerated.item[0] = 9;
		break;	
	default:
		ucontrol->value.enumerated.item[0] = 6;		
	}
	return 0;
}

#define HDSP_SYSTEM_CLOCK_MODE(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, \
  .name = xname, \
  .index = xindex, \
  .access = SNDRV_CTL_ELEM_ACCESS_READ, \
  .info = snd_hdsp_info_system_clock_mode, \
  .get = snd_hdsp_get_system_clock_mode \
}

static int hdsp_system_clock_mode(hdsp_t *hdsp)
{
	if (hdsp->control_register & HDSP_ClockModeMaster) {
		return 0;
	} else if (hdsp_external_sample_rate(hdsp) != hdsp->system_sample_rate) {
			return 0;
	}
	return 1;
}

static int snd_hdsp_info_system_clock_mode(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	static char *texts[] = {"Master", "Slave" };
	
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 2;
	if (uinfo->value.enumerated.item >= uinfo->value.enumerated.items)
		uinfo->value.enumerated.item = uinfo->value.enumerated.items - 1;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_hdsp_get_system_clock_mode(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	ucontrol->value.enumerated.item[0] = hdsp_system_clock_mode(hdsp);
	return 0;
}

#define HDSP_CLOCK_SOURCE(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_PCM, \
  .name = xname, \
  .index = xindex, \
  .info = snd_hdsp_info_clock_source, \
  .get = snd_hdsp_get_clock_source, \
  .put = snd_hdsp_put_clock_source \
}

static int hdsp_clock_source(hdsp_t *hdsp)
{
	if (hdsp->control_register & HDSP_ClockModeMaster) {
		switch (hdsp->system_sample_rate) {
		case 32000:
			return 1;
		case 44100:
			return 2;
		case 48000:
			return 3;
		case 64000:
			return 4;
		case 88200:
			return 5;
		case 96000:
			return 6;
		case 128000:
			return 7;
		case 176400:
			return 8;
		case 192000:
			return 9;
		default:
			return 3;	
		}
	} else {
		return 0;
	}
}

static int hdsp_set_clock_source(hdsp_t *hdsp, int mode)
{
	int rate;
	switch (mode) {
	case HDSP_CLOCK_SOURCE_AUTOSYNC:
		if (hdsp_external_sample_rate(hdsp) != 0) {
		    if (!hdsp_set_rate(hdsp, hdsp_external_sample_rate(hdsp), 1)) {
			hdsp->control_register &= ~HDSP_ClockModeMaster;		
			hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
			return 0;
		    }
		}
		return -1;
	case HDSP_CLOCK_SOURCE_INTERNAL_32KHZ:
		rate = 32000;
		break;
	case HDSP_CLOCK_SOURCE_INTERNAL_44_1KHZ:
		rate = 44100;
		break;	    
	case HDSP_CLOCK_SOURCE_INTERNAL_48KHZ:
		rate = 48000;
		break;
	case HDSP_CLOCK_SOURCE_INTERNAL_64KHZ:
		rate = 64000;
		break;
	case HDSP_CLOCK_SOURCE_INTERNAL_88_2KHZ:
		rate = 88200;
		break;
	case HDSP_CLOCK_SOURCE_INTERNAL_96KHZ:
		rate = 96000;
		break;
	case HDSP_CLOCK_SOURCE_INTERNAL_128KHZ:
		rate = 128000;
		break;
	case HDSP_CLOCK_SOURCE_INTERNAL_176_4KHZ:
		rate = 176400;
		break;
	case HDSP_CLOCK_SOURCE_INTERNAL_192KHZ:
		rate = 192000;
		break;
	default:
		rate = 48000;
	}
	hdsp->control_register |= HDSP_ClockModeMaster;
	hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
	hdsp_set_rate(hdsp, rate, 1);
	return 0;
}

static int snd_hdsp_info_clock_source(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	static char *texts[] = {"AutoSync", "Internal 32.0 kHz", "Internal 44.1 kHz", "Internal 48.0 kHz", "Internal 64.0 kHz", "Internal 88.2 kHz", "Internal 96.0 kHz", "Internal 128 kHz", "Internal 176.4 kHz", "Internal 192.0 KHz" };
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	if (hdsp->io_type == H9632)
	    uinfo->value.enumerated.items = 10;
	else
	    uinfo->value.enumerated.items = 7;	
	if (uinfo->value.enumerated.item >= uinfo->value.enumerated.items)
		uinfo->value.enumerated.item = uinfo->value.enumerated.items - 1;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_hdsp_get_clock_source(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	ucontrol->value.enumerated.item[0] = hdsp_clock_source(hdsp);
	return 0;
}

static int snd_hdsp_put_clock_source(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int change;
	int val;
	
	if (!snd_hdsp_use_is_exclusive(hdsp))
		return -EBUSY;
	val = ucontrol->value.enumerated.item[0];
	if (val < 0) val = 0;
	if (hdsp->io_type == H9632) {
	    if (val > 9) val = 9;
	} else {
	    if (val > 6) val = 6;
	}
	spin_lock_irqsave(&hdsp->lock, flags);
	if (val != hdsp_clock_source(hdsp)) {
		change = (hdsp_set_clock_source(hdsp, val) == 0) ? 1 : 0;
	} else {
		change = 0;
	}
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return change;
}

#define HDSP_DA_GAIN(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, \
  .name = xname, \
  .index = xindex, \
  .info = snd_hdsp_info_da_gain, \
  .get = snd_hdsp_get_da_gain, \
  .put = snd_hdsp_put_da_gain \
}

static int hdsp_da_gain(hdsp_t *hdsp)
{
	switch (hdsp->control_register & HDSP_DAGainMask) {
	case HDSP_DAGainHighGain:
		return 0;
	case HDSP_DAGainPlus4dBu:
		return 1;
	case HDSP_DAGainMinus10dBV:
		return 2;
	default:
		return 1;	
	}
}

static int hdsp_set_da_gain(hdsp_t *hdsp, int mode)
{
	hdsp->control_register &= ~HDSP_DAGainMask;
	switch (mode) {
	case 0:
		hdsp->control_register |= HDSP_DAGainHighGain;
		break;
	case 1:
		hdsp->control_register |= HDSP_DAGainPlus4dBu;
		break;
	case 2:
		hdsp->control_register |= HDSP_DAGainMinus10dBV;		
		break;	    
	default:
		return -1;

	}
	hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
	return 0;
}

static int snd_hdsp_info_da_gain(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	static char *texts[] = {"Hi Gain", "+4 dBu", "-10 dbV"};
	
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 3;
	if (uinfo->value.enumerated.item >= uinfo->value.enumerated.items)
		uinfo->value.enumerated.item = uinfo->value.enumerated.items - 1;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_hdsp_get_da_gain(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	ucontrol->value.enumerated.item[0] = hdsp_da_gain(hdsp);
	return 0;
}

static int snd_hdsp_put_da_gain(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int change;
	int val;
	
	if (!snd_hdsp_use_is_exclusive(hdsp))
		return -EBUSY;
	val = ucontrol->value.enumerated.item[0];
	if (val < 0) val = 0;
	if (val > 2) val = 2;
	spin_lock_irqsave(&hdsp->lock, flags);
	if (val != hdsp_da_gain(hdsp)) {
		change = (hdsp_set_da_gain(hdsp, val) == 0) ? 1 : 0;
	} else {
		change = 0;
	}
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return change;
}

#define HDSP_AD_GAIN(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, \
  .name = xname, \
  .index = xindex, \
  .info = snd_hdsp_info_ad_gain, \
  .get = snd_hdsp_get_ad_gain, \
  .put = snd_hdsp_put_ad_gain \
}

static int hdsp_ad_gain(hdsp_t *hdsp)
{
	switch (hdsp->control_register & HDSP_ADGainMask) {
	case HDSP_ADGainMinus10dBV:
		return 0;
	case HDSP_ADGainPlus4dBu:
		return 1;
	case HDSP_ADGainLowGain:
		return 2;
	default:
		return 1;	
	}
}

static int hdsp_set_ad_gain(hdsp_t *hdsp, int mode)
{
	hdsp->control_register &= ~HDSP_ADGainMask;
	switch (mode) {
	case 0:
		hdsp->control_register |= HDSP_ADGainMinus10dBV;
		break;
	case 1:
		hdsp->control_register |= HDSP_ADGainPlus4dBu;		
		break;
	case 2:
		hdsp->control_register |= HDSP_ADGainLowGain;		
		break;	    
	default:
		return -1;

	}
	hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
	return 0;
}

static int snd_hdsp_info_ad_gain(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	static char *texts[] = {"-10 dBV", "+4 dBu", "Lo Gain"};
	
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 3;
	if (uinfo->value.enumerated.item >= uinfo->value.enumerated.items)
		uinfo->value.enumerated.item = uinfo->value.enumerated.items - 1;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_hdsp_get_ad_gain(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	ucontrol->value.enumerated.item[0] = hdsp_ad_gain(hdsp);
	return 0;
}

static int snd_hdsp_put_ad_gain(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int change;
	int val;
	
	if (!snd_hdsp_use_is_exclusive(hdsp))
		return -EBUSY;
	val = ucontrol->value.enumerated.item[0];
	if (val < 0) val = 0;
	if (val > 2) val = 2;
	spin_lock_irqsave(&hdsp->lock, flags);
	if (val != hdsp_ad_gain(hdsp)) {
		change = (hdsp_set_ad_gain(hdsp, val) == 0) ? 1 : 0;
	} else {
		change = 0;
	}
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return change;
}

#define HDSP_PHONE_GAIN(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, \
  .name = xname, \
  .index = xindex, \
  .info = snd_hdsp_info_phone_gain, \
  .get = snd_hdsp_get_phone_gain, \
  .put = snd_hdsp_put_phone_gain \
}

static int hdsp_phone_gain(hdsp_t *hdsp)
{
	switch (hdsp->control_register & HDSP_PhoneGainMask) {
	case HDSP_PhoneGain0dB:
		return 0;
	case HDSP_PhoneGainMinus6dB:
		return 1;
	case HDSP_PhoneGainMinus12dB:
		return 2;
	default:
		return 0;	
	}
}

static int hdsp_set_phone_gain(hdsp_t *hdsp, int mode)
{
	hdsp->control_register &= ~HDSP_PhoneGainMask;
	switch (mode) {
	case 0:
		hdsp->control_register |= HDSP_PhoneGain0dB;
		break;
	case 1:
		hdsp->control_register |= HDSP_PhoneGainMinus6dB;		
		break;
	case 2:
		hdsp->control_register |= HDSP_PhoneGainMinus12dB;		
		break;	    
	default:
		return -1;

	}
	hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
	return 0;
}

static int snd_hdsp_info_phone_gain(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	static char *texts[] = {"0 dB", "-6 dB", "-12 dB"};
	
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 3;
	if (uinfo->value.enumerated.item >= uinfo->value.enumerated.items)
		uinfo->value.enumerated.item = uinfo->value.enumerated.items - 1;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_hdsp_get_phone_gain(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	ucontrol->value.enumerated.item[0] = hdsp_phone_gain(hdsp);
	return 0;
}

static int snd_hdsp_put_phone_gain(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int change;
	int val;
	
	if (!snd_hdsp_use_is_exclusive(hdsp))
		return -EBUSY;
	val = ucontrol->value.enumerated.item[0];
	if (val < 0) val = 0;
	if (val > 2) val = 2;
	spin_lock_irqsave(&hdsp->lock, flags);
	if (val != hdsp_phone_gain(hdsp)) {
		change = (hdsp_set_phone_gain(hdsp, val) == 0) ? 1 : 0;
	} else {
		change = 0;
	}
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return change;
}

#define HDSP_XLR_BREAKOUT_CABLE(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, \
  .name = xname, \
  .index = xindex, \
  .info = snd_hdsp_info_xlr_breakout_cable, \
  .get = snd_hdsp_get_xlr_breakout_cable, \
  .put = snd_hdsp_put_xlr_breakout_cable \
}

static int hdsp_xlr_breakout_cable(hdsp_t *hdsp)
{
	if (hdsp->control_register & HDSP_XLRBreakoutCable) {
		return 1;
	}
	return 0;
}

static int hdsp_set_xlr_breakout_cable(hdsp_t *hdsp, int mode)
{
	if (mode) {
		hdsp->control_register |= HDSP_XLRBreakoutCable;
	} else {
		hdsp->control_register &= ~HDSP_XLRBreakoutCable;
	}
	hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
	return 0;
}

static int snd_hdsp_info_xlr_breakout_cable(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int snd_hdsp_get_xlr_breakout_cable(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	ucontrol->value.enumerated.item[0] = hdsp_xlr_breakout_cable(hdsp);
	return 0;
}

static int snd_hdsp_put_xlr_breakout_cable(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int change;
	int val;
	
	if (!snd_hdsp_use_is_exclusive(hdsp))
		return -EBUSY;
	val = ucontrol->value.integer.value[0] & 1;
	spin_lock_irqsave(&hdsp->lock, flags);
	change = (int)val != hdsp_xlr_breakout_cable(hdsp);
	hdsp_set_xlr_breakout_cable(hdsp, val);
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return change;
}

/* (De)activates old RME Analog Extension Board
   These are connected to the internal ADAT connector
   Switching this on desactivates external ADAT
*/
#define HDSP_AEB(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, \
  .name = xname, \
  .index = xindex, \
  .info = snd_hdsp_info_aeb, \
  .get = snd_hdsp_get_aeb, \
  .put = snd_hdsp_put_aeb \
}

static int hdsp_aeb(hdsp_t *hdsp)
{
	if (hdsp->control_register & HDSP_AnalogExtensionBoard) {
		return 1;
	}
	return 0;
}

static int hdsp_set_aeb(hdsp_t *hdsp, int mode)
{
	if (mode) {
		hdsp->control_register |= HDSP_AnalogExtensionBoard;
	} else {
		hdsp->control_register &= ~HDSP_AnalogExtensionBoard;
	}
	hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
	return 0;
}

static int snd_hdsp_info_aeb(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int snd_hdsp_get_aeb(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	ucontrol->value.enumerated.item[0] = hdsp_aeb(hdsp);
	return 0;
}

static int snd_hdsp_put_aeb(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int change;
	int val;
	
	if (!snd_hdsp_use_is_exclusive(hdsp))
		return -EBUSY;
	val = ucontrol->value.integer.value[0] & 1;
	spin_lock_irqsave(&hdsp->lock, flags);
	change = (int)val != hdsp_aeb(hdsp);
	hdsp_set_aeb(hdsp, val);
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return change;
}

#define HDSP_PREF_SYNC_REF(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, \
  .name = xname, \
  .index = xindex, \
  .info = snd_hdsp_info_pref_sync_ref, \
  .get = snd_hdsp_get_pref_sync_ref, \
  .put = snd_hdsp_put_pref_sync_ref \
}

static int hdsp_pref_sync_ref(hdsp_t *hdsp)
{
	/* Notice that this looks at the requested sync source,
	   not the one actually in use.
	*/

	switch (hdsp->control_register & HDSP_SyncRefMask) {
	case HDSP_SyncRef_ADAT1:
		return HDSP_SYNC_FROM_ADAT1;
	case HDSP_SyncRef_ADAT2:
		return HDSP_SYNC_FROM_ADAT2;
	case HDSP_SyncRef_ADAT3:
		return HDSP_SYNC_FROM_ADAT3;
	case HDSP_SyncRef_SPDIF:
		return HDSP_SYNC_FROM_SPDIF;
	case HDSP_SyncRef_WORD:
		return HDSP_SYNC_FROM_WORD;
	case HDSP_SyncRef_ADAT_SYNC:
		return HDSP_SYNC_FROM_ADAT_SYNC;
	default:
		return HDSP_SYNC_FROM_WORD;
	}
	return 0;
}

static int hdsp_set_pref_sync_ref(hdsp_t *hdsp, int pref)
{
	hdsp->control_register &= ~HDSP_SyncRefMask;
	switch (pref) {
	case HDSP_SYNC_FROM_ADAT1:
		hdsp->control_register &= ~HDSP_SyncRefMask; /* clear SyncRef bits */
		break;
	case HDSP_SYNC_FROM_ADAT2:
		hdsp->control_register |= HDSP_SyncRef_ADAT2;
		break;
	case HDSP_SYNC_FROM_ADAT3:
		hdsp->control_register |= HDSP_SyncRef_ADAT3;
		break;
	case HDSP_SYNC_FROM_SPDIF:
		hdsp->control_register |= HDSP_SyncRef_SPDIF;
		break;
	case HDSP_SYNC_FROM_WORD:
		hdsp->control_register |= HDSP_SyncRef_WORD;
		break;
	case HDSP_SYNC_FROM_ADAT_SYNC:
		hdsp->control_register |= HDSP_SyncRef_ADAT_SYNC;
		break;
	default:
		return -1;
	}
	hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
	return 0;
}

static int snd_hdsp_info_pref_sync_ref(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	static char *texts[] = {"Word", "IEC958", "ADAT1", "ADAT Sync", "ADAT2", "ADAT3" };
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;

	switch (hdsp->io_type) {
	case Digiface:
	case H9652:
		uinfo->value.enumerated.items = 6;
		break;
	case Multiface:
		uinfo->value.enumerated.items = 4;
		break;
	case H9632:
		uinfo->value.enumerated.items = 3;
		break;
	default:
		uinfo->value.enumerated.items = 0;
		break;
	}
		
	if (uinfo->value.enumerated.item >= uinfo->value.enumerated.items)
		uinfo->value.enumerated.item = uinfo->value.enumerated.items - 1;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_hdsp_get_pref_sync_ref(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	ucontrol->value.enumerated.item[0] = hdsp_pref_sync_ref(hdsp);
	return 0;
}

static int snd_hdsp_put_pref_sync_ref(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int change, max;
	unsigned int val;
	
	if (!snd_hdsp_use_is_exclusive(hdsp))
		return -EBUSY;

	switch (hdsp->io_type) {
	case Digiface:
	case H9652:
		max = 6;
		break;
	case Multiface:
		max = 4;
		break;
	case H9632:
		max = 3;
		break;
	default:
		return -EIO;
	}

	val = ucontrol->value.enumerated.item[0] % max;
	spin_lock_irqsave(&hdsp->lock, flags);
	change = (int)val != hdsp_pref_sync_ref(hdsp);
	hdsp_set_pref_sync_ref(hdsp, val);
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return change;
}

#define HDSP_AUTOSYNC_REF(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, \
  .name = xname, \
  .index = xindex, \
  .access = SNDRV_CTL_ELEM_ACCESS_READ, \
  .info = snd_hdsp_info_autosync_ref, \
  .get = snd_hdsp_get_autosync_ref, \
}

static int hdsp_autosync_ref(hdsp_t *hdsp)
{
	/* This looks at the autosync selected sync reference */
	unsigned int status2 = hdsp_read(hdsp, HDSP_status2Register);

	switch (status2 & HDSP_SelSyncRefMask) {
	case HDSP_SelSyncRef_WORD:
		return HDSP_AUTOSYNC_FROM_WORD;
	case HDSP_SelSyncRef_ADAT_SYNC:
		return HDSP_AUTOSYNC_FROM_ADAT_SYNC;
	case HDSP_SelSyncRef_SPDIF:
		return HDSP_AUTOSYNC_FROM_SPDIF;
	case HDSP_SelSyncRefMask:
		return HDSP_AUTOSYNC_FROM_NONE;	
	case HDSP_SelSyncRef_ADAT1:
		return HDSP_AUTOSYNC_FROM_ADAT1;
	case HDSP_SelSyncRef_ADAT2:
		return HDSP_AUTOSYNC_FROM_ADAT2;
	case HDSP_SelSyncRef_ADAT3:
		return HDSP_AUTOSYNC_FROM_ADAT3;
	default:
		return HDSP_AUTOSYNC_FROM_WORD;
	}
	return 0;
}

static int snd_hdsp_info_autosync_ref(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	static char *texts[] = {"Word", "ADAT Sync", "IEC958", "None", "ADAT1", "ADAT2", "ADAT3" };
	
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 7;
	if (uinfo->value.enumerated.item >= uinfo->value.enumerated.items)
		uinfo->value.enumerated.item = uinfo->value.enumerated.items - 1;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int snd_hdsp_get_autosync_ref(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	
	ucontrol->value.enumerated.item[0] = hdsp_pref_sync_ref(hdsp);
	return 0;
}

#define HDSP_PASSTHRU(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, \
  .name = xname, \
  .index = xindex, \
  .info = snd_hdsp_info_passthru, \
  .put = snd_hdsp_put_passthru, \
  .get = snd_hdsp_get_passthru \
}

static int snd_hdsp_info_passthru(snd_kcontrol_t * kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int snd_hdsp_get_passthru(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;

	spin_lock_irqsave(&hdsp->lock, flags);
	ucontrol->value.integer.value[0] = hdsp->passthru;
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return 0;
}

static int snd_hdsp_put_passthru(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int change;
	unsigned int val;
	int err = 0;

	if (!snd_hdsp_use_is_exclusive(hdsp))
		return -EBUSY;

	val = ucontrol->value.integer.value[0] & 1;
	spin_lock_irqsave(&hdsp->lock, flags);
	change = (ucontrol->value.integer.value[0] != hdsp->passthru);
	if (change)
		err = hdsp_set_passthru(hdsp, val);
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return err ? err : change;
}

#define HDSP_LINE_OUT(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, \
  .name = xname, \
  .index = xindex, \
  .info = snd_hdsp_info_line_out, \
  .get = snd_hdsp_get_line_out, \
  .put = snd_hdsp_put_line_out \
}

static int hdsp_line_out(hdsp_t *hdsp)
{
	return (hdsp->control_register & HDSP_LineOut) ? 1 : 0;
}

static int hdsp_set_line_output(hdsp_t *hdsp, int out)
{
	if (out) {
		hdsp->control_register |= HDSP_LineOut;
	} else {
		hdsp->control_register &= ~HDSP_LineOut;
	}
	hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
	return 0;
}

static int snd_hdsp_info_line_out(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int snd_hdsp_get_line_out(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	
	spin_lock_irqsave(&hdsp->lock, flags);
	ucontrol->value.integer.value[0] = hdsp_line_out(hdsp);
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return 0;
}

static int snd_hdsp_put_line_out(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int change;
	unsigned int val;
	
	if (!snd_hdsp_use_is_exclusive(hdsp))
		return -EBUSY;
	val = ucontrol->value.integer.value[0] & 1;
	spin_lock_irqsave(&hdsp->lock, flags);
	change = (int)val != hdsp_line_out(hdsp);
	hdsp_set_line_output(hdsp, val);
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return change;
}

#define HDSP_MIXER(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, \
  .name = xname, \
  .index = xindex, \
  .access = SNDRV_CTL_ELEM_ACCESS_READWRITE | \
		 SNDRV_CTL_ELEM_ACCESS_VOLATILE, \
  .info = snd_hdsp_info_mixer, \
  .get = snd_hdsp_get_mixer, \
  .put = snd_hdsp_put_mixer \
}

static int snd_hdsp_info_mixer(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 3;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 65536;
	uinfo->value.integer.step = 1;
	return 0;
}

static int snd_hdsp_get_mixer(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int source;
	int destination;
	int addr;

	source = ucontrol->value.integer.value[0];
	destination = ucontrol->value.integer.value[1];
	
	if (source >= hdsp->max_channels) {
		addr = hdsp_playback_to_output_key(hdsp,source-hdsp->max_channels,destination);
	} else {
		addr = hdsp_input_to_output_key(hdsp,source, destination);
	}
	
	spin_lock_irqsave(&hdsp->lock, flags);
	ucontrol->value.integer.value[2] = hdsp_read_gain (hdsp, addr);
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return 0;
}

static int snd_hdsp_put_mixer(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	int change;
	int source;
	int destination;
	int gain;
	int addr;

	if (!snd_hdsp_use_is_exclusive(hdsp))
		return -EBUSY;

	source = ucontrol->value.integer.value[0];
	destination = ucontrol->value.integer.value[1];

	if (source >= hdsp->max_channels) {
		addr = hdsp_playback_to_output_key(hdsp,source-hdsp->max_channels, destination);
	} else {
		addr = hdsp_input_to_output_key(hdsp,source, destination);
	}

	gain = ucontrol->value.integer.value[2];

	spin_lock_irqsave(&hdsp->lock, flags);
	change = gain != hdsp_read_gain(hdsp, addr);
	if (change)
		hdsp_write_gain(hdsp, addr, gain);
	spin_unlock_irqrestore(&hdsp->lock, flags);
	return change;
}

#define HDSP_WC_SYNC_CHECK(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, \
  .name = xname, \
  .index = xindex, \
  .access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE, \
  .info = snd_hdsp_info_sync_check, \
  .get = snd_hdsp_get_wc_sync_check \
}

static int snd_hdsp_info_sync_check(snd_kcontrol_t *kcontrol, snd_ctl_elem_info_t * uinfo)
{
	static char *texts[] = {"No Lock", "Lock", "Sync" };	
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 3;
	if (uinfo->value.enumerated.item >= uinfo->value.enumerated.items)
		uinfo->value.enumerated.item = uinfo->value.enumerated.items - 1;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int hdsp_wc_sync_check(hdsp_t *hdsp)
{
	int status2 = hdsp_read(hdsp, HDSP_status2Register);
	if (status2 & HDSP_wc_lock) {
		if (status2 & HDSP_wc_sync) {
			return 2;
		} else {
			 return 1;
		}
	} else {		
		return 0;
	}
	return 0;
}

static int snd_hdsp_get_wc_sync_check(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);

	ucontrol->value.enumerated.item[0] = hdsp_wc_sync_check(hdsp);
	return 0;
}

#define HDSP_SPDIF_SYNC_CHECK(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, \
  .name = xname, \
  .index = xindex, \
  .access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE, \
  .info = snd_hdsp_info_sync_check, \
  .get = snd_hdsp_get_spdif_sync_check \
}

static int hdsp_spdif_sync_check(hdsp_t *hdsp)
{
	int status = hdsp_read(hdsp, HDSP_statusRegister);
	if (status & HDSP_SPDIFErrorFlag) {
		return 0;
	} else {	
		if (status & HDSP_SPDIFSync) {
			return 2;
		} else {
			return 1;
		}
	}
	return 0;
}

static int snd_hdsp_get_spdif_sync_check(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);

	ucontrol->value.enumerated.item[0] = hdsp_spdif_sync_check(hdsp);
	return 0;
}

#define HDSP_ADATSYNC_SYNC_CHECK(xname, xindex) \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, \
  .name = xname, \
  .index = xindex, \
  .access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE, \
  .info = snd_hdsp_info_sync_check, \
  .get = snd_hdsp_get_adatsync_sync_check \
}

static int hdsp_adatsync_sync_check(hdsp_t *hdsp)
{
	int status = hdsp_read(hdsp, HDSP_statusRegister);
	if (status & HDSP_TimecodeLock) {
		if (status & HDSP_TimecodeSync) {
			return 2;
		} else {
			return 1;
		}
	} else {
		return 0;
	}
}	

static int snd_hdsp_get_adatsync_sync_check(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);

	ucontrol->value.enumerated.item[0] = hdsp_adatsync_sync_check(hdsp);
	return 0;
}

#define HDSP_ADAT_SYNC_CHECK \
{ .iface = SNDRV_CTL_ELEM_IFACE_HWDEP, \
  .access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE, \
  .info = snd_hdsp_info_sync_check, \
  .get = snd_hdsp_get_adat_sync_check \
}

static int hdsp_adat_sync_check(hdsp_t *hdsp, int idx)
{	
	int status = hdsp_read(hdsp, HDSP_statusRegister);
	
	if (status & (HDSP_Lock0>>idx)) {
		if (status & (HDSP_Sync0>>idx)) {
			return 2;
		} else {
			return 1;		
		}
	} else {
		return 0;
	}		
} 

static int snd_hdsp_get_adat_sync_check(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	int offset;
	hdsp_t *hdsp = _snd_kcontrol_chip(kcontrol);

	offset = ucontrol->id.index - 1;
	snd_assert(offset >= 0);

	switch (hdsp->io_type) {
	case Digiface:
	case H9652:
		if (offset >= 3)
			return -EINVAL;
		break;
	case Multiface:
	case H9632:
		if (offset >= 1) 
			return -EINVAL;
		break;
	default:
		return -EIO;
	}

	ucontrol->value.enumerated.item[0] = hdsp_adat_sync_check(hdsp, offset);
	return 0;
}

static snd_kcontrol_new_t snd_hdsp_9632_controls[] = {
HDSP_DA_GAIN("DA Gain", 0),
HDSP_AD_GAIN("AD Gain", 0),
HDSP_PHONE_GAIN("Phones Gain", 0),
HDSP_XLR_BREAKOUT_CABLE("XLR Breakout Cable", 0)
};

static snd_kcontrol_new_t snd_hdsp_controls[] = {
{
	.iface =	SNDRV_CTL_ELEM_IFACE_PCM,
	.name =		SNDRV_CTL_NAME_IEC958("",PLAYBACK,DEFAULT),
	.info =		snd_hdsp_control_spdif_info,
	.get =		snd_hdsp_control_spdif_get,
	.put =		snd_hdsp_control_spdif_put,
},
{
	.access =	SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_INACTIVE,
	.iface =	SNDRV_CTL_ELEM_IFACE_PCM,
	.name =		SNDRV_CTL_NAME_IEC958("",PLAYBACK,PCM_STREAM),
	.info =		snd_hdsp_control_spdif_stream_info,
	.get =		snd_hdsp_control_spdif_stream_get,
	.put =		snd_hdsp_control_spdif_stream_put,
},
{
	.access =	SNDRV_CTL_ELEM_ACCESS_READ,
	.iface =	SNDRV_CTL_ELEM_IFACE_MIXER,
	.name =		SNDRV_CTL_NAME_IEC958("",PLAYBACK,CON_MASK),
	.info =		snd_hdsp_control_spdif_mask_info,
	.get =		snd_hdsp_control_spdif_mask_get,
	.private_value = IEC958_AES0_NONAUDIO |
  			 IEC958_AES0_PROFESSIONAL |
			 IEC958_AES0_CON_EMPHASIS,	                                                                                      
},
{
	.access =	SNDRV_CTL_ELEM_ACCESS_READ,
	.iface =	SNDRV_CTL_ELEM_IFACE_MIXER,
	.name =		SNDRV_CTL_NAME_IEC958("",PLAYBACK,PRO_MASK),
	.info =		snd_hdsp_control_spdif_mask_info,
	.get =		snd_hdsp_control_spdif_mask_get,
	.private_value = IEC958_AES0_NONAUDIO |
			 IEC958_AES0_PROFESSIONAL |
			 IEC958_AES0_PRO_EMPHASIS,
},
HDSP_MIXER("Mixer", 0),
HDSP_SPDIF_IN("IEC958 Input Connector", 0),
HDSP_SPDIF_OUT("IEC958 Output also on ADAT1", 0),
HDSP_SPDIF_PROFESSIONAL("IEC958 Professional Bit", 0),
HDSP_SPDIF_EMPHASIS("IEC958 Emphasis Bit", 0),
HDSP_SPDIF_NON_AUDIO("IEC958 Non-audio Bit", 0),
/* 'Sample Clock Source' complies with the alsa control naming scheme */ 
HDSP_CLOCK_SOURCE("Sample Clock Source", 0),
HDSP_SYSTEM_CLOCK_MODE("System Clock Mode", 0),
HDSP_PREF_SYNC_REF("Preferred Sync Reference", 0),
HDSP_AUTOSYNC_REF("AutoSync Reference", 0),
HDSP_SPDIF_SAMPLE_RATE("SPDIF Sample Rate", 0),
HDSP_SYSTEM_SAMPLE_RATE("System Sample Rate", 0),
/* 'External Rate' complies with the alsa control naming scheme */
HDSP_AUTOSYNC_SAMPLE_RATE("External Rate", 0),
HDSP_WC_SYNC_CHECK("Word Clock Lock Status", 0),
HDSP_SPDIF_SYNC_CHECK("SPDIF Lock Status", 0),
HDSP_ADATSYNC_SYNC_CHECK("ADAT Sync Lock Status", 0),
HDSP_PASSTHRU("Passthru", 0),
HDSP_LINE_OUT("Line Out", 0),
};

#define HDSP_CONTROLS (sizeof(snd_hdsp_controls)/sizeof(snd_kcontrol_new_t))

#define HDSP_9632_CONTROLS (sizeof(snd_hdsp_9632_controls)/sizeof(snd_kcontrol_new_t))

static snd_kcontrol_new_t snd_hdsp_96xx_aeb = HDSP_AEB("Analog Extension Board", 0);
static snd_kcontrol_new_t snd_hdsp_adat_sync_check = HDSP_ADAT_SYNC_CHECK;

int snd_hdsp_create_controls(snd_card_t *card, hdsp_t *hdsp)
{
	unsigned int idx;
	int err;
	snd_kcontrol_t *kctl;

	for (idx = 0; idx < HDSP_CONTROLS; idx++) {
		if ((err = snd_ctl_add(card, kctl = snd_ctl_new1(&snd_hdsp_controls[idx], hdsp))) < 0) {
			return err;
		}
		if (idx == 1)	/* IEC958 (S/PDIF) Stream */
			hdsp->spdif_ctl = kctl;
	}

	/* ADAT SyncCheck status */
	snd_hdsp_adat_sync_check.name = "ADAT Lock Status";
	snd_hdsp_adat_sync_check.index = 1;
	if ((err = snd_ctl_add (card, kctl = snd_ctl_new1(&snd_hdsp_adat_sync_check, hdsp)))) {
		return err;
	}	
	if (hdsp->io_type == Digiface || hdsp->io_type == H9652) {
		for (idx = 1; idx < 3; ++idx) {
			snd_hdsp_adat_sync_check.index = idx+1;
			if ((err = snd_ctl_add (card, kctl = snd_ctl_new1(&snd_hdsp_adat_sync_check, hdsp)))) {
				return err;
			}
		}
	}
	
	/* DA, AD and Phone gain and XLR breakout cable controls for H9632 cards */
	if (hdsp->io_type == H9632) {
		for (idx = 0; idx < HDSP_9632_CONTROLS; idx++) {
			if ((err = snd_ctl_add(card, kctl = snd_ctl_new1(&snd_hdsp_9632_controls[idx], hdsp))) < 0) {
				return err;
			}
		}
	}

	/* AEB control for H96xx card */
	if (hdsp->io_type == H9632 || hdsp->io_type == H9652) {
		if ((err = snd_ctl_add(card, kctl = snd_ctl_new1(&snd_hdsp_96xx_aeb, hdsp))) < 0) {
				return err;
		}	
	}

	return 0;
}

/*------------------------------------------------------------
   /proc interface 
 ------------------------------------------------------------*/

static void
snd_hdsp_proc_read(snd_info_entry_t *entry, snd_info_buffer_t *buffer)
{
	hdsp_t *hdsp = (hdsp_t *) entry->private_data;
	unsigned int status;
	unsigned int status2;
	char *pref_sync_ref;
	char *autosync_ref;
	char *system_clock_mode;
	char *clock_source;
	int x;

	if (hdsp_check_for_iobox (hdsp)) {
		snd_iprintf(buffer, "No I/O box connected.\nPlease connect one and upload firmware.\n");
		return;
	}

	if (hdsp_check_for_firmware(hdsp)) {
		if (hdsp->state & HDSP_FirmwareCached) {
			if (snd_hdsp_load_firmware_from_cache(hdsp) != 0) {
				snd_iprintf(buffer, "Firmware loading from cache failed, please upload manually.\n");
				return;
			}
		} else {
			snd_iprintf(buffer, "No firmware loaded nor cached, please upload firmware.\n");
			return;
		}
	}
	
	status = hdsp_read(hdsp, HDSP_statusRegister);
	status2 = hdsp_read(hdsp, HDSP_status2Register);

	snd_iprintf(buffer, "%s (Card #%d)\n", hdsp->card_name, hdsp->card->number + 1);
	snd_iprintf(buffer, "Buffers: capture %p playback %p\n",
		    hdsp->capture_buffer, hdsp->playback_buffer);
	snd_iprintf(buffer, "IRQ: %d Registers bus: 0x%lx VM: 0x%lx\n",
		    hdsp->irq, hdsp->port, hdsp->iobase);
	snd_iprintf(buffer, "Control register: 0x%x\n", hdsp->control_register);
	snd_iprintf(buffer, "Control2 register: 0x%x\n", hdsp->control2_register);
	snd_iprintf(buffer, "Status register: 0x%x\n", status);
	snd_iprintf(buffer, "Status2 register: 0x%x\n", status2);
	snd_iprintf(buffer, "FIFO status: %d\n", hdsp_read(hdsp, HDSP_fifoStatus) & 0xff);

	snd_iprintf(buffer, "MIDI1 Output status: 0x%x\n", hdsp_read(hdsp, HDSP_midiStatusOut0));
	snd_iprintf(buffer, "MIDI1 Input status: 0x%x\n", hdsp_read(hdsp, HDSP_midiStatusIn0));
	snd_iprintf(buffer, "MIDI2 Output status: 0x%x\n", hdsp_read(hdsp, HDSP_midiStatusOut1));
	snd_iprintf(buffer, "MIDI2 Input status: 0x%x\n", hdsp_read(hdsp, HDSP_midiStatusIn1));

	snd_iprintf(buffer, "\n");

	x = 1 << (6 + hdsp_decode_latency(hdsp->control_register & HDSP_LatencyMask));

	snd_iprintf(buffer, "Buffer Size (Latency): %d samples (2 periods of %lu bytes)\n", x, (unsigned long) hdsp->period_bytes);
	snd_iprintf(buffer, "Hardware pointer (frames): %ld\n", hdsp_hw_pointer(hdsp));
	snd_iprintf(buffer, "Passthru: %s\n", hdsp->passthru ? "yes" : "no");
	snd_iprintf(buffer, "Line out: %s\n", (hdsp->control_register & HDSP_LineOut) ? "on" : "off");

	snd_iprintf(buffer, "Firmware version: %d\n", (status2&HDSP_version0)|(status2&HDSP_version1)<<1|(status2&HDSP_version2)<<2);

	snd_iprintf(buffer, "\n");


	switch (hdsp_clock_source(hdsp)) {
	case HDSP_CLOCK_SOURCE_AUTOSYNC:
		clock_source = "AutoSync";
		break;
	case HDSP_CLOCK_SOURCE_INTERNAL_32KHZ:
		clock_source = "Internal 32 kHz";
		break;
	case HDSP_CLOCK_SOURCE_INTERNAL_44_1KHZ:
		clock_source = "Internal 44.1 kHz";
		break;
	case HDSP_CLOCK_SOURCE_INTERNAL_48KHZ:
		clock_source = "Internal 48 kHz";
		break;
	case HDSP_CLOCK_SOURCE_INTERNAL_64KHZ:
		clock_source = "Internal 64 kHz";
		break;
	case HDSP_CLOCK_SOURCE_INTERNAL_88_2KHZ:
		clock_source = "Internal 88.2 kHz";
		break;
	case HDSP_CLOCK_SOURCE_INTERNAL_96KHZ:
		clock_source = "Internal 96 kHz";
		break;
	case HDSP_CLOCK_SOURCE_INTERNAL_128KHZ:
		clock_source = "Internal 128 kHz";
		break;
	case HDSP_CLOCK_SOURCE_INTERNAL_176_4KHZ:
		clock_source = "Internal 176.4 kHz";
		break;
		case HDSP_CLOCK_SOURCE_INTERNAL_192KHZ:
		clock_source = "Internal 192 kHz";
		break;	
	default:
		clock_source = "Error";		
	}
	snd_iprintf (buffer, "Sample Clock Source: %s\n", clock_source);
			
	if (hdsp_system_clock_mode(hdsp)) {
		system_clock_mode = "Slave";
	} else {
		system_clock_mode = "Master";
	}
	
	switch (hdsp_pref_sync_ref (hdsp)) {
	case HDSP_SYNC_FROM_WORD:
		pref_sync_ref = "Word Clock";
		break;
	case HDSP_SYNC_FROM_ADAT_SYNC:
		pref_sync_ref = "ADAT Sync";
		break;
	case HDSP_SYNC_FROM_SPDIF:
		pref_sync_ref = "SPDIF";
		break;
	case HDSP_SYNC_FROM_ADAT1:
		pref_sync_ref = "ADAT1";
		break;
	case HDSP_SYNC_FROM_ADAT2:
		pref_sync_ref = "ADAT2";
		break;
	case HDSP_SYNC_FROM_ADAT3:
		pref_sync_ref = "ADAT3";
		break;
	default:
		pref_sync_ref = "Word Clock";
		break;
	}
	snd_iprintf (buffer, "Preferred Sync Reference: %s\n", pref_sync_ref);
	
	switch (hdsp_autosync_ref (hdsp)) {
	case HDSP_AUTOSYNC_FROM_WORD:
		autosync_ref = "Word Clock";
		break;
	case HDSP_AUTOSYNC_FROM_ADAT_SYNC:
		autosync_ref = "ADAT Sync";
		break;
	case HDSP_AUTOSYNC_FROM_SPDIF:
		autosync_ref = "SPDIF";
		break;
	case HDSP_AUTOSYNC_FROM_NONE:
		autosync_ref = "None";
		break;	
	case HDSP_AUTOSYNC_FROM_ADAT1:
		autosync_ref = "ADAT1";
		break;
	case HDSP_AUTOSYNC_FROM_ADAT2:
		autosync_ref = "ADAT2";
		break;
	case HDSP_AUTOSYNC_FROM_ADAT3:
		autosync_ref = "ADAT3";
		break;
	default:
		autosync_ref = "---";
		break;
	}
	snd_iprintf (buffer, "AutoSync Reference: %s\n", autosync_ref);
	
	snd_iprintf (buffer, "AutoSync Frequency: %d\n", hdsp_external_sample_rate(hdsp));
	
	snd_iprintf (buffer, "System Clock Mode: %s\n", system_clock_mode);

	snd_iprintf (buffer, "System Clock Frequency: %d\n", hdsp->system_sample_rate);
		
	snd_iprintf(buffer, "\n");

	switch (hdsp_spdif_in(hdsp)) {
	case HDSP_SPDIFIN_OPTICAL:
		snd_iprintf(buffer, "IEC958 input: Optical\n");
		break;
	case HDSP_SPDIFIN_COAXIAL:
		snd_iprintf(buffer, "IEC958 input: Coaxial\n");
		break;
	case HDSP_SPDIFIN_INTERNAL:
		snd_iprintf(buffer, "IEC958 input: Internal\n");
		break;
	case HDSP_SPDIFIN_AES:
		snd_iprintf(buffer, "IEC958 input: AES\n");
		break;
	default:
		snd_iprintf(buffer, "IEC958 input: ???\n");
		break;
	}
	
	if (hdsp->control_register & HDSP_SPDIFOpticalOut) {
		snd_iprintf(buffer, "IEC958 output: Coaxial & ADAT1\n");
	} else {
		snd_iprintf(buffer, "IEC958 output: Coaxial only\n");
	}

	if (hdsp->control_register & HDSP_SPDIFProfessional) {
		snd_iprintf(buffer, "IEC958 quality: Professional\n");
	} else {
		snd_iprintf(buffer, "IEC958 quality: Consumer\n");
	}

	if (hdsp->control_register & HDSP_SPDIFEmphasis) {
		snd_iprintf(buffer, "IEC958 emphasis: on\n");
	} else {
		snd_iprintf(buffer, "IEC958 emphasis: off\n");
	}

	if (hdsp->control_register & HDSP_SPDIFNonAudio) {
		snd_iprintf(buffer, "IEC958 NonAudio: on\n");
	} else {
		snd_iprintf(buffer, "IEC958 NonAudio: off\n");
	}
	if ((x = hdsp_spdif_sample_rate (hdsp)) != 0) {
		snd_iprintf (buffer, "IEC958 sample rate: %d\n", x);
	} else {
		snd_iprintf (buffer, "IEC958 sample rate: Error flag set\n");
	}

	snd_iprintf(buffer, "\n");

	/* Sync Check */
	x = status & HDSP_Sync0;
	if (status & HDSP_Lock0) {
		snd_iprintf(buffer, "ADAT1: %s\n", x ? "Sync" : "Lock");
	} else {
		snd_iprintf(buffer, "ADAT1: No Lock\n");
	}

	switch (hdsp->io_type) {
	case Digiface:
	case H9652:
		x = status & HDSP_Sync1;
		if (status & HDSP_Lock1) {
			snd_iprintf(buffer, "ADAT2: %s\n", x ? "Sync" : "Lock");
		} else {
			snd_iprintf(buffer, "ADAT2: No Lock\n");
		}
		x = status & HDSP_Sync2;
		if (status & HDSP_Lock2) {
			snd_iprintf(buffer, "ADAT3: %s\n", x ? "Sync" : "Lock");
		} else {
			snd_iprintf(buffer, "ADAT3: No Lock\n");
		}
	default:
		/* relax */
		break;
	}

	x = status & HDSP_SPDIFSync;
	if (status & HDSP_SPDIFErrorFlag) {
		snd_iprintf (buffer, "SPDIF: No Lock\n");
	} else {
		snd_iprintf (buffer, "SPDIF: %s\n", x ? "Sync" : "Lock");
	}
	
	x = status2 & HDSP_wc_sync;
	if (status2 & HDSP_wc_lock) {
		snd_iprintf (buffer, "Word Clock: %s\n", x ? "Sync" : "Lock");
	} else {
		snd_iprintf (buffer, "Word Clock: No Lock\n");
	}
	
	x = status & HDSP_TimecodeSync;
	if (status & HDSP_TimecodeLock) {
		snd_iprintf(buffer, "ADAT Sync: %s\n", x ? "Sync" : "Lock");
	} else {
		snd_iprintf(buffer, "ADAT Sync: No Lock\n");
	}

	snd_iprintf(buffer, "\n");
	
	/* Informations about H9632 specific controls */
	if (hdsp->io_type == H9632) {
		char *tmp;
	
		switch (hdsp_ad_gain(hdsp)) {
		case 0:
			tmp = "-10 dBV";
			break;
		case 1:
			tmp = "+4 dBu";
			break;
		default:
			tmp = "Lo Gain";
			break;
		}
		snd_iprintf(buffer, "AD Gain : %s\n", tmp);

		switch (hdsp_da_gain(hdsp)) {
		case 0:
			tmp = "Hi Gain";
			break;
		case 1:
			tmp = "+4 dBu";
			break;
		default:
			tmp = "-10 dBV";
			break;
		}
		snd_iprintf(buffer, "DA Gain : %s\n", tmp);
		
		switch (hdsp_phone_gain(hdsp)) {
		case 0:
			tmp = "0 dB";
			break;
		case 1:
			tmp = "-6 dB";
			break;
		default:
			tmp = "-12 dB";
			break;
		}
		snd_iprintf(buffer, "Phones Gain : %s\n", tmp);

		snd_iprintf(buffer, "XLR Breakout Cable : %s\n", hdsp_xlr_breakout_cable(hdsp) ? "yes" : "no");	
		
		if (hdsp->control_register & HDSP_AnalogExtensionBoard) {
			snd_iprintf(buffer, "AEB : on (ADAT1 internal)\n");
		} else {
			snd_iprintf(buffer, "AEB : off (ADAT1 external)\n");
		}
		snd_iprintf(buffer, "\n");
	}

}

static void __devinit snd_hdsp_proc_init(hdsp_t *hdsp)
{
	snd_info_entry_t *entry;

	if (! snd_card_proc_new(hdsp->card, "hdsp", &entry))
		snd_info_set_text_ops(entry, hdsp, 1024, snd_hdsp_proc_read);
}

static void snd_hdsp_free_buffers(hdsp_t *hdsp)
{
	if (hdsp->capture_buffer_unaligned) {
		snd_hammerfall_free_buffer(hdsp->pci, HDSP_DMA_AREA_BYTES,
					   hdsp->capture_buffer_unaligned,
					   hdsp->capture_buffer_addr, 1);
	}

	if (hdsp->playback_buffer_unaligned) {
		snd_hammerfall_free_buffer(hdsp->pci, HDSP_DMA_AREA_BYTES,
					   hdsp->playback_buffer_unaligned,
					   hdsp->playback_buffer_addr, 0);
	}
}

static int __devinit snd_hdsp_initialize_memory(hdsp_t *hdsp)
{
	void *pb, *cb;
	dma_addr_t pb_addr, cb_addr;
	unsigned long pb_bus, cb_bus;

	cb = snd_hammerfall_get_buffer(hdsp->pci, HDSP_DMA_AREA_BYTES, &cb_addr, 1);
	pb = snd_hammerfall_get_buffer(hdsp->pci, HDSP_DMA_AREA_BYTES, &pb_addr, 0);

	if (cb == 0 || pb == 0) {
		if (cb) {
			snd_hammerfall_free_buffer(hdsp->pci, HDSP_DMA_AREA_BYTES, cb, cb_addr, 1);
		}
		if (pb) {
			snd_hammerfall_free_buffer(hdsp->pci, HDSP_DMA_AREA_BYTES, pb, pb_addr, 0);
		}

		printk(KERN_ERR "%s: no buffers available\n", hdsp->card_name);
		return -ENOMEM;
	}

	/* save raw addresses for use when freeing memory later */

	hdsp->capture_buffer_unaligned = cb;
	hdsp->playback_buffer_unaligned = pb;
	hdsp->capture_buffer_addr = cb_addr;
	hdsp->playback_buffer_addr = pb_addr;

	/* Align to bus-space 64K boundary */

	cb_bus = (cb_addr + 0xFFFF) & ~0xFFFFl;
	pb_bus = (pb_addr + 0xFFFF) & ~0xFFFFl;

	/* Tell the card where it is */

	hdsp_write(hdsp, HDSP_inputBufferAddress, cb_bus);
	hdsp_write(hdsp, HDSP_outputBufferAddress, pb_bus);

	hdsp->capture_buffer = cb + (cb_bus - cb_addr);
	hdsp->playback_buffer = pb + (pb_bus - pb_addr);

	return 0;
}

static int snd_hdsp_set_defaults(hdsp_t *hdsp)
{
	unsigned int i;

	/* ASSUMPTION: hdsp->lock is either held, or
	   there is no need to hold it (e.g. during module
	   initalization).
	 */

	/* set defaults:

	   SPDIF Input via Coax 
	   Master clock mode
	   maximum latency (7 => 2^7 = 8192 samples, 64Kbyte buffer,
	                    which implies 2 4096 sample, 32Kbyte periods).
           Enable line out.			    
	 */

	hdsp->control_register = HDSP_ClockModeMaster | 
		                 HDSP_SPDIFInputCoaxial | 
		                 hdsp_encode_latency(7) | 
		                 HDSP_LineOut;
	

	hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);

#ifdef SNDRV_BIG_ENDIAN
	hdsp->control2_register = HDSP_BIGENDIAN_MODE;
#else
	hdsp->control2_register = 0;
#endif
	if (hdsp->io_type == H9652) {
	        snd_hdsp_9652_enable_mixer (hdsp);
	} else {
	    hdsp_write (hdsp, HDSP_control2Reg, hdsp->control2_register);
	} 

	hdsp_reset_hw_pointer(hdsp);
	hdsp_compute_period_size(hdsp);

	/* silence everything */
	
	for (i = 0; i < HDSP_MATRIX_MIXER_SIZE; ++i) {
		hdsp->mixer_matrix[i] = MINUS_INFINITY_GAIN;
	}

	for (i = 0; i < ((hdsp->io_type == H9652 || hdsp->io_type == H9632) ? 1352 : HDSP_MATRIX_MIXER_SIZE); ++i) {
		if (hdsp_write_gain (hdsp, i, MINUS_INFINITY_GAIN)) {
			return -EIO;
		}
	}
	
	if ((hdsp->io_type != H9652) && line_outs_monitor[hdsp->dev]) {
		
		int lineouts_base;
		
		snd_printk ("sending all inputs and playback streams to line outs.\n");

		/* route all inputs to the line outs for easy monitoring. send
		   odd numbered channels to right, even to left.
		*/
		if (hdsp->io_type == H9632) {
			/* this is the phones/analog output */
			lineouts_base = 10;
		} else {
			lineouts_base = 26;
		}
		
		for (i = 0; i < hdsp->max_channels; i++) {
			if (i & 1) { 
				if (hdsp_write_gain (hdsp, hdsp_input_to_output_key (hdsp, i, lineouts_base), UNITY_GAIN) ||
				    hdsp_write_gain (hdsp, hdsp_playback_to_output_key (hdsp, i, lineouts_base), UNITY_GAIN)) {
				    return -EIO;
				}    
			} else {
				if (hdsp_write_gain (hdsp, hdsp_input_to_output_key (hdsp, i, lineouts_base+1), UNITY_GAIN) ||
				    hdsp_write_gain (hdsp, hdsp_playback_to_output_key (hdsp, i, lineouts_base+1), UNITY_GAIN)) {
				    
				    return -EIO;
				}
			}
		}
	}

	hdsp->passthru = 0;

	/* H9632 specific defaults */
	if (hdsp->io_type == H9632) {
		hdsp->control_register |= (HDSP_DAGainPlus4dBu | HDSP_ADGainPlus4dBu | HDSP_PhoneGain0dB);
		hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
	}

	/* set a default rate so that the channel map is set up.
	 */

	hdsp_set_rate(hdsp, 48000, 1);

	return 0;
}

void hdsp_midi_tasklet(unsigned long arg)
{
	hdsp_t *hdsp = (hdsp_t *)arg;
	
	if (hdsp->midi[0].pending) {
		snd_hdsp_midi_input_read (&hdsp->midi[0]);
	}
	if (hdsp->midi[1].pending) {
		snd_hdsp_midi_input_read (&hdsp->midi[1]);
	}
} 

static irqreturn_t snd_hdsp_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	hdsp_t *hdsp = (hdsp_t *) dev_id;
	unsigned int status;
	int audio;
	int midi0;
	int midi1;
	unsigned int midi0status;
	unsigned int midi1status;
	int schedule = 0;
	
	status = hdsp_read(hdsp, HDSP_statusRegister);

	audio = status & HDSP_audioIRQPending;
	midi0 = status & HDSP_midi0IRQPending;
	midi1 = status & HDSP_midi1IRQPending;

	if (!audio && !midi0 && !midi1) {
		return IRQ_NONE;
	}

	hdsp_write(hdsp, HDSP_interruptConfirmation, 0);

	midi0status = hdsp_read (hdsp, HDSP_midiStatusIn0) & 0xff;
	midi1status = hdsp_read (hdsp, HDSP_midiStatusIn1) & 0xff;
	
	if (audio) {
		if (hdsp->capture_substream) {
			snd_pcm_period_elapsed(hdsp->pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream);
		}
		
		if (hdsp->playback_substream) {
			snd_pcm_period_elapsed(hdsp->pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream);
		}
	}
	
	if (midi0 && midi0status) {
		/* we disable interrupts for this input until processing is done */
		hdsp->control_register &= ~HDSP_Midi0InterruptEnable;
		hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
		hdsp->midi[0].pending = 1;
		schedule = 1;
	}
	if (midi1 && midi1status) {
		/* we disable interrupts for this input until processing is done */
		hdsp->control_register &= ~HDSP_Midi1InterruptEnable;
		hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register);
		hdsp->midi[1].pending = 1;
		schedule = 1;
	}
	if (schedule)
	    tasklet_hi_schedule(&hdsp->midi_tasklet);
	return IRQ_HANDLED;
}

static snd_pcm_uframes_t snd_hdsp_hw_pointer(snd_pcm_substream_t *substream)
{
	hdsp_t *hdsp = _snd_pcm_substream_chip(substream);
	return hdsp_hw_pointer(hdsp);
}

static char *hdsp_channel_buffer_location(hdsp_t *hdsp,
					     int stream,
					     int channel)

{
	int mapped_channel;

        snd_assert(channel >= 0 && channel < hdsp->max_channels, return NULL);
        
	if ((mapped_channel = hdsp->channel_map[channel]) < 0) {
		return NULL;
	}
	
	if (stream == SNDRV_PCM_STREAM_CAPTURE) {
		return hdsp->capture_buffer + (mapped_channel * HDSP_CHANNEL_BUFFER_BYTES);
	} else {
		return hdsp->playback_buffer + (mapped_channel * HDSP_CHANNEL_BUFFER_BYTES);
	}
}

static int snd_hdsp_playback_copy(snd_pcm_substream_t *substream, int channel,
				  snd_pcm_uframes_t pos, void __user *src, snd_pcm_uframes_t count)
{
	hdsp_t *hdsp = _snd_pcm_substream_chip(substream);
	char *channel_buf;

	snd_assert(pos + count <= HDSP_CHANNEL_BUFFER_BYTES / 4, return -EINVAL);

	channel_buf = hdsp_channel_buffer_location (hdsp, substream->pstr->stream, channel);
	snd_assert(channel_buf != NULL, return -EIO);
	if (copy_from_user(channel_buf + pos * 4, src, count * 4))
		return -EFAULT;
	return count;
}

static int snd_hdsp_capture_copy(snd_pcm_substream_t *substream, int channel,
				 snd_pcm_uframes_t pos, void __user *dst, snd_pcm_uframes_t count)
{
	hdsp_t *hdsp = _snd_pcm_substream_chip(substream);
	char *channel_buf;

	snd_assert(pos + count <= HDSP_CHANNEL_BUFFER_BYTES / 4, return -EINVAL);

	channel_buf = hdsp_channel_buffer_location (hdsp, substream->pstr->stream, channel);
	snd_assert(channel_buf != NULL, return -EIO);
	if (copy_to_user(dst, channel_buf + pos * 4, count * 4))
		return -EFAULT;
	return count;
}

static int snd_hdsp_hw_silence(snd_pcm_substream_t *substream, int channel,
				  snd_pcm_uframes_t pos, snd_pcm_uframes_t count)
{
	hdsp_t *hdsp = _snd_pcm_substream_chip(substream);
	char *channel_buf;

	channel_buf = hdsp_channel_buffer_location (hdsp, substream->pstr->stream, channel);
	snd_assert(channel_buf != NULL, return -EIO);
	memset(channel_buf + pos * 4, 0, count * 4);
	return count;
}

static int snd_hdsp_reset(snd_pcm_substream_t *substream)
{
	snd_pcm_runtime_t *runtime = substream->runtime;
	hdsp_t *hdsp = _snd_pcm_substream_chip(substream);
	snd_pcm_substream_t *other;
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		other = hdsp->capture_substream;
	else
		other = hdsp->playback_substream;
	if (hdsp->running)
		runtime->status->hw_ptr = hdsp_hw_pointer(hdsp);
	else
		runtime->status->hw_ptr = 0;
	if (other) {
		struct list_head *pos;
		snd_pcm_substream_t *s;
		snd_pcm_runtime_t *oruntime = other->runtime;
		snd_pcm_group_for_each(pos, substream) {
			s = snd_pcm_group_substream_entry(pos);
			if (s == other) {
				oruntime->status->hw_ptr = runtime->status->hw_ptr;
				break;
			}
		}
	}
	return 0;
}

static int snd_hdsp_hw_params(snd_pcm_substream_t *substream,
				 snd_pcm_hw_params_t *params)
{
	hdsp_t *hdsp = _snd_pcm_substream_chip(substream);
	int err;
	pid_t this_pid;
	pid_t other_pid;

	if (hdsp_check_for_iobox (hdsp)) {
		return -EIO;
	}

	if (hdsp_check_for_firmware(hdsp)) {
		if (hdsp->state & HDSP_FirmwareCached) {
			if (snd_hdsp_load_firmware_from_cache(hdsp) != 0) {
				snd_printk("Firmware loading from cache failed, please upload manually.\n");
			}
		} else {
			snd_printk("No firmware loaded nor cached, please upload firmware.\n");
		}
		return -EIO;
	}

	spin_lock_irq(&hdsp->lock);

	if (substream->pstr->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		hdsp->control_register &= ~(HDSP_SPDIFProfessional | HDSP_SPDIFNonAudio | HDSP_SPDIFEmphasis);
		hdsp_write(hdsp, HDSP_controlRegister, hdsp->control_register |= hdsp->creg_spdif_stream);
		this_pid = hdsp->playback_pid;
		other_pid = hdsp->capture_pid;
	} else {
		this_pid = hdsp->capture_pid;
		other_pid = hdsp->playback_pid;
	}

	if ((other_pid > 0) && (this_pid != other_pid)) {

		/* The other stream is open, and not by the same
		   task as this one. Make sure that the parameters
		   that matter are the same.
		 */

		if (params_rate(params) != hdsp->system_sample_rate) {
			spin_unlock_irq(&hdsp->lock);
			_snd_pcm_hw_param_setempty(params, SNDRV_PCM_HW_PARAM_RATE);
			return -EBUSY;
		}

		if (params_period_size(params) != hdsp->period_bytes / 4) {
			spin_unlock_irq(&hdsp->lock);
			_snd_pcm_hw_param_setempty(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
			return -EBUSY;
		}

		/* We're fine. */

		spin_unlock_irq(&hdsp->lock);
 		return 0;

	} else {
		spin_unlock_irq(&hdsp->lock);
	}

	/* how to make sure that the rate matches an externally-set one ?
	 */

	spin_lock_irq(&hdsp->lock);
	if ((err = hdsp_set_rate(hdsp, params_rate(params), 0)) < 0) {
		spin_unlock_irq(&hdsp->lock);
		_snd_pcm_hw_param_setempty(params, SNDRV_PCM_HW_PARAM_RATE);
		return err;
	} else {
		spin_unlock_irq(&hdsp->lock);
	}

	if ((err = hdsp_set_interrupt_interval(hdsp, params_period_size(params))) < 0) {
		_snd_pcm_hw_param_setempty(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
		return err;
	}

	return 0;
}

static int snd_hdsp_channel_info(snd_pcm_substream_t *substream,
				    snd_pcm_channel_info_t *info)
{
	hdsp_t *hdsp = _snd_pcm_substream_chip(substream);
	int mapped_channel;

	snd_assert(info->channel < hdsp->max_channels, return -EINVAL);

	if ((mapped_channel = hdsp->channel_map[info->channel]) < 0) {
		return -EINVAL;
	}

	info->offset = mapped_channel * HDSP_CHANNEL_BUFFER_BYTES;
	info->first = 0;
	info->step = 32;
	return 0;
}

static int snd_hdsp_ioctl(snd_pcm_substream_t *substream,
			     unsigned int cmd, void *arg)
{
	switch (cmd) {
	case SNDRV_PCM_IOCTL1_RESET:
	{
		return snd_hdsp_reset(substream);
	}
	case SNDRV_PCM_IOCTL1_CHANNEL_INFO:
	{
		snd_pcm_channel_info_t *info = arg;
		return snd_hdsp_channel_info(substream, info);
	}
	default:
		break;
	}

	return snd_pcm_lib_ioctl(substream, cmd, arg);
}

static int snd_hdsp_trigger(snd_pcm_substream_t *substream, int cmd)
{
	hdsp_t *hdsp = _snd_pcm_substream_chip(substream);
	snd_pcm_substream_t *other;
	int running;
	
	if (hdsp_check_for_iobox (hdsp)) {
		return -EIO;
	}

	if (hdsp_check_for_firmware(hdsp)) {
		if (hdsp->state & HDSP_FirmwareCached) {
			if (snd_hdsp_load_firmware_from_cache(hdsp) != 0) {
				snd_printk("Firmware loading from cache failed, please upload manually.\n");
			}
		} else {
			snd_printk("No firmware loaded nor cached, please upload firmware.\n");
		}
		return -EIO;
	}

	spin_lock(&hdsp->lock);
	running = hdsp->running;
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		running |= 1 << substream->stream;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		running &= ~(1 << substream->stream);
		break;
	default:
		snd_BUG();
		spin_unlock(&hdsp->lock);
		return -EINVAL;
	}
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		other = hdsp->capture_substream;
	else
		other = hdsp->playback_substream;

	if (other) {
		struct list_head *pos;
		snd_pcm_substream_t *s;
		snd_pcm_group_for_each(pos, substream) {
			s = snd_pcm_group_substream_entry(pos);
			if (s == other) {
				snd_pcm_trigger_done(s, substream);
				if (cmd == SNDRV_PCM_TRIGGER_START)
					running |= 1 << s->stream;
				else
					running &= ~(1 << s->stream);
				goto _ok;
			}
		}
		if (cmd == SNDRV_PCM_TRIGGER_START) {
			if (!(running & (1 << SNDRV_PCM_STREAM_PLAYBACK)) &&
			    substream->stream == SNDRV_PCM_STREAM_CAPTURE)
				hdsp_silence_playback(hdsp);
		} else {
			if (running &&
			    substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
				hdsp_silence_playback(hdsp);
		}
	} else {
		if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
				hdsp_silence_playback(hdsp);
	}
 _ok:
	snd_pcm_trigger_done(substream, substream);
	if (!hdsp->running && running)
		hdsp_start_audio(hdsp);
	else if (hdsp->running && !running)
		hdsp_stop_audio(hdsp);
	hdsp->running = running;
	spin_unlock(&hdsp->lock);

	return 0;
}

static int snd_hdsp_prepare(snd_pcm_substream_t *substream)
{
	hdsp_t *hdsp = _snd_pcm_substream_chip(substream);
	int result = 0;

	if (hdsp_check_for_iobox (hdsp)) {
		return -EIO;
	}

	if (hdsp_check_for_firmware(hdsp)) {
		if (hdsp->state & HDSP_FirmwareCached) {
			if (snd_hdsp_load_firmware_from_cache(hdsp) != 0) {
				snd_printk("Firmware loading from cache failed, please upload manually.\n");
			}
		} else {
			snd_printk("No firmware loaded nor cached, please upload firmware.\n");
		}
		return -EIO;
	}

	spin_lock(&hdsp->lock);
	if (!hdsp->running)
		hdsp_reset_hw_pointer(hdsp);
	spin_unlock(&hdsp->lock);
	return result;
}

static snd_pcm_hardware_t snd_hdsp_playback_subinfo =
{
	.info =			(SNDRV_PCM_INFO_MMAP |
				 SNDRV_PCM_INFO_MMAP_VALID |
				 SNDRV_PCM_INFO_NONINTERLEAVED |
				 SNDRV_PCM_INFO_SYNC_START |
				 SNDRV_PCM_INFO_DOUBLE),
	.formats =		SNDRV_PCM_FMTBIT_S32_LE,
	.rates =		(SNDRV_PCM_RATE_32000 |
				 SNDRV_PCM_RATE_44100 | 
				 SNDRV_PCM_RATE_48000 | 
				 SNDRV_PCM_RATE_64000 | 
				 SNDRV_PCM_RATE_88200 | 
				 SNDRV_PCM_RATE_96000),
	.rate_min =		32000,
	.rate_max =		96000,
	.channels_min =		14,
	.channels_max =		HDSP_MAX_CHANNELS,
	.buffer_bytes_max =	HDSP_CHANNEL_BUFFER_BYTES * HDSP_MAX_CHANNELS,
	.period_bytes_min =	(64 * 4) * 10,
	.period_bytes_max =	(8192 * 4) * HDSP_MAX_CHANNELS,
	.periods_min =		2,
	.periods_max =		2,
	.fifo_size =		0
};

static snd_pcm_hardware_t snd_hdsp_capture_subinfo =
{
	.info =			(SNDRV_PCM_INFO_MMAP |
				 SNDRV_PCM_INFO_MMAP_VALID |
				 SNDRV_PCM_INFO_NONINTERLEAVED |
				 SNDRV_PCM_INFO_SYNC_START),
	.formats =		SNDRV_PCM_FMTBIT_S32_LE,
	.rates =		(SNDRV_PCM_RATE_32000 |
				 SNDRV_PCM_RATE_44100 | 
				 SNDRV_PCM_RATE_48000 | 
				 SNDRV_PCM_RATE_64000 | 
				 SNDRV_PCM_RATE_88200 | 
				 SNDRV_PCM_RATE_96000),
	.rate_min =		32000,
	.rate_max =		96000,
	.channels_min =		14,
	.channels_max =		HDSP_MAX_CHANNELS,
	.buffer_bytes_max =	HDSP_CHANNEL_BUFFER_BYTES * HDSP_MAX_CHANNELS,
	.period_bytes_min =	(64 * 4) * 10,
	.period_bytes_max =	(8192 * 4) * HDSP_MAX_CHANNELS,
	.periods_min =		2,
	.periods_max =		2,
	.fifo_size =		0
};

static unsigned int hdsp_period_sizes[] = { 64, 128, 256, 512, 1024, 2048, 4096, 8192 };

#define HDSP_PERIOD_SIZES sizeof(hdsp_period_sizes) / sizeof(hdsp_period_sizes[0])

static snd_pcm_hw_constraint_list_t hdsp_hw_constraints_period_sizes = {
	.count = HDSP_PERIOD_SIZES,
	.list = hdsp_period_sizes,
	.mask = 0
};

static unsigned int hdsp_9632_sample_rates[] = { 32000, 44100, 48000, 64000, 88200, 96000, 128000, 176400, 192000 };

#define HDSP_9632_SAMPLE_RATES sizeof(hdsp_9632_sample_rates) / sizeof(hdsp_9632_sample_rates[0])

static snd_pcm_hw_constraint_list_t hdsp_hw_constraints_9632_sample_rates = {
	.count = HDSP_9632_SAMPLE_RATES,
	.list = hdsp_9632_sample_rates,
	.mask = 0
};

static int snd_hdsp_hw_rule_in_channels(snd_pcm_hw_params_t *params,
					snd_pcm_hw_rule_t *rule)
{
	hdsp_t *hdsp = rule->private;
	snd_interval_t *c = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	if (hdsp->io_type == H9632) {
		unsigned int list[3];
		list[0] = hdsp->qs_in_channels;
		list[1] = hdsp->ds_in_channels;
		list[2] = hdsp->ss_in_channels;
		return snd_interval_list(c, 3, list, 0);
	} else {
		unsigned int list[2];
		list[0] = hdsp->ds_in_channels;
		list[1] = hdsp->ss_in_channels;
		return snd_interval_list(c, 2, list, 0);
	}
}

static int snd_hdsp_hw_rule_out_channels(snd_pcm_hw_params_t *params,
					snd_pcm_hw_rule_t *rule)
{
	unsigned int list[3];
	hdsp_t *hdsp = rule->private;
	snd_interval_t *c = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	if (hdsp->io_type == H9632) {
		list[0] = hdsp->qs_out_channels;
		list[1] = hdsp->ds_out_channels;
		list[2] = hdsp->ss_out_channels;
		return snd_interval_list(c, 3, list, 0);
	} else {
		list[0] = hdsp->ds_out_channels;
		list[1] = hdsp->ss_out_channels;
	}
	return snd_interval_list(c, 2, list, 0);
}

static int snd_hdsp_hw_rule_in_channels_rate(snd_pcm_hw_params_t *params,
					     snd_pcm_hw_rule_t *rule)
{
	hdsp_t *hdsp = rule->private;
	snd_interval_t *c = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	snd_interval_t *r = hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	if (r->min > 96000 && hdsp->io_type == H9632) {
		snd_interval_t t = {
			.min = hdsp->qs_in_channels,
			.max = hdsp->qs_in_channels,
			.integer = 1,
		};
		return snd_interval_refine(c, &t);	
	} else if (r->min > 48000 && r->max <= 96000) {
		snd_interval_t t = {
			.min = hdsp->ds_in_channels,
			.max = hdsp->ds_in_channels,
			.integer = 1,
		};
		return snd_interval_refine(c, &t);
	} else if (r->max < 64000) {
		snd_interval_t t = {
			.min = hdsp->ss_in_channels,
			.max = hdsp->ss_in_channels,
			.integer = 1,
		};
		return snd_interval_refine(c, &t);
	}
	return 0;
}

static int snd_hdsp_hw_rule_out_channels_rate(snd_pcm_hw_params_t *params,
					     snd_pcm_hw_rule_t *rule)
{
	hdsp_t *hdsp = rule->private;
	snd_interval_t *c = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	snd_interval_t *r = hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	if (r->min > 96000 && hdsp->io_type == H9632) {
		snd_interval_t t = {
			.min = hdsp->qs_out_channels,
			.max = hdsp->qs_out_channels,
			.integer = 1,
		};
		return snd_interval_refine(c, &t);	
	} else if (r->min > 48000 && r->max <= 96000) {
		snd_interval_t t = {
			.min = hdsp->ds_out_channels,
			.max = hdsp->ds_out_channels,
			.integer = 1,
		};
		return snd_interval_refine(c, &t);
	} else if (r->max < 64000) {
		snd_interval_t t = {
			.min = hdsp->ss_out_channels,
			.max = hdsp->ss_out_channels,
			.integer = 1,
		};
		return snd_interval_refine(c, &t);
	}
	return 0;
}

static int snd_hdsp_hw_rule_rate_out_channels(snd_pcm_hw_params_t *params,
					     snd_pcm_hw_rule_t *rule)
{
	hdsp_t *hdsp = rule->private;
	snd_interval_t *c = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	snd_interval_t *r = hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	if (c->min >= hdsp->ss_out_channels) {
		snd_interval_t t = {
			.min = 32000,
			.max = 48000,
			.integer = 1,
		};
		return snd_interval_refine(r, &t);
	} else if (c->max <= hdsp->qs_out_channels && hdsp->io_type == H9632) {
		snd_interval_t t = {
			.min = 128000,
			.max = 192000,
			.integer = 1,
		};
		return snd_interval_refine(r, &t);
	} else if (c->max <= hdsp->ds_out_channels) {
		snd_interval_t t = {
			.min = 64000,
			.max = 96000,
			.integer = 1,
		};
		return snd_interval_refine(r, &t);
	}
	return 0;
}

static int snd_hdsp_hw_rule_rate_in_channels(snd_pcm_hw_params_t *params,
					     snd_pcm_hw_rule_t *rule)
{
	hdsp_t *hdsp = rule->private;
	snd_interval_t *c = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	snd_interval_t *r = hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	if (c->min >= hdsp->ss_in_channels) {
		snd_interval_t t = {
			.min = 32000,
			.max = 48000,
			.integer = 1,
		};
		return snd_interval_refine(r, &t);
	} else if (c->max <= hdsp->qs_in_channels && hdsp->io_type == H9632) {
		snd_interval_t t = {
			.min = 128000,
			.max = 192000,
			.integer = 1,
		};
		return snd_interval_refine(r, &t);
	} else if (c->max <= hdsp->ds_in_channels) {
		snd_interval_t t = {
			.min = 64000,
			.max = 96000,
			.integer = 1,
		};
		return snd_interval_refine(r, &t);
	}
	return 0;
}

static int snd_hdsp_playback_open(snd_pcm_substream_t *substream)
{
	hdsp_t *hdsp = _snd_pcm_substream_chip(substream);
	unsigned long flags;
	snd_pcm_runtime_t *runtime = substream->runtime;

	if (hdsp_check_for_iobox (hdsp)) {
		return -EIO;
	}

	if (hdsp_check_for_firmware(hdsp)) {
		if (hdsp->state & HDSP_FirmwareCached) {
			if (snd_hdsp_load_firmware_from_cache(hdsp) != 0) {
				snd_printk("Firmware loading from cache failed, please upload manually.\n");
			}
		} else {
			snd_printk("No firmware loaded nor cached, please upload firmware.\n");
		}
		return -EIO;
	}

	spin_lock_irqsave(&hdsp->lock, flags);

	snd_pcm_set_sync(substream);

        runtime->hw = snd_hdsp_playback_subinfo;
	runtime->dma_area = hdsp->playback_buffer;
	runtime->dma_bytes = HDSP_DMA_AREA_BYTES;

	if (hdsp->capture_substream == NULL) {
		hdsp_stop_audio(hdsp);
		hdsp_set_thru(hdsp, -1, 0);
	}

	hdsp->playback_pid = current->pid;
	hdsp->playback_substream = substream;

	spin_unlock_irqrestore(&hdsp->lock, flags);

	snd_pcm_hw_constraint_msbits(runtime, 0, 32, 24);
	snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, &hdsp_hw_constraints_period_sizes);
	if (hdsp->io_type == H9632) {
		runtime->hw.channels_min = hdsp->qs_out_channels;
		runtime->hw.channels_max = hdsp->ss_out_channels;
		runtime->hw.rate_max = 192000;
		runtime->hw.rates = SNDRV_PCM_RATE_KNOT;
		snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_RATE, &hdsp_hw_constraints_9632_sample_rates);
	}
	
	snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS,
			     snd_hdsp_hw_rule_out_channels, hdsp,
			     SNDRV_PCM_HW_PARAM_CHANNELS, -1);
	snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS,
			     snd_hdsp_hw_rule_out_channels_rate, hdsp,
			     SNDRV_PCM_HW_PARAM_RATE, -1);
	snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
			     snd_hdsp_hw_rule_rate_out_channels, hdsp,
			     SNDRV_PCM_HW_PARAM_CHANNELS, -1);

	hdsp->creg_spdif_stream = hdsp->creg_spdif;
	hdsp->spdif_ctl->vd[0].access &= ~SNDRV_CTL_ELEM_ACCESS_INACTIVE;
	snd_ctl_notify(hdsp->card, SNDRV_CTL_EVENT_MASK_VALUE |
		       SNDRV_CTL_EVENT_MASK_INFO, &hdsp->spdif_ctl->id);
	return 0;
}

static int snd_hdsp_playback_release(snd_pcm_substream_t *substream)
{
	hdsp_t *hdsp = _snd_pcm_substream_chip(substream);
	unsigned long flags;

	spin_lock_irqsave(&hdsp->lock, flags);

	hdsp->playback_pid = -1;
	hdsp->playback_substream = NULL;

	spin_unlock_irqrestore(&hdsp->lock, flags);

	hdsp->spdif_ctl->vd[0].access |= SNDRV_CTL_ELEM_ACCESS_INACTIVE;
	snd_ctl_notify(hdsp->card, SNDRV_CTL_EVENT_MASK_VALUE |
		       SNDRV_CTL_EVENT_MASK_INFO, &hdsp->spdif_ctl->id);
	return 0;
}


static int snd_hdsp_capture_open(snd_pcm_substream_t *substream)
{
	hdsp_t *hdsp = _snd_pcm_substream_chip(substream);
	unsigned long flags;
	snd_pcm_runtime_t *runtime = substream->runtime;

	if (hdsp_check_for_iobox (hdsp)) {
		return -EIO;
	}

	if (hdsp_check_for_firmware(hdsp)) {
		if (hdsp->state & HDSP_FirmwareCached) {
			if (snd_hdsp_load_firmware_from_cache(hdsp) != 0) {
				snd_printk("Firmware loading from cache failed, please upload manually.\n");
			}
		} else {
			snd_printk("No firmware loaded nor cached, please upload firmware.\n");
		}
		return -EIO;
	}

	spin_lock_irqsave(&hdsp->lock, flags);

	snd_pcm_set_sync(substream);

	runtime->hw = snd_hdsp_capture_subinfo;
	runtime->dma_area = hdsp->capture_buffer;
	runtime->dma_bytes = HDSP_DMA_AREA_BYTES;

	if (hdsp->playback_substream == NULL) {
		hdsp_stop_audio(hdsp);
		hdsp_set_thru(hdsp, -1, 0);
	}

	hdsp->capture_pid = current->pid;
	hdsp->capture_substream = substream;

	spin_unlock_irqrestore(&hdsp->lock, flags);

	snd_pcm_hw_constraint_msbits(runtime, 0, 32, 24);
	snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, &hdsp_hw_constraints_period_sizes);
	if (hdsp->io_type == H9632) {
		runtime->hw.channels_min = hdsp->qs_in_channels;
		runtime->hw.channels_max = hdsp->ss_in_channels;
		runtime->hw.rate_max = 192000;
		runtime->hw.rates = SNDRV_PCM_RATE_KNOT;
		snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_RATE, &hdsp_hw_constraints_9632_sample_rates);
	}
	snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS,
			     snd_hdsp_hw_rule_in_channels, hdsp,
			     SNDRV_PCM_HW_PARAM_CHANNELS, -1);
	snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS,
			     snd_hdsp_hw_rule_in_channels_rate, hdsp,
			     SNDRV_PCM_HW_PARAM_RATE, -1);
	snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
			     snd_hdsp_hw_rule_rate_in_channels, hdsp,
			     SNDRV_PCM_HW_PARAM_CHANNELS, -1);
	return 0;
}

static int snd_hdsp_capture_release(snd_pcm_substream_t *substream)
{
	hdsp_t *hdsp = _snd_pcm_substream_chip(substream);
	unsigned long flags;

	spin_lock_irqsave(&hdsp->lock, flags);

	hdsp->capture_pid = -1;
	hdsp->capture_substream = NULL;

	spin_unlock_irqrestore(&hdsp->lock, flags);
	return 0;
}

static int snd_hdsp_hwdep_dummy_op(snd_hwdep_t *hw, struct file *file)
{
	/* we have nothing to initialize but the call is required */
	return 0;
}


static int snd_hdsp_hwdep_ioctl(snd_hwdep_t *hw, struct file *file, unsigned int cmd, unsigned long arg)
{
	hdsp_t *hdsp = (hdsp_t *)hw->private_data;	
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case SNDRV_HDSP_IOCTL_GET_PEAK_RMS: {
		hdsp_peak_rms_t __user *peak_rms;
		int i;
		
		if (hdsp->io_type == H9652) {
			unsigned long rms_low, rms_high;
			int doublespeed = 0;
			if (hdsp_read (hdsp, HDSP_statusRegister) & HDSP_DoubleSpeedStatus)
				doublespeed = 1;
			peak_rms = (hdsp_peak_rms_t __user *)arg;
			for (i = 0; i < 26; ++i) {
				if (!(doublespeed && (i & 4))) {
					if (copy_to_user_fromio((void __user *)peak_rms->input_peaks+i*4, hdsp->iobase+HDSP_9652_peakBase-i*4, 4) != 0)
						return -EFAULT;
					if (copy_to_user_fromio((void __user *)peak_rms->playback_peaks+i*4, hdsp->iobase+HDSP_9652_peakBase-(doublespeed ? 14 : 26)*4-i*4, 4) != 0)
						return -EFAULT;
					if (copy_to_user_fromio((void __user *)peak_rms->output_peaks+i*4, hdsp->iobase+HDSP_9652_peakBase-2*(doublespeed ? 14 : 26)*4-i*4, 4) != 0)
						return -EFAULT;
					rms_low = *(u32 *)(hdsp->iobase+HDSP_9652_rmsBase+i*8) & 0xFFFFFF00;
					rms_high = *(u32 *)(hdsp->iobase+HDSP_9652_rmsBase+i*8+4) & 0xFFFFFF00;
					rms_high += (rms_low >> 24);
					rms_low <<= 8;
					if (copy_to_user((void __user *)peak_rms->input_rms+i*8, &rms_low, 4) != 0)
						return -EFAULT;
					if (copy_to_user((void __user *)peak_rms->input_rms+i*8+4, &rms_high, 4) != 0)
						return -EFAULT;					
					rms_low = *(u32 *)(hdsp->iobase+HDSP_9652_rmsBase+(doublespeed ? 14 : 26)*8+i*8) & 0xFFFFFF00;
					rms_high = *(u32 *)(hdsp->iobase+HDSP_9652_rmsBase+(doublespeed ? 14 : 26)*8+i*8+4) & 0xFFFFFF00;
					rms_high += (rms_low >> 24);
					rms_low <<= 8;
					if (copy_to_user((void __user *)peak_rms->playback_rms+i*8, &rms_low, 4) != 0)
						return -EFAULT;
					if (copy_to_user((void __user *)peak_rms->playback_rms+i*8+4, &rms_high, 4) != 0)
						return -EFAULT;					
					rms_low = *(u32 *)(hdsp->iobase+HDSP_9652_rmsBase+2*(doublespeed ? 14 : 26)*8+i*8) & 0xFFFFFF00;
					rms_high = *(u32 *)(hdsp->iobase+HDSP_9652_rmsBase+2*(doublespeed ? 14 : 26)*8+i*8+4) & 0xFFFFFF00;
					rms_high += (rms_low >> 24);
					rms_low <<= 8;
					if (copy_to_user((void __user *)peak_rms->output_rms+i*8, &rms_low, 4) != 0)
						return -EFAULT;
					if (copy_to_user((void __user *)peak_rms->output_rms+i*8+4, &rms_high, 4) != 0)
						return -EFAULT;					
				}
			}
			return 0;
		}
		if (hdsp->io_type == H9632) {
			int j;
			hdsp_9632_meters_t *m;
			int doublespeed = 0;
			if (hdsp_read (hdsp, HDSP_statusRegister) & HDSP_DoubleSpeedStatus)
				doublespeed = 1;
			m = (hdsp_9632_meters_t *)(hdsp->iobase+HDSP_9632_metersBase);
			peak_rms = (hdsp_peak_rms_t __user *)arg;
			for (i = 0, j = 0; i < 16; ++i, ++j) {
				if (copy_to_user((void __user *)peak_rms->input_peaks+i*4, &(m->input_peak[j]), 4) != 0)
					return -EFAULT;
				if (copy_to_user((void __user *)peak_rms->playback_peaks+i*4, &(m->playback_peak[j]), 4) != 0)
					return -EFAULT;
				if (copy_to_user((void __user *)peak_rms->output_peaks+i*4, &(m->output_peak[j]), 4) != 0)
					return -EFAULT;
				if (copy_to_user((void __user *)peak_rms->input_rms+i*8, &(m->input_rms_low[j]), 4) != 0)
					return -EFAULT;
				if (copy_to_user((void __user *)peak_rms->playback_rms+i*8, &(m->playback_rms_low[j]), 4) != 0)
					return -EFAULT;
				if (copy_to_user((void __user *)peak_rms->output_rms+i*8, &(m->output_rms_low[j]), 4) != 0)
					return -EFAULT;
				if (copy_to_user((void __user *)peak_rms->input_rms+i*8+4, &(m->input_rms_high[j]), 4) != 0)
					return -EFAULT;
				if (copy_to_user((void __user *)peak_rms->playback_rms+i*8+4, &(m->playback_rms_high[j]), 4) != 0)
					return -EFAULT;
				if (copy_to_user((void __user *)peak_rms->output_rms+i*8+4, &(m->output_rms_high[j]), 4) != 0)
					return -EFAULT;
				if (doublespeed && i == 3) i += 4;
			}
			return 0;
		}
		if (!(hdsp->state & HDSP_FirmwareLoaded)) {
			snd_printk("firmware needs to be uploaded to the card.\n");	
			return -EINVAL;
		}
		peak_rms = (hdsp_peak_rms_t __user *)arg;
		for (i = 0; i < 26; ++i) {
		    if (copy_to_user((void __user *)peak_rms->playback_peaks+i*4, (void *)hdsp->iobase+HDSP_playbackPeakLevel+i*4, 4) != 0)
			    return -EFAULT;
		    if (copy_to_user((void __user *)peak_rms->input_peaks+i*4, (void *)hdsp->iobase+HDSP_inputPeakLevel+i*4, 4) != 0)
			    return -EFAULT;
		}
		for (i = 0; i < 26; ++i) {
			if (copy_to_user((void __user *)peak_rms->playback_rms+i*8+4, (void *)hdsp->iobase+HDSP_playbackRmsLevel+i*8, 4) != 0)
				return -EFAULT;
			if (copy_to_user((void __user *)peak_rms->playback_rms+i*8, (void *)hdsp->iobase+HDSP_playbackRmsLevel+i*8+4, 4) != 0)
				return -EFAULT;
			if (copy_to_user((void __user *)peak_rms->input_rms+i*8+4, (void *)hdsp->iobase+HDSP_inputRmsLevel+i*8, 4) != 0)
				return -EFAULT;
			if (copy_to_user((void __user *)peak_rms->input_rms+i*8, (void *)hdsp->iobase+HDSP_inputRmsLevel+i*8+4, 4) != 0)
				return -EFAULT;
		}
		for (i = 0; i < 28; ++i) {
		    if (copy_to_user((void __user *)peak_rms->output_peaks+i*4, (void *)hdsp->iobase+HDSP_outputPeakLevel+i*4, 4) != 0)
			    return -EFAULT;
		}
		break;
	}
	case SNDRV_HDSP_IOCTL_GET_CONFIG_INFO: {
		hdsp_config_info_t info;
		unsigned long flags;
		int i;
		
		if (!(hdsp->state & HDSP_FirmwareLoaded)) {
			snd_printk("Firmware needs to be uploaded to the card.\n");	
			return -EINVAL;
		}
		spin_lock_irqsave(&hdsp->lock, flags);
		info.pref_sync_ref = (unsigned char)hdsp_pref_sync_ref(hdsp);
		info.wordclock_sync_check = (unsigned char)hdsp_wc_sync_check(hdsp);
		if (hdsp->io_type != H9632) {
		    info.adatsync_sync_check = (unsigned char)hdsp_adatsync_sync_check(hdsp);
		}
		info.spdif_sync_check = (unsigned char)hdsp_spdif_sync_check(hdsp);
		for (i = 0; i < ((hdsp->io_type != Multiface && hdsp->io_type != H9632) ? 3 : 1); ++i) {
			info.adat_sync_check[i] = (unsigned char)hdsp_adat_sync_check(hdsp, i);
		}
		info.spdif_in = (unsigned char)hdsp_spdif_in(hdsp);
		info.spdif_out = (unsigned char)hdsp_spdif_out(hdsp);
		info.spdif_professional = (unsigned char)hdsp_spdif_professional(hdsp);
		info.spdif_emphasis = (unsigned char)hdsp_spdif_emphasis(hdsp);
		info.spdif_nonaudio = (unsigned char)hdsp_spdif_nonaudio(hdsp);
		info.spdif_sample_rate = hdsp_spdif_sample_rate(hdsp);
		info.system_sample_rate = hdsp->system_sample_rate;
		info.autosync_sample_rate = hdsp_external_sample_rate(hdsp);
		info.system_clock_mode = (unsigned char)hdsp_system_clock_mode(hdsp);
		info.clock_source = (unsigned char)hdsp_clock_source(hdsp);
		info.autosync_ref = (unsigned char)hdsp_autosync_ref(hdsp);
		info.line_out = (unsigned char)hdsp_line_out(hdsp);
		info.passthru = (unsigned char)hdsp->passthru;
		if (hdsp->io_type == H9632) {
			info.da_gain = (unsigned char)hdsp_da_gain(hdsp);
			info.ad_gain = (unsigned char)hdsp_ad_gain(hdsp);
			info.phone_gain = (unsigned char)hdsp_phone_gain(hdsp);
			info.xlr_breakout_cable = (unsigned char)hdsp_xlr_breakout_cable(hdsp);
		
		}
		if (hdsp->io_type == H9632 || hdsp->io_type == H9652) {
			info.analog_extension_board = (unsigned char)hdsp_aeb(hdsp);
		}
		spin_unlock_irqrestore(&hdsp->lock, flags);
		if (copy_to_user(argp, &info, sizeof(info)))
			return -EFAULT;
		break;
	}
	case SNDRV_HDSP_IOCTL_GET_9632_AEB: {
		hdsp_9632_aeb_t h9632_aeb;
		
		if (hdsp->io_type != H9632) return -EINVAL;
		h9632_aeb.aebi = hdsp->ss_in_channels - H9632_SS_CHANNELS;
		h9632_aeb.aebo = hdsp->ss_out_channels - H9632_SS_CHANNELS;
		if (copy_to_user(argp, &h9632_aeb, sizeof(h9632_aeb)))
			return -EFAULT;
		break;
	}
	case SNDRV_HDSP_IOCTL_GET_VERSION: {
		hdsp_version_t hdsp_version;
		int err;
		
		if (hdsp->io_type == H9652 || hdsp->io_type == H9632) return -EINVAL;
		if (hdsp->io_type == Undefined) {
			if ((err = hdsp_get_iobox_version(hdsp)) < 0) {
				return err;
			}
		}
		hdsp_version.io_type = hdsp->io_type;
		hdsp_version.firmware_rev = hdsp->firmware_rev;
		if ((err = copy_to_user(argp, &hdsp_version, sizeof(hdsp_version)))) {
		    	return -EFAULT;
		}
		break;
	}
	case SNDRV_HDSP_IOCTL_UPLOAD_FIRMWARE: {
		hdsp_firmware_t __user *firmware;
		unsigned long __user *firmware_data;
		int err;
		
		if (hdsp->io_type == H9652 || hdsp->io_type == H9632) return -EINVAL;
		/* SNDRV_HDSP_IOCTL_GET_VERSION must have been called */
		if (hdsp->io_type == Undefined) return -EINVAL;

		snd_printk("initializing firmware upload\n");
		firmware = (hdsp_firmware_t __user *)argp;

		if (get_user(firmware_data, &firmware->firmware_data)) {
			return -EFAULT;
		}
		
		if (hdsp_check_for_iobox (hdsp)) {
			return -EIO;
		}

		if (copy_from_user(hdsp->firmware_cache, firmware_data, sizeof(unsigned long)*24413) != 0) {
			return -EFAULT;
		}
		
		hdsp->state |= HDSP_FirmwareCached;

		if ((err = snd_hdsp_load_firmware_from_cache(hdsp)) < 0) {
			return err;
		}
		
		if (!(hdsp->state & HDSP_InitializationComplete)) {
			snd_hdsp_initialize_channels(hdsp);
		
			snd_hdsp_initialize_midi_flush(hdsp);
	    
			if ((err = snd_hdsp_create_alsa_devices(hdsp->card, hdsp)) < 0) {
				snd_printk("error creating alsa devices\n");
			    return err;
			}
		}
		break;
	}
	case SNDRV_HDSP_IOCTL_GET_MIXER: {
		hdsp_mixer_t __user *mixer = (hdsp_mixer_t __user *)argp;
		if (copy_to_user(mixer->matrix, hdsp->mixer_matrix, sizeof(unsigned short)*HDSP_MATRIX_MIXER_SIZE))
			return -EFAULT;
		break;
	}
	default:
		return -EINVAL;
	}
	return 0;
}

static snd_pcm_ops_t snd_hdsp_playback_ops = {
	.open =		snd_hdsp_playback_open,
	.close =	snd_hdsp_playback_release,
	.ioctl =	snd_hdsp_ioctl,
	.hw_params =	snd_hdsp_hw_params,
	.prepare =	snd_hdsp_prepare,
	.trigger =	snd_hdsp_trigger,
	.pointer =	snd_hdsp_hw_pointer,
	.copy =		snd_hdsp_playback_copy,
	.silence =	snd_hdsp_hw_silence,
};

static snd_pcm_ops_t snd_hdsp_capture_ops = {
	.open =		snd_hdsp_capture_open,
	.close =	snd_hdsp_capture_release,
	.ioctl =	snd_hdsp_ioctl,
	.hw_params =	snd_hdsp_hw_params,
	.prepare =	snd_hdsp_prepare,
	.trigger =	snd_hdsp_trigger,
	.pointer =	snd_hdsp_hw_pointer,
	.copy =		snd_hdsp_capture_copy,
};

static int __devinit snd_hdsp_create_hwdep(snd_card_t *card,
					   hdsp_t *hdsp)
{
	snd_hwdep_t *hw;
	int err;
	
	if ((err = snd_hwdep_new(card, "HDSP hwdep", 0, &hw)) < 0)
		return err;
		
	hdsp->hwdep = hw;
	hw->private_data = hdsp;
	strcpy(hw->name, "HDSP hwdep interface");

	hw->ops.open = snd_hdsp_hwdep_dummy_op;
	hw->ops.ioctl = snd_hdsp_hwdep_ioctl;
	hw->ops.release = snd_hdsp_hwdep_dummy_op;
		
	return 0;
}

static int __devinit snd_hdsp_create_pcm(snd_card_t *card,
					 hdsp_t *hdsp)
{
	snd_pcm_t *pcm;
	int err;

	if ((err = snd_pcm_new(card, hdsp->card_name, 0, 1, 1, &pcm)) < 0)
		return err;

	hdsp->pcm = pcm;
	pcm->private_data = hdsp;
	strcpy(pcm->name, hdsp->card_name);

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &snd_hdsp_playback_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &snd_hdsp_capture_ops);

	pcm->info_flags = SNDRV_PCM_INFO_JOINT_DUPLEX;

	return 0;
}

static inline void snd_hdsp_9652_enable_mixer (hdsp_t *hdsp)
{
        hdsp->control2_register |= HDSP_9652_ENABLE_MIXER;
	hdsp_write (hdsp, HDSP_control2Reg, hdsp->control2_register);
}

static inline void snd_hdsp_9652_disable_mixer (hdsp_t *hdsp)
{
        hdsp->control2_register &= ~HDSP_9652_ENABLE_MIXER;
	hdsp_write (hdsp, HDSP_control2Reg, hdsp->control2_register);
}

static inline int snd_hdsp_enable_io (hdsp_t *hdsp)
{
	int i;
	
	if (hdsp_fifo_wait (hdsp, 0, 100)) {
		return -EIO;
	}
	
	for (i = 0; i < hdsp->max_channels; ++i) {
		hdsp_write (hdsp, HDSP_inputEnable + (4 * i), 1);
		hdsp_write (hdsp, HDSP_outputEnable + (4 * i), 1);
	}
	
	return 0;
}

static inline void snd_hdsp_initialize_channels(hdsp_t *hdsp)
{
	int status, aebi_channels, aebo_channels;
	
	switch (hdsp->io_type) {
	case Digiface:
		hdsp->card_name = "RME Hammerfall DSP + Digiface";
		hdsp->ss_in_channels = hdsp->ss_out_channels = DIGIFACE_SS_CHANNELS;
		hdsp->ds_in_channels = hdsp->ds_out_channels = DIGIFACE_DS_CHANNELS;
		break;

	case H9652:
		hdsp->card_name = "RME Hammerfall HDSP 9652";
		hdsp->ss_in_channels = hdsp->ss_out_channels = H9652_SS_CHANNELS;
		hdsp->ds_in_channels = hdsp->ds_out_channels = H9652_DS_CHANNELS;
		break;
	
	case H9632:
		status = hdsp_read(hdsp, HDSP_statusRegister);
		/* HDSP_AEBx bits are low when AEB are connected */
		aebi_channels = (status & HDSP_AEBI) ? 0 : 4;
		aebo_channels = (status & HDSP_AEBO) ? 0 : 4;
		hdsp->card_name = "RME Hammerfall HDSP 9632";
		hdsp->ss_in_channels = H9632_SS_CHANNELS+aebi_channels;
		hdsp->ds_in_channels = H9632_DS_CHANNELS+aebi_channels;
		hdsp->qs_in_channels = H9632_QS_CHANNELS+aebi_channels;
		hdsp->ss_out_channels = H9632_SS_CHANNELS+aebo_channels;
		hdsp->ds_out_channels = H9632_DS_CHANNELS+aebo_channels;
		hdsp->qs_out_channels = H9632_QS_CHANNELS+aebo_channels;
		break;

	case Multiface:
		hdsp->card_name = "RME Hammerfall DSP + Multiface";
		hdsp->ss_in_channels = hdsp->ss_out_channels = MULTIFACE_SS_CHANNELS;
		hdsp->ds_in_channels = hdsp->ds_out_channels = MULTIFACE_DS_CHANNELS;
		break;
		
	default:
 		/* should never get here */
		break;
	}
}

static inline void snd_hdsp_initialize_midi_flush (hdsp_t *hdsp)
{
	snd_hdsp_flush_midi_input (hdsp, 0);
	snd_hdsp_flush_midi_input (hdsp, 1);
}

static int __devinit snd_hdsp_create_alsa_devices(snd_card_t *card, hdsp_t *hdsp)
{
	int err;
	
	if ((err = snd_hdsp_create_pcm(card, hdsp)) < 0) {
		snd_printk("Error creating pcm interface\n");
		return err;
	}
	

	if ((err = snd_hdsp_create_midi(card, hdsp, 0)) < 0) {
		snd_printk("Error creating first midi interface\n");
		return err;
	}


	if ((err = snd_hdsp_create_midi(card, hdsp, 1)) < 0) {
		snd_printk("Error creating second midi interface\n");
		return err;
	}

	if ((err = snd_hdsp_create_controls(card, hdsp)) < 0) {
		snd_printk("Error creating ctl interface\n");
		return err;
	}

	snd_hdsp_proc_init(hdsp);

	hdsp->system_sample_rate = -1;
	hdsp->playback_pid = -1;
	hdsp->capture_pid = -1;
	hdsp->capture_substream = NULL;
	hdsp->playback_substream = NULL;

	if ((err = snd_hdsp_set_defaults(hdsp)) < 0) {
		snd_printk("Error setting default values\n");
		return err;
	}
	
	if (!(hdsp->state & HDSP_InitializationComplete)) {
		sprintf(card->longname, "%s at 0x%lx, irq %d", hdsp->card_name, 
			hdsp->port, hdsp->irq);
	    
		if ((err = snd_card_register(card)) < 0) {
			snd_printk("error registering card\n");
			return err;
		}
		hdsp->state |= HDSP_InitializationComplete;
	}
	
	return 0;
}

static int __devinit snd_hdsp_create(snd_card_t *card,
				     hdsp_t *hdsp,
				     int precise_ptr)
{
	struct pci_dev *pci = hdsp->pci;
	int err;
	int is_9652 = 0;
	int is_9632 = 0;

	hdsp->irq = -1;
	hdsp->state = 0;
	hdsp->midi[0].rmidi = NULL;
	hdsp->midi[1].rmidi = NULL;
	hdsp->midi[0].input = NULL;
	hdsp->midi[1].input = NULL;
	hdsp->midi[0].output = NULL;
	hdsp->midi[1].output = NULL;
	spin_lock_init(&hdsp->midi[0].lock);
	spin_lock_init(&hdsp->midi[1].lock);
	hdsp->iobase = 0;
	hdsp->res_port = NULL;
	hdsp->control_register = 0;
	hdsp->control2_register = 0;
	hdsp->io_type = Undefined;
	hdsp->max_channels = 26;

	hdsp->card = card;
	
	spin_lock_init(&hdsp->lock);

	tasklet_init(&hdsp->midi_tasklet, hdsp_midi_tasklet, (unsigned long)hdsp);
	
	pci_read_config_word(hdsp->pci, PCI_CLASS_REVISION, &hdsp->firmware_rev);
	
	/* From Martin Bjoernsen :
	    "It is important that the card's latency timer register in
	    the PCI configuration space is set to a value much larger
	    than 0 by the computer's BIOS or the driver.
	    The windows driver always sets this 8 bit register [...]
	    to its maximum 255 to avoid problems with some computers."
	*/
	pci_write_config_byte(hdsp->pci, PCI_LATENCY_TIMER, 0xFF);
	
	strcpy(card->driver, "H-DSP");
	strcpy(card->mixername, "Xilinx FPGA");

	switch (hdsp->firmware_rev & 0xff) {
	case 0xa:
	case 0xb:
	case 0x32:
		hdsp->card_name = "RME Hammerfall DSP";
		break;

	case 0x64:
	case 0x65:
	case 0x68:
		hdsp->card_name = "RME HDSP 9652";
		is_9652 = 1;
		break;
	case 0x96:
	case 0x97:
		hdsp->card_name = "RME HDSP 9632";
		hdsp->max_channels = 16;
		is_9632 = 1;
		break;
	default:
		return -ENODEV;
	}

	if ((err = pci_enable_device(pci)) < 0) {
		return err;
	}

	pci_set_master(hdsp->pci);

	hdsp->port = pci_resource_start(pci, 0);

	if ((hdsp->res_port = request_mem_region(hdsp->port, HDSP_IO_EXTENT, "hdsp")) == NULL) {
		snd_printk("unable to grab memory region 0x%lx-0x%lx\n", hdsp->port, hdsp->port + HDSP_IO_EXTENT - 1);
		return -EBUSY;
	}

	if ((hdsp->iobase = (unsigned long) ioremap_nocache(hdsp->port, HDSP_IO_EXTENT)) == 0) {
		snd_printk("unable to remap region 0x%lx-0x%lx\n", hdsp->port, hdsp->port + HDSP_IO_EXTENT - 1);
		return -EBUSY;
	}

	if (request_irq(pci->irq, snd_hdsp_interrupt, SA_INTERRUPT|SA_SHIRQ, "hdsp", (void *)hdsp)) {
		snd_printk("unable to use IRQ %d\n", pci->irq);
		return -EBUSY;
	}

	hdsp->irq = pci->irq;
	hdsp->precise_ptr = precise_ptr;

	if ((err = snd_hdsp_initialize_memory(hdsp)) < 0) {
		return err;
	}
	
	if (!is_9652 && !is_9632 && hdsp_check_for_iobox (hdsp)) {
		/* no iobox connected, we defer initialization */
		snd_printk("card initialization pending : waiting for firmware\n");
		if ((err = snd_hdsp_create_hwdep(card, hdsp)) < 0) {
			return err;
		}
		return 0;
	}
	
	if ((err = snd_hdsp_enable_io(hdsp)) != 0) {
		return err;
	}
	
	if ((hdsp_read (hdsp, HDSP_statusRegister) & HDSP_DllError) != 0) {
		snd_printk("card initialization pending : waiting for firmware\n");
		if ((err = snd_hdsp_create_hwdep(card, hdsp)) < 0) {
			return err;
		}
		return 0;
	} 
	
	snd_printk("Firmware already loaded, initializing card.\n");
	
	if (hdsp_read(hdsp, HDSP_status2Register) & HDSP_version1) {
		hdsp->io_type = Multiface;
	} else {
		hdsp->io_type = Digiface;
	}

	if (is_9652) {
	        hdsp->io_type = H9652;
	}
	
	if (is_9632) {
		hdsp->io_type = H9632;
	}

	if ((err = snd_hdsp_create_hwdep(card, hdsp)) < 0) {
		return err;
	}
	
	snd_hdsp_initialize_channels(hdsp);
	snd_hdsp_initialize_midi_flush(hdsp);

	hdsp->state |= HDSP_FirmwareLoaded;	

	if ((err = snd_hdsp_create_alsa_devices(card, hdsp)) < 0) {
		return err;
	}

	return 0;	
}

static int snd_hdsp_free(hdsp_t *hdsp)
{
	if (hdsp->res_port) {
		/* stop the audio, and cancel all interrupts */
		hdsp->control_register &= ~(HDSP_Start|HDSP_AudioInterruptEnable|HDSP_Midi0InterruptEnable|HDSP_Midi1InterruptEnable);
		hdsp_write (hdsp, HDSP_controlRegister, hdsp->control_register);
	}

	if (hdsp->irq >= 0)
		free_irq(hdsp->irq, (void *)hdsp);

	snd_hdsp_free_buffers(hdsp);
	
	if (hdsp->iobase)
		iounmap((void *) hdsp->iobase);

	if (hdsp->res_port) {
		release_resource(hdsp->res_port);
		kfree_nocheck(hdsp->res_port);
	}
		
	return 0;
}

static void snd_hdsp_card_free(snd_card_t *card)
{
	hdsp_t *hdsp = (hdsp_t *) card->private_data;

	if (hdsp)
		snd_hdsp_free(hdsp);
}

static int __devinit snd_hdsp_probe(struct pci_dev *pci,
				    const struct pci_device_id *pci_id)
{
	static int dev;
	hdsp_t *hdsp;
	snd_card_t *card;
	int err;

	if (dev >= SNDRV_CARDS)
		return -ENODEV;
	if (!enable[dev]) {
		dev++;
		return -ENOENT;
	}

	if (!(card = snd_card_new(index[dev], id[dev], THIS_MODULE, sizeof(hdsp_t))))
		return -ENOMEM;

	hdsp = (hdsp_t *) card->private_data;
	card->private_free = snd_hdsp_card_free;
	hdsp->dev = dev;
	hdsp->pci = pci;
	snd_card_set_dev(card, &pci->dev);

	if ((err = snd_hdsp_create(card, hdsp, precise_ptr[dev])) < 0) {
		snd_card_free(card);
		return err;
	}

	strcpy(card->shortname, "Hammerfall DSP");
	sprintf(card->longname, "%s at 0x%lx, irq %d", hdsp->card_name, 
		hdsp->port, hdsp->irq);

	if ((err = snd_card_register(card)) < 0) {
		snd_card_free(card);
		return err;
	}
	pci_set_drvdata(pci, card);
	dev++;
	return 0;
}

static void __devexit snd_hdsp_remove(struct pci_dev *pci)
{
	snd_card_free(pci_get_drvdata(pci));
	pci_set_drvdata(pci, NULL);
}

static struct pci_driver driver = {
	.name =     "RME Hammerfall DSP",
	.id_table = snd_hdsp_ids,
	.probe =    snd_hdsp_probe,
	.remove = __devexit_p(snd_hdsp_remove),
};

static int __init alsa_card_hdsp_init(void)
{
	return pci_module_init(&driver);
}

static void __exit alsa_card_hdsp_exit(void)
{
	pci_unregister_driver(&driver);
}

module_init(alsa_card_hdsp_init)
module_exit(alsa_card_hdsp_exit)
