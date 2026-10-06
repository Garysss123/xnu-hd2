/* iOS-lab owned first-entry ABI. No runtime or hardware success is implied.
 * Every word is little-endian uint32 on ARMv7; exact size is 128 bytes. */
#ifndef IOS7LAB_LEO_HANDOFF_H
#define IOS7LAB_LEO_HANDOFF_H
#define IOS7LAB_LEO_MAGIC 0x4c454f31U
#define IOS7LAB_LEO_VERSION 1U
#define IOS7LAB_LEO_HANDOFF_BYTES 128U
#define IOS7LAB_LEO_DT_PROPERTY "ios7lab-leo-handoff"
#define IOS7LAB_LEO_MACHINE 2524U
#define IOS7LAB_LEO_RGB565 0x35363552U
#define IOS7LAB_LEO_FLAGS 3U /* bit0: validated inherited DMA; bit1: entry-only */
#define IOS7LAB_LEO_SCTLR_REJECT 0x52080085U /* M,C,B,WXN,TRE,EE,TE; I may be on */
#define IOS7LAB_LEO_MDP_PHYSICAL 0xaa200000U
#define IOS7LAB_LEO_MDP_MAP_BYTES 0x00100000U
#define IOS7LAB_LEO_EARLY_FB_LOW 0x03800000U
#define IOS7LAB_LEO_EARLY_FB_HIGH 0x03b00000U
#define IOS7LAB_LEO_EARLY_VM_RESERVE 0x00220000U
#define IOS7LAB_LEO_STAGE_BASE 0xd000U
typedef struct ios7lab_leo_handoff {
    unsigned int magic, version, bytes, flags;
    unsigned int machine, entry_sctlr, entry_cpsr, entry_atags;
    unsigned int mdp_physical, dma_config, dma_size, dma_address;
    unsigned int dma_stride, dma_xy, width, height;
    unsigned int row_bytes, pixel_format, framebuffer_bytes, memory_base;
    unsigned int memory_bytes, kernel_base, kernel_extent, top_of_kernel_data;
    unsigned int loader_start, loader_end, initrd_start, initrd_end;
    unsigned int atags_end, checkpoint_count, reserved0, reserved1;
} ios7lab_leo_handoff;
/* Loader checkpoints 1..5; XNU 6..10; final physical/fatal boundary 11.
 * Top row = packaged native callsite address; bottom row = D000|stage.
 * Failure uses final boundary plus explicit reason; no second display channel.
 * DT property is exactly this128-byte value under /chosen; no live pointers.
 * Boot_Video mirrors validated dma_address/row_bytes/width/height/depth16.
 * Boot_args v2/r1: r0 PA, virtBase80000000, validated physical ATAG-bank suffix.
 * deviceTreeP is kernel VA (PA-physBase+virtBase), not raw physical pointer.
 * physBase/memSize are nonzero1MiB multiples, memSize<=1GiB; the chosen suffix
 * excludes still-needed loader/initrd/ATAG ranges and does not claim totalRAM.
 * Loader zeros and reserves a16KiB TTB at topOfKernelData; reserve0x220000
 * beyond it: 3*16KiB + l2_size(memSize) + l2_size(1GiB) <=0x20c000.
 * entry_sctlr&SCTLR_REJECT==0; reject unknown inherited translation/execute/
 * endian state rather than inventing Scorpion reset/cache-off assumptions.
 * After final copied bytes: architectural DSB+ICIALLU+BPIALL+DSB+ISB before
 * entering XNU, even when captured I==1. No private Cortex-A8 L2/ACTLR writes.
 * Framebuffer must remain inside validated SMI range, never guessed025/02a.
 */
#endif
