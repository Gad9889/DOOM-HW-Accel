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
  - Scaling 320x200 → 1600x1000 in fullres mode (or bypass in native320 mode)
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
  0x70000000: DG_ScreenBuffer (1600x1000x4 = 6.4MB, 8MB reserved, 32-bit RGBA output)
  0x70800000: I_VideoBuffer output (320x200 = 64KB, FPGA DMAs here)
  0x70810000: Command Buffer (128KB = 4000 cmds × 32B)
  0x70830000: Texture Atlas (16MB, level textures pre-loaded)
  0x71830000: Colormap + RGB palette (8KB colormap + 768B RGB table)
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
- `I_FinishUpdate()`: Scales 320x200 -> 1600x1000 in fullres mode
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

## Stage 2.2 Runtime Perf Telemetry + Texture Cache Hardening

Host-side bottleneck debugging and mitigations added in `doomgeneric/doom_accel.c` and `doomgeneric/doomgeneric_udp.c`:

- **Texture cache hash/probe hardening (CPU side):**
  - Cache table increased from `4096` to `16384` entries.
  - Pointer hash now uses mixed bits (not simple `ptr >> 4`) to avoid hot-bucket collisions from aligned column pointers.
  - Probe path uses bounded probe first, then full-table fallback before declaring miss.
  - If no free slot exists, home-bucket replacement is used (tracked as failed insert event).
  - Added fast-path for repeated consecutive texture pointers.

- **Why this matters:**
  The previous hash/probe strategy could miss existing entries under collisions and repeatedly upload texture columns to uncached DDR, inflating frame time.

- **New runtime counters (sampled once per second):**
  - Command stats: queued columns/spans, flush count, mid-frame flushes, max commands in a batch.
  - Texture stats: lookups/hits/misses, failed inserts, upload KB, atlas wraps, active entries.
  - FPGA wait stats: average `ap_done` wait time per frame.

- **Runtime output format:**
  `doomgeneric_udp` now prints a second `HW:` line next to FPS so bottlenecks can be identified on real hardware quickly.

## Stage 2.3 PS Memory Mapping Note (Critical for FPS)

- On this platform, `/dev/mem` mappings used for FPGA shared DDR are typically non-cacheable from the CPU side.
- CPU scaling (320x200 palette -> 1600x1000 RGBA) must **not** write into the MMIO shared DDR window each frame.
- `DG_ScreenBuffer` is kept in normal cached userspace memory (malloc path in `doomgeneric.c`) for high CPU write bandwidth.
- The MMIO shared region remains used for:
  - `I_VideoBuffer_shared` (0x70800000)
  - command buffer
  - texture atlas
  - colormap
- This avoids a major PS bottleneck when hardware acceleration is enabled.

## Stage 2.4 Headless Hardware Benchmark Mode

- For true hardware-only profiling (no viewer connected), `build.bat` enables `-DUDP_HEADLESS_BENCH`.
- Runtime startup switch: pass `-no-client` to skip blocking TCP accept (no connect/disconnect workaround needed).
- Runtime switch: pass `-bench-headless` (or `-bench-nopresent`) to enable no-client present skip.
- Without this switch, scaling/present logic stays active even if client disconnects (for apples-to-apples comparisons).
- In headless mode, `i_video.c:I_FinishUpdate()` returns early when UDP backend reports no client connection.
- Effect:
  - Skips CPU scaling/conversion/present path when disconnected.
  - FPS reflects game logic + command generation + FPGA execution more directly.
- Runtime logs now include `Tics: N/s` to distinguish game tick pacing from render FPS.
- This is benchmark-only behavior for the UDP backend path used in `doomgeneric/build.bat`.

## Stage 2.5 CPU Scaling Optimization (5x critical)

- `i_video.c:I_FinishUpdate()` now converts each source row once, then duplicates vertical rows with `memcpy`.
- Previous behavior re-ran palette conversion for every vertically duplicated row (`fb_scaling` times), which heavily penalized higher scaling factors.
- New behavior keeps visual output the same but reduces redundant conversion work, especially at `-scaling 5`.
- `i_video.c:cmap_to_fb()` now uses prepacked palette caches (`rgb565_palette` / `rgba_palette`) and a fast path for 32-bit `fb_scaling=5` to reduce per-pixel arithmetic/loop overhead.

## Stage 3 Baseline (PS bottleneck reduction)

- Stage 3 default runtime stream mode is now **native 320x200** in `doomgeneric_udp`:
  - New runtime switches:
    - `-native320` (explicit native mode)
    - `-fullres` (fallback to legacy full-resolution stream path)
- `i_video.c` checks backend stream mode and, in native mode, forces `fb_scaling=1` and output resolution to 320x200.
- This removes fullres expansion from PS in Stage 3 and keeps frame transport focused on native DOOM resolution.
- `doom_udp_viewer.py` updated to receive 320x200 stream and scale on PC client side (`DISPLAY_SCALE`).

## Stage 3.3 (Validated) Async Present + Correct Scaling Mode Selection

- Added optional async present path in `i_video.c` enabled with `-async-present`:
  - Main game/render thread enqueues a 320x200 snapshot (`I_VideoBuffer`) into a small queue.
  - A worker thread performs scale/convert + `DG_DrawFrame()` on queued frames.
- Measured result:
  - At `-scaling 5`, async present reduces PS-side present bottleneck and improved HW path FPS versus sync path.
- Added stream-mode guard in `doomgeneric_udp.c`:
  - If `-scaling > 1` and no explicit `-native320/-fullres` is provided, runtime auto-selects `fullres`.
  - If `-native320` is explicitly set with `-scaling > 1`, runtime prints a note that native mode forces scaling=1.
- Build note:
  - `doomgeneric/build.bat` links with `-pthread`.
- Safety:
  - Default behavior remains synchronous unless `-async-present` is passed.

## Stage 4.1 Baseline (Dual Resolution Path for PL/PS Testing)

- Fullres baseline is now **1600x1000** (exact 5x scale from 320x200).  
  1920x1080 is intentionally dropped from the Stage 4 path.
- Runtime output modes:
  - `320x200` via `-native320` (or `-res320` / `-out320`)
  - `1600x1000` via `-fullres` (or `-res1600` / `-out1600`)
- `doomgeneric_udp` auto-selects `fullres` when `-scaling > 1` is requested and no explicit stream mode is given.
- TCP stream now sends a small hello header (`DGv1`) with width/height so `doom_udp_viewer.py` auto-configures window size for either mode.

## Stage 4.1 Implementation (PL Upscale First Step)

- New runtime switch for PL scaling path:
  - `-pl-scale` (aliases: `-pl_scale`, `-hw-scale`, `-hw_scale`)
- If `-pl-scale` is requested and no explicit stream mode is set, runtime auto-selects `fullres` (1600x1000).
- Native mode guard:
  - If `-native320` is explicitly selected, PL upscale is disabled (native mode remains pure 320x200 output).

- Frame construction strategy (current):
  1. PS + existing HW 3D path produce final indexed 320x200 frame in `I_VideoBuffer`.
  2. In `I_FinishUpdate()`:
     - If PL upscale is enabled and menu is not active: call `HW_UpscaleFrame()` (MODE_UPSCALE), then `DG_DrawFrame()`.
     - If menu is active: fall back to existing PS scaling/composition path for correctness.
  3. `DG_DrawFrame()` always streams from `DG_ScreenBuffer` (now fullres in PL upscale mode).

- PL upscale data flow:
  - `command_buffer` AXI pointer is temporarily rebound to `PHY_VIDEO_BUF` (indexed 320x200 source).
  - `framebuffer_out` AXI pointer is temporarily rebound to `PHY_FB_ADDR` (1600x1000 RGBA destination).
  - HLS `MODE_UPSCALE` expands 320x200 to 1600x1000 using nearest-neighbor x5 and palette expansion.
  - RGB palette (256x3, gamma-corrected) is uploaded by PS into DDR right after colormap (`32*256` offset).
  - After MODE_UPSCALE, pointers are restored to normal draw path bindings.

- HLS mode notes:
  - `MODE_UPSCALE` writes aligned 128-bit bursts to fullres output.
  - `framebuffer_out` AXI depth is sized for 1600x1000x32bpp output.
  - `colormap_ddr` AXI depth includes both 8KB colormap and 768B RGB palette payload.

## Stage 4.2 (Concrete Frame Present Path in HLS)

- Goal:
  - Reduce PS-side frame construction dependency by presenting from PL BRAM framebuffer directly to output DDR.
  - Keep dual resolution testing support (`320x200` and `1600x1000`) without locking PL to one output mode.

- New HLS operation modes in `hls/doom_accel_v3.cpp`:
  - `MODE_PRESENT = 7`
    - Source: `local_framebuffer` (320x200 indexed in BRAM)
    - Destination: `framebuffer_out` (RGBA in DDR)
    - Supports `present_scale=1` (320x200) and `present_scale=5` (1600x1000)
  - `MODE_DRAW_AND_PRESENT = 8`
    - Executes batch draw first, then present in the same kernel invocation.

- New HLS AXI-Lite args:
  - `present_scale`: expected values `1` or `5`
  - `present_rows`: partial-row present control (`0` means all 200 rows)

- Register-map impact:
  - Existing Stage 2/3 registers remain valid.
  - Two new AXI-Lite controls are appended after `num_commands`.
  - Re-export HLS package and use updated generated header/register map before PS integration.

- Practical Stage 4 flow:
  1. PS submits draw commands as usual.
  2. PL renders into BRAM `local_framebuffer`.
  3. PL present mode converts indexed->RGBA using DDR palette and writes final frame to DDR scanout/stream buffer.
  4. PS handles control + optional menu fallback; main 3D frame no longer needs PS-side per-pixel composition in benchmark path.

- Notes:
  - Menu/HUD fallback policy can remain PS-driven initially for correctness while benchmarking 3D path.
  - For production scanout, pair this with double-buffered DDR scanout addresses and vsync pageflip on PS/DRM side.

## Stage 4.3 (Multi-Lane PL Output on ZU3EG PS-PL Slave Ports)

- Goal:
  - Increase PL->DDR frame write throughput for `1600x1000` output without CPU scaling.
  - Use multiple 128-bit AXI masters in parallel instead of trying to exceed 128-bit on one HP/HPC slave port.

- HLS interface changes (`hls/doom_accel_v3.cpp`):
  - Added optional framebuffer AXI outputs:
    - `framebuffer_out` (FB0, existing)
    - `framebuffer_out1` (FB1)
    - `framebuffer_out2` (FB2)
    - `framebuffer_out3` (FB3)
  - Added AXI-Lite scalar:
    - `present_lanes` (`1` or `4`)
  - `MODE_UPSCALE` and `MODE_PRESENT` x5 write path now support:
    - `present_lanes=1`: legacy single-lane writes
    - `present_lanes=4`: quad-lane writes (4 x 128-bit writes/cycle target in write loop)

## Stage 5 Composite Present (HUD/Menu Included in PL Path)

- Runtime now supports a **composite present source** for PL upscale:
  - Present source = `PHY_VIDEO_BUF` (composed indexed frame in DDR)
  - This includes software overlays (HUD/menu/messages) in the PL output.
- Composite mode is enabled by default:
  - `DOOM_PL_COMPOSITE=1` (default)
  - Set `DOOM_PL_COMPOSITE=0` to allow raster->present shared BRAM source experiments.
- `i_video.c` behavior:
  - In composite mode, menu no longer forces PS scaling fallback.
  - PL upscale can be used with menu/HUD correctness preserved.
- `-screen` direct present behavior:
  - Runtime reads active `/dev/fb0` scanout offsets and physical base.
  - If fb0 is 32bpp, PL present outputs `XRGB8888` directly to scanout.
  - If fb0 is 16bpp (common on PYNQ setups), PL present outputs `RGB565` directly to scanout.
  - Present stride is programmed from fb0 runtime stride so non-1600 display widths (eg 2560x1440) are handled without CPU copy.

- Software control changes:
  - New runtime controls in driver:
    - `HW_SetPresentLanes(int)` / `HW_GetPresentLanes()`
  - New CLI switch:
    - `-pl-lanes <1|4>` (aliases: `-pl_lanes`, `-fb-lanes`, `-fb_lanes`)
  - Runtime prints requested/effective lane count in bench startup logs.

- Important addressing rule:
  - In Stage 4.3 implementation, all FB lane pointers are programmed to the same base DDR framebuffer address.
  - HLS writes disjoint word ranges per lane into that same frame, producing one contiguous output image.

- Vivado integration guidance (required to realize benefit):
  - Connect `FB0..FB3` masters to separate PS slave interfaces when possible:
    - Prefer spreading across HP/HPC ports (`HPx_FPD` / `HPCx_FPD`) to avoid one-lane bottleneck.
  - Keep CMD/CMAP/TEX on existing ports unless contention analysis says otherwise.
  - Re-export HLS IP, update BD connections, regenerate bitstream, then benchmark `-pl-lanes 1` vs `-pl-lanes 4`.

## Stage 4.4 (X5 Expand Loop Throughput Fix)

- Problem observed in HLS reports:
  - `PRESENT_EXPAND_X5` / `UPSCALE_EXPAND_ROW` hot loops were running at `II=5`, so widening AXI write lanes alone could not deliver expected x5-frame speedup.

- Kernel-side optimization applied:
  - Replaced per-pixel dynamic lane/packed-word update chain with a two-step row pipeline:
    1. `indexed -> RGBA` row conversion (`320` pixels, `II=1` target).
    2. Deterministic `x5` word pack (`400` words, `II=1` target) using running quotient/remainder for `/5` mapping.
  - Added `upscale_src_rgba[320]` row buffer in BRAM to decouple palette fetch from word packing.
  - This removes the previous loop-carried dependency on dynamic `lane/out_word_idx/packed` updates that forced `II=5`.

- Scope:
  - Applied in both:
    - `MODE_UPSCALE` row generation path.
    - `MODE_PRESENT` x5 row generation path.

## Stage 5 Architecture Contract (PS -> PL)

- Core model:
  - PS runs classic DOOM game logic and visibility/BSP traversal.
  - PL executes raster work from a command stream and builds/presents frame output.

- What PS sends to PL every frame:
  - A command buffer of `DrawCommand` entries (`32B` each), up to `MAX_COMMANDS`.
  - Command payload fields:
    - `cmd_type`: `COLUMN` (walls) or `SPAN` (floors/ceilings)
    - `cmap_index`: light level index
    - `x1/x2/y1/y2`: destination coverage in 320x200 view space
    - `frac/step`: fixed-point texture stepping
    - `tex_offset`: offset into texture atlas in DDR
  - Control scalars (mode, command count, present settings) via AXI-Lite.

- What PS does NOT send per frame:
  - Full texture pixels for each draw call.
  - Full 1600x1000 framebuffer (unless forcing software fallback).

- Static/rarely-updated data PS uploads:
  - Texture atlas in DDR (`Upload_Texture_Data`) with persistent offsets.
  - Colormap table (light mapping).
  - RGB palette table for indexed->RGBA expansion in PL present/upscale path.

- Why upscale is needed for 1600x1000:
  - The classic DOOM renderer is fundamentally a 320x200 renderer.
  - The command stream describes rasterization in 320x200 pixel space.
  - Therefore, if output target is 1600x1000, a scale/present step is required after native raster result is formed.

- Practical output options:
  - Native path:
    - Render + present at 320x200 (no upscale cost).
  - Fullres path (current stage goal):
    - Render at 320x200 in PL, then upscale/present to 1600x1000 in PL.
  - True high-res raster path (future, major redesign):
    - PS must generate high-res draw commands (or PL must reinterpret geometry at higher raster density), not just upscale native pixels.
    - This is a different renderer contract and much larger engine-side change.

- Stage 5 direction:
  - Keep PS->PL contract command-based (not framebuffer-copy based).
  - Keep textures/colormap/palette persistent in DDR.
  - Keep PL responsible for final 1600x1000 present so PS does minimal per-pixel work.

## Stage 5.1 Performance/Quality Target (Current Plan)

- Project direction update:
  - Treat Stage 4 as historical exploration; active implementation track is now Stage 5.
  - Stage 5 focuses on quality + speed at `1600x1000` fullres output.

- Accepted quality/performance target:
  - Add quality uplift (CAS-style sharpening in PL) while keeping total present cost in the `2-3 ms/frame` range.

- Current baseline (before CAS fusion):
  - Fullres x5 present path is ~`1.85 ms/frame` (best-case from latest csynth estimate).

- Stage 5 optimization budget:
  - Available budget for sharpening + edge handling is roughly `0.8-1.2 ms/frame` to remain near `2.7-3.0 ms`.
  - If sharpening exceeds this budget, fallback is to reduce strength or move to selective/ROI sharpen.

- Stage 5 implementation start:
  - HLS fast profile is forced to fullres path:
    - x5 present path only (`1600x1000`)
    - quad-lane FB write path only (`FB0..FB3`)
  - This removes runtime branching from hot present/upscale paths and prepares kernel for CAS fusion.
  - Runtime defaults are aligned to this profile (`present_lanes=4` unless explicitly overridden).

- Stage 5 hardware resource strategy:
  - Use BRAM aggressively for line buffers/windowing (ZU3EG BRAM budget is sufficient for this stage).
  - Use fixed-point arithmetic and limited DSP footprint for sharpen core.

## Stage 5.2 Split-IP Baseline (Before Metrics)

- Motivation:
  - The monolithic kernel has grown too large and mixes unrelated concerns (raster + present/upscale).
  - Stage 5 baseline now splits into two IPs so timing/resource iteration is cleaner before further optimization.

- New HLS tops (same register argument ordering contract as Stage 4 kernel):
  - `hls/doom_raster_v1.cpp`
    - Top: `doom_raster`
    - Handles: `MODE_LOAD_COLORMAP`, `MODE_CLEAR_FB`, `MODE_DRAW_BATCH`, `MODE_DMA_OUT`, `MODE_DRAW_AND_DMA`
    - Output: indexed 320x200/320x168 data path (same as prior raster stage)
  - `hls/doom_present_v1.cpp`
    - Top: `doom_present`
    - Handles: `MODE_UPSCALE` and `MODE_PRESENT`
    - Input: indexed 320x200 source buffer (via `command_buffer` pointer)
    - Output: RGBA 320x200 (`scale=1`) or 1600x1000 (`scale=5`)
    - Supports `present_lanes=1` and `present_lanes=4` (FB0..FB3)

- PS driver split baseline:
  - Raster control base:
    - `ACCEL_BASE_ADDR = 0xA0000000`
  - Present control base:
    - `ACCEL_PRESENT_BASE_ADDR = 0xA0010000`
  - Runtime keeps monolithic fallback:
    - if present IP mapping fails, `present_regs` falls back to `accel_regs`.

- Integration expectation in Vivado:
  - Instantiate both HLS IPs.
  - Connect raster m_axi ports for command/texture/colormap/indexed output.
  - Connect present m_axi ports for indexed input + fullres output lanes.
  - Keep AXI-Lite maps at distinct base addresses matching the macros above.

- Baseline goal for this stage:
  - Validate functional split and stable frame flow first.
  - Only then start new synth/timing/perf comparisons.

## Stage 5.3 Quad-Only Present Path (Overhead Removal)

- Present IP is now intentionally single-mode in fast path:
  - `1600x1000` output only (`x5` from 320x200 source)
  - Quad-lane writes only (`FB0..FB3`)
  - No runtime branch for `scale=1` or `lanes=1`

- Software alignment:
  - `HW_SetPresentLanes()` now hard-clamps to 4 (quad-only).
  - CLI may still accept lane argument for compatibility, but effective value is always 4.

- Reason:
  - Remove control/branch overhead from the hot present loop before introducing CAS.
  - Establish highest-throughput baseline for Stage 5 quality work.

## Stage 5.4 Shared BRAM Raster->Present Handoff

- Goal:
  - Remove indexed-frame DDR roundtrip between raster and present IP.
  - Keep PS out of the per-pixel handoff path for gameplay frames.

- Address contract:
  - `PHY_STAGE5_BRAM_BUF = 0xA1000000`
  - This must be the AXI BRAM Controller base mapped in Vivado Address Editor.

- Runtime policy (software):
  - `HW_SetRasterSharedBRAM(1)`:
    - Raster `framebuffer_out` base is switched to `PHY_STAGE5_BRAM_BUF`.
    - Present source pointer (`command_buffer` arg) is switched to `PHY_STAGE5_BRAM_BUF`.
    - Raster DMA row count is set to `200` (full indexed frame handoff).
  - `HW_SetRasterSharedBRAM(0)`:
    - Raster output and present source both revert to `PHY_VIDEO_BUF` (DDR).
    - Raster DMA row count is set to `168` (legacy view-only DMA preserving software HUD/menu overlay).
- Stage5 benchmark-fast path:
  - when `-bench-hw -pl-scale` is used, runtime forces:
    - `PL composite = OFF`
    - `Raster shared BRAM handoff = ON`
  - `i_video.c` overlays HUD/menu from PS after PL present:
    - gameplay: HUD/status band only (low overhead)
    - menu: full-frame software overlay (correctness path)
  - `i_video.c` keeps deterministic PL present path for gameplay frames.
  - `DOOM_STAGE5_BRAM_HANDOFF=0` disables BRAM handoff globally at runtime.

- Vivado integration expectation:
  - Keep raster/present AXI-Lite control on separate bases (`0xA0000000` / `0xA0010000`).
  - Connect raster `m_axi_FB` and present source-read path to the BRAM controller address space used above.
  - Keep fullres present output lanes (`FB0..FB3`) on DDR HP/HPC path.

- Notes:
  - Monolithic fallback (`present_regs == accel_regs`) continues to use DDR source (`PHY_VIDEO_BUF`).
  - This stage is about cutting handoff overhead; it does not replace final DDR scanout writes.
  - Present IP palette cache behavior:
    - `MODE_LOAD_COLORMAP` now preloads the 256-entry RGB expansion palette into BRAM in present IP.
    - Driver triggers this mode for present IP during `Upload_Colormap()` when split IP is active.
    - Frame-present modes reuse BRAM palette (DDR palette read is only fallback on cold start).

## Stage 5.5 Interface Cleanup (Vivado Wiring Simplification)

- Objective:
  - Keep software register map compatibility while removing unused AXI master ports from the block design.

- Raster IP (`doom_raster_v1.cpp`):
  - Removed AXI masters `FB1/FB2/FB3`.
  - Kept corresponding control arguments as 64-bit AXI-Lite placeholders (`framebuffer_out1/2/3`) so software offsets stay stable.

- Present IP (`doom_present_v1.cpp`):
  - Removed AXI master `TEX` (unused by present path).
  - Kept `texture_atlas` as 64-bit AXI-Lite placeholder for register compatibility.

- Result:
  - Fewer interconnect links in Vivado:
    - Raster now has one FB write master + CMD/CMAP/TEX reads.
    - Present now has FB0..FB3 writes + CMD/CMAP reads.
  - Driver-side register programming remains valid without immediate offset churn.

## Stage 6.0 BRAM Overlay + Stage 6.1 CAS (Current Direction)

- Immediate objective:
  - Keep `-bench-hw -pl-scale` on BRAM source path with low overhead HUD/menu correctness.

- BRAM overlay architecture:
  - Raster IP writes indexed frame to shared BRAM (`PHY_STAGE5_BRAM_BUF`).
  - Present IP reads from shared BRAM and performs upscale/output.
  - PS overlays are applied on final output buffer after PL present:
    - gameplay: HUD/status bar band only (rows 168..199 scaled)
    - menu: full-frame software overlay path
  - Composite mode stays OFF in this path.

- Stage 6.1 visual quality path:
  - Add optional CAS-lite in present IP (post-upscale, before writeout).
  - Runtime controls:
    - `cas_enable` (0/1)
    - `cas_strength` (0..255)
  - Keep default OFF to preserve baseline perf measurements.
