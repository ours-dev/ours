# riscv-boot changes

## Title: a riscv boot shim that OpenSBI starts and that reports its hart and device tree over SBI (2026-10-26)

**Files**
- `kernel/arch/riscv/phys/start.S` — the Linux boot image header, then `_start`: mask interrupts, clear `stvec` and `satp`, clear bss, set the stack, call `bootshim_main`; `PHYS_ENTRY = _start`
- `kernel/arch/riscv/phys/bootshim/shim.cpp` — `bootshim_main(hart_id, device_tree)`: prints both over the SBI console, then parks in `wfi`
- `kernel/arch/riscv/lib/arch/include/arch/riscv/sbi.hpp` — `arch::sbi::call()` (the `ecall`) and `console_putchar()`
- `kernel/arch/riscv/phys/bootshim/BUILD.gn` — `phys_executable("bootshim")` with the fixed-address link config
- `kernel/arch/riscv/phys/BUILD.gn` — `phys.model` (`-mcmodel=medany`), `bootshim.link` (`PHYS_LOAD_ADDRESS=0x80200000`), and the `bootshim` source set
- `kernel/arch/riscv/BUILD.gn` — lists `sbi.hpp` among the headers
- `BUILD.gn` — the default group names the bootshim of `$current_cpu` instead of the x86 one only

**Symptom**
None, the first step of the riscv port. Nothing in the tree ran on riscv: `kernel/arch/riscv/phys` held a `BUILD.gn` with an empty source set.

**Cause**
None, an addition. What it settles is the contract between the firmware and the first image, which every later riscv stage stands on:

- OpenSBI (M-mode firmware, built into QEMU's `virt`) enters the image in S-mode with the MMU off, `a0` the boot hart's ID and `a1` the physical address of the device tree. Nothing else is promised: the stack, the trap vector and the interrupt enables are whatever it left. So `_start` masks interrupts, zeroes `stvec` and `satp`, and gives itself a stack before any C++ runs.
- RAM on `virt` starts at `0x80000000`; OpenSBI keeps the first 2 MiB. It jumps to `0x80200000`, which is why `bootshim.link` fixes the load address there. That address is above the +-2 GiB around zero that the default code model (`medlow`) reaches, so the shim is compiled `medany`: pc-relative addressing, correct wherever it is placed.
- The image starts with the 64-byte Linux boot image header (`text_offset`, `image_size`, the `RISCV`/`RSC\x05` magics), so a loader that takes a flat binary treats it as it would a Linux kernel. QEMU loading an ELF ignores the header and jumps to the ELF entry; the header is for the other loaders.
- Output is an SBI call, not a UART driver. `ecall` traps to M-mode with the extension in `a7`, the function in `a6` and arguments in `a0`-`a5`; the error comes back in `a0` and a value in `a1`. The legacy `console_putchar` (extension `0x01`) needs no probe, unlike the debug console extension.

**Change**
The header, as `start.S` writes it (offsets in bytes):

```asm
_start:  j .Lreal_start          //  0  code0; skips the header
         .word 0                 //  4  code1
         .dword 0x200000         //  8  text_offset: 2 MiB above RAM base, past OpenSBI
         .dword __executable_end - __executable_start   // 16 image_size, bss included
         .dword 0                // 24  flags: little-endian
         .word 0x2               // 32  version 0.2
         ...
         .ascii "RISCV\0\0\0"    // 48  magic
         .ascii "RSC\x05"        // 56  magic2
```

`bootshim_main` is `extern "C"` and `[[noreturn]]`, called with `a0`/`a1` untouched by the bss loop. The bss loop stores bytes: `phys.ld` aligns the start of `.bss` but not its end, so a word store at the tail would write past the image.

`arch::sbi::call()` binds its arguments to registers with `register ... asm("a0")` and lists `"memory"` as a clobber, so that a buffer the firmware reads or writes is not moved across the call. `console_putchar` discards the status with `auto _ =`, and says why: a console that cannot print cannot report that.

Rejected:
- Linking `bootshim.core` (console, panic, `elf_image`) now. It names `$ours_cpu` headers and may carry x86 assumptions; reaching a first line of output without it makes each later addition a visible step.
- A position-independent shim. Nothing upstream relocates it, as with the x86 shim.
- `c.li s4, -13` as the first instruction (the "MZ" EFI trick). It needs the C extension and is only useful to an EFI loader; the plain `j` is enough for a non-EFI image.

**Reference**
Zircon `//zircon/kernel/arch/riscv64/phys/start.S` (`_start`: `a0` hart ID, `a1` DTB, clear `sstatus.SIE`, `sie`, `stvec`, `satp`, then bss, then stack, then `PhysMain`), `linuxboot-header.S` (`LinuxbootHeader`) and `setup.cc`. Linux `arch/riscv/kernel/head.S` (`_start`, the image header) and `sbi_ecall()` / `sbi_console_putchar()` (not checked against a Linux source tree this time; from memory).

The difference this change keeps from Zircon: Zircon's `_start` also loads `tp` and a stack guard, which wait for `PhysMain`; here there is no C++ runtime yet.

**Verification**
- Build: `ninja -C out/bringup.riscv kernel.phys_riscv.phys.dbg_0/phony/kernel/arch/riscv/phys/bootshim/bootshim`. ELF64 RISC-V, `e_flags` 0, entry `0x80200000` = `_start` = `PHYS_ENTRY`.
- Raw image header (`xxd -l 64 bootshim.bin`): `6f000004` (jal +0x40), `text_offset` `0x200000`, `image_size` `0x4290` (equals `__executable_end - __executable_start`), version `2`, `RISCV`, `RSC\x05`.
- QEMU: `qemu-system-riscv64 -M virt -m 256M -nographic -bios default -kernel <elf>` with `DYLD_FALLBACK_LIBRARY_PATH=prebuilt/qemu/lib`. OpenSBI v1.8.1 reports `Domain0 Next Address: 0x80200000`, `Next Arg1: 0x8fe00000`, `Next Mode: S-mode`, then the shim prints `[riscv-bootshim] hart 0x0000000000000000 dtb 0x000000008fe00000`. The `dtb` value equals OpenSBI's `Next Arg1`, so `a1` arrived intact.
- clang-format clean on both new C++ files; clang-tidy (`-p out/bringup.riscv`, with `sbi.hpp` in the header filter) reports nothing.
- `ours-verify` on x86 after the three riscv changes of this day: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), 146 host tests pass. The root `BUILD.gn` change moves the x86 bootshim line into a conditional; `gn gen out/bringup.x86` is unchanged in what it builds.
- Not measured: more than one hart (`-smp 1` only), a platform other than QEMU `virt`, a loader that reads the header (U-Boot).

**Deliberately not done**
- Reading the device tree. The shim prints the address and nothing more; the parse is the next step.
- Finding and loading physload. `bootshim_main` parks.
- Secondary harts. OpenSBI leaves them stopped; starting them is an SBI HSM call for the kernel proper.
- `kernel/tests/fake-kernel/entry_riscv.S`. `BUILD.gn` there builds `entry_$ours_cpu.S`, so the whole default group still fails on riscv at that file. It belongs to the last stage of the chain, not the first.
- The debug console extension (`DBCN`). It needs a probe first; the legacy call does the same job for now.

**How it was found**
None, a new feature. The mismatch the build surfaced on the way, a missing `user-riscv` toolchain, a missing compiler runtime and `-mabi=` reaching `ld.lld`, is recorded in `build-toolchain.md` of the same day.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must `_start` clear `satp` and `stvec` and mask interrupts when the firmware "just booted" the image?
- Why is the shim compiled with `-mcmodel=medany` and the kernel's usual model would not do at `0x80200000`?
- `a1` held `0x8fe00000` on entry. Where does that number come from, and which later stage depends on reading it correctly?
