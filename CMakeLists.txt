cmake_minimum_required(VERSION 3.27)

# Cross compiler for bare metal AArch64 (must be set before project())
set(CMAKE_C_COMPILER aarch64-none-elf-gcc)
set(CMAKE_ASM_COMPILER aarch64-none-elf-gcc)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(baremetal C ASM)

set(CMAKE_C_FLAGS "-ffreestanding -mgeneral-regs-only -Wall -Wextra -pedantic")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_C_FLAGS_DEBUG "-Og -ggdb3")
set(CMAKE_C_FLAGS_MINSIZEREL "-Os -DNDEBUG")

# Memory map (see platform.h): one 2 MB MMU block per core, plus one shared block, at the start of
# QEMU virt's RAM (0x40000000)
set(CORE0_BASE  0x40000000)
set(CORE1_BASE  0x40200000)
set(SHARED_BASE 0x40400000)
set(REGION_SIZE 0x200000)
add_compile_definitions(
    CORE0_BASE=${CORE0_BASE} CORE1_BASE=${CORE1_BASE}
    SHARED_BASE=${SHARED_BASE} REGION_SIZE=${REGION_SIZE})

# Builds core<core_id>.elf, linked at image_base inside the private block at region_base. Each core
# gets its own image, with its own code, data, stack and translation tables
function(add_core_image core_id region_base image_base)
    set(target core${core_id}.elf)
    # Link without newlib/crt0, using our own startup code and memory layout
    add_executable(${target} startup.S common.c mmu.c core${core_id}.c)
    target_compile_definitions(${target} PRIVATE CORE_ID=${core_id})
    target_link_options(${target} PRIVATE
        -nostdlib -T ${CMAKE_SOURCE_DIR}/linker.ld
        -Wl,--defsym=IMAGE_BASE=${image_base}
        -Wl,--defsym=REGION_BASE=${region_base}
        -Wl,--defsym=REGION_SIZE=${REGION_SIZE})
    set_target_properties(${target} PROPERTIES LINK_DEPENDS ${CMAKE_SOURCE_DIR}/linker.ld)
endfunction()

# Core 0's image starts 0x80000 into its block, where -kernel loads a raw image (the DTB sits at the RAM base)
math(EXPR CORE0_IMAGE "${CORE0_BASE} + 0x80000" OUTPUT_FORMAT HEXADECIMAL)
add_core_image(0 ${CORE0_BASE} ${CORE0_IMAGE})
add_core_image(1 ${CORE1_BASE} ${CORE1_BASE})

# Extract raw flat execution binary for -kernel
add_custom_command(TARGET core0.elf POST_BUILD
    COMMAND aarch64-none-elf-objcopy -O binary core0.elf core0.bin)

# -smp 2: two Cortex-A53 cores. -kernel starts core 0 at core0.bin, and the generic loader device
# copies core1.elf into core 1's block without starting core 1 (core 0 does that with PSCI)
set(QEMU_COMMAND
    qemu-system-aarch64
        -M virt
        -cpu cortex-a53
        -smp 2
        -nographic
        -serial telnet:localhost:5678,server=on,wait=off
        -kernel core0.bin
        -device loader,file=core1.elf)

add_custom_target(qemu
    COMMAND ${QEMU_COMMAND}
    DEPENDS core0.elf core1.elf
    USES_TERMINAL
    VERBATIM
)

add_custom_target(qemu_debug
    COMMAND ${QEMU_COMMAND} -S -s
    DEPENDS core0.elf core1.elf
    USES_TERMINAL
    VERBATIM
)
