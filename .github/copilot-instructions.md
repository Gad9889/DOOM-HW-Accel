# DOOM FPGA Acceleration Project - Copilot Memory

## ⚠️ IMPORTANT RULES FOR COPILOT ⚠️

1. **DO NOT make major architecture changes without user confirmation**
2. **DO NOT remove the BRAM framebuffer** - it provides critical performance gains
3. **DO NOT upload unverified code to git** - wait until it works
4. **Ask before changing core design decisions** like buffer locations, BRAM usage, etc.

## Project Overview

FPGA-accelerated DOOM running on PYNQ board (Avnet Ultra96-V2 / aup-zu3).

### Architecture (Batch Rendering v2)

- **PS (Processing System)**: ARM CPU running Linux, handles:
  - Game logic, BSP traversal
  - Sprites, floors/ceilings, HUD rendered via software to I_VideoBuffer
  - Queues wall column commands for FPGA batch processing
  - Scaling 320x200 → 1920x1080 in I_FinishUpdate()
  - Runs doomgeneric C engine

- **PL (Programmable Logic)**: FPGA with custom HLS IP core:
  - **Colormap in BRAM** (8KB) - loaded once at level start, never reloaded
  - **Framebuffer in BRAM** (64KB) - 320x200 buffer, persists across frames
  - **Batch processing** - receives all column commands at once, single handshake
  - Reads textures from DDR via AXI Master (burst reads)
  - DMAs framebuffer BRAM → DDR at frame end

- **Memory Layout** (Physical DDR):

  ```
  0x70000000: DG_ScreenBuffer (1920x1080x4 = 8MB, 32-bit RGBA output)
  0x70800000: I_VideoBuffer output (320x200 = 64KB, FPGA DMAs here)
  0x70810000: Command Buffer (64KB, ~2000 draw commands per frame)
  0x70820000: Texture Atlas (16MB, level textures pre-loaded)
  0x71820000: Colormap (8KB = 32 light levels × 256 palette entries)
  ```

- **Display**: Python TCP viewer on PC connects to port 5000

### Performance Design

Previous approach was 2x SLOWER than software (20 FPS vs 50 FPS) due to:

- Per-column handshaking (8 register writes + poll per column)
- Texture upload per column (128 bytes memcpy × 320 columns = 40KB/frame)
- Colormap reload per column (256 bytes × 320 = 80KB/frame)

New batch approach:

- **Single handshake per frame** instead of 320 handshakes
- **Colormap in BRAM permanently** - load once at level start
- **Framebuffer in BRAM** - 64KB fits easily in 9.8Mb FPGA BRAM
- **Batch command buffer** - CPU queues all commands, FPGA processes in one shot
- **Texture burst reads** - FPGA reads 128-byte texture columns via AXI

## Key Files

### doom_accel.h

- Memory map defines (PHY_FB_ADDR, PHY_VIDEO_BUF, PHY_CMD_BUF, PHY_TEX_ADDR, PHY_CMAP_ADDR)
- `DrawCommand` structure (32 bytes: x, y_start, y_end, cmap_index, frac, step, tex_offset)
- Mode defines: MODE_LOAD_COLORMAP, MODE_CLEAR_FB, MODE_DRAW_BATCH, MODE_DMA_OUT
- Register offsets (update from Vitis HLS synthesis output!)

### doom_accel.c

- `Init_Doom_Accel()`: Maps registers and DDR, sets up pointers
- `Upload_Colormap()`: Copies colormap to DDR, triggers FPGA BRAM load
- `Upload_Texture_Data()`: Copies texture to atlas, returns offset
- `HW_StartFrame()`: Resets command buffer (called from I_StartFrame)
- `HW_QueueColumn()`: Queues draw command (called from R_DrawColumn for walls)
- `HW_FinishFrame()`: Fires FPGA to process batch + DMA out (called from I_FinishUpdate)
- `HW_ClearFramebuffer()`: Clears FPGA BRAM (call at level transitions, not every frame!)
- Debug flag: `debug_sw_fallback` (1=software rendering, 0=FPGA)

### i_video.c

- `I_InitGraphics()`: Skips I_VideoBuffer allocation if already set
- `I_StartFrame()`: Calls `Reset_Texture_Atlas()` and `HW_StartFrame()`
- `I_FinishUpdate()`: Calls `HW_FinishFrame()` then scales 320x200 → 1920x1080
- No framebuffer clear every frame (HUD persists!)

### r_draw.c

- `R_DrawColumn()`: When `drawing_wall=1` and FPGA available, calls `HW_QueueColumn()`
- Sprites and software fallback use normal software path
- `drawing_wall` flag set in r_segs.c around `R_RenderSegLoop()`

### hls/doom_accel_v2.cpp

- Vitis HLS IP with unified CTRL AXI-Lite bundle
- Static BRAM arrays for colormap (8KB) and framebuffer (64KB)
- Modes:
  - MODE_LOAD_COLORMAP: DMA colormap DDR → BRAM (once per level)
  - MODE_CLEAR_FB: Clear framebuffer BRAM
  - MODE_DRAW_BATCH: Process N commands, prefetch texture per column
  - MODE_DMA_OUT: DMA framebuffer BRAM → DDR

## DOOM Rendering Order

1. Walls - BSP traversal, front-to-back → **FPGA via HW_QueueColumn()**
2. Floors/Ceilings - Visplanes (R_DrawSpan) → Software to I_VideoBuffer
3. Sprites - Sorted by distance → Software to I_VideoBuffer
4. HUD - Status bar, messages (V_DrawPatch) → Software to I_VideoBuffer

All rendered to same 320x200 buffer → correct z-ordering automatic.

## Current Status

### Working ✅

- Batch rendering architecture designed
- Command buffer structure defined
- HLS IP v2 with BRAM colormap and framebuffer
- Driver with HW_StartFrame/HW_QueueColumn/HW_FinishFrame API

### TODO 🔧

1. Synthesize doom_accel_v2.cpp in Vitis HLS
2. Update register offsets in doom_accel.h from synthesis report
3. Deploy bitstream to PYNQ
4. Set debug_sw_fallback=0 to test
5. Pre-load level textures at level start (currently still per-column upload)

## Build & Run

```bash
cd doomgeneric
./build.bat          # Cross-compile for ARM
# Copy to PYNQ, run with DOOM1.WAD
```

## After HLS Synthesis

Check the Vitis HLS synthesis report for register offsets. Update doom_accel.h:

```c
#define REG_FB_OUT_LO      0x??  // From synthesis report
#define REG_FB_OUT_HI      0x??
#define REG_TEX_ATLAS_LO   0x??
...
```
