# Bare metal C example for QEMU ARM "virt" machine with two Cortex-A53 cores

Each core runs its own image from its own private 2 MB block of RAM, and has its own MMU translation tables that map only its own block, a shared block and (core 0 only) the devices. If either core touches the other core's block, it gets a translation fault.

- Core 0 owns the UART and the GIC. It runs the same Hello/World loop and SGI cascade as the single-core version, starts core 1, and prints core 1's messages.
- Core 1 uses no peripherals. It posts a message to a mailbox in the shared block every 3 seconds, and core 0 prints it after each loop iteration (every 2 seconds), so core 1's messages can appear up to 2 seconds late.

## Prerequisites

Run `sudo apt install qemu-system-arm cmake gdb-multiarch`, and put Arm's `aarch64-none-elf` toolchain on your `PATH`. Ubuntu doesn't package it; download **AArch64 bare-metal target (aarch64-none-elf)** under **x86_64 Linux hosted cross toolchains** from https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads, extract it, and add its `bin` directory to your `PATH`.

## Build and run

```
cmake -S . -B build
cmake --build build --target qemu       # connect with `telnet localhost 5678`
cmake --build build --target qemu_debug # debug
```

Add `-DCMAKE_BUILD_TYPE=Debug` for a `-Og -ggdb3` build, which skips the interrupts and sleeps like the single-core version. `qemu_debug` starts QEMU paused with a GDB server on `localhost:1234`. The VS Code "C: Remote Attach" configuration loads both images' symbols (`add-symbol-file` for `core1.elf`), and each core shows up as a thread. It also sets `scheduler-locking step`, so stepping one core keeps the other halted. Otherwise GDB resumes both cores for every step, and a breakpoint on the other core can cut the step short. Continue still resumes both. QEMU has no non-stop mode, so whenever one core is stopped, the other is too. From the command line: `gdb-multiarch build/core0.elf -ex "add-symbol-file build/core1.elf" -ex "target remote :1234"`.

If either core touches the other core's block, it reports the fault and halts, for example:

```
core 1: data abort writing 0x40000000 (translation fault, level 2), ESR 0x96000046, PC 0x40201318, halting
```

## Memory map

| Block | Address | Contents | Mapped by |
|---|---|---|---|
| Core 0 | 0x40000000 | DTB (QEMU puts it at the RAM base), `core0.bin` at +0x80000 (where `-kernel` loads a raw image), its stack and translation tables | Core 0 |
| Core 1 | 0x40200000 | `core1.elf`, its stack and translation tables | Core 1 |
| Shared | 0x40400000 | The mailbox | Both, never executable |
| Devices | 0x00000000-0x3FFFFFFF | GIC at 0x08000000, UART at 0x09000000 | Core 0, as Device-nGnRnE, never executable |

The addresses are set in `CMakeLists.txt`, which passes them to both the compiler and the linker (`linker.ld` asserts that each image, including its stack, fits in its block). Everything else is unmapped on both cores.

## How it works

- **Two images.** `core0.elf` and `core1.elf` are built from the same `startup.S`, `common.c` and `mmu.c`, plus `core0.c` or `core1.c`, with `CORE_ID` set to 0 or 1. Each image has its own copy of every global, its own stack and its own translation tables, all inside its own block. This is the same AMP layout as the Zynq UltraScale+ projects, where each core runs its own application.
- **Starting core 1.** With `-kernel` and no firmware, QEMU starts core 0 only. Core 1 is powered off until core 0 calls PSCI `CPU_ON` (an `hvc #0` call, since there's no EL2 or EL3, which QEMU handles itself). The `-device loader,file=core1.elf` option only copies core 1's image into memory; it doesn't start anything. A `wfe` parking loop like the single-core version's never runs on core 1, since core 1 doesn't run at all until it's started.
- **One MMU per core.** Each core has its own `TTBR0_EL1`, `TCR_EL1`, `MAIR_EL1` and `SCTLR_EL1`, and each core points `TTBR0_EL1` at its own tables in its own block (`mmu.c`). Every table entry starts out invalid, and each core fills in only what it uses, so the protection is a whitelist: the other core's block is simply never mapped.
- **The shared block** is normal write-back cacheable, inner shareable memory on both cores, so it's coherent between them. `msg_post` and `print_mailbox` use acquire/release atomics on the `full` flag so the text is always written before the flag is set, and read after it's seen.
- **Faults** go to `sync_entry` in `startup.S`, which passes `ESR_EL1`, `FAR_EL1` and `ELR_EL1` to `handle_sync`. Core 0 prints the fault itself; core 1 posts it to the mailbox.

## Limitations

- **The protection is cooperative.** Both cores run at EL1, which owns its own translation tables, so a core could map the other core's block again by rewriting its own tables. This catches stray pointers and buffer overruns, not deliberate code. To enforce it, a higher exception level has to own the tables: with `-M virt,virtualization=on`, an EL2 hypervisor could give each core stage 2 translation tables that EL1 can't change (QEMU supports this, but this project doesn't do it).
- **The MMU only checks the CPU's own accesses.** DMA from devices isn't checked (the virt machine has an SMMUv3 for that, which isn't used here).
- **QEMU doesn't model caches**, so it can't catch a wrong cacheability attribute. Before turning the MMU and caches on, real hardware also needs its caches invalidated, which QEMU doesn't need and this project doesn't do.
- **The debugger sees each core's own view of memory.** GDB reads memory through the selected thread's MMU, so with core 0 selected, `x 0x40200000` fails with "Cannot access memory". Switch to core 1's thread (`thread 2`) to read core 1's block. Breakpoints work on either core, since QEMU handles them itself.
- **Debug builds spin.** Core 1 waits in `msg_post` for core 0 to empty the mailbox, and `sleep_ms` busy-waits, so each QEMU vCPU uses a full host core.

## Notes

- If a watchdog timer is desired, the Intel 6300ESB `-device i6300esb` PCIe device is available to add to the `qemu-system-aarch64` command
- If you want to read UART messages from a /dev file, replace the `-serial` option in `CMakeLists.txt` with `-serial pty`, then run `screen /dev/pts/?` where ? is the value given by QEMU
- The linker warns that each image "has a LOAD segment with RWX permissions". That's expected, since `linker.ld` puts code and data in one segment, as in the single-core version
