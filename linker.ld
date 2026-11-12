ENTRY(_start)

/* IMAGE_BASE, REGION_BASE and REGION_SIZE are set for each core by CMakeLists.txt (-Wl,--defsym) */

SECTIONS
{
    . = IMAGE_BASE;

    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    .rodata : { *(.rodata .rodata.*) }
    .eh_frame : { KEEP(*(.eh_frame)) } /* Unwind info, used by debuggers for backtraces */
    .data : { *(.data .data.*) }

    /* Zeroed by startup.S */
    .bss : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    . = ALIGN(16);
    . += 0x4000; /* Allocate 16KB stack space */
    _stack_top = .;

    /* Everything, including the stack, must stay inside this core's private block */
    ASSERT(_stack_top <= REGION_BASE + REGION_SIZE, "image doesn't fit in this core's private block")
}
