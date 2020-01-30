#ifndef __SOUND_AZF3328_H
#define __SOUND_AZF3328_H

/* type argument to use for the I/O functions */
#define WORD_VALUE      0x1000
#define DWORD_VALUE     0x2000
#define BYTE_VALUE      0x4000

/*** main I/O area port indices ***/
/* (only 0x70 of 0x80 bytes saved/restored by Windows driver) */
/* the driver initialisation suggests a layout of 3 main areas:
 * from 0x00 (playback), from 0x20 (recording) and from 0x40 (maybe DirectX
 * timer ???). and probably another area from 0x60 to 0x6f
 * (IRQ management, power management etc. ???). */
/* playback area */
#define IDX_IO_PLAY_FLAGS       0x00
     /* able to reactivate output after output muting due to 8/16bit
      * output change, just like 0x0002.
      * 0x0001 is the only bit that's able to start the DMA counter */
  #define DMA_RESUME			0x0001 /* paused if cleared ? */
     /* 0x0002 *temporarily* set during DMA stopping. hmm
      * both 0x0002 and 0x0004 set in playback setup. */
     /* able to reactivate output after output muting due to 8/16bit
      * output change, just like 0x0001. */
  #define DMA_PLAY_SOMETHING1		0x0002 /* \ alternated (toggled) */
     /* 0x0004: NOT able to reactivate output */
  #define DMA_PLAY_SOMETHING2		0x0004 /* / bits */
  #define SOMETHING_ALMOST_ALWAYS_SET	0x0008 /* ???; can be modified */
  #define DMA_EPILOGUE_SOMETHING	0x0010
  #define DMA_SOMETHING_ELSE		0x0020 /* ??? */
  #define SOMETHING_UNMODIFIABLE	0xffc0 /* unused ? not modifiable */
#define IDX_IO_PLAY_IRQMASK     0x02
  /* write back to flags in case flags are set, in order to ACK IRQ in handler
   * (bit 1 of port 0x64 indicates interrupt for one of these three types)
   * sometimes in this case it just writes 0xffff to globally ACK all IRQs
   * settings written are not reflected when reading back, though.
   * seems to be IRQ, too (frequently used: port |= 0x07 !), but who knows ? */
  #define IRQ_PLAY_SOMETHING		0x0001 /* something & ACK */
  #define IRQ_FINISHED_PLAYBUF_1	0x0002 /* 1st dmabuf finished & ACK */
  #define IRQ_FINISHED_PLAYBUF_2	0x0004 /* 2nd dmabuf finished & ACK */
  #define IRQMASK_SOME_STATUS_1		0x0008 /* \ related bits */
  #define IRQMASK_SOME_STATUS_2		0x0010 /* / (checked together in loop) */
  #define IRQMASK_UNMODIFIABLE		0xffe0 /* unused ? not modifiable */
#define IDX_IO_PLAY_DMA_START_1 0x04 /* start address of 1st DMA play area */
#define IDX_IO_PLAY_DMA_START_2 0x08 /* start address of 2nd DMA play area */
#define IDX_IO_PLAY_DMA_LEN_1   0x0c /* length of 1st DMA play area */
#define IDX_IO_PLAY_DMA_LEN_2   0x0e /* length of 2nd DMA play area */
#define IDX_IO_PLAY_DMA_CURRPOS 0x10 /* current DMA position  */
#define IDX_IO_PLAY_DMA_CURROFS	0x14 /* offset within current DMA play area */
#define IDX_IO_PLAY_SOUNDFORMAT 0x16
  /* all unspecified bits can't be modified */
  #define SOUNDFORMAT_FREQUENCY_MASK	0x000f
    /* all _SUSPECTED_ values are not used by Windows drivers, so we don't
     * have any hard facts, only rough measurements */
    #define SOUNDFORMAT_FREQ_SUSPECTED_4000	0x0c
    #define SOUNDFORMAT_FREQ_SUSPECTED_4800	0x0a
    #define SOUNDFORMAT_FREQ_5510		0x0d
    #define SOUNDFORMAT_FREQ_6620		0x0b
    #define SOUNDFORMAT_FREQ_8000		0x00 /* also 0x0e ? */
    #define SOUNDFORMAT_FREQ_9600		0x08
    #define SOUNDFORMAT_FREQ_SUSPECTED_12000	0x09
    #define SOUNDFORMAT_FREQ_11025		0x01 /* also 0x0f ? */
    #define SOUNDFORMAT_FREQ_16000		0x02
    #define SOUNDFORMAT_FREQ_22050		0x03
    #define SOUNDFORMAT_FREQ_32000		0x04
    #define SOUNDFORMAT_FREQ_44100		0x05
    #define SOUNDFORMAT_FREQ_48000		0x06
    #define SOUNDFORMAT_FREQ_SUSPECTED_64000	0x07
  #define SOUNDFORMAT_FLAG_16BIT	0x0010
  #define SOUNDFORMAT_FLAG_2CHANNELS	0x0020
/* recording area (see also: playback bit flag definitions) */
#define IDX_IO_REC_FLAGS	0x20 /* ?? */
#define IDX_IO_REC_IRQMASK	0x22 /* ?? */
  #define IRQ_REC_SOMETHING		0x0001 /* something & ACK */
  #define IRQ_FINISHED_RECBUF_1		0x0002 /* 1st dmabuf finished & ACK */
  #define IRQ_FINISHED_RECBUF_2		0x0004 /* 2nd dmabuf finished & ACK */
  /* hmm, maybe these are just the corresponding *recording* flags ?
   * but OTOH they are most likely at port 0x22 instead */
  #define IRQMASK_SOME_STATUS_1		0x0008 /* \ related bits */
  #define IRQMASK_SOME_STATUS_2		0x0010 /* / (checked together in loop) */
#define IDX_IO_REC_DMA_START_1  0x24
#define IDX_IO_REC_DMA_START_2  0x28
#define IDX_IO_REC_DMA_LEN_1    0x2c
#define IDX_IO_REC_DMA_LEN_2    0x2e
#define IDX_IO_REC_DMA_CURRPOS  0x30
#define IDX_IO_REC_DMA_CURROFS  0x34
#define IDX_IO_REC_SOUNDFORMAT  0x36
/* some third area ? (after playback and recording) */
#define IDX_IO_SOMETHING_FLAGS	0x40 /* gets set to 0x34 just like port 0x0 and 0x20 on card init */
/* general */
#define IDX_IO_60H		0x60 /* writing 0xffff returns 0xffff */
#define IDX_IO_62H		0x62 /* writing to WORD 0x0062 can hang the box ! --> responsible for IRQ management as a whole ?? */
#define IDX_IO_IRQ63H		0x63 /* FIXME !! */
  #define IO_IRQ63H_SOMETHING		0x04 /* being set in IRQ handler in case port 0x00 had 0x0020 set upon IRQ handler */
#define IDX_IO_IRQSTATUS        0x64
  #define IRQ_PLAYBACK			0x0001
  #define IRQ_RECORDING			0x0002
  #define IRQ_MPU401			0x0010
  #define IRQ_SOMEIRQ			0x0020 /* ???? */
  #define IRQ_WHO_KNOWS_UNUSED		0x00e0 /* probably unused */
#define IDX_IO_66H		0x66    /* writing 0xffff returns 0x0000 */
#define IDX_IO_SOME_VALUE	0x68	/* this is always set to 0x3ff, and writable; maybe some buffer limit, but I couldn't find out more */
#define IDX_IO_6AH		0x6A	/* this WORD can be set to have bits 0x0028 activated; actually inhibits PCM playback !!! maybe power management ?? */
#define IDX_IO_6CH		0x6C	/* this WORD can have all its bits activated ? */
#define IDX_IO_6EH		0x6E	/* writing 0xffff returns 0x83fe */
/* further I/O indices not saved/restored, so probably not used */

/*** I/O 2 area port indices ***/
/* (only 0x06 of 0x08 bytes saved/restored by Windows driver) */ 
#define IDX_IO2_LEGACY_ADDR	0x04
  #define LEGACY_SOMETHING		0x01 /* OPL3 ?? */
  #define LEGACY_JOY			0x08

/*** mixer I/O area port indices ***/
/* (only 0x22 of 0x40 bytes saved/restored by Windows driver)
 * generally spoken: AC97 register index = AZF3328 mixer reg index + 2
 * (in other words: AZF3328 NOT fully AC97 compliant) */
  #define MIXER_VOLUME_RIGHT_MASK	0x001f
  #define MIXER_VOLUME_LEFT_MASK	0x1f00
  #define MIXER_MUTE_MASK		0x8000
#define IDX_MIXER_RESET		0x00 /* does NOT seem to have AC97 ID bits */
#define IDX_MIXER_PLAY_MASTER   0x02
#define IDX_MIXER_MODEMOUT      0x04
#define IDX_MIXER_BASSTREBLE    0x06
  #define MIXER_BASSTREBLE_TREBLE_VOLUME_MASK	0x000e
  #define MIXER_BASSTREBLE_BASS_VOLUME_MASK	0x0e00
#define IDX_MIXER_PCBEEP        0x08
#define IDX_MIXER_MODEMIN       0x0a
#define IDX_MIXER_MIC           0x0c
  #define MIXER_MIC_MICGAIN_20DB_ENHANCEMENT_MASK	0x0040
#define IDX_MIXER_LINEIN        0x0e
#define IDX_MIXER_CDAUDIO       0x10
#define IDX_MIXER_VIDEO         0x12
#define IDX_MIXER_AUX           0x14
#define IDX_MIXER_WAVEOUT       0x16
#define IDX_MIXER_FMSYNTH       0x18
#define IDX_MIXER_REC_SELECT    0x1a
  #define MIXER_REC_SELECT_MIC		0x00
  #define MIXER_REC_SELECT_CD		0x01
  #define MIXER_REC_SELECT_VIDEO	0x02
  #define MIXER_REC_SELECT_AUX		0x03
  #define MIXER_REC_SELECT_LINEIN	0x04
  #define MIXER_REC_SELECT_MIXSTEREO	0x05
  #define MIXER_REC_SELECT_MIXMONO	0x06
  #define MIXER_REC_SELECT_MONOIN	0x07
#define IDX_MIXER_REC_VOLUME    0x1c
#define IDX_MIXER_ADVCTL1       0x1e
  /* unlisted bits are unmodifiable */
  #define MIXER_ADVCTL1_3DWIDTH_MASK	0x000e
  #define MIXER_ADVCTL1_HIFI3D_MASK	0x0300
#define IDX_MIXER_ADVCTL2       0x20 /* resembles AC97_GENERAL_PURPOSE reg ! */
  /* unlisted bits are unmodifiable */
  #define MIXER_ADVCTL2_BIT7		0x0080 /* WaveOut 3D Bypass ? mutes WaveOut at LineOut */
  #define MIXER_ADVCTL2_BIT8		0x0100 /* is this Modem Out Select ? */
  #define MIXER_ADVCTL2_BIT9		0x0200 /* Mono Select Source ? */
  #define MIXER_ADVCTL2_BIT13		0x2000 /* 3D enable ? */
  #define MIXER_ADVCTL2_BIT15		0x8000 /* unknown */
  
#define IDX_MIXER_SOMETHING30H	0x30 /* used, but unknown ??? */

/* driver internal flags */
#define SET_CHAN_LEFT	1
#define SET_CHAN_RIGHT	2

#endif /* __SOUND_AZF3328_H  */
