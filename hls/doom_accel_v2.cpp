/**
 * DOOM FPGA Accelerator v2 - Batch Rendering with BRAM Framebuffer
 * 
 * Architecture:
 * - Colormap stored in BRAM (8KB) - loaded once at level start
 * - Framebuffer in BRAM (64KB) - persists across frames
 * - Texture atlas in DDR (16MB) - FPGA reads directly via AXI
 * - Command buffer in DDR - batch of draw commands per frame
 * 
 * Modes:
 * - MODE_LOAD_COLORMAP (1): DMA colormap from DDR to BRAM
 * - MODE_CLEAR_FB (2): Clear framebuffer BRAM
 * - MODE_DRAW_BATCH (3): Process N commands from command buffer
 * - MODE_DMA_OUT (4): DMA framebuffer BRAM to DDR
 */

#include <stdint.h>
#include <ap_int.h>
#include <string.h>

// Screen constants
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 200
#define SBAR_HEIGHT   32   // Status bar (HUD) height at bottom of screen
#define VIEW_HEIGHT   (SCREEN_HEIGHT - SBAR_HEIGHT)  // 168 rows for game view
#define FB_SIZE       (SCREEN_WIDTH * SCREEN_HEIGHT)  // 64000 bytes total
#define VIEW_SIZE     (SCREEN_WIDTH * VIEW_HEIGHT)    // 53760 bytes game view only
#define FRACBITS      16

// Colormap: 32 light levels x 256 palette entries = 8KB
#define NUM_LIGHT_LEVELS 32
#define COLORMAP_SIZE    (NUM_LIGHT_LEVELS * 256)

// Command structure (must match C driver - EXACTLY 32 bytes)
// Using explicit padding to ensure alignment matches
struct __attribute__((packed)) DrawCommand {
    uint8_t  cmd_type;    // 0=column, 1=span       [0]
    uint8_t  cmap_index;  // Light level (0-31)     [1]
    uint16_t x1;          // Start X                [2-3]
    uint16_t x2;          // End X (span only)      [4-5]
    uint16_t y1;          // Start Y / Y row        [6-7]
    uint16_t y2;          // End Y (column only)    [8-9]
    uint16_t reserved1;   // Padding                [10-11]
    uint32_t frac;        // Texture position       [12-15]
    uint32_t step;        // Texture step           [16-19]
    uint32_t tex_offset;  // Offset into atlas      [20-23]
    uint32_t reserved2;   // Padding to 32 bytes    [24-27]
    uint32_t reserved3;   // Padding to 32 bytes    [28-31]
};

// Command types
#define CMD_TYPE_COLUMN 0
#define CMD_TYPE_SPAN   1

// Modes
#define MODE_IDLE           0
#define MODE_LOAD_COLORMAP  1
#define MODE_CLEAR_FB       2
#define MODE_DRAW_BATCH     3
#define MODE_DMA_OUT        4

extern "C" {

void doom_accel(
    // DDR pointers (AXI Master)
    volatile uint8_t* framebuffer_out,    // Where to DMA framebuffer (320x200)
    const uint8_t* texture_atlas,          // Texture data in DDR
    const uint8_t* colormap_ddr,           // Colormap source in DDR
    const DrawCommand* command_buffer,     // Batch of draw commands
    // Control parameters
    uint32_t mode,                         // Operation mode
    uint32_t num_commands                  // Number of commands (for MODE_DRAW_BATCH)
) {
    // ========================================================================
    // INTERFACE PRAGMAS
    // ========================================================================
    
    // AXI Master ports for DDR access
    #pragma HLS INTERFACE m_axi port=framebuffer_out depth=64000 offset=slave bundle=FB
    #pragma HLS INTERFACE m_axi port=texture_atlas depth=16777216 offset=slave bundle=TEX max_read_burst_length=256
    #pragma HLS INTERFACE m_axi port=colormap_ddr depth=8192 offset=slave bundle=CMAP
    #pragma HLS INTERFACE m_axi port=command_buffer depth=4000 offset=slave bundle=CMD max_read_burst_length=32

    // AXI-Lite control (unified CTRL bundle)
    #pragma HLS INTERFACE s_axilite port=framebuffer_out bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=texture_atlas bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=colormap_ddr bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=command_buffer bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=mode bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=num_commands bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=return bundle=CTRL

    // ========================================================================
    // STATIC BRAM STORAGE (Persists across calls!)
    // ========================================================================
    static uint8_t local_colormap[COLORMAP_SIZE];  // 8KB - 32 light levels
    static uint8_t local_framebuffer[FB_SIZE];      // 64KB - 320x200
    
    #pragma HLS BIND_STORAGE variable=local_colormap type=ram_2p impl=bram
    #pragma HLS BIND_STORAGE variable=local_framebuffer type=ram_2p impl=bram

    // ========================================================================
    // MODE DISPATCH
    // ========================================================================
    
    if (mode == MODE_LOAD_COLORMAP) {
        // --------------------------------------------------------------------
        // Load colormap from DDR to BRAM (one-time at level start)
        // --------------------------------------------------------------------
        LOAD_CMAP:
        for (int i = 0; i < COLORMAP_SIZE; i++) {
            #pragma HLS PIPELINE II=1
            local_colormap[i] = colormap_ddr[i];
        }
    }
    else if (mode == MODE_CLEAR_FB) {
        // --------------------------------------------------------------------
        // Clear framebuffer BRAM to palette index 0 (black)
        // --------------------------------------------------------------------
        CLEAR_FB:
        for (int i = 0; i < FB_SIZE; i++) {
            #pragma HLS PIPELINE II=1
            local_framebuffer[i] = 0;
        }
    }
    else if (mode == MODE_DRAW_BATCH) {
        // --------------------------------------------------------------------
        // Process batch of draw commands (columns and spans)
        // --------------------------------------------------------------------
        
        // Local texture cache (128 bytes for columns, 4096 bytes for 64x64 flats)
        uint8_t local_tex[4096];  // Max size for flat textures
        #pragma HLS BIND_STORAGE variable=local_tex type=ram_1p impl=bram
        
        uint32_t last_tex_offset = 0xFFFFFFFF;  // Cache invalidation tracker
        uint32_t last_tex_size = 0;
        
        PROCESS_COMMANDS:
        for (uint32_t cmd_idx = 0; cmd_idx < num_commands; cmd_idx++) {
            // Read command from DDR
            DrawCommand cmd = command_buffer[cmd_idx];
            
            uint8_t cmd_type = cmd.cmd_type;
            uint8_t cmap_idx = cmd.cmap_index;
            uint16_t x1 = cmd.x1;
            uint16_t x2 = cmd.x2;
            uint16_t y1 = cmd.y1;
            uint16_t y2 = cmd.y2;
            uint32_t frac = cmd.frac;
            uint32_t step = cmd.step;
            uint32_t tex_offset = cmd.tex_offset;
            
            // Colormap base offset
            uint32_t cmap_base = (uint32_t)cmap_idx * 256;
            
            if (cmd_type == CMD_TYPE_COLUMN) {
                // ===========================================================
                // COLUMN (Wall) - Vertical strip
                // ===========================================================
                if (x1 >= SCREEN_WIDTH) continue;
                if (y1 >= SCREEN_HEIGHT) y1 = SCREEN_HEIGHT - 1;
                if (y2 >= SCREEN_HEIGHT) y2 = SCREEN_HEIGHT - 1;
                if (y1 > y2) continue;
                
                // Prefetch texture column (128 bytes) if different from cached
                if (tex_offset != last_tex_offset || last_tex_size != 128) {
                    PREFETCH_COL_TEX:
                    for (int t = 0; t < 128; t++) {
                        #pragma HLS PIPELINE II=1
                        local_tex[t] = texture_atlas[tex_offset + t];
                    }
                    last_tex_offset = tex_offset;
                    last_tex_size = 128;
                }
                
                uint32_t current_frac = frac;
                
                DRAW_COLUMN:
                for (uint16_t y = y1; y <= y2; y++) {
                    #pragma HLS PIPELINE II=1
                    
                    uint8_t tex_idx = (current_frac >> FRACBITS) & 127;
                    uint8_t tex_pixel = local_tex[tex_idx];
                    uint8_t lit_pixel = local_colormap[cmap_base + tex_pixel];
                    
                    uint32_t fb_offset = (uint32_t)y * SCREEN_WIDTH + (uint32_t)x1;
                    local_framebuffer[fb_offset] = lit_pixel;
                    
                    current_frac += step;
                }
            }
            else if (cmd_type == CMD_TYPE_SPAN) {
                // ===========================================================
                // SPAN (Floor/Ceiling) - Horizontal strip with 2D texture
                // ===========================================================
                uint16_t y = y1;  // y1 is the row for spans
                if (y >= SCREEN_HEIGHT) continue;
                if (x1 >= SCREEN_WIDTH) x1 = SCREEN_WIDTH - 1;
                if (x2 >= SCREEN_WIDTH) x2 = SCREEN_WIDTH - 1;
                if (x1 > x2) continue;
                
                // Prefetch flat texture (64x64 = 4096 bytes) if different
                if (tex_offset != last_tex_offset || last_tex_size != 4096) {
                    PREFETCH_FLAT_TEX:
                    for (int t = 0; t < 4096; t++) {
                        #pragma HLS PIPELINE II=1
                        local_tex[t] = texture_atlas[tex_offset + t];
                    }
                    last_tex_offset = tex_offset;
                    last_tex_size = 4096;
                }
                
                // Position and step are packed: upper 16 bits = X, lower 16 bits = Y
                // Each part has 6 bits integer, 10 bits fraction
                uint32_t position = frac;
                
                int count = x2 - x1;
                
                DRAW_SPAN:
                for (int i = 0; i <= count; i++) {
                    #pragma HLS PIPELINE II=1
                    
                    // Extract texture coordinates from packed position
                    // ytemp = (position >> 4) & 0x0fc0 -> gives row * 64
                    // xtemp = (position >> 26) -> gives column
                    uint32_t ytemp = (position >> 4) & 0x0fc0;
                    uint32_t xtemp = (position >> 26);
                    uint32_t spot = xtemp | ytemp;
                    
                    uint8_t tex_pixel = local_tex[spot & 4095];  // 64x64 wrap
                    uint8_t lit_pixel = local_colormap[cmap_base + tex_pixel];
                    
                    uint32_t fb_offset = (uint32_t)y * SCREEN_WIDTH + (uint32_t)(x1 + i);
                    local_framebuffer[fb_offset] = lit_pixel;
                    
                    position += step;
                }
            }
        }
    }
    else if (mode == MODE_DMA_OUT) {
        // --------------------------------------------------------------------
        // DMA framebuffer BRAM to DDR - ONLY game view area (rows 0-167)
        // The HUD (rows 168-199) is rendered by CPU and must be preserved
        // --------------------------------------------------------------------
        DMA_OUT:
        for (int i = 0; i < VIEW_SIZE; i++) {
            #pragma HLS PIPELINE II=1
            framebuffer_out[i] = local_framebuffer[i];
        }
    }
    // MODE_IDLE: Do nothing
}

} // extern "C"
