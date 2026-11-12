#include "platform.h"

void sleep_ms(uint32_t ms) {
    uint64_t frq, start, current;
    uint64_t ticks_needed;

    // Read System Counter Frequency
    asm volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    ticks_needed = (frq / 1000) * ms;

    // Read Current System Counter Value
    asm volatile("mrs %0, cntpct_el0" : "=r"(start));
    do {
        asm volatile("mrs %0, cntpct_el0" : "=r"(current));
    } while ((current - start) < ticks_needed);
}

// Posts a message for core 0 to print. The acquire/release pair makes sure core 0 never sees
// full == 1 before the text, and that this core doesn't overwrite text before core 0 has printed it
void msg_post(const char *s) {
    while (__atomic_load_n(&MAILBOX->full, __ATOMIC_ACQUIRE));   // Wait for core 0 to print the previous one

    unsigned i = 0;
    while (s[i] && i < sizeof(MAILBOX->text) - 1) {
        MAILBOX->text[i] = s[i];
        i++;
    }
    MAILBOX->text[i] = '\0';

    __atomic_store_n(&MAILBOX->full, 1, __ATOMIC_RELEASE);
}

// Copies s to p and returns a pointer to the terminating '\0', so calls can be chained
char *append_str(char *p, const char *s) {
    while (*s) {
        *p++ = *s++;
    }
    *p = '\0';
    return p;
}

// Appends v in hex without leading zeros
char *append_hex(char *p, uint64_t v) {
    int shift = 60;
    p = append_str(p, "0x");
    while (shift > 0 && ((v >> shift) & 0xF) == 0) {
        shift -= 4;
    }
    for (; shift >= 0; shift -= 4) {
        *p++ = "0123456789ABCDEF"[(v >> shift) & 0xF];
    }
    *p = '\0';
    return p;
}

// Describes a synchronous exception from ESR_EL1, FAR_EL1 and ELR_EL1, e.g.
// "core 1: data abort writing 0x40000000 (translation fault, level 2), ESR 0x96000046, PC 0x40200150, halting"
void format_fault(char *buf, uint64_t esr, uint64_t far, uint64_t elr) {
    uint32_t ec  = (esr >> 26) & 0x3F;   // Exception class
    uint32_t fsc = esr & 0x3F;           // Fault status code (for aborts)
    char *p = append_str(buf, CORE_ID == 0 ? "core 0: " : "core 1: ");

    if (ec == 0x25 || ec == 0x21) {      // Data / instruction abort taken without a change in EL
        if (ec == 0x21) {
            p = append_str(p, "instruction abort fetching ");
        } else {
            p = append_str(p, (esr & (1 << 6)) ? "data abort writing " : "data abort reading ");  // WnR bit
        }
        p = append_hex(p, far);
        if ((fsc & 0x3C) == 0x04 || (fsc & 0x3C) == 0x0C) {
            p = append_str(p, (fsc & 0x3C) == 0x04 ? " (translation fault, level " : " (permission fault, level ");
            *p++ = '0' + (fsc & 3);
            p = append_str(p, ")");
        }
    } else {
        p = append_str(p, "synchronous exception");
    }

    p = append_str(p, ", ESR ");
    p = append_hex(p, esr);
    p = append_str(p, ", PC ");
    p = append_hex(p, elr);
    append_str(p, ", halting\r\n");
}
