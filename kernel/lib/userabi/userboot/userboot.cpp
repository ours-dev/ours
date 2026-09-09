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

#include <ours/vdso.hpp>

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

/// The vDSO's export table, as this program sees it.
///
/// There is no `syscall` instruction anywhere in this image, and that is the
/// point rather than an accident of style: the kernel refuses a trap whose
/// return address lies outside the vDSO, so one written here would not work
/// even if it were written correctly. Entering the kernel is the vDSO's job.
using DebugWriteFn = auto (*)(char const *buffer, usize length) -> usize;

/// Resolves one entry of the table into something callable.
///
/// Every entry is a byte offset from the table itself, because nothing
/// relocates the vDSO -- see <ours/vdso.hpp>. Adding the base back here is the
/// whole of the "dynamic linking" this program does.
template <typename Fn>
auto vdso_entry(ours::VdsoEntryTable const *const table, u32 const offset) -> Fn {
    // The const is dropped on purpose: the table is read-only data, but what
    // it points at is code, and a pointer-to-function cannot carry it.
    auto const address = reinterpret_cast<usize>(table) + offset;
    return reinterpret_cast<Fn>(address);
}
} // namespace

/// `extern "C"`, because ld.lld resolves the default entry point by the symbol
/// name `_start` and a mangled name would not answer to it. The image is
/// linked -fvisibility=hidden all the same: the entry is reached through the
/// ELF header's e_entry, never through a dynamic symbol.
///
/// The parameter is not decoration. The kernel puts the vDSO's export table
/// address in the first start argument, which arch_build_uspace_irqframe()
/// places in %rdi -- exactly where the ordinary SysV convention says a first
/// parameter lives, so declaring it is all it takes to receive it.
extern "C" [[noreturn]]
auto _start(ours::VdsoEntryTable const *const vdso) -> void {
    // Checked before it is called through. A table this program cannot
    // recognise would otherwise be an indirect call to whatever happens to be
    // at that address, in ring 3, with no way left to say so -- the only
    // output this program has is the very entry it is trying to reach.
    if (vdso == nullptr || vdso->magic != OURS_VDSO_MAGIC || vdso->version != OURS_VDSO_VERSION ||
        vdso->nr_entries < OURS_VDSO_NR_ENTRIES) {
        for (;;) {
            __asm__ volatile("pause");
        }
    }

    auto const debug_write = vdso_entry<DebugWriteFn>(vdso, vdso->debug_write);

    // sizeof - 1, because the kernel is given a length and does not look for a
    // terminator; sending the NUL would put a stray byte on the console.
    static_cast<void>(debug_write(MESSAGE, sizeof(MESSAGE) - 1));

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
