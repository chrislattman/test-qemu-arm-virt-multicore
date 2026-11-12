#include "platform.h"

// Per-core translation tables. Each core's image has its own copy in its own private block, and each
// core's TTBR0_EL1 points to its own copy, so the two cores see different memory maps.
// 4 KB granule with 39-bit virtual addresses: level 1 entries map 1 GB, level 2 entries map 2 MB.
// Virtual addresses equal physical addresses (identity mapping)
static uint64_t l1_table[512] __attribute__((aligned(4096))); // 0x00_0000_0000-0x7F_FFFF_FFFF
static uint64_t l2_ram[512] __attribute__((aligned(4096)));   // 0x4000_0000-0x7FFF_FFFF (RAM)

// Descriptor bits
#define DESC_VALID       (1UL << 0)
#define DESC_TABLE       (1UL << 1)              // Level 1: points to a level 2 table (clear = 1 GB block)
#define DESC_ATTR(n)     ((uint64_t)(n) << 2)    // Index into MAIR_EL1
#define DESC_SH_INNER    (3UL << 8)              // Inner shareable: coherent between the cores
#define DESC_AF          (1UL << 10)             // Access flag (if clear, the first access faults)
#define DESC_PXN         (1UL << 53)             // Not executable at EL1
#define DESC_UXN         (1UL << 54)             // Not executable at EL0

// MAIR_EL1 attribute indexes
#define ATTR_NORMAL      0   // Normal memory, write-back cacheable
#define ATTR_DEVICE      1   // Device-nGnRnE
#define MAIR_VALUE       ((0xFFUL << (8 * ATTR_NORMAL)) | (0x00UL << (8 * ATTR_DEVICE)))

#define NORMAL_BLOCK     (DESC_VALID | DESC_ATTR(ATTR_NORMAL) | DESC_SH_INNER | DESC_AF)
#define DEVICE_BLOCK     (DESC_VALID | DESC_ATTR(ATTR_DEVICE) | DESC_AF | DESC_PXN | DESC_UXN)

#define RAM_BASE         0x40000000UL
#define BLOCK_SIZE       0x200000UL
#define L2_INDEX(addr)   (((uintptr_t)(addr) - RAM_BASE) / BLOCK_SIZE)

_Static_assert(REGION_SIZE == BLOCK_SIZE, "each region must be exactly one level 2 block");

// TCR_EL1
#define TCR_T0SZ         (64 - 39)    // 39-bit virtual addresses through TTBR0_EL1
#define TCR_IRGN0_WB     (1UL << 8)   // Table walks: inner write-back cacheable
#define TCR_ORGN0_WB     (1UL << 10)  // Table walks: outer write-back cacheable
#define TCR_SH0_INNER    (3UL << 12)  // Table walks: inner shareable
#define TCR_EPD1         (1UL << 23)  // No TTBR1_EL1 walks (the upper half of the address space is unused)
#define TCR_IPS_40BIT    (2UL << 32)  // 40-bit physical addresses (the Cortex-A53's maximum)
#define TCR_VALUE        (TCR_T0SZ | TCR_IRGN0_WB | TCR_ORGN0_WB | TCR_SH0_INNER | TCR_EPD1 | TCR_IPS_40BIT)

// SCTLR_EL1
#define SCTLR_M          (1UL << 0)   // MMU enable
#define SCTLR_C          (1UL << 2)   // Data cache enable
#define SCTLR_I          (1UL << 12)  // Instruction cache enable

void mmu_init(void) {
    // Every entry starts out invalid (.bss is zeroed), so any address not mapped here faults.
    // This is a whitelist: the other core's block is simply never mapped
#if CORE_ID == 0
    // 0-1 GB: devices (GIC at 0x08000000, UART at 0x09000000), never executable. Only core 0 uses them
    l1_table[0] = 0x00000000UL | DEVICE_BLOCK;
#endif
    // 1-2 GB: RAM, mapped in 2 MB blocks by the level 2 table
    l1_table[1] = (uintptr_t)l2_ram | DESC_TABLE | DESC_VALID;

    l2_ram[L2_INDEX(OWN_BASE)]    = (uintptr_t)OWN_BASE | NORMAL_BLOCK;
    l2_ram[L2_INDEX(SHARED_BASE)] = (uintptr_t)SHARED_BASE | NORMAL_BLOCK | DESC_PXN | DESC_UXN;

    asm volatile(
        "msr  mair_el1, %0\n"
        "msr  tcr_el1, %1\n"
        "msr  ttbr0_el1, %2\n"
        "isb\n"
        "tlbi vmalle1\n"   // Discard any stale TLB entries
        "dsb  nsh\n"
        "isb\n"
        :: "r"(MAIR_VALUE), "r"(TCR_VALUE), "r"(l1_table) : "memory");

    uint64_t sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
    asm volatile("msr sctlr_el1, %0\n"
                 "isb" :: "r"(sctlr) : "memory");
}
