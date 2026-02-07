# Stage 5 Split-IP Baseline

This baseline splits the monolithic accelerator into two independent HLS tops:

- `doom_raster_v1.cpp` (top: `doom_raster`)
- `doom_present_v1.cpp` (top: `doom_present`)

## Bring-up order

1. Synthesize/package `doom_raster`.
2. Synthesize/package `doom_present`.
3. In Vivado BD, assign AXI-Lite base addresses:
   - Raster: `0xA0010000`
   - Present: `0xA0000000`
4. Connect m_axi ports and regenerate bitstream.
5. Deploy and run software baseline before collecting metrics.

## Software assumptions

`doomgeneric/doom_accel.h` defaults to:

- `ACCEL_BASE_ADDR = 0xA0010000` (raster)
- `ACCEL_PRESENT_BASE_ADDR = 0xA0000000` (present)

Runtime overrides are available without recompiling:

- `DOOM_RASTER_BASE` (hex/dec, eg `0xA0010000`)
- `DOOM_PRESENT_BASE` (hex/dec, eg `0xA0000000`)
- `DOOM_SWAP_IPS=1` (swap defaults quickly if Vivado mapping is flipped)
- `DOOM_STAGE5_BRAM_HANDOFF=0` (disable raster->present shared BRAM handoff; default is enabled for performance)
- `DOOM_PL_COMPOSITE=1` (default; present reads composed `PHY_VIDEO_BUF` so HUD/menu are included)

If the present IP mapping fails at runtime, software falls back to monolithic behavior by reusing raster regs.

## Current present mode

- Present kernel fast path is now quad-only for stage 5:
  - x5 upscale to 1600x1000
  - FB0..FB3 parallel writes
  - no single-lane/native scaling branch in the hot path
- Present output format/stride controls:
  - `present_format=0`: XRGB8888 output
  - `present_format=1`: RGB565 output
  - `present_stride_bytes`: destination row stride (supports direct fb0 scanout with non-1600 width)
- Composite runtime behavior (software integration):
  - Default path presents from `PHY_VIDEO_BUF` (full composed indexed frame) so HUD/menu/messages are visible.
  - Shared BRAM handoff can still be tested by setting `DOOM_PL_COMPOSITE=0`.
