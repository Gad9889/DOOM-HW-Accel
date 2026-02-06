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

#include "doomgeneric.h"
#include "doom_accel.h"
#include "i_video.h"
#include "d_loop.h"

// Configuration
#define LISTEN_PORT 5000

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

static int arg_eq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

int DG_ShouldPresent(void)
{
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

    // Set non-blocking for input handling
    flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    printf("Viewer Connected! Starting Doom...\n");
}

void DG_DrawFrame()
{
    int stream_w = bench_native_320 ? 320 : DOOMGENERIC_RESX;
    int stream_h = bench_native_320 ? 200 : DOOMGENERIC_RESY;
    size_t total_pixels;
    size_t total_bytes_out;
    static uint8_t *pack_buffer = NULL;
    uint32_t *src;
    uint8_t *dst;
    size_t i;
    size_t sent = 0;
    uint64_t start, end;

    if (client_fd < 0)
        return;

    start = GetTimeNs();

    // Revert to 24-bit BGR Packing
    // This reduces bandwidth (higher FPS on 100Mbit) and fixes colors.
    total_pixels = (size_t)stream_w * (size_t)stream_h;
    total_bytes_out = total_pixels * 3;

    if (!pack_buffer)
    {
        pack_buffer = malloc(total_bytes_out);
        if (!pack_buffer)
            return;
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

    for (int i = 1; i < argc; i++)
    {
        if (arg_eq(argv[i], "-bench-headless") || arg_eq(argv[i], "-bench_headless") ||
            arg_eq(argv[i], "-bench-nopresent") || arg_eq(argv[i], "-bench_nopresent"))
        {
            bench_skip_present_no_client = 1;
        }
        else if (arg_eq(argv[i], "-no-client") || arg_eq(argv[i], "-no_client") ||
                 arg_eq(argv[i], "-bench-no-client") || arg_eq(argv[i], "-bench_noclient"))
        {
            bench_skip_client_wait = 1;
        }
        else if (arg_eq(argv[i], "-bench-sw") || arg_eq(argv[i], "-bench_sw"))
        {
            bench_force_sw = 1;
        }
        else if (arg_eq(argv[i], "-bench-hw") || arg_eq(argv[i], "-bench_hw"))
        {
            bench_force_hw = 1;
        }
        else if (arg_eq(argv[i], "-native320") || arg_eq(argv[i], "-native_320"))
        {
            bench_native_320 = 1;
        }
        else if (arg_eq(argv[i], "-fullres") || arg_eq(argv[i], "-full_res"))
        {
            bench_native_320 = 0;
        }
    }

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
    if (bench_skip_present_no_client)
    {
        printf("BENCH: no-client present disabled (-bench-headless)\n");
    }
    if (bench_skip_client_wait)
    {
        printf("BENCH: startup without TCP client (-no-client)\n");
    }
    printf("BENCH: render mode %s (accel_regs=%p, fallback=%d)\n",
           (accel_regs && !debug_sw_fallback) ? "HW" : "SW",
           (void *)accel_regs, debug_sw_fallback);
    printf("BENCH: stream mode %s (%dx%d)\n",
           bench_native_320 ? "native320" : "fullres",
           bench_native_320 ? 320 : DOOMGENERIC_RESX,
           bench_native_320 ? 200 : DOOMGENERIC_RESY);

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
                double fpga_wait_idle_ms_per_frame;
                double fpga_wait_done_ms_per_frame;
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
                fpga_wait_idle_ms_per_frame = (double)hw_stats.fpga_wait_idle_ns / perf_frames / 1000000.0;
                fpga_wait_done_ms_per_frame = (double)hw_stats.fpga_wait_done_ns / perf_frames / 1000000.0;
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
                printf("HW: cmds/frame %.0f (col=%u span=%u) | flush=%u mid=%u max=%u | tex hit=%.1f%% miss=%u upload=%.1f KB wraps=%u entries=%u failins=%u | cmd up=%.1f KB | wait=%.2f ms/frame (idle=%.2f done=%.2f)\n",
                       cmds_per_frame,
                       hw_stats.queued_columns, hw_stats.queued_spans,
                       hw_stats.flush_calls, hw_stats.mid_frame_flushes, hw_stats.max_cmds_seen,
                       tex_hit_rate, hw_stats.tex_cache_misses,
                       (double)hw_stats.tex_upload_bytes / 1024.0,
                       hw_stats.tex_atlas_wraps, hw_stats.tex_cache_entries, hw_stats.tex_cache_failed_inserts,
                       (double)hw_stats.cmd_upload_bytes / 1024.0,
                       fpga_wait_ms_per_frame,
                       fpga_wait_idle_ms_per_frame,
                       fpga_wait_done_ms_per_frame);
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
