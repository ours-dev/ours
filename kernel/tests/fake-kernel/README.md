# fake-kernel

A kernel-shaped ELF that does nothing but prove the boot chain reached it.

The real kernel (`vmours`) does not build yet, which leaves the whole
physload → physboot → kernel path unexercised end to end: every piece of it
is reachable only by booting something. This target stands in for the kernel
so the chain can be assembled, packaged and booted while the kernel proper is
still under construction.

It is a real kernel image, not a placeholder file: it links against
`//kernel/kernel.ld` at the kernel's load address, exposes the same
`MAIN_ENTRY` the linker script expects, and is entered exactly the way
`KernelBoot::do_handoff()` enters `vmours` -- one argument, a pointer to the
`ours::phys::Handoff` that physboot filled in.

What it does on entry:

  1. checks `Handoff::verify()` -- the magic, the boot-memory service table
     and the retained boot-package ranges,
  2. reports what it found on the console, and
  3. halts.

Its own console rather than the phys one: the kernel runs in its own image
with no access to physload's exports. On x86 it drives COM1 directly, and
under QEMU `-serial stdio` puts the report on the terminal. On riscv it
prints through the firmware's console (the legacy SBI putchar), the same
call physload uses, which OpenSBI writes to the `virt` machine's UART.

The entry stub (`entry_<cpu>.S`), the console and the halt
(`arch_<cpu>.cpp`, declared in `arch.hpp`) are per architecture; BUILD.gn
compiles the target's. The report itself, in `fake_kernel.cpp`, is shared.

The report is the test oracle. Reaching `[fake-kernel] handoff ok` means
physload parsed the OBI, brought up boot memory, loaded and bound the
physboot module, and physboot found, loaded, relocated and entered a kernel
out of the kernel package -- with a handoff intact at the far end.
