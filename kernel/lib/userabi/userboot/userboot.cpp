/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// userboot -- the first program vmours runs in ring 3.
///
/// Zircon counterpart: //zircon/kernel/lib/userabi/userboot, and more
/// precisely its cut-down sibling trivial-test-userboot, which is a single
/// translation unit whose only job is to prove that the kernel can build a
/// process, map an ELF into it, reach its entry point, and answer a system
/// call from it.
///
/// Nothing here may reference the C or C++ runtime. There is no libc, no
/// crt1.o and no dynamic linker in this image (see //system:user.config):
/// the kernel jumps straight to the ELF entry point with a stack and
/// nothing else, so `_start` is the whole runtime.

#include <ours/syscall.hpp>

namespace {
/// The one thing this program has to say.
///
/// `constexpr` and read through a RIP-relative reference, which matters more
/// than it looks: the image is a static PIE that the kernel maps at an address
/// of its choosing and does *not* relocate (see load_image() in
/// //kernel/lib/userabi/userboot.cpp -- it has no relocation pass because this
/// binary has no relocations). An absolute address baked into the image here
/// would therefore be read as-is and point nowhere.
constexpr char MESSAGE[] = "userboot: hello from ring 3";

/// The system-call ABI, written out.
///
/// %rax carries the number in and the ours::Status out; %rdi and %rsi are the
/// first two arguments, exactly as the ordinary x86-64 SysV convention would
/// pass them -- which is the point of choosing that register order for the
/// ABI, since it makes this stub nothing more than a `syscall` instruction.
///
/// The clobber list is not boilerplate. SYSCALL destroys %rcx (it puts the
/// return address there) and %r11 (RFLAGS), so both have to be named or the
/// compiler will keep live values in them across this. "memory" is here
/// because the kernel reads the buffer: without it, a store into MESSAGE
/// could legally be sunk past the call.
[[nodiscard]]
auto syscall2(usize const number, usize const arg0, usize const arg1) -> usize {
    usize result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(arg0), "S"(arg1)
                     : "rcx", "r11", "memory");
    return result;
}
} // namespace

/// `extern "C"`, because ld.lld resolves the default entry point by the
/// symbol name `_start` and a mangled name would not answer to it. The
/// image is linked -fvisibility=hidden all the same: the entry is reached
/// through the ELF header's e_entry, never through a dynamic symbol.
extern "C" [[noreturn]]
auto _start() -> void {
    // sizeof - 1, because the kernel is given a length and does not look for a
    // terminator; sending the NUL would put a stray byte on the console.
    static_cast<void>(
        syscall2(OURS_SYS_DEBUG_WRITE, reinterpret_cast<usize>(MESSAGE), sizeof(MESSAGE) - 1));

    // One call, and then nothing. Coming back here at all is the second half
    // of what this program tests: it means SYSRET restored the user %rip and
    // %rsp that the entry stub parked, and that ring 3 resumed rather than
    // faulting. The timer interrupts the kernel goes on logging from user
    // space are the evidence that it did.
    //
    // `pause` is load-bearing, not decoration. C++'s forward-progress rule
    // makes a loop with no side effects, no I/O and no synchronisation
    // undefined behaviour, and clang acts on that licence: at -O2 it deletes
    // `for (;;) {}` outright and lets control fall off the end of the function
    // into whatever bytes follow. An `asm volatile` is a side effect the
    // optimiser must keep, which pins the loop in place -- and `pause` is the
    // right instruction to pin there anyway, since this spins in user mode
    // while the kernel's timer interrupt keeps preempting it.
    for (;;) {
        __asm__ volatile("pause");
    }
}
