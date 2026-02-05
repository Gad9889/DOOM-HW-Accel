# DOOM FPGA Acceleration Project - Copilot Memory

## ⚠️ IMPORTANT RULES FOR COPILOT ⚠️

1. **DO NOT make major architecture changes without user confirmation**
2. **DO NOT remove the BRAM framebuffer** - it provides critical performance gains
3. **DO NOT upload unverified code to git** - wait until it works
4. **Ask before changing core design decisions** like buffer locations, BRAM usage, etc.

## Project Overview

FPGA-accelerated DOOM running on PYNQ board (Avnet Ultra96-V2 / aup-zu3).

### Architecture (Batch Rendering v2 - Stage 1 Complete ✅)

- **PS (Processing System)**: ARM CPU running Linux, handles:
  - Game logic, BSP traversal
  - HUD/status bar rendered via software (rows 168-199, preserved by partial DMA)
  - Queues ALL 3D rendering commands for FPGA batch processing
  - Scaling 320x200 → 1920x1080 in I_FinishUpdate()
  - Runs doomgeneric C engine

- **PL (Programmable Logic)**: FPGA with custom HLS IP core:
  - **Colormap in BRAM** (8KB) - loaded once at level start, never reloaded
  - **Framebuffer in BRAM** (64KB) - 320x200 buffer, persists across frames
  - **Batch processing** - receives all column/span commands at once, single handshake
  - Reads textures from DDR via AXI Master (burst reads)
  - DMAs only VIEW area (rows 0-167) → DDR, preserving HUD

- **FPGA Renders ALL 3D Content**:
  - ✅ Walls (R_DrawColumn with drawing_wall=1)
  - ✅ Floors/Ceilings (R_DrawSpan)
  - ✅ Sprites (R_DrawColumn with drawing_sprite=1)
  - ✅ Masked textures (mid-textures, also via drawing_sprite)
  - ✅ Player weapon sprites (psprites)

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

- `R_DrawColumn()`: Dispatches to FPGA when `(drawing_wall || drawing_sprite)` is set
- `R_DrawSpan()`: Queues floor/ceiling spans to FPGA via `HW_QueueSpan()`
- `drawing_wall` flag set in r_segs.c around `R_RenderSegLoop()`
- `drawing_sprite` flag set in r_things.c around `R_DrawMasked()`

### r_things.c

- `R_DrawMasked()`: Sets `drawing_sprite=1`, renders all sprites/masked textures, then `HW_FlushBatch()`
- Includes world sprites, masked mid-textures, and player weapon sprites

### r_main.c

- `R_RenderPlayerView()`: Calls `HW_FlushBatch()` after `R_DrawPlanes()` (walls+floors batch)
- Then calls `R_DrawMasked()` which does its own `HW_FlushBatch()` (sprites batch)

### hls/doom_accel_v2.cpp

- Vitis HLS IP with unified CTRL AXI-Lite bundle
- Static BRAM arrays for colormap (8KB) and framebuffer (64KB)
- Modes:
  - MODE_LOAD_COLORMAP: DMA colormap DDR → BRAM (once per level)
  - MODE_CLEAR_FB: Clear framebuffer BRAM
  - MODE_DRAW_BATCH: Process N commands, prefetch texture per column
  - MODE_DMA_OUT: DMA framebuffer BRAM → DDR

## DOOM Rendering Order (Stage 1 - All 3D on FPGA)

1. **Walls** - BSP traversal, front-to-back → **FPGA via HW_QueueColumn()** (drawing_wall=1)
2. **Floors/Ceilings** - Visplanes → **FPGA via HW_QueueSpan()**
3. **HW_FlushBatch()** - Execute walls+floors batch, DMA to BRAM
4. **Sprites** - Sorted by distance → **FPGA via HW_QueueColumn()** (drawing_sprite=1)
5. **Masked textures** - Mid-textures on 2-sided linedefs → **FPGA** (drawing_sprite=1)
6. **Player weapon** - psprites → **FPGA** (drawing_sprite=1)
7. **HW_FlushBatch()** - Execute sprites batch, DMA to BRAM
8. **HUD** - Status bar, messages → **Software** (rows 168-199, preserved)

All 3D content rendered to FPGA BRAM framebuffer → DMA only VIEW area to DDR.

## Current Status

### Stage 1 Complete ✅ - Full Hardware 3D Rendering

All 3D rendering now goes through FPGA:
- ✅ Walls via `drawing_wall` flag
- ✅ Floors/ceilings via `HW_QueueSpan()`
- ✅ Sprites via `drawing_sprite` flag
- ✅ Masked textures (mid-textures) via `drawing_sprite`
- ✅ Player weapon sprites via `drawing_sprite`
- ✅ HUD preserved (DMA only rows 0-167)
- ✅ Menu working (rendered after FPGA flush)

### Performance Issue 🔧

Currently running at **~18 FPS** (target: 35+ FPS). Bottlenecks:
1. **Per-command texture upload** - Upload_Texture_Data() called per column
2. **DDR texture reads** - FPGA reads from slow DDR, not BRAM
3. **No texture caching** - Same textures uploaded repeatedly
4. **Sequential command processing** - Could parallelize

### Stage 2 TODO - Performance Optimization

1. Pre-load level textures to texture atlas at level start
2. Cache texture offsets, avoid re-upload of same texture
3. Add texture BRAM cache in HLS (LRU for hot textures)
4. Pipeline command processing with texture prefetch
5. Burst read optimization for texture columns

1. Synthesize doom_accel_v2.cpp in Vitis HLS
2. Update register offsets in doom_accel.h from synthesis report
3. Deploy bitstream to PYNQ
4. Set debug_sw_fallback=0 to test
5. Pre-load level textures at level start (currently still per-column upload)

## Build & Run

```bash
cd doomgeneric/doomgeneric
./build.bat          # Cross-compile for ARM (use cmd.exe or git bash on Windows)
# Copy doom_stream to PYNQ, run with DOOM1.WAD
```

## After HLS Synthesis

Check the Vitis HLS synthesis report for register offsets. Update doom_accel.h:

```c
#define REG_FB_OUT_LO      0x??  // From synthesis report
#define REG_FB_OUT_HI      0x??
#define REG_TEX_ATLAS_LO   0x??
...
```
