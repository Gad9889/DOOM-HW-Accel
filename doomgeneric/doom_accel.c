#include "doom_accel.h"
#include <string.h>
#include <stdio.h>

// GLOBALS
volatile uint32_t *accel_regs = NULL;
void *shared_mem_virt = NULL;

// Texture atlas pointer and rolling offset
uint8_t *tex_atlas_virt = NULL;
uint32_t tex_atlas_offset = 0;
#define TEX_ATLAS_SIZE (16 * 1024 * 1024) // 16MB texture atlas

// Colormap pointer (virtual address)
uint8_t *colormap_virt = NULL;

// I_VideoBuffer shared pointer (320x200 unified buffer)
uint8_t *I_VideoBuffer_shared = NULL;

// DEBUG COUNTER
int debug_log_count = 0;
FILE *debug_file = NULL;

// DEBUG MODE FLAGS
// Set to 1 to use software rendering (colored columns or textured)
// Set to 0 to use FPGA (requires synthesized HLS IP deployed on PYNQ)
int debug_sw_fallback = 0; // Set to 1 until new HLS IP is synthesized

// Reset texture atlas offset at start of each frame
void Reset_Texture_Atlas()
{
    tex_atlas_offset = 0;
}

// Upload a texture column to the atlas and return its offset
uint32_t Upload_Texture_Column(const uint8_t *source, int height)
{
    if (!tex_atlas_virt || !source || height <= 0)
        return 0;

    // Check if we have space (wrap around if needed)
    if (tex_atlas_offset + height > TEX_ATLAS_SIZE)
    {
        tex_atlas_offset = 0; // Wrap around
    }

    uint32_t offset = tex_atlas_offset;

    // Copy the texture column data
    memcpy(tex_atlas_virt + offset, source, height);

    // Advance the offset
    tex_atlas_offset += height;

    return offset;
}

// Upload the colormap table to shared DDR for FPGA access
// DOOM has 32 light levels, each with 256 palette mappings = 8KB total
void Upload_Colormap(const uint8_t *colormaps_ptr, int size)
{
    if (!colormap_virt || !colormaps_ptr || size <= 0)
    {
        printf("ERR: Cannot upload colormap - pointers not initialized\n");
        return;
    }

    // Copy colormap to DDR
    memcpy(colormap_virt, colormaps_ptr, size);
    printf("DEBUG: Uploaded %d bytes of colormap to DDR\n", size);
}

void Init_Doom_Accel()
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
    {
        printf("ERR: Can't open /dev/mem\n");
        exit(1);
    }

    // 1. Map Registers
    accel_regs = (volatile uint32_t *)mmap(NULL, ACCEL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, ACCEL_BASE_ADDR);
    if (accel_regs == MAP_FAILED)
    {
        printf("ERR: Reg mmap failed\n");
        exit(1);
    }

    // 2. Map DDR (entire shared memory region)
    shared_mem_virt = mmap(NULL, MEM_BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PHY_FB_ADDR);
    if (shared_mem_virt == MAP_FAILED)
    {
        printf("ERR: DDR mmap failed\n");
        exit(1);
    }

    // Setup pointers based on memory layout:
    // 0x70000000: DG_ScreenBuffer (1920x1080x4 = 8MB)
    // 0x70800000: I_VideoBuffer (320x200 = 64KB)
    // 0x70810000: Texture Atlas (16MB)
    // 0x71810000: Colormap (8KB)

    DG_ScreenBuffer = (uint32_t *)shared_mem_virt;
    I_VideoBuffer_shared = (uint8_t *)shared_mem_virt + 0x00800000; // +8MB
    tex_atlas_virt = (uint8_t *)shared_mem_virt + 0x00810000;       // +8MB+64KB
    colormap_virt = (uint8_t *)shared_mem_virt + 0x01810000;        // +24MB+64KB
    tex_atlas_offset = 0;

    // CRITICAL: Set I_VideoBuffer to shared DDR so all rendering goes to same buffer
    extern uint8_t *I_VideoBuffer;
    I_VideoBuffer = I_VideoBuffer_shared;
    printf("DEBUG: I_VideoBuffer set to shared DDR at %p\n", I_VideoBuffer);

    // Clear framebuffer to black
    memset(DG_ScreenBuffer, 0, 1920 * 1080 * 4);

    // Clear I_VideoBuffer (320x200)
    memset(I_VideoBuffer_shared, 0, 320 * 200);

    // Setup FPGA Pointer Addresses (set once at init)
    // These tell the AXI masters where to read/write in DDR
    accel_regs[REG_VIDEO_BUF_LO / 4] = PHY_VIDEO_BUF;
    accel_regs[REG_VIDEO_BUF_HI / 4] = 0;
    accel_regs[REG_TEX_ATLAS_LO / 4] = PHY_TEX_ADDR;
    accel_regs[REG_TEX_ATLAS_HI / 4] = 0;
    accel_regs[REG_COLORMAP_LO / 4] = PHY_CMAP_ADDR;
    accel_regs[REG_COLORMAP_HI / 4] = 0;
    
    printf("DEBUG: FPGA pointers set - VIDEO=0x%08X TEX=0x%08X CMAP=0x%08X\n",
           PHY_VIDEO_BUF, PHY_TEX_ADDR, PHY_CMAP_ADDR);

    // Verify FPGA is responding by reading control register
    uint32_t ctrl_reg = accel_regs[REG_CTRL / 4];
    printf("DEBUG: FPGA CTRL register = 0x%08X (expect ap_idle=0x4)\n", ctrl_reg);
    if ((ctrl_reg & 0x4) == 0) {
        printf("WARNING: FPGA ap_idle not set - IP may not be ready!\n");
    }

    // Clear texture atlas
    printf("DEBUG: Clearing texture atlas...\n");
    memset(tex_atlas_virt, 0, TEX_ATLAS_SIZE);
    printf("DEBUG: Texture atlas ready.\n");

    // Open Debug Log
    debug_file = fopen("debug_draw.log", "w");
    if (debug_file)
        fprintf(debug_file, "--- DOOM ACCEL DEBUG LOG ---\n");

    printf("--- ACCEL INIT COMPLETE ---\n");
}

void HW_DrawColumn(int x, int y_start, int y_end, int step, int frac, int tex_offset, int colormap_offset)
{
    if (!accel_regs)
        return;

    // UNIFIED BUFFER: FPGA writes to 320x200 I_VideoBuffer (8-bit palette indexed)
    // No scaling here - CPU handles scaling in I_FinishUpdate()

    // Safety Clamping (for unscaled coords)
    if (y_start < 0)
        y_start = 0;
    if (y_end >= 200)
        y_end = 199;
    if (x < 0 || x >= 320)
        return;
    if (y_start > y_end)
        return;

    // DEBUG LOGGING (First 50 calls only)
    if (debug_file && debug_log_count < 50)
    {
        // Log FPGA control register state before command
        uint32_t ctrl_before = accel_regs[REG_CTRL / 4];
        fprintf(debug_file, "CTRL=0x%02X X=%d Y=%d-%d step=%d frac=%d tex=%d cmap=%d\n",
                ctrl_before, x, y_start, y_end, step, frac, tex_offset, colormap_offset);
        debug_log_count++;
        if (debug_log_count == 50)
        {
            fprintf(debug_file, "--- LOG STOPPED ---\n");
            fclose(debug_file);
            debug_file = NULL;
        }
    }

    // =========================================================
    // DEBUG SOFTWARE FALLBACK MODE
    // Set debug_sw_fallback=1 to draw colored columns via CPU
    // =========================================================
    if (debug_sw_fallback)
    {
        // Draw columns directly to I_VideoBuffer (320x200, 8-bit palette)
        uint8_t *vbuf = I_VideoBuffer_shared;
        if (!vbuf)
            return;

        // Use a distinguishable palette index based on x position
        uint8_t color = (uint8_t)((x * 7) & 0xFF);

        for (int y = y_start; y <= y_end; y++)
        {
            vbuf[y * 320 + x] = color;
        }
        return;
    }
    // =========================================================

    // Send commands to FPGA for unified 320x200 buffer rendering

    // Command 1: [step(32) | frac(32)]
    accel_regs[REG_CMD1_LO / 4] = (uint32_t)frac;
    accel_regs[REG_CMD1_HI / 4] = (uint32_t)step;

    // Command 2: [y_end(16) | y_start(16) | x(16) | 0(16)]
    uint64_t cmd2 = ((uint64_t)y_end << 48) |
                    ((uint64_t)y_start << 32) |
                    ((uint64_t)x << 16);
    accel_regs[REG_CMD2_LO / 4] = (uint32_t)(cmd2 & 0xFFFFFFFF);
    accel_regs[REG_CMD2_HI / 4] = (uint32_t)(cmd2 >> 32);

    // Command 3: [colormap_offset(32) | tex_offset(32)]
    uint64_t cmd3 = ((uint64_t)colormap_offset << 32) | (uint32_t)tex_offset;
    accel_regs[REG_CMD3_LO / 4] = (uint32_t)(cmd3 & 0xFFFFFFFF);
    accel_regs[REG_CMD3_HI / 4] = (uint32_t)(cmd3 >> 32);

    // Memory barrier to ensure all writes are visible before starting FPGA
    __sync_synchronize();

    // Fire: Set ap_start bit (bit 0)
    accel_regs[REG_CTRL / 4] = 0x1;
    
    // Wait for ap_done (bit 1) 
    // Timeout after ~1000 iterations to avoid infinite loop
    int timeout = 10000;
    while ((accel_regs[REG_CTRL / 4] & 0x2) == 0 && --timeout > 0)
        ;
    
    if (timeout == 0 && debug_log_count < 5) {
        printf("FPGA TIMEOUT! CTRL=0x%08X\n", accel_regs[REG_CTRL / 4]);
    }
}