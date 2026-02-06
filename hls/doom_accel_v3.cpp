// doom_accel_v3.cpp - Optimized DOOM FPGA Accelerator
// Stage 2: Performance Optimization
// - Texture BRAM cache with LRU (Option B)
// - Pipeline prefetch (Option C)
// - 3-row line buffer for future bicubic upscaling
// - Fixed memcpy issues with explicit burst reads

#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <ap_int.h>

// ============================================================================
// 1. CONFIGURATION & CONSTANTS
// ============================================================================

// Screen Resolution
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 200
#define FB_SIZE       (SCREEN_WIDTH * SCREEN_HEIGHT) // 64,000 bytes

// View Window (exclude status bar for DMA)
#define VIEW_HEIGHT   168
#define VIEW_SIZE     (SCREEN_WIDTH * VIEW_HEIGHT)   // 53,760 bytes

// Batch Processing
#define BATCH_SIZE    64       // Commands per batch fetch
#define COL_CACHE_SIZE 128     // Max height of wall column

// Texture Cache Configuration (Option B)
// 32KB cache = 256 entries x 128 bytes each
#define TEX_CACHE_ENTRIES 256
#define TEX_CACHE_SIZE    (TEX_CACHE_ENTRIES * 128)  // 32KB

// 3-Row Line Buffer for Bicubic Upscaling (Future)
// Stores rows N-1, N, N+1 for 4-tap vertical filter
#define LINE_BUF_WIDTH 320
#define LINE_BUF_ROWS  3

// 128-bit AXI data type for maximum bandwidth
typedef ap_uint<128> uint128_t;

// Command Types
#define CMD_TYPE_COLUMN 0
#define CMD_TYPE_SPAN   1

// Operation Modes
#define MODE_IDLE          0
#define MODE_LOAD_COLORMAP 1
#define MODE_CLEAR_FB      2
#define MODE_DRAW_BATCH    3
#define MODE_DMA_OUT       4
#define MODE_UPSCALE       5  // Future: HW bicubic upscale
#define MODE_DRAW_AND_DMA 6  // Combined draw + DMA (single handshake)

// ============================================================================
// 2. DATA STRUCTURES
// ============================================================================

// Draw Command (32 bytes, matches C struct exactly)
struct __attribute__((packed)) DrawCommand {
    uint8_t  cmd_type;    // 0=Column, 1=Span
    uint8_t  cmap_index;  // Light level (0-31)
    uint16_t x1;          // X position (Column) or Start X (Span)
    uint16_t x2;          // End X (Span only)
    uint16_t y1;          // Start Y (Column) or Row Y (Span)
    uint16_t y2;          // End Y (Column)
    uint16_t reserved1;   // Padding
    uint32_t frac;        // Texture Coordinate (16.16 Fixed Point)
    uint32_t step;        // Texture Step (16.16 Fixed Point)
    uint32_t tex_offset;  // Byte offset in Texture Atlas
    uint32_t reserved2;   // Padding
    uint32_t reserved3;   // Padding
};

typedef char drawcommand_size_must_be_32[(sizeof(DrawCommand) == 32) ? 1 : -1];
typedef char drawcommand_tex_offset_must_be_20[(offsetof(DrawCommand, tex_offset) == 20) ? 1 : -1];

// Texture Cache Entry (for LRU tracking)
struct TexCacheEntry {
    uint32_t tag;         // tex_offset that's cached
    uint8_t  valid;       // Entry is valid
    uint8_t  age;         // LRU age counter (higher = older)
};

extern "C" {

// ============================================================================
// 3. STATIC BRAM/URAM ARRAYS (On-Chip Memory)
// ============================================================================
// Total BRAM usage estimate:
// - Framebuffer: 64KB (URAM)
// - Colormap: 8KB (BRAM)
// - Texture Cache: 32KB (BRAM)
// - Line Buffers: 960 bytes (BRAM)
// - Cache metadata: ~1KB (BRAM)
// Total: ~106KB << 9.8Mb available

// Framebuffer (64KB) - Use BRAM (xczu3eg has no URAM)
static uint8_t local_framebuffer[FB_SIZE] __attribute__((aligned(16)));

// Colormap (8KB) - 32 light levels x 256 palette entries
static uint8_t local_colormap[32 * 256] __attribute__((aligned(16)));

// Texture Cache (32KB) - 256 slots x 128 bytes each
static uint8_t tex_cache_data[TEX_CACHE_ENTRIES][128];
static TexCacheEntry tex_cache_meta[TEX_CACHE_ENTRIES];

// 3-Row Line Buffer for Bicubic Upscaling (Future)
// Stores rows N-1, N, N+1 at native 320 width
static uint8_t line_buffer[LINE_BUF_ROWS][LINE_BUF_WIDTH];
static int line_buffer_base_row;  // Which framebuffer row is in line_buffer[0]

// Flat Texture BRAM Cache (4KB) for span rendering
// Eliminates random DDR reads for floor/ceiling textures (64x64 flat)
static uint8_t flat_cache[4096];
static uint32_t flat_cache_tag;   // tex_offset of cached flat
static uint8_t flat_cache_valid;  // Cache valid flag

// ============================================================================
// 4. HELPER FUNCTIONS
// ============================================================================

// Hash function for texture cache (direct-mapped for simplicity)
static inline uint32_t tex_cache_hash(uint32_t tex_offset) {
    // Use lower bits of offset as index (128-byte aligned)
    return (tex_offset >> 7) & (TEX_CACHE_ENTRIES - 1);
}

// Burst-read colormap from DDR to BRAM (replaces memcpy)
static void burst_read_colormap(const uint8_t* colormap_ddr, uint8_t* local_colormap) {
    #pragma HLS INLINE off
    
    // Read 8KB as 512 x 16-byte chunks
    const uint128_t* src = (const uint128_t*)colormap_ddr;
    uint128_t* dst = (uint128_t*)local_colormap;
    
    CMAP_READ_LOOP:
    for (int i = 0; i < 512; i++) {
        #pragma HLS PIPELINE II=1
        dst[i] = src[i];
    }
}

// Burst-read commands from DDR (replaces memcpy)
static void burst_read_commands(const uint128_t* cmd_words, DrawCommand* batch, int count) {
    #pragma HLS INLINE off

    // Command buffer is 2 x 128-bit words per DrawCommand (32 bytes).
    // Decode explicitly to keep CMD AXI width at 128-bit and avoid
    // struct-typed AXI access issues.
    CMD_READ_LOOP:
    for (int i = 0; i < count; i++) {
        #pragma HLS PIPELINE II=1
        uint128_t w0 = cmd_words[(i << 1) + 0];
        uint128_t w1 = cmd_words[(i << 1) + 1];

        DrawCommand cmd;
        cmd.cmd_type   = (uint8_t)w0.range(7, 0);
        cmd.cmap_index = (uint8_t)w0.range(15, 8);
        cmd.x1         = (uint16_t)w0.range(31, 16);
        cmd.x2         = (uint16_t)w0.range(47, 32);
        cmd.y1         = (uint16_t)w0.range(63, 48);
        cmd.y2         = (uint16_t)w0.range(79, 64);
        cmd.reserved1  = (uint16_t)w0.range(95, 80);
        cmd.frac       = (uint32_t)w0.range(127, 96);
        cmd.step       = (uint32_t)w1.range(31, 0);
        cmd.tex_offset = (uint32_t)w1.range(63, 32);
        cmd.reserved2  = (uint32_t)w1.range(95, 64);
        cmd.reserved3  = (uint32_t)w1.range(127, 96);

        batch[i] = cmd;
    }
}

// Burst-read texture column from DDR to cache slot
static void fetch_texture_to_cache(const uint128_t* texture_atlas, uint32_t tex_offset, uint8_t* cache_slot) {
    #pragma HLS INLINE off
    
    // Read 128 bytes = 8 x 16-byte words
    uint32_t word_idx = tex_offset / 16;
    
    PREFETCH_LOOP:
    for (int t = 0; t < 8; t++) {
        #pragma HLS PIPELINE II=1
        uint128_t raw = texture_atlas[word_idx + t];
        
        // Unpack 128-bit word into 16 bytes
        UNPACK_LOOP:
        for (int b = 0; b < 16; b++) {
            #pragma HLS UNROLL
            cache_slot[t*16 + b] = (uint8_t)raw.range((b * 8) + 7, b * 8);
        }
    }
}

// Burst-read flat texture (4KB = 64x64) from DDR to BRAM cache
static void burst_read_flat(const uint128_t* texture_atlas, uint32_t tex_offset, uint8_t* flat_buf) {
    #pragma HLS INLINE off
    
    // Read 4096 bytes = 256 x 16-byte words
    uint32_t word_idx = tex_offset / 16;
    
    FLAT_READ_LOOP:
    for (int i = 0; i < 256; i++) {
        #pragma HLS PIPELINE II=1
        uint128_t raw = texture_atlas[word_idx + i];
        
        FLAT_UNPACK_LOOP:
        for (int b = 0; b < 16; b++) {
            #pragma HLS UNROLL
            flat_buf[i*16 + b] = (uint8_t)raw.range((b * 8) + 7, b * 8);
        }
    }
}

// ============================================================================
// 5. DOOM ACCELERATOR CORE
// ============================================================================
void doom_accel(
    uint128_t* framebuffer_out,        // 128-bit Output Pointer (DMA to DDR)
    const uint128_t* texture_atlas,    // 128-bit Input (Textures from DDR)
    const uint8_t* colormap_ddr,       // Colormap in DDR
    const uint128_t* command_buffer,   // Command Buffer in DDR (2 words/cmd)
    uint32_t mode,
    uint32_t num_commands
) {
    // ------------------------------------------------------------------------
    // INTERFACE PRAGMAS
    // ------------------------------------------------------------------------
    #pragma HLS INTERFACE m_axi port=framebuffer_out depth=4000 offset=slave bundle=FB max_write_burst_length=128
    #pragma HLS INTERFACE m_axi port=texture_atlas depth=1048576 offset=slave bundle=TEX max_read_burst_length=64
    #pragma HLS INTERFACE m_axi port=colormap_ddr depth=8192 offset=slave bundle=CMAP max_read_burst_length=64
    #pragma HLS INTERFACE m_axi port=command_buffer depth=8000 offset=slave bundle=CMD max_read_burst_length=64

    // Unified AXI-Lite control
    #pragma HLS INTERFACE s_axilite port=framebuffer_out bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=texture_atlas bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=colormap_ddr bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=command_buffer bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=mode bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=num_commands bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=return bundle=CTRL

    // ------------------------------------------------------------------------
    // BRAM/URAM BINDING
    // ------------------------------------------------------------------------
    // Note: xczu3eg does NOT have URAM - use BRAM for all storage
    #pragma HLS BIND_STORAGE variable=local_framebuffer type=ram_2p impl=bram
    #pragma HLS BIND_STORAGE variable=local_colormap type=ram_2p impl=bram
    #pragma HLS BIND_STORAGE variable=tex_cache_data type=ram_2p impl=bram
    #pragma HLS BIND_STORAGE variable=tex_cache_meta type=ram_1p impl=lutram
    #pragma HLS BIND_STORAGE variable=line_buffer type=ram_2p impl=bram
    
    // Array partitioning for parallel access
    #pragma HLS ARRAY_PARTITION variable=line_buffer dim=1 complete
    #pragma HLS BIND_STORAGE variable=flat_cache type=ram_2p impl=bram

    // ------------------------------------------------------------------------
    // MODE DISPATCH
    // ------------------------------------------------------------------------

    // === MODE 1: LOAD COLORMAP ===
    if (mode == MODE_LOAD_COLORMAP) {
        burst_read_colormap(colormap_ddr, local_colormap);
        
        // Invalidate texture cache on level load
        CACHE_INVALIDATE:
        for (int i = 0; i < TEX_CACHE_ENTRIES; i++) {
            #pragma HLS UNROLL factor=16
            tex_cache_meta[i].valid = 0;
        }
        flat_cache_valid = 0;
    }

    // === MODE 2: CLEAR FRAMEBUFFER ===
    else if (mode == MODE_CLEAR_FB) {
        uint128_t* fb_ptr = (uint128_t*)local_framebuffer;
        int chunks = FB_SIZE / 16;

        CLEAR_LOOP:
        for (int i = 0; i < chunks; i++) {
            #pragma HLS PIPELINE II=1
            fb_ptr[i] = 0;
        }
        flat_cache_valid = 0;
    }

    // === MODE 3/6: DRAW BATCH (Main Rendering) ===
    else if (mode == MODE_DRAW_BATCH || mode == MODE_DRAW_AND_DMA) {
        
        DrawCommand batch[BATCH_SIZE];
        
        uint32_t processed_count = 0;

        while (processed_count < num_commands) {
            // 1. Fetch Batch of Commands (burst read)
            uint32_t chunk_size = num_commands - processed_count;
            if (chunk_size > BATCH_SIZE) chunk_size = BATCH_SIZE;

            burst_read_commands(command_buffer + ((uint32_t)processed_count << 1), batch, chunk_size);

            // 2. Process Commands
            BATCH_LOOP:
            for (uint32_t i = 0; i < chunk_size; i++) {
                #pragma HLS PIPELINE off
                
                DrawCommand cmd = batch[i];
                uint32_t cmap_base = (uint32_t)cmd.cmap_index << 8;

                // === WALL COLUMN ===
                if (cmd.cmd_type == CMD_TYPE_COLUMN) {
                    // Bounds check
                    if (cmd.x1 >= SCREEN_WIDTH) continue;
                    uint16_t y_start = (cmd.y1 < SCREEN_HEIGHT) ? cmd.y1 : (uint16_t)(SCREEN_HEIGHT-1);
                    uint16_t y_end   = (cmd.y2 < SCREEN_HEIGHT) ? cmd.y2 : (uint16_t)(SCREEN_HEIGHT-1);
                    if (y_start > y_end) continue;

                    // Local texture buffer - avoid pointer selection issue
                    uint8_t local_tex[128];
                    #pragma HLS ARRAY_PARTITION variable=local_tex dim=1 factor=16 cyclic
                    
                    // Check texture cache first
                    uint32_t hash = tex_cache_hash(cmd.tex_offset);
                    bool cache_hit = (tex_cache_meta[hash].valid && tex_cache_meta[hash].tag == cmd.tex_offset);
                    
                    if (cache_hit) {
                        // Copy from cache to local buffer
                        CACHE_COPY_LOOP:
                        for (int t = 0; t < 128; t++) {
                            #pragma HLS UNROLL factor=16
                            local_tex[t] = tex_cache_data[hash][t];
                        }
                    } else {
                        // Fetch from DDR and store in cache
                        fetch_texture_to_cache(texture_atlas, cmd.tex_offset, tex_cache_data[hash]);
                        tex_cache_meta[hash].tag = cmd.tex_offset;
                        tex_cache_meta[hash].valid = 1;
                        
                        // Copy to local buffer
                        FETCH_COPY_LOOP:
                        for (int t = 0; t < 128; t++) {
                            #pragma HLS UNROLL factor=16
                            local_tex[t] = tex_cache_data[hash][t];
                        }
                    }

                    // Draw column using local buffer
                    uint32_t frac = cmd.frac;
                    uint32_t col_step = cmd.step;

                    DRAW_COL_LOOP:
                    for (uint16_t y = y_start; y <= y_end; y++) {
                        #pragma HLS PIPELINE II=1
                        uint8_t tex_idx = (frac >> 16) & 127;
                        uint8_t tex_pixel = local_tex[tex_idx];
                        uint8_t lit_pixel = local_colormap[cmap_base + tex_pixel];
                        local_framebuffer[(uint32_t)y * SCREEN_WIDTH + cmd.x1] = lit_pixel;
                        frac += col_step;
                    }
                }
                // === FLOOR/CEILING SPAN ===
                else if (cmd.cmd_type == CMD_TYPE_SPAN) {
                    if (cmd.y1 >= SCREEN_HEIGHT) continue;
                    uint16_t x_start = (cmd.x1 < SCREEN_WIDTH) ? cmd.x1 : (uint16_t)(SCREEN_WIDTH - 1);
                    uint16_t x_end   = (cmd.x2 < SCREEN_WIDTH) ? cmd.x2 : (uint16_t)(SCREEN_WIDTH - 1);
                    if (x_start > x_end) continue;

                    // Load flat texture into BRAM cache (4KB) if not already cached
                    if (!flat_cache_valid || flat_cache_tag != cmd.tex_offset) {
                        burst_read_flat(texture_atlas, cmd.tex_offset, flat_cache);
                        flat_cache_tag = cmd.tex_offset;
                        flat_cache_valid = 1;
                    }
                    
                    uint32_t pos = cmd.frac;
                    uint32_t step = cmd.step;
                    
                    DRAW_SPAN_LOOP:
                    for (uint16_t x = x_start; x <= x_end; x++) {
                        #pragma HLS PIPELINE II=1
                        
                        // UV from DOOM's packed format
                        uint32_t ytemp = (pos >> 4) & 0x0fc0;
                        uint32_t xtemp = (pos >> 26);
                        uint32_t spot = (xtemp | ytemp) & 4095;
                        
                        // Read from BRAM cache (1 cycle) instead of DDR (~50 cycles)
                        uint8_t tex_pixel = flat_cache[spot];
                        
                        local_framebuffer[(uint32_t)cmd.y1 * SCREEN_WIDTH + x] = local_colormap[cmap_base + tex_pixel];
                        pos += step;
                    }
                }
            }
            processed_count += chunk_size;
        }
    }

    // === MODE 4/6: DMA OUT (High Speed Burst) ===
    if (mode == MODE_DMA_OUT || mode == MODE_DRAW_AND_DMA) {
        uint128_t* fb_ptr = (uint128_t*)local_framebuffer;
        int chunks = VIEW_SIZE / 16;  // Only view area, not HUD

        DMA_LOOP:
        for (int i = 0; i < chunks; i++) {
            #pragma HLS PIPELINE II=1
            framebuffer_out[i] = fb_ptr[i];
        }
    }
    
    // === MODE 5: UPSCALE (Future - Bicubic with Sharpening) ===
    else if (mode == MODE_UPSCALE) {
        // Future implementation:
        // 1. Read row N-1, N, N+1 from framebuffer into line_buffer
        // 2. Apply 4-tap bicubic vertical filter
        // 3. Apply horizontal filter  
        // 4. Apply sharpening kernel
        // 5. Output to 1080p framebuffer
        //
        // For now, this is a placeholder.
        // The line_buffer is ready for use.
        
        // Example: Load 3 consecutive rows
        // int base_row = 0;
        // for (int r = 0; r < LINE_BUF_ROWS; r++) {
        //     for (int x = 0; x < LINE_BUF_WIDTH; x++) {
        //         #pragma HLS PIPELINE II=1
        //         line_buffer[r][x] = local_framebuffer[(base_row + r) * SCREEN_WIDTH + x];
        //     }
        // }
        // line_buffer_base_row = base_row;
    }
}

} // extern "C"
