/*
 * cpia CPiA driver
 *
 * Supports CPiA based Video Camera's.
 *
 * (C) Copyright 1999-2000 Peter Pregler,
 * (C) Copyright 1999-2000 Scott J. Bertin,
 * (C) Copyright 1999-2000 Johannes Erdfelt, jerdfelt@valinux.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* #define _CPIA_DEBUG_		define for verbose debug output */
#include <linux/config.h>

#include <linux/module.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/ctype.h>
#include <linux/pagemap.h>
#include <asm/io.h>
#include <asm/semaphore.h>
#include <linux/wrapper.h>

#ifdef CONFIG_KMOD
#include <linux/kmod.h>
#endif

#include "cpia.h"

#ifdef CONFIG_VIDEO_CPIA_PP
extern int cpia_pp_init(void);
#endif
#ifdef CONFIG_VIDEO_CPIA_USB
extern int cpia_usb_init(void);
#endif

#ifdef MODULE
MODULE_AUTHOR("Scott J. Bertin <sbertin@mindspring.com> & Peter Pregler <Peter_Pregler@email.com> & Johannes Erdfelt <jerdfelt@valinux.com>");
MODULE_DESCRIPTION("V4L-driver for Vision CPiA based cameras");
MODULE_SUPPORTED_DEVICE("video");
#endif

#define ABOUT "V4L-Driver for Vision CPiA based cameras"

#ifndef VID_HARDWARE_CPIA
#define VID_HARDWARE_CPIA 24    /* FIXME -> from linux/videodev.h */
#endif

#define CPIA_MODULE_CPIA			(0<<5)
#define CPIA_MODULE_SYSTEM			(1<<5)
#define CPIA_MODULE_VP_CTRL			(5<<5)
#define CPIA_MODULE_CAPTURE			(6<<5)
#define CPIA_MODULE_DEBUG			(7<<5)

#define INPUT (DATA_IN << 8)
#define OUTPUT (DATA_OUT << 8)

#define CPIA_COMMAND_GetCPIAVersion	(INPUT | CPIA_MODULE_CPIA | 1)
#define CPIA_COMMAND_GetPnPID		(INPUT | CPIA_MODULE_CPIA | 2)
#define CPIA_COMMAND_GetCameraStatus	(INPUT | CPIA_MODULE_CPIA | 3)
#define CPIA_COMMAND_GotoHiPower	(OUTPUT | CPIA_MODULE_CPIA | 4)
#define CPIA_COMMAND_GotoLoPower	(OUTPUT | CPIA_MODULE_CPIA | 5)
#define CPIA_COMMAND_GotoSuspend	(OUTPUT | CPIA_MODULE_CPIA | 7)
#define CPIA_COMMAND_GotoPassThrough	(OUTPUT | CPIA_MODULE_CPIA | 8)
#define CPIA_COMMAND_ModifyCameraStatus	(OUTPUT | CPIA_MODULE_CPIA | 10)

#define CPIA_COMMAND_ReadVCRegs		(INPUT | CPIA_MODULE_SYSTEM | 1)
#define CPIA_COMMAND_WriteVCReg		(OUTPUT | CPIA_MODULE_SYSTEM | 2)
#define CPIA_COMMAND_ReadMCPorts	(INPUT | CPIA_MODULE_SYSTEM | 3)
#define CPIA_COMMAND_WriteMCPort	(OUTPUT | CPIA_MODULE_SYSTEM | 4)
#define CPIA_COMMAND_SetBaudRate	(OUTPUT | CPIA_MODULE_SYSTEM | 5)
#define CPIA_COMMAND_SetECPTiming	(OUTPUT | CPIA_MODULE_SYSTEM | 6)
#define CPIA_COMMAND_ReadIDATA		(INPUT | CPIA_MODULE_SYSTEM | 7)
#define CPIA_COMMAND_WriteIDATA		(OUTPUT | CPIA_MODULE_SYSTEM | 8)
#define CPIA_COMMAND_GenericCall	(OUTPUT | CPIA_MODULE_SYSTEM | 9)
#define CPIA_COMMAND_I2CStart		(OUTPUT | CPIA_MODULE_SYSTEM | 10)
#define CPIA_COMMAND_I2CStop		(OUTPUT | CPIA_MODULE_SYSTEM | 11)
#define CPIA_COMMAND_I2CWrite		(OUTPUT | CPIA_MODULE_SYSTEM | 12)
#define CPIA_COMMAND_I2CRead		(INPUT | CPIA_MODULE_SYSTEM | 13)

#define CPIA_COMMAND_GetVPVersion	(INPUT | CPIA_MODULE_VP_CTRL | 1)
#define CPIA_COMMAND_SetColourParams	(OUTPUT | CPIA_MODULE_VP_CTRL | 3)
#define CPIA_COMMAND_SetExposure	(OUTPUT | CPIA_MODULE_VP_CTRL | 4)
#define CPIA_COMMAND_SetColourBalance	(OUTPUT | CPIA_MODULE_VP_CTRL | 6)
#define CPIA_COMMAND_SetSensorFPS	(OUTPUT | CPIA_MODULE_VP_CTRL | 7)
#define CPIA_COMMAND_SetVPDefaults	(OUTPUT | CPIA_MODULE_VP_CTRL | 8)
#define CPIA_COMMAND_SetApcor		(OUTPUT | CPIA_MODULE_VP_CTRL | 9)
#define CPIA_COMMAND_SetFlickerCtrl	(OUTPUT | CPIA_MODULE_VP_CTRL | 10)
#define CPIA_COMMAND_SetVLOffset	(OUTPUT | CPIA_MODULE_VP_CTRL | 11)
#define CPIA_COMMAND_GetColourParams	(INPUT | CPIA_MODULE_VP_CTRL | 16)
#define CPIA_COMMAND_GetColourBalance	(INPUT | CPIA_MODULE_VP_CTRL | 17)
#define CPIA_COMMAND_GetExposure	(INPUT | CPIA_MODULE_VP_CTRL | 18)
#define CPIA_COMMAND_SetSensorMatrix	(OUTPUT | CPIA_MODULE_VP_CTRL | 19)
#define CPIA_COMMAND_ColourBars		(OUTPUT | CPIA_MODULE_VP_CTRL | 25)
#define CPIA_COMMAND_ReadVPRegs		(INPUT | CPIA_MODULE_VP_CTRL | 30)
#define CPIA_COMMAND_WriteVPReg		(OUTPUT | CPIA_MODULE_VP_CTRL | 31)

#define CPIA_COMMAND_GrabFrame		(OUTPUT | CPIA_MODULE_CAPTURE | 1)
#define CPIA_COMMAND_UploadFrame	(OUTPUT | CPIA_MODULE_CAPTURE | 2)
#define CPIA_COMMAND_SetGrabMode	(OUTPUT | CPIA_MODULE_CAPTURE | 3)
#define CPIA_COMMAND_InitStreamCap	(OUTPUT | CPIA_MODULE_CAPTURE | 4)
#define CPIA_COMMAND_FiniStreamCap	(OUTPUT | CPIA_MODULE_CAPTURE | 5)
#define CPIA_COMMAND_StartStreamCap	(OUTPUT | CPIA_MODULE_CAPTURE | 6)
#define CPIA_COMMAND_EndStreamCap	(OUTPUT | CPIA_MODULE_CAPTURE | 7)
#define CPIA_COMMAND_SetFormat		(OUTPUT | CPIA_MODULE_CAPTURE | 8)
#define CPIA_COMMAND_SetROI		(OUTPUT | CPIA_MODULE_CAPTURE | 9)
#define CPIA_COMMAND_SetCompression	(OUTPUT | CPIA_MODULE_CAPTURE | 10)
#define CPIA_COMMAND_SetCompressionTarget (OUTPUT | CPIA_MODULE_CAPTURE | 11)
#define CPIA_COMMAND_SetYUVThresh	(OUTPUT | CPIA_MODULE_CAPTURE | 12)
#define CPIA_COMMAND_SetCompressionParams (OUTPUT | CPIA_MODULE_CAPTURE | 13)
#define CPIA_COMMAND_DiscardFrame	(OUTPUT | CPIA_MODULE_CAPTURE | 14)

#define CPIA_COMMAND_OutputRS232	(OUTPUT | CPIA_MODULE_DEBUG | 1)
#define CPIA_COMMAND_AbortProcess	(OUTPUT | CPIA_MODULE_DEBUG | 4)
#define CPIA_COMMAND_SetDramPage	(OUTPUT | CPIA_MODULE_DEBUG | 5)
#define CPIA_COMMAND_StartDramUpload	(OUTPUT | CPIA_MODULE_DEBUG | 6)
#define CPIA_COMMAND_StartDummyDtream	(OUTPUT | CPIA_MODULE_DEBUG | 8)
#define CPIA_COMMAND_AbortStream	(OUTPUT | CPIA_MODULE_DEBUG | 9)
#define CPIA_COMMAND_DownloadDRAM	(OUTPUT | CPIA_MODULE_DEBUG | 10)

enum {
	FRAME_READY,		/* Ready to grab into */
	FRAME_GRABBING,		/* In the process of being grabbed into */
	FRAME_DONE,		/* Finished grabbing, but not been synced yet */
	FRAME_UNUSED,		/* Unused (no MCAPTURE) */
};

#define COMMAND_NONE			0x0000
#define COMMAND_SETCOMPRESSION		0x0001
#define COMMAND_SETCOMPRESSIONTARGET	0x0002
#define COMMAND_SETCOLOURPARAMS		0x0004
#define COMMAND_SETFORMAT		0x0008
#define COMMAND_PAUSE			0x0010
#define COMMAND_RESUME			0x0020
#define COMMAND_SETYUVTHRESH		0x0040
#define COMMAND_SETECPTIMING		0x0080
#define COMMAND_SETCOMPRESSIONPARAMS	0x0100
#define COMMAND_SETEXPOSURE		0x0200
#define COMMAND_SETCOLOURBALANCE	0x0400
#define COMMAND_SETSENSORFPS		0x0800
#define COMMAND_SETAPCOR		0x1000
#define COMMAND_SETFLICKERCTRL		0x2000
#define COMMAND_SETVLOFFSET		0x4000

/* Developer's Guide Table 5 p 3-34
 * indexed by [mains][sensorFps.baserate][sensorFps.divisor]*/
static u8 flicker_jumps[2][2][4] =
{ { { 76, 38, 19, 9 }, { 92, 46, 23, 11 } },
  { { 64, 32, 16, 8 }, { 76, 38, 19, 9} }
};

/* forward declaration of local function */
static void reset_camera_struct(struct cam_data *cam);

/**********************************************************************
 *
 * Memory management
 *
 * This is a shameless copy from the USB-cpia driver (linux kernel
 * version 2.3.29 or so, I have no idea what this code actually does ;).
 * Actually it seems to be a copy of a shameless copy of the bttv-driver.
 * Or that is a copy of a shameless copy of ... (To the powers: is there
 * no generic kernel-function to do this sort of stuff?)
 *
 * Yes, it was a shameless copy from the bttv-driver. IIRC, Alan says
 * there will be one, but apparentely not yet - jerdfelt
 *
 **********************************************************************/

/* Given PGD from the address space's page table, return the kernel
 * virtual mapping of the physical memory mapped at ADR.
 */
static inline unsigned long uvirt_to_kva(pgd_t *pgd, unsigned long adr)
{
	unsigned long ret = 0UL;
	pmd_t *pmd;
	pte_t *ptep, pte;

	if (!pgd_none(*pgd)) {
		pmd = pmd_offset(pgd, adr);
		if (!pmd_none(*pmd)) {
			ptep = pte_offset(pmd, adr);
			pte = *ptep;
			if (pte_present(pte)) {
				ret = (unsigned long) page_address(pte_page(pte));
				ret |= (adr & (PAGE_SIZE-1));
			}
		}
	}
	return ret;
}

/* Here we want the physical address of the memory.
 * This is used when initializing the contents of the
 * area and marking the pages as reserved.
 */
static inline unsigned long kvirt_to_pa(unsigned long adr)
{
	unsigned long va, kva, ret;

	va = VMALLOC_VMADDR(adr);
	kva = uvirt_to_kva(pgd_offset_k(va), va);
	ret = __pa(kva);
	return ret;
}

static void *rvmalloc(unsigned long size)
{
	void *mem;
	unsigned long adr, page;

	/* Round it off to PAGE_SIZE */
	size += (PAGE_SIZE - 1);
	size &= ~(PAGE_SIZE - 1);

	mem = vmalloc_32(size);
	if (!mem)
		return NULL;

	memset(mem, 0, size); /* Clear the ram out, no junk to the user */
	adr = (unsigned long) mem;
	while (size > 0) {
		page = kvirt_to_pa(adr);
		mem_map_reserve(virt_to_page(__va(page)));
		adr += PAGE_SIZE;
		if (size > PAGE_SIZE)
			size -= PAGE_SIZE;
		else
			size = 0;
	}

	return mem;
}

static void rvfree(void *mem, unsigned long size)
{
	unsigned long adr, page;

	if (!mem)
		return;

	size += (PAGE_SIZE - 1);
	size &= ~(PAGE_SIZE - 1);

	adr = (unsigned long) mem;
	while (size > 0) {
		page = kvirt_to_pa(adr);
		mem_map_unreserve(virt_to_page(__va(page)));
		adr += PAGE_SIZE;
		if (size > PAGE_SIZE)
			size -= PAGE_SIZE;
		else
			size = 0;
	}
	vfree(mem);
}

/**********************************************************************
 *
 * /proc interface
 *
 **********************************************************************/
#ifdef CONFIG_PROC_FS
static struct proc_dir_entry *cpia_proc_root=NULL;

static int cpia_read_proc(char *page, char **start, off_t off,
                          int count, int *eof, void *data)
{
	char *out = page;
	int len, tmp;
	struct cam_data *cam = data;
	char tmpstr[20];

	/* IMPORTANT: This output MUST be kept under PAGE_SIZE
	 *            or we need to get more sophisticated. */

	out += sprintf(out, "read-only\n-----------------------\n");
	out += sprintf(out, "V4L Driver version:       %d.%d.%d\n",
		       CPIA_MAJ_VER, CPIA_MIN_VER, CPIA_PATCH_VER);
	out += sprintf(out, "CPIA Version:             %d.%02d (%d.%d)\n",
	               cam->params.version.firmwareVersion,
	               cam->params.version.firmwareRevision,
	               cam->params.version.vcVersion,
	               cam->params.version.vcRevision);
	out += sprintf(out, "CPIA PnP-ID:              %04x:%04x:%04x\n",
	               cam->params.pnpID.vendor, cam->params.pnpID.product,
	               cam->params.pnpID.deviceRevision);
	out += sprintf(out, "VP-Version:               %d.%d %04x\n",
	               cam->params.vpVersion.vpVersion,
	               cam->params.vpVersion.vpRevision,
	               cam->params.vpVersion.cameraHeadID);
	
	out += sprintf(out, "system_state:             %#04x\n",
	               cam->params.status.systemState);
	out += sprintf(out, "grab_state:               %#04x\n",
	               cam->params.status.grabState);
	out += sprintf(out, "stream_state:             %#04x\n",
	               cam->params.status.streamState);
	out += sprintf(out, "fatal_error:              %#04x\n",
	               cam->params.status.fatalError);
	out += sprintf(out, "cmd_error:                %#04x\n",
	               cam->params.status.cmdError);
	out += sprintf(out, "debug_flags:              %#04x\n",
	               cam->params.status.debugFlags);
	out += sprintf(out, "vp_status:                %#04x\n",
	               cam->params.status.vpStatus);
	out += sprintf(out, "error_code:               %#04x\n",
	               cam->params.status.errorCode);
	out += sprintf(out, "video_size:               %s\n",
	               cam->params.format.videoSize == VIDEOSIZE_CIF ?
		       "CIF " : "QCIF");
	out += sprintf(out, "sub_sample:               %s\n",
	               cam->params.format.subSample == SUBSAMPLE_420 ?
		       "420" : "422");
	out += sprintf(out, "yuv_order:                %s\n",
	               cam->params.format.yuvOrder == YUVORDER_YUYV ?
		       "YUYV" : "UYVY");
	out += sprintf(out, "roi:                      (%3d, %3d) to (%3d, %3d)\n",
	               cam->params.roi.colStart*8,
	               cam->params.roi.rowStart*4,
	               cam->params.roi.colEnd*8,
	               cam->params.roi.rowEnd*4);
	out += sprintf(out, "actual_fps:               %3d\n", cam->fps);
	out += sprintf(out, "transfer_rate:            %4dkB/s\n",
	               cam->transfer_rate);
	
	out += sprintf(out, "\nread-write\n");
	out += sprintf(out, "-----------------------  current       min"
	               "       max   default  comment\n");
	out += sprintf(out, "brightness:             %8d  %8d  %8d  %8d\n",
	               cam->params.colourParams.brightness, 0, 100, 50);
	if (cam->params.version.firmwareVersion == 1 &&
	   cam->params.version.firmwareRevision == 2)
		/* 1-02 firmware limits contrast to 80 */
		tmp = 80;
	else
		tmp = 96;

	out += sprintf(out, "contrast:               %8d  %8d  %8d  %8d"
	               "  steps of 8\n",
	               cam->params.colourParams.contrast, 0, tmp, 48);
	out += sprintf(out, "saturation:             %8d  %8d  %8d  %8d\n",
	               cam->params.colourParams.saturation, 0, 100, 50);
	tmp = (25000+5000*cam->params.sensorFps.baserate)/
	      (1<<cam->params.sensorFps.divisor);
	out += sprintf(out, "sensor_fps:             %4d.%03d  %8d  %8d  %8d\n",
	               tmp/1000, tmp%1000, 3, 30, 15);
	out += sprintf(out, "stream_start_line:      %8d  %8d  %8d  %8d\n",
	               2*cam->params.streamStartLine, 0,
		       cam->params.format.videoSize == VIDEOSIZE_CIF ? 288:144,
		       cam->params.format.videoSize == VIDEOSIZE_CIF ? 240:120);
	out += sprintf(out, "ecp_timing:             %8s  %8s  %8s  %8s\n",
	               cam->params.ecpTiming ? "slow" : "normal", "slow",
		       "normal", "normal");

	if (cam->params.colourBalance.balanceModeIsAuto) {
		sprintf(tmpstr, "auto");
	} else {
		sprintf(tmpstr, "manual");
	}
	out += sprintf(out, "color_balance_mode:     %8s  %8s  %8s"
		       "  %8s\n",  tmpstr, "manual", "auto", "auto");
	out += sprintf(out, "red_gain:               %8d  %8d  %8d  %8d\n",
	               cam->params.colourBalance.redGain, 0, 212, 32);
	out += sprintf(out, "green_gain:             %8d  %8d  %8d  %8d\n",
	               cam->params.colourBalance.greenGain, 0, 212, 6);
	out += sprintf(out, "blue_gain:              %8d  %8d  %8d  %8d\n",
	               cam->params.colourBalance.blueGain, 0, 212, 92);

	if (cam->params.version.firmwareVersion == 1 &&
	   cam->params.version.firmwareRevision == 2)
		/* 1-02 firmware limits gain to 2 */
		sprintf(tmpstr, "%8d  %8d", 1, 2);
	else
		sprintf(tmpstr, "1,2,4,8");

	if (cam->params.exposure.gainMode == 0)
		out += sprintf(out, "max_gain:                unknown  %18s"
		               "  %8d\n", tmpstr, 2); 
	else
		out += sprintf(out, "max_gain:               %8d  %18s  %8d\n", 
		               1<<(cam->params.exposure.gainMode-1), tmpstr, 2);

	switch(cam->params.exposure.expMode) {
	case 1:
	case 3:
		sprintf(tmpstr, "manual");
		break;
	case 2:
		sprintf(tmpstr, "auto");
		break;
	default:
		sprintf(tmpstr, "unknown");
		break;
	}
	out += sprintf(out, "exposure_mode:          %8s  %8s  %8s"
		       "  %8s\n",  tmpstr, "manual", "auto", "auto");
	out += sprintf(out, "centre_weight:          %8s  %8s  %8s  %8s\n",
	               (2-cam->params.exposure.centreWeight) ? "on" : "off",
	               "off", "on", "on");
	out += sprintf(out, "gain:                   %8d  %8d  max_gain  %8d  1,2,4,8 possible\n",
	               1<<cam->params.exposure.gain, 1, 1);
	if (cam->params.version.firmwareVersion == 1 &&
	   cam->params.version.firmwareRevision == 2)
		/* 1-02 firmware limits fineExp to 127 */
		tmp = 255;
	else
		tmp = 511;

	out += sprintf(out, "fine_exp:               %8d  %8d  %8d  %8d\n",
	               cam->params.exposure.fineExp*2, 0, tmp, 0);
	if (cam->params.version.firmwareVersion == 1 &&
	   cam->params.version.firmwareRevision == 2)
		/* 1-02 firmware limits coarseExpHi to 0 */
		tmp = 255;
	else
		tmp = 65535;

	out += sprintf(out, "coarse_exp:             %8d  %8d  %8d"
		       "  %8d\n", cam->params.exposure.coarseExpLo+
		       256*cam->params.exposure.coarseExpHi, 0, tmp, 185);
	out += sprintf(out, "red_comp:               %8d  %8d  %8d  %8d\n",
	               cam->params.exposure.redComp, 220, 255, 220);
	out += sprintf(out, "green1_comp:            %8d  %8d  %8d  %8d\n",
	               cam->params.exposure.green1Comp, 214, 255, 214);
	out += sprintf(out, "green2_comp:            %8d  %8d  %8d  %8d\n",
	               cam->params.exposure.green2Comp, 214, 255, 214);
	out += sprintf(out, "blue_comp:              %8d  %8d  %8d  %8d\n",
	               cam->params.exposure.blueComp, 230, 255, 230);
	
	out += sprintf(out, "apcor_gain1:            %#8x  %#8x  %#8x  %#8x\n",
	               cam->params.apcor.gain1, 0, 0xff, 0x1c);
	out += sprintf(out, "apcor_gain2:            %#8x  %#8x  %#8x  %#8x\n",
	               cam->params.apcor.gain2, 0, 0xff, 0x1a);
	out += sprintf(out, "apcor_gain4:            %#8x  %#8x  %#8x  %#8x\n",
	               cam->params.apcor.gain4, 0, 0xff, 0x2d);
	out += sprintf(out, "apcor_gain8:            %#8x  %#8x  %#8x  %#8x\n",
	               cam->params.apcor.gain8, 0, 0xff, 0x2a);
	out += sprintf(out, "vl_offset_gain1:        %8d  %8d  %8d  %8d\n",
	               cam->params.vlOffset.gain1, 0, 255, 24);
	out += sprintf(out, "vl_offset_gain2:        %8d  %8d  %8d  %8d\n",
	               cam->params.vlOffset.gain2, 0, 255, 28);
	out += sprintf(out, "vl_offset_gain4:        %8d  %8d  %8d  %8d\n",
	               cam->params.vlOffset.gain4, 0, 255, 30);
	out += sprintf(out, "vl_offset_gain8:        %8d  %8d  %8d  %8d\n",
	               cam->params.vlOffset.gain8, 0, 255, 30);
	out += sprintf(out, "flicker_control:        %8s  %8s  %8s  %8s\n",
	               cam->params.flickerControl.flickerMode ? "on" : "off",
		       "off", "on", "off");
	out += sprintf(out, "mains_frequency:        %8d  %8d  %8d  %8d"
	               " only 50/60\n",
	               cam->mainsFreq ? 60 : 50, 50, 60, 50);
	out += sprintf(out, "allowable_overexposure: %8d  %8d  %8d  %8d\n",
	               cam->params.flickerControl.allowableOverExposure, 0,
		       255, 0);
	out += sprintf(out, "compression_mode:       ");
	switch(cam->params.compression.mode) {
	case CPIA_COMPRESSION_NONE:
		out += sprintf(out, "%8s", "none");
		break;
	case CPIA_COMPRESSION_AUTO:
		out += sprintf(out, "%8s", "auto");
		break;
	case CPIA_COMPRESSION_MANUAL:
		out += sprintf(out, "%8s", "manual");
		break;
	default:
		out += sprintf(out, "%8s", "unknown");
		break;
	}
	out += sprintf(out, "    none,auto,manual      auto\n");
	out += sprintf(out, "decimation_enable:      %8s  %8s  %8s  %8s\n",
        	       cam->params.compression.decimation == 
		       DECIMATION_ENAB ? "on":"off", "off", "off",
		       "off");
	out += sprintf(out, "compression_target:    %9s %9s %9s %9s\n",
	               cam->params.compressionTarget.frTargeting  == 
		       CPIA_COMPRESSION_TARGET_FRAMERATE ?
		       "framerate":"quality",
		       "framerate", "quality", "quality");
	out += sprintf(out, "target_framerate:       %8d  %8d  %8d  %8d\n",
	               cam->params.compressionTarget.targetFR, 0, 30, 7);
	out += sprintf(out, "target_quality:         %8d  %8d  %8d  %8d\n",
	               cam->params.compressionTarget.targetQ, 0, 255, 10);
	out += sprintf(out, "y_threshold:            %8d  %8d  %8d  %8d\n",
	               cam->params.yuvThreshold.yThreshold, 0, 31, 15);
	out += sprintf(out, "uv_threshold:           %8d  %8d  %8d  %8d\n",
	               cam->params.yuvThreshold.uvThreshold, 0, 31, 15);
	out += sprintf(out, "hysteresis:             %8d  %8d  %8d  %8d\n",
	               cam->params.compressionParams.hysteresis, 0, 255, 3);
	out += sprintf(out, "threshold_max:          %8d  %8d  %8d  %8d\n",
	               cam->params.compressionParams.threshMax, 0, 255, 11);
	out += sprintf(out, "small_step:             %8d  %8d  %8d  %8d\n",
	               cam->params.compressionParams.smallStep, 0, 255, 1);
	out += sprintf(out, "large_step:             %8d  %8d  %8d  %8d\n",
	               cam->params.compressionParams.largeStep, 0, 255, 3);
	out += sprintf(out, "decimation_hysteresis:  %8d  %8d  %8d  %8d\n",
	               cam->params.compressionParams.decimationHysteresis,
		       0, 255, 2);
	out += sprintf(out, "fr_diff_step_thresh:    %8d  %8d  %8d  %8d\n",
	               cam->params.compressionParams.frDiffStepThresh,
		       0, 255, 5);
	out += sprintf(out, "q_diff_step_thresh:     %8d  %8d  %8d  %8d\n",
	               cam->params.compressionParams.qDiffStepThresh,
		       0, 255, 3);
	out += sprintf(out, "decimation_thresh_mod:  %8d  %8d  %8d  %8d\n",
	               cam->params.compressionParams.decimationThreshMod,
		       0, 255, 2);
	
	len = out - page;
	len -= off;
	if (len < count) {
		*eof = 1;
		if (len <= 0) return 0;
	} else
		len = count;

	*start = page + off;
	return len;
}

static int cpia_write_proc(struct file *file, const char *buffer,
                           unsigned long count, void *data)
{
	struct cam_data *cam = data;
	struct cam_params new_params;
	int retval, find_colon;
	int size = count;
	unsigned long val;
	u32 command_flags = 0;
	u8 new_mains;
	
	if (down_interruptible(&cam->param_lock))
		return -ERESTARTSYS;
	
	/*
	 * Skip over leading whitespace
	 */
	while (count && isspace(*buffer)) {
		--count;
		++buffer;
	}
	
	memcpy(&new_params, &cam->params, sizeof(struct cam_params));
	new_mains = cam->mainsFreq;
	
#define MATCH(x) \
	({ \
		int _len = strlen(x), _ret, _colon_found; \
		_ret = (_len <= count && strncmp(buffer, x, _len) == 0); \
		if (_ret) { \
			buffer += _len; \
			count -= _len; \
			if (find_colon) { \
				_colon_found = 0; \
				while (count && (*buffer == ' ' || *buffer == '\t' || \
				       (!_colon_found && *buffer == ':'))) { \
					if (*buffer == ':')  \
						_colon_found = 1; \
					--count; \
					++buffer; \
				} \
				if (!count || !_colon_found) \
					retval = -EINVAL; \
				find_colon = 0; \
			} \
		} \
		_ret; \
	})
#define FIRMWARE_VERSION(x,y) (new_params.version.firmwareVersion == (x) && \
                               new_params.version.firmwareRevision == (y))
#define VALUE \
	({ \
		char *_p; \
		unsigned long int _ret; \
		_ret = simple_strtoul(buffer, &_p, 0); \
		if (_p == buffer) \
			retval = -EINVAL; \
		else { \
			count -= _p - buffer; \
			buffer = _p; \
		} \
		_ret; \
	})

	retval = 0;
	while (count && !retval) {
		find_colon = 1;
		if (MATCH("brightness")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 100)
					new_params.colourParams.brightness = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETCOLOURPARAMS;
		} else if (MATCH("contrast")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 100) {
					/* contrast is in steps of 8, so round*/
					val = ((val + 3) / 8) * 8;
					/* 1-02 firmware limits contrast to 80*/
					if (FIRMWARE_VERSION(1,2) && val > 80)
						val = 80;

					new_params.colourParams.contrast = val;
				} else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETCOLOURPARAMS;
		} else if (MATCH("saturation")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 100)
					new_params.colourParams.saturation = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETCOLOURPARAMS;
		} else if (MATCH("sensor_fps")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				/* find values so that sensorFPS is minimized,
				 * but >= val */
				if (val > 30)
					retval = -EINVAL;
				else if (val > 25) {
					new_params.sensorFps.divisor = 0;
					new_params.sensorFps.baserate = 1;
				} else if (val > 15) {
					new_params.sensorFps.divisor = 0;
					new_params.sensorFps.baserate = 0;
				} else if (val > 12) {
					new_params.sensorFps.divisor = 1;
					new_params.sensorFps.baserate = 1;
				} else if (val > 7) {
					new_params.sensorFps.divisor = 1;
					new_params.sensorFps.baserate = 0;
				} else if (val > 6) {
					new_params.sensorFps.divisor = 2;
					new_params.sensorFps.baserate = 1;
				} else if (val > 3) {
					new_params.sensorFps.divisor = 2;
					new_params.sensorFps.baserate = 0;
				} else {
					new_params.sensorFps.divisor = 3;
					/* Either base rate would work here */
					new_params.sensorFps.baserate = 1;
				}
				new_params.flickerControl.coarseJump = 
					flicker_jumps[new_mains]
					[new_params.sensorFps.baserate]
					[new_params.sensorFps.divisor];
				if (new_params.flickerControl.flickerMode)
					command_flags |= COMMAND_SETFLICKERCTRL;
			}
			command_flags |= COMMAND_SETSENSORFPS;
		} else if (MATCH("stream_start_line")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				int max_line = 288;

				if (new_params.format.videoSize == VIDEOSIZE_QCIF)
					max_line = 144;
				if (val <= max_line)
					new_params.streamStartLine = val/2;
				else
					retval = -EINVAL;
			}
		} else if (MATCH("ecp_timing")) {
			if (!retval && MATCH("normal"))
				new_params.ecpTiming = 0;
			else if (!retval && MATCH("slow"))
				new_params.ecpTiming = 1;
			else
				retval = -EINVAL;

			command_flags |= COMMAND_SETECPTIMING;
		} else if (MATCH("color_balance_mode")) {
			if (!retval && MATCH("manual"))
				new_params.colourBalance.balanceModeIsAuto = 0;
			else if (!retval && MATCH("auto"))
				new_params.colourBalance.balanceModeIsAuto = 1;
			else
				retval = -EINVAL;

			command_flags |= COMMAND_SETCOLOURBALANCE;
		} else if (MATCH("red_gain")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 212)
					new_params.colourBalance.redGain = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETCOLOURBALANCE;
		} else if (MATCH("green_gain")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 212)
					new_params.colourBalance.greenGain = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETCOLOURBALANCE;
		} else if (MATCH("blue_gain")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 212)
					new_params.colourBalance.blueGain = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETCOLOURBALANCE;
		} else if (MATCH("max_gain")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				/* 1-02 firmware limits gain to 2 */
				if (FIRMWARE_VERSION(1,2) && val > 2)
					val = 2;
				switch(val) {
				case 1:
					new_params.exposure.gainMode = 1;
					break;
				case 2:
					new_params.exposure.gainMode = 2;
					break;
				case 4:
					new_params.exposure.gainMode = 3;
					break;
				case 8:
					new_params.exposure.gainMode = 4;
					break;
				default:
					retval = -EINVAL;
					break;
				}
			}
			command_flags |= COMMAND_SETEXPOSURE;
		} else if (MATCH("exposure_mode")) {
			if (!retval && MATCH("auto"))
				new_params.exposure.expMode = 2;
			else if (!retval && MATCH("manual")) {
				if (new_params.exposure.expMode == 2)
					new_params.exposure.expMode = 3;
				new_params.flickerControl.flickerMode = 0;
				command_flags |= COMMAND_SETFLICKERCTRL;
			} else
				retval = -EINVAL;

			command_flags |= COMMAND_SETEXPOSURE;
		} else if (MATCH("centre_weight")) {
			if (!retval && MATCH("on"))
				new_params.exposure.centreWeight = 1;
			else if (!retval && MATCH("off"))
				new_params.exposure.centreWeight = 2;
			else
				retval = -EINVAL;

			command_flags |= COMMAND_SETEXPOSURE;
		} else if (MATCH("gain")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				switch(val) {
				case 1:
					new_params.exposure.gain = 0;
					new_params.exposure.expMode = 1;
					new_params.flickerControl.flickerMode = 0;
					command_flags |= COMMAND_SETFLICKERCTRL;
					break;
				case 2:
					new_params.exposure.gain = 1;
					new_params.exposure.expMode = 1;
					new_params.flickerControl.flickerMode = 0;
					command_flags |= COMMAND_SETFLICKERCTRL;
					break;
				case 4:
					new_params.exposure.gain = 2;
					new_params.exposure.expMode = 1;
					new_params.flickerControl.flickerMode = 0;
					command_flags |= COMMAND_SETFLICKERCTRL;
					break;
				case 8:
					new_params.exposure.gain = 3;
					new_params.exposure.expMode = 1;
					new_params.flickerControl.flickerMode = 0;
					command_flags |= COMMAND_SETFLICKERCTRL;
					break;
				default:
					retval = -EINVAL;
					break;
				}
				command_flags |= COMMAND_SETEXPOSURE;
				if (new_params.exposure.gain >
				    new_params.exposure.gainMode-1)
					retval = -EINVAL;
			}
		} else if (MATCH("fine_exp")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val < 256) {
					/* 1-02 firmware limits fineExp to 127*/
					if (FIRMWARE_VERSION(1,2) && val > 127)
						val = 127;
					new_params.exposure.fineExp = val;
					new_params.exposure.expMode = 1;
					command_flags |= COMMAND_SETEXPOSURE;
					new_params.flickerControl.flickerMode = 0;
					command_flags |= COMMAND_SETFLICKERCTRL;
				} else
					retval = -EINVAL;
			}
		} else if (MATCH("coarse_exp")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val < 65536) {
					/* 1-02 firmware limits
					 * coarseExp to 255 */
					if (FIRMWARE_VERSION(1,2) && val > 255)
						val = 255;
					new_params.exposure.coarseExpLo =
						val & 0xff;
					new_params.exposure.coarseExpHi =
						val >> 8;
					new_params.exposure.expMode = 1;
					command_flags |= COMMAND_SETEXPOSURE;
					new_params.flickerControl.flickerMode = 0;
					command_flags |= COMMAND_SETFLICKERCTRL;
				} else
					retval = -EINVAL;
			}
		} else if (MATCH("red_comp")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val >= 220 && val <= 255) {
					new_params.exposure.redComp = val;
					command_flags |= COMMAND_SETEXPOSURE;
				} else
					retval = -EINVAL;
			}
		} else if (MATCH("green1_comp")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val >= 214 && val <= 255) {
					new_params.exposure.green1Comp = val;
					command_flags |= COMMAND_SETEXPOSURE;
				} else
					retval = -EINVAL;
			}
		} else if (MATCH("green2_comp")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val >= 214 && val <= 255) {
					new_params.exposure.green2Comp = val;
					command_flags |= COMMAND_SETEXPOSURE;
				} else
					retval = -EINVAL;
			}
		} else if (MATCH("blue_comp")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val >= 230 && val <= 255) {
					new_params.exposure.blueComp = val;
					command_flags |= COMMAND_SETEXPOSURE;
				} else
					retval = -EINVAL;
			}
		} else if (MATCH("apcor_gain1")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				command_flags |= COMMAND_SETAPCOR;
				if (val <= 0xff)
					new_params.apcor.gain1 = val;
				else
					retval = -EINVAL;
			}
		} else if (MATCH("apcor_gain2")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				command_flags |= COMMAND_SETAPCOR;
				if (val <= 0xff)
					new_params.apcor.gain2 = val;
				else
					retval = -EINVAL;
			}
		} else if (MATCH("apcor_gain4")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				command_flags |= COMMAND_SETAPCOR;
				if (val <= 0xff)
					new_params.apcor.gain4 = val;
				else
					retval = -EINVAL;
			}
		} else if (MATCH("apcor_gain8")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				command_flags |= COMMAND_SETAPCOR;
				if (val <= 0xff)
					new_params.apcor.gain8 = val;
				else
					retval = -EINVAL;
			}
		} else if (MATCH("vl_offset_gain1")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 0xff)
					new_params.vlOffset.gain1 = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETVLOFFSET;
		} else if (MATCH("vl_offset_gain2")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 0xff)
					new_params.vlOffset.gain2 = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETVLOFFSET;
		} else if (MATCH("vl_offset_gain4")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 0xff)
					new_params.vlOffset.gain4 = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETVLOFFSET;
		} else if (MATCH("vl_offset_gain8")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 0xff)
					new_params.vlOffset.gain8 = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETVLOFFSET;
		} else if (MATCH("flicker_control")) {
			if (!retval && MATCH("on")) {
				new_params.flickerControl.flickerMode = 1;
				new_params.exposure.expMode = 2;
				command_flags |= COMMAND_SETEXPOSURE;
			} else if (!retval && MATCH("off"))
				new_params.flickerControl.flickerMode = 0;
			else
				retval = -EINVAL;

			command_flags |= COMMAND_SETFLICKERCTRL;
		} else if (MATCH("mains_frequency")) {
			if (!retval && MATCH("50")) {
				new_mains = 0;
				new_params.flickerControl.coarseJump = 
					flicker_jumps[new_mains]
					[new_params.sensorFps.baserate]
					[new_params.sensorFps.divisor];
				if (new_params.flickerControl.flickerMode)
					command_flags |= COMMAND_SETFLICKERCTRL;
			} else if (!retval && MATCH("60")) {
				new_mains = 1;
				new_params.flickerControl.coarseJump = 
					flicker_jumps[new_mains]
					[new_params.sensorFps.baserate]
					[new_params.sensorFps.divisor];
				if (new_params.flickerControl.flickerMode)
					command_flags |= COMMAND_SETFLICKERCTRL;
			} else
				retval = -EINVAL;
		} else if (MATCH("allowable_overexposure")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 0xff) {
					new_params.flickerControl.
						allowableOverExposure = val;
					command_flags |= COMMAND_SETFLICKERCTRL;
				} else
					retval = -EINVAL;
			}
		} else if (MATCH("compression_mode")) {
			if (!retval && MATCH("none"))
				new_params.compression.mode =
					CPIA_COMPRESSION_NONE;
			else if (!retval && MATCH("auto"))
				new_params.compression.mode =
					CPIA_COMPRESSION_AUTO;
			else if (!retval && MATCH("manual"))
				new_params.compression.mode =
					CPIA_COMPRESSION_MANUAL;
			else
				retval = -EINVAL;

			command_flags |= COMMAND_SETCOMPRESSION;
		} else if (MATCH("decimation_enable")) {
			if (!retval && MATCH("off"))
				new_params.compression.decimation = 0;
			else
				retval = -EINVAL;

			command_flags |= COMMAND_SETCOMPRESSION;
		} else if (MATCH("compression_target")) {
			if (!retval && MATCH("quality"))
				new_params.compressionTarget.frTargeting = 
					CPIA_COMPRESSION_TARGET_QUALITY;
			else if (!retval && MATCH("framerate"))
				new_params.compressionTarget.frTargeting = 
					CPIA_COMPRESSION_TARGET_FRAMERATE;
			else
				retval = -EINVAL;

			command_flags |= COMMAND_SETCOMPRESSIONTARGET;
		} else if (MATCH("target_framerate")) {
			if (!retval)
				val = VALUE;

			if (!retval)
				new_params.compressionTarget.targetFR = val;
			command_flags |= COMMAND_SETCOMPRESSIONTARGET;
		} else if (MATCH("target_quality")) {
			if (!retval)
				val = VALUE;

			if (!retval)
				new_params.compressionTarget.targetQ = val;

			command_flags |= COMMAND_SETCOMPRESSIONTARGET;
		} else if (MATCH("y_threshold")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val < 32)
					new_params.yuvThreshold.yThreshold = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETYUVTHRESH;
		} else if (MATCH("uv_threshold")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val < 32)
					new_params.yuvThreshold.uvThreshold = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETYUVTHRESH;
		} else if (MATCH("hysteresis")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 0xff)
					new_params.compressionParams.hysteresis = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETCOMPRESSIONPARAMS;
		} else if (MATCH("threshold_max")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 0xff)
					new_params.compressionParams.threshMax = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETCOMPRESSIONPARAMS;
		} else if (MATCH("small_step")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 0xff)
					new_params.compressionParams.smallStep = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETCOMPRESSIONPARAMS;
		} else if (MATCH("large_step")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 0xff)
					new_params.compressionParams.largeStep = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETCOMPRESSIONPARAMS;
		} else if (MATCH("decimation_hysteresis")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 0xff)
					new_params.compressionParams.decimationHysteresis = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETCOMPRESSIONPARAMS;
		} else if (MATCH("fr_diff_step_thresh")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 0xff)
					new_params.compressionParams.frDiffStepThresh = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETCOMPRESSIONPARAMS;
		} else if (MATCH("q_diff_step_thresh")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 0xff)
					new_params.compressionParams.qDiffStepThresh = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETCOMPRESSIONPARAMS;
		} else if (MATCH("decimation_thresh_mod")) {
			if (!retval)
				val = VALUE;

			if (!retval) {
				if (val <= 0xff)
					new_params.compressionParams.decimationThreshMod = val;
				else
					retval = -EINVAL;
			}
			command_flags |= COMMAND_SETCOMPRESSIONPARAMS;
		} else {
			DBG("No match found\n");
			retval = -EINVAL;
		}

		if (!retval) {
			while (count && isspace(*buffer) && *buffer != '\n') {
				--count;
				++buffer;
			}
			if (count) {
				if (*buffer != '\n' && *buffer != ';')
					retval = -EINVAL;
				else {
					--count;
					++buffer;
				}
			}
		}
	}
#undef MATCH	
#undef FIRMWARE_VERSION
#undef VALUE
#undef FIND_VALUE
#undef FIND_END
	if (!retval) {
		if (command_flags & COMMAND_SETCOLOURPARAMS) {
			/* Adjust cam->vp to reflect these changes */
			cam->vp.brightness =
				new_params.colourParams.brightness*65535/100;
			cam->vp.contrast =
				new_params.colourParams.contrast*65535/100;
			cam->vp.colour =
				new_params.colourParams.saturation*65535/100;
		}
		
		memcpy(&cam->params, &new_params, sizeof(struct cam_params));
		cam->mainsFreq = new_mains;
		cam->cmd_queue |= command_flags;
		retval = size;
	} else
		DBG("error: %d\n", retval);
	
	up(&cam->param_lock);
	
	return retval;
}

static void create_proc_cpia_cam(struct cam_data *cam)
{
	char name[7];
	struct proc_dir_entry *ent;
	
	if (!cpia_proc_root || !cam)
		return;

	sprintf(name, "video%d", cam->vdev.minor);
	
	ent = create_proc_entry(name, S_IFREG|S_IRUGO|S_IWUSR, cpia_proc_root);
	if (!ent)
		return;

	ent->data = cam;
	ent->read_proc = cpia_read_proc;
	ent->write_proc = cpia_write_proc;
	ent->size = 3626;
	cam->proc_entry = ent;
}

static void destroy_proc_cpia_cam(struct cam_data *cam)
{
	char name[7];
	
	if (!cam || !cam->proc_entry)
		return;
	
	sprintf(name, "video%d", cam->vdev.minor);
	remove_proc_entry(name, cpia_proc_root);
	cam->proc_entry = NULL;
}

static void proc_cpia_create(void)
{
	cpia_proc_root = create_proc_entry("cpia", S_IFDIR, 0);

	if (cpia_proc_root)
		cpia_proc_root->owner = THIS_MODULE;
	else
		LOG("Unable to initialise /proc/cpia\n");
}

static void proc_cpia_destroy(void)
{
	remove_proc_entry("cpia", 0);
}
#endif /* CONFIG_PROC_FS */

/* ----------------------- debug functions ---------------------- */

#define printstatus(cam) \
  DBG("%02x %02x %02x %02x %02x %02x %02x %02x\n",\
	cam->params.status.systemState, cam->params.status.grabState, \
	cam->params.status.streamState, cam->params.status.fatalError, \
	cam->params.status.cmdError, cam->params.status.debugFlags, \
	cam->params.status.vpStatus, cam->params.status.errorCode);

/* ----------------------- v4l helpers -------------------------- */

/* supported frame palettes and depths */
static inline int valid_mode(u16 palette, u16 depth)
{
	return (palette == VIDEO_PALETTE_GREY && depth == 8) ||
	       (palette == VIDEO_PALETTE_RGB555 && depth == 16) ||
	       (palette == VIDEO_PALETTE_RGB565 && depth == 16) ||
	       (palette == VIDEO_PALETTE_RGB24 && depth == 24) ||
	       (palette == VIDEO_PALETTE_RGB32 && depth == 32) ||
	       (palette == VIDEO_PALETTE_YUV422 && depth == 16) ||
	       (palette == VIDEO_PALETTE_YUYV && depth == 16) ||
	       (palette == VIDEO_PALETTE_UYVY && depth == 16);
}

static int match_videosize( int width, int height )
{
	/* return the best match, where 'best' is as always
	 * the largest that is not bigger than what is requested. */
	if (width>=352 && height>=288)
		return VIDEOSIZE_352_288; /* CIF */

	if (width>=320 && height>=240)
		return VIDEOSIZE_320_240; /* SIF */

	if (width>=288 && height>=216)
		return VIDEOSIZE_288_216;

	if (width>=256 && height>=192)
		return VIDEOSIZE_256_192;

	if (width>=224 && height>=168)
		return VIDEOSIZE_224_168;

	if (width>=192 && height>=144)
		return VIDEOSIZE_192_144;

	if (width>=176 && height>=144)
		return VIDEOSIZE_176_144; /* QCIF */

	if (width>=160 && height>=120)
		return VIDEOSIZE_160_120; /* QSIF */

	if (width>=128 && height>=96)
		return VIDEOSIZE_128_96;

	if (width>=88 && height>=72)
		return VIDEOSIZE_88_72;

	if (width>=64 && height>=48)
		return VIDEOSIZE_64_48;

	if (width>=48 && height>=48)
		return VIDEOSIZE_48_48;

	return -1;
}

/* these are the capture sizes we support */
static void set_vw_size(struct cam_data *cam)
{
	/* the col/row/start/end values are the result of simple math    */
	/* study the SetROI-command in cpia developers guide p 2-22      */
	/* streamStartLine is set to the recommended value in the cpia   */
	/*  developers guide p 3-37                                      */
	switch(cam->video_size) {
	case VIDEOSIZE_CIF:
		cam->vw.width = 352;
		cam->vw.height = 288;
		cam->params.format.videoSize=VIDEOSIZE_CIF;
		cam->params.roi.colStart=0;
		cam->params.roi.colEnd=44;
		cam->params.roi.rowStart=0;
		cam->params.roi.rowEnd=72;
		cam->params.streamStartLine = 120;
		break;
	case VIDEOSIZE_SIF:
		cam->vw.width = 320;
		cam->vw.height = 240;
		cam->params.format.videoSize=VIDEOSIZE_CIF;
		cam->params.roi.colStart=2;
		cam->params.roi.colEnd=42;
		cam->params.roi.rowStart=6;
		cam->params.roi.rowEnd=66;
		cam->params.streamStartLine = 120;
		break;
	case VIDEOSIZE_288_216:
		cam->vw.width = 288;
		cam->vw.height = 216;
		cam->params.format.videoSize=VIDEOSIZE_CIF;
		cam->params.roi.colStart=4;
		cam->params.roi.colEnd=40;
		cam->params.roi.rowStart=9;
		cam->params.roi.rowEnd=63;
		cam->params.streamStartLine = 120;
		break;
	case VIDEOSIZE_256_192:
		cam->vw.width = 256;
		cam->vw.height = 192;
		cam->params.format.videoSize=VIDEOSIZE_CIF;
		cam->params.roi.colStart=6;
		cam->params.roi.colEnd=38;
		cam->params.roi.rowStart=12;
		cam->params.roi.rowEnd=60;
		cam->params.streamStartLine = 120;
		break;
	case VIDEOSIZE_224_168:
		cam->vw.width = 224;
		cam->vw.height = 168;
		cam->params.format.videoSize=VIDEOSIZE_CIF;
		cam->params.roi.colStart=8;
		cam->params.roi.colEnd=36;
		cam->params.roi.rowStart=15;
		cam->params.roi.rowEnd=57;
		cam->params.streamStartLine = 120;
		break;
	case VIDEOSIZE_192_144:
		cam->vw.width = 192;
		cam->vw.height = 144;
		cam->params.format.videoSize=VIDEOSIZE_CIF;
		cam->params.roi.colStart=10;
		cam->params.roi.colEnd=34;
		cam->params.roi.rowStart=18;
		cam->params.roi.rowEnd=54;
		cam->params.streamStartLine = 120;
		break;
	case VIDEOSIZE_QCIF:
		cam->vw.width = 176;
		cam->vw.height = 144;
		cam->params.format.videoSize=VIDEOSIZE_QCIF;
		cam->params.roi.colStart=0;
		cam->params.roi.colEnd=22;
		cam->params.roi.rowStart=0;
		cam->params.roi.rowEnd=36;
		cam->params.streamStartLine = 60;
		break;
	case VIDEOSIZE_QSIF:
		cam->vw.width = 160;
		cam->vw.height = 120;
		cam->params.format.videoSize=VIDEOSIZE_QCIF;
		cam->params.roi.colStart=1;
		cam->params.roi.colEnd=21;
		cam->params.roi.rowStart=3;
		cam->params.roi.rowEnd=33;
		cam->params.streamStartLine = 60;
		break;
	case VIDEOSIZE_128_96:
		cam->vw.width = 128;
		cam->vw.height = 96;
		cam->params.format.videoSize=VIDEOSIZE_QCIF;
		cam->params.roi.colStart=3;
		cam->params.roi.colEnd=19;
		cam->params.roi.rowStart=6;
		cam->params.roi.rowEnd=30;
		cam->params.streamStartLine = 60;
		break;
	case VIDEOSIZE_88_72:
		cam->vw.width = 88;
		cam->vw.height = 72;
		cam->params.format.videoSize=VIDEOSIZE_QCIF;
		cam->params.roi.colStart=5;
		cam->params.roi.colEnd=16;
		cam->params.roi.rowStart=9;
		cam->params.roi.rowEnd=27;
		cam->params.streamStartLine = 60;
		break;
	case VIDEOSIZE_64_48:
		cam->vw.width = 64;
		cam->vw.height = 48;
		cam->params.format.videoSize=VIDEOSIZE_QCIF;
		cam->params.roi.colStart=7;
		cam->params.roi.colEnd=15;
		cam->params.roi.rowStart=12;
		cam->params.roi.rowEnd=24;
		cam->params.streamStartLine = 60;
		break;
	case VIDEOSIZE_48_48:
		cam->vw.width = 48;
		cam->vw.height = 48;
		cam->params.format.videoSize=VIDEOSIZE_QCIF;
		cam->params.roi.colStart=8;
		cam->params.roi.colEnd=14;
		cam->params.roi.rowStart=6;
		cam->params.roi.rowEnd=30;
		cam->params.streamStartLine = 60;
		break;
	default:
		LOG("bad videosize value: %d\n", cam->video_size);
	}

	return;
}

static int allocate_frame_buf(struct cam_data *cam)
{
	int i;

	cam->frame_buf = rvmalloc(FRAME_NUM * CPIA_MAX_FRAME_SIZE);
	if (!cam->frame_buf)
		return -ENOBUFS;

	for (i = 0; i < FRAME_NUM; i++)
		cam->frame[i].data = cam->frame_buf + i * CPIA_MAX_FRAME_SIZE;

	return 0;
}

static int free_frame_buf(struct cam_data *cam)
{
	int i;
	
	rvfree(cam->frame_buf, FRAME_NUM*CPIA_MAX_FRAME_SIZE);
	cam->frame_buf = 0;
	for (i=0; i < FRAME_NUM; i++)
		cam->frame[i].data = NULL;

	return 0;
}


static void inline free_frames(struct cpia_frame frame[FRAME_NUM])
{
	int i;

	for (i=0; i < FRAME_NUM; i++)
		frame[i].state = FRAME_UNUSED;
	return;
}

/**********************************************************************
 *
 * General functions
 *
 **********************************************************************/
/* send an arbitrary command to the camera */
static int do_command(struct cam_data *cam, u16 command, u8 a, u8 b, u8 c, u8 d)
{
	int retval, datasize;
	u8 cmd[8], data[8];

	switch(command) {
	case CPIA_COMMAND_GetCPIAVersion:
	case CPIA_COMMAND_GetPnPID:
	case CPIA_COMMAND_GetCameraStatus:
	case CPIA_COMMAND_GetVPVersion:
		datasize=8;
		break;
	case CPIA_COMMAND_GetColourParams:
	case CPIA_COMMAND_GetColourBalance:
	case CPIA_COMMAND_GetExposure:
		down(&cam->param_lock);
		datasize=8;
		break;
	default:
		datasize=0;
		break;
	}

	cmd[0] = command>>8;
	cmd[1] = command&0xff;
	cmd[2] = a;
	cmd[3] = b;
	cmd[4] = c;
	cmd[5] = d;
	cmd[6] = datasize;
	cmd[7] = 0;

	retval = cam->ops->transferCmd(cam->lowlevel_data, cmd, data);
	if (retval) {
		DBG("%x - failed, retval=%d\n", command, retval);
		if (command == CPIA_COMMAND_GetColourParams ||
		    command == CPIA_COMMAND_GetColourBalance ||
		    command == CPIA_COMMAND_GetExposure)
			up(&cam->param_lock);
	} else {
		switch(command) {
		case CPIA_COMMAND_GetCPIAVersion:
			cam->params.version.firmwareVersion = data[0];
			cam->params.version.firmwareRevision = data[1];
			cam->params.version.vcVersion = data[2];
			cam->params.version.vcRevision = data[3];
			break;
		case CPIA_COMMAND_GetPnPID:
			cam->params.pnpID.vendor = data[0]+(((u16)data[1])<<8);
			cam->params.pnpID.product = data[2]+(((u16)data[3])<<8);
			cam->params.pnpID.deviceRevision =
				data[4]+(((u16)data[5])<<8);
			break;
		case CPIA_COMMAND_GetCameraStatus:
			cam->params.status.systemState = data[0];
			cam->params.status.grabState = data[1];
			cam->params.status.streamState = data[2];
			cam->params.status.fatalError = data[3];
			cam->params.status.cmdError = data[4];
			cam->params.status.debugFlags = data[5];
			cam->params.status.vpStatus = data[6];
			cam->params.status.errorCode = data[7];
			break;
		case CPIA_COMMAND_GetVPVersion:
			cam->params.vpVersion.vpVersion = data[0];
			cam->params.vpVersion.vpRevision = data[1];
			cam->params.vpVersion.cameraHeadID =
				data[2]+(((u16)data[3])<<8);
			break;
		case CPIA_COMMAND_GetColourParams:
			cam->params.colourParams.brightness = data[0];
			cam->params.colourParams.contrast = data[1];
			cam->params.colourParams.saturation = data[2];
			up(&cam->param_lock);
			break;
		case CPIA_COMMAND_GetColourBalance:
			cam->params.colourBalance.redGain = data[0];
			cam->params.colourBalance.greenGain = data[1];
			cam->params.colourBalance.blueGain = data[2];
			up(&cam->param_lock);
			break;
		case CPIA_COMMAND_GetExposure:
			cam->params.exposure.gain = data[0];
			cam->params.exposure.fineExp = data[1];
			cam->params.exposure.coarseExpLo = data[2];
			cam->params.exposure.coarseExpHi = data[3];
			cam->params.exposure.redComp = data[4];
			cam->params.exposure.green1Comp = data[5];
			cam->params.exposure.green2Comp = data[6];
			cam->params.exposure.blueComp = data[7];
			/* If the *Comp parameters are wacko, generate
			 * a warning, and reset them back to default
			 * values.             - rich@annexia.org
			 */
			if (cam->params.exposure.redComp < 220 ||
			    cam->params.exposure.redComp > 255 ||
			    cam->params.exposure.green1Comp < 214 ||
			    cam->params.exposure.green1Comp > 255 ||
			    cam->params.exposure.green2Comp < 214 ||
			    cam->params.exposure.green2Comp > 255 ||
			    cam->params.exposure.blueComp < 230 ||
			    cam->params.exposure.blueComp > 255)
			  {
			    printk (KERN_WARNING "*_comp parameters have gone AWOL (%d/%d/%d/%d) - reseting them\n",
				    cam->params.exposure.redComp,
				    cam->params.exposure.green1Comp,
				    cam->params.exposure.green2Comp,
				    cam->params.exposure.blueComp);
			    cam->params.exposure.redComp = 220;
			    cam->params.exposure.green1Comp = 214;
			    cam->params.exposure.green2Comp = 214;
			    cam->params.exposure.blueComp = 230;
			  }
			up(&cam->param_lock);
			break;
		default:
			break;
		}
	}
	return retval;
}

/* send a command  to the camera with an additional data transaction */
static int do_command_extended(struct cam_data *cam, u16 command,
                               u8 a, u8 b, u8 c, u8 d,
                               u8 e, u8 f, u8 g, u8 h,
                               u8 i, u8 j, u8 k, u8 l)
{
	int retval;
	u8 cmd[8], data[8];

	cmd[0] = command>>8;
	cmd[1] = command&0xff;
	cmd[2] = a;
	cmd[3] = b;
	cmd[4] = c;
	cmd[5] = d;
	cmd[6] = 8;
	cmd[7] = 0;
	data[0] = e;
	data[1] = f;
	data[2] = g;
	data[3] = h;
	data[4] = i;
	data[5] = j;
	data[6] = k;
	data[7] = l;

	retval = cam->ops->transferCmd(cam->lowlevel_data, cmd, data);
	if (retval)
		LOG("%x - failed\n", command);

	return retval;
}

/**********************************************************************
 *
 * Colorspace conversion
 *
 **********************************************************************/
#define LIMIT(x) ((((x)>0xffffff)?0xff0000:(((x)<=0xffff)?0:(x)&0xff0000))>>16)

static int yuvconvert(unsigned char *yuv, unsigned char *rgb, int out_fmt,
                      int in_uyvy, int mmap_kludge)
{
	int y, u, v, r, g, b, y1;

	switch(out_fmt) {
	case VIDEO_PALETTE_RGB555:
	case VIDEO_PALETTE_RGB565:
	case VIDEO_PALETTE_RGB24:
	case VIDEO_PALETTE_RGB32:
		if (in_uyvy) {
			u = *yuv++ - 128;
			y = (*yuv++ - 16) * 76310;
			v = *yuv++ - 128;
			y1 = (*yuv - 16) * 76310;
		} else {
			y = (*yuv++ - 16) * 76310;
			u = *yuv++ - 128;
			y1 = (*yuv++ - 16) * 76310;
			v = *yuv - 128;
		}
		r = 104635 * v;
		g = -25690 * u + -53294 * v;
		b = 132278 * u;
		break;
	default:
		y = *yuv++;
		u = *yuv++;
		y1 = *yuv++;
		v = *yuv;
		/* Just to avoid compiler warnings */
		r = 0;
		g = 0;
		b = 0;
		break;
	}
	switch(out_fmt) {
	case VIDEO_PALETTE_RGB555:
		*rgb++ = ((LIMIT(g+y) & 0xf8) << 2) | (LIMIT(b+y) >> 3);
		*rgb++ = ((LIMIT(r+y) & 0xf8) >> 1) | (LIMIT(g+y) >> 6);
		*rgb++ = ((LIMIT(g+y1) & 0xf8) << 2) | (LIMIT(b+y1) >> 3);
		*rgb = ((LIMIT(r+y1) & 0xf8) >> 1) | (LIMIT(g+y1) >> 6);
		return 4;
	case VIDEO_PALETTE_RGB565:
		*rgb++ = ((LIMIT(g+y) & 0xfc) << 3) | (LIMIT(b+y) >> 3);
		*rgb++ = (LIMIT(r+y) & 0xf8) | (LIMIT(g+y) >> 5);
		*rgb++ = ((LIMIT(g+y1) & 0xfc) << 3) | (LIMIT(b+y1) >> 3);
		*rgb = (LIMIT(r+y1) & 0xf8) | (LIMIT(g+y1) >> 5);
		return 4;
	case VIDEO_PALETTE_RGB24:
		if (mmap_kludge) {
			*rgb++ = LIMIT(b+y);
			*rgb++ = LIMIT(g+y);
			*rgb++ = LIMIT(r+y);
			*rgb++ = LIMIT(b+y1);
			*rgb++ = LIMIT(g+y1);
			*rgb = LIMIT(r+y1);
		} else {
			*rgb++ = LIMIT(r+y);
			*rgb++ = LIMIT(g+y);
			*rgb++ = LIMIT(b+y);
			*rgb++ = LIMIT(r+y1);
			*rgb++ = LIMIT(g+y1);
			*rgb = LIMIT(b+y1);
		}
		return 6;
	case VIDEO_PALETTE_RGB32:
		if (mmap_kludge) {
			*rgb++ = LIMIT(b+y);
			*rgb++ = LIMIT(g+y);
			*rgb++ = LIMIT(r+y);
			rgb++;
			*rgb++ = LIMIT(b+y1);
			*rgb++ = LIMIT(g+y1);
			*rgb = LIMIT(r+y1);
		} else {
			*rgb++ = LIMIT(r+y);
			*rgb++ = LIMIT(g+y);
			*rgb++ = LIMIT(b+y);
			rgb++;
			*rgb++ = LIMIT(r+y1);
			*rgb++ = LIMIT(g+y1);
			*rgb = LIMIT(b+y1);
		}
		return 8;
	case VIDEO_PALETTE_GREY:
		*rgb++ = y;
		*rgb = y1;
		return 2;
	case VIDEO_PALETTE_YUV422:
	case VIDEO_PALETTE_YUYV:
		*rgb++ = y;
		*rgb++ = u;
		*rgb++ = y1;
		*rgb = v;
		return 4;
	case VIDEO_PALETTE_UYVY:
		*rgb++ = u;
		*rgb++ = y;
		*rgb++ = v;
		*rgb = y1;
		return 4;
	default:
		DBG("Empty: %d\n", out_fmt);
		return 0;
	}
}

static int skipcount(int count, int fmt)
{
	switch(fmt) {
	case VIDEO_PALETTE_GREY:
	case VIDEO_PALETTE_RGB555:
	case VIDEO_PALETTE_RGB565:
	case VIDEO_PALETTE_YUV422:
	case VIDEO_PALETTE_YUYV:
	case VIDEO_PALETTE_UYVY:
		return 2*count;
	case VIDEO_PALETTE_RGB24:
		return 3*count;
	case VIDEO_PALETTE_RGB32:
		return 4*count;
	default:
		return 0;
	}
}

static int parse_picture(struct cam_data *cam, int size)
{
	u8 *obuf, *ibuf, *end_obuf;
	int ll, in_uyvy, compressed, origsize, out_fmt;

	/* make sure params don't change while we are decoding */
	down(&cam->param_lock);

	obuf = cam->decompressed_frame.data;
	end_obuf = obuf+CPIA_MAX_FRAME_SIZE;
	ibuf = cam->raw_image;
	origsize = size;
	out_fmt = cam->vp.palette;

	if ((ibuf[0] != MAGIC_0) || (ibuf[1] != MAGIC_1)) {
		LOG("header not found\n");
		up(&cam->param_lock);
		return -1;
	}

	if ((ibuf[16] != VIDEOSIZE_QCIF) && (ibuf[16] != VIDEOSIZE_CIF)) {
		LOG("wrong video size\n");
		up(&cam->param_lock);
		return -1;
	}
	
	if (ibuf[17] != SUBSAMPLE_422) {
		LOG("illegal subtype %d\n",ibuf[17]);
		up(&cam->param_lock);
		return -1;
	}
	
	if (ibuf[18] != YUVORDER_YUYV && ibuf[18] != YUVORDER_UYVY) {
		LOG("illegal yuvorder %d\n",ibuf[18]);
		up(&cam->param_lock);
		return -1;
	}
	in_uyvy = ibuf[18] == YUVORDER_UYVY;
	
#if 0
	/* FIXME: ROI mismatch occurs when switching capture sizes */
	if ((ibuf[24] != cam->params.roi.colStart) ||
	    (ibuf[25] != cam->params.roi.colEnd) ||
	    (ibuf[26] != cam->params.roi.rowStart) ||
	    (ibuf[27] != cam->params.roi.rowEnd)) {
		LOG("ROI mismatch\n");
		up(&cam->param_lock);
		return -1;
	}
#endif
	
	if ((ibuf[28] != NOT_COMPRESSED) && (ibuf[28] != COMPRESSED)) {
		LOG("illegal compression %d\n",ibuf[28]);
		up(&cam->param_lock);
		return -1;
	}
	compressed = (ibuf[28] == COMPRESSED);
	
	if (ibuf[29] != NO_DECIMATION) {
		LOG("decimation not supported\n");
		up(&cam->param_lock);
		return -1;
	}
	
	cam->params.yuvThreshold.yThreshold = ibuf[30];
	cam->params.yuvThreshold.uvThreshold = ibuf[31];
	cam->params.status.systemState = ibuf[32];
	cam->params.status.grabState = ibuf[33];
	cam->params.status.streamState = ibuf[34];
	cam->params.status.fatalError = ibuf[35];
	cam->params.status.cmdError = ibuf[36];
	cam->params.status.debugFlags = ibuf[37];
	cam->params.status.vpStatus = ibuf[38];
	cam->params.status.errorCode = ibuf[39];
	cam->fps = ibuf[41];
	up(&cam->param_lock);
	
	ibuf += FRAME_HEADER_SIZE;
	size -= FRAME_HEADER_SIZE;
	ll = ibuf[0] | (ibuf[1] << 8);
	ibuf += 2;

	while (size > 0) {
		size -= (ll+2);
		if (size < 0) {
			LOG("Insufficient data in buffer\n");
			return -1;
		}

		while (ll > 1) {
			if (!compressed || (compressed && !(*ibuf & 1))) {
				obuf += yuvconvert(ibuf, obuf, out_fmt,
				                   in_uyvy, cam->mmap_kludge);
				ibuf += 4;
				ll -= 4;
			} else {
				/*skip compressed interval from previous frame*/
				int skipsize = skipcount(*ibuf >> 1, out_fmt);
				obuf += skipsize;
				if (obuf > end_obuf) {
					LOG("Insufficient data in buffer\n");
					return -1;
				}
				++ibuf;
				ll--;
			}
		}
		if (ll == 1) {
			if (*ibuf != EOL) {
				LOG("EOL not found giving up after %d/%d"
				    " bytes\n", origsize-size, origsize);
				return -1;
			}

			ibuf++; /* skip over EOL */

			if ((size > 3) && (ibuf[0] == EOI) && (ibuf[1] == EOI) &&
			   (ibuf[2] == EOI) && (ibuf[3] == EOI)) {
			 	size -= 4;
				break;
			}

			if (size > 1) {
				ll = ibuf[0] | (ibuf[1] << 8);
				ibuf += 2; /* skip over line length */
			}
		} else {
			LOG("line length was not 1 but %d after %d/%d bytes\n",
			    ll, origsize-size, origsize);
			return -1;
		}
	}
	
	cam->decompressed_frame.count = obuf-cam->decompressed_frame.data;

	return cam->decompressed_frame.count;
}

/* InitStreamCap wrapper to select correct start line */
static inline int init_stream_cap(struct cam_data *cam)
{
	return do_command(cam, CPIA_COMMAND_InitStreamCap,
	                  0, cam->params.streamStartLine, 0, 0);
}

/* update various camera modes and settings */
static void dispatch_commands(struct cam_data *cam)
{
	down(&cam->param_lock);
	if (cam->cmd_queue==COMMAND_NONE) {
		up(&cam->param_lock);
		return;
	}
	DEB_BYTE(cam->cmd_queue);
	DEB_BYTE(cam->cmd_queue>>8);
	if (cam->cmd_queue & COMMAND_SETCOLOURPARAMS)
		do_command(cam, CPIA_COMMAND_SetColourParams,
		           cam->params.colourParams.brightness,
		           cam->params.colourParams.contrast,
		           cam->params.colourParams.saturation, 0);

	if (cam->cmd_queue & COMMAND_SETCOMPRESSION)
		do_command(cam, CPIA_COMMAND_SetCompression,
		           cam->params.compression.mode,
			   cam->params.compression.decimation, 0, 0);

	if (cam->cmd_queue & COMMAND_SETFORMAT) {
		do_command(cam, CPIA_COMMAND_SetFormat,
	        	   cam->params.format.videoSize,
	        	   cam->params.format.subSample,
	        	   cam->params.format.yuvOrder, 0);
		do_command(cam, CPIA_COMMAND_SetROI,
	        	   cam->params.roi.colStart, cam->params.roi.colEnd,
	        	   cam->params.roi.rowStart, cam->params.roi.rowEnd);
		cam->first_frame = 1;
	}

	if (cam->cmd_queue & COMMAND_SETCOMPRESSIONTARGET)
		do_command(cam, CPIA_COMMAND_SetCompressionTarget,
		           cam->params.compressionTarget.frTargeting,
		           cam->params.compressionTarget.targetFR,
		           cam->params.compressionTarget.targetQ, 0);

	if (cam->cmd_queue & COMMAND_SETYUVTHRESH)
		do_command(cam, CPIA_COMMAND_SetYUVThresh,
		           cam->params.yuvThreshold.yThreshold,
		           cam->params.yuvThreshold.uvThreshold, 0, 0);

	if (cam->cmd_queue & COMMAND_SETECPTIMING)
		do_command(cam, CPIA_COMMAND_SetECPTiming,
		           cam->params.ecpTiming, 0, 0, 0);

	if (cam->cmd_queue & COMMAND_SETCOMPRESSIONPARAMS)
		do_command_extended(cam, CPIA_COMMAND_SetCompressionParams,
		            0, 0, 0, 0,
		            cam->params.compressionParams.hysteresis,
		            cam->params.compressionParams.threshMax,
		            cam->params.compressionParams.smallStep,
		            cam->params.compressionParams.largeStep,
		            cam->params.compressionParams.decimationHysteresis,
		            cam->params.compressionParams.frDiffStepThresh,
		            cam->params.compressionParams.qDiffStepThresh,
		            cam->params.compressionParams.decimationThreshMod);

	if (cam->cmd_queue & COMMAND_SETEXPOSURE)
		do_command_extended(cam, CPIA_COMMAND_SetExposure,
		                    cam->params.exposure.gainMode,
		                    cam->params.exposure.expMode,
		                    cam->params.exposure.compMode,
		                    cam->params.exposure.centreWeight,
		                    cam->params.exposure.gain,
		                    cam->params.exposure.fineExp,
		                    cam->params.exposure.coarseExpLo,
		                    cam->params.exposure.coarseExpHi,
		                    cam->params.exposure.redComp,
		                    cam->params.exposure.green1Comp,
		                    cam->params.exposure.green2Comp,
		                    cam->params.exposure.blueComp);

	if (cam->cmd_queue & COMMAND_SETCOLOURBALANCE) {
		if (cam->params.colourBalance.balanceModeIsAuto) {
			do_command(cam, CPIA_COMMAND_SetColourBalance,
				   2, 0, 0, 0);
		} else {
			do_command(cam, CPIA_COMMAND_SetColourBalance,
				   1,
				   cam->params.colourBalance.redGain,
				   cam->params.colourBalance.greenGain,
				   cam->params.colourBalance.blueGain);
			do_command(cam, CPIA_COMMAND_SetColourBalance,
				   3, 0, 0, 0);
		}
	}

	if (cam->cmd_queue & COMMAND_SETSENSORFPS)
		do_command(cam, CPIA_COMMAND_SetSensorFPS,
		           cam->params.sensorFps.divisor,
		           cam->params.sensorFps.baserate, 0, 0);

	if (cam->cmd_queue & COMMAND_SETAPCOR)
		do_command(cam, CPIA_COMMAND_SetApcor,
		           cam->params.apcor.gain1,
		           cam->params.apcor.gain2,
		           cam->params.apcor.gain4,
		           cam->params.apcor.gain8);

	if (cam->cmd_queue & COMMAND_SETFLICKERCTRL)
		do_command(cam, CPIA_COMMAND_SetFlickerCtrl,
		           cam->params.flickerControl.flickerMode,
		           cam->params.flickerControl.coarseJump,
		           cam->params.flickerControl.allowableOverExposure, 0);

	if (cam->cmd_queue & COMMAND_SETVLOFFSET)
		do_command(cam, CPIA_COMMAND_SetVLOffset,
		           cam->params.vlOffset.gain1,
		           cam->params.vlOffset.gain2,
		           cam->params.vlOffset.gain4,
		           cam->params.vlOffset.gain8);

	if (cam->cmd_queue & COMMAND_PAUSE)
		do_command(cam, CPIA_COMMAND_EndStreamCap, 0, 0, 0, 0);

	if (cam->cmd_queue & COMMAND_RESUME)
		init_stream_cap(cam);

	up(&cam->param_lock);
	cam->cmd_queue = COMMAND_NONE;
	return;
}

/* kernel thread function to read image from camera */
static void fetch_frame(void *data)
{
	int image_size, retry;
	struct cam_data *cam = (struct cam_data *)data;
	unsigned long oldjif, rate, diff;

	/* Allow up to two bad images in a row to be read and
	 * ignored before an error is reported */
	for (retry = 0; retry < 3; ++retry) {
		if (retry)
			DBG("retry=%d\n", retry);

		if (!cam->ops)
			continue;

		/* load first frame always uncompressed */
		if (cam->first_frame &&
		    cam->params.compression.mode != CPIA_COMPRESSION_NONE)
			do_command(cam, CPIA_COMMAND_SetCompression,
				   CPIA_COMPRESSION_NONE,
				   NO_DECIMATION, 0, 0);

		/* init camera upload */
		if (do_command(cam, CPIA_COMMAND_SetGrabMode,
			       CPIA_GRAB_CONTINUOUS, 0, 0, 0))
			continue;

		if (do_command(cam, CPIA_COMMAND_GrabFrame, 0,
			       cam->params.streamStartLine, 0, 0))
			continue;

		if (cam->ops->wait_for_stream_ready) {
			/* loop until image ready */
			do_command(cam, CPIA_COMMAND_GetCameraStatus,0,0,0,0);
			while (cam->params.status.streamState != STREAM_READY) {
				if (current->need_resched)
					schedule();

				current->state = TASK_INTERRUPTIBLE;

				/* sleep for 10 ms, hopefully ;) */
				schedule_timeout(10*HZ/1000);
				if (signal_pending(current))
					return;

				do_command(cam, CPIA_COMMAND_GetCameraStatus,
				           0, 0, 0, 0);
			}
		}

		/* grab image from camera */
		if (current->need_resched)
			schedule();

		oldjif = jiffies;
		image_size = cam->ops->streamRead(cam->lowlevel_data,
						  cam->raw_image, 0);
		if (image_size <= 0) {
			DBG("streamRead failed: %d\n", image_size);
			continue;
		}

		rate = image_size * HZ / 1024;
		diff = jiffies-oldjif;
		cam->transfer_rate = diff==0 ? rate : rate/diff;
			/* diff==0 ? unlikely but possible */

		/* camera idle now so dispatch queued commands */
		dispatch_commands(cam);

		/* Update our knowledge of the camera state - FIXME: necessary? */
		do_command(cam, CPIA_COMMAND_GetColourBalance, 0, 0, 0, 0);
		do_command(cam, CPIA_COMMAND_GetExposure, 0, 0, 0, 0);

		/* decompress and convert image to by copying it from
		 * raw_image to decompressed_frame
		 */
		if (current->need_resched)
			schedule();

		cam->image_size = parse_picture(cam, image_size);
		if (cam->image_size <= 0)
			DBG("parse_picture failed %d\n", cam->image_size);
		else
			break;
	}

	if (retry < 3) {
		/* FIXME: this only works for double buffering */
		if (cam->frame[cam->curframe].state == FRAME_READY) {
			memcpy(cam->frame[cam->curframe].data,
			       cam->decompressed_frame.data,
			       cam->decompressed_frame.count);
			cam->frame[cam->curframe].state = FRAME_DONE;
		} else
			cam->decompressed_frame.state = FRAME_DONE;

#if 0
		if (cam->first_frame &&
		    cam->params.compression.mode != CPIA_COMPRESSION_NONE) {
			cam->first_frame = 0;
			cam->cmd_queue |= COMMAND_SETCOMPRESSION;
		}
#else
		if (cam->first_frame) {
			cam->first_frame = 0;
			cam->cmd_queue |= COMMAND_SETCOMPRESSION;
			cam->cmd_queue |= COMMAND_SETEXPOSURE;
		}
#endif
	}
}

static int capture_frame(struct cam_data *cam, struct video_mmap *vm)
{
	int retval = 0;

	if (!cam->frame_buf) {
		/* we do lazy allocation */
		if ((retval = allocate_frame_buf(cam)))
			return retval;
	}
	
	/* FIXME: the first frame seems to be captured by the camera
	   without regards to any initial settings, so we throw away
	   that one, the next one is generated with our settings
	   (exposure, color balance, ...)
	*/
	if (cam->first_frame) {
		cam->curframe = vm->frame;
		cam->frame[cam->curframe].state = FRAME_READY;
		fetch_frame(cam);
		if (cam->frame[cam->curframe].state != FRAME_DONE)
			retval = -EIO;
	}
	cam->curframe = vm->frame;
	cam->frame[cam->curframe].state = FRAME_READY;
	fetch_frame(cam);
	if (cam->frame[cam->curframe].state != FRAME_DONE)
		retval=-EIO;

	return retval;
}
  
static int goto_high_power(struct cam_data *cam)
{
	if (do_command(cam, CPIA_COMMAND_GotoHiPower, 0, 0, 0, 0))
		return -1;
	mdelay(100);		/* windows driver does it too */
	if (do_command(cam, CPIA_COMMAND_GetCameraStatus, 0, 0, 0, 0))
		return -1;
	if (cam->params.status.systemState == HI_POWER_STATE) {
		DBG("camera now in HIGH power state\n");
		return 0;
	}
	printstatus(cam);
	return -1;
}

static int goto_low_power(struct cam_data *cam)
{
	if (do_command(cam, CPIA_COMMAND_GotoLoPower, 0, 0, 0, 0))
		return -1;
	if (do_command(cam, CPIA_COMMAND_GetCameraStatus, 0, 0, 0, 0))
		return -1;
	if (cam->params.status.systemState == LO_POWER_STATE) {
		DBG("camera now in LOW power state\n");
		return 0;
	}
	printstatus(cam);
	return -1;
}

static void save_camera_state(struct cam_data *cam)
{
	do_command(cam, CPIA_COMMAND_GetColourBalance, 0, 0, 0, 0);
	do_command(cam, CPIA_COMMAND_GetExposure, 0, 0, 0, 0);

	DBG("%d/%d/%d/%d/%d/%d/%d/%d\n",
	     cam->params.exposure.gain,
	     cam->params.exposure.fineExp,
	     cam->params.exposure.coarseExpLo,
	     cam->params.exposure.coarseExpHi,
	     cam->params.exposure.redComp,
	     cam->params.exposure.green1Comp,
	     cam->params.exposure.green2Comp,
	     cam->params.exposure.blueComp);
	DBG("%d/%d/%d\n",
	     cam->params.colourBalance.redGain,
	     cam->params.colourBalance.greenGain,
	     cam->params.colourBalance.blueGain);
}

static void set_camera_state(struct cam_data *cam)
{
	if(cam->params.colourBalance.balanceModeIsAuto) {
		do_command(cam, CPIA_COMMAND_SetColourBalance, 
	        	   2, 0, 0, 0);
	} else {
		do_command(cam, CPIA_COMMAND_SetColourBalance, 
	        	   1,
	        	   cam->params.colourBalance.redGain,
	        	   cam->params.colourBalance.greenGain,
	        	   cam->params.colourBalance.blueGain);
		do_command(cam, CPIA_COMMAND_SetColourBalance, 
	        	   3, 0, 0, 0);
	}


	do_command_extended(cam, CPIA_COMMAND_SetExposure,
			    cam->params.exposure.gainMode, 1, 1,
			    cam->params.exposure.centreWeight,
	                    cam->params.exposure.gain,
	                    cam->params.exposure.fineExp,
	                    cam->params.exposure.coarseExpLo,
	                    cam->params.exposure.coarseExpHi,
			    cam->params.exposure.redComp,
			    cam->params.exposure.green1Comp,
			    cam->params.exposure.green2Comp,
			    cam->params.exposure.blueComp);
	do_command_extended(cam, CPIA_COMMAND_SetExposure,
			    0, 3, 0, 0,
			    0, 0, 0, 0, 0, 0, 0, 0);

	if (!cam->params.exposure.gainMode)
		cam->params.exposure.gainMode = 2;
	if (!cam->params.exposure.expMode)
		cam->params.exposure.expMode = 2;
	if (!cam->params.exposure.centreWeight)
		cam->params.exposure.centreWeight = 1;
	
	cam->cmd_queue = COMMAND_SETCOMPRESSION |
	                 COMMAND_SETCOMPRESSIONTARGET |
	                 COMMAND_SETCOLOURPARAMS |
	                 COMMAND_SETFORMAT |
	                 COMMAND_SETYUVTHRESH |
	                 COMMAND_SETECPTIMING |
	                 COMMAND_SETCOMPRESSIONPARAMS |
#if 0
	                 COMMAND_SETEXPOSURE |
#endif
	                 COMMAND_SETCOLOURBALANCE |
	                 COMMAND_SETSENSORFPS |
	                 COMMAND_SETAPCOR |
	                 COMMAND_SETFLICKERCTRL |
	                 COMMAND_SETVLOFFSET;
	dispatch_commands(cam);
	save_camera_state(cam);

	return;
}

static void get_version_information(struct cam_data *cam)
{
	/* GetCPIAVersion */
	do_command(cam, CPIA_COMMAND_GetCPIAVersion, 0, 0, 0, 0);

	/* GetPnPID */
	do_command(cam, CPIA_COMMAND_GetPnPID, 0, 0, 0, 0);
}

/* initialize camera */
static int reset_camera(struct cam_data *cam)
{
	/* Start the camera in low power mode */
	if (goto_low_power(cam)) {
		if (cam->params.status.systemState != WARM_BOOT_STATE)
			return -ENODEV;

		/* FIXME: this is just dirty trial and error */
		reset_camera_struct(cam);
		goto_high_power(cam);
		do_command(cam, CPIA_COMMAND_DiscardFrame, 0, 0, 0, 0);
		if (goto_low_power(cam))
			return -NODEV;
	}
	
	/* procedure described in developer's guide p3-28 */
	
	/* Check the firmware version FIXME: should we check PNPID? */
	cam->params.version.firmwareVersion = 0;
	get_version_information(cam);
	if (cam->params.version.firmwareVersion != 1)
		return -ENODEV;
	
	/* The fatal error checking should be done after
	 * the camera powers up (developer's guide p 3-38) */

	/* Set streamState before transition to high power to avoid bug
	 * in firmware 1-02 */
	do_command(cam, CPIA_COMMAND_ModifyCameraStatus, STREAMSTATE, 0,
	           STREAM_NOT_READY, 0);
	
	/* GotoHiPower */
	if (goto_high_power(cam))
		return -ENODEV;

	/* Check the camera status */
	if (do_command(cam, CPIA_COMMAND_GetCameraStatus, 0, 0, 0, 0))
		return -EIO;

	if (cam->params.status.fatalError) {
		DBG("fatal_error:              %#04x\n",
		    cam->params.status.fatalError);
		DBG("vp_status:                %#04x\n",
		    cam->params.status.vpStatus);
		if (cam->params.status.fatalError & ~(COM_FLAG|CPIA_FLAG)) {
			/* Fatal error in camera */
			return -EIO;
		} else if (cam->params.status.fatalError & (COM_FLAG|CPIA_FLAG)) {
			/* Firmware 1-02 may do this for parallel port cameras,
			 * just clear the flags (developer's guide p 3-38) */
			do_command(cam, CPIA_COMMAND_ModifyCameraStatus,
			           FATALERROR, ~(COM_FLAG|CPIA_FLAG), 0, 0);
		}
	}
	
	/* Check the camera status again */
	if (cam->params.status.fatalError) {
		if (cam->params.status.fatalError)
			return -EIO;
	}
	
	/* VPVersion can't be retrieved before the camera is in HiPower,
	 * so get it here instead of in get_version_information. */
	do_command(cam, CPIA_COMMAND_GetVPVersion, 0, 0, 0, 0);

	/* set camera to a known state */
	set_camera_state(cam);
	
	return 0;
}

/* ------------------------- V4L interface --------------------- */
static int cpia_open(struct video_device *dev, int flags)
{
	int i;
	struct cam_data *cam = dev->priv;

	if (!cam) {
		DBG("Internal error, cam_data not found!\n");
		return -EBUSY;
	}
	    
	if (cam->open_count > 0) {
		DBG("Camera already open\n");
		return -EBUSY;
	}
	
	if (!cam->raw_image) {
		cam->raw_image = rvmalloc(CPIA_MAX_IMAGE_SIZE);
		if (!cam->raw_image)
			return -ENOMEM;
	}

	if (!cam->decompressed_frame.data) {
		cam->decompressed_frame.data = rvmalloc(CPIA_MAX_FRAME_SIZE);
		if (!cam->decompressed_frame.data) {
			rvfree(cam->raw_image, CPIA_MAX_IMAGE_SIZE);
			cam->raw_image = NULL;
			return -ENOMEM;
		}
	}
	
	/* open cpia */
	if (cam->ops->open(cam->lowlevel_data)) {
		rvfree(cam->decompressed_frame.data, CPIA_MAX_FRAME_SIZE);
		cam->decompressed_frame.data = NULL;
		rvfree(cam->raw_image, CPIA_MAX_IMAGE_SIZE);
		cam->raw_image = NULL;
		return -ENODEV;
	}
	
	/* reset the camera */
	if ((i = reset_camera(cam)) != 0) {
		cam->ops->close(cam->lowlevel_data);
		rvfree(cam->decompressed_frame.data, CPIA_MAX_FRAME_SIZE);
		cam->decompressed_frame.data = NULL;
		rvfree(cam->raw_image, CPIA_MAX_IMAGE_SIZE);
		cam->raw_image = NULL;
		return i;
	}
	
	/* Set ownership of /proc/cpia/videoX to current user */
	if(cam->proc_entry)
		cam->proc_entry->uid = current->uid;

	/* set mark for loading first frame uncompressed */
	cam->first_frame = 1;

	/* init it to something */
	cam->mmap_kludge = 0;
	
	++cam->open_count;
#ifdef MODULE
	MOD_INC_USE_COUNT;
#endif
	return 0;
}

static void cpia_close(struct video_device *dev)
{
	struct cam_data *cam;

	cam = dev->priv;

	if (cam->ops) {
	        /* Return ownership of /proc/cpia/videoX to root */
		if(cam->proc_entry)
			cam->proc_entry->uid = 0;
	
		/* save camera state for later open (developers guide ch 3.5.3) */
		save_camera_state(cam);

		/* GotoLoPower */
		goto_low_power(cam);

		/* Update the camera ststus */
		do_command(cam, CPIA_COMMAND_GetCameraStatus, 0, 0, 0, 0);

		/* cleanup internal state stuff */
		free_frames(cam->frame);

		/* close cpia */
		cam->ops->close(cam->lowlevel_data);
	}

	if (--cam->open_count == 0) {
		/* clean up capture-buffers */
		if (cam->raw_image) {
			rvfree(cam->raw_image, CPIA_MAX_IMAGE_SIZE);
			cam->raw_image = NULL;
		}

		if (cam->decompressed_frame.data) {
			rvfree(cam->decompressed_frame.data, CPIA_MAX_FRAME_SIZE);
			cam->decompressed_frame.data = NULL;
		}

		if (cam->frame_buf)
			free_frame_buf(cam);

		if (!cam->ops) {
			video_unregister_device(dev);
			kfree(cam);
		}
	}
	

#ifdef MODULE
	MOD_DEC_USE_COUNT;
#endif
	return;
}

static long cpia_read(struct video_device *dev, char *buf,
                      unsigned long count, int noblock)
{
	struct cam_data *cam = dev->priv;

	/* make this _really_ smp and multithredi-safe */
	if (down_interruptible(&cam->busy_lock))
		return -EINTR;

	if (!buf) {
		DBG("buf NULL\n");
		up(&cam->busy_lock);
		return -EINVAL;
	}

	if (!count) {
		DBG("count 0\n");
		up(&cam->busy_lock);
		return 0;
	}

	if (!cam->ops) {
		DBG("ops NULL\n");
		up(&cam->busy_lock);
		return -ENODEV;
	}

	/* upload frame */
	cam->decompressed_frame.state = FRAME_READY;
	cam->mmap_kludge=0;
	fetch_frame(cam);
	if (cam->decompressed_frame.state != FRAME_DONE) {
		DBG("upload failed %d/%d\n", cam->decompressed_frame.count,
		    cam->decompressed_frame.state);
		up(&cam->busy_lock);
		return -EIO;
	}
	cam->decompressed_frame.state = FRAME_UNUSED;

	/* copy data to user space */
	if (cam->decompressed_frame.count > count) {
		DBG("count wrong: %d, %lu\n", cam->decompressed_frame.count,
		    count);
		up(&cam->busy_lock);
		return -EFAULT;
	}
	if (copy_to_user(buf, cam->decompressed_frame.data,
	                cam->decompressed_frame.count)) {
		DBG("copy_to_user failed\n");
		up(&cam->busy_lock);
		return -EFAULT;
	}

	up(&cam->busy_lock);
	return cam->decompressed_frame.count;
}

static int cpia_ioctl(struct video_device *dev, unsigned int ioctlnr, void *arg)
{
	struct cam_data *cam = dev->priv;
	int retval = 0;

	if (!cam || !cam->ops)
		return -ENODEV;
	
	/* make this _really_ smp-safe */
	if (down_interruptible(&cam->busy_lock))
		return -EINTR;

	//DBG("cpia_ioctl: %u\n", ioctlnr);

	switch (ioctlnr) {
	/* query capabilites */
	case VIDIOCGCAP:
	{
		struct video_capability b;

		DBG("VIDIOCGCAP\n");
		strcpy(b.name, "CPiA Camera");
		b.type = VID_TYPE_CAPTURE;
		b.channels = 1;
		b.audios = 0;
		b.maxwidth = 352;	/* VIDEOSIZE_CIF */
		b.maxheight = 288;
		b.minwidth = 48;	/* VIDEOSIZE_48_48 */
		b.minheight = 48;

		if (copy_to_user(arg, &b, sizeof(b)))
			retval = -EFAULT;

		break;
	}

	/* get/set video source - we are a camera and nothing else */
	case VIDIOCGCHAN:
	{
		struct video_channel v;

		DBG("VIDIOCGCHAN\n");
		if (copy_from_user(&v, arg, sizeof(v))) {
			retval = -EFAULT;
			break;
		}
		if (v.channel != 0) {
			retval = -EINVAL;
			break;
		}

		v.channel = 0;
		strcpy(v.name, "Camera");
		v.tuners = 0;
		v.flags = 0;
		v.type = VIDEO_TYPE_CAMERA;
		v.norm = 0;

		if (copy_to_user(arg, &v, sizeof(v)))
			retval = -EFAULT;
		break;
	}
	
	case VIDIOCSCHAN:
	{
		int v;

		DBG("VIDIOCSCHAN\n");
		if (copy_from_user(&v, arg, sizeof(v)))
			retval = -EFAULT;

		if (retval == 0 && v != 0)
			retval = -EINVAL;

		break;
	}

	/* image properties */
	case VIDIOCGPICT:
		DBG("VIDIOCGPICT\n");
		if (copy_to_user(arg, &cam->vp, sizeof(struct video_picture)))
			retval = -EFAULT;
		break;
	
	case VIDIOCSPICT:
	{
		struct video_picture vp;

		DBG("VIDIOCSPICT\n");

		/* copy_from_user */
		if (copy_from_user(&vp, arg, sizeof(vp))) {
			retval = -EFAULT;
			break;
		}

		/* check validity */
		DBG("palette: %d\n", vp.palette);
		DBG("depth: %d\n", vp.depth);
		if (!valid_mode(vp.palette, vp.depth)) {
			retval = -EINVAL;
			break;
		}

		down(&cam->param_lock);
		/* brightness, colour, contrast need no check 0-65535 */
		memcpy( &cam->vp, &vp, sizeof(vp) );
		/* update cam->params.colourParams */
		cam->params.colourParams.brightness = vp.brightness*100/65535;
		cam->params.colourParams.contrast = vp.contrast*100/65535;
		cam->params.colourParams.saturation = vp.colour*100/65535;
		/* contrast is in steps of 8, so round */
		cam->params.colourParams.contrast =
			((cam->params.colourParams.contrast + 3) / 8) * 8;
		if (cam->params.version.firmwareVersion == 1 &&
		    cam->params.version.firmwareRevision == 2 &&
		    cam->params.colourParams.contrast > 80) {
			/* 1-02 firmware limits contrast to 80 */
			cam->params.colourParams.contrast = 80;
		}

		/* queue command to update camera */
		cam->cmd_queue |= COMMAND_SETCOLOURPARAMS;
		up(&cam->param_lock);
		DBG("VIDIOCSPICT: %d / %d // %d / %d / %d / %d\n",
		    vp.depth, vp.palette, vp.brightness, vp.hue, vp.colour,
		    vp.contrast);
		break;
	}

	/* get/set capture window */
	case VIDIOCGWIN:
		DBG("VIDIOCGWIN\n");

		if (copy_to_user(arg, &cam->vw, sizeof(struct video_window)))
			retval = -EFAULT;
		break;
	
	case VIDIOCSWIN:
	{
		/* copy_from_user, check validity, copy to internal structure */
		struct video_window vw;
		DBG("VIDIOCSWIN\n");
		if (copy_from_user(&vw, arg, sizeof(vw))) {
			retval = -EFAULT;
			break;
		}

		if (vw.clipcount != 0) {    /* clipping not supported */
			retval = -EINVAL;
			break;
		}
		if (vw.clips != NULL) {     /* clipping not supported */
			retval = -EINVAL;
			break;
		}

		/* we set the video window to something smaller or equal to what
		* is requested by the user???
		*/
		down(&cam->param_lock);
		if (vw.width != cam->vw.width || vw.height != cam->vw.height) {
			int video_size = match_videosize(vw.width, vw.height);

			if (video_size < 0) {
				retval = -EINVAL;
				up(&cam->param_lock);
				break;
			}
			cam->video_size = video_size;
			set_vw_size(cam);
			DBG("%d / %d\n", cam->vw.width, cam->vw.height);
			cam->cmd_queue |= COMMAND_SETFORMAT;
		}

		// FIXME needed??? memcpy(&cam->vw, &vw, sizeof(vw));
		up(&cam->param_lock);

		/* setformat ignored by camera during streaming,
		 * so stop/dispatch/start */
		if (cam->cmd_queue & COMMAND_SETFORMAT) {
			DBG("\n");
			dispatch_commands(cam);
		}
		DBG("%d/%d:%d\n", cam->video_size,
		    cam->vw.width, cam->vw.height);
		break;
	}

	/* mmap interface */
	case VIDIOCGMBUF:
	{
		struct video_mbuf vm;
		int i;

		DBG("VIDIOCGMBUF\n");
		memset(&vm, 0, sizeof(vm));
		vm.size = CPIA_MAX_FRAME_SIZE*FRAME_NUM;
		vm.frames = FRAME_NUM;
		for (i = 0; i < FRAME_NUM; i++)
			vm.offsets[i] = CPIA_MAX_FRAME_SIZE * i;

		if (copy_to_user((void *)arg, (void *)&vm, sizeof(vm)))
			retval = -EFAULT;

		break;
	}
	
	case VIDIOCMCAPTURE:
	{
		struct video_mmap vm;
		int video_size;

		if (copy_from_user((void *)&vm, (void *)arg, sizeof(vm))) {
			retval = -EFAULT;
			break;
		}
#if 1
		DBG("VIDIOCMCAPTURE: %d / %d / %dx%d\n", vm.format, vm.frame,
		    vm.width, vm.height);
#endif
		if (vm.frame<0||vm.frame>FRAME_NUM) {
			retval = -EINVAL;
			break;
		}

		/* set video format */
		cam->vp.palette = vm.format;
		switch(vm.format) {
		case VIDEO_PALETTE_GREY:
		case VIDEO_PALETTE_RGB555:
		case VIDEO_PALETTE_RGB565:
		case VIDEO_PALETTE_YUV422:
		case VIDEO_PALETTE_YUYV:
		case VIDEO_PALETTE_UYVY:
			cam->vp.depth = 16;
			break;
		case VIDEO_PALETTE_RGB24:
			cam->vp.depth = 24;
			break;
		case VIDEO_PALETTE_RGB32:
			cam->vp.depth = 32;
			break;
		default:
			retval = -EINVAL;
			break;
		}
		if (retval)
			break;

		/* set video size */
		video_size = match_videosize(vm.width, vm.height);
		if (cam->video_size < 0) {
			retval = -EINVAL;
			break;
		}
		if (video_size != cam->video_size) {
			cam->video_size = video_size;
			set_vw_size(cam);
			cam->cmd_queue |= COMMAND_SETFORMAT;
			dispatch_commands(cam);
		}
#if 0
		DBG("VIDIOCMCAPTURE: %d / %d/%d\n", cam->video_size,
		    cam->vw.width, cam->vw.height);
#endif
		/* according to v4l-spec we must start streaming here */
		cam->mmap_kludge = 1;
		retval = capture_frame(cam, &vm);

		break;
	}
	
	case VIDIOCSYNC:
	{
		int frame;

		if (copy_from_user((void *)&frame, arg, sizeof(int))) {
			retval = -EFAULT;
			break;
		}
		//DBG("VIDIOCSYNC: %d\n", frame);

		if (frame<0 || frame >= FRAME_NUM) {
			retval = -EINVAL;
			break;
		}

		switch (cam->frame[frame].state) {
		case FRAME_UNUSED:
		case FRAME_READY:
		case FRAME_GRABBING:
			DBG("sync to unused frame %d\n", frame);
			retval = -EINVAL;
			break;

		case FRAME_DONE:
			cam->frame[frame].state = FRAME_UNUSED;
			//DBG("VIDIOCSYNC: %d synced\n", frame);
			break;
		}
		if (retval == -EINTR) {
			/* FIXME - xawtv does not handle this nice */
			retval = 0;
		}
		break;
	}

	/* pointless to implement overlay with this camera */
	case VIDIOCCAPTURE:
		retval = -EINVAL;
		break;
	case VIDIOCGFBUF:
		retval = -EINVAL;
		break;
	case VIDIOCSFBUF:
		retval = -EINVAL;
		break;
	case VIDIOCKEY:
		retval = -EINVAL;
		break;

	/* tuner interface - we have none */
	case VIDIOCGTUNER:
		retval = -EINVAL;
		break;
	case VIDIOCSTUNER:
		retval = -EINVAL;
		break;
	case VIDIOCGFREQ:
		retval = -EINVAL;
		break;
	case VIDIOCSFREQ:
		retval = -EINVAL;
		break;

	/* audio interface - we have none */
	case VIDIOCGAUDIO:
		retval = -EINVAL;
		break;
	case VIDIOCSAUDIO:
		retval = -EINVAL;
		break;
	default:
		retval = -ENOIOCTLCMD;
		break;
	}

	up(&cam->param_lock);
	up(&cam->busy_lock);
	return retval;
} 

/* FIXME */
static int cpia_mmap(struct video_device *dev, const char *adr,
                     unsigned long size)
{
	unsigned long start = (unsigned long)adr;
	unsigned long page, pos;
	struct cam_data *cam = dev->priv;
	int retval;

	if (!cam || !cam->ops)
		return -ENODEV;
	
	DBG("cpia_mmap: %ld\n", size);

	if (size > FRAME_NUM*CPIA_MAX_FRAME_SIZE)
		return -EINVAL;

	if (!cam || !cam->ops)
		return -ENODEV;
	
	/* make this _really_ smp-safe */
	if (down_interruptible(&cam->busy_lock))
		return -EINTR;

	if (!cam->frame_buf) {	/* we do lazy allocation */
		if ((retval = allocate_frame_buf(cam))) {
			up(&cam->busy_lock);
			return retval;
		}
	}

	pos = (unsigned long)(cam->frame_buf);
	while (size > 0) {
		page = kvirt_to_pa(pos);
		if (remap_page_range(start, page, PAGE_SIZE, PAGE_SHARED)) {
			up(&cam->busy_lock);
			return -EAGAIN;
		}
		start += PAGE_SIZE;
		pos += PAGE_SIZE;
		if (size > PAGE_SIZE)
			size -= PAGE_SIZE;
		else
			size = 0;
	}

	DBG("cpia_mmap: %ld\n", size);
	up(&cam->busy_lock);

	return 0;
}

int cpia_video_init(struct video_device *vdev)
{
#ifdef CONFIG_PROC_FS
	create_proc_cpia_cam(vdev->priv);
#endif
	return 0;
}

static struct video_device cpia_template = {
	name:		"CPiA Camera",
	type:		VID_TYPE_CAPTURE,
	hardware:	VID_HARDWARE_CPIA,      /* FIXME */
	open:		cpia_open,
	close:		cpia_close,
	read:		cpia_read,
	ioctl:		cpia_ioctl,
	mmap:		cpia_mmap,
	initialize:	cpia_video_init,
	minor:		-1,
};

/* initialise cam_data structure  */
static void reset_camera_struct(struct cam_data *cam)
{
	/* The following parameter values are the defaults from
	 * "Software Developer's Guide for CPiA Cameras".  Any changes
	 * to the defaults are noted in comments. */
	cam->params.colourParams.brightness = 50;
	cam->params.colourParams.contrast = 48;
	cam->params.colourParams.saturation = 50;
	cam->params.exposure.gainMode = 2;
	cam->params.exposure.expMode = 2;		/* AEC */
	cam->params.exposure.compMode = 1;
	cam->params.exposure.centreWeight = 1;
	cam->params.exposure.gain = 0;
	cam->params.exposure.fineExp = 0;
	cam->params.exposure.coarseExpLo = 185;
	cam->params.exposure.coarseExpHi = 0;
	cam->params.exposure.redComp = 220;
	cam->params.exposure.green1Comp = 214;
	cam->params.exposure.green2Comp = 214;
	cam->params.exposure.blueComp = 230;
	cam->params.colourBalance.balanceModeIsAuto = 1;
	cam->params.colourBalance.redGain = 32;
	cam->params.colourBalance.greenGain = 6;
	cam->params.colourBalance.blueGain = 92;
	cam->params.apcor.gain1 = 0x1c;
	cam->params.apcor.gain2 = 0x1a;
	cam->params.apcor.gain4 = 0x2d;
	cam->params.apcor.gain8 = 0x2a;
	cam->params.flickerControl.flickerMode = 0;
	cam->params.flickerControl.coarseJump = 
		flicker_jumps[cam->mainsFreq]
		             [cam->params.sensorFps.baserate]
		             [cam->params.sensorFps.divisor];
	cam->params.vlOffset.gain1 = 24;
	cam->params.vlOffset.gain2 = 28;
	cam->params.vlOffset.gain4 = 30;
	cam->params.vlOffset.gain8 = 30;
	cam->params.compressionParams.hysteresis = 3;
	cam->params.compressionParams.threshMax = 11;
	cam->params.compressionParams.smallStep = 1;
	cam->params.compressionParams.largeStep = 3;
	cam->params.compressionParams.decimationHysteresis = 2;
	cam->params.compressionParams.frDiffStepThresh = 5;
	cam->params.compressionParams.qDiffStepThresh = 3;
	cam->params.compressionParams.decimationThreshMod = 2;
	/* End of default values from Software Developer's Guide */
	
	cam->transfer_rate = 0;
	
	/* Set Sensor FPS to 15fps. This seems better than 30fps
	 * for indoor lighting. */
	cam->params.sensorFps.divisor = 1;
	cam->params.sensorFps.baserate = 1;
	
	cam->params.yuvThreshold.yThreshold = 15; /* FIXME? */
	cam->params.yuvThreshold.uvThreshold = 15; /* FIXME? */
	
	cam->params.format.subSample = SUBSAMPLE_422;
	cam->params.format.yuvOrder = YUVORDER_YUYV;
	
	cam->params.compression.mode = CPIA_COMPRESSION_AUTO;
	cam->params.compressionTarget.frTargeting =
		CPIA_COMPRESSION_TARGET_QUALITY;
	cam->params.compressionTarget.targetFR = 7; /* FIXME? */
	cam->params.compressionTarget.targetQ = 10; /* FIXME? */

	cam->video_size = VIDEOSIZE_CIF;
	
	cam->vp.colour = 32768;      /* 50% */
	cam->vp.hue = 32768;         /* 50% */
	cam->vp.brightness = 32768;  /* 50% */
	cam->vp.contrast = 32768;    /* 50% */
	cam->vp.whiteness = 0;       /* not used -> grayscale only */
	cam->vp.depth = 0;           /* FIXME: to be set by user? */
	cam->vp.palette = VIDEO_PALETTE_RGB24;         /* FIXME: to be set by user? */

	cam->vw.x = 0;
	cam->vw.y = 0;
	set_vw_size(cam);
	cam->vw.chromakey = 0;
	/* PP NOTE: my extension to use vw.flags for this, bear it! */
	cam->vw.flags = 0;
	cam->vw.clipcount = 0;
	cam->vw.clips = NULL;

	cam->cmd_queue = COMMAND_NONE;
	cam->first_frame = 0;

	return;
}

/* initialize cam_data structure  */
static void init_camera_struct(struct cam_data *cam,
                               struct cpia_camera_ops *ops )
{
	int i;

	/* Default everything to 0 */
	memset(cam, 0, sizeof(struct cam_data));

	cam->ops = ops;
	init_MUTEX(&cam->param_lock);
	init_MUTEX(&cam->busy_lock);

	reset_camera_struct(cam);

	cam->proc_entry = NULL;

	memcpy(&cam->vdev, &cpia_template, sizeof(cpia_template));
	cam->vdev.priv = cam;
	
	cam->curframe = 0;
	for (i = 0; i < FRAME_NUM; i++) {
		cam->frame[i].width = 0;
		cam->frame[i].height = 0;
		cam->frame[i].state = FRAME_UNUSED;
		cam->frame[i].data = NULL;
	}
	cam->decompressed_frame.width = 0;
	cam->decompressed_frame.height = 0;
	cam->decompressed_frame.state = FRAME_UNUSED;
	cam->decompressed_frame.data = NULL;
}

struct cam_data *cpia_register_camera(struct cpia_camera_ops *ops, void *lowlevel)
{
        struct cam_data *camera;
	
	/* Need a lock when adding/removing cameras.  This doesn't happen
	 * often and doesn't take very long, so grabbing the kernel lock
	 * should be OK. */
	
	if ((camera = kmalloc(sizeof(struct cam_data), GFP_KERNEL)) == NULL) {
		unlock_kernel();
		return NULL;
	}
	
	init_camera_struct( camera, ops );
	camera->lowlevel_data = lowlevel;
	
	/* register v4l device */
	if (video_register_device(&camera->vdev, VFL_TYPE_GRABBER) == -1) {
		kfree(camera);
		unlock_kernel();
		printk(KERN_DEBUG "video_register_device failed\n");
		return NULL;
	}

	/* get version information from camera: open/reset/close */

	/* open cpia */
	if (camera->ops->open(camera->lowlevel_data))
		return camera;
	
	/* reset the camera */
	if (reset_camera(camera) != 0) {
		camera->ops->close(camera->lowlevel_data);
		return camera;
	}

	/* close cpia */
	camera->ops->close(camera->lowlevel_data);

/* Eh? Feeling happy? - jerdfelt */
/*
	camera->ops->open(camera->lowlevel_data);
	camera->ops->close(camera->lowlevel_data);
*/
	
	printk(KERN_INFO "  CPiA Version: %d.%02d (%d.%d)\n",
	       camera->params.version.firmwareVersion,
	       camera->params.version.firmwareRevision,
	       camera->params.version.vcVersion,
	       camera->params.version.vcRevision);
	printk(KERN_INFO "  CPiA PnP-ID: %04x:%04x:%04x\n",
	       camera->params.pnpID.vendor,
	       camera->params.pnpID.product,
	       camera->params.pnpID.deviceRevision);
	printk(KERN_INFO "  VP-Version: %d.%d %04x\n",
	       camera->params.vpVersion.vpVersion,
	       camera->params.vpVersion.vpRevision,
	       camera->params.vpVersion.cameraHeadID);

	return camera;
}

void cpia_unregister_camera(struct cam_data *cam)
{
	if (!cam->open_count) {
		DBG("unregistering video\n");
		video_unregister_device(&cam->vdev);
	} else {
		LOG("/dev/video%d removed while open, "
		    "deferring video_unregister_device\n", cam->vdev.minor);
		DBG("camera open -- setting ops to NULL\n");
		cam->ops = NULL;
	}
	
#ifdef CONFIG_PROC_FS
	DBG("destroying /proc/cpia/video%d\n", cam->vdev.minor);
	destroy_proc_cpia_cam(cam);
#endif	
	if (!cam->open_count) {
		DBG("freeing camera\n");
		kfree(cam);
	}
}

/****************************************************************************
 *
 *  Module routines
 *
 ***************************************************************************/

#ifdef MODULE
int init_module(void)
{
	printk(KERN_INFO "%s v%d.%d.%d\n", ABOUT,
	       CPIA_MAJ_VER, CPIA_MIN_VER, CPIA_PATCH_VER);
#ifdef CONFIG_PROC_FS
	proc_cpia_create();
#endif
#ifdef CONFIG_KMOD
#ifdef CONFIG_VIDEO_CPIA_PP_MODULE
	request_module("cpia_pp");
#endif
#ifdef CONFIG_VIDEO_CPIA_USB_MODULE
	request_module("cpia_usb");
#endif
#endif
return 0;
}

void cleanup_module(void)
{
#ifdef CONFIG_PROC_FS
	proc_cpia_destroy();
#endif
}

#else

int cpia_init(struct video_init *unused)
{
	printk(KERN_INFO "%s v%d.%d.%d\n", ABOUT,
	       CPIA_MAJ_VER, CPIA_MIN_VER, CPIA_PATCH_VER);
#ifdef CONFIG_PROC_FS
	proc_cpia_create();
#endif

#ifdef CONFIG_VIDEO_CPIA_PP
	cpia_pp_init();
#endif
#ifdef CONFIG_KMOD
#ifdef CONFIG_VIDEO_CPIA_PP_MODULE
	request_module("cpia_pp");
#endif

#ifdef CONFIG_VIDEO_CPIA_USB_MODULE
	request_module("cpia_usb");
#endif
#endif	/* CONFIG_KMOD */
#ifdef CONFIG_VIDEO_CPIA_USB
	cpia_usb_init();
#endif
	return 0;
}

/* Exported symbols for modules. */

EXPORT_SYMBOL(cpia_register_camera);
EXPORT_SYMBOL(cpia_unregister_camera);

#endif
