#ifndef __LINUX_VIDEODEV2_H
#define __LINUX_VIDEODEV2_H
/*
 *	Video for Linux Two
 *
 *	Header file for v4l or V4L2 drivers and applications, for
 *	Linux kernels 2.2.x or 2.4.x.
 *
 *	See http://bytesex.org/v4l/ for API specs and other
 *	v4l2 documentation.
 *
 *	Author: Bill Dirks <bdirks@pacbell.net>
 *		Justin Schoeman
 *		et al.
 */
#ifdef __KERNEL__
#include <linux/time.h> /* need struct timeval */
#endif

/*
 *	M I S C E L L A N E O U S
 */

/*  Four-character-code (FOURCC) */
#define v4l2_fourcc(a,b,c,d)\
        (((__u32)(a)<<0)|((__u32)(b)<<8)|((__u32)(c)<<16)|((__u32)(d)<<24))

/*
 *	E N U M S
 */
enum v4l2_field {
	V4L2_FIELD_ANY        = 0, /* driver can choose from none,
				      top, bottom, interlaced
				      depending on whatever it thinks
				      is approximate ... */
	V4L2_FIELD_NONE       = 1, /* this device has no fields ... */
	V4L2_FIELD_TOP        = 2, /* top field only */
	V4L2_FIELD_BOTTOM     = 3, /* bottom field only */
	V4L2_FIELD_INTERLACED = 4, /* both fields interlaced */
	V4L2_FIELD_SEQ_TB     = 5, /* both fields sequential into one
				      buffer, top-bottom order */
	V4L2_FIELD_SEQ_BT     = 6, /* same as above + bottom-top order */
	V4L2_FIELD_ALTERNATE  = 7, /* both fields alternating into
				      separate buffers */
};
#define V4L2_FIELD_HAS_TOP(field)	\
	((field) == V4L2_FIELD_TOP 	||\
	 (field) == V4L2_FIELD_INTERLACED ||\
	 (field) == V4L2_FIELD_SEQ_TB	||\
	 (field) == V4L2_FIELD_SEQ_BT)
#define V4L2_FIELD_HAS_BOTTOM(field)	\
	((field) == V4L2_FIELD_BOTTOM 	||\
	 (field) == V4L2_FIELD_INTERLACED ||\
	 (field) == V4L2_FIELD_SEQ_TB	||\
	 (field) == V4L2_FIELD_SEQ_BT)
#define V4L2_FIELD_HAS_BOTH(field)	\
	((field) == V4L2_FIELD_INTERLACED ||\
	 (field) == V4L2_FIELD_SEQ_TB	||\
	 (field) == V4L2_FIELD_SEQ_BT)

enum v4l2_buf_type {
	V4L2_BUF_TYPE_VIDEO_CAPTURE  = 1,
	V4L2_BUF_TYPE_VIDEO_OUTPUT   = 2,
	V4L2_BUF_TYPE_VIDEO_OVERLAY  = 3,
	V4L2_BUF_TYPE_VBI_CAPTURE    = 4,
	V4L2_BUF_TYPE_VBI_OUTPUT     = 5,
	V4L2_BUF_TYPE_PRIVATE        = 0x80,
};

enum v4l2_ctrl_type {
	V4L2_CTRL_TYPE_INTEGER	     = 1,
	V4L2_CTRL_TYPE_BOOLEAN	     = 2,
	V4L2_CTRL_TYPE_MENU	     = 3,
	V4L2_CTRL_TYPE_BUTTON	     = 4,
};

enum v4l2_tuner_type {
	V4L2_TUNER_RADIO	     = 1,
	V4L2_TUNER_ANALOG_TV	     = 2,
};

enum v4l2_memory {
	V4L2_MEMORY_MMAP             = 1,
	V4L2_MEMORY_USERPTR          = 2,
	V4L2_MEMORY_OVERLAY          = 3,
};

/* see also http://vektor.theorem.ca/graphics/ycbcr/ */
enum v4l2_colorspace {
	/* ITU-R 601 -- broadcast NTSC/PAL */
	V4L2_COLORSPACE_SMPTE170M     = 1,

	/* 1125-Line (US) HDTV */
	V4L2_COLORSPACE_SMPTE240M     = 2,

	/* HD and modern captures. */
	V4L2_COLORSPACE_REC709        = 3,
	
	/* broken BT878 extents (601, luma range 16-253 instead of 16-235) */
	V4L2_COLORSPACE_BT878         = 4,
	
	/* These should be useful.  Assume 601 extents. */
	V4L2_COLORSPACE_470_SYSTEM_M  = 5,
	V4L2_COLORSPACE_470_SYSTEM_BG = 6,
	
	/* I know there will be cameras that send this.  So, this is
	 * unspecified chromaticities and full 0-255 on each of the
	 * Y'CbCr components
	 */
	V4L2_COLORSPACE_JPEG          = 7,
	
	/* For RGB colourspaces, this is probably a good start. */
	V4L2_COLORSPACE_SRGB          = 8,
};

enum v4l2_priority {
	V4L2_PRIORITY_UNSET       = 0,  /* not initialized */
	V4L2_PRIORITY_BACKGROUND  = 1,
	V4L2_PRIORITY_INTERACTIVE = 2,
	V4L2_PRIORITY_RECORD      = 3,
	V4L2_PRIORITY_DEFAULT     = V4L2_PRIORITY_INTERACTIVE,
};

struct v4l2_rect {
	__s32   left;
	__s32   top;
	__s32   width;
	__s32   height;
};

struct v4l2_fract {
	__u32   numerator;
	__u32   denominator;
};

/*
 *	D R I V E R   C A P A B I L I T I E S
 */
struct v4l2_capability
{
	__u8	driver[16];	/* i.e. "bttv" */
	__u8	card[32];	/* i.e. "Hauppauge WinTV" */
	__u8	bus_info[32];	/* "PCI:" + pci_name(pci_dev) */
	__u32   version;        /* should use KERNEL_VERSION() */
	__u32	capabilities;	/* Device capabilities */
	__u32	reserved[4];
};

/* Values for 'capabilities' field */
#define V4L2_CAP_VIDEO_CAPTURE	0x00000001  /* Is a video capture device */
#define V4L2_CAP_VIDEO_OUTPUT	0x00000002  /* Is a video output device */
#define V4L2_CAP_VIDEO_OVERLAY	0x00000004  /* Can do video overlay */
#define V4L2_CAP_VBI_CAPTURE	0x00000010  /* Is a VBI capture device */
#define V4L2_CAP_VBI_OUTPUT	0x00000020  /* Is a VBI output device */
#define V4L2_CAP_RDS_CAPTURE	0x00000100  /* RDS data capture */

#define V4L2_CAP_TUNER		0x00010000  /* has a tuner */
#define V4L2_CAP_AUDIO		0x00020000  /* has audio support */
#define V4L2_CAP_RADIO		0x00040000  /* is a radio device */

#define V4L2_CAP_READWRITE      0x01000000  /* read/write systemcalls */
#define V4L2_CAP_ASYNCIO        0x02000000  /* async I/O */
#define V4L2_CAP_STREAMING      0x04000000  /* streaming I/O ioctls */

/*
 *	V I D E O   I M A G E   F O R M A T
 */

struct v4l2_pix_format
{
	__u32         	 	width;
	__u32	         	height;
	__u32	         	pixelformat;
	enum v4l2_field  	field;
	__u32            	bytesperline;	/* for padding, zero if unused */
	__u32          	 	sizeimage;
        enum v4l2_colorspace	colorspace;
	__u32			priv;		/* private data, depends on pixelformat */
};

/*           Pixel format    FOURCC                  depth  Description   */
#define V4L2_PIX_FMT_RGB332  v4l2_fourcc('R','G','B','1') /*  8  RGB-3-3-2     */
#define V4L2_PIX_FMT_RGB555  v4l2_fourcc('R','G','B','O') /* 16  RGB-5-5-5     */
#define V4L2_PIX_FMT_RGB565  v4l2_fourcc('R','G','B','P') /* 16  RGB-5-6-5     */
#define V4L2_PIX_FMT_RGB555X v4l2_fourcc('R','G','B','Q') /* 16  RGB-5-5-5 BE  */
#define V4L2_PIX_FMT_RGB565X v4l2_fourcc('R','G','B','R') /* 16  RGB-5-6-5 BE  */
#define V4L2_PIX_FMT_BGR24   v4l2_fourcc('B','G','R','3') /* 24  BGR-8-8-8     */
#define V4L2_PIX_FMT_RGB24   v4l2_fourcc('R','G','B','3') /* 24  RGB-8-8-8     */
#define V4L2_PIX_FMT_BGR32   v4l2_fourcc('B','G','R','4') /* 32  BGR-8-8-8-8   */
#define V4L2_PIX_FMT_RGB32   v4l2_fourcc('R','G','B','4') /* 32  RGB-8-8-8-8   */
#define V4L2_PIX_FMT_GREY    v4l2_fourcc('G','R','E','Y') /*  8  Greyscale     */
#define V4L2_PIX_FMT_YVU410  v4l2_fourcc('Y','V','U','9') /*  9  YVU 4:1:0     */
#define V4L2_PIX_FMT_YVU420  v4l2_fourcc('Y','V','1','2') /* 12  YVU 4:2:0     */
#define V4L2_PIX_FMT_YUYV    v4l2_fourcc('Y','U','Y','V') /* 16  YUV 4:2:2     */
#define V4L2_PIX_FMT_UYVY    v4l2_fourcc('U','Y','V','Y') /* 16  YUV 4:2:2     */
#define V4L2_PIX_FMT_YUV422P v4l2_fourcc('4','2','2','P') /* 16  YVU422 planar */
#define V4L2_PIX_FMT_YUV411P v4l2_fourcc('4','1','1','P') /* 16  YVU411 planar */
#define V4L2_PIX_FMT_Y41P    v4l2_fourcc('Y','4','1','P') /* 12  YUV 4:1:1     */

/* two planes -- one Y, one Cr + Cb interleaved  */
#define V4L2_PIX_FMT_NV12    v4l2_fourcc('N','V','1','2') /* 12  Y/CbCr 4:2:0  */
#define V4L2_PIX_FMT_NV21    v4l2_fourcc('N','V','2','1') /* 12  Y/CrCb 4:2:0  */

/*  The following formats are not defined in the V4L2 specification */
#define V4L2_PIX_FMT_YUV410  v4l2_fourcc('Y','U','V','9') /*  9  YUV 4:1:0     */
#define V4L2_PIX_FMT_YUV420  v4l2_fourcc('Y','U','1','2') /* 12  YUV 4:2:0     */
#define V4L2_PIX_FMT_YYUV    v4l2_fourcc('Y','Y','U','V') /* 16  YUV 4:2:2     */
#define V4L2_PIX_FMT_HI240   v4l2_fourcc('H','I','2','4') /*  8  8-bit color   */

/* see http://www.siliconimaging.com/RGB%20Bayer.htm */
#define V4L2_PIX_FMT_SBGGR8  v4l2_fourcc('B','A','8','1') /*  8  BGBG.. GRGR.. */

/* compressed formats */
#define V4L2_PIX_FMT_MJPEG    v4l2_fourcc('M','J','P','G') /* Motion-JPEG   */
#define V4L2_PIX_FMT_JPEG     v4l2_fourcc('J','P','E','G') /* JFIF JPEG     */
#define V4L2_PIX_FMT_DV       v4l2_fourcc('d','v','s','d') /* 1394          */
#define V4L2_PIX_FMT_MPEG     v4l2_fourcc('M','P','E','G') /* MPEG          */

/*  Vendor-specific formats   */
#define V4L2_PIX_FMT_WNVA     v4l2_fourcc('W','N','V','A') /* Winnov hw compress */

/*
 *	F O R M A T   E N U M E R A T I O N
 */
struct v4l2_fmtdesc
{
	__u32	            index;             /* Format number      */
	enum v4l2_buf_type  type;              /* buffer type        */
	__u32               flags;
	__u8	            description[32];   /* Description string */
	__u32	            pixelformat;       /* Format fourcc      */
	__u32	            reserved[4];
};

#define V4L2_FMT_FLAG_COMPRESSED 0x0001


/*
 *	T I M E C O D E
 */
struct v4l2_timecode
{
	__u32	type;
	__u32	flags;
	__u8	frames;
	__u8	seconds;
	__u8	minutes;
	__u8	hours;
	__u8	userbits[4];
};

/*  Type  */
#define V4L2_TC_TYPE_24FPS		1
#define V4L2_TC_TYPE_25FPS		2
#define V4L2_TC_TYPE_30FPS		3
#define V4L2_TC_TYPE_50FPS		4
#define V4L2_TC_TYPE_60FPS		5

/*  Flags  */
#define V4L2_TC_FLAG_DROPFRAME		0x0001 /* "drop-frame" mode */
#define V4L2_TC_FLAG_COLORFRAME		0x0002
#define V4L2_TC_USERBITS_field		0x000C
#define V4L2_TC_USERBITS_USERDEFINED	0x0000
#define V4L2_TC_USERBITS_8BITCHARS	0x0008
/* The above is based on SMPTE timecodes */


/*
 *	C O M P R E S S I O N   P A R A M E T E R S
 */
#if 0
/* ### generic compression settings don't work, there is too much
 * ### codec-specific stuff.  Maybe reuse that for MPEG codec settings
 * ### later ... */
struct v4l2_compression
{
	__u32	quality;
	__u32	keyframerate;
	__u32	pframerate;
	__u32	reserved[5];

/*  what we'll need for MPEG, extracted from some postings on
    the v4l list (Gert Vervoort, PlasmaJohn).

system stream:
  - type: elementary stream(ES), packatised elementary stream(s) (PES)
    program stream(PS), transport stream(TS)
  - system bitrate
  - PS packet size (DVD: 2048 bytes, VCD: 2324 bytes)
  - TS video PID
  - TS audio PID
  - TS PCR PID
  - TS system information tables (PAT, PMT, CAT, NIT and SIT)
  - (MPEG-1 systems stream vs. MPEG-2 program stream (TS not supported
    by MPEG-1 systems)

audio:
  - type: MPEG (+Layer I,II,III), AC-3, LPCM
  - bitrate
  - sampling frequency (DVD: 48 Khz, VCD: 44.1 KHz, 32 kHz)
  - Trick Modes? (ff, rew)
  - Copyright
  - Inverse Telecine

video:
  - picturesize (SIF, 1/2 D1, 2/3 D1, D1) and PAL/NTSC norm can be set
    through excisting V4L2 controls
  - noise reduction, parameters encoder specific?
  - MPEG video version: MPEG-1, MPEG-2
  - GOP (Group Of Pictures) definition:
    - N: number of frames per GOP
    - M: distance between reference (I,P) frames
    - open/closed GOP
  - quantiser matrix: inter Q matrix (64 bytes) and intra Q matrix (64 bytes)
  - quantiser scale: linear or logarithmic
  - scanning: alternate or zigzag
  - bitrate mode: CBR (constant bitrate) or VBR (variable bitrate).
  - target video bitrate for CBR
  - target video bitrate for VBR
  - maximum video bitrate for VBR - min. quantiser value for VBR
  - max. quantiser value for VBR
  - adaptive quantisation value
  - return the number of bytes per GOP or bitrate for bitrate monitoring

*/
};
#endif

struct v4l2_jpegcompression
{
	int quality;

	int  APPn;              /* Number of APP segment to be written,
				 * must be 0..15 */
	int  APP_len;           /* Length of data in JPEG APPn segment */
	char APP_data[60];      /* Data in the JPEG APPn segment. */
	
	int  COM_len;           /* Length of data in JPEG COM segment */
	char COM_data[60];      /* Data in JPEG COM segment */
	
	__u32 jpeg_markers;     /* Which markers should go into the JPEG
				 * output. Unless you exactly know what
				 * you do, leave them untouched.
				 * Inluding less markers will make the
				 * resulting code smaller, but there will
				 * be fewer aplications which can read it.
				 * The presence of the APP and COM marker
				 * is influenced by APP_len and COM_len
				 * ONLY, not by this property! */
	
#define V4L2_JPEG_MARKER_DHT (1<<3)    /* Define Huffman Tables */
#define V4L2_JPEG_MARKER_DQT (1<<4)    /* Define Quantization Tables */
#define V4L2_JPEG_MARKER_DRI (1<<5)    /* Define Restart Interval */
#define V4L2_JPEG_MARKER_COM (1<<6)    /* Comment segment */
#define V4L2_JPEG_MARKER_APP (1<<7)    /* App segment, driver will
                                        * allways use APP0 */
};


/*
 *	M E M O R Y - M A P P I N G   B U F F E R S
 */
struct v4l2_requestbuffers
{
	__u32	                count;
	enum v4l2_buf_type      type;
	enum v4l2_memory        memory;
	__u32	                reserved[2];
};

struct v4l2_buffer
{
	__u32			index;
	enum v4l2_buf_type      type;
	__u32			bytesused;
	__u32			flags;
	enum v4l2_field		field;
	struct timeval		timestamp;
	struct v4l2_timecode	timecode;
	__u32			sequence;

	/* memory location */
	enum v4l2_memory        memory;
	union {
		__u32           offset;
		unsigned long   userptr;
	} m;
	__u32			length;
	__u32			input;
	__u32			reserved;
};

/*  Flags for 'flags' field */
#define V4L2_BUF_FLAG_MAPPED	0x0001  /* Buffer is mapped (flag) */
#define V4L2_BUF_FLAG_QUEUED	0x0002	/* Buffer is queued for processing */
#define V4L2_BUF_FLAG_DONE	0x0004	/* Buffer is ready */
#define V4L2_BUF_FLAG_KEYFRAME	0x0008	/* Image is a keyframe (I-frame) */
#define V4L2_BUF_FLAG_PFRAME	0x0010	/* Image is a P-frame */
#define V4L2_BUF_FLAG_BFRAME	0x0020	/* Image is a B-frame */
#define V4L2_BUF_FLAG_TIMECODE	0x0100	/* timecode field is valid */
#define V4L2_BUF_FLAG_INPUT     0x0200  /* input field is valid */

/*
 *	O V E R L A Y   P R E V I E W
 */
struct v4l2_framebuffer
{
	__u32			capability;
	__u32			flags;
/* FIXME: in theory we should pass something like PCI device + memory
 * region + offset instead of some physical address */
	void*                   base;
	struct v4l2_pix_format	fmt;
};
/*  Flags for the 'capability' field. Read only */
#define V4L2_FBUF_CAP_EXTERNOVERLAY	0x0001
#define V4L2_FBUF_CAP_CHROMAKEY		0x0002
#define V4L2_FBUF_CAP_LIST_CLIPPING     0x0004
#define V4L2_FBUF_CAP_BITMAP_CLIPPING	0x0008
/*  Flags for the 'flags' field. */
#define V4L2_FBUF_FLAG_PRIMARY		0x0001
#define V4L2_FBUF_FLAG_OVERLAY		0x0002
#define V4L2_FBUF_FLAG_CHROMAKEY	0x0004

struct v4l2_clip
{
	struct v4l2_rect        c;
	struct v4l2_clip	*next;
};

struct v4l2_window
{
	struct v4l2_rect        w;
	enum v4l2_field  	field;
	__u32			chromakey;
	struct v4l2_clip	__user *clips;
	__u32			clipcount;
	void			__user *bitmap;
};


/*
 *	C A P T U R E   P A R A M E T E R S
 */
struct v4l2_captureparm
{
	__u32		   capability;	  /*  Supported modes */
	__u32		   capturemode;	  /*  Current mode */
	struct v4l2_fract  timeperframe;  /*  Time per frame in .1us units */
	__u32		   extendedmode;  /*  Driver-specific extensions */
	__u32              readbuffers;   /*  # of buffers for read */
	__u32		   reserved[4];
};
/*  Flags for 'capability' and 'capturemode' fields */
#define V4L2_MODE_HIGHQUALITY	0x0001	/*  High quality imaging mode */
#define V4L2_CAP_TIMEPERFRAME	0x1000	/*  timeperframe field is supported */

struct v4l2_outputparm
{
	__u32		   capability;	 /*  Supported modes */
	__u32		   outputmode;	 /*  Current mode */
	struct v4l2_fract  timeperframe; /*  Time per frame in seconds */
	__u32		   extendedmode; /*  Driver-specific extensions */
	__u32              writebuffers; /*  # of buffers for write */
	__u32		   reserved[4];
};

/*
 *	I N P U T   I M A G E   C R O P P I N G
 */

struct v4l2_cropcap {
	enum v4l2_buf_type      type;	
        struct v4l2_rect        bounds;
        struct v4l2_rect        defrect;
        struct v4l2_fract       pixelaspect;
};

struct v4l2_crop {
	enum v4l2_buf_type      type;
	struct v4l2_rect        c;
};

/*
 *      A N A L O G   V I D E O   S T A N D A R D
 */

typedef __u64 v4l2_std_id;

/* one bit for each */
#define V4L2_STD_PAL_B          ((v4l2_std_id)0x00000001)
#define V4L2_STD_PAL_B1         ((v4l2_std_id)0x00000002)
#define V4L2_STD_PAL_G          ((v4l2_std_id)0x00000004)
#define V4L2_STD_PAL_H          ((v4l2_std_id)0x00000008)
#define V4L2_STD_PAL_I          ((v4l2_std_id)0x00000010)
#define V4L2_STD_PAL_D          ((v4l2_std_id)0x00000020)
#define V4L2_STD_PAL_D1         ((v4l2_std_id)0x00000040)
#define V4L2_STD_PAL_K          ((v4l2_std_id)0x00000080)

#define V4L2_STD_PAL_M          ((v4l2_std_id)0x00000100)
#define V4L2_STD_PAL_N          ((v4l2_std_id)0x00000200)
#define V4L2_STD_PAL_Nc         ((v4l2_std_id)0x00000400)
#define V4L2_STD_PAL_60         ((v4l2_std_id)0x00000800)

#define V4L2_STD_NTSC_M         ((v4l2_std_id)0x00001000)
#define V4L2_STD_NTSC_M_JP      ((v4l2_std_id)0x00002000)

#define V4L2_STD_SECAM_B        ((v4l2_std_id)0x00010000)
#define V4L2_STD_SECAM_D        ((v4l2_std_id)0x00020000)
#define V4L2_STD_SECAM_G        ((v4l2_std_id)0x00040000)
#define V4L2_STD_SECAM_H        ((v4l2_std_id)0x00080000)
#define V4L2_STD_SECAM_K        ((v4l2_std_id)0x00100000)
#define V4L2_STD_SECAM_K1       ((v4l2_std_id)0x00200000)
#define V4L2_STD_SECAM_L        ((v4l2_std_id)0x00400000)

/* ATSC/HDTV */
#define V4L2_STD_ATSC_8_VSB     ((v4l2_std_id)0x01000000)
#define V4L2_STD_ATSC_16_VSB    ((v4l2_std_id)0x02000000)

/* some common needed stuff */
#define V4L2_STD_PAL_BG		(V4L2_STD_PAL_B		|\
				 V4L2_STD_PAL_B1	|\
				 V4L2_STD_PAL_G)
#define V4L2_STD_PAL_DK		(V4L2_STD_PAL_D		|\
				 V4L2_STD_PAL_D1	|\
				 V4L2_STD_PAL_K)
#define V4L2_STD_PAL		(V4L2_STD_PAL_BG	|\
				 V4L2_STD_PAL_DK	|\
				 V4L2_STD_PAL_H		|\
				 V4L2_STD_PAL_I)
#define V4L2_STD_NTSC           (V4L2_STD_NTSC_M	|\
				 V4L2_STD_NTSC_M_JP)
#define V4L2_STD_SECAM_DK      	(V4L2_STD_SECAM_D	|\
				 V4L2_STD_SECAM_K	|\
				 V4L2_STD_SECAM_K1)
#define V4L2_STD_SECAM		(V4L2_STD_SECAM_B	|\
				 V4L2_STD_SECAM_G	|\
				 V4L2_STD_SECAM_H	|\
				 V4L2_STD_SECAM_DK	|\
				 V4L2_STD_SECAM_L)

#define V4L2_STD_525_60		(V4L2_STD_PAL_M		|\
				 V4L2_STD_PAL_60	|\
				 V4L2_STD_NTSC)
#define V4L2_STD_625_50		(V4L2_STD_PAL		|\
				 V4L2_STD_PAL_N		|\
				 V4L2_STD_PAL_Nc	|\
				 V4L2_STD_SECAM)
#define V4L2_STD_ATSC           (V4L2_STD_ATSC_8_VSB    |\
		                 V4L2_STD_ATSC_16_VSB)

#define V4L2_STD_UNKNOWN        0
#define V4L2_STD_ALL            (V4L2_STD_525_60	|\
				 V4L2_STD_625_50)

struct v4l2_standard
{
	__u32	       	     index;
	v4l2_std_id          id;
	__u8		     name[24];
	struct v4l2_fract    frameperiod; /* Frames, not fields */
	__u32		     framelines;
	__u32		     reserved[4];
};


/*
 *	V I D E O   I N P U T S
 */
struct v4l2_input
{
	__u32	     index;		/*  Which input */
	__u8	     name[32];	        /*  Label */
	__u32	     type;		/*  Type of input */
	__u32	     audioset;	        /*  Associated audios (bitfield) */
	__u32        tuner;             /*  Associated tuner */
	v4l2_std_id  std;
	__u32	     status;
	__u32	     reserved[4];
};
/*  Values for the 'type' field */
#define V4L2_INPUT_TYPE_TUNER		1
#define V4L2_INPUT_TYPE_CAMERA		2

/* field 'status' - general */
#define V4L2_IN_ST_NO_POWER    0x00000001  /* Attached device is off */
#define V4L2_IN_ST_NO_SIGNAL   0x00000002
#define V4L2_IN_ST_NO_COLOR    0x00000004

/* field 'status' - analog */
#define V4L2_IN_ST_NO_H_LOCK   0x00000100  /* No horizontal sync lock */
#define V4L2_IN_ST_COLOR_KILL  0x00000200  /* Color killer is active */

/* field 'status' - digital */
#define V4L2_IN_ST_NO_SYNC     0x00010000  /* No synchronization lock */
#define V4L2_IN_ST_NO_EQU      0x00020000  /* No equalizer lock */
#define V4L2_IN_ST_NO_CARRIER  0x00040000  /* Carrier recovery failed */

/* field 'status' - VCR and set-top box */
#define V4L2_IN_ST_MACROVISION 0x01000000  /* Macrovision detected */
#define V4L2_IN_ST_NO_ACCESS   0x02000000  /* Conditional access denied */
#define V4L2_IN_ST_VTR         0x04000000  /* VTR time constant */

/*
 *	V I D E O   O U T P U T S
 */
struct v4l2_output
{
	__u32	     index;		/*  Which output */
	__u8	     name[32];	        /*  Label */
	__u32	     type;		/*  Type of output */
	__u32	     audioset;	        /*  Associated audios (bitfield) */
	__u32	     modulator;         /*  Associated modulator */
	v4l2_std_id  std;
	__u32	     reserved[4];
};
/*  Values for the 'type' field */
#define V4L2_OUTPUT_TYPE_MODULATOR		1
#define V4L2_OUTPUT_TYPE_ANALOG			2
#define V4L2_OUTPUT_TYPE_ANALOGVGAOVERLAY	3

/*
 *	C O N T R O L S
 */
struct v4l2_control
{
	__u32		     id;
	__s32		     value;
};

/*  Used in the VIDIOC_QUERYCTRL ioctl for querying controls */
struct v4l2_queryctrl
{
	__u32	             id;
	enum v4l2_ctrl_type  type;
	__u8		     name[32];	/* Whatever */
	__s32		     minimum;	/* Note signedness */
	__s32		     maximum;
	__s32	             step;
	__s32		     default_value;
	__u32                flags;
	__u32		     reserved[2];
};

/*  Used in the VIDIOC_QUERYMENU ioctl for querying menu items */
struct v4l2_querymenu
{
	__u32		id;
	__u32		index;
	__u8		name[32];	/* Whatever */
	__u32		reserved;
};

/*  Control flags  */
#define V4L2_CTRL_FLAG_DISABLED		0x0001
#define V4L2_CTRL_FLAG_GRABBED		0x0002

/*  Control IDs defined by V4L2 */
#define V4L2_CID_BASE			0x00980900
/*  IDs reserved for driver specific controls */
#define V4L2_CID_PRIVATE_BASE		0x08000000

#define V4L2_CID_BRIGHTNESS		(V4L2_CID_BASE+0)
#define V4L2_CID_CONTRAST		(V4L2_CID_BASE+1)
#define V4L2_CID_SATURATION		(V4L2_CID_BASE+2)
#define V4L2_CID_HUE			(V4L2_CID_BASE+3)
#define V4L2_CID_AUDIO_VOLUME		(V4L2_CID_BASE+5)
#define V4L2_CID_AUDIO_BALANCE		(V4L2_CID_BASE+6)
#define V4L2_CID_AUDIO_BASS		(V4L2_CID_BASE+7)
#define V4L2_CID_AUDIO_TREBLE		(V4L2_CID_BASE+8)
#define V4L2_CID_AUDIO_MUTE		(V4L2_CID_BASE+9)
#define V4L2_CID_AUDIO_LOUDNESS		(V4L2_CID_BASE+10)
#define V4L2_CID_BLACK_LEVEL		(V4L2_CID_BASE+11)
#define V4L2_CID_AUTO_WHITE_BALANCE	(V4L2_CID_BASE+12)
#define V4L2_CID_DO_WHITE_BALANCE	(V4L2_CID_BASE+13)
#define V4L2_CID_RED_BALANCE		(V4L2_CID_BASE+14)
#define V4L2_CID_BLUE_BALANCE		(V4L2_CID_BASE+15)
#define V4L2_CID_GAMMA			(V4L2_CID_BASE+16)
#define V4L2_CID_WHITENESS		(V4L2_CID_GAMMA) /* ? Not sure */
#define V4L2_CID_EXPOSURE		(V4L2_CID_BASE+17)
#define V4L2_CID_AUTOGAIN		(V4L2_CID_BASE+18)
#define V4L2_CID_GAIN			(V4L2_CID_BASE+19)
#define V4L2_CID_HFLIP			(V4L2_CID_BASE+20)
#define V4L2_CID_VFLIP			(V4L2_CID_BASE+21)
#define V4L2_CID_HCENTER		(V4L2_CID_BASE+22)
#define V4L2_CID_VCENTER		(V4L2_CID_BASE+23)
#define V4L2_CID_LASTP1			(V4L2_CID_BASE+24) /* last CID + 1 */

/*
 *	T U N I N G
 */
struct v4l2_tuner
{
	__u32                   index;
	__u8			name[32];
	enum v4l2_tuner_type    type;
	__u32			capability;
	__u32			rangelow;
	__u32			rangehigh;
	__u32			rxsubchans;
	__u32			audmode;
	__s32			signal;
	__s32			afc;
	__u32			reserved[4];
};

struct v4l2_modulator
{
	__u32			index;
	__u8			name[32];
	__u32			capability;
	__u32			rangelow;
	__u32			rangehigh;
	__u32			txsubchans;
	__u32			reserved[4];
};

/*  Flags for the 'capability' field */
#define V4L2_TUNER_CAP_LOW		0x0001
#define V4L2_TUNER_CAP_NORM		0x0002
#define V4L2_TUNER_CAP_STEREO		0x0010
#define V4L2_TUNER_CAP_LANG2		0x0020
#define V4L2_TUNER_CAP_SAP		0x0020
#define V4L2_TUNER_CAP_LANG1		0x0040

/*  Flags for the 'rxsubchans' field */
#define V4L2_TUNER_SUB_MONO		0x0001
#define V4L2_TUNER_SUB_STEREO		0x0002
#define V4L2_TUNER_SUB_LANG2		0x0004
#define V4L2_TUNER_SUB_SAP		0x0004
#define V4L2_TUNER_SUB_LANG1		0x0008

/*  Values for the 'audmode' field */
#define V4L2_TUNER_MODE_MONO		0x0000
#define V4L2_TUNER_MODE_STEREO		0x0001
#define V4L2_TUNER_MODE_LANG2		0x0002
#define V4L2_TUNER_MODE_SAP		0x0002
#define V4L2_TUNER_MODE_LANG1		0x0003

struct v4l2_frequency
{
	__u32	              tuner;
	enum v4l2_tuner_type  type;
        __u32	              frequency;
	__u32	              reserved[8];
};

/*
 *	A U D I O
 */
struct v4l2_audio
{
	__u32	index;
	__u8	name[32];
	__u32	capability;
	__u32	mode;
	__u32	reserved[2];
};
/*  Flags for the 'capability' field */
#define V4L2_AUDCAP_STEREO		0x00001
#define V4L2_AUDCAP_AVL			0x00002

/*  Flags for the 'mode' field */
#define V4L2_AUDMODE_AVL		0x00001

struct v4l2_audioout
{
	__u32	index;
	__u8	name[32];
	__u32	capability;
	__u32	mode;
	__u32	reserved[2];
};

/*
 *	D A T A   S E R V I C E S   ( V B I )
 *
 *	Data services API by Michael Schimek
 */

struct v4l2_vbi_format
{
	__u32	sampling_rate;		/* in 1 Hz */
	__u32	offset;
	__u32	samples_per_line;
	__u32	sample_format;		/* V4L2_PIX_FMT_* */
	__s32	start[2];
	__u32	count[2];
	__u32	flags;			/* V4L2_VBI_* */
	__u32	reserved[2];		/* must be zero */
};

/*  VBI flags  */
#define V4L2_VBI_UNSYNC		(1<< 0)
#define V4L2_VBI_INTERLACED	(1<< 1)


/*
 *	A G G R E G A T E   S T R U C T U R E S
 */

/*	Stream data format
 */
struct v4l2_format
{
	enum v4l2_buf_type type;
	union
	{
		struct v4l2_pix_format	pix;  // V4L2_BUF_TYPE_VIDEO_CAPTURE
		struct v4l2_window	win;  // V4L2_BUF_TYPE_VIDEO_OVERLAY
		struct v4l2_vbi_format	vbi;  // V4L2_BUF_TYPE_VBI_CAPTURE
		__u8	raw_data[200];        // user-defined
	} fmt;
};


/*	Stream type-dependent parameters
 */
struct v4l2_streamparm
{
	enum v4l2_buf_type type;
	union
	{
		struct v4l2_captureparm	capture;
		struct v4l2_outputparm	output;
		__u8	raw_data[200];  /* user-defined */
	} parm;
};



/*
 *	I O C T L   C O D E S   F O R   V I D E O   D E V I C E S
 *
 */
#define VIDIOC_QUERYCAP		_IOR  ('V',  0, struct v4l2_capability)
#define VIDIOC_RESERVED		_IO   ('V',  1)
#define VIDIOC_ENUM_FMT         _IOWR ('V',  2, struct v4l2_fmtdesc)
#define VIDIOC_G_FMT		_IOWR ('V',  4, struct v4l2_format)
#define VIDIOC_S_FMT		_IOWR ('V',  5, struct v4l2_format)
#if 0
#define VIDIOC_G_COMP		_IOR  ('V',  6, struct v4l2_compression)
#define VIDIOC_S_COMP		_IOW  ('V',  7, struct v4l2_compression)
#endif
#define VIDIOC_REQBUFS		_IOWR ('V',  8, struct v4l2_requestbuffers)
#define VIDIOC_QUERYBUF		_IOWR ('V',  9, struct v4l2_buffer)
#define VIDIOC_G_FBUF		_IOR  ('V', 10, struct v4l2_framebuffer)
#define VIDIOC_S_FBUF		_IOW  ('V', 11, struct v4l2_framebuffer)
#define VIDIOC_OVERLAY		_IOW  ('V', 14, int)
#define VIDIOC_QBUF		_IOWR ('V', 15, struct v4l2_buffer)
#define VIDIOC_DQBUF		_IOWR ('V', 17, struct v4l2_buffer)
#define VIDIOC_STREAMON		_IOW  ('V', 18, int)
#define VIDIOC_STREAMOFF	_IOW  ('V', 19, int)
#define VIDIOC_G_PARM		_IOWR ('V', 21, struct v4l2_streamparm)
#define VIDIOC_S_PARM		_IOWR ('V', 22, struct v4l2_streamparm)
#define VIDIOC_G_STD		_IOR  ('V', 23, v4l2_std_id)
#define VIDIOC_S_STD		_IOW  ('V', 24, v4l2_std_id)
#define VIDIOC_ENUMSTD		_IOWR ('V', 25, struct v4l2_standard)
#define VIDIOC_ENUMINPUT	_IOWR ('V', 26, struct v4l2_input)
#define VIDIOC_G_CTRL		_IOWR ('V', 27, struct v4l2_control)
#define VIDIOC_S_CTRL		_IOWR ('V', 28, struct v4l2_control)
#define VIDIOC_G_TUNER		_IOWR ('V', 29, struct v4l2_tuner)
#define VIDIOC_S_TUNER		_IOW  ('V', 30, struct v4l2_tuner)
#define VIDIOC_G_AUDIO		_IOR  ('V', 33, struct v4l2_audio)
#define VIDIOC_S_AUDIO		_IOW  ('V', 34, struct v4l2_audio)
#define VIDIOC_QUERYCTRL	_IOWR ('V', 36, struct v4l2_queryctrl)
#define VIDIOC_QUERYMENU	_IOWR ('V', 37, struct v4l2_querymenu)
#define VIDIOC_G_INPUT		_IOR  ('V', 38, int)
#define VIDIOC_S_INPUT		_IOWR ('V', 39, int)
#define VIDIOC_G_OUTPUT		_IOR  ('V', 46, int)
#define VIDIOC_S_OUTPUT		_IOWR ('V', 47, int)
#define VIDIOC_ENUMOUTPUT	_IOWR ('V', 48, struct v4l2_output)
#define VIDIOC_G_AUDOUT		_IOR  ('V', 49, struct v4l2_audioout)
#define VIDIOC_S_AUDOUT		_IOW  ('V', 50, struct v4l2_audioout)
#define VIDIOC_G_MODULATOR	_IOWR ('V', 54, struct v4l2_modulator)
#define VIDIOC_S_MODULATOR	_IOW  ('V', 55, struct v4l2_modulator)
#define VIDIOC_G_FREQUENCY	_IOWR ('V', 56, struct v4l2_frequency)
#define VIDIOC_S_FREQUENCY	_IOW  ('V', 57, struct v4l2_frequency)
#define VIDIOC_CROPCAP		_IOWR ('V', 58, struct v4l2_cropcap)
#define VIDIOC_G_CROP		_IOWR ('V', 59, struct v4l2_crop)
#define VIDIOC_S_CROP		_IOW  ('V', 60, struct v4l2_crop)
#define VIDIOC_G_JPEGCOMP	_IOR  ('V', 61, struct v4l2_jpegcompression)
#define VIDIOC_S_JPEGCOMP	_IOW  ('V', 62, struct v4l2_jpegcompression)
#define VIDIOC_QUERYSTD      	_IOR  ('V', 63, v4l2_std_id)
#define VIDIOC_TRY_FMT      	_IOWR ('V', 64, struct v4l2_format)
#define VIDIOC_ENUMAUDIO	_IOWR ('V', 65, struct v4l2_audio)
#define VIDIOC_ENUMAUDOUT	_IOWR ('V', 66, struct v4l2_audioout)
#define VIDIOC_G_PRIORITY       _IOR  ('V', 67, enum v4l2_priority)
#define VIDIOC_S_PRIORITY       _IOW  ('V', 68, enum v4l2_priority)

/* for compatibility, will go away some day */
#define VIDIOC_OVERLAY_OLD     	_IOWR ('V', 14, int)
#define VIDIOC_S_PARM_OLD      	_IOW  ('V', 22, struct v4l2_streamparm)
#define VIDIOC_S_CTRL_OLD      	_IOW  ('V', 28, struct v4l2_control)
#define VIDIOC_G_AUDIO_OLD     	_IOWR ('V', 33, struct v4l2_audio)
#define VIDIOC_G_AUDOUT_OLD    	_IOWR ('V', 49, struct v4l2_audioout)
#define VIDIOC_CROPCAP_OLD     	_IOR  ('V', 58, struct v4l2_cropcap)

#define BASE_VIDIOC_PRIVATE	192		/* 192-255 are private */


#ifdef __KERNEL__
/*
 *
 *	V 4 L 2   D R I V E R   H E L P E R   A P I
 *
 *	Some commonly needed functions for drivers (v4l2-common.o module)
 */
#include <linux/fs.h>

/*  Video standard functions  */
extern unsigned int v4l2_video_std_fps(struct v4l2_standard *vs);
extern int v4l2_video_std_construct(struct v4l2_standard *vs,
				    int id, char *name);

/* prority handling */
struct v4l2_prio_state {
	atomic_t prios[4];
};
int v4l2_prio_init(struct v4l2_prio_state *global);
int v4l2_prio_change(struct v4l2_prio_state *global, enum v4l2_priority *local,
		     enum v4l2_priority new);
int v4l2_prio_open(struct v4l2_prio_state *global, enum v4l2_priority *local);
int v4l2_prio_close(struct v4l2_prio_state *global, enum v4l2_priority *local);
enum v4l2_priority v4l2_prio_max(struct v4l2_prio_state *global);
int v4l2_prio_check(struct v4l2_prio_state *global, enum v4l2_priority *local);

/* names for fancy debug output */
extern char *v4l2_field_names[];
extern char *v4l2_type_names[];
extern char *v4l2_ioctl_names[];

/*  Compatibility layer interface  --  v4l1-compat module */
typedef int (*v4l2_kioctl)(struct inode *inode, struct file *file,
			   unsigned int cmd, void *arg);
int v4l_compat_translate_ioctl(struct inode *inode, struct file *file,
			       int cmd, void *arg, v4l2_kioctl driver_ioctl);

#endif /* __KERNEL__ */
#endif /* __LINUX_VIDEODEV2_H */

/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
