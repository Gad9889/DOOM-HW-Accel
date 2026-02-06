# DOOM FPGA Acceleration Project - Copilot Memory

## ⚠️ IMPORTANT RULES FOR COPILOT ⚠️

1. **DO NOT make major architecture changes without user confirmation**
2. **DO NOT remove the BRAM framebuffer** - it provides critical performance gains
3. **DO NOT upload unverified code to git** - wait until it works
4. **Ask before changing core design decisions** like buffer locations, BRAM usage, etc.

## Project Overview

FPGA-accelerated DOOM running on PYNQ board (Avnet Ultra96-V2 / aup-zu3).

### Target Device

- **Part Number**: xczu3eg-sfvc784-2-e (Zynq UltraScale+ ZU3EG)
- **Board**: Avnet Ultra96-V2
- **BRAM**: 9.8 Mb (216 × 36Kb blocks)
- **URAM**: ⚠️ **NOT AVAILABLE** on ZU3EG - use BRAM only
- **DSP**: 360 slices
- **LUT**: 70,560

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
  0x70810000: Command Buffer (128KB = 4000 cmds × 32B)
  0x70830000: Texture Atlas (16MB, level textures pre-loaded)
  0x71830000: Colormap (8KB = 32 light levels × 256 palette entries)
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
- Mode defines: MODE_LOAD_COLORMAP, MODE_CLEAR_FB, MODE_DRAW_BATCH, MODE_DMA_OUT, MODE_DRAW_AND_DMA
- Register offsets (verified from Vitis HLS synthesis):

### doom_accel.c

- `Init_Doom_Accel()`: Maps registers and DDR, sets up pointers
- `Upload_Colormap()`: Copies colormap to DDR, triggers FPGA BRAM load
- `Upload_Texture_Data()`: Copies texture to atlas, returns offset (with SW cache)
- `HW_StartFrame()`: Resets command count (called from I_StartFrame)
- `HW_QueueColumn()`: Queues draw command (called from R_DrawColumn for walls/sprites)
- `HW_QueueSpan()`: Queues span command (called from R_DrawSpan for floors/ceilings)
- `HW_FlushBatch()`: Fires FPGA MODE_DRAW_AND_DMA (single handshake for draw + DMA)
- `HW_FinishFrame()`: No-op (kept for compatibility)
- `HW_ClearFramebuffer()`: Clears FPGA BRAM + resets atlas (level transitions only)
- Overflow handling: HW_QueueColumn/Span call HW_FlushBatch() if cmd_count >= MAX_COMMANDS
- Debug flag: `debug_sw_fallback` (1=software rendering, 0=FPGA)

### i_video.c

- `I_InitGraphics()`: Skips I_VideoBuffer allocation if already set
- `I_StartFrame()`: Calls `HW_StartFrame()` only (NO atlas reset - atlas persists across frames)
- `I_FinishUpdate()`: Scales 320x200 → 1920x1080
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

- `R_RenderPlayerView()`: No intermediate flush; accumulates all commands
- `R_DrawMasked()` calls `HW_FlushBatch()` at the end (single batch for entire frame)

### hls/doom_accel_v3.cpp (Stage 2 - Optimized)

- **Texture BRAM Cache** (32KB): 256 slots × 128 bytes, direct-mapped
- **Flat BRAM Cache** (4KB): Caches 64×64 flat texture for span rendering (BRAM reads vs DDR)
- **Explicit Burst Reads**: Proper AXI burst loops (no memcpy)
- **MODE_DRAW_AND_DMA** (mode 6): Combined draw + DMA in single FPGA invocation
- **3-Row Line Buffer**: Ready for future bicubic upscaling (rows N-1, N, N+1)
- **16-byte Alignment**: All texture offsets aligned for optimal 128-bit AXI

Static BRAM usage:

- Framebuffer: 64KB (BRAM - xczu3eg has no URAM)
- Colormap: 8KB (BRAM)
- Texture Cache: 32KB (BRAM)
- Flat Cache: 4KB (BRAM)
- Line Buffers: 960 bytes (BRAM)
- Total: ~110KB << 9.8Mb available

## DOOM Rendering Order (Stage 2 - Single Batch)

1. **Walls** - BSP traversal, front-to-back → **FPGA via HW_QueueColumn()** (drawing_wall=1)
2. **Floors/Ceilings** - Visplanes → **FPGA via HW_QueueSpan()**
3. **Sprites** - Sorted by distance → **FPGA via HW_QueueColumn()** (drawing_sprite=1)
4. **Masked textures** - Mid-textures on 2-sided linedefs → **FPGA** (drawing_sprite=1)
5. **Player weapon** - psprites → **FPGA** (drawing_sprite=1)
6. **HW_FlushBatch()** - Execute ALL commands in single FPGA invocation (MODE_DRAW_AND_DMA)
7. **HUD** - Status bar, messages → **Software** (rows 168-199, preserved by partial DMA)

All 3D content rendered to FPGA BRAM framebuffer → DMA only VIEW area (rows 0-167) to DDR.

## Current Status

### Stage 2 In Progress 🔧 - Performance Optimization

All 3D rendering goes through FPGA (Stage 1 complete). Now optimizing performance.

**Current FPS: ~21** (target: 35+)

**Bugs Fixed:**

1. ✅ **Command buffer overflow** - CMD buffer was 64KB but MAX_COMMANDS×32 = 128KB.
   Commands overflowed into texture atlas, corrupting textures.
   Fix: Moved PHY_TEX_ADDR from 0x70820000 → 0x70830000 (128KB gap).
2. ✅ **Overflow handler was no-op** - HW_QueueColumn/Span called HW_FinishFrame() (empty).
   Fix: Now calls HW_FlushBatch() which processes + DMAs the batch.
3. ✅ **URAM not available** - xczu3eg has no URAM. Changed to BRAM.
4. ✅ **HLS pointer selection** - Fixed by using local_tex buffer instead of pointer indirection.
5. ✅ **FPGA cache coherency** - Per-frame atlas reset caused different textures at same offsets.
   FPGA texture cache returned stale data from previous frames.
   Fix: Atlas persists across frames. Same texture always at same offset. Cache stays coherent.

**Implemented Optimizations:**

1. ✅ **Software Texture Cache** (doom_accel.c) - Hash-based 4096-entry cache
2. ✅ **HLS Texture BRAM Cache** (doom_accel_v3.cpp) - 32KB on-chip, 256 slots
3. ✅ **Flat BRAM Cache** (doom_accel_v3.cpp) - 4KB cache for floor/ceiling flats
4. ✅ **MODE_DRAW_AND_DMA** - Single FPGA handshake per frame (was 4)
5. ✅ **Single batch** - All 3D in one batch (no intermediate flush)
6. ✅ **Bicubic Upscaling Prep** - 3-row line buffer ready

**Verified Register Offsets (from Vitis HLS synthesis):**

```
Base: 0xA0000000
CTRL:             0x00
framebuffer_out:  0x10 (lo), 0x14 (hi)
texture_atlas:    0x1C (lo), 0x20 (hi)
colormap_ddr:     0x28 (lo), 0x2C (hi)
command_buffer:   0x34 (lo), 0x38 (hi)
mode:             0x40
num_commands:     0x48
```

**TODO:**

1. Resynthesize HLS with memory layout fix (PHY_TEX_ADDR change only affects C side)
2. Deploy and test rendering correctness
3. Measure FPS improvement
4. Implement bicubic upscale in HLS (Stage 3)

## Build & Run

```bash
cd doomgeneric/doomgeneric
./build.bat          # Cross-compile for ARM (use cmd.exe or git bash on Windows)
# Copy doom_stream to PYNQ, run with DOOM1.WAD
```

## After HLS Synthesis

Register offsets have been verified and match doom_accel.h.
No further offset updates needed unless the function signature changes.

## Known Limitations

- **Sky rendering**: Falls through to software path (drawing_wall=0 during R_DrawPlanes sky)
- **R_DrawFuzzColumn** (spectres): Always software, reads neighboring pixels from I_VideoBuffer
- **Texture atlas**: Persistent across frames (~2-4MB per level, 16MB capacity). Reset at level transitions.

## 128-bit AXI Stability Notes (Stage 2.1)

Critical implementation details for `hls/doom_accel_v3.cpp` after migrating AXI bandwidth to 128-bit:

- **Do not reinterpret packed commands as 128-bit words.**
  `DrawCommand` is `__attribute__((packed))` and may be byte-aligned.
  Casting `DrawCommand*` to `ap_uint<128>*` for command burst copies can mis-read fields in HLS.
  For 128-bit CMD AXI, keep `command_buffer` typed as `ap_uint<128>*` and decode fields explicitly
  from two 128-bit words per command (do not cast packed structs across widths).

- **CMD AXI interface is explicitly 128-bit words (not struct-typed).**
  To keep command memory on 128-bit AXI and preserve burst behavior:
  - Top argument uses `const ap_uint<128>* command_buffer`
  - Command decode reconstructs one `DrawCommand` from 2 x 128-bit words
  - CMD m_axi depth is `8000` words for `MAX_COMMANDS=4000`
  This avoids HLS inferring `m_axi_CMD` as 256-bit with unsupported struct burst access.

- **Keep command ABI checks compile-time, but C++98-compatible.**
  Toolchain-safe checks are used:
  - `sizeof(DrawCommand) == 32`
  - `offsetof(DrawCommand, tex_offset) == 20`

- **Use GCC alignment attributes, not `alignas`.**
  This HLS flow rejects `alignas` syntax (`HLS 207-4`).
  For buffers reinterpreted as 128-bit words, use:
  - `__attribute__((aligned(16)))`
  Applied to BRAM arrays like `local_framebuffer` and `local_colormap`.

- **Byte extraction from `ap_uint<128>` uses `.range(high, low)`.**
  This avoids ambiguity in shift/cast behavior and keeps lane extraction explicit.

- **Span safety in HLS path is explicit.**
  Span commands are clamped/checked in hardware (`y1`, `x1`, `x2`) before framebuffer writes.
