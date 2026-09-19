/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// A shell that runs in ring 3 and talks to the kernel only through the vDSO.
///
/// What it is for: userboot proved that the kernel can build a process, map an
/// ELF into it and answer one system call. This is the next question -- can a
/// *program*, one that is not part of the kernel's own bring-up, do something
/// useful with that. Listing the kshell command registry is the smallest
/// useful thing, and it is useful precisely because the registry lives on the
/// other side of the privilege boundary: the names come out of the kernel's
/// linker sections, and nothing in this file knows any of them.
///
/// Every system call it makes goes through //system/lib/ou, which is the one
/// place that knows what the raw ABI looks like. This file names what it wants
/// (ou::debug_line, ou::command_list) and knows nothing else -- not the
/// per-call size limit, not the symbols the loader binds, not that a trap is
/// involved at all.
///
/// There is still no C or C++ runtime here: no libc, no crt1.o, no standard
/// library (see //system:user.config). The kernel jumps straight to the ELF
/// entry point with a stack and nothing else, so `_start` is the whole
/// runtime.

#include <ours/status.hpp>

#include <ou/command.hpp>
#include <ou/debug.hpp>
#include <ou/str.hpp>

namespace {
/// The kshell command registry, fetched into this program's own memory.
///
/// Sized rather than grown: there is no allocator in this image. NoSpace comes
/// back when the registry outgrows it, and the part that fits is still shown
/// -- a truncated list beats no list when the point is to see what exists.
class Registry {
  public:
    auto fetch() -> ours::Status {
        return ou::command_list(m_text);
    }

    [[nodiscard]]
    auto text() const -> ou::Str {
        return ou::command_text(m_text);
    }

  private:
    char m_text[2048] = {};
};

constexpr ou::Str BANNER{"ours shell: the kernel's command registry, seen from ring 3"};
} // namespace

/// `extern "C"`, because ld.lld resolves the default entry point by the symbol
/// name `_start` and a mangled name would not answer to it.
///
/// It takes no arguments. Finding the system calls is not this program's
/// problem: the calls below are undefined symbols that userboot binds to the
/// vDSO's definitions while it loads this image
/// (kernel/lib/userabi/src/userboot.cpp, and elfload::Binder under it).
extern "C" [[noreturn]]
auto _start() -> void {
    static_cast<void>(ou::debug_line(BANNER));

    static Registry registry;
    auto const status = registry.fetch();

    static_cast<void>(ou::debug_line("--- commands ---"));
    static_cast<void>(ou::debug_write(registry.text()));
    if (status == ours::Status::NoSpace) {
        static_cast<void>(
            ou::debug_line("(truncated: the registry outgrew this program's buffer)"));
    } else if (status != ours::Status::Ok) {
        static_cast<void>(ou::debug_line("(command_list failed)"));
    }
    static_cast<void>(ou::debug_line("--- end ---"));

    // Nothing to do next, and nothing to exit to: there is no exit system call
    // yet and returning from _start would return into whatever the kernel left
    // on the stack. Halting here is the honest end of the program.
    //
    // `pause` is load-bearing, not decoration. C++'s forward-progress rule
    // makes a loop with no side effects undefined behaviour, and clang acts on
    // that licence: at -O2 it deletes `for (;;) {}` outright and lets control
    // fall off the end of the function. An `asm volatile` is a side effect the
    // optimiser must keep.
    for (;;) {
        __asm__ volatile("pause");
    }
}
