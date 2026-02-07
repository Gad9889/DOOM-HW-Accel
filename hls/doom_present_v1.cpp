// doom_present_v1.cpp - Stage 5 split present/upscale IP (quad-only fast path)
// Responsibilities:
// - Convert indexed 320x200 frame to 1600x1000 (XRGB8888 or RGB565)
// - Write via quad-lane outputs (FB0..FB3) only

#include <stdint.h>
#include <ap_int.h>

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 200
#define UPSCALE_FACTOR 5
#define OUT_WIDTH (SCREEN_WIDTH * UPSCALE_FACTOR)
#define OUT_WORDS_PER_ROW_8888 (OUT_WIDTH / 4) // 4x32bpp pixels / 128b word
#define OUT_WORDS_PER_ROW_565 (OUT_WIDTH / 8)  // 8x16bpp pixels / 128b word
#define X5_WORDS_PER_LANE_8888 (OUT_WORDS_PER_ROW_8888 / 4)
#define X5_WORDS_PER_LANE_565 (OUT_WORDS_PER_ROW_565 / 4)

#define MODE_IDLE 0
#define MODE_LOAD_COLORMAP 1
#define MODE_UPSCALE 5
#define MODE_PRESENT 7

#define PRESENT_FMT_XRGB8888 0
#define PRESENT_FMT_RGB565 1

typedef ap_uint<128> uint128_t;

extern "C" {

static uint8_t present_src_row[SCREEN_WIDTH];
static uint32_t present_src_rgba[SCREEN_WIDTH];
static uint32_t present_palette_rgba[256];
static uint16_t present_palette_rgb565[256];
static uint128_t present_row_words[OUT_WORDS_PER_ROW_8888];
static uint8_t present_palette_valid = 0;

static void load_present_palette(const uint8_t* colormap_ddr) {
    #pragma HLS INLINE off

    const int palette_offset = 32 * 256;
    const uint8_t* palette_rgb_ddr = colormap_ddr + palette_offset;

    PALETTE_LOAD_LOOP:
    for (int i = 0; i < 256; i++) {
        #pragma HLS PIPELINE II=1
        uint8_t r = palette_rgb_ddr[(i * 3) + 0];
        uint8_t g = palette_rgb_ddr[(i * 3) + 1];
        uint8_t b = palette_rgb_ddr[(i * 3) + 2];
        present_palette_rgba[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        present_palette_rgb565[i] = (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                                               ((uint16_t)(g & 0xFC) << 3) |
                                               ((uint16_t)b >> 3));
    }
}

void doom_present(
    uint128_t* framebuffer_out,
    uint64_t texture_atlas,
    const uint8_t* colormap_ddr,
    const uint128_t* command_buffer,
    uint32_t mode,
    uint32_t num_commands,
    uint32_t present_scale,
    uint32_t present_rows,
    uint128_t* framebuffer_out1,
    uint128_t* framebuffer_out2,
    uint128_t* framebuffer_out3,
    uint32_t present_lanes,
    uint32_t present_format,
    uint32_t present_stride_bytes
) {
    #pragma HLS INTERFACE m_axi port=framebuffer_out depth=800000 offset=slave bundle=FB max_write_burst_length=128
    #pragma HLS INTERFACE m_axi port=framebuffer_out1 depth=800000 offset=slave bundle=FB1 max_write_burst_length=128
    #pragma HLS INTERFACE m_axi port=framebuffer_out2 depth=800000 offset=slave bundle=FB2 max_write_burst_length=128
    #pragma HLS INTERFACE m_axi port=framebuffer_out3 depth=800000 offset=slave bundle=FB3 max_write_burst_length=128
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
    #pragma HLS INTERFACE s_axilite port=present_format bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=present_stride_bytes bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=return bundle=CTRL

    #pragma HLS BIND_STORAGE variable=present_src_row type=ram_1p impl=bram
    #pragma HLS BIND_STORAGE variable=present_src_rgba type=ram_2p impl=bram
    #pragma HLS BIND_STORAGE variable=present_palette_rgba type=ram_1p impl=bram
    #pragma HLS BIND_STORAGE variable=present_row_words type=ram_1p impl=bram
    #pragma HLS ARRAY_PARTITION variable=present_row_words dim=1 block factor=4

    (void)texture_atlas;
    (void)num_commands;

    if (mode == MODE_LOAD_COLORMAP) {
        load_present_palette(colormap_ddr);
        present_palette_valid = 1;
        return;
    }

    if (mode != MODE_UPSCALE && mode != MODE_PRESENT) {
        return;
    }

    int src_rows = (present_rows == 0 || present_rows > SCREEN_HEIGHT)
                       ? SCREEN_HEIGHT
                       : (int)present_rows;
    (void)present_scale;
    (void)present_lanes;

    int out_format = (present_format == PRESENT_FMT_RGB565) ? PRESENT_FMT_RGB565 : PRESENT_FMT_XRGB8888;
    int row_words = (out_format == PRESENT_FMT_RGB565) ? OUT_WORDS_PER_ROW_565 : OUT_WORDS_PER_ROW_8888;
    int lane_words = row_words / 4;
    int dst_stride_words;

    if (!present_palette_valid) {
        load_present_palette(colormap_ddr);
        present_palette_valid = 1;
    }

    if (present_stride_bytes == 0) {
        dst_stride_words = row_words;
    } else {
        dst_stride_words = (int)((present_stride_bytes + 15U) >> 4);
        if (dst_stride_words < row_words) {
            dst_stride_words = row_words;
        }
    }

    const int src_words_per_row = SCREEN_WIDTH / 16;

    PRESENT_ROWS_LOOP:
    for (int y = 0; y < src_rows; y++) {
        const uint128_t* src_words = command_buffer + (y * src_words_per_row);

        SRC_ROW_READ:
        for (int w = 0; w < src_words_per_row; w++) {
            #pragma HLS PIPELINE II=1
            uint128_t raw = src_words[w];

            SRC_ROW_UNPACK:
            for (int b = 0; b < 16; b++) {
                #pragma HLS UNROLL
                present_src_row[(w * 16) + b] = (uint8_t)raw.range((b * 8) + 7, b * 8);
            }
        }

        if (out_format == PRESENT_FMT_RGB565) {
            int q = 0;
            int r = 0;

            PRESENT_X5_PACK_565:
            for (int ow = 0; ow < OUT_WORDS_PER_ROW_565; ow++) {
                #pragma HLS PIPELINE II=1
                uint128_t packed = 0;

                PACK565_PIXELS:
                for (int p = 0; p < 8; p++) {
                    #pragma HLS UNROLL
                    int src_x = (q < SCREEN_WIDTH) ? q : (SCREEN_WIDTH - 1);
                    uint16_t c = present_palette_rgb565[present_src_row[src_x]];
                    packed.range((p * 16) + 15, p * 16) = c;

                    r++;
                    if (r >= 5) {
                        r = 0;
                        q++;
                    }
                }

                present_row_words[ow] = packed;
            }

            PRESENT_X5_ROWS_565:
            for (int vy = 0; vy < UPSCALE_FACTOR; vy++) {
                int dst_word_base = ((y * UPSCALE_FACTOR) + vy) * dst_stride_words;

                PRESENT_X5_WRITE_QUAD_565:
                for (int ow = 0; ow < X5_WORDS_PER_LANE_565; ow++) {
                    #pragma HLS PIPELINE II=1
                    framebuffer_out[dst_word_base + ow] = present_row_words[ow];
                    framebuffer_out1[dst_word_base + X5_WORDS_PER_LANE_565 + ow] = present_row_words[X5_WORDS_PER_LANE_565 + ow];
                    framebuffer_out2[dst_word_base + (2 * X5_WORDS_PER_LANE_565) + ow] = present_row_words[(2 * X5_WORDS_PER_LANE_565) + ow];
                    framebuffer_out3[dst_word_base + (3 * X5_WORDS_PER_LANE_565) + ow] = present_row_words[(3 * X5_WORDS_PER_LANE_565) + ow];
                }
            }
        } else {
            PRESENT_INDEX_TO_RGBA:
            for (int x = 0; x < SCREEN_WIDTH; x++) {
                #pragma HLS PIPELINE II=1
                present_src_rgba[x] = present_palette_rgba[present_src_row[x]];
            }

            int q = 0;
            int r = 0;

            PRESENT_X5_PACK_8888:
            for (int ow = 0; ow < OUT_WORDS_PER_ROW_8888; ow++) {
                #pragma HLS PIPELINE II=1
                uint32_t c0 = present_src_rgba[q];
                uint32_t c1 = present_src_rgba[(q < (SCREEN_WIDTH - 1)) ? (q + 1) : q];
                uint32_t p0 = c0;
                uint32_t p1 = c0;
                uint32_t p2 = c0;
                uint32_t p3 = c0;
                uint128_t packed = 0;

                if (r == 2) {
                    p3 = c1;
                } else if (r == 3) {
                    p2 = c1;
                    p3 = c1;
                } else if (r == 4) {
                    p1 = c1;
                    p2 = c1;
                    p3 = c1;
                }

                packed.range(31, 0) = p0;
                packed.range(63, 32) = p1;
                packed.range(95, 64) = p2;
                packed.range(127, 96) = p3;
                present_row_words[ow] = packed;

                r += 4;
                if (r >= 5) {
                    r -= 5;
                    q++;
                }
            }

            PRESENT_X5_ROWS_8888:
            for (int vy = 0; vy < UPSCALE_FACTOR; vy++) {
                int dst_word_base = ((y * UPSCALE_FACTOR) + vy) * dst_stride_words;

                PRESENT_X5_WRITE_QUAD_8888:
                for (int ow = 0; ow < X5_WORDS_PER_LANE_8888; ow++) {
                    #pragma HLS PIPELINE II=1
                    framebuffer_out[dst_word_base + ow] = present_row_words[ow];
                    framebuffer_out1[dst_word_base + X5_WORDS_PER_LANE_8888 + ow] = present_row_words[X5_WORDS_PER_LANE_8888 + ow];
                    framebuffer_out2[dst_word_base + (2 * X5_WORDS_PER_LANE_8888) + ow] = present_row_words[(2 * X5_WORDS_PER_LANE_8888) + ow];
                    framebuffer_out3[dst_word_base + (3 * X5_WORDS_PER_LANE_8888) + ow] = present_row_words[(3 * X5_WORDS_PER_LANE_8888) + ow];
                }
            }
        }
    }
}

} // extern "C"
