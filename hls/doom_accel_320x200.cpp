#include <stdint.h>
#include <ap_int.h>
#include <string.h> // For memcpy

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 200
#define FRACBITS 16

extern "C" {

/**
 * DOOM FPGA Accelerator - Optimized
 * * Features:
 * 1. Unified AXI-Lite Interface (bundle=CTRL) for simple driver access.
 * 2. BRAM Prefetching: Loads texture column and colormap into on-chip memory
 * before drawing to eliminate DDR latency stalls.
 * 3. AXI Bursting: Uses efficient memcpy for fetching data.
 */
void doom_accel(
    volatile uint8_t* video_buffer,    // Output buffer (320x200)
    const uint8_t* texture_atlas,      // Input texture data
    const uint8_t* colormap,           // Input lighting map
    uint64_t cmd1,                     // [step | frac]
    uint64_t cmd2,                     // [y_end | y_start | x]
    uint64_t cmd3                      // [colormap_offset | tex_offset]
) {
    // ------------------------------------------------------------------------
    // INTERFACE PRAGMAS
    // ------------------------------------------------------------------------
    
    // 1. DATA PORTS (AXI Master) - High bandwidth access to DDR
    #pragma HLS INTERFACE m_axi port=video_buffer depth=64000 offset=slave bundle=VIDEO
    #pragma HLS INTERFACE m_axi port=texture_atlas depth=65536 offset=slave bundle=TEX max_read_burst_length=128
    #pragma HLS INTERFACE m_axi port=colormap depth=256 offset=slave bundle=CMAP

    // 2. CONTROL PORT (AXI Lite) - UNIFIED into 'CTRL'
    // By assigning pointers to CTRL, their address registers appear in the same block as cmds
    #pragma HLS INTERFACE s_axilite port=video_buffer bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=texture_atlas bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=colormap bundle=CTRL
    
    // Command parameters
    #pragma HLS INTERFACE s_axilite port=cmd1 bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=cmd2 bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=cmd3 bundle=CTRL
    
    // Block Control (Start, Done, Idle, Ready)
    #pragma HLS INTERFACE s_axilite port=return bundle=CTRL

    // ------------------------------------------------------------------------
    // INTERNAL STORAGE (BRAM)
    // ------------------------------------------------------------------------
    uint8_t local_cmap[256];
    uint8_t local_tex[128];
    #pragma HLS BIND_STORAGE variable=local_cmap type=ram_1p
    #pragma HLS BIND_STORAGE variable=local_tex type=ram_1p

    // ------------------------------------------------------------------------
    // LOGIC
    // ------------------------------------------------------------------------

    // Unpack parameters
    uint32_t frac = cmd1 & 0xFFFFFFFF;
    uint32_t step = (cmd1 >> 32) & 0xFFFFFFFF;
    
    uint16_t x = (cmd2 >> 16) & 0xFFFF;
    uint16_t y_start = (cmd2 >> 32) & 0xFFFF;
    uint16_t y_end = (cmd2 >> 48) & 0xFFFF;
    
    uint32_t tex_base_offset = cmd3 & 0xFFFFFFFF;
    uint32_t colormap_offset = (cmd3 >> 32) & 0xFFFFFFFF;

    // Safety Checks
    if (x >= SCREEN_WIDTH) return;
    if (y_start >= SCREEN_HEIGHT) y_start = SCREEN_HEIGHT - 1;
    if (y_end >= SCREEN_HEIGHT) y_end = SCREEN_HEIGHT - 1;
    if (y_start > y_end) return;

    // STEP 1: PREFETCH (Burst Read from DDR -> BRAM)
    // This removes the latency of reading DDR for every single pixel.
    memcpy(local_cmap, (const uint8_t*)(colormap + colormap_offset), 256);
    memcpy(local_tex, (const uint8_t*)(texture_atlas + tex_base_offset), 128);

    // STEP 2: DRAWING LOOP (Pipeline II=1)
    // Now runs at 1 clock cycle per pixel!
    uint32_t current_frac = frac;

    DRAW_LOOP:
    for (uint16_t y = y_start; y <= y_end; y++) {
        #pragma HLS PIPELINE II=1
        
        // Fast Lookups from BRAM (1 cycle latency)
        // Note: Mask & 127 handles texture wrapping for standard 128-height textures
        uint8_t tex_index = (current_frac >> FRACBITS) & 127;
        uint8_t tex_pixel = local_tex[tex_index];
        uint8_t lit_pixel = local_cmap[tex_pixel];
        
        // Write Output
        uint32_t fb_offset = (uint32_t)y * SCREEN_WIDTH + (uint32_t)x;
        video_buffer[fb_offset] = lit_pixel;
        
        current_frac += step;
    }
}

} // extern "C"