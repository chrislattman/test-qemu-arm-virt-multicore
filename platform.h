#ifndef PLATFORM_H
#define PLATFORM_H

// --- Memory map ---
// CORE0_BASE, CORE1_BASE, SHARED_BASE and REGION_SIZE come from CMakeLists.txt, which also passes
// them to the linker. Each region is one 2 MB block in the MMU's level 2 table:
//
//   0x40000000  CORE0_BASE   Core 0: the DTB (QEMU puts it at the RAM base), core0.elf at +0x80000
//                            (where -kernel loads a raw image), its stack and translation tables
//   0x40200000  CORE1_BASE   Core 1: core1.elf, its stack and translation tables
//   0x40400000  SHARED_BASE  Shared by both cores: the mailbox
//
// Each core maps only its own block and the shared block (core 0 also maps the devices), so any
// access to the other core's block is a translation fault (see mmu.c)
#if CORE_ID == 0
#define OWN_BASE   CORE0_BASE
#else
#define OWN_BASE   CORE1_BASE
#endif

#include <stdint.h>

// --- Mailbox ---
// Only core 0 touches the UART, so core 1 posts its messages here and core 0 prints them.
// It holds one message at a time: core 1 waits until core 0 has printed the previous one
struct mailbox {
    uint32_t full;   // 1 while text holds a message core 0 hasn't printed yet
    char text[124];
};
#define MAILBOX ((struct mailbox *)(uintptr_t)SHARED_BASE)

// mmu.c
void mmu_init(void);

// common.c
void sleep_ms(uint32_t ms);
void msg_post(const char *s);
char *append_str(char *p, const char *s);
char *append_hex(char *p, uint64_t v);
void format_fault(char *buf, uint64_t esr, uint64_t far, uint64_t elr);

#endif
