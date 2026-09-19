/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// A shell that runs in ring 3 and talks to the kernel only through the vDSO.
///
/// It calls ours_debug_write() and ours_command_list() as plain functions. It
/// does not declare them, resolve them, or know where they are: they are
/// undefined symbols that userboot binds to the vDSO's .dynsym when it loads
/// this image, which is what //system/user_executable.gni arranges and what
/// <ours/vdso.hpp> declares.
///
/// What it is for: userboot proved that the kernel can build a process, map an
/// ELF into it and answer one system call. This is the next question -- can a
/// *program*, one that is not part of the kernel's own bring-up, do something
/// useful with that. Listing the kshell command registry is the smallest
/// useful thing, and it is useful precisely because the registry lives on the
/// other side of the privilege boundary: the names come out of the kernel's
/// linker sections, and nothing in this file knows any of them.
///
/// Nothing here may reference the C or C++ runtime. There is no libc, no
/// crt1.o, no dynamic linker and no standard library in this image (see
/// //system:user.config): the kernel jumps straight to the ELF entry point
/// with a stack and nothing else, so `_start` is the whole runtime. The only
/// headers available are //system/public and the integer-alias prelude.
///
/// That rules out <string_view>, <span> and friends, so the small vocabulary
/// types below are written out. They are still ordinary modern C++ -- constexpr
/// throughout, no macros, no raw loops where a member function says it better
/// -- which is the point: "no standard library" is not the same as "write C".

#include <ours/status.hpp>
#include <ours/vdso.hpp>

namespace {

/// A borrowed, non-owning range of characters.
///
/// The standard library's spelling of this is <string_view>, which is not
/// reachable here. Everything below takes one of these rather than a
/// (pointer, length) pair, so a length can never be passed for the wrong
/// pointer.
class Str {
  public:
    constexpr Str() = default;

    constexpr Str(char const *data, usize size) : m_data(data), m_size(size) {}

    /// Deduces the length of a literal, minus its NUL.
    ///
    /// The NUL is dropped on purpose: every kernel entry below is given a
    /// length and does not look for a terminator, so sending it would put a
    /// stray byte on the console.
    template <usize N>
    constexpr Str(char const (&literal)[N]) : m_data(literal), m_size(N - 1) {}

    [[nodiscard]]
    constexpr auto data() const -> char const * {
        return m_data;
    }

    [[nodiscard]]
    constexpr auto size() const -> usize {
        return m_size;
    }

    [[nodiscard]]
    constexpr auto empty() const -> bool {
        return m_size == 0;
    }

  private:
    char const *m_data = nullptr;
    usize m_size = 0;
};

/// The length of a NUL-terminated string, bounded.
///
/// Bounded because the string in question is written by the kernel into a
/// buffer this program owns, and a missing terminator would otherwise run off
/// the end of it. The kernel promises to terminate; this does not depend on
/// the promise.
constexpr auto length_of(char const *text, usize const limit) -> usize {
    usize n = 0;
    while (n < limit && text[n] != '\0') {
        ++n;
    }
    return n;
}

/// Output, in the only form this program has.
///
/// The kernel's debug write has a ceiling on one call (it will not spend
/// unbounded time in the kernel on a user program's say-so), so anything
/// longer is split here rather than silently truncated there.
class Console {
  public:
    static auto write(Str const text) -> void {
        auto const *at = text.data();
        auto left = text.size();
        while (left != 0) {
            auto const chunk = left < CHUNK ? left : CHUNK;
            static_cast<void>(ours_debug_write(at, chunk));
            at += chunk;
            left -= chunk;
        }
    }

    static auto line(Str const text) -> void {
        write(text);
        write("\n");
    }

  private:
    /// Matches MAX_DEBUG_WRITE in kernel/lib/syscall/src/debug.cpp. A larger
    /// value here does not fail loudly -- the kernel answers OutOfRange and
    /// writes nothing -- so it is worth keeping the two in step.
    static constexpr usize CHUNK = 256;
};

/// The kshell command registry, fetched into this program's own memory.
///
/// Sized rather than grown: there is no allocator in this image. NoSpace comes
/// back when the registry outgrows it, and the part that fits is still shown
/// -- a truncated list beats no list when the point is to see what exists.
class Registry {
  public:
    static constexpr usize CAPACITY = 2048;

    auto fetch() -> ours::Status {
        return ours_command_list(m_text, CAPACITY);
    }

    [[nodiscard]]
    auto text() const -> Str {
        return Str{m_text, length_of(m_text, CAPACITY)};
    }

  private:
    char m_text[CAPACITY] = {};
};

constexpr Str BANNER{"ours shell: the kernel's command registry, seen from ring 3"};

} // namespace

/// `extern "C"`, because ld.lld resolves the default entry point by the symbol
/// name `_start` and a mangled name would not answer to it.
///
/// It takes no arguments any more. It used to take the vDSO's export table,
/// which the kernel put in %rdi, because finding the entry points was this
/// program's own problem: check a magic number, check a version, add a u32
/// offset to the table address, cast the result to a function pointer. All of
/// that is gone -- the calls below are ordinary calls to undefined symbols,
/// and userboot binds them to the vDSO's definitions while it loads this image
/// (kernel/lib/userabi/src/userboot.cpp, bind_imports()).
extern "C" [[noreturn]]
auto _start() -> void {
    Console::line(BANNER);

    static Registry registry;
    auto const status = registry.fetch();

    Console::line("--- commands ---");
    Console::write(registry.text());
    if (status == ours::Status::NoSpace) {
        Console::line("(truncated: the registry outgrew this program's buffer)");
    } else if (status != ours::Status::Ok) {
        Console::line("(command_list failed)");
    }
    Console::line("--- end ---");

    // Nothing to do next, and nothing to exit to: there is no exit syscall yet
    // and returning from _start would return into whatever the kernel left on
    // the stack. Halting here is the honest end of the program.
    for (;;) {
        __asm__ volatile("pause");
    }
}
