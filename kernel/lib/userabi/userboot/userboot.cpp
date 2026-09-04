/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// userboot -- the first program vmours runs in ring 3.
///
/// Zircon counterpart: //zircon/kernel/lib/userabi/userboot, and more
/// precisely its cut-down sibling trivial-test-userboot, which is a single
/// translation unit whose only job is to prove that the kernel can build a
/// process, map an ELF into it, and reach its entry point.  This is at that
/// stage: it takes no arguments, makes no system calls, and returns to
/// nobody.  The debug-write system call and the exit that follow it come
/// once the syscall frame and ProcessDispatcher exist.
///
/// Nothing here may reference the C or C++ runtime.  There is no libc, no
/// crt1.o and no dynamic linker in this image (see //system:user.config):
/// the kernel jumps straight to the ELF entry point with a stack and
/// nothing else, so `_start` is the whole runtime.

/// `extern "C"`, because ld.lld resolves the default entry point by the
/// symbol name `_start` and a mangled name would not answer to it.  The
/// image is linked -fvisibility=hidden all the same: the entry is reached
/// through the ELF header's e_entry, never through a dynamic symbol.
extern "C" [[noreturn]]
auto _start() -> void {
    // `pause` is load-bearing, not decoration.  C++'s forward-progress rule
    // makes a loop with no side effects, no I/O and no synchronisation
    // undefined behaviour, and clang acts on that licence: at -O2 it deletes
    // `for (;;) {}` outright and lets control fall off the end of the
    // function into whatever bytes follow.  An `asm volatile` is a side
    // effect the optimiser must keep, which pins the loop in place -- and
    // `pause` is the right instruction to pin there anyway, since this spins
    // in user mode while the kernel's timer interrupt keeps preempting it.
    for (;;) {
        __asm__ volatile("pause");
    }
}
