# DOOM FPGA Acceleration Project - Copilot Memory

## Project Overview

FPGA-accelerated DOOM running on PYNQ board (Avnet Ultra96-V2 / aup-zu3).

### Architecture (Unified Buffer Approach)

- **PS (Processing System)**: ARM CPU running Linux, handles:
  - Game logic, BSP traversal
  - ALL rendering to shared 320x200 I_VideoBuffer
  - Scaling 320x200 → 1920x1080 in I_FinishUpdate()
  - Runs doomgeneric C engine
- **PL (Programmable Logic)**: FPGA with custom HLS IP core:
  - Writes wall columns to shared 320x200 buffer (same as CPU)
  - Can use BRAM (64KB fits in 9.8Mb) for faster access
  - Reads textures from DDR texture atlas
- **Memory**: Shared physical DDR region
  - I_VideoBuffer: `0x70000000` (320x200x1 = 64KB, 8-bit palette indexed)
  - Texture Atlas: `0x70010000` (16MB region)
  - Colormap: `0x70020000` (256 bytes per light level)
  - DG_ScreenBuffer: `0x71000000` (1920x1080x4 = 8MB, 32-bit RGBA output)
  - CPU maps via `/dev/mem`, FPGA uses AXI master

- **Display**: Python TCP viewer on PC connects to port 5000

### Why Unified Buffer?

Previous approach (FPGA writes 1920x1080, CPU writes 320x200, composite) was broken:

- Z-ordering issues (walls over sprites, HUD broken)
- Transparency hack failed because black (palette 0) is a valid color
- No clean way to composite FPGA and CPU rendered content

New approach:

- FPGA and CPU both write to same 320x200 8-bit buffer
- DOOM's natural BSP/rendering order ensures correct z-ordering
- CPU scales 320x200 → 1920x1080 at frame end (can move to FPGA later)
- 64KB buffer fits in FPGA BRAM for fast single-cycle writes

## Key Files

### doom_accel.c / doom_accel.h

- `Init_Doom_Accel()`: Maps registers and DDR, sets `I_VideoBuffer` to shared DDR region
- `HW_DrawColumn(x, y_start, y_end, step, frac, tex_offset, colormap_offset)`: Sends draw command to FPGA
- `Upload_Texture_Column(source, height)`: Copies texture data to atlas, returns offset
- `Upload_Colormap(colormaps_ptr, size)`: Uploads colormap table to DDR (called once at init)
- `Reset_Texture_Atlas()`: Called at frame start to reset atlas offset
- Debug flag: `debug_sw_fallback` (1=software rendering, 0=FPGA) - set to 1 until HLS IP synthesized

### i_video.c

- `I_InitGraphics()`: Skips I_VideoBuffer allocation if already set by `Init_Doom_Accel()`
- `I_StartFrame()`: Resets texture atlas (NO framebuffer clear - HUD persists!)
- `I_FinishUpdate()`: Uses `cmap_to_fb()` to scale 320x200 → 1920x1080
- No transparency/compositing needed - all content in same buffer

### r_draw.c

- `R_DrawColumn()`: Renders columns to I_VideoBuffer (320x200)
- When `drawing_wall=1` and FPGA available: dispatches to `HW_DrawColumn()`
- When `drawing_wall=0` (sprites) or no FPGA: uses software rendering
- `drawing_wall` flag set in r_segs.c around `R_RenderSegLoop()`

### r_data.c

- `R_InitColormaps()`: Loads colormap from WAD
- After loading, calls `Upload_Colormap()` to copy to DDR for FPGA access

### hls/doom_accel_320x200.cpp

- NEW HLS IP that writes to 320x200 buffer instead of 1920x1080
- Writes 8-bit palette indices (not scaled RGBA)
- Applies colormap for lighting (like dc_colormap[dc_source[...]])
- Optional BRAM version for maximum performance

### doomgeneric.c

- Modified to NOT allocate `DG_ScreenBuffer` if already set by hardware init

## Memory Layout (NEW)

```
Physical Address    Size        Content
-----------------------------------------------------------------
0x70000000         8MB         DG_ScreenBuffer (1920x1080x4, 32-bit RGBA output)
0x70800000         64KB        I_VideoBuffer (320x200, 8-bit palette indexed)
0x70810000         16MB        Texture Atlas (uploaded per-frame)
0x71810000         8KB         Colormaps (32 light levels x 256 bytes)
0xA0000000         4KB         FPGA Register Interface
```

## HLS IP (doom_accel_320x200) - NEW DESIGN

Writes to 320x200 buffer (no scaling - CPU handles that):

```cpp
void doom_accel_320x200(
    volatile uint8_t* video_buffer,    // 320x200 8-bit buffer
    volatile uint8_t* texture_atlas,   // Texture data
    volatile uint8_t* colormap,        // Lighting colormap
    uint64_t cmd1,                     // [step(32) | frac(32)]
    uint64_t cmd2,                     // [y_end(16) | y_start(16) | x(16) | 0]
    uint64_t cmd3                      // [colormap_offset(32) | tex_offset(32)]
) {
    // Unpack commands
    uint32_t frac = cmd1 & 0xFFFFFFFF;
    uint32_t step = (cmd1 >> 32);
    uint16_t x = (cmd2 >> 16) & 0xFFFF;
    uint16_t y_start = (cmd2 >> 32) & 0xFFFF;
    uint16_t y_end = (cmd2 >> 48) & 0xFFFF;
    uint32_t tex_offset = cmd3 & 0xFFFFFFFF;
    uint32_t cmap_offset = (cmd3 >> 32);

    // Draw column to 320x200 buffer
    for (uint16_t y = y_start; y <= y_end; y++) {
        uint8_t tex_pixel = texture_atlas[tex_offset + ((frac >> 16) & 127)];
        uint8_t lit_pixel = colormap[cmap_offset + tex_pixel];
        video_buffer[y * 320 + x] = lit_pixel;
        frac += step;
    }
}
```

## BRAM Version (Optional)

For maximum performance, use FPGA BRAM for the 320x200 buffer:

- 64KB easily fits in 9.8Mb BRAM
- Single-cycle writes vs DDR latency
- DMA to DDR at frame end
- Commands: 0=draw_column, 1=clear, 2=dma_out

## Doom Rendering Order

1. Walls - BSP traversal, front-to-back (FPGA accelerated)
2. Floors/Ceilings - Visplanes (R_DrawSpan)
3. Sprites - Sorted by distance (R_DrawColumn with drawing_wall=0)
4. HUD - Status bar, messages (V_DrawPatch)

All rendered to same 320x200 I_VideoBuffer → correct z-ordering automatic.

## Current Status

### Working ✅

- Correct software rendering (baseline working)
- HUD rendering fixed (removed erroneous framebuffer clear)
- I_VideoBuffer points to shared DDR (unified buffer)
- Texture upload system
- Colormap upload to DDR
- TCP viewer display
- Memory mapping
- FPGA wall dispatch code ready (disabled via debug_sw_fallback=1)

### TODO 🔧

1. Synthesize new doom_accel_320x200 HLS IP (hls/doom_accel_320x200.cpp)
2. Set debug_sw_fallback=0 after FPGA IP is deployed
3. Optional: Move scaling to FPGA for full acceleration
4. Optional: Use BRAM for I_VideoBuffer for max performance

## Build & Run

```bash
cd doomgeneric
./build.bat          # Cross-compile for ARM
# Copy to PYNQ, run with DOOM1.WAD
```
