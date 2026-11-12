// Core 0: owns the UART and the GIC. It starts core 1 with PSCI and prints core 1's messages from
// the mailbox
#include "platform.h"

// --- UART PL011 ---
#define UART0_BASE               0x09000000
#define UART0_DR                 ((volatile uint32_t *)(UART0_BASE + 0x00))
#define UART0_FR                 ((volatile uint32_t *)(UART0_BASE + 0x18))
#define UART_FR_TXFF             (1 << 5)

// --- GICv2 (QEMU virt Memory Map) ---
#define GIC_DIST_BASE    0x08000000  // Distributor Base Address
#define GIC_CPU_BASE     0x08010000  // CPU Interface Base Address

// Distributor Registers
#define GICD_CTLR        ((volatile uint32_t *)(GIC_DIST_BASE + 0x000))
#define GICD_ISENABLER   ((volatile uint32_t *)(GIC_DIST_BASE + 0x100))
#define GICD_SGIR        ((volatile uint32_t *)(GIC_DIST_BASE + 0xF00))

// CPU Interface Registers
#define GICC_CTLR        ((volatile uint32_t *)(GIC_CPU_BASE + 0x000))
#define GICC_PMR         ((volatile uint32_t *)(GIC_CPU_BASE + 0x004))
#define GICC_IAR         ((volatile uint32_t *)(GIC_CPU_BASE + 0x00C))
#define GICC_EOIR        ((volatile uint32_t *)(GIC_CPU_BASE + 0x010))

// Assigning SGI ID IDs
#define HW_INT_SGI_ID    1
#define SW_INT_SGI_ID    2

// --- PSCI (Power State Coordination Interface) ---
// With -kernel and no firmware, QEMU implements PSCI itself and starts every core except core 0
// powered off. The guest runs at EL1 (no EL2 or EL3), so the calls go through HVC
#define PSCI_CPU_ON_64   0xC4000003

// Shared variable across foreground code and async ISR layers
static volatile uint32_t shared_data = 0;

void uart_puts(const char *s) {
    while (*s) {
        // Wait until UART TX FIFO has space
        while (*UART0_FR & UART_FR_TXFF);
        *UART0_DR = *s++;
    }
}

static inline void enable_interrupts(void)  { asm volatile("msr daifclr, #2" ::: "memory"); }
static inline void disable_interrupts(void) { asm volatile("msr daifset, #2" ::: "memory"); }

void trigger_sgi(uint32_t sgi_id) {
    // Bits [25:24]: Target List Filter (0b10 = forward strictly to the current requesting CPU core)
    // Bits [3:0]: SGI Interrupt ID Number
    *GICD_SGIR = (2 << 24) | (sgi_id & 0xF);
}

// The Unified C-Level Interrupt Vector Handler
void handle_irq(void) {
    // Read the Interrupt Acknowledge Register to get the pending ID
    uint32_t iar = *GICC_IAR;
    uint32_t interrupt_id = iar & 0x3FF; // Isolate lower 10 bits

    if (interrupt_id == HW_INT_SGI_ID) {
        uart_puts("[ISR] 'Hardware' Interrupt tripped! Writing data to shared space...\r\n");
        shared_data = 0xAABBCCDD; // Write to shared variable

        // Instantly trigger our secondary nested "Software" interrupt handling line
        trigger_sgi(SW_INT_SGI_ID);
    }
    else if (interrupt_id == SW_INT_SGI_ID) {
        uart_puts("[ISR] 'Software' Interrupt tripped! Processing data inside a Critical Section...\r\n");

        // --- CRITICAL SECTION BEGIN ---
        disable_interrupts(); // Lock out incoming requests safely
        uint32_t local_copy = shared_data;

        // Print proof of successful read
        uart_puts("      Critical Read complete! Value is: 0x");

        // Simple hex print helper routine
        char hex[9];
        for (int i = 7; i >= 0; i--) {
            uint32_t nibble = (local_copy >> (i * 4)) & 0xF;
            hex[7 - i] = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
        }
        hex[8] = '\0';
        uart_puts(hex);
        uart_puts("\r\n");

        enable_interrupts();
        // --- CRITICAL SECTION END ---
    }

    // Signal completion by writing back the processed ID to End of Interrupt register
    *GICC_EOIR = iar;
}

// Synchronous exceptions (e.g. a data abort from an unmapped address), called from sync_entry in
// startup.S, which halts this core afterwards
void handle_sync(uint64_t esr, uint64_t far, uint64_t elr) {
    char buf[160];
    format_fault(buf, esr, far, elr);
    uart_puts(buf);
}

void gic_init(void) {
    *GICD_CTLR = 0; // Disable distributor for configuration
    *GICC_CTLR = 0; // Disable CPU interface for configuration

    // Leave SGIs in Group 0 (the reset default). GICD_CTLR/GICC_CTLR = 1 only
    // enable Group 0, and Group 1 interrupts can't be acknowledged via GICC_IAR
    // without GICC_CTLR.AckCtl, which would cause an endless IRQ storm.

    // Enable SGI 1 and SGI 2 (Bits 1 and 2 in the first ISENABLER register)
    *GICD_ISENABLER |= (1 << HW_INT_SGI_ID) | (1 << SW_INT_SGI_ID);

    // Set Priority Mask Register to allow all interrupts through (0xFF)
    *GICC_PMR = 0xFF;

    // Turn both the distributor and CPU interface back on
    *GICD_CTLR = 1;
    *GICC_CTLR = 1;
}

// Powers on the core with MPIDR target_mpidr, which starts at entry at EL1 with its MMU off.
// Returns 0 on success, or a negative PSCI error code
static int64_t psci_cpu_on(uint64_t target_mpidr, uint64_t entry) {
    register uint64_t x0 asm("x0") = PSCI_CPU_ON_64;
    register uint64_t x1 asm("x1") = target_mpidr;
    register uint64_t x2 asm("x2") = entry;
    register uint64_t x3 asm("x3") = 0; // Context ID, passed to entry in x0
    // The SMC Calling Convention lets the call change x0-x17
    asm volatile("hvc #0"
                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                 :
                 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
                   "x16", "x17", "memory");
    return (int64_t)x0;
}

// Prints the mailbox's message, if core 1 has posted one
static void print_mailbox(void) {
    if (__atomic_load_n(&MAILBOX->full, __ATOMIC_ACQUIRE)) {
        uart_puts(MAILBOX->text);
        __atomic_store_n(&MAILBOX->full, 0, __ATOMIC_RELEASE);
    }
}

void main_app(void) {
    char line[96];

    mmu_init();
    uart_puts("core 0: MMU on\r\n");

    // Empty the mailbox before core 1 can use it
    __atomic_store_n(&MAILBOX->full, 0, __ATOMIC_RELEASE);

    int64_t ret = psci_cpu_on(1, CORE1_BASE);
    if (ret == 0) {
        uart_puts("core 0: started core 1\r\n");
    } else {
        append_str(append_hex(append_str(line, "core 0: PSCI CPU_ON failed with "), (uint64_t)ret), "\r\n");
        uart_puts(line);
    }

#ifdef NDEBUG
    gic_init();
    enable_interrupts(); // Unmask IRQs inside the Cortex-A53 core
#endif
    while (1) {
        uart_puts("Hello\r\n");
#ifdef NDEBUG
        sleep_ms(1000);
#endif
        uart_puts("World!\r\n");
#ifdef NDEBUG
        trigger_sgi(HW_INT_SGI_ID); // Kick off the hardware -> software cascade
        sleep_ms(1000);
#endif
        print_mailbox();
    }
}
