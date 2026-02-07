#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h> // For TCP_NODELAY
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#include "doomgeneric.h"
#include "doom_accel.h"
#include "i_video.h"
#include "d_loop.h"

// Configuration
#define LISTEN_PORT 5000
#define STREAM_HELLO_MAGIC "DGv1"
#define STREAM_HELLO_SIZE 9

// Metrics
uint64_t GetTimeNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

uint64_t perf_send_ns = 0;
uint64_t perf_tick_ns = 0;
int perf_frames = 0;
uint64_t perf_last_report = 0;

// Input Queue
#define KEY_QUEUE_SIZE 64
typedef struct
{
    uint8_t key;
    uint8_t pressed;
} key_event_t;

key_event_t key_queue[KEY_QUEUE_SIZE];
int key_head = 0;
int key_tail = 0;

// Globals
int server_fd = -1;
int client_fd = -1;
static int bench_skip_present_no_client = 0;
static int bench_force_sw = 0;
static int bench_force_hw = 0;
static int bench_skip_client_wait = 0;
static int bench_native_320 = 1;
static int bench_pl_scale = 0;
static int stream_width = 320;
static int stream_height = 200;

typedef enum
{
    OUTPUT_TCP = 0,
    OUTPUT_SCREEN = 1,
    OUTPUT_HEADLESS = 2
} output_mode_t;

static output_mode_t output_mode = OUTPUT_TCP;
static int fb_fd = -1;
static uint8_t *fb_ptr = NULL;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_stride = 0;
static uint32_t fb_bytes_per_pixel = 0;
static uint32_t fb_scanout_xoffset = 0;
static uint32_t fb_scanout_yoffset = 0;
static uint32_t fb_base_offset = 0;
static uint32_t fb_offset_x = 0;
static uint32_t fb_offset_y = 0;
static uint32_t fb_copy_width = 0;
static uint32_t fb_copy_height = 0;
static size_t fb_map_size = 0;

static int arg_eq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

static void print_usage(const char *exe)
{
    printf("Usage: %s [DOOM args] [bench flags]\n", exe);
    printf("\n");
    printf("Bench/runtime flags (canonical):\n");
    printf("  -tcp-screen       Enable TCP viewer mode (default)\n");
    printf("  -screen           Present to local /dev/fb0 (mini-DP)\n");
    printf("  -headless         Disable TCP + skip present (pure headless)\n");
    printf("  -bench-sw         Force software render path\n");
    printf("  -bench-hw         Force hardware render path\n");
    printf("  -no-client        Do not open TCP server / wait for viewer\n");
    printf("  -bench-headless   Skip present when no client is connected\n");
    printf("  -pl-scale         Enable PL fullres upscale/present path\n");
    printf("  -native320        Stream/output mode 320x200\n");
    printf("  -fullres          Stream/output mode 1600x1000\n");
    printf("  -help, --help     Show this message\n");
    printf("\n");
    printf("Common DOOM args:\n");
    printf("  -iwad <file> -timedemo <demo> -scaling <n> -async-present\n");
}

static const char *output_mode_name(void)
{
    if (output_mode == OUTPUT_SCREEN)
        return "screen";
    if (output_mode == OUTPUT_HEADLESS)
        return "headless";
    return "tcp";
}

static int init_screen_output(void)
{
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    int ret;

    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0)
    {
        perror("open /dev/fb0 failed");
        return -1;
    }

    ret = ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    if (ret != 0)
    {
        perror("FBIOGET_VSCREENINFO failed");
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    ret = ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo);
    if (ret != 0)
    {
        perror("FBIOGET_FSCREENINFO failed");
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    fb_width = vinfo.xres;
    fb_height = vinfo.yres;
    fb_bytes_per_pixel = vinfo.bits_per_pixel / 8;
    fb_scanout_xoffset = vinfo.xoffset;
    fb_scanout_yoffset = vinfo.yoffset;
    fb_stride = finfo.line_length;
    fb_map_size = (size_t)fb_stride * (size_t)vinfo.yres_virtual;
    fb_base_offset = (fb_scanout_yoffset * fb_stride) + (fb_scanout_xoffset * fb_bytes_per_pixel);

    if (fb_bytes_per_pixel != 4 && fb_bytes_per_pixel != 2)
    {
        printf("ERR: /dev/fb0 is %u bpp; only 16/32 bpp are supported in -screen mode\n",
               vinfo.bits_per_pixel);
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    fb_ptr = (uint8_t *)mmap(NULL, fb_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_ptr == MAP_FAILED)
    {
        fb_ptr = NULL;
        perror("mmap /dev/fb0 failed");
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    fb_offset_x = (fb_width > (uint32_t)stream_width)
                      ? (uint32_t)(((fb_width - (uint32_t)stream_width) / 2) * fb_bytes_per_pixel)
                      : 0;
    fb_offset_y = (fb_height > (uint32_t)stream_height)
                      ? (uint32_t)(((fb_height - (uint32_t)stream_height) / 2) * fb_stride)
                      : 0;
    fb_copy_width = ((uint32_t)stream_width < fb_width) ? (uint32_t)stream_width : fb_width;
    fb_copy_height = ((uint32_t)stream_height < fb_height) ? (uint32_t)stream_height : fb_height;

    printf("SCREEN: /dev/fb0 vis=%ux%u virt=%ux%u bpp=%u stride=%u\n",
           fb_width, fb_height, vinfo.xres_virtual, vinfo.yres_virtual, vinfo.bits_per_pixel, fb_stride);
    printf("SCREEN: scanout offset x=%u y=%u (base+%u bytes)\n",
           fb_scanout_xoffset, fb_scanout_yoffset, fb_base_offset);
    printf("SCREEN: centered %dx%d\n", stream_width, stream_height);
    printf("SCREEN: copy area %ux%u\n", fb_copy_width, fb_copy_height);
    if (fb_copy_width != (uint32_t)stream_width || fb_copy_height != (uint32_t)stream_height)
    {
        printf("SCREEN: WARNING stream larger than framebuffer, clipping output\n");
    }
    if (fb_copy_width > 0 && fb_copy_height > 0)
    {
        size_t max_write = (size_t)fb_base_offset +
                           (size_t)fb_offset_y +
                           ((size_t)(fb_copy_height - 1) * (size_t)fb_stride) +
                           (size_t)fb_offset_x +
                           ((size_t)fb_copy_width * (size_t)fb_bytes_per_pixel);
        if (max_write > fb_map_size)
        {
            printf("ERR: computed screen write window exceeds mapped fb0 range\n");
            close(fb_fd);
            fb_fd = -1;
            return -1;
        }
    }
    return 0;
}

static int send_all_blocking(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len)
    {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)n;
    }

    return 0;
}

int DG_ShouldPresent(void)
{
    if (output_mode == OUTPUT_HEADLESS)
        return 0;

    if (output_mode == OUTPUT_SCREEN)
        return !bench_skip_present_no_client;

    if (!bench_skip_present_no_client)
        return 1;

    return client_fd >= 0;
}

int DG_UseNative320(void)
{
    return bench_native_320;
}

void DG_Init()
{
    struct sockaddr_in address;
    int opt = 1;
    int addrlen;
    int flag = 1;
    int flags;

    if (output_mode == OUTPUT_HEADLESS)
    {
        printf("BENCH: headless mode, no TCP and no present\n");
        server_fd = -1;
        client_fd = -1;
        return;
    }

    if (output_mode == OUTPUT_SCREEN)
    {
        printf("SCREEN: initializing local framebuffer output on /dev/fb0...\n");
        if (init_screen_output() != 0)
        {
            fprintf(stderr, "ERR: -screen mode requested, but /dev/fb0 init failed. Exiting.\n");
            exit(EXIT_FAILURE);
        }
        printf("SCREEN: ready. Output is going to mini-DP via /dev/fb0\n");
        server_fd = -1;
        client_fd = -1;
        return;
    }

    if (bench_skip_client_wait)
    {
        printf("BENCH: skipping client wait (-no-client), networking disabled.\n");
        server_fd = -1;
        client_fd = -1;
        return;
    }

    // Create socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0)
    {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // Force attach socket to the port
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(LISTEN_PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 1) < 0)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    printf("TCP Server Initialized. Waiting for viewer to connect on port %d...\n", LISTEN_PORT);

    // Blocking accept for simplicity (Game won't start until viewer connects)
    addrlen = sizeof(address);
    if ((client_fd = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen)) < 0)
    {
        perror("accept");
        exit(EXIT_FAILURE);
    }

    // Disable Nagle's Algorithm for low latency
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

    // Send stream hello so client can auto-configure resolution.
    {
        uint8_t hello[STREAM_HELLO_SIZE];
        uint16_t w_be = htons((uint16_t)stream_width);
        uint16_t h_be = htons((uint16_t)stream_height);

        memcpy(hello, STREAM_HELLO_MAGIC, 4);
        memcpy(hello + 4, &w_be, sizeof(w_be));
        memcpy(hello + 6, &h_be, sizeof(h_be));
        hello[8] = 24; // 24-bit BGR payload

        if (send_all_blocking(client_fd, hello, sizeof(hello)) != 0)
        {
            perror("send hello failed");
            close(client_fd);
            client_fd = -1;
            return;
        }
    }

    // Set non-blocking for input handling
    flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    printf("Viewer Connected! Starting Doom...\n");
    printf("Stream hello: %dx%d @ 24bpp BGR\n", stream_width, stream_height);
}

void DG_DrawFrame()
{
    size_t total_pixels;
    size_t total_bytes_out;
    static uint8_t *pack_buffer = NULL;
    static size_t pack_capacity = 0;
    uint32_t *src;
    uint8_t *dst;
    size_t i;
    size_t sent = 0;
    uint64_t start, end;

    if (output_mode == OUTPUT_HEADLESS)
        return;

    if (output_mode == OUTPUT_SCREEN)
    {
        uint64_t start_local = GetTimeNs();
        const uint8_t *src = (const uint8_t *)DG_ScreenBuffer;
        uint8_t *dst = fb_ptr;
        int y;

        if (!fb_ptr)
            return;

        if (fb_bytes_per_pixel == 4)
        {
            uint32_t row_bytes = fb_copy_width * 4;
            for (y = 0; y < (int)fb_copy_height; y++)
            {
                memcpy(dst + fb_base_offset + fb_offset_y + ((uint32_t)y * fb_stride) + fb_offset_x,
                       src + ((uint32_t)y * (uint32_t)stream_width * 4),
                       row_bytes);
            }
        }
        else if (fb_bytes_per_pixel == 2)
        {
            const uint32_t *src32 = (const uint32_t *)DG_ScreenBuffer;
            for (y = 0; y < (int)fb_copy_height; y++)
            {
                const uint32_t *src_row = src32 + ((uint32_t)y * (uint32_t)stream_width);
                uint16_t *dst_row = (uint16_t *)(dst + fb_base_offset + fb_offset_y + ((uint32_t)y * fb_stride) + fb_offset_x);
                int x;

                for (x = 0; x < (int)fb_copy_width; x++)
                {
                    uint32_t p = src_row[x];
                    uint8_t b = (uint8_t)(p & 0xFF);
                    uint8_t g = (uint8_t)((p >> 8) & 0xFF);
                    uint8_t r = (uint8_t)((p >> 16) & 0xFF);
                    dst_row[x] = (uint16_t)(((r & 0xF8) << 8) |
                                            ((g & 0xFC) << 3) |
                                            ((b & 0xF8) >> 3));
                }
            }
        }

        perf_send_ns += (GetTimeNs() - start_local);
        return;
    }

    if (client_fd < 0)
        return;

    start = GetTimeNs();

    // Revert to 24-bit BGR Packing
    // This reduces bandwidth (higher FPS on 100Mbit) and fixes colors.
    total_pixels = (size_t)stream_width * (size_t)stream_height;
    total_bytes_out = total_pixels * 3;

    if (!pack_buffer || pack_capacity < total_bytes_out)
    {
        uint8_t *new_buffer = realloc(pack_buffer, total_bytes_out);
        if (!new_buffer)
        {
            return;
        }
        pack_buffer = new_buffer;
        pack_capacity = total_bytes_out;
    }

    src = (uint32_t *)DG_ScreenBuffer;
    dst = pack_buffer;

    for (i = 0; i < total_pixels; i++)
    {
        uint32_t pixel = src[i];
        *dst++ = (pixel) & 0xFF;       // Blue
        *dst++ = (pixel >> 8) & 0xFF;  // Green
        *dst++ = (pixel >> 16) & 0xFF; // Red
    }

    // Send entire frame
    while (sent < total_bytes_out)
    {
        ssize_t val = send(client_fd, pack_buffer + sent, total_bytes_out - sent, 0);
        if (val < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                continue;
            }
            printf("Client disconnected.\n");
            close(client_fd);
            client_fd = -1;
            return;
        }
        sent += val;
    }

    end = GetTimeNs();
    perf_send_ns += (end - start);
}

void DG_SleepMs(uint32_t ms)
{
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&req, NULL);
}

uint32_t DG_GetTicksMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    if (client_fd >= 0)
    {
        uint8_t buf[2];
        // Read input from TCP stream
        while (1)
        {
            ssize_t len = recv(client_fd, buf, 2, 0);
            if (len == 2)
            {
                int next_head = (key_head + 1) % KEY_QUEUE_SIZE;
                if (next_head != key_tail)
                {
                    key_queue[key_head].key = buf[0];
                    key_queue[key_head].pressed = buf[1];
                    key_head = next_head;
                }
            }
            else
            {
                break;
            }
        }
    }

    if (key_head != key_tail)
    {
        *key = key_queue[key_tail].key;
        *pressed = key_queue[key_tail].pressed;
        key_tail = (key_tail + 1) % KEY_QUEUE_SIZE;
        return 1;
    }

    return 0;
}

void DG_SetWindowTitle(const char *title)
{
}

int main(int argc, char **argv)
{
    int perf_last_gametic = 0;
    int stream_mode_explicit = 0;
    int requested_scaling = 0;

    for (int i = 1; i < argc; i++)
    {
        if (arg_eq(argv[i], "-help") || arg_eq(argv[i], "--help"))
        {
            print_usage(argv[0]);
            return 0;
        }
        else if (arg_eq(argv[i], "-tcp-screen"))
        {
            output_mode = OUTPUT_TCP;
            bench_skip_client_wait = 0;
            bench_skip_present_no_client = 0;
        }
        else if (arg_eq(argv[i], "-screen") || arg_eq(argv[i], "-mini-dp") || arg_eq(argv[i], "-minidp"))
        {
            output_mode = OUTPUT_SCREEN;
            bench_skip_client_wait = 1;
            bench_skip_present_no_client = 0;
        }
        else if (arg_eq(argv[i], "-headless"))
        {
            output_mode = OUTPUT_HEADLESS;
            bench_skip_client_wait = 1;
            bench_skip_present_no_client = 1;
        }
        else if (arg_eq(argv[i], "-bench-headless"))
        {
            bench_skip_present_no_client = 1;
        }
        else if (arg_eq(argv[i], "-no-client"))
        {
            bench_skip_client_wait = 1;
        }
        else if (arg_eq(argv[i], "-bench-sw"))
        {
            bench_force_sw = 1;
        }
        else if (arg_eq(argv[i], "-bench-hw"))
        {
            bench_force_hw = 1;
        }
        else if (arg_eq(argv[i], "-pl-scale"))
        {
            bench_pl_scale = 1;
        }
        else if (arg_eq(argv[i], "-native320"))
        {
            bench_native_320 = 1;
            stream_mode_explicit = 1;
        }
        else if (arg_eq(argv[i], "-fullres"))
        {
            bench_native_320 = 0;
            stream_mode_explicit = 1;
        }
        else if (arg_eq(argv[i], "-scaling") && (i + 1) < argc)
        {
            requested_scaling = atoi(argv[i + 1]);
            i++;
        }
    }

    if (!stream_mode_explicit && bench_pl_scale)
    {
        bench_native_320 = 0;
        printf("BENCH: auto-selecting fullres because PL upscale was requested\n");
    }
    else if (!stream_mode_explicit && requested_scaling > 1)
    {
        bench_native_320 = 0;
        printf("BENCH: auto-selecting fullres because -scaling %d was requested\n", requested_scaling);
    }

    if (bench_native_320 && requested_scaling > 1)
    {
        printf("NOTE: -native320 forces scaling=1; use -fullres to benchmark -scaling %d\n",
               requested_scaling);
    }

    if (requested_scaling <= 0)
    {
        requested_scaling = 1;
    }

    stream_width = bench_native_320 ? 320 : DOOMGENERIC_RESX;
    stream_height = bench_native_320 ? 200 : DOOMGENERIC_RESY;

    Init_Doom_Accel();

    if (bench_force_sw)
    {
        debug_sw_fallback = 1;
        printf("BENCH: forcing software rendering path (debug_sw_fallback=1)\n");
    }
    if (bench_force_hw)
    {
        debug_sw_fallback = 0;
        printf("BENCH: forcing hardware rendering path (debug_sw_fallback=0)\n");
    }
    if (bench_native_320 && bench_pl_scale)
    {
        printf("NOTE: PL upscale requested but stream mode is native320, disabling PL upscale\n");
        bench_pl_scale = 0;
    }
    HW_SetPresentLanes(4);
    HW_SetPLUpscaleEnabled(bench_pl_scale);
    if (bench_pl_scale)
    {
        printf("BENCH: PL fullres upscale enabled\n");
    }
    printf("BENCH: PL output lanes (quad-only fast path): %d\n", HW_GetPresentLanes());
    if (bench_skip_present_no_client)
    {
        printf("BENCH: no-client present disabled (-bench-headless)\n");
    }
    if (bench_skip_client_wait)
    {
        printf("BENCH: startup without TCP client (-no-client)\n");
    }
    printf("BENCH: output mode %s\n", output_mode_name());
    printf("BENCH: render mode %s (raster_regs=%p, present_regs=%p, fallback=%d)\n",
           (accel_regs && !debug_sw_fallback) ? "HW" : "SW",
           (void *)accel_regs, (void *)present_regs, debug_sw_fallback);
    printf("BENCH: stream mode %s (%dx%d)\n",
           bench_native_320 ? "native320" : "fullres",
           stream_width,
           stream_height);

    doomgeneric_Create(argc, argv);

    perf_last_report = GetTimeNs();

    while (1)
    {
        uint64_t start = GetTimeNs();
        doomgeneric_Tick();
        uint64_t end = GetTimeNs();

        perf_tick_ns += (end - start);
        perf_frames++;

        if (end - perf_last_report >= 1000000000ULL)
        {
            double fps = (double)perf_frames;
            HWPerfStats hw_stats;
            uint64_t scale_ns = 0;
            memset(&hw_stats, 0, sizeof(hw_stats));
            HW_GetAndResetPerfStats(&hw_stats);
            I_GetAndResetScalePerfNs(&scale_ns);
            // Avoid division by zero
            if (perf_frames > 0)
            {
                double cmds_per_frame;
                double fpga_wait_ms_per_frame;
                double tex_hit_rate;
                double avg_scale;
                double avg_game_hw;
                int tics_per_sec;
                double avg_tick = (double)perf_tick_ns / perf_frames / 1000000.0; // ms
                double avg_send = (double)perf_send_ns / perf_frames / 1000000.0; // ms
                double avg_render = avg_tick - avg_send;
                uint32_t total_cmds = hw_stats.queued_columns + hw_stats.queued_spans;

                cmds_per_frame = (double)total_cmds / perf_frames;
                fpga_wait_ms_per_frame = (double)hw_stats.fpga_wait_ns / perf_frames / 1000000.0;
                avg_scale = (double)scale_ns / perf_frames / 1000000.0;
                avg_game_hw = avg_render - avg_scale;
                if (avg_game_hw < 0.0)
                    avg_game_hw = 0.0;
                tics_per_sec = gametic - perf_last_gametic;
                perf_last_gametic = gametic;
                tex_hit_rate = (hw_stats.tex_cache_lookups > 0)
                                   ? (100.0 * (double)hw_stats.tex_cache_hits / (double)hw_stats.tex_cache_lookups)
                                   : 0.0;

                printf("FPS: %.1f | Frame: %.2f ms | Render: %.2f ms | Game+HW: %.2f ms | Scale: %.2f ms | Send: %.2f ms | Tics: %d/s | Sx:%d\n",
                       fps, avg_tick, avg_render, avg_game_hw, avg_scale, avg_send, tics_per_sec, fb_scaling);
                printf("HW: cmds/frame %.0f (col=%u span=%u) | flush=%u mid=%u max=%u | tex hit=%.1f%% miss=%u upload=%.1f KB wraps=%u entries=%u failins=%u | wait=%.2f ms/frame\n",
                       cmds_per_frame,
                       hw_stats.queued_columns, hw_stats.queued_spans,
                       hw_stats.flush_calls, hw_stats.mid_frame_flushes, hw_stats.max_cmds_seen,
                       tex_hit_rate, hw_stats.tex_cache_misses,
                       (double)hw_stats.tex_upload_bytes / 1024.0,
                       hw_stats.tex_atlas_wraps, hw_stats.tex_cache_entries, hw_stats.tex_cache_failed_inserts,
                       fpga_wait_ms_per_frame);
                if (!debug_sw_fallback && hw_stats.flush_calls == 0)
                {
                    printf("NOTE: HW mode active but no 3D HW commands this interval (not in level/gameplay path).\n");
                }
            }

            perf_frames = 0;
            perf_tick_ns = 0;
            perf_send_ns = 0;
            perf_last_report = end;
        }
    }

    return 0;
}
