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
volatile uint32_t *present_regs = NULL;
void *shared_mem_virt = NULL;

// Memory region pointers (virtual addresses)
uint8_t *I_VideoBuffer_shared = NULL;
DrawCommand *cmd_buffer_virt = NULL;
uint8_t *tex_atlas_virt = NULL;
uint8_t *colormap_virt = NULL;
static int pl_upscale_enabled = 0;
static int pl_composite_enabled = 1;
static int present_lanes = 4;
static int raster_shared_bram_enabled = 0;
static uint32_t raster_output_phys = PHY_VIDEO_BUF;
static uint32_t present_output_phys = PHY_FB_ADDR;
static int present_output_format = PRESENT_FMT_XRGB8888;
static uint32_t present_stride_bytes = 1600 * 4;
static uint32_t raster_regs_phys = ACCEL_BASE_ADDR;
static uint32_t present_regs_phys = ACCEL_PRESENT_BASE_ADDR;
static int stage5_shared_bram_handoff_enabled = 0;

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

static uint32_t parse_env_u32(const char *name, uint32_t fallback)
{
    const char *value = getenv(name);
    char *endptr = NULL;
    unsigned long parsed;

    if (!value || !*value)
        return fallback;

    parsed = strtoul(value, &endptr, 0);
    if (endptr == value || (endptr && *endptr != '\0'))
    {
        printf("WARN: invalid %s='%s', using 0x%08X\n", name, value, fallback);
        return fallback;
    }

    if (parsed > 0xFFFFFFFFUL)
    {
        printf("WARN: out-of-range %s='%s', using 0x%08X\n", name, value, fallback);
        return fallback;
    }

    return (uint32_t)parsed;
}

static int parse_env_bool(const char *name, int fallback)
{
    const char *value = getenv(name);

    if (!value || !*value)
        return fallback;

    if (!strcmp(value, "1") || !strcmp(value, "true") || !strcmp(value, "TRUE") ||
        !strcmp(value, "yes") || !strcmp(value, "YES") || !strcmp(value, "on") || !strcmp(value, "ON"))
        return 1;

    if (!strcmp(value, "0") || !strcmp(value, "false") || !strcmp(value, "FALSE") ||
        !strcmp(value, "no") || !strcmp(value, "NO") || !strcmp(value, "off") || !strcmp(value, "OFF"))
        return 0;

    printf("WARN: invalid %s='%s', using %d\n", name, value, fallback);
    return fallback;
}

static void resolve_ip_reg_bases(void)
{
    const char *swap_env = getenv("DOOM_SWAP_IPS");
    int swap = 0;
    uint32_t raster_default = ACCEL_BASE_ADDR;
    uint32_t present_default = ACCEL_PRESENT_BASE_ADDR;

    if (swap_env && *swap_env && atoi(swap_env) != 0)
    {
        swap = 1;
    }

    if (swap)
    {
        uint32_t tmp = raster_default;
        raster_default = present_default;
        present_default = tmp;
    }

    raster_regs_phys = parse_env_u32("DOOM_RASTER_BASE", raster_default);
    present_regs_phys = parse_env_u32("DOOM_PRESENT_BASE", present_default);
    // Default ON for performance; can be disabled with DOOM_STAGE5_BRAM_HANDOFF=0.
    stage5_shared_bram_handoff_enabled = parse_env_bool("DOOM_STAGE5_BRAM_HANDOFF", 1);
    // Composite mode means present reads the final indexed frame from PHY_VIDEO_BUF,
    // so HUD/menu/software overlays are included in PL upscale output.
    pl_composite_enabled = parse_env_bool("DOOM_PL_COMPOSITE", 1);

    if (raster_regs_phys == present_regs_phys)
    {
        printf("WARN: raster/present register bases are identical (0x%08X)\n",
               raster_regs_phys);
    }
}

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

static inline void program_raster_output_ptrs(uint32_t phys_addr)
{
    if (!accel_regs)
        return;

    accel_regs[REG_FB_OUT_LO / 4] = phys_addr;
    accel_regs[REG_FB_OUT_HI / 4] = 0;
    accel_regs[REG_FB_OUT1_LO / 4] = phys_addr;
    accel_regs[REG_FB_OUT1_HI / 4] = 0;
    accel_regs[REG_FB_OUT2_LO / 4] = phys_addr;
    accel_regs[REG_FB_OUT2_HI / 4] = 0;
    accel_regs[REG_FB_OUT3_LO / 4] = phys_addr;
    accel_regs[REG_FB_OUT3_HI / 4] = 0;
}

static inline void program_present_source_ptr(uint32_t phys_addr)
{
    if (!present_regs)
        return;

    present_regs[REG_CMD_BUF_LO / 4] = phys_addr;
    present_regs[REG_CMD_BUF_HI / 4] = 0;
}

// ============================================================================
// HELPER: Wait for FPGA idle
// ============================================================================
static inline void wait_fpga_idle_regs(volatile uint32_t *regs, const char *tag)
{
    int timeout = 100000;
    while ((regs[REG_CTRL / 4] & 0x4) == 0 && --timeout > 0)
        ;
    if (timeout == 0)
    {
        printf("WARN: %s idle timeout! CTRL=0x%08X\n", tag, regs[REG_CTRL / 4]);
    }
}

// ============================================================================
// HELPER: Wait for FPGA done
// ============================================================================
static inline void wait_fpga_done_regs(volatile uint32_t *regs, const char *tag)
{
    int timeout = 1000000; // Longer timeout for batch processing
    while ((regs[REG_CTRL / 4] & 0x2) == 0 && --timeout > 0)
        ;
    if (timeout == 0)
    {
        printf("ERR: %s done timeout! CTRL=0x%08X\n", tag, regs[REG_CTRL / 4]);
    }
}

// ============================================================================
// HELPER: Fire FPGA with mode
// ============================================================================
static void fire_fpga_regs(volatile uint32_t *regs, uint32_t mode, uint32_t num_commands, const char *tag)
{
    uint64_t wait_start;

    // Ensure FPGA is idle
    wait_fpga_idle_regs(regs, tag);

    // Set mode and command count
    regs[REG_MODE / 4] = mode;
    regs[REG_NUM_COMMANDS / 4] = num_commands;

    // Memory barrier
    __sync_synchronize();

    // Fire: Set ap_start
    regs[REG_CTRL / 4] = 0x1;

    // Wait for completion
    wait_start = get_time_ns();
    wait_fpga_done_regs(regs, tag);
    perf_stats.fpga_wait_ns += (get_time_ns() - wait_start);
}

static void fire_fpga(uint32_t mode, uint32_t num_commands)
{
    fire_fpga_regs(accel_regs, mode, num_commands, "raster");
}

static void fire_present(uint32_t mode)
{
    fire_fpga_regs(present_regs, mode, 0, "present");
}

// ============================================================================
// Init_Doom_Accel - Initialize hardware acceleration
// ============================================================================
void Init_Doom_Accel(void)
{
    printf("=== DOOM FPGA Accelerator v2 (Batch Mode) ===\n");
    resolve_ip_reg_bases();
    printf("IP register map: raster=0x%08X present=0x%08X\n",
           raster_regs_phys, present_regs_phys);
    printf("Stage5 BRAM handoff: %s (set DOOM_STAGE5_BRAM_HANDOFF=0 to disable)\n",
           stage5_shared_bram_handoff_enabled ? "ENABLED" : "DISABLED");
    printf("PL composite mode: %s (set DOOM_PL_COMPOSITE=0 for BRAM handoff source)\n",
           pl_composite_enabled ? "ENABLED" : "DISABLED");

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
    {
        printf("ERR: Can't open /dev/mem - running without FPGA\n");
        debug_sw_fallback = 1;
        return;
    }

    // 1. Map raster registers
    accel_regs = (volatile uint32_t *)mmap(NULL, ACCEL_SIZE,
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED, fd, raster_regs_phys);
    if (accel_regs == MAP_FAILED)
    {
        printf("ERR: Raster reg mmap failed - running without FPGA\n");
        debug_sw_fallback = 1;
        close(fd);
        return;
    }

    // 1b. Map present registers (split-IP baseline).
    // If unavailable, keep monolithic compatibility by reusing accel_regs.
    present_regs = (volatile uint32_t *)mmap(NULL, ACCEL_SIZE,
                                             PROT_READ | PROT_WRITE,
                                             MAP_SHARED, fd, present_regs_phys);
    if (present_regs == MAP_FAILED)
    {
        present_regs = accel_regs;
        printf("WARN: Present reg mmap failed @0x%08X, using monolithic fallback\n",
               present_regs_phys);
    }

    // 2. Map DDR (entire shared memory region)
    shared_mem_virt = mmap(NULL, MEM_BLOCK_SIZE,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, PHY_FB_ADDR);
    if (shared_mem_virt == MAP_FAILED)
    {
        printf("ERR: DDR mmap failed - running without FPGA\n");
        debug_sw_fallback = 1;
        if (present_regs && present_regs != accel_regs)
        {
            munmap((void *)present_regs, ACCEL_SIZE);
            present_regs = NULL;
        }
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
    printf("  Raster regs:     %p (phys 0x%08X)\n", (void *)accel_regs, raster_regs_phys);
    printf("  Present regs:    %p (phys 0x%08X)%s\n", (void *)present_regs, present_regs_phys,
           (present_regs == accel_regs) ? " [monolithic fallback]" : "");
    printf("  NOTE: DG_ScreenBuffer stays in cached DDR (malloc) for CPU scaling speed.\n");

    // Clear buffers
    memset(shared_mem_virt, 0, DOOMGENERIC_RESX * DOOMGENERIC_RESY * sizeof(uint32_t));
    memset(I_VideoBuffer_shared, 0, 320 * 200);
    memset(cmd_buffer_virt, 0, CMD_BUF_SIZE);

    // Setup FPGA pointer addresses (tell AXI masters where memory is).
    // Default route keeps raster output in DDR; runtime can switch to shared BRAM.
    raster_shared_bram_enabled = 0;
    raster_output_phys = PHY_VIDEO_BUF;
    present_output_phys = PHY_FB_ADDR;
    present_output_format = PRESENT_FMT_XRGB8888;
    present_stride_bytes = 1600 * 4;
    program_raster_output_ptrs(raster_output_phys);
    accel_regs[REG_TEX_ATLAS_LO / 4] = PHY_TEX_ADDR;
    accel_regs[REG_TEX_ATLAS_HI / 4] = 0;
    accel_regs[REG_CMAP_DDR_LO / 4] = PHY_CMAP_ADDR;
    accel_regs[REG_CMAP_DDR_HI / 4] = 0;
    accel_regs[REG_CMD_BUF_LO / 4] = PHY_CMD_BUF;
    accel_regs[REG_CMD_BUF_HI / 4] = 0;
    accel_regs[REG_PRESENT_SCALE / 4] = 1;
    accel_regs[REG_PRESENT_ROWS / 4] = 168;
    accel_regs[REG_PRESENT_LANES / 4] = (uint32_t)present_lanes;
    accel_regs[REG_PRESENT_FORMAT / 4] = (uint32_t)present_output_format;
    accel_regs[REG_PRESENT_STRIDE_BYTES / 4] = present_stride_bytes;

    // Present IP default pointers:
    // - source indexed frame from PHY_VIDEO_BUF
    // - destination fullres frame at PHY_FB_ADDR
    if (present_regs && present_regs != accel_regs)
    {
        present_regs[REG_FB_OUT_LO / 4] = present_output_phys;
        present_regs[REG_FB_OUT_HI / 4] = 0;
        present_regs[REG_FB_OUT1_LO / 4] = present_output_phys;
        present_regs[REG_FB_OUT1_HI / 4] = 0;
        present_regs[REG_FB_OUT2_LO / 4] = present_output_phys;
        present_regs[REG_FB_OUT2_HI / 4] = 0;
        present_regs[REG_FB_OUT3_LO / 4] = present_output_phys;
        present_regs[REG_FB_OUT3_HI / 4] = 0;
        present_regs[REG_TEX_ATLAS_LO / 4] = PHY_TEX_ADDR;
        present_regs[REG_TEX_ATLAS_HI / 4] = 0;
        present_regs[REG_CMAP_DDR_LO / 4] = PHY_CMAP_ADDR;
        present_regs[REG_CMAP_DDR_HI / 4] = 0;
        program_present_source_ptr(raster_output_phys);
        present_regs[REG_PRESENT_SCALE / 4] = 5;
        present_regs[REG_PRESENT_ROWS / 4] = 200;
        present_regs[REG_PRESENT_LANES / 4] = (uint32_t)present_lanes;
        present_regs[REG_PRESENT_FORMAT / 4] = (uint32_t)present_output_format;
        present_regs[REG_PRESENT_STRIDE_BYTES / 4] = present_stride_bytes;
    }

    // Verify FPGA is responding
    uint32_t ctrl_reg = accel_regs[REG_CTRL / 4];
    printf("Raster CTRL register = 0x%08X (expect ap_idle=0x4)\n", ctrl_reg);

    if ((ctrl_reg & 0x4) == 0)
    {
        printf("WARNING: Raster IP not idle - may not be programmed!\n");
    }

    if (present_regs && present_regs != accel_regs)
    {
        uint32_t pctrl = present_regs[REG_CTRL / 4];
        printf("Present CTRL register = 0x%08X (expect ap_idle=0x4)\n", pctrl);
        if ((pctrl & 0x4) == 0)
        {
            printf("WARNING: Present IP not idle - check block design clock/reset\n");
        }
    }

    cmd_count = 0;
    tex_atlas_offset = 0;

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
        if (present_regs && present_regs != accel_regs)
        {
            fire_present(MODE_LOAD_COLORMAP);
        }
        printf("Colormap loaded into FPGA BRAM\n");
    }
}

void Upload_RGBPalette(const uint8_t *palette_rgb, int size)
{
    const int palette_offset = 32 * 256; // right after colormap table
    int copy_size;

    if (!colormap_virt || !palette_rgb || size <= 0)
        return;

    copy_size = size;
    if (copy_size > 256 * 3)
        copy_size = 256 * 3;

    memcpy(colormap_virt + palette_offset, palette_rgb, copy_size);
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
    cmd_count = 0;
    // NOTE: We do NOT clear framebuffer BRAM here!
    // HUD elements persist across frames. Only call HW_ClearFramebuffer()
    // at level transitions or when explicitly needed.
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
        HW_FlushBatch();
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
    DrawCommand *cmd = &cmd_buffer_virt[cmd_count];
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
        HW_FlushBatch();
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
    DrawCommand *cmd = &cmd_buffer_virt[cmd_count];
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

    // Memory barrier to ensure all commands are written to DDR
    __sync_synchronize();

    // Combined draw + DMA in single FPGA invocation (saves one handshake)
    perf_stats.flush_calls++;
    fire_fpga(MODE_DRAW_AND_DMA, cmd_count);

    // Reset command count (batch is done)
    cmd_count = 0;
}

// ============================================================================
// HW_FinishFrame - Called at end of frame (now just a no-op)
// The real work is done by HW_FlushBatch() called from R_RenderPlayerView
// ============================================================================
void HW_FinishFrame(void)
{
    // Nothing to do - HW_FlushBatch already handled the FPGA work
    // This is kept for compatibility and in case we need end-of-frame logic
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

void HW_SetPLUpscaleEnabled(int enable)
{
    extern pixel_t *DG_ScreenBuffer;

    if (!enable)
    {
        pl_upscale_enabled = 0;
        HW_SetRasterSharedBRAM(0);
        printf("PL upscale path: disabled\n");
        return;
    }

    if (debug_sw_fallback || !accel_regs || !present_regs || !shared_mem_virt)
    {
        pl_upscale_enabled = 0;
        printf("WARN: PL upscale requested but FPGA path unavailable\n");
        return;
    }

    pl_upscale_enabled = 1;
    if (pl_composite_enabled)
    {
        HW_SetRasterSharedBRAM(0);
    }
    // Route final output buffer to FPGA-visible fullres DDR region.
    DG_ScreenBuffer = (pixel_t *)shared_mem_virt;
    printf("PL upscale path: enabled (%dx%d output via FPGA, composite=%s)\n",
           DOOMGENERIC_RESX, DOOMGENERIC_RESY, pl_composite_enabled ? "on" : "off");
}

int HW_IsPLUpscaleEnabled(void)
{
    return pl_upscale_enabled;
}

void HW_SetRasterSharedBRAM(int enable)
{
    if (pl_composite_enabled)
    {
        enable = 0;
    }

    if (!stage5_shared_bram_handoff_enabled)
    {
        enable = 0;
    }

    int can_use_split = (!debug_sw_fallback &&
                         accel_regs &&
                         present_regs &&
                         present_regs != accel_regs);
    int desired_enable = (enable && can_use_split) ? 1 : 0;
    uint32_t desired_phys = desired_enable ? PHY_STAGE5_BRAM_BUF : PHY_VIDEO_BUF;

    if (raster_shared_bram_enabled == desired_enable &&
        raster_output_phys == desired_phys)
    {
        return;
    }

    raster_shared_bram_enabled = desired_enable;
    raster_output_phys = desired_phys;

    program_raster_output_ptrs(raster_output_phys);
    if (accel_regs)
    {
        // Raster uses this register as DMA row count:
        // - 168 rows in DDR view-only mode (preserve HUD/menu software overlay)
        // - 200 rows in shared-BRAM handoff mode (full frame source for present IP)
        accel_regs[REG_PRESENT_ROWS / 4] = raster_shared_bram_enabled ? 200 : 168;
    }
    if (present_regs && present_regs != accel_regs)
    {
        program_present_source_ptr(raster_output_phys);
    }
    __sync_synchronize();

    printf("Raster->Present handoff: %s (0x%08X)\n",
           raster_shared_bram_enabled ? "shared BRAM" : "DDR",
           raster_output_phys);
}

void HW_SetPLCompositeEnabled(int enable)
{
    int new_enable = enable ? 1 : 0;
    if (pl_composite_enabled == new_enable)
    {
        return;
    }

    pl_composite_enabled = new_enable;
    if (pl_composite_enabled)
    {
        HW_SetRasterSharedBRAM(0);
    }

    printf("PL composite mode: %s\n", pl_composite_enabled ? "ENABLED" : "DISABLED");
}

int HW_IsPLCompositeEnabled(void)
{
    return pl_composite_enabled;
}

void HW_SetPresentOutputPhys(uint32_t phys_addr)
{
    if (phys_addr == 0)
    {
        phys_addr = PHY_FB_ADDR;
    }

    present_output_phys = phys_addr;

    if (!present_regs)
    {
        return;
    }

    present_regs[REG_FB_OUT_LO / 4] = present_output_phys;
    present_regs[REG_FB_OUT_HI / 4] = 0;
    present_regs[REG_FB_OUT1_LO / 4] = present_output_phys;
    present_regs[REG_FB_OUT1_HI / 4] = 0;
    present_regs[REG_FB_OUT2_LO / 4] = present_output_phys;
    present_regs[REG_FB_OUT2_HI / 4] = 0;
    present_regs[REG_FB_OUT3_LO / 4] = present_output_phys;
    present_regs[REG_FB_OUT3_HI / 4] = 0;
    __sync_synchronize();
}

uint32_t HW_GetPresentOutputPhys(void)
{
    return present_output_phys;
}

void HW_SetPresentOutputFormat(int format)
{
    int new_format = (format == PRESENT_FMT_RGB565) ? PRESENT_FMT_RGB565 : PRESENT_FMT_XRGB8888;
    present_output_format = new_format;

    if (!present_regs)
        return;

    present_regs[REG_PRESENT_FORMAT / 4] = (uint32_t)present_output_format;
    __sync_synchronize();
}

int HW_GetPresentOutputFormat(void)
{
    return present_output_format;
}

void HW_SetPresentStrideBytes(uint32_t stride_bytes)
{
    if (stride_bytes == 0)
    {
        stride_bytes = (present_output_format == PRESENT_FMT_RGB565) ? (1600u * 2u) : (1600u * 4u);
    }

    present_stride_bytes = stride_bytes;

    if (!present_regs)
        return;

    present_regs[REG_PRESENT_STRIDE_BYTES / 4] = present_stride_bytes;
    __sync_synchronize();
}

uint32_t HW_GetPresentStrideBytes(void)
{
    return present_stride_bytes;
}

void HW_SetPresentLanes(int lanes)
{
    (void)lanes;
    present_lanes = 4;

    if (accel_regs)
    {
        accel_regs[REG_PRESENT_LANES / 4] = (uint32_t)present_lanes;
    }
    if (present_regs)
    {
        present_regs[REG_PRESENT_LANES / 4] = (uint32_t)present_lanes;
    }
    __sync_synchronize();

    printf("PL present lanes: %d (quad-only)\n", present_lanes);
}

int HW_GetPresentLanes(void)
{
    return present_lanes;
}

uint64_t HW_UpscaleFrame(void)
{
    uint64_t start_ns;
    uint32_t present_src_phys;

    int monolithic_path;

    if (!pl_upscale_enabled || debug_sw_fallback || !present_regs)
        return 0;

    start_ns = get_time_ns();
    monolithic_path = (present_regs == accel_regs);
    if (pl_composite_enabled)
    {
        present_src_phys = PHY_VIDEO_BUF;
    }
    else
    {
        present_src_phys = monolithic_path ? PHY_VIDEO_BUF : raster_output_phys;
    }

    // Present IP reads indexed source and writes fullres output to present_output_phys.
    present_regs[REG_FB_OUT_LO / 4] = present_output_phys;
    present_regs[REG_FB_OUT_HI / 4] = 0;
    present_regs[REG_FB_OUT1_LO / 4] = present_output_phys;
    present_regs[REG_FB_OUT1_HI / 4] = 0;
    present_regs[REG_FB_OUT2_LO / 4] = present_output_phys;
    present_regs[REG_FB_OUT2_HI / 4] = 0;
    present_regs[REG_FB_OUT3_LO / 4] = present_output_phys;
    present_regs[REG_FB_OUT3_HI / 4] = 0;
    present_regs[REG_CMD_BUF_LO / 4] = present_src_phys;
    present_regs[REG_CMD_BUF_HI / 4] = 0;
    present_regs[REG_PRESENT_SCALE / 4] = 5;
    present_regs[REG_PRESENT_ROWS / 4] = 200;
    present_regs[REG_PRESENT_LANES / 4] = (uint32_t)present_lanes;
    present_regs[REG_PRESENT_FORMAT / 4] = (uint32_t)present_output_format;
    present_regs[REG_PRESENT_STRIDE_BYTES / 4] = present_stride_bytes;
    __sync_synchronize();

    // MODE_PRESENT is preferred for split pipeline; monolithic path keeps MODE_UPSCALE compatibility.
    if (monolithic_path)
    {
        fire_present(MODE_UPSCALE);

        // Restore monolithic draw-path bindings.
        accel_regs[REG_FB_OUT_LO / 4] = PHY_VIDEO_BUF;
        accel_regs[REG_FB_OUT_HI / 4] = 0;
        accel_regs[REG_FB_OUT1_LO / 4] = PHY_VIDEO_BUF;
        accel_regs[REG_FB_OUT1_HI / 4] = 0;
        accel_regs[REG_FB_OUT2_LO / 4] = PHY_VIDEO_BUF;
        accel_regs[REG_FB_OUT2_HI / 4] = 0;
        accel_regs[REG_FB_OUT3_LO / 4] = PHY_VIDEO_BUF;
        accel_regs[REG_FB_OUT3_HI / 4] = 0;
        accel_regs[REG_CMD_BUF_LO / 4] = PHY_CMD_BUF;
        accel_regs[REG_CMD_BUF_HI / 4] = 0;
        accel_regs[REG_PRESENT_SCALE / 4] = 1;
        accel_regs[REG_PRESENT_ROWS / 4] = raster_shared_bram_enabled ? 200 : 168;
        accel_regs[REG_PRESENT_LANES / 4] = (uint32_t)present_lanes;
        accel_regs[REG_PRESENT_FORMAT / 4] = PRESENT_FMT_XRGB8888;
        accel_regs[REG_PRESENT_STRIDE_BYTES / 4] = 1600 * 4;
        __sync_synchronize();
    }
    else
    {
        fire_present(MODE_PRESENT);
    }

    return get_time_ns() - start_ns;
}

void HW_GetAndResetPerfStats(HWPerfStats *out)
{
    if (!out)
        return;

    *out = perf_stats;
    out->tex_cache_entries = (uint32_t)tex_cache_count;
    memset(&perf_stats, 0, sizeof(perf_stats));
}
