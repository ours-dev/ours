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
/// Nothing here may reference the C or C++ runtime. There is no libc and no
/// crt1.o in this image (see //system:user.config): the kernel jumps straight
/// to the ELF entry point with a stack and nothing else, so `_start` is the
/// whole runtime. There is no dynamic linker either -- but there are undefined
/// symbols, which the kernel's own loader binds to the vDSO as it maps this
/// image (//system/user_executable.gni).

#include <ours/vdso.hpp>

namespace {
/// The one thing this program has to say.
///
/// `constexpr` and read through a RIP-relative reference. The kernel does run
/// a relocation pass over this image now (bind_imports() in
/// //kernel/lib/userabi/src/userboot.cpp applies R_X86_64_RELATIVE along with
/// the imports), so an absolute address here would in fact be fixed up -- but
/// RIP-relative is still the right shape: it needs no relocation at all, and
/// the cheapest entry in a relocation table is the one that is not there.
constexpr char MESSAGE[] = "userboot: hello from ring 3";

} // namespace

/// `extern "C"`, because ld.lld resolves the default entry point by the symbol
/// name `_start` and a mangled name would not answer to it. The image is
/// linked -fvisibility=hidden all the same: the entry is reached through the
/// ELF header's e_entry, never through a dynamic symbol.
///
/// It takes no arguments. It used to take the vDSO's export table, check its
/// magic and version, and turn an offset into a function pointer -- work that
/// userboot (the kernel-side loader, not this program) now does by binding
/// this image's undefined symbols to the vDSO's .dynsym before the first
/// instruction runs.
extern "C" [[noreturn]]
auto _start() -> void {
    // There is no `syscall` instruction anywhere in this image, and that is
    // the point rather than an accident of style: the kernel refuses a trap
    // whose return address lies outside the vDSO, so one written here would
    // not work even if it were written correctly. Entering the kernel is the
    // vDSO's job, and this is a call into it.
    //
    // sizeof - 1, because the kernel is given a length and does not look for a
    // terminator; sending the NUL would put a stray byte on the console.
    static_cast<void>(ours_debug_write(MESSAGE, sizeof(MESSAGE) - 1));

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
