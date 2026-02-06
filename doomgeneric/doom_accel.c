// doom_accel.c - FPGA Accelerator Driver for DOOM (Batch Rendering v2)
// Stage 2: Performance Optimization with Texture Caching
#include "doom_accel.h"
#include "doomgeneric.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

// ============================================================================
// GLOBALS
// ============================================================================
volatile uint32_t *accel_regs = NULL;
void *shared_mem_virt = NULL;

// Memory region pointers (virtual addresses)
uint8_t *I_VideoBuffer_shared = NULL;
DrawCommand *cmd_buffer_virt = NULL;
uint8_t *tex_atlas_virt = NULL;
uint8_t *colormap_virt = NULL;
static DrawCommand *cmd_buffer_staging = NULL;
static int use_cmd_staging = 0;
static int fpga_batch_in_flight = 0;

// Command buffer state
uint32_t cmd_count = 0;

// Texture atlas offset (for level texture uploads)
uint32_t tex_atlas_offset = 0;
#define TEX_ATLAS_SIZE (16 * 1024 * 1024) // 16MB

// ============================================================================
// TEXTURE OFFSET CACHE (Option A: Avoid re-uploading same textures)
// ============================================================================
#define TEX_CACHE_SIZE 16384
#define TEX_CACHE_MAX_PROBE 64

typedef struct
{
    const uint8_t *source_ptr; // Original texture pointer (key)
    uint32_t atlas_offset;     // Offset in texture atlas (value)
    int size;                  // Size of texture data
} TexCacheEntry;

static TexCacheEntry tex_offset_cache[TEX_CACHE_SIZE];
static int tex_cache_count = 0;
static const uint8_t *last_source_ptr = NULL;
static int last_source_size = 0;
static uint32_t last_source_offset = 0;
static HWPerfStats perf_stats = {0};

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Mixed hash over pointer and size to avoid clustering on aligned texture columns.
static inline uint32_t tex_ptr_hash(const uint8_t *ptr, int size)
{
#if UINTPTR_MAX > 0xFFFFFFFFu
    uint64_t z = (uint64_t)(uintptr_t)ptr;
    z ^= ((uint64_t)(uint32_t)size * 0x9E3779B1u);
    z ^= z >> 33;
    z *= 0xFF51AFD7ED558CCDULL;
    z ^= z >> 33;
    z *= 0xC4CEB9FE1A85EC53ULL;
    z ^= z >> 33;
    return (uint32_t)z & (TEX_CACHE_SIZE - 1);
#else
    uint32_t z = (uint32_t)(uintptr_t)ptr;
    z ^= (uint32_t)size * 0x9E3779B1u;
    z ^= z >> 16;
    z *= 0x7FEB352DU;
    z ^= z >> 15;
    z *= 0x846CA68BU;
    z ^= z >> 16;
    return z & (TEX_CACHE_SIZE - 1);
#endif
}

// Debug flags
int debug_sw_fallback = 0; // 0 = use FPGA, 1 = software fallback

// ============================================================================
// HELPER: Wait for FPGA idle
// ============================================================================
static inline uint64_t wait_fpga_idle(void)
{
    int timeout = 100000;
    uint64_t wait_start = get_time_ns();
    while ((accel_regs[REG_CTRL / 4] & 0x4) == 0 && --timeout > 0)
        ;
    if (timeout == 0)
    {
        printf("WARN: FPGA idle timeout! CTRL=0x%08X\n", accel_regs[REG_CTRL / 4]);
    }
    return get_time_ns() - wait_start;
}

// ============================================================================
// HELPER: Wait for FPGA done
// ============================================================================
static inline uint64_t wait_fpga_done(void)
{
    int timeout = 1000000; // Longer timeout for batch processing
    uint64_t wait_start = get_time_ns();
    while ((accel_regs[REG_CTRL / 4] & 0x2) == 0 && --timeout > 0)
        ;
    if (timeout == 0)
    {
        printf("ERR: FPGA done timeout! CTRL=0x%08X\n", accel_regs[REG_CTRL / 4]);
    }
    return get_time_ns() - wait_start;
}

// ============================================================================
// HELPER: Fire FPGA with mode
// ============================================================================
static void fire_fpga(uint32_t mode, uint32_t num_commands)
{
    uint64_t wait_idle_ns;
    uint64_t wait_done_ns;

    // Ensure FPGA is idle
    wait_idle_ns = wait_fpga_idle();
    perf_stats.fpga_wait_idle_ns += wait_idle_ns;
    perf_stats.fpga_wait_ns += wait_idle_ns;

    // Set mode and command count
    accel_regs[REG_MODE / 4] = mode;
    accel_regs[REG_NUM_COMMANDS / 4] = num_commands;

    // Memory barrier
    __sync_synchronize();

    // Fire: Set ap_start
    accel_regs[REG_CTRL / 4] = 0x1;

    // Wait for completion
    wait_done_ns = wait_fpga_done();
    perf_stats.fpga_wait_done_ns += wait_done_ns;
    perf_stats.fpga_wait_ns += wait_done_ns;
    fpga_batch_in_flight = 0;
}

static void fire_fpga_async(uint32_t mode, uint32_t num_commands)
{
    uint64_t wait_idle_ns;

    // Ensure previous submission completed before reusing command DDR.
    wait_idle_ns = wait_fpga_idle();
    perf_stats.fpga_wait_idle_ns += wait_idle_ns;
    perf_stats.fpga_wait_ns += wait_idle_ns;

    accel_regs[REG_MODE / 4] = mode;
    accel_regs[REG_NUM_COMMANDS / 4] = num_commands;
    __sync_synchronize();
    accel_regs[REG_CTRL / 4] = 0x1;
    fpga_batch_in_flight = 1;
}

// ============================================================================
// Init_Doom_Accel - Initialize hardware acceleration
// ============================================================================
void Init_Doom_Accel(void)
{
    printf("=== DOOM FPGA Accelerator v2 (Batch Mode) ===\n");

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
    {
        printf("ERR: Can't open /dev/mem - running without FPGA\n");
        debug_sw_fallback = 1;
        return;
    }

    // 1. Map Registers
    accel_regs = (volatile uint32_t *)mmap(NULL, ACCEL_SIZE,
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED, fd, ACCEL_BASE_ADDR);
    if (accel_regs == MAP_FAILED)
    {
        printf("ERR: Reg mmap failed - running without FPGA\n");
        debug_sw_fallback = 1;
        close(fd);
        return;
    }

    // 2. Map DDR (entire shared memory region)
    shared_mem_virt = mmap(NULL, MEM_BLOCK_SIZE,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, PHY_FB_ADDR);
    if (shared_mem_virt == MAP_FAILED)
    {
        printf("ERR: DDR mmap failed - running without FPGA\n");
        debug_sw_fallback = 1;
        munmap((void *)accel_regs, ACCEL_SIZE);
        accel_regs = NULL;
        close(fd);
        return;
    }

    // Setup pointers based on memory layout:
    // 0x70000000: MMIO output region (reserved for future HW upscale / direct scanout)
    // 0x70800000: I_VideoBuffer output (320x200 = 64KB)
    // 0x70810000: Command Buffer (128KB = 4000 cmds x 32B)
    // 0x70830000: Texture Atlas (16MB)
    // 0x71830000: Colormap (8KB)
    I_VideoBuffer_shared = (uint8_t *)shared_mem_virt + (PHY_VIDEO_BUF - PHY_FB_ADDR);
    cmd_buffer_virt = (DrawCommand *)((uint8_t *)shared_mem_virt + (PHY_CMD_BUF - PHY_FB_ADDR));
    tex_atlas_virt = (uint8_t *)shared_mem_virt + (PHY_TEX_ADDR - PHY_FB_ADDR);
    colormap_virt = (uint8_t *)shared_mem_virt + (PHY_CMAP_ADDR - PHY_FB_ADDR);

    // CRITICAL: Set I_VideoBuffer to shared DDR so CPU rendering goes there too
    extern uint8_t *I_VideoBuffer;
    I_VideoBuffer = I_VideoBuffer_shared;

    printf("Memory Layout:\n");
    printf("  MMIO FB region:  %p (phys 0x%08X)\n", shared_mem_virt, PHY_FB_ADDR);
    printf("  I_VideoBuffer:   %p (phys 0x%08X)\n", I_VideoBuffer_shared, PHY_VIDEO_BUF);
    printf("  Command Buffer:  %p (phys 0x%08X)\n", cmd_buffer_virt, PHY_CMD_BUF);
    printf("  Texture Atlas:   %p (phys 0x%08X)\n", tex_atlas_virt, PHY_TEX_ADDR);
    printf("  Colormap:        %p (phys 0x%08X)\n", colormap_virt, PHY_CMAP_ADDR);
    printf("  NOTE: DG_ScreenBuffer stays in cached DDR (malloc) for CPU scaling speed.\n");

    // Clear buffers
    memset(shared_mem_virt, 0, 1920 * 1080 * 4);
    memset(I_VideoBuffer_shared, 0, 320 * 200);
    memset(cmd_buffer_virt, 0, CMD_BUF_SIZE);

    // Stage 3.1: build commands in cached PS RAM, then upload in one burst.
    if (!cmd_buffer_staging)
    {
        cmd_buffer_staging = (DrawCommand *)malloc(CMD_BUF_SIZE);
    }
    if (cmd_buffer_staging)
    {
        memset(cmd_buffer_staging, 0, CMD_BUF_SIZE);
        use_cmd_staging = 1;
        printf("Command staging: enabled (%u bytes cached buffer)\n", (unsigned)CMD_BUF_SIZE);
    }
    else
    {
        use_cmd_staging = 0;
        printf("WARN: Command staging alloc failed, using direct /dev/mem writes\n");
    }

    // Setup FPGA pointer addresses (tell AXI masters where memory is)
    accel_regs[REG_FB_OUT_LO / 4] = PHY_VIDEO_BUF;
    accel_regs[REG_FB_OUT_HI / 4] = 0;
    accel_regs[REG_TEX_ATLAS_LO / 4] = PHY_TEX_ADDR;
    accel_regs[REG_TEX_ATLAS_HI / 4] = 0;
    accel_regs[REG_CMAP_DDR_LO / 4] = PHY_CMAP_ADDR;
    accel_regs[REG_CMAP_DDR_HI / 4] = 0;
    accel_regs[REG_CMD_BUF_LO / 4] = PHY_CMD_BUF;
    accel_regs[REG_CMD_BUF_HI / 4] = 0;

    // Verify FPGA is responding
    uint32_t ctrl_reg = accel_regs[REG_CTRL / 4];
    printf("FPGA CTRL register = 0x%08X (expect ap_idle=0x4)\n", ctrl_reg);

    if ((ctrl_reg & 0x4) == 0)
    {
        printf("WARNING: FPGA not idle - may not be programmed!\n");
    }

    cmd_count = 0;
    tex_atlas_offset = 0;
    fpga_batch_in_flight = 0;

    printf("=== ACCEL INIT COMPLETE ===\n");
}

// ============================================================================
// Upload_Colormap - Copy colormap to DDR and load into FPGA BRAM
// ============================================================================
void Upload_Colormap(const uint8_t *colormaps_ptr, int size)
{
    if (!colormap_virt || !colormaps_ptr || size <= 0)
    {
        printf("ERR: Cannot upload colormap - invalid pointers\n");
        return;
    }

    // Copy colormap to DDR
    memcpy(colormap_virt, colormaps_ptr, size);
    printf("Uploaded %d bytes of colormap to DDR\n", size);

    // If FPGA available, trigger load into BRAM
    if (accel_regs && !debug_sw_fallback)
    {
        fire_fpga(MODE_LOAD_COLORMAP, 0);
        printf("Colormap loaded into FPGA BRAM\n");
    }
}

// ============================================================================
// Upload_Texture_Data - Copy texture data to atlas, return offset
// Uses texture cache to avoid re-uploading same texture data
// ============================================================================
uint32_t Upload_Texture_Data(const uint8_t *source, int size)
{
    int empty_idx = -1;

    if (!tex_atlas_virt || !source || size <= 0)
        return 0;

    perf_stats.tex_cache_lookups++;

    // Fast path for repeated pointer in consecutive draw calls.
    if (source == last_source_ptr && size == last_source_size)
    {
        perf_stats.tex_cache_hits++;
        return last_source_offset;
    }

    // ========== OPTION A: Check texture offset cache first ==========
    uint32_t hash = tex_ptr_hash(source, size);

    // Linear probing for collision resolution
    for (int probe = 0; probe < TEX_CACHE_MAX_PROBE; probe++)
    {
        uint32_t idx = (hash + probe) & (TEX_CACHE_SIZE - 1);

        if (tex_offset_cache[idx].source_ptr == source &&
            tex_offset_cache[idx].size == size)
        {
            // Cache hit! Return existing offset
            perf_stats.tex_cache_hits++;
            last_source_ptr = source;
            last_source_size = size;
            last_source_offset = tex_offset_cache[idx].atlas_offset;
            return tex_offset_cache[idx].atlas_offset;
        }

        if (tex_offset_cache[idx].source_ptr == NULL && empty_idx < 0)
        {
            empty_idx = (int)idx;
            break;
        }
    }

    // Fallback full scan only when bounded probe cannot resolve collisions.
    if (empty_idx < 0)
    {
        for (int probe = TEX_CACHE_MAX_PROBE; probe < TEX_CACHE_SIZE; probe++)
        {
            uint32_t idx = (hash + probe) & (TEX_CACHE_SIZE - 1);

            if (tex_offset_cache[idx].source_ptr == source &&
                tex_offset_cache[idx].size == size)
            {
                perf_stats.tex_cache_hits++;
                last_source_ptr = source;
                last_source_size = size;
                last_source_offset = tex_offset_cache[idx].atlas_offset;
                return tex_offset_cache[idx].atlas_offset;
            }

            if (tex_offset_cache[idx].source_ptr == NULL)
            {
                empty_idx = (int)idx;
                break;
            }
        }
    }

    perf_stats.tex_cache_misses++;

    // ========== Cache miss - upload texture ==========

    // Check space (wrap if needed - shouldn't happen within a level)
    if (tex_atlas_offset + size > TEX_ATLAS_SIZE)
    {
        printf("WARN: Texture atlas overflow, wrapping\n");
        perf_stats.tex_atlas_wraps++;
        tex_atlas_offset = 0;
        // Clear cache on wrap
        memset(tex_offset_cache, 0, sizeof(tex_offset_cache));
        tex_cache_count = 0;
        empty_idx = -1;
        last_source_ptr = NULL;
        last_source_size = 0;
        last_source_offset = 0;
        // Invalidate FPGA texture caches (stale offsets after wrap)
        if (accel_regs && !debug_sw_fallback)
        {
            fire_fpga(MODE_LOAD_COLORMAP, 0);
        }
    }

    // Align to 16 bytes for optimal 128-bit AXI access
    uint32_t aligned_offset = (tex_atlas_offset + 15) & ~15;

    memcpy(tex_atlas_virt + aligned_offset, source, size);
    perf_stats.tex_upload_bytes += (uint64_t)size;
    tex_atlas_offset = aligned_offset + size;

    // ========== Store in cache ==========
    if (empty_idx >= 0)
    {
        tex_offset_cache[empty_idx].source_ptr = source;
        tex_offset_cache[empty_idx].atlas_offset = aligned_offset;
        tex_offset_cache[empty_idx].size = size;
        tex_cache_count++;
    }
    else
    {
        // Table is effectively saturated in this hash neighborhood.
        // Replace home bucket so future accesses to this source hit.
        perf_stats.tex_cache_failed_inserts++;
        tex_offset_cache[hash].source_ptr = source;
        tex_offset_cache[hash].atlas_offset = aligned_offset;
        tex_offset_cache[hash].size = size;
    }

    last_source_ptr = source;
    last_source_size = size;
    last_source_offset = aligned_offset;

    return aligned_offset;
}

// ============================================================================
// Reset_Texture_Atlas - Full reset for level transitions (clears everything)
// ============================================================================
void Reset_Texture_Atlas(void)
{
    tex_atlas_offset = 0;

    // Clear texture offset cache
    memset(tex_offset_cache, 0, sizeof(tex_offset_cache));
    tex_cache_count = 0;
    last_source_ptr = NULL;
    last_source_size = 0;
    last_source_offset = 0;
}

// ============================================================================
// HW_StartFrame - Begin a new frame (reset command buffer)
// ============================================================================
void HW_StartFrame(void)
{
    // Fence previous async submission before starting a new frame.
    HW_WaitForBatch();
    cmd_count = 0;
    // NOTE: We do NOT clear framebuffer BRAM here!
    // HUD elements persist across frames. Only call HW_ClearFramebuffer()
    // at level transitions or when explicitly needed.
}

static void HW_FlushBatchInternal(int blocking)
{
    if (debug_sw_fallback || !accel_regs)
    {
        // Software mode - already rendered during HW_QueueColumn/HW_QueueSpan
        return;
    }

    if (cmd_count == 0)
    {
        // No commands to process - nothing to flush
        return;
    }

    // Command DDR region is reused every flush; fence before overwrite.
    HW_WaitForBatch();

    if (use_cmd_staging)
    {
        size_t bytes = (size_t)cmd_count * sizeof(DrawCommand);
        memcpy(cmd_buffer_virt, cmd_buffer_staging, bytes);
        perf_stats.cmd_upload_bytes += (uint64_t)bytes;
    }

    // Memory barrier to ensure all commands are written to DDR
    __sync_synchronize();

    // Submit draw + DMA and optionally wait (mid-frame overflow path).
    perf_stats.flush_calls++;
    fire_fpga_async(MODE_DRAW_AND_DMA, cmd_count);

    if (blocking)
    {
        HW_WaitForBatch();
    }

    cmd_count = 0;
}

// ============================================================================
// HW_QueueColumn - Queue a column draw command (walls)
// ============================================================================
void HW_QueueColumn(int x, int y_start, int y_end, uint32_t frac, uint32_t step,
                    uint32_t tex_offset, int light_level)
{
    // Safety clamp
    if (x < 0 || x >= 320)
        return;
    if (y_start < 0)
        y_start = 0;
    if (y_end >= 200)
        y_end = 199;
    if (y_start > y_end)
        return;

    // Check buffer capacity
    if (cmd_count >= MAX_COMMANDS)
    {
        // Buffer full - flush current batch and continue
        printf("WARN: Command buffer full (%u cmds), flushing mid-frame\n", cmd_count);
        perf_stats.mid_frame_flushes++;
        HW_FlushBatchInternal(1);
    }

    // Software fallback mode: draw directly
    if (debug_sw_fallback || !accel_regs)
    {
        // CPU software rendering
        uint8_t *vbuf = I_VideoBuffer_shared;
        if (!vbuf)
            return;

        // Simple colored column for debug
        uint8_t color = (uint8_t)((x * 7) & 0xFF);
        for (int y = y_start; y <= y_end; y++)
        {
            vbuf[y * 320 + x] = color;
        }
        return;
    }

    // Queue command for FPGA batch processing
    DrawCommand *cmd = use_cmd_staging ? &cmd_buffer_staging[cmd_count] : &cmd_buffer_virt[cmd_count];
    cmd->cmd_type = CMD_TYPE_COLUMN;
    cmd->cmap_index = (uint8_t)(light_level & 31); // Clamp to 32 levels
    cmd->x1 = (uint16_t)x;
    cmd->x2 = 0; // unused for column
    cmd->y1 = (uint16_t)y_start;
    cmd->y2 = (uint16_t)y_end;
    cmd->reserved1 = 0;
    cmd->frac = frac;
    cmd->step = step;
    cmd->tex_offset = tex_offset;
    cmd->reserved2 = 0;
    cmd->reserved3 = 0;

    cmd_count++;
    perf_stats.queued_columns++;
    if (cmd_count > perf_stats.max_cmds_seen)
    {
        perf_stats.max_cmds_seen = cmd_count;
    }
}

// ============================================================================
// HW_QueueSpan - Queue a span draw command (floors/ceilings)
// ============================================================================
void HW_QueueSpan(int y, int x1, int x2, uint32_t position, uint32_t step,
                  uint32_t tex_offset, int light_level)
{
    // Safety clamp
    if (y < 0 || y >= 200)
        return;
    if (x1 < 0)
        x1 = 0;
    if (x2 >= 320)
        x2 = 319;
    if (x1 > x2)
        return;

    // Check buffer capacity
    if (cmd_count >= MAX_COMMANDS)
    {
        printf("WARN: Command buffer full (%u cmds), flushing mid-frame\n", cmd_count);
        perf_stats.mid_frame_flushes++;
        HW_FlushBatchInternal(1);
    }

    // Software fallback mode: draw directly
    if (debug_sw_fallback || !accel_regs)
    {
        uint8_t *vbuf = I_VideoBuffer_shared;
        if (!vbuf)
            return;

        // Simple colored span for debug
        uint8_t color = (uint8_t)((y * 3) & 0xFF);
        for (int x = x1; x <= x2; x++)
        {
            vbuf[y * 320 + x] = color;
        }
        return;
    }

    // Queue command for FPGA batch processing
    DrawCommand *cmd = use_cmd_staging ? &cmd_buffer_staging[cmd_count] : &cmd_buffer_virt[cmd_count];
    cmd->cmd_type = CMD_TYPE_SPAN;
    cmd->cmap_index = (uint8_t)(light_level & 31);
    cmd->x1 = (uint16_t)x1;
    cmd->x2 = (uint16_t)x2;
    cmd->y1 = (uint16_t)y;
    cmd->y2 = 0; // unused for span
    cmd->reserved1 = 0;
    cmd->frac = position; // Packed texture position
    cmd->step = step;     // Packed texture step
    cmd->tex_offset = tex_offset;
    cmd->reserved2 = 0;
    cmd->reserved3 = 0;

    cmd_count++;
    perf_stats.queued_spans++;
    if (cmd_count > perf_stats.max_cmds_seen)
    {
        perf_stats.max_cmds_seen = cmd_count;
    }
}

// ============================================================================
// HW_FlushBatch - Execute queued commands and DMA to DDR
// Called after walls+floors, before sprites, so CPU can draw on top
// ============================================================================
void HW_FlushBatch(void)
{
    // Stage 3.2: async submit; fence later at frame present.
    HW_FlushBatchInternal(0);
}

void HW_WaitForBatch(void)
{
    uint64_t wait_done_ns;

    if (debug_sw_fallback || !accel_regs || !fpga_batch_in_flight)
    {
        return;
    }

    wait_done_ns = wait_fpga_done();
    perf_stats.fpga_wait_done_ns += wait_done_ns;
    perf_stats.fpga_wait_ns += wait_done_ns;
    fpga_batch_in_flight = 0;
}

// ============================================================================
// HW_FinishFrame - Called at end of frame (now just a no-op)
// The real work is done by HW_FlushBatch() called from R_RenderPlayerView
// ============================================================================
void HW_FinishFrame(void)
{
    // Keep explicit fence hook for compatibility.
    HW_WaitForBatch();
}

// ============================================================================
// HW_ClearFramebuffer - Clear FPGA framebuffer BRAM (level transitions)
// ============================================================================
void HW_ClearFramebuffer(void)
{
    if (debug_sw_fallback || !accel_regs)
    {
        // Software: clear DDR buffer directly
        if (I_VideoBuffer_shared)
        {
            memset(I_VideoBuffer_shared, 0, 320 * 200);
        }
        return;
    }

    fire_fpga(MODE_CLEAR_FB, 0);

    // Reset texture atlas on level transition (FPGA caches also invalidated)
    Reset_Texture_Atlas();
}

void HW_GetAndResetPerfStats(HWPerfStats *out)
{
    if (!out)
        return;

    *out = perf_stats;
    out->tex_cache_entries = (uint32_t)tex_cache_count;
    memset(&perf_stats, 0, sizeof(perf_stats));
}
