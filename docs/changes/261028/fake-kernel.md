# fake-kernel changes

## Title: the fake kernel builds for riscv, with an entry stub and a firmware console of its own (2026-10-28)

**Files**
- `kernel/tests/fake-kernel/entry_riscv.S` — new: masks interrupts, moves onto the image's own 16 KiB stack, calls `fake_kernel_main` with `a0` untouched, `wfi` forever if it returns; publishes `MAIN_ENTRY`
- `kernel/tests/fake-kernel/arch.hpp` — new: what the report needs from the machine, `write_char()`, `halt()` and `handoff_names_console()`, in `ours::fake_kernel`
- `kernel/tests/fake-kernel/arch_x86.cpp` — new: x86's COM1 port I/O, `cli; hlt` and the UART check, moved out of `fake_kernel.cpp` unchanged
- `kernel/tests/fake-kernel/arch_riscv.cpp` — new: one legacy SBI `console_putchar` per byte, `arch::local_irq_disable()` then `arch::halt()` (`wfi`) in a loop, and the UART check for a firmware console
- `kernel/tests/fake-kernel/fake_kernel.cpp` — keeps only the report; the UART check calls `handoff_names_console()`
- `kernel/tests/fake-kernel/BUILD.gn` — compiles `arch_$ours_cpu.cpp` beside `entry_$ours_cpu.S`
- `kernel/tests/fake-kernel/README.md` — says which console each architecture prints to

**Symptom**
The riscv fake kernel did not compile: `fake_kernel.cpp:30:40: error: invalid input constraint 'a' in asm` and `invalid output constraint '=a' in asm` (x86's `outb`/`inb`), and there was no `entry_riscv.S` for `entry_$ours_cpu.S` to name. The integration step of the riscv boot chain needs a riscv kernel image for physboot to load.

**Cause**
The fake kernel was written for one architecture: its console was COM1 through x86 port I/O, its halt `cli; hlt`, and its UART check compared the handoff with `UartKind::X86Pio16550` at 0x3F8, all inline in the one source file. Only the entry stub was already chosen per architecture by name in BUILD.gn.

**Change**
The same per-architecture split the entry stub already had, extended to the three things the report needs from the machine. Before, in `fake_kernel.cpp`:

```cpp
auto write_char(char const character) -> void {
    while ((inb(COM_LINE_STATUS) & TRANSMITTER_EMPTY) == 0) {
    }
    outb(COM_PORT, static_cast<u8>(character));
}
...
if (handoff->uart.kind != ours::phys::UartKind::X86Pio16550 || handoff->uart.base != COM_PORT) {
```

After, `fake_kernel.cpp` calls `write_char()`, `halt()` and `handoff_names_console(handoff->uart)` from `arch.hpp`, and BUILD.gn compiles `arch_x86.cpp` (the code above, moved verbatim) or `arch_riscv.cpp`:

```cpp
auto write_char(char const character) -> void {
    ::arch::sbi::console_putchar(character);
}

auto handoff_names_console(phys::UartHandoff const &uart) -> bool {
    return uart.kind == phys::UartKind::None;
}
```

The riscv console is the firmware's, the one physload uses (the user's call of 2026-10-28): it needs no setup and no transmitter poll, since OpenSBI waits on the UART itself. Its UART check expects `UartKind::None`, because `UartKind` has no value for the SBI console and physload records none; a handoff that names a UART would mean physload found a device this image does not write to, the same disagreement x86's check catches.

The entry stub mirrors `entry_x86.S`: `csrci sstatus, 0x2` for `cli`, `lla sp, FAKE_KERNEL_STACK_END` for the `%rip`-relative `lea` (both give the address the image runs at, wherever physboot relocated it), `mv s0, zero` for `xor %ebp, %ebp`, and `a0` is the handoff on entry as `%rdi` is.

Rejected: `#if` blocks inside `fake_kernel.cpp`. The tree selects architecture code by which file the build compiles, not by preprocessor branches in shared code, and the entry stub already did so here.

**Reference**
The kernel-shaped test image physboot loads in Zircon is `zircon/kernel/phys/test/physboot-hello-world-test.cc` (`PhysbootHandoff()`); Zircon's physboot sets up the kernel's stack from the image's `ZirconAbiSpec`, where this tree's image sets up its own. The riscv entry's interrupt masking and stack setup follow `_start` in `zircon/kernel/arch/riscv64/phys/start.S`, without its bss clear, thread pointer and stack guard, which the fake kernel does not need.

**Verification**
- `ninja -C out/bringup.riscv kernel_riscv.dbg.0/obj/kernel/tests/fake-kernel/fake-kernel` links; `llvm-readelf -h -l` reports `Class: ELF64`, `Machine: RISC-V`, `Entry point address: 0x2000`, and `llvm-nm` has `0000000000002000 T MAIN_ENTRY` and `0000000000002000 T _start`. It has no dynamic relocations.
- The x86 fake image (`fake.obi`) boots to `[fake-kernel] handoff ok` with `uart kind 0x1`, `uart base 0x3f8`.
- clang-tidy against `out/bringup.x86` and `out/bringup.riscv`: no diagnostic in the fake kernel's files; `format_cpp.py --check` clean.
- `ours-verify --out out/x86-fake`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), 150 host tests pass.
- The riscv fake kernel has not run: nothing loads it on riscv yet.

**Deliberately not done**
- A riscv `fake.obi` boot: it needs physload and physboot for riscv, other lanes' work.
- A trap vector for the riscv entry: a trap here would be a bug the report cannot describe anyway, and x86's stub installs no IDT either.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does the riscv entry use `lla` rather than `la` to find its stack, in an image physboot relocates?
- Why does the riscv build expect the handoff to name no UART, and what change elsewhere would make that check fail?
- Why is `wfi` in a loop even with interrupts masked?
