// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// $Log:$
//
// DESCRIPTION:
//	DOOM graphics stuff for X11, UNIX.
//
//-----------------------------------------------------------------------------

static const char
    rcsid[] = "$Id: i_x.c,v 1.6 1997/02/03 22:45:10 b1 Exp $";

#include "config.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_event.h"
#include "d_main.h"
#include "i_video.h"
#include "i_system.h"
#include "z_zone.h"

#include "tables.h"
#include "doomkeys.h"

#include "doomgeneric.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include <fcntl.h>

#include <stdarg.h>

#include <sys/types.h>

// #define CMAP256

struct FB_BitField
{
    uint32_t offset; /* beginning of bitfield	*/
    uint32_t length; /* length of bitfield		*/
};

struct FB_ScreenInfo
{
    uint32_t xres; /* visible resolution		*/
    uint32_t yres;
    uint32_t xres_virtual; /* virtual resolution		*/
    uint32_t yres_virtual;

    uint32_t bits_per_pixel; /* guess what			*/

    /* >1 = FOURCC			*/
    struct FB_BitField red;   /* bitfield in s_Fb mem if true color, */
    struct FB_BitField green; /* else only length is significant */
    struct FB_BitField blue;
    struct FB_BitField transp; /* transparency			*/
};

static struct FB_ScreenInfo s_Fb;
int fb_scaling = 1;
int usemouse = 0;

#ifdef CMAP256

boolean palette_changed;
struct color colors[256];

#else // CMAP256

static struct color colors[256];

#endif // CMAP256

void I_GetEvent(void);

// The screen buffer; this is modified to draw things to the screen

byte *I_VideoBuffer = NULL;

// If true, game is running as a screensaver

boolean screensaver_mode = false;

// Flag indicating whether the screen is currently visible:
// when the screen isnt visible, don't render the screen

boolean screenvisible;

// Mouse acceleration
//
// This emulates some of the behavior of DOS mouse drivers by increasing
// the speed when the mouse is moved fast.
//
// The mouse input values are input directly to the game, but when
// the values exceed the value of mouse_threshold, they are multiplied
// by mouse_acceleration to increase the speed.

float mouse_acceleration = 2.0;
int mouse_threshold = 10;

// Gamma correction level to use

int usegamma = 0;

typedef struct
{
    byte r;
    byte g;
    byte b;
} col_t;

// Palette converted to RGB565

static uint16_t rgb565_palette[256];
static uint32_t rgba_palette[256];

static volatile uint64_t perf_scale_ns = 0;

#define ASYNC_PRESENT_QUEUE_DEPTH 3

static int async_present_enabled = 0;
static int async_present_thread_started = 0;
static int async_present_shutdown = 0;
static int async_present_q_head = 0;
static int async_present_q_tail = 0;
static int async_present_q_count = 0;
static pthread_t async_present_thread;
static pthread_mutex_t async_present_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t async_present_cv = PTHREAD_COND_INITIALIZER;
static uint8_t async_present_queue[ASYNC_PRESENT_QUEUE_DEPTH][SCREENWIDTH * SCREENHEIGHT];

static inline uint64_t video_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void cmap_to_rgb565(uint16_t *out, uint8_t *in, int in_pixels)
{
    int i, j;
    struct color c;
    uint16_t r, g, b;

    for (i = 0; i < in_pixels; i++)
    {
        c = colors[*in];
        r = ((uint16_t)(c.r >> 3)) << 11;
        g = ((uint16_t)(c.g >> 2)) << 5;
        b = ((uint16_t)(c.b >> 3)) << 0;
        *out = (r | g | b);

        in++;
        for (j = 0; j < fb_scaling; j++)
        {
            out++;
        }
    }
}

void cmap_to_fb(uint8_t *out, uint8_t *in, int in_pixels)
{
    int i, k;

    if (s_Fb.bits_per_pixel == 16)
    {
        uint16_t *out16 = (uint16_t *)out;

        if (fb_scaling == 1)
        {
            for (i = 0; i < in_pixels; i++)
            {
                out16[i] = rgb565_palette[in[i]];
            }
            return;
        }

        for (i = 0; i < in_pixels; i++)
        {
            uint16_t p = rgb565_palette[*in++];
            for (k = 0; k < fb_scaling; k++)
            {
                *out16++ = p;
            }
        }
        return;
    }

    if (s_Fb.bits_per_pixel == 32)
    {
        uint32_t *out32 = (uint32_t *)out;

        if (fb_scaling == 1)
        {
            for (i = 0; i < in_pixels; i++)
            {
                out32[i] = rgba_palette[in[i]];
            }
            return;
        }

        if (fb_scaling == 5)
        {
            for (i = 0; i < in_pixels; i++)
            {
                uint32_t p = rgba_palette[*in++];
                out32[0] = p;
                out32[1] = p;
                out32[2] = p;
                out32[3] = p;
                out32[4] = p;
                out32 += 5;
            }
            return;
        }

        for (i = 0; i < in_pixels; i++)
        {
            uint32_t p = rgba_palette[*in++];
            for (k = 0; k < fb_scaling; k++)
            {
                *out32++ = p;
            }
        }
        return;
    }

    // no clue how to convert this
    I_Error("No idea how to convert %d bpp pixels", s_Fb.bits_per_pixel);
}

static void I_BlitScaledFrame(const unsigned char *src_frame)
{
    int y;
    int x_offset;
    int row_stride_bytes;
    int scaled_row_bytes;
    unsigned char *line_in;
    unsigned char *line_out;

    x_offset = (((s_Fb.xres - (SCREENWIDTH * fb_scaling)) * s_Fb.bits_per_pixel / 8)) / 2;
    row_stride_bytes = s_Fb.xres * (s_Fb.bits_per_pixel / 8);
    scaled_row_bytes = SCREENWIDTH * fb_scaling * (s_Fb.bits_per_pixel / 8);

    line_in = (unsigned char *)src_frame;
    line_out = (unsigned char *)DG_ScreenBuffer;

    y = SCREENHEIGHT;

    while (y--)
    {
        unsigned char *line_out_row0;
        int i;
        line_out_row0 = line_out + x_offset;
#ifdef CMAP256
        if (fb_scaling == 1)
        {
            memcpy(line_out_row0, line_in, SCREENWIDTH);
        }
        else
        {
            int j;

            for (j = 0; j < SCREENWIDTH; j++)
            {
                int k;
                for (k = 0; k < fb_scaling; k++)
                {
                    line_out_row0[j * fb_scaling + k] = line_in[j];
                }
            }
        }
#else
        // Convert+scale once, then duplicate vertically with memcpy.
        cmap_to_fb((void *)line_out_row0, (void *)line_in, SCREENWIDTH);
#endif

        for (i = 1; i < fb_scaling; i++)
        {
            memcpy(line_out_row0 + (i * row_stride_bytes), line_out_row0, scaled_row_bytes);
        }

        line_out += fb_scaling * row_stride_bytes;
        line_in += SCREENWIDTH;
    }
}

static void *I_AsyncPresentWorker(void *arg)
{
    (void)arg;

    for (;;)
    {
        int slot;
        uint64_t scale_start;
        uint64_t scale_ns;

        pthread_mutex_lock(&async_present_mutex);
        while (async_present_q_count == 0 && !async_present_shutdown)
        {
            pthread_cond_wait(&async_present_cv, &async_present_mutex);
        }

        if (async_present_q_count == 0 && async_present_shutdown)
        {
            pthread_mutex_unlock(&async_present_mutex);
            break;
        }

        slot = async_present_q_head;
        async_present_q_head = (async_present_q_head + 1) % ASYNC_PRESENT_QUEUE_DEPTH;
        async_present_q_count--;
        pthread_cond_broadcast(&async_present_cv);
        pthread_mutex_unlock(&async_present_mutex);

        scale_start = video_now_ns();
        I_BlitScaledFrame(async_present_queue[slot]);
        scale_ns = video_now_ns() - scale_start;
        __sync_fetch_and_add(&perf_scale_ns, scale_ns);
        DG_DrawFrame();
    }

    return NULL;
}

static void I_InitAsyncPresent(void)
{
    int rc;

    if (!async_present_enabled || async_present_thread_started)
        return;

    async_present_shutdown = 0;
    async_present_q_head = 0;
    async_present_q_tail = 0;
    async_present_q_count = 0;

    rc = pthread_create(&async_present_thread, NULL, I_AsyncPresentWorker, NULL);
    if (rc != 0)
    {
        async_present_enabled = 0;
        printf("WARN: async present thread create failed (rc=%d), falling back to sync present\n", rc);
        return;
    }

    async_present_thread_started = 1;
    printf("I_InitGraphics: Async present enabled (queue=%d)\n", ASYNC_PRESENT_QUEUE_DEPTH);
}

static void I_ShutdownAsyncPresent(void)
{
    if (!async_present_thread_started)
        return;

    pthread_mutex_lock(&async_present_mutex);
    async_present_shutdown = 1;
    pthread_cond_broadcast(&async_present_cv);
    pthread_mutex_unlock(&async_present_mutex);

    pthread_join(async_present_thread, NULL);
    async_present_thread_started = 0;
    async_present_shutdown = 0;
    async_present_q_head = 0;
    async_present_q_tail = 0;
    async_present_q_count = 0;
}

// REMOVED: Transparent compositing approach was fundamentally flawed
// Now using unified buffer approach - FPGA writes to I_VideoBuffer directly

void I_InitGraphics(void)
{
    int i, gfxmodeparm;
    char *mode;
    int native_320 = 0;

#ifdef UDP_HEADLESS_BENCH
    extern int DG_UseNative320(void);
    native_320 = DG_UseNative320();
#endif

    memset(&s_Fb, 0, sizeof(struct FB_ScreenInfo));
    s_Fb.xres = native_320 ? SCREENWIDTH : DOOMGENERIC_RESX;
    s_Fb.yres = native_320 ? SCREENHEIGHT : DOOMGENERIC_RESY;
    s_Fb.xres_virtual = s_Fb.xres;
    s_Fb.yres_virtual = s_Fb.yres;

#ifdef CMAP256

    s_Fb.bits_per_pixel = 8;

#else // CMAP256

    gfxmodeparm = M_CheckParmWithArgs("-gfxmode", 1);

    if (gfxmodeparm)
    {
        mode = myargv[gfxmodeparm + 1];
    }
    else
    {
        // default to rgba8888 like the old behavior, for compatibility
        // maybe could warn here?
        mode = "rgba8888";
    }

    if (strcmp(mode, "rgba8888") == 0)
    {
        // default mode
        s_Fb.bits_per_pixel = 32;

        s_Fb.blue.length = 8;
        s_Fb.green.length = 8;
        s_Fb.red.length = 8;
        s_Fb.transp.length = 8;

        s_Fb.blue.offset = 0;
        s_Fb.green.offset = 8;
        s_Fb.red.offset = 16;
        s_Fb.transp.offset = 24;
    }

    else if (strcmp(mode, "rgb565") == 0)
    {
        s_Fb.bits_per_pixel = 16;

        s_Fb.blue.length = 5;
        s_Fb.green.length = 6;
        s_Fb.red.length = 5;
        s_Fb.transp.length = 0;

        s_Fb.blue.offset = 11;
        s_Fb.green.offset = 5;
        s_Fb.red.offset = 0;
        s_Fb.transp.offset = 16;
    }
    else
        I_Error("Unknown gfxmode value: %s\n", mode);

#endif // CMAP256

    printf("I_InitGraphics: framebuffer: x_res: %d, y_res: %d, x_virtual: %d, y_virtual: %d, bpp: %d\n",
           s_Fb.xres, s_Fb.yres, s_Fb.xres_virtual, s_Fb.yres_virtual, s_Fb.bits_per_pixel);

    printf("I_InitGraphics: framebuffer: RGBA: %d%d%d%d, red_off: %d, green_off: %d, blue_off: %d, transp_off: %d\n",
           s_Fb.red.length, s_Fb.green.length, s_Fb.blue.length, s_Fb.transp.length, s_Fb.red.offset, s_Fb.green.offset, s_Fb.blue.offset, s_Fb.transp.offset);

    printf("I_InitGraphics: DOOM screen size: w x h: %d x %d\n", SCREENWIDTH, SCREENHEIGHT);

    i = M_CheckParmWithArgs("-scaling", 1);
    if (native_320)
    {
        fb_scaling = 1;
        printf("I_InitGraphics: Native320 mode active, forcing scaling factor to 1\n");
    }
    else if (i > 0)
    {
        i = atoi(myargv[i + 1]);
        fb_scaling = i;
        printf("I_InitGraphics: Scaling factor: %d\n", fb_scaling);
    }
    else
    {
        fb_scaling = s_Fb.xres / SCREENWIDTH;
        if (s_Fb.yres / SCREENHEIGHT < fb_scaling)
            fb_scaling = s_Fb.yres / SCREENHEIGHT;
        printf("I_InitGraphics: Auto-scaling factor: %d\n", fb_scaling);
    }

    if (M_CheckParm("-async-present") > 0)
    {
        async_present_enabled = 1;
    }
    if (M_CheckParm("-sync-present") > 0)
    {
        async_present_enabled = 0;
    }

    /* Allocate screen to draw to (skip if already set by hardware init) */
    if (I_VideoBuffer == NULL)
    {
        I_VideoBuffer = (byte *)Z_Malloc(SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);
    }
    else
    {
        printf("I_InitGraphics: Using pre-allocated I_VideoBuffer at %p\n", I_VideoBuffer);
    }

    screenvisible = true;

    extern void I_InitInput(void);
    I_InitInput();

    I_InitAsyncPresent();
}

void I_ShutdownGraphics(void)
{
    I_ShutdownAsyncPresent();

    // Only free if allocated by Z_Malloc (not shared DDR)
    extern uint8_t *I_VideoBuffer_shared;
    if (I_VideoBuffer != I_VideoBuffer_shared)
    {
        Z_Free(I_VideoBuffer);
    }
}

// Forward declarations for FPGA acceleration
extern void Reset_Texture_Atlas(void);
extern void HW_StartFrame(void);
extern void HW_FinishFrame(void);
extern int HW_IsPLUpscaleEnabled(void);
extern uint64_t HW_UpscaleFrame(void);
extern void HW_SetRasterSharedBRAM(int enable);
extern void Upload_RGBPalette(const uint8_t *palette_rgb, int size);
extern boolean menuactive;

void I_StartFrame(void)
{
    // IMPORTANT: Do NOT reset texture atlas per frame!
    // The atlas persists across frames. DOOM reuses the same source pointers
    // for textures (WAD lumps, composite cache), so the SW cache ensures
    // each texture always lives at the same atlas offset. This keeps the
    // FPGA's on-chip texture cache coherent (same offset = same data).
    // Resetting per frame would assign different offsets to different textures
    // across frames, causing FPGA cache hits to return stale/wrong data.
    //
    // Atlas is only reset at level transitions (HW_ClearFramebuffer).
    // Overflow wraps with FPGA cache invalidation (Upload_Texture_Data).

    // Start FPGA batch processing for this frame
    HW_StartFrame();

    // Stage 5 split path:
    // - Gameplay PL present path: raster writes indexed frame to shared BRAM.
    // - Menu/software path: revert raster output to DDR-backed I_VideoBuffer.
    if (HW_IsPLUpscaleEnabled() && !menuactive)
    {
        HW_SetRasterSharedBRAM(1);
    }
    else
    {
        HW_SetRasterSharedBRAM(0);
    }

    // IMPORTANT: Do NOT clear I_VideoBuffer every frame!
    // Original DOOM never clears the framebuffer because:
    // 1. The status bar (HUD) is drawn once and persists between frames
    // 2. The 3D view is naturally overwritten by walls/floors/sprites each frame
    // 3. R_DrawViewBorder handles the border area when view size < full screen
    //
    // Clearing every frame breaks the HUD because ST_Drawer only redraws
    // portions that change, not the entire status bar.
}

void I_StartTic(void)
{
    I_GetEvent();
}

void I_UpdateNoBlit(void)
{
}

//
// I_FinishUpdate
//

// DEBUG: Set to 1 to skip software copy (shows only what FPGA draws)
// Set to 0 to enable transparent overlay (sprites, floors, HUD on top of FPGA walls)
static int debug_skip_sw_copy = 0;

void I_FinishUpdate(void)
{
    uint64_t scale_start;
    uint64_t scale_ns;

#ifdef UDP_HEADLESS_BENCH
    // Headless benchmark mode: when no viewer is connected on the UDP backend,
    // skip CPU scaling/present so FPS reflects game + HW rendering only.
    extern int DG_ShouldPresent(void);
    if (!DG_ShouldPresent())
    {
        return;
    }
#endif

    // DEBUG: If set, skip the software copy entirely to see FPGA output alone
    if (debug_skip_sw_copy)
    {
        DG_DrawFrame();
        return;
    }

    // Stage 4.1 path: PL performs 320x200 -> 1600x1000 upscale in hardware.
    // If menu is open, fall back to PS path for correctness/simplicity.
    if (HW_IsPLUpscaleEnabled() && !menuactive)
    {
        scale_ns = HW_UpscaleFrame();
        __sync_fetch_and_add(&perf_scale_ns, scale_ns);
        DG_DrawFrame();
        return;
    }

    if (async_present_enabled && async_present_thread_started)
    {
        int slot;
        pthread_mutex_lock(&async_present_mutex);
        while (async_present_q_count >= ASYNC_PRESENT_QUEUE_DEPTH)
        {
            pthread_cond_wait(&async_present_cv, &async_present_mutex);
        }
        slot = async_present_q_tail;
        async_present_q_tail = (async_present_q_tail + 1) % ASYNC_PRESENT_QUEUE_DEPTH;
        async_present_q_count++;
        memcpy(async_present_queue[slot], I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
        pthread_cond_signal(&async_present_cv);
        pthread_mutex_unlock(&async_present_mutex);
        return;
    }

    scale_start = video_now_ns();
    I_BlitScaledFrame((unsigned char *)I_VideoBuffer);
    scale_ns = video_now_ns() - scale_start;
    __sync_fetch_and_add(&perf_scale_ns, scale_ns);
    DG_DrawFrame();
}

void I_GetAndResetScalePerfNs(uint64_t *out_ns)
{
    if (!out_ns)
        return;

    *out_ns = __sync_lock_test_and_set(&perf_scale_ns, 0);
}

//
// I_ReadScreen
//
void I_ReadScreen(byte *scr)
{
    memcpy(scr, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
}

//
// I_SetPalette
//
#define GFX_RGB565(r, g, b) ((((r & 0xF8) >> 3) << 11) | (((g & 0xFC) >> 2) << 5) | ((b & 0xF8) >> 3))
#define GFX_RGB565_R(color) ((0xF800 & color) >> 11)
#define GFX_RGB565_G(color) ((0x07E0 & color) >> 5)
#define GFX_RGB565_B(color) (0x001F & color)

void I_SetPalette(byte *palette)
{
    int i;
    uint8_t palette_rgb[256 * 3];
    // col_t* c;

    // for (i = 0; i < 256; i++)
    //{
    //	c = (col_t*)palette;

    //	rgb565_palette[i] = GFX_RGB565(gammatable[usegamma][c->r],
    //								   gammatable[usegamma][c->g],
    //								   gammatable[usegamma][c->b]);

    //	palette += 3;
    //}

    /* performance boost:
     * map to the right pixel format over here! */

    for (i = 0; i < 256; ++i)
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;

        colors[i].a = 0;
        colors[i].r = gammatable[usegamma][*palette++];
        colors[i].g = gammatable[usegamma][*palette++];
        colors[i].b = gammatable[usegamma][*palette++];

        r = colors[i].r;
        g = colors[i].g;
        b = colors[i].b;

        // Prepack palette for fast per-pixel conversion.
        rgb565_palette[i] = ((uint16_t)(r & 0xF8) << 8) |
                            ((uint16_t)(g & 0xFC) << 3) |
                            ((uint16_t)b >> 3);
#ifdef SYS_BIG_ENDIAN
        rgb565_palette[i] = swapeLE16(rgb565_palette[i]);
#endif

        rgba_palette[i] = ((uint32_t)r << s_Fb.red.offset) |
                          ((uint32_t)g << s_Fb.green.offset) |
                          ((uint32_t)b << s_Fb.blue.offset);
#ifdef SYS_BIG_ENDIAN
        rgba_palette[i] = swapLE32(rgba_palette[i]);
#endif

        palette_rgb[i * 3 + 0] = r;
        palette_rgb[i * 3 + 1] = g;
        palette_rgb[i * 3 + 2] = b;
    }

    // Keep PL color expansion palette in sync with current gamma-corrected palette.
    Upload_RGBPalette(palette_rgb, sizeof(palette_rgb));

#ifdef CMAP256

    palette_changed = true;

#endif // CMAP256
}

// Given an RGB value, find the closest matching palette index.

int I_GetPaletteIndex(int r, int g, int b)
{
    int best, best_diff, diff;
    int i;
    col_t color;

    printf("I_GetPaletteIndex\n");

    best = 0;
    best_diff = INT_MAX;

    for (i = 0; i < 256; ++i)
    {
        color.r = GFX_RGB565_R(rgb565_palette[i]);
        color.g = GFX_RGB565_G(rgb565_palette[i]);
        color.b = GFX_RGB565_B(rgb565_palette[i]);

        diff = (r - color.r) * (r - color.r) + (g - color.g) * (g - color.g) + (b - color.b) * (b - color.b);

        if (diff < best_diff)
        {
            best = i;
            best_diff = diff;
        }

        if (diff == 0)
        {
            break;
        }
    }

    return best;
}

void I_BeginRead(void)
{
}

void I_EndRead(void)
{
}

void I_SetWindowTitle(char *title)
{
    DG_SetWindowTitle(title);
}

void I_GraphicsCheckCommandLine(void)
{
}

void I_SetGrabMouseCallback(grabmouse_callback_t func)
{
}

void I_EnableLoadingDisk(void)
{
}

void I_BindVideoVariables(void)
{
}

void I_DisplayFPSDots(boolean dots_on)
{
}

void I_CheckIsScreensaver(void)
{
}
