// Core 1: uses no peripherals. It posts its messages to the mailbox in the shared block, and
// core 0 prints them
#include "platform.h"

// Core 1 never enables any interrupts in the GIC, but the vector table in startup.S needs this
void handle_irq(void) {}

// Synchronous exceptions (e.g. a data abort from an unmapped address), called from sync_entry in
// startup.S, which halts this core afterwards
void handle_sync(uint64_t esr, uint64_t far, uint64_t elr) {
    char buf[124];
    format_fault(buf, esr, far, elr);
    msg_post(buf);
}

void main_app(void) {
    mmu_init();
    msg_post("core 1: MMU on\r\n");

    while (1) {
        msg_post("Hello from core 1\r\n");
#ifdef NDEBUG
        sleep_ms(3000);
#endif
    }
}
