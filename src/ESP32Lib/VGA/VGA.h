/*
	Author: bitluni 2019
	License:
	Creative Commons Attribution ShareAlike 4.0
	https://creativecommons.org/licenses/by-sa/4.0/

	For further details check out:
		https://youtube.com/bitlunislab
		https://github.com/bitluni
		http://bitluni.net
*/
#pragma once

#include "../I2S/I2S.h"

enum vmodeproperties {
	hFront,	hSync, hBack, hRes,	vFront,	vSync, vBack, vRes,	vDiv, hSyncPolarity, vSyncPolarity,	r1sdm0,	r1sdm1,	r1sdm2,	r1odiv,	r0sdm2,	r0odiv
};

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// VGA STANDARD MODES (IN STANDARD MODE TIMING DEPENDS ON PWM_AUDIO SO ALL MACHINES SHARE SAME VIDEO MODES)
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define VgaMode_320x240 { 8, 48, 24, 320, 10, 2, 33, 480, 2, 1, 1, 47,84,7,7,6,6 } // 31469 / 59.94
#define VgaMode_320x240_scanlines { 8, 48, 24, 320, 10, 2, 33, 480, 1, 1, 1, 47,84,7,7,6,6 } // Same as above but without double scanlines

#define VgaMode_360x200 { 9, 54, 27, 360, 13, 2, 34, 400, 2, 1, 0, 249,83,7,6,6,5 } // 31466.7 / 70.082
#define VgaMode_360x200_scanlines { 9, 54, 27, 360, 13, 2, 34, 400, 1, 1, 0, 249,83,7,6,6,5 } // Same as above but without double scanlines

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// VGA MODES WITH SAME FREQUENCY AS REAL MACHINES
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// 48K 50hz VGA
#define VgaMode_320x240_50_48 { 8, 42, 50, 320, 27, 6, 53, 720, 3, 1, 1, 0,44,6,4,8,5 } // 40365 / 50.0801282
#define VgaMode_320x240_50_48_scanlines { 8, 32, 40, 320, 3, 10, 16, 480, 1, 1, 0, 59,45,5,7,5,7 } // 25491 / 50.0801282
#define VgaMode_360x200_50_48 { 18, 36, 54, 360, 31, 3, 33, 600, 3, 1, 1, 51,97,5,4,7,5 } // 33403 / 50.0801282
#define VgaMode_360x200_50_48_scanlines { 8, 32, 40, 360, 3, 10, 6, 400, 1, 1, 0, 151,59,5,8,8,11 } // 20984 / 50.0801282

// TK 50hz
#define VgaMode_320x240_50_TK { 8, 42, 50, 320, 27, 6, 53, 720, 3, 1, 1, 142, 53, 6, 4, 8, 5 } // 40513 / 50.2638854
#define VgaMode_320x240_50_TK_scanlines { 8, 32, 40, 320, 3, 10, 16, 480, 1, 1, 0, 205,71,8,10,5,7 }  // 25584 / 50.2638854
#define VgaMode_360x200_50_TK { 18, 36, 54, 360, 31, 3, 33, 600, 3, 1, 1, 174,251,6,5,7,5 } // 33526 / 50.2638854
#define VgaMode_360x200_50_TK_scanlines { 9, 54, 27, 360, 35, 12, 2, 400, 1, 1, 0, 222,47,8,10,5,7 } // 22568 / 50.2638854

// TK 60hz
#define VgaMode_320x240_60_TK { 8, 48, 24, 320, 10, 2, 33, 480, 2, 1, 1, 83,14,6,6,6,6 }  // 31425 / 59.856887
#define VgaMode_320x240_60_TK_scanlines { 8, 48, 24, 320, 10, 2, 33, 480, 1, 1, 1, 83,14,6,6,6,6 }  // 31425 / 59.856887
#define VgaMode_360x200_60_TK { 18, 36, 54, 360, 31, 3, 33, 600, 3, 1, 1, 164,87,5,3,7,4 } // 39925 / 59.856887
#define VgaMode_360x200_60_TK_scanlines { 9, 54, 27, 360, 35, 12, 2, 400, 1, 1, 0, 22,24, 8,8,8,8 } // 26876 / 59.856887

// 128K 50hz VGA
#define VgaMode_320x240_50_128 { 8, 42, 50, 320, 27, 6, 53, 720, 3, 1, 1, 224,40,6,4,8,5 } // 40316 / 50.020008
#define VgaMode_320x240_50_128_scanlines { 8, 32, 40, 320, 3, 10, 16, 480, 1, 1, 0, 105,42,5,7,5,7 } // 25460 / 50.020008
#define VgaMode_360x200_50_128 { 18, 36, 54, 360, 31, 3, 33, 600, 3, 1, 1, 194,125,8,6,7,5 } // 33363 / 50.020008
#define VgaMode_360x200_50_128_scanlines { 8, 32, 40, 360, 3, 10, 6, 400, 1, 1, 0, 231,16,7,10,8,11 } // 20958 / 50.020008

// PENTAGON 128K 50hz VGA
#define VgaMode_320x240_50_PENTAGON { 16, 32, 48, 320, 1, 3, 17, 720, 3, 1, 0, 143, 10, 8, 6, 5, 4 } // 36182 / 48.828125 / MSV: 1792
#define VgaMode_320x240_50_PENTAGON_scanlines { 16, 32, 48, 320, 1, 3, 17, 480, 1, 1, 0, 174, 40, 5, 7, 5, 7 } // 24463 / 48.828125
#define VgaMode_360x200_50_PENTAGON {  8, 40, 48, 360, 1, 3, 14, 600, 3, 1, 0,  87, 98, 8, 7, 7, 6 } // 30176 / 48.828125 / MSV: 1792
#define VgaMode_360x200_50_PENTAGON_scanlines {  8, 40, 48, 360, 1, 3, 14, 400, 1, 1, 0,  220, 60, 6, 9, 8, 11 } // 20410 / 48.828125

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TV MODES
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define CrtMode_352x272_TV_48 { 22, 32, 42, 352, 18, 3, 19, 272, 1, 1, 1, 0,128,6,13,8,15 }  // 7 MHZ / 15625 / 50.0801282
#define CrtMode_352x272_TV_TK50 { 22, 32, 42, 352, 18, 3, 19, 272, 1, 1, 1, 148,241,7,15,8,15 } // 15682 / 50.2638854
#define CrtMode_352x224_TV_TK60 { 22, 32, 42, 352, 18, 3, 19, 224, 1, 1, 1, 244,8,8,15,8,15 } // 15802 / 59.856887 /
#define CrtMode_352x272_TV_128 { 22, 32, 42, 352, 18, 3, 19, 272, 1, 1, 1, 194,47,7,14,8,15 } // 15606 / 50.020008
#define CrtMode_352x272_TV_PENTAGON { 22, 32, 42, 352, 18, 3, 19, 272, 1, 1, 1, 133,235,6,14,7,14 } // 15234 / 48.828125

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TS2068 MODE (this port — no upstream equivalent)
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// One fixed 512-active-column physical mode, used for every TS2068 video
// mode (see TS2068-ESPECTRUM-PORT-PLAN.md's "Display" section — the 0xFF
// video-mode bits select a renderer, not the VGA timing itself). 512 active
// columns to natively fit hi-res mode's 512x192; standard/hi-color modes
// pixel-double into the same buffer.
//
// Computed (2026-07-10), not bench-verified on a real monitor: horizontal
// porch/sync/back scaled x1.6 from the proven VgaMode_320x240_60_TK
// (320*1.6=512 exactly, so the two modes share the same ~31425Hz horizontal
// frequency — same line rate as an already-working mode, just more active
// pixels per line). Vertical timing (10,2,33,480,vDiv=2) copied unchanged
// from the same mode: 480/vDiv=2 gives a 240-line framebuffer, close to
// TS2068's real 24-top-border+192-active+24-bottom-border shape. Vertical
// frequency targets that same proven 59.856887Hz rather than the ~60.11Hz
// separately derived from TSTATES_PER_FRAME_2068 (cpuESP.h) — that constant
// governs CPU/audio pacing, a different clock domain in this driver
// architecture, unaffected by the VGA output's own refresh rate; reusing an
// already-monitor-proven rate here is lower-risk than an unproven one.
// r1sdm0/r1sdm1/r1sdm2/r0sdm2 exactly match VgaMode_320x240_60_TK's — same
// VCO frequency family, just a smaller odiv (3 vs 6) for the ~1.6x higher
// pixel clock; r1odiv/r0odiv computed via the exact APLLCalc algorithm in
// tools/VideoModeTool.py (0.0000% frequency error against the 2x-pixel-clock
// target for REV1). PLAN.md's risk register already flags this as needing
// real bench time before trusting it on hardware — this is a mathematically
// self-consistent starting point for that bench pass, not a substitute for it.
//
// Superseded on hardware (2026-11-11, Olimex ESP32-SBC-FabGL): with 512
// active columns the monitor stretches the picture across its full width,
// leaving no side border and a 1.25x-wide aspect. The mode is now 640
// active: VgaMode_320x240_60_TK with every horizontal figure doubled
// (16,96,48,640 on an 800-clock line, so the same proven ~31425 Hz line
// rate) and the pixel clock doubled to ~25.14 MHz by halving the APLL
// divider (r1odiv 6 -> 2, i.e. 2*(odiv+2) = 16 -> 8; r0odiv likewise).
// That is standard 640x480 VGA timing within 0.1%. The 2068's 320x240
// frame (32-pixel side borders, 24-line top/bottom) maps onto it at 2x:
// standard mode pixel-doubles, hi-res is 1:1, each with 64-pixel borders.
#define VgaMode_640x480_60_TS2068 { 16, 96, 48, 640, 10, 2, 33, 480, 2, 1, 1, 83,14,6,2,6,2 }

const unsigned short int vidmodes[30][17]={
	VgaMode_320x240, VgaMode_320x240_scanlines, VgaMode_360x200, VgaMode_360x200_scanlines,
	VgaMode_320x240_50_48, VgaMode_320x240_50_48_scanlines, VgaMode_360x200_50_48,  VgaMode_360x200_50_48_scanlines,
	VgaMode_320x240_50_TK, VgaMode_320x240_50_TK_scanlines, VgaMode_360x200_50_TK,  VgaMode_360x200_50_TK_scanlines,
	VgaMode_320x240_60_TK, VgaMode_320x240_60_TK_scanlines, VgaMode_360x200_60_TK,  VgaMode_360x200_60_TK_scanlines,
	VgaMode_320x240_50_128, VgaMode_320x240_50_128_scanlines, VgaMode_360x200_50_128, VgaMode_360x200_50_128_scanlines,
	VgaMode_320x240_50_PENTAGON , VgaMode_320x240_50_PENTAGON_scanlines, VgaMode_360x200_50_PENTAGON, VgaMode_360x200_50_PENTAGON_scanlines,
	CrtMode_352x272_TV_48 ,	CrtMode_352x272_TV_TK50 , CrtMode_352x224_TV_TK60 ,	CrtMode_352x272_TV_128,	CrtMode_352x272_TV_PENTAGON,
	VgaMode_640x480_60_TS2068
};

// Index of the TS2068 mode above within vidmodes[] — SCLD_VGA_MODE_INDEX,
// not TS2068_VGA_MODE_INDEX, to match the SCLD.h/SCLD.cpp naming this port
// otherwise uses for everything TS2068-specific.
#define SCLD_VGA_MODE_INDEX 29

class VGA : public I2S {

  public:

	VGA(const int i2sIndex = 0);

	bool init(int mode, const int *pinMap, const int bitCount, const int clockPin = -1);

	int mode;

	int CenterH = 0;
	int CenterV = 0;

  protected:

	virtual void initSyncBits() = 0;
	virtual long syncBits(bool h, bool v) = 0;

	long vsyncBit;
	long hsyncBit;
	long vsyncBitI;
	long hsyncBitI;

	virtual void allocateLineBuffers();
	virtual void allocateLineBuffers(void **frameBuffer);
	virtual void propagateResolution(const int xres, const int yres) = 0;

};
