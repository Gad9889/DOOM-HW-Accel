// doom_raster_v1.cpp - Stage 5 split baseline raster IP
// Responsibilities:
// - Load colormap into BRAM
// - Clear local indexed framebuffer
// - Execute DOOM draw command batches (columns + spans)
// - DMA indexed view area (320x168) to DDR

#include <stdint.h>
#include <stddef.h>
#include <ap_int.h>

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 200
#define FB_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT)
#define VIEW_HEIGHT 168

#define BATCH_SIZE 64
#define TEX_CACHE_ENTRIES 256

#define MODE_IDLE 0
#define MODE_LOAD_COLORMAP 1
#define MODE_CLEAR_FB 2
#define MODE_DRAW_BATCH 3
#define MODE_DMA_OUT 4
#define MODE_UPSCALE 5
#define MODE_DRAW_AND_DMA 6
#define MODE_PRESENT 7
#define MODE_DRAW_AND_PRESENT 8

#define CMD_TYPE_COLUMN 0
#define CMD_TYPE_SPAN 1

typedef ap_uint<128> uint128_t;

struct __attribute__((packed)) DrawCommand {
    uint8_t cmd_type;
    uint8_t cmap_index;
    uint16_t x1;
    uint16_t x2;
    uint16_t y1;
    uint16_t y2;
    uint16_t reserved1;
    uint32_t frac;
    uint32_t step;
    uint32_t tex_offset;
    uint32_t reserved2;
    uint32_t reserved3;
};

typedef char drawcommand_size_must_be_32[(sizeof(DrawCommand) == 32) ? 1 : -1];
typedef char drawcommand_tex_offset_must_be_20[(offsetof(DrawCommand, tex_offset) == 20) ? 1 : -1];

struct TexCacheEntry {
    uint32_t tag;
    uint8_t valid;
    uint8_t age;
};

extern "C" {

static uint8_t local_framebuffer[FB_SIZE] __attribute__((aligned(16)));
static uint8_t local_colormap[32 * 256] __attribute__((aligned(16)));
static uint8_t tex_cache_data[TEX_CACHE_ENTRIES][128];
static TexCacheEntry tex_cache_meta[TEX_CACHE_ENTRIES];
static uint8_t flat_cache[4096];
static uint32_t flat_cache_tag;
static uint8_t flat_cache_valid;

static inline uint32_t tex_cache_hash(uint32_t tex_offset) {
    return (tex_offset >> 7) & (TEX_CACHE_ENTRIES - 1);
}

static void burst_read_colormap(const uint8_t* colormap_ddr, uint8_t* local_cmap) {
    #pragma HLS INLINE off

    const uint128_t* src = (const uint128_t*)colormap_ddr;
    uint128_t* dst = (uint128_t*)local_cmap;

    CMAP_READ_LOOP:
    for (int i = 0; i < 512; i++) {
        #pragma HLS PIPELINE II=1
        dst[i] = src[i];
    }
}

static void burst_read_commands(const uint128_t* cmd_words, DrawCommand* batch, int count) {
    #pragma HLS INLINE off

    CMD_READ_LOOP:
    for (int i = 0; i < count; i++) {
        #pragma HLS PIPELINE II=1
        uint128_t w0 = cmd_words[(i << 1) + 0];
        uint128_t w1 = cmd_words[(i << 1) + 1];

        DrawCommand cmd;
        cmd.cmd_type = (uint8_t)w0.range(7, 0);
        cmd.cmap_index = (uint8_t)w0.range(15, 8);
        cmd.x1 = (uint16_t)w0.range(31, 16);
        cmd.x2 = (uint16_t)w0.range(47, 32);
        cmd.y1 = (uint16_t)w0.range(63, 48);
        cmd.y2 = (uint16_t)w0.range(79, 64);
        cmd.reserved1 = (uint16_t)w0.range(95, 80);
        cmd.frac = (uint32_t)w0.range(127, 96);
        cmd.step = (uint32_t)w1.range(31, 0);
        cmd.tex_offset = (uint32_t)w1.range(63, 32);
        cmd.reserved2 = (uint32_t)w1.range(95, 64);
        cmd.reserved3 = (uint32_t)w1.range(127, 96);

        batch[i] = cmd;
    }
}

static void fetch_texture_to_cache(const uint128_t* texture_atlas, uint32_t tex_offset, uint8_t* cache_slot) {
    #pragma HLS INLINE off

    uint32_t word_idx = tex_offset / 16;

    TEX_READ_LOOP:
    for (int t = 0; t < 8; t++) {
        #pragma HLS PIPELINE II=1
        uint128_t raw = texture_atlas[word_idx + t];

        TEX_UNPACK_LOOP:
        for (int b = 0; b < 16; b++) {
            #pragma HLS UNROLL
            cache_slot[t * 16 + b] = (uint8_t)raw.range((b * 8) + 7, b * 8);
        }
    }
}

static void burst_read_flat(const uint128_t* texture_atlas, uint32_t tex_offset, uint8_t* flat_buf) {
    #pragma HLS INLINE off

    uint32_t word_idx = tex_offset / 16;

    FLAT_READ_LOOP:
    for (int i = 0; i < 256; i++) {
        #pragma HLS PIPELINE II=1
        uint128_t raw = texture_atlas[word_idx + i];

        FLAT_UNPACK_LOOP:
        for (int b = 0; b < 16; b++) {
            #pragma HLS UNROLL
            flat_buf[i * 16 + b] = (uint8_t)raw.range((b * 8) + 7, b * 8);
        }
    }
}

void doom_raster(
    uint128_t* framebuffer_out,
    const uint128_t* texture_atlas,
    const uint8_t* colormap_ddr,
    const uint128_t* command_buffer,
    uint32_t mode,
    uint32_t num_commands,
    uint32_t present_scale,
    uint32_t present_rows,
    uint64_t framebuffer_out1,
    uint64_t framebuffer_out2,
    uint64_t framebuffer_out3,
    uint32_t present_lanes
) {
    #pragma HLS INTERFACE m_axi port=framebuffer_out depth=400000 offset=slave bundle=FB max_write_burst_length=128
    #pragma HLS INTERFACE m_axi port=texture_atlas depth=1048576 offset=slave bundle=TEX max_read_burst_length=64
    #pragma HLS INTERFACE m_axi port=colormap_ddr depth=8960 offset=slave bundle=CMAP max_read_burst_length=64
    #pragma HLS INTERFACE m_axi port=command_buffer depth=8000 offset=slave bundle=CMD max_read_burst_length=64

    #pragma HLS INTERFACE s_axilite port=framebuffer_out bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=framebuffer_out1 bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=framebuffer_out2 bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=framebuffer_out3 bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=texture_atlas bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=colormap_ddr bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=command_buffer bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=mode bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=num_commands bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=present_scale bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=present_rows bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=present_lanes bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=return bundle=CTRL

    #pragma HLS BIND_STORAGE variable=local_framebuffer type=ram_2p impl=bram
    #pragma HLS BIND_STORAGE variable=local_colormap type=ram_2p impl=bram
    #pragma HLS BIND_STORAGE variable=tex_cache_data type=ram_2p impl=bram
    #pragma HLS BIND_STORAGE variable=tex_cache_meta type=ram_1p impl=lutram
    #pragma HLS BIND_STORAGE variable=flat_cache type=ram_2p impl=bram

    (void)present_scale;
    (void)framebuffer_out1;
    (void)framebuffer_out2;
    (void)framebuffer_out3;
    (void)present_lanes;

    if (mode == MODE_LOAD_COLORMAP) {
        burst_read_colormap(colormap_ddr, local_colormap);

        CACHE_INVALIDATE:
        for (int i = 0; i < TEX_CACHE_ENTRIES; i++) {
            #pragma HLS UNROLL factor=16
            tex_cache_meta[i].valid = 0;
        }
        flat_cache_valid = 0;
    }
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
    else if (mode == MODE_DRAW_BATCH || mode == MODE_DRAW_AND_DMA) {
        DrawCommand batch[BATCH_SIZE];
        uint32_t processed_count = 0;

        while (processed_count < num_commands) {
            uint32_t chunk_size = num_commands - processed_count;
            if (chunk_size > BATCH_SIZE) {
                chunk_size = BATCH_SIZE;
            }

            burst_read_commands(command_buffer + ((uint32_t)processed_count << 1), batch, (int)chunk_size);

            BATCH_LOOP:
            for (uint32_t i = 0; i < chunk_size; i++) {
                #pragma HLS PIPELINE off

                DrawCommand cmd = batch[i];
                uint32_t cmap_base = (uint32_t)cmd.cmap_index << 8;

                if (cmd.cmd_type == CMD_TYPE_COLUMN) {
                    if (cmd.x1 >= SCREEN_WIDTH) {
                        continue;
                    }
                    uint16_t y_start = (cmd.y1 < SCREEN_HEIGHT) ? cmd.y1 : (uint16_t)(SCREEN_HEIGHT - 1);
                    uint16_t y_end = (cmd.y2 < SCREEN_HEIGHT) ? cmd.y2 : (uint16_t)(SCREEN_HEIGHT - 1);
                    if (y_start > y_end) {
                        continue;
                    }

                    uint8_t local_tex[128];
                    #pragma HLS ARRAY_PARTITION variable=local_tex dim=1 factor=16 cyclic

                    uint32_t hash = tex_cache_hash(cmd.tex_offset);
                    bool cache_hit = (tex_cache_meta[hash].valid && tex_cache_meta[hash].tag == cmd.tex_offset);

                    if (cache_hit) {
                        CACHE_COPY_LOOP:
                        for (int t = 0; t < 128; t++) {
                            #pragma HLS UNROLL factor=16
                            local_tex[t] = tex_cache_data[hash][t];
                        }
                    } else {
                        fetch_texture_to_cache(texture_atlas, cmd.tex_offset, tex_cache_data[hash]);
                        tex_cache_meta[hash].tag = cmd.tex_offset;
                        tex_cache_meta[hash].valid = 1;

                        FETCH_COPY_LOOP:
                        for (int t = 0; t < 128; t++) {
                            #pragma HLS UNROLL factor=16
                            local_tex[t] = tex_cache_data[hash][t];
                        }
                    }

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
                else if (cmd.cmd_type == CMD_TYPE_SPAN) {
                    if (cmd.y1 >= SCREEN_HEIGHT) {
                        continue;
                    }
                    uint16_t x_start = (cmd.x1 < SCREEN_WIDTH) ? cmd.x1 : (uint16_t)(SCREEN_WIDTH - 1);
                    uint16_t x_end = (cmd.x2 < SCREEN_WIDTH) ? cmd.x2 : (uint16_t)(SCREEN_WIDTH - 1);
                    if (x_start > x_end) {
                        continue;
                    }

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
                        uint32_t ytemp = (pos >> 4) & 0x0fc0;
                        uint32_t xtemp = (pos >> 26);
                        uint32_t spot = (xtemp | ytemp) & 4095;
                        uint8_t tex_pixel = flat_cache[spot];

                        local_framebuffer[(uint32_t)cmd.y1 * SCREEN_WIDTH + x] = local_colormap[cmap_base + tex_pixel];
                        pos += step;
                    }
                }
            }

            processed_count += chunk_size;
        }
    }

    if (mode == MODE_DMA_OUT || mode == MODE_DRAW_AND_DMA) {
        int dma_rows = VIEW_HEIGHT;
        if (present_rows > 0) {
            dma_rows = (present_rows > SCREEN_HEIGHT) ? SCREEN_HEIGHT : (int)present_rows;
        }
        uint128_t* fb_ptr = (uint128_t*)local_framebuffer;
        int chunks = (SCREEN_WIDTH * dma_rows) / 16;

        DMA_LOOP:
        for (int i = 0; i < chunks; i++) {
            #pragma HLS PIPELINE II=1
            framebuffer_out[i] = fb_ptr[i];
        }
    }
}

} // extern "C"
